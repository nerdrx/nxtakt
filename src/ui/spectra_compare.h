#pragma once

#include <cmath>
#include <string>
#include <vector>

namespace lat {

struct SpectraSoundSnapshot {
    std::vector<float> params;
    std::string state;
    bool valid = false;
};

template <class Instance>
SpectraSoundSnapshot captureSpectraSound(const Instance& instance) {
    SpectraSoundSnapshot snapshot;
    const int count = instance.paramCount();
    if (count < 0) return snapshot;
    snapshot.params.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) snapshot.params.push_back(instance.getParam(i));
    snapshot.state = instance.stateString();
    snapshot.valid = true;
    return snapshot;
}

template <class Instance>
bool restoreSpectraSound(Instance& instance, const SpectraSoundSnapshot& snapshot) {
    const int count = instance.paramCount();
    if (!snapshot.valid || count < 0 || snapshot.params.size() != static_cast<size_t>(count))
        return false;
    for (int i = 0; i < count; ++i) {
        const float value = snapshot.params[static_cast<size_t>(i)];
        const auto& info = instance.paramInfo(i);
        if (!std::isfinite(value) || value < info.min || value > info.max) return false;
    }

    std::vector<float> previous;
    previous.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) previous.push_back(instance.getParam(i));
    const std::string previousState = instance.stateString();
    for (int i = 0; i < count; ++i) instance.setParam(i, snapshot.params[static_cast<size_t>(i)]);
    if (instance.setStateString(snapshot.state.empty() ? "nxspc1" : snapshot.state)) return true;
    for (int i = 0; i < count; ++i) instance.setParam(i, previous[static_cast<size_t>(i)]);
    instance.setStateString(previousState.empty() ? "nxspc1" : previousState);
    return false;
}

} // namespace lat
