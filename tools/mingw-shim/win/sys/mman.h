// <sys/mman.h> for the mingw GUI cross build. See ../README.md.
//
// This is NOT a Windows shared-memory implementation. It is the set of
// declarations src/ipc/shm.h needs in order to COMPILE, backed by functions
// that all fail with ENOSYS -- so daemon mode is unreachable on Windows rather
// than subtly broken. The real thing is CreateFileMappingW/MapViewOfFile plus
// named mutexes, i.e. a second implementation of src/ipc/shm.h.
#pragma once
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

#define MAP_SHARED  0x01
#define MAP_PRIVATE 0x02
#define MAP_ANON    0x20
#define MAP_ANONYMOUS MAP_ANON
#define MAP_FAILED  ((void*)-1)

void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off);
int   munmap(void* addr, size_t len);

// POSIX puts these in <sys/mman.h>, which is why they are here and not in the
// unistd.h shim.
int shm_open(const char* name, int oflag, mode_t mode);
int shm_unlink(const char* name);

#ifdef __cplusplus
}
#endif
