#include "../src/ui/drum_pattern.h"

#include <cassert>
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
    ClipModel c = clip(6.0);
    c.notes = {{0.5, 0.25, 36, 80, 17, 22}, {4.5, 2.0, 42, 81, 18, 23},
               {1.0, 1.0, 60, 82, 19, 24}, {6.0, .25, 38, 83, 20, 25}};
    const DrumPage page = copyDrumPage(c, 1);
    assert(page.valid && page.notes.size() == 1 && page.notes[0].beat == .5);
    assert(page.notes[0].vel == 81 && page.notes[0].chance == 18 && page.notes[0].velTo == 23);
    assert(!copyDrumPage(c, -1).valid && !copyDrumPage(c, 2).valid);

    ClipModel target = clip(6.0);
    target.notes = {{4.0, .25, 36, 1, 2, 3}, {5.5, .25, 42, 4, 5, 6},
                    {4.0, .5, 60, 90, 7, 8}, {0.0, .25, 38, 9, 10, 11}};
    DrumPage replacement;
    replacement.valid = true;
    replacement.notes = {{0.25, 2.0, 36, 100, 61, 62}, {1.0, .25, 46, 70, 63, 64}};
    assert(pasteDrumPage(target, 1, replacement));
    assert(target.notes.size() == 4);
    assert(target.notes[2].pitch == 36 && target.notes[2].beat == 4.25 && target.notes[2].len == 1.75);
    assert(target.notes[3].pitch == 46 && target.notes[3].chance == 63);
    const ClipModel once = target;
    assert(!pasteDrumPage(target, 1, replacement));
    assert(target.notes.size() == once.notes.size());
    assert(pasteDrumPage(target, 1, DrumPage{ {}, true }));
    assert(target.notes.size() == 2); // non-drum and other-page notes survive

    ClipModel doubled = clip(4.0);
    doubled.notes = {{0.5, .25, 36, 90, 31, 32}, {3.75, .25, 60, 91, 33, 34},
                     {3.9, .5, 38, 92, 35, 36}};
    assert(doubleDrumPattern(doubled) && doubled.lengthBeats == 8.0 && doubled.notes.size() == 6);
    assert(doubled.notes[3].beat == 4.5 && doubled.notes[3].vel == 90 && doubled.notes[3].chance == 31);
    assert(std::abs(doubled.notes[5].beat - 7.9) < 1e-12 &&
           std::abs(doubled.notes[5].len - .1) < 1e-12);
    assert(doubleDrumPattern(doubled) && doubled.lengthBeats == 16.0);
    assert(!doubleDrumPattern(doubled));
    ClipModel tooLong = clip(8.1);
    assert(!doubleDrumPattern(tooLong));
    ClipModel nan = clip(std::numeric_limits<double>::quiet_NaN());
    assert(!doubleDrumPattern(nan));
    return 0;
}
