# Sourced by tools/drive.sh. No playback or note auditions.
sleep 2
clk 805 25
shot picker
clk 850 268
clk 805 25
clk 1000 427
key ctrl+s
cp "$NXTAKT_STUDIO_QA_PROJECT" "$OUT/cloned.lattice"
clk 805 25
shot clone-picker
clk 850 120
key ctrl+a
shot name-selected
key Delete
key Escape
key Escape
key ctrl+s
cp "$NXTAKT_STUDIO_QA_PROJECT" "$OUT/cancelled.lattice"
clk 805 25
clk 850 120
key ctrl+a
xd type --clearmodifiers --delay 100 "Night drive"
key Return
key Escape
key ctrl+s
cp "$NXTAKT_STUDIO_QA_PROJECT" "$OUT/renamed.lattice"
key ctrl+z
sleep 1
key ctrl+s
cp "$NXTAKT_STUDIO_QA_PROJECT" "$OUT/rename-undone.lattice"
key ctrl+y
sleep 1
key ctrl+s
cp "$NXTAKT_STUDIO_QA_PROJECT" "$OUT/rename-redone.lattice"
clk 879 25
hover 930 115
shot ghost
drag 350 110 620 110
key ctrl+s
cp "$NXTAKT_STUDIO_QA_PROJECT" "$OUT/paint.lattice"
shot painted
key Escape
clk 805 25
clk 850 427
key ctrl+s
cp "$NXTAKT_STUDIO_QA_PROJECT" "$OUT/new.lattice"
clk 879 25
drag 350 115 480 115
key ctrl+s
cp "$NXTAKT_STUDIO_QA_PROJECT" "$OUT/empty-paint.lattice"
hover 460 115
shot empty-feedback
key Escape
key ctrl+z
sleep 1
key ctrl+s
cp "$NXTAKT_STUDIO_QA_PROJECT" "$OUT/new-undone.lattice"
key ctrl+y
sleep 1
key ctrl+s
cp "$NXTAKT_STUDIO_QA_PROJECT" "$OUT/new-redone.lattice"
