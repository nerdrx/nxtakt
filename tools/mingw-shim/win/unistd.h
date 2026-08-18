// <unistd.h> for the mingw GUI cross build. See README.md in this directory.
//
// mingw-w64 ships a real <unistd.h> (it forwards to io.h/process.h and gives
// us access(), R_OK, close(), read(), write(), getpid(), unlink() and more).
// It is missing exactly five things this codebase reaches for, so this file
// chains to the real header and adds only those.
//
// #include_next is the reason this works: it re-runs the search for
// <unistd.h> starting AFTER the directory this file was found in, so the real
// mingw header is still included, once, with its full contents.
#pragma once
#include_next <unistd.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// src/ipc/shm.h sizes a shared region with this. Fails (ENOSYS) here.
int ftruncate(int fd, off_t length);

// src/ipc/client.h resolves /proc/self/exe to find nxtaktd next to the GUI.
// Answered for that one path via GetModuleFileNameA; everything else fails.
ssize_t readlink(const char* path, char* buf, size_t bufsiz);

#define _SC_PAGESIZE      30
#define _SC_PAGE_SIZE     _SC_PAGESIZE
#define _SC_NPROCESSORS_ONLN 84
long sysconf(int name);

// Daemon spawning. fork() cannot be emulated on Win32 and this build does not
// try: it returns -1/ENOSYS, so the caller's "could not start the engine
// process" path is what runs.
pid_t fork(void);
int   kill(pid_t pid, int sig);

// src/ipc/take.h names the per-user take directory with getuid() (glibc
// declares it here as well as in <pwd.h>, and take.h includes only this
// header) and durability-flushes a finished take with fsync(). getuid's
// definition already lives in win_posix.cpp for <pwd.h>; fsync maps to
// _commit(), the real CRT-fd flush.
uid_t getuid(void);
int   fsync(int fd);

#ifdef __cplusplus
}
#endif
