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
    scheme.setFlags(
        QWebEngineUrlScheme::Flag::SecureScheme
        | QWebEngineUrlScheme::Flag::LocalScheme
        | QWebEngineUrlScheme::Flag::LocalAccessAllowed);
    QWebEngineUrlScheme::registerScheme(scheme);

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("qt-react-shell"));
    app.setOrganizationName(QStringLiteral("research"));

    qInfo() << "Starting qt-react-shell with web root:" << WEB_ROOT_PATH;

    MainWindow window;
    window.show();

    return app.exec();
}
