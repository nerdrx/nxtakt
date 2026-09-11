# Sourced by tools/drive.sh. Use a fresh gen_demo project at 1360x860, scale 1.
# These deliberately omit hover pauses: a click must not need a warm-up frame.
set -e
: "${NXTAKT_TEST_PROJECT:?set NXTAKT_TEST_PROJECT to the disposable demo.lattice}"
for i in 1 2 3 4 5; do
    xd mousemove 5 5
    sleep 0.08
    xd mousemove 228 318 mousedown 1
    sleep 0.06
    xd mouseup 1
    sleep 0.08
done
key ctrl+s
python3 - "$NXTAKT_TEST_PROJECT" <<'PY'
import sys
first = open(sys.argv[1]).read().split('track 1')[0]
assert 'flags 1 0 0' in first, 'five fast clicks must toggle mute five times'
print('PASS: immediate pointer-move clicks toggle the intended track')
PY
# Track names are editable children of a selectable header; both must work.
clk 612 61
key ctrl+s
python3 - "$NXTAKT_TEST_PROJECT" <<'PY'
import sys
keys = open(sys.argv[1]).read().split('track 4')[1]
assert 'flags 0 0 1' in keys, 'clicking track name must select and auto-arm keys'
print('PASS: track name click selects and auto-arms the instrument')
PY
# A real fader gesture changes the model and one undo restores it.
drag 230 440 230 478
key ctrl+s
python3 - "$NXTAKT_TEST_PROJECT" <<'PY'
import re, sys
first = open(sys.argv[1]).read().split('track 1')[0]
assert float(re.search(r'  fader (\S+)', first)[1]) != 0.85, 'fader must move'
print('PASS: fader drag changes the intended track')
PY
key ctrl+z
key ctrl+s
python3 - "$NXTAKT_TEST_PROJECT" <<'PY'
import re, sys
first = open(sys.argv[1]).read().split('track 1')[0]
assert abs(float(re.search(r'  fader (\S+)', first)[1]) - 0.85) < 1e-6
print('PASS: one undo restores the fader gesture')
PY
shot click-regression
