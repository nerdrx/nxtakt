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
    kSpParamCount = 100
};

// The matrix enums, verbatim from the contract's source and destination lists.
enum : int {
    kSOff = 0, kSLfo1, kSLfo2, kSLfo3, kSEnv2, kSEnv3, kSVel, kSKey, kSAft,
    kSMac1, kSMac2, kSMac3, kSMac4, kSRandom
};
enum : int {
    kDOff = 0, kDAPos, kDBPos, kDAWAmt, kDBWAmt, kDALvl, kDBLvl, kDAPitch,
    kDBPitch, kDSub, kDNoise, kDCut, kDRes, kDDrive, kDADet, kDBDet, kDPan,
    kDL1Rate, kDL2Rate, kDL3Rate, kDstCount
};

// LFO Sync, in beats per cycle. Index 0 is "free" and is never read from here.
// 4/4 is assumed for the bar values, exactly as the Delay's table assumes it
// and for the same reason: the time signature is not on the plugin contract.
// APPEND-ONLY: the index is what a project file stores.
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
// nothing but SP_PRESET/SP/SP_END rows, so its author needs the contract
// document and no C++. The macros exist only across the include.
#define SP_PRESET(nm) { nm, {
#define SP(id, v)     { (id), (f32)(v) },
#define SP_END()      { -1, 0.f } } },
const SpPreset kSpPresets[] = {
#include "spectra_presets.inc"
};
#undef SP_PRESET
#undef SP
#undef SP_END

constexpr int kSpPresetCount = (int)(sizeof kSpPresets / sizeof kSpPresets[0]);

// ---------------------------------------------------------------------------
// The device
// ---------------------------------------------------------------------------

class Spectra final : public InternalInstance {
public:
    explicit Spectra(const PluginDesc& d) : InternalInstance(d) {
        // ORDER IS THE CONTRACT. Every addParam() below is an id, the ids are
        // indices, and a saved set stores them. docs/SPECTRA-PARAMS.md is the
        // frozen list; entries may be appended and never moved.
        addIntParam("A Table",  "",   0, 7, 0);
        addParam   ("A Position", "", 0.f, 1.f, 0.f);
        addIntParam("A Coarse", "st", -24, 24, 0);
        addParam   ("A Fine",   "ct", -100.f, 100.f, 0.f);
        addParam   ("A Level",  "",   0.f, 1.f, 0.8f);
        addIntParam("A Unison", "",   1, 7, 1);
        addParam   ("A Detune", "ct", 0.f, 100.f, 15.f);
        addParam   ("A Spread", "",   0.f, 1.f, 0.5f);

        addIntParam("B Table",  "",   0, 7, 1);
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
        addIntParam("LFO Shape",    "",   0, 4, 0);

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
        addIntParam("L2 Shape",    "",    0, 4, 0);                   // 56
        addParam   ("L3 Rate",     "Hz",  0.01f, 40.f, 2.f, true);    // 57
        addIntParam("L3 Sync",     "",    0, kSpSyncCount - 1, 0);    // 58
        addIntParam("L3 Shape",    "",    0, 4, 0);                   // 59
        addReserved();                                                // 60
        addReserved();                                                // 61
        addParam("E3 Attack",  "ms", 0.1f, 5000.f, 2.f,   true);      // 62
        addParam("E3 Decay",   "ms", 1.f,  5000.f, 300.f, true);      // 63
        addParam("E3 Sustain", "",   0.f,  1.f,    0.f);              // 64
        addParam("E3 Release", "ms", 1.f,  8000.f, 150.f, true);      // 65
        addReserved();                                                // 66
        addReserved();                                                // 67
        for (int k = 0; k < 8; ++k) {                                 // 68..91
            char nm[8];
            std::snprintf(nm, sizeof nm, "M%d Src", k + 1);
            addIntParam(nm, "", 0, 13, 0);
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
        addReserved();                                                // 99
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
        return true;
    }

    // --- presets (host.h) --------------------------------------------------
    int presetCount() const override { return kSpPresetCount; }

    const char* presetName(int i) const override {
        return (i >= 0 && i < kSpPresetCount) ? kSpPresets[i].name : nullptr;
    }

    // GUI thread. Writes through setParam and does nothing else, so the whole
    // program sees a preset as a handful of ordinary knob moves.
    void loadPreset(int i) override {
        if (i < 0 || i >= kSpPresetCount) return;
        for (int k = 0; k < n_; ++k) setParam(k, info_[(size_t)k].def);
        const SpPreset& p = kSpPresets[i];
        for (int k = 0; k < 64 && p.set[k].id >= 0; ++k)
            setParam(p.set[k].id, p.set[k].v);
    }

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
                // 120 = all sound off, 123 = all notes off. Anything else is a
                // controller we do not map; ignoring it is the honest answer.
                if (len >= 2 && data[1] == 120) queue(off, kEvSoundOff, 0, 0, chan);
                else if (len >= 2 && data[1] == 123) queue(off, kEvNotesOff, 0, 0, chan);
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

            // The LFO values for THIS sample, read before they are advanced,
            // so the control tick and the audio path see the same numbers.
            const Glob g = { lfoValue(lfo_,  b.lfoShape, shVal_),
                             lfoValue(lfo2_, b.l2Shape,  shVal2_),
                             lfoValue(lfo3_, b.l3Shape,  shVal3_),
                             after_ };

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
    };

    // The per-sample globals every voice reads: the three LFO values and the
    // channel pressure, gathered once so retarget() and renderVoice() see the
    // same numbers.
    struct Glob { f32 l1, l2, l3, after; };

    // A queued note event. Five bytes of payload and a frame stamp; nothing in
    // here allocates and the queue is a fixed array, so midi() stays realtime.
    // The channel rides along because it is part of the note's stable identity
    // (the matrix's Random source hashes it).
    enum : u8 { kEvOn = 0, kEvOff, kEvNotesOff, kEvSoundOff, kEvPressure };
    struct PendEv { int frame; u8 type, a, b, ch; };

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
        int lfoShape;
        f32 master;
        f32 incScale;                        // 440/sr

        // --- v2 ---
        int  subShape; f32 subMul;           // 2^SubOct; 0.5f at the default
        bool nzBypass, nzTrack; f32 nzFc;
        int  warpA, warpB; f32 warpAmtA, warpAmtB;
        bool needTapA, needTapB;             // "compute osc X's voice-0 tap"
        int  l2Shape, l3Shape;
        bool lfree[3];                       // Sync == 0 per LFO
        f32  lRateNorm[3];                   // rate knob on its log scale, 0..1
        f32  a3, d3, s3, r3;
        struct Slot { u8 src, dst; f32 amt; };
        Slot slot[kMods]; int mN; u32 dstMask;
        f32  macro[4];
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
            const int src = (int)clampv(p(kPM1Src + 3 * k) + 0.5f, 0.f, 13.f);
            const int dst = (int)clampv(p(kPM1Src + 3 * k + 1) + 0.5f, 0.f, 19.f);
            const f32 amt = clampv(p(kPM1Src + 3 * k + 2), -1.f, 1.f);
            if (src == kSOff || dst == kDOff || amt == 0.f) continue;
            b.slot[b.mN].src = (u8)src;
            b.slot[b.mN].dst = (u8)dst;
            b.slot[b.mN].amt = amt;
            ++b.mN;
            b.dstMask |= 1u << dst;
        }
        for (int j = 0; j < 4; ++j)
            b.macro[j] = clampv(p(kPMacro1 + j), 0.f, 1.f);

        const int ta = (int)clampv(p(kPATable) + 0.5f, 0.f, 7.f);
        const int tb = (int)clampv(p(kPBTable) + 0.5f, 0.f, 7.f);
        b.tblA = tbl_->frame(ta, 0);
        b.tblB = tbl_->frame(tb, 0);
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

        b.lfoShape = (int)clampv(p(kPLfoShape) + 0.5f, 0.f, 4.f);
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
        b.l2Shape = (int)clampv(p(kPL2Shape) + 0.5f, 0.f, 4.f);
        b.l3Shape = (int)clampv(p(kPL3Shape) + 0.5f, 0.f, 4.f);
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
    // contract's id 37 list, verbatim for id 56 and 59 too.
    static f32 lfoValue(const dsp::Lfo& l, int shape, f32 sh) {
        const f32 ph = l.phase;
        switch (shape) {
            case 1:  return ph < 0.5f ? (4.f * ph - 1.f) : (3.f - 4.f * ph);   // triangle
            case 2:  return 2.f * ph - 1.f;                                    // saw up
            case 3:  return ph < 0.5f ? 1.f : -1.f;                            // square
            case 4:  return sh;                                                // sample & hold
            default: return std::sin(dsp::kTwoPi * ph);
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
            default:          allSoundOff(); break;
        }
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

    // The mono/legato held-note stack, newest last. Maintained in EVERY mode
    // (cheap, silent in Poly) so switching into Mono/Legato mid-phrase starts
    // from the truth rather than from an empty memory.
    void heldPush(u8 n) {
        if (nHeld_ >= kHeld) {          // drop the oldest: fallback wants newest
            for (int i = 1; i < kHeld; ++i) held_[i - 1] = held_[i];
            --nHeld_;
        }
        held_[nHeld_++] = n;
    }
    void heldRemove(u8 n) {
        int w = 0;
        for (int i = 0; i < nHeld_; ++i)
            if (held_[i] != n) held_[w++] = held_[i];
        nHeld_ = w;
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

    void noteOn(u8 note, u8 vel, u8 ch, u64 absSample) {
        const int vm = (int)clampv(p(kPVoiceMode) + 0.5f, 0.f, 2.f);
        const bool otherHeld = nHeld_ > 0;   // before this note joins
        heldRemove(note);
        heldPush(note);

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
        v.fs[0].reset();
        v.fs[1].reset();
        v.fSnap = true;
        v.age = ++age_;
    }

    // Newest matching voice first: a repeated note that stole its own older
    // voice should release the one actually sounding.
    void noteOff(u8 note) {
        const int vm = (int)clampv(p(kPVoiceMode) + 0.5f, 0.f, 2.f);
        heldRemove(note);

        if (vm != 0) {
            Voice& v = voices_[0];
            // Voices left over from a Poly phrase (the mode switched while a
            // chord rang) still honour their note-offs — nothing may strand.
            for (size_t i = 1; i < (size_t)kSpVoices; ++i) {
                Voice& o = voices_[i];
                if (o.active && o.note == note && o.e1.stage != kRel) release(o);
            }
            if (!v.active || v.note != note || v.e1.stage == kRel) return;
            if (nHeld_ > 0) {
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

    void allNotesOff() {
        for (Voice& v : voices_) if (v.active && v.e1.stage != kRel) release(v);
        nHeld_ = 0;
    }
    void allSoundOff() {
        for (Voice& v : voices_) v = Voice{};
        nHeld_ = 0;
    }

    void clearSchedule() {
        nPend_ = 0;
        ovfOff_[0] = ovfOff_[1] = ovfOff_[2] = ovfOff_[3] = 0u;
        ovfPanic_  = 0;
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
            case kSLfo1:   return g.l1;
            case kSLfo2:   return g.l2;
            case kSLfo3:   return g.l3;
            case kSEnv2:   return v ? v->e2.v : 0.f;
            case kSEnv3:   return v ? v->e3.v : 0.f;
            case kSVel:    return v ? v->vel01 : 0.f;
            case kSKey:    return v ? clampv(((f32)v->note - 60.f) * (1.f / 60.f), -1.f, 1.f) : 0.f;
            case kSAft:    return g.after;
            case kSMac1: case kSMac2: case kSMac3: case kSMac4:
                           return b.macro[src - kSMac1];
            case kSRandom: return v ? v->rnote : 0.f;
            default:       return 0.f;
        }
    }

    // The slot sums for the two control-tick destinations (Cutoff, Resonance).
    inline void cutResMods(const Blk& b, const Voice& v, const Glob& g,
                           f32& mdCut, f32& mdRes) const {
        mdCut = 0.f;
        mdRes = 0.f;
        for (int k = 0; k < b.mN; ++k) {
            if (b.slot[k].dst == kDCut)
                mdCut += b.slot[k].amt * srcValue(b.slot[k].src, &v, b, g);
            else if (b.slot[k].dst == kDRes)
                mdRes += b.slot[k].amt * srcValue(b.slot[k].src, &v, b, g);
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
        if (b.dstMask & ((1u << kDL1Rate) | (1u << kDL2Rate) | (1u << kDL3Rate))) {
            // An LFO has one phase for all voices, so a per-voice source
            // driving its rate needs one voice picked: the NEWEST active one
            // (the musically obvious choice for the mono patches this is
            // for); none active reads the voice-bound sources as 0.
            const Voice* nv = nullptr;
            for (const Voice& v : voices_)
                if (v.active && (!nv || v.age > nv->age)) nv = &v;
            dsp::Lfo* ls[3] = { &lfo_, &lfo2_, &lfo3_ };
            const int dstOf[3] = { kDL1Rate, kDL2Rate, kDL3Rate };
            for (int j = 0; j < 3; ++j) {
                if (!(b.dstMask & (1u << dstOf[j])) || !b.lfree[j]) continue;
                f32 s = 0.f;
                for (int k = 0; k < b.mN; ++k)
                    if (b.slot[k].dst == dstOf[j])
                        s += b.slot[k].amt * srcValue(b.slot[k].src, nv, b, g);
                const f32 norm = clampv(b.lRateNorm[j] + s, 0.f, 1.f);
                ls[j]->setRate(sr_, 0.01f * std::exp2(norm * kRateOct));
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
                dsp::svfCoeffs(sr_, voiceCutoff(v, b, g.l1, mc, mdC), voiceQ(b, mr, mdR));
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

        // Glide, then vibrato, then the increment.
        if (v.glideLeft > 0) {
            v.pitch += v.glideStep;
            if (--v.glideLeft == 0) v.pitch = v.pitchTarget;
        }
        const f32 midi = v.pitch + b.lfoPitch * g.l1 * 0.01f;
        const f32 base = clampv(b.incScale * std::exp2((midi - 69.f) * (1.f / 12.f)),
                                0.f, 0.45f);

        // The matrix, per voice at audio rate. md[] is only READ under a
        // dstMask bit, and a bit is only set when a live slot targets that
        // destination — so a patch with an empty matrix runs the v1
        // expressions below untouched.
        f32 md[kDstCount] = {};
        for (int k = 0; k < b.mN; ++k)
            md[b.slot[k].dst] += b.slot[k].amt * srcValue(b.slot[k].src, &v, b, g);

        // Position: the parameter plus both fixed modulators plus the matrix,
        // clamped into the frame axis (inside oscSelect). ENV2 is per voice,
        // so this is too.
        const f32 mod = b.lfoPos * g.l1 + b.env2Pos * v.e2.v;
        f32 posA = b.posA + mod;
        f32 posB = b.posB + mod;
        if (b.dstMask & (1u << kDAPos)) posA += md[kDAPos];
        if (b.dstMask & (1u << kDBPos)) posB += md[kDBPos];

        f32 lvlA = b.lvlA, lvlB = b.lvlB;
        if (b.dstMask & (1u << kDALvl)) lvlA = clampv(lvlA + md[kDALvl], 0.f, 1.f);
        if (b.dstMask & (1u << kDBLvl)) lvlB = clampv(lvlB + md[kDBLvl], 0.f, 1.f);

        // Pitch: ±24 st per full-scale slot, added after coarse/fine/glide,
        // the summed matrix pitch clamped ±48 st (contract).
        f32 incA = base * b.ratioA;
        f32 incB = base * b.ratioB;
        if (b.dstMask & (1u << kDAPitch))
            incA *= std::exp2(clampv(md[kDAPitch] * 24.f, -48.f, 48.f) * (1.f / 12.f));
        if (b.dstMask & (1u << kDBPitch))
            incB *= std::exp2(clampv(md[kDBPitch] * 24.f, -48.f, 48.f) * (1.f / 12.f));

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
            v.fc   = dsp::svfCoeffs(sr_, voiceCutoff(v, b, g.l1, mc, mdC), voiceQ(b, mr, mdR));
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
