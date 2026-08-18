// Tape — NxTakt's tape machine.
//
// Drive with a bias so the harmonics are EVEN, wow and flutter on a real
// fractional-delay transport, a head bump and an HF loss that both track a
// speed switch, hiss you have to ask for, and channel crosstalk. It is the one
// effect in the set whose job is to make things worse in a way people pay for.
//
// ---------------------------------------------------------------------------
// THE PARAMETER IDS ARE FROZEN. See kTpParam* below.
// ---------------------------------------------------------------------------
//
// SIGNAL ORDER, and it is the order a machine has:
//
//   in -> record head (the delay line) -> repro head, read back at
//         base + wow + flutter samples  -> drive/bias  -> head bump
//      -> HF loss -> crosstalk -> hiss -> DC block -> dry/wet -> output
//
// The DRIVE IS AFTER THE TRANSPORT, not before it, which is the one ordering
// decision here that changes the sound: wow modulates the signal that is then
// saturated, so a pitch wobble under a hot input does not modulate the
// distortion at the wow rate. Machines work the other way round and it sounds
// like a fault rather than like tape.
//
// ---------------------------------------------------------------------------
// LATENCY IS REAL AND IS REPORTED
//
// The repro head sits kTpHeadMs = 8 ms downstream of the record head, because
// wow has to modulate a delay and a delay cannot go negative: at 0.5 Hz and
// full depth the transport needs 5.6 ms of excursion in each direction, and 8
// ms is the smallest base that holds it with margin.
//
// So latencyFrames() returns that figure, exactly, and the engine compensates
// for it. This is the SECOND internal device with latency (the Limiter is the
// first) and it obeys the same three rules the Limiter's header sets out: the
// number is fixed at prepare(), it is never a parameter, and it is the truth
// rather than an estimate -- the dry half of Dry/Wet is read from the same line
// at exactly the same base, by an INTEGER tap, so the dry path is the input
// delayed by precisely latencyFrames() samples and nothing else.
//
// A machine with a record-to-repro gap is what a tape delay IS, so this is not
// a cost the device is apologising for; it is the head gap, and at 15 ips 8 ms
// is about an inch and a half of tape.
//
// ---------------------------------------------------------------------------
// THE HARMONICS
//
//     y = ( tanh(g*x + b) - tanh(b) ) / tanh(g)
//
// The subtraction is what makes it usable: tanh(b) is a DC offset, and without
// removing it every Bias change would thump. What survives the subtraction is
// the ASYMMETRY, which is the entire point -- an odd function has only odd
// harmonics, and tanh(x + b) is not odd for b != 0. So:
//
//   * Bias 0 gives an exactly odd shaper: H2 is zero to the precision of the
//     arithmetic (the suite measures it below -100 dB), H3 and up only.
//   * Bias > 0 gives H2, growing with the bias, which is the "warmth" the word
//     is usually waved at.
//
// Dividing by tanh(g) is the level compensation: a full-scale input comes back
// at full scale for any Drive, so the knob adds harmonics rather than volume
// and the THD curve the suite prints is a curve in DISTORTION and not in gain.
// Quiet signals still get louder as Drive rises -- the small-signal gain is
// g/tanh(g) -- and that is not a defect, it is what tape compression IS.
//
// H2 IS NOT MONOTONE ALL THE WAY UP, and the suite prints the whole curve
// rather than pretending otherwise. It rises with Drive to about +12 dB and
// then falls, because tanh(g*x + b) with a large g stops caring about b: the
// asymmetry is a fixed offset and the drive is not, so past the knee the shaper
// converges on an odd square-ish limiter and the even harmonic goes back down.
// The gate asserts the rise over the range where the even harmonic is what the
// Bias knob is for, and asserts PRESENCE everywhere above it.
//
// ---------------------------------------------------------------------------
// WOW DEPTH IS CALIBRATED IN CENTS, and that is the whole reason it is testable
//
// A sinusoidal delay modulation of Ds seconds at f Hz produces an instantaneous
// pitch ratio of 1 + 2*pi*f*Ds at its peak, so
//
//     cents = 1200 * log2(1 + 2*pi*f*Ds)
//
// The knob is defined in that unit -- 0 to kTpWowCents -- and the excursion is
// SOLVED for it: Ds = (2^(cents/1200) - 1) / (2*pi*f). The consequence is that
// the depth means the same thing at 0.5 Hz as at 6 Hz, which is what a user
// expects and what a delay-time knob does not give them. It is also why the
// suite's wow gate can assert an exact number instead of "it wobbles".
//
// FLUTTER is not a slower wow with a bigger number. It is the sum of two
// incommensurate rates (kTpFlutA, kTpFlutB, scaled by the transport speed) plus
// a low-passed noise, because a single sine reads as vibrato and the thing that
// makes flutter sound mechanical is that it never repeats. Every one of those
// three runs on a counter that advances once per SAMPLE -- the noise from a
// dedicated LCG, kept apart from the hiss generator's so the two streams cannot
// interleave differently (the discipline spectra.cpp states for its two
// counters, for the identical reason).
//
// ---------------------------------------------------------------------------
// HISS IS OFF, AND OFF MEANS ZERO
//
// The Hiss parameter's minimum is not "very quiet", it is silent: at kTpHissOff
// the noise term is not attenuated, it is NOT ADDED. A device whose default
// state puts -90 dBFS of noise into every channel of a mix is a device that
// turns a 40-track session into a noise floor, and "you cannot hear it" is not
// the same claim as "it is not there". The suite checks for an exact zero.
#ifndef LAT_FX_TAPE_IN_INTERNAL_DEVICES

// Compiled standalone: an empty translation unit, exactly as spectra.cpp is.
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
    kTpDrive     = 0,   // dB
    kTpBias      = 1,   //      0 = odd shaper, no H2
    kTpSpeed     = 2,   //      0 = 7.5 ips, 1 = 15, 2 = 30
    kTpWowRate   = 3,   // Hz
    kTpWowDepth  = 4,   //      0..1 -> 0..kTpWowCents cents peak
    kTpFlutter   = 5,   //      0..1
    kTpHeadBump  = 6,   // dB
    kTpRolloff   = 7,   //      0..1, how much of the speed's HF loss to apply
    kTpHiss      = 8,   // dB   kTpHissOff means OFF, exactly
    kTpCrosstalk = 9,   //      0..1
    kTpOutput    = 10,  // dB
    kTpMix       = 11,  //      dry/wet
};
constexpr int kTpParamCount = 12;

// The record-to-repro gap. See the header: this IS latencyFrames().
constexpr f64 kTpHeadSec = 0.008;
// The line only ever holds the gap plus the excursion; 30 ms is generous.
constexpr f64 kTpLineSec = 0.030;

// Full wow depth, in cents of peak pitch deviation.
constexpr f32 kTpWowCents = 30.f;

// Flutter: two rates in no small integer ratio, at 15 ips. Scaled by the speed
// factor below, because a faster transport flutters faster.
constexpr f32 kTpFlutA = 7.31f;
constexpr f32 kTpFlutB = 12.79f;
// Peak flutter excursion at full knob, in seconds. Small: at 7.3 Hz this is
// about 8 cents, which is where flutter stops being texture and starts being
// a broken machine.
constexpr f32 kTpFlutSec = 1.2e-4f;

// The Hiss parameter's minimum. Reaching it does not attenuate the noise, it
// removes it.
constexpr f32 kTpHissOff = -90.f;

// Per speed: head-bump centre (Hz), HF corner (Hz), flutter rate scale.
// The bump climbs with speed because it is a wavelength effect at the head gap,
// and the HF corner climbs with it for the same reason in reverse.
constexpr f32 kTpBumpHz[3]  = {  55.f,  80.f, 115.f };
constexpr f32 kTpHfHz[3]    = { 9000.f, 14000.f, 19000.f };
constexpr f32 kTpFlutScl[3] = { 0.70f, 1.00f, 1.40f };

// Head bump Q. Wide enough to be a lift rather than a resonance, narrow enough
// that the centre is a measurable peak -- which the speed gate depends on.
constexpr f32 kTpBumpQ = 1.1f;

constexpr f32 kTpTwoPi = 6.28318530718f;

using dsp::flushDenormal;

// RBJ lowpass, written here rather than shared: see the same note in
// fx_bloom.cpp. Duplicated deliberately so neither device depends on the
// textual include ORDER inside internal_devices.cpp.
inline dsp::BiquadCoeffs tpLowpass(f64 sr, f32 hz, f32 q) {
    const f64 w = dsp::rbj::omega(sr, hz);
    const f64 c = std::cos(w), s = std::sin(w);
    const f64 al = s / (2.0 * (f64)clampv(q, 0.05f, 40.f));
    return dsp::rbj::normalise({ (1.0 - c) * 0.5, 1.0 - c, (1.0 - c) * 0.5,
                                 1.0 + al, -2.0 * c, 1.0 - al });
}

inline f32 tpDbToGain(f32 db) { return (db == 0.f) ? 1.f : std::pow(10.f, db * 0.05f); }

// Glide and snap in one step, per SAMPLE. See the identical note in
// fx_bloom.cpp: biquadSettle() once per block would fire at a sample index that
// depends on the block size, and bit identity across block sizes is a gate.
inline void tpGlideStep(dsp::BiquadCoeffs& c, const dsp::BiquadCoeffs& t, f32 g) {
    dsp::biquadGlide(c, t, g);
    dsp::biquadSettle(c, t);
}

// ---------------------------------------------------------------------------
// Tape
// ---------------------------------------------------------------------------
class Tape final : public InternalInstance {
public:
    explicit Tape(const PluginDesc& d) : InternalInstance(d) {
        addParam   ("Drive",     "dB", 0.f,   24.f,  3.f);
        addParam   ("Bias",      "",   0.f,   1.f,   0.30f);
        addIntParam("Speed",             0,   2,     1);
        addParam   ("Wow Rate",  "Hz", 0.5f,  6.f,   1.f,   true);
        addParam   ("Wow Depth", "",   0.f,   1.f,   0.25f);
        addParam   ("Flutter",   "",   0.f,   1.f,   0.20f);
        addParam   ("Head Bump", "dB", 0.f,   9.f,   3.f);
        addParam   ("HF Rolloff","",   0.f,   1.f,   1.f);
        addParam   ("Hiss",      "dB", kTpHissOff, -40.f, kTpHissOff);
        addParam   ("Crosstalk", "",   0.f,   1.f,   0.15f);
        addParam   ("Output",    "dB", -24.f, 24.f,  0.f);
        addParam   ("Dry/Wet",   "",   0.f,   1.f,   1.f);
    }

    // GUI thread. The only allocation in the device.
    bool prepare(f64 sampleRate, int maxBlock) override {
        sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxBlock_ = maxBlock > 0 ? maxBlock : kMaxBlock;

        headS_ = (int)(kTpHeadSec * sr_ + 0.5);
        if (headS_ < 8) headS_ = 8;
        const int n = (int)(kTpLineSec * sr_) + 16;
        for (int c = 0; c < kCh; ++c) {
            line_[c].resize(n);
            line_[c].reset();
            xtLp_[c].setCutoff(sr_, 6000.f);
            dc_[c].setCutoff(sr_, 8.f);
            dc_[c].reset();
            bump_[c].reset();
            roll_[c].reset();
        }
        // The excursion can never eat the whole gap: 16 samples of margin means
        // the fractional read always has two valid neighbours.
        maxExc_ = (f32)headS_ - 16.f;
        if (maxExc_ < 1.f) maxExc_ = 1.f;

        glide_ = dsp::poleCoef(sr_, 0.005f);
        wowPh_ = flutA_ = flutB_ = 0.f;
        flutRng_ = 0x9E3779B9u;
        hissRng_ = 0x2545F491u;
        flutN_.setCutoff(sr_, 30.f);
        flutN_.reset();

        drive_.setTime(sr_, 0.02f);  drive_.snap(driveGain());
        bias_.setTime(sr_, 0.02f);   bias_.snap(clampv(p(kTpBias), 0.f, 1.f) * kBiasMax);
        xt_.setTime(sr_, 0.02f);     xt_.snap(clampv(p(kTpCrosstalk), 0.f, 1.f));
        outG_.setTime(sr_, 0.02f);   outG_.snap(tpDbToGain(clampv(p(kTpOutput), -24.f, 24.f)));
        mix_.setTime(sr_, 0.02f);    mix_.snap(clampv(p(kTpMix), 0.f, 1.f));

        computeTargets();
        cBump_ = tBump_;
        cRoll_ = tRoll_;
        first_ = true;
        return true;
    }

    // The head gap, in frames. Constant after prepare(), never a parameter, and
    // exact -- see the file header.
    int latencyFrames() const override { return headS_; }

    int presetCount() const override { return kTpPresetCount; }

    const char* presetName(int i) const override {
        return (i >= 0 && i < kTpPresetCount) ? kTpPresets[i].name : nullptr;
    }

    void loadPreset(int i) override {
        if (i < 0 || i >= kTpPresetCount) return;
        for (int k = 0; k < n_; ++k) setParam(k, info_[(size_t)k].def);
        const TpPreset& pr = kTpPresets[i];
        for (int k = 0; k < pr.n; ++k) setParam(pr.set[k].id, pr.set[k].v);
    }

    // REALTIME. No allocation, no block-sized scratch: any nframes is
    // processed, never degraded to passthrough.
    void process(const f32* const* in, f32* const* out, int channels, int nframes) override {
        if (channels <= 0 || nframes <= 0 || !out) return;
        if (isBypassed() || !in || line_[0].buf.empty()) { passthrough(in, out, channels, nframes); return; }

        const int spd = (int)clampv(p(kTpSpeed) + 0.5f, 0.f, 2.f);
        computeTargets();

        drive_.set(driveGain());
        bias_.set(clampv(p(kTpBias), 0.f, 1.f) * kBiasMax);
        xt_.set(clampv(p(kTpCrosstalk), 0.f, 1.f));
        outG_.set(tpDbToGain(clampv(p(kTpOutput), -24.f, 24.f)));
        mix_.set(clampv(p(kTpMix), 0.f, 1.f));
        if (first_) {
            drive_.settle(); bias_.settle(); xt_.settle(); outG_.settle(); mix_.settle();
            first_ = false;
        }

        // --- wow: solve the excursion for the requested CENTS ---------------
        const f32 wowHz    = clampv(p(kTpWowRate), 0.5f, 6.f);
        const f32 wowCents = clampv(p(kTpWowDepth), 0.f, 1.f) * kTpWowCents;
        f32 wowExc = 0.f;                       // samples, peak
        if (wowCents > 0.f) {
            const f32 ratio = std::exp2(wowCents / 1200.f) - 1.f;
            wowExc = ratio / (kTpTwoPi * wowHz) * (f32)sr_;
        }
        const f32 flut  = clampv(p(kTpFlutter), 0.f, 1.f);
        const f32 flutExc = flut * kTpFlutSec * (f32)sr_;
        // Wow gets whatever is left after flutter has had its share, so the two
        // together can never walk the read pointer off the front of the line.
        const f32 wowCap = maxExc_ - flutExc;
        if (wowExc > wowCap) wowExc = wowCap > 0.f ? wowCap : 0.f;

        const f32 wowInc  = (f32)((f64)wowHz / sr_);
        const f32 flutIncA = (f32)((f64)(kTpFlutA * kTpFlutScl[spd]) / sr_);
        const f32 flutIncB = (f32)((f64)(kTpFlutB * kTpFlutScl[spd]) / sr_);

        const f32 hissDb = clampv(p(kTpHiss), kTpHissOff, -40.f);
        const f32 hissG  = (hissDb <= kTpHissOff) ? 0.f : tpDbToGain(hissDb);

        const int nc = channels < kCh ? channels : kCh;
        const f32* src[kCh] = { nullptr, nullptr };
        f32*       dst[kCh] = { nullptr, nullptr };
        for (int c = 0; c < nc; ++c) { src[c] = in[c]; dst[c] = out[c]; }

        for (int i = 0; i < nframes; ++i) {
            tpGlideStep(cBump_, tBump_, glide_);
            tpGlideStep(cRoll_, tRoll_, glide_);

            const f32 xl = src[0] ? src[0][i] : 0.f;
            const f32 xr = (nc > 1 && src[1]) ? src[1][i] : xl;

            // --- the transport ---------------------------------------------
            // One phase accumulator per modulator, each advanced by exactly one
            // increment per SAMPLE, and the noise drawn once per sample from
            // its own counter. That is the whole of block-size invariance: not
            // one of these is a function of where a buffer boundary fell.
            wowPh_ += wowInc;  if (wowPh_ >= 1.f) wowPh_ -= 1.f;
            flutA_ += flutIncA; if (flutA_ >= 1.f) flutA_ -= 1.f;
            flutB_ += flutIncB; if (flutB_ >= 1.f) flutB_ -= 1.f;
            flutRng_ = flutRng_ * 1664525u + 1013904223u;
            const f32 wn = (f32)((i32)(flutRng_ >> 8) - 8388608) * (1.f / 8388608.f);
            const f32 nz = flutN_.process(wn) * kNoiseNorm;

            const f32 wow = wowExc * std::sin(kTpTwoPi * wowPh_);
            const f32 fl  = flutExc * (0.55f * std::sin(kTpTwoPi * flutA_) +
                                       0.30f * std::sin(kTpTwoPi * flutB_) +
                                       0.15f * nz);
            f32 rd = (f32)headS_ + wow + fl;
            rd = clampv(rd, 2.f, (f32)line_[0].capacity() - 2.f);

            // --- repro ------------------------------------------------------
            // READ THEN PUSH, which is the tap convention internal_dsp.h states
            // and the only order for which tap(headS_) is EXACTLY headS_
            // samples of delay -- which is what latencyFrames() promises the
            // engine, and what the suite's impulse test measures.
            f32 w[kCh];
            f32 dry[kCh];
            for (int c = 0; c < kCh; ++c) {
                dry[c] = line_[c].tap(headS_);          // integer tap: exact
                w[c]   = line_[c].tapLerp(rd);
            }
            line_[0].push(flushDenormal(xl));
            line_[1].push(flushDenormal(xr));

            const f32 g  = drive_.next();
            const f32 b  = bias_.next();
            const f32 tb = std::tanh(b);
            const f32 ng = 1.f / std::tanh(g);

            for (int c = 0; c < kCh; ++c) {
                w[c] = (std::tanh(g * w[c] + b) - tb) * ng;
                w[c] = dsp::biquadTick(cBump_, bump_[c], w[c]);
                w[c] = dsp::biquadTick(cRoll_, roll_[c], w[c]);
            }

            // --- crosstalk: the other channel, dulled, at a low level -------
            const f32 ct = xt_.next() * kXtMax;
            if (ct != 0.f) {
                const f32 bl = xtLp_[0].process(w[1]);
                const f32 br = xtLp_[1].process(w[0]);
                w[0] += ct * bl;
                w[1] += ct * br;
            } else {
                // Keep the crosstalk filters tracking so turning the knob up
                // does not start from a cold filter.
                xtLp_[0].process(w[1]);
                xtLp_[1].process(w[0]);
            }

            // --- hiss: added, or genuinely not added ------------------------
            hissRng_ = hissRng_ * 1664525u + 1013904223u;
            if (hissG != 0.f) {
                const u32 h2 = hissRng_ * 1664525u + 1013904223u;
                w[0] += hissG * (f32)((i32)(hissRng_ >> 8) - 8388608) * (1.f / 8388608.f);
                w[1] += hissG * (f32)((i32)(h2 >> 8) - 8388608) * (1.f / 8388608.f);
            }

            const f32 og = outG_.next();
            const f32 mx = mix_.next();
            for (int c = 0; c < kCh; ++c) w[c] = dc_[c].process(w[c]);

            if (dst[0]) dst[0][i] = (dry[0] * (1.f - mx) + w[0] * mx) * og;
            if (nc > 1 && dst[1]) dst[1][i] = (dry[1] * (1.f - mx) + w[1] * mx) * og;
        }

        // The coefficient snap is per SAMPLE, in tpGlideStep.
        for (int c = 0; c < kCh; ++c) {
            bump_[c].check(); roll_[c].check(); dc_[c].check(); xtLp_[c].check();
        }
        flutN_.check();
        if (!dsp::sane(wowPh_)) wowPh_ = 0.f;
        if (!dsp::sane(flutA_)) flutA_ = 0.f;
        if (!dsp::sane(flutB_)) flutB_ = 0.f;

        copyExtra(in, out, nc, channels, nframes);
    }

private:
    static constexpr int kCh = 2;
    // The bias, in shaper units, at Bias = 1. Beyond about 0.8 the shaper's
    // small-signal gain has dropped far enough that the device sounds like a
    // gate rather than like tape.
    static constexpr f32 kBiasMax = 0.70f;
    // Crosstalk at the knob's top, as a linear fraction. -20 dB is about what a
    // well-aligned two-track does; more than that and it is a mono button.
    static constexpr f32 kXtMax = 0.10f;
    // The 30 Hz one-pole takes most of the energy out of the white noise it is
    // fed, so the flutter noise term is renormalised back to roughly unit peak.
    static constexpr f32 kNoiseNorm = 18.f;
    // See driveGain(). 0.35 puts the shaper's small-signal gain at +0.3 dB when
    // the Drive knob is at its minimum, and still reaches g = 5.5 at +24 dB,
    // which is well past the knee.
    static constexpr f32 kDriveScale = 0.35f;

    // The shaper's input gain. kDriveScale is why a Drive of 0 dB is nearly a
    // wire rather than a colour: the small-signal gain of the normalised shaper
    // is g / tanh(g), which is 1.31 (+2.4 dB) at g = 1 and 1.04 (+0.3 dB) at
    // g = 0.35. A device whose minimum setting is +2.4 dB of lift is a device
    // that flatters itself in every A/B, which is the oldest trick in the
    // saturation business and not one this tree plays.
    f32 driveGain() const {
        return kDriveScale * tpDbToGain(clampv(p(kTpDrive), 0.f, 24.f));
    }

    void computeTargets() {
        const int spd = (int)clampv(p(kTpSpeed) + 0.5f, 0.f, 2.f);
        const f32 bumpDb = clampv(p(kTpHeadBump), 0.f, 9.f);
        tBump_ = dsp::rbj::peaking(sr_, kTpBumpHz[spd], bumpDb, kTpBumpQ);

        // Rolloff 0 puts the corner just below Nyquist, which is the closest a
        // one-section filter gets to "not there" without a branch that would
        // click the moment the knob left zero.
        const f32 amt = clampv(p(kTpRolloff), 0.f, 1.f);
        const f32 top = (f32)(sr_ * 0.45);
        const f32 fc  = kTpHfHz[spd] + (1.f - amt) * (top - kTpHfHz[spd]);
        tRoll_ = tpLowpass(sr_, clampv(fc, 200.f, top), 0.70710678f);
    }

    // --- factory presets ---------------------------------------------------
    struct TpPreset {
        const char* name;
        int         n;
        struct { int id; f32 v; } set[12];
    };
    static constexpr int kTpPresetCount = 6;
    static const TpPreset kTpPresets[kTpPresetCount];

    dsp::DelayLine    line_[kCh];
    dsp::BiquadState  bump_[kCh], roll_[kCh];
    dsp::BiquadCoeffs cBump_, cRoll_, tBump_, tRoll_;
    dsp::OnePole      xtLp_[kCh], flutN_;
    dsp::DcBlock      dc_[kCh];
    dsp::Smoother     drive_, bias_, xt_, outG_, mix_;

    int  headS_  = 1;
    f32  maxExc_ = 1.f;
    f32  glide_  = 1.f;
    f32  wowPh_ = 0.f, flutA_ = 0.f, flutB_ = 0.f;
    u32  flutRng_ = 0x9E3779B9u;
    u32  hissRng_ = 0x2545F491u;
    bool first_ = true;
};

const Tape::TpPreset Tape::kTpPresets[Tape::kTpPresetCount] = {
    { "Init", 0, {} },

    // The default machine, aligned and behaving. Almost no wow, a little bump,
    // full HF at 15 ips.
    { "Warm 15ips", 7, {
        { kTpDrive, 4.f }, { kTpBias, 0.35f }, { kTpSpeed, 1 },
        { kTpWowDepth, 0.10f }, { kTpFlutter, 0.12f },
        { kTpHeadBump, 3.5f }, { kTpCrosstalk, 0.15f },
    } },

    // A machine that has been in a garage. Slow wow, a lot of it, and the top
    // end of a 7.5 ips shell.
    { "Wobbly Cassette", 9, {
        { kTpDrive, 6.f }, { kTpBias, 0.5f }, { kTpSpeed, 0 },
        { kTpWowRate, 0.8f }, { kTpWowDepth, 0.85f }, { kTpFlutter, 0.65f },
        { kTpHeadBump, 5.f }, { kTpHiss, -62.f }, { kTpCrosstalk, 0.45f },
    } },

    // 30 ips, driven, bump pulled back: the mastering-deck sound, where the
    // only thing you want off the machine is the saturation.
    { "Hot Master", 8, {
        { kTpDrive, 12.f }, { kTpBias, 0.20f }, { kTpSpeed, 2 },
        { kTpWowDepth, 0.03f }, { kTpFlutter, 0.05f },
        { kTpHeadBump, 1.5f }, { kTpCrosstalk, 0.05f }, { kTpOutput, -2.f },
    } },

    // All bump, no top: a drum bus that gets weight rather than dirt.
    { "Slow Roll", 7, {
        { kTpDrive, 2.f }, { kTpBias, 0.15f }, { kTpSpeed, 0 },
        { kTpWowDepth, 0.06f }, { kTpFlutter, 0.10f },
        { kTpHeadBump, 8.f }, { kTpRolloff, 1.f },
    } },

    // Bias at zero: an exactly odd shaper, hard driven. Odd harmonics only,
    // which is the aggressive half of what a machine can do.
    { "Clean Bias", 7, {
        { kTpDrive, 16.f }, { kTpBias, 0.f }, { kTpSpeed, 2 },
        { kTpWowDepth, 0.f }, { kTpFlutter, 0.f },
        { kTpHeadBump, 0.f }, { kTpCrosstalk, 0.f },
    } },
};

constexpr const char* kTapeUri = "nxtakt:tape";

PluginDesc tapeDesc() {
    PluginDesc d;
    d.format     = PluginFormat::Internal;
    d.uri        = kTapeUri;
    d.name       = "Tape";
    d.vendor     = "NxTakt";
    d.category   = "Saturation";
    d.kind       = PluginKind::Effect;
    d.audioIn    = 2;
    d.audioOut   = 2;
    d.hasMidiIn  = false;
    d.paramCount = kTpParamCount;
    return d;
}

} // namespace
} // namespace detail
} // namespace lat

#endif // LAT_FX_TAPE_IN_INTERNAL_DEVICES
