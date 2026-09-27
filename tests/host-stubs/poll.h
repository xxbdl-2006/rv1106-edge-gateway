#ifndef HOST_STUB_POLL_H
#define HOST_STUB_POLL_H

struct pollfd {
    int fd;
    short events;
    short revents;
};

#define POLLIN 0x001
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010
#define POLLNVAL 0x020

int poll(struct pollfd *fds, unsigned int count, int timeout_ms);

#endif
