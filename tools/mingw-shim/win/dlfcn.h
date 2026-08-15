// <dlfcn.h> for the mingw GUI cross build. See README.md in this directory.
//
// The one shim in this directory that is a real implementation rather than a
// stub: LoadLibraryW / GetProcAddress / FreeLibrary genuinely are what dlopen /
// dlsym / dlclose mean on Windows, so src/plugin/clap_host.cpp compiles and
// WORKS unmodified. CLAP's ABI is identical on both platforms; the loader was
// the only platform-shaped part of it.
//
// (src/plugin/lv2_host.cpp also includes this header, but it needs lilv too and
// is excluded from the Windows link -- see win_stubs.cpp.)
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Only the flags this codebase passes. RTLD_LAZY/RTLD_NOW have no Win32
// meaning -- the loader always resolves everything at LoadLibrary time -- and
// RTLD_LOCAL is the only sharing model Windows has, so all four are accepted
// and ignored.
#define RTLD_LAZY   0x0001
#define RTLD_NOW    0x0002
#define RTLD_LOCAL  0x0000
#define RTLD_GLOBAL 0x0100

void* dlopen(const char* path, int flags);
void* dlsym(void* handle, const char* symbol);
int   dlclose(void* handle);
// Follows POSIX's rule: returns the last error once, then null.
char* dlerror(void);

#ifdef __cplusplus
}
#endif
