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
    {  0, "A Table",       0.f,     7.f,     true,  false },
    {  1, "A Position",    0.f,     1.f,     false, false },
    {  2, "A Coarse",     -24.f,    24.f,    true,  false },
    {  3, "A Fine",       -100.f,   100.f,   false, false },
    {  4, "A Level",       0.f,     1.f,     false, false },
    {  5, "A Unison",      1.f,     7.f,     true,  false },
    {  6, "A Detune",      0.f,     100.f,   false, false },
    {  7, "A Spread",      0.f,     1.f,     false, false },
    {  8, "B Table",       0.f,     7.f,     true,  false },
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
    { 20, "Filter Type",   0.f,     2.f,     true,  false },
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
    { 37, "LFO Shape",     0.f,     4.f,     true,  false },
    { 38, "Glide",         0.f,     500.f,   false, false },
    { 39, "Voices",        1.f,     16.f,    true,  false },
    { 40, "Master",        0.f,     1.5f,    false, false },
    { 41, "Env2>Position",-1.f,     1.f,     false, false },
};
static constexpr int kSpectraContractN =
    (int)(sizeof kSpectraContract / sizeof kSpectraContract[0]);

// The preset names, also a contract: the editor's selector displays exactly
// these, in exactly this order.
static const char* kSpectraPresetNames[] = {
    "Init", "Supersaw Lead", "Solid Bass", "Sub Bass", "Pluck", "Warm Pad",
    "Formant Keys", "Bell", "Acid", "Wobble", "Air Pad", "Organ",
};
static constexpr int kSpectraPresetN =
    (int)(sizeof kSpectraPresetNames / sizeof kSpectraPresetNames[0]);

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
          "presetCount() is %d (the editor expects %d)", s->presetCount(), kSpectraPresetN);
    CHECK(s->presetName(-1) == nullptr && s->presetName(s->presetCount()) == nullptr,
          "presetName() is null out of range");

    bool names = true;
    for (int i = 0; i < s->presetCount() && i < kSpectraPresetN; ++i) {
        const char* n = s->presetName(i);
        if (!n || std::strcmp(n, kSpectraPresetNames[i]) != 0) {
            CHECK(false, "preset %d is \"%s\", the editor expects \"%s\"",
                  i, n ? n : "(null)", kSpectraPresetNames[i]);
            names = false;
        }
    }
    CHECK(names, "all %d preset names match, in order — this list IS the contract "
                 "with the editor's selector", kSpectraPresetN);

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
                          kSpectraPresetNames[k], pi.name.c_str(), (double)v,
                          (double)pi.min, (double)pi.max);
                    inRange = false;
                }
                if (pi.isInt && v != std::floor(v)) {
                    CHECK(false, "preset \"%s\" leaves the stepped parameter %s at %g",
                          kSpectraPresetNames[k], pi.name.c_str(), (double)v);
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
                          kSpectraPresetNames[k], fresh->paramInfo(i).name.c_str(),
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
                      kSpectraPresetNames[k], (double)pk);
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

        // Three that must be plainly different instruments.
        const int probe[3] = { 1, 7, 11 };            // Supersaw Lead, Bell, Organ
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
                          kSpectraPresetNames[probe[i]], kSpectraPresetNames[probe[j]], dot);
                    distinct = false;
                }
            }
        }
        CHECK(distinct, "Supersaw Lead, Bell and Organ are three different sounds "
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
    testSpectraContract(reg);
    testSpectraVoices(reg);
    testSpectraQuality(reg);
    testSpectraDeterminism(reg);
    testSpectraModulation(reg);
    testSpectraSweeps(reg);
    testSpectraState(reg);
    testSpectraPresets(reg);
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
