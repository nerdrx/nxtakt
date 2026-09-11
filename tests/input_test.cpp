#include "../src/ui/window.h"
#include <cstdio>
#include <cstdlib>
using namespace lat;
static int checks = 0;
static void expect(bool value, const char* label) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
}
int main() {
    Input in;
    in.pressKey(KeyCtrl, ModCtrl);
    in.mods = ModCtrl;
    in.pressKey('s', ModCtrl);
    in.keyDown['s'] = in.keyDown[KeyCtrl] = false;
    in.mods = 0;
    expect(in.keyPressed['s'] && in.ctrl(), "save survives an entire chord between frames");
    expect(in.keyStarted['s'] && !in.keyDown['s'], "short press keeps its first-down edge");
    in.newFrame();
    expect(!in.ctrl() && !in.keyPressed['s'] && !in.keyStarted['s'], "released chord clears next frame");
    in.pressKey(KeyCtrl, ModCtrl);
    in.keyDown[KeyCtrl] = false;
    in.pressKey('s', 0);
    expect(!in.ctrl(), "modifier tap alone does not convert a later key to a shortcut");
    in.newFrame();
    in.pressKey('z', ModCtrl | ModShift);
    in.keyDown['z'] = false;
    expect(in.keyStarted['z'] && in.ctrl() && in.shift(), "quick redo preserves both modifiers");
    in.newFrame();
    in.mods = ModCtrl;
    in.pressKey('z', ModCtrl);
    expect(in.keyStarted['z'], "initial undo has a first-down edge");
    in.newFrame();
    in.pressKey('z', ModCtrl);
    expect(in.keyPressed['z'] && !in.keyStarted['z'], "auto-repeat is not another initial press");
    in.newFrame();
    expect(in.ctrl() && in.keyDown['z'], "frame reset retains genuinely held keys and modifiers");
    in.keyDown['z'] = false;
    in.mods = 0;
    in.pressKey(KeyAlt, ModAlt);
    in.pressKey('x', ModAlt);
    in.keyDown['x'] = in.keyDown[KeyAlt] = false;
    expect(in.alt(), "short alt chord survives release");
    in.newFrame();
    expect(!in.alt(), "short alt chord does not stick");
    in.pressKey(-1, ModCtrl);
    in.pressKey(KeyCount, ModCtrl);
    expect(!in.ctrl(), "invalid keys cannot latch modifiers");
    std::printf("input: %d checks passed\n", checks);
}
