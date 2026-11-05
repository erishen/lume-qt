#pragma once

#include <QColor>
#include <QRectF>
#include <QVariant>
#include <QVariantMap>
#include <QVector>
#include <QWidget>

class QPainter;

/**
 * NativeVitalStrip — 关键波形（ECG / SpO₂）的「原生绘制通道」。
 *
 * 为什么需要它：网页侧那条波形走的是 C++ → QWebChannel(IPC) → 渲染进程 →
 * Canvas 的链路，延迟里有 WebEngine 的 IPC 与合成开销，且无法保证确定性
 * （遮挡节流、GC、系统降频都会让帧间隔抖动）。实测同一时刻网页侧到达中位数
 * 250–325ms，而这里直连信号是 0.2ms 级。术中关键波形的显示与报警判定不应该
 * 建立在那条路上，所以这里用 QPainter 直接在宿主窗口里画：信号直连、无 IPC。
 *
 * 除了画，它还承担两件「原生化」的职责：
 *  - 每个通道自己判断「信号中断」（超过阈值没样本 → 画面明确标注冻结帧）；
 *  - 报警判定在原生侧完成，不依赖前端是否渲染成功。
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
    /// 与 Bridge::spo2Sample(double, double) 直连（不经过 QWebChannel）
    void pushSpo2Sample(double value, double tsUs);
    /// 与 Bridge::vitalsReady(QVariantMap) 直连
    void setVitals(const QVariantMap &vitals);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    struct Lane {
        QVector<double> trace;
        QVector<double> ages;      // 「采集 → 本次绘制」延迟样本（ms）
        double lastTsUs = 0.0;
        bool has = false;
        bool wasStale = false;
        double staleMs = 250.0;    // 超过即判「中断」
        QString key;               // 日志用的短名
        QString title;
    };

    static double nowMicros();
    static void recordAge(Lane &lane, double ageMs);
    static double percentile(QVector<double> sorted, double q);

    void push(Lane &lane, double value, double tsUs);
    void drawLane(QPainter &p, const QRectF &rect, Lane &lane, const QColor &color, double gain);
    void paintLaneWatchdog(QPainter &p, const QRectF &rect, Lane &lane, double ageMs);

    Lane m_ecg;
    Lane m_spo2;

    double m_hr = 0.0;
    double m_spo2Value = 0.0;
    double m_sys = 0.0;
    double m_dia = 0.0;
    bool m_hasVitals = false;
    bool m_alarmActive = false;
    qint64 m_paintCount = 0;
};
