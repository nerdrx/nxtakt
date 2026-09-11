# Sourced by tools/drive.sh; fresh gen_demo project, 1360x860, NXTAKT_SCALE=1.
set -e
: "${NXTAKT_TEST_PROJECT:?set a disposable project path}"
check_drums() {
    key ctrl+s
    python3 - "$NXTAKT_TEST_PROJECT" "$1" "$2" "${3:-4}" <<'PY'
import sys
s = open(sys.argv[1]).read().split('track 5\n')[1].split('endtrack')[0]
assert 'nxtakt:drums' in s
assert s.count('    note ') == int(sys.argv[2]), s
assert f'    param 0 {sys.argv[3]}\n' in s, s
assert f'    beats {sys.argv[4]}\n' in s, s
print('PASS: saved drum notes, kit and length:', *sys.argv[2:])
PY
}
clk 616 26
check_drums 0 0
key ctrl+z
key ctrl+s
python3 - "$NXTAKT_TEST_PROJECT" <<'PY'
import sys
s = open(sys.argv[1]).read()
assert 'track 5\n' not in s and 'nxtakt:drums' not in s
print('PASS: one Undo removes the complete new drum track')
PY
clk 616 26
check_drums 0 0
clk 465 600
check_drums 1 0
clk 465 600
check_drums 0 0
key ctrl+z
check_drums 1 0
xd keydown ctrl
wheel 465 600
xd keyup ctrl
key ctrl+s
python3 - "$NXTAKT_TEST_PROJECT" <<'PY'
import sys
s = open(sys.argv[1]).read().split('track 5\n')[1]
assert 'note 0 0.25 36 105' in s, s
print('PASS: Ctrl+wheel increases step velocity')
PY
rclk 465 600
check_drums 0 0
key ctrl+z
check_drums 1 0
clk 508 817
check_drums 14 0
key ctrl+z
check_drums 1 0
clk 508 817
clk 166 618
check_drums 14 1
clk 487 544
clk 575 544
check_drums 14 1 8
clk 1332 544
clk 465 600
check_drums 15 1 8
key ctrl+z
check_drums 14 1 8
clk 1206 544
clk 554 502
shot drums-playing
key space
clk 455 502
shot drums-piano
clk 371 502
shot drums-steps
# The instrument and editor must remain usable at the minimum supported size.
wid=$(xd search --onlyvisible --name '^NxTakt$' | head -1)
xd windowsize "$wid" 1024 768
sleep 1
shot drums-compact
wheel 700 640 6 5
clk 454 695
check_drums 15 1 8
python3 - "$NXTAKT_TEST_PROJECT" <<'PY'
import sys
s = open(sys.argv[1]).read().split('track 5\n')[1]
assert 'note 0 0.25 56 100' in s, s
print('PASS: scrolling the compact editor makes the cowbell lane clickable')
PY
