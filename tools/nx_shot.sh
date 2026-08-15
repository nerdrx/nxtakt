#!/usr/bin/env bash
# Headless screenshot, isolated from any other gamescope on the machine.
#
#   tools/nx_shot.sh -o out.png [-W 1600] [-H 1000] [-s secs] [-- args...]
#
# This is tools/headless_test.sh with one behavioural difference, and it exists
# because that difference matters when more than one agent is working the tree:
# headless_test.sh picks the LAST gamescope-* socket it finds in the runtime
# directory, which is whichever nested compositor happens to be newest -- not
# necessarily the one it just started. Run two of them at once and you get a
# screenshot of somebody else's program, which is exactly as confusing as it
# sounds.
#
# Here the socket set is captured BEFORE launch and the screenshot is taken
# through the one socket that appeared afterwards. If none appears, or more than
# one does, it fails loudly rather than guessing.
set -uo pipefail
cd "$(dirname "$0")/.."

OUT=/tmp/nxtakt_shot.png
W=1600; H=1000; SETTLE=5
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
[[ -x build/nxtakt ]]               || { echo "build/nxtakt missing - run make"; exit 1; }

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

LOG=$(mktemp /tmp/nx-shot-XXXXXX.log)
export GAMESCOPE_WAYLAND_DISPLAY=""
gamescope --backend headless -W "$W" -H "$H" -w "$W" -h "$H" \
          -- ./build/nxtakt "${ARGS[@]}" >"$LOG" 2>&1 &
GS_PID=$!
cleanup() { kill "$GS_PID" 2>/dev/null; wait "$GS_PID" 2>/dev/null; }
trap cleanup EXIT

SOCK=""
for _ in $(seq 1 150); do
    NEW=$(comm -13 <(echo "$BEFORE") <(sockets | sort))
    COUNT=$(echo "$NEW" | grep -c . )
    if [[ "$COUNT" == "1" ]]; then SOCK="$NEW"; break; fi
    if [[ "$COUNT" -gt 1 ]]; then
        echo "ambiguous: $COUNT new gamescope sockets appeared, refusing to guess"
        echo "$NEW"; exit 1
    fi
    sleep 0.1
done
[[ -n "$SOCK" ]] || { echo "gamescope never came up:"; tail -20 "$LOG"; exit 1; }

sleep "$SETTLE"
rm -f "$OUT"
if ! GAMESCOPE_WAYLAND_DISPLAY="$SOCK" gamescopectl screenshot "$OUT" >/dev/null 2>&1; then
    echo "screenshot failed"; tail -20 "$LOG"; exit 1
fi
for _ in $(seq 1 50); do [[ -s "$OUT" ]] && break; sleep 0.1; done
[[ -s "$OUT" ]] || { echo "screenshot never landed at $OUT"; exit 1; }

grep -aE "^\[nxtakt (info|warn|err)" "$LOG" | grep -viE "^\[nxtakt warn\] lv2:" | tail -30
echo "=== $OUT ($(stat -c%s "$OUT") bytes) via $SOCK ==="
