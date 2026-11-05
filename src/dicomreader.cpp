#include "dicomreader.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>

#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dctk.h>
#include <dcmtk/dcmdata/dcpxitem.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace {

/** 单个切片的原始像素 + 元数据
 *
 * 像素以「原始 16 位样本」保存，符号与 rescale（slope/intercept）留到
 * 采样时再应用：
 *  - 内存：384 张 384×274 的序列从 double 的 ~323MB 降到 uint16 的 ~81MB；
 *  - 语义：重采样是最近邻取样，值变换是逐点线性的，先取样后变换与
 *    先变换后取样结果完全一致（灰度归一化本来就在重采样之后做）。
 */
struct Slice {
    std::vector<uint16_t> raw; // rows × cols（行主序），8 位数据也统一存为 uint16
    int rows = 0;
    int cols = 0;
    double z = 0.0;      // 空间 Z（ImagePositionPatient 第 3 轴）
    bool hasZ = false;   // 是否真的读到了 Z —— 不能用 z==0.0 当哨兵
    double resX = 1.0;   // 像素间距
    double resY = 1.0;
    double resZ = 1.0;   // 层厚（无相邻层 info 时的兜底）
    int instance = 0;
    double slope = 1.0;
    double intercept = 0.0;
    bool signedPixels = false;

    /** 取第 idx 个样本的物理值（应用符号与 rescale） */
    double valueAt(size_t idx) const
    {
        const uint16_t rawv = raw[idx];
        double v = 0.0;
        if (signedPixels) {
            // 不用 static_cast<int16_t>(rawv)：无符号→有符号越界转换在
            // C++20 之前属未定义行为，这里显式按补码语义还原。
            v = (rawv & 0x8000u) ? static_cast<double>(static_cast<int32_t>(rawv) - 65536)
                                 : static_cast<double>(rawv);
        } else {
            v = static_cast<double>(rawv);
        }
        return v * slope + intercept;
    }
};

/** 读取单个 DICOM 文件为 Slice */
bool readSlice(const QString &path, Slice &slice, QString &err)
{
    DcmFileFormat ffile;
    const OFCondition status = ffile.loadFile(QFile::encodeName(path).constData());
    if (status.bad()) {
        err = QString("无法加载 %1：%2").arg(path).arg(status.text());
        return false;
    }
    DcmDataset *ds = ffile.getDataset();

    // 矩阵尺寸
    Uint16 rows = 0, cols = 0;
    if (ds->findAndGetUint16(DCM_Rows, rows).bad() ||
        ds->findAndGetUint16(DCM_Columns, cols).bad() ||
        rows == 0 || cols == 0) {
        err = QString("%1：缺少 Rows/Columns").arg(path);
        return false;
    }

    // 像素位深 / 表示
    Uint16 bitsAllocated = 0, pixelRep = 0;
    ds->findAndGetUint16(DCM_BitsAllocated, bitsAllocated);
    ds->findAndGetUint16(DCM_PixelRepresentation, pixelRep);

    if (bitsAllocated != 8 && bitsAllocated != 16) {
        err = QString("%1：不支持的 BitsAllocated=%2（仅支持 8/16 位）").arg(path).arg(bitsAllocated);
        return false;
    }

    // rescale slope / intercept（CT 常非 1/0，MR 多为 1/0，但不可假定）
    //
    // 注意：DCMTK 的 findAndGetFloat64 在标签缺失时会把出参写成 0（不是保留
    // 原值）。之前直接 `findAndGetFloat64(DCM_RescaleSlope, slope)` 会让没有
    // Rescale 标签的序列（MR 很常见）变成 slope=0 → 整个体积全零 → 黑屏。
    // 样例脑部 T1 恰好显式写了 1/0，所以这个坑一直被掩盖。
    double slope = 1.0, intercept = 0.0;
    if (ds->findAndGetFloat64(DCM_RescaleSlope, slope).bad())
        slope = 1.0;
    if (ds->findAndGetFloat64(DCM_RescaleIntercept, intercept).bad())
        intercept = 0.0;

    // 确保像素以未压缩原生格式可用（覆盖 RLE/JPEG 压缩）
    ds->chooseRepresentation(EXS_LittleEndianExplicit, static_cast<const DcmRepresentationParameter *>(nullptr));

    DcmElement *elem = nullptr;
    if (ds->findAndGetElement(DCM_PixelData, elem, OFFalse).bad() || !elem) {
        err = QString("%1：无像素数据").arg(path);
        return false;
    }
    auto *pdata = static_cast<DcmPixelData *>(elem);

    slice.rows = static_cast<int>(rows);
    slice.cols = static_cast<int>(cols);
    slice.slope = slope;
    slice.intercept = intercept;
    slice.signedPixels = (pixelRep == 1);
    slice.raw.clear();

    const size_t expected = static_cast<size_t>(rows) * static_cast<size_t>(cols);

    if (bitsAllocated == 8) {
        // 8 位数据：按字节读取，统一存为 uint16
        Uint8 *byteArray = nullptr;
        if (pdata->getUint8Array(byteArray).bad() || !byteArray) {
            err = QString("%1：像素无法读取为字节数组").arg(path);
            return false;
        }
        const unsigned long count = pdata->getLength();
        if (count < expected) {
            err = QString("%1：像素长度不足（%2 < Rows×Columns=%3）")
                      .arg(path).arg(count).arg(expected);
            return false;
        }
        slice.raw.assign(byteArray, byteArray + expected);
    } else {
        // 16 位数据：保留完整位深。切勿在此处做 >> 8 之类的截断——
        // 本项目样例数据（脑部 T1，BitsStored=16）的有效值仅落在低 8 位
        // （实测值域 0..199，高字节恒为 0），截断会让整个体积变成全零。
        Uint16 *pixArray = nullptr;
        if (pdata->getUint16Array(pixArray).bad() || !pixArray) {
            err = QString("%1：像素无法读取为 16 位组件").arg(path);
            return false;
        }
        const unsigned long byteLen = pdata->getLength(EXS_LittleEndianExplicit);
        const unsigned long pixCount = byteLen / sizeof(Uint16);
        if (pixCount < expected) {
            err = QString("%1：像素长度不足（%2 < Rows×Columns=%3）")
                      .arg(path).arg(pixCount).arg(expected);
            return false;
        }
        slice.raw.assign(pixArray, pixArray + expected);
    }

    // 空间位置（按 Z 排序切片）。hasZ 独立标记，避免「z 恰为 0」被当成缺失。
    const Float64 *posPtr = nullptr;
    unsigned long posCount = 0;
    if (ds->findAndGetFloat64Array(DCM_ImagePositionPatient, posPtr, &posCount).good() && posCount >= 3) {
        slice.z = posPtr[2];
        slice.hasZ = true;
    }

    // InstanceNumber 始终读取：整卷都缺 Z 时按它排序（混合序列也可回退）
    Uint16 inst = 0;
    if (ds->findAndGetUint16(DCM_InstanceNumber, inst).good())
        slice.instance = static_cast<int>(inst);

    // 间距
    const Float64 *spacingPtr = nullptr;
    unsigned long spacingCount = 0;
    if (ds->findAndGetFloat64Array(DCM_PixelSpacing, spacingPtr, &spacingCount).good() && spacingCount >= 2) {
        slice.resX = spacingPtr[0];
        slice.resY = spacingPtr[1];
    }
    Float64 thick = 1.0;
    if (ds->findAndGetFloat64(DCM_SliceThickness, thick).good() && thick > 0.0) {
        slice.resZ = thick;
    }
    return true;
}

/** 最近邻三维重采样：原始 (cx,cy,cz) → 目标 (dim)³
 *
 * 输出保留原始浮点值（未量化），灰度归一化统一在 loadVolume 中做。
 * 所有切片矩阵已由调用方统一为 (rows, cols)，因此固定步长索引是安全的；
 * 这里仍保留一次长度校验，防止未来调用方绕过统一逻辑造成越界读。
 * 支持协作式取消：每个 Z 层检查一次，取消后尽快返回。
 */
bool resample(const std::vector<const Slice *> &slicePtr,
              int cx, int cy, int cz,
              int dim,
              std::vector<double> &out,
              const std::atomic<bool> *cancel)
{
    const size_t sdim = static_cast<size_t>(dim);
    const size_t scx = static_cast<size_t>(cx);
    const size_t layerSize = scx * static_cast<size_t>(cy);
    const int lastX = cx - 1, lastY = cy - 1, lastZ = cz - 1, lastD = dim - 1;

    out.assign(sdim * sdim * sdim, 0.0);
    for (int zi = 0; zi < dim; ++zi) {
        if (cancel && cancel->load()) return false;

        const int zs = std::clamp(static_cast<int>(static_cast<double>(zi) * lastZ / lastD + 0.5), 0, lastZ);
        const Slice *s = slicePtr[static_cast<size_t>(zs)];
        if (s->raw.size() < layerSize)
            continue; // 长度不符的切片留黑，不越界读

        for (int yi = 0; yi < dim; ++yi) {
            const int ys = std::clamp(static_cast<int>(static_cast<double>(yi) * lastY / lastD + 0.5), 0, lastY);
            for (int xi = 0; xi < dim; ++xi) {
                const int xs = std::clamp(static_cast<int>(static_cast<double>(xi) * lastX / lastD + 0.5), 0, lastX);
                const size_t srcIdx = static_cast<size_t>(ys) * scx + static_cast<size_t>(xs);
                const double val = s->valueAt(srcIdx);
                // 转置式填入（DICOM 行主序 → 体积 z 轴最外）
                const size_t dstIdx = (static_cast<size_t>(zi) * sdim + static_cast<size_t>(yi)) * sdim
                                      + static_cast<size_t>(xi);
                out[dstIdx] = val;
            }
        }
    }
    return true;
}

} // namespace

DicomReader::DicomReader(QObject *parent)
    : QObject(parent)
{
}

bool DicomReader::loadVolume(const QString &directory,
                             QByteArray &outData,
                             QVariantMap &outMeta,
                             QString &err,
                             int targetDim,
                             const std::atomic<bool> *cancel)
{
    const QDir dir(directory);
    if (!dir.exists()) {
        err = QString("目录不存在：%1").arg(directory);
        return false;
    }
    if (targetDim < 2) {
        err = QString("targetDim 过小：%1（至少为 2）").arg(targetDim);
        return false;
    }

    // 收集所有 .dcm
    const QFileInfoList files = dir.entryInfoList(QStringList() << "*.dcm" << "*.dicom" << "*.DCM",
                                                  QDir::Files | QDir::Readable,
                                                  QDir::Name);
    if (files.isEmpty()) {
        err = QString("目录中没有 DICOM 文件：%1").arg(directory);
        return false;
    }

    // 解析每个切片（协作式取消：每个文件检查一次，取消后不再继续读盘）
    std::vector<Slice> slices;
    slices.reserve(static_cast<size_t>(files.size()));
    for (const QFileInfo &fi : files) {
        if (cancel && cancel->load()) {
            err = QStringLiteral("解析已取消");
            return false;
        }
        Slice s;
        QString e;
        if (readSlice(fi.absoluteFilePath(), s, e)) {
            slices.push_back(std::move(s));
        } else {
            qWarning().noquote() << "[DicomReader]" << e;
        }
    }
    if (slices.empty()) {
        err = "所有 DICOM 文件解析失败";
        return false;
    }

    // 统一矩阵：以出现次数最多的 (rows, cols) 为基准，剔除尺寸不一致的切片。
    // 之前直接用 slices.front() 的尺寸索引所有切片，混入不同矩阵的切片时会越界读。
    std::map<std::pair<int, int>, int> dimCounts;
    for (const Slice &s : slices) ++dimCounts[{s.rows, s.cols}];
    const auto best = std::max_element(
        dimCounts.begin(), dimCounts.end(),
        [](const auto &a, const auto &b) { return a.second < b.second; });
    const int rows = best->first.first;
    const int cols = best->first.second;
    const size_t expected = static_cast<size_t>(rows) * static_cast<size_t>(cols);

    const size_t parsedCount = slices.size();
    slices.erase(std::remove_if(slices.begin(), slices.end(), [expected, rows, cols](const Slice &s) {
        return s.rows != rows || s.cols != cols || s.raw.size() < expected;
    }), slices.end());
    if (slices.empty()) {
        err = QStringLiteral("所有切片的矩阵尺寸不一致，无法重建体积");
        return false;
    }
    const size_t skipped = parsedCount - slices.size();
    if (skipped > 0) {
        qWarning().noquote() << "[DicomReader] 剔除" << skipped << "张矩阵尺寸不一致的切片"
                             << "（基准 " << cols << "×" << rows << "）";
    }

    // 排序：整卷都有 space Z 才按 Z 排；否则（全缺或混合）按 InstanceNumber 排，
    // 避免用「z==0 当缺失」造成的串层。
    const size_t zCount = static_cast<size_t>(
        std::count_if(slices.begin(), slices.end(), [](const Slice &s) { return s.hasZ; }));
    const bool useZ = (zCount == slices.size());
    if (zCount > 0 && !useZ) {
        qWarning().noquote() << "[DicomReader] 部分切片缺少 ImagePositionPatient（"
                             << zCount << "/" << slices.size() << "），改用 InstanceNumber 排序";
    }
    std::stable_sort(slices.begin(), slices.end(), [useZ](const Slice &a, const Slice &b) {
        return useZ ? (a.z < b.z) : (a.instance < b.instance);
    });

    // 层间距：有真实 Z 时用相邻层差的中位数；只有层厚时退回 SliceThickness
    double resZ = slices.front().resZ;
    if (useZ && slices.size() >= 2) {
        std::vector<double> diffs;
        diffs.reserve(slices.size() - 1);
        for (size_t i = 1; i < slices.size(); ++i) {
            const double dz = std::fabs(slices[i].z - slices[i - 1].z);
            if (dz > 1e-6) diffs.push_back(dz);
        }
        if (!diffs.empty()) {
            const size_t mid = diffs.size() / 2;
            std::nth_element(diffs.begin(), diffs.begin() + static_cast<long>(mid), diffs.end());
            resZ = diffs[mid];
        }
    }

    const int slicesN = static_cast<int>(slices.size());
    qInfo() << "[DicomReader] 解析完成：" << slicesN << "张切片"
            << cols << "×" << rows
            << "→ 重采样到" << targetDim << "³";

    std::vector<const Slice *> ptr;
    ptr.reserve(slices.size());
    for (const Slice &s : slices) ptr.push_back(&s);

    std::vector<double> vol;
    if (!resample(ptr, cols, rows, slicesN, targetDim, vol, cancel)) {
        err = QStringLiteral("解析已取消");
        return false;
    }

    // 灰度归一化：min-max → 0-255（用真实值域，不用位深截断）
    // 注意：必须在重采样之后做，才能反映整个体积的实际动态范围。
    double vmin = 0.0, vmax = 0.0;
    bool first = true;
    for (const double v : vol) {
        if (first) { vmin = vmax = v; first = false; continue; }
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }

    std::vector<uint8_t> bytes(vol.size());
    const double range = vmax - vmin;
    if (range > 1e-9) {
        for (size_t i = 0; i < vol.size(); ++i) {
            const double t = (vol[i] - vmin) / range;
            bytes[i] = static_cast<uint8_t>(std::lround(t * 255.0));
        }
    } else {
        // 整个体积是常数（无对比度）——全部置 0，交给上层显式报错而不是静默出黑图
        std::fill(bytes.begin(), bytes.end(), uint8_t(0));
    }

    outData = QByteArray(reinterpret_cast<const char *>(bytes.data()),
                         static_cast<int>(bytes.size()));

    outMeta.clear();
    outMeta.insert(QStringLiteral("dims"), QVariantList() << targetDim << targetDim << targetDim);
    outMeta.insert(QStringLiteral("spacing"),
                   QVariantList() << slices.front().resX << slices.front().resY << resZ);
    outMeta.insert(QStringLiteral("min"), vmin);
    outMeta.insert(QStringLiteral("max"), vmax);
    outMeta.insert(QStringLiteral("slices"), slicesN);
    outMeta.insert(QStringLiteral("skipped"), static_cast<int>(skipped));
    outMeta.insert(QStringLiteral("source"), directory);
    return true;
}
