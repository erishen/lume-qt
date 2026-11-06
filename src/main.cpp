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
 * 单实例锁：不用 QLockFile，因为它的「陈旧」判定同时看文件年龄与进程存活，
 * 把 staleLockTime 设成 0 又会把进程存活检查一起关掉——进程被杀后反而再也起不来。
 * 这里只认 PID 存活：锁文件里写自己的 pid，启动时若持有者还活着就拒绝，
 * 若已经不在（被 kill -9 / 断电）就直接接管。
 * 返回 true 表示拿到了锁；false 时 holderPid 里是仍活着的那一方。
 */
bool acquireInstanceLock(const QString &path, qint64 *holderPid)
{
    if (holderPid) *holderPid = -1;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const int fd = ::open(path.toUtf8().constData(), O_CREAT | O_EXCL | O_WRONLY, 0644);
        if (fd >= 0) {
            const QByteArray pid = QByteArray::number(static_cast<qint64>(::getpid())) + '\n';
            ::write(fd, pid.constData(), static_cast<size_t>(pid.size()));
            ::close(fd);
            return true;
        }
        QFile existing(path);
        if (!existing.open(QIODevice::ReadOnly)) return false;
        const qint64 pid = existing.readLine().trimmed().toLongLong();
        existing.close();
        // 持有者还活着（EPERM 说明进程存在但属于别人）→ 拒绝启动。
        if (pid > 0 && (::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM)) {
            if (holderPid) *holderPid = pid;
            return false;
        }
        // 死进程留下的锁：清掉，下一轮重新用 O_EXCL 抢。
        QFile::remove(path);
    }
    return false;
}

/// 正常退出时清掉自己的锁；被 kill -9 时清不掉，靠下一次启动的接管逻辑处理。
QString gInstanceLockPath;

void releaseInstanceLock()
{
    if (gInstanceLockPath.isEmpty()) return;
    QFile existing(gInstanceLockPath);
    if (!existing.open(QIODevice::ReadOnly)) return;
    const qint64 pid = existing.readLine().trimmed().toLongLong();
    existing.close();
    if (pid == static_cast<qint64>(::getpid())) QFile::remove(gInstanceLockPath);
}
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
    // 只按 PID 判活：进程被杀留下的锁文件能在下一次启动时自动接管，
    // 不设「锁文件太老就忽略」的时限，避免把仍在运行的旧实例误判为死掉。
    if (!acquireInstanceLock(lockPath, &holderPid)) {
        qWarning().noquote()
            << "[Single] 已有实例在运行（pid=" << holderPid << "，锁：" << lockPath
            << "），本次启动中止。术中不允许两个界面共享同一份台账与体数据状态。";
        return 1;
    }
    gInstanceLockPath = lockPath;
    std::atexit(releaseInstanceLock);

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
