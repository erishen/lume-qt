#pragma once

#include <QFile>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include <functional>

/**
 * AuditSink — 结构化事件台账（JSON Lines，一行一条）。
 *
 * 术中系统里「刚才到底发生了什么」必须能事后复核：某通道什么时候中断、断了多久、
 * 看门狗什么时候报的、是谁恢复的、报警什么时候触发。只往 stderr 打日志不够——
 * 那是人看的，不是可解析、可统计、可归档的证据。
 *
 * 每条事件固定带：wall time、epoch 微秒、会话 id、类型；其余字段原样并入。
 * 文件按天切分：<AppData>/audit/audit-YYYYMMDD.jsonl，同一次运行共用一个 session id。
 *
 * 边界说明：这是工程级台账，不是合规证明。IEC 62304 的证据链还需要版本冻结、
 * 验证记录、变更控制与签核，本类只负责「事件时间线」这一层。
 */
class AuditSink : public QObject
{
public:
    explicit AuditSink(QObject *parent = nullptr);
    ~AuditSink() override;

    /// 打开当日台账文件。dir 为空时用 AppDataLocation/audit。
    /// 返回 false 表示无法写（此时所有 record() 只回落成 qWarning，不中断业务）。
    bool open(const QString &dir = QString());

    /// 追加一条事件。type 必填（如 signal.stale / alarm.on / provider.start）。
    /// 返回写下去的对象（调用方一般不用管）。只允许主线程调用。
    QJsonObject record(const QString &type, const QVariantMap &fields = {});

    /// 观察者：每条事件落盘后回调（用于上行到 Lume 等外部事件总线）。
    /// 同步调用，实现方必须自己保证不阻塞、不抛异常；本地写盘不依赖观察者。
    using Observer = std::function<void(const QJsonObject &)>;
    void setObserver(Observer observer) { m_observer = std::move(observer); }

    QString filePath() const { return m_path; }
    QString sessionId() const { return m_session; }
    int count() const { return m_count; }

private:
    bool openDay(const QString &day);
    bool rotateCurrent();

    QFile m_file;
    QString m_baseDir;
    QString m_day;
    qint64 m_maxBytes = 8ll * 1024 * 1024;
    QString m_path;
    QString m_session;
    int m_count = 0;
    bool m_openFailed = false;
    Observer m_observer;
};
