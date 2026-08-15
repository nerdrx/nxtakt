// <pwd.h> for the mingw GUI cross build. See README.md in this directory.
//
// Windows has no /etc/passwd and no uid; what the app actually wants from this
// header is one thing -- the user's home directory, as a fallback when $HOME is
// unset -- so that is what getpwuid() answers, from %USERPROFILE%.
#pragma once
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// mingw-w64's <sys/types.h> has pid_t and off_t but no uid_t/gid_t -- there is
// nothing on the platform for them to mean. Defined here rather than in the
// unistd.h shim because this is the header whose declarations need them.
#ifndef __LAT_UID_T_DEFINED
#define __LAT_UID_T_DEFINED
typedef int uid_t;
typedef int gid_t;
#endif

struct passwd {
    char* pw_name;
    char* pw_dir;
    char* pw_shell;
};

// Never null on success; returns null only if the environment has neither
// USERPROFILE nor HOMEDRIVE+HOMEPATH, which does not happen on a real session.
// The returned storage is static, exactly as POSIX allows.
struct passwd* getpwuid(uid_t uid);
uid_t getuid(void);

#ifdef __cplusplus
}
#endif
