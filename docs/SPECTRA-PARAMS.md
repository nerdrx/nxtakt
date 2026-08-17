# Spectra — the wavetable instrument's parameter contract (FROZEN)

`nxtakt:spectra` · Internal instrument · audioIn 0, audioOut 2, hasMidiIn true

This file is the interface between the DSP (src/plugin) and the editor
(src/ui). Ids are indices and indices persist in saved sets, so entries may be
APPENDED but never reordered, renamed-in-meaning, or removed. Both sides build
against this file; where an implementation and this file disagree, the
implementation is wrong.

Ranges are in the target's own units. `int` means ParamInfo::isInt.

| id | name | range | notes |
|----|------|-------|-------|
| 0  | A Table      | 0..7 int      | wavetable index, see the table list |
| 1  | A Position   | 0..1          | frame morph position — THE control |
| 2  | A Coarse     | -24..24 int   | semitones |
| 3  | A Fine       | -100..100     | cents |
| 4  | A Level      | 0..1          | |
| 5  | A Unison     | 1..7 int      | voices per note for osc A |
| 6  | A Detune     | 0..100        | cents, max spread across unison |
| 7  | A Spread     | 0..1          | stereo width of the unison fan |
| 8  | B Table      | 0..7 int      | |
| 9  | B Position   | 0..1          | |
| 10 | B Coarse     | -24..24 int   | |
| 11 | B Fine       | -100..100     | |
| 12 | B Level      | 0..1          | default 0 — osc B off out of the box |
| 13 | B Unison     | 1..7 int      | |
| 14 | B Detune     | 0..100        | |
| 15 | B Spread     | 0..1          | |
| 16 | Noise        | 0..1          | white noise level |
| 17 | Sub          | 0..1          | sine one octave below, follows glide |
| 18 | Cutoff       | 20..20000 log | SVF |
| 19 | Resonance    | 0..1          | |
| 20 | Filter Type  | 0..2 int      | 0 LP · 1 BP · 2 HP |
| 21 | Drive        | 0..24         | dB, pre-filter |
| 22 | Env2>Cutoff  | -1..1         | bipolar depth |
| 23 | Keytrack     | 0..1          | cutoff follows pitch |
| 24 | Attack       | 0.1..5000 log | ms, ENV1 = amp, mandatory |
| 25 | Decay        | 1..5000 log   | ms |
| 26 | Sustain      | 0..1          | |
| 27 | Release      | 1..8000 log   | ms |
| 28 | E2 Attack    | 0.1..5000 log | ms, ENV2 = mod envelope |
| 29 | E2 Decay     | 1..5000 log   | ms |
| 30 | E2 Sustain   | 0..1          | |
| 31 | E2 Release   | 1..8000 log   | ms |
| 32 | LFO Rate     | 0.01..40 log  | Hz, used when LFO Sync = 0 |
| 33 | LFO Sync     | 0..9 int      | 0 free · 1=4 bars · 2=2 · 3=1 bar · 4=1/2 · 5=1/4 · 6=1/8 · 7=1/16 · 8=1/4T · 9=1/8T — via setTransport, Tempo-param fallback NOT needed (instrument postdates the transport push) |
| 34 | LFO>Position | -1..1         | applies to BOTH osc positions |
| 35 | LFO>Cutoff   | -1..1         | |
| 36 | LFO>Pitch    | 0..100        | cents, vibrato |
| 37 | LFO Shape    | 0..4 int      | 0 sine · 1 tri · 2 saw · 3 square · 4 S&H |
| 38 | Glide        | 0..500        | ms, constant-time portamento |
| 39 | Voices       | 1..16 int     | polyphony cap, voice-steal quietest |
| 40 | Master       | 0..1.5        | |
| 41 | Env2>Position| -1..1         | bipolar, both oscs |

## The eight tables (index = `A/B Table` value; names are UI labels)

0 **Basic** — saw→pulse continuum · 1 **PWM** — pulse width sweep ·
2 **Harmonic** — odd/even harmonic crossfades · 3 **Formant** — vowel-ish
resonant peaks sweeping up · 4 **Bell** — inharmonic FM-flavoured spectra ·
5 **Digital** — hard-sync/bit-flavoured brightness ramp · 6 **Vox** —
band-limited formant stack, softer than 3 · 7 **Fold** — sine into rising
wavefold.

All PROCEDURAL — generated at first prepare() into a shared immutable set, no
asset files. 32 frames × 2048 samples per table, mip chain per frame
(FFT-truncated to the Nyquist of each octave) for band-limited playback;
interpolation is linear across sample, frame, and mip.

## Modulation summary (fixed routing, v1 — a matrix can append params later)

ENV1→amp (always) · ENV2→cutoff (22) and position (41) · LFO→position (34),
cutoff (35), pitch (36) · velocity→amp (fixed 30% floor, not a param yet) ·
keytrack→cutoff (23).
