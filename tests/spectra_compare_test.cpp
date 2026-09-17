#include "../src/ui/spectra_compare.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {
struct FakeInfo { float min, max; };
struct FakeSound {
    std::vector<float> values{0.2f, 0.4f};
    std::vector<FakeInfo> infos{{0.f, 1.f}, {-1.f, 1.f}};
    std::string state = "old";
    bool acceptState = true;
    std::vector<std::string> writes;
    int paramCount() const { return static_cast<int>(values.size()); }
    const FakeInfo& paramInfo(int i) const { return infos[static_cast<size_t>(i)]; }
    float getParam(int i) const { return values[static_cast<size_t>(i)]; }
    void setParam(int i, float value) {
        values[static_cast<size_t>(i)] = value;
        writes.push_back("p" + std::to_string(i));
    }
    std::string stateString() const { return state; }
    bool setStateString(const std::string& value) {
        writes.push_back("s:" + value);
        if (!acceptState) return false;
        state = value;
        return true;
    }
};

int passed = 0;
int failed = 0;
void check(bool ok, const char* label) {
    if (ok) ++passed;
    else { ++failed; std::fprintf(stderr, "FAIL: %s\n", label); }
}
}

int main() {
    FakeSound source;
    source.values = {0.7f, -0.6f};
    source.state = "new";
    const auto snapshot = lat::captureSpectraSound(source);
    check(snapshot.valid && snapshot.params == source.values && snapshot.state == "new",
          "capture includes every parameter and state");

    FakeSound target;
    check(lat::restoreSpectraSound(target, snapshot), "valid snapshot restores");
    check(target.values == source.values && target.state == source.state,
          "parameters and state are restored");
    check(target.writes.size() == 3 && target.writes[0] == "p0" && target.writes[1] == "p1" &&
              target.writes[2] == "s:new",
          "all parameters are written before state");

    const auto beforeValues = target.values;
    const auto beforeState = target.state;
    auto invalid = snapshot;
    invalid.valid = false;
    target.writes.clear();
    check(!lat::restoreSpectraSound(target, invalid) && target.writes.empty(),
          "invalid snapshot causes no writes");
    invalid.valid = true;
    invalid.params.pop_back();
    check(!lat::restoreSpectraSound(target, invalid) && target.writes.empty(),
          "mismatched parameter count causes no writes");
    invalid = snapshot;
    invalid.params[0] = std::numeric_limits<float>::quiet_NaN();
    check(!lat::restoreSpectraSound(target, invalid) && target.writes.empty(),
          "nonfinite parameter causes no writes");
    invalid = snapshot;
    invalid.params[0] = 2.f;
    check(!lat::restoreSpectraSound(target, invalid) && target.writes.empty(),
          "out of range parameter causes no writes");
    check(target.values == beforeValues && target.state == beforeState,
          "preflight rejection leaves sound unchanged");

    FakeSound rejected;
    rejected.values = {0.1f, 0.2f};
    rejected.state = "before";
    rejected.acceptState = false;
    auto rejectedBefore = rejected.values;
    auto rejectedState = rejected.state;
    check(!lat::restoreSpectraSound(rejected, snapshot), "rejected state returns false");
    check(rejected.values == rejectedBefore && rejected.state == rejectedState,
          "rejected state rolls back all parameters and state");

    FakeSound emptyState;
    emptyState.state = "before";
    auto emptySnapshot = lat::captureSpectraSound(emptyState);
    emptySnapshot.state.clear();
    emptyState.writes.clear();
    check(lat::restoreSpectraSound(emptyState, emptySnapshot) && emptyState.writes.back() == "s:nxspc1",
          "empty state uses Spectra canonical reset");

    std::printf("Spectra compare: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
