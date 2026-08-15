# `tools/mingw-shim/win` — the Windows GUI's POSIX edges

This directory exists so that **not one line of `src/` had to grow a `#ifdef
_WIN32` for the Windows GUI build to compile**, beyond the three places that
genuinely need platform code (`gfx/gl.h`, `gfx/font.cpp`, and the Win32 backends
that were already there).

That is a deliberate trade. The alternative — sprinkling `#if defined(_WIN32)`
through `app.cpp`, `app_chrome.cpp`, `app_detail.cpp`, `app_devices.cpp`,
`app_engine.cpp`, `app_project.cpp`, `app_session.cpp`, `app_undo.cpp`,
`app_internal.h`, `control/learn.cpp` and `ui/engine_handle.cpp` — would have
touched eleven files owned by three different concerns to say the same thing
eleven times: *this platform does not have `<pwd.h>`*.

Everything here is on the include path of the **cross GUI build only**
(`make -f Makefile.mingw gui`). The native Linux build never sees it, and the
headless `engine_test.exe` cross build never sees it either.

## Two kinds of file

**Headers that make a POSIX include resolve.** `pwd.h`, `sys/mman.h`,
`sys/wait.h`, `poll.h`, `sys/socket.h`, `netinet/in.h`, `arpa/inet.h`, and a
`unistd.h` that `#include_next`s the real mingw one and adds the handful of
declarations it is missing (`ftruncate`, `readlink`, `sysconf`, `fork`,
`kill`).

**`win_posix.cpp` — the implementations.** Almost all of them fail, on purpose
and loudly-in-the-log-once, rather than pretending:

| Function | Behaviour on Windows | Consequence |
|---|---|---|
| `shm_open`, `shm_unlink`, `mmap`, `munmap`, `ftruncate` | fail with `ENOSYS` | `src/ipc` cannot attach, so daemon mode is unreachable. Opt-in only (`NXTAKT_ENGINE=daemon`), so the default in-process engine is unaffected. |
| `fork`, `execv`, `waitpid`, `kill` | fail with `ENOSYS` | the GUI cannot spawn `nxtaktd`. Same opt-in path. |
| `readlink` | fails, except `/proc/self/exe`, which is answered with `GetModuleFileNameA` | the "find the daemon next to me" logic gets a truthful answer if it is ever reached |
| `getpwuid`, `getuid` | a synthetic entry whose `pw_dir` is `%USERPROFILE%` | `$HOME`-relative config and sample paths land somewhere sane |
| `socket`, `bind`, `recvfrom`, `poll`, `inet_pton` | fail with `ENOSYS` | `ctl::OscServer::start()` returns false and says so. **The OSC *parser* in the same file is real, portable code and is compiled unchanged** — only the UDP server is missing, and only because nothing here calls `WSAStartup`. |
| `dlopen`, `dlsym`, `dlclose`, `dlerror` | **real**, over `LoadLibraryW`/`GetProcAddress` | CLAP plugin hosting is genuinely functional; this is the one entry that is not a stub |

`win_stubs.cpp` is the other half: the two translation units that could not be
compiled at all (`audio/midi_in.cpp` needs ALSA, `plugin/lv2_host.cpp` needs
lilv) are replaced by "not available on this platform" implementations of
exactly their public entry points.

## Rules

1. **Nothing in here may ever appear on the Linux include path.** It is
   referenced only by `WIN_SHIM_INC` in `Makefile.mingw`.
2. **A stub fails; it never fakes.** No function here returns a plausible
   success. Every failure path in the app that these feed already existed and
   is already exercised on Linux (no audio device, no MIDI, no daemon).
3. **Delete on sight.** Each of these is a placeholder for a real Windows
   implementation: WinMM/WinRT MIDI, winsock OSC, `CreateFileMappingW` IPC. As
   each one lands, its shim goes.
