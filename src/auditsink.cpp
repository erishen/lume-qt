#include "auditsink.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QDebug>

#include <chrono>

namespace {

double nowMicros()
{
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    return static_cast<double>(us);
}

} // namespace

AuditSink::AuditSink(QObject *parent)
    : QObject(parent)
{
}

AuditSink::~AuditSink()
{
    if (m_file.isOpen()) {
        m_file.close();
    }
}

bool AuditSink::open(const QString &dir)
{
    const QString base = dir.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
              + QStringLiteral("/audit")
        : dir;

    if (!QDir().mkpath(base)) {
        m_openFailed = true;
        qWarning().noquote() << "[Audit] 无法创建台账目录：" << base;
        return false;
    }

    const QDateTime now = QDateTime::currentDateTime();
    const QString day = now.toString(QStringLiteral("yyyyMMdd"));
    m_session = now.toString(QStringLiteral("yyyyMMdd'T'HHmmss"))
                + QLatin1Char('-') + QString::number(QCoreApplication::applicationPid());

    m_path = QStringLiteral("%1/audit-%2.jsonl").arg(base, day);
    m_file.setFileName(m_path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        m_openFailed = true;
        qWarning().noquote() << "[Audit] 无法打开台账文件：" << m_path << m_file.errorString();
        return false;
    }
    qInfo().noquote() << "[Audit] 台账：" << m_path << "session:" << m_session;
    return true;
}

QJsonObject AuditSink::record(const QString &type, const QVariantMap &fields)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("ts"), QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    obj.insert(QStringLiteral("tsUs"), nowMicros());
    obj.insert(QStringLiteral("session"), m_session);
    obj.insert(QStringLiteral("type"), type);
    for (auto it = fields.cbegin(); it != fields.cend(); ++it) {
        if (obj.contains(it.key())) {
            continue; // 固定字段不允许被业务字段覆盖
        }
        obj.insert(it.key(), QJsonValue::fromVariant(it.value()));
    }

    if (!m_file.isOpen()) {
        if (!m_openFailed) {
            qWarning().noquote() << "[Audit] 台账未打开，事件丢弃：" << type;
            m_openFailed = true;
        }
        return obj;
    }

    m_file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    m_file.write("\n");
    m_file.flush();
    ++m_count;
    return obj;
}
