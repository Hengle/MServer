#include <poll.h>
#include "ev.hpp"

const char *__BACKEND__ = "poll";

// 是否要使用wepoll替换
// https://github.com/piscisaureus/wepoll
// wepoll使用的机制 NtDeviceIoControlFile 是一个未公开的机制
// 它使用IOCTL_AFD_POLL标记来实现类似epoll_ctl的功能

/// backend using poll implement
class EVBackend final
{
public:
    EVBackend();
    ~EVBackend();

    void wait(class EV *ev_loop, int64_t timeout);
    void modify(int32_t fd, int32_t old_ev, int32_t new_ev);

private:
    std::vector<int32_t> _fd_index;
    std::vector<struct pollfd> _poll_fd;
};

EVBackend::EVBackend() : _fd_index(1024, -1)
{
    _poll_fd.reserve(1024);
}

EVBackend::~EVBackend() {}

void EVBackend::wait(class EV *ev_loop, int64_t timeout)
{
    /**
     * WSAPoll有个bug，connect失败不会触发事件，直到win10 2004才修复
     * https://stackoverflow.com/questions/21653003/is-this-wsapoll-bug-for-non-blocking-sockets-fixed
	 * https://docs.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsapoll
	 * As of Windows 10 version 2004, when a TCP socket fails to connect, (POLLHUP \| POLLERR \| POLLWRNORM) is indicated
    */

    int32_t ev_count = poll(_poll_fd.data(), _poll_fd.size(), timeout * 1000);
    if (EXPECT_FALSE(ev_count < 0))
    {
        switch (errno)
        {
        case EINTR: return;
        case ENOMEM: ELOG("poll ENOMEM"); return;
        default:
            ELOG("poll fatal, %s(%d)", strerror(errno), errno);
            assert(false);
            return;
        }
    }

    for (const pollfd *p = _poll_fd.data(); ev_count > 0; p++)
    {
        if (!p->revents) continue;

        if (EXPECT_FALSE(p->revents & POLLNVAL))
        {
            ELOG("poll invalid fd: %d", p->fd);
            assert(false);
        }
        else
        {
            int32_t events =
                (p->revents & (POLLOUT | POLLERR | POLLHUP) ? EV_WRITE : 0)
                | (p->revents & (POLLIN | POLLERR | POLLHUP) ? EV_READ : 0);

            ev_loop->fd_event(p->fd, events);
        }
        ev_count--;
    }
}

void EVBackend::modify(int32_t fd, int32_t old_ev, int32_t new_ev)
{
    if (old_ev == new_ev) return;

    if (EXPECT_FALSE(_fd_index.size() < size_t(fd + 1)))
    {
        _fd_index.resize(fd + 1, -1);
    }

    int32_t index = _fd_index[fd];
    if (index < 0)
    {
        _fd_index[fd] = index = (int32_t)_poll_fd.size();
        _poll_fd.resize(index + 1);
        _poll_fd[index].fd = fd;
    }
    assert(_poll_fd[index].fd == fd);

    if (new_ev)
    {
        _poll_fd[index].events =
            (new_ev & EV_READ ? POLLIN : 0) | (new_ev & EV_WRITE ? POLLOUT : 0);
    }
    else
    {
        _fd_index[fd] = -1;
        // 不如果不是数组的最后一个，则用数组最后一个位置替换当前位置，然后删除最后一个
        int32_t fd_count = (int)_poll_fd.size();
        if (EXPECT_TRUE(index < --fd_count))
        {
            _poll_fd[index]               = _poll_fd[fd_count];
            _fd_index[_poll_fd[index].fd] = index;
        }
        _poll_fd.pop_back();
    }
}
