#pragma once

#include <QByteArray>
#include <QString>
#include <QVariantMap>
#include <QObject>

#include <atomic>

/**
 * DicomReader — 用 DCMTK 解析 DICOM 序列并重建为归一化体积。
 *
 * 用法（应在后台线程调用，避免阻塞 UI）：
 *   QByteArray data;      // 输出：uint8 体积数据（Dim×Dim×Dim）
 *   QVariantMap meta;     // 输出：{ dims:[d,d,d], spacing:[...], min,max, slices, skipped }
 *   QString err;
 *   bool ok = DicomReader::loadVolume("/path/to/dicoms", data, meta, err);
 *
 * 可选的 cancel 指针用于协作式取消（例如宿主对象析构时）：
 * 解析过程中每个文件 / 每个 Z 层检查一次，置位后尽快返回 false（err="解析已取消"）。
 */
class DicomReader : public QObject
{
    Q_OBJECT
public:
    explicit DicomReader(QObject *parent = nullptr);

    /**
     * 扫描 directory 下所有 .dcm，按空间 Z 排序重建体积。
     * 降采样到 targetDim³（最近邻），灰度归一化为 uint8。
     *
     * 健壮性约定：
     *  - 矩阵尺寸以出现次数最多的 (rows, cols) 为准，尺寸不一致的切片会被剔除
     *    （计入 meta.skipped），不会越界读；
     *  - Z 缺失用独立标记判断，不用 z==0 当哨兵；
     *  - spacing 的 Z 优先取相邻层间距中位数，缺失时退回 SliceThickness。
     */
    static bool loadVolume(const QString &directory,
                           QByteArray &outData,
                           QVariantMap &outMeta,
                           QString &err,
                           int targetDim = 128,
                           const std::atomic<bool> *cancel = nullptr);
};
