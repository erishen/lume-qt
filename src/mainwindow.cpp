#include "mainwindow.h"

#include <QWebChannel>
#include <QWebEngineLoadingInfo>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
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

    // 中央区 = WebView（弹性）+ 底部原生波形条（固定高度）。
    auto *central = new QWidget(this);
    auto *centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);

    m_view = new QWebEngineView(m_profile, central);
    centralLayout->addWidget(m_view, 1);

    m_strip = new NativeVitalStrip(central);
    centralLayout->addWidget(m_strip, 0);

    setCentralWidget(central);

    // 注意顺序：必须先 setPage（替换默认 page），再 setWebChannel，
    // 否则 channel 会绑定到随后被丢弃的旧 page 上，前端将拿不到 bridge。
    setupConsoleBridge();
    setupWebChannel();
    setupNativeStrip();

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

