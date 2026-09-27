#ifndef HOST_STUB_EXTRA_H
#define HOST_STUB_EXTRA_H

/*
 * Force included by `make host-syntax` (-include extra.h). Holds the few
 * POSIX names that MinGW's fcntl.h and signal.h do not provide.
 */

#define F_GETFL 3
#define F_SETFL 4
#define O_NONBLOCK 0x4000
#define SIGPIPE 13

int fcntl(int fd, int command, ...);

#endif
