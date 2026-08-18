// Sampler — NxTakt's stock sample player.
//
// Drop a file on it, play it chromatically. That is the whole promise, and the
// point of the device is that it keeps it without any of the qualifications a
// first version usually ships with.
//
// ---------------------------------------------------------------------------
// WHAT IT DOES, in the order the signal travels
//
//   note -> voice (up to 16, param "Voices", quietest-stolen)
//     glide (constant time) -> pitch, as a RATIO against the root note
//     one sample, read with 4-point Catmull-Rom between Start and End
//       optional loop with an equal-length crossfade (0..50 ms)
//     -> TPT state-variable lowpass (cutoff, resonance, envelope, keytrack)
//     -> ADSR as the VCA, scaled by velocity
//
// ---------------------------------------------------------------------------
// WHAT IT DOES NOT DO, said plainly rather than discovered later
//
//   * NO TIMESTRETCH. Pitch and speed are the same knob: a note an octave up
//     plays twice as fast and lasts half as long, exactly like every hardware
//     sampler ever built. Warping a clip to the transport is what the CLIP
//     layer is for (src/audio/engine.cpp's warp map); this device deliberately
//     does not duplicate it.
//   * NO ANTI-ALIASING FILTER ON THE WAY UP. Pitching a bright sample up
//     folds whatever sat between the old Nyquist and the new one back down.
//     The interpolator is a reconstruction filter, not a decimation filter,
//     and calling it one would be a lie; the honest fix is an oversampled read
//     and it is not in v1. Pitching DOWN is clean, which is the direction a
//     sampler is used in most of the time.
//   * NO SAMPLE MAPS. One file, one root note, chromatic in both directions.
//     Multisampling is a second data structure and a second editor.
//   * NO MODULATION SOURCES beyond the one envelope. The envelope is both the
//     VCA and the cutoff modulator; a second envelope and an LFO are what
//     Spectra is for.
//
// ---------------------------------------------------------------------------
// THE SAMPLE, and the one lifetime rule that matters
//
// `loadSample()` runs on the GUI thread and NEVER in process(). What the audio
// thread sees is a raw `const SampleBuffer*` published with a single release
// store; the `shared_ptr` that keeps it alive lives beside it on the GUI side.
// A buffer that is displaced by a second load is RETIRED, not freed — the same
// discipline `Rack` applies to a displaced Layout and for the same reason:
// nothing inside a PluginInstance can know when the audio thread last
// dereferenced a pointer it published, but the caller can, and says so by
// calling SamplerControl::reclaim().
//
// A sampler with no sample is SILENT. That is the whole of the empty state,
// and it is silent rather than "playing a click" or "passing its input
// through" because an instrument's input is silence by construction. The
// device strip cannot currently say so — it draws `DeviceModel::desc.name`,
// which is a copy taken from the registry at instantiate time and is not
// reachable from inside an instance (src/ui/app_devices.cpp:974). Filed rather
// than hacked around: naming the loaded file in the strip belongs to the
// editor, beside the waveform it will draw.
//
// ---------------------------------------------------------------------------
// DETERMINISM, the same gate Spectra passes and for the same reasons
//
//   * NOTE EVENTS ARE QUEUED AND APPLIED AT THEIR OWN SAMPLE. Voice stealing
//     picks the quietest voice, so allocation is a function of every envelope
//     AT THE INSTANT OF THE NOTE; midi() is called once per block, so applying
//     a note-on when it arrives would make blocks of 1 and of 1024 steal
//     different voices. See incident 6 in docs/PAPER.md, and Spectra::midi().
//   * The control tick counts down on ABSOLUTE sample time across process()
//     calls, so the filter's coefficient updates land on the same samples at
//     every block size.
//   * Envelopes, glide and the read position advance one sample at a time.
//   * There is no random number anywhere in this device, and no clock. Six
//     block sizes are bit-identical; the suite is what says so.
//
// ---------------------------------------------------------------------------
// REALTIME. process() and midi() allocate nothing, lock nothing, throw nothing
// and call nothing that could. The only allocations in the device are the
// decode and the two std::strings, all of them on the GUI thread.
//
// This file is #included by internal_devices.cpp rather than compiled on its
// own — see the guard below, which is spectra.cpp's and is here for the same
// build-system reason.
#ifndef LAT_SAMPLER_IN_INTERNAL_DEVICES

// Compiled standalone. The GUI's Makefile sweeps every src/**/*.cpp into its
// object list, so this file is handed to the compiler on its own as well as
// through internal_devices.cpp; building the device twice would put a dead copy
// in the binary and warn about every helper in it. An empty translation unit is
// the honest answer until the tool and test recipes list this file explicitly,
// at which point the include becomes a declaration and this guard goes away.
// spectra.cpp carries the identical guard for the identical reason.
namespace lat { namespace detail { /* see internal_devices.cpp */ } }

#else

#include "host.h"
#include "internal_base.h"
#include "internal_dsp.h"
#include "../audio/sample.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace lat {

// ---------------------------------------------------------------------------
// The decoder, as a WEAK reference.
//
// `loadSample` is defined in src/audio/sample.cpp, which pulls in libsndfile
// and libsamplerate — and which three of this tree's link targets deliberately
// do NOT link: `nxtaktd` ("renders, does not decode or draw"), `plugin_scan`,
// and `internal_device_test`. The Makefile's tool and test recipes list their
// sources one by one, so a hard reference from this device would break four
// links at once, and adding sample.cpp to those recipes would put a decoder
// inside a daemon that has an architecture reason not to have one.
//
// A weak reference is exactly the right shape for that: where the decoder is
// linked (the GUI, `render`, `timesig_view_test`) the real function binds and
// the sampler loads files; where it is not, the symbol resolves to 0, loadFile()
// answers false, and the device is empty and silent rather than absent. The
// state string still round-trips in that build, so a set does not lose the path
// it names by passing through a process that cannot open it.
//
// The declaration is a REDECLARATION of the one in sample.h -- included above
// so the SampleBuffer layout is a real type here -- with the attribute added;
// GCC and Clang both merge it, and the test at every call site is `if (!fn)`.
SampleRef loadSample(const std::string& path, f64 engineRate) __attribute__((weak));

namespace detail {
namespace {

// ---------------------------------------------------------------------------
// Parameter ids. THESE ARE THE CONTRACT.
//
// ids ARE indices (InternalInstance::addParam) and a saved set stores them, so
// this table is frozen the moment one project file has been written with it:
// entries may be APPENDED and never moved, renamed away from their meaning, or
// removed. The cap is 24 -- four spare -- and that is a promise about how much
// a generic knob strip should ever have to draw for one device.
//
//   id  name          unit  range              default  notes
//   --  ------------  ----  -----------------  -------  ----------------------
//    0  Root Note     -     0 .. 127 (int)     60       plays at unity here
//    1  Coarse        st    -24 .. 24 (int)    0
//    2  Fine          ct    -100 .. 100        0
//    3  Start         -     0 .. 1             0        fraction of the file
//    4  End           -     0 .. 1             1        fraction of the file
//    5  Loop          -     bool               off      loops Start..End
//    6  Crossfade     ms    0 .. 50            5        loop crossfade
//    7  Gate          -     bool               on       off = one-shot
//    8  Attack        ms    0.1 .. 5000 (log)  0.5
//    9  Decay         ms    1 .. 5000 (log)    1000
//   10  Sustain       -     0 .. 1             1
//   11  Release       ms    1 .. 8000 (log)    40
//   12  Cutoff        Hz    20 .. 20000 (log)  20000
//   13  Resonance     -     0 .. 1             0.1
//   14  Env>Cutoff    -     -1 .. 1            0
//   15  Keytrack      -     0 .. 1             0
//   16  Vel>Amp       -     0 .. 1             1
//   17  Glide         ms    0 .. 500           0
//   18  Voices        -     1 .. 16 (int)      16
//   19  Master        -     0 .. 2             1        1 is unity
// ---------------------------------------------------------------------------

enum : int {
    kSmRoot = 0, kSmCoarse, kSmFine,
    kSmStart = 3, kSmEnd, kSmLoop, kSmXfade,
    kSmGate = 7,
    kSmAttack = 8, kSmDecay, kSmSustain, kSmRelease,
    kSmCutoff = 12, kSmRes, kSmEnvCut, kSmKeytrack,
    kSmVel = 16, kSmGlide, kSmVoices, kSmMaster,
    kSmParamCount = 20
};

// ---------------------------------------------------------------------------
// The state string
//
//   nxsmp1;p=<percent-escaped path>
//
// One line of printable ASCII with no whitespace, no quotes and no newline, so
// it drops into project.cpp's `kv()` as an opaque scalar -- the same shape and
// the same escape set the rack's compact form uses (internal_devices.cpp), so
// there is one spelling of "an opaque device state" in this tree rather than
// two. Version-tagged; ';'-separated records tagged by their first character,
// so a later version may add records an older reader will skip.
//
// The path is the ONLY thing in it. Everything else the sampler is, it is
// through its parameters, which the project layer already carries -- and that
// is deliberate: a state string that duplicated a parameter would be a second
// source of truth for it, and the two would drift.
//
// ONE DELIBERATE DIFFERENCE FROM THE RACK'S PARSER. `rackUnesc()` passes a
// malformed `%` escape through as a literal `%`, which is the forgiving choice
// a URI wants. A FILE PATH is not a URI: "%2" and "%252" must not both decode
// to something we then try to open, and a path is exactly the kind of field a
// hostile or corrupt file gets creative with. So the sampler's unescape is
// STRICT and refuses, which is what host.h's setStateString contract asks for.
// ---------------------------------------------------------------------------

constexpr const char* kSmTag = "nxsmp1";

// A path longer than this is not a path, it is an attack or a corruption.
// PATH_MAX is 4096 on Linux; this is that, and the check happens on the
// DECODED bytes so an escape expansion cannot smuggle length past it.
constexpr size_t kSmMaxPath = 4096;

// The rack's set, exactly: everything structural plus everything that is not a
// printable, non-space ASCII character. Escaping space as well as control bytes
// is what keeps the line whitespace-free, which is what `kv()` needs.
bool smNeedsEsc(unsigned char c) {
    return c <= ' ' || c >= 0x7F || c == '%' || c == ';' || c == ',' || c == ':' || c == '=';
}

void smEsc(std::string& o, const std::string& s) {
    static const char kHex[] = "0123456789ABCDEF";
    for (char ch : s) {
        const unsigned char c = (unsigned char)ch;
        if (smNeedsEsc(c)) { o += '%'; o += kHex[c >> 4]; o += kHex[c & 15]; }
        else               o += ch;
    }
}

int smHex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// STRICT. False means "this field is not a path this writer could have
// produced", and the caller must then change nothing at all.
//
// Rejects, in order: a `%` that is not followed by two hex digits; an escape
// that decodes to NUL; a raw byte that the writer would have escaped (so a
// literal space or newline that arrived unescaped is refused rather than
// quietly accepted into a filename); and anything over the length cap.
//
// NUL AND ONLY NUL among the byte values, which is a narrower rule than it
// first looks like it should be. A POSIX filename may contain a newline, a tab
// or any other control byte -- they are unusual and they are legal, and a user
// who has one is not attacking anybody. Refusing them would break the property
// this pair exists to have, which is that escape and unescape are INVERSES: a
// path the sampler can write would be a path it cannot read back, and the set
// would lose the file it names on the next load. NUL is the exception because
// it is not a byte a path can survive at all -- `open(2)` takes a C string and
// would silently truncate there, so accepting one means opening a DIFFERENT
// file from the one the state named.
bool smUnesc(const std::string& s, std::string& out) {
    out.clear();
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = (unsigned char)s[i];
        if (c == '%') {
            if (i + 2 >= s.size()) return false;
            const int hi = smHex(s[i + 1]), lo = smHex(s[i + 2]);
            if (hi < 0 || lo < 0) return false;
            const unsigned char d = (unsigned char)((hi << 4) | lo);
            if (d == 0) return false;              // see the note above
            out += (char)d;
            i += 2;
        } else {
            // A byte the writer would have escaped cannot appear raw. This is
            // the check that turns "the field looks odd" into "refuse".
            if (smNeedsEsc(c)) return false;
            out += (char)c;
        }
        if (out.size() > kSmMaxPath) return false;
    }
    return true;
}

std::vector<std::string> smSplit(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == sep) { out.push_back(s.substr(start, i - start)); start = i + 1; }
    return out;
}

// ---------------------------------------------------------------------------
// Factory presets
//
// The same mechanism Spectra uses, so the whole program still sees a preset as
// a handful of ordinary knob moves: loadPreset() resets every parameter to its
// default and then applies the list, which makes a preset COMPLETE however
// short it is written.
//
// NOT ONE OF THEM NAMES A FILE, and that is a rule and not an omission: a
// preset cannot name a path the user does not have, so shipping one would ship
// six devices that are broken on every machine but the author's. A preset sets
// how the sampler PLAYS; what it plays is the user's.
// ---------------------------------------------------------------------------

struct SmPreset {
    const char* name;
    int         n;
    struct { int id; f32 v; } set[24];
};

const SmPreset kSmPresets[] = {
    { "Init", 0, {} },

    // Hit a key, hear the whole file, ignore the key coming back up. The
    // beatmaker's default and the reason "one-shot" is a mode rather than an
    // envelope trick.
    { "One-Shot", 7, {
        { kSmGate, 0 }, { kSmLoop, 0 },
        { kSmAttack, 0.1f }, { kSmDecay, 5000.f }, { kSmSustain, 1.f },
        { kSmRelease, 5.f }, { kSmVel, 1.f },
    } },

    // A sustaining instrument sample: loop the body, crossfade long enough to
    // hide a splice, and let the key release it.
    { "Pitched Loop", 8, {
        { kSmLoop, 1 }, { kSmXfade, 25.f }, { kSmGate, 1 },
        { kSmAttack, 4.f }, { kSmDecay, 3000.f }, { kSmSustain, 1.f },
        { kSmRelease, 220.f }, { kSmVoices, 8 },
    } },

    // Monophonic, glided, resonant and envelope-swept. The 303 is a filter
    // patch far more than it is an oscillator, which is exactly why a sampler
    // can play one.
    { "303 Glide", 13, {
        { kSmVoices, 1 }, { kSmGlide, 45.f },
        { kSmLoop, 1 }, { kSmXfade, 8.f },
        { kSmCutoff, 320.f }, { kSmRes, 0.82f }, { kSmEnvCut, 0.80f },
        { kSmKeytrack, 0.35f },
        { kSmAttack, 0.5f }, { kSmDecay, 260.f }, { kSmSustain, 0.15f },
        { kSmRelease, 60.f }, { kSmMaster, 0.7f },
    } },

    { "Soft Pad", 10, {
        { kSmLoop, 1 }, { kSmXfade, 50.f },
        { kSmAttack, 800.f }, { kSmDecay, 2500.f }, { kSmSustain, 0.80f },
        { kSmRelease, 1500.f },
        { kSmCutoff, 4000.f }, { kSmRes, 0.15f },
        { kSmVel, 0.50f }, { kSmMaster, 0.80f },
    } },

    // A drum rack in one device: one-shot, no loop, and an envelope short
    // enough that the voice frees itself instead of holding a tail.
    { "Drum Tight", 8, {
        { kSmGate, 0 }, { kSmLoop, 0 },
        { kSmAttack, 0.1f }, { kSmDecay, 220.f }, { kSmSustain, 0.f },
        { kSmRelease, 5.f }, { kSmVoices, 8 }, { kSmVel, 1.f },
    } },
};

constexpr int kSmPresetCount = (int)(sizeof kSmPresets / sizeof kSmPresets[0]);

// ---------------------------------------------------------------------------
// The interpolator
//
// FOUR-POINT CATMULL-ROM, and the reason is measured rather than asserted: a
// repitching sampler on linear interpolation hisses. Linear reconstruction of
// a sinusoid leaves an error that grows as the square of the frequency, and at
// a musically ordinary read rate that error is 30-40 dB below the signal —
// audible as a gritty top end on every note that is not exactly at unity.
// Catmull-Rom's error grows as the FOURTH power, which buys 40-odd dB back for
// four multiply-adds. The suite measures it both ways at ±12 semitones and
// prints both numbers, so the claim is a figure and not an adjective.
//
// Catmull-Rom, not Hermite with a tension knob and not a windowed sinc: it is
// the interpolating cubic through four equally spaced points, it needs no
// state, and it is exact on the sample grid — which matters, because a sampler
// played at unity must return the file BIT-IDENTICALLY rather than nearly.
// ---------------------------------------------------------------------------

// One sample of one channel, with the index clamped into the buffer. The clamp
// is the edge policy: the first and last frames extend outwards, so the
// interpolator never reads past either end and a sample that starts loud does
// not get a phantom pre-echo from wrapped memory.
inline f32 smAt(const f32* d, i64 frames, int ch, i64 i, int c) {
    if (i < 0) i = 0;
    else if (i >= frames) i = frames - 1;
    return d[(size_t)(i * (i64)ch + (i64)c)];
}

// Both channels at a fractional position. `pos` is in source frames and is
// assumed >= 0 (the caller clamps).
inline void smFetch(const f32* d, i64 frames, int ch, f64 pos, f32& outL, f32& outR) {
    const i64 i  = (i64)pos;
    const f32 t  = (f32)(pos - (f64)i);
    const f32 t2 = t * t;
    const f32 t3 = t2 * t;
    const f32 c0 = -0.5f * t3 + t2 - 0.5f * t;
    const f32 c1 =  1.5f * t3 - 2.5f * t2 + 1.f;
    const f32 c2 = -1.5f * t3 + 2.f * t2 + 0.5f * t;
    const f32 c3 =  0.5f * t3 - 0.5f * t2;

    if (i >= 1 && i + 2 < frames) {
        // The interior fast path: no clamping, and the two channels share one
        // set of coefficients and one base offset.
        const f32* b = d + (size_t)((i - 1) * (i64)ch);
        outL = c0 * b[0] + c1 * b[ch] + c2 * b[2 * ch] + c3 * b[3 * ch];
        if (ch > 1) outR = c0 * b[1] + c1 * b[ch + 1] + c2 * b[2 * ch + 1] + c3 * b[3 * ch + 1];
        else        outR = outL;
        return;
    }
    outL = c0 * smAt(d, frames, ch, i - 1, 0) + c1 * smAt(d, frames, ch, i, 0)
         + c2 * smAt(d, frames, ch, i + 1, 0) + c3 * smAt(d, frames, ch, i + 2, 0);
    if (ch > 1)
        outR = c0 * smAt(d, frames, ch, i - 1, 1) + c1 * smAt(d, frames, ch, i, 1)
             + c2 * smAt(d, frames, ch, i + 1, 1) + c3 * smAt(d, frames, ch, i + 2, 1);
    else
        outR = outL;
}

// ---------------------------------------------------------------------------
// The device
// ---------------------------------------------------------------------------

class Sampler final : public InternalInstance, public SamplerControl {
public:
    explicit Sampler(const PluginDesc& d) : InternalInstance(d) {
        // ORDER IS THE CONTRACT. See the table at the top of the file.
        addIntParam("Root Note", "",   0, 127, 60);
        addIntParam("Coarse",    "st", -24, 24, 0);
        addParam   ("Fine",      "ct", -100.f, 100.f, 0.f);

        addParam    ("Start",     "",   0.f, 1.f, 0.f);
        addParam    ("End",       "",   0.f, 1.f, 1.f);
        addBoolParam("Loop",      false);
        addParam    ("Crossfade", "ms", 0.f, 50.f, 5.f);

        // Off = one-shot: the note-off is ignored and the voice plays the
        // region out. On = the key holds the sound, which is what a pitched
        // instrument sample wants.
        addBoolParam("Gate", true);

        // Log times, exactly as Spectra's are: a linear millisecond knob spends
        // nine tenths of its travel in a range nobody sets an attack to.
        addParam("Attack",  "ms", 0.1f, 5000.f, 0.5f,  true);
        addParam("Decay",   "ms", 1.f,  5000.f, 1000.f, true);
        addParam("Sustain", "",   0.f,  1.f,    1.f);
        addParam("Release", "ms", 1.f,  8000.f, 40.f,  true);

        addParam("Cutoff",     "Hz", 20.f, 20000.f, 20000.f, true);
        addParam("Resonance",  "",   0.f,  1.f,     0.1f);
        addParam("Env>Cutoff", "",  -1.f,  1.f,     0.f);
        addParam("Keytrack",   "",   0.f,  1.f,     0.f);

        addParam   ("Vel>Amp", "",   0.f, 1.f,   1.f);
        addParam   ("Glide",   "ms", 0.f, 500.f, 0.f);
        addIntParam("Voices",  "",   1, kSmMaxVoices, kSmMaxVoices);
        // 1 is unity: a file at -6 dBFS comes out at -6 dBFS, which is the
        // least surprising thing a sample player can do.
        addParam   ("Master",  "",   0.f, 2.f, 1.f);
    }

    bool prepare(f64 sampleRate, int maxBlock) override {
        sr_       = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxBlock_ = maxBlock > 0 ? maxBlock : kMaxBlock;

        for (Voice& v : voices_) v = Voice{};
        nPend_ = 0;
        ovfOff_[0] = ovfOff_[1] = ovfOff_[2] = ovfOff_[3] = 0u;
        ovfPanic_  = 0;
        haveOvf_   = false;
        age_       = 0;
        ctrl_      = 0;
        lastPitch_ = 60.f;
        havePitch_ = false;

        // prepare() is by contract called BEFORE the instance is handed to the
        // engine, so this is a moment the audio thread provably is not inside
        // this device -- which is exactly the moment reclaim() asks for.
        retired_.clear();
        return true;
    }

    // --- presets (host.h) --------------------------------------------------
    int presetCount() const override { return kSmPresetCount; }

    const char* presetName(int i) const override {
        return (i >= 0 && i < kSmPresetCount) ? kSmPresets[i].name : nullptr;
    }

    void loadPreset(int i) override {
        if (i < 0 || i >= kSmPresetCount) return;
        for (int k = 0; k < n_; ++k) setParam(k, info_[(size_t)k].def);
        const SmPreset& p = kSmPresets[i];
        for (int k = 0; k < p.n; ++k) setParam(p.set[k].id, p.set[k].v);
        // The sample is deliberately NOT touched: a preset is a way of playing,
        // and switching one must not throw away the file the user just loaded.
    }

    // --- state (host.h) ----------------------------------------------------
    // GUI thread. Empty for an empty sampler, so the project layer writes no
    // `state` key at all and a set with no sampler in it stays byte-identical
    // to what an older writer produced.
    std::string stateString() const override {
        if (path_.empty()) return {};
        std::string o = kSmTag;
        o += ";p=";
        smEsc(o, path_);
        return o;
    }

    // GUI thread. Parses, and only then acts: nothing below touches the device
    // until the whole string has been accepted, so a refusal leaves the sampler
    // exactly as it was. See the contract on host.h::setStateString.
    bool setStateString(const std::string& s) override {
        if (s.empty()) return true;                 // "no state" -- not malformed

        const std::vector<std::string> recs = smSplit(s, ';');
        if (recs.empty() || recs[0] != kSmTag) return badState(s);

        std::string path;
        int seen = 0;
        for (size_t r = 1; r < recs.size(); ++r) {
            const std::string& rec = recs[r];
            // An empty record, or one with no `tag=`, is not something this
            // writer can produce. Refuse rather than skip: skipping would make
            // "nxsmp1;;;;" a valid way of saying nothing.
            if (rec.size() < 2 || rec[1] != '=') return badState(s);
            if (rec[0] != 'p') continue;            // unknown tag: forward compatibility
            // Two `p` records are ambiguous, and choosing one of them is
            // guessing. There is no reading of "which file" that is safe to
            // guess at.
            if (++seen > 1) return badState(s);
            if (!smUnesc(rec.substr(2), path)) return badState(s);
        }
        if (seen != 1 || path.empty()) return badState(s);

        // Accepted. From here the only thing that can still go wrong is the
        // file itself, and a file that is not there is NOT a malformed state:
        // the set is correct and the machine is missing something. The path is
        // kept either way, so a save on this machine writes back the same
        // string that arrived and the set does not lose what it names.
        if (!loadFile(path)) {
            adopt(nullptr, path);
            // Once per instance, like the two warnings below it. One device
            // pointing at one file it cannot open is one line of information;
            // a loader that restores the same device twice, or a set with a
            // hundred samplers in it, must not turn that into a screen.
            if (!warnedMissing_) {
                warnedMissing_ = true;
                LOGW("sampler: %s could not be loaded; the device is silent and keeps "
                     "the path so the set still names it", path.c_str());
            }
        }
        return true;
    }

    // --- SamplerControl (host.h) -------------------------------------------
    SamplerControl* sampler() override { return this; }

    bool loadFile(const std::string& path) override {
        if (path.empty()) return false;
        if (!loadSample) {
            // The weak reference did not bind. See the note at the top of the
            // file: this build has no decoder, which is a fact about the link
            // and not about the file.
            if (!warnedNoDecoder_) {
                warnedNoDecoder_ = true;
                LOGW("sampler: this build links no audio decoder, so files cannot be "
                     "opened here; the device stays empty");
            }
            return false;
        }
        SampleRef sb = loadSample(path, sr_);
        if (!sb || sb->frames <= 0 || sb->channels <= 0) return false;
        adopt(std::move(sb), path);
        warnedMissing_ = false;      // a later miss is news again
        return true;
    }

    // GUI thread. The publish is one release store of a raw pointer; the
    // displaced buffer is RETIRED rather than freed. See the file header.
    //
    // A buffer this device cannot play is turned into "no buffer" HERE, at the
    // one door it can come through, rather than being defended against on the
    // audio thread: mono or stereo, at least one frame, and enough samples in
    // `data` to back the frame count it claims. Everything past this line may
    // assume all three.
    void adopt(SampleRef s, const std::string& path) override {
        if (s) {
            const bool ok = s->frames > 0 && (s->channels == 1 || s->channels == 2) &&
                            s->data.size() >= (size_t)(s->frames * (i64)s->channels);
            if (!ok) {
                LOGW("sampler: a buffer of %lld frames x %d channels (%zu samples) is not "
                     "something this device can play; treating it as empty",
                     (long long)s->frames, s->channels, s->data.size());
                s.reset();
            }
        }
        const SampleBuffer* raw = s ? s.get() : nullptr;
        live_.store(raw, std::memory_order_release);
        if (cur_) retired_.push_back(std::move(cur_));
        cur_  = std::move(s);
        path_ = path;
        if (retired_.size() == kRetireWarn)
            LOGW("sampler: %zu displaced sample buffers are being held; call "
                 "reclaim() while the device is idle (they cannot be freed from "
                 "here -- only the caller knows when the audio thread has let go)",
                 retired_.size());
    }

    void clearSample() override { adopt(nullptr, std::string()); }

    bool               hasSample() const override    { return cur_ != nullptr; }
    const std::string& samplePath() const override   { return path_; }
    i64                sampleFrames() const override { return cur_ ? cur_->frames : 0; }
    void               reclaim() override            { retired_.clear(); }

    // REALTIME. Called before process() for the same block. Events are QUEUED
    // and acted on inside process() at the sample they were stamped for -- the
    // determinism gate, argued at length on Spectra::midi() and in incident 6.
    void midi(const u8* data, int len, int frameOffset) override {
        if (!data || len < 1) return;
        const u8 status = (u8)(data[0] & 0xF0u);
        const int off = frameOffset < 0 ? 0 : frameOffset;
        switch (status) {
            case 0x90:
                if (len >= 3 && data[2] > 0) { queue(off, kEvOn, data[1], data[2]); return; }
                if (len >= 2) queue(off, kEvOff, data[1], 0);
                return;
            case 0x80:
                if (len >= 2) queue(off, kEvOff, data[1], 0);
                return;
            case 0xB0:
                if (len >= 2 && data[1] == 120) queue(off, kEvSoundOff, 0, 0);
                else if (len >= 2 && data[1] == 123) queue(off, kEvNotesOff, 0, 0);
                return;
            default:
                return;
        }
    }

    // REALTIME.
    void process(const f32* const* in, f32* const* out, int channels, int nframes) override {
        (void)in;
        if (channels <= 0 || nframes <= 0 || !out) return;

        const SampleBuffer* sb = live_.load(std::memory_order_acquire);

        // An instrument's input is silence, so bypass means silence out -- and
        // so does an empty sampler, which is the whole of its empty state. The
        // block cap is the promise every device in this tree makes: an absurd
        // nframes degrades to silence rather than to whatever the counters do.
        if (isBypassed() || nframes > kMaxBlock || !sb) {
            passthrough(nullptr, out, channels, nframes);
            clearSchedule();
            return;
        }

        Blk b;
        if (!readParams(b, sb)) {                   // a region with nothing in it
            passthrough(nullptr, out, channels, nframes);
            clearSchedule();
            return;
        }

        f32* dl = out[0];
        f32* dr = channels > 1 ? out[1] : nullptr;

        int ev = 0;
        for (int n = 0; n < nframes; ++n) {
            while (ev < nPend_ && pend_[ev].frame <= n) { apply(pend_[ev], b); ++ev; }
            // The overflow set, if the queue could not hold everything this
            // block brought. Applied at the LAST sample -- see applyOverflow().
            if (haveOvf_ && n == nframes - 1) applyOverflow();

            // Control tick on ABSOLUTE sample time (the Auto Filter's rule, and
            // Spectra's): the counter is a member and survives the block
            // boundary, which is what makes blocks of 1 and of 300
            // bit-identical to blocks of 256.
            if (ctrl_ <= 0) { retarget(b); ctrl_ = kCtrl; }
            --ctrl_;

            f32 accL = 0.f, accR = 0.f;
            for (Voice& v : voices_) {
                if (!v.active) continue;
                renderVoice(v, b, accL, accR);
            }

            const f32 l = accL * b.master;
            const f32 r = accR * b.master;
            if (dl) dl[n] = dr ? l : 0.5f * (l + r);
            if (dr) dr[n] = r;
        }

        // Anything stamped past the end of this block carries over with its
        // offset rebased, exactly as Spectra does it: the engine should not do
        // that, but clamping beats losing a note.
        int keep = 0;
        for (int i = ev; i < nPend_; ++i) {
            pend_[keep] = pend_[i];
            pend_[keep].frame -= nframes;
            if (pend_[keep].frame < 0) pend_[keep].frame = 0;
            ++keep;
        }
        nPend_ = keep;

        copyExtra(nullptr, out, 2, channels, nframes);
    }

private:
    static constexpr int kSmMaxVoices = 16;
    // 16 samples, 0.33 ms at 48 kHz: the Auto Filter's figure and Spectra's,
    // for the same reason. Coefficients are ramped across the gap, not stepped.
    static constexpr int kCtrl = 16;
    // Envelope->cutoff depth at full parameter travel, in octaves. Spectra's.
    static constexpr f32 kEnvCutOct = 6.f;
    static constexpr f32 kAtkAim    = 1.3f;      // see envTick
    static constexpr f32 kEnvOff    = 1e-5f;     // -100 dB: below any dither
    // The declick fade INTO the end point. A sample that does not happen to end
    // at a zero crossing -- most of them -- clicks when the read runs off it,
    // and 2 ms is short enough to be inaudible on a drum transient's tail and
    // long enough to kill the click.
    static constexpr f32 kEndFadeMs = 2.f;
    // Displaced buffers held for reclaim(). A log threshold, not a cap: see the
    // rack's kLayoutWarn for why this may not refuse a load.
    static constexpr size_t kRetireWarn = 32;

    enum : u8 { kIdle = 0, kAtk, kDec, kSus, kRel };

    struct Env { f32 v = 0.f; u8 stage = kIdle; };

    struct Voice {
        bool active = false;
        u8   note   = 0;
        f32  velAmp = 0.f;
        f64  pos    = 0.0;                 // source frames
        f32  pitch = 60.f, pitchTarget = 60.f, glideStep = 0.f;
        int  glideLeft = 0;
        // Latched at note-on. A mode change under a sounding note would make
        // the same key behave two ways in one press.
        bool gate    = true;
        bool looping = false;
        Env  env;
        dsp::SvfCoeffs fc, fInc;
        dsp::SvfState  fs[2];
        bool fSnap = true;
        u32  age = 0;
    };

    enum : u8 { kEvOn = 0, kEvOff, kEvNotesOff, kEvSoundOff };
    struct PendEv { int frame; u8 type, a, b; };

    // Everything read once per block.
    struct Blk {
        const f32* d;
        i64  frames;
        int  ch;
        f64  rateScale;            // buffer rate / engine rate
        f64  startF, endF;         // the region, in source frames
        f64  loopLen;              // endF - startF when looping, else 0
        f64  xfade;                // loop crossfade, in source frames
        f64  fadeOut;              // declick fade into endF, in source frames
        bool loop;
        bool gate;
        f32  ratio;                // root + coarse + fine, as a pitch ratio
        f32  root;
        f32  a, dec, sus, rel;
        f32  cutoff, q, fcMax, keytrack, envCut;
        f32  velDepth;
        f32  master;
        f32  glideMs;
        int  voices;
    };

    // --- parameters -> coefficients ----------------------------------------
    //
    // Returns false when the region has nothing in it (End at or below Start,
    // or fewer than four frames between them, which is the interpolator's
    // support). Silence is the right answer and is stated here rather than
    // discovered by a loop that runs zero times somewhere further down.
    bool readParams(Blk& b, const SampleBuffer* sb) {
        b.d      = sb->data.data();
        b.frames = sb->frames;
        b.ch     = sb->channels < 1 ? 1 : (sb->channels > 2 ? 2 : sb->channels);
        if (b.frames < 4) return false;
        // loadSample() always resamples to the engine rate, so this is 1 for
        // every file the device opens itself. It is not 1 for a buffer handed
        // in through adopt() at a different rate, and honouring it there costs
        // one multiply and means the pitch is right anyway.
        b.rateScale = (sb->rate > 0.0 && sr_ > 0.0) ? (sb->rate / sr_) : 1.0;

        const f64 last = (f64)(b.frames - 1);
        const f32 st = clampv(p(kSmStart), 0.f, 1.f);
        const f32 en = clampv(p(kSmEnd),   0.f, 1.f);
        b.startF = (f64)st * last;
        b.endF   = (f64)en * last;
        if (b.endF - b.startF < 4.0) return false;

        b.loop    = p(kSmLoop) >= 0.5f;
        b.gate    = p(kSmGate) >= 0.5f;
        b.loopLen = b.loop ? (b.endF - b.startF) : 0.0;

        // The crossfade reads the material JUST BEFORE the loop start, so it
        // can only be as long as there is material there -- and never more than
        // half the loop, or the two halves of the fade would overlap each
        // other. Both bounds are silent: the knob keeps its value and the
        // device uses what it can.
        const f64 xf = (f64)clampv(p(kSmXfade), 0.f, 50.f) * 1e-3 * sr_ * b.rateScale;
        b.xfade = 0.0;
        if (b.loop && xf > 0.0) {
            b.xfade = xf;
            if (b.xfade > b.startF)        b.xfade = b.startF;
            if (b.xfade > b.loopLen * 0.5) b.xfade = b.loopLen * 0.5;
            if (b.xfade < 1.0)             b.xfade = 0.0;
        }

        b.fadeOut = (f64)kEndFadeMs * 1e-3 * sr_ * b.rateScale;
        if (b.fadeOut > (b.endF - b.startF) * 0.5) b.fadeOut = (b.endF - b.startF) * 0.5;
        if (b.fadeOut < 1.0) b.fadeOut = 1.0;

        b.root  = clampv(p(kSmRoot), 0.f, 127.f);
        b.ratio = std::exp2((clampv(p(kSmCoarse), -24.f, 24.f) +
                             clampv(p(kSmFine), -100.f, 100.f) * 0.01f) * (1.f / 12.f));

        b.a   = atkCoef(clampv(p(kSmAttack), 0.1f, 5000.f));
        b.dec = decCoef(clampv(p(kSmDecay), 1.f, 5000.f));
        b.sus = clampv(p(kSmSustain), 0.f, 1.f);
        b.rel = decCoef(clampv(p(kSmRelease), 1.f, 8000.f));

        b.cutoff = clampv(p(kSmCutoff), 20.f, 20000.f);
        // Resonance 0..1 -> Q 0.5..20 geometrically, as the Auto Filter and
        // Spectra map it: linear spends most of its travel below audibility.
        b.q        = 0.5f * std::pow(40.f, clampv(p(kSmRes), 0.f, 1.f));
        b.fcMax    = (f32)(sr_ * 0.45);
        b.envCut   = clampv(p(kSmEnvCut), -1.f, 1.f);
        b.keytrack = clampv(p(kSmKeytrack), 0.f, 1.f);

        b.velDepth = clampv(p(kSmVel), 0.f, 1.f);
        b.glideMs  = clampv(p(kSmGlide), 0.f, 500.f);
        b.voices   = (int)clampv(p(kSmVoices) + 0.5f, 1.f, (f32)kSmMaxVoices);
        b.master   = clampv(p(kSmMaster), 0.f, 2.f);
        return true;
    }

    // Spectra's coefficients, and the same two constants: the attack aims past
    // its own target so the curve is exponential rather than asymptotic, and
    // decay/release reach a thousandth of their span in the time asked for.
    f32 atkCoef(f32 ms) const {
        const f32 n = std::fmax(1.f, (f32)(ms * 1e-3 * sr_));
        return clampv(1.f - std::exp(-1.4663371f / n), 1e-7f, 1.f);
    }
    f32 decCoef(f32 ms) const {
        const f32 n = std::fmax(1.f, (f32)(ms * 1e-3 * sr_));
        return clampv(1.f - std::exp(-6.9077553f / n), 1e-7f, 1.f);
    }

    // --- the MIDI queue -----------------------------------------------------
    //
    // AUDIT-3 F3. The queue used to drop EVERYTHING once it was full, panics
    // included, and a panic is the one message whose entire job is to stop a
    // voice that is already sounding. A flood that filled the queue with
    // note-ons and then lost the All Notes Off behind them left up to sixteen
    // voices on for the rest of the session.
    //
    // Two halves, and they answer different failures:
    //
    //  1. RESERVED CAPACITY. Note-ons may fill at most kOnCap of the kPend
    //     slots; note-offs and panics may use all of them. A dropped note-on
    //     costs one note that does not sound, which is recoverable and
    //     inaudible in a flood this dense. A dropped OFF costs a voice that
    //     never stops, which is not. This alone covers every stream that has
    //     any relationship to music, and it keeps those events in the QUEUE --
    //     stamped, ordered and applied at their own sample, so incident 6's
    //     property is untouched below the threshold.
    //
    //  2. AN OVERFLOW SET THAT CANNOT OVERFLOW. Past that, an off is folded
    //     into a 128-bit note mask and a panic into a two-state flag. Both are
    //     O(1), so no flood of any length can lose one.
    //
    // WHY THE OVERFLOW SET LANDS AT THE LAST SAMPLE, and not at frame 0 as the
    // audit sketched. Frame 0 is BEFORE every event still in the queue, so an
    // overflowed note-off for a note whose note-on is queued at frame 300 would
    // be applied to a voice that does not exist yet, and the note would stick
    // -- reintroducing the exact bug being fixed. The last sample is ordered
    // after everything the queue holds, so nothing can outrun it.
    //
    // And on determinism: applying an overflowed event late is not block-size
    // invariant. Nothing can be. The queue is per-block, so a block of 1 can
    // never overflow and a block of 1024 can, which means overflow BEHAVIOUR is
    // block-size dependent by construction whatever we do with it. The gate
    // that survives past the threshold is not bit-identity -- it is "no voice
    // is left sounding", and that one is absolute.
    void queue(int frame, u8 type, u8 a, u8 b) {
        if (type == kEvOn) {
            if (nPend_ >= kOnCap) return;              // droppable, by the argument above
        } else if (nPend_ >= kPend) {
            if (type == kEvOff) {
                ovfOff_[(a >> 5) & 3] |= 1u << (a & 31);
            } else if (type == kEvSoundOff) {
                ovfPanic_ = 2;                          // the stronger of the two wins
            } else if (ovfPanic_ == 0) {
                ovfPanic_ = 1;
            }
            haveOvf_ = true;
            return;
        }
        PendEv& e = pend_[nPend_];
        e.frame = frame;
        e.type  = type;
        e.a     = a;
        e.b     = b;
        ++nPend_;
    }

    void applyOverflow() {
        for (int w = 0; w < 4; ++w) {
            u32 bits = ovfOff_[w];
            while (bits) {
                const int bit = __builtin_ctz(bits);
                bits &= bits - 1u;
                noteOff((u8)(w * 32 + bit));
            }
            ovfOff_[w] = 0u;
        }
        if (ovfPanic_ == 2)      allSoundOff();
        else if (ovfPanic_ == 1) allNotesOff();
        ovfPanic_ = 0;
        haveOvf_  = false;
    }

    void apply(const PendEv& e, const Blk& b) {
        switch (e.type) {
            case kEvOn:       noteOn(e.a, e.b, b); break;
            case kEvOff:      noteOff(e.a); break;
            case kEvNotesOff: allNotesOff(); break;
            default:          allSoundOff(); break;
        }
    }

    void clearSchedule() {
        nPend_ = 0;
        ovfOff_[0] = ovfOff_[1] = ovfOff_[2] = ovfOff_[3] = 0u;
        ovfPanic_  = 0;
        haveOvf_   = false;
    }

    // --- voices -------------------------------------------------------------

    void noteOn(u8 note, u8 vel, const Blk& b) {
        Voice& v = *alloc(b.voices);
        v = Voice{};
        v.active = true;
        v.note   = note;
        // Depth 0 means velocity does nothing at all (every note at unity),
        // depth 1 means the note IS its velocity. Linear, because a sampler's
        // job is to play back what it was given and a curve here would be one
        // more thing between the file and the speaker.
        const f32 depth = b.velDepth;
        v.velAmp = 1.f - depth + depth * ((f32)vel * (1.f / 127.f));

        v.gate    = b.gate;
        // One-shot IGNORES the loop, and that is a rule rather than an
        // oversight: a one-shot never sees a note-off, so a looping one would
        // sound until the transport stopped. Every loop the device can play is
        // reachable in gate mode; no loop that a user could get stuck on is
        // reachable at all.
        v.looping = v.gate && b.loop;

        v.pitchTarget = (f32)note;
        const f32 glideMs = b.glideMs;
        if (glideMs > 0.f && havePitch_) {
            // Constant TIME, exactly as Spectra's: the whole interval is
            // covered in glideMs whatever the interval is.
            const int nsteps = (int)(glideMs * 1e-3 * sr_);
            if (nsteps > 0) {
                v.pitch     = lastPitch_;
                v.glideStep = (v.pitchTarget - v.pitch) / (f32)nsteps;
                v.glideLeft = nsteps;
            } else {
                v.pitch = v.pitchTarget;
            }
        } else {
            v.pitch = v.pitchTarget;
        }
        lastPitch_ = v.pitchTarget;
        havePitch_ = true;

        // The read starts at the Start point, taken from the BLOCK rather than
        // re-derived from the parameter: the block's copy is the one every
        // other position calculation in this callback uses, and a note that
        // started from a different number than the one the loop compares
        // against is exactly the kind of half-sample disagreement that only
        // shows up as a click on one machine.
        v.pos = b.startF;

        v.env.stage = kAtk;
        v.env.v     = 0.f;
        v.fs[0].reset();
        v.fs[1].reset();
        v.fSnap = true;
        v.age   = ++age_;
    }

    // Newest matching voice first, like Spectra's: a repeated note that stole
    // its own older voice should release the one actually sounding.
    //
    // A ONE-SHOT VOICE IS NOT RELEASED. That is what one-shot means, and it is
    // checked here rather than at the caller so that a panic (below) can still
    // stop it -- a mode is a statement about the key, not about the panic.
    void noteOff(u8 note) {
        Voice* best = nullptr;
        for (Voice& v : voices_) {
            if (!v.active || v.note != note || !v.gate) continue;
            if (v.env.stage == kRel) continue;
            if (!best || v.age > best->age) best = &v;
        }
        if (best) release(*best);
    }

    static void release(Voice& v) { if (v.env.stage != kRel) v.env.stage = kRel; }

    // A panic stops a one-shot too: "all notes off" has to mean all of them, or
    // it is not the thing that rescues a stuck session.
    void allNotesOff() { for (Voice& v : voices_) if (v.active && v.env.stage != kRel) release(v); }
    void allSoundOff() { for (Voice& v : voices_) v = Voice{}; }

    // Free voice inside the polyphony cap if there is one, otherwise the
    // QUIETEST -- Spectra's rule and Spectra's reasoning: it is the voice a
    // listener is least likely to miss, and a releasing voice wins on amplitude
    // automatically. Voices already sounding above a freshly lowered cap are
    // left alone to finish.
    // `cap` comes from the block struct rather than from a p() read here. That
    // is not tidiness: `kSmMaxVoices` (the array bound, 16) and `kSmVoices`
    // (the parameter id, 18) are one character apart, the class member shadows
    // the namespace-scope enumerator inside these braces, and the first version
    // of this line read parameter 16 -- Vel>Amp -- as the polyphony cap. Every
    // sampler was monophonic and nothing said so. The device suite's "three
    // notes on three voices" check is what caught it; taking the number from
    // one place is what stops it coming back.
    Voice* alloc(int cap) {
        if (cap < 1) cap = 1;
        if (cap > kSmMaxVoices) cap = kSmMaxVoices;
        Voice* quietest = &voices_[0];
        f32 best = 1e30f;
        for (int i = 0; i < cap; ++i) {
            Voice& v = voices_[(size_t)i];
            if (!v.active) return &v;
            const f32 amp = v.env.v * v.velAmp;
            if (amp < best) { best = amp; quietest = &v; }
        }
        return quietest;
    }

    // --- per-voice cutoff ---------------------------------------------------

    f32 voiceCutoff(const Voice& v, const Blk& b) const {
        const f32 kt = b.keytrack * ((f32)v.note - 60.f) * (1.f / 12.f);
        const f32 ev = b.envCut * v.env.v * kEnvCutOct;
        return clampv(b.cutoff * std::exp2(kt + ev), 20.f, b.fcMax);
    }

    void retarget(const Blk& b) {
        const f32 invk = 1.f / (f32)kCtrl;
        for (Voice& v : voices_) {
            if (!v.active || v.fSnap) continue;
            const dsp::SvfCoeffs tgt = dsp::svfCoeffs(sr_, voiceCutoff(v, b), b.q);
            v.fInc = dsp::svfSlope(v.fc, tgt, invk);
        }
    }

    // --- the voice ----------------------------------------------------------

    inline void renderVoice(Voice& v, const Blk& b, f32& accL, f32& accR) {
        envTick(v.env, b.a, b.dec, b.sus, b.rel);
        if (v.env.stage == kIdle) { v.active = false; return; }

        if (v.glideLeft > 0) {
            v.pitch += v.glideStep;
            if (--v.glideLeft == 0) v.pitch = v.pitchTarget;
        }

        // The read position, guarded on both sides. Below the start (the knob
        // moved under a sounding voice) is clamped rather than wrapped; at or
        // past the end a non-looping voice is finished.
        f64 pos = v.pos;
        if (pos < 0.0) pos = 0.0;

        f32 g = 1.f;
        if (!v.looping) {
            const f64 left = b.endF - pos;
            if (left <= 0.0) { v.active = false; return; }
            if (left < b.fadeOut) g = (f32)(left / b.fadeOut);
        }

        f32 sl, sr;
        smFetch(b.d, b.frames, b.ch, pos, sl, sr);

        // The loop crossfade: over the last `xfade` frames before the end, mix
        // in what sits the same distance before the START -- which is the
        // material the read is about to jump to. At the wrap the mix is exactly
        // 1, so the two sides meet at the same sample and the splice is
        // continuous rather than merely quiet.
        if (v.looping && b.xfade > 0.0) {
            const f64 toEnd = b.endF - pos;
            if (toEnd < b.xfade && toEnd >= 0.0) {
                f32 al, ar;
                f64 back = pos - b.loopLen;
                if (back < 0.0) back = 0.0;
                smFetch(b.d, b.frames, b.ch, back, al, ar);
                const f32 t = (f32)(1.0 - toEnd / b.xfade);
                sl += (al - sl) * t;
                sr += (ar - sr) * t;
            }
        }

        // Advance. Pitch is a ratio against the root note; the rate scale is 1
        // for anything this device decoded itself.
        const f32 semis = v.pitch - b.root;
        const f64 inc = (f64)(b.ratio * std::exp2(semis * (1.f / 12.f))) * b.rateScale;
        v.pos = pos + inc;
        if (v.looping && b.loopLen > 0.0 && v.pos >= b.endF) {
            v.pos -= b.loopLen;
            // A loop shorter than one output sample would otherwise leave the
            // position past the end forever. Snapping to the start is total.
            if (v.pos >= b.endF || v.pos < b.startF) v.pos = b.startF;
        }

        f32 xl = sl * g;
        f32 xr = sr * g;

        // Filter. Coefficients are snapped on a voice's very first sample (one
        // tan per note-on) and walked between control ticks after that.
        if (v.fSnap) {
            v.fc    = dsp::svfCoeffs(sr_, voiceCutoff(v, b), b.q);
            v.fInc  = dsp::SvfCoeffs{ 0.f, 0.f, 0.f, 0.f };
            v.fSnap = false;
        }
        const dsp::SvfOut ol  = dsp::svfTick(v.fc, v.fs[0], xl);
        const dsp::SvfOut orr = dsp::svfTick(v.fc, v.fs[1], xr);
        dsp::svfStep(v.fc, v.fInc);

        const f32 amp = v.env.v * v.velAmp;
        accL += ol.lp * amp;
        accR += orr.lp * amp;
    }

    // Spectra's exponential ADSR, with ONE deliberate difference: a sustain of
    // exactly zero ends the voice instead of parking it silent forever. Spectra
    // can afford to hold such a voice because a note-off is always coming for
    // it; a one-shot never sees one, so "Drum Tight" (sustain 0) would leak a
    // voice per hit until the polyphony cap recycled it.
    static inline void envTick(Env& e, f32 a, f32 d, f32 s, f32 r) {
        switch (e.stage) {
            case kAtk:
                e.v += (kAtkAim - e.v) * a;
                if (e.v >= 1.f) { e.v = 1.f; e.stage = kDec; }
                break;
            case kDec:
                e.v += (s - e.v) * d;
                if (e.v - s < 1e-6f) { e.v = s; e.stage = kSus; }
                break;
            case kSus:
                e.v = s;
                if (s <= 0.f) e.stage = kIdle;
                break;
            case kRel:
                e.v -= e.v * r;
                if (e.v < kEnvOff) { e.v = 0.f; e.stage = kIdle; }
                break;
            default:
                e.v = 0.f;
                break;
        }
    }

    // --- state --------------------------------------------------------------

    // One log line per instance, whatever a file throws at it: a corrupt set
    // with a hundred samplers in it must not turn into a hundred screens of
    // identical warnings that bury everything else.
    bool badState(const std::string& s) {
        if (!warnedBadState_) {
            warnedBadState_ = true;
            LOGW("sampler: device state did not parse (%zu bytes), sample not restored",
                 s.size());
        }
        return false;
    }

    // --- members ------------------------------------------------------------

    // 128 slots. A note-on may take at most kOnCap of them; see queue().
    static constexpr int kPend  = 128;
    static constexpr int kOnCap = 96;
    PendEv pend_[kPend]{};
    int    nPend_ = 0;

    // The overflow set. 128 bits of note-off plus a two-state panic, so neither
    // can ever be lost however long the flood is.
    u32  ovfOff_[4]  = {};
    u8   ovfPanic_   = 0;
    bool haveOvf_    = false;

    Voice voices_[kSmMaxVoices];
    u32   age_  = 0;
    int   ctrl_ = 0;
    f32   lastPitch_ = 60.f;
    bool  havePitch_ = false;

    // The sample. `live_` is what the audio thread reads; `cur_` is what keeps
    // it alive; `retired_` is what a swap displaced and only reclaim() may
    // free. See the file header.
    std::atomic<const SampleBuffer*> live_{nullptr};
    SampleRef                        cur_;
    std::vector<SampleRef>           retired_;
    std::string                      path_;

    bool warnedBadState_  = false;
    bool warnedNoDecoder_ = false;
    bool warnedMissing_   = false;
};

constexpr const char* kSamplerUri = "nxtakt:sampler";

PluginDesc samplerDesc() {
    PluginDesc d;
    d.format     = PluginFormat::Internal;
    d.uri        = kSamplerUri;
    d.name       = "Sampler";
    d.vendor     = "NxTakt";
    d.category   = "Instrument";
    d.kind       = PluginKind::Instrument;
    d.audioIn    = 0;
    d.audioOut   = 2;
    d.hasMidiIn  = true;
    d.paramCount = kSmParamCount;
    return d;
}

} // namespace
} // namespace detail
} // namespace lat

#endif // LAT_SAMPLER_IN_INTERNAL_DEVICES
