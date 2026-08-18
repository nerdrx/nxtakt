// Format-agnostic plugin hosting.
//
// Threading contract (mirrors audio/engine.h):
//   * PluginRegistry::scan() and instantiate() are GUI-thread only. They are
//     slow, they allocate, and they touch std::string.
//   * PluginInstance::process() is audio-thread only: no allocation, no locks,
//     no exceptions, no std::string.
//   * setParam()/setBypassed() are called from the GUI thread *while* the audio
//     thread is inside process(). Both write a single scalar that the backend
//     reads with no ordering against anything else -- a RELAXED ATOMIC, which
//     is what "a plain scalar nothing can tear" always meant and, since
//     AUDIT-3 F2, what it is also spelled as. See the note on
//     PluginInstance::setParam and the header of internal_base.h.
//
// Only LV2 is implemented today. CLAP and VST3 slot into the same three
// entry points in namespace detail; the dispatch in host.cpp already branches
// on PluginDesc::format so adding a backend touches nothing else.
#pragma once
#include "../core/common.h"
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lat {

class PluginRegistry;
class RackControl;
class SamplerControl;

// src/audio/sample.h. Named here as an INCOMPLETE type on purpose: the plugin
// layer has to be able to say "a decoded sample" (SamplerControl, below) and
// has no business including the decoder, which pulls in sndfile and libsamplerate
// — two libraries three of this tree's link targets deliberately do not have.
// A std::shared_ptr may be declared, copied, moved and destroyed with its
// element type incomplete, which is the whole of what is needed here.
struct SampleBuffer;

// Internal = NxTakt's own stock devices. They implement PluginInstance like
// any other backend, so they inherit the browser, knobs, bypass, chains and
// persistence without special cases anywhere else.
enum class PluginFormat : int { LV2 = 0, CLAP, VST3, Internal };
enum class PluginKind   : int { Effect = 0, Instrument, Unknown };

const char* formatName(PluginFormat f);
const char* kindName(PluginKind k);

// A plugin the scanner found on disk. Cheap to copy and to keep in a list;
// nothing here has been loaded yet.
struct PluginDesc {
    std::string  uri;                 // LV2: the plugin URI. Other formats: "path:index".
    std::string  name;
    std::string  vendor;
    std::string  category;
    PluginFormat format    = PluginFormat::LV2;
    PluginKind   kind      = PluginKind::Unknown;
    int          audioIn   = 0;
    int          audioOut  = 0;
    bool         hasMidiIn = false;
    int          paramCount = 0;      // control inputs, filled in by the scanner
};

// One automatable control. Ranges come from the plugin's own metadata, so
// min/max can be anything (including inverted or degenerate) — the backend
// normalises them before we get here.
struct ParamInfo {
    std::string name;
    std::string unit;                 // display symbol, e.g. "dB", "Hz". May be empty.
    f32  min = 0.f, max = 1.f, def = 0.f;
    bool isBool = false;
    bool isInt  = false;
    bool isLogarithmic = false;
    u32  id = 0;                      // backend-defined; LV2 uses the port index
};

// One loaded plugin sitting on one track.
class PluginInstance {
public:
    virtual ~PluginInstance() = default;

    // GUI thread, before the instance is handed to the engine. Returns false if
    // the plugin refused to activate at this rate/block size.
    virtual bool prepare(f64 sampleRate, int maxBlock) = 0;

    // REALTIME. `in` and `out` are arrays of `channels` pointers, each nframes
    // long. Aliasing (in[c] == out[c]) is allowed. Never allocates or locks.
    virtual void process(const f32* const* in, f32* const* out, int channels, int nframes) = 0;

    // REALTIME. Raw MIDI (status + up to two data bytes), delivered by the
    // engine before this block's process(); `frameOffset` is the sample
    // position within that block. Default no-op so effects ignore it; note-
    // capable backends (CLAP note events, LV2 atom sequences, internal
    // instruments) override. Same rules as process(): no allocation, no locks.
    virtual void midi(const u8* data, int len, int frameOffset) { (void)data; (void)len; (void)frameOffset; }

    // The transport, pushed by the engine once per block before process().
    // Realtime rules apply: implementations may store the values and no more.
    //
    // This retires the documented wart where a tempo-synced device (Delay's
    // sync divisions, Auto Filter's LFO) could only learn the BPM through a
    // user-set "Tempo" parameter. Those parameters DELIBERATELY remain in the
    // devices' parameter lists: internal ids are indices, so removing one
    // would shift every id after it and silently mis-restore any set saved
    // before the removal. They are fallbacks now — a device prefers the pushed
    // transport when it has seen one (bpm > 0) and the parameter otherwise,
    // which also keeps offline tools that never push transport working
    // unchanged. A rack forwards this to its sub-devices for free.
    virtual void setTransport(f64 bpm, f64 beat, bool playing) {
        (void)bpm; (void)beat; (void)playing;
    }

    virtual int              paramCount() const = 0;
    virtual const ParamInfo& paramInfo(int i) const = 0;
    virtual f32              getParam(int i) const = 0;

    // REALTIME. The automation path: called from inside the audio callback,
    // before this block's process(), to apply a value the engine computed from
    // a clip envelope.
    //
    // A separate entry point from setParam(), not a relaxation of it, because
    // the callers differ in the one way that matters: setParam() is the ONLY
    // writer on the non-realtime side and may therefore use a single-producer
    // queue — which is exactly what the CLAP backend does. A second producer on
    // that queue would be a data race, so a backend whose parameter path is a
    // queue must give the audio thread a path of its own.
    //
    // Returns false when this backend has no realtime parameter path. The
    // engine then marks the lane inert, emits Ev::AutoLaneInert once so the UI
    // can grey it, and never calls again for that published set. A silently
    // ignored lane would be the worst outcome: the envelope is drawn, the sound
    // does not move, and nothing says why.
    //
    // Same rules as process(): no allocation, no locks, no exceptions.
    virtual bool setParamRT(int i, f32 v) { (void)i; (void)v; return false; }

    // getParam() is realtime-safe to call: a plain load in every backend in the
    // tree. The engine needs it to remember what a parameter was before an
    // envelope took it over, so it can be restored when playback stops.

    // GUI thread, concurrent with process(). Backends store parameters as
    // single floats that the plugin reads once per run(), so a torn read is
    // impossible and a stale read costs at most one block of latency. No lock
    // is taken and no ordering is implied: two parameters written back to back
    // may be observed in either order.
    //
    // The stock devices spell that as a `std::atomic<f32>` accessed with
    // `memory_order_relaxed` (AUDIT-3 F2). Relaxed is the exact strength this
    // paragraph describes -- atomicity and nothing else -- and it compiles to
    // the same instruction a plain store did, so the contract did not change
    // when the spelling did. A backend whose parameter path is a QUEUE rather
    // than a store must still provide setParamRT() above; see its note.
    virtual void setParam(int i, f32 v) = 0;

    virtual const PluginDesc& desc() const = 0;

    // GUI thread. Factory presets: named parameter sets a device ships with.
    //
    // Deliberately the smallest thing that can work. A preset is not a new kind
    // of state — loadPreset() writes through setParam() and nothing else, so
    // the parameters move exactly as if a user had turned every knob, and
    // persistence, automation and the device strip need to learn nothing. The
    // corollary is that a preset cannot carry anything a parameter cannot; a
    // device that grows non-parameter state has to say so itself.
    //
    // Zero presets is the default and means "no selector" to the UI, so every
    // existing backend answers correctly without being touched. presetName()
    // returns a pointer into storage the instance owns for its own lifetime
    // (a string literal in every implementation today), or null out of range.
    //
    // Nothing here is realtime. LV2 and CLAP both have native preset systems
    // that could be surfaced through these same three calls later; the
    // signature was chosen so that would be an implementation, not a change.
    virtual int         presetCount() const      { return 0; }
    virtual const char* presetName(int i) const  { (void)i; return nullptr; }
    virtual void        loadPreset(int i)        { (void)i; }

    // GUI thread. Everything the device is BEYOND its parameters, as one line
    // of printable ASCII with no whitespace, no quotes and no newline — so the
    // project layer can carry it as an opaque scalar (`SavedDevice::state`) and
    // never learn what any particular device keeps in it.
    //
    // Same threading rules as the preset trio above: GUI thread only, allocates
    // freely, never called from process().
    //
    // THE DEFAULT IS "NOTHING", and that is the honest answer for almost every
    // device: a Saturator IS its three knobs. Two devices override today —
    // `nxtakt:rack` (its whole contents; reached through rack() for historical
    // reasons and carrying the identical string) and `nxtakt:sampler` (the path
    // of the file it plays, which no parameter can express).
    //
    // Returning an EMPTY string means "I have no state", and the project layer
    // must treat that as "write no `state` key" rather than as "write an empty
    // one" — a set with no such device in it then stays byte-identical to what
    // an older writer produced.
    //
    // setStateString() returns whether the string was UNDERSTOOD. The contract
    // for a device that overrides it, stated once here because it is easy to
    // get wrong:
    //
    //   * an empty string is not malformed. It means "no state", it is what
    //     stateString() answers for a fresh instance, and it must be accepted
    //     as a no-op returning true.
    //   * anything else that does not parse must be REFUSED: return false,
    //     change nothing, and leave the device exactly as it was. Guessing at a
    //     half-parsed state is how a corrupt file becomes a corrupt session.
    //   * a device may not throw and may not crash on arbitrary bytes. The
    //     string came out of a file a user can edit and a peer can write.
    //
    // ORDERING, on load: parameters FIRST, then setStateString(). This is the
    // same rule and the same trap docs/RACKS.md §Persistence documents for
    // racks — see the note at the call site in src/ui/app_project.cpp.
    virtual std::string stateString() const { return {}; }
    virtual bool setStateString(const std::string& s) { (void)s; return true; }

    // Processing latency in frames at the prepared rate/block size. Constant
    // after prepare() and audio-thread-safe to read; the engine uses it for
    // delay compensation, so a lying plugin smears transients across parallel
    // paths. LV2: the reportsLatency control-out port. CLAP: the latency
    // extension. Internal devices: 0.
    virtual int latencyFrames() const { return 0; }

    // REALTIME-safe to read; set from the GUI thread. When bypassed, process()
    // copies input to output and does not call into the plugin at all.
    virtual void setBypassed(bool b) = 0;
    virtual bool bypassed() const = 0;

    // GUI thread. Non-null only for a device that hosts a chain of its own --
    // today exactly one, `nxtakt:rack`. This is the whole of the contract
    // addition racks needed: a rack IS a PluginInstance, so the browser, the
    // device strip, bypass, automation and the chain scheduler already work on
    // it, and the only thing they cannot express is "and it has an inside".
    //
    // A virtual accessor rather than a dynamic_cast because the concrete Rack
    // lives in an anonymous namespace in internal_devices.cpp and is not a type
    // any caller can name -- and because a future CLAP/VST3 container could
    // answer the same question without being our class at all.
    virtual RackControl* rack() { return nullptr; }

    // GUI thread. Non-null only for a device that plays a FILE -- today exactly
    // one, `nxtakt:sampler`. Same shape and the same justification as rack():
    // the sampler IS a PluginInstance, so the browser, the strip, bypass,
    // automation, presets and persistence already work on it, and the one thing
    // none of them can express is "and it points at a wav on disk".
    //
    // Persistence does NOT go through here -- it goes through the
    // stateString()/setStateString() pair above, which is generic. This
    // interface exists for the two things that are not persistence: handing the
    // device a file the user just dropped on it, and handing it a buffer that
    // was decoded somewhere else.
    virtual SamplerControl* sampler() { return nullptr; }
};

// ---------------------------------------------------------------------------
// Racks
//
// A Rack is a PluginInstance that contains a chain of PluginInstances in series
// and exposes eight macro parameters, each of which drives zero or more
// parameters on the devices inside it. It needs no UI of its own to be usable:
// it appears in the browser like any other device and its macros appear in the
// device strip like any other knobs.
//
// Everything below is GUI-THREAD ONLY. The audio thread reaches a rack through
// PluginInstance alone.
// ---------------------------------------------------------------------------

inline constexpr int kRackMacros      = 8;
inline constexpr int kRackMaxDevices  = 8;
inline constexpr int kRackMaxMappings = 64;
// Racks nest (a rack is a device like any other). The cap exists so a corrupt
// or hostile saved state cannot recurse the loader off the stack.
inline constexpr int kRackMaxDepth    = 4;

// One macro -> one target parameter, over the slice of that parameter's range
// the macro should sweep.
//
// Entirely passive: no pointers, no identity, trivially copyable, safe to write
// to a file. `min > max` is legal and INVERTS the mapping -- the target moves
// down as the macro moves up. That is not an edge case to tolerate, it is half
// the reason macro knobs exist (one knob that opens a filter while it closes a
// send). min == max pins the target to a constant.
//
// The parameter is named by ParamInfo::id, not by index, because the id is what
// SavedDevice already persists and what survives a plugin gaining a parameter.
// The rack resolves id -> index once per edit, never in process().
struct RackMapping {
    int macro  = 0;      // 0 .. kRackMacros-1
    int device = 0;      // index into the rack's chain, in processing order
    u32 param  = 0;      // ParamInfo::id on that device
    f32 min    = 0.f;    // target value when the macro reads 0
    f32 max    = 1.f;    // target value when the macro reads 1
};

// The complete contents of a rack in passive form: what persistence has to
// carry, with no live object in it anywhere.
//
// Device order is chain order. `params` is deliberately the same shape as
// SavedDevice::params, (ParamInfo::id, value), so the project layer can reuse
// the code it already has for a device's parameters.
struct RackState {
    struct Device {
        std::string uri;                             // PluginDesc::uri
        std::vector<std::pair<u32, f32>> params;     // (ParamInfo::id, value)
        bool        bypass = false;
        std::string state;                           // nested rack, compact form; else empty
    };
    std::vector<Device>      devices;
    f32                      macros[kRackMacros] = {};
    std::vector<RackMapping> mappings;
};

// The compact form: one line of printable ASCII with no whitespace, no quotes
// and no newline, so it drops into any line-oriented format (project.cpp's
// `kv`) without needing that format to know anything about racks. Round-trips
// exactly; nested racks are escaped once per level.
//
// Numbers are written and read through the C locale, like the rest of the
// project format -- main.cpp pins LC_NUMERIC to "C" precisely so this is true.
std::string rackStateToString(const RackState& s);
bool        rackStateFromString(const std::string& text, RackState& out);

// The editing face of a rack. GUI THREAD ONLY, every method.
//
// Sub-device creation goes through PluginRegistry::instantiate, which is
// GUI-thread-only and allocating -- so it happens here, at edit time, and never
// in process(). A rack holds a pointer to the registry that instantiated it;
// the registry must outlive the rack.
class RackControl {
public:
    virtual ~RackControl() = default;

    virtual int             deviceCount() const = 0;
    virtual PluginInstance* device(int i) const = 0;   // null if i is out of range

    // Instantiate and splice in. `at` is clamped into the chain. Returns false
    // if the chain is full, the plugin would not load, or this rack has no
    // registry behind it. Mappings that pointed past the insertion point are
    // renumbered so they keep pointing at the same device.
    virtual bool addDevice(const PluginDesc& d) = 0;
    virtual bool insertDevice(int at, const PluginDesc& d) = 0;

    // Unlinks the device. Mappings that targeted it are dropped and the rest
    // renumbered. The instance itself is RETIRED, not destroyed -- see
    // reclaim() for why.
    virtual bool removeDevice(int i) = 0;
    virtual bool moveDevice(int from, int to) = 0;

    virtual int                mappingCount() const = 0;
    virtual const RackMapping& mapping(int i) const = 0;

    // Returns the new mapping's index, or -1 if the macro, the device or the
    // parameter does not exist. `min`/`max` are clamped into the target
    // parameter's own range on the way in (which preserves inversion), so what
    // mapping() reports back is what the macro will actually do.
    //
    // The new mapping is APPLIED: the target snaps to where the macro already
    // sits, so the knob and the macro that now owns it agree from this moment.
    // That is an edit-time property. setState() restores mappings WITHOUT it —
    // see its own note.
    virtual int  addMapping(const RackMapping& m) = 0;
    virtual bool removeMapping(int i) = 0;
    virtual void clearMacro(int macro) = 0;

    virtual RackState state() const = 0;
    // Replaces the entire contents. Restored parameter values are written
    // verbatim and NOTHING in the load path re-derives one: macros are not
    // re-applied, and the mappings are re-added structurally rather than
    // through addMapping(), which would snap. A mapped target parked off its
    // macro's curve therefore comes back parked, exactly as saved.
    virtual bool      setState(const RackState& s) = 0;

    // Frees the instances that removeDevice()/setState() unlinked.
    //
    // They are retained rather than deleted because a rack can be edited while
    // it is live in the engine, and nothing inside a PluginInstance can know
    // when the audio thread has finished with a pointer. Unlinking is safe (the
    // new topology is published atomically); deleting is not. Call this only
    // when the rack is NOT in a chain the engine is processing.
    virtual void reclaim() = 0;
};

// ---------------------------------------------------------------------------
// The Sampler
//
// The editing face of `nxtakt:sampler`. GUI THREAD ONLY, every method — the
// same rule RackControl states, for the same reason: decoding a file allocates,
// blocks on I/O and takes seconds, so it happens here, at edit time, and never
// anywhere near process().
// ---------------------------------------------------------------------------
class SamplerControl {
public:
    virtual ~SamplerControl() = default;

    // Decodes `path` at the rate the instance was prepared at and adopts the
    // result. Returns false if the file could not be read — or if this
    // executable has no decoder linked at all, which is a real case and not a
    // hypothetical: src/audio/sample.cpp is absent from the daemon, from
    // plugin_scan and from the device suite. See the note on the weak reference
    // in sampler.cpp. Either way the device is left EMPTY (and therefore
    // silent) rather than half-loaded.
    virtual bool loadFile(const std::string& path) = 0;

    // Adopts an already-decoded buffer. `path` is what stateString() will carry
    // and what a later load will re-decode; pass "" for a buffer with no file
    // behind it (a recorded take), which then persists as nothing at all.
    //
    // The buffer must be IMMUTABLE from this moment: the audio thread reads it
    // through a raw pointer, and the shared_ptr here is what keeps it alive.
    virtual void adopt(std::shared_ptr<SampleBuffer> s, const std::string& path) = 0;

    // Back to empty: no sample, no path, silence.
    virtual void clearSample() = 0;

    virtual bool               hasSample() const   = 0;
    virtual const std::string& samplePath() const  = 0;
    virtual i64                sampleFrames() const = 0;

    // The buffer the device is currently playing, or null. GUI thread, like
    // everything here.
    //
    // The read-back half of adopt(), and it exists for one caller with one
    // problem: `src/ui/engine_handle.cpp` has to put a sampler's audio into the
    // sample pool so that `nxtaktd` -- which links no decoder, on purpose --
    // can play it, and the state string names only a PATH. Re-decoding the file
    // on the handle's side would be a second decode of the same bytes, at a
    // rate it would have to re-derive, in a translation unit that has no
    // business owning a decoder (GUI-ON-DAEMON.md §15.2).
    //
    // A shared_ptr and not a raw pointer, because the caller may outlive the
    // next adopt(): the pool write is a memcpy of the whole buffer, and a
    // sampler re-pointed on another code path during it would otherwise leave
    // the copy reading a buffer that has moved to `retired_` and may already
    // have been reclaimed. Holding a reference is the only thing that makes the
    // copy safe, and it costs an atomic increment.
    virtual std::shared_ptr<SampleBuffer> sampleBuffer() const = 0;

    // Frees the buffers that loadFile()/adopt()/clearSample() displaced.
    //
    // They are retained rather than dropped for exactly the reason
    // RackControl::reclaim() gives: a sampler can be re-pointed while it is
    // live in the engine, and nothing inside a PluginInstance can know when the
    // audio thread last dereferenced the pointer it published. Swapping is safe
    // (one release store); freeing is not. Call this only when the device is
    // NOT in a chain the engine is processing. prepare() also clears them,
    // because prepare() is by contract called before the instance is handed
    // over.
    virtual void reclaim() = 0;
};

// Backend entry points. One pair per format; host.cpp dispatches to them.
namespace detail {
    void scanLV2(std::vector<PluginDesc>& out);
    std::unique_ptr<PluginInstance> instantiateLV2(const PluginDesc& d, f64 sampleRate, int maxBlock);
    void scanInternal(std::vector<PluginDesc>& out);
    // `reg` is the registry doing the instantiating, and is handed on to the
    // one internal device that needs to create devices of its own (the rack).
    // A null registry yields a rack that works but cannot be filled -- which is
    // what a caller reaching for detail:: directly, outside a registry, gets.
    std::unique_ptr<PluginInstance> instantiateInternal(const PluginDesc& d, f64 sampleRate,
                                                        int maxBlock, PluginRegistry* reg = nullptr);
    // TODO(vst3): void scanVST3(std::vector<PluginDesc>&);
}

class PluginRegistry {
public:
    // GUI thread. Slow (lilv walks every bundle on the system) and allocates.
    // Replaces the previous result wholesale; already-instantiated plugins are
    // unaffected because they hold their own copy of the descriptor.
    void scan();

    const std::vector<PluginDesc>& plugins() const { return plugins_; }

    // Exact URI first, then the permanent alias table in host.cpp: a set saved
    // before the Lattice -> NxTakt rename names its stock devices `lattice:*`
    // and must keep resolving to the `nxtakt:*` descriptors forever. Returns
    // the CANONICAL descriptor either way, so whatever the caller saves next
    // carries the current spelling.
    const PluginDesc* find(const std::string& uri) const;

    // GUI thread. Returns null if the plugin failed to load or activate.
    std::unique_ptr<PluginInstance> instantiate(const PluginDesc& d, f64 sampleRate, int maxBlock);

private:
    std::vector<PluginDesc> plugins_;
};

} // namespace lat
