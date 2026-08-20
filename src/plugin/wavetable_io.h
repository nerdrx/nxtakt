// Custom wavetables: import, identity, cache, and the process-wide store.
//
// This file owns a wavetable's BYTES AND ITS NAME, and deliberately nothing
// else. The mip chain a voice reads is built in src/plugin/spectra_tables.inc
// from what lands here, by the same builder the eight factory tables go
// through; mips are derived data and are never stored, never hashed and never
// sent (docs/SPECTRA-PARAMS.md, "The content hash").
//
// The split is what makes the daemon work. `nxtaktd` renders and does not
// decode: it never imports a file, it INGESTS frames that crossed the wire
// (ingest()) and resolves them by hash exactly as the GUI does. Everything in
// this header below importFile() is therefore reachable from a build with no
// decoder at all, which is every build there is -- see "THE DECODER" below.
//
// THREADING. GUI thread in the GUI, pump thread in the daemon -- i.e. the one
// non-realtime thread that owns the device. Every function here allocates and
// several do file I/O. NOTHING HERE IS EVER REACHED FROM THE AUDIO THREAD: the
// audio thread reads a `const f32*` into a mip chain the seam built, and the
// store's own mutex exists only so that a second non-realtime thread (a test's
// second client, a future loader thread) cannot corrupt the table list.
//
// THE DECODER, and why this file does NOT use the weak reference sampler.cpp
// uses. The precedent is real (src/audio/sample.cpp is absent from nxtaktd,
// plugin_scan, internal_device_test, timesig_view_test and handle_test, so the
// sampler declares `loadSample` weak and checks the pointer), and it is the
// wrong tool for THIS job for three reasons, in increasing order of how much
// they matter:
//
//   1. `loadSample(path, engineRate)` RESAMPLES to the engine rate. A wavetable
//      is not audio at a rate, it is N cycles of exactly C samples each, and
//      a 44.1k file dragged to 48k is a file whose 2048-sample cycles are no
//      longer 2048 samples long. Every interpretation rule below would miss.
//   2. That resample is libsamplerate's. The identity of a custom table is a
//      hash of its frames, so a decode path through a third-party resampler
//      makes a table's NAME depend on which libsamplerate the machine has.
//      The hash has to be a function of the file and of this repository.
//   3. Import would then be untestable in four of the five link targets that
//      contain Spectra, including all three suites this wave adds tests to.
//
// So the WAV reader is here, it is about a hundred lines of RIFF, it links
// nowhere and it produces the same floats on every machine. It reads WAV only
// -- 8/16/24/32-bit PCM and 32/64-bit float -- which is what "import a WAV"
// asked for; a later format goes through a decoder that answers "give me the
// file's own samples at the file's own rate", which is not a call src/audio
// offers today.
#pragma once
#include "../core/common.h"
#include <string>
#include <vector>

namespace lat {
namespace wt {

// A frame is one cycle of exactly this many samples, everywhere: in the store,
// in the cache file, on the wire and in the hash. It is a constant in v3 and
// the hash folds it anyway, so the day it is not, old identities still mean
// what they meant.
inline constexpr int kCycle = 2048;

// The most frames a table can hold, and it is not a policy number: it is
// kSpFrames from spectra_tables.inc, because a custom table has the IDENTICAL
// memory layout to a factory one (32 frames x the factory stride) so that the
// audio-rate read changes by nothing but a base pointer. A source with more
// frames than this is decimated at import, BEFORE the hash, so that the hash
// names exactly what plays. See importFile().
inline constexpr int kMaxFrames = 32;

// What a source file may hold before this file stops believing it is a
// wavetable. 256 is Serum's own cap on a table; a cycle outside 256..4096 is
// not a wavetable cycle by any convention this reads.
inline constexpr int kMaxSrcFrames = 256;
inline constexpr int kMinSrcCycle  = 256;
inline constexpr int kMaxSrcCycle  = 4096;

// The most distinct tables one process will hold. Refused past this with one
// log line rather than evicted, and that is the whole reason it is a refusal:
// resolve() and ingest() hand out `const Table*`, the seam copies through it,
// and an eviction policy would be a dangling pointer waiting for a caller that
// held one a moment too long. 256 tables is 64 MiB of worst case and roughly
// two hundred imports more than a session does.
inline constexpr int kMaxTables = 256;

// How a table was interpreted -- reported so the editor and the log can say
// which of the three rules fired, and asserted by the tests.
enum class Rule : int {
    None = 0,
    Serum,      // (a) N x 2048, sliced, never resampled
    Cycle,      // (b) N x C for a power-of-two C in 256..4096, resampled
    Detect,     // (c) autocorrelation pitch detect, sliced, resampled
};

const char* ruleName(Rule r);

// ONE IMPORTED WAVETABLE: identity, bytes, and the hint that recovers it.
//
// `data` is `frames * kCycle` floats, FRAME-MAJOR -- frame 0's 2048 samples,
// then frame 1's -- which is the order the hash folds and the order the wire
// carries. DC-removed per frame and normalised as a SET (one factor for every
// frame, because inter-frame level is musical), which is the state the hash is
// taken over and the state the mip builder starts from.
struct Table {
    u64              hash   = 0;
    int              frames = 0;
    Rule             rule   = Rule::None;
    int              srcFrames = 0;      // before the kMaxFrames decimation
    int              srcCycle  = 0;      // the cycle length found in the file
    std::string      path;               // recovery hint; "" for a table that
                                         // arrived by hash alone (the wire, a
                                         // factory .nxwt, a cache hit)
    std::vector<f32> data;

    const f32* frame(int i) const { return data.data() + (size_t)i * (size_t)kCycle; }
};

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

// docs/SPECTRA-PARAMS.md, "The content hash", transcribed. A splitmix64 fold
// over the frame count, the cycle length and every sample's float32 VALUE --
// not its bytes, so the number is the same on a big-endian machine -- with
// -0.0 folded to +0.0 because DC removal produces it and two tables that
// differ only in the sign of a zero are the same table.
//
// Returns 0 for a refused input (no frames, or a non-finite sample), and 0 is
// therefore never a valid identity, which is what lets every other API here
// use 0 for "none".
u64 contentHash(const f32* frames, int frameCount);

// 16 lowercase hex digits, zero-padded, and its strict inverse: exactly 16
// characters from [0-9a-f], nothing else, no shorter and no longer.
std::string hashHex(u64 h);
bool        hashFromHex(const char* s, size_t n, u64& out);

// ---------------------------------------------------------------------------
// Import -- the one entry point that reads a file the user chose
// ---------------------------------------------------------------------------

// Reads `path`, applies the three interpretation rules in order, and fills
// `out`. Returns false with a SENTENCE in `err` on any failure -- the string
// is shown to a human, so it says what was wrong with the file rather than
// which function returned false.
//
// On success `out.hash` is set and `out.path` is `path`. The table is NOT
// entered into the store and NOT written to the cache; adopt() does both, so
// that a caller can inspect an import before it takes it.
bool importFile(const std::string& path, Table& out, std::string& err);

// ---------------------------------------------------------------------------
// The store -- process-wide, keyed by content hash
// ---------------------------------------------------------------------------

// A pointer into the store is STABLE FOR THE LIFE OF THE PROCESS. Tables are
// held behind unique_ptrs in a vector that only ever grows, and nothing is
// evicted (see kMaxTables). The seam copies out of one on the GUI thread and
// keeps a mip chain of its own; nothing retains one across a block.

// Already here? Null if not. Never reads a file.
const Table* find(u64 hash);

// Take a table the caller built (importFile) into the store, writing the user
// cache as a side effect unless `cache` is false. Returns the stored copy, or
// null if the store is full. If the hash is already present the EXISTING entry
// is returned unchanged -- identity is content, so two imports of the same
// bytes are one table.
const Table* adopt(const Table& t, bool cache = true);

// Adopt frames that arrived from somewhere that is not a file: the wire. The
// hash is RECOMPUTED from the frames and must equal `hash`, because a peer
// naming a table is a peer that can name it wrongly, and a table filed under a
// hash it does not have is a table that answers the wrong question forever.
// Null on any disagreement.
//
// Does NOT write the user cache: the daemon is not the machine the user
// imports on, and a cache written from the wire would be a directory that
// grows on a headless render box for no one's benefit.
const Table* ingest(u64 hash, int frames, const f32* data);

// THE RESOLUTION ORDER, docs/SPECTRA-PARAMS.md "What an unresolvable slot 8
// does": the store, then the installed factory wavetable directory, then the
// user cache, then a re-import from `path` if one is given. Null when every
// one of those fails, which is the refusal the caller renders amber over.
//
// `path` may be null or empty -- a preset names a hash and never a path.
const Table* resolve(u64 hash, const char* path);

// How many tables the store holds. Diagnostics and tests.
int storeSize();

// ---------------------------------------------------------------------------
// Directories and the cache file
// ---------------------------------------------------------------------------

// $XDG_DATA_HOME/nxtakt/wavetables, else $HOME/.local/share/..., else the
// passwd entry's home, else /tmp -- the ladder src/control/learn.cpp walks for
// $XDG_CONFIG_HOME, because this tree has one answer to "where does nxtakt
// keep a user file" and a wavetable is DATA, not config.
std::string userCacheDir();

// $NXTAKT_WAVETABLES, else <the directory of this executable>/wavetables, else
// <that>/../share/nxtakt/wavetables. Where a factory table a preset names
// lives; read-only as far as this file is concerned.
std::string factoryDir();

// The cache file. Boring on purpose -- it is a cache, not an interchange
// format: a 24-byte header and the frames, little-endian f32. Mips are not in
// it and never will be; they are derived and they rebuild at load.
//
//   char magic[4]  "NXWT"
//   u32  version   kNxwtVersion
//   u32  frames    1..kMaxFrames
//   u32  cycle     kCycle
//   u64  hash      the content hash, which readNxwt RECOMPUTES and compares
//
inline constexpr u32 kNxwtVersion  = 1;
inline constexpr int kNxwtHeaderBytes = 24;

bool writeNxwt(const std::string& file, const Table& t);
bool readNxwt(const std::string& file, Table& out);

// ---------------------------------------------------------------------------
// The wire's discovery half
// ---------------------------------------------------------------------------

// Every content hash a DEVICE STATE names, so that src/ui/engine_handle.cpp
// can put the tables in the pool beside the state without knowing what device
// wrote it.
//
// GENERIC BY CONSTRUCTION, and bounded by three things at once so that it
// cannot mean anything to a device that did not intend it: the caller only
// asks for devices that answer `PluginInstance::wavetable()` non-null; a
// record qualifies only if its key begins `wt` and its value is EXACTLY 16
// lowercase hex digits; and a hash that qualifies is still only shipped if the
// store actually holds it. A device that grows a `wtfoo=<16 hex>` record
// meaning something else costs one wasted lookup and nothing more.
//
// Writes at most `max` hashes, in the order they appear, with duplicates
// dropped. Returns how many were written.
int hashesInDeviceState(const char* state, u64* out, int max);

} // namespace wt
} // namespace lat
