#pragma once

#include <QVariant>
#include <QVariantMap>
#include <QVector>
#include <QWidget>

/**
 * NativeVitalStrip — 关键波形（ECG）的「原生绘制通道」。
 *
 * 为什么需要它：网页侧那条 ECG 走的是 C++ → QWebChannel(IPC) → 渲染进程 →
 * Canvas 的链路，延迟里有 WebEngine 的 IPC 与合成开销，且无法保证确定性
 * （GC、后台节流、系统降频都会让帧间隔抖动）。术中关键波形与报警的主链路
 * 不应该建立在这条路上，所以这里用 QPainter 直接在宿主窗口里画一遍同一路
 * 信号：信号直连、无 IPC、无 WebView。
 *
 * 它同时也是对照物：条上会打印「采集 → 绘制」的 P50/P99，可以和网页那张
 * 延迟卡片并排比较，量化「原生 vs WebView」差多少。
 *
 * 注意：阈值报警是演示级的，不做医学报警算法的任何声明（见 README）。
 */
class NativeVitalStrip : public QWidget
{
    Q_OBJECT

public:
    explicit NativeVitalStrip(QWidget *parent = nullptr);

public slots:
    /// 与 Bridge::ecgSample(double, double) 直连（不经过 QWebChannel）
    void pushEcgSample(double value, double tsUs);
    /// 与 Bridge::vitalsReady(QVariantMap) 直连
    void setVitals(const QVariantMap &vitals);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    static double nowMicros();
    void recordAge(double ageMs);
    static double percentile(const QVector<double> &sorted, double q);

    QVector<double> m_trace;      // 波形样本，末尾为最新
    QVector<double> m_ages;       // 「采集 → 本次绘制」延迟样本（ms）
    double m_lastTsUs = 0.0;
    bool m_hasSample = false;

    double m_hr = 0.0;
    double m_spo2 = 0.0;
    double m_sys = 0.0;
    double m_dia = 0.0;
    bool m_hasVitals = false;
    qint64 m_paintCount = 0;
};
