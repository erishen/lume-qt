#pragma once

#include <QByteArray>
#include <QWebEngineUrlSchemeHandler>

#include <functional>

/**
 * AppSchemeHandler — 将 app:// 自定义 scheme 的请求映射到本地前端构建产物目录。
 *
 * URL 形态：app://app/<path>（host 固定为 "app"）
 *  - app://app/                     → 目录根，回退到 index.html
 *  - app://app/assets/x             → resources/web/assets/x
 *  - app://app/__volume/current.bin → 由 setVolumeProvider 提供的二进制体积（虚拟资源）
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

    /// 体积数据的读取回调（返回空表示当前没有可用数据）。
    ///
    /// 为什么需要这条通道：体积是 2MB 级的 uint8 二进制。走 QWebChannel
    /// 时它只能被序列化成字符串，实测 2,097,152 字节到前端只剩
    /// 2,097,145 个字符（UTF-8 转换把非法序列吃掉了）——不能用于影像数据。
    /// 改由 Chromium 直接以 application/octet-stream 取走，前端
    /// fetch(...).arrayBuffer() 拿到的是真正的二进制：无编码放大、
    /// 无逐字节解码。
    using VolumeProvider = std::function<QByteArray()>;
    void setVolumeProvider(VolumeProvider provider);

private:
    void serveVolume(QWebEngineUrlRequestJob *job);

    QString m_rootDir;
    VolumeProvider m_volumeProvider;
};
