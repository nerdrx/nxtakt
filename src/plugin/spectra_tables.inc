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
