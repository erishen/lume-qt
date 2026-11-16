#include <gtest/gtest.h>

#include <QTemporaryDir>

#include "../src/instancelock.h"

// 单实例锁要守的是三件事：同一路径只能有一个持有者、别人持有时要能报出持有者 pid、
// 释放后可以再次拿到。这里刻意用两个独立的 InstanceLock 实例去抢同一个路径——
// flock 的锁属于「打开的文件描述」，同进程内两次 open 也会互相冲突，因此不需要 fork。

TEST(InstanceLock, FirstAcquireSucceeds)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("instance.lock"));

    InstanceLock lock;
    EXPECT_TRUE(lock.acquire(path));
}

TEST(InstanceLock, SecondAcquireFailsAndReportsHolder)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("instance.lock"));

    InstanceLock first;
    ASSERT_TRUE(first.acquire(path));

    InstanceLock second;
    qint64 holderPid = -1;
    EXPECT_FALSE(second.acquire(path, &holderPid));
    EXPECT_GT(holderPid, 0);
}

TEST(InstanceLock, ReleaseLetsAnotherAcquire)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("instance.lock"));

    InstanceLock first;
    ASSERT_TRUE(first.acquire(path));
    InstanceLock second;
    EXPECT_FALSE(second.acquire(path));

    first.release();
    EXPECT_TRUE(second.acquire(path));
}

// 锁文件残留（上一次被 kill -9）不应阻止新进程拿锁：flock 没有「陈旧锁」这个概念。
TEST(InstanceLock, StaleFileFromDeadProcessIsHarmless)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("instance.lock"));

    QFile stale(path);
    ASSERT_TRUE(stale.open(QIODevice::WriteOnly));
    stale.write("999999\n");
    stale.close();

    InstanceLock lock;
    EXPECT_TRUE(lock.acquire(path));
}
