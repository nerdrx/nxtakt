# Sourced by tools/drive.sh. No playback or note auditions.
sleep 2
shot fresh
clk 125 25
shot add-sound
clk 290 212
key ctrl+s
cp "$NXTAKT_STUDIO_QA_PROJECT" "$OUT/drums.lattice"
shot first-drums
key ctrl+z
sleep 2
key ctrl+s
cp "$NXTAKT_STUDIO_QA_PROJECT" "$OUT/undo.lattice"
key ctrl+y
sleep 2
key ctrl+s
cp "$NXTAKT_STUDIO_QA_PROJECT" "$OUT/redo.lattice"
clk 125 25
clk 290 284
key ctrl+s
cp "$NXTAKT_STUDIO_QA_PROJECT" "$OUT/spectra.lattice"
shot spectra
clk 125 25
clk 286 118
shot record-menu
clk 398 118
shot setup-menu
