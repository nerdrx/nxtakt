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
    // Enough blocks that an edit and a render provably overlapped.
    while (blocks.load(std::memory_order_relaxed) < 40) {
        rc->setState(twoDev);
        rc->setState(oneDev);
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
    CHECK(internals == 12, "scan lists every internal device (%d)", internals);
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
    testHostedInstrument(reg, PluginFormat::LV2, "LV2 instrument (real plugin, atom MIDI path)");
    testHostedInstrument(reg, PluginFormat::CLAP, "CLAP instrument (real plugin, note events)");

    std::printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail == 0 ? 0 : 1;
}
