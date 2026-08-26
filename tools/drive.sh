#!/usr/bin/env bash
# Drive NxTakt's GUI inside a HEADLESS compositor and read the model back.
#
# This is the harness the usability passes are made of, and it lives in the
# repo rather than in a scratchpad because the scratchpad has now been cleaned
# out from under it twice, taking the gesture scripts of two audits with it.
#
#   tools/drive.sh <gesture-script.sh> <outdir> [-- app args...]
#
# The gesture script is SOURCED, with these in scope:
#
#   $DISP      the Xwayland display gamescope published (":N")
#   $OUT       the output directory (screenshots land here)
#   $LOG       the app's stdout+stderr, which NXTAKT_DEBUG_PROBE fills
#   xd ...     xdotool against the right display
#   shot NAME  screenshot into $OUT/NAME.png
#   ...plus everything tools/drive-lib.sh defines: clk, dbl, rclk, drag,
#      hover, wheel, key, mark, probe, expect
#
# NEVER opens a window on the developer's desktop: gamescope runs with
# --backend headless, so there is nothing to look at and nothing to steal
# focus. That is a standing rule of this project, not a convenience.
#
# NXTAKT_DEBUG_PROBE=1 makes every model write the app performs appear in the
# log, which is what lets a gesture be ASSERTED rather than eyeballed. A
# screenshot proves what was drawn; the probe log proves what was meant.
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT="${1:?usage: drive.sh <gesture-script.sh> <outdir> [-- app args...]}"; shift
OUT="${1:?usage: drive.sh <gesture-script.sh> <outdir> [-- app args...]}"; shift
ARGS=()
[[ "${1:-}" == "--" ]] && { shift; ARGS=("$@"); }

# A RELATIVE outdir is the safe spelling: gamescope's screenshot path is taken
# relative to its own cwd, so an absolute $OUT once produced $PWD$OUT and a
# pile of "Failed to save screenshot" lines that looked like a driver problem.
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

W=${DRIVE_W:-1360}
H=${DRIVE_H:-860}
LOG="$OUT/app.log"
GSLOG="$OUT/gamescope.log"

export GAMESCOPE_WAYLAND_DISPLAY=""
export NXTAKT_DEBUG_PROBE=1
unset GDK_SCALE                      # a stable seat: no fractional scale surprises

gamescope --backend headless -W "$W" -H "$H" -w "$W" -h "$H" -- \
    env NXTAKT_DEBUG_PROBE=1 "${DRIVE_BIN:-$REPO/build/nxtakt}" "${ARGS[@]}" \
    >"$LOG" 2>&1 &
GS_PID=$!
cleanup() { kill "$GS_PID" 2>/dev/null; wait "$GS_PID" 2>/dev/null; }
trap cleanup EXIT

# Find the display gamescope published for its Xwayland. It appears in the log
# a moment after start; polling beats sleeping because a loaded machine (five
# agents compiling) can take several seconds to get there.
DISP=""
for _ in $(seq 1 120); do
    DISP=$(grep -aoE 'Starting Xwayland on :[0-9]+' "$LOG" 2>/dev/null | tail -1 | grep -oE ':[0-9]+') && [[ -n "$DISP" ]] && break
    sleep 0.25
done
[[ -z "$DISP" ]] && { echo "drive.sh: gamescope never published a display; see $LOG" >&2; exit 1; }

# xdotool takes its display from the ENVIRONMENT; it has no -display flag, and
# passing one makes it print usage and do nothing — which reads exactly like a
# gesture the app ignored. Hence the env spelling, once, here.
xd()   { DISPLAY="$DISP" xdotool "$@"; }
# Screenshot. gamescopectl is the sanctioned way to ask a running gamescope for
# a frame and it takes an ABSOLUTE path; ImageMagick against the Xwayland root
# is the fallback for a build without it. Gamescope's own keybind is last: it
# resolves the path against the COMPOSITOR's cwd rather than ours, which once
# produced a pile of "Failed to save screenshot to <cwd><abspath>" that read
# like a driver fault and was a path bug.
shot() {
  DISPLAY="$DISP" gamescopectl screenshot "$OUT/$1.png" >/dev/null 2>&1 && { sleep 0.5; [[ -s "$OUT/$1.png" ]] && return 0; }
  DISPLAY="$DISP" magick import -window root "$OUT/$1.png" 2>/dev/null && return 0
  DISPLAY="$DISP" import -window root "$OUT/$1.png" 2>/dev/null && return 0
  echo "  !! shot $1 FAILED (no gamescopectl, no ImageMagick)" >&2
}

# Wait for the app to actually be up before the first gesture: a click into a
# window that has not mapped yet is a click into nothing, and the resulting
# "the gesture did nothing" is the most expensive kind of false finding.
for _ in $(seq 1 120); do
    DISPLAY="$DISP" xdotool search --onlyvisible --class . >/dev/null 2>&1 && break
    sleep 0.25
done
sleep 1.2

# shellcheck source=/dev/null
source "$REPO/tools/drive-lib.sh"
# shellcheck source=/dev/null
source "$SCRIPT"

sleep 0.5
echo "--- drive.sh done: $OUT ---"
