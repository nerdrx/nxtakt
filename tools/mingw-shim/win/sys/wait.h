// <sys/wait.h> for the mingw GUI cross build. See ../README.md.
//
// Process reaping for a daemon this build cannot spawn in the first place
// (fork() fails), so waitpid() failing with ECHILD is consistent rather than
// merely convenient.
#pragma once
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WNOHANG   1
#define WUNTRACED 2

#define WIFEXITED(s)    (((s) & 0x7f) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xff)
#define WIFSIGNALED(s)  (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)
#define WTERMSIG(s)     ((s) & 0x7f)
#define WIFSTOPPED(s)   (((s) & 0xff) == 0x7f)
#define WSTOPSIG(s)     WEXITSTATUS(s)

pid_t waitpid(pid_t pid, int* status, int options);

#ifdef __cplusplus
}
#endif
