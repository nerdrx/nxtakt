# Sourced by tools/drive.sh with a disposable gen_demo project, 1360x860 scale 1.
set -e
: "${NXTAKT_TEST_PROJECT:?set a disposable project path}"
clk 710 204
clk 166 553
clk 210 597
key ctrl+s
python3 - "$NXTAKT_TEST_PROJECT" <<'PY'
import sys
clip = open(sys.argv[1]).read().split('track 4\n')[1].split('  clip 2\n')[1].split('  endclip')[0]
assert '    loop 0\n' in clip, 'Playback page must change the selected clip loop'
print('PASS: Playback page changes the selected clip')
PY
key ctrl+z
clk 71 553
clk 155 750
key ctrl+s
python3 - "$NXTAKT_TEST_PROJECT" <<'PY'
import sys
clip = open(sys.argv[1]).read().split('track 4\n')[1].split('  clip 2\n')[1].split('  endclip')[0]
assert '    loop 1\n' in clip, 'undo restores loop'
assert clip.count('    note ') == 16, 'Duplicate must produce sixteen notes'
print('PASS: Notes page duplicates notes and page switching preserves undo')
PY
key ctrl+z
clk 140 466
clk 521 571
key ctrl+s
python3 - "$NXTAKT_TEST_PROJECT" <<'PY'
import sys
track = open(sys.argv[1]).read().split('track 4\n')[1]
assert '    bypass 1\n' in track, 'Enabled button must bypass the device'
print('PASS: labeled device control changes bypass')
PY
key ctrl+z
# A logarithmic parameter must still be editable and one undo must restore it.
drag 432 630 432 660
key ctrl+s
python3 - "$NXTAKT_TEST_PROJECT" <<'PY'
import re, sys
track = open(sys.argv[1]).read().split('track 4\n')[1]
assert float(re.search(r'    param 1 (\S+)', track)[1]) != 6000.0
print('PASS: redesigned frequency knob edits its parameter')
PY
key ctrl+z
key ctrl+s
python3 - "$NXTAKT_TEST_PROJECT" <<'PY'
import re, sys
track = open(sys.argv[1]).read().split('track 4\n')[1]
assert '    bypass 0\n' in track
assert float(re.search(r'    param 1 (\S+)', track)[1]) == 6000.0
print('PASS: one undo restores the frequency gesture')
PY
shot inspector-regression
