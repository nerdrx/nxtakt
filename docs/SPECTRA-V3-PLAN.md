# Spectra v3 — the phase plan

Written by the v2 orchestrator as a hand-off: this document is the DESIGN;
the implementing sessions (Opus waves) follow it the way v2 followed the
frozen contract. Read docs/SPECTRA-PARAMS.md (v1+v2) and the v2 wave's
commit messages (f176919, 39db292) first — every law below was earned there.

## What v3 is

Serum parity has three gaps left worth closing, plus the debt the v2 wave
deliberately deferred. In one sentence per pillar:

1. **Custom wavetables** — import a WAV, and the table you imported plays
   from the daemon, byte-deterministically, forever.
2. **Modulation grows hands** — drawable LFOs, MIDI as a mod source, a
   matrix you can patch by dragging.
3. **Presets become YOURS** — save from the panel, a user bank beside the
   factory one, and a second factory bank (total ≥ 96).

Explicitly OUT of v3 (state this in the release notes rather than letting
anyone rediscover it): a wavetable *editor* (draw/harmonic pen — v4 at the
earliest); per-instrument FX (NxTakt has a device chain; racks are the
answer); an audio-rate oscilloscope in the panel (the audio lives in the
daemon's process; an honest tap is a wire feature to design on its own,
not a side effect of a synth wave).

## Pillar 1 — custom wavetables (the big one)

**Import.** A WAV lands on the panel's hero well (the sampler's drop idiom,
reuse its target/badge language) or via a browse chip. Interpretation rules,
in order: (a) Serum convention — N×2048 single-cycle frames, N ≤ 256 →
resample never, slice directly; (b) any mono/stereo WAV whose length is an
integer multiple of a power-of-two cycle 256..4096 → resample each cycle to
2048 via windowed sinc; (c) arbitrary WAV → pitch-detect the first cycle
(autocorrelation, the transient tooling in core has prior art), slice at
detected period, resample, cap 32 frames. Always: DC-remove per frame,
normalize the SET (not per frame — inter-frame level is musical), build the
SAME per-octave FFT-truncated mip chain the 8 factory tables get
(spectra.cpp builds it once at prepare; factor that builder out and share
it — one implementation, proven once).

**Identity & state.** A custom table's identity is its CONTENT HASH
(splitmix64 over the resampled float frames), not its path. The state
string carries `wt=<hash>;wtpath=<escaped path>` — hash is the identity,
path is the recovery hint. Loading a set: try the hash in the table cache,
else re-import from path, else the osc falls back to factory table 0 with
the refused-device amber idiom naming the missing file. Table cache on
disk: `~/.local/share/nxtakt/wavetables/<hash>.nxwt` (raw f32 frames +
tiny header; NOT the mips — mips rebuild at load, they are derived data).

**The wire.** The daemon must play the same table. Follow §15's device-state
pattern exactly: the imported table crosses as a pool blob
(`PoolKindWavetable`, new), named by the state string's hash, shipped
beside the device state like a sampler's audio rides beside its path.
Retirement discipline: copy-on-pump-thread, retire immediately (the synth
owns its own mip memory after the copy — same argument as §15.3, the audio
thread never touches pool memory). Removal tests for every new field; the
daemon-vs-in-process parity leg is a RENDER of a custom-table patch,
cmp-identical (there is no reason for it not to be bit-exact — the mips are
deterministic functions of the frames).

**Params.** Table selection stays what it is today (id 2/10 select the
slot); custom tables occupy slots 8+ per oscillator, so the enum WIDENS
(the v2 filter-type precedent: widening an enum is backward-compatible
when old values keep their numbers). No new param ids for this pillar
beyond `WT Slots` bookkeeping if the implementer needs one — prefer none.

## Pillar 2 — modulation grows hands

- **Drawable/step LFOs**: each LFO gains shape "Custom": a 16-step grid
  with per-step level and a global smooth 0..1. The GRID IS STATE, not
  params (the state string grows an `lfoN=<16 hex levels>;smooth` block —
  presets can carry it; the .inc row format grows an optional `SPLFO(n,
  "0123456789abcdef", 0.5f)` macro). Step advance is tempo-synced or free
  per the existing rate/sync params; determinism: step index derives from
  the transport beat (synced) or the voice's accumulated phase (free),
  never wall time.
- **LFO one-shot mode**: a per-LFO mode enum (Loop / One-shot) — one-shot
  makes any LFO an envelope. 3 new param ids.
- **MIDI mod sources**: Mod Wheel (CC1), Pitch Bend (as a source AND its
  hardwired pitch action with a Bend Range param, 0..24 st, default 2),
  CC-learn slot (one generic "MIDI CC" source whose CC number is state,
  learned by right-clicking the source selector — the rack's learn idiom).
  Sources append to the matrix enum (append-only, the v2 rule). New param
  ids: Bend Range (1).
- **Matrix drag-assign**: the killer UX. Drag from a source chip (each
  LFO/ENV/macro section gets a small grab handle) onto any modulatable
  knob → fills the first empty matrix slot with that source→dest at +0.30,
  opens the slot's amount for immediate drag. The knob draws a thin mod
  ring (the sum of active matrix contributions at rest) — Serum's single
  best affordance. Editor-only work; zero DSP.
- **Per-slot curve**: matrix amount response enum per slot (Linear /
  Exp / S-curve) — 8 new ids. (Polarity already lives in the amount sign.)

Param budget: ~12 new ids + kMaxParams is at 128 with 100 used — fits.
If the implementer lands above 128, raise kMaxParams once more (the v2
precedent: 64→128 was one line plus its filed justification).

## Pillar 3 — presets become yours

- **User presets**: PluginInstance grows `savePreset(name)` — writes the
  current `stateString()` to `~/.config/nxtakt/presets/<uri-slug>/<name>.nxp`
  (text: one state string + a `name`/`category` header). `presetCount()`
  et al. enumerate factory + user, user bank listed under a "User" header
  in the dropdown (the editor's category derivation already handles
  headerless names; give user presets an explicit header instead). A save
  chip in the panel's GLOBAL section: click → inline name field (the
  marker-rename idiom) → saved → dropdown refreshes. Overwrite asks
  nothing but status-bar-announces ("preset 'X' replaced — undo has the
  old one"? NO — presets are files, not session state; announce without
  undo, keep `<name>.nxp.bak` of the overwritten one instead).
  This API lands on the CONTRACT (host.h) — sampler and future
  instruments get it for free; keep the base implementation generic
  (any stateString-bearing device can save presets).
- **Factory bank 2**: +48 presets in the v2 categories, from a fresh
  sound-design brief: exploit v3 (≥ 10 presets on custom-shape LFOs,
  ≥ 6 with drawn step-sequences as their identity, ≥ 4 wavetable-import
  showcases shipping a small factory .nxwt each ≤ 64 KiB). Same range
  checker, same audibility smoke (the v2 harness is in the repo history;
  scratchpad/smoke49.cpp pattern — 97/97 must sound).

## The wave plan for the implementing session

Contract first, always: **Wave 0** extends SPECTRA-PARAMS.md with the v3
ids/state-blocks/enums above, frozen before code. Then FOUR parallel
agents with disjoint ownership:

| agent | owns | delivers |
|---|---|---|
| wt | spectra.cpp (table pipeline), new src/plugin/wavetable_io.{h,cpp}, ipc/pool.h (+kind), nxtaktd.cpp (apply), engine_handle.cpp (ship), tests (ipc/daemon/handle sections) | import, cache, wire crossing, parity render, removal table |
| mod | spectra.cpp (LFO/matrix/MIDI DSP — COORDINATE: wt agent also owns spectra.cpp → run these two SEQUENTIALLY (wt first) or split spectra.cpp by a textual-include seam first (spectra_tables.inc); the orchestrator decides at launch | drawable LFOs, one-shot, MIDI sources, bend, curves; invariance + red proofs |
| ed | app_spectra.cpp | drop-import UX, LFO grid editor, drag-assign + mod rings, save-preset chip, User category |
| bank | spectra_presets.inc + factory .nxwt assets | bank 2, checker, smoke |

Standing rules (verbatim from the v2 waves — give these to every agent):
no git write operations ever (`git show HEAD:path` to read baselines;
restore by file copy; the stash incident is PAPER.md incident 13);
byte-verified mirrors in the session scratchpad `<ns>-mine/`; GUI tests
only in headless gamescope via the drive harness; gates = zero warnings,
full `make test` from deleted build/, four demo renders cmp-identical to
the pinned baseline, red-then-green for every fix, removal tests for
every mirrored field; sanitizers (ASan+UBSan+LSan, TSan) over anything
touching the pool; counters documented "with an event" increment AFTER
the event, release-ordered. Bit-identity gates: v1 AND v2 defaults and
all 49 existing presets render identically before/after.

Release shape: v0.10.0. Tag only after CI green on the tip (caps are 30/35
min as of 82b63ad); windows-cross timeouts are rerunnable flakes; a
"cancelled" CI on a superseded push is the concurrency group, not a flake.

## Open questions the implementer may decide (and record)

- Stereo custom tables: import as mid-only for v3 (state the choice) or
  per-channel table pairs (cost: 2× memory, real Serum feature). Default
  to mid-only unless it falls out free.
- The .nxwt header format: version byte + frame count + cycle length —
  keep it boring; it is a cache, not an interchange format.
- Whether the mod ring draws matrix sums at audio rate (it must not — the
  ring is the at-rest sum; the moving value is the knob's existing
  behavior; state this in a comment or the ring will grow into a meter).
