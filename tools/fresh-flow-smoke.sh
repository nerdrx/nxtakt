#!/bin/bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-"$repo/build/fresh-flow-smoke"}
mkdir -p "$out"
fresh_tmp=$(mktemp -d /tmp/nxtakt-fresh.XXXX)
trap 'rm -rf "$fresh_tmp"' EXIT
cat > "$fresh_tmp/empty.lattice" <<'PROJECT'
nxtakt 10
tempo 120
sig 4 4
quantum 4
metronome 0
nextuid 3
name Untitled
track 0
  uid 1
  name 1 Audio
  color 4
  fader 0.85
  pan 0
  flags 0 0 0
  width 112
endtrack
scene 0
  uid 2
  name Pattern 1
  tempo 0
endscene
PROJECT
mkdir -p "$fresh_tmp/config/nxtakt"
printf 'jack0 "-"\njack1 "-"\njack2 "-"\njack3 "-"\n' > "$fresh_tmp/config/nxtakt/audio-settings.txt"
export XDG_CONFIG_HOME="$fresh_tmp/config" NXTAKT_SESSION="fresh-smoke-$$" NXTAKT_SCALE=1
export NXTAKT_STUDIO_QA_PROJECT="$fresh_tmp/empty.lattice"
"$repo/tools/drive.sh" "$repo/tools/fresh-flow-gestures.sh" "$out" -- "$NXTAKT_STUDIO_QA_PROJECT"
python3 - "$out" <<'PY'
from pathlib import Path
import sys,re
p=Path(sys.argv[1])
for name,tracks,instrument in [('drums',1,'Drum'),('undo',1,'1 Audio'),('redo',1,'Drum'),('spectra',2,'Spectra')]:
 t=(p/(name+'.lattice')).read_text()
 assert len(re.findall(r'^track ',t,re.M))==tracks,(name,t)
 assert instrument in t,(name,instrument)
assert 'the engine refused SetClip' not in (p/'app.log').read_text()
print('PASS: empty lane reused for drums, undo/redo retain lane count, Spectra appends a second channel')
PY
