#include "appschemehandler.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QWebEngineUrlRequestJob>

AppSchemeHandler::AppSchemeHandler(const QString &rootDir, QObject *parent)
    : QWebEngineUrlSchemeHandler(parent)
    , m_rootDir(QDir::cleanPath(rootDir))
{
}

void AppSchemeHandler::requestStarted(QWebEngineUrlRequestJob *job)
{
    const QUrl url = job->requestUrl();
    QString path = url.path();

    // 根路径回退到 index.html
    if (path.isEmpty() || path == QLatin1String("/")) {
        path = QStringLiteral("/index.html");
    }

    // 去掉前导 '/'
    if (path.startsWith(QLatin1Char('/'))) {
        path.remove(0, 1);
    }

    // 解析并校验路径，防止目录穿越
    QFileInfo info(m_rootDir + QLatin1Char('/') + path);
    const QDir rootDir(m_rootDir);
    const QString canonicalRoot = rootDir.canonicalPath();
    const QString canonicalFile = info.canonicalFilePath();

    if (!canonicalFile.startsWith(canonicalRoot) || !info.exists() || !info.isFile()) {
        job->fail(QWebEngineUrlRequestJob::UrlNotFound);
        return;
    }

    QFile *file = new QFile(info.absoluteFilePath(), job);
    if (!file->open(QIODevice::ReadOnly)) {
        job->fail(QWebEngineUrlRequestJob::RequestFailed);
        return;
    }

    // 依据扩展名给出 MIME 类型
    const QString suffix = info.suffix().toLower();
    QByteArray mime;
    if (suffix == QLatin1String("html")) {
        mime = "text/html";
    } else if (suffix == QLatin1String("js") || suffix == QLatin1String("mjs")) {
        mime = "application/javascript";
    } else if (suffix == QLatin1String("css")) {
        mime = "text/css";
    } else if (suffix == QLatin1String("json")) {
        mime = "application/json";
    } else if (suffix == QLatin1String("svg")) {
        mime = "image/svg+xml";
    } else if (suffix == QLatin1String("png")) {
        mime = "image/png";
    } else if (suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg")) {
        mime = "image/jpeg";
    } else if (suffix == QLatin1String("woff")) {
        mime = "font/woff";
    } else if (suffix == QLatin1String("woff2")) {
        mime = "font/woff2";
    } else if (suffix == QLatin1String("ico")) {
        mime = "image/x-icon";
    } else {
        mime = "application/octet-stream";
    }

    job->reply(mime.constData(), file);
}
