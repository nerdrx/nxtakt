// EngineHandle: both paths. See engine_handle.h for the shape and
// docs/GUI-ON-DAEMON.md §2 for why it is a concrete class rather than an
// interface.
//
// The file is in three parts: the local path (unchanged from step 1), the
// RemoteEngine that step 2 and step 3 add, and the dispatch between them.
#include "engine_handle.h"
#include "../ipc/client.h"
// SampleBuffer as a COMPLETE type. The Makefile's note on build/handle_test says
// the seam links no sndfile "because the handle is the seam and the seam has no
// business knowing how a wav is decoded", and that still holds: this is a
// header, nothing here calls loadSample(), buildPeaks() or buildTransients(),
// and the seam still does not know how a wav is decoded. What it now has to
// know is how a DECODED one is SHAPED, because a sampler's audio has to reach a
// daemon that links no decoder either (GUI-ON-DAEMON.md §15.2).
#include "../audio/sample.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>

#include <cerrno>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace lat {

// ===========================================================================
// RemoteEngine — the daemon path
// ===========================================================================
//
// Everything ipc-shaped lives in here so that engine_handle.h can stay free of
// src/ipc and the view translation units with it.
//
// THE THREE JOBS
// --------------
//   1. Own the link: reap, attach or spawn-and-attach, create the sample pool,
//      and stop a daemon we started when we go (§6, parent-of-record).
//   2. Translate. A Command carrying an RtClip full of GUI-heap pointers has to
//      become a WireClip full of pool offsets. That is what samples_/notes_
//      below are for and it is the whole of step 3.
//   3. Stand in for the engine's retirement events. `pushClip()` hands the
//      engine a fresh RtNote[] and waits for Ev::NotesRetired before freeing the
//      one it displaced; there is no engine here to send it, and the array never
//      crossed the boundary in the first place (it was COPIED into the pool). So
//      this object keeps the same per-cell "last published" table the engine
//      keeps and synthesises the event. Without it App::retiringNotes_ grows for
//      the life of the session and nothing ever comes home.

namespace {

// The command names the refusal log prints. A switch rather than a table so
// that adding a Cmd without adding a name fails the -Wswitch build.
const char* cmdName(Cmd t) {
    switch (t) {
        case Cmd::SetPlaying:        return "SetPlaying";
        case Cmd::SetTempo:          return "SetTempo";
        case Cmd::SetQuantum:        return "SetQuantum";
        case Cmd::SetMetronome:      return "SetMetronome";
        case Cmd::LaunchClip:        return "LaunchClip";
        case Cmd::StopTrack:         return "StopTrack";
        case Cmd::LaunchScene:       return "LaunchScene";
        case Cmd::StopAll:           return "StopAll";
        case Cmd::SetClip:           return "SetClip";
        case Cmd::ClearClip:         return "ClearClip";
        case Cmd::TrackVol:          return "TrackVol";
        case Cmd::TrackPan:          return "TrackPan";
        case Cmd::TrackMute:         return "TrackMute";
        case Cmd::TrackSolo:         return "TrackSolo";
        case Cmd::TrackArm:          return "TrackArm";
        case Cmd::MasterVol:         return "MasterVol";
        case Cmd::ClipGain:          return "ClipGain";
        case Cmd::ClipWarp:          return "ClipWarp";
        case Cmd::ClipLoop:          return "ClipLoop";
        case Cmd::SetChain:          return "SetChain";
        case Cmd::SetReturnChain:    return "SetReturnChain";
        case Cmd::SetMasterChain:    return "SetMasterChain";
        case Cmd::SendLevel:         return "SendLevel";
        case Cmd::ReturnVol:         return "ReturnVol";
        case Cmd::RecordSlot:        return "RecordSlot";
        case Cmd::RecordMidiSlot:    return "RecordMidiSlot";
        case Cmd::SetArrangement:    return "SetArrangement";
        case Cmd::SetTrackAutos:     return "SetTrackAutos";
        case Cmd::Locate:            return "Locate";
        case Cmd::BackToArrangement: return "BackToArrangement";
        case Cmd::SetSignatures:     return "SetSignatures";
    }
    return "?";
}

// FNV-1a over the scalars plus the payload. See RemoteEngine::poolRefFor for
// why a fingerprint exists at all.
//
// `probes` caps how much of the payload is mixed: 0 means EVERY byte, and any
// positive number means that many 8-byte words, evenly strided. The two callers
// want different answers and the difference is not a tuning knob — it is what
// each buffer is allowed to do underneath us. See sampleRefFor / notesRefFor.
u64 fingerprint(const void* p, size_t bytes, i64 a, i64 b, f64 c, size_t probes) {
    u64 h = 1469598103934665603ull;
    auto mix = [&](u64 v) {
        for (int i = 0; i < 8; ++i) { h ^= (v >> (i * 8)) & 0xffull; h *= 1099511628211ull; }
    };
    mix((u64)a);
    mix((u64)b);
    u64 cbits = 0;
    std::memcpy(&cbits, &c, sizeof cbits);
    mix(cbits);
    mix((u64)bytes);
    if (!p || !bytes) return h;
    const u8* base = (const u8*)p;
    const size_t words = bytes / sizeof(u64);
    const size_t step  = (probes && words > probes) ? words / probes : 1;
    for (size_t i = 0; i < words; i += step) {
        u64 w = 0;
        std::memcpy(&w, base + i * sizeof(u64), sizeof w);
        mix(w);
    }
    // The bytes past the last whole word. Only reachable on the full path — a
    // strided sample has already given up on completeness — but a hash that
    // ignored a payload's tail would be a trap for the next caller.
    if (step == 1)
        for (size_t i = words * sizeof(u64); i < bytes; ++i) mix((u64)base[i]);
    return h;
}

// A RACK'S CONTENTS, HASHED IN FULL — every field of every sub-device, every
// mapping, every macro — and the "in full" is the same decision notesRefFor
// made, reached from the same place.
//
// The audio path can afford 256 strided probes because a SampleBuffer's samples
// are immutable for the life of the allocation: the question there is only "is
// the buffer at this address still the buffer I cached", and the address is half
// the answer. Neither half is available here. A rack is EDITED IN PLACE all
// session — a knob inside it, a device dropped in, a mapping dragged onto a
// macro — so the question is genuinely "did the content change"; and there is no
// address to key on at all, because RackControl::state() builds a fresh
// RackState on every call, at whatever address the allocator felt like. So the
// content is the only thing there is to hash, and a stride that skipped a field
// would be the notes bug again: a mapping's `max` changed, the same fingerprint
// computed, the cached publication served, and a rack in the daemon that goes on
// sweeping the old range with nothing but the ear to notice.
//
// It is cheap in absolute terms, which is what makes "in full" easy: RackState
// is bounded by the format — kRackMaxDevices sub-devices, kRackMaxMappings
// mappings, eight macros — so a worst case is a few thousand words, hashed once
// per rack per frame. Compare rackStateToString(), which is a shortest-round-
// tripping snprintf per parameter and is therefore deliberately NOT on this
// path: it runs only when this number has moved.
//
// Nested racks ride the same hash for free: RackState::Device::state holds the
// nested rack's own compact form, so hashing those bytes covers every level down
// to kRackMaxDepth.
u64 rackFingerprint(const RackState& s) {
    u64 h = 1469598103934665603ull;
    auto mix = [&](u64 v) {
        for (int i = 0; i < 8; ++i) { h ^= (v >> (i * 8)) & 0xffull; h *= 1099511628211ull; }
    };
    auto text = [&](const std::string& t) {
        mix((u64)t.size());
        for (unsigned char c : t) mix((u64)c);
    };
    auto f32bits = [&](f32 v) { u32 b = 0; std::memcpy(&b, &v, sizeof b); mix((u64)b); };

    mix((u64)s.devices.size());
    for (const RackState::Device& d : s.devices) {
        text(d.uri);
        mix(d.bypass ? 1ull : 0ull);
        mix((u64)d.params.size());
        for (const auto& p : d.params) { mix((u64)p.first); f32bits(p.second); }
        text(d.state);                       // a nested rack, one level at a time
    }
    for (int i = 0; i < kRackMacros; ++i) f32bits(s.macros[i]);
    mix((u64)s.mappings.size());
    for (const RackMapping& m : s.mappings) {
        mix((u64)(i64)m.macro);
        mix((u64)(i64)m.device);
        mix((u64)m.param);
        f32bits(m.min);
        f32bits(m.max);
    }
    return h;
}

// ONE DEVICE'S GENERIC STATE, and it is two fingerprints in a trench coat
// because the thing it identifies is two things.
//
// THE TEXT, IN FULL. The rack's decision (rackFingerprint) reached from the same
// place: `stateString()` builds a fresh string at a fresh address on every call,
// so there is no address to key on, and a device is edited in place all session,
// so the question is genuinely "did the content change". A stride that skipped a
// field would be the notes bug wearing a state string -- a path's last character
// changed, the same fingerprint computed, the cached publication served, and a
// sampler in the daemon still playing the previous file with nothing but the ear
// to notice. It is cheap in absolute terms: bounded by kMaxDeviceState, and
// today's only implementer writes a path.
//
// THE BUFFER, STRIDED. sampleRefFor's decision, reached from ITS place: a
// SampleBuffer's samples are immutable for the life of the allocation, so
// address plus shape plus 256 spread words settles "is this still the buffer I
// sent", which is the only question there is.
//
// AND THE BUFFER HAS TO BE IN HERE AT ALL, which is the part worth stating: the
// state string of a sampler is a PATH, and a path does not identify audio. A
// file edited on disk and re-loaded, or the same file re-decoded at a new engine
// rate, produces the same string and a different buffer. Hashing only the text
// would serve the cached publication and leave the daemon playing the old bytes
// under the right name -- a difference nothing on screen could show.
u64 deviceStateFingerprint(const std::string& text, const SampleBuffer* sb) {
    u64 h = 1469598103934665603ull;
    auto mix = [&](u64 v) {
        for (int i = 0; i < 8; ++i) { h ^= (v >> (i * 8)) & 0xffull; h *= 1099511628211ull; }
    };
    mix((u64)text.size());
    for (unsigned char c : text) mix((u64)c);
    if (!sb) { mix(~0ull); return h; }
    mix((u64)(uintptr_t)sb);
    mix((u64)sb->frames);
    mix((u64)sb->channels);
    u64 rbits = 0;
    std::memcpy(&rbits, &sb->rate, sizeof rbits);
    mix(rbits);
    mix(fingerprint(sb->data.data(), sb->data.size() * sizeof(f32),
                    sb->frames, sb->channels, sb->rate, 256));
    return h;
}

// argv[0]'s directory plus "nxtaktd". The daemon ships beside the GUI, so
// /proc/self/exe is the answer that keeps working from a build tree, an install
// prefix and a test harness alike. $NXTAKT_DAEMON overrides it outright.
std::string daemonPath() {
    if (const char* s = env("DAEMON")) return s;
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return "nxtaktd";
    buf[n] = '\0';
    char* slash = std::strrchr(buf, '/');
    if (!slash) return "nxtaktd";
    *slash = '\0';
    return std::string(buf) + "/nxtaktd";
}

// The chain an App owner id and a Cmd name, as one flat index. The daemon uses
// exactly this layout for its own chains_[] (nxtaktd.cpp, chainIndex()), and
// for the same reason: every operation below is "whatever chain this device is
// on", and a three-way branch per operation is three chances to get it wrong.
constexpr int kChainCount  = kMaxTracks + kMaxReturns + 1;
constexpr int kMasterChain = kMaxTracks + kMaxReturns;
constexpr u32 kNoDev       = 0xffffffffu;

// A state-bearing scalar is one whose value the engine HOLDS. Those are the
// ones a respawned daemon has to be told again (§6's "push mixer scalars and
// tempo"). The rest are actions — a launch, a stop, a locate — and replaying
// one after a crash would be the GUI inventing a performance the user did not
// give: §4.4's honest default is that the transport comes back STOPPED.
bool cmdIsResyncState(Cmd t) {
    switch (t) {
        case Cmd::SetTempo: case Cmd::SetQuantum: case Cmd::SetMetronome:
        case Cmd::TrackVol: case Cmd::TrackPan: case Cmd::TrackMute:
        case Cmd::TrackSolo: case Cmd::TrackArm: case Cmd::MasterVol:
        case Cmd::ClipGain: case Cmd::ClipWarp: case Cmd::ClipLoop:
        case Cmd::SendLevel: case Cmd::ReturnVol:
            return true;
        default:
            return false;
    }
}

} // namespace

struct RemoteEngine {
    ipc::EngineClient cli;
    std::string session;
    std::string driver;                 // what we would spawn a replacement with
    // > 0 only when WE started it. §6's parent-of-record rule: quitting the GUI
    // stops the daemon it spawned and leaves alone one it merely attached to.
    pid_t spawned = -1;

    // Cached because the region they live in goes away with the daemon, and
    // sampleRate() in particular must keep answering a usable number on the way
    // out — a zero would make loadSample() resample a whole set to nothing.
    char driverName[40] = {};
    f64  rate = 48000.0;
    u32  block = 0;

    // -- step 3: the double copy (§5 step 3, decision (i-a)) ----------------
    //
    // A ClipModel's audio is a SampleBuffer on the GUI heap and the engine is in
    // another process, so the bytes have to exist twice: once where
    // drawWaveform() reads them and once in the pool where the daemon can. The
    // doc weighs this against decoding straight into the pool and picks the copy
    // for this step, because the alternative touches src/audio/sample.h,
    // project.cpp, the undo snapshot and tools/render.cpp — none of which this
    // wave owns — and would make a high-risk step unreviewable.
    //
    // Keyed by the SOURCE ADDRESS, with a content fingerprint beside it. The
    // address alone is not enough: a SampleBuffer can be freed and a different
    // one allocated at the same address (undo does exactly this), and serving
    // the cached offset then would publish the wrong audio. Note arrays go
    // further and are edited in place at the same address, which is why the two
    // fingerprints cover different amounts of their payload — sampleRefFor and
    // notesRefFor each say how much, and why.
    struct Cached { u64 ref = 0; u64 finger = 0; };
    std::unordered_map<const void*, Cached> samples;
    std::unordered_map<const void*, Cached> notes;

    // AN ARRANGEMENT'S NOTES CANNOT USE THE ADDRESS CACHE, and the reason is a
    // difference between the two containers rather than a preference.
    //
    // A session cell's RtNote[] is App's own array for that cell: one
    // allocation, edited in place, at a stable address for as long as the clip
    // exists. That is exactly what an address-keyed cache with a content
    // fingerprint is for. An ARRANGEMENT's notes live INSIDE the lane's single
    // allocation ([RtArrangement][items][clips][notes], ARRANGEMENT.md §9.2), so
    // every republication of a lane — and a drag republishes it every frame —
    // puts them at a NEW address, and the old one is freed by App as soon as the
    // retirement comes home. An address-keyed entry would therefore be dead the
    // moment it was written: the map would grow by one entry per frame, each
    // holding a pool block nothing would ever release, and the addresses would
    // be reused by the allocator into the bargain.
    //
    // So the arrangement's notes are keyed by their POSITION — lane, then clip
    // index — with the same content fingerprint beside them. That is the "last
    // published" table this file already keeps for every other pointer the GUI
    // hands over, and it gives the property that matters: a lane republished
    // with the notes unchanged reuses the block it already wrote, so dragging an
    // item does not allocate.
    //
    // Index kMaxTracks is the transport cell (addressed as track -1), the same
    // convention pubArr uses.
    std::vector<Cached> arrNotes[kMaxTracks + 1];

    // -- the retirement stand-in --------------------------------------------
    //
    // The engine announces a *replaced* pointer, and only when it differs from
    // the incoming one. Same rule here, same tables, so App's publishNotes /
    // publishAutos / publishWarp bookkeeping sees exactly what it would have.
    const void* pubNotes[kMaxTracks][kMaxScenes] = {};
    const void* pubAutos[kMaxTracks][kMaxScenes] = {};
    const void* pubWarp [kMaxTracks][kMaxScenes] = {};
    const void* pubArr  [kMaxTracks + 1] = {};      // index kMaxTracks = transport
    const void* pubSigs = nullptr;                 // the signature map: one array, one slot
    const void* pubTrackAutos[kMaxTracks] = {};
    const void* pubChain[kChainCount] = {};
    // ONE inbox for both the wire's events and the synthesised ones, drained by
    // App through popEvent(). It is filled by pumpWire(), which runs from
    // poll() — deliberately, so that a device add completes whether or not the
    // caller happens to be draining events this frame. Reconciliation that only
    // advanced when somebody asked for an event would be a coupling nothing
    // states and nothing tests.
    std::deque<Event> synth;

    // -- step 4: devices ----------------------------------------------------
    //
    // The GUI's PluginInstance is the identity and the model; the daemon's
    // device id is what sounds. See the long note in engine_handle.h for why
    // the seam is here rather than in App.
    struct DevSlot {
        PluginInstance* src = nullptr;
        u32  id  = kNoDev;
        bool live = false;              // EvDeviceAdded landed
        bool bypass = false;            // what we last told the daemon
        std::vector<i32> map;           // GUI param index -> daemon index, -1 none
        std::vector<f32> pushed;        // last value written, per GUI param index
        // The fingerprint of the rack state we last SENT for this device, or 0
        // if it is not a rack / nothing has gone yet. It doubles as the refusal
        // tombstone: a state the daemon rejected leaves its fingerprint here, so
        // syncRacks() does not re-send it sixty times a second, and the next
        // genuine edit produces a different number and genuinely retries. Same
        // discipline as DevChain::refused, one level down.
        u64  rackFinger = 0;
        bool rackFailed = false;        // for the log line and for diagnostics
        // The same three fields for the GENERIC state channel (v10). Separate
        // from the rack's and not folded into them, because the two channels are
        // separate commands with separate refusals and a device could in
        // principle answer both -- one number covering two publications would
        // tombstone whichever failed second.
        u64  stateFinger = 0;
        bool stateFailed = false;
        // Whether a state has EVER gone for this slot. An empty state is a
        // meaningful thing to send (a sampler whose file was cleared) and a
        // meaningless thing to send FIRST -- almost every device in a set has no
        // state at all, and a command per device per chain publish, forever,
        // would be the reconciler talking to itself.
        bool stateSent = false;
    };
    struct DevChain {
        u32 target = ipc::DevTargetTrack;
        i32 idx    = 0;
        // Mirrors the daemon's chain, in processing order, including entries
        // whose AddDevice has not been answered yet. Keeping the placeholders
        // in place is what makes the next add's chainPos correct: the daemon
        // applies device commands strictly in the order it dequeued them.
        std::vector<DevSlot> live;
        std::vector<PluginInstance*> want;    // what the GUI last published
        // Instances the daemon REFUSED. Kept so reconcile() does not retry the
        // same failing AddDevice sixty times a second; dropped as soon as the
        // GUI stops asking for that instance, so a remove-and-re-add retries.
        std::vector<PluginInstance*> refused;
        bool dirty = false;
        bool loggedRack = false;
    };
    DevChain chains[kChainCount];
    struct PendingAdd { int chain = 0; PluginInstance* src = nullptr; };
    std::deque<PendingAdd> pendingAdds;
    // Stable storage for what remoteDevice() hands back: a vector's elements
    // move when it grows, and a caller holding the pointer across a chain edit
    // would be reading freed memory.
    std::unordered_map<const PluginInstance*, RemoteDevice> devInfo;
    u64 devAdded = 0, devFailed = 0;
    // Rack states this handle put on the wire, and the ones the daemon refused.
    // Readable because "refused with a reason" is only true if somebody can see
    // the reason, and because a rack that quietly never published is otherwise
    // indistinguishable from one that published an empty state.
    u64 racksSent = 0, racksFailed = 0;
    // The same pair for the generic device-state channel. Readable for the same
    // reason: a sampler that quietly never published is otherwise
    // indistinguishable from one that published and was refused, and both sound
    // like an instrument that went missing.
    u64 statesSent = 0, statesFailed = 0;
    // Arrangement lanes and automation sets this handle put on the wire, and the
    // ones the daemon refused (EvArrangementAck with ArrAckRefused). A refusal
    // means a timeline that is drawn and does not play, which is not a state a
    // UI may be silent about.
    u64 arrPublished = 0, arrRefused = 0;
    // Signature maps put on the wire, and the ones the daemon refused. A
    // refusal means the set plays in 4/4 while its ruler draws something else,
    // which is the exact disagreement §11.6 made unrenderable in the transport
    // readout and which still has to be sayable in the status line.
    u64 sigsPublished = 0, sigsRefused = 0;
    bool loggedAsync = false;

    // -- step 7: recording ---------------------------------------------------
    //
    // WHAT THE GUI STILL OWNS, AND WHY IT MATTERS THAT IT DOES.
    //
    // App::startRecording allocates a capture buffer, hands it to pushCommand,
    // and frees it on exactly one event: Ev::RecordFinished for that pointer.
    // That contract (engine.h, app.h) does not change here and could not — the
    // whole point of the seam is that App's recording UI works unedited.
    //
    // So the buffer stays App's, and in daemon mode it is never lent to
    // anybody: the audio thread that appends is the daemon's, into memory the
    // daemon allocated. What this table holds is the borrow that ISN'T — the
    // GUI's pointer, kept only so that the take coming home as a FILE can be
    // copied into it and handed back as the event App is waiting for.
    //
    // A take is therefore returned by this handle, not by the engine, in the
    // same sense the note-array retirements above are. Every path out of a take
    // ends in exactly one Ev::RecordFinished for the pointer that went in:
    // arrived, empty, refused, or lost with the engine.
    struct PendingTake {
        int   track = -1, slot = -1;
        bool  midi  = false;
        void* guiBuf = nullptr;      // App's, borrowed for a memcpy and nothing else
        i64   cap    = 0;            // frames, or notes
    };
    std::vector<PendingTake> takes;
    // Releases owed to the daemon. A take the daemon holds is a file on disk
    // and a slot out of kMaxPendingTakes, so "the command ring was full when I
    // finished reading it" may not be the end of the story — it is retried from
    // poll() until it goes.
    std::deque<std::pair<u64, bool>> takeReleases;   // {uid, keepFile}
    u64 takesReturned = 0;    // takes that became an Ev::RecordFinished with frames
    u64 takesEmpty    = 0;    // ...with none, which is an ordinary outcome
    u64 takesFailed   = 0;    // EvTakeFailed, or a file that would not read
    u64 takesLost     = 0;    // cancelled because the engine went away

    // -- step 5: the catalog -------------------------------------------------
    std::vector<PluginDesc> catalog;
    u32  catalogCut = 0;
    bool catalogRead = false;

    // -- step 6: lifecycle ---------------------------------------------------
    bool stopping   = false;            // EvEngineStopping seen
    bool spawnedDied = false;           // the child we started has exited
    u64  openedNs   = 0;
    u64  lastHb     = 0;
    u64  lastHbNs   = 0;
    u64  resyncs    = 0;
    // Every state-bearing scalar this handle has forwarded, by (type, a, b).
    // §6's recovery needs the mixer and the tempo put back, and the handle is
    // the only thing on this path that saw them go by — App's model has them,
    // but App has no idea an engine was replaced. std::map and not a hash: it
    // is walked in full exactly once per respawn and never touched on a hot
    // path, and an ordered container makes the replay deterministic.
    std::map<u64, Command> scalarShadow;
    static constexpr size_t kMaxShadow = 4096;
    bool loggedShadowFull = false;

    // -- accounting ---------------------------------------------------------
    u64  refusals = 0;                  // commands consumed because we cannot carry them
    u64  tears    = 0;                  // snapshots that exhausted the seqlock retries
    u64  poolFull = 0;
    bool loggedCmd[64] = {};            // one log line per Cmd type, not per push
    bool loggedPoolFull = false;
    // syncDeviceStates()'s own once-per-session gate. Separate from
    // loggedPoolFull because a sampler's audio is the largest single thing this
    // handle writes, so it is the first publication a nearly-full pool refuses
    // -- and "the clips would not fit" and "the instrument would not fit" are
    // different sentences for the person reading the log.
    bool loggedStatePool = false;
    bool loggedLost = false;

    // ---------------------------------------------------------------------
    // Link
    // ---------------------------------------------------------------------

    // §4.1's ladder, in the order EngineClient already implements: attach first
    // (a live daemon is the common case and the cheap one), and only reap and
    // spawn when that fails. Attaching first is what stops a pre-emptive reap
    // from unlinking a region whose daemon is merely between shm_open() and
    // ftruncate() — see the note on EngineClient::attach().
    bool open(const char* sess, const char* drv) {
        session = (sess && *sess) ? sess : "default";
        driver  = (drv && *drv) ? drv : "";

        if (!cli.attach(session.c_str(), 0)) {
            LOGI("no engine on session '%s'; starting %s",
                 session.c_str(), daemonPath().c_str());
            // Reaping the corpse on failure rather than leaving a zombie behind
            // a GUI that is about to run in degraded mode is inside
            // spawnAndAttach().
            if (!spawnAndAttach(2000)) return false;
        }

        openedNs = ipc::monotonicNs();
        // The catalog may already be there: we could have attached to a daemon
        // that somebody else had already made scan. EvScanComplete is the other
        // way in and it will not fire again for a scan that is already done.
        if (cli.scanState() == ipc::ScanDone) readCatalog();

        std::snprintf(driverName, sizeof driverName, "daemon:%s", cli.header().driverName);
        const f64 r = cli.sampleRate();
        if (r >= 8000.0 && r <= 384000.0) rate = r;
        block = cli.blockSize();

        // The pool, before anything decodes. §1.4's ordering constraint: the GUI
        // must know the engine rate before it resamples, AND it must have
        // somewhere to put what it decodes. Both are true from here on, and
        // App::init() does not touch a sample until after openLocal() returns.
        if (!cli.createPool(session.c_str())) {
            // Not fatal, and specifically not a reason to fall back to a local
            // engine: transport, tempo, the mixer and the meters all work with
            // no pool at all. What is lost is clips, and the daemon says so —
            // every clip that names an offset comes back RejectNoPool.
            LOGE("no sample pool: %s (clips will not sound)", cli.error());
        }
        LOGI("engine: nxtaktd session '%s', %s, %.0f Hz / %u frames, pid %d%s",
             session.c_str(), cli.header().driverName, rate, block, (int)cli.enginePid(),
             spawned > 0 ? " (started by us)" : " (already running)");
        return true;
    }

    void close() {
        if (refusals)
            LOGW("daemon mode refused %llu command(s) it could not carry",
                 (unsigned long long)refusals);
        if (takesReturned || takesEmpty || takesFailed || takesLost)
            LOGI("takes: %llu returned, %llu empty, %llu failed, %llu lost with "
                 "the engine", (unsigned long long)takesReturned,
                 (unsigned long long)takesEmpty, (unsigned long long)takesFailed,
                 (unsigned long long)takesLost);
        // Every release still owed, on the way out. The daemon we are about to
        // stop does not need them, but a daemon we merely ATTACHED to outlives
        // us and would hold those files for the rest of its life.
        pumpTakeReleases();
        // App frees its own capture buffers after close() returns (app.cpp's
        // shutdown), so this drops the borrow rather than answering it: an event
        // pushed here would go into a queue nobody will drain again.
        takes.clear();
        if (tears)
            LOGW("%llu frame(s) drawn from a state snapshot that could not be "
                 "proved coherent — the engine was stopped mid-publish",
                 (unsigned long long)tears);

        // The pool goes FIRST, and the order is not the obvious one. "Stop the
        // engine, then drop its memory" reads better and leaks: stopping the
        // daemon is a signal plus a wait of up to three seconds, and anything
        // that kills the GUI inside that window (a session ending, a compositor
        // tearing down, an impatient user) leaves 256 MiB named in /dev/shm with
        // nobody left to unlink it. Unlinking first is safe precisely because of
        // the property the pool was designed around: shm_unlink removes the NAME
        // and not the mapping, so the daemon keeps reading the samples it is
        // playing right up until it exits.
        cli.detach();
        cli.closePool();

        // §6: we stop what we started and leave alone what we found. SIGTERM
        // runs nxtaktd's ordinary shutdown (publish the flag, unlink the
        // region); SIGKILL is the escalation a supervisor owes a process that
        // will not go.
        stopSpawned();
    }

    // ---------------------------------------------------------------------
    // The pool
    // ---------------------------------------------------------------------

    // Returns the pool offset that holds a copy of [p, p+bytes), writing one if
    // this is the first time or if the content has changed underneath us.
    // 0 means the pool refused, which is reported once and then counted.
    u64 poolRefFor(std::unordered_map<const void*, Cached>& tbl, const void* p,
                   size_t bytes, u64 finger, bool asNotes, i64 frames, int channels) {
        if (!p || !bytes) return 0;
        auto it = tbl.find(p);
        if (it != tbl.end()) {
            if (it->second.finger == finger) return it->second.ref;
            // The address was reused, or the buffer was edited in place. Drop
            // our own reference to the old block — it frees now if no clip cell
            // ever saw it, and waits for the daemon's echo if one did.
            cli.poolRelease(it->second.ref);
            tbl.erase(it);
        }
        const u64 ref = asNotes
            ? cli.poolWriteNotes((const ipc::WireNote*)p, frames)
            : cli.poolWrite((const f32*)p, frames, channels, rate, 0);
        if (!ref) {
            ++poolFull;
            if (!loggedPoolFull) {
                loggedPoolFull = true;
                LOGE("the sample pool would not take %zu B: %s. Clips beyond this "
                     "point will not sound. [further attempts silent]", bytes, cli.error());
            }
            return 0;
        }
        tbl.emplace(p, Cached{ref, finger});
        return ref;
    }

    // AUDIO: strided, 256 probes, and that is enough for what this fingerprint
    // has to answer for. A SampleBuffer's samples are immutable for the life of
    // the allocation — a re-decode, a resample or an undo produces a NEW buffer,
    // never an edit in place — so the question here is never "did these bytes
    // change" but "is the buffer at this address still the buffer I cached".
    // Frames, channels, rate and byte count plus 256 spread words settle that,
    // and they settle it in a microsecond on a buffer of any length. Hashing
    // every sample of a ten-minute stereo file (a quarter of a gigabyte) on
    // every publication would buy nothing and cost that.
    u64 sampleRefFor(const RtClip& rc) {
        if (!rc.data || rc.frames <= 0 || rc.channels < 1) return 0;
        const size_t bytes = (size_t)rc.frames * (size_t)rc.channels * sizeof(f32);
        return poolRefFor(samples, rc.data, bytes,
                          fingerprint(rc.data, bytes, rc.frames, rc.channels, rate, 256),
                          false, rc.frames, rc.channels);
    }

    // NOTES: every byte, and the strided sample was a real bug here.
    //
    // A note array IS edited in place, at the same address, all the time: that
    // is what the piano roll does. So this fingerprint has to answer the hard
    // question — did the content change — and a strided sample of a long clip
    // cannot. Any stride above 1 skips whole notes, and the arithmetic is worse
    // than that: RtNote is 24 B, three words a note, so an 800-note clip is
    // 2 400 words and 2 400 / 256 is a stride of 9. Every sampled index is then
    // a multiple of 3 — always a note's FIRST word, `beat` — so no note's pitch,
    // velocity, chance or velTo byte was hashed at all. Changing one produced
    // the same fingerprint, poolRefFor served the cached block, and the daemon
    // went on playing the pre-edit notes with nothing to notice it but the ear.
    //
    // Hashing all of it costs what the payload costs, and the payload is tiny:
    // 24 B a note, so a 10 000-note clip is 240 kB — tens of microseconds, once
    // per publication of that clip (this is the SetClip path, not a per-frame
    // one). The alternative designs — a separate cheap checksum beside the
    // fingerprint, or keying notes by content instead of address — buy nothing
    // over it and add a second thing to keep true.
    //
    // WireNote is asserted to mirror RtNote field for field (pool.h), which is
    // what makes poolRefFor's write below a cast and not a conversion loop —
    // and what makes hashing these RtNote bytes the same as hashing exactly
    // what the daemon will reinterpret and play.
    u64 notesRefFor(const RtClip& rc) {
        if (!rc.notes || rc.noteCount <= 0) return 0;
        const size_t bytes = (size_t)rc.noteCount * sizeof(RtNote);
        return poolRefFor(notes, rc.notes, bytes,
                          fingerprint(rc.notes, bytes, rc.noteCount, 0, 0.0, 0),
                          true, rc.noteCount, 0);
    }

    // One RtClip, as the wire spells it: every scalar copied and the two
    // pointers replaced by pool offsets. Shared by the session clip table and by
    // the arrangement blob, which is what makes a clip that is in a scene AND on
    // the timeline name ONE pool block — written once, retired once. Two copies
    // of this conversion would be two chances for those to drift apart.
    // `cell` >= 0 routes the NOTES through the arrangement's positional cache
    // instead of the address one; -1 is the session-cell path. The samples go
    // through the address cache either way, and correctly: RtClip::data points
    // at the shared SampleBuffer on App's heap in both cases, never into the
    // lane's allocation.
    ipc::WireClip wireClipOf(const RtClip& rc, int cell = -1, int idx = -1) {
        ipc::WireClip w = ipc::defaultWireClip();
        w.sampleRef    = sampleRefFor(rc);
        w.notesRef     = cell >= 0 ? laneNotesRef(cell, idx, rc) : notesRefFor(rc);
        w.frames       = rc.frames;
        w.loopStart    = rc.loopStart;
        w.loopEnd      = rc.loopEnd;
        w.noteCount    = rc.noteCount;
        w.clipBpm      = rc.clipBpm;
        w.lengthBeats  = rc.lengthBeats;
        w.prob         = rc.prob;
        w.followBeats  = rc.followBeats;
        w.gain         = rc.gain;
        w.channels     = rc.channels;
        w.warp         = rc.warp;
        w.quantumIdx   = rc.quantumIdx;
        w.followAction = rc.followAction;
        w.loop         = rc.loop ? 1u : 0u;
        w.isMidi       = rc.isMidi ? 1u : 0u;
        w.valid        = rc.valid ? 1u : 0u;
        return w;
    }

    // ---------------------------------------------------------------------
    // Commands
    // ---------------------------------------------------------------------

    // Consume a command the remote path cannot carry, loudly. Answering `false`
    // instead would be worse than wrong: App::flushPending() re-queues a refused
    // publication and retries it every frame, so a permanent `false` wedges the
    // FIFO and with it every scalar behind it. "Refused with a reason" here
    // means consumed, counted, and logged once per command type — never
    // silently dropped, and never pretended to have worked.
    bool refuse(Cmd t, const char* why) {
        ++refusals;
        const u32 i = (u32)t;
        if (i < 64 && !loggedCmd[i]) {
            loggedCmd[i] = true;
            LOGW("daemon mode cannot carry %s: %s [further ones counted, not logged]",
                 cmdName(t), why);
        }
        return true;
    }

    // The engine's retirement rule, verbatim: announce the displaced pointer,
    // and only when it differs from the incoming one.
    void retire(const void*& slot, const void* fresh, Ev ev, i32 a, i32 b) {
        const void* old = slot;
        slot = fresh;
        if (old && old != fresh) {
            Event e;
            e.type = ev; e.a = a; e.b = b; e.x = 0.0;
            e.p = const_cast<void*>(old);
            synth.push_back(e);
        }
    }

    bool push(const Command& c) {
        const u32 type = (u32)c.type;

        if (ipc::commandIsScalar(type)) {
            if (!cli.pushCommand(c.type, c.a, c.b, c.x)) return false;
            shadowScalar(c);
            return true;
        }

        switch (c.type) {
            case Cmd::SetClip:
            case Cmd::ClearClip:
                return pushClipCell(c);

            // The chain family stays refused ON THE WIRE — a client has no
            // business naming an RtChain, because it has no RtChains, and that
            // is the design and not a "not yet" (control.h's command policy).
            //
            // What changed in step 4 is that a refusal on the wire is not a
            // refusal to the caller. The RtChain App just built is a complete
            // DESCRIPTION of the chain it wants, readable through
            // PluginInstance's virtuals, so it is consumed here and turned into
            // AddDevice/RemoveDevice/MoveDevice against the daemon's own
            // instances. Answering `true` is what stops App::addDevice() from
            // rolling the device back out of the model as "engine busy"; the
            // displaced chain is retired by the synthesised Ev::ChainRetired
            // that setChain() queues, so App frees exactly what it would have.
            case Cmd::SetChain:
            case Cmd::SetReturnChain:
            case Cmd::SetMasterChain:
                setChain(c);
                return true;

            // §7, and the last item on its list. The GUI's `p` does not cross
            // and does not need to; its capacity does.
            case Cmd::RecordSlot:
            case Cmd::RecordMidiSlot:
                return pushTake(c);

            // Both go through App's deferred FIFO, so `false` here means "try
            // again next frame" and nothing else — see §11.2.
            case Cmd::SetArrangement: return pushArrangement(c);
            case Cmd::SetTrackAutos:  return pushTrackAutos(c);
            case Cmd::SetSignatures: return pushSignatures(c);

            // Every scalar returned above, through commandIsScalar()'s own
            // exhaustive switch — which is where a newly appended Cmd gets its
            // -Wswitch reminder, and where it belongs, because the classifier is
            // the protocol. Anything arriving here is a command this build does
            // not know at all, and it is refused rather than guessed at.
            default: break;
        }
        return refuse(c.type, "this build does not know that command");
    }

    // Step 3. The RtClip App built is full of GUI-heap pointers; what goes over
    // the wire is a WireClip full of pool offsets, written into the clip table
    // by EngineClient::setClip.
    //
    // ORDER MATTERS. The pool writes happen first and are idempotent (the cache
    // keeps the block across a refusal), then the cell write, and only if THAT
    // succeeds does the retirement bookkeeping run — because App's own
    // publishNotes() runs on exactly the same condition, and the two tables have
    // to agree or a note array comes home twice or never.
    bool pushClipCell(const Command& c) {
        const int t = c.a, s = c.b;
        if (t < 0 || t >= kMaxTracks || s < 0 || s >= kMaxScenes) return true;  // nothing to do

        if (c.type == Cmd::ClearClip) {
            if (!cli.clearClip(t, s)) return false;
            retire(pubNotes[t][s], nullptr, Ev::NotesRetired, t, s);
            retire(pubAutos[t][s], nullptr, Ev::AutosRetired, t, s);
            retire(pubWarp[t][s],  nullptr, Ev::WarpRetired,  t, s);
            return true;
        }

        const RtClip& rc = c.clip;
        ipc::WireClip w = wireClipOf(rc);

        // A clip whose bytes never reached the pool must not be published as a
        // valid one: the daemon would answer RejectBadPoolRef and the cell would
        // sit refused. Publish it as an EMPTY cell instead, which is a state the
        // protocol has a name for, and let the pool-full log line say why.
        if (rc.valid && ((rc.data && !w.sampleRef) || (rc.notes && !w.notesRef))) {
            w = ipc::WireClip{};
            w.channels = 1; w.clipBpm = 120.0; w.lengthBeats = 4.0; w.gain = 1.0f;
            w.warp = (i32)Warp::Beats; w.quantumIdx = -1; w.prob = 1.0;
            w.followAction = (i32)Follow::None;
        }

        // Three cross-process gaps this step does not close, and RtClip is where
        // they show: `autos`, `markers` and `transients` have no WireClip field
        // to travel in, so a clip with an envelope, a warp map or a transient
        // grid plays without them. The daemon cannot be at fault for this — it
        // is not expressible — so the GUI has to be the one that says so.
        if (rc.valid && (rc.autos || rc.markers || rc.transients))
            refuse(Cmd::SetClip, "clip envelopes, warp markers and transients have no "
                                 "WireClip field; the clip crosses without them");

        if (!cli.setClip(t, s, w)) return false;     // busy cell or full ring: retry

        retire(pubNotes[t][s], rc.notes,   Ev::NotesRetired, t, s);
        retire(pubAutos[t][s], rc.autos,   Ev::AutosRetired, t, s);
        retire(pubWarp[t][s],  rc.markers, Ev::WarpRetired,  t, s);
        return true;
    }

    // One arrangement clip's notes, by position. See the note on arrNotes for
    // why this is not the address cache. Hashed IN FULL, for exactly the reason
    // notesRefFor gives: this is the "did the content change" question, and a
    // strided sample of a 24-byte-per-note array only ever lands on `beat`.
    u64 laneNotesRef(int cell, int idx, const RtClip& rc) {
        if (!rc.notes || rc.noteCount <= 0 || idx < 0) return 0;
        std::vector<Cached>& v = arrNotes[cell];
        if ((size_t)idx >= v.size()) v.resize((size_t)idx + 1);
        Cached& e = v[(size_t)idx];

        const size_t bytes = (size_t)rc.noteCount * sizeof(RtNote);
        const u64 finger = fingerprint(rc.notes, bytes, rc.noteCount, 0, 0.0, 0);
        if (e.ref && e.finger == finger) return e.ref;
        if (e.ref) { cli.poolRelease(e.ref); e.ref = 0; e.finger = 0; }

        const u64 ref = cli.poolWriteNotes((const ipc::WireNote*)rc.notes, rc.noteCount);
        if (!ref) {
            ++poolFull;
            if (!loggedPoolFull) {
                loggedPoolFull = true;
                LOGE("the sample pool would not take %zu B of arrangement notes: %s "
                     "[further attempts silent]", bytes, cli.error());
            }
            return 0;
        }
        e.ref = ref;
        e.finger = finger;
        return ref;
    }

    // Drop the positional entries past `keep`. Called after a lane is accepted,
    // never before: until then the blocks are still named by the lane the engine
    // is playing, and poolRelease on a Live block defers rather than frees, so
    // "after" is both correct and the only order that is obviously correct.
    void trimLaneNotes(int cell, size_t keep) {
        std::vector<Cached>& v = arrNotes[cell];
        for (size_t i = keep; i < v.size(); ++i)
            if (v[i].ref) cli.poolRelease(v[i].ref);
        if (v.size() > keep) v.resize(keep);
    }

    // ---------------------------------------------------------------------
    // The arrangement (docs/ARRANGEMENT.md §9, GUI-ON-DAEMON.md §11.9)
    // ---------------------------------------------------------------------
    //
    // The daemon has been able to take an arrangement since wave 8g — it has
    // translateArrangement() and daemon_test §16b proves it plays one. What was
    // missing was this: the GUI hands the handle an already-built
    // `RtArrangement`, a struct of pointers into one GUI-heap allocation, and
    // somebody has to turn it into the blob.
    //
    // It is the clip encoder one level out, over the same pointer -> pool-ref
    // cache: every RtClip inside the lane goes through sampleRefFor/notesRefFor
    // exactly as a session cell's does, so a clip used by a session slot AND by
    // an arrangement item names ONE block, is written once, and is retired once.
    // That sharing is not an optimisation, it is what makes a set whose scenes
    // were dragged onto the timeline cost the same memory as the set did.
    //
    // WHAT IS NOT CARRIED, and it is stated per-lane rather than left to be
    // discovered: RtClip::autos, ::markers and ::transients have no WireClip
    // field, so an arrangement clip with an envelope, a warp map or a transient
    // grid crosses without them — the same three gaps §11.4 named for session
    // clips, in the same place, for the same reason.
    bool pushArrangement(const Command& c) {
        const int cell = (c.a == -1) ? kMaxTracks : c.a;
        if (cell < 0 || cell > kMaxTracks) return refuse(c.type, "no such arrangement lane");

        const RtArrangement* ra = (const RtArrangement*)c.p;

        // Clearing is a command with ref == 0 and is always expressible. It has
        // to go through the same busy/ring check as a publication, or a clear
        // could overtake a publication that has not been acknowledged.
        if (!ra) {
            if (!cli.setArrangement(c.a, 0)) return false;
            retire(pubArr[cell], nullptr, Ev::ArrangementRetired, c.a, 0);
            trimLaneNotes(cell, 0);
            return true;
        }

        // Bounds FIRST, and refused as a whole rather than truncated. The
        // daemon applies exactly these numbers (control.h, kMaxArr*) and answers
        // RejectBadArrangement for a blob that breaks one, so clamping here
        // would only move the refusal somewhere it could not be attributed —
        // and a lane silently missing its last items is worse than a lane the
        // status line says was refused.
        if (ra->itemCount < 0 || ra->clipCount < 0 ||
            (i64)ra->itemCount > ipc::kMaxArrItems ||
            (i64)ra->clipCount > ipc::kMaxArrItems ||
            (i64)ra->noteCount > ipc::kMaxArrNotes) {
            return refuse(c.type, "the lane is past the protocol's bounds "
                                  "(512 items, 65536 notes); it is not published");
        }
        if ((ra->itemCount && !ra->items) || (ra->clipCount && !ra->clips))
            return refuse(c.type, "the lane declares items or clips it does not carry");

        std::vector<ipc::WireArrItem> items((size_t)ra->itemCount);
        for (int i = 0; i < ra->itemCount; ++i) {
            const RtArrItem& s = ra->items[i];
            ipc::WireArrItem& w = items[(size_t)i];
            w.start     = s.start;
            w.length    = s.length;
            w.offset    = s.offset;
            w.fadeIn    = s.fadeIn;
            w.fadeOut   = s.fadeOut;
            w.fadeShape = s.fadeShape;
            w.clip      = s.clip;
        }

        std::vector<ipc::WireClip> clips((size_t)ra->clipCount);
        bool dropped = false, poolShort = false;
        for (int i = 0; i < ra->clipCount; ++i) {
            const RtClip& rc = ra->clips[i];
            ipc::WireClip& w = clips[(size_t)i];
            w = wireClipOf(rc, cell, i);
            if (rc.valid && ((rc.data && !w.sampleRef) || (rc.notes && !w.notesRef)))
                poolShort = true;
            if (rc.valid && (rc.autos || rc.markers || rc.transients)) dropped = true;
        }

        // A lane whose audio never reached the pool must not be published as if
        // it had: the daemon would answer RejectBadArrangement, the whole lane
        // would be refused, and App's FIFO would retry it for ever against a
        // pool that is still full. Consumed and reported instead.
        if (poolShort)
            return refuse(c.type, "the sample pool would not take this lane's audio; "
                                  "the timeline is not published");
        if (dropped)
            refuse(c.type, "an arrangement clip's envelope, warp map or transients have "
                           "no WireClip field; the lane crosses without them");

        ipc::WireArrHeader h{};
        h.noteCount = ra->noteCount;
        h.loopStart = ra->loopStart;
        h.loopEnd   = ra->loopEnd;
        h.loopOn    = ra->loopOn ? 1u : 0u;

        const u64 blob = cli.poolWriteArrangement(h, items, clips);
        if (!blob) {
            ++poolFull;
            if (!loggedPoolFull) {
                loggedPoolFull = true;
                LOGE("the sample pool would not take an arrangement blob: %s "
                     "[further attempts silent]", cli.error());
            }
            return false;                       // transient: the pool may free up
        }
        // A refused publication must free the blob it just wrote, or a busy lane
        // dragged for a second leaks one block per frame. The client only takes
        // ownership of a blob it accepted.
        if (!cli.setArrangement(c.a, blob)) { cli.poolRelease(blob); return false; }

        retire(pubArr[cell], c.p, Ev::ArrangementRetired, c.a, 0);
        trimLaneNotes(cell, (size_t)ra->clipCount);
        ++arrPublished;
        return true;
    }

    // THE SIGNATURE MAP (protocol v8). Cmd::SetSignatures spent several waves
    // refused with a reason — it was outside ipc::commandIsKnown's bound, the
    // daemon answered RejectUnknownCommand, and daemon mode PLAYED EVERY SET IN
    // 4/4 while the ruler drew 7/8. Refused-and-visible beat accepted-and-
    // ignored, and now neither is necessary.
    //
    // It is the shortest of the pooled payloads: one flat array, nothing else in
    // the pool named by it, no per-track addressing. Note there is no content
    // cache and there should not be — session.h's syncSignatures only calls this
    // when the map has actually changed, which is a handful of times in a
    // session, so a cache would be a second thing to keep true in exchange for
    // nothing.
    bool pushSignatures(const Command& c) {
        const RtSig* map = (const RtSig*)c.p;
        const i32 n = c.a;

        if (!map || n <= 0) {
            if (!cli.clearSignatures()) return false;
            retire(pubSigs, nullptr, Ev::SigsRetired, 0, 0);
            return true;
        }
        if (n > kMaxSigs)
            return refuse(c.type, "the signature map is past kMaxSigs; the set keeps "
                                  "the map the engine already has");

        // A copy, field by field, rather than a cast over the RtSig array: the
        // two structs are asserted to mirror each other (pool.h, WireSig), but
        // this is the one place where the wire's shape is stated rather than
        // inherited, and the assert is what makes the copy free.
        std::vector<ipc::WireSig> ws((size_t)n);
        for (i32 i = 0; i < n; ++i) {
            ws[(size_t)i].bar  = map[i].bar;
            ws[(size_t)i].num  = map[i].num;
            ws[(size_t)i].den  = map[i].den;
            ws[(size_t)i].pad  = 0;
            ws[(size_t)i].beat = map[i].beat;
        }

        const u64 blob = cli.poolWriteSignatures(ws.data(), n);
        if (!blob) {
            ++poolFull;
            if (!loggedPoolFull) {
                loggedPoolFull = true;
                LOGE("the sample pool would not take a %d-entry signature map: %s "
                     "[further attempts silent]", n, cli.error());
            }
            return false;
        }
        if (!cli.setSignatures(blob, n)) { cli.poolRelease(blob); return false; }

        // The retirement stand-in, one array wide. The GUI keeps the RtSig[] it
        // published until Ev::SigsRetired names it; nothing crossed (the map was
        // COPIED into a blob), so there is no engine here to send that event and
        // App would hold every map it ever published for the life of the session.
        retire(pubSigs, c.p, Ev::SigsRetired, 0, 0);
        ++sigsPublished;
        return true;
    }

    bool pushTrackAutos(const Command& c) {
        if (c.a < 0 || c.a >= kMaxTracks) return refuse(c.type, "no such automation lane");
        const RtAutoSetN* as = (const RtAutoSetN*)c.p;

        if (!as) {
            if (!cli.setTrackAutos(c.a, 0)) return false;
            retire(pubTrackAutos[c.a], nullptr, Ev::TrackAutosRetired, c.a, 0);
            return true;
        }
        if (as->laneCount < 0 || as->pointCount < 0 ||
            (i64)as->laneCount > ipc::kMaxArrLanes ||
            (i64)as->pointCount > ipc::kMaxArrPoints)
            return refuse(c.type, "the automation set is past the protocol's bounds "
                                  "(32 lanes, 65536 points); it is not published");
        if ((as->laneCount && !as->lanes) || (as->pointCount && !as->points))
            return refuse(c.type, "the automation set declares lanes or points it does "
                                  "not carry");

        std::vector<ipc::WireAutoLane>  lanes((size_t)as->laneCount);
        std::vector<ipc::WireAutoPoint> points((size_t)as->pointCount);
        for (int i = 0; i < as->laneCount; ++i) {
            const RtAutoLane& s = as->lanes[i];
            ipc::WireAutoLane& w = lanes[(size_t)i];
            w.target  = s.target;
            w.index   = s.index;
            w.devSlot = s.devSlot;
            w.xform   = s.xform;
            w.first   = s.first;
            w.count   = s.count;
            w.lo      = s.lo;
            w.hi      = s.hi;
            w.flags   = s.flags;
            w.pad     = 0;
        }
        // Reinterpreted rather than converted would be legal here — WireAutoPoint
        // is asserted to mirror RtAutoPoint field for field — but the copy is
        // written out because RtAutoSetN itself holds pointers and the blob's
        // point array has to be a separate, contiguous run either way.
        for (int i = 0; i < as->pointCount; ++i) {
            const RtAutoPoint& s = as->points[i];
            ipc::WireAutoPoint& w = points[(size_t)i];
            w.beat  = s.beat;
            w.value = s.value;
            w.curve = s.curve;
            w.pad[0] = w.pad[1] = w.pad[2] = 0;
        }

        const u64 blob = cli.poolWriteTrackAutos(lanes, points);
        if (!blob) {
            ++poolFull;
            if (!loggedPoolFull) {
                loggedPoolFull = true;
                LOGE("the sample pool would not take a track-automation blob: %s "
                     "[further attempts silent]", cli.error());
            }
            return false;
        }
        if (!cli.setTrackAutos(c.a, blob)) { cli.poolRelease(blob); return false; }

        retire(pubTrackAutos[c.a], c.p, Ev::TrackAutosRetired, c.a, 0);
        ++arrPublished;
        return true;
    }

    // ---------------------------------------------------------------------
    // Devices (§5 step 4)
    // ---------------------------------------------------------------------

    // App's owner addressing (Command::a for the three chain commands) as the
    // flat chain index, plus the wire's (target, index) pair for it.
    static int chainOf(Cmd t, i32 a, u32& target, i32& idx) {
        switch (t) {
            case Cmd::SetChain:
                if (a < 0 || a >= kMaxTracks) return -1;
                target = ipc::DevTargetTrack; idx = a; return a;
            case Cmd::SetReturnChain:
                if (a < 0 || a >= kMaxReturns) return -1;
                target = ipc::DevTargetReturn; idx = a; return kMaxTracks + a;
            case Cmd::SetMasterChain:
                target = ipc::DevTargetMaster; idx = 0; return kMasterChain;
            default:
                return -1;
        }
    }

    // -- recording (§7) ------------------------------------------------------

    PendingTake* takeOn(int track) {
        for (PendingTake& p : takes) if (p.track == track) return &p;
        return nullptr;
    }

    // Hands App back the buffer it lent, as the event it is waiting for. THE
    // ONLY WAY a capture buffer is ever returned on this path, so every exit
    // from a take goes through here — including the ones that are failures.
    // `frames` <= 0 is App's own "Recording cancelled", verbatim.
    void returnTake(const PendingTake& p, i64 frames) {
        Event e;
        e.type = p.midi ? Ev::MidiRecordFinished : Ev::RecordFinished;
        e.a    = p.track;
        e.b    = p.slot;
        e.x    = (f64)frames;
        e.p    = p.guiBuf;
        synth.push_back(e);
    }

    void forgetTake(int track) {
        for (size_t i = 0; i < takes.size(); ++i)
            if (takes[i].track == track) { takes.erase(takes.begin() + (long)i); return; }
    }

    // Cmd::RecordSlot / Cmd::RecordMidiSlot from App. A toggle on this side too,
    // and the mirror of the daemon's: the FIRST one for a track is a start, and
    // the table entry is what makes the second a stop.
    //
    // `false` is legal here and means exactly what it means everywhere else:
    // nothing was sent, so try again. App::startRecording answers it by freeing
    // the buffer it just allocated and saying "engine busy", which is right
    // because the daemon's state genuinely did not move.
    bool pushTake(const Command& c) {
        const bool midi = (c.type == Cmd::RecordMidiSlot);
        const int  t = c.a, s = c.b;
        if (t < 0 || t >= kMaxTracks || s < 0 || s >= kMaxScenes)
            return refuse(c.type, "track or slot index out of range");

        if (PendingTake* live = takeOn(t)) {
            // A stop. App resends the same payload for the same slot.
            if (live->slot == s && live->midi == midi)
                return cli.recordSlot(t, s, live->cap, midi);

            // Anything else is a second take on a track that already has one —
            // the engine's hand-over path, which needs a second buffer on the
            // far side that this side has no way to have ready.
            //
            // ANSWERED `false`, NOT CONSUMED, and the distinction matters:
            // consuming it would leave App holding a capture buffer that only a
            // finish event frees, and no finish event is coming for a take that
            // was never started. `false` is also honest rather than a
            // convenience — this is not a permanent refusal, it is "not while
            // that take is running", and it clears when the take ends.
            // App::startRecording answers it by freeing the buffer and saying
            // the engine is busy, which is the true sentence.
            ++refusals;
            if (!loggedCmd[(u32)c.type & 63u]) {
                loggedCmd[(u32)c.type & 63u] = true;
                LOGW("a take is already running on track %d slot %d; the take asked "
                     "for on slot %d is refused until it ends", t, live->slot, s);
            }
            return false;
        }

        // A start. The buffer is App's and stays App's; what crosses is how big
        // it is, because that is what the daemon has to allocate.
        if (!c.p || !(c.x >= 1.0))
            return refuse(c.type, "a take needs a buffer and a positive capacity");
        const i64 cap = (i64)c.x;
        if (!cli.recordSlot(t, s, cap, midi)) return false;
        PendingTake p;
        p.track = t; p.slot = s; p.midi = midi;
        p.guiBuf = c.p; p.cap = cap;
        takes.push_back(p);
        return true;
    }

    // EvTakeReady: the daemon has written the take and told us where. Read it
    // into the buffer App has been holding all along and hand it back as the
    // event App has been waiting for.
    //
    // THE COPY IS ON THE GUI THREAD, deliberately and once per take. It is the
    // one cost this design has that the in-process path does not, and it lands
    // beside a cost App already pays in the same breath — sampleFromRecording()
    // copies the buffer again and builds a peak envelope over it. A take is a
    // gesture that has just ended; a frame spent finishing it is a frame nobody
    // is playing through.
    void takeReady(const ipc::WireEvent& w) {
        PendingTake* pp = takeOn(w.a);
        if (!pp) {
            // Nothing is holding a buffer for this. Still release it: the file
            // is the daemon's to drop, and a take nobody claims is a slot out of
            // kMaxPendingTakes for the rest of the session.
            LOGW("a take arrived for track %d, which is not recording here; "
                 "releasing it", (int)w.a);
            if (w.ref) takeReleases.emplace_back(w.ref, false);
            return;
        }
        const PendingTake p = *pp;
        forgetTake(w.a);

        const i64 announced = (i64)w.x;
        if ((w.flags & ipc::TakeWasEmpty) || announced <= 0) {
            ++takesEmpty;
            returnTake(p, 0);
            if (w.ref) takeReleases.emplace_back(w.ref, false);
            return;
        }

        char path[768];
        cli.takePathFor(w, path, sizeof path);
        i64 got = -1;
        if (path[0]) {
            got = p.midi ? ipc::readMidiTake(path, (ipc::WireNote*)p.guiBuf, p.cap)
                         : ipc::readAudioTake(path, (f32*)p.guiBuf, p.cap, nullptr, nullptr);
        }
        if (got < 0) {
            // The material is on disk and this process could not read it. Say
            // so, name the file, and tell the daemon to KEEP it: a take is never
            // destroyed on the word of the process that failed to pick it up.
            ++takesFailed;
            LOGE("take %llu was written to '%s' and could not be read back: %s. "
                 "The file is being kept.",
                 (unsigned long long)w.ref, path[0] ? path : "(no take directory)",
                 std::strerror(errno));
            returnTake(p, 0);
            if (w.ref) takeReleases.emplace_back(w.ref, true);
            return;
        }
        if (got < announced)
            LOGW("take %llu announced %lld %s and %lld fitted the buffer",
                 (unsigned long long)w.ref, (long long)announced,
                 p.midi ? "notes" : "frames", (long long)got);
        ++takesReturned;
        returnTake(p, got);
        if (w.ref) takeReleases.emplace_back(w.ref, false);
    }

    // Every take this handle is still holding a buffer for, handed back empty.
    // A take in flight when the engine dies is GONE — there is no half a
    // recording — and the one thing that must not happen is App waiting forever
    // for an event that has no sender left, with its capture buffer pinned.
    void cancelTakes(const char* why) {
        if (takes.empty()) return;
        LOGW("%zu take(s) in flight when %s; each is handed back empty",
             takes.size(), why);
        for (const PendingTake& p : takes) { ++takesLost; returnTake(p, 0); }
        takes.clear();
        takeReleases.clear();
    }

    // Retried from poll(), because a release that never lands leaves the daemon
    // holding a file for the rest of its life.
    void pumpTakeReleases() {
        while (!takeReleases.empty()) {
            const auto& r = takeReleases.front();
            if (!cli.releaseTake(r.first, r.second)) return;
            takeReleases.pop_front();
        }
    }

    // Ev::ChainRetired's own addressing (engine.h): a track index, kMaxTracks+i
    // for a return, -1 for the master. Not the same as Command::a for a return
    // chain, which is the bare index — App uses this only for its log line, but
    // sending the wrong one would make that line lie.
    static i32 retireAddr(int chain) {
        return chain == kMasterChain ? -1 : (i32)chain;
    }

    // The GUI has declared a chain. Record it and let reconcile() make it so.
    //
    // Nothing is refused here and nothing spins: App::publishChain() is
    // deliberately not on the deferred FIFO, so a `false` would make it free the
    // chain it built and log "engine busy", and App::addDevice() would then roll
    // the device back out of the model — which is exactly the "visible and
    // silent" failure this step exists to remove. The work is asynchronous and
    // the answer is "accepted".
    void setChain(const Command& c) {
        u32 target = 0; i32 idx = 0;
        const int ci = chainOf(c.type, c.a, target, idx);
        if (ci < 0) { refuse(c.type, "no such chain owner"); return; }

        DevChain& ch = chains[ci];
        ch.target = target;
        ch.idx    = idx;
        ch.want.clear();
        const RtChain* rc = (const RtChain*)c.p;
        if (rc)
            for (int i = 0; i < rc->count && i < kMaxChainFx; ++i)
                if (rc->fx[i]) ch.want.push_back(rc->fx[i]);

        // A refusal tombstone only survives while the GUI keeps asking for that
        // instance. Drop the others, so removing a device that would not load
        // and adding it again genuinely retries rather than being remembered as
        // broken forever.
        for (size_t i = 0; i < ch.refused.size();) {
            const bool stillWanted =
                std::find(ch.want.begin(), ch.want.end(), ch.refused[i]) != ch.want.end();
            if (stillWanted) ++i;
            else ch.refused.erase(ch.refused.begin() + (long)i);
        }

        // docs/RACKS.md §4 says a rack's descriptor does not describe its
        // contents, and until protocol v7 there was no wire field that could —
        // so the daemon instantiated 'nxtakt:rack', got an EMPTY one, and the
        // device sounded as a passthrough while the GUI drew what was supposed to
        // be inside it. CmdSetRackState is that field: syncRacks() carries the
        // contents as a PoolKindRackState blob and the daemon applies them with
        // rackStateFromString + setState. The warning that used to be here is
        // gone because the sentence it was warning about is no longer true.
        if (!ch.loggedRack)
            for (PluginInstance* p : ch.want)
                if (p && p->rack()) {
                    ch.loggedRack = true;
                    LOGI("daemon mode: a rack's contents cross as a pool blob "
                         "(docs/RACKS.md); the engine builds its own copy of the "
                         "sub-chain and re-derives nothing");
                    break;
                }

        ch.dirty = true;
        retire(pubChain[ci], c.p, Ev::ChainRetired, retireAddr(ci), 0);
    }

    // Walks every dirty chain and issues at most what the rings will take. Runs
    // once per frame from poll().
    //
    // THE INVARIANT: `live` is what the daemon's chain will be once everything
    // already sent has been applied — placeholders included. Because the daemon
    // dequeues device commands strictly in order, a position computed against
    // `live` is the position the daemon will use, even for commands whose
    // answers have not come back yet.
    void reconcile() {
        if (!cli.attached()) return;
        for (int ci = 0; ci < kChainCount; ++ci) {
            DevChain& ch = chains[ci];
            if (!ch.dirty) continue;

            // A chain with an unanswered add is not reconcilable: its `live`
            // holds a placeholder with no device id, so a removal or a move on
            // it could not be expressed. One frame of waiting, and the queue is
            // the daemon's, not ours.
            bool waiting = false;
            for (const DevSlot& s : ch.live) if (!s.live) { waiting = true; break; }
            if (waiting) continue;

            // What the daemon should end up with: everything the GUI wants,
            // minus anything it has already refused.
            std::vector<PluginInstance*> want;
            want.reserve(ch.want.size());
            for (PluginInstance* p : ch.want)
                if (std::find(ch.refused.begin(), ch.refused.end(), p) == ch.refused.end())
                    want.push_back(p);
            if ((int)want.size() > kMaxChainFx) want.resize(kMaxChainFx);

            bool stalled = false;

            // 1. Removals first, so that every position computed below is a
            //    position in the chain as it will be, not as it was.
            for (size_t i = 0; i < ch.live.size() && !stalled;) {
                DevSlot& s = ch.live[i];
                if (std::find(want.begin(), want.end(), s.src) != want.end()) { ++i; continue; }
                if (!cli.removeDevice(s.id)) { stalled = true; break; }
                devInfo.erase(s.src);
                ch.live.erase(ch.live.begin() + (long)i);
            }

            // 2. Position by position, move what is in the wrong place and add
            //    what is not there at all.
            for (size_t pos = 0; pos < want.size() && !stalled; ++pos) {
                if (pos < ch.live.size() && ch.live[pos].src == want[pos]) continue;

                size_t at = ch.live.size();
                for (size_t j = pos; j < ch.live.size(); ++j)
                    if (ch.live[j].src == want[pos]) { at = j; break; }

                if (at < ch.live.size()) {
                    if (!cli.moveDevice(ch.live[at].id, (i32)pos)) { stalled = true; break; }
                    DevSlot moved = std::move(ch.live[at]);
                    ch.live.erase(ch.live.begin() + (long)at);
                    ch.live.insert(ch.live.begin() + (long)pos, std::move(moved));
                    continue;
                }

                PluginInstance* p = want[pos];
                const std::string uri = p ? p->desc().uri : std::string();
                if (uri.empty()) { ch.refused.push_back(p); continue; }
                if (!cli.addDevice(ch.target, ch.idx, (i32)pos, uri.c_str())) {
                    stalled = true;                 // ring or pool full: retry next frame
                    break;
                }
                DevSlot s;
                s.src = p;
                ch.live.insert(ch.live.begin() + (long)pos, std::move(s));
                pendingAdds.push_back(PendingAdd{ci, p});
                if (!loggedAsync) {
                    loggedAsync = true;
                    LOGI("daemon mode: devices instantiate in the engine, so a chain "
                         "edit takes effect a moment after it is drawn (the first one "
                         "waits for the engine's plugin scan)");
                }
            }

            // Converged only when nothing is outstanding and the two agree.
            if (!stalled && ch.live.size() == want.size()) {
                bool same = true;
                for (size_t i = 0; i < want.size(); ++i)
                    if (ch.live[i].src != want[i] || !ch.live[i].live) { same = false; break; }
                ch.dirty = !same;
            }
        }
    }

    // Every frame, for every device the daemon has: push what moved.
    //
    // This is a POLL and not a hook, because there is no hook to have: a knob
    // drag calls PluginInstance::setParam() on the GUI's own instance and
    // nothing tells this object. Polling the model is what makes the mirror
    // complete — it also carries a project load's restored parameters, an undo,
    // a rack macro driving its targets and a bypass toggle, none of which have
    // a command of their own on this path.
    //
    // Cost is one virtual getParam() per mapped control per frame: a session
    // with ten devices of twenty controls is 200 calls, which is two orders of
    // magnitude below one draw call.
    void syncParams() {
        for (int ci = 0; ci < kChainCount; ++ci)
            for (DevSlot& s : chains[ci].live) {
                if (!s.live || !s.src) continue;

                // Bypass is a COMMAND and not a param write, and getting that
                // wrong is a one-line reflex: it has to order against the chain
                // edits around it, because bypass-then-remove and
                // remove-then-bypass are different (§3.7).
                const bool bp = s.src->bypassed();
                if (bp != s.bypass && cli.setBypass(s.id, bp)) s.bypass = bp;

                const int n = s.src->paramCount();
                const int m = (int)s.map.size();
                for (int i = 0; i < n && i < m; ++i) {
                    if (s.map[i] < 0) continue;
                    const f32 v = s.src->getParam(i);
                    // Bitwise, not `!=`. A plugin that reports NaN would make
                    // every comparison unequal and turn this into a write per
                    // control per frame, forever.
                    if (std::memcmp(&v, &s.pushed[i], sizeof v) == 0) continue;
                    if (cli.setDeviceParam(s.id, (u32)s.map[i], v)) s.pushed[i] = v;
                }
            }
    }

    // A RACK'S CONTENTS, ONCE PER FRAME, AND ONLY WHEN THEY MOVED.
    //
    // The same reason syncParams() is a poll and not a hook: a user dropping a
    // device into a rack, dragging a mapping onto a macro or turning a knob
    // three levels down calls straight into the GUI's own RackControl and
    // nothing tells this object. There is no command to hang a hook on, so the
    // model is compared against what was last sent.
    //
    // The comparison is a fingerprint over RackState (see rackFingerprint) and
    // NOT over rackStateToString()'s output, which is the one performance
    // decision in here worth stating: state() is a walk of virtual getters,
    // cheap enough for a frame; the string is a shortest-round-tripping snprintf
    // per parameter, which for eight devices of sixty controls is hundreds of
    // formats. So the string is built only when the fingerprint has moved, which
    // is exactly when there is something to send.
    //
    // ORDER: after syncParams(), deliberately. The macros are ordinary params
    // and go through the param table; the contents go through this command. The
    // daemon re-orders them correctly on its own side (it applies the pending
    // param row before setState and re-seeds its cache after — RACKS.md's
    // ordering trap), but sending them in the order the GUI itself would apply
    // them keeps the two sides' reasoning the same shape.
    void syncRacks() {
        for (int ci = 0; ci < kChainCount; ++ci)
            for (DevSlot& s : chains[ci].live) {
                if (!s.live || !s.src) continue;
                RackControl* rc = s.src->rack();
                if (!rc) continue;

                const RackState st = rc->state();
                const u64 finger = rackFingerprint(st);
                if (finger == s.rackFinger) continue;

                const std::string text = rackStateToString(st);
                if (text.size() + 1 > ipc::kMaxRackState) {
                    // Bounded by the format, so this needs a hostile or a
                    // pathological rack — but a silent truncation would install a
                    // DIFFERENT rack, not a shorter one, so it is refused and
                    // tombstoned like any other permanent failure.
                    if (!s.rackFailed) {
                        s.rackFailed = true;
                        LOGE("device %u: its rack serialises to %zu bytes, past the "
                             "%llu the pool will carry; the engine keeps the contents "
                             "it already has", s.id, text.size(),
                             (unsigned long long)ipc::kMaxRackState);
                    }
                    s.rackFinger = finger;
                    continue;
                }

                auto it = devInfo.find(s.src);
                const u32 gen = it != devInfo.end() ? it->second.generation
                                                    : cli.deviceGeneration(s.id);
                // A refusal here is "could not send" — ring full, pool full,
                // nothing attached — so the fingerprint is deliberately NOT
                // advanced and the next frame tries again.
                if (!cli.setRackState(s.id, gen, text.c_str())) continue;
                s.rackFinger = finger;
                s.rackFailed = false;
                ++racksSent;
            }
    }

    // ONE DEVICE'S GENERIC STATE, ONCE PER FRAME, AND ONLY WHEN IT MOVED.
    //
    // syncRacks() with the serial numbers filed off, and for the identical
    // reason: a user dropping a file onto a sampler calls straight into the
    // GUI's own SamplerControl and nothing tells this object. There is no
    // command to hang a hook on, so the model is compared against what was last
    // sent.
    //
    // A RACK IS SKIPPED HERE, and that is not a tidy-up waiting to happen. In
    // this tree a rack's contents are NOT reachable through stateString():
    // `Rack` overrides neither half of the generic pair, so calling it would
    // return the base class's empty string and calling setStateString() on the
    // far side would hit the base class's accept-anything-remember-nothing
    // default. The rack has its own command precisely because its state has its
    // own accessor. GUI-ON-DAEMON.md §15.1 says what folding the two would take
    // and why this wave could not.
    //
    // ORDER: after syncParams() and after syncRacks(), so that a state string is
    // the LAST thing to land for a device in any one frame. host.h's ordering
    // rule ("parameters FIRST, then setStateString()") is enforced on the far
    // side -- doSetDeviceState applies the pending param row itself -- but
    // sending them in the order the far side will apply them keeps the two
    // sides' reasoning the same shape, which is the same argument syncRacks()
    // makes one line up.
    void syncDeviceStates() {
        for (int ci = 0; ci < kChainCount; ++ci)
            for (DevSlot& s : chains[ci].live) {
                if (!s.live || !s.src) continue;
                if (s.src->rack()) continue;            // see above

                const std::string text = s.src->stateString();
                // The buffer is held for the whole publication, on purpose: the
                // pool write below is a memcpy of every sample, and a sampler
                // re-pointed during it would otherwise leave the copy reading a
                // buffer that has already moved to `retired_`. host.h says so at
                // the accessor.
                SamplerControl* sc = s.src->sampler();
                const SampleRef buf = sc ? sc->sampleBuffer() : SampleRef{};
                const u64 finger = deviceStateFingerprint(text, buf.get());
                if (finger == s.stateFinger) continue;

                // Nothing to say, and nothing has ever been said. Almost every
                // device in a set lands here exactly once and never again.
                if (text.empty() && !buf && !s.stateSent) { s.stateFinger = finger; continue; }

                if (text.size() + 1 > ipc::kMaxDeviceState) {
                    // A silent truncation would install a DIFFERENT state, not a
                    // shorter one -- for a sampler, a different FILE -- so it is
                    // refused and tombstoned like any other permanent failure.
                    if (!s.stateFailed) {
                        s.stateFailed = true;
                        LOGE("device %u: its state serialises to %zu bytes, past the "
                             "%llu the pool will carry; the engine keeps the state it "
                             "already has", s.id, text.size(),
                             (unsigned long long)ipc::kMaxDeviceState);
                    }
                    s.stateFinger = finger;
                    continue;
                }

                auto it = devInfo.find(s.src);
                const u32 gen = it != devInfo.end() ? it->second.generation
                                                    : cli.deviceGeneration(s.id);
                // A refusal here is "could not send" -- ring full, pool full,
                // nothing attached -- so the fingerprint is deliberately NOT
                // advanced and the next frame tries again. The pool refusal is
                // the one that matters: a sampler's audio is the largest single
                // thing this handle ever writes, so a full pool shows up here
                // first and shows up as an instrument that will not load.
                if (!cli.setDeviceState(s.id, gen, text.c_str(),
                                        buf ? buf->data.data() : nullptr,
                                        buf ? buf->frames      : 0,
                                        buf ? buf->channels    : 0,
                                        buf ? buf->rate        : 0.0)) {
                    if (!s.stateSent && buf && !loggedStatePool) {
                        loggedStatePool = true;
                        LOGW("the engine would not take device %u's state yet (%s); "
                             "retrying every frame [further attempts silent]",
                             s.id, cli.error());
                    }
                    continue;
                }
                s.stateFinger = finger;
                s.stateFailed = false;
                s.stateSent   = true;
                ++statesSent;
            }
    }

    // An AddDevice has been answered. Bind the id, work out which of the GUI's
    // controls each of the daemon's is, and push every value at once.
    void bindDevice(int ci, PluginInstance* src, u32 id) {
        if (ci < 0 || ci >= kChainCount) return;
        DevSlot* slot = nullptr;
        for (DevSlot& s : chains[ci].live) if (s.src == src && !s.live) { slot = &s; break; }
        if (!slot) {
            // The GUI removed it again before the answer arrived. Undo the add
            // rather than leaving an instance sounding that nothing references.
            LOGW("device %u arrived for a chain slot that is gone; removing it", id);
            cli.removeDevice(id);
            return;
        }
        slot->id   = id;
        slot->live = true;
        ++devAdded;

        ipc::DeviceMirror m;
        const bool ok = cli.readDevice(id, m);

        RemoteDevice info;
        info.id         = id;
        info.generation = cli.deviceGeneration(id);
        info.live       = true;
        info.uri        = ok ? m.uri  : src->desc().uri;
        info.name       = ok ? m.name : src->desc().name;
        info.latencyFrames  = ok ? m.latencyFrames : 0;
        info.paramsTruncated = ok ? m.truncatedParams : 0;

        // docs/PARAM-ADDRESS.md: a control is named by ParamInfo::id, never by
        // its position. The two sides load the same plugin build so the ids
        // match — but neither promises the other an index ORDER, and a
        // positional guess that was wrong would move the wrong knob silently,
        // which is the worst failure this corner of the system has.
        const int n = src->paramCount();
        slot->map.assign((size_t)n, -1);
        slot->pushed.assign((size_t)n, 0.f);
        for (int i = 0; i < n; ++i) {
            const u32 pid = src->paramInfo(i).id;
            for (size_t j = 0; j < m.params.size(); ++j)
                if (m.params[j].id == pid) { slot->map[i] = (i32)j; break; }
            if (slot->map[i] >= 0) ++info.paramsMapped; else ++info.paramsUnmapped;
        }
        if (info.paramsUnmapped)
            LOGW("device %u ('%s'): %u of %d controls have no counterpart in the "
                 "engine's copy and will not follow the knob",
                 id, info.name.c_str(), info.paramsUnmapped, n);

        // Prime every value NOW rather than waiting for syncParams() to notice a
        // difference: a project load sets the parameters on the instance before
        // it publishes the chain, so the values are already right and nothing
        // would ever look changed.
        for (int i = 0; i < n; ++i) {
            if (slot->map[i] < 0) continue;
            const f32 v = src->getParam(i);
            if (cli.setDeviceParam(id, (u32)slot->map[i], v)) slot->pushed[i] = v;
        }
        slot->bypass = src->bypassed();
        if (slot->bypass) cli.setBypass(id, true);

        devInfo[src] = std::move(info);
        chains[ci].dirty = true;        // one more pass to declare it converged
    }

    void failDevice(int ci, PluginInstance* src, u32 reason) {
        ++devFailed;
        if (ci >= 0 && ci < kChainCount) {
            DevChain& ch = chains[ci];
            for (size_t i = 0; i < ch.live.size(); ++i)
                if (ch.live[i].src == src && !ch.live[i].live) {
                    ch.live.erase(ch.live.begin() + (long)i);
                    break;
                }
            // Tombstoned so reconcile() does not ask again every frame. Cleared
            // the moment the GUI stops asking for this instance.
            if (std::find(ch.refused.begin(), ch.refused.end(), src) == ch.refused.end())
                ch.refused.push_back(src);
            ch.dirty = true;
        }
        RemoteDevice info;
        info.failed = true;
        info.uri    = src ? src->desc().uri : std::string();
        info.name   = src ? src->desc().name : std::string();
        info.error  = ipc::rejectReasonName(reason);
        LOGE("the engine would not load '%s': %s", info.uri.c_str(), info.error.c_str());
        devInfo[src] = std::move(info);
    }

    // The daemon would not take a rack's contents. The fingerprint stays where
    // syncRacks() put it, which tombstones this exact state: it is not re-sent
    // until the user edits the rack again, and then it is. Silence here would be
    // a rack drawn full and sounding empty — the very thing this feature exists
    // to end — so it is logged once per device.
    void failRackState(u32 id, u32 reason) {
        ++racksFailed;
        for (int ci = 0; ci < kChainCount; ++ci)
            for (DevSlot& s : chains[ci].live)
                if (s.live && s.id == id) {
                    if (!s.rackFailed) {
                        s.rackFailed = true;
                        auto it = devInfo.find(s.src);
                        LOGE("the engine would not take the contents of the rack on "
                             "device %u ('%s'): %s. It is drawn full here and sounds "
                             "as whatever the engine already had.",
                             id, it != devInfo.end() ? it->second.name.c_str() : "?",
                             ipc::rejectReasonName(reason));
                        if (it != devInfo.end()) it->second.error = ipc::rejectReasonName(reason);
                    }
                    return;
                }
        LOGW("the engine refused a rack state for device %u, which is not on any "
             "chain we track: %s", id, ipc::rejectReasonName(reason));
    }

    // The daemon would not take a device's state. The fingerprint stays where
    // syncDeviceStates() put it, which tombstones this exact state: it is not
    // re-sent until the device changes again, and then it is. Silence here would
    // be a sampler drawn with a filename and sounding like nothing -- the very
    // thing this channel exists to end -- so it is logged once per device.
    void failDeviceState(u32 id, u32 reason) {
        ++statesFailed;
        for (int ci = 0; ci < kChainCount; ++ci)
            for (DevSlot& s : chains[ci].live)
                if (s.live && s.id == id) {
                    if (!s.stateFailed) {
                        s.stateFailed = true;
                        auto it = devInfo.find(s.src);
                        LOGE("the engine would not take the state of device %u ('%s'): "
                             "%s. It is drawn loaded here and sounds as whatever the "
                             "engine already had.",
                             id, it != devInfo.end() ? it->second.name.c_str() : "?",
                             ipc::rejectReasonName(reason));
                        if (it != devInfo.end()) it->second.error = ipc::rejectReasonName(reason);
                    }
                    return;
                }
        LOGW("the engine refused a device state for device %u, which is not on any "
             "chain we track: %s", id, ipc::rejectReasonName(reason));
    }

    u32 pendingDevices() const { return (u32)pendingAdds.size(); }

    // Everything the daemon knew is gone with it. Device ids are the DEAD
    // engine's and mean nothing to its replacement (§11.4), so every chain goes
    // back to "wanted, nothing there" and reconcile() rebuilds it from the
    // instances the GUI still holds.
    void forgetDevices() {
        for (int ci = 0; ci < kChainCount; ++ci) {
            chains[ci].live.clear();
            chains[ci].refused.clear();
            chains[ci].dirty = !chains[ci].want.empty();
        }
        pendingAdds.clear();
        devInfo.clear();
    }

    // ---------------------------------------------------------------------
    // The catalog (§5 step 5)
    // ---------------------------------------------------------------------

    void readCatalog() {
        std::vector<ipc::CatalogEntry> rows;
        cli.readCatalog(rows);
        catalog.clear();
        catalog.reserve(rows.size());
        for (const ipc::CatalogEntry& e : rows) {
            PluginDesc d;
            d.uri        = e.uri;
            d.name       = e.name;
            d.vendor     = e.vendor;
            d.category   = e.category;
            d.format     = (PluginFormat)e.format;
            d.kind       = (PluginKind)e.kind;
            d.audioIn    = (int)e.audioIn;
            d.audioOut   = (int)e.audioOut;
            d.hasMidiIn  = e.hasMidiIn;
            d.paramCount = (int)e.paramCount;
            catalog.push_back(std::move(d));
        }
        catalogCut  = cli.catalogTruncated();
        catalogRead = true;
        LOGI("the engine's plugin catalog: %zu plugins%s", catalog.size(),
             catalogCut ? " (and more than the table can carry)" : "");
        if (catalogCut)
            LOGW("%u plugin(s) did not fit the catalog table and cannot be browsed",
                 catalogCut);
    }

    // ---------------------------------------------------------------------
    // Events
    // ---------------------------------------------------------------------

    // Drains the wire into `synth`. Called from poll(), so the boundary's own
    // bookkeeping advances on the frame clock rather than on whether the caller
    // felt like asking for an event.
    void pumpWire() {
        ipc::WireEvent w;
        while (cli.popEvent(w)) {
            if (w.type >= ipc::kDaemonEventBase) { observeDaemon(w); continue; }
            // A pointer-carrying engine event cannot have crossed — the daemon
            // does not forward them — so anything here is a scalar one.
            if (!ipc::eventIsScalar(w.type)) continue;
            Event e;
            e.type = (Ev)w.type;
            e.a = w.a; e.b = w.b; e.x = w.x;
            e.p = nullptr;
            synth.push_back(e);
        }
    }

    bool pop(Event& e) {
        // One queue, in arrival order: the wire's events as pumpWire() took
        // them off the ring, and the synthesised retirements interleaved where
        // the pushes that caused them happened.
        if (synth.empty()) pumpWire();
        if (synth.empty()) return false;
        e = synth.front();
        synth.pop_front();
        return true;
    }

    void observeDaemon(const ipc::WireEvent& w) {
        switch (w.type) {
            case ipc::EvCommandRejected:
                LOGW("the engine refused %s: %s",
                     cmdName((Cmd)w.a), ipc::rejectReasonName((u32)w.b));
                break;
            case ipc::EvEngineStopping:
                stopping = true;
                LOGW("the engine is shutting down");
                break;
            case ipc::EvEventDropped:
                LOGW("the engine dropped an event that could not cross (%d)", (int)w.a);
                break;
            case ipc::EvPoolAttached:
                LOGI("the engine mapped the sample pool (%.0f B, epoch %llu)",
                     w.x, (unsigned long long)w.ref);
                break;

            // The daemon answers device commands strictly in the order it
            // dequeued them, so the front of pendingAdds is the request this
            // answers. EvDeviceFailed is shared with Remove/Move/SetBypass,
            // which is why `x` (the command type) has to be checked: popping
            // the queue on somebody else's failure would bind the next add's id
            // to the wrong instance.
            case ipc::EvDeviceAdded: {
                if (pendingAdds.empty()) {
                    LOGW("EvDeviceAdded for device %llu with nothing outstanding",
                         (unsigned long long)w.ref);
                    break;
                }
                const PendingAdd pa = pendingAdds.front();
                pendingAdds.pop_front();
                bindDevice(pa.chain, pa.src, (u32)w.ref);
                break;
            }
            case ipc::EvDeviceFailed: {
                // A rack-state refusal is answered against a DEVICE ID (`a`),
                // not against the pendingAdds queue: it is not an add, so
                // popping that queue for it would bind the next add's id to the
                // wrong instance — the exact bug the `x` check below exists to
                // prevent, one command wider.
                if ((u32)w.x == ipc::CmdSetRackState) { failRackState((u32)w.a, (u32)w.b); break; }
                if ((u32)w.x == ipc::CmdSetDeviceState) { failDeviceState((u32)w.a, (u32)w.b); break; }
                if ((u32)w.x != ipc::CmdAddDevice) {
                    LOGW("the engine refused a device command: %s",
                         ipc::rejectReasonName((u32)w.b));
                    break;
                }
                if (pendingAdds.empty()) break;
                const PendingAdd pa = pendingAdds.front();
                pendingAdds.pop_front();
                failDevice(pa.chain, pa.src, (u32)w.b);
                break;
            }
            case ipc::EvScanComplete:
                readCatalog();
                break;

            // The client's own bookkeeping already ran in popEvent()'s
            // observe() — the blob freed or kept, the blocks it named marked
            // Live or displaced. What is left is the part only a UI can do:
            // SAY SO. A refused lane is a timeline drawn on screen that does not
            // play, and the acknowledgement carries the reason.
            case ipc::EvSignaturesAck:
                if (w.flags & ipc::SigAckRefused) {
                    ++sigsRefused;
                    LOGE("the engine refused the signature map: %s. The set is drawn "
                         "in its own metre here and PLAYS IN 4/4.",
                         ipc::rejectReasonName((u32)w.x));
                }
                break;
            case ipc::EvArrangementAck:
                if (w.flags & ipc::ArrAckRefused) {
                    ++arrRefused;
                    LOGE("the engine refused %s for %s: %s. That part of the "
                         "timeline is drawn here and does not play.",
                         (w.flags & ipc::ArrAckAutos) ? "the automation"
                                                      : "the arrangement",
                         w.a == -1 ? "the transport cell" : "a track",
                         ipc::rejectReasonName((u32)w.x));
                }
                break;
            case ipc::EvDeviceChanged:
                // Ours to have caused: we are the only writer of this chain, of
                // this bypass flag and of this rack's contents, so the model
                // already knows. The client's own bookkeeping (the slot
                // generation the param guard is stamped with) ran in
                // popEvent()'s observe(), which is the part that matters.
                //
                // One field is NOT ours and has to be taken: a rack's latency is
                // the sum of the chain the DAEMON built, and RACKS.md §1 is
                // explicit that the figure is not constant after prepare(). Ours
                // would be the sum of the GUI's copy, which renders nothing.
                if (w.flags & (ipc::DeviceChangedRackState | ipc::DeviceChangedState))
                    for (int ci = 0; ci < kChainCount; ++ci)
                        for (DevSlot& s : chains[ci].live)
                            if (s.live && s.id == (u32)w.ref) {
                                auto it = devInfo.find(s.src);
                                if (it != devInfo.end()) it->second.latencyFrames = w.a;
                            }
                break;
            case ipc::EvDeviceRemoved:
                break;

            // §7. The take came back as a filename; App gets it as the event it
            // has been waiting for since it pushed the command.
            case ipc::EvTakeReady:
                takeReady(w);
                break;
            case ipc::EvTakeFailed: {
                PendingTake* pp = takeOn(w.a);
                LOGE("the engine refused a take on track %d slot %d: %s",
                     (int)w.a, (int)w.b, ipc::rejectReasonName((u32)w.x));
                if (!pp) break;
                const PendingTake p = *pp;
                forgetTake(w.a);
                ++takesFailed;
                returnTake(p, 0);
                break;
            }
            default:
                // EvClipAck and EvBlockRetired are already applied by
                // EngineClient::popEvent()'s observe().
                break;
        }
    }

    // ---------------------------------------------------------------------
    // Lifecycle (§6)
    // ---------------------------------------------------------------------

    // A DAEMON WE SPAWNED IS OUR CHILD, AND A CHILD NOBODY WAITS FOR IS A
    // ZOMBIE. This is not housekeeping: `processAlive(pid, startTicks)` reads
    // /proc, and a zombie still has a /proc entry with the same start ticks —
    // so a GUI that started its own daemon would watch it be SIGKILLed and go
    // on reporting it alive for the rest of the session, which is precisely the
    // one state §4.4 says may be acted on automatically. The bug is invisible
    // to a client that merely ATTACHED (no child, no zombie), which is why
    // daemon_test never saw it.
    //
    // WNOHANG, from poll(), on the GUI thread: never blocks, never reaps
    // anything that is not ours.
    void reapChild() {
        if (spawned <= 0) return;
        int st = 0;
        const pid_t r = ::waitpid(spawned, &st, WNOHANG);
        if (r == spawned || (r < 0 && errno == ECHILD)) {
            spawned    = -1;        // nothing left to signal in close()
            spawnedDied = true;
        }
    }

    EngineLink linkState() {
        if (!cli.attached()) return EngineLink::Detached;
        if (stopping)        return EngineLink::Stopping;
        // The engine we started has exited. Dead is dead whatever the region
        // still says, and it is the one state that may provoke a restart.
        if (spawnedDied)     return EngineLink::Lost;
        // alive() with an unreachable tolerance answers only the two questions
        // staleness is not: has the creator gone, and did it publish the
        // shutdown flag. Both are "dead", and dead is the ONLY state that may
        // provoke an automatic restart (§4.4).
        if (!cli.alive(~0ull))  return EngineLink::Lost;
        if (cli.alive())        return EngineLink::Live;
        // Attached, the process is there, but nothing has been published
        // recently. A daemon between fork() and its first pump tick looks
        // exactly like this, so it is Starting until it has ever beaten.
        return lastHb == 0 ? EngineLink::Starting : EngineLink::Stale;
    }

    u32 silentMs() const {
        if (!lastHbNs) return 0;
        const u64 now = ipc::monotonicNs();
        return now > lastHbNs ? (u32)((now - lastHbNs) / 1000000ull) : 0u;
    }

    void noteHeartbeat() {
        const u64 hb = cli.heartbeat();
        if (hb != lastHb || !lastHbNs) { lastHb = hb; lastHbNs = ipc::monotonicNs(); }
    }

    // The scalars a replacement engine has to be told again.
    void shadowScalar(const Command& c) {
        if (!cmdIsResyncState(c.type)) return;
        const u64 key = ((u64)(u32)c.type << 40) ^ ((u64)(u32)c.a << 20) ^ (u64)(u32)c.b;
        auto it = scalarShadow.find(key);
        if (it != scalarShadow.end()) { it->second = c; return; }
        if (scalarShadow.size() >= kMaxShadow) {
            if (!loggedShadowFull) {
                loggedShadowFull = true;
                LOGW("the scalar shadow is full (%zu); a restarted engine may come back "
                     "with some mixer values missing", kMaxShadow);
            }
            return;
        }
        scalarShadow.emplace(key, c);
    }

    bool spawnAndAttach(int timeoutMs) {
        const std::string path = daemonPath();
        const char* args[6];
        int n = 0;
        args[n++] = "--session";
        args[n++] = session.c_str();
        if (!driver.empty()) { args[n++] = "--driver"; args[n++] = driver.c_str(); }
        args[n] = nullptr;
        spawned = ipc::EngineClient::spawnDaemon(path.c_str(), args);
        if (spawned < 0) { LOGE("could not fork a daemon (%s)", path.c_str()); return false; }
        if (!cli.attach(session.c_str(), timeoutMs)) {
            LOGE("the engine did not come up: %s", cli.error());
            ::kill(spawned, SIGTERM);
            ipc::EngineClient::waitFor(spawned, 1000);
            spawned = -1;
            return false;
        }
        return true;
    }

    void stopSpawned() {
        if (spawned <= 0) { spawned = -1; return; }
        ::kill(spawned, SIGTERM);
        if (!ipc::EngineClient::waitFor(spawned, 2000)) {
            LOGW("the engine did not stop on SIGTERM; killing it");
            ::kill(spawned, SIGKILL);
            ipc::EngineClient::waitFor(spawned, 1000);
        }
        spawned = -1;
    }

    // §6's recovery. The pleasant part is that the POOL SURVIVES: it is the
    // GUI's region, it was never unlinked, and attach() re-announces it — so
    // putting a set back is a memcpy of the clip table plus one SetClip per
    // occupied cell. Nothing is decoded and no offset changes.
    //
    // What does NOT survive is device ids, so every chain is re-issued from
    // scratch, and the transport, which comes back stopped on purpose.
    bool restart() {
        LOGW("restarting the audio engine");
        stopSpawned();
        // detach(), not close(): closePool() would unlink the samples, which
        // are the one thing worth keeping across this.
        cli.detach();
        ipc::EngineClient::reapStale(session.c_str());
        forgetDevices();
        synth.clear();
        // AFTER the clear, not before: cancelTakes() answers each outstanding
        // take through `synth`, and an answer wiped by the line above is a
        // capture buffer App pins for the rest of the session.
        cancelTakes("the engine was restarted");
        stopping    = false;
        spawnedDied = false;
        loggedLost  = false;
        lastHb     = 0;
        lastHbNs   = 0;
        catalogRead = false;

        if (!spawnAndAttach(4000)) return false;

        std::snprintf(driverName, sizeof driverName, "daemon:%s", cli.header().driverName);
        const f64 r = cli.sampleRate();
        if (r >= 8000.0 && r <= 384000.0) rate = r;
        block = cli.blockSize();
        openedNs = ipc::monotonicNs();

        const int cells = cli.republishClips();
        // The blobs are still in the pool — it is the session's region and it
        // outlives the engine — so putting the timeline back is one command per
        // occupied lane and no re-encoding of anything (§10.7 step 3).
        const int lanes = cli.republishArrangements();
        const int sigs  = cli.republishSignatures();
        // Clips first, then scalars: a republished SetClip carries the cell's
        // gain/warp/loop from the shadow, and a Cmd::ClipGain the user moved
        // afterwards is in the scalar shadow. Replaying the scalars last is
        // what makes the later of the two win, which is the one that is right.
        int scalars = 0;
        for (const auto& kv : scalarShadow)
            if (cli.pushCommand(kv.second.type, kv.second.a, kv.second.b, kv.second.x))
                ++scalars;

        ++resyncs;
        LOGI("engine restarted: pid %d, %d clip cell(s), %d arrangement lane(s), "
             "%d signature map(s) and %d scalar(s) republished, %zu chain(s) queued "
             "for rebuild. The transport is stopped.",
             (int)cli.enginePid(), cells, lanes, sigs, scalars, (size_t)std::count_if(
                 chains, chains + kChainCount, [](const DevChain& c) { return c.dirty; }));
        return true;
    }
};

// ===========================================================================
// EngineHandle
// ===========================================================================

EngineHandle::EngineHandle()  = default;
EngineHandle::~EngineHandle() = default;

bool EngineHandle::open(const char* driver) {
    const char* which = env("ENGINE");
    if (which && (!std::strcmp(which, "daemon") || !std::strcmp(which, "remote"))) {
        const char* sess = env("SESSION");
        if (openDaemon(sess, driver)) return true;
        // §8's exception, and the reason this does not silently fall back to a
        // local engine: two engines is worse than none. A GUI that cannot reach
        // a daemon opens anyway, loads the project, edits and saves — with every
        // send() a no-op and a log line saying so — because the case a fallback
        // is actually for is a broken audio setup on somebody else's machine,
        // and quietly starting a second engine under a wedged one is §4.4's
        // worst available outcome.
        LOGE("NXTAKT_ENGINE=daemon but no engine could be reached: "
             "running with no engine at all (the set can still be edited and saved)");
        return true;
    }
    return openLocalEngine(driver);
}

bool EngineHandle::openLocalEngine(const char* driver) {
    engine_ = std::unique_ptr<Engine>(new (std::nothrow) Engine());
    if (!engine_) { LOGE("could not allocate the engine"); return false; }

    audio_ = createBackend(*engine_, driver);
    if (!audio_) {
        // Not an error. A set can be edited, saved and looked at with no audio
        // device at all, and refusing to start would make a broken ALSA
        // configuration on somebody else's machine fatal.
        LOGW("no audio backend available - running silent");
        engine_->prepare(48000.0, 1024);
    }

    // MIDI comes up after the audio backend: the reader thread pushes straight
    // into the engine's ring, so the engine must already be prepared. Missing
    // hardware or a missing sequencer device is not an error - a set can be
    // played entirely from the mouse.
    if (midi_.start(*engine_)) LOGI("midi in: alsa seq client %d:0", midi_.clientId());
    else                       LOGW("no MIDI input - continuing without it");
    return true;
}

bool EngineHandle::openDaemon(const char* session, const char* driver) {
    auto r = std::unique_ptr<RemoteEngine>(new (std::nothrow) RemoteEngine());
    if (!r) { LOGE("could not allocate the engine client"); return false; }
    if (!r->open(session, driver)) return false;
    remote_ = std::move(r);

    // HARDWARE MIDI, and this is §1.3's option 3 rather than its option 1.
    //
    // MidiInput no longer takes an Engine&; it takes a sink, and the sink is the
    // one place in the program that knows where the engine is. So the ALSA
    // client stays in the process the user's aconnect wiring already names, and
    // the reader thread's messages go over the wire instead of into a ring that
    // is not there.
    //
    // THE LOCK IS THE POINT. Locally the engine has two MIDI rings — one for the
    // reader thread, one for the GUI (engine.h, pushMidi / pushMidiFromGui) — so
    // each has exactly one producer. There is only ONE MIDI ring in the control
    // region, and the GUI thread is already pushing the computer keyboard and
    // the note previews into it, so the reader would be a second producer on a
    // structure lat::Ring documents as single-producer. Two concurrent pushes
    // can write the same slot and publish one index; it takes playing the
    // computer keyboard and a controller in the same microsecond, and it is
    // real. Neither producer is realtime, so a mutex costs nothing that matters
    // — §1.3 recommends exactly this and rejects the frame-of-latency handoff
    // because you can hear that one.
    if (midi_.start([this](const MidiMsg& m) {
            std::lock_guard<std::mutex> lk(midiMx_);
            return remote_ && remote_->cli.pushMidi(m.status, m.d1, m.d2, m.frame);
        }))
        LOGI("midi in: alsa seq client %d:0, forwarded to the engine over the wire",
             midi_.clientId());
    else
        LOGW("no MIDI input - continuing without it");
    return true;
}

void EngineHandle::close() {
    // Order matters, and it is the order App::shutdown() used to spell out
    // inline. The MIDI reader goes first: it pushes into the engine's ring from
    // its own thread, so it has to be joined before anything else starts
    // tearing the engine down, or a push could land in a ring nobody owns any
    // more. Stopping the backend then joins the audio thread.
    midi_.stop();
    if (audio_) { audio_->stop(); audio_.reset(); }
    // engine_ is deliberately NOT released here: App frees the chains, note
    // arrays and capture buffers the engine was borrowing *after* this returns,
    // and one of the debug hooks still reaches it through local(). It dies with
    // the handle, which dies with App.
    if (remote_) { remote_->close(); remote_.reset(); }
}

// ---------------------------------------------------------------------------
// The snapshot
// ---------------------------------------------------------------------------
//
// One tight copy per frame. What this fixes and what it does not, precisely:
//
//   It removes the INTRA-FRAME incoherence, which is the one that showed. The
//   four reads drawClipSlot used to make were separated by whatever the draw
//   code did in between — easily a millisecond, i.e. several audio blocks at
//   256 frames — so a slot could be drawn Playing with activeSlot == -1. After
//   this they are one copy taken microseconds apart.
//
//   LOCALLY it does NOT make the copy itself atomic against Engine::publish().
//   There is nothing to gate on: publish() bumps no generation counter, and
//   blocksRendered is incremented at the TOP of process() while publish() runs
//   at the bottom, so a reader that saw the same blocksRendered either side of
//   its copy could still have straddled the publish for that block. engine.h is
//   frozen, so adding one is not on the table — and the remaining window is the
//   duration of this function.
//
//   REMOTELY it closes completely, and that is what step 2 bought: SharedState
//   is a seqlock as of shm v5 (odd while the daemon's mirror is writing), so the
//   copy below runs inside readCoherent() and is retried until it provably did
//   not straddle a publish. A copy that could not be proved coherent after eight
//   tries is counted in snapshotTears() and handed over anyway, because a UI
//   that hangs on a stopped daemon is worse than one that draws a stale frame.
void EngineHandle::poll(EngineState& out) {
    if (remote_) {
        const ipc::SharedState& s = remote_->cli.state();
        const bool ok = s.readCoherent([&] {
            out.beat    = s.beat.load(std::memory_order_relaxed);
            out.tempo   = s.tempo.load(std::memory_order_relaxed);
            out.playing = s.playing.load(std::memory_order_relaxed) != 0;
            out.cpu     = s.cpu.load(std::memory_order_relaxed);

            out.posBar       = s.posBar.load(std::memory_order_relaxed);
            out.posBeat      = s.posBeat.load(std::memory_order_relaxed);
            out.posSixteenth = s.posSixteenth.load(std::memory_order_relaxed);
            out.posSigNum    = s.posSigNum.load(std::memory_order_relaxed);
            out.posSigDen    = s.posSigDen.load(std::memory_order_relaxed);

            out.sampleRate    = s.sampleRate.load(std::memory_order_relaxed);
            out.blockSize     = s.blockSize.load(std::memory_order_relaxed);
            out.latencyFrames = s.latencyFrames.load(std::memory_order_relaxed);

            for (int t = 0; t < kMaxTracks; ++t) {
                out.slotState[t]   = s.slotState[t].load(std::memory_order_relaxed);
                out.activeSlot[t]  = s.activeSlot[t].load(std::memory_order_relaxed);
                out.pendingSlot[t] = s.pendingSlot[t].load(std::memory_order_relaxed);
                out.clipPhase[t]   = s.clipPhase[t].load(std::memory_order_relaxed);
                out.meterL[t]      = s.meterL[t].load(std::memory_order_relaxed);
                out.meterR[t]      = s.meterR[t].load(std::memory_order_relaxed);
                out.recState[t]    = s.recState[t].load(std::memory_order_relaxed);
                out.recSlotIdx[t]  = s.recSlotIdx[t].load(std::memory_order_relaxed);
            }
            for (int i = 0; i < kMaxReturns; ++i) {
                out.returnMeterL[i] = s.returnMeterL[i].load(std::memory_order_relaxed);
                out.returnMeterR[i] = s.returnMeterR[i].load(std::memory_order_relaxed);
            }
            out.masterMeterL = s.masterMeterL.load(std::memory_order_relaxed);
            out.masterMeterR = s.masterMeterR.load(std::memory_order_relaxed);

            out.arrOverride    = s.arrOverride.load(std::memory_order_relaxed);
            out.journalDropped = s.journalDropped.load(std::memory_order_relaxed);
        });
        if (!ok) ++remote_->tears;
        // A detached client hands back a zeroed block, in which sampleRate is 0.
        // Every other field reading as a stopped transport is correct; that one
        // is not, because the status bar prints it and because a 0 here next to
        // a live sampleRate() accessor would look like a contradiction.
        if (out.sampleRate <= 0.0) out.sampleRate = remote_->rate;

        // The link, once. §4.4's rule is that a stale heartbeat is NOT grounds
        // for respawning — a laptop resuming from suspend and a JACK restart
        // both look exactly like a wedged engine for a few hundred milliseconds,
        // and a second daemon under a live one is the worst available outcome.
        // So this notices and says so, and does nothing else: a restart happens
        // on a user click, through restartEngine().
        remote_->reapChild();
        remote_->noteHeartbeat();
        out.link         = remote_->linkState();
        out.linkSilentMs = out.link == EngineLink::Live ? 0u : remote_->silentMs();
        if (!remote_->cli.alive() && !remote_->loggedLost) {
            remote_->loggedLost = true;
            LOGW("the audio engine stopped answering. Your set is intact; "
                 "use Restart engine to reconnect.");
        }

        // THE DAEMON-CRASH ARM OF THE CRASH MATRIX. A take whose engine is
        // provably gone will never be answered, and App frees its capture buffer
        // on the answer and on nothing else — so a GUI that merely reported the
        // lost engine and waited would sit there with the buffer pinned and the
        // slot stuck in "recording" for the rest of the session. Keyed on Lost
        // and not on Stale, for §4.4's reason: a laptop coming out of suspend is
        // not a dead engine, and cancelling a take for it would throw away a
        // performance that is still being made.
        if (out.link == EngineLink::Lost) remote_->cancelTakes("the engine was lost");

        // The frame's housekeeping, in the one order that converges: take the
        // wire's answers first (an EvDeviceAdded arriving now binds an id this
        // pass can already use), then make the daemon's chains match the ones
        // the GUI declared, then mirror the knobs.
        remote_->pumpWire();
        remote_->pumpTakeReleases();
        remote_->reconcile();
        remote_->syncParams();
        remote_->syncRacks();
        remote_->syncDeviceStates();
        out.devicesPending = remote_->pendingDevices();
        return;
    }

    const Engine* e = engine_.get();
    if (!e) { out = EngineState{}; return; }        // detached: a stopped transport

    out.beat    = e->beat.load(std::memory_order_relaxed);
    out.tempo   = e->tempo.load(std::memory_order_relaxed);
    out.playing = e->playing.load(std::memory_order_relaxed);
    out.cpu     = e->cpu.load(std::memory_order_relaxed);

    // The engine's own bars.beats.sixteenths and the signature at the playhead.
    // Read here rather than recomputed by the transport bar from the session's
    // map, because the two differ exactly when a map was REFUSED — see the note
    // in engine_state.h.
    out.posBar       = e->posBar.load(std::memory_order_relaxed);
    out.posBeat      = e->posBeat.load(std::memory_order_relaxed);
    out.posSixteenth = e->posSixteenth.load(std::memory_order_relaxed);
    out.posSigNum    = e->posSigNum.load(std::memory_order_relaxed);
    out.posSigDen    = e->posSigDen.load(std::memory_order_relaxed);

    out.sampleRate    = e->sampleRate();
    out.blockSize     = audio_ ? (u32)audio_->bufferSize() : 0u;
    out.latencyFrames = e->latencyFrames.load(std::memory_order_relaxed);

    for (int t = 0; t < kMaxTracks; ++t) {
        out.slotState[t]   = e->slotState[t].load(std::memory_order_relaxed);
        out.activeSlot[t]  = e->activeSlot[t].load(std::memory_order_relaxed);
        out.pendingSlot[t] = e->pendingSlot[t].load(std::memory_order_relaxed);
        out.clipPhase[t]   = e->clipPhase[t].load(std::memory_order_relaxed);
        out.meterL[t]      = e->meterL[t].load(std::memory_order_relaxed);
        out.meterR[t]      = e->meterR[t].load(std::memory_order_relaxed);
        out.recState[t]    = e->recState[t].load(std::memory_order_relaxed);
        out.recSlotIdx[t]  = e->recSlotIdx[t].load(std::memory_order_relaxed);
    }
    for (int i = 0; i < kMaxReturns; ++i) {
        out.returnMeterL[i] = e->returnMeterL[i].load(std::memory_order_relaxed);
        out.returnMeterR[i] = e->returnMeterR[i].load(std::memory_order_relaxed);
    }
    out.masterMeterL = e->masterMeterL.load(std::memory_order_relaxed);
    out.masterMeterR = e->masterMeterR.load(std::memory_order_relaxed);

    out.arrOverride    = e->arrOverride.load(std::memory_order_relaxed);
    out.journalDropped = e->journalDropped.load(std::memory_order_relaxed);

    // An in-process engine cannot be stale (it is this process) and cannot be
    // lost (it dies with us), so Live is the only thing local mode can honestly
    // report — and the `!e` path above already returned Detached.
    out.link           = EngineLink::Live;
    out.linkSilentMs   = 0;
    out.devicesPending = 0;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

bool EngineHandle::send(Cmd t, i32 a, i32 b, f64 x) {
    Command c;
    c.type = t; c.a = a; c.b = b; c.x = x;
    return pushCommand(c);
}

bool EngineHandle::pushCommand(const Command& c) {
    if (remote_) return remote_->push(c);
    return engine_ ? engine_->pushCommand(c) : false;
}

bool EngineHandle::pushMidi(const MidiMsg& m) {
    if (remote_) {
        // The same lock the hardware reader's sink takes. The two are the two
        // producers on ONE shared-memory ring; see openDaemon().
        std::lock_guard<std::mutex> lk(midiMx_);
        return remote_->cli.pushMidi(m.status, m.d1, m.d2, m.frame);
    }
    // Locally the engine keeps a ring per producer, so this needs no lock: the
    // reader thread is on pushMidi() and this is pushMidiFromGui().
    return engine_ ? engine_->pushMidiFromGui(m) : false;
}

bool EngineHandle::popEvent(Event& e) {
    if (remote_) return remote_->pop(e);
    return engine_ ? engine_->popEvent(e) : false;
}

f64 EngineHandle::sampleRate() const {
    if (remote_) {
        // The live wire value, and the cached one only if the region has gone.
        // Never 0: every caller of this resamples with it.
        const f64 r = remote_->cli.sampleRate();
        return (r >= 8000.0 && r <= 384000.0) ? r : remote_->rate;
    }
    return engine_ ? engine_->sampleRate() : 48000.0;
}

u32 EngineHandle::journalDropped() const {
    if (remote_) {
        // TWO hops can lose a journal entry across the boundary — the engine's
        // ring into the daemon's pump, and the pump's ring into ours — and §5.4
        // refuses a take on either. The sum is what a caller asking "was
        // anything lost?" means, and it is monotonic like the parts.
        const u64 sum = (u64)remote_->cli.engineJournalDropped() + remote_->cli.journalDropped();
        return sum > 0xffffffffull ? 0xffffffffu : (u32)sum;
    }
    return engine_ ? engine_->journalDropped.load(std::memory_order_relaxed) : 0u;
}

// ---------------------------------------------------------------------------
// Backend description
// ---------------------------------------------------------------------------

const char* EngineHandle::driverName() const {
    if (remote_) return remote_->driverName;
    return audio_ ? audio_->name() : nullptr;
}
f64 EngineHandle::driverSampleRate() const {
    if (remote_) return sampleRate();
    return audio_ ? audio_->sampleRate() : 0.0;
}
int EngineHandle::driverBufferSize() const {
    if (remote_) return (int)remote_->cli.blockSize();
    return audio_ ? audio_->bufferSize() : 0;
}
// All three answer "no" in daemon mode rather than lying about the GUI's own
// (unstarted) MidiInput. The status bar draws exactly that, which is the
// intended reading: there is no hardware MIDI on this path yet.
// All three answer for the real MidiInput on BOTH paths now. They used to say
// "no" in daemon mode, which was honest at the time (the reader was not started)
// and would be a lie today.
bool EngineHandle::midiRunning() const { return midi_.running(); }
int  EngineHandle::midiClientId() const { return midi_.clientId(); }
u64  EngineHandle::midiReceived() const { return midi_.received(); }

u64 EngineHandle::remoteRefusals() const { return remote_ ? remote_->refusals : 0u; }
u64 EngineHandle::snapshotTears() const  { return remote_ ? remote_->tears : 0u; }

// The pool copy, read back through the same two indirections the daemon uses:
// the clip cell says which block, the pool says where it is. Nothing here reads
// the GUI's own RtNote[] — that would defeat the point.
i64 EngineHandle::publishedNotes(int track, int slot, RtNote* out, i64 max) const {
    if (!remote_) return -1;
    const ipc::WireClip& c = remote_->cli.clipShadow(track, slot);
    if (!c.notesRef || c.noteCount <= 0) return 0;
    const ipc::WireNote* blk = remote_->cli.pool().data<ipc::WireNote>(c.notesRef);
    if (!blk) return 0;                         // the block went away under us
    if (out && max > 0) {
        // Reinterpreted, not converted — WireNote mirrors RtNote field for
        // field (pool.h asserts every offset), and this is the same cast
        // nxtaktd::buildClip makes on the far side. Reading it any other way
        // would be reading something other than what the daemon plays.
        const RtNote* src = (const RtNote*)blk;
        const i64 n = c.noteCount < max ? c.noteCount : max;
        for (i64 i = 0; i < n; ++i) out[i] = src[i];
    }
    return c.noteCount;
}

// ---------------------------------------------------------------------------
// Devices, the catalog, the link
// ---------------------------------------------------------------------------

const RemoteDevice* EngineHandle::remoteDevice(const PluginInstance* gui) const {
    if (!remote_ || !gui) return nullptr;
    auto it = remote_->devInfo.find(gui);
    return it == remote_->devInfo.end() ? nullptr : &it->second;
}
u32 EngineHandle::devicesPending() const { return remote_ ? remote_->pendingDevices() : 0u; }
u64 EngineHandle::devicesAdded() const   { return remote_ ? remote_->devAdded : 0u; }
u64 EngineHandle::devicesFailed() const  { return remote_ ? remote_->devFailed : 0u; }
u64 EngineHandle::racksPublished() const { return remote_ ? remote_->racksSent : 0u; }
u64 EngineHandle::racksRefused() const   { return remote_ ? remote_->racksFailed : 0u; }
u64 EngineHandle::deviceStatesPublished() const { return remote_ ? remote_->statesSent : 0u; }
u64 EngineHandle::deviceStatesRefused() const   { return remote_ ? remote_->statesFailed : 0u; }
u64 EngineHandle::arrangementsPublished() const { return remote_ ? remote_->arrPublished : 0u; }
u64 EngineHandle::arrangementsRefused() const   { return remote_ ? remote_->arrRefused : 0u; }
u64 EngineHandle::poolBlocksLive() const {
    return remote_ ? remote_->cli.pool().liveBlocks() : 0u;
}
u64 EngineHandle::signaturesPublished() const { return remote_ ? remote_->sigsPublished : 0u; }
u64 EngineHandle::signaturesRefused() const   { return remote_ ? remote_->sigsRefused : 0u; }
// All four are 0 in local mode, and that is the honest answer rather than a
// missing one: a local take is handed straight back by the engine and none of
// these four states can arise.
u64 EngineHandle::takesReturned() const { return remote_ ? remote_->takesReturned : 0u; }
u64 EngineHandle::takesEmpty() const    { return remote_ ? remote_->takesEmpty : 0u; }
u64 EngineHandle::takesFailed() const   { return remote_ ? remote_->takesFailed : 0u; }
u64 EngineHandle::takesLost() const     { return remote_ ? remote_->takesLost : 0u; }

const std::vector<PluginDesc>& EngineHandle::catalog() const {
    // Empty in local mode ON PURPOSE, and callers must read it that way: there
    // the process's own PluginRegistry IS the engine's registry, so a second
    // copy of the same list would be two things to keep in step for no gain.
    static const std::vector<PluginDesc> kNone;
    return remote_ ? remote_->catalog : kNone;
}
u32  EngineHandle::catalogTruncated() const { return remote_ ? remote_->catalogCut : 0u; }
bool EngineHandle::catalogReady() const     { return remote_ ? remote_->catalogRead : false; }
bool EngineHandle::scanRunning() const {
    return remote_ && remote_->cli.scanState() == ipc::ScanRunning;
}
bool EngineHandle::requestScan() {
    // Nothing to ask for in local mode: App::ensurePluginScan() runs the scan
    // in this process and that is the same catalog.
    if (!remote_) return false;
    if (remote_->catalogRead) return true;
    if (remote_->cli.scanState() == ipc::ScanDone) { remote_->readCatalog(); return true; }
    return remote_->cli.scanPlugins();
}

EngineLink EngineHandle::link() const {
    if (remote_) return remote_->linkState();
    return engine_ ? EngineLink::Live : EngineLink::Detached;
}

i32 EngineHandle::enginePid() const {
    return remote_ ? remote_->cli.enginePid() : -1;
}

bool EngineHandle::restartEngine() {
    if (!remote_) return false;
    // Under the MIDI lock: restart() detaches the client and re-attaches it, and
    // the hardware reader thread is pushing into that same client from its own
    // thread. Without this, a note arriving during the respawn dereferences a
    // ring the detach has already unmapped.
    std::lock_guard<std::mutex> lk(midiMx_);
    return remote_->restart();
}
u64 EngineHandle::resyncs() const { return remote_ ? remote_->resyncs : 0u; }

} // namespace lat
