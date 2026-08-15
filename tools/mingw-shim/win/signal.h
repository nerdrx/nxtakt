// <signal.h> for the mingw GUI cross build. See README.md in this directory.
//
// The C runtime's six signals (SIGINT, SIGTERM, SIGABRT, SIGFPE, SIGILL,
// SIGSEGV) are all mingw declares, because they are all Windows has. Two more
// are referenced by this codebase:
//
//   SIGPIPE   src/main.cpp ignores it, so that a closed stdout cannot kill the
//             app. On Windows a closed handle is an error return, never a
//             signal, so the problem it guards against does not exist -- but
//             signal(SIGPIPE, SIG_IGN) still has to compile.
//   SIGKILL   src/ui/engine_handle.cpp uses it to stop a daemon it started.
//             kill() fails with ENOSYS here (nothing can be spawned in the
//             first place), so the number only needs to exist.
//
// The values are the Linux ones. Nothing on this platform interprets them:
// mingw's signal() rejects any number it does not know and returns SIG_ERR,
// which is the correct outcome for both of the above.
#pragma once
#include_next <signal.h>

#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif
