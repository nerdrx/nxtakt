// <netinet/in.h> for the mingw GUI cross build. See ../README.md.
#pragma once
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned short in_port_t;
typedef unsigned int   in_addr_t;

#define INADDR_ANY       ((in_addr_t)0x00000000)
#define INADDR_LOOPBACK  ((in_addr_t)0x7f000001)

struct in_addr { in_addr_t s_addr; };

struct sockaddr_in {
    sa_family_t    sin_family;
    in_port_t      sin_port;
    struct in_addr sin_addr;
    unsigned char  sin_zero[8];
};

// x86 is little-endian and this build targets nothing else, so these are the
// byte swap, unconditionally. (Keeping them here rather than in arpa/inet.h
// matches where a caller expects to find them after including this header.)
static inline unsigned short htons(unsigned short v) { return (unsigned short)((v << 8) | (v >> 8)); }
static inline unsigned short ntohs(unsigned short v) { return htons(v); }
static inline unsigned int   htonl(unsigned int v) {
    return (v << 24) | ((v & 0x0000ff00u) << 8) | ((v & 0x00ff0000u) >> 8) | (v >> 24);
}
static inline unsigned int   ntohl(unsigned int v) { return htonl(v); }

#ifdef __cplusplus
}
#endif
