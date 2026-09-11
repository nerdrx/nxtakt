# Usability pass: coming from FL Studio

This is a heuristic review backed by scripted GUI checks, not a study with
recruited users. The strongest remaining difference is the musical structure:
NxTakt Session holds clips in track/scene slots; it is not FL's Channel Rack step
sequencer. Arrangement is the linear timeline. Placing a Session clip there
creates an independent arrangement copy.

## Familiar navigation

| Key | NxTakt destination |
| --- | --- |
| F5 | Arrangement (the Playlist equivalent) |
| F6 | Session / clips |
| F7 | Editor for the currently selected clip, including arrangement clips |
| F9 | Mixer-focused Session view |
| F1 | Shortcut reference |

These aliases follow [FL Studio's documented view shortcuts](https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/basics_shortcuts.htm).
Existing NxTakt shortcuts remain available. Ctrl+B still toggles the browser;
it does not duplicate notes as it does in FL. Use the visible Duplicate or
Double loop action instead.

## Friction addressed

- Empty clip inspectors offer **Add instrument**, opening the instrument
  category, or **Create MIDI clip** once a note-playing device is present.
- The device catalog separates **All**, **Instruments**, and **Effects**.
- Clip bodies select for editing; the launch triangle performs playback.
- Double-clicking a note selects it instead of deleting it. Right-click and
  Delete remain explicit erase actions. This avoids destructive surprises for
  users accustomed to [FL's note properties gesture](https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/pianoroll.htm).
- Inspector pages use complete labels; numeric fields look editable. The
  inspector scrolls when shortened, without scrolling the piano roll.
- Empty Arrangement explains the existing transfer gesture: drag a Session
  clip, press F5 while holding, then drop it onto the timeline.

## Motion

Hover feedback fades over 150 ms; press/grab feedback takes 90 ms. Knob arcs
and fader handles change light/color while hit boxes and parameter values stay
immediate. Selected tabs retain their short transition. There are no ambient
loops or moving controls. Set `NXTAKT_REDUCED_MOTION=1` for immediate hover/press feedback and softer tab transitions.

## Practical assessment

Editing and mixing now offer more familiar entry points. Learning Session's
clip/scene model still takes an explanation, especially for someone who starts
in FL's step sequencer. These shortcuts and labels reduce that transition;
they do not imply full FL shortcut or workflow compatibility.
