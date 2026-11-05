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
constexpr int kInfoWidth = 300;       // 右侧数值/延迟区宽度
constexpr double kStripHeight = 112.0;

const QColor kBg(0x0b, 0x10, 0x17);
const QColor kBorder(0x1e, 0x26, 0x34);
const QColor kGrid(0x1a, 0x23, 0x32);
const QColor kTrace(0x34, 0xd3, 0x99);
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
}

double NativeVitalStrip::nowMicros()
{
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    return static_cast<double>(us);
}

double NativeVitalStrip::percentile(const QVector<double> &sorted, double q)
{
    if (sorted.isEmpty()) return 0.0;
    const qsizetype idx = std::min<qsizetype>(
        sorted.size() - 1,
        static_cast<qsizetype>(std::floor(static_cast<double>(sorted.size()) * q)));
    return sorted.at(std::max<qsizetype>(0, idx));
}

void NativeVitalStrip::recordAge(double ageMs)
{
    m_ages.append(ageMs);
    if (m_ages.size() > kAgeCapacity) {
        m_ages.remove(0, m_ages.size() - kAgeCapacity);
    }
}

void NativeVitalStrip::pushEcgSample(double value, double tsUs)
{
    m_trace.append(value);
    if (m_trace.size() > kTraceCapacity) {
        m_trace.remove(0, m_trace.size() - kTraceCapacity);
    }
    m_lastTsUs = tsUs;
    m_hasSample = true;
    update();
}

void NativeVitalStrip::setVitals(const QVariantMap &vitals)
{
    m_hr = vitals.value(QStringLiteral("hr")).toDouble();
    m_spo2 = vitals.value(QStringLiteral("spo2")).toDouble();
    m_sys = vitals.value(QStringLiteral("sys")).toDouble();
    m_dia = vitals.value(QStringLiteral("dia")).toDouble();
    m_hasVitals = true;
    update();
}

void NativeVitalStrip::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    // 「采集 → 本次绘制」的实测延迟：信号直连，无 IPC。
    if (m_hasSample) {
        recordAge((nowMicros() - m_lastTsUs) / 1000.0);
    }

    QVector<double> ages = m_ages;
    std::sort(ages.begin(), ages.end());

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), kBg);

    const double w = static_cast<double>(width());
    const double h = static_cast<double>(height());
    const double traceW = std::max(10.0, w - static_cast<double>(kInfoWidth));

    // 分隔线 + 边框
    p.setPen(QPen(kBorder, 1.0));
    p.drawLine(QPointF(traceW, 0.0), QPointF(traceW, h));
    p.drawLine(QPointF(0.0, 0.0), QPointF(w, 0.0));

    // ---- 波形区 ----
    p.setFont(QFont(QStringLiteral("Helvetica"), 10));
    p.setPen(kDim);
    p.drawText(QRectF(12.0, 4.0, traceW - 24.0, 18.0),
               Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("ECG · 原生绘制通道（QPainter，不经 WebView）"));

    p.setPen(QPen(kGrid, 1.0));
    for (double x = 12.0; x < traceW - 8.0; x += 16.0) {
        p.drawLine(QPointF(x, 26.0), QPointF(x, h - 8.0));
    }
    for (double y = 30.0; y < h - 8.0; y += 16.0) {
        p.drawLine(QPointF(8.0, y), QPointF(traceW - 8.0, y));
    }

    if (m_trace.size() >= 2) {
        const double left = 12.0;
        const double span = traceW - left - 12.0;
        const double baseline = h * 0.62;
        const double gain = h * 0.42;   // 波形值域约 -0.3..1.2，1.0 对应 0.42h
        QPolygonF poly;
        poly.reserve(m_trace.size());
        const qsizetype n = m_trace.size();
        for (qsizetype i = 0; i < n; ++i) {
            const double x = left + span * (static_cast<double>(i) / static_cast<double>(n - 1));
            const double y = baseline - m_trace.at(i) * gain;
            poly.append(QPointF(x, y));
        }
        p.setPen(QPen(kTrace, 1.6));
        p.drawPolyline(poly);
    } else {
        p.setPen(kDim);
        p.drawText(QRectF(12.0, 26.0, traceW - 24.0, h - 34.0),
                   Qt::AlignCenter,
                   QStringLiteral("等待 ECG 样本…"));
    }

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
                   QStringLiteral("SpO₂ %1 %").arg(m_spo2, 0, 'f', 0));
        p.drawText(QRectF(infoX, 68.0, 200.0, 22.0),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("NIBP %1/%2").arg(m_sys, 0, 'f', 0).arg(m_dia, 0, 'f', 0));
    } else {
        p.setPen(kDim);
        p.drawText(QRectF(infoX, 24.0, 220.0, 22.0),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("等待生命体征…"));
    }

    // 延迟对照：这一行是「原生 vs WebView」的直接证据
    p.setFont(QFont(QStringLiteral("Menlo"), 9));
    p.setPen(kText);
    const double p50 = percentile(ages, 0.5);
    const double p99 = percentile(ages, 0.99);
    p.drawText(QRectF(infoX, 90.0, static_cast<double>(kInfoWidth) - 20.0, 16.0),
               Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("采集→绘制  P50 %1 ms · P99 %2 ms")
                   .arg(p50, 0, 'f', 2)
                   .arg(p99, 0, 'f', 2));

    // 每 ~120 帧（约 2 秒）落一条日志：截图之外也要有可核查的数字
    if (++m_paintCount % 120 == 0) {
        qInfo().noquote() << QStringLiteral("[VitalStrip] 采集→绘制 P50 %1 ms · P99 %2 ms（n=%3）")
                                 .arg(p50, 0, 'f', 2)
                                 .arg(p99, 0, 'f', 2)
                                 .arg(ages.size());
    }

    // 演示级阈值报警：真实设备需要独立报警算法与冗余通道，这里仅示意
    const bool alarm = m_hasVitals && (m_spo2 < 90.0 || m_hr < 50.0);
    if (alarm) {
        p.setPen(kAlarm);
        p.setFont(QFont(QStringLiteral("Helvetica"), 10, QFont::Bold));
        const QString what = m_spo2 < 90.0 ? QStringLiteral("低血氧报警")
                                          : QStringLiteral("心动过缓报警");
        p.drawText(QRectF(infoX, 24.0 + 66.0, 200.0, 20.0), Qt::AlignLeft, what);
        p.setPen(Qt::NoPen);
        p.setBrush(kAlarm);
        p.drawEllipse(QPointF(infoX + 8.0, 12.0), 4.0, 4.0);
    }
}
