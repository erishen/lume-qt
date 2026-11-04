#pragma once

#include <QObject>
#include <QVariant>
#include <QVariantMap>

#include <atomic>
#include <memory>
#include <thread>

// DICOM 后台加载线程的持有结构（堆分配、由 shared_ptr 共享）。
//  - thread：后台解析线程（unique_ptr 保证单一所有权）；
//  - alive：生命周期标志，独立于 Bridge 对象，确保 Bridge 析构后
//    已入队的回调仍能安全读取它而不会产生 use-after-free。
struct DicomThreadHolder {
    std::unique_ptr<std::thread> thread;
    std::atomic<bool> alive{false};
};

class QTimer;

/**
 * Bridge — 通过 QWebChannel 暴露给前端 JS 的 C++ 对象。
 *
 * 前端可通过 `bridge` 访问以下接口（见 frontend/src/qwebchannel.ts）：
 *  - invoke: bridge.getPlatformInfo() / bridge.showMessage(msg) / bridge.add(a, b)
 *  - signal: bridge.dataFromCpp ← 由 C++ 主动推送数据到 JS
 *
 * 医疗影像实时数据模拟：
 *  - startMedicalSimulation() / stopMedicalSimulation() 控制
 *  - ecgSample(double) 每 40ms 推送一个心电图波形样本
 *  - vitalsReady(map)  每 600ms 推送一次生命体征快照
 *
 * 神经内科介入模拟：
 *  - startNeurologySimulation() / stopNeurologySimulation() 控制
 *  - eegSample(double)     每 50ms 推送脑电图（EEG）样本
 *  - emgSample(double)     每 100ms 推送肌电图（EMG）样本
 *  - neuralSpike(double)   每 75ms 推送神经元活动样本
 *  - interventionReady(map) 每 500ms 推送神经干预事件快照
 */
class Bridge : public QObject
{
    Q_OBJECT
    // 暴露给 JS 的属性（只读，经 Q_PROPERTY 自动同步到 QWebChannel）
    Q_PROPERTY(QString appName READ appName CONSTANT)
    Q_PROPERTY(QString platform READ platform CONSTANT)

public:
    explicit Bridge(QObject *parent = nullptr);
    ~Bridge() override;

    QString appName() const;
    QString platform() const;

    // Q_INVOKABLE 方法可被 JS 直接调用（qwebchannel 会生成对应 Promise）
    Q_INVOKABLE QString getPlatformInfo() const;
    Q_INVOKABLE QString showMessage(const QString &message);
    Q_INVOKABLE int add(int a, int b);

    // ---- 医疗实时数据模拟控制 ----
    Q_INVOKABLE void startMedicalSimulation();
    Q_INVOKABLE void stopMedicalSimulation();

    // ---- 神经内科介入模拟控制 ----
    Q_INVOKABLE void startNeurologySimulation();
    Q_INVOKABLE void stopNeurologySimulation();

    // ---- DICOM 体积加载（后台线程解析，结果经信号回传） ----
    Q_INVOKABLE void loadDicomVolume(const QString &directory);
    // 返回内置 DICOM 样例序列目录（脑部 T1 MRI）
    Q_INVOKABLE QString dicomSamplePath() const;

public slots:
    // JS 可通过 bridge.setDataFromCpp(...) 触发，用于演示双向通信
    void setDataFromCpp(const QVariant &data);

private slots:
    void emitEcgSample();
    void emitVitals();
    void emitEegSample();
    void emitEmgSample();
    void emitNeuralSpike();
    void emitIntervention();

signals:
    // C++ → JS 信号
    void dataFromCpp(const QVariant &data);
    // 医疗实时数据信号
    void ecgSample(double value);
    void vitalsReady(const QVariantMap &vitals);
    // 神经内科实时数据信号
    void eegSample(double value);
    void emgSample(double value);
    void neuralSpike(double value);
    void interventionReady(const QVariantMap &intervention);
    // DICOM 体积加载结果（meta 含 dims/spacing；base64Pixels 为 uint8 体积）
    void dicomVolumeReady(const QVariantMap &meta, const QString &base64Pixels);
    void dicomVolumeError(const QString &message);

private:
    static double synthesizeEcg(double phase);
    static double clampTo(double v, double lo, double hi);
    static double randomUnit();          // [0,1) 均匀随机数
    static double randomRange(double lo, double hi);
    double synthesizeEeg();              // 脑电图：α/β/γ/δ 波段叠加
    double synthesizeEmg();              // 肌电图：基线 + 突发
    double synthesizeNeural();           // 神经元活动：棘波 + 背景

    int m_callCount = 0;

    // 医疗模拟状态
    QTimer *m_ecgTimer = nullptr;
    QTimer *m_vitalsTimer = nullptr;
    double m_ecgPhase = 0.0;
    double m_hr = 72.0;
    double m_spo2 = 98.0;
    double m_rr = 16.0;
    double m_sys = 118.0;
    double m_dia = 76.0;
    double m_temp = 36.6;
    int m_vitalsCount = 0;

    // 神经内科模拟状态
    QTimer *m_eegTimer = nullptr;
    QTimer *m_emgTimer = nullptr;
    QTimer *m_neuralTimer = nullptr;
    QTimer *m_interventionTimer = nullptr;
    bool m_neurologyActive = false;
    int m_interventionCount = 0;   // 介入事件诊断日志计数（与 vitals 计数独立）

    // EEG 波段强度（随机游走，模拟不同意识状态）
    double m_alpha = 0.8;   // 8-13 Hz
    double m_beta = 0.4;    // 13-30 Hz
    double m_gamma = 0.2;   // 30-100 Hz
    double m_delta = 0.1;   // 0.5-4 Hz

    // EEG 相位累加器
    double m_eegPhase = 0.0;

    // EMG / 神经元状态
    double m_emgBurst = 0.0;      // 当前肌肉爆发包络
    double m_neuralSpikeEnv = 0.0; // 神经元棘波包络

    // DICOM 后台加载线程的持有结构（堆上、shared_ptr 管理）。
    // 使 worker 收尾闭包能精确删除“本线程”、并在 Bridge 析构后安全
    // 跳过已入队的回调（详见 bridge.cpp 的 loadDicomVolume / ~Bridge）。
    std::shared_ptr<DicomThreadHolder> m_dicomHolder;
};
