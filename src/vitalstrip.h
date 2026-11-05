#pragma once

#include <QColor>
#include <QElapsedTimer>
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

signals:
    /// 结构化审计事件（中断/恢复/报警触发与解除）。字段含 type/channel/detail，
    /// 由 MainWindow 转交 AuditSink 落盘；控件本身不直接碰文件。
    void auditEvent(const QVariantMap &event);

public:
    /// 静音报警提示（边框与文字降为琥珀色），seconds 秒后自动恢复；传 0 立即解除。
    /// 静音只改变「提示方式」，不改变判定，也不隐藏当前数值——这是报警系统的最低要求。
    bool muteAlarms(int seconds);
    bool alarmsMuted() const;
    int muteRemainingMs() const;
    /// 点击/快捷键触发的默认静音时长（`QTRS_ALARM_MUTE_SEC`，默认 120 s，限 5–300）
    int defaultMuteSeconds() const;

public slots:
    /// 与 Bridge::ecgSample(double, double) 直连（不经过 QWebChannel）
    void pushEcgSample(double value, double tsUs);
    /// 与 Bridge::spo2Sample(double, double) 直连（不经过 QWebChannel）
    void pushSpo2Sample(double value, double tsUs);
    /// 与 Bridge::vitalsReady(QVariantMap) 直连
    void setVitals(const QVariantMap &vitals);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    struct Lane {
        QVector<double> trace;
        QVector<double> ages;      // 「采集 → 本次绘制」延迟样本（ms）
        double lastTsUs = 0.0;
        /// 最近一个样本到达本进程的宿主时刻（epoch 微秒）。
        /// 年龄 = (now − lastReceiptUs) + lastResidualUs，与时间戳来自哪个时钟无关。
        double lastReceiptUs = 0.0;
        /// 样本到达时的「扣掉时钟偏移后的延迟」（微秒）
        double lastResidualUs = 0.0;
        /// 最近若干「宿主时刻 − 采样时刻」原始差值，用于估计设备时钟偏移
        QVector<double> deltas;
        bool has = false;
        bool wasStale = false;
        double staleMs = 250.0;    // 超过即判「中断」
        QString key;               // 日志用的短名
        QString title;
    };

    /// 单侧测量只能得到「时钟偏移 + 延迟」之和：用最近窗口的最小差值作偏移下界估计，
    /// 残差即链路延迟。时间戳来自宿主墙钟时偏移≈0，同样适用。
    static double residualUs(Lane &lane, double rawUs);
    static double nowMicros();
    static void recordAge(Lane &lane, double ageMs);
    static double percentile(QVector<double> sorted, double q);

    void push(Lane &lane, double value, double tsUs);
    void drawLane(QPainter &p, const QRectF &rect, Lane &lane, const QColor &color, double gain);
    void paintLaneWatchdog(QPainter &p, const QRectF &rect, Lane &lane, double ageMs);

    Lane m_ecg;
    Lane m_spo2;

    /// 报警边沿检测：只在变化时产生审计事件，避免每帧刷台账
    bool m_alarmActive = false;
    /// 静音计时：到期自动恢复，且到期本身也记一条台账（谁把提示压掉了、压了多久）
    QElapsedTimer m_muteTimer;
    int m_muteSeconds = 0;
    bool m_muteExpiryLogged = false;
    int m_defaultMuteSeconds = 120;
    double m_hr = 0.0;
    double m_spo2Value = 0.0;
    double m_sys = 0.0;
    double m_dia = 0.0;
    bool m_hasVitals = false;
    qint64 m_paintCount = 0;
};
