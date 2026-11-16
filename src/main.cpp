#include <QApplication>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <QWebEngineUrlScheme>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include "instancelock.h"
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
/**
 * 单实例锁的取舍见 src/instancelock.h：不用 QLockFile（它的陈旧判定同时看文件年龄
 * 与进程存活，staleLockTime=0 会连存活检查一起关掉），也不用「O_EXCL 建锁 + 读 pid
 * 判活 + 删陈旧锁」——那条路径里判定与删除不是原子的。改用 flock，所有权由内核维护。
 */
InstanceLock gInstanceLock;
/**
 * 从旧的 qt-react-shell 迁移本机数据与偏好。
 *
 * 改名会改变 QStandardPaths::AppDataLocation 和 QSettings 的文件名；不迁移的话，
 * 用户看到的就是「台账、PACS 落盘、窗口/体渲染偏好全没了」。只在目标不存在时
 * 整个搬一次，失败则保留旧目录并继续启动，绝不删除任何证据。
 */
void migrateLegacyProfile()
{
    const QString oldData = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                            + QStringLiteral("/research/qt-react-shell");
    const QString newData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (QFileInfo::exists(oldData) && !QFileInfo::exists(newData)) {
        QDir().mkpath(QFileInfo(newData).absolutePath());
        if (QDir().rename(oldData, newData)) {
            qInfo().noquote() << "[Migrate] 旧应用数据已迁移：" << oldData << "→" << newData;
        } else {
            qWarning().noquote() << "[Migrate] 旧应用数据迁移失败，保留原目录：" << oldData;
        }
    }

    QSettings oldSettings(QStringLiteral("research"), QStringLiteral("qt-react-shell"));
    QSettings newSettings;
    int copied = 0;
    for (const QString &key : oldSettings.allKeys()) {
        if (!newSettings.contains(key)) {
            newSettings.setValue(key, oldSettings.value(key));
            ++copied;
        }
    }
    if (copied > 0) {
        newSettings.sync();
        qInfo().noquote() << "[Migrate] 已迁移" << copied << "项设置到 lume-qt";
    }
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
    app.setApplicationName(QStringLiteral("lume-qt"));
    app.setOrganizationName(QStringLiteral("research"));

    // 必须在任何 QSettings / AppDataLocation 读取之前完成迁移。
    migrateLegacyProfile();

    // 台账、体数据状态、PACS 落地目录都是「一个应用一份」：两个实例同时跑会互相
    // 覆盖 session.end、重复写事件、抢同一个体积文件。宁可在启动时明确拒绝，
    // 也不要让两份看起来都正常的界面共享一份状态。
    const QString stateDir = QStandardPaths::writableLocation(
                                 QStandardPaths::AppDataLocation) + QStringLiteral("/state");
    QDir().mkpath(stateDir);
    const QString lockPath = stateDir + QStringLiteral("/instance.lock");
    qint64 holderPid = -1;
    // flock 拿不到就是另一个实例真的持有；文件里的 pid 只用于这条提示。
    if (!gInstanceLock.acquire(lockPath, &holderPid)) {
        qWarning().noquote()
            << "[Single] 已有实例在运行（pid=" << holderPid << "，锁：" << lockPath
            << "），本次启动中止。术中不允许两个界面共享同一份台账与体数据状态。";
        return 1;
    }
    // 锁随进程生命周期存在：全局对象析构时释放，被 kill -9 时由内核关闭 fd 释放。

    qInfo() << "Starting lume-qt with web root:" << WEB_ROOT_PATH;

    MainWindow window;
    // 临时验证开关：置顶可避免 macOS 遮挡检测把 WebView 的渲染/定时器节流，
    // 使延迟测量不被节流污染（仅诊断用，QTRS_TOPMOST=1 时才生效）。
    if (qEnvironmentVariableIsSet("QTRS_TOPMOST")) {
        window.setWindowFlag(Qt::WindowStaysOnTopHint, true);
    }
    window.show();

    return app.exec();
}
