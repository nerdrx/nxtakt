#include "../src/ui/progression.h"
#include <cassert>
#include <cmath>
#include <limits>

int main() {
    using namespace lat;
    std::vector<NoteModel> notes;
    assert(buildProgression(60, false, 0, false, false, 0.0, 1.0, 1.0, 4.0, notes) == 12);
    assert(notes[0].pitch == 60 && notes[1].pitch == 64 && notes[2].pitch == 67);
    assert(notes[3].pitch == 67 && notes[4].pitch == 71 && notes[5].pitch == 74);
    assert(notes[6].pitch == 69 && notes[7].pitch == 72 && notes[8].pitch == 76);
    assert(notes[9].pitch == 65 && notes[10].pitch == 69 && notes[11].pitch == 72);
    assert(notes[6].beat == 2.0 && notes[6].len == 1.0);

    assert(buildProgression(60, true, 0, false, false, 0.0, 1.0, 1.0, 1.0, notes) == 3);
    assert(notes[0].pitch == 60 && notes[1].pitch == 63 && notes[2].pitch == 67);
    assert(buildProgression(60, false, 0, true, false, 0.0, 1.0, .5, 1.0, notes) == 4);
    assert(notes[0].pitch == 60 && notes[1].pitch == 64 && notes[2].pitch == 67 && notes[3].pitch == 71);
    assert(notes[0].len == .5);

    std::vector<NoteModel> root, smooth;
    assert(buildProgression(60, false, 0, false, false, 0.0, 1.0, 1.0, 3.0, root) == 9);
    assert(buildProgression(60, false, 0, false, true, 0.0, 1.0, 1.0, 3.0, smooth) == 9);
    int rootMotion = 0, smoothMotion = 0;
    for (int i = 3; i < 9; ++i) {
        rootMotion += std::abs((int)root[i].pitch - (int)root[i - 3].pitch);
        smoothMotion += std::abs((int)smooth[i].pitch - (int)smooth[i - 3].pitch);
    }
    assert(smoothMotion <= rootMotion);

    assert(buildProgression(60, false, 0, false, false, 3.5, 1.0, 1.0, 4.0, notes) == 3);
    assert(notes[0].beat == 3.5 && notes[0].len == .5);
    assert(buildProgression(60, false, 0, false, false, 0.0, .000001, 1.0, 1000.0, notes) == 12);
    assert(buildProgression(60, false, 0, false, false, -1.0, 1.0, 1.0, 4.0, notes) == 0 && notes.empty());
    assert(buildProgression(60, false, 0, false, false, 0.0, 1.0, 0.0, 4.0, notes) == 0 && notes.empty());
    assert(buildProgression(60, false, 0, false, false, 0.0, 1.0, 1.01, 4.0, notes) == 0 && notes.empty());
    assert(buildProgression(60, false, 0, false, false, 0.0, 1.0, 1.0, 1000.0, notes) == 12);
    assert(buildProgression(127, false, 0, false, false, 0.0, 1.0, 1.0, 4.0, notes) == 0 && notes.empty());
    assert(buildProgression(120, false, 0, false, false, 0.0, 1.0, 1.0, 4.0, notes) == 0 && notes.empty());
    assert(buildProgression(60, false, 4, false, false, 0.0, 1.0, 1.0, 4.0, notes) == 0 && notes.empty());
    assert(buildProgression(60, false, 0, false, false, 0.0, 1.0,
                            std::numeric_limits<double>::quiet_NaN(), 4.0, notes) == 0);
    assert(buildProgression(60, false, 0, false, false, 0.0, 1.0,
                            std::numeric_limits<double>::infinity(), 4.0, notes) == 0);
    assert(buildProgression(60, false, 0, false, false, 0.0, 1.0,
                            1.0, std::numeric_limits<double>::infinity(), notes) == 0);
    return 0;
}
