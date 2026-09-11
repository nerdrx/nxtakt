# Spectra Studio usability inspection

This is a task-based inspection of NxTakt 0.18.0 in a headless graphical
session, not a user study. It checks whether common sound-design actions work
and remain reachable; it does not establish how quickly new users learn them.

## Perspective for FL Studio users

Knobs, a piano roll, drum steps and a dedicated instrument editor provide
familiar starting points. **+ Spectra** creates an instrument track and MIDI
pattern together; **+ Drums** remains the route into drum programming.

NxTakt is Session-first: clip launching, track arming and the relationship
between Session and Arrangement differ from FL Studio's Channel Rack and
Playlist. Familiar controls should not be mistaken for identical routing or
playback behavior. The [Spectra Studio guide](SPECTRA-STUDIO.md) explains
patch editing; the [drum guide](DRUM-SEQUENCER.md) covers step programming.

## Verified tasks

| Task | Observed result |
| --- | --- |
| Create an instrument | **+ Spectra** created the instrument and a pattern. One Undo removed the complete new track. |
| Find a sound | Preset search found and loaded **Afterburn**. |
| Change an oscillator | The waveform selector opened directly, changed the waveform and supported Undo. |
| Edit integrated effects | A change to the final effect parameter, index 136, persisted. |
| Save a patch | A user preset saved successfully using an isolated configuration directory. |
| Assign modulation | Right-click assignment from LFO 1 to oscillator A Level persisted. |
| Make a custom wavetable | Drawing on a factory waveform triggered the unsaved-edit exit guard. Commit stored the custom table and selected waveform index 8. |
| Monitor live notes | C3 lit cyan while held and cleared on release. Offscreen pitches appeared in a badge. No MIDI notes were created by the visualization. |
| Work in a small window | At 1024 × 768, controls remained reachable; scrolling exposed the lower Voicing controls. |

The automated suite passed **5,154 checks**, including coverage of Spectra's
137 parameters and 185 factory preset entries. Final focused reruns also
passed: engine **766**, daemon **880**, and engine handle **385** checks.
These results support functional correctness alongside the graphical
inspection; they do not measure readability, motor accessibility or learning
time across a representative group of users.

## Final refinements and limits

The final polish includes a Sound-page scrollbar with a 24-pixel-wide drag
target and clearing preset-search keyboard focus when its sidebar is hidden.
Both were checked in the final graphical pass: dragging the scrollbar reached
Voicing without changing the patch, and hiding an active search field released
keyboard focus.

Live-note indicators follow note gates delivered to a track, including live
input and clip playback. They do not follow release tails or individually show
notes generated privately inside an instrument's arpeggiator. Folded or
scrolled-away pitches are named without moving the editor under the pointer.

Further evaluation should involve FL Studio users completing the same tasks
without coaching, particularly first-note playback, arming, clip launching and
moving between Session and Arrangement. The current inspection confirms the
paths work; their discoverability remains a question for that study.
