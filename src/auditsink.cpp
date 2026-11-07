#include "auditsink.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
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

constexpr QFileDevice::Permissions kOwnerDir =
    QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
constexpr QFileDevice::Permissions kOwnerFile =
    QFileDevice::ReadOwner | QFileDevice::WriteOwner;

void restrictDir(const QString &path)
{
    if (!path.isEmpty())
        QFile::setPermissions(path, kOwnerDir);
}

void restrictFile(const QString &path)
{
    if (!path.isEmpty())
        QFile::setPermissions(path, kOwnerFile);
}

void pruneAuditFiles(const QString &base, int retainDays)
{
    if (retainDays <= 0)
        return;
    const QDate cutoff = QDate::currentDate().addDays(-retainDays);
    if (!cutoff.isValid())
        return;

    const QDir dir(base);
    const QFileInfoList files = dir.entryInfoList(
        {QStringLiteral("audit-*.jsonl*")}, QDir::Files, QDir::Name);
    int removed = 0;
    for (const QFileInfo &info : files) {
        // 从文件名取日期，而不是 mtime：复制/恢复备份不会让旧 PHI 重新变成「新文件」。
        const QDate day = QDate::fromString(info.fileName().mid(6, 8), QStringLiteral("yyyyMMdd"));
        if (!day.isValid() || day >= cutoff)
            continue;
        if (QFile::remove(info.absoluteFilePath()))
            ++removed;
    }
    if (removed > 0)
        qInfo().noquote() << "[Audit] 按保留期清理旧台账：" << removed << "份";
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
    restrictDir(base);

    const QDateTime now = QDateTime::currentDateTime();
    const QString day = now.toString(QStringLiteral("yyyyMMdd"));
    m_session = now.toString(QStringLiteral("yyyyMMdd'T'HHmmss"))
                + QLatin1Char('-') + QString::number(QCoreApplication::applicationPid());

    m_baseDir = base;
    // 台账是「一直追加」的，不设上限迟早把磁盘写满（术中机器往往没人管）。
    // 默认单文件 8 MB，可用 QTRS_AUDIT_MAX_MB 调整。
    if (const QByteArray env = qgetenv("QTRS_AUDIT_MAX_MB"); !env.isEmpty()) {
        const double mb = env.toDouble();
        if (mb > 0.0) m_maxBytes = static_cast<qint64>(mb * 1024.0 * 1024.0);
    }
    // 保留期是显式开关：默认不自动删任何证据，只有设了天数才按文件名日期清理。
    if (const QByteArray retain = qgetenv("QTRS_AUDIT_RETAIN_DAYS"); !retain.isEmpty())
        pruneAuditFiles(base, retain.toInt());
    if (!openDay(day)) return false;
    qInfo().noquote() << "[Audit] 台账：" << m_path << "session:" << m_session;
    return true;
}

bool AuditSink::openDay(const QString &day)
{
    m_day = day;
    m_path = QStringLiteral("%1/audit-%2.jsonl").arg(m_baseDir, day);

    // 增量开新文件前先看大小：轮转而不是直接覆盖，旧证据必须留着。
    if (QFileInfo::exists(m_path) && QFileInfo(m_path).size() >= m_maxBytes) {
        if (!rotateCurrent()) return false;
    }

    m_file.setFileName(m_path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        m_openFailed = true;
        qWarning().noquote() << "[Audit] 无法打开台账文件：" << m_path << m_file.errorString();
        return false;
    }
    // 台账含 studyUid/落盘目录/PACS 端点，按 PHI 处理：仅属主可读写。
    restrictFile(m_path);
    m_openFailed = false;
    return true;
}

bool AuditSink::rotateCurrent()
{
    m_file.close();
    const QString base = QStringLiteral("%1/audit-%2.jsonl").arg(m_baseDir, m_day);
    for (int i = 1; i < 1000; ++i) {
        const QString rotated = QStringLiteral("%1.%2").arg(base).arg(i, 3, 10, QLatin1Char('0'));
        if (QFileInfo::exists(rotated)) continue;
        if (QFile::rename(base, rotated)) {
            restrictFile(rotated);
            qInfo().noquote() << "[Audit] 台账轮转：" << rotated;
            return true;
        }
        qWarning().noquote() << "[Audit] 台账轮转失败：" << base << "→" << rotated;
        return false;
    }
    qWarning().noquote() << "[Audit] 台账轮转失败：编号用尽";
    return false;
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

    if (m_observer)
        m_observer(obj);

    // 跨天要切新文件，否则第二天的事件会写进昨天的台账。
    const QString today = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd"));
    if (today != m_day) {
        m_file.close();
        openDay(today);
    } else if (m_file.isOpen() && m_file.size() >= m_maxBytes) {
        rotateCurrent();
        openDay(m_day);
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
