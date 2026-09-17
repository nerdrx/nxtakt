# Source through tools/drive.sh with a single Spectra track, 1360x860, scale 1.
set -e
: "${NXTAKT_TEST_PROJECT:?set a disposable project path}"
patch() {
 key ctrl+s
 python3 - "$NXTAKT_TEST_PROJECT" "$OUT/$2.json" "$1" <<'CHECK'
import json,re,sys
s=open(sys.argv[1]).read()
p={'params':{k:float(v) for k,v in re.findall(r'^    param (\d+) (\S+)',s,re.M)},
   'state':re.findall(r'^    state (.*)$',s,re.M)}
assert len(p['params'])==137,len(p['params'])
if sys.argv[3]=='store': json.dump(p,open(sys.argv[2],'w'))
else: assert p==json.load(open(sys.argv[2])),(p,json.load(open(sys.argv[2])))
print('PASS:',sys.argv[3],sys.argv[2])
CHECK
}
key F7
clk 135 467
clk 460 571
# Store a default patch and a sequence patch with non-parameter arp state.
clk 120 388
patch store A
clk 440 228
clk 120 260
xd type --clearmodifiers 'Cinder Steps'
key Return
clk 120 388
patch store B
python3 - "$OUT/A.json" "$OUT/B.json" <<'CHECK'
import json,sys
a,b=[json.load(open(p)) for p in sys.argv[1:]]
assert a!=b and not a['state'] and b['state'],(a['state'],b['state'])
print('PASS: A and B differ in parameters and arp state')
CHECK
clk 603 228
clk 518 228
patch check A
# Recalling unchanged A must not create an extra undo step.
clk 518 228
key ctrl+z
patch check B
key ctrl+shift+z
patch check A
clk 682 228
patch check B
# Unstored edits remain recoverable through Undo after a recall.
clk 454 282
patch store edited
clk 518 228
patch check A
key ctrl+z
patch check edited
clk 682 228
patch check B
shot compare-B
wid=$(xd search --onlyvisible --name '^NxTakt$' | head -1)
xd windowsize "$wid" 1024 768
sleep 1
clk 484 228
patch check A
clk 648 228
patch check B
shot compact-compare
