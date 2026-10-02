#!/bin/bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-"$repo/build/pattern-picker-smoke"}
mkdir -p "$out"
pattern_tmp=$(mktemp -d /tmp/nxtakt-paint.XXXX)
trap 'rm -rf "$pattern_tmp"' EXIT
"$repo/build/gen_demo" "$pattern_tmp/demo" > "$out/fixture.log" 2>&1
mkdir -p "$pattern_tmp/config/nxtakt"
printf 'jack0 "-"\njack1 "-"\njack2 "-"\njack3 "-"\n' > "$pattern_tmp/config/nxtakt/audio-settings.txt"
export XDG_CONFIG_HOME="$pattern_tmp/config" NXTAKT_SESSION="paint-smoke-$$" NXTAKT_SCALE=1
export NXTAKT_STUDIO_QA_PROJECT="$pattern_tmp/demo/demo.lattice"
"$repo/tools/drive.sh" "$repo/tools/pattern-picker-gestures.sh" "$out" -- "$NXTAKT_STUDIO_QA_PROJECT"
python3 - "$out" <<'PYTEST'
from pathlib import Path
import re,sys
p=Path(sys.argv[1])
def text(name): return (p/(name+'.lattice')).read_text()
def scenes(name): return len(re.findall(r'^scene ',text(name),re.M))
for name,count in [('cloned',5),('renamed',5),('rename-undone',5),('rename-redone',5),('paint',5),('new',6),('empty-paint',6),('new-undone',5),('new-redone',6)]:
    assert scenes(name)==count,(name,scenes(name),count)
assert text('cloned')==text('cancelled')
assert '  name Night drive\n' in text('renamed')
assert '  name Full copy\n' in text('rename-undone')
assert '  name Night drive\n' in text('rename-redone')
for track in re.findall(r'^track \d+\n(.*?)^endtrack',text('cloned'),re.M|re.S):
    original=re.search(r'^  clip 2\n(.*?)^  endclip',track,re.M|re.S).group(1)
    clone=re.search(r'^  clip 4\n(.*?)^  endclip',track,re.M|re.S).group(1)
    assert re.sub(r'^    uid .*\n','',original,flags=re.M)==re.sub(r'^    uid .*\n','',clone,flags=re.M)
    assert re.search(r'^    uid (.*)$',original,re.M).group(1)!=re.search(r'^    uid (.*)$',clone,re.M).group(1)
assert len(re.findall(r'^    aclip$',text('paint'),re.M))==15
assert text('new')==text('empty-paint')
assert not re.search(r'^  clip 5$',text('new'),re.M)
assert 'the engine refused SetClip' not in (p/'app.log').read_text()
print('PASS: pattern selection, independent clone payloads and identities, Ctrl+A rename undo/redo, cloned-pattern painting, empty pattern feedback, New undo/redo')
PYTEST
