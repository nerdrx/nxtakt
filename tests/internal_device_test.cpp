// Internal device, plugin-MIDI and latency-reporting tests.
//
// Everything here goes through the public registry path — scan(), find(),
// instantiate() — so the tests exercise the same code the browser and the
// device chain use, not a private constructor. Failures are recorded, not
// thrown, so one broken case never hides the rest.
//
// Built by `make build/internal_device_test`, and run by `make test`. The include
// flags are not optional -- lv2_host.cpp needs lilv's headers and clap_host.cpp
// needs the vendored CLAP ones:
//
//   g++ -std=c++20 -O2 $(pkg-config --cflags lilv-0) -Ivendor/clap/include \
//       tests/internal_device_test.cpp src/plugin/host.cpp \
//       src/plugin/lv2_host.cpp src/plugin/clap_host.cpp \
//       src/plugin/internal_devices.cpp src/core/common.cpp \
//       -o build/internal_device_test $(pkg-config --libs lilv-0) -ldl
//
// Add -fsanitize=address,undefined for the sanitiser run; the whole suite is
// clean under both.
#include "../src/plugin/host.h"
// The sampler takes a decoded buffer in through SamplerControl, so the suite has
// to be able to BUILD one. sample.h is a header of plain structs -- nothing here
// calls buildPeaks(), buildTransients() or loadSample(), so including it costs
// this binary no link dependency at all, which is the whole reason the sampler
// can be tested in a process that deliberately has no decoder.
#include "../src/audio/sample.h"
// Spectra's table accessor -- detail::spectraTables() and friends.
#include "../src/plugin/internal_base.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

using namespace lat;

// ---------------------------------------------------------------------------
// tiny check framework (same shape as engine_test.cpp)
// ---------------------------------------------------------------------------

static int gPass = 0, gFail = 0;

static void checkImpl(bool ok, int line, const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (ok) { ++gPass; std::printf("  PASS  %s\n", msg); }
    else    { ++gFail; std::printf("  FAIL  %s   (internal_device_test.cpp:%d)\n", msg, line); }
}
#define CHECK(cond, ...) checkImpl((cond), __LINE__, __VA_ARGS__)

static void banner(const char* s) { std::printf("\n== %s\n", s); }
static void note(const char* s)   { std::printf("  note  %s\n", s); }

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static constexpr f64 kSR    = 48000.0;
static constexpr int kBlock = 256;

// Stereo scratch pair with the pointer plumbing PluginInstance::process() wants.
struct Buf {
    std::vector<f32> l, r;
    f32* p[2];
    explicit Buf(int n = kBlock) : l((size_t)n, 0.f), r((size_t)n, 0.f) {
        p[0] = l.data();
        p[1] = r.data();
    }
    void clear() {
        std::fill(l.begin(), l.end(), 0.f);
        std::fill(r.begin(), r.end(), 0.f);
    }
    f32 peak() const {
        f32 m = 0.f;
        for (f32 v : l) m = std::fmax(m, std::fabs(v));
        for (f32 v : r) m = std::fmax(m, std::fabs(v));
        return m;
    }
    bool finite() const {
        for (f32 v : l) if (!std::isfinite(v)) return false;
        for (f32 v : r) if (!std::isfinite(v)) return false;
        return true;
    }
};

// Deterministic noise: a test that fails only on some runs is worse than no
// test. Plain 32-bit LCG, plenty white enough to excite a reverb tank.
struct Noise {
    u32 s = 0x13579BDFu;
    f32 next() {
        s = s * 1664525u + 1013904223u;
        return (f32)((i32)(s >> 8) - 8388608) * (1.f / 8388608.f);
    }
};

static int paramIndex(const PluginInstance& p, const char* name) {
    for (int i = 0; i < p.paramCount(); ++i)
        if (p.paramInfo(i).name == name) return i;
    return -1;
}

static std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// ---------------------------------------------------------------------------
// Pre-rename device URIs
//
// Every set saved before the Lattice -> NxTakt rename names its stock devices
// `lattice:*`. Those files are the user's work and will never be migrated, so
// the alias is permanent and this section is the thing that stops a careless
// cleanup from deleting it. The three properties that matter:
//
//   1. the alias resolves at all;
//   2. it resolves to the SAME descriptor the canonical URI does -- not a
//      second, subtly different copy;
//   3. the descriptor it returns carries the CANONICAL uri, which is what
//      makes a load-then-save quietly upgrade the file (serializeDevices
//      writes desc().uri).
//
// Plus: a descriptor that never went through the registry -- exactly what a
// project loader builds when it has only the saved uri -- still instantiates.
// ---------------------------------------------------------------------------

static void testLegacyUris(PluginRegistry& reg) {
    banner("pre-rename URI aliases (lattice: -> nxtakt:)");

    struct { const char* legacy; const char* canonical; } kPairs[] = {
        { "lattice:saturator", "nxtakt:saturator" },
        { "lattice:pulse",     "nxtakt:pulse"     },
    };

    for (const auto& p : kPairs) {
        const PluginDesc* viaLegacy = reg.find(p.legacy);
        const PluginDesc* viaNew    = reg.find(p.canonical);
        CHECK(viaLegacy != nullptr, "registry still resolves '%s'", p.legacy);
        CHECK(viaNew != nullptr, "registry resolves '%s'", p.canonical);
        if (!viaLegacy || !viaNew) continue;

        CHECK(viaLegacy == viaNew, "'%s' and '%s' are the same descriptor",
              p.legacy, p.canonical);
        CHECK(viaLegacy->uri == p.canonical,
              "'%s' resolves to a descriptor carrying the canonical uri ('%s')",
              p.legacy, viaLegacy->uri.c_str());

        auto inst = reg.instantiate(*viaLegacy, kSR, kBlock);
        CHECK(inst != nullptr, "'%s' instantiates", p.legacy);
        if (inst)
            CHECK(inst->desc().uri == p.canonical,
                  "the instance reports the canonical uri, so a re-save upgrades it");

        // The project-loader shape: a descriptor assembled from a saved uri
        // that never came out of the registry. instantiateInternal has to
        // accept the old spelling directly for this to work.
        PluginDesc fromFile = *viaLegacy;
        fromFile.uri = p.legacy;
        auto direct = reg.instantiate(fromFile, kSR, kBlock);
        CHECK(direct != nullptr, "a stale descriptor carrying '%s' instantiates", p.legacy);
        if (direct)
            CHECK(direct->desc().uri == p.canonical,
                  "and it too rebuilds its descriptor as '%s'", p.canonical);
    }

    CHECK(reg.find("lattice:no-such-device") == nullptr,
          "the alias does not invent devices that never existed");
}

// ---------------------------------------------------------------------------
// Saturator
// ---------------------------------------------------------------------------

static void testSaturator(PluginRegistry& reg) {
    banner("Saturator");

    const PluginDesc* d = reg.find("nxtakt:saturator");
    CHECK(d != nullptr, "registry finds nxtakt:saturator");
    if (!d) return;
    CHECK(d->format == PluginFormat::Internal && d->kind == PluginKind::Effect,
          "descriptor: internal effect, %d in / %d out", d->audioIn, d->audioOut);

    auto sat = reg.instantiate(*d, kSR, kBlock);
    CHECK(sat != nullptr, "instantiate + prepare");
    if (!sat) return;

    const int pDrive = paramIndex(*sat, "Drive");
    const int pTrim  = paramIndex(*sat, "Output");
    const int pMix   = paramIndex(*sat, "Mix");
    CHECK(pDrive >= 0 && pTrim >= 0 && pMix >= 0, "params Drive/Output/Mix present");
    if (pDrive < 0 || pTrim < 0 || pMix < 0) return;
    CHECK(sat->paramInfo(pDrive).isLogarithmic && sat->paramInfo(pDrive).unit == "dB",
          "Drive is flagged logarithmic and carries a dB unit");

    Buf in, out;

    // 1. silence in, silence out.
    in.clear();
    out.clear();
    sat->process(in.p, out.p, 2, kBlock);
    CHECK(out.peak() == 0.f, "silence in -> silence out (peak %.9f)", (double)out.peak());

    // 2. a small sine at drive 0 comes back at unity. tanh(x) ~ x only for small
    //    x, so the test signal sits at -20 dBFS where the shaper is still linear
    //    to within a fraction of a dB.
    const f32 amp = 0.1f;
    for (int i = 0; i < kBlock; ++i) {
        const f32 s = amp * std::sin(6.2831853f * 220.f * (f32)i / (f32)kSR);
        in.l[(size_t)i] = in.r[(size_t)i] = s;
    }
    sat->setParam(pDrive, 0.f);
    sat->setParam(pTrim, 0.f);
    sat->setParam(pMix, 1.f);
    out.clear();
    sat->process(in.p, out.p, 2, kBlock);
    f32 maxErr = 0.f;
    for (int i = 0; i < kBlock; ++i)
        maxErr = std::fmax(maxErr, std::fabs(out.l[(size_t)i] - in.l[(size_t)i]));
    CHECK(maxErr < amp * 0.01f, "drive 0 dB is unity within 1%% (max err %.6f)", (double)maxErr);

    // 3. 24 dB of drive stays finite and bounded. The gain compensation is
    //    referenced to a 0.5 sine, so a hot input must not run away.
    sat->setParam(pDrive, 24.f);
    for (int i = 0; i < kBlock; ++i) {
        const f32 s = 0.9f * std::sin(6.2831853f * 220.f * (f32)i / (f32)kSR);
        in.l[(size_t)i] = in.r[(size_t)i] = s;
    }
    out.clear();
    sat->process(in.p, out.p, 2, kBlock);
    CHECK(out.finite(), "drive 24 dB output is finite");
    CHECK(out.peak() > 0.f && out.peak() <= 1.f,
          "drive 24 dB output is bounded (peak %.4f)", (double)out.peak());
    CHECK(out.peak() < 0.9f, "drive 24 dB is compensated, not just louder (peak %.4f)",
          (double)out.peak());

    // 4. in-place processing is legal per the contract.
    out.clear();
    for (int i = 0; i < kBlock; ++i) out.l[(size_t)i] = out.r[(size_t)i] = 0.3f;
    sat->process(out.p, out.p, 2, kBlock);
    CHECK(out.finite() && out.peak() > 0.f, "processes in place");

    // 5. bypass hands the input straight through.
    sat->setBypassed(true);
    out.clear();
    sat->process(in.p, out.p, 2, kBlock);
    f32 bypassErr = 0.f;
    for (int i = 0; i < kBlock; ++i)
        bypassErr = std::fmax(bypassErr, std::fabs(out.l[(size_t)i] - in.l[(size_t)i]));
    CHECK(bypassErr == 0.f, "bypass is a bit-exact copy");
    sat->setBypassed(false);
}

// ---------------------------------------------------------------------------
// Pulse
// ---------------------------------------------------------------------------

static void noteOn(PluginInstance& p, u8 key, u8 vel, int frame = 0) {
    const u8 msg[3] = { 0x90, key, vel };
    p.midi(msg, 3, frame);
}
static void noteOff(PluginInstance& p, u8 key, int frame = 0) {
    const u8 msg[3] = { 0x80, key, 0 };
    p.midi(msg, 3, frame);
}

// Runs `blocks` blocks and returns the loudest sample seen.
static f32 runFor(PluginInstance& p, Buf& out, int blocks, bool* allFinite = nullptr) {
    f32 peak = 0.f;
    if (allFinite) *allFinite = true;
    for (int b = 0; b < blocks; ++b) {
        out.clear();
        p.process(nullptr, out.p, 2, kBlock);
        peak = std::fmax(peak, out.peak());
        if (allFinite && !out.finite()) *allFinite = false;
    }
    return peak;
}

static void testPulse(PluginRegistry& reg) {
    banner("Pulse");

    const PluginDesc* d = reg.find("nxtakt:pulse");
    CHECK(d != nullptr, "registry finds nxtakt:pulse");
    if (!d) return;
    CHECK(d->kind == PluginKind::Instrument && d->hasMidiIn && d->audioOut == 2,
          "descriptor: internal instrument, midi in, %d out", d->audioOut);

    auto syn = reg.instantiate(*d, kSR, kBlock);
    CHECK(syn != nullptr, "instantiate + prepare");
    if (!syn) return;

    const int pAttack  = paramIndex(*syn, "Attack");
    const int pDecay   = paramIndex(*syn, "Decay");
    const int pRelease = paramIndex(*syn, "Release");
    const int pCutoff  = paramIndex(*syn, "Cutoff");
    const int pShape   = paramIndex(*syn, "Shape");
    CHECK(pAttack >= 0 && pDecay >= 0 && pRelease >= 0 && pCutoff >= 0 && pShape >= 0,
          "params Attack/Decay/Release/Cutoff/Shape present");
    if (pAttack < 0 || pDecay < 0 || pRelease < 0 || pCutoff < 0 || pShape < 0) return;

    Buf out;

    // 1. no MIDI at all is silence, not a stuck voice.
    CHECK(runFor(*syn, out, 8) == 0.f, "no midi -> silence");

    // 2. a note produces sound.
    syn->setParam(pAttack, 0.005f);
    syn->setParam(pDecay, 2.f);            // hold the note up while we look at it
    syn->setParam(pRelease, 0.05f);
    noteOn(*syn, 60, 100);
    bool fin = false;
    const f32 held = runFor(*syn, out, 20, &fin);
    CHECK(held > 0.01f, "note on -> non-zero output (peak %.4f)", (double)held);
    CHECK(fin, "held note stays finite");

    // 3. note off plus the release tail decays to silence.
    noteOff(*syn, 60);
    runFor(*syn, out, (int)(kSR / kBlock));            // one second of tail
    const f32 tail = runFor(*syn, out, 8);
    CHECK(tail == 0.f, "note off -> release tail reaches silence (residual %.9f)", (double)tail);

    // 4. eight simultaneous notes: all voices busy, output still finite.
    syn->setParam(pDecay, 2.f);
    for (int i = 0; i < 8; ++i) noteOn(*syn, (u8)(48 + i * 3), (u8)(80 + i * 5), i * 8);
    const f32 chord = runFor(*syn, out, 40, &fin);
    CHECK(fin, "8 simultaneous notes stay finite");
    CHECK(chord > 0.f, "8 simultaneous notes sound (peak %.4f)", (double)chord);

    // 5. a ninth note has to steal a voice rather than allocate one.
    noteOn(*syn, 84, 110);
    CHECK(runFor(*syn, out, 8, &fin) > 0.f && fin, "voice steal on the 9th note is clean");

    // 6. sweeping parameters from "the GUI" while the audio thread renders must
    //    not produce a NaN or an explosion, whatever order the values land in.
    bool sweepOk = true;
    for (int b = 0; b < 200; ++b) {
        const f32 t = (f32)b / 200.f;
        syn->setParam(pCutoff, lerpf(20.f, 18000.f, t));
        syn->setParam(pShape, t);
        syn->setParam(pAttack, lerpf(0.001f, 2.f, t));
        syn->setParam(pDecay, lerpf(2.f, 0.001f, t));
        syn->setParam(pRelease, lerpf(0.001f, 2.f, t));
        if ((b % 16) == 0) noteOn(*syn, (u8)(36 + (b % 60)), 100, b % kBlock);
        if ((b % 16) == 8) noteOff(*syn, (u8)(36 + ((b - 8) % 60)));
        out.clear();
        syn->process(nullptr, out.p, 2, kBlock);
        if (!out.finite() || out.peak() > 8.f) { sweepOk = false; break; }
    }
    CHECK(sweepOk, "param sweep while processing stays finite and bounded");

    // 7. all-notes-off (CC 123) clears everything, then silence returns.
    const u8 cc[3] = { 0xB0, 123, 0 };
    syn->midi(cc, 3, 0);
    syn->setParam(pRelease, 0.01f);
    runFor(*syn, out, (int)(kSR / kBlock));
    CHECK(runFor(*syn, out, 8) == 0.f, "CC 123 all-notes-off returns to silence");
}

// ---------------------------------------------------------------------------
// Shared harness for the effect devices
//
// Everything below MEASURES. "The EQ boosts" is not a test; "the EQ boosts by
// 11.9 dB at the frequency it was asked to boost at, and by 0.1 dB two decades
// below it" is. The measurement tool is a single-bin DFT at the probe frequency
// over a whole number of cycles, which is exact for a steady sine and needs no
// window and no FFT.
// ---------------------------------------------------------------------------

// The devices under test are the stock effects: stereo in, stereo out, no MIDI.
static const char* kEffectUris[] = {
    "nxtakt:saturator", "nxtakt:eq3", "nxtakt:compressor",
    "nxtakt:delay", "nxtakt:reverb", "nxtakt:autofilter", "nxtakt:chorus",
    "nxtakt:limiter", "nxtakt:utility",
};

// Runs a steady sine through the device and returns the output/input magnitude
// at that exact frequency, in dB. `cycles` is chosen by the caller so that the
// measurement window is a whole number of periods.
static f64 probeGainDb(PluginInstance& p, f64 freq, f32 amp, int cycles) {
    Buf in, out;
    const f64 w = 6.283185307179586 * freq / kSR;
    u64 n = 0;

    auto fill = [&](int k) {
        for (int i = 0; i < k; ++i) {
            const f32 s = amp * (f32)std::sin(w * (f64)(n + (u64)i));
            in.l[(size_t)i] = in.r[(size_t)i] = s;
        }
    };

    // Settle: filters, smoothers and detectors all need to reach steady state
    // before the number means anything.
    for (int b = 0; b < 40; ++b) { fill(kBlock); out.clear(); p.process(in.p, out.p, 2, kBlock); n += (u64)kBlock; }

    const int N = (int)std::llround((f64)cycles * kSR / freq);
    f64 re = 0.0, im = 0.0;
    int done = 0;
    while (done < N) {
        const int k = (N - done) < kBlock ? (N - done) : kBlock;
        fill(k);
        out.clear();
        p.process(in.p, out.p, 2, k);
        for (int i = 0; i < k; ++i) {
            const f64 ph = w * (f64)(n + (u64)i);
            re += (f64)out.l[(size_t)i] * std::cos(ph);
            im += (f64)out.l[(size_t)i] * std::sin(ph);
        }
        n += (u64)k;
        done += k;
    }
    const f64 mag = 2.0 * std::sqrt(re * re + im * im) / (f64)N;
    if (mag <= 1e-12 || amp <= 0.f) return -200.0;
    return 20.0 * std::log10(mag / (f64)amp);
}

// The four properties every stock effect owes the user, checked the same way
// for all of them so a device added later cannot quietly skip one.
static void testEffectContract(PluginRegistry& reg) {
    banner("stock effects: the common contract");

    for (const char* uri : kEffectUris) {
        const PluginDesc* d = reg.find(uri);
        CHECK(d != nullptr, "%s: in the registry", uri);
        if (!d) continue;
        CHECK(d->format == PluginFormat::Internal && d->kind == PluginKind::Effect &&
              d->audioIn == 2 && d->audioOut == 2 && !d->hasMidiIn,
              "%s: stereo effect descriptor", uri);

        auto fx = reg.instantiate(*d, kSR, kBlock);
        CHECK(fx != nullptr, "%s: instantiate + prepare", uri);
        if (!fx) continue;

        CHECK(fx->paramCount() == d->paramCount,
              "%s: descriptor param count matches the instance (%d)", uri, fx->paramCount());

        // Every parameter has to be automatable through the realtime path, or
        // the engine greys its lane out. There is no reason for a stock device
        // to have one that is not.
        bool rtOk = true;
        for (int i = 0; i < fx->paramCount(); ++i)
            if (!fx->setParamRT(i, fx->paramInfo(i).def)) rtOk = false;
        CHECK(rtOk, "%s: every parameter accepts a realtime write", uri);

        // 1. Silence in, silence out. A fresh instance has zeroed state, so
        //    even the delay and the reverb owe an exact zero here -- their
        //    tails can only contain what was put into them.
        Buf in, out;
        f32 residue = 0.f;
        for (int b = 0; b < 8; ++b) {
            out.clear();
            fx->process(in.p, out.p, 2, kBlock);
            residue = std::fmax(residue, out.peak());
        }
        CHECK(residue == 0.f, "%s: silence in -> silence out (%.9f)", uri, (double)residue);

        // 2. A sine sweeping 20 Hz -> 18 kHz stays finite. This is the shape
        //    that finds a filter that goes unstable at one end of its range.
        bool fin = true;
        f32 peak = 0.f;
        f64 ph = 0.0;
        for (int b = 0; b < 400; ++b) {
            const f64 t = (f64)b / 400.0;
            const f64 f = 20.0 * std::pow(900.0, t);          // 20 Hz .. 18 kHz
            for (int i = 0; i < kBlock; ++i) {
                ph += 6.283185307179586 * f / kSR;
                in.l[(size_t)i] = in.r[(size_t)i] = 0.5f * (f32)std::sin(ph);
            }
            out.clear();
            fx->process(in.p, out.p, 2, kBlock);
            if (!out.finite()) { fin = false; break; }
            peak = std::fmax(peak, out.peak());
        }
        CHECK(fin, "%s: swept sine stays finite", uri);
        CHECK(peak < 8.f, "%s: swept sine stays bounded (peak %.3f)", uri, (double)peak);

        // 3. Every parameter swept end to end WHILE processing. Each parameter
        //    gets its own pass so a fault is attributable, and the pass runs
        //    both directions because a device can be fine going up and unstable
        //    coming down (a delay time shrinking under feedback, say).
        Noise ns;
        bool sweepOk = true;
        const char* badParam = "";
        for (int pi = 0; pi < fx->paramCount() && sweepOk; ++pi) {
            const ParamInfo& info = fx->paramInfo(pi);
            for (int b = 0; b < 120; ++b) {
                f32 t = (f32)b / 60.f;
                if (t > 1.f) t = 2.f - t;                     // up, then back down
                fx->setParam(pi, lerpf(info.min, info.max, t));
                for (int i = 0; i < kBlock; ++i)
                    in.l[(size_t)i] = in.r[(size_t)i] = 0.25f * ns.next();
                out.clear();
                fx->process(in.p, out.p, 2, kBlock);
                if (!out.finite() || out.peak() > 32.f) {
                    sweepOk = false;
                    badParam = info.name.c_str();
                    break;
                }
            }
            fx->setParam(pi, info.def);
        }
        CHECK(sweepOk, "%s: every parameter sweeps during processing without NaN%s%s",
              uri, sweepOk ? "" : " -- failed on ", badParam);

        // 4. ...and it is still a working device afterwards, not a silenced one.
        for (int i = 0; i < kBlock; ++i)
            in.l[(size_t)i] = in.r[(size_t)i] = 0.25f * (f32)std::sin(6.2831853 * 440.0 * i / kSR);
        f32 after = 0.f;
        for (int b = 0; b < 8; ++b) {
            out.clear();
            fx->process(in.p, out.p, 2, kBlock);
            after = std::fmax(after, out.peak());
        }
        CHECK(after > 0.01f, "%s: still passes audio after the sweep (peak %.4f)",
              uri, (double)after);
    }
}

// ---------------------------------------------------------------------------
// EQ Three
// ---------------------------------------------------------------------------

static void testEq3(PluginRegistry& reg) {
    banner("EQ Three");

    const PluginDesc* d = reg.find("nxtakt:eq3");
    CHECK(d != nullptr, "registry finds nxtakt:eq3");
    if (!d) return;

    auto eq = reg.instantiate(*d, kSR, kBlock);
    CHECK(eq != nullptr, "instantiate + prepare");
    if (!eq) return;

    const int pLoF = paramIndex(*eq, "Low Freq");
    const int pLoG = paramIndex(*eq, "Low Gain");
    const int pMidF = paramIndex(*eq, "Mid Freq");
    const int pMidG = paramIndex(*eq, "Mid Gain");
    const int pMidQ = paramIndex(*eq, "Mid Q");
    const int pHiF = paramIndex(*eq, "High Freq");
    const int pHiG = paramIndex(*eq, "High Gain");
    CHECK(pLoF >= 0 && pLoG >= 0 && pMidF >= 0 && pMidG >= 0 && pMidQ >= 0 &&
          pHiF >= 0 && pHiG >= 0, "all seven parameters present");
    if (pLoG < 0 || pMidG < 0 || pHiG < 0) return;

    // 1. Defaults are flat, and not approximately: at 0 dB every RBJ section
    //    collapses to b0 = 1, b1 = a1, b2 = a2, which is an exact passthrough
    //    in transposed direct form II. The device on a channel doing nothing
    //    has to do NOTHING.
    Buf in, out;
    for (int i = 0; i < kBlock; ++i)
        in.l[(size_t)i] = in.r[(size_t)i] = 0.4f * (f32)std::sin(6.2831853 * 997.0 * i / kSR);
    out.clear();
    eq->process(in.p, out.p, 2, kBlock);
    f32 flatErr = 0.f;
    for (int i = 0; i < kBlock; ++i)
        flatErr = std::fmax(flatErr, std::fabs(out.l[(size_t)i] - in.l[(size_t)i]));
    CHECK(flatErr < 1e-6f, "defaults are unity (max err %.9f)", (double)flatErr);

    // 2. A measured mid boost. +12 dB at 1 kHz, Q 1: the peak of an RBJ
    //    peaking section sits exactly on the centre frequency at exactly the
    //    requested gain, so this is a number with a right answer.
    eq->setParam(pMidF, 1000.f);
    eq->setParam(pMidQ, 1.f);
    eq->setParam(pMidG, 12.f);
    const f64 at1k = probeGainDb(*eq, 1000.0, 0.2f, 100);
    CHECK(std::fabs(at1k - 12.0) < 0.5, "+12 dB at 1 kHz measures %.2f dB", at1k);

    // ...and it is a BAND, not a shelf: two octaves out the boost is nearly
    // gone. (A Q of 1 gives a bandwidth of ~1.4 octaves, so 250 Hz should be
    // down around a dB or two.)
    const f64 at250 = probeGainDb(*eq, 250.0, 0.2f, 25);
    CHECK(at250 < 3.0 && at250 > -0.5, "the boost is local: %.2f dB at 250 Hz", at250);

    // A cut of the same size is symmetric, which is the property the cookbook
    // formulas exist to guarantee.
    eq->setParam(pMidG, -12.f);
    const f64 cut1k = probeGainDb(*eq, 1000.0, 0.2f, 100);
    CHECK(std::fabs(cut1k + 12.0) < 0.5, "-12 dB at 1 kHz measures %.2f dB", cut1k);
    eq->setParam(pMidG, 0.f);

    // 3. Shelves. An RBJ shelf with S = 1 passes through exactly half its gain
    //    at the corner frequency, so a +12 dB low shelf at 100 Hz measures
    //    +6 dB at 100 Hz -- and the mid and top are untouched.
    eq->setParam(pLoF, 100.f);
    eq->setParam(pLoG, 12.f);
    const f64 lowCorner = probeGainDb(*eq, 100.0, 0.2f, 20);
    const f64 lowDeep   = probeGainDb(*eq, 30.0, 0.2f, 10);
    const f64 lowFar    = probeGainDb(*eq, 2000.0, 0.2f, 100);
    CHECK(std::fabs(lowCorner - 6.0) < 0.6, "low shelf: %.2f dB at its 100 Hz corner", lowCorner);
    CHECK(lowDeep > 10.0, "low shelf: %.2f dB at 30 Hz (approaching the full +12)", lowDeep);
    CHECK(std::fabs(lowFar) < 1.0, "low shelf leaves 2 kHz alone (%.2f dB)", lowFar);
    eq->setParam(pLoG, 0.f);

    eq->setParam(pHiF, 8000.f);
    eq->setParam(pHiG, 12.f);
    const f64 hiCorner = probeGainDb(*eq, 8000.0, 0.2f, 400);
    const f64 hiLow    = probeGainDb(*eq, 200.0, 0.2f, 20);
    CHECK(std::fabs(hiCorner - 6.0) < 0.6, "high shelf: %.2f dB at its 8 kHz corner", hiCorner);
    CHECK(std::fabs(hiLow) < 1.0, "high shelf leaves 200 Hz alone (%.2f dB)", hiLow);
    eq->setParam(pHiG, 0.f);

    // 4. All three bands boosted at once. The sections are cascaded, so their
    //    dB responses add -- and at 1 kHz, with the shelves parked at 100 Hz
    //    and 8 kHz, the sum is the mid band alone. That the shelves contribute
    //    nothing here is the point: three bands that all bleed into the middle
    //    are one badly-tuned band.
    eq->setParam(pLoG, 6.f);
    eq->setParam(pMidG, 6.f);
    eq->setParam(pHiG, 6.f);
    const f64 all1k = probeGainDb(*eq, 1000.0, 0.2f, 100);
    const f64 all30 = probeGainDb(*eq, 30.0, 0.2f, 10);
    CHECK(std::fabs(all1k - 6.0) < 0.5,
          "three +6 dB bands leave 1 kHz to the mid alone (%.2f dB)", all1k);
    CHECK(all30 > 5.0 && all30 < 7.5,
          "and 30 Hz to the low shelf alone (%.2f dB)", all30);

    // 5. Sweeping the mid frequency across its whole range under a full-scale
    //    input: the coefficient glide must not ring, and the state must not
    //    escape. 15 dB of boost is the worst case the device allows.
    eq->setParam(pLoG, 0.f);
    eq->setParam(pHiG, 0.f);
    eq->setParam(pMidG, 15.f);
    eq->setParam(pMidQ, 6.f);
    bool sweepFin = true;
    f32 sweepPeak = 0.f;
    for (int b = 0; b < 240; ++b) {
        const f32 t = (f32)b / 120.f;
        eq->setParam(pMidF, lerpf(200.f, 8000.f, t > 1.f ? 2.f - t : t));
        for (int i = 0; i < kBlock; ++i)
            in.l[(size_t)i] = in.r[(size_t)i] = 0.9f * (f32)std::sin(6.2831853 * 800.0 * (b * kBlock + i) / kSR);
        out.clear();
        eq->process(in.p, out.p, 2, kBlock);
        if (!out.finite()) { sweepFin = false; break; }
        sweepPeak = std::fmax(sweepPeak, out.peak());
    }
    CHECK(sweepFin, "sweeping Mid Freq at +15 dB / Q 6 stays finite");
    CHECK(sweepPeak < 8.f, "and bounded (peak %.3f, +15 dB of 0.9 is 5.06)", (double)sweepPeak);
}

// ---------------------------------------------------------------------------
// Compressor
// ---------------------------------------------------------------------------

// Feeds a steady sine and returns the peak of the last block, in dBFS.
static f64 steadyPeakDb(PluginInstance& p, f64 freq, f32 amp, int blocks) {
    Buf in, out;
    f32 peak = 0.f;
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < kBlock; ++i) {
            const f32 s = amp * (f32)std::sin(6.2831853 * freq * (b * kBlock + i) / kSR);
            in.l[(size_t)i] = in.r[(size_t)i] = s;
        }
        out.clear();
        p.process(in.p, out.p, 2, kBlock);
        if (b >= blocks - 4) peak = std::fmax(peak, out.peak());
    }
    return 20.0 * std::log10(std::fmax((f64)peak, 1e-9));
}

static void testCompressor(PluginRegistry& reg) {
    banner("Compressor");

    const PluginDesc* d = reg.find("nxtakt:compressor");
    CHECK(d != nullptr, "registry finds nxtakt:compressor");
    if (!d) return;

    auto cp = reg.instantiate(*d, kSR, kBlock);
    CHECK(cp != nullptr, "instantiate + prepare");
    if (!cp) return;

    const int pThr = paramIndex(*cp, "Threshold");
    const int pRat = paramIndex(*cp, "Ratio");
    const int pAtk = paramIndex(*cp, "Attack");
    const int pRel = paramIndex(*cp, "Release");
    const int pKne = paramIndex(*cp, "Knee");
    const int pMak = paramIndex(*cp, "Makeup");
    const int pGr  = paramIndex(*cp, "Gain Reduction");
    CHECK(pThr >= 0 && pRat >= 0 && pAtk >= 0 && pRel >= 0 && pKne >= 0 &&
          pMak >= 0 && pGr >= 0, "all seven parameters present, including the readout");
    if (pThr < 0 || pRat < 0 || pGr < 0) return;
    note("Gain Reduction is an output value on an ordinary parameter: ParamInfo "
         "has no read-only flag, so the device writes it and the UI reads it.");

    Buf in, out;

    // 1. Below the threshold (and below the knee) the device is unity. This is
    //    what makes it safe to leave on a channel.
    cp->setParam(pThr, -18.f);
    cp->setParam(pKne, 6.f);
    cp->setParam(pRat, 4.f);
    cp->setParam(pMak, 0.f);
    for (int i = 0; i < kBlock; ++i)
        in.l[(size_t)i] = in.r[(size_t)i] = 0.03f * (f32)std::sin(6.2831853 * 220.0 * i / kSR);
    for (int b = 0; b < 8; ++b) { out.clear(); cp->process(in.p, out.p, 2, kBlock); }
    f32 unityErr = 0.f;
    for (int i = 0; i < kBlock; ++i)
        unityErr = std::fmax(unityErr, std::fabs(out.l[(size_t)i] - in.l[(size_t)i]));
    CHECK(unityErr < 1e-6f, "-30 dBFS under a -18 dB threshold is untouched (err %.9f)",
          (double)unityErr);
    CHECK(cp->getParam(pGr) < 0.01f, "and the readout says 0 dB of reduction (%.4f)",
          (double)cp->getParam(pGr));

    // 2. The ratio, measured. Hard knee, fast attack, slow release so the
    //    envelope sits on the peak; then the output level over the threshold
    //    divided into the input level over the threshold IS the ratio.
    cp->setParam(pKne, 0.f);
    cp->setParam(pThr, -20.f);
    cp->setParam(pAtk, 1.f);
    cp->setParam(pRel, 300.f);
    struct { f32 ratio; f64 inDb; } kCases[] = { {2.f, -10.0}, {4.f, -10.0}, {8.f, -4.0} };
    for (const auto& c : kCases) {
        cp->setParam(pRat, c.ratio);
        const f32 amp = (f32)std::pow(10.0, c.inDb / 20.0);
        const f64 outDb = steadyPeakDb(*cp, 220.0, amp, 60);
        const f64 measured = (c.inDb - (-20.0)) / (outDb - (-20.0));
        CHECK(std::fabs(measured - (f64)c.ratio) < 0.15 * (f64)c.ratio,
              "%.0f:1 measures %.2f:1 (%.1f dBFS in -> %.2f dBFS out)",
              (double)c.ratio, measured, c.inDb, outDb);
    }

    // 3. Makeup gain is exactly what it says.
    cp->setParam(pRat, 4.f);
    const f64 noMakeup = steadyPeakDb(*cp, 220.0, 0.316f, 60);
    cp->setParam(pMak, 6.f);
    const f64 withMakeup = steadyPeakDb(*cp, 220.0, 0.316f, 60);
    CHECK(std::fabs((withMakeup - noMakeup) - 6.0) < 0.3,
          "6 dB of makeup adds %.2f dB", withMakeup - noMakeup);
    cp->setParam(pMak, 0.f);

    // 4. Attack and release TIMES, measured off the readout. The envelope is a
    //    one-pole in the dB domain, so "attack" is the time to cover 63% of the
    //    distance to the target reduction -- that is what the number on the
    //    knob promises, and it is checkable to a couple of milliseconds by
    //    running short blocks and reading the meter after each.
    const int kSmall = 64;                                  // 1.33 ms resolution
    auto measureAttack = [&](f32 attackMs) {
        cp->prepare(kSR, kBlock);                           // zero the envelope
        cp->setParam(pThr, -30.f);
        cp->setParam(pKne, 0.f);
        cp->setParam(pRat, 8.f);
        cp->setParam(pAtk, attackMs);
        cp->setParam(pRel, 500.f);
        Buf b(kSmall), o(kSmall);
        for (int i = 0; i < kSmall; ++i) b.l[(size_t)i] = b.r[(size_t)i] = 0.7f;   // DC burst
        f64 t63 = -1.0, final_ = 0.0;
        for (int k = 0; k < 400; ++k) {
            o.clear();
            cp->process(b.p, o.p, 2, kSmall);
            final_ = (f64)cp->getParam(pGr);
        }
        const f64 target = final_;
        cp->prepare(kSR, kBlock);
        cp->setParam(pThr, -30.f);
        cp->setParam(pKne, 0.f);
        cp->setParam(pRat, 8.f);
        cp->setParam(pAtk, attackMs);
        cp->setParam(pRel, 500.f);
        for (int k = 0; k < 400 && t63 < 0.0; ++k) {
            o.clear();
            cp->process(b.p, o.p, 2, kSmall);
            if ((f64)cp->getParam(pGr) >= 0.632 * target)
                t63 = 1000.0 * (f64)((k + 1) * kSmall) / kSR;
        }
        return t63;
    };
    const f64 a5  = measureAttack(5.f);
    const f64 a50 = measureAttack(50.f);
    CHECK(a5 > 3.0 && a5 < 9.0, "a 5 ms attack reaches 63%% of its reduction in %.2f ms", a5);
    CHECK(a50 > 42.0 && a50 < 62.0, "a 50 ms attack reaches 63%% in %.2f ms", a50);
    CHECK(a50 > a5 * 5.0, "and the two times are in the ratio the knob claims (%.1fx)",
          a50 / a5);

    // Release: hold the burst until the reduction settles, drop to silence, and
    // time the fall to 37% of where it was.
    {
        cp->prepare(kSR, kBlock);
        cp->setParam(pThr, -30.f);
        cp->setParam(pKne, 0.f);
        cp->setParam(pRat, 8.f);
        cp->setParam(pAtk, 1.f);
        cp->setParam(pRel, 100.f);
        Buf loud(kSmall), quiet(kSmall), o(kSmall);
        for (int i = 0; i < kSmall; ++i) loud.l[(size_t)i] = loud.r[(size_t)i] = 0.7f;
        for (int k = 0; k < 400; ++k) { o.clear(); cp->process(loud.p, o.p, 2, kSmall); }
        const f64 held = (f64)cp->getParam(pGr);
        f64 t37 = -1.0;
        for (int k = 0; k < 800 && t37 < 0.0; ++k) {
            o.clear();
            cp->process(quiet.p, o.p, 2, kSmall);
            if ((f64)cp->getParam(pGr) <= 0.368 * held)
                t37 = 1000.0 * (f64)((k + 1) * kSmall) / kSR;
        }
        CHECK(held > 10.0, "the burst is held down by %.2f dB", held);
        CHECK(t37 > 80.0 && t37 < 130.0,
              "a 100 ms release falls to 37%% of that in %.2f ms", t37);
    }

    // 5. Stereo link: one gain for both channels. A loud left and a quiet right
    //    must come out with the SAME gain applied, or the image walks.
    {
        cp->prepare(kSR, kBlock);
        cp->setParam(pThr, -24.f);
        cp->setParam(pKne, 0.f);
        cp->setParam(pRat, 6.f);
        cp->setParam(pAtk, 1.f);
        cp->setParam(pRel, 400.f);
        Buf s(kBlock), o(kBlock);
        for (int b = 0; b < 40; ++b) {
            for (int i = 0; i < kBlock; ++i) {
                const f64 ph = 6.2831853 * 220.0 * (b * kBlock + i) / kSR;
                s.l[(size_t)i] = 0.8f * (f32)std::sin(ph);
                s.r[(size_t)i] = 0.05f * (f32)std::sin(ph);
            }
            o.clear();
            cp->process(s.p, o.p, 2, kBlock);
        }
        f64 gl = 0.0, gr = 0.0;
        for (int i = 0; i < kBlock; ++i) {
            if (std::fabs(s.l[(size_t)i]) > 0.4f) {
                gl = (f64)o.l[(size_t)i] / (f64)s.l[(size_t)i];
                gr = (f64)o.r[(size_t)i] / (f64)s.r[(size_t)i];
                break;
            }
        }
        CHECK(gl > 0.0 && std::fabs(gl - gr) < 1e-3,
              "one gain for both channels (L %.5f, R %.5f)", gl, gr);
        CHECK(gl < 0.9, "and the loud side really is being reduced (%.2f dB)",
              20.0 * std::log10(std::fmax(gl, 1e-9)));
    }

    // 6. The readout tracks reality: it is the worst reduction in the block,
    //    and it releases back to zero when the signal does. (The release is an
    //    exponential in dB, so "zero" is a limit -- 20 dB of reduction with a
    //    50 ms release needs a good half second to get under a tenth of a dB,
    //    and reading the meter before then is reading the release, not a bug.)
    {
        cp->setParam(pRel, 50.f);
        Buf q(kBlock), o(kBlock);
        for (int k = 0; k < 200; ++k) { o.clear(); cp->process(q.p, o.p, 2, kBlock); }
        CHECK(cp->getParam(pGr) < 0.05f, "the readout returns to 0 dB on silence (%.4f)",
              (double)cp->getParam(pGr));
    }
}

// ---------------------------------------------------------------------------
// Delay
// ---------------------------------------------------------------------------

// Sends a single unit impulse (both channels) and captures `frames` samples of
// output into l/r. Fresh instance in, so the delay time is snapped rather than
// glided and the echo lands where the maths says it does.
static void impulseResponse(PluginInstance& p, int frames,
                            std::vector<f32>& l, std::vector<f32>& r) {
    l.assign((size_t)frames, 0.f);
    r.assign((size_t)frames, 0.f);
    Buf in, out;
    int done = 0;
    bool first = true;
    while (done < frames) {
        const int k = (frames - done) < kBlock ? (frames - done) : kBlock;
        in.clear();
        if (first) { in.l[0] = in.r[0] = 1.f; first = false; }
        out.clear();
        p.process(in.p, out.p, 2, k);
        for (int i = 0; i < k; ++i) {
            l[(size_t)(done + i)] = out.l[(size_t)i];
            r[(size_t)(done + i)] = out.r[(size_t)i];
        }
        done += k;
    }
}

static void testDelay(PluginRegistry& reg) {
    banner("Delay");

    const PluginDesc* d = reg.find("nxtakt:delay");
    CHECK(d != nullptr, "registry finds nxtakt:delay");
    if (!d) return;

    {
        auto probe = reg.instantiate(*d, kSR, kBlock);
        if (probe) {
            CHECK(paramIndex(*probe, "Tempo") >= 0,
                  "the device carries its own Tempo parameter");
            note("PluginInstance has no transport channel, so tempo sync runs off a "
                 "device parameter (120 BPM default). See the comment on class Delay "
                 "for the host.h addition that would replace it.");
            const int ps = paramIndex(*probe, "Sync");
            const int pd = paramIndex(*probe, "Division");
            CHECK(ps >= 0 && probe->paramInfo(ps).isBool, "Sync is flagged as a switch");
            CHECK(pd >= 0 && probe->paramInfo(pd).isInt, "Division is flagged as stepped");
        }
    }

    // 1. Free mode: an impulse comes back exactly where it was sent to.
    {
        auto dl = reg.instantiate(*d, kSR, kBlock);
        CHECK(dl != nullptr, "instantiate + prepare");
        if (!dl) return;
        dl->setParam(paramIndex(*dl, "Sync"), 0.f);
        dl->setParam(paramIndex(*dl, "Time"), 100.f);          // ms
        dl->setParam(paramIndex(*dl, "Feedback"), 0.5f);
        dl->setParam(paramIndex(*dl, "Tone"), 18000.f);
        dl->setParam(paramIndex(*dl, "Dry/Wet"), 1.f);
        dl->setParam(paramIndex(*dl, "Ping Pong"), 0.f);

        std::vector<f32> l, r;
        impulseResponse(*dl, 24000, l, r);

        const int expect = (int)(0.100 * kSR);                 // 4800
        int at = 0;
        f32 best = 0.f;
        for (int i = 100; i < 8000; ++i)
            if (std::fabs(l[(size_t)i]) > best) { best = std::fabs(l[(size_t)i]); at = i; }
        CHECK(at == expect, "a 100 ms echo lands at sample %d (expected %d)", at, expect);
        CHECK(std::fabs(best - 1.f) < 0.001f,
              "and at full amplitude, dry/wet at 100%% (%.5f)", (double)best);

        // The feedback path is lowpassed, which smears each repeat in time but
        // leaves its total (a one-pole has unity DC gain) equal to the feedback
        // fraction. So the ENERGY of repeat n is fb^n, measured as a sum.
        auto echoSum = [&](int centre) {
            f64 s = 0.0;
            for (int i = centre - 200; i < centre + 3000 && i < (int)l.size(); ++i)
                if (i >= 0) s += (f64)l[(size_t)i];
            return s;
        };
        const f64 e1 = echoSum(expect);
        const f64 e2 = echoSum(2 * expect);
        const f64 e3 = echoSum(3 * expect);
        CHECK(std::fabs(e2 / e1 - 0.5) < 0.05,
              "the second repeat is %.3f of the first at 50%% feedback", e2 / e1);
        CHECK(std::fabs(e3 / e2 - 0.5) < 0.05,
              "and the third is %.3f of the second", e3 / e2);
    }

    // 2. Sync mode: 1/8 at 120 BPM is 250 ms, whoever is telling us the tempo.
    {
        auto dl = reg.instantiate(*d, kSR, kBlock);
        if (!dl) return;
        dl->setParam(paramIndex(*dl, "Sync"), 1.f);
        dl->setParam(paramIndex(*dl, "Division"), 3.f);        // 1/8
        dl->setParam(paramIndex(*dl, "Tempo"), 120.f);
        dl->setParam(paramIndex(*dl, "Feedback"), 0.f);
        dl->setParam(paramIndex(*dl, "Dry/Wet"), 1.f);

        std::vector<f32> l, r;
        impulseResponse(*dl, 24000, l, r);
        int at = 0;
        f32 best = 0.f;
        for (int i = 100; i < 24000; ++i)
            if (std::fabs(l[(size_t)i]) > best) { best = std::fabs(l[(size_t)i]); at = i; }
        CHECK(at == 12000, "1/8 at 120 BPM lands at sample %d (expected 12000 = 250 ms)", at);

        // ...and the tempo really is the divisor: same division, 60 BPM.
        auto slow = reg.instantiate(*d, kSR, kBlock);
        slow->setParam(paramIndex(*slow, "Sync"), 1.f);
        slow->setParam(paramIndex(*slow, "Division"), 3.f);
        slow->setParam(paramIndex(*slow, "Tempo"), 60.f);
        slow->setParam(paramIndex(*slow, "Feedback"), 0.f);
        slow->setParam(paramIndex(*slow, "Dry/Wet"), 1.f);
        impulseResponse(*slow, 48000, l, r);
        at = 0; best = 0.f;
        for (int i = 100; i < 48000; ++i)
            if (std::fabs(l[(size_t)i]) > best) { best = std::fabs(l[(size_t)i]); at = i; }
        CHECK(at == 24000, "the same division at 60 BPM lands at %d (expected 24000)", at);
    }

    // 2b. The transport outranks the Tempo parameter. The engine pushes
    // setTransport() before every process(); a device that has seen one must
    // follow it and ignore the parameter — the parameter exists for hosts that
    // never push (this suite, everywhere else in it). The Tempo param is set
    // to a WRONG value on purpose: if the device is still reading it, the echo
    // lands at 12000 and this fails, which is the regression this test is for.
    {
        auto dl = reg.instantiate(*d, kSR, kBlock);
        if (!dl) return;
        dl->setParam(paramIndex(*dl, "Sync"), 1.f);
        dl->setParam(paramIndex(*dl, "Division"), 3.f);        // 1/8
        dl->setParam(paramIndex(*dl, "Tempo"), 120.f);         // the decoy
        dl->setParam(paramIndex(*dl, "Feedback"), 0.f);
        dl->setParam(paramIndex(*dl, "Dry/Wet"), 1.f);

        std::vector<f32> l(48000, 0.f), r(48000, 0.f);
        std::vector<f32> inL(48000, 0.f), inR(48000, 0.f);
        inL[0] = inR[0] = 1.f;
        for (int i = 0; i < 48000; i += kBlock) {
            // 48000 is not a multiple of kBlock; the last block is short.
            const int n = 48000 - i < kBlock ? 48000 - i : kBlock;
            const f32* in[2]  = { inL.data() + i, inR.data() + i };
            f32* out[2] = { l.data() + i, r.data() + i };
            dl->setTransport(60.0, 0.0, true);                 // what the engine does
            dl->process(in, out, 2, n);
        }
        int at = 0; f32 best = 0.f;
        for (int i = 100; i < 48000; ++i)
            if (std::fabs(l[(size_t)i]) > best) { best = std::fabs(l[(size_t)i]); at = i; }
        CHECK(at == 24000,
              "pushed transport (60 BPM) outranks the Tempo param (120): echo at %d (expected 24000)",
              at);
    }

    // 3. Ping-pong alternates sides rather than spreading.
    {
        auto dl = reg.instantiate(*d, kSR, kBlock);
        if (!dl) return;
        dl->setParam(paramIndex(*dl, "Sync"), 0.f);
        dl->setParam(paramIndex(*dl, "Time"), 100.f);
        dl->setParam(paramIndex(*dl, "Feedback"), 0.6f);
        dl->setParam(paramIndex(*dl, "Tone"), 18000.f);
        dl->setParam(paramIndex(*dl, "Dry/Wet"), 1.f);
        dl->setParam(paramIndex(*dl, "Ping Pong"), 1.f);

        std::vector<f32> l, r;
        impulseResponse(*dl, 24000, l, r);
        auto win = [](const std::vector<f32>& v, int a, int b) {
            f64 s = 0.0;
            for (int i = a; i < b && i < (int)v.size(); ++i) s += std::fabs((f64)v[(size_t)i]);
            return s;
        };
        const f64 l1 = win(l, 4700, 7000), r1 = win(r, 4700, 7000);
        const f64 l2 = win(l, 9500, 12000), r2 = win(r, 9500, 12000);
        CHECK(l1 > 0.5 && r1 < 0.01 * l1,
              "the first repeat is left only (L %.4f, R %.4f)", l1, r1);
        CHECK(r2 > 0.1 && l2 < 0.01 * r2,
              "the second repeat is right only (L %.4f, R %.4f)", l2, r2);
    }

    // 4. Fully dry is bit-exact, which is what makes the device safe to leave
    //    in a chain at zero.
    {
        auto dl = reg.instantiate(*d, kSR, kBlock);
        if (!dl) return;
        dl->setParam(paramIndex(*dl, "Dry/Wet"), 0.f);
        Buf in, out;
        f32 err = 0.f;
        for (int b = 0; b < 12; ++b) {
            for (int i = 0; i < kBlock; ++i)
                in.l[(size_t)i] = in.r[(size_t)i] = 0.5f * (f32)std::sin(6.2831853 * 330.0 * (b * kBlock + i) / kSR);
            out.clear();
            dl->process(in.p, out.p, 2, kBlock);
            if (b >= 4)
                for (int i = 0; i < kBlock; ++i)
                    err = std::fmax(err, std::fabs(out.l[(size_t)i] - in.l[(size_t)i]));
        }
        CHECK(err < 1e-6f, "dry/wet at 0 is a bit-exact copy (err %.9f)", (double)err);
    }

    // 5. Maximum feedback does not run away. 0.95 into a unity-DC-gain filter
    //    settles at 1/(1-0.95) = 20x the input, and no higher.
    {
        auto dl = reg.instantiate(*d, kSR, kBlock);
        if (!dl) return;
        dl->setParam(paramIndex(*dl, "Sync"), 0.f);
        dl->setParam(paramIndex(*dl, "Time"), 20.f);
        dl->setParam(paramIndex(*dl, "Feedback"), 0.95f);
        dl->setParam(paramIndex(*dl, "Dry/Wet"), 1.f);
        Buf in, out;
        for (int i = 0; i < kBlock; ++i) in.l[(size_t)i] = in.r[(size_t)i] = 0.05f;
        f32 peak = 0.f;
        bool fin = true;
        for (int b = 0; b < 400; ++b) {
            out.clear();
            dl->process(in.p, out.p, 2, kBlock);
            if (!out.finite()) { fin = false; break; }
            peak = std::fmax(peak, out.peak());
        }
        CHECK(fin, "0.95 feedback on DC stays finite");
        CHECK(peak < 1.5f, "and settles near the predicted 20x (peak %.3f of 0.05)", (double)peak);
    }
}

// ---------------------------------------------------------------------------
// Reverb
// ---------------------------------------------------------------------------

// Excites the tank with a noise burst, then measures the decay of the tail in
// 25 ms windows. Returns the RT60 in seconds (extrapolated from the first
// 20 dB of decay, which is what an acoustician does too -- the last 40 dB are
// buried in whatever else is going on) and fills `env` with the window RMS.
static f64 measureRt60(PluginInstance& p, std::vector<f64>& env) {
    const int kWin = (int)(0.025 * kSR);
    Buf in(kWin), out(kWin);
    Noise ns;

    // 300 ms of noise into the tank.
    for (int w = 0; w < 12; ++w) {
        for (int i = 0; i < kWin; ++i) in.l[(size_t)i] = in.r[(size_t)i] = 0.25f * ns.next();
        out.clear();
        p.process(in.p, out.p, 2, kWin);
    }

    env.clear();
    in.clear();
    for (int w = 0; w < 400; ++w) {                    // up to 10 s of tail
        out.clear();
        p.process(in.p, out.p, 2, kWin);
        f64 s = 0.0;
        for (int i = 0; i < kWin; ++i)
            s += (f64)out.l[(size_t)i] * (f64)out.l[(size_t)i] +
                 (f64)out.r[(size_t)i] * (f64)out.r[(size_t)i];
        env.push_back(std::sqrt(s / (f64)(2 * kWin)));
    }

    const f64 ref = env.empty() ? 0.0 : env[0];
    if (ref <= 0.0) return -1.0;
    for (size_t i = 1; i < env.size(); ++i) {
        if (env[i] <= ref * 0.1) {                     // -20 dB
            const f64 t20 = 0.025 * (f64)i;
            return t20 * 3.0;                          // -20 dB -> -60 dB
        }
    }
    return -1.0;
}

static void testReverb(PluginRegistry& reg) {
    banner("Reverb");

    const PluginDesc* d = reg.find("nxtakt:reverb");
    CHECK(d != nullptr, "registry finds nxtakt:reverb");
    if (!d) return;

    auto rv = reg.instantiate(*d, kSR, kBlock);
    CHECK(rv != nullptr, "instantiate + prepare");
    if (!rv) return;

    const int pPre = paramIndex(*rv, "Pre-Delay");
    const int pDec = paramIndex(*rv, "Decay");
    const int pDmp = paramIndex(*rv, "Damping");
    const int pWid = paramIndex(*rv, "Width");
    const int pMix = paramIndex(*rv, "Dry/Wet");
    CHECK(pPre >= 0 && pDec >= 0 && pDmp >= 0 && pWid >= 0 && pMix >= 0,
          "params Pre-Delay/Decay/Damping/Width/Dry/Wet present");
    if (pDec < 0 || pMix < 0) return;

    // 1. Wet level at defaults: a reverb that needs the fader moved before it
    //    can be heard, or one that doubles the level, is a reverb nobody trusts.
    {
        auto r2 = reg.instantiate(*d, kSR, kBlock);
        r2->setParam(paramIndex(*r2, "Dry/Wet"), 1.f);
        Buf in, out;
        Noise ns;
        f64 inSum = 0.0, outSum = 0.0;
        int n = 0;
        for (int b = 0; b < 100; ++b) {
            for (int i = 0; i < kBlock; ++i) in.l[(size_t)i] = in.r[(size_t)i] = 0.25f * ns.next();
            out.clear();
            r2->process(in.p, out.p, 2, kBlock);
            if (b >= 40) {                              // after the tank fills
                for (int i = 0; i < kBlock; ++i) {
                    inSum  += (f64)in.l[(size_t)i] * (f64)in.l[(size_t)i];
                    outSum += (f64)out.l[(size_t)i] * (f64)out.l[(size_t)i];
                    ++n;
                }
            }
        }
        const f64 db = 20.0 * std::log10(std::sqrt(std::fmax(outSum, 1e-30) / std::fmax(inSum, 1e-30)));
        CHECK(std::fabs(db) < 6.0, "100%% wet sits %.2f dB from the dry level", db);
        (void)n;
    }

    // 2. The tail decays, and it decays monotonically. A few percent of ripple
    //    is the modulated tank breathing, not a fault; a tail that grows is a
    //    tank that is going to take the mix with it.
    rv->setParam(pMix, 1.f);
    rv->setParam(pDec, 2.f);
    rv->setParam(pDmp, 18000.f);
    std::vector<f64> env;
    const f64 rt2 = measureRt60(*rv, env);
    CHECK(!env.empty() && env[0] > 1e-4, "the tank rings after the input stops (%.5f)",
          env.empty() ? 0.0 : env[0]);

    bool mono = true;
    size_t badAt = 0;
    for (size_t i = 1; i < env.size() && env[i - 1] > 1e-7; ++i) {
        if (env[i] > env[i - 1] * 1.12) { mono = false; badAt = i; break; }
    }
    CHECK(mono, "the tail decays monotonically to -140 dB%s", mono ? "" : " -- rose at window");
    if (!mono) note("non-monotonic window index above");
    (void)badAt;

    CHECK(rt2 > 0.0, "RT60 is measurable (%.2f s)", rt2);
    CHECK(rt2 > 1.0 && rt2 < 3.6, "a 2 s decay measures RT60 = %.2f s", rt2);

    // 3. RT60 tracks the knob. Two more settings, and the ordering plus the
    //    rough proportionality both have to hold.
    rv->prepare(kSR, kBlock);
    rv->setParam(pMix, 1.f);
    rv->setParam(pDmp, 18000.f);
    rv->setParam(pDec, 0.5f);
    const f64 rtShort = measureRt60(*rv, env);
    rv->prepare(kSR, kBlock);
    rv->setParam(pMix, 1.f);
    rv->setParam(pDmp, 18000.f);
    rv->setParam(pDec, 6.f);
    const f64 rtLong = measureRt60(*rv, env);
    CHECK(rtShort > 0.2 && rtShort < 1.2, "a 0.5 s decay measures %.2f s", rtShort);
    CHECK(rtLong > 3.5 && rtLong < 10.0, "a 6 s decay measures %.2f s", rtLong);
    CHECK(rtShort < rt2 && rt2 < rtLong,
          "RT60 is monotonic in the knob (%.2f < %.2f < %.2f)", rtShort, rt2, rtLong);

    // 4. Damping shortens the tail rather than lengthening it, and the device
    //    stays sane at the extremes of it.
    rv->prepare(kSR, kBlock);
    rv->setParam(pMix, 1.f);
    rv->setParam(pDec, 4.f);
    rv->setParam(pDmp, 18000.f);
    const f64 rtOpen = measureRt60(*rv, env);
    rv->prepare(kSR, kBlock);
    rv->setParam(pMix, 1.f);
    rv->setParam(pDec, 4.f);
    rv->setParam(pDmp, 800.f);
    const f64 rtDamped = measureRt60(*rv, env);
    CHECK(rtDamped > 0.0 && rtDamped < rtOpen,
          "damping shortens the broadband tail (%.2f s damped vs %.2f s open)",
          rtDamped, rtOpen);

    // 5. Pre-delay is a real delay: the wet output stays silent for that long
    //    after an impulse. (Plus the input diffusers, which are ~30 samples.)
    {
        auto r3 = reg.instantiate(*d, kSR, kBlock);
        r3->setParam(paramIndex(*r3, "Dry/Wet"), 1.f);
        r3->setParam(paramIndex(*r3, "Pre-Delay"), 100.f);
        std::vector<f32> l, r;
        impulseResponse(*r3, 24000, l, r);
        const int expect = (int)(0.100 * kSR);
        f32 before = 0.f;
        for (int i = 0; i < expect - 100; ++i) before = std::fmax(before, std::fabs(l[(size_t)i]));
        f32 after = 0.f;
        for (int i = expect; i < expect + 8000 && i < (int)l.size(); ++i)
            after = std::fmax(after, std::fabs(l[(size_t)i]));
        CHECK(before < 1e-6f, "100 ms of pre-delay is silent (%.9f)", (double)before);
        CHECK(after > 1e-4f, "and the tank fires after it (%.5f)", (double)after);
    }

    // 5b. Pre-delay ZERO is zero, not a buffer's worth. The pre-delay line is
    //     read-then-push, so a tap of 0 lands on the write position -- the
    //     sample from a whole (power-of-two) buffer ago, i.e. ~341 ms at 48 kHz.
    //     With the tap clamped to >= 1 sample, an impulse must reach the wet
    //     output within the first few milliseconds.
    {
        auto r5 = reg.instantiate(*d, kSR, kBlock);
        r5->setParam(paramIndex(*r5, "Dry/Wet"), 1.f);
        r5->setParam(paramIndex(*r5, "Pre-Delay"), 0.f);
        std::vector<f32> l, r;
        impulseResponse(*r5, 24000, l, r);
        int first = -1;
        for (int i = 0; i < (int)l.size(); ++i) {
            if (std::fabs(l[(size_t)i]) > 1e-4f) { first = i; break; }
        }
        CHECK(first >= 0, "pre-delay 0: the tank fires at all");
        CHECK(first >= 0 && first < 1000,
              "pre-delay 0 fires within 1000 frames, not a buffer later (first at %d)", first);
    }

    // 6. Width 0 collapses the two outputs onto each other exactly.
    {
        auto r4 = reg.instantiate(*d, kSR, kBlock);
        r4->setParam(paramIndex(*r4, "Dry/Wet"), 1.f);
        r4->setParam(paramIndex(*r4, "Width"), 0.f);
        Buf in, out;
        Noise ns;
        f32 diff = 0.f;
        for (int b = 0; b < 40; ++b) {
            for (int i = 0; i < kBlock; ++i) in.l[(size_t)i] = in.r[(size_t)i] = 0.25f * ns.next();
            out.clear();
            r4->process(in.p, out.p, 2, kBlock);
            if (b >= 20)
                for (int i = 0; i < kBlock; ++i)
                    diff = std::fmax(diff, std::fabs(out.l[(size_t)i] - out.r[(size_t)i]));
        }
        CHECK(diff < 1e-6f, "width 0 is mono (max L-R %.9f)", (double)diff);
    }

    // 7. Fully dry is bit-exact.
    {
        auto r5 = reg.instantiate(*d, kSR, kBlock);
        r5->setParam(paramIndex(*r5, "Dry/Wet"), 0.f);
        Buf in, out;
        f32 err = 0.f;
        for (int b = 0; b < 12; ++b) {
            for (int i = 0; i < kBlock; ++i)
                in.l[(size_t)i] = in.r[(size_t)i] = 0.5f * (f32)std::sin(6.2831853 * 330.0 * (b * kBlock + i) / kSR);
            out.clear();
            r5->process(in.p, out.p, 2, kBlock);
            if (b >= 4)
                for (int i = 0; i < kBlock; ++i)
                    err = std::fmax(err, std::fabs(out.l[(size_t)i] - in.l[(size_t)i]));
        }
        CHECK(err < 1e-6f, "dry/wet at 0 is a bit-exact copy (err %.9f)", (double)err);
    }
}

// ---------------------------------------------------------------------------
// Block-size invariance
//
// A device whose output depends on the buffer size it was handed is a device
// whose render depends on the audio interface it was made on. Both new devices
// that run anything at control rate -- the Auto Filter's coefficient tick and
// the Limiter's moving-average rebuild -- carry their counters ACROSS process()
// calls precisely so this holds, and this is the test that says whether they
// really do.
//
// Bit-identical is the bar, not "close". The chunk sizes are chosen to break
// anything block-aligned: 1 (every call a single sample), 7 (coprime with the
// 16-sample control tick), 300 (bigger than the prepared block size).
//
// The parameters are set once and left alone, which is the honest scope of the
// claim: every device in the file reads its parameters once per block, so a
// knob MOVING mid-render lands on a block boundary and always will. What is
// being tested is that nothing else does.
// ---------------------------------------------------------------------------

// Feeds `in` through `p` in blocks of `chunk`, IN PLACE, which is how the
// engine calls a device on a track.
static void renderChunked(PluginInstance& p, const std::vector<f32>& inL,
                          const std::vector<f32>& inR, int chunk,
                          std::vector<f32>& oL, std::vector<f32>& oR) {
    const int frames = (int)inL.size();
    oL.assign((size_t)frames, 0.f);
    oR.assign((size_t)frames, 0.f);
    std::vector<f32> bl((size_t)chunk, 0.f), br((size_t)chunk, 0.f);
    for (int i = 0; i < frames; i += chunk) {
        const int k = (frames - i) < chunk ? (frames - i) : chunk;
        for (int j = 0; j < k; ++j) {
            bl[(size_t)j] = inL[(size_t)(i + j)];
            br[(size_t)j] = inR[(size_t)(i + j)];
        }
        const f32* cin[2]  = { bl.data(), br.data() };
        f32*       cout[2] = { bl.data(), br.data() };
        p.process(cin, cout, 2, k);
        for (int j = 0; j < k; ++j) {
            oL[(size_t)(i + j)] = bl[(size_t)j];
            oR[(size_t)(i + j)] = br[(size_t)j];
        }
    }
}

static void testBlockInvariance(PluginRegistry& reg) {
    banner("new devices: the output does not depend on the block size");

    const int kFrames = 6000;
    std::vector<f32> inL((size_t)kFrames), inR((size_t)kFrames);
    Noise ns;
    for (int i = 0; i < kFrames; ++i) {
        inL[(size_t)i] = 0.3f * ns.next();
        inR[(size_t)i] = 0.3f * ns.next();
    }

    for (const char* uri : { "nxtakt:autofilter", "nxtakt:chorus",
                             "nxtakt:limiter", "nxtakt:utility" }) {
        const PluginDesc* d = reg.find(uri);
        CHECK(d != nullptr, "%s: in the registry", uri);
        if (!d) continue;

        // Settings that make every control path actually move: a modulated
        // filter, a modulated delay with feedback, a limiter that is limiting,
        // a utility that is off its identity path.
        const std::string u = uri;
        auto build = [&]() -> std::unique_ptr<PluginInstance> {
            auto p = reg.instantiate(*d, kSR, kBlock);
            if (!p) return p;
            if (u == "nxtakt:autofilter") {
                p->setParam(paramIndex(*p, "Cutoff"), 800.f);
                p->setParam(paramIndex(*p, "Resonance"), 0.6f);
                p->setParam(paramIndex(*p, "LFO Amount"), 3.f);
                p->setParam(paramIndex(*p, "LFO Sync"), 0.f);
                p->setParam(paramIndex(*p, "LFO Rate"), 7.f);
                p->setParam(paramIndex(*p, "LFO Phase"), 90.f);
                p->setParam(paramIndex(*p, "Env Amount"), 2.f);
            } else if (u == "nxtakt:chorus") {
                p->setParam(paramIndex(*p, "Feedback"), 0.6f);
                p->setParam(paramIndex(*p, "Voices"), 4.f);
                p->setParam(paramIndex(*p, "Rate"), 3.f);
            } else if (u == "nxtakt:limiter") {
                p->setParam(paramIndex(*p, "Input"), 12.f);
                p->setParam(paramIndex(*p, "Ceiling"), -6.f);
                p->setParam(paramIndex(*p, "Release"), 40.f);
            } else {
                p->setParam(paramIndex(*p, "Gain"), -3.f);
                p->setParam(paramIndex(*p, "Width"), 1.7f);
                p->setParam(paramIndex(*p, "DC Block"), 1.f);
                p->setParam(paramIndex(*p, "Invert R"), 1.f);
            }
            return p;
        };

        std::vector<f32> refL, refR, altL, altR;
        auto ref = build();
        if (!ref) continue;
        renderChunked(*ref, inL, inR, kBlock, refL, refR);

        f32 worst = 0.f;
        for (int chunk : { 1, 7, 300 }) {
            auto alt = build();
            if (!alt) break;
            renderChunked(*alt, inL, inR, chunk, altL, altR);
            f32 diff = 0.f;
            for (int i = 0; i < kFrames; ++i) {
                diff = std::fmax(diff, std::fabs(refL[(size_t)i] - altL[(size_t)i]));
                diff = std::fmax(diff, std::fabs(refR[(size_t)i] - altR[(size_t)i]));
            }
            CHECK(diff == 0.f, "%s: blocks of %d are bit-identical to blocks of %d (max diff %.9f)",
                  uri, chunk, kBlock, (double)diff);
            worst = std::fmax(worst, diff);
        }

        // The 300-frame pass was larger than the prepared block size, so this
        // also says the device PROCESSES an oversized block rather than
        // degrading to passthrough the way Pulse and the Rack must.
        CHECK(worst == 0.f, "%s: an oversized block is processed, not degraded", uri);
    }
}

// ---------------------------------------------------------------------------
// Auto Filter
// ---------------------------------------------------------------------------

static void testAutoFilter(PluginRegistry& reg) {
    banner("Auto Filter");

    const PluginDesc* d = reg.find("nxtakt:autofilter");
    CHECK(d != nullptr, "registry finds nxtakt:autofilter");
    if (!d) return;

    auto make = [&](int type, f32 cutoff, f32 res) {
        auto f = reg.instantiate(*d, kSR, kBlock);
        if (!f) return f;
        f->setParam(paramIndex(*f, "Type"), (f32)type);
        f->setParam(paramIndex(*f, "Cutoff"), cutoff);
        f->setParam(paramIndex(*f, "Resonance"), res);
        f->setParam(paramIndex(*f, "LFO Amount"), 0.f);
        f->setParam(paramIndex(*f, "Env Amount"), 0.f);
        return f;
    };

    // The tempo wart, stated in the test as well as in the source, because a
    // test is where someone looks to find out what a device promises.
    {
        auto probe = reg.instantiate(*d, kSR, kBlock);
        CHECK(probe != nullptr, "instantiate + prepare");
        if (!probe) return;
        CHECK(paramIndex(*probe, "Tempo") >= 0,
              "the device carries its own Tempo parameter, like the Delay");
        note("PluginInstance still has no transport channel, so the synced LFO runs "
             "off a device parameter (120 BPM default) -- the same wart, the same "
             "shape, and the same one-line host.h fix documented on class Delay.");
        const int ps = paramIndex(*probe, "LFO Sync");
        const int pd = paramIndex(*probe, "LFO Division");
        const int pt = paramIndex(*probe, "Type");
        CHECK(ps >= 0 && probe->paramInfo(ps).isBool, "LFO Sync is flagged as a switch");
        CHECK(pd >= 0 && probe->paramInfo(pd).isInt, "LFO Division is flagged as stepped");
        CHECK(pt >= 0 && probe->paramInfo(pt).isInt, "Type is flagged as stepped");

        // Defaults: cutoff parked at the top of its range, so dropping the
        // device on a channel is very nearly a wire. Measured rather than
        // asserted by eye -- a 2-pole at 18 kHz is what it is at 1 kHz.
        const f64 g1k = probeGainDb(*probe, 1000.0, 0.25f, 40);
        CHECK(std::fabs(g1k) < 0.5, "at its defaults it is within %.2f dB of unity at 1 kHz", g1k);
    }

    // 1. Lowpass: flat a decade below, 12 dB/oct above.
    {
        auto f = make(0, 1000.f, 0.f);
        if (!f) return;
        const f64 lo = probeGainDb(*f, 100.0, 0.25f, 40);
        const f64 hi = probeGainDb(*f, 4000.0, 0.25f, 80);
        CHECK(std::fabs(lo) < 0.6, "LP: 100 Hz passes at %.2f dB (cutoff 1 kHz)", lo);
        CHECK(hi < -18.0 && hi > -32.0,
              "LP: two octaves up is %.1f dB, i.e. the 12 dB/oct a 2-pole owes", hi);
    }

    // 2. Highpass: the mirror image.
    {
        auto f = make(2, 1000.f, 0.f);
        if (!f) return;
        const f64 lo = probeGainDb(*f, 250.0, 0.25f, 40);
        const f64 hi = probeGainDb(*f, 8000.0, 0.25f, 80);
        CHECK(lo < -18.0 && lo > -32.0, "HP: two octaves down is %.1f dB", lo);
        CHECK(std::fabs(hi) < 0.6, "HP: 8 kHz passes at %.2f dB", hi);
    }

    // 3. Bandpass, normalised: unity at the cutoff whatever the resonance, and
    //    down on both sides. The normalisation is the point -- a raw SVF band
    //    tap has a peak gain of Q, so this would read +26 dB at resonance 1.
    {
        for (f32 res : { 0.2f, 1.0f }) {
            auto f = make(1, 1000.f, res);
            if (!f) continue;
            const f64 at   = probeGainDb(*f, 1000.0, 0.25f, 60);
            const f64 down = probeGainDb(*f, 125.0, 0.25f, 20);
            CHECK(std::fabs(at) < 1.0,
                  "BP: unity at the cutoff at resonance %.1f (%.2f dB)", (double)res, at);
            CHECK(down < -12.0, "BP: three octaves down is %.1f dB at resonance %.1f",
                  down, (double)res);
        }
    }

    // 4. Resonance does what its name says, at the cutoff.
    {
        auto flat = make(0, 1000.f, 0.f);
        auto ring = make(0, 1000.f, 1.f);
        if (!flat || !ring) return;
        const f64 a = probeGainDb(*flat, 1000.0, 0.05f, 60);
        const f64 b = probeGainDb(*ring, 1000.0, 0.05f, 60);
        CHECK(b - a > 20.0, "LP: resonance 1 peaks %.1f dB above resonance 0 at the cutoff",
              b - a);
        CHECK(b < 40.0, "and does not run away (%.1f dB)", b);
    }

    // 5. The synced LFO and the free LFO are the SAME LFO.
    //
    // A 1/4 at 120 BPM is one cycle every 0.5 s, i.e. 2 Hz. If the division
    // maths is right, a synced filter and a free-running one at 2 Hz must
    // produce bit-identical output from the same input -- which tests the
    // conversion far more sharply than measuring a wobble could, and is the
    // part of the tempo story that does NOT depend on where the BPM came from.
    {
        const int kFrames = 4096;
        std::vector<f32> inL((size_t)kFrames), inR((size_t)kFrames);
        Noise ns;
        for (int i = 0; i < kFrames; ++i) inL[(size_t)i] = inR[(size_t)i] = 0.3f * ns.next();

        auto sync = reg.instantiate(*d, kSR, kBlock);
        auto freeRun = reg.instantiate(*d, kSR, kBlock);
        if (!sync || !freeRun) return;
        for (PluginInstance* p : { sync.get(), freeRun.get() }) {
            p->setParam(paramIndex(*p, "Cutoff"), 600.f);
            p->setParam(paramIndex(*p, "LFO Amount"), 3.f);
        }
        sync->setParam(paramIndex(*sync, "LFO Sync"), 1.f);
        sync->setParam(paramIndex(*sync, "LFO Division"), 5.f);      // 1/4
        sync->setParam(paramIndex(*sync, "Tempo"), 120.f);
        freeRun->setParam(paramIndex(*freeRun, "LFO Sync"), 0.f);
        freeRun->setParam(paramIndex(*freeRun, "LFO Rate"), 2.f);

        std::vector<f32> aL, aR, bL, bR;
        renderChunked(*sync, inL, inR, kBlock, aL, aR);
        renderChunked(*freeRun, inL, inR, kBlock, bL, bR);
        f32 diff = 0.f;
        for (int i = 0; i < kFrames; ++i)
            diff = std::fmax(diff, std::fabs(aL[(size_t)i] - bL[(size_t)i]));
        CHECK(diff == 0.f, "1/4 at 120 BPM is exactly 2 Hz (max diff %.9f)", (double)diff);

        // ...and the tempo really is the divisor.
        auto half = reg.instantiate(*d, kSR, kBlock);
        auto slow = reg.instantiate(*d, kSR, kBlock);
        if (!half || !slow) return;
        for (PluginInstance* p : { half.get(), slow.get() }) {
            p->setParam(paramIndex(*p, "Cutoff"), 600.f);
            p->setParam(paramIndex(*p, "LFO Amount"), 3.f);
        }
        half->setParam(paramIndex(*half, "LFO Sync"), 1.f);
        half->setParam(paramIndex(*half, "LFO Division"), 5.f);
        half->setParam(paramIndex(*half, "Tempo"), 60.f);
        slow->setParam(paramIndex(*slow, "LFO Sync"), 0.f);
        slow->setParam(paramIndex(*slow, "LFO Rate"), 1.f);
        renderChunked(*half, inL, inR, kBlock, aL, aR);
        renderChunked(*slow, inL, inR, kBlock, bL, bR);
        diff = 0.f;
        for (int i = 0; i < kFrames; ++i)
            diff = std::fmax(diff, std::fabs(aL[(size_t)i] - bL[(size_t)i]));
        CHECK(diff == 0.f, "the same division at 60 BPM is exactly 1 Hz (max diff %.9f)",
              (double)diff);
    }

    // 6. The LFO's stereo phase: 0 means the two channels are the same filter,
    //    which is what a mono-compatible setting has to mean.
    {
        Buf in, out;
        Noise ns;
        for (int i = 0; i < kBlock; ++i) in.l[(size_t)i] = in.r[(size_t)i] = 0.3f * ns.next();

        auto same = reg.instantiate(*d, kSR, kBlock);
        if (!same) return;
        same->setParam(paramIndex(*same, "Cutoff"), 700.f);
        same->setParam(paramIndex(*same, "LFO Amount"), 3.f);
        same->setParam(paramIndex(*same, "LFO Phase"), 0.f);
        f32 lr = 0.f;
        for (int b = 0; b < 8; ++b) {
            out.clear();
            same->process(in.p, out.p, 2, kBlock);
            for (int i = 0; i < kBlock; ++i)
                lr = std::fmax(lr, std::fabs(out.l[(size_t)i] - out.r[(size_t)i]));
        }
        CHECK(lr == 0.f, "LFO phase 0: the two channels are bit-identical (%.9f)", (double)lr);

        auto wide = reg.instantiate(*d, kSR, kBlock);
        if (!wide) return;
        wide->setParam(paramIndex(*wide, "Cutoff"), 700.f);
        wide->setParam(paramIndex(*wide, "LFO Amount"), 3.f);
        wide->setParam(paramIndex(*wide, "LFO Phase"), 180.f);
        f32 spread = 0.f;
        for (int b = 0; b < 8; ++b) {
            out.clear();
            wide->process(in.p, out.p, 2, kBlock);
            for (int i = 0; i < kBlock; ++i)
                spread = std::fmax(spread, std::fabs(out.l[(size_t)i] - out.r[(size_t)i]));
        }
        CHECK(spread > 0.01f, "LFO phase 180: the channels move apart (%.4f)", (double)spread);
    }

    // 7. The envelope follower opens the filter, and only when there is
    //    something to open it with. Same device, same settings, two input
    //    levels: the loud one has to let far more 2 kHz through.
    {
        auto f = make(0, 200.f, 0.f);
        if (!f) return;
        f->setParam(paramIndex(*f, "Env Amount"), 4.f);
        f->setParam(paramIndex(*f, "Env Attack"), 1.f);
        f->setParam(paramIndex(*f, "Env Release"), 50.f);
        const f64 quiet = probeGainDb(*f, 2000.0, 0.002f, 60);
        const f64 loud  = probeGainDb(*f, 2000.0, 0.5f,   60);
        CHECK(loud - quiet > 15.0,
              "env amount +4 oct: a loud input passes 2 kHz %.1f dB better than a quiet one",
              loud - quiet);

        // Negative amount closes it instead, which is the other half of a
        // bipolar control being bipolar.
        auto g = make(0, 4000.f, 0.f);
        if (!g) return;
        g->setParam(paramIndex(*g, "Env Amount"), -4.f);
        const f64 q2 = probeGainDb(*g, 2000.0, 0.002f, 60);
        const f64 l2 = probeGainDb(*g, 2000.0, 0.5f,   60);
        CHECK(q2 - l2 > 15.0, "env amount -4 oct: a loud input passes %.1f dB LESS", q2 - l2);
    }
}

// ---------------------------------------------------------------------------
// Chorus
// ---------------------------------------------------------------------------

static void testChorus(PluginRegistry& reg) {
    banner("Chorus");

    const PluginDesc* d = reg.find("nxtakt:chorus");
    CHECK(d != nullptr, "registry finds nxtakt:chorus");
    if (!d) return;

    // 1. Fully dry is bit-exact, like the Delay's. Same reason: a modulation
    //    device at zero has to be safe to leave in a chain.
    {
        auto ch = reg.instantiate(*d, kSR, kBlock);
        CHECK(ch != nullptr, "instantiate + prepare");
        if (!ch) return;
        ch->setParam(paramIndex(*ch, "Dry/Wet"), 0.f);
        Buf in, out;
        f32 err = 0.f;
        Noise ns;
        for (int b = 0; b < 12; ++b) {
            for (int i = 0; i < kBlock; ++i) in.l[(size_t)i] = in.r[(size_t)i] = 0.3f * ns.next();
            out.clear();
            ch->process(in.p, out.p, 2, kBlock);
            for (int i = 0; i < kBlock; ++i)
                err = std::fmax(err, std::fabs(out.l[(size_t)i] - in.l[(size_t)i]));
        }
        CHECK(err == 0.f, "dry/wet at 0 is a bit-exact copy (%.9f)", (double)err);
    }

    // 2. The wet path is a delay of about the time it was asked for. One voice,
    //    no depth, so the tap is stationary and an impulse says exactly where.
    {
        auto ch = reg.instantiate(*d, kSR, kBlock);
        if (!ch) return;
        ch->setParam(paramIndex(*ch, "Voices"), 1.f);
        ch->setParam(paramIndex(*ch, "Depth"), 0.f);
        ch->setParam(paramIndex(*ch, "Delay"), 10.f);            // ms
        ch->setParam(paramIndex(*ch, "Feedback"), 0.f);
        ch->setParam(paramIndex(*ch, "Dry/Wet"), 1.f);
        std::vector<f32> l, r;
        impulseResponse(*ch, 4800, l, r);
        int at = 0;
        f32 best = 0.f;
        for (int i = 8; i < 4800; ++i)
            if (std::fabs(l[(size_t)i]) > best) { best = std::fabs(l[(size_t)i]); at = i; }
        const int expect = (int)(0.010 * kSR);                   // 480
        CHECK(std::abs(at - expect) <= 1,
              "a 10 ms tap lands at sample %d (expected %d)", at, expect);
    }

    // 3. Modulation is what makes it a chorus rather than a short delay: with
    //    depth up the tap moves, so a steady sine comes back with a varying
    //    delay -- audible as a swing in the level of the summed output.
    {
        auto ch = reg.instantiate(*d, kSR, kBlock);
        if (!ch) return;
        ch->setParam(paramIndex(*ch, "Voices"), 1.f);
        ch->setParam(paramIndex(*ch, "Depth"), 8.f);
        ch->setParam(paramIndex(*ch, "Rate"), 4.f);
        ch->setParam(paramIndex(*ch, "Dry/Wet"), 0.5f);
        Buf in, out;
        f32 lo = 1e9f, hi = 0.f;
        for (int b = 0; b < 120; ++b) {
            for (int i = 0; i < kBlock; ++i)
                in.l[(size_t)i] = in.r[(size_t)i] =
                    0.5f * (f32)std::sin(6.2831853 * 500.0 * (b * kBlock + i) / kSR);
            out.clear();
            ch->process(in.p, out.p, 2, kBlock);
            if (b < 20) continue;                                // let the line fill
            const f32 pk = out.peak();
            lo = std::fmin(lo, pk);
            hi = std::fmax(hi, pk);
        }
        CHECK(hi > lo * 1.2f, "the comb moves: block peak swings %.3f .. %.3f",
              (double)lo, (double)hi);
    }

    // 4. Stereo. The two channels read the same line in quadrature, so a MONO
    //    input comes out decorrelated -- and Width 0 folds that back to
    //    identical, exactly.
    {
        Buf in, out;
        for (int i = 0; i < kBlock; ++i)
            in.l[(size_t)i] = in.r[(size_t)i] =
                0.4f * (f32)std::sin(6.2831853 * 300.0 * i / kSR);

        auto wide = reg.instantiate(*d, kSR, kBlock);
        if (!wide) return;
        wide->setParam(paramIndex(*wide, "Width"), 1.f);
        wide->setParam(paramIndex(*wide, "Dry/Wet"), 1.f);
        f32 spread = 0.f;
        for (int b = 0; b < 40; ++b) {
            out.clear();
            wide->process(in.p, out.p, 2, kBlock);
            if (b > 20)
                for (int i = 0; i < kBlock; ++i)
                    spread = std::fmax(spread, std::fabs(out.l[(size_t)i] - out.r[(size_t)i]));
        }
        CHECK(spread > 0.01f, "width 1: a mono input comes out stereo (%.4f)", (double)spread);

        auto narrow = reg.instantiate(*d, kSR, kBlock);
        if (!narrow) return;
        narrow->setParam(paramIndex(*narrow, "Width"), 0.f);
        narrow->setParam(paramIndex(*narrow, "Dry/Wet"), 1.f);
        f32 same = 0.f;
        for (int b = 0; b < 40; ++b) {
            out.clear();
            narrow->process(in.p, out.p, 2, kBlock);
            for (int i = 0; i < kBlock; ++i)
                same = std::fmax(same, std::fabs(out.l[(size_t)i] - out.r[(size_t)i]));
        }
        CHECK(same == 0.f, "width 0: the wet folds to mono exactly (%.9f)", (double)same);
    }

    // 5. Every voice count works, and full feedback in both polarities stays
    //    bounded -- a modulated delay with feedback is a flanger, and a flanger
    //    is the classic way to build a device that explodes.
    {
        for (f32 v = 1.f; v <= 4.f; v += 1.f) {
            for (f32 fb : { -0.9f, 0.9f }) {
                auto ch = reg.instantiate(*d, kSR, kBlock);
                if (!ch) continue;
                ch->setParam(paramIndex(*ch, "Voices"), v);
                ch->setParam(paramIndex(*ch, "Feedback"), fb);
                ch->setParam(paramIndex(*ch, "Depth"), 6.f);
                ch->setParam(paramIndex(*ch, "Rate"), 2.f);
                ch->setParam(paramIndex(*ch, "Dry/Wet"), 1.f);
                Buf in, out;
                Noise ns;
                f32 peak = 0.f;
                bool fin = true;
                for (int b = 0; b < 200; ++b) {
                    for (int i = 0; i < kBlock; ++i)
                        in.l[(size_t)i] = in.r[(size_t)i] = 0.25f * ns.next();
                    out.clear();
                    ch->process(in.p, out.p, 2, kBlock);
                    if (!out.finite()) { fin = false; break; }
                    peak = std::fmax(peak, out.peak());
                }
                CHECK(fin && peak < 8.f,
                      "%d voice(s) at %+.1f feedback stay finite and bounded (peak %.3f)",
                      (int)v, (double)fb, (double)peak);
            }
        }
    }

    // 6. And it decays to true silence rather than to denormals grinding round
    //    the feedback line, which is the failure this device is most exposed to.
    //
    // Feedback 0.5 rather than 0.9, so the arithmetic of the test is honest:
    // 0.5 per 12 ms round trip is 6 dB per repeat, so 300 dB of decay takes
    // about 0.6 s and the 3.2 s below is not a race. At 0.9 the same decay
    // takes six seconds and a passing test would only mean the loop was long.
    {
        auto ch = reg.instantiate(*d, kSR, kBlock);
        if (!ch) return;
        ch->setParam(paramIndex(*ch, "Feedback"), 0.5f);
        ch->setParam(paramIndex(*ch, "Dry/Wet"), 1.f);
        Buf in, out;
        for (int i = 0; i < kBlock; ++i)
            in.l[(size_t)i] = in.r[(size_t)i] = 0.5f * (f32)std::sin(6.2831853 * 220.0 * i / kSR);
        for (int b = 0; b < 8; ++b) { out.clear(); ch->process(in.p, out.p, 2, kBlock); }
        in.clear();
        f32 tail = 1.f;
        for (int b = 0; b < 600; ++b) {
            out.clear();
            ch->process(in.p, out.p, 2, kBlock);
            tail = out.peak();
        }
        CHECK(tail == 0.f, "the tail reaches exact zero, not a denormal floor (%.3e)",
              (double)tail);
    }
}

// ---------------------------------------------------------------------------
// Limiter
//
// The interesting device, and the reason is one number. It is the only internal
// device with latency; the engine caches what it reports when a chain is
// published (docs/RACKS.md §1) and aligns every parallel path in the project
// against that figure. So the tests below are not "does it limit" -- they are
// "is the number it reports the number of samples it actually delays", and then
// "is the ceiling a ceiling".
// ---------------------------------------------------------------------------

static void testLimiter(PluginRegistry& reg) {
    banner("Limiter");

    const PluginDesc* d = reg.find("nxtakt:limiter");
    CHECK(d != nullptr, "registry finds nxtakt:limiter");
    if (!d) return;

    auto lim = reg.instantiate(*d, kSR, kBlock);
    CHECK(lim != nullptr, "instantiate + prepare");
    if (!lim) return;

    // 1. It reports latency at all, and reports the right amount: 5 ms.
    const int lat  = lim->latencyFrames();
    const int want = (int)(0.005 * kSR + 0.5);
    CHECK(lat == want, "reports %d frames at %.0f Hz (5 ms; expected %d)", lat, kSR, want);
    CHECK(lat == 240, "which at 48 kHz is 240 frames -- the figure LSP's limiter also reports");

    // Constant after prepare, across a re-prepare, across a second instance and
    // across actually running: the four ways a latency figure goes stale.
    CHECK(lim->prepare(kSR, kBlock) && lim->latencyFrames() == lat,
          "stable across a re-prepare (%d)", lim->latencyFrames());
    auto other = reg.instantiate(*d, kSR, kBlock);
    CHECK(other && other->latencyFrames() == lat, "a second instance agrees (%d)",
          other ? other->latencyFrames() : -1);
    {
        Buf in, out;
        for (int b = 0; b < 8; ++b) { out.clear(); lim->process(in.p, out.p, 2, kBlock); }
        CHECK(lim->latencyFrames() == lat, "unchanged after processing (%d)", lim->latencyFrames());
    }

    // It scales with the sample rate, because five milliseconds does. A figure
    // that did not would be wrong at every rate but one.
    {
        auto at441 = reg.instantiate(*d, 44100.0, 64);
        const int w441 = (int)(0.005 * 44100.0 + 0.5);
        CHECK(at441 && at441->latencyFrames() == w441,
              "at 44.1 kHz / 64 frames it reports %d (expected %d)",
              at441 ? at441->latencyFrames() : -1, w441);
    }

    // 2. THE HONESTY TEST: the reported latency IS the measured latency.
    //
    // A unit impulse against a 0 dB ceiling needs no gain reduction at all, so
    // the output is the input delayed and nothing else. If the peak does not
    // land on exactly `lat`, the engine's compensation is wrong by the
    // difference and every parallel path in the set is smeared by it.
    {
        auto p = reg.instantiate(*d, kSR, kBlock);
        if (!p) return;
        p->setParam(paramIndex(*p, "Ceiling"), 0.f);
        std::vector<f32> l, r;
        impulseResponse(*p, 2048, l, r);
        int at = -1;
        for (int i = 0; i < 2048; ++i) if (l[(size_t)i] != 0.f) { at = i; break; }
        CHECK(at == lat, "an impulse comes out at sample %d, exactly the %d it reports", at, lat);
        CHECK(at >= 0 && std::fabs(l[(size_t)at] - 1.f) < 1e-6f,
              "and at full amplitude (%.6f), so nothing else happened to it",
              at >= 0 ? (double)l[(size_t)at] : 0.0);
    }

    // 3. Below the ceiling it is a BIT-EXACT wire plus that delay, which is what
    //    makes it safe on a master bus that is not being pushed.
    {
        auto p = reg.instantiate(*d, kSR, kBlock);
        if (!p) return;
        p->setParam(paramIndex(*p, "Ceiling"), 0.f);
        const int kFrames = 4096;
        std::vector<f32> inL((size_t)kFrames), inR((size_t)kFrames), oL, oR;
        Noise ns;
        for (int i = 0; i < kFrames; ++i) {
            inL[(size_t)i] = 0.2f * ns.next();
            inR[(size_t)i] = 0.2f * ns.next();
        }
        renderChunked(*p, inL, inR, kBlock, oL, oR);
        f32 err = 0.f;
        for (int i = 0; i + lat < kFrames; ++i) {
            err = std::fmax(err, std::fabs(oL[(size_t)(i + lat)] - inL[(size_t)i]));
            err = std::fmax(err, std::fabs(oR[(size_t)(i + lat)] - inR[(size_t)i]));
        }
        CHECK(err == 0.f, "under the ceiling it is bit-identical, %d frames late (%.9f)",
              lat, (double)err);
    }

    // 4. THE CEILING IS A CEILING. Five signals chosen to break a limiter that
    //    smooths its gain in the wrong place: a steady sine (the easy case), an
    //    impulse train (nothing but transients), white noise (a peak somewhere
    //    in every window), a step from silence to full scale (the classic
    //    overshoot), and DC (a detector that assumes zero mean).
    {
        struct Sig { const char* name; int kind; };
        static const Sig kSigs[] = {
            { "a 200 Hz sine", 0 }, { "an impulse train", 1 }, { "white noise", 2 },
            { "a step to full scale", 3 }, { "DC", 4 },
        };
        for (const Sig& s : kSigs) {
            for (f32 ceilDb : { -0.3f, -6.f, -24.f }) {
                auto p = reg.instantiate(*d, kSR, kBlock);
                if (!p) continue;
                p->setParam(paramIndex(*p, "Input"), 12.f);      // hit it hard
                p->setParam(paramIndex(*p, "Ceiling"), ceilDb);
                p->setParam(paramIndex(*p, "Release"), 1.f);     // the fastest, i.e. the worst
                Buf in, out;
                Noise ns;
                f32 peak = 0.f;
                for (int b = 0; b < 80; ++b) {
                    for (int i = 0; i < kBlock; ++i) {
                        const int n = b * kBlock + i;
                        f32 v = 0.f;
                        switch (s.kind) {
                            case 0: v = 0.9f * (f32)std::sin(6.2831853 * 200.0 * n / kSR); break;
                            case 1: v = (n % 977) == 0 ? 1.f : 0.f; break;
                            case 2: v = 0.9f * ns.next(); break;
                            case 3: v = n > 20000 ? 1.f : 0.f; break;
                            default: v = 1.f; break;
                        }
                        in.l[(size_t)i] = v;
                        in.r[(size_t)i] = -v;                    // and not correlated
                    }
                    out.clear();
                    p->process(in.p, out.p, 2, kBlock);
                    peak = std::fmax(peak, out.peak());
                }
                const f32 ceil = dbToGain(ceilDb);
                CHECK(peak <= ceil * 1.0001f,
                      "%s at +12 dB never exceeds a %.1f dB ceiling (peak %.6f vs %.6f)",
                      s.name, (double)ceilDb, (double)peak, (double)ceil);
            }
        }
    }

    // 5. The gain reduction readout says what it is doing.
    {
        auto p = reg.instantiate(*d, kSR, kBlock);
        if (!p) return;
        const int pGr = paramIndex(*p, "Gain Reduction");
        p->setParam(paramIndex(*p, "Ceiling"), -12.f);
        Buf in, out;
        for (int i = 0; i < kBlock; ++i) in.l[(size_t)i] = in.r[(size_t)i] = 0.9f;
        for (int b = 0; b < 20; ++b) { out.clear(); p->process(in.p, out.p, 2, kBlock); }
        const f32 gr = p->getParam(pGr);
        // 0.9 is -0.92 dBFS; held against a -12 dB ceiling, that is 11.1 dB down.
        CHECK(std::fabs(gr - 11.1f) < 1.f,
              "the readout shows %.2f dB of reduction (expected ~11.1)", (double)gr);
        in.clear();
        for (int b = 0; b < 400; ++b) { out.clear(); p->process(in.p, out.p, 2, kBlock); }
        CHECK(p->getParam(pGr) == 0.f, "and returns to 0 when the signal goes away (%.4f)",
              (double)p->getParam(pGr));
    }

    // 6. Release: the gain comes back, and sooner with a fast release than a
    //    slow one.
    {
        auto run = [&](f32 relMs) {
            auto p = reg.instantiate(*d, kSR, kBlock);
            if (!p) return 999;
            p->setParam(paramIndex(*p, "Ceiling"), -12.f);
            p->setParam(paramIndex(*p, "Release"), relMs);
            Buf in, out;
            for (int i = 0; i < kBlock; ++i) in.l[(size_t)i] = in.r[(size_t)i] = 0.95f;
            for (int b = 0; b < 8; ++b) { out.clear(); p->process(in.p, out.p, 2, kBlock); }
            // Now a quiet signal, well under the ceiling: count the blocks until
            // the gain is back at unity.
            for (int i = 0; i < kBlock; ++i) in.l[(size_t)i] = in.r[(size_t)i] = 0.05f;
            int blocks = 0;
            for (; blocks < 400; ++blocks) {
                out.clear();
                p->process(in.p, out.p, 2, kBlock);
                if (blocks > 2 && std::fabs(out.peak() - 0.05f) < 1e-7f) break;
            }
            return blocks;
        };
        const int fast = run(5.f);
        const int lazy = run(800.f);
        CHECK(fast < lazy, "a 5 ms release recovers in %d blocks, an 800 ms one in %d",
              fast, lazy);
        CHECK(lazy < 400, "and the slow one does recover rather than hanging (%d blocks)", lazy);
    }
}

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------

static void testUtility(PluginRegistry& reg) {
    banner("Utility");

    const PluginDesc* d = reg.find("nxtakt:utility");
    CHECK(d != nullptr, "registry finds nxtakt:utility");
    if (!d) return;

    // Deterministic, decorrelated stereo: a signal where every one of the
    // operations below has something to bite on.
    const int kFrames = 2048;
    std::vector<f32> inL((size_t)kFrames), inR((size_t)kFrames);
    {
        Noise ns;
        for (int i = 0; i < kFrames; ++i) {
            inL[(size_t)i] = 0.3f * ns.next();
            inR[(size_t)i] = 0.3f * ns.next();
        }
    }

    std::vector<f32> oL, oR;
    auto run = [&](std::vector<std::pair<const char*, f32>> set,
                   std::vector<f32>& l, std::vector<f32>& r) {
        auto u = reg.instantiate(*d, kSR, kBlock);
        if (!u) return false;
        for (const auto& kv : set) u->setParam(paramIndex(*u, kv.first), kv.second);
        renderChunked(*u, inL, inR, kBlock, l, r);
        return true;
    };

    // 1. THE PROPERTY: at its defaults it is a bit-exact wire. Not nearly.
    {
        CHECK(run({}, oL, oR), "instantiate + prepare");
        f32 err = 0.f;
        for (int i = 0; i < kFrames; ++i) {
            err = std::fmax(err, std::fabs(oL[(size_t)i] - inL[(size_t)i]));
            err = std::fmax(err, std::fabs(oR[(size_t)i] - inR[(size_t)i]));
        }
        CHECK(err == 0.f, "defaults are a bit-exact wire (%.9f)", (double)err);
    }

    // 2. Gain is exact too, because Width 1 never enters the mid/side path.
    {
        run({ { "Gain", -6.f } }, oL, oR);
        const f32 g = dbToGain(-6.f);
        f32 err = 0.f;
        for (int i = 0; i < kFrames; ++i)
            err = std::fmax(err, std::fabs(oL[(size_t)i] - inL[(size_t)i] * g));
        CHECK(err == 0.f, "-6 dB is exactly the input times %.6f (%.9f)", (double)g, (double)err);

        run({ { "Gain", -70.f } }, oL, oR);
        f32 pk = 0.f;
        for (int i = 0; i < kFrames; ++i) pk = std::fmax(pk, std::fabs(oL[(size_t)i]));
        CHECK(pk == 0.f, "the bottom of the Gain range is true silence, not -70 dB of hiss");
    }

    // 3. Width. 0 folds to mono, 2 doubles the side, and both are the M/S
    //    arithmetic the comment claims rather than something adjacent to it.
    {
        run({ { "Width", 0.f } }, oL, oR);
        f32 diff = 0.f, err = 0.f;
        for (int i = 0; i < kFrames; ++i) {
            diff = std::fmax(diff, std::fabs(oL[(size_t)i] - oR[(size_t)i]));
            const f32 mid = 0.5f * (inL[(size_t)i] + inR[(size_t)i]);
            err = std::fmax(err, std::fabs(oL[(size_t)i] - mid));
        }
        CHECK(diff == 0.f, "width 0: the channels are identical (%.9f)", (double)diff);
        CHECK(err == 0.f, "width 0: and equal to the mid exactly (%.9f)", (double)err);

        run({ { "Width", 2.f } }, oL, oR);
        err = 0.f;
        for (int i = 0; i < kFrames; ++i) {
            const f32 mid = 0.5f * (inL[(size_t)i] + inR[(size_t)i]);
            const f32 sid = 0.5f * (inL[(size_t)i] - inR[(size_t)i]) * 2.f;
            err = std::fmax(err, std::fabs(oL[(size_t)i] - (mid + sid)));
            err = std::fmax(err, std::fabs(oR[(size_t)i] - (mid - sid)));
        }
        CHECK(err == 0.f, "width 2: mid + 2*side, exactly (%.9f)", (double)err);
    }

    // 4. Mono is Width 0 by another name, and says so by producing the same
    //    samples.
    {
        std::vector<f32> aL, aR;
        run({ { "Mono", 1.f } }, aL, aR);
        run({ { "Width", 0.f } }, oL, oR);
        f32 err = 0.f;
        for (int i = 0; i < kFrames; ++i)
            err = std::fmax(err, std::fabs(aL[(size_t)i] - oL[(size_t)i]));
        CHECK(err == 0.f, "Mono and Width 0 are the same device (%.9f)", (double)err);
    }

    // 5. Polarity, and the reason it is applied before the width control: an
    //    inverted channel folded to mono has to CANCEL. That is what someone
    //    flipping a polarity switch is listening for.
    {
        run({ { "Invert L", 1.f } }, oL, oR);
        f32 err = 0.f;
        for (int i = 0; i < kFrames; ++i)
            err = std::fmax(err, std::fabs(oL[(size_t)i] + inL[(size_t)i]));
        CHECK(err == 0.f, "Invert L is exactly the negated input (%.9f)", (double)err);

        auto u = reg.instantiate(*d, kSR, kBlock);
        if (!u) return;
        u->setParam(paramIndex(*u, "Invert L"), 1.f);
        u->setParam(paramIndex(*u, "Mono"), 1.f);
        Buf in, out;
        for (int i = 0; i < kBlock; ++i)
            in.l[(size_t)i] = in.r[(size_t)i] = 0.4f * (f32)std::sin(6.2831853 * 440.0 * i / kSR);
        f32 pk = 0.f;
        for (int b = 0; b < 8; ++b) {
            out.clear();
            u->process(in.p, out.p, 2, kBlock);
            pk = std::fmax(pk, out.peak());
        }
        CHECK(pk == 0.f, "inverted on one side and folded to mono, a correlated pair cancels (%.9f)",
              (double)pk);
    }

    // 6. The DC blocker removes an offset without eating the bass, and does
    //    nothing at all when it is switched off.
    {
        auto off = reg.instantiate(*d, kSR, kBlock);
        auto on  = reg.instantiate(*d, kSR, kBlock);
        if (!off || !on) return;
        on->setParam(paramIndex(*on, "DC Block"), 1.f);
        Buf in, out;
        for (int i = 0; i < kBlock; ++i)
            // 750 Hz has a period of exactly 64 samples at 48 kHz, so a
            // 256-frame block holds four whole cycles and the block mean of the
            // music is exactly zero -- which is what lets the mean below be a
            // measurement of the DC offset alone.
            in.l[(size_t)i] = in.r[(size_t)i] =
                0.5f + 0.2f * (f32)std::sin(6.2831853 * 750.0 * i / kSR);

        f32 meanOff = 0.f, meanOn = 0.f;
        for (int b = 0; b < 200; ++b) {
            out.clear();
            off->process(in.p, out.p, 2, kBlock);
            f64 s = 0.0;
            for (int i = 0; i < kBlock; ++i) s += (f64)out.l[(size_t)i];
            meanOff = (f32)(s / kBlock);
        }
        for (int b = 0; b < 200; ++b) {
            out.clear();
            on->process(in.p, out.p, 2, kBlock);
            f64 s = 0.0;
            for (int i = 0; i < kBlock; ++i) s += (f64)out.l[(size_t)i];
            meanOn = (f32)(s / kBlock);
        }
        CHECK(std::fabs(meanOff - 0.5f) < 0.01f, "switched off, the offset survives (%.4f)",
              (double)meanOff);
        CHECK(std::fabs(meanOn) < 0.005f, "switched on, the offset is gone (%.5f)", (double)meanOn);

        // ...and the music is still there: 750 Hz passes at unity, and so does
        // 40 Hz, which is the check that says this is a DC blocker and not a
        // high-pass filter that ate the bottom of the mix.
        auto probe = reg.instantiate(*d, kSR, kBlock);
        if (!probe) return;
        probe->setParam(paramIndex(*probe, "DC Block"), 1.f);
        const f64 g = probeGainDb(*probe, 750.0, 0.25f, 50);
        CHECK(std::fabs(g) < 0.1, "and 750 Hz passes at %.3f dB", g);
        const f64 gLow = probeGainDb(*probe, 40.0, 0.25f, 20);
        CHECK(std::fabs(gLow) < 0.3, "and 40 Hz at %.3f dB -- a DC blocker, not a high-pass", gLow);
    }
}

// ---------------------------------------------------------------------------
// Hosted-instrument smoke test: proves the backend's note path (LV2 atom
// sequences, CLAP note events) against whatever real plugin is installed.
// ---------------------------------------------------------------------------

static void testHostedInstrument(PluginRegistry& reg, PluginFormat fmt, const char* label) {
    banner(label);

    // mda first — it is the most commonly installed set and its synths make
    // sound immediately with default parameters — then any other instrument.
    // Everything is discovered at runtime; nothing here assumes a given plugin
    // is installed.
    auto isMda = [](const std::string& name) {
        std::string l = name;
        for (char& c : l) c = (char)std::tolower((unsigned char)c);
        return l.find("mda") != std::string::npos;
    };
    std::vector<const PluginDesc*> candidates;
    for (int pass = 0; pass < 2; ++pass) {
        for (const PluginDesc& d : reg.plugins()) {
            if (d.format != fmt || !d.hasMidiIn) continue;
            if (d.kind != PluginKind::Instrument || d.audioOut == 0) continue;
            if (isMda(d.name) == (pass == 0)) candidates.push_back(&d);
        }
    }

    if (candidates.empty()) {
        note("no instrument with a MIDI input is installed for this format; skipping");
        return;
    }

    // Several plugins are tried because an individual one may refuse to
    // instantiate here (missing feature, broken bundle) or may need parameters
    // we do not set. The first one that speaks is enough to prove the path.
    const int kTries = (int)candidates.size() < 8 ? (int)candidates.size() : 8;
    for (int c = 0; c < kTries; ++c) {
        const PluginDesc* d = candidates[(size_t)c];
        auto inst = reg.instantiate(*d, kSR, kBlock);
        if (!inst) { note((d->name + ": would not instantiate, trying the next").c_str()); continue; }

        Buf out;
        // Prime it: some plugins need a block before they will accept notes.
        runFor(*inst, out, 2);

        noteOn(*inst, 60, 110, 0);
        bool fin = false;
        const f32 peak = runFor(*inst, out, (int)(kSR / kBlock), &fin);   // one second

        if (peak <= 1e-5f) {
            note((d->name + ": silent with default parameters, trying the next").c_str());
            continue;
        }
        CHECK(fin, "%s: output is finite", d->name.c_str());
        CHECK(peak > 1e-5f, "%s: note-on through the atom path produces audio (peak %.4f)",
              d->name.c_str(), (double)peak);

        // Note-off must be heard too, or we forged only half a sequence.
        noteOff(*inst, 60, 0);
        runFor(*inst, out, (int)(4 * kSR / kBlock));                      // four seconds
        const f32 tail = runFor(*inst, out, 8);
        CHECK(tail < peak, "%s: note-off is honoured (tail %.6f vs peak %.4f)",
              d->name.c_str(), (double)tail, (double)peak);
        return;
    }

    note("no installed instrument of this format produced audio; note path unverified here");
}

// ---------------------------------------------------------------------------
// Latency reporting (PluginInstance::latencyFrames).
//
// Two halves. The internal devices are a fixed contract -- both are
// zero-latency by construction and must say so. The LV2 half proves the other
// direction, that a plugin which *does* report latency is actually read: no
// URI is hard-coded, because the point is to work on whatever the machine has.
// Candidates are discovered by name (the effects that classically carry
// lookahead or a linear-phase filter), a handful are instantiated, and the
// first one that reports a nonzero figure is the witness. If nothing on this
// system reports latency the section notes it and passes -- a missing plugin is
// not a failing host.
// ---------------------------------------------------------------------------

static void testInternalLatency(PluginRegistry& reg) {
    banner("latency: internal devices");

    // Every internal device EXCEPT the Limiter, which has a lookahead and says
    // so -- see testLimiter, which measures that its figure is the truth.
    //
    // The rack is in the list because an EMPTY rack is zero-latency like the
    // rest of them. What it reports when it has something in it is the chain
    // sum, and that is testRackLatency's business.
    for (const char* uri : { "nxtakt:saturator", "nxtakt:pulse", "nxtakt:eq3",
                             "nxtakt:compressor", "nxtakt:delay", "nxtakt:reverb",
                             "nxtakt:autofilter", "nxtakt:chorus", "nxtakt:utility",
                             "nxtakt:rack" }) {
        const PluginDesc* d = reg.find(uri);
        CHECK(d != nullptr, "%s: in the registry", uri);
        if (!d) continue;
        auto inst = reg.instantiate(*d, kSR, kBlock);
        CHECK(inst != nullptr, "%s: instantiate + prepare", uri);
        if (!inst) continue;
        CHECK(inst->latencyFrames() == 0, "%s reports 0 frames of latency (%d)",
              uri, inst->latencyFrames());
        // Zero at any block size and rate, not just the one we prepared with:
        // neither device has anything that could scale with either.
        CHECK(inst->prepare(44100.0, 64) && inst->latencyFrames() == 0,
              "%s still reports 0 at 44.1 kHz / 64 frames", uri);
    }
}

static void testLv2Latency(PluginRegistry& reg) {
    banner("latency: LV2 (real plugin, reportsLatency port)");

    // Words that show up in the names of plugins that delay their output: a
    // lookahead limiter, x42's digital peak limiter (dpl), a linear-phase EQ,
    // a convolver. Searched in this order, so the cheapest and most commonly
    // installed candidates are tried first.
    static const char* kHints[] = {
        "limiter", "lookahead", "look-ahead", "dpl", "delayline",
        "linear phase", "linearphase", "convol", "oversampl",
    };

    std::vector<const PluginDesc*> candidates;
    for (const char* hint : kHints) {
        for (const PluginDesc& d : reg.plugins()) {
            if (d.format != PluginFormat::LV2 || d.audioOut == 0) continue;
            if (lower(d.name + " " + d.uri).find(hint) == std::string::npos) continue;
            if (std::find(candidates.begin(), candidates.end(), &d) == candidates.end())
                candidates.push_back(&d);
        }
    }

    if (candidates.empty()) {
        note("no plugin on this system looks like it would report latency; skipping");
        return;
    }

    // Bounded: every attempt dlopen()s a plugin binary, and one witness is all
    // the backend needs to prove.
    const int kTries = (int)candidates.size() < 12 ? (int)candidates.size() : 12;
    for (int c = 0; c < kTries; ++c) {
        const PluginDesc* d = candidates[(size_t)c];
        auto inst = reg.instantiate(*d, kSR, kBlock);
        if (!inst) { note((d->name + ": would not instantiate, trying the next").c_str()); continue; }

        const int lat = inst->latencyFrames();
        // Every plugin, latent or not, has to give a sane answer.
        CHECK(lat >= 0, "%s: latency is not negative (%d)", d->name.c_str(), lat);
        if (lat <= 0) continue;

        CHECK(lat > 0, "%s: reports %d frames of latency after prepare (%.2f ms at %.0f Hz)",
              d->name.c_str(), lat, 1000.0 * lat / kSR, kSR);

        // Constant after prepare, which is the whole contract: preparing the
        // same instance again at the same rate and block size has to produce
        // the same number, and so does a second, independent instance. The
        // first catches a value that leaks state from the settling block; the
        // second catches one that depends on load order.
        const bool re = inst->prepare(kSR, kBlock);
        CHECK(re, "%s: re-prepares at the same rate/block", d->name.c_str());
        CHECK(re && inst->latencyFrames() == lat,
              "%s: latency is stable across two prepares (%d then %d)",
              d->name.c_str(), lat, inst->latencyFrames());

        auto other = reg.instantiate(*d, kSR, kBlock);
        CHECK(other && other->latencyFrames() == lat,
              "%s: a second instance agrees (%d)", d->name.c_str(),
              other ? other->latencyFrames() : -1);

        // And it must survive actually running: latencyFrames() is read from
        // the engine after the chain is published, long after the first block.
        Buf in, out;
        for (int b = 0; b < 4; ++b) { out.clear(); inst->process(in.p, out.p, 2, kBlock); }
        CHECK(inst->latencyFrames() == lat, "%s: latency unchanged after processing (%d)",
              d->name.c_str(), inst->latencyFrames());
        return;
    }

    note("no installed LV2 plugin reported a nonzero latency; the read path is unverified here");
}

// ---------------------------------------------------------------------------
// Rack
//
// A rack is a PluginInstance that contains PluginInstances, so almost every
// test here is a COMPARISON: the rack is measured against the thing it is
// supposed to be indistinguishable from. "The rack works" is not a claim that
// can fail usefully; "a rack containing the Saturator is bit-for-bit the
// Saturator" is.
// ---------------------------------------------------------------------------

// Fills a buffer pair with the same deterministic noise every time, so two
// chains can be fed identical input and their outputs compared sample for
// sample.
static void fillNoise(Buf& b, u32 seed) {
    Noise ns;
    ns.s = seed;
    for (size_t i = 0; i < b.l.size(); ++i) b.l[i] = b.r[i] = 0.3f * ns.next();
}

// Largest absolute difference between two buffers.
static f32 maxDiff(const Buf& a, const Buf& b) {
    f32 m = 0.f;
    for (size_t i = 0; i < a.l.size(); ++i) {
        m = std::fmax(m, std::fabs(a.l[i] - b.l[i]));
        m = std::fmax(m, std::fabs(a.r[i] - b.r[i]));
    }
    return m;
}

static RackControl* asRack(PluginInstance* p) { return p ? p->rack() : nullptr; }

// Builds a rack containing the named devices, in order. Returns null if
// anything refused, so a failing case reports once rather than crashing.
static std::unique_ptr<PluginInstance> makeRack(PluginRegistry& reg,
                                                std::vector<const char*> uris) {
    const PluginDesc* rd = reg.find("nxtakt:rack");
    if (!rd) return nullptr;
    auto inst = reg.instantiate(*rd, kSR, kBlock);
    if (!inst) return nullptr;
    RackControl* rc = asRack(inst.get());
    if (!rc) return nullptr;
    for (const char* u : uris) {
        const PluginDesc* d = reg.find(u);
        if (!d || !rc->addDevice(*d)) return nullptr;
    }
    return inst;
}

static void testRack(PluginRegistry& reg) {
    banner("Rack: the container contract");

    const PluginDesc* rd = reg.find("nxtakt:rack");
    CHECK(rd != nullptr, "registry finds nxtakt:rack");
    if (!rd) return;
    CHECK(rd->format == PluginFormat::Internal && rd->audioIn == 2 && rd->audioOut == 2,
          "descriptor: internal, %d in / %d out", rd->audioIn, rd->audioOut);
    CHECK(rd->hasMidiIn, "descriptor declares a MIDI input, so a rack can hold an instrument");
    CHECK(rd->paramCount == kRackMacros, "descriptor advertises %d macros", rd->paramCount);

    auto empty = reg.instantiate(*rd, kSR, kBlock);
    CHECK(empty != nullptr, "instantiate + prepare");
    if (!empty) return;

    RackControl* rc = asRack(empty.get());
    CHECK(rc != nullptr, "PluginInstance::rack() exposes the editing face");
    if (!rc) return;
    CHECK(empty->paramCount() == kRackMacros, "the instance has %d macro parameters",
          empty->paramCount());
    CHECK(empty->paramInfo(0).name == "Macro 1" &&
          empty->paramInfo(kRackMacros - 1).name == "Macro 8",
          "macros are named Macro 1 .. Macro %d", kRackMacros);
    CHECK(empty->paramInfo(0).min == 0.f && empty->paramInfo(0).max == 1.f,
          "a macro runs 0..1");

    bool rtOk = true;
    for (int i = 0; i < empty->paramCount(); ++i) if (!empty->setParamRT(i, 0.5f)) rtOk = false;
    CHECK(rtOk, "every macro accepts a realtime write on an empty rack");
    for (int i = 0; i < empty->paramCount(); ++i) empty->setParam(i, 0.f);

    // 1. An empty rack is a wire. Not "nearly" -- the samples are the input's.
    CHECK(rc->deviceCount() == 0, "a fresh rack is empty");
    Buf in, out;
    fillNoise(in, 0x2468ACE1u);
    out.clear();
    empty->process(in.p, out.p, 2, kBlock);
    CHECK(maxDiff(in, out) == 0.f, "an empty rack is a bit-exact passthrough");
    CHECK(empty->latencyFrames() == 0, "an empty rack reports 0 frames of latency");

    // 2. A rack containing one device IS that device. Measured against a bare
    //    Saturator fed the same samples with the same parameters, sample for
    //    sample -- so a scratch-buffer copy that dropped or duplicated a frame
    //    would show up here as a nonzero difference rather than as "sounds ok".
    banner("Rack: one device is that device");
    const PluginDesc* sd = reg.find("nxtakt:saturator");
    CHECK(sd != nullptr, "registry finds nxtakt:saturator");
    if (sd) {
        auto bare = reg.instantiate(*sd, kSR, kBlock);
        auto rack = makeRack(reg, { "nxtakt:saturator" });
        CHECK(bare && rack, "built a bare Saturator and a rack containing one");
        if (bare && rack) {
            RackControl* r = asRack(rack.get());
            PluginInstance* inner = r ? r->device(0) : nullptr;
            CHECK(inner != nullptr, "the rack hands back the device it contains");
            CHECK(r && r->deviceCount() == 1, "deviceCount is 1");
            if (inner) {
                CHECK(inner->desc().uri == "nxtakt:saturator",
                      "and it is the right one (%s)", inner->desc().uri.c_str());

                const int pDrive = paramIndex(*bare, "Drive");
                const int pMix   = paramIndex(*bare, "Mix");
                bare->setParam(pDrive, 18.f);  bare->setParam(pMix, 0.8f);
                inner->setParam(pDrive, 18.f); inner->setParam(pMix, 0.8f);

                Buf a, b;
                f32 worst = 0.f;
                for (int blk = 0; blk < 8; ++blk) {
                    fillNoise(in, 0x1111u + (u32)blk);
                    a.clear(); b.clear();
                    bare->process(in.p, a.p, 2, kBlock);
                    rack->process(in.p, b.p, 2, kBlock);
                    worst = std::fmax(worst, maxDiff(a, b));
                }
                CHECK(worst == 0.f,
                      "a rack containing the Saturator is bit-exact with the Saturator alone "
                      "(max diff %.9f)", (double)worst);
            }
        }
    }

    // 3. Two devices in a rack == the same two devices in series on a track.
    //    The engine runs a track chain as fx->process(bufs, bufs, ...), in
    //    place, so that is exactly how the reference is built here.
    banner("Rack: two in series equals two on a track");
    {
        auto refA = reg.instantiate(*reg.find("nxtakt:eq3"), kSR, kBlock);
        auto refB = reg.instantiate(*reg.find("nxtakt:compressor"), kSR, kBlock);
        auto rack = makeRack(reg, { "nxtakt:eq3", "nxtakt:compressor" });
        CHECK(refA && refB && rack, "built the reference pair and the rack");
        if (refA && refB && rack) {
            RackControl* r = asRack(rack.get());
            CHECK(r && r->deviceCount() == 2, "the rack holds two devices");

            // Same non-default settings on both sides, so the test is not
            // comparing two flat EQs and two idle compressors.
            struct { const char* name; f32 v; } kEq[] = {
                { "Low Gain", 6.f }, { "Mid Gain", -8.f }, { "Mid Freq", 2200.f },
                { "High Gain", 4.f },
            };
            struct { const char* name; f32 v; } kComp[] = {
                { "Threshold", -30.f }, { "Ratio", 8.f }, { "Attack", 3.f },
                { "Release", 60.f }, { "Makeup", 4.f },
            };
            for (const auto& p : kEq) {
                refA->setParam(paramIndex(*refA, p.name), p.v);
                r->device(0)->setParam(paramIndex(*r->device(0), p.name), p.v);
            }
            for (const auto& p : kComp) {
                refB->setParam(paramIndex(*refB, p.name), p.v);
                r->device(1)->setParam(paramIndex(*r->device(1), p.name), p.v);
            }

            Buf a, b;
            f32 worst = 0.f;
            bool fin = true;
            for (int blk = 0; blk < 12; ++blk) {
                fillNoise(in, 0x7777u + (u32)blk);
                a.clear(); b.clear();
                // Track: in -> a, then a -> a in place. Rack: in -> b.
                refA->process(in.p, a.p, 2, kBlock);
                refB->process(a.p, a.p, 2, kBlock);
                rack->process(in.p, b.p, 2, kBlock);
                worst = std::fmax(worst, maxDiff(a, b));
                if (!b.finite()) fin = false;
            }
            CHECK(fin, "the rack's output is finite");
            CHECK(worst == 0.f,
                  "EQ Three -> Compressor inside a rack equals the same two on a track "
                  "(max diff %.9f)", (double)worst);

            // Order matters, and the rack has to honour it: swapping the two
            // must change the sound, or the chain is not really in series.
            CHECK(r->moveDevice(0, 1), "moveDevice reorders the chain");
            CHECK(r->device(0)->desc().uri == "nxtakt:compressor" &&
                  r->device(1)->desc().uri == "nxtakt:eq3", "the order actually changed");
            fillNoise(in, 0x7777u);
            b.clear();
            rack->process(in.p, b.p, 2, kBlock);
            Buf c;
            refA->process(in.p, c.p, 2, kBlock);
            refB->process(c.p, c.p, 2, kBlock);
            CHECK(maxDiff(b, c) > 0.f, "and the reordered chain no longer matches the old order");
            CHECK(r->moveDevice(1, 0), "moveDevice puts it back");
        }
    }

    // 4. Bypass short-circuits the whole chain, not each device.
    banner("Rack: bypass short-circuits the chain");
    {
        auto rack = makeRack(reg, { "nxtakt:saturator", "nxtakt:eq3" });
        CHECK(rack != nullptr, "built a two-device rack");
        if (rack) {
            RackControl* r = asRack(rack.get());
            // Make both devices audibly non-transparent first, so "bypass is a
            // copy" is a real claim rather than a coincidence.
            r->device(0)->setParam(paramIndex(*r->device(0), "Drive"), 30.f);
            r->device(1)->setParam(paramIndex(*r->device(1), "Low Gain"), 12.f);

            fillNoise(in, 0x515Au);
            out.clear();
            rack->process(in.p, out.p, 2, kBlock);
            CHECK(maxDiff(in, out) > 0.001f,
                  "the un-bypassed rack changes the signal (max diff %.5f)",
                  (double)maxDiff(in, out));

            rack->setBypassed(true);
            out.clear();
            rack->process(in.p, out.p, 2, kBlock);
            CHECK(maxDiff(in, out) == 0.f, "bypass is a bit-exact copy of the input");
            rack->setBypassed(false);
        }
    }

    // 5. Macros. The scaling rule is target = min + (max - min) * macro, and
    //    each of its three interesting consequences gets its own case.
    banner("Rack: macro mapping");
    {
        auto rack = makeRack(reg, { "nxtakt:saturator", "nxtakt:delay" });
        CHECK(rack != nullptr, "built a Saturator + Delay rack");
        if (rack) {
            RackControl* r = asRack(rack.get());
            PluginInstance* sat = r->device(0);
            PluginInstance* dly = r->device(1);
            const int pDrive = paramIndex(*sat, "Drive");
            const int pMix   = paramIndex(*sat, "Mix");
            const int pFb    = paramIndex(*dly, "Feedback");

            // (a) one macro, one target, over part of the parameter's range.
            RackMapping m;
            m.macro = 0; m.device = 0; m.param = sat->paramInfo(pDrive).id;
            m.min = 6.f; m.max = 30.f;
            const int mi = r->addMapping(m);
            CHECK(mi == 0, "addMapping accepts a mapping onto Drive (index %d)", mi);
            CHECK(r->mappingCount() == 1, "the rack reports one mapping");

            rack->setParam(0, 0.f);
            CHECK(sat->getParam(pDrive) == 6.f, "macro 0 -> Drive %.3f (want 6)",
                  (double)sat->getParam(pDrive));
            rack->setParam(0, 1.f);
            CHECK(sat->getParam(pDrive) == 30.f, "macro 1 -> Drive %.3f (want 30)",
                  (double)sat->getParam(pDrive));
            rack->setParam(0, 0.5f);
            CHECK(std::fabs(sat->getParam(pDrive) - 18.f) < 1e-4f,
                  "macro 0.5 -> Drive %.4f (want 18, the midpoint of the MAPPED range "
                  "and not of the parameter's)", (double)sat->getParam(pDrive));
            CHECK(r->mapping(0).min == 6.f && r->mapping(0).max == 30.f,
                  "the mapping reads back with the range it was given");

            // The mapped slice is a slice: the macro cannot reach the ends of
            // the parameter's own range, which is the point of a partial range.
            rack->setParam(0, 0.f);
            CHECK(sat->getParam(pDrive) > sat->paramInfo(pDrive).min,
                  "at macro 0 the target sits above the parameter's own minimum");

            // (b) INVERTED. Same macro, a second target, running the other way.
            RackMapping inv;
            inv.macro = 0; inv.device = 0; inv.param = sat->paramInfo(pMix).id;
            inv.min = 1.f; inv.max = 0.f;                 // down as the macro goes up
            CHECK(r->addMapping(inv) == 1, "addMapping accepts an inverted range");

            rack->setParam(0, 0.f);
            const f32 mixLow = sat->getParam(pMix);
            const f32 drvLow = sat->getParam(pDrive);
            rack->setParam(0, 1.f);
            const f32 mixHigh = sat->getParam(pMix);
            const f32 drvHigh = sat->getParam(pDrive);
            CHECK(mixLow == 1.f && mixHigh == 0.f,
                  "an inverted mapping runs Mix %.3f -> %.3f as the macro goes 0 -> 1",
                  (double)mixLow, (double)mixHigh);
            CHECK(mixHigh < mixLow && drvHigh > drvLow,
                  "one macro drives one target up and the other down at the same time");
            rack->setParam(0, 0.25f);
            CHECK(std::fabs(sat->getParam(pMix) - 0.75f) < 1e-5f,
                  "and it interpolates the inverted range correctly (%.5f at 0.25, want 0.75)",
                  (double)sat->getParam(pMix));

            // (c) one macro, two targets on DIFFERENT devices.
            RackMapping two;
            two.macro = 1; two.device = 1; two.param = dly->paramInfo(pFb).id;
            two.min = 0.f; two.max = 0.9f;
            CHECK(r->addMapping(two) == 2, "a mapping onto the second device");
            RackMapping twoB;
            twoB.macro = 1; twoB.device = 0; twoB.param = sat->paramInfo(pDrive).id;
            twoB.min = 0.f; twoB.max = 12.f;
            CHECK(r->addMapping(twoB) == 3, "and a second target for the same macro");

            rack->setParam(1, 1.f);
            CHECK(std::fabs(dly->getParam(pFb) - 0.9f) < 1e-5f &&
                  std::fabs(sat->getParam(pDrive) - 12.f) < 1e-4f,
                  "one macro moved both targets (Feedback %.4f, Drive %.4f)",
                  (double)dly->getParam(pFb), (double)sat->getParam(pDrive));
            rack->setParam(1, 0.f);
            CHECK(dly->getParam(pFb) == 0.f && sat->getParam(pDrive) == 0.f,
                  "and both came back");

            // (d) the automation path drives macros too, not just the GUI one.
            CHECK(rack->setParamRT(1, 1.f),
                  "setParamRT on a macro succeeds (all targets are internal devices)");
            CHECK(std::fabs(dly->getParam(pFb) - 0.9f) < 1e-5f,
                  "and it moved the target (Feedback %.4f)", (double)dly->getParam(pFb));
            rack->setParamRT(1, 0.f);

            // (e) endpoints outside the target's range are clamped in, and
            //     clamping does not silently un-invert anything.
            RackMapping wild;
            wild.macro = 2; wild.device = 0; wild.param = sat->paramInfo(pMix).id;
            wild.min = 5.f; wild.max = -5.f;              // Mix is 0..1
            const int wi = r->addMapping(wild);
            CHECK(wi >= 0, "a mapping with out-of-range endpoints is accepted");
            if (wi >= 0)
                CHECK(r->mapping(wi).min == 1.f && r->mapping(wi).max == 0.f,
                      "and clamped to 1 -> 0, still inverted (%.2f -> %.2f)",
                      (double)r->mapping(wi).min, (double)r->mapping(wi).max);

            // (f) a mapping onto a parameter that does not exist is refused
            //     rather than silently doing nothing at run time.
            RackMapping bad;
            bad.macro = 3; bad.device = 0; bad.param = 9999u;
            CHECK(r->addMapping(bad) < 0, "a mapping onto a nonexistent parameter is refused");
            bad.macro = 3; bad.device = 7; bad.param = 0;
            CHECK(r->addMapping(bad) < 0, "a mapping onto a nonexistent device is refused");

            // (g) removing a device takes its mappings with it and renumbers
            //     the rest, or macro 1 would end up driving the wrong knob.
            const int before = r->mappingCount();
            CHECK(r->removeDevice(1), "removeDevice unlinks the Delay");
            CHECK(r->deviceCount() == 1, "the chain is one device shorter");
            CHECK(r->mappingCount() == before - 1,
                  "the mapping that targeted it went with it (%d -> %d)",
                  before, r->mappingCount());
            bool renumbered = true;
            for (int i = 0; i < r->mappingCount(); ++i)
                if (r->mapping(i).device >= r->deviceCount()) renumbered = false;
            CHECK(renumbered, "every surviving mapping still points inside the chain");

            // The rack still works after the edit.
            fillNoise(in, 0x9090u);
            out.clear();
            rack->process(in.p, out.p, 2, kBlock);
            CHECK(out.finite() && out.peak() > 0.f, "the rack still passes audio after an edit");
        }
    }

    // 6. A rack containing an instrument responds to midi().
    banner("Rack: an instrument inside");
    {
        auto rack = makeRack(reg, { "nxtakt:pulse" });
        CHECK(rack != nullptr, "built a rack containing nxtakt:pulse");
        if (rack) {
            RackControl* r = asRack(rack.get());
            PluginInstance* syn = r->device(0);
            syn->setParam(paramIndex(*syn, "Attack"), 0.005f);
            syn->setParam(paramIndex(*syn, "Decay"), 2.f);
            syn->setParam(paramIndex(*syn, "Release"), 0.05f);

            Buf o;
            CHECK(runFor(*rack, o, 8) == 0.f, "no midi through the rack -> silence");

            noteOn(*rack, 60, 100);
            bool fin = false;
            const f32 peak = runFor(*rack, o, 20, &fin);
            CHECK(peak > 0.01f, "midi() forwarded through the rack makes sound (peak %.4f)",
                  (double)peak);
            CHECK(fin, "and the output is finite");

            noteOff(*rack, 60);
            runFor(*rack, o, (int)(kSR / kBlock));
            CHECK(runFor(*rack, o, 8) == 0.f, "note-off is forwarded too");

            // Bypass drops events rather than feeding a chain nobody renders.
            rack->setBypassed(true);
            noteOn(*rack, 64, 110);
            CHECK(runFor(*rack, o, 8) == 0.f, "a bypassed rack is silent and eats the note");
            rack->setBypassed(false);
            const u8 cc[3] = { 0xB0, 123, 0 };
            rack->midi(cc, 3, 0);
            runFor(*rack, o, (int)(kSR / kBlock));
        }
    }

    // 7. Macro sweeps while processing. The macro path writes parameters on
    //    two devices from two threads' worth of entry points; the one thing
    //    that must never come out of it is a NaN.
    banner("Rack: macro sweeps during processing");
    {
        auto rack = makeRack(reg, { "nxtakt:delay", "nxtakt:reverb", "nxtakt:compressor" });
        CHECK(rack != nullptr, "built a three-device rack");
        if (rack) {
            RackControl* r = asRack(rack.get());
            // Map every macro onto something, several of them inverted, so the
            // sweep exercises the real mapping loop and not an empty one.
            struct { int dev; const char* param; f32 lo, hi; } kMaps[] = {
                { 0, "Feedback", 0.f,   0.95f  }, { 0, "Dry/Wet",  1.f,   0.f    },
                { 0, "Tone",     18000.f, 200.f }, { 1, "Decay",   0.2f,  12.f   },
                { 1, "Damping",  500.f, 18000.f }, { 1, "Dry/Wet", 0.f,   1.f    },
                { 2, "Threshold", 0.f,  -60.f  }, { 2, "Ratio",    1.f,   20.f   },
            };
            int made = 0;
            for (int i = 0; i < kRackMacros; ++i) {
                PluginInstance* d = r->device(kMaps[i].dev);
                const int pi = paramIndex(*d, kMaps[i].param);
                if (pi < 0) continue;
                RackMapping m;
                m.macro = i; m.device = kMaps[i].dev; m.param = d->paramInfo(pi).id;
                m.min = kMaps[i].lo; m.max = kMaps[i].hi;
                if (r->addMapping(m) >= 0) ++made;
            }
            CHECK(made == kRackMacros, "all %d macros mapped (%d)", kRackMacros, made);

            Noise ns;
            bool ok = true;
            f32 peak = 0.f;
            for (int b = 0; b < 400 && ok; ++b) {
                f32 t = (f32)(b % 100) / 50.f;
                if (t > 1.f) t = 2.f - t;
                for (int i = 0; i < kRackMacros; ++i) {
                    // Half the macros through the GUI path, half through the
                    // realtime one, because they are different code.
                    const f32 v = (i & 1) ? t : 1.f - t;
                    if (i & 1) rack->setParam(i, v);
                    else       rack->setParamRT(i, v);
                }
                for (int i = 0; i < kBlock; ++i)
                    in.l[(size_t)i] = in.r[(size_t)i] = 0.25f * ns.next();
                out.clear();
                rack->process(in.p, out.p, 2, kBlock);
                if (!out.finite() || out.peak() > 32.f) ok = false;
                peak = std::fmax(peak, out.peak());
            }
            CHECK(ok, "macro sweeps during processing stay finite and bounded (peak %.3f)",
                  (double)peak);

            fillNoise(in, 0xBEEFu);
            out.clear();
            rack->process(in.p, out.p, 2, kBlock);
            CHECK(out.peak() > 0.f, "and it is still a working device afterwards");
        }
    }

    // 8. The passive form round-trips. This is what persistence will carry, so
    //    it is checked as a value, not as a side effect.
    banner("Rack: the serializable description");
    {
        auto rack = makeRack(reg, { "nxtakt:eq3", "nxtakt:saturator" });
        CHECK(rack != nullptr, "built a rack to describe");
        if (rack) {
            RackControl* r = asRack(rack.get());
            r->device(0)->setParam(paramIndex(*r->device(0), "Mid Gain"), -7.5f);
            r->device(1)->setBypassed(true);
            RackMapping m;
            m.macro = 4; m.device = 1;
            m.param = r->device(1)->paramInfo(paramIndex(*r->device(1), "Drive")).id;
            m.min = 24.f; m.max = 3.f;                    // inverted, partial
            CHECK(r->addMapping(m) >= 0, "mapped a macro for the round trip");
            rack->setParam(4, 0.375f);

            const RackState s = r->state();
            CHECK(s.devices.size() == 2, "state() lists both devices in chain order");
            CHECK(s.devices.size() == 2 && s.devices[0].uri == "nxtakt:eq3" &&
                  s.devices[1].uri == "nxtakt:saturator", "and in the right order");
            CHECK(s.devices.size() == 2 && s.devices[1].bypass,
                  "a bypassed sub-device is recorded as bypassed");
            CHECK(s.mappings.size() == 1 && s.mappings[0].min == 24.f && s.mappings[0].max == 3.f,
                  "the inverted mapping survives into the passive form");
            CHECK(std::fabs(s.macros[4] - 0.375f) < 1e-6f, "macro positions are recorded");

            const std::string text = rackStateToString(s);
            CHECK(!text.empty(), "the compact form is not empty");
            CHECK(text.find('\n') == std::string::npos && text.find(' ') == std::string::npos,
                  "it has no newline and no whitespace, so it survives a line-oriented format");
            CHECK(text.compare(0, 8, "nxrack1;") == 0, "it carries a version tag");

            RackState back;
            CHECK(rackStateFromString(text, back), "and it parses back");
            CHECK(back.devices.size() == s.devices.size(), "with the same device count");
            bool same = back.devices.size() == s.devices.size() &&
                        back.mappings.size() == s.mappings.size();
            for (size_t i = 0; same && i < s.devices.size(); ++i)
                same = back.devices[i].uri == s.devices[i].uri &&
                       back.devices[i].bypass == s.devices[i].bypass &&
                       back.devices[i].params == s.devices[i].params;
            for (size_t i = 0; same && i < s.mappings.size(); ++i)
                same = back.mappings[i].macro  == s.mappings[i].macro &&
                       back.mappings[i].device == s.mappings[i].device &&
                       back.mappings[i].param  == s.mappings[i].param &&
                       back.mappings[i].min    == s.mappings[i].min &&
                       back.mappings[i].max    == s.mappings[i].max;
            for (int i = 0; same && i < kRackMacros; ++i) same = back.macros[i] == s.macros[i];
            CHECK(same, "the round trip is exact: uris, params, bypass, mappings and macros");
            CHECK(rackStateToString(back) == text, "and re-serialising produces the same bytes");

            // A rejected parse clears its output, so these use a scratch value
            // rather than the one being carried into setState below.
            RackState junk;
            CHECK(!rackStateFromString("", junk), "an empty string is rejected");
            CHECK(!rackStateFromString("not-a-rack;d=x", junk), "so is a foreign tag");
            CHECK(junk.devices.empty(), "and a rejected parse leaves nothing behind");

            // Restoring into a fresh rack rebuilds it: same devices, same
            // parameter values, same macro positions.
            auto fresh = reg.instantiate(*rd, kSR, kBlock);
            RackControl* fr = asRack(fresh.get());
            CHECK(fr && fr->setState(back), "setState rebuilds a rack from the passive form");
            if (fr) {
                CHECK(fr->deviceCount() == 2, "the restored rack has both devices");
                CHECK(fr->deviceCount() == 2 &&
                      fr->device(0)->desc().uri == "nxtakt:eq3" &&
                      fr->device(1)->desc().uri == "nxtakt:saturator",
                      "in chain order");
                CHECK(fr->deviceCount() == 2 && fr->device(1)->bypassed(),
                      "with the sub-device bypass restored");
                CHECK(fr->deviceCount() == 2 &&
                      std::fabs(fr->device(0)->getParam(paramIndex(*fr->device(0), "Mid Gain"))
                                + 7.5f) < 1e-4f,
                      "and the parameter values restored verbatim");
                CHECK(std::fabs(fresh->getParam(4) - 0.375f) < 1e-6f, "macro 4 is where it was");
                CHECK(fr->mappingCount() == 1, "and the mapping came back");

                // The restored rack sounds like the original, which is the only
                // property a user cares about.
                fillNoise(in, 0xC0DEu);
                Buf a, b;
                rack->process(in.p, a.p, 2, kBlock);
                fresh->process(in.p, b.p, 2, kBlock);
                CHECK(maxDiff(a, b) == 0.f, "and it sounds identical to the rack it came from");
            }
        }
    }

    // 9. Nesting. A rack is a device, so a rack can contain one, and the
    //    passive form has to survive the recursion.
    banner("Rack: a rack inside a rack");
    {
        auto outer = makeRack(reg, { "nxtakt:rack" });
        CHECK(outer != nullptr, "a rack accepts a rack as a sub-device");
        if (outer) {
            RackControl* o = asRack(outer.get());
            RackControl* i = asRack(o->device(0));
            CHECK(i != nullptr, "the inner rack exposes its own RackControl");
            if (i) {
                const PluginDesc* satd = reg.find("nxtakt:saturator");
                CHECK(satd && i->addDevice(*satd), "and it can be filled");
                i->device(0)->setParam(paramIndex(*i->device(0), "Drive"), 21.f);

                const std::string text = rackStateToString(o->state());
                CHECK(text.find("nxrack1") != std::string::npos, "the outer form carries the tag");

                RackState st;
                CHECK(rackStateFromString(text, st), "the nested form parses");
                CHECK(st.devices.size() == 1 && !st.devices[0].state.empty(),
                      "and the inner rack's state rode along as an opaque field");

                auto rebuilt = reg.instantiate(*rd, kSR, kBlock);
                RackControl* rb = asRack(rebuilt.get());
                CHECK(rb && rb->setState(st), "setState restores the nest");
                if (rb && rb->deviceCount() == 1) {
                    RackControl* inner = asRack(rb->device(0));
                    CHECK(inner && inner->deviceCount() == 1,
                          "the inner rack came back with its device");
                    if (inner && inner->deviceCount() == 1)
                        CHECK(std::fabs(inner->device(0)->getParam(
                                  paramIndex(*inner->device(0), "Drive")) - 21.f) < 1e-4f,
                              "and with its parameter value");
                }
            }
        }
    }

    // 10. A PARKED TARGET. The one case a round trip of "typical" values cannot
    //     catch, and the one a user hits within a minute of making a macro.
    //
    //     Map Drive to a macro, move the macro, then turn Drive BY HAND. The
    //     target now sits off the macro's curve — which is legal, is what the
    //     knob is for, and is what state() records. Restore has to give it back
    //     verbatim.
    //
    //     It did not. setState re-added every mapping through addMapping(),
    //     whose whole job at edit time is to SNAP the target onto the macro's
    //     curve, and it did so against the macro position the fresh rack held
    //     BEFORE the state was applied (the macros are written last). So the
    //     parked value was replaced by a re-derivation from a macro that was not
    //     even at its saved position yet, on every load.
    //
    //     Every value below is exactly representable and the compact form
    //     round-trips shortest-form floats, so these are `==` and not a
    //     tolerance: "comes back exactly as saved" is the claim.
    banner("Rack: a target parked off its macro's curve survives a save/load");
    {
        auto rack = makeRack(reg, { "nxtakt:saturator", "nxtakt:delay" });
        CHECK(rack != nullptr, "built a Saturator + Delay rack to park a knob in");
        if (rack) {
            RackControl* r = asRack(rack.get());
            PluginInstance* sat = r->device(0);
            PluginInstance* dly = r->device(1);
            const int pDrive = paramIndex(*sat, "Drive");
            const int pFb    = paramIndex(*dly, "Feedback");

            // TWO MACROS ON ONE TARGET, which is also the load-order case: the
            // restore must not apply either of them, and it must not let the
            // mapping added last decide what Drive reads.
            RackMapping a;
            a.macro = 2; a.device = 0; a.param = sat->paramInfo(pDrive).id;
            a.min = 0.f; a.max = 36.f;
            CHECK(r->addMapping(a) == 0, "macro 2 mapped onto Drive");
            RackMapping b;
            b.macro = 5; b.device = 0; b.param = sat->paramInfo(pDrive).id;
            b.min = 0.f; b.max = 12.f;
            CHECK(r->addMapping(b) == 1, "and macro 5 onto the SAME target");
            RackMapping c;
            c.macro = 2; c.device = 1; c.param = dly->paramInfo(pFb).id;
            c.min = 0.f; c.max = 0.5f;
            CHECK(r->addMapping(c) == 2, "with a third onto the Delay's Feedback");

            rack->setParam(2, 0.5f);
            rack->setParam(5, 0.25f);
            CHECK(sat->getParam(pDrive) == 3.f && dly->getParam(pFb) == 0.25f,
                  "the macros drive their targets while they are being edited "
                  "(Drive %.3f, Feedback %.4f)",
                  (double)sat->getParam(pDrive), (double)dly->getParam(pFb));

            // THE PARK. Nothing here is unusual: it is one drag on the device's
            // own knob, after the mapping was made.
            sat->setParam(pDrive, 28.5f);
            dly->setParam(pFb, 0.75f);
            CHECK(sat->getParam(pDrive) == 28.5f && dly->getParam(pFb) == 0.75f,
                  "then the user turns both by hand, off every macro's curve");

            const std::string text = rackStateToString(r->state());
            RackState saved;
            CHECK(rackStateFromString(text, saved), "the state serialises and parses back");
            bool carried = false;
            if (!saved.devices.empty())
                for (const auto& pv : saved.devices[0].params)
                    if (pv.first == sat->paramInfo(pDrive).id && pv.second == 28.5f) carried = true;
            CHECK(carried, "and the STRING carries the parked value — the loss is the "
                           "rack's, not the format's");

            auto fresh = reg.instantiate(*rd, kSR, kBlock);
            RackControl* fr = asRack(fresh.get());
            CHECK(fr && fr->setState(saved), "setState rebuilds the rack");
            if (fr && fr->deviceCount() == 2) {
                PluginInstance* fsat = fr->device(0);
                PluginInstance* fdly = fr->device(1);
                const int fDrive = paramIndex(*fsat, "Drive");
                const int fFb    = paramIndex(*fdly, "Feedback");

                CHECK(fsat->getParam(fDrive) == 28.5f,
                      "THE BUG: Drive comes back parked at %.4f, not re-derived from "
                      "a macro (want 28.5; 18 would be macro 2's curve, 3 macro 5's, "
                      "0 either macro at its pre-restore zero)",
                      (double)fsat->getParam(fDrive));
                CHECK(fdly->getParam(fFb) == 0.75f,
                      "and Feedback at %.4f, on a second device the same macro drives "
                      "(want 0.75)", (double)fdly->getParam(fFb));
                CHECK(fresh->getParam(2) == 0.5f && fresh->getParam(5) == 0.25f,
                      "with both macro positions restored (%.3f, %.3f)",
                      (double)fresh->getParam(2), (double)fresh->getParam(5));

                // Structural, and in the saved order — "last write wins" is a
                // statement about mapping order, so a restore that reordered
                // them would change which macro owns Drive.
                CHECK(fr->mappingCount() == 3, "all three mappings came back (%d)",
                      fr->mappingCount());
                CHECK(fr->mappingCount() == 3 &&
                      fr->mapping(0).macro == 2 && fr->mapping(0).max == 36.f &&
                      fr->mapping(1).macro == 5 && fr->mapping(1).max == 12.f &&
                      fr->mapping(2).macro == 2 && fr->mapping(2).device == 1,
                      "in the order they were saved in");

                // Not restored INERT: the mappings are live the moment the user
                // touches a macro again. A fix that simply stopped adding them
                // would pass everything above and break the feature.
                fresh->setParam(5, 1.f);
                CHECK(fsat->getParam(fDrive) == 12.f,
                      "and moving macro 5 drives Drive again: %.3f (want 12)",
                      (double)fsat->getParam(fDrive));
                fresh->setParam(2, 1.f);
                CHECK(fsat->getParam(fDrive) == 36.f && fdly->getParam(fFb) == 0.5f,
                      "macro 2 owns it after that — last write wins, across the "
                      "restore (Drive %.3f, Feedback %.4f)",
                      (double)fsat->getParam(fDrive), (double)fdly->getParam(fFb));
            }
        }
    }

    // 11. The same claim one level down, because setState recurses into a
    //     nested rack through the same path and a fix that only held at depth 0
    //     would be a fix for the test rather than for the bug.
    banner("Rack: a parked target inside a NESTED rack survives too");
    {
        auto outer = makeRack(reg, { "nxtakt:rack" });
        CHECK(outer != nullptr, "built an outer rack holding a rack");
        if (outer) {
            RackControl* o = asRack(outer.get());
            RackControl* i = asRack(o->device(0));
            const PluginDesc* satd = reg.find("nxtakt:saturator");
            if (i && satd && i->addDevice(*satd)) {
                PluginInstance* sat = i->device(0);
                const int pDrive = paramIndex(*sat, "Drive");
                RackMapping m;
                m.macro = 1; m.device = 0; m.param = sat->paramInfo(pDrive).id;
                m.min = 0.f; m.max = 36.f;
                CHECK(i->addMapping(m) == 0, "the inner rack maps macro 1 onto Drive");
                o->device(0)->setParam(1, 0.75f);
                sat->setParam(pDrive, 4.5f);          // parked, off the 27 the macro gives
                CHECK(sat->getParam(pDrive) == 4.5f, "and its target is parked at 4.5");

                RackState st;
                CHECK(rackStateFromString(rackStateToString(o->state()), st),
                      "the nested state round-trips");
                auto rebuilt = reg.instantiate(*rd, kSR, kBlock);
                RackControl* rb = asRack(rebuilt.get());
                CHECK(rb && rb->setState(st), "and restores");
                RackControl* inner = (rb && rb->deviceCount() == 1) ? asRack(rb->device(0)) : nullptr;
                CHECK(inner && inner->deviceCount() == 1, "the inner rack came back filled");
                if (inner && inner->deviceCount() == 1) {
                    PluginInstance* fsat = inner->device(0);
                    CHECK(fsat->getParam(paramIndex(*fsat, "Drive")) == 4.5f,
                          "with its parked Drive at %.3f, not the 27 macro 1 would "
                          "re-derive", (double)fsat->getParam(paramIndex(*fsat, "Drive")));
                    CHECK(rb->device(0)->getParam(1) == 0.75f,
                          "and the inner rack's own macro where it was (%.3f)",
                          (double)rb->device(0)->getParam(1));
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Rack latency: the chain sum.
//
// The real claim -- that the rack ADDS rather than reporting max(), or first(),
// or 0 -- needs a device that actually delays. When this section was written
// there was no such device in the stock set, so the witness had to be whatever
// latent LV2 plugin the machine happened to have, and on a machine with none
// the claim went unverified.
//
// The Limiter changed that: it is a stock device with a real 5 ms lookahead, so
// the arithmetic is now checked everywhere, on every run, with no dependency on
// what is installed. The LV2 witness is kept anyway, because "the rack sums a
// THIRD-PARTY plugin's figure" is a different statement from "the rack sums its
// own", and the first one is the one that gets a real project wrong.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Rack: an edit may not rewrite the layout the audio thread is reading
// ---------------------------------------------------------------------------
//
// AUDIT 3, CRITICAL-2. The published Layout used to be a slot in a four-deep
// ring, on the argument that four edits would have to land inside one audio
// block for the slot being read to be rewritten. setState() alone issues six:
// one to unlink, one per device, one per mapping, one to close -- back to back,
// in microseconds, on whatever thread called it. So the ring wrapped INSIDE a
// single restore while process() was holding a Layout* for the whole block, and
// the audio thread read `n` and `dev[]` out of a structure being overwritten:
// a torn count indexing an array of pointers to sub-devices old enough that
// reclaim() had already destroyed them.
//
// This drives exactly that shape: one thread rendering, one thread restoring,
// and reclaim() in between at the only moment a caller can promise is quiet.
//
// It is a THREAD-SANITIZER test first and a crash test second. Built plain it
// asserts the audible half -- the render stays finite and the rack keeps
// working -- and it passes either way, because a torn read of a live pointer
// usually produces plausible audio. Built with -fsanitize=thread it is the
// whole finding:
//
//   g++ -std=c++20 -O1 -g -fsanitize=thread ... tests/internal_device_test.cpp
//
// Before the fix that reports a data race on Layout::n and Layout::dev[]
// between Rack::republish and Rack::process. After it, none.
static void testRackLayoutUnderRender(PluginRegistry& reg) {
    banner("Rack: a setState() may not rewrite the layout being rendered");

    const PluginDesc* rd = reg.find("nxtakt:rack");
    if (!rd) { CHECK(false, "registry finds nxtakt:rack"); return; }
    auto inst = reg.instantiate(*rd, kSR, kBlock);
    RackControl* rc = inst ? asRack(inst.get()) : nullptr;
    CHECK(rc != nullptr, "a rack to edit under load");
    if (!rc) return;

    // Two states with DIFFERENT device counts, so a torn `n` indexes past what
    // the other state published rather than landing on a same-shaped chain.
    RackState twoDev, oneDev;
    for (const char* u : { "nxtakt:saturator", "nxtakt:autofilter" }) {
        RackState::Device d;
        d.uri = u;
        twoDev.devices.push_back(d);
    }
    { RackState::Device d; d.uri = "nxtakt:saturator"; oneDev.devices.push_back(d); }
    for (int m = 0; m < 2; ++m) {
        RackMapping mp;
        mp.macro = m; mp.device = 0; mp.param = 0; mp.min = 0.f; mp.max = 1.f;
        twoDev.mappings.push_back(mp);
        oneDev.mappings.push_back(mp);
    }
    CHECK(rc->setState(twoDev), "a two-device state loads");

    std::atomic<bool> run{true};
    std::atomic<long> blocks{0};
    std::atomic<bool> finite{true};

    std::thread audio([&] {
        std::vector<f32> l((size_t)kBlock, 0.f), r((size_t)kBlock, 0.f);
        std::vector<f32> ol((size_t)kBlock, 0.f), orr((size_t)kBlock, 0.f);
        for (int i = 0; i < kBlock; ++i) {
            l[(size_t)i] = 0.25f * std::sin(0.05f * (f32)i);
            r[(size_t)i] = -l[(size_t)i];
        }
        const f32* in[2]  = { l.data(), r.data() };
        f32*       out[2] = { ol.data(), orr.data() };
        while (run.load(std::memory_order_relaxed)) {
            inst->process(in, out, 2, kBlock);
            for (int i = 0; i < kBlock; ++i)
                if (!std::isfinite(ol[(size_t)i]) || !std::isfinite(orr[(size_t)i]))
                    finite.store(false, std::memory_order_relaxed);
            blocks.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // The editor. NO reclaim() inside this loop, and that is not an oversight:
    // reclaim()'s contract is "at a moment the caller knows is quiet", and this
    // is the loudest moment there is. Calling it here would destroy sub-devices
    // under the render thread and report a pile of races that are the TEST's
    // fault rather than the rack's, burying the one that is not. The frees are
    // exercised below, after the join, which is where the contract puts them.
    const int kEdits = 24;
    for (int i = 0; i < kEdits; ++i) rc->setState((i & 1) ? oneDev : twoDev);
    // Enough blocks that an edit and a render provably overlapped -- with the
    // edits BUDGETED, which they were not.
    //
    // THIS SPIN USED TO BE UNBOUNDED, and it made the section fail about one
    // run in four. Every setState() instantiates a fresh sub-device per entry
    // and cannot reclaim the old ones while the render thread is live, so the
    // number of edits it managed before `blocks` reached 40 -- a pure
    // scheduling accident -- decided whether `owned_` crossed kOwnedCap (64).
    // Past that the rack correctly refuses to add ("call reclaim() while the
    // rack is idle"), setState leaves the chain empty, and the two assertions
    // below fail for a reason that is the TEST's and not the rack's. The kEdits
    // loop above already spends 36 of the 64; eight more pairs spend 24, which
    // leaves headroom and still guarantees an edit lands inside a render.
    for (int spare = 8; blocks.load(std::memory_order_relaxed) < 40; ) {
        if (spare > 0) { rc->setState(twoDev); rc->setState(oneDev); --spare; }
        else            std::this_thread::yield();
    }
    run.store(false, std::memory_order_relaxed);
    audio.join();

    CHECK(blocks.load() > 0, "%ld blocks rendered while %d states were restored under them",
          blocks.load(), kEdits);
    CHECK(finite.load(), "and every sample of every one of them is finite");

    // Quiet now: the render thread is joined. This is the moment reclaim() asks
    // for, and it frees both the retired layouts and the unlinked sub-devices.
    rc->reclaim();
    CHECK(rc->deviceCount() == 1, "the rack ends holding the last state it was given (%d)",
          rc->deviceCount());
    CHECK(rc->mappingCount() == 2, "with its mappings intact (%d)", rc->mappingCount());

    // And it still renders after everything has been reclaimed: the layout the
    // audio thread ends on is the LIVE one, which reclaim() must never free.
    std::vector<f32> l((size_t)kBlock, 0.1f), r((size_t)kBlock, 0.1f);
    std::vector<f32> ol((size_t)kBlock, 0.f), orr((size_t)kBlock, 0.f);
    const f32* in[2]  = { l.data(), r.data() };
    f32*       out[2] = { ol.data(), orr.data() };
    inst->process(in, out, 2, kBlock);
    bool ok = true;
    for (int i = 0; i < kBlock; ++i)
        if (!std::isfinite(ol[(size_t)i]) || !std::isfinite(orr[(size_t)i])) ok = false;
    CHECK(ok, "and renders finite audio after the final reclaim()");
}

static void testRackLatency(PluginRegistry& reg) {
    banner("Rack: latencyFrames is the chain sum");

    const PluginDesc* rd = reg.find("nxtakt:rack");
    if (!rd) { note("no rack in the registry; skipping"); return; }

    // Internal, zero-latency only: the sum of three zeroes is a zero, and a
    // rack that reported anything else would be inventing delay compensation.
    {
        auto rack = makeRack(reg, { "nxtakt:eq3", "nxtakt:compressor", "nxtakt:saturator" });
        CHECK(rack != nullptr, "built a three internal-device rack");
        if (rack)
            CHECK(rack->latencyFrames() == 0,
                  "a rack of zero-latency devices reports 0 (%d)", rack->latencyFrames());
    }

    // Internal, with the stock latent device. No discovery, no skipping: this
    // runs on every machine.
    {
        const PluginDesc* ld = reg.find("nxtakt:limiter");
        auto probe = ld ? reg.instantiate(*ld, kSR, kBlock) : nullptr;
        const int lim = probe ? probe->latencyFrames() : 0;
        probe.reset();
        CHECK(lim > 0, "the stock Limiter reports %d frames, so it can be the witness", lim);

        auto rack = reg.instantiate(*rd, kSR, kBlock);
        RackControl* r = asRack(rack.get());
        CHECK(rack && r && ld, "built an empty rack");
        if (rack && r && ld && lim > 0) {
            CHECK(rack->latencyFrames() == 0, "empty: 0 frames");
            CHECK(r->addDevice(*ld), "Limiter added");
            CHECK(rack->latencyFrames() == lim, "one Limiter: %d frames (%d)",
                  lim, rack->latencyFrames());

            const PluginDesc* eq = reg.find("nxtakt:eq3");
            CHECK(eq && r->addDevice(*eq), "EQ Three added between them");
            CHECK(rack->latencyFrames() == lim,
                  "a zero-latency device leaves the sum at %d (%d)", lim, rack->latencyFrames());

            CHECK(r->addDevice(*ld), "a second Limiter added");
            CHECK(rack->latencyFrames() == 2 * lim,
                  "two Limiters sum to %d (%d) -- not %d, not 0",
                  2 * lim, rack->latencyFrames(), lim);

            // A rack inside a rack: the sum has to nest, because engine.cpp
            // reads only the outermost figure.
            auto outer = reg.instantiate(*rd, kSR, kBlock);
            RackControl* o = asRack(outer.get());
            if (outer && o) {
                CHECK(o->addDevice(*rd), "an inner rack added to an outer one");
                RackControl* inner = asRack(o->device(0));
                CHECK(inner != nullptr, "and it is a rack");
                if (inner) {
                    CHECK(inner->addDevice(*ld), "Limiter added INSIDE the inner rack");
                    CHECK(outer->latencyFrames() == lim,
                          "the outer rack reports the nested %d frames (%d)",
                          lim, outer->latencyFrames());
                }
            }

            CHECK(r->removeDevice(2), "removed the second Limiter");
            CHECK(rack->latencyFrames() == lim, "the sum came back down to %d (%d)",
                  lim, rack->latencyFrames());
            CHECK(rack->prepare(kSR, kBlock) && rack->latencyFrames() == lim,
                  "re-prepare leaves the sum at %d (%d)", lim, rack->latencyFrames());
        }
    }

    // Now a real third-party latent plugin. Same discovery as testLv2Latency:
    // no URI is hard-coded, because the point is to work on whatever is
    // installed.
    static const char* kHints[] = {
        "limiter", "lookahead", "look-ahead", "dpl", "linear phase", "linearphase",
        "convol", "oversampl",
    };
    std::vector<const PluginDesc*> candidates;
    for (const char* hint : kHints)
        for (const PluginDesc& d : reg.plugins()) {
            if (d.format != PluginFormat::LV2 || d.audioOut == 0 || d.audioIn == 0) continue;
            if (lower(d.name + " " + d.uri).find(hint) == std::string::npos) continue;
            if (std::find(candidates.begin(), candidates.end(), &d) == candidates.end())
                candidates.push_back(&d);
        }

    const int kTries = (int)candidates.size() < 12 ? (int)candidates.size() : 12;
    for (int c = 0; c < kTries; ++c) {
        const PluginDesc* d = candidates[(size_t)c];
        auto probe = reg.instantiate(*d, kSR, kBlock);
        if (!probe) continue;
        const int lat = probe->latencyFrames();
        if (lat <= 0) continue;
        probe.reset();

        note((d->name + ": using this as the latent witness").c_str());

        auto rack = reg.instantiate(*rd, kSR, kBlock);
        RackControl* r = asRack(rack.get());
        CHECK(rack && r, "built an empty rack");
        if (!rack || !r) return;

        CHECK(rack->latencyFrames() == 0, "empty: 0 frames");

        CHECK(r->addDevice(*d), "%s: added to the rack", d->name.c_str());
        CHECK(rack->latencyFrames() == lat,
              "one latent device: the rack reports its %d frames (%d)",
              lat, rack->latencyFrames());

        // A zero-latency device between them must not change the total.
        const PluginDesc* eq = reg.find("nxtakt:eq3");
        CHECK(eq && r->addDevice(*eq), "EQ Three added");
        CHECK(rack->latencyFrames() == lat,
              "adding a zero-latency device leaves the sum at %d (%d)",
              lat, rack->latencyFrames());

        // Two of the latent one: the sum, and nothing else. This is the check
        // that a rack reporting max(), or first(), or 0 would fail.
        CHECK(r->addDevice(*d), "%s: added a second time", d->name.c_str());
        CHECK(rack->latencyFrames() == 2 * lat,
              "two latent devices sum to %d frames (%d) -- not %d, not 0",
              2 * lat, rack->latencyFrames(), lat);

        // And it survives actually running, like every other backend's figure.
        Buf in, out;
        for (int b = 0; b < 4; ++b) { out.clear(); rack->process(in.p, out.p, 2, kBlock); }
        CHECK(rack->latencyFrames() == 2 * lat, "unchanged after processing (%d)",
              rack->latencyFrames());

        // Removing one takes its share back out.
        CHECK(r->removeDevice(2), "removed the second latent device");
        CHECK(rack->latencyFrames() == lat,
              "the sum came back down to %d (%d)", lat, rack->latencyFrames());

        // Re-preparing must not double-count or forget.
        CHECK(rack->prepare(kSR, kBlock) && rack->latencyFrames() == lat,
              "re-prepare leaves the sum at %d (%d)", lat, rack->latencyFrames());
        return;
    }

    note("no installed LV2 plugin reports a nonzero latency; the chain sum is unverified "
         "against a real latent device here");
}

// ---------------------------------------------------------------------------
// Spectra — the wavetable instrument
//
// docs/SPECTRA-PARAMS.md is FROZEN and is the interface between src/plugin and
// src/ui. The first test below transcribes it by hand and compares the
// transcription against what the device reports, so that if either side is
// edited the suite names the divergence instead of the editor discovering it
// at run time. Everything after that MEASURES: "it sounds fine" is not a test,
// "the alias energy at C7 is 75 dB below the harmonic energy" is.
// ---------------------------------------------------------------------------

// The contract, transcribed from the table in the doc. Column order is the
// doc's: id, name, min, max, int, log.
struct SpectraParamSpec {
    int         id;
    const char* name;
    f32         mn, mx;
    bool        isInt;
    bool        isLog;
};

static const SpectraParamSpec kSpectraContract[] = {
    {  0, "A Table",       0.f,     8.f,     true,  false },   // widened by v3
    {  1, "A Position",    0.f,     1.f,     false, false },
    {  2, "A Coarse",     -24.f,    24.f,    true,  false },
    {  3, "A Fine",       -100.f,   100.f,   false, false },
    {  4, "A Level",       0.f,     1.f,     false, false },
    {  5, "A Unison",      1.f,     7.f,     true,  false },
    {  6, "A Detune",      0.f,     100.f,   false, false },
    {  7, "A Spread",      0.f,     1.f,     false, false },
    {  8, "B Table",       0.f,     8.f,     true,  false },   // widened by v3
    {  9, "B Position",    0.f,     1.f,     false, false },
    { 10, "B Coarse",     -24.f,    24.f,    true,  false },
    { 11, "B Fine",       -100.f,   100.f,   false, false },
    { 12, "B Level",       0.f,     1.f,     false, false },
    { 13, "B Unison",      1.f,     7.f,     true,  false },
    { 14, "B Detune",      0.f,     100.f,   false, false },
    { 15, "B Spread",      0.f,     1.f,     false, false },
    { 16, "Noise",         0.f,     1.f,     false, false },
    { 17, "Sub",           0.f,     1.f,     false, false },
    { 18, "Cutoff",        20.f,    20000.f, false, true  },
    { 19, "Resonance",     0.f,     1.f,     false, false },
    { 20, "Filter Type",   0.f,     5.f,     true,  false },   // widened by v2
    { 21, "Drive",         0.f,     24.f,    false, false },
    { 22, "Env2>Cutoff",  -1.f,     1.f,     false, false },
    { 23, "Keytrack",      0.f,     1.f,     false, false },
    { 24, "Attack",        0.1f,    5000.f,  false, true  },
    { 25, "Decay",         1.f,     5000.f,  false, true  },
    { 26, "Sustain",       0.f,     1.f,     false, false },
    { 27, "Release",       1.f,     8000.f,  false, true  },
    { 28, "E2 Attack",     0.1f,    5000.f,  false, true  },
    { 29, "E2 Decay",      1.f,     5000.f,  false, true  },
    { 30, "E2 Sustain",    0.f,     1.f,     false, false },
    { 31, "E2 Release",    1.f,     8000.f,  false, true  },
    { 32, "LFO Rate",      0.01f,   40.f,    false, true  },
    { 33, "LFO Sync",      0.f,     9.f,     true,  false },
    { 34, "LFO>Position", -1.f,     1.f,     false, false },
    { 35, "LFO>Cutoff",   -1.f,     1.f,     false, false },
    { 36, "LFO>Pitch",     0.f,     100.f,   false, false },
    { 37, "LFO Shape",     0.f,     5.f,     true,  false },   // widened by v3
    { 38, "Glide",         0.f,     2000.f,  false, false },   // widened by v2
    { 39, "Voices",        1.f,     16.f,    true,  false },
    { 40, "Master",        0.f,     1.5f,    false, false },
    { 41, "Env2>Position",-1.f,     1.f,     false, false },
    // --- v2, ids 42..99 ("v2 — the parity push"). Reserved ids are
    // registered: name "—", 0..1, default 0.
    { 42, "Sub Shape",     0.f,     2.f,     true,  false },
    { 43, "Sub Oct",      -2.f,     0.f,     true,  false },
    { 44, "Noise Color",   0.f,     1.f,     false, false },
    { 45, "Noise Track",   0.f,     1.f,     true,  false },
    { 46, "—",             0.f,     1.f,     false, false },
    { 47, "—",             0.f,     1.f,     false, false },
    { 48, "A Warp",        0.f,     7.f,     true,  false },
    { 49, "A Warp Amt",    0.f,     1.f,     false, false },
    { 50, "B Warp",        0.f,     7.f,     true,  false },
    { 51, "B Warp Amt",    0.f,     1.f,     false, false },
    { 52, "—",             0.f,     1.f,     false, false },
    { 53, "—",             0.f,     1.f,     false, false },
    { 54, "L2 Rate",       0.01f,   40.f,    false, true  },
    { 55, "L2 Sync",       0.f,     9.f,     true,  false },
    { 56, "L2 Shape",      0.f,     5.f,     true,  false },   // widened by v3
    { 57, "L3 Rate",       0.01f,   40.f,    false, true  },
    { 58, "L3 Sync",       0.f,     9.f,     true,  false },
    { 59, "L3 Shape",      0.f,     5.f,     true,  false },   // widened by v3
    // v3 spends this block's two reserved ids: 0 Loop (v2 verbatim) · 1 One-shot.
    { 60, "L2 Mode",       0.f,     1.f,     true,  false },
    { 61, "L3 Mode",       0.f,     1.f,     true,  false },
    { 62, "E3 Attack",     0.1f,    5000.f,  false, true  },
    { 63, "E3 Decay",      1.f,     5000.f,  false, true  },
    { 64, "E3 Sustain",    0.f,     1.f,     false, false },
    { 65, "E3 Release",    1.f,     8000.f,  false, true  },
    { 66, "—",             0.f,     1.f,     false, false },
    { 67, "—",             0.f,     1.f,     false, false },
    { 68, "M1 Src",        0.f,     17.f,    true,  false },
    { 69, "M1 Dst",        0.f,     19.f,    true,  false },
    { 70, "M1 Amt",       -1.f,     1.f,     false, false },
    { 71, "M2 Src",        0.f,     17.f,    true,  false },
    { 72, "M2 Dst",        0.f,     19.f,    true,  false },
    { 73, "M2 Amt",       -1.f,     1.f,     false, false },
    { 74, "M3 Src",        0.f,     17.f,    true,  false },
    { 75, "M3 Dst",        0.f,     19.f,    true,  false },
    { 76, "M3 Amt",       -1.f,     1.f,     false, false },
    { 77, "M4 Src",        0.f,     17.f,    true,  false },
    { 78, "M4 Dst",        0.f,     19.f,    true,  false },
    { 79, "M4 Amt",       -1.f,     1.f,     false, false },
    { 80, "M5 Src",        0.f,     17.f,    true,  false },
    { 81, "M5 Dst",        0.f,     19.f,    true,  false },
    { 82, "M5 Amt",       -1.f,     1.f,     false, false },
    { 83, "M6 Src",        0.f,     17.f,    true,  false },
    { 84, "M6 Dst",        0.f,     19.f,    true,  false },
    { 85, "M6 Amt",       -1.f,     1.f,     false, false },
    { 86, "M7 Src",        0.f,     17.f,    true,  false },
    { 87, "M7 Dst",        0.f,     19.f,    true,  false },
    { 88, "M7 Amt",       -1.f,     1.f,     false, false },
    { 89, "M8 Src",        0.f,     17.f,    true,  false },
    { 90, "M8 Dst",        0.f,     19.f,    true,  false },
    { 91, "M8 Amt",       -1.f,     1.f,     false, false },
    { 92, "—",             0.f,     1.f,     false, false },
    { 93, "—",             0.f,     1.f,     false, false },
    { 94, "Macro 1",       0.f,     1.f,     false, false },
    { 95, "Macro 2",       0.f,     1.f,     false, false },
    { 96, "Macro 3",       0.f,     1.f,     false, false },
    { 97, "Macro 4",       0.f,     1.f,     false, false },
    { 98, "Voice Mode",    0.f,     2.f,     true,  false },
    // Bend Range: the revision's ONE stated exception to "every default does
    // what v2 did" — it defaults to 2 semitones, not 0.
    { 99, "Bend Range",    0.f,     24.f,    true,  false },
    // --- v3, ids 100..110 ("v3 — hands on the modulation").
    { 100, "L1 Mode",      0.f,     1.f,     true,  false },
    { 101, "M1 Curve",     0.f,     2.f,     true,  false },
    { 102, "M2 Curve",     0.f,     2.f,     true,  false },
    { 103, "M3 Curve",     0.f,     2.f,     true,  false },
    { 104, "M4 Curve",     0.f,     2.f,     true,  false },
    { 105, "M5 Curve",     0.f,     2.f,     true,  false },
    { 106, "M6 Curve",     0.f,     2.f,     true,  false },
    { 107, "M7 Curve",     0.f,     2.f,     true,  false },
    { 108, "M8 Curve",     0.f,     2.f,     true,  false },
    // --- v4, the arpeggiator ("v4 — the arpeggiator"). v3's generic reserved
    // tail (109, 110) is spent on the two ids that open the block and the
    // append runs to 124, so the whole feature is ONE contiguous run with no
    // hole in it. The other eight reserved ids (46, 47, 52, 53, 66, 67, 92, 93)
    // stay exactly where they are.
    { 109, "Arp On",       0.f,     1.f,     true,  false },
    { 110, "Arp Mode",     0.f,     9.f,     true,  false },
    { 111, "Arp Rate",     0.01f,   40.f,    false, true  },
    { 112, "Arp Sync",     0.f,     9.f,     true,  false },
    { 113, "Arp Octaves",  1.f,     4.f,     true,  false },
    { 114, "Arp Oct Mode", 0.f,     2.f,     true,  false },
    { 115, "Arp Gate",     1.f,     200.f,   false, false },
    { 116, "Arp Swing",    0.f,     100.f,   false, false },
    { 117, "Arp Hold",     0.f,     1.f,     true,  false },
    { 118, "Arp Retrig",   0.f,     1.f,     true,  false },
    { 119, "Arp Vel Mode", 0.f,     2.f,     true,  false },
    { 120, "Arp Fixed Vel",1.f,     127.f,   true,  false },
    { 121, "Arp Steps",    1.f,     16.f,    true,  false },
    { 122, "Arp Chance",   0.f,     100.f,   false, false },
    { 123, "—",            0.f,     1.f,     false, false },
    { 124, "—",            0.f,     1.f,     false, false },
};
static constexpr int kSpectraContractN =
    (int)(sizeof kSpectraContract / sizeof kSpectraContract[0]);

// The factory bank (v2): presets are CONTENT, not interface — the contract
// says "retiring or renaming presets never breaks a project" — so the suite
// checks the bank's RULES rather than transcribing its names: 49 rows, row 0
// Init, the category tags in contract order with the contract counts,
// alphabetical within a category, and the naming constraints. The bank file
// itself (src/plugin/spectra_presets.inc) is authored against the doc.
// v3 appends BANK 2: 48 more presets in the same seven categories, after
// bank 1 and never interleaved with it (a preset index is stored in no set,
// but the user-preset contract freezes the factory ORDER). v4 appends BANK 3,
// the arpeggiators. So the bank is Init + 48 + 48 + 24 and the category sweep
// below runs three times, over a PER-BANK table.
// v4 appends BANK 3: 24 arpeggiator presets, after banks 1 and 2 and never
// interleaved with them. It is a HALF-SIZE bank with its own distribution, and
// the distribution is deliberate rather than a scaled copy: an arpeggiator
// lives in Bass, Lead, Pluck and Sequence, so twenty of the twenty-four are
// there; Pad and Keys get one each for the two things only they show — a
// latched overlapping arp and a triplet ladder — and FX gets the Chance/Random
// one. So the bank table is PER BANK and not one row repeated: a third
// repetition of the 48-row distribution would be a different bank from the one
// that shipped.
struct SpectraPresetCat { const char* tag; int count; };
static const SpectraPresetCat kSpectraBank12[] = {
    { "BA", 9 }, { "LD", 9 }, { "PD", 8 }, { "KY", 7 },
    { "PL", 6 }, { "FX", 5 }, { "SQ", 4 },
};
static const SpectraPresetCat kSpectraBank3[] = {
    { "BA", 6 }, { "LD", 6 }, { "PD", 1 }, { "KY", 2 },
    { "PL", 4 }, { "FX", 1 }, { "SQ", 4 },
};
struct SpectraPresetBank { const SpectraPresetCat* cats; int n; int total; };
static const SpectraPresetBank kSpectraPresetBanks[] = {
    { kSpectraBank12, 7, 48 },
    { kSpectraBank12, 7, 48 },
    { kSpectraBank3,  7, 24 },
};
static constexpr int kSpectraPresetBankN =
    (int)(sizeof kSpectraPresetBanks / sizeof kSpectraPresetBanks[0]);
static constexpr int kSpectraPresetN = 1 + 48 + 48 + 24;
static const char* spPresetName(const PluginInstance& s, int k) {
    const char* n = s.presetName(k);
    return n ? n : "(null)";
}

// --- measurement helpers ---------------------------------------------------

// In-place radix-2 complex FFT. Only used by the tests, so clarity beats
// speed; the twiddles are computed inline in f64.
static void tFft(std::vector<f32>& re, std::vector<f32>& im) {
    const int n = (int)re.size();
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[(size_t)i], re[(size_t)j]); std::swap(im[(size_t)i], im[(size_t)j]); }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const f64 ang = -6.283185307179586 / (f64)len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < len / 2; ++k) {
                const f64 wr = std::cos(ang * k), wi = std::sin(ang * k);
                const int a = i + k, b = i + k + len / 2;
                const f64 vr = (f64)re[(size_t)b] * wr - (f64)im[(size_t)b] * wi;
                const f64 vi = (f64)re[(size_t)b] * wi + (f64)im[(size_t)b] * wr;
                re[(size_t)b] = (f32)((f64)re[(size_t)a] - vr);
                im[(size_t)b] = (f32)((f64)im[(size_t)a] - vi);
                re[(size_t)a] = (f32)((f64)re[(size_t)a] + vr);
                im[(size_t)a] = (f32)((f64)im[(size_t)a] + vi);
            }
        }
    }
}

// Magnitude of one frequency in a rendered buffer, by direct projection. Exact
// for a steady tone over a whole number of periods and good enough elsewhere;
// this is how the voice-stealing test watches individual notes appear and go.
static f64 tBinMag(const std::vector<f32>& x, int from, int n, f64 freq) {
    const f64 w = 6.283185307179586 * freq / kSR;
    f64 re = 0.0, im = 0.0;
    for (int i = 0; i < n; ++i) {
        const f64 ph = w * (f64)i;
        re += (f64)x[(size_t)(from + i)] * std::cos(ph);
        im += (f64)x[(size_t)(from + i)] * std::sin(ph);
    }
    return 2.0 * std::sqrt(re * re + im * im) / (f64)n;
}

static f64 tMidiHz(f64 note) { return 440.0 * std::pow(2.0, (note - 69.0) / 12.0); }

// One MIDI byte triple at an absolute frame. The render helper below delivers
// it in whatever block happens to contain that frame, which is exactly what
// the engine does and exactly what makes the block-size test meaningful.
struct SpEvent { int frame; u8 st, a, b; };

// Renders `frames` samples in chunks of `chunk`, delivering the (sorted) event
// list on the way. `bpm > 0` pushes the transport before every block, like the
// engine's process loop.
static void spRender(PluginInstance& p, const std::vector<SpEvent>& ev, int frames,
                     int chunk, std::vector<f32>& L, std::vector<f32>& R, f64 bpm = 0.0) {
    L.assign((size_t)frames, 0.f);
    R.assign((size_t)frames, 0.f);
    std::vector<f32> bl((size_t)chunk, 0.f), br((size_t)chunk, 0.f);
    size_t next = 0;
    for (int i = 0; i < frames; i += chunk) {
        const int n = (frames - i) < chunk ? (frames - i) : chunk;
        while (next < ev.size() && ev[next].frame < i + n) {
            const u8 m[3] = { ev[next].st, ev[next].a, ev[next].b };
            p.midi(m, 3, ev[next].frame - i);
            ++next;
        }
        if (bpm > 0.0) p.setTransport(bpm, 0.0, true);
        std::fill(bl.begin(), bl.begin() + n, 0.f);
        std::fill(br.begin(), br.begin() + n, 0.f);
        f32* o[2] = { bl.data(), br.data() };
        p.process(nullptr, o, 2, n);
        for (int j = 0; j < n; ++j) {
            L[(size_t)(i + j)] = bl[(size_t)j];
            R[(size_t)(i + j)] = br[(size_t)j];
        }
    }
}

static int spIdx(const PluginInstance& p, const char* name) { return paramIndex(p, name); }

// A patch that makes every path in the instrument move at once. Used by the
// determinism and state tests, where the point is to leave nothing switched
// off: two oscillators at full unison, noise, sub, a resonant driven filter, a
// modulated cutoff and position, vibrato, glide and a sample-and-hold LFO.
static void spBusyPatch(PluginInstance& s) {
    s.setParam(spIdx(s, "A Table"), 5.f);
    s.setParam(spIdx(s, "A Position"), 0.4f);
    s.setParam(spIdx(s, "A Level"), 0.7f);
    s.setParam(spIdx(s, "A Unison"), 7.f);
    s.setParam(spIdx(s, "A Detune"), 35.f);
    s.setParam(spIdx(s, "A Spread"), 0.8f);
    s.setParam(spIdx(s, "B Table"), 3.f);
    s.setParam(spIdx(s, "B Position"), 0.6f);
    s.setParam(spIdx(s, "B Level"), 0.5f);
    s.setParam(spIdx(s, "B Unison"), 5.f);
    s.setParam(spIdx(s, "B Detune"), 22.f);
    s.setParam(spIdx(s, "B Spread"), 0.6f);
    s.setParam(spIdx(s, "B Coarse"), 7.f);
    s.setParam(spIdx(s, "B Fine"), -13.f);
    s.setParam(spIdx(s, "Noise"), 0.15f);
    s.setParam(spIdx(s, "Sub"), 0.3f);
    s.setParam(spIdx(s, "Cutoff"), 900.f);
    s.setParam(spIdx(s, "Resonance"), 0.55f);
    s.setParam(spIdx(s, "Drive"), 9.f);
    s.setParam(spIdx(s, "Env2>Cutoff"), 0.7f);
    s.setParam(spIdx(s, "Keytrack"), 0.4f);
    s.setParam(spIdx(s, "Attack"), 3.f);
    s.setParam(spIdx(s, "Decay"), 800.f);
    s.setParam(spIdx(s, "Sustain"), 0.6f);
    s.setParam(spIdx(s, "Release"), 250.f);
    s.setParam(spIdx(s, "E2 Attack"), 1.f);
    s.setParam(spIdx(s, "E2 Decay"), 300.f);
    s.setParam(spIdx(s, "E2 Sustain"), 0.2f);
    s.setParam(spIdx(s, "LFO Rate"), 5.5f);
    s.setParam(spIdx(s, "LFO Shape"), 4.f);        // sample & hold
    s.setParam(spIdx(s, "LFO>Position"), 0.6f);
    s.setParam(spIdx(s, "LFO>Cutoff"), 0.5f);
    s.setParam(spIdx(s, "LFO>Pitch"), 25.f);
    s.setParam(spIdx(s, "Env2>Position"), -0.4f);
    s.setParam(spIdx(s, "Glide"), 60.f);
    s.setParam(spIdx(s, "Voices"), 6.f);
    s.setParam(spIdx(s, "Master"), 0.6f);
}

// The MIDI the busy patch is driven with: overlapping notes, a chord, a
// released note and one more note than the polyphony cap, so voice allocation,
// stealing and glide are all exercised.
static std::vector<SpEvent> spScript() {
    return {
        {     0, 0x90, 45,  90 },
        {   700, 0x90, 57, 120 },
        {  2300, 0x90, 64,  40 },
        {  4001, 0x80, 45,   0 },
        {  4600, 0x90, 69, 110 },
        {  5000, 0x90, 72,  20 },
        {  5003, 0x90, 76,  95 },
        {  5100, 0x90, 79,  70 },
        {  5111, 0x90, 84, 127 },     // one past the cap: steals
        {  8000, 0x80, 57,   0 },
        {  8100, 0x80, 64,   0 },
        { 11000, 0xB0, 123,  0 },     // all notes off
    };
}

// ---------------------------------------------------------------------------

// MUST RUN BEFORE ANY OTHER TEST INSTANTIATES A SPECTRA: the first check --
// spectraTables() is null until a Spectra has prepared -- is observable
// exactly once per process. main() calls this immediately before
// testSpectraContract(), which is the suite's first Spectra instantiation.
static void testSpectraTables(PluginRegistry& reg) {
    banner("Spectra wavetables, as the editor sees them");

    // 1. NULL BEFORE ANYTHING HAS PREPARED. This is the property the editor's
    //    fallback path exists for, and it is only observable once per process,
    //    so it has to be the first thing asked. It also says the accessor does
    //    not BUILD the set: calling it in a program with no Spectra in it must
    //    not spend ten megabytes and a second of FFTs on a picture nobody
    //    asked for.
    CHECK(detail::spectraTables() == nullptr,
          "null before any Spectra has prepared");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    CHECK(d != nullptr, "registry finds nxtakt:spectra");
    if (!d) return;

    auto syn = reg.instantiate(*d, kSR, kBlock);
    CHECK(syn != nullptr, "instantiate + prepare");
    if (!syn) return;

    const detail::SpectraTableSet* t = detail::spectraTables();
    CHECK(t != nullptr, "non-null once a Spectra has prepared");
    if (!t) return;

    // 2. GEOMETRY, against the file header's own numbers.
    CHECK(t->tables == 8 && t->frames == 32,
          "%d tables x %d frames", t->tables, t->frames);
    CHECK(t->len == 2048, "a mip-0 frame is %d samples", t->len);
    CHECK(t->stride == 10240, "the whole mip chain is %d floats a frame", t->stride);
    CHECK(t->data != nullptr, "the set has a pointer to point at");

    // 3. STABLE AND SHARED. A second instance must publish the same set, not a
    //    second one: the tables are built once per process and every voice in
    //    the program reads the same floats.
    auto syn2 = reg.instantiate(*d, kSR, kBlock);
    CHECK(syn2 != nullptr, "a second Spectra instantiates");
    const detail::SpectraTableSet* t2 = detail::spectraTables();
    CHECK(t2 == t && t2->data == t->data,
          "a second instance publishes the SAME set, not a second one");

    // Re-preparing must not move it either -- prepare() is called again on
    // every sample-rate change, and a display holding the pointer across one
    // would be reading freed memory if it did.
    syn->prepare(44100.0, 64);
    CHECK(detail::spectraTables() == t && detail::spectraTables()->data == t->data,
          "re-preparing at another rate does not move the set");

    // 4. THE FRAME PAIR IS THE OSCILLATOR'S. Position 0..1 spans 31 steps, and
    //    the pair is (f0, f0+1) with f0 stopping at frames-2 -- so the last
    //    frame is arrived at by blend, never stepped onto.
    {
        const detail::SpectraFrameView v0 = t->morph(0, 0.f);
        CHECK(v0.valid() && v0.a == t->frame(0, 0) && v0.b == t->frame(0, 1) &&
              std::fabs(v0.blend) < 1e-6f,
              "position 0.00 -> frames 0/1 at blend %.3f", (double)v0.blend);

        const detail::SpectraFrameView v1 = t->morph(0, 1.f);
        CHECK(v1.valid() && v1.a == t->frame(0, 30) && v1.b == t->frame(0, 31) &&
              std::fabs(v1.blend - 1.f) < 1e-6f,
              "position 1.00 -> frames 30/31 at blend %.3f", (double)v1.blend);

        const detail::SpectraFrameView vm = t->morph(0, 0.5f);
        CHECK(vm.valid() && vm.a == t->frame(0, 15) && vm.b == t->frame(0, 16) &&
              std::fabs(vm.blend - 0.5f) < 1e-5f,
              "position 0.50 -> frames 15/16 at blend %.3f", (double)vm.blend);

        // Out of range is clamped and not undefined: the panel reads Position
        // from a device that may not be a Spectra at all.
        const detail::SpectraFrameView vlo = t->morph(0, -3.f);
        const detail::SpectraFrameView vhi = t->morph(0, 7.f);
        CHECK(vlo.a == v0.a && vhi.b == v1.b, "position is clamped, not wrapped");
        CHECK(t->morph(-1, 0.5f).a == t->morph(0, 0.5f).a &&
              t->morph(99, 0.5f).a == t->morph(7, 0.5f).a,
              "the table index is clamped too");
    }

    // 5. at() IS THE FRAME LERP, exactly. Not approximately: the display and
    //    the voice have to agree about what "between two frames" means, and
    //    the voice's is a + (b - a) * blend.
    {
        const detail::SpectraFrameView v = t->morph(4, 0.37f);
        f64 worst = 0.0;
        for (int i = 0; i < v.len; ++i) {
            const f32 want = v.a[i] + (v.b[i] - v.a[i]) * v.blend;
            worst = std::fmax(worst, std::fabs((f64)(v.at(i) - want)));
        }
        CHECK(worst == 0.0, "at() is the frame lerp bit for bit (worst %g)", worst);
    }

    // 6. EVERY MIP-0 FRAME IS UNIT PEAK AND HAS NO DC. Both are properties the
    //    display leans on: it draws these without a normalising pass, and it
    //    draws them centred on the well's midline.
    {
        f64 worstPeak = 0.0, worstDc = 0.0;
        int worstT = -1, worstF = -1;
        for (int tb = 0; tb < t->tables; ++tb) {
            for (int fr = 0; fr < t->frames; ++fr) {
                const f32* f = t->frame(tb, fr);
                f64 pk = 0.0, sum = 0.0;
                for (int i = 0; i < t->len; ++i) {
                    pk = std::fmax(pk, std::fabs((f64)f[i]));
                    sum += (f64)f[i];
                }
                const f64 dp = std::fabs(pk - 1.0);
                if (dp > worstPeak) { worstPeak = dp; worstT = tb; worstF = fr; }
                worstDc = std::fmax(worstDc, std::fabs(sum) / (f64)t->len);
            }
        }
        CHECK(worstPeak < 1e-4, "every frame peaks at 1 (worst %g, table %d frame %d)",
              worstPeak, worstT, worstF);
        CHECK(worstDc < 1e-4, "no frame carries DC (worst mean %g)", worstDc);
    }

    // 7. THE EIGHT TABLES ARE EIGHT TABLES. A set where two of them held the
    //    same floats would draw eight identical pictures and nobody would
    //    notice for a year.
    {
        int same = 0;
        for (int a = 0; a < t->tables; ++a)
            for (int b = a + 1; b < t->tables; ++b) {
                const f32* fa = t->frame(a, 16);
                const f32* fb = t->frame(b, 16);
                f64 diff = 0.0;
                for (int i = 0; i < t->len; ++i) diff += std::fabs((f64)(fa[i] - fb[i]));
                if (diff / (f64)t->len < 1e-6) ++same;
            }
        CHECK(same == 0, "no two tables share a frame (%d collisions)", same);
    }

    // 8. AND THE FRAME AXIS MOVES. Frame 0 and frame 31 of a table must differ,
    //    or Position is a knob that does nothing on it.
    {
        int flat = 0;
        for (int tb = 0; tb < t->tables; ++tb) {
            const f32* f0 = t->frame(tb, 0);
            const f32* f31 = t->frame(tb, 31);
            f64 diff = 0.0;
            for (int i = 0; i < t->len; ++i) diff += std::fabs((f64)(f0[i] - f31[i]));
            if (diff / (f64)t->len < 1e-4) ++flat;
        }
        CHECK(flat == 0, "every table's frame axis moves (%d flat)", flat);
    }

    // 9. THE PEAK-PRESERVING DECIMATION the display uses, checked against the
    //    thing it promises: the extremes of every column survive it. Striding
    //    is the comparison, and it is here to show the difference is real and
    //    not a matter of taste.
    {
        const detail::SpectraFrameView v = t->morph(0, 0.f);   // Basic at a saw
        const int cols = 120;
        f32 decLo = 1e9f, decHi = -1e9f, strLo = 1e9f, strHi = -1e9f;
        for (int c = 0; c < cols; ++c) {
            const int i0 = (int)((long long)c * v.len / cols);
            int i1 = (int)((long long)(c + 1) * v.len / cols);
            if (i1 <= i0) i1 = i0 + 1;
            for (int i = i0; i < i1; ++i) {
                decLo = std::fmin(decLo, v.at(i));
                decHi = std::fmax(decHi, v.at(i));
            }
            strLo = std::fmin(strLo, v.at(i0));       // naive stride: first only
            strHi = std::fmax(strHi, v.at(i0));
        }
        f32 fullLo = 1e9f, fullHi = -1e9f;
        for (int i = 0; i < v.len; ++i) {
            fullLo = std::fmin(fullLo, v.at(i));
            fullHi = std::fmax(fullHi, v.at(i));
        }
        CHECK(decLo == fullLo && decHi == fullHi,
              "min/max decimation keeps the frame's extremes exactly (%.4f .. %.4f)",
              (double)decLo, (double)decHi);
        CHECK(strHi < fullHi || strLo > fullLo,
              "striding loses one (stride %.4f .. %.4f vs %.4f .. %.4f)",
              (double)strLo, (double)strHi, (double)fullLo, (double)fullHi);
    }
}

static void testSpectraContract(PluginRegistry& reg) {
    banner("Spectra: the frozen parameter contract");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    CHECK(d != nullptr, "registry finds nxtakt:spectra");
    if (!d) return;

    CHECK(d->kind == PluginKind::Instrument, "descriptor: instrument");
    CHECK(d->format == PluginFormat::Internal, "descriptor: internal");
    CHECK(d->audioIn == 0 && d->audioOut == 2 && d->hasMidiIn,
          "descriptor: %d in, %d out, midi %s", d->audioIn, d->audioOut,
          d->hasMidiIn ? "yes" : "no");
    CHECK(d->paramCount == kSpectraContractN,
          "descriptor advertises %d parameters (contract says %d)",
          d->paramCount, kSpectraContractN);

    auto s = reg.instantiate(*d, kSR, kBlock);
    CHECK(s != nullptr, "instantiate + prepare");
    if (!s) return;

    CHECK(s->paramCount() == kSpectraContractN,
          "the instance reports %d parameters (contract says %d)",
          s->paramCount(), kSpectraContractN);
    if (s->paramCount() != kSpectraContractN) return;

    // Every row, mechanically. A mismatch names the parameter and the field, so
    // whichever side moved is obvious from the failure line alone.
    int bad = 0;
    for (int i = 0; i < kSpectraContractN; ++i) {
        const SpectraParamSpec& c = kSpectraContract[i];
        const ParamInfo& pi = s->paramInfo(i);
        if (pi.name != c.name) {
            CHECK(false, "id %d: name is \"%s\", contract says \"%s\"",
                  i, pi.name.c_str(), c.name);
            ++bad;
            continue;
        }
        if ((int)pi.id != c.id) { CHECK(false, "%s: id is %u, contract says %d", c.name, pi.id, c.id); ++bad; }
        if (pi.min != c.mn)     { CHECK(false, "%s: min is %g, contract says %g", c.name, (double)pi.min, (double)c.mn); ++bad; }
        if (pi.max != c.mx)     { CHECK(false, "%s: max is %g, contract says %g", c.name, (double)pi.max, (double)c.mx); ++bad; }
        if (pi.isInt != c.isInt){ CHECK(false, "%s: isInt is %d, contract says %d", c.name, (int)pi.isInt, (int)c.isInt); ++bad; }
        if (pi.isLogarithmic != c.isLog) {
            CHECK(false, "%s: isLogarithmic is %d, contract says %d",
                  c.name, (int)pi.isLogarithmic, (int)c.isLog);
            ++bad;
        }
        if (pi.def < pi.min || pi.def > pi.max) {
            CHECK(false, "%s: default %g is outside [%g, %g]",
                  c.name, (double)pi.def, (double)pi.min, (double)pi.max);
            ++bad;
        }
    }
    CHECK(bad == 0, "all %d parameters match the contract in name, id, range, "
                    "int flag and log flag", kSpectraContractN);

    // The one default the contract states in words rather than in the table.
    CHECK(s->getParam(12) == 0.f, "B Level defaults to 0 — osc B is off out of the box");

    // Ids are indices, and that is the property that makes a saved set
    // restorable. Stated as its own check because it is the thing a careless
    // insertion breaks.
    bool ids = true;
    for (int i = 0; i < s->paramCount(); ++i) if ((int)s->paramInfo(i).id != i) ids = false;
    CHECK(ids, "every ParamInfo::id equals its index");

    // setParamRT is the base class's plain-store path; the instrument must not
    // have broken it.
    CHECK(s->setParamRT(18, 1234.f), "setParamRT accepted (the automation path works)");
    CHECK(s->getParam(18) == 1234.f, "setParamRT wrote the value (%g)", (double)s->getParam(18));
    CHECK(s->setParamRT(1000, 1.f), "setParamRT out of range is 'ignored', not 'no RT path'");
    s->setParam(18, 20000.f);

    // Out-of-range writes clamp on both paths rather than corrupting state.
    s->setParam(5, 99.f);
    CHECK(s->getParam(5) == 7.f, "setParam clamps to the declared max (%g)", (double)s->getParam(5));
    s->setParam(5, -99.f);
    CHECK(s->getParam(5) == 1.f, "setParam clamps to the declared min (%g)", (double)s->getParam(5));

    CHECK(s->latencyFrames() == 0, "reports zero latency (a generator, no lookahead)");
}

// ---------------------------------------------------------------------------

static void testSpectraVoices(PluginRegistry& reg) {
    banner("Spectra: notes, polyphony and voice stealing");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;
    auto s = reg.instantiate(*d, kSR, kBlock);
    if (!s) return;

    Buf out;

    // 1. silence in, silence out.
    CHECK(runFor(*s, out, 8) == 0.f, "no midi -> silence");

    // 2. a note sounds.
    s->setParam(spIdx(*s, "Attack"), 2.f);
    s->setParam(spIdx(*s, "Decay"), 4000.f);
    s->setParam(spIdx(*s, "Sustain"), 0.8f);
    s->setParam(spIdx(*s, "Release"), 60.f);
    noteOn(*s, 60, 100);
    bool fin = false;
    const f32 held = runFor(*s, out, 20, &fin);
    CHECK(held > 0.05f, "note on -> sound (peak %.4f)", (double)held);
    CHECK(fin, "held note stays finite");

    // 3. note off returns to silence inside the release, and the release is the
    //    thing that bounds it: 60 ms asked for, checked at 400 ms.
    noteOff(*s, 60);
    runFor(*s, out, (int)(0.4 * kSR / kBlock));
    CHECK(runFor(*s, out, 8) == 0.f, "note off -> silence well inside the release");

    // 4. sixteen simultaneous notes: the full polyphony, all of it audible.
    s->setParam(spIdx(*s, "Voices"), 16.f);
    s->setParam(spIdx(*s, "Master"), 0.3f);
    for (int i = 0; i < 16; ++i) noteOn(*s, (u8)(40 + i * 3), (u8)(70 + i * 3), i * 4);
    const f32 chord = runFor(*s, out, 30, &fin);
    CHECK(fin, "16 simultaneous notes stay finite");
    CHECK(chord > 0.05f, "16 simultaneous notes sound (peak %.4f)", (double)chord);

    // 5. a seventeenth has to steal rather than allocate.
    noteOn(*s, 88, 120);
    CHECK(runFor(*s, out, 8, &fin) > 0.f && fin, "the 17th note steals cleanly");
    const u8 cc[3] = { 0xB0, 123, 0 };
    s->midi(cc, 3, 0);
    runFor(*s, out, (int)(kSR / kBlock));
    CHECK(runFor(*s, out, 8) == 0.f, "CC 123 all-notes-off returns to silence");

    // 6. STEALING THE QUIETEST, measured rather than asserted.
    //
    //    The sub oscillator alone, so every voice is one clean sine an octave
    //    below its note and the three of them can be told apart by projection.
    //    Two voices of polyphony; a loud note, a quiet one, then a third. The
    //    quiet one must be the one that goes.
    {
        auto v = reg.instantiate(*d, kSR, kBlock);
        if (!v) return;
        v->setParam(spIdx(*v, "A Level"), 0.f);
        v->setParam(spIdx(*v, "Sub"), 1.f);
        v->setParam(spIdx(*v, "Cutoff"), 20000.f);
        v->setParam(spIdx(*v, "Resonance"), 0.f);
        v->setParam(spIdx(*v, "Attack"), 1.f);
        v->setParam(spIdx(*v, "Decay"), 5000.f);
        v->setParam(spIdx(*v, "Sustain"), 1.f);
        v->setParam(spIdx(*v, "Release"), 2.f);
        v->setParam(spIdx(*v, "Voices"), 2.f);
        v->setParam(spIdx(*v, "Master"), 1.f);

        const f64 fLoud  = tMidiHz(72 - 12);      // sub is one octave down
        const f64 fQuiet = tMidiHz(76 - 12);
        const f64 fNew   = tMidiHz(79 - 12);

        std::vector<f32> L, R;
        const std::vector<SpEvent> ev = {
            {     0, 0x90, 72, 127 },     // loud
            {    10, 0x90, 76,   1 },     // quiet: velocity floor is 30%
            { 24000, 0x90, 79, 100 },     // must steal the quiet one
        };
        spRender(*v, ev, 48000, kBlock, L, R);

        const f64 loudBefore  = tBinMag(L, 12000, 9600, fLoud);
        const f64 quietBefore = tBinMag(L, 12000, 9600, fQuiet);
        const f64 loudAfter   = tBinMag(L, 36000, 9600, fLoud);
        const f64 quietAfter  = tBinMag(L, 36000, 9600, fQuiet);
        const f64 newAfter    = tBinMag(L, 36000, 9600, fNew);

        CHECK(loudBefore > 0.2 && quietBefore > 0.05,
              "both notes present before the steal (loud %.3f, quiet %.3f)",
              loudBefore, quietBefore);
        CHECK(quietBefore < loudBefore * 0.6,
              "the second note really is the quieter one (%.3f vs %.3f)",
              quietBefore, loudBefore);
        CHECK(newAfter > 0.2, "the new note sounds after the steal (%.3f)", newAfter);
        CHECK(loudAfter > loudBefore * 0.8,
              "the LOUD note survived the steal (%.3f, was %.3f)", loudAfter, loudBefore);
        CHECK(quietAfter < quietBefore * 0.1,
              "the QUIET note is the one that was stolen (%.4f, was %.4f)",
              quietAfter, quietBefore);
    }

    // 7. the polyphony cap is a cap: with Voices = 1 the second note replaces
    //    the first outright.
    {
        auto v = reg.instantiate(*d, kSR, kBlock);
        if (!v) return;
        v->setParam(spIdx(*v, "A Level"), 0.f);
        v->setParam(spIdx(*v, "Sub"), 1.f);
        v->setParam(spIdx(*v, "Attack"), 1.f);
        v->setParam(spIdx(*v, "Decay"), 5000.f);
        v->setParam(spIdx(*v, "Sustain"), 1.f);
        v->setParam(spIdx(*v, "Voices"), 1.f);
        v->setParam(spIdx(*v, "Master"), 1.f);

        std::vector<f32> L, R;
        const std::vector<SpEvent> ev = { { 0, 0x90, 60, 100 }, { 24000, 0x90, 67, 100 } };
        spRender(*v, ev, 48000, kBlock, L, R);
        const f64 first = tBinMag(L, 36000, 9600, tMidiHz(48));
        const f64 second = tBinMag(L, 36000, 9600, tMidiHz(55));
        CHECK(second > 0.2 && first < 0.02,
              "Voices = 1 is monophonic: the new note replaced the old (%.4f vs %.4f)",
              second, first);
    }
}

// ---------------------------------------------------------------------------
// Sound quality. Three claims, three measurements.
// ---------------------------------------------------------------------------

static void testSpectraQuality(PluginRegistry& reg) {
    banner("Spectra: band-limiting, unison width and envelope edges");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;

    // --- 1. ALIASING AT C7, every table, both ends of the position axis.
    //
    // A wavetable oscillator at 2093 Hz has room for eleven harmonics below
    // Nyquist. Everything else in the spectrum is either aliasing or
    // interpolation noise, and the mip chain exists to keep it inaudible. The
    // measurement is a 16384-point Hann-windowed FFT of the steady part of the
    // note: energy within 6% of a multiple of f0 is signal, everything else is
    // not, and the ratio has to sit below -60 dB.
    {
        const int kN = 16384;
        f64 worst = -300.0;
        const char* worstAt = "";
        bool all = true;
        for (int tbl = 0; tbl < 8; ++tbl) {
            for (int end = 0; end < 2; ++end) {
                auto s = reg.instantiate(*d, kSR, kBlock);
                if (!s) return;
                s->setParam(spIdx(*s, "A Table"), (f32)tbl);
                s->setParam(spIdx(*s, "A Position"), end ? 1.f : 0.f);
                s->setParam(spIdx(*s, "A Level"), 1.f);
                s->setParam(spIdx(*s, "A Unison"), 1.f);
                s->setParam(spIdx(*s, "Attack"), 5.f);
                s->setParam(spIdx(*s, "Decay"), 5000.f);
                s->setParam(spIdx(*s, "Sustain"), 1.f);
                s->setParam(spIdx(*s, "Cutoff"), 20000.f);
                s->setParam(spIdx(*s, "Resonance"), 0.f);
                s->setParam(spIdx(*s, "Master"), 1.f);

                std::vector<f32> L, R;
                const std::vector<SpEvent> ev = { { 0, 0x90, 96, 127 } };   // C7
                spRender(*s, ev, 8192 + kN, kBlock, L, R);

                std::vector<f32> re((size_t)kN), im((size_t)kN, 0.f);
                for (int i = 0; i < kN; ++i)
                    re[(size_t)i] = L[(size_t)(8192 + i)] *
                        (f32)(0.5 - 0.5 * std::cos(6.283185307179586 * i / kN));
                tFft(re, im);

                const f64 f0 = tMidiHz(96);
                const f64 binHz = kSR / (f64)kN;
                f64 sig = 0.0, alias = 0.0;
                for (int k = 4; k < kN / 2; ++k) {
                    const f64 h  = (f64)k * binHz / f0;
                    const f64 dh = std::fabs(h - std::floor(h + 0.5));
                    const f64 e  = (f64)re[(size_t)k] * re[(size_t)k] +
                                   (f64)im[(size_t)k] * im[(size_t)k];
                    if (std::floor(h + 0.5) >= 1.0 && dh < 0.06) sig += e;
                    else                                          alias += e;
                }
                const f64 db = 10.0 * std::log10(alias / (sig + 1e-30));
                if (db > worst) { worst = db; worstAt = kSpectraContract[0].name; }
                if (!(db < -60.0)) {
                    all = false;
                    CHECK(false, "table %d at position %d: alias/signal %.1f dB at C7 "
                                 "(must be below -60)", tbl, end, db);
                }
            }
        }
        (void)worstAt;
        CHECK(all, "C7, all 8 tables x both position extremes: alias energy peaks at "
                   "%.1f dB below the harmonics (limit -60)", worst);
    }

    // --- 2. THE POSITION SWEEP ITSELF must not alias either: modulating the
    // frame axis crossfades two band-limited frames, so the result stays band
    // limited. Swept by an LFO across the whole axis while the same note holds.
    {
        auto s = reg.instantiate(*d, kSR, kBlock);
        if (!s) return;
        s->setParam(spIdx(*s, "A Table"), 5.f);        // the brightest table
        s->setParam(spIdx(*s, "A Position"), 0.5f);
        s->setParam(spIdx(*s, "A Level"), 1.f);
        s->setParam(spIdx(*s, "LFO Rate"), 6.f);
        s->setParam(spIdx(*s, "LFO>Position"), 1.f);
        s->setParam(spIdx(*s, "Attack"), 5.f);
        s->setParam(spIdx(*s, "Decay"), 5000.f);
        s->setParam(spIdx(*s, "Sustain"), 1.f);
        s->setParam(spIdx(*s, "Cutoff"), 20000.f);
        s->setParam(spIdx(*s, "Master"), 1.f);

        const int kN = 16384;
        std::vector<f32> L, R;
        const std::vector<SpEvent> ev = { { 0, 0x90, 96, 127 } };
        spRender(*s, ev, 8192 + kN, kBlock, L, R);

        std::vector<f32> re((size_t)kN), im((size_t)kN, 0.f);
        for (int i = 0; i < kN; ++i)
            re[(size_t)i] = L[(size_t)(8192 + i)] *
                (f32)(0.5 - 0.5 * std::cos(6.283185307179586 * i / kN));
        tFft(re, im);

        // With the position moving, the harmonics are no longer lines, so the
        // honest measurement is coarser: everything above the last harmonic
        // that fits under Nyquist is aliasing and nothing else can be.
        const f64 f0 = tMidiHz(96);
        const f64 binHz = kSR / (f64)kN;
        const f64 top = std::floor(kSR * 0.5 / f0) * f0;        // 11 * f0
        f64 sig = 0.0, above = 0.0;
        for (int k = 4; k < kN / 2; ++k) {
            const f64 f = (f64)k * binHz;
            const f64 e = (f64)re[(size_t)k] * re[(size_t)k] +
                          (f64)im[(size_t)k] * im[(size_t)k];
            if (f < top + 0.25 * f0) sig += e; else above += e;
        }
        const f64 db = 10.0 * std::log10(above / (sig + 1e-30));
        CHECK(db < -60.0,
              "a full-range LFO position sweep at C7 leaves %.1f dB above the last "
              "harmonic that fits (limit -60)", db);
    }

    // --- 3. UNISON THAT SURVIVES A MONO FOLD.
    //
    // Seven voices, maximum detune, maximum spread. Panning is constant power
    // and never inverts a polarity, so the mono sum cannot cancel; the number
    // below is what says so. 70.7% is the floor for perfectly decorrelated
    // equal-power channels, so anything near it is the physics and not a bug.
    {
        auto s = reg.instantiate(*d, kSR, kBlock);
        if (!s) return;
        s->setParam(spIdx(*s, "A Unison"), 7.f);
        s->setParam(spIdx(*s, "A Detune"), 40.f);
        s->setParam(spIdx(*s, "A Spread"), 1.f);
        s->setParam(spIdx(*s, "A Level"), 1.f);
        s->setParam(spIdx(*s, "Attack"), 5.f);
        s->setParam(spIdx(*s, "Decay"), 5000.f);
        s->setParam(spIdx(*s, "Sustain"), 1.f);
        s->setParam(spIdx(*s, "Cutoff"), 20000.f);

        std::vector<f32> L, R;
        const std::vector<SpEvent> ev = { { 0, 0x90, 57, 110 } };
        spRender(*s, ev, 96000, kBlock, L, R);

        f64 es = 0.0, em = 0.0;
        const int from = 24000, n = 72000;
        for (int i = from; i < from + n; ++i) {
            es += (f64)L[(size_t)i] * L[(size_t)i] + (f64)R[(size_t)i] * R[(size_t)i];
            const f64 m = 0.5 * ((f64)L[(size_t)i] + (f64)R[(size_t)i]);
            em += m * m;
        }
        const f64 rmsS = std::sqrt(es / (2.0 * n));
        const f64 rmsM = std::sqrt(em / (f64)n);
        CHECK(rmsS > 0.01, "the 7-voice unison note is actually sounding (RMS %.4f)", rmsS);
        CHECK(rmsM >= 0.5 * rmsS,
              "mono sum keeps %.1f%% of the stereo RMS (floor 50%%) — the fan widens "
              "without cancelling", 100.0 * rmsM / rmsS);

        // And it IS wide: identical channels would keep 100%, so a number
        // strictly below that is the proof the spread did something.
        CHECK(rmsM < 0.99 * rmsS, "and the channels are genuinely different (%.1f%%)",
              100.0 * rmsM / rmsS);
    }

    // --- 4. THE ENVELOPE EDGES.
    //
    // Note-on with a 50 ms attack must ramp, not step. Note-off must FADE: the
    // tail's block peaks fall monotonically and the last non-zero sample is
    // 100 dB below the note, so the envelope's own floor is what ends it rather
    // than a jump to zero.
    {
        auto s = reg.instantiate(*d, kSR, kBlock);
        if (!s) return;
        s->setParam(spIdx(*s, "A Level"), 1.f);
        s->setParam(spIdx(*s, "Attack"), 50.f);
        s->setParam(spIdx(*s, "Decay"), 5000.f);
        s->setParam(spIdx(*s, "Sustain"), 1.f);
        s->setParam(spIdx(*s, "Release"), 200.f);
        s->setParam(spIdx(*s, "Cutoff"), 20000.f);
        s->setParam(spIdx(*s, "Master"), 1.f);

        std::vector<f32> L, R;
        const std::vector<SpEvent> ev = { { 0, 0x90, 60, 110 }, { 48000, 0x80, 60, 0 } };
        spRender(*s, ev, 48000 + 96000, kBlock, L, R);

        f32 sustain = 0.f;
        for (int i = 24000; i < 48000; ++i) sustain = std::fmax(sustain, std::fabs(L[(size_t)i]));
        f32 firstMs = 0.f;
        for (int i = 0; i < 48; ++i) firstMs = std::fmax(firstMs, std::fabs(L[(size_t)i]));
        CHECK(sustain > 0.05f, "note sounds (sustain peak %.4f)", (double)sustain);
        CHECK(firstMs < 0.05f * sustain,
              "a 50 ms attack has moved only %.2f%% of the way after 1 ms — no note-on step",
              100.0 * (double)(firstMs / sustain));

        // Block peaks over the tail, 5 ms apart. The envelope is a one-pole, so
        // its peaks are monotone; the oscillator underneath makes each window's
        // peak a little noisy, hence the 2% slack.
        const int win = 240;
        f32 prev = 1e30f;
        bool mono = true;
        int lastAt = -1;
        f32 lastMag = 0.f;
        for (int i = 48000; i + win <= 48000 + 96000; i += win) {
            f32 pk = 0.f;
            for (int j = 0; j < win; ++j) pk = std::fmax(pk, std::fabs(L[(size_t)(i + j)]));
            if (pk > prev * 1.02f + 1e-9f) mono = false;
            prev = pk;
        }
        for (int i = 48000; i < 48000 + 96000; ++i)
            if (L[(size_t)i] != 0.f) { lastAt = i; lastMag = std::fabs(L[(size_t)i]); }
        CHECK(mono, "the release tail decays monotonically — it is a fade, not a gate");
        CHECK(lastAt > 48000 + 4800,
              "the tail lasts %.0f ms, i.e. the Release parameter bounds it",
              (lastAt - 48000) / 48.0);
        CHECK(lastMag < 1e-4f * sustain,
              "the last non-zero sample is %.2e, %.0f dB below the note: the envelope "
              "floor ends it, not a step to zero",
              (double)lastMag, 20.0 * std::log10((double)(lastMag / sustain)));
    }

    // --- 5. the 0.1 ms attack IS allowed to click — that is the user's choice,
    // and the check is only that it stays bounded and finite rather than that
    // it is smooth.
    {
        auto s = reg.instantiate(*d, kSR, kBlock);
        if (!s) return;
        s->setParam(spIdx(*s, "Attack"), 0.1f);
        s->setParam(spIdx(*s, "A Level"), 1.f);
        s->setParam(spIdx(*s, "Sustain"), 1.f);
        s->setParam(spIdx(*s, "Decay"), 5000.f);
        std::vector<f32> L, R;
        const std::vector<SpEvent> ev = { { 0, 0x90, 60, 127 } };
        spRender(*s, ev, 4800, kBlock, L, R);
        bool fin = true;
        f32 pk = 0.f;
        for (f32 v : L) { if (!std::isfinite(v)) fin = false; pk = std::fmax(pk, std::fabs(v)); }
        CHECK(fin && pk < 4.f,
              "the shortest attack (0.1 ms) is bounded and finite (peak %.3f)", (double)pk);
    }
}

// ---------------------------------------------------------------------------

static void testSpectraDeterminism(PluginRegistry& reg) {
    banner("Spectra: the render does not depend on the block size");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;

    const int kFrames = 14000;
    const std::vector<SpEvent> ev = spScript();

    auto build = [&]() -> std::unique_ptr<PluginInstance> {
        auto s = reg.instantiate(*d, kSR, kBlock);
        if (s) spBusyPatch(*s);
        return s;
    };

    std::vector<f32> refL, refR, altL, altR;
    auto ref = build();
    if (!ref) return;
    spRender(*ref, ev, kFrames, kBlock, refL, refR, 120.0);

    f32 energy = 0.f;
    for (f32 v : refL) energy = std::fmax(energy, std::fabs(v));
    CHECK(energy > 0.05f, "the reference render is not silent (peak %.4f)", (double)energy);

    bool allSame = true;
    for (int chunk : { 1, 7, 64, 300, 1024 }) {
        auto alt = build();
        if (!alt) break;
        spRender(*alt, ev, kFrames, chunk, altL, altR, 120.0);
        f32 diff = 0.f;
        for (int i = 0; i < kFrames; ++i) {
            diff = std::fmax(diff, std::fabs(refL[(size_t)i] - altL[(size_t)i]));
            diff = std::fmax(diff, std::fabs(refR[(size_t)i] - altR[(size_t)i]));
        }
        if (diff != 0.f) allSame = false;
        CHECK(diff == 0.f,
              "blocks of %d are bit-identical to blocks of %d (max diff %.9f)",
              chunk, kBlock, (double)diff);
    }
    CHECK(allSame, "every control path — envelopes, the LFO, sample-and-hold, the "
                   "filter's control tick, glide — runs on absolute sample time");

    // The same instance twice: the random streams are seeded in prepare(), so a
    // re-prepared device renders the same file again. A wall-clock seed would
    // fail here and only here.
    {
        auto a = build();
        auto b = build();
        if (a && b) {
            std::vector<f32> aL, aR, bL, bR;
            spRender(*a, ev, kFrames, kBlock, aL, aR, 120.0);
            spRender(*b, ev, kFrames, kBlock, bL, bR, 120.0);
            f32 diff = 0.f;
            for (int i = 0; i < kFrames; ++i)
                diff = std::fmax(diff, std::fabs(aL[(size_t)i] - bL[(size_t)i]));
            CHECK(diff == 0.f,
                  "two fresh instances render identically — the noise, the phase "
                  "randomisation and the sample-and-hold are seeded, not clocked");
        }
    }
}

// ---------------------------------------------------------------------------

static void testSpectraModulation(PluginRegistry& reg) {
    banner("Spectra: transport-synced LFO and glide");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;

    // --- 1. a quarter note at 120 BPM IS 2 Hz. Not approximately: the two
    // renders have to be bit-identical, which is the Delay's test pattern and
    // the only version of this claim that cannot drift.
    {
        auto mk = [&](bool sync) -> std::unique_ptr<PluginInstance> {
            auto s = reg.instantiate(*d, kSR, kBlock);
            if (!s) return s;
            s->setParam(spIdx(*s, "A Level"), 1.f);
            s->setParam(spIdx(*s, "Cutoff"), 700.f);
            s->setParam(spIdx(*s, "Resonance"), 0.6f);
            s->setParam(spIdx(*s, "LFO>Cutoff"), 0.9f);
            s->setParam(spIdx(*s, "Attack"), 3.f);
            s->setParam(spIdx(*s, "Decay"), 5000.f);
            s->setParam(spIdx(*s, "Sustain"), 1.f);
            if (sync) s->setParam(spIdx(*s, "LFO Sync"), 5.f);      // 1/4
            else      s->setParam(spIdx(*s, "LFO Rate"), 2.f);
            return s;
        };
        const std::vector<SpEvent> ev = { { 0, 0x90, 45, 110 } };
        std::vector<f32> fL, fR, sL, sR;
        auto freeRun = mk(false);
        auto synced  = mk(true);
        if (freeRun && synced) {
            spRender(*freeRun, ev, 48000, kBlock, fL, fR, 0.0);      // no transport
            spRender(*synced,  ev, 48000, kBlock, sL, sR, 120.0);    // 120 BPM pushed
            f32 diff = 0.f, pk = 0.f;
            for (int i = 0; i < 48000; ++i) {
                diff = std::fmax(diff, std::fabs(fL[(size_t)i] - sL[(size_t)i]));
                pk   = std::fmax(pk, std::fabs(fL[(size_t)i]));
            }
            CHECK(pk > 0.05f, "the modulated note sounds (peak %.4f)", (double)pk);
            CHECK(diff == 0.f,
                  "LFO Sync = 1/4 at a pushed 120 BPM is bit-identical to 2 Hz free "
                  "(max diff %.9f)", (double)diff);
        }
    }

    // --- 2. the sync divisions are the ones the contract lists. Half the tempo
    // is half the rate, and a 1/8 is twice a 1/4: checked by counting the LFO's
    // effect rather than by reading a number out of the device.
    {
        auto mk = [&](int sync, f32 rate) -> std::unique_ptr<PluginInstance> {
            auto s = reg.instantiate(*d, kSR, kBlock);
            if (!s) return s;
            s->setParam(spIdx(*s, "A Level"), 1.f);
            s->setParam(spIdx(*s, "Cutoff"), 700.f);
            s->setParam(spIdx(*s, "Resonance"), 0.6f);
            s->setParam(spIdx(*s, "LFO>Cutoff"), 0.9f);
            s->setParam(spIdx(*s, "Attack"), 3.f);
            s->setParam(spIdx(*s, "Decay"), 5000.f);
            s->setParam(spIdx(*s, "Sustain"), 1.f);
            s->setParam(spIdx(*s, "LFO Sync"), (f32)sync);
            s->setParam(spIdx(*s, "LFO Rate"), rate);
            return s;
        };
        // (division index, the free rate it must equal at 120 BPM)
        const struct { int div; f32 hz; const char* label; } kCases[] = {
            { 1, 0.125f, "4 bars" }, { 3, 0.5f, "1 bar" }, { 4, 1.f, "1/2" },
            { 6, 4.f, "1/8" }, { 7, 8.f, "1/16" },
        };
        const std::vector<SpEvent> ev = { { 0, 0x90, 45, 110 } };
        for (const auto& c : kCases) {
            auto a = mk(c.div, 2.f);
            auto b = mk(0, c.hz);
            if (!a || !b) continue;
            std::vector<f32> aL, aR, bL, bR;
            spRender(*a, ev, 24000, kBlock, aL, aR, 120.0);
            spRender(*b, ev, 24000, kBlock, bL, bR, 0.0);
            f32 diff = 0.f;
            for (int i = 0; i < 24000; ++i)
                diff = std::fmax(diff, std::fabs(aL[(size_t)i] - bL[(size_t)i]));
            CHECK(diff == 0.f, "division %d (%s) at 120 BPM == %g Hz free (diff %.9f)",
                  c.div, c.label, (double)c.hz, (double)diff);
        }
    }

    // --- 3. GLIDE IS MONOTONIC. The sub oscillator alone, so the output is one
    // clean sine and its instantaneous period can be read off the zero
    // crossings. A rising glide must shorten every period, with no overshoot,
    // no wobble and no step at the end.
    {
        auto s = reg.instantiate(*d, kSR, kBlock);
        if (!s) return;
        s->setParam(spIdx(*s, "A Level"), 0.f);
        s->setParam(spIdx(*s, "Sub"), 1.f);
        s->setParam(spIdx(*s, "Cutoff"), 20000.f);
        s->setParam(spIdx(*s, "Resonance"), 0.f);
        s->setParam(spIdx(*s, "Attack"), 1.f);
        s->setParam(spIdx(*s, "Decay"), 5000.f);
        s->setParam(spIdx(*s, "Sustain"), 1.f);
        s->setParam(spIdx(*s, "Release"), 2.f);
        s->setParam(spIdx(*s, "Voices"), 1.f);
        s->setParam(spIdx(*s, "Glide"), 300.f);
        s->setParam(spIdx(*s, "Master"), 1.f);

        // Note 48, let it stop, then note 72: the second note glides up from
        // where the first one was, over 300 ms.
        const std::vector<SpEvent> ev = {
            {     0, 0x90, 48, 110 },
            {  9600, 0x80, 48,   0 },
            { 12000, 0x90, 72, 110 },
        };
        std::vector<f32> L, R;
        spRender(*s, ev, 48000, kBlock, L, R);

        // Rising zero crossings, interpolated, over the glide window.
        std::vector<f64> cross;
        for (int i = 12300; i < 12000 + 14400; ++i) {
            const f32 a = L[(size_t)(i - 1)], b = L[(size_t)i];
            if (a <= 0.f && b > 0.f) cross.push_back((f64)i - (f64)b / ((f64)b - (f64)a));
        }
        CHECK(cross.size() > 20, "the glide window has %zu cycles to measure", cross.size());

        bool monotone = true;
        f64 firstP = 0.0, lastP = 0.0;
        for (size_t i = 2; i < cross.size(); ++i) {
            const f64 prev = cross[i - 1] - cross[i - 2];
            const f64 cur  = cross[i] - cross[i - 1];
            if (i == 2) firstP = prev;
            lastP = cur;
            // 0.6% of slack: the crossings are read off a sine that is itself
            // changing frequency inside each cycle.
            if (cur > prev * 1.006 + 0.05) monotone = false;
        }
        CHECK(monotone, "every cycle of the glide is shorter than the one before it — "
                        "the pitch rises monotonically");
        CHECK(firstP > lastP * 3.0,
              "and it covers the whole two octaves (%.1f samples/cycle -> %.1f)",
              firstP, lastP);

        // After the glide the pitch has ARRIVED, exactly, rather than
        // asymptotically approaching: constant-time portamento finishes.
        const f64 target = tMidiHz(72 - 12);
        const f64 mag = tBinMag(L, 30000, 12000, target);
        CHECK(mag > 0.4, "the glide lands on the target pitch (magnitude %.3f at %.1f Hz)",
              mag, target);
    }

    // --- 4. glide of zero is instantaneous, which is what the default must be.
    {
        auto s = reg.instantiate(*d, kSR, kBlock);
        if (!s) return;
        s->setParam(spIdx(*s, "A Level"), 0.f);
        s->setParam(spIdx(*s, "Sub"), 1.f);
        s->setParam(spIdx(*s, "Attack"), 1.f);
        s->setParam(spIdx(*s, "Decay"), 5000.f);
        s->setParam(spIdx(*s, "Sustain"), 1.f);
        s->setParam(spIdx(*s, "Voices"), 1.f);
        s->setParam(spIdx(*s, "Master"), 1.f);
        const std::vector<SpEvent> ev = { { 0, 0x90, 48, 110 }, { 4800, 0x90, 72, 110 } };
        std::vector<f32> L, R;
        spRender(*s, ev, 24000, kBlock, L, R);
        const f64 hi = tBinMag(L, 5300, 4800, tMidiHz(60));
        CHECK(hi > 0.4, "Glide = 0 arrives at the new pitch immediately (%.3f)", hi);
    }
}

// ---------------------------------------------------------------------------

static void testSpectraSweeps(PluginRegistry& reg) {
    banner("Spectra: every parameter sweeps under audio without breaking");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;
    auto s = reg.instantiate(*d, kSR, kBlock);
    if (!s) return;
    spBusyPatch(*s);

    // One parameter at a time, over its whole declared range, while notes come
    // and go. The established pattern from the effect suite; the only addition
    // is that this device is being played at the same time.
    Buf out;
    char badParam[128] = "";
    bool ok = true;
    for (int pi = 0; pi < s->paramCount() && ok; ++pi) {
        const ParamInfo& info = s->paramInfo(pi);
        const f32 keep = s->getParam(pi);
        for (int k = 0; k <= 24 && ok; ++k) {
            const f32 t = (f32)k / 24.f;
            s->setParam(pi, lerpf(info.min, info.max, t));
            if ((k % 4) == 0) noteOn(*s, (u8)(40 + (pi * 3 + k) % 45), (u8)(60 + k), k * 3);
            if ((k % 4) == 3) noteOff(*s, (u8)(40 + (pi * 3 + k - 3) % 45), k);
            out.clear();
            s->process(nullptr, out.p, 2, kBlock);
            if (!out.finite() || out.peak() > 12.f) {
                ok = false;
                std::snprintf(badParam, sizeof badParam, "%s (peak %.3f)",
                              info.name.c_str(), (double)out.peak());
            }
        }
        s->setParam(pi, keep);
    }
    CHECK(ok, "all %d parameters sweep during playback without NaN or blow-up%s%s",
          s->paramCount(), ok ? "" : " — failed on ", badParam);

    // And it is still an instrument afterwards.
    const u8 cc[3] = { 0xB0, 120, 0 };
    s->midi(cc, 3, 0);
    for (int i = 0; i < s->paramCount(); ++i) s->setParam(i, s->paramInfo(i).def);
    s->setParam(spIdx(*s, "Sustain"), 1.f);
    noteOn(*s, 60, 110);
    CHECK(runFor(*s, out, 20) > 0.02f, "still plays after every knob has been swept");

    // Absurd block sizes are refused rather than overrun. kMaxBlock is the
    // documented ceiling; a block above it degrades to silence, like Pulse.
    {
        std::vector<f32> big((size_t)kMaxBlock + 64, 1.f), big2((size_t)kMaxBlock + 64, 1.f);
        f32* o[2] = { big.data(), big2.data() };
        s->process(nullptr, o, 2, kMaxBlock + 64);
        bool silent = true;
        for (int i = 0; i < kMaxBlock + 64; ++i) if (big[(size_t)i] != 0.f) silent = false;
        CHECK(silent, "a block larger than kMaxBlock degrades to silence, not to a overrun");
    }
    // A legal oversized block (larger than the prepared one) is processed.
    {
        std::vector<f32> l(2048, 0.f), r(2048, 0.f);
        f32* o[2] = { l.data(), r.data() };
        noteOn(*s, 60, 110);
        s->process(nullptr, o, 2, 2048);
        f32 pk = 0.f;
        for (f32 v : l) pk = std::fmax(pk, std::fabs(v));
        CHECK(pk > 0.f, "a 2048-frame block on a 256-frame prepare is processed (peak %.4f)",
              (double)pk);
    }

    // Mono output: one channel is a fold of the pair, not half the instrument.
    {
        auto m = reg.instantiate(*d, kSR, kBlock);
        if (m) {
            m->setParam(spIdx(*m, "Sustain"), 1.f);
            m->setParam(spIdx(*m, "A Unison"), 7.f);
            m->setParam(spIdx(*m, "A Spread"), 1.f);
            noteOn(*m, 60, 110);
            std::vector<f32> mono((size_t)kBlock, 0.f);
            f32* o[1] = { mono.data() };
            f32 pk = 0.f;
            for (int b = 0; b < 20; ++b) {
                std::fill(mono.begin(), mono.end(), 0.f);
                m->process(nullptr, o, 1, kBlock);
                for (f32 v : mono) pk = std::fmax(pk, std::fabs(v));
            }
            CHECK(pk > 0.02f, "a mono host gets the folded pair, not silence (peak %.4f)",
                  (double)pk);
        }
    }
}

// ---------------------------------------------------------------------------

static void testSpectraState(PluginRegistry& reg) {
    banner("Spectra: state save and restore through the parameters");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;

    auto src = reg.instantiate(*d, kSR, kBlock);
    if (!src) return;
    spBusyPatch(*src);

    // What a project file stores: (ParamInfo::id, value), nothing else.
    std::vector<std::pair<u32, f32>> saved;
    for (int i = 0; i < src->paramCount(); ++i)
        saved.emplace_back(src->paramInfo(i).id, src->getParam(i));
    CHECK((int)saved.size() == kSpectraContractN, "saved %zu parameters", saved.size());

    auto dst = reg.instantiate(*d, kSR, kBlock);
    if (!dst) return;
    for (const auto& kv : saved) {
        // The loader resolves id -> index, exactly as the rack's does.
        int idx = -1;
        for (int i = 0; i < dst->paramCount(); ++i)
            if (dst->paramInfo(i).id == kv.first) { idx = i; break; }
        if (idx >= 0) dst->setParam(idx, kv.second);
    }

    bool same = true;
    for (int i = 0; i < src->paramCount(); ++i)
        if (src->getParam(i) != dst->getParam(i)) same = false;
    CHECK(same, "every restored value is exactly the value that was saved");

    // And the restored device is the same instrument, not merely the same
    // numbers: identical MIDI has to give an identical render.
    const std::vector<SpEvent> ev = spScript();
    std::vector<f32> aL, aR, bL, bR;
    spRender(*src, ev, 9000, kBlock, aL, aR, 120.0);
    spRender(*dst, ev, 9000, kBlock, bL, bR, 120.0);
    f32 diff = 0.f, pk = 0.f;
    for (int i = 0; i < 9000; ++i) {
        diff = std::fmax(diff, std::fabs(aL[(size_t)i] - bL[(size_t)i]));
        diff = std::fmax(diff, std::fabs(aR[(size_t)i] - bR[(size_t)i]));
        pk   = std::fmax(pk, std::fabs(aL[(size_t)i]));
    }
    CHECK(pk > 0.05f, "the comparison render is not silent (peak %.4f)", (double)pk);
    CHECK(diff == 0.f, "a restored instrument renders bit-identically (max diff %.9f)",
          (double)diff);
}

// ---------------------------------------------------------------------------

static void testSpectraPresets(PluginRegistry& reg) {
    banner("Spectra: factory presets");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;
    auto s = reg.instantiate(*d, kSR, kBlock);
    if (!s) return;

    CHECK(s->presetCount() == kSpectraPresetN,
          "presetCount() is %d (the contract's bank is Init + 48 + 48 + 24 = %d rows)",
          s->presetCount(), kSpectraPresetN);
    CHECK(s->presetName(-1) == nullptr && s->presetName(s->presetCount()) == nullptr,
          "presetName() is null out of range");
    CHECK(s->presetName(0) && std::strcmp(s->presetName(0), "Init") == 0,
          "row 0 is \"Init\" (it is \"%s\")", spPresetName(*s, 0));

    // The naming and ordering rules, mechanically: "<tag> Name", letters /
    // digits / spaces only, <= 20 chars, no trailing number (numbered names
    // are banned); categories in contract order at contract counts;
    // alphabetical within a category.
    {
        bool ok = true;
        int row = 1;
        for (int bank = 0; bank < kSpectraPresetBankN; ++bank)
        for (int ci = 0; ci < kSpectraPresetBanks[bank].n; ++ci) {
            const SpectraPresetCat& cat = kSpectraPresetBanks[bank].cats[ci];
            // Alphabetical WITHIN each bank's own group of a category: bank 2
            // appends, so its BA rows sort among themselves and not into
            // bank 1's. The editor draws a header wherever the tag changes,
            // which is why the popover shows the seven categories three times.
            const char* prev = nullptr;
            for (int j = 0; j < cat.count; ++j, ++row) {
                const char* n = s->presetName(row);
                if (!n) { CHECK(false, "row %d has no name", row); ok = false; continue; }
                const size_t len = std::strlen(n);
                if (len > 20) {
                    CHECK(false, "\"%s\" is %zu chars (cap 20)", n, len);
                    ok = false;
                }
                if (len < 4 || n[0] != cat.tag[0] || n[1] != cat.tag[1] || n[2] != ' ') {
                    CHECK(false, "row %d \"%s\" does not carry the tag \"%s \"",
                          row, n, cat.tag);
                    ok = false;
                    continue;
                }
                bool chars = true;
                for (size_t c = 0; c < len; ++c) {
                    const char ch = n[c];
                    if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                          (ch >= '0' && ch <= '9') || ch == ' '))
                        chars = false;
                }
                if (!chars) { CHECK(false, "\"%s\" uses characters outside letters/digits/spaces", n); ok = false; }
                if (len >= 2 && n[len - 2] == ' ' && n[len - 1] >= '0' && n[len - 1] <= '9') {
                    CHECK(false, "\"%s\" is a numbered name, which the contract bans", n);
                    ok = false;
                }
                if (prev && std::strcmp(prev, n) >= 0) {
                    CHECK(false, "\"%s\" is out of alphabetical order after \"%s\"", n, prev);
                    ok = false;
                }
                prev = n;
            }
        }
        CHECK(ok && row == kSpectraPresetN,
              "banks 1 and 2 are BA9 LD9 PD8 KY7 PL6 FX5 SQ4 and bank 3 (the arps) is "
              "BA6 LD6 PD1 KY2 PL4 FX1 SQ4, in order, alphabetical within each bank's "
              "category, names tagged and <= 20 chars (%d rows)", row);
    }

    // Init is the defaults, exactly. Not approximately: a preset that drifts
    // from the constructor is a preset that cannot be trusted as a reset.
    {
        auto a = reg.instantiate(*d, kSR, kBlock);
        if (a) {
            spBusyPatch(*a);
            a->loadPreset(0);
            bool def = true;
            for (int i = 0; i < a->paramCount(); ++i)
                if (a->getParam(i) != a->paramInfo(i).def) {
                    CHECK(false, "Init left %s at %g, the default is %g",
                          a->paramInfo(i).name.c_str(), (double)a->getParam(i),
                          (double)a->paramInfo(i).def);
                    def = false;
                }
            CHECK(def, "Init restores every parameter to its constructor default exactly");
        }
    }

    // Every preset, every parameter, inside its declared range. loadPreset
    // writes through setParam, which clamps, so this cannot fail on a stray
    // table value alone -- what it does catch is a preset naming a parameter id
    // that does not exist, or leaving one non-finite.
    {
        bool inRange = true;
        for (int k = 0; k < s->presetCount(); ++k) {
            auto a = reg.instantiate(*d, kSR, kBlock);
            if (!a) break;
            a->loadPreset(k);
            for (int i = 0; i < a->paramCount(); ++i) {
                const ParamInfo& pi = a->paramInfo(i);
                const f32 v = a->getParam(i);
                if (!std::isfinite(v) || v < pi.min || v > pi.max) {
                    CHECK(false, "preset \"%s\" leaves %s at %g, outside [%g, %g]",
                          spPresetName(*s, k), pi.name.c_str(), (double)v,
                          (double)pi.min, (double)pi.max);
                    inRange = false;
                }
                if (pi.isInt && v != std::floor(v)) {
                    CHECK(false, "preset \"%s\" leaves the stepped parameter %s at %g",
                          spPresetName(*s, k), pi.name.c_str(), (double)v);
                    inRange = false;
                }
            }
        }
        CHECK(inRange, "every preset leaves every parameter finite, in range, and "
                       "integral where the contract says stepped");
    }

    // A preset is COMPLETE: loading it after any other patch gives the same
    // state as loading it into a fresh instance. This is the check that catches
    // a preset which only lists what it changes and forgets to reset the rest.
    {
        bool complete = true;
        for (int k = 0; k < s->presetCount(); ++k) {
            auto fresh = reg.instantiate(*d, kSR, kBlock);
            auto dirty = reg.instantiate(*d, kSR, kBlock);
            if (!fresh || !dirty) break;
            fresh->loadPreset(k);
            spBusyPatch(*dirty);
            dirty->loadPreset((k + 5) % s->presetCount());
            dirty->loadPreset(k);
            for (int i = 0; i < fresh->paramCount(); ++i)
                if (fresh->getParam(i) != dirty->getParam(i)) {
                    CHECK(false, "preset \"%s\" is not self-contained: %s is %g from a "
                                 "dirty state and %g from a fresh one",
                          spPresetName(*s, k), fresh->paramInfo(i).name.c_str(),
                          (double)dirty->getParam(i), (double)fresh->getParam(i));
                    complete = false;
                    break;
                }
        }
        CHECK(complete, "every preset lands on the same state from any starting patch");
    }

    // Every preset makes a sound, and the ones that are supposed to be
    // different instruments actually are. The spectrum is reduced to sixteen
    // logarithmic bands and compared by cosine similarity, which is a coarse
    // enough measure that it only fires when two presets really are the same
    // patch under two names.
    {
        const int kN = 8192;
        std::vector<std::vector<f64>> bands((size_t)s->presetCount());
        bool sounded = true;
        for (int k = 0; k < s->presetCount(); ++k) {
            auto a = reg.instantiate(*d, kSR, kBlock);
            if (!a) break;
            a->loadPreset(k);
            std::vector<f32> L, R;
            const std::vector<SpEvent> ev = { { 0, 0x90, 55, 110 } };
            // Long enough that the slow pads have opened up before the window.
            spRender(*a, ev, 48000 + kN, kBlock, L, R);
            f32 pk = 0.f;
            for (int i = 0; i < 48000 + kN; ++i) pk = std::fmax(pk, std::fabs(L[(size_t)i]));
            if (!(pk > 0.01f)) {
                CHECK(false, "preset \"%s\" is silent (peak %.5f)",
                      spPresetName(*s, k), (double)pk);
                sounded = false;
            }
            std::vector<f32> re((size_t)kN), im((size_t)kN, 0.f);
            for (int i = 0; i < kN; ++i)
                re[(size_t)i] = L[(size_t)(40000 + i)] *
                    (f32)(0.5 - 0.5 * std::cos(6.283185307179586 * i / kN));
            tFft(re, im);
            std::vector<f64> b(16, 0.0);
            for (int j = 4; j < kN / 2; ++j) {
                const f64 f = (f64)j * kSR / (f64)kN;
                int bi = (int)((std::log2(std::fmax(f, 20.0) / 20.0)) * 16.0 / 10.0);
                if (bi < 0) bi = 0;
                if (bi > 15) bi = 15;
                b[(size_t)bi] += (f64)re[(size_t)j] * re[(size_t)j] +
                                 (f64)im[(size_t)j] * im[(size_t)j];
            }
            f64 norm = 0.0;
            for (f64 v : b) norm += v * v;
            norm = std::sqrt(norm);
            if (norm > 0.0) for (f64& v : b) v /= norm;
            bands[(size_t)k] = b;
        }
        CHECK(sounded, "every one of the %d presets renders a non-silent note",
              s->presetCount());

        // Three from three categories that must be plainly different
        // instruments: the first Bass (row 1), the first Pad (row 19: after
        // BA9 + LD9), the first Keys (row 27: after PD8).
        const int probe[3] = { 1, 19, 27 };
        bool distinct = true;
        f64 worst = 0.0;
        for (int i = 0; i < 3; ++i) {
            for (int j = i + 1; j < 3; ++j) {
                if (bands[(size_t)probe[i]].empty() || bands[(size_t)probe[j]].empty()) continue;
                f64 dot = 0.0;
                for (int k = 0; k < 16; ++k)
                    dot += bands[(size_t)probe[i]][(size_t)k] * bands[(size_t)probe[j]][(size_t)k];
                if (dot > worst) worst = dot;
                if (dot > 0.95) {
                    CHECK(false, "presets \"%s\" and \"%s\" have near-identical spectra "
                                 "(similarity %.3f)",
                          spPresetName(*s, probe[i]), spPresetName(*s, probe[j]), dot);
                    distinct = false;
                }
            }
        }
        CHECK(distinct, "the first Bass, Pad and Keys are three different sounds "
                        "(worst spectral similarity %.3f, limit 0.95)", worst);
    }

    // Presets go through setParam, so they are ordinary parameter moves as far
    // as the rest of the program is concerned. Nothing else has a preset list.
    {
        const PluginDesc* pd = reg.find("nxtakt:pulse");
        if (pd) {
            auto pulse = reg.instantiate(*pd, kSR, kBlock);
            if (pulse)
                CHECK(pulse->presetCount() == 0 && pulse->presetName(0) == nullptr,
                      "a device with no presets answers 0 / null, so the UI draws no "
                      "selector for it");
        }
    }
}

// ---------------------------------------------------------------------------
// Spectra v2 (ids 42..99) — sub/noise completion, warp, LFO2/3, ENV3, the
// mod matrix, macros, voice modes. Same house rule as the v1 sections:
// everything below MEASURES.
// ---------------------------------------------------------------------------

// A fresh default instance with the handful of levels every v2 test wants:
// osc A only, filter open, full sustain.
static std::unique_ptr<PluginInstance> spV2Base(PluginRegistry& reg, const PluginDesc& d) {
    auto s = reg.instantiate(d, kSR, kBlock);
    if (!s) return s;
    s->setParam(spIdx(*s, "A Level"), 1.f);
    s->setParam(spIdx(*s, "Attack"), 2.f);
    s->setParam(spIdx(*s, "Decay"), 5000.f);
    s->setParam(spIdx(*s, "Sustain"), 1.f);
    s->setParam(spIdx(*s, "Cutoff"), 20000.f);
    s->setParam(spIdx(*s, "Resonance"), 0.f);
    s->setParam(spIdx(*s, "Master"), 1.f);
    return s;
}

// One matrix slot, by raw contract ids: slot k is 68+3k / 69+3k / 70+3k.
static void spV2Slot(PluginInstance& s, int k, int src, int dst, f32 amt) {
    s.setParam(68 + 3 * k, (f32)src);
    s.setParam(69 + 3 * k, (f32)dst);
    s.setParam(70 + 3 * k, amt);
}

// Alias measurement, the v1 C7 test's method factored out: Hann-windowed FFT
// of the steady part, energy within 6% of a multiple of f0 is signal,
// everything else is not; returns 10*log10(alias/signal).
static f64 spV2AliasDb(const std::vector<f32>& L, int from, int kN, f64 f0) {
    std::vector<f32> re((size_t)kN), im((size_t)kN, 0.f);
    for (int i = 0; i < kN; ++i)
        re[(size_t)i] = L[(size_t)(from + i)] *
            (f32)(0.5 - 0.5 * std::cos(6.283185307179586 * i / kN));
    tFft(re, im);
    const f64 binHz = kSR / (f64)kN;
    f64 sig = 0.0, alias = 0.0;
    for (int k = 4; k < kN / 2; ++k) {
        const f64 h  = (f64)k * binHz / f0;
        const f64 dh = std::fabs(h - std::floor(h + 0.5));
        const f64 e  = (f64)re[(size_t)k] * re[(size_t)k] +
                       (f64)im[(size_t)k] * im[(size_t)k];
        if (std::floor(h + 0.5) >= 1.0 && dh < 0.06) sig += e;
        else                                          alias += e;
    }
    return 10.0 * std::log10(alias / (sig + 1e-30));
}

// Energy in [lo, hi) Hz over a Hann-windowed window, in dB (relative scale).
static f64 spV2BandDb(const std::vector<f32>& L, int from, int kN, f64 lo, f64 hi) {
    std::vector<f32> re((size_t)kN), im((size_t)kN, 0.f);
    for (int i = 0; i < kN; ++i)
        re[(size_t)i] = L[(size_t)(from + i)] *
            (f32)(0.5 - 0.5 * std::cos(6.283185307179586 * i / kN));
    tFft(re, im);
    const f64 binHz = kSR / (f64)kN;
    f64 e = 0.0;
    for (int k = 1; k < kN / 2; ++k) {
        const f64 f = (f64)k * binHz;
        if (f >= lo && f < hi)
            e += (f64)re[(size_t)k] * re[(size_t)k] + (f64)im[(size_t)k] * im[(size_t)k];
    }
    return 10.0 * std::log10(e + 1e-30);
}

static f32 spV2MaxDiff(const std::vector<f32>& a, const std::vector<f32>& b) {
    f32 d = 0.f;
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) d = std::fmax(d, std::fabs(a[i] - b[i]));
    return d;
}

static void testSpectraV2SubNoise(PluginRegistry& reg) {
    banner("Spectra v2: sub shapes and octaves, noise color and tracking");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;

    // --- 1. SUB OCT places the fundamental. Sine sub alone at C3; the
    // contract's default -1 is one octave below, 0 is unison, -2 two down.
    {
        const f64 fn = tMidiHz(48);
        const struct { f32 oct; f64 mul; } cases[3] = { {0.f, 1.0}, {-1.f, 0.5}, {-2.f, 0.25} };
        bool ok = true;
        for (const auto& c : cases) {
            auto s = spV2Base(reg, *d);
            if (!s) return;
            s->setParam(spIdx(*s, "A Level"), 0.f);
            s->setParam(spIdx(*s, "Sub"), 1.f);
            s->setParam(spIdx(*s, "Sub Oct"), c.oct);
            std::vector<f32> L, R;
            spRender(*s, { { 0, 0x90, 48, 110 } }, 48000, kBlock, L, R);
            const f64 want  = tBinMag(L, 24000, 16384, fn * c.mul);
            const f64 above = tBinMag(L, 24000, 16384, fn * c.mul * 2.0);
            if (!(want > 0.1 && want > 8.0 * above)) {
                CHECK(false, "Sub Oct %g: fundamental %.3f at %.1f Hz, %.4f an octave up",
                      (double)c.oct, want, fn * c.mul, above);
                ok = false;
            }
        }
        CHECK(ok, "Sub Oct 0/-1/-2 put a clean sine at f, f/2, f/4");
    }

    // --- 2. SUB SHAPES. The triangle carries h3 and (nearly) no h2; the
    // polyBLEP square likewise, with more of it; both keep the fundamental.
    {
        bool ok = true;
        for (int shape = 1; shape <= 2; ++shape) {
            auto s = spV2Base(reg, *d);
            if (!s) return;
            s->setParam(spIdx(*s, "A Level"), 0.f);
            s->setParam(spIdx(*s, "Sub"), 1.f);
            s->setParam(spIdx(*s, "Sub Shape"), (f32)shape);
            s->setParam(spIdx(*s, "Sub Oct"), 0.f);
            std::vector<f32> L, R;
            spRender(*s, { { 0, 0x90, 48, 110 } }, 48000, kBlock, L, R);
            const f64 fn = tMidiHz(48);
            const f64 h1 = tBinMag(L, 24000, 16384, fn);
            const f64 h2 = tBinMag(L, 24000, 16384, fn * 2.0);
            const f64 h3 = tBinMag(L, 24000, 16384, fn * 3.0);
            if (!(h1 > 0.1 && h3 > 0.02 * h1 && h2 < 0.05 * h1)) {
                CHECK(false, "sub shape %d: h1 %.3f h2 %.4f h3 %.4f — not an odd-harmonic wave",
                      shape, h1, h2, h3);
                ok = false;
            }
        }
        CHECK(ok, "sub triangle and square are odd-harmonic waves on the sub pitch");
    }

    // --- 3. THE POLYBLEP EARNS ITS NAME: the square sub two octaves up the
    // keyboard still keeps its aliasing floor down.
    {
        auto s = spV2Base(reg, *d);
        if (!s) return;
        s->setParam(spIdx(*s, "A Level"), 0.f);
        s->setParam(spIdx(*s, "Sub"), 1.f);
        s->setParam(spIdx(*s, "Sub Shape"), 2.f);
        s->setParam(spIdx(*s, "Sub Oct"), 0.f);
        std::vector<f32> L, R;
        spRender(*s, { { 0, 0x90, 84, 110 } }, 8192 + 16384, kBlock, L, R);
        const f64 db = spV2AliasDb(L, 8192, 16384, tMidiHz(84));
        CHECK(db < -40.0, "polyBLEP square sub at C6: alias/signal %.1f dB (limit -40)", db);
    }

    // --- 4. NOISE COLOR tilts the white source down. Color 0 puts the pole
    // at 200 Hz; 6 dB/oct means the top of the band drops by tens of dB
    // against the bypassed white of color 1.
    {
        f64 tilt[2] = { 0.0, 0.0 };     // [0] color 1 (bypass), [1] color 0
        for (int c = 0; c < 2; ++c) {
            auto s = spV2Base(reg, *d);
            if (!s) return;
            s->setParam(spIdx(*s, "A Level"), 0.f);
            s->setParam(spIdx(*s, "Noise"), 1.f);
            s->setParam(spIdx(*s, "Noise Color"), c == 0 ? 1.f : 0.f);
            std::vector<f32> L, R;
            spRender(*s, { { 0, 0x90, 60, 110 } }, 8192 + 16384, kBlock, L, R);
            tilt[c] = spV2BandDb(L, 8192, 16384, 4000.0, 16000.0) -
                      spV2BandDb(L, 8192, 16384, 50.0, 200.0);
        }
        CHECK(tilt[1] < tilt[0] - 20.0,
              "Noise Color 0 tilts the top band down %.1f dB against bypassed white "
              "(needs > 20)", tilt[0] - tilt[1]);
    }

    // --- 5. NOISE TRACK moves the color pole with the note: the same color
    // is brighter under a high note than a low one.
    {
        f64 hf[2] = { 0.0, 0.0 };
        const u8 notes[2] = { 36, 96 };
        for (int c = 0; c < 2; ++c) {
            auto s = spV2Base(reg, *d);
            if (!s) return;
            s->setParam(spIdx(*s, "A Level"), 0.f);
            s->setParam(spIdx(*s, "Noise"), 1.f);
            s->setParam(spIdx(*s, "Noise Color"), 0.5f);
            s->setParam(spIdx(*s, "Noise Track"), 1.f);
            std::vector<f32> L, R;
            spRender(*s, { { 0, 0x90, notes[c], 110 } }, 8192 + 16384, kBlock, L, R);
            hf[c] = spV2BandDb(L, 8192, 16384, 6000.0, 16000.0) -
                    spV2BandDb(L, 8192, 16384, 100.0, 400.0);
        }
        CHECK(hf[1] > hf[0] + 10.0,
              "Noise Track follows the note: C7 is %.1f dB brighter than C2 at the same "
              "color (needs > 10)", hf[1] - hf[0]);
    }
}

static void testSpectraV2Warp(PluginRegistry& reg) {
    banner("Spectra v2: the seven warp modes");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;

    auto render = [&](int mode, f32 amt, u8 note, std::vector<f32>& L,
                      int frames = 8192 + 16384) {
        auto s = spV2Base(reg, *d);
        if (!s) return false;
        s->setParam(spIdx(*s, "A Warp"), (f32)mode);
        s->setParam(spIdx(*s, "A Warp Amt"), amt);
        std::vector<f32> R;
        spRender(*s, { { 0, 0x90, note, 110 } }, frames, kBlock, L, R);
        return true;
    };

    // --- 1. AT ZERO DEPTH EVERY MODE IS OFF — bit-identically, the id-49
    // gate. (The Quantize formula alone would say otherwise at a = 0; the
    // depth gate governs, as the implementation notes record.)
    {
        std::vector<f32> off, m;
        if (!render(0, 0.f, 60, off)) return;
        bool same = true;
        for (int mode = 1; mode <= 7; ++mode) {
            if (!render(mode, 0.f, 60, m)) return;
            const f32 diff = spV2MaxDiff(off, m);
            if (diff != 0.f) {
                CHECK(false, "mode %d at zero depth differs from Off (max %.9f)",
                      mode, (double)diff);
                same = false;
            }
        }
        CHECK(same, "all seven modes at zero depth render bit-identical to Off");
    }

    // --- 2. AND AT DEPTH, EVERY MODE DOES SOMETHING.
    {
        std::vector<f32> off, m;
        if (!render(0, 0.f, 60, off)) return;
        bool moved = true;
        for (int mode = 1; mode <= 7; ++mode) {
            if (!render(mode, mode == 1 ? 0.5f : 0.8f, 60, m)) return;
            f32 pk = 0.f;
            bool fin = true;
            for (f32 v : m) { pk = std::fmax(pk, std::fabs(v)); if (!std::isfinite(v)) fin = false; }
            if (!fin || !(pk > 0.02f) || !(spV2MaxDiff(off, m) > 0.01f)) {
                CHECK(false, "mode %d at depth: finite %d, peak %.4f, moved %.4f",
                      mode, (int)fin, (double)pk, (double)spV2MaxDiff(off, m));
                moved = false;
            }
        }
        CHECK(moved, "every warp mode at depth is audible, finite and bounded");
    }

    // --- 3. SYNC'S RATIO IS THE CONTRACT'S: a = 1/7 makes r = 2, and
    // frac(2p) has period 1/2 — the energy sits on 2f0 and the f0 line all
    // but vanishes.
    {
        std::vector<f32> m;
        if (!render(1, 1.f / 7.f, 57, m)) return;
        const f64 f0 = tMidiHz(57);
        const f64 at1 = tBinMag(m, 8192, 16384, f0);
        const f64 at2 = tBinMag(m, 8192, 16384, f0 * 2.0);
        CHECK(at2 > 5.0 * at1,
              "Sync at a=1/7 doubles the read rate: |2f0| %.4f vs |f0| %.4f", at2, at1);
    }

    // --- 4. ALIASING BOUNDS, per mode, at C6 and full depth (Sync at 0.5:
    // r = 4.5, well past anything a table alone reaches). The contract
    // accepts "mild aliasing" for the phase reshapers and chooses the mip
    // under Sync from f*r; these bounds are the measured behaviour with
    // headroom, recorded so a regression cannot hide behind the word "mild".
    // FM is measured against its own sidebands' harmonicity (both oscs at
    // the same pitch keep the products harmonic); RM at equal pitch is a
    // spectrum reshuffle and is covered by section 2's bounds.
    {
        // Measured on the reference render (this suite, 2026-08): Sync -35.5,
        // Bend+ -14.2, Bend- -32.0, Mirror -20.1, Quantize -19.6 dB. The
        // limits sit ~4 dB above those. Bend+ is the honest worst case: its
        // p^(1/(1+3a)) has an unbounded phase slope at p = 0, so full depth
        // at C6 genuinely reads past the mip — the contract accepts it, and
        // this row is what keeps "accepted" from quietly becoming "worse".
        const struct { int mode; f32 amt; f64 limit; const char* name; } cases[] = {
            { 1, 0.5f, -30.0, "Sync"     },
            { 2, 1.0f, -10.0, "Bend+"    },
            { 3, 1.0f, -28.0, "Bend-"    },
            { 4, 1.0f, -16.0, "Mirror"   },
            { 5, 0.5f, -15.0, "Quantize" },
        };
        for (const auto& c : cases) {
            std::vector<f32> m;
            if (!render(c.mode, c.amt, 84, m)) return;
            const f64 db = spV2AliasDb(m, 8192, 16384, tMidiHz(84));
            CHECK(db < c.limit, "%s at C6, depth %.2f: alias/signal %.1f dB (limit %.0f)",
                  c.name, (double)c.amt, db, c.limit);
        }
    }

    // --- 5. FM AT EQUAL PITCH STAYS HARMONIC: through-zero linear FM by a
    // same-pitch modulator can only put energy on harmonics of f0, so the
    // alias measure stays a real measure under FM too.
    {
        auto s = spV2Base(reg, *d);
        if (!s) return;
        s->setParam(spIdx(*s, "A Warp"), 6.f);
        s->setParam(spIdx(*s, "A Warp Amt"), 0.5f);
        s->setParam(spIdx(*s, "B Level"), 0.f);        // silent, still the tap
        std::vector<f32> L, R;
        spRender(*s, { { 0, 0x90, 69, 110 } }, 8192 + 16384, kBlock, L, R);
        const f64 db = spV2AliasDb(L, 8192, 16384, tMidiHz(69));
        f32 pk = 0.f;
        for (f32 v : L) pk = std::fmax(pk, std::fabs(v));
        CHECK(pk > 0.05f && db < -25.0,
              "A FM'd by a silent B at equal pitch: sounding (%.3f) and harmonic "
              "(%.1f dB non-harmonic, limit -25)", (double)pk, db);
    }

    // --- 6. MUTUAL FM/RM (the one-sample delay) is stable and audible both
    // ways at once.
    {
        auto s = spV2Base(reg, *d);
        if (!s) return;
        s->setParam(spIdx(*s, "B Level"), 0.6f);
        s->setParam(spIdx(*s, "B Coarse"), 7.f);
        s->setParam(spIdx(*s, "A Warp"), 6.f);
        s->setParam(spIdx(*s, "A Warp Amt"), 0.4f);
        s->setParam(spIdx(*s, "B Warp"), 7.f);
        s->setParam(spIdx(*s, "B Warp Amt"), 0.7f);
        std::vector<f32> L, R;
        spRender(*s, { { 0, 0x90, 60, 110 } }, 24000, kBlock, L, R);
        f32 pk = 0.f;
        bool fin = true;
        for (f32 v : L) { pk = std::fmax(pk, std::fabs(v)); if (!std::isfinite(v)) fin = false; }
        CHECK(fin && pk > 0.05f && pk < 4.f,
              "A FM'd by B while B ring-mods against A: finite, sounding, bounded "
              "(peak %.3f)", (double)pk);
    }
}

static void testSpectraV2Matrix(PluginRegistry& reg) {
    banner("Spectra v2: the eight-slot mod matrix");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;
    const std::vector<SpEvent> one = { { 0, 0x90, 60, 110 } };

    // --- 1. A NORM DESTINATION SUMS EXACTLY: Macro1 at 1.0 through a
    // full-depth slot into A Pos lands bit-identically on A Position = 1.0.
    // Not approximately — the sum is 0 + 1 in the same domain the knob is in.
    {
        auto a = spV2Base(reg, *d);
        auto b = spV2Base(reg, *d);
        if (!a || !b) return;
        a->setParam(spIdx(*a, "A Position"), 1.f);
        spV2Slot(*b, 0, 9 /*Macro1*/, 1 /*A Pos*/, 1.f);
        b->setParam(spIdx(*b, "Macro 1"), 1.f);
        std::vector<f32> aL, aR, bL, bR;
        spRender(*a, one, 12000, kBlock, aL, aR);
        spRender(*b, one, 12000, kBlock, bL, bR);
        CHECK(spV2MaxDiff(aL, bL) == 0.f && spV2MaxDiff(aR, bR) == 0.f,
              "Macro1 -> A Pos at full depth == the knob at 1.0, bit for bit");
    }

    // --- 2. A DEAD SLOT COSTS NOTHING AND CHANGES NOTHING: source Off,
    // dest Off, or amount 0 each leave the render bit-identical.
    {
        auto ref = spV2Base(reg, *d);
        if (!ref) return;
        std::vector<f32> rL, rR;
        spRender(*ref, one, 12000, kBlock, rL, rR);
        const struct { int src, dst; f32 amt; const char* what; } dead[3] = {
            { 0, 1, 1.f, "source Off" },
            { 9, 0, 1.f, "dest Off"   },
            { 9, 1, 0.f, "amount 0"   },
        };
        bool same = true;
        for (const auto& c : dead) {
            auto s = spV2Base(reg, *d);
            if (!s) return;
            spV2Slot(*s, 0, c.src, c.dst, c.amt);
            s->setParam(spIdx(*s, "Macro 1"), 1.f);
            std::vector<f32> L, R;
            spRender(*s, one, 12000, kBlock, L, R);
            if (spV2MaxDiff(rL, L) != 0.f) {
                CHECK(false, "a slot with %s changed the render", c.what);
                same = false;
            }
        }
        CHECK(same, "slots with source Off / dest Off / amount 0 contribute nothing");
    }

    // --- 3. VELOCITY AS A SOURCE: vel/127 into B Level turns a silent osc B
    // on for a hard note and leaves it (near) off for a soft one.
    {
        auto s = spV2Base(reg, *d);
        if (!s) return;
        s->setParam(spIdx(*s, "A Level"), 0.f);
        spV2Slot(*s, 0, 6 /*Velocity*/, 6 /*B Level*/, 1.f);
        std::vector<f32> L, R;
        spRender(*s, { { 0, 0x90, 60, 127 }, { 24000, 0x80, 60, 0 },
                       { 36000, 0x90, 60, 4 } }, 72000, kBlock, L, R);
        f32 hard = 0.f, soft = 0.f;
        for (int i = 4000; i < 20000; ++i) hard = std::fmax(hard, std::fabs(L[(size_t)i]));
        for (int i = 40000; i < 56000; ++i) soft = std::fmax(soft, std::fabs(L[(size_t)i]));
        CHECK(hard > 0.05f && soft < 0.15f * hard,
              "velocity drives B Level: vel 127 peaks %.3f, vel 4 peaks %.3f",
              (double)hard, (double)soft);
    }

    // --- 4. PAN IS EQUAL-POWER AND REACHES THE EDGES: a macro at 1.0 through
    // a full slot is hard right — the left channel collapses.
    {
        auto s = spV2Base(reg, *d);
        if (!s) return;
        spV2Slot(*s, 0, 9 /*Macro1*/, 16 /*Pan*/, 1.f);
        s->setParam(spIdx(*s, "Macro 1"), 1.f);
        std::vector<f32> L, R;
        spRender(*s, one, 12000, kBlock, L, R);
        f32 pl = 0.f, pr = 0.f;
        for (int i = 2000; i < 12000; ++i) {
            pl = std::fmax(pl, std::fabs(L[(size_t)i]));
            pr = std::fmax(pr, std::fabs(R[(size_t)i]));
        }
        CHECK(pr > 0.05f && pl < 1e-4f * pr,
              "Pan at +1 is hard right: L %.2e vs R %.3f", (double)pl, (double)pr);
    }

    // --- 5. ENV3 IS A REAL SOURCE: routed to Cutoff over a dark filter, its
    // sustain-1 plateau holds the filter open.
    {
        f64 hf[2] = { 0.0, 0.0 };
        for (int c = 0; c < 2; ++c) {
            auto s = spV2Base(reg, *d);
            if (!s) return;
            s->setParam(spIdx(*s, "Cutoff"), 250.f);
            s->setParam(spIdx(*s, "E3 Sustain"), 1.f);
            if (c) spV2Slot(*s, 0, 5 /*ENV3*/, 11 /*Cutoff*/, 0.7f);
            std::vector<f32> L, R;
            spRender(*s, one, 8192 + 16384, kBlock, L, R);
            hf[c] = spV2BandDb(L, 8192, 16384, 2000.0, 8000.0);
        }
        CHECK(hf[1] > hf[0] + 10.0,
              "ENV3 -> Cutoff opens the filter by %.1f dB up top (needs > 10)",
              hf[1] - hf[0]);
    }

    // --- 6. LFO2 EXISTS AND ONLY SPEAKS THROUGH THE MATRIX: routed to A Pos
    // it moves the render; unrouted, its knobs change nothing.
    {
        auto ref = spV2Base(reg, *d);
        auto silent = spV2Base(reg, *d);
        auto routed = spV2Base(reg, *d);
        if (!ref || !silent || !routed) return;
        silent->setParam(spIdx(*silent, "L2 Rate"), 9.f);
        silent->setParam(spIdx(*silent, "L2 Shape"), 3.f);
        routed->setParam(spIdx(*routed, "L2 Rate"), 6.f);
        spV2Slot(*routed, 0, 2 /*LFO2*/, 1 /*A Pos*/, 1.f);
        std::vector<f32> rL, rR, sL, sR, mL, mR;
        spRender(*ref, one, 24000, kBlock, rL, rR);
        spRender(*silent, one, 24000, kBlock, sL, sR);
        spRender(*routed, one, 24000, kBlock, mL, mR);
        CHECK(spV2MaxDiff(rL, sL) == 0.f,
              "an unrouted LFO2's knobs change nothing (no fixed routings)");
        CHECK(spV2MaxDiff(rL, mL) > 0.01f,
              "LFO2 -> A Pos moves the sound (max diff %.4f)",
              (double)spV2MaxDiff(rL, mL));
    }

    // --- 7. RANDOM IS PER NOTE AND PER IDENTITY: two notes at different
    // stamped samples take different pitches through Random -> A Pitch; the
    // whole render is still bit-repeatable instance to instance.
    {
        auto a = spV2Base(reg, *d);
        auto b = spV2Base(reg, *d);
        if (!a || !b) return;
        spV2Slot(*a, 0, 13 /*Random*/, 7 /*A Pitch*/, 1.f);
        spV2Slot(*b, 0, 13, 7, 1.f);
        const std::vector<SpEvent> two = {
            { 0,     0x90, 60, 110 }, { 20000, 0x80, 60, 0 },
            { 26000, 0x90, 60, 110 }, { 50000, 0x80, 60, 0 },
        };
        std::vector<f32> aL, aR, bL, bR;
        spRender(*a, two, 56000, kBlock, aL, aR);
        spRender(*b, two, 56000, kBlock, bL, bR);
        CHECK(spV2MaxDiff(aL, bL) == 0.f,
              "Random-per-note renders bit-identically across instances — a hash of "
              "the note identity, not a stream");

        // The two notes really did land on different pitches: compare their
        // 16-band spectra; a ±24 st spread makes them plainly different.
        auto bandsOf = [&](int from) {
            const int kN = 8192;
            std::vector<f32> re((size_t)kN), im((size_t)kN, 0.f);
            for (int i = 0; i < kN; ++i)
                re[(size_t)i] = aL[(size_t)(from + i)] *
                    (f32)(0.5 - 0.5 * std::cos(6.283185307179586 * i / kN));
            tFft(re, im);
            std::vector<f64> bnd(24, 0.0);
            for (int j = 4; j < kN / 2; ++j) {
                const f64 f = (f64)j * kSR / (f64)kN;
                int bi = (int)(std::log2(std::fmax(f, 30.0) / 30.0) * 24.0 / 9.5);
                if (bi < 0) bi = 0;
                if (bi > 23) bi = 23;
                bnd[(size_t)bi] += (f64)re[(size_t)j] * re[(size_t)j] +
                                   (f64)im[(size_t)j] * im[(size_t)j];
            }
            f64 n = 0.0;
            for (f64 v : bnd) n += v * v;
            n = std::sqrt(n);
            if (n > 0.0) for (f64& v : bnd) v /= n;
            return bnd;
        };
        const std::vector<f64> n1 = bandsOf(8000), n2 = bandsOf(34000);
        f64 dot = 0.0;
        for (int i = 0; i < 24; ++i) dot += n1[(size_t)i] * n2[(size_t)i];
        CHECK(dot < 0.9,
              "the two notes drew different Randoms: spectral similarity %.3f "
              "(limit 0.9)", dot);
    }

    // --- 8. AFTERTOUCH IS A SOURCE: channel pressure through a slot into
    // Cutoff brightens a held note when it arrives.
    {
        auto s = spV2Base(reg, *d);
        if (!s) return;
        s->setParam(spIdx(*s, "Cutoff"), 250.f);
        spV2Slot(*s, 0, 8 /*Aftertouch*/, 11 /*Cutoff*/, 0.8f);
        std::vector<f32> L, R;
        spRender(*s, { { 0, 0x90, 60, 110 }, { 24000, 0xD0, 127, 0 } },
                 24000 + 16384, kBlock, L, R);
        const f64 before = spV2BandDb(L, 6000, 16384, 2000.0, 8000.0);
        const f64 after  = spV2BandDb(L, 24000 + 2000 - 2000, 16384, 2000.0, 8000.0);
        CHECK(after > before + 10.0,
              "channel pressure opens the filter: %.1f dB more top after 0xD0 "
              "(needs > 10)", after - before);
    }

    // --- 9. SLOTS SUM ON ONE DESTINATION: two half-depth macro slots into
    // A Pos land where one full-depth slot does, bit for bit.
    {
        auto a = spV2Base(reg, *d);
        auto b = spV2Base(reg, *d);
        if (!a || !b) return;
        spV2Slot(*a, 0, 9, 1, 1.f);
        a->setParam(spIdx(*a, "Macro 1"), 1.f);
        spV2Slot(*b, 2, 9, 1, 0.5f);       // arbitrary slots: order is summed,
        spV2Slot(*b, 5, 9, 1, 0.5f);       // not positional
        b->setParam(spIdx(*b, "Macro 1"), 1.f);
        std::vector<f32> aL, aR, bL, bR;
        spRender(*a, one, 12000, kBlock, aL, aR);
        spRender(*b, one, 12000, kBlock, bL, bR);
        CHECK(spV2MaxDiff(aL, bL) < 1e-6f,
              "0.5 + 0.5 on one destination equals 1.0 (max diff %.2e)",
              (double)spV2MaxDiff(aL, bL));
    }
}

static void testSpectraV2Voice(PluginRegistry& reg) {
    banner("Spectra v2: Mono and Legato voice modes, filter widening");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;

    // --- 1. MONO IS ONE VOICE: two notes stamped one frame apart leave only
    // the newer one sounding.
    {
        auto s = spV2Base(reg, *d);
        if (!s) return;
        s->setParam(spIdx(*s, "Voice Mode"), 1.f);
        std::vector<f32> L, R;
        spRender(*s, { { 0, 0x90, 45, 110 }, { 1, 0x90, 52, 110 } },
                 48000, kBlock, L, R);
        const f64 low  = tBinMag(L, 24000, 16384, tMidiHz(45));
        const f64 high = tBinMag(L, 24000, 16384, tMidiHz(52));
        CHECK(high > 0.02 && high > 8.0 * low,
              "Mono keeps one voice: |new| %.4f vs |old| %.4f", high, low);
    }

    // --- 2. MONO RETRIGGERS, LEGATO DOES NOT. Same overlapped phrase, low
    // sustain: the retrigger's fresh attack peaks far above the legato's
    // continued sustain plateau.
    {
        f32 pk[2] = { 0.f, 0.f };
        for (int mode = 1; mode <= 2; ++mode) {
            auto s = spV2Base(reg, *d);
            if (!s) return;
            s->setParam(spIdx(*s, "Voice Mode"), (f32)mode);
            s->setParam(spIdx(*s, "Sustain"), 0.2f);
            s->setParam(spIdx(*s, "Decay"), 120.f);
            std::vector<f32> L, R;
            spRender(*s, { { 0, 0x90, 48, 127 }, { 24000, 0x90, 55, 127 } },
                     33600, kBlock, L, R);
            for (int i = 24000; i < 33600; ++i)
                pk[mode - 1] = std::fmax(pk[mode - 1], std::fabs(L[(size_t)i]));
        }
        CHECK(pk[0] > 2.0f * pk[1],
              "the overlapped note re-attacks in Mono (peak %.3f) and only glides in "
              "Legato (peak %.3f)", (double)pk[0], (double)pk[1]);
    }

    // --- 3. LEGATO DETACHED RETRIGGERS: after everything is released, the
    // next note gets its own attack.
    {
        auto s = spV2Base(reg, *d);
        if (!s) return;
        s->setParam(spIdx(*s, "Voice Mode"), 2.f);
        s->setParam(spIdx(*s, "Sustain"), 0.2f);
        s->setParam(spIdx(*s, "Decay"), 120.f);
        s->setParam(spIdx(*s, "Release"), 40.f);
        std::vector<f32> L, R;
        spRender(*s, { { 0, 0x90, 48, 127 }, { 20000, 0x80, 48, 0 },
                       { 30000, 0x90, 55, 127 } }, 40000, kBlock, L, R);
        f32 pk = 0.f;
        for (int i = 30000; i < 40000; ++i) pk = std::fmax(pk, std::fabs(L[(size_t)i]));
        CHECK(pk > 0.3f, "a detached Legato note re-attacks (peak %.3f)", (double)pk);
    }

    // --- 4. MONO NOTE-OFF FALLS BACK to the most recent still-held note.
    {
        auto s = spV2Base(reg, *d);
        if (!s) return;
        s->setParam(spIdx(*s, "Voice Mode"), 1.f);
        std::vector<f32> L, R;
        spRender(*s, { { 0, 0x90, 45, 110 }, { 12000, 0x90, 50, 110 },
                       { 24000, 0x80, 50, 0 } }, 60000, kBlock, L, R);
        const f64 back  = tBinMag(L, 36000, 16384, tMidiHz(45));
        const f64 gone  = tBinMag(L, 36000, 16384, tMidiHz(50));
        CHECK(back > 0.02 && back > 5.0 * gone,
              "releasing the top of a held pair falls back: |held| %.4f vs "
              "|released| %.4f", back, gone);
    }

    // --- 5. LEGATO GLIDES THE OVERLAP: with 2000 ms glide (the widened id
    // 38), 250 ms in, the pitch is still travelling — between the two notes.
    {
        auto s = spV2Base(reg, *d);
        if (!s) return;
        s->setParam(spIdx(*s, "Voice Mode"), 2.f);
        s->setParam(spIdx(*s, "Glide"), 2000.f);
        std::vector<f32> L, R;
        spRender(*s, { { 0, 0x90, 48, 110 }, { 24000, 0x90, 60, 110 } },
                 48000, kBlock, L, R);
        // 250 ms after the overlap the glide has covered ~1.5 of 12 st: the
        // instantaneous pitch sits between the endpoints, so BOTH endpoint
        // bins are weak over this window compared to the note before.
        const f64 before = tBinMag(L, 8000, 8192, tMidiHz(48));
        const f64 stillA = tBinMag(L, 30000, 8192, tMidiHz(48));
        const f64 yetB   = tBinMag(L, 30000, 8192, tMidiHz(60));
        CHECK(before > 0.02 && stillA < 0.5 * before && yetB < 0.5 * before,
              "a 2 s glide is mid-travel at 250 ms: |48| %.4f -> %.4f, |60| %.4f",
              before, stillA, yetB);
    }

    // --- 6. THE WIDENED FILTER: LP24 rolls off far harder than LP12 above
    // cutoff, HP24 harder below, and the Notch notches.
    {
        f64 hf12 = 0.0, hf24 = 0.0;
        for (int t = 0; t < 2; ++t) {
            auto s = spV2Base(reg, *d);
            if (!s) return;
            s->setParam(spIdx(*s, "A Level"), 0.f);
            s->setParam(spIdx(*s, "Noise"), 1.f);
            s->setParam(spIdx(*s, "Cutoff"), 800.f);
            s->setParam(spIdx(*s, "Filter Type"), t == 0 ? 0.f : 3.f);
            std::vector<f32> L, R;
            spRender(*s, { { 0, 0x90, 60, 110 } }, 8192 + 16384, kBlock, L, R);
            const f64 v = spV2BandDb(L, 8192, 16384, 6000.0, 16000.0) -
                          spV2BandDb(L, 8192, 16384, 100.0, 700.0);
            if (t == 0) hf12 = v; else hf24 = v;
        }
        CHECK(hf24 < hf12 - 12.0,
              "LP24 is %.1f dB steeper than LP12 three octaves up (needs > 12)",
              hf12 - hf24);

        // Notch: a sine sub parked exactly on the cutoff all but disappears;
        // the same sub through the wide-open v1 LP does not.
        f64 mag[2] = { 0.0, 0.0 };
        for (int t = 0; t < 2; ++t) {
            auto s = spV2Base(reg, *d);
            if (!s) return;
            s->setParam(spIdx(*s, "A Level"), 0.f);
            s->setParam(spIdx(*s, "Sub"), 1.f);
            s->setParam(spIdx(*s, "Sub Oct"), 0.f);
            s->setParam(spIdx(*s, "Resonance"), 0.7f);
            if (t == 1) {
                s->setParam(spIdx(*s, "Filter Type"), 5.f);
                s->setParam(spIdx(*s, "Cutoff"), (f32)tMidiHz(81));
            }
            std::vector<f32> L, R;
            spRender(*s, { { 0, 0x90, 81, 110 } }, 48000, kBlock, L, R);
            mag[t] = tBinMag(L, 24000, 16384, tMidiHz(81));
        }
        CHECK(mag[1] < 0.1 * mag[0],
              "the Notch parked on the tone kills it: %.4f -> %.4f", mag[0], mag[1]);
    }
}

// The v2 busy patch: both warps on (mutually — FM one way, RM the other),
// every slot of the matrix live across nine destinations and eight distinct
// sources, all three LFOs running (one synced, one S&H), ENV3 in play, a
// shaped tracking sub, colored tracked noise, LP24, drive — nothing left at
// rest except what the script then moves.
static void spV2BusyPatch(PluginInstance& s) {
    spBusyPatch(s);
    s.setParam(spIdx(s, "Filter Type"), 3.f);       // LP24
    s.setParam(spIdx(s, "Sub Shape"), 2.f);
    s.setParam(spIdx(s, "Sub Oct"), -2.f);
    s.setParam(spIdx(s, "Noise Color"), 0.4f);
    s.setParam(spIdx(s, "Noise Track"), 1.f);
    s.setParam(spIdx(s, "A Warp"), 6.f);            // A FM'd by B
    s.setParam(spIdx(s, "A Warp Amt"), 0.45f);
    s.setParam(spIdx(s, "B Warp"), 7.f);            // B ring-mods against A
    s.setParam(spIdx(s, "B Warp Amt"), 0.6f);
    s.setParam(spIdx(s, "L2 Rate"), 3.7f);
    s.setParam(spIdx(s, "L2 Shape"), 1.f);
    s.setParam(spIdx(s, "L3 Sync"), 6.f);           // 1/8 against the transport
    s.setParam(spIdx(s, "L3 Shape"), 4.f);          // S&H
    s.setParam(spIdx(s, "E3 Attack"), 40.f);
    s.setParam(spIdx(s, "E3 Decay"), 500.f);
    s.setParam(spIdx(s, "E3 Sustain"), 0.4f);
    s.setParam(spIdx(s, "Macro 2"), 0.7f);
    spV2Slot(s, 0, 2  /*LFO2*/,      1  /*A Pos*/,     0.5f);
    spV2Slot(s, 1, 3  /*LFO3 S&H*/,  11 /*Cutoff*/,    0.4f);
    spV2Slot(s, 2, 5  /*ENV3*/,      8  /*B Pitch*/,   0.3f);
    spV2Slot(s, 3, 13 /*Random*/,    16 /*Pan*/,       0.8f);
    spV2Slot(s, 4, 6  /*Velocity*/,  13 /*Drive*/,     0.5f);
    spV2Slot(s, 5, 10 /*Macro2*/,    12 /*Resonance*/, 0.3f);
    spV2Slot(s, 6, 8  /*Aftertouch*/,10 /*Noise Lvl*/, 0.5f);
    spV2Slot(s, 7, 7  /*KeyTrk*/,    14 /*A Detune*/,  0.4f);
}

static void testSpectraV2Determinism(PluginRegistry& reg) {
    banner("Spectra v2: block-size invariance with everything engaged");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;

    const int kFrames = 14000;
    // The v1 script plus channel pressure landing mid-phrase — aftertouch
    // rides the same stamped-sample queue as the notes.
    std::vector<SpEvent> ev = spScript();
    ev.push_back({ 3000, 0xD0, 96, 0 });
    ev.push_back({ 9000, 0xD0, 20, 0 });
    std::sort(ev.begin(), ev.end(),
              [](const SpEvent& a, const SpEvent& b) { return a.frame < b.frame; });

    // --- 1. the busy v2 patch, Poly.
    {
        auto build = [&]() {
            auto s = reg.instantiate(*d, kSR, kBlock);
            if (s) spV2BusyPatch(*s);
            return s;
        };
        auto ref = build();
        if (!ref) return;
        std::vector<f32> refL, refR, altL, altR;
        spRender(*ref, ev, kFrames, kBlock, refL, refR, 120.0);
        f32 pk = 0.f;
        for (f32 v : refL) pk = std::fmax(pk, std::fabs(v));
        CHECK(pk > 0.05f, "the v2 busy render is not silent (peak %.4f)", (double)pk);

        bool all = true;
        for (int chunk : { 1, 7, 64, 300, 1024 }) {
            auto alt = build();
            if (!alt) break;
            spRender(*alt, ev, kFrames, chunk, altL, altR, 120.0);
            f32 diff = 0.f;
            for (int i = 0; i < kFrames; ++i) {
                diff = std::fmax(diff, std::fabs(refL[(size_t)i] - altL[(size_t)i]));
                diff = std::fmax(diff, std::fabs(refR[(size_t)i] - altR[(size_t)i]));
            }
            if (diff != 0.f) all = false;
            CHECK(diff == 0.f,
                  "v2 busy patch: blocks of %d bit-identical to %d (max diff %.9f)",
                  chunk, kBlock, (double)diff);
        }
        CHECK(all, "warp (mutual FM/RM and its one-sample delay), all matrix slots, "
                   "LFO2/3, ENV3, colored noise, LP24 — all on absolute sample time");

        // Two instances, same file: the Random hash and the S&H streams are
        // identity- and seed-based, never clocked.
        auto a = build();
        auto b = build();
        if (a && b) {
            std::vector<f32> aL, aR, bL, bR;
            spRender(*a, ev, kFrames, kBlock, aL, aR, 120.0);
            spRender(*b, ev, kFrames, kBlock, bL, bR, 120.0);
            CHECK(spV2MaxDiff(aL, bL) == 0.f,
                  "two fresh instances render the v2 busy patch identically");
        }
    }

    // --- 2. the same, Legato with a long glide — the held-note stack, the
    // no-retrigger path and the fallback all land on stamped samples.
    {
        auto build = [&]() {
            auto s = reg.instantiate(*d, kSR, kBlock);
            if (s) {
                spV2BusyPatch(*s);
                s->setParam(spIdx(*s, "Voice Mode"), 2.f);
                s->setParam(spIdx(*s, "Glide"), 900.f);
            }
            return s;
        };
        auto ref = build();
        if (!ref) return;
        std::vector<f32> refL, refR, altL, altR;
        spRender(*ref, ev, kFrames, kBlock, refL, refR, 120.0);
        f32 pk = 0.f;
        for (f32 v : refL) pk = std::fmax(pk, std::fabs(v));
        CHECK(pk > 0.02f, "the Legato render is not silent (peak %.4f)", (double)pk);
        bool all = true;
        for (int chunk : { 1, 64, 300, 1024 }) {
            auto alt = build();
            if (!alt) break;
            spRender(*alt, ev, kFrames, chunk, altL, altR, 120.0);
            f32 diff = 0.f;
            for (int i = 0; i < kFrames; ++i) {
                diff = std::fmax(diff, std::fabs(refL[(size_t)i] - altL[(size_t)i]));
                diff = std::fmax(diff, std::fabs(refR[(size_t)i] - altR[(size_t)i]));
            }
            if (diff != 0.f) all = false;
            CHECK(diff == 0.f,
                  "Legato + glide: blocks of %d bit-identical to %d (max diff %.9f)",
                  chunk, kBlock, (double)diff);
        }
        CHECK(all, "Mono/Legato voice management applies at stamped samples");
    }
}

// ---------------------------------------------------------------------------
// Spectra v3 (ids 100..110, the spent reserved ids 60/61/99, three widened
// enums and the state string) — drawable LFOs, one-shot LFOs, MIDI as a mod
// source, and the per-slot matrix response curves.
//
// House rule, unchanged since v1: everything below MEASURES. A drawn grid is
// not asserted to exist, its sixteen levels are read out of the rendered audio
// and compared against d/15; a one-shot is not asserted to be an envelope, it
// is shown to clamp; a response curve is not asserted to be x*x, the number is
// checked at four points including the two the contract fixes (f(0) = 0 and
// f(1) = 1).
//
// HOW A MODULATOR IS READ OUT OF THE AUDIO. spV3Probe() below is a patch whose
// output level IS the modulation: one oscillator, one unison voice, a saw at
// position 0, the filter wide open and the envelopes flat, so the voice's
// output is `saw(t) * A Level * ENV1 * velAmp * Master` and every factor but
// A Level is constant for the whole render. A matrix slot driving A Level from
// a base of 0 with amount 1 then makes the local peak of the output
// proportional to the source, and dividing by the render's own maximum cancels
// the constant. That is the whole instrument, used as a voltmeter.
// ---------------------------------------------------------------------------

// One matrix slot including v3's per-slot curve: slot k is 68+3k / 69+3k /
// 70+3k for the triple and 101+k for the curve, by raw contract ids.
static void spV3Slot(PluginInstance& s, int k, int src, int dst, f32 amt, int curve = 0) {
    s.setParam(68 + 3 * k, (f32)src);
    s.setParam(69 + 3 * k, (f32)dst);
    s.setParam(70 + 3 * k, amt);
    s.setParam(101 + k, (f32)curve);
}

static std::unique_ptr<PluginInstance> spV3Probe(PluginRegistry& reg, const PluginDesc& d) {
    auto s = reg.instantiate(d, kSR, kBlock);
    if (!s) return s;
    s->setParam(spIdx(*s, "A Table"), 0.f);
    s->setParam(spIdx(*s, "A Position"), 0.f);
    s->setParam(spIdx(*s, "A Level"), 0.f);         // the slot supplies it
    s->setParam(spIdx(*s, "A Unison"), 1.f);
    s->setParam(spIdx(*s, "A Detune"), 0.f);
    s->setParam(spIdx(*s, "A Spread"), 0.f);
    s->setParam(spIdx(*s, "B Level"), 0.f);
    s->setParam(spIdx(*s, "Attack"), 0.1f);
    s->setParam(spIdx(*s, "Decay"), 5000.f);
    s->setParam(spIdx(*s, "Sustain"), 1.f);
    s->setParam(spIdx(*s, "Release"), 1.f);
    s->setParam(spIdx(*s, "Cutoff"), 20000.f);
    s->setParam(spIdx(*s, "Resonance"), 0.f);
    s->setParam(spIdx(*s, "Master"), 1.f);
    s->setParam(spIdx(*s, "Voices"), 1.f);
    return s;
}

// Peak |x| over a window, clipped to the buffer.
static f32 spV3Peak(const std::vector<f32>& x, int from, int n) {
    f32 pk = 0.f;
    const int lo = from < 0 ? 0 : from;
    const int hi = (from + n) > (int)x.size() ? (int)x.size() : (from + n);
    for (int i = lo; i < hi; ++i) pk = std::fmax(pk, std::fabs(x[(size_t)i]));
    return pk;
}

// One note, held for the whole render, at C6 — 1046.5 Hz is ~46 samples a
// cycle, so a 256-sample probe window holds five and a half of them and its
// peak is a stable reading of the level.
static std::vector<SpEvent> spV3Note(int frame = 0, u8 note = 84) {
    return { { frame, 0x90, note, 127 } };
}

static const char* kSpV3RampGrid = "0123456789abcdef";

// ---------------------------------------------------------------------------

static void testSpectraV3Contract(PluginRegistry& reg) {
    banner("Spectra v3: the widened enums, the spent reserved ids and the append");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;
    auto s = reg.instantiate(*d, kSR, kBlock);
    if (!s) return;

    // v3 took the count to 111; v4 appends past it and this check follows the
    // tip rather than freezing at the revision that wrote it — the property is
    // "the append is dense and nothing moved", not "the list stopped here".
    CHECK(s->paramCount() >= 111, "kSpParamCount went 100 -> 111 and up (it is %d)",
          s->paramCount());
    CHECK(d->paramCount == s->paramCount(), "the descriptor advertises the same (%d)",
          d->paramCount);

    // The one default the whole revision is written around.
    CHECK(s->getParam(99) == 2.f,
          "Bend Range defaults to 2 semitones, not 0 — v3's ONE stated exception "
          "to \"every default does what v2 did\" (it is %g)", (double)s->getParam(99));

    // ...and every other new default is inert.
    bool inert = true;
    for (int id : { 100, 60, 61 }) if (s->getParam(id) != 0.f) inert = false;
    for (int k = 0; k < 8; ++k) if (s->getParam(101 + k) != 0.f) inert = false;
    CHECK(inert, "L1/L2/L3 Mode default to Loop and all eight curves to Linear, so "
                 "every v3 parameter but Bend Range is the v2 behaviour");

    // The three widenings are supersets: the old top value keeps its meaning
    // and the new one is reachable.
    s->setParam(0, 8.f);  CHECK(s->getParam(0) == 8.f, "A Table reaches the custom slot 8");
    s->setParam(8, 8.f);  CHECK(s->getParam(8) == 8.f, "B Table reaches the custom slot 8");
    s->setParam(0, 9.f);  CHECK(s->getParam(0) == 8.f, "A Table clamps above 8 (a later, "
                                                       "wider build's value arrives as 8)");
    s->setParam(0, 0.f);
    s->setParam(8, 1.f);
    for (int id : { 37, 56, 59 }) {
        s->setParam(id, 5.f);
        CHECK(s->getParam(id) == 5.f, "id %d (LFO Shape) reaches 5 = Custom", id);
        s->setParam(id, 6.f);
        CHECK(s->getParam(id) == 5.f, "id %d clamps above 5", id);
        s->setParam(id, 0.f);
    }
    for (int k = 0; k < 8; ++k) {
        s->setParam(68 + 3 * k, 16.f);
        if (s->getParam(68 + 3 * k) != 16.f)
            CHECK(false, "M%d Src does not reach source 16 (MIDI CC)", k + 1);
        s->setParam(68 + 3 * k, 0.f);
    }
    CHECK(true, "all eight M Src reach 16 — 14 Mod Wheel, 15 Pitch Bend, 16 MIDI CC");

    // The destination enum does NOT widen: nothing v3 adds is a modulatable
    // target. Stated as a check because "we did not do it" is a decision.
    s->setParam(69, 20.f);
    CHECK(s->getParam(69) == 19.f, "M1 Dst still stops at 19 — the destination enum "
                                   "does not widen in v3");
    s->setParam(69, 0.f);

    // Eight reserved ids remain, and 92/93 are two of them: they were the
    // obvious home for the per-slot curves and could not hold eight. v3's own
    // tail (109, 110) is spent by v4 and its replacement tail is 123/124, so
    // the count of reserved ids is still ten and the LIST has moved by two.
    int reserved = 0;
    for (int id : { 46, 47, 52, 53, 66, 67, 92, 93, 123, 124 })
        if (s->paramInfo(id).name == "\xE2\x80\x94") ++reserved;
    CHECK(reserved == 10, "ten ids are still registered reserved (\"—\", 0..1, default 0): "
                          "the eight v2 left plus the current tail (%d found)", reserved);
    CHECK(s->paramInfo(92).name == "\xE2\x80\x94" && s->paramInfo(93).name == "\xE2\x80\x94",
          "92 and 93 stayed reserved — an id ARRAY must be contiguous and two ids "
          "cannot hold eight curves");
}

// ---------------------------------------------------------------------------

static void testSpectraV3State(PluginRegistry& reg) {
    banner("Spectra v3: the state string (nxspc1)");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;
    auto s = reg.instantiate(*d, kSR, kBlock);
    if (!s) return;

    // THE ROUND-TRIP GATE. Spectra had no state string before v3; a v2 project
    // must still write no `state` key at all.
    CHECK(s->stateString().empty(),
          "a fresh instance's stateString() is EMPTY, so a v2 project round-trips "
          "byte-identically");
    spBusyPatch(*s);
    CHECK(s->stateString().empty(),
          "...and stays empty however many PARAMETERS move — state is state");
    CHECK(s->setStateString(""), "the empty state is accepted (it is not malformed)");

    // Every one of bank 1's 49 rows is a v2 preset and must emit nothing.
    {
        auto q = reg.instantiate(*d, kSR, kBlock);
        int nonEmpty = 0;
        if (q) for (int i = 0; i < 49 && i < q->presetCount(); ++i) {
            q->loadPreset(i);
            if (!q->stateString().empty()) ++nonEmpty;
        }
        CHECK(nonEmpty == 0, "all 49 bank-1 presets still emit no state (%d did)", nonEmpty);
    }

    // --- what a v3 state looks like, and that it is its own inverse.
    {
        auto q = reg.instantiate(*d, kSR, kBlock);
        if (!q) return;
        const std::string in =
            "nxspc1;lfo1=0123456789abcdef;lfo3=f0f0f0f0f0f0f0f0;smooth1=250;"
            "smooth3=1000;cc=74";
        CHECK(q->setStateString(in), "a full grid/smooth/cc state is accepted");
        CHECK(q->stateString() == in,
              "...and is re-emitted EXACTLY (write and read are inverses)\n        got: %s",
              q->stateString().c_str());

        // Records are written in the contract's order whatever order they
        // arrive in, so a round trip is canonical rather than merely lossless.
        auto r = reg.instantiate(*d, kSR, kBlock);
        if (r) {
            CHECK(r->setStateString("nxspc1;cc=74;smooth3=1000;smooth1=250;"
                                    "lfo3=f0f0f0f0f0f0f0f0;lfo1=0123456789abcdef"),
                  "the same records in a different order are accepted");
            CHECK(r->stateString() == in, "...and normalise to the contract's order");
        }
    }

    // A smooth with no grid, and a grid with no smooth: independent records,
    // and an all-zero grid is the DEFAULT and emits nothing.
    {
        auto q = reg.instantiate(*d, kSR, kBlock);
        if (q) {
            CHECK(q->setStateString("nxspc1;lfo2=0000000000000000"),
                  "an all-zero grid parses");
            CHECK(q->stateString().empty(),
                  "...and emits nothing, because all sixteen levels 0 IS the default "
                  "(got \"%s\")", q->stateString().c_str());
            CHECK(q->setStateString("nxspc1;smooth2=1"), "a smooth with no grid parses");
            CHECK(q->stateString() == "nxspc1;smooth2=1",
                  "...and is emitted on its own");
        }
    }

    // A custom-table record survives a machine that cannot resolve it: THE
    // RECORD IS KEPT AND RE-EMITTED VERBATIM, and setStateString still returns
    // TRUE — a missing file is not a malformed state.
    {
        auto q = reg.instantiate(*d, kSR, kBlock);
        if (q) {
            const std::string wt =
                "nxspc1;wtA=0123456789abcdef;wtpathA=/tmp/no%20such%20file.wav";
            CHECK(q->setStateString(wt),
                  "a state naming a wavetable this machine does not have is ACCEPTED");
            CHECK(q->stateString() == wt,
                  "...and its records are re-emitted verbatim, so saving here does not "
                  "lose the file's name\n        got: %s", q->stateString().c_str());
            q->setParam(0, 8.f);
            CHECK(q->getParam(0) == 8.f,
                  "...and the PARAMETER keeps its value — the set's intent is not "
                  "edited by the machine that could not honour it");
        }
    }

    // --- refusals. Every one of these leaves the device exactly as it was.
    {
        auto q = reg.instantiate(*d, kSR, kBlock);
        if (!q) return;
        const std::string good = "nxspc1;lfo1=0123456789abcdef;smooth1=250;cc=7";
        q->setStateString(good);

        struct Bad { const char* s; const char* why; };
        const Bad bad[] = {
            { "nxsmp1;lfo1=0123456789abcdef",       "a version tag from another device" },
            { "nxspc2;lfo1=0123456789abcdef",       "a version tag from a later build" },
            { "nxspc1;;;;",                          "empty records (the reason they are refused rather than skipped)" },
            { "nxspc1;lfo1",                         "a record with no '='" },
            { "nxspc1;=0123456789abcdef",            "a record with an empty key" },
            { "nxspc1;lfo1=0123456789abcde",         "a 15-digit grid" },
            { "nxspc1;lfo1=0123456789abcdefa",       "a 17-digit grid" },
            { "nxspc1;lfo1=0123456789ABCDEF",        "an UPPERCASE grid (read is strict, so read and write are inverses)" },
            { "nxspc1;lfo1=0123456789abcdeg",        "a non-hex grid digit" },
            { "nxspc1;lfo1=0000000000000000;lfo1=1000000000000000", "a duplicate key (choosing one of two is guessing)" },
            { "nxspc1;smooth1=1001",                 "a smooth over 1000" },
            { "nxspc1;smooth1=0250",                 "a leading zero (two spellings of one number)" },
            { "nxspc1;smooth1=",                     "an empty smooth" },
            { "nxspc1;smooth1=25.0",                 "a DECIMAL POINT — the locale hazard the integer encoding exists to avoid" },
            { "nxspc1;smooth1=-1",                   "a negative smooth" },
            { "nxspc1;cc=128",                       "a controller number over 127" },
            { "nxspc1;cc=007",                       "a leading-zero controller number" },
            { "nxspc1;wtA=0123456789abcde",          "a 15-digit content hash" },
            { "nxspc1;wtpathA=/tmp/a b",             "a raw space in a path (a byte the writer would have escaped)" },
            { "nxspc1;wtpathA=/tmp/a%2",             "a truncated escape" },
            { "nxspc1;wtpathA=/tmp/a%zz",            "a non-hex escape" },
            { "nxspc1;wtpathA=/tmp/a%00b",           "an escape decoding to NUL" },
            { "nxspc1;wtpathA=",                     "an empty path" },
        };
        int refused = 0;
        for (const Bad& t : bad) {
            const bool ok = q->setStateString(t.s);
            if (ok) CHECK(false, "accepted a state it must refuse: %s", t.why);
            else    ++refused;
        }
        CHECK(refused == (int)(sizeof bad / sizeof bad[0]),
              "all %d malformed states are refused", (int)(sizeof bad / sizeof bad[0]));
        CHECK(q->stateString() == good,
              "and a refusal changed NOTHING: the state is still the one that was "
              "accepted\n        got: %s", q->stateString().c_str());
    }

    // Forward compatibility: a record this build does not know is SKIPPED, and
    // the ones beside it still apply. That is the whole reason the format is
    // key=value and not positional.
    {
        auto q = reg.instantiate(*d, kSR, kBlock);
        if (q) {
            CHECK(q->setStateString("nxspc1;lfo9=deadbeefdeadbeef;smooth1=500;"
                                    "future=whatever42"),
                  "records from a later build are skipped, not refused");
            CHECK(q->stateString() == "nxspc1;smooth1=500",
                  "...and this build re-emits only what it understands (got \"%s\")",
                  q->stateString().c_str());
        }
    }

    // A path with every byte class the escaper cares about, through the
    // SAMPLER'S escaper — the same function, not a second one.
    {
        auto q = reg.instantiate(*d, kSR, kBlock);
        if (q) {
            const std::string st = "nxspc1;wtA=00000000000000ff;"
                                   "wtpathA=/tmp/a%20b%3Bc%2Cd%3Ae%3Df%25g%09h";
            CHECK(q->setStateString(st), "a path carrying space ; , : = %% and a tab parses");
            CHECK(q->stateString() == st,
                  "...and re-escapes to the identical bytes (uppercase hex, the "
                  "sampler's spelling)\n        got: %s", q->stateString().c_str());
        }
    }

    // loadPreset resets STATE as well as parameters. Deliberately unlike the
    // sampler, which keeps its file: a wavetable and a drawn grid are sound
    // design, and sound design is what a preset replaces.
    {
        auto q = reg.instantiate(*d, kSR, kBlock);
        if (q) {
            q->setStateString("nxspc1;lfo1=0123456789abcdef;smooth1=250;cc=74");
            q->loadPreset(0);                          // Init: zero overrides
            CHECK(q->stateString().empty(),
                  "loadPreset(Init) resets every state block to its default too "
                  "(got \"%s\")", q->stateString().c_str());
        }
    }

    // ...and a preset row that CARRIES state hands it over. Bank 2 is authored
    // with SPLFO/SPWTA/SPCC rows; at least one of them must reach the device,
    // or the row format is a no-op nobody would notice.
    {
        auto q = reg.instantiate(*d, kSR, kBlock);
        int withState = 0, withGrid = 0, withCC = 0, withWt = 0;
        if (q) for (int i = 49; i < q->presetCount(); ++i) {
            q->loadPreset(i);
            const std::string st = q->stateString();
            if (st.empty()) continue;
            ++withState;
            if (st.find(";lfo") != std::string::npos) ++withGrid;
            if (st.find(";cc=") != std::string::npos) ++withCC;
            if (st.find(";wt")  != std::string::npos) ++withWt;
        }
        CHECK(withState > 0, "%d bank-2 presets carry state through the SPLFO/SPWTA/SPCC "
                             "macros", withState);
        CHECK(withGrid > 0, "%d of them carry a drawn grid", withGrid);
        CHECK(withCC > 0, "%d of them learn a controller", withCC);
        CHECK(withWt > 0, "%d of them name a custom wavetable", withWt);
    }

    // The state is a state: two instances given the same string render the
    // same audio, which is the only property a state string actually promises.
    {
        auto a = reg.instantiate(*d, kSR, kBlock);
        auto b2 = reg.instantiate(*d, kSR, kBlock);
        if (a && b2) {
            for (auto* q : { a.get(), b2.get() }) {
                spBusyPatch(*q);
                q->setParam(37, 5.f);                 // LFO Shape = Custom
                q->setStateString("nxspc1;lfo1=08c4f6a20d9315be;smooth1=250");
            }
            std::vector<f32> aL, aR, bL, bR;
            spRender(*a, spScript(), 9000, kBlock, aL, aR, 120.0);
            spRender(*b2, spScript(), 9000, kBlock, bL, bR, 120.0);
            CHECK(spV3Peak(aL, 0, 9000) > 0.02f, "the state-restored render is not silent");
            CHECK(spV2MaxDiff(aL, bL) == 0.f && spV2MaxDiff(aR, bR) == 0.f,
                  "two instances given the same state string render bit-identically");
        }
    }
}

// ---------------------------------------------------------------------------

static void testSpectraV3CustomLfo(PluginRegistry& reg) {
    banner("Spectra v3: the drawable 16-step LFO (shape 5)");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;

    // One cycle a second at 48 kHz is 3000 samples a step, which is wide
    // enough to read a 256-sample probe out of the middle of.
    const int kStep = 3000;
    const int kFrames = 16 * kStep + 2000;

    auto build = [&](const char* grid, int smoothMilli, f32 amt) {
        auto s = spV3Probe(reg, *d);
        if (!s) return s;
        s->setParam(spIdx(*s, "LFO Shape"), 5.f);
        s->setParam(spIdx(*s, "LFO Sync"), 0.f);
        s->setParam(spIdx(*s, "LFO Rate"), 1.f);
        spV3Slot(*s, 0, 1 /*LFO1*/, 5 /*A Level*/, amt);
        if (grid) {
            char st[128];
            std::snprintf(st, sizeof st, "nxspc1;lfo1=%s;smooth1=%d", grid, smoothMilli);
            if (smoothMilli == 0) std::snprintf(st, sizeof st, "nxspc1;lfo1=%s", grid);
            s->setStateString(st);
        }
        return s;
    };

    // --- 1. shape 5 with NO grid in the state is not malformed: the grid reads
    //        as its default, all zeros, and the LFO is silent. An all-zero grid
    //        MUST be silence — that is the reason the domain is unipolar.
    {
        auto s = build(nullptr, 0, 1.f);
        std::vector<f32> L, R;
        if (s) spRender(*s, spV3Note(), 8000, kBlock, L, R);
        CHECK(s && spV3Peak(L, 0, 8000) == 0.f,
              "shape 5 with no grid in the state is SILENT, not malformed (peak %.9f)",
              s ? (double)spV3Peak(L, 0, 8000) : -1.0);
    }

    // --- 2. the sixteen levels, read out of the audio. L(d) = d/15 exactly,
    //        digit 0 is 0.0 and digit 15 is 1.0.
    {
        const char* grid = "0369cf0369cf0369";     // 0,3,6,9,12,15 repeating
        const u8 want[16] = { 0,3,6,9,12,15, 0,3,6,9,12,15, 0,3,6,9 };
        auto s = build(grid, 0, 1.f);
        std::vector<f32> L, R;
        if (!s) return;
        spRender(*s, spV3Note(), kFrames, kBlock, L, R);

        f32 mx = 0.f;
        f32 probe[16];
        for (int k = 0; k < 16; ++k) {
            probe[k] = spV3Peak(L, k * kStep + kStep / 2 - 128, 256);
            mx = std::fmax(mx, probe[k]);
        }
        CHECK(mx > 0.05f, "the drawn-grid render is not silent (max probe %.4f)", (double)mx);
        f64 worst = 0.0;
        int bad = 0;
        for (int k = 0; k < 16; ++k) {
            const f64 got = (f64)probe[k] / (f64)mx;
            const f64 exp = (f64)want[k] / 15.0;
            worst = std::fmax(worst, std::fabs(got - exp));
            if (std::fabs(got - exp) > 0.04) {
                CHECK(false, "step %d reads %.4f, the grid digit %u says %.4f",
                      k, got, (unsigned)want[k], exp);
                ++bad;
            }
        }
        CHECK(bad == 0, "all sixteen steps read L(d) = d/15 out of the audio "
                        "(worst error %.4f)", worst);

        // ...and the SCALE is absolute, not merely proportional. Normalising by
        // the render's own maximum would let L(d) = d/16 pass unnoticed, so the
        // full-scale digit is calibrated against an unmodulated A Level of 1:
        // digit 15 is exactly 1.0 and digit 0 is exactly 0.0, which is what
        // makes an all-zero grid silence and a full grid a full-scale source.
        {
            auto cal = spV3Probe(reg, *d);
            if (cal) {
                cal->setParam(spIdx(*cal, "A Level"), 1.f);
                std::vector<f32> cL, cR;
                spRender(*cal, spV3Note(), 8000, kBlock, cL, cR);
                const f32 unit = spV3Peak(cL, 4000, 256);
                CHECK(unit > 0.05f, "the absolute calibration reads %.4f", (double)unit);
                CHECK(std::fabs((f64)mx / unit - 1.0) < 0.02,
                      "digit f is L = 1.0 on the destination's OWN scale, not merely the "
                      "loudest step (%.4f of an unmodulated A Level of 1)",
                      (double)(mx / unit));
                CHECK(std::fabs((f64)probe[1] / unit - 3.0 / 15.0) < 0.02,
                      "digit 3 is 3/15 = %.4f absolute (reads %.4f)",
                      3.0 / 15.0, (double)(probe[1] / unit));
            }
        }
        CHECK(probe[0] == 0.f && probe[6] == 0.f && probe[12] == 0.f,
              "digit 0 is EXACTLY 0.0 — the three steps drawn at zero are bit-silent");

        // The leftmost digit is step 0, the step in effect at phase 0. Read at
        // the very start of the render, before the LFO has moved.
        auto s2 = build("f00000000000000f", 0, 1.f);
        std::vector<f32> L2, R2;
        if (s2) {
            spRender(*s2, spV3Note(), kFrames, kBlock, L2, R2);
            const f32 first = spV3Peak(L2, 200, 256);
            const f32 last  = spV3Peak(L2, 15 * kStep + kStep / 2, 256);
            const f32 mid   = spV3Peak(L2, 8 * kStep + kStep / 2, 256);
            CHECK(first > 0.05f && last > 0.05f && mid == 0.f,
                  "the LEFTMOST digit is step 0 and the rightmost is step 15 "
                  "(first %.4f, middle %.4f, last %.4f)",
                  (double)first, (double)mid, (double)last);
        }
    }

    // --- 3. UNIPOLAR, and the check is crisp: a negative amount on a source
    //        that never goes below zero can only ever clamp A Level to 0, so
    //        the render is silent. The same slot with a bipolar shape is not.
    {
        auto uni = build("0369cf0369cf0369", 0, -1.f);
        std::vector<f32> L, R;
        if (uni) spRender(*uni, spV3Note(), kFrames, kBlock, L, R);
        CHECK(uni && spV3Peak(L, 0, kFrames) == 0.f,
              "a drawn grid at amount -1 is SILENT: its domain is [0..1], not [-1..1] "
              "(peak %.9f)", uni ? (double)spV3Peak(L, 0, kFrames) : -1.0);

        auto bip = spV3Probe(reg, *d);
        if (bip) {
            bip->setParam(spIdx(*bip, "LFO Shape"), 0.f);      // sine, bipolar
            bip->setParam(spIdx(*bip, "LFO Rate"), 1.f);
            spV3Slot(*bip, 0, 1, 5, -1.f);
            std::vector<f32> bL, bR;
            spRender(*bip, spV3Note(), kFrames, kBlock, bL, bR);
            CHECK(spV3Peak(bL, 0, kFrames) > 0.05f,
                  "...while shapes 0..4 keep the [-1..1] domain they have always had "
                  "(peak %.4f)", (double)spV3Peak(bL, 0, kFrames));
        }
    }

    // --- 4. smooth. s = 0 is a HARD STAIRCASE selected outright; s > 0 lags.
    {
        // Grid f then 0: one full-scale step followed by a zero, so the
        // boundary at 1*3000 is the sharpest edge the grid can make.
        const char* grid = "f0f0f0f0f0f0f0f0";
        auto hard = build(grid, 0, 1.f);
        auto soft = build(grid, 500, 1.f);
        std::vector<f32> hL, hR, sL, sR;
        if (!hard || !soft) return;
        spRender(*hard, spV3Note(), kFrames, kBlock, hL, hR);
        spRender(*soft, spV3Note(), kFrames, kBlock, sL, sR);

        const int edge = kStep;                 // f -> 0
        const f32 hAfter = spV3Peak(hL, edge + 60, 192);
        const f32 hDeep  = spV3Peak(hL, edge + 1200, 512);
        const f32 sAfter = spV3Peak(sL, edge + 60, 192);
        const f32 hFull  = spV3Peak(hL, kStep / 2 - 128, 256);
        CHECK(hFull > 0.05f, "the staircase render is not silent (%.4f)", (double)hFull);
        // The oscillator stops on the sample the step changes, so what is left
        // just after the edge is the FILTER'S OWN TAIL and not a lag: it is
        // three orders of magnitude down and gone entirely a step later, while
        // the smoothed edge below is still a fifth of full scale.
        CHECK(hAfter < 0.005f * hFull,
              "smooth 0 is a hard staircase: the level is already down %.1f dB one "
              "sample past the edge (only the filter's ring is left)",
              20.0 * std::log10((double)(hAfter + 1e-12f) / (double)hFull));
        CHECK(hDeep == 0.f,
              "...and bit-silent well inside the zero step (%.9f)", (double)hDeep);
        CHECK(sAfter > 0.15f * hFull,
              "smooth 500 lags the same edge (%.4f of full scale %.4f)",
              (double)(sAfter / hFull), (double)hFull);

        // ...and it is a LAG and not a delay: it decays towards the new level
        // rather than stepping there late.
        const f32 s1 = spV3Peak(sL, edge + 60, 192);
        const f32 s2 = spV3Peak(sL, edge + 600, 192);
        const f32 s3 = spV3Peak(sL, edge + 1600, 192);
        CHECK(s1 > s2 && s2 > s3,
              "the smoothed edge decays monotonically (%.4f > %.4f > %.4f)",
              (double)s1, (double)s2, (double)s3);

        // The coefficient is continuous across its own default: a very small
        // smooth is very nearly the staircase.
        auto tiny = build(grid, 1, 1.f);
        std::vector<f32> tL, tR;
        if (tiny) {
            spRender(*tiny, spV3Note(), kFrames, kBlock, tL, tR);
            CHECK(spV3Peak(tL, edge + 60, 192) < 0.02f * hFull,
                  "smooth 1/1000 is within a whisker of the s = 0 branch — the control "
                  "is continuous across its default");
        }
    }

    // --- 5. THE DIVISION IS THE WHOLE CYCLE, not one step. Sync 5 (1/4) at
    //        120 bpm is a 0.5 s quarter note, so all sixteen steps run inside
    //        24000 samples and the grid repeats twice in 48000.
    {
        auto s = spV3Probe(reg, *d);
        if (s) {
            s->setParam(spIdx(*s, "LFO Shape"), 5.f);
            s->setParam(spIdx(*s, "LFO Sync"), 5.f);          // 1/4
            spV3Slot(*s, 0, 1, 5, 1.f);
            s->setStateString("nxspc1;lfo1=f000000000000000");
            std::vector<f32> L, R;
            spRender(*s, spV3Note(), 60000, kBlock, L, R, 120.0);
            // Step 0 is 1500 samples long; the pulse must recur every 24000.
            const f32 p0 = spV3Peak(L, 200, 512);
            const f32 p1 = spV3Peak(L, 24200, 512);
            const f32 gap = spV3Peak(L, 12000, 512);
            CHECK(p0 > 0.05f && p1 > 0.05f && gap == 0.f,
                  "sync 1/4 runs ALL SIXTEEN steps inside one quarter note: the pulse "
                  "is at 0 and 24000 and the middle is silent (%.4f / %.4f / %.9f)",
                  (double)p0, (double)p1, (double)gap);
        }
    }

    // --- 6. determinism: the grid is state and the step index is a projection
    //        of the phase, so nothing here can carry between renders.
    {
        auto a = build("08c4f6a20d9315be", 250, 1.f);
        auto b2 = build("08c4f6a20d9315be", 250, 1.f);
        std::vector<f32> aL, aR, bL, bR;
        if (a && b2) {
            spRender(*a, spV3Note(), kFrames, kBlock, aL, aR);
            spRender(*b2, spV3Note(), kFrames, kBlock, bL, bR);
            CHECK(spV2MaxDiff(aL, bL) == 0.f,
                  "two instances with the same grid and smooth render bit-identically");
        }
    }
}

// ---------------------------------------------------------------------------

static void testSpectraV3OneShot(PluginRegistry& reg) {
    banner("Spectra v3: One-shot LFOs (ids 60/61/100) — an LFO that is an envelope");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;

    const int kStep = 3000;                    // 1 Hz, 16 steps, 48 kHz

    // A rising 16-step ramp on LFO1, driving A Level. In Loop it sweeps and
    // wraps; in One-shot it sweeps once and holds.
    auto build = [&](int mode, int noteFrame) {
        auto s = spV3Probe(reg, *d);
        if (!s) return s;
        s->setParam(spIdx(*s, "LFO Shape"), 5.f);
        s->setParam(spIdx(*s, "LFO Sync"), 0.f);
        s->setParam(spIdx(*s, "LFO Rate"), 1.f);
        s->setParam(100, (f32)mode);                       // L1 Mode
        spV3Slot(*s, 0, 1, 5, 1.f);
        s->setStateString((std::string("nxspc1;lfo1=") + kSpV3RampGrid).c_str());
        (void)noteFrame;
        return s;
    };

    // --- 1. Loop mode is v2 verbatim: setting id 100 to 0 explicitly renders
    //        identically to never having touched it. That is the whole
    //        bit-identity claim for this feature, in one check.
    {
        auto a = build(0, 0);
        auto b2 = spV3Probe(reg, *d);
        if (a && b2) {
            b2->setParam(spIdx(*b2, "LFO Shape"), 5.f);
            b2->setParam(spIdx(*b2, "LFO Rate"), 1.f);
            spV3Slot(*b2, 0, 1, 5, 1.f);
            b2->setStateString((std::string("nxspc1;lfo1=") + kSpV3RampGrid).c_str());
            std::vector<f32> aL, aR, bL, bR;
            spRender(*a, spV3Note(), 60000, kBlock, aL, aR);
            spRender(*b2, spV3Note(), 60000, kBlock, bL, bR);
            CHECK(spV2MaxDiff(aL, bL) == 0.f,
                  "L1 Mode = Loop is bit-identical to a build that never had the "
                  "parameter");
        }
    }

    // --- 2. one-shot RAMPS ONCE AND CLAMPS; loop wraps.
    {
        auto loop = build(0, 0);
        auto once = build(1, 0);
        std::vector<f32> lL, lR, oL, oR;
        if (!loop || !once) return;
        spRender(*loop, spV3Note(), 100000, kBlock, lL, lR);
        spRender(*once, spV3Note(), 100000, kBlock, oL, oR);

        f32 mx = 0.f;
        f32 pr[16];
        for (int k = 0; k < 16; ++k) {
            pr[k] = spV3Peak(oL, k * kStep + kStep / 2 - 128, 256);
            mx = std::fmax(mx, pr[k]);
        }
        CHECK(mx > 0.05f, "the one-shot ramp is not silent (max %.4f)", (double)mx);
        int bad = 0;
        for (int k = 0; k < 16; ++k)
            if (std::fabs((f64)pr[k] / mx - (f64)k / 15.0) > 0.04) ++bad;
        CHECK(bad == 0, "the one-shot climbs its sixteen steps exactly once (%d off)", bad);

        // Past the end of the cycle: one-shot HOLDS phase 1.0 (step 15),
        // loop has wrapped back to step 0, which this grid draws at zero.
        const f32 oHold = spV3Peak(oL, 60000, 512);
        const f32 lWrap = spV3Peak(lL, 48200, 512);
        CHECK(std::fabs((f64)oHold / mx - 1.0) < 0.04,
              "one-shot CLAMPS at 1.0 and holds it for as long as the voice lives "
              "(%.4f of full scale, 12500 samples past the end)", (double)(oHold / mx));
        CHECK(lWrap == 0.f,
              "...while Loop has wrapped back to step 0 (%.9f)", (double)lWrap);
        const f32 oLater = spV3Peak(oL, 95000, 512);
        CHECK(std::fabs((f64)oLater - (f64)oHold) < 0.02 * mx,
              "and it is still holding a second later");
    }

    // --- 3. THE ORIGIN IS THE NOTE, NOT THE CLOCK. The same ramp measured
    //        relative to a note-on at 17000 is the same ramp.
    {
        auto once = build(1, 0);
        std::vector<f32> L, R;
        if (once) {
            spRender(*once, spV3Note(17000), 100000, kBlock, L, R);
            f32 mx = 0.f, pr[16];
            for (int k = 0; k < 16; ++k) {
                pr[k] = spV3Peak(L, 17000 + k * kStep + kStep / 2 - 128, 256);
                mx = std::fmax(mx, pr[k]);
            }
            int bad = 0;
            for (int k = 0; k < 16; ++k)
                if (std::fabs((f64)pr[k] / mx - (f64)k / 15.0) > 0.05) ++bad;
            CHECK(bad == 0 && mx > 0.05f,
                  "a one-shot's phase is 0 at the NOTE-ON's stamped sample, whatever the "
                  "transport is doing (%d steps off)", bad);
            CHECK(spV3Peak(L, 0, 16800) == 0.f,
                  "...and nothing sounds before the note");
        }
    }

    // --- 4. SYNC SETS THE SPEED, NOT THE ALIGNMENT. A one-shot locked to the
    //        bar line would not be an envelope.
    {
        auto s = spV3Probe(reg, *d);
        if (s) {
            s->setParam(spIdx(*s, "LFO Shape"), 5.f);
            s->setParam(spIdx(*s, "LFO Sync"), 3.f);          // one bar = 2 s @ 120
            s->setParam(100, 1.f);
            spV3Slot(*s, 0, 1, 5, 1.f);
            s->setStateString("nxspc1;lfo1=0000000000000000");
            s->setStateString((std::string("nxspc1;lfo1=") + kSpV3RampGrid).c_str());
            std::vector<f32> L, R;
            // A note a quarter of the way into the bar: if sync aligned the
            // phase, the ramp would start a quarter of the way up.
            spRender(*s, spV3Note(24000), 160000, kBlock, L, R, 120.0);
            const f32 atOn   = spV3Peak(L, 24100, 400);
            const f32 atHalf = spV3Peak(L, 24000 + 48000, 512);
            const f32 mx     = spV3Peak(L, 24000, 136000);
            CHECK(mx > 0.05f, "the synced one-shot is not silent (%.4f)", (double)mx);
            CHECK(atOn / mx < 0.10f,
                  "a synced one-shot starts at phase 0 AT THE NOTE, not at the bar's "
                  "phase (%.4f of full scale)", (double)(atOn / mx));
            CHECK(std::fabs((f64)atHalf / mx - 8.0 / 15.0) < 0.08,
                  "...and its SPEED is the division: half a bar in, it is half way up "
                  "(%.4f, want %.4f)", (double)(atHalf / mx), 8.0 / 15.0);
        }
    }

    // --- 5. IT RETRIGGERS EXACTLY WHEN ENV1 DOES. Mono retriggers on every
    //        note-on; a Legato overlap does not; a note-off fallback does not.
    {
        auto mk = [&](int voiceMode) {
            auto s = build(1, 0);
            if (s) {
                s->setParam(spIdx(*s, "Voice Mode"), (f32)voiceMode);
                s->setParam(spIdx(*s, "Glide"), 0.f);
            }
            return s;
        };
        // Note A at 0, note B at 24000 while A is still held (an overlap).
        const std::vector<SpEvent> ev = {
            {     0, 0x90, 84, 127 },
            { 24000, 0x90, 84, 100 },        // same note: Mono retrigger / Legato overlap
        };
        auto mono = mk(1);
        auto lega = mk(2);
        std::vector<f32> mL, mR, lL, lR;
        if (mono && lega) {
            spRender(*mono, ev, 100000, kBlock, mL, mR);
            spRender(*lega, ev, 100000, kBlock, lL, lR);
            const f32 mMax = spV3Peak(mL, 0, 100000);
            const f32 lMax = spV3Peak(lL, 0, 100000);
            // Just after the second note-on: a retrigger is back at step 0
            // (level 0), a non-retrigger is still 8/15 of the way up.
            const f32 mAfter = spV3Peak(mL, 24200, 400);
            const f32 lAfter = spV3Peak(lL, 24200, 400);
            CHECK(mMax > 0.05f && lMax > 0.05f, "both mono renders sound");
            CHECK(mAfter / mMax < 0.10f,
                  "Mono: every note-on retriggers the one-shot, exactly as it retriggers "
                  "ENV1 (%.4f of full scale just after)", (double)(mAfter / mMax));
            CHECK(std::fabs((f64)lAfter / lMax - 8.0 / 15.0) < 0.08,
                  "Legato: an OVERLAPPED note-on does not retrigger it — a fallback and "
                  "an overlap are not note-ons (%.4f, want %.4f)",
                  (double)(lAfter / lMax), 8.0 / 15.0);
        }

        // The note-off fallback: hold two notes, release the newer. The
        // surviving voice glides to the older note and keeps running.
        auto leg2 = mk(2);
        if (leg2) {
            const std::vector<SpEvent> ev2 = {
                {     0, 0x90, 84, 127 },
                {  6000, 0x90, 79, 110 },
                { 24000, 0x80, 79,   0 },      // fallback to 84, NOT a note-on
            };
            std::vector<f32> L, R;
            spRender(*leg2, ev2, 100000, kBlock, L, R);
            const f32 mx = spV3Peak(L, 0, 100000);
            const f32 after = spV3Peak(L, 24400, 400);
            CHECK(mx > 0.05f && std::fabs((f64)after / mx - 8.0 / 15.0) < 0.10,
                  "a note-off FALLBACK does not retrigger it either (%.4f, want %.4f)",
                  (double)(after / mx), 8.0 / 15.0);
        }
    }

    // --- 6. S&H (shape 4) under one-shot: ONE draw, at note-on, held for the
    //        life of the voice, and the value is the note's identity hash.
    {
        auto mk = [&](u8 note, int frame) {
            auto s = spV3Probe(reg, *d);
            if (!s) return s;
            s->setParam(spIdx(*s, "LFO Shape"), 4.f);        // S&H
            s->setParam(spIdx(*s, "LFO Rate"), 3.f);
            s->setParam(100, 1.f);                           // one-shot
            s->setParam(spIdx(*s, "A Level"), 0.5f);         // bipolar source
            spV3Slot(*s, 0, 1, 5, 0.5f);
            (void)note; (void)frame;
            return s;
        };
        f32 val[4] = {};
        bool ok = true;
        const u8 notes[4] = { 84, 85, 84, 86 };
        const int frames[4] = { 0, 0, 5000, 0 };
        for (int i = 0; i < 4; ++i) {
            auto s = mk(notes[i], frames[i]);
            if (!s) { ok = false; break; }
            std::vector<f32> L, R;
            spRender(*s, spV3Note(frames[i], notes[i]), 40000, kBlock, L, R);
            const int from = frames[i] + 2000;
            const f32 a = spV3Peak(L, from, 512);
            const f32 b3 = spV3Peak(L, from + 20000, 512);
            if (std::fabs((f64)a - (f64)b3) > 0.03 * std::fmax(a, 1e-6f)) ok = false;
            val[i] = a;
        }
        CHECK(ok, "a one-shot S&H draws ONCE at note-on and holds — it wraps exactly "
                  "once, at phase 0");
        CHECK(!(val[0] == val[1] && val[1] == val[2] && val[2] == val[3]),
              "...and the draw is the NOTE'S identity (channel, note, stamped sample), "
              "so different notes draw differently (%.4f %.4f %.4f %.4f)",
              (double)val[0], (double)val[1], (double)val[2], (double)val[3]);

        // The same note at the same sample, twice: identical, because there is
        // no RNG state carried between notes.
        auto a = mk(84, 0), b4 = mk(84, 0);
        if (a && b4) {
            std::vector<f32> aL, aR, bL, bR;
            spRender(*a, spV3Note(0, 84), 40000, kBlock, aL, aR);
            spRender(*b4, spV3Note(0, 84), 40000, kBlock, bL, bR);
            CHECK(spV2MaxDiff(aL, bL) == 0.f,
                  "the same note at the same sample draws the same value in two "
                  "instances — no RNG state, no clock");
        }
    }

    // --- 7. all three LFOs have a mode, and L2/L3's live on the reserved ids
    //        the block that owns them left behind.
    {
        bool all = true;
        const int modeId[3] = { 100, 60, 61 };
        const char* shapeN[3] = { "LFO Shape", "L2 Shape", "L3 Shape" };
        const char* rateN[3]  = { "LFO Rate", "L2 Rate", "L3 Rate" };
        const int srcOf[3]    = { 1, 2, 3 };
        for (int j = 0; j < 3; ++j) {
            auto s = spV3Probe(reg, *d);
            if (!s) { all = false; break; }
            s->setParam(spIdx(*s, shapeN[j]), 5.f);
            s->setParam(spIdx(*s, rateN[j]), 1.f);
            s->setParam(modeId[j], 1.f);
            spV3Slot(*s, 0, srcOf[j], 5, 1.f);
            char st[64];
            std::snprintf(st, sizeof st, "nxspc1;lfo%d=%s", j + 1, kSpV3RampGrid);
            s->setStateString(st);
            std::vector<f32> L, R;
            spRender(*s, spV3Note(), 100000, kBlock, L, R);
            const f32 mx = spV3Peak(L, 0, 100000);
            const f32 hold = spV3Peak(L, 80000, 512);
            if (!(mx > 0.05f && std::fabs((f64)hold / mx - 1.0) < 0.05)) all = false;
        }
        CHECK(all, "LFO1 (id 100), LFO2 (id 60) and LFO3 (id 61) each ramp once and hold "
                   "in One-shot, each through its own drawn grid");
    }
}

// ---------------------------------------------------------------------------

static void testSpectraV3Midi(PluginRegistry& reg) {
    banner("Spectra v3: MIDI as a modulation source (wheel, bend, one learned CC)");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;

    // --- 1. the mod wheel, through a slot.
    {
        auto s = spV3Probe(reg, *d);
        if (s) {
            spV3Slot(*s, 0, 14 /*Mod Wheel*/, 5 /*A Level*/, 1.f);
            const std::vector<SpEvent> ev = {
                {     0, 0x90, 84, 127 },
                { 12000, 0xB0,  1, 127 },
                { 24000, 0xB0,  1,  64 },
                { 36000, 0xB0,  1,   0 },
            };
            std::vector<f32> L, R;
            spRender(*s, ev, 48000, kBlock, L, R);
            const f32 before = spV3Peak(L, 4000, 512);
            const f32 full   = spV3Peak(L, 16000, 512);
            const f32 half   = spV3Peak(L, 28000, 512);
            const f32 off    = spV3Peak(L, 40000, 512);
            CHECK(before == 0.f, "the wheel is 0 after prepare() (%.9f)", (double)before);
            // Absolute, not proportional: a wheel scaled by anything but 1/127
            // would keep every ratio below and still be wrong.
            auto cal = spV3Probe(reg, *d);
            f32 unit = 0.f;
            if (cal) {
                cal->setParam(spIdx(*cal, "A Level"), 1.f);
                std::vector<f32> cL, cR;
                spRender(*cal, spV3Note(), 8000, kBlock, cL, cR);
                unit = spV3Peak(cL, 4000, 512);
            }
            CHECK(unit > 0.05f && std::fabs((f64)full / unit - 1.0) < 0.02,
                  "CC 1 at 127 opens the destination FULLY: w = 127/127 = 1 exactly "
                  "(%.4f of an unmodulated A Level of 1)", (double)(full / unit));
            CHECK(std::fabs((f64)half / full - 64.0 / 127.0) < 0.04,
                  "...and w = data2/127 exactly (%.4f at 64, want %.4f)",
                  (double)(half / full), 64.0 / 127.0);
            CHECK(off == 0.f, "CC 1 at 0 closes it again (%.9f)", (double)off);
        }
    }

    // --- 2. a wheel NO SLOT READS changes nothing. That is the v2 gate for
    //        every new MIDI source but bend, in one measurement.
    {
        auto a = spV3Probe(reg, *d);
        auto b2 = spV3Probe(reg, *d);
        if (a && b2) {
            for (auto* q : { a.get(), b2.get() }) {
                spBusyPatch(*q);
                q->setParam(spIdx(*q, "Master"), 0.6f);
            }
            std::vector<SpEvent> plain = spScript();
            std::vector<SpEvent> noisy = plain;
            for (int f = 500; f < 11000; f += 137)
                noisy.push_back({ f, 0xB0, 1, (u8)((f / 137) & 0x7F) });
            for (int f = 700; f < 11000; f += 211)
                noisy.push_back({ f, 0xB0, 74, (u8)((f / 211) & 0x7F) });
            std::sort(noisy.begin(), noisy.end(),
                      [](const SpEvent& x, const SpEvent& y) { return x.frame < y.frame; });
            std::vector<f32> aL, aR, bL, bR;
            spRender(*a, plain, 14000, kBlock, aL, aR, 120.0);
            spRender(*b2, noisy, 14000, kBlock, bL, bR, 120.0);
            CHECK(spV3Peak(aL, 0, 14000) > 0.05f, "the comparison render is not silent");
            CHECK(spV2MaxDiff(aL, bL) == 0.f && spV2MaxDiff(aR, bR) == 0.f,
                  "a stream full of CC 1 and CC 74 that no slot reads renders "
                  "BIT-IDENTICALLY to one with none — every new source but bend is "
                  "inert by construction");
        }
    }

    // --- 3. pitch bend, hardwired through Bend Range (id 99). A wheel at full
    //        travel down is exactly -1, so the pitch moves by exactly the range.
    {
        auto mk = [&](f32 range) {
            auto s = spV2Base(reg, *d);
            if (s) {
                s->setParam(spIdx(*s, "A Table"), 0.f);
                s->setParam(spIdx(*s, "A Unison"), 1.f);
                s->setParam(99, range);
            }
            return s;
        };
        const std::vector<SpEvent> ev = {
            { 0, 0xE0, 0, 0 },              // v14 = 0 -> b = -1
            { 1, 0x90, 69, 127 },           // A4 = 440 Hz
        };
        auto s = mk(2.f);
        std::vector<f32> L, R;
        if (s) {
            spRender(*s, ev, 24000, kBlock, L, R);
            const f64 at440 = tBinMag(L, 8000, 8192, 440.0);
            const f64 at392 = tBinMag(L, 8000, 8192, 440.0 * std::pow(2.0, -2.0 / 12.0));
            CHECK(at392 > 4.0 * at440,
                  "Bend Range 2 with the wheel full down moves A4 to G4: %.5f at 392 Hz "
                  "against %.5f at 440", at392, at440);
        }
        // Range 0 is inert and bit-identical to v2.
        auto z0 = mk(0.f);
        auto z1 = mk(0.f);
        if (z0 && z1) {
            std::vector<f32> zL, zR, nL, nR;
            spRender(*z0, ev, 24000, kBlock, zL, zR);
            spRender(*z1, { { 1, 0x90, 69, 127 } }, 24000, kBlock, nL, nR);
            CHECK(spV2MaxDiff(zL, nL) == 0.f,
                  "Bend Range 0 is INERT: the same stream with and without bend bytes "
                  "renders bit-identically");
        }
        // ...and 24 st is the top of the range.
        auto wide = mk(24.f);
        if (wide) {
            std::vector<f32> wL, wR;
            spRender(*wide, ev, 24000, kBlock, wL, wR);
            const f64 at110 = tBinMag(wL, 8000, 8192, 110.0);
            const f64 at440b = tBinMag(wL, 8000, 8192, 440.0);
            // 440 is the fourth harmonic of 110, so a saw down there still has
            // real energy at the old fundamental; the test is which one leads.
            CHECK(at110 > 2.5 * at440b,
                  "Bend Range 24 drops A4 two octaves to A2 (%.5f at 110 Hz against "
                  "%.5f at 440)", at110, at440b);
        }
    }

    // --- 4. the wheel reaches the pitch BEFORE Coarse/Fine, so the sub follows
    //        it exactly as it follows vibrato.
    {
        auto s = spV2Base(reg, *d);
        if (s) {
            s->setParam(spIdx(*s, "A Level"), 0.f);
            s->setParam(spIdx(*s, "Sub"), 1.f);
            s->setParam(spIdx(*s, "A Coarse"), 12.f);     // must NOT move the sub
            s->setParam(99, 2.f);
            const std::vector<SpEvent> ev = { { 0, 0xE0, 0, 0 }, { 1, 0x90, 69, 127 } };
            std::vector<f32> L, R;
            spRender(*s, ev, 24000, kBlock, L, R);
            const f64 at220 = tBinMag(L, 8000, 8192, 220.0);
            const f64 at196 = tBinMag(L, 8000, 8192, 220.0 * std::pow(2.0, -2.0 / 12.0));
            CHECK(at196 > 4.0 * at220,
                  "the sub follows the wheel (%.5f at 196 Hz against %.5f at 220) — the "
                  "bend lands on the POST-GLIDE pitch, before per-oscillator tuning",
                  at196, at220);
        }
    }

    // --- 5. bend as a matrix source (15), live at the same time as the
    //        hardwired action. Setting Bend Range to 0 disables only the first.
    {
        auto s = spV3Probe(reg, *d);
        if (s) {
            s->setParam(99, 0.f);
            s->setParam(spIdx(*s, "A Level"), 0.5f);
            spV3Slot(*s, 0, 15 /*Pitch Bend*/, 5 /*A Level*/, 0.5f);
            const std::vector<SpEvent> ev = {
                {     0, 0x90, 84, 127 },
                { 12000, 0xE0, 0, 0 },                    // -1
                { 24000, 0xE0, 0x7F, 0x7F },              // +0.999878
            };
            std::vector<f32> L, R;
            spRender(*s, ev, 40000, kBlock, L, R);
            const f32 centre = spV3Peak(L, 6000, 512);
            const f32 down   = spV3Peak(L, 18000, 512);
            const f32 up     = spV3Peak(L, 34000, 512);
            CHECK(centre > 0.05f, "bend reads 0 = CENTRE after prepare (%.4f)", (double)centre);
            CHECK(down == 0.f, "full down (-1) closes A Level to 0 (%.9f)", (double)down);
            CHECK(std::fabs((f64)up / centre - 2.0) < 0.06,
                  "full up (+0.999878) doubles it — the asymmetry is MIDI's and is not "
                  "rescaled, so the centre stays exactly 0 (%.4f)", (double)(up / centre));
        }
    }

    // --- 6. the learned CC. There is exactly ONE learn slot: every slot whose
    //        source is 16 reads the same controller.
    {
        auto s = spV3Probe(reg, *d);
        if (s) {
            spV3Slot(*s, 0, 16 /*MIDI CC*/, 5, 1.f);
            const std::vector<SpEvent> ev = {
                {     0, 0x90, 84, 127 },
                { 12000, 0xB0, 74, 127 },
                { 24000, 0xB0, 75,  40 },      // a DIFFERENT value on a
                                               // controller nobody learned
            };
            std::vector<f32> L, R;
            spRender(*s, ev, 36000, kBlock, L, R);
            CHECK(spV3Peak(L, 0, 36000) == 0.f,
                  "with no CC learned, source 16 is constant 0 — inert like every other "
                  "v3 default");

            auto q = spV3Probe(reg, *d);
            if (q) {
                spV3Slot(*q, 0, 16, 5, 1.f);
                q->setStateString("nxspc1;cc=74");
                std::vector<f32> qL, qR;
                spRender(*q, ev, 36000, kBlock, qL, qR);
                const f32 before = spV3Peak(qL, 4000, 512);
                const f32 after  = spV3Peak(qL, 16000, 512);
                const f32 other  = spV3Peak(qL, 30000, 512);
                CHECK(before == 0.f && after > 0.05f,
                      "the learned controller (cc=74, from the STATE) drives source 16 "
                      "(%.9f before, %.4f after)", (double)before, (double)after);
                CHECK(std::fabs((f64)other - (f64)after) < 0.02 * (f64)after,
                      "...and CC 75 does not touch it — one learn slot, not one per slot");
            }
        }
    }

    // --- 7. CC 120 and 123 keep the meanings v1 gave them and do NOT feed the
    //        three new sources, even when 123 is what somebody learned.
    {
        auto s = spV3Probe(reg, *d);
        if (s) {
            spV3Slot(*s, 0, 16, 5, 1.f);
            s->setStateString("nxspc1;cc=123");
            const std::vector<SpEvent> ev = {
                {     0, 0x90, 84, 127 },
                { 12000, 0xB0, 123, 127 },
            };
            std::vector<f32> L, R;
            spRender(*s, ev, 30000, kBlock, L, R);
            CHECK(spV3Peak(L, 0, 30000) == 0.f,
                  "a learned CC 123 is still ONLY All Notes Off: it does not feed "
                  "source 16 (%.9f)", (double)spV3Peak(L, 0, 30000));
        }
    }

    // --- 8. the queue's overflow rule. Past the cap a controller FOLDS into
    //        one slot and lands at the block's last sample, so no flood of any
    //        length can lose its current value.
    {
        auto s = spV3Probe(reg, *d);
        if (s) {
            spV3Slot(*s, 0, 14 /*wheel*/, 5, 1.f);
            std::vector<SpEvent> ev;
            ev.push_back({ 0, 0x90, 84, 127 });
            // One block, 200 note-ons of a note that is instantly released,
            // then the wheel. The queue is long past full when it arrives.
            for (int i = 0; i < 200; ++i) ev.push_back({ 512 + i % 200, 0x90, 40, 1 });
            ev.push_back({ 700, 0xB0, 1, 127 });
            std::sort(ev.begin(), ev.end(),
                      [](const SpEvent& x, const SpEvent& y) { return x.frame < y.frame; });
            std::vector<f32> L, R;
            spRender(*s, ev, 30000, 1024, L, R);
            CHECK(spV3Peak(L, 20000, 512) > 0.05f,
                  "a wheel value that overflowed a full queue still lands, at the "
                  "block's last sample (%.4f)", (double)spV3Peak(L, 20000, 512));
        }
    }
}

// ---------------------------------------------------------------------------

static void testSpectraV3Curves(PluginRegistry& reg) {
    banner("Spectra v3: the per-slot matrix response curves (ids 101..108)");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;

    // Pitch bend is the source, because it is the one source the suite can set
    // to an EXACT value: v14/8192 - 1 with v14 chosen by hand. Bend Range is 0
    // throughout, so the wheel moves the curve under test and nothing else.
    auto probe = [&](int curve, int v14, f32 base, f32 amt) {
        auto s = spV3Probe(reg, *d);
        if (!s) return 0.f;
        s->setParam(99, 0.f);                              // bend does not move pitch
        s->setParam(spIdx(*s, "A Level"), base);
        spV3Slot(*s, 0, 15 /*Pitch Bend*/, 5 /*A Level*/, amt, curve);
        const std::vector<SpEvent> ev = {
            { 0, 0xE0, (u8)(v14 & 0x7F), (u8)((v14 >> 7) & 0x7F) },
            { 1, 0x90, 84, 127 },
        };
        std::vector<f32> L, R;
        spRender(*s, ev, 20000, kBlock, L, R);
        return spV3Peak(L, 8000, 1024);
    };

    // The scale: A Level 1.0 with no modulation, which is what f(x) = 1 reads.
    const f32 unit = probe(0, 8192, 1.f, 0.f);
    CHECK(unit > 0.05f, "the curve probe is calibrated (full scale reads %.4f)", (double)unit);
    if (!(unit > 0.f)) return;

    // f(0) = 0 and f(1) = 1 for all three: a curve can never make an idle
    // source contribute and can never change a full-scale source's reach.
    {
        bool zero = true, one = true;
        for (int c = 0; c < 3; ++c) {
            if (probe(c, 8192, 0.f, 1.f) != 0.f) zero = false;
            if (std::fabs((f64)probe(c, 16383, 0.f, 1.f) / unit - 1.0) > 0.01) one = false;
        }
        CHECK(zero, "f(0) = 0 EXACTLY for Linear, Exp and S-curve — an idle source "
                    "cannot be made to contribute");
        CHECK(one, "f(1) = 1 for all three — a curve cannot change a full-scale "
                   "source's reach");
    }

    // The formulas, at |u| = 0.5 and |u| = 0.25. Base 0.5 and amount 0.5, so
    // the reading is 0.5 + 0.5 * sign(u) * f(|u|).
    {
        struct Pt { int v14; f64 u; };
        const Pt pts[] = { { 12288, 0.5 }, { 10240, 0.25 }, { 4096, -0.5 }, { 6144, -0.25 } };
        const char* nm[3] = { "Linear f(x)=x", "Exp f(x)=x*x", "S-curve f(x)=x*x*(3-2x)" };
        int bad = 0;
        f64 worst = 0.0;
        for (int c = 0; c < 3; ++c) {
            for (const Pt& pt : pts) {
                const f64 x = std::fabs(pt.u);
                const f64 f = c == 0 ? x : (c == 1 ? x * x : x * x * (3.0 - 2.0 * x));
                const f64 want = 0.5 + 0.5 * (pt.u < 0 ? -f : f);
                const f64 got  = (f64)probe(c, pt.v14, 0.5f, 0.5f) / unit;
                worst = std::fmax(worst, std::fabs(got - want));
                if (std::fabs(got - want) > 0.02) {
                    CHECK(false, "%s at u = %+.2f reads %.4f, the contract says %.4f",
                          nm[c], pt.u, got, want);
                    ++bad;
                }
            }
        }
        CHECK(bad == 0, "all three curves match the contract's formulas at u = ±0.25 and "
                        "±0.5 (worst error %.4f)", worst);
    }

    // The curve is applied SYMMETRICALLY about zero, so a bipolar source stays
    // bipolar and zero stays zero.
    {
        bool sym = true, zero = true;
        for (int c = 1; c < 3; ++c) {
            const f64 up   = (f64)probe(c, 12288, 0.5f, 0.5f) / unit - 0.5;
            const f64 down = (f64)probe(c, 4096,  0.5f, 0.5f) / unit - 0.5;
            if (std::fabs(up + down) > 0.02) sym = false;
            if (std::fabs((f64)probe(c, 8192, 0.5f, 0.5f) / unit - 0.5) > 0.01) zero = false;
        }
        CHECK(sym, "Exp and S-curve are symmetric about zero: sign(u)*f(|u|), so a "
                   "bipolar source stays bipolar");
        CHECK(zero, "...and zero stays zero for every curve");
    }

    // LINEAR IS A SELECTED BRANCH, not a multiply by one. Two renders, one with
    // the curve parameter never touched and one set to 0 explicitly.
    {
        auto mk = [&](bool setCurve) {
            auto s = spV3Probe(reg, *d);
            if (!s) return s;
            spBusyPatch(*s);
            s->setParam(spIdx(*s, "Master"), 0.6f);
            for (int k = 0; k < 8; ++k) {
                s->setParam(68 + 3 * k, (f32)(1 + k % 13));
                s->setParam(69 + 3 * k, (f32)(1 + k * 2));
                s->setParam(70 + 3 * k, 0.4f);
                if (setCurve) s->setParam(101 + k, 0.f);
            }
            return s;
        };
        auto a = mk(false);
        auto b2 = mk(true);
        if (a && b2) {
            std::vector<f32> aL, aR, bL, bR;
            spRender(*a, spScript(), 14000, kBlock, aL, aR, 120.0);
            spRender(*b2, spScript(), 14000, kBlock, bL, bR, 120.0);
            CHECK(spV3Peak(aL, 0, 14000) > 0.05f, "the eight-slot render is not silent");
            CHECK(spV2MaxDiff(aL, bL) == 0.f && spV2MaxDiff(aR, bR) == 0.f,
                  "eight slots at Linear render bit-identically to a build with no curve "
                  "parameter at all");
        }
    }

    // Per SLOT, not per instrument: slot 0 curved and slot 1 linear on the same
    // source must differ.
    {
        auto s = spV3Probe(reg, *d);
        if (s) {
            s->setParam(99, 0.f);
            s->setParam(spIdx(*s, "A Level"), 0.f);
            spV3Slot(*s, 0, 15, 5, 0.25f, 1);        // Exp
            spV3Slot(*s, 1, 15, 5, 0.25f, 0);        // Linear
            const std::vector<SpEvent> ev = {
                { 0, 0xE0, (u8)(12288 & 0x7F), (u8)(12288 >> 7) },
                { 1, 0x90, 84, 127 },
            };
            std::vector<f32> L, R;
            spRender(*s, ev, 20000, kBlock, L, R);
            const f64 got = (f64)spV3Peak(L, 8000, 1024) / unit;
            const f64 want = 0.25 * 0.25 + 0.25 * 0.5;      // Exp + Linear
            CHECK(std::fabs(got - want) < 0.02,
                  "two slots on one source with different curves sum independently "
                  "(%.4f, want %.4f) — the curve is the SLOT's response", got, want);
        }
    }
}

// ---------------------------------------------------------------------------

static void testSpectraV3Refusal(PluginRegistry& reg) {
    banner("Spectra v3: an unresolvable custom slot (the refusal contract)");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;

    // A patch on slot 8 that nothing resolves must render FACTORY TABLE 0 —
    // "silence would be a worse lie than the wrong table" — and must render it
    // identically to a patch that asked for table 0 in the first place.
    auto mk = [&](int table, const char* state) {
        auto s = spV2Base(reg, *d);
        if (!s) return s;
        s->setParam(spIdx(*s, "A Unison"), 1.f);
        s->setParam(spIdx(*s, "A Position"), 0.4f);
        s->setParam(0, (f32)table);
        if (state) s->setStateString(state);
        return s;
    };
    auto refused = mk(8, "nxspc1;wtA=0123456789abcdef;wtpathA=/tmp/nx-no-such-table.wav");
    auto factory = mk(0, nullptr);
    if (refused && factory) {
        std::vector<f32> rL, rR, fL, fR;
        spRender(*refused, spV3Note(), 20000, kBlock, rL, rR);
        spRender(*factory, spV3Note(), 20000, kBlock, fL, fR);
        CHECK(spV3Peak(rL, 0, 20000) > 0.05f,
              "an unresolvable slot 8 still SOUNDS (%.4f)", (double)spV3Peak(rL, 0, 20000));
        CHECK(spV2MaxDiff(rL, fL) == 0.f,
              "...and it sounds exactly like factory table 0");
        CHECK(refused->getParam(0) == 8.f,
              "the parameter keeps its value — the set's intent is not edited by the "
              "machine that could not honour it");
        CHECK(refused->stateString() ==
              "nxspc1;wtA=0123456789abcdef;wtpathA=/tmp/nx-no-such-table.wav",
              "and the records are re-emitted verbatim, so a save here does not lose "
              "the file's name\n        got: %s", refused->stateString().c_str());
    }
}

// ---------------------------------------------------------------------------

// The v3 kitchen sink: v2's busy patch plus a drawn grid, a one-shot LFO, all
// three MIDI sources and three different response curves, so that nothing the
// revision added is switched off when the block size changes.
static void spV3BusyPatch(PluginInstance& s) {
    spV2BusyPatch(s);
    s.setParam(spIdx(s, "LFO Shape"), 5.f);        // LFO1 drawn
    s.setParam(spIdx(s, "L2 Shape"), 5.f);         // LFO2 drawn
    s.setParam(100, 1.f);                          // L1 Mode = One-shot
    s.setParam(61, 1.f);                           // L3 Mode = One-shot
    s.setParam(99, 7.f);                           // Bend Range
    spV3Slot(s, 0, 2  /*LFO2*/,      1  /*A Pos*/,     0.5f, 2);
    spV3Slot(s, 1, 3  /*LFO3*/,      11 /*Cutoff*/,    0.4f, 1);
    spV3Slot(s, 2, 14 /*Mod Wheel*/, 8  /*B Pitch*/,   0.3f, 2);
    spV3Slot(s, 3, 13 /*Random*/,    16 /*Pan*/,       0.8f, 0);
    spV3Slot(s, 4, 15 /*Pitch Bend*/,13 /*Drive*/,     0.5f, 1);
    spV3Slot(s, 5, 16 /*MIDI CC*/,   12 /*Resonance*/, 0.3f, 2);
    spV3Slot(s, 6, 1  /*LFO1*/,      10 /*Noise Lvl*/, 0.5f, 1);
    spV3Slot(s, 7, 8  /*Aftertouch*/,17 /*LFO1 Rate*/, 0.4f, 2);
    s.setStateString("nxspc1;lfo1=0369cf0369cf0369;lfo2=08c4f6a20d9315be;"
                     "smooth1=250;smooth2=40;cc=74");
}

static void testSpectraV3Determinism(PluginRegistry& reg) {
    banner("Spectra v3: block-size invariance with everything v3 added engaged");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;

    const int kFrames = 14000;
    // The v2 script plus mid-block controller traffic: wheel, learned CC,
    // pitch bend and channel pressure, none of them on a block boundary at any
    // of the sizes below. If any of the four were applied when midi() was
    // called rather than at its stamped sample, this cannot pass.
    std::vector<SpEvent> ev = spScript();
    ev.push_back({ 3000, 0xD0, 96, 0 });
    ev.push_back({ 9000, 0xD0, 20, 0 });
    for (int f = 137; f < kFrames; f += 379)
        ev.push_back({ f, 0xB0, 1, (u8)((f / 379 * 13) & 0x7F) });
    for (int f = 251; f < kFrames; f += 431)
        ev.push_back({ f, 0xB0, 74, (u8)((f / 431 * 7) & 0x7F) });
    for (int f = 89; f < kFrames; f += 293) {
        const int v14 = (f * 37) & 0x3FFF;
        ev.push_back({ f, 0xE0, (u8)(v14 & 0x7F), (u8)((v14 >> 7) & 0x7F) });
    }
    std::sort(ev.begin(), ev.end(),
              [](const SpEvent& a, const SpEvent& b) { return a.frame < b.frame; });

    for (int mode = 0; mode < 2; ++mode) {
        auto build = [&]() {
            auto s = reg.instantiate(*d, kSR, kBlock);
            if (s) {
                spV3BusyPatch(*s);
                s->setParam(spIdx(*s, "Voice Mode"), mode ? 2.f : 0.f);
                s->setParam(spIdx(*s, "Glide"), mode ? 900.f : 0.f);
            }
            return s;
        };
        auto ref = build();
        if (!ref) return;
        std::vector<f32> refL, refR, altL, altR;
        spRender(*ref, ev, kFrames, kBlock, refL, refR, 120.0);
        CHECK(spV3Peak(refL, 0, kFrames) > 0.02f,
              "the v3 busy render (%s) is not silent (peak %.4f)",
              mode ? "Legato" : "Poly", (double)spV3Peak(refL, 0, kFrames));

        bool all = true;
        for (int chunk : { 1, 7, 64, 300, 1024 }) {
            auto alt = build();
            if (!alt) break;
            spRender(*alt, ev, kFrames, chunk, altL, altR, 120.0);
            const f32 diff = std::fmax(spV2MaxDiff(refL, altL), spV2MaxDiff(refR, altR));
            if (diff != 0.f) all = false;
            CHECK(diff == 0.f, "v3 busy patch (%s): blocks of %d bit-identical to %d "
                               "(max diff %.9f)", mode ? "Legato" : "Poly", chunk,
                  kBlock, (double)diff);
        }
        CHECK(all, "drawn grids, one-shot LFOs, wheel/bend/CC traffic and three "
                   "response curves all land on ABSOLUTE sample time (%s)",
              mode ? "Legato" : "Poly");

        auto a = build();
        auto b2 = build();
        if (a && b2) {
            std::vector<f32> aL, aR, bL, bR;
            spRender(*a, ev, kFrames, kBlock, aL, aR, 120.0);
            spRender(*b2, ev, kFrames, kBlock, bL, bR, 120.0);
            CHECK(spV2MaxDiff(aL, bL) == 0.f,
                  "two fresh instances render the v3 busy patch identically (%s)",
                  mode ? "Legato" : "Poly");
        }
    }
}

// ---------------------------------------------------------------------------

// spRender with a TRUTHFUL transport: the beat advances with the samples, as a
// host's does. spRender itself pushes a constant beat, which is a fine model of
// an offline render and is deliberately kept that way — the drawn grid's step
// index falls back to accumulated phase under exactly that host, and the tests
// above rely on it. This one models the other host.
static void spRenderBeat(PluginInstance& p, const std::vector<SpEvent>& ev, int frames,
                         int chunk, std::vector<f32>& L, std::vector<f32>& R,
                         f64 bpm, f64 beat0) {
    L.assign((size_t)frames, 0.f);
    R.assign((size_t)frames, 0.f);
    std::vector<f32> bl((size_t)chunk, 0.f), br((size_t)chunk, 0.f);
    size_t next = 0;
    for (int i = 0; i < frames; i += chunk) {
        const int n = (frames - i) < chunk ? (frames - i) : chunk;
        while (next < ev.size() && ev[next].frame < i + n) {
            const u8 m[3] = { ev[next].st, ev[next].a, ev[next].b };
            p.midi(m, 3, ev[next].frame - i);
            ++next;
        }
        p.setTransport(bpm, beat0 + (f64)i * bpm / (60.0 * kSR), true);
        std::fill(bl.begin(), bl.begin() + n, 0.f);
        std::fill(br.begin(), br.begin() + n, 0.f);
        f32* o[2] = { bl.data(), br.data() };
        p.process(nullptr, o, 2, n);
        for (int j = 0; j < n; ++j) {
            L[(size_t)(i + j)] = bl[(size_t)j];
            R[(size_t)(i + j)] = br[(size_t)j];
        }
    }
}

static void testSpectraV3BeatLock(PluginRegistry& reg) {
    banner("Spectra v3: the drawn grid locks to the transport beat (orchestrator ruling)");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;

    // One bar per cycle at 120 bpm: four beats, 96000 samples, 6000 a step.
    // A render that STARTS at beat 2.0 is halfway through the bar, so the
    // sequence must start on step 8 and not on step 0. That is the whole of
    // what "a step sequencer locks to the grid" means, and it is the one
    // behaviour the ruling adds over v2's rate-sync.
    auto mk = [&]() {
        auto s = spV3Probe(reg, *d);
        if (!s) return s;
        s->setParam(spIdx(*s, "LFO Shape"), 5.f);
        s->setParam(spIdx(*s, "LFO Sync"), 3.f);          // one bar
        spV3Slot(*s, 0, 1, 5, 1.f);
        s->setStateString("nxspc1;lfo1=00000000f0000000");  // one pulse, at step 8
        return s;
    };

    auto atZero = mk();
    auto atTwo  = mk();
    std::vector<f32> zL, zR, tL, tR;
    if (atZero && atTwo) {
        spRenderBeat(*atZero, spV3Note(), 60000, kBlock, zL, zR, 120.0, 0.0);
        spRenderBeat(*atTwo,  spV3Note(), 60000, kBlock, tL, tR, 120.0, 2.0);
        CHECK(spV3Peak(zL, 0, 20000) == 0.f,
              "starting at beat 0, the step-8 pulse has not arrived in the first "
              "20000 samples (%.9f)", (double)spV3Peak(zL, 0, 20000));
        CHECK(spV3Peak(zL, 48200, 512) > 0.05f,
              "...it arrives halfway through the bar, at 48000 (%.4f)",
              (double)spV3Peak(zL, 48200, 512));
        CHECK(spV3Peak(tL, 200, 512) > 0.05f,
              "starting at BEAT 2.0 the sequence begins ON step 8 — the grid is "
              "locked to the bar line, not to prepare() (%.4f)",
              (double)spV3Peak(tL, 200, 512));
        CHECK(spV3Peak(tL, 8000, 512) == 0.f,
              "...and step 9 is silent again, so it is the grid and not a level "
              "offset (%.9f)", (double)spV3Peak(tL, 8000, 512));
    }

    // ...and the lock is still block-size invariant, because the beat ANCHORS
    // the counter and does not drive it.
    {
        auto ref = mk();
        std::vector<f32> rL, rR, aL, aR;
        if (ref) {
            spRenderBeat(*ref, spV3Note(), 30000, kBlock, rL, rR, 120.0, 2.0);
            bool all = true;
            for (int chunk : { 1, 7, 64, 300, 1024 }) {
                auto alt = mk();
                if (!alt) break;
                spRenderBeat(*alt, spV3Note(), 30000, chunk, aL, aR, 120.0, 2.0);
                const f32 diff = spV2MaxDiff(rL, aL);
                if (diff != 0.f) all = false;
                CHECK(diff == 0.f, "beat-locked grid: blocks of %d bit-identical to %d "
                                   "(max diff %.9f)", chunk, kBlock, (double)diff);
            }
            CHECK(all, "the transport ANCHORS the beat counter and never drives it, so "
                       "a phase read once per block cannot quantise the render");
        }
    }

    // Shapes 0..4 are NOT phase-locked: the ruling's point 1. A sine LFO
    // rendered from beat 0 and from beat 2 is the same signal, because it rate
    // syncs exactly as it did in v1 and v2.
    {
        auto mk2 = [&]() {
            auto s = spV3Probe(reg, *d);
            if (!s) return s;
            s->setParam(spIdx(*s, "LFO Shape"), 0.f);      // sine
            s->setParam(spIdx(*s, "LFO Sync"), 3.f);
            s->setParam(spIdx(*s, "A Level"), 0.5f);
            spV3Slot(*s, 0, 1, 5, 0.5f);
            return s;
        };
        auto a = mk2(), b2 = mk2();
        if (a && b2) {
            std::vector<f32> aL2, aR2, bL2, bR2;
            spRenderBeat(*a,  spV3Note(), 30000, kBlock, aL2, aR2, 120.0, 0.0);
            spRenderBeat(*b2, spV3Note(), 30000, kBlock, bL2, bR2, 120.0, 2.0);
            CHECK(spV3Peak(aL2, 0, 30000) > 0.05f, "the sine-LFO render is not silent");
            CHECK(spV2MaxDiff(aL2, bL2) == 0.f,
                  "shapes 0..4 in Loop are NOT phase-locked: the same patch from beat 0 "
                  "and from beat 2 is bit-identical, exactly as in v2");
        }
    }

    // ...and a ONE-SHOT drawn grid ignores the beat entirely: its origin is the
    // note (the ruling's point 3).
    {
        auto mk3 = [&](f64 beat0) {
            auto s = spV3Probe(reg, *d);
            if (!s) return std::vector<f32>{};
            s->setParam(spIdx(*s, "LFO Shape"), 5.f);
            s->setParam(spIdx(*s, "LFO Sync"), 3.f);
            s->setParam(100, 1.f);                         // one-shot
            spV3Slot(*s, 0, 1, 5, 1.f);
            s->setStateString((std::string("nxspc1;lfo1=") + kSpV3RampGrid).c_str());
            std::vector<f32> L, R;
            spRenderBeat(*s, spV3Note(12000), 60000, kBlock, L, R, 120.0, beat0);
            return L;
        };
        const std::vector<f32> a = mk3(0.0), b2 = mk3(2.0);
        if (!a.empty() && !b2.empty()) {
            CHECK(spV3Peak(a, 0, 60000) > 0.05f, "the one-shot render is not silent");
            CHECK(spV2MaxDiff(a, b2) == 0.f,
                  "a One-shot drawn grid is identical from beat 0 and from beat 2 — its "
                  "origin is the NOTE, and sync sets only its speed");
        }
    }
}

// ---------------------------------------------------------------------------
// Spectra v4 (ids 109..124, one widened source enum and two new state records)
// — the step-sequencer arpeggiator.
//
// House rule, unchanged since v1: everything below MEASURES. A mode's note
// order is not asserted, it is READ OUT of the rendered audio one step at a
// time; a gate is not asserted to be a percentage of the step, the sounding
// length is counted in samples and divided by the step; swing is not asserted
// to be B/3 at 100 %, the onset is found and the delay is measured.
//
// HOW A STEP IS READ OUT OF THE AUDIO. spV4Probe() below is table 7 at
// position 0 — which that table's own generator says is EXACTLY a sine, the
// fold's drive being zero there — with one unison voice, no detune, no spread,
// the filter wide open and an envelope that is flat at 1 within half a
// millisecond and gone within one. So a monophonic step is a pure tone whose
// upward zero crossings COUNT its frequency, and its start and end are a square
// amplitude edge. Chord mode is the one polyphonic case and is read with a
// single-bin DFT at each candidate fundamental instead.
//
// THE CLOCK EVERY TIMING NUMBER COMES FROM. Arp Sync 6 is 1/8, one eighth note
// is 0.25 s at 120 bpm, and 0.25 s is 12000 samples at 48 kHz. Every constant
// below is derived from that one and is written out rather than hidden.
// ---------------------------------------------------------------------------

// Raw contract ids, exactly as spV3Slot uses them: ids are the frozen
// interface, so a test that spells them is a test that catches a reorder.
enum : int {
    kApOn = 109, kApMode = 110, kApRate = 111, kApSync = 112, kApOctaves = 113,
    kApOctMode = 114, kApGate = 115, kApSwing = 116, kApHold = 117,
    kApRetrig = 118, kApVelMode = 119, kApFixedVel = 120, kApSteps = 121,
    kApChance = 122
};
static constexpr int kApStep     = 12000;   // samples in a 1/8 step at 120 bpm
static constexpr int kApSyncEigh = 6;       // the sync table's 1/8 index
static const char* kApRowOn  = "05050505050505050505050505050505";  // the default
static const char* kApRowFull = "ffffffffffffffff";                  // the default

static std::unique_ptr<PluginInstance> spV4Probe(PluginRegistry& reg, const PluginDesc& d) {
    auto s = reg.instantiate(d, kSR, kBlock);
    if (!s) return s;
    s->setParam(spIdx(*s, "A Table"), 7.f);          // Fold at position 0 = sine
    s->setParam(spIdx(*s, "A Position"), 0.f);
    s->setParam(spIdx(*s, "A Level"), 0.8f);
    s->setParam(spIdx(*s, "A Unison"), 1.f);
    s->setParam(spIdx(*s, "A Detune"), 0.f);
    s->setParam(spIdx(*s, "A Spread"), 0.f);
    s->setParam(spIdx(*s, "B Level"), 0.f);
    s->setParam(spIdx(*s, "Attack"), 0.5f);
    s->setParam(spIdx(*s, "Decay"), 5000.f);
    s->setParam(spIdx(*s, "Sustain"), 1.f);
    s->setParam(spIdx(*s, "Release"), 1.f);
    s->setParam(spIdx(*s, "Cutoff"), 20000.f);
    s->setParam(spIdx(*s, "Resonance"), 0.f);
    s->setParam(spIdx(*s, "Master"), 1.f);
    s->setParam(spIdx(*s, "Voices"), 16.f);
    s->setParam(kApOn, 1.f);
    s->setParam(kApSync, (f32)kApSyncEigh);
    s->setParam(kApGate, 50.f);
    return s;
}

// The MIDI note sounding in a window, from the sine's upward zero crossings.
// -1 means "silent". Deliberately not an FFT: a pure tone's crossings are its
// frequency exactly, and a wrong answer here would be a wrong answer nobody
// could argue with.
static int spV4NoteIn(const std::vector<f32>& x, int from, int n) {
    if (spV3Peak(x, from, n) < 0.02f) return -1;
    int first = -1, last = -1, cross = 0;
    const int hi = (from + n) > (int)x.size() ? (int)x.size() : (from + n);
    for (int i = (from < 1 ? 1 : from); i < hi; ++i) {
        if (x[(size_t)(i - 1)] < 0.f && x[(size_t)i] >= 0.f) {
            if (first < 0) first = i;
            last = i;
            ++cross;
        }
    }
    if (cross < 3 || last <= first) return -1;
    const f64 hz = (f64)(cross - 1) * kSR / (f64)(last - first);
    return (int)std::lround(69.0 + 12.0 * std::log2(hz / 440.0));
}

// Step `k`'s note, read from the middle of its sounding half.
static int spV4Step(const std::vector<f32>& x, int k) {
    return spV4NoteIn(x, k * kApStep + 1500, 3000);
}

// One DFT bin, Hann-windowed, for the one polyphonic mode. Chord is the only
// place a window holds more than one fundamental at once.
static f64 spV4Bin(const std::vector<f32>& x, int from, int n, f64 hz) {
    f64 re = 0.0, im = 0.0, wsum = 0.0;
    const f64 w = 6.283185307179586 * hz / kSR;
    for (int i = 0; i < n; ++i) {
        const int k = from + i;
        if (k < 0 || k >= (int)x.size()) continue;
        const f64 win = 0.5 - 0.5 * std::cos(6.283185307179586 * (f64)i / (f64)n);
        const f64 v = (f64)x[(size_t)k] * win;
        re += v * std::cos(w * (f64)i);
        im -= v * std::sin(w * (f64)i);
        wsum += win;
    }
    if (wsum <= 0.0) return 0.0;
    return 2.0 * std::sqrt(re * re + im * im) / wsum;
}
static f64 spV4Hz(int note) { return 440.0 * std::pow(2.0, ((f64)note - 69.0) / 12.0); }

// The sounding length of step k, in samples, by an amplitude gate. The window
// is 128 samples, so every note used for a timing measurement is high enough
// that 128 samples hold a whole cycle.
static int spV4Sounding(const std::vector<f32>& x, int from, int to) {
    int n = 0;
    for (int i = from; i + 128 <= to && i + 128 <= (int)x.size(); i += 32)
        if (spV3Peak(x, i, 128) > 0.05f) n += 32;
    return n;
}
// The first RISING edge at or after `from`: silence, then sound. A rising edge
// and not "is it sounding", because a gate of 50 % leaves the second half of
// every step lit and a level test would report the sample it started looking
// at. The 128-sample window means the answer can be up to 128 samples early,
// which every tolerance below allows for.
static int spV4Onset(const std::vector<f32>& x, int from, int to) {
    bool was = spV3Peak(x, from, 128) > 0.05f;
    for (int i = from + 32; i + 128 <= to && i + 128 <= (int)x.size(); i += 32) {
        const bool on = spV3Peak(x, i, 128) > 0.05f;
        if (on && !was) return i;
        was = on;
    }
    return -1;
}

// A chord, held for the whole render, pressed on one sample so that the first
// note-on is the NEW CHORD and the rest join it.
static std::vector<SpEvent> spV4Chord(std::initializer_list<u8> notes, int frame = 0) {
    std::vector<SpEvent> ev;
    for (u8 n : notes) ev.push_back({ frame, 0x90, n, 100 });
    return ev;
}

// The melody the arp plays, step by step, over `steps` steps.
static std::vector<int> spV4Melody(const std::vector<f32>& x, int steps) {
    std::vector<int> m;
    m.reserve((size_t)steps);
    for (int k = 0; k < steps; ++k) m.push_back(spV4Step(x, k));
    return m;
}
static std::string spV4Show(const std::vector<int>& m) {
    std::string o;
    char b[16];
    for (size_t i = 0; i < m.size(); ++i) {
        std::snprintf(b, sizeof b, "%s%d", i ? " " : "", m[i]);
        o += b;
    }
    return o;
}
static bool spV4Eq(const std::vector<int>& a, std::initializer_list<int> b) {
    if (a.size() != b.size()) return false;
    size_t i = 0;
    for (int v : b) if (a[i++] != v) return false;
    return true;
}

// ---------------------------------------------------------------------------

static void testSpectraV4Contract(PluginRegistry& reg) {
    banner("Spectra v4: the spent reserved tail, the append and the widened source");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;
    auto s = reg.instantiate(*d, kSR, kBlock);
    if (!s) return;

    // 125 ids total (0..124): 115 functional, 10 reserved. The whole parameter
    // table is checked mechanically by testSpectraContract against the
    // transcription above; what is checked HERE is the budget and the defaults
    // the contract argues for in words.
    CHECK(s->paramCount() == 125, "kSpParamCount is 125 (%d)", s->paramCount());
    CHECK(s->paramCount() <= 128, "125 <= kMaxParams = 128: v4 fits and the cap is NOT raised");

    // THE BIT-IDENTITY SWITCH. Everything else in the block is reachable only
    // behind it.
    CHECK(s->getParam(kApOn) == 0.f, "Arp On defaults to 0 — the revision's bit-identity switch");
    CHECK(s->getParam(kApMode) == 0.f, "Arp Mode defaults to 0 (Up), so a v3 .nxp's zero lands on it");

    // The defaults that are not simply the bottom of their range, each one
    // named in the contract rather than left to be discovered.
    CHECK(s->getParam(kApSync) == 7.f,
          "Arp Sync defaults to 7 = 1/16 — the only v4 default that is not the "
          "bottom of its range, and it costs nothing because Arp On is 0 (%g)",
          (double)s->getParam(kApSync));
    CHECK(s->getParam(kApRetrig) == 1.f,
          "Arp Retrig defaults to 1: a player expects the first note they press to sound");
    CHECK(s->getParam(kApGate) == 50.f, "Arp Gate defaults to 50 %%");
    CHECK(s->getParam(kApRate) == 2.f, "Arp Rate defaults to 2 Hz, LFO Rate's default");
    CHECK(s->getParam(kApOctaves) == 1.f, "Arp Octaves defaults to 1 — the played octave");
    CHECK(s->getParam(kApSteps) == 16.f, "Arp Steps defaults to 16");
    CHECK(s->getParam(kApChance) == 100.f, "Arp Chance defaults to 100 %% — the no-draw branch");
    CHECK(s->getParam(kApFixedVel) == 100.f, "Arp Fixed Vel defaults to 100");
    CHECK(s->getParam(kApVelMode) == 0.f, "Arp Vel Mode defaults to 0 (As Played)");
    CHECK(s->getParam(kApHold) == 0.f, "Arp Hold defaults to 0");
    CHECK(s->getParam(kApSwing) == 0.f, "Arp Swing defaults to 0 — the no-offset branch");

    // THE FLOOR IS 1 AND NOT 0, and the reason is this device's own wire: a
    // note-on with velocity 0 is a note-off, so a parameter that could express
    // "no note" would express it.
    s->setParam(kApFixedVel, 0.f);
    CHECK(s->getParam(kApFixedVel) == 1.f,
          "Arp Fixed Vel clamps to 1, never 0 — a generated 0 would be a generated note-off");

    // The one widened enum, and the one that did NOT widen for the third
    // revision running.
    CHECK(s->paramInfo(68).max == 17.f, "M1 Src widens to 0..17 — 17 is Arp Step");
    CHECK(s->paramInfo(69).max == 19.f,
          "the destination enum does NOT widen: Arp Rate cannot stay locked to a bar "
          "line if it is modulated, and a gate consumed once per step is not an "
          "audio-rate target");

    // The eight reserved ids v4 deliberately did not spend.
    bool res = true;
    for (int id : { 46, 47, 52, 53, 66, 67, 92, 93, 123, 124 })
        if (s->paramInfo(id).name != "—") res = false;
    CHECK(res, "46/47, 52/53, 66/67, 92/93 stay reserved and 123/124 are v4's own tail — "
               "a reserved id belongs to the block it sits in");
}

// ---------------------------------------------------------------------------

static void testSpectraV4State(PluginRegistry& reg) {
    banner("Spectra v4: the two grid rows, their non-zero defaults, and the refusal line");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;

    // THE EMPTY-STATE ROUND TRIP, restated for v4. Spectra had no state string
    // before v3 and a set that uses none must still write none, so a v2 or v3
    // project round-trips through a v4 build byte-identically. A row still at
    // its default is not emitted.
    {
        auto s = reg.instantiate(*d, kSR, kBlock);
        if (!s) return;
        CHECK(s->stateString().empty(),
              "a fresh v4 instance still writes NO state string (\"%s\")",
              s->stateString().c_str());
        s->setParam(kApOn, 1.f);
        s->setParam(kApMode, 8.f);
        s->setParam(kApSwing, 60.f);
        CHECK(s->stateString().empty(),
              "...and turning the arp on and moving its knobs still writes none: the "
              "rows are STATE and the parameters are not (\"%s\")", s->stateString().c_str());
        // Writing the defaults explicitly is still the default.
        CHECK(s->setStateString(std::string("nxspc1;arpl=") + kApRowFull + ";arps=" + kApRowOn),
              "the default rows parse");
        CHECK(s->stateString().empty(),
              "...and round-trip to the empty state, because they ARE the default (\"%s\")",
              s->stateString().c_str());
    }

    // THE DEFAULTS ARE NOT ALL ZEROS, and that is a genuine divergence from the
    // LFO grids this feature otherwise copies exactly: an all-zero step row is
    // an arp that plays nothing, which is a broken default rather than an inert
    // one. The inert switch is Arp On, not the grid. `05` and not `01` because
    // the octave field is BIASED — code 2 is offset 0.
    {
        auto s = reg.instantiate(*d, kSR, kBlock);
        if (!s) return;
        // One step turned off is enough to make the row non-default, and then
        // BOTH rows are emitted, because a state this build writes is complete.
        CHECK(s->setStateString("nxspc1;arps=05050505050505050505050505050004"),
              "a step row with step 15 turned off parses");
        const std::string st = s->stateString();
        CHECK(st == "nxspc1;arpl=ffffffffffffffff;arps=05050505050505050505050505050004",
              "it is re-emitted with BOTH rows, level first, the untouched level row is "
              "the all-f default, and the REST keeps the octave it was drawn with — "
              "turning a step off and on again may not lose it (\"%s\")", st.c_str());
        auto s2 = reg.instantiate(*d, kSR, kBlock);
        if (s2) {
            CHECK(s2->setStateString(st) && s2->stateString() == st,
                  "and the round trip is an exact inverse");
        }
    }

    // DEGRADED, NOT REFUSED. An octave code of 5, 6 or 7 clamps to 4 (offset
    // +2) and bits 5..7 are masked off: these are values a LATER, WIDER build
    // could legitimately write, and the versioning rule's job is to let a newer
    // state land on an older build rather than break it.
    {
        auto s = reg.instantiate(*d, kSR, kBlock);
        if (!s) return;
        //  0xff = on(1) + code 7 (bits1..3) + tie(0x10) + every reserved bit.
        //  The clamp takes code 7 -> 4 and the mask takes 0xe0 -> 0, leaving
        //  0x19 = on, octave code 4 (+2), tie.
        CHECK(s->setStateString("nxspc1;arps=ff050505050505050505050505050505"),
              "an octave code of 7 with every reserved bit set is ACCEPTED");
        const std::string st = s->stateString();
        CHECK(st.find(";arps=19") != std::string::npos,
              "...and lands as 0x19 — code clamped to 4, reserved bits masked (\"%s\")",
              st.c_str());
    }

    // REFUSED, and the whole state refuses: these are strings this writer could
    // not have produced, so nothing is applied and the device is left as it was.
    {
        auto s = reg.instantiate(*d, kSR, kBlock);
        if (!s) return;
        s->setStateString("nxspc1;arpl=0123456789abcdef");
        const std::string good = s->stateString();
        const char* bad[] = {
            "nxspc1;arpl=0123456789abcde",              // 15 digits
            "nxspc1;arpl=0123456789abcdeff",            // 17
            "nxspc1;arpl=0123456789ABCDEF",             // uppercase
            "nxspc1;arpl=0123456789abcdeg",             // not hex
            "nxspc1;arps=0505050505050505050505050505050",   // 31 digits
            "nxspc1;arps=05050505050505050505050505050505f", // 33
            "nxspc1;arps=050505050505050505050505050505G5",  // not hex
            "nxspc1;arps=0505050505050505050505050505050A",  // UPPERCASE hex
            "nxspc1;arps=050505050505050505050505050505FF",  // and again, tie bits
            "nxspc1;arpl=ffffffffffffffff;arpl=0000000000000000",  // duplicate
            "nxspc1;arps=05050505050505050505050505050505;arps=05050505050505050505050505050505",
        };
        int refused = 0;
        for (const char* b : bad) if (!s->setStateString(b)) ++refused;
        CHECK(refused == (int)(sizeof bad / sizeof bad[0]),
              "all %d malformed arp rows are REFUSED — length, charset and CASE, on "
              "both rows, plus a duplicate key (%d)",
              (int)(sizeof bad / sizeof bad[0]), refused);
        CHECK(s->stateString() == good,
              "...and a refusal changed nothing at all (\"%s\")", s->stateString().c_str());
    }

    // loadPreset RESETS THE ARP STATE TOO — v3's extension of "a preset is
    // COMPLETE however short it is written", covering the two new rows without
    // amendment.
    {
        auto s = reg.instantiate(*d, kSR, kBlock);
        if (!s) return;
        s->setStateString("nxspc1;arpl=0123456789abcdef;arps=00010203040506070809000102030405");
        CHECK(!s->stateString().empty(), "a drawn arp pattern is state");
        s->loadPreset(0);                                  // Init
        CHECK(s->stateString().empty(),
              "loadPreset resets arpl and arps to their defaults, so switching to a "
              "preset that mentions neither lands where a fresh instance would (\"%s\")",
              s->stateString().c_str());
    }

    // THE SPARP MACRO'S PARSE SIDE, which is this file's half of it: the bank
    // authors the two rows and spectra.cpp turns them into state. A row that
    // turns the arp on and carries an SPARP must arrive as `arpl`/`arps` in the
    // loaded state — and it must arrive the SAME whatever the instrument was
    // doing before, because loadPreset resets every state block first.
    {
        auto a = reg.instantiate(*d, kSR, kBlock);
        auto b = reg.instantiate(*d, kSR, kBlock);
        if (a && b) {
            b->setStateString("nxspc1;lfo2=0123456789abcdef;smooth2=250;cc=74;"
                              "arpl=0123456789abcdef;arps="
                              "1517151715171517151715171517151f");
            int arps = 0, on = 0, same = 0;
            for (int k = 0; k < a->presetCount(); ++k) {
                a->loadPreset(k);
                if (a->getParam(kApOn) != 1.f) continue;
                ++on;
                const std::string st = a->stateString();
                if (st.find(";arps=") != std::string::npos &&
                    st.find(";arpl=") != std::string::npos) ++arps;
                b->loadPreset(k);
                if (b->stateString() == st) ++same;
            }
            CHECK(on >= 24, "the factory bank carries %d rows with Arp On = 1", on);
            CHECK(arps == on,
                  "every one of them arrives with BOTH arp rows in its state: SPARP's "
                  "two arguments are mandatory and both reach the device (%d of %d)",
                  arps, on);
            CHECK(same == on,
                  "...and a preset lands on the same arp state from a patch with a drawn "
                  "grid, a learned CC and a drawn arp pattern already in it (%d of %d)",
                  same, on);
        }
    }

    // Forward compatibility is unchanged: a key this build does not know is
    // SKIPPED, and the arp rows around it still land.
    {
        auto s = reg.instantiate(*d, kSR, kBlock);
        if (!s) return;
        CHECK(s->setStateString("nxspc1;arpp=ffffffffffffffff;arpl=0123456789abcdef"),
              "the pre-declared future third row (arpp) is SKIPPED, not refused");
        CHECK(s->stateString() == "nxspc1;arpl=0123456789abcdef;arps=" + std::string(kApRowOn),
              "...and the rows this build knows still landed (\"%s\")",
              s->stateString().c_str());
    }
}

// ---------------------------------------------------------------------------

static void testSpectraV4Modes(PluginRegistry& reg) {
    banner("Spectra v4: the nine modes, read one step at a time out of the audio");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;

    // The contract's own worked example, transposed up an octave so the sine's
    // zero crossings are quick to count: N = [C5, E5, G5] = 72, 76, 79.
    const auto chord = spV4Chord({ 72, 76, 79 });
    auto run = [&](int mode, int steps) {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (!s) return std::vector<int>{};
        s->setParam(kApMode, (f32)mode);
        spRender(*s, chord, steps * kApStep, kBlock, L, R, 120.0);
        return spV4Melody(L, steps);
    };

    struct Row { int mode; const char* name; std::initializer_list<int> want; };
    const Row rows[] = {
        { 0, "Up",                 { 72, 76, 79, 72, 76, 79 } },
        { 1, "Down",               { 79, 76, 72, 79, 76, 72 } },
        { 2, "Up-Down Inclusive",  { 72, 76, 79, 79, 76, 72 } },
        { 3, "Up-Down Exclusive",  { 72, 76, 79, 76, 72, 76 } },
        { 4, "Down-Up",            { 79, 76, 72, 76, 79, 76 } },
        { 5, "As Played",          { 72, 76, 79, 72, 76, 79 } },
        { 8, "Thumb",              { 72, 76, 72, 79, 72, 76 } },
        { 9, "Pinky",              { 72, 79, 76, 79, 72, 79 } },
    };
    for (const Row& r : rows) {
        const std::vector<int> got = run(r.mode, 6);
        CHECK(spV4Eq(got, r.want), "mode %d %s plays %s (got %s)",
              r.mode, r.name, spV4Show(std::vector<int>(r.want)).c_str(),
              spV4Show(got).c_str());
    }

    // AS PLAYED IS THE INSERTION ORDER, and it is the one mode that can tell a
    // rolled chord from a struck one. Pressed high-to-low it plays high-to-low.
    {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (s) {
            std::vector<SpEvent> ev = { { 0, 0x90, 79, 100 }, { 1, 0x90, 72, 100 },
                                        { 2, 0x90, 76, 100 } };
            s->setParam(kApMode, 5.f);
            spRender(*s, ev, 6 * kApStep, kBlock, L, R, 120.0);
            const std::vector<int> m = spV4Melody(L, 6);
            CHECK(spV4Eq(m, { 79, 72, 76, 79, 72, 76 }),
                  "As Played follows the held stack's insertion order, not the pitch "
                  "order (got %s)", spV4Show(m).c_str());
        }
    }

    // ...and As Played SURVIVES A NOTE-OFF, because heldRemove compacts the
    // stack in place and preserves order. This is a property of the existing
    // stack, not a new one.
    {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (s) {
            std::vector<SpEvent> ev = { { 0, 0x90, 79, 100 }, { 1, 0x90, 72, 100 },
                                        { 2, 0x90, 76, 100 },
                                        { 3 * kApStep + 100, 0x80, 72, 0 } };
            s->setParam(kApMode, 5.f);
            s->setParam(kApRetrig, 0.f);          // do not restart on the note-off
            spRender(*s, ev, 8 * kApStep, kBlock, L, R, 120.0);
            const std::vector<int> m = spV4Melody(L, 8);
            CHECK(m[0] == 79 && m[1] == 72 && m[2] == 76 &&
                  m[4] == 79 && m[5] == 76 && m[6] == 79,
                  "releasing the middle-pressed note leaves the other two in the order "
                  "they were pressed (got %s)", spV4Show(m).c_str());
        }
    }

    // A SINGLE NOTE. Up-Down Exclusive and Down-Up have a natural cycle length
    // of 2c-2 = 0 at c = 1, which is not a cycle; max(2c-2, 1) makes both
    // degenerate to Up, and Thumb and Pinky get the same guard.
    for (int mode : { 3, 4, 8, 9 }) {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (!s) continue;
        s->setParam(kApMode, (f32)mode);
        spRender(*s, spV4Chord({ 72 }), 4 * kApStep, kBlock, L, R, 120.0);
        const std::vector<int> m = spV4Melody(L, 4);
        CHECK(spV4Eq(m, { 72, 72, 72, 72 }),
              "mode %d on ONE note degenerates to Up rather than to a cycle of length 0 "
              "(got %s)", mode, spV4Show(m).c_str());
    }

    // CHORD (mode 7) is the one polyphonic mode: all of N on every step. Read
    // with a single DFT bin at each of the three fundamentals, because zero
    // crossings cannot see three tones at once.
    {
        auto up = spV4Probe(reg, *d);
        auto ch = spV4Probe(reg, *d);
        std::vector<f32> uL, uR, cL, cR;
        if (up && ch) {
            ch->setParam(kApMode, 7.f);
            spRender(*up, chord, 2 * kApStep, kBlock, uL, uR, 120.0);
            spRender(*ch, chord, 2 * kApStep, kBlock, cL, cR, 120.0);
            const int at = 1500, n = 4096;
            const f64 c72 = spV4Bin(cL, at, n, spV4Hz(72));
            const f64 c76 = spV4Bin(cL, at, n, spV4Hz(76));
            const f64 c79 = spV4Bin(cL, at, n, spV4Hz(79));
            const f64 u72 = spV4Bin(uL, at, n, spV4Hz(72));
            const f64 u76 = spV4Bin(uL, at, n, spV4Hz(76));
            const f64 u79 = spV4Bin(uL, at, n, spV4Hz(79));
            CHECK(u72 > 0.1 && u76 < 0.02 && u79 < 0.02,
                  "the control: step 0 of Up holds ONE fundamental (%.3f / %.3f / %.3f)",
                  u72, u76, u79);
            CHECK(c72 > 0.1 && c76 > 0.1 && c79 > 0.1,
                  "Chord sounds all of N on every step (%.3f / %.3f / %.3f)",
                  c72, c76, c79);
        }
    }

    // CHORD'S CYCLE LENGTH IS 1, which is not a special case in the arithmetic
    // — it is the whole point: with M = 1 the octave axis advances on EVERY
    // step, so Chord over two octaves alternates the chord at +0 and at +1.
    {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (s) {
            s->setParam(kApMode, 7.f);
            s->setParam(kApOctaves, 2.f);
            spRender(*s, spV4Chord({ 60 }), 4 * kApStep, kBlock, L, R, 120.0);
            const std::vector<int> m = spV4Melody(L, 4);
            CHECK(spV4Eq(m, { 60, 72, 60, 72 }),
                  "Chord x 2 octaves alternates chord-at-+0 and chord-at-+1 (got %s)",
                  spV4Show(m).c_str());
        }
    }
}

// ---------------------------------------------------------------------------

static void testSpectraV4Octaves(PluginRegistry& reg) {
    banner("Spectra v4: the octave cycle, the note counter first, and the keyboard's edge");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;

    auto run = [&](int oct, int octMode, int mode, int steps,
                   std::initializer_list<u8> notes, const char* rows = nullptr) {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (!s) return std::vector<int>{};
        s->setParam(kApOctaves, (f32)oct);
        s->setParam(kApOctMode, (f32)octMode);
        s->setParam(kApMode, (f32)mode);
        if (rows) s->setStateString(std::string("nxspc1;arps=") + rows);
        spRender(*s, spV4Chord(notes), steps * kApStep, kBlock, L, R, 120.0);
        return spV4Melody(L, steps);
    };

    // THE NOTE COUNTER ADVANCES FIRST. Two notes over two octaves is
    // C E | C+12 E+12, not C C+12 E E+12: j = k mod M is the FAST axis and
    // u = (k div M) mod L_oct is the slow one.
    {
        const std::vector<int> m = run(2, 0, 0, 8, { 60, 64 });
        CHECK(spV4Eq(m, { 60, 64, 72, 76, 60, 64, 72, 76 }),
              "the arp completes one full traversal of the note cycle before the octave "
              "moves (got %s)", spV4Show(m).c_str());
    }
    // ...and the consequence the contract names rather than leaves to be found:
    // Up-Down over two octaves bounces INSIDE octave 0, then inside octave 1.
    {
        const std::vector<int> m = run(2, 0, 3, 8, { 60, 64, 67 });
        CHECK(spV4Eq(m, { 60, 64, 67, 64, 72, 76, 79, 76 }),
              "Up-Down Exclusive over two octaves bounces inside each octave in turn, "
              "not across the whole span (got %s)", spV4Show(m).c_str());
    }
    // Down: 0, -1, -2, -3.
    {
        const std::vector<int> m = run(3, 1, 0, 6, { 84 });
        CHECK(spV4Eq(m, { 84, 72, 60, 84, 72, 60 }),
              "Oct Mode Down walks 0, -1, -2 (got %s)", spV4Show(m).c_str());
    }
    // Alternate is an up-down-EXCLUSIVE cycle of length 2O-2, which is why
    // O = 3 gives 0, +1, +2, +1 and not 0, +1, +2, +2, +1.
    {
        const std::vector<int> m = run(3, 2, 0, 8, { 48 });
        CHECK(spV4Eq(m, { 48, 60, 72, 60, 48, 60, 72, 60 }),
              "Oct Mode Alternate over three octaves is 0 +1 +2 +1 — 2O-2 and not 2O, "
              "so it never sits on the top octave for two whole note-cycles (got %s)",
              spV4Show(m).c_str());
    }
    // O = 1 is a cycle of length 1 in every mode, including Alternate.
    {
        const std::vector<int> m = run(1, 2, 0, 3, { 60 });
        CHECK(spV4Eq(m, { 60, 60, 60 }), "one octave stays in the played octave (got %s)",
              spV4Show(m).c_str());
    }

    // THE STEP ROW'S OWN OCTAVE COLUMN, which is the exact tool the contract
    // delegates the full-span bounce to. Codes are biased: c - 2, so 0x01 is
    // -2, 0x05 is 0 and 0x09 is +2.
    {
        //           step 0: 05 (+0) · 1: 07 (+1) · 2: 09 (+2) · 3: 03 (-1)
        const std::vector<int> m =
            run(1, 0, 0, 4, { 60 }, "05070903050505050505050505050505");
        CHECK(spV4Eq(m, { 60, 72, 84, 48 }),
              "the step row's octave field is BIASED by 2: codes 2,3,4,1 are +0,+1,+2,-1 "
              "(got %s)", spV4Show(m).c_str());
    }

    // A PITCH OUTSIDE 0..127 MAKES THE STEP SILENT. It is not clamped — a
    // clamped note is a wrong note played confidently — and the step still
    // advances every index, which is the property the next check names.
    {
        //           step 1 asks for +2 octaves from C8 (108): 132, off the keyboard.
        const std::vector<int> m =
            run(1, 0, 0, 4, { 108 }, "05090505050505050505050505050505");
        CHECK(m[0] == 108 && m[1] == -1 && m[2] == 108 && m[3] == 108,
              "a step whose pitch leaves the keyboard is SILENT, not clamped, and the "
              "steps around it are unmoved (got %s)", spV4Show(m).c_str());
    }
    {
        // ...and it does not renumber the melody: with three notes and step 1
        // pushed off the top, step 2 still plays the note step 2 owns.
        const std::vector<int> m =
            run(1, 0, 0, 6, { 105, 109, 112 }, "05090505050505050505050505050505");
        CHECK(m[0] == 105 && m[1] == -1 && m[2] == 112 && m[3] == 105,
              "an out-of-range step advances every index — the melody is not renumbered "
              "(got %s)", spV4Show(m).c_str());
    }
}

// ---------------------------------------------------------------------------

static void testSpectraV4Clock(PluginRegistry& reg) {
    banner("Spectra v4: the step clock, per-step division, swing, gate and the step row");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;

    // THE DIVISION NAMES ONE STEP, NOT THE CYCLE — the one place v4 reads the
    // shared sync table differently from v3's Custom LFO shape, and the
    // divergence is forced: Arp Steps is variable, so a whole-cycle reading
    // would make the pattern-length knob a tempo knob.
    {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (s) {
            s->setParam(kApGate, 50.f);
            spRender(*s, spV4Chord({ 84 }), 6 * kApStep, kBlock, L, R, 120.0);
            int onsets = 0, firstGap = -1, prev = -1;
            for (int i = 0; i + 128 < 6 * kApStep; i += 32) {
                const bool on = spV3Peak(L, i, 128) > 0.05f;
                const bool was = i > 0 && spV3Peak(L, i - 32, 128) > 0.05f;
                if (on && !was) {
                    if (prev >= 0 && firstGap < 0) firstGap = i - prev;
                    prev = i;
                    ++onsets;
                }
            }
            CHECK(onsets == 6, "1/8 at 120 bpm fires six times in six step-lengths (%d)", onsets);
            CHECK(firstGap > kApStep - 200 && firstGap < kApStep + 200,
                  "one step is %d samples — the DIVISION IS THE STEP and not the cycle "
                  "(measured %d)", kApStep, firstGap);
        }
    }
    // ...and shortening the pattern does NOT speed it up. That is the whole
    // argument for the per-step reading, so it is measured and not assumed.
    {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (s) {
            s->setParam(kApSteps, 3.f);
            s->setParam(kApGate, 40.f);
            spRender(*s, spV4Chord({ 84 }), 6 * kApStep, kBlock, L, R, 120.0);
            const int a = spV4Onset(L, kApStep - 2000, 2 * kApStep);
            const int b = spV4Onset(L, 2 * kApStep - 2000, 3 * kApStep);
            CHECK(a >= 0 && b >= 0 && (b - a) > kApStep - 300 && (b - a) < kApStep + 300,
                  "Arp Steps = 3 keeps the step length at %d samples: a length control "
                  "that is secretly a tempo control is not a length control (%d)",
                  kApStep, b - a);
        }
    }
    // FREE RUNNING (Sync 0): Arp Rate gives steps per second and the transport
    // is not read at all.
    {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (s) {
            s->setParam(kApSync, 0.f);
            s->setParam(kApRate, 8.f);           // 8 steps a second = 6000 samples
            s->setParam(kApGate, 40.f);
            spRender(*s, spV4Chord({ 84 }), 5 * 6000, kBlock, L, R, 120.0);
            const int a = spV4Onset(L, 6000 - 1500, 12000);
            const int b = spV4Onset(L, 12000 - 1500, 18000);
            CHECK(a >= 0 && b >= 0 && (b - a) > 5800 && (b - a) < 6200,
                  "free-running at 8 Hz puts a step every 6000 samples (%d)", b - a);
        }
    }

    // GATE is a fraction of the NOMINAL step.
    for (int g : { 25, 50, 90 }) {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (!s) continue;
        s->setParam(kApGate, (f32)g);
        spRender(*s, spV4Chord({ 84 }), 4 * kApStep, kBlock, L, R, 120.0);
        const int lit = spV4Sounding(L, kApStep, 2 * kApStep);
        const f64 pct = 100.0 * (f64)lit / (f64)kApStep;
        CHECK(pct > (f64)g - 6.0 && pct < (f64)g + 6.0,
              "Arp Gate %d %% sounds %.1f %% of the step", g, pct);
    }

    // SWING delays ODD k by B * Swing / 300, so 100 % is exactly B/3 — the 2:1
    // triplet, the universal meaning of full swing. Even steps do not move.
    for (int sw : { 50, 100 }) {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (!s) continue;
        s->setParam(kApSwing, (f32)sw);
        s->setParam(kApGate, 40.f);
        spRender(*s, spV4Chord({ 84 }), 4 * kApStep, kBlock, L, R, 120.0);
        const int even = spV4Onset(L, 2 * kApStep - 400, 3 * kApStep);
        const int odd  = spV4Onset(L, 3 * kApStep - 400, 4 * kApStep);
        const f64 want = (f64)kApStep * (f64)sw / 300.0;
        CHECK(even >= 2 * kApStep - 400 && even < 2 * kApStep + 400,
              "swing %d %%: EVEN step 2 is not delayed (onset %d, step at %d)",
              sw, even, 2 * kApStep);
        CHECK(odd >= 0 && std::fabs((f64)(odd - 3 * kApStep) - want) < 400.0,
              "swing %d %%: odd step 3 is delayed by B*Swing/300 = %.0f samples "
              "(measured %d)", sw, want, odd - 3 * kApStep);
    }
    // ODD `k` IS DELAYED, AND `k` IS THE ABSOLUTE STEP NUMBER — never the
    // pattern index. With an ODD pattern length the two disagree on every other
    // loop, which is the only place the distinction is observable and therefore
    // the only place it can be checked: at Steps = 3, k = 3 has pattern index 0
    // and k = 4 has pattern index 1. The swing must follow k, so that it stays
    // welded to the beat where a listener expects it rather than flipping every
    // time an odd pattern wraps.
    {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (s) {
            s->setParam(kApSwing, 100.f);
            s->setParam(kApSteps, 3.f);
            s->setParam(kApGate, 40.f);
            spRender(*s, spV4Chord({ 84 }), 6 * kApStep, kBlock, L, R, 120.0);
            const f64 want = (f64)kApStep / 3.0;
            const int k3 = spV4Onset(L, 3 * kApStep - 2000, 4 * kApStep + 6000);
            const int k4 = spV4Onset(L, 4 * kApStep - 1000, 5 * kApStep + 6000);
            CHECK(k3 >= 0 && std::fabs((f64)(k3 - 3 * kApStep) - want) < 400.0,
                  "Steps = 3: k = 3 is ODD and IS delayed, though its pattern index 0 is "
                  "even (delay %d, want %.0f)", k3 - 3 * kApStep, want);
            CHECK(k4 >= 0 && std::fabs((f64)(k4 - 4 * kApStep)) < 400.0,
                  "...and k = 4 is EVEN and is not, though its pattern index 1 is odd "
                  "(delay %d)", k4 - 4 * kApStep);
        }
    }
    // ...and the gate does NOT stretch with the swing: a swung pair keeps two
    // notes of the same length and moves the second one.
    {
        auto flat = spV4Probe(reg, *d);
        auto swung = spV4Probe(reg, *d);
        std::vector<f32> fL, fR, sL, sR;
        if (flat && swung) {
            flat->setParam(kApGate, 40.f);
            swung->setParam(kApGate, 40.f);
            swung->setParam(kApSwing, 100.f);
            spRender(*flat, spV4Chord({ 84 }), 4 * kApStep, kBlock, fL, fR, 120.0);
            spRender(*swung, spV4Chord({ 84 }), 4 * kApStep, kBlock, sL, sR, 120.0);
            const int a = spV4Sounding(fL, 3 * kApStep, 4 * kApStep);
            const int b = spV4Sounding(sL, 3 * kApStep, 4 * kApStep + 4000);
            CHECK(std::abs(a - b) < 400,
                  "an odd step's sounding length is the same swung and unswung (%d vs %d) "
                  "— a gate that stretched with swing would turn a feel control into a "
                  "duration control", a, b);
        }
    }

    // THE THREE STATES OF A STEP, and there are exactly three.
    //
    // REST: the previous note ends at its own gate, unaffected.
    {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (s) {
            s->setStateString("nxspc1;arps=05040505050505050505050505050505");
            spRender(*s, spV4Chord({ 84, 88, 91 }), 4 * kApStep, kBlock, L, R, 120.0);
            const std::vector<int> m = spV4Melody(L, 4);
            CHECK(m[0] == 84 && m[1] == -1 && m[2] == 91 && m[3] == 84,
                  "an OFF step is a rest and does not renumber the melody: step 2 still "
                  "plays the third note (got %s)", spV4Show(m).c_str());
        }
    }
    // ...and "the previous note ends at its OWN gate, unaffected" is a claim
    // about the rest doing NOTHING, so it needs a note that outlives the rest to
    // be visible at all. At gate 190 % step 0's note runs to 1.9 steps, straight
    // through the rest at step 1.
    {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (s) {
            s->setParam(kApGate, 190.f);
            s->setStateString("nxspc1;arps=05040405050505050505050505050505");
            spRender(*s, spV4Chord({ 84 }), 4 * kApStep, kBlock, L, R, 120.0);
            const int lit = spV4Sounding(L, 0, 3 * kApStep);
            CHECK(lit > (int)(1.8 * kApStep) && lit < (int)(2.0 * kApStep),
                  "a rest does not cut the note before it: step 0's note still ends at its "
                  "own gate, 1.9 steps later (%d of %d)", lit, kApStep);
        }
    }
    // TIE: the sounding note's off moves to this step's onset plus the gate. No
    // new note-on, so the pitch is the PREVIOUS step's and the sound is
    // continuous across the step boundary.
    {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (s) {
            s->setParam(kApGate, 50.f);
            s->setStateString("nxspc1;arps=05150505050505050505050505050505");
            spRender(*s, spV4Chord({ 84, 88, 91 }), 4 * kApStep, kBlock, L, R, 120.0);
            const std::vector<int> m = spV4Melody(L, 4);
            CHECK(m[0] == 84 && m[1] == 84 && m[2] == 91,
                  "a TIE holds the previous note through its step, and step 2 still plays "
                  "the note step 2 owns (got %s)", spV4Show(m).c_str());
            // Continuity: the gate boundary that would have fallen halfway
            // through step 0 is gone, and the note runs to halfway through
            // step 1 instead.
            const int lit = spV4Sounding(L, 0, 2 * kApStep);
            CHECK(lit > (int)(1.4 * kApStep) && lit < (int)(1.6 * kApStep),
                  "...and it sounds for one and a half steps rather than half a step "
                  "(%d of %d)", lit, kApStep);
        }
    }
    // A TIE WITH NOTHING TO HOLD IS NOTHING: after a rest, after a step that
    // left the keyboard, and at the very start.
    {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (s) {
            //  step 0 rest · step 1 tie · step 2 on
            s->setStateString("nxspc1;arps=04150505050505050505050505050505");
            spRender(*s, spV4Chord({ 84, 88, 91 }), 4 * kApStep, kBlock, L, R, 120.0);
            const std::vector<int> m = spV4Melody(L, 4);
            CHECK(m[0] == -1 && m[1] == -1 && m[2] == 91,
                  "a tie whose predecessor did not sound is SILENT, and the step after it "
                  "is untouched (got %s)", spV4Show(m).c_str());
        }
    }
    // A PATTERN THAT IS ALL TIES SOUNDS NOTHING AT ALL, because no step ever
    // starts a note — which is why the 16-tie bound is unreachable in practice.
    {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (s) {
            s->setStateString("nxspc1;arps=15151515151515151515151515151515");
            spRender(*s, spV4Chord({ 84 }), 4 * kApStep, kBlock, L, R, 120.0);
            CHECK(spV3Peak(L, 0, 4 * kApStep) < 0.001f,
                  "an all-tie pattern is silent (%.6f)",
                  (double)spV3Peak(L, 0, 4 * kApStep));
        }
    }

    // GATE OVER 100 % ON A REPEATED NOTE NUMBER. The naive reading is a
    // stuck-note bug: noteOff() releases the NEWEST matching voice, so a
    // generated off arriving after the next step's on would release the note
    // just started and leave the old one ringing for the rest of the session.
    // The arp emits its off for the outgoing copy IMMEDIATELY BEFORE the on for
    // the new one, at the same stamped sample — so a one-note Up arp at 200 %
    // gate re-attacks cleanly and, when the keys go, everything stops.
    {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (s) {
            s->setParam(kApGate, 200.f);
            std::vector<SpEvent> ev = { { 0, 0x90, 84, 100 },
                                        { 6 * kApStep, 0x80, 84, 0 } };
            spRender(*s, ev, 12 * kApStep, kBlock, L, R, 120.0);
            CHECK(spV3Peak(L, 2 * kApStep, kApStep) > 0.05f,
                  "gate 200 %% keeps a one-note arp sounding");
            CHECK(spV3Peak(L, 9 * kApStep, 3 * kApStep) < 0.001f,
                  "...and nothing is stranded once the key is released: a repeated note "
                  "number never overlaps itself (%.6f)",
                  (double)spV3Peak(L, 9 * kApStep, 3 * kApStep));
        }
    }
    // ...and overlap between DIFFERENT note numbers is real, which is the only
    // place overlap means anything. Two notes at 190 % gate hold two voices.
    {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (s) {
            s->setParam(kApGate, 190.f);
            spRender(*s, spV4Chord({ 72, 79 }), 4 * kApStep, kBlock, L, R, 120.0);
            const int at = 2 * kApStep + 500, n = 4096;
            CHECK(spV4Bin(L, at, n, spV4Hz(72)) > 0.05 && spV4Bin(L, at, n, spV4Hz(79)) > 0.05,
                  "gate 190 %% overlaps two DIFFERENT note numbers (%.3f / %.3f)",
                  spV4Bin(L, at, n, spV4Hz(72)), spV4Bin(L, at, n, spV4Hz(79)));
        }
    }
}

// ---------------------------------------------------------------------------

static void testSpectraV4Velocity(PluginRegistry& reg) {
    banner("Spectra v4: the three velocity modes and the level row");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;

    // The voices' own fixed routing is velAmp = 0.30 + 0.70*vel/127, downstream
    // of all three modes and untouched by any of them, so the peak of a step is
    // a linear readout of the velocity the arp generated.
    auto peakOf = [&](int velMode, int fixed, const char* lvl,
                      std::initializer_list<u8> notes, int step, int vel) {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (!s) return 0.f;
        s->setParam(kApVelMode, (f32)velMode);
        s->setParam(kApFixedVel, (f32)fixed);
        if (lvl) s->setStateString(std::string("nxspc1;arpl=") + lvl);
        std::vector<SpEvent> ev;
        int i = 0;
        for (u8 n : notes) ev.push_back({ i++, 0x90, n, (u8)(vel ? vel : 100) });
        spRender(*s, ev, (step + 2) * kApStep, kBlock, L, R, 120.0);
        return spV3Peak(L, step * kApStep + 1500, 3000);
    };
    auto velOf = [](f32 pk) { return (f64)((pk / 0.8f) - 0.30f) / 0.70f * 127.0; };

    // 0 AS PLAYED — the velocity of the key that CONTRIBUTED the note, which is
    // why the held stack grew a parallel velocity array. Two keys at two
    // velocities keep them apart.
    {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (s) {
            std::vector<SpEvent> ev = { { 0, 0x90, 84, 40 }, { 1, 0x90, 91, 127 } };
            spRender(*s, ev, 4 * kApStep, kBlock, L, R, 120.0);
            const f64 v0 = velOf(spV3Peak(L, 1500, 3000));
            const f64 v1 = velOf(spV3Peak(L, kApStep + 1500, 3000));
            CHECK(std::fabs(v0 - 40.0) < 6.0 && std::fabs(v1 - 127.0) < 6.0,
                  "As Played gives each generated note the velocity of the key that "
                  "contributed it (%.0f then %.0f, played 40 then 127)", v0, v1);
        }
    }
    // 1 FIXED — every generated note carries id 120, whatever was played.
    {
        const f64 a = velOf(peakOf(1, 60, nullptr, { 84, 91 }, 0, 127));
        const f64 b = velOf(peakOf(1, 60, nullptr, { 84, 91 }, 1, 127));
        CHECK(std::fabs(a - 60.0) < 6.0 && std::fabs(b - 60.0) < 6.0,
              "Fixed overrides the played velocity on every step (%.0f, %.0f)", a, b);
    }
    // 2 PATTERN — the level row at the PATTERN index, ABSOLUTE and not a
    // scaling of the played velocity. Digit 0 is velocity 1 and digit 15 is
    // 127: the floor is 1 because 0 is a note-off on this device's wire, so a
    // step drawn at the bottom would emit nothing instead of emitting quietly.
    {
        const char* row = "f0f8ffffffffffff";   // 15, 0, 15, 8, ...
        const f64 v0 = velOf(peakOf(2, 100, row, { 84 }, 0, 20));
        const f64 v1 = velOf(peakOf(2, 100, row, { 84 }, 1, 20));
        const f64 v3 = velOf(peakOf(2, 100, row, { 84 }, 3, 20));
        CHECK(std::fabs(v0 - 127.0) < 6.0, "level digit f is velocity 127 (%.0f)", v0);
        CHECK(std::fabs(v1 - 1.0) < 6.0,
              "level digit 0 is velocity 1 and NOT 0 — a step drawn at the bottom of the "
              "row emits quietly rather than emitting nothing (%.1f)", v1);
        const f64 want = 1.0 + (f64)(int)(126.0 * (8.0 / 15.0) + 0.5);
        CHECK(std::fabs(v3 - want) < 6.0,
              "level digit 8 is velocity %.0f = 1 + round(126*8/15) (%.0f)", want, v3);
        CHECK(std::fabs(v0 - 127.0) < 6.0 && std::fabs(v1 - 1.0) < 6.0,
              "...and Pattern is ABSOLUTE: a key played at 20 still reaches 127");
    }
    // The velocity floor a played key already has is downstream and untouched.
    {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (s) {
            spRender(*s, spV4Chord({ 84 }, 0), 2 * kApStep, kBlock, L, R, 120.0);
            const f64 v = velOf(spV3Peak(L, 1500, 3000));
            CHECK(std::fabs(v - 100.0) < 6.0,
                  "the voices' 30 %% velocity floor is downstream of all three modes and "
                  "is not touched (%.0f)", v);
        }
    }
}

// ---------------------------------------------------------------------------

static void testSpectraV4HoldRetrig(PluginRegistry& reg) {
    banner("Spectra v4: Hold and Retrigger, all four combinations, and the line a knob may not cross");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;

    // RETRIGGER = 1: a NEW CHORD — heldSet empty immediately before this
    // note-on joined it — resets the position to step 0 at that note-on's
    // stamped sample and sounds step 0 immediately.
    {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (s) {
            const int at = kApStep + 3000;      // deliberately NOT on a step boundary
            std::vector<SpEvent> ev = { { at, 0x90, 84, 100 } };
            spRender(*s, ev, 4 * kApStep, kBlock, L, R, 120.0);
            const int on = spV4Onset(L, at - 200, at + kApStep);
            CHECK(on >= at - 200 && on <= at + 200,
                  "step 0 fires at the note-on's own stamped sample, not at the next grid "
                  "line (pressed at %d, sounded at %d)", at, on);
        }
    }
    // ...and ADDING A FINGER to a held chord does NOT retrigger, or rolling a
    // chord on would stutter the pattern once per finger.
    {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (s) {
            std::vector<SpEvent> ev = { { 0, 0x90, 84, 100 },
                                        { kApStep + 4000, 0x90, 91, 100 } };
            spRender(*s, ev, 4 * kApStep, kBlock, L, R, 120.0);
            const int on = spV4Onset(L, kApStep + 4200, 2 * kApStep + 2000);
            CHECK(on >= 2 * kApStep - 200 && on <= 2 * kApStep + 300,
                  "a note-on joining a NON-EMPTY set never moves the position: the next "
                  "onset is still the grid's (%d, grid at %d)", on, 2 * kApStep);
        }
    }
    // RETRIGGER = 0 is free-run: the pattern position is a pure function of the
    // clock and no note-on ever moves it. This is the "locked to the bar line"
    // setting.
    {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (s) {
            s->setParam(kApRetrig, 0.f);
            const int at = kApStep + 3000;
            std::vector<SpEvent> ev = { { at, 0x90, 84, 100 } };
            spRender(*s, ev, 4 * kApStep, kBlock, L, R, 120.0);
            CHECK(spV3Peak(L, at, kApStep - 3200) < 0.02f,
                  "Retrig 0: pressing between grid lines sounds nothing until the grid "
                  "gets there (%.4f)", (double)spV3Peak(L, at, kApStep - 3200));
            const int on = spV4Onset(L, at, 3 * kApStep);
            CHECK(on >= 2 * kApStep - 200 && on <= 2 * kApStep + 300,
                  "...and it rejoins the grid wherever the grid has got to (%d)", on);
        }
    }

    // HOLD = 1 LATCHES. Press a chord, release it, the arp keeps running.
    {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (s) {
            s->setParam(kApHold, 1.f);
            std::vector<SpEvent> ev = { { 0, 0x90, 84, 100 }, { 1, 0x90, 91, 100 },
                                        { 500, 0x80, 84, 0 }, { 600, 0x80, 91, 0 } };
            spRender(*s, ev, 6 * kApStep, kBlock, L, R, 120.0);
            const std::vector<int> m = spV4Melody(L, 6);
            CHECK(spV4Eq(m, { 84, 91, 84, 91, 84, 91 }),
                  "Hold keeps the set playing after every key is released, and note-offs "
                  "never remove from the latch (got %s)", spV4Show(m).c_str());
        }
    }
    // ...and A NEW CHORD REPLACES THE LATCH: press one new note and the latch
    // becomes that note alone.
    {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (s) {
            s->setParam(kApHold, 1.f);
            s->setParam(kApRetrig, 0.f);
            std::vector<SpEvent> ev = { { 0, 0x90, 84, 100 }, { 1, 0x90, 91, 100 },
                                        { 500, 0x80, 84, 0 }, { 600, 0x80, 91, 0 },
                                        { 3 * kApStep + 100, 0x90, 79, 100 },
                                        { 3 * kApStep + 300, 0x80, 79, 0 } };
            spRender(*s, ev, 7 * kApStep, kBlock, L, R, 120.0);
            const std::vector<int> m = spV4Melody(L, 7);
            CHECK(m[0] == 84 && m[1] == 91 && m[2] == 84 &&
                  m[4] == 79 && m[5] == 79 && m[6] == 79,
                  "a new chord CLEARS the latch first, so one new note becomes the whole "
                  "of it (got %s)", spV4Show(m).c_str());
        }
    }
    // Hold 1 -> 0 drops the latch, and the arp immediately plays heldSet —
    // which here is empty, so it goes quiet after the sounding note's gate. AND
    // IT EMITS NO NOTE-ON: a parameter change may STOP notes and may never
    // START them.
    {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (s) {
            s->setParam(kApHold, 1.f);
            std::vector<SpEvent> ev = { { 0, 0x90, 84, 100 }, { 500, 0x80, 84, 0 } };
            std::vector<f32> bl((size_t)kBlock), br((size_t)kBlock);
            L.assign((size_t)(6 * kApStep), 0.f);
            size_t next = 0;
            for (int i = 0; i < 6 * kApStep; i += kBlock) {
                if (i >= 3 * kApStep) s->setParam(kApHold, 0.f);
                while (next < ev.size() && ev[next].frame < i + kBlock) {
                    const u8 m[3] = { ev[next].st, ev[next].a, ev[next].b };
                    s->midi(m, 3, ev[next].frame - i);
                    ++next;
                }
                s->setTransport(120.0, 0.0, true);
                std::fill(bl.begin(), bl.end(), 0.f);
                std::fill(br.begin(), br.end(), 0.f);
                f32* o[2] = { bl.data(), br.data() };
                s->process(nullptr, o, 2, kBlock);
                for (int j = 0; j < kBlock && i + j < 6 * kApStep; ++j)
                    L[(size_t)(i + j)] = bl[(size_t)j];
            }
            CHECK(spV3Peak(L, kApStep, kApStep) > 0.05f, "the latch was running");
            CHECK(spV3Peak(L, 4 * kApStep, 2 * kApStep) < 0.02f,
                  "Hold 1 -> 0 drops the latch and the arp goes quiet; it does not start "
                  "a note the player is not holding (%.4f)",
                  (double)spV3Peak(L, 4 * kApStep, 2 * kApStep));
        }
    }

    // CC 123 empties heldSet — it already did — AND latchSet, so a panic a
    // latch could outlive is not a panic.
    {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (s) {
            s->setParam(kApHold, 1.f);
            std::vector<SpEvent> ev = { { 0, 0x90, 84, 100 }, { 500, 0x80, 84, 0 },
                                        { 3 * kApStep + 100, 0xB0, 123, 0 } };
            spRender(*s, ev, 7 * kApStep, kBlock, L, R, 120.0);
            CHECK(spV3Peak(L, kApStep, kApStep) > 0.05f, "the latch was running");
            CHECK(spV3Peak(L, 5 * kApStep, 2 * kApStep) < 0.001f,
                  "CC 123 empties the LATCH as well as the held set, and the arp emits "
                  "nothing until a note-on arrives (%.6f)",
                  (double)spV3Peak(L, 5 * kApStep, 2 * kApStep));
        }
    }
    // CC 120 does the same and additionally clears the arp's sounding-note
    // bookkeeping, since the voices it referred to are gone.
    {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (s) {
            s->setParam(kApHold, 1.f);
            std::vector<SpEvent> ev = { { 0, 0x90, 84, 100 }, { 500, 0x80, 84, 0 },
                                        { 3 * kApStep + 100, 0xB0, 120, 0 } };
            spRender(*s, ev, 7 * kApStep, kBlock, L, R, 120.0);
            CHECK(spV3Peak(L, 4 * kApStep, 3 * kApStep) == 0.f,
                  "CC 120 is a hard stop and the arp does not resume (%.9f)",
                  (double)spV3Peak(L, 4 * kApStep, 3 * kApStep));
        }
    }
}

// ---------------------------------------------------------------------------

static void testSpectraV4Transitions(PluginRegistry& reg) {
    banner("Spectra v4: Arp On both ways — a parameter change may STOP notes and may never START them");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;

    // A render that flips Arp On at a block boundary. Both transitions are
    // block-granular BY CONSTRUCTION — parameters are not events in this device
    // and never have been — which is why the bit-identity gate is stated over
    // renders whose arp parameters are not automated mid-render.
    auto flip = [&](f32 from, f32 to, int at, int frames, std::vector<f32>& L) {
        auto s = spV4Probe(reg, *d);
        if (!s) return false;
        s->setParam(kApOn, from);
        std::vector<f32> bl((size_t)kBlock), br((size_t)kBlock);
        L.assign((size_t)frames, 0.f);
        const u8 on[3] = { 0x90, 84, 100 };
        bool sent = false;
        for (int i = 0; i < frames; i += kBlock) {
            if (i >= at) s->setParam(kApOn, to);
            if (!sent) { s->midi(on, 3, 0); sent = true; }
            s->setTransport(120.0, 0.0, true);
            std::fill(bl.begin(), bl.end(), 0.f);
            std::fill(br.begin(), br.end(), 0.f);
            f32* o[2] = { bl.data(), br.data() };
            s->process(nullptr, o, 2, kBlock);
            for (int j = 0; j < kBlock && i + j < frames; ++j) L[(size_t)(i + j)] = bl[(size_t)j];
        }
        return true;
    };

    // 0 -> 1 WITH NOTES HELD. Every voice sounding from a direct note-on is
    // RELEASED (an ENV release, not a cut) at frame 0; the held stack is
    // untouched, so the arp starts from the truth and begins at the next onset.
    {
        std::vector<f32> L;
        if (flip(0.f, 1.f, 2 * kApStep, 6 * kApStep, L)) {
            CHECK(spV3Peak(L, kApStep, 1000) > 0.05f, "the directly-played note was sounding");
            CHECK(spV3Peak(L, 2 * kApStep + 2000, 1000) < 0.02f,
                  "Arp On 0 -> 1 releases it (%.4f)",
                  (double)spV3Peak(L, 2 * kApStep + 2000, 1000));
            CHECK(spV3Peak(L, 3 * kApStep, kApStep / 2) > 0.05f,
                  "...and the arp then plays the still-held key from the next onset");
        }
    }
    // 1 -> 0 WITH NOTES HELD. Every note the arp generated is released and the
    // held key does NOT re-sound: a key that was never delivered to the voice
    // engine cannot be resumed without synthesising a note-on the player did
    // not play. The arp may invent MIDI; a knob may not. The player re-presses.
    {
        std::vector<f32> L;
        if (flip(1.f, 0.f, 2 * kApStep, 6 * kApStep, L)) {
            CHECK(spV3Peak(L, 500, 1000) > 0.05f, "the arp was running");
            CHECK(spV3Peak(L, 3 * kApStep, 3 * kApStep) < 0.02f,
                  "Arp On 1 -> 0 releases the generated notes and does NOT resume the "
                  "held key (%.4f)", (double)spV3Peak(L, 3 * kApStep, 3 * kApStep));
        }
    }
    // ...and the mono note-off fallback returns the instant Arp On goes to 0,
    // while it is DISABLED with the arp on: a fallback would sound a note the
    // arp did not schedule, which is inventing MIDI at a note-off.
    {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (s) {
            s->setParam(spIdx(*s, "Voice Mode"), 1.f);     // Mono
            s->setParam(kApGate, 30.f);
            s->setParam(kApMode, 7.f);                     // Chord: one note per step
            std::vector<SpEvent> ev = { { 0, 0x90, 84, 100 },
                                        { 100, 0x90, 91, 100 },
                                        { kApStep + 6000, 0x80, 91, 0 } };
            spRender(*s, ev, 3 * kApStep, kBlock, L, R, 120.0);
            // The note-off lands inside the gap between two steps. With the
            // fallback live it would sound 84 there; with the arp on nothing
            // may start.
            CHECK(spV3Peak(L, kApStep + 6200, 2000) < 0.02f,
                  "with the arp on, an incoming note-off starts nothing (%.4f)",
                  (double)spV3Peak(L, kApStep + 6200, 2000));
        }
    }
}

// ---------------------------------------------------------------------------

static void testSpectraV4Chance(PluginRegistry& reg) {
    banner("Spectra v4: Chance and Random are pure functions of a stable identity");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;

    auto melody = [&](int chance, int mode, std::initializer_list<u8> notes, int steps) {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (!s) return std::vector<int>{};
        s->setParam(kApChance, (f32)chance);
        s->setParam(kApMode, (f32)mode);
        spRender(*s, spV4Chord(notes), steps * kApStep, kBlock, L, R, 120.0);
        return spV4Melody(L, steps);
    };

    // C = 100 always passes and C = 0 never does: the draw lands in 0..99.
    {
        const std::vector<int> none = melody(0, 0, { 72, 76, 79 }, 8);
        bool silent = true;
        for (int v : none) if (v != -1) silent = false;
        CHECK(silent, "Chance 0 never sounds a step (got %s)", spV4Show(none).c_str());
    }
    // A DROPPED DRAW IS SILENT BUT STILL ADVANCES EVERY INDEX. This is the
    // load-bearing resolution: the surviving steps play exactly the notes they
    // would have played at Chance 100, at exactly the same k.
    {
        const std::vector<int> full = melody(100, 0, { 72, 76, 79 }, 16);
        const std::vector<int> half = melody(50, 0, { 72, 76, 79 }, 16);
        int dropped = 0, moved = 0;
        for (size_t i = 0; i < half.size(); ++i) {
            if (half[i] == -1) ++dropped;
            else if (half[i] != full[i]) ++moved;
        }
        CHECK(dropped > 2 && dropped < 14,
              "Chance 50 drops some steps and not all (%d of 16)", dropped);
        CHECK(moved == 0,
              "and every surviving step plays the note its k owns — Chance drops notes, "
              "it does not renumber the melody (%d moved)\n         100%%: %s\n          50%%: %s",
              moved, spV4Show(full).c_str(), spV4Show(half).c_str());
    }
    // A TIE FOLLOWING A DROPPED STEP IS SILENT, because there is no previous
    // note to hold.
    {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (s) {
            s->setParam(kApChance, 0.f);
            s->setStateString("nxspc1;arps=05151515151515151515151515151515");
            spRender(*s, spV4Chord({ 84 }), 4 * kApStep, kBlock, L, R, 120.0);
            CHECK(spV3Peak(L, 0, 4 * kApStep) < 0.001f,
                  "with every draw lost, the ties after them hold nothing (%.6f)",
                  (double)spV3Peak(L, 0, 4 * kApStep));
        }
    }

    // RANDOM IS A DRAW, NOT A WALK: its cycle length is still c, so the octave
    // axis and the loop counter advance exactly as they do under Up, and every
    // element it draws is in the set.
    {
        const std::vector<int> m = melody(100, 6, { 72, 76, 79 }, 16);
        bool inSet = true, varied = false;
        for (size_t i = 0; i < m.size(); ++i) {
            if (m[i] != 72 && m[i] != 76 && m[i] != 79) inSet = false;
            if (i && m[i] != m[0]) varied = true;
        }
        CHECK(inSet, "every Random draw lands inside the note set (got %s)",
              spV4Show(m).c_str());
        CHECK(varied, "...and it is a draw and not a constant (%s)", spV4Show(m).c_str());
    }

    // THE SET IS HASHED ASCENDING, DELIBERATELY, even in As Played mode.
    // Playing C-E-G and playing G-E-C are the same chord, and a random pattern
    // that changed because a player rolled the chord the other way would be a
    // bug the player could hear and never explain. Two renders of the same
    // notes in different play orders must be BYTE-IDENTICAL under Random and
    // under Chance.
    auto rolled = [&](int mode, int chance, bool up, std::vector<f32>& L) {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> R;
        if (!s) return false;
        s->setParam(kApMode, (f32)mode);
        s->setParam(kApChance, (f32)chance);
        // ALL THREE ON THE SAME FRAME. The order they are handed to midi() is
        // the order they enter the held stack, so the two renders differ in
        // insertion order and in nothing else; pressing them a sample apart
        // would make step 0 fire against a set of ONE note, which is a
        // different chord and correctly a different render.
        std::vector<SpEvent> ev;
        if (up) ev = { { 0, 0x90, 72, 100 }, { 0, 0x90, 76, 100 }, { 0, 0x90, 79, 100 } };
        else    ev = { { 0, 0x90, 79, 100 }, { 0, 0x90, 76, 100 }, { 0, 0x90, 72, 100 } };
        spRender(*s, ev, 12 * kApStep, kBlock, L, R, 120.0);
        return true;
    };
    {
        std::vector<f32> a, b;
        if (rolled(6, 100, true, a) && rolled(6, 100, false, b)) {
            CHECK(spV3Peak(a, 0, (int)a.size()) > 0.05f, "the Random render is not silent");
            CHECK(spV2MaxDiff(a, b) == 0.f,
                  "Random mode: a chord rolled UP and the same chord rolled DOWN render "
                  "bit-identically — the set is folded ASCENDING and the play order "
                  "cannot reach the hash (max diff %.9f)", (double)spV2MaxDiff(a, b));
        }
    }
    {
        std::vector<f32> a, b;
        if (rolled(0, 45, true, a) && rolled(0, 45, false, b)) {
            CHECK(spV3Peak(a, 0, (int)a.size()) > 0.05f, "the Chance render is not silent");
            CHECK(spV2MaxDiff(a, b) == 0.f,
                  "Chance: the same chord in either play order drops the same steps "
                  "(max diff %.9f)", (double)spV2MaxDiff(a, b));
        }
    }
    // ...and the two draws are INDEPENDENT: salt 1 for the note and salt 2 for
    // the chance, so a Random pattern at Chance 100 and the same pattern at
    // Chance 50 agree wherever the second one sounds.
    {
        auto full = melody(100, 6, { 72, 76, 79 }, 16);
        auto part = melody(50, 6, { 72, 76, 79 }, 16);
        int moved = 0;
        for (size_t i = 0; i < part.size(); ++i)
            if (part[i] != -1 && part[i] != full[i]) ++moved;
        CHECK(moved == 0,
              "the note draw and the chance draw are salted apart: turning Chance down "
              "does not change WHICH note a surviving step plays (%d moved)", moved);
    }
}

// ---------------------------------------------------------------------------

static void testSpectraV4Source17(PluginRegistry& reg) {
    banner("Spectra v4: Arp Step as a modulation source (17)");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;

    // v3's voltmeter, verbatim: A Level from a base of 0, driven by one slot at
    // amount 1, so the local peak of the output IS the source.
    //
    // ONE THING HAS TO CHANGE FOR v4 and it is worth stating: with the arp on,
    // an incoming note-on never reaches the voices, so the voltmeter's carrier
    // has to be a note THE ARP GENERATED. Gate 200 % makes one sound
    // continuously, which is what lets a rest and a tie be read at all — the
    // staircase is instance-wide and has to be audible while the arp is not
    // starting anything.
    auto probe = [&](const char* rows, const char* lvl, int chance, int steps,
                     std::vector<f32>& L) {
        auto s = spV3Probe(reg, *d);
        std::vector<f32> R;
        if (!s) return false;
        spV3Slot(*s, 0, 17, 5, 1.f);                 // source 17 -> A Level
        s->setParam(kApOn, 1.f);
        s->setParam(kApSync, (f32)kApSyncEigh);
        s->setParam(kApGate, 200.f);
        s->setParam(kApChance, (f32)chance);
        std::string st = std::string("nxspc1;arpl=") + lvl;
        if (rows) st += std::string(";arps=") + rows;
        s->setStateString(st);
        spRender(*s, spV3Note(0, 84), steps * kApStep, kBlock, L, R, 120.0);
        return true;
    };

    // The level row read out one step at a time, as d/15.
    {
        std::vector<f32> L;
        if (probe(nullptr, "0f8fffffffffffff", 100, 6, L)) {
            const f32 s0 = spV3Peak(L, 3000, 4000);
            const f32 s1 = spV3Peak(L, kApStep + 3000, 4000);
            const f32 s2 = spV3Peak(L, 2 * kApStep + 3000, 4000);
            const f32 s3 = spV3Peak(L, 3 * kApStep + 3000, 4000);
            CHECK(s1 > 0.05f, "the source-17 voltmeter reads something (%.4f)", (double)s1);
            CHECK(s0 < 0.01f * s1, "step 0 (digit 0) reads 0 (%.4f)", (double)(s0 / s1));
            CHECK(std::fabs(s2 / s1 - 8.f / 15.f) < 0.06f,
                  "step 2 (digit 8) reads 8/15 = %.3f (%.3f)", 8.0 / 15.0,
                  (double)(s2 / s1));
            CHECK(std::fabs(s3 / s1 - 1.f) < 0.06f, "step 3 (digit f) reads 1.0 (%.3f)",
                  (double)(s3 / s1));
        }
    }
    // IT FOLLOWS THE STEP CLOCK, NOT THE NOTES. An OFF step and a tie both
    // leave it reading the grid's level at the current index — a staircase
    // that dropped to zero every rest would be a different and much worse
    // control. Gate 200 % keeps step 0's note sounding across both.
    {
        std::vector<f32> L;
        //  step 0 on · step 1 TIE · step 2 REST · step 3 on. The tie is what
        //  keeps the carrier alive across both, which is exactly the reading
        //  the contract's own off(k) = onset(k+m) + gate gives.
        if (probe("05150405050505050505050505050505", "f84cffffffffffff", 100, 6, L)) {
            const f32 s0 = spV3Peak(L, 3000, 4000);
            const f32 s1 = spV3Peak(L, kApStep + 3000, 4000);
            const f32 s2 = spV3Peak(L, 2 * kApStep + 3000, 4000);
            const f32 s3 = spV3Peak(L, 3 * kApStep + 3000, 4000);
            CHECK(s0 > 0.05f, "the carrier note is sounding (%.4f)", (double)s0);
            CHECK(std::fabs(s1 / s0 - 8.f / 15.f) < 0.06f,
                  "a TIE leaves it reading the level at that index (%.3f)", (double)(s1 / s0));
            CHECK(std::fabs(s2 / s0 - 4.f / 15.f) < 0.06f,
                  "a REST leaves it reading the level at that index (%.3f)", (double)(s2 / s0));
            CHECK(std::fabs(s3 / s0 - 12.f / 15.f) < 0.06f,
                  "and step 3's own digit follows immediately after (%.3f)", (double)(s3 / s0));
        }
    }
    // AN EMPTY NOTE SET LEAVES IT RUNNING TOO. The arp sounds nothing for four
    // steps and then a key arrives — and the level it reads is the level the
    // CLOCK implies, not the level a staircase that had been waiting would
    // read. This is the same "nothing accumulates" property the melody has.
    {
        auto s = spV3Probe(reg, *d);
        std::vector<f32> L, R;
        if (s) {
            spV3Slot(*s, 0, 17, 5, 1.f);
            s->setParam(kApOn, 1.f);
            s->setParam(kApSync, (f32)kApSyncEigh);
            s->setParam(kApGate, 200.f);
            s->setParam(kApRetrig, 0.f);              // the grid owns the phrase
            s->setStateString("nxspc1;arpl=ffff4fffffffffff");   // digit 4 at step 4
            // The key arrives in the middle of step 3, and with Retrig 0 it
            // moves nothing: the arp first sounds at step 4's own onset.
            spRender(*s, spV3Note(3 * kApStep + 6000, 84), 7 * kApStep, kBlock, L, R, 120.0);
            const f32 s4 = spV3Peak(L, 4 * kApStep + 3000, 4000);
            const f32 s5 = spV3Peak(L, 5 * kApStep + 3000, 4000);
            CHECK(s5 > 0.05f, "the arp sounds once a key arrives (%.4f)", (double)s5);
            CHECK(std::fabs(s4 / s5 - 4.f / 15.f) < 0.08f,
                  "the staircase kept walking through four steps with no notes in the "
                  "set: step 4 reads digit 4 and not digit 0 (%.3f)", (double)(s4 / s5));
        }
    }
    // IT IS 0 WHENEVER ARP ON IS 0, which is the inert condition the
    // bit-identity gate rests on: a slot pointed at source 17 with the arp off
    // contributes exactly nothing, which is v3's contribution of exactly
    // nothing.
    {
        auto off = spV3Probe(reg, *d);
        auto ctl = spV3Probe(reg, *d);
        std::vector<f32> aL, aR, bL, bR;
        if (off && ctl) {
            spV3Slot(*off, 0, 17, 5, 1.f);        // source 17, arp OFF
            off->setStateString("nxspc1;arpl=0123456789abcdef");
            spRender(*off, spV3Note(0, 84), 3 * kApStep, kBlock, aL, aR, 120.0);
            spRender(*ctl, spV3Note(0, 84), 3 * kApStep, kBlock, bL, bR, 120.0);
            CHECK(spV3Peak(aL, 0, (int)aL.size()) == 0.f,
                  "with Arp On 0, a slot on source 17 contributes exactly 0 (%.9f)",
                  (double)spV3Peak(aL, 0, (int)aL.size()));
            CHECK(spV2MaxDiff(aL, bL) == 0.f,
                  "...bit-identically to the same patch with no slot at all");
        }
    }
}

// ---------------------------------------------------------------------------

static void spV4BusyPatch(PluginInstance& s) {
    // The v3 busy patch plus a dense arp: nine modes are swept by the caller,
    // and everything the arp owns is off its default — swing, a gate over
    // 100 %, octave alternation, a pattern with rests and ties in it, a chance
    // below 100, a shortened pattern and Pattern velocity.
    spV3BusyPatch(s);
    s.setParam(kApOn, 1.f);
    s.setParam(kApSync, 7.f);              // 1/16
    s.setParam(kApOctaves, 3.f);
    s.setParam(kApOctMode, 2.f);           // Alternate
    s.setParam(kApGate, 155.f);            // over 100 %
    s.setParam(kApSwing, 62.f);
    s.setParam(kApVelMode, 2.f);           // Pattern
    s.setParam(kApSteps, 13.f);            // an ODD length, so the swing must
                                           // stay welded to k and not to i
    s.setParam(kApChance, 78.f);
    s.setStateString("nxspc1;arpl=f8c4fac6f8c4fac6;arps=05150705040915150507051504050f05");
}

static std::vector<SpEvent> spV4Script(int frames) {
    // A rolled chord, a finger added and removed mid-phrase, a second chord,
    // controller traffic that is never on a block boundary at any of the sizes
    // below, and a panic near the end.
    std::vector<SpEvent> ev = {
        {     0, 0x90, 45,  90 },
        {   401, 0x90, 52, 110 },
        {  1103, 0x90, 57,  40 },
        {  3307, 0x90, 64,  70 },
        {  9001, 0x80, 52,   0 },
        { 15013, 0x90, 61, 120 },
        { 21107, 0x80, 57,   0 },
        { 27011, 0x80, 45,   0 },
        { 27013, 0x80, 61,   0 },
        { 27017, 0x80, 64,   0 },
        { 29009, 0x90, 48, 100 },
        { 29011, 0x90, 55,  60 },
        { 29013, 0x90, 60,  80 },
    };
    ev.push_back({ 3000, 0xD0, 96, 0 });
    for (int f = 137; f < frames; f += 379)
        ev.push_back({ f, 0xB0, 1, (u8)((f / 379 * 13) & 0x7F) });
    for (int f = 89; f < frames; f += 293) {
        const int v14 = (f * 37) & 0x3FFF;
        ev.push_back({ f, 0xE0, (u8)(v14 & 0x7F), (u8)((v14 >> 7) & 0x7F) });
    }
    std::sort(ev.begin(), ev.end(),
              [](const SpEvent& a, const SpEvent& b) { return a.frame < b.frame; });
    return ev;
}

static void testSpectraV4Determinism(PluginRegistry& reg) {
    banner("Spectra v4: block-size invariance with the arp RUNNING — the acid test");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;

    const int kFrames = 40000;
    const std::vector<SpEvent> ev = spV4Script(kFrames);

    // THE ACID TEST. An arp that emitted at the top of the block would quantise
    // every generated note to the block boundary, and the same MIDI in blocks
    // of 1 and of 1024 would then produce different audio — the exact failure
    // the note queue was built to prevent, arriving through a new door. Every
    // mode, in Poly and in Legato, at five block sizes.
    int allOk = 0, allRun = 0;
    for (int mode = 0; mode < 10; ++mode) {
        for (int vm = 0; vm < 2; ++vm) {
            auto build = [&]() {
                auto s = reg.instantiate(*d, kSR, kBlock);
                if (s) {
                    spV4BusyPatch(*s);
                    s->setParam(kApMode, (f32)mode);
                    s->setParam(spIdx(*s, "Voice Mode"), vm ? 2.f : 0.f);
                    s->setParam(spIdx(*s, "Glide"), vm ? 40.f : 0.f);
                }
                return s;
            };
            auto ref = build();
            if (!ref) return;
            std::vector<f32> refL, refR, altL, altR;
            spRender(*ref, ev, kFrames, kBlock, refL, refR, 120.0);
            if (spV3Peak(refL, 0, kFrames) <= 0.02f) {
                CHECK(false, "mode %d (%s): the arp render is silent", mode, vm ? "Legato" : "Poly");
                continue;
            }
            bool ok = true;
            for (int chunk : { 1, 7, 64, 300, 1024 }) {
                auto alt = build();
                if (!alt) break;
                spRender(*alt, ev, kFrames, chunk, altL, altR, 120.0);
                if (std::fmax(spV2MaxDiff(refL, altL), spV2MaxDiff(refR, altR)) != 0.f) {
                    ok = false;
                    CHECK(false, "mode %d (%s): blocks of %d differ from %d (max diff %.9f)",
                          mode, vm ? "Legato" : "Poly", chunk, kBlock,
                          (double)std::fmax(spV2MaxDiff(refL, altL), spV2MaxDiff(refR, altR)));
                }
            }
            ++allRun;
            if (ok) ++allOk;
        }
    }
    CHECK(allOk == allRun && allRun == 20,
          "all nine modes plus Chord, in Poly and Legato, are bit-identical at blocks of "
          "1, 7, 64, 300 and 1024 — generated note-ons and note-offs land at STAMPED "
          "SAMPLES (%d of %d)", allOk, allRun);

    // TWO RENDERS OF THE SAME PROJECT ARE BYTE-IDENTICAL, and it is a gate.
    // Random and Chance are splitmix64 over a stable identity — never a stream,
    // never a clock — so no RNG state is carried between steps and voice
    // stealing cannot perturb either.
    {
        auto build = [&]() {
            auto s = reg.instantiate(*d, kSR, kBlock);
            if (s) { spV4BusyPatch(*s); s->setParam(kApMode, 6.f); }   // Random
            return s;
        };
        auto a = build();
        auto b = build();
        auto c = build();
        std::vector<f32> aL, aR, bL, bR, cL, cR;
        if (a && b && c) {
            spRender(*a, ev, kFrames, kBlock, aL, aR, 120.0);
            spRender(*b, ev, kFrames, kBlock, bL, bR, 120.0);
            spRender(*c, ev, kFrames, 97, cL, cR, 120.0);
            CHECK(spV3Peak(aL, 0, kFrames) > 0.02f, "the Random busy render is not silent");
            CHECK(spV2MaxDiff(aL, bL) == 0.f && spV2MaxDiff(aR, bR) == 0.f,
                  "two fresh instances render the same arp project identically");
            CHECK(spV2MaxDiff(aL, cL) == 0.f,
                  "...and so does a third at an irregular block size");
        }
    }

    // The arp NEVER ENTERS THE EVENT QUEUE, so no density of arp output can
    // push an incoming note-off out of it. A 1/16 arp at 999 bpm generating for
    // forty thousand samples, with a note-off at the very end, must leave
    // nothing sounding.
    {
        auto s = reg.instantiate(*d, kSR, kBlock);
        if (s) {
            spV4BusyPatch(*s);
            s->setParam(kApMode, 7.f);            // Chord: the densest output
            s->setParam(kApGate, 200.f);
            s->setParam(kApChance, 100.f);
            std::vector<SpEvent> e2 = { { 0, 0x90, 60, 100 }, { 1, 0x90, 64, 100 },
                                        { 2, 0x90, 67, 100 },
                                        { 20000, 0xB0, 123, 0 } };
            std::vector<f32> L, R;
            spRender(*s, e2, 60000, kBlock, L, R, 999.0);
            CHECK(spV3Peak(L, 0, 20000) > 0.02f, "the dense arp was generating");
            CHECK(spV3Peak(L, 40000, 20000) < 0.001f,
                  "an incoming All Notes Off is still honoured under the densest arp "
                  "output this device can make (%.6f)",
                  (double)spV3Peak(L, 40000, 20000));
        }
    }
}

// ---------------------------------------------------------------------------

static void testSpectraV4BeatLock(PluginRegistry& reg) {
    banner("Spectra v4: the step clock is the ORCHESTRATOR RULING's second consumer");

    const PluginDesc* d = reg.find("nxtakt:spectra");
    if (!d) return;

    // A LOCATE LANDS ON THE RIGHT STEP, without the arp having run there. This
    // is what "every index is a modulus of k" buys, and it cannot be had from a
    // counter that advances per step.
    //
    // Sync 5 is 1/4, so at 120 bpm a step is one beat. A render that STARTS at
    // beat 5.0 with a four-step pattern is on pattern index 1, and Up over
    // three notes is on note index 5 mod 3 = 2 — the TOP note. A render from
    // beat 0 is on index 0, the bottom one.
    auto at = [&](f64 beat0) {
        auto s = spV4Probe(reg, *d);
        std::vector<f32> L, R;
        if (!s) return std::vector<int>{};
        s->setParam(kApSync, 5.f);              // 1/4: one step = one beat
        s->setParam(kApRetrig, 0.f);            // the grid owns the phrase
        s->setParam(kApGate, 60.f);
        spRenderBeat(*s, spV4Chord({ 72, 76, 79 }), 4 * 24000, kBlock, L, R, 120.0, beat0);
        std::vector<int> m;
        for (int k = 0; k < 4; ++k) m.push_back(spV4NoteIn(L, k * 24000 + 3000, 6000));
        return m;
    };
    {
        const std::vector<int> z = at(0.0), f = at(5.0);
        CHECK(spV4Eq(z, { 72, 76, 79, 72 }),
              "from beat 0 the arp starts on note index 0 (got %s)", spV4Show(z).c_str());
        CHECK(spV4Eq(f, { 79, 72, 76, 79 }),
              "from BEAT 5 it starts on 5 mod 3 = index 2 — the locate lands on the step "
              "the bar implies, without the arp having run there (got %s)",
              spV4Show(f).c_str());
    }

    // ...and the lock is still block-size invariant, because the beat ANCHORS
    // the counter and never drives it. A phase read once per block would
    // quantise the render.
    {
        auto mk = [&]() {
            auto s = spV4Probe(reg, *d);
            if (s) {
                s->setParam(kApSync, 7.f);
                s->setParam(kApRetrig, 0.f);
                s->setParam(kApSwing, 55.f);
                s->setParam(kApGate, 140.f);
                s->setParam(kApMode, 3.f);
            }
            return s;
        };
        auto ref = mk();
        std::vector<f32> rL, rR, aL, aR;
        if (ref) {
            spRenderBeat(*ref, spV4Chord({ 60, 64, 67, 71 }), 40000, kBlock, rL, rR, 132.0, 2.0);
            CHECK(spV3Peak(rL, 0, 40000) > 0.02f, "the beat-locked arp render is not silent");
            bool all = true;
            for (int chunk : { 1, 7, 64, 300, 1024 }) {
                auto alt = mk();
                if (!alt) break;
                spRenderBeat(*alt, spV4Chord({ 60, 64, 67, 71 }), 40000, chunk, aL, aR, 132.0, 2.0);
                const f32 diff = spV2MaxDiff(rL, aL);
                if (diff != 0.f) all = false;
                CHECK(diff == 0.f, "beat-locked arp: blocks of %d bit-identical to %d "
                                   "(max diff %.9f)", chunk, kBlock, (double)diff);
            }
            CHECK(all, "the arp adds nothing to the ruling and takes nothing from it: it "
                       "reads beatAcc_ and divides by the beats in a step");
        }
    }
}

// ---------------------------------------------------------------------------
// Sampler
//
// The parameter list is the contract (src/plugin/sampler.cpp's table): ids are
// indices and a saved set stores them, so this transcription is what stops a
// careless reorder. Everything else here is measured rather than asserted --
// the interpolation section in particular prints decibels, because "a repitching
// sampler needs better than linear" is a claim that only means something with a
// number attached.
//
// NOTE ON THIS SUITE'S BUILD. internal_device_test deliberately does NOT link
// src/audio/sample.cpp -- no libsndfile, no libsamplerate -- so the sampler's
// weak reference to loadSample() does not bind here and loadFile() answers
// false for every path. That is not a gap in the coverage, it is one of the
// cases: the device has to be silent and honest in a process with no decoder,
// and the tests below check exactly that. Everything that needs actual audio
// goes in through SamplerControl::adopt(), which takes a buffer the test built
// itself and is the same door a decoded file comes through.
// ---------------------------------------------------------------------------

static const char* kSamplerParamNames[] = {
    "Root Note", "Coarse", "Fine",
    "Start", "End", "Loop", "Crossfade",
    "Gate",
    "Attack", "Decay", "Sustain", "Release",
    "Cutoff", "Resonance", "Env>Cutoff", "Keytrack",
    "Vel>Amp", "Glide", "Voices", "Master",
};
static constexpr int kSamplerContractN =
    (int)(sizeof kSamplerParamNames / sizeof kSamplerParamNames[0]);

static const char* kSamplerPresetNames[] = {
    "Init", "One-Shot", "Pitched Loop", "303 Glide", "Soft Pad", "Drum Tight",
};
static constexpr int kSamplerPresetN =
    (int)(sizeof kSamplerPresetNames / sizeof kSamplerPresetNames[0]);

static int smIdx(const PluginInstance& p, const char* name) { return paramIndex(p, name); }

// A pure sine, in f64 and then rounded once, so the buffer is as close to the
// ideal signal as a float array gets. That matters: the interpolation test
// measures the distance between what the device produced and a perfect tone,
// and a sloppy source would put its own error into that number.
static SampleRef smSine(f64 hz, i64 frames, int ch = 1, f32 amp = 0.5f, f64 rate = kSR) {
    auto sb = std::make_shared<SampleBuffer>();
    sb->channels = ch;
    sb->frames   = frames;
    sb->rate     = rate;
    sb->data.assign((size_t)(frames * (i64)ch), 0.f);
    for (i64 i = 0; i < frames; ++i) {
        const f32 v = (f32)((f64)amp * std::sin(6.283185307179586 * hz * (f64)i / rate));
        for (int c = 0; c < ch; ++c) sb->data[(size_t)(i * ch + c)] = v;
    }
    return sb;
}

// A linear ramp from 0 to `amp`. A POSITION PROBE: the value coming out names
// the frame it was read from, so start points, end points, loop wraps and
// polyphony can all be checked by looking at a number instead of at a spectrum.
// The lowpass passes DC at unity, so the mean of a window is the mean position.
static SampleRef smRamp(i64 frames, int ch = 1, f32 amp = 0.8f) {
    auto sb = std::make_shared<SampleBuffer>();
    sb->channels = ch;
    sb->frames   = frames;
    sb->rate     = kSR;
    sb->data.assign((size_t)(frames * (i64)ch), 0.f);
    for (i64 i = 0; i < frames; ++i) {
        const f32 v = amp * (f32)((f64)i / (f64)(frames - 1));
        for (int c = 0; c < ch; ++c) sb->data[(size_t)(i * ch + c)] = v;
    }
    return sb;
}

// The measurement patch: everything that could colour the signal is flat or
// off, and the envelope settles to exactly 1 within a millisecond and stays
// there. Deliberately NOT the defaults -- the defaults are a musical starting
// point and a measurement wants a wire.
static void smFlatPatch(PluginInstance& s) {
    s.setParam(smIdx(s, "Root Note"), 69.f);
    s.setParam(smIdx(s, "Attack"),    0.1f);
    s.setParam(smIdx(s, "Decay"),     5000.f);
    s.setParam(smIdx(s, "Sustain"),   1.f);
    s.setParam(smIdx(s, "Release"),   5.f);
    s.setParam(smIdx(s, "Cutoff"),    20000.f);
    s.setParam(smIdx(s, "Resonance"), 0.f);
    s.setParam(smIdx(s, "Vel>Amp"),   0.f);
    s.setParam(smIdx(s, "Master"),    1.f);
}

static f64 smMean(const std::vector<f32>& x, int from, int n) {
    f64 acc = 0.0;
    for (int i = 0; i < n; ++i) acc += (f64)x[(size_t)(from + i)];
    return acc / (f64)n;
}

static f64 smPeak(const std::vector<f32>& x, int from, int n) {
    f64 m = 0.0;
    for (int i = 0; i < n; ++i) m = std::fmax(m, std::fabs((f64)x[(size_t)(from + i)]));
    return m;
}

// SIGNAL-TO-ERROR against the ideal tone, and the reason it is a least-squares
// FIT rather than a subtraction of a tone the test wrote itself:
//
//   everything between the interpolator and the output is LINEAR and TIME
//   INVARIANT -- the state variable filter, the settled envelope, the master
//   gain -- so all of it can do to a sine is change its amplitude and its
//   phase. Fitting A*cos + B*sin at the expected frequency absorbs exactly
//   that and nothing else, so what is left in the residual is the
//   interpolator's error and only the interpolator's error. Comparing against
//   a fixed reference tone instead would measure the filter's phase shift and
//   call it distortion.
static f64 smSnrDb(const std::vector<f32>& y, int from, int n, f64 freq) {
    const f64 w = 6.283185307179586 * freq / kSR;
    f64 suu = 0, svv = 0, suv = 0, suy = 0, svy = 0;
    for (int i = 0; i < n; ++i) {
        const f64 c = std::cos(w * (f64)i), s = std::sin(w * (f64)i);
        const f64 v = (f64)y[(size_t)(from + i)];
        suu += c * c; svv += s * s; suv += c * s; suy += c * v; svy += s * v;
    }
    const f64 det = suu * svv - suv * suv;
    if (std::fabs(det) < 1e-12) return -999.0;
    const f64 A = (svv * suy - suv * svy) / det;
    const f64 B = (suu * svy - suv * suy) / det;
    f64 sig = 0, res = 0;
    for (int i = 0; i < n; ++i) {
        const f64 f = A * std::cos(w * (f64)i) + B * std::sin(w * (f64)i);
        const f64 v = (f64)y[(size_t)(from + i)];
        sig += f * f;
        res += (v - f) * (v - f);
    }
    if (res <= 0.0) return 999.0;
    return 10.0 * std::log10(sig / res);
}

// The same read the device does, with LINEAR interpolation, so the two numbers
// in the quality section come from the same source, the same increment and the
// same measurement. This is the thing Catmull-Rom is being compared against and
// it is worth having in the suite rather than in a comment.
static void smLinearRef(const SampleBuffer& sb, f64 ratio, std::vector<f32>& out, int n) {
    out.assign((size_t)n, 0.f);
    f64 pos = 0.0;
    for (int i = 0; i < n; ++i) {
        i64 k = (i64)pos;
        if (k < 0) k = 0;
        if (k + 1 >= sb.frames) k = sb.frames - 2;
        const f32 t = (f32)(pos - (f64)k);
        const f32 a = sb.data[(size_t)k], b = sb.data[(size_t)(k + 1)];
        out[(size_t)i] = a + (b - a) * t;
        pos += ratio;
    }
}

// A patch with every path in the device moving at once: a loop with a
// crossfade, a resonant envelope-swept filter, keytracking, glide, velocity
// scaling and a polyphony cap low enough that the script below steals.
static void smBusyPatch(PluginInstance& s) {
    s.setParam(smIdx(s, "Root Note"),  60.f);
    s.setParam(smIdx(s, "Coarse"),     -5.f);
    s.setParam(smIdx(s, "Fine"),       17.f);
    s.setParam(smIdx(s, "Start"),      0.20f);
    s.setParam(smIdx(s, "End"),        0.70f);
    s.setParam(smIdx(s, "Loop"),       1.f);
    s.setParam(smIdx(s, "Crossfade"),  18.f);
    s.setParam(smIdx(s, "Gate"),       1.f);
    s.setParam(smIdx(s, "Attack"),     4.f);
    s.setParam(smIdx(s, "Decay"),      600.f);
    s.setParam(smIdx(s, "Sustain"),    0.55f);
    s.setParam(smIdx(s, "Release"),    240.f);
    s.setParam(smIdx(s, "Cutoff"),     900.f);
    s.setParam(smIdx(s, "Resonance"),  0.6f);
    s.setParam(smIdx(s, "Env>Cutoff"), 0.7f);
    s.setParam(smIdx(s, "Keytrack"),   0.45f);
    s.setParam(smIdx(s, "Vel>Amp"),    0.8f);
    s.setParam(smIdx(s, "Glide"),      55.f);
    s.setParam(smIdx(s, "Voices"),     6.f);
    s.setParam(smIdx(s, "Master"),     0.7f);
}

// ---------------------------------------------------------------------------

static void testSamplerContract(PluginRegistry& reg) {
    banner("Sampler: the descriptor and the frozen parameter table");

    const PluginDesc* d = reg.find("nxtakt:sampler");
    CHECK(d != nullptr, "registry finds nxtakt:sampler");
    if (!d) return;

    CHECK(d->format == PluginFormat::Internal && d->kind == PluginKind::Instrument,
          "descriptor: an internal INSTRUMENT");
    CHECK(d->audioIn == 0 && d->audioOut == 2,
          "0 in / 2 out (%d / %d) -- an instrument's input is silence",
          d->audioIn, d->audioOut);
    CHECK(d->hasMidiIn, "it takes MIDI");
    CHECK(d->name == "Sampler" && d->vendor == "NxTakt" && d->category == "Instrument",
          "name / vendor / category are what the browser groups on");
    CHECK(d->paramCount == kSamplerContractN,
          "the descriptor advertises %d parameters (the table says %d)",
          d->paramCount, kSamplerContractN);

    auto s = reg.instantiate(*d, kSR, kBlock);
    CHECK(s != nullptr, "instantiate + prepare");
    if (!s) return;

    CHECK(s->paramCount() == kSamplerContractN,
          "the instance has %d parameters", s->paramCount());
    CHECK(s->paramCount() <= 24,
          "and it is inside the 24 the design budgeted for a generic knob strip");
    if (s->paramCount() != kSamplerContractN) return;

    // Names, in order. THIS is the frozen part: ids are indices, so a reorder
    // silently mis-restores every set ever saved.
    bool names = true;
    for (int i = 0; i < kSamplerContractN; ++i) {
        if (s->paramInfo(i).name != kSamplerParamNames[i]) {
            CHECK(false, "parameter %d is \"%s\", the table says \"%s\"",
                  i, s->paramInfo(i).name.c_str(), kSamplerParamNames[i]);
            names = false;
        }
        if (s->paramInfo(i).id != (u32)i) {
            CHECK(false, "parameter %d carries id %u -- ids ARE indices here",
                  i, s->paramInfo(i).id);
            names = false;
        }
    }
    CHECK(names, "all %d parameters are in the table's order and carry their index "
                 "as their id", kSamplerContractN);

    // The flags a generic strip actually draws from.
    struct Row { const char* name; const char* unit; f32 mn, mx, def; bool log, isB, isI; };
    const Row kRows[] = {
        { "Root Note",  "",   0.f,    127.f,   60.f,    false, false, true  },
        { "Coarse",     "st", -24.f,  24.f,    0.f,     false, false, true  },
        { "Fine",       "ct", -100.f, 100.f,   0.f,     false, false, false },
        { "Start",      "",   0.f,    1.f,     0.f,     false, false, false },
        { "End",        "",   0.f,    1.f,     1.f,     false, false, false },
        { "Loop",       "",   0.f,    1.f,     0.f,     false, true,  false },
        { "Crossfade",  "ms", 0.f,    50.f,    5.f,     false, false, false },
        { "Gate",       "",   0.f,    1.f,     1.f,     false, true,  false },
        { "Attack",     "ms", 0.1f,   5000.f,  0.5f,    true,  false, false },
        { "Decay",      "ms", 1.f,    5000.f,  1000.f,  true,  false, false },
        { "Sustain",    "",   0.f,    1.f,     1.f,     false, false, false },
        { "Release",    "ms", 1.f,    8000.f,  40.f,    true,  false, false },
        { "Cutoff",     "Hz", 20.f,   20000.f, 20000.f, true,  false, false },
        { "Resonance",  "",   0.f,    1.f,     0.1f,    false, false, false },
        { "Env>Cutoff", "",   -1.f,   1.f,     0.f,     false, false, false },
        { "Keytrack",   "",   0.f,    1.f,     0.f,     false, false, false },
        { "Vel>Amp",    "",   0.f,    1.f,     1.f,     false, false, false },
        { "Glide",      "ms", 0.f,    500.f,   0.f,     false, false, false },
        { "Voices",     "",   1.f,    16.f,    16.f,    false, false, true  },
        { "Master",     "",   0.f,    2.f,     1.f,     false, false, false },
    };
    bool rows = true;
    for (int i = 0; i < kSamplerContractN; ++i) {
        const ParamInfo& pi = s->paramInfo(i);
        const Row& r = kRows[i];
        if (pi.unit != r.unit || pi.min != r.mn || pi.max != r.mx || pi.def != r.def ||
            pi.isLogarithmic != r.log || pi.isBool != r.isB || pi.isInt != r.isI) {
            CHECK(false, "%s: unit \"%s\" [%g, %g] def %g log=%d bool=%d int=%d -- "
                         "the table says \"%s\" [%g, %g] def %g log=%d bool=%d int=%d",
                  pi.name.c_str(), pi.unit.c_str(), (double)pi.min, (double)pi.max,
                  (double)pi.def, (int)pi.isLogarithmic, (int)pi.isBool, (int)pi.isInt,
                  r.unit, (double)r.mn, (double)r.mx, (double)r.def,
                  (int)r.log, (int)r.isB, (int)r.isI);
            rows = false;
        }
    }
    CHECK(rows, "every range, default, unit and flag matches the table -- the four "
                "envelope-ish times and the cutoff are logarithmic, Loop and Gate "
                "are toggles, Root/Coarse/Voices are stepped");

    CHECK(s->latencyFrames() == 0, "it reports no latency, because it has none");
    CHECK(s->rack() == nullptr, "it is not a container");
    CHECK(s->sampler() != nullptr, "and it DOES answer sampler(), which is how the "
                                   "GUI hands it a file");
}

// ---------------------------------------------------------------------------

static void testSamplerEmpty(PluginRegistry& reg) {
    banner("Sampler: with no sample it is silent, and says so rather than guessing");

    const PluginDesc* d = reg.find("nxtakt:sampler");
    if (!d) return;
    auto s = reg.instantiate(*d, kSR, kBlock);
    if (!s) return;
    SamplerControl* sc = s->sampler();
    CHECK(sc != nullptr, "sampler() is non-null on a fresh instance");
    if (!sc) return;

    CHECK(!sc->hasSample(), "a fresh sampler has no sample");
    CHECK(sc->samplePath().empty(), "and no path");
    CHECK(sc->sampleFrames() == 0, "and no frames");
    CHECK(s->stateString().empty(),
          "so its state string is EMPTY -- which is what makes the project layer "
          "write no `state` key and keeps a set with no sampler in it byte-identical");

    // Every note in the book, into a device with nothing to play.
    const std::vector<SpEvent> ev = {
        { 0, 0x90, 60, 100 }, { 100, 0x90, 64, 127 }, { 200, 0x80, 60, 0 },
        { 300, 0xB0, 123, 0 }, { 400, 0x90, 72, 1 }, { 500, 0xB0, 120, 0 },
    };
    std::vector<f32> L, R;
    spRender(*s, ev, 4800, kBlock, L, R);
    f64 pk = 0.0;
    bool fin = true;
    for (int i = 0; i < 4800; ++i) {
        pk = std::fmax(pk, std::fabs((f64)L[(size_t)i]));
        pk = std::fmax(pk, std::fabs((f64)R[(size_t)i]));
        if (!std::isfinite(L[(size_t)i]) || !std::isfinite(R[(size_t)i])) fin = false;
    }
    CHECK(fin, "an empty sampler renders finite audio");
    CHECK(pk == 0.0, "and it is EXACTLY silent (peak %.9f), not nearly", pk);

    // The no-decoder case, which is this binary. See the section header.
    CHECK(!sc->loadFile(""), "loadFile(\"\") is refused");
    CHECK(!sc->loadFile("/nonexistent/definitely-not-here.wav"),
          "loadFile() of a missing file is refused");
    CHECK(!sc->hasSample() && sc->samplePath().empty(),
          "and a refused load leaves the device empty rather than half-loaded");

    // Loading, then clearing, gets back to exactly the empty state.
    sc->adopt(smRamp(4800), "/tmp/probe.wav");
    CHECK(sc->hasSample() && sc->sampleFrames() == 4800, "adopt() takes a buffer in");
    sc->clearSample();
    CHECK(!sc->hasSample() && sc->samplePath().empty() && s->stateString().empty(),
          "clearSample() gets all the way back to empty");
    spRender(*s, ev, 2400, kBlock, L, R);
    CHECK(smPeak(L, 0, 2400) == 0.0, "and silent again afterwards");
    sc->reclaim();
    CHECK(true, "reclaim() frees the displaced buffers without touching the live one");

    // A buffer this device cannot play is turned into "no buffer" at the door.
    {
        auto bad = std::make_shared<SampleBuffer>();
        bad->channels = 5;
        bad->frames   = 100;
        bad->data.assign(500, 0.1f);
        sc->adopt(bad, "/tmp/fivechannel.wav");
        CHECK(!sc->hasSample(),
              "a 5-channel buffer is refused at adopt() rather than defended against "
              "on the audio thread");
    }
    {
        auto lying = std::make_shared<SampleBuffer>();
        lying->channels = 2;
        lying->frames   = 100000;          // far more than `data` can back
        lying->data.assign(64, 0.1f);
        sc->adopt(lying, "/tmp/lying.wav");
        CHECK(!sc->hasSample(),
              "so is a buffer whose frame count its own data cannot back");
    }

    // -- sampleBuffer(): the read-back half of adopt() (GUI-ON-DAEMON.md §15) --
    //
    // It exists for one caller with one problem: `src/ui/engine_handle.cpp` has
    // to put a sampler's audio into the sample pool so a daemon that links no
    // decoder can play it, and the state string names only a PATH. Everything
    // here is about the two properties that caller depends on.
    sc->clearSample();
    CHECK(sc->sampleBuffer() == nullptr,
          "an empty sampler hands back no buffer, so a caller cannot publish "
          "audio for a device that has none");
    {
        SampleRef mine = smRamp(4800, 2, 0.25f);
        sc->adopt(mine, "/tmp/readback.wav");
        SampleRef got = sc->sampleBuffer();
        CHECK(got.get() == mine.get(),
              "and a loaded one hands back the buffer it was given, not a copy");
        CHECK(got && got->frames == 4800 && got->channels == 2,
              "with its shape intact (%lld frames x %d)",
              got ? (long long)got->frames : -1, got ? got->channels : -1);

        // THE LIFETIME PROPERTY, and it is the whole reason the accessor returns
        // a shared_ptr rather than a raw pointer. The pool write on the handle's
        // side is a memcpy of every sample; a sampler re-pointed DURING it — a
        // second file drop, a project load — moves the old buffer into `retired_`
        // and reclaim() then frees it. A caller holding a reference is reading
        // memory that is still there; a caller holding a raw pointer is not.
        mine.reset();                       // the test drops its own reference
        sc->adopt(smRamp(1200, 2, 0.1f), "/tmp/other.wav");
        sc->reclaim();                      // frees what adopt() displaced
        CHECK(got && got->frames == 4800 && got->data.size() == 9600 &&
              got->data[0] == 0.f,
              "the reference taken BEFORE a re-point and a reclaim still reads "
              "its own buffer (%lld frames, %zu samples)",
              got ? (long long)got->frames : -1, got ? got->data.size() : 0u);
        CHECK(sc->sampleBuffer().get() != got.get(),
              "while the device itself has moved on to the new one");
    }
    sc->clearSample();
    sc->reclaim();
    CHECK(sc->sampleBuffer() == nullptr, "and clearSample() empties it again");
}

// ---------------------------------------------------------------------------

static void testSamplerPlayback(PluginRegistry& reg) {
    banner("Sampler: start, end, loop, gate, velocity and polyphony");

    const PluginDesc* d = reg.find("nxtakt:sampler");
    if (!d) return;

    const i64 kFrames = 48000;                       // one second of ramp
    const f32 kAmp    = 0.8f;
    // The ramp is a position probe: mean(output) tells you mean(read position).
    auto mk = [&](f64* meanOut, void (*patch)(PluginInstance&, int),
                  int arg, const std::vector<SpEvent>& ev, int frames,
                  int from, int n) -> bool {
        auto s = reg.instantiate(*d, kSR, kBlock);
        if (!s) return false;
        SamplerControl* sc = s->sampler();
        if (!sc) return false;
        sc->adopt(smRamp(kFrames, 1, kAmp), "/tmp/ramp.wav");
        smFlatPatch(*s);
        s->setParam(smIdx(*s, "Root Note"), 60.f);
        if (patch) patch(*s, arg);
        std::vector<f32> L, R;
        spRender(*s, ev, frames, kBlock, L, R);
        *meanOut = smMean(L, from, n);
        return true;
    };

    const std::vector<SpEvent> hold = { { 0, 0x90, 60, 127 } };

    // --- 1. Start is a fraction of the file, and it lands where it says.
    {
        f64 m0 = 0.0, m5 = 0.0;
        auto setStart = [](PluginInstance& s, int pct) {
            s.setParam(smIdx(s, "Start"), (f32)pct * 0.01f);
        };
        const bool a = mk(&m0, setStart, 0,  hold, 8000, 2000, 1000);
        const bool b = mk(&m5, setStart, 50, hold, 8000, 2000, 1000);
        if (a && b) {
            // Half the file further in is half the ramp higher, and the ramp's
            // full travel is kAmp.
            const f64 want = 0.5 * (f64)kAmp;
            CHECK(std::fabs((m5 - m0) - want) < 0.01,
                  "Start = 0.5 begins half a file later: the probe reads %.4f higher "
                  "and half the ramp is %.4f", m5 - m0, want);
        }
    }

    // --- 2. End stops the voice, and the 2 ms declick means it does not click.
    {
        auto s = reg.instantiate(*d, kSR, kBlock);
        if (s) {
            SamplerControl* sc = s->sampler();
            sc->adopt(smRamp(kFrames, 1, kAmp), "/tmp/ramp.wav");
            smFlatPatch(*s);
            s->setParam(smIdx(*s, "Root Note"), 60.f);
            s->setParam(smIdx(*s, "End"), 0.25f);      // 12000 frames
            std::vector<f32> L, R;
            spRender(*s, hold, 24000, kBlock, L, R);
            CHECK(smPeak(L, 4000, 4000) > 0.05, "the voice sounds before the end point");
            CHECK(smPeak(L, 14000, 10000) == 0.0,
                  "and is EXACTLY silent past it: End is a stop, not a fade to nothing");
            f64 jump = 0.0;
            for (int i = 11000; i < 13000; ++i)
                jump = std::fmax(jump, std::fabs((f64)L[(size_t)i] - (f64)L[(size_t)(i - 1)]));
            CHECK(jump < 0.005,
                  "and the 2 ms declick keeps the biggest step at the end point down to "
                  "%.5f -- a sample that does not finish at zero must not click", jump);
        }
    }

    // --- 3. Loop: the region repeats, and the crossfade is what makes the
    // splice inaudible rather than merely present.
    {
        auto mkLoop = [&](bool loop, f32 xfade, std::vector<f32>& L) -> bool {
            auto s = reg.instantiate(*d, kSR, kBlock);
            if (!s) return false;
            s->sampler()->adopt(smRamp(kFrames, 1, kAmp), "/tmp/ramp.wav");
            smFlatPatch(*s);
            s->setParam(smIdx(*s, "Root Note"), 60.f);
            s->setParam(smIdx(*s, "Start"), 0.25f);
            s->setParam(smIdx(*s, "End"),   0.50f);
            s->setParam(smIdx(*s, "Loop"),  loop ? 1.f : 0.f);
            s->setParam(smIdx(*s, "Crossfade"), xfade);
            std::vector<f32> R;
            spRender(*s, hold, 48000, kBlock, L, R);
            return true;
        };
        std::vector<f32> off, hard, soft;
        const bool a = mkLoop(false, 0.f, off);
        const bool b = mkLoop(true,  0.f, hard);
        const bool c = mkLoop(true,  20.f, soft);
        if (a && b && c) {
            CHECK(smPeak(off, 30000, 10000) == 0.0,
                  "with Loop off the voice ends when the region does");
            CHECK(smPeak(hard, 30000, 10000) > 0.05,
                  "with Loop on it is still sounding three regions later");
            f64 jHard = 0.0, jSoft = 0.0;
            for (int i = 20000; i < 40000; ++i) {
                jHard = std::fmax(jHard, std::fabs((f64)hard[(size_t)i] - (f64)hard[(size_t)(i - 1)]));
                jSoft = std::fmax(jSoft, std::fabs((f64)soft[(size_t)i] - (f64)soft[(size_t)(i - 1)]));
            }
            CHECK(jSoft < jHard * 0.25,
                  "a 20 ms crossfade cuts the biggest step at the loop point from "
                  "%.4f to %.5f", jHard, jSoft);
        }
    }

    // --- 4. Gate vs one-shot.
    {
        auto mkGate = [&](bool gate, std::vector<f32>& L) -> bool {
            auto s = reg.instantiate(*d, kSR, kBlock);
            if (!s) return false;
            s->sampler()->adopt(smRamp(kFrames, 1, kAmp), "/tmp/ramp.wav");
            smFlatPatch(*s);
            s->setParam(smIdx(*s, "Root Note"), 60.f);
            s->setParam(smIdx(*s, "Gate"), gate ? 1.f : 0.f);
            s->setParam(smIdx(*s, "Release"), 5.f);
            const std::vector<SpEvent> ev = { { 0, 0x90, 60, 127 }, { 4800, 0x80, 60, 0 } };
            std::vector<f32> R;
            spRender(*s, ev, 24000, kBlock, L, R);
            return true;
        };
        std::vector<f32> g, o;
        if (mkGate(true, g) && mkGate(false, o)) {
            CHECK(smPeak(g, 12000, 12000) < 1e-6,
                  "Gate on: the note-off releases the voice (tail %.2e)",
                  smPeak(g, 12000, 12000));
            CHECK(smPeak(o, 12000, 12000) > 0.05,
                  "Gate off (one-shot): the same note-off is ignored and the file "
                  "plays out");
        }
    }

    // --- 5. One-shot ignores Loop, which is the rule that makes it impossible
    // to build a voice nothing can stop.
    {
        auto s = reg.instantiate(*d, kSR, kBlock);
        if (s) {
            s->sampler()->adopt(smRamp(kFrames, 1, kAmp), "/tmp/ramp.wav");
            smFlatPatch(*s);
            s->setParam(smIdx(*s, "Root Note"), 60.f);
            s->setParam(smIdx(*s, "Gate"), 0.f);
            s->setParam(smIdx(*s, "Loop"), 1.f);
            s->setParam(smIdx(*s, "End"),  0.25f);
            std::vector<f32> L, R;
            spRender(*s, hold, 48000, kBlock, L, R);
            CHECK(smPeak(L, 20000, 28000) == 0.0,
                  "a one-shot does not loop: the voice ends at the end point even with "
                  "Loop on, so no key press can leave a voice nothing releases");
        }
    }

    // --- 6. A panic stops a one-shot too.
    {
        auto s = reg.instantiate(*d, kSR, kBlock);
        if (s) {
            s->sampler()->adopt(smRamp(kFrames, 1, kAmp), "/tmp/ramp.wav");
            smFlatPatch(*s);
            s->setParam(smIdx(*s, "Root Note"), 60.f);
            s->setParam(smIdx(*s, "Gate"), 0.f);
            s->setParam(smIdx(*s, "Release"), 5.f);
            const std::vector<SpEvent> ev = { { 0, 0x90, 60, 127 }, { 4800, 0xB0, 123, 0 } };
            std::vector<f32> L, R;
            spRender(*s, ev, 24000, kBlock, L, R);
            CHECK(smPeak(L, 12000, 12000) < 1e-6,
                  "All Notes Off releases a one-shot: a mode is a statement about the "
                  "key, not about the panic");
        }
    }

    // --- 7. Velocity, at both ends of its depth knob.
    {
        auto mkVel = [&](f32 depth, u8 vel, f64* mean) -> bool {
            auto s = reg.instantiate(*d, kSR, kBlock);
            if (!s) return false;
            s->sampler()->adopt(smRamp(kFrames, 1, kAmp), "/tmp/ramp.wav");
            smFlatPatch(*s);
            s->setParam(smIdx(*s, "Root Note"), 60.f);
            s->setParam(smIdx(*s, "Vel>Amp"), depth);
            const std::vector<SpEvent> ev = { { 0, 0x90, 60, vel } };
            std::vector<f32> L, R;
            spRender(*s, ev, 8000, kBlock, L, R);
            *mean = smMean(L, 2000, 1000);
            return true;
        };
        f64 loud = 0, soft = 0, flatA = 0, flatB = 0;
        if (mkVel(1.f, 127, &loud) && mkVel(1.f, 32, &soft) &&
            mkVel(0.f, 127, &flatA) && mkVel(0.f, 32, &flatB)) {
            const f64 want = 32.0 / 127.0;
            CHECK(loud > 1e-4 && std::fabs(soft / loud - want) < 0.02,
                  "Vel>Amp = 1: velocity 32 plays at %.3f of velocity 127, and 32/127 "
                  "is %.3f", soft / loud, want);
            CHECK(std::fabs(flatA - flatB) < 1e-6,
                  "Vel>Amp = 0: velocity does nothing at all (%.6f vs %.6f)",
                  flatA, flatB);
        }
    }

    // --- 8. Polyphony and stealing.
    {
        auto mkPoly = [&](int voices, f64* mean) -> bool {
            auto s = reg.instantiate(*d, kSR, kBlock);
            if (!s) return false;
            s->sampler()->adopt(smRamp(kFrames, 1, kAmp), "/tmp/ramp.wav");
            smFlatPatch(*s);
            s->setParam(smIdx(*s, "Root Note"), 60.f);
            s->setParam(smIdx(*s, "Voices"), (f32)voices);
            // Three notes at the SAME pitch, so the probe adds up cleanly.
            const std::vector<SpEvent> ev = {
                { 0, 0x90, 60, 127 }, { 1, 0x90, 60, 127 }, { 2, 0x90, 60, 127 },
            };
            std::vector<f32> L, R;
            spRender(*s, ev, 8000, kBlock, L, R);
            *mean = smMean(L, 3000, 1000);
            return true;
        };
        f64 one = 0, three = 0;
        if (mkPoly(1, &one) && mkPoly(3, &three)) {
            CHECK(one > 1e-4 && std::fabs(three / one - 3.0) < 0.05,
                  "three notes on three voices are %.2fx one note; on ONE voice they "
                  "are 1x, because the third steals the first two", three / one);
        }
    }

    // --- 9. A region with nothing in it is silence, not a division by zero.
    {
        auto s = reg.instantiate(*d, kSR, kBlock);
        if (s) {
            s->sampler()->adopt(smRamp(kFrames, 1, kAmp), "/tmp/ramp.wav");
            smFlatPatch(*s);
            s->setParam(smIdx(*s, "Root Note"), 60.f);
            s->setParam(smIdx(*s, "Start"), 0.8f);
            s->setParam(smIdx(*s, "End"),   0.2f);       // End below Start
            std::vector<f32> L, R;
            spRender(*s, hold, 8000, kBlock, L, R);
            bool fin = true;
            for (f32 v : L) if (!std::isfinite(v)) fin = false;
            CHECK(fin && smPeak(L, 0, 8000) == 0.0,
                  "End below Start is an empty region: exactly silent, and finite");
        }
    }

    // --- 10. Stereo in, stereo out, and mono spread to both.
    {
        auto s = reg.instantiate(*d, kSR, kBlock);
        if (s) {
            auto sb = smRamp(kFrames, 2, kAmp);
            // Make the right channel the negative of the left, so a device that
            // silently played channel 0 twice would fail here.
            for (i64 i = 0; i < kFrames; ++i) sb->data[(size_t)(i * 2 + 1)] *= -1.f;
            s->sampler()->adopt(sb, "/tmp/stereo.wav");
            smFlatPatch(*s);
            s->setParam(smIdx(*s, "Root Note"), 60.f);
            std::vector<f32> L, R;
            spRender(*s, hold, 8000, kBlock, L, R);
            const f64 ml = smMean(L, 2000, 1000), mr = smMean(R, 2000, 1000);
            CHECK(ml > 1e-4 && std::fabs(ml + mr) < 1e-4,
                  "a stereo file keeps its channels apart (L %.4f, R %.4f)", ml, mr);
        }
        auto m = reg.instantiate(*d, kSR, kBlock);
        if (m) {
            m->sampler()->adopt(smRamp(kFrames, 1, kAmp), "/tmp/mono.wav");
            smFlatPatch(*m);
            m->setParam(smIdx(*m, "Root Note"), 60.f);
            std::vector<f32> L, R;
            spRender(*m, hold, 8000, kBlock, L, R);
            const f64 ml = smMean(L, 2000, 1000), mr = smMean(R, 2000, 1000);
            CHECK(ml > 1e-4 && std::fabs(ml - mr) < 1e-9,
                  "and a mono file comes out of both sides identically");
        }
    }

    // --- 11. Glide: the second note starts at the FIRST note's pitch.
    {
        auto mkGlide = [&](f32 ms, f64* at1k, f64* at2k) -> bool {
            auto s = reg.instantiate(*d, kSR, kBlock);
            if (!s) return false;
            s->sampler()->adopt(smSine(1000.0, 480000, 1), "/tmp/sine.wav");
            smFlatPatch(*s);
            s->setParam(smIdx(*s, "Root Note"), 60.f);
            s->setParam(smIdx(*s, "Voices"), 1.f);
            s->setParam(smIdx(*s, "Glide"), ms);
            const std::vector<SpEvent> ev = {
                { 0, 0x90, 60, 100 }, { 12000, 0x80, 60, 0 }, { 12000, 0x90, 72, 100 },
            };
            std::vector<f32> L, R;
            spRender(*s, ev, 24000, kBlock, L, R);
            // The first 20 ms of the second note: with a 200 ms glide the pitch
            // has barely left the old note.
            *at1k = tBinMag(L, 12200, 960, 1000.0);
            *at2k = tBinMag(L, 12200, 960, 2000.0);
            return true;
        };
        f64 g1 = 0, g2 = 0, n1 = 0, n2 = 0;
        if (mkGlide(200.f, &g1, &g2) && mkGlide(0.f, &n1, &n2)) {
            CHECK(n2 > n1 * 4.0,
                  "with no glide the second note is already an octave up (2 kHz %.4f "
                  "vs 1 kHz %.4f)", n2, n1);
            CHECK(g1 > g2 * 4.0,
                  "with a 200 ms glide it starts from the note it left (1 kHz %.4f vs "
                  "2 kHz %.4f) -- the 303 preset is this and a filter", g1, g2);
        }
    }

    // --- 12. Coarse and Fine are a pitch ratio against the root.
    {
        auto mkTune = [&](f32 coarse, f32 fine, f64 wantHz) -> void {
            auto s = reg.instantiate(*d, kSR, kBlock);
            if (!s) return;
            s->sampler()->adopt(smSine(1000.0, 480000, 1), "/tmp/sine.wav");
            smFlatPatch(*s);
            s->setParam(smIdx(*s, "Root Note"), 60.f);
            s->setParam(smIdx(*s, "Coarse"), coarse);
            s->setParam(smIdx(*s, "Fine"), fine);
            const std::vector<SpEvent> ev = { { 0, 0x90, 60, 100 } };
            std::vector<f32> L, R;
            spRender(*s, ev, 24000, kBlock, L, R);
            const f64 hit = tBinMag(L, 4000, 16000, wantHz);
            const f64 ref = tBinMag(L, 4000, 16000, 1000.0);
            CHECK(hit > 0.1 && (wantHz == 1000.0 || hit > ref * 4.0),
                  "Coarse %+g st / Fine %+g ct puts the 1 kHz source at %.1f Hz "
                  "(magnitude %.3f there, %.3f at 1 kHz)",
                  (double)coarse, (double)fine, wantHz, hit, ref);
        };
        mkTune(0.f,   0.f, 1000.0);
        mkTune(12.f,  0.f, 2000.0);
        mkTune(-12.f, 0.f, 500.0);
        mkTune(0.f, 100.f, 1000.0 * std::pow(2.0, 1.0 / 12.0));
    }
}

// ---------------------------------------------------------------------------
// Interpolation quality
//
// The claim being tested: a repitching sampler on LINEAR interpolation hisses,
// and four-point Catmull-Rom is what stops it. Both numbers are printed for
// every ratio, from the same source and the same measurement, so the claim is a
// margin in decibels rather than an assertion.
// ---------------------------------------------------------------------------

static void testSamplerQuality(PluginRegistry& reg) {
    banner("Sampler: interpolation quality, measured in dB against the ideal tone");

    const PluginDesc* d = reg.find("nxtakt:sampler");
    if (!d) return;

    const i64 kSrcFrames = 480000;                   // ten seconds: never runs out
    const int kAnalyse   = 40000;
    const int kFrom      = 20000;
    const int kRender    = 96000;

    struct Case { f64 srcHz; f64 gate; const char* what; };
    const Case kCases[] = {
        { 1000.0, 60.0, "a 1 kHz tone -- the middle of where a sampled instrument lives" },
        { 4000.0, 45.0, "a 4 kHz tone -- near the top of what a real sample carries" },
    };

    for (const Case& c : kCases) {
        f64 worstCr = 1e9, worstLin = 1e9;
        int worstCrSt = 0;
        f64 crAt12 = 0.0, crAtM12 = 0.0;
        bool ok = true;

        for (int st : { -12, -7, -5, -3, -1, 1, 3, 5, 7, 12 }) {
            auto s = reg.instantiate(*d, kSR, kBlock);
            if (!s) return;
            SampleRef src = smSine(c.srcHz, kSrcFrames, 1);
            s->sampler()->adopt(src, "/tmp/sine.wav");
            smFlatPatch(*s);
            const std::vector<SpEvent> ev = { { 0, 0x90, (u8)(69 + st), 100 } };
            std::vector<f32> L, R;
            spRender(*s, ev, kRender, kBlock, L, R);

            const f64 ratio = std::pow(2.0, (f64)st / 12.0);
            const f64 outHz = c.srcHz * ratio;
            const f64 cr    = smSnrDb(L, kFrom, kAnalyse, outHz);

            std::vector<f32> lin;
            smLinearRef(*src, ratio, lin, kRender);
            const f64 li = smSnrDb(lin, kFrom, kAnalyse, outHz);

            std::printf("  note  %s %+3d st (x%.5f): Catmull-Rom %6.1f dB, linear %6.1f dB\n",
                        c.srcHz == 1000.0 ? "1 kHz" : "4 kHz", st, ratio, cr, li);
            if (cr < worstCr) { worstCr = cr; worstCrSt = st; }
            if (li < worstLin) worstLin = li;
            if (st == 12)  crAt12  = cr;
            if (st == -12) crAtM12 = cr;
            if (cr < c.gate) ok = false;
        }

        CHECK(ok, "%s: every ratio clears %.0f dB (worst %.1f dB, at %+d semitones)",
              c.what, c.gate, worstCr, worstCrSt);
        CHECK(crAt12 >= 60.0 && crAtM12 >= 60.0,
              "and the two the brief names -- +12 and -12 semitones -- are %.1f dB and "
              "%.1f dB, both past 60", crAt12, crAtM12);
        CHECK(worstCr > worstLin + 12.0,
              "Catmull-Rom is %.1f dB better than linear at its own worst ratio "
              "(%.1f vs %.1f) -- that gap is the whole reason for the four-point read",
              worstCr - worstLin, worstCr, worstLin);
    }

    // At the root note the read lands on the sample grid at every step, and
    // Catmull-Rom is EXACT there: c1 = 1 and the other three coefficients are
    // 0. What comes out is the file, through a linear filter and a settled
    // envelope and nothing else -- so the residual is the float noise floor.
    {
        auto s = reg.instantiate(*d, kSR, kBlock);
        if (!s) return;
        s->sampler()->adopt(smSine(1000.0, kSrcFrames, 1), "/tmp/sine.wav");
        smFlatPatch(*s);
        const std::vector<SpEvent> ev = { { 0, 0x90, 69, 100 } };
        std::vector<f32> L, R;
        spRender(*s, ev, kRender, kBlock, L, R);
        const f64 snr = smSnrDb(L, kFrom, kAnalyse, 1000.0);
        CHECK(snr > 120.0,
              "at the root note the interpolator is exact on the grid: %.1f dB, which "
              "is the float noise floor and not a filter", snr);
    }
}

// ---------------------------------------------------------------------------

static void testSamplerDeterminism(PluginRegistry& reg) {
    banner("Sampler: the render does not depend on the block size");

    const PluginDesc* d = reg.find("nxtakt:sampler");
    if (!d) return;

    const int kFrames = 20000;
    // Overlapping notes, a chord, a released note and more notes than the cap,
    // so voice allocation, stealing, glide and the loop wrap are all exercised.
    const std::vector<SpEvent> ev = {
        {     0, 0x90, 45,  90 },
        {   700, 0x90, 57, 120 },
        {  2300, 0x90, 64,  40 },
        {  4001, 0x80, 45,   0 },
        {  4600, 0x90, 69, 110 },
        {  5000, 0x90, 72,  20 },
        {  5003, 0x90, 76,  95 },
        {  5100, 0x90, 79,  70 },
        {  5111, 0x90, 84, 127 },     // one past the cap: steals
        {  8000, 0x80, 57,   0 },
        {  8100, 0x80, 64,   0 },
        { 14000, 0xB0, 123,  0 },     // all notes off
    };

    auto build = [&]() -> std::unique_ptr<PluginInstance> {
        auto s = reg.instantiate(*d, kSR, kBlock);
        if (!s) return s;
        // A source with structure in it, so a one-sample position slip shows.
        s->sampler()->adopt(smSine(700.0, 96000, 2), "/tmp/busy.wav");
        smBusyPatch(*s);
        return s;
    };

    std::vector<f32> refL, refR, altL, altR;
    auto ref = build();
    if (!ref) return;
    spRender(*ref, ev, kFrames, kBlock, refL, refR, 120.0);

    CHECK(smPeak(refL, 0, kFrames) > 0.02,
          "the reference render is not silent (peak %.4f)", smPeak(refL, 0, kFrames));

    bool allSame = true;
    for (int chunk : { 1, 7, 64, 300, 1024 }) {
        auto alt = build();
        if (!alt) break;
        spRender(*alt, ev, kFrames, chunk, altL, altR, 120.0);
        f32 diff = 0.f;
        for (int i = 0; i < kFrames; ++i) {
            diff = std::fmax(diff, std::fabs(refL[(size_t)i] - altL[(size_t)i]));
            diff = std::fmax(diff, std::fabs(refR[(size_t)i] - altR[(size_t)i]));
        }
        if (diff != 0.f) allSame = false;
        CHECK(diff == 0.f,
              "blocks of %d are bit-identical to blocks of %d (max diff %.9f)",
              chunk, kBlock, (double)diff);
    }
    CHECK(allSame, "six block sizes agree to the bit: the queued MIDI, the control "
                   "tick on absolute sample time, the per-sample envelope and the "
                   "per-sample read position all survive the boundary");

    // Two fresh instances render the same file. There is no random number in
    // this device at all, so this is a check that none arrives later -- and
    // that nothing reads a clock.
    {
        auto a = build();
        auto b = build();
        if (a && b) {
            std::vector<f32> aL, aR, bL, bR;
            spRender(*a, ev, kFrames, kBlock, aL, aR, 120.0);
            spRender(*b, ev, kFrames, kBlock, bL, bR, 120.0);
            f32 diff = 0.f;
            for (int i = 0; i < kFrames; ++i)
                diff = std::fmax(diff, std::fabs(aL[(size_t)i] - bL[(size_t)i]));
            CHECK(diff == 0.f, "two fresh instances render identically");
        }
    }
}

// ---------------------------------------------------------------------------
// The state string
//
// host.h's setStateString contract, exercised against the one device that has
// any state: an empty string is a no-op, anything that does not parse is
// REFUSED with the device left exactly as it was, and no byte sequence may
// crash it. The string comes out of a file a user can edit.
// ---------------------------------------------------------------------------

static void testSamplerState(PluginRegistry& reg) {
    banner("Sampler: the state string round-trips, and refuses everything else");

    const PluginDesc* d = reg.find("nxtakt:sampler");
    if (!d) return;

    // --- 1. The shape.
    {
        auto s = reg.instantiate(*d, kSR, kBlock);
        if (!s) return;
        s->sampler()->adopt(smRamp(4800), "/home/x/kit/kick.wav");
        CHECK(s->stateString() == "nxsmp1;p=/home/x/kit/kick.wav",
              "an ordinary path rides verbatim behind the version tag: \"%s\"",
              s->stateString().c_str());
    }

    // --- 2. Escaping. Every one of these is a legal Linux filename, and every
    // one of them would break a line-oriented format if it went through raw.
    {
        const char* kPaths[] = {
            "/tmp/a file with spaces.wav",
            "/tmp/semi;colon.wav",
            "/tmp/com,ma.wav",
            "/tmp/co:lon.wav",
            "/tmp/eq=uals.wav",
            "/tmp/per%cent.wav",
            "/tmp/new\nline.wav",
            "/tmp/tab\there.wav",
            "/tmp/\xC3\xA9t\xC3\xA9-2026.wav",        // UTF-8
            "/tmp/nxsmp1;p=notapath.wav",             // looks like our own format
            "relative/path.wav",
            "/tmp/\xFF\xFE-not-utf8.wav",
        };
        bool clean = true, trip = true;
        for (const char* p : kPaths) {
            auto a = reg.instantiate(*d, kSR, kBlock);
            auto b = reg.instantiate(*d, kSR, kBlock);
            if (!a || !b) break;
            a->sampler()->adopt(smRamp(4800), p);
            const std::string st = a->stateString();
            for (char ch : st) {
                const unsigned char u = (unsigned char)ch;
                if (u <= ' ' || u >= 0x7F) {
                    CHECK(false, "state for \"%s\" carries byte 0x%02X -- it must be "
                                 "printable ASCII with no whitespace", p, (unsigned)u);
                    clean = false;
                    break;
                }
            }
            if (!b->setStateString(st) || b->sampler()->samplePath() != p ||
                b->stateString() != st) {
                CHECK(false, "\"%s\" did not round-trip (came back as \"%s\")",
                      p, b->sampler()->samplePath().c_str());
                trip = false;
            }
        }
        CHECK(clean, "every escaped state string is one line of printable ASCII with "
                     "no whitespace, so project.cpp's kv() can carry it blind");
        CHECK(trip, "and every path -- spaces, separators, newlines, UTF-8, invalid "
                    "UTF-8, and a path that spells out this very format -- comes back "
                    "byte for byte");
    }

    // --- 3. A path that will not open is NOT a malformed state. The set is
    // right and the machine is missing a file; losing the path would lose the
    // only record of what the set names.
    {
        auto s = reg.instantiate(*d, kSR, kBlock);
        if (!s) return;
        const std::string st = "nxsmp1;p=/nowhere/at/all.wav";
        CHECK(s->setStateString(st),
              "a state naming a file this machine does not have is ACCEPTED");
        CHECK(!s->sampler()->hasSample(),
              "the device is silent, because it has nothing to play");
        CHECK(s->sampler()->samplePath() == "/nowhere/at/all.wav" &&
              s->stateString() == st,
              "and it still writes the path back out, so the set does not forget");
        std::vector<f32> L, R;
        const std::vector<SpEvent> ev = { { 0, 0x90, 60, 127 } };
        spRender(*s, ev, 4800, kBlock, L, R);
        CHECK(smPeak(L, 0, 4800) == 0.0, "and renders exact silence meanwhile");
    }

    // --- 4. The empty string is not malformed: it is what a fresh instance
    // answers, and it must be a no-op.
    {
        auto s = reg.instantiate(*d, kSR, kBlock);
        if (!s) return;
        s->sampler()->adopt(smRamp(4800), "/tmp/keep.wav");
        CHECK(s->setStateString(""), "the empty string is accepted");
        CHECK(s->sampler()->hasSample() && s->sampler()->samplePath() == "/tmp/keep.wav",
              "and changes nothing");
    }

    // --- 5. MALFORMED. Each one must be refused AND leave the device exactly
    // as it was -- a half-parsed state is how a corrupt file becomes a corrupt
    // session.
    {
        std::string huge = "nxsmp1;p=/tmp/";
        huge.append(6000, 'a');
        std::string withNul = "nxsmp1;p=/tmp/a.wav";
        withNul.push_back('\0');
        withNul += "extra";

        struct Bad { std::string s; const char* why; };
        const Bad kBad[] = {
            { "garbage",                       "no version record at all" },
            { "p=/tmp/a.wav",                  "a p record with no version in front of it" },
            { "nxsmp2;p=/tmp/a.wav",           "a version this reader does not know" },
            { "NXSMP1;p=/tmp/a.wav",           "the version tag is case sensitive" },
            { "nxsmp1 ;p=/tmp/a.wav",          "a space inside the version record" },
            { "nxsmp1",                        "a version and no path" },
            { "nxsmp1;",                       "an empty trailing record" },
            { "nxsmp1;;p=/tmp/a.wav",          "an empty record in the middle" },
            { "nxsmp1;p=",                     "an empty path" },
            { "nxsmp1;p=/tmp/a.wav;p=/tmp/b.wav", "two p records: which file?" },
            { "nxsmp1;px/tmp/a.wav",           "a record with no '='" },
            { "nxsmp1;=/tmp/a.wav",            "a record with no tag" },
            { "nxsmp1;p=/tmp/a%",              "a '%' with nothing after it" },
            { "nxsmp1;p=/tmp/a%A",             "a '%' with one nibble after it" },
            { "nxsmp1;p=/tmp/a%ZZ.wav",        "a '%' with no hex after it" },
            { "nxsmp1;p=/tmp/a%2g.wav",        "a '%' with half a hex byte after it" },
            { "nxsmp1;p=/tmp/a%00b.wav",       "an escaped NUL, which open(2) would truncate at" },
            { "nxsmp1;p=%00",                  "a path that is nothing but a NUL" },
            { "nxsmp1;p=/tmp/a%%.wav",         "a '%' whose first nibble is another '%'" },
            { "nxsmp1;p=/tmp/a.wav;",          "a trailing empty record after a good one" },
            { "nxsmp1;p=/tmp/a b.wav",         "a raw space where an escape belongs" },
            { "nxsmp1;p=/tmp/a\tb.wav",        "a raw tab" },
            { "nxsmp1;p=/tmp/a\nb.wav",        "a raw newline" },
            { "nxsmp1;p=/tmp/\xC3\xA9.wav",    "raw UTF-8, which this writer escapes" },
            { huge,                            "a path past the 4096-byte cap" },
            { withNul,                         "a string with an embedded NUL byte" },
        };

        auto s = reg.instantiate(*d, kSR, kBlock);
        if (!s) return;
        SamplerControl* sc = s->sampler();
        sc->adopt(smRamp(4800), "/tmp/known.wav");
        const std::string before = s->stateString();

        int refused = 0, intact = 0;
        const int nBad = (int)(sizeof kBad / sizeof kBad[0]);
        for (const Bad& b : kBad) {
            const bool took = s->setStateString(b.s);
            if (!took) ++refused;
            else CHECK(false, "\"%s\" was ACCEPTED (%s)", b.s.c_str(), b.why);
            if (sc->hasSample() && sc->samplePath() == "/tmp/known.wav" &&
                s->stateString() == before) ++intact;
            else CHECK(false, "\"%s\" changed the device (%s)", b.s.c_str(), b.why);
        }
        CHECK(refused == nBad, "all %d malformed states are refused (%d)", nBad, refused);
        CHECK(intact == nBad,
              "and every one of them leaves the sample, the path and the state string "
              "exactly as they were (%d of %d)", intact, nBad);
    }

    // --- 6. Unknown records are SKIPPED, which is what makes the format
    // extensible: a later writer may add a record and this reader must not
    // treat the file as corrupt.
    {
        auto s = reg.instantiate(*d, kSR, kBlock);
        if (!s) return;
        CHECK(s->setStateString("nxsmp1;z=whatever;p=/tmp/a.wav;q=1"),
              "records this version does not know are skipped, not refused");
        CHECK(s->sampler()->samplePath() == "/tmp/a.wav",
              "and the path still arrives");
    }

    // --- 7. THE PROJECT LAYER'S SEQUENCE, walked by hand.
    //
    // src/ui/app_project.cpp is not linked by any suite -- it is GUI code --
    // so what is checked here is the sequence it performs rather than the
    // function that performs it: serializeDevices() writes (ParamInfo::id,
    // value) pairs plus stateString(); materializeDevices() applies the
    // parameters FIRST and setStateString() second. Doing it in that order is
    // the rule docs/RACKS.md §Persistence derives and host.h restates, and it
    // matters for this device: Start is a parameter and every voice is placed
    // from it.
    {
        auto src = reg.instantiate(*d, kSR, kBlock);
        if (!src) return;
        src->sampler()->adopt(smSine(700.0, 96000, 2), "/tmp/set/loop.wav");
        smBusyPatch(*src);

        std::vector<std::pair<u32, f32>> params;
        for (int i = 0; i < src->paramCount(); ++i)
            params.emplace_back(src->paramInfo(i).id, src->getParam(i));
        const std::string state = src->stateString();
        CHECK((int)params.size() == kSamplerContractN && !state.empty(),
              "a save takes %zu parameter pairs and one state string",
              params.size());

        auto dst = reg.instantiate(*d, kSR, kBlock);
        if (!dst) return;
        for (const auto& kv : params) {                    // parameters FIRST
            int idx = -1;
            for (int i = 0; i < dst->paramCount(); ++i)
                if (dst->paramInfo(i).id == kv.first) { idx = i; break; }
            if (idx >= 0) dst->setParam(idx, kv.second);
        }
        CHECK(dst->setStateString(state), "and the state string is applied AFTER them");
        // The file is not on this machine (and this binary could not open it
        // anyway), so the restored device is silent -- but it must be the same
        // device otherwise. Hand it the same buffer and the two have to agree
        // to the bit.
        dst->sampler()->adopt(smSine(700.0, 96000, 2), dst->sampler()->samplePath());

        bool same = true;
        for (int i = 0; i < src->paramCount(); ++i)
            if (src->getParam(i) != dst->getParam(i)) same = false;
        CHECK(same, "every restored parameter is exactly the value that was saved");
        CHECK(dst->stateString() == state,
              "and the restored device writes back the same state string");

        const std::vector<SpEvent> ev = {
            { 0, 0x90, 45, 90 }, { 700, 0x90, 57, 120 }, { 4001, 0x80, 45, 0 },
            { 5000, 0x90, 72, 20 }, { 9000, 0xB0, 123, 0 },
        };
        std::vector<f32> aL, aR, bL, bR;
        spRender(*src, ev, 14000, kBlock, aL, aR, 120.0);
        spRender(*dst, ev, 14000, kBlock, bL, bR, 120.0);
        f32 diff = 0.f;
        for (int i = 0; i < 14000; ++i) {
            diff = std::fmax(diff, std::fabs(aL[(size_t)i] - bL[(size_t)i]));
            diff = std::fmax(diff, std::fabs(aR[(size_t)i] - bR[(size_t)i]));
        }
        CHECK(smPeak(aL, 0, 14000) > 0.02, "the comparison render is not silent");
        CHECK(diff == 0.f, "and a restored sampler renders bit-identically (max diff "
                           "%.9f)", (double)diff);
    }

    // --- 8. Fuzz. Not a proof, a floor: nothing here may crash, and anything
    // ACCEPTED has to round-trip, because an accepted state that does not
    // round-trip is a set that changes every time it is opened.
    {
        auto s = reg.instantiate(*d, kSR, kBlock);
        if (!s) return;
        u32 rng = 0xC0FFEEu;
        auto next = [&]() { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
        int accepted = 0, tripped = 0;
        bool ok = true;
        for (int k = 0; k < 4000; ++k) {
            std::string t;
            if (k & 1) t = "nxsmp1;p=";              // half of them start plausibly
            const int n = (int)(next() % 40u);
            for (int i = 0; i < n; ++i) t.push_back((char)(next() % 256u));
            const bool took = s->setStateString(t);
            if (took) {
                ++accepted;
                const std::string round = s->stateString();
                if (s->setStateString(round) && s->stateString() == round) ++tripped;
                else ok = false;
            }
        }
        CHECK(ok, "4000 fuzzed states: none crashed, and all %d that were accepted "
                  "round-trip through their own spelling (%d)", accepted, tripped);
    }
}

// ---------------------------------------------------------------------------

static void testSamplerPresets(PluginRegistry& reg) {
    banner("Sampler: factory presets");

    const PluginDesc* d = reg.find("nxtakt:sampler");
    if (!d) return;
    auto s = reg.instantiate(*d, kSR, kBlock);
    if (!s) return;

    CHECK(s->presetCount() == kSamplerPresetN,
          "presetCount() is %d (the brief asked for %d)", s->presetCount(), kSamplerPresetN);
    CHECK(s->presetName(-1) == nullptr && s->presetName(s->presetCount()) == nullptr,
          "presetName() is null out of range");

    bool names = true;
    for (int i = 0; i < s->presetCount() && i < kSamplerPresetN; ++i) {
        const char* n = s->presetName(i);
        if (!n || std::strcmp(n, kSamplerPresetNames[i]) != 0) {
            CHECK(false, "preset %d is \"%s\", expected \"%s\"",
                  i, n ? n : "(null)", kSamplerPresetNames[i]);
            names = false;
        }
    }
    CHECK(names, "all %d preset names match, in order", kSamplerPresetN);

    // Init is the constructor's defaults, exactly.
    {
        auto a = reg.instantiate(*d, kSR, kBlock);
        if (a) {
            smBusyPatch(*a);
            a->loadPreset(0);
            bool def = true;
            for (int i = 0; i < a->paramCount(); ++i)
                if (a->getParam(i) != a->paramInfo(i).def) {
                    CHECK(false, "Init left %s at %g, the default is %g",
                          a->paramInfo(i).name.c_str(), (double)a->getParam(i),
                          (double)a->paramInfo(i).def);
                    def = false;
                }
            CHECK(def, "Init restores every parameter to its constructor default exactly");
        }
    }

    // In range, integral where stepped, and self-contained.
    {
        bool inRange = true, complete = true;
        for (int k = 0; k < s->presetCount(); ++k) {
            auto fresh = reg.instantiate(*d, kSR, kBlock);
            auto dirty = reg.instantiate(*d, kSR, kBlock);
            if (!fresh || !dirty) break;
            fresh->loadPreset(k);
            for (int i = 0; i < fresh->paramCount(); ++i) {
                const ParamInfo& pi = fresh->paramInfo(i);
                const f32 v = fresh->getParam(i);
                if (!std::isfinite(v) || v < pi.min || v > pi.max) {
                    CHECK(false, "preset \"%s\" leaves %s at %g, outside [%g, %g]",
                          kSamplerPresetNames[k], pi.name.c_str(), (double)v,
                          (double)pi.min, (double)pi.max);
                    inRange = false;
                }
                if (pi.isInt && v != std::floor(v)) {
                    CHECK(false, "preset \"%s\" leaves the stepped %s at %g",
                          kSamplerPresetNames[k], pi.name.c_str(), (double)v);
                    inRange = false;
                }
            }
            smBusyPatch(*dirty);
            dirty->loadPreset((k + 3) % s->presetCount());
            dirty->loadPreset(k);
            for (int i = 0; i < fresh->paramCount(); ++i)
                if (fresh->getParam(i) != dirty->getParam(i)) {
                    CHECK(false, "preset \"%s\" is not self-contained: %s is %g from a "
                                 "dirty state and %g from a fresh one",
                          kSamplerPresetNames[k], fresh->paramInfo(i).name.c_str(),
                          (double)dirty->getParam(i), (double)fresh->getParam(i));
                    complete = false;
                    break;
                }
        }
        CHECK(inRange, "every preset leaves every parameter finite, in range, and "
                       "integral where the table says stepped");
        CHECK(complete, "every preset lands on the same state from any starting patch");
    }

    // NO PRESET NAMES A FILE. It is the rule that keeps the six of them from
    // being six broken devices on every machine but the author's -- and the
    // corollary is that loading one must not throw away the sample the user
    // already has.
    {
        bool kept = true;
        for (int k = 0; k < s->presetCount(); ++k) {
            auto a = reg.instantiate(*d, kSR, kBlock);
            if (!a) break;
            a->sampler()->adopt(smRamp(4800), "/tmp/mine.wav");
            a->loadPreset(k);
            if (!a->sampler()->hasSample() || a->sampler()->samplePath() != "/tmp/mine.wav" ||
                a->stateString() != "nxsmp1;p=/tmp/mine.wav")
                kept = false;
        }
        CHECK(kept, "no preset touches the sample: a preset is a way of PLAYING, and "
                    "it cannot name a file the user does not have");
    }

    // And each of the six actually does something different with the same
    // source and the same note.
    {
        f64 mean[kSamplerPresetN] = {};
        bool sounded = true;
        for (int k = 0; k < s->presetCount() && k < kSamplerPresetN; ++k) {
            auto a = reg.instantiate(*d, kSR, kBlock);
            if (!a) break;
            a->sampler()->adopt(smSine(600.0, 96000, 1), "/tmp/tone.wav");
            a->loadPreset(k);
            a->setParam(smIdx(*a, "Root Note"), 60.f);
            const std::vector<SpEvent> ev = { { 0, 0x90, 60, 110 }, { 24000, 0x80, 60, 0 } };
            std::vector<f32> L, R;
            spRender(*a, ev, 72000, kBlock, L, R);
            bool fin = true;
            for (f32 v : L) if (!std::isfinite(v)) fin = false;
            mean[k] = smPeak(L, 0, 72000);
            if (!fin || mean[k] <= 1e-4) {
                CHECK(false, "preset \"%s\" renders nothing (peak %.6f, finite=%d)",
                      kSamplerPresetNames[k], mean[k], (int)fin);
                sounded = false;
            }
        }
        CHECK(sounded, "all %d presets make a finite, audible sound from the same "
                       "source", kSamplerPresetN);
    }
}

// ---------------------------------------------------------------------------
// AUDIT-3 F3 — a MIDI flood must not be able to strand a voice
//
// Both instruments share the queue design, so both are checked here with the
// same three floods. RED before the fix: the queue dropped everything once
// full, so the panic behind 128 note-ons was thrown away and the voices it was
// meant to stop sounded for the rest of the session.
//
// The three cases are the three ways to run out of room:
//   a. more note-ons than the whole queue, then a panic;
//   b. exactly enough note-ons to fill the reserve, then note-offs, then a
//      panic -- so the panic can only be caught by the overflow set;
//   c. more note-ons than the queue, then an OFF for each of them and no panic
//      at all, which is what a MIDI file with a dense chord actually looks
//      like.
// ---------------------------------------------------------------------------

static void testMidiFlood(PluginRegistry& reg, const char* uri, const char* label,
                          void (*patch)(PluginInstance&)) {
    banner(label);

    const PluginDesc* d = reg.find(uri);
    if (!d) return;

    // Everything sustains, so a voice that is not stopped is a voice that is
    // still audible three seconds later.
    auto build = [&]() -> std::unique_ptr<PluginInstance> {
        auto s = reg.instantiate(*d, kSR, kBlock);
        if (!s) return s;
        if (patch) patch(*s);
        return s;
    };

    // One block, `nOn` note-ons in it, then `tail` bytes, then silence.
    auto flood = [&](int nOn, int nOff, int panic, f64* tailPeak) -> bool {
        auto s = build();
        if (!s) return false;
        const int kBig = 1024;
        std::vector<f32> bl((size_t)kBig, 0.f), br((size_t)kBig, 0.f);
        f32* o[2] = { bl.data(), br.data() };

        for (int i = 0; i < nOn; ++i) {
            const u8 m[3] = { 0x90, (u8)(24 + (i % 96)), 100 };
            s->midi(m, 3, i % kBig);
        }
        for (int i = 0; i < nOff; ++i) {
            const u8 m[3] = { 0x80, (u8)(24 + (i % 96)), 0 };
            s->midi(m, 3, (nOn + i) % kBig);
        }
        if (panic) {
            const u8 m[3] = { 0xB0, (u8)panic, 0 };
            s->midi(m, 3, kBig - 1);
        }
        s->process(nullptr, o, 2, kBig);

        // Four seconds of nothing. Anything still sounding at the end is stuck.
        f64 pk = 0.0;
        for (int blk = 0; blk < 750; ++blk) {
            std::fill(bl.begin(), bl.end(), 0.f);
            std::fill(br.begin(), br.end(), 0.f);
            s->process(nullptr, o, 2, kBig);
            if (blk >= 700)
                for (int i = 0; i < kBig; ++i) {
                    pk = std::fmax(pk, std::fabs((f64)bl[(size_t)i]));
                    pk = std::fmax(pk, std::fabs((f64)br[(size_t)i]));
                }
        }
        *tailPeak = pk;
        return true;
    };

    // Sanity: without the panic the flood really does leave voices sounding,
    // so the three checks below are measuring the panic and not the envelope.
    f64 stuck = 0.0;
    if (flood(200, 0, 0, &stuck))
        CHECK(stuck > 1e-4,
              "a flood with NO panic leaves voices sounding four seconds later (peak "
              "%.5f) -- which is what makes the next three checks mean something",
              stuck);

    f64 a = 0.0, b = 0.0, c = 0.0, e = 0.0;
    if (flood(200, 0, 123, &a))
        CHECK(a < 1e-6,
              "200 note-ons then All Notes Off: silent (peak %.2e). The panic is "
              "behind more note-ons than the queue holds, and it still lands", a);
    if (flood(200, 0, 120, &e))
        CHECK(e < 1e-6, "the same with All Sound Off (peak %.2e)", e);
    if (flood(96, 40, 123, &b))
        CHECK(b < 1e-6,
              "96 note-ons + 40 note-offs (which fills the reserve too) then All Notes "
              "Off: silent (peak %.2e) -- this one can only be the overflow set", b);
    if (flood(200, 200, 0, &c))
        CHECK(c < 1e-6,
              "200 note-ons then a note-off for every one of them and NO panic: silent "
              "(peak %.2e). A note-off is never dropped, only note-ons are", c);
}

static void spFloodPatch(PluginInstance& s) {
    s.setParam(spIdx(s, "A Level"), 0.9f);
    s.setParam(spIdx(s, "Attack"), 1.f);
    s.setParam(spIdx(s, "Decay"), 5000.f);
    s.setParam(spIdx(s, "Sustain"), 1.f);
    s.setParam(spIdx(s, "Release"), 20.f);
}

static void smFloodPatch(PluginInstance& s) {
    s.sampler()->adopt(smSine(500.0, 96000, 1), "/tmp/flood.wav");
    smFlatPatch(s);
    s.setParam(smIdx(s, "Root Note"), 60.f);
    s.setParam(smIdx(s, "Loop"), 1.f);          // so nothing ends on its own
    s.setParam(smIdx(s, "Start"), 0.1f);
    s.setParam(smIdx(s, "End"), 0.4f);
    s.setParam(smIdx(s, "Crossfade"), 10.f);
    s.setParam(smIdx(s, "Release"), 20.f);
}

// ---------------------------------------------------------------------------

// ===========================================================================
// Shimmer, Bloom and Tape — the suite sections, for splicing into
// tests/internal_device_test.cpp VERBATIM.
//
// HOW TO MERGE
//
//   1. paste this whole file into tests/internal_device_test.cpp, anywhere
//      after the `Noise` helper (it needs Buf, Noise, CHECK, banner, note,
//      paramIndex, kSR and kBlock, and nothing else the suite defines);
//   2. add these eight calls to main(), next to the other effect sections:
//
//          testFxContract(reg);
//          testShimmerClimb(reg);
//          testShimmerStability(reg);
//          testBloomCrossover(reg);
//          testBloomDynamics(reg);
//          testTape(reg);
//          testFxBlockInvariance(reg);
//          testFxPresets(reg);
//
//   3. bump the registry count: `CHECK(internals == 13, ...)` becomes 16.
//
// Everything else this file needs it brings with it: its own single-bin DFT,
// its own chunked renderer, its own second-order allpass reference. That is
// deliberate — nothing here depends on WHERE in the file it lands.
//
// LAT_FX_LOCAL_REGISTRY is defined only by the scratchpad harness, which runs
// these same sections against a local factory so they could be proven before
// the devices were registered. In the merged suite it is never defined and the
// two adapters below are the whole of the difference.
// ===========================================================================

#ifndef LAT_FX_LOCAL_REGISTRY
static const PluginDesc* fxFind(PluginRegistry& reg, const char* uri) {
    return reg.find(uri);
}
static std::unique_ptr<PluginInstance> fxMake(PluginRegistry& reg, const PluginDesc& d) {
    return reg.instantiate(d, kSR, kBlock);
}
#endif

static const char* kFxUris[] = { "nxtakt:shimmer", "nxtakt:bloom", "nxtakt:tape" };

// ---------------------------------------------------------------------------
// Measurement helpers
//
// Everything below MEASURES. A single-bin DFT over a whole window is exact for
// a steady sinusoid and needs no window function and no FFT; for the reverb
// tail, where nothing is steady, it is a Hann-windowed bin, which is what
// "FFT the tail at t seconds" means in practice with one bin instead of 2048.
// ---------------------------------------------------------------------------

// Magnitude at `freq`, in dBFS, of x[from .. from+n). Hann-windowed and
// corrected for the window's coherent gain, so a full-scale sine reads 0 dB.
static f64 fxBinDb(const std::vector<f32>& x, size_t from, size_t n, f64 freq) {
    if (from + n > x.size() || n < 8) return -300.0;
    const f64 w = 6.283185307179586 * freq / kSR;
    f64 re = 0.0, im = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const f64 win = 0.5 - 0.5 * std::cos(6.283185307179586 * (f64)i / (f64)n);
        const f64 ph  = w * (f64)i;
        const f64 v   = (f64)x[from + i] * win;
        re += v * std::cos(ph);
        im += v * std::sin(ph);
    }
    const f64 mag = 4.0 * std::sqrt(re * re + im * im) / (f64)n;   // 4 = 2 / 0.5
    return (mag <= 1e-15) ? -300.0 : 20.0 * std::log10(mag);
}

// Steady-state output/input magnitude at `freq`, in dB. Settles first, then
// integrates a whole number of cycles.
static f64 fxProbeDb(PluginInstance& p, f64 freq, f32 amp, int cycles, int settleBlocks = 60) {
    Buf in, out;
    const f64 w = 6.283185307179586 * freq / kSR;
    u64 n = 0;
    auto fill = [&](int k) {
        for (int i = 0; i < k; ++i)
            in.l[(size_t)i] = in.r[(size_t)i] = amp * (f32)std::sin(w * (f64)(n + (u64)i));
    };
    for (int b = 0; b < settleBlocks; ++b) {
        fill(kBlock); out.clear(); p.process(in.p, out.p, 2, kBlock); n += (u64)kBlock;
    }
    const int N = (int)std::llround((f64)cycles * kSR / freq);
    f64 re = 0.0, im = 0.0;
    int done = 0;
    while (done < N) {
        const int k = (N - done) < kBlock ? (N - done) : kBlock;
        fill(k); out.clear(); p.process(in.p, out.p, 2, k);
        for (int i = 0; i < k; ++i) {
            const f64 ph = w * (f64)(n + (u64)i);
            re += (f64)out.l[(size_t)i] * std::cos(ph);
            im += (f64)out.l[(size_t)i] * std::sin(ph);
        }
        n += (u64)k;
        done += k;
    }
    const f64 mag = 2.0 * std::sqrt(re * re + im * im) / (f64)N;
    if (mag <= 1e-14 || amp <= 0.f) return -300.0;
    return 20.0 * std::log10(mag / (f64)amp);
}

// Feeds a whole signal through a device in blocks of `chunk`, IN PLACE, which
// is how the engine calls a device on a track.
static void fxRender(PluginInstance& p, const std::vector<f32>& inL,
                     const std::vector<f32>& inR, int chunk,
                     std::vector<f32>& oL, std::vector<f32>& oR) {
    const int frames = (int)inL.size();
    oL.assign((size_t)frames, 0.f);
    oR.assign((size_t)frames, 0.f);
    std::vector<f32> bl((size_t)chunk, 0.f), br((size_t)chunk, 0.f);
    for (int i = 0; i < frames; i += chunk) {
        const int k = (frames - i) < chunk ? (frames - i) : chunk;
        for (int j = 0; j < k; ++j) {
            bl[(size_t)j] = inL[(size_t)(i + j)];
            br[(size_t)j] = inR[(size_t)(i + j)];
        }
        const f32* cin[2]  = { bl.data(), br.data() };
        f32*       cout[2] = { bl.data(), br.data() };
        p.process(cin, cout, 2, k);
        for (int j = 0; j < k; ++j) {
            oL[(size_t)(i + j)] = bl[(size_t)j];
            oR[(size_t)(i + j)] = br[(size_t)j];
        }
    }
}

// Sets a parameter by NAME and says so if the name has moved. Ids are frozen,
// but a test that silently sets nothing is worse than one that fails.
static bool fxSet(PluginInstance& p, const char* name, f32 v) {
    const int i = paramIndex(p, name);
    if (i < 0) { CHECK(false, "parameter \"%s\" is missing", name); return false; }
    p.setParam(i, v);
    return true;
}

// A second-order allpass, written from the RBJ cookbook independently of the
// device's own copy. This is the reference the Bloom crossover nulls against —
// see that section for why nulling against the raw input is impossible for any
// IIR crossover and what the honest gate is instead.
struct FxAp2 {
    f64 b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    f64 z1 = 0, z2 = 0;
    void set(f64 sr, f64 hz, f64 q) {
        const f64 w = 6.283185307179586 * hz / sr;
        const f64 c = std::cos(w), s = std::sin(w);
        const f64 al = s / (2.0 * q);
        const f64 a0 = 1.0 + al;
        b0 = (1.0 - al) / a0; b1 = (-2.0 * c) / a0; b2 = (1.0 + al) / a0;
        a1 = (-2.0 * c) / a0; a2 = (1.0 - al) / a0;
        z1 = z2 = 0;
    }
    f64 tick(f64 x) {                       // transposed direct form II
        const f64 y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// ---------------------------------------------------------------------------
// The three new effects: the common contract
//
// The same four properties testEffectContract() checks for every older stock
// effect, checked here for the three new ones so that this file can be proven
// on its own. If the merge also appends these URIs to kEffectUris[], the
// duplicate coverage is harmless.
// ---------------------------------------------------------------------------

static void testFxContract(PluginRegistry& reg) {
    banner("Shimmer / Bloom / Tape: the common contract");

    for (const char* uri : kFxUris) {
        const PluginDesc* d = fxFind(reg, uri);
        CHECK(d != nullptr, "%s: in the registry", uri);
        if (!d) continue;
        CHECK(d->format == PluginFormat::Internal && d->kind == PluginKind::Effect &&
              d->audioIn == 2 && d->audioOut == 2 && !d->hasMidiIn,
              "%s: stereo effect descriptor", uri);

        auto fx = fxMake(reg, *d);
        CHECK(fx != nullptr, "%s: instantiate + prepare", uri);
        if (!fx) continue;

        CHECK(fx->paramCount() == d->paramCount,
              "%s: descriptor param count matches the instance (%d)", uri, fx->paramCount());

        bool rtOk = true;
        for (int i = 0; i < fx->paramCount(); ++i)
            if (!fx->setParamRT(i, fx->paramInfo(i).def)) rtOk = false;
        CHECK(rtOk, "%s: every parameter accepts a realtime write", uri);

        // 1. Silence in, silence out — an EXACT zero, including the Tape's
        //    hiss generator, which at its default is not attenuated but absent.
        Buf in, out;
        f32 residue = 0.f;
        for (int b = 0; b < 8; ++b) {
            out.clear();
            fx->process(in.p, out.p, 2, kBlock);
            residue = std::fmax(residue, out.peak());
        }
        CHECK(residue == 0.f, "%s: silence in -> silence out, exactly (%.9f)", uri, (double)residue);

        // 2. A sine sweeping 20 Hz -> 18 kHz stays finite and bounded.
        bool fin = true;
        f32 peak = 0.f;
        f64 ph = 0.0;
        for (int b = 0; b < 400; ++b) {
            const f64 t = (f64)b / 400.0;
            const f64 f = 20.0 * std::pow(900.0, t);
            for (int i = 0; i < kBlock; ++i) {
                ph += 6.283185307179586 * f / kSR;
                in.l[(size_t)i] = in.r[(size_t)i] = 0.5f * (f32)std::sin(ph);
            }
            out.clear();
            fx->process(in.p, out.p, 2, kBlock);
            if (!out.finite()) { fin = false; break; }
            peak = std::fmax(peak, out.peak());
        }
        CHECK(fin, "%s: swept sine stays finite", uri);
        CHECK(peak < 8.f, "%s: swept sine stays bounded (peak %.3f)", uri, (double)peak);

        // 3. Every parameter swept end to end, both directions, while
        //    processing. One pass each so a fault is attributable.
        Noise ns;
        bool sweepOk = true;
        const char* badParam = "";
        for (int pi = 0; pi < fx->paramCount() && sweepOk; ++pi) {
            const ParamInfo& info = fx->paramInfo(pi);
            for (int b = 0; b < 120; ++b) {
                f32 t = (f32)b / 60.f;
                if (t > 1.f) t = 2.f - t;
                fx->setParam(pi, lerpf(info.min, info.max, t));
                for (int i = 0; i < kBlock; ++i)
                    in.l[(size_t)i] = in.r[(size_t)i] = 0.25f * ns.next();
                out.clear();
                fx->process(in.p, out.p, 2, kBlock);
                if (!out.finite() || out.peak() > 32.f) {
                    sweepOk = false;
                    badParam = info.name.c_str();
                    break;
                }
            }
            fx->setParam(pi, info.def);
        }
        CHECK(sweepOk, "%s: every parameter sweeps during processing without NaN%s%s",
              uri, sweepOk ? "" : " -- failed on ", badParam);

        // 4. ...and it is still a working device afterwards.
        for (int i = 0; i < kBlock; ++i)
            in.l[(size_t)i] = in.r[(size_t)i] = 0.25f * (f32)std::sin(6.2831853 * 440.0 * i / kSR);
        f32 after = 0.f;
        for (int b = 0; b < 16; ++b) {
            out.clear();
            fx->process(in.p, out.p, 2, kBlock);
            after = std::fmax(after, out.peak());
        }
        CHECK(after > 0.01f, "%s: still passes audio after the sweep (peak %.4f)",
              uri, (double)after);

        // 5. Bypass is a true wire, sample for sample.
        fx->setBypassed(true);
        Buf bin, bout;
        Noise bn;
        bool wire = true;
        for (int b = 0; b < 4 && wire; ++b) {
            for (int i = 0; i < kBlock; ++i) {
                bin.l[(size_t)i] = 0.4f * bn.next();
                bin.r[(size_t)i] = 0.4f * bn.next();
            }
            bout.clear();
            fx->process(bin.p, bout.p, 2, kBlock);
            for (int i = 0; i < kBlock; ++i)
                if (bout.l[(size_t)i] != bin.l[(size_t)i] || bout.r[(size_t)i] != bin.r[(size_t)i])
                    wire = false;
        }
        CHECK(wire, "%s: bypass is a bit-exact wire", uri);
        fx->setBypassed(false);
    }
}

// ---------------------------------------------------------------------------
// Shimmer: the climb
//
// THE SIGNATURE. A 440 Hz burst at Shift = +12 with the shifter carrying the
// whole feedback has to leave a tail whose energy WALKS UP the octaves — 880,
// then 1760, then 3520 — in that order, and ends up above where it started.
// A reverb that merely gets brighter is not a shimmer.
//
// The measurement is a Hann-windowed single bin at each octave, taken every
// 100 ms of the tail, and the assertions are about the ORDER in which each bin
// takes the lead. The absolute timing is a property of the tank's traversal
// time (Size), not of the effect existing, so the gate is the sequence and the
// table below is what says how fast.
// ---------------------------------------------------------------------------

static void testShimmerClimb(PluginRegistry& reg) {
    banner("Shimmer: the octave climb is measurable");

    const PluginDesc* d = fxFind(reg, "nxtakt:shimmer");
    CHECK(d != nullptr, "nxtakt:shimmer: in the registry");
    if (!d) return;
    auto p = fxMake(reg, *d);
    if (!p) return;

    fxSet(*p, "Decay", 20.f);
    fxSet(*p, "Size", 0.5f);
    fxSet(*p, "Shift", 3.f);            // +12
    fxSet(*p, "Shift Amount", 1.f);
    fxSet(*p, "Damping", 18000.f);
    fxSet(*p, "Low Cut", 60.f);
    fxSet(*p, "Mod Depth", 0.f);
    fxSet(*p, "Dry/Wet", 1.f);

    // 500 ms of 440 Hz, then eight seconds of nothing.
    const int burst  = (int)(0.5 * kSR);
    const int frames = (int)(8.5 * kSR);
    std::vector<f32> inL((size_t)frames, 0.f), inR((size_t)frames, 0.f);
    for (int i = 0; i < burst; ++i) {
        const f32 s = 0.5f * (f32)std::sin(6.283185307179586 * 440.0 * (f64)i / kSR);
        inL[(size_t)i] = inR[(size_t)i] = s;
    }
    std::vector<f32> oL, oR;
    fxRender(*p, inL, inR, kBlock, oL, oR);

    const f64 bins[5] = { 440.0, 880.0, 1760.0, 3520.0, 7040.0 };
    const size_t win = 4096;
    std::printf("  note  tail spectrum, dBFS per octave bin (window %zu = %.0f ms)\n",
                win, 1000.0 * (f64)win / kSR);
    std::printf("  note      t(s)     440     880    1760    3520    7040\n");

    // WHEN each rung PEAKS, which is the honest way to say "in sequence".
    //
    // The naive version -- the first moment each bin overtakes the one below it
    // -- is not a sequence at all: bins that are still 100 dB down cross each
    // other in whatever order the noise floor puts them, and a rung that is
    // climbing can pass the rung above it on the way up. The energy PEAK is
    // unambiguous: it is the moment that octave holds the most of what the tank
    // contains, and a shifter that transposes upward has to reach them in
    // order or it is not transposing upward.
    f64 peakT[5] = { -1, -1, -1, -1, -1 };
    f64 peakDb[5] = { -400, -400, -400, -400, -400 };
    const int steps = 120;
    for (int k = 0; k < steps; ++k) {
        const f64 t = 0.50 + 0.025 * (f64)k;
        const size_t at = (size_t)(t * kSR);
        if (at + win > oL.size()) break;
        f64 db[5];
        for (int b = 0; b < 5; ++b) {
            db[b] = fxBinDb(oL, at, win, bins[b]);
            if (db[b] > peakDb[b]) { peakDb[b] = db[b]; peakT[b] = t; }
        }
        if (k % 8 == 0)
            std::printf("  note    %6.3f  %6.1f  %6.1f  %6.1f  %6.1f  %6.1f\n",
                        t, db[0], db[1], db[2], db[3], db[4]);
    }

    std::printf("  note  peak of each octave: 440 at %.3f s (%.1f dB), 880 at %.3f s (%.1f dB), "
                "1760 at %.3f s (%.1f dB), 3520 at %.3f s (%.1f dB), 7040 at %.3f s (%.1f dB)\n",
                peakT[0], peakDb[0], peakT[1], peakDb[1], peakT[2], peakDb[2],
                peakT[3], peakDb[3], peakT[4], peakDb[4]);

    CHECK(peakDb[1] > -80.0 && peakDb[2] > -80.0 && peakDb[3] > -80.0,
          "the tail contains real energy at 880, 1760 and 3520 Hz (%.1f, %.1f, %.1f dB) — "
          "none of which the 440 Hz burst put there", peakDb[1], peakDb[2], peakDb[3]);
    CHECK(peakT[1] > peakT[0] && peakT[2] > peakT[1] && peakT[3] > peakT[2],
          "and each octave peaks AFTER the one below it: 440 at %.3f, 880 at %.3f, "
          "1760 at %.3f, 3520 at %.3f s. That ordering is the signature — a reverb "
          "that merely brightens has no order to it",
          peakT[0], peakT[1], peakT[2], peakT[3]);
    CHECK(peakT[4] > peakT[3],
          "the fourth rung too: 7040 peaks at %.3f s, after 3520 at %.3f s",
          peakT[4], peakT[3]);

    // The headline: at the moment 3520 is loudest, it is louder than 440 was
    // ever going to be from here on — two octaves of energy above a note the
    // tank was only ever fed at 440 Hz.
    {
        const size_t at = (size_t)(peakT[3] * kSR);
        const f64 f440  = fxBinDb(oL, at, win, 440.0);
        const f64 f3520 = fxBinDb(oL, at, win, 3520.0);
        CHECK(f3520 > f440,
              "at %.3f s the 3520 Hz bin (%.1f dB) exceeds the 440 Hz bin (%.1f dB) "
              "by %.1f dB — energy the input never contained",
              peakT[3], f3520, f440, f3520 - f440);
    }

    // The level check, which is what stops a reverb from being one people turn
    // up and then turn back down. Fully wet, at the defaults, against the same
    // noise fed straight through.
    {
        auto q = fxMake(reg, *d);
        if (q) {
            fxSet(*q, "Dry/Wet", 1.f);
            const int n = (int)(4.0 * kSR);
            std::vector<f32> nl((size_t)n), nr((size_t)n), wl, wr;
            Noise ns;
            for (int i = 0; i < n; ++i) { nl[(size_t)i] = 0.3f * ns.next(); nr[(size_t)i] = 0.3f * ns.next(); }
            fxRender(*q, nl, nr, kBlock, wl, wr);
            f64 dryE = 0.0, wetE = 0.0;
            for (int i = (int)(1.0 * kSR); i < n; ++i) {
                dryE += (f64)nl[(size_t)i] * nl[(size_t)i];
                wetE += (f64)wl[(size_t)i] * wl[(size_t)i];
            }
            const f64 rel = 10.0 * std::log10(wetE / (dryE > 0 ? dryE : 1.0));
            CHECK(rel > -10.0 && rel < 6.0,
                  "fully wet at the defaults, the tail sits %+.1f dB against the dry signal "
                  "— a wet path nobody has to make up for on the fader", rel);
        }
    }

    // THE CONTROL. The same tank with the shifter taken out of the loop
    // (Shift Amount 0) has to leave the tail exactly where the burst put it.
    // Without this the section would be measuring nothing but a reverb that
    // gets brighter, which is what any modulated tank does.
    {
        auto q = fxMake(reg, *d);
        if (q) {
            fxSet(*q, "Decay", 20.f);
            fxSet(*q, "Size", 0.5f);
            fxSet(*q, "Shift", 3.f);
            fxSet(*q, "Shift Amount", 0.f);      // the shifter is not in the loop
            fxSet(*q, "Damping", 18000.f);
            fxSet(*q, "Low Cut", 60.f);
            fxSet(*q, "Mod Depth", 0.f);
            fxSet(*q, "Dry/Wet", 1.f);
            std::vector<f32> qL, qR;
            fxRender(*q, inL, inR, kBlock, qL, qR);
            const size_t at = (size_t)(1.5 * kSR);
            const f64 f440  = fxBinDb(qL, at, win, 440.0);
            const f64 f3520 = fxBinDb(qL, at, win, 3520.0);
            std::printf("  note  control (Shift Amount 0) at 1.5 s: 440 %.1f dB, 3520 %.1f dB\n",
                        f440, f3520);
            CHECK(f440 > f3520 + 30.0,
                  "with Shift Amount at 0 the tail stays at 440 Hz (%.1f dB) and 3520 has "
                  "nothing in it (%.1f dB, %.1f dB down) — the ladder is the shifter and "
                  "not the tank", f440, f3520, f440 - f3520);
        }
    }
}

// ---------------------------------------------------------------------------
// Shimmer: stability and the tail
//
// A pitch shifter in a feedback loop is the classic way to build an oscillator
// by accident. Three things are checked, and the first two are the ones that
// decide whether this device can be shipped:
//
//   1. thirty seconds of full-scale noise with every knob at its most
//      dangerous setting cannot drive the output past +3 dBFS. The bound is
//      arithmetic — the soft clipper on every delay-line input caps the tank
//      contents at 1.35 and the output taps are averages of two lines — so
//      this test is checking the code, not hoping about the signal.
//   2. when the input stops, it decays. -60 dB, and how long it took.
//   3. the tail reaches TRUE ZERO and not a denormal asymptote.
// ---------------------------------------------------------------------------

static void testShimmerStability(PluginRegistry& reg) {
    banner("Shimmer: 30 s at maximum everything, then silence");

    const PluginDesc* d = fxFind(reg, "nxtakt:shimmer");
    if (!d) return;
    auto p = fxMake(reg, *d);
    if (!p) return;

    // Maximum everything that can add energy: longest decay, biggest tank,
    // widest shift, all of the feedback through the shifter, no damping, no
    // low cut, fastest and deepest modulation, fully wet.
    for (int i = 0; i < p->paramCount(); ++i) p->setParam(i, p->paramInfo(i).max);
    // ...and then the two that are LEAST safe at their minimum rather than
    // their maximum: no low cut and no damping is the setting that lets the
    // loop accumulate whatever the shifter cannot move.
    fxSet(*p, "Low Cut", 20.f);
    fxSet(*p, "Damping", 18000.f);
    fxSet(*p, "Shift", 4.f);
    fxSet(*p, "Dry/Wet", 1.f);
    const int decayIdx = paramIndex(*p, "Decay");
    const f32 decaySec = decayIdx >= 0 ? p->getParam(decayIdx) : 30.f;

    Buf in, out;
    Noise ns;
    f32 worst = 0.f;
    bool finite = true;
    const int blocks = (int)(30.0 * kSR / kBlock);
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < kBlock; ++i) {
            in.l[(size_t)i] = ns.next();            // full scale
            in.r[(size_t)i] = ns.next();
        }
        out.clear();
        p->process(in.p, out.p, 2, kBlock);
        if (!out.finite()) { finite = false; break; }
        worst = std::fmax(worst, out.peak());
    }
    const f64 worstDb = worst > 0.f ? 20.0 * std::log10((f64)worst) : -300.0;
    CHECK(finite, "30 s of full-scale noise stays finite");
    CHECK(worst <= 1.4125f,
          "and never exceeds +3 dBFS (peak %.4f = %+.2f dBFS, decay %.1f s)",
          (double)worst, worstDb, (double)decaySec);

    // Input stops. How long until -60 dB?
    f64 minus60 = -1.0;
    const int tailBlocks = (int)(2.0 * (f64)decaySec * kSR / kBlock) + 64;
    Buf sil;
    for (int b = 0; b < tailBlocks; ++b) {
        out.clear();
        p->process(sil.p, out.p, 2, kBlock);
        const f32 pk = out.peak();
        const f64 t  = (f64)(b + 1) * (f64)kBlock / kSR;
        if (pk < 1e-3f) { minus60 = t; break; }
    }
    CHECK(minus60 > 0.0, "the tail falls below -60 dBFS after %.2f s (decay is set to %.1f s)",
          minus60, (double)decaySec);
    CHECK(minus60 > 0.0 && minus60 <= (f64)decaySec * 1.5,
          "which is inside 1.5x the requested decay — no runaway (%.2f s vs %.1f s)",
          minus60, (double)decaySec);

    // --- and the tail reaches TRUE ZERO ------------------------------------
    // A separate instance at the SHORTEST decay and smallest tank, because the
    // property under test is "every state variable in the loop is flushed" and
    // not "how long 30 seconds of reverb takes to fall 700 dB". At the longest
    // decay it gets there too; it just takes four minutes of wall clock to
    // prove, which is not a thing to put in a suite that runs on every commit.
    {
        auto q = fxMake(reg, *d);
        if (q) {
            fxSet(*q, "Decay", 0.3f);
            fxSet(*q, "Size", 0.f);
            fxSet(*q, "Shift", 3.f);
            fxSet(*q, "Shift Amount", 0.5f);
            fxSet(*q, "Dry/Wet", 1.f);
            Buf in, out2;
            Noise ns2;
            for (int b = 0; b < 40; ++b) {
                for (int i = 0; i < kBlock; ++i) {
                    in.l[(size_t)i] = 0.8f * ns2.next();
                    in.r[(size_t)i] = 0.8f * ns2.next();
                }
                out2.clear();
                q->process(in.p, out2.p, 2, kBlock);
            }
            f64 zeroAt = -1.0;
            Buf sil2;
            for (int b = 0; b < (int)(20.0 * kSR / kBlock); ++b) {
                out2.clear();
                q->process(sil2.p, out2.p, 2, kBlock);
                if (out2.peak() == 0.f) { zeroAt = (f64)(b + 1) * (f64)kBlock / kSR; break; }
            }
            CHECK(zeroAt > 0.0,
                  "the tail reaches TRUE ZERO at %.2f s and stays there — not a denormal "
                  "asymptote the FPU spends the rest of the session maintaining", zeroAt);
            // ...and stays there, which is the half that a device with an
            // ungrounded modulator or a stuck LFO would fail.
            f32 after = 0.f;
            for (int b = 0; b < 400; ++b) {
                out2.clear();
                q->process(sil2.p, out2.p, 2, kBlock);
                after = std::fmax(after, out2.peak());
            }
            CHECK(after == 0.f, "and two more seconds of silence produce exactly zero (%.9f)",
                  (double)after);
        }
    }
}

// ---------------------------------------------------------------------------
// Bloom: the crossover
//
// THE GATE FOR ANY MULTIBAND. If the three bands do not recombine into what
// went in, every other measurement in this section is measuring the crossover
// rather than the compressor.
//
// WHAT "SUMS FLAT" CAN AND CANNOT MEAN FOR AN IIR CROSSOVER, stated once,
// because it is the thing multiband devices are usually dishonest about: an
// LR4 pair sums to a second-order ALLPASS, never to unity. That is not a
// defect of this implementation, it is what Linkwitz-Riley IS; the magnitude
// is flat and the phase is not. So a null against the raw input is
// mathematically impossible for any device that does not buy linear phase with
// latency, and the strongest true statement is the pair below:
//
//   * the split-and-sum nulls below -80 dB against the input passed through an
//     INDEPENDENTLY WRITTEN AP2(fLow) -> AP2(fHigh) — which proves both the LR4
//     identity and, crucially, that the low band carries its AP2(fHigh)
//     compensation. Remove that one filter and this test fails by 6 dB.
//   * the magnitude of the split-and-sum is flat across the audio band.
//
// The dynamics are made inert by setting both ratios to 1, which makes every
// band gain exactly 1.0f — not nearly — so what is measured is the crossover.
// ---------------------------------------------------------------------------

static void testBloomCrossover(PluginRegistry& reg) {
    banner("Bloom: the crossover sums flat (the multiband gate)");

    const PluginDesc* d = fxFind(reg, "nxtakt:bloom");
    CHECK(d != nullptr, "nxtakt:bloom: in the registry");
    if (!d) return;
    auto p = fxMake(reg, *d);
    if (!p) return;

    const f32 fLow = 250.f, fHigh = 2500.f;
    fxSet(*p, "Low Split", fLow);
    fxSet(*p, "High Split", fHigh);
    fxSet(*p, "Depth", 1.f);            // NOT the wire path
    fxSet(*p, "Up Ratio", 1.f);         // ...but the dynamics are inert
    fxSet(*p, "Down Ratio", 1.f);
    fxSet(*p, "Dry/Wet", 1.f);

    // Pink-ish noise: white through a -3 dB/octave ladder. A crossover null is
    // a broadband claim and white noise under-weights the bottom two octaves
    // where an uncompensated allpass does its damage.
    const int frames = 1 << 17;
    std::vector<f32> inL((size_t)frames), inR((size_t)frames);
    {
        Noise ns;
        f32 b0 = 0.f, b1 = 0.f, b2 = 0.f;
        for (int i = 0; i < frames; ++i) {
            const f32 w = ns.next();
            b0 = 0.99765f * b0 + w * 0.0990460f;
            b1 = 0.96300f * b1 + w * 0.2965164f;
            b2 = 0.57000f * b2 + w * 1.0526913f;
            const f32 v = clampv((b0 + b1 + b2 + w * 0.1848f) * 0.20f, -1.f, 1.f);
            inL[(size_t)i] = inR[(size_t)i] = v;
        }
    }

    std::vector<f32> oL, oR;
    fxRender(*p, inL, inR, kBlock, oL, oR);

    // The reference: the same input through two independently written
    // second-order allpasses at the same two corners.
    FxAp2 a1, a2;
    a1.set(kSR, (f64)fLow, 0.70710678118654752);
    a2.set(kSR, (f64)fHigh, 0.70710678118654752);
    std::vector<f64> ref((size_t)frames);
    for (int i = 0; i < frames; ++i) ref[(size_t)i] = a2.tick(a1.tick((f64)inL[(size_t)i]));

    // Skip the first 20000 frames: the device's coefficient glide and both
    // filter states have to settle before a null means anything.
    const int from = 20000;
    f64 sigE = 0.0, errE = 0.0, worst = 0.0;
    for (int i = from; i < frames; ++i) {
        const f64 e = (f64)oL[(size_t)i] - ref[(size_t)i];
        errE += e * e;
        sigE += ref[(size_t)i] * ref[(size_t)i];
        worst = std::fmax(worst, std::fabs(e));
    }
    const f64 nullDb = (errE <= 0.0 || sigE <= 0.0)
                           ? -300.0 : 10.0 * std::log10(errE / sigE);
    std::printf("  note  residual %.1f dB, worst sample error %.3e\n", nullDb, worst);
    CHECK(nullDb < -80.0,
          "the three bands recombine into AP2(%.0f) o AP2(%.0f) applied to the input, "
          "to %.1f dB — the low band's allpass compensation is present and correct",
          (double)fLow, (double)fHigh, nullDb);

    // ...and the magnitude of that sum is flat, which is the half of "sums
    // flat" that a listener actually hears.
    {
        auto q = fxMake(reg, *d);
        if (q) {
            fxSet(*q, "Low Split", fLow);
            fxSet(*q, "High Split", fHigh);
            fxSet(*q, "Depth", 1.f);
            fxSet(*q, "Up Ratio", 1.f);
            fxSet(*q, "Down Ratio", 1.f);
            fxSet(*q, "Dry/Wet", 1.f);
            f64 worstDev = 0.0, atF = 0.0;
            for (int k = 0; k < 25; ++k) {
                const f64 f = 30.0 * std::pow(18000.0 / 30.0, (f64)k / 24.0);
                const f64 g = fxProbeDb(*q, f, 0.25f, 40);
                if (std::fabs(g) > worstDev) { worstDev = std::fabs(g); atF = f; }
            }
            CHECK(worstDev < 0.05,
                  "and its magnitude is flat to %.4f dB across 30 Hz - 18 kHz "
                  "(worst at %.0f Hz)", worstDev, atF);
        }
    }
}

// ---------------------------------------------------------------------------
// Bloom: both directions, per band
//
// Downward compression is what every compressor does. UPWARD is the signature,
// and it is checked per band because a multiband that only works in the middle
// is a single-band with extra filters.
//
// The arithmetic being asserted, once:
//
//     down:  out = thrDown + (in - thrDown) / ratio      for in > thrDown
//     up:    out = thrUp   - (thrUp - in)  / ratio       for in < thrUp
//
// with thrDown = -15 dBFS and thrUp = -25 dBFS, both fixed by the device. The
// two bands that are not under test have their gain pulled to the floor so
// their crossover leakage cannot contaminate the number.
// ---------------------------------------------------------------------------

static void testBloomDynamics(PluginRegistry& reg) {
    banner("Bloom: downward and UPWARD compression, per band");

    const PluginDesc* d = fxFind(reg, "nxtakt:bloom");
    if (!d) return;

    const char* gains[3] = { "Low Gain", "Mid Gain", "High Gain" };
    const char* names[3] = { "low", "mid", "high" };
    const f64   probe[3] = { 80.0, 800.0, 6000.0 };
    const f32   kThrDown = -15.f, kThrUp = -25.f;

    // --- downward -----------------------------------------------------------
    for (int b = 0; b < 3; ++b) {
        auto p = fxMake(reg, *d);
        if (!p) continue;
        fxSet(*p, "Low Split", 250.f);
        fxSet(*p, "High Split", 2500.f);
        fxSet(*p, "Depth", 1.f);
        fxSet(*p, "Up Ratio", 1.f);           // downward only
        fxSet(*p, "Down Ratio", 4.f);
        fxSet(*p, "Attack", 1.f);
        fxSet(*p, "Release", 1000.f);
        for (int k = 0; k < 3; ++k) if (k != b) fxSet(*p, gains[k], -24.f);

        const f64 g = fxProbeDb(*p, probe[b], 1.f, 60, 400);
        const f64 want = (f64)kThrDown + (0.0 - (f64)kThrDown) / 4.0;   // -3.75 dB out
        CHECK(std::fabs(g - want) < 1.0,
              "%s band: 0 dBFS in comes out at %+.2f dB, the 4:1 math says %+.2f dB "
              "(%.2f dB of gain reduction)", names[b], g, want, -g);
    }

    // --- UPWARD -------------------------------------------------------------
    for (int b = 0; b < 3; ++b) {
        auto p = fxMake(reg, *d);
        if (!p) continue;
        fxSet(*p, "Low Split", 250.f);
        fxSet(*p, "High Split", 2500.f);
        fxSet(*p, "Depth", 1.f);
        fxSet(*p, "Up Ratio", 4.f);
        fxSet(*p, "Down Ratio", 1.f);         // upward only
        fxSet(*p, "Attack", 1.f);
        fxSet(*p, "Release", 1000.f);
        for (int k = 0; k < 3; ++k) if (k != b) fxSet(*p, gains[k], -24.f);

        const f32 amp = 0.01f;                                  // -40 dBFS
        const f64 g   = fxProbeDb(*p, probe[b], amp, 60, 400);
        const f64 want = ((f64)kThrUp - (-40.0)) * (1.0 - 1.0 / 4.0);   // +11.25 dB
        CHECK(std::fabs(g - want) < 1.0,
              "%s band: -40 dBFS in RISES by %+.2f dB, the 1:4 upward math says %+.2f dB "
              "(out at %.2f dBFS) — this is the signature",
              names[b], g, want, -40.0 + g);
    }

    // --- Depth scales both, and Depth 0 is a wire ---------------------------
    {
        auto p = fxMake(reg, *d);
        if (p) {
            fxSet(*p, "Depth", 0.5f);
            fxSet(*p, "Up Ratio", 4.f);
            fxSet(*p, "Down Ratio", 1.f);
            fxSet(*p, "Attack", 1.f);
            fxSet(*p, "Release", 1000.f);
            fxSet(*p, "Low Gain", -24.f);
            fxSet(*p, "High Gain", -24.f);
            const f64 g = fxProbeDb(*p, 800.0, 0.01f, 60, 400);
            CHECK(std::fabs(g - 11.25 * 0.5) < 1.0,
                  "Depth 0.5 gives half the upward action in dB (%+.2f, half of %+.2f)",
                  g, 11.25);
        }
    }

    {
        auto p = fxMake(reg, *d);
        if (p) {
            // Depth 0, every gain at unity, fully wet: the documented wire.
            Buf in, out;
            Noise ns;
            bool wire = true;
            f32 worst = 0.f;
            for (int b = 0; b < 40 && wire; ++b) {
                for (int i = 0; i < kBlock; ++i) {
                    in.l[(size_t)i] = 0.5f * ns.next();
                    in.r[(size_t)i] = 0.5f * ns.next();
                }
                out.clear();
                p->process(in.p, out.p, 2, kBlock);
                for (int i = 0; i < kBlock; ++i) {
                    worst = std::fmax(worst, std::fabs(out.l[(size_t)i] - in.l[(size_t)i]));
                    if (out.l[(size_t)i] != in.l[(size_t)i] ||
                        out.r[(size_t)i] != in.r[(size_t)i]) wire = false;
                }
            }
            CHECK(wire, "Depth 0 with unity gains is a BIT-EXACT wire (worst diff %.9f) — "
                        "the crossover keeps running underneath so that automating Depth "
                        "off zero does not click, but nothing it computes reaches the output",
                  (double)worst);
        }
    }

    // Depth 0 is a wire on the OUTPUT and not a bypass of the filters: raising
    // Depth off zero must produce a band split that is already tracking.
    {
        auto p = fxMake(reg, *d);
        if (p) {
            fxSet(*p, "Depth", 0.f);
            fxSet(*p, "Up Ratio", 6.f);
            fxSet(*p, "Down Ratio", 6.f);
            Buf in, out;
            Noise ns;
            for (int b = 0; b < 60; ++b) {
                for (int i = 0; i < kBlock; ++i) {
                    in.l[(size_t)i] = 0.3f * ns.next();
                    in.r[(size_t)i] = 0.3f * ns.next();
                }
                out.clear();
                p->process(in.p, out.p, 2, kBlock);
            }
            fxSet(*p, "Depth", 1.f);
            for (int i = 0; i < kBlock; ++i) {
                in.l[(size_t)i] = 0.3f * ns.next();
                in.r[(size_t)i] = 0.3f * ns.next();
            }
            out.clear();
            p->process(in.p, out.p, 2, kBlock);
            // A cold crossover would put a transient of several times the
            // signal level into the first block. A warm one cannot.
            CHECK(out.peak() < 4.f * 0.3f,
                  "and Depth 0 -> 1 in one block does not spike (peak %.3f on a 0.3 signal)",
                  (double)out.peak());
        }
    }
}

// ---------------------------------------------------------------------------
// Tape
//
// Four gates, each of which is the thing the corresponding knob claims to be:
//
//   1. WOW is a pitch deviation of a stated size, measured by timing the zero
//      crossings of a 1 kHz tone and converting to cents. The device defines
//      Wow Depth IN CENTS and solves the delay excursion for it, so this test
//      asserts a number rather than "it wobbles".
//   2. the THD curve rises monotonically with Drive, H2 is present when Bias
//      is up and ABSENT (below -100 dB) when Bias is zero, because the shaper
//      is exactly odd there.
//   3. the Speed switch moves the head bump.
//   4. Hiss at its default is zero. Not quiet. Zero.
// ---------------------------------------------------------------------------

// Every upward zero crossing of x, linearly interpolated, as a fractional
// sample index. Exact enough for cents at 1 kHz: the interpolation error on a
// full-scale sine is a few parts per million of a period.
static void fxZeroCross(const std::vector<f32>& x, size_t from, size_t to,
                        std::vector<f64>& out) {
    out.clear();
    for (size_t i = from + 1; i < to && i < x.size(); ++i) {
        if (x[i - 1] <= 0.f && x[i] > 0.f) {
            const f64 a = (f64)x[i - 1], b = (f64)x[i];
            out.push_back((f64)(i - 1) + (b != a ? (-a / (b - a)) : 0.0));
        }
    }
}

static void testTape(PluginRegistry& reg) {
    banner("Tape: wow in cents, the THD curve, the speed switch, and silence");

    const PluginDesc* d = fxFind(reg, "nxtakt:tape");
    CHECK(d != nullptr, "nxtakt:tape: in the registry");
    if (!d) return;

    // --- latency is real and reported --------------------------------------
    {
        auto p = fxMake(reg, *d);
        if (p) {
            const int lat = p->latencyFrames();
            CHECK(lat > 0, "latencyFrames() reports the record-to-repro gap (%d frames = %.2f ms)",
                  lat, 1000.0 * (f64)lat / kSR);
            // ...and it is the truth: an impulse comes out exactly there.
            fxSet(*p, "Wow Depth", 0.f);
            fxSet(*p, "Flutter", 0.f);
            fxSet(*p, "Drive", 0.f);
            fxSet(*p, "Bias", 0.f);
            fxSet(*p, "Head Bump", 0.f);
            fxSet(*p, "HF Rolloff", 0.f);
            fxSet(*p, "Crosstalk", 0.f);
            fxSet(*p, "Dry/Wet", 0.f);          // the dry path: an integer tap
            const int n = lat + 2048;
            std::vector<f32> inL((size_t)n, 0.f), inR((size_t)n, 0.f), oL, oR;
            inL[0] = inR[0] = 1.f;
            fxRender(*p, inL, inR, kBlock, oL, oR);
            int at = -1;
            f32 pk = 0.f;
            for (int i = 0; i < n; ++i)
                if (std::fabs(oL[(size_t)i]) > pk) { pk = std::fabs(oL[(size_t)i]); at = i; }
            CHECK(at == lat,
                  "and an impulse arrives at frame %d, which is exactly what it said (%d)",
                  at, lat);
        }
    }

    // --- 1. wow, in cents ---------------------------------------------------
    {
        auto p = fxMake(reg, *d);
        if (p) {
            fxSet(*p, "Drive", 0.f);
            fxSet(*p, "Bias", 0.f);
            fxSet(*p, "Head Bump", 0.f);
            fxSet(*p, "HF Rolloff", 0.f);
            fxSet(*p, "Crosstalk", 0.f);
            fxSet(*p, "Flutter", 0.f);
            fxSet(*p, "Wow Rate", 1.f);
            fxSet(*p, "Wow Depth", 1.f);
            fxSet(*p, "Dry/Wet", 1.f);

            const int frames = (int)(6.0 * kSR);
            std::vector<f32> inL((size_t)frames), inR((size_t)frames), oL, oR;
            for (int i = 0; i < frames; ++i)
                inL[(size_t)i] = inR[(size_t)i] =
                    0.7f * (f32)std::sin(6.283185307179586 * 1000.0 * (f64)i / kSR);
            fxRender(*p, inL, inR, kBlock, oL, oR);

            std::vector<f64> zc;
            fxZeroCross(oL, (size_t)(1.0 * kSR), (size_t)(5.5 * kSR), zc);
            // Average over 10 periods: 10 ms, which is 1% of a 1 Hz wow cycle,
            // so the smoothing costs a fraction of a cent and buys immunity to
            // interpolation noise.
            f64 hi = -1e9, lo = 1e9;
            for (size_t i = 10; i < zc.size(); ++i) {
                const f64 per  = (zc[i] - zc[i - 10]) / 10.0;
                const f64 cents = 1200.0 * std::log2((kSR / 1000.0) / per);
                hi = std::fmax(hi, cents);
                lo = std::fmin(lo, cents);
            }
            const f64 dev = 0.5 * (hi - lo);
            std::printf("  note  wow at full depth, 1 Hz: %+.2f / %+.2f cents (+-%.2f)\n",
                        hi, lo, dev);
            CHECK(zc.size() > 1000, "the 1 kHz tone gave %zu zero crossings to time", zc.size());
            CHECK(dev >= 5.0 && dev <= 32.0,
                  "full Wow Depth is +-%.2f cents at 1 Hz, which is in the range a machine "
                  "wobbles by", dev);
            // The knob is defined in cents and the excursion is SOLVED for it,
            // so the upward deviation is not "about right", it is the number.
            CHECK(std::fabs(hi - 30.0) < 1.0,
                  "and the UPWARD deviation is %+.2f cents against the 30.00 the knob "
                  "promises — the excursion is solved from the cents, not tuned to it", hi);
            // The two halves are not equal and cannot be: a delay modulation of
            // +-d gives a pitch ratio of 1+k one way and 1-k the other, and
            // 1200*log2(1-k) is further from zero than 1200*log2(1+k). 0.5
            // cents of asymmetry at 30 cents of depth is exactly that, and NOT
            // a read pointer that ran out of line.
            CHECK(std::fabs(hi + lo) < 3.0,
                  "the two halves differ by %.2f cents, which is the arithmetic of pitch "
                  "ratios (%+.2f up, %+.2f down) and not a transport out of headroom",
                  std::fabs(hi + lo), hi, lo);
        }
    }
    {
        // Wow at zero is a machine that does not wobble at all.
        auto p = fxMake(reg, *d);
        if (p) {
            fxSet(*p, "Drive", 0.f); fxSet(*p, "Bias", 0.f);
            fxSet(*p, "Head Bump", 0.f); fxSet(*p, "HF Rolloff", 0.f);
            fxSet(*p, "Crosstalk", 0.f); fxSet(*p, "Flutter", 0.f);
            fxSet(*p, "Wow Depth", 0.f); fxSet(*p, "Dry/Wet", 1.f);
            const int frames = (int)(3.0 * kSR);
            std::vector<f32> inL((size_t)frames), inR((size_t)frames), oL, oR;
            for (int i = 0; i < frames; ++i)
                inL[(size_t)i] = inR[(size_t)i] =
                    0.7f * (f32)std::sin(6.283185307179586 * 1000.0 * (f64)i / kSR);
            fxRender(*p, inL, inR, kBlock, oL, oR);
            std::vector<f64> zc;
            fxZeroCross(oL, (size_t)(1.0 * kSR), (size_t)(2.8 * kSR), zc);
            f64 worst = 0.0;
            for (size_t i = 10; i < zc.size(); ++i) {
                const f64 per = (zc[i] - zc[i - 10]) / 10.0;
                worst = std::fmax(worst, std::fabs(1200.0 * std::log2((kSR / 1000.0) / per)));
            }
            CHECK(worst < 0.5, "Wow Depth 0 leaves %.3f cents of deviation", worst);
        }
    }

    // --- 2. the THD curve ---------------------------------------------------
    {
        const f32 drives[6] = { 0.f, 3.f, 6.f, 12.f, 18.f, 24.f };
        f64 h2b[6], h3b[6], h2n[6], h3n[6];
        for (int bias = 0; bias < 2; ++bias) {
            for (int k = 0; k < 6; ++k) {
                auto p = fxMake(reg, *d);
                if (!p) continue;
                fxSet(*p, "Drive", drives[k]);
                fxSet(*p, "Bias", bias ? 0.5f : 0.f);
                fxSet(*p, "Speed", 2.f);
                fxSet(*p, "Wow Depth", 0.f);
                fxSet(*p, "Flutter", 0.f);
                fxSet(*p, "Head Bump", 0.f);
                fxSet(*p, "HF Rolloff", 0.f);
                fxSet(*p, "Crosstalk", 0.f);
                fxSet(*p, "Dry/Wet", 1.f);

                const int frames = 1 << 16;
                std::vector<f32> inL((size_t)frames), inR((size_t)frames), oL, oR;
                for (int i = 0; i < frames; ++i)
                    inL[(size_t)i] = inR[(size_t)i] =
                        0.5f * (f32)std::sin(6.283185307179586 * 1000.0 * (f64)i / kSR);
                fxRender(*p, inL, inR, kBlock, oL, oR);
                const size_t at = (size_t)(0.2 * kSR), win = 32768;
                const f64 f1 = fxBinDb(oL, at, win, 1000.0);
                const f64 f2 = fxBinDb(oL, at, win, 2000.0);
                const f64 f3 = fxBinDb(oL, at, win, 3000.0);
                if (bias) { h2b[k] = f2 - f1; h3b[k] = f3 - f1; }
                else      { h2n[k] = f2 - f1; h3n[k] = f3 - f1; }
            }
        }
        std::printf("  note  THD vs Drive at -6 dBFS in, 1 kHz (dB relative to H1)\n");
        std::printf("  note    Drive     H2(bias 0)  H3(bias 0)  H2(bias .5) H3(bias .5)\n");
        for (int k = 0; k < 6; ++k)
            std::printf("  note    %5.1f dB   %8.1f    %8.1f    %8.1f    %8.1f\n",
                        (double)drives[k], h2n[k], h3n[k], h2b[k], h3b[k]);

        bool mono3 = true, mono3b = true, mono2 = true;
        for (int k = 1; k < 6; ++k) {
            if (h3n[k] <= h3n[k - 1]) mono3 = false;
            if (h3b[k] <= h3b[k - 1]) mono3b = false;
        }
        // H2 rises to the knee and then falls back, because a hard-driven
        // tanh(g*x + b) stops caring about b. See the device header; the gate
        // is the rise up to +12 dB, and PRESENCE above it.
        for (int k = 1; k < 4; ++k) if (h2b[k] <= h2b[k - 1]) mono2 = false;
        CHECK(mono3, "H3 rises monotonically with Drive at Bias 0 (%.1f dB -> %.1f dB over 0..24)",
              h3n[0], h3n[5]);
        CHECK(mono3b, "and at Bias 0.5 too (%.1f -> %.1f dB) — the THD curve is monotone in Drive",
              h3b[0], h3b[5]);
        CHECK(mono2, "H2 rises with Drive up to the knee (%.1f dB at 0 -> %.1f dB at 12)",
              h2b[0], h2b[3]);
        {
            bool present = true;
            for (int k = 0; k < 6; ++k) if (h2b[k] <= -40.0) present = false;
            CHECK(present, "and stays present past it (%.1f dB at 24 dB of drive, where the "
                           "shaper has converged on an odd limiter)", h2b[5]);
        }

        bool h2gone = true;
        for (int k = 0; k < 6; ++k) if (h2n[k] > -100.0) h2gone = false;
        CHECK(h2gone,
              "at Bias 0 the shaper is exactly odd and H2 is ABSENT at every drive "
              "(worst %.1f dB)", *std::max_element(h2n, h2n + 6));
        CHECK(h2b[3] > -60.0 && h2b[3] > h2n[3] + 40.0,
              "at Bias 0.5 the even harmonic is there: H2 is %.1f dB at 12 dB of drive, "
              "%.1f dB above where an odd shaper leaves it", h2b[3], h2b[3] - h2n[3]);
    }

    // --- 3. the speed switch moves the head bump ----------------------------
    {
        f64 peakAt[3] = { 0, 0, 0 }, peakDb[3] = { -300, -300, -300 };
        for (int s = 0; s < 3; ++s) {
            auto p = fxMake(reg, *d);
            if (!p) continue;
            fxSet(*p, "Drive", 0.f);
            fxSet(*p, "Bias", 0.f);
            fxSet(*p, "Speed", (f32)s);
            fxSet(*p, "Wow Depth", 0.f);
            fxSet(*p, "Flutter", 0.f);
            fxSet(*p, "Head Bump", 9.f);
            fxSet(*p, "HF Rolloff", 0.f);
            fxSet(*p, "Crosstalk", 0.f);
            fxSet(*p, "Dry/Wet", 1.f);
            for (int k = 0; k < 40; ++k) {
                const f64 f = 25.0 * std::pow(400.0 / 25.0, (f64)k / 39.0);
                const f64 g = fxProbeDb(*p, f, 0.2f, 30, 120);
                if (g > peakDb[s]) { peakDb[s] = g; peakAt[s] = f; }
            }
        }
        std::printf("  note  head bump centre: 7.5 ips %.0f Hz (%+.1f dB), "
                    "15 ips %.0f Hz (%+.1f dB), 30 ips %.0f Hz (%+.1f dB)\n",
                    peakAt[0], peakDb[0], peakAt[1], peakDb[1], peakAt[2], peakDb[2]);
        CHECK(peakAt[0] < peakAt[1] && peakAt[1] < peakAt[2],
              "the Speed switch moves the head bump up with the transport "
              "(%.0f -> %.0f -> %.0f Hz)", peakAt[0], peakAt[1], peakAt[2]);
        CHECK(peakAt[2] / peakAt[0] > 1.5,
              "and it moves it far enough to hear (%.2fx from 7.5 to 30 ips)",
              peakAt[2] / peakAt[0]);
        CHECK(peakDb[1] > 6.0, "with 9 dB asked for, %.1f dB arrives at the centre", peakDb[1]);
    }

    // --- 3b. and the HF rolloff tracks it too -------------------------------
    {
        f64 hf[3];
        for (int s = 0; s < 3; ++s) {
            auto p = fxMake(reg, *d);
            if (!p) continue;
            fxSet(*p, "Drive", 0.f); fxSet(*p, "Bias", 0.f);
            fxSet(*p, "Speed", (f32)s);
            fxSet(*p, "Wow Depth", 0.f); fxSet(*p, "Flutter", 0.f);
            fxSet(*p, "Head Bump", 0.f); fxSet(*p, "HF Rolloff", 1.f);
            fxSet(*p, "Crosstalk", 0.f); fxSet(*p, "Dry/Wet", 1.f);
            hf[s] = fxProbeDb(*p, 12000.0, 0.2f, 240, 120);
        }
        std::printf("  note  response at 12 kHz: 7.5 ips %+.2f dB, 15 ips %+.2f dB, "
                    "30 ips %+.2f dB\n", hf[0], hf[1], hf[2]);
        CHECK(hf[0] < hf[1] && hf[1] < hf[2],
              "the HF loss tracks the speed as well: a slower machine is darker");
    }

    // --- 4. hiss at the default is exactly zero -----------------------------
    {
        auto p = fxMake(reg, *d);
        if (p) {
            // Everything else busy; only the input is silent.
            fxSet(*p, "Drive", 18.f);
            fxSet(*p, "Bias", 0.8f);
            fxSet(*p, "Wow Depth", 1.f);
            fxSet(*p, "Flutter", 1.f);
            fxSet(*p, "Head Bump", 9.f);
            fxSet(*p, "Crosstalk", 1.f);
            fxSet(*p, "Dry/Wet", 1.f);
            Buf in, out;
            f32 worst = 0.f;
            for (int b = 0; b < 200; ++b) {
                out.clear();
                p->process(in.p, out.p, 2, kBlock);
                worst = std::fmax(worst, out.peak());
            }
            CHECK(worst == 0.f,
                  "Hiss at its default puts EXACTLY zero into a silent channel (%.9f) — "
                  "not -90 dBFS, not a denormal: the term is not added", (double)worst);
        }
    }
    {
        auto p = fxMake(reg, *d);
        if (p) {
            fxSet(*p, "Hiss", -50.f);
            Buf in, out;
            f32 worst = 0.f;
            for (int b = 0; b < 40; ++b) {
                out.clear();
                p->process(in.p, out.p, 2, kBlock);
                worst = std::fmax(worst, out.peak());
            }
            const f64 db = worst > 0.f ? 20.0 * std::log10((f64)worst) : -300.0;
            CHECK(worst > 0.f && db > -70.0 && db < -30.0,
                  "and asking for hiss produces it, at about the level asked for "
                  "(%.1f dBFS peak for a -50 dB setting)", db);
        }
    }
}

// ---------------------------------------------------------------------------
// Block-size invariance for the three
//
// The bar is BIT-IDENTICAL, not close. Every LFO phase, every grain phase and
// every noise counter in these three devices advances exactly once per SAMPLE
// and never once per block, and both new devices with biquads take their
// coefficient-snap decision per sample for the same reason. This is the test
// that says whether that is true rather than intended.
//
// 1024 is in the list on purpose: it is four times the prepared block size, so
// this also says the devices PROCESS an oversized block rather than degrading
// to passthrough the way Pulse and the Rack must.
// ---------------------------------------------------------------------------

static void testFxBlockInvariance(PluginRegistry& reg) {
    banner("Shimmer / Bloom / Tape: the output does not depend on the block size");

    const int kFrames = 24000;
    std::vector<f32> inL((size_t)kFrames), inR((size_t)kFrames);
    Noise ns;
    for (int i = 0; i < kFrames; ++i) {
        inL[(size_t)i] = 0.3f * ns.next();
        inR[(size_t)i] = 0.3f * ns.next();
    }

    for (const char* uri : kFxUris) {
        const PluginDesc* d = fxFind(reg, uri);
        if (!d) continue;
        const std::string u = uri;

        // Settings that make every modulator, detector and counter move.
        auto build = [&]() -> std::unique_ptr<PluginInstance> {
            auto p = fxMake(reg, *d);
            if (!p) return p;
            if (u == "nxtakt:shimmer") {
                fxSet(*p, "Decay", 8.f);
                fxSet(*p, "Size", 0.7f);
                fxSet(*p, "Shift", 3.f);
                fxSet(*p, "Shift Amount", 0.6f);
                fxSet(*p, "Mod Rate", 3.f);
                fxSet(*p, "Mod Depth", 1.f);
                fxSet(*p, "Pre-Delay", 37.f);
                fxSet(*p, "Dry/Wet", 0.8f);
            } else if (u == "nxtakt:bloom") {
                fxSet(*p, "Depth", 1.f);
                fxSet(*p, "Up Ratio", 8.f);
                fxSet(*p, "Down Ratio", 6.f);
                fxSet(*p, "Attack", 0.5f);
                fxSet(*p, "Release", 40.f);
                fxSet(*p, "Low Gain", 3.f);
                fxSet(*p, "High Gain", -2.f);
                fxSet(*p, "Input", 6.f);
                fxSet(*p, "Dry/Wet", 0.7f);
            } else {
                fxSet(*p, "Drive", 12.f);
                fxSet(*p, "Bias", 0.6f);
                fxSet(*p, "Speed", 0.f);
                fxSet(*p, "Wow Rate", 3.3f);
                fxSet(*p, "Wow Depth", 0.8f);
                fxSet(*p, "Flutter", 0.9f);
                fxSet(*p, "Head Bump", 6.f);
                fxSet(*p, "Hiss", -55.f);
                fxSet(*p, "Crosstalk", 0.6f);
                fxSet(*p, "Dry/Wet", 0.75f);
            }
            return p;
        };

        std::vector<f32> refL, refR, altL, altR;
        auto ref = build();
        if (!ref) continue;
        fxRender(*ref, inL, inR, kBlock, refL, refR);

        f32 worst = 0.f;
        for (int chunk : { 1, 7, 64, 300, 1024 }) {
            auto alt = build();
            if (!alt) break;
            fxRender(*alt, inL, inR, chunk, altL, altR);
            f32 diff = 0.f;
            for (int i = 0; i < kFrames; ++i) {
                diff = std::fmax(diff, std::fabs(refL[(size_t)i] - altL[(size_t)i]));
                diff = std::fmax(diff, std::fabs(refR[(size_t)i] - altR[(size_t)i]));
            }
            CHECK(diff == 0.f,
                  "%s: blocks of %d are bit-identical to blocks of %d (max diff %.9f)",
                  uri, chunk, kBlock, (double)diff);
            worst = std::fmax(worst, diff);
        }
        CHECK(worst == 0.f, "%s: an oversized block is processed, not degraded", uri);
    }
}

// ---------------------------------------------------------------------------
// Factory presets
//
// The same three properties Spectra's preset section checks: Init is exactly
// the constructor, every preset stays inside every declared range, and loading
// one from any other state lands on the same place as loading it into a fresh
// instance.
// ---------------------------------------------------------------------------

static void testFxPresets(PluginRegistry& reg) {
    banner("Shimmer / Bloom / Tape: factory presets");

    for (const char* uri : kFxUris) {
        const PluginDesc* d = fxFind(reg, uri);
        if (!d) continue;
        auto p = fxMake(reg, *d);
        if (!p) continue;

        CHECK(p->presetCount() >= 4 && p->presetCount() <= 12,
              "%s: ships %d factory presets", uri, p->presetCount());
        CHECK(p->presetName(-1) == nullptr && p->presetName(p->presetCount()) == nullptr,
              "%s: presetName() is null out of range", uri);

        bool named = true;
        for (int i = 0; i < p->presetCount(); ++i) {
            const char* n = p->presetName(i);
            if (!n || !*n) named = false;
        }
        CHECK(named, "%s: every preset has a name", uri);
        if (p->presetCount() > 0)
            CHECK(std::strcmp(p->presetName(0), "Init") == 0,
                  "%s: preset 0 is Init", uri);

        // Init restores the constructor exactly, from any state.
        {
            auto a = fxMake(reg, *d);
            if (a) {
                for (int i = 0; i < a->paramCount(); ++i)
                    a->setParam(i, a->paramInfo(i).max);
                a->loadPreset(0);
                bool def = true;
                for (int i = 0; i < a->paramCount(); ++i)
                    if (a->getParam(i) != a->paramInfo(i).def) def = false;
                CHECK(def, "%s: Init restores every parameter to its constructor default exactly",
                      uri);
            }
        }

        // Every preset, every parameter, inside its declared range; and a
        // preset loaded over a busy state equals the same preset loaded fresh.
        bool inRange = true, stable = true;
        for (int k = 0; k < p->presetCount(); ++k) {
            auto a = fxMake(reg, *d);
            auto b = fxMake(reg, *d);
            if (!a || !b) continue;
            for (int i = 0; i < b->paramCount(); ++i) b->setParam(i, b->paramInfo(i).min);
            a->loadPreset(k);
            b->loadPreset(k);
            for (int i = 0; i < a->paramCount(); ++i) {
                const ParamInfo& pi = a->paramInfo(i);
                const f32 v = a->getParam(i);
                if (!(v >= pi.min && v <= pi.max)) inRange = false;
                if (a->getParam(i) != b->getParam(i)) stable = false;
            }
        }
        CHECK(inRange, "%s: every preset value is inside its parameter's declared range", uri);
        CHECK(stable, "%s: a preset lands on the same state from anywhere", uri);

        // ...and every preset actually runs.
        bool fine = true;
        for (int k = 0; k < p->presetCount(); ++k) {
            auto a = fxMake(reg, *d);
            if (!a) continue;
            a->loadPreset(k);
            Buf in, out;
            Noise ns;
            for (int b = 0; b < 24; ++b) {
                for (int i = 0; i < kBlock; ++i) {
                    in.l[(size_t)i] = 0.4f * ns.next();
                    in.r[(size_t)i] = 0.4f * ns.next();
                }
                out.clear();
                a->process(in.p, out.p, 2, kBlock);
                if (!out.finite() || out.peak() > 8.f) fine = false;
            }
        }
        CHECK(fine, "%s: every preset processes noise finitely and in range", uri);
    }
}


int main() {
    std::printf("internal device tests\n");

    PluginRegistry reg;
    reg.scan();

    banner("registry");
    int internals = 0, firstNonInternal = -1;
    for (size_t i = 0; i < reg.plugins().size(); ++i) {
        if (reg.plugins()[i].format == PluginFormat::Internal) ++internals;
        else if (firstNonInternal < 0) firstNonInternal = (int)i;
    }
    CHECK(internals == 16, "scan lists every internal device (%d)", internals);
    CHECK(firstNonInternal < 0 || firstNonInternal == internals,
          "internal devices sort to the front of the list");

    testLegacyUris(reg);
    testSaturator(reg);
    testPulse(reg);
    testSpectraTables(reg);      // must precede the first Spectra instantiation
    testSpectraContract(reg);
    testSpectraVoices(reg);
    testSpectraQuality(reg);
    testSpectraDeterminism(reg);
    testSpectraModulation(reg);
    testSpectraSweeps(reg);
    testSpectraState(reg);
    testSpectraPresets(reg);
    testSpectraV2SubNoise(reg);
    testSpectraV2Warp(reg);
    testSpectraV2Matrix(reg);
    testSpectraV2Voice(reg);
    testSpectraV2Determinism(reg);
    testSpectraV3Contract(reg);
    testSpectraV3State(reg);
    testSpectraV3CustomLfo(reg);
    testSpectraV3OneShot(reg);
    testSpectraV3Midi(reg);
    testSpectraV3Curves(reg);
    testSpectraV3Refusal(reg);
    testSpectraV3BeatLock(reg);
    testSpectraV3Determinism(reg);
    testSpectraV4Contract(reg);
    testSpectraV4State(reg);
    testSpectraV4Modes(reg);
    testSpectraV4Octaves(reg);
    testSpectraV4Clock(reg);
    testSpectraV4Velocity(reg);
    testSpectraV4HoldRetrig(reg);
    testSpectraV4Transitions(reg);
    testSpectraV4Chance(reg);
    testSpectraV4Source17(reg);
    testSpectraV4BeatLock(reg);
    testSpectraV4Determinism(reg);
    testSamplerContract(reg);
    testSamplerEmpty(reg);
    testSamplerPlayback(reg);
    testSamplerQuality(reg);
    testSamplerDeterminism(reg);
    testSamplerState(reg);
    testSamplerPresets(reg);
    testMidiFlood(reg, "nxtakt:spectra", "Spectra: a MIDI flood may not strand a voice "
                                         "(AUDIT-3 F3)", spFloodPatch);
    testMidiFlood(reg, "nxtakt:sampler", "Sampler: a MIDI flood may not strand a voice",
                  smFloodPatch);
    testEffectContract(reg);
    testEq3(reg);
    testCompressor(reg);
    testDelay(reg);
    testReverb(reg);
    testAutoFilter(reg);
    testChorus(reg);
    testLimiter(reg);
    testUtility(reg);
    testBlockInvariance(reg);
    testRack(reg);
    testInternalLatency(reg);
    testLv2Latency(reg);
    testRackLayoutUnderRender(reg);
    testRackLatency(reg);
    testFxContract(reg);
    testShimmerClimb(reg);
    testShimmerStability(reg);
    testBloomCrossover(reg);
    testBloomDynamics(reg);
    testTape(reg);
    testFxBlockInvariance(reg);
    testFxPresets(reg);
    testHostedInstrument(reg, PluginFormat::LV2, "LV2 instrument (real plugin, atom MIDI path)");
    testHostedInstrument(reg, PluginFormat::CLAP, "CLAP instrument (real plugin, note events)");

    std::printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
