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

---

# v4 — the arpeggiator (FROZEN)

Spends the last two of v3's reserved ids, appends ids 111..124, widens one
enum, and adds two records to the state string. Everything above this line is
untouched: the 42 v1 ids, the 58 v2 ids and the 11 v3 ids keep their meaning,
their ranges, their defaults and their fixed routings, and the v2 rules (blocks
with reserved tails, the Curve column, the state/versioning rule) plus v3's
addition (an id ARRAY must be contiguous) govern this revision as written.

**Gate, restated for v4, and it is stronger than v3's.** v3 had one stated
exception — Bend Range's default of 2 — because pitch bend is a hardwired MIDI
action. **v4 has no exception at all.** Every v4 parameter is reachable only
behind Arp On (id 109), Arp On defaults to 0, and 0 selects the v3 path
outright rather than computing something that happens to equal it. So: a v1,
v2 or v3 state loaded into a v4 build, fed any MIDI stream a v3 build could act
on, renders **bit-identical**. That is the release gate and it is named as one
in the determinism section below.

**What this feature is.** A STEP-SEQUENCER arpeggiator, not a plain one. v3
already shipped 16-step drawable grids: the hex-grid state format, the `d/15`
level mapping, the `.inc` state-macro discipline, the editor's drawable-row
vocabulary and — the expensive part — a **beat-anchored step clock that is
block-size invariant**. All five are reused verbatim here. A plain up/down arp
would have been a smaller feature standing on none of it.

## On spending reserved ids (v4's answer)

Ten reserved ids stood before v4: 46, 47 (sub & noise), 52, 53 (warp), 66, 67
(ENV3), 92, 93 (matrix) and 109, 110 (the v3 tail). **v4 spends 109 and 110 and
leaves the other eight exactly where they are.** The reason is one sentence
long and it follows from a rule v3 already wrote:

- **A reserved id belongs to the block it sits in.** v3 spent 60 and 61 on
  L2 Mode and L3 Mode — the LFOs the block owns — and spent 99 on Bend Range,
  which sits in the voice block and is a voice-wide performance control. It did
  not scatter the matrix curves into the warp block's spare pair to save two
  indices. 46/47 are the sub and noise block's; 52/53 are warp's; 66/67 are
  ENV3's; 92/93 are the matrix's. An arpeggiator is none of those things, and
  an Arp Gate wedged between B Warp Amt and L2 Rate would cost every reader of
  this file forever to save one id once.
- **109 and 110 belong to nothing.** They are v3's generic tail — appended past
  the last feature block, owned by no feature — and they sit **immediately
  before 111**, which is where a v4 append starts. Spending them makes the
  arpeggiator one contiguous run, 109..124, with no hole and no lookup table.
  That is the same contiguity argument v3 used to refuse 92/93 for the curves,
  read in the other direction.

Both spent ids satisfy the v2 condition without strain: **Arp On defaults to 0
and 0 is off**, and **Arp Mode defaults to 0 and 0 is Up**, so a v3 `.nxp` file
— which writes every id including the reserved ones, all zero — lands on
exactly the v4 defaults.

**Reserved ids spent by v4: 109, 110.**
**Reserved ids that remain: 46, 47, 52, 53, 66, 67, 92, 93 — eight, unchanged.**

## Widened enum (append-only: every old value keeps its number and its meaning)

| id(s) | was | becomes | notes |
|-------|-----|---------|-------|
| 68+3k (M1..M8 Src) | 0..16 int | 0..17 int | 17 **Arp Step** [0..1], instance-wide: the arp's level row read at the current step. Semantics in "Arp Step as a modulation source" below. |

The destination enum (ids 69+3k, 0..19) does **not** widen in v4, for the third
revision running. Nothing v4 adds is a modulatable target, and the two
candidates are refusals rather than omissions: **Arp Rate is not a destination**
because a modulated rate cannot stay locked to a bar line and the whole feature
is built on being locked to one, and **Arp Gate is not a destination** because
a gate is consumed once per step at a stamped sample, not read at audio rate —
a destination that is sampled once per step is a different mechanism wearing
the matrix's clothes. Both stay parameters, and both are automatable, which is
the thing an author actually wanted.

## Block: the arpeggiator — reserved ids 109, 110 and new ids 111..124

| id | name | range | default | unit | curve | meaning |
|----|------|-------|---------|------|-------|---------|
| 109 | Arp On | 0..1 int | **0** | | lin | 0 = the arp does not exist: incoming notes reach the voices exactly as in v3, and every id below is read by nothing. 1 = incoming notes stop reaching the voices directly and the arp generates the notes instead. **This is the revision's bit-identity switch**; see the gate above and obligation 4 below. Was reserved in v3. |
| 110 | Arp Mode | 0..9 int | 0 | | lin | The note order. Enum below, **append-only forever after**. Was reserved in v3. |
| 111 | Arp Rate | 0.01..40 | 2 | Hz | log | Steps per second, used when Arp Sync = 0 and only then. Same range, default and curve as LFO Rate (id 32) — deliberately, so the editor reuses the readout. |
| 112 | Arp Sync | 0..9 int | **7** | | lin | The division table of id 33, **verbatim and cited, not restated**: 0 free · 1 four bars · 2 two bars · 3 one bar · 4 1/2 · 5 1/4 · 6 1/8 · 7 1/16 · 8 1/4T · 9 1/8T. It is an append-only list and it does not change in v4. **The division is the length of ONE STEP** — see "The step clock", where that divergence from the LFO grid's whole-cycle reading is stated and argued. At index 0 the arp is free-running: Arp Rate (111) gives steps per second, the transport is not read, and the arp keeps time from prepare(). Default 7 = 1/16, the only rate anyone reaches for first; it costs nothing because Arp On is 0. |
| 113 | Arp Octaves | 1..4 int | 1 | oct | lin | Length of the octave cycle. 1 = the arp stays in the played octave. |
| 114 | Arp Oct Mode | 0..2 int | 0 | | lin | 0 Up · 1 Down · 2 Alternate. Composition with the note cycle below — the note counter advances first. |
| 115 | Arp Gate | 1..200 | 50 | % | lin | Sounding length of a generated note as a percentage of the **nominal** step (never of the swung interval — see below). Over 100 % the note is still sounding when the next step starts; what that does to the voice allocator is stated below and it is not left to the allocator to decide. |
| 116 | Arp Swing | 0..100 | 0 | % | lin | Delays odd-numbered steps. The exact expression, in beats, is below. 0 selects a no-offset branch outright. |
| 117 | Arp Hold | 0..1 int | 0 | | lin | Latch. 1 = the arp keeps playing the set after every key is released. The latch set and its replacement rule are below. |
| 118 | Arp Retrig | 0..1 int | **1** | | lin | 1 = a new chord restarts the pattern at step 0 on that note-on's stamped sample. 0 = free-run: the pattern position is a pure function of the clock and no note-on ever moves it. Default 1 because a keyboard player expects the first note they press to sound when they press it; an author who wants the grid to own the phrase sets 0. Costs nothing — Arp On is 0. |
| 119 | Arp Vel Mode | 0..2 int | 0 | | lin | 0 As Played · 1 Fixed (id 120) · 2 Pattern (the level row). Exact mapping below. |
| 120 | Arp Fixed Vel | 1..127 int | 100 | | lin | The velocity every generated note carries when Vel Mode = 1. **The floor is 1, not 0**: this device treats a note-on with velocity 0 as a note-off (spectra.cpp's `data[2] > 0` test), so a generated 0 would be a generated note-off and the arp would silently emit nothing. A parameter that can express "no note" is a parameter that will. |
| 121 | Arp Steps | 1..16 int | 16 | | lin | Pattern length. The grid rows are ALWAYS sixteen entries long; Steps truncates the READ, never the storage, so shortening and re-lengthening a pattern is lossless. Same reason v3 stores all sixteen LFO levels. |
| 122 | Arp Chance | 0..100 | 100 | % | lin | Global probability that a step which would sound a new note actually sounds it. 100 selects the no-draw branch outright — no hash is computed and the render is bit-exact. Composition with a future per-step probability is pre-declared below. |
| 123 | — | | | | | reserved |
| 124 | — | | | | | reserved |

Registered exactly as the v2 rule says for the tail (name `—`, 0..1, default 0,
hidden, never read).

## The Mode enum (id 110) — ordering frozen, append-only forever

Let `N = [n0 < n1 < … < n_{c-1}]` be the arp's current note set **sorted
ascending**, `c ≥ 1` its size, and `H = [h0, …, h_{c-1}]` the same notes in the
held stack's insertion order, oldest first. Every mode is a cycle of length
`M(mode, c)`; the element at cycle position `j ∈ 0..M-1` is the note the step
sounds, before the octave cycle and the step row's octave offset are added.

| value | name | M(c) | element at j |
|-------|------|------|--------------|
| 0 | **Up** | `c` | `N[j]` |
| 1 | **Down** | `c` | `N[c-1-j]` |
| 2 | **Up-Down Inclusive** | `2c` | `j < c ? N[j] : N[2c-1-j]` — both endpoints repeated. |
| 3 | **Up-Down Exclusive** | `max(2c-2, 1)` | `j < c ? N[j] : N[2c-2-j]` — endpoints played once. |
| 4 | **Down-Up** | `max(2c-2, 1)` | `j < c ? N[c-1-j] : N[j-c+1]` — exclusive, from the top. |
| 5 | **As Played** | `c` | `H[j]` |
| 6 | **Random** | `c` | `N[idx]`, `idx` from the identity hash — see "Random and Chance". |
| 7 | **Chord** | `1` | **all of N**, every note on every step. |
| 8 | **Thumb** | `c ≤ 1 ? 1 : 2(c-1)` | `j` even → `N[0]`; `j = 2t+1` → `N[1+t]`. |
| 9 | **Pinky** | `c ≤ 1 ? 1 : 2(c-1)` | `j = 2t` → `N[t]`; `j` odd → `N[c-1]`. |

Worked, for `N = [C4, E4, G4]`: Up `C E G` · Down `G E C` · Up-Down Inclusive
`C E G G E C` · Up-Down Exclusive `C E G E` · Down-Up `G E C E` · Thumb
`C E C G` · Pinky `C G E G`.

- **Why `max(2c-2, 1)`.** At `c = 1` the natural formula gives a cycle of
  length 0, which is not a cycle. Modes 3 and 4 degenerate to Up on a single
  note. Modes 2, 8 and 9 need no guard (`2c` and the `c ≤ 1` leg cover it).
- **Why there is no "Down-Up Inclusive".** It is mode 2 started half a cycle
  later, and offering a mode that is another mode's phase is offering a mode
  nobody can tell apart in a preset list.
- **As Played survives note-offs.** `heldRemove()` compacts the stack in place
  and preserves order, so releasing the middle note of a chord leaves the other
  two in the order they were pressed. This is a property of the existing stack,
  not a new one.
- **Chord's cycle length is 1**, which is not a special case in the arithmetic —
  it is the whole point: with `M = 1` the octave axis advances on **every**
  step (see the composition rule), so Chord over two octaves alternates the
  chord at +0 and the chord at +1, which is what a chord arp is for.
- **Random is a draw, not a walk.** Its cycle length is still `c` so that the
  octave axis and the loop counter advance exactly as they do under Up; only
  the element is drawn. It does **not** avoid immediate repeats: a
  repeat-avoiding draw carries state, and state is what obligation 3 forbids.

## The octave cycle (ids 113, 114) and how it composes with the note cycle

Let `O = Arp Octaves` and let the octave axis have length

```
L_oct = (Oct Mode == 2 Alternate) ? (O <= 1 ? 1 : 2*O - 2) : O
```

with offset, in octaves, at axis position `u ∈ 0..L_oct-1`:

- **0 Up** — `+u` → 0, +1, +2, +3
- **1 Down** — `-u` → 0, −1, −2, −3
- **2 Alternate** — `u < O ? +u : +(2*O - 2 - u)` → for O = 3: 0, +1, +2, +1.
  The octave axis is itself an up-down-**exclusive** cycle, which is why its
  length is `2O-2` and not `2O`; an inclusive one would sit on the top octave
  for two whole note-cycles.

**The note counter advances first.** With `k` the absolute step number
(defined in the next section):

```
j = k mod M                      // note-cycle position — the FAST axis
u = (k div M) mod L_oct          // octave-cycle position — the SLOW axis
```

so the arp completes one full traversal of the note cycle before the octave
moves. `div` is floor division; `k` is never negative (see the step clock).

**The consequence, stated because the other reading exists elsewhere.**
Up-Down over two octaves bounces **inside octave 0, then inside octave 1** — it
does not bounce across the whole two-octave span. Some instruments do the
latter by flattening notes and octaves into one pool first. This contract does
not, for two reasons: a flattened pool makes the note cycle's length depend on
the octave mode, so "which counter advances first" stops having an answer; and
an author who wants the full-span bounce already has an exact tool for it — the
step row's per-step octave column, which is precisely what that column is for.
Write the shape you want into sixteen steps and it plays the shape you wrote.

**Final pitch** of a sounding step, with `i = k mod Steps` the pattern index and
`stepOct(i) ∈ -2..+2` the step row's offset:

```
pitch = element + 12 * (octOffset(u) + stepOct(i))
```

**A pitch outside 0..127 makes the step silent.** It is not clamped. A clamped
note is a wrong note played confidently, and an arp four octaves up from a top-C
chord should run out of keyboard rather than pile onto the last one. The step
still advances every index; only the note is not emitted.

## The step clock (ids 111, 112, 115, 116)

**The division names ONE STEP, not the cycle.** This is the one place v4
deliberately reads the sync table differently from v3's Custom LFO shape, where
"the division is the length of the WHOLE cycle". Both readings are stated in
their own sections so neither is a surprise, and the divergence is forced:

- An arp's rate control has named the step in every instrument ever built.
  "1/16 arp" means 1/16 notes, not sixteen steps crammed into a sixteenth.
- Arp Steps (121) is variable, 1..16. Under a whole-cycle reading, changing the
  pattern length would change the step duration — so shortening a pattern from
  16 to 12 would speed the arp up by a third. A length control that is secretly
  a tempo control is not a length control.

An LFO has no length parameter, which is exactly why the other reading is right
for it.

### The arp position `A`

`A` is a real number: **steps elapsed since the arp's origin**. Everything the
arp does is a function of `A`, and `A` is a function of the engine's clock and
of nothing else.

- **Synced (Arp Sync > 0) with a transport running** — `A = beatAcc_ / B`,
  where `B = kSpSyncBeats[ArpSync]` beats per step and `beatAcc_` is
  **v3's own anchored beat counter, the same variable, read the same way**: an
  f64 beat position advanced ONE SAMPLE AT A TIME at the pushed tempo,
  **anchored to the pushed transport beat and never driven by it**, re-anchoring
  only when the transport starts or when a demonstrably advancing host disagrees
  by more than 1/64 beat. See the ORCHESTRATOR RULING recorded in v3's
  determinism obligation 1 and in spectra.cpp's header — the arp is the second
  consumer of that ruling and adds nothing to it.
- **Free (Arp Sync = 0)** — `A` accumulates `ArpRate / sr` per sample from
  prepare(). The transport is not read at all.
- **Synced with no transport running** — `A` accumulates
  `bpm / (60 · B · sr)` per sample, with `bpm` the engine's pushed tempo or its
  120 fallback. This is the ruling's point 3 verbatim: a bank authored at a
  fixed tempo with the transport stopped must still play.

Then, with `arpOrigin_` the retrigger origin (0 at prepare(), see Hold and
Retrigger):

```
Aeff = A - arpOrigin_
```

`Aeff` is clamped at 0 from below; it can never be negative, because
`arpOrigin_` is only ever set to a value of `A` that has already occurred.

### The absolute step number `k`, and why it is a modulus and not a counter

```
k = floor(Aeff)          // the step whose onset most recently passed
i = k mod Steps          // the pattern index, 0..Steps-1
L = k div Steps          // the pattern's loop counter
```

**Every index the arp uses is a modulus of `k`.** Nothing accumulates: not the
note-cycle position, not the octave-cycle position, not the loop counter, not
the random stream. This is a deliberate architecture and it buys three things
that a stateful counter cannot:

1. **A locate lands on the right step.** After the host jumps to bar 33, the arp
   is at the step bar 33 implies, without having run there.
2. **A step that does not sound does not renumber the melody.** An OFF step, a
   tie, a step whose pitch left the keyboard and a step that lost its Chance
   draw all still advance `k`. The step row is a RHYTHM laid over a melody; a
   rhythm that renumbered the melody would be a different feature.
3. **Obligation 3 becomes free.** Randomness keyed on `k` is keyed on a pure
   function of the clock, so "two renders of the same project are byte-identical"
   is true by construction rather than by discipline.

### Swing (id 116) — which steps, and by how much

**Odd `k` is delayed. Even `k` is not.** `k` is the ABSOLUTE step number, never
the pattern index — so an odd-length pattern does not flip the swing on every
loop, and the swing stays welded to the beat where a listener expects it.

The onset of step `k`, in the `Aeff` domain (units of steps):

```
onset(k) = k + (k & 1) * (Swing / 300)
```

In **beats**, when synced with `B = kSpSyncBeats[ArpSync]` beats per step, the
delay applied to an odd step is exactly

```
delay_beats = B * Swing / 300
```

and when free-running, `delay_seconds = (Swing / 300) / ArpRate`.

At Swing = 100 the delay is `B/3`: a pair of steps spanning `2B` beats has its
second onset at `4B/3`, which is the 2:1 triplet feel and the universal meaning
of full swing. It stops there rather than running to a full step, because a
control whose top end collapses the pair onto one onset has a top end nobody can
use. **Swing = 0 selects a no-offset branch outright** — `onset(k) = k`,
bit-exact — the same discipline as smooth 0 and Warp Amt 0.

### Gate (id 115) and what overlap does to the voice allocator

A note started at step `k` ends at

```
off(k) = onset(k) + Gate / 100                       // no tie
off(k) = onset(k + m) + Gate / 100                   // tied through m steps
```

in `Aeff` units. **Gate is a fraction of the NOMINAL step and swing does not
scale it** — a swung pair keeps two notes of the same length and moves the
second one, which is what swing is; a gate that stretched with the swing would
turn a rhythm control into a duration control.

Gate > 100 % means step `k+1`'s note-on is emitted while step `k`'s note is
still sounding. **The arp does not get its own voice allocator and does not
ask for one.** It emits note-ons and note-offs; the ordinary v1 allocator —
first free voice, else steal the quietest, capped by Voices (id 39) — handles
them exactly as it handles a played chord. Two rules make that safe, and both
are the arp's obligation, not the allocator's:

- **Same note number, still sounding: the arp emits its note-off first, at the
  same stamped sample, immediately before the new note-on.** Without this rule
  the generated off for step `k` would arrive after step `k+1`'s on, and
  `noteOff()` releases the NEWEST matching voice — so the arp would release the
  note it had just started and leave the old one ringing forever. The
  consequence is worth stating plainly: **gate > 100 % cannot overlap a note
  with itself.** A one-note Up arp at 200 % gate re-attacks cleanly; overlap
  happens between DIFFERENT note numbers, which is the only place overlap means
  anything.
- **In Mono and Legato (Voice Mode 1, 2) overlap is a glide, not a stack.**
  Legato's overlapped note-ons do not retrigger envelopes, so an arp at
  gate > 100 % under Legato is a legato arp — which is the reason to reach for
  that combination.

An author who asks for Chord × 4 octaves × 200 % gate can demand more voices
than id 39 allows. That is a voice-steal, it steals the quietest, and it is the
same answer a played chord gets. Nothing is special-cased.

## Hold (117) and Retrigger (118) — the four combinations

Two sets exist and they are not the same set:

- **`heldSet`** — the physical held notes. This is spectra.cpp's existing
  64-deep mono/legato stack, which the v2 notes already require be "maintained
  in EVERY mode". The arp **reads it and never writes it**; see "What the arp
  does to the held-note stack".
- **`latchSet`** — the arp's own set, in force only while Arp Hold = 1.

One condition drives both features, and it is a condition the code already
computes: **`heldSet` was EMPTY immediately before this note-on joined it** —
spectra.cpp's `otherHeld == false` in `noteOn()`. Call that a **new chord**.

- **Latch rule (Hold = 1).** A new chord CLEARS `latchSet` first. Every note-on
  then adds to `latchSet`. Note-offs never remove from it. So: press a chord,
  release it, the arp keeps running; press one new note and the latch becomes
  that note alone; press more notes before releasing and they join.
- **Retrigger rule (Retrig = 1).** A new chord sets
  `arpOrigin_ = A` at that note-on's **stamped sample**, and step 0 fires at
  that same sample. A note-on joining a non-empty set never retriggers —
  otherwise rolling a chord on would stutter the pattern once per finger.

The four combinations, in full:

| Hold | Retrig | behaviour |
|------|--------|-----------|
| 0 | 0 | **Free-run, unlatched.** The pattern position is a pure function of the clock; note-ons never move it. Pressing and releasing keys changes WHICH notes the steps play, from the next step onward. Release everything and the arp goes quiet after the sounding note's gate; press again and it rejoins the grid wherever the grid has got to. This is the "locked to the bar line" setting. |
| 0 | 1 | **Retrigger, unlatched.** A new chord resets the position to step 0 at its stamped sample and sounds step 0 immediately. Adding a finger to a held chord does not. Release everything and the arp stops; the next press starts a new phrase from step 0. The default pair. |
| 1 | 0 | **Latched, free-running.** A new chord replaces `latchSet` but does not move the position — the phrase changes notes without changing its place on the grid. Turning keys into a chord progression over a running pattern. |
| 1 | 1 | **Latched, retriggering.** The same new-chord press both replaces `latchSet` and resets the position to step 0. One condition, two consequences, and they are independent because the two parameters are. |

**Switching Hold while notes are held.** 0 → 1 seeds `latchSet` from the current
`heldSet`; if that is empty, the arp stays quiet until a key arrives. 1 → 0
drops `latchSet` and the arp immediately plays `heldSet`, which may be empty —
in which case it goes quiet after the sounding note's gate. Neither transition
emits a note-on: a parameter change may stop notes, never start them (see below).

**CC 123 (all notes off)** empties `heldSet` — it already does — and **also
empties `latchSet`**, and the arp's sounding generated notes release. **CC 120
(all sound off)** does the same and additionally clears the arp's
sounding-note bookkeeping, since the voices it referred to are gone. After
either, the arp emits nothing until a note-on arrives. A panic that a latch
could outlive would not be a panic.

## Velocity (ids 119, 120) and the level row

- **0 As Played** — the velocity of the key that contributed the note. This
  requires the held stack to carry velocities, which it did not; see "What the
  arp does to the held-note stack". In Chord mode every note carries its own.
- **1 Fixed** — every generated note carries Arp Fixed Vel (id 120), whatever
  was played.
- **2 Pattern** — the level row supplies it, at the PATTERN index `i = k mod
  Steps`:

  ```
  velocity = 1 + (int)(126 * L(d_i) + 0.5)          // L(d) = d / 15
  ```

  so digit 0 → velocity **1** and digit 15 → velocity **127**. The floor is 1
  and not 0 for id 120's reason: 0 is a note-off on this device's wire, so a
  step drawn at the bottom of the row would emit nothing instead of emitting
  quietly. An author who wants a step silent turns the step OFF in the step row,
  which is the control that means that.

  Pattern is **absolute**, not a scaling of the played velocity. A mode that
  multiplies the two is a legitimate fourth value and appends as `3` under the
  append-only rule; v4 does not guess which one a bank wants.

The velocity floor the voices already apply (`velAmp = 0.30 + 0.70·vel/127`,
v1's fixed routing) is downstream of all three modes and is not touched.

## Chance (id 122) and Random mode — pure functions of a stable identity

Both draws come from one hash, built the way v3's Random-per-note source (13)
and its one-shot S&H draw are built: **splitmix64 over a stable identity, never
a stream, never a clock, no RNG state carried between draws.**

The identity is **the note set, the step index and the loop counter**. Since
`k = L · Steps + i`, the pair (step index, loop counter) IS `k`, so the identity
is two terms and not three:

```
mix64(x):  x += 0x9E3779B97F4A7C15
           x ^= x >> 30;  x *= 0xBF58476D1CE4E5B9
           x ^= x >> 27;  x *= 0x94D049BB133111EB
           x ^= x >> 31;  return x

setHash = mix64(c)                                   // c = the note count
for each note in N, ASCENDING:                       // never the played order
    setHash = mix64(setHash ^ (u64)noteNumber)

arpHash(k, salt) = mix64( setHash ^ (u64)k ^ ((u64)salt << 56) )
```

`salt = 1` for the Random-mode note draw, `salt = 2` for the Chance draw — two
independent draws from one identity, the same idiom v3 used to salt a one-shot
S&H by its LFO number. This is the finaliser spectra.cpp already carries; there
is still no second hash in this device.

**The set is hashed ASCENDING, deliberately**, even in As Played mode. Playing
C-E-G and playing G-E-C are the same chord, and a random pattern that changed
because a player rolled the chord the other way would be a bug the player could
hear and never explain.

**Random-mode index**, from `h = arpHash(k, 1)`:

```
idx = (u32)(((h >> 40) * (u64)c) >> 24)              // exactly 0..c-1
```

A multiply-shift over the top 24 bits, not a modulo — integer arithmetic with
no bias argument to have and no float in the path.

**Chance test**, from `h = arpHash(k, 2)`, with `C = Arp Chance` in 0..100:

```
sounds  iff  (u32)(((h >> 40) * 100u) >> 24) < C
```

The left side lands in 0..99, so `C = 100` always passes and `C = 0` never does.
**At C = 100 the branch is SELECTED OUT** — no hash is computed at all. The two
agree exactly; the selection is the bit-exactness discipline (Warp Amt 0,
smooth 0, Linear curve), not a correction.

- **Chance is tested only on steps that would sound a NEW note.** An OFF step is
  already silent; a TIE is not a new note and is not tested.
- **A step that loses its draw is silent but still advances every index.**
  Chance drops notes; it does not stall the melody. The alternative — hold the
  previous note when the draw fails — is a different feature (it is a tie), and
  it would make the pattern position depend on the draw, which would cost
  property 2 above.
- **A tie following a dropped step is silent**, because a tie holds the previous
  note and there is no previous note to hold.

**Per-step probability: v4 specifies none, and here is the composition rule for
the revision that does.** The step row leaves three bits per step spare (below).
They cannot carry a probability with their natural sense, because the step row's
default is a usable pattern rather than all zeros, and a zero probability field
would mean "never" — so the row's own default would be an arp that plays
nothing. A later revision therefore adds a **third row**, `arpp`, sixteen hex
digits with the level row's exact shape and a default of sixteen `f`s, giving
`q_i = d_i / 15`. The composition is a multiply:

```
p_i = (Chance / 100) * q_i
```

so a global Chance of 100 leaves the per-step value alone and a per-step of 1
leaves Chance alone, and neither control can surprise the other. The identity
hash needs no new term — it already carries `k`. This is written down now so
that the row, when it lands, is an append and not a renegotiation.

## The step grid — STATE, not parameters

Two rows in the `nxspc1;…` block, following v3's LFO grid precedent exactly: the
same one-line ASCII record, the same lowercase-hex charset, the same
leftmost-is-step-0 ordering, the same `d/15` level mapping, the same
"a missing block reads as its default", and the same cost, stated as plainly as
v3 stated it — **a drawn arp pattern cannot be automated and cannot be a matrix
destination.** It is shape, like a table is shape.

### `arpl` — the level / velocity row

| key | value | meaning |
|-----|-------|---------|
| `arpl` | exactly 16 characters from `[0-9a-f]` | The arp's level row. Leftmost digit is **step 0**; rightmost is step 15. Lowercase only, on write and on read — strict, so write and read are exact inverses. Digit `d` is level `L(d) = d/15`, digit 0 exactly 0.0 and digit 15 exactly 1.0. **Verbatim the `lfo1`/`lfo2`/`lfo3` encoding.** Read by Vel Mode 2 (Pattern) and by the Arp Step modulation source. |

**Default when the record is absent: `ffffffffffffffff`** — level 1.0 on every
step.

### `arps` — the step row

| key | value | meaning |
|-----|-------|---------|
| `arps` | exactly 32 characters from `[0-9a-f]` | The arp's step row. **Two digits per step, high nibble first**, sixteen pairs; the leftmost pair is step 0. Lowercase only. The pair for step `n` is `v = 16·hi + lo`, laid out below. |

**Default when the record is absent: `05050505050505050505050505050505`** —
`05` sixteen times: every step on, octave offset 0, no tie. **Not `01`** — the octave field is BIASED (code 2 is offset 0), so the byte that means "on, unshifted, untied" is `0x05` and not `0x01`. Stated because a reader who skims the bit table will assume the other one.

| bit(s) | mask | field | meaning |
|--------|------|-------|---------|
| 0 | `0x01` | **Step On** | 1 = this step participates. 0 = rest. |
| 1..3 | `0x0E` | **Octave** | code `c = (v >> 1) & 7`; offset = `c - 2`, so `c ∈ 0..4` is `-2..+2`. |
| 4 | `0x10` | **Tie** | read only when Step On = 1. |
| 5..7 | `0xE0` | reserved | written 0 by every v4 build, **IGNORED on read**. |

The three states of a step, and there are exactly three:

| Step On | Tie | behaviour |
|---------|-----|-----------|
| 1 | 0 | **Sound** a new note: this step's element, this step's octave offset, this step's velocity if Vel Mode = Pattern. |
| 1 | 1 | **Hold** the previous note through this step. No new note-on, no new note-off; the sounding note's `off` moves to this step's onset plus the gate. This step's octave offset is **ignored** — there is no new note to offset. A tie whose predecessor did not sound (a rest, a dropped Chance draw, a pitch off the keyboard, or the very first step after the arp started) is **silent**: a tie with nothing to hold is nothing. |
| 0 | — | **Rest.** Tie is not read. The previous note ends at its own gate, unaffected. |

**Tie-chain bound.** A note is held through at most 16 consecutive tie steps;
the 17th forces the note off at its own onset. A pattern in which every one of
its `Steps` steps is a tie sounds nothing at all (no step ever starts a note),
so the bound is unreachable in practice — it exists so that no reading of the
grid can produce an unbounded note, which is the only failure mode here that a
user cannot recover from by releasing a key.

### What malformed means, and what merely-newer means

The distinction v3 drew between a refusal and a degradation applies unchanged,
and the two rows sit on opposite sides of it in one place each:

- **Refused** (the whole `setStateString()` refuses, nothing is applied, the
  device is left exactly as it was): a wrong length, any character outside
  `[0-9a-f]`, any uppercase character, a duplicate `arpl` or `arps` key, a
  record with no `=`. These are strings this writer could not have produced.
- **Degraded, not refused**: an octave code of 5, 6 or 7 **clamps to 4**
  (offset +2), and bits 5..7 are **masked off**. These are values a LATER,
  wider build could legitimately write, and the versioning rule's job is to let
  a newer state land on an older build rather than break it — exactly the
  argument v3 made for a Table value of 9 arriving as a clamped 8.

## src/plugin/spectra_presets.inc — the `SPARP` macro

The v1/v2/v3 macros are unchanged. v4 adds one, in the established style:

```
SP_PRESET("SQ Thumb Ladder")
SP(109,  1)          // Arp On
SP(110,  8)          // Arp Mode = Thumb
SP(112,  7)          // Arp Sync = 1/16
SP(113,  2)          // Arp Octaves
SP(115,  85.f)       // Arp Gate
SP(116,  58.f)       // Arp Swing
SP(119,  2)          // Arp Vel Mode = Pattern
SPARP("f9c6f8c4fac6f8c4", "05051505070515050505150507050005")
SP_END()
```

| macro | signature | rules |
|-------|-----------|-------|
| `SPARP` | `SPARP("<16 lowercase hex>", "<32 lowercase hex>")` | The level row then the step row, in that order — the order they appear in this document and in the state string. **Both arguments are mandatory.** A preset that wants a default row writes the default string (`ffffffffffffffff`, or `05` sixteen times) rather than omitting the argument: a two-argument macro with an optional argument is a preprocessor trap and this file has enough of those. At most one `SPARP` per preset. |

**Placement** is `SPLFO`'s: anywhere between `SP_PRESET` and `SP_END`, in any
order relative to the `SP` rows, convention parameters-first-state-after, and it
does **not** count against the 64-override cap, which is a cap on `SP` rows.

**A state macro never sets a parameter**, v3's rule, unchanged and worth
restating because `SPARP` is where a bank author will trip on it next:
**`SPARP(…)` does not set Arp On.** The row must say `SP(109, 1)` itself. The
bank's range checker enforces the pairing exactly as it enforces v3's: **an
`SPARP` in a preset that does not set id 109 to 1 is a checker failure, not a
silent no-op.**

The checker does **not** require `SP(119, 2)` (Vel Mode = Pattern) alongside an
`SPARP`, and the asymmetry with v3's grid-requires-Custom-shape rule is
deliberate rather than an oversight. v3's pairing exists because a drawn LFO
grid with a non-Custom shape is read by nothing at all. An arp level row is
never read by nothing: it feeds Vel Mode 2 **and** the Arp Step modulation
source (17), which is live whenever the arp is on regardless of Vel Mode. So the
condition that makes a row dead is Arp On = 0, and that is the condition the
checker tests.

**`loadPreset` resets the arp state too.** v3's extension of "a preset is
COMPLETE however short it is" covers the two new rows without amendment:
`loadPreset` resets every id to its default AND every state block — `lfo*`,
`smooth*`, `cc`, `wt*`, and now `arpl` and `arps` — to its default, then applies
the row's overrides and state macros. Switching from a patch with a drawn arp
pattern to a preset that mentions none lands on exactly the state a fresh
instance would have.

## Arp Step as a modulation source (matrix source 17)

| value | name | domain | wire |
|-------|------|--------|------|
| 17 | Arp Step | [0..1] | `L(d_i)` — the level row's digit at the current pattern index `i = k mod Steps`, as `d/15`. **Instance-wide**, like Aftertouch (8) and the three v3 MIDI sources. |

- **It follows the STEP CLOCK, not the notes.** An OFF step, a tie, a dropped
  Chance draw and an empty note set all leave it reading the grid's level at the
  current index. That is what makes it useful: it is a tempo-locked sixteen-step
  staircase that the filter or the position can walk in lockstep with the
  pattern the arp is playing, and a staircase that dropped to zero every rest
  would be a different and much worse control.
- **It is a hard staircase.** There is no smoothing: the arp grid has no
  `smooth` companion and does not get one in v4. An author who wants it smoothed
  has the matrix curves (101..108) for shaping and a destination's own lag for
  the rest.
- **It is 0 whenever Arp On is 0**, and 0 after prepare() until the first step
  onset. This is the inert condition that matters and it is the one the
  bit-identity gate rests on: with the arp off, a slot pointed at source 17
  contributes exactly nothing, which is the same as v3's contribution of exactly
  nothing.
- **Cadence** is v2's, unchanged: per voice per sample for ordinary
  destinations, sampled at the absolute-timed control tick for destinations 11,
  12 and 17..19.

## What the arp does to the held-note stack, and to the voice engine

**To the stack: nothing. It reads it and never writes it.** The 64-deep stack
that Mono and Legato maintain stays the record of PHYSICAL keys, in every voice
mode, exactly as the v2 notes require. Generated note-ons and note-offs bypass
`heldPush`/`heldRemove` entirely and enter the voice engine at the point
`noteOn()`/`noteOff()` do their voice work. If generated notes were pushed onto
that stack, the stack would describe notes nobody is holding, and Hold,
Retrigger and the mono fallback — all three of which read it — would each be
wrong in a different way.

**One addition to the stack, and it is data, not semantics:** it gains parallel
velocity and channel arrays, written by `heldPush` and compacted by
`heldRemove` alongside the note numbers. Vel Mode 0 (As Played) needs the
velocity, and the identity hash `noteRandom(channel, note, absSample)` that
every generated note-on feeds needs the channel. Nothing about the stack's
ordering, depth, drop-oldest rule or maintained-in-every-mode rule changes.

**The mono note-off fallback is disabled while Arp On = 1.** In Mono and Legato,
v2 falls back to the most recent still-held note when a note-off arrives. With
the arp running, the arp's own note-offs are the truth, and a fallback to a
physical key would sound a note the arp did not schedule — inventing MIDI at a
note-off, which is precisely what obligation 2 exists to prevent. The fallback
returns the instant Arp On goes to 0.

**Arp On 0 → 1 with notes held.** Every voice sounding from a direct note-on is
**released** (an ENV release, not a cut) at frame 0 of the block in which the
change is observed. The held stack is untouched — it was maintained anyway — so
the arp starts from the truth. `latchSet` is seeded from `heldSet` if Hold = 1.
`arpOrigin_` is left alone if Retrig = 0; if Retrig = 1 it is left alone too,
because a parameter change is not a new chord. The arp begins at the next onset.

**Arp On 1 → 0 with notes held.** Every note the arp generated is released at
frame 0 and its bookkeeping is cleared. **Notes still physically held do NOT
re-sound.** A key that was never delivered to the voice engine cannot be resumed
without synthesising a note-on the player did not play, and this contract draws
the line there: **the arp may invent MIDI; a parameter change may not.** The
player re-presses. This is the same class of concession v2 made for a mid-phrase
Voice Mode switch, and it is stated for the same reason.

**Why both transitions are block-granular and not stamped.** Parameters are not
events in this device and never have been — they are read once per block into
`Blk` — so the transition lands at frame 0 of the block that observes it. That
is block-size dependent by construction, exactly as v2's Voice Mode switch and
v3's transport re-anchor already are, and it is why the bit-identity gate below
is stated over renders in which the arp's own parameters are not automated
mid-render. Every other property in this section is invariant.

## v4 id map

| block | ids | functional | note |
|-------|-----|-----------|------|
| v1 (frozen) | 0..41 | 42 | |
| v2 blocks (frozen) | 42..99 | 50 | |
| v3 blocks (frozen) | 100..110 | 9 | two reserved ids spent below |
| — reserved spent | 109, 110 | +2 | Arp On, Arp Mode |
| arpeggiator | 111..122 | 12 | continuous with 109..110 above it |
| v4 reserved tail | 123..124 | 0 | |

**Arp block, first..last: 109..124** — fourteen functional ids (109..122) and a
two-id reserved tail (123, 124), one contiguous run with no hole in it.

**125 ids total (0..124): 115 functional, 10 reserved.** `kSpParamCount` goes
111 → 125.

### Budget — v4 fits, and it is the last revision that will

**125 ≤ kMaxParams = 128 (internal_base.h). v4 fits and the cap is NOT raised.**

**But it leaves THREE.** The v2 rule that every revision has followed says new
ids arrive in blocks with reserved tails; three ids cannot hold a block and a
tail, and v4's own tail is two of the three. So, filed here in advance, in full,
so that the next author does not have to re-derive it:

> **The next revision must raise `kMaxParams` from 128 to 256.** It is one line
> in `src/plugin/internal_base.h`. The precedent is the whole history of the
> constant: 16 → 64 when Spectra v1 arrived with 42 ids, 64 → 128 when v2
> arrived with 100. The justification is the one that header already writes
> down and it does not weaken with the number: **this is a per-instance array
> bound, not an id space.** Ids ARE indices and a saved set stores them, so
> raising the cap cannot disturb any existing device — every id that was 0..15
> is still 0..15, no device gains or loses a parameter, and no state file
> changes by a byte. The cost is 128 further unused slots per internal
> instance: a `ParamInfo` (two `std::string`, three `f32`, three `bool`, a
> `u32` — about 88 bytes) plus an `atomic<f32>`, so roughly 12 KB per instance,
> a few hundred kilobytes across a project with thirty devices in it. What it
> buys is the property the fixed array exists for and which no other layout
> gives: `addParam()` can never allocate, so a device's parameter list is built
> without a heap call and `process()` reads a member array with no indirection.

v4 does not make that raise, because v4 does not need it and a cap raised
speculatively is a cap nobody checks. It is written here rather than left to be
discovered, which is this document's habit.

# Determinism obligations (v4)

Every one of these is a gate, not an aspiration, and they extend rather than
replace v3's nine. The rule they all serve is still the one v1 wrote: the same
input renders the same audio, in blocks of 1 and of 1024, in the daemon and in
process. **v4 is the first feature in this instrument that invents its own
MIDI**, so obligations 2 and 4 below are new in kind and not only in subject.

1. **The step clock derives from the transport beat when synced and from
   accumulated phase when free — never from wall time.** Synced with a
   transport, `A = beatAcc_ / kSpSyncBeats[ArpSync]`, where `beatAcc_` is the
   f64 beat counter v3 introduced: advanced ONE SAMPLE AT A TIME at the pushed
   tempo, **anchored to the pushed transport beat and never driven by it**,
   re-anchoring only when the transport starts or when a demonstrably advancing
   host disagrees by more than 1/64 of a beat. This is the ORCHESTRATOR RULING
   recorded in v3's obligation 1 and in spectra.cpp's header, **cited and reused
   verbatim, not re-derived** — the arp is its second consumer and changes
   nothing about it. Free-running, or synced with no transport, `A` accumulates
   per sample from prepare(), which is the ruling's own point 3. Either way the
   clock is the engine's, never the wall's. A re-anchor is a block-boundary
   event by construction — the transport arrives once per block — so on a
   re-anchor the arp emits note-offs for what is sounding and resumes at the new
   position; **it never replays skipped steps**, and the bit-identity gate is
   stated over renders whose transport does not re-anchor mid-render, exactly as
   v3's is.

2. **Generated note-ons and note-offs land at STAMPED SAMPLES inside the block,
   exactly as incoming MIDI does. This is the acid test and it is stated as
   one.** The arp runs inside the per-sample event loop: `Aeff` advances one
   sample at a time, and when it crosses an onset the arp calls the same
   `noteOn()`/`noteOff()` bodies an incoming event calls, at that sample, with
   that sample's absolute position as the identity stamp. **A project containing
   an arp must render `cmp`-identical at block sizes 1, 7, 64, 1024 and at the
   host's own irregular blocking.** An arp that emitted at the top of the block
   would quantise every note to the block boundary, and the same MIDI in blocks
   of 1 and of 1024 would produce different audio — which is the exact failure
   the note queue was built to prevent, arriving through a new door.
   - **Ordering at a coincident sample is fixed**: incoming MIDI first, then the
     arp. A note-on arriving on a step boundary is in the set that step plays,
     and can retrigger the pattern to that step. Within the arp at one sample,
     **note-offs precede note-ons**, which is what makes the same-note
     re-attack rule above work.
   - **The arp cannot overflow the event queue, because it never enters it.**
     Generated events are applied directly at their sample and consume no
     queue slot, so no density of arp output can push an incoming note-off out
     of the queue.

3. **Random mode and Chance are pure functions of a stable identity — the note
   set, the step index and the pattern's loop counter — hashed the way v3's
   Random-per-note source (13) and its one-shot S&H draw are hashed: splitmix64
   over the identity, never a stream, never a clock.** The construction is
   spelled out above; `setHash` folds the note numbers ASCENDING so a rolled
   chord is the same chord, and `k` carries the step index and the loop counter
   together because `k = L·Steps + i`. No RNG state is carried between steps, no
   draw depends on a previous draw, and voice stealing cannot perturb either.
   **Two renders of the same project must be byte-identical, and this is a
   gate**: the regression suite renders every arp preset twice in one process
   and once in a fresh process and `cmp`s all three.

4. **Bit-identity, and it is the release gate.** With Arp On at its default (0),
   **every existing patch and all 97 factory presets render byte-identically to
   v3.** Not "mathematically equal" — `cmp`-identical WAV, over the full 97-row
   bank plus Init, at 44.1, 48 and 96 kHz, at block sizes 1, 7, 64 and 1024,
   against renders taken from the v3 tip. This holds by construction and not by
   care: Arp On = 0 selects the v3 MIDI path outright (incoming notes reach
   `noteOn()` unchanged), source 17 is unreachable because matrix slots default
   to Off, the held stack's new velocity and channel arrays change no
   arithmetic, and no v4 default is anything other than inert. **v4 has no
   stated exception**, which is the one thing it does that v3 could not.

5. **The arp writes nothing the rest of the instrument reads.** It reads the
   held-note stack and never writes it; it allocates no voices and asks for no
   allocator of its own; it adds no destination to the matrix. So the voice
   engine's determinism arguments — v1's allocator, v2's block-size invariance,
   v3's per-note identity — are unchanged by this revision rather than extended
   by it, and none of them had to be re-proved.

6. **Swing 0, Chance 100 and Arp On 0 are SELECTED branches, not computed
   ones.** No offset arithmetic at Swing 0, no hash evaluated at Chance 100, no
   arp code reached at all with Arp On 0 — the same discipline Warp Amt 0,
   Noise Color 1.0, smooth 0 and the Linear matrix curve already follow, and for
   the same reason: mathematically equal is not bit-identical, and the gate is
   the second one.

7. **Every index the arp uses is a modulus of the absolute step number `k`, and
   `k` is a floor of the clock.** Nothing in the arp accumulates: not the note
   position, not the octave position, not the loop counter, not the randomness.
   A locate therefore lands on the step the bar implies without the arp having
   run there, and a step that does not sound cannot renumber the ones that do.
   This is the property that makes obligations 1 and 3 hold together rather than
   separately.

8. **No arp arithmetic goes through a locale-sensitive formatter or a
   transcendental.** The two new state records are hex; the level mapping is
   `d/15`; the swing offset is `Swing/300`; the gate is `Gate/100`; the two
   random draws are integer multiply-shifts. There is no `pow`, no `exp`, no
   table and no `printf` of a float anywhere in the arp, so there is no libm
   version and no `de_DE` in this path.

# What v4 does NOT change

Stated so nobody rediscovers it. The destination enum (0..19) does not widen,
for the third revision running. The sync division table does not change — v4
cites it and reads it per-step, which is a reading and not an edit. The eight
factory tables, their generation and their mip policy do not change. The v1
fixed routings stay live and still sum with the matrix. The matrix stays eight
slots and gains no ninth. The LFO Custom grid, its smooth, and its whole-cycle
reading of the division are untouched. The 64-override cap on a preset row stays
64. The `.nxp` format, the user-preset contract and the path escaping are
untouched. `kMaxParams` stays 128 — see the budget above for why that is the
last time this sentence can be written.

# v4 resolutions — where the brief was ambiguous or impossible

Recorded in v3's habit, because v3's author was right that this list is the
useful part. Each is a place the design was under-determined or self-
contradictory, and each was **resolved and written down** rather than invented
silently.

1. **"Reuse the LFO sync-division table verbatim" vs. what a division means.**
   v3's Custom LFO shape reads the division as the length of the WHOLE
   sixteen-step cycle. An arp cannot: Arp Steps (121) is variable, so a
   whole-cycle reading makes the pattern-length knob a tempo knob. **Resolved:
   the TABLE is verbatim and shared; the READING is per-step for the arp and
   per-cycle for the LFO.** Both readings are stated in their own sections. The
   divergence is real and is the single most confusable thing in v4, which is
   why it is argued in two places rather than mentioned in one.

2. **Which of the ten reserved ids to spend.** The brief says to spend them
   "where they sit contiguously and sensibly". Only 109/110 do both: the other
   eight sit in pairs inside four unrelated feature blocks, and v3's own
   practice (60/61 went to the LFOs their block owns) says a reserved id belongs
   to its block. **Resolved: spend 109 and 110, leave eight.** Scattering an
   arpeggiator across sub, warp, ENV3 and matrix to save four indices would have
   cost every future reader more than the indices are worth — v3's own argument
   for refusing 92/93.

3. **Which two arp params may take 109/110.** The v2 rule requires that a spent
   reserved id's default be 0 and that 0 mean "no effect", because a v3 `.nxp`
   writes every id including the reserved ones as 0. Only seven arp params have
   a default of 0. **Resolved: Arp On (0 = off) and Arp Mode (0 = Up).** The
   choice is also the readable one — the two ids that open the block are the
   switch and the mode.

4. **Up-Down across multiple octaves: bounce per octave, or across the whole
   span?** Both exist in shipping instruments. The brief asks "which counter
   advances first", which presupposes two composed counters, and a flattened
   note×octave pool has no answer to that question. **Resolved: two axes, note
   fast, octave slow — so Up-Down bounces inside each octave in turn.** The
   consequence is named in the contract, and the full-span bounce is delegated
   to the step row's per-step octave column, which is exactly the control that
   expresses it.

5. **"Chord = every held note on every step" leaves the octave cycle
   undefined.** With no note axis to traverse, the octave axis has nothing to
   wait for. **Resolved: Chord's note-cycle length is 1**, which falls out of the
   composition rule with no special case and gives the musically right answer —
   Chord × 2 octaves alternates chord-at-+0 and chord-at-+1.

6. **Up-Down Exclusive and Down-Up have a cycle length of 0 on a single note.**
   `2c-2 = 0` at `c = 1`. **Resolved: `max(2c-2, 1)`**, so both degenerate to Up.
   Thumb and Pinky get the same guard at `c = 1`.

7. **Gate > 100 % on a repeated note number is undefined, and the naive reading
   is a stuck-note bug.** `noteOff()` releases the NEWEST matching voice, so a
   generated off arriving after the next step's on would release the wrong
   voice and leave the old one ringing for the rest of the session. **Resolved:
   the arp emits its note-off for the outgoing copy immediately before the
   note-on for the new one, at the same stamped sample, offs-before-ons.** The
   honest consequence — gate > 100 % cannot overlap a note with itself — is
   stated in the contract rather than left to be found.

8. **Whether the gate scales with swing.** Undetermined by the brief.
   **Resolved: gate is a fraction of the NOMINAL step and swing does not scale
   it**, so a swung pair keeps two equal-length notes and moves the second one.
   A gate that stretched with swing would silently turn a feel control into a
   duration control.

9. **What Swing = 100 % means.** "Delayed by how much" has two conventions: a
   third of a step (the triplet feel) or a whole step (the pair collapses).
   **Resolved: `delay_beats = B · Swing / 300`, so 100 % is exactly `B/3`, the
   2:1 triplet.** The other convention's top end is unusable, and a control
   whose last 20 % nobody can turn to is a badly scaled control.

10. **Whether a step that does not sound advances the melody.** OFF steps, ties,
    dropped Chance draws and out-of-range pitches all raise it. **Resolved:
    every index is a modulus of `k`, so all four advance everything.** The
    alternative — a counter that only ON steps advance — needs state, and state
    cannot survive a locate. This resolution is the load-bearing one: it is what
    makes determinism obligations 1, 3 and 7 a single property instead of three
    negotiated ones.

11. **Per-step probability.** The brief makes it optional ("if you specify
    one"). The step row's three spare bits cannot carry it: the row's default is
    a usable pattern rather than all zeros, and a zero probability field would
    make the default row an arp that plays nothing. **Resolved: v4 specifies
    none, and pre-declares the shape of the one that lands later** — a third row
    `arpp` with the level row's exact encoding and an all-`f` default, composing
    as `p_i = (Chance/100) · q_i`, needing no new hash term. Naming the future
    record now makes it an append instead of a renegotiation.

12. **A default that is not all zeros.** v3's rule is "a missing block reads as
    its default, and every default is inert" — and for the LFO grids inert means
    all zeros. An all-zero arp step row is an arp that plays nothing, which is a
    broken default rather than an inert one. **Resolved: the arp rows' defaults
    are `ffffffffffffffff` and `05`×16, and the inert switch is Arp On (109),
    not the grid.** The rows are shape; the parameter is the off switch. Stated
    loudly in both places because it is a genuine divergence from the LFO
    precedent this feature otherwise copies exactly.

13. **What the checker must enforce alongside `SPARP`.** v3's rule is that a
    state macro never sets a parameter and the checker enforces the pairing.
    **Resolved: `SPARP` requires `SP(109, 1)` and does NOT require
    `SP(119, 2)`** — the level row is never dead when the arp is on, because it
    feeds matrix source 17 whatever Vel Mode says. The asymmetry with the LFO
    rule is argued in place so it does not read as an oversight.

14. **Whether a parameter change may start notes.** Turning the arp off with
    keys held, and turning Hold off with a latch running, both raise it.
    **Resolved: a parameter change may STOP notes and may never START them.**
    The arp is licensed to invent MIDI; a knob is not. So Arp On 1 → 0 releases
    the generated notes and does not resume the held keys — the player
    re-presses. The line is drawn once and both transitions follow it.

15. **What the arp does to the held-note stack.** The stack is shared with Mono
    and Legato and is documented as the record of what is held. **Resolved: the
    arp reads it and never writes it**, generated notes bypass
    `heldPush`/`heldRemove` entirely, and the stack gains only parallel velocity
    and channel arrays (data, not semantics) so that Vel Mode 0 and the per-note
    identity hash have what they need. The one behavioural consequence — **the
    mono note-off fallback is disabled while the arp is on** — is stated in the
    contract, because a fallback would sound a note the arp did not schedule,
    which is inventing MIDI at a note-off.

16. **Arp Sync's default.** Consistency with LFO Sync says 0 (free); musical
    sense says 1/16. **Resolved: 7 (1/16)**, because Arp On defaults to 0 so the
    default costs exactly nothing in render terms, and because the first thing a
    user does after switching an arp on is not "fix the rate". Recorded here
    rather than buried, since it is the only v4 default that is not simply the
    bottom of its range.

---

# v4 implementation notes (NOT contract — the contract above stays frozen)

Decisions the DSP took where the v4 contract leaves latitude, recorded in v2's
and v3's habit so the editor and the preset bank read the same behaviour the
voices run. Where any of these later needs to change, it changes here and in
spectra.cpp together; the tables above do not move.

- **The step row's audio-side packing, and the one contract-visible thing it
  decides.** The audio thread takes each row in ONE atomic load, which is v3's
  `gridBits_` rule and is not optional (state arrives through
  `setStateString()` on the daemon's pump thread while the audio thread is
  inside `process()`; a torn step nibble would be a step nobody drew, and it
  would be nondeterministic). Sixteen steps in one u64 means four bits a step,
  and a step has exactly **fifteen** reachable states after the contract's
  degradation: five octave codes for a rest, and five more each for on-untied
  and on-tied. Fifteen fits. The consequence a reader needs is that **a REST
  KEEPS ITS OCTAVE**: `arps=…04…` round-trips as `04` and not as `00`, so
  turning a step off and on again does not lose the octave it was drawn with.
  Tie is the only field the contract says is conditional, and it is the only
  one a rest drops.
- **A degraded `arps` value is re-emitted NORMALISED**, unlike the `wt`
  records, which are re-emitted verbatim. An octave code of 7 written by a
  later build arrives as 4 and is saved as 4, and the reserved bits are saved
  as 0. The two are not the same case: a `wt` hash is the NAME of a file this
  machine may simply not have, and losing it would lose the set's intent, while
  an out-of-range octave code is a value this build has decided what to do
  with. Keeping it verbatim would mean saving a state whose sound this build
  cannot reproduce.
- **Record order.** `arpl` then `arps`, emitted after the `wt`/`wtpath`
  records — the order the contract lists them in and the order `SPARP` takes
  its two arguments in. Reading is order-free, as the format says.
- **The tie is a LOOKAHEAD, not an accumulation**, and it has to be. The
  contract's own formula is `off(k) = onset(k + m) + Gate/100`, and a note at a
  gate under 100 % would have ENDED before the tie step arrived to extend it —
  so `m`, the run of tie steps after `k`, is read off the grid when the note
  starts. It is a pure function of `(k + t) mod Steps` and carries no state,
  which is why the arp keeps nothing but the list of notes it has started. Two
  small consequences: "a tie whose predecessor did not sound is silent" falls
  out with no bookkeeping at all (there is nothing to hold, so nothing is
  held), and the tie-chain bound is applied as `m <= 16` at the note's start
  rather than as "the 17th forces the note off at its own onset". The two agree
  everywhere the bound is reachable, and it is not reachable: a run of sixteen
  ties needs every step of the pattern to be one, and then no step ever STARTS
  a note.
- **Ordering inside one sample, spelled out.** Incoming MIDI first (the queue,
  then the overflow set at the block's last sample), then the arp. Inside the
  arp at one sample: every gate expiry that is due, then the note-off for every
  note number about to sound that is still sounding, then all the note-ons.
  ALL offs before ALL ons, which is what obligation 2 asks for and is a
  stronger reading than per-note interleaving — it has to be picked one way,
  because `alloc()` steals the quietest voice and the two orders can allocate
  differently.
- **`otherHeld` for a generated note.** The Legato-overlap test in `noteOn()`
  asks whether a key was already down. For a note the arp invented, the same
  question about the set the arp owns is "is another GENERATED note still
  sounding", and that is what is passed. At a gate of 100 % or less the
  previous note is already off, so a step re-attacks; above 100 % it is a
  legato glide, which is the reason to reach for Legato + gate > 100 %.
- **The free-running clock advances only while Arp On = 1.** Id 109 says that
  at 0 "the arp does not exist" and "every id below is read by nothing", and
  Arp Rate is one of those ids — so a free clock ticking at Arp Rate with the
  arp off would be reading it. The SYNCED clock needs no such decision: it is
  `beatAcc_ / kSpSyncBeats[ArpSync]`, and `beatAcc_` is v3's and advances
  regardless, which is exactly what makes a locate land right.
- **The first block after `prepare()` is not a transition.** A set saved with
  the arp on has no "off" to come from, and a render that starts at bar 33 must
  SOUND the step bar 33 implies rather than wait for the next one. So a fresh
  instance serves the step it lands on; only a genuine mid-run 0 -> 1 waits for
  the next onset, which is what the contract's sentence is about. A transport
  re-anchor resumes the same way — at the step the new position implies.
- **Arp Chance rounds.** The parameter is a float percent and the draw compares
  against an integer, so the read is `(int)(v + 0.5)` — the same spelling every
  other stepped read in this file uses. 100 is the selected no-draw branch, and
  99.6 rounds into it.
- **The two rows are read once per block**, into the same `ArpCfg` the arp's
  parameters land in, so a step and the pattern it is read from can never
  disagree inside one block. A live edit therefore lands at a block boundary,
  which is the cadence every parameter in this device already has.
- **`kSpParamCount` is 125 and `kMaxParams` stays 128**, as the budget section
  says. The next revision raises it; this one did not need to.

---

# v5 — the wavetable editor (FROZEN)

Spends **no parameter ids**, widens **no enum**, adds **one** state record per
oscillator, adds **one** rung to the resolution ladder and **one** directory to
the tree. Everything above this line is untouched: the 42 v1 ids, the 58 v2 ids,
the 11 v3 ids and the 16 v4 ids keep their meaning, their ranges, their defaults
and their fixed routings, and every rule this file has accumulated — blocks with
reserved tails, the Curve column, the state/versioning rule, contiguous id
arrays, "a reserved id belongs to its block" — governs this revision as written
and is not amended by it.

**Gate, restated for v5, and it is v4's with nothing subtracted.** v5 adds no
parameter, so there is no switch to default to off and no default to argue
about. A v1, v2, v3 or v4 state loaded into a v5 build renders **bit-identical**,
because the only thing v5 can change about a render is which 32×2048 floats sit
behind slot 8, and a state that does not name a table does not name one. **v5
has no exception**, for the same reason v4 had none and one better: v4 had to
argue that Arp On = 0 selects the old path, and v5 does not have a switch to
argue about.

**What this feature is.** The last Serum-parity gap, deferred twice in this file
("a wavetable *editor* (draw/harmonic pen) — v4 at the earliest") and taken here.
Two pens over one 32-frame table, a frame-range morph, and a durable home for the
result. It is deliberately the SMALLEST feature that makes a drawn table a first-
class citizen: **a drawn table is a custom table**, indistinguishable from an
imported one in every layer below the editor, and that identity is the whole
design.

**What v5 is NOT.** It is not a second kind of table. There is no "drawn" flag,
no drawn slot, no drawn code path in the voice, and no way for the DSP to ask
where a table came from. The editor edits FRAMES; provenance is not a property of
a wavetable, content is — which is v3's identity rule read out loud.

## The claim v3 pre-paid, and whether it holds

The brief's reasoning is that a drawn table needs no contract change at all,
because v3's identity is a content hash and the `.nxwt` cache is keyed by hash.
Checked against the v3 text, rung by rung, **it holds in three places and fails
in exactly one**, and the one is the whole of this revision's real work:

- **Identity — holds.** "The content hash" is defined over the resampled f32
  frames "as they stand after slicing, resampling to 2048, DC removal and set
  normalisation, and BEFORE any mip is built". A drawn table does no slicing and
  no resampling — it is authored at 2048 — and it is DC-removed and set-normalised
  at commit (below), so it arrives at the fold in exactly the canonical state the
  rule names. `contentHash(frames, 32)` applies with no amendment, and a drawn
  table and an imported table with the same samples are **the same table**, which
  is correct and is not a coincidence.
- **State — holds.** `wtA=<16 hex>` names a hash. It has never named a file.
- **The wire — holds.** `wt::hashesInDeviceState()` qualifies a record on three
  bounds — the key begins `wt`, the value is exactly 16 lowercase hex digits, and
  the store actually holds the hash — and a drawn table satisfies all three the
  moment `wt::adopt()` has taken it. `PoolKindWavetable` ships the frames, the
  daemon `ingest()`s them and **recomputes the hash**, refusing a disagreement.
  The daemon cannot tell a drawn table from an imported one and must not be able
  to.
- **Durability — FAILS.** The resolution ladder's fourth rung is "a re-import
  from the `wtpath` record". For an imported table that rung is the recovery: the
  WAV is the original and the cache is a copy. **For a drawn table there is no
  original.** Writing the only copy of a user's authored work into a directory
  this document itself calls a cache — a thing whose defining property is that
  deleting it is safe — is the one place the free ride runs out.

So: v3's mechanism carries a drawn table end to end. What it does not carry is
the fact that the bytes are now irreplaceable. That is what the next section
decides.

## The durability decision

**A drawn table is SAVED, not cached.** It is written to a user-owned wavetable
library that is not the cache, is not deleted by clearing the cache, and is named
in the resolution ladder ahead of the cache.

```
$XDG_DATA_HOME/nxtakt/drawn/<16 lowercase hex>.nxwt
```

falling back to `$HOME/.local/share/nxtakt/drawn/`, then the passwd entry's home,
then `/tmp` — the ladder `wt::userCacheDir()` already walks, because this tree has
one answer to "where does nxtakt keep a user file" and a drawn wavetable is DATA.
Published as `wt::drawnDir()`, beside `wt::userCacheDir()` and `wt::factoryDir()`.

**It is a SIBLING of `wavetables/`, not a subdirectory of it, and that is the
entire point of the path.** The cache is `.../nxtakt/wavetables/`; the gesture
that clears it — the user's `rm -rf`, a future Clear Cache button, a packaging
script — names that directory, and a drawn library inside it would be swept up by
every one of them. `.../nxtakt/drawn/` cannot be reached by any correct spelling
of "clear the wavetable cache".

**The resolution ladder gains one rung, and the append is the whole contract
change.** v3's order was: the in-memory store; `wt::factoryDir()/<hash>.nxwt`; the
user cache; a re-import from `wtpath`. It becomes:

| # | rung | note |
|---|------|------|
| 1 | the in-memory table store | `wt::find()`; never reads a file |
| 2 | `wt::factoryDir()/<hash>.nxwt` | frozen; a preset's tables live here |
| 3 | **`wt::drawnDir()/<hash>.nxwt`** | **NEW.** The user's authored library |
| 4 | `wt::userCacheDir()/<hash>.nxwt` | frozen; imports cached here |
| 5 | recovery from `wtpath` | frozen in intent, widened by extension below |

**A rung that fails for any reason falls through to the next**, and only the
exhaustion of all five is a refusal — stated explicitly here because v3 wrote
"if all of those fail" and left the middle unsaid. A `.nxwt` whose bytes do not
fold to its own name is refused by `readNxwt` and is therefore a rung that
failed, not a table that plays wrongly.

**Rung 3 sits above the cache** for one reason that is not taste: rungs 2, 3 and
4 are hash-keyed and therefore interchangeable when they hit, so their order can
only be about which one should win when a machine holds two copies — and the
answer is the one the user authored, because it is the copy that is not
disposable and the copy whose absence is the failure worth noticing early.

**Rung 5 widens by file extension.** If the `wtpath` record's decoded value ends
in `.nxwt` (byte-exact, lowercase), recovery is `wt::readNxwt()` and not
`wt::importFile()`; `readNxwt` already recomputes and compares the hash, so a
file that is not the table it claims to be simply fails the rung. Any other tail
is `importFile()` — WAV — exactly as in v3. **There is no fallback between the
two arms**: an `.nxwt` that fails to read does not then get offered to the WAV
reader, because a file that lies about being a wavetable cache is not a file to
guess about. This widening is what lets a drawn table travel by file copy: a user
who moves `~/.local/share/nxtakt/drawn/` to another machine, or hands one file to
a collaborator, has a set that resolves.

**A commit writes ONE file in ONE place.** `wt::adopt(t, /*cache=*/false)` takes
the table into the store WITHOUT touching the user cache, and the commit writes
`drawnDir()/<hash>.nxwt` itself. Two copies of an irreplaceable file under two
policies is not redundancy, it is two things to keep in sync.

**The write is atomic**: a temporary in the same directory, then `rename()` over
the target — learn.cpp's `writeAtomic` discipline, the same one `savePreset()`
uses — so a crash mid-commit cannot leave a half-written table under a name that
claims a hash. There is no `.bak` generation: the filename IS the content hash,
so a second commit of the same drawing writes the same bytes to the same name and
a commit of a different drawing is a different file. **A drawn table is never
overwritten and never has to be**, which is the property content-addressing was
always going to buy and the reason it is worth saying out loud.

**The user is told.** A commit is not silent and is not a cache write: the editor
reports "Saved to `<path>`" with the real path, once, on every commit. A user who
does not know a file exists cannot back it up, and this is the one file in this
feature whose loss cannot be undone by any other machine.

### The alternatives, and why each was rejected

1. **Write to the cache and accept the loss.** Rejected. It puts the only copy
   of authored work in the directory this document defines as safe to delete, and
   it makes a routine maintenance gesture destroy work with no warning and no
   recovery. There is a version of "accept the loss and say so loudly" that is
   honest engineering; this is not it, because the fix costs one directory and
   one ladder rung.
2. **Carry the frames in the project file.** Rejected on size and on format. The
   raw frames are 32 × 2048 × 4 = **262,144 bytes**; the project's `state` value
   is escaped printable text, so the frames must be encoded — base64 is
   **349,528 bytes** before escaping — and the project format is line-oriented
   text where `state` is one line. A set with eight drawn Spectras is 5.6 MB on
   sixteen lines. The deeper objection is that project.cpp's own header says the
   `state` value is "an opaque scalar: stored verbatim, escaped by `esc()` like
   any other string, never parsed here", and its round-trip guarantee is stated
   over exactly that; a state that grows by a third of a megabyte per drawing is
   still opaque and still round-trips, and is also a project file no human can
   read, diff or repair. The `nxspc1;…` grammar shares its shape with the sampler
   and the rack — "one line of printable ASCII" — and a third of a megabyte is not
   that shape.
3. **Carry them in the preset (`.nxp`).** Not a trade-off; **arithmetically
   excluded.** The `.nxp` cap is 256 KiB and "a preset larger than that is
   corruption". 32 × 2048 × 4 = 262,144 = **exactly 256 KiB**, so the RAW frames
   fill the cap to the byte with no room for the version tag, the `uri`, the
   `name` or a single `param` line — and they cannot be raw, because the format is
   text. Rejected before encoding overhead is even counted.
4. **A `wtframesA` record in the state string.** Rejected: it is option 2 wearing
   a different key, and it additionally breaks the state string's own promise that
   a Spectra with no v3/v4/v5 state writes the empty string and a state is one
   readable line.
5. **Write into `wt::factoryDir()`.** Rejected: it is read-only by contract
   ("read-only as far as this file is concerned") and it is a **build output** —
   `make` copies tables there and `make dist` ships them. A user's drawing in a
   directory that `make` owns is a drawing that a rebuild deletes.
6. **A durable user library, hash-named, ahead of the cache in the ladder.**
   **Chosen.** 256 KiB per table, on disk, in a directory the user owns, in a
   format the tree already reads and writes and already hash-verifies. It costs
   one accessor, one ladder rung, one directory and one sentence in the release
   notes.

### What is still lost, stated plainly

Deleting `~/.local/share/nxtakt/drawn/` destroys every drawn table on that
machine, and no set, preset or project can bring one back. This is true of every
file a user authors and it is not made less true by being content-addressed. The
refusal contract is what a set does about it — Table stays 8, the `wtA` and
`wtname` records are re-emitted verbatim, the oscillator renders factory table 0,
the editor draws the amber idiom naming the table — and that is v3's contract
applying without amendment, which is the correct amount of new behaviour for this
case: none.

## The two pens

Both pens edit **one frame** of the table: 2048 f32 samples, `wt::kCycle`, the
same length the hash folds and the mip builder reads. **There is no lower "pen
resolution."** A pen that drew into 256 points and upsampled would need a defined
upsample in the identity path, which is a second resampler and a second libm
dependence, to buy nothing — 2048 points across a canvas is about three samples
per pixel and no user has ever wanted the fourth.

The frame's 2048 samples are the **canonical representation**. Both pens read
them and both pens write them; nothing else is stored, and the harmonic view is
derived.

### The waveform pen

Draws a single cycle freehand.

- **Domain.** x maps to sample index `i = clamp(round(x_norm · 2047), 0, 2047)`;
  y maps to a value **clamped to ±1**. The pen cannot draw past full scale.
- **The canvas shows ±2, not ±1.** ±1 is drawn as a gridline pair and there is a
  visible headroom band beyond it, because DC removal can push a curve past ±1
  and a curve that vanished off the top of its own canvas would be a curve the
  user cannot see or fix. **The editor never clips a sample for display.**
- **Sparse strokes interpolate LINEARLY, in index order, across the span the
  stroke crossed.** Between two consecutively delivered points `(i0, v0)` and
  `(i1, v1)`, every index strictly between them is written with the linear
  interpolant; `i1 == i0` overwrites with `v1`; a stroke that reverses direction
  writes twice and the later write wins.
- **Untouched samples keep their previous value, and a stroke does NOT wrap.** A
  stroke from i=400 to i=900 changes 501 samples and nothing else. The cycle is a
  ring to the oscillator and a line to the pen: a discontinuity at the wrap is a
  legitimate waveform (it is what a sawtooth IS), so there is no wrap-continuity
  rule and no attempt to close the curve.
- **Linear and not a spline**, and it is a decision rather than laziness: a spline
  overshoots, an overshoot is a sample the user did not draw, and the pen must be
  able to draw a hard vertical step — a pulse edge — which no interpolating spline
  can express.
- **A line tool (shift-drag) is the same rule with two points** and needs no
  further specification.

**DC removal is applied at STROKE END, per frame, and the user watches it
happen.** The curve slides vertically the moment the pointer lifts. Applied
during the stroke, the curve would crawl under the cursor; applied at commit, the
last thing the user saw would not be the thing that got saved — which is the exact
failure the brief names, and it is worth one visible jump per stroke to avoid it.
The removal is: accumulate the frame's mean in **f64, ascending index order**,
then subtract it in f32 from every sample in ascending index order. Fixed
accumulation type and fixed order, so the same drawing gives the same frame.

**Normalisation is NOT per-frame and NOT per-stroke. It is one set-wide factor at
commit**, exactly as import applies it, and its visibility argument is different
in kind: a single scalar over all 32 frames **changes no shape and no inter-frame
relationship**. Nothing the user drew moves; the whole table gets louder or
quieter together. The editor carries a live peak readout so the factor is legible
before the commit, and that readout is the honest version of "you can see it
happen" — there is nothing else to see, because there is nothing else that moves.

### The harmonic pen

Draws a magnitude spectrum; the waveform is the inverse transform.

- **Editable harmonics: 1..256.** Harmonic 0 (DC) is not editable and is forced to
  zero. Harmonics **257..1023 exist, are not editable, and are PRESERVED** — see
  the interoperation rules. 1023 is what a 2048-point cycle carries and what
  `kSpMaxHarm` already is; 256 is the most bars a human can address, and a bar per
  harmonic past 256 is under a pixel on any canvas anyone will build. A control
  the user cannot hit is not a control.
- **The scale is dB.** Bar top is **0 dB = magnitude 1.0**, which is a full-scale
  sine at that harmonic under the analysis scaling `2/N` the mip builder already
  uses. Bar floor is **−80 dB**. Linear was considered and refused: a fifth
  harmonic at −40 dB is one four-hundredth of the canvas height and therefore
  invisible and undrawable, and the harmonics that give a wavetable its character
  live between −20 and −60 dB.
- **The floor is a HARD ZERO, not −80 dB.** A bar dragged to the bottom sets the
  magnitude to exactly 0.0f. "Drag it away" must mean the harmonic is gone, not
  that it is quiet; a −80 dB residue on 256 harmonics is a table with a floor of
  hiss in it that no gesture can remove.
- **No harmonic may exceed 0 dB.** The clamp exists so that the set normalisation
  at commit is a correction and not a rescue.
- **The dB→magnitude map is `m = (bar == floor) ? 0.0f : 10^(dB/20)`**, and it is
  the one libm call this feature adds. It sits **upstream of the content hash**,
  precisely where the import path's `std::sin` sits, and it carries the identical
  bounded consequence — see the determinism gates.

#### The phase convention: ALL-SINE, and why

Every harmonic is synthesised as a sine, phase −90° relative to cosine:

```
frame[i] = Σ over h of  m_h · sin(2π · h · i / 2048),    h = 1 .. 1023
```

Four arguments, in increasing order of how much they matter:

1. **It puts the canonical spectra where the ear expects them.** `m_h = 1/h` over
   all harmonics is exactly a sawtooth; `m_h = 1/h` over odd harmonics only is
   exactly a square. Those two gestures are the first two things anybody draws,
   and under all-cosine they are neither of those waveforms.
2. **It makes DC exactly zero by construction.** A sum of sines is odd-symmetric
   about i=0, so its mean is zero to the precision of the sum, and the per-frame
   DC removal is a **no-op on a harmonic-pen frame**. The pen therefore never
   moves what the user drew — the property the waveform pen has to buy with a
   visible jump, the harmonic pen gets for free.
3. **It puts a zero crossing at the cycle boundary for every drawn spectrum.**
   `sin(0) = 0` for every harmonic, so `frame[0] = 0` always, and the cycle joins
   itself continuously. Under all-cosine, `frame[0] = Σ m_h` — the largest value
   in the frame — so a flat spectrum produces a table whose every frame slams
   discontinuously at the wrap, and the band-limiting mip builder rings at a
   discontinuity that the drawing does not contain.
4. **And this is the one that is specifically about a WAVETABLE rather than an
   oscillator: frame morphing is a LINEAR CROSSFADE IN THE TIME DOMAIN.** `A
   Position` interpolates between adjacent frames sample by sample. Two adjacent
   frames with identical magnitude spectra and different phases **cancel as they
   cross** — the morph dips, goes hollow in the middle, and comes back, and it
   sounds like a phaser rather than a morph. A single fixed phase convention
   across every frame the pen writes is what makes a morph sound like a morph.
   This is why a phase choice that is inaudible in a one-shot spectrum is loud in
   a 32-frame table, and it is why the convention is stated in the contract and
   not left to the implementer.

**Rejected: all-cosine**, whose flat spectrum is an impulse with a crest factor
near 256 — one drawn frame would drag the whole table's set normalisation down by
48 dB. **Rejected: Schroeder phase** (`φ_h = −π h(h−1)/H`), which has the best
crest factor of the three and is perfectly deterministic, and which makes every
drawn spectrum sound glassy and chirped in a way the user did not ask for and
cannot see on the bars. **Rejected: random phase**, at once, for putting a draw
inside an identity.

#### How the two pens interoperate

- **Opening the harmonic view NEVER modifies the frame.** The view is derived: a
  forward `spFft` at 2048 of the current frame, displaying `|H_h|` for h in
  1..256. Round-tripping waveform → harmonic view → waveform with no bar touched
  is the **identity by construction**, not by numerical luck. An f32 FFT/IFFT pair
  is not bit-exact and this rule is what makes that fact irrelevant.
- **Touching a bar rewrites that harmonic's magnitude AND its phase, and leaves
  every untouched harmonic's COMPLEX value alone.** Synthesis is an inverse
  transform over the full 0..1023 spectrum in which touched harmonics carry
  `(m, sine phase)`, untouched harmonics carry their analysed `(re, im)`, and DC
  is zero. So a drawn sawtooth opened in the harmonic view and given one nudge at
  harmonic 3 keeps its harmonics 257..1023 exactly, and keeps harmonics 1, 2, 4..256
  exactly, and differs from itself at harmonic 3 and in the last bits of all 2048
  samples.
- **It IS lossy, and here is exactly where.** One bar touched re-rounds all 2048
  samples through an IFFT and rewrites one harmonic's phase to the convention.
  Nothing else is lost. In particular, harmonic content the bars cannot show is
  not silently discarded — which is the failure mode a "the pen edits 256
  harmonics and zeroes the rest" design would have had.
- **N consecutive harmonic edits perform exactly ONE forward analysis and N
  inverse syntheses. This is a gate, not an optimisation.** The view holds the
  analysed spectrum for as long as the frame has not been edited in the waveform
  domain, and each bar edit re-synthesises from that held spectrum rather than
  re-analysing the frame it just synthesised. Without this rule, fifty bar edits
  are fifty FFT round trips of accumulated f32 error, and the drift is audible
  before it is visible.
- **A waveform-domain edit invalidates the held spectrum.** The next time the
  harmonic view is opened it re-analyses. A time-domain stroke touches every
  harmonic, so there is nothing to preserve and nothing to pretend about:
  harmonic → waveform pen → harmonic does **not** preserve the harmonics the user
  drew, and the bars will show what the stroke actually made.

## Frames

**The table always has exactly 32 frames.** `kSpFrames` is the memory layout of
every table in this instrument, factory and custom, and the audio-rate read is
built on it. The editor never has 31 frames or 33.

- **The pen edits the SELECTED frame**, an integer cursor 0..31.
- **The cursor is EDITOR-ONLY.** It is not a parameter, it is not a state record,
  it is not saved, and a reopened editor starts at frame 0. A cursor is a place to
  look, not a value.
- **The cursor is NOT `A Position` (id 1) and never follows it.** The editor draws
  the live position as a marker on the frame strip and leaves the cursor where the
  user put it. Position is continuous, automatable and modulatable by four
  different things; a cursor that followed it would let an automation lane drag
  the pen around mid-stroke.
- **Moving.** Click a frame in the strip, or `[` / `]`, or the arrow keys. No wrap
  at either end.
- **Opening the editor on a table with fewer than 32 source frames stretches it to
  32 first**, by the same linear frame-axis interpolation `spBuildCustomMips()`
  already performs (cited, not re-derived), and the editor says that it did. A
  one-frame import becomes 32 identical frames, which is what a table with no
  frame axis is. Committing then yields a **different hash from the original**,
  which is correct: the content differs, and identity is content.

### Frame operations

| op | effect |
|----|--------|
| **Clear** | the cursor frame becomes 2048 zeros |
| **Copy / Paste** | one frame's 2048 samples, to and from an editor-local clipboard |
| **Duplicate** | copy the cursor frame into the next slot, pushing the tail down |
| **Insert** | insert a copy of the cursor frame AT the cursor, pushing the tail down |
| **Delete** | remove the cursor frame, pulling the tail up, and copy the (new) last frame into slot 31 |
| **Morph** | fill a range between two endpoints — below |
| **Re-phase endpoints** | rewrite two named frames to the pen's sine phase, magnitudes untouched — below |

**Insert and Delete are destructive and the contract says so.** The frame count
is fixed at 32, so Insert **drops what was frame 31** and Delete **duplicates the
last frame** to keep the count. There is no way to spell "insert" in a fixed-length
array that does not lose something at one end; the alternative — refusing to insert
when frame 31 is non-zero — is a tool that stops working the moment the table is
full, which is always. The editor confirms nothing and undoes everything: these
are editor-local operations on an uncommitted working copy, and undo is the
editor's own.

### Morph — the operation that makes 32 frames authorable

Given two frames `a < b` that the user has drawn, **Morph replaces frames
`a+1 .. b-1`** and does not touch `a` or `b`.

**The domain is HARMONIC: magnitudes are interpolated per harmonic and the result
is synthesised at the pen's sine phase.**

```
for k in a+1 .. b-1:
    t      = (k - a) / (b - a)
    m_h(k) = (1-t) · |H_h(a)| + t · |H_h(b)|          h = 1 .. 1023
    frame_k = Σ over h of  m_h(k) · sin(2π h i / 2048)
```

Ascending in `h`, ascending in `k`, f32 throughout — a fixed order, so the fill is
reproducible.

**Why not the time domain, and why there is no third option.** A time-domain fill
is `f_k = (1−t)·f_a + t·f_b`. That is *exactly* what `A Position` already computes
between adjacent frames — `spRead`'s linear frame blend — so a time-domain fill
writes 30 frames that produce audio indistinguishable from having drawn only two.
**It is a no-op you can hear.** And there is no third domain to reach for:
interpolating the COMPLEX spectrum is, by the linearity of the transform, the
time-domain crossfade written in a more expensive way. The choice is
magnitude-only or crossfade, and crossfade already exists.

**What magnitude interpolation buys, and it is the whole character of the
result:** harmonics fade in and out individually instead of two waveforms
cancelling through a hollow middle. A saw at `a` and a square at `b` sweeps its
even harmonics down to nothing across the range; a bright frame and a dark frame
roll off rather than crossfade. This is what a wavetable morph is supposed to
sound like and it is the reason the operation exists.

**The honest cost, stated rather than hidden.** The fill is synthesised at sine
phase; the endpoints keep whatever phase they were drawn with. If an endpoint is
not already in sine phase, there is a **phase step at the boundary** between `a`
and `a+1`, or between `b−1` and `b`, and it is audible as a click in the morph at
exactly that position. Three ways out were available and the choice is stated:

- take `a`'s phases throughout — **rejected**, it is asymmetric in `a` and `b` and
  moves the discontinuity to the far end rather than removing it;
- interpolate the phases — **rejected**, it needs unwrapping (which is ill-defined
  for a spectrum with near-zero magnitudes) and, done linearly on the complex
  values, it degenerates back into the crossfade;
- **synthesise the fill at the convention's phase, uniformly, and offer the fix as
  a separate explicit operation** — **chosen**, because it is the only choice
  symmetric in `a` and `b`, it needs no unwrapping, and it makes the fill's frames
  mutually phase-coherent, which is precisely what stops them cancelling as
  Position sweeps across them.

**Re-phase endpoints** is that fix: a named, user-initiated operation that rewrites
frames `a` and `b` to sine phase with their magnitudes untouched. It is **never
applied silently**. When a Morph's endpoints are not in sine phase the editor says
so in one line — the fill re-phased, the endpoints did not move — and offers it.
A tool that quietly rewrites the two frames the user actually drew is a tool the
user stops trusting.

**Bounds.** `b > a + 1` or the operation is a no-op with a line. The fill runs
over harmonics 1..1023 and not 1..256: the fill is not the pen and has no screen
to fit in.

## Commit — the canonicalisation, in order

A commit turns the editor's 32 working frames into a table with a name. Every
step is fixed and ordered, because the last step is the identity.

1. **Every sample must be finite.** One non-finite sample refuses the whole commit
   with a sentence. This is v3's rule — "a frame containing a non-finite sample is
   refused at import and never reaches the hash" — applying to the pen unchanged,
   so NaN payloads cannot become identity by a second door.
2. **Per-frame DC removal**, ascending frame then ascending index, mean accumulated
   in f64 and subtracted in f32. Normally a no-op: the waveform pen already did it
   at stroke end and the harmonic pen's convention makes it one by construction.
   It runs anyway, because a commit must not depend on which pen last touched a
   frame.
3. **Set-wide peak**: `pk = max |sample|` over all 32 × 2048, ascending.
4. **Refuse a silent table.** If `pk <= 1e-9f` — the same constant the import path
   already uses — the commit is refused with the sentence "this table is silent".
   Import maps that case to a gain of 1 and carries on, because an import is
   recovering someone else's file; a drawing that is all zeros is a mistake, and
   letting it through would burn an identity on silence forever.
5. **Multiply every sample by `g = 1 / pk`**, ascending.
6. **`u64 h = wt::contentHash(frames, 32)`.** The frames are now in exactly the
   state v3's rule names.
7. **Write `drawnDir()/<hashHex(h)>.nxwt`** atomically (`writeNxwt` to a temp in
   the same directory, then `rename()`). A file already there is left alone: same
   name means same bytes, and `readNxwt` recomputes anyway.
8. **`wt::adopt(t, /*cache=*/false)`**, then `spBuildCustomMips()`, then publish
   the base with the existing release store.
9. **Rewrite the device's state records**: `wtA` (or `wtB`) becomes the new hash,
   `wtpathA` becomes the drawn file's full path, `wtnameA` becomes the display
   name if one is set and is dropped if not.

**A commit that changes the frames CLEARS AND REWRITES `wtpath`.** Editing an
imported table produces a new hash, and the WAV that `wtpath` named is no longer
the table the hash names. A path that recovers a *different* table than its own
record's hash is worse than no path at all — it is the one thing rung 5 must never
do — so the record follows the content or it goes.

## Preview, and the bound that made it necessary

**The editor's working copy is not the playing table, and the playing table
changes only at commit.** But an editor with no audition is not an editor, and
the obvious implementation — commit on every stroke — collides with a real cap:
`spBuildCustomMips()` allocates 1.31 MB per distinct hash into a store that is
**never freed** (the lifetime invariant: the audio thread holds a raw base
pointer and there is no event that says no voice is inside a table any more), and
that store is bounded at `kSpMaxCustom = 32`. Thirty-two strokes and the
thirty-third is refused. This is not a thing to leave for the implementer to
discover.

**v5 adds one mechanism: a per-oscillator PREVIEW ARENA that is recycled, and is
not in the built store and not in identity.**

- **A ring of `kSpPreviewRing = 4` buffers** of `kSpFrames × kSpStride` floats
  each, allocated on the oscillator's first preview and **never freed** — the
  lifetime invariant applied to a fixed four rather than to an unbounded store.
  5.24 MB per oscillator with an open editor, which is one or two at a time.
- **Publishing a preview** builds the mip chain into the next buffer in the ring
  and stores the base with the same release store `spResolveCustom()` uses. The
  audio thread's read is unchanged and does not know a preview from a table.
- **The recycle proof is a stated bound, not a hope.** The audio thread takes the
  base at the top of a block and does not retain it past that block. A ring buffer
  is not rewritten until three further publishes have happened, so the editor's
  **minimum preview interval** must satisfy `4 × interval > maxBlock /
  sampleRate`, and the editor computes it from the values `prepare()` was given
  rather than hard-coding one: `interval = max(50 ms, 2 × maxBlock / sampleRate)`.
  At 4096 frames and 44.1 kHz that is a 92.9 ms block against 4 × 185.8 ms of
  protection.
- **Previews are rate-limited to that interval and are published on STROKE END**,
  not per pointer motion. A stroke ends when the pointer lifts; a user does not
  lift sixty times a second.
- **A preview touches NOTHING that is identity.** No hash, no file, no `wt`
  record, no store entry. `stateString()` during an open editor names the last
  **committed** table and nothing else.
- **`cancelPreview(osc)` republishes the committed base**, and closing an editor
  without committing does exactly that.

**The user-visible consequence, and it is loud.** Saving a project mid-edit saves
the last committed table; the uncommitted drawing is not in the set and is lost
when the editor closes. So: **an editor closing with uncommitted changes must
ask.** That obligation is in the contract because the alternative is a user losing
an hour of drawing to a window close, and no amount of implementation care fixes a
design that permits it.

**The commit cap is unchanged and is correct.** Distinct COMMITTED tables per
process stay bounded at `kSpMaxCustom = 32`, exactly as imports are, and
`spBuildCustomMips()` already returns the existing build for a hash it has seen —
so re-committing an unchanged drawing costs nothing. Thirty-two distinct saved
tables in one session is a session, not a stroke count.

## Parameters — none, and the cap is not raised

**v5 spends ZERO parameter ids.**

`A Table` (id 0) and `B Table` (id 8) already run 0..8, and value 8 already means
"this oscillator's custom table, identified by this oscillator's `wt` record". A
drawn table IS that. There is no drawn slot, no `0..9` widening, and no new
parameter anywhere in this revision — which follows directly from the design
decision at the top: if a drawn table were a second kind of table it would need
one, and it is not.

**The candidates that were considered and refused**, so nobody re-derives them:

- **A "drawn" flag parameter** — refused. Provenance is not a property of content
  and the DSP has no use for it. See "What v5 is NOT."
- **An editor-frame-cursor parameter** — refused. A cursor is not a value, it is
  not automatable in any meaningful sense, and making it a parameter would put an
  automation lane in a position to move the pen.
- **A morph-domain parameter** — refused. The domain is a frozen decision, made
  once, above; a knob for it would be a knob for "should this operation be a
  no-op".

### v5 id map

| block | ids | functional | note |
|-------|-----|-----------|------|
| v1 (frozen) | 0..41 | 42 | |
| v2 blocks (frozen) | 42..99 | 50 | |
| v3 blocks (frozen) | 100..110 | 9 | 109, 110 spent by v4 |
| v4 arpeggiator (frozen) | 109..124 | 14 | reserved tail 123, 124 |
| **v5** | **—** | **0** | **spends nothing** |

**125 ids total (0..124): 115 functional, 10 reserved. `kSpParamCount` stays 125.
`kMaxParams` stays 128.**

**The 256 raise is NOT required by v5**, and the paragraph v4 pre-filed stands
exactly as written, unconsumed and unamended. Three ids remain (125, 126, 127) and
they still cannot hold a block and a tail, so v4's sentence is still true: the
next revision that appends a block must raise `kMaxParams` from 128 to 256, it is
one line in `src/plugin/internal_base.h`, and the justification is already
written. v5 does not raise it, because v5 does not need it and a cap raised
speculatively is a cap nobody checks. **This is the first revision in this file's
history to append a feature and spend no ids at all**, and it is worth naming as
the thing that made it possible: v3 built the custom-table mechanism as content
plus state rather than as parameters, and v5 is the interest on that.

## State — one new record per oscillator

Added to the `nxspc1;…` block in the established format. The version tag does not
move; see the versioning rule below.

| key | value | meaning |
|-----|-------|---------|
| `wtnameA` `wtnameB` | escaped text, ≤ **64** DECODED bytes | that oscillator's custom table's DISPLAY NAME. **Never identity, never consulted in resolution, never sent over the wire.** Absent means "no name". |

- **Escaping is `wtpath`'s, verbatim**, and the implementation shares the same
  helper rather than growing a third escaper: escape any byte with `c <= ' ' ||
  c >= 0x7F || c == '%' || c == ';' || c == ',' || c == ':' || c == '='` as `%`
  plus two **uppercase** hex digits; leave every other byte raw. Unescaping is
  **STRICT** and refuses, in order: a `%` not followed by two hex digits; an escape
  decoding to NUL; a raw byte the writer would have escaped; anything over 64
  decoded bytes. A name is UTF-8 and survives because every byte ≥ 0x7F is escaped.
- **An empty name is not written.** A `wtnameA=` record is a record with an empty
  value, which is a thing this writer would not produce; clearing a name drops the
  record. There is exactly one spelling of "no name" and it is absence.
- **`wtnameA` with no `wtA` is skipped, not refused.** It is a display string for a
  table that is not there — inert, harmless, and precisely the shape of thing a
  later or earlier build might leave behind. A duplicate `wtnameA` IS refused,
  under the existing duplicate-key rule; choosing one of two is guessing.
- **64 bytes, not 4096.** It is a label, not a path, and a state string is one
  readable line.
- **The `wt`-prefix hazard, named and bounded.** `wt::hashesInDeviceState()`
  qualifies a record whose key begins `wt` and whose value is exactly 16 lowercase
  hex digits. A table named literally `deadbeefdeadbeef` therefore qualifies. The
  cost is the one wavetable_io.h already writes down — "one wasted lookup and
  nothing more", because a hash that qualifies is still only shipped if the store
  actually holds it — and it is the same hazard `wtpath` has carried since v3 (a
  relative path of 16 hex characters). Named here so it is a known bound rather
  than a surprise; not designed around, because designing around it would mean a
  key outside the `wt` family and the family is worth more than the lookup.

**Record order on write:** `wtA`, `wtpathA`, `wtnameA`, `wtB`, `wtpathB`,
`wtnameB` — the name follows its table, as the path does. Reading is order-free,
as the format has always said.

**Defaults stay inert and the empty-state rule stands.** `stateString()` still
returns the EMPTY string when no grid is drawn, no CC is learned, no arp row is
drawn and no custom table is named. A missing `wtname` reads as no name, and
`customName()` falls back exactly as it did before v5.

### `customName()` — a compatible widening, not a new method

`WavetableControl::customName(int osc)` is documented as "basename, display". Its
resolution order becomes, and this is a strict superset of what it did:

1. the `wtname` record, if present;
2. `basename(wtpath)`, if a path is present — v3's behaviour;
3. the bare 16-hex hash — v3's behaviour.

Every table that has no name displays exactly what it displayed before. This is
why v5 adds no `customTitle()`: a second method that answers the same question
slightly better is a contract with two answers in it.

### Versioning rule

**The state tag stays `nxspc1` and it does not move for v5.** The tag versions the
GRAMMAR, not the build: v5 adds a record under the existing grammar, and the
grammar's own forward-compatibility rule — "a record whose key this build does not
know is skipped" — was written for exactly this. Bumping to `nxspc2` would make
every v5 state unreadable by every v4 build in order to communicate nothing a v4
build could act on.

- **A v4 build reading a v5 state** skips `wtnameA`/`wtnameB`, resolves the table
  by hash exactly as before, and displays `basename(wtpath)` — the drawn file's
  name, `<hash>.nxwt`. It is a full degradation with no refusal and no loss of
  sound. It also cannot resolve rung 3, so a drawn table whose file lives only in
  `drawnDir()` lands on the refusal contract there: amber, factory table 0, records
  re-emitted verbatim. **A v4 build never loses the set's intent**, which is the
  whole reason the refusal contract is shaped the way it is.
- **A v5 build reading a v1..v4 state** finds no `wtname`, which reads as no name,
  which is v3/v4 behaviour verbatim.
- **`loadPreset` resets the new record too.** v3's extension of "a preset is
  COMPLETE however short it is" covers it without amendment: `loadPreset` resets
  every id to its default and every state block — `lfo*`, `smooth*`, `cc`, `wt*`,
  `arpl`, `arps`, and now `wtname*` — to its default, then applies the row's
  overrides and state macros.

## src/plugin/spectra_presets.inc — the `SPWTNA` / `SPWTNB` macros

The v1/v2/v3/v4 macros are unchanged. v5 adds two, in the established style, and
adds them as a **pair of A/B macros rather than one macro with an oscillator
argument**, because v3 already settled that question ("a 0/1 argument beside
`SPLFO`'s 1-based `n` is a trap") and this file has one answer to it.

```
SP_PRESET("PD Drawn Glass")
SP(  0,  8)          // A Table = custom slot 8
SP(  1,  0.4f)       // A Position
SPWTA("3f9c1a0b7d24e685")
SPWTNA("Drawn Glass")
SP_END()
```

| macro | signature | rules |
|-------|-----------|-------|
| `SPWTNA` / `SPWTNB` | `SPWTNA("<name>")` | The display name for oscillator A's / B's custom table. **1..64 bytes, printable ASCII or UTF-8, no control bytes.** At most one of each per preset. **Requires the matching `SPWTA` / `SPWTB` in the same row** — a name for a table the row does not name is a name for nothing, and the bank's range checker fails it rather than dropping it silently, exactly as it fails an `SPARP` without `SP(109, 1)`. |

**Placement** is `SPLFO`'s and `SPARP`'s: anywhere between `SP_PRESET` and
`SP_END`, in any order relative to the `SP` rows, convention
parameters-first-state-after, and it does **not** count against the 64-override
cap, which is a cap on `SP` rows.

**A state macro never sets a parameter**, v3's rule, unchanged: `SPWTNA` sets no
id, and `SPWTA` still does not set `A Table` to 8. The row says `SP(0, 8)` itself.

**Why a macro at all.** Without it, a factory preset that ships a drawn table
displays sixteen hex digits — which is what v3 already does for any preset naming
a factory table, and which was a wart nobody had to look at because factory
`.nxwt` files were rare. A revision whose entire premise is that users will author
a LIBRARY of tables cannot ship the bank that demonstrates it with hashes for
names.

## Host contract — five additions to `WavetableControl` (host.h)

Append-only, each with a default so that a backend which does not draw is already
correct without being touched — the discipline `savePreset()` and
`factoryPresetCount()` established. `customName()`'s widening is above and is not
a new method.

```cpp
// GUI thread. Copies this oscillator's resolved table into `out` as
// kSpFrames * wt::kCycle floats, frame-major, STRETCHED to 32 frames by the
// same linear frame-axis interpolation spBuildCustomMips() performs when the
// resolved table has fewer. False, and `out` untouched, when this oscillator
// has no resolved custom table.
virtual bool readFrames(int osc, f32* out) const { (void)osc; (void)out; return false; }

// GUI thread. Build and publish a PREVIEW from `frames` (32 * kCycle,
// frame-major). Touches no hash, no file and no state record: what
// stateString() names does not move. False if any sample is non-finite, if the
// oscillator has no handle, or if the caller is inside the minimum preview
// interval. Rate-limited by the contract, not by the caller.
virtual bool previewFrames(int osc, const f32* frames) { (void)osc; (void)frames; return false; }

// GUI thread. The nine-step commit above: canonicalise, hash, write the drawn
// file atomically, adopt, build, publish, and rewrite wt/wtpath/wtname. `name`
// may be null or empty for no name. False with lastError() set, and NOTHING
// changed, on any refusal.
virtual bool commitFrames(int osc, const f32* frames, const char* name) { ... return false; }

// GUI thread. Drop any preview and republish the committed table. Idempotent.
virtual void cancelPreview(int osc) { (void)osc; }

// GUI thread. Set or clear the display name of this oscillator's custom table.
// Content is unchanged, so IDENTITY IS UNCHANGED: a rename writes no file and
// produces no new hash. Null or empty clears. False if the name is over 64
// bytes or holds a control byte.
virtual bool setCustomName(int osc, const char* name) { (void)osc; (void)name; return false; }
```

`customFrames(int osc)` is **unchanged** and keeps meaning "the SOURCE frame count
of the resolved table, 1..32". After a drawn commit it is always 32, which is a
fact about drawn tables and not a change to the method.

# Determinism obligations (v5)

Every one of these is a gate, not an aspiration, and they extend rather than
replace v3's nine and v4's eight. The rule they all serve is still the one v1
wrote: the same input renders the same audio, in blocks of 1 and of 1024, in the
daemon and in process.

1. **Frames → mip chain is a pure function of the frames, and v5 adds NOTHING to
   that path.** A committed drawing goes through `spBuildCustomMips()` — the same
   factored builder the eight factory tables and every imported table go through,
   from identical f32 input, in the same order — so v3's obligation 7 covers a
   drawn table with no amendment and no second proof. **The same drawing produces
   the same table on every machine that has the frames**, and the frames are what
   crosses the wire.

2. **Gesture → frames is UPSTREAM of identity, and that is where the libm is.**
   The pen path calls libm in two places: `10^(dB/20)` for the harmonic pen's bar
   map, and `std::sin`/`std::cos` inside `spTwiddle()` for the forward analysis
   and the inverse syntheses. The second adds nothing — the mip builder already
   makes exactly those calls for every table in the instrument. The first is new
   and is **deliberate, and it sits precisely where the import path's rules (b)
   and (c) put theirs.** wavetable_io.cpp names that dependence and bounds it, and
   the bound here is the same one and is weaker: two machines with different libm
   versions could in principle turn the same GESTURE into two hashes — and a
   gesture is never replayed on a second machine, because what travels is the
   frames. **No machine ever renders two different tables under one hash.** The
   pen path's answer to "is it pure?" is therefore: downstream of the frames,
   yes, absolutely, and it is the same code that already was; upstream of them,
   no, and it does not need to be.

3. **A drawn table crosses the wire by the same `PoolKindWavetable` route as an
   imported one, and the daemon renders it identically.** `hashesInDeviceState()`
   finds `wtA`/`wtB` because the key begins `wt` and the value is 16 lowercase hex
   — a drawn table's record is byte-indistinguishable from an imported one's — the
   pool ships the frames, and `wt::ingest()` **recomputes the hash and refuses a
   disagreement**. Gate: a project whose Spectra names a drawn table renders
   `cmp`-identical in the daemon and in process, at 44.1, 48 and 96 kHz, at block
   sizes 1, 7, 64 and 1024, which is the same gate v3 wrote for imports and the
   same test row with a different table in it.

4. **The daemon never draws, and nothing in the editor is reachable from
   `nxtaktd`.** The pens, the preview arena, the commit and `drawnDir()` sit above
   the seam exactly where `importFile()` sits, on the one non-realtime thread that
   owns the device. Nothing here is reachable from the audio thread — the audio
   thread reads a `const f32*` into a mip chain, which is the only thing it has
   ever done — and no preview publish allocates on it.

5. **The synthesis and the fill are exact, ordered arithmetic.** Sine phase
   synthesis sums harmonics **ascending in h**; the morph fills **ascending in k**;
   DC means accumulate in **f64, ascending in i**; the peak scan is ascending. Two
   runs of the same drawing in one process, and one run in a fresh process,
   produce identical frames and therefore the identical hash. **Gate: draw, fill,
   commit, twice, and `cmp` the two `.nxwt` files.**

6. **No wall clock and no RNG anywhere in the pen path.** A drawn table's identity
   is a function of its samples and of nothing else — not of when it was drawn,
   not of how long the stroke took, not of a seed. The preview interval reads a
   clock and the preview is not identity; nothing that reaches `contentHash()`
   ever does.

7. **The commit is atomic and self-verifying.** `writeNxwt` to a temporary in the
   same directory, then `rename()`, so a crash mid-commit cannot leave a partial
   file under a name that claims a hash — and even if it could, `readNxwt`
   recomputes the fold and refuses a file whose bytes do not name themselves, so a
   torn drawn table is a rung that fails and never a table that plays wrongly.

8. **The preview arena's recycle bound is arithmetic, not timing luck.** Four
   buffers, a minimum publish interval of `max(50 ms, 2 × maxBlock / sampleRate)`,
   and the audio thread's rule that a base pointer is not retained past the block
   it was read in. A buffer is not rewritten until three further publishes have
   happened, so the protection is four intervals against one block period. The
   editor computes the interval from the values `prepare()` was given.

9. **Bit-identity, and it is the release gate.** **Every existing patch, every one
   of the 121 rows of the factory bank (Init included), and every factory `.nxwt`
   render byte-identically to v4.** Not "mathematically equal" — `cmp`-identical
   WAV, at 44.1, 48 and 96 kHz, at block sizes 1, 7, 64 and 1024, against renders
   taken from the v4 tip. This holds by construction and not by care, and the
   construction is thinner than v4's because there is less to it: **v5 adds no
   parameter, changes no default, changes no enum and changes no arithmetic in any
   render path.** The only new state record is absent from every state that
   exists, and its presence changes a display string. **v5 has no exception, and
   unlike v4 it did not have to argue for one.**

10. **Both factory banks still read.** Every `.nxwt` in `wt::factoryDir()` still
    reads, still recomputes to its own name, and still builds the identical mip
    chain; the new rung 3 is appended below rung 2 and cannot shadow it. Gate: the
    factory wavetable directory's every file round-trips `readNxwt` → `contentHash`
    → its own filename, and the mip chain each produces is `cmp`-identical to the
    v4 tip's.

# What v5 does NOT change

Stated so nobody rediscovers it. **No parameter id is added, spent, widened or
retired**; `kSpParamCount` stays 125 and `kMaxParams` stays 128, and the eight
reserved ids from v2/v3 (46, 47, 52, 53, 66, 67, 92, 93) and v4's tail (123, 124)
all stay exactly where they are. The destination enum (0..19) does not widen, for
the fourth revision running. `A/B Table` stays 0..8. The eight factory tables,
their generation and their mip policy do not change. The content hash does not
change — not its construction, not its finaliser, not its `-0.0` fold, not its
frame-major order. The `.nxwt` format does not change and `kNxwtVersion` stays 1.
The state string's version tag stays `nxspc1`. The `nxspc1` grammar does not
change: same charset, same escaping, same refusal rules, same duplicate-key rule.
The `.nxp` format, the user-preset contract and the path escaping are untouched.
The 64-override cap on a preset row stays 64. `kSpFrames` stays 32, `wt::kCycle`
stays 2048, `kSpStride` and the ten-level mip layout stay exactly as they are.
`kSpMaxCustom` stays 32 and `kMaxTables` stays 256. The arpeggiator, the matrix,
the LFO grids and their smoothing are not touched by a single line of this
revision.

# v5 resolutions — where the brief was ambiguous or impossible

Recorded in v3's habit and v4's, because both authors were right that this list is
the useful part. Each is a place the design was under-determined, self-contradictory
or simply wrong, and each was **resolved and written down** rather than invented
silently.

1. **"A drawn table written to the cache resolves forever with no contract change
   at all" — true for three of the four things it claims, false for the fourth.**
   Identity, the state record and the wire all carry a drawn table with no
   amendment; **durability does not**, because the ladder's recovery rung is a
   re-import and a drawing has nothing to re-import from. **Resolved: the claim is
   verified rung by rung in its own section, and the one failure is what the
   revision is about.** Reported rather than quietly patched, because "no contract
   change" was the premise the brief was working from.

2. **Where a drawn table lives.** The brief offered four options and asked for one.
   **Resolved: a durable, hash-named, user-owned library at
   `$XDG_DATA_HOME/nxtakt/drawn/`, a SIBLING of the cache and not a child of it**,
   inserted into the ladder as rung 3, ahead of the cache. The project-file and
   preset options are rejected on arithmetic (256 KiB raw is exactly the `.nxp`
   cap, and 350 KiB base64 is over it before a single `param` line); the
   cache-and-accept-the-loss option is rejected because the cache's defining
   property is that deleting it is safe. Full argument and all five rejections in
   "The durability decision".

3. **The path a `wtpath` record may name.** v3 defined rung 5 as a re-import, and
   `importFile()` reads WAV. A drawn table's recovery file is a `.nxwt`, which
   that reader refuses. **Resolved: rung 5 branches on the file extension —
   `.nxwt` goes to `readNxwt()` (which already recomputes and compares the hash),
   anything else goes to `importFile()` — and there is no fallback between the two
   arms.** This is the minimum widening that makes a drawn table portable by file
   copy, and it costs one `strcmp` on the tail.

4. **Whether a drawn table is a new KIND of table.** The brief does not say, and
   the shape of the question invites a flag. **Resolved: it is not, and there is no
   provenance anywhere in the contract.** Identity is content, so "drawn" is not a
   property a table can have. The consequence is the design's best simplification:
   **the editor opens on any resolved custom table, imported or drawn**, because a
   table is 32 × 2048 f32 either way — and editing an imported table simply
   produces a new table, which is exactly right.

5. **The frame count is fixed at 32, so "insert/delete" as the brief lists them
   are impossible as stated.** A fixed array cannot grow. **Resolved: Insert pushes
   the tail down and DROPS frame 31; Delete pulls the tail up and DUPLICATES the
   new last frame into slot 31.** Both are destructive at one end and the contract
   says so. The alternative — refusing to insert when frame 31 is non-zero — is a
   tool that stops working exactly when the table is full, which is always.

6. **"Morph in the waveform domain or the harmonic domain" presupposes two
   options; there are two, and one of them is a no-op.** A time-domain fill is
   `(1−t)f_a + t·f_b`, which is **precisely what `A Position` already computes**
   between adjacent frames — so it writes thirty frames that sound like having
   drawn two. And there is no third domain: **complex-spectrum interpolation IS
   the time-domain crossfade, by the linearity of the transform.** **Resolved:
   magnitude-only interpolation, sine-phase synthesis, over harmonics 1..1023.**

7. **A magnitude-only fill leaves the phase of the fill undetermined at its
   boundaries.** Two endpoints have two phase sets and the fill has one. **Resolved:
   the fill is synthesised at the pen's convention throughout — the only choice
   symmetric in `a` and `b` and the only one that needs no phase unwrapping — the
   resulting boundary step is stated as an audible cost, and the fix is a NAMED,
   USER-INITIATED "re-phase endpoints" operation that is never applied silently.**
   A tool that quietly rewrites the two frames the user actually drew is a tool the
   user stops trusting.

8. **How many harmonics the pen edits, and what happens to the rest.** A 2048-point
   cycle carries 1023 and nobody can address 1023 bars. The naive answer — edit 256,
   zero the rest — destroys the upper spectrum of any waveform-drawn frame the
   moment the harmonic view is touched. **Resolved: edit 1..256; harmonics 257..1023
   are PRESERVED with their full complex value; only a TOUCHED bar has its phase
   rewritten.** That is what makes the round-trip statement precise instead of
   apologetic.

9. **Round-tripping between the pens: is it lossy?** It is, and the brief asks
   where. **Resolved, in three parts: opening the harmonic view never modifies the
   frame (identity by construction, not by numerical luck); touching one bar
   rewrites that harmonic's phase and re-rounds all 2048 samples; a waveform stroke
   invalidates the analysis, because a time-domain stroke touches every harmonic and
   there is nothing to pretend about.** Plus one gate that is genuinely load-bearing
   and that no brief would have asked for: **N consecutive bar edits perform exactly
   ONE forward analysis and N inverse syntheses**, because fifty bar edits through
   fifty FFT round trips accumulate f32 drift that is audible before it is visible.

10. **"Whether the drawn cycle is DC-removed and normalised on commit, and whether
    the user can see it happen" — the two halves have different answers.**
    **Resolved: DC removal happens at STROKE END and is visibly a vertical slide;
    set normalisation happens at COMMIT and is one scalar over all 32 frames.** The
    first is where it is because doing it during the stroke makes the curve crawl
    under the cursor and doing it at commit makes the last thing the user saw not be
    the thing that got saved. The second is defensible precisely because it **changes
    no shape and no inter-frame relationship** — there is nothing to watch except a
    level, and the editor carries a peak readout so the level is legible in advance.
    Net property, and it is the one to state: **nothing the user drew ever moves at
    commit.**

11. **An all-zero drawn table.** The import path maps a peak of zero to a gain of 1
    and carries on. A drawing of silence is not a recovered file, it is a mistake,
    and it would take an identity forever. **Resolved: a commit whose set peak is
    `<= 1e-9f` — the same constant the import path already uses, so there is one
    number — is REFUSED with a sentence.** Divergence from import, deliberate,
    stated in both directions.

12. **A table with fewer than 32 source frames, opened in the editor.** An import
    may have 1..32. **Resolved: the editor always edits 32; opening stretches by the
    same linear frame-axis interpolation `spBuildCustomMips()` already performs, and
    says that it did.** Committing then yields a different hash, which is correct
    and is not a bug to be hidden — the content differs.

13. **The frame cursor's relationship to `A Position` (id 1).** Both address the
    frame axis and one of them is automatable. **Resolved: the cursor is editor-only
    state, is never saved, and NEVER follows Position** — the editor draws Position
    as a marker and leaves the cursor alone. A cursor that followed a modulated,
    automatable parameter would let an LFO drag the pen mid-stroke.

14. **The feature is unusable after 32 edits, and the brief does not mention it.**
    `spBuildCustomMips()` allocates 1.31 MB per distinct hash into a store that is
    never freed and is capped at `kSpMaxCustom = 32`; an editor that committed per
    stroke would exhaust it in a minute. **Resolved: a per-oscillator PREVIEW ARENA
    of four recycled buffers, outside the built store and outside identity, with a
    stated recycle bound (`4 × interval > maxBlock / sampleRate`) rather than a
    hopeful one — and a commit that stays an explicit act, bounded at 32 distinct
    tables per process exactly as imports are.** This is the single largest thing
    the brief's outline did not contain, and leaving it to the implementer would
    have meant discovering it as a bug report.

15. **The consequence of separating preview from commit, which is a user-visible
    loss.** If the playing table changes only at commit, then a project saved
    mid-edit does not contain the drawing. **Resolved: stated in the contract, and
    turned into an obligation — an editor closing with uncommitted changes MUST
    ask.** The alternative is a design that permits losing an hour of drawing to a
    window close, and no amount of implementation care fixes that.

16. **Editing an imported table leaves `wtpath` pointing at the wrong file.** The
    new content has a new hash; the WAV the path names has the old one. **Resolved:
    any commit that changes the frames CLEARS AND REWRITES `wtpath` to the drawn
    file.** A path that recovers a different table than its own record's hash names
    is worse than no path at all — it is the one thing rung 5 must never do.

17. **The brief says "prefer ZERO new parameter ids" and does not say whether a
    display name is worth a state record.** Zero ids was achievable and is what
    happened. The name was the only judgement call. **Resolved: one record per
    oscillator, `wtnameA`/`wtnameB`, plus two preset macros.** The argument is that
    this revision's entire premise is a user building a LIBRARY of drawn tables, and
    a library whose every entry is sixteen hex digits is not a library. It is the one
    place v5 adds surface the brief did not ask for, and it is flagged here rather
    than buried so that a reviewer can strike it in one edit if they disagree: the
    record is inert when absent, `customName()`'s fallback is v3's behaviour verbatim,
    and nothing else in the revision depends on it.

18. **`wtnameA` begins with `wt`, and `hashesInDeviceState()` qualifies on that
    prefix.** A table named exactly `deadbeefdeadbeef` would qualify as a hash.
    **Resolved: named and bounded rather than designed around** — the cost is the one
    wavetable_io.h already writes down ("one wasted lookup and nothing more", since a
    qualifying hash is only shipped if the store holds it), and it is the same hazard
    `wtpath` has carried since v3. Moving the key outside the `wt` family would buy a
    lookup and cost the family.

19. **Whether the state tag moves to `nxspc2`.** A new record could be read as a new
    format. **Resolved: it does not move, and the rule is written down for good — the
    tag versions the GRAMMAR, not the build.** Bumping it would make every v5 state
    unreadable by every v4 build in order to communicate nothing a v4 build could act
    on, and it would throw away the skip-unknown-keys rule that was written for
    exactly this case.

20. **What the editor needs from `host.h`, which the brief does not mention at
    all.** `WavetableControl`'s six methods can import, name, count and clear; none
    of them can read frames, preview, or commit. **Resolved: five append-only
    additions with defaults (`readFrames`, `previewFrames`, `commitFrames`,
    `cancelPreview`, `setCustomName`) and ONE compatible widening
    (`customName()`'s resolution order gains the `wtname` record ahead of its two
    existing fallbacks).** No `customTitle()`: a second method that answers the same
    question slightly better is a contract with two answers in it.

---

# v5 implementation notes (NOT contract — the contract above stays frozen)

Decisions the editor and the seam may take where the v5 contract leaves latitude,
recorded in v2's, v3's and v4's habit so the DSP author and the editor author read
the same behaviour. Where any of these later needs to change, it changes here and
in the code together; the sections above do not move.

- **Grid and snap are editor latitude.** An optional x-grid of 2/4/8/16/32
  divisions with snap, and an optional y-quantise, are useful and are not
  contract: they change which samples the pen writes, and the pen's rule ("linear
  across the span the stroke crossed") already covers whatever points the UI
  delivers. Nothing downstream can tell.
- **Undo is editor-local and unbounded by the contract.** The working copy is
  32 × 2048 floats, so a full-table undo step is 256 KiB; a stack of 64 is 16 MB
  and is fine. Per-frame steps are the obvious optimisation and nothing here
  depends on which is chosen.
- **The harmonic view's held spectrum is 1024 complex f32 per frame — 8 KiB —
  and is held for the CURSOR frame only.** Moving the cursor drops it; the gate is
  "N consecutive edits, one analysis", and consecutive means without leaving the
  frame.
- **Preview at stroke end, and only when something changed.** A stroke that
  writes the identical samples publishes nothing. The rate limit is a floor, not a
  schedule: there is no timer, and a slow drawer publishes once per stroke.
- **The drawn file's mtime is the only "when".** The contract carries no
  timestamp — a timestamp in a content-addressed name would be a second identity —
  so a "recently drawn" list in the editor reads `stat()` and is a view, not state.
- **`drawnDir()` is created lazily, on the first commit, at 0755** (learn.cpp's
  `ensureParentDir`). A user who never draws never has the directory, which is the
  same discipline the user preset directory follows.
- **A commit that finds the file already present skips the write.** The name is
  the content, so the bytes are known to match; `readNxwt`'s recompute makes even
  a wrong-bytes case fail closed. Skipping is an optimisation with no observable
  behaviour, which is why it is here and not above.
- **The morph's synthesis is the mip builder's `spIfft` with `spTwiddle`
  twiddles**, reused rather than re-derived, so the fill and the render share one
  transform and one set of tables. A second FFT in this device would be a second
  thing to keep in agreement.
- **The pen never sees a mip.** Everything above the seam works in 2048-sample
  frames; `spBuildCustomMips()` is the only thing that has ever built a mip and it
  stays that way.
