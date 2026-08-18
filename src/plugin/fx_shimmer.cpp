// Shimmer — NxTakt's pitch-shifting reverb.
//
// A feedback delay network with a granular pitch shifter INSIDE the loop, so
// every traversal of the tank arrives an interval higher than the last. That is
// the whole product: a pad played into it grows a choir above itself that was
// never played.
//
// ---------------------------------------------------------------------------
// THE PARAMETER IDS ARE FROZEN. See kShParam* below. Ids are indices and a
// saved set stores them, so nothing here may be reordered, renamed away or
// removed — the same rule Spectra's header states, for the same reason.
// ---------------------------------------------------------------------------
//
// HOW IT IS BUILT, in the order the signal travels
//
//   in -> mono sum -> pre-delay -> input LP -> 4 Schroeder allpasses
//      -> FDN: 4 delay lines, unequal lengths scaled by Size
//              each line: damping LP, low-cut HP, modulated fractional read
//              feedback mixed by a 4x4 Hadamard, scaled per line for RT60
//      -> the loop's u-projection (0.5 * sum of the four) feeds the SHIFTER,
//         whose output is injected back along the same direction
//      -> taps -> width -> dry/wet
//
// ---------------------------------------------------------------------------
// WHY AN FDN AND NOT THE REVERB'S TANK
//
// The stock Reverb is a Dattorro figure-of-eight: one loop, one decay gain, two
// halves that feed each other. It is the right shape for a plate and the wrong
// shape for this, for two reasons that are both about the shifter.
//
//   * A shifter in the loop must be fed something whose ENERGY can be bounded.
//     An FDN's state is a vector and its feedback matrix is a matrix, so
//     "inject the shifted signal without adding energy" is a statement about
//     operator norms that can be proved rather than tuned. The proof is below
//     and it is the whole of the stability gate.
//   * Four unequal lines give four different traversal times, so the climb is a
//     smear rather than a staircase. One loop would produce an audible ladder
//     of discrete octaves, which is a novelty; the smear is the sound people
//     buy.
//
// The Hadamard is the mixing matrix rather than a Householder because a
// Householder (I - 2/N * J) is diagonal in the ones/orthogonal split — it
// negates the ones direction and leaves everything else alone, so it does not
// actually MIX and relies entirely on the unequal delays to do that job. The
// 4x4 Hadamard/2 is orthogonal AND dense: every line feeds every line.
//
// ---------------------------------------------------------------------------
// STABILITY, which is a proof and not a taste setting
//
// Write the four line outputs after damping and low cut as the vector s. One
// sample of feedback is
//
//     fb = G * ( (1-a) * H s  +  a * u (u . s)' )
//
// where H is the orthogonal Hadamard, u = (1/2, 1/2, 1/2, 1/2) is a UNIT
// vector, a is Shift Amount, ' is the pitch shifter and G is the diagonal of
// per-line decay gains. Three facts:
//
//   1. ||H s|| = ||s||, exactly: H is orthogonal.
//   2. ||u (u.s)'|| = |(u.s)'|, since ||u|| = 1; and the shifter's two-grain
//      overlap-add uses an EQUAL-POWER window (sin and cos of the same angle,
//      so the squares sum to exactly one), which cannot raise the power of what
//      it is given.
//   3. the crossfade between them is LINEAR, so the norm of the sum is at most
//      (1-a) + a = 1.
//
// Therefore ||fb|| <= max(G) * ||s||, and max(G) < 1 for every reachable Decay.
// The loop is a contraction at every setting, which is what "no runaway" means.
//
// The equal-power window can still put the two grains momentarily in phase and
// hand back a PEAK up to sqrt(2) of what it was given, which the norm argument
// above does not see. That is what the soft clipper on every line input is for:
// kShClip bounds the contents of the delay lines ABSOLUTELY, so the output bound
// is arithmetic rather than statistical. It is also the reason a hard-driven
// Shimmer blooms and compresses instead of screaming, which is the sound of
// every hardware unit this device is descended from.
//
// LOW CUT is not a tone control. A reverb loop with a pitch shifter in it is a
// positive feedback path for anything the shifter cannot move — DC and the
// bottom octave, where a +12 shift of 20 Hz is still 40 Hz and still in the
// loop. Without the high-pass the tank accumulates a subsonic pedal that eats
// the headroom the clipper was defending. 120 Hz is the default because that is
// where it stops being audible as a filter and starts being audible as clarity.
//
// ---------------------------------------------------------------------------
// THE SHIFTER
//
// Two-grain overlap-add on a delay line, which is the technique the engine's
// granular warp uses (src/audio/engine.cpp) with two changes:
//
//   * the window is EQUAL POWER, not equal gain. The warp crossfades two reads
//     of the SAME material a grain apart, which are correlated, so a linear
//     crossfade is right. Here the two grains are at unrelated phases of a
//     transposed read, so the linear crossfade would notch every time they
//     opposed. sin(pi*p) and cos(pi*p) sum in POWER to exactly one.
//   * the grain phase is the state, not a sample counter. p advances by
//     (ratio - 1) / grain per sample, both read delays are affine in p, and the
//     wrap is a floor() — so the whole shifter is a function of absolute sample
//     time and survives any block size bit-exactly.
//
// Grain length is 50 ms. Shorter warbles, longer smears; 50 ms is where a +12
// shift of a sustained note stops sounding like a robot and the pre-echo is
// still shorter than the tank's own first reflection.
//
// ---------------------------------------------------------------------------
// WHAT AMOUNT REALLY TRADES
//
// Shift Amount is the crossfade coefficient a above, so it is literally "how
// much of the feedback passes through the shifter" — and because the crossfade
// is linear (it has to be, see the proof), a = 1 leaves NO unshifted path. The
// fundamental then leaves the tank in one traversal and the tail is a rocket:
// that is correct behaviour, it is what 100% means, and it is not the setting
// anyone mixes with. Everything between 0.15 and 0.4 is the product.
#ifndef LAT_FX_SHIMMER_IN_INTERNAL_DEVICES

// Compiled standalone: an empty translation unit, exactly as spectra.cpp is and
// for the identical reason (the GUI Makefile sweeps src/**/*.cpp; every tool and
// test recipe lists its sources one by one). The guard and the #include in
// internal_devices.cpp go together the day the recipes name this file.
namespace lat { namespace detail { /* see internal_devices.cpp */ } }

#else

#include "host.h"
#include "internal_base.h"
#include "internal_dsp.h"

#include <cmath>

namespace lat {
namespace detail {
namespace {

// ---------------------------------------------------------------------------
// FROZEN PARAMETER IDS
//
// An id IS an index into the instance's parameter array and a saved set stores
// it, so this table is append-only: a value may never be reused for a different
// knob and an entry may never be deleted. The constructor below adds them in
// exactly this order; the suite looks every parameter up BY NAME and fails
// loudly on a miss, which is what catches a reorder that this comment alone
// could not.
// ---------------------------------------------------------------------------
enum : int {
    kShDecay   = 0,   // s      RT60 of the unshifted tank
    kShSize    = 1,   //        delay-line length scale
    kShPre     = 2,   // ms     pre-delay
    kShDiff    = 3,   //        input diffusion
    kShShift   = 4,   //        0 off, 1 +5, 2 +7, 3 +12, 4 +19
    kShAmount  = 5,   //        crossfade: how much feedback is shifted
    kShDamp    = 6,   // Hz     in-loop high cut
    kShLowCut  = 7,   // Hz     in-loop low cut
    kShModRate = 8,   // Hz     tank chorusing rate
    kShModDep  = 9,   //        tank chorusing depth
    kShWidth   = 10,  //        stereo width of the wet signal
    kShMix     = 11,  //        dry/wet
};
constexpr int kShParamCount = 12;

constexpr int kShLines = 4;
constexpr f32 kShPi    = 3.14159265358979f;

// THE OUTPUT BOUND, as two numbers whose PRODUCT is the whole of it.
//
// kShClip is what a delay line may absolutely contain; the wet output is a
// convex combination of two line reads (the taps average two lines, and the
// width matrix is a convex combination of those), so the wet signal is bounded
// by kShClip and the OUTPUT is bounded by kShClip * kShOutWet = 1.375, which is
// +2.77 dBFS. That is the number the stability gate checks against, and it is
// arithmetic rather than a measurement that happened to come out low.
//
// The split between the two is a gain-staging decision and not a free one. The
// tank loses a great deal of what is put into it -- the damping filter, the low
// cut and every pass through the shifter all remove energy -- so a wet path
// that ran at unity inside would come out 18 dB under the dry signal and be a
// reverb people turn up and then turn back down. Running the tank LOW and the
// output HIGH buys that level back without moving the bound: the clipper
// engages a little earlier on a hot input, which is where the bloom comes from.
constexpr f32 kShClip   = 0.55f;
constexpr f32 kShOutWet = 2.5f;

// Grain length of the in-loop shifter, in seconds. See the header.
constexpr f64 kShGrainSec = 0.050;

// Line lengths in milliseconds at Size = 0.5, deliberately not in any small
// integer ratio: equal or near-equal lengths make an FDN ring on the comb its
// lines share.
constexpr f32 kShBaseMs[kShLines] = { 41.3f, 53.7f, 67.1f, 79.3f };

// Input injection signs. Not all +1: an input that lands entirely on the ones
// direction is an input the shifter's projection sees at full strength before
// the tank has diffused it, which reads as a hard octave rather than a bloom.
constexpr f32 kShInSign[kShLines] = { 1.f, 1.f, -1.f, 1.f };

// Pre-diffusion allpass lengths, in samples at 29761 Hz (the Dattorro paper's
// rate), scaled at prepare() exactly as the stock Reverb scales its own.
constexpr int kShRefDif[4] = { 113, 162, 241, 399 };

// Shift ratios, indexed by the Shift parameter. Frozen with the parameter: the
// INDEX is what a project file stores.
constexpr f32 kShRatio[5] = {
    1.f,                        // off
    1.33483985f,                // +5   2^(5/12)
    1.49830708f,                // +7   2^(7/12)
    2.f,                        // +12
    2.99661407f,                // +19  octave + fifth
};

// Anything below this is inaudible and on its way to being a denormal. The tank
// is flushed at every push, which is what lets the tail reach TRUE zero rather
// than an asymptote the FPU spends hundreds of cycles a sample maintaining.
using dsp::flushDenormal;

// ---------------------------------------------------------------------------
// Two-grain equal-power pitch shifter
//
// REALTIME apart from prepare(). One delay line, one phase, no allocation in
// process(). See the header for why the window is sin/cos and not the warp
// engine's raised cosine.
// ---------------------------------------------------------------------------
struct ShShifter {
    dsp::DelayLine line;
    f32 ph    = 0.f;      // grain phase, cycles, [0,1)
    f32 grain = 16.f;     // grain length in samples
    f32 base  = 4.f;      // minimum read delay: keeps the loop delay-free-path free

    // GUI thread.
    void prepare(f64 sr) {
        grain = (f32)(kShGrainSec * sr);
        if (!(grain > 16.f)) grain = 16.f;
        base  = 4.f;
        line.resize((int)(grain + base) + 16);
        reset();
    }
    void reset() { line.reset(); ph = 0.f; }
    void check() { if (!dsp::sane(ph)) ph = 0.f; }

    // REALTIME. `inc` is (ratio - 1) / grain, precomputed once per block.
    inline f32 process(f32 x, f32 inc, bool bypass) {
        line.push(flushDenormal(x));
        if (bypass) return line.tap(1);          // exactly x, one push old
        ph += inc;
        ph -= std::floor(ph);
        if (!(ph >= 0.f && ph < 1.f)) ph = 0.f;  // a bad ratio cannot stick
        const f32 pb = (ph >= 0.5f) ? (ph - 0.5f) : (ph + 0.5f);
        const f32 dA = base + (1.f - ph) * grain;
        const f32 dB = base + (1.f - pb) * grain;
        // sin(pi*ph) and sin(pi*ph + pi/2): both non-negative on [0,1), and
        // their squares sum to exactly one. That identity IS the energy bound
        // the file header's proof leans on.
        const f32 wA = std::sin(kShPi * ph);
        const f32 wB = std::cos(kShPi * ph);
        return wA * line.tapLerp(dA) + std::fabs(wB) * line.tapLerp(dB);
    }
};

// ---------------------------------------------------------------------------
// Shimmer
// ---------------------------------------------------------------------------
class Shimmer final : public InternalInstance {
public:
    explicit Shimmer(const PluginDesc& d) : InternalInstance(d) {
        addParam   ("Decay",       "s",  0.3f,   30.f,    4.f,    true);
        addParam   ("Size",        "",   0.f,    1.f,     0.5f);
        addParam   ("Pre-Delay",   "ms", 0.f,    250.f,   0.f);
        addParam   ("Diffusion",   "",   0.f,    1.f,     0.75f);
        addIntParam("Shift",              0,     4,       3);
        addParam   ("Shift Amount","",   0.f,    1.f,     0.35f);
        addParam   ("Damping",     "Hz", 1000.f, 18000.f, 7000.f, true);
        addParam   ("Low Cut",     "Hz", 20.f,   1000.f,  120.f,  true);
        addParam   ("Mod Rate",    "Hz", 0.05f,  5.f,     0.8f,   true);
        addParam   ("Mod Depth",   "",   0.f,    1.f,     0.35f);
        addParam   ("Width",       "",   0.f,    1.f,     1.f);
        addParam   ("Dry/Wet",     "",   0.f,    1.f,     0.35f);
    }

    // GUI thread. The only allocation in the device.
    bool prepare(f64 sampleRate, int maxBlock) override {
        sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxBlock_ = maxBlock > 0 ? maxBlock : kMaxBlock;

        // Every line is sized for the LONGEST it can ever be asked to be
        // (Size = 1) plus the modulation excursion, so a Size sweep while
        // playing only moves a read pointer.
        for (int i = 0; i < kShLines; ++i) {
            const f32 maxMs = kShBaseMs[i] * kShSizeMax;
            maxLen_[i] = (int)(maxMs * 1e-3f * (f32)sr_) + 1;
            line_[i].resize(maxLen_[i] + (int)kShModMax + 8);
        }
        modMax_ = (f32)kShModMax;

        const f64 k = sr_ / 29761.0;
        for (int i = 0; i < 4; ++i) {
            lenDif_[i] = (int)(kShRefDif[i] * k + 0.5);
            if (lenDif_[i] < 1) lenDif_[i] = 1;
            dif_[i].resize(lenDif_[i] + 4);
        }

        pre_.resize((int)(0.25 * sr_) + 8);
        shift_.prepare(sr_);

        inLp_.setCutoff(sr_, (f32)std::fmin(16000.0, sr_ * 0.45));

        preT_.setTime(sr_, 0.08f);
        preT_.snap(preDelay());
        amt_.setTime(sr_, 0.05f);
        amt_.snap(clampv(p(kShAmount), 0.f, 1.f));
        width_.setTime(sr_, 0.02f);
        width_.snap(clampv(p(kShWidth), 0.f, 1.f));
        mix_.setTime(sr_, 0.02f);
        mix_.snap(clampv(p(kShMix), 0.f, 1.f));
        for (int i = 0; i < kShLines; ++i) {
            len_[i].setTime(sr_, 0.15f);        // a Size move bends, never clicks
            len_[i].snap(lineLen(i));
            g_[i].setTime(sr_, 0.10f);
            g_[i].snap(decayGain(i));
        }
        lfo_.reset();
        first_ = true;
        clear();
        return true;
    }

    int presetCount() const override { return kShPresetCount; }

    const char* presetName(int i) const override {
        return (i >= 0 && i < kShPresetCount) ? kShPresets[i].name : nullptr;
    }

    // GUI thread. Writes through setParam and does nothing else, so the whole
    // program sees a preset as a handful of ordinary knob moves.
    void loadPreset(int i) override {
        if (i < 0 || i >= kShPresetCount) return;
        for (int k = 0; k < n_; ++k) setParam(k, info_[(size_t)k].def);
        const ShPreset& pr = kShPresets[i];
        for (int k = 0; k < pr.n; ++k) setParam(pr.set[k].id, pr.set[k].v);
    }

    // REALTIME. No allocation, no locks, no block-sized scratch — which is why
    // there is no nframes > maxBlock passthrough here: the device processes any
    // length it is handed, and the block-invariance gate at 1024 frames is what
    // says that is true rather than merely intended.
    void process(const f32* const* in, f32* const* out, int channels, int nframes) override {
        if (channels <= 0 || nframes <= 0 || !out) return;
        // "No input" is not "nothing to do": the tank still has to ring out.
        if (isBypassed() || pre_.buf.empty()) { passthrough(in, out, channels, nframes); return; }

        const f32 dampHz = clampv(p(kShDamp), 1000.f, 18000.f);
        const f32 cutHz  = clampv(p(kShLowCut), 20.f, 1000.f);
        for (int i = 0; i < kShLines; ++i) {
            damp_[i].setCutoff(sr_, dampHz);
            cut_[i].setCutoff(sr_, cutHz);
            len_[i].set(lineLen(i));
            g_[i].set(decayGain(i));
        }
        preT_.set(preDelay());
        amt_.set(clampv(p(kShAmount), 0.f, 1.f));
        width_.set(clampv(p(kShWidth), 0.f, 1.f));
        mix_.set(clampv(p(kShMix), 0.f, 1.f));
        if (first_) {
            preT_.settle(); amt_.settle(); width_.settle(); mix_.settle();
            for (int i = 0; i < kShLines; ++i) { len_[i].settle(); g_[i].settle(); }
            first_ = false;
        }

        const int  sel      = (int)clampv(p(kShShift) + 0.5f, 0.f, 4.f);
        const f32  ratio    = kShRatio[sel];
        const bool noShift  = (sel == 0);
        const f32  shInc    = (ratio - 1.f) / shift_.grain;

        const f32 diff = clampv(p(kShDiff), 0.f, 1.f);
        const f32 g1   = 0.75f  * (0.30f + 0.70f * diff);
        const f32 g2   = 0.625f * (0.30f + 0.70f * diff);

        lfo_.setRate(sr_, clampv(p(kShModRate), 0.05f, 5.f));
        const f32 modDepth = clampv(p(kShModDep), 0.f, 1.f) * modMax_;

        const int nc = channels < kCh ? channels : kCh;
        const f32* src[kCh] = { nullptr, nullptr };
        f32*       dst[kCh] = { nullptr, nullptr };
        for (int c = 0; c < nc; ++c) { src[c] = in ? in[c] : nullptr; dst[c] = out[c]; }

        for (int i = 0; i < nframes; ++i) {
            const f32 xl = src[0] ? src[0][i] : 0.f;
            const f32 xr = (nc > 1 && src[1]) ? src[1][i] : xl;

            // --- input chain -----------------------------------------------
            const f32 mono = 0.5f * (xl + xr);
            const f32 xp = pre_.tapLerp(preT_.next());
            pre_.push(flushDenormal(mono));

            f32 v = inLp_.process(xp);
            v = dsp::allpassTick(dif_[0], (f32)lenDif_[0], g1, v);
            v = dsp::allpassTick(dif_[1], (f32)lenDif_[1], g1, v);
            v = dsp::allpassTick(dif_[2], (f32)lenDif_[2], g2, v);
            v = dsp::allpassTick(dif_[3], (f32)lenDif_[3], g2, v);
            v *= kShInGain;

            // --- read the tank, one modulated fractional tap per line ------
            // The LFO ticks once per SAMPLE and is read at four fixed phase
            // offsets: one oscillator, four angles, and no way for the four to
            // drift apart or to depend on where a block boundary fell.
            lfo_.tick();
            f32 d[kShLines];
            for (int k = 0; k < kShLines; ++k) {
                const f32 m = dsp::Lfo::shape(0, lfo_.phase + kShModPhase[k]);
                f32 dl = len_[k].next() + modDepth * m;
                dl = clampv(dl, 4.f, (f32)maxLen_[k] + modMax_);
                d[k] = line_[k].tapLerp(dl);
            }

            // --- per-line tone: damp (high cut) then low cut ---------------
            f32 s[kShLines];
            for (int k = 0; k < kShLines; ++k) {
                const f32 lo = damp_[k].process(d[k]);
                s[k] = lo - cut_[k].process(lo);       // 6 dB/oct high-pass
            }

            // --- 4x4 Hadamard / 2: orthogonal and dense --------------------
            const f32 ha = s[0] + s[1], hb = s[2] + s[3];
            const f32 hc = s[0] - s[1], hd = s[2] - s[3];
            f32 m[kShLines];
            m[0] = 0.5f * (ha + hb);
            m[1] = 0.5f * (hc + hd);
            m[2] = 0.5f * (ha - hb);
            m[3] = 0.5f * (hc - hd);

            // --- the shifter, on the loop's unit ones-direction ------------
            // m[0] IS 0.5*(s0+s1+s2+s3) = <s, u> with u the unit vector
            // (1/2,1/2,1/2,1/2). Injecting 0.5*sh back into every line puts the
            // result on u again, so the shifted branch is a rank-one projection
            // and its operator norm is the shifter's own gain. See the header.
            const f32 sh = shift_.process(m[0], shInc, noShift);

            const f32 a  = amt_.next();
            const f32 ua = 1.f - a;

            for (int k = 0; k < kShLines; ++k) {
                const f32 fb = g_[k].next() * (ua * m[k] + a * 0.5f * sh);
                line_[k].push(flushDenormal(softClip(v * kShInSign[k] + fb)));
            }

            // --- output ----------------------------------------------------
            const f32 yl = kShOutWet * 0.5f * (d[0] + d[2]);
            const f32 yr = kShOutWet * 0.5f * (d[1] + d[3]);
            const f32 w   = width_.next();
            const f32 mid = 0.5f * (yl + yr);
            const f32 sid = 0.5f * (yl - yr) * w;
            const f32 mx  = mix_.next();

            if (dst[0]) dst[0][i] = xl * (1.f - mx) + (mid + sid) * mx;
            if (nc > 1 && dst[1]) dst[1][i] = xr * (1.f - mx) + (mid - sid) * mx;
        }

        // Once per block: a recursive structure that has gone non-finite never
        // recovers on its own, so the check is the difference between one bad
        // block and a dead device.
        inLp_.check();
        shift_.check();
        bool bad = false;
        for (int k = 0; k < kShLines; ++k) {
            damp_[k].check();
            cut_[k].check();
            if (!dsp::sane(line_[k].buf[line_[k].w])) bad = true;
        }
        if (bad) clear();

        copyExtra(in, out, nc, channels, nframes);
    }

private:
    static constexpr int kCh = 2;
    // Size = 1 makes every line 2.5x its nominal length; Size = 0 makes it
    // 0.3x. The buffers are sized for the top of that range once, at prepare().
    static constexpr f32 kShSizeMax = 2.5f;
    static constexpr f32 kShSizeMin = 0.30f;
    // Modulation excursion in samples. Small on purpose: this is chorusing
    // inside the tank, which wants to be felt and not heard. At 48 kHz, 24
    // samples is half a millisecond.
    static constexpr int kShModMax = 24;
    // Input trim into the tank. Measured, not guessed: the level check in the
    // suite renders white noise fully wet at the defaults and reports where the
    // tail sits against the dry signal. White noise is the worst case for this
    // device — a 7 kHz damping filter takes most of it — so a few dB under dry
    // there is a wet path that sits right on musical material.
    static constexpr f32 kShInGain = 0.45f;

    // Four fixed phase offsets on one LFO. Irrational-ish spacing so the four
    // lines never sweep together, which is what a single shared modulator would
    // otherwise sound like.
    static constexpr f32 kShModPhase[kShLines] = { 0.f, 0.29f, 0.53f, 0.77f };

    // The absolute bound the stability proof rests on. Algebraic sigmoid rather
    // than tanh: strictly bounded by kShClip, monotone, odd, and one sqrt
    // instead of two exponentials.
    static inline f32 softClip(f32 x) {
        const f32 a = x * (1.f / kShClip);
        return kShClip * a / std::sqrt(1.f + a * a);
    }

    // Pre-delay in samples, and the ONE is not decoration.
    //
    // The tap convention is `read then push`, so tapLerp(d) is d samples of
    // delay -- for d >= 1. At d = 0 the read lands on the write position, which
    // holds the sample from a WHOLE BUFFER ago: the pre-delay line is a quarter
    // of a second long and rounded up to a power of two, so Pre-Delay at zero
    // would silently mean 341 ms at 48 kHz. That is not a rounding error, it is
    // the parameter meaning its own maximum, and the tail gate is what found
    // it: a tank fed through a third of a second of hidden delay was still
    // ringing after the test had decided it was silent.
    f32 preDelay() const {
        const f32 n = clampv(p(kShPre), 0.f, 250.f) * 1e-3f * (f32)sr_;
        return clampv(n, 1.f, (f32)pre_.capacity() - 2.f);
    }

    // Line length in samples for the current Size. Smoothed, so a Size sweep is
    // a tape-style bend rather than a click.
    f32 lineLen(int i) const {
        const f32 sz = clampv(p(kShSize), 0.f, 1.f);
        const f32 sc = kShSizeMin + (kShSizeMax - kShSizeMin) * sz;
        const f32 n  = kShBaseMs[i] * sc * 1e-3f * (f32)sr_;
        return clampv(n, 8.f, (f32)maxLen_[i]);
    }

    // The FDN decay law: a line of T seconds must lose 60 dB in RT60 seconds,
    // so its per-traversal gain is 10^(-3*T/RT60). Per LINE, not one gain for
    // the whole tank -- that is the difference between four lines that decay
    // together and four lines that decay at four different rates and turn a
    // tail into a flutter.
    f32 decayGain(int i) const {
        const f32 rt = clampv(p(kShDecay), 0.3f, 30.f);
        const f32 t  = lineLen(i) / (f32)sr_;
        return clampv(std::pow(10.f, -3.f * t / rt), 0.f, 0.9995f);
    }

    void clear() {
        pre_.reset();
        for (auto& x : dif_) x.reset();
        for (int k = 0; k < kShLines; ++k) {
            line_[k].reset();
            damp_[k].reset();
            cut_[k].reset();
        }
        inLp_.reset();
        shift_.reset();
    }

    // --- factory presets ---------------------------------------------------
    // Same shape as Spectra's: a name and a list of (id, value). loadPreset()
    // resets to the defaults first, so a preset is COMPLETE however short it is
    // written and "Init" is exactly the constructor.
    struct ShPreset {
        const char* name;
        int         n;
        struct { int id; f32 v; } set[12];
    };
    static constexpr int kShPresetCount = 6;
    static const ShPreset kShPresets[kShPresetCount];

    dsp::DelayLine line_[kShLines], dif_[4], pre_;
    dsp::OnePole   damp_[kShLines], cut_[kShLines], inLp_;
    dsp::Smoother  len_[kShLines], g_[kShLines], preT_, amt_, width_, mix_;
    dsp::Lfo       lfo_;
    ShShifter      shift_;

    int  lenDif_[4] = { 1, 1, 1, 1 };
    int  maxLen_[kShLines] = { 1, 1, 1, 1 };
    f32  modMax_ = 0.f;
    bool first_ = true;
};

constexpr f32 Shimmer::kShModPhase[kShLines];

const Shimmer::ShPreset Shimmer::kShPresets[Shimmer::kShPresetCount] = {
    { "Init", 0, {} },

    // The one everybody tries first: a big room that grows an octave choir.
    { "Cathedral", 8, {
        { kShDecay, 9.f }, { kShSize, 0.85f }, { kShPre, 40.f },
        { kShShift, 3 }, { kShAmount, 0.32f },
        { kShDamp, 5500.f }, { kShLowCut, 140.f }, { kShMix, 0.45f },
    } },

    // Amount high enough that the climb is the sound rather than a colour.
    { "Ghost Choir", 9, {
        { kShDecay, 14.f }, { kShSize, 0.70f }, { kShPre, 90.f },
        { kShShift, 3 }, { kShAmount, 0.55f },
        { kShDamp, 4200.f }, { kShLowCut, 220.f },
        { kShModDep, 0.60f }, { kShMix, 0.55f },
    } },

    // A wash you can leave on a bus. Short, bright, mostly dry.
    { "Subtle Air", 8, {
        { kShDecay, 2.2f }, { kShSize, 0.35f }, { kShShift, 3 },
        { kShAmount, 0.14f }, { kShDamp, 9000.f }, { kShLowCut, 200.f },
        { kShModDep, 0.20f }, { kShMix, 0.18f },
    } },

    // The fifth instead of the octave: consonant enough to sit under a chord
    // without deciding what the chord is.
    { "Fifth Halo", 8, {
        { kShDecay, 6.f }, { kShSize, 0.6f }, { kShShift, 2 },
        { kShAmount, 0.28f }, { kShDamp, 7000.f }, { kShLowCut, 160.f },
        { kShModDep, 0.45f }, { kShMix, 0.40f },
    } },

    // Nineteen semitones, long, dark, slow modulation: the sound of a room that
    // is not in this building.
    { "Frozen Sky", 9, {
        { kShDecay, 24.f }, { kShSize, 1.f }, { kShPre, 140.f },
        { kShShift, 4 }, { kShAmount, 0.45f },
        { kShDamp, 3400.f }, { kShLowCut, 300.f },
        { kShModRate, 0.25f }, { kShMix, 0.60f },
    } },
};

constexpr const char* kShimmerUri = "nxtakt:shimmer";

PluginDesc shimmerDesc() {
    PluginDesc d;
    d.format     = PluginFormat::Internal;
    d.uri        = kShimmerUri;
    d.name       = "Shimmer";
    d.vendor     = "NxTakt";
    d.category   = "Reverb";
    d.kind       = PluginKind::Effect;
    d.audioIn    = 2;
    d.audioOut   = 2;
    d.hasMidiIn  = false;
    d.paramCount = kShParamCount;
    return d;
}

} // namespace
} // namespace detail
} // namespace lat

#endif // LAT_FX_SHIMMER_IN_INTERNAL_DEVICES
