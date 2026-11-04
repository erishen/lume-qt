#pragma once

#include <QWebEngineUrlSchemeHandler>

/**
 * AppSchemeHandler — 将 app:// 自定义 scheme 的请求映射到本地前端构建产物目录。
 *
 * URL 形态：app://app/<path>（host 固定为 "app"）
 *  - app://app/          → 目录根，回退到 index.html
 *  - app://app/assets/x  → resources/web/assets/x
 *
 * 相比直接使用 file:// 加载 SPA，自定义 scheme 能规避 Chromium 对
 * file:// 下 fetch / 模块脚本 / 路由的诸多限制，是 Qt WebEngine 托管
 * 前端应用的推荐做法。
 */
class AppSchemeHandler : public QWebEngineUrlSchemeHandler
{
    Q_OBJECT

public:
    explicit AppSchemeHandler(const QString &rootDir, QObject *parent = nullptr);

    void requestStarted(QWebEngineUrlRequestJob *job) override;

private:
    QString m_rootDir;
};
