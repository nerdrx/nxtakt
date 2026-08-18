// EngineHandle — the one thing in the UI that knows where the engine is.
//
// docs/GUI-ON-DAEMON.md §2.1, option (b)+(c): a CONCRETE class holding whichever
// backing it was opened with, no vtable. The doc rejects a virtual IEngine
// explicitly and the reason is worth keeping in view: the dominant call class
// used to be twenty polled atomics read ~3 000 times a frame, so a virtual
// getter per field would have been twenty virtuals on the hot path plus twenty
// pairs of near-identical one-line overrides to keep in sync — an abstraction
// paying for the wrong axis. The snapshot (poll(), engine_state.h) removes the
// hot path entirely, and what is left is a handful of calls a frame where a
// branch on a member that never changes after init() predicts perfectly.
//
// ONE BACKING ON LINUX, AS OF §8(3)'s COLLECT (GUI-ON-DAEMON.md §18)
// ------------------------------------------------------------------
//   daemon   an ipc::EngineClient talking to nxtaktd over the control region,
//            with the GUI's samples living in an ipc::SamplePool it owns.
//            The default since §8 step 2's flip (§16), and since §18 the ONLY
//            engine this class can open on Linux: the in-process path that
//            step 1 shipped left the App link one release after the flip, on
//            the flip's own schedule. NXTAKT_ENGINE=local (and the doc's
//            older spelling, inproc) survives as a LOUD warning that takes
//            the daemon anyway — see open().
//
// The one exception is the WINDOWS PORT, where the carve-out is structural
// and not policy: src/ipc compiles there via failing stubs (PORTING.md) —
// shm_open/fork cannot succeed — so a daemon is unreachable and the
// in-process engine (an Engine, an audio backend, the MIDI reader, all in
// this process) stays compiled and stays the default, behind _WIN32. Every
// local-path member and dispatch arm below exists only under that macro.
//
// `local()` answers null ALWAYS on Linux — that used to be the load-bearing
// test of daemon mode and is now a constant: no caller may assume there is an
// in-process Engine to reach, and on Linux there never is one.
//
// WHAT DAEMON MODE CARRIES, AND WHAT IT DOES NOT
// ----------------------------------------------
// Carried: transport, tempo, quantum, metronome, the mixer scalars, sends and
// return levels, every polled meter and indicator, the computer-MIDI keyboard
// and note previews, and — step 3 — session clips, both audio and MIDI, through
// the sample pool.
//
// Also carried: device chains (§5 step 4, reconciled from the declaration the
// GUI publishes), the plugin catalog, and — protocol v7 — a rack's CONTENTS,
// which used to be the loudest gap on the list because a rack loaded empty and
// passed audio through.
//
// Also carried: the arrangement and its automation (one pool blob per lane, over
// the same pointer -> pool-ref cache the clip table uses) and — protocol v8 —
// the time-signature map, which is why daemon mode no longer plays every set in
// 4/4.
//
// And hardware MIDI: MidiInput takes a SINK now rather than an Engine&, so the
// ALSA reader thread's messages go over the wire in daemon mode (§1.3 option 3,
// with the lock the second producer needs).
//
// And recording (§7, protocol v9), which was the whole of the remaining list.
// The GUI's capture buffer never crosses: the command carries its CAPACITY, the
// daemon allocates the buffer its own audio thread appends into, and the
// finished take comes back as a FILE that this handle copies into the buffer App
// has been holding all along. App's recording code is unchanged and unaware —
// it pushes Cmd::RecordSlot and it gets Ev::RecordFinished for its own pointer.
//
// Nothing is left on the "refused with a reason" list except the three chain
// commands, which are refused permanently and by design (a client has no
// RtChains to name).
//
// Threading: GUI thread only, like everything in src/ui. The SPSC contract in
// core/ring.h holds against the shared-memory rings, with this thread as the
// sole producer of commands and of MIDI (plus the reader thread, under the
// mutex below). On the Windows arm the Engine it owns is the one the audio
// thread runs, and the ring pushes here are that contract's producer side.
#pragma once
#include "engine_state.h"
#ifdef _WIN32
#include "../audio/backend.h"   // AudioBackend: the in-process arm's, Windows-only
#endif
#include "../audio/engine.h"
#include "../audio/midi_in.h"
#include "../plugin/host.h"      // PluginDesc, and PluginInstance as an identity
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace lat {

// engine_state.h cannot see kMaxReturns (it includes core/ and nothing else, so
// that a view translation unit gets no engine header with it). This file has
// both, so this is where the duplicated number is held to account.
static_assert(kEsReturns == kMaxReturns,
              "EngineState's return arrays must be exactly kMaxReturns wide");

// The daemon half, defined in engine_handle.cpp. Held behind a pointer on
// purpose: src/ipc pulls in shm.h, pool.h, control.h and client.h — some four
// thousand lines and a <vector>/<string> dependency — and every view translation
// unit in src/ui reaches this header through app.h. Step 1's whole direction of
// travel was to get engine internals *out* of the view TUs, and swapping an
// Engine for an EngineClient there would have undone it.
struct RemoteEngine;

// What the daemon made of one device the GUI published. Pure data, no ipc: a
// caller that wants to draw "loading…", a reject reason or the real latency
// asks for this rather than for a DeviceMirror.
//
// Keyed by the GUI's own PluginInstance*, because that is the identity the
// model already has. §5 step 4 asks for a `DeviceModel { u64 uid; u32 deviceId;
// ... }` instead, which is the right end state and is a change to
// src/ui/session.h — see the note on publishChain() below for why this wave
// could not make it.
struct RemoteDevice {
    u32  id         = 0;        // the daemon's device id
    u32  generation = 0;        // its slot generation, the param-write guard
    bool live       = false;    // EvDeviceAdded has landed
    bool failed     = false;    // EvDeviceFailed did; `error` says why
    i32  latencyFrames = 0;     // the DAEMON's figure, which is the one that sounds
    u32  paramsMapped   = 0;    // GUI controls matched to a daemon control by id
    u32  paramsUnmapped = 0;    // GUI controls with no counterpart over there
    u32  paramsTruncated = 0;   // controls past ipc::kMaxDevParams (64)
    std::string uri, name, error;
};

class EngineHandle {
public:
    EngineHandle();
    ~EngineHandle();                      // out-of-line: RemoteEngine is opaque
    EngineHandle(const EngineHandle&)            = delete;
    EngineHandle& operator=(const EngineHandle&) = delete;

    // --- lifecycle ---------------------------------------------------------

    // Opens the engine this run is configured for. ON LINUX THE DAEMON IS THE
    // ENGINE (§8 step 2's flip in GUI-ON-DAEMON.md §16; §8(3)'s deletion in
    // §18): unset, `daemon` and `remote` all open the remote path, and
    // `NXTAKT_ENGINE=local` (or the doc's older `inproc`) no longer selects
    // anything — it is warned about LOUDLY and takes the daemon, exactly like
    // an unrecognised value, because a spelling that used to mean an engine
    // must not quietly come to mean no engine. `NXTAKT_SESSION` names the
    // session; it defaults to "default". On the Windows port the in-process
    // engine survives and stays the default: src/ipc is failing stubs there
    // (PORTING.md) and a daemon cannot exist, so `=local` still works and an
    // explicit `=daemon` still gets to try, fail, and say so.
    //
    // `driver` is "jack", "alsa" or null for auto, i.e. NXTAKT_AUDIO. In daemon
    // mode it is forwarded to a daemon we spawn and ignored for one we merely
    // attach to, which already has a driver.
    //
    // Returns false only if nothing could be opened at all. A missing audio
    // backend is not an error (local mode prepares the engine anyway and the set
    // is silent), missing MIDI hardware is not an error, and neither is a daemon
    // that will not start: §8's degraded mode says a GUI with no engine should
    // still open, load, edit and save rather than refuse to run. A daemon that
    // could not be reached NEVER falls back to a local engine — named or
    // defaulted, the answer is the engine-free mode, the Detached banner and a
    // Restart button that retries (§16 has the two-paragraph why).
    bool open(const char* driver);

    // The pre-rename spelling, kept because tests call it and a deprecation
    // cycle for an internal header is ceremony. New code says open().
    bool openLocal(const char* driver) { return open(driver); }

    // The daemon half of open()'s dispatch, exposed for callers that genuinely
    // mean it (and for tests).
    bool openDaemon(const char* session, const char* driver);
#ifdef _WIN32
    // The in-process half. Windows-only since §18's collect: on Linux the
    // symbol does not exist, so a caller that wants it back has to come
    // through this header and read why it left.
    bool openLocalEngine(const char* driver);
#endif

    // Joins the MIDI reader (it pushes over the wire from its own thread, so
    // it has to be gone before the client detaches), then closes the daemon
    // link. What happens to the daemon is a policy question, answered per §6:
    // a daemon we SPAWNED is stopped with us, a daemon we merely attached to
    // is left running. Parent-of-record, the same rule every
    // editor/language-server pair uses.
    //
    // On the Windows port's in-process arm the old ordering still holds and
    // still matters: the MIDI reader first (a producer on the engine's ring),
    // then the audio backend (whose stop() joins the audio thread), so that
    // nothing can be inside process() when App::shutdown() frees what the
    // engine was borrowing. The Engine itself outlives this call there: a
    // couple of App's frees are still reachable through local().
    void close();

#ifdef _WIN32
    bool localOpen() const { return engine_ != nullptr; }
#else
    // Constant since §18: there is no in-process engine to be open on Linux.
    bool localOpen() const { return false; }
#endif
    bool remoteOpen() const { return remote_ != nullptr; }

    // The in-process Engine — **null in daemon mode, and null ALWAYS on
    // Linux** (§18). Deliberately narrow even where it exists: everything a
    // view needs is in EngineState, and everything a command needs is below.
    // The offline Engines that survive §18 — tools/render, gen_demo, the
    // headless hook's private engine behind App::pumpJournal(Engine&) — are
    // constructed directly and never come through here. Every caller must
    // cope with null, which on Linux the compiler now proves.
#ifdef _WIN32
    Engine* local() { return engine_.get(); }
#else
    Engine* local() { return nullptr; }
#endif

    // The record journal, both modes: local pops the engine's own ring, remote
    // pops ipc::JournalRing (the daemon's pump forwards every entry, and the
    // structs are static_assert-mirrored). This is the mirror app.h once said
    // there was "no reason" to build -- the reason was that without it the
    // DEFAULT engine records arrangements into a ring nobody reads.
    bool popJournal(ArrJournal& j);

    // --- the per-frame snapshot (§2.1) -------------------------------------
    //
    // Called ONCE at the top of frame(). Everything the UI draws from comes out
    // of `out`; nothing else reads an engine atomic.
    //
    // Not const: in daemon mode this is also the frame's housekeeping hook (link
    // state, retry counters), which is exactly where the doc's §2.2 wants it.
    void poll(EngineState& out);

    // --- commands (GUI -> engine) ------------------------------------------
    //
    // Every one returns whether the ring accepted it. App does not call these
    // directly for the bursty paths — App::send()/pushClip() go through the
    // pending queue in app_engine.cpp, which is what stops a project load from
    // outrunning a 1024-deep ring (docs/ARRANGEMENT.md §15) and which doubles as
    // §2.2's dirty-cell set and scalar outbox for the daemon path: a refused
    // push is re-queued and retried on the next frame, in order.
    //
    // WHICH MEANS `false` HAS ONE MEANING AND ONLY ONE: "try again". A command
    // daemon mode can never carry must NOT answer false, or the caller re-queues
    // it forever and nothing behind it in the FIFO ever moves again. Those are
    // consumed, counted in remoteRefusals(), and logged once each with a reason.
    bool send(Cmd t, i32 a = 0, i32 b = 0, f64 x = 0.0);
    bool pushCommand(const Command& c);
    // The GUI's MIDI ring, which is NOT the hardware reader's: Ring tolerates
    // one producer and there are two (engine.h, pushMidi / pushMidiFromGui).
    bool pushMidi(const MidiMsg& m);
    bool popEvent(Event& e);

    // --- live scalars -------------------------------------------------------
    //
    // Not part of the frame snapshot, deliberately. sampleRate() is read by
    // paths that decode or resample, where "as of this frame" is the wrong
    // question and a zero from a snapshot taken before the engine was prepared
    // would silently resample everything wrong. journalDropped() is read at the
    // moment a take is committed, where §5.4 wants the count *now* and a value
    // one frame old could commit a take that should have been refused.
    f64 sampleRate() const;
    u32 journalDropped() const;

    // --- what the status bar prints about the backend ----------------------
    // In daemon mode these come off ControlHeader::driver and SharedState
    // instead; the call sites do not change.
    const char* driverName() const;
    f64  driverSampleRate() const;
    int  driverBufferSize() const;
    bool midiRunning() const;
    int  midiClientId() const;
    u64  midiReceived() const;

    // --- devices (§5 step 4) ------------------------------------------------
    //
    // THERE IS NO addDevice() HERE, AND THAT IS THE DESIGN OF THIS STEP.
    //
    // §5 step 4 describes rewriting App's device code around device ids:
    // `DeviceModel::inst` deleted, `addDevice()` becoming "send CmdAddDevice and
    // wait for the event", the knobs reading a DeviceMirror. That is the right
    // end state and it is unreachable from these files — it edits
    // src/ui/session.h and src/ui/app_devices.cpp, and it would delete the
    // in-process path that §8 says stays supported through step 6.
    //
    // So the seam is put one level lower, at the one call every chain edit
    // already funnels through: App::publishChain() builds an RtChain and hands
    // it to pushCommand(). An RtChain cannot cross a process boundary — but
    // everything the daemon needs in order to BUILD ITS OWN is readable off it
    // through PluginInstance's virtuals: desc().uri, paramInfo(i).id,
    // getParam(i), bypassed(). So the remote path reads the chain the GUI
    // declared and reconciles the daemon toward it with AddDevice /
    // RemoveDevice / MoveDevice / SetBypass, then mirrors params every frame.
    //
    // Two consequences worth stating rather than discovering:
    //
    //   * The GUI's PluginInstance is not silent-and-pointless in daemon mode,
    //     it is the MODEL. It holds the parameter values, the bypass flag and
    //     the rack contents; it just never renders audio, because there is no
    //     in-process engine to call process(). That is exactly §4's split — the
    //     GUI is the authority on what exists, the engine on what sounds.
    //   * Instantiation is asynchronous, and the GUI does not currently know
    //     that. A device appears in the model at once and starts sounding a
    //     frame or several later (or, on the very first one, after the daemon's
    //     plugin scan). devicesPending() is how a status line says so.
    //
    // Params are addressed by ParamInfo::id, per docs/PARAM-ADDRESS.md: the GUI
    // and the daemon load the same plugin build, so the ids match, but the
    // INDEX ordering is not something either side promises the other. A control
    // whose id has no counterpart is counted, never guessed at.

    // What the daemon made of a device the GUI published, or null if this is
    // not the daemon path or the instance is not in any published chain.
    const RemoteDevice* remoteDevice(const PluginInstance* gui) const;
    // Devices asked for whose answer has not arrived. Also in EngineState.
    u32 devicesPending() const;
    u64 devicesAdded() const;
    u64 devicesFailed() const;

    // --- rack contents (protocol v7, docs/RACKS.md) -------------------------
    //
    // A rack is a PluginInstance that owns PluginInstances and its descriptor
    // says nothing about its contents, so until v7 the daemon instantiated
    // `nxtakt:rack`, got an EMPTY one, and the device sounded as a passthrough
    // while the GUI drew the sub-chain. It now crosses as a PoolKindRackState
    // blob — `rackStateToString()`'s output, which is already one line of
    // printable ASCII and already version-tagged — applied on the far side with
    // `rackStateFromString` + `setState`.
    //
    // Nothing calls anything here: the handle polls the GUI's own RackControl
    // once a frame, exactly as it polls the parameters, because a device dropped
    // into a rack or a mapping dragged onto a macro has no command to hook. What
    // is polled is a FINGERPRINT of RackState; the string is only built when
    // that has moved.
    //
    // Both counters are diagnostics with an audible meaning: `racksRefused()`
    // non-zero is a rack that is drawn full and sounds as whatever the engine
    // already had, and the log says which one and why.
    u64 racksPublished() const;
    u64 racksRefused() const;

    // --- generic device state (protocol v10, GUI-ON-DAEMON.md §15) ----------
    //
    // The sampler's channel, and the channel of every device after it that keeps
    // something no parameter can say. `PluginInstance::stateString()` crosses as
    // a PoolKindDeviceState blob and the far side applies it with
    // `setStateString()` after the device's pending parameters, which is
    // host.h's own ordering rule.
    //
    // For a device that plays a FILE it carries more than the string: `nxtaktd`
    // links no decoder by design, so the GUI's decoded SampleBuffer rides along
    // in the pool and the daemon's instance adopts a copy of it. Until this
    // existed a sampler in daemon mode was STRUCTURALLY SILENT -- the set drew
    // an instrument with a filename on it and the engine had never heard of
    // either.
    //
    // Polled once a frame like the racks and the parameters, and for the same
    // reason: dropping a file on a sampler calls straight into the GUI's own
    // SamplerControl and there is no command to hook.
    //
    // `deviceStatesRefused()` non-zero is an instrument that is drawn loaded and
    // plays nothing; the log line says which and why.
    u64 deviceStatesPublished() const;
    u64 deviceStatesRefused() const;

    // --- the arrangement (docs/ARRANGEMENT.md §9) ---------------------------
    //
    // The daemon has been able to take an arrangement since wave 8g; what was
    // missing was the encoder on this side. A lane crosses as ONE pool blob
    // ([WireArrHeader][WireArrItem[]][WireClip[]]) over the same pointer ->
    // pool-ref cache the clip table uses, so a clip that is both in a scene and
    // on the timeline names one block and is written once. The transport's loop
    // brace is the lane addressed as track -1.
    //
    // `arrangementsRefused()` non-zero is a timeline that is drawn and does not
    // play; the log line says which lane and why.
    u64 arrangementsPublished() const;
    u64 arrangementsRefused() const;

    // --- the signature map (protocol v8) ------------------------------------
    //
    // A flat RtSig[] crossing as a PoolKindSignatures blob. It is published
    // through pushCommand(Cmd::SetSignatures) like everything else, which is
    // why session.h's publishSignatures() takes an EngineHandle& now: it used
    // to take an Engine& and could therefore not be called at all in daemon
    // mode, and the set played in 4/4.
    //
    // `signaturesRefused()` non-zero means exactly that state has come back —
    // the map is unwalkable, the engine kept 4/4 — and it must be sayable,
    // because the transport readout reads the ENGINE's counters (§11.6) and
    // will show 4/4 without explaining why.
    u64 signaturesPublished() const;
    u64 signaturesRefused() const;

    // --- recording (§7, protocol v9) ----------------------------------------
    //
    // Four numbers, and between them they say everything a status line needs to
    // about a take that did not become a clip. All 0 in local mode, where a take
    // never leaves the process and none of these states exists.
    //
    //   returned  takes that came home with frames in them
    //   empty     takes stopped before their quantized start fired. An ORDINARY
    //             outcome, counted separately precisely so it is not mistaken
    //             for one of the two below.
    //   failed    the daemon refused the take, or wrote a file this process
    //             could not read. Non-zero means a performance that happened and
    //             was not kept, and the log line names the file — which still
    //             exists, because a take is never destroyed on the word of the
    //             process that failed to read it.
    //   lost      takes in flight when the engine died or was restarted. Also a
    //             performance that is gone, and the one number that distinguishes
    //             "your engine crashed" from "your take was empty".
    u64 takesReturned() const;
    u64 takesEmpty() const;
    u64 takesFailed() const;
    u64 takesLost() const;

    // Pool blocks currently allocated. A diagnostic with one specific job: a
    // publication path that writes a fresh block every frame instead of reusing
    // the one it already wrote looks EXACTLY like one that works — the audio is
    // right, the events arrive, and the pool fills up over a long session. This
    // is the only number that can tell them apart from outside. 0 in local mode.
    u64 poolBlocksLive() const;

    // --- the plugin catalog (§5 step 5) -------------------------------------
    //
    // What the DAEMON can instantiate, which is not in general what this
    // process can find: a different LV2_PATH, a different user, a bundle that
    // crashes lilv here and not there. A browser drawn from the local
    // PluginRegistry can therefore offer a row whose double-click can only ever
    // fail, which is the whole reason §3 asked for a catalog table.
    //
    // Empty in local mode — there the local registry IS the daemon's — and
    // empty in daemon mode until the scan completes. Ask with requestScan();
    // scanRunning() drives the spinner.
    const std::vector<PluginDesc>& catalog() const;
    // Plugins the daemon found that did not fit the table. Non-zero must be
    // drawn: "…and N more this build cannot list" beats a silently short list.
    u32  catalogTruncated() const;
    bool catalogReady() const;
    bool scanRunning() const;
    bool requestScan();

    // --- lifecycle (§6) -----------------------------------------------------
    EngineLink link() const;
    // The engine's process id, or -1 when there is no daemon. Status-bar
    // material, and the only handle on the daemon a test has.
    i32 enginePid() const;

    // §6's recovery, and the ONLY thing that may run it is a user click or a
    // provably dead engine — never a stale heartbeat (§4.4). Reaps the orphan
    // region, respawns, re-attaches (which re-announces the pool), republishes
    // every clip cell from the client's shadow — a memcpy, no decode, no offset
    // changes — replays the mixer scalars and the tempo, and re-issues
    // AddDevice for every device on every chain, because device ids do not
    // survive an engine (§11.4).
    //
    // The transport deliberately comes back STOPPED, per §4.4's honest default.
    //
    // False if nothing could be started; the handle is then Detached and the
    // set is still editable and saveable.
    //
    // §16: it also works from that Detached state, when open() wanted a daemon
    // and could not reach one — the same click retries the first spawn with
    // the configuration open() recorded. That is the recovery path for a
    // missing or broken nxtaktd fixed after startup, without relaunching.
    bool restartEngine();
    u64  resyncs() const;      // completed restartEngine()s

    // --- daemon-mode diagnostics -------------------------------------------
    //
    // How many commands were consumed because the remote path cannot carry them
    // yet. Must be readable, because "refused with a reason" is only true if
    // somebody can see the reason: every distinct command type is also logged
    // once, and close() prints the tally.
    u64 remoteRefusals() const;
    // Snapshots that exhausted readCoherent()'s retry budget, i.e. frames drawn
    // from a copy that may have straddled a publish. Zero unless the daemon is
    // stopped mid-publish.
    u64 snapshotTears() const;

    // WHAT THE DAEMON CAN ACTUALLY SEE for a session cell's notes: the pool
    // block the last published WireClip for (track, slot) points at, read back
    // out of the shared region and copied into `out` (up to `max` notes).
    // Returns the note count published there, 0 if the cell has no notes, and
    // -1 if this is not the daemon path.
    //
    // It exists because the GUI's own RtNote[] is NOT what sounds — a COPY of
    // it in the pool is, made when the clip was published and reused whenever
    // the handle believes the array has not changed. Those two can disagree,
    // and when they do the symptom is an edit that is on screen and not in the
    // audio. handle_test asserts against this rather than against the GUI's own
    // array, because only this side of the copy can fail.
    i64 publishedNotes(int track, int slot, RtNote* out, i64 max) const;

private:
#ifdef _WIN32
    // The in-process engine, Windows-only since §18 (the port's src/ipc is
    // failing stubs, so this is the only engine that can exist there). Heap,
    // not by value: Engine is ~2.3 MB of scratch buffers, and a pointer is
    // what lets local() answer "there is no in-process engine".
    std::unique_ptr<Engine> engine_;
    std::unique_ptr<AudioBackend> audio_;
#endif
    // The MIDI reader survives §18 on both platforms — it is the daemon
    // path's hardware input. Its thread pushes over the wire through the sink
    // openDaemon() hands it (on the Windows arm, into the in-process engine's
    // ring via openLocalEngine()'s).
    MidiInput midi_;
    // Guards the DAEMON path's single MIDI ring against its two producers: this
    // thread (the computer keyboard, note previews) and the ALSA reader thread.
    // lat::Ring is documented single-producer and neither of these is realtime,
    // so a mutex is the whole fix — docs/GUI-ON-DAEMON.md §1.3, option 3. It
    // also covers restartEngine(), which unmaps the ring the reader is pushing
    // into. Unused on the Windows in-process arm, where the engine keeps a
    // ring per producer.
    mutable std::mutex midiMx_;

    // Null unless openDaemon() succeeded. Null with nothing else open is §8's
    // degraded mode and is a supported state (on the Windows arm, "nothing
    // else" includes engine_; the two are never both set).
    std::unique_ptr<RemoteEngine> remote_;

    // What open() recorded on the daemon path, so restartEngine() can retry a
    // spawn that failed at startup (§16). wantDaemon_ distinguishes "asked for
    // a daemon and got nothing" — where Restart should try again — from a
    // handle that never asked (unopened, or the Windows arm's local mode),
    // where it must keep answering false.
    bool wantDaemon_ = false;
    std::string session_, driver_;
};

} // namespace lat
