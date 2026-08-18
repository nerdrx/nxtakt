// NxTakt IPC — the sample pool: the session region the GUI owns and the
// engine reads.
//
// This is the header that makes `RtClip::data` cross a process boundary
// (docs/PROCESS-SPLIT.md §2.4, §3.5). A clip's audio no longer travels as a
// GUI-heap pointer; it lives in one shared region and travels as a `u64` byte
// offset. The daemon adds that offset to its own mapping of the same region and
// hands the *resulting pointer* to `Engine::pushCommand`. Engine never learns
// that anything changed — which is the whole point of putting the translation
// in the daemon: `src/audio` stays frozen and there is exactly one place in the
// tree where an untrusted number becomes a pointer the audio thread will
// dereference, so exactly one place has to get the bounds check right.
//
// OWNERSHIP IS DELIBERATELY ASYMMETRIC
// ------------------------------------
// The control region is created by `nxtaktd` and dies with it. The pool is the
// other way round: **the GUI creates it, the GUI unlinks it, and it outlives
// engine restarts.** That is not a stylistic choice, it is the feature — §4.4's
// "republish is not a reload" only works if killing the engine leaves the
// decoded audio where it is, so that the replacement daemon attaches to the
// same region and the same offsets still name the same samples. Concretely:
//
//   creator   the GUI (or a test, or a future headless controller)
//   name      /nxtakt-pool-<session>
//   writer    the GUI, and only the GUI: allocation, free-list surgery, sample
//             data, block metadata. Single-writer is what keeps this lock-free
//             without any cleverness.
//   reader    the daemon, mapped PROT_READ (ShmRegion::attach(..., readOnly)).
//             A daemon bug cannot corrupt the allocator, because the pages are
//             not writable in that address space.
//
// SIZE, ftruncate AND SIGBUS
// --------------------------
// The region is sized once, at create, and never grows or shrinks. §3.5 sketches
// an 8 GiB PROT_NONE reservation grown with MAP_FIXED plus a Cmd::PoolGrow
// handshake; this ships the simpler half of that idea, which gets the same
// property for free: /dev/shm is tmpfs, so `ftruncate` to 256 MiB creates a
// *sparse* file and costs no memory at all. Pages are committed on first write,
// and the only writer is the GUI. So there is no growth event to sequence, no
// window in which the engine holds an offset past the committed end, and no
// mmap on any thread but the one that created the region.
//
// SIGBUS has exactly two causes here and both are the writer's:
//
//   * writing past the end of the object — impossible, the allocator bounds
//     every block against `arenaBytes` and the daemon re-checks;
//   * writing a page tmpfs cannot back (i.e. /dev/shm is full). That fault
//     lands on the GUI thread that is decoding a file, never on the audio
//     thread, because a block is fully written before its offset is published.
//     Publication is the release store on `PoolBlock::magic`, so "the daemon
//     can see this offset" and "every page behind it is committed" are the same
//     event.
//
// Shrinking the object under a live mapping *would* fault the reader, which is
// what `F_SEAL_SHRINK` is for. `ShmRegion::create(..., seal=true)` asks; a
// plain `shm_open` object is not sealable on Linux (sealing is a memfd
// property) so the answer is normally no, and `sealed()` says so rather than
// pretending. Nothing else in the process can shrink it either: after create
// the GUI closes its fd and no one re-opens the object O_RDWR — the daemon
// opens O_RDONLY. That is a weaker guarantee than a seal and it is documented
// as such; the memfd + SCM_RIGHTS upgrade lands with the socket (§3.2).
//
// LAYOUT
// ------
//   payload+0        PoolHeader        magic, version, bump, free-list head
//   payload+4096     arena             [PoolBlock][data][PoolBlock][data]...
//
// §3.5 sketches a separate `BlockDesc[NBlocks]` table. This puts the descriptor
// *inline*, immediately before its data, for one reason that matters more than
// the symmetry: the daemon can then validate an offset knowing nothing but the
// offset. `poolValidate(ref)` reads the header at `ref - sizeof(PoolBlock)` and
// checks a magic that is mixed with `ref` itself, so a plausible-looking but
// wrong offset — an off-by-one block, a stale offset from a previous session,
// a number a corrupted GUI invented — fails on its own terms rather than
// indexing a side table with an index that is equally untrusted. The reattach
// key §4.3 wants lives in the same header.
//
// Header-only, like the rest of src/ipc.
#pragma once
#include "../audio/engine.h"   // RtNote — the layout WireNote must mirror
#include "shm.h"

namespace lat::ipc {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

inline constexpr u64 kPoolMagic      = 0x4C54435F504F4F4Cull;  // "LTC_POOL"
inline constexpr u64 kPoolBlockMagic = 0x4C54435F424C4B31ull;  // "LTC_BLK1"

// Bump on any change to PoolHeader/PoolBlock/WireNote layout or meaning. It is
// folded into the pool region's layout hash, so a mismatched GUI and daemon
// refuse to share a pool instead of misreading one.
//
//   v2 — PoolKindString (phase 3). No layout moved; the *meaning* of `kind`
//        grew, and a v1 daemon handed a v2 string block would refuse it as
//        "not sample data" rather than misreading it. Bumping is still the
//        right call: the two builds no longer agree about what a pool can
//        contain, and that is exactly what this number is for.
//   v3 — RESERVED for AUTOMATION.md §8.2 (PoolKindAutomation, clip envelopes
//        across the boundary). That design is written and its numbers are
//        spelled out in the doc; it has not shipped in this tree. The number is
//        burned rather than reused, because "pool v3" already means something
//        specific to anyone reading that document, and two meanings for one
//        version is exactly the confusion a version exists to prevent.
//   v4 — the arrangement (ARRANGEMENT.md §9.2): PoolKindArrangement and
//        PoolKindTrackAutos. Again no layout moved and again the *meaning* of
//        `kind` grew: a v2 daemon handed an arrangement blob would refuse it as
//        "not sample data", which is correct behaviour and still not a version
//        two builds should be able to disagree about silently.
//   v5 — rack contents (docs/RACKS.md, GUI-ON-DAEMON.md §12.3): PoolKindRackState.
//        A third time the same shape — no layout moved, `kind` grew a meaning —
//        and a third time worth a number, because a v4 daemon handed a rack-state
//        blob refuses it as "not a string" and a rack would then silently stay
//        empty, which is exactly the failure this feature exists to end.
//   v6 — the signature map (PoolKindSignatures). Same shape of change again and
//        the same argument for a number: a v5 daemon handed a signature blob
//        refuses it, and a set that plays in 4/4 while its ruler draws 7/8 is
//        precisely the silent disagreement this whole layer exists to prevent.
//   v7 — GENERIC DEVICE STATE (PoolKindDeviceState, GUI-ON-DAEMON.md §15). The
//        first pool kind that is not one payload but a HEADER plus one: a
//        WireDeviceState followed by the device's own stateString() bytes, and
//        the header may name a second block (PoolKindSamples) holding a decoded
//        sample. Until it existed `nxtakt:sampler` was STRUCTURALLY SILENT in
//        daemon mode — the state string carrying the path did not cross, and
//        nxtaktd deliberately links no decoder, so even if it had the daemon
//        could not have opened the file. Same argument for a number as every
//        entry above it: a v6 daemon handed one of these refuses it as "not a
//        string", and a sampler that is drawn full and sounds empty is exactly
//        the silent disagreement this layer exists to prevent.
inline constexpr u32 kPoolVersion = 7;

// Every block — header and data — starts on a 64-byte line. Blocks are large
// (a stereo bar of audio is hundreds of kilobytes) so the padding is noise,
// and in exchange every `const f32*` the engine receives is cache-line and
// SIMD aligned, and `ref % 64 == 0` becomes a free first sanity check on an
// untrusted offset.
inline constexpr size_t kPoolAlign = 64;

// The arena starts here, payload-relative. Fixed rather than sizeof-derived so
// that adding a field to PoolHeader does not silently move every block; such a
// change goes through kPoolVersion instead. 4 KiB leaves the header room to
// grow for the whole life of the format.
inline constexpr size_t kPoolArenaOffset = 4096;

// 256 MiB of *address space and file length*, not of memory: tmpfs allocates on
// first touch, so an empty pool costs one page. Roughly 45 minutes of stereo
// float at 48 kHz — beyond any live set — and the cost of guessing high is
// nothing, which is exactly why the growth handshake is not worth its risk yet.
inline constexpr size_t kDefaultPoolBytes = 256ull << 20;

// What a block holds. Kept explicit rather than inferred from `channels`
// because it is one of the things the daemon checks: a notes offset arriving
// where sample data was expected must be a rejection, not an array of f32s
// reinterpreted from RtNote.
enum : u32 {
    PoolKindNone    = 0,
    PoolKindSamples = 1,   // interleaved f32, `frames` * `channels` of them
    PoolKindNotes   = 2,   // WireNote[], `frames` of them

    // A NUL-terminated byte string: a plugin URI, a preset name, a path.
    // Phase 3's answer to "the rings cannot carry a string" (§3.2) without a
    // socket to carry one over: the client allocates a blob, writes the bytes,
    // and the 32-byte command carries the offset. `frames` is the byte length
    // including the terminator. The daemon copies it out on the pump thread
    // and echoes the offset straight back as EvBlockRetired — a string never
    // reaches the audio thread, so its retirement needs no proof at all. See
    // docs/PROCESS-SPLIT.md §11.2.
    PoolKindString  = 3,

    // RESERVED, not implemented here. AUTOMATION.md §8.2 names this number for
    // clip envelopes crossing the boundary. It is declared so that a later wave
    // finds its own number free rather than discovering that 8g took it.
    PoolKindAutomation = 4,

    // One track's arrangement lane (docs/ARRANGEMENT.md §9.2), or -- for the
    // cell addressed as track -1 -- the transport's loop brace.
    //
    //   [WireArrHeader][WireArrItem[itemCount]][WireClip[clipCount]]
    //
    // The notes are NOT in the blob: each WireClip names its own notesRef into
    // the pool exactly as a session clip's does, so the existing WireNote
    // reinterpretation and the existing per-block retirement both keep working
    // unchanged.
    //
    // Unlike every other kind here this one is *translated, not reinterpreted*:
    // RtClip holds five pointers, and a pointer in a client-writable region is a
    // pointer the client chose. See Daemon::translateArrangement.
    PoolKindArrangement = 5,

    // One track's arrangement automation (§6.2), the RtAutoSetN payload:
    //
    //   [WireAutoSetHeader][WireAutoLane[laneCount]][WireAutoPoint[pointCount]]
    //
    // Distinct from PoolKindAutomation because the containers differ --
    // RtAutoSet has sixteen lanes by value, RtAutoSetN has a variable count
    // behind a pointer -- and a blob that is handed to the wrong builder is
    // exactly what the `kind` field exists to refuse.
    PoolKindTrackAutos = 6,

    // One rack's complete contents, as `rackStateToString()` writes them:
    // NUL-terminated printable ASCII, no whitespace and no newline
    // (docs/RACKS.md, "Persistence"). `frames` is the byte length including the
    // terminator, exactly as PoolKindString's is.
    //
    // A SEPARATE KIND FROM PoolKindString, AND THAT IS THE POINT. The two are
    // byte-identical in shape and could not be more different in consequence: a
    // string blob names a plugin the daemon is about to load, a rack-state blob
    // is a recipe the daemon will instantiate an entire sub-chain from. Handing
    // one where the other was expected must be a refusal rather than a plausible
    // parse, and `kind` is the field that exists to make it one. They also have
    // different length budgets — see kMaxRackState — which a shared kind could
    // not express.
    PoolKindRackState = 7,

    // The whole time-signature map, as a flat WireSig[], `frames` of them.
    //
    // The simplest pooled payload there is — one array, no nested references,
    // nothing else in the pool named by it — which is exactly why it took the
    // longest to cross: Cmd::SetSignatures carries a `const RtSig*` and there
    // was no pool kind for it, so the daemon answered RejectUnknownCommand and
    // PLAYED EVERY SET IN 4/4 while the ruler drew 7/8.
    //
    // TRANSLATED, NOT REINTERPRETED, and this one is worth being explicit about
    // because WireSig mirrors RtSig field for field and a cast would compile.
    // The engine holds the map for as long as it is the map, on the audio
    // thread, and the pool is CLIENT-WRITABLE: a client that rewrote its own
    // blob in place would be rewriting the array the audio thread is bisecting.
    // A WireNote blob is safe to reinterpret because it is bounds-checked once
    // and then read; a signature map is a live structure. So the daemon copies
    // it into its own heap and validates it there. See Daemon::doSetSignatures.
    PoolKindSignatures = 8,

    // ONE DEVICE'S GENERIC STATE (GUI-ON-DAEMON.md §15):
    //
    //   [WireDeviceState][char text[textBytes]]
    //
    // `text` is `PluginInstance::stateString()`'s output, NUL-terminated, byte
    // for byte what a saved set would carry — see WireDeviceState for why the
    // wire-only fields live in the header and not in the string.
    //
    // A SEPARATE KIND FROM PoolKindRackState, and for once the two are NOT the
    // same shape: a rack state is a bare string, this is a header plus a string,
    // and a reader that took one for the other would read a length prefix as
    // text or text as a length. They are also reached differently — a rack's
    // contents come off RackControl::state(), a device's state off the generic
    // stateString() virtual — which is the whole reason both channels exist.
    PoolKindDeviceState = 9,
};

// The longest string the pool will carry. Not a buffer size — a policy: the
// daemon copies a blob into a fixed stack buffer, so an unbounded blob would
// be an unbounded copy driven by a peer. Every string the protocol has (a
// plugin URI is the longest) fits several times over.
inline constexpr u64 kMaxPoolString = 1024;

// The longest rack state the pool will carry, and it needs its own number
// because kMaxPoolString's reasoning does not reach it. A URI is bounded by what
// a plugin author typed; a rack state is bounded by the FORMAT — up to eight
// devices, each with its URI, its bypass flag and one `id:value` pair per
// control, nested up to kRackMaxDepth with one escaping pass per level. Eight
// devices of sixty controls is already past 1024 and the depth cap multiplies
// it. 64 KiB clears the worst case the format admits by a wide margin and is
// still a bounded copy on the daemon's pump thread, which is the property
// kMaxPoolString was really protecting.
//
// The daemon copies a rack state into a std::string and not a stack buffer for
// the same reason: 64 KiB of stack on a thread that also loads plugins is not a
// trade worth making, and the pump is not realtime.
inline constexpr u64 kMaxRackState = 65536;

// The longest generic device state the pool will carry, TEXT ONLY — the
// WireDeviceState header in front of it is not counted, because this number is
// a bound on a peer-driven copy and the header is a fixed 48 bytes the daemon
// reads before it trusts anything.
//
// The same 64 KiB as a rack's, and deliberately the same number rather than a
// smaller one derived from today's only user: `nxtakt:sampler` writes a
// percent-escaped path and nothing else, so its worst case is about 12 KiB —
// but this channel is GENERIC by construction, the next device to override
// stateString() will not ask permission, and a bound that has to be revisited
// per device is a bound that will be discovered by a truncation. 64 KiB is
// still a bounded copy on the daemon's pump thread, which is the property
// kMaxPoolString was really protecting.
inline constexpr u64 kMaxDeviceState = 65536;

// Block lifecycle. See "the free-after-confirm rule" below — these four states
// *are* the rule, written down.
enum : u32 {
    BlockFree      = 0,  // on the free list; may be handed out again
    BlockQuiescent = 1,  // allocated, referenced by no clip cell: safe to free
    BlockLive      = 2,  // published to the engine through a clip cell
    BlockRetiring  = 3,  // displaced, awaiting the daemon's offset echo
};

inline const char* poolKindName(u32 k) {
    switch (k) {
        case PoolKindSamples:     return "samples";
        case PoolKindNotes:       return "notes";
        case PoolKindString:      return "string";
        case PoolKindAutomation:  return "automation";
        case PoolKindArrangement: return "arrangement";
        case PoolKindTrackAutos:  return "track-autos";
        case PoolKindRackState:   return "rack-state";
        case PoolKindSignatures:  return "signatures";
        case PoolKindDeviceState: return "device-state";
        default:                  return "none";
    }
}

inline const char* poolStateName(u32 s) {
    switch (s) {
        case BlockFree:      return "free";
        case BlockQuiescent: return "quiescent";
        case BlockLive:      return "live";
        case BlockRetiring:  return "retiring";
        default:             return "?";
    }
}

// ---------------------------------------------------------------------------
// WireNote
// ---------------------------------------------------------------------------
//
// The wire twin of lat::RtNote, mirroring it field for field so the daemon can
// hand `(const RtNote*)(poolBase + notesRef)` straight to the engine with no
// copy and no translation pass on a 10 000-note clip. The asserts below are
// what make that cast honest: if RtNote ever changes, this stops compiling
// instead of quietly reinterpreting a piano roll.
struct WireNote {
    f64 beat;
    f64 len;
    u8  pitch, vel;
    u8  pad[6];
};

static_assert(std::is_trivially_copyable_v<WireNote>);
static_assert(sizeof(WireNote) == sizeof(RtNote), "WireNote must mirror RtNote");
static_assert(alignof(WireNote) == alignof(RtNote));
static_assert(offsetof(WireNote, beat)  == offsetof(RtNote, beat));
static_assert(offsetof(WireNote, len)   == offsetof(RtNote, len));
static_assert(offsetof(WireNote, pitch) == offsetof(RtNote, pitch));
static_assert(offsetof(WireNote, vel)   == offsetof(RtNote, vel));

// ---------------------------------------------------------------------------
// WireSig
// ---------------------------------------------------------------------------
//
// The wire twin of lat::RtSig, asserted to mirror it field for field. Unlike
// WireNote the mirror does NOT license a cast — see PoolKindSignatures for why
// the daemon copies instead — it is here so that a copy is a memcpy and so that
// a change to RtSig cannot alter the protocol without the build noticing.
//
// `beat` is DERIVED (engine.h): the absolute beat the entry's bar begins on. It
// crosses rather than being recomputed because sigMapValid RE-DERIVES it and
// refuses a map whose beats do not follow from its own bar lengths — so sending
// it is what lets the far side check the sender's arithmetic instead of
// trusting it.
struct WireSig {
    i32 bar;
    i32 num, den;
    i32 pad;
    f64 beat;
};

static_assert(std::is_trivially_copyable_v<WireSig>);
static_assert(sizeof(WireSig) == sizeof(RtSig), "WireSig must mirror RtSig");
static_assert(alignof(WireSig) == alignof(RtSig));
static_assert(offsetof(WireSig, bar)  == offsetof(RtSig, bar));
static_assert(offsetof(WireSig, num)  == offsetof(RtSig, num));
static_assert(offsetof(WireSig, den)  == offsetof(RtSig, den));
static_assert(offsetof(WireSig, beat) == offsetof(RtSig, beat));

// ---------------------------------------------------------------------------
// WireDeviceState — the head of a PoolKindDeviceState blob
// ---------------------------------------------------------------------------
//
// THE TWO SPELLINGS, AND HOW THEY ARE KEPT APART. This is the whole design
// decision of §15 and it is one sentence: **`text` is the PERSISTED spelling,
// verbatim, and everything that is wire-only lives out here in the header.**
//
// The alternative — appending a wire-only record to the state string itself,
// which both the rack's and the sampler's `;`-separated, tag-per-record formats
// invite — was rejected. A state string is written into project files by
// `src/core/project.cpp` and read back by a device's own parser; a pool offset
// is meaningless five seconds later and catastrophic five days later, when it
// names whatever now lives at that offset in somebody else's pool. Keeping the
// transient fields in a binary header makes "could this be saved?" a question
// about which struct a field is in, rather than a question about whether every
// writer remembers to strip a record.
//
// So: the daemon hands `text` to `setStateString()` unmodified, and a set saved
// on either side of the wire produces the same bytes.
//
// THE SAMPLE. `audioRef` names a SECOND block — an ordinary PoolKindSamples
// one, written by exactly the code that writes a clip's audio — because the
// GUI decodes and the daemon deliberately does not (`nxtaktd` links no
// libsndfile; see the weak-decoder note in src/plugin/sampler.cpp). The shape
// is carried here as well as in the block's own header, and the daemon checks
// them against each other: the block header is in a CLIENT-WRITABLE region, so
// a frame count read from it is a number a peer chose, and computing the copy
// bound from a number the command also states is one more thing a corrupt
// writer has to get consistently wrong.
//
// A device with no sample sets audioRef = 0, which is every device but one.
struct WireDeviceState {
    u32 version;        // kDeviceStateVersion
    u32 textBytes;      // including the NUL; <= kMaxDeviceState
    u64 audioRef;       // 0 = none; otherwise a PoolKindSamples block
    i64 audioFrames;
    u32 audioChannels;  // 1 or 2
    u32 pad;
    f64 audioRate;
};

// Bumped when the MEANING of the fields above changes. Separate from
// kPoolVersion because a blob is also validated by a daemon that may be newer
// than the client in a development tree, and "I do not understand this state"
// deserves a refusal with a reason rather than a layout-hash mismatch at
// attach() — which is the right answer for a whole-region disagreement and the
// wrong one for a single command.
inline constexpr u32 kDeviceStateVersion = 1;

static_assert(std::is_trivially_copyable_v<WireDeviceState>);
static_assert(sizeof(WireDeviceState) == 40);

// ---------------------------------------------------------------------------
// PoolBlock — the inline descriptor, immediately before its data
// ---------------------------------------------------------------------------
//
// `magic` is mixed with the block's own data offset and is the *last* field
// written when a block is handed out. Both properties are load-bearing:
//
//   mixed  — a valid magic proves not just "an NxTakt block lives here" but
//            "an NxTakt block whose data starts at exactly the offset you
//            asked about". A stale offset from a previous allocation, or one
//            that lands mid-block, fails immediately instead of yielding a
//            self-consistent header describing somebody else's samples.
//   last   — publishing it with release ordering means a reader that sees the
//            magic sees every other field, and every page of the data behind
//            it, because they were all written first.
//
// Only `magic` and `state` are atomic. Everything else is written before the
// magic's release store and never touched again while a block is reachable, so
// the acquire on the magic already orders it; making them atomic too would buy
// nothing and cost the compiler its ability to fold the writes.
struct PoolBlock {
    std::atomic<u64> magic;      // kPoolBlockMagic ^ dataOffset, 0 while unbuilt
    u64 bytes;                   // usable data bytes, always a multiple of 64
    u64 next;                    // free list: data offset of the next free block
    u64 key;                     // GUI content key, hash(path,size,mtime); 0 = none
    i64 frames;                  // audio frames, or note count for PoolKindNotes
    f64 rate;                    // sample rate of the material, 0 = unknown
    u32 channels;
    u32 kind;                    // PoolKind*
    std::atomic<u32> state;      // Block*
    u32 refs;                    // GUI-side references (ClipModel handles)
    u32 live;                    // clip cells the GUI believes point here
    u32 pad0;
    u32 reserved[14];
};
static_assert(sizeof(PoolBlock) == 128, "block header must stay 128 B: it is part of every offset");
static_assert(sizeof(PoolBlock) % kPoolAlign == 0, "a 64-aligned header keeps data 64-aligned");

// ---------------------------------------------------------------------------
// PoolHeader
// ---------------------------------------------------------------------------
//
// The allocator's metadata lives *inside the region*, not on the GUI's heap,
// because §4.3's replacement GUI has to be able to adopt a pool it did not
// build — the blocks are still playing, so their extents, refcounts and content
// keys have to be discoverable from the region alone.
struct PoolHeader {
    u64 magic;                    // kPoolMagic
    u32 version;                  // kPoolVersion
    u32 flags;                    // bit 0: the object is F_SEAL_SHRINK'd
    u64 totalBytes;               // payload bytes of the region
    u64 arenaOffset;              // == kPoolArenaOffset, recorded for the reader
    u64 arenaBytes;

    std::atomic<u64> bump;        // next fresh block header, payload-relative
    std::atomic<u64> freeHead;    // data offset of the first free block, 0 = none
    std::atomic<u64> liveBlocks;  // diagnostics
    std::atomic<u64> bytesUsed;   // diagnostics: data bytes handed out
    std::atomic<u64> generation;  // +1 per alloc/free; churn indicator
    std::atomic<u64> epoch;       // matches ControlHeader::poolEpoch

    i32 creatorPid;
    u32 reserved[9];

    // Creator only, before publishReady().
    void init(size_t payloadBytes, u64 sessionEpoch, bool isSealed) {
        magic       = kPoolMagic;
        version     = kPoolVersion;
        flags       = isSealed ? 1u : 0u;
        totalBytes  = (u64)payloadBytes;
        arenaOffset = (u64)kPoolArenaOffset;
        arenaBytes  = payloadBytes > kPoolArenaOffset ? (u64)(payloadBytes - kPoolArenaOffset) : 0;
        bump.store((u64)kPoolArenaOffset, std::memory_order_relaxed);
        freeHead.store(0, std::memory_order_relaxed);
        liveBlocks.store(0, std::memory_order_relaxed);
        bytesUsed.store(0, std::memory_order_relaxed);
        generation.store(0, std::memory_order_relaxed);
        epoch.store(sessionEpoch, std::memory_order_relaxed);
        creatorPid = (i32)::getpid();
        for (u32& r : reserved) r = 0;
    }

    u64 arenaEnd() const { return arenaOffset + arenaBytes; }
};

// ---------------------------------------------------------------------------
// Region naming and layout hash
// ---------------------------------------------------------------------------

inline void poolRegionName(const char* session, char* out, size_t cap) {
    std::snprintf(out, cap, "/nxtakt-pool-%s", (session && *session) ? session : "default");
}

namespace pool {
// The seed string is part of the hash, so the rename changed kHash. That is
// deliberate and costs nothing: the hash only has to make two peers agree on
// a layout, both peers are built from this header, and a region never
// outlives the processes sharing it. The `.v1` suffix is the pool protocol
// version and is unchanged -- the wire format did not move, only its name.
inline constexpr u32 kHash =
    hashMix(hashMix(hashMix(hashMix(
        fnv1a("nxtakt.pool.v1"),
        (u64)sizeof(PoolHeader)), (u64)sizeof(PoolBlock)),
        (u64)(sizeof(WireNote) * 65536 + kPoolArenaOffset)),
        (u64)kPoolVersion);
} // namespace pool

// ---------------------------------------------------------------------------
// Validation — the one place an untrusted u64 becomes a pointer
// ---------------------------------------------------------------------------
//
// Everything the daemon knows about a `ref` it got over the wire, it learns
// here. The rule is absolute: **a bad offset must never become a pointer the
// engine dereferences.** So this is a total function over `u64` — every
// possible input, including 0, including 2^64-1, including an offset that lands
// one byte inside a valid block — either returns false with a reason or proves
// that `base + ref` is a readable, correctly-typed, still-allocated block with
// at least `needBytes` in it.
//
// Ordered cheapest-first, and deliberately not short-circuited into a single
// boolean: the reason string is what turns "the clip is silent" into a log line
// that names the bug.
inline bool poolValidate(const u8* base, size_t payloadBytes, const PoolHeader* hdr,
                         u64 ref, u32 wantKind, u64 needBytes, const char** why,
                         u64* outBytes = nullptr) {
    auto no = [&](const char* r) { if (why) *why = r; return false; };
    if (why) *why = "";
    if (outBytes) *outBytes = 0;

    if (!base || !hdr)                       return no("no pool attached");
    if (ref == 0)                            return no("null offset");
    if (ref % kPoolAlign != 0)               return no("offset is not 64-byte aligned");

    const u64 arenaLo = hdr->arenaOffset;
    const u64 arenaHi = hdr->arenaEnd();
    if (arenaHi > payloadBytes)              return no("pool header describes an arena past the mapping");
    // The header sits immediately before the data, so the data offset must
    // leave room for it inside the arena.
    if (ref < arenaLo + sizeof(PoolBlock))   return no("offset is before the first possible block");
    if (ref >= arenaHi)                      return no("offset is past the end of the arena");

    // Only the bump-allocated prefix has ever held a block. An offset past it
    // points at memory the allocator has not touched, where a stale magic from
    // a previous *session* could otherwise survive in a reused page.
    const u64 bump = hdr->bump.load(std::memory_order_acquire);
    if (ref > bump)                          return no("offset is past the allocator's high-water mark");

    const PoolBlock* b = (const PoolBlock*)(base + ref - sizeof(PoolBlock));
    if (b->magic.load(std::memory_order_acquire) != (kPoolBlockMagic ^ ref))
        return no("no block header at that offset (bad magic)");

    // Past the magic we are on the block's own terms, but it is still a number
    // another process wrote: a wild `bytes` would turn a valid block into an
    // arbitrary read. Load it ONCE into a local (F3): every check below, and the
    // size handed back to the caller, must reason about one snapshot — a writer
    // that flips `b->bytes` between the arena-bound check and the extent check
    // could otherwise widen the accepted read past the mapping. `kind` gets the
    // same one-load treatment for the same reason.
    const u64 bytes = b->bytes;
    const u32 kind  = b->kind;
    if (bytes == 0 || bytes % kPoolAlign != 0) return no("block size is not a positive multiple of 64");
    if (bytes > arenaHi - ref)               return no("block extends past the end of the arena");
    if (ref + bytes > bump)                  return no("block extends past the allocator's high-water mark");

    const u32 st = b->state.load(std::memory_order_acquire);
    if (st == BlockFree)                     return no("block has been freed");
    if (wantKind != PoolKindNone && kind != wantKind)
        return no(wantKind == PoolKindSamples     ? "block does not hold sample data"
                : wantKind == PoolKindNotes       ? "block does not hold notes"
                : wantKind == PoolKindString      ? "block does not hold a string"
                : wantKind == PoolKindArrangement ? "block does not hold an arrangement"
                : wantKind == PoolKindTrackAutos  ? "block does not hold track automation"
                : wantKind == PoolKindRackState   ? "block does not hold a rack state"
                : wantKind == PoolKindSignatures  ? "block does not hold a signature map"
                : wantKind == PoolKindDeviceState ? "block does not hold a device state"
                                                  : "block is of the wrong kind");
    if (needBytes > bytes)                   return no("block is smaller than the clip claims");
    // Hand the validated extent back so callers never re-read the mutable field
    // (F4): the scan/copy bound must come from what validation proved, not from
    // a fresh load a hostile writer can widen after the fact.
    if (outBytes) *outBytes = bytes;
    return true;
}

// ---------------------------------------------------------------------------
// SamplePool — the writer side (the GUI)
// ---------------------------------------------------------------------------
//
// Creator, allocator and sole writer. An arena with a bump pointer and an
// address-ordered free list; first fit, split on over-large fits, coalesce on
// free, and retract the bump when the tail comes back. That last one is why a
// pool that has held one block and freed it hands out the *same offset* next
// time: the common edit-a-clip-and-repush loop reuses one block forever instead
// of walking the arena, and a test can assert it.
//
// Nothing here is realtime and nothing here is called from an audio thread.
// The engine's side of this file is poolValidate() and one addition.
//
// THE FREE-AFTER-CONFIRM RULE
// ---------------------------
// This is §3.5's retirement pattern, and it is the only part of the pool that
// is subtle, so it is stated as an invariant rather than described:
//
//   A block may be returned to the free list only when both
//     (a) the GUI holds no references of its own            — refs == 0, and
//     (b) it is not reachable from the engine               — state is
//         Quiescent, i.e. either it was never published, or the daemon has
//         echoed its offset back in an EvBlockRetired event.
//
// The states enforce it. `markLive` (a clip cell now points here) moves a block
// to Live. `markDisplaced` (the cell was overwritten or cleared) moves it to
// Retiring — *not* to Quiescent, and never straight to free, no matter what the
// GUI's own refcount says. Only `confirmRetired`, driven by the daemon's echo,
// moves Retiring to Quiescent. `free()` refuses outright on Live or Retiring:
// a GUI bug becomes a rejected call and a log line instead of the engine
// reading a block that has been handed to something else.
//
// What the echo actually proves is on the daemon's side of the boundary
// (src/daemon/nxtaktd.cpp, "retirement"): the displacing command has been
// handed to Engine::pushCommand, no other clip cell still names the offset, and
// the audio thread has since run drainCommands() — after which no voice can
// reach the old data, because a voice holds `&clips_[t][s]` and that cell now
// holds the new clip.
//
// Note the failure mode if that proof were ever wrong, because it bounds the
// whole design: the pool region stays mapped for the daemon's entire life and
// never shrinks, so a premature free cannot produce a wild pointer or a
// segfault. The worst case is a voice reading bytes that now belong to a
// different clip — audible, findable, and not a crash. Every other ownership
// bug in this codebase is worse than that.
class SamplePool {
public:
    SamplePool() = default;
    ~SamplePool() { close(); }
    SamplePool(const SamplePool&)            = delete;
    SamplePool& operator=(const SamplePool&) = delete;

    // -- lifecycle ----------------------------------------------------------

    bool create(const char* name, size_t payloadBytes = kDefaultPoolBytes, u64 epoch = 1) {
        close();
        if (payloadBytes <= kPoolArenaOffset + sizeof(PoolBlock) + kPoolAlign) {
            setErr("pool size %zu is too small to hold a single block", payloadBytes);
            return false;
        }
        // F8b: no pre-emptive reapIfStale() here. It is redundant with
        // create()'s own EEXIST reap (the only correctly guarded form: it reaps
        // only after O_EXCL fails), and it is dangerous — the pool's ftruncate
        // is 256 MiB wide, so a sibling between shm_open() and ftruncate() spends
        // a long time looking "unsized", and a pre-reap would unlink its live
        // name. Worse, keying that reap on ShmHeader::creatorPid is wrong for the
        // pool specifically: the pool is DESIGNED to outlive its creator and be
        // adopted (abandon()/attach()), and an adopter did not use to re-stamp
        // the creator — so a live, adopted pool read as stale. Adoption now
        // re-stamps the owner (see attach()), and create() alone reaps.
        if (!region_.create(name, payloadBytes, pool::kHash, kShmVersion, /*seal*/true)) {
            setErr("%s", region_.error());
            return false;
        }
        hdr_ = region_.at<PoolHeader>(0);
        if (!hdr_) {
            setErr("%s: pool header does not fit its own region", name);
            region_.close();
            return false;
        }
        hdr_->init(region_.payloadBytes(), epoch, region_.sealed());
        base_ = region_.payload();
        region_.publishReady();
        err_[0] = '\0';
        return true;
    }

    // Attach to a pool somebody else created — the §4.3 reattach path, and how
    // a test inspects a pool from a second handle. Read/write, because the
    // attacher is a *GUI*: the daemon uses PoolReader instead.
    bool attach(const char* name, int timeoutMs = 0) {
        close();
        if (!region_.attach(name, pool::kHash, kShmVersion, timeoutMs)) {
            setErr("%s", region_.error());
            return false;
        }
        hdr_ = region_.at<PoolHeader>(0);
        if (!hdr_ || hdr_->magic != kPoolMagic || hdr_->version != kPoolVersion) {
            setErr("%s: not an NxTakt sample pool (magic/version)", name);
            region_.close();
            hdr_ = nullptr;
            return false;
        }
        base_ = region_.payload();
        // F8b: this handle is a GUI adopting a pool its creator may have left
        // behind. Take ownership of the liveness key so reapIfStale() keys on
        // THIS live process, not the original (possibly dead) creator — a live
        // adopted pool must never read as an orphan. attach() maps R/W for a
        // GUI, so this write is legal; the daemon's PoolReader maps read-only
        // and never adopts.
        region_.adoptCreator();
        err_[0] = '\0';
        return true;
    }

    void close() {
        hdr_  = nullptr;
        base_ = nullptr;
        region_.close();
    }

    // Detach without unlinking: the region stays in /dev/shm for whoever
    // adopts it next. This is what a GUI does when it hands a live session to
    // a replacement, and what a crash does implicitly.
    void abandon() {
        hdr_  = nullptr;
        base_ = nullptr;
        region_.release();
    }

    bool        valid()  const { return base_ != nullptr && hdr_ != nullptr; }
    const char* name()   const { return region_.name(); }
    const char* error()  const { return err_; }
    bool        sealed() const { return region_.sealed(); }
    size_t      bytes()  const { return region_.payloadBytes(); }
    u64         epoch()  const { return hdr_ ? hdr_->epoch.load(std::memory_order_relaxed) : 0; }

    const PoolHeader* header() const { return hdr_; }
    const u8*         base()   const { return base_; }

    u64 bump() const { return hdr_ ? hdr_->bump.load(std::memory_order_relaxed) : 0; }
    u64 used() const { return hdr_ ? hdr_->bytesUsed.load(std::memory_order_relaxed) : 0; }
    u64 liveBlocks() const { return hdr_ ? hdr_->liveBlocks.load(std::memory_order_relaxed) : 0; }

    // Bytes the allocator could still hand out in one piece. Reported rather
    // than computed by callers because "how much is left" is bump headroom plus
    // the largest free block, and getting that wrong is how a pool looks full
    // when it is merely fragmented.
    u64 largestFree() const {
        if (!valid()) return 0;
        u64 best = hdr_->arenaEnd() - hdr_->bump.load(std::memory_order_relaxed);
        if (best > sizeof(PoolBlock)) best -= sizeof(PoolBlock); else best = 0;
        for (u64 o = hdr_->freeHead.load(std::memory_order_relaxed); o; ) {
            const PoolBlock* b = blockAt(o);
            if (!b) break;
            if (b->bytes > best) best = b->bytes;
            o = b->next;
        }
        return best;
    }

    // -- allocation ---------------------------------------------------------

    // Returns the data offset, or 0. The block comes back Quiescent with one
    // GUI reference: allocated, safe to free, not yet visible to the engine.
    u64 alloc(size_t wantBytes, u32 kind, i64 frames, u32 channels, f64 rate, u64 key) {
        if (!valid() || wantBytes == 0) return 0;
        const u64 need = (u64)alignUp(wantBytes, kPoolAlign);

        u64 ref = takeFromFreeList(need);
        if (!ref) ref = takeFromBump(need);
        if (!ref) {
            setErr("pool exhausted: %llu B wanted, %llu B largest free block",
                   (unsigned long long)need, (unsigned long long)largestFree());
            return 0;
        }

        // Direct pointer, not blockAt(): a freshly bump-carved block has no magic
        // yet (it is written with release at the end of this function), so it
        // would not pass validRef()'s F6 magic check — and it must not have to.
        // alloc() is the trusted producer that just carved `ref`; validRef() is
        // for offsets that arrived from outside.
        PoolBlock* b = (PoolBlock*)(base_ + ref - sizeof(PoolBlock));
        b->key      = key;
        b->frames   = frames;
        b->rate     = rate;
        b->channels = channels;
        b->kind     = kind;
        b->refs     = 1;
        b->live     = 0;
        b->next     = 0;
        b->state.store(BlockQuiescent, std::memory_order_relaxed);
        // Last, with release: everything above and every byte the caller is
        // about to write is ordered before any reader can find this block.
        // (The caller writes the data next and only then publishes `ref` over
        // the command ring, which is a second release edge — this one is what
        // makes the block self-describing even so.)
        b->magic.store(kPoolBlockMagic ^ ref, std::memory_order_release);

        hdr_->liveBlocks.fetch_add(1, std::memory_order_relaxed);
        hdr_->bytesUsed.fetch_add(b->bytes, std::memory_order_relaxed);
        hdr_->generation.fetch_add(1, std::memory_order_release);
        err_[0] = '\0';
        return ref;
    }

    // Decode-straight-into-shm is the point of §3.5's "one large win": callers
    // that can synthesise or decode in place should alloc() and write through
    // data<f32>(ref) rather than build a vector and hand it here.
    u64 writeSamples(const f32* interleaved, i64 frames, int channels,
                     f64 rate = 0.0, u64 key = 0) {
        if (frames <= 0 || channels < 1 || channels > 2) return 0;
        const size_t n = (size_t)frames * (size_t)channels;
        const u64 ref = alloc(n * sizeof(f32), PoolKindSamples, frames, (u32)channels, rate, key);
        if (!ref) return 0;
        if (interleaved) std::memcpy(base_ + ref, interleaved, n * sizeof(f32));
        else             std::memset(base_ + ref, 0, n * sizeof(f32));
        return ref;
    }

    // A string blob (PoolKindString). Returns the offset, or 0. The NUL is
    // written and counted, so a daemon that trusts nothing still gets a
    // terminated buffer if it copies `bytes` of it — but it must not trust
    // that either, and does not (Daemon::readPoolString).
    u64 writeString(const char* s) { return writeText(s, PoolKindString, kMaxPoolString); }

    // A rack's contents (PoolKindRackState), i.e. rackStateToString()'s output.
    // Same bytes-and-a-terminator shape as writeString and deliberately NOT the
    // same kind or the same bound — see PoolKindRackState and kMaxRackState.
    u64 writeRackState(const char* s) { return writeText(s, PoolKindRackState, kMaxRackState); }

    // One device's generic state (PoolKindDeviceState): the header, then the
    // NUL-terminated state string. `hdr.textBytes` is IGNORED and recomputed
    // from `s`, so a header that disagrees with the string it precedes cannot be
    // built here by accident — the same rule poolWriteArrangement applies to its
    // counts, for the same reason.
    //
    // The audio the header may name is NOT written here: it is an ordinary
    // PoolKindSamples block from writeSamples(), so a sampler's audio goes
    // through exactly the allocator path, the validation and the free-after-
    // confirm bookkeeping a clip's does, and there is one implementation of
    // "audio in the pool" rather than two.
    u64 writeDeviceState(const WireDeviceState& hdr, const char* s) {
        if (!s) return 0;
        const size_t len = std::strlen(s);
        if (len + 1 > kMaxDeviceState) return 0;
        const u64 bytes = (u64)sizeof(WireDeviceState) + (u64)len + 1;
        const u64 ref = alloc(bytes, PoolKindDeviceState, (i64)bytes, 0, 0.0, 0);
        if (!ref) return 0;
        WireDeviceState h = hdr;
        h.version   = kDeviceStateVersion;
        h.textBytes = (u32)(len + 1);
        std::memcpy(base_ + ref, &h, sizeof h);
        std::memcpy(base_ + ref + sizeof h, s, len);
        base_[ref + sizeof h + len] = '\0';
        return ref;
    }

    // The shared half. `cap` is the caller's policy bound including the NUL, and
    // it is checked BEFORE the allocation so that an over-long payload is a
    // refusal the caller can report rather than a pool that fills with strings
    // nobody asked for.
    u64 writeText(const char* s, u32 kind, u64 cap) {
        if (!s) return 0;
        const size_t len = std::strlen(s);
        if (len + 1 > cap) return 0;
        const u64 ref = alloc(len + 1, kind, (i64)(len + 1), 0, 0.0, 0);
        if (!ref) return 0;
        std::memcpy(base_ + ref, s, len);
        base_[ref + len] = '\0';
        return ref;
    }

    // The signature map (PoolKindSignatures), `count` entries. Bounded by
    // kMaxSigs on the way in rather than clamped, so a caller with a bigger map
    // gets a refusal it can report rather than a map that silently loses its
    // tail — the last entry of a signature map runs FOREVER, so a truncated one
    // is not a shorter song, it is a different one.
    u64 writeSignatures(const WireSig* sigs, i64 count) {
        if (count <= 0 || count > kMaxSigs) return 0;
        const size_t n = (size_t)count * sizeof(WireSig);
        const u64 ref = alloc(n, PoolKindSignatures, count, 0, 0.0, 0);
        if (!ref) return 0;
        if (sigs) std::memcpy(base_ + ref, sigs, n);
        else      std::memset(base_ + ref, 0, n);
        return ref;
    }

    u64 writeNotes(const WireNote* notes, i64 count, u64 key = 0) {
        if (count <= 0) return 0;
        const size_t n = (size_t)count * sizeof(WireNote);
        const u64 ref = alloc(n, PoolKindNotes, count, 0, 0.0, key);
        if (!ref) return 0;
        if (notes) std::memcpy(base_ + ref, notes, n);
        else       std::memset(base_ + ref, 0, n);
        return ref;
    }

    template <typename T> T* data(u64 ref) {
        return validRef(ref) ? (T*)(base_ + ref) : nullptr;
    }
    template <typename T> const T* data(u64 ref) const {
        return validRef(ref) ? (const T*)(base_ + ref) : nullptr;
    }

    // Content-key lookup: §4.3's "matched clips do not reload from disk and,
    // crucially, do not have their poolRef changed, so anything currently
    // playing keeps playing without a glitch". Walks the arena rather than an
    // index because the arena *is* the index and a few hundred blocks is a
    // microsecond.
    u64 findByKey(u64 key) const {
        if (!valid() || key == 0) return 0;
        for (u64 off = hdr_->arenaOffset + sizeof(PoolBlock);
             off + sizeof(PoolBlock) <= hdr_->bump.load(std::memory_order_relaxed); ) {
            const PoolBlock* b = (const PoolBlock*)(base_ + off - sizeof(PoolBlock));
            if (b->magic.load(std::memory_order_acquire) != (kPoolBlockMagic ^ off)) break;
            if (b->key == key && b->state.load(std::memory_order_relaxed) != BlockFree) return off;
            off += b->bytes + sizeof(PoolBlock);
        }
        return 0;
    }

    // -- the free-after-confirm state machine -------------------------------

    void addRef(u64 ref)  { if (PoolBlock* b = blockAt(ref)) ++b->refs; }

    // Drops a GUI reference. Frees only if the block is provably out of the
    // engine's reach; otherwise the free waits for confirmRetired(), which is
    // the entire rule.
    bool release(u64 ref) {
        PoolBlock* b = blockAt(ref);
        if (!b) return false;
        if (b->refs > 0) --b->refs;
        if (b->refs == 0 && b->state.load(std::memory_order_relaxed) == BlockQuiescent)
            return free(ref);
        return false;
    }

    // A clip cell now points at this block.
    void markLive(u64 ref) {
        if (PoolBlock* b = blockAt(ref)) {
            ++b->live;
            b->state.store(BlockLive, std::memory_order_release);
        }
    }

    // The clip cell write that would have pointed here was refused at the
    // boundary, so the engine never saw it. Undoing markLive is therefore
    // safe in a way markDisplaced is not: there is nothing to retire, because
    // nothing was ever published.
    void unmarkLive(u64 ref) {
        PoolBlock* b = blockAt(ref);
        if (!b) return;
        if (b->live > 0) --b->live;
        if (b->live == 0 && b->state.load(std::memory_order_relaxed) == BlockLive)
            b->state.store(BlockQuiescent, std::memory_order_release);
    }

    // A clip cell stopped pointing at this block. The block does NOT become
    // freeable here even at live == 0 and refs == 0: the engine may still be
    // holding the old clip, and only the daemon can say otherwise.
    void markDisplaced(u64 ref) {
        PoolBlock* b = blockAt(ref);
        if (!b) return;
        if (b->live > 0) --b->live;
        if (b->live == 0 && b->state.load(std::memory_order_relaxed) == BlockLive)
            b->state.store(BlockRetiring, std::memory_order_release);
    }

    // The daemon echoed this offset back: the engine cannot reach it any more.
    // Returns true if the block was freed as a result.
    bool confirmRetired(u64 ref) {
        PoolBlock* b = blockAt(ref);
        if (!b) return false;
        if (b->state.load(std::memory_order_relaxed) != BlockRetiring) return false;
        if (b->live > 0) {                 // re-published while the echo flew
            b->state.store(BlockLive, std::memory_order_release);
            return false;
        }
        b->state.store(BlockQuiescent, std::memory_order_release);
        return b->refs == 0 ? free(ref) : false;
    }

    // The hard free. Refuses a block the engine might still hold: this is the
    // last line of the rule and it is a refusal, not an assert, because the
    // GUI must survive its own bugs.
    bool free(u64 ref) {
        PoolBlock* b = blockAt(ref);
        if (!b) return false;
        const u32 st = b->state.load(std::memory_order_relaxed);
        if (st == BlockLive || st == BlockRetiring) {
            setErr("refusing to free block %llu: it is %s (the engine may still read it)",
                   (unsigned long long)ref, poolStateName(st));
            return false;
        }
        if (st == BlockFree) return false;

        hdr_->liveBlocks.fetch_sub(1, std::memory_order_relaxed);
        hdr_->bytesUsed.fetch_sub(b->bytes, std::memory_order_relaxed);
        b->refs = 0;
        b->live = 0;
        b->key  = 0;
        b->kind = PoolKindNone;
        b->state.store(BlockFree, std::memory_order_release);
        insertFree(ref);
        normalize();
        hdr_->generation.fetch_add(1, std::memory_order_release);
        return true;
    }

    // -- inspection (tests, diagnostics) ------------------------------------

    PoolBlock* blockAt(u64 ref) {
        return validRef(ref) ? (PoolBlock*)(base_ + ref - sizeof(PoolBlock)) : nullptr;
    }
    const PoolBlock* blockAt(u64 ref) const {
        return validRef(ref) ? (const PoolBlock*)(base_ + ref - sizeof(PoolBlock)) : nullptr;
    }
    u32 stateOf(u64 ref) const {
        const PoolBlock* b = blockAt(ref);
        return b ? b->state.load(std::memory_order_relaxed) : BlockFree;
    }
    u32 refsOf(u64 ref) const { const PoolBlock* b = blockAt(ref); return b ? b->refs : 0; }
    u32 liveOf(u64 ref) const { const PoolBlock* b = blockAt(ref); return b ? b->live : 0; }

    u32 freeListLength() const {
        if (!valid()) return 0;
        u32 n = 0;
        for (u64 o = hdr_->freeHead.load(std::memory_order_relaxed); o && n < 1u << 20; ++n) {
            const PoolBlock* b = blockAt(o);
            if (!b) break;
            o = b->next;
        }
        return n;
    }

private:
    bool validRef(u64 ref) const {
        if (!valid() || ref == 0 || ref % kPoolAlign != 0) return false;
        if (ref < hdr_->arenaOffset + sizeof(PoolBlock)) return false;
        if (ref > hdr_->bump.load(std::memory_order_relaxed)) return false;
        // Bounds alone let any 64-aligned offset under the bump masquerade as a
        // block, so a forged EvBlockRetired echo pointing into live sample data
        // would be reinterpreted as a PoolBlock (F6). The self-mixed magic is
        // exactly the check the daemon-side poolValidate() makes; the writer
        // side must make it too, or a hostile echo corrupts the free list.
        const PoolBlock* b = (const PoolBlock*)(base_ + ref - sizeof(PoolBlock));
        if (b->magic.load(std::memory_order_acquire) != (kPoolBlockMagic ^ ref)) return false;
        return true;
    }

    // First fit over the address-ordered free list, splitting when the leftover
    // is worth having. First fit rather than best fit on purpose: clip buffers
    // are large and few (§3.5), the list is short, and best fit's win only
    // shows up in workloads this one is not.
    u64 takeFromFreeList(u64 need) {
        u64 prev = 0;
        for (u64 off = hdr_->freeHead.load(std::memory_order_relaxed); off; ) {
            PoolBlock* b = blockAt(off);
            if (!b) break;
            const u64 next = b->next;
            if (b->bytes >= need) {
                // Split only if the tail can hold a header plus one aligned
                // line; otherwise the caller keeps the slack.
                const u64 slack = b->bytes - need;
                if (slack >= sizeof(PoolBlock) + kPoolAlign) {
                    const u64 tailRef = off + need + sizeof(PoolBlock);
                    PoolBlock* t = (PoolBlock*)(base_ + tailRef - sizeof(PoolBlock));
                    std::memset((void*)t, 0, sizeof(PoolBlock));
                    t->bytes = slack - sizeof(PoolBlock);
                    t->next  = next;
                    t->state.store(BlockFree, std::memory_order_relaxed);
                    t->magic.store(kPoolBlockMagic ^ tailRef, std::memory_order_release);
                    b->bytes = need;
                    relinkFree(prev, tailRef);
                } else {
                    relinkFree(prev, next);
                }
                return off;
            }
            prev = off;
            off  = next;
        }
        return 0;
    }

    u64 takeFromBump(u64 need) {
        const u64 hdrOff = hdr_->bump.load(std::memory_order_relaxed);
        const u64 ref    = hdrOff + sizeof(PoolBlock);
        if (ref < hdrOff) return 0;                                  // overflow
        if (need > hdr_->arenaEnd() || ref > hdr_->arenaEnd() - need) return 0;
        PoolBlock* b = (PoolBlock*)(base_ + hdrOff);
        std::memset((void*)b, 0, sizeof(PoolBlock));
        b->bytes = need;
        // The high-water mark moves *before* the block is published, so a
        // reader that sees the magic also sees a bump that covers it.
        hdr_->bump.store(ref + need, std::memory_order_release);
        return ref;
    }

    // Points the free list's `prev` link (or the head, when prev is 0) at
    // `next`. Every unlink and every splice in this allocator is this one
    // operation, which is the only reason the head-vs-body special case does
    // not appear four times.
    void relinkFree(u64 prev, u64 next) {
        if (prev) blockAt(prev)->next = next;
        else      hdr_->freeHead.store(next, std::memory_order_relaxed);
    }

    // Address-ordered insertion. The order is not for search speed — the list
    // is short — it is what makes coalescing a single linear pass instead of a
    // neighbour lookup structure.
    void insertFree(u64 ref) {
        PoolBlock* b = blockAt(ref);
        if (!b) return;                       // forged/absorbed ref: refuse to splice
        u64 prev = 0;
        u64 cur  = hdr_->freeHead.load(std::memory_order_relaxed);
        while (cur && cur < ref) {
            PoolBlock* c = blockAt(cur);
            if (!c) break;                    // a corrupted `next` ends the walk, not the process
            prev = cur; cur = c->next;
        }
        b->next = cur;
        if (prev) { PoolBlock* p = blockAt(prev); if (p) p->next = ref; }
        else      hdr_->freeHead.store(ref, std::memory_order_relaxed);
    }

    // One pass: merge every pair of adjacent free blocks, then give the arena's
    // tail back to the bump pointer. Done eagerly on every free rather than
    // lazily on allocation failure, because it is O(free list) on a list of
    // tens of entries and it is what makes the pool's behaviour predictable
    // enough to assert on.
    void normalize() {
        u64 cur = hdr_->freeHead.load(std::memory_order_relaxed);
        while (cur) {
            PoolBlock* b = blockAt(cur);
            if (!b) break;                                      // corrupted head/next: stop, don't fault
            const u64 next = b->next;
            if (next && cur + b->bytes + sizeof(PoolBlock) == next) {
                PoolBlock* n = blockAt(next);
                if (!n) break;                                  // the mergee is not a real block
                b->bytes += sizeof(PoolBlock) + n->bytes;
                b->next   = n->next;
                n->magic.store(0, std::memory_order_release);   // absorbed
                continue;                                       // retry the same block
            }
            cur = next;
        }
        // Tail retraction: a free block that ends at the high-water mark is not
        // a hole, it is unused arena. Giving it back is what makes "free the
        // only block, allocate the same size, get the same offset" true.
        for (;;) {
            const u64 bump = hdr_->bump.load(std::memory_order_relaxed);
            u64 prev = 0, cur2 = hdr_->freeHead.load(std::memory_order_relaxed);
            bool retracted = false;
            while (cur2) {
                PoolBlock* b = blockAt(cur2);
                if (cur2 + b->bytes == bump) {
                    relinkFree(prev, b->next);
                    b->magic.store(0, std::memory_order_release);
                    hdr_->bump.store(cur2 - sizeof(PoolBlock), std::memory_order_release);
                    retracted = true;
                    break;
                }
                prev = cur2;
                cur2 = b->next;
            }
            if (!retracted) break;
        }
    }

    void setErr(const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(err_, sizeof err_, fmt, ap);
        va_end(ap);
    }

    ShmRegion   region_;
    PoolHeader* hdr_  = nullptr;
    u8*         base_ = nullptr;
    char        err_[256] = {};
};

// ---------------------------------------------------------------------------
// PoolReader — the reader side (the daemon)
// ---------------------------------------------------------------------------
//
// Read-only, by page permission and not merely by convention. It owns no
// allocator state and has no way to acquire any: everything it can do is
// validate an offset and turn it into a `const` pointer.
//
// The mapping is established once, on the daemon's control thread, and held for
// the daemon's whole life. That is deliberate and it is what makes handing
// `base + ref` to the engine safe: the audio thread never sees an mmap, and no
// address the engine holds can be unmapped while it might still be reading —
// the region only goes away when the daemon does.
class PoolReader {
public:
    PoolReader() = default;
    ~PoolReader() { close(); }
    PoolReader(const PoolReader&)            = delete;
    PoolReader& operator=(const PoolReader&) = delete;

    bool attach(const char* name, int timeoutMs = 0) {
        close();
        if (!region_.attach(name, pool::kHash, kShmVersion, timeoutMs, /*readOnly*/true)) {
            setErr("%s", region_.error());
            return false;
        }
        // Size before contents: the layout hash already agreed, but a header
        // read out of a mapping too small to hold one is the one mistake that
        // would fault instead of failing.
        if (region_.payloadBytes() < kPoolArenaOffset + sizeof(PoolBlock)) {
            setErr("%s: %zu B is too small to be a sample pool", name, region_.payloadBytes());
            region_.close();
            return false;
        }
        const PoolHeader* h = (const PoolHeader*)region_.payload();
        if (h->magic != kPoolMagic || h->version != kPoolVersion) {
            setErr("%s: not an NxTakt sample pool (magic 0x%016llx, version %u)",
                   name, (unsigned long long)h->magic, h->version);
            region_.close();
            return false;
        }
        if (h->arenaOffset != kPoolArenaOffset || h->arenaEnd() > region_.payloadBytes()) {
            setErr("%s: pool arena (%llu + %llu) does not fit the %zu B mapping",
                   name, (unsigned long long)h->arenaOffset, (unsigned long long)h->arenaBytes,
                   region_.payloadBytes());
            region_.close();
            return false;
        }
        hdr_   = h;
        base_  = region_.payload();
        bytes_ = region_.payloadBytes();
        std::snprintf(name_, sizeof name_, "%s", name);
        err_[0] = '\0';
        return true;
    }

    void close() {
        hdr_ = nullptr; base_ = nullptr; bytes_ = 0; name_[0] = '\0';
        region_.close();
    }

    bool        valid() const { return base_ != nullptr && hdr_ != nullptr; }
    const char* name()  const { return name_; }
    const char* error() const { return err_; }
    size_t      bytes() const { return bytes_; }
    u64         epoch() const { return hdr_ ? hdr_->epoch.load(std::memory_order_relaxed) : 0; }
    const PoolHeader* header() const { return hdr_; }

    bool validate(u64 ref, u32 kind, u64 needBytes, const char** why,
                  u64* outBytes = nullptr) const {
        return poolValidate(base_, bytes_, hdr_, ref, kind, needBytes, why, outBytes);
    }

    // Only ever called after validate() said yes. Returns a pointer into a
    // mapping that outlives every clip that could reference it.
    const u8* at(u64 ref) const { return base_ + ref; }

    // The block's own header. Also only after validate() — that is what proves
    // the 128 bytes before `ref` are inside the mapping and are a header.
    // Needed because the *declared size* of a block bounds how far the daemon
    // may read into it, which matters for a string: the terminator has to be
    // inside the allocation or the blob is not a string at all.
    const PoolBlock* block(u64 ref) const {
        return (const PoolBlock*)(base_ + ref - sizeof(PoolBlock));
    }

    // The inverse: an address the engine handed back (Ev::NotesRetired carries
    // one) turned into the offset the GUI knows it by. Returns 0 for anything
    // that is not inside this pool, which is the answer that matters — a
    // pointer from somewhere else must not be echoed as if it were a block.
    u64 offsetOf(const void* p) const {
        if (!valid() || !p) return 0;
        const u8* q = (const u8*)p;
        if (q < base_ || q >= base_ + bytes_) return 0;
        return (u64)(q - base_);
    }

    // Is the GUI that created this pool still running? The pool's ownership
    // inversion is what makes this useful to the daemon: the control region's
    // creator is the daemon itself, so the only pid in shared memory that
    // belongs to the *client* is this one. It is the daemon's whole answer to
    // "my client was SIGKILLed", and it is why a take does not need a heartbeat
    // in the protocol to be reclaimable.
    bool creatorAlive() const { return region_.creatorAlive(); }

private:
    void setErr(const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(err_, sizeof err_, fmt, ap);
        va_end(ap);
    }

    ShmRegion         region_;
    const PoolHeader* hdr_   = nullptr;
    const u8*         base_  = nullptr;
    size_t            bytes_ = 0;
    char              name_[128] = {};
    char              err_[256]  = {};
};

} // namespace lat::ipc
