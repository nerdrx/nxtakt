# Sourced by tools/drive.sh. No playback or note auditions.
sleep 2
clk 630 405
clk 630 405
clk 385 405
shot brush-enabled
drag 350 110 730 110
key ctrl+s
cp "$NXTAKT_STUDIO_QA_PROJECT" "$OUT/paint.lattice"
shot painted
key ctrl+z
key ctrl+s
cp "$NXTAKT_STUDIO_QA_PROJECT" "$OUT/undo.lattice"
key ctrl+y
key ctrl+s
cp "$NXTAKT_STUDIO_QA_PROJECT" "$OUT/redo.lattice"
drag 350 110 730 110
key ctrl+s
cp "$NXTAKT_STUDIO_QA_PROJECT" "$OUT/duplicate.lattice"
drag 860 110 990 110
key ctrl+s
cp "$NXTAKT_STUDIO_QA_PROJECT" "$OUT/second.lattice"
key ctrl+z
key ctrl+s
cp "$NXTAKT_STUDIO_QA_PROJECT" "$OUT/second-undo.lattice"
key Escape
clk 125 25
shot menu
