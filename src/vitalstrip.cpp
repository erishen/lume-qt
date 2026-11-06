#include "vitalstrip.h"

#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QSoundEffect>
#include <QStandardPaths>
#include <QKeyEvent>
#include <QMouseEvent>
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
const QColor kMidAlarm(0xf2, 0xc9, 0x4c);
const QColor kMuted(0x94, 0x9e, 0xb0);
} // namespace

namespace {

/// 写一个 16-bit 单声道 WAV：pulses 是「响 N ms / 停 M ms」的序列。
/// 报警音自己合成而不是塞二进制素材：音高、节奏都写在代码里，评审时看得见。
bool writeToneWav(const QString &path, int freqHz, const QVector<QPair<int, int>> &pulses)
{
    constexpr int sampleRate = 44100;
    constexpr double amplitude = 0.6;
    QByteArray pcm;
    for (const auto &pulse : pulses) {
        const int onSamples = sampleRate * pulse.first / 1000;
        const int offSamples = sampleRate * pulse.second / 1000;
        for (int i = 0; i < onSamples; ++i) {
            const double t = static_cast<double>(i) / sampleRate;
            const qint16 v = static_cast<qint16>(
                amplitude * 32767.0 * std::sin(2.0 * M_PI * freqHz * t));
            pcm.append(static_cast<char>(v & 0xff));
            pcm.append(static_cast<char>((v >> 8) & 0xff));
        }
        pcm.append(QByteArray(offSamples * 2, '\0'));
    }

    QByteArray wav;
    auto appendU32 = [&wav](quint32 v) {
        wav.append(static_cast<char>(v & 0xff));
        wav.append(static_cast<char>((v >> 8) & 0xff));
        wav.append(static_cast<char>((v >> 16) & 0xff));
        wav.append(static_cast<char>((v >> 24) & 0xff));
    };
    auto appendU16 = [&wav](quint16 v) {
        wav.append(static_cast<char>(v & 0xff));
        wav.append(static_cast<char>((v >> 8) & 0xff));
    };
    wav.append("RIFF");
    appendU32(static_cast<quint32>(36 + pcm.size()));
    wav.append("WAVEfmt ");
    appendU32(16);
    appendU16(1);            // PCM
    appendU16(1);            // 单声道
    appendU32(sampleRate);
    appendU32(sampleRate * 2);
    appendU16(2);            // 每帧字节
    appendU16(16);           // 位深
    wav.append("data");
    appendU32(static_cast<quint32>(pcm.size()));
    wav.append(pcm);

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    return f.write(wav) == wav.size();
}

QString toneFilePath(int level)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                        + QStringLiteral("/sounds");
    QDir().mkpath(dir);
    return dir + (level == 2 ? QStringLiteral("/alarm-high.wav")
                             : QStringLiteral("/alarm-mid.wav"));
}

} // namespace

NativeVitalStrip::NativeVitalStrip(QWidget *parent)
    : QWidget(parent)
{
    setFixedHeight(static_cast<int>(kStripHeight));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    // 静音要能用键盘触发：术中手套上手，快捷键比精确点按可靠。
    setFocusPolicy(Qt::StrongFocus);

    m_ecg.key = QStringLiteral("ECG");
    m_ecg.title = QStringLiteral("ECG · 原生绘制通道（QPainter，不经 WebView）");
    m_ecg.staleMs = 250.0;

    m_spo2.key = QStringLiteral("SpO2");
    m_spo2.title = QStringLiteral("SpO₂ 体积描记波 · 原生判定（不经 WebView）");
    m_spo2.staleMs = 400.0;

    // 声音默认开：报警系统没有声音说不过去；开发时可用 QTRS_ALARM_SOUND=0 关掉。
    m_soundEnabled = qEnvironmentVariable("QTRS_ALARM_SOUND") != QStringLiteral("0");

    if (m_soundEnabled) {
        // 高：三连短响（120 ms 响 / 60 ms 停）；中：两声长响（220 ms / 140 ms）。
        // 节奏差异比音高差异更容易在嘈杂的手术室里被分辨出来。
        const QString highPath = toneFilePath(2);
        const QString midPath = toneFilePath(1);
        if (!QFile::exists(highPath) || QFile(highPath).size() == 0) {
            writeToneWav(highPath, 1000,
                         {{120, 60}, {120, 60}, {120, 260}});
        }
        if (!QFile::exists(midPath) || QFile(midPath).size() == 0) {
            writeToneWav(midPath, 620, {{220, 140}, {220, 300}});
        }
        m_highTone = std::make_unique<QSoundEffect>();
        m_highTone->setSource(QUrl::fromLocalFile(highPath));
        m_highTone->setVolume(0.9f);
        m_highTone->setLoopCount(1);
        m_midTone = std::make_unique<QSoundEffect>();
        m_midTone->setSource(QUrl::fromLocalFile(midPath));
        m_midTone->setVolume(0.9f);
        m_midTone->setLoopCount(1);
    }

    bool ok = false;
    const int sec = qEnvironmentVariable("QTRS_ALARM_MUTE_SEC").toInt(&ok);
    if (ok) m_defaultMuteSeconds = std::clamp(sec, 5, 300);
}

NativeVitalStrip::~NativeVitalStrip() = default;

void NativeVitalStrip::setAlarmThresholds(const AlarmThresholds &thresholds)
{
    m_thresholds = thresholds;
    update();
}

bool NativeVitalStrip::alarmsMuted() const
{
    return m_muteTimer.isValid() && m_muteSeconds > 0
        && m_muteTimer.elapsed() < static_cast<qint64>(m_muteSeconds) * 1000;
}

int NativeVitalStrip::alarmLevel() const
{
    return m_alarmLevel;
}

int NativeVitalStrip::defaultMuteSeconds() const
{
    return m_defaultMuteSeconds;
}

int NativeVitalStrip::muteRemainingMs() const
{
    if (!alarmsMuted()) return 0;
    const qint64 left = static_cast<qint64>(m_muteSeconds) * 1000 - m_muteTimer.elapsed();
    return static_cast<int>(std::max<qint64>(0, left));
}

bool NativeVitalStrip::muteAlarms(int seconds)
{
    if (seconds <= 0) {
        if (m_muteTimer.isValid()) {
            m_muteTimer.invalidate();
            emit auditEvent(QVariantMap{
                {QStringLiteral("type"), QStringLiteral("alarm.unmute")},
                {QStringLiteral("reason"), QStringLiteral("manual")},
                {QStringLiteral("source"), QStringLiteral("native-strip")},
            });
        }
        update();
        return false;
    }
    // 上限 5 分钟：静音是「暂时压掉提示」，不是关掉报警系统。
    m_muteSeconds = std::min(seconds, 300);
    m_muteTimer.start();
    m_muteExpiryLogged = false;
    emit auditEvent(QVariantMap{
        {QStringLiteral("type"), QStringLiteral("alarm.mute")},
        {QStringLiteral("seconds"), m_muteSeconds},
        {QStringLiteral("alarmLevel"), m_alarmLevel},
        {QStringLiteral("hr"), m_hr},
        {QStringLiteral("spo2"), m_spo2Value},
        {QStringLiteral("source"), QStringLiteral("native-strip")},
    });
    update();
    return true;
}

void NativeVitalStrip::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        // 点一下整个区域即可静音/解除：报警时手上没空去对准某个小按钮。
        muteAlarms(alarmsMuted() ? 0 : defaultMuteSeconds());
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void NativeVitalStrip::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_M) {
        muteAlarms(alarmsMuted() ? 0 : defaultMuteSeconds());
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        muteAlarms(0);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
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

    // 报警判定在原生侧完成：不依赖前端是否渲染成功。
    // 两级：高（SpO₂<spo2High 或 HR<hrHigh）/ 中（SpO₂<spo2Mid 或 HR<hrMid）。
    // 阈值来自 AlarmThresholds（默认与改造前一致），可由 env/Lume 覆盖；演示级口径，不做医学声明。
    const int level = alarmLevelFor(m_spo2Value, m_hr, m_hasVitals, m_thresholds);
    const bool highAlarm = level == 2;
    if (level != m_alarmLevel) {
        // 级别边沿记入台账：什么时候报的、哪一级、依据哪个值、当时用的是哪套阈值，事后能复核
        const bool spo2Culprit = highAlarm ? (m_spo2Value < m_thresholds.spo2High)
                                           : (m_spo2Value < m_thresholds.spo2Mid);
        m_alarmLevel = level;
        emit auditEvent(QVariantMap{
            {QStringLiteral("type"), level > 0 ? QStringLiteral("alarm.on") : QStringLiteral("alarm.off")},
            {QStringLiteral("priority"), level == 2 ? QStringLiteral("high")
                                                    : level == 1 ? QStringLiteral("medium")
                                                                 : QStringLiteral("none")},
            {QStringLiteral("channel"), level > 0 ? (spo2Culprit ? QStringLiteral("SpO2")
                                                                 : QStringLiteral("HR"))
                                                  : QStringLiteral("all")},
            {QStringLiteral("hr"), m_hr},
            {QStringLiteral("spo2"), m_spo2Value},
            {QStringLiteral("source"), QStringLiteral("native-strip")},
            {QStringLiteral("spo2High"), m_thresholds.spo2High},
            {QStringLiteral("spo2Mid"), m_thresholds.spo2Mid},
            {QStringLiteral("hrHigh"), m_thresholds.hrHigh},
            {QStringLiteral("hrMid"), m_thresholds.hrMid},
        });
    }
    if (m_alarmLevel > 0) {
        const bool muted = alarmsMuted();
        // 静音到期：提示恢复成红色，同时记一条「到期解除」的台账，
        // 免得事后只看到 mute 看不到是谁、什么时候把提示放回来的。
        if (!muted && m_muteTimer.isValid() && !m_muteExpiryLogged) {
            m_muteExpiryLogged = true;
            emit auditEvent(QVariantMap{
                {QStringLiteral("type"), QStringLiteral("alarm.unmute")},
                {QStringLiteral("reason"), QStringLiteral("expired")},
                {QStringLiteral("seconds"), m_muteSeconds},
                {QStringLiteral("source"), QStringLiteral("native-strip")},
            });
        }
        const QColor tone = muted ? kMuted : (m_alarmLevel == 2 ? kAlarm : kMidAlarm);
        p.setPen(tone);
        p.setFont(QFont(QStringLiteral("Helvetica"), 10, QFont::Bold));
        const QString prefix = m_alarmLevel == 2 ? QStringLiteral("高优先级") : QStringLiteral("中优先级");
        // 文案里的「是哪一项触发」必须跟判定用同一套阈值：否则会出现
        // 「中优先级 · 心率偏低 75 bpm」这种自相矛盾的提示（实测踩到）。
        const double spo2Limit = (m_alarmLevel == 2) ? m_thresholds.spo2High : m_thresholds.spo2Mid;
        const QString what = (m_spo2Value < spo2Limit)
                                 ? QStringLiteral("血氧偏低 %1%").arg(m_spo2Value, 0, 'f', 0)
                                 : QStringLiteral("心率偏低 %1 bpm").arg(m_hr, 0, 'f', 0);
        const QString label = muted
            ? QStringLiteral("%1 · %2 · 已静音 %3 s")
                  .arg(prefix, what).arg(muteRemainingMs() / 1000)
            : QStringLiteral("%1 · %2").arg(prefix, what);
        p.drawText(QRectF(infoX, 132.0, 200.0, 20.0), Qt::AlignLeft, label);
        p.setPen(Qt::NoPen);
        p.setBrush(tone);
        p.drawEllipse(QPointF(infoX + 8.0, 12.0), 4.0, 4.0);

        // 声音：高优先级 3 s 复鸣、中优先级 8 s；静音期间不响（提示被压掉，判定仍在）。
        const int intervalMs = m_alarmLevel == 2 ? 3000 : 8000;
        if (m_soundEnabled && !muted
            && (!m_beepTimer.isValid() || m_beepTimer.elapsed() >= intervalMs)) {
            m_beepTimer.start();
            QSoundEffect *toneEffect = (m_alarmLevel == 2 ? m_highTone : m_midTone).get();
            if (toneEffect != nullptr && toneEffect->status() != QSoundEffect::Error) {
                toneEffect->play();
                // 把音源状态一起打出来：Ready 说明 WAV 已被解码器接受，
                // 而不是「调用过 play() 就算响过了」。
                qInfo().noquote()
                    << QStringLiteral("[Alarm] tone level=%1 status=%2 hr=%3 spo2=%4")
                           .arg(m_alarmLevel)
                           .arg(static_cast<int>(toneEffect->status()))
                           .arg(m_hr, 0, 'f', 0)
                           .arg(m_spo2Value, 0, 'f', 0);
            } else {
                // 音频设备不可用 / WAV 加载失败时退回系统提示音：
                // 声音质量降级，但「有报警」这件事不能因此消失。
                QApplication::beep();
                if (!m_toneFallbackWarned) {
                    m_toneFallbackWarned = true;
                    qWarning() << "[Alarm] 报警音加载失败，退回系统提示音";
                }
            }
        }
    }

    // 整条边框随报警级别变色：红=高、黄=中、灰虚线=已静音。
    // 不依赖文字被看见，也不依赖前端是否渲染成功。
    if (m_alarmLevel > 0) {
        const bool muted = alarmsMuted();
        const QColor tone = muted ? kMuted : (m_alarmLevel == 2 ? kAlarm : kMidAlarm);
        p.setPen(QPen(tone, 3.0, muted ? Qt::DashLine : Qt::SolidLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5));
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
        QVariantMap ev;
        ev.insert(QStringLiteral("channel"), lane.key);
        ev.insert(QStringLiteral("ageMs"), ageMs);
        ev.insert(QStringLiteral("thresholdMs"), lane.staleMs);
        ev.insert(QStringLiteral("source"), QStringLiteral("native-strip"));
        if (stale) {
            qWarning().noquote() << QStringLiteral(
                                        "[VitalStrip] %1 信号中断：%2 ms 无样本（画面为冻结帧）")
                                        .arg(lane.key)
                                        .arg(ageMs, 0, 'f', 0);
            ev.insert(QStringLiteral("type"), QStringLiteral("signal.stale"));
            emit auditEvent(ev);
        } else {
            qInfo().noquote() << QStringLiteral("[VitalStrip] %1 信号恢复").arg(lane.key);
            ev.insert(QStringLiteral("type"), QStringLiteral("signal.recover"));
            emit auditEvent(ev);
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
