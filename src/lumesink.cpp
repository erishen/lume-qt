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

    m_url = url;
    m_enabled = true;
    m_nam = new QNetworkAccessManager(this);
    qInfo().noquote() << "[Lume] 审计上行已启用：" << m_url.toString();
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
    body.insert(QStringLiteral("app"), QStringLiteral("qt-react-shell"));
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
