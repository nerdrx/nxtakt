#include "../src/ui/drum_rhythm.h"

#include <cassert>
#include <array>
#include <cmath>
#include <limits>

using namespace lat;

static ClipModel clip(double length = 8.0) {
    ClipModel c;
    c.kind = ClipKind::Midi;
    c.lengthBeats = length;
    return c;
}

int main() {
    // Every density and rotation must keep its exact count and even spacing.
    for (int hits = 1; hits <= 16; ++hits) {
        for (int rotation = 0; rotation < 16; ++rotation) {
            ClipModel pattern = clip(12);
            assert(generateDrumRhythm(pattern, 42, 1, hits, rotation, 100));
            assert(pattern.notes.size() == static_cast<size_t>(hits));
            int minGap = 16, maxGap = 0;
            for (int i = 0; i < hits; ++i) {
                const double here = pattern.notes[i].beat;
                const double next = i + 1 < hits ? pattern.notes[i + 1].beat : pattern.notes[0].beat + 4;
                assert(here >= 4 && here < 8);
                const int gap = static_cast<int>(std::round((next - here) * 4));
                minGap = std::min(minGap, gap);
                maxGap = std::max(maxGap, gap);
            }
            assert(maxGap - minGap <= 1);
            assert(!generateDrumRhythm(pattern, 42, 1, hits, rotation, 100));
        }
    }
    ClipModel c = clip();
    assert(generateDrumRhythm(c, 36, 0, 5, 0, 100));
    assert(c.notes.size() == 5);
    assert(c.notes[0].beat == 0.0 && c.notes[1].beat == 1.0);
    const ClipModel once = c;
    assert(!generateDrumRhythm(c, 36, 0, 5, 0, 100));
    assert(c.notes.size() == once.notes.size());
    for (size_t i = 0; i < c.notes.size(); ++i) assert(sameDrumNote(c.notes[i], once.notes[i]));

    ClipModel rotated = clip();
    assert(generateDrumRhythm(rotated, 38, 0, 1, 15, 127));
    assert(rotated.notes.size() == 1 && rotated.notes[0].beat == 3.75);
    assert(rotated.notes[0].vel == 127 && rotated.notes[0].chance == 100 && rotated.notes[0].velTo == 0);

    ClipModel full = clip();
    assert(generateDrumRhythm(full, 39, 0, 16, 7, 1));
    assert(full.notes.size() == 16);
    ClipModel empty = full;
    assert(generateDrumRhythm(empty, 39, 0, 0, 0, 1));
    assert(empty.notes.empty());
    assert(!generateDrumRhythm(empty, 39, 0, 0, 0, 1));

    ClipModel keep = clip();
    keep.notes = {{0.0, .5, 36, 80, 11, 22}, {4.0, .5, 36, 81, 12, 23},
                  {0.0, .5, 60, 82, 13, 24}};
    assert(generateDrumRhythm(keep, 36, 0, 1, 0, 90));
    assert(keep.notes.size() == 3 && keep.notes[0].pitch == 60 && keep.notes[0].chance == 13);
    assert(keep.notes[1].pitch == 36 && keep.notes[1].vel == 90 && keep.notes[2].beat == 4.0);

    ClipModel partial = clip(4.1);
    assert(generateDrumRhythm(partial, 42, 1, 1, 0, 100));
    assert(partial.notes.size() == 1 && std::abs(partial.notes.back().len - .1) < 1e-12);

    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (const auto args : std::array<std::array<int, 5>, 8>{
             std::array<int, 5>{35, 0, 1, 0, 100}, {36, -1, 1, 0, 100},
             {36, 2, 1, 0, 100}, {36, 0, -1, 0, 100}, {36, 0, 17, 0, 100},
             {36, 0, 1, -1, 100}, {36, 0, 1, 0, 0}, {36, 0, 1, 0, 128}}) {
        ClipModel bad = clip();
        assert(!generateDrumRhythm(bad, args[0], args[1], args[2], args[3], args[4]));
    }
    ClipModel huge = clip();
    assert(!generateDrumRhythm(huge, 36, std::numeric_limits<int>::max(), 1, 0, 100));
    ClipModel nanClip = clip(nan);
    assert(!generateDrumRhythm(nanClip, 36, 0, 1, 0, 100));
    return 0;
}
