#include "lumesink.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

LumeSink::LumeSink(QObject *parent)
    : QObject(parent)
{
    const QByteArray raw = qgetenv("QTRS_LUME_URL");
    if (raw.isEmpty())
        return;

    QString base = QString::fromLocal8Bit(raw).trimmed();
    if (base.isEmpty())
        return;
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    const QUrl url(base + QStringLiteral("/api/medmon/audit"));
    if (!url.isValid()) {
        qWarning().noquote() << "[Lume] QTRS_LUME_URL 不是合法 URL：" << base << "，上行已禁用";
        return;
    }

    m_base = QUrl(base);
    m_url = url;
    m_enabled = true;
    m_nam = new QNetworkAccessManager(this);
    qInfo().noquote() << "[Lume] 审计上行已启用：" << m_url.toString();
}

void LumeSink::fetchConfig(std::function<void(const QJsonObject &)> callback)
{
    if (!m_enabled || !m_nam || !m_base.isValid() || !callback)
        return;

    QUrl url(m_base.toString() + QStringLiteral("/api/medmon/config"));
    QNetworkRequest request(url);
    request.setTransferTimeout(2000);
    QNetworkReply *reply = m_nam->get(request);
    connect(reply, &QNetworkReply::finished, this, [reply, callback]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qWarning().noquote() << "[Lume] 拉取报警配置失败：" << reply->errorString()
                                 << "（继续用本地默认阈值）";
            return;
        }
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            qWarning().noquote() << "[Lume] 报警配置不是合法 JSON 对象：" << err.errorString();
            return;
        }
        callback(doc.object());
    });
}

void LumeSink::post(const QJsonObject &event)
{
    if (!m_enabled)
        return;
    // 队列上限：网络长时间不可用时只保留最近的事件，绝不无限占内存。
    if (m_queue.size() >= m_queueMax) {
        m_queue.removeFirst();
        ++m_dropped;
    }
    m_queue.append(event);
    if (!m_busy)
        flush();
}

void LumeSink::flush()
{
    if (!m_enabled || m_busy || !m_nam || m_queue.isEmpty())
        return;

    QJsonArray batch;
    const int take = static_cast<int>(qMin<qsizetype>(m_queue.size(), m_batchMax));
    for (int i = 0; i < take; ++i)
        batch.append(m_queue.takeAt(0));

    QJsonObject body;
    body.insert(QStringLiteral("app"), QStringLiteral("lume-qt"));
    body.insert(QStringLiteral("sentAt"), QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    body.insert(QStringLiteral("events"), batch);

    QNetworkRequest request(m_url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setTransferTimeout(3000);

    m_busy = true;
    QNetworkReply *reply = m_nam->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, batch]() {
        reply->deleteLater();
        m_busy = false;
        if (reply->error() == QNetworkReply::NoError) {
            const int n = static_cast<int>(batch.size());
            if (m_sent == 0)
                qInfo().noquote() << "[Lume] 首次上行成功，共" << n << "条";
            m_sent += n;
            flush();
            return;
        }
        ++m_failed;
        if (m_failed == 1 || m_failed % 20 == 0) {
            qWarning().noquote() << "[Lume] 上行失败（第" << m_failed << "次）："
                                 << reply->errorString() << "，"
                                 << static_cast<int>(m_queue.size() + batch.size()) << "条待重试";
        }
        // 失败批次放回队首重试；超过上限则丢弃最旧的，保证不无界增长。
        const int last = static_cast<int>(batch.size()) - 1;
        for (int i = last; i >= 0; --i) {
            if (m_queue.size() >= m_queueMax) {
                m_queue.removeLast();
                ++m_dropped;
            }
            m_queue.prepend(batch.at(i));
        }
        QTimer::singleShot(m_retryMs, this, &LumeSink::flush);
    });
}


void LumeSink::ask(const QString &message, std::function<void(const QJsonObject &)> callback)
{
    QJsonObject result;
    result.insert(QStringLiteral("ok"), false);
    result.insert(QStringLiteral("answer"), QString());
    result.insert(QStringLiteral("notes"), QJsonArray());
    result.insert(QStringLiteral("error"), QString());
    result.insert(QStringLiteral("mode"), QStringLiteral("model"));

    if (!m_enabled || !m_nam || !m_base.isValid()) {
        result.insert(QStringLiteral("error"),
                      QStringLiteral("Lume 未启用：启动时没有设置 QTRS_LUME_URL"));
        if (callback)
            callback(result);
        return;
    }

    QUrl url(m_base.toString() + QStringLiteral("/react/api/chat"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Accept", "text/event-stream");
    // 模型 + 一次工具调用实测约 35–90 秒；给 90 秒，超时仍走明确错误分支。
    request.setTransferTimeout(90000);

    QJsonObject body;
    body.insert(QStringLiteral("message"), message);
    // 只开放一次性复盘工具，避免模型在多轮里反复查同一批事件（实测一轮约 30–60 秒）。
    QJsonArray tools;
    tools.append(QStringLiteral("medmon_review"));
    body.insert(QStringLiteral("tools"), tools);
    // 每次点击都用独立会话，避免上一轮的上下文污染这一段复盘结论。
    body.insert(QStringLiteral("sessionId"),
                QStringLiteral("sess-qt-rs-%1").arg(QDateTime::currentMSecsSinceEpoch()));

    qInfo().noquote() << "[Lume] AI 复盘请求：" << url.toString();
    QNetworkReply *reply = m_nam->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [reply, callback, result]() mutable {
        reply->deleteLater();
        const QByteArray raw = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QString answer;
        QString error;
        QStringList notes;

        if (reply->error() != QNetworkReply::NoError) {
            if (reply->error() == QNetworkReply::TimeoutError)
                error = QStringLiteral("模型响应超时（约 90 秒），可稍后重试");
            else {
                error = reply->errorString();
                if (status > 0)
                    error += QStringLiteral("（HTTP %1）").arg(status);
            }
        } else if (status >= 400) {
            error = QStringLiteral("Lume 返回 HTTP %1").arg(status);
            const QJsonObject obj = QJsonDocument::fromJson(raw).object();
            if (obj.contains(QStringLiteral("error")))
                error += QStringLiteral("：") + obj.value(QStringLiteral("error")).toString();
        }

        // SSE 解析：每行一个 data: {...}，delta 拼回答，note 收工具轨迹/演示提示。
        // 即使 HTTP 失败也扫一遍，某些代理会把错误塞在 SSE 里而不是状态码里。
        const QString text = QString::fromUtf8(raw);
        for (const QString &lineRaw : text.split(QLatin1Char('\n'))) {
            QString line = lineRaw;
            if (line.endsWith(QLatin1Char('\r')))
                line.chop(1);
            if (!line.startsWith(QStringLiteral("data:")))
                continue;
            const QByteArray payload = line.mid(5).trimmed().toUtf8();
            if (payload == "[DONE]")
                continue;
            QJsonParseError parseError{};
            const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
            if (parseError.error != QJsonParseError::NoError || !doc.isObject())
                continue;
            const QJsonObject event = doc.object();
            const QString type = event.value(QStringLiteral("t")).toString();
            const QString data = event.value(QStringLiteral("d")).toString();
            if (type == QStringLiteral("delta")) {
                answer += data;
            } else if (type == QStringLiteral("note")) {
                if (!data.isEmpty())
                    notes.append(data);
            } else if (type == QStringLiteral("error")) {
                if (data.isEmpty())
                    error = QStringLiteral("Lume 返回错误");
                else if (error.isEmpty())
                    error = data;
            }
        }

        if (answer.isEmpty() && error.isEmpty()) {
            // 有些错误只出现在 notes 里（例如模型未配置），别把它伪装成空成功。
            for (const QString &note : notes) {
                if (note.contains(QStringLiteral("demo engine"), Qt::CaseInsensitive)
                    || note.contains(QStringLiteral("LLM_API_KEY"), Qt::CaseInsensitive)) {
                    error = QStringLiteral("Lume 未配置大模型（当前只返回离线演示内容）");
                    break;
                }
            }
            if (error.isEmpty())
                error = QStringLiteral("Lume 没有返回回答");
        }

        QJsonArray notesArray;
        for (const QString &note : notes)
            notesArray.append(note);
        const bool demo = notes.join(QLatin1Char(' ')).contains(QStringLiteral("demo engine"),
                                                                Qt::CaseInsensitive);

        result.insert(QStringLiteral("ok"), error.isEmpty() && !answer.isEmpty());
        result.insert(QStringLiteral("answer"), answer);
        result.insert(QStringLiteral("notes"), notesArray);
        result.insert(QStringLiteral("error"), error);
        result.insert(QStringLiteral("mode"), demo ? QStringLiteral("demo") : QStringLiteral("model"));
        qInfo().noquote() << QStringLiteral("[Lume] AI 复盘完成：%1 字，工具轨迹 %2 条，模式 %3")
                                 .arg(answer.size())
                                 .arg(notes.size())
                                 .arg(demo ? QStringLiteral("demo") : QStringLiteral("model"));
        if (callback)
            callback(result);
    });
}
