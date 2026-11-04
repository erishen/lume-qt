#pragma once

#include <QByteArray>
#include <QString>
#include <QVariantMap>
#include <QObject>

/**
 * DicomReader — 用 DCMTK 解析 DICOM 序列并重建为归一化体积。
 *
 * 用法（应在后台线程调用，避免阻塞 UI）：
 *   QByteArray data;      // 输出：uint8 体积数据（Dim×Dim×Dim）
 *   QVariantMap meta;     // 输出：{ dims:[d,d,d], spacing:[...], min,max, slices }
 *   QString err;
 *   bool ok = DicomReader::loadVolume("/path/to/dicoms", data, meta, err);
 */
class DicomReader : public QObject
{
    Q_OBJECT
public:
    explicit DicomReader(QObject *parent = nullptr);

    /**
     * 扫描 directory 下所有 .dcm，按空间 Z 排序重建体积。
     * 降采样到 targetDim³（最近邻），灰度归一化为 uint8。
     */
    static bool loadVolume(const QString &directory,
                           QByteArray &outData,
                           QVariantMap &outMeta,
                           QString &err,
                           int targetDim = 128);
};