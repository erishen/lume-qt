#pragma once

#include "deviceprovider.h"

#include <QTimer>
#include <QVariant>
#include <QVariantMap>

/**
 * SimulatorProvider — 内置模拟数据源（DeviceProvider 的一种实现）。
 *
 * 原来这些定时器与波形合成放在 Bridge 里，Bridge 既是设备又是传输层。
 * 拆出来之后 Bridge 只做转发，换真实设备（串口/网口/DICOM 网络）时替换 provider 即可。
 *
 * 时间戳语义：用宿主 system_clock 打戳（hasHardwareClock() == false）。
 * 真实设备接入时这里应换成设备时间戳，否则「采集时刻」本身不可信。
 */
class SimulatorProvider : public DeviceProvider
{
    Q_OBJECT

public:
    explicit SimulatorProvider(QObject *parent = nullptr);

    QString kind() const override { return QStringLiteral("simulator"); }
    QString description() const override;

    /// 时间戳来自「设备侧」时返回 true。模拟器默认为宿主墙钟（false）；
    /// 用 QTRS_CLOCK_OFFSET_MS 模拟设备时钟后即为 true。
    bool hasHardwareClock() const override { return m_clockOffsetUs != 0.0; }

    bool startMedical() override;
    void stopMedical() override;
    bool startNeurology() override;
    void stopNeurology() override;

private:
    static double nowMicros();
    /// 采集时刻：宿主墙钟 + 设备时钟偏移（模拟真实设备时基与宿主不同源）
    double stampMicros() const { return nowMicros() + m_clockOffsetUs; }
    static double clampTo(double v, double lo, double hi);
    static double randomUnit();
    static double randomRange(double lo, double hi);
    static double synthesizeEcg(double phase);
    static double synthesizePleth(double phase);
    double synthesizeEeg();
    double synthesizeEmg();
    double synthesizeNeural();

    void emitEcgSample();
    void emitSpo2Sample();
    void emitVitals();
    void emitEegSample();
    void emitEmgSample();
    void emitNeuralSpike();
    void emitIntervention();

    /// 设备时钟与宿主时钟的偏移（微秒）。0 表示与宿主同源。
    double m_clockOffsetUs = 0.0;

    // 医疗模拟状态
    QTimer *m_ecgTimer = nullptr;
    QTimer *m_spo2Timer = nullptr;
    QTimer *m_vitalsTimer = nullptr;
    double m_ecgPhase = 0.0;
    double m_spo2Phase = 0.0;
    double m_hr = 72.0;
    /// 可注入的固定生命体征：用于验证报警链路（QTRS_SIM_HR / QTRS_SIM_SPO2）。
    double m_hrFixed = -1.0;
    double m_spo2Fixed = -1.0;
    double m_spo2 = 98.0;
    double m_rr = 16.0;
    double m_sys = 118.0;
    double m_dia = 76.0;
    double m_temp = 36.6;
    int m_vitalsCount = 0;
    bool m_medicalActive = false;

    // 神经内科模拟状态
    QTimer *m_eegTimer = nullptr;
    QTimer *m_emgTimer = nullptr;
    QTimer *m_neuralTimer = nullptr;
    QTimer *m_interventionTimer = nullptr;
    bool m_neurologyActive = false;
    int m_interventionCount = 0;

    // EEG 波段强度（随机游走，模拟不同意识状态）
    double m_alpha = 0.8;   // 8-13 Hz
    double m_beta = 0.4;    // 13-30 Hz
    double m_gamma = 0.2;   // 30-100 Hz
    double m_delta = 0.1;   // 0.5-4 Hz

    double m_eegPhase = 0.0;
    double m_emgBurst = 0.0;
    double m_neuralSpikeEnv = 0.0;
};
