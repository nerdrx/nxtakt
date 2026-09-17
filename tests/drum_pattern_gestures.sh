# Sourced by tools/drive.sh; fresh gen_demo project, 1360x860, scale 1.
set -e
: "${NXTAKT_TEST_PROJECT:?set a disposable project path}"
check_pattern() {
 key ctrl+s
 python3 - "$NXTAKT_TEST_PROJECT" "$1" "$2" "${3:-5}" <<'CHECK'
import sys,re
s=open(sys.argv[1]).read().split('track '+sys.argv[4]+'\n')[1].split('endtrack')[0]
assert s.count('    note ')==int(sys.argv[2]),s
assert '    beats '+sys.argv[3]+'\n' in s,s
print('PASS: notes',sys.argv[2],'beats',sys.argv[3],'track',sys.argv[4])
CHECK
}
clk 616 26
# Missing clipboard leaves the model and undo stack alone.
clk 705 817
check_pattern 0 4
clk 508 817
check_pattern 14 4
clk 617 817
clk 807 817
check_pattern 28 8
shot doubled-pattern
# The view follows into the second bar: clearing kick must affect that bar only.
wheel 485 782 4 5
clk 765 782
check_pattern 25 8
key ctrl+s
python3 - "$NXTAKT_TEST_PROJECT" <<'CHECK'
import sys,re
s=open(sys.argv[1]).read().split('track 5\n')[1].split('endtrack')[0]
kick=[float(b) for b in re.findall(r'^    note (\S+) \S+ 36 ',s,re.M)]
assert kick==[0,2,2.75],kick
print('PASS: clearing targets only the repeated bar')
CHECK
clk 705 817
check_pattern 28 8
# Identical paste must not create another undo step.
clk 705 817
key ctrl+z
check_pattern 25 8
key ctrl+shift+z
check_pattern 28 8
clk 807 817
check_pattern 56 16
clk 807 817
check_pattern 56 16
key ctrl+z
check_pattern 28 8
# Clipboard survives changing clips.
clk 616 26
clk 705 817
check_pattern 14 4 6
shot pasted-another-clip
wid=$(xd search --onlyvisible --name '^NxTakt$' | head -1)
xd windowsize "$wid" 1024 768
sleep 1
shot compact-pattern-tools
clk 807 725
check_pattern 28 8 6
key ctrl+z
check_pattern 14 4 6
