#include "../src/ui/hit_map.h"
#include "../src/ui/click_sequence.h"
#include <cstdio>
#include <cstdlib>
using namespace lat;
static int checks = 0;
static void expect(bool value, const char* label) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
}
int main() {
    ClickSequence clicks;
    expect(!clicks.press(0, 1000, 10, 10, 1), "first click is single");
    expect(!clicks.press(0, 1100, 50, 10, 1), "rapid clicks on different controls stay single");
    expect(clicks.press(0, 1200, 52, 12, 1), "nearby second click is double");
    expect(!clicks.press(0, 1300, 52, 12, 1), "third click starts a fresh pair");
    expect(!clicks.press(2, 1400, 52, 12, 1), "different buttons are not a double click");
    expect(!clicks.press(2, 2000, 52, 12, 1), "late click starts a fresh pair");
    expect(clicks.press(2, 2100, 60, 12, 2), "position tolerance follows display scale");
    HitMap map;
    expect(map.at(10, 10) == 0, "empty layout has no target");
    map.add(1, {0, 0, 100, 100});
    map.add(2, {20, 20, 24, 24});
    expect(map.at(5, 5) == 1, "container remains clickable");
    expect(map.at(21, 21) == 2, "quick pointer move resolves new control without a hover frame");
    expect(map.at(5, 5) == 1, "moving out immediately resolves container");
    map.add(3, {30, 20, 24, 24});
    expect(map.at(33, 25) == 3, "last painted overlapping control owns pointer");
    expect(map.at(24, 25) == 2, "uncovered neighbour retains ownership");
    map.add(4, Rect{70, 70, 4, 4}.inset(-3).intersect({0, 0, 72, 100}));
    expect(map.at(68, 71) == 4, "grab padding is clickable");
    expect(map.at(72, 71) == 1, "padding never escapes panel clip");
    map.add(5, {10, 10, 0, 20});
    expect(map.at(10, 10) == 1, "empty clipped control is inert");
    expect(!map.accepts(5, 10, 10), "newly clipped control rejects stale hover");
    expect(!map.accepts(2, 5, 5), "current geometry rejects stale target");
    expect(map.accepts(2, 21, 21), "current geometry accepts its own face");
    expect(map.accepts(99, 5, 5), "tooltip query can precede current geometry");
    map.clear();
    expect(map.at(21, 21) == 0, "removed controls cannot retain hover");
    map.add(2, {50, 50, 24, 24});
    expect(map.at(21, 21) == 0, "old layout location is forgotten");
    expect(map.at(50, 50) == 2, "new control location resolves");
    expect(map.at(74, 50) == 0, "adjacent boundary belongs only to next control");
    std::printf("hit map: %d checks passed\n", checks);
}
