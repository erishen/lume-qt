#include "mainwindow.h"

#include <QWebChannel>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <QUrl>
#include <QUrlQuery>

#include "appschemehandler.h"
#include "bridge.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("qt-react-shell"));
    resize(1280, 800);

    // 独立 off-the-record profile，避免污染用户默认 profile，
    // 同时让我们能自定义 scheme handler 与权限策略。
    m_profile = new QWebEngineProfile(QStringLiteral("qt-react-shell"), this);
    m_schemeHandler = new AppSchemeHandler(QStringLiteral(WEB_ROOT_PATH), this);
    m_profile->installUrlSchemeHandler(QByteArrayLiteral("app"), m_schemeHandler);

    // 允许本地资源访问远程资源（如需加载外部图片/CDN 可放开），
    // 并允许 JS 弹出 alert/confirm 等对话框。
    m_profile->settings()->setAttribute(
        QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
    m_profile->settings()->setAttribute(
        QWebEngineSettings::JavascriptCanOpenWindows, true);

    m_view = new QWebEngineView(m_profile, this);
    setCentralWidget(m_view);

    setupWebChannel();

    // 加载前端（React 构建产物）
    const QUrl url(QStringLiteral("app://app/index.html"));
    m_view->load(url);
}

MainWindow::~MainWindow() = default;

void MainWindow::setupWebChannel()
{
    m_bridge = new Bridge(this);

    QWebChannel *channel = new QWebChannel(this);
    channel->registerObject(QStringLiteral("bridge"), m_bridge);
    m_view->page()->setWebChannel(channel);
}
