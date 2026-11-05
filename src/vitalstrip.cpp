#include "vitalstrip.h"

#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPolygonF>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {
constexpr int kTraceCapacity = 600;   // 40ms 采样 → 24 秒窗口
constexpr int kAgeCapacity = 240;     // 约 10 秒的延迟样本
constexpr int kDeltaCapacity = 240;   // 估时钟偏移用的原始差值窗口（约 10 秒样本）
constexpr int kInfoWidth = 300;       // 右侧数值/延迟区宽度
constexpr double kStripHeight = 176.0;
constexpr double kLaneHeader = 18.0;

const QColor kBg(0x0b, 0x10, 0x17);
const QColor kBorder(0x1e, 0x26, 0x34);
const QColor kGrid(0x1a, 0x23, 0x32);
const QColor kTraceEcg(0x34, 0xd3, 0x99);
const QColor kTraceSpo2(0x60, 0xa5, 0xfa);
const QColor kText(0xa9, 0xb4, 0xd0);
const QColor kDim(0x5c, 0x66, 0x78);
const QColor kAlarm(0xf8, 0x71, 0x71);
} // namespace

NativeVitalStrip::NativeVitalStrip(QWidget *parent)
    : QWidget(parent)
{
    setFixedHeight(static_cast<int>(kStripHeight));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAttribute(Qt::WA_OpaquePaintEvent, true);

    m_ecg.key = QStringLiteral("ECG");
    m_ecg.title = QStringLiteral("ECG · 原生绘制通道（QPainter，不经 WebView）");
    m_ecg.staleMs = 250.0;

    m_spo2.key = QStringLiteral("SpO2");
    m_spo2.title = QStringLiteral("SpO₂ 体积描记波 · 原生判定（不经 WebView）");
    m_spo2.staleMs = 400.0;
}

double NativeVitalStrip::nowMicros()
{
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    return static_cast<double>(us);
}

double NativeVitalStrip::percentile(QVector<double> sorted, double q)
{
    if (sorted.isEmpty()) return 0.0;
    const qsizetype idx = std::min<qsizetype>(
        sorted.size() - 1,
        static_cast<qsizetype>(std::floor(static_cast<double>(sorted.size()) * q)));
    return sorted.at(std::max<qsizetype>(0, idx));
}

double NativeVitalStrip::residualUs(Lane &lane, double rawUs)
{
    // 时间戳可能来自设备时钟（与宿主有未知常量偏移）。最近窗口内的最小差值
    // 就是偏移的下界估计，扣掉它剩下的才是链路延迟。断流时没有新样本进窗口，
    // 偏移估计不会漂移，中断判定仍然成立。
    lane.deltas.append(rawUs);
    if (lane.deltas.size() > kDeltaCapacity) {
        lane.deltas.remove(0, lane.deltas.size() - kDeltaCapacity);
    }
    double min = lane.deltas.at(0);
    for (double v : lane.deltas) {
        if (v < min) min = v;
    }
    return rawUs - min;
}

void NativeVitalStrip::recordAge(Lane &lane, double ageMs)
{
    lane.ages.append(ageMs);
    if (lane.ages.size() > kAgeCapacity) {
        lane.ages.remove(0, lane.ages.size() - kAgeCapacity);
    }
}

void NativeVitalStrip::push(Lane &lane, double value, double tsUs)
{
    lane.trace.append(value);
    if (lane.trace.size() > kTraceCapacity) {
        lane.trace.remove(0, lane.trace.size() - kTraceCapacity);
    }
    lane.lastTsUs = tsUs;
    const double receiveUs = nowMicros();
    lane.lastReceiptUs = receiveUs;
    lane.lastResidualUs = residualUs(lane, receiveUs - tsUs);
    lane.has = true;
    update();
}

void NativeVitalStrip::pushEcgSample(double value, double tsUs)
{
    push(m_ecg, value, tsUs);
}

void NativeVitalStrip::pushSpo2Sample(double value, double tsUs)
{
    push(m_spo2, value, tsUs);
}

void NativeVitalStrip::setVitals(const QVariantMap &vitals)
{
    m_hr = vitals.value(QStringLiteral("hr")).toDouble();
    m_spo2Value = vitals.value(QStringLiteral("spo2")).toDouble();
    m_sys = vitals.value(QStringLiteral("sys")).toDouble();
    m_dia = vitals.value(QStringLiteral("dia")).toDouble();
    m_hasVitals = true;
    update();
}

void NativeVitalStrip::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    const double now = nowMicros();
    // 「采集 → 本次绘制」的实测延迟：信号直连，无 IPC。
    // 拆成「到达时的残差」+「到达后到本帧的排队时间」，这样与时间戳来源无关：
    // 时间戳是设备时钟、与宿主有常量偏移时，残差已把偏移扣掉，不会出现负值或虚高。
    const double ecgAgeMs = m_ecg.has
        ? (now - m_ecg.lastReceiptUs + m_ecg.lastResidualUs) / 1000.0 : -1.0;
    const double spo2AgeMs = m_spo2.has
        ? (now - m_spo2.lastReceiptUs + m_spo2.lastResidualUs) / 1000.0 : -1.0;
    if (m_ecg.has) recordAge(m_ecg, ecgAgeMs);
    if (m_spo2.has) recordAge(m_spo2, spo2AgeMs);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), kBg);

    const double w = static_cast<double>(width());
    const double h = static_cast<double>(height());
    const double traceW = std::max(10.0, w - static_cast<double>(kInfoWidth));
    const double laneH = (h - 1.0) / 2.0;

    // 分隔线 + 边框
    p.setPen(QPen(kBorder, 1.0));
    p.drawLine(QPointF(traceW, 0.0), QPointF(traceW, h));
    p.drawLine(QPointF(0.0, 0.0), QPointF(w, 0.0));
    p.drawLine(QPointF(0.0, laneH), QPointF(traceW, laneH));

    drawLane(p, QRectF(0.0, 0.0, traceW, laneH), m_ecg, kTraceEcg, 0.42);
    drawLane(p, QRectF(0.0, laneH + 1.0, traceW, laneH), m_spo2, kTraceSpo2, 0.40);

    // ---- 原生看门狗：通道自己判断「信号中断」，不依赖前端 ----
    paintLaneWatchdog(p, QRectF(0.0, 0.0, traceW, laneH), m_ecg, ecgAgeMs);
    paintLaneWatchdog(p, QRectF(0.0, laneH + 1.0, traceW, laneH), m_spo2, spo2AgeMs);

    // ---- 右侧信息区 ----
    const double infoX = traceW + 14.0;
    p.setFont(QFont(QStringLiteral("Helvetica"), 10));
    p.setPen(kDim);
    p.drawText(QRectF(infoX, 4.0, static_cast<double>(kInfoWidth) - 20.0, 18.0),
               Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("生命体征 · 原生通道"));

    p.setFont(QFont(QStringLiteral("Helvetica"), 11));
    if (m_hasVitals) {
        p.setPen(kText);
        p.drawText(QRectF(infoX, 24.0, 180.0, 22.0),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("HR %1 bpm").arg(m_hr, 0, 'f', 0));
        p.drawText(QRectF(infoX, 46.0, 180.0, 22.0),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("SpO₂ %1 %").arg(m_spo2Value, 0, 'f', 0));
        p.drawText(QRectF(infoX, 68.0, 200.0, 22.0),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("NIBP %1/%2").arg(m_sys, 0, 'f', 0).arg(m_dia, 0, 'f', 0));
    } else {
        p.setPen(kDim);
        p.drawText(QRectF(infoX, 24.0, 220.0, 22.0),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("等待生命体征…"));
    }

    // 延迟对照：两行是「原生 vs WebView」的直接证据
    QVector<double> ecgAges = m_ecg.ages;
    QVector<double> spo2Ages = m_spo2.ages;
    std::sort(ecgAges.begin(), ecgAges.end());
    std::sort(spo2Ages.begin(), spo2Ages.end());
    const double ecgP50 = percentile(ecgAges, 0.5);
    const double ecgP99 = percentile(ecgAges, 0.99);
    const double spo2P50 = percentile(spo2Ages, 0.5);
    const double spo2P99 = percentile(spo2Ages, 0.99);

    p.setFont(QFont(QStringLiteral("Menlo"), 9));
    p.setPen(kText);
    p.drawText(QRectF(infoX, 94.0, static_cast<double>(kInfoWidth) - 20.0, 16.0),
               Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("ECG 采集→绘制  P50 %1 · P99 %2 ms")
                   .arg(ecgP50, 0, 'f', 2)
                   .arg(ecgP99, 0, 'f', 2));
    p.drawText(QRectF(infoX, 110.0, static_cast<double>(kInfoWidth) - 20.0, 16.0),
               Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("SpO₂ 采集→绘制  P50 %1 · P99 %2 ms")
                   .arg(spo2P50, 0, 'f', 2)
                   .arg(spo2P99, 0, 'f', 2));

    // 报警判定在原生侧完成：不依赖前端是否渲染成功
    const bool alarm = m_hasVitals && (m_spo2Value < 90.0 || m_hr < 50.0);
    if (alarm) {
        p.setPen(kAlarm);
        p.setFont(QFont(QStringLiteral("Helvetica"), 10, QFont::Bold));
        const QString what = m_spo2Value < 90.0 ? QStringLiteral("低血氧报警")
                                                : QStringLiteral("心动过缓报警");
        p.drawText(QRectF(infoX, 132.0, 200.0, 20.0), Qt::AlignLeft, what);
        p.setPen(Qt::NoPen);
        p.setBrush(kAlarm);
        p.drawEllipse(QPointF(infoX + 8.0, 12.0), 4.0, 4.0);
    }

    // 每 ~120 帧（约 2 秒）落一条日志：截图之外也要有可核查的数字
    if (++m_paintCount % 120 == 0) {
        qInfo().noquote() << QStringLiteral(
                                 "[VitalStrip] ECG P50 %1 P99 %2 ms · SpO2 P50 %3 P99 %4 ms"
                                 "（ECG n=%5 / SpO2 n=%6）")
                                 .arg(ecgP50, 0, 'f', 2)
                                 .arg(ecgP99, 0, 'f', 2)
                                 .arg(spo2P50, 0, 'f', 2)
                                 .arg(spo2P99, 0, 'f', 2)
                                 .arg(ecgAges.size())
                                 .arg(spo2Ages.size());
    }
}

void NativeVitalStrip::drawLane(QPainter &p, const QRectF &rect, Lane &lane,
                                const QColor &color, double gain)
{
    const double traceW = rect.width();
    const double laneH = rect.height();

    p.setFont(QFont(QStringLiteral("Helvetica"), 10));
    p.setPen(kDim);
    p.drawText(QRectF(rect.left() + 12.0, rect.top() + 2.0, traceW - 24.0, kLaneHeader),
               Qt::AlignLeft | Qt::AlignVCenter,
               lane.title);

    // 网格
    p.setPen(QPen(kGrid, 1.0));
    for (double x = 12.0; x < traceW - 8.0; x += 16.0) {
        p.drawLine(QPointF(rect.left() + x, rect.top() + kLaneHeader + 4.0),
                   QPointF(rect.left() + x, rect.bottom() - 6.0));
    }
    for (double y = rect.top() + kLaneHeader + 8.0; y < rect.bottom() - 6.0; y += 16.0) {
        p.drawLine(QPointF(8.0, y), QPointF(traceW - 8.0, y));
    }

    if (lane.trace.size() >= 2) {
        const double left = 12.0;
        const double span = traceW - left - 12.0;
        const double baseline = rect.top() + laneH * 0.66;
        const double amplitude = laneH * gain;
        QPolygonF poly;
        poly.reserve(lane.trace.size());
        const qsizetype n = lane.trace.size();
        for (qsizetype i = 0; i < n; ++i) {
            const double x = left + span * (static_cast<double>(i) / static_cast<double>(n - 1));
            const double y = baseline - lane.trace.at(i) * amplitude;
            poly.append(QPointF(x, y));
        }
        p.setPen(QPen(color, 1.6));
        p.drawPolyline(poly);
    } else {
        p.setPen(kDim);
        p.drawText(QRectF(rect.left() + 12.0, rect.top() + kLaneHeader,
                          traceW - 24.0, laneH - kLaneHeader - 8.0),
                   Qt::AlignCenter,
                   QStringLiteral("等待 %1 样本…").arg(lane.key));
    }
}

void NativeVitalStrip::paintLaneWatchdog(QPainter &p, const QRectF &rect, Lane &lane,
                                        double ageMs)
{
    if (!lane.has || ageMs < 0.0) return;

    const bool stale = ageMs > lane.staleMs;
    if (stale != lane.wasStale) {
        lane.wasStale = stale;
        if (stale) {
            qWarning().noquote() << QStringLiteral(
                                        "[VitalStrip] %1 信号中断：%2 ms 无样本（画面为冻结帧）")
                                        .arg(lane.key)
                                        .arg(ageMs, 0, 'f', 0);
        } else {
            qInfo().noquote() << QStringLiteral("[VitalStrip] %1 信号恢复").arg(lane.key);
        }
    }
    if (!stale) return;

    // 冻结帧标记：宁可画面变丑，也不能让「旧波形」看起来是实时的
    const QRectF box(rect.left() + 12.0, rect.center().y() - 11.0, 236.0, 22.0);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xf8, 0x71, 0x71, 60));
    p.drawRoundedRect(box, 4.0, 4.0);
    p.setPen(kAlarm);
    p.setFont(QFont(QStringLiteral("Helvetica"), 10, QFont::Bold));
    p.drawText(box, Qt::AlignCenter,
               QStringLiteral("信号中断 %1 ms · 冻结帧").arg(ageMs, 0, 'f', 0));
}
