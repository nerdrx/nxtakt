// Small, view-independent MIDI composition helpers.
#pragma once

#include "session.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace lat {

enum class ChordKind { Major, Minor, Diminished, Augmented, Seventh, Major7, Minor7, Sus2, Sus4 };

inline bool buildChord(int root, ChordKind kind, int inversion, std::vector<int>& out) {
    out.clear();
    static constexpr int major[] = {0, 4, 7};
    static constexpr int minor[] = {0, 3, 7};
    static constexpr int diminished[] = {0, 3, 6};
    static constexpr int augmented[] = {0, 4, 8};
    static constexpr int seventh[] = {0, 4, 7, 10};
    static constexpr int major7[] = {0, 4, 7, 11};
    static constexpr int minor7[] = {0, 3, 7, 10};
    static constexpr int sus2[] = {0, 2, 7};
    static constexpr int sus4[] = {0, 5, 7};
    const int* steps = major;
    int count = 3;
    switch (kind) {
    case ChordKind::Minor: steps = minor; break;
    case ChordKind::Diminished: steps = diminished; break;
    case ChordKind::Augmented: steps = augmented; break;
    case ChordKind::Seventh: steps = seventh; count = 4; break;
    case ChordKind::Major7: steps = major7; count = 4; break;
    case ChordKind::Minor7: steps = minor7; count = 4; break;
    case ChordKind::Sus2: steps = sus2; break;
    case ChordKind::Sus4: steps = sus4; break;
    default: break;
    }
    if (root < 0 || root > 127 || inversion < 0 || inversion >= count) return false;
    out.clear();
    for (int i = 0; i < count; ++i) {
        int pitch = root + steps[i] + (i < inversion ? 12 : 0);
        if (pitch < 0 || pitch > 127) { out.clear(); return false; }
        out.push_back(pitch);
    }
    std::sort(out.begin(), out.end());
    if (std::adjacent_find(out.begin(), out.end()) != out.end()) { out.clear(); return false; }
    return !out.empty();
}

enum class MelodyDirection { Up, Down, UpDown };

// Build one evenly timed, scale-aware line. Chromatic mode uses semitones;
// active scales walk only admitted pitches, preserving the chosen root octave.
inline int buildScalePattern(const ScaleKey& key, int rootPitch, int count,
                             f64 startBeat, f64 beatStep, f64 noteLen,
                             f64 clipLength, std::vector<NoteModel>& out,
                             MelodyDirection direction = MelodyDirection::Up) {
    out.clear();
    if (rootPitch < 0 || rootPitch > 127 || count <= 0 || count > 256 || !std::isfinite(startBeat) || !std::isfinite(beatStep) ||
        !std::isfinite(noteLen) || !std::isfinite(clipLength) || startBeat < 0.0 ||
        beatStep <= 0.0 || noteLen <= 0.0 || clipLength <= startBeat)
        return 0;
    if (direction != MelodyDirection::Up && direction != MelodyDirection::Down &&
        direction != MelodyDirection::UpDown) return 0;
    int pitch = rootPitch;
    const int initialStep = direction == MelodyDirection::Down ? -1 : 1;
    while (pitch >= 0 && pitch <= 127 && !key.contains(pitch)) pitch += initialStep;
    const int low = pitch, high = std::min(127, pitch + 12);
    int walk = initialStep;
    for (int i = 0; i < count; ++i) {
        const f64 beat = startBeat + beatStep * i;
        if (beat >= clipLength || pitch < 0 || pitch > 127) break;
        NoteModel note;
        note.beat = beat;
        note.len = std::min(noteLen, clipLength - beat);
        note.pitch = (u8)pitch;
        out.push_back(note);
        int next = pitch + walk;
        while (next >= 0 && next <= 127 && !key.contains(next)) next += walk;
        if (direction == MelodyDirection::UpDown && (next > high || next < low)) {
            walk = -walk;
            next = pitch + walk;
            while (next >= low && next <= high && !key.contains(next)) next += walk;
            if (next < low || next > high) break;
        }
        pitch = next;
    }
    return (int)out.size();
}

} // namespace lat
