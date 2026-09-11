# NxTakt 0.16: studio workspace

This release makes the existing DAW easier to read and operate across Session,
Arrangement, clip editing, automation, browsing, and device panels. Projects and
the audio engine keep their existing formats and behavior.

This visual direction supersedes the older glass/nebula styling in DESIGN.md.

## 0.16 structural redesign

NX violet (#7700FF) and cyan (#00E5FF) anchor the interface over deep neutral
surfaces. The two-row command bar separates playback from performance settings.
Numbered track headers and individual mixer cards establish clear channels.
Returns can be shown with the A-D button in the Scenes header, freeing the
workspace when hidden. Existing track widths are preserved in projects, with a
112px display minimum for usable controls.

The larger detail panel has a material header, focused Notes, Playback and Launch
pages with full labels and aligned fields, plus an explicit Fit view action.
MIDI clips open centered on their notes with rows fitted to the pitch range,
subject to a readable minimum size. Shared controls use stronger
violet selected states; cyan marks live state and MIDI identity.

The [FL Studio usability pass](FL-STUDIO-WORKFLOW.md) documents navigation
aliases, creation entry points and deliberate workflow differences. Clip bodies
now edit without launching; triangles launch. Note double-clicks are harmless.
Hover/press feedback takes 150/90ms and respects reduced-motion settings.

## Interaction contract

- Pointer ownership is resolved against the last complete layout using the
  **current** pointer position, then checked against the control's current
  geometry. A fast move-and-click needs no preparatory hover frame. Painted
  overlap order and panel clipping still determine which control receives it.
- Button faces stay aligned with their hit boxes. Hover and press feedback use
  light and color instead of moving the target.
- Track names select tracks; scene names launch scenes. Double-click still
  renames, and an active scene rename does not launch playback.
- Double-clicks require nearby presses, so quickly moving between controls does not reset values or enter rename mode.
- Fast keyboard chords retain their modifiers and initial press until the frame handles them, including Save and Undo after key release.
- Mixer scrolling does not also scroll the clip grid. Child controls retain
  ownership over their containing return/master strip.

## Layout and appearance

- Deep neutral surfaces, softly rounded matte controls, NX violet and cyan accents and clearer secondary text. Decorative glow and the animated background are off by default.
- Text sizes: 11px supporting labels, 13px body/header, 18px transport readout; Noto Sans preferred with existing system fallbacks.
- 84px two-row transport bar; roomy transport controls; explicit Library toggle. Optional
  diagnostics disappear as width decreases instead of overlapping transport.
- 32px Session rows and 40px track headers; 28px mute/solo/arm and send controls;
  32px pan controls; wider faders. Detail defaults to 390px and has a 12px
  resize grip, with a viewport-dependent cap preserving the upper workspace.
- Piano rows fit the material (up to 18 logical px), pitches are labeled, short notes remain visible, and
  selected/hovered note ends show resize grips. A focused 300px inspector keeps the piano roll as the main working surface.
- Arrangement track titles offer a full-row automation target. Larger marker,
  automation and lane-resize targets expose the editing affordances.
- Library file rows are 32px. Plugin rows show name and maker separately; search
  matches either. Device cards, knobs, editor selectors, tabs, rack controls
  and sampler boundary grabs have more room.

These are mouse/keyboard desktop dimensions. This release does not claim
screen-reader support, a touchscreen interface, or real-Windows validation.

## Validation and development

Build and run all headless engine, IPC, daemon, device, view, handle and hit-map
checks with `make -j8 && make test`. The suite also renders a demo and scans
plugins. `tests/hit_map_test.cpp` covers rapid target changes, paint order,
clipping, padding, geometry changes and spatial double-clicks.
`tests/input_test.cpp` covers short keyboard chords, release and repeat.

Run actual input tests in a private, invisible gamescope session:

```sh
build/gen_demo /tmp/nxtakt-click-test
NXTAKT_SCALE=1 NXTAKT_SESSION=click-test \
NXTAKT_TEST_PROJECT=/tmp/nxtakt-click-test/demo.lattice \
  tools/drive.sh tests/click_gestures.sh /tmp/nxtakt-click-results \
  -- /tmp/nxtakt-click-test/demo.lattice
```

Use a newly generated disposable project: the script saves mute, selection and
fader changes and verifies undo. It intentionally sends pointer motion and
button-down together. Other GUI checks should cover Session, populated MIDI
and audio detail, Arrangement, device panels, F1 help, 1024x768 actual window
size and fractional scale. Gamescope's output size alone can scale the image;
resize the application window when testing its layout.

`tests/inspector_gestures.sh` also verifies inspector navigation, clip loop and
note duplication, device bypass, logarithmic parameter editing and undo against
a disposable saved project. Run it with the same harness and environment.

The source of record is `nerdrx/nxtakt`. `make dist VERSION=0.16.0` creates an
NX Hub-compatible Linux archive and checksum. Pushing a `v*` tag triggers the
GitHub release workflow, which builds on Ubuntu, runs the full suite, checks
the packaged binaries and only then publishes assets. Prefer those portable
artifacts over uploading a build from a rolling-release workstation.
