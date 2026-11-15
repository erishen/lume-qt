#include <gtest/gtest.h>

#include <QString>
#include <QStringLiteral>

#include "../src/schemepath.h"

// app://app/<path> 的归一化规则。这里守的是三件事：
// 根与 undefined 路由回退到入口页、带扩展名的资源不被改写、可疑路径不借回退洗白。

TEST(SchemePath, RootFallsBackToIndex)
{
    EXPECT_EQ(scheme::resolveRequestPath(QStringLiteral("/")), QStringLiteral("index.html"));
    EXPECT_EQ(scheme::resolveRequestPath(QString()), QStringLiteral("index.html"));
}

TEST(SchemePath, HistoryDeepLinksFallBack)
{
    EXPECT_EQ(scheme::resolveRequestPath(QStringLiteral("/console")), QStringLiteral("index.html"));
    EXPECT_EQ(scheme::resolveRequestPath(QStringLiteral("/review/7")), QStringLiteral("index.html"));
    EXPECT_EQ(scheme::resolveRequestPath(QStringLiteral("console")), QStringLiteral("index.html"));
}

TEST(SchemePath, AssetRequestsPassThrough)
{
    EXPECT_EQ(scheme::resolveRequestPath(QStringLiteral("/assets/index-abc.js")),
              QStringLiteral("assets/index-abc.js"));
    EXPECT_EQ(scheme::resolveRequestPath(QStringLiteral("/index.html")), QStringLiteral("index.html"));
    EXPECT_EQ(scheme::resolveRequestPath(QStringLiteral("/favicon.ico")), QStringLiteral("favicon.ico"));
}

TEST(SchemePath, VirtualVolumeRequestPassesThrough)
{
    // 虚拟资源由 serveVolume 处理，不能被回退吃掉。
    EXPECT_EQ(scheme::resolveRequestPath(QStringLiteral("/__volume/current.bin")),
              QStringLiteral("__volume/current.bin"));
}

TEST(SchemePath, SuspiciousPathsAreNotRewritten)
{
    // 带 . / \ 的路径不满足路由字符集，原样交给 canonical 校验去 404，
    // 不会被回退悄悄洗成首页。
    EXPECT_EQ(scheme::resolveRequestPath(QStringLiteral("/../etc/passwd")),
              QStringLiteral("../etc/passwd"));
    EXPECT_EQ(scheme::resolveRequestPath(QStringLiteral("/a_b/c.d")), QStringLiteral("a_b/c.d"));
}
