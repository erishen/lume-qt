#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QUrl>

/**
 * LumeSink — 把术中审计事件上行到 Lume 事件总线（HTTP/JSON）。
 *
 * 边界：本地台账仍然是唯一权威（AuditSink 写盘不受网络影响）。Lume 侧只是
 * 「跨端可见的事件镜像」，丢了不影响本地复核，也不会阻塞业务流程——
 * 队列有上限、请求有超时、失败按退避重试。
 *
 * 端点约定（参考实现见 tools/lume-medmon/app.lume）：
 *   QTRS_LUME_URL=http://127.0.0.1:8090   →  POST /api/medmon/audit
 *   body: { app, sentAt, events: [ {ts,tsUs,session,type,...}, ... ] }
 *
 * 只有设置了 QTRS_LUME_URL 才启用；未设置时对象存在但完全不建网络栈。
 */
class LumeSink : public QObject
{
    Q_OBJECT

public:
    explicit LumeSink(QObject *parent = nullptr);

    bool enabled() const { return m_enabled; }
    QUrl endpoint() const { return m_url; }

    /// 投递一条事件（来自 AuditSink 的观察者回调）。线程：主线程。
    void post(const QJsonObject &event);

    int sent() const { return m_sent; }
    int dropped() const { return m_dropped; }
    int failed() const { return m_failed; }

private:
    void flush();

    bool m_enabled = false;
    QUrl m_url;
    class QNetworkAccessManager *m_nam = nullptr;
    QJsonArray m_queue;
    bool m_busy = false;
    int m_sent = 0;
    int m_failed = 0;
    int m_dropped = 0;
    int m_batchMax = 50;
    int m_queueMax = 500;
    int m_retryMs = 5000;
};
