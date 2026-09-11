# Drum sequencer

Click **+ Drums** in the Session toolbar. NxTakt creates a Drum Machine track
and an empty MIDI pattern, then opens the step editor. Click a step to enable
it, click again to remove it, and press **Play pattern** to hear the loop.
Playback follows the project's tempo and launch grid.

## Sounds

Choose **808**, **707**, or **909** on the **Kit** inspector page. These are
original synthesized sounds inspired by those drum-machine families, not
recordings or emulations of the original hardware. All eight sounds work
without external plugins or sample downloads.

**Edit drum sounds** opens the instrument controls. Each voice has its own
level, tuning and decay; Output controls the whole kit. Velocity changes the
strength of each hit. A closed hi-hat cuts off the open hi-hat.

| Lane | MIDI note |
| --- | ---: |
| Kick | 36 |
| Snare | 38 |
| Clap | 39 |
| Closed hat | 42 |
| Open hat | 46 |
| Tom | 45 |
| Rim | 37 |
| Cowbell | 56 |

## Pattern editing

- Click a lane name to audition its sound.
- Click a cell to toggle a hit; right-click explicitly erases it.
- Hold Ctrl and scroll over an enabled step to change its velocity.
- Choose 16, 32 or 64 steps, then press **Set to** to apply the length.
  **Trim to** shortens the pattern and removes notes beyond its end.
- Use the page arrows to edit each group of sixteen steps.
- Choose a starter rhythm and press **Replace page**. This replaces the eight
  drum lanes on the current page, preserving other pages and other pitches.
- Ctrl+Z restores step edits, erased notes, replaced pages and trimmed notes.
- On a smaller editor, scroll vertically for lanes; Shift+scroll moves across
  the grid when it cannot fit horizontally.

Each step is a sixteenth note. Four-step shading helps locate the beat, and
a moving highlight follows playback without animating the whole surface.

## From FL Studio

Think of each lane as one channel in a step sequencer. The kit selector changes
the sounds; the rhythm stays intact. **Play pattern** launches this clip, while
Space controls the shared transport. **Stop pattern** stops this track at its
launch boundary, allowing other tracks to continue.

The **Steps / Piano** switch shows two views of the same MIDI notes. Use Piano
for off-grid timing, note lengths and pitches outside the drum map. In Steps,
each cell represents its entire sixteenth-note interval: toggling it off removes
all notes for that lane within the cell, including off-grid notes.

Drum clips also open in the arrangement's step editor. Patterns, kit selection
and sound parameters save in the normal project file and work in offline
renders. Automatic drum-editor detection currently applies to Drum Machine
devices placed directly on a track.
