// EngineHandle in daemon mode, with no window and no GUI.
//
// The other suites test the two ends of the boundary: ipc_test the transport,
// daemon_test the far side against a real spawned nxtaktd. This tests the NEAR
// side — the object src/ui actually holds — doing exactly what App does to it
// and nothing else: open, poll, send, publish a clip, drain events, close.
//
// Three things are only reachable from here:
//
//   * `local()` answering null, which is what every caller that used to assume
//     an in-process Engine has to cope with;
//   * the retirement stand-in. A GUI-heap RtNote[] is COPIED into the pool, so
//     the engine never holds it and can never send Ev::NotesRetired for it. The
//     handle has to. Without that, App::retiringNotes_ grows for the life of the
//     session and nothing ever comes home;
//   * close(). A headless gamescope run cannot reach it — the compositor kills
//     the GUI outright — so "the daemon we spawned is stopped and both regions
//     are unlinked" has no other test.
//
// Built by `make build/handle_test` and run by `make test`. It spawns its own
// daemon and cleans up after itself. The recipe is in the Makefile; it grew
// src/plugin when rack contents landed, because a rack is the one device whose
// state cannot be faked — see the note above step 4b.
#include "src/ui/engine_handle.h"
// For the protocol's own bounds, and for those only. The handle deliberately
// keeps src/ipc out of its header so the view translation units never see it;
// a TEST asserting what happens at kMaxArrItems has to name the real number
// rather than a copy of it that can drift.
#include "src/ipc/control.h"
// For reapStale(): a SIGKILLed daemon leaves its region behind, and picking it
// up is the client's job (§4.1), not a tidy-up this test invented.
#include "src/ipc/client.h"
// SampleBuffer, built by hand. No decoder is linked here and none is wanted:
// what this suite drives is the SEAM, and a sampler is handed a decoded buffer
// through SamplerControl::adopt() whether a file or a test made it.
#include "src/audio/sample.h"
// For the hardware-MIDI check below, which sends a real sequencer event into
// the GUI's own ALSA client. There is no other way to prove that path: the
// reader thread is the producer and a fake would test the fake.
#include <alsa/asoundlib.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>
#include <dirent.h>
#include <unistd.h>
#include <cstdlib>
#include <csignal>
#include <atomic>
#include <thread>

using namespace lat;

static int gPass = 0, gFail = 0;
#define CHECK(c, ...) do { if (c) { ++gPass; std::printf("  PASS  "); } \
    else { ++gFail; std::printf("  FAIL  "); } std::printf(__VA_ARGS__); std::printf("\n"); \
    std::fflush(stdout); } while (0)

static void sleepMs(int ms) { timespec t{ms/1000,(long)(ms%1000)*1000000L}; nanosleep(&t,nullptr); }

static void banner(const char* s) { std::printf("\n== %s\n", s); }

static int  countShm(const char* needle);
static void testRecording(const char* baseSession);

// The master peak over `frames` polls, with the event pump running — i.e. the
// frame loop App runs, with the meter read off the snapshot like every other
// indicator. poll() is also where the handle reconciles chains and mirrors
// params, so calling it is not incidental to what is being measured.
static f32 peakOver(lat::EngineHandle& eng, lat::EngineState& es, int frames) {
    lat::Event e;
    f32 peak = 0.f;
    // SETTLE FIRST, and the reason is not tidiness. This is a PEAK over a
    // window, and a change made just before the call — a param write that
    // poll() has not pushed yet, a chain edit the daemon has not applied — is
    // still audible during the first frames of it. A window that spans the
    // transition reports the level BEFORE the change as confidently as the one
    // after, which is exactly the class of check that passes against a dropped
    // write. A third of the window is thrown away, then the peak is measured.
    const int settle = frames / 3 + 10;
    for (int i = 0; i < settle; ++i) {
        eng.poll(es);
        while (eng.popEvent(e)) {}
        sleepMs(10);
    }
    for (int i = 0; i < frames; ++i) {
        eng.poll(es);
        while (eng.popEvent(e)) {}
        peak = std::fmax(peak, es.masterMeterL);
        sleepMs(10);
    }
    return peak;
}

// ---------------------------------------------------------------------------
// A PluginInstance that describes a plugin and renders nothing
// ---------------------------------------------------------------------------
//
// In daemon mode the GUI's own instance renders nothing either — there is no
// in-process engine to call process() — so this is not a stand-in for the real
// thing, it is the real thing's job. What App's device code puts into an
// RtChain is an object that answers desc(), paramInfo(i).id, getParam(i) and
// bypassed(), and those four are the entire input the handle takes off a chain.
//
// Every method used here is virtual, which is what lets this suite go on
// linking no plugin backend: host.h is a header, and the vtable is ours.
static constexpr int kDrive = 0, kOutput = 1, kMix = 2;

struct FakeDevice : lat::PluginInstance {
    lat::PluginDesc d;
    lat::ParamInfo  pi[3];
    f32  v[3] = { 0.f, 0.f, 1.f };
    bool byp = false;

    explicit FakeDevice(const char* uri = "nxtakt:saturator", const char* name = "Saturator") {
        d.uri = uri; d.name = name; d.vendor = "NxTakt";
        d.format = lat::PluginFormat::Internal;
        d.kind   = lat::PluginKind::Effect;
        d.audioIn = 2; d.audioOut = 2; d.paramCount = 3;
        // The ids are what matter and they are the internal devices' own:
        // addParam() numbers them by ordinal (internal_devices.cpp). The NAMES
        // here are cosmetic — the handle matches on id, per PARAM-ADDRESS.md,
        // and never on a name or a position.
        pi[kDrive]  = { "Drive",  "dB", 0.f,  36.f, 0.f, false, false, true,  0u };
        pi[kOutput] = { "Output", "dB", -24.f, 24.f, 0.f, false, false, false, 1u };
        pi[kMix]    = { "Mix",    "",   0.f,   1.f, 1.f, false, false, false, 2u };
    }

    bool prepare(f64, int) override { return true; }
    void process(const f32* const*, f32* const*, int, int) override {}
    int  paramCount() const override { return 3; }
    const lat::ParamInfo& paramInfo(int i) const override { return pi[i < 0 || i > 2 ? 0 : i]; }
    f32  getParam(int i) const override { return (i < 0 || i > 2) ? 0.f : v[i]; }
    void setParam(int i, f32 x) override { if (i >= 0 && i <= 2) v[i] = x; }
    const lat::PluginDesc& desc() const override { return d; }
    void setBypassed(bool b) override { byp = b; }
    bool bypassed() const override { return byp; }
};

// ---------------------------------------------------------------------------
// Recording through the seam  (docs/GUI-ON-DAEMON.md §7)
// ---------------------------------------------------------------------------
//
// This is the near side of the last feature, and the ONE thing only this suite
// can show: that App's recording code does not have to change. So everything
// below goes through pushCommand()/popEvent() with a GUI-heap buffer, in exactly
// the sequence app_engine.cpp uses — allocate, push Cmd::RecordSlot with `p` and
// a capacity, push the same command again to stop, wait for Ev::RecordFinished
// carrying that same pointer back — and asserts on what comes home.
//
// It also carries the quantization proof, and that needs a reference. The same
// script is run against an IN-PROCESS engine driven by the block loop below, so
// the two takes can be compared frame for frame rather than against a number
// somebody worked out by hand. If the daemon's boundaries ever stopped being the
// engine's own, this is what would say so.

// A block clock for an in-process Engine, feeding the same synthetic ramp the
// daemon's null driver feeds behind NXTAKT_DEBUG_INPUT. It is a copy of that
// loop on purpose: the point of the reference is that the ENGINE is the same
// code, not that the harness is.
class LocalDriver {
public:
    bool start(lat::Engine& e, f64 rate, int block) {
        eng_ = &e; sr_ = rate; block_ = block;
        outL_.assign((size_t)block, 0.f); outR_.assign((size_t)block, 0.f);
        inL_.assign((size_t)block, 0.f);  inR_.assign((size_t)block, 0.f);
        run_.store(true);
        th_ = std::thread([this] { loop(); });
        return true;
    }
    void stop() { if (!run_.exchange(false)) return; if (th_.joinable()) th_.join(); }
    ~LocalDriver() { stop(); }

private:
    void loop() {
        const u64 blockNs = (u64)((f64)block_ / sr_ * 1e9);
        const u64 origin = nowNs();
        u64 rendered = 0;
        while (run_.load()) {
            const u64 now = nowNs();
            const u64 due = now >= origin ? (now - origin) / blockNs + 1 : 1;
            u64 want = due > rendered ? due - rendered : 0;
            if (want > 32) { rendered = due - 1; want = 1; }
            for (u64 i = 0; i < want; ++i) {
                for (int k = 0; k < block_; ++k) {
                    const f32 v = (f32)(ramp_ % 65536) / 65536.f;
                    inL_[(size_t)k] = v; inR_[(size_t)k] = -v;
                    ++ramp_;
                }
                eng_->process(inL_.data(), inR_.data(), outL_.data(), outR_.data(), block_);
                ++rendered;
            }
            const u64 next = origin + rendered * blockNs;
            timespec ts{(time_t)(next / 1000000000ull), (long)(next % 1000000000ull)};
            ::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
        }
    }
    static u64 nowNs() {
        timespec t{};
        ::clock_gettime(CLOCK_MONOTONIC, &t);
        return (u64)t.tv_sec * 1000000000ull + (u64)t.tv_nsec;
    }
    lat::Engine* eng_ = nullptr;
    f64 sr_ = 48000.0;
    int block_ = 256;
    std::vector<f32> outL_, outR_, inL_, inR_;
    u64 ramp_ = 0;
    std::thread th_;
    std::atomic<bool> run_{false};
};

// App::startRecording / stopRecording / finishRecording, with App taken out.
// Returns the frames (or notes) the finish event reported, -1 if nothing came
// home inside the timeout, and -2 if the take was never armed.
//
// The buffer is the CALLER's, exactly as App's is, and the assertion that
// matters most is inside: the event that comes home carries the caller's own
// pointer. Anything else would be a take handed back for somebody else's buffer.
static i64 runTake(lat::EngineHandle& eng, lat::EngineState& es, int track, int slot,
                   bool midi, void* buf, i64 cap, int holdMs, f64* startBeatOut = nullptr) {
    lat::Command c;
    c.type = midi ? lat::Cmd::RecordMidiSlot : lat::Cmd::RecordSlot;
    c.a = track; c.b = slot; c.p = buf; c.x = (f64)cap;
    if (!eng.pushCommand(c)) return -2;

    lat::Event e;
    bool armed = false;
    f64  startBeat = -1.0;
    for (int i = 0; i < 600 && !armed; ++i) {
        eng.poll(es);
        while (eng.popEvent(e))
            if (e.type == lat::Ev::RecordStarted && e.a == track) {
                armed = true; startBeat = e.x;
            }
        if (!armed) sleepMs(10);
    }
    if (startBeatOut) *startBeatOut = startBeat;
    if (!armed) return -2;

    for (int i = 0; i < holdMs / 10; ++i) { eng.poll(es); while (eng.popEvent(e)) {} sleepMs(10); }

    if (!eng.pushCommand(c)) return -2;          // the same command stops it
    for (int i = 0; i < 900; ++i) {
        eng.poll(es);
        while (eng.popEvent(e)) {
            const bool fin = midi ? e.type == lat::Ev::MidiRecordFinished
                                  : e.type == lat::Ev::RecordFinished;
            if (fin && e.p == buf) return (i64)e.x;
            if (fin) { std::printf("  FAIL  a take came home for the WRONG buffer\n"); ++gFail; }
        }
        sleepMs(10);
    }
    return -1;
}

// How many frames a stereo ramp take is contiguous for, and whether the right
// channel is the left one's negation throughout. Zero breaks is the property:
// a gap is a block the audio thread dropped, which no length check can see.
static void rampStats(const f32* buf, i64 frames, int& breaks, int& swapped, int& silent) {
    breaks = swapped = silent = 0;
    for (i64 i = 0; i < frames; ++i) {
        const f32 l = buf[i * 2], r = buf[i * 2 + 1];
        if (r != -l) ++swapped;
        if (l == 0.f && r == 0.f) ++silent;
        if (i == 0) continue;
        const f32 prev = buf[(i - 1) * 2];
        if (!(prev > 0.9f && l < 0.1f) && l - prev != 1.f / 65536.f) ++breaks;
    }
}

static void testRecording(const char* baseSession) {
    banner("recording through the seam: App's own sequence, unedited");

    // --- the in-process reference ------------------------------------------
    //
    // Run FIRST, so the number the daemon has to match is a measurement and not
    // a constant somebody typed. Same tempo, same quantum, same actions.
    const i64 cap = 48000 * 8;
    std::vector<f32> refBuf((size_t)cap * 2, 0.f);
    i64 refFrames = -1;
    f64 refStart  = -1.0;
    {
        lat::EngineHandle loc;
        lat::EngineState les;
        CHECK(loc.openLocalEngine("null"), "an in-process engine, for the reference take");
        LocalDriver drv;
        if (loc.local()) drv.start(*loc.local(), 48000.0, 256);
        loc.send(lat::Cmd::SetTempo, 0, 0, 120.0);
        loc.send(lat::Cmd::SetQuantum, 4);              // 1 Bar
        loc.send(lat::Cmd::SetPlaying, 1);
        sleepMs(120);
        refFrames = runTake(loc, les, 0, 0, false, refBuf.data(), cap, 300, &refStart);
        CHECK(refFrames > 0, "the in-process take came home (%lld frames)",
              (long long)refFrames);
        int b = 0, s = 0, z = 0;
        if (refFrames > 0) rampStats(refBuf.data(), refFrames, b, s, z);
        CHECK(refFrames > 0 && b == 0 && s == 0,
              "contiguous, channels the right way round (%d breaks, %d swapped)", b, s);
        drv.stop();
        loc.close();
        CHECK(loc.takesReturned() == 0 && loc.takesLost() == 0,
              "and the take counters read 0 locally, because none of those states "
              "exists in-process");
    }

    // --- the same take, through the daemon ----------------------------------
    char rsession[80];
    std::snprintf(rsession, sizeof rsession, "%s-rec", baseSession);
    ::setenv("NXTAKT_SESSION", rsession, 1);
    ::setenv("NXTAKT_DEBUG_INPUT", "ramp", 1);      // the daemon's capture signal
    lat::EngineHandle eng;
    lat::EngineState es;
    const bool up = eng.open("null");
    ::unsetenv("NXTAKT_DEBUG_INPUT");
    CHECK(up && eng.remoteOpen(), "a daemon-mode handle on session '%s'", rsession);
    if (!up || !eng.remoteOpen()) { ::setenv("NXTAKT_SESSION", baseSession, 1); return; }

    eng.send(lat::Cmd::SetTempo, 0, 0, 120.0);
    eng.send(lat::Cmd::SetQuantum, 4);
    eng.send(lat::Cmd::SetPlaying, 1);
    sleepMs(150);

    std::vector<f32> buf((size_t)cap * 2, 0.f);
    f64 remStart = -1.0;
    const i64 frames = runTake(eng, es, 0, 0, false, buf.data(), cap, 300, &remStart);
    CHECK(frames > 0,
          "Ev::RecordFinished came back carrying the GUI's OWN buffer (%lld frames) "
          "— App's finishRecording() matches on that pointer and would leak the "
          "take if anything else arrived", (long long)frames);

    // THE QUANTIZATION PROOF. The launch-quantum machinery is engine-side and
    // identical on both paths, but "identical code" is an argument and this is a
    // measurement: same set, same actions, same boundaries, same frame count.
    CHECK(refFrames > 0 && frames > 0 && std::llabs((long long)(frames - refFrames)) <= 1,
          "and it is the SAME LENGTH as the in-process take: %lld vs %lld frames "
          "(one bar at 120 BPM). Quantized start and stop land on the same frames "
          "on both paths.", (long long)frames, (long long)refFrames);
    CHECK(remStart >= 0.0 && std::fabs(std::fmod(remStart, 4.0)) < 1e-6,
          "both began on a bar line (%.4f remote, %.4f local)", remStart, refStart);

    int breaks = 0, swapped = 0, silent = 0;
    if (frames > 0) rampStats(buf.data(), frames, breaks, swapped, silent);
    CHECK(frames > 0 && silent < frames / 2,
          "the buffer really holds the daemon's captured input, not the zeros the "
          "GUI allocated (%d silent of %lld)", silent, (long long)frames);
    CHECK(breaks == 0 && swapped == 0,
          "contiguous and correctly interleaved after crossing a file and a process "
          "boundary (%d breaks, %d swapped)", breaks, swapped);
    CHECK(eng.takesReturned() == 1 && eng.takesFailed() == 0 && eng.takesLost() == 0,
          "the handle counts it returned (%llu returned, %llu empty, %llu failed, "
          "%llu lost)", (unsigned long long)eng.takesReturned(),
          (unsigned long long)eng.takesEmpty(), (unsigned long long)eng.takesFailed(),
          (unsigned long long)eng.takesLost());

    // --- a take that captured nothing ---------------------------------------
    //
    // Stopped inside the same bar it was queued in. The engine cancels it in
    // SILENCE (engine.cpp: "there is no buffer to hand back, so no event goes
    // out either") — which is fine in-process, where App's buffer was never
    // lent, and would strand a daemon-mode take forever. App still has to get
    // its buffer back, and it gets it as the zero-frame finish it already knows
    // how to read: "Recording cancelled".
    {
        const u64 empty0 = eng.takesEmpty();
        std::vector<f32> b2((size_t)48000 * 2, 0.f);
        lat::Command c;
        c.type = lat::Cmd::RecordSlot; c.a = 1; c.b = 0; c.p = b2.data(); c.x = 48000.0;
        CHECK(eng.pushCommand(c), "queue a take on track 1");
        sleepMs(30);
        CHECK(eng.pushCommand(c), "and stop it before its bar line");
        lat::Event e;
        i64 got = -1;
        for (int i = 0; i < 900 && got < 0; ++i) {
            eng.poll(es);
            while (eng.popEvent(e))
                if (e.type == lat::Ev::RecordFinished && e.p == b2.data()) got = (i64)e.x;
            sleepMs(10);
        }
        CHECK(got == 0,
              "the buffer comes home with zero frames (%lld) rather than never "
              "coming home at all", (long long)got);
        CHECK(eng.takesEmpty() == empty0 + 1, "counted as empty, not as failed (%llu)",
              (unsigned long long)eng.takesEmpty());
    }

    // --- a MIDI take --------------------------------------------------------
    {
        std::vector<lat::RtNote> notes(4096);
        eng.send(lat::Cmd::TrackArm, 2, 1);
        lat::Command c;
        c.type = lat::Cmd::RecordMidiSlot;
        c.a = 2; c.b = 0; c.p = notes.data(); c.x = (f64)notes.size();
        CHECK(eng.pushCommand(c), "queue a MIDI take on track 2");
        lat::Event e;
        bool armed = false;
        for (int i = 0; i < 600 && !armed; ++i) {
            eng.poll(es);
            while (eng.popEvent(e)) if (e.type == lat::Ev::RecordStarted && e.a == 2) armed = true;
            if (!armed) sleepMs(10);
        }
        CHECK(armed, "it arms on a bar line");
        for (int n = 0; n < 3; ++n) {
            eng.pushMidi(lat::MidiMsg{0x90, (u8)(48 + n), 100, 0});
            for (int i = 0; i < 6; ++i) { eng.poll(es); while (eng.popEvent(e)) {} sleepMs(10); }
            eng.pushMidi(lat::MidiMsg{0x80, (u8)(48 + n), 0, 0});
            for (int i = 0; i < 2; ++i) { eng.poll(es); while (eng.popEvent(e)) {} sleepMs(10); }
        }
        CHECK(eng.pushCommand(c), "and stop it");
        i64 count = -1;
        for (int i = 0; i < 900 && count < 0; ++i) {
            eng.poll(es);
            while (eng.popEvent(e))
                if (e.type == lat::Ev::MidiRecordFinished && e.p == notes.data())
                    count = (i64)e.x;
            sleepMs(10);
        }
        CHECK(count >= 3, "the notes come home in the GUI's own RtNote[] (%lld)",
              (long long)count);
        bool pitched = count >= 3;
        for (i64 i = 0; i < count && i < 3; ++i)
            if (notes[(size_t)i].pitch < 48 || notes[(size_t)i].pitch > 50) pitched = false;
        CHECK(pitched, "with the pitches that were played, in the GUI's own array — "
                       "which is the whole of what App::finishMidiRecording reads");
        eng.send(lat::Cmd::TrackArm, 2, 0);
    }

    // --- a second take on a busy track --------------------------------------
    //
    // App cannot ask for this (startRecording refuses a track whose recState is
    // not idle), but recState is a MIRRORED atomic and a fast enough second
    // click can read it stale. What must not happen then is a consumed command:
    // App frees its capture buffer on the finish event and on nothing else, so
    // "accepted" for a take that will never start is a buffer pinned for the
    // rest of the session.
    //
    // `false` is the honest answer as well as the safe one — it is not a
    // permanent refusal, it is "not while that take is running", and App reads
    // it as "engine busy" and frees the buffer it just allocated.
    {
        std::vector<f32> b4((size_t)48000 * 2, 0.f), b5((size_t)48000 * 2, 0.f);
        lat::Command c;
        c.type = lat::Cmd::RecordSlot; c.a = 4; c.b = 0; c.p = b4.data(); c.x = 48000.0;
        CHECK(eng.pushCommand(c), "start a take on track 4 slot 0");
        lat::Event e;
        bool armed = false;
        for (int i = 0; i < 600 && !armed; ++i) {
            eng.poll(es);
            while (eng.popEvent(e)) if (e.type == lat::Ev::RecordStarted && e.a == 4) armed = true;
            if (!armed) sleepMs(10);
        }
        CHECK(armed, "it arms");

        lat::Command c2 = c;
        c2.b = 1; c2.p = b5.data();
        CHECK(!eng.pushCommand(c2),
              "a second take on the SAME track, a different slot, answers false — "
              "not consumed, so App frees the buffer instead of pinning it forever");

        // And the live take is untouched by having been refused at.
        CHECK(eng.pushCommand(c), "the original take still stops");
        i64 got = -1;
        for (int i = 0; i < 900 && got < 0; ++i) {
            eng.poll(es);
            while (eng.popEvent(e))
                if (e.type == lat::Ev::RecordFinished && e.p == b4.data()) got = (i64)e.x;
            sleepMs(10);
        }
        CHECK(got > 0, "and comes home with its material (%lld frames)", (long long)got);
    }

    // --- THE DAEMON DIES MID-TAKE -------------------------------------------
    //
    // The second arm of the crash matrix. App frees its capture buffer on the
    // finish event and on nothing else, so a GUI that merely reported the lost
    // engine would sit with the buffer pinned and the slot stuck in "recording"
    // for the rest of the session. The handle answers for the engine that
    // cannot: every take in flight is handed back empty, once, the moment the
    // link is provably Lost.
    {
        std::vector<f32> b3((size_t)cap * 2, 0.f);
        lat::Command c;
        c.type = lat::Cmd::RecordSlot; c.a = 3; c.b = 0; c.p = b3.data(); c.x = (f64)cap;
        CHECK(eng.pushCommand(c), "start a take on track 3");
        lat::Event e;
        bool armed = false;
        for (int i = 0; i < 600 && !armed; ++i) {
            eng.poll(es);
            while (eng.popEvent(e)) if (e.type == lat::Ev::RecordStarted && e.a == 3) armed = true;
            if (!armed) sleepMs(10);
        }
        CHECK(armed, "it is armed and the daemon is capturing into its own buffer");

        const i32 pid = eng.enginePid();
        CHECK(pid > 0 && ::kill(pid, SIGKILL) == 0, "SIGKILL the engine mid-take (pid %d)",
              (int)pid);

        i64 got = -1;
        bool lost = false;
        for (int i = 0; i < 900 && got < 0; ++i) {
            eng.poll(es);
            if (es.link == lat::EngineLink::Lost) lost = true;
            while (eng.popEvent(e))
                if (e.type == lat::Ev::RecordFinished && e.p == b3.data()) got = (i64)e.x;
            sleepMs(10);
        }
        CHECK(lost, "the handle reports the engine Lost");
        CHECK(got == 0,
              "and hands the capture buffer back empty (%lld frames) — the GUI is "
              "free, the slot is idle, and nothing is waiting on a process that no "
              "longer exists", (long long)got);
        CHECK(eng.takesLost() >= 1, "counted as lost with the engine (%llu), which is "
              "the number that distinguishes a crash from an empty take",
              (unsigned long long)eng.takesLost());

        // Nothing wedged: the handle still polls, still answers, still restarts.
        eng.poll(es);
        CHECK(es.link == lat::EngineLink::Lost, "and it keeps polling without wedging");
    }

    eng.close();
    sleepMs(400);
    // The engine was SIGKILLed above, so its control region is an ORPHAN — it
    // never ran the shutdown that unlinks the name. That is §4.1's whole reason
    // for reapStale(), and calling it here is what the GUI does on its next
    // attach rather than a tidy-up this test invented.
    CHECK(countShm(rsession) == 1,
          "one region is left: a SIGKILLed daemon cannot unlink its own name (%d)",
          countShm(rsession));
    CHECK(ipc::EngineClient::reapStale(rsession),
          "and reapStale() takes it, because the creator is provably gone");
    CHECK(countShm(rsession) == 0, "the recording session's regions are gone (%d left)",
          countShm(rsession));
    ::setenv("NXTAKT_SESSION", baseSession, 1);
}

// ---------------------------------------------------------------------------
// One OFFLINE render of an in-process engine  (used by the v11 payload section)
// ---------------------------------------------------------------------------
//
// Deliberately not LocalDriver. That one renders against the wall clock, which
// is exactly what the daemon's null driver does and exactly what makes a
// frame-for-frame comparison across the boundary impossible. This renders block
// after block with nothing else in the process, so what comes back is a
// FUNCTION of the clip and of nothing else — which is what lets the same call,
// made twice, be a bit-identity claim.
//
// THE ENGINE IS THE CALLER'S, and reused for every render in the section. The
// four per-Engine side tables in engine.cpp (parked events, delay compensation,
// automation holds, the arrangement cursor) are keyed by the engine's ADDRESS
// and hold four slots between them, so a helper that built its own engine each
// call would start evicting on the fifth — and one of the things it would evict
// is the automation state leg (a) is about. prepare() resets tracks, clips,
// meters and the beat; the leading Cmd::SetPlaying(0) normalises the one thing
// it does not touch, the transport flag, so every call starts in the same place
// whatever the previous one left behind.
//
// `frames` is rendered block-aligned into `outL`; the right channel is
// scratch, because every clip here is mono and centred and the two are equal.
static void renderLocal(lat::Engine& e, const lat::RtClip& cl, f64 tempo,
                        i64 frames, std::vector<f32>& outL) {
    const int block = 256;
    e.prepare(48000.0, block);

    auto push = [&](lat::Cmd t, i32 a, i32 b, f64 x) {
        lat::Command c; c.type = t; c.a = a; c.b = b; c.x = x; e.pushCommand(c);
    };
    push(lat::Cmd::SetPlaying, 0, 0, 0.0);        // a known transport, whatever ran last
    push(lat::Cmd::SetTempo,   0, 0, tempo);
    push(lat::Cmd::SetQuantum, 0, 0, 0.0);        // 0 beats: a launch fires on the spot
    push(lat::Cmd::TrackVol,   0, 0, 1.0);
    push(lat::Cmd::MasterVol,  0, 0, 1.0);
    lat::Command sc;
    sc.type = lat::Cmd::SetClip; sc.a = 0; sc.b = 0; sc.clip = cl;
    e.pushCommand(sc);
    push(lat::Cmd::SetPlaying, 1, 0, 0.0);
    push(lat::Cmd::LaunchClip, 0, 0, 0.0);

    outL.assign((size_t)frames, 0.f);
    std::vector<f32> scratchR((size_t)block, 0.f);
    for (i64 d = 0; d + block <= frames; d += block)
        e.process(nullptr, nullptr, outL.data() + d, scratchR.data(), block);
}

// The peak of a rendered stretch, skipping the head of it. The skip is the
// offline twin of peakOver()'s settle: the declick ramp is inside the first
// milliseconds of every launch, and a peak taken across it would report the
// ramp on a quiet clip and nothing at all on a loud one.
static f32 peakFrom(const std::vector<f32>& v, size_t from) {
    f32 p = 0.f;
    for (size_t i = from; i < v.size(); ++i) p = std::fmax(p, std::fabs(v[i]));
    return p;
}

static int countShm(const char* needle) {
    DIR* d = ::opendir("/dev/shm");
    if (!d) return -1;
    int n = 0;
    while (dirent* e = ::readdir(d)) if (std::strstr(e->d_name, needle)) ++n;
    ::closedir(d);
    return n;
}

int main() {
    char session[64];
    std::snprintf(session, sizeof session, "htest-%d", (int)::getpid());
    ::setenv("NXTAKT_ENGINE", "daemon", 1);
    ::setenv("NXTAKT_SESSION", session, 1);
    ::setenv("NXTAKT_DAEMON", "build/nxtaktd", 0);   // an externally-set path wins, so the daemon can be sanitised too

    std::printf("== EngineHandle, daemon mode, session '%s'\n", session);

    EngineHandle eng;
    CHECK(eng.openLocal("null"), "openLocal() dispatched on NXTAKT_ENGINE and opened something");
    CHECK(!eng.localOpen(), "localOpen() is false: no in-process Engine was created");
    CHECK(eng.remoteOpen(), "remoteOpen() is true");
    CHECK(eng.local() == nullptr, "local() answers null, which is the load-bearing test");
    CHECK(std::fabs(eng.sampleRate() - 48000.0) < 1e-9,
          "sampleRate() is live off the wire before anything decodes (%.0f)", eng.sampleRate());
    CHECK(eng.driverName() && std::strstr(eng.driverName(), "null"),
          "driverName() comes off ControlHeader ('%s')", eng.driverName() ? eng.driverName() : "");
    // HARDWARE MIDI IS ON THIS PATH NOW (§1.3). It used to be asserted absent
    // here, which was honest while MidiInput took an Engine& and there was no
    // Engine to give it. What can be asserted on every machine is that the three
    // accessors AGREE — a running reader has a client id, a stopped one does not
    // — because whether snd-seq exists at all is a property of the box.
    CHECK(eng.midiRunning() == (eng.midiClientId() >= 0),
          "the MIDI accessors agree (running %d, client %d)",
          (int)eng.midiRunning(), eng.midiClientId());

    EngineState es;
    eng.poll(es);
    CHECK(std::fabs(es.sampleRate - 48000.0) < 1e-9 && es.blockSize == 256,
          "the snapshot carries the engine format (%.0f Hz / %u frames)",
          es.sampleRate, es.blockSize);

    // --- scalars (step 2) --------------------------------------------------
    CHECK(eng.send(Cmd::SetTempo, 0, 0, 140.0), "SetTempo crosses");
    CHECK(eng.send(Cmd::SetQuantum, 0), "SetQuantum crosses");
    CHECK(eng.send(Cmd::SetPlaying, 1), "SetPlaying crosses");
    bool tempoSeen = false, beatMoved = false;
    const f64 b0 = es.beat;
    for (int i = 0; i < 100 && !(tempoSeen && beatMoved); ++i) {
        sleepMs(20);
        eng.poll(es);
        if (std::fabs(es.tempo - 140.0) < 1e-6) tempoSeen = true;
        if (es.beat > b0 + 0.5) beatMoved = true;
    }
    CHECK(tempoSeen, "the snapshot reports the tempo the GUI set (%.1f)", es.tempo);
    CHECK(beatMoved && es.playing, "the transport runs in the daemon (beat %.2f, playing %d)",
          es.beat, (int)es.playing);

    // --- MIDI (step 2) -----------------------------------------------------
    MidiMsg m{}; m.status = 0x90; m.d1 = 60; m.d2 = 100;
    CHECK(eng.pushMidi(m), "pushMidi crosses on the MIDI ring");

    // --- HARDWARE MIDI, end to end (§1.3) ----------------------------------
    //
    // Not a stand-in: a real ALSA sequencer client, connected to the GUI's own
    // input port, sending a real note-on. What it proves is the whole of the new
    // path — snd_seq_event_input on the reader thread, toWire, the SINK the
    // handle installed, and cli.pushMidi accepting it onto the shared-memory
    // ring. `received()` counts only messages a sink ACCEPTED, so it cannot move
    // for a reader that was started with nowhere to put them.
    //
    // THE SKIP IS GATED ON THE TEST'S OWN SEQUENCER, NOT ON THE HANDLE'S.
    // Skipping when `midiRunning()` is false would have been the obvious
    // shape and is exactly wrong: it cannot tell "this box has no snd-seq" from
    // "the handle never installed a sink", so removing the sink would have made
    // this section quietly stop testing anything instead of going red. So the
    // test opens a sequencer FIRST — if IT can, so could the handle, and
    // midiRunning() being false is then a failure and not an environment.
    {
        snd_seq_t* seq = nullptr;
        const bool opened = snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, 0) >= 0 && seq;
        if (!opened) {
            std::printf("  SKIP  no ALSA sequencer on this machine; "
                        "hardware MIDI is untested here\n");
        } else {
            CHECK(eng.midiRunning(),
                  "the handle opened an ALSA client too (%d): this machine has a "
                  "sequencer, so a reader that is not running is a missing sink and "
                  "not a missing kernel module", eng.midiClientId());
            snd_seq_set_client_name(seq, "handle_test");
            const int outPort = snd_seq_create_simple_port(
                seq, "out",
                SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
            CHECK(outPort >= 0, "with an output port (%d)", outPort);
            const int conn = snd_seq_connect_to(seq, outPort, eng.midiClientId(), 0);
            CHECK(conn >= 0, "connected to NxTakt's input at %d:0", eng.midiClientId());

            const u64 got0 = eng.midiReceived();
            snd_seq_event_t ev;
            snd_seq_ev_clear(&ev);
            snd_seq_ev_set_source(&ev, outPort);
            snd_seq_ev_set_subs(&ev);
            snd_seq_ev_set_direct(&ev);
            snd_seq_ev_set_noteon(&ev, 0, 60, 100);
            snd_seq_event_output_direct(seq, &ev);
            snd_seq_drain_output(seq);

            bool arrived = false;
            for (int i = 0; i < 200 && !arrived; ++i) {
                sleepMs(10);
                arrived = eng.midiReceived() > got0;
            }
            CHECK(arrived,
                  "A NOTE PLAYED ON A HARDWARE PORT REACHED THE DAEMON: "
                  "received() %llu -> %llu. The reader thread has no Engine to "
                  "push into on this path; it pushes over the wire",
                  (unsigned long long)got0, (unsigned long long)eng.midiReceived());

            snd_seq_ev_clear(&ev);
            snd_seq_ev_set_source(&ev, outPort);
            snd_seq_ev_set_subs(&ev);
            snd_seq_ev_set_direct(&ev);
            snd_seq_ev_set_noteoff(&ev, 0, 60, 0);
            snd_seq_event_output_direct(seq, &ev);
            snd_seq_drain_output(seq);
            snd_seq_close(seq);
        }
    }

    // --- a clip through the pool (step 3) ----------------------------------
    // A DC buffer on this process's heap, exactly as a decoded SampleBuffer is:
    // the handle is the thing that has to notice it cannot travel as a pointer.
    const i64 frames = 48000;
    std::vector<f32> dc((size_t)frames, 0.5f);

    Command c;
    c.type = Cmd::SetClip;
    c.a = 0; c.b = 0;
    c.clip.data        = dc.data();
    c.clip.frames      = frames;
    c.clip.channels    = 1;
    c.clip.loopStart   = 0;
    c.clip.loopEnd     = frames;
    c.clip.warp        = (int)Warp::Off;
    c.clip.loop        = true;
    c.clip.quantumIdx  = 0;
    c.clip.lengthBeats = 4.0;
    c.clip.gain        = 1.0f;
    c.clip.valid       = true;
    CHECK(eng.pushCommand(c), "SetClip with a GUI-heap f32* is accepted by the handle");

    // Drain a few frames so the ack comes home, as App::frame() would.
    Event e;
    for (int i = 0; i < 40; ++i) { eng.poll(es); while (eng.popEvent(e)) {} sleepMs(20); }

    CHECK(eng.send(Cmd::TrackVol, 0, 0, 1.0), "TrackVol crosses");
    CHECK(eng.send(Cmd::MasterVol, 0, 0, 1.0), "MasterVol crosses");
    CHECK(eng.send(Cmd::LaunchClip, 0, 0), "LaunchClip crosses");

    f32 peak = 0.f;
    for (int i = 0; i < 150; ++i) {
        eng.poll(es);
        peak = std::fmax(peak, es.masterMeterL);
        while (eng.popEvent(e)) {}
        sleepMs(10);
    }
    CHECK(peak > 0.3f && peak < 0.7f,
          "the daemon renders the clip the handle copied into the pool "
          "(master peak %.4f, expected ~0.5)", (double)peak);
    CHECK(es.activeSlot[0] == 0, "track 0 reports slot 0 active (%d)", es.activeSlot[0]);

    // --- a clip replaced: the retirement stand-in ---------------------------
    std::vector<RtNote> n1(4), n2(4);
    for (int i = 0; i < 4; ++i) { n1[i].beat = i; n1[i].len = 1; n1[i].pitch = (u8)(60+i); n1[i].vel = 100; }
    for (int i = 0; i < 4; ++i) { n2[i].beat = i; n2[i].len = 1; n2[i].pitch = (u8)(72+i); n2[i].vel = 100; }

    Command mc;
    mc.type = Cmd::SetClip; mc.a = 1; mc.b = 0;
    mc.clip.isMidi = true; mc.clip.notes = n1.data(); mc.clip.noteCount = 4;
    mc.clip.lengthBeats = 4.0; mc.clip.gain = 1.0f; mc.clip.valid = true;
    mc.clip.quantumIdx = 0;
    CHECK(eng.pushCommand(mc), "a MIDI clip's notes cross as a pool block");
    for (int i = 0; i < 20; ++i) { eng.poll(es); while (eng.popEvent(e)) {} sleepMs(10); }

    mc.clip.notes = n2.data();
    bool pushed = false;
    for (int i = 0; i < 40 && !pushed; ++i) {
        pushed = eng.pushCommand(mc);
        if (!pushed) { eng.poll(es); while (eng.popEvent(e)) {} sleepMs(20); }
    }
    CHECK(pushed, "replacing that clip's notes is accepted (after the cell's ack)");

    void* retired = nullptr;
    for (int i = 0; i < 40 && !retired; ++i) {
        eng.poll(es);
        while (eng.popEvent(e)) if (e.type == Ev::NotesRetired) retired = e.p;
        sleepMs(10);
    }
    CHECK(retired == (void*)n1.data(),
          "Ev::NotesRetired came back for the DISPLACED array (%p, wanted %p) — "
          "without this App::retiringNotes_ grows for the life of the session",
          retired, (void*)n1.data());

    // --- a notes-only edit on a BIG clip has to reach the pool --------------
    //
    // The clip cache is keyed by the source address, so an array edited IN
    // PLACE — which is every piano-roll edit — is recognised as changed only by
    // its fingerprint. That fingerprint used to be 256 strided 8-byte words of
    // the payload. RtNote is 24 B, three words a note, so 800 notes are 2 400
    // words and the stride was 2 400 / 256 = 9: every word it looked at was a
    // multiple of 3, which is always a note's FIRST word (`beat`). The word
    // holding pitch, velocity, CHANCE and velTo was not "unlikely to be
    // sampled", it was never sampled at all. Turning one note's chance produced
    // the same fingerprint, poolRefFor served the cached block, and the daemon
    // kept playing the old notes.
    //
    // So the assertion is deliberately NOT about the GUI's own array (which is
    // right by construction — the test just wrote it) but about the pool block
    // the published clip cell points at, which is the memory the daemon
    // reinterprets as RtNote[] and plays.
    banner("the pool fingerprint sees a notes-only edit on a long clip");
    {
        const i64 kN    = 800;      // > 683: past the point the stride opened up
        const i64 kEdit = 700;      // and far enough in that the old hash never looked
        std::vector<RtNote> big((size_t)kN);
        for (i64 i = 0; i < kN; ++i) {
            big[(size_t)i].beat   = 0.25 * (double)i;
            big[(size_t)i].len    = 0.25;
            big[(size_t)i].pitch  = (u8)(36 + (i % 48));
            big[(size_t)i].vel    = 100;
            big[(size_t)i].chance = 100;
        }

        Command bc;
        bc.type = Cmd::SetClip; bc.a = 2; bc.b = 0;
        bc.clip.isMidi = true;
        bc.clip.notes = big.data(); bc.clip.noteCount = (int)kN;
        bc.clip.lengthBeats = 200.0; bc.clip.gain = 1.0f; bc.clip.valid = true;
        bc.clip.quantumIdx = 0;

        bool put = false;
        for (int i = 0; i < 40 && !put; ++i) {
            put = eng.pushCommand(bc);
            if (!put) { eng.poll(es); while (eng.popEvent(e)) {} sleepMs(20); }
        }
        CHECK(put, "an %lld-note clip is published", (long long)kN);
        for (int i = 0; i < 20; ++i) { eng.poll(es); while (eng.popEvent(e)) {} sleepMs(10); }

        std::vector<RtNote> seen((size_t)kN);
        i64 got = eng.publishedNotes(2, 0, seen.data(), kN);
        CHECK(got == kN, "the pool block behind cell (2,0) holds all %lld of them (%lld)",
              (long long)kN, (long long)got);
        CHECK(got == kN && seen[(size_t)kEdit].chance == 100 &&
              seen[(size_t)kEdit].pitch == big[(size_t)kEdit].pitch,
              "and note %lld arrived intact (chance %d, pitch %d)", (long long)kEdit,
              got == kN ? (int)seen[(size_t)kEdit].chance : -1,
              got == kN ? (int)seen[(size_t)kEdit].pitch : -1);

        // THE EDIT. One byte, in place, at the same address — chance, which is
        // the smallest thing the piano roll can change and the one with no
        // other field to give it away.
        big[(size_t)kEdit].chance = 37;

        put = false;
        for (int i = 0; i < 60 && !put; ++i) {
            put = eng.pushCommand(bc);
            if (!put) { eng.poll(es); while (eng.popEvent(e)) {} sleepMs(20); }
        }
        CHECK(put, "the same clip is republished after the edit (same pointer, same count)");
        for (int i = 0; i < 30; ++i) { eng.poll(es); while (eng.popEvent(e)) {} sleepMs(10); }

        got = eng.publishedNotes(2, 0, seen.data(), kN);
        CHECK(got == kN && seen[(size_t)kEdit].chance == 37,
              "THE DAEMON'S COPY CHANGED: note %lld reads chance %d (want 37). A "
              "fingerprint that did not cover this byte leaves the engine playing "
              "the pre-edit notes with nothing on screen to say so",
              (long long)kEdit, got == kN ? (int)seen[(size_t)kEdit].chance : -1);

        bool restIntact = got == kN;
        for (i64 i = 0; restIntact && i < kN; ++i) {
            const RtNote& a = seen[(size_t)i];
            const RtNote& b = big[(size_t)i];
            if (a.beat != b.beat || a.len != b.len || a.pitch != b.pitch ||
                a.vel != b.vel || a.chance != b.chance) restIntact = false;
        }
        CHECK(restIntact, "and every other note came across unchanged — the republish "
                          "is a fresh copy of the whole array, not a patch");
    }

    // =====================================================================
    // STEP 4: a device published as a chain reaches the engine and SOUNDS
    // =====================================================================
    //
    // This is the whole of step 4 from the near side. App::publishChain() hands
    // pushCommand() an RtChain full of PluginInstance*; the handle reads the
    // chain's DESCRIPTION off those instances and reconciles the daemon toward
    // it. Nothing about App changes, and nothing about the RtChain crosses.
    //
    // FakeDevice is a PluginInstance that renders nothing and describes
    // nxtakt:saturator. That is not a shortcut, it is the point: in daemon mode
    // the GUI's instance never renders anything either — it is the model, and
    // what the handle needs from it is exactly desc().uri, paramInfo(i).id,
    // getParam(i) and bypassed(). A fake that supplies those is the same input
    // a real one is. (It also lets this suite keep its promise of linking no
    // plugin backends: every one of those is a virtual call.)
    banner("step 4: a device chain, over the wire");

    const f32 dryPeak = peakOver(eng, es, 120);
    CHECK(dryPeak > 0.3f && dryPeak < 0.7f,
          "the bare clip still meters %.4f on the master", (double)dryPeak);

    FakeDevice sat;
    RtChain ch0;
    ch0.fx[0] = &sat;
    ch0.count = 1;
    Command chain;
    chain.type = Cmd::SetChain; chain.a = 0; chain.p = &ch0;
    CHECK(eng.pushCommand(chain),
          "Cmd::SetChain is ACCEPTED now — answering false would make "
          "App::addDevice() roll the device back out of the model as "
          "'engine busy', which is the visible-and-silent bug step 4 removes");

    // Asynchronous, and the GUI has to be able to say so: the chain is in the
    // model already and is not yet the chain that sounds. A `devicesPending`
    // that nobody ever saw non-zero would read as a plausible zero for ever,
    // which is why it is asserted here and not only at the end.
    eng.poll(es);
    CHECK(eng.devicesPending() == 1 && es.devicesPending == 1,
          "one device is outstanding while the engine loads it (%u / %u)",
          eng.devicesPending(), es.devicesPending);

    // The first AddDevice starts the daemon's plugin scan, so this is the one
    // place a device add can take seconds rather than a frame.
    const RemoteDevice* rd = nullptr;
    for (int i = 0; i < 1200 && !(rd && rd->live); ++i) {
        eng.poll(es);
        while (eng.popEvent(e)) {}
        rd = eng.remoteDevice(&sat);
        sleepMs(10);
    }
    CHECK(rd && rd->live, "the engine instantiated it: device %u '%s'",
          rd ? rd->id : 0u, rd ? rd->name.c_str() : "-");
    CHECK(rd && rd->uri == "nxtakt:saturator",
          "and the engine's own table names it '%s'", rd ? rd->uri.c_str() : "-");
    CHECK(rd && rd->paramsMapped == 3 && rd->paramsUnmapped == 0,
          "all 3 controls matched by ParamInfo::id, none guessed at (mapped %u, "
          "unmapped %u) — docs/PARAM-ADDRESS.md",
          rd ? rd->paramsMapped : 0u, rd ? rd->paramsUnmapped : 0u);
    CHECK(eng.devicesAdded() == 1 && eng.devicesFailed() == 0,
          "one added, none failed (%llu / %llu)",
          (unsigned long long)eng.devicesAdded(), (unsigned long long)eng.devicesFailed());
    CHECK(eng.devicesPending() == 0 && es.devicesPending == 0,
          "and nothing is outstanding, so the chain on screen is the chain that sounds");

    // The device is in the chain, and the meter proves it: the clip is DC 0.5
    // and the saturator's shaper is y = tanh(g*x) * comp, so at the default
    // 0 dB drive it reads tanh(0.5) = 0.4621. Not "unchanged" — the point is
    // that it changed by exactly the amount the DEVICE would change it.
    //
    // (The obvious probe, turning Drive up, is deliberately not the one used
    // below: the device's gain compensation is written so that a large drive
    // tends to tanh(0.5) too — see internal_devices.cpp — so on a DC 0.5 the
    // two ends of that knob happen to meter identically. A test that cannot
    // tell a working param write from a dropped one is worse than no test.)
    const f32 satPeak = peakOver(eng, es, 120);
    CHECK(satPeak > 0.40f && satPeak < dryPeak * 0.98f,
          "the saturator is really in the chain: %.4f, which is tanh(0.5) on a "
          "DC 0.5 clip, against %.4f dry", (double)satPeak, (double)dryPeak);

    // --- THE PRIZE: a knob turned on the GUI's instance changes the audio ---
    //
    // Nothing is sent here. sat.setParam() is exactly what drawDeviceStrip()
    // does to the instance it holds; the handle notices on the next poll() and
    // writes the param table. That is the whole knob path in daemon mode — and
    // it is a poll rather than a hook because a knob drag has no command to
    // hang one on.
    //
    // Output trim, and not Drive, so the number is monotone: -12 dB is a factor
    // of 0.251. It is also the control with ParamInfo::id 1, so a mapping that
    // had guessed positionally and got it wrong would move Drive instead and
    // show up here as no change at all.
    sat.setParam(kOutput, -12.f);
    const f32 trimmed = peakOver(eng, es, 150);
    CHECK(trimmed < satPeak * 0.40f && trimmed > satPeak * 0.15f,
          "turning Output to -12 dB on the GUI's OWN instance changed what the "
          "daemon renders: %.4f -> %.4f (x0.251 expected). Nothing was sent",
          (double)satPeak, (double)trimmed);
    sat.setParam(kOutput, 0.f);

    // --- bypass is a command, not a param write ----------------------------
    //
    // A one-line change that is easy to get wrong by reflex: bypass has to
    // order against the chain edits around it, so it is Cmd::SetBypass and not
    // a slot in the param table (§3.7).
    sat.setBypassed(true);
    const f32 bypassed = peakOver(eng, es, 150);
    CHECK(std::fabs(bypassed - dryPeak) < 0.02f,
          "bypassing it on the model bypasses it in the engine: %.4f, back to "
          "the dry %.4f", (double)bypassed, (double)dryPeak);
    sat.setBypassed(false);
    const f32 unbypassed = peakOver(eng, es, 150);
    CHECK(std::fabs(unbypassed - satPeak) < 0.02f,
          "and un-bypassing puts it back: %.4f", (double)unbypassed);

    // --- removing it: the chain shrinks and the retirement comes home ------
    //
    // App::removeDevice() publishes the shorter chain and then hangs the dead
    // instance off the retiring_ entry publishChain() just made, freeing it
    // when Ev::ChainRetired arrives. There is no engine here to send that, so
    // the handle synthesises it for the DISPLACED chain — without which
    // App::retiring_ grows for the life of the session and every removed plugin
    // leaks.
    RtChain ch1;
    ch1.count = 0;
    Command clear;
    clear.type = Cmd::SetChain; clear.a = 0; clear.p = &ch1;
    CHECK(eng.pushCommand(clear), "an empty chain for track 0 is accepted");
    void* retiredChain = nullptr;
    for (int i = 0; i < 200 && !retiredChain; ++i) {
        eng.poll(es);
        while (eng.popEvent(e)) if (e.type == Ev::ChainRetired) retiredChain = e.p;
        sleepMs(10);
    }
    CHECK(retiredChain == (void*)&ch0,
          "Ev::ChainRetired came back for the DISPLACED chain (%p, wanted %p)",
          retiredChain, (void*)&ch0);
    CHECK(eng.remoteDevice(&sat) == nullptr,
          "and the device is gone from the mirror");
    const f32 afterPeak = peakOver(eng, es, 150);
    CHECK(std::fabs(afterPeak - dryPeak) < 0.02f,
          "the track is dry again and still sounding: %.4f (was %.4f with the "
          "device, %.4f before it)", (double)afterPeak, (double)satPeak, (double)dryPeak);

    // --- a plugin the engine does not have is refused ONCE -----------------
    //
    // Fail closed, and fail once. A failure that were retried every frame would
    // be a command per frame for the life of the session.
    FakeDevice ghost("nxtakt:no-such-device", "Ghost");
    RtChain ch2;
    ch2.fx[0] = &ghost;
    ch2.count = 1;
    Command bad;
    bad.type = Cmd::SetChain; bad.a = 0; bad.p = &ch2;
    CHECK(eng.pushCommand(bad), "a chain naming an unknown plugin is still accepted");
    const RemoteDevice* gd = nullptr;
    for (int i = 0; i < 300 && !(gd && gd->failed); ++i) {
        eng.poll(es);
        while (eng.popEvent(e)) {}
        gd = eng.remoteDevice(&ghost);
        sleepMs(10);
    }
    CHECK(gd && gd->failed, "and the engine answered with a reason: '%s'",
          gd ? gd->error.c_str() : "-");
    const u64 failed0 = eng.devicesFailed();
    for (int i = 0; i < 60; ++i) { eng.poll(es); while (eng.popEvent(e)) {} sleepMs(5); }
    CHECK(eng.devicesFailed() == failed0,
          "it is not retried every frame (%llu failures, unchanged over 60 more "
          "polls)", (unsigned long long)eng.devicesFailed());

    // =====================================================================
    // STEP 4b: a RACK's CONTENTS (protocol v7, docs/RACKS.md)
    // =====================================================================
    //
    // Everything above this point uses FakeDevice, because what the handle needs
    // off a chain is four virtual calls and a fake supplies them. A rack is the
    // one device where that stops being true: `rackStateToString` is real code
    // in internal_devices.cpp, `setState` is real code in the daemon, and a fake
    // RackControl on this side would only prove that the handle can serialise a
    // fake. So this section links the plugin layer and uses a REAL
    // `nxtakt:rack` holding a REAL `nxtakt:saturator`.
    //
    // The GUI's rack renders nothing — there is no in-process engine — so every
    // level measured below was computed by plugins the daemon instantiated from
    // a string this process wrote.
    banner("step 4b: a rack's contents, and they sound");

    PluginRegistry reg;
    reg.scan();
    const PluginDesc* rackDesc = reg.find("nxtakt:rack");
    const PluginDesc* satDesc  = reg.find("nxtakt:saturator");
    CHECK(rackDesc != nullptr && satDesc != nullptr,
          "the local registry has nxtakt:rack and nxtakt:saturator");

    std::unique_ptr<PluginInstance> rackInst;
    if (rackDesc) rackInst = reg.instantiate(*rackDesc, eng.sampleRate(), 1024);
    RackControl* rc = rackInst ? rackInst->rack() : nullptr;
    CHECK(rc != nullptr, "and a real rack instance answers rack() non-null");

    if (rc && satDesc) {
        RtChain chR;
        chR.fx[0] = rackInst.get();
        chR.count = 1;
        Command rackChain;
        rackChain.type = Cmd::SetChain; rackChain.a = 0; rackChain.p = &chR;
        CHECK(eng.pushCommand(rackChain), "publish a chain holding one rack");

        const RemoteDevice* rr = nullptr;
        for (int i = 0; i < 600 && !(rr && rr->live); ++i) {
            eng.poll(es);
            while (eng.popEvent(e)) {}
            rr = eng.remoteDevice(rackInst.get());
            sleepMs(10);
        }
        CHECK(rr && rr->live, "the engine instantiated it: device %u '%s'",
              rr ? rr->id : 0u, rr ? rr->name.c_str() : "-");

        // An EMPTY rack is a passthrough. This is the state the whole feature
        // exists to leave behind, so it is measured rather than assumed: without
        // it, "the rack sounds" below could be a rack that was never empty.
        const f32 emptyRack = peakOver(eng, es, 150);
        CHECK(std::fabs(emptyRack - dryPeak) < 0.02f,
              "an empty rack passes the clip straight through: %.4f (dry %.4f)",
              (double)emptyRack, (double)dryPeak);

        // --- fill it, and send NOTHING -------------------------------------
        //
        // rc->addDevice() is exactly what a rack editor calls on the GUI's own
        // instance. There is no command for it and there could not be one, so
        // the handle notices by polling a fingerprint of RackState — the same
        // shape as the parameter mirror, one level down.
        CHECK(rc->addDevice(*satDesc), "drop a saturator into the GUI's rack");
        CHECK(rc->deviceCount() == 1, "the GUI's rack holds one device");
        const u64 sent0 = eng.racksPublished();

        // tanh(0.5) = 0.4621 on a DC 0.5 clip, which is the saturator's own
        // shaper and NOT "some change": the number identifies the device.
        const f32 filled = peakOver(eng, es, 200);
        CHECK(filled > 0.44f && filled < 0.48f,
              "the rack's CONTENTS crossed and sound: %.4f, which is tanh(0.5) "
              "computed by a plugin the daemon built from our state string "
              "(empty rack was %.4f)", (double)filled, (double)emptyRack);
        CHECK(eng.racksPublished() > sent0 && eng.racksRefused() == 0,
              "one rack state published, none refused (%llu / %llu)",
              (unsigned long long)eng.racksPublished(),
              (unsigned long long)eng.racksRefused());

        // --- an in-place edit three levels down is noticed ------------------
        //
        // The sub-device's Output, turned on the GUI's own instance inside the
        // GUI's own rack. Nothing is sent; the fingerprint moves because it
        // hashes the state IN FULL, and a strided one would not have — which is
        // the notes bug (§11.4) wearing a rack.
        PluginInstance* sub = rc->device(0);
        CHECK(sub != nullptr, "the rack hands back its sub-device");
        if (sub) {
            sub->setParam(kOutput, -12.f);
            const f32 trimmed = peakOver(eng, es, 200);
            CHECK(trimmed < filled * 0.40f && trimmed > filled * 0.15f,
                  "turning Output to -12 dB INSIDE the rack changed what the "
                  "daemon renders: %.4f -> %.4f (x0.251 expected)",
                  (double)filled, (double)trimmed);

            // --- THE PARKED VALUE, across a process boundary ---------------
            //
            // RACKS.md: setState restores mappings STRUCTURALLY and writes the
            // macros without driving them, precisely so a target the user parked
            // off its macro's curve comes back parked. Here macro 0 is mapped to
            // Output over -24..0 dB and left at 1.0 — which would DERIVE 0 dB,
            // i.e. the full 0.4621 — while Output itself is parked at -12.
            //
            // 0.116 means the daemon honoured the parked value. 0.462 means
            // something in the load path re-derived it from the macro, which is
            // the bug that makes a set drift a little every time it is opened.
            RackMapping mp;
            mp.macro  = 0;
            mp.device = 0;
            mp.param  = 1u;              // ParamInfo::id of Output
            mp.min    = -24.f;
            mp.max    = 0.f;
            CHECK(rc->addMapping(mp) >= 0, "map macro 0 to Output over -24..0 dB");
            rackInst->setParam(0, 1.0f);   // drives Output to 0 dB, at edit time
            sub->setParam(kOutput, -12.f); // and now PARK it off the curve
            const f32 parked = peakOver(eng, es, 250);
            CHECK(parked < filled * 0.40f && parked > filled * 0.15f,
                  "the parked -12 dB survived the wire even though macro 0 sits "
                  "at 1.0: %.4f (a re-derived target would read ~%.4f)",
                  (double)parked, (double)filled);

            // And a genuine macro move still moves it: 0.0 derives -24 dB.
            rackInst->setParam(0, 0.0f);
            const f32 swept = peakOver(eng, es, 250);
            CHECK(swept < parked * 0.5f,
                  "and sweeping the macro to 0 takes the target to -24 dB: "
                  "%.4f -> %.4f", (double)parked, (double)swept);
        }

        CHECK(eng.racksRefused() == 0,
              "no rack state was refused over the whole section (%llu)",
              (unsigned long long)eng.racksRefused());
    }

    // =====================================================================
    // STEP 4e: THE SAMPLER — generic device state, and the audio it names
    // =====================================================================
    //
    // The near side of GUI-ON-DAEMON.md §15. `nxtakt:sampler` IS the file it
    // plays and no parameter can say which one, so until this wave a sampler in
    // daemon mode was structurally silent: the path lives in stateString(),
    // generic device state did not cross, and `nxtaktd` links no decoder anyway.
    //
    // Everything below is driven exactly as `app_devices.cpp` drives it — a
    // buffer handed to the GUI's OWN SamplerControl::adopt(), which is what
    // dropping a file on a sampler card calls — and nothing is sent by hand.
    // The handle notices by polling a fingerprint of the state string and of the
    // buffer behind it, the same shape as the rack's, for the same reason: there
    // is no command to hook.
    banner("step 4e: a sampler's file and its audio cross, and it SOUNDS");
    {
        // Track 1's meter, not the master: track 0 is where the clip section
        // leaves a 0.5 DC clip running, and a master peak would be that clip
        // whatever the sampler did.
        auto peakTrack1 = [&](int frames) {
            Event ev;
            f32 pk = 0.f;
            const int settle = frames / 3 + 10;
            for (int i = 0; i < settle; ++i) { eng.poll(es); while (eng.popEvent(ev)) {} sleepMs(10); }
            for (int i = 0; i < frames; ++i) {
                eng.poll(es);
                while (eng.popEvent(ev)) {}
                pk = std::fmax(pk, es.meterL[1]);
                sleepMs(10);
            }
            return pk;
        };
        auto noteOn  = [&](u8 v) { MidiMsg mm{}; mm.status = 0x90; mm.d1 = 60; mm.d2 = v; eng.pushMidi(mm); };
        auto noteOff = [&]()     { MidiMsg mm{}; mm.status = 0x80; mm.d1 = 60; mm.d2 = 0; eng.pushMidi(mm); };

        const PluginDesc* smpDesc = reg.find("nxtakt:sampler");
        CHECK(smpDesc != nullptr, "the local registry has nxtakt:sampler");
        std::unique_ptr<PluginInstance> smpInst;
        if (smpDesc) smpInst = reg.instantiate(*smpDesc, eng.sampleRate(), 1024);
        SamplerControl* sc = smpInst ? smpInst->sampler() : nullptr;
        CHECK(sc != nullptr, "and a real sampler instance answers sampler() non-null");

        if (sc && smpDesc) {
            Command arm{};
            arm.type = Cmd::TrackArm; arm.a = 1; arm.b = 1;
            CHECK(eng.pushCommand(arm),
                  "arm track 1 — MIDI reaches a chain only on an armed track");

            RtChain chS;
            chS.fx[0] = smpInst.get();
            chS.count = 1;
            Command sChain{};
            sChain.type = Cmd::SetChain; sChain.a = 1; sChain.p = &chS;
            CHECK(eng.pushCommand(sChain), "publish a chain on track 1 holding one sampler");

            const RemoteDevice* sd = nullptr;
            for (int i = 0; i < 600 && !(sd && sd->live); ++i) {
                eng.poll(es);
                while (eng.popEvent(e)) {}
                sd = eng.remoteDevice(smpInst.get());
                sleepMs(10);
            }
            CHECK(sd && sd->live, "the engine instantiated it: device %u '%s'",
                  sd ? sd->id : 0u, sd ? sd->name.c_str() : "-");

            // --- the negative control ---------------------------------------
            //
            // Measured rather than assumed, because "the sampler sounds" below
            // only means something beside "the sampler was silent". This is the
            // shipped behaviour of every wave up to this one.
            noteOn(127);
            const f32 emptyPeak = peakTrack1(120);
            noteOff();
            CHECK(emptyPeak < 1e-3f,
                  "a sampler with no state is SILENT in the daemon: %.4f",
                  (double)emptyPeak);

            // --- give the GUI's own sampler a buffer, and send nothing -------
            //
            // adopt() is what a file drop calls (app_devices.cpp). The PATH is
            // what stateString() will carry and what a saved set would carry;
            // the AUDIO is what the daemon cannot produce for itself.
            // TEN seconds, not one. Under a sanitizer a frame of this loop costs
            // milliseconds, so a one-second sample runs out inside peakTrack1's
            // own settle and the window measures the silence after the note
            // rather than the note. A test whose answer depends on how fast the
            // binary is, is a test that will go red on somebody else's machine.
            const i64 kFrames = 480000;
            auto buf = std::make_shared<SampleBuffer>();
            buf->frames   = kFrames;
            buf->channels = 2;
            buf->rate     = eng.sampleRate();
            buf->data.assign((size_t)kFrames * 2, 0.5f);
            const char* kPath = "/tmp/nxtakt-dw-kick.wav";
            sc->adopt(buf, kPath);
            CHECK(sc->hasSample() && sc->samplePath() == kPath,
                  "the GUI's sampler holds the buffer and names the file");
            CHECK(smpInst->stateString() == std::string("nxsmp1;p=/tmp/nxtakt-dw-kick.wav"),
                  "and its state string is the PERSISTED spelling ('%s')",
                  smpInst->stateString().c_str());

            // WAIT FOR THE PUBLICATION BEFORE PLAYING THE NOTE, and this is not
            // belt and braces. The sampler's gate is on, so a note-on is HELD:
            // a voice that starts while the engine's sampler is still empty
            // produces silence for as long as the key is down, and a buffer
            // adopted afterwards does not retrigger it. Pushing the note first
            // and hoping the state beat it there is a test whose answer depends
            // on how long a 3.8 MB pool write takes — which is exactly what
            // changes under a sanitizer, and did.
            const u64 sent0 = eng.deviceStatesPublished();
            bool published = false;
            for (int i = 0; i < 600 && !published; ++i) {
                eng.poll(es);
                while (eng.popEvent(e)) {}
                published = eng.deviceStatesPublished() > sent0;
                sleepMs(10);
            }
            CHECK(published, "the state went out (%llu -> %llu)",
                  (unsigned long long)sent0,
                  (unsigned long long)eng.deviceStatesPublished());
            // and let the daemon apply it: one pump tick plus the copy.
            for (int i = 0; i < 100; ++i) { eng.poll(es); while (eng.popEvent(e)) {} sleepMs(10); }

            noteOn(127);
            const f32 loaded = peakTrack1(200);
            noteOff();
            CHECK(eng.deviceStatesPublished() > sent0 && eng.deviceStatesRefused() == 0,
                  "one device state published, none refused (%llu / %llu)",
                  (unsigned long long)eng.deviceStatesPublished(),
                  (unsigned long long)eng.deviceStatesRefused());
            CHECK(loaded > 0.4f,
                  "and the SAME note now meters %.4f on track 1 — audio this "
                  "process decoded, written into this process's pool, played by "
                  "an instrument in another one (empty was %.4f)",
                  (double)loaded, (double)emptyPeak);

            // --- LEVEL-MATCHED AGAINST THE SAME PATCH IN-PROCESS -------------
            //
            // A second sampler, in THIS process, prepared at the same rate, fed
            // the same buffer through the same adopt() and the same note. This
            // is the rack wave's proof shape: the same number on both sides, not
            // "it made a sound".
            //
            // The device is rendered directly rather than through an Engine on
            // purpose — an in-process Engine would add a mixer, a meter's
            // ballistics and a scheduling phase to a comparison that is about
            // the INSTRUMENT. What crosses the wire is the buffer and the path;
            // what has to match is what the instrument does with them.
            f32 localPeak = 0.f;
            if (std::unique_ptr<PluginInstance> ref2 =
                    reg.instantiate(*smpDesc, eng.sampleRate(), 1024)) {
                SamplerControl* sc2 = ref2->sampler();
                CHECK(sc2 != nullptr, "a second sampler, in this process");
                if (sc2) {
                    ref2->prepare(eng.sampleRate(), 256);
                    auto buf2 = std::make_shared<SampleBuffer>(*buf);
                    sc2->adopt(buf2, kPath);
                    const u8 on[3] = { 0x90, 60, 127 };
                    ref2->midi(on, 3, 0);
                    std::vector<f32> l((size_t)256, 0.f), r((size_t)256, 0.f);
                    f32* outp[2] = { l.data(), r.data() };
                    const f32* inp[2] = { l.data(), r.data() };
                    // A few blocks: the amp envelope's 0.5 ms attack is inside
                    // the first one, so one block would measure the attack
                    // rather than the level.
                    for (int b = 0; b < 8; ++b) {
                        std::fill(l.begin(), l.end(), 0.f);
                        std::fill(r.begin(), r.end(), 0.f);
                        ref2->process(inp, outp, 2, 256);
                        for (int i = 0; i < 256; ++i) localPeak = std::fmax(localPeak, std::fabs(l[(size_t)i]));
                    }
                }
            }
            CHECK(localPeak > 0.4f, "the in-process patch peaks at %.4f", (double)localPeak);
            CHECK(std::fabs(loaded - localPeak) < 0.01f,
                  "AND THE TWO AGREE: daemon %.4f vs in-process %.4f",
                  (double)loaded, (double)localPeak);

            // --- an INDEPENDENT client, measuring the daemon's own meter -----
            //
            // §11.8's experiment, made a test. This process decoded nothing on
            // that client's behalf and the client owns no pool: everything it
            // reads is what nxtaktd published about audio it is rendering out of
            // a buffer it never opened a file for.
            {
                ipc::EngineClient probe;
                if (probe.attach(session, 2000)) {
                    // Let the previous note's RELEASE finish first. The sampler
                    // is polyphonic and its release is 40 ms, so a second note
                    // pushed on top of a still-dying one sums two voices and
                    // reads ~1.0 -- a true measurement of the wrong thing, and
                    // the kind of number that makes a comparison table lie.
                    for (int i = 0; i < 200; ++i) {
                        eng.poll(es); while (eng.popEvent(e)) {} sleepMs(5);
                    }
                    noteOn(127);
                    f32 probePeak = 0.f;
                    for (int i = 0; i < 1500; ++i) {
                        eng.poll(es);                       // keep the near side pumping
                        while (eng.popEvent(e)) {}
                        probePeak = std::fmax(probePeak, probe.state().meterL[1].load());
                        sleepMs(1);
                    }
                    noteOff();
                    // The engine publishes a meter once per audio block and decays
                    // it by 0.72 each block, so this polls FASTER than the block
                    // rate: a 10 ms poll against a 5.33 ms block misses one
                    // publication in two and reads 0.72x the peak.
                    CHECK(std::fabs(probePeak - loaded) < 0.01f,
                          "a second EngineClient on the same session measures %.4f "
                          "on track 1 — the same number, and it decoded nothing, "
                          "drew nothing and owns no pool",
                          (double)probePeak);
                    probe.detach();
                } else {
                    CHECK(false, "a second client could not attach: %s", probe.error());
                }
            }

            // --- a re-point is noticed, and the PATH is not enough -----------
            //
            // The fingerprint hashes the buffer as well as the string, and this
            // is why: the same file, re-decoded, is the same PATH and different
            // AUDIO. Hashing only the text would serve the cached publication and
            // leave the daemon playing the old bytes under the right name.
            {
                auto quiet = std::make_shared<SampleBuffer>();
                quiet->frames   = kFrames;
                quiet->channels = 2;
                quiet->rate     = eng.sampleRate();
                quiet->data.assign((size_t)kFrames * 2, 0.125f);
                const u64 sentBefore = eng.deviceStatesPublished();
                sc->adopt(quiet, kPath);                    // SAME path, new bytes
                CHECK(smpInst->stateString() == std::string("nxsmp1;p=/tmp/nxtakt-dw-kick.wav"),
                      "the state string is unchanged by the re-point");
                // WAIT for the publication instead of assuming a fixed number of
                // frames is enough for it. The fingerprint moving is what this
                // paragraph is about, so "did it go out" has to be observed
                // before "what does it sound like" is asked.
                bool republished = false;
                for (int i = 0; i < 400 && !republished; ++i) {
                    eng.poll(es);
                    while (eng.popEvent(e)) {}
                    republished = eng.deviceStatesPublished() > sentBefore;
                    sleepMs(10);
                }
                CHECK(republished,
                      "the fingerprint moved and a second state went out (%llu -> %llu)",
                      (unsigned long long)sentBefore,
                      (unsigned long long)eng.deviceStatesPublished());
                for (int i = 0; i < 100; ++i) { eng.poll(es); while (eng.popEvent(e)) {} sleepMs(10); }
                noteOn(127);
                const f32 requiet = peakTrack1(200);
                noteOff();
                CHECK(requiet < loaded * 0.5f && requiet > 0.05f,
                      "and the daemon plays the NEW buffer anyway: %.4f (was %.4f) "
                      "— the path alone does not identify audio",
                      (double)requiet, (double)loaded);
            }

            // --- clearing ----------------------------------------------------
            const u64 sentClear = eng.deviceStatesPublished();
            sc->clearSample();
            CHECK(smpInst->stateString().empty(),
                  "an empty sampler has no state string at all, so a set with no "
                  "sampler in it stays byte-identical to what an older writer produced");
            for (int i = 0; i < 400 && eng.deviceStatesPublished() == sentClear; ++i) {
                eng.poll(es); while (eng.popEvent(e)) {} sleepMs(10);
            }
            CHECK(eng.deviceStatesPublished() > sentClear,
                  "the empty state went out too (%llu -> %llu)",
                  (unsigned long long)sentClear,
                  (unsigned long long)eng.deviceStatesPublished());
            for (int i = 0; i < 100; ++i) { eng.poll(es); while (eng.popEvent(e)) {} sleepMs(10); }
            noteOn(127);
            const f32 cleared = peakTrack1(200);
            noteOff();
            CHECK(cleared < 1e-3f,
                  "and the daemon's sampler is empty and silent again: %.4f",
                  (double)cleared);

            CHECK(eng.deviceStatesRefused() == 0,
                  "no device state was refused over the whole section (%llu)",
                  (unsigned long long)eng.deviceStatesRefused());

            // Put track 1 back the way it was found.
            RtChain none;
            none.count = 0;
            Command drop{};
            drop.type = Cmd::SetChain; drop.a = 1; drop.p = &none;
            eng.pushCommand(drop);
            Command unarm{};
            unarm.type = Cmd::TrackArm; unarm.a = 1; unarm.b = 0;
            eng.pushCommand(unarm);
            for (int i = 0; i < 40; ++i) { eng.poll(es); while (eng.popEvent(e)) {} sleepMs(10); }
        }
    }

    // =====================================================================
    // STEP 5: the browser lists what the DAEMON can load
    // =====================================================================
    banner("step 5: the catalog");

    CHECK(eng.catalogReady(), "the catalog arrived with EvScanComplete");
    const std::vector<PluginDesc>& cat = eng.catalog();
    CHECK(cat.size() > 2, "it has %zu plugins", cat.size());
    bool sawSat = false, sawPulse = false;
    for (const PluginDesc& d : cat) {
        if (d.uri == "nxtakt:saturator") sawSat = true;
        if (d.uri == "nxtakt:pulse")     sawPulse = true;
    }
    CHECK(sawSat && sawPulse, "including the stock devices (saturator %d, pulse %d)",
          (int)sawSat, (int)sawPulse);
    // The reason a catalog exists at all: this list is the DAEMON's answer, so
    // a row the browser draws is a row AddDevice can load. A GUI browsing its
    // own PluginRegistry could offer one the daemon has never heard of.
    for (const PluginDesc& d : cat)
        if (d.uri == "nxtakt:pulse")
            CHECK(d.kind == PluginKind::Instrument && d.hasMidiIn,
                  "with their real shape: Pulse is an instrument that takes MIDI");
    CHECK(eng.catalogTruncated() == 0,
          "nothing was dropped for want of table space (%u)", eng.catalogTruncated());

    // =====================================================================
    // STEP 4c: THE ARRANGEMENT, over the wire
    // =====================================================================
    //
    // The daemon has been able to take an arrangement since wave 8g —
    // translateArrangement() and daemon_test §16b prove it plays one. What was
    // missing until now is the near side: App builds an `RtArrangement`, a
    // struct of pointers into one GUI-heap allocation, and something has to turn
    // it into the blob. That is what this exercises, and the only honest proof
    // is a timeline that SOUNDS in another process.
    banner("step 4c: the arrangement");

    // Clear the chain and stop everything, so what the meter reads is the lane
    // and not a session slot or a device left over from step 4.
    RtChain chNone;
    chNone.count = 0;
    Command clearChain;
    clearChain.type = Cmd::SetChain; clearChain.a = 0; clearChain.p = &chNone;
    CHECK(eng.pushCommand(clearChain), "clear track 0's chain");
    CHECK(eng.send(Cmd::StopAll), "stop every session slot");
    CHECK(eng.send(Cmd::SetPlaying, 0), "and stop the transport");
    for (int i = 0; i < 60; ++i) { eng.poll(es); while (eng.popEvent(e)) {} sleepMs(5); }
    CHECK(eng.send(Cmd::Locate, 0, 0, 0.0), "locate to beat 0");

    // One item, one clip: the same DC 0.5 buffer the session cell used, which is
    // the point of sharing the pointer -> pool-ref cache. It is already in the
    // pool, so this publication writes no samples at all.
    RtClip arrClip{};
    arrClip.data        = dc.data();
    arrClip.frames      = frames;
    arrClip.channels    = 1;
    arrClip.loopStart   = 0;
    arrClip.loopEnd     = frames;
    arrClip.warp        = (int)Warp::Off;
    arrClip.loop        = true;      // the buffer is 2 beats; the item is 16
    arrClip.quantumIdx  = 0;
    arrClip.lengthBeats = 2.0;
    arrClip.gain        = 1.0f;
    arrClip.valid       = true;

    RtArrItem arrItem{};
    arrItem.start  = 0.0;
    // Comfortably longer than peakOver's whole window (a 0.6 s settle plus 1.5 s
    // of measurement, i.e. ~4 beats at 120): a window that ran off the end of
    // the item would read silence and call it a broken encoder.
    arrItem.length = 16.0;
    arrItem.offset = 0.0;
    arrItem.clip   = 0;

    RtArrangement lane{};
    lane.items     = &arrItem;
    lane.clips     = &arrClip;
    lane.itemCount = 1;
    lane.clipCount = 1;

    const u64 arrPub0 = eng.arrangementsPublished();
    Command setLane;
    setLane.type = Cmd::SetArrangement; setLane.a = 0; setLane.p = &lane;
    CHECK(eng.pushCommand(setLane),
          "Cmd::SetArrangement with a GUI-heap RtArrangement is ACCEPTED and encoded");
    CHECK(eng.arrangementsPublished() == arrPub0 + 1,
          "one lane published (%llu)", (unsigned long long)eng.arrangementsPublished());

    // The transport cell: a lane addressed as track -1, carrying no items and
    // only the loop brace. Same command, same encoder, one branch.
    RtArrangement cellArr{};
    cellArr.loopStart = 0.0;
    cellArr.loopEnd   = 8.0;
    cellArr.loopOn    = 0;
    Command setCell;
    setCell.type = Cmd::SetArrangement; setCell.a = -1; setCell.p = &cellArr;
    CHECK(eng.pushCommand(setCell), "and the transport cell, as track -1");

    for (int i = 0; i < 100; ++i) { eng.poll(es); while (eng.popEvent(e)) {} sleepMs(5); }
    CHECK(eng.arrangementsRefused() == 0,
          "the daemon accepted both (%llu refused)",
          (unsigned long long)eng.arrangementsRefused());

    // §4.2: a session launch takes a track OUT of the arrangement, and step 4
    // launched one. The engine sets that bit at the quantized launch it computes,
    // so it is cleared with a command rather than assumed away.
    CHECK(eng.send(Cmd::BackToArrangement, -1),
          "put every track back on the arrangement (a session launch overrode it)");
    CHECK(eng.send(Cmd::SetPlaying, 1), "roll the transport");
    const f32 lanePeak = peakOver(eng, es, 150);
    CHECK(lanePeak > 0.4f && lanePeak < 0.6f,
          "THE TIMELINE PLAYS FROM THE DAEMON: master %.4f on an item that names "
          "a pool block the session clip already wrote", (double)lanePeak);
    eng.poll(es);
    CHECK(es.beat > 0.5, "and the cursor advanced (%.4f)", es.beat);

    // --- the retirement stand-in, one lane out -----------------------------
    //
    // The GUI's RtArrangement is a single allocation App frees when
    // Ev::ArrangementRetired comes home. Nothing crossed — the lane was COPIED
    // into a pool blob — so there is no engine to send that event and the handle
    // has to synthesise it, exactly as it does for a displaced RtNote[]. Without
    // it App::arr_.retiring grows for the life of the session.
    RtArrangement lane2 = lane;
    Command setLane2;
    setLane2.type = Cmd::SetArrangement; setLane2.a = 0; setLane2.p = &lane2;
    CHECK(eng.pushCommand(setLane2), "publish a second lane for track 0");
    void* retiredLane = nullptr;
    for (int i = 0; i < 200 && !retiredLane; ++i) {
        eng.poll(es);
        while (eng.popEvent(e)) if (e.type == Ev::ArrangementRetired) retiredLane = e.p;
        sleepMs(5);
    }
    CHECK(retiredLane == (void*)&lane,
          "Ev::ArrangementRetired came home for the DISPLACED lane (%p, wanted %p)",
          retiredLane, (void*)&lane);

    // --- arrangement automation ---------------------------------------------
    //
    // Its own blob, its own command, its own retirement — and measurable, which
    // is the only way to tell a set the engine EVALUATES from one it merely
    // accepted: two points taking track 0's fader from unity to silence over the
    // first four beats.
    // A CONSTANT lane, not a ramp, and that is a deliberate choice about what is
    // measurable. peakOver is a peak HOLD over a window; against a ramp it
    // reports the loudest instant in the window, so the number depends on where
    // the cursor happened to be and a check written round it is really a check
    // on the sleep. Two points at the same value make the fader a constant
    // 0.8 — which is -8.74 dB, i.e. x0.365 — for as long as the test runs.
    RtAutoPoint pts[2];
    pts[0].beat = 0.0;   pts[0].value = 0.8f;
    pts[1].beat = 512.0; pts[1].value = 0.8f;
    RtAutoLane al{};
    al.target  = (i32)AutoTarget::TrackVol;
    al.xform   = (i32)AutoXform::Fader;
    al.devSlot = -1;
    al.first   = 0;
    al.count   = 2;
    al.lo      = 0.f;
    al.hi      = 1.f;
    RtAutoSetN autos{};
    autos.points     = pts;
    autos.lanes      = &al;
    autos.laneCount  = 1;
    autos.pointCount = 2;

    Command setAutos;
    setAutos.type = Cmd::SetTrackAutos; setAutos.a = 0; setAutos.p = &autos;
    CHECK(eng.pushCommand(setAutos), "Cmd::SetTrackAutos is accepted and encoded");
    CHECK(eng.send(Cmd::SetPlaying, 0) && eng.send(Cmd::Locate, 0, 0, 0.0) &&
          eng.send(Cmd::BackToArrangement, -1) && eng.send(Cmd::SetPlaying, 1),
          "back to beat 0 and roll");
    const f32 ducked = peakOver(eng, es, 120);
    // Bounded on BOTH sides. Below the un-automated level proves the lane is
    // being evaluated; above zero proves it is being evaluated rather than
    // simply silencing the track, which is what a dropped or mis-shaped blob
    // would look like and which a one-sided check would pass.
    CHECK(ducked < lanePeak * 0.55f && ducked > lanePeak * 0.20f,
          "the automation is EVALUATED in the daemon: %.4f, which is fader 0.8 "
          "(x0.365) on the %.4f the lane plays at unity",
          (double)ducked, (double)lanePeak);
    CHECK(eng.arrangementsRefused() == 0,
          "and nothing was refused over the whole section (%llu)",
          (unsigned long long)eng.arrangementsRefused());

    // --- a lane with NOTES, republished, must not allocate every time -------
    //
    // An arrangement's RtNote[] lives INSIDE the lane's single allocation, so
    // every republication puts it at a NEW address and the old one is freed as
    // soon as the retirement comes home. An address-keyed cache — which is what
    // the SESSION clip path correctly uses — would therefore write a fresh pool
    // block per publication and never release one, and a drag republishes the
    // lane every frame. The audio would be right the whole time and the pool
    // would fill up over an afternoon, which is why the only check that can see
    // this is a block count.
    //
    // The lane is heap-allocated and freed each time on purpose: a stack object
    // reused in a loop keeps one address and would make an address cache look
    // like it worked.
    {
        std::vector<RtNote> notes(64);
        for (size_t n = 0; n < notes.size(); ++n) {
            notes[n].beat  = (f64)n * 0.25;
            notes[n].len   = 0.2;
            notes[n].pitch = (u8)(48 + (n % 12));
            notes[n].vel   = 100;
        }
        u64 blocksAfterFirst = 0;
        u8* prev = nullptr;             // freed only AFTER the next one exists
        for (int rep = 0; rep < 24; ++rep) {
            // One allocation holding the lane, its item, its clip and its notes,
            // exactly as App builds one.
            auto* mem = new u8[sizeof(RtArrangement) + sizeof(RtArrItem) +
                               sizeof(RtClip) + notes.size() * sizeof(RtNote)];
            auto* la  = new (mem) RtArrangement{};
            auto* it  = (RtArrItem*)(mem + sizeof(RtArrangement));
            auto* cl  = (RtClip*)((u8*)it + sizeof(RtArrItem));
            auto* nt  = (RtNote*)((u8*)cl + sizeof(RtClip));
            *it = RtArrItem{};
            it->start = 0.0; it->length = 16.0; it->clip = 0;
            *cl = RtClip{};
            cl->notes       = nt;
            cl->noteCount   = (i64)notes.size();
            cl->isMidi      = true;
            cl->lengthBeats = 16.0;
            cl->gain        = 1.0f;
            cl->valid       = true;
            for (size_t n = 0; n < notes.size(); ++n) nt[n] = notes[n];
            la->items = it; la->clips = cl; la->itemCount = 1; la->clipCount = 1;
            la->noteCount = (i64)notes.size();

            Command mc;
            mc.type = Cmd::SetArrangement; mc.a = 1; mc.p = la;
            bool sent = false;
            for (int i = 0; i < 200 && !sent; ++i) {
                eng.poll(es);
                while (eng.popEvent(e)) {}
                sent = eng.pushCommand(mc);
                if (!sent) sleepMs(5);
            }
            if (!sent) { CHECK(false, "a notes-bearing lane would not publish"); delete[] mem; break; }

            for (int i = 0; i < 20; ++i) { eng.poll(es); while (eng.popEvent(e)) {} sleepMs(5); }
            // The previous lane is freed only NOW, which is what App's
            // retirement ordering does and what guarantees the allocator hands
            // out a DIFFERENT address for the next one. Freeing before
            // allocating gets the same block back every time, and an
            // address-keyed cache then looks like it works.
            delete[] prev;
            prev = mem;
            if (rep == 0) blocksAfterFirst = eng.poolBlocksLive();
        }
        delete[] prev;
        CHECK(blocksAfterFirst > 0, "a notes-bearing lane put %llu block(s) in the pool",
              (unsigned long long)blocksAfterFirst);
        const u64 after = eng.poolBlocksLive();
        CHECK(after <= blocksAfterFirst,
              "and 23 more republications added none: %llu blocks, was %llu. The "
              "lane's notes are cached BY POSITION, not by address, because the "
              "address is new every time",
              (unsigned long long)after, (unsigned long long)blocksAfterFirst);

        Command mclear;
        mclear.type = Cmd::SetArrangement; mclear.a = 1; mclear.p = nullptr;
        bool done = false;
        for (int i = 0; i < 200 && !done; ++i) {
            eng.poll(es);
            while (eng.popEvent(e)) {}
            done = eng.pushCommand(mclear);
            if (!done) sleepMs(5);
        }
        CHECK(done, "the lane clears again");
    }

    // --- a lane past the protocol's bounds is CONSUMED, never answered false --
    //
    // §11.2's rule: `false` means "try again" and App's FIFO retries it every
    // frame until it succeeds. A lane that can never be carried must therefore
    // be consumed, counted and logged — never refused with a false, which would
    // wedge the queue and with it the transport.
    const u64 refusals0 = eng.remoteRefusals();
    RtArrangement huge{};
    huge.items     = &arrItem;
    huge.clips     = &arrClip;
    huge.itemCount = (int)ipc::kMaxArrItems + 1;
    huge.clipCount = 1;
    Command setHuge;
    setHuge.type = Cmd::SetArrangement; setHuge.a = 1; setHuge.p = &huge;
    CHECK(eng.pushCommand(setHuge),
          "a lane past kMaxArrItems is CONSUMED, not answered false");
    CHECK(eng.remoteRefusals() == refusals0 + 1,
          "and counted (%llu -> %llu)", (unsigned long long)refusals0,
          (unsigned long long)eng.remoteRefusals());

    // --- clear, so the sections after this one see the set they expect ------
    Command clearLane;
    clearLane.type = Cmd::SetArrangement; clearLane.a = 0; clearLane.p = nullptr;
    CHECK(eng.pushCommand(clearLane), "clear track 0's lane");
    Command clearAutos;
    clearAutos.type = Cmd::SetTrackAutos; clearAutos.a = 0; clearAutos.p = nullptr;
    CHECK(eng.pushCommand(clearAutos), "clear its automation");
    Command clearCell;
    clearCell.type = Cmd::SetArrangement; clearCell.a = -1; clearCell.p = nullptr;
    CHECK(eng.pushCommand(clearCell), "clear the transport cell");
    for (int i = 0; i < 100; ++i) { eng.poll(es); while (eng.popEvent(e)) {} sleepMs(5); }
    eng.send(Cmd::SetPlaying, 0);
    eng.send(Cmd::Locate, 0, 0, 0.0);
    CHECK(eng.send(Cmd::LaunchClip, 0, 0) && eng.send(Cmd::SetPlaying, 1),
          "and put the session clip back for the sections that follow");
    const f32 backToSlot = peakOver(eng, es, 150);
    CHECK(backToSlot > 0.4f, "which sounds again: %.4f", (double)backToSlot);

    // =====================================================================
    // STEP 4d: THE SIGNATURE MAP
    // =====================================================================
    //
    // Cmd::SetSignatures could not even be SENT in daemon mode until now:
    // session.h's publishSignatures() took an Engine&, there is no Engine here,
    // and app_chrome.cpp guarded the call on local(). So daemon mode played
    // every set in 4/4 however the ruler was drawn — and the ruler was drawn
    // from the session's own map, so it drew 7/8 with total confidence.
    //
    // What is asserted is BAR ARITHMETIC and not the signature fields, for the
    // reason §11.6 gives about poisoning: 4/4 is what an unpublished map reads
    // AND what an ignored one reads, so "posSigNum == 4" would pass against a
    // daemon that never got the map. A bar boundary at 3.5 beats cannot.
    banner("step 4d: the signature map");

    CHECK(eng.send(Cmd::SetPlaying, 0), "stop the transport");
    const auto barAtBeat = [&](f64 beat) {
        eng.send(Cmd::Locate, 0, 0, beat);
        for (int i = 0; i < 60; ++i) { eng.poll(es); while (eng.popEvent(e)) {} sleepMs(10); }
        return es.posBar;
    };
    const i32 barIn44 = barAtBeat(7.0);
    CHECK(es.posSigNum == 4 && es.posSigDen == 4,
          "the engine starts in 4/4 (%d/%d)", es.posSigNum, es.posSigDen);

    // The array is GUI-heap and stays alive until Ev::SigsRetired names it —
    // exactly the contract session.h's publishSignatures() documents, which is
    // the caller this stands in for.
    RtSig sevenEight[1];
    sevenEight[0].bar = 0; sevenEight[0].num = 7; sevenEight[0].den = 8;
    sevenEight[0].pad = 0; sevenEight[0].beat = 0.0;
    Command sigCmd;
    sigCmd.type = Cmd::SetSignatures; sigCmd.a = 1; sigCmd.p = sevenEight;
    const u64 sigPub0 = eng.signaturesPublished();
    CHECK(eng.pushCommand(sigCmd),
          "Cmd::SetSignatures with a GUI-heap RtSig[] is ACCEPTED and encoded");
    CHECK(eng.signaturesPublished() == sigPub0 + 1, "one map published (%llu)",
          (unsigned long long)eng.signaturesPublished());

    bool inSeven = false;
    for (int i = 0; i < 300 && !inSeven; ++i) {
        eng.poll(es);
        while (eng.popEvent(e)) {}
        inSeven = es.posSigNum == 7 && es.posSigDen == 8;
        sleepMs(10);
    }
    CHECK(inSeven, "the engine's published signature is 7/8 (%d/%d)",
          es.posSigNum, es.posSigDen);
    CHECK(eng.signaturesRefused() == 0, "and nothing was refused (%llu)",
          (unsigned long long)eng.signaturesRefused());

    const i32 barIn78 = barAtBeat(7.0);
    CHECK(barIn78 == barIn44 + 1,
          "AND IT PLAYS IN IT: beat 7 is bar %d now and was bar %d in 4/4, i.e. "
          "the bars are 3.5 beats long", barIn78, barIn44);

    // The retirement stand-in. syncSignatures keeps the array it published and
    // frees it when Ev::SigsRetired names it; nothing crossed (the map was
    // COPIED into a blob), so the handle has to send that event or every map a
    // session ever publishes is held for the life of it.
    RtSig fiveFour[1];
    fiveFour[0].bar = 0; fiveFour[0].num = 5; fiveFour[0].den = 4;
    fiveFour[0].pad = 0; fiveFour[0].beat = 0.0;
    Command sigCmd2;
    sigCmd2.type = Cmd::SetSignatures; sigCmd2.a = 1; sigCmd2.p = fiveFour;
    CHECK(eng.pushCommand(sigCmd2), "publish a 5/4 map over it");
    void* retiredSigs = nullptr;
    for (int i = 0; i < 200 && !retiredSigs; ++i) {
        eng.poll(es);
        while (eng.popEvent(e)) if (e.type == Ev::SigsRetired) retiredSigs = e.p;
        sleepMs(5);
    }
    CHECK(retiredSigs == (void*)sevenEight,
          "Ev::SigsRetired came home for the DISPLACED map (%p, wanted %p)",
          retiredSigs, (void*)sevenEight);

    // RETRIED, because `false` from the handle means exactly one thing: try
    // again (§11.2). The 5/4 publication above is still un-acknowledged at this
    // instant — the retirement event the loop just waited for is SYNTHESISED at
    // push time and says nothing about the daemon — so the first attempt is
    // refused on flow control, which is what App::flushPending() exists to do.
    Command sigClear;
    sigClear.type = Cmd::SetSignatures; sigClear.a = 0; sigClear.p = nullptr;
    bool cleared = false;
    for (int i = 0; i < 200 && !cleared; ++i) {
        eng.poll(es);
        while (eng.popEvent(e)) {}
        cleared = eng.pushCommand(sigClear);
        if (!cleared) sleepMs(5);
    }
    CHECK(cleared, "clear the map (retried through the refusal, as the FIFO does)");
    bool backTo44 = false;
    for (int i = 0; i < 300 && !backTo44; ++i) {
        eng.poll(es);
        while (eng.popEvent(e)) {}
        backTo44 = es.posSigNum == 4 && es.posSigDen == 4;
        sleepMs(10);
    }
    CHECK(backTo44, "and the engine is back in 4/4 (%d/%d)", es.posSigNum, es.posSigDen);
    eng.send(Cmd::Locate, 0, 0, 0.0);

    // =====================================================================
    // STEP 4f: THE THREE PAYLOADS THAT USED TO BE REFUSED (protocol v11)
    // =====================================================================
    //
    // Until v11 the handle carried a clip's SAMPLE and its NOTES and nothing
    // else: RtClip::autos, ::markers and ::transients had no WireClip field, so
    // every clip that had one was consumed with a reason ("clip envelopes, warp
    // markers and transients have no WireClip field") and crossed WITHOUT them.
    // And because the loader builds a transient grid for every audio sample,
    // that was not a corner: EVERY audio set in daemon mode counted a refusal
    // per clip and played its envelopes, its warp and its beat-repeat wrong.
    //
    // So the first check below is the regression gate and the reason this
    // section exists: a clip carrying all three is pushed and remoteRefusals()
    // MUST NOT MOVE. If the amber refusal ever comes back, that is the check
    // that goes red, before any of the audio ones do.
    //
    // The rest is the audio, three legs, each measured against the SAME RtClip
    // rendered by an in-process engine. Every leg is a BOUND (|daemon - local|
    // < 0.01) and not a comparison of frames, and the reason is structural
    // rather than a tolerance somebody settled for:
    //
    //   * THE DAEMON'S FRAMES NEVER CROSS THE BOUNDARY. What comes back over
    //     the wire is the snapshot — block-decayed meters and counters — so
    //     there is no frame stream on this side of it to diff. (The recording
    //     section is the exception and it is an exception on purpose: a take is
    //     the one payload the engine hands back sample for sample.)
    //   * AND THE TWO STREAMS ARE NOT ALIGNABLE ANYWAY. nxtaktd's null driver
    //     renders against the WALL CLOCK: it computes how many blocks are due
    //     from CLOCK_MONOTONIC and renders that many, so the number of blocks
    //     between the launch and any given instant is a property of the machine
    //     and of nothing else. There is no frame k on one side that is frame k
    //     on the other.
    //
    // A PEAK survives both, because every clip here is DC by construction: its
    // level is a property of the material and of the payload under test, not of
    // where the render happened to start. Bit-identity IS claimed, once, where
    // it is achievable — leg (a) rendered twice in-process and memcmp'd.
    banner("step 4f: clip envelopes, warp markers and transients cross (v11)");
    {
        // Track 2, slot 2: track 0 carries the session clip step 6 relaunches
        // and track 1 is where the sampler section parked its chain. The master
        // meter is the one being read, so everything else has to be silent —
        // hence the StopAll, and hence putting the session clip back at the end.
        const int kTrk = 2, kSlot = 2;

        const u64 refusals0 = eng.remoteRefusals();
        const u64 blocks0   = eng.poolBlocksLive();

        // 300 BPM, and the number is load-bearing rather than decorative. The
        // engine re-musicalises the grain hop at every grain boundary as one
        // sixteenth of the CURRENT tempo (engine.cpp, warpHop): at 300 BPM that
        // is 2400 output frames, and leg (c) below needs a hop no larger than
        // the 50 ms cap on the transient snap window (0.05 * 48000 = 2400) or
        // no grid could move a grain origin far enough to be heard. See the leg
        // itself for the arithmetic.
        const f64 kTempo = 300.0;
        CHECK(eng.send(Cmd::StopAll), "stop every session slot: the master meter "
              "is about to be the measurement");
        eng.send(Cmd::SetTempo, 0, 0, kTempo);
        eng.send(Cmd::SetQuantum, 0);                    // launch on the spot
        eng.send(Cmd::TrackVol, kTrk, 0, 1.0);
        eng.send(Cmd::MasterVol, 0, 0, 1.0);
        eng.send(Cmd::SetPlaying, 1);
        for (int i = 0; i < 200 && std::fabs(es.tempo - kTempo) > 1e-6; ++i) {
            eng.poll(es);
            while (eng.popEvent(e)) {}
            sleepMs(10);
        }

        auto localEngine = std::make_unique<Engine>();
        std::vector<f32> localFrames;

        // ONE event pump for the whole section, and it is not a convenience.
        // The retirement events below are SYNTHESISED by the handle at push
        // time, so they are in the queue the instant pushCommand() returns —
        // and every wait, settle and meter poll in this section drains that
        // queue. A leg that popped events itself would be racing the loop that
        // was about to swallow the very event it is waiting for, which is
        // precisely how a retirement test passes for the wrong reason.
        void* sawAutosRetired = nullptr;
        void* sawWarpRetired  = nullptr;
        auto pump = [&]() {
            eng.poll(es);
            while (eng.popEvent(e)) {
                if (e.type == Ev::AutosRetired) sawAutosRetired = e.p;
                if (e.type == Ev::WarpRetired)  sawWarpRetired  = e.p;
            }
        };

        // The daemon's meter, polled at 1 ms. TEN would be wrong and quietly so:
        // the engine publishes the master meter once per audio block (5.33 ms at
        // 48 kHz / 256) and decays it by 0.72 each time, so a 10 ms poll misses
        // one publication in two and reads 0.72x the peak — which is 28% low,
        // i.e. bigger than every bound below by a factor of thirty.
        auto daemonPeak = [&](int settleMs, int windowMs) {
            for (int i = 0; i < settleMs; ++i) { pump(); sleepMs(1); }
            f32 pk = 0.f;
            for (int i = 0; i < windowMs; ++i) {
                pump();
                pk = std::fmax(pk, es.masterMeterL);
                sleepMs(1);
            }
            return pk;
        };

        // Publish into the cell, retrying through flow control exactly as
        // App::flushPending() does — `false` from the handle means "try again
        // next frame" and never "this cannot be carried" (§11.2).
        auto publish = [&](const RtClip& cl) {
            Command sc;
            sc.type = Cmd::SetClip; sc.a = kTrk; sc.b = kSlot; sc.clip = cl;
            bool ok = false;
            for (int i = 0; i < 300 && !ok; ++i) {
                ok = eng.pushCommand(sc);
                if (!ok) { pump(); sleepMs(5); }
            }
            for (int i = 0; i < 40; ++i) { pump(); sleepMs(5); }
            return ok;
        };

        // Stop, republish, relaunch. The stop is not tidiness: a voice holds
        // &clips_[t][s] and keeps its own grain phase, so measuring a variant
        // against a voice that started under the PREVIOUS one would measure a
        // clip that changed underneath the read heads. Every leg starts a voice.
        auto relaunch = [&](const RtClip& cl) {
            eng.send(Cmd::StopTrack, kTrk);
            for (int i = 0; i < 40; ++i) { pump(); sleepMs(5); }
            const bool ok = publish(cl);
            eng.send(Cmd::LaunchClip, kTrk, kSlot);
            return ok;
        };

        // --- the material -------------------------------------------------
        //
        // GUI-heap buffers, alive for the whole section, exactly as App's
        // decoded SampleBuffers are: the handle copies them into the pool and
        // the daemon plays the copy. DC everywhere, because a peak has to mean
        // "the payload changed the gain" and not "the window caught a crest".
        const i64 kOneSec = 48000;
        std::vector<f32> dcHalf((size_t)kOneSec, 0.5f);           // leg (a)

        // leg (b): one second of 0.25 followed by one second of 0.5.
        const i64 kTwoSec = 96000;
        std::vector<f32> stepBuf((size_t)kTwoSec, 0.25f);
        for (i64 i = kOneSec; i < kTwoSec; ++i) stepBuf[(size_t)i] = 0.5f;

        // leg (c): 0.25 with a 480-frame 0.5 burst centred on every 4800th
        // frame, and a transient 2400 frames before each burst. The arithmetic
        // is derived in the leg.
        std::vector<f32> burstBuf((size_t)kTwoSec, 0.25f);
        std::vector<i64> grid;
        for (i64 k = 1; k * 4800 < kTwoSec; ++k) {
            const i64 p = k * 4800;
            for (i64 i = p - 240; i < p + 240; ++i) burstBuf[(size_t)i] = 0.5f;
            grid.push_back(p - 2400);
        }

        // Two clip envelopes and two warp maps, so the retirement leg has a
        // DISPLACED pointer to name. envA is the one leg (a) measures.
        RtAutoPoint ptsA[2], ptsB[2];
        ptsA[0].beat = 0.0; ptsA[0].value = 0.25f;
        ptsA[1].beat = 64.0; ptsA[1].value = 0.25f;      // held flat: a constant 0.25
        ptsB[0].beat = 0.0; ptsB[0].value = 0.75f;
        ptsB[1].beat = 64.0; ptsB[1].value = 0.75f;
        auto makeSet = [](RtAutoSet& s, RtAutoPoint* p) {
            s.points = p; s.pointCount = 2; s.laneCount = 1;
            s.lanes[0].target  = (i32)AutoTarget::TrackVol;
            s.lanes[0].xform   = (i32)AutoXform::Direct;   // the value IS the gain
            s.lanes[0].devSlot = -1;                       // an engine scalar
            s.lanes[0].index   = 0;
            s.lanes[0].first   = 0;
            s.lanes[0].count   = 2;
            s.lanes[0].lo = 0.f; s.lanes[0].hi = 1.f;
        };
        RtAutoSet envA, envB;
        makeSet(envA, ptsA);
        makeSet(envB, ptsB);

        WarpMarker mapA[2], mapB[2];
        mapA[0].srcFrame = 0;       mapA[0].beat = 0.0;
        mapA[1].srcFrame = kTwoSec; mapA[1].beat = 10.0;   // 96000 frames over 10 beats
        mapB[0].srcFrame = 0;       mapB[0].beat = 0.0;
        mapB[1].srcFrame = kTwoSec; mapB[1].beat = 20.0;

        // --- 1. THE REGRESSION GATE: all three payloads, and NO REFUSAL -----
        RtClip loaded;
        loaded.data        = burstBuf.data();
        loaded.frames      = kTwoSec;
        loaded.channels    = 1;
        loaded.loopStart   = 0;
        loaded.loopEnd     = kTwoSec;
        loaded.warp        = (int)Warp::Beats;
        loaded.loop        = true;
        loaded.quantumIdx  = 0;
        loaded.clipBpm     = kTempo;
        loaded.lengthBeats = 10.0;
        loaded.gain        = 1.0f;
        loaded.valid       = true;
        loaded.autos       = &envA;
        loaded.markers     = mapA;
        loaded.markerCount = 2;
        loaded.transients  = grid.data();
        loaded.transientCount = (int)grid.size();

        CHECK(publish(loaded),
              "a clip carrying a sample, an envelope, a warp map AND a transient "
              "grid is accepted by the handle");
        CHECK(eng.remoteRefusals() == refusals0,
              "AND NOTHING WAS REFUSED: %llu -> %llu. Before v11 this exact push "
              "counted one — and since the loader builds a grid for every audio "
              "sample, so did every clip of every audio set",
              (unsigned long long)refusals0, (unsigned long long)eng.remoteRefusals());

        // FOUR blocks for one clip, and naming them is what makes the count at
        // the end of the section mean something: the sample, the transient grid,
        // the envelope blob and the marker blob. Two of those four are the
        // payloads that had no wire before v11.
        const u64 blocks1 = eng.poolBlocksLive();
        CHECK(blocks1 == blocks0 + 4,
              "and it cost FOUR pool blocks — sample, grid, envelope, map: %llu "
              "-> %llu", (unsigned long long)blocks0, (unsigned long long)blocks1);

        eng.send(Cmd::LaunchClip, kTrk, kSlot);
        bool cellLive = false;
        for (int i = 0; i < 300 && !cellLive; ++i) {
            pump();
            cellLive = es.activeSlot[kTrk] == kSlot;
            sleepMs(10);
        }
        CHECK(cellLive,
              "and the DAEMON took the cell rather than answering RejectBadClip: "
              "track %d reports slot %d active (%d)", kTrk, kSlot, es.activeSlot[kTrk]);

        // --- 2. the retirement stand-in, for two more pointers --------------
        //
        // Nothing crossed: the envelope was COPIED into a pool blob and the map
        // into another, so the engine never holds either GUI-heap object and can
        // never hand one back. The handle has to, and App's free bookkeeping is
        // the only reader — exactly the Ev::NotesRetired argument at the top of
        // this file, twice more. (The transient grid deliberately has no event:
        // it belongs to the SampleBuffer, not to the clip, and outlives every
        // clip over it — engine.h says so and the daemon's retirement agrees.)
        RtClip swapped = loaded;
        swapped.autos   = &envB;
        swapped.markers = mapB;
        sawAutosRetired = sawWarpRetired = nullptr;
        CHECK(publish(swapped), "replace it with a DIFFERENT envelope and a "
              "DIFFERENT warp map");
        for (int i = 0; i < 200 && !(sawAutosRetired && sawWarpRetired); ++i) {
            pump();
            sleepMs(5);
        }
        CHECK(sawAutosRetired == (void*)&envA,
              "Ev::AutosRetired came home for the DISPLACED set (%p, wanted %p) — "
              "the set was COPIED into a blob, so the engine never held it and "
              "could never announce it",
              sawAutosRetired, (void*)&envA);
        CHECK(sawWarpRetired == (void*)mapA,
              "Ev::WarpRetired came home for the DISPLACED map (%p, wanted %p)",
              sawWarpRetired, (void*)mapA);

        // --- 3a. THE ENVELOPE: one TrackVol lane, held at 0.25 --------------
        //
        // A DC 0.5 clip through a constant 0.25 lane is 0.125 on the master, and
        // 0.5 without the lane. Warp::Off, so nothing but the envelope is in
        // play: no rate, no grains, no map.
        RtClip envClip;
        envClip.data        = dcHalf.data();
        envClip.frames      = kOneSec;
        envClip.channels    = 1;
        envClip.loopStart   = 0;
        envClip.loopEnd     = kOneSec;
        envClip.warp        = (int)Warp::Off;
        envClip.loop        = true;
        envClip.quantumIdx  = 0;
        envClip.clipBpm     = kTempo;
        envClip.lengthBeats = 5.0;
        envClip.gain        = 1.0f;
        envClip.valid       = true;
        envClip.autos       = &envA;

        relaunch(envClip);
        const f32 envDaemon = daemonPeak(700, 1200);
        renderLocal(*localEngine, envClip, kTempo, 48000, localFrames);
        const f32 envLocal = peakFrom(localFrames, 4800);
        CHECK(envDaemon > 0.10f && envDaemon < 0.15f,
              "the daemon plays the clip envelope: master peak %.4f, and 0.5 DC "
              "through a lane held at 0.25 is 0.125", (double)envDaemon);
        CHECK(std::fabs(envDaemon - envLocal) < 0.01f,
              "AND THE TWO AGREE: daemon %.4f vs in-process %.4f — the same "
              "RtAutoSet, one of them through the pool",
              (double)envDaemon, (double)envLocal);

        RtClip envOff = envClip;
        envOff.autos = nullptr;
        relaunch(envOff);
        const f32 envNone = daemonPeak(700, 1200);
        CHECK(envNone > 0.4f && envNone > envDaemon * 3.f,
              "and WITHOUT the envelope the same clip meters %.4f instead of "
              "%.4f — the payload is audible, not merely carried",
              (double)envNone, (double)envDaemon);

        // --- 4. bit-identity, where bit-identity is achievable --------------
        //
        // Not across the boundary (see the section head) — here, twice, in this
        // process. If the offline render were not a pure function of the clip,
        // every "daemon vs in-process" number above would be a comparison
        // against a moving reference and would mean nothing.
        std::vector<f32> again;
        renderLocal(*localEngine, envClip, kTempo, 48000, again);
        CHECK(again.size() == localFrames.size() &&
              std::memcmp(again.data(), localFrames.data(),
                          localFrames.size() * sizeof(f32)) == 0,
              "the in-process render is BIT-IDENTICAL run to run (%zu frames)",
              localFrames.size());

        // --- 3b. THE WARP MAP: the same clip, moved -------------------------
        //
        // The sample is a second of 0.25 followed by a second of 0.5, looped
        // whole. WITHOUT markers the clip's own tempo is 200x the session's, so
        // the flat rate is 0.005 and the read head crawls: after a minute it has
        // not left the quiet half, and the grain heads (which read at natural
        // speed from an origin that is still down there) have not either. WITH a
        // two-marker map pinning the whole sample onto ten beats, the local rate
        // is 1.0, the loop takes two seconds, and the loud half is played. Same
        // buffer, same loop points, same everything else: the MAP moved the
        // audio, and the meter says by how much.
        //
        // A marked clip is granular in Beats mode whatever its slope (engine.cpp
        // is explicit about why: some segment will have slope one, and switching
        // the stretcher on and off at that crossing would click). At slope 1 the
        // grain advance equals the hop, so both read heads sit on the same frame
        // and the overlap-add is transparent — which is why the marked variant
        // measures the material's own 0.5 and not some window's fraction of it.
        RtClip warpClip;
        warpClip.data        = stepBuf.data();
        warpClip.frames      = kTwoSec;
        warpClip.channels    = 1;
        warpClip.loopStart   = 0;
        warpClip.loopEnd     = kTwoSec;
        warpClip.warp        = (int)Warp::Beats;
        warpClip.loop        = true;
        warpClip.quantumIdx  = 0;
        warpClip.clipBpm     = kTempo * 200.0;
        warpClip.lengthBeats = 10.0;
        warpClip.gain        = 1.0f;
        warpClip.valid       = true;

        relaunch(warpClip);
        const f32 warpNone = daemonPeak(700, 1500);
        CHECK(warpNone > 0.2f && warpNone < 0.3f,
              "unmarked, the clip is stuck in its quiet half: %.4f (~0.25)",
              (double)warpNone);

        RtClip warpOn = warpClip;
        warpOn.markers     = mapA;
        warpOn.markerCount = 2;
        relaunch(warpOn);
        const f32 warpDaemon = daemonPeak(900, 2500);
        renderLocal(*localEngine, warpOn, kTempo, 48000 * 4, localFrames);
        const f32 warpLocal = peakFrom(localFrames, 4800);
        CHECK(warpDaemon > 0.45f && warpDaemon > warpNone * 1.5f,
              "THE MAP MOVED THE AUDIO: %.4f with the two markers, %.4f without "
              "them, off the same buffer and the same loop window",
              (double)warpDaemon, (double)warpNone);
        CHECK(std::fabs(warpDaemon - warpLocal) < 0.01f,
              "AND THE TWO AGREE: daemon %.4f vs in-process %.4f",
              (double)warpDaemon, (double)warpLocal);

        // --- 3c. THE TRANSIENT GRID: where the grains start -----------------
        //
        // Read from engine.cpp rather than guessed, because every number here is
        // one of its:
        //
        //   * the grain hop is a sixteenth of the CURRENT tempo in OUTPUT
        //     frames — 2400 at 300 BPM (warpHop);
        //   * a clip whose tempo is half the session's is granular at rate 2, so
        //     grain origins are 4800 SOURCE frames apart (adv = rate * hop);
        //   * the ideal origin at each boundary is the voice's srcPos, i.e.
        //     4800k, and the snap window is min(adv/2, 0.05 * sr) = min(2400,
        //     2400) = 2400 — which at this tempo is a whole hop, and that is the
        //     only reason a grid can do anything audible at all here;
        //   * the two read heads run at natural speed and the crossfade is a
        //     raised cosine over the hop, so a source frame `d` past its grain's
        //     origin is played at weight w(d/hop) by the incoming head and a
        //     frame a hop further on at 1 - w by the outgoing one. The weight is
        //     therefore ZERO at each origin and ONE a hop past it.
        //
        // So a burst sitting exactly ON the ideal origins is windowed out, and a
        // grid that pulls every origin back by 2400 puts those same bursts a
        // full hop past an origin, at weight one. WITHOUT the grid the meter
        // reads the 0.25 floor plus the shoulder of the window (~0.256); WITH
        // it, the burst's own 0.5. The grid did not change one sample of the
        // material — it changed where the grains BEGIN.
        RtClip trClip;
        trClip.data        = burstBuf.data();
        trClip.frames      = kTwoSec;
        trClip.channels    = 1;
        trClip.loopStart   = 0;
        trClip.loopEnd     = kTwoSec;
        trClip.warp        = (int)Warp::Beats;
        trClip.loop        = true;
        trClip.quantumIdx  = 0;
        trClip.clipBpm     = kTempo * 0.5;          // rate 2: granular, adv 4800
        trClip.lengthBeats = 10.0;
        trClip.gain        = 1.0f;
        trClip.valid       = true;

        relaunch(trClip);
        const f32 trNone = daemonPeak(700, 1500);
        CHECK(trNone > 0.2f && trNone < 0.32f,
              "with no grid the grains open ON the bursts and window them out: "
              "%.4f (the 0.25 floor, plus the shoulder of the raised cosine)",
              (double)trNone);

        RtClip trOn = trClip;
        trOn.transients     = grid.data();
        trOn.transientCount = (int)grid.size();
        relaunch(trOn);
        const f32 trDaemon = daemonPeak(700, 1500);
        renderLocal(*localEngine, trOn, kTempo, 48000 * 3, localFrames);
        const f32 trLocal = peakFrom(localFrames, 9600);
        CHECK(trDaemon > 0.45f && trDaemon > trNone * 1.5f,
              "THE GRID SNAPPED THE GRAINS INTO THE BURSTS: %.4f with %d "
              "transients, %.4f without them, off the same %lld frames",
              (double)trDaemon, (int)grid.size(), (double)trNone, (long long)kTwoSec);
        CHECK(std::fabs(trDaemon - trLocal) < 0.01f,
              "AND THE TWO AGREE: daemon %.4f vs in-process %.4f — grain "
              "scheduling in another process, off a grid this one wrote",
              (double)trDaemon, (double)trLocal);

        // --- 5. nothing was refused over the whole of it -------------------
        CHECK(eng.remoteRefusals() == refusals0,
              "and over the whole section, with three payloads published nine "
              "times, remoteRefusals() never moved (%llu)",
              (unsigned long long)eng.remoteRefusals());

        // --- 6. the displaced blocks come home ------------------------------
        //
        // This section published NINE clips into one cell, with four different
        // envelope sets and four different warp maps between them. Every one of
        // those displaced a blob, and a displaced blob is NOT freed when the
        // handle drops its reference (pool.h: Live -> Retiring, never straight
        // to free). It is freed when the daemon echoes the offset back, and the
        // daemon only echoes once the displacing command has been through
        // Engine::pushCommand AND the audio thread has run drainCommands()
        // since — i.e. once no voice can reach the old bytes. So this is a WAIT
        // and not a read: the count comes down when the proof arrives, not when
        // the GUI decides it should.
        //
        // What it comes down TO is stated exactly rather than as an inequality,
        // because an inequality here would pass against a leak of any size the
        // slack allowed. Six: the three sample buffers this section decoded, the
        // one transient grid, and ONE envelope and ONE map — the last of each,
        // still cached against the cell. The samples and the grid are keyed by
        // ADDRESS and are meant to outlive the cell exactly as App's
        // SampleBuffer does; the other six blobs are gone.
        const u64 blocksPeak = eng.poolBlocksLive();
        Command clr;
        clr.type = Cmd::ClearClip; clr.a = kTrk; clr.b = kSlot;
        bool clearedCell = false;
        for (int i = 0; i < 300 && !clearedCell; ++i) {
            clearedCell = eng.pushCommand(clr);
            if (!clearedCell) { pump(); sleepMs(5); }
        }
        CHECK(clearedCell, "the cell clears");
        u64 blocksNow = eng.poolBlocksLive();
        for (int i = 0; i < 600 && blocksNow != blocks0 + 6; ++i) {
            pump();
            sleepMs(10);
            blocksNow = eng.poolBlocksLive();
        }
        CHECK(blocksNow == blocks0 + 6,
              "and the pool holds exactly what this section still owns and not "
              "one block more: %llu, from %llu at the start and %llu with the "
              "cell still published — three samples, one grid, one envelope, "
              "one map. The other six blobs went home on the daemon's echo",
              (unsigned long long)blocksNow, (unsigned long long)blocks0,
              (unsigned long long)blocksPeak);

        // --- put the set back the way this section found it ----------------
        eng.send(Cmd::SetTempo, 0, 0, 140.0);
        eng.send(Cmd::LaunchClip, 0, 0);
        for (int i = 0; i < 60; ++i) { pump(); sleepMs(10); }
        const f32 restored = peakOver(eng, es, 120);
        eng.send(Cmd::SetPlaying, 0);
        eng.send(Cmd::Locate, 0, 0, 0.0);
        CHECK(restored > 0.4f,
              "the session clip is back on track 0 for the sections that follow: "
              "%.4f", (double)restored);
    }

    // =====================================================================
    // STEP 6: the link state, and a restart that puts the set back
    // =====================================================================
    banner("step 6: lifecycle");

    eng.poll(es);
    CHECK(eng.link() == EngineLink::Live && es.link == EngineLink::Live,
          "the link reads Live and the snapshot carries it");
    CHECK(engineLinkBanner(EngineLink::Live) == nullptr,
          "a live engine draws no banner");
    CHECK(engineLinkBanner(EngineLink::Lost) != nullptr &&
          engineLinkOffersRestart(EngineLink::Lost) &&
          !engineLinkOffersRestart(EngineLink::Starting),
          "a lost one draws '%s' and offers a restart; a starting one does not",
          engineLinkBanner(EngineLink::Lost));

    // Put the chain back so the restart has something to rebuild, and turn the
    // master down so the replay has something to prove.
    CHECK(eng.pushCommand(chain), "the saturator chain is published again");
    CHECK(eng.send(Cmd::MasterVol, 0, 0, 0.5), "and the master fader is moved to 0.5");
    for (int i = 0; i < 200 && !(eng.remoteDevice(&sat) && eng.remoteDevice(&sat)->live); ++i) {
        eng.poll(es);
        while (eng.popEvent(e)) {}
        sleepMs(10);
    }
    CHECK(eng.remoteDevice(&sat) && eng.remoteDevice(&sat)->live, "and it is live again");
    const u32 oldId  = eng.remoteDevice(&sat)->id;
    const i32 oldPid = eng.enginePid();
    CHECK(oldPid > 0, "the engine's pid is reachable (%d)", oldPid);

    // --- WEDGED IS NOT DEAD, and this is the distinction §4.4 turns on -----
    //
    // SIGSTOP is what a laptop resuming from suspend and a JACK restart both
    // look like: the process is there, it is simply not publishing. The rule
    // the UI is most likely to violate is respawning on this. So the handle has
    // to be able to SEE it — Stale, with a silence it can put a number on — and
    // must do nothing about it.
    ::kill(oldPid, SIGSTOP);
    for (int i = 0; i < 200 && eng.link() != EngineLink::Stale; ++i) {
        eng.poll(es);
        while (eng.popEvent(e)) {}
        sleepMs(10);
    }
    eng.poll(es);
    CHECK(es.link == EngineLink::Stale,
          "a SIGSTOPped engine reads Stale, not Lost: the process is alive and "
          "a respawn under it would be the worst available outcome (§4.4)");
    CHECK(es.linkSilentMs > 300,
          "and the silence is measured, not guessed: %u ms", es.linkSilentMs);
    CHECK(eng.resyncs() == 0, "nothing restarted itself");
    ::kill(oldPid, SIGCONT);
    for (int i = 0; i < 200 && eng.link() != EngineLink::Live; ++i) {
        eng.poll(es);
        while (eng.popEvent(e)) {}
        sleepMs(10);
    }
    eng.poll(es);
    CHECK(es.link == EngineLink::Live && es.linkSilentMs == 0,
          "and it comes back on its own when the engine does");

    // --- DEAD, and §6's recovery ------------------------------------------
    //
    // SIGKILL leaves an orphaned control region and a live sample pool, which
    // is exactly the state §4.3 designed for: the samples outlive the engine so
    // a replacement can adopt them.
    ::kill(oldPid, SIGKILL);
    for (int i = 0; i < 300 && eng.link() != EngineLink::Lost; ++i) {
        eng.poll(es);
        while (eng.popEvent(e)) {}
        sleepMs(10);
    }
    eng.poll(es);
    CHECK(es.link == EngineLink::Lost,
          "a killed engine reads Lost — the creator pid is gone, checked with "
          "its start time and never the pid alone");
    CHECK(engineLinkBanner(es.link) != nullptr && engineLinkOffersRestart(es.link),
          "which draws '%s' and offers a restart", engineLinkBanner(es.link));

    // §6's recovery. Device ids do not survive an engine, the pool does, and
    // the transport comes back stopped.
    CHECK(eng.restartEngine(), "restartEngine() reaped, respawned and re-attached");
    CHECK(eng.enginePid() > 0 && eng.enginePid() != oldPid,
          "it is a different process (%d, was %d)", eng.enginePid(), oldPid);
    CHECK(eng.resyncs() == 1, "one resync (%llu)", (unsigned long long)eng.resyncs());
    CHECK(eng.link() == EngineLink::Starting || eng.link() == EngineLink::Live,
          "the link is up again");

    const RemoteDevice* rd2 = nullptr;
    for (int i = 0; i < 1200 && !(rd2 && rd2->live); ++i) {
        eng.poll(es);
        while (eng.popEvent(e)) {}
        rd2 = eng.remoteDevice(&sat);
        sleepMs(10);
    }
    CHECK(rd2 && rd2->live, "the chain was rebuilt against the new engine: device %u",
          rd2 ? rd2->id : 0u);
    // The id is the NEW engine's. Nothing here asserts what it is, on purpose:
    // §11.4's point is that ids do not survive, so a client that expected a
    // particular one would be encoding the thing that is not true.
    CHECK(rd2 && rd2->live, "with an id issued by the engine that is running now "
          "(was %u, is %u)", oldId, rd2 ? rd2->id : 0u);
    CHECK(!es.playing, "and the transport came back STOPPED, per §4.4's honest default");

    // The clip table survived as a memcpy — no decode, no offset change — and
    // the master fader came back off the scalar shadow, which is the only
    // record of it on this path. Launch it again (the transport is stopped and
    // the launch was deliberately NOT replayed) and listen.
    CHECK(eng.send(Cmd::SetPlaying, 1), "start the transport again");
    CHECK(eng.send(Cmd::LaunchClip, 0, 0), "and relaunch the clip");
    const f32 rePeak = peakOver(eng, es, 250);
    CHECK(rePeak > 0.15f,
          "the republished clip sounds again with no decode: %.4f", (double)rePeak);
    CHECK(rePeak < dryPeak * 0.85f,
          "and at the 0.5 master the scalar shadow replayed: %.4f vs the 1.0 "
          "master's %.4f — a respawned engine is told the mixer again, because "
          "App has no idea one was replaced", (double)rePeak, (double)dryPeak);

    // --- refusals are counted, not silent ----------------------------------
    //
    // Cmd::SetSignatures used to be the archetype here and no longer is (the map
    // crosses as a pool blob now), and Cmd::SetArrangement used to be and no
    // longer is either. What is left in the permanent class is a command aimed
    // at something that does not exist: it can never be carried, so it must be
    // consumed and counted rather than answered false — a permanent false wedges
    // App's retry FIFO and with it the transport (§11.2).
    const u64 before = eng.remoteRefusals();
    Command nowhere;
    nowhere.type = Cmd::SetTrackAutos; nowhere.a = kMaxTracks + 99; nowhere.p = nullptr;
    CHECK(eng.pushCommand(nowhere),
          "a command aimed at a lane that does not exist is CONSUMED, not "
          "answered false");
    CHECK(eng.remoteRefusals() >= before + 1,
          "and counted (%llu -> %llu)",
          (unsigned long long)before, (unsigned long long)eng.remoteRefusals());
    CHECK(eng.snapshotTears() == 0, "no snapshot failed the seqlock (%llu)",
          (unsigned long long)eng.snapshotTears());

    // --- close --------------------------------------------------------------
    eng.close();
    sleepMs(500);
    CHECK(countShm(session) == 0,
          "close() stopped the daemon it spawned and unlinked both regions "
          "(%d left in /dev/shm)", countShm(session));

    testRecording(session);

    // =====================================================================
    // The other two backings, so the new accessors are not daemon-only
    // =====================================================================
    banner("the local and the degraded backings");
    {
        // §8's exception: a handle that opened NOTHING is a supported state,
        // not an error. The GUI still loads, edits and saves; every send() is a
        // no-op and the banner says why.
        EngineHandle none;
        EngineState nes;
        none.poll(nes);
        CHECK(none.link() == EngineLink::Detached && nes.link == EngineLink::Detached,
              "an unopened handle is Detached in both the accessor and the snapshot");
        CHECK(engineLinkBanner(nes.link) != nullptr,
              "which draws '%s'", engineLinkBanner(nes.link));
        CHECK(!none.send(Cmd::SetPlaying, 1), "and every send() is a no-op");
        CHECK(none.remoteDevice(nullptr) == nullptr && none.catalog().empty() &&
              !none.restartEngine(),
              "with no devices, no catalog and nothing to restart");
    }
    {
        // "null" matches neither backend name, so createBackend() returns
        // nothing and openLocalEngine() prepares the engine silent — which is
        // the whole of what this needs: an in-process Engine to be Live about.
        EngineHandle loc;
        EngineState les;
        CHECK(loc.openLocalEngine("null"), "openLocalEngine() opened an in-process engine");
        loc.poll(les);
        CHECK(loc.local() != nullptr && loc.link() == EngineLink::Live &&
              les.link == EngineLink::Live,
              "local mode is Live: an in-process engine cannot be stale (it is "
              "this process) and cannot be lost (it dies with us)");
        CHECK(les.devicesPending == 0 && loc.devicesPending() == 0,
              "nothing is ever pending locally — instantiation is synchronous there");
        CHECK(loc.catalog().empty() && !loc.catalogReady() && !loc.requestScan(),
              "and the catalog is empty on purpose: App's own PluginRegistry IS "
              "the local engine's registry, so a second copy would be two things "
              "to keep in step for no gain");
        CHECK(!loc.restartEngine(), "restartEngine() is a daemon-only idea");
        loc.close();
    }

    // =====================================================================
    // The selection matrix — the daemon is the default (§16)
    // =====================================================================
    // Every row of GUI-ON-DAEMON.md §16's table, pinned. The env is the whole
    // input to open()'s dispatch, so each block sets exactly the row it means
    // and uses a fresh session name so nothing attaches to a leftover.
    banner("the selection matrix: the daemon is the default (§16)");
    {
        // --- unset -> daemon, when one can be spawned ----------------------
        // THE FLIP LINE. Reverting open()'s default (the pre-flip dispatch:
        // "anything but =daemon gives the in-process engine") turns exactly
        // the second CHECK red: an env-free open() then builds an Engine in
        // this process, local() stops answering null and remoteOpen() reads
        // false. Proven red once by file-copy revert; see §16.
        char dsess[64];
        std::snprintf(dsess, sizeof dsess, "htest-def-%d", (int)::getpid());
        ::unsetenv("NXTAKT_ENGINE");
        ::unsetenv("LATTICE_ENGINE");
        ::setenv("NXTAKT_SESSION", dsess, 1);
        EngineHandle def;
        CHECK(def.open("null"), "open() with NXTAKT_ENGINE unset opened something");
        CHECK(def.remoteOpen() && def.local() == nullptr,
              "and it is the DAEMON: the unnamed default spawns nxtaktd "
              "(remote %d, local %p)", (int)def.remoteOpen(), (void*)def.local());
        CHECK(def.enginePid() > 0, "a real one (pid %d)", def.enginePid());
        def.close();
        sleepMs(400);
        CHECK(countShm(dsess) == 0,
              "close() stopped the daemon the default spawned (%d regions left)",
              countShm(dsess));

        // --- unset + no spawnable daemon -> NO engine, not a local one -----
        // §8(4)'s degraded mode, kept deliberately over a fallback: this
        // branch cannot tell a missing binary from a live daemon it merely
        // could not attach (§16), and §8(3) deletes the path a fallback would
        // land on one release after the flip.
        char nsess[64];
        std::snprintf(nsess, sizeof nsess, "htest-none-%d", (int)::getpid());
        ::setenv("NXTAKT_SESSION", nsess, 1);
        ::setenv("NXTAKT_DAEMON", "/nonexistent/flp-no-such-nxtaktd", 1);
        EngineHandle none2;
        EngineState ns;
        CHECK(none2.open("null"),
              "open() with the default unreachable still opens (degraded mode)");
        CHECK(!none2.remoteOpen() && !none2.localOpen() && none2.local() == nullptr,
              "with NO engine of either kind: the default does NOT fall back to "
              "an in-process engine");
        none2.poll(ns);
        CHECK(none2.link() == EngineLink::Detached && ns.link == EngineLink::Detached,
              "the link is Detached in the accessor and the snapshot");
        CHECK(engineLinkBanner(ns.link) != nullptr && engineLinkOffersRestart(ns.link),
              "so the chrome draws '%s' and offers Restart", engineLinkBanner(ns.link));
        CHECK(!none2.send(Cmd::SetPlaying, 1), "and every send() is a no-op");

        // --- the Restart button is the recovery path (§16) -----------------
        // The binary appears (an install is fixed, a build finishes) and the
        // click the Detached banner already offers retries the first spawn —
        // no relaunch. While it is still absent, the same click fails
        // honestly rather than pretending.
        CHECK(!none2.restartEngine(),
              "restartEngine() while the binary is still absent fails honestly");
        ::setenv("NXTAKT_DAEMON", "build/nxtaktd", 1);
        CHECK(none2.restartEngine(),
              "and succeeds from Detached once it exists: the banner's Restart "
              "recovers a failed startup");
        CHECK(none2.remoteOpen() && none2.enginePid() > 0,
              "with a real daemon behind it (pid %d)", none2.enginePid());
        bool liveAgain = false;
        for (int i = 0; i < 200 && !liveAgain; ++i) {
            none2.poll(ns);
            if (ns.link == EngineLink::Live) liveAgain = true;
            sleepMs(10);
        }
        CHECK(liveAgain, "and the link comes back Live");
        none2.close();
        sleepMs(400);
        CHECK(countShm(nsess) == 0,
              "close() stopped that daemon too (%d regions left)", countShm(nsess));

        // --- explicit =daemon + no spawnable daemon -> §8's no-fallback ----
        // Same observable state as the default row above, on purpose: one
        // failure policy, not two (§16). What the explicitness changes is
        // only the log line.
        char esess[64];
        std::snprintf(esess, sizeof esess, "htest-exp-%d", (int)::getpid());
        ::setenv("NXTAKT_ENGINE", "daemon", 1);
        ::setenv("NXTAKT_SESSION", esess, 1);
        ::setenv("NXTAKT_DAEMON", "/nonexistent/flp-no-such-nxtaktd", 1);
        EngineHandle exp;
        CHECK(exp.open("null") && !exp.remoteOpen() && !exp.localOpen(),
              "NXTAKT_ENGINE=daemon with no reachable daemon opens engine-free: "
              "no silent local fallback when the daemon was asked for by name");
        CHECK(exp.link() == EngineLink::Detached, "and is Detached");
        exp.close();
        ::setenv("NXTAKT_DAEMON", "build/nxtaktd", 1);

        // --- explicit =local (and the doc's =inproc) -> in-process ---------
        ::setenv("NXTAKT_ENGINE", "local", 1);
        EngineHandle l1;
        CHECK(l1.open("null") && l1.localOpen() && l1.local() != nullptr &&
              !l1.remoteOpen(),
              "NXTAKT_ENGINE=local opens the in-process engine");
        CHECK(l1.link() == EngineLink::Live, "which is Live");
        l1.close();
        ::setenv("NXTAKT_ENGINE", "inproc", 1);
        EngineHandle l2;
        CHECK(l2.open("null") && l2.localOpen() && !l2.remoteOpen(),
              "and =inproc, the ship plan's older spelling, still means the same");
        l2.close();

        // --- an unrecognised value -> warned, then the default -------------
        // A typo'd mode must not quietly select a DIFFERENT engine than the
        // default would have (§4.4): it is logged and the default (daemon)
        // is taken.
        char tsess[64];
        std::snprintf(tsess, sizeof tsess, "htest-typo-%d", (int)::getpid());
        ::setenv("NXTAKT_ENGINE", "sideways", 1);
        ::setenv("NXTAKT_SESSION", tsess, 1);
        EngineHandle typo;
        CHECK(typo.open("null") && typo.remoteOpen() && !typo.localOpen(),
              "NXTAKT_ENGINE=sideways is not a mode: warned, and the default "
              "(the daemon) is what opens");
        typo.close();
        sleepMs(400);
        CHECK(countShm(tsess) == 0,
              "and its daemon is stopped with it (%d regions left)", countShm(tsess));

        // The rows that follow in this file ran first: unset/absent binary is
        // above, =daemon with a live daemon is the whole top of main(). Put
        // the env back the way the top of main() left it all the same.
        ::setenv("NXTAKT_ENGINE", "daemon", 1);
        ::setenv("NXTAKT_SESSION", session, 1);
    }

    // =====================================================================
    // The record journal crosses the wire (§5 under the default engine)
    // =====================================================================
    // The daemon has always FORWARDED the journal (journalForwarded counts
    // it); this pins the half that was missing: a consumer. eng.popJournal()
    // must deliver the engine's entries in remote mode -- delete the remote
    // arm of EngineHandle::popJournal and every CHECK below goes red, which
    // is this fix's removal test. Before the fix, arrangement recording under
    // the DEFAULT engine drained nothing and committed nothing.
    banner("the record journal crosses the wire (§5: the default engine records)");
    {
        char jsess[64];
        std::snprintf(jsess, sizeof jsess, "htest-jrn-%d", (int)::getpid());
        ::unsetenv("NXTAKT_ENGINE");
        ::setenv("NXTAKT_SESSION", jsess, 1);
        EngineHandle jd;
        EngineState js;
        CHECK(jd.open("null") && jd.remoteOpen(),
              "an env-free open(): the default engine, a spawned daemon");

        ArrJournal j{};
        CHECK(!jd.popJournal(j), "the ring is empty before the transport moves");

        // SetPlaying 1 runs armTransport() in the ENGINE, whose journal gains
        // a TakeStart stamped with the engine's own beat. Two hops later it
        // must come out of the handle.
        CHECK(jd.send(Cmd::SetPlaying, 1), "start the transport");
        bool got = false;
        for (int i = 0; i < 300 && !(got = jd.popJournal(j)); ++i) sleepMs(10);
        CHECK(got, "an entry crosses engine ring -> daemon pump -> wire ring -> handle");
        CHECK(j.kind == (u32)JournalKind::TakeStart,
              "and it is the pass's opening entry (kind %u)", j.kind);
        const u32 seq0 = j.seq;

        // A locate writes the next entry; §5.4's contiguity check runs on the
        // ENGINE's seq, so the wire must deliver it unrenumbered.
        CHECK(jd.send(Cmd::Locate, 0, 0, 8.0), "locate while playing");
        ArrJournal j2{};
        bool got2 = false;
        for (int i = 0; i < 300; ++i) {
            while (jd.popJournal(j2)) {
                if (j2.kind == (u32)JournalKind::Locate) { got2 = true; break; }
            }
            if (got2) break;
            sleepMs(10);
        }
        CHECK(got2, "the locate's entry arrives too");
        CHECK(got2 && j2.seq > seq0,
              "with the ENGINE's own seq, still monotonic across the wire "
              "(%u after %u) -- the contiguity check §5.4 hangs a refusal on "
              "works unmodified in daemon mode", j2.seq, seq0);

        jd.close();
        ::setenv("NXTAKT_ENGINE", "daemon", 1);
        ::setenv("NXTAKT_SESSION", session, 1);
    }

    std::printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
