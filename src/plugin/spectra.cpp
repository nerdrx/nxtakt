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
#include <cstring>
#include <vector>

namespace lat {
namespace detail {
namespace {

// ---------------------------------------------------------------------------
// Table geometry
// ---------------------------------------------------------------------------

constexpr int kSpTables = 8;
constexpr int kSpFrames = 32;
constexpr int kSpMips   = 10;
constexpr int kSpGen    = 4096;      // analysis length: 4x the harmonics kept

// Harmonic limit and storage length per mip level. See the header.
constexpr int kSpHarm[kSpMips] = { 1023, 511, 255, 127, 63, 31, 15, 7, 3, 1 };
constexpr int kSpLen [kSpMips] = { 2048, 2048, 2048, 1024, 512, 512, 512, 512, 512, 512 };

constexpr int spMipOff(int m) {
    int o = 0;
    for (int i = 0; i < m; ++i) o += kSpLen[i];
    return o;
}
constexpr int kSpStride = spMipOff(kSpMips);          // 10240 floats per frame

// Mip offsets as an array the audio thread can index without a loop.
constexpr int kSpOff[kSpMips] = {
    spMipOff(0), spMipOff(1), spMipOff(2), spMipOff(3), spMipOff(4),
    spMipOff(5), spMipOff(6), spMipOff(7), spMipOff(8), spMipOff(9),
};

// The whole set: 8 tables * 32 frames * 10240 floats = 10.5 MB, built once and
// shared by every instance for the life of the process.
struct SpectraTables {
    std::vector<f32> d;
    const f32* frame(int table, int f) const {
        return d.data() + ((size_t)table * kSpFrames + (size_t)f) * (size_t)kSpStride;
    }
};

// ---------------------------------------------------------------------------
// FFT
//
// In-place iterative radix-2 complex FFT. `tw` holds cos/sin at -2*pi*i/n for
// i in [0, n/2) so the inner loop does no trigonometry.
//
// ATTRIBUTION: this is fftRadix2 from src/audio/sample.cpp, copied rather than
// shared. src/plugin does not include src/audio and should not start doing so
// for twenty-five lines of butterflies; the transient detector's copy stays the
// one the sample layer uses.
// ---------------------------------------------------------------------------

void spFft(f32* re, f32* im, int n, const f32* tw) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len >> 1;
        const int step = n / len;               // stride into the twiddle table
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < half; ++k) {
                const f32 wr = tw[(size_t)(k * step) * 2 + 0];
                const f32 wi = tw[(size_t)(k * step) * 2 + 1];
                const int a = i + k, b = i + k + half;
                const f32 vr = re[b] * wr - im[b] * wi;
                const f32 vi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - vr; im[b] = im[a] - vi;
                re[a] = re[a] + vr; im[a] = im[a] + vi;
            }
        }
    }
}

// IDFT(X) = swap(DFT(swap(X)))/N, so handing the transform its arrays the wrong
// way round twice is the whole inverse. After this the real part is in `re`.
void spIfft(f32* re, f32* im, int n, const f32* tw) {
    spFft(im, re, n, tw);
    const f32 s = 1.f / (f32)n;
    for (int i = 0; i < n; ++i) { re[i] *= s; im[i] *= s; }
}

void spTwiddle(std::vector<f32>& t, int n) {
    t.assign((size_t)(n / 2) * 2, 0.f);
    for (int i = 0; i < n / 2; ++i) {
        const f64 a = -6.283185307179586 * (f64)i / (f64)n;
        t[(size_t)i * 2 + 0] = (f32)std::cos(a);
        t[(size_t)i * 2 + 1] = (f32)std::sin(a);
    }
}

// ---------------------------------------------------------------------------
// The eight tables
//
// A frame is a harmonic spectrum: hr[h] is the cosine coefficient of harmonic
// h and hi[h] is MINUS its sine coefficient, so that synthesising at length N
// means writing X[h] = (N/2)*(hr + i*hi) and its conjugate at N-h. h = 0 is
// never written: no table carries DC.
// ---------------------------------------------------------------------------

constexpr int kSpMaxHarm = 1023;

struct SpSpec {
    f32 hr[kSpMaxHarm + 1];
    f32 hi[kSpMaxHarm + 1];
};

f32 spFrac(f32 x)  { x -= (f32)(int)x; return x < 0.f ? x + 1.f : x; }
f32 spSaw(f32 p)   { return 2.f * spFrac(p) - 1.f; }
f32 spSin(f32 p)   { return std::sin(dsp::kTwoPi * p); }

// Triangle wavefolder. Exactly the identity on [-1, 1] and a reflection above
// it, which is what makes the low end of the Fold table a clean sine.
f32 spFold(f32 x) {
    return (2.f / 3.14159265f) * std::asin(std::sin(1.5707963f * x));
}

// The five tables that are easier to say in the time domain. `t` is the frame
// morph, 0..1; `p` is phase in cycles.
f32 spShape(int table, f32 t, f32 p) {
    switch (table) {
        case 0: {
            // Basic — saw to pulse. Subtracting a half-cycle-shifted copy of the
            // saw cancels the EVEN harmonics progressively, so t = 0 is a saw,
            // t = 1 is a square, and everything between is one continuum rather
            // than a crossfade between two waves.
            return spSaw(p) - t * spSaw(p + 0.5f);
        }
        case 1: {
            // PWM — the difference of two saws a duty cycle apart IS a pulse of
            // that duty cycle, with zero mean at every width.
            const f32 w = 0.5f - 0.45f * t;
            return spSaw(p) - spSaw(p + w);
        }
        case 4: {
            // Bell — two-modulator FM. The ratios are integers so the frame
            // stays periodic (a wavetable frame must be), and 3 and 7 are
            // chosen because the partial cluster they build reads as metallic
            // and inharmonic to the ear even though every partial is a
            // harmonic. The index sweep is the frame morph.
            const f32 idx = 0.05f + 1.35f * t;
            const f32 m   = 0.62f * spSin(3.f * p) + 0.38f * spSin(7.f * p);
            return spSin(p + idx * m);
        }
        case 5: {
            // Digital — hard sync plus quantisation. The synced saw restarts at
            // the master period whatever fraction of its own cycle it is in,
            // which is the discontinuity that makes the sound; the step
            // quantiser adds the bit-crushed edge on top. Both ramp with t, so
            // the frame axis is a straight brightness ramp.
            const f32 r  = 1.f + 3.f * t;
            const f32 lv = std::exp2(6.f - 4.5f * t);       // 64 -> 2.8 levels
            const f32 y  = spSaw(p * r);
            return std::floor(y * lv + 0.5f) / lv;
        }
        default: {
            // Fold — a sine driven into a triangle folder. t = 0 is exactly a
            // sine, because the folder is the identity below unity gain.
            const f32 g = 1.f + 7.f * t;
            return spFold(g * spSin(p));
        }
    }
}

// The three tables that are easier to say as harmonics. Returns the amplitude
// of harmonic h (as a sine), 0 for none.
f32 spPartial(int table, f32 t, int h) {
    const f32 fh = (f32)h;
    switch (table) {
        case 2: {
            // Harmonic — odd against even. At t = 0 only the odd harmonics
            // sound (hollow, clarinet-ish); at t = 1 only the even ones, which
            // is the same waveform an octave up. The fundamental keeps a floor
            // so the perceived pitch stays put instead of jumping at the top of
            // the sweep.
            f32 w = (h & 1) ? (1.f - t) : t;
            if (h == 1) w = std::fmax(w, 0.30f);
            return w / fh;
        }
        case 3: {
            // Formant — three resonant peaks in HARMONIC-NUMBER space (a table
            // has no pitch of its own), sweeping upwards across the frame axis.
            // Narrow gaussians, so the peaks are heard as peaks.
            const f32 f1 = 3.f + 12.f * t,  b1 = 1.6f + 2.0f * t;
            const f32 f2 = 9.f + 30.f * t,  b2 = 3.0f + 5.0f * t;
            const f32 f3 = 18.f + 70.f * t, b3 = 5.0f + 12.f * t;
            auto pk = [fh](f32 c, f32 bw, f32 g) {
                const f32 z = (fh - c) / bw;
                return z * z > 30.f ? 0.f : g * std::exp(-z * z);
            };
            const f32 body = pk(f1, b1, 1.f) + pk(f2, b2, 0.55f) + pk(f3, b3, 0.30f);
            return (body + 0.06f / fh) / std::sqrt(fh);
        }
        default: {
            // Vox — the same idea, softer on purpose: wider peaks that overlap,
            // a steeper base rolloff, and formants that move like a vowel
            // (roughly "ah" towards "ee") instead of climbing. Where table 3 is
            // a filter sweep you can hear resonating, this one is a voice.
            const f32 f1 = 11.f - 6.5f * t, b1 = 4.5f;
            const f32 f2 = 15.f + 22.f * t, b2 = 7.5f;
            const f32 f3 = 30.f + 20.f * t, b3 = 11.f;
            auto pk = [fh](f32 c, f32 bw, f32 g) {
                const f32 z = (fh - c) / bw;
                return z * z > 30.f ? 0.f : g * std::exp(-z * z);
            };
            const f32 body = pk(f1, b1, 1.f) + pk(f2, b2, 0.5f) + pk(f3, b3, 0.22f);
            return (body + 0.10f / fh) / std::pow(fh, 1.3f);
        }
    }
}

bool spIsAdditive(int table) { return table == 2 || table == 3 || table == 6; }

// ---------------------------------------------------------------------------
// Generation. GUI thread, once per process. Allocates freely; nothing here is
// ever reached from the audio thread.
// ---------------------------------------------------------------------------

const SpectraTables* spBuildTables() {
    SpectraTables* out = new SpectraTables();
    out->d.assign((size_t)kSpTables * kSpFrames * kSpStride, 0.f);

    std::vector<f32> twGen, tw[kSpMips];
    spTwiddle(twGen, kSpGen);
    for (int m = 0; m < kSpMips; ++m) spTwiddle(tw[m], kSpLen[m]);

    std::vector<f32> re((size_t)kSpGen), im((size_t)kSpGen);
    std::vector<f32> sre(2048), sim(2048);
    SpSpec spec;

    for (int tb = 0; tb < kSpTables; ++tb) {
        for (int fr = 0; fr < kSpFrames; ++fr) {
            const f32 t = (f32)fr / (f32)(kSpFrames - 1);

            // --- 1. the frame as harmonics
            if (spIsAdditive(tb)) {
                for (int h = 0; h <= kSpMaxHarm; ++h) {
                    const f32 a = h == 0 ? 0.f : spPartial(tb, t, h);
                    spec.hr[h] = 0.f;
                    spec.hi[h] = -a;                 // hi = -(sine coefficient)
                }
            } else {
                for (int i = 0; i < kSpGen; ++i) {
                    re[(size_t)i] = spShape(tb, t, (f32)i / (f32)kSpGen);
                    im[(size_t)i] = 0.f;
                }
                spFft(re.data(), im.data(), kSpGen, twGen.data());
                const f32 s = 2.f / (f32)kSpGen;
                for (int h = 0; h <= kSpMaxHarm; ++h) {
                    spec.hr[h] = h == 0 ? 0.f : re[(size_t)h] * s;
                    spec.hi[h] = h == 0 ? 0.f : im[(size_t)h] * s;
                }
            }

            // --- 2. one band-limited synthesis per mip level
            f32 norm = 1.f;
            for (int m = 0; m < kSpMips; ++m) {
                const int n = kSpLen[m];
                const int hi = kSpHarm[m] < n / 2 - 1 ? kSpHarm[m] : n / 2 - 1;
                for (int i = 0; i < n; ++i) { sre[(size_t)i] = 0.f; sim[(size_t)i] = 0.f; }
                const f32 half = 0.5f * (f32)n;
                for (int h = 1; h <= hi; ++h) {
                    sre[(size_t)h] = half * spec.hr[h];
                    sim[(size_t)h] = half * spec.hi[h];
                    sre[(size_t)(n - h)] =  sre[(size_t)h];
                    sim[(size_t)(n - h)] = -sim[(size_t)h];
                }
                spIfft(sre.data(), sim.data(), n, tw[m].data());

                // Every level of a frame shares ONE normalising factor, taken
                // from the widest one. Normalising each level to its own peak
                // would make a glide across a mip boundary step in loudness;
                // sharing it means the coarser levels are simply quieter, which
                // is what losing their top harmonics actually does.
                if (m == 0) {
                    f32 pk = 0.f;
                    for (int i = 0; i < n; ++i) pk = std::fmax(pk, std::fabs(sre[(size_t)i]));
                    norm = pk > 1e-9f ? 1.f / pk : 1.f;
                }
                f32* dst = const_cast<f32*>(out->frame(tb, fr)) + kSpOff[m];
                for (int i = 0; i < n; ++i) dst[i] = sre[(size_t)i] * norm;
            }
        }
    }
    return out;
}

// Built at the first prepare() and never freed: the set is immutable, every
// instance points into it, and nothing in the program knows when the last
// instance dies. The pointer lives in a static, so it stays a root and the leak
// checker is right not to complain about it.
const SpectraTables& spTables() {
    static const SpectraTables* t = spBuildTables();
    return *t;
}

// ---------------------------------------------------------------------------
// The set as the EDITOR sees it -- see the declaration in internal_base.h for
// what this is, what it is not, and why it is not on the plugin contract.
//
// Two statics and one store, and each of the three is chosen rather than
// reached for:
//
//   * spView() is a function-local static, so the view is built exactly once
//     however many instances prepare() at once, by the same rule that makes
//     spTables() itself safe. It holds the geometry the anonymous namespace
//     above owns (kSpFrames, kSpLen[0], kSpStride) so that the header does not
//     have to, and so that a change to the mip layout cannot leave a second
//     copy of those numbers behind in src/ui.
//
//   * RELEASE / ACQUIRE, and not the relaxed spelling the parameter array uses.
//     That is not inconsistency, it is the difference between the two things:
//     a parameter store publishes a VALUE, which is atomic on its own, and this
//     store publishes ten megabytes of TABLE written before it. Release on the
//     store and acquire on the load are what carry those writes to a reader on
//     another thread; relaxed would let it see the pointer and stale memory
//     behind it. Both compile to the plain load and store on x86-64 and to one
//     cheap barrier on aarch64, so the cost of being right here is nil.
//
//   * Publication is IDEMPOTENT and adds no mutation to anything the audio
//     thread reads. The tables themselves are untouched by all of this: they
//     are built once, by the same call that was already there, and this only
//     hands out their address.
// ---------------------------------------------------------------------------

std::atomic<const SpectraTableSet*> gSpPublished{nullptr};

const SpectraTableSet& spView() {
    static const SpectraTableSet v = [] {
        SpectraTableSet s;
        s.data   = spTables().d.data();
        s.tables = kSpTables;
        s.frames = kSpFrames;
        s.len    = kSpLen[0];
        s.stride = kSpStride;
        return s;
    }();
    return v;
}

// GUI thread, from prepare(). Called once per instance and cheap after the
// first: a compare, and a store of a pointer that is already what is there.
void spPublish() { gSpPublished.store(&spView(), std::memory_order_release); }

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
    kSpParamCount = 42
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
    int         n;
    struct { int id; f32 v; } set[40];
};

const SpPreset kSpPresets[] = {
    { "Init", 0, {} },

    { "Supersaw Lead", 21, {
        { kPATable, 0 }, { kPAPos, 0.02f }, { kPALevel, 0.72f },
        { kPAUni, 7 }, { kPADet, 28.f }, { kPASpread, 0.85f },
        { kPBTable, 0 }, { kPBPos, 0.10f }, { kPBLevel, 0.48f },
        { kPBUni, 7 }, { kPBDet, 22.f }, { kPBSpread, 0.70f }, { kPBFine, 7.f },
        { kPCutoff, 14000.f }, { kPRes, 0.15f },
        { kPAttack, 6.f }, { kPDecay, 1500.f }, { kPSustain, 0.85f }, { kPRelease, 320.f },
        { kPVoices, 8 }, { kPMaster, 0.50f },
    } },

    { "Solid Bass", 21, {
        { kPATable, 0 }, { kPAPos, 0.18f }, { kPALevel, 0.90f },
        { kPAUni, 2 }, { kPADet, 8.f }, { kPASpread, 0.20f },
        { kPSub, 0.35f },
        { kPCutoff, 900.f }, { kPRes, 0.35f }, { kPFType, 0 }, { kPDrive, 6.f },
        { kPE2Cut, 0.55f }, { kPE2Attack, 0.5f }, { kPE2Decay, 180.f }, { kPE2Sustain, 0.f },
        { kPAttack, 1.f }, { kPDecay, 700.f }, { kPSustain, 0.70f }, { kPRelease, 90.f },
        { kPGlide, 25.f }, { kPVoices, 1 },
    } },

    { "Sub Bass", 12, {
        { kPALevel, 0.18f }, { kPAPos, 0.f }, { kPAUni, 1 },
        { kPSub, 1.f },
        { kPCutoff, 300.f }, { kPRes, 0.05f },
        { kPAttack, 4.f }, { kPDecay, 900.f }, { kPSustain, 0.90f }, { kPRelease, 200.f },
        { kPGlide, 40.f }, { kPVoices, 1 },
    } },

    { "Pluck", 18, {
        { kPATable, 2 }, { kPAPos, 0.35f }, { kPALevel, 0.85f },
        { kPAUni, 3 }, { kPADet, 12.f }, { kPASpread, 0.50f },
        { kPCutoff, 500.f }, { kPRes, 0.50f },
        { kPE2Cut, 0.75f }, { kPE2Attack, 0.1f }, { kPE2Decay, 220.f },
        { kPE2Sustain, 0.f }, { kPE2Release, 200.f },
        { kPAttack, 0.5f }, { kPDecay, 320.f }, { kPSustain, 0.f }, { kPRelease, 260.f },
        { kPMaster, 0.62f },
    } },

    { "Warm Pad", 22, {
        { kPATable, 6 }, { kPAPos, 0.25f }, { kPALevel, 0.62f },
        { kPAUni, 5 }, { kPADet, 24.f }, { kPASpread, 0.90f },
        { kPBTable, 3 }, { kPBPos, 0.40f }, { kPBLevel, 0.34f }, { kPBCoarse, 12 },
        { kPBUni, 3 }, { kPBDet, 16.f }, { kPBSpread, 0.60f },
        { kPCutoff, 3200.f }, { kPRes, 0.20f },
        { kPLfoRate, 0.18f }, { kPLfoShape, 0 }, { kPLfoPos, 0.45f },
        { kPAttack, 900.f }, { kPDecay, 2500.f }, { kPSustain, 0.75f }, { kPRelease, 1800.f },
    } },

    { "Formant Keys", 15, {
        { kPATable, 3 }, { kPAPos, 0.30f }, { kPALevel, 0.85f },
        { kPAUni, 2 }, { kPADet, 10.f }, { kPASpread, 0.35f },
        { kPCutoff, 6000.f }, { kPRes, 0.25f }, { kPKeytrack, 0.85f },
        { kPE2Cut, 0.30f }, { kPE2Decay, 400.f }, { kPE2Sustain, 0.20f },
        { kPAttack, 3.f }, { kPDecay, 900.f }, { kPRelease, 350.f },
    } },

    { "Bell", 16, {
        { kPATable, 4 }, { kPAPos, 0.55f }, { kPALevel, 0.78f },
        { kPAUni, 2 }, { kPADet, 6.f }, { kPASpread, 0.40f },
        { kPBTable, 4 }, { kPBPos, 0.80f }, { kPBLevel, 0.28f }, { kPBCoarse, 12 },
        { kPCutoff, 12000.f },
        { kPE2Pos, -0.35f }, { kPE2Decay, 1200.f }, { kPE2Sustain, 0.f },
        { kPAttack, 1.f }, { kPDecay, 2200.f },
    } },

    { "Acid", 19, {
        { kPATable, 5 }, { kPAPos, 0.25f }, { kPALevel, 0.80f }, { kPAUni, 1 },
        { kPCutoff, 320.f }, { kPRes, 0.82f }, { kPFType, 0 }, { kPDrive, 9.f },
        { kPKeytrack, 0.35f },
        { kPE2Cut, 0.80f }, { kPE2Attack, 0.2f }, { kPE2Decay, 260.f }, { kPE2Sustain, 0.05f },
        { kPAttack, 0.5f }, { kPDecay, 900.f }, { kPSustain, 0.60f }, { kPRelease, 80.f },
        { kPGlide, 45.f }, { kPMaster, 0.38f },
    } },

    { "Wobble", 19, {
        { kPATable, 0 }, { kPAPos, 0.50f }, { kPALevel, 0.80f },
        { kPAUni, 5 }, { kPADet, 20.f }, { kPASpread, 0.70f },
        { kPSub, 0.40f },
        { kPCutoff, 700.f }, { kPRes, 0.60f }, { kPDrive, 8.f },
        { kPLfoSync, 5 }, { kPLfoShape, 1 }, { kPLfoCut, 0.85f },
        { kPAttack, 2.f }, { kPDecay, 2000.f }, { kPSustain, 0.90f }, { kPRelease, 200.f },
        { kPVoices, 4 }, { kPMaster, 0.45f },
    } },

    { "Air Pad", 19, {
        { kPATable, 7 }, { kPAPos, 0.45f }, { kPALevel, 0.55f },
        { kPAUni, 5 }, { kPADet, 30.f }, { kPASpread, 1.f },
        { kPNoise, 0.12f },
        { kPCutoff, 8000.f }, { kPRes, 0.15f },
        { kPLfoRate, 0.12f }, { kPLfoShape, 0 }, { kPLfoPos, 0.35f }, { kPLfoPitch, 6.f },
        { kPAttack, 1200.f }, { kPDecay, 3000.f }, { kPSustain, 0.70f }, { kPRelease, 2500.f },
        { kPVoices, 8 }, { kPMaster, 0.55f },
    } },

    { "Organ", 14, {
        { kPATable, 2 }, { kPAPos, 0.f }, { kPALevel, 0.70f }, { kPAUni, 1 },
        { kPBTable, 2 }, { kPBPos, 1.f }, { kPBLevel, 0.45f }, { kPBCoarse, 12 }, { kPBUni, 1 },
        { kPCutoff, 20000.f }, { kPRes, 0.f },
        { kPAttack, 1.f }, { kPDecay, 5.f }, { kPSustain, 1.f },
    } },
};

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
        addIntParam("Filter Type", "",   0, 2, 0);
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

        addParam   ("Glide",  "ms", 0.f, 500.f, 0.f);
        addIntParam("Voices", "",   1, kSpVoices, kSpVoices);
        addParam   ("Master", "",   0.f, 1.5f, 0.7f);
        addParam   ("Env2>Position", "", -1.f, 1.f, 0.f);
    }

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
        for (int k = 0; k < p.n; ++k) setParam(p.set[k].id, p.set[k].v);
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
        const int off = frameOffset < 0 ? 0 : frameOffset;
        switch (status) {
            case 0x90:
                if (len >= 3 && data[2] > 0) { queue(off, kEvOn, data[1], data[2]); return; }
                if (len >= 2) queue(off, kEvOff, data[1], 0);
                return;
            case 0x80:
                if (len >= 2) queue(off, kEvOff, data[1], 0);
                return;
            case 0xB0:
                // 120 = all sound off, 123 = all notes off. Anything else is a
                // controller we do not map; ignoring it is the honest answer.
                if (len >= 2 && data[1] == 120) queue(off, kEvSoundOff, 0, 0);
                else if (len >= 2 && data[1] == 123) queue(off, kEvNotesOff, 0, 0);
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

            // The LFO value for THIS sample, read before it is advanced, so the
            // control tick and the audio path see the same number.
            const f32 lfoV = lfoValue(b.lfoShape);

            // Control tick on ABSOLUTE sample time (the Auto Filter's rule):
            // the counter is a member and survives the block boundary, which is
            // what makes blocks of 1 and of 300 bit-identical to blocks of 256.
            if (ctrl_ <= 0) { retarget(b, lfoV); ctrl_ = kCtrl; }
            --ctrl_;

            f32 accL = 0.f, accR = 0.f;
            for (Voice& v : voices_) {
                if (!v.active) continue;
                renderVoice(v, b, lfoV, accL, accR);
            }

            lfoTick();

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
        Env  e1, e2;
        dsp::SvfCoeffs fc, fInc;
        dsp::SvfState  fs[2];
        bool fSnap = true;
        u32  age = 0;
    };

    // A queued note event. Four bytes of payload and a frame stamp; nothing in
    // here allocates and the queue is a fixed array, so midi() stays realtime.
    enum : u8 { kEvOn = 0, kEvOff, kEvNotesOff, kEvSoundOff };
    struct PendEv { int frame; u8 type, a, b; };

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
    };

    // --- parameter -> coefficient -----------------------------------------

    void readParams(Blk& b) {
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
        buildFan(b.uniA, clampv(p(kPADet), 0.f, 100.f), clampv(p(kPASpread), 0.f, 1.f),
                 b.uratA, b.panLA, b.panRA, b.ugainA, b.maxRatA);
        buildFan(b.uniB, clampv(p(kPBDet), 0.f, 100.f), clampv(p(kPBSpread), 0.f, 1.f),
                 b.uratB, b.panLB, b.panRB, b.ugainB, b.maxRatB);

        b.cutoff   = clampv(p(kPCutoff), 20.f, 20000.f);
        // Resonance 0..1 -> Q 0.5..20 geometrically, as the Auto Filter maps
        // it: a linear map spends most of its travel below where resonance
        // becomes audible.
        b.q        = 0.5f * std::pow(40.f, clampv(p(kPRes), 0.f, 1.f));
        b.fcMax    = (f32)(sr_ * 0.45);
        b.keytrack = clampv(p(kPKeytrack), 0.f, 1.f);
        b.env2Cut  = clampv(p(kPE2Cut), -1.f, 1.f);
        b.lfoCut   = clampv(p(kPLfoCut), -1.f, 1.f);
        b.ftype    = (int)clampv(p(kPFType) + 0.5f, 0.f, 2.f);

        const f32 drv = clampv(p(kPDrive), 0.f, 24.f);
        // A branch, not a computation: at 0 dB the drive stage is a wire and
        // not tanh(x), so the default patch is exactly the oscillator it says
        // it is. The compensation is the Saturator's -- the gain that keeps a
        // -6 dBFS reference at the level it had at unity drive.
        b.drive  = drv > 0.f;
        b.driveG = dbToGain(drv);
        b.driveC = b.drive ? std::tanh(0.5f) / std::tanh(b.driveG * 0.5f) : 1.f;

        b.a1 = atkCoef(clampv(p(kPAttack), 0.1f, 5000.f));
        b.d1 = decCoef(clampv(p(kPDecay), 1.f, 5000.f));
        b.s1 = clampv(p(kPSustain), 0.f, 1.f);
        b.r1 = decCoef(clampv(p(kPRelease), 1.f, 8000.f));
        b.a2 = atkCoef(clampv(p(kPE2Attack), 0.1f, 5000.f));
        b.d2 = decCoef(clampv(p(kPE2Decay), 1.f, 5000.f));
        b.s2 = clampv(p(kPE2Sustain), 0.f, 1.f);
        b.r2 = decCoef(clampv(p(kPE2Release), 1.f, 8000.f));

        b.lfoShape = (int)clampv(p(kPLfoShape) + 0.5f, 0.f, 4.f);
        b.lfoPos   = clampv(p(kPLfoPos), -1.f, 1.f);
        b.lfoPitch = clampv(p(kPLfoPitch), 0.f, 100.f);
        b.env2Pos  = clampv(p(kPE2Pos), -1.f, 1.f);

        const int sync = (int)clampv(p(kPLfoSync) + 0.5f, 0.f, (f32)(kSpSyncCount - 1));
        if (sync > 0) {
            // The pushed transport, with no Tempo parameter behind it: this
            // instrument postdates setTransport, so a knob that only existed to
            // work around its absence would be a wart with no history to excuse
            // it. A host that never pushes one gets 120, stated here and in the
            // contract rather than guessed at.
            const f32 bpm = trBpm_ > 0.0 ? clampv((f32)trBpm_, 20.f, 999.f) : 120.f;
            lfo_.setRate(sr_, bpm / (60.f * kSpSyncBeats[sync]));
        } else {
            lfo_.setRate(sr_, clampv(p(kPLfoRate), 0.01f, 40.f));
        }

        b.master   = clampv(p(kPMaster), 0.f, 1.5f);
        b.incScale = (f32)(440.0 / sr_);
    }

    // The unison fan: detune spreads SYMMETRICALLY about the centre and the
    // spread pans that same fan, so voice u sits at the same place in pitch and
    // in the image. Panning is constant power and never inverts a polarity,
    // which is the whole reason a mono sum of this cannot cancel.
    static void buildFan(int u, f32 detune, f32 spread,
                         f32* rat, f32* panL, f32* panR, f32& gain, f32& maxRat) {
        maxRat = 1.f;
        for (int i = 0; i < u; ++i) {
            const f32 off = (u > 1) ? ((f32)i / (f32)(u - 1)) * 2.f - 1.f : 0.f;
            const f32 ct  = off * detune * 0.5f;          // total spread = detune
            rat[i] = std::exp2(ct * (1.f / 1200.f));
            if (rat[i] > maxRat) maxRat = rat[i];
            const f32 th = (off * spread + 1.f) * 0.7853981f;   // 0 .. pi/2
            panL[i] = 1.4142136f * std::cos(th);
            panR[i] = 1.4142136f * std::sin(th);
        }
        for (int i = u; i < kUni; ++i) { rat[i] = 1.f; panL[i] = 0.f; panR[i] = 0.f; }
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

    f32 lfoValue(int shape) const {
        const f32 ph = lfo_.phase;
        switch (shape) {
            case 1:  return ph < 0.5f ? (4.f * ph - 1.f) : (3.f - 4.f * ph);   // triangle
            case 2:  return 2.f * ph - 1.f;                                    // saw up
            case 3:  return ph < 0.5f ? 1.f : -1.f;                            // square
            case 4:  return shVal_;                                            // sample & hold
            default: return std::sin(dsp::kTwoPi * ph);
        }
    }

    void lfoTick() {
        const f32 was = lfo_.phase;
        lfo_.tick();
        // A new sample-and-hold value on every wrap. Drawn from the LFO's OWN
        // counter, never the note counter -- see the file header.
        if (lfo_.phase < was) shVal_ = 2.f * rnd(lfoRng_) - 1.f;
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
    void queue(int frame, u8 type, u8 a, u8 b) {
        if (type == kEvOn) {
            if (nPend_ >= kOnCap) return;              // droppable, by the argument above
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
            case kEvOn:       noteOn(e.a, e.b); break;
            case kEvOff:      noteOff(e.a); break;
            case kEvNotesOff: allNotesOff(); break;
            default:          allSoundOff(); break;
        }
    }

    void noteOn(u8 note, u8 vel) {
        Voice* pv = alloc();
        Voice& v = *pv;
        v = Voice{};
        v.active = true;
        v.note   = note;
        // Velocity with a 30% floor (the contract's fixed routing): the softest
        // possible note is quiet, not inaudible.
        v.velAmp = 0.30f + 0.70f * ((f32)vel * (1.f / 127.f));
        v.pitchTarget = (f32)note;

        const f32 glideMs = clampv(p(kPGlide), 0.f, 500.f);
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
        v.fs[0].reset();
        v.fs[1].reset();
        v.fSnap = true;
        v.age = ++age_;
    }

    // Newest matching voice first: a repeated note that stole its own older
    // voice should release the one actually sounding.
    void noteOff(u8 note) {
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
    }
    void allSoundOff() { for (Voice& v : voices_) v = Voice{}; }

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

    // --- per-voice cutoff --------------------------------------------------

    f32 voiceCutoff(const Voice& v, const Blk& b, f32 lfoV) const {
        const f32 kt = b.keytrack * ((f32)v.note - 60.f) * (1.f / 12.f);
        const f32 e2 = b.env2Cut * v.e2.v * kEnvCutOct;
        const f32 lf = b.lfoCut * lfoV * kLfoCutOct;
        return clampv(b.cutoff * std::exp2(kt + e2 + lf), 20.f, b.fcMax);
    }

    // One control tick: every sounding voice gets a new coefficient target and
    // the per-sample slope that walks it there over the next kCtrl samples.
    void retarget(const Blk& b, f32 lfoV) {
        const f32 invk = 1.f / (f32)kCtrl;
        for (Voice& v : voices_) {
            if (!v.active || v.fSnap) continue;      // fSnap: snapped on first sample
            const dsp::SvfCoeffs tgt = dsp::svfCoeffs(sr_, voiceCutoff(v, b, lfoV), b.q);
            v.fInc = dsp::svfSlope(v.fc, tgt, invk);
        }
    }

    // --- the voice -----------------------------------------------------------

    inline void renderVoice(Voice& v, const Blk& b, f32 lfoV, f32& accL, f32& accR) {
        // Envelopes, per sample. Cheap enough that a control rate would buy
        // nothing, and per-sample is inherently block-size invariant.
        envTick(v.e1, b.a1, b.d1, b.s1, b.r1);
        envTick(v.e2, b.a2, b.d2, b.s2, b.r2);
        if (v.e1.stage == kIdle) { v.active = false; return; }

        // Glide, then vibrato, then the increment.
        if (v.glideLeft > 0) {
            v.pitch += v.glideStep;
            if (--v.glideLeft == 0) v.pitch = v.pitchTarget;
        }
        const f32 midi = v.pitch + b.lfoPitch * lfoV * 0.01f;
        const f32 base = clampv(b.incScale * std::exp2((midi - 69.f) * (1.f / 12.f)),
                                0.f, 0.45f);

        // Position: the parameter plus both modulators, clamped into the frame
        // axis. ENV2 is per voice, so this is too.
        const f32 mod = b.lfoPos * lfoV + b.env2Pos * v.e2.v;

        f32 xl = 0.f, xr = 0.f;

        if (b.lvlA > 0.f) oscillator(v.phA, b.tblA, b.posA + mod, base * b.ratioA,
                                     b.uniA, b.uratA, b.panLA, b.panRA,
                                     b.ugainA * b.lvlA, b.maxRatA, xl, xr);
        else               advance(v.phA, base * b.ratioA, b.uniA, b.uratA);

        if (b.lvlB > 0.f) oscillator(v.phB, b.tblB, b.posB + mod, base * b.ratioB,
                                     b.uniB, b.uratB, b.panLB, b.panRB,
                                     b.ugainB * b.lvlB, b.maxRatB, xl, xr);
        else               advance(v.phB, base * b.ratioB, b.uniB, b.uratB);

        // Noise: always drawn, whatever the level, so the voice's random stream
        // is a function of time alone and not of a parameter.
        const f32 nl = 2.f * rnd(v.rng) - 1.f;
        const f32 nr = 2.f * rnd(v.rng) - 1.f;
        xl += nl * b.lvlN;
        xr += nr * b.lvlN;

        // Sub: one octave below the note, so it follows the glide for free.
        const f32 subInc = base * 0.5f;
        if (b.lvlSub > 0.f) {
            const f32 s = std::sin(dsp::kTwoPi * v.subPh) * b.lvlSub;
            xl += s;
            xr += s;
        }
        v.subPh += subInc;
        if (v.subPh >= 1.f) v.subPh -= 1.f;

        if (b.drive) {
            xl = std::tanh(b.driveG * xl) * b.driveC;
            xr = std::tanh(b.driveG * xr) * b.driveC;
        }

        // Filter. Coefficients are snapped on a voice's very first sample (one
        // tan per note-on) and walked between control ticks after that.
        if (v.fSnap) {
            v.fc   = dsp::svfCoeffs(sr_, voiceCutoff(v, b, lfoV), b.q);
            v.fInc = dsp::SvfCoeffs{ 0.f, 0.f, 0.f, 0.f };
            v.fSnap = false;
        }
        const dsp::SvfOut ol = dsp::svfTick(v.fc, v.fs[0], xl);
        const dsp::SvfOut orr = dsp::svfTick(v.fc, v.fs[1], xr);
        dsp::svfStep(v.fc, v.fInc);

        // The bandpass is normalised by k = 1/Q so its peak stays at unity, for
        // the reason the Auto Filter gives: an SVF's raw bandpass tap has a peak
        // gain of Q, and a band filter that gets 26 dB louder as the resonance
        // knob turns is a hazard rather than a feature.
        f32 yl, yr;
        if (b.ftype == 0)      { yl = ol.lp; yr = orr.lp; }
        else if (b.ftype == 1) { yl = ol.bp * v.fc.k; yr = orr.bp * v.fc.k; }
        else                   { yl = ol.hp; yr = orr.hp; }

        const f32 amp = v.e1.v * v.velAmp;
        accL += yl * amp;
        accR += yr * amp;
    }

    // One oscillator: mip choice from the fan's HIGHEST increment, frame pair
    // from the position, then one linear read per unison voice.
    inline void oscillator(f32* ph, const f32* tbl, f32 pos, f32 inc, int u,
                           const f32* rat, const f32* panL, const f32* panR,
                           f32 gain, f32 maxRat, f32& outL, f32& outR) {
        const f32 fpos = clampv(pos, 0.f, 1.f) * (f32)(kSpFrames - 1);
        int f0 = (int)fpos;
        if (f0 > kSpFrames - 2) f0 = kSpFrames - 2;
        if (f0 < 0) f0 = 0;
        const f32 ff = fpos - (f32)f0;
        const int f1 = f0 + 1;

        const f32 mipf = spLog2(inc * maxRat) + 12.f;
        int m0 = (int)mipf;
        f32 mf = mipf - (f32)m0;
        if (m0 < 0)             { m0 = 0; mf = 0.f; }
        if (m0 > kSpMips - 2)   { m0 = kSpMips - 2; mf = 1.f; }

        f32 l = 0.f, r = 0.f;
        for (int i = 0; i < u; ++i) {
            const f32 s = spRead(tbl, f0, f1, ff, m0, mf, ph[i]);
            l += s * panL[i];
            r += s * panR[i];
            ph[i] += inc * rat[i];
            if (ph[i] >= 1.f) ph[i] -= 1.f;
            if (!(ph[i] >= 0.f && ph[i] < 1.f)) ph[i] = 0.f;
        }
        outL += l * gain;
        outR += r * gain;
    }

    // The same phase bookkeeping with no read, for a silent oscillator.
    static inline void advance(f32* ph, f32 inc, int u, const f32* rat) {
        for (int i = 0; i < u; ++i) {
            ph[i] += inc * rat[i];
            if (ph[i] >= 1.f) ph[i] -= 1.f;
            if (!(ph[i] >= 0.f && ph[i] < 1.f)) ph[i] = 0.f;
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
    // TWO counters, deliberately: see the file header. The note stream and the
    // sample-and-hold stream must not be able to interleave into each other.
    u32   noteRng_ = 0x9E3779B9u;
    u32   lfoRng_  = 0x2545F491u;
    f32   lastPitch_ = 60.f;
    bool  havePitch_ = false;
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
