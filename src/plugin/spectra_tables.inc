// Spectra's wavetable pipeline — table geometry, the FFT, the eight procedural
// tables, the mip builder and the set the editor reads.
//
// A TEXTUAL INCLUDE, not a translation unit: this is a slab of spectra.cpp's
// anonymous namespace, cut out at a seam that already existed. It compiles
// nowhere else and carries no include guard because it is #included exactly
// once, from spectra.cpp, at the point this text was lifted from. Everything it
// needs — the headers, `namespace lat::detail::{anonymous}` — is open around
// the include site.
//
// WHY IT WAS CUT: two waves need spectra.cpp at once — one grows the table
// pipeline (custom wavetable import, docs/SPECTRA-V3-PLAN.md pillar 1), the
// other grows the voice and its modulation (pillar 2). Disjoint file ownership
// is how this project parallelises, so the file became two along the line the
// two jobs already fell on. The extraction is a PURE MOVE: it changes no byte
// of generated audio, which its render gate proves.
//
// The mip-chain reasoning, the level-selection rule and the determinism
// obligations are documented at the top of spectra.cpp and not repeated here.
//
// V3 ADDS THE CUSTOM SLOT. `A/B Table` widens 0..7 -> 0..8; slot 8 is THIS
// OSCILLATOR'S imported table (docs/SPECTRA-PARAMS.md, "Custom wavetable
// slots"). Everything about it that is audible lives at the bottom of this
// file, behind five functions that are the whole seam between the wave that
// imports wavetables and the wave that plays them. The frames themselves --
// the reader, the three interpretation rules, the content hash, the disk cache
// and the process store -- are src/plugin/wavetable_io.{h,cpp}, which knows
// nothing about mips; this file knows nothing about files.

// The include has to close and re-open the namespaces this text is pasted
// into, because that is what a textual include costs: `#include` is a
// file-scope directive and this slab lives three namespaces deep inside
// spectra.cpp. The alternative was one line in spectra.cpp, which is another
// wave's file this week. The three names below are exactly the three the
// header of this file names as "open around the include site".
} // namespace   (the anonymous one)
} // namespace detail
} // namespace lat
#include "wavetable_io.h"
#include <chrono>
#include <memory>
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
// FFT -- MOVED, v5. The bodies now live in src/plugin/wavetable_io.cpp as
// wt::fft / wt::ifft / wt::twiddle, and these three names are what is left of
// them: thin forwarders, kept so that every call site below reads exactly as it
// did and the diff that moved them is one file.
//
// WHY IT MOVED. v5's harmonic pen and its frame morph need a forward analysis
// and an inverse synthesis at wt::kCycle, above the seam, on a working copy
// that has no mip and no hash yet. Those live in wavetable_io.cpp -- the file
// that owns a wavetable's frames -- and the alternative to moving the transform
// was a THIRD radix-2 FFT in this tree (src/audio/sample.cpp has one; this had
// one). The v5 implementation notes name the choice: "the fill and the render
// share one transform and one set of tables. A second FFT in this device would
// be a second thing to keep in agreement."
//
// THE MOVE IS ARITHMETICALLY NEUTRAL, and the release gate is the proof rather
// than the claim: 1500 reference renders -- 121 factory rows plus one row per
// factory .nxwt, at 44.1/48/96 kHz and block 1/7/64/1024 -- cmp-identical to
// the v4 tip. Nothing is inlined across the boundary that was inlined before,
// and at these flags (-O2, no -march, no -ffast-math) f32 SSE arithmetic has no
// contraction to lose.
//
// ATTRIBUTION, unchanged: this is fftRadix2 from src/audio/sample.cpp, copied
// rather than shared, because src/plugin does not include src/audio.
// ---------------------------------------------------------------------------

inline void spFft(f32* re, f32* im, int n, const f32* tw) { wt::fft(re, im, n, tw); }
inline void spIfft(f32* re, f32* im, int n, const f32* tw) { wt::ifft(re, im, n, tw); }
inline void spTwiddle(std::vector<f32>& t, int n) { wt::twiddle(t, n); }

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
// THE MIP BUILDER -- one implementation, and the only one
//
// Takes ONE frame as a harmonic spectrum and writes that frame's whole mip
// chain, kSpStride floats. The eight factory tables go through it and so does
// every imported one, which is the plan's "build the SAME per-octave
// FFT-truncated mip chain the factory tables get -- factor that builder out and
// share it, one implementation, proven once".
//
// `norm` IS THE ONE THING THE TWO CALLERS DISAGREE ABOUT, and the disagreement
// is deliberate rather than a wart:
//
//   * <= 0 means DERIVE IT from this frame's own widest mip, which is what the
//     factory has always done: each procedural frame is scaled so its mip 0
//     peaks at exactly 1. Those frames come out of eight unrelated formulae
//     with no common level, so per-frame is the only normalisation that makes
//     the set playable, and the editor's SpectraTableSet::frame() documents
//     that property to anyone drawing one.
//   * > 0 means USE IT. An imported table is normalised as a SET -- inter-frame
//     level is musical, and a table whose upper frames are meant to be louder
//     than its lower ones must stay that way -- so the custom builder passes 1
//     here and applies one set-wide factor across all 32 frames afterwards.
//
// What does NOT vary is the rule underneath both: every LEVEL of a frame shares
// ONE factor. Normalising each level to its own peak would make a glide across
// a mip boundary step in loudness; sharing it means the coarser levels are
// simply quieter, which is what losing their top harmonics actually does.
//
// `sre`/`sim` are scratch of at least kSpLen[0]; passed in so a loop over a
// thousand frames does not allocate a thousand times.
// ---------------------------------------------------------------------------

void spBuildFrameMips(const SpSpec& spec, f32* dst, const std::vector<f32>* tw,
                      std::vector<f32>& sre, std::vector<f32>& sim, f32 norm) {
    f32 g = norm > 0.f ? norm : 1.f;
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

        if (m == 0 && norm <= 0.f) {
            f32 pk = 0.f;
            for (int i = 0; i < n; ++i) pk = std::fmax(pk, std::fabs(sre[(size_t)i]));
            g = pk > 1e-9f ? 1.f / pk : 1.f;
        }
        f32* d = dst + kSpOff[m];
        for (int i = 0; i < n; ++i) d[i] = sre[(size_t)i] * g;
    }
}

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

            // --- 2. one band-limited synthesis per mip level, through the
            //        shared builder. `norm` = -1 asks it to derive the factor
            //        from this frame's own widest mip, which is byte for byte
            //        what the loop that used to live here did.
            spBuildFrameMips(spec, const_cast<f32*>(out->frame(tb, fr)), tw,
                             sre, sim, -1.f);
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

// The factory set's base, published by the same call and read by spTableBase()
// on the AUDIO thread. A pointer and not a call to spTables(), because
// spTables() is a function-local static and a magic-static guard is a thing the
// audio path should not have to reason about; by the time a voice exists,
// prepare() has already run this store.
std::atomic<const f32*> gSpFactoryBase{nullptr};

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
void spPublish() {
    gSpFactoryBase.store(spTables().d.data(), std::memory_order_release);
    gSpPublished.store(&spView(), std::memory_order_release);
}

// ---------------------------------------------------------------------------
// CUSTOM WAVETABLES — the seam
//
// Five functions are the contract between the wave that imports wavetables and
// the wave that plays them (docs/SPECTRA-V3-PLAN.md pillar 1). Everything else
// here exists to make those five true.
//
// THE LAYOUT IS THE FACTORY'S, EXACTLY. A built custom table is
// kSpFrames * kSpStride floats — 32 frames of the identical ten-level mip
// chain — so spRead() reaches it with nothing changed but the base pointer it
// was handed. That is a constraint and it has a price, stated here rather than
// discovered later: a source file with more than 32 frames is DECIMATED to 32
// at import (wavetable_io.cpp, "THE FRAME-AXIS CAP"), before the hash, so a
// 256-frame Serum table plays as the 32 frames evenly spread across it. The
// alternative is a variable frame count in the audio-rate read, which is a
// change to the read and to the editor's SpectraTableSet, and the read was not
// this wave's to change.
//
// -------------------- THE LIFETIME INVARIANT, VERBATIM --------------------
//
//   A BUILT CUSTOM TABLE IS ALLOCATED ONCE AND NEVER FREED, and that is the
//   whole of its retirement proof: the audio thread holds a raw base pointer
//   into one for as long as a block lasts, nothing inside a PluginInstance can
//   know when it last dereferenced that pointer, and there is no event in this
//   engine that says "no voice is inside this table any more". So the pointer
//   is made permanently valid instead of proved dead — the same trade
//   spTables() itself makes for the factory set, for the same reason, and the
//   reason SamplerControl::reclaim() exists one level up is that a sampler
//   could not make it.
//
//   The cost is bounded and the bound is kSpMaxCustom: past it an import is
//   REFUSED with a log line rather than served by evicting something a voice
//   may be reading. The store is a function-local static, so every allocation
//   in it stays reachable from a root and the leak checker is right not to
//   complain.
// ---------------------------------------------------------------------------

// The widened `A/B Table` enum's one new value. 0..7 stay exactly what they
// were; custom space BEGINS at 8 and a later revision widens it again.
constexpr int kSpCustomSlot = kSpTables;

// An OSCILLATOR HANDLE, not an oscillator index — see spAcquireOsc(). Two per
// instance, so this is 128 Spectras with custom tables in one set.
constexpr int kSpMaxOsc = 256;

// The most distinct custom tables one process will BUILD. Each is 1.31 MB of
// mip chain (32 * kSpStride * 4), so this is 42 MB of worst case against the
// factory set's 10.5.
constexpr int kSpMaxCustom = 32;

struct SpBuiltTable {
    u64              hash = 0;
    std::vector<f32> d;                 // kSpFrames * kSpStride
};

// Function-local static: a root for the leak checker, and built on first use
// rather than at load, which keeps a process that never imports at zero cost.
std::vector<std::unique_ptr<SpBuiltTable>>& spBuiltStore() {
    static std::vector<std::unique_ptr<SpBuiltTable>> v;
    return v;
}

// What the AUDIO thread reads: one published base per oscillator handle, or
// null for "this oscillator has no resolved custom table". Release on the
// store and acquire on the load, and for the reason spView() states at length —
// this publishes 1.31 MB of table written before it, not a value.
std::atomic<const f32*> gSpCustomBase[kSpMaxOsc];

// What the GUI thread keeps. NOT read by the audio thread and not atomic.
//
// The record is kept EVEN WHEN RESOLUTION FAILED, which is the refusal contract
// doing its job: docs/SPECTRA-PARAMS.md requires stateString() to re-emit the
// hash and the path verbatim on a machine that is missing the file, so that
// saving there does not lose the file's name.
struct SpOscRec {
    u64         hash   = 0;      // 0 = no custom table at all
    int         frames = 0;      // 0 = named but unresolved (the amber case)
    std::string path;            // "" when the state named none (a preset)
    std::string name;            // basename(path), or the bare hash
    bool        used   = false;  // handle allocation

    // --- v5. THE COMMITTED BASE, remembered so that a preview can be undone.
    // gSpCustomBase[osc] is what the AUDIO thread reads and a preview publish
    // overwrites it, so cancelPreview() needs somewhere to read the committed
    // pointer back from. Null when this oscillator has no resolved table, which
    // is also exactly what a cancel should then publish.
    const f32*  base   = nullptr;
};
SpOscRec gSpOsc[kSpMaxOsc];

std::string spBaseName(const std::string& p) {
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

// THE MIP BUILD ITSELF, factored out of spBuildCustomMips() by v5 so that the
// PREVIEW ARENA can call it with a destination that is not in the built store.
// Not one character of the arithmetic moved: same order, same scratch, same
// analysis length, same per-set normalisation. `src` is `nSrc * wt::kCycle`
// floats, frame-major, 1..kSpFrames of them; `dst` is kSpFrames * kSpStride.
void spBuildMipsInto(const f32* src, int nSrc, f32* dst) {
    std::vector<f32> twSrc, tw[kSpMips];
    spTwiddle(twSrc, wt::kCycle);
    for (int m = 0; m < kSpMips; ++m) spTwiddle(tw[m], kSpLen[m]);

    std::vector<f32> re((size_t)wt::kCycle), im((size_t)wt::kCycle);
    std::vector<f32> sre(2048), sim(2048);
    SpSpec spec;

    const int n = nSrc;
    for (int fr = 0; fr < kSpFrames; ++fr) {
        // THE FRAME AXIS. An import has 1..32 frames and the layout has exactly
        // 32, so the source axis is stretched onto the destination one by the
        // same LINEAR interpolation the audio-rate read already performs
        // between adjacent frames (spRead's `ff`). Nearest-frame would put a
        // step into a morph the read is built to make continuous; a single-
        // frame import lands as 32 identical frames, which is what a table with
        // no frame axis is.
        const f64 x = n <= 1 ? 0.0
                             : (f64)fr * (f64)(n - 1) / (f64)(kSpFrames - 1);
        int s0 = (int)x;
        if (s0 > n - 1) s0 = n - 1;
        const int s1 = s0 + 1 < n ? s0 + 1 : s0;
        const f32 bl = (f32)(x - (f64)s0);
        const f32* a = src + (size_t)s0 * (size_t)wt::kCycle;
        const f32* c = src + (size_t)s1 * (size_t)wt::kCycle;
        for (int i = 0; i < wt::kCycle; ++i) {
            re[(size_t)i] = a[i] + (c[i] - a[i]) * bl;
            im[(size_t)i] = 0.f;
        }

        // The analysis length is the cycle length itself, and it is exactly
        // right rather than merely convenient: a 2048-sample cycle carries
        // harmonics 1..1023 and a Nyquist bin, kSpMaxHarm is 1023, so every
        // harmonic the file actually has is kept and nothing is aliased. (The
        // factory's five shaped tables analyse at 4096 because they RENDER a
        // formula that has harmonics above the limit; an imported cycle cannot.)
        spFft(re.data(), im.data(), wt::kCycle, twSrc.data());
        const f32 s = 2.f / (f32)wt::kCycle;
        for (int h = 0; h <= kSpMaxHarm; ++h) {
            spec.hr[h] = h == 0 ? 0.f : re[(size_t)h] * s;
            spec.hi[h] = h == 0 ? 0.f : im[(size_t)h] * s;
        }
        // norm = 1: DO NOT normalise per frame. The frames arrived normalised
        // as a SET and inter-frame level is musical.
        spBuildFrameMips(spec, dst + (size_t)fr * (size_t)kSpStride,
                         tw, sre, sim, 1.f);
    }

    // ONE set-wide factor, taken across every frame's widest mip. The frames
    // were normalised as a set in the time domain; band-limiting moves the peak
    // by a hair (Gibbs at a discontinuity), so this puts the SET back on 1
    // without ever putting a FRAME on 1.
    f32 pk = 0.f;
    for (int fr = 0; fr < kSpFrames; ++fr) {
        const f32* m0 = dst + (size_t)fr * (size_t)kSpStride + kSpOff[0];
        for (int i = 0; i < kSpLen[0]; ++i) pk = std::fmax(pk, std::fabs(m0[i]));
    }
    const f32 g = pk > 1e-9f ? 1.f / pk : 1.f;
    const size_t total = (size_t)kSpFrames * (size_t)kSpStride;
    for (size_t i = 0; i < total; ++i) dst[i] *= g;
}

// The mip chain for one imported table, built once per HASH and shared by every
// oscillator that names it. Null when the store is full.
const f32* spBuildCustomMips(const wt::Table& t) {
    for (const std::unique_ptr<SpBuiltTable>& b : spBuiltStore())
        if (b->hash == t.hash) return b->d.data();
    if ((int)spBuiltStore().size() >= kSpMaxCustom) {
        LOGW("spectra: this process has already built %d custom wavetables; "
             "further imports are refused rather than evicted, because a voice "
             "may be reading one", kSpMaxCustom);
        return nullptr;
    }

    auto b = std::make_unique<SpBuiltTable>();
    b->hash = t.hash;
    b->d.assign((size_t)kSpFrames * (size_t)kSpStride, 0.f);
    spBuildMipsInto(t.data.data(), t.frames, b->d.data());

    spBuiltStore().push_back(std::move(b));
    LOGI("spectra: built a custom wavetable, %s, %d source frame(s) -> %d",
         wt::hashHex(t.hash).c_str(), t.frames, kSpFrames);
    return spBuiltStore().back()->d.data();
}

// ---------------------------------------------------------------------------
// THE FIVE. Nothing below renames, reorders or widens them.
//
// `[[maybe_unused]]` on the free functions and not on the class: their callers
// live in spectra.cpp, which this wave does not own, and an anonymous-namespace
// function with no caller yet is a -Wunused-function against a zero-warning
// gate. The attribute goes away with the diff that wires them up.
// ---------------------------------------------------------------------------

// slot 0..7 = factory; slot 8 = this oscillator's import, nullptr if unresolved.
// Audio thread. Never allocates, never locks.
//
// NULL IS A REAL ANSWER and the caller must have a branch for it: the refusal
// contract says an oscillator whose slot 8 will not resolve renders FACTORY
// TABLE 0, so the call site is `spTableFor()` below unless it has its own
// reason. Silence would be a worse lie than the wrong table.
[[maybe_unused]] const f32* spTableBase(int slot, int osc) noexcept {
    if ((unsigned)slot < (unsigned)kSpTables) {
        const f32* base = gSpFactoryBase.load(std::memory_order_acquire);
        return base ? base + (size_t)slot * (size_t)kSpFrames * (size_t)kSpStride
                    : nullptr;
    }
    if (slot == kSpCustomSlot && (unsigned)osc < (unsigned)kSpMaxOsc)
        return gSpCustomBase[osc].load(std::memory_order_acquire);
    return nullptr;
}

// spTableBase() with the refusal contract's fallback applied: factory table 0
// whenever the asked-for slot has nothing behind it. Audio thread, same rules.
// This is what a voice should call; spTableBase is what it is built on.
[[maybe_unused]] const f32* spTableFor(int slot, int osc) noexcept {
    if (const f32* b = spTableBase(slot, osc)) return b;
    return spTableBase(0, osc);
}

// GUI thread. Resolve from a state string's identity; true if osc now has slot 8.
//
// The record is written FIRST and kept whatever happens next, so a machine that
// cannot find the file still re-emits the hash and the path it was given.
[[maybe_unused]] bool spResolveCustom(int osc, u64 hash, const char* path) {
    if ((unsigned)osc >= (unsigned)kSpMaxOsc) return false;
    SpOscRec& r = gSpOsc[osc];
    r.hash   = hash;
    r.path   = path ? path : "";
    r.frames = 0;
    r.name   = r.path.empty() ? (hash ? wt::hashHex(hash) : std::string())
                              : spBaseName(r.path);
    r.base   = nullptr;
    gSpCustomBase[osc].store(nullptr, std::memory_order_release);
    if (hash == 0) return false;

    const wt::Table* t = wt::resolve(hash, r.path.empty() ? nullptr : r.path.c_str());
    if (!t) return false;
    const f32* base = spBuildCustomMips(*t);
    if (!base) return false;
    r.frames = t->frames;
    r.base   = base;
    gSpCustomBase[osc].store(base, std::memory_order_release);
    return true;
}

// GUI thread. Import a WAV; returns content hash, 0 on failure, err gets a sentence.
[[maybe_unused]] u64 spImportWavetable(int osc, const char* path, std::string& err) {
    err.clear();
    if ((unsigned)osc >= (unsigned)kSpMaxOsc) {
        err = "this instance has no oscillator slot for a custom table";
        return 0;
    }
    if (!path || !*path) { err = "no file"; return 0; }

    wt::Table t;
    if (!wt::importFile(path, t, err)) return 0;
    const wt::Table* stored = wt::adopt(t, true);
    if (!stored) {
        err = "this process already holds as many wavetables as it will hold";
        return 0;
    }
    const f32* base = spBuildCustomMips(*stored);
    if (!base) {
        err = "this process already holds as many wavetables as it will hold";
        return 0;
    }
    SpOscRec& r = gSpOsc[osc];
    r.hash   = stored->hash;
    r.path   = path;
    r.name   = spBaseName(r.path);
    r.frames = stored->frames;
    r.base   = base;
    gSpCustomBase[osc].store(base, std::memory_order_release);
    LOGI("spectra: imported %s as %s (%s, %d source frame(s) of %d samples)",
         path, wt::hashHex(stored->hash).c_str(), wt::ruleName(stored->rule),
         stored->srcFrames, stored->srcCycle);
    return stored->hash;
}

// GUI thread. What osc has now, for stateString() and the editor.
[[maybe_unused]] u64 spCustomHash(int osc) {
    return (unsigned)osc < (unsigned)kSpMaxOsc ? gSpOsc[osc].hash : 0;
}

[[maybe_unused]] const char* spCustomPath(int osc) {   // "" when none
    return (unsigned)osc < (unsigned)kSpMaxOsc ? gSpOsc[osc].path.c_str() : "";
}

// The SOURCE frame count of the resolved table — 1..32 — and 0 when the
// oscillator names a table it could not find. Not part of the five, and it
// exists for one caller: WavetableControl::customFrames(), which reports what
// the FILE had rather than the 32 slots the audio layout always has.
[[maybe_unused]] int spCustomFrames(int osc) {
    return (unsigned)osc < (unsigned)kSpMaxOsc ? gSpOsc[osc].frames : 0;
}

[[maybe_unused]] void spClearCustom(int osc) {
    if ((unsigned)osc >= (unsigned)kSpMaxOsc) return;
    gSpCustomBase[osc].store(nullptr, std::memory_order_release);
    SpOscRec& r = gSpOsc[osc];
    r.hash = 0;
    r.frames = 0;
    r.path.clear();
    r.name.clear();
    r.base = nullptr;
    // The BUILT table is not touched: see the lifetime invariant above. Neither
    // is the PREVIEW ARENA: it is four buffers that are also never freed, and
    // the store above has just published null, which is what a cancel publishes
    // for an oscillator with nothing committed.
}

// ---------------------------------------------------------------------------
// v5 — THE PREVIEW ARENA, and the bound that made it necessary
//
// docs/SPECTRA-PARAMS.md, "Preview, and the bound that made it necessary", and
// v5 resolution #14. This is the single largest thing the feature's outline did
// not contain, and it is the reason the feature is buildable at all.
//
// THE ARITHMETIC THAT FORCES IT. spBuildCustomMips() allocates
// kSpFrames * kSpStride * 4 = 32 * 10240 * 4 = 1,310,720 bytes -- 1.31 MB --
// per DISTINCT HASH into spBuiltStore(), which is NEVER FREED (the lifetime
// invariant: the audio thread holds a raw base pointer and there is no event in
// this engine that says no voice is inside a table any more) and is bounded at
// kSpMaxCustom = 32. An editor that committed on every stroke would therefore
// take the thirty-third stroke and be REFUSED, roughly a minute into a drawing.
//
// THE MECHANISM. A per-oscillator ring of kSpPreviewRing = 4 buffers of the
// same kSpFrames * kSpStride floats, allocated on that oscillator's first
// preview and never freed -- the lifetime invariant applied to a fixed four
// rather than to an unbounded store. 5.24 MB per oscillator with an open
// editor, which is one or two at a time. The ring is OUTSIDE spBuiltStore()
// and OUTSIDE identity: a preview touches no hash, no file, no `wt` record and
// no store entry, and stateString() during an open editor names the last
// COMMITTED table and nothing else.
//
// THE RECYCLE PROOF, and it is arithmetic rather than timing luck.
//
//   * The audio thread takes the base at the top of a block and does not retain
//     it past that block. So a buffer is safe to rewrite once one block period
//     has elapsed since the last read that could have taken it.
//   * A ring of four is not rewritten until THREE FURTHER PUBLISHES have
//     happened after the one that wrote it.
//   * Publishes are rate-limited to `interval` and are on STROKE END, so the
//     minimum time between a write and its overwrite is 3 * interval, and the
//     contract's bound is stated with the slack of a fourth: the protection is
//     4 * interval against one block period of exposure.
//   * interval = max(50 ms, 2 * maxBlock / sampleRate), computed from the
//     values prepare() was given rather than hard-coded, so the bound holds at
//     every rate and block size the host can choose:
//
//         4 * interval  >=  4 * 2 * maxBlock / sampleRate
//                       =   8 * (maxBlock / sampleRate)
//                       >   maxBlock / sampleRate                 [the block]
//
//     -- a factor of EIGHT at the large-block end, where the 50 ms floor is not
//     what binds, and far more than that at the small-block end where it is. At
//     4096 frames and 44.1 kHz that is a 92.9 ms block against 4 x 185.8 ms of
//     protection.
//
// THE CLOCK IS NOT IDENTITY. The interval reads steady_clock; nothing that
// reaches contentHash() ever does. Obligation 6.
// ---------------------------------------------------------------------------

// The ring size and the interval formula are wt::pen's, not this file's: the
// editor computes the same interval to decide when to offer a preview, so the
// number lives in the header both sides read. Aliased here so the arena's own
// code reads as it always would.
constexpr int kSpPreviewRing = wt::pen::kPreviewRing;

struct SpPreviewArena {
    std::vector<f32> ring[kSpPreviewRing];
    int              next     = 0;         // the buffer the NEXT publish writes
    f64              interval = 0.05;      // seconds; wt::pen::previewInterval
    f64              lastAt   = -1e18;     // steady_clock seconds of the last
                                           // publish; -1e18 = never
    u64              publishes = 0;        // diagnostics and the tests' bound
};

// A function-local static, so the arenas are a root for the leak checker, and
// null until an oscillator actually previews: a process that never draws pays
// nothing, exactly as spBuiltStore() does.
std::unique_ptr<SpPreviewArena>* spPreviewArenas() {
    static std::unique_ptr<SpPreviewArena> a[kSpMaxOsc];
    return a;
}

f64 spNowSeconds() {
    using namespace std::chrono;
    return duration_cast<duration<f64>>(steady_clock::now().time_since_epoch()).count();
}

// GUI thread. Tell an oscillator what prepare() was given, so that the rate
// limit is computed from the real block period rather than from a guess. Safe
// to call on every prepare(); it only writes the interval.
[[maybe_unused]] void spPreviewClock(int osc, f64 sampleRate, int maxBlock) {
    if ((unsigned)osc >= (unsigned)kSpMaxOsc) return;
    std::unique_ptr<SpPreviewArena>& a = spPreviewArenas()[osc];
    const f64 iv = wt::pen::previewInterval(sampleRate, maxBlock);
    if (!a) { a = std::make_unique<SpPreviewArena>(); a->interval = iv; return; }
    a->interval = iv;
}

// GUI thread. Build and publish a PREVIEW from 32 * wt::kCycle frames.
//
// False, and NOTHING published, when: the handle is out of range, the frames
// are null, any sample is non-finite, or the caller is inside the minimum
// preview interval. The rate limit is enforced HERE and not by the caller,
// because the contract says "rate-limited by the contract, not by the caller"
// and a bound a caller can forget is not a bound.
[[maybe_unused]] bool spPreviewFrames(int osc, const f32* frames) {
    if ((unsigned)osc >= (unsigned)kSpMaxOsc || !frames) return false;
    const size_t n = (size_t)kSpFrames * (size_t)wt::kCycle;
    for (size_t i = 0; i < n; ++i)
        if (!std::isfinite(frames[i])) return false;

    std::unique_ptr<SpPreviewArena>& a = spPreviewArenas()[osc];
    if (!a) a = std::make_unique<SpPreviewArena>();

    const f64 now = spNowSeconds();
    if (now - a->lastAt < a->interval) return false;      // inside the interval

    std::vector<f32>& buf = a->ring[a->next];
    if (buf.empty()) buf.assign((size_t)kSpFrames * (size_t)kSpStride, 0.f);
    spBuildMipsInto(frames, kSpFrames, buf.data());

    // The SAME release store spResolveCustom() uses, for the same reason: this
    // publishes 1.31 MB of table written before it, not a value. The audio
    // thread's read is unchanged and does not know a preview from a table.
    gSpCustomBase[osc].store(buf.data(), std::memory_order_release);
    a->next = (a->next + 1) % kSpPreviewRing;
    a->lastAt = now;
    ++a->publishes;
    return true;
}

// GUI thread. Drop any preview and republish the COMMITTED table -- which is
// gSpOsc[osc].base, or null when this oscillator has nothing committed, and
// null is the right answer there because the refusal contract renders factory
// table 0 over it. Idempotent: calling it twice publishes the same pointer
// twice, and calling it with no preview outstanding is the same store.
[[maybe_unused]] void spCancelPreview(int osc) {
    if ((unsigned)osc >= (unsigned)kSpMaxOsc) return;
    gSpCustomBase[osc].store(gSpOsc[osc].base, std::memory_order_release);
    // The arena is NOT freed and the ring position is NOT reset. Freeing it
    // would break the lifetime invariant (a block in flight may hold one of
    // these pointers); resetting `next` would let the next preview rewrite the
    // buffer the audio thread is most likely to still be reading.
}

// Diagnostics for the tests: how many buffers this oscillator's arena has
// actually allocated, and how many previews it has published. Zero when the
// oscillator has never previewed.
[[maybe_unused]] int spPreviewBuffers(int osc) {
    if ((unsigned)osc >= (unsigned)kSpMaxOsc) return 0;
    const std::unique_ptr<SpPreviewArena>& a = spPreviewArenas()[osc];
    if (!a) return 0;
    int n = 0;
    for (int i = 0; i < kSpPreviewRing; ++i) if (!a->ring[i].empty()) ++n;
    return n;
}
[[maybe_unused]] u64 spPreviewCount(int osc) {
    if ((unsigned)osc >= (unsigned)kSpMaxOsc) return 0;
    const std::unique_ptr<SpPreviewArena>& a = spPreviewArenas()[osc];
    return a ? a->publishes : 0;
}

// ---------------------------------------------------------------------------
// v5 — READ, and COMMIT
// ---------------------------------------------------------------------------

// GUI thread. Copies this oscillator's RESOLVED table into `out` as
// kSpFrames * wt::kCycle floats, frame-major, STRETCHED to 32 frames by the
// same linear frame-axis interpolation spBuildMipsInto() performs when the
// resolved table has fewer. False, and `out` untouched, when this oscillator
// has no resolved custom table.
//
// The editor always edits 32 frames; a one-frame import opens as 32 identical
// ones, which is what a table with no frame axis is. Committing then yields a
// DIFFERENT HASH from the original, and that is correct rather than a bug to be
// hidden: the content differs, and identity is content.
[[maybe_unused]] bool spReadFrames(int osc, f32* out) {
    if ((unsigned)osc >= (unsigned)kSpMaxOsc || !out) return false;
    const SpOscRec& r = gSpOsc[osc];
    if (r.hash == 0 || r.frames <= 0) return false;
    const wt::Table* t = wt::find(r.hash);
    if (!t || t->frames <= 0) return false;
    wt::pen::stretchFrames(t->data.data(), t->frames, out);
    return true;
}

// GUI thread. THE NINE-STEP COMMIT, in order, because the last step is the
// identity. Returns the new content hash, or 0 with a SENTENCE in `err` and
// NOTHING CHANGED on any refusal.
//
//   1..5  wt::pen::canonicalise(): finite, per-frame DC removal, set peak,
//         refuse silence, one set-wide gain. See wavetable_io.cpp.
//   6     wt::contentHash() over frames that are now in exactly the state v3's
//         rule names -- no slicing, no resampling, authored at 2048, DC-removed
//         and set-normalised. A drawn table and an imported table with the same
//         samples are THE SAME TABLE, which is correct and is not a coincidence.
//   7     wt::writeDrawn(): drawnDir()/<hex>.nxwt, atomically, skipping a file
//         that is already there.
//   8     wt::adopt(t, /*cache=*/false) -- ONE COPY IN ONE PLACE. Two copies of
//         an irreplaceable file under two policies is not redundancy, it is two
//         things to keep in sync. Then spBuildCustomMips(), then the release
//         store.
//   9     is the DEVICE's, not this file's: spectra.cpp rewrites wt/wtpath/
//         wtname, because this file owns what a table IS and that file owns what
//         the state SAYS. `outPath` is what it writes into wtpath.
//
// Step 7 before step 8 is deliberate: a table in the store whose file failed to
// write would be a table that resolves for this session and is gone at the next
// launch, which is precisely the loss rung 3 exists to prevent.
[[maybe_unused]] u64 spCommitFrames(int osc, const f32* frames, std::string& outPath,
                                    std::string& err) {
    err.clear();
    outPath.clear();
    if ((unsigned)osc >= (unsigned)kSpMaxOsc) {
        err = "this instance has no oscillator slot for a custom table";
        return 0;
    }
    if (!frames) { err = "there is nothing to commit"; return 0; }

    wt::Table t;
    t.frames    = kSpFrames;
    t.rule      = wt::Rule::Serum;      // authored at 2048; no rule was applied
    t.srcFrames = kSpFrames;
    t.srcCycle  = wt::kCycle;
    t.data.assign(frames, frames + (size_t)kSpFrames * (size_t)wt::kCycle);

    if (!wt::pen::canonicalise(t.data.data(), err)) return 0;

    t.hash = wt::contentHash(t.data.data(), kSpFrames);
    if (t.hash == 0) {
        err = "this table has a sample that is not a number; nothing was saved";
        return 0;
    }

    if (!wt::writeDrawn(t, outPath)) {
        err = "this drawing could not be saved to your wavetable library";
        return 0;
    }
    t.path = outPath;

    const wt::Table* stored = wt::adopt(t, /*cache=*/false);
    if (!stored) {
        err = "this process already holds as many wavetables as it will hold";
        return 0;
    }
    const f32* base = spBuildCustomMips(*stored);
    if (!base) {
        err = "this process has already built as many custom wavetables as it will build";
        return 0;
    }

    SpOscRec& r = gSpOsc[osc];
    r.hash   = stored->hash;
    r.path   = outPath;
    r.name   = spBaseName(outPath);
    r.frames = stored->frames;
    r.base   = base;
    gSpCustomBase[osc].store(base, std::memory_order_release);
    LOGI("spectra: saved a drawn wavetable to %s", outPath.c_str());
    return stored->hash;
}

// ---------------------------------------------------------------------------
// Oscillator handles, and why `osc` is one
//
// The seam's `osc` is an OSCILLATOR HANDLE and not the 0/1 index of an
// oscillator within an instance, because a custom table belongs to ONE
// oscillator of ONE device: two Spectras in a set import two different files
// onto their A oscillators, and a bare index would make the second overwrite
// the first. An instance takes a pair at construction and gives them back at
// destruction.
//
// Handles 0 and 1 exist and are ordinary, so a caller that passes a bare index
// gets defined behaviour (one shared pair) rather than an out-of-range read;
// allocation starts at 2 so that it never collides with such a caller.
// ---------------------------------------------------------------------------

[[maybe_unused]] int spAcquireOsc() {
    for (int i = 2; i < kSpMaxOsc; ++i)
        if (!gSpOsc[i].used) { gSpOsc[i].used = true; return i; }
    // Past the cap an instance simply has no custom slot: spTableBase's factory
    // arm ignores `osc`, so every one of the eight factory tables still plays.
    LOGW("spectra: no free oscillator handle; this instance plays factory "
         "tables only");
    return -1;
}

[[maybe_unused]] void spReleaseOsc(int osc) {
    if ((unsigned)osc < 2u || (unsigned)osc >= (unsigned)kSpMaxOsc) return;
    spClearCustom(osc);
    gSpOsc[osc].used = false;
}

// ---------------------------------------------------------------------------
// WHERE THE HOST CONTRACT'S FACE LIVES, AND WHY IT IS NOT HERE
//
// `WavetableControl` (src/plugin/host.h) is implemented in spectra.cpp, as a
// nested class of the device, because two of its six methods have to touch the
// DEVICE's state records and not just this file's: importing a table has to
// make the device's stateString() start naming it, and clearing one has to make
// it stop. spectra.cpp's own note at "THE OVERRIDE POINT" spells the handshake
// out — spImportWavetable() here, adoptCustom() there — and the split is the
// same one the whole seam is: this file owns what a table IS, that file owns
// what the state SAYS.
//
// The `wt3-filed-spectra.cpp.diff` in this wave's scratchpad carries the exact
// override text.
// ---------------------------------------------------------------------------
