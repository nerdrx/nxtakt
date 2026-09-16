#include "../src/ui/compose.h"
#include <cassert>
#include <cmath>
#include <limits>

int main() {
    using namespace lat;
    std::vector<int> chord;
    assert(buildChord(60, ChordKind::Major7, 1, chord));
    assert((chord == std::vector<int>{64, 67, 71, 72}));
    assert(!buildChord(127, ChordKind::Major, 0, chord));
    assert(!buildChord(60, ChordKind::Major, 3, chord));

    ScaleKey key;
    key.root = 0; key.mode = 1;
    std::vector<NoteModel> notes;
    assert(buildScalePattern(key, 60, 8, 0.0, 0.5, 0.25, 4.0, notes) == 8);
    for (const NoteModel& n : notes) assert(key.contains(n.pitch));
    assert(notes.back().pitch == 72);
    assert(buildScalePattern(key, 60, 8, 3.5, 0.5, 1.0, 4.0, notes) == 1);
    assert(notes.front().beat == 3.5 && notes.front().len == 0.5);
    assert(buildScalePattern(key, 60, 8, -1.0, 0.5, 0.25, 4.0, notes) == 0 && notes.empty());
    assert(buildScalePattern(key, 60, 8, std::numeric_limits<double>::quiet_NaN(), 0.5, 0.25, 4.0, notes) == 0 && notes.empty());
    assert(buildScalePattern(key, 127, 8, 0.0, 0.5, 0.25, 4.0, notes) == 1);
    assert(notes.front().pitch == 127 && key.contains(notes.front().pitch));
    return 0;
}
