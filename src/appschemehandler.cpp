#include "appschemehandler.h"

#include <QBuffer>
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

    // 正常请求不打日志（每次启动 5+ 条会刷屏）；
    // 只在 404 / 打开失败时告警，这两类正是「页面空白」时的关键线索。
    // 根路径回退到 index.html
    if (path.isEmpty() || path == QLatin1String("/")) {
        path = QStringLiteral("/index.html");
    }

    // 去掉前导 '/'
    if (path.startsWith(QLatin1Char('/'))) {
        path.remove(0, 1);
    }

    // 虚拟二进制资源：不走文件系统，直接交给 provider 供数
    if (path == QLatin1String("__volume/current.bin")) {
        serveVolume(job);
        return;
    }

    // 解析并校验路径，防止目录穿越
    QFileInfo info(m_rootDir + QLatin1Char('/') + path);
    const QDir rootDir(m_rootDir);
    const QString canonicalRoot = rootDir.canonicalPath();
    const QString canonicalFile = info.canonicalFilePath();

    if (!canonicalFile.startsWith(canonicalRoot) || !info.exists() || !info.isFile()) {
        qWarning().noquote()
            << "[Scheme] 404:" << url.toString()
            << "canonicalFile=" << canonicalFile
            << "canonicalRoot=" << canonicalRoot
            << "exists=" << info.exists() << "isFile=" << info.isFile();
        job->fail(QWebEngineUrlRequestJob::UrlNotFound);
        return;
    }

    QFile *file = new QFile(info.absoluteFilePath(), job);
    if (!file->open(QIODevice::ReadOnly)) {
        qWarning().noquote() << "[Scheme] 打开失败:" << info.absoluteFilePath()
                             << file->errorString();
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

void AppSchemeHandler::setVolumeProvider(VolumeProvider provider)
{
    m_volumeProvider = std::move(provider);
}

void AppSchemeHandler::serveVolume(QWebEngineUrlRequestJob *job)
{
    if (!m_volumeProvider) {
        qWarning().noquote() << "[Scheme] 体积请求但未注册 provider";
        job->fail(QWebEngineUrlRequestJob::UrlNotFound);
        return;
    }

    const QByteArray data = m_volumeProvider();
    if (data.isEmpty()) {
        qWarning().noquote() << "[Scheme] 体积请求时数据尚未就绪";
        job->fail(QWebEngineUrlRequestJob::UrlNotFound);
        return;
    }

    // QBuffer 由 job 持有（parent = job），job 结束时一并释放
    auto *buffer = new QBuffer(job);
    buffer->setData(data);
    if (!buffer->open(QIODevice::ReadOnly)) {
        qWarning().noquote() << "[Scheme] 体积缓冲打开失败";
        job->fail(QWebEngineUrlRequestJob::RequestFailed);
        return;
    }
    job->reply("application/octet-stream", buffer);
}
