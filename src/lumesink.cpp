#include "lumesink.h"

#include <QDateTime>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStringList>
#include <QTimer>

namespace {

QString digestForLume(const QString &value)
{
    const QByteArray hash = QCryptographicHash::hash(
        QByteArrayLiteral("lume-qt|") + value.toUtf8(), QCryptographicHash::Sha256);
    return QString::fromLatin1(hash.toHex().left(16));
}

QString scrubText(const QString &text)
{
    QString out = text;
    // 路径先于 UID 折叠：路径里常带 DICOM UID，先吃掉整段才不会留下半截。
    // 字符类允许空格（macOS 的 "Application Support" 这类目录名很常见），
    // 到句子标点或换行停止；宁可连尾随描述一起折掉，也不能留下半截路径。
    static const QRegularExpression homePathRe(
        QStringLiteral("~/[^\\r\\n,;\"'。；，、]+"));
    static const QRegularExpression windowsPathRe(
        QStringLiteral("[A-Za-z]:[\\\\/][^\\r\\n,;\"'。；，、]+"));
    static const QRegularExpression unixPathRe(
        QStringLiteral("(?:/Users|/home|/private|/tmp|/var|/opt|/Library|/Applications|/System|/Volumes)"
                       "/[^\\r\\n,;\"'。；，、]+"));
    // DICOM UID 可以只有三段（2.25.<整数> 是 UUID 派生形式）；旧正则要求五段，
    // 会把 2.25.* 与 1.2.840.10008 这类短 UID 原样放行。IPv4 与此同形，会一并
    // 折叠成 <uid>，属于可接受的过度脱敏：本地 JSONL 仍是完整权威记录。
    static const QRegularExpression uidRe(QStringLiteral("\\d+(?:\\.\\d+){2,}"));
    out.replace(homePathRe, QStringLiteral("<path>"));
    out.replace(windowsPathRe, QStringLiteral("<path>"));
    out.replace(unixPathRe, QStringLiteral("<path>"));
    out.replace(uidRe, QStringLiteral("<uid>"));
    return out;
}

QString safeUrlForLog(const QUrl &url)
{
    // 端点可能配置成 http://user:pass@host/... 或带 query token。日志只保留
    // scheme/host/port/path；userinfo、query、fragment 一律不落盘。
    QUrl copy = url;
    copy.setUserInfo(QString());
    copy.setQuery(QString());
    copy.setFragment(QString());
    return copy.toString();
}

bool isPatientKey(const QString &key)
{
    const QString k = key.toLower();
    // 黑名单只作纵深防御；真正的边界是下面的白名单。这里把常见 DICOM
    // 患者/机构/医嘱字段都列出，避免以后新增事件字段时先漏一轮。
    static const QStringList blocked = {
        QStringLiteral("accession"),
        QStringLiteral("accessionnumber"),
        QStringLiteral("admissionid"),
        QStringLiteral("admittingdiagnosesdescription"),
        QStringLiteral("institutionaddress"),
        QStringLiteral("institutionname"),
        QStringLiteral("institutionaldepartmentname"),
        QStringLiteral("issuerofpatientid"),
        QStringLiteral("medicalrecordnumber"),
        QStringLiteral("nameofphysiciansreadingstudy"),
        QStringLiteral("operatorname"),
        QStringLiteral("otherpatientids"),
        QStringLiteral("patientaddress"),
        QStringLiteral("patientcomments"),
        QStringLiteral("patienttelephone"),
        QStringLiteral("performingsphysicianname"),
        QStringLiteral("personname"),
        QStringLiteral("referringphysician"),
        QStringLiteral("requestedprocedureid"),
        QStringLiteral("studyid"),
        QStringLiteral("studydescription"),
        QStringLiteral("visitnumber"),
    };
    return k.startsWith(QStringLiteral("patient")) || blocked.contains(k);
}

bool isIdentifierKey(const QString &key)
{
    const QString k = key.toLower();
    return k == QStringLiteral("studyuid")
        || k == QStringLiteral("studyinstanceuid")
        || k == QStringLiteral("dir")
        || k == QStringLiteral("destdir")
        || k == QStringLiteral("sourcedir")
        || k == QStringLiteral("filepath")
        || k == QStringLiteral("path");
}

bool isAllowedLumeKey(const QString &key)
{
    // 上行只保留审计/时序需要的字段。未知字段一律丢弃，而不是「默认放行」：
    // 新增 DICOM 元数据或调试字段时，漏字段的代价是 PHI 出端，远大于少一个字段。
    static const QStringList allowed = {
        QStringLiteral("ts"),
        QStringLiteral("tsUs"),
        QStringLiteral("type"),
        QStringLiteral("level"),
        QStringLiteral("priority"),
        QStringLiteral("channel"),
        QStringLiteral("spo2"),
        QStringLiteral("hr"),
        QStringLiteral("ms"),
        QStringLiteral("ageMs"),
        QStringLiteral("p50"),
        QStringLiteral("p95"),
        QStringLiteral("p50Ms"),
        QStringLiteral("p95Ms"),
        QStringLiteral("thresholdMs"),
        QStringLiteral("parseMs"),
        QStringLiteral("roundTripMs"),
        QStringLiteral("batchGapMs"),
        QStringLiteral("batchSamples"),
        QStringLiteral("gapSeconds"),
        QStringLiteral("seconds"),
        QStringLiteral("host"),
        QStringLiteral("port"),
        QStringLiteral("studies"),
        QStringLiteral("events"),
        QStringLiteral("files"),
        QStringLiteral("layers"),
        QStringLiteral("dims"),
        QStringLiteral("states"),
        QStringLiteral("group"),
        QStringLiteral("provider"),
        QStringLiteral("platform"),
        QStringLiteral("providerDescription"),
        QStringLiteral("calledAe"),
        QStringLiteral("previousLastEvent"),
        QStringLiteral("previousSession"),
        QStringLiteral("previousStart"),
        QStringLiteral("bytes"),
        QStringLiteral("bytes8"),
        QStringLiteral("bytesHu"),
        QStringLiteral("keptWeb"),
        QStringLiteral("hasHardwareClock"),
        QStringLiteral("rssBeforeMb"),
        QStringLiteral("rssAfterMb"),
        QStringLiteral("savedAtUs"),
        QStringLiteral("reason"),
        QStringLiteral("attempt"),
        QStringLiteral("method"),
        QStringLiteral("backoffMs"),
        QStringLiteral("alarmActive"),
        QStringLiteral("alarmLevel"),
        QStringLiteral("from"),
        QStringLiteral("storeAe"),
        QStringLiteral("storePort"),
        QStringLiteral("to"),
        QStringLiteral("session"),
        QStringLiteral("source"),
        QStringLiteral("message"),
        QStringLiteral("event"),
        // 已哈希的标识字段（来自旧事件或嵌套对象）可以保留。
        QStringLiteral("studyUidHash"),
        QStringLiteral("studyInstanceUidHash"),
        QStringLiteral("dirHash"),
        QStringLiteral("destDirHash"),
        QStringLiteral("sourceDirHash"),
        QStringLiteral("filePathHash"),
        QStringLiteral("pathHash"),
    };
    return allowed.contains(key, Qt::CaseInsensitive);
}

QJsonValue sanitizeValue(const QJsonValue &value);

QJsonObject sanitizeObject(const QJsonObject &input)
{
    QJsonObject out;
    for (auto it = input.begin(); it != input.end(); ++it) {
        const QString key = it.key();
        if (isPatientKey(key)) {
            continue;
        }
        if (isIdentifierKey(key)) {
            const QString raw = it.value().toString();
            if (!raw.isEmpty()) {
                const QString hashKey = key + QStringLiteral("Hash");
                if (!out.contains(hashKey)) {
                    out.insert(hashKey, digestForLume(raw));
                }
            }
            continue;
        }
        if (!isAllowedLumeKey(key)) {
            continue;
        }
        out.insert(key, sanitizeValue(it.value()));
    }
    return out;
}

QJsonValue sanitizeValue(const QJsonValue &value)
{
    if (value.isObject()) {
        return sanitizeObject(value.toObject());
    }
    if (value.isArray()) {
        QJsonArray array;
        for (const QJsonValue &item : value.toArray()) {
            array.append(sanitizeValue(item));
        }
        return array;
    }
    if (value.isString()) {
        return scrubText(value.toString());
    }
    return value;
}

} // namespace

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
        qWarning().noquote() << "[Lume] QTRS_LUME_URL 不是合法 URL：" << safeUrlForLog(QUrl(base))
                             << "，上行已禁用";
        return;
    }

    m_base = QUrl(base);
    m_url = url;
    m_token = QString::fromLocal8Bit(qgetenv("QTRS_LUME_TOKEN")).trimmed();
    m_enabled = true;
    m_nam = new QNetworkAccessManager(this);
    qInfo().noquote() << "[Lume] 审计上行已启用：" << safeUrlForLog(m_url)
                      << (m_token.isEmpty() ? QStringLiteral("（无鉴权头）")
                                            : QStringLiteral("（Bearer 鉴权）"));
}

QJsonObject LumeSink::sanitizedForLume(const QJsonObject &event)
{
    return sanitizeObject(event);
}

int LumeSink::backoffDelayMs(int baseMs, int failedCount)
{
    if (baseMs <= 0)
        baseMs = 1000;
    int delay = baseMs;
    for (int i = 1; i < failedCount; ++i) {
        if (delay >= 60000)
            return 60000;
        delay = qMin(delay * 2, 60000);
    }
    return delay;
}

void LumeSink::applyAuth(QNetworkRequest &request) const
{
    if (!m_token.isEmpty()) {
        request.setRawHeader("Authorization",
                             QByteArrayLiteral("Bearer ") + m_token.toUtf8());
    }
}

void LumeSink::fetchConfig(std::function<void(const QJsonObject &)> callback)
{
    if (!m_enabled || !m_nam || !m_base.isValid() || !callback)
        return;

    QUrl url(m_base.toString() + QStringLiteral("/api/medmon/config"));
    QNetworkRequest request(url);
    applyAuth(request);
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
    m_queue.append(sanitizedForLume(event));
    if (!m_busy)
        flush();
}

void LumeSink::flush()
{
    if (!m_enabled || m_busy || !m_nam || m_queue.isEmpty())
        return;
    // 失败后的退避窗口内不抢跑：新事件 post() 会触发 flush，但不能绕过
    // 已排好的重试定时器，否则退避形同虚设。
    if (QDateTime::currentMSecsSinceEpoch() < m_nextAttemptAtMs)
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
    applyAuth(request);
    request.setTransferTimeout(3000);

    m_busy = true;
    QNetworkReply *reply = m_nam->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, batch]() {
        reply->deleteLater();
        m_busy = false;
        if (reply->error() == QNetworkReply::NoError) {
            const int n = static_cast<int>(batch.size());
            const bool recovered = m_failed > 0;
            if (m_sent == 0)
                qInfo().noquote() << "[Lume] 首次上行成功，共" << n << "条";
            else if (recovered)
                qInfo().noquote() << "[Lume] 上行恢复，本次补发" << n << "条";
            m_failed = 0;
            m_nextAttemptAtMs = 0;
            m_sent += n;
            flush();
            return;
        }
        ++m_failed;
        const int delayMs = backoffDelayMs(m_retryBaseMs, m_failed);
        m_nextAttemptAtMs = QDateTime::currentMSecsSinceEpoch() + delayMs;
        if (m_failed == 1 || m_failed % 20 == 0) {
            qWarning().noquote() << "[Lume] 上行失败（第" << m_failed << "次）："
                                 << reply->errorString() << "，"
                                 << static_cast<int>(m_queue.size() + batch.size()) << "条待重试，"
                                 << (delayMs / 1000) << "s 后重试";
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
        QTimer::singleShot(delayMs, this, &LumeSink::flush);
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
    applyAuth(request);
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

    qInfo().noquote() << "[Lume] AI 复盘请求：" << safeUrlForLog(url);
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
