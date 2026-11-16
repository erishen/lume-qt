#ifndef INSTANCELOCK_H
#define INSTANCELOCK_H

#include <QString>
#include <QtGlobal>

/**
 * 进程级单实例锁，基于 flock(2)。
 *
 * 为什么不是「O_CREAT | O_EXCL 建锁 + 读 pid 判存活 + 删陈旧锁」：那条路径里
 * 「判断陈旧」与「删除陈旧」是两步，A、B 同时判定旧锁已死时，A 建好的新锁可能
 * 被 B 的删除动作一起删掉，随后 B 也建锁成功——两个实例同时认为自己独占。
 *
 * flock 把所有权交给内核：锁跟着打开的文件描述符走，进程被 kill -9 时 fd 由内核
 * 关闭、锁立即释放，因此根本不存在「陈旧锁」这个状态，也就不需要在我们的代码里
 * 判定存活、删除文件。文件里写的 pid 只用于失败时的诊断提示，不参与所有权判定。
 *
 * 代价：flock 只对本地文件系统可靠，NFS 上的语义依赖服务端实现（本应用状态目录
 * 在本地，不跨机共享）；锁文件在退出后残留，内容为最后一次持有者的 pid，无害。
 */
class InstanceLock
{
public:
    InstanceLock() = default;
    ~InstanceLock();

    InstanceLock(const InstanceLock &) = delete;
    InstanceLock &operator=(const InstanceLock &) = delete;

    /// 尝试拿锁。成功返回 true；失败返回 false，holderPid 尽力填成文件里记录的 pid
    /// （读不到时为 -1）。非阻塞，拿不到立即返回。
    bool acquire(const QString &path, qint64 *holderPid = nullptr);

    /// 释放锁并关闭 fd。重复调用安全。
    void release();

private:
    int m_fd = -1;
    QString m_path;
};

#endif // INSTANCELOCK_H
