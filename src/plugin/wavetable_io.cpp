// Custom wavetables: the WAV reader, the three interpretation rules, the
// content hash, the .nxwt cache and the process-wide store.
//
// Read wavetable_io.h first: it says what this file owns, what it deliberately
// does not (the mip chain), and why it carries its own RIFF reader instead of
// the weak `loadSample` reference sampler.cpp uses.
//
// DETERMINISM, stated once because everything below serves it. A table's
// identity is a hash of its frames, so every arithmetic step between a file and
// those frames has to be a function of this repository and of nothing else:
//
//   * the WAV reader converts integer PCM by an exact power-of-two divide, so
//     a 16-bit file yields the same floats on every machine and every build;
//   * rule (a) -- the Serum convention, and the common case -- does no
//     arithmetic on the samples at all beyond the mono fold, the per-frame DC
//     removal and one set-wide multiply, so an N x 2048 file has the SAME
//     identity everywhere, forever;
//   * rules (b) and (c) resample through the windowed sinc below, which calls
//     std::sin. That is a libm dependence in the identity path and it is worth
//     naming: two machines with different libm versions can, in principle,
//     import the same non-2048 file to two different hashes. It costs nothing
//     that matters -- a table is resolved by hash from the cache, the factory
//     directory or the wire long before a re-import is reached, and the path
//     record is what recovers a table whose hash a machine cannot reproduce.
//     The alternative (a hand-rolled sine) would buy cross-machine identity for
//     the rarer half of the import rules and cost a second sine in a tree that
//     already builds its factory tables with std::sin.
#include "wavetable_io.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace lat {
namespace wt {
namespace {

// The cache file stores little-endian f32 and this tree has never run on a
// big-endian host. Stated as a compile error rather than a comment so that the
// day it does, the byte-swap lands here and not in a bug report about a
// wavetable that plays as noise. (The HASH is endian-independent by
// construction -- it folds float VALUES -- so only the FILE is affected.)
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "the .nxwt cache stores little-endian f32; add the swap here");

// ---------------------------------------------------------------------------
// splitmix64, the finaliser docs/SPECTRA-PARAMS.md names -- the one spectra.cpp
// already uses for the Random-per-note source. There is no second hash in this
// device and this is not a second one.
// ---------------------------------------------------------------------------
inline u64 mix64(u64 x) {
    x += 0x9E3779B97F4A7C15ull;
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27; x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return x;
}

// ---------------------------------------------------------------------------
// A minimal RIFF/WAVE reader
//
// PCM 8/16/24/32-bit and IEEE float 32/64, plus WAVE_FORMAT_EXTENSIBLE, which
// is what any modern tool writes above 16 bits. Chunks are walked rather than
// assumed to be in order, because a wavetable exported by a synth routinely
// carries a `clm ` or `LIST` chunk between `fmt ` and `data`.
//
// Everything is bounds-checked against the mapping and nothing is trusted: the
// file came from a user and this reader is the only thing between it and a
// hundred megabytes of allocation.
// ---------------------------------------------------------------------------

constexpr u64 kMaxWavBytes = 64ull << 20;      // a wavetable is kilobytes; 64 MiB
                                               // is four orders of margin and a
                                               // bound a bad file cannot pass.

inline u32 rd32(const u8* p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
inline u16 rd16(const u8* p) { return (u16)((u32)p[0] | ((u32)p[1] << 8)); }

// One decoded WAV, folded to MONO on the way out.
//
// MID-ONLY, AND IT IS A CHOICE the plan asked to have stated: a stereo import
// becomes (L + R + ...) / channels. Serum's per-channel table pairs are a real
// feature and they are twice the memory, twice the wire, twice the mip build
// and a second base pointer in the audio-rate read -- none of which falls out
// free, so v3 imports the mid and says so.
struct Wav {
    std::vector<f32> mono;
    int  rate = 0;
};

bool readWav(const std::string& path, Wav& out, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open " + path; return false; }
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); err = "cannot size " + path; return false; }
    const long end = std::ftell(f);
    if (end <= 0) { std::fclose(f); err = path + " is empty"; return false; }
    if ((u64)end > kMaxWavBytes) {
        std::fclose(f);
        err = path + " is larger than a wavetable can be (64 MiB)";
        return false;
    }
    std::vector<u8> buf((size_t)end);
    std::rewind(f);
    const size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (got != buf.size()) { err = "short read on " + path; return false; }

    const u8* b = buf.data();
    const size_t n = buf.size();
    if (n < 12 || std::memcmp(b, "RIFF", 4) != 0 || std::memcmp(b + 8, "WAVE", 4) != 0) {
        err = path + " is not a RIFF/WAVE file";
        return false;
    }

    u16 format = 0, channels = 0, bits = 0;
    u32 rate = 0;
    const u8* data = nullptr;
    u64 dataBytes = 0;

    size_t at = 12;
    while (at + 8 <= n) {
        const u32 id0 = rd32(b + at);
        u64 sz = rd32(b + at + 4);
        const size_t body = at + 8;
        if (sz > n - body) sz = n - body;          // a truncated tail is read short
        if (std::memcmp(&id0, "fmt ", 4) == 0 && sz >= 16) {
            format   = rd16(b + body + 0);
            channels = rd16(b + body + 2);
            rate     = rd32(b + body + 4);
            bits     = rd16(b + body + 14);
            // WAVE_FORMAT_EXTENSIBLE: the real format is the first two bytes of
            // the SubFormat GUID.
            if (format == 0xFFFE && sz >= 40) format = rd16(b + body + 24);
        } else if (std::memcmp(&id0, "data", 4) == 0) {
            data = b + body;
            dataBytes = sz;
        }
        at = body + (size_t)sz + ((size_t)sz & 1u);   // chunks are word-aligned
    }

    if (!data || dataBytes == 0) { err = path + " has no data chunk"; return false; }
    if (channels < 1 || channels > 32) { err = path + " has an impossible channel count"; return false; }
    if (rate == 0) rate = 44100;
    if (format != 1 && format != 3) {
        err = path + " is compressed; import an uncompressed PCM or float WAV";
        return false;
    }
    const int bytesPerSample = format == 3 ? (bits == 64 ? 8 : 4) : (int)(bits / 8);
    if (format == 1 && bits != 8 && bits != 16 && bits != 24 && bits != 32) {
        err = path + " has a bit depth this reader does not know";
        return false;
    }
    if (format == 3 && bits != 32 && bits != 64) {
        err = path + " is float WAV at a width this reader does not know";
        return false;
    }
    const u64 stride = (u64)bytesPerSample * (u64)channels;
    const u64 frames = dataBytes / stride;
    if (frames == 0) { err = path + " holds no whole frames"; return false; }

    out.rate = (int)rate;
    out.mono.assign((size_t)frames, 0.f);
    const f64 chNorm = 1.0 / (f64)channels;
    for (u64 i = 0; i < frames; ++i) {
        const u8* p = data + i * stride;
        f64 acc = 0.0;
        for (int c = 0; c < channels; ++c, p += bytesPerSample) {
            f64 v = 0.0;
            switch (format == 3 ? 100 + bits : bits) {
                case 8:  v = ((f64)p[0] - 128.0) * (1.0 / 128.0); break;
                case 16: v = (f64)(i16)rd16(p) * (1.0 / 32768.0); break;
                case 24: {
                    i32 s = (i32)((u32)p[0] << 8 | (u32)p[1] << 16 | (u32)p[2] << 24);
                    v = (f64)(s >> 8) * (1.0 / 8388608.0);
                    break;
                }
                case 32: v = (f64)(i32)rd32(p) * (1.0 / 2147483648.0); break;
                case 132: { f32 s; std::memcpy(&s, p, 4); v = (f64)s; break; }
                case 164: { f64 s; std::memcpy(&s, p, 8); v = s; break; }
                default: break;
            }
            acc += v;
        }
        out.mono[(size_t)i] = (f32)(acc * chNorm);
    }
    return true;
}

// ---------------------------------------------------------------------------
// The windowed-sinc cycle resampler
//
// One periodic cycle of `n` samples in, one of exactly kCycle out. The taps
// wrap, because a wavetable frame IS periodic and a clamped edge would put a
// discontinuity into the one place a wavetable must not have one.
//
// The weights are normalised by their own sum, which makes DC exact at every
// ratio and removes the amplitude ripple a fixed-window sinc otherwise leaves
// across the output phase. Downsampling (n > kCycle) narrows the sinc to
// kCycle/n and widens the window by the same factor, which is the anti-alias
// filter the plan asked for.
// ---------------------------------------------------------------------------

constexpr int kSincZeros = 24;

inline f64 sincpi(f64 t) {
    if (t == 0.0) return 1.0;
    const f64 x = 3.14159265358979323846 * t;
    return std::sin(x) / x;
}

void resampleCycle(const f32* c, int n, f32* out) {
    if (n == kCycle) {
        std::memcpy(out, c, (size_t)kCycle * sizeof(f32));
        return;
    }
    const f64 ratio = n > kCycle ? (f64)kCycle / (f64)n : 1.0;
    const f64 half  = (f64)kSincZeros / ratio;
    const f64 step  = (f64)n / (f64)kCycle;
    for (int j = 0; j < kCycle; ++j) {
        const f64 x  = (f64)j * step;
        const long lo = (long)std::floor(x - half);
        const long hi = (long)std::ceil (x + half);
        f64 acc = 0.0, wsum = 0.0;
        for (long k = lo; k <= hi; ++k) {
            const f64 t = x - (f64)k;
            const f64 u = t / half;
            if (u <= -1.0 || u >= 1.0) continue;
            // Blackman, written as its cosine sum so the taper reaches exactly
            // zero at the window edge.
            const f64 pu = 3.14159265358979323846 * u;
            const f64 w  = 0.42 + 0.5 * std::cos(pu) + 0.08 * std::cos(2.0 * pu);
            const f64 g  = w * sincpi(ratio * t);
            long m = k % n;
            if (m < 0) m += n;
            acc  += g * (f64)c[m];
            wsum += g;
        }
        out[j] = (f32)(wsum != 0.0 ? acc / wsum : 0.0);
    }
}

// ---------------------------------------------------------------------------
// Rule (c): the period, by autocorrelation
//
// The cumulative-mean-normalised difference function -- YIN's step 3, which is
// what turns a raw autocorrelation into something that does not answer "one
// octave down" on every voiced signal. src/audio/sample.cpp's transient
// detector is the prior art the plan points at for the shape of this kind of
// pass; the function itself is different because the question is.
//
// Returns 0 when nothing periodic enough was found.
// ---------------------------------------------------------------------------

constexpr int kMinPeriod = 32;

int detectPeriod(const std::vector<f32>& x) {
    const int maxLag = (int)std::min<size_t>((size_t)kMaxSrcCycle, x.size() / 2);
    if (maxLag <= kMinPeriod) return 0;
    const int w = (int)std::min<size_t>((size_t)4096, x.size() - (size_t)maxLag);
    if (w < kMinPeriod * 2) return 0;

    std::vector<f64> d((size_t)maxLag + 1, 0.0);
    for (int tau = 1; tau <= maxLag; ++tau) {
        f64 s = 0.0;
        for (int i = 0; i < w; ++i) {
            const f64 v = (f64)x[(size_t)i] - (f64)x[(size_t)(i + tau)];
            s += v * v;
        }
        d[(size_t)tau] = s;
    }
    // d'(tau) = d(tau) / mean(d(1..tau))
    std::vector<f64> dn((size_t)maxLag + 1, 1.0);
    f64 run = 0.0;
    for (int tau = 1; tau <= maxLag; ++tau) {
        run += d[(size_t)tau];
        dn[(size_t)tau] = run > 0.0 ? d[(size_t)tau] * (f64)tau / run : 1.0;
    }
    // The FIRST dip below the threshold, not the deepest: the deepest is an
    // octave (or two) below the true period on anything harmonic.
    constexpr f64 kThresh = 0.15;
    for (int tau = kMinPeriod; tau <= maxLag; ++tau) {
        if (dn[(size_t)tau] >= kThresh) continue;
        int best = tau;
        while (best + 1 <= maxLag && dn[(size_t)(best + 1)] < dn[(size_t)best]) ++best;
        return best;
    }
    int best = 0;
    f64 bv = 1e300;
    for (int tau = kMinPeriod; tau <= maxLag; ++tau)
        if (dn[(size_t)tau] < bv) { bv = dn[(size_t)tau]; best = tau; }
    return bv < 0.6 ? best : 0;
}

// The largest power of two in [kMinSrcCycle, kMaxSrcCycle] that divides `n`.
int largestPow2Cycle(i64 n) {
    for (int c = kMaxSrcCycle; c >= kMinSrcCycle; c >>= 1)
        if (n % (i64)c == 0 && n / (i64)c >= 1) return c;
    return 0;
}

// ---------------------------------------------------------------------------
// The store
// ---------------------------------------------------------------------------

std::mutex           gMu;
std::vector<std::unique_ptr<Table>> gTables;
bool                 gLoggedFull = false;

const Table* findLocked(u64 hash) {
    for (const std::unique_ptr<Table>& t : gTables)
        if (t->hash == hash) return t.get();
    return nullptr;
}

const Table* insertLocked(const Table& src) {
    if (const Table* got = findLocked(src.hash)) return got;
    if ((int)gTables.size() >= kMaxTables) {
        if (!gLoggedFull) {
            gLoggedFull = true;
            LOGW("wavetables: this process already holds %d distinct tables; "
                 "further imports are refused rather than evicted, because a "
                 "table's address is handed out [further attempts silent]",
                 kMaxTables);
        }
        return nullptr;
    }
    gTables.push_back(std::make_unique<Table>(src));
    return gTables.back().get();
}

std::string homeDir() {
    if (const char* h = std::getenv("HOME"); h && *h) return h;
    if (passwd* pw = ::getpwuid(::getuid())) return pw->pw_dir;
    return "";
}

std::string exeDir() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return ".";
    buf[n] = '\0';
    char* slash = std::strrchr(buf, '/');
    if (!slash) return ".";
    *slash = '\0';
    return buf;
}

bool ensureDir(const std::string& dir) {
    if (dir.empty()) return false;
    for (size_t i = 1; i <= dir.size(); ++i) {
        if (i != dir.size() && dir[i] != '/') continue;
        const std::string part = dir.substr(0, i);
        if (::mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    return true;
}

bool fileExists(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

} // namespace

const char* ruleName(Rule r) {
    switch (r) {
        case Rule::Serum:  return "2048-frame convention";
        case Rule::Cycle:  return "power-of-two cycle";
        case Rule::Detect: return "detected period";
        case Rule::None:   break;
    }
    return "none";
}

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

u64 contentHash(const f32* frames, int frameCount) {
    if (!frames || frameCount <= 0 || frameCount > kMaxFrames) return 0;
    u64 h = mix64((u64)(u32)frameCount);
    h = mix64(h ^ (u64)(u32)kCycle);
    const size_t n = (size_t)frameCount * (size_t)kCycle;
    for (size_t i = 0; i < n; ++i) {
        const f32 v = frames[i];
        if (!std::isfinite(v)) return 0;         // never reaches the hash
        u32 u;
        std::memcpy(&u, &v, sizeof u);
        if (u == 0x80000000u) u = 0;             // -0.0 folds to +0.0
        h = mix64(h ^ (u64)u);
    }
    return h;
}

std::string hashHex(u64 h) {
    static const char kHex[] = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i) { s[(size_t)i] = kHex[h & 15ull]; h >>= 4; }
    return s;
}

bool hashFromHex(const char* s, size_t n, u64& out) {
    if (!s || n != 16) return false;
    u64 h = 0;
    for (size_t i = 0; i < 16; ++i) {
        const char c = s[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else return false;                        // LOWERCASE ONLY: the contract
        h = (h << 4) | (u64)(u32)d;
    }
    out = h;
    return true;
}

// ---------------------------------------------------------------------------
// Import
// ---------------------------------------------------------------------------

bool importFile(const std::string& path, Table& out, std::string& err) {
    err.clear();
    out = Table{};
    if (path.empty()) { err = "no file"; return false; }

    Wav w;
    if (!readWav(path, w, err)) return false;
    const i64 n = (i64)w.mono.size();
    if (n < kMinPeriod) { err = path + " is too short to hold a cycle"; return false; }

    // --- THE THREE INTERPRETATION RULES, IN ORDER (SPECTRA-V3-PLAN.md §1) ---
    int  cycle = 0, srcFrames = 0;
    Rule rule  = Rule::None;

    if (n % (i64)kCycle == 0 && n / (i64)kCycle <= (i64)kMaxSrcFrames) {
        // (a) The Serum convention: N x 2048 single-cycle frames. Sliced
        //     directly and NEVER resampled, which is what makes this the one
        //     rule whose identity is the same on every machine.
        cycle = kCycle;
        srcFrames = (int)(n / (i64)kCycle);
        rule = Rule::Serum;
    } else if (const int c = largestPow2Cycle(n);
               c != 0 && n / (i64)c <= (i64)kMaxSrcFrames) {
        // (b) An integer multiple of a power-of-two cycle in 256..4096. The
        //     LARGEST such divisor, because 3072 samples is three cycles of
        //     1024 and also six of 512 and twelve of 256, and only the first
        //     reading is the one the exporter meant.
        cycle = c;
        srcFrames = (int)(n / (i64)c);
        rule = Rule::Cycle;
    } else {
        // (c) Anything else: find the period, slice at it, cap at kMaxFrames.
        const int p = detectPeriod(w.mono);
        if (p < kMinPeriod) {
            err = path + " has no detectable pitch, so there is no cycle to "
                         "slice it into";
            return false;
        }
        cycle = p;
        srcFrames = (int)std::min<i64>((i64)kMaxFrames, n / (i64)p);
        if (srcFrames < 1) { err = path + " is shorter than one detected cycle"; return false; }
        rule = Rule::Detect;
    }

    // --- THE FRAME-AXIS CAP -------------------------------------------------
    //
    // A custom table has the IDENTICAL memory layout to a factory one, which is
    // 32 frames, so a source with more than 32 is decimated to 32 by even index
    // selection -- and it happens HERE, before the DC removal, before the
    // normalisation and therefore before the hash, so that the hash names
    // exactly the frames that will play, cross the wire and land in the cache.
    // See wavetable_io.h's kMaxFrames for why 32 is not a policy number.
    const int frames = srcFrames > kMaxFrames ? kMaxFrames : srcFrames;
    auto sourceOf = [&](int i) -> int {
        if (frames == srcFrames) return i;
        if (frames == 1) return 0;
        // Round-half-up on i*(srcFrames-1)/(frames-1), in integers, so the map
        // is the same arithmetic on every machine.
        return (int)((2ll * (i64)i * (i64)(srcFrames - 1) + (i64)(frames - 1)) /
                     (2ll * (i64)(frames - 1)));
    };

    out.data.assign((size_t)frames * (size_t)kCycle, 0.f);
    std::vector<f32> cyc((size_t)cycle, 0.f);
    for (int i = 0; i < frames; ++i) {
        const i64 at = (i64)sourceOf(i) * (i64)cycle;
        for (int k = 0; k < cycle; ++k) {
            const i64 j = at + (i64)k;
            cyc[(size_t)k] = j < n ? w.mono[(size_t)j] : 0.f;
        }
        resampleCycle(cyc.data(), cycle, out.data.data() + (size_t)i * (size_t)kCycle);
    }

    // --- DC PER FRAME, NORMALISE THE SET ------------------------------------
    //
    // Per frame for the DC because a frame with an offset is a frame that
    // thumps at every morph; SET-WIDE for the level because inter-frame level
    // is musical -- a table whose top frames are meant to be louder than its
    // bottom ones stays that way, which per-frame normalisation would flatten.
    f32 peak = 0.f;
    for (int i = 0; i < frames; ++i) {
        f32* fr = out.data.data() + (size_t)i * (size_t)kCycle;
        f64 sum = 0.0;
        for (int k = 0; k < kCycle; ++k) sum += (f64)fr[k];
        const f32 dc = (f32)(sum / (f64)kCycle);
        for (int k = 0; k < kCycle; ++k) {
            fr[k] -= dc;
            if (!std::isfinite(fr[k])) {
                err = path + " decodes to a frame containing a non-finite sample";
                return false;
            }
            const f32 a = std::fabs(fr[k]);
            if (a > peak) peak = a;
        }
    }
    if (!(peak > 1e-9f)) { err = path + " is silent once its DC is removed"; return false; }
    const f32 g = 1.f / peak;
    for (f32& v : out.data) v *= g;

    out.frames    = frames;
    out.rule      = rule;
    out.srcFrames = srcFrames;
    out.srcCycle  = cycle;
    out.path      = path;
    out.hash      = contentHash(out.data.data(), frames);
    if (out.hash == 0) { err = path + " produced a table with no identity"; return false; }
    return true;
}

// ---------------------------------------------------------------------------
// The store
// ---------------------------------------------------------------------------

const Table* find(u64 hash) {
    if (hash == 0) return nullptr;
    std::lock_guard<std::mutex> lk(gMu);
    return findLocked(hash);
}

int storeSize() {
    std::lock_guard<std::mutex> lk(gMu);
    return (int)gTables.size();
}

const Table* adopt(const Table& t, bool cache) {
    if (t.hash == 0 || t.frames <= 0 || t.frames > kMaxFrames) return nullptr;
    if (t.data.size() != (size_t)t.frames * (size_t)kCycle) return nullptr;
    const Table* stored;
    {
        std::lock_guard<std::mutex> lk(gMu);
        stored = insertLocked(t);
    }
    if (stored && cache) {
        const std::string dir = userCacheDir();
        if (ensureDir(dir)) {
            const std::string file = dir + "/" + hashHex(t.hash) + ".nxwt";
            if (!fileExists(file) && !writeNxwt(file, *stored))
                LOGW("wavetables: could not write the cache entry %s", file.c_str());
        }
    }
    return stored;
}

const Table* ingest(u64 hash, int frames, const f32* data) {
    if (hash == 0 || !data || frames <= 0 || frames > kMaxFrames) return nullptr;
    // THE HASH IS RECOMPUTED AND NOT BELIEVED. A peer names a table by hash;
    // a peer can name it wrongly, and a table filed under a hash it does not
    // have would answer the wrong question for the life of the process.
    //
    // BEFORE the "already have it" short-circuit and not after, which is the
    // whole point and was a bug for one afternoon: with the lookup first, a
    // peer offering the WRONG BYTES under a hash the store already held was
    // handed the right table and told yes. The right table is not the problem —
    // the yes is. A client that sent bytes this side never looked at believes
    // its table crossed, and the next one it sends under a hash the store does
    // NOT hold is the one that would have been caught, by which time the two
    // sides have disagreed for a whole session. Recomputing costs one fold over
    // at most 256 KiB on the pump thread; being lied to costs correctness.
    const u64 real = contentHash(data, frames);
    if (real == 0 || real != hash) {
        LOGW("wavetables: refused an ingested table -- it hashes to %s and was "
             "offered as %s", hashHex(real).c_str(), hashHex(hash).c_str());
        return nullptr;
    }
    if (const Table* got = find(hash)) return got;
    Table t;
    t.hash   = hash;
    t.frames = frames;
    t.rule   = Rule::Serum;      // it arrived as frames; no rule was applied here
    t.srcFrames = frames;
    t.srcCycle  = kCycle;
    t.data.assign(data, data + (size_t)frames * (size_t)kCycle);
    std::lock_guard<std::mutex> lk(gMu);
    return insertLocked(t);
}

const Table* resolve(u64 hash, const char* path) {
    if (hash == 0) return nullptr;

    // 1. the in-memory store
    if (const Table* got = find(hash)) return got;

    const std::string hex = hashHex(hash);

    // 2. the installed factory wavetable directory
    {
        const std::string dir = factoryDir();
        if (!dir.empty()) {
            Table t;
            if (readNxwt(dir + "/" + hex + ".nxwt", t) && t.hash == hash)
                if (const Table* got = adopt(t, false)) return got;
        }
    }

    // 3. the user cache
    {
        const std::string dir = userCacheDir();
        if (!dir.empty()) {
            Table t;
            if (readNxwt(dir + "/" + hex + ".nxwt", t) && t.hash == hash)
                if (const Table* got = adopt(t, false)) return got;
        }
    }

    // 4. a re-import from the recovery hint. On success this writes the user
    //    cache, so the next resolution stops at step 3 -- and it yields the
    //    same hash BY CONSTRUCTION or it is not the table that was asked for,
    //    which is what the equality below insists on.
    if (path && *path) {
        Table t;
        std::string err;
        if (importFile(path, t, err)) {
            if (t.hash == hash) return adopt(t, true);
            LOGW("wavetables: %s re-imports to %s, not the %s the set names",
                 path, hashHex(t.hash).c_str(), hex.c_str());
        } else {
            LOGW("wavetables: %s", err.c_str());
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Directories and the cache file
// ---------------------------------------------------------------------------

std::string userCacheDir() {
    std::string base;
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) base = x;
    else if (const std::string h = homeDir(); !h.empty()) base = h + "/.local/share";
    else base = "/tmp";
    return base + "/nxtakt/wavetables";
}

std::string factoryDir() {
    if (const char* x = std::getenv("NXTAKT_WAVETABLES"); x && *x) return x;
    const std::string d = exeDir();
    struct stat st{};
    const std::string beside = d + "/wavetables";
    if (::stat(beside.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return beside;
    return d + "/../share/nxtakt/wavetables";
}

bool writeNxwt(const std::string& file, const Table& t) {
    if (t.frames <= 0 || t.frames > kMaxFrames) return false;
    if (t.data.size() != (size_t)t.frames * (size_t)kCycle) return false;

    u8 hdr[kNxwtHeaderBytes];
    std::memcpy(hdr + 0, "NXWT", 4);
    const u32 ver = kNxwtVersion, fr = (u32)t.frames, cy = (u32)kCycle;
    std::memcpy(hdr + 4,  &ver, 4);
    std::memcpy(hdr + 8,  &fr,  4);
    std::memcpy(hdr + 12, &cy,  4);
    std::memcpy(hdr + 16, &t.hash, 8);

    // Write-and-rename, learn.cpp's discipline: a cache entry truncated by a
    // crash would come back as a table that fails its own hash check, which is
    // survivable -- and this costs one rename to make it impossible instead.
    const std::string tmp = file + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr &&
                    std::fwrite(t.data.data(), sizeof(f32), t.data.size(), f) == t.data.size() &&
                    std::fflush(f) == 0;
    std::fclose(f);
    if (!ok || std::rename(tmp.c_str(), file.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

bool readNxwt(const std::string& file, Table& out) {
    out = Table{};
    FILE* f = std::fopen(file.c_str(), "rb");
    if (!f) return false;                       // a miss is not an error
    u8 hdr[kNxwtHeaderBytes];
    if (std::fread(hdr, 1, sizeof hdr, f) != sizeof hdr) { std::fclose(f); return false; }
    u32 ver = 0, fr = 0, cy = 0;
    u64 hash = 0;
    std::memcpy(&ver,  hdr + 4,  4);
    std::memcpy(&fr,   hdr + 8,  4);
    std::memcpy(&cy,   hdr + 12, 4);
    std::memcpy(&hash, hdr + 16, 8);
    if (std::memcmp(hdr, "NXWT", 4) != 0 || ver != kNxwtVersion ||
        cy != (u32)kCycle || fr == 0 || fr > (u32)kMaxFrames) {
        std::fclose(f);
        return false;
    }
    std::vector<f32> data((size_t)fr * (size_t)kCycle);
    const size_t got = std::fread(data.data(), sizeof(f32), data.size(), f);
    std::fclose(f);
    if (got != data.size()) return false;

    // THE HASH IS RECOMPUTED. A cache entry is a file on a disk a user shares
    // with everything else on the machine; one that does not hash to its own
    // name is not a cache hit, it is a table that would play under somebody
    // else's identity for the life of the process.
    if (contentHash(data.data(), (int)fr) != hash) {
        LOGW("wavetables: %s does not hash to its own name; ignoring it", file.c_str());
        return false;
    }
    out.hash      = hash;
    out.frames    = (int)fr;
    out.rule      = Rule::Serum;
    out.srcFrames = (int)fr;
    out.srcCycle  = kCycle;
    out.data      = std::move(data);
    return true;
}

// ---------------------------------------------------------------------------
// The wire's discovery half
// ---------------------------------------------------------------------------

int hashesInDeviceState(const char* state, u64* out, int max) {
    if (!state || !out || max <= 0) return 0;
    int n = 0;
    const char* p = state;
    while (*p && n < max) {
        const char* rec = p;
        while (*p && *p != ';') ++p;
        const size_t len = (size_t)(p - rec);
        if (*p == ';') ++p;
        // A record is `<key>=<value>`. Only `wt`-prefixed keys with a value of
        // exactly 16 lowercase hex digits qualify; see the header for the three
        // things that together keep this from meaning anything to a device that
        // did not intend it.
        const char* eq = (const char*)std::memchr(rec, '=', len);
        if (!eq) continue;
        const size_t klen = (size_t)(eq - rec);
        if (klen < 2 || rec[0] != 'w' || rec[1] != 't') continue;
        const size_t vlen = len - klen - 1;
        u64 h = 0;
        if (!hashFromHex(eq + 1, vlen, h)) continue;
        bool dup = false;
        for (int i = 0; i < n; ++i) if (out[i] == h) { dup = true; break; }
        if (!dup) out[n++] = h;
    }
    return n;
}

} // namespace wt
} // namespace lat
