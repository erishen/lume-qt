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
    std::vector<uint16_t> pixels; // rows × cols（原始 16 位，未应用 rescale）
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

    // 确保像素以未压缩原生格式可用（覆盖 RLE/JPEG 压缩）
    ds->chooseRepresentation(EXS_LittleEndianExplicit, static_cast<const DcmRepresentationParameter *>(nullptr));

    DcmElement *elem = nullptr;
    if (ds->findAndGetElement(DCM_PixelData, elem, OFFalse).bad() || !elem) {
        err = QString("%1：无像素数据").arg(path);
        return false;
    }
    auto *pdata = static_cast<DcmPixelData *>(elem);

    Uint16 *pixArray = nullptr;
    if (pdata->getUint16Array(pixArray).bad() || !pixArray) {
        err = QString("%1：像素无法读取为 16 位组件").arg(path);
        return false;
    }
    const unsigned long byteLen = pdata->getLength(EXS_LittleEndianExplicit);
    const unsigned long pixCount = byteLen / sizeof(Uint16);

    slice.rows = static_cast<int>(rows);
    slice.cols = static_cast<int>(cols);
    slice.pixels.assign(pixArray, pixArray + pixCount);

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

/** 最近邻三维重采样：原始 (cx,cy,cz) → 目标 (dim)³ */
void resample(const std::vector<const Slice *> &slicePtr,
              int cx, int cy, int cz,
              int dim,
              std::vector<uint8_t> &out)
{
    const size_t sdim = static_cast<size_t>(dim);
    const size_t scx = static_cast<size_t>(cx);
    const int lastX = cx - 1, lastY = cy - 1, lastZ = cz - 1, lastD = dim - 1;

    out.assign(sdim * sdim * sdim, 0);
    for (int zi = 0; zi < dim; ++zi) {
        const int zs = std::clamp(static_cast<int>(static_cast<double>(zi) * lastZ / lastD + 0.5), 0, lastZ);
        const Slice *s = slicePtr[static_cast<size_t>(zs)];
        for (int yi = 0; yi < dim; ++yi) {
            const int ys = std::clamp(static_cast<int>(static_cast<double>(yi) * lastY / lastD + 0.5), 0, lastY);
            for (int xi = 0; xi < dim; ++xi) {
                const int xs = std::clamp(static_cast<int>(static_cast<double>(xi) * lastX / lastD + 0.5), 0, lastX);
                const size_t srcIdx = static_cast<size_t>(ys) * scx + static_cast<size_t>(xs);
                const uint16_t val = s->pixels[srcIdx];
                // 转置式填入（DICOM 行主序 → 体积 z 轴最外）
                const size_t dstIdx = (static_cast<size_t>(zi) * sdim + static_cast<size_t>(yi)) * sdim
                                      + static_cast<size_t>(xi);
                out[dstIdx] = static_cast<uint8_t>((val >> 8) & 0xFF);
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

    std::vector<uint8_t> vol;
    resample(ptr, cols, rows, slicesN, targetDim, vol);

    // 归一化（min-max → 0-255 已在 resample 用高 8 位近似）
    // 使用 min-max 归一化增强对比度
    uint8_t lo = 255, hi = 0;
    for (const uint8_t v : vol) {
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    if (hi > lo) {
        const float range = static_cast<float>(hi - lo);
        for (uint8_t &v : vol) {
            v = static_cast<uint8_t>(static_cast<float>(v - lo) / range * 255.0f);
        }
    }

    outData = QByteArray(reinterpret_cast<const char *>(vol.data()),
                         static_cast<int>(vol.size()));

    outMeta.clear();
    outMeta.insert(QStringLiteral("dims"), QVariantList() << targetDim << targetDim << targetDim);
    outMeta.insert(QStringLiteral("spacing"),
                   QVariantList() << slices.front().resX << slices.front().resY << slices.front().resZ);
    outMeta.insert(QStringLiteral("min"), static_cast<double>(lo));
    outMeta.insert(QStringLiteral("max"), static_cast<double>(hi));
    outMeta.insert(QStringLiteral("slices"), slicesN);
    outMeta.insert(QStringLiteral("source"), directory);
    return true;
}