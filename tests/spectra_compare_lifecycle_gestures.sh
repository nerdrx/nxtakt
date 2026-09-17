# Run with HOME set to a disposable directory containing Music/reload.lattice.
# Start a separate copy of that single-Spectra project, scale 1, 1360x860.
set -e
: "${NXTAKT_TEST_PROJECT:?set a disposable project path}"
: "${NXTAKT_COMPARE_HOME:?set the disposable HOME used to launch NxTakt}"
[[ "$HOME" == "$NXTAKT_COMPARE_HOME" && "$HOME" == /tmp/* ]]
source "$REPO/tests/spectra_compare_gestures.sh"

# Deleting and undoing deletion restores the same instrument and its slots.
clk 1270 117
clk 576 571
key ctrl+z
clk 460 571
clk 518 228
patch check second-A
clk 682 228
patch check second-B

# Undoing creation then branching into a new instrument can reuse its UID.
# The new instrument must start with empty slots, not the discarded sound.
clk 724 26
export COMPARE_TRACK=2
clk 120 260
xd type --clearmodifiers 'Cinder Steps'
key Return
clk 120 388
clk 440 228
key ctrl+z
key ctrl+z
clk 724 26
patch store third-default
clk 518 228
patch check third-default

# Load another project using the real browser confirmation gesture.
# reload.lattice reuses the first instrument's UID with a different patch.
clk 1270 117
clk 56 170
# '..' is first; the fixture is the only file in the disposable Music folder.
dbl 90 343
dbl 90 343
grep -F "from $HOME/Music/reload.lattice" "$LOG"
export NXTAKT_TEST_PROJECT="$HOME/Music/reload.lattice"
export COMPARE_TRACK=0
clk 460 571
patch store reloaded
clk 518 228
patch check reloaded
clk 682 228
patch check reloaded
shot cleared-project-slots
