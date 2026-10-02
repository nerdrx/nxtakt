# Sourced by tools/drive.sh; isolated fixture only. Logical pixels at scale 1.
: "${NXTAKT_STUDIO_QA_PROJECT:?Use tools/studio-smoke.sh}"
sleep 2
shot startup
mark place-drop
clk 630 405
clk 630 405
clk 672 405
key ctrl+s
cp "$NXTAKT_STUDIO_QA_PROJECT" "$OUT/placed.lattice"
shot playlist-populated
mark edit-pattern
clk 363 580
key ctrl+s
cp "$NXTAKT_STUDIO_QA_PROJECT" "$OUT/step.lattice"
key ctrl+z
key ctrl+s
cp "$NXTAKT_STUDIO_QA_PROJECT" "$OUT/undo.lattice"
key ctrl+y
key ctrl+s
cp "$NXTAKT_STUDIO_QA_PROJECT" "$OUT/redo.lattice"
mark rename-channel
clk 222 580
# Let each deletion reach a rendered frame; burst events can coalesce.
key BackSpace
key BackSpace
key BackSpace
key BackSpace
xd type --clearmodifiers --delay 90 "Keys QA"
key Return
key ctrl+s
cp "$NXTAKT_STUDIO_QA_PROJECT" "$OUT/renamed.lattice"
key ctrl+z
key ctrl+s
cp "$NXTAKT_STUDIO_QA_PROJECT" "$OUT/rename-undone.lattice"
clk 243 580
shot piano-pattern
clk 822 140
clk 243 580
shot piano-restored
clk 890 140
clk 243 442
shot sample-editor
clk 890 140
mark move-resize
# Rack starts x80,y353,w660,h330.
drag 150 370 160 320
drag 737 620 795 660
shot rack-resized
mark sound-editor
clk 125 25
clk 168 143
clk 125 25
shot spectra-front
clk 1198 244
shot presets-compact
clk 1198 244
clk 100 722
shot spectra-after-track-select
key F7
shot piano-and-spectra
clk 890 140
clk 100 320
clk 1170 25
shot spectra-raised
mark audio-and-restart
clk 1310 25
shot audio-front
clk 125 25
shot more-over-audio
clk 125 25
clk 1250 540
sleep 2
shot engine-restarted
key ctrl+s
cp "$NXTAKT_STUDIO_QA_PROJECT" "$OUT/restarted.lattice"
mark mixer-fader
clk 1328 92
drag 447 754 447 765
key ctrl+s
cp "$NXTAKT_STUDIO_QA_PROJECT" "$OUT/fader.lattice"
key ctrl+z
key ctrl+s
cp "$NXTAKT_STUDIO_QA_PROJECT" "$OUT/fader-undone.lattice"
mark drum-floating
clk 125 25
clk 64 143
clk 125 25
shot drum-floating
