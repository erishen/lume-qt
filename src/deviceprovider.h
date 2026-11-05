#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

/**
 * DeviceProvider — 真实设备接入的抽象接口。
 *
 * 壳子里的数据只有模拟器一种实现，但术中接入的设备（监护仪、导联盒、导航系统、
 * DICOM 网络）形态差别很大：采样率、协议、时间基准都不同。把它们统一到这个接口，
 * Bridge 只负责「对外转发 + 冠时间戳」，换设备时不必改前端契约，也不必改原生条。
 *
 * 契约（继承者必须遵守）：
 *  - 每个波形样本都要带上**采集时刻**（epoch 微秒），不是「投递时刻」；
 *    模拟器用宿主墙钟，真实设备应尽量使用设备侧时间戳/PTP，并用
 *    hasHardwareClock() 如实声明——「采集时刻不可信」时上层算出来的延迟没有临床意义。
 *  - 只发信号、不碰 UI；UI 卡顿不能反过来阻塞采集。
 *  - startMedical()/stopMedical() 要幂等。
 */
class DeviceProvider : public QObject
{
    Q_OBJECT

public:
    explicit DeviceProvider(QObject *parent = nullptr) : QObject(parent) {}
    ~DeviceProvider() override = default;

    /// 设备类别标识，如 "simulator" / "serial-monitor" / "dicom-net"
    virtual QString kind() const = 0;
    /// 给日志与界面的可读描述（型号、端口、速率等）
    virtual QString description() const = 0;
    /// 时间戳是否来自设备侧（false = 用宿主墙钟，只能做相对测量）
    virtual bool hasHardwareClock() const { return false; }

    virtual bool startMedical() = 0;
    virtual void stopMedical() = 0;
    virtual bool startNeurology() = 0;
    virtual void stopNeurology() = 0;

signals:
    // 波形通道：value + 采集时刻（epoch 微秒）
    void ecgSample(double value, double tsUs);
    void spo2Sample(double value, double tsUs);
    void eegSample(double value, double tsUs);
    void emgSample(double value, double tsUs);
    void neuralSpike(double value, double tsUs);
    // 数值/事件快照：map 内必须带 tsUs
    void vitalsReady(const QVariantMap &vitals);
    void interventionReady(const QVariantMap &intervention);
};
