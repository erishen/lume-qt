#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <memory>
#include <thread>

// DICOM 后台加载线程的持有结构（堆分配、由 shared_ptr 共享）。
//  - thread：后台解析线程（unique_ptr 保证单一所有权）；
//  - alive：生命周期标志，独立于 Bridge 对象，确保 Bridge 析构后
//    已入队的回调仍能安全读取它而不会产生 use-after-free；
//  - cancel：协作式取消标志。~Bridge 先置位再 join，让解析在下一个
//    文件/切片边界尽快退出，避免关窗时被整卷解析（数秒）阻塞。
struct PacsThreadHolder {
    std::unique_ptr<std::thread> thread;
    std::atomic<bool> alive{false};

    /// 必须自己 join：std::thread 析构时若仍 joinable 会直接 std::terminate()。
    /// 曾经就是第二次请求替换 holder 时，旧 holder 析构带走了未 join 的线程 → 进程 abort。
    ~PacsThreadHolder()
    {
        alive = false;
        // 若最后一个引用恰好在工作线程里释放（回调没能回到主线程），自 join 会死锁
        if (thread && thread->joinable() && thread->get_id() != std::this_thread::get_id()) {
            thread->join();
        }
    }
};

struct DicomThreadHolder {
    std::unique_ptr<std::thread> thread;
    std::atomic<bool> alive{false};
    std::atomic<bool> cancel{false};
};

class QTimer;

/**
 * Bridge — 通过 QWebChannel 暴露给前端 JS 的 C++ 对象。
 *
 * 前端可通过 `bridge` 访问以下接口（见 frontend/src/qwebchannel.ts）：
 *  - invoke: bridge.getPlatformInfo() / bridge.showMessage(msg) / bridge.add(a, b)
 *  - signal: bridge.dataFromCpp ← 由 C++ 主动推送数据到 JS
 *
 * 医疗影像实时数据模拟：
 *  - startMedicalSimulation() / stopMedicalSimulation() 控制
 *  - ecgSample(double value, double tsUs) 每 40ms 推送一个心电图波形样本
 *  - vitalsReady(map)  每 600ms 推送一次生命体征快照
 *
 * 所有实时信号的载荷都带采集时刻 tsUs（epoch 微秒，system_clock），
 * 与 JS 侧 performance.timeOrigin + performance.now() 同一时基，
 * 因此前端可以算出「采集 → 收到 → 绘制」的真实链路延迟，而不是只能测
 * 「C++ emit → JS 回调」这段。
 *
 * 神经内科介入模拟：
 *  - startNeurologySimulation() / stopNeurologySimulation() 控制
 *  - eegSample(double)     每 50ms 推送脑电图（EEG）样本
 *  - emgSample(double)     每 100ms 推送肌电图（EMG）样本
 *  - neuralSpike(double)   每 75ms 推送神经元活动样本
 *  - interventionReady(map) 每 500ms 推送神经干预事件快照
 */
class Bridge : public QObject
{
    Q_OBJECT
    // 暴露给 JS 的属性（只读，经 Q_PROPERTY 自动同步到 QWebChannel）
    Q_PROPERTY(QString appName READ appName CONSTANT)
    Q_PROPERTY(QString platform READ platform CONSTANT)
    /// 原生体渲染是否接管了右侧面板；为 true 时前端不要再自己 fetch 一份 33MB 体数据
    Q_PROPERTY(bool nativeVolume READ nativeVolume CONSTANT)

public:
    explicit Bridge(QObject *parent = nullptr);
    ~Bridge() override;

    QString appName() const;
    QString platform() const;
    bool nativeVolume() const;

    // Q_INVOKABLE 方法可被 JS 直接调用（qwebchannel 会生成对应 Promise）
    Q_INVOKABLE QString getPlatformInfo() const;
    Q_INVOKABLE QString showMessage(const QString &message);
    Q_INVOKABLE int add(int a, int b);

    // ---- 医疗实时数据模拟控制 ----
    Q_INVOKABLE void startMedicalSimulation();
    Q_INVOKABLE void stopMedicalSimulation();

    // ---- 神经内科介入模拟控制 ----
    Q_INVOKABLE void startNeurologySimulation();
    Q_INVOKABLE void stopNeurologySimulation();

    // ---- PACS（DICOM 网络）----
    /// C-ECHO 连通性验证；结果经 pacsStatus 信号回主线程（网络调用在工作线程里跑，
    /// 不允许阻塞 UI 与原生波形绘制）。
    Q_INVOKABLE void pacsProbe(const QString &host, int port, const QString &calledAe);
    /// STUDY 级 C-FIND：确认这例病人/这台设备在 PACS 上有哪些检查。
    Q_INVOKABLE void pacsFindStudies(const QString &host, int port, const QString &calledAe);
    /// 最近一次 PACS 状态快照。信号可能在前端订阅之前就发过（启动即探测），
    /// 只靠信号会让界面永远停在「未配置」——和时钟口径一样，必须能拉一次当前值。
    Q_INVOKABLE QVariantMap pacsState() const;

    /// 原生体渲染第一帧完成（毫秒），供前端记「数据到位 → 原生上屏」的耗时
    void reportNativeFrame(int ms);

    /// 最近一次体积加载的耗时快照：网页可能在事件发出之后才挂载，
    /// 只靠信号会漏掉第一次（看板就显示「—」），所以必须能拉一次当前值。
    Q_INVOKABLE QVariantMap lastVolumeTiming() const;
    /// 最近一次报警边沿的单次耗时快照（同上，供晚挂载的网页补首行）
    Q_INVOKABLE QVariantMap lastAlarmTiming() const;
    /// 原生条 → Bridge：把报警边沿耗时转成 QWebChannel 信号
    void notifyAlarmSpan(int level, double detectMs, double paintedMs, double auditMs);

    /// 上一次运行是否异常结束（没有 session.end）：{previousSession, previousStart, gapSeconds, events}
    Q_INVOKABLE QVariantMap lastRecovery() const;
    /// C-GET 取回整例检查到本地目录，成功后自动重解析并替换当前体数据。
    /// 这是把「PACS 上有」变成「屏幕上能看到」的那一步。
    Q_INVOKABLE void pacsRetrieveStudy(const QString &host, int port,
                                       const QString &calledAe, const QString &studyUid);

    // ---- 结构化审计台账 ----
    /// 追加一条审计事件（前端看门狗等 WebView 侧事件走这里）。
    /// map 里的 type 是事件类型，其余字段原样并入；固定字段（ts/tsUs/session）不可覆盖。
    Q_INVOKABLE void logAuditEvent(const QVariantMap &event);
    /// 台账检索：{type:"alarm."|"", text:"93", limit:50} → {events:[...], scanned, returned, truncated}
    /// 事件按「新→旧」返回。只读文件，不写台账；单次最多扫 5000 行，避免翻旧账拖住 UI。
    Q_INVOKABLE QVariantMap queryAuditEvents(const QVariantMap &filter);
    /// 台账本体：MainWindow 把原生条的事件也写到同一份文件
    class AuditSink *auditSink() const { return m_audit; }
    class LumeSink *lumeSink() const { return m_lume; }

    // ---- 当前设备数据源（前端据此判断时间戳时基） ----
    // 返回 {kind, description, hasHardwareClock}。hasHardwareClock=true 时 tsUs 来自
    // 设备时钟，与宿主墙钟存在未知常量偏移：前端必须先估偏移再算延迟，否则看到的
    // 是把「时钟差」当成「延迟」的假值。
    Q_INVOKABLE QVariantMap providerInfo() const;

    // ---- DICOM 体积加载（后台线程解析，结果经信号回传） ----
    Q_INVOKABLE void loadDicomVolume(const QString &directory);
    // 返回内置 DICOM 样例序列目录（脑部 T1 MRI）
    Q_INVOKABLE QString dicomSamplePath() const;
    // 最近一次解析出的体积字节（供 AppSchemeHandler 以二进制供数；不暴露给 JS）
    QByteArray volumeBytes() const;
    QByteArray volumeHuBytes() const;

public slots:
    // JS 可通过 bridge.setDataFromCpp(...) 触发，用于演示双向通信
    void setDataFromCpp(const QVariant &data);

signals:
    // C++ → JS 信号
    void dataFromCpp(const QVariant &data);
    // 医疗实时数据信号（tsUs = 采集时刻，epoch 微秒）
    void ecgSample(double value, double tsUs);
    // SpO₂ 体积描记波形（pleth）。与 ECG 一样是「关键波形」：原生条直连这条信号，
    // WebView 侧不订阅它——关键通路的显示与报警判定都不依赖 IPC。
    void spo2Sample(double value, double tsUs);
    void vitalsReady(const QVariantMap &vitals);
    // 神经内科实时数据信号
    void eegSample(double value, double tsUs);
    void emgSample(double value, double tsUs);
    void neuralSpike(double value, double tsUs);
    void interventionReady(const QVariantMap &intervention);
    // DICOM 体积加载结果。第二个参数是虚拟资源 URL（app://app/__volume/current.bin），
    // 不是体积数据本身：2MB 级二进制不能走 QWebChannel 的文本序列化
    // （实测会被 UTF-8 转换破坏），前端 fetch 该 URL 取真正的 ArrayBuffer。
    void dicomVolumeReady(const QVariantMap &meta, const QString &volumeUrl);
    /// 原生侧首帧耗时（ms），与 dicom.volumeNativeFrame 台账同源
    void volumeNativeFirstFrame(int ms);
    /// 报警边沿的「单次耗时」：{level, detectMs, paintedMs, auditMs}
    void alarmTiming(const QVariantMap &timing);
    void dicomVolumeError(const QString &message);
    // PACS 结果：{ok, host, port, calledAe, roundTripMs, message, tsUs}
    void pacsStatus(const QVariantMap &status);
    /// 启动时发现上次会话未正常关闭（进程被杀/崩溃）：恢复到「有明确状态」而不是静默空白
    void sessionRecovered(const QVariantMap &info);

    // C-FIND 结果：studies 列表 + {count, host, calledAe, tsUs}
    void pacsStudies(const QVariantList &studies, const QVariantMap &meta);
    // 取回结果：{ok, studyUid, files, dir, ms, message, detail}
    void pacsRetrieve(const QVariantMap &status);

private:
    /// 扫描台账找出未闭合的会话（进程被杀/崩溃时没有 session.end）
    void recoverLastSession();
    /// 恢复上次的体数据来源：重启后先看到上次那一例，而不是空白或样例
    void restoreLastVolume();
    /// 记录「当前显示的体数据来自哪个目录」，供下次启动恢复
    void saveVolumeState(const QString &directory);
    QString stateFilePath() const;

    /// 当前时刻（epoch 微秒）。给 DICOM 等非设备来源的载荷打时间戳。
    static double nowMicros();

    int m_callCount = 0;

    /// 设备接入层：模拟器只是其中一种实现，换真实设备时替换这里即可。
    /// Bridge 只负责把它发出的信号原样转发给 QWebChannel，不参与采样与合成。
    class DeviceProvider *m_provider = nullptr;

    /// 事件台账（JSON Lines）。一行一条，按天切分。
    class AuditSink *m_audit = nullptr;
    class LumeSink *m_lume = nullptr;
    /// 上次运行异常结束的摘要（空表示上次是正常退出或没有历史）
    QVariantMap m_recovery;
    QElapsedTimer m_volumeParseTimer;
    int m_lastParseMs = -1;
    int m_lastNativeFrameMs = -1;
    QVariantMap m_lastAlarmTiming;

    /// PACS 后台请求（单飞：上一次没回来就不再发，避免网络卡住时堆线程）
    /// 解析在途时的排队目录：PACS 取回与前端样例加载会撞在一起，
    /// 早期版本直接忽略后来的请求，导致「取回了但画面还是样例」。
    QString m_pendingDicomDir;

    /// PACS 取回的检查一旦解析成功，就占据「当前体数据」：样例的自动加载不应再覆盖它
    /// （术中屏幕上必须是这个病人的影像，不是演示数据）。为空表示没有权威来源。
    QString m_authoritativeVolumeDir;

    std::shared_ptr<PacsThreadHolder> m_pacsHolder;
    bool m_pacsBusy = false;
    /// 心跳单独一条线程/关联：长取回（C-MOVE 收尾可能数秒）期间仍要能发现掉线，
    /// 不能因为「有人在取图」就把在线检测停掉。
    std::shared_ptr<PacsThreadHolder> m_heartbeatHolder;
    bool m_heartbeatBusy = false;
    QVariantMap m_pacsState;

    /// 最近一次 PACS 端点：心跳与重试都复用它，免得各处再解析一遍配置。
    QString m_pacsHost;
    int m_pacsPort = -1;
    QString m_pacsAe;

    /// 在线心跳：只做 C-ECHO，不重新枚举。发现掉线才是目的，
    /// 状态翻转时才记台账（否则每 30 s 一行心跳会把台账淹掉）。
    class QTimer *m_pacsHeartbeat = nullptr;
    bool m_pacsOnline = false;
    bool m_pacsProbed = false;

    /// 单次探测（heartbeat=false 时成功后接着 C-FIND）。
    void probePacs(const QString &host, int port, const QString &calledAe, bool heartbeat);

    /// 单次取回尝试；失败且可重试时由调用方按退避再排一次。
    void startRetrieveAttempt(const QString &host, int port, const QString &calledAe,
                              const QString &studyUid, const QString &destDir,
                              const QString &method, int attempt);

    // DICOM 后台加载线程的持有结构（堆上、shared_ptr 管理）。
    // 使 worker 收尾闭包能精确删除“本线程”、并在 Bridge 析构后安全
    // 跳过已入队的回调（详见 bridge.cpp 的 loadDicomVolume / ~Bridge）。
    std::shared_ptr<DicomThreadHolder> m_dicomHolder;

    // 最近一次解析出的体积（主线程写入、主线程读取，无需加锁）
    double m_rssBeforeLoad = -1.0;
    QByteArray m_volumeBytes;
    QByteArray m_volumeHuBytes;
};
