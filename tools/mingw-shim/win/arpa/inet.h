// <arpa/inet.h> for the mingw GUI cross build. See ../README.md.
#pragma once
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

// Real, and worth being real: it is pure string parsing with no socket layer
// behind it, and ctl::OscServer uses its return value to decide whether the
// configured address was even well formed before it tries to bind. Getting a
// truthful "that is not an address" out of a build with no sockets is strictly
// better than one more ENOSYS.
int inet_pton(int af, const char* src, void* dst);
const char* inet_ntop(int af, const void* src, char* dst, socklen_t size);

#ifdef __cplusplus
}
#endif
