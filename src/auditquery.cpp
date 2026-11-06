#include "auditquery.h"

#include <QJsonDocument>
#include <QJsonValue>

namespace {

/// 事件里的「文本部分」：type + 字符串/布尔字段。用于子串匹配（"sig" 能命中 signal.stall）。
QString matchableText(const QJsonObject &event)
{
    QString out = event.value(QStringLiteral("type")).toString().toLower();
    for (auto it = event.begin(); it != event.end(); ++it) {
        // ts/tsUs 等微秒时间戳不参与搜索，否则会淹没真正的数值命中。
        if (it.key() == QStringLiteral("ts") || it.key().endsWith(QStringLiteral("Us"))) continue;
        switch (it.value().type()) {
        case QJsonValue::Bool:
            out += QLatin1Char(' ') + QString(it.value().toBool() ? QStringLiteral("true")
                                                                 : QStringLiteral("false"));
            break;
        case QJsonValue::String:
            out += QLatin1Char(' ') + it.value().toString();
            break;
        case QJsonValue::Array:
        case QJsonValue::Object:
            out += QLatin1Char(' ')
                   + QString::fromUtf8(QJsonDocument(it.value().toObject()).toJson(QJsonDocument::Compact));
            break;
        default:
            break;
        }
    }
    return out.toLower();
}

/// 数值按「整值」参与匹配：搜 93 命中 spo2=93，不会命中 ageMs=393（子串会把两者混在一起）。
QStringList numericTokens(const QJsonObject &event)
{
    QStringList out;
    for (auto it = event.begin(); it != event.end(); ++it) {
        if (it.key().endsWith(QStringLiteral("Us"))) continue;
        if (it.value().type() != QJsonValue::Double) continue;
        const double d = it.value().toDouble();
        if (d == static_cast<double>(static_cast<qint64>(d))) out << QString::number(static_cast<qint64>(d));
        else out << QString::number(d);
    }
    return out;
}

bool looksNumeric(const QString &text)
{
    if (text.isEmpty()) return false;
    bool ok = false;
    text.toDouble(&ok);
    return ok;
}

} // namespace

int clampAuditLimit(int limit)
{
    if (limit <= 0) return AuditFilter::kDefaultLimit;
    if (limit > AuditFilter::kMaxLimit) return AuditFilter::kMaxLimit;
    return limit;
}

bool auditEventMatches(const QJsonObject &event, const AuditFilter &filter)
{
    if (!filter.typePrefix.isEmpty()
        && !event.value(QStringLiteral("type")).toString().startsWith(filter.typePrefix)) {
        return false;
    }
    if (filter.text.isEmpty()) return true;
    // 纯数字只比对数值字段（整值相等）：否则会话 id「…-93905」、ageMs=393 都会命中「93」，
    // 把真正的 SpO₂ 93% 淹掉。文字检索仍走子串，方便只记得半个类型名（"sig"）。
    if (looksNumeric(filter.text)) {
        bool ok = false;
        const double wanted = filter.text.toDouble(&ok);
        if (!ok) return false;
        for (const QString &token : numericTokens(event)) {
            if (token.toDouble() == wanted) return true;
        }
        return false;
    }
    return matchableText(event).contains(filter.text.toLower());
}

QJsonArray filterAuditEvents(const QJsonArray &events, const AuditFilter &filter)
{
    const int limit = clampAuditLimit(filter.limit);
    QJsonArray matched;
    for (const QJsonValue &v : events) {
        const QJsonObject ev = v.toObject();
        if (auditEventMatches(ev, filter)) matched.append(ev);
    }

    // 从尾部（最新）取 limit 条，再按「新→旧」输出：界面上先看到刚发生的事。
    QJsonArray out;
    const int size = static_cast<int>(matched.size());
    const int from = qMax(0, size - limit);
    for (int i = size - 1; i >= from; --i) out.append(matched.at(i));
    return out;
}
