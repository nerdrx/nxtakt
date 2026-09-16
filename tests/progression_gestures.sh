# Source through tools/drive.sh at 1360x860 with an empty eight-beat MIDI clip.
# Requires a disposable NXTAKT_TEST_PROJECT and isolated XDG_CONFIG_HOME.
set -e
: "${NXTAKT_TEST_PROJECT:?set a disposable project path}"
: "${XDG_CONFIG_HOME:?set an isolated configuration directory}"
key F7
clk 270 550
clk 260 595
wheel 40 610 5 5
clk 165 765
key ctrl+s
python3 - "$NXTAKT_TEST_PROJECT" <<"CHECK"
import re,sys
n=[(float(a),float(b),int(c)) for a,b,c in re.findall(r"^    note (\S+) (\S+) (\d+)",open(sys.argv[1]).read(),re.M)]
assert len(n)==12,n
assert [p for t,l,p in n[:3]]==[60,64,67],n
for i,pcs in enumerate([{0,4,7},{7,11,2},{9,0,4},{5,9,0}]):
 g=n[i*3:i*3+3]
 assert {p%12 for t,l,p in g}==pcs,n
 assert all(t==i*2 and abs(l-1.8)<1e-6 for t,l,p in g),n
print("PASS: smooth C-major progression timing, no accidental root change during scrolling")
CHECK
shot progression
key ctrl+z
key ctrl+s
python3 - "$NXTAKT_TEST_PROJECT" <<"CHECK"
import sys
assert "    note " not in open(sys.argv[1]).read()
print("PASS: one Undo clears entire progression")
CHECK
key ctrl+shift+z
key ctrl+s
python3 - "$NXTAKT_TEST_PROJECT" <<"CHECK"
import sys
assert open(sys.argv[1]).read().count("    note ")==12
print("PASS: Redo restores progression")
CHECK
key ctrl+z
clk 202 600
clk 295 600
clk 165 765
key ctrl+s
python3 - "$NXTAKT_TEST_PROJECT" <<"CHECK"
import re,sys
n=[(float(a),float(b),int(c)) for a,b,c in re.findall(r"^    note (\S+) (\S+) (\d+)",open(sys.argv[1]).read(),re.M)]
assert len(n)==16,n
for i,pcs in enumerate([{0,3,7,10},{7,10,2,5},{8,0,3,7},{5,8,0,3}]):
 assert {p%12 for t,l,p in n[i*4:i*4+4]}==pcs,n
print("PASS: natural-minor seventh chords")
CHECK
shot minor-sevenths
key ctrl+z
wheel 40 610 8 4
clk 165 595
clk 203 740
wheel 40 610 5 5
shot melody-controls

clk 294 730
clk 165 775
key ctrl+s
python3 - "$NXTAKT_TEST_PROJECT" <<'CHECK'
import re,sys
n=[(float(a),float(b),int(c)) for a,b,c in re.findall(r'^    note (\S+) (\S+) (\d+)',open(sys.argv[1]).read(),re.M)]
assert [p for t,l,p in n]==[60,59,57,55,53,52,50,48],n
assert all(t==i*.5 and l==.25 for i,(t,l,p) in enumerate(n)),n
print('PASS: descending C-major notes and timing')
CHECK
shot descending
key ctrl+z
clk 294 730
drag 260 650 260 642
clk 165 775
key ctrl+s
python3 - "$NXTAKT_TEST_PROJECT" <<'CHECK'
import re,sys
n=[(float(a),float(b),int(c)) for a,b,c in re.findall(r'^    note (\S+) (\S+) (\d+)',open(sys.argv[1]).read(),re.M)]
assert [p for t,l,p in n]==[60,62,64,65,67,69,71,72,71,69,67,65,64,62,60,62],n
assert all(t==i*.5 and l==.25 for i,(t,l,p) in enumerate(n)),n
print('PASS: up/down melody turns at octave without repeated end notes')
CHECK
shot up-down
key ctrl+z
key ctrl+s
python3 - "$NXTAKT_TEST_PROJECT" <<'CHECK'
import sys
assert '    note ' not in open(sys.argv[1]).read()
print('PASS: one Undo clears generated melody')
CHECK
