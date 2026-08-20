// Shared-memory IPC tests.
//
// Exercises src/ipc/shm.h across a real process boundary: the parent creates a
// region, fork()s, and the child attaches to it by name through shm_open. Two
// rings then run concurrently in opposite directions with sequence-numbered
// payloads, so a lost, duplicated or reordered message is a hard failure rather
// than a statistical one.
//
// Nothing here links the engine, the GUI or any audio library — the IPC layer
// depends on libc alone and the test keeps it that way.
//
//   g++ -std=c++20 -O2 -Wall -Wextra tests/ipc_test.cpp -o ipc_test -lrt -lpthread
#include "../src/ipc/shm.h"
#include "../src/ipc/pool.h"
#include "../src/ipc/client.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace lat;

// ---------------------------------------------------------------------------
// tiny check framework  (same shape as tests/engine_test.cpp)
// ---------------------------------------------------------------------------

static int gPass = 0, gFail = 0;
static const char* gTag = "";          // "" in the parent, "child " in the child

static void checkImpl(bool ok, int line, const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (ok) { ++gPass; std::printf("  %sPASS  %s\n", gTag, msg); }
    else    { ++gFail; std::printf("  %sFAIL  %s   (ipc_test.cpp:%d)\n", gTag, msg, line); }
    std::fflush(stdout);
}
#define CHECK(cond, ...) checkImpl((cond), __LINE__, __VA_ARGS__)

static void banner(const char* s) { std::printf("\n== %s\n", s); std::fflush(stdout); }
static void note(const char* fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    std::printf("  %snote  %s\n", gTag, msg);
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// wire types
// ---------------------------------------------------------------------------
//
// Deliberately *not* lat::Command / lat::Event: both of those carry a void* and
// RtClip carries a const f32*, which are meaningless in the peer's address
// space. These are the shapes the real protocol has to move to — fixed width,
// no pointers, a pool handle where a pointer used to be. See
// docs/PROCESS-SPLIT.md.

struct WireCmd {
    u32 type;
    i32 a, b;
    f64 x;
    u64 seq;        // strictly increasing, checked by the consumer
    u64 check;      // derived from seq: catches torn or stale slots
    u64 poolRef;    // stands in for the future sample-pool handle
};

struct WireEvt {
    u32 type;
    i32 a, b;
    f64 x;
    u64 seq;
    u64 check;
};

static_assert(std::is_trivially_copyable_v<WireCmd>);
static_assert(std::is_trivially_copyable_v<WireEvt>);

// Cheap avalanche so a one-bit error in seq cannot survive into check.
static inline u64 mix64(u64 v) {
    v ^= v >> 33; v *= 0xff51afd7ed558ccdull;
    v ^= v >> 33; v *= 0xc4ceb9fe1a85ec53ull;
    v ^= v >> 33;
    return v;
}

// Test-only status block: the child's verdict, readable by the parent without
// squeezing it through an exit code.
struct TestStatus {
    std::atomic<u64> cmdsSeen;
    std::atomic<u64> evtsSent;
    std::atomic<u64> badOrder;
    std::atomic<u64> badPayload;
    std::atomic<u32> childDone;
};

// ---------------------------------------------------------------------------
// region layout
// ---------------------------------------------------------------------------
//
// Both sides compute these offsets from the same constants and fold them into
// the layout hash, so a build that disagrees fails at attach() instead of
// reading a ring through the wrong offset.

using CmdRing = ipc::ShmSpscRing<WireCmd, 1024>;
using EvtRing = ipc::ShmSpscRing<WireEvt, 1024>;

namespace layout {
inline constexpr size_t kState  = 0;
inline constexpr size_t kStatus = ipc::alignUp(kState  + sizeof(ipc::SharedState), ipc::kCacheLine);
inline constexpr size_t kCmds   = ipc::alignUp(kStatus + sizeof(TestStatus),       ipc::kCacheLine);
inline constexpr size_t kEvts   = ipc::alignUp(kCmds   + CmdRing::bytes(),         ipc::kCacheLine);
inline constexpr size_t kBytes  = kEvts + EvtRing::bytes();

inline constexpr u32 kHash =
    ipc::hashMix(ipc::hashMix(ipc::hashMix(ipc::fnv1a("nxtakt.ipc_test.v1"),
                 (u64)kBytes), (u64)CmdRing::capacity()), (u64)sizeof(WireCmd));
}

// ---------------------------------------------------------------------------
// cleanup discipline
// ---------------------------------------------------------------------------
//
// The creator's destructor unlinks, but a test that dies on a signal or an
// early return must not leave anything in /dev/shm either — an orphan region
// would make the *next* run's create() take the stale-reap path and mask the
// bug. Only the creating process arms this; the child clears it immediately
// after fork so it can never unlink a name it does not own.

static char gShmName[64]     = {};
static char gShmNameAlt[64]  = {};
static bool gOwnsShm         = false;

static void cleanupShm() {
    if (!gOwnsShm) return;
    if (gShmName[0])    ipc::ShmRegion::forceUnlink(gShmName);
    if (gShmNameAlt[0]) ipc::ShmRegion::forceUnlink(gShmNameAlt);
}
static void fatalSignal(int sig) {
    cleanupShm();
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}
static void armCleanup() {
    gOwnsShm = true;
    std::atexit(cleanupShm);
    for (int s : {SIGINT, SIGTERM, SIGSEGV, SIGABRT, SIGBUS, SIGPIPE}) ::signal(s, fatalSignal);
}

// Regions THIS RUN created, and no others.
//
// It used to count every /dev/shm entry with "nxtakt" in the name, and that is
// a leak check with a false positive built into it: a developer's own daemon, a
// second suite running beside this one in CI, or another test process on the
// same machine all show up as "this run leaked". It has been observed failing
// for exactly that reason, on a clean tree, and a leak check that cries wolf is
// one that gets ignored the day it is right.
//
// Every name this file creates carries this process's pid (see main()), so the
// tag is both necessary and sufficient: nothing of ours can escape it and
// nothing of anybody else's can match it.
static char gRunTag[24] = {};

static int countNxTaktShm() {
    DIR* d = ::opendir("/dev/shm");
    if (!d) return -1;
    int n = 0;
    while (dirent* e = ::readdir(d))
        if (std::strstr(e->d_name, "nxtakt") && gRunTag[0] &&
            std::strstr(e->d_name, gRunTag)) {
            ++n;
            note("leftover /dev/shm/%s", e->d_name);
        }
    ::closedir(d);
    return n;
}

// Does a POSIX shm object by this name exist? Accepts both the "/name" and
// "name" spellings, matching how the region layer normalises them.
static bool shmExistsPool(const char* name) {
    const char* body = (name && *name == '/') ? name + 1 : name;
    char path[128];
    std::snprintf(path, sizeof path, "/dev/shm/%s", body ? body : "");
    return ::access(path, F_OK) == 0;
}

// ---------------------------------------------------------------------------
// 1. region create / attach / validate
// ---------------------------------------------------------------------------

static void testRegionBasics() {
    banner("1. region create, attach and header validation");

    // Note the shape used throughout: the call runs on its own line and only
    // then is its result checked. Folding it into CHECK()'s condition would
    // leave the order of the condition and the error()-reading argument
    // unspecified, and the message would report a stale error string.
    ipc::ShmRegion creator;
    const bool made = creator.create(gShmNameAlt, layout::kBytes, layout::kHash);
    CHECK(made, "create(%s, %zu bytes) -> %s", gShmNameAlt, layout::kBytes,
          made ? "ok" : creator.error());
    if (!made) return;

    CHECK(creator.isCreator(), "the creating region owns the unlink");
    CHECK(creator.payloadBytes() >= layout::kBytes,
          "payload is at least the requested %zu bytes (got %zu)",
          layout::kBytes, creator.payloadBytes());
    CHECK(creator.totalBytes() % 4096 == 0,
          "region is page-rounded (%zu bytes)", creator.totalBytes());

    // Not ready yet: an attacher must not see it.
    {
        ipc::ShmRegion early;
        const bool got = early.attach(gShmNameAlt, layout::kHash, ipc::kShmVersion, 0);
        CHECK(!got, "attach before publishReady() is refused (%s)", early.error());
    }

    auto* st = creator.at<ipc::SharedState>(layout::kState);
    CHECK(st != nullptr, "SharedState fits at offset %zu", layout::kState);
    if (st) st->init(48000.0, 256);
    creator.publishReady();

    // Same process, second independent mapping — still a real shm_open path.
    ipc::ShmRegion peer;
    const bool joined = peer.attach(gShmNameAlt, layout::kHash, ipc::kShmVersion, 1000);
    CHECK(joined, "attach after publishReady() succeeds%s%s",
          joined ? "" : ": ", joined ? "" : peer.error());
    CHECK(!peer.isCreator(), "an attacher never owns the unlink");
    CHECK(peer.totalBytes() == creator.totalBytes(),
          "attacher maps the same size (%zu vs %zu)", peer.totalBytes(), creator.totalBytes());

    if (peer.valid()) {
        auto* pst = peer.at<ipc::SharedState>(layout::kState);
        CHECK(pst && pst->sampleRate.load() == 48000.0,
              "attacher reads the state the creator published (%.0f Hz)",
              pst ? pst->sampleRate.load() : -1.0);
        CHECK(peer.header()->creatorPid == (i32)::getpid(),
              "header records the creator pid (%d)", peer.header()->creatorPid);
        CHECK(peer.header()->attached.load() == 1, "attach count is %u",
              peer.header()->attached.load());
        // Writes land on the other mapping: this is one region, not a copy.
        pst->tempo.store(137.5);
        CHECK(st && st->tempo.load() == 137.5,
              "a write through the attacher is visible to the creator (%.1f)",
              st ? st->tempo.load() : -1.0);

        // shm v4 (GUI-ON-DAEMON.md §1.2): the two fields the GUI polls every
        // frame that the block could not carry before. init() must zero them,
        // and they must cross like everything else in here.
        bool zeroed = pst->latencyFrames.load() == 0;
        for (int i = 0; i < ipc::kShmReturns; ++i)
            zeroed = zeroed && pst->returnMeterL[i].load() == 0.f &&
                     pst->returnMeterR[i].load() == 0.f;
        CHECK(zeroed, "init() zeroes latencyFrames and the %d return meters",
              ipc::kShmReturns);
        pst->latencyFrames.store(1536);
        pst->returnMeterL[ipc::kShmReturns - 1].store(0.75f);
        pst->returnMeterR[0].store(0.25f);
        CHECK(st && st->latencyFrames.load() == 1536 &&
                  st->returnMeterL[ipc::kShmReturns - 1].load() == 0.75f &&
                  st->returnMeterR[0].load() == 0.25f,
              "PDC latency and the return meters cross the mapping (%d frames)",
              st ? st->latencyFrames.load() : -1);
    }

    banner("2. mismatch handling");
    {
        ipc::ShmRegion bad;
        CHECK(!bad.attach(gShmNameAlt, layout::kHash, ipc::kShmVersion + 1, 200),
              "attaching with the wrong protocol version fails");
        CHECK(std::strstr(bad.error(), "version mismatch") != nullptr,
              "...and says why: %s", bad.error());
        CHECK(!bad.valid(), "the failed attacher holds no mapping");
    }
    {
        ipc::ShmRegion bad;
        CHECK(!bad.attach(gShmNameAlt, layout::kHash ^ 0xdeadbeefu, ipc::kShmVersion, 200),
              "attaching with the wrong layout hash fails");
        CHECK(std::strstr(bad.error(), "layout mismatch") != nullptr,
              "...and says why: %s", bad.error());
    }
    {
        ipc::ShmRegion bad;
        CHECK(!bad.attach("nxtakt-does-not-exist-xyz", layout::kHash, ipc::kShmVersion, 20),
              "attaching to a missing region times out cleanly (%s)", bad.error());
    }
    {
        ipc::ShmRegion bad;
        CHECK(!bad.create("has/slash", 4096, layout::kHash), "a malformed name is rejected");
        CHECK(!bad.create("", 4096, layout::kHash), "an empty name is rejected");
    }
    {
        // A live creator owns the name; a second create() must not steal it.
        ipc::ShmRegion squatter;
        CHECK(!squatter.create(gShmNameAlt, layout::kBytes, layout::kHash),
              "create() refuses a name a live process already owns (%s)", squatter.error());
        CHECK(peer.valid() && peer.at<ipc::SharedState>(layout::kState)->tempo.load() == 137.5,
              "...and the existing region is untouched");
    }
    CHECK(!ipc::ShmRegion::reapIfStale(gShmNameAlt),
          "reapIfStale() leaves a region whose creator is alive");

    // creator's destructor unlinks; peer's mapping stays valid until it goes.
}

// ---------------------------------------------------------------------------
// 3. backpressure
// ---------------------------------------------------------------------------

static void testBackpressure() {
    banner("3. full-ring backpressure");

    ipc::ShmRegion r;
    if (!r.create(gShmNameAlt, layout::kBytes, layout::kHash)) {
        CHECK(false, "create for backpressure test: %s", r.error());
        return;
    }
    CmdRing* ring = CmdRing::createAt(r, layout::kCmds);
    r.publishReady();
    if (!ring) { CHECK(false, "ring did not fit at offset %zu", layout::kCmds); return; }

    u32 accepted = 0, refused = 0;
    for (u32 i = 0; i < CmdRing::capacity() * 3; ++i) {
        WireCmd c{};
        c.type = 7; c.a = (i32)i; c.seq = i; c.check = mix64(i);
        if (ring->push(c)) ++accepted; else ++refused;
    }
    CHECK(accepted == CmdRing::capacity(),
          "exactly capacity (%u) pushes are accepted, then push() refuses (%u accepted, %u refused)",
          CmdRing::capacity(), accepted, refused);
    CHECK(refused > 0, "a full ring refuses instead of overwriting");
    CHECK(ring->size() == CmdRing::capacity(), "size() reports full (%u)", ring->size());

    // Everything that was accepted must come back intact and in order: the
    // refused pushes must not have clobbered a slot.
    bool order = true, payload = true;
    u32 got = 0;
    WireCmd out{};
    while (ring->pop(out)) {
        if (out.seq != got) order = false;
        if (out.check != mix64(out.seq) || out.a != (i32)out.seq) payload = false;
        ++got;
    }
    CHECK(got == accepted, "every accepted message pops back (%u of %u)", got, accepted);
    CHECK(order, "FIFO order survives saturation");
    CHECK(payload, "no slot was corrupted by the refused pushes");
    CHECK(ring->empty(), "the drained ring reports empty");

    // And it still works afterwards.
    WireCmd c{}; c.seq = 99; c.check = mix64(99); c.a = 99;
    CHECK(ring->push(c), "push() works again once the consumer has drained");
    CHECK(ring->pop(out) && out.seq == 99 && out.check == mix64(99),
          "...and the message round-trips");
}

// ---------------------------------------------------------------------------
// 4. cross-process exchange
// ---------------------------------------------------------------------------

static constexpr u64 kMessages = 100000;
static constexpr u64 kTimeoutNs = 30ull * 1000000000ull;   // generous: this is a liveness bound

static WireCmd mkCmd(u64 seq) {
    WireCmd c{};
    c.type    = (u32)(seq % 17);
    c.a       = (i32)(seq & 0x7fffffffu);
    c.b       = (i32)(seq % 32);
    c.x       = (f64)seq * 0.25;
    c.seq     = seq;
    c.check   = mix64(seq);
    c.poolRef = seq * 64;
    return c;
}
static bool cmdOk(const WireCmd& c, u64 seq) {
    return c.seq == seq && c.check == mix64(seq) && c.type == (u32)(seq % 17) &&
           c.a == (i32)(seq & 0x7fffffffu) && c.b == (i32)(seq % 32) &&
           c.x == (f64)seq * 0.25 && c.poolRef == seq * 64;
}
static WireEvt mkEvt(u64 seq) {
    WireEvt e{};
    e.type  = (u32)(seq % 6);
    e.a     = (i32)(seq % 32);
    e.b     = (i32)(seq % 8);
    e.x     = (f64)seq * 0.5;
    e.seq   = seq;
    e.check = mix64(seq ^ 0xa5a5a5a5ull);
    return e;
}
static bool evtOk(const WireEvt& e, u64 seq) {
    return e.seq == seq && e.check == mix64(seq ^ 0xa5a5a5a5ull) &&
           e.type == (u32)(seq % 6) && e.a == (i32)(seq % 32) &&
           e.b == (i32)(seq % 8) && e.x == (f64)seq * 0.5;
}

// The child: engine role. Consumes commands, produces events, publishes state.
// Returns an exit code; never runs the parent's atexit handlers.
[[noreturn]] static void childMain() {
    gTag     = "child ";
    gOwnsShm = false;                    // the child must never unlink

    ipc::ShmRegion r;
    if (!r.attach(gShmName, layout::kHash, ipc::kShmVersion, 5000)) {
        std::printf("  child FAIL  attach: %s\n", r.error());
        std::fflush(stdout);
        ::_exit(2);
    }
    auto*    state  = r.at<ipc::SharedState>(layout::kState);
    auto*    status = r.at<TestStatus>(layout::kStatus);
    CmdRing* cmds   = CmdRing::attachAt(r, layout::kCmds);
    EvtRing* evts   = EvtRing::attachAt(r, layout::kEvts);
    if (!state || !status || !cmds || !evts) {
        std::printf("  child FAIL  layout did not map\n");
        std::fflush(stdout);
        ::_exit(3);
    }
    state->engineState.store(ipc::SharedState::StateRunning, std::memory_order_relaxed);

    u64  sent = 0, seen = 0, badOrder = 0, badPayload = 0, spins = 0;
    u64  lastBlock = 0;
    const u64 deadline = ipc::monotonicNs() + kTimeoutNs;
    WireCmd c{};

    while (sent < kMessages || seen < kMessages) {
        if (sent < kMessages && evts->push(mkEvt(sent))) ++sent;

        while (seen < kMessages && cmds->pop(c)) {
            if (c.seq != seen)      ++badOrder;
            else if (!cmdOk(c, seen)) ++badPayload;
            ++seen;
        }

        // Stand in for Engine::publish(): one stamp per "block" of 1024
        // commands. Keyed on CROSSING a block boundary, not on `seen` landing
        // exactly on a multiple of 1024: the inner pop-loop above drains in
        // gulps under load (the parent runs far ahead), so `seen` can leap past
        // a boundary without ever equalling it — and a coincidence-based
        // `(seen & 0x3ff) == 0` then never fires, leaving playing/beat at their
        // init values. Crossing-based stamping publishes once per block whatever
        // the gulp size, so the parent always observes a live playhead.
        if (seen / 1024 != lastBlock) {
            lastBlock = seen / 1024;
            state->beat.store((f64)seen / 24000.0, std::memory_order_relaxed);
            state->playing.store(1, std::memory_order_relaxed);
            state->stampHeartbeat();
        }
        // The deadline is checked off a spin counter, not off progress: a
        // wedged peer makes no progress, which is exactly when the check has
        // to fire.
        if ((++spins & 0xffffull) == 0 && ipc::monotonicNs() > deadline) {
            std::printf("  child FAIL  timed out (sent %llu, seen %llu)\n",
                        (unsigned long long)sent, (unsigned long long)seen);
            std::fflush(stdout);
            ::_exit(4);
        }
    }

    status->cmdsSeen.store(seen, std::memory_order_relaxed);
    status->evtsSent.store(sent, std::memory_order_relaxed);
    status->badOrder.store(badOrder, std::memory_order_relaxed);
    status->badPayload.store(badPayload, std::memory_order_relaxed);
    state->engineState.store(ipc::SharedState::StateStopping, std::memory_order_relaxed);
    state->stampHeartbeat();
    status->childDone.store(1, std::memory_order_release);

    std::printf("  child note  consumed %llu commands, produced %llu events\n",
                (unsigned long long)seen, (unsigned long long)sent);
    std::fflush(stdout);
    ::_exit((badOrder || badPayload) ? 5 : 0);
}

static void testCrossProcess() {
    banner("4. cross-process exchange: 2 x 100k messages through two rings");
    note("parent = GUI role (pushes commands, drains events)");
    note("child  = engine role (drains commands, pushes events, publishes state)");

    // fork first, then create: the child must reach the region through
    // shm_open by name, not by inheriting a mapping. That is the code path the
    // real GUI/daemon pair takes.
    std::fflush(stdout);
    const pid_t pid = ::fork();
    if (pid == 0) childMain();
    if (pid < 0) { CHECK(false, "fork failed: %s", std::strerror(errno)); return; }

    ipc::ShmRegion r;
    if (!r.create(gShmName, layout::kBytes, layout::kHash)) {
        CHECK(false, "create: %s", r.error());
        ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0);
        return;
    }
    auto*    state  = r.at<ipc::SharedState>(layout::kState);
    auto*    status = r.at<TestStatus>(layout::kStatus);
    CmdRing* cmds   = CmdRing::createAt(r, layout::kCmds);
    EvtRing* evts   = EvtRing::createAt(r, layout::kEvts);
    if (!state || !status || !cmds || !evts) {
        CHECK(false, "layout did not fit in %zu payload bytes", r.payloadBytes());
        ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0);
        return;
    }
    state->init(48000.0, 256);
    r.publishReady();                    // the child's attach() unblocks here

    u64  sent = 0, got = 0, badOrder = 0, badPayload = 0;
    u64  pushRefused = 0, popEmpty = 0, spins = 0;
    bool timedOut = false;
    const u64 deadline = ipc::monotonicNs() + kTimeoutNs;
    const auto t0 = std::chrono::steady_clock::now();
    WireEvt e{};

    while (sent < kMessages || got < kMessages) {
        if (sent < kMessages) {
            if (cmds->push(mkCmd(sent))) ++sent; else ++pushRefused;
        }
        if (got < kMessages) {
            if (evts->pop(e)) {
                if (e.seq != got)      ++badOrder;
                else if (!evtOk(e, got)) ++badPayload;
                ++got;
            } else {
                ++popEmpty;
            }
        }
        // Spin counter, not progress: a dead child stops making progress and
        // that is precisely when the deadline has to be reachable.
        if ((++spins & 0xffffull) == 0 && ipc::monotonicNs() > deadline) { timedOut = true; break; }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const f64 secs = std::chrono::duration<f64>(t1 - t0).count();

    CHECK(!timedOut, "the exchange completed inside %llu s",
          (unsigned long long)(kTimeoutNs / 1000000000ull));
    CHECK(sent == kMessages, "parent pushed %llu commands", (unsigned long long)sent);
    CHECK(got == kMessages, "parent received %llu events", (unsigned long long)got);
    CHECK(badOrder == 0, "events arrived in strict FIFO order (%llu breaks)",
          (unsigned long long)badOrder);
    CHECK(badPayload == 0, "every event payload was intact (%llu corrupt)",
          (unsigned long long)badPayload);

    int wstat = 0;
    ::waitpid(pid, &wstat, 0);
    CHECK(WIFEXITED(wstat) && WEXITSTATUS(wstat) == 0,
          "child exited cleanly (exited=%d status=%d)", WIFEXITED(wstat) != 0,
          WIFEXITED(wstat) ? WEXITSTATUS(wstat) : -1);
    CHECK(status->childDone.load(std::memory_order_acquire) == 1,
          "child published its verdict through shared memory");
    CHECK(status->cmdsSeen.load() == kMessages,
          "child received all %llu commands (%llu)", (unsigned long long)kMessages,
          (unsigned long long)status->cmdsSeen.load());
    CHECK(status->badOrder.load() == 0, "commands arrived in strict FIFO order (%llu breaks)",
          (unsigned long long)status->badOrder.load());
    CHECK(status->badPayload.load() == 0, "every command payload was intact (%llu corrupt)",
          (unsigned long long)status->badPayload.load());

    // The polled block: written by the child, read here, exactly as the GUI
    // will read the engine's publish().
    CHECK(state->generation.load() > 0,
          "SharedState generation advanced across the process boundary (%llu)",
          (unsigned long long)state->generation.load());
    CHECK(state->playing.load() == 1, "the child's transport flag is visible");
    CHECK(state->beat.load() > 0.0, "the child's playhead is visible (%.3f)", state->beat.load());
    CHECK(state->engineState.load() == ipc::SharedState::StateStopping,
          "the child's final engine state is visible (%u)", state->engineState.load());
    CHECK(state->enginePid.load() == (i32)::getpid(),
          "enginePid is whoever called init() (%d)", state->enginePid.load());

    if (secs > 0.0) {
        const f64 total = (f64)(sent + got);
        note("throughput: %.2f M msgs/sec aggregate (%llu msgs in %.3f s, %.0f ns/msg)",
             total / secs / 1e6, (unsigned long long)(sent + got), secs, secs / total * 1e9);
        note("backpressure encountered: %llu refused pushes, %llu empty pops",
             (unsigned long long)pushRefused, (unsigned long long)popEmpty);
    }
}

// ---------------------------------------------------------------------------
// 4b. SharedState is a seqlock (shm v5)
// ---------------------------------------------------------------------------
//
// The property under test is the one docs/GUI-ON-DAEMON.md §2.1 asks for and
// step 1 could not have locally: a reader can take a snapshot of the WHOLE block
// that provably came from one publish. Everything else in this file exercises
// message delivery; this exercises coherence, which is a different failure and
// an invisible one — a torn snapshot is not a lost message, it is a frame in
// which a clip slot is drawn Playing with activeSlot == -1.
//
// The writer stamps every field it touches with the same publish number `k`, so
// "coherent" has an exact meaning: every field the reader copied carries one
// value of k. It then PARKS mid-publish, sequence held odd, which is what turns
// the negative control from a statistical hope into a certainty — an unguarded
// copy taken during the park has the new k in the transport fields and the old k
// in the per-track arrays, every single time.
//
// The negative control is the load-bearing half. "readCoherent() never returned
// a torn copy" proves nothing on its own if nothing was ever torn; the run has
// to demonstrate that this writer tears an unguarded reader, and then that the
// guarded reader on the same data does not.

namespace seq {
inline constexpr size_t kState = 0;
inline constexpr size_t kDone  = ipc::alignUp(kState + sizeof(ipc::SharedState), ipc::kCacheLine);
inline constexpr size_t kBytes = kDone + ipc::kCacheLine;
inline constexpr u32    kHash  =
    ipc::hashMix(ipc::fnv1a("nxtakt.ipc_test.seqlock.v1"), (u64)kBytes);

// Every field the reader checks, stamped with the publish number. Chosen to
// span the block: two before the writer's park and two after it, so a straddle
// cannot hide.
inline void stamp(ipc::SharedState& s, u64 k) {
    s.beat.store((f64)k, std::memory_order_relaxed);
    s.tempo.store((f64)k, std::memory_order_relaxed);
}
inline void stampLate(ipc::SharedState& s, u64 k) {
    for (int t = 0; t < kMaxTracks; ++t) {
        s.slotState[t].store((i32)k, std::memory_order_relaxed);
        s.activeSlot[t].store((i32)k, std::memory_order_relaxed);
    }
    s.masterMeterL.store((f32)k, std::memory_order_relaxed);
}

// Copies the four sentinels out. Returns true if they agree, i.e. if the copy
// came from one publish.
struct Sample { f64 beat, tempo; i32 slot0, act31; f32 master; };
inline void grab(const ipc::SharedState& s, Sample& o) {
    o.beat   = s.beat.load(std::memory_order_relaxed);
    o.tempo  = s.tempo.load(std::memory_order_relaxed);
    o.slot0  = s.slotState[0].load(std::memory_order_relaxed);
    o.act31  = s.activeSlot[kMaxTracks - 1].load(std::memory_order_relaxed);
    o.master = s.masterMeterL.load(std::memory_order_relaxed);
}
inline bool agrees(const Sample& x) {
    const f64 k = x.beat;
    return x.tempo == k && (f64)x.slot0 == k && (f64)x.act31 == k && (f64)x.master == k;
}
} // namespace seq

static void seqWriterMain() {
    gOwnsShm = false;                     // the child never unlinks
    gTag = "child ";
    ipc::ShmRegion r;
    if (!r.attach(gShmNameAlt, seq::kHash, ipc::kShmVersion, 2000)) ::_exit(11);
    auto* s    = r.at<ipc::SharedState>(seq::kState);
    auto* done = r.at<std::atomic<u32>>(seq::kDone);
    if (!s || !done) ::_exit(12);

    for (u64 k = 1; k <= 400; ++k) {
        s->publishBegin();
        seq::stamp(*s, k);
        // The park. 200 us out of a 1 ms cadence: a fifth of the time this
        // block is half-written, which no reader can miss and no reader may
        // believe.
        timespec park{0, 200000};
        ::nanosleep(&park, nullptr);
        seq::stampLate(*s, k);
        s->publishEnd();
        timespec gap{0, 800000};
        ::nanosleep(&gap, nullptr);
    }
    done->store(1, std::memory_order_release);
    ::_exit(0);
}

static void testStateSeqlock() {
    banner("4b. SharedState is a seqlock: a snapshot cannot straddle a publish");

    std::snprintf(gShmNameAlt, sizeof gShmNameAlt, "nxtakt-ipc-seq-%d", (int)::getpid());

    ipc::ShmRegion r;
    if (!r.create(gShmNameAlt, seq::kBytes, seq::kHash)) {
        CHECK(false, "create: %s", r.error());
        gShmNameAlt[0] = '\0';
        return;
    }
    auto* s    = r.at<ipc::SharedState>(seq::kState);
    auto* done = r.at<std::atomic<u32>>(seq::kDone);
    if (!s || !done) {
        CHECK(false, "layout did not fit in %zu payload bytes", r.payloadBytes());
        gShmNameAlt[0] = '\0';
        return;
    }
    s->init(48000.0, 256);
    seq::stamp(*s, 0);
    seq::stampLate(*s, 0);
    done->store(0, std::memory_order_relaxed);
    r.publishReady();

    std::fflush(stdout);
    const pid_t pid = ::fork();
    if (pid == 0) seqWriterMain();
    if (pid < 0) { CHECK(false, "fork failed: %s", std::strerror(errno)); gShmNameAlt[0] = '\0'; return; }

    u64 guarded = 0, guardedTorn = 0, guardedGaveUp = 0;
    u64 plain = 0, plainTorn = 0;
    u64 sawOdd = 0;
    const u64 deadline = ipc::monotonicNs() + 10ull * 1000000000ull;

    while (done->load(std::memory_order_acquire) == 0 && ipc::monotonicNs() < deadline) {
        // The negative control, first and on the same data: §2.1's own recipe
        // without the parity gate is exactly a plain copy, and a plain copy is
        // what step 1's local poll() has to live with.
        seq::Sample p{};
        seq::grab(*s, p);
        ++plain;
        if (!seq::agrees(p)) ++plainTorn;

        if (s->generation.load(std::memory_order_relaxed) & 1ull) ++sawOdd;

        seq::Sample g{};
        const bool ok = s->readCoherent([&] { seq::grab(*s, g); });
        ++guarded;
        if (!ok) ++guardedGaveUp;
        else if (!seq::agrees(g)) ++guardedTorn;
    }

    int wstat = 0;
    ::waitpid(pid, &wstat, 0);
    CHECK(WIFEXITED(wstat) && WEXITSTATUS(wstat) == 0,
          "the writer exited cleanly (exited=%d status=%d)", WIFEXITED(wstat) != 0,
          WIFEXITED(wstat) ? WEXITSTATUS(wstat) : -1);

    CHECK(sawOdd > 0,
          "the sequence was observed ODD, so publishBegin() really brackets the "
          "publish (%llu of %llu samples)",
          (unsigned long long)sawOdd, (unsigned long long)plain);
    CHECK((s->generation.load() & 1ull) == 0,
          "and EVEN once the writer is done, so publishEnd() closes it (%llu)",
          (unsigned long long)s->generation.load());

    // The negative control. Remove publishBegin() from the writer and this stays
    // 0 while the check below starts failing — which is the same fact from the
    // other side and the reason both are asserted.
    CHECK(plainTorn > 0,
          "an UNGUARDED copy of the same block tears: %llu of %llu samples "
          "disagreed with themselves",
          (unsigned long long)plainTorn, (unsigned long long)plain);
    CHECK(guardedTorn == 0,
          "readCoherent() never handed back a torn snapshot (%llu of %llu proved "
          "coherent, %llu gave up on the retry budget)",
          (unsigned long long)(guarded - guardedTorn - guardedGaveUp),
          (unsigned long long)guarded, (unsigned long long)guardedGaveUp);
    note("the writer parks 200 us mid-publish, so the unguarded tear rate is a "
         "property of the test rather than of the machine it runs on.");

    r.close();
    gShmNameAlt[0] = '\0';
}

// ---------------------------------------------------------------------------
// 5. stale-region reaping
// ---------------------------------------------------------------------------

static void testStaleReap() {
    banner("5. crash-orphan detection and reaping");

    // A child creates a region and is killed with SIGKILL — no destructor, no
    // unlink. This is exactly the engine-crash case.
    char orphan[64];
    std::snprintf(orphan, sizeof orphan, "nxtakt-ipc-orphan-%d", (int)::getpid());
    std::snprintf(gShmNameAlt, sizeof gShmNameAlt, "%s", orphan);

    std::fflush(stdout);
    const pid_t pid = ::fork();
    if (pid == 0) {
        ipc::ShmRegion r;
        if (!r.create(orphan, 4096, layout::kHash)) ::_exit(1);
        r.publishReady();
        for (;;) ::pause();              // killed below, region left behind
    }
    if (pid < 0) { CHECK(false, "fork failed"); return; }

    // Wait for the region to appear.
    ipc::ShmRegion probe;
    const bool up = probe.attach(orphan, layout::kHash, ipc::kShmVersion, 3000);
    CHECK(up, "child created the region%s%s", up ? "" : ": ", up ? "" : probe.error());
    CHECK(!ipc::ShmRegion::reapIfStale(orphan), "a live creator's region is not reaped");
    probe.close();

    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);

    CHECK(ipc::ShmRegion::reapIfStale(orphan),
          "the orphan of a SIGKILLed creator is detected and unlinked");
    CHECK(!ipc::ShmRegion::reapIfStale(orphan), "reaping is idempotent");

    // And a fresh creator can now take the name.
    ipc::ShmRegion fresh;
    const bool retaken = fresh.create(orphan, 4096, layout::kHash);
    CHECK(retaken, "a new creator claims the reclaimed name%s%s",
          retaken ? "" : ": ", retaken ? "" : fresh.error());
    fresh.close();
    gShmNameAlt[0] = '\0';
}

// ---------------------------------------------------------------------------
// 6. F6 — a forged EvBlockRetired must not corrupt the client's free list
// ---------------------------------------------------------------------------
//
// The daemon echoes EvBlockRetired back to say "the engine can no longer reach
// this block". A hostile (or buggy) peer can push the same event with a ref
// that points into live sample data: as f32 bit patterns, near-silent audio is
// full of offsets whose reinterpreted PoolBlock header reads state=Retiring,
// live=0, refs=0 — exactly the shape confirmRetired() used to act on. The fix
// is the self-mixed block magic in validRef(): an interior offset is not a
// block, so blockAt() returns null and the free list is never touched.

static void testForgedRetirement() {
    banner("6. a forged EvBlockRetired cannot corrupt the client free list (F6)");
    char session[32];
    std::snprintf(session, sizeof session, "ipc-forge-%d", (int)::getpid());

    // Register the pool name for the crash/atexit cleanup path so a mid-test
    // failure cannot leave it in /dev/shm.
    std::snprintf(gShmNameAlt, sizeof gShmNameAlt, "nxtakt-pool-%s", session);

    ipc::EngineClient c;
    if (!c.createPool(session, 4u << 20)) { CHECK(false, "createPool: %s", c.error()); return; }
    ipc::SamplePool& p = c.pool();

    // Fill with a denormal ~ near-silence: the audit's "qualifying offset"
    // pattern, so the forged header genuinely looks retirement-shaped.
    std::vector<f32> dc((size_t)4096 * 2, 4.2e-45f);
    const u64 a = c.poolWrite(dc.data(), 4096, 2, 48000.0, 1);
    CHECK(a != 0, "a real sample block at %llu", (unsigned long long)a);
    if (!a) { c.closePool(); return; }

    const u32 freeLen0 = p.freeListLength();
    const u64 live0    = p.liveBlocks();

    // An interior, 64-aligned offset of the block's own data, under the bump —
    // the dangerous case bounds-only validation used to accept.
    const u64 forged = a + 1024;
    CHECK(forged % ipc::kPoolAlign == 0 && forged < p.bump(),
          "the forged ref is 64-aligned and under the bump (the dangerous case)");

    ipc::WireEvent e{};
    e.type = ipc::EvBlockRetired;
    e.ref  = forged;
    CHECK(c.observe(e), "observe() consumes the forged echo as a protocol event");

    // Nothing moved: free list, live count, and the real block are all intact.
    CHECK(p.freeListLength() == freeLen0,
          "the free list is unchanged (%u == %u)", p.freeListLength(), freeLen0);
    CHECK(p.liveBlocks() == live0,
          "the live-block count is unchanged (%llu == %llu)",
          (unsigned long long)p.liveBlocks(), (unsigned long long)live0);
    CHECK(p.stateOf(a) == ipc::BlockQuiescent,
          "the real block is untouched (%s)", ipc::poolStateName(p.stateOf(a)));

    // The allocator still works and hands back a usable block.
    const u64 b = c.poolWrite(dc.data(), 1024, 2, 48000.0, 2);
    CHECK(b != 0 && b != a, "a subsequent alloc still works (%llu)", (unsigned long long)b);
    CHECK(b && p.data<f32>(b) != nullptr, "and the block is usable");

    // Positive control: a genuinely-Retiring block IS retired by its own echo,
    // so the magic check did not break the real path.
    const u64 g = c.poolWrite(dc.data(), 512, 2, 48000.0, 3);
    CHECK(g != 0, "a third block to retire for real (%llu)", (unsigned long long)g);
    p.markLive(g);         // a clip cell points here
    p.markDisplaced(g);    // it stopped pointing here -> Retiring
    CHECK(p.stateOf(g) == ipc::BlockRetiring, "the real block is Retiring (%s)",
          ipc::poolStateName(p.stateOf(g)));
    ipc::WireEvent ge{};
    ge.type = ipc::EvBlockRetired;
    ge.ref  = g;
    c.observe(ge);
    CHECK(p.stateOf(g) == ipc::BlockQuiescent,
          "its own echo retires it correctly (%s)", ipc::poolStateName(p.stateOf(g)));

    c.poolRelease(a);
    c.poolRelease(b);
    c.poolRelease(g);
    c.closePool();
    CHECK(!shmExistsPool(session), "the forge-test pool is unlinked");
    gShmNameAlt[0] = '\0';
}

// ---------------------------------------------------------------------------
// 7. F8 — an adopted pool is not reaped when its ORIGINAL creator dies
// ---------------------------------------------------------------------------
//
// The pool is designed to outlive its creator: a crashed GUI leaves it behind
// and a replacement adopts it. reapIfStale() keys liveness on
// ShmHeader::creatorPid, which used to keep naming the dead original — so a
// live, adopted pool read as an orphan and could be unlinked out from under its
// new owner. adoptCreator() (called from SamplePool::attach) moves the liveness
// key to the adopting process, so the pool reads as alive.

static void testAdoptedPoolLiveness() {
    banner("7. an adopted pool survives a reap-check after its creator dies (F8)");
    char pool[48];
    std::snprintf(pool, sizeof pool, "nxtakt-ipc-adopt-%d", (int)::getpid());
    std::snprintf(gShmNameAlt, sizeof gShmNameAlt, "%s", pool);   // crash-safe cleanup

    std::fflush(stdout);
    const pid_t pid = ::fork();
    if (pid == 0) {
        ipc::SamplePool sp;
        if (!sp.create(pool, 2u << 20)) ::_exit(1);
        std::vector<f32> dc((size_t)1024 * 2, 0.5f);
        sp.writeSamples(dc.data(), 1024, 2, 48000.0, 1);
        sp.abandon();                // detach WITHOUT unlink: leave it behind
        for (;;) ::pause();          // SIGKILLed below; region persists
    }
    if (pid < 0) { CHECK(false, "fork failed"); return; }

    // Parent adopts the pool the child created (this re-stamps the liveness key).
    ipc::SamplePool mine;
    const bool up = mine.attach(pool, 3000);
    CHECK(up, "adopted the child's pool%s%s", up ? "" : ": ", up ? "" : mine.error());
    const u64 childBump = mine.bump();

    // The original creator is now provably gone.
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);

    // Pre-fix, this reaped a live adopted pool. Now the key is us, and we live.
    CHECK(!ipc::ShmRegion::reapIfStale(pool),
          "reapIfStale() leaves the adopted pool alone (the key is now this process)");
    CHECK(shmExistsPool(pool), "the pool region still exists in /dev/shm");
    CHECK(mine.valid(), "and our mapping is still valid");
    CHECK(mine.bump() == childBump && childBump > ipc::kPoolArenaOffset,
          "the block the child wrote survived (bump %llu)", (unsigned long long)childBump);

    // We adopted (unlink_ == false), so unlink explicitly to keep /dev/shm clean.
    mine.close();
    ipc::ShmRegion::forceUnlink(pool);
    CHECK(!shmExistsPool(pool), "and it unlinks cleanly on the way out");
    gShmNameAlt[0] = '\0';
}

// ---------------------------------------------------------------------------
// 8. the wire classifiers and shapes wave 8g adds
// ---------------------------------------------------------------------------
//
// 8a appended four commands and two events to engine.h and left them
// compiled-and-unused; commandIsKnown() still bounded at Cmd::RecordMidiSlot,
// so all four classified as UNKNOWN. That failed closed — which is why the tree
// was safe — and this is the assertion that it is now classified properly and
// that nothing else moved class while we were in there.

static void testArrangementClassifiers() {
    banner("8. the arrangement's commands and events classify (the 8a hand-off)");

    // The four appended commands. Two are pure scalars; two carry a pool blob.
    // None of them may be "unknown" any more, and none may be mistaken for a
    // pointer-carrying command — which is what a wrong answer here would mean
    // in the daemon: a permanent refusal that no client could work around.
    struct Row { const char* name; Cmd c; bool known, scalar, pooled, arr, ptr; };
    const Row rows[] = {
        {"SetArrangement",    Cmd::SetArrangement,    true,  false, false, true,  false},
        {"SetTrackAutos",     Cmd::SetTrackAutos,     true,  false, false, true,  false},
        {"Locate",            Cmd::Locate,            true,  true,  false, false, false},
        {"BackToArrangement", Cmd::BackToArrangement, true,  true,  false, false, false},
        // Unchanged neighbours, so a reclassification cannot slip in unnoticed.
        {"SetClip",           Cmd::SetClip,           true,  false, true,  false, false},
        {"SetChain",          Cmd::SetChain,          true,  false, false, false, true},
        // v9: the two Record commands stopped carrying a pointer. What crosses
        // is a capacity, and the buffer the audio thread appends into is the
        // daemon's own -- so `pointer` here reading true again would be the
        // daemon refusing recording outright, permanently, with no client-side
        // workaround. It is exactly the assertion that was wrong for a wave.
        {"RecordSlot",        Cmd::RecordSlot,        true,  false, false, false, false},
        {"RecordMidiSlot",    Cmd::RecordMidiSlot,    true,  false, false, false, false},
        {"SetTempo",          Cmd::SetTempo,          true,  true,  false, false, false},
    };
    for (const Row& r : rows) {
        const u32 t = (u32)r.c;
        const bool ok = ipc::commandIsKnown(t)       == r.known  &&
                        ipc::commandIsScalar(t)      == r.scalar &&
                        ipc::commandIsPooled(t)      == r.pooled &&
                        ipc::commandIsArrangement(t) == r.arr    &&
                        ipc::commandCarriesPointer(t)== r.ptr;
        CHECK(ok, "%s (%u): known=%d scalar=%d pooled=%d arrangement=%d pointer=%d",
              r.name, t, (int)ipc::commandIsKnown(t), (int)ipc::commandIsScalar(t),
              (int)ipc::commandIsPooled(t), (int)ipc::commandIsArrangement(t),
              (int)ipc::commandCarriesPointer(t));
    }
    // SetSignatures is the last enumerator as of protocol v8. This line was
    // itself the hand-copied number it warns about -- it still said
    // BackToArrangement + 1 after the bound moved past it, and a stale test
    // binary (the Makefile missed control.h as a dependency) hid the mismatch
    // from every local run while a fresh CI build caught it.
    CHECK(!ipc::commandIsKnown((u32)Cmd::SetSignatures + 1),
          "and one past the last enumerator is still unknown — the bound is the "
          "enum's end, not a hand-copied number");

    // Both new events carry a pointer to a block the DAEMON built, so both are
    // consumed rather than forwarded. A `true` here would send a daemon-heap
    // address to another process.
    CHECK(!ipc::eventIsScalar((u32)Ev::ArrangementRetired), "Ev::ArrangementRetired is not scalar");
    CHECK(!ipc::eventIsScalar((u32)Ev::TrackAutosRetired),  "Ev::TrackAutosRetired is not scalar");
    CHECK(ipc::eventIsScalar((u32)Ev::ClipStarted), "and the scalar events still are");

    // The wire shapes, and the mirrors that make a memcpy honest.
    CHECK(sizeof(ipc::WireArrHeader) == 48 && sizeof(ipc::WireArrItem) == 40,
          "WireArrHeader is %zu B and WireArrItem is %zu B",
          sizeof(ipc::WireArrHeader), sizeof(ipc::WireArrItem));
    CHECK(sizeof(ipc::WireJournal) == sizeof(ArrJournal),
          "WireJournal mirrors ArrJournal (%zu B)", sizeof(ipc::WireJournal));
    CHECK(ipc::arrangementBytes(2, 1) == 48 + 2 * 40 + 176,
          "a two-item, one-clip blob is %llu B (WireClip is 176 from v11)",
          (unsigned long long)ipc::arrangementBytes(2, 1));
    CHECK(ipc::kMaxArrLanes == kMaxRtArrLanes,
          "the wire lane bound and the engine's agree (%d)", (int)ipc::kMaxArrLanes);
    CHECK(ipc::kProtocolVersion == 12 && ipc::kPoolVersion == 9 && ipc::kShmVersion == 6,
          "protocol v%u, pool v%u, shm v%u", ipc::kProtocolVersion, ipc::kPoolVersion,
          ipc::kShmVersion);
    CHECK(ipc::control::kJournal > ipc::control::kParams &&
          ipc::control::kCatalog >= ipc::control::kJournal + ipc::JournalRing::bytes(),
          "the journal is the ninth section, appended at %zu",
          ipc::control::kJournal);
    // v6's tenth section. The property that matters is not where it is but that
    // it went on the END: every offset below it is unchanged, so the only
    // reason a v5 and a v6 binary refuse each other is the hash and the
    // version, and not a section that quietly moved under a peer.
    CHECK(ipc::control::kCatalog > ipc::control::kJournal &&
          ipc::control::kBytes == ipc::control::kCatalog + ipc::kCatalogTableBytes,
          "the catalog is the tenth section, appended at %zu of %zu B",
          ipc::control::kCatalog, ipc::control::kBytes);
    CHECK(sizeof(ipc::WirePluginDesc) == 512 && ipc::kMaxCatalog == 2048 &&
          ipc::kCatalogTableBytes == 512ull * 2048ull,
          "WirePluginDesc is %zu B x %u = %zu B", sizeof(ipc::WirePluginDesc),
          (u32)ipc::kMaxCatalog, ipc::kCatalogTableBytes);
    // The row must be able to hold the longest URI the pool's string blobs can
    // carry, or the catalog could advertise a plugin AddDevice cannot name.
    CHECK(sizeof(ipc::WirePluginDesc::uri) >= sizeof(ipc::WireDeviceInfo::uri),
          "a catalog URI (%zu B) is at least as wide as a device URI (%zu B)",
          sizeof(ipc::WirePluginDesc::uri), sizeof(ipc::WireDeviceInfo::uri));
}

// ---------------------------------------------------------------------------
// 9. the journal ring across a real process boundary, and its drop counter
// ---------------------------------------------------------------------------
//
// §9.6: the journal gets a ring of its own because the two channels have
// different failure budgets. An event must not be lost — EvClipAck wedges a
// clip cell for the rest of the session if it is — while a journal entry must
// be KNOWN to be lost, because §5.4 refuses a take whose journal has a gap
// rather than committing it short.
//
// So both halves are asserted here: that a full run of entries crosses with its
// `seq` contiguous, and that an OVERRUN is visible as a gap in `seq` and as a
// drop count. Overflowing the ring on purpose is the point — the failure has to
// be observable, or "refuse, do not commit short" is not decidable.

static void testJournalRingCrossProcess() {
    banner("9. the journal ring carries entries, and a gap is observable (§5.4, §9.6)");

    char nm[64];
    std::snprintf(nm, sizeof nm, "nxtakt-ipc-journal-%d", (int)::getpid());
    std::snprintf(gShmNameAlt, sizeof gShmNameAlt, "%s", nm);

    // A region holding exactly one journal ring plus a phase word. The phase is
    // what makes the overflow DETERMINISTIC rather than a race we hope to win:
    // the consumer is held off while the producer overruns the ring, and only
    // then allowed to drain, so the gap lands in a known place with a known
    // size.
    struct JStatus {
        std::atomic<u64> seen, gaps, gapSize, bad;
        std::atomic<u32> phase;      // see the handshake below
    };
    const size_t kStatusOff = 0;
    const size_t kRingOff   = ipc::alignUp(sizeof(JStatus), ipc::kCacheLine);
    const size_t kBytes     = kRingOff + ipc::JournalRing::bytes();
    const u32    kHash      = ipc::fnv1a("nxtakt.journal_test.v1");

    ipc::ShmRegion region;
    if (!region.create(nm, kBytes, kHash)) {
        CHECK(false, "create the journal region: %s", region.error());
        return;
    }
    JStatus* st = region.at<JStatus>(kStatusOff);
    ipc::JournalRing* ring = ipc::JournalRing::createAt(region, kRingOff);
    CHECK(st && ring, "a %zu B region holding one JournalRing", kBytes);
    if (!st || !ring) return;
    st->seen.store(0); st->gaps.store(0); st->gapSize.store(0); st->bad.store(0);
    st->phase.store(0, std::memory_order_release);
    region.publishReady();

    const u32 kBurstA = 12000;                       // overruns the ring on purpose
    const u32 kBurstB = 200;                         // sent after the drain: the gap
    CHECK(kBurstA > ipc::JournalRing::capacity(),
          "burst A is %u entries through a %u-slot ring, so it must overflow",
          kBurstA, ipc::JournalRing::capacity());

    // The payload every entry carries, derived from seq so a torn slot is a
    // hard failure rather than a plausible one.
    const auto fill = [](u32 i) {
        ipc::WireJournal j{};
        j.kind  = (u32)JournalKind::NoteOn;
        j.seq   = i;
        j.track = (i32)(i % 32);
        j.a     = 60 + (i32)(i % 12);
        j.beat  = (f64)i * 0.25;
        return j;
    };

    std::fflush(stdout);
    const pid_t pid = ::fork();
    if (pid == 0) {
        gOwnsShm = false;                            // the child never unlinks
        ipc::ShmRegion r;
        if (!r.attach(nm, kHash, ipc::kShmVersion, 3000)) ::_exit(1);
        JStatus* cs = r.at<JStatus>(kStatusOff);
        ipc::JournalRing* cr = ipc::JournalRing::attachAt(r, kRingOff);
        if (!cs || !cr) ::_exit(1);

        u64 seen = 0, gaps = 0, gapSize = 0, bad = 0;
        u32  expect = 0;
        bool first  = true;
        const auto drain = [&](u32 untilPhase) {
            const u64 deadline = ipc::monotonicNs() + 10ull * 1000000000ull;
            for (;;) {
                ipc::WireJournal j;
                while (cr->pop(j)) {
                    ++seen;
                    if (j.kind != (u32)JournalKind::NoteOn || j.track != (i32)(j.seq % 32) ||
                        j.beat != (f64)j.seq * 0.25)
                        ++bad;
                    // A consumer that sees seq jump knows exactly HOW MANY
                    // entries it lost and WHERE. That is the whole reason seq
                    // increments on every attempted push and not on every
                    // successful one.
                    if (!first && j.seq != expect) { ++gaps; gapSize += j.seq - expect; }
                    first  = false;
                    expect = j.seq + 1;
                }
                if (cs->phase.load(std::memory_order_acquire) >= untilPhase && cr->empty())
                    return;
                if (ipc::monotonicNs() >= deadline) return;
            }
        };

        // Held off until the producer has finished overrunning the ring.
        while (cs->phase.load(std::memory_order_acquire) < 1) {}
        drain(1);
        cs->phase.store(2, std::memory_order_release);   // "I am empty, send more"
        while (cs->phase.load(std::memory_order_acquire) < 3) {}
        drain(3);

        cs->seen.store(seen, std::memory_order_relaxed);
        cs->gaps.store(gaps, std::memory_order_relaxed);
        cs->gapSize.store(gapSize, std::memory_order_relaxed);
        cs->bad.store(bad, std::memory_order_relaxed);
        cs->phase.store(4, std::memory_order_release);
        ::_exit(0);
    }
    if (pid < 0) { CHECK(false, "fork failed"); region.close(); return; }

    // Burst A, with nobody draining: an entry that will not fit is GONE,
    // because there is nowhere to put it back. This is the daemon's own
    // behaviour and the engine's before it.
    u64 dropped = 0;
    for (u32 i = 0; i < kBurstA; ++i) if (!ring->push(fill(i))) ++dropped;
    st->phase.store(1, std::memory_order_release);

    const auto waitPhase = [&](u32 want) {
        const u64 deadline = ipc::monotonicNs() + 10ull * 1000000000ull;
        while (ipc::monotonicNs() < deadline)
            if (st->phase.load(std::memory_order_acquire) >= want) return true;
        return false;
    };
    const bool drained = waitPhase(2);
    CHECK(drained, "the consumer drained burst A and asked for more");

    // Burst B, into a ring that is now empty: every one of these arrives, so
    // the loss shows up as an INTERIOR gap rather than a truncated tail.
    for (u32 i = 0; i < kBurstB; ++i)
        if (!ring->push(fill(kBurstA + i))) ++dropped;
    st->phase.store(3, std::memory_order_release);

    const bool finished = waitPhase(4);
    ::waitpid(pid, nullptr, 0);
    CHECK(finished, "and finished");

    const u64 seen = st->seen.load(), gaps = st->gaps.load(),
              gapSize = st->gapSize.load(), bad = st->bad.load();
    CHECK(bad == 0, "every entry that crossed arrived intact (%llu torn)",
          (unsigned long long)bad);
    CHECK(seen + dropped == kBurstA + kBurstB,
          "%llu entries crossed and %llu were refused: %llu of %u sent",
          (unsigned long long)seen, (unsigned long long)dropped,
          (unsigned long long)(seen + dropped), kBurstA + kBurstB);
    CHECK(dropped > 0, "the ring did overflow, so the failure being tested happened "
          "(%llu refused)", (unsigned long long)dropped);
    CHECK(gaps == 1, "the loss is VISIBLE to the consumer as a gap in seq (%llu gap)",
          (unsigned long long)gaps);
    CHECK(gapSize == dropped,
          "and the gap measures EXACTLY what was lost: %llu missing, %llu refused — "
          "the same fact reported from both sides, which is what makes 'refuse the "
          "take, do not commit it short' decidable",
          (unsigned long long)gapSize, (unsigned long long)dropped);
    note("a consumer seeing this discards the take and names the count, per §5.4.");

    region.close();
    CHECK(!shmExistsPool(nm), "the journal region unlinks cleanly");
    gShmNameAlt[0] = '\0';
}

// ---------------------------------------------------------------------------
// 10. the take file  (docs/GUI-ON-DAEMON.md §7, src/ipc/take.h)
// ---------------------------------------------------------------------------
//
// §7's choice was between a second shared region written by the daemon and a
// FILE. This is the file half tested where it can be tested exhaustively: in
// one process, with no daemon, no timing and no audio device. What daemon_test
// and handle_test then have to prove is only that the two ends use it, not that
// it works.
//
// The properties that matter are the ones a wrong answer would make silent:
//   * what was written is what is read back, to the bit — a take is the
//     performance and a resampled or rounded one is a different performance;
//   * a short read is a FAILURE and not a short success (§5.4's rule, applied
//     to the transport rather than to the journal);
//   * a file that is not one of ours is refused rather than reinterpreted;
//   * the client composes paths from a directory it was GIVEN.

static void testTakeFiles() {
    banner("10. the take file: written, read back, and refused when it is not one");

    char dir[256];
    std::snprintf(dir, sizeof dir, "/tmp/nxtakt-taketest-%d/takes/s", (int)::getpid());
    CHECK(ipc::takeMkdirP(dir), "mkdir -p makes the whole take path ('%s')", dir);
    CHECK(ipc::takeMkdirP(dir), "and is idempotent, because two daemons may race it");

    char pw[512], pm[512];
    ipc::takePath(dir, 7, /*midi*/false, pw, sizeof pw);
    ipc::takePath(dir, 7, /*midi*/true,  pm, sizeof pm);
    CHECK(std::strstr(pw, "/7.wav") && std::strstr(pm, "/7.ntk"),
          "an audio take is <uid>.wav and a MIDI take is <uid>.ntk ('%s', '%s')",
          std::strrchr(pw, '/'), std::strrchr(pm, '/'));

    // -- audio, bit for bit -------------------------------------------------
    //
    // A ramp with the SIGN of the channel in it, so a reader that swapped or
    // dropped a channel cannot pass: every odd sample is the negation of the
    // even one before it.
    const i64 frames = 5000;
    std::vector<f32> src((size_t)frames * 2);
    for (i64 i = 0; i < frames; ++i) {
        const f32 v = (f32)(i % 65536) / 65536.f;
        src[(size_t)i * 2]     =  v;
        src[(size_t)i * 2 + 1] = -v;
    }
    CHECK(ipc::writeAudioTake(pw, src.data(), frames, 2, 48000.0), "an audio take writes");

    std::vector<f32> back((size_t)frames * 2, 12345.f);
    int ch = 0; f64 rate = 0.0;
    const i64 got = ipc::readAudioTake(pw, back.data(), frames, &ch, &rate);
    CHECK(got == frames, "and reads back its whole length (%lld of %lld)",
          (long long)got, (long long)frames);
    CHECK(ch == 2 && rate == 48000.0, "with its channel count and rate (%d ch, %.0f Hz)",
          ch, rate);
    CHECK(std::memcmp(src.data(), back.data(), src.size() * sizeof(f32)) == 0,
          "and every sample is IDENTICAL — float32 in, float32 out, no conversion "
          "anywhere on the path");

    // -- a buffer smaller than the take -------------------------------------
    //
    // The client's buffer is App's and is exactly the capacity it asked for, so
    // this cannot happen in the ordinary path. It can happen after an engine
    // restart with a different sample rate, and the honest answer is "as much as
    // fits, and say so" rather than a write past the end.
    std::vector<f32> small(200 * 2, 0.f);
    const i64 clipped = ipc::readAudioTake(pw, small.data(), 200, nullptr, nullptr);
    CHECK(clipped == 200, "a buffer shorter than the take gets exactly what fits (%lld)",
          (long long)clipped);
    CHECK(std::memcmp(src.data(), small.data(), small.size() * sizeof(f32)) == 0,
          "and it is the take's FIRST frames, in order");

    // -- MIDI ---------------------------------------------------------------
    std::vector<ipc::WireNote> notes(64);
    for (size_t i = 0; i < notes.size(); ++i) {
        notes[i] = ipc::WireNote{};
        notes[i].beat  = (f64)i * 0.25;
        notes[i].len   = 0.5;
        notes[i].pitch = (u8)(36 + i % 60);
        notes[i].vel   = (u8)(1 + i % 126);
    }
    CHECK(ipc::writeMidiTake(pm, notes.data(), (i64)notes.size(), 12.5), "a MIDI take writes");
    std::vector<ipc::WireNote> nback(notes.size());
    f64 startBeat = 0.0;
    const i64 ngot = ipc::readMidiTake(pm, nback.data(), (i64)nback.size(), &startBeat);
    CHECK(ngot == (i64)notes.size(), "and reads back every note (%lld)", (long long)ngot);
    CHECK(std::memcmp(notes.data(), nback.data(), notes.size() * sizeof(ipc::WireNote)) == 0,
          "field for field — beat, length, pitch and velocity");
    CHECK(startBeat == 12.5, "carrying the beat the take began on (%.2f)", startBeat);

    // -- what is refused ----------------------------------------------------
    char bogus[512];
    std::snprintf(bogus, sizeof bogus, "%s/notatake.wav", dir);
    { FILE* f = std::fopen(bogus, "wb"); std::fwrite("not a wav at all", 1, 16, f); std::fclose(f); }
    CHECK(ipc::readAudioTake(bogus, back.data(), frames, nullptr, nullptr) < 0,
          "a file that is not a RIFF/WAVE is REFUSED, not reinterpreted");
    CHECK(ipc::readMidiTake(pw, nback.data(), (i64)nback.size()) < 0,
          "and a WAV read as a MIDI take is refused on its magic");
    CHECK(ipc::readAudioTake("/tmp/nxtakt-there-is-no-such-take.wav", back.data(), frames,
                             nullptr, nullptr) < 0,
          "a take that is not there answers -1 rather than 0 frames of silence — the "
          "difference between 'lost' and 'empty' is the whole of what a user needs");

    // A take past the ceiling is refused by the WRITER, so a capacity the daemon
    // should never have accepted cannot become a file it cannot finish.
    CHECK(!ipc::writeAudioTake(pw, src.data(),
                               (i64)(ipc::kMaxTakeBytes / 8ull) + 1, 2, 48000.0),
          "a take past kMaxTakeBytes is refused at the write, not truncated");

    // -- the sweep ----------------------------------------------------------
    //
    // The daemon's reclaim for a client that died: take files nobody will ever
    // claim, removed by the next session that owns the directory.
    { FILE* f = std::fopen((std::string(dir) + "/9.wav.part").c_str(), "wb"); std::fclose(f); }
    { FILE* f = std::fopen((std::string(dir) + "/keepme.txt").c_str(), "wb"); std::fclose(f); }
    const u32 swept = ipc::sweepTakes(dir);
    CHECK(swept == 4, "the sweep takes the wav, the ntk, the bogus wav and the half-written "
                      ".part (%u)", swept);
    CHECK(::access((std::string(dir) + "/keepme.txt").c_str(), F_OK) == 0,
          "and NOTHING else: it unlinks files, so it only ever names its own");
    CHECK(ipc::sweepTakes("/tmp/nxtakt-there-is-no-such-directory") == 0,
          "sweeping a directory that is not there is 0 and not an error");

    ::unlink((std::string(dir) + "/keepme.txt").c_str());
    ::rmdir(dir);
    char up[256];
    std::snprintf(up, sizeof up, "/tmp/nxtakt-taketest-%d/takes", (int)::getpid());
    ::rmdir(up);
    std::snprintf(up, sizeof up, "/tmp/nxtakt-taketest-%d", (int)::getpid());
    ::rmdir(up);

    // -- the directory rule --------------------------------------------------
    //
    // The formula exists in exactly one place and the client never evaluates it
    // — the DAEMON does, and publishes the answer. This asserts the formula's
    // own shape; testTakeDirIsTheDaemonsWord (daemon_test §17) asserts the half
    // that matters, which is that the client uses the published value.
    char d1[256], d2[256];
    ::setenv("XDG_RUNTIME_DIR", "/run/user/9999", 1);
    ipc::takeDirFor("sess", d1, sizeof d1);
    ::unsetenv("XDG_RUNTIME_DIR");
    ipc::takeDirFor("sess", d2, sizeof d2);
    CHECK(!std::strcmp(d1, "/run/user/9999/nxtakt/takes/sess"),
          "the runtime dir is where takes live ('%s')", d1);
    CHECK(std::strstr(d2, "/tmp/nxtakt-") && std::strstr(d2, "/takes/sess"),
          "and a daemon with no runtime dir still has somewhere to write ('%s')", d2);
    ipc::takeDirFor(nullptr, d2, sizeof d2);
    CHECK(std::strstr(d2, "/takes/default"), "an unnamed session is 'default' ('%s')", d2);
}

// ---------------------------------------------------------------------------
// 11. the device-state blob  (GUI-ON-DAEMON.md §15)
// ---------------------------------------------------------------------------
//
// PoolKindDeviceState is the first pool kind that is a HEADER plus a payload,
// and the first that names a SECOND block. Both of those are new ways to be
// wrong, so both are tested here — in one process, with no daemon — and what
// daemon_test then has to prove is only that the far side uses it.
//
// The properties a wrong answer would make silent:
//   * the text is the PERSISTED spelling, byte for byte. If anything wire-only
//     ever leaked into it, a saved set would carry a pool offset.
//   * a state blob and a string blob are DIFFERENT KINDS. They are both "some
//     bytes and a terminator", so nothing but `kind` stands between a state
//     read as a URI and a URI read as a state.
//   * an over-long state is REFUSED and not truncated. A truncated state string
//     is not a shorter state, it is a different one — for a sampler, a
//     different FILE.
//   * CmdSetDeviceState classifies as a DEVICE command, and CmdTakeRelease
//     still does not, after v10 renumbered it.

static void testDeviceStateBlob() {
    banner("11. the device-state blob: header, text, kind, and the classifiers");

    // -- classification, first: it is what decides whether the command is even
    //    delivered, and 8a's lesson is that a bound spelled by hand goes stale.
    CHECK(ipc::commandIsDevice(ipc::CmdSetDeviceState),
          "CmdSetDeviceState is a device command");
    CHECK(ipc::commandIsKnown(ipc::CmdSetDeviceState),
          "and this build knows it (an unknown one is answered RejectUnknownCommand)");
    CHECK(!ipc::commandCarriesPointer(ipc::CmdSetDeviceState),
          "and it carries no pointer: the payload is a pool offset");
    // The renumber. CmdTakeRelease moved from +6 to +7 so the device commands
    // stay contiguous; if it had stayed put it would now be INSIDE
    // commandIsDevice's range and would be dispatched to the device queue.
    CHECK(!ipc::commandIsDevice(ipc::CmdTakeRelease),
          "CmdTakeRelease is still not a device command after the v10 renumber");
    CHECK(ipc::commandIsTakeRelease(ipc::CmdTakeRelease) &&
          ipc::commandIsKnown(ipc::CmdTakeRelease),
          "and it still classifies as itself");
    CHECK(ipc::CmdSetDeviceState != ipc::CmdTakeRelease &&
          ipc::CmdSetDeviceState != ipc::CmdSetRackState,
          "the three occupy distinct slots");

    CHECK(sizeof(ipc::WireDeviceState) == 56 && ipc::kDeviceStateVersion == 2,
          "WireDeviceState is %zu B at v%u",
          sizeof(ipc::WireDeviceState), ipc::kDeviceStateVersion);

    char session[48];
    std::snprintf(session, sizeof session, "ipc-devstate-%d", (int)::getpid());
    std::snprintf(gShmNameAlt, sizeof gShmNameAlt, "nxtakt-pool-%s", session);

    ipc::EngineClient c;
    if (!c.createPool(session, 4u << 20)) { CHECK(false, "createPool: %s", c.error()); return; }
    ipc::SamplePool& p = c.pool();

    // Validation through the READER's entry point, deliberately: poolValidate is
    // the one function the daemon puts between an untrusted offset and a
    // pointer, so a kind check tested through anything else would be testing a
    // different function than the one that matters.
    auto validates = [&](u64 r, u32 kind, u64 need, const char** why, u64* out) {
        return ipc::poolValidate(p.base(), p.bytes(), p.header(), r, kind, need, why, out);
    };

    // A sampler's real spelling, escapes and all.
    const char* kState = "nxsmp1;p=%2Ftmp%2Fkick%20one.wav";
    ipc::WireDeviceState h{};
    const u64 ref = p.writeDeviceState(h, kState);
    CHECK(ref != 0, "a state blob at %llu", (unsigned long long)ref);
    if (!ref) { c.closePool(); return; }

    const char* why = "";
    u64 blockBytes = 0;
    CHECK(validates(ref, ipc::PoolKindDeviceState, sizeof(ipc::WireDeviceState),
                    &why, &blockBytes),
          "it validates as a device state (%s)", why);
    // Evaluate BEFORE the CHECK: `why` is an out-parameter of the call, and a
    // CHECK argument is not sequenced against the call in its condition -- the
    // message would quote the PREVIOUS reason (section 12 hit this first).
    const bool notString = !validates(ref, ipc::PoolKindString, 1, &why, nullptr);
    CHECK(notString, "and NOT as a string: %s", why);
    const bool notRack = !validates(ref, ipc::PoolKindRackState, 1, &why, nullptr);
    CHECK(notRack, "and NOT as a rack state: %s", why);

    ipc::WireDeviceState got{};
    std::memcpy(&got, p.data<u8>(ref), sizeof got);
    CHECK(got.version == ipc::kDeviceStateVersion, "the header says v%u", got.version);
    CHECK(got.textBytes == std::strlen(kState) + 1,
          "textBytes counts the terminator (%u == %zu)",
          got.textBytes, std::strlen(kState) + 1);
    CHECK(got.audioRef == 0, "no sample named");
    const char* text = (const char*)p.data<u8>(ref) + sizeof(ipc::WireDeviceState);
    CHECK(!std::strcmp(text, kState),
          "and the TEXT IS THE PERSISTED SPELLING, byte for byte ('%s')", text);

    // The header's counts are recomputed from the string, so a caller that
    // builds a header disagreeing with what it hands over cannot publish one.
    ipc::WireDeviceState lying{};
    lying.textBytes = 9999;
    lying.version   = 77;
    const u64 ref2 = p.writeDeviceState(lying, "x");
    CHECK(ref2 != 0, "a second blob from a header that lies about its own text");
    if (ref2) {
        ipc::WireDeviceState g2{};
        std::memcpy(&g2, p.data<u8>(ref2), sizeof g2);
        CHECK(g2.textBytes == 2 && g2.version == ipc::kDeviceStateVersion,
              "the writer recomputes textBytes and stamps the version (%u, v%u)",
              g2.textBytes, g2.version);
    }

    // Over-long: refused, never truncated.
    std::string huge((size_t)ipc::kMaxDeviceState, 'z');    // + NUL is one past
    CHECK(p.writeDeviceState(h, huge.c_str()) == 0,
          "a state one byte past kMaxDeviceState is refused rather than truncated");
    std::string atMax((size_t)ipc::kMaxDeviceState - 1, 'z');
    const u64 ref3 = p.writeDeviceState(h, atMax.c_str());
    CHECK(ref3 != 0, "and one exactly at the bound is taken");

    // The second block. A sampler's audio is an ORDINARY PoolKindSamples block,
    // so it goes through the allocator, the validation and the free-after-confirm
    // bookkeeping a clip's does -- that is the whole reason it is not carried
    // inside the state blob.
    std::vector<f32> dc((size_t)512 * 2, 0.5f);
    const u64 aref = c.poolWrite(dc.data(), 512, 2, 48000.0, 0);
    CHECK(aref != 0, "the audio is an ordinary sample block (%llu)",
          (unsigned long long)aref);
    ipc::WireDeviceState wa{};
    wa.audioRef      = aref;
    wa.audioFrames   = 512;
    wa.audioChannels = 2;
    wa.audioRate     = 48000.0;
    const u64 ref4 = p.writeDeviceState(wa, kState);
    CHECK(ref4 != 0, "and a state that names it");
    if (ref4) {
        ipc::WireDeviceState g4{};
        std::memcpy(&g4, p.data<u8>(ref4), sizeof g4);
        CHECK(g4.audioRef == aref && g4.audioFrames == 512 && g4.audioChannels == 2,
              "the shape crosses beside the offset, so the far side never has to "
              "trust the block's own frame count alone");
        CHECK(validates(g4.audioRef, ipc::PoolKindSamples,
                        (u64)512 * 2 * sizeof(f32), &why, nullptr),
              "and the block it names validates as samples (%s)", why);
    }

    // FREE-AFTER-CONFIRM, both blocks, the AddDevice dance run twice.
    p.markLive(ref4); p.markLive(aref);
    p.markDisplaced(ref4); p.release(ref4);
    p.markDisplaced(aref); p.release(aref);
    CHECK(p.stateOf(ref4) == ipc::BlockRetiring && p.stateOf(aref) == ipc::BlockRetiring,
          "both blocks are Retiring while the daemon has not answered (%s, %s)",
          ipc::poolStateName(p.stateOf(ref4)), ipc::poolStateName(p.stateOf(aref)));
    ipc::WireEvent e{};
    e.type = ipc::EvBlockRetired; e.ref = ref4; c.observe(e);
    e.ref = aref;                                c.observe(e);
    CHECK(p.stateOf(ref4) == ipc::BlockFree && p.stateOf(aref) == ipc::BlockFree,
          "and both are freed by their own echoes (%s, %s)",
          ipc::poolStateName(p.stateOf(ref4)), ipc::poolStateName(p.stateOf(aref)));

    c.poolRelease(ref);
    c.poolRelease(ref2);
    c.poolRelease(ref3);
    c.closePool();
    CHECK(!shmExistsPool(session), "the device-state pool is unlinked");
    gShmNameAlt[0] = '\0';
}

// ---------------------------------------------------------------------------
// 12. the clip's other three pointers  (GUI-ON-DAEMON.md §17)
// ---------------------------------------------------------------------------
//
// v11 gave RtClip's last three pointers a wire spelling: `autos`, `markers` and
// `transients` became autoRef/markersRef/transientsRef with four counts beside
// noteCount, and PoolKindAutomation stopped being reserved while PoolKindWarp
// and PoolKindTransients were born. Until then a Beats-warped clip crossed
// without the map that shapes it, a clip's envelopes did not cross at all, and
// every audio clip under the default engine counted an amber refusal — three
// silent failures, so the shapes, the kinds and the retirement are pinned here
// in one process with no daemon, exactly as §11 pins the device state's.
//
// The properties a wrong answer would make silent:
//   * the CELL is 176 bytes and its size is folded into the region layout hash.
//     A field added without a version bump would let two builds read one clip
//     table differently.
//   * the three blobs are THREE KINDS, and two of them are flat pointer-free
//     arrays that a reader would happily cast either way — a transient grid
//     read as a warp map is every second onset reinterpreted as a beat.
//     Nothing but `kind` stands between them.
//   * the envelope blob is HEADERLESS: lanes then points, counts off the cell.
//     Its arithmetic therefore has to hold for an ODD lane count, or the point
//     array lands misaligned for exactly the clips nobody tests.
//   * free-after-confirm reaches all three. A ref marked live and never
//     confirmed is a leak; one freed early is a voice reading somebody else's
//     clip.
//
// NOT here: the cell flow itself. writeCell() refuses without an attached
// daemon (its first line) and clipShadow() only moves when a write lands, so
// "one setClip marks all five refs Live" cannot be observed from a pool-only
// process — daemon_test owns that half live. What this section owns is
// everything up to the ring.

static void testClipBlobs() {
    banner("12. the whole clip: envelopes, warp markers and a transient grid");

    // -- shapes first. sizeof(WireClip) is part of the layout hash, so a change
    //    here is a change two builds must not be able to disagree about.
    CHECK(sizeof(ipc::WireClip) == 176, "WireClip is %zu B", sizeof(ipc::WireClip));
    CHECK(ipc::kProtocolVersion == 12 && ipc::kPoolVersion == 9,
          "at protocol v%u over pool v%u", ipc::kProtocolVersion, ipc::kPoolVersion);
    CHECK(sizeof(ipc::WireWarpMarker) == 16 && alignof(ipc::WireWarpMarker) == 8,
          "WireWarpMarker is %zu B: one source frame pinned to one beat",
          sizeof(ipc::WireWarpMarker));

    // A half-filled cell must claim NOTHING. defaultWireClip is what a client
    // starts from, and a stray ref in it would be an offset the daemon
    // validates against a block the client never wrote.
    const ipc::WireClip d = ipc::defaultWireClip();
    CHECK(d.autoRef == 0 && d.markersRef == 0 && d.transientsRef == 0,
          "a default clip names NO blob: all three new refs are zero");
    CHECK(d.autoLaneCount == 0 && d.autoPointCount == 0 &&
          d.markerCount == 0 && d.transientCount == 0,
          "and all four new counts are zero, so the extent it claims is zero too");

    // The bounds are protocol, not editor policy: each count is a multiply
    // operand for a byte extent and an index bound the engine will trust.
    CHECK(ipc::kMaxClipAutoLanes == 16 && ipc::kMaxClipAutoPoints == 4096 &&
          ipc::kMaxWarpMarkers == 8192 && ipc::kMaxWireTransients == 8192,
          "the four ceilings are %lld lanes, %lld points, %lld markers, %lld onsets",
          (long long)ipc::kMaxClipAutoLanes, (long long)ipc::kMaxClipAutoPoints,
          (long long)ipc::kMaxWarpMarkers,   (long long)ipc::kMaxWireTransients);
    CHECK(ipc::RejectBadAutomation == 15,
          "and RejectBadAutomation is live at %u: '%s'",
          (unsigned)ipc::RejectBadAutomation,
          ipc::rejectReasonName(ipc::RejectBadAutomation));

    // No header term: the clip blob is trackAutosBytes minus the 16 the track
    // blob spends stating counts the cell already states.
    CHECK(ipc::clipAutosBytes(2, 3) ==
              2 * sizeof(ipc::WireAutoLane) + 3 * sizeof(ipc::WireAutoPoint),
          "clipAutosBytes(2,3) is %llu B — two arrays and NO header",
          (unsigned long long)ipc::clipAutosBytes(2, 3));
    CHECK(ipc::clipAutosBytes(2, 3) == 128, "which is 2*40 + 3*16");
    CHECK(ipc::clipAutosBytes(0, 0) == 0, "and a set with nothing in it is 0 B");

    // The odd-lane case is the one that bites, and it is why WireAutoLane
    // spells out a trailing pad word instead of inheriting its size from the
    // ABI: RtAutoLane's real size is 36, and 36 * an odd count would leave the
    // f64-leading point array on a 4-byte boundary. control.h asserts this at
    // compile time; saying it out loud here is what tells a reader why the pad
    // may not be deleted.
    CHECK(sizeof(ipc::WireAutoLane) == 40, "WireAutoLane is %zu B, pad word and all",
          sizeof(ipc::WireAutoLane));
    CHECK((7 * sizeof(ipc::WireAutoLane)) % alignof(ipc::WireAutoPoint) == 0,
          "so an ODD lane count still lands the points aligned (7*%zu %% %zu == 0)",
          sizeof(ipc::WireAutoLane), alignof(ipc::WireAutoPoint));

    CHECK(ipc::PoolKindAutomation == 4 && ipc::PoolKindWarp == 10 &&
          ipc::PoolKindTransients == 11,
          "the three kinds hold distinct numbers (%d, %d, %d)",
          (int)ipc::PoolKindAutomation, (int)ipc::PoolKindWarp,
          (int)ipc::PoolKindTransients);
    CHECK(!std::strcmp(ipc::poolKindName(ipc::PoolKindAutomation), "automation") &&
          !std::strcmp(ipc::poolKindName(ipc::PoolKindWarp), "warp-markers") &&
          !std::strcmp(ipc::poolKindName(ipc::PoolKindTransients), "transients"),
          "and each names itself in a log line ('%s', '%s', '%s')",
          ipc::poolKindName(ipc::PoolKindAutomation),
          ipc::poolKindName(ipc::PoolKindWarp),
          ipc::poolKindName(ipc::PoolKindTransients));

    char session[48];
    std::snprintf(session, sizeof session, "ipc-clipblobs-%d", (int)::getpid());
    std::snprintf(gShmNameAlt, sizeof gShmNameAlt, "nxtakt-pool-%s", session);

    ipc::EngineClient c;
    if (!c.createPool(session, 4u << 20)) { CHECK(false, "createPool: %s", c.error()); return; }
    ipc::SamplePool& p = c.pool();

    // Through the READER's entry point, deliberately, for §11's reason: this is
    // the one function the daemon puts between an untrusted offset and a
    // pointer, so a kind check made anywhere else would be checking a different
    // function than the one that matters.
    // The reason string lands in `why` as an OUT-PARAMETER of the call, and an
    // argument is not sequenced against the call that fills it — quoting it
    // inline in the same CHECK prints the PREVIOUS blob's reason. So every
    // validation below lands in `ok` first and the message reads `why` after.
    const char* why = "";
    bool ok = false;
    auto validates = [&](u64 r, u32 kind, u64 need) {
        ok = ipc::poolValidate(p.base(), p.bytes(), p.header(), r, kind, need, &why, nullptr);
        return ok;
    };

    // What a detached client's shadow says. It is the republish source, so a
    // ref in a cell that was never published would be an offset re-sent into a
    // pool that no longer holds it — the one half of the cell flow that IS
    // observable without a daemon.
    const ipc::WireClip& sh = c.clipShadow(0, 0);
    CHECK(sh.autoRef == 0 && sh.markersRef == 0 && sh.transientsRef == 0 && sh.valid == 0,
          "an unpublished cell's shadow names none of the three new blobs");

    // -- the envelopes. THREE lanes on purpose: odd, so the point array's
    //    alignment is exercised by the real writer and not only by arithmetic.
    ipc::WireAutoLane lanes[3]{};
    for (int i = 0; i < 3; ++i) {
        lanes[i].target = i + 1;
        lanes[i].first  = i * 2;
        lanes[i].count  = (i == 2) ? 1 : 2;
        lanes[i].lo     = 0.0f;
        lanes[i].hi     = 1.0f;
    }
    ipc::WireAutoPoint pts[5]{};
    for (int i = 0; i < 5; ++i) { pts[i].beat = i * 0.5; pts[i].value = 0.25f * (f32)i; }

    const u64 aref = c.poolWriteClipAutos(lanes, 3, pts, 5);
    CHECK(aref != 0, "an envelope blob at %llu (3 lanes, 5 points)",
          (unsigned long long)aref);
    if (!aref) { c.closePool(); gShmNameAlt[0] = '\0'; return; }

    validates(aref, ipc::PoolKindAutomation, ipc::clipAutosBytes(3, 5));
    CHECK(ok, "it validates as clip envelopes at the extent the CELL would claim (%s)", why);
    validates(aref, ipc::PoolKindString, 1);
    CHECK(!ok, "and NOT as a string: '%s'", why);
    validates(aref, ipc::PoolKindTrackAutos, 1);
    CHECK(!ok, "and NOT as TRACK automation — different container, different builder: '%s'", why);

    // Layout, read back at hand-computed offsets rather than through a struct
    // that would hide the question: LANES FIRST, then points, nothing between.
    const u8* ab = p.data<u8>(aref);
    CHECK(ab != nullptr, "the blob reads back through the writer's own handle");
    if (ab) {
        ipc::WireAutoLane gl{};
        std::memcpy(&gl, ab + 2 * sizeof(ipc::WireAutoLane), sizeof gl);
        CHECK(gl.target == 3 && gl.first == 4 && gl.count == 1,
              "the third lane sits at 2*40 (target %d, window %d+%d)",
              gl.target, gl.first, gl.count);
        ipc::WireAutoPoint gp{};
        std::memcpy(&gp, ab + 3 * sizeof(ipc::WireAutoLane) + 4 * sizeof(ipc::WireAutoPoint),
                    sizeof gp);
        CHECK(gp.beat == 2.0 && gp.value == 1.0f,
              "and the fifth point at 3*40 + 4*16 (beat %.2f, value %.2f)",
              gp.beat, (f64)gp.value);
    }
    note("the block's own frame field holds the lane count, but the daemon reads "
         "the counts off the CELL — a block field is a number the client chose");

    // Lanes with no points is a legal set (a lane that is declared and empty);
    // NO lanes is not, and the writer says so before it allocates.
    const u64 aref2 = c.poolWriteClipAutos(lanes, 3, nullptr, 0);
    CHECK(aref2 != 0, "lanes with no points is still a set (%llu)",
          (unsigned long long)aref2);
    CHECK(c.poolWriteClipAutos(lanes, 0, pts, 5) == 0,
          "but a set with NO LANES is refused: it addresses nothing");
    CHECK(c.poolWriteClipAutos(lanes, -1, pts, 5) == 0,
          "and a negative lane count is refused before it becomes a memcpy length");

    // -- the warp map.
    ipc::WireWarpMarker mk[4]{};
    for (int i = 0; i < 4; ++i) { mk[i].srcFrame = (i64)i * 24000; mk[i].beat = (f64)i; }
    const u64 wref = c.poolWriteWarp(mk, 4);
    CHECK(wref != 0, "a warp map at %llu (4 markers)", (unsigned long long)wref);
    validates(wref, ipc::PoolKindWarp, 4 * sizeof(ipc::WireWarpMarker));
    CHECK(ok, "it validates as warp markers (%s)", why);
    validates(wref, ipc::PoolKindNotes, 1);
    CHECK(!ok, "and NOT as notes: '%s'", why);
    validates(wref, ipc::PoolKindSamples, 1);
    CHECK(!ok, "and NOT as sample data: '%s'", why);
    const ipc::WireWarpMarker* gm = p.data<ipc::WireWarpMarker>(wref);
    CHECK(gm != nullptr, "the map reads back as a marker array");
    if (gm)
        CHECK(gm[3].srcFrame == 72000 && gm[3].beat == 3.0,
              "REINTERPRETED, not translated: the last pin is frame %lld at beat %.1f",
              (long long)gm[3].srcFrame, gm[3].beat);

    // -- the transient grid. Its key is the SAMPLE's content key, because its
    //    lifetime is the sample's: built once at load, shared by every clip
    //    over that sample, so two such clips must name ONE block.
    const u64 kKey = 0xC0FFEEull;
    const i64 onsets[6] = {0, 12000, 24500, 36000, 48000, 60000};
    const u64 tref = c.poolWriteTransients(onsets, 6, kKey);
    CHECK(tref != 0, "a transient grid at %llu (6 onsets)", (unsigned long long)tref);
    validates(tref, ipc::PoolKindTransients, 6 * sizeof(i64));
    CHECK(ok, "it validates as a transient grid (%s)", why);
    validates(tref, ipc::PoolKindSamples, 1);
    CHECK(!ok, "and NOT as sample data: '%s'", why);
    validates(tref, ipc::PoolKindWarp, 1);
    CHECK(!ok, "and NOT as a warp map — that cast would read every second onset as a "
          "BEAT: '%s'", why);
    const ipc::PoolBlock* tb = p.blockAt(tref);
    CHECK(tb != nullptr && tb->key == kKey,
          "the grid carries the SAMPLE's content key (0x%llx), because its "
          "lifetime is the sample's and not the clip's",
          (unsigned long long)(tb ? tb->key : 0));
    // And the dedup path must SEE it. Regression: findByKey's arena walk used
    // to guard on a further header fitting under the high-water mark, so a
    // trailing block shorter than 128 B — which this six-onset grid is, and it
    // is the last block in the arena right here — was invisible to the lookup,
    // and a second clip over the same sample would allocate a duplicate grid.
    CHECK(p.findByKey(kKey) == tref,
          "findByKey(0x%llx) finds the grid even as a small TRAILING block (%llu)",
          (unsigned long long)kKey, (unsigned long long)p.findByKey(kKey));
    const i64* gt = p.data<i64>(tref);
    CHECK(gt != nullptr, "the grid reads back as i64[]");
    if (gt)
        CHECK(gt[2] == 24500 && gt[5] == 60000,
              "onsets in source frames, strictly increasing (%lld, %lld)",
              (long long)gt[2], (long long)gt[5]);

    // The writers refuse only what is theirs to refuse — "nothing to write".
    CHECK(c.poolWriteWarp(nullptr, 4) == 0 && c.poolWriteWarp(mk, 0) == 0,
          "a warp write with no markers is 0, not an empty block");
    CHECK(c.poolWriteTransients(nullptr, 6) == 0 && c.poolWriteTransients(onsets, -3) == 0,
          "and a transient write with nothing to write is 0 as well");

    // The CEILINGS are the daemon's, not the allocator's, and that split is
    // deliberate: the bound has to be enforced where the untrusted number
    // becomes a pointer, and enforcing it twice invites the two copies to
    // drift. So the client can build a map it will be told off for — and being
    // told off is a rejected cell with a reason, not a truncated map.
    std::vector<ipc::WireWarpMarker> over((size_t)ipc::kMaxWarpMarkers + 1);
    const u64 oref = c.poolWriteWarp(over.data(), (i64)over.size());
    CHECK(oref != 0,
          "a map one past kMaxWarpMarkers is still WRITTEN (%llu): the ceiling "
          "lives on the daemon's side of the wire", (unsigned long long)oref);
    if (oref) c.poolRelease(oref);

    // FREE-AFTER-CONFIRM, all three refs, §11's step 9 run on the new kinds.
    // The rule does not care what a block holds — it cares that the engine may
    // still be reading it — and v11 added three new ways to forget to ask.
    p.markLive(aref); p.markLive(wref); p.markLive(tref);
    CHECK(p.stateOf(aref) == ipc::BlockLive && p.stateOf(wref) == ipc::BlockLive &&
          p.stateOf(tref) == ipc::BlockLive,
          "published: envelopes, map and grid are all Live");
    p.markDisplaced(aref); c.poolRelease(aref);
    p.markDisplaced(wref); c.poolRelease(wref);
    p.markDisplaced(tref); c.poolRelease(tref);
    CHECK(p.stateOf(aref) == ipc::BlockRetiring && p.stateOf(wref) == ipc::BlockRetiring &&
          p.stateOf(tref) == ipc::BlockRetiring,
          "all three are Retiring while the daemon has not answered (%s, %s, %s)",
          ipc::poolStateName(p.stateOf(aref)), ipc::poolStateName(p.stateOf(wref)),
          ipc::poolStateName(p.stateOf(tref)));
    ipc::WireEvent e{};
    e.type = ipc::EvBlockRetired;
    e.ref = aref; c.observe(e);
    e.ref = wref; c.observe(e);
    e.ref = tref; c.observe(e);
    CHECK(p.stateOf(aref) == ipc::BlockFree && p.stateOf(wref) == ipc::BlockFree &&
          p.stateOf(tref) == ipc::BlockFree,
          "and each is freed by its OWN echo, none by another's (%s, %s, %s)",
          ipc::poolStateName(p.stateOf(aref)), ipc::poolStateName(p.stateOf(wref)),
          ipc::poolStateName(p.stateOf(tref)));

    note("the cell flow — one setClip marking all five refs Live and displacing "
         "five old ones on the ack — needs an attached daemon; daemon_test owns it");

    c.poolRelease(aref2);
    c.closePool();
    CHECK(!shmExistsPool(session), "the clip-blob pool is unlinked");
    gShmNameAlt[0] = '\0';
}


// ---------------------------------------------------------------------------
// 13. THE WAVETABLE BLOB — PoolKindWavetable, pool v9 / protocol v12
//
// A custom wavetable crosses beside a device state exactly as a sampler's audio
// does, and is named DIFFERENTLY: audio is named by a shape, a wavetable is
// named by a CONTENT HASH that is a function of its own samples. Everything
// that is interesting about this payload follows from that one difference, and
// this section is where the wire half of it is checked in one process.
//
// THE HASH IS TRANSCRIBED HERE, from docs/SPECTRA-PARAMS.md, "The content
// hash", and NOT taken from lat::wt::contentHash. That is the point: ipc_test
// links src/ipc and libc and nothing else, on purpose, so the number this file
// computes is an INDEPENDENT reading of the specification. daemon_test then
// sends a blob stamped with it to a real nxtaktd, whose wt::ingest() recomputes
// the hash with the real implementation and refuses anything that disagrees —
// so "the doc, the test and the code agree" is a thing the suite proves rather
// than a thing it assumes.
// ---------------------------------------------------------------------------

static u64 wtMix64(u64 x) {
    x += 0x9E3779B97F4A7C15ull;
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27; x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return x;
}

// h = mix64(frameCount); h = mix64(h ^ 2048); then every sample's float32 bits,
// frame-major, with -0.0 folded to +0.0.
static u64 wtHash(const f32* frames, int frameCount, int cycle = 2048) {
    u64 h = wtMix64((u64)(u32)frameCount);
    h = wtMix64(h ^ (u64)(u32)cycle);
    const size_t n = (size_t)frameCount * (size_t)cycle;
    for (size_t i = 0; i < n; ++i) {
        u32 u;
        std::memcpy(&u, &frames[i], sizeof u);
        if (u == 0x80000000u) u = 0;
        h = wtMix64(h ^ (u64)u);
    }
    return h;
}

static void testWavetableBlob() {
    banner("13. the wavetable blob: identity, shape, kinds and free-after-confirm");

    // -- the shapes, which are part of the layout hash's promise -------------
    CHECK(ipc::PoolKindWavetable == 12, "PoolKindWavetable is %u",
          (unsigned)ipc::PoolKindWavetable);
    CHECK(std::strcmp(ipc::poolKindName(ipc::PoolKindWavetable), "wavetables") == 0,
          "and names itself '%s' in a log line", ipc::poolKindName(ipc::PoolKindWavetable));
    CHECK(sizeof(ipc::WireWavetable) == 16 && alignof(ipc::WireWavetable) == 8,
          "WireWavetable is %zu B: a hash, a frame count and a cycle length",
          sizeof(ipc::WireWavetable));
    CHECK(ipc::kMaxWavetables == 8 && ipc::kMaxWavetableBytes == (4ull << 20),
          "at most %u tables in a block of at most %llu bytes",
          ipc::kMaxWavetables, (unsigned long long)ipc::kMaxWavetableBytes);
    // The worst case the format admits has to FIT the bound, or the bound is a
    // truncation waiting for the set that reaches it.
    const u64 worst = (u64)ipc::kMaxWavetables * (16ull + 32ull * 2048ull * 4ull);
    CHECK(worst < ipc::kMaxWavetableBytes,
          "and the worst case the format admits (%llu B) fits inside it",
          (unsigned long long)worst);

    // -- the hash, against the properties the contract states ---------------
    std::vector<f32> a((size_t)2 * 2048, 0.f);
    for (int fr = 0; fr < 2; ++fr)
        for (int i = 0; i < 2048; ++i)
            a[(size_t)fr * 2048 + i] =
                (f32)std::sin(6.283185307179586 * (f64)(fr + 1) * (f64)i / 2048.0);
    const u64 ha = wtHash(a.data(), 2);
    CHECK(ha != 0, "a two-frame table hashes to %016llx", (unsigned long long)ha);
    CHECK(wtHash(a.data(), 2) == ha, "and the same samples hash the same way twice");

    std::vector<f32> b = a;
    b[1234] = std::nextafterf(b[1234], 1.f);
    CHECK(wtHash(b.data(), 2) != ha,
          "one sample moved by one ULP is a different table");

    std::vector<f32> z((size_t)2048, 0.f), nz((size_t)2048, -0.f);
    CHECK(wtHash(z.data(), 1) == wtHash(nz.data(), 1),
          "-0.0 folds to +0.0: DC removal produces it and two tables that differ "
          "only in the sign of a zero are the same table");
    CHECK(wtHash(a.data(), 1) != ha,
          "and the FRAME COUNT is in the fold, so a prefix is not the same table");

    // -- the block ----------------------------------------------------------
    char session[48];
    std::snprintf(session, sizeof session, "ipc-wt-%d", (int)::getpid());
    std::snprintf(gShmNameAlt, sizeof gShmNameAlt, "nxtakt-pool-%s", session);

    ipc::EngineClient c;
    if (!c.createPool(session, 8u << 20)) { CHECK(false, "createPool: %s", c.error()); return; }
    ipc::SamplePool& p = c.pool();

    const char* why = "";
    bool okv = false;
    auto validates = [&](u64 r, u32 kind, u64 need) {
        okv = ipc::poolValidate(p.base(), p.bytes(), p.header(), r, kind, need, &why, nullptr);
        return okv;
    };

    ipc::WireWavetable ent[2]{};
    ent[0].hash = ha;   ent[0].frames = 2; ent[0].cycle = 2048;
    ent[1].hash = wtHash(z.data(), 1); ent[1].frames = 1; ent[1].cycle = 2048;
    const f32* dat[2] = { a.data(), z.data() };

    const u64 wref = p.writeWavetables(ent, dat, 2);
    CHECK(wref != 0, "a two-table blob at %llu", (unsigned long long)wref);
    if (!wref) { c.closePool(); return; }

    CHECK(validates(wref, ipc::PoolKindWavetable, 2 * sizeof(ipc::WireWavetable)),
          "it validates as wavetables (%s)", why);
    const bool notSamples = !validates(wref, ipc::PoolKindSamples, 1);
    CHECK(notSamples, "and NOT as sample data: %s", why);
    const bool notState = !validates(wref, ipc::PoolKindDeviceState, 1);
    CHECK(notState, "and NOT as a device state: %s", why);
    const bool notString = !validates(wref, ipc::PoolKindString, 1);
    CHECK(notString, "and NOT as a string: %s", why);

    // The mirror refusal, which is the one that matters for a payload this
    // close in shape to another: a SAMPLE block must not pass as wavetables.
    std::vector<f32> pcm((size_t)512, 0.25f);
    const u64 sref = p.writeSamples(pcm.data(), 256, 2, 48000.0, 0);
    CHECK(sref != 0, "a sample block at %llu", (unsigned long long)sref);
    const bool sampleNotWt = !validates(sref, ipc::PoolKindWavetable, 1);
    CHECK(sampleNotWt, "and a sample block does NOT pass as wavetables: %s", why);

    // The block's own count is the entry count, exactly as a note array's is.
    CHECK(p.blockAt(wref)->frames == 2,
          "the block's `frames` is the TABLE COUNT (%lld), the note array's rule",
          (long long)p.blockAt(wref)->frames);

    // The payload reads back byte for byte. It has to: these floats are hashed
    // on the far side and a single changed bit is a different table.
    const ipc::WireWavetable* rd = p.data<ipc::WireWavetable>(wref);
    CHECK(rd[0].hash == ha && rd[0].frames == 2 && rd[0].cycle == 2048 &&
          rd[1].hash == ent[1].hash && rd[1].frames == 1,
          "both entries read back with their identity and shape");
    const f32* run = (const f32*)(p.data<u8>(wref) + 2 * sizeof(ipc::WireWavetable));
    CHECK(std::memcmp(run, a.data(), a.size() * sizeof(f32)) == 0,
          "table 0's %zu samples are byte-identical in the pool", a.size());
    CHECK(std::memcmp(run + a.size(), z.data(), z.size() * sizeof(f32)) == 0,
          "and table 1's follow it immediately, in entry order");
    CHECK(wtHash(run, 2) == ha,
          "and the pool-resident copy hashes to the identity it was written under");

    // -- refusals, at the bound rather than by truncation --------------------
    ipc::WireWavetable many[ipc::kMaxWavetables + 1]{};
    const f32* manyd[ipc::kMaxWavetables + 1]{};
    for (u32 i = 0; i <= ipc::kMaxWavetables; ++i) {
        many[i].hash = i + 1; many[i].frames = 1; many[i].cycle = 2048;
        manyd[i] = z.data();
    }
    CHECK(p.writeWavetables(many, manyd, ipc::kMaxWavetables + 1) == 0,
          "a block naming %u tables is REFUSED, not truncated to %u",
          ipc::kMaxWavetables + 1, ipc::kMaxWavetables);
    CHECK(p.writeWavetables(many, manyd, ipc::kMaxWavetables) != 0,
          "and exactly %u is accepted: the bound is a bound and not an off-by-one",
          ipc::kMaxWavetables);
    ipc::WireWavetable one = ent[0];
    const f32* none[1] = { nullptr };
    CHECK(p.writeWavetables(&one, none, 1) == 0, "a table with no samples is refused");
    one.frames = 0;
    const f32* good[1] = { a.data() };
    CHECK(p.writeWavetables(&one, good, 1) == 0, "and a table with no frames is refused");
    one = ent[0];
    one.frames = 4096;                  // past kMaxWavetableBytes on its own
    CHECK(p.writeWavetables(&one, good, 1) == 0,
          "and a single table larger than the whole bound is refused");

    // -- the device state that names it -------------------------------------
    ipc::WireDeviceState h{};
    h.wtRef   = wref;
    h.wtCount = 2;
    const char* kState = "nxspc1;wtA=0123456789abcdef;wtpathA=%2Ftmp%2Ft.wav";
    const u64 dref = p.writeDeviceState(h, kState);
    CHECK(dref != 0, "a device state naming the blob at %llu", (unsigned long long)dref);
    ipc::WireDeviceState back{};
    std::memcpy(&back, p.data<u8>(dref), sizeof back);
    CHECK(back.version == ipc::kDeviceStateVersion && back.wtRef == wref &&
          back.wtCount == 2 && back.audioRef == 0,
          "and the header carries wtRef/wtCount through, with no audio beside them");
    CHECK(back.textBytes == (u32)std::strlen(kState) + 1,
          "with the text length RECOMPUTED from the string (%u)", back.textBytes);

    // -- FREE-AFTER-CONFIRM, all three blocks -------------------------------
    p.markLive(dref); p.markLive(wref); p.markLive(sref);
    p.markDisplaced(dref); p.release(dref);
    p.markDisplaced(wref); p.release(wref);
    p.markDisplaced(sref); p.release(sref);
    CHECK(p.stateOf(dref) == ipc::BlockRetiring && p.stateOf(wref) == ipc::BlockRetiring &&
          p.stateOf(sref) == ipc::BlockRetiring,
          "all three are Retiring while the daemon has not answered (%s, %s, %s)",
          ipc::poolStateName(p.stateOf(dref)), ipc::poolStateName(p.stateOf(wref)),
          ipc::poolStateName(p.stateOf(sref)));
    ipc::WireEvent e{};
    e.type = ipc::EvBlockRetired;
    e.ref = dref; c.observe(e);
    e.ref = wref; c.observe(e);
    CHECK(p.stateOf(dref) == ipc::BlockFree && p.stateOf(wref) == ipc::BlockFree &&
          p.stateOf(sref) == ipc::BlockRetiring,
          "the two that were echoed are freed and the third is NOT (%s, %s, %s) — "
          "a block comes home on its OWN offset",
          ipc::poolStateName(p.stateOf(dref)), ipc::poolStateName(p.stateOf(wref)),
          ipc::poolStateName(p.stateOf(sref)));
    e.ref = sref; c.observe(e);
    CHECK(p.stateOf(sref) == ipc::BlockFree, "and the third on its own echo");

    c.closePool();
    CHECK(!shmExistsPool(session), "the wavetable pool is unlinked");
    gShmNameAlt[0] = '\0';
}

// ---------------------------------------------------------------------------

int main() {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);   // survives fork() without duplicating output
    std::printf("nxtakt ipc tests  (protocol v%u, %zu-byte region, %u-slot rings)\n",
                ipc::kShmVersion, layout::kBytes, CmdRing::capacity());

    std::snprintf(gRunTag, sizeof gRunTag, "-%d", (int)::getpid());
    std::snprintf(gShmName,    sizeof gShmName,    "nxtakt-ipc-test-%d",     (int)::getpid());
    std::snprintf(gShmNameAlt, sizeof gShmNameAlt, "nxtakt-ipc-test-%d-alt", (int)::getpid());
    armCleanup();

    testRegionBasics();
    testBackpressure();
    testCrossProcess();
    testStateSeqlock();
    testStaleReap();
    testForgedRetirement();
    testAdoptedPoolLiveness();
    testArrangementClassifiers();
    testJournalRingCrossProcess();
    testTakeFiles();
    testDeviceStateBlob();
    testClipBlobs();
    testWavetableBlob();

    banner("14. /dev/shm is clean");
    cleanupShm();
    const int leftover = countNxTaktShm();
    CHECK(leftover == 0, "no nxtakt region left in /dev/shm (found %d)", leftover);

    std::printf("\n----------------------------------------\n");
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
