# Sourced by tools/drive.sh with a disposable gen_demo project,1360x860 scale1.
set -e
: "${NXTAKT_TEST_PROJECT:?set a disposable project path}"
clk 710 204
shot edit-without-launch
# FL-style exploration of an existing note must not destroy it.
dbl 440 660
key ctrl+s
python3 - "$NXTAKT_TEST_PROJECT" <<'PY'
import sys
clip = open(sys.argv[1]).read().split('track 4\n')[1].split('  clip 2\n')[1].split('  endclip')[0]
assert clip.count('    note ') == 8
print('PASS: double-clicking an existing note preserves the pattern')
PY
rclk 440 660
key ctrl+s
python3 - "$NXTAKT_TEST_PROJECT" <<'PY'
import sys
clip = open(sys.argv[1]).read().split('track 4\n')[1].split('  clip 2\n')[1].split('  endclip')[0]
assert clip.count('    note ') == 7
print('PASS: right-click remains an explicit erase action')
PY
key ctrl+z
key ctrl+s
python3 - "$NXTAKT_TEST_PROJECT" <<'PY'
import sys
clip = open(sys.argv[1]).read().split('track 4\n')[1].split('  clip 2\n')[1].split('  endclip')[0]
assert clip.count('    note ') == 8
print('PASS: undo restores the erased note')
PY
key F9
shot mixer-shortcut
key F7
shot editor-shortcut
key F5
shot arrange-shortcut
key F6
# Explicit launch remains independently reachable.
clk 672 204
shot explicit-launch
key space
