#include "simulatorprovider.h"

#include <QDateTime>
#include <QDebug>
#include <QTimer>

#include <chrono>
#include <cmath>
#include <cstdlib>

// 压力模式：QT_STRESS_LEVEL 环境变量 >0 时，采样率提高 8-10 倍（模拟医疗大数据量）
static int stressLevel()
{
    return qEnvironmentVariableIntValue("QT_STRESS_LEVEL");
}

double SimulatorProvider::nowMicros()
{
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    return static_cast<double>(us);
}

double SimulatorProvider::randomUnit()
{
    return std::rand() / static_cast<double>(RAND_MAX);
}

double SimulatorProvider::randomRange(double lo, double hi)
{
    return lo + (hi - lo) * randomUnit();
}

SimulatorProvider::SimulatorProvider(QObject *parent)
    : DeviceProvider(parent)
{
    // 模拟「设备时钟与宿主时钟不同源」：QTRS_CLOCK_OFFSET_MS 表示设备时基相对
    // 宿主快/慢多少毫秒。接真实设备后这个偏移是真实存在的（未知且恒定），
    // 前端必须能把它与真实延迟区分开，否则延迟看板全是假值。
    m_clockOffsetUs = qEnvironmentVariableIntValue("QTRS_CLOCK_OFFSET_MS") * 1000.0;

    const int stress = stressLevel();

    // ECG 波形样本：40ms 采样（25 Hz），模拟监护仪刷新率
    m_ecgTimer = new QTimer(this);
    m_ecgTimer->setInterval(stress > 0 ? 5 : 40);
    connect(m_ecgTimer, &QTimer::timeout, this, &SimulatorProvider::emitEcgSample);

    // 生命体征快照：600ms 一次
    // 故障注入（演示与验证用）：QTRS_FAULT 里出现通道名，就在启动 3 秒后停掉该通道，
    // 用来确认「信号中断」会被原生看门狗与前端看门狗分别捕获，而不是静默画旧波形。
    const QByteArray fault = qgetenv("QTRS_FAULT");
    auto injectFault = [](const char *name, QTimer *timer) {
        QTimer::singleShot(3000, timer, [name, timer] {
            if (!timer->isActive()) {
                return;
            }
            timer->stop();
            qWarning().noquote() << QStringLiteral("[Bridge] 故障注入：%1 通道已停止").arg(name);
        });
    };

    m_spo2Timer = new QTimer(this);
    if (qEnvironmentVariableIsSet("QTRS_SIM_HR")) {
        m_hrFixed = qEnvironmentVariable("QTRS_SIM_HR").toDouble();
    }
    if (qEnvironmentVariableIsSet("QTRS_SIM_SPO2")) {
        m_spo2Fixed = qEnvironmentVariable("QTRS_SIM_SPO2").toDouble();
    }
    m_spo2Timer->setInterval(stress > 0 ? 5 : 40);
    connect(m_spo2Timer, &QTimer::timeout, this, &SimulatorProvider::emitSpo2Sample);

    m_vitalsTimer = new QTimer(this);
    m_vitalsTimer->setInterval(stress > 0 ? 100 : 600);
    connect(m_vitalsTimer, &QTimer::timeout, this, &SimulatorProvider::emitVitals);

    if (fault.contains("spo2")) {
        injectFault("SpO2", m_spo2Timer);
    }
    if (fault.contains("ecg")) {
        injectFault("ECG", m_ecgTimer);
    }
    if (fault.contains("vitals")) {
        injectFault("Vitals", m_vitalsTimer);
    }

    // ---- 神经内科介入模拟定时器 ----
    // EEG：50ms（20 Hz）采样
    m_eegTimer = new QTimer(this);
    m_eegTimer->setInterval(stress > 0 ? 10 : 50);
    connect(m_eegTimer, &QTimer::timeout, this, &SimulatorProvider::emitEegSample);

    // EMG：100ms（10 Hz）采样
    m_emgTimer = new QTimer(this);
    m_emgTimer->setInterval(stress > 0 ? 20 : 100);
    connect(m_emgTimer, &QTimer::timeout, this, &SimulatorProvider::emitEmgSample);

    // 神经元活动：75ms 采样
    m_neuralTimer = new QTimer(this);
    m_neuralTimer->setInterval(stress > 0 ? 10 : 75);
    connect(m_neuralTimer, &QTimer::timeout, this, &SimulatorProvider::emitNeuralSpike);

    // 神经干预事件：500ms 一次
    m_interventionTimer = new QTimer(this);
    m_interventionTimer->setInterval(stress > 0 ? 50 : 500);
    connect(m_interventionTimer, &QTimer::timeout, this, &SimulatorProvider::emitIntervention);

    if (m_clockOffsetUs != 0.0) {
        qInfo().noquote() << QStringLiteral("[Simulator] 设备时钟偏移注入：%1 ms（此时基与宿主不同源）")
                                 .arg(m_clockOffsetUs / 1000.0);
    }
    if (stress > 0) {
        qInfo() << "[Simulator] 压力模式 QT_STRESS_LEVEL =" << stress
                << "— ECG" << m_ecgTimer->interval() << "ms"
                << "EEG" << m_eegTimer->interval() << "ms"
                << "EMG" << m_emgTimer->interval() << "ms"
                << "IV" << m_interventionTimer->interval() << "ms";
    }
}

bool SimulatorProvider::startMedical()
{
    qInfo() << "[Simulator] 启动医疗实时数据模拟";
    m_ecgTimer->start();
    m_spo2Timer->start();
    m_vitalsTimer->start();
    m_medicalActive = true;
    return true;
}

void SimulatorProvider::stopMedical()
{
    qInfo() << "[Simulator] 停止医疗实时数据模拟";
    m_ecgTimer->stop();
    m_spo2Timer->stop();
    m_vitalsTimer->stop();
    m_medicalActive = false;
}

void SimulatorProvider::emitEcgSample()
{
    // 心率 72 bpm ≈ 1.2 周期/秒；每 40ms 相位步进 1.2*0.04 = 0.048 周期
    m_ecgPhase += 0.048;
    if (m_ecgPhase >= 1.0) {
        m_ecgPhase -= 1.0;
    }
    emit ecgSample(synthesizeEcg(m_ecgPhase), stampMicros());
}

double SimulatorProvider::synthesizeEcg(double phase)
{
    // 经典心电图形态：P 波 → QRS 波群 → T 波，高斯峰叠加
    auto gauss = [](double x, double center, double width, double amp) {
        const double d = (x - center) / width;
        return amp * std::exp(-d * d);
    };

    double v = 0.0;
    v += gauss(phase, 0.18, 0.035, 0.12);   // P 波
    v -= gauss(phase, 0.32, 0.012, 0.10);   // Q 波
    v += gauss(phase, 0.36, 0.014, 1.00);   // R 波（主峰）
    v -= gauss(phase, 0.40, 0.012, 0.22);   // S 波
    v += gauss(phase, 0.62, 0.045, 0.28);   // T 波

    // 基线呼吸性漂移 + 轻微噪声
    v += 0.02 * std::sin(phase * 2.0 * M_PI * 0.5);
    v += (randomUnit() - 0.5) * 0.012;
    return v;
}

void SimulatorProvider::emitSpo2Sample()
{
    // 血氧波形固定在 75 次/分附近（1.25 周期/秒），每 40ms 步进 0.05 周期；
    // 振幅随当前 SpO₂ 轻微变化，用来区分「波形停了」和「数值变了」。
    m_spo2Phase += 0.05;
    if (m_spo2Phase >= 1.0) {
        m_spo2Phase -= 1.0;
    }
    emit spo2Sample(synthesizePleth(m_spo2Phase), stampMicros());
}

double SimulatorProvider::synthesizePleth(double phase)
{
    // 体积描记波：快速升支 + 主峰 + 降支上的重搏切迹（dicrotic notch）
    auto gauss = [](double x, double center, double width, double amp) {
        const double d = (x - center) / width;
        return amp * std::exp(-d * d);
    };

    double v = 0.0;
    v += gauss(phase, 0.12, 0.045, 1.0);   // 升支/主峰
    v += gauss(phase, 0.34, 0.050, 0.42);   // 降支
    v -= gauss(phase, 0.30, 0.020, 0.22);   // 重搏切迹
    v -= gauss(phase, 0.62, 0.090, 0.10);   // 舒张期回落后的小波

    // 基线与呼吸调制
    v += 0.03 * std::sin(phase * 2.0 * M_PI * 0.4);
    v += (randomUnit() - 0.5) * 0.010;
    return v;
}

void SimulatorProvider::emitVitals()
{
    // 生理参数随机游走，贴近真实波动
    auto drift = [](double cur, double step, double lo, double hi) {
        double v = cur + (std::rand() % 7 - 3) * step;
        return clampTo(v, lo, hi);
    };

    // 注入值固定不动：报警验证需要「一直低」而不是随机游走偶尔穿过阈值。
    m_hr   = m_hrFixed >= 0.0 ? m_hrFixed : drift(m_hr, 0.2, 58.0, 102.0);
    m_spo2 = m_spo2Fixed >= 0.0 ? m_spo2Fixed : drift(m_spo2, 0.1, 93.0, 100.0);
    m_rr   = drift(m_rr,   0.1,  12.0,  24.0);
    m_sys  = drift(m_sys,  0.3, 108.0, 142.0);
    m_dia  = drift(m_dia,  0.2,  68.0,  92.0);
    m_temp = drift(m_temp, 0.02, 36.0,  38.0);

    QVariantMap vitals;
    vitals.insert(QStringLiteral("hr"),   std::round(m_hr));
    vitals.insert(QStringLiteral("spo2"), std::round(m_spo2));
    vitals.insert(QStringLiteral("rr"),   std::round(m_rr));
    vitals.insert(QStringLiteral("sys"),  std::round(m_sys));
    vitals.insert(QStringLiteral("dia"),  std::round(m_dia));
    vitals.insert(QStringLiteral("temp"), std::round(m_temp * 10.0) / 10.0);
    vitals.insert(QStringLiteral("time"),
                  QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")));
    vitals.insert(QStringLiteral("tsUs"), stampMicros());

    // 诊断：仅记录前 3 条推送，验证实时数据通道
    if (m_vitalsCount < 3) {
        qInfo().noquote() << "[Simulator] vitalsReady #" << (m_vitalsCount + 1) << ":" << vitals;
        ++m_vitalsCount;
    }

    emit vitalsReady(vitals);
}


bool SimulatorProvider::startNeurology()
{
    if (m_neurologyActive) return true;
    m_neurologyActive = true;
    qInfo() << "[Simulator] 启动神经内科介入模拟";
    m_eegTimer->start();
    m_emgTimer->start();
    m_neuralTimer->start();
    m_interventionTimer->start();
    return true;
}

void SimulatorProvider::stopNeurology()
{
    if (!m_neurologyActive) return;
    m_neurologyActive = false;
    qInfo() << "[Simulator] 停止神经内科介入模拟";
    m_eegTimer->stop();
    m_emgTimer->stop();
    m_neuralTimer->stop();
    m_interventionTimer->stop();
}

double SimulatorProvider::synthesizeEeg()
{
    // 脑电图：α/β/γ/δ 波段叠加 + 噪声，模拟不同意识状态下的脑电
    // 波段强度缓慢随机游走，模拟清醒→嗜睡→睡眠的过渡
    m_alpha   = clampTo(m_alpha   + randomRange(-0.03, 0.03), 0.1, 1.2);
    m_beta    = clampTo(m_beta    + randomRange(-0.02, 0.02), 0.05, 1.0);
    m_gamma   = clampTo(m_gamma   + randomRange(-0.02, 0.02), 0.05, 0.8);
    m_delta   = clampTo(m_delta   + randomRange(-0.02, 0.02), 0.02, 0.6);

    m_eegPhase += 0.5; // 相位推进（20 Hz 采样 → 每样本 0.5 rad @ ~6.4 rad/s 基频）
    if (m_eegPhase > 2.0 * M_PI) m_eegPhase -= 2.0 * M_PI;

    double v = 0.0;
    v += m_alpha * std::sin(m_eegPhase * 3.0);          // ~10 Hz
    v += m_beta  * std::sin(m_eegPhase * 6.5);          // ~20 Hz
    v += m_gamma * std::sin(m_eegPhase * 12.0);         // ~38 Hz
    v += m_delta * std::sin(m_eegPhase * 1.2);          // ~4 Hz

    // 少量背景噪声（微伏级脑电）
    v += (randomUnit() - 0.5) * 0.08;

    // 偶发棘波（癫痫样放电，3% 概率）
    if (randomUnit() < 0.03) {
        v += randomRange(0.4, 1.2);
    }

    return clampTo(v, -2.0, 2.0);
}

double SimulatorProvider::synthesizeEmg()
{
    // 肌电图：基线噪声 + 偶发肌肉收缩爆发（运动单位电位）
    // 爆发包络缓入缓出
    m_emgBurst = clampTo(m_emgBurst + randomRange(-0.12, 0.12), 0.0, 1.0);
    if (randomUnit() < 0.04) m_emgBurst = 1.0; // 触发一次肌肉收缩

    double v = 0.0;
    v += (randomUnit() - 0.5) * 0.3;                 // 基线噪声
    v += m_emgBurst * (randomUnit() - 0.5) * 2.4;    // 收缩时的高频放电
    v += m_emgBurst * 0.3 * std::sin(m_eegPhase * 2.0); // 低频包络

    // EMG 幅度通常为 mV 级，归一化到 [0,1]
    return clampTo(0.5 + v * 0.5, 0.0, 1.0);
}

double SimulatorProvider::synthesizeNeural()
{
    // 神经元活动：背景振荡 + 随机棘波（动作电位）
    m_neuralSpikeEnv = clampTo(m_neuralSpikeEnv + randomRange(-0.08, 0.08), 0.0, 1.0);

    double v = 0.0;
    v += 0.15 * std::sin(m_eegPhase * 0.7);          // 低频背景振荡
    v += (randomUnit() - 0.5) * 0.1;                 // 背景噪声

    // 棘波：2% 概率触发一个动作电位尖峰
    if (randomUnit() < 0.02) {
        m_neuralSpikeEnv = 1.0;
    }
    v += m_neuralSpikeEnv * (randomUnit() - 0.2) * 0.8; // 棘波贡献

    return clampTo(v, -1.0, 1.0);
}

void SimulatorProvider::emitEegSample()
{
    emit eegSample(synthesizeEeg(), stampMicros());
}

void SimulatorProvider::emitEmgSample()
{
    emit emgSample(synthesizeEmg(), stampMicros());
}

void SimulatorProvider::emitNeuralSpike()
{
    emit neuralSpike(synthesizeNeural(), stampMicros());
}

void SimulatorProvider::emitIntervention()
{
    // 随机选择一种神经内科介入方式，生成事件快照
    static const char *labels[] = { "TMS", "DBS", "ECT", "NF" };
    const int idx = std::rand() % 4;

    QVariantMap iv;
    iv.insert(QStringLiteral("type"), labels[idx]);
    iv.insert(QStringLiteral("label"), QString::fromLatin1(labels[idx]));
    iv.insert(QStringLiteral("intensity"), std::round(randomRange(30.0, 100.0)));
    iv.insert(QStringLiteral("frequency"), std::round(randomRange(1.0, 130.0)));
    iv.insert(QStringLiteral("duration"), std::round(randomRange(5.0, 60.0)));
    iv.insert(QStringLiteral("status"), randomUnit() < 0.7 ? QStringLiteral("active")
                                                           : QStringLiteral("completed"));
    iv.insert(QStringLiteral("time"),
              QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")));

    // 诊断：仅记录前 3 条介入事件，验证 C++ → JS 神经学通道
    if (m_interventionCount < 3) {
        qInfo().noquote() << "[Simulator] interventionReady #" << (m_interventionCount + 1) << ":" << iv;
        ++m_interventionCount;
    }

    iv.insert(QStringLiteral("tsUs"), stampMicros());
    emit interventionReady(iv);
}


double SimulatorProvider::clampTo(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}


QString SimulatorProvider::description() const
{
    const QString clock = m_clockOffsetUs != 0.0
        ? QStringLiteral("设备时钟时基，偏移 %1ms").arg(m_clockOffsetUs / 1000.0)
        : QStringLiteral("宿主墙钟时基");
    return QStringLiteral("内置生理信号模拟器（ECG %1ms / SpO₂ %2ms / vitals %3ms，%4）")
        .arg(m_ecgTimer ? m_ecgTimer->interval() : 0)
        .arg(m_spo2Timer ? m_spo2Timer->interval() : 0)
        .arg(m_vitalsTimer ? m_vitalsTimer->interval() : 0)
        .arg(clock);
}
