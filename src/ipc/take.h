// NxTakt IPC — a finished take, on disk.
//
// docs/GUI-ON-DAEMON.md §7 option (2), which §13.8 re-affirmed: **the daemon
// writes the take and the client is told where to find it.** This header is the
// "where" and the "what shape": the directory rule, the file names, and the two
// formats a take can have.
//
// WHY A FILE AND NOT A SECOND SHARED REGION
// -----------------------------------------
// §7's option (1) is a take region — a second `ShmRegion` created by the daemon
// and mapped read-only by the client, i.e. pool.h with its page permissions
// inverted. It would work. It costs a second allocator, a second retirement
// protocol running the other way, a second epoch handshake, and — the part that
// decided it — it would make the daemon a *writer* of shared memory the client
// reads, which is the one asymmetry the whole design has been built around
// (pool.h: "a daemon bug cannot corrupt the allocator, because the pages are not
// writable in that address space"). A take is written once, sequentially, by a
// thread that is not the audio thread, and read once. That is a file. It is also
// a file the user still has after the GUI has crashed, which no shared region
// can claim: the take survives its reader.
//
// The cost is honest and is stated here rather than discovered: one write and
// one read of the take's bytes that the in-process path does not pay, both off
// the audio thread, and a directory that has to be swept.
//
// TWO FORMATS, ONE MECHANISM
// --------------------------
//   audio   a canonical 32-bit float WAV. §7 asks for `.wav` by name, and it is
//           what makes "promoted into the project directory on save" a rename
//           rather than a conversion — and what makes a take recovered by hand
//           after a crash openable in anything.
//   midi    `.ntk`: a 32-byte header and a WireNote[]. A MIDI take in a WAV
//           would be a lie about what the bytes are, and MIDI takes are small
//           enough (4096 notes is 96 KiB) that nothing about the format is
//           load-bearing except that it is checkable.
//
// Both are written whole, to a temporary name, and renamed into place. A reader
// that can see the final name is therefore looking at a complete file — the same
// publication edge `PoolBlock::magic` gives the pool, made out of the one thing
// a filesystem guarantees.
//
// Header-only and libc-only, like the rest of src/ipc. Deliberately NO call to
// lat::env(): that lives in common.cpp and ipc_test links no .cpp at all.
#pragma once
#include "pool.h"          // WireNote, and through it core/ and the wire types

#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lat::ipc {

// Bumped when the bytes of a take file change meaning. Independent of
// kProtocolVersion: a take file outlives the pair of processes that made it,
// which is the entire argument for it being a file.
inline constexpr u32 kTakeFormatVersion = 1;

// The ceiling on one take, in BYTES of payload. 256 MiB is ~23 minutes of
// stereo float at 48 kHz — past any take a session slot holds, and the number
// that stops a client asking for a capacity the daemon would try to allocate.
// It is a refusal with a reason (RejectTakeTooLarge), never a silent clamp:
// a take quietly cut to a third of what was asked for is the "committed short"
// failure docs/ARRANGEMENT.md §5.4 refuses by name.
inline constexpr u64 kMaxTakeBytes = 256ull * 1024ull * 1024ull;

// How many finished-but-unclaimed takes the daemon will hold at once. Each one
// is a buffer and a file, so this is the memory bound a client that stops
// answering runs into — and the reason it is a refusal rather than an eviction
// is the same reason everything else here is: the oldest take is still a take.
inline constexpr u32 kMaxPendingTakes = 8;

// ---------------------------------------------------------------------------
// Where takes live
// ---------------------------------------------------------------------------
//
// `$XDG_RUNTIME_DIR/nxtakt/takes/<session>/`, falling back to `/tmp/nxtakt-<uid>
// /takes/<session>/` when the runtime dir is unset (a daemon started from a
// service manager, a container, a cron job). The DAEMON decides, and publishes
// what it decided in ControlHeader::takeDir — the client never recomputes it.
// Two processes agreeing on a formula they each evaluate separately is exactly
// the bug this avoids: they need not share an environment, and if they do not,
// the one that gets it wrong reads an empty directory and calls it a lost take.
inline void takeDirFor(const char* session, char* out, size_t cap) {
    if (!out || cap == 0) return;
    const char* s = (session && *session) ? session : "default";
    if (const char* rt = ::getenv("XDG_RUNTIME_DIR"))
        std::snprintf(out, cap, "%s/nxtakt/takes/%s", rt, s);
    else
        std::snprintf(out, cap, "/tmp/nxtakt-%u/takes/%s", (unsigned)::getuid(), s);
}

// mkdir -p, for a path this header composed. Returns true when the directory
// exists afterwards, whoever created it.
inline bool takeMkdirP(const char* dir) {
    if (!dir || !*dir) return false;
    char buf[512];
    std::snprintf(buf, sizeof buf, "%s", dir);
    for (char* p = buf + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        ::mkdir(buf, 0700);
        *p = '/';
    }
    if (::mkdir(buf, 0700) == 0) return true;
    struct stat st{};
    return ::stat(buf, &st) == 0 && S_ISDIR(st.st_mode);
}

// The take's own name. The uid is the daemon's monotonic take counter, so two
// takes never collide and a stale file is identifiable as stale.
inline void takePath(const char* dir, u64 uid, bool midi, char* out, size_t cap) {
    std::snprintf(out, cap, "%s/%llu.%s", dir ? dir : ".", (unsigned long long)uid,
                  midi ? "ntk" : "wav");
}

// ---------------------------------------------------------------------------
// The WAV a take is
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct WavHeader {
    char riff[4];          // "RIFF"
    u32  riffSize;         // 36 + dataBytes
    char wave[4];          // "WAVE"
    char fmt[4];           // "fmt "
    u32  fmtSize;          // 16
    u16  format;           // 3 = IEEE float
    u16  channels;
    u32  rate;
    u32  byteRate;
    u16  blockAlign;
    u16  bits;             // 32
    char data[4];          // "data"
    u32  dataSize;
};
#pragma pack(pop)
static_assert(sizeof(WavHeader) == 44, "the canonical float-WAV header is 44 bytes");

// The MIDI take's header. 32 bytes so the notes start cache-aligned, and
// version-tagged for the same reason the WAV is format-tagged: a file that
// outlives its writer has to say what it is.
struct MidiTakeHeader {
    u32 magic;             // kMidiTakeMagic
    u32 version;           // kTakeFormatVersion
    i64 count;             // notes that follow
    f64 startBeat;         // the take's beat zero, for the record
    u32 flags;
    u32 pad;
};
static_assert(sizeof(MidiTakeHeader) == 32);
inline constexpr u32 kMidiTakeMagic = 0x4b54584eu;   // "NXTK", little-endian

// Writes `bytes` to `path` via `path.part`, then renames. The rename is the
// publication: a reader either sees nothing or sees the whole take.
inline bool takeWriteAtomic(const char* path, const void* a, size_t aBytes,
                            const void* b, size_t bBytes) {
    // Wider than any path this header composes plus ".part", so the compose
    // cannot truncate — a truncated temporary name renames over the wrong file.
    char tmp[768];
    std::snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE* f = std::fopen(tmp, "wb");
    if (!f) return false;
    bool ok = true;
    if (aBytes && std::fwrite(a, 1, aBytes, f) != aBytes) ok = false;
    if (ok && bBytes && std::fwrite(b, 1, bBytes, f) != bBytes) ok = false;
    if (std::fflush(f) != 0) ok = false;
    // fsync before the rename. Not for the crash-consistency folklore — for the
    // one case this header exists to serve: the writing process dying between
    // the rename and the page cache being written back would otherwise leave a
    // name pointing at nothing, which is worse than no name at all.
    if (ok && ::fsync(::fileno(f)) != 0) ok = false;
    if (std::fclose(f) != 0) ok = false;
    if (!ok || ::rename(tmp, path) != 0) { ::unlink(tmp); return false; }
    return true;
}

inline bool writeAudioTake(const char* path, const f32* interleaved, i64 frames,
                           int channels, f64 rate) {
    if (!path || frames < 0 || channels <= 0) return false;
    const u64 bytes = (u64)frames * (u64)channels * 4ull;
    if (bytes > kMaxTakeBytes) return false;
    WavHeader h{};
    std::memcpy(h.riff,   "RIFF", 4);
    std::memcpy(h.wave,   "WAVE", 4);
    std::memcpy(h.fmt,    "fmt ", 4);
    std::memcpy(h.data,   "data", 4);
    h.fmtSize    = 16;
    h.format     = 3;
    h.channels   = (u16)channels;
    h.rate       = (u32)(rate > 0.0 ? rate : 48000.0);
    h.blockAlign = (u16)(channels * 4);
    h.byteRate   = h.rate * h.blockAlign;
    h.bits       = 32;
    h.dataSize   = (u32)bytes;
    h.riffSize   = (u32)(36ull + bytes);
    return takeWriteAtomic(path, &h, sizeof h, interleaved, (size_t)bytes);
}

inline bool writeMidiTake(const char* path, const WireNote* notes, i64 count,
                          f64 startBeat) {
    if (!path || count < 0) return false;
    const u64 bytes = (u64)count * sizeof(WireNote);
    if (bytes > kMaxTakeBytes) return false;
    MidiTakeHeader h{};
    h.magic     = kMidiTakeMagic;
    h.version   = kTakeFormatVersion;
    h.count     = count;
    h.startBeat = startBeat;
    return takeWriteAtomic(path, &h, sizeof h, notes, (size_t)bytes);
}

// Reads a take written by writeAudioTake back into a caller-owned buffer.
// Returns the frames copied, or -1 if the file is missing or is not a float WAV
// this build wrote. NEVER a partial success answered as a success: a take read
// short is the failure §5.4 refuses.
//
// Chunk-walking rather than assuming the 44-byte header, because a file that
// has been through another tool (promoted, trimmed, round-tripped) is still a
// take and there is no reason to refuse it for having a LIST chunk.
inline i64 readAudioTake(const char* path, f32* out, i64 maxFrames,
                         int* channelsOut, f64* rateOut) {
    if (!path || !out || maxFrames <= 0) return -1;
    FILE* f = std::fopen(path, "rb");
    if (!f) return -1;
    char riff[12];
    if (std::fread(riff, 1, 12, f) != 12 ||
        std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) {
        std::fclose(f);
        return -1;
    }
    u16 format = 0, channels = 0, bits = 0;
    u32 rate = 0;
    i64 frames = -1;
    char id[4];
    u32 size = 0;
    while (std::fread(id, 1, 4, f) == 4 && std::fread(&size, 4, 1, f) == 1) {
        const long next = std::ftell(f) + (long)size + (long)(size & 1u);
        if (!std::memcmp(id, "fmt ", 4) && size >= 16) {
            u16 fmt16[8];
            if (std::fread(fmt16, 2, 8, f) != 8) break;
            format   = fmt16[0];
            channels = fmt16[1];
            std::memcpy(&rate, &fmt16[2], 4);
            bits     = fmt16[7];
        } else if (!std::memcmp(id, "data", 4)) {
            if (format != 3 || bits != 32 || channels == 0) break;
            const u64 avail = (u64)size / (4ull * channels);
            const i64 want  = (i64)(avail < (u64)maxFrames ? avail : (u64)maxFrames);
            const size_t n  = (size_t)want * channels;
            frames = (n && std::fread(out, sizeof(f32), n, f) != n) ? -1 : want;
            break;
        }
        if (std::fseek(f, next, SEEK_SET) != 0) break;
    }
    std::fclose(f);
    if (frames >= 0) {
        if (channelsOut) *channelsOut = (int)channels;
        if (rateOut)     *rateOut     = (f64)rate;
    }
    return frames;
}

// The MIDI twin. Returns notes copied, or -1.
inline i64 readMidiTake(const char* path, WireNote* out, i64 maxNotes,
                        f64* startBeatOut = nullptr) {
    if (!path || !out || maxNotes < 0) return -1;
    FILE* f = std::fopen(path, "rb");
    if (!f) return -1;
    MidiTakeHeader h{};
    if (std::fread(&h, sizeof h, 1, f) != 1 || h.magic != kMidiTakeMagic ||
        h.version != kTakeFormatVersion || h.count < 0) {
        std::fclose(f);
        return -1;
    }
    const i64 want = h.count < maxNotes ? h.count : maxNotes;
    i64 got = want;
    if (want && std::fread(out, sizeof(WireNote), (size_t)want, f) != (size_t)want) got = -1;
    std::fclose(f);
    if (got >= 0 && startBeatOut) *startBeatOut = h.startBeat;
    return got;
}

inline bool removeTake(const char* path) {
    return path && *path && ::unlink(path) == 0;
}

// Unlinks every take file in `dir`, including the `.part` of a write that never
// finished. Returns how many went. This is the daemon's reclaim: a client that
// died holding takes leaves files nobody will ever claim, and a session that
// starts by sweeping its own directory cannot accumulate them across runs.
//
// Deliberately name-checked rather than "everything in the directory": this
// unlinks files, and a directory that is not what we think it is must cost
// nothing.
inline u32 sweepTakes(const char* dir) {
    if (!dir || !*dir) return 0;
    DIR* d = ::opendir(dir);
    if (!d) return 0;
    u32 n = 0;
    while (dirent* e = ::readdir(d)) {
        const char* dot = std::strrchr(e->d_name, '.');
        if (!dot) continue;
        if (std::strcmp(dot, ".wav") && std::strcmp(dot, ".ntk") && std::strcmp(dot, ".part"))
            continue;
        char p[640];
        std::snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (::unlink(p) == 0) ++n;
    }
    ::closedir(d);
    return n;
}

} // namespace lat::ipc
