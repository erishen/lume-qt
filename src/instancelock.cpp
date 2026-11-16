#include "instancelock.h"

#include <QByteArray>
#include <QFile>

#include <cerrno>

#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

InstanceLock::~InstanceLock()
{
    release();
}

bool InstanceLock::acquire(const QString &path, qint64 *holderPid)
{
    if (holderPid)
        *holderPid = -1;
    release();

    // O_CREAT 只负责「文件不存在就建」，不承担互斥；互斥由下面的 flock 完成。
    const int fd = ::open(path.toUtf8().constData(), O_CREAT | O_RDWR, 0644);
    if (fd < 0)
        return false;

    // LOCK_NB：拿不到立即返回，不阻塞启动（阻塞等锁在桌面应用里表现为卡在启动画面）。
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (holderPid) {
            QFile existing(path);
            if (existing.open(QIODevice::ReadOnly))
                *holderPid = existing.readLine().trimmed().toLongLong();
        }
        ::close(fd);
        return false;
    }

    // 拿到锁之后才写 pid：此时文件归我们独占，写入不会与别人交叉。
    const QByteArray pid = QByteArray::number(static_cast<qint64>(::getpid())) + '\n';
    if (::ftruncate(fd, 0) == 0)
        ::write(fd, pid.constData(), static_cast<size_t>(pid.size()));
    // 不把 fd 泄漏给子进程：否则子进程继承 fd 会连带继承锁的生命周期。
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);

    m_fd = fd;
    m_path = path;
    return true;
}

void InstanceLock::release()
{
    if (m_fd < 0)
        return;
    ::flock(m_fd, LOCK_UN);
    ::close(m_fd);
    m_fd = -1;
    // 故意不删锁文件：删除会在「路径消失」与「下一个进程新建 inode」之间开一个窗口，
    // 让仍在收尾的本进程与新建者可能同时认为自己持锁。内核已随 fd 释放锁，残留无害。
    m_path.clear();
}
