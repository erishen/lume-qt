#pragma once

#include <QMainWindow>

class QWebEngineView;
class QWebEngineProfile;
class AppSchemeHandler;
class Bridge;

/**
 * MainWindow — 宿主窗口。
 *
 * 布局：单一 QWebEngineView 全窗口渲染 React 前端。
 * 通信：QWebChannel 将 Bridge 对象暴露给 JS，前端经 qwebchannel.js
 *       连接（见 frontend/src/qwebchannel.ts）。
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private:
    void setupWebChannel();

    QWebEngineView *m_view = nullptr;
    QWebEngineProfile *m_profile = nullptr;
    AppSchemeHandler *m_schemeHandler = nullptr;
    Bridge *m_bridge = nullptr;
};
