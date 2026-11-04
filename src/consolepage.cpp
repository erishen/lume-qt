#include "consolepage.h"

#include <QDebug>
#include <QWebEngineProfile>

namespace {
/// 过滤前端自己注入的诊断探针日志，避免刷屏（它们由 installFrontendDiagnostics 标记）
bool isDiagnosticProbe(const QString &message)
{
    return message.startsWith(QStringLiteral("[__qtrs_probe]"));
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
