#include <QApplication>
#include <QWebEngineUrlScheme>
#include <QDebug>

#include "mainwindow.h"

namespace {
/**
 * 术中场景要求「窗口被别的应用挡住也要继续刷新」。
 * macOS 上 Chromium 默认做遮挡检测，会把 rAF 与定时器节流到秒级——实测同一份代码，
 * 窗口前台时到达延迟 0.3–1.0ms，被遮挡时涨到数百毫秒（各通道读数还会趋同），
 * 屏幕上看到的则是冻结帧。这些开关必须在 QApplication 构造前设好。
 */
void disableRendererThrottling()
{
    const QByteArray ours = "--disable-features=CalculateNativeWinOcclusion"
                             " --disable-background-timer-throttling"
                             " --disable-backgrounding-occluded-windows"
                             " --disable-renderer-backgrounding";
    const QByteArray existing = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
    if (existing.contains("CalculateNativeWinOcclusion")) {
        return; // 用户已自行配置，尊重之
    }
    const QByteArray merged = existing.isEmpty() ? ours : existing + ' ' + ours;
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", merged);
}
} // namespace

int main(int argc, char *argv[])
{
    disableRendererThrottling();
    qInfo().noquote() << "[Chromium] flags:" << qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");

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
