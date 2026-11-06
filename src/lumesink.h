#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QUrl>

#include <functional>

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
    /// 投递前会做一次脱敏：studyUid/dir 换成不可逆摘要，患者相关键直接移除，
    /// 消息里的绝对路径与 DICOM UID 也会被替换。本地台账不受影响。
    void post(const QJsonObject &event);

    /// 纯函数，供 post() 与单测使用；不修改入参。
    static QJsonObject sanitizedForLume(const QJsonObject &event);

    /// 拉取 Lume 下发的配置（`GET <base>/api/medmon/config`）。
    /// 未启用 / 请求失败时回调不会被调用——调用方保留本地默认值即可。
    void fetchConfig(std::function<void(const QJsonObject &)> callback);

    /// 让 Lume 的 Agent 读本机台账并生成一段复盘（`POST <base>/react/api/chat`）。
    /// 这是异步调用：reply 的 SSE 全部收完后回调一次，结果结构：
    ///   { ok, answer, notes:[...], error, mode:"model"|"demo" }
    /// 未启用或网络失败也回调，由调用方决定怎么显示；不会阻塞 UI。
    void ask(const QString &message, std::function<void(const QJsonObject &)> callback);

    int sent() const { return m_sent; }
    int dropped() const { return m_dropped; }
    int failed() const { return m_failed; }

private:
    void flush();

    bool m_enabled = false;
    QUrl m_base;
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
