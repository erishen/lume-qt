#include "mainwindow.h"

#include <QWebChannel>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <QUrl>
#include <QUrlQuery>

#include "appschemehandler.h"
#include "bridge.h"
#include "consolepage.h"

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

    // 注意顺序：必须先 setPage（替换默认 page），再 setWebChannel，
    // 否则 channel 会绑定到随后被丢弃的旧 page 上，前端将拿不到 bridge。
    setupConsoleBridge();
    setupWebChannel();

    // 加载前端（React 构建产物）
    const QUrl url(QStringLiteral("app://app/index.html"));
    connect(m_view, &QWebEngineView::loadFinished, this, [](bool ok) {
        if (!ok) {
            qWarning() << "[WebEngine] 前端加载失败：app://app/index.html";
            return;
        }
        qInfo() << "[WebEngine] 前端已加载，诊断钩子已注入";
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
}

