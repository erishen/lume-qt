#pragma once

#include <QElapsedTimer>
#include <QMainWindow>
#include <QUrl>

class QTabBar;
class QStackedWidget;
class QWebEngineView;
class QWebEngineProfile;
class QWebEnginePage;
class AppSchemeHandler;
class Bridge;
class NativeVitalStrip;

/**
 * MainWindow — 宿主窗口。
 *
 * 布局：顶部 Tab 在「Web 监护台」和「原生影像（OpenGL）」之间切换；
 *       底部固定高度的原生关键波形条 NativeVitalStrip 始终常驻。
 *
 * 为什么关键通道不走 WebView：网页那条链路要过 QWebChannel IPC 与渲染进程
 * 合成，抖动量级不可控（GC、后台节流、系统降频）。术中关键波形与报警的
 * 主链路应直连 Qt 信号 + QPainter 绘制，WebView 只承担信息展示。
 * 通信：QWebChannel 将 Bridge 对象暴露给 JS，前端经 qwebchannel.js
 *       连接（见 frontend/src/qwebchannel.ts）。
 * 诊断：自定义 QWebEnginePage 把前端 console.log / JS 异常 / WebGL 错误
 *       转发到 Qt 日志（stderr），避免“前端全黑但终端毫无线索”。
 */
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QSlider;
class QLabel;
class QWebChannel;
class QWebEngineProfile;
class QSettings;
class VolumeView;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private:
    void setupWebChannel();
    void setupNativeVolume();
    void setupConsoleBridge();
    void setupNativeStrip();
    /// 把顶层 Tab 可见性下发给 Web 页，暂停/恢复 React 的持续绘制与数据订阅。
    void setWebTabActive(bool active);
    /// 报警阈值：env 兜底 → Lume 下发覆盖；每次生效都记一条 alarm.config 台账。
    void applyAlarmConfig();
    /// 窗值偏好按模态分桶：CT 与 MR/a.u. 不能共用一套 W/L（会把 1500/300 夹成 33/83）。
    /// 模态未知时退回旧的全局键，避免启动时猜错桶。
    QString volumeSettingsKey(const QString &leaf) const;
    QVariant volumeSettingsValue(QSettings &settings, const QString &leaf,
                                 const QVariant &fallback) const;
    bool volumeSettingsContains(QSettings &settings, const QString &leaf) const;
    void reloadWebView();
    void installDiagnosticsScript(QWebEngineProfile *profile);

    QWebEngineView *m_view = nullptr;
    /// 页面脚本就绪前不要向 QWebChannel 送实时信号：否则启动期样本会在
    /// 传输层排队，页面加载完一次补送，直接把 p99 顶到几十秒。
    bool m_webReady = false;
    QWebEngineProfile *m_profile = nullptr;
    QWebEnginePage *m_page = nullptr;
    QWebChannel *m_channel = nullptr;
    QUrl m_url;
    AppSchemeHandler *m_schemeHandler = nullptr;
    Bridge *m_bridge = nullptr;
    NativeVitalStrip *m_strip = nullptr;
    QTabBar *m_tabs = nullptr;
    QStackedWidget *m_pages = nullptr;
    VolumeView *m_volumeView = nullptr;
    QLabel *m_volumeStatus = nullptr;
    QComboBox *m_volumePreset = nullptr;
    QCheckBox *m_volumeOverlay = nullptr;
    QDoubleSpinBox *m_windowWidthBox = nullptr;
    QDoubleSpinBox *m_windowLevelBox = nullptr;
    QCheckBox *m_boneBox = nullptr;
    QLabel *m_boneHint = nullptr;
    QComboBox *m_clipCombo = nullptr;
    QSlider *m_clipSlider = nullptr;
    bool m_syncingWindow = false;
    QLabel *m_volumeLegendText = nullptr;
    QString m_volumeSourceText;
    /// 手动调窗前的预设档（0–3），供「恢复上次预设」按钮/快捷键回退。
    int m_lastPresetIndex = 0;
    /// 重启后要恢复的手动窗值；等体数据到达（有 HU 范围可夹取）再套用。
    bool m_hasPendingManualWindow = false;
    double m_pendingWindowWidth = 80.0;
    double m_pendingWindowLevel = 40.0;
    /// 手动拖窗会连续发信号，落盘节流到 500 ms，避免每帧写一次 QSettings。
    QElapsedTimer m_windowSaveClock;
    /// 启动恢复与体数据到达时的「程序性改控件」不能回写偏好，
    /// 否则恢复出来的值会把用户真正的上次选择覆盖掉。
    bool m_restoringSettings = false;
    /// 当前体数据的模态；已知后才按 ct/mr 分桶读写偏好。
    bool m_volumeModalityKnown = false;
    bool m_volumeIsCt = false;
};
