#pragma once

#include <QMainWindow>

class QWebEngineView;
class QWebEngineProfile;
class QWebEnginePage;
class AppSchemeHandler;
class Bridge;
class NativeVitalStrip;

/**
 * MainWindow — 宿主窗口。
 *
 * 布局：QWebEngineView（React 前端，占满剩余空间）+ 底部固定高度的
 *       原生关键波形条 NativeVitalStrip。
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

    QWebEngineView *m_view = nullptr;
    QWebEngineProfile *m_profile = nullptr;
    QWebEnginePage *m_page = nullptr;
    AppSchemeHandler *m_schemeHandler = nullptr;
    Bridge *m_bridge = nullptr;
    NativeVitalStrip *m_strip = nullptr;
    VolumeView *m_volumeView = nullptr;
    QLabel *m_volumeStatus = nullptr;
    QComboBox *m_volumePreset = nullptr;
    QCheckBox *m_volumeOverlay = nullptr;
    QDoubleSpinBox *m_windowWidthBox = nullptr;
    QDoubleSpinBox *m_windowLevelBox = nullptr;
    QCheckBox *m_boneBox = nullptr;
    QComboBox *m_clipCombo = nullptr;
    QSlider *m_clipSlider = nullptr;
    bool m_syncingWindow = false;
    QLabel *m_volumeLegendText = nullptr;
    QString m_volumeSourceText;
};
