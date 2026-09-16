#include "../src/ui/note_feel.h"

#include <cassert>
#include <cmath>
#include <limits>

using namespace lat;

static ClipModel clip() {
    ClipModel c;
    c.kind = ClipKind::Midi;
    c.lengthBeats = 4.0;
    return c;
}

int main() {
    ClipModel c = clip();
    c.notes = {{1.0, .5, 60, 100, 37, 77}, {2.0, .25, 64, 110, 52, 0}, {3.0, .5, 67, 90, 10, 120}};
    const ClipModel before = c;
    assert(!humanizeNotes(c, {0, 1}, 0.0, 0, 1));
    assert(c.notes[0].chance == before.notes[0].chance && c.notes[0].velTo == before.notes[0].velTo);
    ClipModel neutral = clip();
    neutral.notes = {{1.0, .5, 60, 0}, {1.0 + 5e-10, .5, 64, 0}};
    const ClipModel neutralBefore = neutral;
    assert(!humanizeNotes(neutral, {0, 1, 1}, 0.0, 0, 1));
    assert(!strumNotes(neutral, {0, 1, 0}, 0.0, false));
    assert(neutral.notes[0].beat == neutralBefore.notes[0].beat && neutral.notes[0].vel == 0);
    assert(neutral.notes[1].beat == neutralBefore.notes[1].beat && neutral.notes[1].vel == 0);
    assert(humanizeNotes(c, {0}, .25, 20, 42));
    const double beat = c.notes[0].beat;
    const int vel = c.notes[0].vel;
    assert(beat >= 0.0 && beat <= 1.5 && vel >= 1 && vel <= 127);
    assert(c.notes[1].beat == before.notes[1].beat && c.notes[2].beat == before.notes[2].beat);
    ClipModel d = before;
    humanizeNotes(d, {0}, .25, 20, 42);
    assert(d.notes[0].beat == beat && d.notes[0].vel == vel);

    ClipModel limits = clip();
    limits.notes = {{0, .5, 60, 1, 31, 19}, {3.5, .5, 64, 127, 32, 18}};
    assert(humanizeNotes(limits, {0, 1}, .25, 64, 99));
    assert(limits.notes[0].beat >= 0.0 && limits.notes[0].beat <= .5 && limits.notes[0].vel >= 1);
    assert(limits.notes[1].beat >= 0.0 && limits.notes[1].beat <= 3.5 && limits.notes[1].vel <= 127);
    assert(limits.notes[0].chance == 31 && limits.notes[0].velTo == 19);
    assert(limits.notes[1].chance == 32 && limits.notes[1].velTo == 18);

    ClipModel chord = clip();
    chord.notes = {{1, 1, 60, 80}, {1, 1, 64, 80}, {1, 1, 67, 80}};
    assert(strumNotes(chord, {0, 1, 2}, 1.0, false));
    assert(chord.notes[0].beat == 1 && chord.notes[1].beat == 1.5 && chord.notes[2].beat == 2);
    ClipModel subset = clip();
    subset.notes = {{1, 1, 60, 80}, {1, 1, 64, 80}, {1, 1, 67, 80}, {1, 1, 72, 80}};
    assert(strumNotes(subset, {0, 2}, .5, false));
    assert(subset.notes[1].beat == 1 && subset.notes[3].beat == 1);
    ClipModel boundary = clip();
    boundary.notes = {{2.5, 1.0, 60, 80}, {2.5, .5, 64, 80}, {2.5, 1.0, 67, 80}};
    assert(strumNotes(boundary, {0, 1, 2}, 1.0, false));
    assert(boundary.notes[0].beat == 2.5 && boundary.notes[1].beat == 2.75 && boundary.notes[2].beat == 3.0);
    for (const auto& n : boundary.notes) assert(n.beat + n.len <= boundary.lengthBeats);
    ClipModel duplicate = clip();
    duplicate.notes = {{1, 1, 60, 80}, {1, 1, 64, 80}, {1, 1, 67, 80}};
    ClipModel unique = duplicate;
    assert(strumNotes(duplicate, {2, 0, 2, 1, 0}, .5, false));
    assert(strumNotes(unique, {0, 1, 2}, .5, false));
    for (size_t i = 0; i < duplicate.notes.size(); ++i) assert(duplicate.notes[i].beat == unique.notes[i].beat);
    ClipModel down = clip();
    down.notes = {{1, 1, 60, 80}, {1, 1, 64, 80}, {1, 1, 67, 80}};
    assert(strumNotes(down, {0, 1, 2}, .5, true));
    assert(down.notes[2].beat == 1 && down.notes[1].beat == 1.25 && down.notes[0].beat == 1.5);
    assert(!strumNotes(chord, {1}, .5, false));
    const ClipModel invalidBefore = chord;
    assert(!strumNotes(chord, {0, 99, 0}, std::numeric_limits<double>::quiet_NaN(), false));
    assert(chord.notes[0].beat == invalidBefore.notes[0].beat);
    assert(!humanizeNotes(chord, {0}, std::numeric_limits<double>::infinity(), 1, 1));
    assert(!humanizeNotes(chord, {0}, .1, 65, 1));

    ClipModel bad = clip();
    bad.notes = {{3.8, .5, 60, 100}, {1, .5, 64, 100}};
    const ClipModel badBefore = bad;
    assert(!humanizeNotes(bad, {0, 1}, .1, 10, 2));
    assert(bad.notes[0].beat == badBefore.notes[0].beat);
    assert(!strumNotes(bad, {0, 1}, .1, false));
    return 0;
}
