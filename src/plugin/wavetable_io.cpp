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

    // 3. v5: THE USER'S AUTHORED LIBRARY, and it sits ABOVE the cache.
    //    Rungs 2, 3 and 4 are hash-keyed and therefore interchangeable when
    //    they hit, so their order can only be about which copy wins on a
    //    machine that holds two -- and the answer is the one the user
    //    authored, because it is the copy that is not disposable and the copy
    //    whose absence is the failure worth noticing early. Appended BELOW
    //    rung 2, so it cannot shadow a factory table.
    {
        const std::string dir = drawnDir();
        if (!dir.empty()) {
            Table t;
            if (readNxwt(dir + "/" + hex + ".nxwt", t) && t.hash == hash)
                if (const Table* got = adopt(t, false)) return got;
        }
    }

    // 4. the user cache
    {
        const std::string dir = userCacheDir();
        if (!dir.empty()) {
            Table t;
            if (readNxwt(dir + "/" + hex + ".nxwt", t) && t.hash == hash)
                if (const Table* got = adopt(t, false)) return got;
        }
    }

    // 5. a recovery from the `wtpath` hint. On a WAV this writes the user
    //    cache, so the next resolution stops at rung 4 -- and it yields the
    //    same hash BY CONSTRUCTION or it is not the table that was asked for,
    //    which is what the equality below insists on.
    //
    //    v5 WIDENS THIS RUNG BY FILE EXTENSION, and by nothing else. A decoded
    //    value ending in `.nxwt` (byte-exact, lowercase) is read with
    //    readNxwt(), which already recomputes and compares the fold; any other
    //    tail is importFile(), WAV, exactly as in v3. THERE IS NO FALLBACK
    //    BETWEEN THE TWO ARMS: a file that lies about being a wavetable cache
    //    is not a file to guess about, and offering it to the WAV reader
    //    afterwards would be guessing. This costs one strcmp on the tail and is
    //    what lets a drawn table travel by file copy -- a user who moves
    //    `~/.local/share/nxtakt/drawn/` to another machine, or hands one file
    //    to a collaborator, has a set that resolves.
    //
    //    A drawn file recovered here is adopted WITHOUT the cache. It is
    //    already the one durable copy; a second under a policy that says
    //    deleting it is safe would be two things to keep in sync, not
    //    redundancy.
    if (path && *path) {
        const size_t plen = std::strlen(path);
        const bool isNxwt = plen >= 5 && std::memcmp(path + plen - 5, ".nxwt", 5) == 0;
        if (isNxwt) {
            Table t;
            if (readNxwt(path, t) && t.hash == hash) {
                t.path = path;
                if (const Table* got = adopt(t, false)) return got;
            } else {
                LOGW("wavetables: %s is not the %s the set names", path, hex.c_str());
            }
            return nullptr;                 // no fallback to the WAV arm
        }
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

// v5. THE SAME LADDER userCacheDir() WALKS, ending in the same /tmp, and a
// SIBLING of `wavetables/` rather than a child of it. Written as its own
// function and not as `userCacheDir() + "/../drawn"` because the point of the
// path is that no spelling of "clear the wavetable cache" can reach it, and a
// path that contains the cache directory's name is a path that a careless
// prefix match can.
std::string drawnDir() {
    std::string base;
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) base = x;
    else if (const std::string h = homeDir(); !h.empty()) base = h + "/.local/share";
    else base = "/tmp";
    return base + "/nxtakt/drawn";
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

bool writeDrawn(const Table& t, std::string& outPath) {
    outPath.clear();
    if (t.hash == 0 || t.frames <= 0 || t.frames > kMaxFrames) return false;
    const std::string dir = drawnDir();
    if (dir.empty() || !ensureDir(dir)) {
        LOGW("wavetables: could not create the drawn-wavetable directory %s",
             dir.c_str());
        return false;
    }
    const std::string file = dir + "/" + hashHex(t.hash) + ".nxwt";
    outPath = file;
    if (fileExists(file)) return true;      // same name means same bytes
    if (!writeNxwt(file, t)) {
        LOGW("wavetables: could not write the drawn wavetable %s", file.c_str());
        outPath.clear();
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
// v5: THE TRANSFORM
//
// spFft / spIfft / spTwiddle, MOVED here from spectra_tables.inc without one
// character of the arithmetic changing, so that the pen and the mip builder
// share one transform and one set of twiddles. See the header for why, and the
// release gate for the proof that a move across a translation unit boundary is
// arithmetically free at these flags.
//
// ATTRIBUTION, carried over verbatim: this is fftRadix2 from
// src/audio/sample.cpp, copied rather than shared, because src/plugin does not
// include src/audio and should not start doing so for twenty-five lines of
// butterflies.
// ---------------------------------------------------------------------------

void fft(f32* re, f32* im, int n, const f32* tw) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len >> 1;
        const int step = n / len;               // stride into the twiddle table
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < half; ++k) {
                const f32 wr = tw[(size_t)(k * step) * 2 + 0];
                const f32 wi = tw[(size_t)(k * step) * 2 + 1];
                const int a = i + k, b = i + k + half;
                const f32 vr = re[b] * wr - im[b] * wi;
                const f32 vi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - vr; im[b] = im[a] - vi;
                re[a] = re[a] + vr; im[a] = im[a] + vi;
            }
        }
    }
}

// IDFT(X) = swap(DFT(swap(X)))/N, so handing the transform its arrays the wrong
// way round twice is the whole inverse. After this the real part is in `re`.
void ifft(f32* re, f32* im, int n, const f32* tw) {
    fft(im, re, n, tw);
    const f32 s = 1.f / (f32)n;
    for (int i = 0; i < n; ++i) { re[i] *= s; im[i] *= s; }
}

void twiddle(std::vector<f32>& t, int n) {
    t.assign((size_t)(n / 2) * 2, 0.f);
    for (int i = 0; i < n / 2; ++i) {
        const f64 a = -6.283185307179586 * (f64)i / (f64)n;
        t[(size_t)i * 2 + 0] = (f32)std::cos(a);
        t[(size_t)i * 2 + 1] = (f32)std::sin(a);
    }
}

// ---------------------------------------------------------------------------
// v5: THE PEN
//
// docs/SPECTRA-PARAMS.md, "The two pens", "Frames", "Morph" and "Commit". The
// header carries the arguments; this carries the arithmetic, and every loop
// below has a FIXED ORDER because the frames it produces are the identity.
// ---------------------------------------------------------------------------
namespace pen {
namespace {

// ONE twiddle table for kCycle, built on first use and never rebuilt. A
// function-local static, so it is a root for the leak checker and a process
// that never draws never pays for it. Every pen transform is at kCycle -- the
// pen has no other length, because a pen that drew into 256 points and
// upsampled would need a defined upsample IN THE IDENTITY PATH, which is a
// second resampler and a second libm dependence, to buy nothing.
const std::vector<f32>& cycleTwiddle() {
    static const std::vector<f32> t = [] {
        std::vector<f32> v;
        twiddle(v, kCycle);
        return v;
    }();
    return t;
}

// Scratch for one transform. Not a static: the pen runs on the one non-realtime
// thread that owns the device, and 16 KiB on the stack of a GUI call is nothing
// against a shared buffer that two windows could enter at once.
struct Scratch {
    std::vector<f32> re, im;
    Scratch() : re((size_t)kCycle, 0.f), im((size_t)kCycle, 0.f) {}
};

inline f32 clamp1(f32 v) { return v < -1.f ? -1.f : (v > 1.f ? 1.f : v); }
inline int clampIdx(int i) { return i < 0 ? 0 : (i > kCycle - 1 ? kCycle - 1 : i); }

} // namespace

int index(f32 xNorm) {
    if (!(xNorm == xNorm)) return 0;               // a NaN from a UI is index 0
    const f32 x = xNorm * (f32)(kCycle - 1);
    // round-half-away-from-zero, and std::lround rather than a cast, because a
    // cast truncates and the contract says round.
    return clampIdx((int)std::lround((double)x));
}

void stroke(f32* frame, int i0, f32 v0, int i1, f32 v1) {
    if (!frame) return;
    i0 = clampIdx(i0);
    i1 = clampIdx(i1);
    v0 = clamp1(v0);
    v1 = clamp1(v1);
    if (i1 == i0) { frame[i0] = v1; return; }      // the later write wins

    // ASCENDING IN INDEX, whichever way the pointer moved, so that a stroke and
    // its mirror image write the same samples in the same order.
    const int lo = i0 < i1 ? i0 : i1;
    const int hi = i0 < i1 ? i1 : i0;
    const f32 vlo = i0 < i1 ? v0 : v1;
    const f32 vhi = i0 < i1 ? v1 : v0;
    const f32 span = (f32)(hi - lo);
    for (int i = lo; i <= hi; ++i) {
        const f32 t = (f32)(i - lo) / span;
        frame[i] = vlo + (vhi - vlo) * t;
    }
    // The endpoints are written exactly rather than through the interpolant, so
    // that a one-pixel stroke and a thousand-pixel stroke agree about where they
    // started and stopped.
    frame[lo] = vlo;
    frame[hi] = vhi;
}

void removeDc(f32* frame) {
    if (!frame) return;
    f64 sum = 0.0;
    for (int i = 0; i < kCycle; ++i) sum += (f64)frame[i];     // f64, ascending
    const f32 mean = (f32)(sum / (f64)kCycle);
    for (int i = 0; i < kCycle; ++i) frame[i] -= mean;         // f32, ascending
}

void analyse(const f32* frame, Spectrum& out) {
    out = Spectrum{};
    if (!frame) return;
    Scratch sc;
    for (int i = 0; i < kCycle; ++i) { sc.re[(size_t)i] = frame[i]; sc.im[(size_t)i] = 0.f; }
    fft(sc.re.data(), sc.im.data(), kCycle, cycleTwiddle().data());
    // THE MIP BUILDER'S SCALING, to the character: s = 2/N, hr = cosine
    // coefficient, hi = MINUS the sine coefficient, h = 0 forced to zero. 2/2048
    // and its inverse 1024 are both powers of two, so analyse -> synthesise
    // multiplies by exactly 1.0 and the round trip loses nothing to the scaling.
    const f32 s = 2.f / (f32)kCycle;
    for (int h = 0; h <= kMaxHarm; ++h) {
        out.hr[h] = h == 0 ? 0.f : sc.re[(size_t)h] * s;
        out.hi[h] = h == 0 ? 0.f : sc.im[(size_t)h] * s;
    }
}

f32 magnitude(const Spectrum& s, int h) {
    if (h < 0 || h > kMaxHarm) return 0.f;
    return std::sqrt(s.hr[h] * s.hr[h] + s.hi[h] * s.hi[h]);
}

f32 magFromDb(f32 db) {
    if (!(db > kFloorDb)) return 0.f;          // THE FLOOR IS A HARD ZERO
    if (db > 0.f) db = 0.f;                    // no harmonic may exceed 0 dB
    return (f32)std::pow(10.0, (double)db / 20.0);   // the one new libm call
}

f32 dbFromMag(f32 m) {
    if (!(m > 0.f)) return kFloorDb;
    const f32 db = (f32)(20.0 * std::log10((double)m));
    return db < kFloorDb ? kFloorDb : (db > 0.f ? 0.f : db);
}

void setBar(Spectrum& s, int h, f32 mag) {
    if (h < 1 || h > kEditHarm) return;        // DC is not editable; 257..1023
                                               // are not editable and PRESERVED
    if (!(mag > 0.f)) mag = 0.f;
    if (mag > 1.f) mag = 1.f;
    // (m, SINE PHASE): frame[i] = m*sin(2*pi*h*i/N) means A_h = 0 and B_h = m,
    // and the convention stores hi = -B_h.
    s.hr[h] = 0.f;
    s.hi[h] = -mag;
}

void toSinePhase(Spectrum& s) {
    s.hr[0] = 0.f;
    s.hi[0] = 0.f;
    for (int h = 1; h <= kMaxHarm; ++h) {      // ascending in h
        const f32 m = magnitude(s, h);
        s.hr[h] = 0.f;
        s.hi[h] = -m;
    }
}

void synthesise(const Spectrum& s, f32* frame) {
    if (!frame) return;
    Scratch sc;
    // Exactly spBuildFrameMips' reconstruction at n = kCycle: X[h] =
    // (N/2)*(hr + i*hi), its conjugate at N-h, and nothing at all at h = 0 --
    // DC is FORCED TO ZERO by never being written.
    const f32 half = 0.5f * (f32)kCycle;
    for (int h = 1; h <= kMaxHarm; ++h) {      // ascending in h
        sc.re[(size_t)h] = half * s.hr[h];
        sc.im[(size_t)h] = half * s.hi[h];
        sc.re[(size_t)(kCycle - h)] =  sc.re[(size_t)h];
        sc.im[(size_t)(kCycle - h)] = -sc.im[(size_t)h];
    }
    ifft(sc.re.data(), sc.im.data(), kCycle, cycleTwiddle().data());
    for (int i = 0; i < kCycle; ++i) frame[i] = sc.re[(size_t)i];
}

bool morph(f32* frames, int a, int b) {
    if (!frames) return false;
    if (a < 0 || b >= kMaxFrames || b <= a + 1) return false;   // b > a+1 or no-op

    Spectrum sa, sb;
    analyse(frames + (size_t)a * (size_t)kCycle, sa);
    analyse(frames + (size_t)b * (size_t)kCycle, sb);

    // The two magnitude sets, taken once. Ascending in h.
    std::vector<f32> ma((size_t)kMaxHarm + 1, 0.f), mb((size_t)kMaxHarm + 1, 0.f);
    for (int h = 1; h <= kMaxHarm; ++h) {
        ma[(size_t)h] = magnitude(sa, h);
        mb[(size_t)h] = magnitude(sb, h);
    }

    Spectrum k;
    for (int fr = a + 1; fr < b; ++fr) {                        // ascending in k
        const f32 t = (f32)(fr - a) / (f32)(b - a);
        k = Spectrum{};
        for (int h = 1; h <= kMaxHarm; ++h) {                   // ascending in h
            // MAGNITUDE ONLY, at the pen's SINE PHASE. f32 throughout.
            const f32 m = (1.f - t) * ma[(size_t)h] + t * mb[(size_t)h];
            k.hr[h] = 0.f;
            k.hi[h] = -m;
        }
        synthesise(k, frames + (size_t)fr * (size_t)kCycle);
    }
    // a and b are NOT TOUCHED. If either is not already in sine phase there is
    // an audible phase step at the boundary, and rephaseEndpoints() is the named
    // fix -- it is not called from here, and that is the contract.
    return true;
}

bool rephaseEndpoints(f32* frames, int a, int b) {
    if (!frames) return false;
    if (a < 0 || a >= kMaxFrames || b < 0 || b >= kMaxFrames || a == b) return false;
    const int lo = a < b ? a : b, hi = a < b ? b : a;
    for (int fr : { lo, hi }) {                                 // ascending
        Spectrum s;
        f32* f = frames + (size_t)fr * (size_t)kCycle;
        analyse(f, s);
        toSinePhase(s);                    // magnitudes untouched
        synthesise(s, f);
    }
    return true;
}

void clearFrame(f32* frames, int k) {
    if (!frames || k < 0 || k >= kMaxFrames) return;
    f32* f = frames + (size_t)k * (size_t)kCycle;
    for (int i = 0; i < kCycle; ++i) f[i] = 0.f;
}

// INSERT DROPS FRAME 31. Say it here, where a reader hits it: the frame count is
// fixed at 32, so the tail has to go somewhere and there is nowhere. The
// alternative -- refusing to insert when frame 31 is non-zero -- is a tool that
// stops working exactly when the table is full, which is always.
void insertFrame(f32* frames, int k) {
    if (!frames || k < 0 || k >= kMaxFrames) return;
    const size_t n = (size_t)kCycle * sizeof(f32);
    for (int i = kMaxFrames - 1; i > k; --i)                    // descending, so
        std::memcpy(frames + (size_t)i * kCycle,                // the shift does
                    frames + (size_t)(i - 1) * kCycle, n);      // not eat itself
    // frames[k] is already the copy: inserting AT the cursor leaves the cursor
    // frame where it is and puts its duplicate immediately after it. What was
    // frame 31 is gone.
}

void duplicateFrame(f32* frames, int k) {
    if (!frames || k < 0 || k >= kMaxFrames) return;
    // "copy the cursor frame into the next slot, pushing the tail down" is
    // insert-at-k+1 of a copy of k -- which produces the identical array Insert
    // does. The two differ only in where the editor leaves the CURSOR, and a
    // cursor is editor-only state. Frame 31 is dropped either way.
    insertFrame(frames, k);
}

// DELETE DUPLICATES THE NEW LAST FRAME. The same fixed-32 argument from the
// other end: pulling the tail up leaves slot 31 holding what slot 30 now holds,
// which IS "copy the (new) last frame into slot 31" -- stated rather than left
// as a consequence of the memcpy, because a reader will look for the copy.
void deleteFrame(f32* frames, int k) {
    if (!frames || k < 0 || k >= kMaxFrames) return;
    const size_t n = (size_t)kCycle * sizeof(f32);
    for (int i = k; i < kMaxFrames - 1; ++i)                    // ascending
        std::memcpy(frames + (size_t)i * kCycle,
                    frames + (size_t)(i + 1) * kCycle, n);
    // Slot 31 now equals slot 30 -- the new last frame, duplicated.
}

void stretchFrames(const f32* src, int srcFrames, f32* dst) {
    if (!src || !dst || srcFrames <= 0 || srcFrames > kMaxFrames) return;
    const int n = srcFrames;
    for (int fr = 0; fr < kMaxFrames; ++fr) {
        // spBuildCustomMips' frame axis, transcribed rather than re-derived.
        const f64 x = n <= 1 ? 0.0
                             : (f64)fr * (f64)(n - 1) / (f64)(kMaxFrames - 1);
        int s0 = (int)x;
        if (s0 > n - 1) s0 = n - 1;
        const int s1 = s0 + 1 < n ? s0 + 1 : s0;
        const f32 bl = (f32)(x - (f64)s0);
        const f32* a = src + (size_t)s0 * (size_t)kCycle;
        const f32* c = src + (size_t)s1 * (size_t)kCycle;
        f32* d = dst + (size_t)fr * (size_t)kCycle;
        for (int i = 0; i < kCycle; ++i) d[i] = a[i] + (c[i] - a[i]) * bl;
    }
}

bool canonicalise(f32* frames, std::string& err) {
    err.clear();
    if (!frames) { err = "there is nothing to commit"; return false; }
    const size_t n = (size_t)kMaxFrames * (size_t)kCycle;

    // 1. EVERY SAMPLE MUST BE FINITE. v3's rule -- "a frame containing a
    //    non-finite sample is refused at import and never reaches the hash" --
    //    applying to the pen unchanged, so a NaN payload cannot become an
    //    identity by a second door. Checked BEFORE anything is written, so a
    //    refusal leaves the working copy exactly as it was.
    for (size_t i = 0; i < n; ++i) {
        if (!std::isfinite(frames[i])) {
            err = "this table has a sample that is not a number; nothing was saved";
            return false;
        }
    }

    // A refusal must change nothing, and step 4 can refuse AFTER step 2 has
    // written. So step 2 runs on a copy and is committed at the end.
    std::vector<f32> work(frames, frames + n);

    // 2. PER-FRAME DC REMOVAL, ascending frame then ascending index, mean in
    //    f64 and subtracted in f32. Normally a no-op -- the waveform pen already
    //    did it at stroke end and the harmonic pen's sine convention makes it
    //    one by construction -- and it runs anyway, because a commit must not
    //    depend on which pen last touched a frame.
    for (int fr = 0; fr < kMaxFrames; ++fr) removeDc(work.data() + (size_t)fr * (size_t)kCycle);

    // 3. THE SET-WIDE PEAK, ascending.
    f32 pk = 0.f;
    for (size_t i = 0; i < n; ++i) {
        const f32 a = std::fabs(work[i]);
        if (a > pk) pk = a;
    }

    // 4. REFUSE A SILENT TABLE. Import maps this case to a gain of 1 and carries
    //    on because it is recovering someone else's file; a drawing that is all
    //    zeros is a mistake, and letting it through would burn an identity on
    //    silence forever. Deliberate divergence, stated in both directions.
    if (pk <= kSilent) {
        err = "this table is silent";
        return false;
    }

    // 5. ONE SET-WIDE FACTOR, ascending. Not per-frame and not per-stroke: a
    //    single scalar over all 32 frames changes no shape and no inter-frame
    //    relationship, so nothing the user drew moves.
    const f32 g = 1.f / pk;
    for (size_t i = 0; i < n; ++i) work[i] *= g;

    std::memcpy(frames, work.data(), n * sizeof(f32));
    return true;
}

f64 previewInterval(f64 sampleRate, int maxBlock) {
    if (!(sampleRate > 0.0) || maxBlock <= 0) return 0.05;
    const f64 twoBlocks = 2.0 * (f64)maxBlock / sampleRate;
    return twoBlocks > 0.05 ? twoBlocks : 0.05;
}

} // namespace pen

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
