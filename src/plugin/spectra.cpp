// Spectra — NxTakt's wavetable instrument.
//
// The parameter list, the eight table names and the modulation routing are
// FROZEN in docs/SPECTRA-PARAMS.md. That file is the interface between this
// code and the editor in src/ui; where the two disagree, this file is wrong.
// Nothing here may reorder, rename or remove a parameter, because ids are
// indices and indices persist in saved sets.
//
// ---------------------------------------------------------------------------
// HOW IT IS BUILT, in the order the signal travels
//
//   note -> voice (up to 16, param 39, quietest-stolen)
//     glide (constant time, param 38) -> pitch
//     osc A + osc B, each: table (0..7) x position (frame morph) x up to 7
//       unison phases, detuned symmetrically and panned across the fan
//     noise (white, per voice) + sub (sine, one octave below, follows glide)
//     -> tanh drive -> TPT state-variable filter (LP/BP/HP) -> ENV1 as the VCA
//   ENV2 modulates cutoff and position; one instance-wide LFO modulates
//   position, cutoff and pitch.
//
// ---------------------------------------------------------------------------
// THE WAVETABLES
//
// All eight are PROCEDURAL and are generated exactly once per process, into an
// immutable set shared by every instance and never freed. 32 frames per table;
// each frame is band-limited into a mip chain by FFT truncation, so a frame is
// stored ten times over at ten harmonic limits rather than once.
//
// A frame is generated as a HARMONIC SPECTRUM, either analytically (the three
// additive tables) or by rendering the waveform into 4096 time samples and
// taking its forward FFT (the five shaped tables). 4096 is four times the
// harmonic limit we keep, so the harmonics that a naive render aliases live at
// 1024..2047 and are DISCARDED rather than folded back onto the ones we keep.
// DC is dropped unconditionally: a table with an offset thumps on every note.
//
// The mip chain, and the one number in it that is not obvious:
//
//   level m keeps harmonics 1..(1023 >> m), and is STORED at a length of
//   2048, 2048, 2048, 1024, 512, 512, 512, 512, 512, 512.
//
// The lengths are not 2*harmonics. They are eight times the harmonic limit
// wherever that fits in 2048, because the interpolator is LINEAR and a linear
// interpolator reconstructing L samples per cycle passes an image of harmonic h
// at (L - h) attenuated by only sinc^2((L-h)/L). With L = 2*H that factor is
// -4 dB at the top harmonic; with L = 8*H it is -34 dB, and since the top
// harmonic of a musical table is itself 30-50 dB down, the images land below
// -70 dB where they belong. Doubling the storage of the top three levels is
// what buys that, and it is the difference between "band-limited" as a claim
// and as a measurement (see the C7 aliasing test).
//
// Level 0 is the exception: 1023 harmonics cannot be 8x oversampled inside
// 2048 samples, so it is 2x and its images sit a few hundred Hz below Nyquist.
// It is only ever selected below 23.4 Hz — one octave below the lowest note on
// a piano — because the selector picks the coarsest level that is still
// alias-free, and everything above that runs on level 1 or higher.
//
// SELECTION. Level 0 is safe while 1023*inc < 0.5, level m while
// (1023>>m)*inc < 0.5, so the first safe level is ceil(log2(2048*inc)) and the
// mip coordinate is log2(4096*inc): its floor is that level and its fraction
// crossfades towards the next coarser one. At an integer coordinate the chosen
// level sits exactly at the aliasing limit, which is the brightest it is
// allowed to be, so the scheme never throws away an octave it did not have to.
// The level is chosen from the HIGHEST inc in a unison fan, not the centre one,
// so detune can never push a voice past the limit its mip was picked for.
//
// Interpolation is linear in all three axes, as the contract says: two samples
// per frame, two frames per mip, two mips.
//
// THE EDITOR DRAWS THESE, and the bottom of this file is how. spectraTables()
// (declared in internal_base.h) hands out a const view of the set once the
// first prepare() has built it, so the panel's hero display is a read of the
// same floats the voices read rather than a picture of the same idea. It adds
// no lock, no copy and no mutation: the set was already immutable and already
// shared, and the accessor only says where it is.
//
// ---------------------------------------------------------------------------
// DETERMINISM, which is a gate and not an aspiration
//
//   * Every random number comes from a per-instance counter seeded to a fixed
//     value in prepare(). Never a clock, never an address.
//   * There are TWO such counters -- one for note-on phase randomisation, one
//     for the sample-and-hold LFO -- so that the two streams cannot be made to
//     interleave differently by anything. Both are drawn inside process() at
//     absolute sample positions, which is the other half of the same property.
//   * NOTE EVENTS ARE QUEUED AND APPLIED AT THEIR OWN SAMPLE, not when midi()
//     hands them over. Voice stealing picks the quietest voice, so allocation
//     depends on envelope state, and midi() is called once per block. See the
//     note on midi() itself.
//   * The control-rate tick (kCtrl) counts down across process() calls on
//     ABSOLUTE sample time, exactly as the Auto Filter's does and for exactly
//     its reason: blocks of 1, 7 and 300 must be bit-identical to blocks of
//     256, because a render must not depend on the buffer size it was made at.
//   * The LFO phase advances one sample at a time (dsp::Lfo::tick documents
//     why), and its tempo sync sets a RATE. It deliberately does not derive its
//     phase from the pushed beat position: the transport arrives once per
//     block, so a phase read from it would quantise to the block boundary and
//     destroy the property above. Rate-synced is what the Auto Filter does too.
//
// ---------------------------------------------------------------------------
// V2 — THE PARITY PUSH (ids 42..99), and how the v1 gates were kept
//
// The contract's own gate: every new parameter at its default reproduces v1
// output EXACTLY. The discipline that buys it is visible all through the
// voice: wherever v2 adds arithmetic, the v1 EXPRESSION is kept on its own
// branch and the new one is only taken when a v2 parameter has left its
// default (warp mode 0 / amount 0 is the v1 read; a destination the matrix
// does not reach keeps the v1 formula, selected by a per-block bitmask; sub
// shape 0 is the v1 sine and Sub Oct's default multiplier is 0.5f exactly;
// noise color 1.0 bypasses the color filter by branch, not by neutrality).
// Floating point makes "mathematically equal" and "bit-identical" different
// claims, and the gate is the second one.
//
// Determinism grew three obligations and kept the old rules:
//   * LFO2/3 mirror LFO1 sample-by-sample: own phase, own S&H stream from an
//     own fixed-seed counter, rate from the pushed transport when synced.
//   * The matrix's Random source is NOT a stream at all: it is a hash of the
//     note's stable identity (channel, note, the note-on's absolute timeline
//     sample as stamped on the event), so it cannot be perturbed by voice
//     stealing, block size, or anything drawn before it.
//   * Aftertouch (channel pressure) goes through the same event queue as
//     notes and lands at its stamped sample.
// Matrix modulation of cutoff, resonance and LFO rates applies at the
// control tick (absolute-time, kCtrl), exactly as v1's LFO->cutoff does; all
// other destinations are evaluated per voice per sample.
//
// Mutual FM/RM is well-defined the way the contract says: the modulator tap
// is the other oscillator's voice 0, read at its RAW phase (pre-warp,
// pre-level, mono) and DELAYED ONE SAMPLE — each sample both oscillators read
// last sample's taps, then both write this sample's, so A<->B has no
// evaluation order and block boundaries cannot move it.
//
// ---------------------------------------------------------------------------
// V3 — HANDS ON THE MODULATION (ids 100..110, the spent reserved ids 60/61/99,
// three widened enums and the state string this device never had)
//
// The contract's gate is v2's with one named exception: a v1 or v2 state fed a
// stream a v1/v2 build could act on renders BIT-IDENTICAL, and the exception is
// PITCH BEND, whose bytes older builds discarded unread and which now moves
// pitch through Bend Range (id 99, default 2 st). Everything else v3 adds is
// selected by a parameter whose default is the v2 behaviour or by a state block
// whose absence is the v2 behaviour, so the same discipline v2 wrote holds here:
// where v3 adds arithmetic the v2 EXPRESSION is kept on its own branch and the
// new one is only taken when a v3 feature has actually left its default —
// matrix curve 0 is SELECTED and not a multiply by 1; a smooth of 0 is a
// no-filter branch and not a coefficient of 1; a bend of 0 semitones is not
// added to the pitch, it is branched around.
//
// The four things v3 grows, and the one sentence each that is load-bearing:
//
//   * DRAWABLE LFOs (shape 5). A 16-step UNIPOLAR grid with a per-LFO smooth,
//     both in the state string rather than in parameters. Unipolar because
//     sixteen levels cannot be symmetric about an exact zero and this document's
//     spine is that a default of zero means no effect: an all-zero grid must be
//     silence. The step index is floor(p*16) of the SAME phase the other five
//     shapes read, so which cycle the LFO is in and how it got there is
//     unchanged by this shape existing.
//   * ONE-SHOT LFOs (ids 60/61/100). Loop is v2, verbatim and instance-wide.
//     One-shot makes the LFO PER VOICE: phase 0 at the note-on's stamped sample,
//     clamped at 1.0, retriggered exactly when ENV1 is and never otherwise. Sync
//     sets the SPEED in one-shot and not the alignment — a one-shot locked to
//     the bar line would not be an envelope.
//   * MIDI AS A SOURCE. Mod wheel (CC 1), pitch bend, and one learned CC whose
//     number is state. All three are QUEUED and applied at their stamped sample,
//     through the same queue as notes and channel pressure and for the identical
//     reason: applying them when midi() is called would make the same MIDI in
//     blocks of 1 and of 1024 produce different audio.
//   * PER-SLOT MATRIX CURVES (ids 101..108). x, x*x and x*x*(3-2x) — multiplies,
//     never a pow or an exp, so there is no libm version in a modulation path.
//     Applied symmetrically about zero, so a bipolar source stays bipolar.
//
// THE TRANSPORT AND THE STEP INDEX — the orchestrator's ruling, recorded here
// because the contract's obligation 1 OVERSTATES the shipped behaviour and this
// file is where a reader will look for the truth.
//
// The obligation says a synced Loop LFO's phase is `frac(beat / beatsPerCycle)`.
// spectra.cpp does not do that and never has: it RATE-syncs — the LFO
// accumulates at a beat-derived rate — which the DETERMINISM section above has
// documented since v1, because the transport arrives once per block and a phase
// read straight from it quantises to the block boundary. The ruling, in four
// lines:
//
//   1. shapes 0..4 in Loop keep the shipped rate-sync, bit-identical. They are
//      not phase-locked, and nothing here may make them so.
//   2. the NEW Custom shape (5) DOES derive its step index from the transport
//      beat when synced — a step sequencer that does not lock to the grid is
//      not a step sequencer, and a shape that did not exist yesterday carries
//      no bit-identity constraint.
//   3. with no transport running it falls back to accumulated phase, so a
//      preset authored at 120 BPM with no transport still runs.
//   4. One-shot is per voice and phase-0-at-note-on, untouched by all of it.
//
// HOW (2) STAYS BLOCK-SIZE INVARIANT, which is the reason the plain reading is
// unbuildable. `beatAcc_` is an f64 beat counter advanced ONE SAMPLE AT A TIME
// at the pushed tempo — absolute-timed, exactly like ctrl_ and like every LFO
// phase in this file — and it is ANCHORED to the pushed transport beat, not
// driven by it. It anchors when the transport starts, and again whenever a host
// that is demonstrably advancing its beat (the pushed value changed since the
// last block) disagrees with the counter by more than 1/64 of a beat, which is
// a locate or a loop wrap and nothing else. A truthful host therefore locks the
// grid to its bar line and never re-anchors again; a host that pushes a
// constant beat — an offline render, a test harness — anchors once and free
// runs, which is case (3). The beat sets the ORIGIN, the tempo sets the RATE,
// and neither of them is read at a block boundary.
//
// REALTIME. process() and midi() allocate nothing, lock nothing, throw nothing
// and call nothing that could. The only allocation in the device is the shared
// table set, built on the GUI thread at the first prepare() in the process.
//
// This file is #included by internal_devices.cpp rather than compiled on its
// own -- see the guard below.
#ifndef LAT_SPECTRA_IN_INTERNAL_DEVICES

// Compiled standalone. The GUI's Makefile sweeps every src/**/*.cpp into its
// object list, so this file is handed to the compiler on its own as well as
// through internal_devices.cpp; building the instrument twice would put a dead
// copy in the binary and warn about every helper in it. An empty translation
// unit is the honest answer until the tool and test recipes list this file
// explicitly, at which point the include becomes a declaration and this guard
// goes away.
namespace lat { namespace detail { /* see internal_devices.cpp */ } }

#else

#include "host.h"
#include "internal_base.h"
#include "internal_dsp.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace lat {
namespace detail {
namespace {

// The wavetable pipeline: geometry, the FFT, the eight procedural tables, the
// mip chains and the set the editor draws. Cut into a file of its own so a
// table wave and a voice wave can own one each; its header says why it is an
// include and not a translation unit.
#include "spectra_tables.inc"

// ---------------------------------------------------------------------------
// THE CUSTOM-WAVETABLE SEAM (docs/SPECTRA-PARAMS.md, "Custom wavetable slots")
//
// spectra_tables.inc, above, owns the table pipeline and hands this file six
// functions and one handle allocator. This file only ever CALLS them; it never
// defines them, which is the whole of the two waves' file split.
//
//   spTableBase(slot, osc)   audio thread; slot 0..7 factory, 8 this
//                            oscillator's import, nullptr = unresolved
//   spTableFor(slot, osc)    the same with the refusal contract's fallback
//                            already applied — factory table 0 on a null
//   spResolveCustom / spImportWavetable / spCustomHash / spCustomPath /
//   spClearCustom            GUI thread; identity, import and teardown
//   spAcquireOsc / spReleaseOsc   the OSCILLATOR HANDLE, below
//
// `osc` IS A HANDLE, NOT 0-OR-1, and that is the one thing a reader of this
// file has to carry across the seam: a custom table belongs to one oscillator
// of ONE device, so two Spectras in a set that import two different files onto
// their A oscillators must not overwrite each other. This device takes a pair
// at construction and gives them back at destruction; oscH_[0] is A's and
// oscH_[1] is B's, and -1 means the process ran out and this instance plays
// factory tables only.
//
// THE RECORD IS THE SEAM'S. spResolveCustom() stores the hash and the path it
// was given BEFORE it tries to find the file and keeps them whatever happens,
// so `spCustomHash(h)` answers "what does this oscillator NAME" and not "what
// did it manage to load". That is what lets stateString() re-emit a missing
// file's name verbatim, which is the refusal contract's own requirement.
//
// ---------------------------------------------------------------------------
// THE PATH ESCAPER IS THE SAMPLER'S, SHARED AND NOT COPIED.
//
// The contract says the escaping is "verbatim the sampler's" and that the
// implementation is "expected to share the sampler's helpers rather than write
// a second escaper". Verbatim IS the same function, so these are declarations
// and not definitions: internal_devices.cpp includes this file and then
// sampler.cpp into ONE translation unit, and both open the same
// `lat::detail::{anonymous}`, so the definitions arrive a few hundred lines
// below and the linker never sees a second one. A copy would be a second
// definition of what a path is, and the two would drift on the day one of them
// is fixed.
// ---------------------------------------------------------------------------

bool smNeedsEsc(unsigned char c);
void smEsc(std::string& o, const std::string& s);
bool smUnesc(const std::string& s, std::string& out);
std::vector<std::string> smSplit(const std::string& s, char sep);

// ---------------------------------------------------------------------------
// Small realtime helpers
// ---------------------------------------------------------------------------

// log2 to about 1e-5, with no libm call and no table. Used once per oscillator
// per sample to place the mip coordinate, where a call would show up.
// Deterministic: a pure function of the bit pattern.
f32 spLog2(f32 x) {
    if (!(x > 0.f)) return -60.f;
    u32 bits;
    std::memcpy(&bits, &x, sizeof bits);
    const f32 e = (f32)((int)((bits >> 23) & 0xFFu) - 127);
    bits = (bits & 0x007FFFFFu) | 0x3F800000u;          // mantissa into [1, 2)
    f32 m;
    std::memcpy(&m, &bits, sizeof m);
    return e + (-1.7417939f + (2.8212026f + (-1.4699568f +
               (0.44717955f - 0.056570851f * m) * m) * m) * m);
}

// One mip level, one frame, linear between two samples. `phase` is 0..1; the
// mask makes the wrap from the last sample back to the first free.
inline f32 spReadMip(const f32* base, int len, f32 phase) {
    const f32 x  = phase * (f32)len;
    const int i  = (int)x;
    const f32 fr = x - (f32)i;
    const u32 mask = (u32)len - 1u;
    const f32 a = base[(u32)i & mask];
    const f32 b = base[((u32)i + 1u) & mask];
    return a + (b - a) * fr;
}

// The full read: linear across sample, frame and mip, exactly as the contract
// says. Eight loads and seven lerps.
inline f32 spRead(const f32* tbl, int f0, int f1, f32 ff, int m0, f32 mf, f32 phase) {
    const f32* fa = tbl + (size_t)f0 * kSpStride;
    const f32* fb = tbl + (size_t)f1 * kSpStride;
    const int  l0 = kSpLen[m0], l1 = kSpLen[m0 + 1];
    const int  o0 = kSpOff[m0], o1 = kSpOff[m0 + 1];

    const f32 a0 = spReadMip(fa + o0, l0, phase);
    const f32 b0 = spReadMip(fb + o0, l0, phase);
    const f32 s0 = a0 + (b0 - a0) * ff;

    const f32 a1 = spReadMip(fa + o1, l1, phase);
    const f32 b1 = spReadMip(fb + o1, l1, phase);
    const f32 s1 = a1 + (b1 - a1) * ff;

    return s0 + (s1 - s0) * mf;
}

// ---------------------------------------------------------------------------
// Parameter ids. These ARE the contract; the names below are what the editor
// looks up and what the mechanical test in tests/internal_device_test.cpp
// compares against a transcription of the doc.
// ---------------------------------------------------------------------------

enum : int {
    kPATable = 0, kPAPos, kPACoarse, kPAFine, kPALevel, kPAUni, kPADet, kPASpread,
    kPBTable,     kPBPos, kPBCoarse, kPBFine, kPBLevel, kPBUni, kPBDet, kPBSpread,
    kPNoise = 16, kPSub,
    kPCutoff = 18, kPRes, kPFType, kPDrive, kPE2Cut, kPKeytrack,
    kPAttack = 24, kPDecay, kPSustain, kPRelease,
    kPE2Attack = 28, kPE2Decay, kPE2Sustain, kPE2Release,
    kPLfoRate = 32, kPLfoSync, kPLfoPos, kPLfoCut, kPLfoPitch, kPLfoShape,
    kPGlide = 38, kPVoices, kPMaster, kPE2Pos,

    // --- v2, ids 42..99 (docs/SPECTRA-PARAMS.md, "v2 — the parity push").
    // Reserved ids are registered (name "—", 0..1, default 0) and never read.
    kPSubShape = 42, kPSubOct, kPNzColor, kPNzTrack,
    kPAWarp = 48, kPAWarpAmt, kPBWarp, kPBWarpAmt,
    kPL2Rate = 54, kPL2Sync, kPL2Shape, kPL3Rate, kPL3Sync, kPL3Shape,
    kPE3Attack = 62, kPE3Decay, kPE3Sustain, kPE3Release,
    kPM1Src = 68,                    // slot k: ids 68+3k / 69+3k / 70+3k
    kPMacro1 = 94, kPMacro2, kPMacro3, kPMacro4,
    kPVoiceMode = 98,

    // --- v3. Three of v2's reserved ids become functional and the block
    // appends 100..110 (docs/SPECTRA-PARAMS.md, "v3 — hands on the
    // modulation"). L2/L3 Mode land on 60/61 because those two reserved ids sit
    // inside the block that owns those LFOs; L1 Mode cannot join them, because
    // LFO1's parameters have lived outside that block since v1, so it opens the
    // append instead. The per-slot curves could not fit in 92/93 — an id ARRAY
    // must be contiguous and two ids cannot hold eight — so 92/93 stay reserved.
    kPL2Mode = 60, kPL3Mode = 61,
    kPBendRange = 99,
    kPL1Mode = 100,
    kPM1Curve = 101,                 // slot k: id 101 + k

    // --- v4, the arpeggiator (docs/SPECTRA-PARAMS.md, "v4 — the
    // arpeggiator"). It spends v3's generic reserved tail (109, 110) and
    // appends 111..124, so the whole feature is ONE contiguous run 109..124
    // with no hole and no lookup table. The other eight reserved ids stay
    // where they are: a reserved id belongs to the block it sits in, and an
    // arpeggiator is not a sub, a warp, an ENV3 or a matrix.
    //
    // Both spent ids satisfy the v2 condition without strain — Arp On
    // defaults to 0 and 0 is off, Arp Mode defaults to 0 and 0 is Up — so a
    // v3 `.nxp` file, which writes every id including the reserved ones as 0,
    // lands on exactly the v4 defaults.
    kPArpOn = 109, kPArpMode = 110,
    kPArpRate = 111, kPArpSync, kPArpOctaves, kPArpOctMode, kPArpGate,
    kPArpSwing, kPArpHold, kPArpRetrig, kPArpVelMode, kPArpFixedVel,
    kPArpSteps, kPArpChance,          // ...122; 123 and 124 are the tail
    kSpParamCount = 125
};

// The matrix enums, verbatim from the contract's source and destination lists.
// v3 appends three MIDI sources and v4 appends one more; the destination enum
// does NOT widen, for the third revision running (nothing v3 or v4 adds is a
// modulatable target — a drawn grid is a shape, a bend range is a performance
// calibration, a curve is the slot's own response, and the two v4 candidates
// are refusals rather than omissions: a modulated Arp Rate cannot stay locked
// to a bar line, and Arp Gate is consumed once per step at a stamped sample
// rather than read at audio rate).
enum : int {
    kSOff = 0, kSLfo1, kSLfo2, kSLfo3, kSEnv2, kSEnv3, kSVel, kSKey, kSAft,
    kSMac1, kSMac2, kSMac3, kSMac4, kSRandom,
    kSWheel, kSBend, kSCC, kSArpStep, kSrcCount
};

// The three matrix response curves (ids 101..108). f(0) = 0 and f(1) = 1 for
// all three, so a curve can never make an idle source contribute and never
// changes a full-scale source's reach. Linear is a SELECTED branch and never
// reaches here — that is what makes the bit-identity gate hold.
enum : int { kCvLinear = 0, kCvExp, kCvS };

// `u` in its source's own domain. A unipolar source has u >= 0, for which
// sign(u)*f(|u|) IS f(u), so the contract's two rules are one expression.
inline f32 spCurve(int c, f32 u) {
    const f32 x = u < 0.f ? -u : u;
    const f32 f = (c == kCvExp) ? x * x : x * x * (3.f - 2.f * x);
    return u < 0.f ? -f : f;
}

// The LFO mode enum (ids 60/61/100).
enum : int { kLfoLoop = 0, kLfoOneShot };

// The drawn grid: 16 steps, one hex digit each, level d/15 — digit 0 is exactly
// 0.0 and digit 15 is exactly 1.0.
constexpr int kSpSteps = 16;
inline f32 spStepLevel(u8 d) { return (f32)d * (1.f / 15.f); }
enum : int {
    kDOff = 0, kDAPos, kDBPos, kDAWAmt, kDBWAmt, kDALvl, kDBLvl, kDAPitch,
    kDBPitch, kDSub, kDNoise, kDCut, kDRes, kDDrive, kDADet, kDBDet, kDPan,
    kDL1Rate, kDL2Rate, kDL3Rate, kDstCount
};

// ---------------------------------------------------------------------------
// v4: the arpeggiator's own enums and its two grid rows.
//
// The MODE enum (id 110) is append-only forever. `M(mode, c)` is the cycle
// length over a note set of size c and the element at cycle position j is the
// note the step sounds, before the octave cycle and the step row's octave
// offset are added.
enum : int {
    kArpUp = 0, kArpDown, kArpUpDownInc, kArpUpDownExc, kArpDownUp,
    kArpAsPlayed, kArpRandom, kArpChord, kArpThumb, kArpPinky, kArpModeCount
};

// Vel Mode (119) and Oct Mode (114).
enum : int { kArpVelPlayed = 0, kArpVelFixed, kArpVelPattern };
enum : int { kArpOctUp = 0, kArpOctDown, kArpOctAlt };

// A note is held through at most 16 consecutive tie steps; the 17th forces the
// note off at its own onset. Unreachable in practice — a pattern whose every
// step is a tie sounds nothing at all, because no step ever STARTS a note — so
// this exists only so that no reading of the grid can produce an unbounded
// note, the one failure here a user could not recover from by releasing a key.
constexpr int kArpMaxTie = 16;

// THE STEP ROW'S AUDIO-SIDE PACKING, and why it is four bits and not five.
//
// The wire format is a byte per step: on (bit 0), a biased three-bit octave
// code (bits 1..3, offset = code - 2), tie (bit 4), three reserved bits. After
// masking the reserved bits off and clamping the octave code to 0..4 — the
// contract's "degraded, not refused" — a step has exactly FIFTEEN reachable
// states: five octave codes for a rest, and five more for each of on-untied
// and on-tied. Tie is "read only when Step On = 1", so a rest has no tie bit;
// its OCTAVE is kept, because turning a step off and on again must not lose
// the octave the author drew on it.
//
// Fifteen fits in a nibble, so sixteen steps fit in ONE u64 and the audio
// thread takes the whole row in a single atomic load and can never see it
// half-updated. That is v3's gridBits_ argument, applied to a row v3's
// four-bit packing would not otherwise have held.
//
//   code 0 + c     rest, octave code c                (c = 0..4)
//   code 5 + c     on, octave offset c-2, no tie
//   code 10 + c    on, octave offset c-2, tie
inline u8 spArpPack(u8 raw) {
    int c = (int)((raw >> 1) & 7u);
    if (c > 4) c = 4;                                   // 5, 6, 7 clamp to +2
    if (!(raw & 0x01u)) return (u8)c;                   // rest; tie is not read
    return (u8)((raw & 0x10u) ? 10 + c : 5 + c);
}
// The byte a v4 build writes back for a packed nibble: reserved bits 0, octave
// code un-clamped because it was clamped on the way in.
inline u8 spArpUnpack(u8 code) {
    if (code < 5)  return (u8)((u32)code << 1);
    if (code < 10) return (u8)(0x01u | ((u32)(code - 5) << 1));
    return (u8)(0x01u | ((u32)(code - 10) << 1) | 0x10u);
}
inline bool spArpOn(u8 code)  { return code >= 5; }
inline bool spArpTie(u8 code) { return code >= 10; }
inline int  spArpOct(u8 code) {
    return (code < 5 ? (int)code : (code < 10 ? (int)code - 5 : (int)code - 10)) - 2;
}
// `05` — on, octave offset 0, no tie — is the row's default byte, and it packs
// to 7. Stated as a constant because a reader who skims the bit table will
// assume the default is `01`: the octave field is BIASED, so the byte that
// means "on, unshifted, untied" is 0x05 and code 2 is offset 0.
constexpr u8 kArpStepDefault = 7;

// LFO Sync, in beats per cycle. Index 0 is "free" and is never read from here.
// 4/4 is assumed for the bar values, exactly as the Delay's table assumes it
// and for the same reason: the time signature is not on the plugin contract.
// APPEND-ONLY: the index is what a project file stores.
//
// v4 CITES this table and reads it PER STEP; v3's Custom LFO shape reads it as
// the length of the whole sixteen-step cycle. The divergence is forced and is
// argued in the contract in two places: an arp's rate control has named the
// step in every instrument ever built, and Arp Steps (121) is variable, so a
// whole-cycle reading would make the pattern-length knob a tempo knob.
constexpr int kSpSyncCount = 10;
constexpr f32 kSpSyncBeats[kSpSyncCount] = {
    0.f,          // 0 free
    16.f,         // 1 four bars
    8.f,          // 2 two bars
    4.f,          // 3 one bar
    2.f,          // 4 1/2
    1.f,          // 5 1/4
    0.5f,         // 6 1/8
    0.25f,        // 7 1/16
    2.f / 3.f,    // 8 1/4 triplet
    1.f / 3.f,    // 9 1/8 triplet
};

// ---------------------------------------------------------------------------
// THE STATE STRING (docs/SPECTRA-PARAMS.md, "The state string")
//
//   nxspc1;<key>=<value>;<key>=<value>;...
//
// One line of printable ASCII with no whitespace, no quotes and no newline —
// the sampler's shape and the rack's shape, because this tree has one spelling
// of "an opaque device state" and not three.
//
// EMPTY IS THE DEFAULT AND IS A GATE. Spectra had no state string at all before
// v3; stateString() returned {} and the project layer wrote no `state` key.
// That stays true for every set that uses no v3 state, so a v2 project round
// trips through a v3 build BYTE-identically. Nothing below is emitted unless a
// grid is drawn, a smooth is turned up, a CC is learned or a table is imported.
//
// Every number in it is either hex or a plain integer, and that is deliberate:
// smooth is stored in THOUSANDTHS rather than as `0.5` because a decimal point
// is a locale hazard (a de_DE writer emits `0,5`) and a state string that
// depends on the writer's locale is not a state string.
// ---------------------------------------------------------------------------

constexpr const char* kSpTag = "nxspc1";

// Hex, LOWERCASE ONLY on read as well as on write — strict, so that write and
// read are exact inverses and a round trip never normalises anything.
inline int spHexLo(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Exactly 16 lowercase hex digits -> u64. False means "not a value this writer
// could have produced", and the caller must then change nothing at all.
inline bool spParseHex64(const std::string& s, u64& out) {
    if (s.size() != 16) return false;
    u64 v = 0;
    for (char c : s) {
        const int d = spHexLo(c);
        if (d < 0) return false;
        v = (v << 4) | (u64)d;
    }
    out = v;
    return true;
}

inline std::string spFmtHex64(u64 v) {
    static const char kHex[] = "0123456789abcdef";
    std::string o(16, '0');
    for (int i = 15; i >= 0; --i) { o[(size_t)i] = kHex[v & 15u]; v >>= 4; }
    return o;
}

// 1..`maxDigits` decimal digits, 0..`hi`, NO LEADING ZEROS (`0` itself
// excepted). The leading-zero rule is what makes the encoding canonical: two
// spellings of the same number would make a round trip normalise.
inline bool spParseUInt(const std::string& s, int maxDigits, int hi, int& out) {
    if (s.empty() || (int)s.size() > maxDigits) return false;
    if (s.size() > 1 && s[0] == '0') return false;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    if (v > hi) return false;
    out = v;
    return true;
}

// A record key is a letter followed by letters and digits. The contract's own
// key list contains `wtA` and `wtpathB`, so the charset is the ASCII alphabet
// in BOTH cases and not the `[a-z][a-z0-9]*` its prose says — see the note in
// setStateString(). Unknown keys are skipped; a record with no `=` is refused.
inline bool spKeyOk(const std::string& k) {
    if (k.empty()) return false;
    for (size_t i = 0; i < k.size(); ++i) {
        const char c = k[i];
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const bool digit = c >= '0' && c <= '9';
        if (!alpha && !(digit && i > 0)) return false;
    }
    return true;
}

// The whole of Spectra's v3 state, in one struct so that "reset the state to
// its default" is an assignment and a parse that refuses can be thrown away
// without having touched the device.
struct SpState {
    u8  grid[3][kSpSteps] = {};      // 0..15 per step; all zero = no grid drawn
    i16 smooth[3] = { 0, 0, 0 };     // thousandths, 0..1000
    i16 cc = -1;                     // learned controller, -1 = none learned
    u64 wt[2] = { 0, 0 };            // custom table content hash per osc
    std::string wtPath[2];           // recovery hint only, never identity

    // v5. The custom table's DISPLAY NAME per oscillator, <= 64 DECODED bytes.
    // NEVER IDENTITY, never consulted in resolution, never sent over the wire.
    // Empty means "no name", and there is exactly one spelling of that: a
    // `wtnameA=` record is a record with an empty value, which this writer would
    // not produce, so clearing a name DROPS the record.
    //
    // It lives in the state and not in the seam, unlike the hash and the path,
    // and the split is the file's own: this file owns what the state SAYS and
    // spectra_tables.inc owns what the table IS. A name is not a property of a
    // table -- two devices naming the same drawn table may call it two things,
    // and the hash is the same table either way.
    std::string wtName[2];

    // --- v4: the arp's two rows. THEIR DEFAULTS ARE NOT ZERO, and that is a
    // genuine divergence from the LFO grids this feature otherwise copies
    // exactly. v3's rule is "a missing block reads as its default, and every
    // default is inert" — and for an LFO grid inert means all zeros. An
    // all-zero arp step row is an arp that plays NOTHING, which is a broken
    // default rather than an inert one. So the rows default to a usable
    // pattern and the inert switch is Arp On (109), not the grid.
    //
    // `arpSt` holds the PACKED nibble (spArpPack), not the wire byte: the
    // clamp and the reserved-bit mask are the contract's degradation and they
    // happen once, on the way in.
    u8  arpLv[kSpSteps] = { 15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15 };
    u8  arpSt[kSpSteps] = { 7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7 };   // 0x05 packed:
                                                                 // on, +0, no tie

    bool gridDrawn(int n) const {
        for (int i = 0; i < kSpSteps; ++i) if (grid[n][i]) return true;
        return false;
    }
    bool arpDrawn() const {
        for (int i = 0; i < kSpSteps; ++i)
            if (arpLv[i] != 15 || arpSt[i] != kArpStepDefault) return true;
        return false;
    }
    // "Emit nothing when no v3 feature is in use" — the round-trip gate, and
    // v4 keeps it: an arp row that is still its default is not emitted, so a
    // v2 or v3 project round-trips through a v4 build byte-identically.
    bool inUse() const {
        for (int n = 0; n < 3; ++n) if (gridDrawn(n) || smooth[n] != 0) return true;
        return cc >= 0 || wt[0] != 0 || wt[1] != 0 || arpDrawn();
    }
};

// ---------------------------------------------------------------------------
// Factory presets
//
// A preset is a name and a list of (parameter id, value). loadPreset() resets
// every parameter to its default first and then applies the list, so a preset
// is COMPLETE however short it is written: switching from any patch to any
// preset lands on the same state as loading that preset into a fresh instance,
// and `Init` -- whose list is empty -- is exactly the constructor's defaults.
//
// The names are the contract with the editor and are displayed verbatim.
// ---------------------------------------------------------------------------

struct SpPreset {
    const char* name;
    // 64 overrides (the contract's cap, raised from v1's 40) plus a
    // terminator: id -1 ends the list, so the include needs no counting.
    struct { int id; f32 v; } set[65];
};

// The factory bank lives in spectra_presets.inc — a textual include of
// nothing but SP_PRESET/SP/SP_END rows plus v3's three state macros, so its
// author needs the contract document and no C++. The macros exist only across
// the include.
//
// THE FILE IS INCLUDED TWICE, and that is the whole trick. A row's parameters
// are an aggregate initialiser and its state macros may appear ANYWHERE between
// SP_PRESET and SP_END, in any order relative to the SP rows (the contract says
// so, and the bank's convention of "parameters first, state after" is a
// convention and not a rule). One aggregate cannot be written out of order, so
// pass 1 builds the parameter arrays with the state macros expanding to
// nothing, and pass 2 builds the state with SP expanding to nothing. Two
// cheap passes over one file beats a row format the author has to count.
#define SP_PRESET(nm) { nm, {
#define SP(id, v)     { (id), (f32)(v) },
#define SP_END()      { -1, 0.f } } },
#define SPLFO(n, g, s)
#define SPWTA(h)
#define SPWTB(h)
#define SPWTNA(n)
#define SPWTNB(n)
#define SPCC(n)
#define SPARP(l, s)
const SpPreset kSpPresets[] = {
#include "spectra_presets.inc"
};
#undef SP_PRESET
#undef SP
#undef SP_END
#undef SPLFO
#undef SPWTA
#undef SPWTB
#undef SPWTNA
#undef SPWTNB
#undef SPCC
#undef SPARP

constexpr int kSpPresetCount = (int)(sizeof kSpPresets / sizeof kSpPresets[0]);

// Pass 2: the same rows read for their state. A row that carries none produces
// a default-constructed SpState, which is what "loadPreset resets state too"
// needs — a preset is COMPLETE however short it is written, and v3 extends that
// rule from parameters to state.
//
// A malformed macro argument is a BUILD-time fact about the bank file and not a
// run-time one, so it is asserted rather than handled: an SPLFO grid that is not
// sixteen lowercase hex digits, or an out-of-range n, leaves that field at its
// default and the bank's own range checker is what says so.
inline void spPresetGrid(SpState& st, int n, const char* g, f32 smooth) {
    if (n < 1 || n > 3 || !g) return;
    const int j = n - 1;
    std::string s(g);
    if (s.size() != kSpSteps) return;
    for (int i = 0; i < kSpSteps; ++i) {
        const int d = spHexLo(s[(size_t)i]);
        if (d < 0) return;
        st.grid[j][i] = (u8)d;
    }
    // Quantized to thousandths when the row is written out as a state string,
    // so quantize it HERE and never carry a float the state cannot express.
    const f32 c = clampv(smooth, 0.f, 1.f);
    st.smooth[j] = (i16)(int)(c * 1000.f + 0.5f);
}

inline void spPresetHash(SpState& st, int osc, const char* h) {
    u64 v = 0;
    if (h && spParseHex64(std::string(h), v)) st.wt[osc] = v;
}

// v5's SPWTNA / SPWTNB. A PAIR of A/B macros rather than one macro with an
// oscillator argument, because v3 already settled that question -- "a 0/1
// argument beside SPLFO's 1-based n is a trap" -- and this file has one answer
// to it.
//
// 1..64 bytes, no control bytes. A malformed argument is a BUILD-time fact
// about the bank file and not a run-time one, so it is left at the default here
// exactly as SPLFO's and SPARP's are, and the bank's own range checker is what
// says so. The PAIRING rule -- an SPWTNA requires the matching SPWTA in the same
// row, because a name for a table the row does not name is a name for nothing --
// is likewise the checker's, in the suite, not this function's: dropping it
// silently is what the checker exists to prevent.
inline void spPresetName(SpState& st, int osc, const char* n) {
    if (!n || !*n) return;
    const std::string v(n);
    if (v.size() > 64) return;
    for (char c : v) if ((unsigned char)c < 0x20 || (unsigned char)c == 0x7F) return;
    st.wtName[osc] = v;
}

// v4's one new macro: the level row then the step row, in that order — the
// order they appear in the contract and in the state string. BOTH ARGUMENTS
// ARE MANDATORY; a preset that wants a default row writes the default string
// rather than omitting the argument, because a two-argument macro with an
// optional argument is a preprocessor trap and this file has enough of those.
//
// The pairing rule is the bank's, not this file's: SPARP does not set Arp On,
// and a row carrying one without `SP(109, 1)` is a checker failure rather than
// a silent no-op. A malformed argument is a BUILD-time fact about the bank and
// is left at the default here, exactly as SPLFO's is.
inline void spPresetArp(SpState& st, const char* lvl, const char* stp) {
    if (!lvl || !stp) return;
    const std::string l(lvl), s(stp);
    if (l.size() != (size_t)kSpSteps || s.size() != (size_t)(2 * kSpSteps)) return;
    u8 lv[kSpSteps], sv[kSpSteps];
    for (int i = 0; i < kSpSteps; ++i) {
        const int d = spHexLo(l[(size_t)i]);
        const int hi = spHexLo(s[(size_t)(2 * i)]);
        const int lo = spHexLo(s[(size_t)(2 * i + 1)]);
        if (d < 0 || hi < 0 || lo < 0) return;
        lv[i] = (u8)d;
        sv[i] = spArpPack((u8)(16 * hi + lo));
    }
    for (int i = 0; i < kSpSteps; ++i) { st.arpLv[i] = lv[i]; st.arpSt[i] = sv[i]; }
}

const std::vector<SpState>& spPresetStates() {
    static const std::vector<SpState> v = [] {
        std::vector<SpState> o;
        o.reserve((size_t)kSpPresetCount);
        SpState cur;
#define SP_PRESET(nm)   cur = SpState{};
#define SP(id, v)
#define SP_END()        o.push_back(cur);
#define SPLFO(n, g, s)  spPresetGrid(cur, (n), (g), (f32)(s));
#define SPWTA(h)        spPresetHash(cur, 0, (h));
#define SPWTB(h)        spPresetHash(cur, 1, (h));
#define SPWTNA(n)       spPresetName(cur, 0, (n));
#define SPWTNB(n)       spPresetName(cur, 1, (n));
#define SPCC(n)         cur.cc = (i16)(n);
#define SPARP(l, s)     spPresetArp(cur, (l), (s));
#include "spectra_presets.inc"
#undef SP_PRESET
#undef SP
#undef SP_END
#undef SPLFO
#undef SPWTA
#undef SPWTB
#undef SPWTNA
#undef SPWTNB
#undef SPCC
#undef SPARP
        return o;
    }();
    return v;
}

// ---------------------------------------------------------------------------
// The device
// ---------------------------------------------------------------------------

class Spectra final : public InternalInstance {
public:
    explicit Spectra(const PluginDesc& d) : InternalInstance(d) {
        // ORDER IS THE CONTRACT. Every addParam() below is an id, the ids are
        // indices, and a saved set stores them. docs/SPECTRA-PARAMS.md is the
        // frozen list; entries may be appended and never moved.
        // Widened 0..7 -> 0..8 by the v3 contract. 0..7 are the eight
        // procedural factory tables, frozen; 8 is THIS oscillator's imported
        // custom table, named by the state string's wtA/wtB record. Custom
        // space begins at 8 and grows upward, so a later revision widens again
        // and every stored value keeps its meaning. v3 registers no dead slots:
        // a knob the user can turn to a value that refuses is worse than a knob
        // that stops.
        addIntParam("A Table",  "",   0, 8, 0);
        addParam   ("A Position", "", 0.f, 1.f, 0.f);
        addIntParam("A Coarse", "st", -24, 24, 0);
        addParam   ("A Fine",   "ct", -100.f, 100.f, 0.f);
        addParam   ("A Level",  "",   0.f, 1.f, 0.8f);
        addIntParam("A Unison", "",   1, 7, 1);
        addParam   ("A Detune", "ct", 0.f, 100.f, 15.f);
        addParam   ("A Spread", "",   0.f, 1.f, 0.5f);

        addIntParam("B Table",  "",   0, 8, 1);              // widened by v3
        addParam   ("B Position", "", 0.f, 1.f, 0.f);
        addIntParam("B Coarse", "st", -24, 24, 0);
        addParam   ("B Fine",   "ct", -100.f, 100.f, 0.f);
        // 0 by default: osc B is off out of the box, so the instrument makes
        // one sound rather than two detuned ones the first time it is played.
        addParam   ("B Level",  "",   0.f, 1.f, 0.f);
        addIntParam("B Unison", "",   1, 7, 1);
        addParam   ("B Detune", "ct", 0.f, 100.f, 15.f);
        addParam   ("B Spread", "",   0.f, 1.f, 0.5f);

        addParam("Noise", "", 0.f, 1.f, 0.f);
        addParam("Sub",   "", 0.f, 1.f, 0.f);

        addParam   ("Cutoff",      "Hz", 20.f, 20000.f, 20000.f, true);
        addParam   ("Resonance",   "",   0.f,  1.f,     0.1f);
        // Widened 0..2 -> 0..5 by the v2 contract (a strict superset: the
        // three v1 values keep their meaning; 3 LP24, 4 HP24, 5 Notch).
        addIntParam("Filter Type", "",   0, 5, 0);
        addParam   ("Drive",       "dB", 0.f,  24.f,    0.f);
        addParam   ("Env2>Cutoff", "",  -1.f,  1.f,     0.f);
        addParam   ("Keytrack",    "",   0.f,  1.f,     0.f);

        addParam("Attack",  "ms", 0.1f, 5000.f, 2.f,   true);
        addParam("Decay",   "ms", 1.f,  5000.f, 400.f, true);
        addParam("Sustain", "",   0.f,  1.f,    0.7f);
        addParam("Release", "ms", 1.f,  8000.f, 120.f, true);

        addParam("E2 Attack",  "ms", 0.1f, 5000.f, 2.f,   true);
        addParam("E2 Decay",   "ms", 1.f,  5000.f, 300.f, true);
        addParam("E2 Sustain", "",   0.f,  1.f,    0.f);
        addParam("E2 Release", "ms", 1.f,  8000.f, 150.f, true);

        addParam   ("LFO Rate",     "Hz", 0.01f, 40.f, 2.f, true);
        addIntParam("LFO Sync",     "",   0, kSpSyncCount - 1, 0);
        addParam   ("LFO>Position", "",  -1.f, 1.f, 0.f);
        addParam   ("LFO>Cutoff",   "",  -1.f, 1.f, 0.f);
        addParam   ("LFO>Pitch",    "ct", 0.f, 100.f, 0.f);
        // Widened 0..4 -> 0..5 by the v3 contract: 5 is Custom, the drawable
        // 16-step grid. The grid itself is state and not a parameter — which is
        // what keeps three drawable LFOs from costing 51 ids, and what makes a
        // drawn grid unautomatable. It is shape, like a table is shape.
        addIntParam("LFO Shape",    "",   0, 5, 0);

        // Widened 0..500 -> 0..2000 ms by the v2 contract (still lin, still
        // constant-time, 0 = off; every stored plain-ms value keeps meaning).
        addParam   ("Glide",  "ms", 0.f, 2000.f, 0.f);
        addIntParam("Voices", "",   1, kSpVoices, kSpVoices);
        addParam   ("Master", "",   0.f, 1.5f, 0.7f);
        addParam   ("Env2>Position", "", -1.f, 1.f, 0.f);

        // --- v2, ids 42..99. APPEND ONLY, same as everything above. Every
        // default is "do what v1 did" — that is the contract's bit-identity
        // gate, not a style choice.
        addIntParam("Sub Shape",   "",    0, 2, 0);                   // 42
        addIntParam("Sub Oct",     "oct", -2, 0, -1);                 // 43
        addParam   ("Noise Color", "",    0.f, 1.f, 1.f);             // 44
        addIntParam("Noise Track", "",    0, 1, 0);                   // 45
        addReserved();                                                // 46
        addReserved();                                                // 47
        addIntParam("A Warp",      "",    0, 7, 0);                   // 48
        addParam   ("A Warp Amt",  "",    0.f, 1.f, 0.f);             // 49
        addIntParam("B Warp",      "",    0, 7, 0);                   // 50
        addParam   ("B Warp Amt",  "",    0.f, 1.f, 0.f);             // 51
        addReserved();                                                // 52
        addReserved();                                                // 53
        addParam   ("L2 Rate",     "Hz",  0.01f, 40.f, 2.f, true);    // 54
        addIntParam("L2 Sync",     "",    0, kSpSyncCount - 1, 0);    // 55
        addIntParam("L2 Shape",    "",    0, 5, 0);                   // 56 (v3)
        addParam   ("L3 Rate",     "Hz",  0.01f, 40.f, 2.f, true);    // 57
        addIntParam("L3 Sync",     "",    0, kSpSyncCount - 1, 0);    // 58
        addIntParam("L3 Shape",    "",    0, 5, 0);                   // 59 (v3)
        // v3 spends this block's two reserved ids on the LFOs the block owns:
        // 0 Loop is v2 verbatim, 1 One-shot makes the LFO an envelope.
        addIntParam("L2 Mode",     "",    0, 1, 0);                   // 60 (v3)
        addIntParam("L3 Mode",     "",    0, 1, 0);                   // 61 (v3)
        addParam("E3 Attack",  "ms", 0.1f, 5000.f, 2.f,   true);      // 62
        addParam("E3 Decay",   "ms", 1.f,  5000.f, 300.f, true);      // 63
        addParam("E3 Sustain", "",   0.f,  1.f,    0.f);              // 64
        addParam("E3 Release", "ms", 1.f,  8000.f, 150.f, true);      // 65
        addReserved();                                                // 66
        addReserved();                                                // 67
        for (int k = 0; k < 8; ++k) {                                 // 68..91
            char nm[12];
            // Src widened 0..13 -> 0..16 by v3: 14 Mod Wheel, 15 Pitch Bend,
            // 16 MIDI CC (the one learned slot). The DESTINATION enum does not
            // widen — nothing v3 adds is a modulatable target.
            std::snprintf(nm, sizeof nm, "M%d Src", k + 1);
            addIntParam(nm, "", 0, kSrcCount - 1, 0);
            std::snprintf(nm, sizeof nm, "M%d Dst", k + 1);
            addIntParam(nm, "", 0, 19, 0);
            std::snprintf(nm, sizeof nm, "M%d Amt", k + 1);
            addParam(nm, "", -1.f, 1.f, 0.f);
        }
        addReserved();                                                // 92
        addReserved();                                                // 93
        addParam("Macro 1", "", 0.f, 1.f, 0.f);                       // 94
        addParam("Macro 2", "", 0.f, 1.f, 0.f);                       // 95
        addParam("Macro 3", "", 0.f, 1.f, 0.f);                       // 96
        addParam("Macro 4", "", 0.f, 1.f, 0.f);                       // 97
        addIntParam("Voice Mode", "", 0, 2, 0);                       // 98
        // --- v3, the last spent reserved id and the 100..110 append.
        //
        // BEND RANGE IS THE REVISION'S ONE STATED EXCEPTION to "every default
        // does what v2 did", and it is named in the contract rather than left
        // to be discovered: it defaults to 2 semitones and not to 0. A build
        // that ignored the pitch wheel is a broken instrument, and this is the
        // revision that stops ignoring it. 0 is inert and bit-identical to v2.
        addIntParam("Bend Range", "st", 0, 24, 2);                    // 99 (v3)
        addIntParam("L1 Mode", "", 0, 1, 0);                          // 100
        for (int k = 0; k < 8; ++k) {                                 // 101..108
            char nm[12];
            std::snprintf(nm, sizeof nm, "M%d Curve", k + 1);
            addIntParam(nm, "", 0, 2, 0);
        }
        // --- v4, the arpeggiator: v3's generic reserved tail spent on the two
        // ids that OPEN the block (the switch and the mode — the readable
        // choice as well as the only pair whose defaults are 0-means-nothing),
        // then the 111..124 append. One contiguous run, 109..124.
        //
        // ARP ON IS THE REVISION'S BIT-IDENTITY SWITCH. It defaults to 0 and 0
        // selects the v3 MIDI path OUTRIGHT — incoming notes reach noteOn()
        // unchanged and no arp code is reached at all — which is why v4 has no
        // stated exception to "a v3 state renders bit-identically", the one
        // thing it does that v3 could not.
        addIntParam("Arp On",       "",    0, 1, 0);                  // 109 (v4)
        addIntParam("Arp Mode",     "",    0, kArpModeCount - 1, 0);  // 110 (v4)
        addParam   ("Arp Rate",     "Hz",  0.01f, 40.f, 2.f, true);   // 111
        // Default 7 = 1/16, the only rate anyone reaches for first, and the
        // only v4 default that is not simply the bottom of its range. It costs
        // exactly nothing in render terms because Arp On is 0.
        addIntParam("Arp Sync",     "",    0, kSpSyncCount - 1, 7);   // 112
        addIntParam("Arp Octaves",  "oct", 1, 4, 1);                  // 113
        addIntParam("Arp Oct Mode", "",    0, 2, 0);                  // 114
        addParam   ("Arp Gate",     "%",   1.f, 200.f, 50.f);         // 115
        addParam   ("Arp Swing",    "%",   0.f, 100.f, 0.f);          // 116
        addIntParam("Arp Hold",     "",    0, 1, 0);                  // 117
        // Default 1: a keyboard player expects the first note they press to
        // sound when they press it. An author who wants the grid to own the
        // phrase sets 0.
        addIntParam("Arp Retrig",   "",    0, 1, 1);                  // 118
        addIntParam("Arp Vel Mode", "",    0, 2, 0);                  // 119
        // THE FLOOR IS 1, NOT 0. This device treats a note-on with velocity 0
        // as a note-off, so a generated 0 would be a generated note-off and the
        // arp would silently emit nothing.
        addIntParam("Arp Fixed Vel","",    1, 127, 100);              // 120
        addIntParam("Arp Steps",    "",    1, kSpSteps, kSpSteps);    // 121
        addParam   ("Arp Chance",   "%",   0.f, 100.f, 100.f);        // 122
        addReserved();                                                // 123
        addReserved();                                                // 124

        // The pair of oscillator handles this instance owns for the life of it.
        // See the seam's note: they are handles and not indices, so two devices
        // that import two files onto their A oscillators do not collide.
        oscH_[0] = spAcquireOsc();
        oscH_[1] = spAcquireOsc();
    }

    ~Spectra() override {
        spReleaseOsc(oscH_[0]);
        spReleaseOsc(oscH_[1]);
    }

    // A reserved id IS registered (contract rule: the addParam() sequence
    // stays dense so indices never move): name "—", 0..1, default 0, hidden
    // by the editor, never read by the DSP.
    void addReserved() { addParam("—", "", 0.f, 1.f, 0.f); }

    bool prepare(f64 sampleRate, int maxBlock) override {
        sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxBlock_ = maxBlock > 0 ? maxBlock : kMaxBlock;

        // GUI thread. The one allocation in the device, and only the first
        // instance in the process pays for it. The second line publishes the
        // same set for the editor to draw; it allocates nothing and mutates
        // nothing, and it is here rather than in the constructor because the
        // set does not exist until this line has run.
        tbl_ = &spTables();
        spPublish();

        // v5. The preview arena's rate limit is computed FROM THE VALUES
        // prepare() WAS GIVEN and not hard-coded, so the recycle bound
        // (4 * interval > maxBlock / sampleRate) holds at every rate and block
        // size a host can choose. This allocates the arena's bookkeeping and
        // not its buffers; those wait for the first preview.
        for (int o = 0; o < 2; ++o)
            if (oscH_[o] >= 0) spPreviewClock(oscH_[o], sr_, maxBlock_);

        for (Voice& v : voices_) v = Voice{};
        nPend_    = 0;
        ovfOff_[0] = ovfOff_[1] = ovfOff_[2] = ovfOff_[3] = 0u;
        ovfPanic_ = 0;
        haveOvf_  = false;
        age_      = 0;
        ctrl_     = 0;
        noteRng_  = 0x9E3779B9u;      // fixed seeds: a render must be repeatable
        lfoRng_   = 0x2545F491u;
        lfo_.reset();
        shVal_    = 0.f;
        lastPitch_ = 60.f;
        havePitch_ = false;
        // v2 state, same rules: fixed seeds, zeroed counters, no clocks.
        lfo2_.reset();
        lfo3_.reset();
        shVal2_   = 0.f;
        shVal3_   = 0.f;
        lfo2Rng_  = 0x6C078965u;      // one counter per S&H stream, so the
        lfo3Rng_  = 0xB5297A4Du;      // three streams cannot interleave
        after_    = 0.f;
        absPos_   = 0;
        nHeld_    = 0;
        // v3, same rules again. The three MIDI sources are 0 after prepare()
        // by the contract — bend meaning CENTRE, not "wherever the wheel was
        // left". A render must not depend on the performance before it.
        wheel_    = 0.f;
        bend_     = 0.f;
        ccVal_    = 0.f;
        ovfCCNum_ = -1;
        ovfCCVal_ = 0;
        ovfBend_  = -1;
        // The drawn grid's lag is initialised at the PHASE ORIGIN, which for a
        // Loop LFO is prepare(); it never carries across renders.
        publishStateToAudio();
        for (int j = 0; j < 3; ++j) {
            smY_[j] = gridAt(j, 0.f);
            smA_[j] = 1.f;
        }
        beatAcc_    = 0.0;
        beatInc_    = 0.0;
        trBeatWas_  = 0.0;
        trBeatSeen_ = false;
        trPlayWas_  = false;
        // v4, same rules once more: fixed origins, zeroed counters, no clocks.
        // Every index the arp uses is a modulus of a step number that is a
        // floor of this clock, so there is nothing else here to reset.
        arp_          = ArpCfg{};
        arpFree_      = 0.0;
        arpOrigin_    = 0.0;
        arpNextOnset_ = 0.0;
        arpFiredK_    = -1;
        arpFresh_     = true;
        arpReanchor_  = false;
        arpStepLvl_   = 0.f;       // 0 after prepare() until the first onset
        nLatch_       = 0;
        arpDropSounds();
        resolveTables();
        return true;
    }

    // --- presets (host.h) --------------------------------------------------
    //
    // The bank is factory THEN user, and factoryPresetCount() is the boundary
    // the editor's popover draws its "User" header at. Everything behind the
    // forwarding is generic and lives on PluginInstance (host.cpp): it writes
    // every parameter AND this device's own stateString() into an `nxp1` file,
    // atomically, keeping one generation of `.nxp.bak`. A device opts in by
    // forwarding, which is the whole of what the contract asks of it.
    int presetCount() const override { return kSpPresetCount + userPresetCount(); }
    int factoryPresetCount() const override { return kSpPresetCount; }

    const char* presetName(int i) const override {
        if (i < 0) return nullptr;
        if (i < kSpPresetCount) return kSpPresets[i].name;
        return userPresetName(i - kSpPresetCount);
    }

    bool savePreset(const char* name) override { return saveUserPreset(name); }

    // GUI thread. Writes through setParam and does nothing else, so the whole
    // program sees a preset as a handful of ordinary knob moves.
    void loadPreset(int i) override {
        if (i >= kSpPresetCount) { loadUserPreset(i - kSpPresetCount); return; }
        if (i < 0) return;
        for (int k = 0; k < n_; ++k) setParam(k, info_[(size_t)k].def);
        const SpPreset& p = kSpPresets[i];
        for (int k = 0; k < 64 && p.set[k].id >= 0; ++k)
            setParam(p.set[k].id, p.set[k].v);
        // v3: LOADPRESET RESETS STATE TOO. A preset is COMPLETE however short
        // it is written — v1's rule — and v3 extends it from parameters to
        // state, so switching from a patch with a drawn grid and an imported
        // table to a preset that mentions neither lands on exactly the state a
        // fresh instance would have. Deliberately UNLIKE the sampler, which
        // keeps its file across a preset change: the sampler's file is the
        // material the user brought, while a wavetable and a drawn grid are
        // sound design, and sound design is what a preset replaces.
        adoptState(spPresetStates()[(size_t)i]);
    }

    // --- state (host.h) ----------------------------------------------------
    //
    // GUI thread. EMPTY WHEN NO V3 FEATURE IS IN USE, so the project layer
    // writes no `state` key at all and a v2 set round-trips through a v3 build
    // byte-identically. That is a gate and not a nicety: Spectra had no state
    // string before this revision, and every set written by every older build
    // must keep loading and saving to the same bytes.
    std::string stateString() const override {
        if (!v3InUse()) return {};
        static const char kHex[] = "0123456789abcdef";
        std::string o = kSpTag;
        char buf[32];
        for (int n = 0; n < 3; ++n) {
            if (!st_.gridDrawn(n)) continue;
            std::snprintf(buf, sizeof buf, ";lfo%d=", n + 1);
            o += buf;
            for (int i = 0; i < kSpSteps; ++i) o += kHex[st_.grid[n][i] & 15u];
        }
        for (int n = 0; n < 3; ++n) {
            if (st_.smooth[n] == 0) continue;
            std::snprintf(buf, sizeof buf, ";smooth%d=%d", n + 1, (int)st_.smooth[n]);
            o += buf;
        }
        if (st_.cc >= 0) {
            std::snprintf(buf, sizeof buf, ";cc=%d", (int)st_.cc);
            o += buf;
        }
        // The hash is the identity and the path is only a recovery hint, so the
        // hashes come first and a path is never written without one. Both are
        // read from the SEAM and not from st_, because the editor imports
        // through WavetableControl and never touches this object; the seam
        // keeps the record it was given whether or not the file was found, so
        // this re-emits a missing file's name verbatim — which is exactly what
        // the refusal contract asks for.
        for (int osc = 0; osc < 2; ++osc) {
            if (!wtHashOf(osc)) continue;
            std::snprintf(buf, sizeof buf, ";wt%c=", osc ? 'B' : 'A');
            o += buf;
            o += spFmtHex64(wtHashOf(osc));
        }
        for (int osc = 0; osc < 2; ++osc) {
            const std::string pth = wtPathOf(osc);
            if (!wtHashOf(osc) || pth.empty()) continue;
            std::snprintf(buf, sizeof buf, ";wtpath%c=", osc ? 'B' : 'A');
            o += buf;
            smEsc(o, pth);
        }
        // v5's one new record, and it is APPENDED rather than interleaved.
        //
        // The contract lists the write order as wtA, wtpathA, wtnameA, wtB,
        // wtpathB, wtnameB -- the name following its table, as the path does.
        // This writer has emitted the two hashes and then the two paths since
        // v3, and reordering it would change the bytes a v3 or v4 state
        // round-trips to, which is a gate this file has carried since the
        // revision that added the records ("a v2 project round trips through a
        // v3 build BYTE-identically"). Reading is order-free, as the format has
        // always said, so the grouping is not observable to any reader; the
        // round trip is. Grouping wins.
        //
        // A NAME IS NEVER WRITTEN WITHOUT ITS TABLE. `wtnameA` with no `wtA` is
        // a display string for a table that is not there -- read as inert and
        // skipped, so producing one would be producing a record this reader
        // discards.
        for (int osc = 0; osc < 2; ++osc) {
            if (!wtHashOf(osc) || st_.wtName[osc].empty()) continue;
            std::snprintf(buf, sizeof buf, ";wtname%c=", osc ? 'B' : 'A');
            o += buf;
            smEsc(o, st_.wtName[osc]);
        }
        // v4's two rows, level then step — the order the contract lists them
        // in and the order SPARP takes them in. A row still at its default is
        // NOT emitted, which is what keeps the empty-state round-trip gate:
        // a v2 or v3 project that never touched the arp still writes no
        // `state` key at all.
        if (st_.arpDrawn()) {
            o += ";arpl=";
            for (int i = 0; i < kSpSteps; ++i) o += kHex[st_.arpLv[i] & 15u];
            o += ";arps=";
            for (int i = 0; i < kSpSteps; ++i) {
                const u8 v = spArpUnpack(st_.arpSt[i]);
                o += kHex[(v >> 4) & 15u];
                o += kHex[v & 15u];
            }
        }
        return o;
    }

    // What this oscillator NAMES — the seam's record when this instance owns a
    // handle, and the parsed record when the process ran out of handles and
    // there is nowhere else for it to live.
    u64 wtHashOf(int osc) const {
        return oscH_[osc] >= 0 ? spCustomHash(oscH_[osc]) : st_.wt[osc];
    }
    std::string wtPathOf(int osc) const {
        if (oscH_[osc] < 0) return st_.wtPath[osc];
        const char* p2 = spCustomPath(oscH_[osc]);
        return p2 ? std::string(p2) : std::string();
    }
    // The round-trip gate: nothing is written unless a v3 feature is in use.
    bool v3InUse() const {
        return st_.inUse() || wtHashOf(0) != 0 || wtHashOf(1) != 0;
    }

    // GUI thread. Parses into a FRESH SpState and only then acts: nothing here
    // touches the device until the whole string has been accepted, so a refusal
    // leaves the instrument exactly as it was.
    //
    // ON THE KEY CHARSET, because it is the one place this file reads the
    // contract's prose against the contract's own table and picks the table.
    // The prose says a key matches `[a-z][a-z0-9]*`; the record list then names
    // `wtA`, `wtB`, `wtpathA` and `wtpathB`, which that pattern rejects. The
    // KEYS are what the preset macros (SPWTA/SPWTB) and the editor spell, so
    // the pattern is the loose statement and the accepted charset here is
    // `[A-Za-z][A-Za-z0-9]*` — a strict superset of the prose, so every key the
    // prose allows is still a key, and the four the table names parse.
    bool setStateString(const std::string& s) override {
        if (s.empty()) return true;                 // "no state" -- not malformed

        const std::vector<std::string> recs = smSplit(s, ';');
        if (recs.empty() || recs[0] != kSpTag) return badState(s);

        SpState st;
        std::vector<std::string> seen;
        seen.reserve(recs.size());
        for (size_t r = 1; r < recs.size(); ++r) {
            const std::string& rec = recs[r];
            const size_t eq = rec.find('=');
            // An empty record, or one with no `key=`, is not something this
            // writer can produce. Refuse rather than skip: skipping would make
            // "nxspc1;;;;" a valid way of saying nothing.
            if (eq == std::string::npos || eq == 0) return badState(s);
            const std::string key = rec.substr(0, eq);
            if (!spKeyOk(key)) return badState(s);
            // Two records with one key are ambiguous, and choosing one of them
            // is guessing.
            for (const std::string& k : seen) if (k == key) return badState(s);
            seen.push_back(key);
            if (!readRecord(st, key, rec.substr(eq + 1))) return badState(s);
        }

        // Accepted. From here the only thing that can still go wrong is a
        // custom table this machine does not have, and A MISSING FILE IS NOT A
        // MALFORMED STATE: the set is correct and the machine is incomplete.
        // The records are kept either way, so saving on a machine that is
        // missing the file does not lose the file's name. This is the sampler's
        // rule and the reason for it is the sampler's reason.
        adoptState(st);
        return true;
    }

    // --- WavetableControl (host.h) -----------------------------------------
    //
    // THE OVERRIDE POINT, and it is exactly here: between setStateString()
    // above and the three accessors below. The table wave already owns the
    // control's BODY — `SpWavetableControl` is defined at the end of
    // spectra_tables.inc — so its diff against this file is two lines and no
    // rearrangement:
    //
    //     WavetableControl* wavetable() override {
    //         wtc_.bind(oscH_[0], oscH_[1]);      // or oscHandle(0/1)
    //         return &wtc_;
    //     }
    //     ... and `SpWavetableControl wtc_;` beside the members.
    //
    // The handles are already acquired (the constructor) and released (the
    // destructor), so binding is all that is left. The one thing that side must
    // NOT do is write st_: this file owns what the STATE says and that file
    // owns what the table IS, and stateString() reads the seam's record rather
    // than a copy precisely so an import made through WavetableControl needs no
    // notification to reach the next save. If a caller ever does want to name a
    // table from this side, adoptCustom() below is the door.

    // GUI thread. Records that this oscillator now names a custom table, or
    // (hash 0) that it names none. Idempotent, and it is the ONLY writer of the
    // wt records outside setStateString().
    void adoptCustom(int osc, u64 hash, const char* path) {
        if (osc < 0 || osc > 1) return;
        // v5. A NAME BELONGS TO ITS TABLE, so a change of table drops it. The
        // alternative -- carrying the old name onto the new content -- is a
        // library whose labels lie, which is worse than a library with no
        // labels. commitFrames() sets the new name immediately after, and
        // setCustomName() keeps the hash and therefore keeps the name: a rename
        // writes no file and produces no new hash.
        if (st_.wt[osc] != hash) { warnedTable_[osc] = false; st_.wtName[osc].clear(); }
        st_.wt[osc] = hash;
        st_.wtPath[osc] = path ? path : "";
        resolveTables();
    }

    // v5. The display name, and the ONLY writer of it outside setStateString()
    // and loadPreset(). Content is unchanged, so identity is unchanged.
    bool setCustomNameRec(int osc, const char* name) {
        if (osc < 0 || osc > 1) return false;
        if (!name || !*name) { st_.wtName[osc].clear(); return true; }
        const std::string v(name);
        if (v.size() > 64) return false;
        for (char c : v) if ((unsigned char)c < 0x20 || (unsigned char)c == 0x7F) return false;
        st_.wtName[osc] = v;
        return true;
    }
    const std::string& customNameRec(int osc) const {
        static const std::string kNone;
        return (osc == 0 || osc == 1) ? st_.wtName[osc] : kNone;
    }

    // The oscillator handles, for the table wave's WavetableControl to bind.
    int oscHandle(int osc) const { return (osc == 0 || osc == 1) ? oscH_[osc] : -1; }
    bool customResolved(int osc) const { return (osc == 0 || osc == 1) && wtOk_[osc]; }

    // --- WavetableControl (host.h) -----------------------------------------
    //
    // The editor's door onto the seam. GUI THREAD ONLY, every method: reading a
    // WAV, resampling it and building ten mip levels per frame allocates and
    // takes tens of milliseconds.
    //
    // `osc` here is the oscillator INDEX — 0 for A, 1 for B — translated to the
    // seam's HANDLE by oscHandle(). That translation is the whole reason this
    // class exists rather than the seam being the contract: host.h speaks about
    // a device's oscillators, the seam speaks about a process's.
    //
    // hasCustom() is true as soon as a table is NAMED and customFrames() is 0
    // until it is RESOLVED — exactly the pair the panel needs to draw the amber
    // refusal over a chip that still says which file is missing.
    class Wavetables final : public WavetableControl {
    public:
        explicit Wavetables(Spectra& s) : d_(s) {}

        bool importFile(int osc, const char* path) override {
            const int h = d_.oscHandle(osc);
            if (h < 0) { err_ = "this instance has no oscillator slot for a custom table"; return false; }
            std::string e;
            const u64 hash = spImportWavetable(h, path, e);
            if (!hash) { err_ = e.empty() ? std::string("import failed") : e; return false; }
            // The seam owns what the table IS; this call is what makes the
            // DEVICE say so, so stateString() names it and the wire ships it.
            d_.adoptCustom(osc, hash, path);
            err_.clear();
            return true;
        }
        bool hasCustom(int osc) const override {
            const int h = d_.oscHandle(osc);
            return h >= 0 && spCustomHash(h) != 0;
        }
        // v5's compatible widening, and it is a strict superset: the `wtname`
        // record first, then v3's two fallbacks unchanged. Every table that has
        // no name displays exactly what it displayed before.
        //
        // Rung 3 -- the bare 16-hex hash -- is what the contract's own
        // enumeration names as v3's behaviour, and it is what the seam has
        // always kept in SpOscRec::name for a table that arrived by hash alone.
        // The v3 CODE returned "" there; the v3 CONTRACT said hash. v5 states
        // the three rungs explicitly, so the code follows the contract and the
        // hash is what a preset-named table now shows. Nothing consults this
        // string for anything but display.
        const char* customName(int osc) const override {
            const int h = d_.oscHandle(osc);
            if (h < 0) return "";
            if (const std::string& n = d_.customNameRec(osc); !n.empty()) {
                disp_ = n;
                return disp_.c_str();
            }
            if (const char* p = spCustomPath(h); p && *p) {
                const char* slash = std::strrchr(p, '/');
                disp_ = slash ? slash + 1 : p;
                return disp_.c_str();
            }
            const u64 hash = spCustomHash(h);
            if (!hash) return "";
            disp_ = spFmtHex64(hash);
            return disp_.c_str();
        }
        int customFrames(int osc) const override {
            const int h = d_.oscHandle(osc);
            return h >= 0 && d_.customResolved(osc) ? spCustomFrames(h) : 0;
        }
        void clearCustom(int osc) override {
            d_.adoptCustom(osc, 0, nullptr);          // this calls spClearCustom()
            err_.clear();
        }
        const char* lastError() const override { return err_.c_str(); }

        // ------------------------------------------------------------------
        // v5 -- THE EDITOR'S FIVE. GUI thread, every one of them, and none of
        // them reachable from nxtaktd: the daemon renders and never draws.
        // ------------------------------------------------------------------

        bool readFrames(int osc, f32* out) const override {
            const int h = d_.oscHandle(osc);
            return h >= 0 && spReadFrames(h, out);
        }

        // The rate limit is the SEAM'S, not this class's and not the caller's --
        // the contract says "rate-limited by the contract, not by the caller",
        // and a bound the editor can forget is not a bound. A refusal here is
        // ordinary: it means "too soon", and the editor tries again on the next
        // stroke end.
        bool previewFrames(int osc, const f32* frames) override {
            const int h = d_.oscHandle(osc);
            if (h < 0) { err_ = "this instance has no oscillator slot for a custom table"; return false; }
            if (!spPreviewFrames(h, frames)) {
                err_ = "the preview was not published";
                return false;
            }
            err_.clear();
            return true;
        }

        void cancelPreview(int osc) override {
            const int h = d_.oscHandle(osc);
            if (h >= 0) spCancelPreview(h);
        }

        // The nine-step commit. Steps 1..8 are the seam's; step 9 -- the state
        // records -- is this side's, because this file owns what the state SAYS.
        //
        // A COMMIT THAT CHANGES THE FRAMES CLEARS AND REWRITES wtpath, and that
        // is what adoptCustom() with the DRAWN file's path does: editing an
        // imported table produces a new hash, and the WAV the old path named is
        // no longer the table the hash names. A path that recovers a DIFFERENT
        // table than its own record's hash is the one thing rung 5 must never
        // do, so the record follows the content or it goes.
        bool commitFrames(int osc, const f32* frames, const char* name) override {
            const int h = d_.oscHandle(osc);
            if (h < 0) { err_ = "this instance has no oscillator slot for a custom table"; return false; }
            // The name is validated BEFORE anything is written, so a commit
            // cannot half-succeed with a file on disk and a refused label.
            if (name && *name) {
                const std::string v(name);
                if (v.size() > 64) { err_ = "that name is longer than 64 bytes"; return false; }
                for (char c : v)
                    if ((unsigned char)c < 0x20 || (unsigned char)c == 0x7F) {
                        err_ = "that name contains a control character";
                        return false;
                    }
            }
            std::string path, e;
            const u64 hash = spCommitFrames(h, frames, path, e);
            if (!hash) { err_ = e.empty() ? std::string("the commit was refused") : e; return false; }
            d_.adoptCustom(osc, hash, path.c_str());     // drops any old name
            d_.setCustomNameRec(osc, name);              // sets the new one, or none
            err_.clear();
            return true;
        }

        // A rename writes no file and produces no new hash: identity is content
        // and a name is not content.
        bool setCustomName(int osc, const char* name) override {
            if (d_.oscHandle(osc) < 0) { err_ = "this instance has no oscillator slot for a custom table"; return false; }
            if (!d_.setCustomNameRec(osc, name)) {
                err_ = "that name is longer than 64 bytes or contains a control character";
                return false;
            }
            err_.clear();
            return true;
        }

    private:
        Spectra&            d_;
        mutable std::string err_;
        // customName() returns a `const char*` into something that must outlive
        // the call. It always did (the seam's std::string); the widening needs a
        // buffer of its own for the hash arm, and one mutable member is that.
        mutable std::string disp_;
    };

    WavetableControl* wavetable() override { return &wtctl_; }
    Wavetables wtctl_{*this};

    // REALTIME. Called before process() for the same block; voice state is
    // therefore plain members with no synchronisation, exactly as Pulse's is.
    //
    // Events are QUEUED here and acted on inside process(), at the exact sample
    // they were stamped for, rather than being applied the moment they arrive.
    // That is not tidiness, it is the determinism gate:
    //
    //   voice allocation steals the QUIETEST voice, so which voice a note takes
    //   is a function of every envelope's value AT THE INSTANT OF THE NOTE. A
    //   host calls midi() once per block, so applying a note-on immediately
    //   would read those envelopes as they stood at the START of the block --
    //   and the same MIDI in blocks of 1 and of 1024 would then steal different
    //   voices and render different audio.
    //
    // Note-offs go through the same queue, and must: a note-on and its note-off
    // inside one block have to be applied in order, and the off cannot find a
    // voice the on has not created yet.
    void midi(const u8* data, int len, int frameOffset) override {
        if (!data || len < 1) return;
        const u8 status = (u8)(data[0] & 0xF0u);
        const u8 chan   = (u8)(data[0] & 0x0Fu);   // part of the note's stable
                                                   // identity (matrix Random)
        const int off = frameOffset < 0 ? 0 : frameOffset;
        switch (status) {
            case 0x90:
                if (len >= 3 && data[2] > 0) { queue(off, kEvOn, data[1], data[2], chan); return; }
                if (len >= 2) queue(off, kEvOff, data[1], 0, chan);
                return;
            case 0x80:
                if (len >= 2) queue(off, kEvOff, data[1], 0, chan);
                return;
            case 0xB0:
                // 120 = all sound off, 123 = all notes off, and they keep the
                // meanings v1 gave them: they are panics and they do NOT feed
                // the mod wheel, the bend or the learned CC, even if 120 or 123
                // is what somebody learned.
                if (len < 2) return;
                if (data[1] == 120) { queue(off, kEvSoundOff, 0, 0, chan); return; }
                if (data[1] == 123) { queue(off, kEvNotesOff, 0, 0, chan); return; }
                // v3: CC 1 is the Mod Wheel source, and one other number is the
                // learned MIDI CC source. Every other controller is one we do
                // not map, and ignoring it is still the honest answer — the
                // filter is HERE rather than in apply() so that a controller
                // flood cannot push note-ons out of the queue.
                if (len >= 3) {
                    const int learn = ccNum_.load(std::memory_order_relaxed);
                    if (data[1] == 1 || (int)data[1] == learn)
                        queue(off, kEvCC, data[1], (u8)(data[2] & 0x7Fu), chan);
                }
                return;
            case 0xE0:
                // Pitch bend. `v14 = lsb | (msb << 7)`; the two halves ride the
                // event as they arrived and are folded in apply(), so the queue
                // stays five bytes wide.
                if (len >= 3) queue(off, kEvBend, (u8)(data[1] & 0x7Fu),
                                    (u8)(data[2] & 0x7Fu), chan);
                return;
            case 0xD0:
                // Channel pressure — the matrix's Aftertouch source. Queued
                // like a note so it lands at its stamped sample; instance-wide
                // by the contract ("applies to all voices").
                if (len >= 2) queue(off, kEvPressure, data[1], 0, chan);
                return;
            default:
                return;
        }
    }

    // REALTIME.
    void process(const f32* const* in, f32* const* out, int channels, int nframes) override {
        (void)in;
        if (channels <= 0 || nframes <= 0 || !out) return;
        // An instrument's "input" is silence, so bypass means silence out. The
        // block cap is belt and braces -- nothing here is sized per block -- but
        // it keeps the same promise Pulse makes, that an absurd nframes degrades
        // to silence rather than to whatever the loop counters do.
        if (isBypassed() || nframes > kMaxBlock || !tbl_) {
            passthrough(nullptr, out, channels, nframes);
            clearSchedule();
            absPos_ += (u64)nframes;      // engine time still advances
            return;
        }

        Blk b;
        readParams(b);

        f32* dl = out[0];
        f32* dr = channels > 1 ? out[1] : nullptr;

        int ev = 0;
        for (int n = 0; n < nframes; ++n) {
            // Note events land on the sample they were stamped for. See midi().
            while (ev < nPend_ && pend_[ev].frame <= n) { apply(pend_[ev]); ++ev; }
            // And anything the queue could not hold, at the block's last
            // sample — after everything above, which is the whole point. See
            // queue() for why it is last and not first.
            if (haveOvf_ && n == nframes - 1) applyOverflow();

            // v4: THE ARP, and it is here for one reason — ordering at a
            // coincident sample is fixed, incoming MIDI first and then the arp,
            // so a note-on arriving on a step boundary is in the set that step
            // plays and can retrigger the pattern to that step. The arp calls
            // the same voice bodies an incoming event calls, at THIS sample,
            // with this sample's absolute position as the identity stamp; it
            // never enters the event queue, so no density of arp output can
            // push an incoming note-off out of it.
            if (arp_.on) arpTick(n);

            // The LFO values for THIS sample, read before they are advanced,
            // so the control tick and the audio path see the same numbers.
            const Glob g = { globLfo(0, lfo_,  b, shVal_),
                             globLfo(1, lfo2_, b, shVal2_),
                             globLfo(2, lfo3_, b, shVal3_),
                             after_, wheel_, bend_, ccVal_, arpStepLvl_ };

            // v3: a One-shot LFO is PER VOICE, so this sample's value is
            // gathered for every sounding voice here — before the control tick
            // and before any voice renders, so both read the same number.
            if (b.anyOneShot)
                for (Voice& v : voices_) { if (v.active) voiceLfoRead(v, b); }

            // Control tick on ABSOLUTE sample time (the Auto Filter's rule):
            // the counter is a member and survives the block boundary, which is
            // what makes blocks of 1 and of 300 bit-identical to blocks of 256.
            if (ctrl_ <= 0) { retarget(b, g); ctrl_ = kCtrl; }
            --ctrl_;

            f32 accL = 0.f, accR = 0.f;
            for (Voice& v : voices_) {
                if (!v.active) continue;
                renderVoice(v, b, g, accL, accR);
            }

            lfoTicks();
            if (b.anyOneShot)
                for (Voice& v : voices_) { if (v.active) voiceLfoTick(v, b); }

            const f32 l = accL * b.master;
            const f32 r = accR * b.master;
            if (dl) dl[n] = dr ? l : 0.5f * (l + r);
            if (dr) dr[n] = r;
        }

        // Anything stamped past the end of this block (the engine should not do
        // that, but clamping beats losing a note) carries over with its offset
        // rebased on the next one.
        int keep = 0;
        for (int i = ev; i < nPend_; ++i) {
            pend_[keep] = pend_[i];
            pend_[keep].frame -= nframes;
            if (pend_[keep].frame < 0) pend_[keep].frame = 0;
            ++keep;
        }
        nPend_ = keep;
        absPos_ += (u64)nframes;

        copyExtra(nullptr, out, 2, channels, nframes);
    }

private:
    static constexpr int kSpVoices = 16;
    static constexpr int kUni      = 7;
    // 16 samples, 0.33 ms at 48 kHz: the same figure and the same reasoning as
    // the Auto Filter's. The fastest cutoff modulation this instrument can
    // produce moves by a hundredth of an octave between ticks, and the
    // coefficients are ramped across the gap rather than stepped.
    static constexpr int kCtrl = 16;
    // Modulation depths, in octaves at full parameter travel.
    static constexpr f32 kEnvCutOct = 6.f;
    static constexpr f32 kLfoCutOct = 4.f;
    // The matrix's two log-domain spans: cutoff (id 18, 20..20000 Hz) and the
    // LFO rates (0.01..40 Hz). A norm-domain contribution of 1.0 is the whole
    // travel of the knob, which is what the contract's "log domain of id N"
    // means.
    static constexpr f32 kCutOct  = 9.9657843f;    // log2(20000/20)
    static constexpr f32 kRateOct = 11.9657843f;   // log2(40/0.01)
    static constexpr int kMods    = 8;
    // The one-pole attack aims past its own target so the curve is exponential
    // rather than asymptotic; 1.3 is the usual overshoot and reaches 1.0 in
    // ln(1.3/0.3) = 1.466 time constants.
    static constexpr f32 kAtkAim = 1.3f;
    static constexpr f32 kEnvOff = 1e-5f;      // -100 dB: below any dither

    enum : u8 { kIdle = 0, kAtk, kDec, kSus, kRel };

    struct Env { f32 v = 0.f; u8 stage = kIdle; };

    struct Voice {
        bool active = false;
        u8   note   = 0;
        f32  velAmp = 0.f;
        f32  pitch = 60.f, pitchTarget = 60.f, glideStep = 0.f;
        int  glideLeft = 0;
        f32  phA[kUni] = {}, phB[kUni] = {};
        f32  subPh = 0.f;
        u32  rng = 1u;
        Env  e1, e2, e3;
        dsp::SvfCoeffs fc, fInc;
        dsp::SvfState  fs[2];
        dsp::SvfState  fs2[2];       // second stage, LP24/HP24 only
        bool fSnap = true;
        u32  age = 0;
        // v2 per-note state
        f32  vel01 = 0.f;            // raw velocity 0..1 (matrix source 6)
        f32  rnote = 0.f;            // Random-per-note, the identity hash
        f32  lastA = 0.f, lastB = 0.f;   // FM/RM taps, one sample late
        f32  nzL = 0.f, nzR = 0.f;   // Noise Color one-pole state
        f32  nzCoef = 1.f;

        // --- v3: the per-voice half of a One-shot LFO (ids 60/61/100).
        // A one-shot LFO IS an envelope, so it lives where the envelopes live.
        // osPh is 0 at the note-on's stamped sample and clamps at 1.0; osVal is
        // this sample's value, computed once before any voice renders so that
        // the control tick and the audio path see the same number (exactly the
        // rule the instance-wide LFOs already follow). osY is the drawn-grid
        // smoothing lag, initialised to L(d_0) at the phase origin.
        f32  osPh[3]  = {};
        f32  osVal[3] = {};
        f32  osY[3]   = {};
        f32  osSh[3]  = {};          // S&H, drawn once at note-on from the
                                     // note's identity hash salted by the LFO
        f32  osInc[3] = {};          // only owned here when a rate slot drives
        f32  osA[3]   = {};          // this LFO; otherwise the block owns both
        bool osSnap = true;          // "take the block's inc/coef on my first
                                     // sample" — a note-on has no Blk to read
    };

    // The per-sample globals every voice reads: the three LFO values, the
    // channel pressure and v3's three MIDI sources, gathered once so retarget()
    // and renderVoice() see the same numbers.
    struct Glob { f32 l1, l2, l3, after, wheel, bend, cc, arpStep; };

    // A queued note event. Five bytes of payload and a frame stamp; nothing in
    // here allocates and the queue is a fixed array, so midi() stays realtime.
    // The channel rides along because it is part of the note's stable identity
    // (the matrix's Random source hashes it).
    //
    // v3 adds two: a controller (number in `a`, value in `b`) and a pitch bend
    // (the two 7-bit halves in `a` and `b`). Both are queued for the reason
    // every other event is — applying them when midi() is called would make the
    // same MIDI in blocks of 1 and of 1024 produce different audio.
    enum : u8 { kEvOn = 0, kEvOff, kEvNotesOff, kEvSoundOff, kEvPressure,
                kEvCC, kEvBend };
    struct PendEv { int frame; u8 type, a, b, ch; };

    // ---------------------------------------------------------------------
    // v4: THE ARPEGGIATOR
    //
    // Read the contract's "v4 — the arpeggiator" section before this code; the
    // three sentences that decide the whole shape of it are:
    //
    //  1. NOTHING ACCUMULATES. Every index the arp uses is a modulus of the
    //     absolute step number `k`, and `k` is a floor of the clock. Not the
    //     note-cycle position, not the octave-cycle position, not the loop
    //     counter, not the randomness. That is what makes a locate land on the
    //     step the bar implies without the arp having run there, and what
    //     stops a rest, a tie, an out-of-range pitch or a lost Chance draw
    //     from renumbering the melody.
    //  2. GENERATED EVENTS LAND AT STAMPED SAMPLES. The arp runs inside the
    //     per-sample event loop and calls the same noteOn()/noteOff() voice
    //     bodies an incoming event calls, at that sample, with that sample's
    //     absolute position as the identity stamp. An arp that emitted at the
    //     top of the block would quantise every note to the block boundary.
    //     Ordering at a coincident sample is fixed: incoming MIDI first, then
    //     the arp; and within the arp, note-offs before note-ons.
    //  3. THE ARP NEVER ENTERS THE EVENT QUEUE, so no density of arp output
    //     can push an incoming note-off out of it, and it consumes no slot.
    //
    // The one bookkeeping that IS stateful is the list of notes the arp has
    // started and not yet stopped — you cannot turn off what you did not
    // remember turning on — plus the tie chain that extends them. Neither is
    // an index the melody derives from, and both are cleared by every event
    // that invalidates them (a panic, a re-anchor, a retrigger, Arp On going
    // either way).
    // ---------------------------------------------------------------------

    // Everything the arp reads from parameters, once per block. A member and
    // not a Blk field because noteOn() and noteOff() reach it too, and those
    // are called from apply() which has no Blk.
    struct ArpCfg {
        bool on = false;
        int  mode = kArpUp;
        bool freeRun = false;        // Arp Sync == 0: the transport is not read
        f64  beatsInv = 4.0;         // 1 / beats-per-step, when synced
        f64  inc = 0.0;              // steps per sample, when free
        int  octaves = 1;
        int  octMode = kArpOctUp;
        f64  gate = 0.5;             // Gate / 100, a fraction of the NOMINAL step
        f64  swing = 0.0;            // Swing / 300, in steps
        bool swingOn = false;        // Swing 0 selects the no-offset branch
        bool hold = false;
        bool retrig = true;
        int  velMode = kArpVelPlayed;
        int  fixedVel = 100;
        int  steps = kSpSteps;
        int  chance = 100;           // 100 selects the no-draw branch outright
        u64  lvBits = 0;             // the two rows, one atomic load each
        u64  stBits = 0;
    };

    // One note the arp has started and not yet stopped. `off` is in the Aeff
    // domain (units of steps), so it survives a tempo change the way the clock
    // does, and it already carries the tie run the note was born with.
    struct ArpSound { f64 off; u8 note, ch; };
    // Two generations of a 64-deep chord is the arithmetic worst case (gate
    // caps at 200 %, so at most two steps can overlap, and the same-note rule
    // means a repeat never stacks with itself).
    static constexpr int kArpSounds = 128;

    // Everything read from the parameters once per block. Reading them once is
    // the one block-size dependence every device in this tree shares: a knob
    // MOVING lands on a block boundary, which is what every plugin format does.
    struct Blk {
        const f32* tblA;
        const f32* tblB;
        f32 posA, posB;
        f32 ratioA, ratioB;                 // coarse+fine as a pitch ratio
        f32 lvlA, lvlB, lvlN, lvlSub;
        int uniA, uniB;
        f32 uratA[kUni], panLA[kUni], panRA[kUni], ugainA;
        f32 uratB[kUni], panLB[kUni], panRB[kUni], ugainB;
        f32 maxRatA, maxRatB;
        f32 cutoff, q, fcMax, keytrack, env2Cut, lfoCut;
        int ftype;
        bool drive;
        f32 driveG, driveC;
        f32 a1, d1, s1, r1;
        f32 a2, d2, s2, r2;
        f32 lfoPos, lfoPitch, env2Pos;
        f32 master;
        f32 incScale;                        // 440/sr

        // --- v2 ---
        int  subShape; f32 subMul;           // 2^SubOct; 0.5f at the default
        bool nzBypass, nzTrack; f32 nzFc;
        int  warpA, warpB; f32 warpAmtA, warpAmtB;
        bool needTapA, needTapB;             // "compute osc X's voice-0 tap"
        bool lfree[3];                       // Sync == 0 per LFO
        f32  lRateNorm[3];                   // rate knob on its log scale, 0..1
        f32  a3, d3, s3, r3;
        // `curve` is v3's per-slot response (ids 101..108). Linear is 0 and the
        // value is TESTED and not applied: a slot at Linear runs the v2
        // expression untouched, which is what makes the bit-identity gate hold.
        struct Slot { u8 src, dst, curve; f32 amt; };
        Slot slot[kMods]; int mN; u32 dstMask;
        f32  macro[4];

        // --- v3 ---
        int  lmode[3];                       // 0 Loop (instance) · 1 One-shot
        bool anyOneShot;                     // any of the three, this block
        int  lshape[3];                      // 0..5; 5 = the drawn grid
        bool anyCustom;                      // any LFO on shape 5
        bool beatStep[3];                    // shape 5, Loop, synced, transport
                                             // running: the step index comes
                                             // from the beat (the ruling above)
        f32  lBeats[3];                       // beats per cycle, 0 when free
        f32  lInc[3];                        // the LFO's increment this block
        f32  smA[3];                         // drawn-grid smooth coefficient
        bool rateSlot[3];                    // a matrix slot drives this free
                                             // LFO's rate: the tick owns it
        f32  bendRange;                      // semitones at full wheel travel
        f32  driveDb;
        f32  cutNorm, resNorm;               // matrix base positions
        f32  detA, detB;
        f32  offA[kUni], offB[kUni];         // the fan's -1..1 offsets
    };

    // --- parameter -> coefficient -----------------------------------------

    void readParams(Blk& b) {
        // The matrix first: dstMask gates arithmetic all through this function
        // and the voice — a destination no live slot reaches keeps the exact
        // v1 expression, which is how the bit-identity gate survives the
        // feature. A slot with source Off, dest Off or amount 0 contributes
        // nothing and costs nothing.
        b.mN = 0;
        b.dstMask = 0;
        for (int k = 0; k < kMods; ++k) {
            const int src = (int)clampv(p(kPM1Src + 3 * k) + 0.5f, 0.f, (f32)(kSrcCount - 1));
            const int dst = (int)clampv(p(kPM1Src + 3 * k + 1) + 0.5f, 0.f, 19.f);
            const f32 amt = clampv(p(kPM1Src + 3 * k + 2), -1.f, 1.f);
            if (src == kSOff || dst == kDOff || amt == 0.f) continue;
            b.slot[b.mN].src = (u8)src;
            b.slot[b.mN].dst = (u8)dst;
            // v3, ids 101..108: slot k's curve is id 101 + k, an id ARRAY and
            // therefore contiguous — which is why 92/93 could not hold it.
            b.slot[b.mN].curve = (u8)(int)clampv(p(kPM1Curve + k) + 0.5f, 0.f, 2.f);
            b.slot[b.mN].amt = amt;
            ++b.mN;
            b.dstMask |= 1u << dst;
        }
        for (int j = 0; j < 4; ++j)
            b.macro[j] = clampv(p(kPMacro1 + j), 0.f, 1.f);

        // Table selection, widened to 0..8 by v3. Slot 8 is this oscillator's
        // import and comes from the table wave's seam; a nullptr is an
        // unresolvable slot 8, and the refusal contract says what happens then:
        // the PARAMETER keeps its value (the set's intent is not edited by the
        // machine that could not honour it), the state records are kept and
        // re-emitted verbatim, and the oscillator renders FACTORY TABLE 0 for as
        // long as the resolution fails — silence would be a worse lie than the
        // wrong table, and the editor's amber badge says which it is. The log
        // line is not here: resolution is decided on the GUI thread, in
        // setStateString(), and that is where the once-per-instance warning is.
        const int ta = (int)clampv(p(kPATable) + 0.5f, 0.f, (f32)kSpCustomSlot);
        const int tb = (int)clampv(p(kPBTable) + 0.5f, 0.f, (f32)kSpCustomSlot);
        // spTableBase and not spTableFor: the seam offers a wrapper with the
        // fallback already applied, but the refusal is THIS file's contract and
        // is spelled out here so it is one branch a reader can find and a test
        // can break. tbl_->frame(0, 0) is also the one answer that survives the
        // set not being published yet, which the wrapper cannot give.
        b.tblA = spTableBase(ta, oscH_[0]);
        b.tblB = spTableBase(tb, oscH_[1]);
        if (!b.tblA) b.tblA = tbl_->frame(0, 0);
        if (!b.tblB) b.tblB = tbl_->frame(0, 0);
        b.posA = clampv(p(kPAPos), 0.f, 1.f);
        b.posB = clampv(p(kPBPos), 0.f, 1.f);

        b.ratioA = std::exp2((clampv(p(kPACoarse), -24.f, 24.f) +
                              clampv(p(kPAFine), -100.f, 100.f) * 0.01f) * (1.f / 12.f));
        b.ratioB = std::exp2((clampv(p(kPBCoarse), -24.f, 24.f) +
                              clampv(p(kPBFine), -100.f, 100.f) * 0.01f) * (1.f / 12.f));

        b.lvlA   = clampv(p(kPALevel), 0.f, 1.f);
        b.lvlB   = clampv(p(kPBLevel), 0.f, 1.f);
        b.lvlN   = clampv(p(kPNoise), 0.f, 1.f);
        b.lvlSub = clampv(p(kPSub), 0.f, 1.f);

        b.uniA = (int)clampv(p(kPAUni) + 0.5f, 1.f, (f32)kUni);
        b.uniB = (int)clampv(p(kPBUni) + 0.5f, 1.f, (f32)kUni);
        b.detA = clampv(p(kPADet), 0.f, 100.f);
        b.detB = clampv(p(kPBDet), 0.f, 100.f);
        buildFan(b.uniA, b.detA, clampv(p(kPASpread), 0.f, 1.f),
                 b.uratA, b.panLA, b.panRA, b.offA, b.ugainA, b.maxRatA);
        buildFan(b.uniB, b.detB, clampv(p(kPBSpread), 0.f, 1.f),
                 b.uratB, b.panLB, b.panRB, b.offB, b.ugainB, b.maxRatB);

        b.cutoff   = clampv(p(kPCutoff), 20.f, 20000.f);
        // Resonance 0..1 -> Q 0.5..20 geometrically, as the Auto Filter maps
        // it: a linear map spends most of its travel below where resonance
        // becomes audible.
        b.q        = 0.5f * std::pow(40.f, clampv(p(kPRes), 0.f, 1.f));
        b.fcMax    = (f32)(sr_ * 0.45);
        b.keytrack = clampv(p(kPKeytrack), 0.f, 1.f);
        b.env2Cut  = clampv(p(kPE2Cut), -1.f, 1.f);
        b.lfoCut   = clampv(p(kPLfoCut), -1.f, 1.f);
        b.ftype    = (int)clampv(p(kPFType) + 0.5f, 0.f, 5.f);
        // The matrix's base positions on the two norm scales. One log2 per
        // block; only read when a slot targets Cutoff/Resonance.
        b.cutNorm  = (f32)(std::log2((f64)b.cutoff / 20.0) / (f64)kCutOct);
        b.resNorm  = clampv(p(kPRes), 0.f, 1.f);

        const f32 drv = clampv(p(kPDrive), 0.f, 24.f);
        // A branch, not a computation: at 0 dB the drive stage is a wire and
        // not tanh(x), so the default patch is exactly the oscillator it says
        // it is. The compensation is the Saturator's -- the gain that keeps a
        // -6 dBFS reference at the level it had at unity drive.
        b.drive   = drv > 0.f;
        b.driveG  = dbToGain(drv);
        b.driveC  = b.drive ? std::tanh(0.5f) / std::tanh(b.driveG * 0.5f) : 1.f;
        b.driveDb = drv;                       // the matrix sums in dB

        b.a1 = atkCoef(clampv(p(kPAttack), 0.1f, 5000.f));
        b.d1 = decCoef(clampv(p(kPDecay), 1.f, 5000.f));
        b.s1 = clampv(p(kPSustain), 0.f, 1.f);
        b.r1 = decCoef(clampv(p(kPRelease), 1.f, 8000.f));
        b.a2 = atkCoef(clampv(p(kPE2Attack), 0.1f, 5000.f));
        b.d2 = decCoef(clampv(p(kPE2Decay), 1.f, 5000.f));
        b.s2 = clampv(p(kPE2Sustain), 0.f, 1.f);
        b.r2 = decCoef(clampv(p(kPE2Release), 1.f, 8000.f));
        b.a3 = atkCoef(clampv(p(kPE3Attack), 0.1f, 5000.f));
        b.d3 = decCoef(clampv(p(kPE3Decay), 1.f, 5000.f));
        b.s3 = clampv(p(kPE3Sustain), 0.f, 1.f);
        b.r3 = decCoef(clampv(p(kPE3Release), 1.f, 8000.f));

        b.lfoPos   = clampv(p(kPLfoPos), -1.f, 1.f);
        b.lfoPitch = clampv(p(kPLfoPitch), 0.f, 100.f);
        b.env2Pos  = clampv(p(kPE2Pos), -1.f, 1.f);

        const int sync = (int)clampv(p(kPLfoSync) + 0.5f, 0.f, (f32)(kSpSyncCount - 1));
        b.lfree[0] = sync == 0;
        if (sync > 0) {
            // The pushed transport, with no Tempo parameter behind it: this
            // instrument postdates setTransport, so a knob that only existed to
            // work around its absence would be a wart with no history to excuse
            // it. A host that never pushes one gets 120, stated here and in the
            // contract rather than guessed at.
            const f32 bpm = trBpm_ > 0.0 ? clampv((f32)trBpm_, 20.f, 999.f) : 120.f;
            lfo_.setRate(sr_, bpm / (60.f * kSpSyncBeats[sync]));
        } else if (!(b.dstMask & (1u << kDL1Rate))) {
            // When a matrix slot drives this rate, the control tick owns it
            // (retarget); a per-block write here would quantise the modulation
            // to block boundaries and break block-size invariance.
            lfo_.setRate(sr_, clampv(p(kPLfoRate), 0.01f, 40.f));
        }

        // LFO2/3 — the same block, three times (contract: same division
        // table, same shapes, same behaviour; no fixed routings).
        {
            dsp::Lfo* ls[2] = { &lfo2_, &lfo3_ };
            const int syncIds[2]  = { kPL2Sync, kPL3Sync };
            const int rateIds[2]  = { kPL2Rate, kPL3Rate };
            const int rateDst[2]  = { kDL2Rate, kDL3Rate };
            for (int j = 0; j < 2; ++j) {
                const int sy = (int)clampv(p(syncIds[j]) + 0.5f, 0.f, (f32)(kSpSyncCount - 1));
                b.lfree[j + 1] = sy == 0;
                if (sy > 0) {
                    const f32 bpm = trBpm_ > 0.0 ? clampv((f32)trBpm_, 20.f, 999.f) : 120.f;
                    ls[j]->setRate(sr_, bpm / (60.f * kSpSyncBeats[sy]));
                } else if (!(b.dstMask & (1u << rateDst[j]))) {
                    ls[j]->setRate(sr_, clampv(p(rateIds[j]), 0.01f, 40.f));
                }
            }
        }

        // --- v3: shapes (widened to 0..5), modes, and the drawn grid's
        // smoothing coefficient.
        b.lshape[0] = (int)clampv(p(kPLfoShape) + 0.5f, 0.f, 5.f);
        b.lshape[1] = (int)clampv(p(kPL2Shape)  + 0.5f, 0.f, 5.f);
        b.lshape[2] = (int)clampv(p(kPL3Shape)  + 0.5f, 0.f, 5.f);
        b.lmode[0]  = (int)clampv(p(kPL1Mode) + 0.5f, 0.f, 1.f);
        b.lmode[1]  = (int)clampv(p(kPL2Mode) + 0.5f, 0.f, 1.f);
        b.lmode[2]  = (int)clampv(p(kPL3Mode) + 0.5f, 0.f, 1.f);
        b.anyOneShot = b.lmode[0] || b.lmode[1] || b.lmode[2];
        b.anyCustom  = b.lshape[0] == 5 || b.lshape[1] == 5 || b.lshape[2] == 5;
        {
            const dsp::Lfo* ls[3] = { &lfo_, &lfo2_, &lfo3_ };
            const int rateDst[3] = { kDL1Rate, kDL2Rate, kDL3Rate };
            const int syncOf[3]  = { kPLfoSync, kPL2Sync, kPL3Sync };
            // "A transport is running" is the ruling's own condition for the
            // beat-derived step index; a host that never pushed one, or that
            // pushed one and stopped, falls back to accumulated phase.
            const bool running = trPlaying_ && trBpm_ > 0.0;
            for (int j = 0; j < 3; ++j) {
                b.lInc[j]    = ls[j]->inc;
                b.rateSlot[j] = b.lfree[j] && (b.dstMask & (1u << rateDst[j])) != 0;
                b.smA[j]     = smoothCoef(b.lInc[j], j);
                const int sy = (int)clampv(p(syncOf[j]) + 0.5f, 0.f,
                                           (f32)(kSpSyncCount - 1));
                b.lBeats[j]  = sy > 0 ? kSpSyncBeats[sy] : 0.f;
                b.beatStep[j] = b.lshape[j] == 5 && b.lmode[j] == kLfoLoop &&
                                b.lBeats[j] > 0.f && running;
            }
        }
        syncBeat();
        // v3's one stated exception to "every default does what v2 did".
        b.bendRange = (f32)(int)clampv(p(kPBendRange) + 0.5f, 0.f, 24.f);
        // Rate-knob positions on the log scale, for the matrix's rate
        // destinations. Only meaningful when a slot targets them.
        b.lRateNorm[0] = (f32)(std::log2((f64)clampv(p(kPLfoRate), 0.01f, 40.f) / 0.01) / (f64)kRateOct);
        b.lRateNorm[1] = (f32)(std::log2((f64)clampv(p(kPL2Rate), 0.01f, 40.f) / 0.01) / (f64)kRateOct);
        b.lRateNorm[2] = (f32)(std::log2((f64)clampv(p(kPL3Rate), 0.01f, 40.f) / 0.01) / (f64)kRateOct);

        // Sub & noise completion (ids 42..45). subMul is exactly 0.5f at the
        // default (-1 oct): exp2 of an integer is exact, so the v1 sub
        // increment `base * 0.5f` is reproduced bit for bit.
        b.subShape = (int)std::floor(clampv(p(kPSubShape), 0.f, 2.f) + 0.5f);
        b.subMul   = std::exp2(std::floor(clampv(p(kPSubOct), -2.f, 0.f) + 0.5f));
        const f32 nzColor = clampv(p(kPNzColor), 0.f, 1.f);
        b.nzBypass = nzColor >= 1.f;      // "at exactly 1.0 the filter is
                                          // bypassed" — a branch, not a limit
        b.nzFc     = 200.f * std::pow(100.f, nzColor);
        b.nzTrack  = (int)clampv(p(kPNzTrack) + 0.5f, 0.f, 1.f) != 0;

        // Warp (ids 48..51).
        b.warpA    = (int)clampv(p(kPAWarp) + 0.5f, 0.f, 7.f);
        b.warpAmtA = clampv(p(kPAWarpAmt), 0.f, 1.f);
        b.warpB    = (int)clampv(p(kPBWarp) + 0.5f, 0.f, 7.f);
        b.warpAmtB = clampv(p(kPBWarpAmt), 0.f, 1.f);
        // "An osc whose Level is 0 still computes voice 0 whenever the other
        // osc's mode is FM/RM": needTapX = the OTHER osc wants X as modulator.
        b.needTapA = b.warpB == 6 || b.warpB == 7;
        b.needTapB = b.warpA == 6 || b.warpA == 7;

        b.master   = clampv(p(kPMaster), 0.f, 1.5f);
        b.incScale = (f32)(440.0 / sr_);

        readArpParams();
    }

    // --- v4: the arp's block read -------------------------------------------
    //
    // ARP ON = 0 IS A SELECTED BRANCH AND NOT A COMPUTED ONE: nothing below the
    // first `if` runs, no arp state advances, source 17 is exactly 0, and
    // incoming notes reach noteOn() by the v3 path. That is the whole of the
    // revision's bit-identity gate, and it holds by construction rather than by
    // care.
    //
    // Both Arp On transitions are BLOCK-GRANULAR and not stamped, because
    // parameters are not events in this device and never have been — they are
    // read once per block — so the change lands at frame 0 of the block that
    // observes it, exactly as v2's Voice Mode switch and v3's transport
    // re-anchor already do. Every other property of the arp is invariant.
    void readArpParams() {
        const bool on      = (int)clampv(p(kPArpOn) + 0.5f, 0.f, 1.f) != 0;
        const bool wasOn   = arp_.on;
        const bool wasHold = arp_.hold;
        // THE FIRST BLOCK AFTER prepare() IS NOT A TRANSITION. A set that was
        // saved with the arp on has no "off" to come from, and a render that
        // starts at bar 33 must SOUND the step bar 33 implies rather than wait
        // for the next one — which is the locate property, arriving at frame 0
        // of the first block instead of at a jump. So a fresh instance serves
        // the step it lands on, and only a genuine mid-run 0 -> 1 waits.
        const bool fresh   = arpFresh_;
        arpFresh_ = false;

        if (!on) {
            // 1 -> 0 WITH NOTES HELD. Every note the arp generated is released
            // at frame 0 and its bookkeeping is cleared, and notes still
            // physically held do NOT re-sound: a key that was never delivered
            // to the voice engine cannot be resumed without synthesising a
            // note-on the player did not play. The arp may invent MIDI; a
            // parameter change may not. The player re-presses.
            if (wasOn) { arpReleaseSounds(); latchClear(); }
            arp_.on      = false;
            arp_.hold    = false;
            arpStepLvl_  = 0.f;
            arpReanchor_ = false;
            return;
        }

        arp_.on       = true;
        arp_.mode     = (int)clampv(p(kPArpMode) + 0.5f, 0.f, (f32)(kArpModeCount - 1));
        const int sy  = (int)clampv(p(kPArpSync) + 0.5f, 0.f, (f32)(kSpSyncCount - 1));
        arp_.freeRun  = sy == 0;
        arp_.beatsInv = sy > 0 ? 1.0 / (f64)kSpSyncBeats[sy] : 4.0;
        arp_.inc      = (f64)clampv(p(kPArpRate), 0.01f, 40.f) / sr_;
        arp_.octaves  = (int)clampv(p(kPArpOctaves) + 0.5f, 1.f, 4.f);
        arp_.octMode  = (int)clampv(p(kPArpOctMode) + 0.5f, 0.f, 2.f);
        arp_.gate     = (f64)clampv(p(kPArpGate), 1.f, 200.f) * 0.01;
        const f32 sw  = clampv(p(kPArpSwing), 0.f, 100.f);
        arp_.swingOn  = sw > 0.f;             // Swing 0 selects the no-offset
        arp_.swing    = (f64)sw / 300.0;      // branch outright, bit-exact
        arp_.hold     = (int)clampv(p(kPArpHold) + 0.5f, 0.f, 1.f) != 0;
        arp_.retrig   = (int)clampv(p(kPArpRetrig) + 0.5f, 0.f, 1.f) != 0;
        arp_.velMode  = (int)clampv(p(kPArpVelMode) + 0.5f, 0.f, 2.f);
        arp_.fixedVel = (int)clampv(p(kPArpFixedVel) + 0.5f, 1.f, 127.f);
        arp_.steps    = (int)clampv(p(kPArpSteps) + 0.5f, 1.f, (f32)kSpSteps);
        arp_.chance   = (int)clampv(p(kPArpChance) + 0.5f, 0.f, 100.f);
        // ONE atomic load per row: sixteen nibbles in a u64, so the audio
        // thread can never see a row half-written. See publishStateToAudio().
        arp_.lvBits   = arpLvBits_.load(std::memory_order_relaxed);
        arp_.stBits   = arpStBits_.load(std::memory_order_relaxed);

        if (!wasOn) {
            // 0 -> 1 WITH NOTES HELD. Every voice sounding from a direct
            // note-on is RELEASED (an ENV release, not a cut) at frame 0. The
            // held stack is untouched — it was maintained anyway — so the arp
            // starts from the truth. arpOrigin_ is left alone whatever Retrig
            // says, because a parameter change is not a new chord.
            for (Voice& v : voices_) if (v.active && v.e1.stage != kRel) release(v);
            arpDropSounds();
            if (arp_.hold) latchSeedFromHeld(); else latchClear();
            arpFiredK_ = arpCurK(arpAeff()) - (fresh ? 1 : 0);
        } else if (arp_.hold != wasHold) {
            // SWITCHING HOLD WHILE NOTES ARE HELD. 0 -> 1 seeds latchSet from
            // heldSet (empty is legal: the arp stays quiet until a key
            // arrives); 1 -> 0 drops it and the arp immediately plays heldSet,
            // which may also be empty. NEITHER TRANSITION EMITS A NOTE-ON.
            if (arp_.hold) latchSeedFromHeld(); else latchClear();
        }

        // A re-anchor is a block-boundary event by construction — the
        // transport arrives once per block — so on one the arp emits note-offs
        // for what is sounding and resumes at the new position. It NEVER
        // replays skipped steps.
        if (arpReanchor_ && !arp_.freeRun) {
            arpReleaseSounds();
            // Resume AT the new position: the step the bar implies is the step
            // that sounds, which is the whole of what a locate landing on the
            // right step means. Skipped steps are not replayed.
            arpFiredK_ = arpCurK(arpAeff()) - 1;
        }
        arpReanchor_  = false;
        // Swing, Sync and Rate are all knobs, so the next onset is recomputed
        // every block rather than cached across a change.
        arpNextOnset_ = arpOnset(arpFiredK_ + 1);
    }

    // The unison fan: detune spreads SYMMETRICALLY about the centre and the
    // spread pans that same fan, so voice u sits at the same place in pitch and
    // in the image. Panning is constant power and never inverts a polarity,
    // which is the whole reason a mono sum of this cannot cancel.
    static void buildFan(int u, f32 detune, f32 spread,
                         f32* rat, f32* panL, f32* panR, f32* offOut,
                         f32& gain, f32& maxRat) {
        maxRat = 1.f;
        for (int i = 0; i < u; ++i) {
            const f32 off = (u > 1) ? ((f32)i / (f32)(u - 1)) * 2.f - 1.f : 0.f;
            offOut[i] = off;    // kept for the matrix's Detune destinations,
                                // which rebuild the ratios at audio rate
            const f32 ct  = off * detune * 0.5f;          // total spread = detune
            rat[i] = std::exp2(ct * (1.f / 1200.f));
            if (rat[i] > maxRat) maxRat = rat[i];
            const f32 th = (off * spread + 1.f) * 0.7853981f;   // 0 .. pi/2
            panL[i] = 1.4142136f * std::cos(th);
            panR[i] = 1.4142136f * std::sin(th);
        }
        for (int i = u; i < kUni; ++i) { rat[i] = 1.f; panL[i] = 0.f; panR[i] = 0.f; offOut[i] = 0.f; }
        // Detuned voices sum incoherently, so their RMS grows as sqrt(u).
        gain = 1.f / std::sqrt((f32)u);
    }

    f32 atkCoef(f32 ms) const {
        const f32 n = std::fmax(1.f, (f32)(ms * 1e-3 * sr_));
        return clampv(1.f - std::exp(-1.4663371f / n), 1e-7f, 1.f);
    }
    f32 decCoef(f32 ms) const {
        const f32 n = std::fmax(1.f, (f32)(ms * 1e-3 * sr_));
        return clampv(1.f - std::exp(-6.9077553f / n), 1e-7f, 1.f);   // ln(1000)
    }

    // --- LFO ---------------------------------------------------------------

    // One shape read, shared by all three LFOs — the shape list is the
    // contract's id 37 list, verbatim for id 56 and 59 too. Taking the phase as
    // an argument rather than the Lfo is v3's only change to it, because a
    // One-shot LFO's phase lives in a voice and not in the generator; every
    // expression below is v1's, character for character.
    //
    // Shape 5 (Custom) is NOT here. It reads a grid and a filter state that
    // belong to an instance or to a voice, so it has its own two readers below.
    static f32 lfoShapeAt(int shape, f32 ph, f32 sh) {
        switch (shape) {
            case 1:  return ph < 0.5f ? (4.f * ph - 1.f) : (3.f - 4.f * ph);   // triangle
            case 2:  return 2.f * ph - 1.f;                                    // saw up
            case 3:  return ph < 0.5f ? 1.f : -1.f;                            // square
            case 4:  return sh;                                                // sample & hold
            default: return std::sin(dsp::kTwoPi * ph);
        }
    }

    static f32 lfoValue(const dsp::Lfo& l, int shape, f32 sh) {
        return lfoShapeAt(shape, l.phase, sh);
    }

    // --- v3: the drawn grid (shape 5) --------------------------------------

    // The step in effect at phase `ph`. `i = clamp(floor(p*16), 0, 15)` of the
    // SAME phase the other five shapes read, so which cycle the LFO is in and
    // how it got there is unchanged by this shape existing — and the clamp is
    // what makes a One-shot's phase of exactly 1.0 land on step 15 rather than
    // off the end. The division is the length of the WHOLE cycle: sync 5 (1/4)
    // runs all sixteen steps inside one quarter note.
    // ONE atomic load per call: a grid can never be observed half-updated.
    f32 gridAt(int j, f32 ph) const {
        int i = (int)(ph * (f32)kSpSteps);
        if (i < 0) i = 0;
        if (i > kSpSteps - 1) i = kSpSteps - 1;
        const u64 bits = gridBits_[j].load(std::memory_order_relaxed);
        return spStepLevel((u8)((bits >> (4 * i)) & 15u));
    }

    // The smoothing one-pole's coefficient, `a = 1 - exp(-1 / (s * T_step *
    // sr))`. T_step is the cycle period over sixteen, so `T_step * sr` is
    // `1 / (16 * inc)` samples and the whole coefficient collapses to
    // `1 - exp(-16 * inc / s)` — A PURE FUNCTION OF THE LFO'S INCREMENT AND s,
    // with no clock, no elapsed time and nothing a block boundary can move.
    // As s -> 0 it tends to 1 and the filter tends to the s = 0 branch, so the
    // control is continuous across its own default.
    f32 smoothCoef(f32 inc, int j) const {
        const int sm = smoothQ_[j].load(std::memory_order_relaxed);
        if (sm <= 0) return 1.f;             // the s = 0 branch never reads this
        const f32 s = (f32)sm * (1.f / 1000.f);
        return clampv(1.f - std::exp(-(f32)kSpSteps * inc / s), 0.f, 1.f);
    }

    // The instance-wide (Loop mode) read, once per sample per LFO. A smooth of
    // 0 selects the no-filter branch OUTRIGHT — a hard staircase, bit-exact —
    // rather than trusting a coefficient of 1 to be neutral, which is the same
    // discipline Warp Amt 0 and Noise Color 1.0 already follow.
    f32 globLfo(int j, const dsp::Lfo& l, const Blk& b, f32 sh) {
        const int shape = b.lshape[j];
        if (shape != 5) return lfoShapeAt(shape, l.phase, sh);
        // The ruling: SYNCED and with a transport running, the step index comes
        // from the beat, so the sequence locks to the bar line. Free, or with
        // no transport, it is the LFO's own accumulated phase — the same phase
        // the other five shapes read. Neither is a clock: see the file header
        // for why beatAcc_ is anchored to the transport rather than driven by
        // it, which is what keeps blocks of 1 and of 1024 identical.
        f32 ph;
        if (b.beatStep[j]) {
            f64 q = beatAcc_ / (f64)b.lBeats[j];
            q -= std::floor(q);
            ph = (f32)q;
            if (!(ph >= 0.f && ph < 1.f)) ph = 0.f;
        } else {
            ph = l.phase;
        }
        const f32 lv = gridAt(j, ph);
        if (smoothQ_[j].load(std::memory_order_relaxed) <= 0) return lv;
        smY_[j] += (lv - smY_[j]) * smA_[j];
        return smY_[j];
    }

    // Once per process(), before the sample loop. See the file header: the beat
    // counter is ANCHORED to the transport, never driven by it, so it advances
    // on absolute sample time like everything else in this device.
    void syncBeat() {
        const f64 bpm = trBpm_ > 0.0 ? (f64)clampv((f32)trBpm_, 20.f, 999.f) : 120.0;
        beatInc_ = bpm / (60.0 * sr_);
        const bool started   = trPlaying_ && !trPlayWas_;
        // "Demonstrably advancing": a host that pushes the same beat every
        // block is not telling us the time, and re-anchoring to it would make a
        // render depend on its own block size. This is the check that makes the
        // fallback in the ruling's point 3 automatic rather than a mode.
        const bool advancing = trBeatSeen_ && trBeat_ != trBeatWas_;
        if (trPlaying_ &&
            (started || (advancing && std::fabs(trBeat_ - beatAcc_) > (1.0 / 64.0)))) {
            beatAcc_ = trBeat_;
            // v4: the arp is the second consumer of this counter and a jump in
            // it is a locate or a loop wrap. readArpParams() acts on the flag,
            // which is why it is a flag and not the action: the arp's own
            // parameters are not read yet at this point in the block.
            arpReanchor_ = true;
        }
        trBeatWas_  = trBeat_;
        trBeatSeen_ = true;
        trPlayWas_  = trPlaying_;
    }

    // --- v3: One-shot LFOs, the per-voice half -----------------------------

    // dsp::Lfo::setRate's body, for the one-shot increment. Spelled out rather
    // than borrowed because a one-shot's increment lives in a VOICE and there
    // is no dsp::Lfo there to set; the arithmetic is that function's, exactly.
    static f32 rateInc(f64 sr, f32 hz) {
        if (sr <= 0.0 || !(hz > 0.f)) return 0.f;
        return clampv((f32)((f64)hz / sr), 0.f, 0.5f);
    }

    // This sample's value for every one-shot LFO of one voice, read BEFORE any
    // phase advances — the same sentence, and the same reason, as the three
    // instance-wide LFOs in process().
    void voiceLfoRead(Voice& v, const Blk& b) {
        if (v.osSnap) {
            // A note-on has no Blk to read, so the voice takes the block's
            // increment and coefficient on its first rendered sample. The
            // control tick owns both from there.
            for (int j = 0; j < 3; ++j) { v.osInc[j] = b.lInc[j]; v.osA[j] = b.smA[j]; }
            v.osSnap = false;
        }
        for (int j = 0; j < 3; ++j) {
            if (b.lmode[j] != kLfoOneShot) continue;
            const f32 ph = v.osPh[j];
            if (b.lshape[j] != 5) { v.osVal[j] = lfoShapeAt(b.lshape[j], ph, v.osSh[j]); continue; }
            const f32 lv = gridAt(j, ph);
            if (smoothQ_[j].load(std::memory_order_relaxed) <= 0) { v.osVal[j] = lv; continue; }
            v.osY[j] += (lv - v.osY[j]) * v.osA[j];
            v.osVal[j] = v.osY[j];
        }
    }

    // ...and the advance, at the bottom of the sample, beside lfoTicks(). The
    // phase CLAMPS at 1.0 instead of wrapping: that clamp is the whole
    // difference between an LFO and an envelope.
    static void voiceLfoTick(Voice& v, const Blk& b) {
        for (int j = 0; j < 3; ++j) {
            if (b.lmode[j] != kLfoOneShot || v.osPh[j] >= 1.f) continue;
            v.osPh[j] += v.osInc[j];
            if (!(v.osPh[j] >= 0.f)) v.osPh[j] = 0.f;
            if (v.osPh[j] > 1.f)     v.osPh[j] = 1.f;
        }
    }

    // A new sample-and-hold value on every wrap, drawn from that LFO's OWN
    // counter, never the note counter and never each other's -- see the file
    // header: three streams, three seeds, no interleaving.
    static void lfoTickOne(dsp::Lfo& l, u32& rng, f32& sh) {
        const f32 was = l.phase;
        l.tick();
        if (l.phase < was) sh = 2.f * rnd(rng) - 1.f;
    }

    void lfoTicks() {
        lfoTickOne(lfo_,  lfoRng_,  shVal_);
        lfoTickOne(lfo2_, lfo2Rng_, shVal2_);
        lfoTickOne(lfo3_, lfo3Rng_, shVal3_);
        // One sample at a time, for dsp::Lfo::tick's reason and for the same
        // gate. Advanced whether or not any LFO reads it, so its value is a
        // function of time alone and never of the routing.
        beatAcc_ += beatInc_;
    }

    // 24 bits of a plain LCG, in [0, 1). Deterministic and seeded in prepare().
    static f32 rnd(u32& s) {
        s = s * 1664525u + 1013904223u;
        return (f32)(s >> 8) * (1.f / 16777216.f);
    }

    // --- voices ------------------------------------------------------------

    // REALTIME. One slot per event; 128 note events inside a single audio block
    // is not music, and dropping the overflow is better than growing an array
    // on the audio thread.
    //
    // AUDIT-3 F3, applied. It used to drop EVERYTHING once full, which is
    // defensible for a note-on and indefensible for the other three: a note-off
    // and the two panics exist to STOP a voice that is already sounding, and
    // losing one leaves it on for the rest of the session. The audit's own
    // sentence: "a flood that fills the queue with note-ons and then drops the
    // All Notes Off behind them leaves up to 16 voices sounding until the next
    // panic that happens to fit."
    //
    // Two halves, answering two different failures:
    //
    //  1. RESERVED CAPACITY. A note-on may fill at most kOnCap of the kPend
    //     slots; offs and panics may use all of them. A dropped note-on costs
    //     one note that does not sound -- inaudible in a flood this dense and
    //     recoverable in every case. This half covers every stream that has any
    //     relationship to music, and it keeps those events IN THE QUEUE:
    //     stamped, ordered, applied at their own sample. Incident 6's property
    //     is untouched below the threshold, which is the only place it was ever
    //     meaningful.
    //
    //  2. AN OVERFLOW SET THAT CANNOT OVERFLOW. Past that, an off is folded
    //     into a 128-bit note mask and a panic into a two-state flag -- both
    //     O(1), so no flood of any length can lose one. This is the guarantee;
    //     half 1 is the quality.
    //
    // WHY IT LANDS AT THE LAST SAMPLE and not at frame 0, which is what the
    // audit sketched. Frame 0 precedes every event still in the queue, so an
    // overflowed note-off whose note-on is queued at frame 300 would be applied
    // to a voice that does not exist yet -- and the note would stick, which is
    // the exact bug being fixed. The last sample is ordered after everything
    // the queue holds, so nothing can outrun it.
    //
    // AND ON DETERMINISM, against incident 6 directly. Applying an overflowed
    // event late is not block-size invariant. Nothing can be: the queue is
    // per-block, so a block of 1 can never overflow and a block of 1024 can,
    // which makes overflow behaviour block-size dependent BY CONSTRUCTION
    // whatever is done with it. The gate that survives past the threshold is
    // therefore not bit-identity -- it is "no voice is left sounding", and that
    // one is absolute rather than approximate.
    void queue(int frame, u8 type, u8 a, u8 b, u8 ch) {
        if (type == kEvOn || type == kEvPressure) {
            // Both droppable, by the argument above: a pressure value lost in
            // a flood this dense is replaced by the next one.
            if (nPend_ >= kOnCap) return;
        } else if (type == kEvCC || type == kEvBend) {
            // v3. A controller and a bend FOLD rather than drop — the contract
            // puts them under the queue's overflow rule — and they fold at
            // kOnCap rather than at kPend, so the 32 slots this file reserves
            // for note-offs and panics stay reserved for note-offs and panics.
            // Only the LAST value of each survives, and that is right rather
            // than merely convenient: a controller has no history worth
            // keeping, only a current value. Both folds are O(1), so no flood
            // of any length can lose one.
            if (nPend_ >= kOnCap) {
                if (type == kEvCC) { ovfCCNum_ = (i16)a; ovfCCVal_ = (i16)b; }
                else               { ovfBend_  = (i16)((int)a | ((int)b << 7)); }
                haveOvf_ = true;
                return;
            }
        } else if (nPend_ >= kPend) {
            if (type == kEvOff) {
                ovfOff_[(a >> 5) & 3] |= 1u << (a & 31);
            } else if (type == kEvSoundOff) {
                ovfPanic_ = 2;                          // the stronger of the two wins
            } else if (ovfPanic_ == 0) {
                ovfPanic_ = 1;
            }
            haveOvf_ = true;
            return;
        }
        PendEv& e = pend_[nPend_];
        e.frame = frame;
        e.type  = type;
        e.a     = a;
        e.b     = b;
        e.ch    = ch;
        ++nPend_;
    }

    // REALTIME. The overflow set, drained at the block's last sample.
    void applyOverflow() {
        // v3's two folded controllers first: they are values, not actions, and
        // a note the panic below is about to stop should still have seen them.
        if (ovfCCNum_ >= 0) { applyCC((u8)ovfCCNum_, (u8)ovfCCVal_); ovfCCNum_ = -1; }
        if (ovfBend_  >= 0) { bend_ = bendOf(ovfBend_);              ovfBend_  = -1; }
        for (int w = 0; w < 4; ++w) {
            u32 bits = ovfOff_[w];
            while (bits) {
                const int bit = __builtin_ctz(bits);
                bits &= bits - 1u;
                noteOff((u8)(w * 32 + bit));
            }
            ovfOff_[w] = 0u;
        }
        if (ovfPanic_ == 2)      allSoundOff();
        else if (ovfPanic_ == 1) allNotesOff();
        ovfPanic_ = 0;
        haveOvf_  = false;
    }

    void apply(const PendEv& e) {
        switch (e.type) {
            // The absolute timeline sample the event was stamped for — block
            // start plus its in-block frame — is the third leg of the note's
            // stable identity (the matrix's Random source).
            case kEvOn:       noteOn(e.a, e.b, e.ch, absPos_ + (u64)e.frame); break;
            case kEvOff:      noteOff(e.a); break;
            case kEvNotesOff: allNotesOff(); break;
            case kEvPressure: after_ = (f32)e.a * (1.f / 127.f); break;
            case kEvCC:       applyCC(e.a, e.b); break;
            case kEvBend:     bend_ = bendOf((int)e.a | ((int)e.b << 7)); break;
            default:          allSoundOff(); break;
        }
    }

    // v3, matrix sources 14 and 16. One event serves both, because learning
    // CC 1 is legal and redundant (source 14 already has it) and a controller
    // that is both must move both. There is exactly ONE learn slot: every
    // matrix slot whose source is 16 reads the same controller, which is what
    // makes the learn a property of the INSTRUMENT and not of a slot.
    void applyCC(u8 num, u8 val) {
        const f32 v = (f32)val * (1.f / 127.f);
        if (num == 1) wheel_ = v;
        const int learn = ccNum_.load(std::memory_order_relaxed);
        if (learn >= 0 && (int)num == learn) ccVal_ = v;
    }

    // Matrix source 15. `b = (v14 - 8192) / 8192`, domain [-1, +0.999878]. The
    // asymmetry is MIDI's — 8192 is the centre of a 14-bit range whose top
    // value is 16383 — and it is stated rather than hidden by rescaling,
    // because rescaling would make the centre not exactly 0.
    static f32 bendOf(int v14) {
        return (f32)(v14 - 8192) * (1.f / 8192.f);
    }

    // Random-per-note (matrix source 13): a splitmix64-finalised hash of the
    // note's stable identity — channel, note number, and the note-on's
    // absolute timeline sample as stamped on the event. NO stream, no clock:
    // it cannot be perturbed by voice stealing or block size, and the same
    // render yields the same values, which is the contract's own wording.
    static f32 noteRandom(u8 ch, u8 note, u64 absSample) {
        u64 x = absSample ^ ((u64)note << 48) ^ ((u64)ch << 56);
        x += 0x9E3779B97F4A7C15ull;
        x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
        x ^= x >> 27; x *= 0x94D049BB133111EBull;
        x ^= x >> 31;
        return (f32)(x >> 40) * (2.f / 16777216.f) - 1.f;      // [-1, 1)
    }

    // v3, shape 4 under One-shot. "A one-shot wraps exactly once, at phase 0,
    // so it draws one value at note-on and holds it", and the draw is the
    // note's stable identity SALTED BY THE LFO NUMBER — the same construction
    // matrix source 13 uses, so no RNG state is carried between notes and voice
    // stealing cannot perturb it. There is no second hash in this device: this
    // is source 13's, with the identity offset before the finaliser runs.
    static f32 lfoRandom(u8 ch, u8 note, u64 absSample, int j) {
        const u64 salt = (u64)(j + 1) * 0x9E3779B97F4A7C15ull;
        u64 x = (absSample ^ ((u64)note << 48) ^ ((u64)ch << 56)) + salt;
        x += 0x9E3779B97F4A7C15ull;
        x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
        x ^= x >> 27; x *= 0x94D049BB133111EBull;
        x ^= x >> 31;
        return (f32)(x >> 40) * (2.f / 16777216.f) - 1.f;      // [-1, 1)
    }

    // v3. A one-shot LFO retriggers EXACTLY when ENV1 retriggers and never
    // otherwise: in Mono every note-on retriggers it; in Legato an overlapped
    // note-on does NOT, and the note-off fallback to a still-held note does
    // not, because a fallback is not a note-on. That is the same sentence v2
    // wrote about envelopes, and a one-shot LFO is an envelope — so this is
    // called from exactly the two places that set e1.stage = kAtk.
    void oneShotTrigger(Voice& v, u8 ch, u8 note, u64 absSample) {
        for (int j = 0; j < 3; ++j) {
            v.osPh[j]  = 0.f;
            v.osY[j]   = gridAt(j, 0.f);               // the lag starts at L(d_0)
            v.osSh[j]  = lfoRandom(ch, note, absSample, j);
            v.osVal[j] = 0.f;
        }
        v.osSnap = true;
    }

    // The mono/legato held-note stack, newest last. Maintained in EVERY mode
    // (cheap, silent in Poly) so switching into Mono/Legato mid-phrase starts
    // from the truth rather than from an empty memory.
    //
    // v4 GIVES IT TWO PARALLEL ARRAYS AND NOTHING ELSE. Velocity, because Vel
    // Mode 0 (As Played) is the velocity of the key that contributed the note;
    // channel, because the identity hash noteRandom(channel, note, absSample)
    // that every generated note-on feeds needs it. Ordering, depth, the
    // drop-oldest rule and the maintained-in-every-mode rule are untouched —
    // this is data, not semantics. THE ARP READS THIS STACK AND NEVER WRITES
    // IT: generated notes bypass heldPush/heldRemove entirely, because a stack
    // that described notes nobody is holding would make Hold, Retrigger and
    // the mono fallback each wrong in a different way.
    void heldPush(u8 n, u8 vel, u8 ch) {
        if (nHeld_ >= kHeld) {          // drop the oldest: fallback wants newest
            for (int i = 1; i < kHeld; ++i) {
                held_[i - 1]    = held_[i];
                heldVel_[i - 1] = heldVel_[i];
                heldCh_[i - 1]  = heldCh_[i];
            }
            --nHeld_;
        }
        heldVel_[nHeld_] = vel;
        heldCh_[nHeld_]  = ch;
        held_[nHeld_++]  = n;
    }
    void heldRemove(u8 n) {
        int w = 0;
        for (int i = 0; i < nHeld_; ++i) {
            if (held_[i] == n) continue;
            held_[w]    = held_[i];
            heldVel_[w] = heldVel_[i];
            heldCh_[w]  = heldCh_[i];
            ++w;
        }
        nHeld_ = w;
    }

    // --- v4: the latch set ---------------------------------------------------
    //
    // `latchSet` is the arp's OWN set and is in force only while Arp Hold = 1.
    // It is not `heldSet`: note-offs never remove from it, and a NEW CHORD —
    // spectra.cpp's existing `otherHeld == false`, the condition that says
    // heldSet was empty immediately before this note-on joined it — clears it
    // first. Press a chord, release it, the arp keeps running; press one new
    // note and the latch becomes that note alone; press more before releasing
    // and they join.
    void latchClear() { nLatch_ = 0; }
    void latchAdd(u8 n, u8 vel, u8 ch) {
        int w = 0;                       // re-press moves a note to the end,
        for (int i = 0; i < nLatch_; ++i) {   // exactly as heldPush does
            if (latch_[i] == n) continue;
            latch_[w] = latch_[i]; latchVel_[w] = latchVel_[i];
            latchCh_[w] = latchCh_[i]; ++w;
        }
        nLatch_ = w;
        if (nLatch_ >= kHeld) {
            for (int i = 1; i < kHeld; ++i) {
                latch_[i - 1] = latch_[i]; latchVel_[i - 1] = latchVel_[i];
                latchCh_[i - 1] = latchCh_[i];
            }
            --nLatch_;
        }
        latchVel_[nLatch_] = vel;
        latchCh_[nLatch_]  = ch;
        latch_[nLatch_++]  = n;
    }
    void latchSeedFromHeld() {
        nLatch_ = nHeld_;
        for (int i = 0; i < nHeld_; ++i) {
            latch_[i] = held_[i]; latchVel_[i] = heldVel_[i]; latchCh_[i] = heldCh_[i];
        }
    }

    // Retarget a sounding mono voice's pitch: a glide when Glide > 0, a jump
    // when not. Constant TIME, like every other glide in this file.
    void monoGlide(Voice& v, u8 note, f32 glideMs) {
        v.note = note;
        v.pitchTarget = (f32)note;
        const int nsteps = (int)(glideMs * 1e-3 * sr_);
        if (glideMs > 0.f && nsteps > 0) {
            v.glideStep = (v.pitchTarget - v.pitch) / (f32)nsteps;
            v.glideLeft = nsteps;
        } else {
            v.pitch = v.pitchTarget;
            v.glideLeft = 0;
        }
    }

    // An INCOMING note-on. The stack is updated in every mode and whatever the
    // arp is doing — it is the record of physical keys — and then exactly one
    // of two things happens: with the arp off the note reaches the voices as it
    // always has, and with the arp on it does not reach them at all, because
    // "incoming notes stop reaching the voices directly and the arp generates
    // the notes instead" is what id 109 means.
    void noteOn(u8 note, u8 vel, u8 ch, u64 absSample) {
        const bool otherHeld = nHeld_ > 0;   // before this note joins: the
                                             // contract's NEW CHORD condition
        heldRemove(note);
        heldPush(note, vel, ch);
        if (arp_.on) { arpNoteOn(note, vel, ch, otherHeld); return; }
        noteOnVoice(note, vel, ch, absSample, otherHeld);
    }

    // The voice half, shared by an incoming note-on and by one the arp
    // invented. `otherHeld` is what selects a Legato overlap; for a generated
    // note the arp passes "another generated note is still sounding", which is
    // the same question asked of the set the arp actually owns.
    void noteOnVoice(u8 note, u8 vel, u8 ch, u64 absSample, bool otherHeld) {
        const int vm = (int)clampv(p(kPVoiceMode) + 0.5f, 0.f, 2.f);

        if (vm == 0) { polyNoteOn(note, vel, ch, absSample); return; }

        // Mono / Legato: voices_[0] is THE voice; id 39 is ignored (contract).
        Voice& v = voices_[0];
        const bool sounding = v.active;
        const f32 glideMs = clampv(p(kPGlide), 0.f, 2000.f);

        if (vm == 2 && sounding && otherHeld) {
            // Legato overlap: no retrigger of anything — the note only
            // glides. Velocity keeps the phrase's value; the per-note Random
            // updates to the new note's identity (it IS a note-on).
            v.rnote = noteRandom(ch, note, absSample);
            monoGlide(v, note, glideMs);
            lastPitch_ = (f32)note;
            havePitch_ = true;
            v.age = ++age_;
            return;
        }

        // Mono retrigger, or a detached Legato note. A voice that is still
        // sounding keeps its phases and filter state and restarts ENV1-3 from
        // their CURRENT values — a retrigger is a new attack, not a click; a
        // silent voice starts exactly like a Poly note.
        const f32 fromPitch = sounding ? v.pitch : lastPitch_;
        const bool canGlide = sounding || havePitch_;
        if (!sounding) {
            v = Voice{};
            for (int i = 0; i < kUni; ++i) {
                v.phA[i] = rnd(noteRng_);
                v.phB[i] = rnd(noteRng_);
            }
            v.subPh = rnd(noteRng_);
            v.rng   = (noteRng_ = noteRng_ * 1664525u + 1013904223u) | 1u;
        }
        v.active = true;
        v.note   = note;
        v.velAmp = 0.30f + 0.70f * ((f32)vel * (1.f / 127.f));
        v.vel01  = (f32)vel * (1.f / 127.f);
        v.rnote  = noteRandom(ch, note, absSample);
        v.pitchTarget = (f32)note;
        if (glideMs > 0.f && canGlide) {
            const int nsteps = (int)(glideMs * 1e-3 * sr_);
            if (nsteps > 0) {
                v.pitch     = fromPitch;
                v.glideStep = (v.pitchTarget - v.pitch) / (f32)nsteps;
                v.glideLeft = nsteps;
            } else {
                v.pitch = v.pitchTarget;
                v.glideLeft = 0;
            }
        } else {
            v.pitch = v.pitchTarget;
            v.glideLeft = 0;
        }
        lastPitch_ = v.pitchTarget;
        havePitch_ = true;
        v.e1.stage = kAtk;
        v.e2.stage = kAtk;
        v.e3.stage = kAtk;
        oneShotTrigger(v, ch, note, absSample);
        v.age = ++age_;
    }

    // The v1 note-on, verbatim plus the v2 per-note fields (vel01, rnote, e3).
    void polyNoteOn(u8 note, u8 vel, u8 ch, u64 absSample) {
        Voice* pv = alloc();
        Voice& v = *pv;
        v = Voice{};
        v.active = true;
        v.note   = note;
        // Velocity with a 30% floor (the contract's fixed routing): the softest
        // possible note is quiet, not inaudible.
        v.velAmp = 0.30f + 0.70f * ((f32)vel * (1.f / 127.f));
        v.vel01  = (f32)vel * (1.f / 127.f);
        v.rnote  = noteRandom(ch, note, absSample);
        v.pitchTarget = (f32)note;

        const f32 glideMs = clampv(p(kPGlide), 0.f, 2000.f);
        if (glideMs > 0.f && havePitch_) {
            // Constant TIME: the whole interval is covered in `glideMs`,
            // whatever the interval is.
            const int nsteps = (int)(glideMs * 1e-3 * sr_);
            if (nsteps > 0) {
                v.pitch     = lastPitch_;
                v.glideStep = (v.pitchTarget - v.pitch) / (f32)nsteps;
                v.glideLeft = nsteps;
            } else {
                v.pitch = v.pitchTarget;
            }
        } else {
            v.pitch = v.pitchTarget;
        }
        lastPitch_ = v.pitchTarget;
        havePitch_ = true;

        // Phases randomised per note-on from the note counter. Starting every
        // unison voice at zero would make the fan sum to one loud in-phase
        // transient and would make every note identical; a clock would make the
        // render unreproducible.
        for (int i = 0; i < kUni; ++i) {
            v.phA[i] = rnd(noteRng_);
            v.phB[i] = rnd(noteRng_);
        }
        v.subPh = rnd(noteRng_);
        v.rng   = (noteRng_ = noteRng_ * 1664525u + 1013904223u) | 1u;

        v.e1.stage = kAtk; v.e1.v = 0.f;
        v.e2.stage = kAtk; v.e2.v = 0.f;
        v.e3.stage = kAtk; v.e3.v = 0.f;
        oneShotTrigger(v, ch, note, absSample);
        v.fs[0].reset();
        v.fs[1].reset();
        v.fSnap = true;
        v.age = ++age_;
    }

    // Newest matching voice first: a repeated note that stole its own older
    // voice should release the one actually sounding.
    void noteOff(u8 note) {
        heldRemove(note);
        // With the arp on, an incoming note-off changes the SET and nothing
        // else: the arp's own note-offs are the truth, and note-offs never
        // remove from the latch.
        if (arp_.on) return;
        noteOffVoice(note, true);
    }

    // The voice half. `fallback` is v2's mono note-off fallback to the most
    // recent still-held note, and the arp passes false: with the arp running, a
    // fallback would sound a note the arp did not schedule, which is inventing
    // MIDI at a note-off. The fallback returns the instant Arp On goes to 0.
    void noteOffVoice(u8 note, bool fallback) {
        const int vm = (int)clampv(p(kPVoiceMode) + 0.5f, 0.f, 2.f);

        if (vm != 0) {
            Voice& v = voices_[0];
            // Voices left over from a Poly phrase (the mode switched while a
            // chord rang) still honour their note-offs — nothing may strand.
            for (size_t i = 1; i < (size_t)kSpVoices; ++i) {
                Voice& o = voices_[i];
                if (o.active && o.note == note && o.e1.stage != kRel) release(o);
            }
            if (!v.active || v.note != note || v.e1.stage == kRel) return;
            if (fallback && nHeld_ > 0) {
                // Fall back to the most recent still-held note (contract). A
                // glide, not a retrigger: a fallback is not a note-on, so the
                // envelopes keep running — see the implementation notes.
                const u8 back = held_[nHeld_ - 1];
                monoGlide(v, back, clampv(p(kPGlide), 0.f, 2000.f));
                lastPitch_ = (f32)back;
                havePitch_ = true;
            } else {
                release(v);
            }
            return;
        }

        Voice* best = nullptr;
        for (Voice& v : voices_) {
            if (!v.active || v.note != note) continue;
            if (v.e1.stage == kRel) continue;
            if (!best || v.age > best->age) best = &v;
        }
        if (best) release(*best);
    }

    static void release(Voice& v) {
        if (v.e1.stage != kRel) v.e1.stage = kRel;
        if (v.e2.stage != kRel) v.e2.stage = kRel;
    }

    // CC 123. It empties heldSet — it already did — and v4 adds that it also
    // empties latchSet, so the arp's sounding generated notes release with
    // everything else and the arp emits nothing until a note-on arrives. A
    // panic a latch could outlive would not be a panic. The sounding-note
    // bookkeeping is KEPT: those voices are releasing, not gone, and a later
    // gate expiry hitting a voice already in release is a no-op.
    void allNotesOff() {
        for (Voice& v : voices_) if (v.active && v.e1.stage != kRel) release(v);
        nHeld_ = 0;
        latchClear();
    }
    // CC 120 does the same and additionally clears the arp's sounding-note
    // bookkeeping, since the voices it referred to are gone.
    void allSoundOff() {
        for (Voice& v : voices_) v = Voice{};
        nHeld_ = 0;
        latchClear();
        arpDropSounds();
    }

    // -----------------------------------------------------------------------
    // v4: the arpeggiator's engine
    // -----------------------------------------------------------------------

    // `A` — steps elapsed since the arp's origin, a real number and a function
    // of the engine's clock and of nothing else.
    //
    //  * SYNCED (Arp Sync > 0): A = beatAcc_ / beatsPerStep, where beatAcc_ is
    //    v3's OWN anchored beat counter, the same variable read the same way —
    //    an f64 beat position advanced one sample at a time at the pushed
    //    tempo, ANCHORED to the pushed transport beat and never driven by it.
    //    See the ORCHESTRATOR RULING in this file's header; the arp is its
    //    second consumer and adds nothing to it. This one expression covers
    //    the ruling's point 3 as well without a branch: with no transport
    //    running, beatAcc_ simply accumulates from prepare() at the pushed
    //    tempo or its 120 fallback, which IS "A accumulates bpm/(60·B·sr) per
    //    sample".
    //  * FREE (Arp Sync = 0): A accumulates ArpRate/sr per sample from
    //    prepare() and the transport is not read at all.
    f64 arpAbs() const {
        return arp_.freeRun ? arpFree_ : beatAcc_ * arp_.beatsInv;
    }
    // Aeff = A - arpOrigin_, clamped at 0 from below.
    f64 arpAeff() const {
        const f64 a = arpAbs() - arpOrigin_;
        return a > 0.0 ? a : 0.0;
    }

    // onset(k) = k + (k & 1) * (Swing / 300). ODD `k` IS DELAYED and `k` is the
    // ABSOLUTE step number, never the pattern index — so an odd-length pattern
    // does not flip the swing on every loop and the swing stays welded to the
    // beat where a listener expects it. Swing 0 selects the no-offset branch
    // outright, bit-exact.
    f64 arpOnset(i64 k) const {
        const f64 base = (f64)k;
        if (!arp_.swingOn || !(k & 1)) return base;
        return base + arp_.swing;
    }

    // The largest k with onset(k) <= aeff, as a PURE FUNCTION of aeff — never a
    // counter. Swing is at most 1/3 of a step, so the onsets stay strictly
    // increasing and the answer is floor(aeff) or one less.
    i64 arpCurK(f64 aeff) const {
        const f64 fl = std::floor(aeff);
        const i64 f = (i64)fl;
        if (f < 0) return -1;
        if (!arp_.swingOn || !(f & 1)) return f;
        return (aeff - fl >= arp_.swing) ? f : f - 1;
    }

    // The identity the two draws are built on: THE NOTE SET, folded ASCENDING,
    // and the absolute step number. Since k = L·Steps + i, the pair (step
    // index, loop counter) IS k, so the identity is two terms and not three.
    //
    // Ascending DELIBERATELY, even in As Played mode: playing C-E-G and playing
    // G-E-C are the same chord, and a random pattern that changed because a
    // player rolled the chord the other way would be a bug the player could
    // hear and never explain.
    static u64 arpMix64(u64 x) {
        x += 0x9E3779B97F4A7C15ull;
        x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
        x ^= x >> 27; x *= 0x94D049BB133111EBull;
        x ^= x >> 31;
        return x;
    }
    static u64 arpSetHash(const u8* asc, int c) {
        u64 h = arpMix64((u64)c);
        for (int i = 0; i < c; ++i) h = arpMix64(h ^ (u64)asc[i]);
        return h;
    }
    static u64 arpHash(u64 setHash, i64 k, int salt) {
        return arpMix64(setHash ^ (u64)k ^ ((u64)(u32)salt << 56));
    }

    // The mode's cycle length over a set of size c. `max(2c-2, 1)` because at
    // c = 1 the natural formula gives a cycle of length 0, which is not a
    // cycle: modes 3 and 4 degenerate to Up on a single note, and Thumb and
    // Pinky get the same guard.
    static int arpCycle(int mode, int c) {
        switch (mode) {
            case kArpUpDownInc: return 2 * c;
            case kArpUpDownExc:
            case kArpDownUp:    return c > 1 ? 2 * c - 2 : 1;
            case kArpChord:     return 1;
            case kArpThumb:
            case kArpPinky:     return c > 1 ? 2 * (c - 1) : 1;
            default:            return c;      // Up, Down, As Played, Random
        }
    }

    // The element at cycle position j, as an INDEX into the ascending set (or,
    // for As Played, into the insertion-order one). Chord is handled by the
    // caller: its cycle length is 1 and it sounds ALL of N on every step, which
    // is not a special case in the arithmetic — with M = 1 the octave axis
    // advances on every step, so Chord over two octaves alternates the chord at
    // +0 and the chord at +1, which is what a chord arp is for.
    static int arpElement(int mode, int c, int j, u64 setHash, i64 k) {
        switch (mode) {
            case kArpUp:        return j;
            case kArpDown:      return c - 1 - j;
            case kArpUpDownInc: return j < c ? j : 2 * c - 1 - j;
            case kArpUpDownExc: return j < c ? j : 2 * c - 2 - j;
            case kArpDownUp:    return j < c ? c - 1 - j : j - c + 1;
            case kArpAsPlayed:  return j;      // read from H, not N
            case kArpRandom: {
                // A multiply-shift over the top 24 bits, not a modulo: integer
                // arithmetic with no bias argument to have and no float in the
                // path. Random is a DRAW, not a walk — its cycle length is
                // still c so the octave axis and the loop counter advance
                // exactly as they do under Up, and it does not avoid immediate
                // repeats, because a repeat-avoiding draw carries state.
                const u64 h = arpHash(setHash, k, 1);
                return (int)(u32)(((h >> 40) * (u64)(u32)c) >> 24);
            }
            case kArpThumb:     return (j & 1) ? 1 + (j - 1) / 2 : 0;
            case kArpPinky:     return (j & 1) ? c - 1 : j / 2;
            default:            return 0;
        }
    }

    // The octave axis. Alternate is itself an up-down-EXCLUSIVE cycle, which is
    // why its length is 2O-2 and not 2O: an inclusive one would sit on the top
    // octave for two whole note-cycles.
    static int arpOctLen(int octMode, int o) {
        if (octMode != kArpOctAlt) return o;
        return o > 1 ? 2 * o - 2 : 1;
    }
    static int arpOctOffset(int octMode, int o, int u) {
        if (octMode == kArpOctUp)   return u;
        if (octMode == kArpOctDown) return -u;
        return u < o ? u : 2 * o - 2 - u;
    }

    // --- generated events ---------------------------------------------------

    void arpDropSounds() { nArpSnd_ = 0; }

    // Release everything the arp has started, and forget it. Used by the
    // Arp On 1 -> 0 transition and by a transport re-anchor.
    void arpReleaseSounds() {
        for (int i = 0; i < nArpSnd_; ++i) noteOffVoice(arpSnd_[i].note, false);
        arpDropSounds();
    }

    void arpEmitOn(u8 note, u8 vel, u8 ch, u64 absSample, f64 off) {
        // "another generated note is still sounding" is the arp's own reading
        // of the Legato-overlap condition, so gate > 100 % under Legato is a
        // legato arp — which is the reason to reach for that combination.
        noteOnVoice(note, vel, ch, absSample, nArpSnd_ > 0);
        if (nArpSnd_ >= kArpSounds) {
            noteOffVoice(arpSnd_[0].note, false);          // unreachable in
            for (int i = 1; i < nArpSnd_; ++i) arpSnd_[i - 1] = arpSnd_[i];
            --nArpSnd_;                                    // practice; see the cap
        }
        arpSnd_[nArpSnd_].note  = note;
        arpSnd_[nArpSnd_].ch    = ch;
        arpSnd_[nArpSnd_].off   = off;
        ++nArpSnd_;
    }

    void arpRemoveSound(int i) {
        for (int j = i + 1; j < nArpSnd_; ++j) arpSnd_[j - 1] = arpSnd_[j];
        --nArpSnd_;
    }

    // Every generated note whose gate has expired, at this sample. Called once
    // per sample and, inside a step that sounds, BEFORE the new note-ons — the
    // contract's offs-before-ons.
    void arpExpire(f64 aeff) {
        for (int i = 0; i < nArpSnd_;) {
            if (arpSnd_[i].off <= aeff) { noteOffVoice(arpSnd_[i].note, false); arpRemoveSound(i); }
            else ++i;
        }
    }

    // --- the note set -------------------------------------------------------
    //
    // Two sets exist and they are not the same set: heldSet is the physical
    // keys, latchSet is the arp's own and is in force only while Hold = 1.
    int arpSet(u8* asc, u8* ascVel, u8* ascCh,
               u8* ord, u8* ordVel, u8* ordCh) const {
        const u8* src   = arp_.hold ? latch_    : held_;
        const u8* sVel  = arp_.hold ? latchVel_ : heldVel_;
        const u8* sCh   = arp_.hold ? latchCh_  : heldCh_;
        const int c     = arp_.hold ? nLatch_   : nHeld_;
        for (int i = 0; i < c; ++i) { ord[i] = src[i]; ordVel[i] = sVel[i]; ordCh[i] = sCh[i]; }
        // Insertion sort into ascending order. c is at most 64 and a step is at
        // most a few dozen a second, so the quadratic is free and the code is
        // one thing a reader can check.
        for (int i = 0; i < c; ++i) {
            int j = i;
            const u8 n = ord[i], v = ordVel[i], ch = ordCh[i];
            while (j > 0 && asc[j - 1] > n) {
                asc[j] = asc[j - 1]; ascVel[j] = ascVel[j - 1]; ascCh[j] = ascCh[j - 1];
                --j;
            }
            asc[j] = n; ascVel[j] = v; ascCh[j] = ch;
        }
        return c;
    }

    // --- one step -----------------------------------------------------------

    void arpFireStep(i64 k, int frame, f64 aeff) {
        const int i = (int)(k % (i64)arp_.steps);
        const u8  lv = (u8)((arp_.lvBits >> (4 * i)) & 15u);
        const u8  code = (u8)((arp_.stBits >> (4 * i)) & 15u);

        // Matrix source 17 follows the STEP CLOCK and not the notes: an OFF
        // step, a tie, a dropped Chance draw and an empty note set all leave it
        // reading the grid's level at the current index. A staircase that
        // dropped to zero every rest would be a different and much worse
        // control.
        arpStepLvl_ = spStepLevel(lv);

        if (!spArpOn(code)) {           // REST. Tie is not read, and the
            arpExpire(aeff);            // previous note ends at its own gate,
            return;                     // unaffected.
        }
        if (spArpTie(code)) {
            // HOLD the previous note through this step: no new note-on, no new
            // note-off, and this step's octave offset is IGNORED — there is no
            // new note to offset. The hold itself was already decided when the
            // note STARTED (see arpTieRun below), because the contract's own
            // formula is `off(k) = onset(k + m) + Gate/100` and a note whose
            // gate is under 100 % would otherwise have ended before the tie
            // step arrived to extend it. So a tie step does nothing here but
            // advance the clock and the staircase — and A TIE WHOSE
            // PREDECESSOR DID NOT SOUND IS SILENT falls out with no state at
            // all: there is nothing to hold, so nothing is held.
            arpExpire(aeff);
            return;
        }

        // SOUND a new note.
        u8 asc[kHeld], ascVel[kHeld], ascCh[kHeld];
        u8 ord[kHeld], ordVel[kHeld], ordCh[kHeld];
        const int c = arpSet(asc, ascVel, ascCh, ord, ordVel, ordCh);
        if (c <= 0) { arpExpire(aeff); return; }

        const bool needHash = arp_.mode == kArpRandom || arp_.chance < 100;
        const u64 setHash = needHash ? arpSetHash(asc, c) : 0ull;

        // Chance is tested ONLY on steps that would sound a new note, and a
        // step that loses its draw is silent but still advances every index:
        // Chance drops notes, it does not stall the melody. At 100 the branch
        // is SELECTED OUT and no hash is computed at all.
        if (arp_.chance < 100) {
            const u64 h = arpHash(setHash, k, 2);
            if (!((u32)(((h >> 40) * 100u) >> 24) < (u32)arp_.chance)) {
                arpExpire(aeff);
                return;
            }
        }

        // The two axes, note fast and octave slow: the note counter advances
        // FIRST, so the arp completes one full traversal of the note cycle
        // before the octave moves. Up-Down over two octaves therefore bounces
        // inside octave 0, then inside octave 1 — the contract names that
        // consequence and delegates the full-span bounce to the step row's own
        // octave column, which is exactly the control that expresses it.
        const int M = arpCycle(arp_.mode, c);
        const int j = (int)(k % (i64)M);
        const int L = arpOctLen(arp_.octMode, arp_.octaves);
        const int u = (int)((k / (i64)M) % (i64)L);
        const int shift = 12 * (arpOctOffset(arp_.octMode, arp_.octaves, u) + spArpOct(code));

        u8 pn[kHeld], pv[kHeld], pc[kHeld];
        int nP = 0;
        const int first = arp_.mode == kArpChord ? 0 : arpElement(arp_.mode, c, j, setHash, k);
        const int last  = arp_.mode == kArpChord ? c - 1 : first;
        for (int e = first; e <= last; ++e) {
            const int idx = e < 0 ? 0 : (e >= c ? c - 1 : e);
            const bool played = arp_.mode == kArpAsPlayed;
            const int pitch = (int)(played ? ord[idx] : asc[idx]) + shift;
            // A PITCH OUTSIDE 0..127 MAKES THE STEP SILENT. It is not clamped:
            // a clamped note is a wrong note played confidently, and an arp
            // four octaves up from a top-C chord should run out of keyboard
            // rather than pile onto the last one. The step still advances every
            // index; only the note is not emitted.
            if (pitch < 0 || pitch > 127) continue;
            pn[nP] = (u8)pitch;
            pc[nP] = played ? ordCh[idx] : ascCh[idx];
            const u8 asPlayed = played ? ordVel[idx] : ascVel[idx];
            pv[nP] = arp_.velMode == kArpVelFixed   ? (u8)arp_.fixedVel
                   : arp_.velMode == kArpVelPattern
                        // Pattern is ABSOLUTE, not a scaling of the played
                        // velocity, and its floor is 1: 0 is a note-off on this
                        // device's wire, so a step drawn at the bottom of the
                        // row would emit nothing instead of emitting quietly.
                        ? (u8)(1 + (int)(126.f * spStepLevel(lv) + 0.5f))
                        : (asPlayed > 0 ? asPlayed : (u8)1);
            ++nP;
        }
        if (nP == 0) { arpExpire(aeff); return; }

        // OFFS BEFORE ONS, in two waves. First the ordinary gate expiries;
        // then, for every note number about to sound that is still sounding, ITS
        // off — because noteOff() releases the NEWEST matching voice, so a
        // generated off arriving AFTER the next step's on would release the note
        // just started and leave the old one ringing forever. The honest
        // consequence is that gate > 100 % cannot overlap a note with itself;
        // overlap happens between DIFFERENT note numbers, which is the only
        // place overlap means anything.
        arpExpire(aeff);
        for (int q = 0; q < nP; ++q) {
            for (int s = 0; s < nArpSnd_;) {
                if (arpSnd_[s].note == pn[q]) { noteOffVoice(pn[q], false); arpRemoveSound(s); }
                else ++s;
            }
        }
        // `off(k) = onset(k + m) + Gate/100`, m the run of tie steps that
        // follows. A LOOKAHEAD and not an accumulation: which steps are ties is
        // a pure function of (k + t) mod Steps, so this reads the grid and
        // carries nothing.
        const f64 off = arpOnset(k + arpTieRun(k)) + arp_.gate;
        const u64 abs = absPos_ + (u64)frame;
        for (int q = 0; q < nP; ++q) arpEmitOn(pn[q], pv[q], pc[q], abs, off);
    }

    // How many steps after k are ties, capped at kArpMaxTie so that no reading
    // of the grid can produce an unbounded note — the only failure mode here a
    // user could not recover from by releasing a key. The cap is unreachable in
    // practice: a run of sixteen ties needs every step of the pattern to be one,
    // and then no step ever STARTS a note, so the pattern is silent.
    int arpTieRun(i64 k) const {
        int m = 0;
        while (m < kArpMaxTie) {
            const int i = (int)((k + m + 1) % (i64)arp_.steps);
            if (!spArpTie((u8)((arp_.stBits >> (4 * i)) & 15u))) break;
            ++m;
        }
        return m;
    }

    // One sample of the arp, called from the per-sample event loop AFTER every
    // incoming event for this sample has been applied — the contract's fixed
    // ordering, incoming MIDI first and then the arp.
    void arpTick(int frame) {
        const f64 aeff = arpAeff();
        if (aeff >= arpNextOnset_) {
            i64 k = arpCurK(aeff);
            // A guard and nothing more: eight steps inside one sample needs a
            // rate three orders of magnitude past anything this device can be
            // set to. If it ever happened, the arp resumes at the current step
            // rather than replaying a burst.
            if (arpFiredK_ < k - 8) arpFiredK_ = k - 1;
            for (i64 s = arpFiredK_ + 1; s <= k; ++s) arpFireStep(s, frame, aeff);
            arpFiredK_   = k;
            arpNextOnset_ = arpOnset(k + 1);
        } else if (nArpSnd_ > 0) {
            arpExpire(aeff);
        }
        if (arp_.freeRun) arpFree_ += arp_.inc;
    }

    // An incoming note-on while the arp is on: the latch and the retrigger, and
    // nothing else. One condition drives both, and it is the one the code
    // already computed — heldSet was EMPTY immediately before this note-on
    // joined it. A note-on joining a non-empty set never retriggers, because
    // rolling a chord on would otherwise stutter the pattern once per finger.
    void arpNoteOn(u8 note, u8 vel, u8 ch, bool otherHeld) {
        const bool newChord = !otherHeld;
        if (arp_.hold) {
            if (newChord) latchClear();
            latchAdd(note, vel, ch);
        }
        if (newChord && arp_.retrig) {
            arpOrigin_    = arpAbs();     // at THIS note-on's stamped sample
            arpFiredK_    = -1;
            arpNextOnset_ = arpOnset(0);
        }
    }

    void clearSchedule() {
        nPend_ = 0;
        ovfOff_[0] = ovfOff_[1] = ovfOff_[2] = ovfOff_[3] = 0u;
        ovfPanic_  = 0;
        ovfCCNum_  = -1;
        ovfCCVal_  = 0;
        ovfBend_   = -1;
        haveOvf_   = false;
    }

    // Free voice inside the polyphony cap if there is one, otherwise the
    // QUIETEST -- which is what the contract asks for and is also the least bad
    // answer: the voice a listener is least likely to miss. A releasing voice
    // wins on amplitude automatically, so no separate rule is needed for it.
    //
    // Voices already sounding ABOVE a freshly lowered cap are left alone to
    // finish; lowering polyphony mid-chord should not cut the chord off.
    Voice* alloc() {
        const int cap = (int)clampv(p(kPVoices) + 0.5f, 1.f, (f32)kSpVoices);
        Voice* quietest = &voices_[0];
        f32 best = 1e30f;
        for (int i = 0; i < cap; ++i) {
            Voice& v = voices_[(size_t)i];
            if (!v.active) return &v;
            const f32 amp = v.e1.v * v.velAmp;
            if (amp < best) { best = amp; quietest = &v; }
        }
        return quietest;
    }

    // --- the matrix's source fetch ------------------------------------------

    // One slot source, for one voice, this sample. `v` may be null (the LFO
    // rate destinations are instance-wide; with no active voice the
    // voice-bound sources read 0).
    inline f32 srcValue(int src, const Voice* v, const Blk& b, const Glob& g) const {
        switch (src) {
            // v3: a One-shot LFO belongs to a VOICE, so a slot that reads one
            // reads that voice's value. With no voice — a rate destination with
            // nothing sounding — it reads 0, exactly as every other voice-bound
            // source already does. In Loop mode (the default) this is the v2
            // expression, selected and not computed.
            //
            // The DOMAIN is shape-dependent from v3 on: [-1..1] for shapes 0..4
            // and [0..1] for shape 5, because sixteen levels cannot be
            // symmetric about an exact zero and an all-zero grid must be
            // silence. A bipolar reading is one negative Amt away.
            case kSLfo1:   return b.lmode[0] ? (v ? v->osVal[0] : 0.f) : g.l1;
            case kSLfo2:   return b.lmode[1] ? (v ? v->osVal[1] : 0.f) : g.l2;
            case kSLfo3:   return b.lmode[2] ? (v ? v->osVal[2] : 0.f) : g.l3;
            case kSEnv2:   return v ? v->e2.v : 0.f;
            case kSEnv3:   return v ? v->e3.v : 0.f;
            case kSVel:    return v ? v->vel01 : 0.f;
            case kSKey:    return v ? clampv(((f32)v->note - 60.f) * (1.f / 60.f), -1.f, 1.f) : 0.f;
            case kSAft:    return g.after;
            case kSMac1: case kSMac2: case kSMac3: case kSMac4:
                           return b.macro[src - kSMac1];
            case kSRandom: return v ? v->rnote : 0.f;
            // v3's three MIDI sources. All instance-wide and omni, like the
            // Aftertouch source they follow in every respect, and all three 0
            // after prepare() — bend meaning centre.
            case kSWheel:  return g.wheel;
            case kSBend:   return g.bend;
            case kSCC:     return g.cc;
            // v4's one new source. Instance-wide, a HARD staircase (the arp
            // grid has no smooth companion and does not get one in v4 — an
            // author who wants it smoothed has the matrix curves for shaping
            // and a destination's own lag for the rest), and exactly 0 whenever
            // Arp On is 0 and after prepare() until the first step onset. That
            // last sentence is the inert condition the bit-identity gate rests
            // on.
            case kSArpStep: return g.arpStep;
            default:       return 0.f;
        }
    }

    // One slot's signed contribution: the source, then v3's response curve,
    // then the slot's depth. LINEAR IS A SELECTED BRANCH and not a multiply by
    // one — that is what makes the bit-identity gate hold, and it is the same
    // discipline the whole of v2 is built on.
    inline f32 slotMod(const Blk::Slot& s, const Voice* v,
                       const Blk& b, const Glob& g) const {
        const f32 u = srcValue(s.src, v, b, g);
        return s.amt * (s.curve == kCvLinear ? u : spCurve(s.curve, u));
    }

    // LFO1's value for one voice: the instance generator in Loop mode, the
    // voice's own in One-shot. Every routing that consumes a one-shot LFO
    // becomes per voice, INCLUDING the fixed v1 routings (34, 35, 36).
    static inline f32 lfo1Of(const Voice& v, const Blk& b, const Glob& g) {
        return b.lmode[0] ? v.osVal[0] : g.l1;
    }

    // The slot sums for the two control-tick destinations (Cutoff, Resonance).
    inline void cutResMods(const Blk& b, const Voice& v, const Glob& g,
                           f32& mdCut, f32& mdRes) const {
        mdCut = 0.f;
        mdRes = 0.f;
        for (int k = 0; k < b.mN; ++k) {
            if (b.slot[k].dst == kDCut)
                mdCut += slotMod(b.slot[k], &v, b, g);
            else if (b.slot[k].dst == kDRes)
                mdRes += slotMod(b.slot[k], &v, b, g);
        }
    }

    // --- per-voice cutoff --------------------------------------------------

    // `matrixed` selects between the v1 expression — kept verbatim, because
    // "mathematically the same" is not "bit-identical" — and the contract's
    // norm-domain sum: clamp(base_norm + fixed_v1 + Σ, 0, 1) mapped to Hz
    // after the clamp (then the engine's fcMax guard, as always).
    f32 voiceCutoff(const Voice& v, const Blk& b, f32 lfoV, bool matrixed, f32 mdCut) const {
        const f32 kt = b.keytrack * ((f32)v.note - 60.f) * (1.f / 12.f);
        const f32 e2 = b.env2Cut * v.e2.v * kEnvCutOct;
        const f32 lf = b.lfoCut * lfoV * kLfoCutOct;
        if (!matrixed)
            return clampv(b.cutoff * std::exp2(kt + e2 + lf), 20.f, b.fcMax);
        const f32 norm = clampv(b.cutNorm + (kt + e2 + lf) * (1.f / kCutOct) + mdCut, 0.f, 1.f);
        return clampv(20.f * std::exp2(norm * kCutOct), 20.f, b.fcMax);
    }

    f32 voiceQ(const Blk& b, bool matrixed, f32 mdRes) const {
        if (!matrixed) return b.q;
        return 0.5f * std::pow(40.f, clampv(b.resNorm + mdRes, 0.f, 1.f));
    }

    // The Noise Color coefficient for one voice. Track multiplies fc by
    // f_note/261.63 — C4 reference, post-glide, i.e. 2^((pitch-60)/12).
    f32 noiseCoef(const Voice& v, const Blk& b) const {
        f32 fc = b.nzFc;
        if (b.nzTrack) fc *= std::exp2((v.pitch - 60.f) * (1.f / 12.f));
        fc = clampv(fc, 1.f, (f32)(sr_ * 0.49));
        return clampv(1.f - std::exp(-dsp::kTwoPi * fc / (f32)sr_), 1e-5f, 1.f);
    }

    // One control tick: every sounding voice gets a new coefficient target and
    // the per-sample slope that walks it there over the next kCtrl samples.
    // The matrix's control-tick destinations live here too: Cutoff and
    // Resonance per voice (the same cadence v1's LFO->cutoff already has), and
    // the three LFO rates instance-wide.
    void retarget(const Blk& b, const Glob& g) {
        static constexpr int kRateDst[3] = { kDL1Rate, kDL2Rate, kDL3Rate };
        if (b.dstMask & ((1u << kDL1Rate) | (1u << kDL2Rate) | (1u << kDL3Rate))) {
            // An LFO has one phase for all voices, so a per-voice source
            // driving its rate needs one voice picked: the NEWEST active one
            // (the musically obvious choice for the mono patches this is
            // for); none active reads the voice-bound sources as 0.
            //
            // v3: THAT RULE IS FOR LOOP MODE AND ONLY FOR IT. A one-shot LFO
            // does belong to a voice, so its rate is evaluated per voice, in
            // the loop further down; the newest-voice rule exists only because
            // a Loop LFO has no voice to belong to.
            const Voice* nv = nullptr;
            for (const Voice& v : voices_)
                if (v.active && (!nv || v.age > nv->age)) nv = &v;
            dsp::Lfo* ls[3] = { &lfo_, &lfo2_, &lfo3_ };
            for (int j = 0; j < 3; ++j) {
                if (!(b.dstMask & (1u << kRateDst[j])) || !b.lfree[j]) continue;
                if (b.lmode[j] == kLfoOneShot) continue;
                f32 s = 0.f;
                for (int k = 0; k < b.mN; ++k)
                    if (b.slot[k].dst == kRateDst[j])
                        s += slotMod(b.slot[k], nv, b, g);
                const f32 norm = clampv(b.lRateNorm[j] + s, 0.f, 1.f);
                ls[j]->setRate(sr_, 0.01f * std::exp2(norm * kRateOct));
            }
        }

        // v3: the drawn grid's smoothing coefficient, recomputed HERE — at the
        // absolute-timed control tick, the cadence the contract names and for
        // the block-size-invariance reason v2's matrix destinations use.
        // Instance-wide for a Loop LFO, per voice for a One-shot one.
        if (b.anyCustom) {
            const dsp::Lfo* ls[3] = { &lfo_, &lfo2_, &lfo3_ };
            for (int j = 0; j < 3; ++j)
                if (b.lshape[j] == 5 && b.lmode[j] == kLfoLoop)
                    smA_[j] = smoothCoef(ls[j]->inc, j);
        }
        if (b.anyOneShot) {
            for (Voice& v : voices_) {
                if (!v.active) continue;         // NOT gated on fSnap: a voice's
                                                 // one-shot phase runs from its
                                                 // very first sample
                for (int j = 0; j < 3; ++j) {
                    if (b.lmode[j] != kLfoOneShot) continue;
                    f32 inc = b.lInc[j];
                    if (b.rateSlot[j]) {
                        f32 s = 0.f;
                        for (int k = 0; k < b.mN; ++k)
                            if (b.slot[k].dst == kRateDst[j])
                                s += slotMod(b.slot[k], &v, b, g);
                        const f32 norm = clampv(b.lRateNorm[j] + s, 0.f, 1.f);
                        inc = rateInc(sr_, 0.01f * std::exp2(norm * kRateOct));
                    }
                    v.osInc[j] = inc;
                    if (b.lshape[j] == 5) v.osA[j] = smoothCoef(inc, j);
                }
            }
        }

        const f32 invk = 1.f / (f32)kCtrl;
        const bool mc = (b.dstMask & (1u << kDCut)) != 0;
        const bool mr = (b.dstMask & (1u << kDRes)) != 0;
        for (Voice& v : voices_) {
            if (!v.active || v.fSnap) continue;      // fSnap: snapped on first sample
            f32 mdC = 0.f, mdR = 0.f;
            if (mc || mr) cutResMods(b, v, g, mdC, mdR);
            const dsp::SvfCoeffs tgt =
                dsp::svfCoeffs(sr_, voiceCutoff(v, b, lfo1Of(v, b, g), mc, mdC),
                               voiceQ(b, mr, mdR));
            v.fInc = dsp::svfSlope(v.fc, tgt, invk);
            if (!b.nzBypass) v.nzCoef = noiseCoef(v, b);
        }
    }

    // --- the oscillator's shared selection ----------------------------------

    // Frame pair and mip pair for one oscillator this sample. This is v1's
    // arithmetic hoisted out of the read loop, expression for expression,
    // because the FM/RM tap reads share it with the main read.
    struct OscSel { int f0, f1, m0; f32 ff, mf; };

    static inline OscSel oscSelect(f32 pos, f32 inc, f32 maxRat) {
        OscSel s;
        const f32 fpos = clampv(pos, 0.f, 1.f) * (f32)(kSpFrames - 1);
        int f0 = (int)fpos;
        if (f0 > kSpFrames - 2) f0 = kSpFrames - 2;
        if (f0 < 0) f0 = 0;
        s.ff = fpos - (f32)f0;
        s.f0 = f0;
        s.f1 = f0 + 1;

        const f32 mipf = spLog2(inc * maxRat) + 12.f;
        int m0 = (int)mipf;
        f32 mf = mipf - (f32)m0;
        if (m0 < 0)             { m0 = 0; mf = 0.f; }
        if (m0 > kSpMips - 2)   { m0 = kSpMips - 2; mf = 1.f; }
        s.m0 = m0;
        s.mf = mf;
        return s;
    }

    // The FM/RM modulator tap: this osc's voice 0 read at its RAW phase
    // (pre-warp, pre-level, mono), at the mip of the unwarped fundamental.
    // Read BEFORE any phase advances, so what the other oscillator sees one
    // sample later is this sample's value.
    static inline f32 oscTap(const f32* ph, const f32* tbl, const OscSel& s) {
        return spRead(tbl, s.f0, s.f1, s.ff, s.m0, s.mf, ph[0]);
    }

    // p^e for p in [0,1) — the Bend curves. exp2(e*log2(p)) with the same
    // fast log2 the mip selector trusts: deterministic, smooth to ~1e-5, and
    // an order of magnitude cheaper than powf in a 7-voice unison loop.
    static inline f32 spPow01(f32 p, f32 e) {
        if (p <= 0.f) return 0.f;
        return std::exp2(e * spLog2(p));
    }

    // The read-phase warp (contract, "Block: warp"): Bend+/Bend-/Mirror/
    // Quantize. Sync is in the caller (it also moves the mip); FM moves the
    // increment; RM scales the output.
    static inline f32 spWarpPhase(int mode, f32 a, f32 p) {
        switch (mode) {
            case 2:  return spPow01(p, 1.f / (1.f + 3.f * a));
            case 3:  return spPow01(p, 1.f + 3.f * a);
            case 4:  return (1.f - a) * p + a * (1.f - std::fabs(2.f * p - 1.f));
            case 5: {
                const f32 n = (f32)(2 + (int)(62.f * (1.f - a) + 0.5f));
                return (f32)(int)(p * n) / n;
            }
            default: return p;
        }
    }

    // One oscillator, v1 path: mip choice from the fan's HIGHEST increment,
    // frame pair from the position, then one linear read per unison voice.
    // Taken whenever the osc's warp is Off or at zero depth — the arithmetic
    // is v1's, bit for bit.
    inline void oscillator(f32* ph, const f32* tbl, const OscSel& s, f32 inc, int u,
                           const f32* rat, const f32* panL, const f32* panR,
                           f32 gain, f32& outL, f32& outR) {
        f32 l = 0.f, r = 0.f;
        for (int i = 0; i < u; ++i) {
            const f32 smp = spRead(tbl, s.f0, s.f1, s.ff, s.m0, s.mf, ph[i]);
            l += smp * panL[i];
            r += smp * panR[i];
            ph[i] += inc * rat[i];
            if (ph[i] >= 1.f) ph[i] -= 1.f;
            if (!(ph[i] >= 0.f && ph[i] < 1.f)) ph[i] = 0.f;
        }
        outL += l * gain;
        outR += r * gain;
    }

    // One oscillator, warp engaged (mode 1..7 at depth > 0). `m` is the other
    // osc's one-sample-delayed voice-0 tap.
    inline void oscWarp(f32* ph, const f32* tbl, const OscSel& sel, int mode, f32 a,
                        f32 inc, f32 m, int u, const f32* rat,
                        const f32* panL, const f32* panR,
                        f32 gain, f32 maxRat, f32& outL, f32& outR) {
        OscSel s = sel;
        f32 slave = 1.f;
        if (mode == 1) {
            // Sync: slave ratio r = 1 + 7a; the mip follows f*r (the one warp
            // whose brightness genuinely moves), the master wrap is not
            // BLEP'd — the contract accepts that, same policy as table 5.
            slave = 1.f + 7.f * a;
            const f32 mipf = spLog2(inc * maxRat * slave) + 12.f;
            int m0 = (int)mipf;
            f32 mf = mipf - (f32)m0;
            if (m0 < 0)             { m0 = 0; mf = 0.f; }
            if (m0 > kSpMips - 2)   { m0 = kSpMips - 2; mf = 1.f; }
            s.m0 = m0;
            s.mf = mf;
        }
        f32 incEff = inc;
        if (mode == 6) {
            // Through-zero linear FM: k = 2^(3a)-1, so a full-scale modulator
            // detunes by exactly 36a semitones; a negative (1 + k*m) runs the
            // wavetable backwards, which is the through-zero.
            const f32 k = std::exp2(3.f * a) - 1.f;
            incEff = inc * (1.f + k * m);
        }
        f32 l = 0.f, r = 0.f;
        for (int i = 0; i < u; ++i) {
            f32 lp;
            if (mode == 1)      lp = spFrac(ph[i] * slave);
            else                lp = spWarpPhase(mode, a, ph[i]);
            const f32 smp = spRead(tbl, s.f0, s.f1, s.ff, s.m0, s.mf, lp);
            l += smp * panL[i];
            r += smp * panR[i];
            ph[i] += incEff * rat[i];
            // frac in both directions: FM can step more than a cycle and can
            // step backwards. (int) truncation then a negative fix-up.
            ph[i] -= (f32)(int)ph[i];
            if (ph[i] < 0.f) ph[i] += 1.f;
            if (!(ph[i] >= 0.f && ph[i] < 1.f)) ph[i] = 0.f;
        }
        f32 g = gain;
        if (mode == 7) g *= (1.f - a) + a * m;    // dry-to-ring-mod crossfade
        outL += l * g;
        outR += r * g;
    }

    // The same phase bookkeeping with no read, for a silent oscillator.
    static inline void advance(f32* ph, f32 inc, int u, const f32* rat) {
        for (int i = 0; i < u; ++i) {
            ph[i] += inc * rat[i];
            if (ph[i] >= 1.f) ph[i] -= 1.f;
            if (!(ph[i] >= 0.f && ph[i] < 1.f)) ph[i] = 0.f;
        }
    }

    // Silent but FM'd: the phases must advance exactly as if audible, because
    // this osc's tap can still be the other one's modulator.
    static inline void advanceFm(f32* ph, f32 inc, f32 m, f32 a, int u, const f32* rat) {
        const f32 k = std::exp2(3.f * a) - 1.f;
        const f32 incEff = inc * (1.f + k * m);
        for (int i = 0; i < u; ++i) {
            ph[i] += incEff * rat[i];
            ph[i] -= (f32)(int)ph[i];
            if (ph[i] < 0.f) ph[i] += 1.f;
            if (!(ph[i] >= 0.f && ph[i] < 1.f)) ph[i] = 0.f;
        }
    }

    // The matrix's pitch destinations (7, 8) in semitones, ±24 st per
    // full-scale slot. v2 clamped the summed matrix pitch at ±48 st; v3 sums
    // the pitch wheel into that clamp, and since the wheel was already applied
    // to the voice's base pitch the residual is what this stage adds. At
    // bendSt == 0 the two arms are the same number and the first is v2's
    // expression, character for character.
    static inline f32 matrixPitch(f32 mdPitch, f32 bendSt) {
        const f32 m = mdPitch * 24.f;
        if (bendSt == 0.f) return clampv(m, -48.f, 48.f);
        return clampv(bendSt + m, -48.f, 48.f) - bendSt;
    }

    // polyBLEP corrective for the sub square's edges; t in [0,1), dt = inc.
    static inline f32 spBlep(f32 t, f32 dt) {
        if (dt <= 0.f) return 0.f;
        if (t < dt)       { const f32 x = t / dt;          return x + x - x * x - 1.f; }
        if (t > 1.f - dt) { const f32 x = (t - 1.f) / dt;  return x * x + x + x + 1.f; }
        return 0.f;
    }

    // --- the voice -----------------------------------------------------------

    inline void renderVoice(Voice& v, const Blk& b, const Glob& g, f32& accL, f32& accR) {
        // Envelopes, per sample. Cheap enough that a control rate would buy
        // nothing, and per-sample is inherently block-size invariant. ENV3
        // ticks whether or not a slot reads it: envelope state must be a
        // function of the notes, never of the routing.
        envTick(v.e1, b.a1, b.d1, b.s1, b.r1);
        envTick(v.e2, b.a2, b.d2, b.s2, b.r2);
        envTick(v.e3, b.a3, b.d3, b.s3, b.r3);
        if (v.e1.stage == kIdle) { v.active = false; return; }

        // Glide, then vibrato, then the pitch wheel, then the increment.
        if (v.glideLeft > 0) {
            v.pitch += v.glideStep;
            if (--v.glideLeft == 0) v.pitch = v.pitchTarget;
        }
        const f32 lfo1 = lfo1Of(v, b, g);
        // v3, id 99. The wheel is added to the POST-GLIDE pitch alongside
        // vibrato and therefore BEFORE per-oscillator Coarse/Fine — so the sub
        // (id 17) follows the wheel exactly as it follows vibrato. A range of 0
        // or a centred wheel is BRANCHED AROUND rather than added as a zero:
        // "mathematically equal" is not "bit-identical", and this is the one
        // new path that can change an existing set's render at all.
        const f32 bendSt = (b.bendRange > 0.f && g.bend != 0.f) ? g.bend * b.bendRange : 0.f;
        const f32 midi = bendSt != 0.f ? (v.pitch + b.lfoPitch * lfo1 * 0.01f + bendSt)
                                       : (v.pitch + b.lfoPitch * lfo1 * 0.01f);
        const f32 base = clampv(b.incScale * std::exp2((midi - 69.f) * (1.f / 12.f)),
                                0.f, 0.45f);

        // The matrix, per voice at audio rate. md[] is only READ under a
        // dstMask bit, and a bit is only set when a live slot targets that
        // destination — so a patch with an empty matrix runs the v1
        // expressions below untouched.
        f32 md[kDstCount] = {};
        for (int k = 0; k < b.mN; ++k)
            md[b.slot[k].dst] += slotMod(b.slot[k], &v, b, g);

        // Position: the parameter plus both fixed modulators plus the matrix,
        // clamped into the frame axis (inside oscSelect). ENV2 is per voice,
        // so this is too.
        const f32 mod = b.lfoPos * lfo1 + b.env2Pos * v.e2.v;
        f32 posA = b.posA + mod;
        f32 posB = b.posB + mod;
        if (b.dstMask & (1u << kDAPos)) posA += md[kDAPos];
        if (b.dstMask & (1u << kDBPos)) posB += md[kDBPos];

        f32 lvlA = b.lvlA, lvlB = b.lvlB;
        if (b.dstMask & (1u << kDALvl)) lvlA = clampv(lvlA + md[kDALvl], 0.f, 1.f);
        if (b.dstMask & (1u << kDBLvl)) lvlB = clampv(lvlB + md[kDBLvl], 0.f, 1.f);

        // Pitch: ±24 st per full-scale slot, added after coarse/fine/glide,
        // the summed matrix pitch clamped ±48 st (contract).
        //
        // v3: the wheel is already in `base`, and the contract sums it WITH the
        // matrix pitch and clamps the pair at ±48 st total. Clamping the sum
        // and subtracting the part already applied is the whole of that, and at
        // bendSt == 0 the expression is v2's, selected and not computed.
        f32 incA = base * b.ratioA;
        f32 incB = base * b.ratioB;
        if (b.dstMask & (1u << kDAPitch))
            incA *= std::exp2(matrixPitch(md[kDAPitch], bendSt) * (1.f / 12.f));
        if (b.dstMask & (1u << kDBPitch))
            incB *= std::exp2(matrixPitch(md[kDBPitch], bendSt) * (1.f / 12.f));

        // Detune: the fan's ratios rebuilt at audio rate; the pans hold (the
        // spread's geometry does not move, only its width in cents).
        const f32* ratA = b.uratA;
        const f32* ratB = b.uratB;
        f32 maxRatA = b.maxRatA, maxRatB = b.maxRatB;
        f32 dynRatA[kUni], dynRatB[kUni];
        if (b.dstMask & (1u << kDADet)) {
            const f32 det = clampv(b.detA + md[kDADet] * 100.f, 0.f, 100.f);
            maxRatA = 1.f;
            for (int i = 0; i < b.uniA; ++i) {
                dynRatA[i] = std::exp2(b.offA[i] * det * (0.5f / 1200.f));
                if (dynRatA[i] > maxRatA) maxRatA = dynRatA[i];
            }
            for (int i = b.uniA; i < kUni; ++i) dynRatA[i] = 1.f;
            ratA = dynRatA;
        }
        if (b.dstMask & (1u << kDBDet)) {
            const f32 det = clampv(b.detB + md[kDBDet] * 100.f, 0.f, 100.f);
            maxRatB = 1.f;
            for (int i = 0; i < b.uniB; ++i) {
                dynRatB[i] = std::exp2(b.offB[i] * det * (0.5f / 1200.f));
                if (dynRatB[i] > maxRatB) maxRatB = dynRatB[i];
            }
            for (int i = b.uniB; i < kUni; ++i) dynRatB[i] = 1.f;
            ratB = dynRatB;
        }

        // Warp depth, then the contract's id-49 gate: at zero depth every
        // mode IS Off — enforced by selection, so it is bit-identical rather
        // than approximately neutral.
        f32 wAmtA = b.warpAmtA, wAmtB = b.warpAmtB;
        if (b.dstMask & (1u << kDAWAmt)) wAmtA = clampv(wAmtA + md[kDAWAmt], 0.f, 1.f);
        if (b.dstMask & (1u << kDBWAmt)) wAmtB = clampv(wAmtB + md[kDBWAmt], 0.f, 1.f);
        const int modeA = wAmtA > 0.f ? b.warpA : 0;
        const int modeB = wAmtB > 0.f ? b.warpB : 0;

        f32 xl = 0.f, xr = 0.f;

        const bool useA = lvlA > 0.f;
        const bool useB = lvlB > 0.f;
        OscSel selA{}, selB{};
        if (useA || b.needTapA) selA = oscSelect(posA, incA, maxRatA);
        if (useB || b.needTapB) selB = oscSelect(posB, incB, maxRatB);

        // Fresh taps, read before any phase advances; last sample's values
        // (v.lastA/B) are what modulate THIS sample — the contract's
        // one-sample delay, which is what makes mutual FM/RM well-defined.
        f32 tapA = 0.f, tapB = 0.f;
        if (b.needTapA) tapA = oscTap(v.phA, b.tblA, selA);
        if (b.needTapB) tapB = oscTap(v.phB, b.tblB, selB);

        if (useA) {
            if (modeA == 0)
                oscillator(v.phA, b.tblA, selA, incA, b.uniA, ratA,
                           b.panLA, b.panRA, b.ugainA * lvlA, xl, xr);
            else
                oscWarp(v.phA, b.tblA, selA, modeA, wAmtA, incA, v.lastB, b.uniA,
                        ratA, b.panLA, b.panRA, b.ugainA * lvlA, maxRatA, xl, xr);
        } else if (modeA == 6) {
            advanceFm(v.phA, incA, v.lastB, wAmtA, b.uniA, ratA);
        } else {
            advance(v.phA, incA, b.uniA, ratA);
        }

        if (useB) {
            if (modeB == 0)
                oscillator(v.phB, b.tblB, selB, incB, b.uniB, ratB,
                           b.panLB, b.panRB, b.ugainB * lvlB, xl, xr);
            else
                oscWarp(v.phB, b.tblB, selB, modeB, wAmtB, incB, v.lastA, b.uniB,
                        ratB, b.panLB, b.panRB, b.ugainB * lvlB, maxRatB, xl, xr);
        } else if (modeB == 6) {
            advanceFm(v.phB, incB, v.lastA, wAmtB, b.uniB, ratB);
        } else {
            advance(v.phB, incB, b.uniB, ratB);
        }

        v.lastA = tapA;
        v.lastB = tapB;

        // Noise: always drawn, whatever the level, so the voice's random stream
        // is a function of time alone and not of a parameter.
        const f32 nl = 2.f * rnd(v.rng) - 1.f;
        const f32 nr = 2.f * rnd(v.rng) - 1.f;
        f32 lvlN = b.lvlN, lvlSub = b.lvlSub;
        if (b.dstMask & (1u << kDNoise)) lvlN = clampv(lvlN + md[kDNoise], 0.f, 1.f);
        if (b.dstMask & (1u << kDSub))   lvlSub = clampv(lvlSub + md[kDSub], 0.f, 1.f);
        if (b.nzBypass) {
            // Color at exactly 1.0: the v1 white path, bit for bit.
            xl += nl * lvlN;
            xr += nr * lvlN;
        } else {
            // One-pole 6 dB/oct lowpass on the white source; the coefficient
            // walks at the control tick (retarget), like the filter's.
            v.nzL += v.nzCoef * (nl - v.nzL);
            v.nzR += v.nzCoef * (nr - v.nzR);
            xl += v.nzL * lvlN;
            xr += v.nzR * lvlN;
        }

        // Sub: the voice's post-glide pitch shifted by Sub Oct (0.5f at the
        // default — v1's octave-below, bit for bit), so it follows the glide
        // for free. Square gets polyBLEP edges; the triangle's slope kinks
        // fall off at 1/h^2 an octave or two down and need none.
        const f32 subInc = base * b.subMul;
        if (lvlSub > 0.f) {
            f32 sv;
            if (b.subShape == 1) {
                sv = v.subPh < 0.5f ? (4.f * v.subPh - 1.f) : (3.f - 4.f * v.subPh);
            } else if (b.subShape == 2) {
                sv = (v.subPh < 0.5f ? 1.f : -1.f)
                   + spBlep(v.subPh, subInc)
                   - spBlep(spFrac(v.subPh + 0.5f), subInc);
            } else {
                sv = std::sin(dsp::kTwoPi * v.subPh);
            }
            const f32 s = sv * lvlSub;
            xl += s;
            xr += s;
        }
        v.subPh += subInc;
        if (v.subPh >= 1.f) v.subPh -= 1.f;

        if (b.dstMask & (1u << kDDrive)) {
            // Matrix on Drive: sum in dB, clamp to the knob's 0..24, and only
            // then the wire-vs-tanh branch — 0 dB stays a wire under
            // modulation too.
            const f32 db = clampv(b.driveDb + md[kDDrive] * 24.f, 0.f, 24.f);
            if (db > 0.f) {
                const f32 dg = dbToGain(db);
                const f32 dc = std::tanh(0.5f) / std::tanh(dg * 0.5f);
                xl = std::tanh(dg * xl) * dc;
                xr = std::tanh(dg * xr) * dc;
            }
        } else if (b.drive) {
            xl = std::tanh(b.driveG * xl) * b.driveC;
            xr = std::tanh(b.driveG * xr) * b.driveC;
        }

        // Filter. Coefficients are snapped on a voice's very first sample (one
        // tan per note-on) and walked between control ticks after that.
        if (v.fSnap) {
            const bool mc = (b.dstMask & (1u << kDCut)) != 0;
            const bool mr = (b.dstMask & (1u << kDRes)) != 0;
            f32 mdC = 0.f, mdR = 0.f;
            if (mc || mr) cutResMods(b, v, g, mdC, mdR);
            v.fc   = dsp::svfCoeffs(sr_, voiceCutoff(v, b, lfo1, mc, mdC), voiceQ(b, mr, mdR));
            v.fInc = dsp::SvfCoeffs{ 0.f, 0.f, 0.f, 0.f };
            v.fSnap = false;
            if (!b.nzBypass) v.nzCoef = noiseCoef(v, b);
        }
        const dsp::SvfOut ol  = dsp::svfTick(v.fc, v.fs[0], xl);
        const dsp::SvfOut orr = dsp::svfTick(v.fc, v.fs[1], xr);

        // The 24 dB modes tick their second stage on the same (pre-step)
        // coefficients as the first — two identical stages in series sharing
        // cutoff and resonance, as the widened contract row says.
        f32 yl = 0.f, yr = 0.f;
        if (b.ftype == 3) {
            yl = dsp::svfTick(v.fc, v.fs2[0], ol.lp).lp;
            yr = dsp::svfTick(v.fc, v.fs2[1], orr.lp).lp;
        } else if (b.ftype == 4) {
            yl = dsp::svfTick(v.fc, v.fs2[0], ol.hp).hp;
            yr = dsp::svfTick(v.fc, v.fs2[1], orr.hp).hp;
        }
        dsp::svfStep(v.fc, v.fInc);

        // The bandpass is normalised by k = 1/Q so its peak stays at unity, for
        // the reason the Auto Filter gives: an SVF's raw bandpass tap has a peak
        // gain of Q, and a band filter that gets 26 dB louder as the resonance
        // knob turns is a hazard rather than a feature. (The k read here is the
        // post-step one, exactly as v1 read it.)
        if (b.ftype == 0)      { yl = ol.lp; yr = orr.lp; }
        else if (b.ftype == 1) { yl = ol.bp * v.fc.k; yr = orr.bp * v.fc.k; }
        else if (b.ftype == 2) { yl = ol.hp; yr = orr.hp; }
        else if (b.ftype == 5) { yl = ol.lp + ol.hp; yr = orr.lp + orr.hp; }

        const f32 amp = v.e1.v * v.velAmp;
        if (b.dstMask & (1u << kDPan)) {
            // Equal power, base centre, on the voice's summed output before
            // Master — the unison fan's own pan law, one stage later.
            const f32 pan = clampv(md[kDPan], -1.f, 1.f);
            const f32 th  = (pan + 1.f) * 0.7853981f;
            accL += yl * amp * (1.4142136f * std::cos(th));
            accR += yr * amp * (1.4142136f * std::sin(th));
        } else {
            accL += yl * amp;
            accR += yr * amp;
        }
    }

    // Exponential ADSR. The attack aims past 1 so its curve bends the way an
    // analogue one does; decay and release are one-poles that reach a thousandth
    // of their span in the time asked for. The release stops at -100 dB and not
    // at some audible floor, so a note-off FADES rather than stepping.
    static inline void envTick(Env& e, f32 a, f32 d, f32 s, f32 r) {
        switch (e.stage) {
            case kAtk:
                e.v += (kAtkAim - e.v) * a;
                if (e.v >= 1.f) { e.v = 1.f; e.stage = kDec; }
                break;
            case kDec:
                e.v += (s - e.v) * d;
                if (e.v - s < 1e-6f) { e.v = s; e.stage = kSus; }
                break;
            case kSus:
                e.v = s;
                break;
            case kRel:
                e.v -= e.v * r;
                if (e.v < kEnvOff) { e.v = 0.f; e.stage = kIdle; }
                break;
            default:
                e.v = 0.f;
                break;
        }
    }

    // --- v3 state parsing ---------------------------------------------------

    // One record. Returns false ONLY for a key this build knows carrying a
    // value it could not have written; an unrecognised key returns true and is
    // skipped, which is the forward compatibility the format is for.
    static bool readRecord(SpState& st, const std::string& key, const std::string& val) {
        if (key.size() == 4 && key.compare(0, 3, "lfo") == 0 &&
            key[3] >= '1' && key[3] <= '3') {
            if ((int)val.size() != kSpSteps) return false;
            const int n = key[3] - '1';
            for (int i = 0; i < kSpSteps; ++i) {
                const int d = spHexLo(val[(size_t)i]);
                if (d < 0) return false;
                st.grid[n][i] = (u8)d;
            }
            return true;
        }
        if (key.size() == 7 && key.compare(0, 6, "smooth") == 0 &&
            key[6] >= '1' && key[6] <= '3') {
            int v = 0;
            if (!spParseUInt(val, 4, 1000, v)) return false;
            st.smooth[key[6] - '1'] = (i16)v;
            return true;
        }
        if (key == "cc") {
            int v = 0;
            if (!spParseUInt(val, 3, 127, v)) return false;
            st.cc = (i16)v;
            return true;
        }
        if (key == "wtA" || key == "wtB") {
            u64 h = 0;
            if (!spParseHex64(val, h)) return false;
            st.wt[key[2] == 'B' ? 1 : 0] = h;
            return true;
        }
        // --- v4. The two arp rows sit on both sides of the refusal /
        // degradation line in one place each, and the contract draws it:
        //
        //   REFUSED — a wrong length, a character outside [0-9a-f], an
        //   uppercase character. These are strings this writer could not have
        //   produced, so the whole state refuses and the device is untouched.
        //   (A duplicate key and a record with no `=` are refused by the
        //   caller, for every key.)
        //
        //   DEGRADED — an octave code of 5, 6 or 7 clamps to 4, and bits 5..7
        //   are masked off. These are values a LATER, WIDER build could
        //   legitimately write, and the versioning rule's job is to let a newer
        //   state land on an older build rather than break it — exactly the
        //   argument v3 made for a Table value of 9 arriving as a clamped 8.
        //   spArpPack() is where both happen.
        if (key == "arpl") {
            if ((int)val.size() != kSpSteps) return false;
            u8 tmp[kSpSteps];
            for (int i = 0; i < kSpSteps; ++i) {
                const int d = spHexLo(val[(size_t)i]);
                if (d < 0) return false;
                tmp[i] = (u8)d;
            }
            for (int i = 0; i < kSpSteps; ++i) st.arpLv[i] = tmp[i];
            return true;
        }
        if (key == "arps") {
            if ((int)val.size() != 2 * kSpSteps) return false;
            u8 tmp[kSpSteps];
            for (int i = 0; i < kSpSteps; ++i) {
                const int hi = spHexLo(val[(size_t)(2 * i)]);
                const int lo = spHexLo(val[(size_t)(2 * i + 1)]);
                if (hi < 0 || lo < 0) return false;
                tmp[i] = spArpPack((u8)(16 * hi + lo));
            }
            for (int i = 0; i < kSpSteps; ++i) st.arpSt[i] = tmp[i];
            return true;
        }
        if (key == "wtpathA" || key == "wtpathB") {
            std::string pth;
            if (!smUnesc(val, pth) || pth.empty()) return false;
            st.wtPath[key[6] == 'B' ? 1 : 0] = pth;
            return true;
        }
        // v5. THE ESCAPING IS wtpath'S, VERBATIM -- the same smUnesc, not a
        // third escaper -- so a name is UTF-8 and survives because every byte
        // >= 0x7F is escaped. STRICT, and it refuses in the order the contract
        // lists: a `%` not followed by two hex digits, an escape decoding to
        // NUL, and a raw byte the writer would have escaped are all smUnesc's;
        // over 64 DECODED bytes is this line's. An empty value is refused
        // because it is not a record this writer could produce.
        if (key == "wtnameA" || key == "wtnameB") {
            std::string nm;
            if (!smUnesc(val, nm) || nm.empty() || nm.size() > 64) return false;
            st.wtName[key[6] == 'B' ? 1 : 0] = nm;
            return true;
        }
        return true;                    // unknown key: forward compatibility
    }

    // GUI thread. Copies the two fields the audio thread reads out of st_ into
    // the atomics it actually reads, so st_ itself goes back to being what its
    // own comment claims: GUI-thread-owned. Called from every path that writes
    // st_'s grids or smooths.
    void publishStateToAudio() {
        for (int j = 0; j < 3; ++j) {
            u64 bits = 0;
            for (int i = 0; i < kSpSteps; ++i)
                bits |= (u64)(st_.grid[j][i] & 15u) << (4 * i);
            gridBits_[j].store(bits, std::memory_order_relaxed);
            smoothQ_[j].store(st_.smooth[j], std::memory_order_relaxed);
        }
        // v4's two rows, on gridBits_'s terms exactly: sixteen nibbles in one
        // u64 each, so a row is taken in a single load and can never be
        // observed half-updated. The step row's eleven reachable states fit a
        // nibble — see spArpPack.
        u64 lb = 0, sb = 0;
        for (int i = 0; i < kSpSteps; ++i) {
            lb |= (u64)(st_.arpLv[i] & 15u) << (4 * i);
            sb |= (u64)(st_.arpSt[i] & 15u) << (4 * i);
        }
        arpLvBits_.store(lb, std::memory_order_relaxed);
        arpStBits_.store(sb, std::memory_order_relaxed);
    }

    // GUI thread. The one writer of st_ outside adoptCustom(), and therefore
    // the one place the derived state — the audio-readable CC number, the
    // drawn grid's lag origin, and the custom-table resolution — is refreshed.
    void adoptState(const SpState& st) {
        const u64 was[2] = { st_.wt[0], st_.wt[1] };
        st_ = st;
        ccNum_.store((int)st_.cc, std::memory_order_relaxed);
        publishStateToAudio();
        for (int j = 0; j < 3; ++j) {
            smY_[j] = gridAt(j, 0.f);
            smA_[j] = 1.f;
        }
        for (int o = 0; o < 2; ++o) if (st_.wt[o] != was[o]) warnedTable_[o] = false;
        resolveTables();
    }

    // GUI thread. Asks the table wave's seam to resolve each custom slot this
    // state names, and records the answer. The audio thread never asks: it
    // takes whatever pointer spTableBase() hands it and renders factory table 0
    // on a null, so a failure here costs one warning and no branch in the voice.
    void resolveTables() {
        for (int o = 0; o < 2; ++o) {
            if (oscH_[o] < 0) { wtOk_[o] = false; continue; }
            // Called with hash 0 as well, and deliberately: that is how "this
            // preset names no table" reaches the seam and clears the record.
            wtOk_[o] = spResolveCustom(oscH_[o], st_.wt[o],
                                       st_.wtPath[o].empty() ? nullptr
                                                             : st_.wtPath[o].c_str());
            if (st_.wt[o] == 0 || wtOk_[o] || warnedTable_[o]) continue;
            warnedTable_[o] = true;
            const std::string what = st_.wtPath[o].empty() ? spFmtHex64(st_.wt[o])
                                                           : st_.wtPath[o];
            LOGW("spectra: oscillator %c's custom wavetable (%s) could not be resolved; "
                 "that oscillator renders factory table 0 and the state keeps the record, "
                 "so the set still names the file", o ? 'B' : 'A', what.c_str());
        }
    }

    // One log line per instance, whatever a set throws at it — the sampler's
    // rule, and this device can appear thirty times in one project.
    bool badState(const std::string& s) {
        if (!warnedBadState_) {
            warnedBadState_ = true;
            LOGW("spectra: device state did not parse (%zu bytes); the instrument is "
                 "unchanged", s.size());
        }
        return false;
    }

    const SpectraTables* tbl_ = nullptr;

    // 128 slots. A note-on may take at most kOnCap of them, leaving 32 that
    // only a note-off or a panic can reach; past even that, the overflow set
    // below catches them. See queue().
    static constexpr int kPend  = 128;
    static constexpr int kOnCap = 96;
    PendEv pend_[kPend]{};
    int    nPend_ = 0;

    // 128 bits of note-off plus a two-state panic flag: O(1), so neither can be
    // lost however long the flood is.
    u32  ovfOff_[4] = {};
    u8   ovfPanic_  = 0;
    bool haveOvf_   = false;

    Voice voices_[kSpVoices];
    u32   age_ = 0;
    int   ctrl_ = 0;
    dsp::Lfo lfo_;
    f32   shVal_ = 0.f;
    // SEPARATE counters, deliberately: see the file header. The note stream
    // and the three sample-and-hold streams must not be able to interleave
    // into each other.
    u32   noteRng_ = 0x9E3779B9u;
    u32   lfoRng_  = 0x2545F491u;
    f32   lastPitch_ = 60.f;
    bool  havePitch_ = false;

    // --- v2 instance state ---
    dsp::Lfo lfo2_, lfo3_;
    f32   shVal2_ = 0.f, shVal3_ = 0.f;
    u32   lfo2Rng_ = 0x6C078965u;
    u32   lfo3Rng_ = 0xB5297A4Du;
    f32   after_ = 0.f;                 // channel pressure, instance-wide
    u64   absPos_ = 0;                  // absolute sample position of the
                                        // NEXT block's first frame
    static constexpr int kHeld = 64;    // mono/legato held-note stack
    u8    held_[kHeld] = {};
    int   nHeld_ = 0;
    // v4's one addition to the stack, and it is DATA, not semantics: Vel Mode 0
    // (As Played) needs the velocity and the per-note identity hash needs the
    // channel. Written by heldPush and compacted by heldRemove alongside the
    // note numbers; nothing about the stack's ordering, depth, drop-oldest rule
    // or maintained-in-every-mode rule changes.
    u8    heldVel_[kHeld] = {};
    u8    heldCh_[kHeld]  = {};

    // --- v3 instance state ---
    //
    // st_ is GUI-thread-owned: it is written by setStateString(), loadPreset()
    // and the import path, all of which the contract puts on that thread, and
    // read by the audio thread through the grids and the smooths. Those reads
    // are of bytes that only change when a patch changes, which is the same
    // class of access every parameter already has.
    //
    // ccNum_ WAS the one exception and is atomic, because midi() reads it on the
    // audio thread to decide whether a controller is worth queueing at all.
    //
    // gridBits_ and smoothQ_ are two more of the same, and they are not
    // optional. The paragraph above used to argue that the grids and the
    // smooths "only change when a patch changes, which is the same class of
    // access every parameter already has" — and that was true right up until
    // this device grew a state string. A PARAMETER is written through setParam
    // on one thread; STATE arrives through setStateString(), which the daemon
    // calls on its pump thread while the audio thread is inside process().
    // TSan caught the write of st_ racing the read of st_.smooth in exactly
    // that window (found by the wavetable wave, whose tests were the first to
    // hand a SOUNDING Spectra a state).
    //
    // The consequence was not cosmetic: a torn smooth is one control tick of a
    // wrong coefficient, but a torn grid nibble is an LFO step at a level
    // nobody drew, and it is nondeterministic — which would end "the same
    // input renders the same audio" for every project load under the default
    // engine, this tree's spine.
    //
    // Sixteen levels pack into ONE u64, so the audio thread takes a grid in a
    // single load and can never see it half-old. Relaxed is enough: these carry
    // no other memory with them, exactly as ccNum_ does not.
    SpState st_;
    std::atomic<int> ccNum_{-1};
    std::atomic<u64> gridBits_[3] = {};    // 16 nibbles, step i at bits 4i
    std::atomic<i16> smoothQ_[3]  = {};    // thousandths, mirrors st_.smooth

    f32   wheel_ = 0.f;                 // CC 1, matrix source 14
    f32   bend_  = 0.f;                 // 0xE0, source 15; 0 is centre
    f32   ccVal_ = 0.f;                 // the learned controller, source 16
    i16   ovfCCNum_ = -1, ovfCCVal_ = 0;
    i16   ovfBend_  = -1;

    f32   smY_[3] = {};                 // Loop-mode drawn-grid lag state
    f32   smA_[3] = { 1.f, 1.f, 1.f };  // ...and its control-tick coefficient

    // The beat counter the drawn grid's step index locks to when synced. f64
    // because it is a position and not an increment: an f32 beat would lose a
    // step boundary in a long set. See the file header for the anchoring rule.
    f64   beatAcc_ = 0.0, beatInc_ = 0.0, trBeatWas_ = 0.0;
    bool  trBeatSeen_ = false, trPlayWas_ = false;

    // --- v4 instance state ---
    //
    // The two rows follow gridBits_ exactly: sixteen nibbles in one u64 each,
    // taken in a single atomic load, for the reason gridBits_ exists — state
    // arrives through setStateString(), which the daemon calls on its pump
    // thread while the audio thread is inside process(), and a torn step nibble
    // would be a step nobody drew and would be NONDETERMINISTIC.
    std::atomic<u64> arpLvBits_{0xffffffffffffffffull};   // default: level 1.0
    std::atomic<u64> arpStBits_{0x7777777777777777ull};   // default: on, +0, no tie

    ArpCfg arp_;
    // The free-running clock, in steps, advanced only while the arp is on —
    // id 109's own words are that with Arp On 0 "the arp does not exist" and
    // "every id below is read by nothing", and Arp Rate is one of those ids.
    // The SYNCED clock needs no member: it is beatAcc_ / beatsPerStep.
    f64   arpFree_      = 0.0;
    f64   arpOrigin_    = 0.0;   // the retrigger origin, in the A domain
    f64   arpNextOnset_ = 0.0;   // onset(arpFiredK_ + 1); one compare a sample
    i64   arpFiredK_    = -1;    // a CURSOR, not an index: every index the arp
                                 // uses is a modulus of k, and this is only
                                 // "which onsets have already been served"
    bool  arpReanchor_   = false;
    bool  arpFresh_      = true;   // no readArpParams() since prepare()
    f32   arpStepLvl_    = 0.f;  // matrix source 17
    ArpSound arpSnd_[kArpSounds] = {};
    int   nArpSnd_ = 0;
    u8    latch_[kHeld] = {}, latchVel_[kHeld] = {}, latchCh_[kHeld] = {};
    int   nLatch_ = 0;

    // Did slot 8 resolve for this oscillator? Decided on the GUI thread by
    // resolveTables(); the audio thread never asks, it just takes the pointer
    // spTableBase() hands it and falls back to factory 0 on a null.
    // The two oscillator handles this instance owns, from spAcquireOsc(); -1
    // means the process ran out and this Spectra plays factory tables only.
    int   oscH_[2] = { -1, -1 };
    bool  wtOk_[2] = { false, false };
    // The sampler's warnedMissing_ discipline: one device pointing at one file
    // it cannot open is one line of information; a set with thirty Spectras in
    // it must not turn that into a screen.
    bool  warnedTable_[2] = { false, false };
    bool  warnedBadState_ = false;
};

constexpr const char* kSpectraUri = "nxtakt:spectra";

PluginDesc spectraDesc() {
    PluginDesc d;
    d.format     = PluginFormat::Internal;
    d.uri        = kSpectraUri;
    d.name       = "Spectra";
    d.vendor     = "NxTakt";
    d.category   = "Instrument";
    d.kind       = PluginKind::Instrument;
    d.audioIn    = 0;
    d.audioOut   = 2;
    d.hasMidiIn  = true;
    d.paramCount = kSpParamCount;
    return d;
}

} // namespace

// The one symbol this file exports, and the only thing outside src/plugin that
// knows Spectra's tables exist. Declared in internal_base.h; everything it
// returns is const and shared, so there is nothing here to synchronise beyond
// the acquire that pairs with spPublish()'s release.
//
// Null before the first prepare() in the process, and non-null forever after.
const SpectraTableSet* spectraTables() {
    return gSpPublished.load(std::memory_order_acquire);
}

} // namespace detail
} // namespace lat

#endif // LAT_SPECTRA_IN_INTERNAL_DEVICES
