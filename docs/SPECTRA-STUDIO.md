# Spectra Studio

Spectra Studio in NxTakt 0.18.0 is a focused editor for the built-in wavetable
instrument. Click **+ Spectra** in the Session toolbar to create a track and
open the editor. **Back to chain** returns to the track's devices.

## Find and play a sound

Open **Presets**, search by name or choose a category, then select a patch.
The factory library contains **185 entries: Init and 184 sounds**, including
**64 new Studio sounds**. Existing factory entries retain their names and
indices. The sounds use built-in procedural tables and require no downloads.
See the [factory sound catalog](SPECTRA-PRESETS.md) for categories, patch
character and macro assignments.

Use **Hear C3** or **Hear chord** to audition the selected track's instrument.
You can also play an attached MIDI keyboard or enable the computer keyboard
with **Ctrl+Shift+K**. Live MIDI reaches armed tracks. **Silence** stops sounding
notes on armed instruments, including a latched arpeggiator.

The preset header identifies modified sounds. To keep a variation, enter a
new name in the preset sidebar and choose **Save new preset**. Saving the
project also preserves the instrument's settings. Hide **Presets** when you
want more space for editing.

## Five editing pages

| Page | What to shape |
| --- | --- |
| **Sound** | Two wavetable oscillators, pitch, unison, detune, stereo width and warp; filter and amplitude envelope; sub, noise, output and voice settings. |
| **Effects** | Integrated chorus, ping-pong delay and reverb. These settings belong to the patch. |
| **Modulation** | Three LFOs, two additional envelopes and eight routing slots with source, destination, amount and curve. **Detailed tools** opens the custom LFO and deeper modulation controls. |
| **Arpeggiator** | Note order, timing, octave range, gate, swing, hold, retrigger, velocity and a 16-step pattern. |
| **Wavetable** | Custom wavetable drawing and editing. Apply or discard a pending drawing before changing patches or leaving the editor. |

Selectors open a list directly. Numeric fields can be dragged or scrolled;
right-click controls for their available actions. Pages scroll when their
contents exceed the available height.

## Four performance macros

The Sound page exposes four macros. Every new Studio sound assigns all four:
Macro 1 opens the filter, while the remaining assignments vary with the patch.
Start at zero to hear the authored sound, then raise a macro gradually to
explore it. Labels reflect the first assigned destination; the
[patch catalog](SPECTRA-PRESETS.md) describes each macro's musical purpose.
The Modulation page lets you inspect and change routes.

## Program the arpeggiator

Enable the arpeggiator, then hold a note or chord. Choose a note order and a
Clock division; choose **free** for a rate in Hz. **Hold** keeps the last notes
after key release. **Retrigger** controls whether new notes restart the pattern.

Click a pattern cell to cycle **On → Tie → Rest**. A tie extends the previous
note. Scroll over a cell to transpose it between two octaves down and two
up. Drag its separate lower velocity bar to set its displayed percentage (16 velocity levels); choose
**Pattern** in the Velocity selector to hear those levels. **Played** follows
the incoming velocity and **Fixed** uses the Fixed velocity field.

**Steps** sets the active pattern length. Cells beyond that length retain their
settings for later. Narrow windows arrange the 16 cells in two rows of eight.
Edits participate in undo and preserve the patch's other state, including
custom wavetables.

## Import a wavetable

Open **Files** and drop a WAV file onto an oscillator's waveform preview on
the Sound page. The imported table becomes that oscillator's custom waveform.
Use the Wavetable page for further editing. Factory sounds remain usable
without imported files.

## See the notes you play

The piano roll highlights the selected track's live MIDI and clip note gates
with cyan keys, a subtle pitch guide and a mark at the playhead. When stopped,
the live mark appears at the grid's left edge. Notes outside the visible pitch
range, including folded-away pitches, are listed without moving your view.
These indicators do not create or record MIDI notes.

This display follows notes delivered to the track: hardware MIDI, the computer
keyboard, auditions and Session or Arrangement clips. It represents held note
gates, not the duration of an instrument's release tail. Notes generated
privately inside a plugin, including Spectra's arpeggiator, are not reported
individually; the incoming held pitches are shown instead.

The existing [Drum Machine and step sequencer](DRUM-SEQUENCER.md) remain
available through **+ Drums**.

## Upgrade the interface and daemon together

NxTakt 0.18.0 expands the parameter transport used by Spectra's integrated
effects. Install matching **nxtakt** and **nxtaktd** versions and restart both
so the interface and audio engine use the same protocol. Mixing this release's
interface with an older running daemon is not supported.
