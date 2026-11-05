#include "mainwindow.h"

#include <QWebChannel>
#include <QWebEngineLoadingInfo>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QCheckBox>
#include <QStandardItemModel>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QShortcut>
#include <QSplitter>
#include <QWebEngineView>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QUrlQuery>

#include <memory>

#include "appschemehandler.h"
#include "auditsink.h"
#include "bridge.h"
#include "consolepage.h"
#include "vitalstrip.h"
#include "volumeview.h"

namespace {
/// 前端诊断探针脚本：捕获 WebGL 上下文创建失败与着色器编译/链接错误。
///
/// 这类错误在前端只表现为“画布全黑”，且 three.js 只在 devtools 里报，
/// 终端完全看不到。以 QWebEngineScript（DocumentCreation 注入）方式装入，
/// 可确保在 React 挂载、three.js 编译着色器之前就完成拦截。
const char *kFrontendDiagnostics = R"JS(
(function () {
  if (window.__qtrsDiagInstalled) return;
  window.__qtrsDiagInstalled = true;

  function mark(level, args) {
    try {
      var parts = [];
      for (var i = 0; i < args.length; i++) {
        var a = args[i];
        parts.push(typeof a === 'string' ? a : (a && a.stack) ? a.stack : String(a));
      }
      console.log('[__qtrs_probe] ' + level + ': ' + parts.join(' | '));
    } catch (e) { /* 忽略诊断自身异常 */ }
  }

  window.addEventListener('error', function (ev) {
    mark('window.error', [ev.message, ev.filename + ':' + ev.lineno]);
  }, true);
  window.addEventListener('unhandledrejection', function (ev) {
    mark('unhandledrejection', [ev.reason]);
  });

  // 报告 QWebChannel transport 是否可用；缺失时 bridge 会静默降级为 null，
  // 这里让它在终端里可见，避免「所有 C++ 功能都无反应」却无从排查。
  mark('transport', [
    'qt=' + typeof window.qt,
    'transport=' + (window.qt ? typeof window.qt.webChannelTransport : 'n/a'),
    'send=' + (window.qt && window.qt.webChannelTransport ? typeof window.qt.webChannelTransport.send : 'n/a')
  ]);

  // 拦截 WebGL 上下文创建：返回 null 时明确报告原因
  var origGetContext = HTMLCanvasElement.prototype.getContext;
  HTMLCanvasElement.prototype.getContext = function (type) {
    var ctx = origGetContext.apply(this, arguments);
    if (!ctx && (type === 'webgl2' || type === 'webgl')) {
      mark('webgl-context-failed', [type, 'getContext(' + type + ') 返回 null']);
    }
    return ctx;
  };

  // 拦截着色器编译/链接错误
  var proto = (typeof WebGL2RenderingContext !== 'undefined') ? WebGL2RenderingContext.prototype : null;
  if (proto) {
    var origCompile = proto.compileShader;
    proto.compileShader = function (shader) {
      origCompile.apply(this, arguments);
      if (!this.getShaderParameter(shader, this.COMPILE_STATUS)) {
        mark('shader-compile-failed', [this.getShaderInfoLog(shader) || '(无 info log)']);
      }
    };
    var origLink = proto.linkProgram;
    proto.linkProgram = function (program) {
      origLink.apply(this, arguments);
      if (!this.getProgramParameter(program, this.LINK_STATUS)) {
        mark('shader-link-failed', [this.getProgramInfoLog(program) || '(无 info log)']);
      }
    };
  }
})();
)JS";
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("qt-react-shell"));
    resize(1280, 800);

    // 必须用「命名（持久化）」profile，不能用 off-the-record 的无参构造：
    // 实测 Qt 6.12 下 off-the-record profile 不会把 app:// 请求投递给
    // installUrlSchemeHandler 注册的 handler（loadFinished(false)、无 404、
    // 无 JS 日志），页面直接空白。自定义 scheme 与 off-the-record 不兼容。
    //
    // 代价是会在 ~/Library/Application Support/research/qt-react-shell/ 留下
    // History / Cookies / Session Storage。并发启动多个实例实测仍能加载，
    // 但会共享/覆盖这份缓存；排查异常时先确认没有残留进程（见 README「已知事项」）。
    m_profile = new QWebEngineProfile(QStringLiteral("qt-react-shell"), this);
    m_schemeHandler = new AppSchemeHandler(QStringLiteral(WEB_ROOT_PATH), this);
    // 体积二进制由 scheme handler 直接供数（见 AppSchemeHandler::serveVolume）
    m_schemeHandler->setVolumeProvider([this]() {
        return m_bridge ? m_bridge->volumeBytes() : QByteArray();
    });
    // 前端产物是本地资源，缓存只会带来「改了代码但界面还是旧 bundle」的假象
    // （实测：重新构建后仍加载上一次的 index.xxx.js）。本地应用不需要磁盘缓存。
    m_profile->setHttpCacheType(QWebEngineProfile::NoCache);
    m_profile->installUrlSchemeHandler(QByteArrayLiteral("app"), m_schemeHandler);

    // 允许本地资源访问远程资源（如需加载外部图片/CDN 可放开），
    // 并允许 JS 弹出 alert/confirm 等对话框。
    m_profile->settings()->setAttribute(
        QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
    m_profile->settings()->setAttribute(
        QWebEngineSettings::JavascriptCanOpenWindows, true);

    // 中央区 = WebView（弹性）+ 右侧原生体渲染（固定宽度）+ 底部原生波形条。
    // 体渲染放在 WebView 外面是刻意的：同一份 128³ 数据同时走两条路径，
    // 哪条在遮挡/后台时停更，一眼就能对比出来。
    auto *central = new QWidget(this);
    auto *centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);

    auto *centerSplit = new QSplitter(Qt::Horizontal, central);
    m_view = new QWebEngineView(m_profile, centerSplit);
    centerSplit->addWidget(m_view);

    auto *volumePanel = new QWidget(centerSplit);
    auto *volumeLayout = new QVBoxLayout(volumePanel);
    volumeLayout->setContentsMargins(8, 8, 8, 8);
    volumeLayout->setSpacing(4);
    auto *volumeTitle = new QLabel(QStringLiteral("DICOM 体渲染 · 原生 OpenGL（不经 WebView）"),
                                   volumePanel);
    volumeTitle->setStyleSheet(QStringLiteral("color:#8fa6c8;font-size:12px;"));
    m_volumeView = new VolumeView(volumePanel);
    m_volumeStatus = new QLabel(QStringLiteral("等待体数据…"), volumePanel);
    m_volumeStatus->setStyleSheet(QStringLiteral("color:#7b8798;font-size:11px;"));
    m_volumeStatus->setWordWrap(true);
    auto *volumeLegend = new QLabel(volumePanel);
    volumeLegend->setFixedHeight(10);
    volumeLegend->setStyleSheet(QStringLiteral(
        "background:qlineargradient(x1:0,y1:0,x2:1,y2:0,"
        "stop:0 #f2a24f, stop:0.5 #efd3a8, stop:1 #9fc4ea);"
        "border-radius:3px;"));
    // 窗预设 + 伪彩开关：默认灰阶软组织窗，符合阅片习惯；
    // 伪彩只为定位（区分深度/密度），标注「非诊断」，不冒充标准色标。
    auto *controls = new QHBoxLayout;
    controls->setSpacing(6);
    m_volumePreset = new QComboBox(volumePanel);
    m_volumePreset->addItems({QStringLiteral("软组织窗 W80/L40"),
                              QStringLiteral("骨窗 W1500/L300"),
                              QStringLiteral("血管窗 W400/L100"),
                              QStringLiteral("数据自适应")});
    m_volumePreset->setStyleSheet(QStringLiteral("font-size:11px;"));
    m_volumeOverlay = new QCheckBox(QStringLiteral("伪彩（非诊断）"), volumePanel);
    m_volumeOverlay->setStyleSheet(QStringLiteral("color:#9fb2c9;font-size:11px;"));
    controls->addWidget(m_volumePreset, 1);
    controls->addWidget(m_volumeOverlay, 0);

    m_volumeLegendText = new QLabel(volumePanel);
    m_volumeLegendText->setStyleSheet(QStringLiteral("color:#7b8798;font-size:10px;"));
    m_volumeLegendText->setWordWrap(true);
    m_volumeLegendText->setText(QStringLiteral("灰阶显示 · 亮度 = 窗内位置（伪彩关闭）"));

    volumeLayout->addWidget(volumeTitle);
    volumeLayout->addWidget(m_volumeView, 1);
    volumeLayout->addLayout(controls);
    volumeLayout->addWidget(volumeLegend);
    volumeLayout->addWidget(m_volumeLegendText);
    volumeLayout->addWidget(m_volumeStatus);

    connect(m_volumePreset, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (m_volumeView == nullptr) return;
        m_volumeView->setPreset(idx);
        if (m_volumeStatus && !m_volumeSourceText.isEmpty()) {
            m_volumeStatus->setText(QStringLiteral("%1 · %2")
                                        .arg(m_volumeSourceText, m_volumeView->windowText()));
        }
    });
    // 环境变量便于自动化验证预设：QTRS_VOLUME_PRESET=0/1/2/3、QTRS_VOLUME_COLOR=1
    {
        const QByteArray presetEnv = qgetenv("QTRS_VOLUME_PRESET");
        if (!presetEnv.isEmpty()) {
            m_volumePreset->setCurrentIndex(presetEnv.toInt());
        }
        if (qgetenv("QTRS_VOLUME_COLOR") == "1") {
            m_volumeOverlay->setChecked(true);
        }
    }

    connect(m_volumeOverlay, &QCheckBox::toggled, this, [this](bool on) {
        if (m_volumeView == nullptr) return;
        m_volumeView->setColorOverlay(on);
        if (m_volumeLegendText) {
            m_volumeLegendText->setText(
                on ? QStringLiteral("伪彩叠加 · 暖=近/软组织，冷=深/骨·对比剂（仅供定位，非诊断色标）")
                   : QStringLiteral("灰阶显示 · 亮度 = 窗内位置（伪彩关闭）"));
        }
    });
    centerSplit->addWidget(volumePanel);
    centerSplit->setStretchFactor(0, 1);
    centerSplit->setStretchFactor(1, 0);
    centerSplit->setSizes({840, 420});

    centralLayout->addWidget(centerSplit, 1);

    m_strip = new NativeVitalStrip(central);
    centralLayout->addWidget(m_strip, 0);

    setCentralWidget(central);

    // 注意顺序：必须先 setPage（替换默认 page），再 setWebChannel，
    // 否则 channel 会绑定到随后被丢弃的旧 page 上，前端将拿不到 bridge。
    setupConsoleBridge();
    setupWebChannel();
    setupNativeStrip();
    setupNativeVolume();

    // 加载前端（React 构建产物）
    // 若设置了 QTRS_DEV_URL（如 http://localhost:5173），则加载 Vite 开发服务器，
    // 前端改动即时生效（热重载）；否则加载本地构建产物 app://app/index.html。
    const QByteArray devUrlEnv = qgetenv("QTRS_DEV_URL");
    const QUrl url(devUrlEnv.isEmpty()
                       ? QStringLiteral("app://app/index.html")
                       : QString::fromLocal8Bit(devUrlEnv));

    // WebEngine 渲染进程首次启动时，自定义 scheme 的首帧导航偶发失败。
    // 有限重试一次作为兜底；两次都失败才判定为真正的加载错误。
    // 无论成败，setupConsoleBridge() 里的 loadingChanged 都会打印 Chromium
    // 的真实错误域/错误码，用于区分资源缺失、scheme 未投递与进程被提前终止。
    auto retries = std::make_shared<int>(0);
    connect(m_view, &QWebEngineView::loadFinished, this,
            [this, url, retries](bool ok) {
        if (!ok) {
            if ((*retries)++ < 1) {
                qWarning().noquote() << "[WebEngine] 前端加载失败，重试一次："
                                     << url.toString();
                QTimer::singleShot(300, m_view, [this, url]() { m_view->load(url); });
                return;
            }
            qWarning().noquote() << "[WebEngine] 前端加载失败：" << url.toString()
                                 << "（WEB_ROOT=" << QStringLiteral(WEB_ROOT_PATH) << "；"
                                 << "请确认已执行 make cpp 生成前端产物，并查看上方「加载错误详情」）";
            return;
        }
        qInfo().noquote() << "[WebEngine] 前端已加载：" << url.toString();
    });
    m_view->load(url);
}

MainWindow::~MainWindow() = default;

void MainWindow::setupWebChannel()
{
    m_bridge = new Bridge(this);

    QWebChannel *channel = new QWebChannel(this);
    channel->registerObject(QStringLiteral("bridge"), m_bridge);
    m_page->setWebChannel(channel);
}

void MainWindow::setupNativeVolume()
{
    // QTRS_NATIVE_VOLUME=0 可关掉右侧面板（只保留网页侧体渲染，用于对照）。
    const QByteArray enable = qgetenv("QTRS_NATIVE_VOLUME");
    if (enable == "0") {
        if (m_volumeView) {
            if (auto *panel = m_volumeView->parentWidget()) panel->setVisible(false);
        }
        qInfo() << "[Volume] 原生体渲染面板已按 QTRS_NATIVE_VOLUME=0 关闭";
        return;
    }
    if (m_bridge == nullptr || m_volumeView == nullptr) return;

    connect(m_bridge, &Bridge::dicomVolumeReady, this,
            [this](const QVariantMap &meta, const QString &volumeUrl) {
        Q_UNUSED(volumeUrl);
        const QVariantList dims = meta.value(QStringLiteral("dims")).toList();
        const int dim = dims.isEmpty() ? 128 : dims.at(0).toInt();
        // 原生侧用 int16 HU：只有保住 HU，「W80/L40」这类临床窗才有意义。
        m_volumeView->setVolume(m_bridge->volumeHuBytes(), dim, meta);
        const bool isCt = meta.value(QStringLiteral("modality")).toString().compare(
                              QStringLiteral("CT"), Qt::CaseInsensitive) == 0;
        const QString unit = isCt ? QStringLiteral("HU") : QStringLiteral("%1 a.u. (非 HU)")
                                                        .arg(meta.value(QStringLiteral("modality")).toString());
        m_volumeSourceText = QStringLiteral("%1³ · %2 %3…%4 · 来源 %5")
                                 .arg(dim)
                                 .arg(unit)
                                 .arg(static_cast<int>(meta.value(QStringLiteral("min")).toDouble()))
                                 .arg(static_cast<int>(meta.value(QStringLiteral("max")).toDouble()))
                                 .arg(meta.value(QStringLiteral("source")).toString());
        if (m_volumePreset) {
            // CT 才有 HU 窗预设；MR/其他只有「数据自适应」。
            if (isCt) {
                // 切到 CT 后重新应用一次预设（环境变量或用户上次选择），别留存 MR 的自适应档
                if (const QByteArray p = qgetenv("QTRS_VOLUME_PRESET"); !p.isEmpty()) {
                    m_volumePreset->setCurrentIndex(p.toInt());
                }
            }
            if (auto *model = m_volumePreset->model()) {
                for (int i = 0; i < 3; ++i) {
                    model->setData(model->index(i, 0), isCt, Qt::UserRole - 1); // 禁用项
                }
            }
            if (!isCt && m_volumePreset->currentIndex() != 3) {
                m_volumePreset->setCurrentIndex(3);
            }
        }
        if (m_volumeStatus) {
            m_volumeStatus->setText(QStringLiteral("%1 · %2")
                                        .arg(m_volumeSourceText, m_volumeView->windowText()));
        }
    });
    connect(m_bridge, &Bridge::dicomVolumeError, this, [this](const QString &message) {
        if (m_volumeStatus) m_volumeStatus->setText(QStringLiteral("解析失败：%1").arg(message));
    });
    connect(m_volumeView, &VolumeView::firstFrameRendered, this,
            [this](int ms, const QString &dims) {
        if (m_bridge) {
            m_bridge->logAuditEvent(QVariantMap{
                {QStringLiteral("type"), QStringLiteral("dicom.volumeNativeFrame")},
                {QStringLiteral("ms"), ms},
                {QStringLiteral("dims"), dims},
                {QStringLiteral("source"), QStringLiteral("native-opengl")},
            });
        }
    });
}

void MainWindow::setupNativeStrip()
{
    // 关键波形直连 Bridge 信号：不经过 QWebChannel，因此这里同时也是
    // 「原生通道 vs WebView 通道」延迟对照的基准。
    connect(m_bridge, &Bridge::ecgSample, m_strip, &NativeVitalStrip::pushEcgSample);
    // SpO₂ 波形只走原生：WebView 侧不订阅这条通道，关键通路的显示与判定不依赖 IPC
    connect(m_bridge, &Bridge::spo2Sample, m_strip, &NativeVitalStrip::pushSpo2Sample);
    connect(m_bridge, &Bridge::vitalsReady, m_strip, &NativeVitalStrip::setVitals);

    // 原生条的中断/恢复/报警事件写进同一份台账：这些判定发生在原生侧，
    // 与前端看门狗是两个独立来源，事后复核需要能区分是谁报的（source 字段）。
    auto *audit = m_bridge->auditSink();
    // 静音快捷键：M 切换（窗口级，焦点在 WebView 里也生效）。术中没有空去点小按钮。
    auto *muteKey = new QShortcut(QKeySequence(Qt::Key_M), this);
    muteKey->setContext(Qt::WindowShortcut);
    connect(muteKey, &QShortcut::activated, this, [this]() {
        m_strip->muteAlarms(m_strip->alarmsMuted() ? 0 : m_strip->defaultMuteSeconds());
    });

    connect(m_strip, &NativeVitalStrip::auditEvent, this, [audit](const QVariantMap &ev) {
        if (!audit) return;
        const QString type = ev.value(QStringLiteral("type")).toString();
        if (type.isEmpty()) return;
        QVariantMap fields = ev;
        fields.remove(QStringLiteral("type"));
        audit->record(type, fields);
    });
}

void MainWindow::setupConsoleBridge()
{
    // 用自定义 QWebEnginePage 替换默认 page，把前端 console/异常转发到终端。
    // 必须传入 m_profile：page 所属的 profile 决定 app:// scheme handler 是否生效，
    // 若用默认 profile，页面将因找不到 scheme handler 而加载失败。
    m_page = new ConsolePage(m_profile, m_view);
    m_view->setPage(m_page);

    // 以 DocumentCreation 注入诊断脚本：在 React 挂载与 three.js 编译着色器
    // 之前完成拦截，才能捕获到首帧的着色器编译错误。
    QWebEngineScript script;
    script.setName(QStringLiteral("qt-react-shell-diagnostics"));
    script.setSourceCode(QByteArray(kFrontendDiagnostics));
    script.setInjectionPoint(QWebEngineScript::DocumentCreation);
    script.setWorldId(0); // 主世界，与前端脚本共享全局对象
    script.setRunsOnSubFrames(false);
    m_profile->scripts()->insert(script);

    // loadFinished(false) 只有一个 bool，排查「页面空白」时信息量太少。
    // loadingChanged 会带出 Chromium 侧的真实错误域/错误码/错误文本，
    // 例如自定义 scheme 未投递时是 InternalErrorDomain / ERR_UNKNOWN_URL_SCHEME，
    // 资源缺失则是 ConnectionErrorDomain / ERR_FILE_NOT_FOUND。
    connect(m_page, &QWebEnginePage::loadingChanged, this,
            [](const QWebEngineLoadingInfo &info) {
        if (info.status() != QWebEngineLoadingInfo::LoadFailedStatus)
            return;
        qWarning().noquote() << "[WebEngine] 加载错误详情："
                             << info.url().toString()
                             << "domain=" << info.errorDomain()
                             << "code=" << info.errorCode()
                             << "detail=" << info.errorString();
    });
}

