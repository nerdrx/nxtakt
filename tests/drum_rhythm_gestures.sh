# Sourced by tools/drive.sh; fresh gen_demo project, 1360x860, scale 1.
set -e
: "${NXTAKT_TEST_PROJECT:?set a disposable project path}"
check_rhythm() {
 key ctrl+s
 python3 - "$NXTAKT_TEST_PROJECT" "$1" <<'CHECK'
import sys,re
s=open(sys.argv[1]).read().split('track 5\n')[1].split('endtrack')[0]
notes=[(float(b),int(p),int(v)) for b,p,v in re.findall(r'^    note (\S+) \S+ (\d+) (\d+)',s,re.M)]
mode=sys.argv[2]
kick=[(i,36,100) for i in (0,1,2,3)]
snare=[(i+.25,38,101) for i in (0,1,2,3)]
expected={'empty':[], 'kick':kick, 'both':kick+snare,
          'page2':kick+snare+[(i,38,101) for i in (4.25,5.25,6.25,7.25)],
          'cleared':kick+snare}[mode]
assert sorted(notes)==sorted(expected),(mode,notes,expected)
print('PASS:',mode)
CHECK
}
clk 616 26
clk 765 782
check_rhythm kick
# Identical generation must not consume an undo entry.
clk 765 782
key ctrl+z
check_rhythm empty
key ctrl+shift+z
check_rhythm kick
clk 382 621
wheel 571 782
wheel 661 782
clk 765 782
check_rhythm both
shot generated-rhythm
# Extend to two bars, generate only on page two.
clk 487 544
clk 575 544
clk 1332 544
clk 765 782
check_rhythm page2
# Zero Hits clears only the chosen page and lane.
wheel 485 782 4 5
clk 765 782
check_rhythm cleared
key ctrl+z
check_rhythm page2
# Compact view keeps the generator reachable.
wid=$(xd search --onlyvisible --name '^NxTakt$' | head -1)
xd windowsize "$wid" 1024 768
sleep 1
shot compact-rhythm
clk 765 690
check_rhythm cleared
key ctrl+z
check_rhythm page2
