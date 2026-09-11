// DSP contract tests for the stock drum instrument. No audio device or plugin
// scan is needed: the exported factory returns the same PluginInstance used
// by the registry, and all events travel through its public MIDI API.
#include "../src/plugin/drum_machine.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

using namespace lat;
namespace {
constexpr int kRate = 48000;
constexpr std::array<int, 8> kNotes{36, 38, 39, 42, 46, 45, 37, 56};
int passed = 0, failed = 0;
void check(bool ok, const char* label, int kit = -1, int voice = -1) {
    if (ok) ++passed;
    else {
        ++failed;
        std::fprintf(stderr, "FAIL: %s (kit %d, voice %d)\n", label, kit, voice);
    }
}
struct Event {
    int frame;
    u8 status, a, b;
};
Event note(int frame, int pitch, int velocity = 127) {
    return {frame, 0x90, (u8)pitch, (u8)velocity};
}
struct Audio {
    std::vector<f32> left, right;
    explicit Audio(int count) : left(count), right(count) {}
};
std::unique_ptr<PluginInstance> instrument(int kit = 0, int rate = kRate) {
    auto p = detail::makeDrumMachine(detail::drumMachineDesc());
    if (!p || !p->prepare(rate, 4096)) {
        std::fprintf(stderr, "Could not prepare drum instrument\n");
        std::exit(2);
    }
    p->setParam(0, (f32)kit);
    return p;
}
Audio render(PluginInstance& p, int frames, int block, const std::vector<Event>& events = {}) {
    for (const auto& event : events) {
        const u8 data[]{event.status, event.a, event.b};
        p.midi(data, 3, event.frame);
    }
    Audio result(frames);
    std::vector<f32> zero(block, 0.f);
    for (int start = 0; start < frames; start += block) {
        const int count = std::min(block, frames - start);
        const f32* in[]{zero.data(), zero.data()};
        f32* out[]{result.left.data() + start, result.right.data() + start};
        p.process(in, out, 2, count);
    }
    return result;
}
double energy(const Audio& a, int from = 0, int to = -1) {
    if (to < 0) to = (int)a.left.size();
    double sum = 0;
    for (int i = from; i < to; ++i)
        sum += double(a.left[i]) * a.left[i] + double(a.right[i]) * a.right[i];
    return sum / std::max(1, 2 * (to - from));
}
bool bounded(const Audio& a, f32 limit) {
    for (const auto* channel : {&a.left, &a.right})
        for (f32 sample : *channel)
            if (!std::isfinite(sample) || std::abs(sample) > limit) return false;
    return true;
}
double difference(const Audio& a, const Audio& b, int from = 0) {
    if (a.left.size() != b.left.size()) return 1e30;
    double largest = 0;
    for (size_t i = (size_t)from; i < a.left.size(); ++i) {
        largest = std::max(largest, std::abs(double(a.left[i]) - b.left[i]));
        largest = std::max(largest, std::abs(double(a.right[i]) - b.right[i]));
    }
    return largest;
}
// Search a broad bass band, after the attack. This checks that a kick really
// has a bass fundamental without pinning its design to one waveform or pitch.
double kickFrequency(const Audio& a) {
    constexpr int start = kRate / 20, count = kRate / 5;
    constexpr double pi = 3.14159265358979323846;
    double bestPower = -1, bestFrequency = 0;
    for (int frequency = 20; frequency <= 250; frequency += 2) {
        double re = 0, im = 0;
        for (int n = 0; n < count; ++n) {
            const double window = 0.5 - 0.5 * std::cos(2 * pi * n / (count - 1));
            const double sample = (a.left[start + n] + a.right[start + n]) * 0.5 * window;
            const double phase = 2 * pi * frequency * n / kRate;
            re += sample * std::cos(phase);
            im += sample * std::sin(phase);
        }
        const double power = re * re + im * im;
        if (power > bestPower) { bestPower = power; bestFrequency = frequency; }
    }
    return bestFrequency;
}
void voicesAndKits() {
    std::vector<Audio> kicks;
    for (int kit = 0; kit < 3; ++kit) {
        for (int voice = 0; voice < 8; ++voice) {
            auto p = instrument(kit);
            const Audio loud = render(*p, kRate / 2, 257, {note(0, kNotes[voice])});
            check(bounded(loud, 1.00001f), "voice output is finite and bounded at defaults", kit, voice);
            check(energy(loud) > 1e-8, "supported voice produces audible energy", kit, voice);
            auto soft = instrument(kit);
            const Audio quiet = render(*soft, kRate / 2, 257, {note(0, kNotes[voice], 32)});
            check(energy(quiet) > 1e-10 && energy(loud) > energy(quiet) * 2,
                  "velocity meaningfully controls strike energy", kit, voice);
            if (voice == 0) kicks.push_back(loud);
        }
    }
    check(difference(kicks[0], kicks[1]) > 1e-4, "808 and 707 kicks are distinct");
    check(difference(kicks[1], kicks[2]) > 1e-4, "707 and 909 kicks are distinct");
    check(difference(kicks[0], kicks[2]) > 1e-4, "808 and 909 kicks are distinct");
    for (int kit = 0; kit < 3; ++kit) {
        const double frequency = kickFrequency(kicks[kit]);
        check(frequency >= 26 && frequency <= 180, "kick has a plausible bass fundamental", kit);
        auto p = instrument(kit);
        const Audio tail = render(*p, kRate * 2, 512, {note(0, 36)});
        check(energy(tail, kRate * 3 / 2, kRate * 2) < energy(tail, 0, kRate / 5) * 0.1,
              "default kick decays substantially instead of sustaining", kit);
    }
}
void determinismAndReset() {
    for (int kit = 0; kit < 3; ++kit) {
        for (int voice = 0; voice < 8; ++voice) {
            auto p = instrument(kit);
            const Audio first = render(*p, 4096, 256, {note(0, kNotes[voice], 103)});
            const Audio retrigger = render(*p, 4096, 256, {note(0, kNotes[voice], 103)});
            check(difference(first, retrigger) < 1e-6, "same voice retriggers deterministically", kit, voice);
            check(p->prepare(kRate, 4096), "prepare can reset a sounding instrument", kit, voice);
            const Audio reset = render(*p, 4096, 256, {note(0, kNotes[voice], 103)});
            check(difference(first, reset) < 1e-6, "prepare resets synthesis state while retaining parameters", kit, voice);
        }
    }
    auto p = instrument();
    const u8 data[]{0x90, 36, 127};
    p->midi(data, 3, 512);
    p->prepare(kRate, 4096);
    check(energy(render(*p, 2048, 256)) == 0, "prepare discards queued future notes");
}
void schedulingAndChoke() {
    const std::vector<Event> sequence{
        note(0, 36), note(113, 42, 90), note(510, 38), note(600, 46),
        note(900, 42), note(1023, 39), note(1501, 45), note(2048, 37), note(2601, 56)};
    for (int kit = 0; kit < 3; ++kit) {
        auto a = instrument(kit), b = instrument(kit);
        const Audio wide = render(*a, 4096, 4096, sequence);
        const Audio split = render(*b, 4096, 127, sequence);
        check(difference(wide, split) < 1e-6, "event timing and audio survive irregular block partitioning", kit);
        auto offset = instrument(kit);
        const Audio delayed = render(*offset, 2048, 256, {note(513, 38)});
        check(energy(delayed, 0, 513) == 0, "future event emits no samples before its offset", kit);
        check(energy(delayed, 513, 769) > 1e-8, "future event triggers inside the correct block", kit);
        auto stopped = instrument(kit);
        const Audio cut = render(*stopped, 2048, 333, {note(0, 46), {777, 0xB0, 120, 0}});
        check(energy(cut, 0, 777) > 1e-8 && energy(cut, 777, 2048) == 0,
              "all-sound-off takes effect at its sample offset", kit);
        auto mixed = instrument(kit), closed = instrument(kit), open = instrument(kit);
        const Audio choked = render(*mixed, kRate / 2, 257, {note(0, 46), note(4800, 42)});
        const Audio closedOnly = render(*closed, kRate / 2, 257, {note(4800, 42)});
        const Audio openOnly = render(*open, kRate / 2, 257, {note(0, 46)});
        check(energy(openOnly, 4800, 9600) > 1e-8, "open hat still has a tail at choke time", kit);
        check(difference(choked, closedOnly, 4800) < 1e-6,
              "closed hat stops the open hat tail without stopping itself", kit);
        auto simultaneous = instrument(kit), closedReference = instrument(kit);
        check(difference(render(*simultaneous, 2048, 256, {note(0, 46), note(0, 42)}),
                         render(*closedReference, 2048, 256, {note(0, 42)})) < 1e-6,
              "equal-offset hat events retain arrival order for choking", kit);
        auto reversed = instrument(kit);
        std::vector<Event> outOfOrder = sequence;
        std::reverse(outOfOrder.begin(), outOfOrder.end());
        check(difference(wide, render(*reversed, 4096, 4096, outOfOrder)) < 1e-6,
              "out-of-order submission still observes MIDI sample timestamps", kit);
    }
    auto p = instrument();
    check(energy(render(*p, 2048, 256, {note(0, 60), note(400, 0), note(800, 127),
                                      note(1000, 36, 0), {1200, 0x80, 38, 127}})) == 0,
          "unsupported pitches, zero velocity, and note-offs cannot start a voice");
}
void parameterState() {
    auto p = instrument();
    check(p->desc().uri == "nxtakt:drums" && p->desc().kind == PluginKind::Instrument && p->desc().hasMidiIn,
          "descriptor exposes a MIDI drum instrument with its stable URI");
    check(p->paramCount() == 26, "saved parameter layout has 26 stable entries");
    check(std::abs(p->getParam(1) + 6.f) < 1e-6, "default output is -6 dB");
    if (p->paramCount() != 26) return;
    std::vector<f32> state;
    for (int index = 0; index < p->paramCount(); ++index) {
        const ParamInfo& info = p->paramInfo(index);
        check(std::isfinite(info.min) && std::isfinite(info.max) && info.min <= info.def && info.def <= info.max,
              "parameter metadata has finite ordered bounds", -1, index);
        p->setParam(index, info.min - 1000);
        check(p->getParam(index) == info.min, "GUI parameter path clamps below range", -1, index);
        check(p->setParamRT(index, info.max + 1000) && p->getParam(index) == info.max,
              "realtime parameter path clamps above range", -1, index);
        f32 value = info.min + (info.max - info.min) * 0.37f;
        if (info.isInt) value = std::round(value);
        p->setParam(index, value);
        state.push_back(p->getParam(index));
    }
    auto restored = instrument();
    for (int index = 0; index < p->paramCount(); ++index) restored->setParam(index, state[index]);
    for (int index = 0; index < p->paramCount(); ++index)
        check(restored->getParam(index) == state[index], "saved scalar parameters restore unchanged", -1, index);
    const std::vector<Event> hit{note(0, 36), note(127, 38), note(511, 56)};
    check(difference(render(*p, 8192, 256, hit), render(*restored, 8192, 256, hit)) < 1e-6,
          "restoring parameter state reproduces audio");
    for (bool maximum : {false, true}) {
        auto extreme = instrument();
        for (int index = 1; index < extreme->paramCount(); ++index) {
            const auto& info = extreme->paramInfo(index);
            extreme->setParam(index, maximum ? info.max : info.min);
        }
        std::vector<Event> all;
        for (int pitch : kNotes) all.push_back(note(0, pitch));
        check(bounded(render(*extreme, kRate / 2, 257, all), 1.00001f),
              "simultaneous voices remain finite and bounded at parameter extremes");
    }
    // Rates either side of the default catch hardcoded phase/filter increments.
    for (int rate : {44100, 96000}) {
        auto atRate = instrument(2, rate);
        const Audio hitAtRate = render(*atRate, rate / 4, 251, {note(0, 36), note(0, 42)});
        check(bounded(hitAtRate, 1.00001f) && energy(hitAtRate) > 1e-8,
              "non-default sample rate produces finite audible drums", -1, rate);
    }
}
} // namespace
int main() {
    voicesAndKits();
    determinismAndReset();
    schedulingAndChoke();
    parameterState();
    std::printf("drum machine: %d checks passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
