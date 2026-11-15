#include "schemepath.h"

#include <QLatin1Char>
#include <QLatin1String>
#include <QRegularExpression>
#include <QStringLiteral>

namespace scheme {

QString resolveRequestPath(const QString &requestPath)
{
    QString path = requestPath;
    if (path.startsWith(QLatin1Char('/'))) {
        path.remove(0, 1);
    }

    if (path.isEmpty()) {
        return QStringLiteral("index.html");
    }

    // 路由式路径没有扩展名。前端一旦用 history 路由，深链刷新请求的就是
    // /console 这种路径；宿主不回退，页面就只剩 404。
    static const QRegularExpression routePathRe(QStringLiteral("^[A-Za-z0-9/_-]+$"));
    if (routePathRe.match(path).hasMatch()) {
        return QStringLiteral("index.html");
    }

    return path;
}

} // namespace scheme
