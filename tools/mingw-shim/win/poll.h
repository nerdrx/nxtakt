// <poll.h> for the mingw GUI cross build. See README.md in this directory.
//
// Win32 does have WSAPoll, but nothing in this build calls WSAStartup, so
// there are no sockets to poll: poll() here fails with ENOSYS and the only
// caller (ctl::OscServer's reader thread, which never starts because socket()
// failed first) is already written to handle it.
#pragma once
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POLLIN   0x001
#define POLLPRI  0x002
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

struct pollfd {
    int   fd;
    short events;
    short revents;
};
typedef unsigned long nfds_t;

int poll(struct pollfd* fds, nfds_t nfds, int timeout);

#ifdef __cplusplus
}
#endif
