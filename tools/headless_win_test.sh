#!/usr/bin/env bash
# Run the WINDOWS build of NxTakt under wine, inside a headless gamescope, and
# screenshot it. Nothing appears on the developer's desktop.
#
#   tools/headless_win_test.sh [-o out.png] [-W 1360] [-H 860] [-s secs] [-- args...]
#
# This is tools/nx_shot.sh with wine in the middle, and the three things that
# differ are the three things that make it work:
#
#  1. wine needs an X11 display. gamescope --backend headless publishes a
#     Wayland socket AND an XWayland display for its children, and wine takes
#     the X11 path -- which is the one wine's winex11.drv is built around.
#     There is no wine Wayland driver in play here and that is deliberate: the
#     point is to exercise OUR Win32/WGL backend, not wine's newest one.
#
#  2. `wine explorer /desktop=nx,WxH` puts the app in a virtual desktop window.
#     Without it a wine app is a bare X11 window whose size is up to the
#     compositor, and gamescope will happily hand it the whole output; with it
#     the client area is exactly what was asked for, which is what makes two
#     screenshots comparable.
#
#  3. WINEPREFIX is private and WINEDLLOVERRIDES kills the Mono/Gecko
#     bootstrap. The same discipline as .github/workflows/windows.yml, for the
#     same reason: without it the first run pops an installer dialog and hangs
#     forever, inside a compositor nobody can click on.
#
# ONE THING TO KNOW: `wine explorer` does not pass the child's stdout through,
# so the "[nxtakt ...]" lines below usually come out empty. That is a property
# of explorer, not a sign the app is silent. To READ THE LOG, drop the virtual
# desktop and run the exe directly -- the window is then sized by the
# compositor rather than by us, which is worse for screenshots and fine for
# debugging:
#
#   gamescope --backend headless -W 1360 -H 860 -w 1360 -h 860 \
#     -- wine ./build-mingw/nxtakt.exe set.lattice
#
# That is how "WASAPI backend up" and the font that was chosen get confirmed.
set -uo pipefail
cd "$(dirname "$0")/.."

OUT=/tmp/nxtakt_win_shot.png
W=1360; H=860; SETTLE=12
ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        -o) OUT="$2"; shift 2 ;;
        -W) W="$2"; shift 2 ;;
        -H) H="$2"; shift 2 ;;
        -s) SETTLE="$2"; shift 2 ;;
        --) shift; ARGS=("$@"); break ;;
        *) ARGS+=("$1"); shift ;;
    esac
done

command -v gamescope    >/dev/null || { echo "gamescope not installed"; exit 1; }
command -v gamescopectl >/dev/null || { echo "gamescopectl not installed"; exit 1; }
WINE=$(command -v wine || command -v wine64)
[[ -n "$WINE" ]]            || { echo "wine not installed"; exit 1; }
[[ -f build-mingw/nxtakt.exe ]] || { echo "build-mingw/nxtakt.exe missing - make -f Makefile.mingw gui"; exit 1; }

# A private prefix, kept between runs (creating one costs ~10s). Inside
# build-mingw/ so `make -f Makefile.mingw clean` takes it with everything else.
export WINEPREFIX="${WINEPREFIX:-$PWD/build-mingw/.wineprefix}"
export WINEARCH=win64
export WINEDLLOVERRIDES="mscoree,mshtml="
export WINEDEBUG="${WINEDEBUG:--all}"

if [[ ! -f "$WINEPREFIX/system.reg" ]]; then
    echo "=== creating wine prefix at $WINEPREFIX ==="
    "$WINE" wineboot --init >/dev/null 2>&1
    command -v wineserver >/dev/null && wineserver -w || sleep 3
fi

# Screenshot the compositor we started and no other. See tools/nx_shot.sh for
# why picking "the newest gamescope socket" is not good enough.
RT="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
sockets() {
    for s in "$RT"/gamescope-*; do
        [[ -S "$s" ]] || continue
        local b; b=$(basename "$s")
        [[ "$b" == *.lock || "$b" == *-ei ]] && continue
        echo "$b"
    done
}
BEFORE=$(sockets | sort)

LOG=$(mktemp /tmp/nx-win-shot-XXXXXX.log)
export GAMESCOPE_WAYLAND_DISPLAY=""
gamescope --backend headless -W "$W" -H "$H" -w "$W" -h "$H" \
          -- "$WINE" explorer "/desktop=nx,${W}x${H}" \
             ./build-mingw/nxtakt.exe "${ARGS[@]}" >"$LOG" 2>&1 &
GS_PID=$!
cleanup() {
    kill "$GS_PID" 2>/dev/null
    wait "$GS_PID" 2>/dev/null
    # wine leaves its services behind; without this the next run inherits a
    # half-dead prefix and blocks in wineboot.
    command -v wineserver >/dev/null && WINEPREFIX="$WINEPREFIX" wineserver -k 2>/dev/null
}
trap cleanup EXIT

SOCK=""
for _ in $(seq 1 200); do
    NEW=$(comm -13 <(echo "$BEFORE") <(sockets | sort))
    COUNT=$(echo "$NEW" | grep -c .)
    if [[ "$COUNT" == "1" ]]; then SOCK="$NEW"; break; fi
    if [[ "$COUNT" -gt 1 ]]; then
        echo "ambiguous: $COUNT new gamescope sockets appeared, refusing to guess"; exit 1
    fi
    sleep 0.1
done
[[ -n "$SOCK" ]] || { echo "gamescope never came up:"; tail -30 "$LOG"; exit 1; }

# Longer than the Linux script's settle by default: wine has to map the prefix,
# load 20-odd DLLs and bring up a virtual desktop before our first frame.
sleep "$SETTLE"
rm -f "$OUT"
if ! GAMESCOPE_WAYLAND_DISPLAY="$SOCK" gamescopectl screenshot "$OUT" >/dev/null 2>&1; then
    echo "screenshot failed"; tail -30 "$LOG"; exit 1
fi
for _ in $(seq 1 50); do [[ -s "$OUT" ]] && break; sleep 0.1; done

echo "=== nxtakt (windows build, under wine) ==="
grep -aE "^\[nxtakt" "$LOG" | head -40
echo "=== wine diagnostics ==="
grep -aiE "err:|fixme:(win|wgl|opengl)|wine:" "$LOG" | head -20
echo "=== $OUT ($(stat -c%s "$OUT" 2>/dev/null || echo 0) bytes) via $SOCK ==="
echo "=== full log: $LOG ==="
