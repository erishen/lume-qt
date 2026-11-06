#include "bridge.h"
#include "auditsink.h"
#include "auditquery.h"
#include "lumesink.h"
#include "deviceprovider.h"
#include "dicomreader.h"
#include "pacsclient.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace {

#ifdef Q_OS_MACOS
#include <mach/mach.h>
#include <malloc/malloc.h>

/// 把 free 掉的页真正还给系统。系统 malloc 对大块释放后不保证立即 madvise，
/// 不刷一次「驻留」读数看不出变化（实测释放 48 MB 前后都停在 536 MB）。
size_t releaseFreePages()
{
    return malloc_zone_pressure_relief(nullptr, 0);
}
/// 进程自身物理驻留（MB）。`top`/`ps` 的 MEM 在这里抖动很大，
/// 验证「释放了多少」只能在同一进程内量前后差，别拿两次启动互相比。
double residentMb()
{
    // phys_footprint 含被压缩的脏页，resident_size 不含：体数据这种「刚写完就
    // 释放」的页会被系统压缩掉，用 resident_size 量会看到 80 MB 凭空消失。
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
        return -1.0;
    }
    return static_cast<double>(info.phys_footprint) / (1024.0 * 1024.0);
}
#else
double residentMb() { return -1.0; }
size_t releaseFreePages() { return 0; }
#endif

/// DCMTK 的失败文案是给开发者看的（"Operation now in progress" 其实是被拒连），
/// 看板上要能让人一眼判读，这里做一层人话映射；原始 detail 仍完整进台账。
QString humanPacsError(const QString &raw)
{
    if (raw.isEmpty()) return raw;
    if (raw.contains(QStringLiteral("TCP Initialization Error"))) {
        return QStringLiteral("无法建立 TCP 连接（目标未响应或被网络策略拦截）");
    }
    if (raw.contains(QStringLiteral("No Presentation Contexts"))) {
        return QStringLiteral("协商失败：双方没有共同的 DICOM 能力（检查 AE 与传输语法）");
    }
    if (raw.contains(QStringLiteral("Refused"))) {
        return QStringLiteral("对方拒绝了关联（检查 calledAE 是否在白名单内）");
    }
    return raw;
}

} // namespace

#include "simulatorprovider.h"

#include <QDateTime>
#include <QDebug>
#include <QMetaObject>
#include <QSysInfo>
#include <QTimer>

#include <chrono>
#include <cmath>
#include <cstdlib>

// ---------------------------------------------------------------------------
// 随机数辅助
// ---------------------------------------------------------------------------

double Bridge::nowMicros()
{
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    return static_cast<double>(us);
}

Bridge::Bridge(QObject *parent)
    : QObject(parent)
{
    // 设备接入层：目前只有内置模拟器。换成串口监护仪 / 网口监护仪 / DICOM 网络时，
    // 实现同一个 DeviceProvider 接口并在这里替换即可，前端契约与原生条都不用动。
    auto *provider = new SimulatorProvider(this);
    m_provider = provider;

    // Bridge 只做转发：设备发出的每个样本原样进 QWebChannel 与原生条。
    // 时间戳由 provider 在采集时刻打，转发过程不重新计时。
    // 诊断开关：QTRS_MUTE_SIGNALS=1 时不把样本转发给 QWebChannel，用于区分
    // 「渲染进程内存增长」是消息传输导致还是 Chromium 自身分配（原生条仍照常刷新）。
    if (qgetenv("QTRS_MUTE_SIGNALS") != "1") {
        // QTRS_SAMPLE_DIV=N：只把每 N 个波形样本转发一个给网页侧（诊断/降频用，
        // 用来确认渲染进程内存增长是否随消息条数等比变化；原生条不受影响）。
        const int div = qMax(1, qgetenv("QTRS_SAMPLE_DIV").toInt());
        auto *cEcg = new int(0);
        auto *cEeg = new int(0);
        auto *cEmg = new int(0);
        auto *cSpo2 = new int(0);
        connect(provider, &DeviceProvider::ecgSample, this, [this, div, cEcg](double v, double ts) {
            if (div > 1 && (++*cEcg % div) != 0) return;
            emit ecgSample(v, ts);
        });
        connect(provider, &DeviceProvider::eegSample, this, [this, div, cEeg](double v, double ts) {
            if (div > 1 && (++*cEeg % div) != 0) return;
            emit eegSample(v, ts);
        });
        connect(provider, &DeviceProvider::emgSample, this, [this, div, cEmg](double v, double ts) {
            if (div > 1 && (++*cEmg % div) != 0) return;
            emit emgSample(v, ts);
        });
        connect(provider, &DeviceProvider::spo2Sample, this, [this, div, cSpo2](double v, double ts) {
            if (div > 1 && (++*cSpo2 % div) != 0) return;
            emit spo2Sample(v, ts);
        });
        connect(provider, &DeviceProvider::vitalsReady, this, &Bridge::vitalsReady);
        connect(provider, &DeviceProvider::neuralSpike, this, &Bridge::neuralSpike);
        connect(provider, &DeviceProvider::interventionReady, this, &Bridge::interventionReady);
    }

    // 调试入口：QTRS_VOLUME_DIR=<dir> 启动后加载指定 DICOM 序列（验证窗预设用）。
    // 延后 6 s：网页侧启动时会自动加载演示样例，等它落地后再覆盖，验证才不会看错数据。
    if (const QByteArray volDir = qgetenv("QTRS_VOLUME_DIR"); !volDir.isEmpty()) {
        const QString dir = QString::fromUtf8(volDir);
        QTimer::singleShot(6000, this, [this, dir]() {
            m_authoritativeVolumeDir = dir;
            loadDicomVolume(dir);
        });
    }
    qInfo().noquote() << "[Bridge] provider:" << provider->kind() << "·" << provider->description()
                      << "· 硬件时基:" << (provider->hasHardwareClock() ? "是" : "否（宿主墙钟）");

    // 台账在 Bridge 构造时打开，会话 id 从这一刻起贯穿整次运行。
    m_audit = new AuditSink(this);
    m_audit->open();

    // 术语对照：本地台账是唯一权威；Lume 是跨端可见的事件镜像（QTRS_LUME_URL 才启用）。
    // 观察者在落盘之后调用，网络故障不会影响本地证据链。
    m_lume = new LumeSink(this);
    m_audit->setObserver([this](const QJsonObject &event) {
        if (m_lume)
            m_lume->post(event);
    });
    m_audit->record(QStringLiteral("session.start"), {
        {QStringLiteral("provider"), provider->kind()},
        {QStringLiteral("providerDescription"), provider->description()},
        {QStringLiteral("hasHardwareClock"), provider->hasHardwareClock()},
        {QStringLiteral("platform"), m_audit->filePath().isEmpty() ? QStringLiteral("unknown")
                                                                   : QSysInfo::productType()},
    });

    // 崩溃/被杀之后的重启：先回答「上次是怎么结束的」，再恢复上次看的那一例。
    // 术中重启最怕的不是空白，而是「看起来正常、但不知道之前发生了什么」。
    recoverLastSession();
    if (!m_recovery.isEmpty()) {
        const QVariantMap info = m_recovery;
        m_audit->record(QStringLiteral("session.recovered"), info);
        qWarning().noquote() << QStringLiteral("[Recovery] 上次会话未正常结束：%1（距今 %2 s，%3 条事件）")
                                    .arg(info.value(QStringLiteral("previousSession")).toString())
                                    .arg(info.value(QStringLiteral("gapSeconds")).toInt())
                                    .arg(info.value(QStringLiteral("events")).toInt());
    }
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this]() {
        if (m_audit) {
            m_audit->record(QStringLiteral("session.end"), {
                {QStringLiteral("reason"), QStringLiteral("quit")},
                {QStringLiteral("events"), m_audit->count()},
            });
        }
    });
    QTimer::singleShot(900, this, [this]() { restoreLastVolume(); });

    // QTRS_PACS=host:port:calledAE 时启动后自动做一次 C-ECHO：
    // 接入现场第一步永远是「能不能连上」，让它自己先答一次，而不是等人点按钮。
    const QByteArray pacsSpec = qgetenv("QTRS_PACS");
    if (!pacsSpec.isEmpty()) {
        PacsClient::Endpoint ep;
        QString perr;
        if (PacsClient::parseEndpoint(QString::fromLocal8Bit(pacsSpec), &ep, &perr)
            && !ep.calledAe.isEmpty()) {
            QTimer::singleShot(1200, this, [this, ep]() {
                pacsProbe(ep.host, ep.port, ep.calledAe);
            });
        } else {
            qWarning().noquote() << "[PACS] QTRS_PACS 无法解析：" << perr;
        }
    }
}

Bridge::~Bridge()
{
    // PACS 网络请求可能还卡在连接/响应超时上（最长几秒）：先置 alive=false 让回调不再碰
    // this，再 join，避免关窗时后台线程访问已释放对象。
    if (m_pacsHolder) {
        m_pacsHolder->alive = false;
        if (m_pacsHolder->thread && m_pacsHolder->thread->joinable())
            m_pacsHolder->thread->join();
    }
    if (m_heartbeatHolder) {
        m_heartbeatHolder->alive = false;
        if (m_heartbeatHolder->thread && m_heartbeatHolder->thread->joinable())
            m_heartbeatHolder->thread->join();
    }

    // 若 DICOM 解析仍在进行：先置 alive=false（位于堆上、独立于 this），
    // 再置 cancel=true 让 worker 在下一个边界退出，最后 join。
    // 已入队的回调会因 alive=false 跳过对 this 的访问，避免 Bridge 释放后的
    // use-after-free；cancel 让 join 不必等整卷解析完（否则关窗会卡住数秒）。
    if (m_dicomHolder) {
        m_dicomHolder->alive = false;
        m_dicomHolder->cancel = true;
        if (m_dicomHolder->thread && m_dicomHolder->thread->joinable())
            m_dicomHolder->thread->join();
        // 释放本副本；若仍有已入队闭包持有 holder 副本，它会存活到那些
        // 闭包执行完毕（此时 alive 已为 false，不会访问 this）。
    }
}

namespace {

/// 心跳间隔：默认 30 s，`QTRS_PACS_HEARTBEAT_MS=0` 可关闭。
/// 术中 PACS 掉线必须由自己发现，不能等下一次有人点「取回」才知道。
int pacsHeartbeatIntervalMs()
{
    const QByteArray raw = qgetenv("QTRS_PACS_HEARTBEAT_MS");
    if (raw.isEmpty()) return 30000;
    bool ok = false;
    const int ms = raw.toInt(&ok);
    if (!ok || ms < 0) return 30000;
    return ms;
}

constexpr int kPacsRetrieveMaxAttempts = 3;

/// 取回方式：`auto`（先 C-GET，不支持则回落 C-MOVE）/ `get` / `move`。
QString pacsRetrieveMode()
{
    const QByteArray raw = qgetenv("QTRS_PACS_MODE");
    const QString mode = QString::fromLocal8Bit(raw).trimmed().toLower();
    if (mode == QStringLiteral("get") || mode == QStringLiteral("move")) return mode;
    return QStringLiteral("auto");
}

/// C-MOVE 的接收端身份：PACS 的 AE 表里必须登记这套 AE/端口，否则推不过来。
QString pacsStoreAe()
{
    const QByteArray raw = qgetenv("QTRS_PACS_STORE_AE");
    return raw.isEmpty() ? QStringLiteral("QTRSSTORE") : QString::fromLocal8Bit(raw);
}

int pacsStorePort()
{
    bool ok = false;
    const int port = qgetenv("QTRS_PACS_STORE_PORT").toInt(&ok);
    return (ok && port > 0 && port <= 65535) ? port : 11113;
}

/// 服务端明确不会做 C-GET 的几种表现。C-MOVE 是这时唯一的退路，
/// 重试同一个 C-GET 只会把同样的拒绝再走三遍。
bool getLooksUnsupported(const QString &err)
{
    const QString lower = err.toLower();
    return lower.contains(QStringLiteral("scp role"))
        || lower.contains(QStringLiteral("presentation context"))
        || lower.contains(QStringLiteral("部分失败"))
        || lower.contains(QStringLiteral("0x0122"));
}

} // namespace

void Bridge::pacsProbe(const QString &host, int port, const QString &calledAe)
{
    probePacs(host, port, calledAe, false);
}

void Bridge::probePacs(const QString &host, int port, const QString &calledAe, bool heartbeat)
{
    // 心跳与取回各走各的关联：取回可能持续数秒，掉线检测不能跟着停。
    if (heartbeat ? m_heartbeatBusy : m_pacsBusy) {
        qInfo() << "[PACS] 上一次请求尚未返回，跳过本次探测";
        return;
    }
    if (heartbeat) m_heartbeatBusy = true;
    else m_pacsBusy = true;

    m_pacsHost = host;
    m_pacsPort = port;
    m_pacsAe = calledAe;

    PacsClient::Endpoint ep;
    ep.host = host;
    ep.port = port;
    ep.calledAe = calledAe;

    auto holder = std::make_shared<PacsThreadHolder>();
    holder->alive = true;
    if (heartbeat) m_heartbeatHolder = holder;
    else m_pacsHolder = holder;

    if (!heartbeat) {
        qInfo().noquote() << QStringLiteral("[PACS] C-ECHO → %1:%2 AE=%3")
                                 .arg(host).arg(port).arg(calledAe);
    }
    holder->thread = std::make_unique<std::thread>([this, holder, ep, heartbeat]() {
        QString err;
        int roundTripMs = -1;
        const bool ok = PacsClient::echo(ep, &err, &roundTripMs);

        QMetaObject::invokeMethod(this, [this, holder, ep, ok, err, roundTripMs, heartbeat]() {
            if (!holder->alive) return;
            if (heartbeat) {
                m_heartbeatBusy = false;
                if (m_heartbeatHolder == holder) m_heartbeatHolder.reset();
            } else {
                m_pacsBusy = false;
            }

            const bool hadState = m_pacsProbed;
            const bool wasOnline = m_pacsOnline;
            m_pacsProbed = true;
            m_pacsOnline = ok;

            QVariantMap status;
            status.insert(QStringLiteral("ok"), ok);
            status.insert(QStringLiteral("host"), ep.host);
            status.insert(QStringLiteral("port"), ep.port);
            status.insert(QStringLiteral("calledAe"), ep.calledAe);
            status.insert(QStringLiteral("roundTripMs"), roundTripMs);
            status.insert(QStringLiteral("message"), humanPacsError(err));
            status.insert(QStringLiteral("detail"), err);
            status.insert(QStringLiteral("heartbeat"), heartbeat);
            status.insert(QStringLiteral("tsUs"), nowMicros());
            // 心跳不重新枚举，病例数沿用上一次结果，别在看板上闪成 0。
            const QVariant count = m_pacsState.value(QStringLiteral("count"));
            if (count.isValid()) status.insert(QStringLiteral("count"), count);

            if (!heartbeat) {
                if (ok) {
                    qInfo().noquote() << QStringLiteral("[PACS] C-ECHO 成功：%1 ms").arg(roundTripMs);
                } else {
                    qWarning().noquote() << "[PACS] C-ECHO 失败：" << err;
                }
                if (m_audit) {
                    m_audit->record(ok ? QStringLiteral("pacs.echo")
                                       : QStringLiteral("pacs.echoFailed"), {
                        {QStringLiteral("host"), ep.host},
                        {QStringLiteral("port"), ep.port},
                        {QStringLiteral("calledAe"), ep.calledAe},
                        {QStringLiteral("roundTripMs"), roundTripMs},
                        {QStringLiteral("message"), err},
                    });
                }
            } else if (!hadState || wasOnline != ok) {
                // 心跳只在「翻转」时留痕：接通 / 掉线各一行，稳态不写。
                if (ok) {
                    qInfo().noquote() << QStringLiteral("[PACS] 心跳恢复：%1 ms").arg(roundTripMs);
                } else {
                    qWarning().noquote() << "[PACS] 心跳失败：" << err;
                }
                if (m_audit) {
                    m_audit->record(ok ? QStringLiteral("pacs.online")
                                       : QStringLiteral("pacs.offline"), {
                        {QStringLiteral("host"), ep.host},
                        {QStringLiteral("port"), ep.port},
                        {QStringLiteral("calledAe"), ep.calledAe},
                        {QStringLiteral("roundTripMs"), roundTripMs},
                        {QStringLiteral("message"), err},
                    });
                }
            }

            m_pacsState = status;

            // 心跳若每 30 s 推一次状态，前端会把「已连通 N ms」整行刷新；
            // 只在翻转时推，稳态的往返毫秒变化不值得打扰。
            if (!heartbeat || !hadState || wasOnline != ok) {
                emit pacsStatus(status);
            }

            // 连通性确认后立刻枚举一次：接入现场的真实顺序就是「先 echo 再找病人」，
            // 分两步手动触发只会让人忘记第二步。
            if (ok && !heartbeat) {
                pacsFindStudies(ep.host, ep.port, ep.calledAe);
            }
        });
    });

    // 只要配了端点就把心跳挂上（失败也挂：掉线后要能自己发现恢复）。
    if (!heartbeat && m_pacsHeartbeat == nullptr) {
        const int interval = pacsHeartbeatIntervalMs();
        if (interval > 0) {
            m_pacsHeartbeat = new QTimer(this);
            m_pacsHeartbeat->setInterval(interval);
            connect(m_pacsHeartbeat, &QTimer::timeout, this, [this]() {
                if (m_pacsPort > 0) probePacs(m_pacsHost, m_pacsPort, m_pacsAe, true);
            });
            m_pacsHeartbeat->start();
        }
    }
}

void Bridge::pacsRetrieveStudy(const QString &host, int port, const QString &calledAe,
                               const QString &studyUid)
{
    if (m_pacsBusy) {
        qInfo() << "[PACS] 上一次请求尚未返回，跳过本次取回";
        return;
    }

    m_pacsHost = host;
    m_pacsPort = port;
    m_pacsAe = calledAe;

    // 路径里的点号是 DICOM UID 的一部分，文件系统允许。
    const QString destDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                            + QStringLiteral("/pacs/") + studyUid;

    // auto 先试 C-GET：它不需要 PACS 侧登记任何 AE，能用就是最省事的一条。
    const QString mode = pacsRetrieveMode();
    const QString method = (mode == QStringLiteral("move")) ? QStringLiteral("move")
                                                            : QStringLiteral("get");
    startRetrieveAttempt(host, port, calledAe, studyUid, destDir, method, 1);
}

void Bridge::startRetrieveAttempt(const QString &host, int port, const QString &calledAe,
                                  const QString &studyUid, const QString &destDir,
                                  const QString &method, int attempt)
{
    if (m_pacsBusy) {
        qInfo() << "[PACS] 上一次请求尚未返回，本次取回尝试推迟";
        QTimer::singleShot(500, this, [this, host, port, calledAe, studyUid, destDir, method,
                                       attempt]() {
            startRetrieveAttempt(host, port, calledAe, studyUid, destDir, method, attempt);
        });
        return;
    }
    m_pacsBusy = true;

    PacsClient::Endpoint ep;
    ep.host = host;
    ep.port = port;
    ep.calledAe = calledAe;
    // C-GET 会连续收几十到几百个对象，超时按单次 DIMSE 计而不是整个检索，
    // 给 15 s 容错；真断流时由服务端最终响应失败暴露出来。
    ep.timeoutMs = 15000;

    auto holder = std::make_shared<PacsThreadHolder>();
    holder->alive = true;
    m_pacsHolder = holder;

    const QString verb = method == QStringLiteral("move") ? QStringLiteral("C-MOVE")
                                                         : QStringLiteral("C-GET");
    qInfo().noquote() << QStringLiteral("[PACS] %1 %2 → %3:%4 AE=%5（第 %6/%7 次）")
                             .arg(verb)
                             .arg(studyUid.left(24) + QStringLiteral("…"))
                             .arg(host).arg(port).arg(calledAe)
                             .arg(attempt).arg(kPacsRetrieveMaxAttempts);
    holder->thread = std::make_unique<std::thread>([this, holder, ep, studyUid, destDir, method,
                                                    verb, attempt]() {
        QElapsedTimer timer;
        timer.start();
        QString err;
        int files = 0;
        bool ok = false;
        if (method == QStringLiteral("move")) {
            ok = PacsClient::moveStudy(ep, studyUid, pacsStoreAe(), pacsStorePort(), destDir,
                                       &files, &err);
        } else {
            ok = PacsClient::retrieveStudy(ep, studyUid, destDir, &files, &err);
        }
        const int ms = static_cast<int>(timer.elapsed());

        QMetaObject::invokeMethod(this, [this, holder, ep, studyUid, destDir, ok, files, err, ms,
                                          method, verb, attempt]() {
            if (!holder->alive) return;
            m_pacsBusy = false;

            // 服务端不做 C-GET 时，退路是 C-MOVE（前提是它在 AE 表里认得我们的接收端）。
            const bool fallbackToMove = !ok && method != QStringLiteral("move")
                                        && pacsRetrieveMode() != QStringLiteral("get")
                                        && getLooksUnsupported(err);
            const bool willRetry = !ok && !fallbackToMove
                                   && attempt < kPacsRetrieveMaxAttempts
                                   && PacsClient::isRetryableError(err);

            QVariantMap status;
            status.insert(QStringLiteral("ok"), ok);
            status.insert(QStringLiteral("studyUid"), studyUid);
            status.insert(QStringLiteral("files"), files);
            status.insert(QStringLiteral("dir"), destDir);
            status.insert(QStringLiteral("ms"), ms);
            status.insert(QStringLiteral("method"), method);
            status.insert(QStringLiteral("attempt"), attempt);
            status.insert(QStringLiteral("maxAttempts"), kPacsRetrieveMaxAttempts);
            status.insert(QStringLiteral("retrying"), willRetry || fallbackToMove);
            status.insert(QStringLiteral("message"),
                          fallbackToMove
                              ? QStringLiteral("C-GET 不被支持，改用 C-MOVE：%1")
                                    .arg(humanPacsError(err))
                              : willRetry ? QStringLiteral("取回失败，正在重试（第 %1 次）：%2")
                                                .arg(attempt + 1).arg(humanPacsError(err))
                                          : humanPacsError(err));
            status.insert(QStringLiteral("detail"), err);
            status.insert(QStringLiteral("tsUs"), nowMicros());

            if (ok) {
                qInfo().noquote() << QStringLiteral("[PACS] %1 完成：%2 个对象，%3 ms → %4")
                                         .arg(verb).arg(files).arg(ms).arg(destDir);
                if (m_audit) {
                    m_audit->record(QStringLiteral("pacs.retrieve"), {
                        {QStringLiteral("studyUid"), studyUid},
                        {QStringLiteral("files"), files},
                        {QStringLiteral("ms"), ms},
                        {QStringLiteral("method"), method},
                        {QStringLiteral("attempt"), attempt},
                        {QStringLiteral("dir"), destDir},
                    });
                }
                // 标记为权威来源，并清掉仍在排队的样例请求
                m_authoritativeVolumeDir = destDir;
                m_pendingDicomDir.clear();
                // 取回的是一堆 DICOM 文件而不是体数据：交给既有解析链路，
                // 走的是和本地样例完全同一条路径（含重采样与 app:// 二进制供数）。
                loadDicomVolume(destDir);
            } else if (fallbackToMove) {
                qWarning().noquote() << QStringLiteral("[PACS] C-GET 不被支持，回落 C-MOVE（AE=%1:%2）：")
                                            .arg(pacsStoreAe()).arg(pacsStorePort())
                                     << err;
                if (m_audit) {
                    m_audit->record(QStringLiteral("pacs.retrieveFallback"), {
                        {QStringLiteral("studyUid"), studyUid},
                        {QStringLiteral("from"), QStringLiteral("get")},
                        {QStringLiteral("to"), QStringLiteral("move")},
                        {QStringLiteral("storeAe"), pacsStoreAe()},
                        {QStringLiteral("storePort"), pacsStorePort()},
                        {QStringLiteral("message"), err},
                    });
                }
                QTimer::singleShot(300, this, [this, ep, studyUid, destDir, attempt]() {
                    startRetrieveAttempt(ep.host, ep.port, ep.calledAe, studyUid, destDir,
                                         QStringLiteral("move"), attempt);
                });
            } else if (willRetry) {
                // 退避 0.6 s / 1.8 s：术中网络抖动通常是亚秒级，
                // 重试太快只会撞在同一段抖动上，太慢则让等待变得不可忍受。
                const int backoffMs = attempt == 1 ? 600 : 1800;
                qWarning().noquote() << QStringLiteral("[PACS] %1 第 %2 次失败（%3 ms），%4 ms 后重试：")
                                            .arg(verb).arg(attempt).arg(ms).arg(backoffMs)
                                     << err;
                if (m_audit) {
                    m_audit->record(QStringLiteral("pacs.retrieveRetry"), {
                        {QStringLiteral("studyUid"), studyUid},
                        {QStringLiteral("method"), method},
                        {QStringLiteral("attempt"), attempt},
                        {QStringLiteral("backoffMs"), backoffMs},
                        {QStringLiteral("message"), err},
                    });
                }
                QTimer::singleShot(backoffMs, this, [this, ep, studyUid, destDir, method,
                                                     attempt]() {
                    startRetrieveAttempt(ep.host, ep.port, ep.calledAe, studyUid, destDir, method,
                                         attempt + 1);
                });
            } else {
                qWarning().noquote() << QStringLiteral("[PACS] %1 失败：").arg(verb) << err;
                if (m_audit) {
                    m_audit->record(QStringLiteral("pacs.retrieveFailed"), {
                        {QStringLiteral("studyUid"), studyUid},
                        {QStringLiteral("ms"), ms},
                        {QStringLiteral("method"), method},
                        {QStringLiteral("attempt"), attempt},
                        {QStringLiteral("message"), err},
                    });
                }
            }
            emit pacsRetrieve(status);
        });
    });
}

QVariantMap Bridge::pacsState() const
{
    return m_pacsState;
}

void Bridge::pacsFindStudies(const QString &host, int port, const QString &calledAe)
{
    if (m_pacsBusy) {
        qInfo() << "[PACS] 上一次请求尚未返回，跳过本次查询";
        return;
    }
    m_pacsBusy = true;

    PacsClient::Endpoint ep;
    ep.host = host;
    ep.port = port;
    ep.calledAe = calledAe;

    auto holder = std::make_shared<PacsThreadHolder>();
    holder->alive = true;
    m_pacsHolder = holder;

    qInfo().noquote() << QStringLiteral("[PACS] C-FIND(STUDY) → %1:%2 AE=%3")
                             .arg(host).arg(port).arg(calledAe);
    holder->thread = std::make_unique<std::thread>([this, holder, ep]() {
        QString err;
        QList<QVariantMap> studies;
        const bool ok = PacsClient::findStudies(ep, &studies, &err);

        QMetaObject::invokeMethod(this, [this, holder, ep, ok, studies, err]() {
            if (!holder->alive) return;
            m_pacsBusy = false;

            QVariantList list;
            for (const QVariantMap &s : studies) list.append(s);

            QVariantMap meta;
            meta.insert(QStringLiteral("ok"), ok);
            meta.insert(QStringLiteral("count"), list.size());
            meta.insert(QStringLiteral("host"), ep.host);
            meta.insert(QStringLiteral("calledAe"), ep.calledAe);
            meta.insert(QStringLiteral("message"), humanPacsError(err));
            meta.insert(QStringLiteral("detail"), err);
            meta.insert(QStringLiteral("tsUs"), nowMicros());

            qInfo().noquote() << QStringLiteral("[PACS] C-FIND 返回 %1 例%2")
                                     .arg(list.size())
                                     .arg(ok ? QString() : QStringLiteral("（失败：%1）").arg(err));
            if (m_audit) {
                m_audit->record(ok ? QStringLiteral("pacs.find")
                                   : QStringLiteral("pacs.findFailed"), {
                    {QStringLiteral("host"), ep.host},
                    {QStringLiteral("calledAe"), ep.calledAe},
                    {QStringLiteral("studies"), list.size()},
                    {QStringLiteral("message"), err},
                });
            }
            m_pacsState.insert(QStringLiteral("count"), list.size());
            // 整份列表进快照：前端刷新或晚订阅时仍能在多例之间切换，
            // 而不是只认「第一例」。
            m_pacsState.insert(QStringLiteral("studies"), list);
            // 第一例的 StudyInstanceUID 一并进快照：取回按钮依赖它，
            // 前端晚订阅就会拿不到 studies 信号。
            m_pacsState.insert(QStringLiteral("studyUid"),
                               list.isEmpty() ? QString()
                                              : list.first().toMap()
                                                    .value(QStringLiteral("studyInstanceUid"))
                                                    .toString());
            m_pacsState.insert(QStringLiteral("ok"), ok);
            m_pacsState.insert(QStringLiteral("message"), err);
            emit pacsStudies(list, meta);

            // 自动取回开关：现场联调时希望「连上就看到」而不是先点按钮，
            // QTRS_PACS_AUTO_GET=1 让 C-FIND 命中第一例后直接 C-GET。
            // 取回开关：`1` 命中即取；给毫秒数则延时取回（现场可用它模拟
            // 「C-FIND 之后网络抖了一下」，也让重试与退避有可复现的验证路径）。
            const QByteArray autoGet = qgetenv("QTRS_PACS_AUTO_GET");
            if (!autoGet.isEmpty() && autoGet != "0" && !list.isEmpty()) {
                const QString uid = list.first().toMap()
                                        .value(QStringLiteral("studyInstanceUid")).toString();
                if (!uid.isEmpty()) {
                    bool numeric = false;
                    int delayMs = autoGet.toInt(&numeric);
                    if (!numeric || delayMs <= 1) delayMs = 0;
                    QTimer::singleShot(delayMs, this, [this, ep, uid]() {
                        pacsRetrieveStudy(ep.host, ep.port, ep.calledAe, uid);
                    });
                }
            }
        });
    });
}

void Bridge::logAuditEvent(const QVariantMap &event)
{
    if (!m_audit) return;
    const QString type = event.value(QStringLiteral("type")).toString();
    if (type.isEmpty()) {
        qWarning() << "[Audit] 前端事件缺少 type，已忽略";
        return;
    }
    QVariantMap fields = event;
    fields.remove(QStringLiteral("type"));
    m_audit->record(type, fields);
}

QVariantMap Bridge::queryAuditEvents(const QVariantMap &filter)
{
    AuditFilter f;
    f.typePrefix = filter.value(QStringLiteral("type")).toString().trimmed();
    f.text = filter.value(QStringLiteral("text")).toString().trimmed();
    f.limit = clampAuditLimit(filter.value(QStringLiteral("limit")).toInt());

    // 分片按文件名正序读（audit-YYYYMMDD.jsonl 在前、.1 轮转副本在后），
    // 读进来后再按时间排序——文件名顺序不等于事件顺序，"先读的"不能当成"旧的"。
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                        + QStringLiteral("/audit");
    QStringList files = QDir(dir).entryList({QStringLiteral("audit-*.jsonl*")}, QDir::Files, QDir::Name);

    constexpr int kScanBudget = 5000;
    QJsonArray scannedEvents;   // 旧→新，供 filterAuditEvents 取尾部
    int scanned = 0;
    bool truncated = false;
    for (const QString &name : files) {
        QFile file(QStringLiteral("%1/%2").arg(dir, name));
        if (!file.open(QIODevice::ReadOnly)) continue;
        // 一次读整份再按行切：文件被轮转限制在几十 MB 内，逐行 readLine 在慢盘上更吃亏。
        const QByteArray blob = file.readAll();
        const QList<QByteArray> lines = blob.split('\n');
        QJsonArray dayEvents;
        for (const QByteArray &raw : lines) {
            const QByteArray line = raw.trimmed();
            if (line.isEmpty()) continue;
            const QJsonObject ev = QJsonDocument::fromJson(line).object();
            if (ev.isEmpty()) continue;   // 崩溃时可能留下半行，跳过而不是让整次查询失败
            dayEvents.append(ev);
            ++scanned;
        }
        // 单文件内保持文件顺序（即时间顺序），整体按「天」从旧到新拼接。
        const int before = static_cast<int>(scannedEvents.size());
        for (const QJsonValue &v : dayEvents) scannedEvents.append(v);
        if (scanned >= kScanBudget) {
            truncated = static_cast<int>(scannedEvents.size()) > before || files.size() > 1;
            break;
        }
    }

    // 事件自带 ISO 时间戳：按 ts（不足则按 tsUs）排一次，避免文件分片顺序与轮转
    // 副本把「最新 8 条」变成「最旧 8 条」（实测踩到：默认视图停在一早的会话）。
    QList<QJsonObject> ordered;
    ordered.reserve(scannedEvents.size());
    for (const QJsonValue &v : scannedEvents) ordered.append(v.toObject());
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const QJsonObject &a, const QJsonObject &b) {
                         const QString ta = a.value(QStringLiteral("ts")).toString();
                         const QString tb = b.value(QStringLiteral("ts")).toString();
                         if (ta != tb) return ta < tb;
                         return a.value(QStringLiteral("tsUs")).toDouble()
                                < b.value(QStringLiteral("tsUs")).toDouble();
                     });
    QJsonArray orderedArray;
    for (const QJsonObject &ev : ordered) orderedArray.append(ev);
    const QJsonArray hits = filterAuditEvents(orderedArray, f);
    qInfo().noquote() << QStringLiteral("[Audit] 查询 type=%1 text=%2 扫描 %3 条 → 命中 %4 条")
                             .arg(f.typePrefix.isEmpty() ? QStringLiteral("*") : f.typePrefix,
                                  f.text.isEmpty() ? QStringLiteral("*") : f.text)
                             .arg(scanned)
                             .arg(hits.size());

    QVariantMap out;
    out.insert(QStringLiteral("events"), hits.toVariantList());
    out.insert(QStringLiteral("scanned"), scanned);
    out.insert(QStringLiteral("returned"), hits.size());
    out.insert(QStringLiteral("truncated"), truncated);
    out.insert(QStringLiteral("session"), m_audit ? m_audit->sessionId() : QString());
    return out;
}

QVariantMap Bridge::providerInfo() const
{
    QVariantMap info;
    if (!m_provider) {
        info.insert(QStringLiteral("kind"), QStringLiteral("none"));
        info.insert(QStringLiteral("description"), QStringLiteral("无数据源"));
        info.insert(QStringLiteral("hasHardwareClock"), false);
        return info;
    }
    info.insert(QStringLiteral("kind"), m_provider->kind());
    info.insert(QStringLiteral("description"), m_provider->description());
    info.insert(QStringLiteral("hasHardwareClock"), m_provider->hasHardwareClock());
    return info;
}

QVariantMap Bridge::lastRecovery() const
{
    return m_recovery;
}

QString Bridge::stateFilePath() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                        + QStringLiteral("/state");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/last-volume.json");
}

void Bridge::saveVolumeState(const QString &directory)
{
    // 样例目录不记：下次启动仍会走样例自动加载，记下来只会让状态文件说谎。
    if (directory.isEmpty() || directory == QStringLiteral(DICOM_SAMPLE_PATH)) return;
    QJsonObject obj;
    obj.insert(QStringLiteral("dir"), directory);
    obj.insert(QStringLiteral("tsUs"), nowMicros());
    QFile f(stateFilePath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning().noquote() << "[Recovery] 无法写入状态文件：" << f.errorString();
        return;
    }
    f.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void Bridge::restoreLastVolume()
{
    if (qEnvironmentVariableIsSet("QTRS_NO_RESTORE")) return;
    QFile f(stateFilePath());
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
    const QString dir = obj.value(QStringLiteral("dir")).toString();
    if (dir.isEmpty() || !QDir(dir).exists()) {
        qInfo().noquote() << "[Recovery] 上次的体数据目录已不存在，跳过恢复：" << dir;
        return;
    }
    qInfo().noquote() << "[Recovery] 恢复上次体数据来源：" << dir;
    if (m_audit) {
        m_audit->record(QStringLiteral("volume.restore"), {
            {QStringLiteral("dir"), dir},
            {QStringLiteral("savedAtUs"), obj.value(QStringLiteral("tsUs")).toDouble()},
        });
    }
    // 恢复出来的真实数据同样是权威来源：不能被几百毫秒后的样例自动加载覆盖。
    // 没有这一行，重启后原生面板会先画出上次那一例，随即被演示样例顶掉。
    m_authoritativeVolumeDir = dir;
    loadDicomVolume(dir);
}

void Bridge::recoverLastSession()
{
    // 台账按天切分：崩溃可能跨零点，因此今天与昨天都要看。
    const QString auditDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                             + QStringLiteral("/audit");
    const QDate today = QDate::currentDate();
    QStringList files;
    for (int back = 1; back >= 0; --back) {
        files << QStringLiteral("%1/audit-%2.jsonl")
                     .arg(auditDir, today.addDays(-back).toString(QStringLiteral("yyyyMMdd")));
    }

    QMap<QString, QJsonObject> starts;   // session -> session.start 事件
    QSet<QString> ended;
    QMap<QString, QJsonObject> lastOf;   // session -> 最后一条事件
    QMap<QString, int> counts;           // session -> 事件条数
    for (const QString &path : files) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) continue;
        while (!f.atEnd()) {
            const QByteArray line = f.readLine().trimmed();
            if (line.isEmpty()) continue;
            const QJsonObject ev = QJsonDocument::fromJson(line).object();
            const QString type = ev.value(QStringLiteral("type")).toString();
            const QString session = ev.value(QStringLiteral("session")).toString();
            if (session.isEmpty()) continue;
            lastOf.insert(session, ev);
            counts[session] += 1;
            if (type == QStringLiteral("session.start")) starts.insert(session, ev);
            else if (type == QStringLiteral("session.end")) ended.insert(session);
        }
    }

    // 只看「最近一次运行」：它没写 session.end 才算异常结束。
    // 更早的未闭合会话是历史，已经在当时的重启里报过；否则告警会一直挂着不走。
    const QString current = m_audit ? m_audit->sessionId() : QString();
    QString latest;
    double latestTs = 0.0;
    for (auto it = starts.cbegin(); it != starts.cend(); ++it) {
        if (it.key() == current) continue;
        const double ts = it.value().value(QStringLiteral("tsUs")).toDouble();
        if (ts > latestTs) {
            latestTs = ts;
            latest = it.key();
        }
    }
    if (latest.isEmpty() || ended.contains(latest)) return;
    const QString candidate = latest;

    const double lastTs = lastOf.value(candidate).value(QStringLiteral("tsUs")).toDouble();
    m_recovery.insert(QStringLiteral("previousSession"), candidate);
    m_recovery.insert(QStringLiteral("previousStart"),
                      starts.value(candidate).value(QStringLiteral("ts")).toString());
    m_recovery.insert(QStringLiteral("previousLastEvent"),
                      lastOf.value(candidate).value(QStringLiteral("type")).toString());
    m_recovery.insert(QStringLiteral("tsUs"), lastTs);
    m_recovery.insert(QStringLiteral("events"), counts.value(candidate));
    m_recovery.insert(QStringLiteral("gapSeconds"),
                      static_cast<int>(qRound((nowMicros() - lastTs) / 1000000.0)));
}

void Bridge::reportNativeFrame(int ms)
{
    m_lastNativeFrameMs = ms;
    emit volumeNativeFirstFrame(ms);
}

QVariantMap Bridge::lastVolumeTiming() const
{
    QVariantMap m;
    if (m_lastParseMs >= 0) m.insert(QStringLiteral("parseMs"), m_lastParseMs);
    if (m_lastNativeFrameMs >= 0) m.insert(QStringLiteral("firstFrameMs"), m_lastNativeFrameMs);
    return m;
}

void Bridge::loadDicomVolume(const QString &directory)
{
    // 重入保护：只允许一个在途 DICOM 解析（UI 实际只触发一次，此处仅为
    // 健壮性兜底；如需排队可改为任务队列实现）。
    if (m_dicomHolder) {
        // 不丢请求：记住最后一个待解析目录，当前解析结束再接手。
        // 术中场景里「取回的新数据被丢掉」比慢一拍更糟。
        qInfo().noquote() << "[Bridge] DICOM 解析已在进行中，排队后续请求：" << directory;
        m_pendingDicomDir = directory;
        return;
    }

    // PACS 取回的检查是真实病人数据，样例只是演示：一旦后者已经占据画面，
    // 样例的自动加载必须让位，否则取回成功几秒后又被演示数据覆盖。
    if (!m_authoritativeVolumeDir.isEmpty()
        && directory == QStringLiteral(DICOM_SAMPLE_PATH)) {
        qInfo().noquote() << "[Bridge] 当前体数据来自 PACS 取回，忽略样例自动加载："
                          << directory;
        return;
    }

    m_rssBeforeLoad = residentMb();
    qInfo().noquote() << "[Bridge] 开始后台解析 DICOM：" << directory;
    m_volumeParseTimer.start();

    // holder 位于堆上，worker 闭包与收尾闭包各持一份 shared_ptr 副本：
    //  - 收尾闭包仅操作 holder->thread，绝不读成员指针，因此不会被之后
    //    新建的线程误伤（修 #7 竞态）；
    //  - alive 标志独立于 Bridge，Bridge 析构后仍可被已入队回调安全读取。
    auto holder = std::make_shared<DicomThreadHolder>();
    holder->alive = true;
    holder->thread = std::make_unique<std::thread>([this, directory, holder]() {
        QByteArray data;
        QByteArray huData;
        QVariantMap meta;
        QString err;
        // 0 = 自动网格（最长边，128–256）
        const bool ok = DicomReader::loadVolume(directory, data, meta, err, 0,
                                              &holder->cancel, &huData);

        // 结果必须回到 Qt 主线程再发射信号（QWebChannel 依赖所在线程）。
        // 闭包通过 holder->alive 判断 Bridge 是否仍存活，避免析构后
        // 对已释放 this 的访问（use-after-free）。
        if (ok) {
            // 事件型通道不能用「发布时刻」当采集时刻：体积是一次性解析，
            // 前端要的是「这次解析花了多久」，不是「距上次发布过了多久」。
            m_lastParseMs = static_cast<int>(m_volumeParseTimer.elapsed());
            meta.insert(QStringLiteral("tsUs"), nowMicros());
            meta.insert(QStringLiteral("parseMs"), m_lastParseMs);
            QMetaObject::invokeMethod(this, [this, holder, meta, data, huData]() {
                if (!holder->alive) return;
                // 体积数据存进成员，由 AppSchemeHandler 以二进制供数；
                // 信号只传 URL，避免 2MB 二进制走文本序列化。
                m_volumeBytes = data;
                m_volumeHuBytes = huData;
                if (m_audit) m_audit->record(QStringLiteral("dicom.volumeReady"), {
                    {QStringLiteral("source"), meta.value(QStringLiteral("source"))},
                    {QStringLiteral("bytes"), static_cast<double>(data.size())},
                    {QStringLiteral("dims"), meta.value(QStringLiteral("dims"))},
                });
                // 记住来源：进程被杀后重启，先把上次这一例恢复出来
                saveVolumeState(meta.value(QStringLiteral("source")).toString());
                emit dicomVolumeReady(meta, QStringLiteral("app://app/__volume/current.bin"));

                // 交接完成后释放只在「传递过程」里需要的 CPU 副本：
                //  - 8-bit：只有网页侧 Volume3D 需要；原生面板接管时它是纯浪费
                //    （256³ = 16 MB 常驻）；
                //  - int16 HU：只给原生建纹理，建完就该放手（256³ = 32 MB）。
                // 用 QTRS_KEEP_VOLUME_BYTES=1 可保留两者做 RSS 对照实验。
                const int webBytes = static_cast<int>(data.size());
                const int huBytes = static_cast<int>(huData.size());
                const bool keepWeb = !nativeVolume() || qEnvironmentVariableIsSet("QTRS_KEEP_VOLUME_BYTES");
                if (!keepWeb) m_volumeBytes = QByteArray();
                if (!qEnvironmentVariableIsSet("QTRS_KEEP_VOLUME_BYTES")) m_volumeHuBytes = QByteArray();
                // 这里的 lambda 自己还捕获着 data/huData：在函数内量驻留只能量到
                // 「引用还没放掉」的假值。等这次交接彻底返回、网页侧也度过启动高峰
                // （实测 300 ms 时进程正被 WebEngine 首屏抬到 700 MB+）后再读稳态。
                QTimer::singleShot(60000, this, [this, webBytes, huBytes, keepWeb]() {
                    // data/huData 到这一刻早已析构；这里再请 malloc 把空页还给系统。
                    if (!keepWeb || !qEnvironmentVariableIsSet("QTRS_KEEP_VOLUME_BYTES")) releaseFreePages();
                    const double rssAfter = residentMb();
                    const double rssBefore = m_rssBeforeLoad;
                    if (m_audit) {
                        m_audit->record(QStringLiteral("volume.memory"), {
                            {QStringLiteral("bytes8"), static_cast<double>(webBytes)},
                            {QStringLiteral("bytesHu"), static_cast<double>(huBytes)},
                            {QStringLiteral("keptWeb"), keepWeb},
                            {QStringLiteral("rssBeforeMb"), rssBefore},
                            {QStringLiteral("rssAfterMb"), rssAfter},
                        });
                    }
                    qInfo().noquote() << QStringLiteral("[Volume] 体数据内存稳态：解析前 %1 MB → 60s 后 %2 MB（8-bit %3 KB / HU %4 KB，网页侧%5）")
                                             .arg(rssBefore, 0, 'f', 1)
                                             .arg(rssAfter, 0, 'f', 1)
                                             .arg(webBytes / 1024)
                                             .arg(huBytes / 1024)
                                             .arg(keepWeb ? QStringLiteral("保留") : QStringLiteral("不需要"));
                });
            });
        } else {
            QMetaObject::invokeMethod(this, [this, holder, err]() {
                if (holder->alive) {
                    if (m_audit) m_audit->record(QStringLiteral("dicom.volumeError"), {
                        {QStringLiteral("message"), err},
                    });
                    emit dicomVolumeError(err);
                }
            });
        }

        // 回到主线程收尾：仅 join/释放“本线程”对象（holder->thread），
        // 不读成员指针，因此即便期间又发起了新的 loadDicomVolume，
        // 也不会误伤新线程。
        QMetaObject::invokeMethod(this, [this, holder]() {
            if (!holder->alive) return;   // Bridge 已析构，跳过（不访问 this）
            if (holder->thread && holder->thread->joinable())
                holder->thread->join();
            if (m_dicomHolder == holder)
                m_dicomHolder.reset();

            // 收尾后立刻接手等待中的目录（例如 PACS 取回结果）
            if (!m_pendingDicomDir.isEmpty()) {
                const QString next = m_pendingDicomDir;
                m_pendingDicomDir.clear();
                loadDicomVolume(next);
            }
        });
    });

    m_dicomHolder = holder;
}

QString Bridge::appName() const
{
    return QStringLiteral("qt-react-shell");
}

bool Bridge::nativeVolume() const
{
    return qgetenv("QTRS_NATIVE_VOLUME") != "0";
}

QString Bridge::platform() const
{
    return QSysInfo::prettyProductName();
}

QString Bridge::getPlatformInfo() const
{
    return QStringLiteral("%1 — %2 (Qt %3)")
        .arg(platform(),
             QSysInfo::currentCpuArchitecture(),
             QString::fromLatin1(qVersion()));
}

QByteArray Bridge::volumeHuBytes() const
{
    return m_volumeHuBytes;
}

QByteArray Bridge::volumeBytes() const
{
    return m_volumeBytes;
}

QString Bridge::dicomSamplePath() const
{
    return QStringLiteral(DICOM_SAMPLE_PATH);
}

QString Bridge::showMessage(const QString &message)
{
    ++m_callCount;
    qInfo() << "[Bridge] showMessage from JS:" << message;

    // 演示 JS → C++ 返回值（C++ 可加工后回传）
    return QStringLiteral("C++ 已收到（第 %1 次调用）: %2").arg(m_callCount).arg(message);
}

int Bridge::add(int a, int b)
{
    return a + b;
}

void Bridge::setDataFromCpp(const QVariant &data)
{
    qInfo() << "[Bridge] setDataFromCpp from JS:" << data;

    // 回推一个带时间戳的数据，演示 C++ → JS 主动推送
    QVariantMap payload;
    payload.insert(QStringLiteral("time"), QDateTime::currentDateTime().toString(Qt::ISODate));
    payload.insert(QStringLiteral("echo"), data);
    payload.insert(QStringLiteral("seq"), m_callCount);
    emit dataFromCpp(payload);
}

// ---------------------------------------------------------------------------
// 医疗影像实时数据模拟
// ---------------------------------------------------------------------------

void Bridge::startMedicalSimulation()
{
    if (!m_provider) return;
    if (m_provider->startMedical()) {
        qInfo().noquote() << "[Bridge] 启动医疗数据源:" << m_provider->description();
        if (m_audit) m_audit->record(QStringLiteral("provider.start"), {
            {QStringLiteral("provider"), m_provider->kind()},
            {QStringLiteral("group"), QStringLiteral("medical")},
        });
    }
}

void Bridge::stopMedicalSimulation()
{
    if (!m_provider) return;
    m_provider->stopMedical();
    qInfo() << "[Bridge] 停止医疗数据源";
}

// ---------------------------------------------------------------------------
// 神经内科介入数据模拟
// ---------------------------------------------------------------------------

void Bridge::startNeurologySimulation()
{
    if (!m_provider) return;
    if (m_provider->startNeurology()) {
        qInfo().noquote() << "[Bridge] 启动神经内科数据源:" << m_provider->description();
        if (m_audit) m_audit->record(QStringLiteral("provider.start"), {
            {QStringLiteral("provider"), m_provider->kind()},
            {QStringLiteral("group"), QStringLiteral("neurology")},
        });
    }
}

void Bridge::stopNeurologySimulation()
{
    if (!m_provider) return;
    m_provider->stopNeurology();
    qInfo() << "[Bridge] 停止神经内科数据源";
}

