#pragma once

// Pure, deterministic Spectra edit suggestions. The caller owns one undo
// point and applies the returned id/value pairs through its normal UI path.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace lat {

constexpr int kSpectraVariationParamCount = 137;

struct SpectraParamChange {
    int id = 0;
    float value = 0.f;
};

using SpectraParamValues = std::array<float, kSpectraVariationParamCount>;
using SpectraChangeList = std::vector<SpectraParamChange>;

namespace spectra_variation_detail {

class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed ? seed : 0x9e3779b97f4a7c15ull) {}

    std::uint32_t next() {
        std::uint64_t z = (state_ += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return static_cast<std::uint32_t>((z ^ (z >> 31)) >> 32);
    }

    float unit() { return static_cast<float>(next()) / 4294967295.f; }

private:
    std::uint64_t state_;
};

inline float bounded(float value, float lo, float hi) {
    return std::clamp(value, lo, hi);
}

inline void add(SpectraChangeList& out, const SpectraParamValues& base,
                int id, float value) {
    value = bounded(value, -1.0e6f, 1.0e6f);
    if (value != base[static_cast<std::size_t>(id)]) out.push_back({id, value});
}

inline void mutate(SpectraChangeList& out, const SpectraParamValues& base,
                   Rng& rng, int id, float lo, float hi, float strength) {
    const float target = lo + (hi - lo) * rng.unit();
    add(out, base, id, base[static_cast<std::size_t>(id)] +
                       (target - base[static_cast<std::size_t>(id)]) * strength);
}

inline void mutateLog(SpectraChangeList& out, const SpectraParamValues& base,
                      Rng& rng, int id, float lo, float hi, float strength) {
    const float target = lo * std::pow(hi / lo, rng.unit());
    const float source = std::max(base[static_cast<std::size_t>(id)], 1.0e-6f);
    add(out, base, id, std::exp(std::log(source) +
                                (std::log(target) - std::log(source)) * strength));
}

inline void mutateInt(SpectraChangeList& out, const SpectraParamValues& base,
                      Rng& rng, int id, int lo, int hi, float strength) {
    const int target = lo + static_cast<int>(rng.unit() * float(hi - lo + 1));
    const float value = base[static_cast<std::size_t>(id)] +
                        (static_cast<float>(std::clamp(target, lo, hi)) -
                         base[static_cast<std::size_t>(id)]) * strength;
    // Keep source continuity at low strength. Caller applies normal metadata
    // clamping; only the randomized target is restricted to the safe range.
    add(out, base, id, static_cast<float>(static_cast<int>(value + 0.5f)));
}

} // namespace spectra_variation_detail

// Make a bounded musical edit list. IDs 0/8 (custom tables), pitch/tuning,
// matrix routes, arp, master/output, and state-only data are intentionally
// absent. The result is suitable for one undo point plus one auto-capture pass.
inline SpectraChangeList makeSpectraVariation(const SpectraParamValues& base,
                                              std::uint64_t seed,
                                              float strength = 0.5f) {
    using namespace spectra_variation_detail;
    if (!std::isfinite(strength)) return {};
    strength = std::clamp(strength, 0.f, 1.f);
    if (strength == 0.f) return {};
    Rng rng(seed);
    SpectraChangeList out;
    out.reserve(32);

    // Oscillator character: preserve table selection and pitch tuning.
    mutate(out, base, rng, 1, 0.08f, 0.92f, strength);
    mutate(out, base, rng, 4, 0.10f, 0.90f, strength);
    mutateInt(out, base, rng, 5, 1, 4, strength);
    mutate(out, base, rng, 6, 2.f, 55.f, strength);
    mutate(out, base, rng, 7, 0.10f, 0.90f, strength);
    mutate(out, base, rng, 9, 0.08f, 0.92f, strength);
    mutate(out, base, rng, 12, 0.10f, 0.80f, strength);
    mutateInt(out, base, rng, 13, 1, 4, strength);
    mutate(out, base, rng, 14, 2.f, 55.f, strength);
    mutate(out, base, rng, 15, 0.10f, 0.90f, strength);

    // Filter, sub/noise and envelopes. Keep drive and resonance below harsh edges.
    mutate(out, base, rng, 16, 0.02f, 0.32f, strength);
    mutate(out, base, rng, 17, 0.05f, 0.48f, strength);
    mutateLog(out, base, rng, 18, 100.f, 12000.f, strength);
    mutate(out, base, rng, 19, 0.08f, 0.72f, strength);
    mutateInt(out, base, rng, 20, 0, 5, strength);
    mutate(out, base, rng, 21, 0.f, 12.f, strength);
    mutateLog(out, base, rng, 24, 1.f, 900.f, strength);
    mutateLog(out, base, rng, 25, 20.f, 1400.f, strength);
    mutate(out, base, rng, 26, 0.12f, 0.88f, strength);
    mutateLog(out, base, rng, 27, 30.f, 1800.f, strength);
    mutateLog(out, base, rng, 28, 1.f, 700.f, strength);
    mutateLog(out, base, rng, 29, 20.f, 1300.f, strength);
    mutate(out, base, rng, 30, 0.05f, 0.75f, strength);
    mutateLog(out, base, rng, 31, 20.f, 1500.f, strength);

    // Warp can add motion, but never reaches the most destructive settings.
    mutateInt(out, base, rng, 48, 0, 5, strength);
    mutate(out, base, rng, 49, 0.08f, 0.62f, strength);
    mutateInt(out, base, rng, 50, 0, 5, strength);
    mutate(out, base, rng, 51, 0.08f, 0.62f, strength);

    // Integrated effects: wet mix and feedback remain deliberately moderate.
    mutate(out, base, rng, 125, 0.04f, 0.48f, strength);
    mutate(out, base, rng, 126, 0.15f, 4.0f, strength);
    mutate(out, base, rng, 127, 0.08f, 0.62f, strength);
    mutate(out, base, rng, 128, 0.10f, 0.85f, strength);
    mutate(out, base, rng, 129, 0.04f, 0.42f, strength);
    mutateLog(out, base, rng, 130, 35.f, 700.f, strength);
    mutate(out, base, rng, 132, 0.05f, 0.48f, strength);
    mutate(out, base, rng, 133, 0.04f, 0.42f, strength);
    mutate(out, base, rng, 134, 0.10f, 0.82f, strength);
    mutate(out, base, rng, 135, 0.10f, 0.72f, strength);
    mutate(out, base, rng, 136, 0.15f, 0.88f, strength);
    return out;
}

// Macro-only variation. Exactly IDs 94..97 can appear in the result.
inline SpectraChangeList makeSpectraMacroVariation(const SpectraParamValues& base,
                                                   std::uint64_t seed,
                                                   float strength = 0.5f) {
    using namespace spectra_variation_detail;
    if (!std::isfinite(strength)) return {};
    strength = std::clamp(strength, 0.f, 1.f);
    if (strength == 0.f) return {};
    Rng rng(seed);
    SpectraChangeList out;
    for (int id = 94; id <= 97; ++id) mutate(out, base, rng, id, 0.08f, 0.92f, strength);
    return out;
}

} // namespace lat
