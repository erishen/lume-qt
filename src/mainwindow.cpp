#include "mainwindow.h"

#include <QWebChannel>
#include <QWebEngineLoadingInfo>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QCheckBox>
#include <QPushButton>
#include <QSettings>
#include <QStandardItemModel>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSlider>
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
#include "lumesink.h"
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
                              QStringLiteral("数据自适应"),
                              QStringLiteral("自定义（手动调窗）")});
    m_volumePreset->setStyleSheet(QStringLiteral("font-size:11px;"));
    m_volumeOverlay = new QCheckBox(QStringLiteral("伪彩（非诊断）"), volumePanel);
    m_volumeOverlay->setStyleSheet(QStringLiteral("color:#9fb2c9;font-size:11px;"));
    controls->addWidget(m_volumePreset, 1);
    controls->addWidget(m_volumeOverlay, 0);

    m_volumeLegendText = new QLabel(volumePanel);
    m_volumeLegendText->setStyleSheet(QStringLiteral("color:#7b8798;font-size:10px;"));
    m_volumeLegendText->setWordWrap(true);
    m_volumeLegendText->setText(QStringLiteral("灰阶显示 · 亮度 = 窗内位置（伪彩关闭）"));

    // 手动窗：ALT+左键在图上拖也能调，但需要能看到具体数值、也需要能精确输入。
    auto *windowRow = new QHBoxLayout;
    windowRow->setSpacing(4);
    auto *wLabel = new QLabel(QStringLiteral("W"), volumePanel);
    auto *lLabel = new QLabel(QStringLiteral("L"), volumePanel);
    wLabel->setStyleSheet(QStringLiteral("color:#9fb2c9;font-size:11px;"));
    lLabel->setStyleSheet(QStringLiteral("color:#9fb2c9;font-size:11px;"));
    m_windowWidthBox = new QDoubleSpinBox(volumePanel);
    m_windowLevelBox = new QDoubleSpinBox(volumePanel);
    for (auto *box : {m_windowWidthBox, m_windowLevelBox}) {
        box->setDecimals(0);
        box->setKeyboardTracking(false);
        box->setStyleSheet(QStringLiteral("font-size:11px;"));
        box->setFixedWidth(78);
    }
    m_windowWidthBox->setRange(1, 4000);
    m_windowWidthBox->setSuffix(QStringLiteral(" HU"));
    m_windowLevelBox->setRange(-2000, 4000);
    m_windowLevelBox->setSuffix(QStringLiteral(" HU"));
    windowRow->addWidget(wLabel, 0);
    windowRow->addWidget(m_windowWidthBox, 1);
    windowRow->addWidget(lLabel, 0);
    windowRow->addWidget(m_windowLevelBox, 1);
    // 手动调窗最容易发生的失误是「调完忘了回临床预设」；给一个显式的回退入口，
    // 而不是指望术者记住上次选的是哪一档。
    auto *restoreWindowBtn = new QPushButton(QStringLiteral("恢复上次预设"), volumePanel);
    restoreWindowBtn->setStyleSheet(QStringLiteral("font-size:11px;"));
    restoreWindowBtn->setToolTip(QStringLiteral("回到手动调窗前的预设（⌘⇧W；也接受物理 Ctrl+Shift+W）"));
    windowRow->addWidget(restoreWindowBtn, 0);
    windowRow->addStretch(0);

    // 去骨 + 切面：术中想看清颅内结构，最常用的是「把颅骨拿掉」和「剖开一半」。
    auto *clipRow = new QHBoxLayout;
    clipRow->setSpacing(6);
    m_boneBox = new QCheckBox(QStringLiteral("去骨（CT）"), volumePanel);
    m_boneBox->setStyleSheet(QStringLiteral("color:#9fb2c9;font-size:11px;"));
    m_clipCombo = new QComboBox(volumePanel);
    m_clipCombo->addItems({QStringLiteral("完整"), QStringLiteral("矢状面（左右）"),
                           QStringLiteral("冠状面（前后）"), QStringLiteral("横断面（上下）")});
    m_clipCombo->setStyleSheet(QStringLiteral("font-size:11px;"));
    m_clipSlider = new QSlider(Qt::Horizontal, volumePanel);
    m_clipSlider->setRange(0, 100);
    m_clipSlider->setValue(100);
    m_clipSlider->setFixedWidth(86);
    m_clipSlider->setEnabled(false);
    m_clipSlider->setToolTip(QStringLiteral("切面位置：0% 近端 → 100% 远端"));
    clipRow->addWidget(m_boneBox, 0);
    clipRow->addWidget(m_clipCombo, 1);
    clipRow->addWidget(m_clipSlider, 0);

    volumeLayout->addWidget(volumeTitle);
    volumeLayout->addWidget(m_volumeView, 1);
    volumeLayout->addLayout(controls);
    volumeLayout->addLayout(windowRow);
    volumeLayout->addLayout(clipRow);
    volumeLayout->addWidget(volumeLegend);
    volumeLayout->addWidget(m_volumeLegendText);
    volumeLayout->addWidget(m_volumeStatus);

    connect(m_volumePreset, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (m_volumeView == nullptr) return;
        if (idx >= 4) return; // 「自定义」不重置窗，保留当前 W/L
        // 环境变量驱动的预设是自动化/标定用的，不该污染用户偏好；
        // 只有人手在下拉里选的那次才更新回退档位并落盘。
        if (!m_restoringSettings) {
            m_lastPresetIndex = idx;
            if (qgetenv("QTRS_VOLUME_PRESET").isEmpty()) {
                QSettings settings;
                settings.setValue(QStringLiteral("volume/preset"), idx);
                settings.setValue(QStringLiteral("volume/lastPreset"), idx);
            }
        }
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
        if (!qgetenv("QTRS_VOLUME_COLOR").isEmpty()) {
            // 0 也要显式关掉，否则下面的「恢复上次偏好」会把它又打开。
            m_volumeOverlay->setChecked(qgetenv("QTRS_VOLUME_COLOR") == "1");
        }
    }
    // 恢复上次的窗位/预设/去骨/切面。env 变量优先（自动化与标定要可复现），
    // 没有 env 时才用用户上次的选择。手动窗值必须等体数据到达后再套，
    // 因为夹取范围依赖这批数据的 HU 上下限。
    {
        m_restoringSettings = true;
        QSettings settings;
        m_lastPresetIndex = std::clamp(settings.value(QStringLiteral("volume/lastPreset"), 0).toInt(), 0, 3);
        if (qgetenv("QTRS_VOLUME_PRESET").isEmpty()) {
            const int savedPreset = std::clamp(settings.value(QStringLiteral("volume/preset"), 0).toInt(), 0, 4);
            m_volumePreset->setCurrentIndex(savedPreset);
            if (savedPreset == 4 && settings.contains(QStringLiteral("volume/manualW"))) {
                m_pendingWindowWidth = settings.value(QStringLiteral("volume/manualW"), 80.0).toDouble();
                m_pendingWindowLevel = settings.value(QStringLiteral("volume/manualL"), 40.0).toDouble();
                m_hasPendingManualWindow = true;
            }
        }
        if (qgetenv("QTRS_VOLUME_COLOR").isEmpty() && settings.contains(QStringLiteral("volume/overlay"))) {
            m_volumeOverlay->setChecked(settings.value(QStringLiteral("volume/overlay")).toBool());
        }
        m_restoringSettings = false;
    }

    auto applyManualWindow = [this]() {
        if (m_syncingWindow || m_volumeView == nullptr) return;
        if (m_windowWidthBox == nullptr || m_windowLevelBox == nullptr) return;
        m_volumeView->setManualWindow(m_windowWidthBox->value(), m_windowLevelBox->value());
    };
    // 「恢复上次预设」= 回到手动调窗之前那一档；没有手动调过时也至少给一次可见的窗值确认。
    auto restoreLastPreset = [this, restoreWindowBtn]() {
        if (m_volumeView == nullptr || m_volumePreset == nullptr) return;
        m_volumePreset->setCurrentIndex(m_lastPresetIndex);
        // setCurrentIndex 在索引未变时不会发信号，兜底再应用一次。
        m_volumeView->setPreset(m_lastPresetIndex);
        if (m_volumeStatus && !m_volumeSourceText.isEmpty()) {
            m_volumeStatus->setText(QStringLiteral("%1 · %2（已恢复上次预设）")
                                        .arg(m_volumeSourceText, m_volumeView->windowText()));
        }
        qInfo().noquote() << "[Volume] 恢复上次预设" << m_lastPresetIndex << m_volumeView->windowText();
        Q_UNUSED(restoreWindowBtn);
    };
    connect(restoreWindowBtn, &QPushButton::clicked, this, restoreLastPreset);
    // Qt 在 macOS 上把 Ctrl 映射成 Command：QKeySequence("Ctrl+Shift+W") 实际是 ⌘⇧W。
    // 再注册一个 Meta+Shift+W（对应物理 Control）覆盖习惯用真 Ctrl 的人，
    // 两个键都走同一个回退逻辑，避免提示文案和实际按键对不上。
    auto *restoreKey = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+W")), this);
    restoreKey->setContext(Qt::ApplicationShortcut);
    connect(restoreKey, &QShortcut::activated, this, restoreLastPreset);
    auto *restoreKeyCtrl = new QShortcut(QKeySequence(QStringLiteral("Meta+Shift+W")), this);
    restoreKeyCtrl->setContext(Qt::ApplicationShortcut);
    connect(restoreKeyCtrl, &QShortcut::activated, this, restoreLastPreset);
    connect(m_windowWidthBox, &QDoubleSpinBox::editingFinished, this, applyManualWindow);
    connect(m_windowLevelBox, &QDoubleSpinBox::editingFinished, this, applyManualWindow);
    if (m_volumeView) {
        connect(m_volumeView, &VolumeView::windowChanged, this,
                [this](double w, double l, bool manual) {
            m_syncingWindow = true;
            if (m_windowWidthBox) m_windowWidthBox->setValue(w);
            if (m_windowLevelBox) m_windowLevelBox->setValue(l);
            m_syncingWindow = false;
            if (manual && m_volumePreset) {
                m_volumePreset->setCurrentIndex(4);
                if (!m_restoringSettings && qgetenv("QTRS_VOLUME_WL").isEmpty()) {
                    QSettings settings;
                    settings.setValue(QStringLiteral("volume/preset"), 4);
                    if (!m_windowSaveClock.isValid() || m_windowSaveClock.elapsed() > 500) {
                        settings.setValue(QStringLiteral("volume/manualW"), w);
                        settings.setValue(QStringLiteral("volume/manualL"), l);
                        m_windowSaveClock.restart();
                    }
                }
            }
            if (m_volumeStatus && !m_volumeSourceText.isEmpty()) {
                m_volumeStatus->setText(QStringLiteral("%1 · %2")
                                            .arg(m_volumeSourceText, m_volumeView->windowText()));
            }
        });
    }

    connect(m_boneBox, &QCheckBox::toggled, this, [this](bool on) {
        if (m_volumeView) m_volumeView->setBoneRemoval(on);
        // 非 CT 时这个框会被程序禁用并清空，那不算用户选择，别把 CT 的偏好覆盖掉。
        if (!m_restoringSettings && m_boneBox && m_boneBox->isEnabled()
            && qgetenv("QTRS_VOLUME_BONE").isEmpty()) {
            QSettings settings;
            settings.setValue(QStringLiteral("volume/bone"), on);
        }
    });
    auto applyClip = [this]() {
        if (m_volumeView == nullptr || m_clipCombo == nullptr || m_clipSlider == nullptr) return;
        const int axis = m_clipCombo->currentIndex() - 1; // 0 = 完整
        m_clipSlider->setEnabled(axis >= 0);
        m_volumeView->setClipPlane(axis, static_cast<float>(m_clipSlider->value()) / 100.0f);
        if (!m_restoringSettings && qgetenv("QTRS_VOLUME_CLIP").isEmpty()) {
            QSettings settings;
            settings.setValue(QStringLiteral("volume/clipAxis"), axis);
            settings.setValue(QStringLiteral("volume/clipPos"), m_clipSlider->value());
        }
    };
    connect(m_clipCombo, &QComboBox::currentIndexChanged, this, applyClip);
    connect(m_clipSlider, &QSlider::valueChanged, this, [applyClip](int) { applyClip(); });

    connect(m_volumeOverlay, &QCheckBox::toggled, this, [this](bool on) {
        if (m_volumeView == nullptr) return;
        m_volumeView->setColorOverlay(on);
        if (!m_restoringSettings && qgetenv("QTRS_VOLUME_COLOR").isEmpty()) {
            QSettings settings;
            settings.setValue(QStringLiteral("volume/overlay"), on);
        }
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
    m_url = url;
    m_view->load(url);
}

MainWindow::~MainWindow() = default;

void MainWindow::setupWebChannel()
{
    m_bridge = new Bridge(this);

    m_channel = new QWebChannel(this);
    m_channel->registerObject(QStringLiteral("bridge"), m_bridge);
    m_page->setWebChannel(m_channel);
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
        m_restoringSettings = true;
        QSettings settings;
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
            // 预设文案跟着数据走：CT 才是 HU 窗；非 CT 用强度档，别让人以为在套 HU。
            const QStringList ctLabels = {QStringLiteral("软组织窗 W80/L40"),
                                          QStringLiteral("骨窗 W1500/L300"),
                                          QStringLiteral("血管窗 W400/L100"),
                                          QStringLiteral("数据自适应")};
            const QStringList auLabels = {QStringLiteral("低强度窗（软组织近似）"),
                                          QStringLiteral("高强度窗（骨近似）"),
                                          QStringLiteral("中强度窗（血管近似）"),
                                          QStringLiteral("数据自适应")};
            const QStringList &labels = isCt ? ctLabels : auLabels;
            for (int i = 0; i < labels.size(); ++i) {
                m_volumePreset->setItemText(i, labels.at(i));
            }
            if (isCt) {
                // 切到 CT 后重新应用一次预设（环境变量或用户上次选择），别留存 MR 的自适应档
                if (const QByteArray p = qgetenv("QTRS_VOLUME_PRESET"); !p.isEmpty()) {
                    m_volumePreset->setCurrentIndex(p.toInt());
                }
            }
            // CT 是诊断数据，默认灰阶；MR/其他本来就不是诊断图像（a.u.），
            // 默认开伪彩只作定位演示，勾选框里仍标「非诊断」。
            if (m_volumeOverlay && qgetenv("QTRS_VOLUME_COLOR").isEmpty() && !settings.contains(QStringLiteral("volume/overlay"))) {
                m_volumeOverlay->setChecked(!isCt);
            }
        }
        if (m_windowWidthBox && m_windowLevelBox) {
            const QString suffix = isCt ? QStringLiteral(" HU") : QStringLiteral(" a.u.");
            m_windowWidthBox->setSuffix(suffix);
            m_windowLevelBox->setSuffix(suffix);
        }
        if (m_volumeStatus) {
            m_volumeStatus->setText(QStringLiteral("%1 · %2")
                                        .arg(m_volumeSourceText, m_volumeView->windowText()));
        }
        if (m_boneBox) {
            // 去骨阈值是按 HU 定的；MR/a.u. 没有这个语义，直接禁用并写明原因，
            // 而不是让它悄悄按 a.u. 300 去切。
            m_boneBox->setEnabled(isCt);
            m_boneBox->setToolTip(isCt ? QString() : QStringLiteral("仅 CT 有 HU 骨阈值"));
            if (!isCt) m_boneBox->setChecked(false);
        }
        // 重启恢复：手动窗值要等 HU 上下限就位后才能夹取，所以压到这里。
        if (m_hasPendingManualWindow && m_volumeView) {
            m_volumeView->setManualWindow(m_pendingWindowWidth, m_pendingWindowLevel);
            m_hasPendingManualWindow = false;
            if (m_volumePreset) m_volumePreset->setCurrentIndex(4);
        }
        // 自动化/标定用：QTRS_VOLUME_BONE=1、QTRS_VOLUME_CLIP=轴,位置(0..1)
        if (const QByteArray bone = qgetenv("QTRS_VOLUME_BONE"); !bone.isEmpty()) {
            // env 显式给了 0 也算覆盖：关掉，而不是让偏好把它恢复回来。
            m_boneBox->setChecked(bone != "0");
        } else if (isCt && m_boneBox) {
            m_boneBox->setChecked(settings.value(QStringLiteral("volume/bone"), false).toBool());
        }
        if (qgetenv("QTRS_VOLUME_CLIP").isEmpty()) {
            if (settings.contains(QStringLiteral("volume/clipAxis"))) {
                // 先把两个值读出来再改控件：setCurrentIndex 会触发 applyClip 立刻回写，
                // 同一进程内的 QSettings 写会覆盖读缓存，后读到的就不是用户存的值了。
                const int axis = std::clamp(settings.value(QStringLiteral("volume/clipAxis"), -1).toInt(), -1, 2);
                const int pos = std::clamp(settings.value(QStringLiteral("volume/clipPos"), 100).toInt(), 0, 100);
                m_clipCombo->setCurrentIndex(axis + 1);
                m_clipSlider->setValue(pos);
            }
        }
        if (const QByteArray clip = qgetenv("QTRS_VOLUME_CLIP"); !clip.isEmpty()) {
            const QStringList parts = QString::fromLocal8Bit(clip).split(QLatin1Char(','));
            if (parts.size() == 2) {
                const int axis = parts.at(0).toInt();
                m_clipCombo->setCurrentIndex(std::clamp(axis, 0, 2) + 1);
                m_clipSlider->setValue(std::clamp(static_cast<int>(parts.at(1).toDouble() * 100.0), 0, 100));
            }
        }
        // 自动化/标定用：QTRS_VOLUME_WL=窗宽,窗位
        if (const QByteArray wl = qgetenv("QTRS_VOLUME_WL"); !wl.isEmpty()) {
            const QStringList parts = QString::fromLocal8Bit(wl).split(QLatin1Char(','));
            if (parts.size() == 2) {
                m_volumeView->setManualWindow(parts.at(0).toDouble(), parts.at(1).toDouble());
            }
        }
        m_restoringSettings = false;
    });
    connect(m_bridge, &Bridge::dicomVolumeError, this, [this](const QString &message) {
        if (m_volumeStatus) m_volumeStatus->setText(QStringLiteral("解析失败：%1").arg(message));
    });
    // 原生接管后，网页侧的 DicomPanel 不再挂载（避免重复拉一份体数据），
    // 因此「加载样例」这一步要由 C++ 侧发起，否则面板永远停在「等待体数据」。
    if (qgetenv("QTRS_NATIVE_VOLUME") != "0") {
        QTimer::singleShot(1200, this, [this]() {
            if (m_bridge) m_bridge->loadDicomVolume(m_bridge->dicomSamplePath());
        });
    }

    connect(m_volumeView, &VolumeView::firstFrameRendered, this,
            [this](int ms, const QString &dims) {
        if (m_bridge) {
            m_bridge->reportNativeFrame(ms);
            m_bridge->logAuditEvent(QVariantMap{
                {QStringLiteral("type"), QStringLiteral("dicom.volumeNativeFrame")},
                {QStringLiteral("ms"), ms},
                {QStringLiteral("dims"), dims},
                {QStringLiteral("source"), QStringLiteral("native-opengl")},
            });
        }
    });
}

void MainWindow::applyAlarmConfig()
{
    QStringList warnings;
    AlarmThresholds local = defaultAlarmThresholds();
    applyAlarmEnv(local, &warnings);
    m_strip->setAlarmThresholds(local);
    for (const QString &w : warnings)
        qWarning().noquote() << "[Alarm] 阈值配置：" << w;

    if (m_bridge) {
        QVariantMap fields{
            {QStringLiteral("type"), QStringLiteral("alarm.config")},
            {QStringLiteral("source"), QStringLiteral("env")},
            {QStringLiteral("spo2High"), local.spo2High},
            {QStringLiteral("spo2Mid"), local.spo2Mid},
            {QStringLiteral("hrHigh"), local.hrHigh},
            {QStringLiteral("hrMid"), local.hrMid},
        };
        if (!warnings.isEmpty())
            fields.insert(QStringLiteral("rejected"), warnings.join(QStringLiteral("; ")));
        m_bridge->logAuditEvent(fields);
    }

    // Lume 下发优先：配置集中在一处，术中各台不必各自 export 环境变量。
    // 拉不到就保持 env/默认值——配置服务不可用绝不改变本地判定能力。
    LumeSink *lume = m_bridge ? m_bridge->lumeSink() : nullptr;
    if (!lume || !lume->enabled())
        return;

    lume->fetchConfig([this](const QJsonObject &cfg) {
        QStringList rejected;
        const AlarmThresholds applied =
            alarmThresholdsFromJson(cfg, m_strip->alarmThresholds(), &rejected);
        m_strip->setAlarmThresholds(applied);
        for (const QString &w : rejected)
            qWarning().noquote() << "[Alarm] Lume 配置被拒：" << w;
        qInfo().noquote() << QStringLiteral("[Alarm] 阈值来源 Lume：SpO₂<%1/%2 · HR<%3/%4")
                                     .arg(applied.spo2High).arg(applied.spo2Mid)
                                     .arg(applied.hrHigh).arg(applied.hrMid);
        if (m_bridge) {
            QVariantMap fields{
                {QStringLiteral("type"), QStringLiteral("alarm.config")},
                {QStringLiteral("source"), QStringLiteral("lume")},
                {QStringLiteral("spo2High"), applied.spo2High},
                {QStringLiteral("spo2Mid"), applied.spo2Mid},
                {QStringLiteral("hrHigh"), applied.hrHigh},
                {QStringLiteral("hrMid"), applied.hrMid},
            };
            if (!rejected.isEmpty())
                fields.insert(QStringLiteral("rejected"), rejected.join(QStringLiteral("; ")));
            m_bridge->logAuditEvent(fields);
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

    applyAlarmConfig();
}

void MainWindow::setupConsoleBridge()
{
    // 用自定义 QWebEnginePage 替换默认 page，把前端 console/异常转发到终端。
    // 必须传入 m_profile：page 所属的 profile 决定 app:// scheme handler 是否生效，
    // 若用默认 profile，页面将因找不到 scheme handler 而加载失败。
    m_page = new ConsolePage(m_profile, m_view);
    m_view->setPage(m_page);
    installDiagnosticsScript(m_profile);

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

    // 渲染进程内存兜底：QTRS_RELOAD_MIN=N 时每 N 分钟重载一次网页（波形在原生条上不受影响）。
    // Ctrl+R 可手动重载，用于长台次里把渲染进程的 RSS 拉回基线。
    auto *reloadShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+R")), this);
    reloadShortcut->setContext(Qt::ApplicationShortcut);
    connect(reloadShortcut, &QShortcut::activated, this, &MainWindow::reloadWebView);
    auto *refreshShortcut = new QShortcut(QKeySequence::Refresh, this);
    refreshShortcut->setContext(Qt::ApplicationShortcut);
    connect(refreshShortcut, &QShortcut::activated, this, &MainWindow::reloadWebView);
    // 默认 30 分钟重建一次渲染进程；QTRS_RELOAD_MIN=0 可关闭（严格执行台次时长时不必重建）。
    int reloadMinutes = 30;
    if (const QByteArray envMinutes = qgetenv("QTRS_RELOAD_MIN"); !envMinutes.isEmpty())
        reloadMinutes = envMinutes.toInt();
    if (reloadMinutes > 0) {
        const int minutes = reloadMinutes;
        auto *reloadTimer = new QTimer(this);
        reloadTimer->setInterval(minutes * 60000);
        connect(reloadTimer, &QTimer::timeout, this, &MainWindow::reloadWebView);
        reloadTimer->start();
        qInfo().noquote() << "[WebEngine] 每" << minutes << "分钟自动重载界面";
    }
}

void MainWindow::installDiagnosticsScript(QWebEngineProfile *profile)
{
    // 以 DocumentCreation 注入诊断脚本：在 React 挂载与 three.js 编译着色器
    // 之前完成拦截，才能捕获到首帧的着色器编译错误。
    QWebEngineScript script;
    script.setName(QStringLiteral("qt-react-shell-diagnostics"));
    script.setSourceCode(QByteArray(kFrontendDiagnostics));
    script.setInjectionPoint(QWebEngineScript::DocumentCreation);
    script.setWorldId(0); // 主世界，与前端脚本共享全局对象
    script.setRunsOnSubFrames(false);
    profile->scripts()->insert(script);
}

void MainWindow::reloadWebView()
{
    if (!m_view || !m_bridge || m_url.isEmpty())
        return;
    // 单纯的 reload() 复用同一个渲染进程，RSS 不会回落（实测重载后仍按原速上涨）。
    // 要真正回收，只能换成独立 profile + 新 page：Chromium 会为它起一个新的渲染进程，
    // 旧进程随旧 page/profile 销毁。波形在原生条上，期间不中断。
    qInfo().noquote() << "[WebEngine] 重建渲染进程（内存兜底）";
    auto *oldPage = m_page;
    auto *oldProfile = m_profile;
    auto *oldChannel = m_channel;

    m_profile = new QWebEngineProfile(this);
    m_profile->installUrlSchemeHandler(QByteArrayLiteral("app"), m_schemeHandler);
    m_page = new ConsolePage(m_profile, m_view);
    installDiagnosticsScript(m_profile);
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
    m_channel = new QWebChannel(this);
    m_channel->registerObject(QStringLiteral("bridge"), m_bridge);
    m_page->setWebChannel(m_channel);

    m_view->setPage(m_page);
    m_view->load(m_url);

    oldPage->deleteLater();
    if (oldChannel)
        oldChannel->deleteLater();
    // 旧 profile 必须等旧 page 真正销毁后再删，否则渲染进程会带着半关闭状态退出。
    QTimer::singleShot(400, oldProfile, [oldProfile]() { delete oldProfile; });
}

