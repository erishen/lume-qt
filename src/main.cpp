#include <QApplication>
#include <QWebEngineUrlScheme>
#include <QDebug>

#include "mainwindow.h"

int main(int argc, char *argv[])
{
    // 注册自定义 scheme 必须在 QApplication 构造之前，
    // 否则 Qt WebEngine 会拒绝使用未注册的 scheme。
    QWebEngineUrlScheme scheme("app");
    scheme.setSyntax(QWebEngineUrlScheme::Syntax::Host);
    // FetchApiAllowed 是硬性前提：没有它，页面里 fetch('app://app/...') 会直接
    // 抛 "Failed to fetch"（Chromium 把自定义 scheme 视为不可取数）。
    // 体积二进制正是靠 fetch 取走的（见 AppSchemeHandler::serveVolume）。
    scheme.setFlags(
        QWebEngineUrlScheme::Flag::SecureScheme
        | QWebEngineUrlScheme::Flag::LocalScheme
        | QWebEngineUrlScheme::Flag::LocalAccessAllowed
        | QWebEngineUrlScheme::Flag::CorsEnabled
        | QWebEngineUrlScheme::Flag::FetchApiAllowed);
    QWebEngineUrlScheme::registerScheme(scheme);

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("qt-react-shell"));
    app.setOrganizationName(QStringLiteral("research"));

    qInfo() << "Starting qt-react-shell with web root:" << WEB_ROOT_PATH;

    MainWindow window;
    // 临时验证开关：置顶可避免 macOS 遮挡检测把 WebView 的渲染/定时器节流，
    // 使延迟测量不被节流污染（仅诊断用，QTRS_TOPMOST=1 时才生效）。
    if (qEnvironmentVariableIsSet("QTRS_TOPMOST")) {
        window.setWindowFlag(Qt::WindowStaysOnTopHint, true);
    }
    window.show();

    return app.exec();
}
