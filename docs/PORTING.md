# Porting NxTakt

Linux is the primary target. Windows is secondary. As of the `windows-cross`
CI job the **engine is tested on Windows** — the real test suite, cross-built
with mingw-w64 and executed by Wine on every push.

As of **first light (2026-08-15)** the **GUI runs**: `build-mingw/nxtakt.exe`
is a Windows binary that opens a Win32 window, brings up an OpenGL 3.3 core
context through WGL, draws the whole NX interface, loads a `.lattice` set with
libsndfile, opens a WASAPI endpoint, and responds to the keyboard and the
mouse. That has been done under Wine, once, by hand — not in CI, not on real
Windows hardware. Every claim below says which.

## Status

| Area | State |
|---|---|
| `src/audio/engine.cpp`, `src/core/common.cpp`, `tests/engine_test.cpp` | **Tested in CI.** Cross-compiled to `engine_test.exe` and run under Wine on every push. All 706 checks pass, exactly as on Linux. |
| `src/ui/window_win32.cpp` | **Ran.** Creates the window, the pixel format, the 3.3 core context; the message pump delivers keyboard and mouse. Compile-checked in CI, executed only by hand under Wine. |
| `src/audio/backend_win32.cpp` | **Ran.** WASAPI shared mode came up under Wine at 48 kHz / 1440 frames / 2 ch float32, first try, unmodified. Audible output has NOT been verified. |
| `src/gfx/gl_win32_loader.h` | **New, ran.** Resolves the 29 GL 2.0+ entry points through `wglGetProcAddress`. Linux does not compile a byte of it. |
| `src/gfx/font.cpp` | **Ported and fixed.** Windows font discovery (`%WINDIR%\Fonts`, then the font registry) plus a genuine cross-platform bug fix — see "What first light found". |
| `src/ui/app.cpp`, `app_*.cpp`, `src/control/*`, `src/core/*`, `src/gfx/*`, `src/ui/*` | **Compiled and running, unmodified.** No `#ifdef _WIN32` was added to any of them; `tools/mingw-shim/win/` supplies the POSIX headers they include. |
| `src/plugin/clap_host.cpp` | **Compiled and running, unmodified.** `dlopen` maps to `LoadLibraryW` in the shim, so CLAP hosting is real on Windows. No CLAP plugin has actually been loaded yet. |
| `src/plugin/lv2_host.cpp` | **Excluded.** Needs lilv. Stubbed by `tools/mingw-shim/win/win_stubs.cpp`; the scan reports "not available on Windows". |
| `src/audio/midi_in.cpp` | **Excluded.** ALSA sequencer. Stubbed; the app degrades to "no MIDI input" exactly as it does on a Linux box without snd-seq. |
| `src/control/osc.cpp` | **Compiled, server inert.** The OSC *parser* is real portable code and is linked. `socket()` fails with `ENOSYS` (nothing calls `WSAStartup`), so `OscServer::start()` returns false and says so. |
| `src/ipc/*`, `tests/ipc_test.cpp` | **Compiles via failing stubs, never runs.** `shm_open`/`mmap`/`fork` all fail, so daemon mode (`NXTAKT_ENGINE=daemon`) is unreachable. The default in-process engine is unaffected. |
| ASIO, WASAPI exclusive mode, WinMM MIDI, VST3 | **Not written.** |

"Ran" here means: on this machine, under Wine 11.15, inside headless gamescope,
with Mesa/radeonsi behind Wine's `opengl32`. It does not mean tested on
Windows, and it does not mean tested twice.

## Where the platform lives

Everything platform-specific is behind one of two interfaces: `IWindowBackend`
(`src/ui/window_backend.h`) and `AudioBackend` (`src/audio/backend.h`). Files
ending in `_win32.cpp` guard their entire contents with `#if defined(_WIN32)`,
so they compile to an empty translation unit on Linux and need no build-system
filtering.

| File | Platform | Notes |
|---|---|---|
| `src/ui/window_x11.cpp` | Linux | X11 + GLX |
| `src/ui/window_wayland.cpp` | Linux | Wayland + EGL, optional at build time |
| `src/ui/window_win32.cpp` | Windows | Win32 + WGL, OpenGL 3.3 core |
| `src/audio/backend.cpp` | Linux | JACK (also PipeWire) + ALSA fallback |
| `src/audio/backend_win32.cpp` | Windows | WASAPI shared mode |
| `src/plugin/lv2_host.cpp` | Linux | lilv + `dlfcn.h` |
| `src/gfx/gl_win32_loader.h` | Windows | the `wglGetProcAddress` loader; header-only, so it adds no translation unit to either build |
| `tools/mingw-shim/win/` | Windows | the POSIX headers mingw does not have, and the two files (ALSA MIDI, lilv) that had no portable half. Never on the Linux include path |

There are exactly five `#if defined(_WIN32)` sites in `src/` outside the
`*_win32.*` files: the factory declarations in `window_backend.h`, the platform
switch in `window.cpp`, the header selection in `gl.h`, and two in `font.cpp`
(one `#include <windows.h>`, one `findSystemFont`). Everything else in `src/` compiles
for both platforms from identical source, which is the property
`tools/mingw-shim/win/` exists to buy and the reason to keep it rather than
sprinkle conditionals through eleven files.

The factory declarations in `window_backend.h` are now themselves per-platform.
Declaring `createX11Backend()` unconditionally is what let `window.cpp` call it
from a Windows build and only find out at link time; the header now exposes
`createWin32Backend()` under `_WIN32` and the X11/Wayland pair otherwise, so
the mistake is a compile error rather than a link error.

## Building it

The headless subset, which is what CI builds and needs nothing but a compiler:

```
make -f Makefile.mingw config     # toolchain, flags, thread model
make -f Makefile.mingw            # engine_test.exe + the two Win32 objects
make -f Makefile.mingw check      # ... then run the suite under wine
```

The GUI, which needs a sysroot:

```
tools/win-sysroot.sh              # fetch pinned freetype/sndfile/samplerate + closure
make -f Makefile.mingw gui        # -> build-mingw/nxtakt.exe, with its DLLs beside it
tools/headless_win_test.sh -- demo.lattice     # run it under wine, screenshot it
```

Needs `mingw-w64` and `wine` (64-bit; nothing here is 32-bit), plus `gamescope`
for the headless run. `verify-sources` asserts that the cross build's file list
and warning flags still match the native `Makefile`, so the two cannot drift
into testing different code; CI runs it before building.

### If the machine has no cross-compiler

`tools/win-sysroot.sh --toolchain` also fetches a relocatable mingw-w64 GCC
(Arch's packages, extracted into `build-mingw/toolchain/root`; add its
`usr/bin` to `PATH`). That is how first light was reached — the machine had no
mingw-w64 and no way to install one — and it is exactly what CI does *not* do,
because CI has apt. GCC relocates itself from `argv[0]`, so an extracted `usr/`
tree works from anywhere as long as it stays intact.

Note for anyone repeating this: **check which C runtime your mingw targets
before fetching a sysroot.**

```
echo 'int main(){}' | x86_64-w64-mingw32-gcc -x c - -o /tmp/t.exe
x86_64-w64-mingw32-objdump -p /tmp/t.exe | grep -i 'DLL Name'
```

`api-ms-win-crt-*` means UCRT (current Arch, current Debian) and the sysroot
must come from MSYS2's `ucrt64`; `msvcrt.dll` means the old runtime and it must
come from `mingw64`. Mixing them gives you two heaps and two stdio states in
one process, and the failure is not at link time.

## What the first real cross-compile found

Worth recording, because the ratio is the interesting part.

**The hand-written Win32 code was essentially correct.** Both
`window_win32.cpp` and `backend_win32.cpp` compile with `-Wall -Wextra` and
produce zero diagnostics. Two conservative changes were made anyway:

* `SetProcessDpiAwarenessContext((HANDLE)-4)` now goes through `INT_PTR`. A
  bare `int` → 64-bit-pointer cast is implementation-defined and warns under
  `-Wint-to-pointer-cast` on some targets; widening first makes the sign
  extension explicit.
* `backend_win32.cpp` includes `<mmreg.h>` directly. `WIN32_LEAN_AND_MEAN`
  keeps `mmsystem.h` out of `windows.h`, and whether `audioclient.h` pulls
  `mmreg.h` in by itself differs between the Windows SDK and mingw-w64 —
  `WAVEFORMATEXTENSIBLE` and the `WAVE_FORMAT_*` tags used by `classify()` live
  there.

**The actual blocker was in the build, not the code.** `sizeof(lat::Engine)` is
about 2.35 MB — the realtime path never allocates, so every per-track buffer is
a fixed-size member — and `tests/engine_test.cpp` declares `Host h;`, which
embeds an `Engine` by value, as a *local* in roughly thirty test functions.
Linux hands a thread 8 MB of stack and nobody ever noticed. A PE gets 2 MB of
stack reserve by default under mingw-w64, and Windows itself defaults to 1 MB,
so the first test overflowed the stack and the process died with a bare access
violation inside `Engine::prepare` — after printing the banner and not one
result line.

`Makefile.mingw` links with `-Wl,--stack,16777216`, and CI reads
`SizeOfStackReserve` back out of the PE header so that a dropped flag is
reported as a dropped flag rather than as a mystery crash. Reproduce the
failure natively with:

```
ulimit -s 2048; ./build/engine_test     # SIGSEGV
ulimit -s 16384; ./build/engine_test    # 199 passed
```

The link flag is the right fix for the test binary. It is *not* a fix for the
application: `src/main.cpp` will have the same problem the moment it puts an
`Engine` anywhere near the stack, and 2 MB is also the default for every thread
the app creates. Either keep `--stack` in the Windows link for the app too, or
give `Engine` a heap-allocating factory. The second is better and is the one to
do if the GUI is ever built.

Also worth knowing: the cross build defines `__USE_MINGW_ANSI_STDIO=1`. Without
it `printf` resolves to msvcrt's implementation, which does not understand
`%zu` and is inconsistent about `%lld` — and the suite prints frame counts and
buffer offsets with exactly those in the text of ~30 assertions. That would not
have failed the run, it would have quietly printed garbage next to `PASS`.

## Windows dependencies — solved with an MSYS2 sysroot

Three libraries have no Windows system equivalent and stand between the source
tree and a GUI build:

| Library | Used by |
|---|---|
| freetype | `gfx/font.cpp` |
| libsndfile | `audio/sample.cpp`, `tools/render` |
| libsamplerate | `audio/sample.cpp`, `tools/render` |

**glew is not on this list and never was.** `src/gfx/gl.h` declares the GL
prototypes itself and the Windows build resolves them through
`wglGetProcAddress`; the older version of this document listed glew as a
Windows dependency, and it was wrong.

Everything else comes from the OS: `opengl32`, `gdi32`, `ole32`, `oleaut32`,
`avrt`, `ksuser`, `winmm`. Not needed on Windows at all: jack, alsa, x11,
xcursor, fontconfig, wayland-\*, egl, xkbcommon, lilv.

### What was done: `tools/win-sysroot.sh`

This document used to recommend vcpkg and to reject MSYS2 on the grounds that
"pacman only runs on Windows, so it needs a `windows-latest` runner". **That
reasoning was wrong, and it is the reason the GUI stayed unbuildable for so
long.** pacman is irrelevant. An MSYS2 package is a zstd-compressed tar of a
`ucrt64/` prefix served over plain HTTPS; `curl` and `tar` are the entire tool
requirement, on any Linux, with no Windows host anywhere in the loop.

`tools/win-sysroot.sh` fetches 22 pinned packages (each with its sha256 in the
script) into `build-mingw/sysroot`, then verifies that every non-system DLL
import inside the sysroot resolves within it. Cold run: about 17 MB and a few
seconds. Compare with the ten-minute vcpkg build this replaces.

The closure is bigger than three libraries because MSYS2 builds freetype
against harfbuzz:

```
freetype -> harfbuzz, libpng, zlib, bzip2, brotli
harfbuzz -> glib2, graphite2, libstdc++/libgcc
glib2    -> gettext(libintl), libiconv, pcre2, libffi
sndfile  -> FLAC, ogg, vorbis, opus, mpg123, lame
```

That is 24 DLLs beside the .exe, and it is the price of not building freetype
ourselves. **Shared, not static, on purpose**: the old note was right that a
static libsndfile's FLAC/ogg/vorbis/opus link order is a trap — the answer is
simply not to link statically. `make -f Makefile.mingw gui-dlls` walks the
import table of the .exe and of every DLL it copies, transitively, so the set
is derived rather than listed.

**One real caveat.** MSYS2's mirrors prune old versions, so a pinned URL will
eventually 404 — the script says so at the failure and the fix is to bump the
version and its hash together. That is fine for a developer command and is the
main reason there is no CI job yet (see below).

## What first light found

Four things, in the order they bit.

### 1. `opengl32.dll` exports GL 1.1, and that is the whole loader problem

Expected, and cheap once it is written: `src/gfx/gl_win32_loader.h` declares
the 29 GL 2.0+ entry points the renderer uses as function pointers **inside
`namespace lat`, with the same names as the functions**. Every GL call in this
codebase is written inside that namespace, so ordinary lookup finds the pointer
first — no `#define glCreateShader` anywhere. A GL 2.0+ call added outside
`namespace lat` resolves to the global name instead and fails at LINK time,
naming the function, rather than calling a null pointer at run time.

They are `inline` variables, so the loader is a header with no `.cpp`, and the
Linux build's file list is untouched by its existence. `window_win32.cpp` calls
`loadGLWin32()` once, immediately after `wglMakeCurrent`, and a missing entry
point is fatal with the function's name in the log.

### 2. A wine prefix's `C:\windows\Fonts` is empty

`findSystemFont` probing `%WINDIR%\Fonts\segoeui.ttf` and friends is correct
for Windows and finds **nothing at all** under Wine, which takes the whole GUI
down with "no usable system font found". Wine discovers the host's fonts
through fontconfig and publishes them in
`HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts` as `Z:\usr\share\...`
paths. So the Windows branch tries the directory first (one hit on real
Windows, three syscalls) and the font registry second. The registry is the
documented location on both, so this is not a Wine special case — it is the
lookup that should have been there anyway.

### 3. The font atlas had a real, cross-platform bug in it

**This is the find worth keeping.** With Wine's bundled Tahoma every glyph came
out as a 45-degree hatch. Not a Windows bug, not a Wine bug, not a GL bug — the
GL texture read back byte-identical to what the CPU had uploaded, so the atlas
itself was wrong.

Cause: `font.cpp` assumed `FT_Load_Char` always returns 8-bit gray. A TrueType
file may carry **monochrome bitmap strikes** for small sizes, and FreeType
prefers a strike when one matches the requested ppem. Wine's Tahoma has them.
The glyph then arrives at 1 bit per pixel with `pitch == 1` for a 6-pixel-wide
glyph, and a `memcpy` of `width` bytes per row reads six bytes out of a
one-byte row — drawing the neighbouring rows' bits as coverage. Hence the
hatch.

DejaVu and Liberation ship no strikes, so on every Linux box FreeType always
returned gray and the bug was invisible. It would have appeared on the first
user whose font had strikes, on any platform.

Fixed in two parts: `blitGlyph()` handles `FT_PIXEL_MODE_MONO` (and refuses,
loudly, to guess at anything else), and all loads now pass `FT_LOAD_NO_BITMAP`
so the outline is always rasterised. The second is the design decision: strikes
are aliased, this interface's type is antialiased coverage throughout, and
mixing the two would have made 9 px labels jagged and 11 px labels smooth. The
row-stride arithmetic also moved from `size_t` to `ptrdiff_t`, because
FreeType's `pitch` is signed and a negative one used to become an address in
the exabytes.

Linux output is provably unchanged: the same set rendered at the same size with
`NXTAKT_REDUCED_MOTION=1` produces a **byte-identical PNG** before and after.

### 4. The hand-written Win32 code was right

`window_win32.cpp` and `backend_win32.cpp` had never run. On first contact:

* the window, the WGL bootstrap, `wglChoosePixelFormatARB`, the 3.3 core
  context and `SwapBuffers` all worked unmodified;
* the message pump delivered **keyboard** (space started the transport) and
  **mouse** (a click switched Session → Arrangement) with no fixes;
* **WASAPI came up on the first try** — 48000 Hz, 1440 frames, 2 ch, float32 —
  through Wine's emulation onto the host.

The only change either file needed was the one-line `loadGLWin32()` call. For
code written against an API by eye, that is a better hit rate than it had any
right to be.

## Plugin formats

| Format | Linux | Windows | State |
|---|---|---|---|
| LV2 | native | rare | Implemented (`plugin/lv2_host.cpp`). Needs lilv, which has no cross build here, so the Windows link takes the stub in `tools/mingw-shim/win/win_stubs.cpp` and the scan reports it. |
| CLAP | yes | yes | Implemented (`plugin/clap_host.cpp`), and **cross-compiles unmodified**: the shim's `dlfcn.h` maps `dlopen`/`dlsym` onto `LoadLibraryW`/`GetProcAddress`, which is what they mean on Windows. Header-only ABI, identical both sides. No plugin has been loaded on Windows yet. |
| VST3 | some | dominant | Not implemented. `TODO(vst3)` in `plugin/host.h`. Steinberg SDK is dual GPLv3/proprietary and cannot be vendored here. |
| AU | — | — | macOS only, out of scope. |

`PluginRegistry::scan()` dispatches on `PluginDesc::format`, so adding a format
touches only the entry points in `namespace lat::detail`.

Audio drivers are a separate axis. Windows currently gets WASAPI shared mode
(~10 ms round trip — fine for playback, not for tracking). ASIO is the next
step and is what every Windows DAW ships; the SDK is licensed and cannot be
vendored, so it has to be an opt-in build. See the `TODO(asio)` block at the
top of `src/audio/backend_win32.cpp`.

## What is NOT verified

Read this before believing the screenshot.

* **Real Windows.** Everything above happened under Wine on Linux, with Mesa
  behind Wine's `opengl32`. A real driver's WGL, a real audio endpoint and a
  real window manager have never seen this binary. Wine is a good proxy for the
  Win32 API and a poor one for drivers.
* **Sound.** WASAPI *opened* and the render thread runs. Nobody has confirmed
  audio actually leaves the machine, and the transport running is not evidence
  of that.
* **DPI.** Tested at exactly one scale: 96 dpi, `dpiScale` 1.0. The
  per-monitor-v2 path, `WM_DPICHANGED` and `AdjustWindowRectExForDpi` are all
  unexercised — a headless compositor has one monitor and never rescales it.
* **Input beyond two events.** One key (space) and one click (a tab). Not
  tested: drag with capture, double-click, the wheel, `WM_MOUSELEAVE`, text
  entry and the UTF-16 surrogate path, `WM_KILLFOCUS`, or the computer-MIDI
  keyboard's scancode mapping — which is the one input path with a real
  portability question in it (Set-1 make codes vs evdev).
* **Resize, minimise, multi-monitor, Alt+Tab.** The virtual desktop never
  changed size.
* **CLAP loading.** The loader compiles and the scanner runs; no `.clap` file
  was on the machine.
* **Anything under `src/ipc`.** Daemon mode is unreachable by construction.

## Still to do

Roughly in the order you hit them making this a Windows product rather than a
demonstration.

1. **Run it on real Windows.** Everything else on this list is speculation
   until that happens. The binary and its DLLs are self-contained; copying
   `build-mingw/` onto a Windows box is the whole test.
2. **A CI job.** Deliberately not added yet — see below.
3. **`tools/mingw-shim/win/` should shrink to nothing.** Each header there is a
   placeholder for a real implementation: WinMM/WinRT MIDI, winsock OSC,
   `CreateFileMappingW` IPC. The shim's README lists them with what each one
   currently costs.
4. **Bundle a font.** The registry lookup works, but a bundled face in
   `assets/` would make typography identical everywhere and delete the whole
   discovery path on both platforms. Still the better answer.
5. **Strip the binary.** `nxtakt.exe` is 45 MB because `-g` is on; `-s` or a
   separate `.pdb`-equivalent split is a packaging decision, not a build one.
6. **Stack sizes.** The app links with `-Wl,--stack,16777216`. The `Engine`
   itself is on the heap (`EngineHandle` owns a `unique_ptr`), so this is
   belt-and-braces rather than load-bearing as it is for `engine_test.exe`.
7. **No MIDI input on Windows.** There is no WinMM/WinRT MIDI path; the stub
   reports it at startup.
8. **No ASIO, no WASAPI exclusive mode.** Latency is whatever the shared mix
   engine gives you.
8. **Multi-channel output is stereo-in-a-wider-buffer.** `writeOut()` silences
   everything past L/R on surround endpoints instead of folding down.
9. **No device-change recovery.** `AUDCLNT_E_DEVICE_INVALIDATED` (default
   device switched, USB interface unplugged) kills the render thread and audio
   stops for good until restart. Needs an `IMMNotificationClient` and a
   re-Initialize path.
10. **No packaging.** No icon, no `.rc` manifest, no installer. Without an
    application manifest the DPI awareness relies entirely on the runtime
    `SetProcessDpiAwarenessContext` call in `window_win32.cpp`.
11. **`src/ipc` has no Windows implementation.** `shm.h` is POSIX `shm_open` +
    `mmap` + process-shared pthread mutexes. The Win32 equivalent is
    `CreateFileMappingW`/`MapViewOfFile` plus named mutexes — a second
    implementation, not a build fix, which is why `ipc_test` is excluded from
    `Makefile.mingw` rather than patched into it.
12. **Timer resolution.** Nothing calls `timeBeginPeriod`; if the UI loop's
    sleep granularity turns out to matter, that is where to look.

## Why there is no GUI job in CI yet

`.github/workflows/windows.yml` is unchanged. The GUI build is a developer
command, not a checked one, and that is a decision rather than an omission:

* **The pinned sysroot will rot.** MSYS2 mirrors prune superseded versions.
  `glib2` and `harfbuzz` move weekly; within a couple of months some URL in
  `tools/win-sysroot.sh` will 404 and the job would go red for a reason that
  has nothing to do with the commit that turned it red. A developer hitting
  that gets a clear message and bumps two lines. CI hitting it blocks everyone.
* **Compiling is not the interesting property, and it is all a runner could
  check.** A GitHub runner has no window station, no audio endpoint and no GPU.
  It could link `nxtakt.exe` and confirm the PE header — worth something, but
  much less than the `engine_test.exe` job already earns, and at the cost of a
  fetch that can fail on its own.

**Add the job when either** the sysroot is mirrored somewhere stable (a release
asset on this repo, or a `actions/cache` entry keyed on the pinned list, with
the fetch as the cache-miss path only) **or** there is something to run — a
headless smoke test that starts the GUI under Wine with a software GL stack
(`GALLIUM_DRIVER=llvmpipe`) and asserts it reached "renderer up". Until then
`make -f Makefile.mingw` covers the code CI can actually make claims about, and
`tools/headless_win_test.sh` is how the GUI gets checked, by hand, on a machine
with a GPU.
