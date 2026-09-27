#ifndef HOST_STUB_SYS_SOCKET_H
#define HOST_STUB_SYS_SOCKET_H

/*
 * Minimal POSIX socket declarations so that src/rtsp_server.c can be
 * syntax checked on a host without POSIX sockets (MinGW).
 *
 * These are declarations only. Nothing here is linked or executed; the goal is
 * to catch typos, missing fields and new warnings before the real cross build
 * on the Ubuntu VM. See `make host-syntax`.
 */

#include <stddef.h>
#include <sys/time.h>

#define AF_INET 2
#define SOCK_STREAM 1
#define SOL_SOCKET 0xFFFF
#define SO_REUSEADDR 0x0004
#define SO_RCVTIMEO 0x1006
#define SO_SNDTIMEO 0x1005
#define MSG_NOSIGNAL 0x4000
#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2
#define INADDR_ANY 0U

struct sockaddr {
    unsigned short sa_family;
    char sa_data[14];
};

struct in_addr {
    unsigned int s_addr;
};

struct sockaddr_in {
    unsigned short sin_family;
    unsigned short sin_port;
    struct in_addr sin_addr;
    char sin_zero[8];
};

int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr *address, unsigned int length);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *address, unsigned int *length);
int setsockopt(int fd, int level, int name, const void *value,
               unsigned int length);
int shutdown(int fd, int how);
int close(int fd);
long send(int fd, const void *buffer, size_t length, int flags);
long recv(int fd, void *buffer, size_t length, int flags);

#endif
