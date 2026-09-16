# Source through tools/drive.sh with an empty Spectra MIDI clip, 1360x860.
# Requires a disposable NXTAKT_TEST_PROJECT and isolated XDG_CONFIG_HOME.
set -e
: "${NXTAKT_TEST_PROJECT:?set a disposable project path}"
: "${XDG_CONFIG_HOME:?set an isolated configuration directory}"
key F7
clk 135 467
clk 120 260
xd type --clearmodifiers afterburn
key Return
clk 120 387
key ctrl+s
python3 - "$NXTAKT_TEST_PROJECT" "$OUT/baseline.json" <<'CHECK'
import json,re,sys
s=open(sys.argv[1]).read()
p={int(k):float(v) for k,v in re.findall(r'^    param (\d+) (\S+)',s,re.M)}
assert len(p)==137
json.dump(p,open(sys.argv[2],'w'))
CHECK
check_patch() {
  key ctrl+s
  python3 - "$NXTAKT_TEST_PROJECT" "$OUT/baseline.json" "$1" <<'CHECK'
import json,re,sys
s=open(sys.argv[1]).read()
p={int(k):float(v) for k,v in re.findall(r'^    param (\d+) (\S+)',s,re.M)}
a={int(k):v for k,v in json.load(open(sys.argv[2])).items()}
changed={k for k in p if p[k]!=a[k]}
if sys.argv[3]=='sound':
    assert len(changed)>10,changed
    protected={0,2,3,8,10,11,38,39,40,*range(68,125),131}
    assert not changed & protected,changed & protected
elif sys.argv[3]=='macros': assert changed and changed<={94,95,96,97},changed
else: assert not changed,changed
print('PASS:',sys.argv[3],len(changed),'changed parameters')
CHECK
}
clk 457 228
check_patch sound
shot variation
key ctrl+z
check_patch undo
clk 597 228
check_patch macros
key ctrl+z
check_patch undo
clk 242 387
python3 - "$XDG_CONFIG_HOME/nxtakt/spectra-favorites.txt" <<'CHECK'
import sys
assert 'factory:BA Afterburn' in open(sys.argv[1]).read()
print('PASS: starred preset persisted')
CHECK
clk 212 223
shot favorites
clk 1270 117
clk 50 467
clk 270 550
clk 165 784
key ctrl+s
python3 - "$NXTAKT_TEST_PROJECT" <<'CHECK'
import re,sys
notes=re.findall(r'^    note (\S+) (\S+) (\d+)',open(sys.argv[1]).read(),re.M)
assert notes==[('0','1','60'),('0','1','64'),('0','1','67')],notes
print('PASS: inserted C major chord with correct timing')
CHECK
shot chord
key ctrl+z
key ctrl+s
python3 - "$NXTAKT_TEST_PROJECT" <<'CHECK'
import sys
assert '    note ' not in open(sys.argv[1]).read()
print('PASS: one Undo removes complete chord')
CHECK
drag 210 638 210 623
clk 165 784
key ctrl+s
python3 - "$NXTAKT_TEST_PROJECT" <<'CHECK'
import re,sys
notes=re.findall(r'^    note (\S+) (\S+) (\d+)',open(sys.argv[1]).read(),re.M)
assert len(notes)==3 and all(float(n[0])>0 and float(n[0])+float(n[1])<=8 for n in notes),notes
print('PASS: edited start beat persists into chord insertion')
CHECK
key ctrl+z
drag 210 638 210 653
clk 165 595
clk 203 740
wheel 40 610 3 5
shot scale-controls
clk 165 775
key ctrl+s
python3 - "$NXTAKT_TEST_PROJECT" <<'CHECK'
import re,sys
notes=re.findall(r'^    note (\S+) (\S+) (\d+)',open(sys.argv[1]).read(),re.M)
assert [int(n[2]) for n in notes]==[60,62,64,65,67,69,71,72],notes
assert [float(n[0]) for n in notes]==[i*.5 for i in range(8)],notes
assert all(float(n[1])==.25 for n in notes),notes
print('PASS: C major scale run, eight editable MIDI notes, saved timing')
CHECK
shot scale-run
key ctrl+z
key ctrl+s
python3 - "$NXTAKT_TEST_PROJECT" <<'CHECK'
import sys
assert '    note ' not in open(sys.argv[1]).read()
print('PASS: one Undo removes complete scale run')
CHECK
