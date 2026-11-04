#include "consolepage.h"

#include <QDebug>
#include <QWebEngineProfile>

namespace {
/// 过滤前端诊断脚本自身产生的高频日志，避免刷屏；
/// 但 transport / shader / WebGL 上下文这类关键诊断必须保留——
/// 它们正是「画布全黑却无任何提示」时唯一可见的根因线索。
bool isDiagnosticProbe(const QString &message)
{
    if (!message.startsWith(QStringLiteral("[__qtrs_probe] ")))
        return false;
    return !message.contains(QStringLiteral("transport"))
        && !message.contains(QStringLiteral("shader-compile-failed"))
        && !message.contains(QStringLiteral("shader-link-failed"))
        && !message.contains(QStringLiteral("webgl-context-failed"));
}
} // namespace

ConsolePage::ConsolePage(QWebEngineProfile *profile, QObject *parent)
    : QWebEnginePage(profile, parent)
{
}

void ConsolePage::javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level,
                                            const QString &message,
                                            int lineNumber,
                                            const QString &sourceID)
{
    if (isDiagnosticProbe(message))
        return;

    // 着色器编译错误信息很长，做一次折叠，避免刷屏淹没其他线索
    QString text = message;
    constexpr int kMaxLen = 2000;
    if (text.size() > kMaxLen) {
        text = text.left(kMaxLen) + QStringLiteral(" …(已截断，共 %1 字符)").arg(message.size());
    }

    const QString where = sourceID.isEmpty()
        ? QStringLiteral("frontend")
        : QStringLiteral("%1:%2").arg(sourceID).arg(lineNumber);

    switch (level) {
    case QWebEnginePage::InfoMessageLevel:
        qInfo().noquote() << "[JS]" << where << text;
        break;
    case QWebEnginePage::WarningMessageLevel:
        qWarning().noquote() << "[JS 警告]" << where << text;
        break;
    case QWebEnginePage::ErrorMessageLevel:
        qWarning().noquote() << "[JS 错误]" << where << text;
        break;
    }
}
