// <sys/socket.h> for the mingw GUI cross build. See ../README.md.
//
// Deliberately NOT a winsock wrapper. Winsock's shapes are close enough to
// POSIX's that mapping them looks like a fifteen-minute job, and it is not:
// SOCKET is an unsigned handle rather than a small int, close() is
// closesocket(), errno is WSAGetLastError(), and none of it works at all until
// somebody calls WSAStartup. Half-doing that would leave an OSC server that
// binds and then silently never receives.
//
// So: the types are declared, the calls fail with ENOSYS, and
// ctl::OscServer::start() returns false with "socket() failed" -- which is a
// path it already has, and which the UI already reports. The OSC *parser* in
// the same translation unit is untouched portable code and is compiled and
// linked as-is.
#pragma once
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int socklen_t;
typedef unsigned short sa_family_t;

#define AF_INET      2
#define AF_INET6    23
#define SOCK_DGRAM   2
#define SOCK_STREAM  1
#define SOL_SOCKET   0xffff
#define SO_REUSEADDR 0x0004
#define MSG_DONTWAIT 0x0040

struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

int     socket(int domain, int type, int protocol);
int     bind(int fd, const struct sockaddr* addr, socklen_t len);
int     setsockopt(int fd, int level, int optname, const void* val, socklen_t len);
ssize_t recvfrom(int fd, void* buf, size_t len, int flags,
                 struct sockaddr* from, socklen_t* fromlen);
ssize_t sendto(int fd, const void* buf, size_t len, int flags,
               const struct sockaddr* to, socklen_t tolen);

#ifdef __cplusplus
}
#endif
