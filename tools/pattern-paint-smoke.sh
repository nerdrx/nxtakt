#!/bin/bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-"$repo/build/pattern-paint-smoke"}
mkdir -p "$out"
pattern_tmp=$(mktemp -d /tmp/nxtakt-paint.XXXX)
trap 'rm -rf "$pattern_tmp"' EXIT
"$repo/build/gen_demo" "$pattern_tmp/demo" > "$out/fixture.log" 2>&1
mkdir -p "$pattern_tmp/config/nxtakt"
printf 'jack0 "-"\njack1 "-"\njack2 "-"\njack3 "-"\n' > "$pattern_tmp/config/nxtakt/audio-settings.txt"
export XDG_CONFIG_HOME="$pattern_tmp/config" NXTAKT_SESSION="paint-smoke-$$" NXTAKT_SCALE=1
export NXTAKT_STUDIO_QA_PROJECT="$pattern_tmp/demo/demo.lattice"
"$repo/tools/drive.sh" "$repo/tools/pattern-paint-gestures.sh" "$out" -- "$NXTAKT_STUDIO_QA_PROJECT"
python3 - "$out" <<'PYTEST'
import pathlib,re,sys
out=pathlib.Path(sys.argv[1])
def items(name):
    text=(out/(name+'.lattice')).read_text()
    return re.findall(r'^    aclip\n(.*?)^    endaclip$',text,re.M|re.S)
for name,count in [('paint',20),('undo',0),('redo',20),('duplicate',20),('second',30),('second-undo',20)]:
    assert len(items(name))==count,(name,len(items(name)),count)
assert items('paint')==items('redo')==items('duplicate')==items('second-undo')
uids=[re.search(r'^      uid (\d+)$',item,re.M).group(1) for item in items('paint')]
assert len(set(uids))==20
text=(out/'paint.lattice').read_text()
keys=re.search(r'^track 4\n(.*?)(?=^track 5\n|^return )',text,re.M|re.S).group(1)
clip=re.search(r'^  clip 2\n(.*?)^  endclip',keys,re.M|re.S).group(1)
assert len(re.findall(r'^    note ',clip,re.M))==8
assert 'note 0 0.5 57 100' in clip
print('PASS: drag paints all channels, fills repeats, prevents duplicates, preserves source notes, coalesces undo and separates strokes')
PYTEST
