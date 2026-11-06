#include "alarmconfig.h"

#include <QJsonValue>
#include <QtGlobal>

namespace {

// 阈值允许区间的意义：SpO₂ 低于 50 会让严重低氧漏报；HR 低于 20 同理。
// 上限则防「把阈值调到 100 永远报警」这类配置把界面变成常红噪音。
bool readDouble(const QJsonObject &json, const QString &key, double *out, QStringList *warnings)
{
    if (!json.contains(key))
        return false;
    const QJsonValue value = json.value(key);
    if (!value.isDouble()) {
        if (warnings)
            warnings->append(QStringLiteral("%1 不是数字，已忽略").arg(key));
        return false;
    }
    *out = value.toDouble();
    return true;
}

bool acceptSpo2(double value, const QString &key, QStringList *warnings)
{
    if (value < 50.0 || value > 100.0) {
        if (warnings)
            warnings->append(QStringLiteral("%1=%2 超出 [50,100]，已忽略").arg(key).arg(value));
        return false;
    }
    return true;
}

bool acceptHr(double value, const QString &key, QStringList *warnings)
{
    if (value < 20.0 || value > 200.0) {
        if (warnings)
            warnings->append(QStringLiteral("%1=%2 超出 [20,200]，已忽略").arg(key).arg(value));
        return false;
    }
    return true;
}

} // namespace

AlarmThresholds defaultAlarmThresholds()
{
    return AlarmThresholds{};
}

void applyAlarmEnv(AlarmThresholds &thresholds, QStringList *warnings)
{
    struct Item
    {
        const char *env;
        double *target;
        bool spo2;
    };
    const Item items[] = {
        {"QTRS_ALARM_SPO2_HIGH", &thresholds.spo2High, true},
        {"QTRS_ALARM_SPO2_MID", &thresholds.spo2Mid, true},
        {"QTRS_ALARM_HR_HIGH", &thresholds.hrHigh, false},
        {"QTRS_ALARM_HR_MID", &thresholds.hrMid, false},
    };

    for (const Item &item : items) {
        const QByteArray raw = qgetenv(item.env);
        if (raw.isEmpty())
            continue;
        bool ok = false;
        const double value = QString::fromLatin1(raw).toDouble(&ok);
        if (!ok) {
            if (warnings)
                warnings->append(QStringLiteral("%1=%2 不是数字，已忽略")
                                     .arg(QString::fromLatin1(item.env), QString::fromLatin1(raw)));
            continue;
        }
        const bool accepted = item.spo2 ? acceptSpo2(value, QString::fromLatin1(item.env), warnings)
                                        : acceptHr(value, QString::fromLatin1(item.env), warnings);
        if (accepted)
            *item.target = value;
    }

    // 中优先级必须不严于高优先级，否则「中」永远先触发，分级失去意义。
    if (thresholds.spo2Mid < thresholds.spo2High) {
        if (warnings)
            warnings->append(QStringLiteral("spo2Mid=%1 < spo2High=%2，中优先级已对齐到高优先级")
                                 .arg(thresholds.spo2Mid)
                                 .arg(thresholds.spo2High));
        thresholds.spo2Mid = thresholds.spo2High;
    }
    if (thresholds.hrMid < thresholds.hrHigh) {
        if (warnings)
            warnings->append(QStringLiteral("hrMid=%1 < hrHigh=%2，中优先级已对齐到高优先级")
                                 .arg(thresholds.hrMid)
                                 .arg(thresholds.hrHigh));
        thresholds.hrMid = thresholds.hrHigh;
    }
}

AlarmThresholds alarmThresholdsFromJson(const QJsonObject &json,
                                        const AlarmThresholds &fallback,
                                        QStringList *warnings)
{
    AlarmThresholds out = fallback;

    double value = 0.0;
    if (readDouble(json, QStringLiteral("spo2High"), &value, warnings)
        && acceptSpo2(value, QStringLiteral("spo2High"), warnings)) {
        out.spo2High = value;
    }
    if (readDouble(json, QStringLiteral("spo2Mid"), &value, warnings)
        && acceptSpo2(value, QStringLiteral("spo2Mid"), warnings)) {
        out.spo2Mid = value;
    }
    if (readDouble(json, QStringLiteral("hrHigh"), &value, warnings)
        && acceptHr(value, QStringLiteral("hrHigh"), warnings)) {
        out.hrHigh = value;
    }
    if (readDouble(json, QStringLiteral("hrMid"), &value, warnings)
        && acceptHr(value, QStringLiteral("hrMid"), warnings)) {
        out.hrMid = value;
    }

    if (out.spo2Mid < out.spo2High) {
        if (warnings)
            warnings->append(QStringLiteral("spo2Mid < spo2High，中优先级已对齐"));
        out.spo2Mid = out.spo2High;
    }
    if (out.hrMid < out.hrHigh) {
        if (warnings)
            warnings->append(QStringLiteral("hrMid < hrHigh，中优先级已对齐"));
        out.hrMid = out.hrHigh;
    }
    return out;
}

int alarmLevelFor(double spo2, double hr, bool hasVitals, const AlarmThresholds &thresholds)
{
    if (!hasVitals)
        return 0; // 没有数据不等于正常，只是「无法判定」；由断流 watchdog 负责提示
    if (spo2 < thresholds.spo2High || hr < thresholds.hrHigh)
        return 2;
    if (spo2 < thresholds.spo2Mid || hr < thresholds.hrMid)
        return 1;
    return 0;
}
