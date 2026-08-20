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

---

# v2 implementation notes (NOT contract — the contract above stays frozen)

Decisions the DSP took where the contract leaves latitude, recorded so the
editor and preset agents read the same behaviour the voices run. Where any of
these later needs to change, it changes here and in spectra.cpp together; the
tables above do not move.

- **Bit-identity discipline.** Everywhere v2 adds arithmetic to a v1 path,
  the v1 EXPRESSION is kept on its own branch, selected when the v2 feature
  is at its default (warp mode/amount 0, empty matrix per destination via a
  per-block destination bitmask, Noise Color exactly 1.0, Sub Shape 0 with
  `2^SubOct == 0.5f` exact). "Mathematically equal" is not "bit-identical";
  the gate is the second one.
- **Sub pitch base (id 42/43).** "Follows osc A's post-glide pitch" is
  implemented as the voice's post-glide, post-vibrato pitch — the same base
  the v1 sub tracked — NOT including A Coarse/Fine or matrix A-Pitch. It has
  to be: a v1 state with A Coarse nonzero must render bit-identically, and a
  sub that suddenly tracked Coarse would move. Sub Shape 1 (triangle) is the
  naive triangle (slope kinks fall at 1/h², inaudible at sub registers);
  shape 2 (square) carries polyBLEP edges as the row says.
- **Warp amount zero (id 49/51).** "At 0 every mode is bit-identical to Off"
  and Quantize's `N = 2 + round(62·(1-a))` cannot both hold at a = 0 (N = 64
  is not the identity). The id-49 sentence governs: depth 0 selects the Off
  path outright, for every mode; the N formula applies for a > 0. Bend's
  `p^e` is computed as `exp2(e·log2(p))` with the mip selector's fast log2
  (deterministic, ~1e-5), which is also why the a = 0 identity is enforced by
  selection rather than trusted to arithmetic.
- **FM/RM tap (warp modes 6/7).** The tap is the other osc's voice 0 read at
  its RAW phase accumulator — "pre-warp" means Sync/Bend/Mirror/Quantize's
  read-phase transform is not applied to the tap; under FM the accumulator
  itself is FM'd (there is no unwarped phase to read), and under RM the tap
  is the pre-crossfade read. Taps are read before any phase advances and
  consumed one sample later (the contract's delay). An osc at Level 0 under
  its own FM mode still advances its phases FM'd, because its tap may feed
  the other osc.
- **Matrix cadence.** Destinations 11 (Cutoff), 12 (Resonance) and 17..19
  (LFO rates) apply at the control tick (kCtrl = 16 samples, absolute-timed),
  exactly the cadence v1's LFO→cutoff already had — the filter walks its
  coefficients between ticks, so this is where those destinations physically
  live. Every other destination is per voice per sample. Cutoff maps through
  the norm clamp and then keeps the engine's fcMax guard (0.45·sr), which
  only bites below ~44.1 kHz.
- **LFO-rate destinations (17..19).** An LFO is instance-wide; per-voice
  sources feeding its rate read from the NEWEST active voice (age order), and
  from 0 with no active voice. When a slot drives a free LFO's rate, the rate
  knob's per-block write is skipped so the tick owns it (block-size
  invariance). Synced LFOs ignore rate slots, as the table says.
- **Random source (13).** splitmix64 finalisation of
  `absSample ^ (note << 48) ^ (channel << 56)`, top 24 bits mapped to
  [-1, 1). `absSample` is the note-on's absolute sample position: samples
  processed since prepare() plus the event's stamped in-block frame.
- **Aftertouch source (8).** MIDI channel pressure (0xD0), queued through
  the same event queue as notes and applied at its stamped sample;
  instance-wide; 0 after prepare(). Poly aftertouch (0xA0) is not mapped.
- **Mono/Legato (id 98).** The mono voice is voice slot 0. A held-note stack
  (64 deep, oldest dropped first) is maintained in every mode so a mid-phrase
  mode switch starts from the truth. Note-off fallback to the most recent
  held note is a GLIDE, not a retrigger — a fallback is not a note-on, and
  the contract only retriggers on note-ons. Mono retriggers ENV1-3 from
  their CURRENT values (a new attack, not a click) and keeps the sounding
  voice's phases; a retrigger from silence starts exactly like a Poly note.
  Legato-overlap note-ons update the voice's note (KeyTrk source follows
  immediately, the pitch glides) and its per-note Random, and keep velocity.
  Poly voices still sounding when the mode switches honour their note-offs.
- **Pan destination (16).** Equal-power: `θ = (pan+1)·π/4`,
  gains `√2·cos θ / √2·sin θ` — the unison fan's own law, applied to the
  voice's summed output before Master. The centre gain is therefore ~1.0 but
  not bitwise 1.0, which is fine: the v1 path is only left when a slot
  actually targets Pan.
- **24 dB filter modes.** Two identical TPT SVF stages sharing coefficients;
  the second stage's state is reset at note-on and not ticked in the 12 dB
  modes. Switching type mid-note reuses whatever the second stage last held —
  a one-transient concession the 12 dB modes never see.
- **Reserved ids** are registered exactly as the rule says (name `—`, 0..1,
  default 0) and are never read by the DSP.
- **Measured warp alias energy** (C6, non-harmonic/harmonic, 16384-pt Hann,
  48 kHz — the suite's regression gates sit ~4 dB above these): Sync a=0.5
  −35.5 dB · Bend+ a=1 −14.2 dB · Bend− a=1 −32.0 dB · Mirror a=1 −20.1 dB ·
  Quantize a=0.5 −19.6 dB · FM a=0.5 (equal pitch) −26.8 dB non-harmonic.
  Bend+ is the true worst case: p^(1/(1+3a)) has an unbounded phase slope at
  p=0, so full depth at high pitch genuinely exceeds the (contract-mandated)
  unwarped-fundamental mip — the "mild aliasing, accepted" the contract
  states. The v1 gate (every table at C7 under −60 dB with warps Off) is
  unchanged and still measured.

---

# v3 — hands on the modulation (FROZEN)

Spends three of v2's eleven reserved ids, appends ids 100..110, widens three
enums, and gives Spectra the state string it has never had. Everything above
this line is untouched: the 42 v1 ids and the 58 v2 ids keep their meaning,
their ranges, their defaults and their fixed routings, and the v2 rules
(blocks with reserved tails, the Curve column, the state/versioning rule)
govern this revision as written.

**Gate, restated for v3.** A v1 or v2 state loaded into a v3 build, fed a MIDI
stream a v1/v2 build could act on — note on/off, channel pressure, CC 120 and
CC 123 — renders bit-identical. Every v3 default is "do what v2 did", with
exactly ONE exception, and it is named here rather than left to be discovered:
**Bend Range (id 99) defaults to 2 semitones, not 0.** Pitch bend is the only
hardwired new MIDI action in v3; every other new MIDI source reaches the audio
through a matrix slot, and matrix slots default to Off, so they are inert by
construction. A build that ignored the pitch wheel is a broken instrument, and
this is the revision that stops ignoring it; the gate above is written to say
precisely what the exception costs — bend bytes, which older builds discarded
unread, now move pitch. Nothing else in v3 changes what an existing set sounds
like.

**On spending reserved ids.** The v2 rule says a reserved id may be given real
meaning only if default 0 means "no effect". v3 adds a second condition, and
it is the one that decided most of the map below: **an id ARRAY must be
contiguous.** The matrix's eight per-slot curves are addressed as `101 + k` for
slot k the way its triples are addressed as `68 + 3k`; two reserved ids cannot
hold eight, and scattering an array across a lookup table to save two indices
would buy nothing and cost every reader. So ids 92 and 93 stay reserved and the
curves append. Where a reserved id fits a scalar or a pair in its own block, it
is spent.

## Widened enums (append-only: every old value keeps its number and its meaning)

| id(s) | was | becomes | notes |
|-------|-----|---------|-------|
| 0, 8 | A/B Table 0..7 int | 0..8 int | 0..7 are the eight procedural factory tables, frozen. **8 is this oscillator's imported custom table**, named by the `wtA`/`wtB` records of the state string. See "Custom wavetable slots" below for the numbering and for what an unresolvable slot 8 does. |
| 37, 56, 59 | LFO Shape 0..4 int | 0..5 int | 5 = **Custom**, the drawable 16-step grid. The grid itself is state, not a param. Shapes 0..4 are untouched. |
| 68+3k (M1..M8 Src) | 0..13 int | 0..16 int | 14 **Mod Wheel** [0..1] · 15 **Pitch Bend** [-1..+0.999878] · 16 **MIDI CC** [0..1], the one learned slot. Semantics below. |

The destination enum (ids 69+3k, 0..19) does **not** widen in v3. Nothing v3
adds is a modulatable target: a drawn grid is a shape, a bend range is a
performance calibration, and a curve is the slot's own response.

Source LFO1/2/3 (values 1..3) now have a shape-dependent domain: **[-1..1] for
shapes 0..4, [0..1] for shape 5 (Custom)**. That is stated once here and
repeated in the Custom-shape section with the reason.

## Block: LFO mode & pitch bend — reserved ids 60, 61, 99 and new id 100

Reserved ids 60 and 61 sit inside the "LFO2 & LFO3" block and each of them
goes to the LFO that block owns. LFO1's parameters have lived outside that
block since v1 (ids 32..37, frozen and full), so **L1 Mode is not contiguous
with L2/L3 Mode and cannot be**. That costs nothing an implementation was not
already paying: per-LFO ids have never had a uniform stride (rate/sync/shape
are 32/33/37, 54/55/56, 57/58/59), so a per-LFO id table already exists and
gains one column.

| id | name | range | default | unit | curve | meaning |
|----|------|-------|---------|------|-------|---------|
| 60 | L2 Mode | 0..1 int | 0 | | lin | LFO2's mode; enum below. Was reserved in v2. |
| 61 | L3 Mode | 0..1 int | 0 | | lin | LFO3's mode. Was reserved in v2. |
| 99 | Bend Range | 0..24 int | **2** | st | lin | Pitch-wheel depth. A wheel at full travel offsets every sounding voice by ±this many semitones, added to the voice's **post-glide** pitch alongside vibrato (id 36) and therefore BEFORE per-oscillator Coarse/Fine — so the sub (id 17) follows the wheel, exactly as it follows vibrato. Summed with the matrix A/B Pitch destinations (7, 8) and clamped with them at ±48 st total. **0 is inert and bit-identical to v2**; the default is 2 and is the revision's one stated exception to "every default does what v2 did". Was reserved in v2. |
| 100 | L1 Mode | 0..1 int | 0 | | lin | LFO1's mode. First id of the v3 append. |

**The mode enum** (values 0..1; a later revision may append, e.g. a looping
per-voice "Trigger", under the append-only rule):

- **0 Loop** — v2 behaviour, verbatim and bit-identical. The LFO is ONE
  instance-wide generator. Free (Sync 0) it accumulates at its Rate from
  prepare(); synced it is a function of the transport beat. A note-on does not
  touch it. The v2 note about LFO-rate destinations (17..19) reading from the
  newest active voice applies in this mode and only in this mode.
- **1 One-shot** — the LFO becomes **per voice** and runs exactly once. Each
  voice owns a phase for it; the phase is set to 0 at the voice's note-on, on
  the note-on's stamped sample, and advances until it reaches 1.0, where it
  **clamps** and the LFO holds the shape evaluated at phase 1.0 for as long as
  the voice lives. This is what makes any LFO an envelope.
  - The Sync param (33/55/58) still selects the SPEED in one-shot — the
    increment is `Rate` Hz when free and `tempo / (60 · beatsPerCycle)` Hz when
    synced — but it does **not** select the alignment. A one-shot's origin is
    its note, not the bar line; a one-shot locked to the grid would not be an
    envelope.
  - Every routing that consumes a one-shot LFO becomes per voice, including
    the fixed v1 routings (34, 35, 36). Where the consumer is a control-tick
    destination (11, 12, 17..19 — v2's cadence rule) the per-voice value is
    sampled at the tick, exactly as Cutoff already is.
  - LFO-rate destinations (17..19) targeting a one-shot LFO are evaluated per
    voice from that voice's own sources. The newest-voice rule does not apply;
    it exists only because a Loop LFO has no voice to belong to.
  - Shape 4 (S&H) draws at each phase wrap. A one-shot wraps exactly once, at
    phase 0, so it draws one value at note-on and holds it. The draw uses the
    voice's stable identity hash — the same `(channel, note, stamped absolute
    sample)` splitmix64 that matrix source 13 uses, salted by the LFO number —
    so it is a deterministic function of the note and nothing else.
  - **A one-shot LFO retriggers exactly when ENV1 retriggers**, and never
    otherwise. In Mono (Voice Mode 1) every note-on retriggers it; in Legato
    (2) an overlapped note-on does not, and the note-off fallback to a held
    note does not, because a fallback is not a note-on. That is the same
    sentence v2 wrote about envelopes, and one-shot LFOs are envelopes.

## Block: matrix response curves — ids 101..108

One curve per matrix slot. **Slot k (0-based) is id `101 + k`**; M1 Curve is
101 and M8 Curve is 108. The v2 triple (68+3k / 69+3k / 70+3k) is unmoved.

| id | name | range | default | unit | curve | meaning |
|----|------|-------|---------|------|-------|---------|
| 101 | M1 Curve | 0..2 int | 0 | | lin | slot 1's response; enum below. |
| 102..108 | M2..M8 Curve | 0..2 int | 0 | | lin | same, seven more times (M8 Curve = 108). |

The curve reshapes the SOURCE value before the slot's Amt multiplies it, and
it is applied at whatever cadence the slot's destination already runs at (per
voice per sample, or the control tick for destinations 11, 12 and 17..19).
Polarity stays where v2 put it, in the sign of Amt.

With `u` the source's value and `f` the curve function:

- a source whose domain is [0..1] contributes `amt · f(u)`;
- a source whose domain is [-1..1] contributes `amt · sign(u) · f(|u|)` — the
  curve is applied symmetrically about zero, so a bipolar source stays bipolar
  and zero stays zero.

`f`, exactly, for all three values:

- **0 Linear** — `f(x) = x`. The default, and the branch is SELECTED and not
  computed: a slot at Linear runs the v2 expression untouched, which is what
  makes the bit-identity gate hold.
- **1 Exp** — `f(x) = x·x`. "Exp" is the UI's word for "slow at the bottom,
  quick at the top"; the formula is the contract, and it is a plain multiply
  rather than a transcendental so that it is exact on every machine.
- **2 S-curve** — `f(x) = x·x·(3 − 2x)`, the smoothstep. Exact at 0 and at 1,
  symmetric about 0.5.

All three satisfy `f(0) = 0` and `f(1) = 1`: a curve can never make an idle
source contribute, and can never change a full-scale source's reach.

## Block: v3 reserved tail — ids 109..110

| id | name | range | default | unit | curve | meaning |
|----|------|-------|---------|------|-------|---------|
| 109 | — | | | | | reserved |
| 110 | — | | | | | reserved |

Registered exactly as the v2 rule says (name `—`, 0..1, default 0, hidden,
never read).

## v3 id map

| block | ids | functional | note |
|-------|-----|-----------|------|
| v1 (frozen) | 0..41 | 42 | |
| v2 blocks (frozen) | 42..99 | 47 | three reserved ids spent below |
| — reserved spent | 60, 61, 99 | +3 | L2 Mode, L3 Mode, Bend Range |
| LFO1 mode | 100 | 1 | |
| matrix curves | 101..108 | 8 | slot k = 101 + k |
| v3 reserved tail | 109..110 | 0 | |

**Reserved ids spent by v3: 60, 61, 99.**
**Reserved ids that remain: 46, 47 (sub & noise), 52, 53 (warp), 66, 67
(ENV3), 92, 93 (matrix) — eight.** 92 and 93 were the obvious home for the
per-slot curves and could not hold them; see "On spending reserved ids".

**111 ids total (0..110): 101 functional, 10 reserved.** `kSpParamCount` goes
100 → 111.

**Budget: 111 ≤ kMaxParams = 128 (internal_base.h). v3 fits with 17 ids of
headroom and the cap is NOT raised.** The 64→128 precedent stands unused here;
if a later revision needs it, the raise is one line in internal_base.h plus
its filed justification, exactly as v2 did it.

## The Custom LFO shape (value 5)

A 16-step grid with one level per step and one smoothing amount, per LFO. Both
live in the state string, not in parameters — that is the plan's choice and it
is what keeps three drawable LFOs from costing 51 ids. The cost is stated
plainly so nobody has to find it: **a drawn grid and its smooth cannot be
automated and cannot be a matrix destination.** They are shape, like a table
is shape.

- **Step index.** `i = clamp(floor(p · 16), 0, 15)` where `p ∈ [0,1)` is the
  LFO's phase — the SAME phase the other five shapes read, so which cycle the
  LFO is in and how it got there is unchanged by this shape existing.
- **The division is the length of the WHOLE cycle, not of one step.** Sync 5
  (1/4) runs all sixteen steps inside one quarter note. An author who wants
  1/16 steps picks sync 3 (one bar): four beats over sixteen steps.
- **Output domain is [0..1], unipolar** — the one place a Custom LFO differs
  from shapes 0..4. This is deliberate and it is forced: sixteen levels cannot
  be symmetric about an exact zero (a symmetric set containing 0 has odd
  cardinality), and this document's spine is that a default of zero means no
  effect. An all-zero grid must be silence, so level 0 is 0.0 and the grid
  climbs from there. A bipolar reading is one negative Amt away, and the fixed
  routings (34, 35, 36) receive a one-sided offset rather than a wobble —
  which is what a step sequence wants anyway.
- **Level quantization.** Step level `L(d) = d / 15` for the stored hex digit
  `d ∈ 0..15`. Digit 0 is 0.0 exactly, digit 15 is 1.0 exactly.
- **Smooth.** `s ∈ [0,1]`, stored as thousandths (below).
  - `s = 0` selects a no-filter branch outright: the output is the step level,
    a hard staircase, bit-exact. (Same discipline as Warp Amt 0.)
  - `s > 0` runs a one-pole lag toward the current step level,
    `y += (L(d_i) − y) · a` per sample with
    `a = 1 − exp(−1 / (s · T_step · sr))`, where `T_step` is the current step
    duration in seconds (the cycle period divided by 16). The coefficient is
    recomputed at the control tick (kCtrl = 16 samples, absolute-timed), the
    same cadence and for the same block-size-invariance reason v2's matrix
    destinations use. As `s → 0` the coefficient tends to 1 and the filter
    tends to the s = 0 branch, so the control is continuous across its own
    default.
  - The lag state is per voice in One-shot mode and instance-wide in Loop
    mode, and is initialised to `L(d_0)` at the phase origin — note-on for
    one-shot, prepare() for loop. It never carries across renders.
- Setting a shape param to 5 with no grid in the state is not malformed: the
  grid reads as its default, all zeros, and the LFO is silent. See the
  versioning rule.

## The new matrix sources (14, 15, 16)

All three are **instance-wide** and **omni** (any MIDI channel), like the
Aftertouch source (8) they follow in every respect, and all three are **0
after prepare()** — bend meaning centre.

| value | name | domain | wire |
|-------|------|--------|------|
| 14 | Mod Wheel | [0..1] | CC 1 (0xB0, controller 1); `w = data2 / 127`. |
| 15 | Pitch Bend | [-1..+0.999878] | 0xE0; `v14 = lsb \| (msb << 7)`, `b = (v14 − 8192) / 8192`. The asymmetry is MIDI's — 8192 is the centre of a 14-bit range whose top value is 16383 — and it is stated rather than hidden by rescaling, because rescaling would make the centre not exactly 0. |
| 16 | MIDI CC | [0..1] | the learned controller number carried in the state's `cc` record; `v = data2 / 127`. |

There is exactly **one** learn slot. Every matrix slot whose source is 16
reads the same controller; that is the plan's "one generic MIDI CC source" and
it means the learn is a property of the instrument, not of a slot. A later
revision may append sources 17, 18, … with their own state records; the state
format below is written so that costs one record each and no format change.

With no CC learned, source 16 is constant 0 — inert, like every other v3
default. Learning CC 1 is legal and redundant (source 14 already has it).

Pitch bend reaches the audio TWICE and both are live at once: hardwired
through Bend Range (id 99), and through any matrix slot that selects source
15. Setting Bend Range to 0 disables only the first.

CC 121 (reset all controllers) is not handled in v3; wheel, bend and the
learned CC hold their last value until the next message or the next prepare().
CC 120 and 123 keep the meanings v1 gave them and do not touch these three.

## Custom wavetable slots

`A Table` (id 0) and `B Table` (id 8) widen 0..7 → **0..8**.

- **0..7** — the eight procedural factory tables, frozen, unchanged.
- **8** — this oscillator's imported custom table, identified by this
  oscillator's `wtA` / `wtB` state record.

Custom space **begins** at 8 and grows upward: a later revision that wants a
shared import pool widens the range again (a widening is always a superset, so
every stored value keeps its meaning). v3 registers no dead slots — a knob the
user can turn to a value that refuses is worse than a knob that stops.

**What an out-of-range slot does.** The base clamps a stored value to the
registered range, so a value written by a later, wider build arrives as 8 on a
v3 build: the oscillator lands on the custom slot, the `wt` record for that
oscillator still names the right file, and the extra slots' records are simply
records this build does not read. That is the versioning rule doing its job,
not a special case.

**What an unresolvable slot 8 does — the refusal contract.** A table is
resolved by hash, in this order: the in-memory table cache; the installed
factory wavetable directory (`<factory wavetables>/<hash>.nxwt`); the user
cache (`$XDG_DATA_HOME/nxtakt/wavetables/<hash>.nxwt`, falling back to
`~/.local/share`); and finally a re-import from the `wtpath` record if one is
present, which on success writes the user cache and yields the same hash by
construction. If all of those fail:

- the **parameter keeps its value** — Table stays 8. The set's intent is not
  edited by the machine that could not honour it.
- the **state records are kept and re-emitted verbatim** by `stateString()`,
  so saving on a machine that is missing the file does not lose the file's
  name. This is the sampler's rule and the reason for it is the sampler's
  reason.
- the **oscillator renders factory table 0** for as long as the resolution
  fails. Silence would be a worse lie than the wrong table; an amber badge
  says which it is.
- the editor draws the **refused-device amber idiom** naming `wtpathA` /
  `wtpathB`, or the bare hash when no path record is present. One line, and
  the DSP logs once per instance (the sampler's `warnedMissing_` discipline —
  a set with thirty Spectras in it must not become a screen).
- `setStateString()` still returns **true**. A missing file is not a malformed
  state: the set is correct and the machine is incomplete. Only a record this
  writer could not have produced refuses, and a refusal changes nothing at all.

## The state string

v3 gives Spectra a state string. It did not have one; `stateString()` returned
`{}` and the project layer wrote no `state` key. **That must stay true for
every set that uses no v3 state**: `stateString()` returns the EMPTY string
when no grid is drawn, no CC is learned and no custom table is imported, so a
v2 project round-trips through a v3 build byte-identically.

```
nxspc1;<record>;<record>;...
```

One line of printable ASCII with no whitespace, no quotes and no newline —
the sampler's shape and the rack's shape, because this tree has one spelling
of "an opaque device state" and not three. Version-tagged first record;
`;`-separated records; each record is `<key>=<value>` with `key` matching
`[A-Za-z][A-Za-z0-9]*`. (It was written `[a-z][a-z0-9]*` here first, which
this document's own key table contradicts three lines later: `wtA`, `wtB`,
`wtpathA` and `wtpathB` carry an oscillator letter. The REGEX was the typo, not
the keys — the keys are already in shipped files.) A record whose key this build
does not know is **skipped**
(forward compatibility); a record with no `=`, or an empty record, is
**refused** — skipping those would make `nxspc1;;;;` a valid way of saying
nothing. A duplicate key is refused: choosing one of two is guessing.

Records are written in the order below and read in any order.

| key | value | meaning |
|-----|-------|---------|
| `lfo1` `lfo2` `lfo3` | exactly 16 characters from `[0-9a-f]` | LFO n's drawn grid. Leftmost digit is **step 0**, the step in effect at phase 0; rightmost is step 15. Lowercase only, on write and on read — strict, so that write and read are exact inverses and a round trip never normalises anything. Digit `d` is level `d/15`. |
| `smooth1` `smooth2` `smooth3` | 1..4 decimal digits, value 0..1000, no leading zeros (`0` itself excepted) | LFO n's smooth, `s = value / 1000`. An INTEGER and not a float: a decimal point is a locale hazard (a `de_DE` writer emits `0,5`) and a state string that depends on the writer's locale is not a state string. 1/1000 is far finer than the control. |
| `cc` | 1..3 decimal digits, value 0..127, no leading zeros | the learned controller number for matrix source 16. |
| `wtA` `wtB` | exactly 16 characters from `[0-9a-f]` | the content hash of that oscillator's custom table. Identity. |
| `wtpathA` `wtpathB` | escaped path, ≤ 4096 DECODED bytes | recovery hint only — where the table was imported from. Never identity. |

**A missing block reads as its default**, and every default is inert: no grid
(all sixteen levels 0), smooth 0, no CC learned (source 16 reads 0), no custom
table (slot 8 unresolvable → the refusal contract above). **Every state a v3
build writes carries every block it uses**, so a state is complete however
short it is — the same property the preset rule has had since v1.

### Path escaping

**Verbatim the sampler's**, and the implementation is expected to share the
sampler's helpers rather than write a second escaper: escape any byte with
`c <= ' ' || c >= 0x7F || c == '%' || c == ';' || c == ',' || c == ':' ||
c == '='` as `%` followed by two **uppercase** hex digits; leave every other
byte raw. Unescaping is **STRICT** and refuses, in order: a `%` not followed by
two hex digits; an escape decoding to NUL; a raw byte the writer would have
escaped; and anything over 4096 decoded bytes. NUL and only NUL among the byte
values, for the reason sampler.cpp states at length: a path that can be written
must be a path that can be read back, or the set loses the file it names.

### The content hash

The identity of a custom table is a splitmix64 fold over the **resampled f32
frames** — the samples as they stand after slicing, resampling to 2048, DC
removal and set normalisation, and BEFORE any mip is built. Mips are derived
data and hashing them would make the identity depend on the mip builder.

```
mix64(x):  x += 0x9E3779B97F4A7C15
           x ^= x >> 30;  x *= 0xBF58476D1CE4E5B9
           x ^= x >> 27;  x *= 0x94D049BB133111EB
           x ^= x >> 31;  return x

h = mix64(frameCount)
h = mix64(h ^ 2048)                       // the cycle length, a constant today
for i in 0 .. frameCount*2048 - 1:        // FRAME-MAJOR: frame 0 samples
    u = float32_bits(frames[i])           // 0..2047, then frame 1, ...
    if u == 0x80000000: u = 0             // -0.0 folds to +0.0
    h = mix64(h ^ (u64)u)
```

The finaliser is the one spectra.cpp already uses for the Random source; there
is no second hash in this device.

**Byte order: there is none to get wrong.** The fold consumes the 32-bit IEEE-754
`binary32` VALUE of each sample, not its bytes, so the hash is identical on a
big-endian machine. (The `.nxwt` cache FILE, which is not this hash's business,
stores its frames little-endian.) `-0.0` is folded to `+0.0` because DC removal
can produce it and two tables that differ only in the sign of a zero are the
same table. A frame containing a non-finite sample is **refused at import** and
never reaches the hash, so NaN payloads cannot become identity.

Rendered as **16 lowercase hex digits, zero-padded**.

## src/plugin/spectra_presets.inc — the row format, v3

The v1/v2 macros are unchanged: `SP_PRESET("<name>")` opens, `SP(<id>,
<value>)` is one parameter override with its mandatory `// <name>` comment,
`SP_END()` closes, at most 64 `SP` rows. v3 adds three macros so a preset row
can carry state as well as parameters, and a preset author still needs this
document and no C++.

```
SP_PRESET("SQ Ladder Walk")
SP( 37,  5)          // LFO Shape = Custom
SP( 33,  3)          // LFO Sync = 1 bar (16 steps over four beats)
SP( 35,  0.55f)      // LFO>Cutoff
SPLFO(1, "08c4f6a20d9315be", 0.25f)
SP_END()

SP_PRESET("PD Glass Import")
SP(  0,  8)          // A Table = custom slot 8
SPWTA("3f9c1a0b7d24e685")
SP_END()
```

| macro | signature | rules |
|-------|-----------|-------|
| `SPLFO` | `SPLFO(<n>, "<16 lowercase hex>", <smooth>)` | `n` is the LFO number, **1..3**. The grid string is exactly 16 characters from `[0-9a-f]`. `smooth` is an `f`-suffixed float literal in 0..1; it is quantized to thousandths when the row is written out as a state string. At most one `SPLFO` per LFO number per preset. |
| `SPWTA` / `SPWTB` | `SPWTA("<16 lowercase hex>")` | the content hash of a custom table for oscillator A / B. **Hash only — a factory preset may not name a path**, which is the sampler's rule and the same rule: a preset cannot name a file the user does not have. The hash must resolve in the installed factory wavetable directory, so a preset using this macro ships a `.nxwt` beside it. At most one of each per preset. Separate macros rather than `SPWT(osc, …)` because the oscillators are named A and B everywhere else in this file and a 0/1 argument beside `SPLFO`'s 1-based `n` is a trap. |
| `SPCC` | `SPCC(<n>)` | the learned controller number, `0..127`, for matrix source 16. At most one per preset. |

**Placement.** All three may appear anywhere between `SP_PRESET` and
`SP_END`, in any order relative to the `SP` rows; the convention this bank
follows is parameters first, state after. They do **not** count against the
64-override cap — that cap is on `SP` rows, which are the array the cap sizes.

**A state macro never sets a parameter, and this is the trap to know.**
`SPLFO(1, …)` stores LFO1's grid; it does **not** set LFO Shape to Custom.
The row must say `SP(37, 5)` itself. Likewise `SPWTA` does not set `A Table`
to 8. The rule that "a row lists every parameter override it makes" is worth
more than the two lines it costs, and the bank's range checker enforces the
pairing: a grid with a non-Custom shape, or a `wt` hash with a Table that is
not 8, is a checker failure and not a silent no-op.

**loadPreset resets state too.** A preset is COMPLETE however short it is
written — v1's rule — and v3 extends it from parameters to state: `loadPreset`
resets every id to its default AND every state block to its default, then
applies the row's overrides and the row's state macros. So switching from a
patch with a drawn grid and an imported table to a preset that mentions
neither lands on exactly the state a fresh instance would have. This is
deliberately UNLIKE the sampler, which keeps its file across a preset change:
the sampler's file is the material the user brought, while a wavetable and a
drawn grid are sound design, and sound design is what a preset replaces.

---

## The `.nxwt` cache file, cited rather than re-specified

A resolved custom table is cached — and a *factory* table is shipped — as a
`.nxwt` file. The format is defined in `src/plugin/wavetable_io.h`; it is
repeated here so a preset author or a packager never has to open a header to
know what they are handling:

```
char magic[4]   "NXWT"
u32  version    kNxwtVersion (1)
u32  frames     1..kMaxFrames
u32  cycle      kCycle (2048)
u64  hash       the content hash, which readNxwt RECOMPUTES and compares
```

…followed by `frames × cycle` little-endian `f32`. **Mips are not in the file
and never will be**: they are derived and they rebuild at load, which is also
why this is a cache and not an interchange format. The stored hash is a check,
not the authority — a file whose bytes do not fold to its own name is refused,
so a corrupt or hand-edited entry can never play as the table its name claims.

Factory tables live beside the binary (`<exeDir>/wavetables`, with
`$NXTAKT_WAVETABLES` and `<exeDir>/../share/nxtakt/wavetables` the other rungs
of `wt::factoryDir()`'s ladder); `make` copies them there and `make dist` ships
them there. A preset naming a factory table is unplayable without its file,
which makes those tables a build output rather than an asset lying beside one.

## What the matrix does NOT do

It **sums**; it never scales. Every slot adds its contribution to a
destination's base value — no slot multiplies, attenuates or otherwise modifies
another slot's routing. So a patch idea of the form "the mod wheel deepens the
vibrato" is not expressible: the wheel cannot scale an LFO's existing depth.
What it CAN do is raise a floor, add a second source to the same destination,
or drive that LFO's rate. This is written down because it is the first thing a
preset author tries and the second thing they discover — and because a
VCA-style scaling matrix would be a different feature with a different cost,
not an oversight in this one.

# The user-preset contract (host.h) — CONTRACT, not a Spectra feature

This lands on `PluginInstance`, not on Spectra. Every stateString-bearing
device inherits it — the sampler gets a user bank for free, and so does the
next instrument — and the base implementation is generic: it writes every
parameter and the device's own state string, and it knows nothing about any
particular device.

## The API addition

```cpp
// GUI thread. Writes the device's CURRENT parameters and stateString() to
// the user preset directory under `name`. Returns false and changes NOTHING
// on refusal. The default is false: "this device does not save presets".
virtual bool savePreset(const char* name) { (void)name; return false; }

// GUI thread. presetCount() enumerates factory presets THEN user presets.
// This is the boundary: indices [0, factoryPresetCount()) are factory,
// [factoryPresetCount(), presetCount()) are user. The default answers
// presetCount(), so every existing backend — which has no user bank — is
// already correct without being touched.
virtual int factoryPresetCount() const { return presetCount(); }
```

Two methods and no more, in the spirit of the preset trio above them
("deliberately the smallest thing that can work"). A per-preset flag or a
richer descriptor would let the UI ask better questions and would also be a
format the next device has to implement; a boundary index answers the only
question the popover actually asks, which is where to draw the "User" header.

`loadPreset(i)` is unchanged in signature and gains a leg: for
`i >= factoryPresetCount()` it reads the file, resets every parameter to its
default, applies the file's parameters, and only then calls
`setStateString()` — **parameters first, then state**, which is host.h's own
load ordering and the same trap docs/RACKS.md documents for racks. A file that
does not parse is refused whole: nothing is applied and the device is left
exactly as it was.

**Two contract weakenings, stated because they are easy to miss:**

1. `presetName(int)`'s pointer was documented as valid "for the instance's
   lifetime". It is now valid **until the next successful `savePreset()` on
   that instance**, which is the only call that can change the list. Every
   existing implementation returns a string literal and is unaffected.
2. The user list is scanned at construction and re-scanned by a successful
   `savePreset()`. A file added by another process, or by the user's file
   manager, appears when the device is next constructed. There is no watcher
   and no `rescan()` on the contract; a preset list that changes under a live
   popover is a bug source, and the cost of the honest version is reopening
   the device.

**Deleting a user preset is NOT on the v3 contract.** They are files; the
file manager is the delete UI. A later revision may add `deletePreset(int)`
under the same append-only discipline.

## Directory layout

```
$XDG_CONFIG_HOME/nxtakt/presets/<uri-slug>/<name-slug>.nxp
```

`XDG_CONFIG_HOME`, else `$HOME/.config`, else the passwd entry's home
`/.config`, else `/tmp` — the same ladder `defaultMapPath()` walks in
src/control/learn.cpp, because there is one answer to "where does nxtakt keep
a user file" in this tree.

- **`<uri-slug>`** — the device's URI with every byte outside
  `[A-Za-z0-9._-]` replaced by `-`. `nxtakt:spectra` → `nxtakt-spectra`;
  `nxtakt:sampler` → `nxtakt-sampler`.
- **`<name-slug>`** — the display name with every byte outside
  `[A-Za-z0-9 ._-]` replaced by `_`, leading and trailing spaces and dots
  trimmed, capped at 64 bytes, and `preset` if nothing survives. The slug is a
  filename and nothing else; **the display name is the file's `name` header**,
  which is authoritative. Renaming a file on disk does not rename a preset.
- **Collision.** If `<name-slug>.nxp` exists and its `name` header is a
  DIFFERENT display name, try `<name-slug>-2.nxp`, `-3`, … up to `-99`, then
  refuse. If its `name` header is the same display name, that is an overwrite
  — see below.
- **Scan.** Non-recursive, files whose whole tail is `.nxp` only, so
  `.nxp.bak` is never enumerated. A file that does not parse is skipped with
  one log line and does not prevent the others from loading; a corrupt file in
  a directory must not cost the user the bank.
- The wavetable cache is elsewhere and is data, not config:
  `$XDG_DATA_HOME/nxtakt/wavetables/<hash>.nxwt`, falling back to
  `~/.local/share`.

## The `.nxp` file format

Line-oriented UTF-8 text, LF, a trailing `\r` stripped from every line so a
file that visited Windows still loads. First line is the version tag alone.
Every later line is `<key><space><value>`, where the value is the rest of the
line — so a display name may contain spaces. Unknown keys are skipped
(forward compatibility). Cap: 256 KiB; a preset larger than that is corruption.

```
nxp1
uri nxtakt:spectra
name Ladder Walk
category User
param 0 8
param 1 0.35
param 18 320
...
state nxspc1;lfo1=08c4f6a20d9315be;smooth1=250
```

| key | cardinality | value |
|-----|-------------|-------|
| `uri` | exactly 1 | the device URI. **Must equal the loading device's URI or the file is refused** — a Spectra preset must not half-load into a sampler. |
| `name` | exactly 1 | the display name. 1..64 bytes, no control bytes. Authoritative over the filename. |
| `category` | 0 or 1 | free text; the base implementation writes `User`. Carried and round-tripped, **not yet consumed**: v3's popover draws every user preset under one "User" header. The field exists so grouping is a later behaviour change and not a later format change. |
| `param` | 0..n | `<id> <value>`. `id` is a decimal index; `value` is the plain target-unit value in project.cpp's own spelling — `fmtF32`, the shortest decimal that reads back bit-identical. Same writer, same reader, one spelling of "a saved parameter" in the tree. Ids the running build does not know are ignored; ids the file omits read as their default, which is the versioning rule again. |
| `state` | 0 or 1 | the device's state string, verbatim. It is already printable ASCII with no whitespace, so it needs no escaping and gets none. Absent means the empty state. |

The base implementation writes **every** parameter id, not just the
non-defaults. A hundred and eleven short lines is not a cost worth optimising,
and a file that lists everything is a file a user can read and edit.

**Refusal is all-or-nothing**, exactly like `setStateString()`: a bad version
tag, a `uri` mismatch, a missing or duplicated `name`, a malformed `param`, a
`state` the device refuses, or an oversized file means the file is not applied
at all and the device is left as it was, with one amber line saying so.

## Saving

`savePreset(name)` refuses — returns false, writes nothing — on: a null or
empty name; a name over 64 bytes; a name containing a control byte; a
directory that cannot be created; any I/O failure. Otherwise:

1. `mkdir -p` the `<uri-slug>` directory at 0755 (learn.cpp's `ensureParentDir`).
2. Resolve `<name-slug>` and the collision rule above.
3. **If the target exists and carries the same display name, rename it to
   `<name-slug>.nxp.bak` first**, replacing any existing `.bak`. Exactly one
   generation is kept. Presets are files, not session state, so an overwrite
   does not enter undo; the editor announces it in the status bar and the
   `.bak` is what "undo" means here.
4. Write to a temporary file in the same directory and `rename()` over the
   target — atomic, learn.cpp's `writeAtomic` discipline, so a crash mid-save
   cannot leave a half-written preset where a whole one was.
5. Re-scan the user directory before returning, so `presetCount()` and
   `presetName()` already include the new preset when the call returns true —
   and every pointer previously returned by `presetName()` is now stale.

## Ordering

`presetCount()` = factory count + user count. Factory presets keep their bank
order, frozen (the file order the v2 contract fixes: Init, then category
order, alphabetical within a category). User presets follow, sorted by display
name **byte-wise ascending** — `memcmp`, not locale collation — with the
filename as the tiebreak. A directory listing has no order, and a locale
collation has a different one on every machine; the bank must look the same on
the user's laptop and in a test running under `de_DE`.

# Determinism obligations (v3)

Every one of these is a gate, not an aspiration. The rule they all serve is
the one v1 wrote and v2 restated: the same input renders the same audio, in
blocks of 1 and of 1024, in the daemon and in process.

1. **The step-LFO index derives from the transport beat (synced) or from
   accumulated phase (free), and NEVER from wall time.**

   *Corrected during the wave, and the correction matters.* The sentence here
   first said a Loop LFO's synced phase "is what v2 made it: `frac(beat /
   beatsPerCycle)`". That is not what v2 made it and never was: spectra.cpp
   RATE-syncs — the accumulator advances at a beat-derived rate and is not
   phase-locked to the transport — as its own header has said since v1.
   Implementing the sentence literally would have moved every existing synced
   LFO and broken the bit-identity gate, which outranks it. So:

   * shapes 0..4 in Loop mode keep the shipped rate-sync behaviour, bit-identical;
   * the NEW Custom shape derives its STEP INDEX from the transport beat when
     synced — which is what makes a step sequencer lock to the grid, and carries
     no bit-identity debt because the shape did not exist to be identical to —
     falling back to accumulated phase when no transport is running, because a
     bank authored at a fixed tempo with the transport stopped must still play.

   Either way the clock is the engine's, never the wall's.
2. **A One-shot LFO's phase resets at the note-on's stamped sample**, inside
   the per-sample event loop, not at the top of the block — the same rule and
   the same reason note-ons themselves have it. Its phase then accumulates in
   the voice, so it is a function of samples elapsed since a stamped event and
   of nothing else. A synced one-shot takes its increment from the transport
   tempo and its ORIGIN from the note; the tempo is already deterministic, so
   the pair is.
3. **The smooth filter's coefficient is a function of the step duration and
   the sample rate**, recomputed at the absolute-timed control tick. It never
   reads elapsed real time, and its state is initialised at the phase origin,
   so it cannot carry a value from one render into the next.
4. **MIDI CC, pitch bend and mod wheel are QUEUED and applied at their stamped
   sample**, through the same event queue as notes and channel pressure, and
   for the identical reason: applying them when `midi()` is called would make
   the same MIDI in blocks of 1 and of 1024 produce different audio. They also
   participate in the queue's overflow rule (anything the queue could not hold
   lands at the block's last sample, after everything else).
5. **A one-shot LFO's S&H draw is the note's stable identity hash**, salted by
   the LFO number — `(channel, note, the note-on's stamped absolute sample)`,
   splitmix64-finalised, exactly source 13's construction. No RNG state is
   carried between notes and voice stealing cannot perturb it.
6. **The matrix curves are exact arithmetic.** `x·x` and `x·x·(3−2x)` are
   multiplies; there is no `pow`, no `exp`, no table, and therefore no
   libm-version dependence in a modulation path. Linear is a SELECTED branch,
   not a multiply by 1.
7. **Custom wavetable mips are a deterministic function of the imported
   frames**, and the frames that cross the wire are the ones that were hashed.
   The daemon and the in-process engine build the mips with the SAME factored
   builder the eight factory tables use, from identical f32 input, so a render
   of a custom-table patch is `cmp`-identical between them. This is the parity
   leg the plan asks for and there is no reason for it to be anything less than
   bit-exact.
8. **The content hash is endian- and locale-independent by construction** — it
   folds float VALUES, and every number in a state string is either hex or a
   plain integer. No state Spectra writes goes through a locale-sensitive
   formatter, which is why smooth is thousandths and not `0.5`.
9. **Pitch bend is the only new path that can change an existing set's
   render**, and only when the stream carries bend. Everything else v3 adds is
   selected by a parameter whose default is the v2 behaviour, or by a state
   block whose absence is the v2 behaviour.

# What v3 does NOT change

Stated so nobody rediscovers it: the destination enum (0..19) does not widen.
The eight factory tables, their generation and their mip policy do not change.
The sync division table does not change. The v1 fixed routings stay live and
still sum with the matrix. The 64-override cap on a preset row stays 64.
`kMaxParams` stays 128. The matrix stays eight slots — a ninth slot is ids and
a new block, and v3 spends its ids on the curves the eight already have.
