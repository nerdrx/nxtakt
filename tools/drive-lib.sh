# Gesture vocabulary for tools/drive.sh. Sourced, never run.
#
# Every helper here is deliberately SLOWER than a human: an immediate-mode UI
# decides hover, press and release on separate frames, and a gesture whose
# frames arrive faster than the app draws is a gesture the app never saw. The
# sleeps are the difference between measuring the program and measuring xdotool.

# A press-move-release with intermediate motion, because a drag with no motion
# between down and up is a CLICK to any hit test that tracks movement.
drag() {                       # drag x0 y0 x1 y1 [modifier]
  local x0=$1 y0=$2 x1=$3 y1=$4 mod=${5:-}
  xd mousemove "$x0" "$y0"; sleep 0.25
  [[ -n "$mod" ]] && { xd keydown "$mod"; sleep 0.1; }
  xd mousedown 1; sleep 0.2
  local n=12 i
  for ((i=1;i<=n;i++)); do
    xd mousemove $(( x0+(x1-x0)*i/n )) $(( y0+(y1-y0)*i/n )); sleep 0.05
  done
  sleep 0.2; xd mouseup 1
  [[ -n "$mod" ]] && xd keyup "$mod"
  sleep 0.5
}

clk()  { xd mousemove "$1" "$2"; sleep 0.3; xd click "${3:-1}"; sleep 0.6; }
rclk() { clk "$1" "$2" 3; }
dbl()  { xd mousemove "$1" "$2"; sleep 0.3; xd click --repeat 2 --delay 90 1; sleep 0.7; }
hover(){ xd mousemove "$1" "$2"; sleep "${3:-0.7}"; }
wheel(){ xd mousemove "$1" "$2"; sleep 0.3
         local n=${3:-1} btn=${4:-4} i
         for ((i=0;i<n;i++)); do xd click "$btn"; sleep 0.25; done; sleep 0.4; }
key()  { xd key "$@"; sleep 0.35; }

# Park the pointer somewhere inert between gestures, so a hover state left over
# from the last one cannot be mistaken for this one's result.
mark() { echo "@@@ $*"; xd mousemove 5 5; sleep 0.35; }

# What the app SAYS it did. NXTAKT_DEBUG_PROBE turns model writes into log
# lines; this is how a gesture is asserted rather than admired.
probe() { grep -a "PROBE" "$LOG" | tail -n "${1:-8}"; }

# expect <pattern> <what it means> — a soft assertion that annotates the run
# rather than aborting it, because a usability pass wants the WHOLE list of
# findings, not the first one.
expect() {
  if grep -aq -- "$1" "$LOG"; then echo "  OK   $2"
  else                             echo "  MISS $2   (no /$1/ in the log)"; fi
}

# Hit-zone probe: walk a line of pixels and report where the app claims a hot
# widget, which is how a drag edge's REAL width gets measured instead of read
# out of the source. Requires NXTAKT_DEBUG_HOT=1 (the app logs its hot id).
scanx() {                      # scanx y x0 x1 [step]
  local y=$1 x0=$2 x1=$3 st=${4:-1} x
  for ((x=x0;x<=x1;x+=st)); do
    xd mousemove "$x" "$y"; sleep 0.06
    echo "  x=$x $(grep -a 'HOT ' "$LOG" | tail -1)"
  done
}
