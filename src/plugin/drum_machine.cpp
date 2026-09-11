#include "drum_machine.h"
#include "internal_base.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace lat::detail {
namespace {
constexpr double kTau = 6.2831853071795864769;
constexpr int kVoices = 8;
constexpr int kEvents = 512;
constexpr int kNotes[kVoices] = {36, 38, 39, 42, 46, 45, 37, 56};
constexpr const char* kNames[kVoices] = {
    "Kick", "Snare", "Clap", "Closed hat", "Open hat", "Tom", "Rim", "Cowbell"
};
// Time to approximately -60 dB, in seconds. Kit distinctions are deliberately
// designed synthesis recipes, not attempts to reproduce any recorded sample.
constexpr float kLengths[3][kVoices] = {
    {0.80f, 0.28f, 0.25f, 0.065f, 0.60f, 0.50f, 0.08f, 0.32f},
    {0.35f, 0.18f, 0.16f, 0.050f, 0.28f, 0.24f, 0.06f, 0.20f},
    {0.55f, 0.26f, 0.22f, 0.085f, 0.45f, 0.38f, 0.07f, 0.25f}
};

class DrumMachine final : public InternalInstance {
public:
    explicit DrumMachine(const PluginDesc& desc) : InternalInstance(desc) {
        addIntParam("Kit", 0, 2, 0);
        addParam("Output", "dB", -60.f, 6.f, -6.f);
        for (int i = 0; i < kVoices; ++i) {
            char name[48];
            std::snprintf(name, sizeof name, "%s level", kNames[i]);
            addParam(name, "dB", -60.f, 6.f, 0.f);
            std::snprintf(name, sizeof name, "%s tune", kNames[i]);
            addParam(name, "st", -12.f, 12.f, 0.f);
            std::snprintf(name, sizeof name, "%s decay", kNames[i]);
            addParam(name, "", 0.1f, 2.f, 1.f);
        }
    }

    bool prepare(f64 sampleRate, int maxBlock) override {
        sr_ = std::isfinite(sampleRate) && sampleRate >= 8000.0 && sampleRate <= 384000.0
            ? sampleRate : 48000.0;
        maxBlock_ = maxBlock > 0 ? maxBlock : kMaxBlock;
        voices_ = {};
        eventCount_ = 0;
        return true;
    }

    // MIDI is collected before process(). A bounded stable insertion preserves
    // sample offsets and arrival order, including multiple hits in one block.
    // Events beyond a short block carry forward; none are clamped to its end.
    void midi(const u8* data, int len, int frameOffset) override {
        if (!data || len < 1) return;
        const int status = data[0] & 0xf0;
        if (status == 0xb0 && len >= 3 && (data[1] == 120 || data[1] == 123)) {
            enqueue({std::max(0, frameOffset), -1, 0});
            return;
        }
        // Note-off and velocity-zero note-on do not cut drum one-shots short.
        if (status != 0x90 || len < 3 || data[2] == 0 || data[2] > 127) return;
        for (int v = 0; v < kVoices; ++v) {
            if (data[1] == kNotes[v]) {
                enqueue({std::max(0, frameOffset), v, data[2]});
                return;
            }
        }
    }

    void process(const f32* const*, f32* const* out, int channels, int nframes) override {
        if (!out || channels <= 0 || nframes <= 0) return;
        if (isBypassed()) {
            passthrough(nullptr, out, channels, nframes);
            voices_ = {};
            eventCount_ = 0;
            return;
        }
        const int kit = (int)std::lround(safeParam(0, 0.f));
        const float output = gain(safeParam(1, -6.f));
        std::array<float, kVoices> levels{}, tunes{}, decays{};
        for (int v = 0; v < kVoices; ++v) {
            levels[v] = gain(safeParam(2 + v * 3, 0.f));
            tunes[v] = std::exp2(safeParam(3 + v * 3, 0.f) / 12.f);
            decays[v] = safeParam(4 + v * 3, 1.f);
        }
        int consumed = 0;
        for (int frame = 0; frame < nframes; ++frame) {
            while (consumed < eventCount_ && events_[consumed].offset <= frame) {
                const Event& event = events_[consumed++];
                if (event.voice < 0) voices_ = {};
                else trigger(event.voice, event.velocity, kit, tunes[event.voice], decays[event.voice]);
            }
            float mixed = 0.f;
            for (int v = 0; v < kVoices; ++v)
                if (voices_[v].active) mixed += renderVoice(v) * levels[v];
            // A bounded, zero-latency output stage keeps simultaneous accents
            // safe without flattening the individual drum's velocity response.
            const float sample = std::tanh(mixed * output * 0.65f);
            for (int c = 0; c < channels; ++c) if (out[c]) out[c][frame] = sample;
        }
        for (int i = consumed; i < eventCount_; ++i) {
            events_[i - consumed] = events_[i];
            events_[i - consumed].offset -= nframes;
        }
        eventCount_ -= consumed;
    }

private:
    struct Event { int offset = 0, voice = 0, velocity = 0; };
    struct Voice {
        bool active = false;
        int kit = 0;
        u32 age = 0, noise = 1;
        std::array<double, 6> phase{};
        float velocity = 0.f, tune = 1.f;
        float env = 0.f, transient = 0.f, pitch = 0.f;
        float envCoeff = 0.f, transientCoeff = 0.f, pitchCoeff = 0.f;
        float lowNoise = 0.f, smoothNoise = 0.f;
        float lowCoeff = 0.f, highCoeff = 0.f;
        float heldNoise = 0.f;
        int hold = 0, holdPeriod = 1;
    };
    std::array<Voice, kVoices> voices_{};
    std::array<Event, kEvents> events_{};
    int eventCount_ = 0;

    float safeParam(int index, float fallback) const {
        const float value = p(index);
        return std::isfinite(value) ? clampv(value, info_[index].min, info_[index].max) : fallback;
    }
    static float gain(float db) { return std::pow(10.f, db * 0.05f); }
    float decayCoefficient(float seconds) const {
        return (float)std::exp(-6.90775527898 / (std::max(0.001f, seconds) * sr_));
    }
    void enqueue(Event event) {
        if (eventCount_ == kEvents) {
            // Overload drops new hits. A panic must still silence everything.
            if (event.voice < 0) { voices_ = {}; eventCount_ = 0; }
            else return;
        }
        int at = eventCount_++;
        while (at > 0 && events_[at - 1].offset > event.offset) {
            events_[at] = events_[at - 1];
            --at;
        }
        events_[at] = event;
    }
    void trigger(int index, int velocity, int kit, float tune, float decay) {
        // Hats share a choke group. Processing order at equal offsets is MIDI
        // arrival order, matching the rest of the instrument's event contract.
        if (index == 3 || index == 4) {
            voices_[3].active = false;
            voices_[4].active = false;
        }
        Voice& v = voices_[index];
        v = {};
        v.active = true;
        v.kit = clampv(kit, 0, 2);
        v.velocity = std::pow((float)velocity / 127.f, 1.15f);
        v.tune = tune;
        v.env = v.transient = v.pitch = 1.f;
        v.envCoeff = decayCoefficient(kLengths[v.kit][index] * decay);
        v.transientCoeff = decayCoefficient((index == 0 ? 0.018f : 0.060f) * decay);
        v.pitchCoeff = (float)std::exp(-1.0 / (0.022 * sr_));
        v.lowCoeff = (float)(1.0 - std::exp(-kTau * 1200.0 / sr_));
        v.highCoeff = (float)(1.0 - std::exp(-kTau * 7000.0 / sr_));
        v.holdPeriod = std::max(1, (int)(sr_ / 14000.0));
        v.noise = 0x9e3779b9u ^ ((u32)(index + 1) * 0x85ebca6bu) ^ ((u32)v.kit * 0xc2b2ae35u);
    }
    float oscillator(Voice& v, int slot, float hz, bool square = false) const {
        // Nyquist protection matters for metallic oscillators at high tuning.
        const double frequency = std::min((double)hz * v.tune, sr_ * 0.42);
        v.phase[slot] += frequency / sr_;
        v.phase[slot] -= std::floor(v.phase[slot]);
        const float sine = (float)std::sin(kTau * v.phase[slot]);
        return square ? (sine >= 0.f ? 1.f : -1.f) : sine;
    }
    float renderVoice(int index) {
        Voice& v = voices_[index];
        v.noise ^= v.noise << 13;
        v.noise ^= v.noise >> 17;
        v.noise ^= v.noise << 5;
        float noise = (float)(v.noise >> 8) * (2.f / 16777216.f) - 1.f;
        if (v.kit == 1) {
            // Deliberate short, grainy digital texture: original noise is held
            // and quantized, not read from a sampled drum-machine recording.
            if (v.hold-- <= 0) {
                v.heldNoise = std::round(noise * 63.f) / 63.f;
                v.hold = v.holdPeriod - 1;
            }
            noise = v.heldNoise;
        }
        v.lowNoise += v.lowCoeff * (noise - v.lowNoise);
        v.smoothNoise += v.highCoeff * (noise - v.smoothNoise);
        const float high = noise - v.lowNoise;
        const float band = v.smoothNoise - v.lowNoise;
        const float ageSeconds = (float)(v.age / sr_);
        float sound = 0.f;
        switch (index) {
        case 0: { // Pitch-swept membrane and a short beater transient.
            const float base = v.kit == 0 ? 49.f : v.kit == 1 ? 65.f : 55.f;
            const float sweep = v.kit == 0 ? 110.f : v.kit == 1 ? 80.f : 170.f;
            const float body = oscillator(v, 0, base + sweep * v.pitch);
            const float shaped = v.kit == 2 ? std::tanh(body * 1.7f) : body;
            sound = shaped * v.env + noise * v.transient * (v.kit == 0 ? 0.10f : 0.24f);
            break;
        }
        case 1: { // Two drum modes plus snare-wire noise.
            const float body = 0.50f * oscillator(v, 0, v.kit == 1 ? 220.f : 180.f)
                             + 0.25f * oscillator(v, 1, v.kit == 2 ? 345.f : 330.f);
            sound = body * v.transient + band * v.env * (v.kit == 0 ? 0.90f : 1.35f);
            break;
        }
        case 2: { // Three hand attacks, followed by the room/noise tail.
            const float interval = v.kit == 1 ? 0.008f : 0.011f;
            const float burstAge = std::fmod(ageSeconds, interval);
            const float attacks = ageSeconds < interval * 3.f
                                ? std::exp(-burstAge * 700.f) : 0.f;
            sound = band * (0.55f * v.env + 0.85f * attacks) * v.env;
            break;
        }
        case 3:
        case 4: { // Inharmonic metal, with different analog/digital balances.
            constexpr float frequencies[6] = {263.f, 397.f, 571.f, 821.f, 1223.f, 1733.f};
            float metal = 0.f;
            for (int i = 0; i < 6; ++i)
                metal += oscillator(v, i, frequencies[i] * (v.kit == 2 ? 1.21f : 1.f), true);
            metal /= 6.f;
            sound = (v.kit == 1 ? high * 0.90f
                     : high * 0.60f + metal * (v.kit == 0 ? 0.38f : 0.55f)) * v.env * 0.58f;
            break;
        }
        case 5: {
            const float body = oscillator(v, 0, (v.kit == 1 ? 145.f : 115.f) + 70.f * v.pitch);
            sound = (body + 0.24f * oscillator(v, 1, 183.f)) * v.env * 0.8f
                  + noise * v.transient * 0.09f;
            break;
        }
        case 6:
            sound = (0.6f * oscillator(v, 0, v.kit == 1 ? 510.f : 430.f)
                   + 0.4f * oscillator(v, 1, 1710.f)) * v.env
                  + high * v.transient * 0.18f;
            break;
        case 7:
            sound = (0.55f * oscillator(v, 0, v.kit == 1 ? 620.f : 540.f, v.kit != 1)
                   + 0.45f * oscillator(v, 1, v.kit == 2 ? 845.f : 800.f, v.kit != 1)) * v.env * 0.55f;
            break;
        }
        v.env *= v.envCoeff;
        v.transient *= v.transientCoeff;
        v.pitch *= v.pitchCoeff;
        ++v.age;
        if (v.env < 1e-5f) v.active = false;
        // Avoid a long denormal tail on a voice whose body is still ringing.
        if (v.transient < 1e-12f) v.transient = 0.f;
        if (v.pitch < 1e-12f) v.pitch = 0.f;
        return sound * v.velocity;
    }
};
} // namespace

PluginDesc drumMachineDesc() {
    PluginDesc desc;
    desc.uri = "nxtakt:drums";
    desc.name = "Drum Machine";
    desc.vendor = "NxTakt";
    desc.category = "Drum Machine";
    desc.format = PluginFormat::Internal;
    desc.kind = PluginKind::Instrument;
    desc.audioIn = 0;
    desc.audioOut = 2;
    desc.hasMidiIn = true;
    desc.paramCount = 26;
    return desc;
}

std::unique_ptr<PluginInstance> makeDrumMachine(const PluginDesc& desc) {
    return std::make_unique<DrumMachine>(desc);
}
} // namespace lat::detail
