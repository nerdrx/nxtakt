#!/usr/bin/env bash
# Real, silent GUI regression in headless Gamescope. Never opens a desktop window.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-"$repo/build/studio-smoke"}
mkdir -p "$out"
out=$(cd "$out" && pwd)
studio_tmp=$(mktemp -d /tmp/nxtakt-studio-smoke.XXXXXX)
trap 'rm -rf "$studio_tmp"' EXIT
"$repo/build/gen_demo" "$studio_tmp/demo" > "$out/fixture.log" 2>&1
mkdir -p "$studio_tmp/config/nxtakt"
printf 'jack0 "-"\njack1 "-"\njack2 "-"\njack3 "-"\n' > "$studio_tmp/config/nxtakt/audio-settings.txt"
export XDG_CONFIG_HOME="$studio_tmp/config" NXTAKT_SESSION="studio-smoke-$$" NXTAKT_SCALE=1
export NXTAKT_STUDIO_QA_PROJECT="$studio_tmp/demo/demo.lattice"
export DRIVE_W=1360 DRIVE_H=860
"$repo/tools/drive.sh" "$repo/tools/studio-smoke-gestures.sh" "$out" -- "$NXTAKT_STUDIO_QA_PROJECT"
python3 - "$out" <<'PY'
import pathlib,re,sys
out=pathlib.Path(sys.argv[1])
def count(name):
    text=(out/(name+'.lattice')).read_text()
    track=re.search(r'^track 4\n(.*?)(?=^track 5\n|\Z)',text,re.M|re.S).group(1)
    clip=re.search(r'^  clip 2\n(.*?)^  endclip',track,re.M|re.S).group(1)
    return len(re.findall(r'^    note ',clip,re.M))
for name,expected in [('placed',8),('step',9),('undo',8),('redo',9),('restarted',9)]:
    assert count(name)==expected,(name,count(name),expected)
placed=(out/'placed.lattice').read_text()
assert len(re.findall(r'^  arrangement$',placed,re.M))==5
# Placement copied the original melody; editing its pattern must not alter it.
assert '      note 0 0.25 60 100' not in (out/'step.lattice').read_text()
def field(name,key):
    text=(out/(name+'.lattice')).read_text()
    track=re.search(r'^track 4\n(.*?)(?=^track 5\n|\Z)',text,re.M|re.S).group(1)
    return re.search(r'^  '+key+r' (.*)$',track,re.M).group(1)
assert field('renamed','name')=='Keys QA'
assert field('rename-undone','name')=='keys'
assert float(field('fader','fader'))!=.85
assert float(field('fader-undone','fader'))==.85
log=(out/'app.log').read_text()
assert 'engine restarted: pid' in log
assert 'The transport is stopped.' in log
assert 'workspace focus 1' in log and 'workspace focus 3' in log
print('PASS: GUI placement, independent pattern edits, undo/redo, floating tools, rename, fader undo and engine restart')
PY
