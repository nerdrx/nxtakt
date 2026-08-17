// The shared base of every NxTakt stock device.
//
// This was the first ~170 lines of internal_devices.cpp until Spectra needed a
// file of its own. Nothing about it changed in the move except kMaxParams (see
// the constant): the extraction is mechanical, and the existing device suite is
// what says so.
//
// WHY A HEADER AND NOT A SECOND .cpp. Everything here is a class definition
// with inline bodies -- there is no code to link, so a header costs nothing and
// keeps the one property that matters: an instrument or effect can now live in
// its own file and still BE an InternalInstance, without internal_devices.cpp
// having to grow past the point where anyone can read it.
//
// Realtime rules, unchanged and inherited by every device that derives from
// this: everything the audio thread touches is a fixed-size member sized at
// construction; prepare() only recomputes coefficients; process() and midi()
// allocate nothing, lock nothing and throw nothing.
//
// Parameters are plain float stores. The GUI writes them while the audio thread
// reads them, exactly as documented on PluginInstance::setParam: a 4-byte
// aligned float cannot tear on any target we build for, and a stale read costs
// at most one block of latency.
#pragma once
#include "host.h"
#include "internal_dsp.h"

#include <cstring>

namespace lat {
namespace detail {

// --- shared base -----------------------------------------------------------
// Parameter storage, descriptor and bypass are identical for every internal
// device, so they live here and each device only writes DSP.
class InternalInstance : public PluginInstance {
public:
    explicit InternalInstance(const PluginDesc& d) : desc_(d) {}

    int              paramCount() const override     { return n_; }
    const ParamInfo& paramInfo(int i) const override { return info_[(size_t)i]; }

    f32 getParam(int i) const override {
        return (i >= 0 && i < n_) ? pv_[(size_t)i] : 0.f;
    }

    // GUI thread, concurrent with process(). See the file header.
    void setParam(int i, f32 v) override {
        if (i < 0 || i >= n_) return;
        pv_[(size_t)i] = clampv(v, info_[(size_t)i].min, info_[(size_t)i].max);
    }

    // REALTIME (host.h): the automation path. Literally setParam's body, and
    // that is the honest answer for this backend rather than a shortcut —
    // there is no queue to have a second producer on, only a clamp and a
    // 4-byte aligned plain store into pv_[], which is precisely what the file
    // header already argues is safe for the GUI-side writer. Two writers
    // instead of one changes nothing about tearing: the value a run() reads is
    // always one of the two that were written, never a mixture.
    bool setParamRT(int i, f32 v) override {
        if (i < 0 || i >= n_) return true;    // out of range, not "no RT path"
        pv_[(size_t)i] = clampv(v, info_[(size_t)i].min, info_[(size_t)i].max);
        return true;
    }

    const PluginDesc& desc() const override { return desc_; }

    // Stated explicitly rather than inherited from the default in host.h, so
    // that "these devices are sample-aligned with their own input" is a
    // property of the devices and not an accident of what the base class
    // happens to return today.
    //
    // It is true by construction for all of them but one. The Saturator is a
    // memoryless waveshaper (out[i] depends only on in[i]); Pulse and Spectra
    // are generators whose first sample of a voice lands on the frame the
    // note-on asked for; EQ Three, the Compressor and the Auto Filter are
    // recursive but causal, so their first output sample responds to their
    // first input sample; the Delay, the Chorus and the Reverb are parallel wet
    // paths whose dry component is untouched, and their delay is the effect
    // rather than a processing cost; the Utility is a per-sample gain. None of
    // those has a lookahead buffer, an FFT window or an oversampling filter,
    // which are the three things that produce latency.
    //
    // THE EXCEPTION IS THE LIMITER, which overrides this with the real figure
    // because it genuinely delays its output — see the comment on the device.
    // Any other internal device that acquires a lookahead must do the same: the
    // engine's delay compensation trusts what it is told and caches it when the
    // chain is published, so reporting 0 while actually delaying would smear
    // transients across every parallel path in the set.
    int latencyFrames() const override      { return 0; }

    void setBypassed(bool b) override       { bypassed_ = b; }
    bool bypassed() const override          { return bypassed_; }

    // REALTIME (host.h): the engine pushes this once per block before
    // process(). Plain stores only. Devices that sync to tempo read trBpm_ and
    // prefer it over their Tempo parameter whenever it is non-zero — zero
    // means "no transport has ever been pushed" (offline tools, standalone
    // tests), in which case the parameter keeps working exactly as before.
    // The parameters stay in the lists deliberately: internal ids are indices,
    // so removing one would shift every id after it and silently mis-restore
    // any set saved before the removal.
    //
    // A device written AFTER this call existed (Spectra) has no such parameter
    // and needs none: it reads trBpm_ and treats "never pushed" as its own
    // documented fallback instead of carrying a knob that only exists to be a
    // workaround.
    void setTransport(f64 bpm, f64 beat, bool playing) override {
        trBpm_ = bpm; trBeat_ = beat; trPlaying_ = playing;
    }

protected:
    // 64, raised from 16 when Spectra arrived with 42.
    //
    // This is a per-instance array bound, not an id space: ids ARE indices and
    // a saved set stores them, so raising the cap cannot disturb any existing
    // device -- every id that was 0..15 is still 0..15, and no device gains or
    // loses a parameter by the change. The cost is 48 unused ParamInfo slots
    // (a std::string pair each) on every internal instance, which is a few
    // kilobytes across a whole project and buys the one thing a fixed array
    // must buy: addParam() can never allocate, so a device's parameter list is
    // built without a heap call and process() reads a member array with no
    // indirection.
    static constexpr int kMaxParams = 64;

    // Transport as last pushed by the host; 0 BPM = never pushed.
    f64  trBpm_ = 0.0;
    f64  trBeat_ = 0.0;
    bool trPlaying_ = false;

    // Construction time only. Returns the parameter index so devices can keep
    // named constants honest.
    int addParam(const char* name, const char* unit, f32 mn, f32 mx, f32 def,
                 bool logarithmic = false) {
        if (n_ >= kMaxParams) return n_ - 1;
        ParamInfo& pi = info_[(size_t)n_];
        pi.name = name;
        pi.unit = unit;
        pi.min  = mn;
        pi.max  = mx;
        pi.def  = clampv(def, mn, mx);
        pi.isLogarithmic = logarithmic;
        pi.id   = (u32)n_;
        pv_[(size_t)n_] = pi.def;
        return n_++;
    }

    // A switch. Stored as a float like everything else -- the contract has one
    // parameter type -- with isBool set so the UI draws a toggle instead of a
    // knob with two positions.
    int addBoolParam(const char* name, bool def) {
        const int i = addParam(name, "", 0.f, 1.f, def ? 1.f : 0.f);
        info_[(size_t)i].isBool = true;
        return i;
    }

    // A stepped choice (the delay's musical division). isInt asks the UI to
    // quantise the knob; the DSP rounds anyway, because an automation lane can
    // still hand us 3.5.
    int addIntParam(const char* name, int mn, int mx, int def) {
        const int i = addParam(name, "", (f32)mn, (f32)mx, (f32)def);
        info_[(size_t)i].isInt = true;
        return i;
    }

    // The same, with a display unit. Spectra's stepped parameters are not all
    // dimensionless -- a coarse tune is semitones and wants to say so -- and
    // the four-argument form above has no room to.
    int addIntParam(const char* name, const char* unit, int mn, int mx, int def) {
        const int i = addParam(name, unit, (f32)mn, (f32)mx, (f32)def);
        info_[(size_t)i].isInt = true;
        return i;
    }

    f32 p(int i) const { return pv_[(size_t)i]; }

    // REALTIME. An OUTPUT value the device publishes for the UI to display --
    // today only the compressor's and the limiter's gain reduction.
    //
    // THE WART, stated plainly: ParamInfo has no read-only flag (host.h), so a
    // meter has to be an ordinary parameter that the device writes over. Three
    // consequences, all of them tolerable and none of them invisible:
    //   * the UI draws it as a knob the user can turn. Turning it does nothing
    //     for longer than one block, because process() overwrites it.
    //   * it is automatable. Drawing an envelope on it is equally pointless,
    //     and setParamRT's return value cannot say so.
    //   * it is persisted with the rest of the parameters, so a saved set
    //     carries whatever the meter happened to read at save time. Harmless:
    //     the next block overwrites it.
    // The fix is one bool on ParamInfo (`isOutput`) plus the UI honouring it,
    // which is a host.h change and therefore not this file's to make.
    void setReadout(int i, f32 v) {
        if (i < 0 || i >= n_) return;
        pv_[(size_t)i] = clampv(v, info_[(size_t)i].min, info_[(size_t)i].max);
    }

    // These devices model a stereo pair. A chain that hands us more channels
    // than that gets the extras copied rather than dropped -- silence would be
    // a worse answer than "unprocessed", and the engine only ever calls with 2.
    static void copyExtra(const f32* const* in, f32* const* out, int first,
                          int channels, int nframes) {
        for (int c = first; c < channels; ++c) {
            if (!out[c]) continue;
            const f32* src = in ? in[c] : nullptr;
            if (src == out[c]) continue;
            if (src) std::memcpy(out[c], src, (size_t)nframes * sizeof(f32));
            else     std::memset(out[c], 0, (size_t)nframes * sizeof(f32));
        }
    }

    // REALTIME. Bypass and "we have nothing to say" both land here.
    static void passthrough(const f32* const* in, f32* const* out, int channels, int nframes) {
        const size_t bytes = (size_t)nframes * sizeof(f32);
        for (int c = 0; c < channels; ++c) {
            if (!out[c]) continue;
            const f32* src = in ? in[c] : nullptr;
            if (src == out[c]) continue;              // in-place: already correct
            if (src) std::memcpy(out[c], src, bytes);
            else     std::memset(out[c], 0, bytes);
        }
    }

    PluginDesc desc_;
    ParamInfo  info_[kMaxParams];
    f32        pv_[kMaxParams]{};
    int        n_ = 0;
    bool       bypassed_ = false;
    f64        sr_ = 48000.0;
    int        maxBlock_ = kMaxBlock;
};

} // namespace detail
} // namespace lat
