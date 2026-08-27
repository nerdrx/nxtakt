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
// does", as v5 leaves it -- FIVE rungs, one appended and one widened:
//
//   1  the in-memory store                  find(); never reads a file
//   2  factoryDir()/<hash>.nxwt             a preset's tables
//   3  drawnDir()/<hash>.nxwt               v5. the user's authored library
//   4  userCacheDir()/<hash>.nxwt           imports cached here
//   5  recovery from `path`                 .nxwt -> readNxwt, else importFile
//
// A RUNG THAT FAILS FOR ANY REASON FALLS THROUGH TO THE NEXT, and only the
// exhaustion of all five is a refusal -- which is the amber the caller renders.
// A .nxwt whose bytes do not fold to its own name is refused by readNxwt and is
// therefore a rung that failed, not a table that plays wrongly.
//
// Rung 5's one exception to "fall through": the two arms do not fall through to
// EACH OTHER. A path ending `.nxwt` is read as a .nxwt or not at all.
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

// v5. THE USER'S AUTHORED LIBRARY, and it is not a cache.
//
//   $XDG_DATA_HOME/nxtakt/drawn  ->  ~/.local/share/nxtakt/drawn  ->  ...
//
// The same ladder userCacheDir() walks, ending in the same /tmp, because this
// tree has one answer to "where does nxtakt keep a user file" -- and a SIBLING
// of `wavetables/`, never a child of it. That is the whole point of the path:
// the gesture that clears the cache names `.../nxtakt/wavetables`, and a drawn
// library inside it would be swept up by every correct spelling of "clear the
// wavetable cache". `.../nxtakt/drawn` cannot be reached by any of them.
//
// Rung 3 of the resolution ladder, ABOVE the cache, because rungs 2, 3 and 4
// are hash-keyed and therefore interchangeable when they hit -- so their order
// can only be about which copy wins on a machine that holds two, and the
// answer is the one the user authored. Created lazily, on the first commit; a
// user who never draws never has the directory.
std::string drawnDir();

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

// v5. THE COMMIT'S ONE WRITE, and the only writer of drawnDir(). Creates the
// directory lazily on the first commit, at 0755 (learn.cpp's ensureParentDir
// discipline) -- a user who never draws never has it -- then writes
// `drawnDir()/<hashHex(t.hash)>.nxwt` through writeNxwt, which is a temporary
// in the SAME DIRECTORY followed by rename(). A crash mid-commit therefore
// cannot leave a half-written table under a name that claims a hash, and even
// if it could, readNxwt recomputes the fold and refuses a file whose bytes do
// not name themselves.
//
// A FILE ALREADY THERE IS LEFT ALONE and true is returned: the filename IS the
// content hash, so the same drawing writes the same bytes to the same name. A
// drawn table is never overwritten and never has to be -- which is the property
// content-addressing was always going to buy.
//
// There is no `.bak` generation for the same reason. `outPath` receives the
// full path either way, because the editor has to be able to tell the user
// where the one irreplaceable copy of their work landed.
bool writeDrawn(const Table& t, std::string& outPath);

// ---------------------------------------------------------------------------
// v5: THE TRANSFORM, and why it moved here
//
// These three were spFft / spIfft / spTwiddle in spectra_tables.inc, where the
// mip builder is. They are HERE now, unchanged in text and in arithmetic, and
// the seam calls them through this header -- because v5's pen needs the same
// transform the mip builder uses and the alternative was a THIRD radix-2 FFT in
// this tree (src/audio/sample.cpp has one, the mip builder had one). The v5
// implementation notes ask for exactly this: "the fill and the render share one
// transform and one set of tables. A second FFT in this device would be a
// second thing to keep in agreement."
//
// The move is arithmetically NEUTRAL and the release gate is what says so: 1500
// reference renders across three rates and four block sizes, cmp-identical.
// It is a move and not a copy; nothing calls a second one.
//
// `tw` holds cos/sin at -2*pi*i/n for i in [0, n/2), so the inner loop does no
// trigonometry. `n` must be a power of two and `tw` must have been filled by
// twiddle() for that same n.
// ---------------------------------------------------------------------------

void fft (f32* re, f32* im, int n, const f32* tw);
void ifft(f32* re, f32* im, int n, const f32* tw);
void twiddle(std::vector<f32>& t, int n);

// ---------------------------------------------------------------------------
// v5: THE PEN -- gesture to frames
//
// docs/SPECTRA-PARAMS.md, "v5 -- the wavetable editor (FROZEN)", "The two
// pens", "Frames" and "Commit". Everything here works on a WORKING COPY that
// the editor owns: `kMaxFrames * kCycle` floats, frame-major, the identical
// layout Table::data has. Nothing here touches the store, a file, a hash or a
// mip -- the pen never sees a mip -- and nothing here is reachable from the
// audio thread or from the daemon.
//
// WHERE THE LIBM IS, stated once because it is the determinism obligation.
// Two calls: `10^(dB/20)` in magFromDb() and the std::sin/std::cos inside
// twiddle(). The second adds nothing -- the mip builder already makes exactly
// those calls for every table in the instrument. The first is new, is
// deliberate, and sits precisely where the import path's rules (b) and (c) put
// theirs: UPSTREAM of the content hash. The bound is the same one and is
// weaker, because a GESTURE is never replayed on a second machine -- what
// travels is the frames. No machine ever renders two different tables under
// one hash.
//
// NO WALL CLOCK AND NO RNG. A drawn table's identity is a function of its
// samples and of nothing else. The preview interval reads a clock; a preview is
// not identity.
// ---------------------------------------------------------------------------
namespace pen {

// A 2048-point cycle carries harmonics 1..1023 and a Nyquist bin. 1023 is
// kSpMaxHarm, which is what the mip builder keeps, so the pen's spectrum and
// the render's spectrum have exactly the same extent.
inline constexpr int kMaxHarm = 1023;

// The bars a human can address. A bar per harmonic past 256 is under a pixel on
// any canvas anyone will build, and a control the user cannot hit is not a
// control. 257..1023 EXIST, are not editable, and are PRESERVED.
inline constexpr int kEditHarm = 256;

// Bar top is 0 dB = magnitude 1.0 -- a full-scale sine at that harmonic under
// the 2/N analysis scaling the mip builder already uses. Bar floor is -80 dB,
// and the floor is a HARD ZERO rather than -80 dB: "drag it away" must mean the
// harmonic is gone, not that it is quiet.
inline constexpr f32 kFloorDb = -80.f;

// The commit's silence test, and it is the SAME CONSTANT the import path uses,
// so there is one number. Import maps a peak of zero to a gain of 1 and carries
// on because it is recovering someone else's file; a drawing of silence is a
// mistake, and letting it through would burn an identity on silence forever.
inline constexpr f32 kSilent = 1e-9f;

// ------------------------------------------------------------------ waveform

// x in 0..1 to a sample index: clamp(round(x * 2047), 0, 2047). The pen's whole
// domain rule in one function so that the UI and the tests cannot disagree.
int index(f32 xNorm);

// ONE SEGMENT OF A STROKE, between two consecutively delivered points. Every
// index strictly between i0 and i1 gets the linear interpolant; i1 == i0
// overwrites with v1; a stroke that reverses direction writes twice and the
// later write wins. Values are clamped to +/-1 -- the pen cannot draw past full
// scale -- and indices are clamped to 0..2047.
//
// LINEAR AND NOT A SPLINE, and it is a decision rather than laziness: a spline
// overshoots, an overshoot is a sample the user did not draw, and the pen must
// be able to draw a hard vertical step (a pulse edge), which no interpolating
// spline can express.
//
// A STROKE DOES NOT WRAP. From i=400 to i=900 changes 501 samples and nothing
// else. The cycle is a ring to the oscillator and a LINE to the pen: a
// discontinuity at the wrap is a legitimate waveform -- it is what a sawtooth
// IS -- so there is no wrap-continuity rule and no attempt to close the curve.
//
// A line tool (shift-drag) is this same call with the two endpoints.
void stroke(f32* frame, int i0, f32 v0, int i1, f32 v1);

// DC removal, applied at STROKE END and not during the stroke and not at
// commit: during, the curve would crawl under the cursor; at commit, the last
// thing the user saw would not be the thing that got saved. The user watches
// the curve slide vertically the moment the pointer lifts.
//
// The mean accumulates in f64 in ASCENDING INDEX ORDER and is subtracted in f32
// in ascending index order. Fixed accumulation type and fixed order, so the
// same drawing gives the same frame on every machine.
void removeDc(f32* frame);

// ------------------------------------------------------------------ harmonic

// One frame's spectrum in the MIP BUILDER'S OWN CONVENTION, so that the pen and
// the render describe a harmonic with the same two numbers: hr[h] is the cosine
// coefficient of harmonic h and hi[h] is MINUS its sine coefficient, both
// already scaled by the 2/N the builder applies. h = 0 is DC and is always
// zero; the Nyquist bin is not represented, exactly as kSpMaxHarm says.
//
// 1024 complex f32 -- 8 KiB -- and the editor holds ONE, for the cursor frame.
struct Spectrum {
    f32 hr[kMaxHarm + 1] = {};
    f32 hi[kMaxHarm + 1] = {};
};

// THE FORWARD ANALYSIS. Reads a frame, writes the spectrum, and MODIFIES
// NOTHING: opening the harmonic view never touches the frame, so the round trip
// waveform -> harmonic view -> waveform with no bar touched is the identity BY
// CONSTRUCTION and not by numerical luck. An f32 FFT/IFFT pair is not bit-exact
// and this is what makes that fact irrelevant.
void analyse(const f32* frame, Spectrum& out);

// |H_h| for h in 0..kMaxHarm, and 0 outside. This is what a bar displays.
f32 magnitude(const Spectrum& s, int h);

// The dB<->magnitude map, and magFromDb is THE ONE LIBM CALL THIS FEATURE ADDS.
//
//   m = (db <= kFloorDb) ? 0.0f : 10^(db/20)
//
// The floor is a hard zero rather than -80 dB because a -80 dB residue on 256
// harmonics is a table with a floor of hiss in it that no gesture can remove.
// No harmonic may exceed 0 dB: the clamp exists so that the set normalisation
// at commit is a correction and not a rescue.
f32 magFromDb(f32 db);
f32 dbFromMag(f32 m);              // kFloorDb for m <= 0; the display inverse.

// TOUCHING A BAR rewrites that harmonic's magnitude AND its phase, and leaves
// every untouched harmonic's COMPLEX value alone. The touched harmonic carries
// (m, sine phase): hr = 0, hi = -m.
//
// `h` outside 1..kEditHarm is ignored -- DC is not editable and is forced to
// zero, and 257..1023 are not editable and are preserved.
void setBar(Spectrum& s, int h, f32 mag);

// Rewrites EVERY harmonic to sine phase with its magnitude untouched, over the
// full 1..kMaxHarm. This is "Re-phase endpoints"' arithmetic; it is never
// applied silently and never by morph().
void toSinePhase(Spectrum& s);

// THE INVERSE SYNTHESIS, over the full 0..1023 spectrum, DC forced to zero.
// Exactly the reconstruction spBuildFrameMips performs at n = kCycle, so the
// pen writes what the render would have read.
//
// N CONSECUTIVE BAR EDITS PERFORM EXACTLY ONE FORWARD ANALYSIS AND N INVERSE
// SYNTHESES, and that is a gate rather than an optimisation: the editor holds
// the analysed Spectrum for as long as the frame has not been edited in the
// waveform domain, and each bar edit calls setBar() then synthesise() on the
// HELD spectrum rather than re-analysing the frame it just synthesised. Without
// that, fifty bar edits are fifty FFT round trips of accumulated f32 error, and
// the drift is audible before it is visible. A waveform-domain edit invalidates
// the held spectrum; the next open re-analyses.
void synthesise(const Spectrum& s, f32* frame);

// ------------------------------------------------------------------ the table

// MORPH, and it is the operation that makes 32 frames authorable. Replaces
// frames a+1 .. b-1 and does not touch a or b.
//
// THE DOMAIN IS HARMONIC: magnitudes are interpolated per harmonic over
// 1..kMaxHarm -- not 1..256, because the fill is not the pen and has no screen
// to fit in -- and the result is synthesised at the pen's SINE PHASE. Ascending
// in h, ascending in k, f32 throughout, so the fill is reproducible.
//
// A time-domain fill is exactly what `A Position` already computes between
// adjacent frames, so it would write thirty frames that sound like having drawn
// two: a no-op you can hear. And there is no third domain -- interpolating the
// COMPLEX spectrum is, by the linearity of the transform, the time-domain
// crossfade written more expensively.
//
// THE HONEST COST: the fill is at sine phase and the endpoints keep whatever
// phase they were drawn with, so an endpoint that is not already in sine phase
// leaves a PHASE STEP at the a/a+1 or b-1/b boundary, audible as a click in the
// morph at exactly that position. rephaseEndpoints() is the named fix and it is
// never applied here.
//
// False (and nothing written) unless 0 <= a and a + 1 < b < kMaxFrames.
bool morph(f32* frames, int a, int b);

// RE-PHASE ENDPOINTS. Rewrites frames a and b to sine phase with their
// magnitudes untouched. A named, user-initiated operation: a tool that quietly
// rewrites the two frames the user actually drew is a tool the user stops
// trusting, so morph() calls this on nothing and the editor offers it in a
// line. Idempotent -- re-phasing a sine-phase frame is the same frame to the
// precision of the transform pair.
//
// False unless both indices are in range and distinct.
bool rephaseEndpoints(f32* frames, int a, int b);

// FRAME OPERATIONS, on a fixed array of kMaxFrames. Insert and Delete are
// DESTRUCTIVE AT ONE END and the contract says so: there is no way to spell
// "insert" in a fixed-length array that does not lose something, and the
// alternative -- refusing when frame 31 is non-zero -- is a tool that stops
// working the moment the table is full, which is always.
void clearFrame(f32* frames, int k);        // 2048 zeros
void insertFrame(f32* frames, int k);       // copy AT k, tail down, DROPS 31
void duplicateFrame(f32* frames, int k);    // copy into k+1, tail down, DROPS 31
void deleteFrame(f32* frames, int k);       // remove k, tail up, DUPLICATES the
                                            // new last frame into slot 31

// THE FRAME-AXIS STRETCH. An imported table may have 1..32 source frames and
// the editor always edits 32, so opening one stretches it first -- by the same
// LINEAR frame-axis interpolation spBuildCustomMips() already performs, cited
// and not re-derived. A one-frame import becomes 32 identical frames, which is
// what a table with no frame axis is. Committing then yields a DIFFERENT HASH
// from the original, which is correct: the content differs, and identity is
// content.
//
// `src` is srcFrames * kCycle; `dst` is kMaxFrames * kCycle. May not alias.
void stretchFrames(const f32* src, int srcFrames, f32* dst);

// COMMIT STEPS 1..5, in order, on the editor's working copy, leaving the frames
// in exactly the state v3's identity rule names -- so that contentHash() may be
// taken over them with no amendment:
//
//   1. every sample must be finite (v3's rule, applying to the pen unchanged,
//      so a NaN payload cannot become an identity by a second door);
//   2. per-frame DC removal, ascending frame then ascending index, f64 mean
//      subtracted in f32 -- normally a no-op, and it runs anyway because a
//      commit must not depend on which pen last touched a frame;
//   3. the set-wide peak, ascending;
//   4. a peak <= kSilent REFUSES with "this table is silent";
//   5. multiply every sample by 1/pk, ascending.
//
// NORMALISATION IS ONE SET-WIDE FACTOR and never per-frame or per-stroke: a
// single scalar over all 32 frames changes no shape and no inter-frame
// relationship. Nothing the user drew moves at commit.
//
// False with a SENTENCE in `err` and `frames` UNTOUCHED on a refusal.
bool canonicalise(f32* frames, std::string& err);

// ------------------------------------------------------------------- preview

// The number of buffers in a per-oscillator PREVIEW ARENA. Four, and the
// arithmetic below is why.
inline constexpr int kPreviewRing = 4;

// THE MINIMUM PREVIEW INTERVAL, in seconds, and it is ONE NUMBER IN ONE PLACE
// so that the seam, the editor and the tests cannot disagree about it:
//
//     interval = max(50 ms, 2 * maxBlock / sampleRate)
//
// computed from the values prepare() was given rather than hard-coded, because
// the recycle bound has to hold at every rate and block size a host can choose.
//
// THE BOUND, which is arithmetic and not timing luck. The audio thread takes a
// base pointer at the top of a block and does not retain it past that block,
// and a ring of kPreviewRing is not rewritten until kPreviewRing - 1 further
// publishes have happened. So the protection is
//
//     kPreviewRing * interval  >=  4 * 2 * maxBlock / sampleRate
//                              =   8 * (maxBlock / sampleRate)
//                              >   maxBlock / sampleRate
//
// -- a factor of eight at the large-block end, where the 50 ms floor is not
// what binds, and far more at the small-block end where it is. At 4096 frames
// and 44.1 kHz: a 92.9 ms block against 4 x 185.8 ms of protection.
//
// The preview is rate-limited to this and published on STROKE END, not per
// pointer motion: a user does not lift the pointer sixty times a second. The
// limit is a FLOOR, not a schedule -- there is no timer, and a slow drawer
// publishes once per stroke.
f64 previewInterval(f64 sampleRate, int maxBlock);

} // namespace pen

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
