// THE KEYMAP, ONCE.
//
// This program's only written documentation used to be `nxtakt --help`, which
// is a thing a GUI user never runs. The obvious fix -- a keys panel in the
// app -- was also the obvious way to end up with two lists that disagree
// within a wave, so there is one list and both surfaces read it: main.cpp's
// usage() prints it, and App::drawKeysSheet (app_chrome.cpp) draws it under F1.
//
// A binding that is not in here is a binding nobody can find. That is the
// point of the file, and it is the standing rule for anyone adding one: the
// key goes in this table in the same commit it goes in the code.
//
// Deliberately free of every other header in the program. It is data.
#pragma once

namespace lat {
namespace keys {

struct Row {
    // A section heading opens a group; `keys` is null on a heading row, and
    // `what` carries the title. A row whose `keys` is null and whose `what` is
    // null is a blank line -- the printed form wants them, the drawn form
    // spends them as spacing.
    const char* keys;
    const char* what;
};

// The drawn sheet is two columns of these; the printed form is one. Keep each
// `what` under about 70 characters or the panel has to ellipsise it.
inline constexpr Row table[] = {
    {nullptr, "TRANSPORT"},
    {"Space",         "play / stop  (works while the computer keyboard is on)"},
    {"Home",          "playhead back to the start  -  Stop does NOT rewind"},
    {"Esc",           "stop all clips  (with a note selected: deselect it first)"},
    {"Enter",         "launch the selected clip  (Session view)"},
    {"M",             "metronome"},
    {nullptr, nullptr},

    {nullptr, "VIEWS AND PANELS"},
    {"Tab",           "Session / Arrangement"},
    {"Ctrl+B",        "browser"},
    {"Ctrl+D",        "clip detail panel"},
    {"F1",            "this list"},
    {nullptr, nullptr},

    {nullptr, "THE SET"},
    {"Ctrl+S",        "save"},
    {"Ctrl+T",        "add track"},
    {"Ctrl+Enter",    "add scene"},
    {"Ctrl+Z",        "undo   (edits only: the transport and a take in flight"},
    {"",              "        are outside it -- an undo while recording cancels"},
    {"",              "        the take)"},
    {"Ctrl+Shift+Z",  "redo   (Ctrl+Y does the same)"},
    {"",              "open a set: double-click a .lattice file in the browser,"},
    {"",              "twice to confirm -- it replaces what you have open"},
    {nullptr, nullptr},

    {nullptr, "ANY KNOB, FADER OR NUMBER"},
    {"drag",          "vertical: up is more.  A trough drags horizontally."},
    {"Ctrl+drag",     "fine  (Shift does the same)"},
    {"wheel",         "adjust the control under the pointer, whatever is beneath"},
    {"Ctrl+wheel",    "fine"},
    {"double-click",  "reset to the default  (it says so when there is not one)"},
    {"right-click",   "Reset / Type in value, plus whatever that control offers"},
    {nullptr, nullptr},

    {nullptr, "ARRANGEMENT"},
    // The owner's question, answered first and in the panel: "how would I add
    // a note block in the arrange section? except for audio i wouldn't know
    // how to add anything there." The gesture already worked; nothing said so.
    {"double-click",  "an empty lane writes a one-bar note block there"},
    {"drag",          "across an empty lane paints a run of them"},
    {"right-drag",    "across a lane erases what you sweep over"},
    {"Ctrl+drag",     "a clip leaves a copy behind  (Shift does the same)"},
    {"Ctrl+E",        "split the selected clip at the cursor"},
    {"Ctrl+U",        "duplicate the selected clip  (inside notes: the loop)"},
    {"Del",           "delete the selected clip, or the selected note"},
    {"Alt",           "held, a drag ignores the grid"},
    {",  /  .",       "jump to the previous / next marker from the playhead"},
    {"",              "markers live on the ruler's UPPER band: double-click to"},
    {"",              "drop one, click a flag to jump, drag to move, double-click"},
    {"",              "a flag to rename, right-click to delete, right-drag along"},
    {"",              "the band to clear several.  The LOWER band locates, drags"},
    {"",              "the loop brace, and right-click adds a time-signature"},
    {"",              "change at that bar."},
    {nullptr, nullptr},

    {nullptr, "PIANO ROLL  (CLIP tab, MIDI clips)"},
    {"click",         "add / select a note.  Double-click adds or deletes."},
    {"drag",          "move it; the right edge sizes it.  Right-click deletes."},
    {"wheel",         "scroll pitch    Shift+wheel scrolls time"},
    {"Ctrl+wheel",    "zoom time about the cursor"},
    {"arrows",        "nudge the selected note (grid step / semitone)"},
    {"Shift+Up/Down", "nudge by an octave"},
    {"Ctrl+U",        "double the loop and duplicate its notes"},
    {"",              "ALL / FOLD / KEY choose what the pitch axis shows;"},
    {"",              "VEL / CHANCE / RANGE choose what the bottom lane edits."},
    {nullptr, nullptr},

    {nullptr, "RECORDING"},
    {"",              "The round button arms the INTENT, like Live's session"},
    {"",              "record. With it lit: click an empty slot on an armed"},
    {"",              "track to start a take, click again to stop. Click a MIDI"},
    {"",              "clip to overdub another pass into it."},
    {"",              "ARR sends the take to the timeline instead of the grid;"},
    {"",              "AUTO records control moves into the playing clip."},
    {nullptr, nullptr},

    {nullptr, "COMPUTER MIDI KEYBOARD"},
    {"Ctrl+Shift+K",  "on / off.  While it is on the letter keys are NOTES and"},
    {"",              "not shortcuts -- that is what the lit KBD chip means."},
    {"",              "FL layout, by key POSITION, on any keyboard layout:"},
    {"",              "   Z X C V B N M  lower octave, white     S D _ G H J  black"},
    {"",              "   Q W E R T Y U + I O P  the two above    2 3 _ 5 6 7 _ 9 0  black"},
    {"PgUp / PgDn",   "octave.  Velocity is the number beside the KBD chip."},
};

inline constexpr int count = (int)(sizeof(table) / sizeof(table[0]));

} // namespace keys
} // namespace lat
