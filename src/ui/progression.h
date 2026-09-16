// Small, bounded diatonic chord-progression helper.
#pragma once

#include "session.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace lat {

namespace progression_detail {

inline bool validNoteSet(const std::vector<int>& pitches) {
    return !pitches.empty() && std::all_of(pitches.begin(), pitches.end(),
        [](int p) { return p >= 0 && p <= 127; }) &&
        std::adjacent_find(pitches.begin(), pitches.end()) == pitches.end();
}

inline bool chooseSmooth(const std::array<int, 4>& base, int count,
                         const std::vector<int>& previous,
                         std::vector<int>& chosen) {
    bool found = false;
    long bestCost = 0;
    std::array<int, 4> shifts{};
    std::array<int, 4> best{};
    const auto visit = [&](auto&& self, int i) -> void {
        if (i == count) {
            std::vector<int> candidate;
            candidate.reserve((std::size_t)count);
            for (int n = 0; n < count; ++n) candidate.push_back(base[n] + shifts[n] * 12);
            std::sort(candidate.begin(), candidate.end());
            if (!validNoteSet(candidate)) return;
            long cost = 0;
            for (int n = 0; n < count; ++n) cost += std::abs(candidate[n] - previous[n]);
            bool better = !found || cost < bestCost;
            if (!better && cost == bestCost) {
                for (int n = 0; n < count; ++n) {
                    if (candidate[n] == best[n]) continue;
                    better = candidate[n] < best[n];
                    break;
                }
            }
            if (better) {
                found = true;
                bestCost = cost;
                for (int n = 0; n < count; ++n) best[n] = candidate[n];
            }
            return;
        }
        // A five-octave search is bounded and covers every useful nearby inversion.
        for (int shift = -2; shift <= 2; ++shift) {
            shifts[i] = shift;
            self(self, i + 1);
        }
    };
    visit(visit, 0);
    if (!found) return false;
    chosen.assign(best.begin(), best.begin() + count);
    return true;
}

} // namespace progression_detail

inline int buildProgression(int rootPitch, bool minor, int progression, bool seventh,
                            bool smooth, double startBeat, double chordBeats,
                            double gate, double clipLength,
                            std::vector<NoteModel>& out) {
    out.clear();
    if (rootPitch < 0 || rootPitch > 127 || progression < 0 || progression >= 4 ||
        !std::isfinite(startBeat) || !std::isfinite(chordBeats) ||
        !std::isfinite(gate) || !std::isfinite(clipLength) || startBeat < 0.0 ||
        chordBeats <= 0.0 || gate <= 0.0 || gate > 1.0 || clipLength <= startBeat)
        return 0;

    static constexpr int majorScale[] = {0, 2, 4, 5, 7, 9, 11};
    static constexpr int minorScale[] = {0, 2, 3, 5, 7, 8, 10};
    static constexpr int presets[][4] = {{0, 4, 5, 3}, {0, 5, 3, 4},
                                         {1, 4, 0, 0}, {0, 3, 4, 0}};
    const int* scale = minor ? minorScale : majorScale;
    const int degreeCount = seventh ? 4 : 3;
    std::vector<int> previous;

    for (int chordIndex = 0; chordIndex < 4; ++chordIndex) {
        const double beat = startBeat + chordBeats * chordIndex;
        if (!std::isfinite(beat) || beat >= clipLength) break;
        const int degree = presets[progression][chordIndex % 4];
        std::array<int, 4> base{};
        for (int n = 0; n < degreeCount; ++n) {
            const int scaleDegree = degree + n * 2;
            base[n] = rootPitch + scale[scaleDegree % 7] + 12 * (scaleDegree / 7);
        }

        std::vector<int> pitches;
        if (chordIndex == 0 || !smooth) {
            pitches.assign(base.begin(), base.begin() + degreeCount);
            if (!progression_detail::validNoteSet(pitches)) { out.clear(); return 0; }
        } else if (!progression_detail::chooseSmooth(base, degreeCount, previous, pitches)) {
            out.clear();
            return 0;
        }

        const double length = std::min(chordBeats * gate, clipLength - beat);
        if (!std::isfinite(length) || length <= 0.0) break;
        for (int pitch : pitches) {
            NoteModel note;
            note.beat = beat;
            note.len = length;
            note.pitch = static_cast<u8>(pitch);
            out.push_back(note);
        }
        previous = pitches;
    }
    return static_cast<int>(out.size());
}

} // namespace lat
