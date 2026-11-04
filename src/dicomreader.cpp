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
#include <vector>

namespace {

/** 单个切片的原始像素 + 元数据 */
struct Slice {
    std::vector<double> pixels; // rows × cols（已应用符号与 rescale 的物理值）
    int rows = 0;
    int cols = 0;
    double z = 0.0;      // 空间 Z（ImagePositionPatient 第 3 轴）
    double resX = 1.0;   // 像素间距
    double resY = 1.0;
    double resZ = 1.0;
    int instance = 0;
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

    // rescale slope / intercept（CT 常非 1/0，MR 多为 1/0，但不可假定）
    double slope = 1.0, intercept = 0.0;
    ds->findAndGetFloat64(DCM_RescaleSlope, slope);
    ds->findAndGetFloat64(DCM_RescaleIntercept, intercept);

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
    slice.pixels.clear();

    if (bitsAllocated == 8) {
        // 8 位数据：按字节读取
        Uint8 *byteArray = nullptr;
        if (pdata->getUint8Array(byteArray).bad() || !byteArray) {
            err = QString("%1：像素无法读取为字节数组").arg(path);
            return false;
        }
        const unsigned long count = pdata->getLength();
        slice.pixels.reserve(count);
        for (unsigned long i = 0; i < count; ++i) {
            slice.pixels.push_back(static_cast<double>(byteArray[i]) * slope + intercept);
        }
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
        if (pixCount == 0) {
            err = QString("%1：像素数据长度为 0").arg(path);
            return false;
        }
        slice.pixels.reserve(pixCount);
        for (unsigned long i = 0; i < pixCount; ++i) {
            double v;
            if (pixelRep == 1) {
                // PixelRepresentation=1 表示有符号，需按有符号 16 位解读
                v = static_cast<double>(static_cast<Sint16>(pixArray[i]));
            } else {
                v = static_cast<double>(pixArray[i]);
            }
            slice.pixels.push_back(v * slope + intercept);
        }
    }

    // 空间位置（按 Z 排序切片）
    const Float64 *posPtr = nullptr;
    unsigned long posCount = 0;
    if (ds->findAndGetFloat64Array(DCM_ImagePositionPatient, posPtr, &posCount).good() && posCount >= 3) {
        slice.z = posPtr[2];
    } else {
        Uint16 inst = 0;
        ds->findAndGetUint16(DCM_InstanceNumber, inst);
        slice.instance = static_cast<int>(inst);
    }

    // 间距
    const Float64 *spacingPtr = nullptr;
    unsigned long spacingCount = 0;
    if (ds->findAndGetFloat64Array(DCM_PixelSpacing, spacingPtr, &spacingCount).good() && spacingCount >= 2) {
        slice.resX = spacingPtr[0];
        slice.resY = spacingPtr[1];
    }
    Float64 thick = 1.0;
    if (ds->findAndGetFloat64(DCM_SliceThickness, thick).good()) {
        slice.resZ = thick;
    }
    return true;
}

/** 最近邻三维重采样：原始 (cx,cy,cz) → 目标 (dim)³
 *
 * 输出保留原始浮点值（未量化），灰度归一化统一在 loadVolume 中做。
 */
void resample(const std::vector<const Slice *> &slicePtr,
              int cx, int cy, int cz,
              int dim,
              std::vector<double> &out)
{
    const size_t sdim = static_cast<size_t>(dim);
    const size_t scx = static_cast<size_t>(cx);
    const int lastX = cx - 1, lastY = cy - 1, lastZ = cz - 1, lastD = dim - 1;

    out.assign(sdim * sdim * sdim, 0.0);
    for (int zi = 0; zi < dim; ++zi) {
        const int zs = std::clamp(static_cast<int>(static_cast<double>(zi) * lastZ / lastD + 0.5), 0, lastZ);
        const Slice *s = slicePtr[static_cast<size_t>(zs)];
        for (int yi = 0; yi < dim; ++yi) {
            const int ys = std::clamp(static_cast<int>(static_cast<double>(yi) * lastY / lastD + 0.5), 0, lastY);
            for (int xi = 0; xi < dim; ++xi) {
                const int xs = std::clamp(static_cast<int>(static_cast<double>(xi) * lastX / lastD + 0.5), 0, lastX);
                const size_t srcIdx = static_cast<size_t>(ys) * scx + static_cast<size_t>(xs);
                const double val = s->pixels[srcIdx];
                // 转置式填入（DICOM 行主序 → 体积 z 轴最外）
                const size_t dstIdx = (static_cast<size_t>(zi) * sdim + static_cast<size_t>(yi)) * sdim
                                      + static_cast<size_t>(xi);
                out[dstIdx] = val;
            }
        }
    }
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
                             int targetDim)
{
    const QDir dir(directory);
    if (!dir.exists()) {
        err = QString("目录不存在：%1").arg(directory);
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

    // 解析每个切片
    std::vector<Slice> slices;
    slices.reserve(static_cast<size_t>(files.size()));
    for (const QFileInfo &fi : files) {
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

    // 按 Z 空间排序（无 Z 则按 InstanceNumber）
    std::stable_sort(slices.begin(), slices.end(), [](const Slice &a, const Slice &b) {
        const double za = a.z != 0.0 ? a.z : static_cast<double>(a.instance);
        const double zb = b.z != 0.0 ? b.z : static_cast<double>(b.instance);
        return za < zb;
    });

    // 统计体积维度（取统一矩阵）
    const int rows = slices.front().rows;
    const int cols = slices.front().cols;
    const int slicesN = static_cast<int>(slices.size());
    qInfo() << "[DicomReader] 解析完成：" << slicesN << "张切片"
            << cols << "×" << rows
            << "→ 重采样到" << targetDim << "³";

    std::vector<const Slice *> ptr;
    ptr.reserve(slices.size());
    for (const Slice &s : slices) ptr.push_back(&s);

    std::vector<double> vol;
    resample(ptr, cols, rows, slicesN, targetDim, vol);

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
                   QVariantList() << slices.front().resX << slices.front().resY << slices.front().resZ);
    outMeta.insert(QStringLiteral("min"), vmin);
    outMeta.insert(QStringLiteral("max"), vmax);
    outMeta.insert(QStringLiteral("slices"), slicesN);
    outMeta.insert(QStringLiteral("source"), directory);
    return true;
}