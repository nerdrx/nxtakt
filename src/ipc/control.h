// NxTakt IPC — the control region: what actually travels between nxtaktd and
// the GUI, and where it lives inside a ShmRegion.
//
// shm.h is the transport (a validated mapping and an SPSC ring); this header is
// the protocol: the wire message shapes, the region map, and the layout hash
// both sides fold into ShmRegion::attach() so a mismatched build fails at
// startup instead of reading a ring through the wrong offset.
//
// Region map (docs/PROCESS-SPLIT.md §3.3), all offsets past ShmHeader:
//
//   kHeader   ControlHeader                 protocol version, heartbeat counter,
//                                           shutdown flag, boundary counters,
//                                           the sample-pool handshake
//   kState    ipc::SharedState              the polled atomics block
//   kCmds     ShmSpscRing<WireCommand,4096> GUI -> engine
//   kEvts     ShmSpscRing<WireEvent,4096>   engine -> GUI
//   kMidi     ShmSpscRing<WireMidi,1024>    GUI/MIDI reader -> engine
//   kClips    WireClip[32][32]              the clip table, GUI -> engine
//   kDevices  WireDeviceInfo[320]           device metadata, daemon -> GUI
//   kParams   WireDeviceParams[320]         param values, GUI -> daemon
//   kJournal  ShmSpscRing<WireJournal,4096> the record journal, daemon -> GUI
//
// The doc's map has three payload sections; this adds two. ControlHeader exists
// because ShmHeader is the *transport's* header (magic, layout, creator
// liveness) and must not grow protocol fields — kShmVersion says "these two
// builds agree about how a region is shaped", kProtocolVersion says "these two
// builds agree about what the messages mean", and they change for different
// reasons. The MIDI ring is here because Engine::pushMidi() already exists and
// a phase-1 daemon that could not carry MIDI would be a regression against the
// in-process build.
//
// Header-only, like shm.h, and it deliberately pulls in nothing but core/ and
// audio/engine.h — the enums, not the Engine — because Cmd and Ev *are* the
// protocol's vocabulary and a second, hand-copied definition of them in the
// IPC layer would drift the first time somebody adds a command.
#pragma once
#include "../audio/engine.h"
#include "pool.h"
#include "shm.h"
// The take's file format and its directory rule (v9). Part of the protocol in
// the same sense the pool is: EvTakeReady names a file, and both sides have to
// agree what is in it. Header-only and libc-only, like the rest.
#include "take.h"

namespace lat::ipc {

// ---------------------------------------------------------------------------
// Versioning
// ---------------------------------------------------------------------------

// Bump on any change to the meaning of a wire message, a reject reason, or a
// ControlHeader field. Layout changes bump kShmVersion (shm.h) instead — and
// the layout hash below catches the times we forget either.
//
//   v2 — the sample pool (phase 2): the clip table, the pool handshake in
//        ControlHeader, EvClipAccepted/EvBlockRetired, and SetClip/ClearClip
//        moving from "refused" to "accepted".
//   v3 — devices (phase 3): AddDevice/RemoveDevice/MoveDevice/SetBypass, string
//        blobs through the pool, the device metadata table (daemon -> client)
//        and the param table (client -> daemon). Cmd::SendLevel/ReturnVol join
//        the scalars; Cmd::SetChain and its return/master siblings become
//        daemon-internal and are refused on the wire *permanently* rather than
//        pending a phase.
//   v4 — RESERVED for AUTOMATION.md §8.3 (clip envelopes across the boundary,
//        WireClip growing by autoRef/autoLaneCount/autoPointCount). That design
//        names v4 explicitly and has not shipped in this tree. Burning the
//        number is cheaper than having "protocol v4" mean one thing in a
//        document and another in a header.
//   v5 — the arrangement (ARRANGEMENT.md §9.1). Cmd::SetArrangement /
//        SetTrackAutos / Locate / BackToArrangement classify and cross;
//        PoolKindArrangement and PoolKindTrackAutos ride the pool;
//        RejectBadArrangement; EvArrangementAck; and the control region grows a
//        NINTH section, the record journal's own ring (§9.6). This is a
//        deliberate incompatibility on a shipped protocol: a v3 binary and a v5
//        binary refuse each other at attach() with a specific message, which is
//        the mechanism working as designed.
//   v6 — the plugin catalog (GUI-ON-DAEMON.md §3, option B). A TENTH section:
//        WirePluginDesc[kMaxCatalog], written once by the daemon when its scan
//        completes and read once by a client on EvScanComplete. Until this
//        existed the catalog was reachable one URI at a time, so a GUI browser
//        could only list what its OWN process found — which is the thing phase
//        3 removed the GUI's plugin layer to stop being true. ControlHeader
//        gains catalogCount/catalogTruncated out of its reserved words.
//   v7 — rack contents (docs/RACKS.md; GUI-ON-DAEMON.md §12.3 named this the
//        cheapest remaining device feature and it is). CmdSetRackState carries a
//        PoolKindRackState blob; RejectNotARack and RejectBadRackState say why a
//        refusal happened; EvDeviceChanged grows DeviceChangedRackState. No
//        section moved and no struct grew: the whole feature is one command, one
//        pool kind and two reject reasons. Until it existed a rack in daemon mode
//        loaded EMPTY and passed audio through, which is the loudest thing on
//        §12.7's owed list.
//   v8 — the signature map. Cmd::SetSignatures finally classifies and crosses,
//        as a PoolKindSignatures blob answered by EvSignaturesAck; commandIsKnown's
//        bound moves to the last enumerator because the daemon now genuinely
//        honours the command rather than answering RejectUnknownCommand. Until
//        this, daemon mode PLAYED EVERY SET IN 4/4 while the ruler drew 7/8 —
//        refused-and-visible, which was the right way to be wrong, and is no
//        longer necessary.
//   v9 — RECORDING (GUI-ON-DAEMON.md §7, option 2). Cmd::RecordSlot and
//        Cmd::RecordMidiSlot stop carrying a pointer and start carrying a
//        CAPACITY: the client's `p` never crosses and never needs to, because
//        the buffer the audio thread appends into is the DAEMON's. A finished
//        take is written to a file under ControlHeader::takeDir and announced by
//        EvTakeReady; the client copies it out and answers CmdTakeRelease, which
//        is free-after-confirm run in the other direction (src/ipc/take.h).
//        ControlHeader grows `takeDir` — the first field in this region that is a
//        *path* — so every section below kHeader moves and a v8 binary and a v9
//        binary refuse each other at attach(), which is the mechanism working.
//
//        This is the last item on §7's list, and with it the daemon path carries
//        everything the in-process one does.
//   v10 — GENERIC DEVICE STATE, and with it the SAMPLER (GUI-ON-DAEMON.md §15).
//        CmdSetDeviceState carries a PoolKindDeviceState blob — the device's own
//        stateString() output, plus, for a device that plays a file, a pool
//        reference to the audio the GUI decoded. It is a SIBLING of
//        CmdSetRackState and not a replacement for it, because a rack's contents
//        are not reachable through stateString() in this tree (RackControl is
//        the accessor; src/plugin/internal_devices.cpp overrides neither half of
//        the generic pair) and that file was not this wave's to change. §15.1
//        argues the case and says what it would take to fold the two.
//
//        ControlHeader gains `deviceStatesApplied` out of its reserved words, so
//        no section moves; the version and the layout hash are what make a v9
//        and a v10 binary refuse each other, which is the mechanism working.
//
//        Until this existed `nxtakt:sampler` was STRUCTURALLY SILENT in daemon
//        mode: generic device state did not cross at all, so the path the
//        sampler is defined by never reached the engine, and nxtaktd links no
//        decoder so it could not have opened the file even if it had. A set with
//        an instrument in it played the instrument in-process and nothing at all
//        through the daemon.
//   v11 — THE WHOLE CLIP (GUI-ON-DAEMON.md §17). WireClip grows the three
//        references RtClip always had and the wire never carried: `autoRef`
//        (clip envelopes, PoolKindAutomation — the fields and bounds
//        AUTOMATION.md §8.3 reserved against v4, landing seven versions
//        later), `markersRef` (the warp-marker map, PoolKindWarp) and
//        `transientsRef` (the sample's onset grid, PoolKindTransients), each
//        with its count beside noteCount. sizeof(WireClip) goes 120 -> 176, so
//        the clip table and every section after it move and the layout hash
//        moves with them — a v10 and a v11 binary refuse each other at
//        attach(), which is the mechanism working. RejectBadAutomation stops
//        being reserved and is returned; pool version 7 -> 8 rides along
//        (PoolKindAutomation implemented, PoolKindWarp/PoolKindTransients
//        new).
//
//        Until this existed the GUI counted an honest refusal per affected
//        clip — and because the loader computes a transient grid for every
//        audio sample, that was EVERY set with audio in it, under what v10's
//        flip made the DEFAULT engine: an amber "N refused" for a set that was
//        otherwise carried perfectly, and Beats-warped clips playing without
//        the envelopes, maps and grids that shape them.
inline constexpr u32 kProtocolVersion = 11;

// Daemon-generated wire events start here, well clear of lat::Ev. The event
// ring carries a superset of Ev: the boundary itself has things to report
// (a refused command, "I am going away") that no engine ever needs to say.
inline constexpr u32 kDaemonEventBase = 0x1000;

// Daemon-*consumed* commands start here, well clear of lat::Cmd, and for the
// mirror-image reason: the boundary has things done to it that no Engine ever
// hears about. `AddDevice` is the archetype — it names a plugin by URI,
// instantiates it in the daemon's address space, and only then turns into a
// `Cmd::SetChain` the engine understands. lat::Cmd stays the engine's
// vocabulary; this is the boundary's.
inline constexpr u32 kDaemonCommandBase = 0x1000;

enum : u32 {
    // flags = DevTarget*, a = track/return index (ignored for master),
    // b = chain position (-1 appends), ref = pool offset of a PoolKindString
    // holding the plugin URI. Answered by exactly one EvDeviceAdded or
    // EvDeviceFailed, and the URI blob is retired (EvBlockRetired) either way.
    CmdAddDevice    = kDaemonCommandBase + 0,

    // ref = device id. Answered by EvDeviceRemoved (or EvDeviceFailed).
    CmdRemoveDevice = kDaemonCommandBase + 1,

    // ref = device id, b = new position within its own chain. Answered by
    // EvDeviceChanged.
    CmdMoveDevice   = kDaemonCommandBase + 2,

    // ref = device id, a = 0/1. A *command* and not a param-table write
    // because §3.7 says so and it is right: bypass has to land in a defined
    // order relative to the chain edits around it.
    CmdSetBypass    = kDaemonCommandBase + 3,

    // Force the plugin scan to start now rather than on the first AddDevice.
    // Purely an optimisation for a GUI that knows it is about to need the
    // catalog; the lazy path is identical.
    CmdScanPlugins  = kDaemonCommandBase + 4,

    // Install a rack's complete contents. a = device id, flags = the device's
    // WireDeviceInfo generation (the same stale-write guard the param table
    // uses), ref = a PoolKindRackState blob holding rackStateToString()'s
    // output. Answered by exactly one EvDeviceChanged (flags &
    // DeviceChangedRackState) or EvDeviceFailed, and the blob is retired
    // (EvBlockRetired) either way — the URI blob's discipline, verbatim.
    //
    // WHY A COMMAND OF ITS OWN, AND NOT A FIELD ON CmdAddDevice. Both would
    // work for a rack that is loaded once and never touched. Only this one works
    // for a rack that is EDITED: a field on the add would make every change to a
    // rack's contents a remove-and-re-add of the whole rack, which destroys and
    // reloads every plugin inside it and puts an audible hole in the track for
    // as long as that takes. A rack is a container people open and rearrange
    // while the set is running, so the wire needs a verb for "its contents are
    // now this" that is not "it is now a different device".
    //
    // ORDERING (docs/RACKS.md, "Persistence"): the daemon applies the device's
    // pending param row BEFORE setState and re-seeds its parameter cache after,
    // because setState writes the macros WITHOUT driving their targets and any
    // macro write that lands after it would re-derive every mapped parameter
    // from the macro position — silently rounding a target the user parked off
    // its macro's curve. See Daemon::doSetRackState.
    CmdSetRackState = kDaemonCommandBase + 5,

    // Install one device's GENERIC state: whatever `PluginInstance::stateString()`
    // said, plus — for a device that plays a file — the audio the GUI decoded.
    // a = device id, flags = the device's WireDeviceInfo generation (the same
    // stale-write guard the param table and CmdSetRackState carry), ref = a
    // PoolKindDeviceState blob. Answered by exactly one EvDeviceChanged (flags &
    // DeviceChangedState) or EvDeviceFailed; the blob AND the sample block it
    // names are retired (EvBlockRetired) either way.
    //
    // WHY A SIBLING OF CmdSetRackState AND NOT A WIDENING OF IT. The obvious
    // move is to make one "device state" command and delete the rack's, since
    // host.h says a rack's state IS a state string. In THIS tree it is not
    // reachable as one: `Rack` overrides neither stateString() nor
    // setStateString(), and its contents are read through RackControl::state()
    // and written through RackControl::setState() — see
    // src/plugin/internal_devices.cpp, which is where the fold would have to
    // happen and which this wave did not own. Sending a rack down this channel
    // today would call the BASE-CLASS setStateString(), which accepts anything
    // and does nothing: accepted-and-ignored, which is the trade this boundary
    // refuses. GUI-ON-DAEMON.md §15.1 states what folding them would take.
    //
    // ORDERING, host.h's own rule and RACKS.md's trap in generic clothing: the
    // daemon applies the device's pending param row BEFORE setStateString and
    // re-seeds its parameter cache after. A state string is allowed to move
    // parameters; a param write that landed after it would overwrite what the
    // state just restored. See Daemon::doSetDeviceState.
    CmdSetDeviceState = kDaemonCommandBase + 6,

    // "I have copied that take out of its file; it is yours to drop."
    // ref = the take uid EvTakeReady carried, flags = TakeReleaseKeepFile.
    //
    // FREE-AFTER-CONFIRM, RUN THE OTHER WAY. The pool's rule is that the client
    // may not reuse a block until the daemon has said the engine cannot reach it
    // (EvBlockRetired); this is the mirror image, and it exists for the mirror
    // reason. The daemon holds the take's buffer AND its file until this
    // arrives, so a client that dies between the announcement and the copy loses
    // nothing that a restart cannot pick up off the disk, and a client that
    // never answers costs a bounded amount (take.h, kMaxPendingTakes) rather
    // than the session's memory.
    //
    // Unanswered is not a protocol error: the daemon reclaims on the client's
    // death (a new attach epoch sweeps the directory) rather than on a timer,
    // because a timer would be a deadline on a GUI thread that may legitimately
    // be busy decoding the take it was just given.
    //
    // RENUMBERED from +6 to +7 in v10, when CmdSetDeviceState took the slot
    // below it. It is a wire change and it rides the version bump that a wire
    // change is for; the alternative was a HOLE in commandIsDevice()'s range
    // (a device command at +7 with a take release at +6 inside it), and a
    // range predicate with a named exception in it is a predicate the next
    // person appends to wrongly.
    CmdTakeRelease  = kDaemonCommandBase + 7,
};

// CmdTakeRelease::flags.
enum : u32 {
    // Drop the buffer but LEAVE THE FILE. The client says this when it could not
    // read the take: the material is still on disk, the log line says where, and
    // the next attach's sweep is what finally removes it. A take is never
    // destroyed on the word of the process that failed to read it.
    TakeReleaseKeepFile = 1u << 0,
};

// Cmd::RecordSlot / Cmd::RecordMidiSlot::flags on the wire.
enum : u32 {
    TakeCmdMidi = 1u << 0,   // the take is notes, and `x` counts notes
};

// EvTakeReady::flags.
enum : u32 {
    TakeIsMidi     = 1u << 0,   // read it with readMidiTake, not readAudioTake
    TakeWasEmpty   = 1u << 1,   // nothing was captured; there is NO file
    TakeHitCeiling = 1u << 2,   // capture stopped because the buffer filled
};

// What a chain belongs to. The engine has three chain commands with three
// different shapes (Cmd::SetChain / SetReturnChain / SetMasterChain); one enum
// on the wire keeps the client from having to know which is which.
enum : u32 {
    DevTargetTrack  = 0,   // a = track index
    DevTargetReturn = 1,   // a = return index
    DevTargetMaster = 2,   // a ignored
};

// ControlHeader::scanState. The plugin scan is lazy and asynchronous, which is
// §3.6 being honest: it always took a second, the in-process GUI just blocked
// on it.
enum : u32 {
    ScanIdle    = 0,   // not started; the first device command starts it
    ScanRunning = 1,
    ScanDone    = 2,
};

inline const char* devTargetName(u32 t) {
    switch (t) {
        case DevTargetTrack:  return "track";
        case DevTargetReturn: return "return";
        case DevTargetMaster: return "master";
        default:              return "?";
    }
}

enum : u32 {
    EvCommandRejected = kDaemonEventBase + 0,  // a = Cmd, b = RejectReason
    EvEngineStopping  = kDaemonEventBase + 1,  // clean shutdown has begun
    EvEventDropped    = kDaemonEventBase + 2,  // a = Ev that could not cross

    // The daemon has finished with clip cell (a, b): ref = the generation it
    // acted on, x = a RejectReason, flags per EvClipAckFlag below. Exactly one
    // of these answers every SetClip/ClearClip, accepted or refused. It is a
    // *flow-control* acknowledgement, not permission to free anything: it is
    // what lets the client know a cell may be rewritten, and what tells it
    // whether the engine took the write (see WireClip::generation).
    EvClipAck         = kDaemonEventBase + 3,

    // ref = a pool offset the engine can no longer reach; a, b = the clip cell
    // it was displaced from; flags = PoolKind*. **The only thing that
    // authorises a free.** See the free-after-confirm rule in pool.h and the
    // proof it rests on in src/daemon/nxtaktd.cpp.
    EvBlockRetired    = kDaemonEventBase + 4,

    // The daemon has mapped the pool named in ControlHeader; ref = the epoch,
    // x = the mapped byte count. Purely informational — the client may publish
    // clips before it arrives, they are simply refused until the pool is in.
    EvPoolAttached    = kDaemonEventBase + 5,

    // --- devices (phase 3) -------------------------------------------------
    //
    // A device command is answered exactly once, the same discipline EvClipAck
    // established: a silent refusal would leave a GUI showing an
    // "instantiating…" strip forever.

    // The plugin is loaded, prepared and in a published chain.
    // ref = device id, flags = DevTarget*, a = target index, b = chain
    // position, x = param count. **The metadata is not in this event** — it is
    // in the device table at `ControlMap::device(id)`, which the client reads
    // once and mirrors. See §11.3 for why a table and not a blob.
    EvDeviceAdded     = kDaemonEventBase + 6,

    // ref = the URI blob offset the client sent (0 if the command never named
    // one), b = a Reject* reason, a = DevTarget*. The daemon is still alive;
    // that is the whole point of answering rather than dying.
    EvDeviceFailed    = kDaemonEventBase + 7,

    // ref = device id. The instance is destroyed, the chain is republished and
    // the id is free for reuse — with a bumped WireDeviceInfo::generation, so
    // a param write aimed at the old occupant cannot land on the new one.
    EvDeviceRemoved   = kDaemonEventBase + 8,

    // ref = device id. Something in the device's table row changed that the
    // client did not write: a move (b = the new position), a bypass the daemon
    // applied (flags & DeviceChangedBypass), a re-published chain.
    EvDeviceChanged   = kDaemonEventBase + 9,

    // The plugin scan finished. a = plugin count, x = seconds it took.
    EvScanComplete    = kDaemonEventBase + 10,

    // --- the arrangement (wave 8g) -----------------------------------------
    //
    // a = track (-1 = SetArrangement's transport cell), b unused, ref = the
    // generation the client stamped on the command, x = a RejectReason, flags
    // per EvArrAckFlag below. Exactly one of these answers every
    // SetArrangement and every SetTrackAutos, accepted or refused.
    //
    // §9 does not name this event; it is here for the reason §11.2 gives for
    // answering every device command and §10.4 gives for answering every clip
    // cell. An arrangement blob is a pool block the client allocated, so a
    // silent refusal would leave the client holding a block it can never free
    // and a track it can never re-publish. "Answered exactly once" is the
    // discipline EvClipAck established and this is the same shape: ref is the
    // generation, x is the reason, and the flags say which command it answers.
    EvArrangementAck  = kDaemonEventBase + 11,

    // Answers every Cmd::SetSignatures exactly once. ref = the generation the
    // client stamped, x = a RejectReason, flags per EvSigAckFlag below.
    //
    // Its own event and not a flag on EvArrangementAck, even though the two are
    // the same shape: EvArrangementAck is addressed by TRACK and there is
    // exactly one signature map for the whole set, so folding it in would mean
    // an ack whose `a` means nothing and a client that has to remember which
    // flag makes it meaningless.
    EvSignaturesAck   = kDaemonEventBase + 12,

    // --- recording (v9) -----------------------------------------------------
    //
    // A take is finished and its file is complete. ref = the take uid, a =
    // track, b = slot, x = frames (or NOTES, for a MIDI take), flags per
    // TakeIs*/TakeWas* above. The path is `takePath(header.takeDir, ref, midi)`
    // and the client composes it from the directory the DAEMON published — never
    // from its own environment (take.h says why).
    //
    // Answered by exactly one CmdTakeRelease, and RETRIED until it goes out: the
    // take sits in the daemon's table with its buffer and its file intact for as
    // long as the client's event ring is full, which is the property the
    // in-process path gets from engine.cpp's parking buffer and this path has to
    // get from somewhere. A full ring must not be able to destroy the only copy
    // of a performance.
    //
    // x == 0 with TakeWasEmpty is a legal, ordinary answer: it is the take that
    // was stopped before its quantized start ever fired. There is no file, and
    // the client turns it into the same zero-frame Ev::RecordFinished the engine
    // would have sent in-process.
    EvTakeReady       = kDaemonEventBase + 13,

    // The take did not happen and will not: a = track, b = slot, x = a Reject*
    // reason, ref = the uid if one was ever assigned. The client still owes its
    // GUI a hand-back — App's capture buffer is freed on the finish event and on
    // nothing else — so this becomes a zero-frame Ev::RecordFinished with a log
    // line, exactly as a refused start does.
    EvTakeFailed      = kDaemonEventBase + 14,
};

// EvSignaturesAck::flags.
enum : u32 {
    SigAckRefused  = 1u << 0,   // the engine did not get it; x says why
    SigAckWasClear = 1u << 1,   // the command carried ref == 0
};

// EvArrangementAck::flags.
enum : u32 {
    ArrAckRefused  = 1u << 0,   // the engine did not get it; x says why
    ArrAckWasClear = 1u << 1,   // the command carried ref == 0
    ArrAckAutos    = 1u << 2,   // it answers SetTrackAutos, not SetArrangement
};

// EvDeviceChanged::flags.
enum : u32 {
    DeviceChangedMoved  = 1u << 0,
    DeviceChangedBypass = 1u << 1,
    // A CmdSetRackState landed: the device's contents, and therefore its
    // latencyFrames, are not what the client last read. b carries the new
    // latency so a status bar does not have to re-read the row for it.
    DeviceChangedRackState = 1u << 2,
    // A CmdSetDeviceState landed: the device's state string, and possibly the
    // sample it names, are now what the client asked for. `a` carries the
    // device's latencyFrames as re-read from the instance, for the reason the
    // rack's does — a state string may change what a device costs.
    DeviceChangedState = 1u << 3,
};

// EvClipAck::flags.
enum : u32 {
    ClipAckWasClear = 1u << 0,   // the command was Cmd::ClearClip
    ClipAckRefused  = 1u << 1,   // the engine did not get it; x says why
};

// Why the daemon refused a command. The pointer-payload family is what remains
// of phase 1's scalars-only rule, and as of v9 it is the chain family alone:
// the two Record commands stopped carrying an address when the daemon started
// supplying the buffer. The pool family below it is every reason that is a bad
// offset caught before it could become a pointer.
enum : u32 {
    RejectNone           = 0,
    RejectPointerPayload = 1,  // SetChain / SetReturnChain / SetMasterChain
    RejectUnknownCommand = 2,
    RejectBadIndex       = 3,  // track/slot out of range
    RejectNotFinite      = 4,  // NaN/inf in x
    RejectNoPool         = 5,  // a clip references the pool and none is mapped
    RejectBadPoolRef     = 6,  // offset failed poolValidate() — see the log line
    RejectBadClip        = 7,  // the clip's own scalars are inconsistent

    // --- devices (phase 3) --------------------------------------------------
    RejectUnknownUri     = 8,  // the scan does not know that URI
    RejectInstantiate    = 9,  // the plugin refused to load or to activate
    RejectDeviceTableFull= 10, // kMaxDevices devices already exist
    RejectChainFull      = 11, // kMaxChainFx devices already on that chain
    RejectBadDevice      = 12, // no such device id, or its generation is stale
    RejectBadString      = 13, // the URI blob is not a terminated string
    RejectScanBusy       = 14, // the scan is running and the queue is full

    // AUTOMATION.md §8.4 named this number while the design was still
    // reserved; v11 makes it live. The clip's envelope payload is malformed —
    // counts past kMaxClipAutoLanes/kMaxClipAutoPoints, a lane window outside
    // the point array, a target/xform/devSlot out of range, or a non-finite
    // clamp — and per §8.4 a malformed set REFUSES THE WHOLE SetClip; the
    // daemon never silently strips the automation and plays the rest.
    RejectBadAutomation  = 15,

    // --- the arrangement (wave 8g) ------------------------------------------
    //
    // The whole-blob refusal discipline of RejectBadClip, applied to a payload
    // that is a hundred structures rather than one: a client that sent
    // something impossible is TOLD, not partially obeyed. There is no
    // "the daemon took the first forty items" outcome.
    RejectBadArrangement = 16,

    // --- rack contents (v7) -------------------------------------------------
    RejectNotARack       = 17,  // CmdSetRackState named a device with no rack()
    // The blob did not parse, or the rack would not take it. One reason and not
    // two on purpose: rackStateFromString and setState both answer a bare bool,
    // and inventing a distinction the plugin layer does not make would be this
    // header claiming to know something it does not.
    RejectBadRackState   = 18,

    // --- the signature map (v8) ---------------------------------------------
    //
    // The blob's own shape, or a map sigMapValid refuses. One reason for both,
    // because the daemon runs the ENGINE's own validator: a map this reason
    // names is a map the engine would have handed straight back, and inventing
    // a distinction the validator does not make would be this header claiming
    // to know something it does not.
    RejectBadSignatures  = 19,

    // --- recording (v9) -----------------------------------------------------
    //
    // A take is already in flight on that track, or the daemon is already
    // holding kMaxPendingTakes finished takes nobody has claimed. Both are the
    // same answer to the client — "not now, and here is why" — and both are
    // states App cannot reach on its own (startRecording refuses a track whose
    // recState is not idle), so either one arriving means the two sides'
    // pictures of the track have diverged and the log line says so.
    RejectTakeBusy       = 20,
    // The capacity asked for is past take.h's kMaxTakeBytes, or is not positive.
    // Refused rather than clamped: see kMaxTakeBytes.
    RejectTakeTooLarge   = 21,
    // The take's own buffer could not be allocated, or its file could not be
    // written. The one reason for both because the client's recourse is the
    // same and because the log line — which has the errno and the path — is
    // where the difference actually lives.
    RejectTakeIo         = 22,
    // CmdTakeRelease named a take this daemon is not holding. Harmless (a
    // duplicate release, or one that outlived an engine restart) and counted
    // anyway, because the alternative is a client that thinks it has been
    // freeing takes for an hour.
    RejectNoTake         = 23,

    // --- generic device state (v10) -----------------------------------------
    //
    // The state string arrived, and the device DID NOT KEEP IT: after
    // setStateString() answered true, the device's own stateString() came back
    // empty for a non-empty input. That is the base-class default — accept
    // anything, remember nothing — so this reason is the boundary refusing
    // "accepted and ignored" one more time, and it is what makes a state sent
    // to the wrong device a refusal instead of silence. See
    // Daemon::doSetDeviceState for why the round trip is the test and why it is
    // a NON-EMPTINESS test rather than an equality one.
    RejectNotStateful     = 24,
    // The blob's own shape (version, length, terminator), or a state string the
    // device refused. One reason for both, for the reason RejectBadRackState
    // gives: setStateString answers a bare bool and inventing a distinction the
    // plugin layer does not make would be this header claiming to know
    // something it does not.
    RejectBadDeviceState  = 25,
    // The state named a sample block and it did not validate, or its shape
    // disagreed with what the header said, or the device it named plays no
    // files. Distinct from RejectBadPoolRef because the client's recourse is
    // different: a bad clip offset is a clip that does not sound, and this is a
    // device that will sound EMPTY while it is drawn full — the exact failure
    // this whole channel exists to end, so it gets its own line in the log.
    RejectBadDeviceSample = 26,
};

inline const char* rejectReasonName(u32 r) {
    switch (r) {
        case RejectPointerPayload:  return "carries a pointer the daemon owns";
        case RejectUnknownCommand:  return "unknown command type";
        case RejectBadIndex:        return "track/slot index out of range";
        case RejectNotFinite:       return "non-finite scalar";
        case RejectNoPool:          return "no sample pool is attached";
        case RejectBadPoolRef:      return "sample pool offset failed validation";
        case RejectBadClip:         return "clip fields are inconsistent";
        case RejectUnknownUri:      return "no plugin with that URI was found";
        case RejectInstantiate:     return "the plugin failed to load or activate";
        case RejectDeviceTableFull: return "the device table is full";
        case RejectChainFull:       return "the chain is full";
        case RejectBadDevice:       return "no such device";
        case RejectBadString:       return "the string blob is not terminated";
        case RejectScanBusy:        return "the plugin scan is busy and the queue is full";
        case RejectBadAutomation:   return "the automation blob is inconsistent";
        case RejectBadArrangement:  return "the arrangement blob is inconsistent";
        case RejectNotARack:        return "that device is not a rack";
        case RejectBadRackState:    return "the rack state would not parse or would not apply";
        case RejectBadSignatures:   return "the signature map is not walkable";
        case RejectTakeBusy:        return "a take is already in flight, or too many are unclaimed";
        case RejectTakeTooLarge:    return "the take capacity asked for is past the ceiling";
        case RejectTakeIo:          return "the take's buffer or its file could not be written";
        case RejectNoTake:          return "no take with that id is held";
        case RejectNotStateful:     return "that device does not keep a state string";
        case RejectBadDeviceState:  return "the device state would not parse or would not apply";
        case RejectBadDeviceSample: return "the sample the device state named is not usable";
        default:                    return "none";
    }
}

// ---------------------------------------------------------------------------
// Wire types
// ---------------------------------------------------------------------------
//
// Fixed width, no pointers, no bool, trivially copyable — the ring's static
// asserts enforce the last one and the reviews enforce the rest. `ref` is the
// field every pointer becomes: a pool offset in phase 2, a device id in phase
// 3. It is carried now, unused, so adding those phases does not change the
// region layout.

struct WireCommand {
    u32 type;        // lat::Cmd
    u32 flags;
    i32 a, b;
    f64 x;
    u64 ref;
};

struct WireEvent {
    u32 type;        // lat::Ev, or one of the Ev* daemon codes above
    u32 flags;
    i32 a, b;
    f64 x;
    u64 ref;
};

// The wire twin of lat::MidiMsg. Identical layout by construction (asserted
// below), so the daemon's translation is a field copy the compiler folds away,
// and a future change to MidiMsg cannot silently change the wire.
struct WireMidi {
    u8  status, d1, d2, pad;
    i32 frame;
};

static_assert(std::is_trivially_copyable_v<WireCommand>);
static_assert(std::is_trivially_copyable_v<WireEvent>);
static_assert(std::is_trivially_copyable_v<WireMidi>);
static_assert(sizeof(WireCommand) == 32 && sizeof(WireEvent) == 32);
static_assert(sizeof(WireMidi) == sizeof(MidiMsg), "WireMidi must mirror MidiMsg");

// ---------------------------------------------------------------------------
// WireClip — lat::RtClip with the five pointers replaced by pool offsets
// ---------------------------------------------------------------------------
//
// Every scalar of RtClip, plus `sampleRef`/`notesRef` where `data` and `notes`
// used to be — and, from v11, `autoRef`/`markersRef`/`transientsRef` where
// `autos`/`markers`/`transients` used to be nothing at all (GUI-ON-DAEMON.md
// §17). This is the whole of §2.4 and half of §2.5's problem, solved: the
// GUI names sample data by an offset into a region the daemon also maps, and
// the daemon does the one addition that turns it back into a pointer.
//
// It travels in a *table*, not in the command, for the reason §3.4 gives: 112
// bytes do not fit a 32-byte message, and of the two ways to carry them the
// table is idempotent (a cell can be rewritten and re-sent with no ordering
// question) and makes republish-after-engine-restart a memcpy rather than a
// protocol. Cmd::SetClip{a=track, b=slot, ref=generation} says which cell moved.
//
// `generation` is the cell's own counter, bumped by the client on every write
// and echoed by the daemon in EvClipAccepted. It exists because a table is
// mutable state shared with a peer that reads it later: if the client wrote a
// cell twice before the daemon popped the first command, the daemon would read
// the *second* value for both commands and never learn what the first one
// displaced — a pool block that is retired on the client's books and never
// retired on the daemon's, i.e. a leak. So the client refuses to overwrite a
// cell that has not been acknowledged yet and retries on the next frame,
// which is the same "handle a refused push" discipline §5 already requires of
// every ring push. One 1 ms pump tick is the entire cost.
struct WireClip {
    u64 sampleRef;      // pool offset of interleaved f32, 0 = none  <-- was const f32*
    u64 notesRef;       // pool offset of WireNote[], 0 = none       <-- was const RtNote*
    // v11: the three pointers that used to have no wire spelling at all.
    // Their counts sit beside noteCount below, i64 like it, because the count
    // is a multiply operand for the blob's byte extent and the far side bounds
    // it BEFORE multiplying (see buildClip in nxtaktd.cpp).
    u64 autoRef;        // PoolKindAutomation blob, 0 = none          <-- was const RtAutoSet*
    u64 markersRef;     // PoolKindWarp blob, 0 = none                <-- was const WarpMarker*
    u64 transientsRef;  // PoolKindTransients blob, 0 = none          <-- was const i64*
    i64 frames;
    i64 loopStart, loopEnd;
    i64 noteCount;
    i64 autoLaneCount;      // <= kMaxClipAutoLanes  (== kMaxRtAutoLanes)
    i64 autoPointCount;     // <= kMaxClipAutoPoints, across all lanes
    i64 markerCount;        // 0, or >= 2 (one marker pins nothing) and <= kMaxWarpMarkers
    i64 transientCount;     // <= kMaxWireTransients (== sample.h's kMaxTransients)
    f64 clipBpm, lengthBeats, prob, followBeats;
    f32 gain;
    i32 channels;
    i32 warp;           // lat::Warp
    i32 quantumIdx;     // -1 = follow the global quantum
    i32 followAction;   // lat::Follow
    u32 loop;           // u32, never bool
    u32 isMidi;
    u32 valid;
    u32 generation;     // +1 per client write to this cell
    u32 reserved;       // spelled out rather than left to the padding
};

static_assert(std::is_trivially_copyable_v<WireClip>);
static_assert(sizeof(WireClip) == 176, "WireClip is part of the region layout");
static_assert(alignof(WireClip) == 8);

// The payload bounds, protocol from v11 on. Each count above is a multiply
// operand for a byte extent and an index bound the engine will trust, so each
// gets a ceiling here exactly as kMaxArr* got theirs — and a blob past one is
// REFUSED whole, never truncated.
//
//   kMaxClipAutoLanes   == kMaxRtAutoLanes: RtAutoSet's lane array is fixed
//                         width by value, so this is the container's own
//                         capacity, not a policy. Asserted below.
//   kMaxClipAutoPoints  AUTOMATION.md §2.1's number: 4096 breakpoints across
//                         a clip's lanes is minutes of dense hand-drawn
//                         automation on a payload that is *about* one or two
//                         parameters.
//   kMaxWarpMarkers     a marker is a transient somebody pinned to a beat, so
//                         the transient cap is the natural ceiling for the map
//                         derived from it.
//   kMaxWireTransients  == kMaxTransients (audio/sample.h): the detector stops
//                         adding at 8192, so a grid past that was not built by
//                         this codebase. The equality is asserted in
//                         engine_handle.cpp, where sample.h is visible —
//                         this header may include only core/ and engine.h.
inline constexpr i64 kMaxClipAutoLanes  = (i64)kMaxRtAutoLanes;
inline constexpr i64 kMaxClipAutoPoints = 4096;
inline constexpr i64 kMaxWarpMarkers    = 8192;
inline constexpr i64 kMaxWireTransients = 8192;

static_assert(kMaxClipAutoLanes == 16,
              "RtAutoSet's fixed lane width is shipped protocol; a change to "
              "kMaxRtAutoLanes must ride a version bump, not slip through here");

// The envelope blob's byte extent: [WireAutoLane[lanes]][WireAutoPoint[points]],
// counts bounded by the caller FIRST (the same deliberately-not-defensive
// contract arrangementBytes states). WireAutoLane's trailing pad word makes its
// size a multiple of alignof(WireAutoPoint) — asserted beside the struct — so
// the point array lands aligned for any lane count, given the 64-aligned
// offsets poolValidate guarantees.
inline constexpr u64 clipAutosBytes(i64 laneCount, i64 pointCount);

// Defaults that match RtClip's, so a client can send a half-filled cell and get
// the engine's documented behaviour rather than a clip at 0 BPM.
inline WireClip defaultWireClip() {
    WireClip c{};
    c.channels     = 1;
    c.clipBpm      = 120.0;
    c.lengthBeats  = 4.0;
    c.gain         = 1.0f;
    c.warp         = (i32)Warp::Beats;
    c.loop         = 1;
    c.quantumIdx   = -1;
    c.prob         = 1.0;
    c.followAction = (i32)Follow::None;
    return c;
}

// The name the task-level API uses for the same thing; RtClip's wire twin is
// only ever sent by Cmd::SetClip, so both spellings mean this struct.
using WireSetClip = WireClip;

// ---------------------------------------------------------------------------
// The arrangement on the wire (docs/ARRANGEMENT.md §9.2)
// ---------------------------------------------------------------------------
//
// One blob per track, allocated and written by the client, referenced by
// Cmd::SetArrangement{a = track, b = generation, ref = poolOffset}. Layout:
//
//   [WireArrHeader][WireArrItem[itemCount]][WireClip[clipCount]]
//
// The notes are NOT in the blob. Each WireClip names its own notesRef into the
// pool exactly as a session clip's does, so the existing WireNote
// reinterpretation and the existing per-block retirement both keep working.
//
// TRANSLATED, NOT REINTERPRETED. This is the one structural difference from
// every other pooled payload and it is a security property rather than a
// stylistic one. A WireNote blob is *reinterpreted* — WireNote mirrors RtNote
// field for field and holds no pointers, so (const RtNote*)(poolBase + ref) is
// honest and a 10 000-note clip costs nothing at the boundary. An arrangement
// blob cannot be: RtClip holds five pointers (data, notes, autos, markers,
// transients) and **a pointer in a client-writable region is a pointer the
// client chose**. So the daemon BUILDS — see Daemon::translateArrangement.

// The bounds, mirrored from src/ui/session.h. They are protocol from v5 on
// rather than editor policy: the engine's per-block cost is O(1) only while the
// lane invariant holds, and the engine is downstream of an untrusted process.
// src/ipc may not include src/ui, so the numbers are repeated here and the one
// that also exists in engine.h is asserted against it.
inline constexpr i64 kMaxArrItems  = 512;      // items per track
inline constexpr i64 kMaxArrNotes  = 65536;    // notes per track, over every item
inline constexpr i64 kMaxArrLanes  = 32;       // arrangement automation lanes per track
inline constexpr i64 kMaxArrPoints = 65536;    // breakpoints per track across its lanes
inline constexpr f64 kMaxOverlapBeats = 4.0;   // the longest admissible crossfade
inline constexpr f64 kMinArrBeats     = 1.0 / 64.0;
inline constexpr f64 kArrOverlapEps   = 1e-9;  // float noise from a trim, not an overlap
// The timeline is finite in the only sense that matters here: every beat a
// client sends becomes a multiply against the tempo somewhere downstream, so it
// gets a bound like every other multiply operand. 2^31 beats is ~340 years at
// 120 BPM and leaves start+length far inside f64's exact-integer range.
inline constexpr f64 kMaxArrBeat = 2147483648.0;

static_assert((int)kMaxArrLanes == kMaxRtArrLanes,
              "the wire lane bound and the engine's must be the same number");

struct WireArrHeader {
    i64 itemCount, clipCount, noteCount;
    f64 loopStart, loopEnd;
    u32 loopOn, pad;
};

struct WireArrItem {
    f64 start, length, offset;
    f32 fadeIn, fadeOut;
    i32 fadeShape, clip;         // `clip` indexes the blob's WireClip[]
};

static_assert(std::is_trivially_copyable_v<WireArrHeader>);
static_assert(std::is_trivially_copyable_v<WireArrItem>);
static_assert(sizeof(WireArrHeader) == 48, "WireArrHeader is protocol");
static_assert(sizeof(WireArrItem)   == 40, "WireArrItem is protocol");
static_assert(alignof(WireArrHeader) == 8 && alignof(WireArrItem) == 8);
// Every section of the blob has to land 8-aligned given an offset that is
// 64-aligned by poolValidate, or the daemon's reads are unaligned by
// construction rather than by a client's choice.
static_assert(sizeof(WireArrHeader) % 8 == 0 && sizeof(WireArrItem) % 8 == 0 &&
              sizeof(WireClip) % 8 == 0);

// The byte extent a blob with these declared counts must have. Callers bound
// the counts FIRST — this is deliberately not defensive, so that a caller which
// forgot the bound gets a wrong answer in a test rather than a silent clamp in
// production. (Every caller in this tree bounds them; daemon_test proves it.)
inline constexpr u64 arrangementBytes(i64 itemCount, i64 clipCount) {
    return (u64)sizeof(WireArrHeader) + (u64)itemCount * sizeof(WireArrItem) +
           (u64)clipCount * sizeof(WireClip);
}

// ---------------------------------------------------------------------------
// Arrangement automation on the wire (§6.2)
// ---------------------------------------------------------------------------
//
//   [WireAutoSetHeader][WireAutoLane[laneCount]][WireAutoPoint[pointCount]]
//
// The same single-allocation shape RtAutoSetN has. The two element types mirror
// RtAutoLane/RtAutoPoint field for field, asserted below — but unlike WireNote
// the mirror does NOT license a cast, because RtAutoSetN itself holds two
// pointers. The mirror is here so a copy is a memcpy and so the engine's
// structs cannot change under the protocol without the build noticing.

// ALIGNMENT IS NOT INCIDENTAL HERE, and it is not theoretical: `RtAutoLane` is
// 36 bytes and 4-aligned, while `RtAutoPoint` begins with an f64. An ODD lane
// count therefore leaves the point array on a 4-byte boundary in any layout
// that just concatenates the two arrays — which UBSan catches, on the engine
// side, where the misaligned f64 is actually read.
//
// Two independent places have to get this right and both do it explicitly
// rather than by arithmetic that happens to divide today:
//
//   * the WIRE blob, below, where WireAutoLane spells out a trailing pad word
//     so its size is a multiple of alignof(WireAutoPoint). The static_assert is
//     the guard: remove the pad and this stops compiling.
//   * the DAEMON's built block, which rounds every array offset up to the
//     member's own alignment (Daemon::buildTrackAutos). That one cannot use the
//     wire's trick, because it lays out RtAutoLane and not WireAutoLane.
struct WireAutoSetHeader { i64 laneCount, pointCount; };

struct WireAutoLane {
    i32 target, index, devSlot, xform;
    i32 first, count;
    f32 lo, hi;
    u32 flags;
    u32 pad;             // load-bearing: see the alignment note above
};

struct WireAutoPoint {
    f64 beat;
    f32 value;
    u8  curve;
    u8  pad[3];
};

static_assert(std::is_trivially_copyable_v<WireAutoSetHeader>);
static_assert(sizeof(WireAutoSetHeader) == 16, "WireAutoSetHeader is protocol");
static_assert(sizeof(WireAutoPoint) == sizeof(RtAutoPoint), "WireAutoPoint must mirror RtAutoPoint");
static_assert(alignof(WireAutoPoint) == alignof(RtAutoPoint));
static_assert(offsetof(WireAutoPoint, beat)  == offsetof(RtAutoPoint, beat));
static_assert(offsetof(WireAutoPoint, value) == offsetof(RtAutoPoint, value));
static_assert(offsetof(WireAutoPoint, curve) == offsetof(RtAutoPoint, curve));
// RtAutoLane has no trailing pad member of its own; the wire twin spells one
// out so the struct's size is stated rather than inherited from the ABI.
static_assert(sizeof(WireAutoLane) == sizeof(RtAutoLane) + 4 ||
              sizeof(WireAutoLane) == sizeof(RtAutoLane),
              "WireAutoLane must mirror RtAutoLane's fields");
static_assert(offsetof(WireAutoLane, target)  == offsetof(RtAutoLane, target));
static_assert(offsetof(WireAutoLane, index)   == offsetof(RtAutoLane, index));
static_assert(offsetof(WireAutoLane, devSlot) == offsetof(RtAutoLane, devSlot));
static_assert(offsetof(WireAutoLane, xform)   == offsetof(RtAutoLane, xform));
static_assert(offsetof(WireAutoLane, first)   == offsetof(RtAutoLane, first));
static_assert(offsetof(WireAutoLane, count)   == offsetof(RtAutoLane, count));
static_assert(offsetof(WireAutoLane, lo)      == offsetof(RtAutoLane, lo));
static_assert(offsetof(WireAutoLane, hi)      == offsetof(RtAutoLane, hi));
static_assert(offsetof(WireAutoLane, flags)   == offsetof(RtAutoLane, flags));

// The blob's sections concatenate with no padding, which is only legal because
// each one's size is a multiple of the NEXT one's alignment. Asserted rather
// than assumed: RtAutoLane's real size is 36 and it is only WireAutoLane's
// explicit pad word that makes an odd lane count safe here.
static_assert(sizeof(WireAutoSetHeader) % alignof(WireAutoLane) == 0);
static_assert(sizeof(WireAutoLane) % alignof(WireAutoPoint) == 0,
              "an odd lane count would leave the point array misaligned: "
              "WireAutoLane needs its trailing pad");
static_assert(sizeof(WireArrHeader) % alignof(WireArrItem) == 0);
static_assert(sizeof(WireArrItem) % alignof(WireClip) == 0,
              "an odd item count would leave the clip array misaligned");

inline constexpr u64 trackAutosBytes(i64 laneCount, i64 pointCount) {
    return (u64)sizeof(WireAutoSetHeader) + (u64)laneCount * sizeof(WireAutoLane) +
           (u64)pointCount * sizeof(WireAutoPoint);
}

// Declared beside WireClip, defined here where the element types exist. The
// CLIP blob has no header — the counts travel in the WireClip cell, exactly as
// noteCount does — so this is trackAutosBytes minus the header term.
inline constexpr u64 clipAutosBytes(i64 laneCount, i64 pointCount) {
    return (u64)laneCount * sizeof(WireAutoLane) + (u64)pointCount * sizeof(WireAutoPoint);
}

// ---------------------------------------------------------------------------
// WireJournal — the recording journal's message (§5.3, §9.6)
// ---------------------------------------------------------------------------
//
// lat::ArrJournal field for field, pointer-free, asserted to mirror. It rides a
// ring of its own and NOT the event ring, because the two channels have
// different failure budgets: an event must not be lost (EvClipAck wedges a clip
// cell for the rest of the session if it is), and a journal entry must be
// *known* to be lost (a take with a gap is refused rather than committed short).
// Mixing them lets a burst of journal entries evict an acknowledgement.
struct WireJournal {
    u32 kind;      // lat::JournalKind
    u32 seq;       // monotonic per engine run; a gap means a drop
    i32 track;
    i32 a;         // slot / pitch / velocity, per kind
    f64 beat;      // the ENGINE's beat, exact
};

static_assert(std::is_trivially_copyable_v<WireJournal>);
static_assert(sizeof(WireJournal) == sizeof(ArrJournal), "WireJournal must mirror ArrJournal");
static_assert(alignof(WireJournal) == alignof(ArrJournal));
static_assert(offsetof(WireJournal, kind)  == offsetof(ArrJournal, kind));
static_assert(offsetof(WireJournal, seq)   == offsetof(ArrJournal, seq));
static_assert(offsetof(WireJournal, track) == offsetof(ArrJournal, track));
static_assert(offsetof(WireJournal, a)     == offsetof(ArrJournal, a));
static_assert(offsetof(WireJournal, beat)  == offsetof(ArrJournal, beat));

// ---------------------------------------------------------------------------
// Devices: two tables, opposite directions
// ---------------------------------------------------------------------------
//
// Phase 3's problem is §2.5's: `RtChain::fx` is an array of PluginInstance*,
// and both the pointer and the object it names have to stop crossing. They do,
// by the plugin moving into the daemon (§3.6). What is left crossing is
// *description* and *value*, and those pull in opposite directions, so they get
// one table each:
//
//   device table   WireDeviceInfo[kMaxDevices]    DAEMON writes, client reads.
//                  What a loaded plugin is: uri, name, latency, and one
//                  WireParamInfo per control. Static per instance, so it is
//                  written once at AddDevice and read once at EvDeviceAdded.
//
//   param table    WireDeviceParams[kMaxDevices]  CLIENT writes, daemon reads.
//                  What its controls are currently set to. Written at knob
//                  rate, scanned by the daemon's pump every millisecond.
//
// Both are preallocated in the control region for the reason everything here
// is: no side may wait for an allocation, and a republish after an engine
// restart must not need one either.
//
// §3.7 puts the param table in the *session* region instead, so that it
// survives an engine restart the way the sample pool does. It is here because
// the device *ids* it is indexed by are the daemon's, and they do not survive:
// a respawned daemon re-instantiates from scratch and hands out fresh ids, so
// a surviving table would be indexed by numbers that no longer mean anything.
// The client keeps its own mirror and re-pushes after a respawn, exactly as it
// re-pushes the clip table — see §11.4.

// A device id is an index into both tables. There are enough for every chain
// position the engine can address (32 tracks + 4 returns + master, 8 devices
// each = 296) with room to spare, so "the table is full" means a leak, not a
// large session.
inline constexpr u32 kMaxDevices   = 320;

// Controls per device that cross the boundary. §3.7 sketches 256; 64 covers
// every plugin in practice and keeps the table at 1.4 MiB instead of 5.5.
// A device with more reports the first 64 and says so in `truncatedParams`,
// which is a visible, testable degradation rather than a silent one.
inline constexpr u32 kMaxDevParams = 64;

static_assert(kMaxDevices >= (u32)(kMaxTracks * kMaxChainFx + kMaxReturns * kMaxChainFx + kMaxChainFx),
              "the device table must be able to hold every addressable chain position");

// shm.h carries the return-bus meters but cannot see kMaxReturns — it includes
// core/ and nothing else, deliberately. This header includes both, so this is
// where the duplicated number is held to account (shm.h, kShmReturns).
static_assert(kShmReturns == kMaxReturns,
              "SharedState's return-meter arrays must be exactly kMaxReturns wide");

// WireParamInfo::flags — lat::ParamInfo's three booleans, as bits.
enum : u32 {
    ParamIsBool = 1u << 0,
    ParamIsInt  = 1u << 1,
    ParamIsLog  = 1u << 2,
};

// lat::ParamInfo with the two std::strings truncated to fixed widths. A name
// longer than 31 bytes is cut, never wrapped and never heap-allocated: this
// struct lives in shared memory and the whole point of the exercise is that
// nothing in it can point anywhere.
struct WireParamInfo {
    f32  min, max, def;
    u32  id;              // backend-defined; LV2 uses the port index
    u32  flags;           // ParamIs*
    u32  reserved;
    char name[32];        // NUL-terminated, truncated
    char unit[8];         // "dB", "Hz", ""
};
static_assert(std::is_trivially_copyable_v<WireParamInfo>);
static_assert(sizeof(WireParamInfo) == 64, "WireParamInfo is part of the region layout");

// WireDeviceInfo::state.
enum : u32 {
    DeviceSlotFree = 0,
    DeviceSlotLive = 1,
};

// Written by the daemon, read by the client. `state` and `generation` are the
// only atomics, and `state` is written *last* with release for the same reason
// PoolBlock::magic is: a reader that sees the slot live has seen every field
// that describes it.
struct WireDeviceInfo {
    std::atomic<u32> state;        // DeviceSlot*
    std::atomic<u32> generation;   // +1 per allocation of this slot; never reused
    std::atomic<u32> bypass;       // what the daemon actually applied
    u32 paramCount;                // <= kMaxDevParams
    u32 truncatedParams;           // controls beyond kMaxDevParams, 0 normally
    i32 target;                    // DevTarget*
    i32 targetIdx;                 // track or return index
    i32 chainPos;                  // position within that chain
    i32 latencyFrames;             // PluginInstance::latencyFrames()
    u32 format;                    // lat::PluginFormat
    u32 kind;                      // lat::PluginKind
    u32 audioIn, audioOut;
    u32 hasMidiIn;
    u32 reserved[2];
    char uri[128];
    char name[64];
    char vendor[64];
    WireParamInfo params[kMaxDevParams];
};
static_assert(std::is_trivially_copyable_v<WireDeviceInfo>);
static_assert(sizeof(WireDeviceInfo) == 320 + sizeof(WireParamInfo) * kMaxDevParams,
              "WireDeviceInfo is part of the region layout");

// Written by the client, read by the daemon's pump thread.
//
// This is §3.7 relocated and otherwise unchanged: the GUI stores a plain float
// and bumps a generation; the reader notices the generation and re-reads. No
// ring, therefore no drops — a dropped param write would leave the knob and the
// plugin permanently disagreeing, which is the worst bug class in this corner
// of the system.
//
// `deviceGeneration` is the one addition a process boundary forces. Device ids
// are reused, so a write that was in flight when a device was removed could
// otherwise land on its replacement. The client stamps the slot generation it
// believes it is talking to and the daemon ignores anything stale.
struct WireDeviceParams {
    std::atomic<u32> generation;        // client: +1 per write batch (release)
    std::atomic<u32> engineGeneration;  // daemon: +1 when the plugin moved a param
    std::atomic<u32> deviceGeneration;  // client: the WireDeviceInfo generation it wrote for
    std::atomic<u32> reserved;
    std::atomic<f32> value[kMaxDevParams];
};
static_assert(sizeof(WireDeviceParams) == 16 + 4 * kMaxDevParams,
              "WireDeviceParams is part of the region layout");
static_assert(std::atomic<f32>::is_always_lock_free,
              "a param table of non-lock-free atomics would put a futex in the pump");

// ---------------------------------------------------------------------------
// The plugin catalog: one more table, same direction as the device table
// ---------------------------------------------------------------------------
//
// docs/GUI-ON-DAEMON.md §3 sizes three ways to get the browser its list and
// picks this one. The argument in one line: the registry lives in the daemon
// now, so a GUI that browses its OWN PluginRegistry is browsing a different
// machine's answer — a different LV2_PATH, a different user, a different set of
// bundles that crash lilv — and a double-click on a row it can see but the
// daemon cannot ends in EvDeviceFailed(RejectUnknownUri) with no way to have
// known. The catalog is therefore what the DAEMON can instantiate, published
// where a client can read it.
//
// Same discipline as WireDeviceInfo and for the same reason: `state` is stored
// LAST with release, so a reader that sees a row Live has seen every byte of
// it. Unlike the device table this one is written exactly once per scan, from
// the pump thread, before scanState goes to ScanDone — so a client that walks
// [0, catalogCount) after seeing ScanDone (or EvScanComplete) is reading a
// table nobody will touch again.
//
// §3 option C — the AF_UNIX socket — is strictly better and three or four times
// the work, and this is deliberately NOT a detour on the way to it: when the
// socket lands the catalog moves onto it and this table is deleted.
enum : u32 {
    CatalogSlotFree = 0,
    CatalogSlotLive = 1,
};

// PluginDesc with its four std::strings truncated to fixed widths. `category`
// is carried even though nothing draws it yet, because the browser will want to
// group by it and adding a field later costs another version bump.
struct WirePluginDesc {
    std::atomic<u32> state;        // CatalogSlot*, stored LAST with release
    u32 format;                    // lat::PluginFormat
    u32 kind;                      // lat::PluginKind
    u32 audioIn, audioOut;
    u32 hasMidiIn;
    u32 paramCount;                // what the SCANNER found, not kMaxDevParams
    u32 reserved;
    char uri[256];                 // LV2 URIs are long; 256 is the pool's own limit
    char name[96];
    char vendor[64];
    char category[64];
};
static_assert(std::is_trivially_copyable_v<WirePluginDesc>);
static_assert(sizeof(WirePluginDesc) == 512, "WirePluginDesc is part of the region layout");

// 2048 x 512 B = 1 MiB, one page of it resident until a scan writes into it.
// The machine this was developed on has 410 plugins; a heavily populated LV2
// install is a few hundred more. A catalog larger than this reports the first
// kMaxCatalog and says how many it could not carry in
// ControlHeader::catalogTruncated, which the browser draws — a visible,
// testable degradation, never a silently short list.
inline constexpr u32 kMaxCatalog = 2048;

// ---------------------------------------------------------------------------
// Command policy
// ---------------------------------------------------------------------------
//
// Three classes now, where phase 1 had two.
//
//   scalar   Seventeen commands whose whole payload is `a`, `b` and `x`
//            (docs/PROCESS-SPLIT.md §2.1). They cross unchanged.
//   pooled   SetClip and ClearClip. Their payload is a WireClip in the clip
//            table, whose pointers have become pool offsets — so they cross,
//            but only after the daemon has validated every offset against its
//            own mapping of the pool. This is phase 2's entire delta.
//   device   The five CmdAddDevice-family codes above. They are not lat::Cmd
//            at all: the daemon consumes them, loads or unloads a plugin, and
//            *generates* the Cmd::SetChain the engine sees. Phase 3's delta.
//   take     RecordSlot and RecordMidiSlot. v9's delta, and the one class whose
//            payload does not travel at all: the command carries a CAPACITY and
//            the daemon supplies the buffer. What comes back is a file.
//   refused  The three chain commands, and now only those. Their `Command::p` is
//            an RtChain* full of PluginInstance* built by the daemon from its own
//            device table: a client has no business naming one, because it has
//            no RtChains. This is not "not yet" — it is the design. Use
//            AddDevice.
inline constexpr bool commandIsScalar(u32 type) {
    if (type >= kDaemonCommandBase) return false;
    switch ((Cmd)type) {
        case Cmd::SetPlaying: case Cmd::SetTempo: case Cmd::SetQuantum:
        case Cmd::SetMetronome: case Cmd::LaunchClip: case Cmd::StopTrack:
        case Cmd::LaunchScene: case Cmd::StopAll: case Cmd::TrackVol:
        case Cmd::TrackPan: case Cmd::TrackMute: case Cmd::TrackSolo:
        case Cmd::TrackArm: case Cmd::MasterVol: case Cmd::ClipGain:
        case Cmd::ClipWarp: case Cmd::ClipLoop:
        // Phase 3: the bus topology's own scalars. SendLevel and ReturnVol are
        // pure numbers into the engine's mixer and always were; they only look
        // new because the engine grew return buses in the same wave.
        case Cmd::SendLevel: case Cmd::ReturnVol:
        // Wave 8g: the arrangement's two pure scalars. Locate is {x = the beat
        // to go to} and BackToArrangement is {a = track, or -1 for all} — no
        // payload, no pool reference, nothing to translate.
        case Cmd::Locate: case Cmd::BackToArrangement:
            return true;
        case Cmd::SetClip: case Cmd::ClearClip:
        case Cmd::SetChain: case Cmd::SetReturnChain: case Cmd::SetMasterChain:
        case Cmd::RecordSlot: case Cmd::RecordMidiSlot:
        case Cmd::SetArrangement: case Cmd::SetTrackAutos:
        // Time signatures: Cmd::SetSignatures carries a pointer to a GUI-built
        // map, so it is not a scalar. As of v8 it is a pooled BLOB
        // (commandIsSignatures) and the daemon honours it; before that it sat
        // outside commandIsKnown's bound on purpose, answering
        // RejectUnknownCommand, and the daemon played every set in 4/4.
        case Cmd::SetSignatures:
            return false;
    }
    return false;
}

// Carries a clip cell rather than a scalar: the daemon reads the table, not
// the command.
inline constexpr bool commandIsPooled(u32 type) {
    return type < kDaemonCommandBase &&
           ((Cmd)type == Cmd::SetClip || (Cmd)type == Cmd::ClearClip);
}

// Carries a pool BLOB rather than a table cell: `ref` names a block the client
// allocated and the daemon *builds* from (§9.3). A fifth class rather than a
// fourth flavour of `pooled`, because the two behave differently in every way
// that matters at the boundary: a pooled command names a cell in a table that
// exists whether or not anything is in it and is answered by EvClipAck; this
// one names a variable-length blob, is answered by EvArrangementAck, and its
// payload is a daemon-heap allocation with a two-layer retirement behind it.
// `ref == 0` is the clear form and is legal.
inline constexpr bool commandIsArrangement(u32 type) {
    return type < kDaemonCommandBase &&
           ((Cmd)type == Cmd::SetArrangement || (Cmd)type == Cmd::SetTrackAutos);
}

// Carries the signature map as a PoolKindSignatures blob: a = the entry count,
// b = the client's generation, ref = the offset (0 clears). A class of its own
// rather than a third arrangement flavour, because it is answered by a different
// event and because it is addressed by nothing — there is one map per set, not
// one per track. `ref == 0` is the clear form and is legal.
inline constexpr bool commandIsSignatures(u32 type) {
    return type < kDaemonCommandBase && (Cmd)type == Cmd::SetSignatures;
}

// Consumed by the daemon, never forwarded verbatim. The bound is spelled as the
// LAST daemon command for the reason commandIsKnown gives below: a hand-copied
// name here is what left four appended commands classifying as unknown for a
// whole wave.
inline constexpr bool commandIsDevice(u32 type) {
    return type >= kDaemonCommandBase && type <= CmdSetDeviceState;
}

// A take command: `a` = track, `b` = slot, `x` = capacity in FRAMES (or in
// NOTES when flags & TakeCmdMidi), ref unused. A class of its own and not a
// scalar, even though every field it uses is a number, because the daemon does
// something no scalar does with it — it allocates, it holds a per-track state
// machine, and it owes an EvTakeReady or an EvTakeFailed for every start it
// accepted. Classifying it as a scalar would forward it straight to the engine
// with `p` null, which is a Record command the engine takes and then writes
// nothing into: a silent take.
inline constexpr bool commandIsTake(u32 type) {
    return type < kDaemonCommandBase &&
           ((Cmd)type == Cmd::RecordSlot || (Cmd)type == Cmd::RecordMidiSlot);
}

// The client's half of the take's free-after-confirm. Consumed by the daemon,
// never forwarded, answers nothing.
inline constexpr bool commandIsTakeRelease(u32 type) { return type == CmdTakeRelease; }

// Names memory the sender does not own on the receiving side. Permanently
// refused, not deferred.
inline constexpr bool commandCarriesPointer(u32 type) {
    return !commandIsScalar(type) && !commandIsPooled(type) && !commandIsDevice(type) &&
           !commandIsArrangement(type) && !commandIsSignatures(type) &&
           !commandIsTake(type) && !commandIsTakeRelease(type);
}

// True for a type this build knows at all. An unknown type is a peer from the
// future; the version check should already have caught it, so this is the
// belt to that pair of braces.
//
// The bound is the LAST enumerator, not a hand-copied number: 8a appended four
// commands and this line still said RecordMidiSlot, so SetArrangement..
// BackToArrangement classified as unknown. That failed closed — the daemon
// answered RejectUnknownCommand — which is the right way for it to be wrong,
// and it is why 8a could land the header compiled-and-unused without breaking
// anything. Spelling the bound as the last enumerator is what keeps the next
// append from repeating it.
//
// The bound moved to Cmd::SetSignatures in v8, and ONLY because the daemon now
// genuinely honours that command. Moving it earlier would have turned a visible
// RejectUnknownCommand into a silently accepted no-op, which is the trade this
// whole boundary refuses to make.
inline constexpr bool commandIsKnown(u32 type) {
    return type <= (u32)Cmd::SetSignatures || commandIsDevice(type) ||
           commandIsTakeRelease(type);
}

// Events that cannot cross as they stand: each hands a pointer back to whoever
// allocated it. Two of the four are now *consumed* rather than dropped, and
// both stay false here because "may not be forwarded verbatim" is what this
// predicate means:
//
//   Ev::NotesRetired  (phase 2) its pointer lands inside the sample pool, so
//                     the daemon turns it back into the offset the client
//                     knows it by and republishes it as EvBlockRetired.
//   Ev::ChainRetired  (phase 3) its pointer is a chain the *daemon* built, so
//                     the daemon keeps it: the retirement dance §2.5 describes
//                     still happens, it just happens between two threads of
//                     one process now. §3.6 said this event would disappear
//                     from the protocol; it disappeared from the wire.
//   Ev::RecordFinished / Ev::MidiRecordFinished
//                     (v9) their pointer is the take buffer the DAEMON handed
//                     the engine, so the daemon keeps it: the take is written to
//                     a file and re-announced as EvTakeReady. Same treatment as
//                     Ev::ChainRetired, same reason. They stay false HERE
//                     because "false" means "may not be forwarded verbatim",
//                     which is exactly true of an event whose payload is an
//                     address in this process — and because the moment one of
//                     them is forwarded verbatim the client gets a pointer it
//                     would happily pass to delete[].
//
// AutosRetired, WarpRetired and SigsRetired remain unreachable, because the
// commands that would allocate their payloads are still refused or are
// translated into something else. If their counter ever moves, something
// reached the engine that should not have.
inline constexpr bool eventIsScalar(u32 type) {
    switch ((Ev)type) {
        case Ev::ClipStarted: case Ev::ClipStopped: case Ev::TrackStopped:
        case Ev::Xrun: case Ev::TransportStopped: case Ev::RecordStarted:
        // AutoLaneInert names a lane by index — no payload, so it crosses.
        case Ev::AutoLaneInert:
            return true;
        case Ev::ChainRetired: case Ev::RecordFinished: case Ev::NotesRetired:
        case Ev::MidiRecordFinished:
        // AutosRetired carries a pointer into GUI memory, like NotesRetired.
        case Ev::AutosRetired: case Ev::WarpRetired:
        // Wave 8g. Both carry a pointer to a block the DAEMON built in its own
        // address space (§9.3), so both are consumed rather than forwarded —
        // the same treatment Ev::ChainRetired gets, and for the same reason.
        // They are the tighter of the two accepted proofs for layer 1 of the
        // arrangement's retirement: pushed from inside drainCommands(), so the
        // event's arrival *is* the drain.
        case Ev::ArrangementRetired: case Ev::TrackAutosRetired:
        // SigsRetired carries a pointer into GUI memory, like AutosRetired.
        case Ev::SigsRetired:
            return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// ControlHeader
// ---------------------------------------------------------------------------
//
// Written by the daemon, read by everyone. The counters are not decoration:
// they are the only way a client (or a test) can tell "the daemon accepted my
// command" from "the daemon threw it away", which matters precisely because
// phase 1 throws some away on purpose.
struct ControlHeader {
    u32 protocolVersion;             // kProtocolVersion of the daemon
    u32 flags;
    i32 daemonPid;
    u32 driverIsNull;                // 1 = --driver null (no audio device)

    std::atomic<u64> heartbeat;      // +1 per daemon pump tick
    std::atomic<u64> startedNs;      // CLOCK_MONOTONIC at publishReady()
    std::atomic<u32> shutdown;       // 1 = the daemon has stopped cleanly
    std::atomic<u32> reserved0;

    // Boundary accounting.
    std::atomic<u64> commandsApplied;   // forwarded to Engine::pushCommand
    std::atomic<u64> commandsRejected;  // refused, with an EvCommandRejected
    std::atomic<u64> commandsDeferred;  // engine ring full, retried next tick
    std::atomic<u64> midiApplied;
    std::atomic<u64> eventsForwarded;
    std::atomic<u64> eventsDropped;     // pointer-carrying, cannot cross
    std::atomic<u64> clipsApplied;      // clip cells forwarded to the engine
    std::atomic<u64> blocksRetired;     // EvBlockRetired events published

    // --- devices (phase 3) --------------------------------------------------
    std::atomic<u64> devicesAdded;      // instances created
    std::atomic<u64> devicesRemoved;    // instances destroyed
    std::atomic<u64> devicesFailed;     // AddDevice answered with EvDeviceFailed
    std::atomic<u64> devicesLive;       // instances alive right now
    std::atomic<u64> paramWrites;       // setParam() calls made from the pump
    std::atomic<u64> chainsPublished;   // Cmd::Set*Chain handed to the engine
    std::atomic<u64> chainsRetired;     // RtChains freed after their proof landed
    std::atomic<u32> scanState;         // ScanState below
    std::atomic<u32> scanPlugins;       // catalog size once the scan is done

    // --- the retirement proof (§11.5) ---------------------------------------
    //
    // Engine::drains counts completed drainCommands() passes and is the exact
    // primitive phase 2's retirement deadline was standing in for. It is
    // republished here for one reason beyond diagnostics: a client (or a test)
    // cannot see the daemon's Engine, so without this it could not tell an
    // engine that counts its drains from one that does not — and the daemon's
    // behaviour differs between the two.
    std::atomic<u64> engineDrains;
    std::atomic<u32> drainsExact;       // 1 = the engine counts; retirement is a proof

    // Ev::RecordFinished events naming a buffer no take of the daemon's owns.
    //
    // IT MUST READ 0 FOREVER, and it is here for the same reason
    // arrOrderViolations is: the condition is structurally impossible and
    // "structurally impossible" is what this class of bug is called until
    // somebody weakens a guard. A non-zero value means the daemon FREED a
    // capture buffer while the engine still had an event in flight naming it —
    // which, once the allocator hands that address to the next take, is a
    // finished-take event applied to the wrong recording and a use-after-free
    // on the audio thread (audit 3, CRITICAL-1).
    //
    // Taken out of `reserved1` rather than appended, so ControlHeader keeps its
    // size and every section offset stays where it was — the same move
    // catalogCount and rackStatesApplied made. NO protocol bump rides with it,
    // deliberately: a build that does not write this field leaves it 0, and 0
    // is the honest answer for such a build in both directions.
    std::atomic<u32> takesOrphanedFinish;

    // --- the arrangement (docs/ARRANGEMENT.md §9.5, §9.6) -------------------
    //
    // arrBuiltFreed is not decoration and not a diagnostic: it is the ONLY
    // thing that makes the ordering between the two retirement layers
    // observable from outside this process. The daemon bumps it at the instant
    // it frees a built block and *then* queues that block's pool echoes, so a
    // client that pops an EvBlockRetired for one of those refs and then reads
    // this counter is guaranteed by the ring's release/acquire edge to see the
    // free already counted. A test can therefore assert the ordering rather
    // than trust it. See daemon_test §16d.
    std::atomic<u64> arrangementsApplied;  // SetArrangement/SetTrackAutos forwarded
    std::atomic<u64> arrangementsRejected; // refused with RejectBadArrangement
    std::atomic<u64> arrBuiltFreed;        // layer 1: built blocks freed on the proof
    std::atomic<u64> arrRefsEchoed;        // layer 2: pool blocks echoed after a free
    // Layer-2 echoes refused because a built block that still points into the
    // pool had not been freed yet. Structurally impossible; counted anyway,
    // because "impossible" is what this class of bug is called right up until
    // somebody reorders two lines. It must read 0 forever.
    std::atomic<u64> arrOrderViolations;

    // The journal's own accounting. journalDropped is the DAEMON's hop --
    // entries popped from the engine that would not fit the client's ring --
    // and it is published separately from the engine's (SharedState::
    // journalDropped) because there are two hops and a take has to be refusable
    // on either (§5.4, §9.6).
    std::atomic<u64> journalForwarded;
    std::atomic<u64> journalDropped;

    char driverName[32];             // "null", "JACK", "ALSA"

    // --- the sample-pool handshake (docs/PROCESS-SPLIT.md §3.5) ------------
    //
    // The only fields in this region the *client* writes. That inversion is
    // the pool's ownership inversion showing through: the GUI creates the pool
    // and therefore the GUI is the one with something to announce. Without a
    // socket to pass an fd over (§3.2) the announcement is a name plus an
    // epoch, published here.
    //
    // Ordering: the client fills poolName and poolBytes, then stores poolEpoch
    // with release. The daemon loads poolEpoch with acquire and only then
    // reads the name, so it can never map a half-written string. The daemon
    // answers in poolAttachedEpoch.
    // 96 bytes, matching ShmRegion's own name limit: a truncated region name
    // would be a name the daemon could not open, or worse, could.
    char poolName[96];                  // client -> daemon, e.g. /nxtakt-pool-foo
    std::atomic<u64> poolBytes;         // client: payload bytes of that region
    std::atomic<u64> poolEpoch;         // client: +1 per published pool, 0 = none
    std::atomic<u64> poolAttachedEpoch; // daemon: the epoch it has mapped, 0 = none
    std::atomic<u64> poolAttachFailures;// daemon: attaches that did not work

    // --- the plugin catalog (v6) --------------------------------------------
    //
    // Taken out of the reserved words rather than appended, so ControlHeader
    // keeps its size and every section offset above kCatalog stays where it
    // was. `scanPlugins` already says how many the scan FOUND; these two say
    // how many made it into the table and how many did not fit. They are
    // different numbers exactly when a machine has more than kMaxCatalog
    // plugins, and a browser that drew the first without the second would be
    // silently short.
    //
    // Written before scanState goes to ScanDone, with that release store as
    // the publication edge for the whole table.
    std::atomic<u32> catalogCount;      // rows published, <= kMaxCatalog
    std::atomic<u32> catalogTruncated;  // plugins beyond kMaxCatalog, 0 normally

    // --- rack contents (v7) -------------------------------------------------
    //
    // CmdSetRackState commands the daemon accepted and applied. A u32 out of the
    // reserved words rather than a u64 appended, for the reason catalogCount is
    // one: taking from `reserved` keeps ControlHeader's size and therefore every
    // section offset below it. Counting rack EDITS and not blocks, 2^32 of them
    // is not a number a session reaches.
    //
    // Refusals are not counted separately — they are already devicesFailed, and
    // a second counter for the same event is a second thing to keep true.
    std::atomic<u32> rackStatesApplied;

    // --- generic device state (v10) -----------------------------------------
    //
    // CmdSetDeviceState commands the daemon accepted and applied. Out of the
    // reserved words for the reason rackStatesApplied is: ControlHeader keeps
    // its size, so every section offset below it stays where it was and the
    // version plus the layout hash are what make two builds refuse each other.
    //
    // Refusals are already devicesFailed and are deliberately not counted twice.
    std::atomic<u32> deviceStatesApplied;

    // Cmd::SetSignatures commands accepted and handed to the engine. It reads 0
    // on every build before v8 AND on a set that never publishes a map, which is
    // why nothing asserts it equals a number on its own: the discriminating
    // check is that it MOVED across a publication.
    std::atomic<u32> signaturesApplied;

    // --- recording (v9) -----------------------------------------------------
    //
    // Out of the reserved words, so ControlHeader's size does not move for
    // these — `takeDir` below is what moves it, once, and these ride along.
    //
    // Every one of the four is a state a UI has to be able to say out loud:
    //   started    takes the daemon accepted and armed
    //   committed  takes whose file was written and announced
    //   failed     starts refused, buffers not allocated, files not written
    //   reclaimed  takes dropped because the client that owned them went away.
    //              This is the GUI-crash arm of the crash matrix, counted rather
    //              than inferred: a daemon that leaked instead of reclaiming and
    //              one that reclaimed are otherwise indistinguishable from
    //              outside until /dev/shm or RAM runs out.
    std::atomic<u32> takesStarted;
    std::atomic<u32> takesCommitted;
    std::atomic<u32> takesFailed;
    std::atomic<u32> takesReclaimed;

    // WHERE THE DAEMON WRITES TAKES, absolute, NUL-terminated, or empty when it
    // could not make a directory at all (in which case every take start is
    // refused with RejectTakeIo — fail closed, never "record into nowhere").
    //
    // Published by the daemon at init(), before publishReady(), and never
    // rewritten: a client that has read it once may keep it. The client must
    // compose take paths from THIS and not from its own $XDG_RUNTIME_DIR — see
    // take.h. 160 bytes covers a runtime dir, "/nxtakt/takes/", and a session
    // name; a longer one is truncated here and the daemon logs it, because a
    // truncated path is a path that names the wrong directory rather than one
    // that fails to open.
    char takeDir[160];
    u32  reserved[3];

    // Creator only, before publishReady(). `takes` is the directory the daemon
    // will write takes into, or null/empty if it could not make one — which is a
    // legal state and a fully refusing one, not a silent fallback.
    void init(i32 pid, bool nullDriver, const char* driver, const char* takes = nullptr) {
        protocolVersion = kProtocolVersion;
        flags           = 0;
        daemonPid       = pid;
        driverIsNull    = nullDriver ? 1u : 0u;
        heartbeat.store(0, std::memory_order_relaxed);
        startedNs.store(monotonicNs(), std::memory_order_relaxed);
        shutdown.store(0, std::memory_order_relaxed);
        reserved0.store(0, std::memory_order_relaxed);
        commandsApplied.store(0, std::memory_order_relaxed);
        commandsRejected.store(0, std::memory_order_relaxed);
        commandsDeferred.store(0, std::memory_order_relaxed);
        midiApplied.store(0, std::memory_order_relaxed);
        eventsForwarded.store(0, std::memory_order_relaxed);
        eventsDropped.store(0, std::memory_order_relaxed);
        clipsApplied.store(0, std::memory_order_relaxed);
        blocksRetired.store(0, std::memory_order_relaxed);
        devicesAdded.store(0, std::memory_order_relaxed);
        devicesRemoved.store(0, std::memory_order_relaxed);
        devicesFailed.store(0, std::memory_order_relaxed);
        devicesLive.store(0, std::memory_order_relaxed);
        paramWrites.store(0, std::memory_order_relaxed);
        chainsPublished.store(0, std::memory_order_relaxed);
        chainsRetired.store(0, std::memory_order_relaxed);
        scanState.store(ScanIdle, std::memory_order_relaxed);
        scanPlugins.store(0, std::memory_order_relaxed);
        catalogCount.store(0, std::memory_order_relaxed);
        catalogTruncated.store(0, std::memory_order_relaxed);
        rackStatesApplied.store(0, std::memory_order_relaxed);
        deviceStatesApplied.store(0, std::memory_order_relaxed);
        signaturesApplied.store(0, std::memory_order_relaxed);
        takesStarted.store(0, std::memory_order_relaxed);
        takesCommitted.store(0, std::memory_order_relaxed);
        takesFailed.store(0, std::memory_order_relaxed);
        takesReclaimed.store(0, std::memory_order_relaxed);
        std::memset(takeDir, 0, sizeof takeDir);
        if (takes && *takes) std::snprintf(takeDir, sizeof takeDir, "%s", takes);
        engineDrains.store(0, std::memory_order_relaxed);
        drainsExact.store(0, std::memory_order_relaxed);
        takesOrphanedFinish.store(0, std::memory_order_relaxed);
        arrangementsApplied.store(0, std::memory_order_relaxed);
        arrangementsRejected.store(0, std::memory_order_relaxed);
        arrBuiltFreed.store(0, std::memory_order_relaxed);
        arrRefsEchoed.store(0, std::memory_order_relaxed);
        arrOrderViolations.store(0, std::memory_order_relaxed);
        journalForwarded.store(0, std::memory_order_relaxed);
        journalDropped.store(0, std::memory_order_relaxed);
        std::memset(driverName, 0, sizeof driverName);
        std::snprintf(driverName, sizeof driverName, "%s", driver ? driver : "?");
        std::memset(poolName, 0, sizeof poolName);
        poolBytes.store(0, std::memory_order_relaxed);
        poolEpoch.store(0, std::memory_order_relaxed);
        poolAttachedEpoch.store(0, std::memory_order_relaxed);
        poolAttachFailures.store(0, std::memory_order_relaxed);
        for (u32& r : reserved) r = 0;
    }
};

// ---------------------------------------------------------------------------
// Region layout
// ---------------------------------------------------------------------------
//
// Capacities: 4096 slots each way, up from lat::Ring's 1024, because a process
// boundary makes bursts worse rather than better — the GUI can be descheduled
// for a whole frame while a scene launch queues 32 commands, and the daemon
// still only drains at its pump tick. 4096 * 32 B is 128 KiB a side; the whole
// control region is well under a megabyte.

using CommandRing = ShmSpscRing<WireCommand, 4096>;
using EventRing   = ShmSpscRing<WireEvent, 4096>;
using MidiRing    = ShmSpscRing<WireMidi, 1024>;

// The record journal's own ring, daemon -> client (§9.6). 4096 slots, matching
// Engine's own Ring<ArrJournal, 4096> exactly, so the two hops have the same
// capacity and neither is the narrow one by accident: 24 B a slot is 96 KiB,
// which is under one of the message rings.
//
// NOT the event ring. Mixing them lets a burst of journal entries evict events,
// and events carry EvClipAck — a lost ack wedges a clip-table cell for the rest
// of the session. NOT the pool either: the journal is fixed-size, high-rate and
// continuous, which is what a ring is for and what an
// allocate-write-publish-retire lifecycle emphatically is not.
using JournalRing = ShmSpscRing<WireJournal, 4096>;

// The clip table. 32 x 32 x 176 B is 176 KiB — the same order as one ring, and
// preallocated for the same reason everything else here is: the engine cannot
// wait for an allocation and a republish must not need one either.
inline constexpr size_t kClipTableBytes = sizeof(WireClip) * kMaxTracks * kMaxScenes;

// 320 x 4.3 KiB of metadata and 320 x 272 B of values: 1.4 MiB, which on tmpfs
// costs one page per table until something is written into it. Preallocated for
// the same reason the clip table is — a device may not wait for an allocation,
// and neither may a republish.
inline constexpr size_t kDeviceTableBytes = sizeof(WireDeviceInfo)   * kMaxDevices;
inline constexpr size_t kParamTableBytes  = sizeof(WireDeviceParams) * kMaxDevices;

// 1 MiB, and — like every table here — one resident page until a scan fills it.
// It is the largest single section in the region and it is still a fifth of
// what the pool's own header costs, which is the sizing argument §3 option B
// makes: a fixed budget for the catalog is affordable precisely because the
// catalog is small compared with audio.
inline constexpr size_t kCatalogTableBytes = sizeof(WirePluginDesc) * kMaxCatalog;

namespace control {

inline constexpr size_t kHeader  = 0;
inline constexpr size_t kState   = alignUp(kHeader  + sizeof(ControlHeader),  kCacheLine);
inline constexpr size_t kCmds    = alignUp(kState   + sizeof(SharedState),    kCacheLine);
inline constexpr size_t kEvts    = alignUp(kCmds    + CommandRing::bytes(),   kCacheLine);
inline constexpr size_t kMidi    = alignUp(kEvts    + EventRing::bytes(),     kCacheLine);
inline constexpr size_t kClips   = alignUp(kMidi    + MidiRing::bytes(),      kCacheLine);
inline constexpr size_t kDevices = alignUp(kClips   + kClipTableBytes,        kCacheLine);
inline constexpr size_t kParams  = alignUp(kDevices + kDeviceTableBytes,      kCacheLine);
// The ninth section (§9.6). Appended rather than inserted: every offset above
// it is unchanged, so the only reason a v3 binary and a v5 binary refuse each
// other is the hash and the version — which is the mechanism, not an accident
// of layout churn.
inline constexpr size_t kJournal = alignUp(kParams  + kParamTableBytes,       kCacheLine);
// The tenth section (v6). Appended for the same reason the ninth was: every
// offset above it is unchanged, so nothing that already worked can be moved out
// from under a peer by this — the only reason a v5 binary and a v6 binary
// refuse each other is the hash and the version, which is the mechanism rather
// than an accident of layout churn.
inline constexpr size_t kCatalog = alignUp(kJournal + JournalRing::bytes(),   kCacheLine);
inline constexpr size_t kBytes   = kCatalog + kCatalogTableBytes;

// Everything that could move an offset out from under a peer goes into the
// hash: the total size, every section offset, every message size, the ring
// capacities and the protocol version.
inline constexpr u32 kHash =
    hashMix(hashMix(hashMix(hashMix(hashMix(hashMix(hashMix(hashMix(hashMix(hashMix(hashMix(hashMix(hashMix(
        fnv1a("nxtakt.control.v6"),   // reseeded per protocol: see pool::kHash
        (u64)kBytes), (u64)kState), (u64)kCmds), (u64)kEvts), (u64)kMidi), (u64)kClips),
        (u64)kDevices), (u64)kParams), (u64)kJournal), (u64)kCatalog),
        (u64)(sizeof(WireCommand) * 65536 + sizeof(WireEvent) * 256 + sizeof(WireMidi))),
        (u64)(sizeof(WireJournal) * 65536ull + JournalRing::capacity()) ^
        (u64)(sizeof(WireArrHeader) * 65536ull + sizeof(WireArrItem))),
        (u64)(CommandRing::capacity() * 65536ull + EventRing::capacity()) ^
        (u64)(kProtocolVersion * 65536u + (u32)sizeof(WireClip)) ^
        (u64)(sizeof(WireDeviceInfo) * 65536ull + sizeof(WireDeviceParams)) ^
        (u64)(sizeof(WirePluginDesc) * 65536ull + kMaxCatalog));

} // namespace control

// The default region name for a session. POSIX shm names are one path
// component, so the session id is pasted in rather than nested.
inline void controlRegionName(const char* session, char* out, size_t cap) {
    std::snprintf(out, cap, "/nxtakt-engine-%s", (session && *session) ? session : "default");
}
// The pool's name lives in pool.h (poolRegionName) because the pool is not part
// of the control protocol: the GUI could hand the daemon any name it likes
// through ControlHeader::poolName, and the default is only a default.

// ---------------------------------------------------------------------------
// ControlMap — the five pointers, resolved once
// ---------------------------------------------------------------------------
//
// at<T>() is bounds- and alignment-checked, so a layout mistake surfaces here,
// at startup, on both sides. Nothing below ever recomputes an offset.
struct ControlMap {
    ControlHeader*    hdr     = nullptr;
    SharedState*      state   = nullptr;
    CommandRing*      cmds    = nullptr;
    EventRing*        evts    = nullptr;
    MidiRing*         midi    = nullptr;
    WireClip*         clips   = nullptr;   // [kMaxTracks * kMaxScenes], row-major
    WireDeviceInfo*   devices = nullptr;   // [kMaxDevices], daemon -> client
    WireDeviceParams* params  = nullptr;   // [kMaxDevices], client -> daemon
    JournalRing*      journal = nullptr;   // daemon pump -> client, §9.6
    WirePluginDesc*   catalog = nullptr;   // [kMaxCatalog], daemon -> client, v6

    bool valid() const {
        return hdr && state && cmds && evts && midi && clips && devices && params &&
               journal && catalog;
    }

    WirePluginDesc* catalogRow(u32 i) {
        return (catalog && i < kMaxCatalog) ? catalog + i : nullptr;
    }
    const WirePluginDesc* catalogRow(u32 i) const {
        return const_cast<ControlMap*>(this)->catalogRow(i);
    }

    WireDeviceInfo* device(u32 id) {
        return (devices && id < kMaxDevices) ? devices + id : nullptr;
    }
    const WireDeviceInfo* device(u32 id) const {
        return const_cast<ControlMap*>(this)->device(id);
    }
    WireDeviceParams* param(u32 id) {
        return (params && id < kMaxDevices) ? params + id : nullptr;
    }
    const WireDeviceParams* param(u32 id) const {
        return const_cast<ControlMap*>(this)->param(id);
    }

    // The clip table is one flat array with an accessor rather than a 2-D
    // pointer, so the region layout has one offset in it and the index
    // arithmetic lives in exactly one place on both sides.
    WireClip* clip(int track, int slot) {
        if (!clips || track < 0 || track >= kMaxTracks || slot < 0 || slot >= kMaxScenes)
            return nullptr;
        return clips + (size_t)track * kMaxScenes + (size_t)slot;
    }
    const WireClip* clip(int track, int slot) const {
        return const_cast<ControlMap*>(this)->clip(track, slot);
    }

    // Creator: adopt the memory and reset every ring. Must run before
    // ShmRegion::publishReady().
    bool create(ShmRegion& r) {
        hdr   = r.at<ControlHeader>(control::kHeader);
        state = r.at<SharedState>(control::kState);
        cmds  = CommandRing::createAt(r, control::kCmds);
        evts  = EventRing::createAt(r, control::kEvts);
        midi  = MidiRing::createAt(r, control::kMidi);
        clips = r.at<WireClip>(control::kClips);
        // The table needs the last WireClip to fit too, which at<T>() cannot
        // know: it checks one T, and this is an array.
        if (clips && !r.at<WireClip>(control::kClips + kClipTableBytes - sizeof(WireClip)))
            clips = nullptr;
        journal = JournalRing::createAt(r, control::kJournal);
        mapTables(r);
        return valid();
    }
    // Attacher: adopt the memory, touching nothing.
    bool attach(ShmRegion& r) {
        hdr   = r.at<ControlHeader>(control::kHeader);
        state = r.at<SharedState>(control::kState);
        cmds  = CommandRing::attachAt(r, control::kCmds);
        evts  = EventRing::attachAt(r, control::kEvts);
        midi  = MidiRing::attachAt(r, control::kMidi);
        clips = r.at<WireClip>(control::kClips);
        if (clips && !r.at<WireClip>(control::kClips + kClipTableBytes - sizeof(WireClip)))
            clips = nullptr;
        journal = JournalRing::attachAt(r, control::kJournal);
        mapTables(r);
        return valid();
    }
    void clear() { *this = ControlMap{}; }

private:
    // Same last-element check as the clip table, for the same reason: at<T>()
    // proves one T fits, and these are arrays of hundreds.
    void mapTables(ShmRegion& r) {
        devices = r.at<WireDeviceInfo>(control::kDevices);
        if (devices && !r.at<WireDeviceInfo>(control::kDevices + kDeviceTableBytes -
                                             sizeof(WireDeviceInfo)))
            devices = nullptr;
        params = r.at<WireDeviceParams>(control::kParams);
        if (params && !r.at<WireDeviceParams>(control::kParams + kParamTableBytes -
                                              sizeof(WireDeviceParams)))
            params = nullptr;
        catalog = r.at<WirePluginDesc>(control::kCatalog);
        if (catalog && !r.at<WirePluginDesc>(control::kCatalog + kCatalogTableBytes -
                                             sizeof(WirePluginDesc)))
            catalog = nullptr;
    }
};

} // namespace lat::ipc
