// Implementations for the POSIX declarations in this directory.
// See README.md for the policy: a stub fails, it never fakes.
//
// Compiled into the Windows GUI only (`make -f Makefile.mingw gui`).
#if !defined(_WIN32)
#error "tools/mingw-shim/win is for the Windows cross build only"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <pwd.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <dlfcn.h>

namespace {

// Every unimplemented entry point goes through here, so that a build which
// somehow does reach one leaves a trace naming the function instead of just an
// errno. Once per function, because a failing poll() in a loop would otherwise
// be a fire hose.
int nosys(const char* fn) {
    static const char* seen[32];
    static int n = 0;
    bool fresh = true;
    for (int i = 0; i < n; ++i) if (std::strcmp(seen[i], fn) == 0) { fresh = false; break; }
    if (fresh) {
        if (n < 32) seen[n++] = fn;
        std::fprintf(stderr, "[nxtakt warn] %s() is not implemented on Windows "
                             "(tools/mingw-shim/win)\n", fn);
    }
    errno = ENOSYS;
    return -1;
}

} // namespace

extern "C" {

// ---- src/ipc: shared memory ------------------------------------------------
int shm_open(const char*, int, mode_t)          { return nosys("shm_open"); }
int shm_unlink(const char*)                     { return nosys("shm_unlink"); }
int ftruncate(int, off_t)                       { return nosys("ftruncate"); }
int munmap(void*, size_t)                       { return nosys("munmap"); }

void* mmap(void*, size_t, int, int, int, off_t) {
    nosys("mmap");
    return MAP_FAILED;
}

// ---- src/ipc: spawning the daemon -----------------------------------------
pid_t fork(void)                                { return (pid_t)nosys("fork"); }
int   kill(pid_t, int)                          { return nosys("kill"); }

pid_t waitpid(pid_t, int* status, int) {
    if (status) *status = 0;
    nosys("waitpid");
    errno = ECHILD;                 // "no such child" is the truthful answer
    return -1;
}

// /proc/self/exe is the one path worth answering: it is how the GUI finds
// nxtaktd next to itself, and Windows has an exact equivalent.
ssize_t readlink(const char* path, char* buf, size_t bufsiz) {
    if (path && std::strcmp(path, "/proc/self/exe") == 0 && buf && bufsiz) {
        const DWORD n = GetModuleFileNameA(nullptr, buf, (DWORD)bufsiz);
        // GetModuleFileNameA NUL-terminates and returns the length without the
        // NUL, except on truncation where it returns bufsiz. readlink() does
        // not terminate at all, so returning the length is the whole contract.
        if (n > 0 && n < bufsiz) return (ssize_t)n;
    }
    return (ssize_t)nosys("readlink");
}

long sysconf(int name) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    switch (name) {
    case _SC_PAGESIZE:           return (long)si.dwPageSize;
    case _SC_NPROCESSORS_ONLN:   return (long)si.dwNumberOfProcessors;
    default:                     return nosys("sysconf");
    }
}

// ---- <unistd.h>: the take writer's flush -----------------------------------
// _commit() flushes a CRT fd's buffers to the device — the honest fsync.
int fsync(int fd) { return _commit(fd); }

// ---- <pwd.h> ---------------------------------------------------------------
uid_t getuid(void) { return 0; }

struct passwd* getpwuid(uid_t) {
    static struct passwd pw;
    static char dir[MAX_PATH * 2];
    static char name[256];
    static char shell[] = "";

    if (!pw.pw_dir) {
        const char* home = std::getenv("USERPROFILE");
        if (home && *home) {
            std::snprintf(dir, sizeof dir, "%s", home);
        } else {
            const char* dr = std::getenv("HOMEDRIVE");
            const char* pa = std::getenv("HOMEPATH");
            if (dr && pa) std::snprintf(dir, sizeof dir, "%s%s", dr, pa);
            else          std::snprintf(dir, sizeof dir, "C:\\");
        }
        const char* user = std::getenv("USERNAME");
        std::snprintf(name, sizeof name, "%s", (user && *user) ? user : "user");
        pw.pw_name  = name;
        pw.pw_dir   = dir;
        pw.pw_shell = shell;
    }
    return &pw;
}

// ---- sockets: declared, not implemented (see sys/socket.h) -----------------
int     socket(int, int, int)                                    { return nosys("socket"); }
int     bind(int, const struct sockaddr*, socklen_t)             { return nosys("bind"); }
int     setsockopt(int, int, int, const void*, socklen_t)        { return nosys("setsockopt"); }
ssize_t recvfrom(int, void*, size_t, int, struct sockaddr*, socklen_t*)
                                                                 { return (ssize_t)nosys("recvfrom"); }
ssize_t sendto(int, const void*, size_t, int, const struct sockaddr*, socklen_t)
                                                                 { return (ssize_t)nosys("sendto"); }
int     poll(struct pollfd*, nfds_t, int)                        { return nosys("poll"); }

// Real: dotted-quad parsing, no socket layer involved.
int inet_pton(int af, const char* src, void* dst) {
    if (af != AF_INET || !src || !dst) return -1;
    unsigned b[4] = {0, 0, 0, 0};
    int n = 0;
    const char* p = src;
    for (;;) {
        if (*p < '0' || *p > '9') return 0;
        unsigned v = 0, digits = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (unsigned)(*p++ - '0');
            if (++digits > 3 || v > 255) return 0;
        }
        if (n > 3) return 0;
        b[n++] = v;
        if (*p == 0) break;
        if (*p != '.') return 0;
        ++p;
    }
    if (n != 4) return 0;
    unsigned char* out = (unsigned char*)dst;
    out[0] = (unsigned char)b[0]; out[1] = (unsigned char)b[1];
    out[2] = (unsigned char)b[2]; out[3] = (unsigned char)b[3];
    return 1;
}

const char* inet_ntop(int af, const void* src, char* dst, socklen_t size) {
    if (af != AF_INET || !src || !dst) return nullptr;
    const unsigned char* b = (const unsigned char*)src;
    const int n = std::snprintf(dst, (size_t)size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return (n > 0 && n < size) ? dst : nullptr;
}

// ---- <dlfcn.h>: the one real implementation --------------------------------
static char g_dlerr[512];
static bool g_dlerrSet = false;

static void setDlError(const char* what, const char* path) {
    const DWORD e = GetLastError();
    std::snprintf(g_dlerr, sizeof g_dlerr, "%s(\"%s\") failed: Windows error %lu",
                  what, path ? path : "(null)", (unsigned long)e);
    g_dlerrSet = true;
}

void* dlopen(const char* path, int) {
    if (!path) return (void*)GetModuleHandleW(nullptr);   // handle to self

    // Plugin paths arrive as UTF-8 (they came out of a UTF-8 project file or a
    // UTF-8 directory walk), so widen rather than calling the A variant, which
    // would interpret them in the ANSI code page and fail on any non-ASCII
    // path.
    const int need = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    wchar_t stackBuf[MAX_PATH];
    wchar_t* w = stackBuf;
    wchar_t* heap = nullptr;
    if (need <= 0) { setDlError("dlopen", path); return nullptr; }
    if ((size_t)need > sizeof stackBuf / sizeof stackBuf[0]) {
        heap = (wchar_t*)std::malloc((size_t)need * sizeof(wchar_t));
        if (!heap) { setDlError("dlopen", path); return nullptr; }
        w = heap;
    }
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, need);

    // LOAD_WITH_ALTERED_SEARCH_PATH so a plugin's own DLLs, which sit next to
    // it in its bundle directory, are found -- the default search order looks
    // next to the EXE, not next to the DLL being loaded.
    HMODULE m = LoadLibraryExW(w, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) setDlError("dlopen", path);
    std::free(heap);
    return (void*)m;
}

void* dlsym(void* handle, const char* symbol) {
    if (!handle || !symbol) { setDlError("dlsym", symbol); return nullptr; }
    void* p = (void*)GetProcAddress((HMODULE)handle, symbol);
    if (!p) setDlError("dlsym", symbol);
    return p;
}

int dlclose(void* handle) {
    if (!handle) return 0;
    return FreeLibrary((HMODULE)handle) ? 0 : -1;
}

char* dlerror(void) {
    if (!g_dlerrSet) return nullptr;
    g_dlerrSet = false;                 // POSIX: reporting an error clears it
    return g_dlerr;
}

} // extern "C"
