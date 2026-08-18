# NxTakt: A Professional-Grade DAW in Four Days

## Deterministic Gates for Multi-Agent Software Engineering — an Experience Report

*nerdrx (owner/director) · Claude Fable 5 / Claude Opus 5 agents (implementation) · August 2026*

---

### Abstract

NxTakt is a from-scratch, zero-framework digital audio workstation for Linux
— session view, arrangement timeline, piano roll, plugin hosting (LV2/CLAP +
twelve internal devices including a wavetable instrument), a
process-separated audio engine, and a GPU-native interface — built in four
days (2026-08-15 → 08-18) on top of a ten-day-old engine core, by parallel
waves of LLM agents under a single orchestrator, and released continuously
(v0.1.0 → v0.4.x) through a gated pipeline into an auto-discovering app hub.
This report describes the method that made the pace survivable:
**deterministic gates** — bit-identical audio renders, clean-binary test
suites, red-then-green regression proofs, removal tests for every mirrored
field — enforced by an orchestrator that gates every agent's work in
isolation before merging, plus **contract-first parallelism** that let two
agents build a synthesizer's DSP and editor simultaneously without reading
each other's code. We report quantitative results (≈3,400 assertions across
six suites; −75.5 dB worst-case wavetable aliasing; 0.19–4.8% of the
realtime budget for 1–16 synth voices; a 0.27 ms animated background; UI
frame ≤0.91 ms), eleven production incidents with root causes, and the
failure modes of the method itself — including a test binary whose stale
Makefile dependencies let every local gate pass against expectations the
committed source no longer met.

---

### 1. Introduction

The project brief was one sentence from the owner: a native Linux
replacement for Ableton Live, "the whole feature set … with its look and
feel," later sharpened by three constraints that shaped everything: **no
frameworks** (C++20, hand-written Makefile, no toolkit, no runtime), **use
subagents whenever possible / parallellize as much as possible**, and — for
the interface — a shared design language across the owner's app family,
applied "only where it makes sense" and required to look "professional and
EXPENSIVE."

Two properties of a DAW make it an unusually sharp testbed for multi-agent
engineering. First, its core invariant is *auditable to the bit*: an offline
render of a deterministic engine either reproduces exactly or it does not,
which turns the vaguest question in software review — "did this change
behavior?" — into `cmp`. Second, it spans radically different disciplines
(lock-free realtime DSP, GPU rendering, IPC, file formats, interaction
design) whose experts rarely coexist in one head; a wave of specialized
agents with disjoint file ownership maps onto it naturally.

### 2. System

**Engine.** A single realtime `process()` with zero allocation, zero locks,
and command/event rings crossing to the interface; sub-block launch
splitting for sample-accurate quantization; granular time-stretching;
plugin-delay compensation summing all parallel paths; a signature *map*
(bar-varying meters) honored by metronome, quantizer, and views alike.
Determinism is a design axiom: per-note probability, launch chance, and
follow actions all derive their randomness from stable identities (note
index, loop lap counter) rather than accumulated time — because accumulated
beats differ by ulps across buffer sizes, and a determinism gate at 64 and
1024 frames per block must agree.

**Process split.** `nxtaktd` hosts the engine behind shared-memory rings, a
client-writable sample pool, and a seqlock-published state mirror. In daemon
mode (opt-in, `NXTAKT_ENGINE=daemon`) clips, devices, racks-with-contents,
arrangements, signature maps, hardware MIDI, and recording all cross the
boundary; recording takes are daemon-written files whose rename is the
publication, so an unclaimed take costs a file, not RAM. Crash matrices are
tested with `kill -9` in both directions.

**Interface.** A batched SDF renderer — one shader, one vertex format, five
then nine modes — draws everything; glyphs from a FreeType atlas; the NX
design language ("liquid glass on deep space") is implemented as typed
tokens with the tier system enforced structurally (the card tier *has no
blur field*), and working surfaces (grid, roll, timeline) are deliberately
flat and fast while chrome takes the glass. A living nebula background costs
five quads, one draw call, 0.27 ms.

**Instrument.** Spectra, a wavetable synthesizer: two oscillators over eight
procedural tables (32 frames × 2048 samples, per-octave FFT-truncated mip
chains at ~8× the harmonic limit so linear interpolation's images sit below
−34 dB), up to 7-voice unison per oscillator, 16-voice polyphony, SVF filter,
two envelopes, transport-synced LFO, twelve factory presets.

### 3. Method

**Waves of disjoint ownership.** Work proceeds in waves of 2–4 agents whose
file ownership is spelled out to the file level and disjoint by
construction. The orchestrator holds contested files, applies agents' filed
diffs to files they may not touch, and is the only party that commits.

**The gates.** An agent's report is treated as a claim, not a result. Every
wave is gated by the orchestrator, usually in an isolated worktree (HEAD +
only that agent's diff), against: (1) zero compiler warnings; (2) the full
suite from *freshly deleted* test binaries; (3) four demo-scene renders
`cmp`-identical to a read-only, commit-stamped baseline; (4) for every bug
fix, a red-then-green proof — the discriminating test run against the
reverted code and watched failing; (5) for every field mirrored across the
process boundary, a *removal test* — delete the write, watch the check go
red — because an unwritten field reads as a plausible zero, not as missing;
(6) for UI work, headless-compositor screenshots (never the developer's
desktop), with bit-identical-screenshot gates where "nothing changed for
existing sets" is the claim.

**Contract-first parallelism.** For the synthesizer, the orchestrator froze
a 42-parameter contract (ids, ranges, units, table names, preset names) in a
document before either agent started; the DSP and editor agents built
against the document simultaneously and met only there. The editor shipped
guarded against *any* device (a 13-parameter effect renders inert sockets
and an amber explanation), which made the contract testable before its
implementation existed.

**Owner in the loop, at the right altitude.** The owner never reviewed
diffs; they reviewed *screenshots and releases* and issued verdicts in
plain language ("cheap AF," "all that roundedness," "buttons don't belong
together," "light rides the tilt"). Each verdict was translated into token-
level changes within hours and then *codified upstream* into the design
spec (v1.1 "angular, never rounded"; v1.3 "light rides motion"), so the
correction outlived the incident.

### 4. Results

#### 4.1 Scale and pace

| release | date | LOC (src+tests+tools) | artifact | suites (total checks) |
|---|---|---|---|---|
| v0.1.0 | 08-15 | 61,767 | 9.78 MB | 2,964 |
| v0.2.0 | 08-15 | 65,214 | 10.04 MB | 2,964 |
| v0.3.0 | 08-16 | 71,860 | 10.25 MB | ~3,150 |
| v0.4.0 | 08-18 | 75,989 | 10.68 MB | 3,430 |
| v0.4.1 | 08-18 | ~77,600 | — | 3,355* |

\* v0.4.1's count is lower than v0.4.0's despite growth because the audit
retired duplicated coverage while adding 25 checks; suite totals are an
imperfect proxy and reported as such.

Six suites at v0.4.1: engine 708, IPC 134, daemon 713, internal devices
597, view-grid-vs-engine 1,020, engine-handle 183. The 1,020-assertion
suite exists to pin one property: every bar line the view draws is a
downbeat the engine would play — mutated by 10⁻⁷ it fails 624 of them.

#### 4.2 Audio quality (measured in-suite, gated)

| claim | measured | gate |
|---|---|---|
| Spectra aliasing, C7, worst of 8 tables × 2 positions | **−75.5 dB** | −60 |
| position-sweep alias energy at C7 | −128.8 dB | −60 |
| 7-voice unison mono-sum retention | 87.5% | ≥50% |
| block-size determinism (1,7,64,300,1024 vs 256) | bit-identical | exact |
| tempo-synced LFO vs free-running equivalent | bit-identical | exact |
| release tail termination | −102 dB, monotone | no step |

A within-family comparison on a single off-harmonic-energy metric: Spectra's
mip-chain oscillator scores −36.6 dB where the older PolyBLEP instrument
scores −18.3 dB — an 18 dB improvement attributable to the table pipeline.
A planned cross-vendor comparison was **discarded as invalid**: the naive
harmonicity metric scores deliberate detune (supersaw), FM inharmonicity
(DX-class patches), and vibrato as "impurity," i.e. it measures patch
design, not aliasing; and the block-size-determinism differentiator we
expected against third-party plugins did not materialize on the two mda
synths tested (both bit-identical). Negative results reported as such.

#### 4.3 Performance

| measurement | value |
|---|---|
| Spectra, 1 voice (Init) | 10.0 µs/block = 0.19% of realtime budget |
| Spectra, 16-note chord (Warm Pad, worst) | 256 µs = 4.8% |
| UI frame, demo set, full NX skin | 0.73–0.91 ms |
| living background pass | 0.27 ms (5 quads, 1 draw call) |
| draw calls, demo session view | 176 (pre-skin) → 123 |
| draw calls, 32×32 stress set | 542 → 256 |
| automation lane, 96 points | ~1,300 quads → ~192 |

The one measured performance regression of the design-language work — frame
1.09–1.20 ms — was traced to the gradient-stop table re-uploading on every
batch (~8 µs of driver sync each); making the table persistent restored
0.74–0.91 ms. The diagnosis was an agent's; the fix predated its filed task.

#### 4.4 Interaction usability (measured, then fixed)

The usability pass computed every interactive rectangle against floors
(≥8 px drag edges, ≥16 px clickables) at two DPI scales, then *drove* 23
gesture classes via a scripted compositor and read back the model. Selected
findings: arrangement trim zones measured 5 px (fixed to 11 with 3 px
outside slop); the loop brace's ends were draggable in no pixels at all
(zone size zero — the gesture did not exist); and on any item narrower than
28 px the fade-out corner sat entirely inside fade-in's rectangle, so
grabbing a short clip's tail **silently edited its head** — reproduced
before/after with the same script. A cursor-badge system (＋/pen/split/×)
now marks affordances that controls cannot self-describe, under the rule
that a badge answers "what will a click do here" only where the answer is
not obvious.

### 5. Incidents

Thirteen production incidents, each of which changed the method:

1. **The stale binary hid the stale test.** `build/ipc_test` depended on one
   of four headers; a protocol bump elsewhere never triggered a rebuild, so
   every local gate — including the authoring agent's and the orchestrator's
   — passed a binary whose expectations the committed source no longer met.
   Three CI runs stayed red while local stayed green. *Consequence:* test
   rules list every header; gates run from deleted binaries; CI emits
   failing checks as publicly readable annotations.
2. **A scheduler bet in a test.** An assertion required observing a seqlock
   mid-publish from another process; on a one-CPU runner the observer only
   runs while the writer is parked, and 122 million samples saw zero.
   *Consequence:* statistical observations demoted to telemetry; the
   deterministic stall-injection control is the proof.
3. **Shared-scratchpad clobbering** (three times): baselines overwritten by
   sibling agents. *Consequence:* commit-stamped, write-protected baseline
   directories; per-agent namespaces.
4. **A zombie is alive by `/proc`.** A daemon child killed with SIGKILL kept
   its `/proc` entry (nobody waited); the GUI reported it alive forever.
   *Consequence:* `waitpid(WNOHANG)` in the poll; "process gone" is the one
   auto-actionable state.
5. **The invisible-field class.** Mirrored shared-memory fields that nobody
   writes read as plausible zeros (four dead meters, a playhead ignoring
   latency). *Consequence:* the removal-test rule, applied ~40 times since.
6. **Voice-stealing nondeterminism.** Applying MIDI at arrival made
   different block sizes steal different voices, because stealing reads
   envelope state and `midi()` arrives once per block. *Consequence:* note
   events queue and apply at their stamped sample.
7. **A latent cross-platform font bug** found by the Windows port: FreeType
   MONO bitmap strikes (1 bit/px) memcpy'd as 8-bit rows — six bytes read
   from one-byte rows. Invisible on Linux only because default fonts ship no
   strikes.
8. **Shadow through glass.** SDF drop shadows painted inside their own
   border box showed through 9%-alpha fills; the shader now reconstructs and
   excludes the box.
9. **Comment rot as hazard.** The engine header's threading contract named a
   method that never existed in the tree; an enumerator's comment declared a
   capability absent two waves after it shipped. Thirteen such corrected in
   one pass; a wrong comment is worse than none because readers plan against
   it.
10. **A cancelled take's buffer leaked for six waves** behind the comment
   "there is no buffer to hand back" — the arm had already handed one over.
   Two tests *codified* the leak ("cancelling is silent") and were corrected
   along with the code.
11. **Two individually correct fixes, one critical bug.** The engine
   learned to hand a cancelled take's buffer back (fixing a six-wave leak);
   that silently invalidated the first premise of the daemon's
   cancel-inference ("no finish event can be in flight"), opening a path to
   a live recording's buffer being freed under the appending audio thread —
   via same-size reallocation returning the same address to a
   match-by-pointer table. Both changes were individually tested and
   individually right; no per-change gate can see a property that only
   exists in their composition. Found by the third adversarial audit;
   fixed with a fourth premise that is a proof (an observed-empty event
   ring), not a delay. *Consequence:* audits are an institution, not an
   event — every N waves, hostile eyes re-read the seams that recent
   changes touch.
12. **Host-process loss mid-wave.** Three agents died with a restart; two
   resumed from transcripts, one's transcript was lost and its working-tree
   diff was recovered, gated exactly as hard as reported work, and committed
   with its provenance stated.
13. **The friendly-fire stash.** Mid-wave, files reverted to HEAD under two
   agents at the same minute — one had a hero panel snap back to an
   illustration under its own editor, another lost seven of its eight files.
   Both denied running git write operations, truthfully. The cause confessed
   in a third agent's final report: `git stash push/pop` cycles used to
   bisect an unexplained test-count delta — each stash window exposed every
   OTHER agent's uncommitted work at HEAD, and whoever read a shared file
   during the window read the wrong tree. The delta being bisected was
   itself another agent's legitimate work arriving in a shared test file.
   *Consequence:* the no-git-write rule during waves now covers *reads
   through the working tree* — `git show HEAD:path` is the only sanctioned
   way to see the baseline; agents keep byte-verified mirrors of their owned
   files (`*-mine/`), restore by file copy only, and the orchestrator
   re-verifies every mirror against the tree at the gate. The wave's five
   agents were gated and merged despite the incident; the mirrors are why.

### 6. Discussion

**What the gates buy.** The renders-bit-identical gate converts the hardest
review question into a mechanical one and makes large refactors (a quality
pass touching a 4,000-line engine core) safe to run *unattended overnight*.
The red-then-green and removal-test rules convert "trust me" reports into
artifacts; in practice roughly one agent report in three contained at least
one claim the gate falsified or sharpened.

**What they don't.** Gates verify preservation, not taste. Every design
judgment that mattered — the toy-like radii, the purple wash, capsule
controls — was caught by the owner's eye on a screenshot, not by any check;
the method's answer is to codify each verdict into tokens and specs so it
is caught *next* time. Gates also share blind spots with their fixtures:
incident #1 is precisely a gate trusting a fixture that lied.

**Agent disagreement as signal.** The strongest moments were agents pushing
back: refusing a seqlock recipe as insufficient and proving why; declining
to fake a test whose failure was the product's own bug; discarding a
requested comparison as methodologically invalid. An orchestration that
punished deviation would have shipped all three mistakes.

### 7. Limitations

Single owner, single machine, one model family; four days is an existence
proof, not a controlled study. The Windows port has reached first light
under Wine only. Daemon mode is feature-complete but soaking as opt-in.
LLM-written LOC is an input measure, not value; we lean on the assertion
count, the measured DSP quality, and the shipped releases instead.

### 8. Conclusion

A professional-grade DAW — the genre of software usually measured in
team-decades — sustained four days of multi-agent development at roughly a
release per day without a single regression escaping to a user, because
every claim that could be made mechanical was: rendered audio to the bit,
suites from clean binaries, proofs run red before green, fields proven by
their own removal. The method's residue is the repository itself: every
incident above is written down where the next reader — human or agent —
will trip over it.

---

*Artifacts: github.com/nerdrx/nxtakt (source, releases, CI), docs/ (specs,
audits, contracts), this paper at docs/PAPER.md.*
