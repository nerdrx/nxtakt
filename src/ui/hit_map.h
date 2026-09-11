// Resolve the pointer against the last complete layout, in paint order.
// This keeps overlapping controls topmost without making a quick click wait
// for a hover frame. Entries use the effective (padded AND clipped) hit box.
#pragma once
#include "../gfx/renderer.h"
#include <vector>

namespace lat {
class HitMap {
public:
    void add(u64 id, const Rect& box) {
        entries_.push_back({id, box});
    }
    u64 at(f32 x, f32 y) const {
        for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
            if (it->box.contains(x, y)) return it->id;
        return 0;
    }
    bool accepts(u64 id, f32 x, f32 y) const {
        for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
            if (it->id == id) return it->box.contains(x, y);
        // Some callers set their tooltip before drawing the control. Its
        // last-layout hit is valid until current geometry has been submitted.
        return true;
    }
    void clear() { entries_.clear(); }
private:
    struct Entry { u64 id; Rect box; };
    std::vector<Entry> entries_;
};
} // namespace lat
