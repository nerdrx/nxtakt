#pragma once

#include "session.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

namespace lat {

namespace note_feel_detail {

inline bool validNote(const ClipModel& clip, const NoteModel& note) {
    return std::isfinite(note.beat) && std::isfinite(note.len) && note.len > 0.0 &&
           note.beat >= 0.0 && note.beat <= clip.lengthBeats &&
           note.len <= clip.lengthBeats - note.beat;
}

inline std::vector<int> targets(const ClipModel& clip, const std::vector<int>& in) {
    std::vector<int> out;
    for (int i : in) {
        if (i < 0 || (size_t)i >= clip.notes.size()) continue;
        out.push_back(i);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

inline bool validClip(const ClipModel& clip) {
    return std::isfinite(clip.lengthBeats) && clip.lengthBeats > 0.0;
}

} // namespace note_feel_detail

inline bool humanizeNotes(ClipModel& clip, const std::vector<int>& targetIndices,
                          double timingBeats, int velocityAmount, uint64_t seed) {
    if (!note_feel_detail::validClip(clip) || !std::isfinite(timingBeats) ||
        timingBeats < 0.0 || timingBeats > 0.25 || velocityAmount < 0 || velocityAmount > 64)
        return false;
    if (timingBeats == 0.0 && velocityAmount == 0) return false;
    const auto indices = note_feel_detail::targets(clip, targetIndices);
    for (int i : indices)
        if (!note_feel_detail::validNote(clip, clip.notes[(size_t)i])) return false;
    if (indices.empty()) return false;

    std::mt19937_64 rng(seed);
    const int64_t amount = (int64_t)std::llround(timingBeats * 1000000.0);
    std::uniform_int_distribution<int64_t> timing(-amount, amount);
    std::uniform_int_distribution<int> velocity(-velocityAmount, velocityAmount);
    bool changed = false;
    for (int i : indices) {
        NoteModel& note = clip.notes[(size_t)i];
        const double oldBeat = note.beat;
        const int oldVelocity = note.vel;
        const double delta = (double)timing(rng) / 1000000.0;
        const double latest = std::max(0.0, clip.lengthBeats - note.len);
        note.beat = std::clamp(note.beat + delta, 0.0, latest);
        note.vel = (u8)std::clamp(oldVelocity + velocity(rng), 1, 127);
        changed |= note.beat != oldBeat || note.vel != oldVelocity;
    }
    return changed;
}

inline bool strumNotes(ClipModel& clip, const std::vector<int>& targetIndices,
                       double spreadBeats, bool descending) {
    if (!note_feel_detail::validClip(clip) || !std::isfinite(spreadBeats) ||
        spreadBeats < 0.0 || spreadBeats > 1.0)
        return false;
    if (spreadBeats == 0.0) return false;
    const auto indices = note_feel_detail::targets(clip, targetIndices);
    for (int i : indices)
        if (!note_feel_detail::validNote(clip, clip.notes[(size_t)i])) return false;
    if (indices.empty()) return false;

    std::vector<int> byBeat = indices;
    std::stable_sort(byBeat.begin(), byBeat.end(), [&](int a, int b) {
        return clip.notes[(size_t)a].beat < clip.notes[(size_t)b].beat;
    });
    std::vector<std::vector<int>> groups;
    for (int i : byBeat) {
        if (groups.empty() || std::abs(clip.notes[(size_t)groups.back().front()].beat -
                                     clip.notes[(size_t)i].beat) > 1e-9)
            groups.push_back({i});
        else
            groups.back().push_back(i);
    }

    bool changed = false;
    for (auto& group : groups) {
        if (group.size() < 2) continue;
        std::sort(group.begin(), group.end(), [&](int a, int b) {
            const int pa = clip.notes[(size_t)a].pitch, pb = clip.notes[(size_t)b].pitch;
            return descending ? pa > pb : pa < pb;
        });
        const double originalBeat = clip.notes[(size_t)group.front()].beat;
        double allowed = spreadBeats;
        const double count = (double)(group.size() - 1);
        for (size_t k = 1; k < group.size(); ++k) {
            const NoteModel& note = clip.notes[(size_t)group[k]];
            const double room = std::max(0.0, clip.lengthBeats - note.len - originalBeat);
            allowed = std::min(allowed, room * count / (double)k);
        }
        for (size_t k = 0; k < group.size(); ++k) {
            NoteModel& note = clip.notes[(size_t)group[k]];
            const double beat = originalBeat + allowed * (double)k / count;
            changed |= note.beat != beat;
            note.beat = beat;
        }
    }
    return changed;
}

} // namespace lat
