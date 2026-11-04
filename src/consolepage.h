#pragma once

#include <QWebEnginePage>

class QWebEngineProfile;

/**
 * ConsolePage — 带诊断输出的 QWebEnginePage。
 *
 * 把前端的 console.log / console.error / JS 异常转发到 Qt 日志（stderr）。
 *
 * 为什么需要它：WebGL 着色器编译失败、JS 运行时异常等问题，前端只会表现为
 * “画布一片空白”，而在 Qt 应用里这些信息默认完全不可见——排查只能靠猜测。
 * 转发到终端后，前端 console.log("[DicomPanel] ...") 会直接出现在启动终端，
 * 着色器编译错误也能被捕获并打印。
 */
class ConsolePage : public QWebEnginePage
{
    Q_OBJECT

public:
    /// 必须传入 profile：页面所属 profile 决定了 app:// scheme handler 等设置，
    /// 若使用默认 profile，自定义 scheme 将无法解析（页面直接加载失败）。
    explicit ConsolePage(QWebEngineProfile *profile, QObject *parent = nullptr);

protected:
    /// 前端 console.* 消息（level: InfoMessageLevel / WarningMessageLevel / ErrorMessageLevel）
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level,
                                  const QString &message,
                                  int lineNumber,
                                  const QString &sourceID) override;
};
