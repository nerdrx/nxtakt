# Source through tools/drive.sh at 1360x860, scale 1, disposable Spectra clip:
# C4/E4/G4 at beat 1 and D4/F4/A4 at beat 4; all length 1, velocity 100, eight beats.
set -e
: "${NXTAKT_TEST_PROJECT:?set a disposable project path}"
check_feel() {
  key ctrl+s
  python3 - "$NXTAKT_TEST_PROJECT" "$1" <<"CHECK"
import re,sys
n={int(p):(float(t),float(l),int(v)) for t,l,p,v in re.findall(r"^    note (\S+) (\S+) (\d+) (\d+)",open(sys.argv[1]).read(),re.M)}
a={p:(1.0 if p in [60,64,67] else 4.0,1.0,100) for p in [60,64,67,62,65,69]}
mode=sys.argv[2]
if mode=="undo": assert n==a,n
elif mode=="humanize":
 assert n!=a and set(n)==set(a),n
 for p in n:
  if p in [60,64]: assert abs(n[p][0]-a[p][0])<=.025001 and n[p][1]==1 and 92<=n[p][2]<=108,n
  else: assert n[p]==a[p],n
elif mode=="selected-strum":
 a[64]=(1.125,1.0,100)
 assert n==a,n
elif mode=="tracked-selection":
 # Existing F4 at beat4 shares pitch65 after transpose: inspect full list instead.
 rows=[(float(t),float(l),int(p),int(v)) for t,l,p,v in re.findall(r"^    note (\S+) (\S+) (\d+) (\d+)",open(sys.argv[1]).read(),re.M)]
 assert sorted(rows)==sorted([(1,1,61,100),(1.125,1,65,100),(1,1,67,100),(4,1,62,100),(4,1,65,100),(4,1,69,100)]),rows
else:
 for chord in [[60,64,67],[62,65,69]]:
  order=chord if mode=="up" else chord[::-1]
  for k,p in enumerate(order): a[p]=(a[p][0]+k*.0625,1.0,100)
 assert n==a,n
print("PASS:",mode)
CHECK
}
key F7
clk 1205 543
wheel 40 610 8 5
clk 560 715
xd keydown Shift
clk 560 656
xd keyup Shift
clk 165 770
check_feel humanize
shot humanized-selection
key ctrl+z
check_feel undo
key ctrl+shift+z
check_feel humanize
key ctrl+z
# Undo restores the model; explicitly select the same chord members again.
key Escape
clk 560 715
xd keydown Shift
clk 560 656
xd keyup Shift
clk 240 644
clk 165 770
check_feel selected-strum
key Up
check_feel tracked-selection
key ctrl+z
key ctrl+z
check_feel undo
key Escape
clk 165 770
check_feel up
shot strum-up
key ctrl+z
clk 292 726
clk 165 770
check_feel down
shot strum-down
key ctrl+z
check_feel undo
