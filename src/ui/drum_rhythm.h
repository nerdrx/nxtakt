#pragma once

#include "session.h"

#include <algorithm>
#include <cmath>

namespace lat {

inline bool sameDrumNote(const NoteModel& a, const NoteModel& b) {
    return a.beat == b.beat && a.len == b.len && a.pitch == b.pitch &&
           a.vel == b.vel && a.chance == b.chance && a.velTo == b.velTo;
}

inline bool generateDrumRhythm(ClipModel& clip, int pitch, int page, int hits,
                               int rotation, int velocity) {
    constexpr int pitches[] = {36, 38, 39, 42, 46, 45, 37, 56};
    bool supported = false;
    for (const int candidate : pitches) supported |= pitch == candidate;
    if (!supported || page < 0 || hits < 0 || hits > 16 || rotation < 0 ||
        rotation > 15 || velocity < 1 || velocity > 127 ||
        !std::isfinite(clip.lengthBeats) || clip.lengthBeats <= 0.0)
        return false;

    const double pageStart = static_cast<double>(page) * 4.0;
    if (!std::isfinite(pageStart) || pageStart >= clip.lengthBeats) return false;
    const double pageEnd = std::min(pageStart + 4.0, clip.lengthBeats);

    std::vector<NoteModel> candidate;
    candidate.reserve(clip.notes.size() + 16);
    for (const NoteModel& note : clip.notes) {
        if (note.pitch == pitch && note.beat >= pageStart && note.beat < pageEnd)
            continue;
        candidate.push_back(note);
    }
    if (hits > 0) {
        for (int step = 0; step < 16; ++step) {
            if ((((step - rotation + 16) % 16) * hits) % 16 >= hits) continue;
            const double beat = pageStart + step * 0.25;
            if (beat >= pageEnd) continue;
            NoteModel note;
            note.beat = beat;
            note.len = std::min(0.25, clip.lengthBeats - beat);
            note.pitch = static_cast<u8>(pitch);
            note.vel = static_cast<u8>(velocity);
            note.chance = 100;
            note.velTo = 0;
            candidate.push_back(note);
        }
    }
    std::stable_sort(candidate.begin(), candidate.end(),
                     [](const NoteModel& a, const NoteModel& b) { return a.beat < b.beat; });
    if (candidate.size() == clip.notes.size() &&
        std::equal(candidate.begin(), candidate.end(), clip.notes.begin(), sameDrumNote))
        return false;
    clip.notes.swap(candidate);
    return true;
}

} // namespace lat
