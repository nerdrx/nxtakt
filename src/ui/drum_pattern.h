#pragma once

#include "drum_rhythm.h"

#include <algorithm>
#include <cmath>

namespace lat {

struct DrumPage {
    std::vector<NoteModel> notes;
    bool valid = false;
};

inline constexpr int kDrumPatternPitches[] = {36, 38, 39, 42, 46, 45, 37, 56};

inline bool isDrumPatternPitch(int pitch) {
    for (const int candidate : kDrumPatternPitches)
        if (candidate == pitch) return true;
    return false;
}

inline bool validDrumPage(const ClipModel& clip, int page, double& start,
                          double& end) {
    if (clip.kind != ClipKind::Midi || page < 0 ||
        !std::isfinite(clip.lengthBeats) || clip.lengthBeats <= 0.0)
        return false;
    start = static_cast<double>(page) * 4.0;
    if (!std::isfinite(start) || start >= clip.lengthBeats) return false;
    end = std::min(start + 4.0, clip.lengthBeats);
    return end > start;
}

inline DrumPage copyDrumPage(const ClipModel& clip, int page) {
    DrumPage result;
    double start = 0.0, end = 0.0;
    if (!validDrumPage(clip, page, start, end)) return result;
    result.valid = true;
    for (const NoteModel& note : clip.notes) {
        if (!isDrumPatternPitch(note.pitch) || !std::isfinite(note.beat) ||
            note.beat < start || note.beat >= end)
            continue;
        NoteModel copy = note;
        copy.beat -= start;
        result.notes.push_back(copy);
    }
    return result;
}

inline bool pasteDrumPage(ClipModel& clip, int page, const DrumPage& source) {
    double start = 0.0, end = 0.0;
    if (!source.valid || !validDrumPage(clip, page, start, end)) return false;

    std::vector<NoteModel> candidate;
    candidate.reserve(clip.notes.size() + source.notes.size());
    for (const NoteModel& note : clip.notes) {
        if (isDrumPatternPitch(note.pitch) && std::isfinite(note.beat) &&
            note.beat >= start && note.beat < end)
            continue;
        candidate.push_back(note);
    }
    for (const NoteModel& note : source.notes) {
        if (!isDrumPatternPitch(note.pitch) || !std::isfinite(note.beat) ||
            !std::isfinite(note.len) || note.beat < 0.0 || note.beat >= end - start ||
            note.len <= 0.0)
            continue;
        NoteModel copy = note;
        copy.beat += start;
        copy.len = std::clamp(copy.len, 0.0, end - copy.beat);
        candidate.push_back(copy);
    }
    std::stable_sort(candidate.begin(), candidate.end(),
                     [](const NoteModel& a, const NoteModel& b) { return a.beat < b.beat; });
    if (candidate.size() == clip.notes.size() &&
        std::equal(candidate.begin(), candidate.end(), clip.notes.begin(), sameDrumNote))
        return false;
    clip.notes.swap(candidate);
    return true;
}

inline bool doubleDrumPattern(ClipModel& clip) {
    const double oldLength = clip.lengthBeats;
    if (clip.kind != ClipKind::Midi || !std::isfinite(oldLength) || oldLength <= 0.0 ||
        oldLength * 2.0 > 16.0)
        return false;
    std::vector<NoteModel> additions;
    additions.reserve(clip.notes.size());
    for (const NoteModel& note : clip.notes) {
        if (!std::isfinite(note.beat) || !std::isfinite(note.len) ||
            note.beat < 0.0 || note.beat >= oldLength || note.len <= 0.0)
            continue;
        NoteModel copy = note;
        copy.beat += oldLength;
        copy.len = std::min(copy.len, oldLength - note.beat);
        additions.push_back(copy);
    }
    clip.lengthBeats = oldLength * 2.0;
    clip.notes.insert(clip.notes.end(), additions.begin(), additions.end());
    std::stable_sort(clip.notes.begin(), clip.notes.end(),
                     [](const NoteModel& a, const NoteModel& b) { return a.beat < b.beat; });
    return true;
}

} // namespace lat
