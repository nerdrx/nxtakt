// Bloom — NxTakt's three-band upward and downward compressor.
//
// The OTT-class effect, and the reason people pay for it is the UPWARD half:
// downward compression makes a mix quieter where it is loud, which every
// compressor does; upward compression makes it LOUDER where it is quiet, which
// is what turns a dry loop into a wall. Three bands, both directions, one Depth
// knob that scales the whole thing.
//
// ---------------------------------------------------------------------------
// THE PARAMETER IDS ARE FROZEN. See kBlParam* below. Ids are indices and a
// saved set stores them; nothing here may be reordered or removed.
// ---------------------------------------------------------------------------
//
// ---------------------------------------------------------------------------
// THE CROSSOVER, which is the whole of whether a multiband is honest
//
// Three bands from two Linkwitz-Riley 4th-order splits, each LR4 being two
// cascaded Butterworth sections at Q = 1/sqrt(2):
//
//     low  = LP4(fLow)  -> AP2(fHigh)      <- the compensation, see below
//     rest = HP4(fLow)
//     mid  = LP4(fHigh, rest)
//     high = HP4(fHigh, rest)
//
// THE IDENTITY THE WHOLE THING RESTS ON is that an LR4 pair sums to a SECOND
// ORDER ALLPASS with the same corner and the same Q -- not to unity:
//
//     LP4(z) + HP4(z) = AP2(z)
//
// That is exact for the RBJ coefficient set, and it is worth writing down why,
// because "the crossover sums flat" is otherwise a claim rather than a fact.
// With c = cos(w), alpha = sin(w)/sqrt(2) and A(z) the shared denominator, the
// two numerators are k_L (1+z^-1)^2 and k_H (1-z^-1)^2 with k_L = (1-c)/2 and
// k_H = (1+c)/2, so the sum over A^2 has to equal A_rev/A, i.e.
//
//     k_L^2 (1+z^-1)^4 + k_H^2 (1-z^-1)^4  =  A(z) A_rev(z)
//
// Matching the five coefficients gives three conditions, and all three reduce
// to 2*alpha^2 = 1 - c^2, which is exactly what alpha = sin(w)/sqrt(2) says.
// The suite checks it by measurement anyway (the null test), because an
// algebraic identity that the code does not actually implement is worth
// nothing.
//
// The consequence is the AP2(fHigh) on the LOW band. Without it the low band
// arrives with the phase of one allpass and the mid and high bands with the
// phase of two, and the sum has a hole at the lower crossover that no amount of
// magnitude flatness hides. With it,
//
//     low + mid + high = AP2(fHigh) * AP2(fLow) * input
//
// exactly: flat magnitude everywhere, and a phase response that is a pure
// allpass. This is why the split-and-sum CANNOT null against the raw input --
// no IIR crossover can, and any device that claims it does is either
// linear-phase (and lying about its latency) or not measuring. The gate in the
// suite is the one that is actually true and actually strong: the sum nulls
// against the input passed through an INDEPENDENTLY WRITTEN pair of second
// order allpasses at the same two corners, below -80 dB, and its magnitude
// response is flat to a hundredth of a decibel.
//
// LATENCY IS ZERO and honestly so: nine biquad sections per channel, no
// lookahead, no window, no oversampling.
//
// ---------------------------------------------------------------------------
// THE DYNAMICS
//
// Per band, on a stereo-LINKED peak detector (two independent followers pull
// the stereo image around every time one side is louder -- the same reasoning
// the stock Compressor's detector states):
//
//     down: level above kBlThrDown is pushed back by the down ratio
//     up:   level below kBlThrUp   is pulled up  by the up ratio
//     gain = Depth * (down + up), in dB, then applied
//
// The two thresholds are FIXED and are not parameters. That is the OTT bargain:
// the thing is a preset with a Depth knob, and exposing four thresholds would
// make it a worse multiband compressor rather than a better one. They sit far
// enough apart (-15 and -25 dBFS per band) that ordinary material spends most
// of its time between them and is left alone.
//
// UPWARD COMPRESSION HAS TO HAVE A CEILING or it is a noise generator: an
// infinitely quiet band asks for infinite gain. kBlMaxBoost caps it at 24 dB,
// which is where a room tone stops being ambience and starts being a hiss.
//
// ATTACK AND RELEASE ARE SCALED PER BAND -- lows slower, highs faster. This is
// the OTT trick and it is not cosmetic: a 60 Hz cycle is 16 ms long, so a 5 ms
// attack on the low band tracks the WAVEFORM rather than the envelope and turns
// a bass note into a square wave. kBlTimeScale is the multiplier per band.
//
// ---------------------------------------------------------------------------
// DEPTH 0 IS A TRUE WIRE, and here is exactly how
//
// At Depth 0 with the three band gains, the input gain and the output gain all
// at 0 dB and Dry/Wet at 1, process() writes the INPUT to the output, sample
// for sample, bit for bit. Not "the crossover sums to something flat" -- the
// actual input floats.
//
// The crossover KEEPS RUNNING underneath that, and is not skipped. The cost is
// that Depth 0 is not a CPU bypass (device bypass is, and that is what bypass
// is for); the benefit is that automating Depth up off zero starts from filter
// state that has been tracking the signal all along, so the band split fades in
// instead of arriving with a hole in it. Given the choice between a free wire
// that clicks and a wire that costs nine biquads and does not, this is a device
// people will automate.
#ifndef LAT_FX_BLOOM_IN_INTERNAL_DEVICES

// Compiled standalone: an empty translation unit, exactly as spectra.cpp is and
// for the identical reason.
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
// FROZEN PARAMETER IDS. Append-only; ids are indices and saved sets store them.
// ---------------------------------------------------------------------------
enum : int {
    kBlLowSplit  = 0,   // Hz
    kBlHighSplit = 1,   // Hz
    kBlDepth     = 2,   //      scales both directions; 0 is a wire
    kBlUpRatio   = 3,
    kBlDownRatio = 4,
    kBlAttack    = 5,   // ms   before the per-band scale
    kBlRelease   = 6,   // ms   before the per-band scale
    kBlLowGain   = 7,   // dB
    kBlMidGain   = 8,   // dB
    kBlHighGain  = 9,   // dB
    kBlInput     = 10,  // dB
    kBlOutput    = 11,  // dB
    kBlMix       = 12,  //      dry/wet
};
constexpr int kBlParamCount = 13;

constexpr int kBlBands = 3;

// The two fixed thresholds, in dBFS per band. See the header for why they are
// not knobs.
constexpr f32 kBlThrDown = -15.f;
constexpr f32 kBlThrUp   = -25.f;

// The ceiling on upward gain. Without it a silent band asks for infinity.
constexpr f32 kBlMaxBoost = 24.f;

// Attack/release multipliers per band: low, mid, high. The OTT trick.
constexpr f32 kBlTimeScale[kBlBands] = { 2.5f, 1.f, 0.6f };

// Detector floor. -140 dBFS: below anything a 32-bit float mix contains, and
// the reason the level computation never sees a zero.
constexpr f32 kBlEnvFloor = 1e-7f;

using dsp::flushDenormal;

// --- RBJ sections this device needs and internal_dsp.h does not carry --------
// The shared header has peaking and the two shelves, because that is what EQ
// Three needed. A crossover needs the other three, and they are written here
// rather than added to the header for the same reason spectra.cpp keeps its own
// FFT: one device wanting them is not a reason to grow the shared surface.
//
// All three come from the SAME w and alpha, which is precisely what makes the
// LP4 + HP4 = AP2 identity in the header hold exactly rather than nearly.
inline dsp::BiquadCoeffs blLowpass(f64 sr, f32 hz, f32 q) {
    const f64 w = dsp::rbj::omega(sr, hz);
    const f64 c = std::cos(w), s = std::sin(w);
    const f64 al = s / (2.0 * (f64)clampv(q, 0.05f, 40.f));
    return dsp::rbj::normalise({ (1.0 - c) * 0.5, 1.0 - c, (1.0 - c) * 0.5,
                                 1.0 + al, -2.0 * c, 1.0 - al });
}

inline dsp::BiquadCoeffs blHighpass(f64 sr, f32 hz, f32 q) {
    const f64 w = dsp::rbj::omega(sr, hz);
    const f64 c = std::cos(w), s = std::sin(w);
    const f64 al = s / (2.0 * (f64)clampv(q, 0.05f, 40.f));
    return dsp::rbj::normalise({ (1.0 + c) * 0.5, -(1.0 + c), (1.0 + c) * 0.5,
                                 1.0 + al, -2.0 * c, 1.0 - al });
}

inline dsp::BiquadCoeffs blAllpass(f64 sr, f32 hz, f32 q) {
    const f64 w = dsp::rbj::omega(sr, hz);
    const f64 c = std::cos(w), s = std::sin(w);
    const f64 al = s / (2.0 * (f64)clampv(q, 0.05f, 40.f));
    return dsp::rbj::normalise({ 1.0 - al, -2.0 * c, 1.0 + al,
                                 1.0 + al, -2.0 * c, 1.0 - al });
}

// Exact at 0 dB, which is what lets "all gains at unity" be a BIT-EXACT wire
// rather than a multiply by 0.99999994.
inline f32 blDbToGain(f32 db) {
    return (db == 0.f) ? 1.f : std::pow(10.f, db * 0.05f);
}

// Glide one coefficient set one sample towards its target AND take the snap
// decision in the same breath.
//
// internal_dsp.h offers biquadSettle() as a once-per-BLOCK residue killer,
// which is right for a device that is not required to be bit-identical across
// block sizes and wrong for one that is. The snap fires the first time the
// remaining distance drops below 1e-9, so at 256 frames it fires up to 255
// samples later than at 1 frame, and the glide keeps nudging the coefficients
// by sub-1e-9 amounts in between -- which is a DIFFERENT number, and bit
// identity is the bar. Taking the decision per sample makes the coefficient
// trajectory a function of absolute sample time and nothing else.
inline void blGlideStep(dsp::BiquadCoeffs& c, const dsp::BiquadCoeffs& t, f32 g) {
    dsp::biquadGlide(c, t, g);
    dsp::biquadSettle(c, t);
}

// ---------------------------------------------------------------------------
// Bloom
// ---------------------------------------------------------------------------
class Bloom final : public InternalInstance {
public:
    explicit Bloom(const PluginDesc& d) : InternalInstance(d) {
        addParam("Low Split",  "Hz", 40.f,   1000.f,  250.f,  true);
        addParam("High Split", "Hz", 800.f,  14000.f, 2500.f, true);
        addParam("Depth",      "",   0.f,    1.f,     0.f);
        addParam("Up Ratio",   "",   1.f,    20.f,    4.f,    true);
        addParam("Down Ratio", "",   1.f,    20.f,    4.f,    true);
        addParam("Attack",     "ms", 0.1f,   200.f,   10.f,   true);
        addParam("Release",    "ms", 5.f,    1000.f,  150.f,  true);
        addParam("Low Gain",   "dB", -24.f,  24.f,    0.f);
        addParam("Mid Gain",   "dB", -24.f,  24.f,    0.f);
        addParam("High Gain",  "dB", -24.f,  24.f,    0.f);
        addParam("Input",      "dB", -24.f,  24.f,    0.f);
        addParam("Output",     "dB", -24.f,  24.f,    0.f);
        addParam("Dry/Wet",    "",   0.f,    1.f,     1.f);
    }

    // GUI thread. Nothing here allocates: every member is a fixed-size filter
    // section or a scalar. There is no delay line in this device at all, which
    // is the same sentence as "latencyFrames() is zero".
    bool prepare(f64 sampleRate, int maxBlock) override {
        sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxBlock_ = maxBlock > 0 ? maxBlock : kMaxBlock;

        // A coefficient glide of 2 ms: fast enough that a crossover move
        // follows the knob, slow enough that it is a slide and not a step.
        glide_ = dsp::poleCoef(sr_, 0.002f);

        for (int b = 0; b < kBlBands; ++b) {
            gain_[b].setTime(sr_, 0.02f);
            gain_[b].snap(blDbToGain(bandGainDb(b)));
            env_[b] = 0.f;
        }
        inG_.setTime(sr_, 0.02f);
        inG_.snap(blDbToGain(clampv(p(kBlInput), -24.f, 24.f)));
        outG_.setTime(sr_, 0.02f);
        outG_.snap(blDbToGain(clampv(p(kBlOutput), -24.f, 24.f)));
        mix_.setTime(sr_, 0.02f);
        mix_.snap(clampv(p(kBlMix), 0.f, 1.f));

        computeTargets();
        cLpL_ = tLpL_; cHpL_ = tHpL_; cLpH_ = tLpH_; cHpH_ = tHpH_; cApH_ = tApH_;
        reset();
        first_ = true;
        return true;
    }

    int presetCount() const override { return kBlPresetCount; }

    const char* presetName(int i) const override {
        return (i >= 0 && i < kBlPresetCount) ? kBlPresets[i].name : nullptr;
    }

    void loadPreset(int i) override {
        if (i < 0 || i >= kBlPresetCount) return;
        for (int k = 0; k < n_; ++k) setParam(k, info_[(size_t)k].def);
        const BlPreset& pr = kBlPresets[i];
        for (int k = 0; k < pr.n; ++k) setParam(pr.set[k].id, pr.set[k].v);
    }

    // REALTIME. No allocation, no block-sized scratch, so any nframes is
    // processed rather than degraded to passthrough.
    void process(const f32* const* in, f32* const* out, int channels, int nframes) override {
        if (channels <= 0 || nframes <= 0 || !out) return;
        if (isBypassed() || !in) { passthrough(in, out, channels, nframes); return; }

        computeTargets();

        const f32 depth = clampv(p(kBlDepth), 0.f, 1.f);
        const f32 upR   = clampv(p(kBlUpRatio), 1.f, 20.f);
        const f32 dnR   = clampv(p(kBlDownRatio), 1.f, 20.f);
        const f32 upK   = 1.f - 1.f / upR;
        const f32 dnK   = 1.f - 1.f / dnR;
        const f32 atkMs = clampv(p(kBlAttack), 0.1f, 200.f);
        const f32 relMs = clampv(p(kBlRelease), 5.f, 1000.f);
        for (int b = 0; b < kBlBands; ++b) {
            aAtk_[b] = dsp::poleCoef(sr_, atkMs * 1e-3f * kBlTimeScale[b]);
            aRel_[b] = dsp::poleCoef(sr_, relMs * 1e-3f * kBlTimeScale[b]);
            gain_[b].set(blDbToGain(bandGainDb(b)));
        }
        const f32 inDb  = clampv(p(kBlInput), -24.f, 24.f);
        const f32 outDb = clampv(p(kBlOutput), -24.f, 24.f);
        const f32 mixV  = clampv(p(kBlMix), 0.f, 1.f);
        inG_.set(blDbToGain(inDb));
        outG_.set(blDbToGain(outDb));
        mix_.set(mixV);
        if (first_) {
            for (int b = 0; b < kBlBands; ++b) gain_[b].settle();
            inG_.settle(); outG_.settle(); mix_.settle();
            first_ = false;
        }

        // THE WIRE. Depth at zero with every gain at unity and the mix full
        // wet: the output is the input, bit for bit. The crossover below still
        // runs -- see the file header for why that is the right trade.
        const bool wire = (depth == 0.f) && (mixV == 1.f) && (inDb == 0.f) &&
                          (outDb == 0.f) && (bandGainDb(0) == 0.f) &&
                          (bandGainDb(1) == 0.f) && (bandGainDb(2) == 0.f);

        const int nc = channels < kCh ? channels : kCh;
        const f32* src[kCh] = { nullptr, nullptr };
        f32*       dst[kCh] = { nullptr, nullptr };
        for (int c = 0; c < nc; ++c) { src[c] = in[c]; dst[c] = out[c]; }

        for (int i = 0; i < nframes; ++i) {
            // Coefficients glide per sample towards the targets. Per SAMPLE and
            // not per block: a crossover that jumps at a buffer boundary is a
            // click whose loudness depends on the audio interface.
            blGlideStep(cLpL_, tLpL_, glide_);
            blGlideStep(cHpL_, tHpL_, glide_);
            blGlideStep(cLpH_, tLpH_, glide_);
            blGlideStep(cHpH_, tHpH_, glide_);
            blGlideStep(cApH_, tApH_, glide_);

            const f32 ig = inG_.next();
            const f32 xl = src[0] ? src[0][i] : 0.f;
            const f32 xr = (nc > 1 && src[1]) ? src[1][i] : xl;
            const f32 dl = xl, dr = xr;             // the dry pair, untouched
            const f32 al = xl * ig, ar = xr * ig;

            // --- the split -------------------------------------------------
            f32 band[kBlBands][kCh];
            splitOne(0, al, band[0][0], band[1][0], band[2][0]);
            splitOne(1, ar, band[0][1], band[1][1], band[2][1]);

            // --- per band: detector, both directions, band gain ------------
            f32 wl = 0.f, wr = 0.f;
            for (int b = 0; b < kBlBands; ++b) {
                const f32 bl = band[b][0], br = band[b][1];
                // Stereo-linked peak. max() and not a sum: the loudest side is
                // what a listener localises, and following the sum would let a
                // hard-panned hit duck the other channel by half as much.
                const f32 pk = std::fmax(std::fabs(bl), std::fabs(br));
                f32 e = env_[b];
                e += (pk - e) * ((pk > e) ? aAtk_[b] : aRel_[b]);
                env_[b] = flushDenormal(e);

                f32 g = gain_[b].next();
                if (depth > 0.f) {
                    const f32 lvl = 20.f * std::log10(std::fmax(e, kBlEnvFloor));
                    f32 db = 0.f;
                    if (lvl > kBlThrDown) db -= (lvl - kBlThrDown) * dnK;
                    if (lvl < kBlThrUp)   db += std::fmin((kBlThrUp - lvl) * upK, kBlMaxBoost);
                    if (db != 0.f) g *= blDbToGain(db * depth);
                }
                wl += bl * g;
                wr += br * g;
            }

            const f32 og = outG_.next();
            const f32 mx = mix_.next();
            wl *= og;
            wr *= og;

            if (wire) {
                if (dst[0]) dst[0][i] = dl;
                if (nc > 1 && dst[1]) dst[1][i] = dr;
            } else {
                if (dst[0]) dst[0][i] = dl * (1.f - mx) + wl * mx;
                if (nc > 1 && dst[1]) dst[1][i] = dr * (1.f - mx) + wr * mx;
            }
        }

        // The coefficient snap happens per SAMPLE, in blGlideStep -- see its
        // note. Nothing block-shaped is left to do here but the sanity checks.
        for (int c = 0; c < kCh; ++c) {
            st_[c].lp1.check(); st_[c].lp2.check();
            st_[c].hp1.check(); st_[c].hp2.check();
            st_[c].mlp1.check(); st_[c].mlp2.check();
            st_[c].mhp1.check(); st_[c].mhp2.check();
            st_[c].ap.check();
        }
        for (int b = 0; b < kBlBands; ++b) if (!dsp::sane(env_[b])) env_[b] = 0.f;

        copyExtra(in, out, nc, channels, nframes);
    }

private:
    static constexpr int kCh = 2;

    struct ChanState {
        dsp::BiquadState lp1, lp2;      // LP4(fLow)
        dsp::BiquadState hp1, hp2;      // HP4(fLow)
        dsp::BiquadState mlp1, mlp2;    // LP4(fHigh) on the remainder
        dsp::BiquadState mhp1, mhp2;    // HP4(fHigh) on the remainder
        dsp::BiquadState ap;            // AP2(fHigh) compensating the low band
        void reset() {
            lp1.reset(); lp2.reset(); hp1.reset(); hp2.reset();
            mlp1.reset(); mlp2.reset(); mhp1.reset(); mhp2.reset(); ap.reset();
        }
    };

    // REALTIME. One channel through the whole tree. The allpass on the low band
    // is the compensation the header derives; without it the three bands do not
    // sum flat and the null gate fails by 6 dB at the lower crossover.
    inline void splitOne(int c, f32 x, f32& lo, f32& mid, f32& hi) {
        ChanState& s = st_[c];
        f32 l = dsp::biquadTick(cLpL_, s.lp1, x);
        l     = dsp::biquadTick(cLpL_, s.lp2, l);
        f32 r = dsp::biquadTick(cHpL_, s.hp1, x);
        r     = dsp::biquadTick(cHpL_, s.hp2, r);

        f32 m = dsp::biquadTick(cLpH_, s.mlp1, r);
        m     = dsp::biquadTick(cLpH_, s.mlp2, m);
        f32 h = dsp::biquadTick(cHpH_, s.mhp1, r);
        h     = dsp::biquadTick(cHpH_, s.mhp2, h);

        lo  = dsp::biquadTick(cApH_, s.ap, l);
        mid = m;
        hi  = h;
    }

    f32 bandGainDb(int b) const {
        return clampv(p(kBlLowGain + b), -24.f, 24.f);
    }

    // The two corners, with the ordering constraint applied where it belongs:
    // a High Split at or below the Low Split is not an error the user made, it
    // is two knobs passing each other during an automation sweep, and the
    // answer is a band that narrows to nothing rather than a filter that
    // inverts.
    void computeTargets() {
        const f32 fl = clampv(p(kBlLowSplit), 40.f, 1000.f);
        f32 fh = clampv(p(kBlHighSplit), 800.f, 14000.f);
        if (fh < fl * 1.2f) fh = fl * 1.2f;
        const f32 nyq = (f32)(sr_ * 0.45);
        const f32 flc = clampv(fl, 20.f, nyq);
        const f32 fhc = clampv(fh, 20.f, nyq);

        tLpL_ = blLowpass (sr_, flc, kQ);
        tHpL_ = blHighpass(sr_, flc, kQ);
        tLpH_ = blLowpass (sr_, fhc, kQ);
        tHpH_ = blHighpass(sr_, fhc, kQ);
        tApH_ = blAllpass (sr_, fhc, kQ);
    }

    void reset() {
        for (int c = 0; c < kCh; ++c) st_[c].reset();
        for (int b = 0; b < kBlBands; ++b) env_[b] = 0.f;
    }

    // Butterworth. Two of these cascaded IS Linkwitz-Riley 4th order, and it is
    // the only Q for which the sum identity in the header holds.
    static constexpr f32 kQ = 0.70710678f;

    // --- factory presets ---------------------------------------------------
    struct BlPreset {
        const char* name;
        int         n;
        struct { int id; f32 v; } set[13];
    };
    static constexpr int kBlPresetCount = 6;
    static const BlPreset kBlPresets[kBlPresetCount];

    ChanState st_[kCh];
    dsp::BiquadCoeffs cLpL_, cHpL_, cLpH_, cHpH_, cApH_;
    dsp::BiquadCoeffs tLpL_, tHpL_, tLpH_, tHpH_, tApH_;
    dsp::Smoother gain_[kBlBands], inG_, outG_, mix_;
    f32  env_[kBlBands]  = { 0.f, 0.f, 0.f };
    f32  aAtk_[kBlBands] = { 1.f, 1.f, 1.f };
    f32  aRel_[kBlBands] = { 1.f, 1.f, 1.f };
    f32  glide_ = 1.f;
    bool first_ = true;
};

const Bloom::BlPreset Bloom::kBlPresets[Bloom::kBlPresetCount] = {
    { "Init", 0, {} },

    // Depth low, ratios mild, slow release: the one you leave on a bus and
    // forget about.
    { "Gentle Glue", 6, {
        { kBlDepth, 0.22f }, { kBlUpRatio, 2.f }, { kBlDownRatio, 2.5f },
        { kBlAttack, 25.f }, { kBlRelease, 300.f }, { kBlMix, 1.f },
    } },

    // The thing itself. Both directions hard, fast, and a smile on the band
    // gains -- which is what everyone reaches for it for.
    { "OTT", 8, {
        { kBlDepth, 1.f }, { kBlUpRatio, 6.f }, { kBlDownRatio, 6.f },
        { kBlAttack, 1.f }, { kBlRelease, 60.f },
        { kBlLowGain, -1.f }, { kBlHighGain, 2.f }, { kBlOutput, -4.f },
    } },

    // Parallel rather than serial: full depth, mixed in at a third, which keeps
    // the transients the serial version eats.
    { "Parallel Crush", 7, {
        { kBlDepth, 1.f }, { kBlUpRatio, 8.f }, { kBlDownRatio, 8.f },
        { kBlAttack, 0.5f }, { kBlRelease, 90.f }, { kBlMix, 0.35f },
        { kBlOutput, -2.f },
    } },

    // Splits placed for a kit: the low band under the kick, the high band on
    // the cymbals, and a fast attack that lets the stick through.
    { "Drum Smash", 9, {
        { kBlLowSplit, 140.f }, { kBlHighSplit, 3500.f },
        { kBlDepth, 0.75f }, { kBlUpRatio, 5.f }, { kBlDownRatio, 4.f },
        { kBlAttack, 3.f }, { kBlRelease, 45.f },
        { kBlHighGain, 3.f }, { kBlOutput, -3.f },
    } },

    // Upward only (down ratio at 1), slow, wide: raises the floor of a sparse
    // vocal or a room mic without touching the peaks at all.
    { "Air Lift", 7, {
        { kBlLowSplit, 400.f }, { kBlHighSplit, 5000.f },
        { kBlDepth, 0.5f }, { kBlUpRatio, 4.f }, { kBlDownRatio, 1.f },
        { kBlRelease, 400.f }, { kBlHighGain, 2.f },
    } },
};

constexpr const char* kBloomUri = "nxtakt:bloom";

PluginDesc bloomDesc() {
    PluginDesc d;
    d.format     = PluginFormat::Internal;
    d.uri        = kBloomUri;
    d.name       = "Bloom";
    d.vendor     = "NxTakt";
    d.category   = "Dynamics";
    d.kind       = PluginKind::Effect;
    d.audioIn    = 2;
    d.audioOut   = 2;
    d.hasMidiIn  = false;
    d.paramCount = kBlParamCount;
    return d;
}

} // namespace
} // namespace detail
} // namespace lat

#endif // LAT_FX_BLOOM_IN_INTERNAL_DEVICES
