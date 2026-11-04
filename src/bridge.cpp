#include "bridge.h"
#include "dicomreader.h"

#include <QDateTime>
#include <QDebug>
#include <QMetaObject>
#include <QSysInfo>
#include <QTimer>

#include <cmath>
#include <cstdlib>

// ---------------------------------------------------------------------------
// 随机数辅助
// ---------------------------------------------------------------------------

double Bridge::randomUnit()
{
    return std::rand() / static_cast<double>(RAND_MAX);
}

double Bridge::randomRange(double lo, double hi)
{
    return lo + (hi - lo) * randomUnit();
}

// 压力模式：QT_STRESS_LEVEL 环境变量 >0 时，采样率提高 8-10 倍（模拟医疗大数据量）
static int stressLevel()
{
    return qEnvironmentVariableIntValue("QT_STRESS_LEVEL");
}

Bridge::Bridge(QObject *parent)
    : QObject(parent)
{
    const int stress = stressLevel();

    // ECG 波形样本：40ms 采样（25 Hz），模拟监护仪刷新率
    m_ecgTimer = new QTimer(this);
    m_ecgTimer->setInterval(stress > 0 ? 5 : 40);
    connect(m_ecgTimer, &QTimer::timeout, this, &Bridge::emitEcgSample);

    // 生命体征快照：600ms 一次
    m_vitalsTimer = new QTimer(this);
    m_vitalsTimer->setInterval(stress > 0 ? 100 : 600);
    connect(m_vitalsTimer, &QTimer::timeout, this, &Bridge::emitVitals);

    // ---- 神经内科介入模拟定时器 ----
    // EEG：50ms（20 Hz）采样
    m_eegTimer = new QTimer(this);
    m_eegTimer->setInterval(stress > 0 ? 10 : 50);
    connect(m_eegTimer, &QTimer::timeout, this, &Bridge::emitEegSample);

    // EMG：100ms（10 Hz）采样
    m_emgTimer = new QTimer(this);
    m_emgTimer->setInterval(stress > 0 ? 20 : 100);
    connect(m_emgTimer, &QTimer::timeout, this, &Bridge::emitEmgSample);

    // 神经元活动：75ms 采样
    m_neuralTimer = new QTimer(this);
    m_neuralTimer->setInterval(stress > 0 ? 10 : 75);
    connect(m_neuralTimer, &QTimer::timeout, this, &Bridge::emitNeuralSpike);

    // 神经干预事件：500ms 一次
    m_interventionTimer = new QTimer(this);
    m_interventionTimer->setInterval(stress > 0 ? 50 : 500);
    connect(m_interventionTimer, &QTimer::timeout, this, &Bridge::emitIntervention);

    if (stress > 0) {
        qInfo() << "[Bridge] ⚡ 压力模式 QT_STRESS_LEVEL =" << stress
                << "— ECG" << m_ecgTimer->interval() << "ms"
                << "EEG" << m_eegTimer->interval() << "ms"
                << "EMG" << m_emgTimer->interval() << "ms"
                << "IV" << m_interventionTimer->interval() << "ms";
    }
}

Bridge::~Bridge()
{
    // 若 DICOM 解析仍在进行：先置 alive=false（位于堆上、独立于 this），
    // 再 join 等待线程结束。已入队的回调会因 alive=false 跳过对 this 的访问，
    // 避免 Bridge 释放后的 use-after-free。
    if (m_dicomHolder) {
        m_dicomHolder->alive = false;
        if (m_dicomHolder->thread && m_dicomHolder->thread->joinable())
            m_dicomHolder->thread->join();
        // 释放本副本；若仍有已入队闭包持有 holder 副本，它会存活到那些
        // 闭包执行完毕（此时 alive 已为 false，不会访问 this）。
    }
}

void Bridge::loadDicomVolume(const QString &directory)
{
    // 重入保护：只允许一个在途 DICOM 解析（UI 实际只触发一次，此处仅为
    // 健壮性兜底；如需排队可改为任务队列实现）。
    if (m_dicomHolder) {
        qWarning() << "[Bridge] DICOM 解析已在进行中，忽略重复请求：" << directory;
        return;
    }

    qInfo().noquote() << "[Bridge] 开始后台解析 DICOM：" << directory;

    // holder 位于堆上，worker 闭包与收尾闭包各持一份 shared_ptr 副本：
    //  - 收尾闭包仅操作 holder->thread，绝不读成员指针，因此不会被之后
    //    新建的线程误伤（修 #7 竞态）；
    //  - alive 标志独立于 Bridge，Bridge 析构后仍可被已入队回调安全读取。
    auto holder = std::make_shared<DicomThreadHolder>();
    holder->alive = true;
    holder->thread = std::make_unique<std::thread>([this, directory, holder]() {
        QByteArray data;
        QVariantMap meta;
        QString err;
        const bool ok = DicomReader::loadVolume(directory, data, meta, err);

        // 结果必须回到 Qt 主线程再发射信号（QWebChannel 依赖所在线程）。
        // 闭包通过 holder->alive 判断 Bridge 是否仍存活，避免析构后
        // 对已释放 this 的访问（use-after-free）。
        if (ok) {
            const QString base64 = QString::fromLatin1(data.toBase64());
            QMetaObject::invokeMethod(this, [this, holder, meta, base64]() {
                if (holder->alive) emit dicomVolumeReady(meta, base64);
            });
        } else {
            QMetaObject::invokeMethod(this, [this, holder, err]() {
                if (holder->alive) emit dicomVolumeError(err);
            });
        }

        // 回到主线程收尾：仅 join/释放“本线程”对象（holder->thread），
        // 不读成员指针，因此即便期间又发起了新的 loadDicomVolume，
        // 也不会误伤新线程。
        QMetaObject::invokeMethod(this, [this, holder]() {
            if (!holder->alive) return;   // Bridge 已析构，跳过（不访问 this）
            if (holder->thread && holder->thread->joinable())
                holder->thread->join();
            if (m_dicomHolder == holder)
                m_dicomHolder.reset();
        });
    });

    m_dicomHolder = holder;
}

QString Bridge::appName() const
{
    return QStringLiteral("qt-react-shell");
}

QString Bridge::platform() const
{
    return QSysInfo::prettyProductName();
}

QString Bridge::getPlatformInfo() const
{
    return QStringLiteral("%1 — %2 (Qt %3)")
        .arg(platform(),
             QSysInfo::currentCpuArchitecture(),
             QString::fromLatin1(qVersion()));
}

QString Bridge::dicomSamplePath() const
{
    return QStringLiteral(DICOM_SAMPLE_PATH);
}

QString Bridge::showMessage(const QString &message)
{
    ++m_callCount;
    qInfo() << "[Bridge] showMessage from JS:" << message;

    // 演示 JS → C++ 返回值（C++ 可加工后回传）
    return QStringLiteral("C++ 已收到（第 %1 次调用）: %2").arg(m_callCount).arg(message);
}

int Bridge::add(int a, int b)
{
    return a + b;
}

void Bridge::setDataFromCpp(const QVariant &data)
{
    qInfo() << "[Bridge] setDataFromCpp from JS:" << data;

    // 回推一个带时间戳的数据，演示 C++ → JS 主动推送
    QVariantMap payload;
    payload.insert(QStringLiteral("time"), QDateTime::currentDateTime().toString(Qt::ISODate));
    payload.insert(QStringLiteral("echo"), data);
    payload.insert(QStringLiteral("seq"), m_callCount);
    emit dataFromCpp(payload);
}

// ---------------------------------------------------------------------------
// 医疗影像实时数据模拟
// ---------------------------------------------------------------------------

void Bridge::startMedicalSimulation()
{
    qInfo() << "[Bridge] 启动医疗实时数据模拟";
    m_ecgTimer->start();
    m_vitalsTimer->start();
}

void Bridge::stopMedicalSimulation()
{
    qInfo() << "[Bridge] 停止医疗实时数据模拟";
    m_ecgTimer->stop();
    m_vitalsTimer->stop();
}

void Bridge::emitEcgSample()
{
    // 心率 72 bpm ≈ 1.2 周期/秒；每 40ms 相位步进 1.2*0.04 = 0.048 周期
    m_ecgPhase += 0.048;
    if (m_ecgPhase >= 1.0) {
        m_ecgPhase -= 1.0;
    }
    emit ecgSample(synthesizeEcg(m_ecgPhase));
}

double Bridge::synthesizeEcg(double phase)
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

void Bridge::emitVitals()
{
    // 生理参数随机游走，贴近真实波动
    auto drift = [](double cur, double step, double lo, double hi) {
        double v = cur + (std::rand() % 7 - 3) * step;
        return clampTo(v, lo, hi);
    };

    m_hr   = drift(m_hr,   0.2,  58.0, 102.0);
    m_spo2 = drift(m_spo2, 0.1,  93.0, 100.0);
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

    // 诊断：仅记录前 3 条推送，验证实时数据通道
    if (m_vitalsCount < 3) {
        qInfo().noquote() << "[Bridge] vitalsReady #" << (m_vitalsCount + 1) << ":" << vitals;
        ++m_vitalsCount;
    }

    emit vitalsReady(vitals);
}

// ---------------------------------------------------------------------------
// 神经内科介入数据模拟
// ---------------------------------------------------------------------------

void Bridge::startNeurologySimulation()
{
    if (m_neurologyActive) return;
    m_neurologyActive = true;
    qInfo() << "[Bridge] 启动神经内科介入模拟";
    m_eegTimer->start();
    m_emgTimer->start();
    m_neuralTimer->start();
    m_interventionTimer->start();
}

void Bridge::stopNeurologySimulation()
{
    if (!m_neurologyActive) return;
    m_neurologyActive = false;
    qInfo() << "[Bridge] 停止神经内科介入模拟";
    m_eegTimer->stop();
    m_emgTimer->stop();
    m_neuralTimer->stop();
    m_interventionTimer->stop();
}

double Bridge::synthesizeEeg()
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

double Bridge::synthesizeEmg()
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

double Bridge::synthesizeNeural()
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

void Bridge::emitEegSample()
{
    emit eegSample(synthesizeEeg());
}

void Bridge::emitEmgSample()
{
    emit emgSample(synthesizeEmg());
}

void Bridge::emitNeuralSpike()
{
    emit neuralSpike(synthesizeNeural());
}

void Bridge::emitIntervention()
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
        qInfo().noquote() << "[Bridge] interventionReady #" << (m_interventionCount + 1) << ":" << iv;
        ++m_interventionCount;
    }

    emit interventionReady(iv);
}

double Bridge::clampTo(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
