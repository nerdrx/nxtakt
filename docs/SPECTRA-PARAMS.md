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

---

# v2 — the parity push (FROZEN)

Appends ids 42..99. Everything above this line is untouched: the 42 v1 ids
keep their meaning, their ranges, their defaults, their fixed routings. Two
v1 params get strictly-compatible widenings (20 and 38, below); nothing else
about v1 moves. **Gate: a v1 state loaded into a v2 build must render
bit-identical**, which is why every new param's default is "do what v1 did".

Rules for this revision and all later ones:

- New ids come in **blocks** with reserved tail ids. A reserved id IS
  registered (name `—`, range 0..1, default 0, hidden in the editor, ignored
  by the DSP) so the addParam() sequence stays dense and indices never move.
  A later revision may give a reserved id real meaning only if default 0
  means "no effect".
- **Curve** column: `lin` is the identity mapping, `log` is the skew flag v1
  already uses (the `true` argument to addParam) — same thing others call exp.
- **State/versioning rule.** A saved set stores `(id, plain target-unit
  value)` pairs. Ids absent from an old state read as the registered default.
  Ids in a state that the running build does not know are ignored. Presets
  load as `reset every id to default, then apply overrides` — so every
  preset, however old, defines **all** ids, including ids added after it was
  written.

## Widened v1 params (the only two, ever, under this rule: a widening must be
a superset in plain units so every stored value keeps its exact meaning)

| id | was | becomes | notes |
|----|-----|---------|-------|
| 20 | Filter Type 0..2 int | 0..5 int | 0 LP12 · 1 BP12 · 2 HP12 (the v1 values, v1's single SVF stage is 12 dB/oct) · 3 LP24 · 4 HP24 · 5 Notch. 24s are two identical SVF stages in series sharing cutoff and resonance. Notch is the LP+HP sum of one stage. |
| 38 | Glide 0..500 ms | 0..2000 ms | still constant-time portamento, still lin, 0 = off. Old states store plain ms and are untouched. |

Drive (0..24 dB, pre-filter) already exists as id 21 and is a matrix
destination below; v2 adds no second drive.

## Block: sub & noise — ids 42..47

Sub Level stays id 17, Noise Level stays id 16. Defaults reproduce v1: sine
sub one octave down, white noise.

| id | name | range | default | unit | curve | meaning |
|----|------|-------|---------|------|-------|---------|
| 42 | Sub Shape   | 0..2 int | 0 | | lin | 0 sine · 1 triangle · 2 square (polyBLEP); the sub follows osc A's post-glide pitch shifted by Sub Oct. |
| 43 | Sub Oct     | -2..0 int | -1 | oct | lin | octave offset of the sub; -1 is v1's "one octave below", 0 is unison with the note. |
| 44 | Noise Color | 0..1 | 1 | | lin | one-pole 6 dB/oct lowpass on the white source, fc = 200·100^color Hz; at exactly 1.0 the filter is bypassed (bit-identical to v1 white). |
| 45 | Noise Track | 0..1 int | 0 | | lin | 1 = the color filter's fc is multiplied by f_note/261.63 (C4 reference), post-glide. |
| 46 | —           | | | | | reserved |
| 47 | —           | | | | | reserved |

## Block: warp — ids 48..53

One mode + amount per oscillator, applied per unison voice to the read phase
p ∈ [0,1) before frame/mip interpolation.

| id | name | range | default | unit | curve | meaning |
|----|------|-------|---------|------|-------|---------|
| 48 | A Warp     | 0..7 int | 0 | | lin | 0 Off · 1 Sync · 2 Bend+ · 3 Bend- · 4 Mirror · 5 Quantize · 6 FM · 7 RM — exact semantics below. |
| 49 | A Warp Amt | 0..1 | 0 | | lin | depth `a` in the mode formulas; at 0 every mode is bit-identical to Off. |
| 50 | B Warp     | 0..7 int | 0 | | lin | as 48, for osc B. |
| 51 | B Warp Amt | 0..1 | 0 | | lin | as 49. |
| 52 | —          | | | | | reserved |
| 53 | —          | | | | | reserved |

Mode semantics (`a` = amount, `p` = raw phase ramp, `m` = the modulator):

- **Off** — lookup phase is p; a is ignored.
- **Sync** — slave ratio r = 1 + 7a; lookup phase = frac(p·r), mip chosen
  for f·r; the step at the master wrap is not BLEP'd — mild aliasing at high
  r is accepted, same policy as table 5's hard-sync flavour.
- **Bend+** — lookup phase = p^(1/(1+3a)): the ramp is lifted, the front of
  the cycle is read fast and the tail stretched.
- **Bend-** — lookup phase = p^(1+3a): the exact inverse bias — front
  stretched, tail rushed.
- **Mirror** — lookup phase = (1-a)·p + a·(1-|2p-1|): crossfade toward a
  triangle-folded phase that reads the cycle forward then backward.
- **Quantize** — lookup phase = floor(p·N)/N with N = 2 + round(62·(1-a)):
  quantizes the READ PHASE (a staircase through the frame, 64 steps down to
  2), never the output amplitude.
- **FM** — through-zero linear FM of this osc by the other: the phase
  increment is scaled by (1 + k·m), k = 2^(3a) - 1, so a full-scale
  modulator peak detunes by exactly 36·a semitones (k may drive the
  increment negative; that is the through-zero).
- **RM** — out = (1-a)·y + a·(y·m): dry-to-ring-mod crossfade.

The modulator tap `m` for FM/RM is the OTHER oscillator's unison voice 0,
pre-warp, pre-level, mono, **delayed one sample** — that one-sample delay is
what makes A↔B mutual FM/RM well-defined and block-size invariant. An osc
whose Level is 0 still computes voice 0 whenever the other osc's mode is
FM/RM. Except under Sync, the mip is chosen from the unwarped fundamental;
Bend/Mirror/Quantize/FM can locally exceed it and alias mildly — accepted.

## Block: LFO2 & LFO3 — ids 54..61

Same block shape and semantics as LFO1's ids 32/33/37: same sync-division
table as id 33, same shape list as id 37 (0 sine · 1 tri · 2 saw · 3 square
· 4 S&H), same phase/trigger behaviour as LFO1. No fixed routings — LFO2/3
reach the signal only through the matrix.

| id | name | range | default | unit | curve | meaning |
|----|------|-------|---------|------|-------|---------|
| 54 | L2 Rate  | 0.01..40 | 2 | Hz | log | used when L2 Sync = 0. |
| 55 | L2 Sync  | 0..9 int | 0 | | lin | division table of id 33, verbatim. |
| 56 | L2 Shape | 0..4 int | 0 | | lin | shape list of id 37, verbatim. |
| 57 | L3 Rate  | 0.01..40 | 2 | Hz | log | |
| 58 | L3 Sync  | 0..9 int | 0 | | lin | |
| 59 | L3 Shape | 0..4 int | 0 | | lin | |
| 60 | —        | | | | | reserved |
| 61 | —        | | | | | reserved |

## Block: ENV3 — ids 62..67

Assignable ADSR, the exact shape (ranges, defaults, curves) of ENV2's block
28..31. Reaches the signal only through the matrix.

| id | name | range | default | unit | curve | meaning |
|----|------|-------|---------|------|-------|---------|
| 62 | E3 Attack  | 0.1..5000 | 2 | ms | log | |
| 63 | E3 Decay   | 1..5000 | 300 | ms | log | |
| 64 | E3 Sustain | 0..1 | 0 | | lin | |
| 65 | E3 Release | 1..8000 | 150 | ms | log | |
| 66 | —          | | | | | reserved |
| 67 | —          | | | | | reserved |

## Block: modulation matrix — ids 68..93

Eight slots of (source, destination, amount); slot k (0-based) is ids
68+3k / 69+3k / 70+3k. A slot with source Off or dest Off contributes
nothing and costs nothing. Several slots may hit one destination; they sum.
The fixed v1 routings (22, 23, 34, 35, 36, 41) stay live and sum with the
matrix.

| id | name | range | default | unit | curve | meaning |
|----|------|-------|---------|------|-------|---------|
| 68 | M1 Src | 0..13 int | 0 | | lin | source enum below. |
| 69 | M1 Dst | 0..19 int | 0 | | lin | destination enum below. |
| 70 | M1 Amt | -1..1 | 0 | | lin | slot depth; the per-destination scaling is below. |
| 71..91 | M2..M8 Src/Dst/Amt | | | | | same triple, seven more times (M8 Amt = 91). |
| 92 | —      | | | | | reserved |
| 93 | —      | | | | | reserved |

**Sources** (value domain in brackets): 0 Off · 1 LFO1 [-1..1] · 2 LFO2
[-1..1] · 3 LFO3 [-1..1] · 4 ENV2 [0..1] · 5 ENV3 [0..1] · 6 Velocity
[0..1] · 7 KeyTrk [(note-60)/60, clamped -1..1] · 8 Aftertouch [channel
pressure, 0..1, applies to all voices] · 9..12 Macro1..4 [0..1, ids 94..97]
· 13 Random [uniform -1..1, drawn once at note-on from a hash of (channel,
note number, the note-on's absolute timeline sample as stamped on the
event) — the note's stable identity. NO wall-clock, no RNG state carried
between notes: the same render yields the same values, and block-size
bit-identity (the determinism gate) holds by construction].

**Destinations.** Normalized-domain targets sum as
`eff = clamp(base_norm + fixed_v1_routings + Σ amt·src, 0, 1)` where
base_norm is the base param's position on its own lin/log scale (for Cutoff
that is exactly the domain Env2>Cutoff already sums in), mapped to units
after the clamp. Unit-domain targets sum in the stated unit and clamp to the
stated span. Evaluated per voice at audio rate.

0 Off · 1 A Pos [norm, ±1 = full 0..1] · 2 B Pos [norm] · 3 A Warp Amt
[norm] · 4 B Warp Amt [norm] · 5 A Level [norm] · 6 B Level [norm] ·
7 A Pitch [unit: ±24 st full-scale, added after coarse/fine/glide, total
matrix pitch clamped ±48 st] · 8 B Pitch [unit, as 7] · 9 Sub Level [norm]
· 10 Noise Level [norm] · 11 Cutoff [norm, log domain of id 18] ·
12 Resonance [norm] · 13 Drive [unit: ±24 dB, clamped to 0..24] ·
14 A Detune [unit: ±100 ct, clamped 0..100] · 15 B Detune [unit, as 14] ·
16 Pan [unit: -1 hard left..+1 hard right, clamp ±1; equal-power, applied
to the voice's summed output before Master; base is centre] · 17 LFO1 Rate
[norm, log domain of id 32; only when that LFO is free (Sync 0), ignored
when synced] · 18 LFO2 Rate [as 17] · 19 LFO3 Rate [as 17].

## Block: macros — ids 94..97

| id | name | range | default | unit | curve | meaning |
|----|------|-------|---------|------|-------|---------|
| 94 | Macro 1 | 0..1 | 0 | | lin | does nothing until a matrix slot reads it; exists so one knob (and one automation address) can drive many destinations. |
| 95 | Macro 2 | 0..1 | 0 | | lin | |
| 96 | Macro 3 | 0..1 | 0 | | lin | |
| 97 | Macro 4 | 0..1 | 0 | | lin | |

## Block: voice — ids 98..99

Glide time is the widened id 38, above.

| id | name | range | default | unit | curve | meaning |
|----|------|-------|---------|------|-------|---------|
| 98 | Voice Mode | 0..2 int | 0 | | lin | 0 Poly (v1 behaviour, Voices id 39 caps polyphony) · 1 Mono (one voice, every note-on retriggers ENV1-3, glides from the previous pitch when Glide > 0, note-off falls back to the most recent still-held note) · 2 Legato (Mono, but overlapped note-ons do NOT retrigger envelopes — they only glide; detached notes retrigger). In modes 1-2, id 39 is ignored. |
| 99 | —          | | | | | reserved |

## v2 id map

| block | ids | functional |
|-------|-----|-----------|
| v1 (frozen) | 0..41 | 42 |
| sub & noise | 42..47 | 4 |
| warp | 48..53 | 4 |
| LFO2/LFO3 | 54..61 | 6 |
| ENV3 | 62..67 | 4 |
| matrix | 68..93 | 24 |
| macros | 94..97 | 4 |
| voice | 98..99 | 1 |

**100 ids total (0..99): 89 functional, 11 reserved.**

## The factory bank — 48 presets + Init

Presets are ordinary knob moves (loadPreset resets all ids to defaults, then
applies overrides); nothing in a saved set references a preset index, so
retiring or renaming presets never breaks a project. The v1 inline demo list
retires; its sounds may be re-authored into the bank.

| category | tag | count | character |
|----------|-----|-------|-----------|
| Bass     | BA | 9 | low end; mostly Mono/Legato with glide, drive, LP24 |
| Lead     | LD | 9 | front-of-mix mono/poly solos; warp and unison |
| Pad      | PD | 8 | slow attack, wide unison, matrix motion on positions |
| Keys     | KY | 7 | playable poly, fast attack, velocity in the matrix |
| Pluck    | PL | 6 | zero-sustain, E2/E3 to cutoff/position |
| FX       | FX | 5 | non-tonal, S&H, Random-per-note, ring-mod |
| Sequence | SQ | 4 | authored at 1/16 gating with tempo-synced LFOs |

Naming: `"<tag> <Name>"` — the two-letter tag, one space, Title Case name;
letters/digits/spaces only, whole string ≤ 20 chars; no numbered names
("Bass 2" is banned). File order is the category order above, alphabetical
within a category. Row 0 is always `Init` (no tag, zero overrides) and is
not counted in the 48 — the bank file has 49 rows.

## src/plugin/spectra_presets.inc — the row format

Presets live in `src/plugin/spectra_presets.inc`, a textual include expanded
inside spectra.cpp's kSpPresets initializer (spectra.cpp defines and undefs
the macros around the include). The file contains **nothing but comments and
these three macros**, so a content author needs this document and no C++:

```
SP_PRESET("Init")
SP_END()

SP_PRESET("BA Neon Drop")
SP( 18,  320.f)   // Cutoff
SP( 19,  0.62f)   // Resonance
SP( 20,  3)       // Filter Type = LP24
SP( 98,  2)       // Voice Mode = Legato
SP_END()
```

- `SP_PRESET("<name>")` opens a preset; `SP(<id>, <value>)` is one override;
  `SP_END()` closes it.
- `<id>` is the raw id from the tables in this file (ids are the frozen
  interface, so raw ids are stable forever); the trailing `// <name>`
  comment is mandatory, and for enums it names the value
  (`// Filter Type = LP24`).
- Values are plain target units exactly as this file states them: float
  params take `f`-suffixed literals, int params take bare ints.
- List only non-default values; everything unlisted is the default. At most
  64 overrides per preset (the SpPreset set-array cap rises from 40 to 64).
