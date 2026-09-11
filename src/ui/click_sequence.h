#pragma once
#include "../core/common.h"
#include <cmath>

namespace lat {
// A double-click is two nearby presses, not merely two clicks anywhere in
// the application. Distances are scaled with the same DPI as the UI.
class ClickSequence {
public:
    bool press(int button, f64 milliseconds, f32 x, f32 y, f32 scale) {
        const f32 radius = 5.f * std::max(1.f, scale);
        const bool twice = button == button_ && milliseconds >= time_ &&
            milliseconds - time_ < 400.0 &&
            std::abs(x - x_) <= radius && std::abs(y - y_) <= radius;
        button_ = twice ? -1 : button;
        time_ = milliseconds; x_ = x; y_ = y;
        return twice;
    }
private:
    int button_ = -1;
    f64 time_ = 0;
    f32 x_ = 0, y_ = 0;
};
} // namespace lat
