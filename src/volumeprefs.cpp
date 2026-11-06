#include "volumeprefs.h"

namespace qtrs {

QString volumePreferenceScope(bool isCt, bool modalityKnown)
{
    if (!modalityKnown) return QString();
    return isCt ? QStringLiteral("ct") : QStringLiteral("mr");
}

QString volumePreferenceKey(bool isCt, bool modalityKnown, const QString &leaf)
{
    const QString scope = volumePreferenceScope(isCt, modalityKnown);
    if (scope.isEmpty()) return QStringLiteral("volume/%1").arg(leaf);
    return QStringLiteral("volume/%1/%2").arg(scope, leaf);
}

} // namespace qtrs
