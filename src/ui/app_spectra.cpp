// Spectra: the editor for the wavetable instrument.
//
// THE CONTRACT, and nothing but the contract. Every id, range, log flag, table
// name and sync division below is read off docs/SPECTRA-PARAMS.md; this file
// has never seen src/plugin/spectra.cpp and must never need to. That is not
// purity for its own sake -- the two halves were written at the same time by
// two people, and the frozen table is the only thing that could keep them
// agreeing without one waiting for the other. The v2 blocks (ids 42..99, "the
// parity push") were added under the same discipline: this file was written
// against the contract while the DSP was being written against the same
// contract somewhere else, and neither side has seen the other.
//
// ONE EXCEPTION, and it is a narrow one: the hero display reads the real
// wavetables through detail::spectraTables() (declared in
// src/plugin/internal_base.h, never in spectra.cpp). That is a const view of
// shared immutable memory and four integers of geometry -- no parameter id, no
// range, no name, nothing the frozen table has an opinion about. The panel
// still cannot see how a table is generated and still does not want to; what it
// gained is the ability to draw the frames instead of a drawing of them. The
// display falls back to its old illustration when the accessor returns null,
// which is every process that has never instantiated a Spectra.
//
// ---------------------------------------------------------------------------
// THE JUDGMENT, for this file (docs/DESIGN.md §4)
//
// A plugin editor is chrome, so it takes the tier language in full. The panel
// is ONE card -- the lit-violet edge and an elevation, exactly like the rack
// panel, because it is the inside of the device box beside it. Everything
// *inside* it recesses: the wavetable display is a well, every trough is a
// well, and there is no second frosted layer anywhere (§4: glass inside glass
// reads as fog). Sections are separated by gradient hairlines that fade at both
// ends; there is not a solid rule in here.
//
// ONE exception arrived with v2, and it is the one §4 licenses: the preset
// list is a POPOVER -- a floating surface that overlaps other content -- and
// that is precisely the tier-2 sheet ("modals, slide-overs, menus, toasts").
// It carries its legibility in fill alpha, the lit edge, and the sheet shadow,
// and it is the only tier-2 surface in the panel.
//
// Light comes from the upper left in every one of it: the knob caps are
// --glass-1 circles with the 1px --edge stroke, the troughs are lit along the
// same 147deg axis, and the one specular in the panel is bound to the POSITION
// VALUE rather than to a clock (§1, "light rides motion"). Nothing in this
// panel animates on a timer except the page tab's sliding indicator, which is
// §5's own idiom and collapses under reduced motion inside tabPill itself.
//
// ---------------------------------------------------------------------------
// WHY TWO PAGES, AND NOT A WIDER PANEL OR A SCROLL
//
// The dock is 200 logical pixels tall and the strip reserves the panel's width
// from lay::spectraPanelW BEFORE this file draws anything, so the v2 surface
// has exactly the footprint the v1 surface had: seven columns by two knob
// rows. The contract now names 89 functional parameters; they do not fit on
// one face of that size, and the design doc rules out the alternatives one by
// one: a scroll inside the panel hides controls behind a gesture (§5 wants
// working surfaces flat and fast), and a wider panel just moves the scroll
// into the strip. What §5 does offer is the tab indicator -- ONE sliding
// element between equal slots -- so the panel has two faces:
//
//   MAIN  the v1 face, verbatim: hero, oscillators, filter, ENV1/2, LFO1,
//         global. Plus the two strictly-compatible widenings the contract
//         allows (Filter Type 0..5, Glide 0..2000) and the preset popover.
//   MOD   everything the parity push added: sub & noise, warp, LFO2/3,
//         ENV3, the macros, the eight-slot matrix, voice mode.
//
// The MOD page is cut on the SAME column grid (the colX[] the v1 page uses),
// so the seams stand still when the page turns -- the panel reads as one
// instrument showing its other face, not as two different panels.
//
// ---------------------------------------------------------------------------
// ...AND WHY v4 MAKES IT THREE, WHICH IS AN ARGUMENT AND NOT A HABIT
//
// v4 adds fourteen parameters and a sixteen-step, two-row drawable grid. Both
// existing pages are FULL -- MAIN has seven columns with no spare cell and MOD
// has seven with two -- and the dock's 200 logical pixels are not negotiable
// from here. So there are exactly three ways to land an arpeggiator, and two
// of them are refusals:
//
//   * PUT IT ON MOD. It does not fit, and making it fit means taking something
//     off. The only candidates are the four macros or LFO 3, and both are
//     reachable today: hiding a control that was reachable is the one cost
//     this panel has never paid, in three revisions. Refused.
//   * SCROLL, OR GROW THE DOCK. §5 wants working surfaces flat and fast, and
//     the dock height is the constraint the whole layout is cut to. Refused,
//     for the reason the two-page note above already gives.
//   * A THIRD PAGE. §5's sliding indicator does not care whether there are two
//     slots or three, and tabPill now hit-tests the UN-inset slot (widgets.cpp
//     1.7), so the band can carry a third tab and still clear the 16px floor.
//
// WHAT THE THIRD PAGE COSTS, said out loud rather than left to be discovered:
// each tab slot narrows, and the arp is one page turn away from the notes it
// plays. The first is paid for in pixels -- the band widens from 88 to 108
// logical px, so a slot is 36 wide and 16 tall rather than 44 and 16, and the
// amber note beside it gives up twenty pixels it had to spare. The second is
// the honest cost and it is small: nothing on MAIN or MOD is read WHILE
// editing an arp pattern, which is exactly the test that made warp and the
// matrix share a page and would have failed here.
//
// WHY THE ARP PAGE IS CUT DIFFERENTLY. MAIN and MOD are seven sections in
// seven columns. The arp is fourteen parameters -- which fill exactly two of
// those columns, band and both rows, with no cell left over -- and ONE control
// that is the whole feature. So columns 0 and 1 are the instrument's settings
// and columns 2..6 are the pattern, on the same colX[] grid as the other two
// pages, with the seams inside the spanned block left undrawn (a hairline
// through the middle of a sixteen-step grid is noise, not structure).
//
// ---------------------------------------------------------------------------
// ...AND WHY v5 MAKES IT FOUR, WHICH IS A DIFFERENT ARGUMENT AGAIN
//
// v5 is a wavetable EDITOR: two pens over 2048 samples, 32 frames, a morph and
// a commit. It spends no parameter ids at all, so nothing about it is a knob
// looking for a cell. What it needs is the one thing this panel has never had
// to find: a CANVAS -- a surface whose vertical resolution is the feature,
// because a pen with sixty pixels of travel draws a staircase and calls it a
// curve.
//
// FOUR SHAPES WERE AVAILABLE AND THREE ARE REFUSED:
//
//   * A MODAL OVERLAY over the panel. Refused twice over. §4 licenses a
//     tier-2 sheet for menus and slide-overs, and this panel already spends
//     its one on the preset popover -- but a drawing canvas is the FLATTEST
//     working surface in the program (§5), and putting it on frosted glass
//     over a card that is already glass is the "glass inside glass reads as
//     fog" rule broken to buy nothing. And it buys nothing literally: an
//     overlay over a 200px panel is 200px tall. The scarce axis is height and
//     a sheet does not make more of it.
//   * AN EXPANDED DETAIL-PANEL TAKEOVER -- the editor eating the whole detail
//     strip, full window width. It has the most pixels and it is the one shape
//     this panel cannot take: it would hide the device strip, the chain, every
//     other device's card AND all three of this panel's own pages, which is a
//     hundred reachable controls hidden at once by a surface that four
//     revisions have refused to hide ONE for. It also lives in app_devices.cpp
//     and app.cpp, which this wave does not own, and a shape that needs
//     another agent's file is a shape that is not available.
//   * A SCROLL INSIDE A PAGE. Refused for the third revision running, for the
//     reason the two-page note above gives.
//   * A FOURTH PAGE. Taken. §5's sliding indicator does not care whether there
//     are three slots or four, and tabPill hit-tests the UN-inset slot
//     (widgets.cpp 1.7), so the band carries a fourth tab over the 16px floor.
//     The band widens 108 -> 132 so a slot is 33 logical px rather than 27 --
//     the twenty-four pixels come out of the amber note, which has 872 and
//     needs about 300, exactly as v4 took twenty for the third.
//
// AND THE PART THAT IS NEW, WHICH IS WHY THIS ARGUMENT IS NOT v4'S AGAIN.
// Every page above this one is cut to a dock 200 logical pixels tall, and the
// three notes in this file that call that height "not negotiable from here"
// were true when they were written and are FALSE NOW: the detail panel grew a
// SPLITTER last release (app.cpp, UiDetailSplit), and the dock runs 120 to
// window-height-minus-180. So the DRAW page does the one thing no other page
// in this panel can do -- it SPENDS height, all of it, and gets better when
// the user drags the splitter down. The canvas takes the whole body and every
// pixel the dock gains; the footer prints the number ("canvas 110 px - 27 per
// unit") so the resolution is a fact rather than a feeling; and a TALLER chip
// drives the splitter from here, once, reversibly, saying what it did. At the
// 200px default the page is completely usable and slightly cramped, which is
// the honest trade and is stated on screen rather than in a release note.
//
// WHY THE DRAW PAGE IS CUT DIFFERENTLY, and it is the arp's cut with the
// proportions swapped. Columns 0 and 1 are the editor's controls -- six rows
// of sixteen, which is exactly what a 130px body holds -- and columns 2..6 are
// the frame strip and the canvas, one 802px-wide surface that is the feature.
// Only the seam at column 2 is drawn: the other five would be hairlines
// through the middle of a drawing.
//
// ---------------------------------------------------------------------------
// THE GUARD, and why every single lookup has one
//
// The panel draws against whatever PluginInstance it is pointed at. Normally
// that is a Spectra; under NXTAKT_DEBUG_SPECTRA it is deliberately whatever
// device happens to be first in the chain, and during the days before the DSP
// landed it was ALWAYS something else. The v2 push adds the third case, and it
// is the everyday one for a while: a REAL Spectra that still has only the 42
// v1 parameters, against which every id from 42 up is absent. So a parameter
// is a *slot* that may or may not be filled: `has(id)` is `paramCount() > id`,
// an unfilled slot draws as an empty socket at 40% with a dash for a value, and
// it claims no hot rectangle at all -- so the pointer passes straight through
// it and the cursor never changes over a control that does not exist. The
// title bar says WHY in amber (§9: say what happened): "DSP has N of 100
// parameters". A panel that crashed, or drew a knob wired to nothing, would be
// the worse of the two failures by a distance: the second one lies.
//
// The two widenings get the same treatment from the other side: the filter
// cluster asks paramInfo(20) how many types the device actually has and draws
// three segments or six, and the glide knob takes its ceiling from
// paramInfo(38). Against the v1 DSP both look exactly as they always did;
// against the v2 DSP they come alive with no edit here.
//
#include "app.h"
#include "app_internal.h"
#include "../gfx/gl.h"
#include "../plugin/internal_base.h"    // detail::spectraTables() -- see above
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace lat {

namespace {

// ---------------------------------------------------------------------------
// The contract, transcribed. Ids are indices and indices persist in saved sets,
// so this list may be APPENDED to and never reordered.
// ---------------------------------------------------------------------------
enum SpecParam : int {
    pATable = 0, pAPos,  pACoarse, pAFine, pALevel, pAUnison, pADetune, pASpread,
    pBTable = 8, pBPos,  pBCoarse, pBFine, pBLevel, pBUnison, pBDetune, pBSpread,
    pNoise  = 16, pSub,
    pCutoff = 18, pReso, pFType, pDrive, pE2Cutoff, pKeytrack,
    pAttack = 24, pDecay, pSustain, pRelease,
    pE2Attack = 28, pE2Decay, pE2Sustain, pE2Release,
    pLfoRate = 32, pLfoSync, pLfoPos, pLfoCut, pLfoPitch, pLfoShape,
    pGlide  = 38, pVoices, pMaster, pE2Pos,
    pCount
};
static_assert(pCount == 42, "the v1 block of docs/SPECTRA-PARAMS.md is frozen at 42");

// The v2 blocks ("v2 -- the parity push"), same discipline. Reserved ids
// (46/47, 52/53, 60/61, 66/67, 92/93, 99) are registered by the DSP but hidden
// here, exactly as the contract instructs, so they get no enum name at all --
// a name would be an invitation to draw them.
enum SpecParamV2 : int {
    pSubShape = 42, pSubOct = 43, pNoiseColor = 44, pNoiseTrack = 45,
    pAWarp = 48, pAWarpAmt = 49, pBWarp = 50, pBWarpAmt = 51,
    pL2Rate = 54, pL2Sync = 55, pL2Shape = 56,
    pL3Rate = 57, pL3Sync = 58, pL3Shape = 59,
    pE3Attack = 62, pE3Decay = 63, pE3Sustain = 64, pE3Release = 65,
    pM1Src = 68, pM1Dst = 69, pM1Amt = 70,        // slot k is 68+3k / 69+3k / 70+3k
    pMacro1 = 94,
    pVoiceMode = 98,
    pCountV2 = 100
};
static_assert(pM1Src + 3 * 7 + 2 == 91, "M8 Amt is id 91 per the contract");
static_assert(pMacro1 + 3 == 97 && pVoiceMode == 98, "v2 tail ids off the contract");

// The v3 append ("v3 -- hands on the modulation"). Three of v2's reserved ids
// are spent (60, 61, 99) and ids 100..108 are new; 109/110 are the fresh
// reserved tail and get no name here, for the reason the v2 block gives.
enum SpecParamV3 : int {
    pL2Mode = 60, pL3Mode = 61,
    pBendRange = 99,
    pL1Mode = 100,
    pM1Curve = 101,                              // slot k is 101 + k
    pCountV3 = 111
};
static_assert(pM1Curve + 7 == 108, "M8 Curve is id 108 per the contract");

// The v4 append ("v4 -- the arpeggiator"). v3's two reserved ids are spent on
// the switch and the mode, 111..122 are new, and 123/124 are the fresh reserved
// tail -- unnamed, for the reason the v2 block gives.
//
// 109 AND 110 ARE THE v2 TRAP, ONE REVISION ON, AND IT IS WORSE HERE. A v3 DSP
// registers both -- 0..1, default 0, hidden -- exactly as the reserved rule
// instructs, so `has(109)` answers "present" about a parameter that means
// nothing, and Arp On's real range is 0..1 with a default of 0 as well: the two
// cannot be told apart by has(), by range, or by default. The honest test is
// the one the title bar already makes -- does this DSP have the v4 block AT ALL
// -- so it is made once, into `arpLive` (pc >= pCountV4), and EVERY control on
// the arp page passes through it, including the ones whose ids are new and
// which has() would have answered correctly. A guard that is right for twelve
// ids and wrong for two is a guard nobody can check.
enum SpecParamV4 : int {
    pArpOn = 109, pArpMode = 110, pArpRate = 111, pArpSync = 112,
    pArpOctaves = 113, pArpOctMode = 114, pArpGate = 115, pArpSwing = 116,
    pArpHold = 117, pArpRetrig = 118, pArpVelMode = 119, pArpFixedVel = 120,
    pArpSteps = 121, pArpChance = 122,
    pCountV4 = 125
};
static_assert(pArpChance + 2 == 124, "the arp's reserved tail is 123 and 124");
static_assert(pCountV4 == 125, "kSpParamCount goes 111 -> 125 in v4");

// The eight tables, by index. UI labels, per the contract's own wording.
const char* const kTables[8] = {"Basic", "PWM", "Harmonic", "Formant",
                                "Bell",  "Digital", "Vox",  "Fold"};
// LFO Sync, 0..9. 0 is free-run and the rate knob owns the readout then; every
// other entry is a division and the knob shows it instead of a frequency.
// Shared verbatim by LFO2 and LFO3 -- the contract says "division table of
// id 33, verbatim", and one array is how two things stay verbatim.
const char* const kSyncDiv[10] = {"free", "4 bars", "2 bars", "1 bar", "1/2",
                                  "1/4",  "1/8",    "1/16",   "1/4T",  "1/8T"};
// SIX shapes since v3: id 37/56/59 widen 0..4 -> 0..5, and 5 is the drawn
// 16-step grid. The cluster below always draws six segments and lets the
// DEVICE say how many of them are real -- the filter cluster's rule, applied
// to the other enum the contract widened.
const char* const kShapeName[6] = {"sine", "tri", "saw", "square", "S&H", "custom"};

// v2 enums, straight off the contract's tables.
const char* const kSubShape[3]  = {"sine", "tri", "sqr"};
const char* const kWarpMode[8]  = {"Off",      "Sync", "Bend+", "Bend-",
                                   "Mirror",   "Quantize", "FM", "RM"};
const char* const kVoiceMode[3] = {"poly", "mono", "leg"};
// Matrix sources 0..13 and destinations 0..19, in the contract's order. The
// spellings are display labels cut to what a 50px selector can carry; the
// tooltip says the long form.
// v3 appends three MIDI sources (14/15/16) under the append-only rule, so this
// list GREW at the end and every old value kept its number. How many of them a
// device actually has is paramInfo(68).max, asked at the call site.
// v4 appends ONE more (17, Arp Step) under the same rule, so this list grew at
// the end a second time and every old value kept its number again.
const char* const kMatrixSrc[18] = {
    "Off",     "LFO 1",   "LFO 2",   "LFO 3",   "ENV 2",   "ENV 3",  "Velo",
    "KeyTrk",  "AfterT",  "Macro 1", "Macro 2", "Macro 3", "Macro 4", "Random",
    "Wheel",   "Bend",    "MIDI CC", "ArpStep"};
constexpr int kSrcCount = 18;
// The matrix source Arp Step IS. Named once so the grab handle, the header
// label and the inert sentence cannot drift apart.
constexpr int kSrcArpStep = 17;
const char* const kMatrixDst[20] = {
    "Off",     "A Pos",   "B Pos",   "A Warp",  "B Warp",  "A Level", "B Level",
    "A Pitch", "B Pitch", "Sub",     "Noise",   "Cutoff",  "Reso",    "Drive",
    "A Det",   "B Det",   "Pan",     "L1 Rate", "L2 Rate", "L3 Rate"};
constexpr int kDstCount = 20;
// The three per-slot response curves (ids 101..108). The contract's own words
// for the shapes; the tooltip carries the formulas.
const char* const kCurveName[3] = {"lin", "exp", "S"};
// LFO mode (ids 60/61/100). One-shot makes an LFO an envelope.
const char* const kLfoMode[2] = {"loop", "1shot"};

// v4's three enums, straight off the contract's tables. Arp Mode (id 110) is
// TEN values and its ordering is frozen append-only forever; the spellings are
// cut to what a 112px stepper carries and the tooltip says the long form.
const char* const kArpMode[10] = {"Up",        "Down",   "Up-Dn Inc", "Up-Dn Exc",
                                  "Down-Up",   "As Played", "Random", "Chord",
                                  "Thumb",     "Pinky"};
const char* const kArpOctMode[3] = {"up", "down", "alt"};
const char* const kArpVelMode[3] = {"played", "fixed", "patt"};

// ---------------------------------------------------------------------------
// THE DESTINATION MAP -- which knob in this panel a matrix destination lands
// on. It is the whole of what drag-assign and the mod rings need, and it is
// read off the contract's destination list and nothing else.
//
// TWO ENTRIES ARE -1 AND BOTH ARE HONEST. Destination 0 is Off, and
// destination 16 is Pan, which this instrument has no parameter for at all
// (the contract makes it a matrix-only destination: "base is centre"). A drop
// on a knob is refused when the knob is not in this table, and Pan simply
// cannot be reached by dragging -- it is reached by picking it in a slot's
// destination selector, which is where every destination is always reachable.
//
// A PITCH DESTINATION LANDS ON THE COARSE KNOB, and that is a judgment worth
// stating: destination 7 is "A Pitch [unit: +/-24 st, added after
// coarse/fine/glide]", so it is not the Coarse parameter -- but Coarse is the
// only control in the panel that MEANS "osc A's pitch in semitones", and a
// mod ring on it reads exactly right. The scale row below is what keeps the
// ring honest about it.
const int kDestParam[kDstCount] = {
    -1,         pAPos,      pBPos,      pAWarpAmt,  pBWarpAmt,
    pALevel,    pBLevel,    pACoarse,   pBCoarse,   pSub,
    pNoise,     pCutoff,    pReso,      pDrive,     pADetune,
    pBDetune,   -1,         pLfoRate,   pL2Rate,    pL3Rate};
// Amount -> the fraction of the CONTROL's own travel one unit of Amt covers.
// The contract's normalized destinations are 1.0 by construction ("+/-1 = full
// 0..1", and Cutoff/LFO-rate say "norm, log domain of id N", which is exactly
// the knob's own log travel). The unit-domain destinations are the ones with a
// number here: Pitch reaches +/-24 st on a knob that spans 48, Drive reaches
// +/-24 dB on a knob that spans 24, Detune +/-100 ct on a knob that spans 100.
const f32 kDestSpan[kDstCount] = {
    0.f,  1.f,  1.f,  1.f,  1.f,
    1.f,  1.f,  0.5f, 0.5f, 1.f,
    1.f,  1.f,  1.f,  1.f,  1.f,
    1.f,  0.f,  1.f,  1.f,  1.f};

// A source's value domain, for the mod ring. The contract states it per source
// and there is exactly one shape-dependent case: LFO 1..3 are bipolar for
// shapes 0..4 and UNIPOLAR for shape 5 (Custom), because sixteen levels cannot
// be symmetric about an exact zero. That case is resolved at the call site,
// which is the only place that can see the shape parameters.
bool srcBipolarStatic(int src) {
    return src == 7 || src == 13 || src == 15;   // KeyTrk, Random, Pitch Bend
}
// Matrix source 1/2/3 is LFO 1/2/3, and its polarity is its SHAPE's.
const int kLfoShapeId[3] = {pLfoShape, pL2Shape, pL3Shape};
const int kLfoRateId[3]  = {pLfoRate,  pL2Rate,  pL3Rate};
const int kLfoSyncId[3]  = {pLfoSync,  pL2Sync,  pL3Sync};

// THE KNOB'S OWN GEOMETRY IS THE KNOB'S.
//
// The mod ring is drawn AROUND knobNx's arc, and this file used to mirror the
// three numbers that places it -- the two sweep angles and the cap radius --
// because they are file-locals in src/ui/widgets.cpp. The mirror is gone:
// Ui::knobRing() takes the same rect and the same style the knob was given and
// is concentric with it by construction, so moving the sweep moves both.
//
// ---------------------------------------------------------------------------
// PRESET CATEGORIES, derived from nothing but the name.
//
// The contract's factory-bank rule fixes every preset name as "<TAG> <Name>":
// a two-letter category tag, one space, a Title Case name -- and fixes the
// seven tags. So the category IS the name's first two characters, and the
// popover needs no second channel of metadata the PluginInstance boundary
// would have to grow: a name whose chars [0..1] match a known tag and whose
// char [2] is the mandated space files under that tag's category; anything
// else ("Init", which the contract exempts, and the whole v1 demo list, which
// predates the rule) files under no header at the top of the list. Rows keep
// the bank's own order inside a category because the contract orders the file
// by category and alphabetically within one -- the popover just draws a header
// wherever the tag changes.
// ---------------------------------------------------------------------------
struct PresetCat { const char* tag; const char* label; };
const PresetCat kPresetCat[7] = {{"BA", "BASS"},  {"LD", "LEAD"},  {"PD", "PAD"},
                                 {"KY", "KEYS"},  {"PL", "PLUCK"}, {"FX", "FX"},
                                 {"SQ", "SEQUENCE"}};
int presetCatOf(const char* n) {
    if (!n || !n[0] || !n[1] || n[2] != ' ') return -1;
    for (int i = 0; i < 7; ++i)
        if (n[0] == kPresetCat[i].tag[0] && n[1] == kPresetCat[i].tag[1]) return i;
    return -1;
}

constexpr f32 kTwoPi = 6.28318530717958647692f;

// NXTAKT_DEBUG_PROBE, read once -- app_devices.cpp's and app_chrome.cpp's
// spelling. It is here because of a gap v4 made impossible to ignore: every
// knob in this panel announces its writes through the widget layer's
// probeValue(), and the FOUR drawable rows announce nothing at all, so a driven
// grid stroke could only ever be asserted from a tooltip. drive.sh's header is
// explicit that a screenshot proves what was DRAWN and the probe log proves
// what was MEANT; the state string is what these rows mean.
bool probeOn() {
    static const bool on = env("DEBUG_PROBE") != nullptr;
    return on;
}

// A wavetable arrives as a WAV and nothing else -- the plan's three
// interpretation rules all start from one. The test is on the NAME because
// that is all a drag in flight has; the importer is what actually reads the
// bytes, and its refusal is what the amber line carries when the name lied.
bool looksLikeWav(const std::string& p) {
    if (p.size() < 5) return false;
    const char* t = p.c_str() + p.size() - 4;
    return t[0] == '.' && (t[1] | 32) == 'w' && (t[2] | 32) == 'a' && (t[3] | 32) == 'v';
}
const char* baseNameOf(const char* p) {
    if (!p || !*p) return "";
    const char* slash = strrchr(p, '/');
    return slash ? slash + 1 : p;
}

// PRESETS. host.h's contract says presetName() may return null out of range,
// and "out of range" includes every index on every device that has no presets
// -- which is all of them but this one. So the null is handled here, once, and
// nothing below has to remember that a name can be absent.
const char* presetNameOf(const PluginInstance& p, int i) {
    const char* n = p.presetName(i);
    return n ? n : "?";
}

// ---------------------------------------------------------------------------
// THE GUARD, RAISED ONE LEVEL: FROM THE PARAMETER TO THE CONTRACT ITSELF
//
// Every guard above this line answers "does the device this panel is pointed at
// have parameter N". v3 asks a question that is one storey higher, because two
// of its three pillars are not parameters at all: they are METHODS on
// PluginInstance that a sibling wave is adding to src/plugin/host.h --
// `wavetable()`, `savePreset()` and `factoryPresetCount()` -- and this file is
// not allowed to add them and must build with or without them.
//
// `has(id)` cannot express that. A method that does not exist is not a runtime
// absence, it is a compile error, and "compiles today, lights up when the
// header lands, with no edit here" is the exact property that makes four
// parallel agents safe. So the same discipline is applied with the only tool
// that can: a C++20 `requires` test per method, and an `if constexpr` that
// DISCARDS the call in the build where the method is absent.
//
// The result is that every call site below reads like an ordinary null check --
// `wtSupported(inst)` is false, `wtImport(...)` returns false, `psFactoryCount()`
// answers presetCount() -- and every one of those is the same answer the
// runtime null would give. A panel built against today's host.h draws the
// import affordance and the save chip INERT AND EXPLAINED; the same source,
// rebuilt after the sibling's header lands, draws them live. Nothing in
// between, and nothing to remember.
//
// The proposed header addition is filed verbatim as
// ed3-filed-src-plugin-host.h.diff; these shims are what stands in for its
// absence, and they are correct to leave in place after it arrives.
// ---------------------------------------------------------------------------
template <class P>
concept HasWavetable = requires(P* p) { p->wavetable(); };
template <class P>
concept HasSavePreset = requires(P* p) { p->savePreset("x"); };
template <class P>
concept HasFactoryCount = requires(const P* p) { p->factoryPresetCount(); };

// Non-null only when the contract HAS the accessor and the device answers one.
template <class P> bool wtSupported(P* p) {
    if constexpr (HasWavetable<P>) return p && p->wavetable() != nullptr;
    else { (void)p; return false; }
}
template <class P> bool wtImport(P* p, int osc, const char* path) {
    if constexpr (HasWavetable<P>) {
        auto* w = p ? p->wavetable() : nullptr;
        return w && w->importFile(osc, path);
    } else { (void)p; (void)osc; (void)path; return false; }
}
template <class P> bool wtHasCustom(P* p, int osc) {
    if constexpr (HasWavetable<P>) {
        auto* w = p ? p->wavetable() : nullptr;
        return w && w->hasCustom(osc);
    } else { (void)p; (void)osc; return false; }
}
template <class P> const char* wtName(P* p, int osc) {
    if constexpr (HasWavetable<P>) {
        auto* w = p ? p->wavetable() : nullptr;
        const char* n = w ? w->customName(osc) : nullptr;
        return n ? n : "";
    } else { (void)p; (void)osc; return ""; }
}
template <class P> int wtFrames(P* p, int osc) {
    if constexpr (HasWavetable<P>) {
        auto* w = p ? p->wavetable() : nullptr;
        return w ? w->customFrames(osc) : 0;
    } else { (void)p; (void)osc; return 0; }
}
template <class P> void wtClear(P* p, int osc) {
    if constexpr (HasWavetable<P>) {
        auto* w = p ? p->wavetable() : nullptr;
        if (w) w->clearCustom(osc);
    } else { (void)p; (void)osc; }
}
template <class P> const char* wtError(P* p) {
    if constexpr (HasWavetable<P>) {
        auto* w = p ? p->wavetable() : nullptr;
        const char* e = w ? w->lastError() : nullptr;
        return (e && *e) ? e : "the import was refused and said nothing";
    } else { (void)p; return ""; }
}

// ---------------------------------------------------------------------------
// v5's FIVE, AND THE SAME TRICK PLAYED ONE STOREY DEEPER
//
// v3 asked "does this build's PluginInstance have wavetable()". v5 asks a
// harder question: does the WavetableControl that answers have `readFrames`,
// `previewFrames`, `commitFrames`, `cancelPreview` and `setCustomName` -- five
// methods a sibling wave is appending to src/plugin/host.h, and five that this
// file may not add.
//
// `HasWavetable` CANNOT STAND IN FOR THEM. A build can have wavetable() and
// not have the v5 methods, and that build is the one this file compiles in
// today -- so a shim that tested the accessor and then called readFrames()
// would be a compile error in exactly the configuration the acceptance test
// runs. Each of the five therefore gets its own requires-expression written
// THROUGH wavetable(), which short-circuits for free: a build with no
// wavetable() at all fails every one of them without a second test.
//
// THE FIVE ARE ONE FEATURE AND THEY ARE TESTED AS ONE. `kWtEditor<P>` is the
// conjunction, every shim below is gated on the conjunction rather than on its
// own concept, and every control on the editor page passes through it. That is
// `arpLive`'s rule in a new place: a guard that is right for four of five is a
// guard nobody can check, and there is no honest editor with four of these --
// one that can draw and preview but not commit is a toy that eats an hour.
//
// THERE ARE THREE STATES ON SCREEN AND NOT TWO, and the page says which:
//   contract absent                   inert, and the sentence names the five
//   contract present, wavetable() null  inert, and the sentence names the
//                                     accessor -- v3's own refusal, verbatim
//   contract present, device answers  LIVE. A device whose own host.h defaults
//                                     refuse (all five ship with one) draws
//                                     live and REPORTS the refusal, because
//                                     there is no capability query in this
//                                     contract and a refusal that arrives as a
//                                     return value is said out loud, not
//                                     guessed at in advance.
// ---------------------------------------------------------------------------
template <class P>
concept HasWtRead    = requires(P* p, f32* o)        { p->wavetable()->readFrames(0, o); };
template <class P>
concept HasWtPreview = requires(P* p, const f32* fr) { p->wavetable()->previewFrames(0, fr); };
template <class P>
concept HasWtCommit  = requires(P* p, const f32* fr) { p->wavetable()->commitFrames(0, fr, "n"); };
template <class P>
concept HasWtCancel  = requires(P* p)                { p->wavetable()->cancelPreview(0); };
template <class P>
concept HasWtSetName = requires(P* p)                { p->wavetable()->setCustomName(0, "n"); };
template <class P>
inline constexpr bool kWtEditor = HasWtRead<P> && HasWtPreview<P> && HasWtCommit<P> &&
                                  HasWtCancel<P> && HasWtSetName<P>;

template <class P> bool wtReadFrames(P* p, int osc, f32* out) {
    if constexpr (kWtEditor<P>) {
        auto* w = p ? p->wavetable() : nullptr;
        return w && w->readFrames(osc, out);
    } else { (void)p; (void)osc; (void)out; return false; }
}
template <class P> bool wtPreviewFrames(P* p, int osc, const f32* fr) {
    if constexpr (kWtEditor<P>) {
        auto* w = p ? p->wavetable() : nullptr;
        return w && w->previewFrames(osc, fr);
    } else { (void)p; (void)osc; (void)fr; return false; }
}
template <class P> bool wtCommitFrames(P* p, int osc, const f32* fr, const char* name) {
    if constexpr (kWtEditor<P>) {
        auto* w = p ? p->wavetable() : nullptr;
        return w && w->commitFrames(osc, fr, name);
    } else { (void)p; (void)osc; (void)fr; (void)name; return false; }
}
template <class P> void wtCancelPreview(P* p, int osc) {
    if constexpr (kWtEditor<P>) {
        auto* w = p ? p->wavetable() : nullptr;
        if (w) w->cancelPreview(osc);
    } else { (void)p; (void)osc; }
}
template <class P> bool wtSetCustomName(P* p, int osc, const char* name) {
    if constexpr (kWtEditor<P>) {
        auto* w = p ? p->wavetable() : nullptr;
        return w && w->setCustomName(osc, name);
    } else { (void)p; (void)osc; (void)name; return false; }
}

// The user-preset half. `psSupported` is a CONTRACT question and not a device
// one -- savePreset()'s own default returns false, so a device that does not
// save presets answers the refusal itself and the chip stays live to hear it.
template <class P> constexpr bool psSupported() { return HasSavePreset<P>; }
template <class P> bool psSave(P* p, const char* name) {
    if constexpr (HasSavePreset<P>) return p && p->savePreset(name);
    else { (void)p; (void)name; return false; }
}
// The boundary between the factory bank and the user bank. The contract's own
// default answers presetCount(), so a build without the method -- and a device
// without a user bank -- both say "every preset is a factory preset", which is
// what draws no User header at all.
template <class P> int psFactoryCount(const P* p) {
    if constexpr (HasFactoryCount<P>) return p ? p->factoryPresetCount() : 0;
    else return p ? p->presetCount() : 0;
}

// ---------------------------------------------------------------------------
// THE STATE STRING, this side of it.
//
// v3 gives Spectra `nxspc1;<record>;<record>;...` and puts TWO of this file's
// controls in it rather than in parameters: the three drawn LFO grids and their
// smooths. So this panel is a WRITER of a device state string, which nothing in
// the editor has ever been before, and the contract's rules are transcribed
// here the same way its parameter table is:
//
//   * a record is `key=value`, `key` matching [a-z][a-z0-9]*;
//   * a record with no `=`, an empty record, or a duplicate key is REFUSED --
//     the whole string, not the record;
//   * a key this build does not know is SKIPPED on read and CARRIED VERBATIM on
//     write. That last half is this file's whole obligation to its siblings:
//     `wtA`, `wtpathA`, `cc` and anything a later revision adds are written by
//     code that is not here, and a grid edit that dropped them would delete an
//     imported wavetable by drawing a step.
//
// WRITING IS MINIMAL-CHURN. Records that were there keep their position;
// a known record that has gone back to its default is dropped, so a grid the
// user cleared collapses the state back towards the empty string the contract
// says a v2 project must round-trip to. A record that is new is appended in the
// contract's own order.
//
// PARSE FAILURE IS READ-ONLY, NOT REPAIR. If the device hands back something
// this parser refuses, the grid draws but does not write: editing would mean
// choosing what the malformed half meant, and the contract's answer to that
// everywhere else is "refuse and change nothing".
// ---------------------------------------------------------------------------
constexpr char kStateTag[] = "nxspc1";
constexpr int  kGridSteps  = 16;
constexpr int  kGridLevels = 16;      // digit 0..15, level = d/15

struct SpecState {
    bool parsed  = false;             // the device's string was understood
    bool present = false;             // ...and it was not empty
    std::vector<std::pair<std::string, std::string>> recs;   // in arrival order
    int  grid[3][kGridSteps]{};       // digit 0..15 per step
    int  smooth[3]{};                 // thousandths, 0..1000

    // v4's two rows, and the ONE place this file's defaults are not zero.
    //
    // The contract says so loudly and gives the reason: "a missing block reads
    // as its default, and every default is inert" is v3's rule, and for an LFO
    // grid inert means all zeros -- but an all-zero arp STEP row is an arp that
    // plays nothing, which is a broken default rather than an inert one. The
    // rows are SHAPE; the inert switch is Arp On (109), which defaults to 0.
    // So `arpl` defaults to sixteen f's and `arps` to `05` sixteen times --
    // and 0x05 and not 0x01, because the octave field is BIASED (code 2 is
    // offset 0), which is the trap a reader who skims the bit table falls into.
    int  arpLvl[kGridSteps] = {15, 15, 15, 15, 15, 15, 15, 15,
                               15, 15, 15, 15, 15, 15, 15, 15};
    // THE STEP ROW IS KEPT RAW, and that is the important half.
    //
    // Bits 5..7 are reserved, and "a later, wider build writes them" is exactly
    // who this panel is carrying bytes for. Masking them on READ is what the
    // contract asks (they are ignored); masking them on WRITE would delete a
    // future field the first time a user dragged a bar -- the same failure the
    // unknown-RECORD rule exists to prevent, one level down. So the byte is
    // stored whole, the interpretation masks and clamps, and a step the stroke
    // never touched emits the bytes it arrived with.
    int  arpRaw[kGridSteps] = {5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5};

    // THE CONTRACT'S KEY CHARSET, and a discrepancy inside the contract that
    // this reader must not turn into a refusal. The state-string section says
    // `key` matches `[a-z][a-z0-9]*`; the key TABLE four lines below it names
    // `wtA`, `wtB`, `wtpathA` and `wtpathB`, which that regex rejects. The
    // table is the authority -- it is the list of keys that actually exist, and
    // the DSP emits exactly those spellings -- so the accepted set is the
    // superset that contains both readings. What the rule is really for is
    // refusing an EMPTY key and a key with punctuation in it, and this still
    // refuses both.
    static bool keyOk(const std::string& k) {
        const auto alpha = [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        };
        if (k.empty() || !alpha(k[0])) return false;
        for (char c : k)
            if (!alpha(c) && !(c >= '0' && c <= '9')) return false;
        return true;
    }
    static int hexOf(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        return -1;                    // uppercase is REFUSED, per the contract
    }
    const std::string* find(const char* key) const {
        for (const auto& r : recs) if (r.first == key) return &r.second;
        return nullptr;
    }
    bool gridEmpty(int n) const {
        for (int i = 0; i < kGridSteps; ++i) if (grid[n][i]) return false;
        return true;
    }

    // --- the arp rows, read ------------------------------------------------
    // The two DEGRADATIONS the contract names -- an octave code of 5..7 clamps
    // to 4, bits 5..7 are ignored -- live HERE and nowhere else, because they
    // are reading rules and not storage rules. Both are values a later, wider
    // build could legitimately write, which is why neither is a refusal.
    bool arpOn(int i)  const { return (arpRaw[i] & 0x01) != 0; }
    bool arpTie(int i) const { return arpOn(i) && (arpRaw[i] & 0x10) != 0; }
    int  arpOct(int i) const { return std::min(4, (arpRaw[i] >> 1) & 7) - 2; }
    // ...and written, through the one door, so the reserved bits cannot be
    // dropped by a caller that forgot they were there.
    void arpSet(int i, bool on, bool tie, int oct) {
        arpRaw[i] = (arpRaw[i] & 0xE0) | (tie ? 0x10 : 0) |
                    (clampv(oct + 2, 0, 4) << 1) | (on ? 0x01 : 0);
    }
    bool arpLvlDefault() const {
        for (int i = 0; i < kGridSteps; ++i) if (arpLvl[i] != 15) return false;
        return true;
    }
    bool arpStepDefault() const {
        for (int i = 0; i < kGridSteps; ++i) if (arpRaw[i] != 0x05) return false;
        return true;
    }

    bool parse(const std::string& s) {
        *this = SpecState{};
        if (s.empty()) { parsed = true; return true; }
        size_t p = s.find(';');
        const std::string tag = s.substr(0, p == std::string::npos ? s.size() : p);
        if (tag != kStateTag) return false;
        while (p != std::string::npos) {
            const size_t q = s.find(';', p + 1);
            const std::string rec =
                s.substr(p + 1, q == std::string::npos ? std::string::npos : q - p - 1);
            p = q;
            if (rec.empty()) return false;                 // `;;` says nothing twice
            const size_t eq = rec.find('=');
            if (eq == std::string::npos) return false;
            const std::string k = rec.substr(0, eq), v = rec.substr(eq + 1);
            if (!keyOk(k) || find(k.c_str())) return false;
            recs.emplace_back(k, v);
        }
        present = !recs.empty();
        // The two blocks this panel owns, decoded. A malformed value in a key
        // this build DOES know is a refusal: the writer could not have produced
        // it, so guessing is the one thing the contract forbids.
        for (int n = 0; n < 3; ++n) {
            char key[12];
            snprintf(key, sizeof key, "lfo%d", n + 1);
            if (const std::string* g = find(key)) {
                if ((int)g->size() != kGridSteps) return false;
                for (int i = 0; i < kGridSteps; ++i) {
                    const int d = hexOf((*g)[(size_t)i]);
                    if (d < 0) return false;
                    grid[n][i] = d;
                }
            }
            snprintf(key, sizeof key, "smooth%d", n + 1);
            if (const std::string* sm = find(key)) {
                if (sm->empty() || sm->size() > 4) return false;
                if (sm->size() > 1 && (*sm)[0] == '0') return false;   // no leading zeros
                int v = 0;
                for (char c : *sm) {
                    if (c < '0' || c > '9') return false;
                    v = v * 10 + (c - '0');
                }
                if (v > 1000) return false;
                smooth[n] = v;
            }
        }
        // v4's two rows, on exactly the same terms. A LENGTH that is wrong, a
        // character outside [0-9a-f] and an uppercase character are strings this
        // writer could not have produced, so they refuse the whole state -- the
        // contract's own list. Everything a wider build could legally write is
        // handled by the accessors above instead, which is the line between a
        // refusal and a degradation.
        if (const std::string* al = find("arpl")) {
            if ((int)al->size() != kGridSteps) return false;
            for (int i = 0; i < kGridSteps; ++i) {
                const int d = hexOf((*al)[(size_t)i]);
                if (d < 0) return false;
                arpLvl[i] = d;
            }
        }
        if (const std::string* as = find("arps")) {
            if ((int)as->size() != kGridSteps * 2) return false;
            for (int i = 0; i < kGridSteps; ++i) {
                const int hi = hexOf((*as)[(size_t)(i * 2)]);
                const int lo = hexOf((*as)[(size_t)(i * 2 + 1)]);
                if (hi < 0 || lo < 0) return false;
                arpRaw[i] = hi * 16 + lo;      // whole, reserved bits included
            }
        }
        parsed = true;
        return true;
    }

    // What this panel's two blocks should read as now, folded back into the
    // records that were already there. Unknown keys keep their place and their
    // bytes; a defaulted block leaves no record behind at all.
    std::string emit() const {
        // Is `k` one of the two blocks this panel owns? If so `v` comes back as
        // the value it should carry NOW, and empty means "back at its default,
        // so it gets no record at all".
        const auto mine = [&](const std::string& k, std::string& v) {
            for (int n = 0; n < 3; ++n) {
                char g[12], sm[12];
                snprintf(g, sizeof g, "lfo%d", n + 1);
                snprintf(sm, sizeof sm, "smooth%d", n + 1);
                if (k == g) {
                    v.clear();
                    if (!gridEmpty(n))
                        for (int i = 0; i < kGridSteps; ++i)
                            v += "0123456789abcdef"[clampv(grid[n][i], 0, 15)];
                    return true;
                }
                if (k == sm) {
                    v = smooth[n] > 0 ? std::to_string(clampv(smooth[n], 0, 1000))
                                      : std::string();
                    return true;
                }
            }
            // v4's two. Same shape, same "at its default, so it gets no record
            // at all" -- and the default here is a pattern rather than nothing,
            // so a fresh instance still round-trips to the empty string the
            // contract says a v2 project must.
            if (k == "arpl") {
                v.clear();
                if (!arpLvlDefault())
                    for (int i = 0; i < kGridSteps; ++i)
                        v += "0123456789abcdef"[clampv(arpLvl[i], 0, 15)];
                return true;
            }
            if (k == "arps") {
                v.clear();
                if (!arpStepDefault())
                    for (int i = 0; i < kGridSteps; ++i) {
                        const int b = arpRaw[i] & 0xff;
                        v += "0123456789abcdef"[(b >> 4) & 0xf];
                        v += "0123456789abcdef"[b & 0xf];
                    }
                return true;
            }
            return false;
        };
        std::vector<std::pair<std::string, std::string>> out;
        for (const auto& r : recs) {
            std::string v;
            if (!mine(r.first, v)) { out.push_back(r); continue; }   // not ours
            if (!v.empty()) out.emplace_back(r.first, v);            // ours, in use
        }                                                           // ours, default: gone
        // ...and the blocks that are newly in use, in the contract's own order.
        const auto want = [&](const char* k) {
            for (const auto& r : out) if (r.first == k) return;
            std::string v;
            if (mine(k, v) && !v.empty()) out.emplace_back(k, v);
        };
        char k[12];
        for (int n = 0; n < 3; ++n) { snprintf(k, sizeof k, "lfo%d", n + 1);    want(k); }
        for (int n = 0; n < 3; ++n) { snprintf(k, sizeof k, "smooth%d", n + 1); want(k); }
        want("arpl");                          // the contract's own order:
        want("arps");                          // the level row, then the step row
        if (out.empty()) return {};
        std::string s = kStateTag;
        for (const auto& r : out) { s += ';'; s += r.first; s += '='; s += r.second; }
        return s;
    }
};

// A micro-label cut to fit its box -- app_devices.cpp's helper, and for the
// same reason: microIn draws glyph by glyph to get §5's tracking, so it has no
// ellipsis logic, and a scissor around every label would cost two draw calls
// apiece.
void microFit(Ui& ui, const Font& f, const Rect& b, const char* s, const Col& c,
              Align a = Align::Center, f32 pad = 0.f) {
    if (!s || !*s) return;
    const f32 avail = b.w - pad * 2.f;
    if (avail <= 1.f) return;
    char buf[72];
    snprintf(buf, sizeof buf, "%s", s);
    if (ui.microWidth(f, buf) > avail) {
        const f32 dots = ui.microWidth(f, "..");
        size_t n = strlen(buf);
        while (n > 1 && ui.microWidth(f, buf) + dots > avail) buf[--n] = 0;
        if (n + 2 < sizeof buf) { buf[n] = '.'; buf[n + 1] = '.'; buf[n + 2] = 0; }
    }
    ui.microIn(f, b, buf, c, a, pad);
}

// ---------------------------------------------------------------------------
// PANEL-LOCAL STATE that should be App members, held here as file statics.
//
// The page the panel is showing and the preset popover's open/scroll/cursor
// are UI state exactly like spectraPreset_ -- but app.h is another wave's file
// this round, so they live here, keyed on the open panel's uid and reset when
// it changes. That is sound because at most ONE Spectra panel is open at a
// time (spectraOpenUid_ is a single field), and the cost is only that the
// state is process-global rather than per-App -- a distinction without a
// difference in a program with one App. The proposed member form is filed as
// sed-filed-src-ui-app.h.diff; when it lands, these statics become those
// members and nothing else in this file moves.
// ---------------------------------------------------------------------------
struct SpectraDrop {
    bool open = false;
    bool pending = false;   // the debug hook standing in for a click, see below
    // True only for the frame openDrop() ran on. A fast click delivers its
    // press and its release in ONE frame; the release opens the popover
    // through the chip, and the popover's click-outside rule would then read
    // the SAME frame's press -- at the chip, which is outside the list -- and
    // close what just opened. The rule skips its birth frame instead.
    bool justOpened = false;
    f32  scroll = -1.f;     // device px; -1 = "bring the current preset into view"
    int  cursor = -1;       // keyboard highlight, an index into the popover rows
};
SpectraDrop g_drop;
int g_page = 0;             // 0 MAIN, 1 MOD
u64 g_pageUid = 0;          // which panel the two fields above belong to

// --- v3's own panel-local state, on the same terms and for the same reason ---

// THE WAVETABLE IMPORT. `osc` is which oscillator the hero's import row is
// pointed at -- A or B -- and it is UI state and not a parameter, because
// "which of the two am I loading" is a question about the pointer and not
// about the sound. `err` holds the refusal of the last import that failed, so
// the amber line survives the frame the drop happened on; it is cleared by the
// next successful import or by pointing the row at the other oscillator.
// `dragHold` is NXTAKT_DEBUG_SPECTRAWTDRAG's, re-armed every frame for exactly
// the reason app_sampler.cpp re-arms its own.
struct SpectraWt {
    int osc = 0;
    std::string err;
    std::string dragHold;
};
SpectraWt g_wt;

// THE GRID PAINT GESTURE. `lfo` is -1 when nothing is being drawn. The start
// step and level are what a shift-drag interpolates FROM: a line needs two
// ends, and the first end is where the press landed.
struct SpectraGrid {
    int lfo = -1;
    int startStep = 0, startLevel = 0;
    // THE ERASE SWEEP, which is the same stroke with the other button down.
    // `erase` is the grid a right-drag is clearing (-1 for none) and
    // `eraseStep` is where the last frame's sample landed, so the span between
    // two frames is cleared rather than the one step the pointer happened to be
    // over -- exactly the hole-filling the paint stroke already does, for
    // exactly the same reason: a hand sweeping across sixteen steps moves
    // faster than the frame that is watching it.
    int erase = -1;
    int eraseStep = 0;
};
// FOUR drawable level rows share this since v4, keyed by SLOT: 0..2 are LFO
// 1..3's grids and 3 is the arp's level row. One struct because it is one
// stroke -- the arp's level row is the LFO grid's gesture verbatim, which is
// what "identical in feel" has to mean if it is to stay true.
SpectraGrid g_gridDrag;

// THE ARP STEP ROW'S OWN GESTURE, and it needs one because the row is not a
// level row: a cell there carries a three-state on/tie/off AND an octave offset
// of -2..+2, which are two edits of two different kinds. The whole argument for
// separating them by the stroke's AXIS rather than by a modifier is at the
// drawing site; this is only the bookkeeping it needs.
struct SpectraArpStep {
    bool active = false;
    // 0 undecided, 1 horizontal (paint the state), 2 vertical (set the octave).
    // Decided once per stroke and never revisited: an axis that could flip
    // mid-stroke is a gesture that changes verb under your hand.
    int  axis = 0;
    int  cell = 0;              // the step the press landed in
    f32  px = 0.f, py = 0.f;    // ...and where, which is what decides the axis
    int  raw0 = 0;              // that step's byte BEFORE the press, for the revert
    int  paint = 0;             // 0 rest, 1 sound, 2 tie -- the state being stamped
    int  last = 0;              // the previous frame's step, so a span is filled
    bool erase = false;
    int  eraseStep = 0;
};
SpectraArpStep g_arpDrag;

// DRAG-ASSIGN, in three states and no more:
//   src < 0                      nothing in flight
//   src >= 0 && !tuning          a source is being dragged; the pointer is
//                                looking for a modulatable control
//   tuneSlot >= 0                a drop has landed and the destination knob is
//                                temporarily the new slot's AMOUNT knob
// The third state is what "opens its amount for immediate drag" means in a UI
// whose gesture ended when the button came up: the control you dropped on is
// where the depth is set, immediately, without going to look for the slot.
struct SpectraAssign {
    int src = -1;               // matrix source value being dragged
    // THE GESTURE RUN BACKWARDS, which is what the control menu's "Assign to
    // matrix slot" needs. A drag knows its source and goes looking for a
    // destination; a right-click on a knob knows its DESTINATION and has to go
    // looking for a source. Same three states, same ghost, same refusals -- so
    // it is the same struct with the ends swapped, and the source handles that
    // already light up under a drag light up under this too.
    //   wantDest < 0                nothing pending
    //   wantDest >= 0               a destination is waiting for a source; the
    //                               next click on a source handle completes it
    int wantDest = -1;          // matrix destination value the menu picked
    int wantParam = -1;         // ...and the panel control it came from
    char wantLabel[24] = {};    // ...and that control's own name, for the say-so
    int tuneSlot = -1;          // the slot whose amount the destination knob edits
    int tuneDest = -1;          // ...and the destination it was assigned to
    int tuneParam = -1;         // the panel control that is standing in for it
    // The drop lands INSIDE the destination knob's own draw, which has already
    // gone past the point where it would have drawn itself as an amount. So the
    // frame the assignment happens on is a frame amount mode is not visible on,
    // and the end-of-frame rule below would read that as "the knob is gone" and
    // cancel what had just been made. One flag, and it costs one frame.
    bool tuneFresh = false;
};
SpectraAssign g_assign;

// THE SAVE FIELD. Open means the preset chip has become a text field; the
// buffer is the marker-rename idiom's `renameBuf_`, held here for the reason
// every other field in this file is held here.
struct SpectraSave {
    bool open = false;
    bool pending = false;       // the debug hook standing in for a click
    // SpectraDrop::justOpened's twin, and the same bug it was written for.
    // textField's dismissal rule is "a press anywhere else takes the value as
    // typed" -- which is right, and which fires on the BIRTH FRAME if a press
    // is already in flight when the field is opened by something that is not
    // that press. The chip's own click cannot do it (a button fires on the
    // RELEASE, so the press is a frame old by then) but the debug hook can, and
    // so could any future opener. The field skips its birth frame instead.
    bool justOpened = false;
    std::string buf;
};
SpectraSave g_save;

// ===========================================================================
// v5 -- THE WAVETABLE EDITOR'S OWN STATE AND ARITHMETIC
//
// Everything from here to the end of this block is the DRAW page's, and all of
// it is EDITOR-LOCAL: not a parameter, not a state record, not saved, and
// nothing here is identity until commitFrames() takes it. The contract says
// that in as many words about the frame cursor and it is true of the whole
// working copy: "the editor's working copy is not the playing table, and the
// playing table changes only at commit".
//
// THE TRANSFORM IS THIS FILE'S OWN, AND THAT IS ALLOWED. The contract's
// implementation notes tell the DSP author to reuse spBuildCustomMips()'s
// spIfft/spTwiddle for the morph -- and this file has never seen spectra.cpp
// and must never need to. There is no contradiction, because determinism
// obligation 2 draws the line in exactly the right place: "downstream of the
// frames, yes, absolutely; upstream of them, no, and it does not need to be."
// What crosses the wire and what feeds contentHash() is the FRAMES. So the
// editor owes a transform that is DETERMINISTIC -- fixed radix, fixed order,
// a twiddle table built once ascending in k -- and owes nothing else, and this
// is one. Two runs of the same drawing in one process and one in a fresh
// process produce the identical frames, which is obligation 5 in full.
// ===========================================================================
constexpr int kWeFrames  = 32;        // kSpFrames. The contract freezes it.
constexpr int kWeCycle   = 2048;      // wt::kCycle
constexpr int kWeHarm    = 1023;      // what a 2048-point cycle carries
constexpr int kWeBars    = 256;       // ...and the most a human can address
constexpr f32 kWeFloorDb = -80.f;     // bar floor; AT the floor the magnitude
                                      // is a hard 0.0f, never 10^(-80/20)
constexpr f32 kWeSilentPk = 1e-9f;    // the import path's own constant
constexpr int kWeUndo    = 48;        // editor-local, bounded, per the notes

inline void weSwap(f32& a, f32& b) { const f32 t = a; a = b; b = t; }

// The twiddles, built ONCE, ascending in k, in f64 and stored in f32. This is
// the one place the pen path calls libm besides 10^(dB/20), and it is the same
// call the mip builder already makes for every table in the instrument.
struct WeTwiddle {
    f32 cr[kWeCycle / 2], ci[kWeCycle / 2];
    WeTwiddle() {
        for (int k = 0; k < kWeCycle / 2; ++k) {
            const f64 a = -2.0 * 3.14159265358979323846 * (f64)k / (f64)kWeCycle;
            cr[k] = (f32)std::cos(a);
            ci[k] = (f32)std::sin(a);
        }
    }
};
const WeTwiddle& weTw() { static const WeTwiddle t; return t; }

// In-place iterative radix-2 FFT at exactly kWeCycle. Ascending in stage,
// ascending in block, ascending in k -- a fixed order, so the same frame
// analyses to the same spectrum every time it is asked.
void weFft(f32* re, f32* im, bool inverse) {
    constexpr int N = kWeCycle;
    for (int i = 1, j = 0; i < N; ++i) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j |= bit;
        if (i < j) { weSwap(re[i], re[j]); weSwap(im[i], im[j]); }
    }
    const WeTwiddle& tw = weTw();
    for (int len = 2; len <= N; len <<= 1) {
        const int half = len >> 1, step = N / len;
        for (int i = 0; i < N; i += len) {
            for (int k = 0; k < half; ++k) {
                const int t = k * step;
                const f32 wr = tw.cr[t];
                const f32 wi = inverse ? -tw.ci[t] : tw.ci[t];
                const int a = i + k, b = a + half;
                const f32 xr = re[b] * wr - im[b] * wi;
                const f32 xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr;        im[a] += xi;
            }
        }
    }
    if (inverse) {
        const f32 g = 1.f / (f32)N;
        for (int i = 0; i < N; ++i) { re[i] *= g; im[i] *= g; }
    }
}

// THE ANALYSIS SCALING IS 2/N, which is the contract's own: "bar top is 0 dB =
// magnitude 1.0, which is a full-scale sine at that harmonic under the
// analysis scaling 2/N the mip builder already uses". Check it against the
// synthesis below and they agree by construction: a frame that is exactly
// sin(2*pi*h*i/N) analyses to m_h = 1 and nothing else.
inline f32 weMagOf(const f32* re, const f32* im, int h) {
    return 2.f * std::sqrt(re[h] * re[h] + im[h] * im[h]) / (f32)kWeCycle;
}

// dB <-> magnitude. The ONE libm call this feature adds, sitting exactly where
// the import path's std::sin sits and carrying the identical bounded
// consequence (determinism obligation 2). AT the floor it is a hard zero and
// not 10^(-80/20): "drag it away" has to mean the harmonic is gone.
inline f32 weDbToMag(f32 db) {
    return db <= kWeFloorDb ? 0.f : std::pow(10.f, db * 0.05f);
}
inline f32 weMagToDb(f32 m) {
    return m <= 0.f ? kWeFloorDb
                    : clampv(20.f * std::log10(m), kWeFloorDb, 0.f);
}

// DC removal, exactly as the contract spells it: mean accumulated in f64 in
// ASCENDING index order, subtracted in f32 in ASCENDING index order. Fixed
// accumulation type and fixed order, so the same drawing gives the same frame.
void weDcRemove(f32* frame) {
    f64 acc = 0.0;
    for (int i = 0; i < kWeCycle; ++i) acc += (f64)frame[i];
    const f32 mean = (f32)(acc / (f64)kWeCycle);
    for (int i = 0; i < kWeCycle; ++i) frame[i] -= mean;
}

// SINE-PHASE SYNTHESIS, and the reason it is an inverse transform rather than
// a sum of sines is speed and nothing else -- the two are the same arithmetic.
// X[h] = -i * m_h * N/2 and X[N-h] = conj(X[h]) gives, term by term,
// (1/N)(X_h e^{i0} + X_{N-h} e^{-i0}) = m_h sin(0), which is the contract's
// formula with h ascending. DC is zero by construction and so is Nyquist: the
// contract's sum runs h = 1..1023 and bin 1024 is not one of them.
//
// `mag` is indexed by harmonic, 1..kWeHarm; mag[0] is ignored.
// `keepRe` / `keepIm`, when given, are the HELD analysis, and `sine[h]` says
// which harmonics carry the convention's phase instead. That is the whole of
// "touched harmonics carry (m, sine phase), untouched harmonics carry their
// analysed (re, im), and DC is zero".
void weSynth(const f32* mag, const bool* sine, const f32* keepRe, const f32* keepIm,
             f32* outFrame, f32* scratchRe, f32* scratchIm) {
    const f32 half = (f32)kWeCycle * 0.5f;
    scratchRe[0] = scratchIm[0] = 0.f;                      // DC: forced to zero
    scratchRe[kWeCycle / 2] = scratchIm[kWeCycle / 2] = 0.f; // Nyquist: not a harmonic
    for (int h = 1; h <= kWeHarm; ++h) {                    // ASCENDING in h
        // `sine` is the BAR array and is 1..256 long, so a harmonic above the
        // pen's reach can never be "touched" -- which is the contract's rule
        // (257..1023 keep their full complex value) falling out of the bound
        // rather than being enforced by a second test.
        const bool conv = !keepRe || !sine || (h <= kWeBars && sine[h]);
        f32 xr, xi;
        if (conv) { xr = 0.f;        xi = -mag[h] * half; }
        else      { xr = keepRe[h];  xi = keepIm[h]; }
        scratchRe[h] = xr;                 scratchIm[h] = xi;
        scratchRe[kWeCycle - h] = xr;      scratchIm[kWeCycle - h] = -xi;
    }
    weFft(scratchRe, scratchIm, true);
    for (int i = 0; i < kWeCycle; ++i) outFrame[i] = scratchRe[i];
}

// Is this frame already in the pen's sine phase? Every harmonic of a sine-phase
// frame is purely negative-imaginary, so the real part is the deviation, and
// weighting it by magnitude is what stops a numerical speck at -200 dB from
// answering for the whole frame. An all-zero frame answers "yes", which is
// right: silence has no phase step.
bool weIsSinePhase(const f32* re, const f32* im) {
    f64 num = 0.0, den = 0.0;
    for (int h = 1; h <= kWeHarm; ++h) {
        const f64 a = std::sqrt((f64)re[h] * re[h] + (f64)im[h] * im[h]);
        num += (f64)std::fabs(re[h]) * a;
        den += a * a;
    }
    return den <= 1e-20 || (num / den) < 1e-3;
}

// wtpath's escaping, read back for DISPLAY only. The contract's writer escapes
// any byte <= ' ', >= 0x7F, or one of % ; , : = as `%` plus two uppercase hex.
// This is the inverse, and it is deliberately not the contract's STRICT
// unescape: the only consumer is the "Saved to <path>" sentence, and a path
// that will not decode is shown raw rather than withheld. Nothing here reads
// back into a record.
std::string weUnescape(const std::string& v) {
    std::string out;
    out.reserve(v.size());
    const auto hex = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '%' && i + 2 < v.size()) {
            const int hi = hex(v[i + 1]), lo = hex(v[i + 2]);
            if (hi >= 0 && lo >= 0) { out += (char)(hi * 16 + lo); i += 2; continue; }
        }
        out += v[i];
    }
    return out;
}

// ---------------------------------------------------------------------------
// THE WORKING COPY, and every scrap of state the DRAW page owns.
//
// A file static for the reason every other panel-local struct above is one:
// app.h is another wave's file. Keyed on the open panel's uid and dropped when
// it changes, exactly like g_page. 32 x 2048 floats is 256 KiB and there are
// two of them (the working copy and what was READ, which is what draws the
// "this frame differs" marks); the undo stack is per-frame steps, bounded at
// 48, so its worst case is 48 x 8 KiB and its whole-table steps -- Insert,
// Delete, Morph, Re-phase -- are the four that cost 256 KiB apiece.
//
// EVERYTHING IS ALLOCATED LAZILY, on the first open, and released when the
// editor closes. A user who never draws pays nothing, which is the same
// discipline drawnDir() follows on the other side of the seam.
// ---------------------------------------------------------------------------
struct SpectraEdit {
    bool open = false;
    u64  uid  = 0;
    int  osc  = 0;                       // which oscillator was read
    int  view = 0;                       // 0 the waveform pen, 1 the harmonic pen
    int  cur  = 0;                       // the frame cursor, 0..31, EDITOR-ONLY
    int  ma = 0, mb = 31;                // the morph's two endpoints
    bool fromDevice = false;             // readFrames() answered
    bool stretched  = false;             // ...and it had fewer than 32 frames
    int  srcFrames  = 0;

    std::vector<f32> f;                  // 32 * 2048, the working copy
    std::vector<f32> was;                // ...as it was read or last committed
    std::vector<f32> clip;               // one frame, the editor-local clipboard
    bool clipOk = false;

    // THE HELD SPECTRUM, for the CURSOR FRAME ONLY. The gate is "N consecutive
    // harmonic edits perform exactly ONE forward analysis and N inverse
    // syntheses", and consecutive means without leaving the frame -- so moving
    // the cursor drops it and a waveform stroke drops it.
    std::vector<f32> sre, sim;           // 2048 each: the analysed spectrum
    std::vector<f32> mag;                // 1024: the bar magnitudes, h = 1..1023
    std::vector<f32> tre, tim;           // 2048 each: the synthesis scratch
    bool specOk = false;
    int  specFrame = -1;
    bool touched[kWeBars + 1]{};         // 1..256: this bar was edited
    int  nTouched = 0;
    int  nAnalyse = 0, nSynth = 0;       // the gate, counted on screen

    // The stroke. One at a time, and its kind decides which pen owns it.
    int  stroke = 0;                     // 0 none 1 draw 2 erase
    int  lastI = -1; f32 lastV = 0.f;    // the previous delivered point
    int  anchI = 0;  f32 anchV = 0.f;    // shift-drag's anchor, which never moves
    bool strokeChanged = false;
    // A SHIFT-DRAG IS A RUBBER BAND AND NOT A TRAIL. The line runs from an
    // anchor that does not move, so every frame of the drag must draw it onto
    // the state the stroke STARTED from -- otherwise a sweep leaves the
    // previous frame's longer line behind it and the "line tool" paints a fan.
    // The waveform pen restores from the undo step it pushed at the press; the
    // harmonic pen cannot (its undo step is the FRAME and its edit is the
    // magnitude array), so it keeps its own two.
    std::vector<f32>  magWas;
    std::vector<char> touchWas;
    f64  prevAt = 0.0;                   // when the last preview was published

    struct Step { int frame = -1; std::vector<f32> data; };
    std::vector<Step> undo;

    std::string name;                    // the display name, for setCustomName
    bool nameOpen = false, nameJustOpened = false;
    bool previewing = false;             // a preview is published and standing
    std::string savedPath;               // what the last commit reported
    std::string note;                    // the one-line round-trip sentence
    int  noteInk = 0;                    // 0 quiet 1 cyan (happening) 2 amber 3 violet

    // A MORPH FOUND ITS ENDPOINTS OFF SINE PHASE. The contract: "when a
    // Morph's endpoints are not in sine phase the editor says so in one line
    // ... and OFFERS it". The line is the canvas sentence; the offer is this,
    // which puts an amber ring on RE-PHASE until it is answered -- because the
    // line is one line and the whole of what it has to say does not fit in one.
    bool rephaseOffered = false;

    // The ask. The contract makes it an obligation: "an editor closing with
    // uncommitted changes MUST ask", because the alternative is losing an hour
    // of drawing to a window close.
    int  askWhat = 0;                    // 0 none 1 leave the page 2 close the
                                         // panel 3 read the other oscillator
    int  askArg  = 0;

    f32* frame(int k)             { return f.data() + (size_t)k * kWeCycle; }
    const f32* frame(int k) const { return f.data() + (size_t)k * kWeCycle; }
    const f32* wasFrame(int k) const { return was.data() + (size_t)k * kWeCycle; }

    bool frameDirty(int k) const {
        if (!open || (int)was.size() != kWeFrames * kWeCycle) return false;
        const f32* a = frame(k); const f32* b = wasFrame(k);
        for (int i = 0; i < kWeCycle; ++i) if (a[i] != b[i]) return true;
        return false;
    }
    bool anyDirty() const {
        if (!open) return false;
        for (int k = 0; k < kWeFrames; ++k) if (frameDirty(k)) return true;
        return false;
    }
    void alloc() {
        f.assign((size_t)kWeFrames * kWeCycle, 0.f);
        was.assign((size_t)kWeFrames * kWeCycle, 0.f);
        sre.assign(kWeCycle, 0.f); sim.assign(kWeCycle, 0.f);
        tre.assign(kWeCycle, 0.f); tim.assign(kWeCycle, 0.f);
        mag.assign(kWeCycle / 2, 0.f);
        magWas.assign(kWeCycle / 2, 0.f);
        touchWas.assign(kWeBars + 1, (char)0);
        clip.assign(kWeCycle, 0.f);
    }
    void release() {
        *this = SpectraEdit{};
    }
    // Every mutation goes through here first, so nothing can change the working
    // copy without leaving a way back. `k < 0` takes the whole table, which is
    // what the four operations that move frames around each other need.
    void push(int k) {
        Step st;
        st.frame = k;
        if (k < 0) st.data.assign(f.begin(), f.end());
        else       st.data.assign(f.begin() + (size_t)k * kWeCycle,
                                  f.begin() + (size_t)(k + 1) * kWeCycle);
        undo.push_back(std::move(st));
        if ((int)undo.size() > kWeUndo) undo.erase(undo.begin());
    }
    bool pop() {
        if (undo.empty()) return false;
        const Step& st = undo.back();
        if (st.frame < 0) {
            if (st.data.size() == f.size()) f = st.data;
        } else if ((int)st.data.size() == kWeCycle) {
            for (int i = 0; i < kWeCycle; ++i) frame(st.frame)[i] = st.data[(size_t)i];
        }
        undo.pop_back();
        specOk = false;
        return true;
    }
    // The set-wide peak, ascending, which is the number the commit's gain comes
    // from and the only thing about normalisation there is to watch.
    f32 peak() const {
        f32 pk = 0.f;
        for (size_t i = 0; i < f.size(); ++i) {
            const f32 a = std::fabs(f[i]);
            if (a > pk) pk = a;
        }
        return pk;
    }
    bool allFinite() const {
        for (size_t i = 0; i < f.size(); ++i) if (!std::isfinite(f[i])) return false;
        return true;
    }
    void say(const char* s, int ink) { note = s ? s : ""; noteInk = ink; }
};
SpectraEdit g_edit;

// THE DOCK HEIGHT THE TALLER CHIP REPLACED, and it lives OUTSIDE the editor
// on purpose. TALLER changes a setting that belongs to the detail panel, not
// to a drawing: if it were an editor member, leaving the DRAW page would drop
// the "put it back" affordance and leave the user with a 380px dock and no
// memory of what it was. The chip remembers across an editor's whole life, and
// zero means "the dock is where the user left it".
f32 g_weGrewFrom = 0.f;

// The two debug hooks' landing pad. They cannot write g_edit directly because
// the editor does not exist until the page is first drawn and opening it
// RELEASES the struct; these are consumed by the first open and then gone, so
// a hook is a starting state and never a thing that keeps re-asserting itself
// under a driven gesture.
int g_weSeedView = -1, g_weSeedFrame = -1;

// The analysis, and the ONE place it happens. Everything that wants a spectrum
// for the cursor frame calls this; it is a no-op when the held spectrum is
// already this frame's, which is exactly the gate ("N consecutive edits, ONE
// forward analysis") written as code rather than as a promise.
void weEnsureSpectrum(SpectraEdit& e) {
    if (e.specOk && e.specFrame == e.cur) return;
    const f32* fr = e.frame(e.cur);
    for (int i = 0; i < kWeCycle; ++i) { e.sre[(size_t)i] = fr[i]; e.sim[(size_t)i] = 0.f; }
    weFft(e.sre.data(), e.sim.data(), false);
    for (int h = 1; h <= kWeHarm; ++h)
        e.mag[(size_t)h] = weMagOf(e.sre.data(), e.sim.data(), h);
    e.mag[0] = 0.f;
    for (int b = 0; b <= kWeBars; ++b) e.touched[b] = false;
    e.nTouched = 0;
    e.specOk = true;
    e.specFrame = e.cur;
    ++e.nAnalyse;
}

// ...and the synthesis that answers a bar edit, from the HELD spectrum rather
// than from the frame it just wrote. Without that, fifty bar edits are fifty
// FFT round trips of accumulated f32 error.
void weResynth(SpectraEdit& e) {
    weSynth(e.mag.data(), e.touched, e.sre.data(), e.sim.data(),
            e.frame(e.cur), e.tre.data(), e.tim.data());
    ++e.nSynth;
}

// ---------------------------------------------------------------------------
// THE DISPLAYED WAVEFORM IS THE REAL ONE -- EXCEPT WHEN IT SAYS OTHERWISE
//
// It used to be an illustration, and the comment that stood here said so at
// length: the tables lived on the far side of the PluginInstance boundary,
// that boundary had exactly three ways through it (paramCount, paramInfo,
// getParam), and what was drawn was the same FAMILY of shapes regenerated on
// this side from the same two numbers the DSP morphs with.
//
// The plugin now publishes the set. detail::spectraTables() is a const pointer
// to the eight tables the voices are reading -- 32 frames of 2048 samples each,
// generated once per process and immutable -- and the hero display morphs
// between the same frame pair, at the same blend, at mip 0. What is on screen
// is the floats, not a drawing of them: band-limited where the real table is
// band-limited, and wrong about nothing.
//
// v2 adds warp, and warp DOES reshape the audible wave -- so the display has
// to say where it stands: the hero draws the UNWARPED table, and when a warp
// is audibly on, the corner label says "pre-warp" beside the frame number. An
// exact warped preview is not cheaply available: Bend/Mirror/Quantize are pure
// phase remaps this side could reproduce, but FM and RM read the OTHER
// oscillator's voice-0 signal at audio rate through a one-sample delay, and
// Sync re-picks the mip -- a preview that was exact for four modes and a lie
// for the others would be worse than a label that is true for all eight. If a
// per-mode preview is ever wanted, it must reproduce the contract's formulas
// exactly or stay out of the well.
//
// WHAT SURVIVES OF THE OLD PATH, and why it is not deleted. The accessor is
// null until some Spectra in the process has prepare()d, which is never in a
// set that contains none -- and the panel can be opened on a non-Spectra by the
// debug hook, in a program that has therefore built no tables at all. The
// generator below is what is drawn then, and the label in the corner of the
// well changes from "wavetable" to "illustration - no table set" so the two
// states can never be confused for each other. It is the SAME family of shapes
// as before, honestly wrong about the exact spectrum in the same way, and it is
// now reached in one state instead of all of them.
// ---------------------------------------------------------------------------
inline f32 fracf(f32 x) { return x - std::floor(x); }

f32 specimenSample(int table, f32 pos, f32 x) {
    x = fracf(x);
    pos = clampv(pos, 0.f, 1.f);
    switch (table) {
    case 0: {                                   // Basic: saw -> pulse continuum
        const f32 saw = 2.f * x - 1.f;
        const f32 sq  = x < 0.5f ? 1.f : -1.f;
        return saw * (1.f - pos) + sq * pos;
    }
    case 1: {                                   // PWM: the width sweeps
        const f32 duty = 0.5f - 0.46f * pos;
        // DC removed, so the trace stays centred in the well as the duty
        // narrows instead of climbing out of the top of it.
        return (x < duty ? 1.f : -1.f) - (2.f * duty - 1.f);
    }
    case 2: {                                   // Harmonic: odd <-> even
        const f32 odd = std::sin(kTwoPi * x)
                      + std::sin(kTwoPi * 3.f * x) / 3.f
                      + std::sin(kTwoPi * 5.f * x) / 5.f;
        const f32 even = std::sin(kTwoPi * 2.f * x) / 2.f
                       + std::sin(kTwoPi * 4.f * x) / 4.f
                       + std::sin(kTwoPi * 6.f * x) / 6.f;
        return odd * (1.f - pos) + even * pos * 2.f;
    }
    case 3: {                                   // Formant: the peak sweeps up
        const f32 f = 2.f + 11.f * pos;
        const f32 win = 0.5f * (1.f - std::cos(kTwoPi * x));
        return std::sin(kTwoPi * f * x) * win;
    }
    case 4: {                                   // Bell: inharmonic FM
        const f32 idx = 0.4f + 5.5f * pos;
        return std::sin(kTwoPi * x + idx * std::sin(kTwoPi * 1.41f * x));
    }
    case 5: {                                   // Digital: sync + bit steps
        const f32 k = 1.f + 3.f * pos;
        const f32 y = 2.f * fracf(x * k) - 1.f;
        const f32 q = 3.f + std::floor(13.f * pos);
        return std::floor(y * q) / q;
    }
    case 6: {                                   // Vox: a softer formant stack
        const f32 f1 = 2.f + 3.f * pos, f2 = 5.f + 6.f * pos;
        const f32 win = 0.35f + 0.65f * (0.5f * (1.f - std::cos(kTwoPi * x)));
        return (0.62f * std::sin(kTwoPi * x)
              + 0.28f * std::sin(kTwoPi * f1 * x)
              + 0.14f * std::sin(kTwoPi * f2 * x)) * win;
    }
    default: {                                  // Fold: sine into a wavefold
        f32 y = std::sin(kTwoPi * x) * (1.f + 5.f * pos);
        for (int i = 0; i < 8 && std::fabs(y) > 1.f; ++i)
            y = (y > 0.f ? 2.f : -2.f) - y;
        return y;
    }
    }
}

// ---------------------------------------------------------------------------
// PEAK-PRESERVING DECIMATION
//
// A frame is 2048 samples and the well is a hundred-odd pixels wide, so twenty
// samples have to become one column. STRIDING them -- keeping every twentieth
// and dropping the rest -- is the obviously wrong answer, and wrong in a way
// that is worse than blurry: a wavetable's character lives in its extremes, so
// a saw loses its edge, the Digital table loses the steps that are its whole
// point, and any frame whose top harmonic beats against the stride grows a
// moire pattern that is nowhere in the audio. The display would be showing
// something the instrument does not contain, which is the exact failure this
// whole change was made to end.
//
// So each column reduces its own span to the two samples that matter, the
// smallest and the largest, and emits BOTH -- in the order they occur, at the
// position they occur at. Keeping the order and the position is what makes the
// result a waveform and not an envelope: the polyline still runs left to right
// through time and a ramp still draws as a ramp, it simply cannot miss a peak
// on the way past. Cost is one pass over 2048 floats per trace, twice a frame.
//
// Output is (u, y) pairs, u in 0..1 across one cycle. The last pair closes the
// cycle at u = 1 with the first sample again, which is not decoration: a frame
// IS periodic, and a trace that stopped one sample short would leave a notch
// against the right-hand edge of the well.
// ---------------------------------------------------------------------------
template <class Sample>
void decimateFrame(std::vector<f32>& out, int cols, int n, const Sample& sample) {
    out.clear();
    if (cols < 1 || n < 1) return;
    out.reserve((size_t)cols * 4 + 2);
    for (int c = 0; c < cols; ++c) {
        const int i0 = (int)((long long)c * n / cols);
        int i1 = (int)((long long)(c + 1) * n / cols);
        if (i1 <= i0) i1 = i0 + 1;
        if (i1 > n) i1 = n;
        int lo = i0, hi = i0;
        f32 vlo = sample(i0), vhi = vlo;
        for (int i = i0 + 1; i < i1; ++i) {
            const f32 v = sample(i);
            if (v < vlo) { vlo = v; lo = i; }
            if (v > vhi) { vhi = v; hi = i; }
        }
        const bool loFirst = lo <= hi;
        const int  ia = loFirst ? lo  : hi;
        const int  ib = loFirst ? hi  : lo;
        const f32  ya = loFirst ? vlo : vhi;
        const f32  yb = loFirst ? vhi : vlo;
        out.push_back((f32)ia / (f32)n); out.push_back(ya);
        out.push_back((f32)ib / (f32)n); out.push_back(yb);
    }
    out.push_back(1.f); out.push_back(sample(0));
}

// Scale a decimated trace to unit peak. Only the ILLUSTRATION needs this --
// the real frames arrive unit-peak from the generator, which normalises every
// one of them to its own widest mip. Applying it after decimation is exact
// rather than approximate: decimation keeps the extremes, so the peak of what
// is left is the peak of what there was.
void normaliseTrace(std::vector<f32>& pts) {
    f32 peak = 1e-6f;
    for (size_t i = 1; i < pts.size(); i += 2) peak = std::max(peak, std::fabs(pts[i]));
    const f32 k = 1.f / peak;
    for (size_t i = 1; i < pts.size(); i += 2) pts[i] *= k;
}

// The ADSR curve, drawn from the four values and honest to the exponential
// segments a synth envelope actually runs: the attack is convex, the decay and
// the release are the same falling exponential, and the sustain is flat.
//
// The TIME axis is logarithmic, and that is the only cosmetic choice in here:
// a 5 ms attack against an 8 s release on a linear axis is one pixel against
// two hundred, which draws every envelope in the program as an identical
// right-angled step. Log widths make the four knobs visibly do something.
f32 envAt(f32 u, f32 atk, f32 dec, f32 sus, f32 rel) {
    const f32 wa = std::log10(1.f + atk), wd = std::log10(1.f + dec),
              wr = std::log10(1.f + rel);
    const f32 sum = std::max(1e-3f, wa + wd + wr);
    const f32 ta = 0.72f * wa / sum, td = 0.72f * wd / sum, tr = 0.72f * wr / sum;
    const f32 th = 0.28f;                          // the sustain hold, fixed
    if (u < ta) {
        const f32 t = ta > 1e-6f ? u / ta : 1.f;
        return (1.f - std::exp(-4.f * t)) / (1.f - std::exp(-4.f));
    }
    if (u < ta + td) {
        const f32 t = td > 1e-6f ? (u - ta) / td : 1.f;
        return sus + (1.f - sus) * std::exp(-4.f * t);
    }
    if (u < ta + td + th) return sus;
    const f32 t = tr > 1e-6f ? (u - ta - td - th) / tr : 1.f;
    return sus * std::exp(-4.f * clampv(t, 0.f, 1.f));
}

} // namespace

// ---------------------------------------------------------------------------
// Identity and panel resolution
// ---------------------------------------------------------------------------

bool App::isSpectra(const PluginInstance* p) {
    return p && p->desc().uri == "nxtakt:spectra";
}

// Resolved from the uid every frame, and a uid rather than a chain index for
// the reason rackOpenUid_ is one: a chain edit must not slide an open panel
// onto whatever device inherited the slot.
int App::spectraOpenIdx(const std::vector<DeviceModel>& devices) const {
    if (!spectraOpenUid_) return -1;
    for (size_t i = 0; i < devices.size(); ++i) {
        if (devices[i].uid != spectraOpenUid_) continue;
        if (!devices[i].inst) return -1;
        // spectraForced_ is the debug hook standing in for a mouse: it opens
        // the panel on whatever device is first in the chain, which is how the
        // guarded states get a screenshot of their own.
        return (spectraForced_ || isSpectra(devices[i].inst.get())) ? (int)i : -1;
    }
    return -1;                                   // the device went away
}

// ---------------------------------------------------------------------------
// The panel
//
// THE SUB-ID REGISTRY for uiId(UiSpectraPanel, sub, uidKey). v1 grew these at
// the call sites and collected a real collision doing it: the steppers spent
// 30..32+id, which for OSC B's table (id 8) lands on 40..42 -- the very
// numbers the filter segments were using, so hovering the LP segment also lit
// osc B's next-table arrow. The kinds are hashes, so nothing enforces this
// but a list; this is the list, and every band below is disjoint by
// inspection:
//
//     0          the close button
//     2          the MAIN/MOD/ARP page tab (tabPill derives its slot ids itself)
//    60..63      the preset row: prev, next, the name chip, the popover latch
//   100+id*4+k   a stepper on param `id` (k: 0 gesture, 1 prev, 2 next).
//                v4's widest id is 122, so this band's top is 590 -- ten below
//                the filter segments at 600, which is the whole reason the
//                filter block was numbered at 600 and not at 550.
//   600..605     the filter-type segments
//    50..55      LFO1's shape segments (v1's numbers, kept; v3 added the sixth)
//   630..645     LFO2 (630+) and LFO3 (640+) shape segments
//   650..652     the sub shape segments
//   660..662     the voice mode segments
//   670          the noise-track toggle
//   700..707     matrix source selectors, 720..727 destination selectors
//
// ...and v3's band, which is 800+ so that nothing it spends can walk into the
// stepper range (100 + id*4 + k, and the widest id is 110, so 542 is its top):
//
//   800          the hero well's drop target
//   801, 802     the import row's A / B target segments
//   803          the browse chip, 804 the revert-to-factory chip
//   810..812     the three LFO grids, 820..822 their smooth troughs
//   830..847     the source grab handles, indexed by matrix source value
//                (v4's Arp Step is source 17, so this band's top moved 841 ->
//                847 -- still clear of the save chip at 850)
//   850          the save chip, 851 the inline name field
//   860..867     the matrix's per-slot curve selectors
//   870          LFO1's compact sync selector (the Custom layout only)
//   880..882     the three LFO mode toggles
//
// ...and v4's band, 900+, chosen so it clears the stepper range's new top (590)
// and the whole of v3's:
//
//   900          Arp On, 901 Arp Hold, 902 Arp Retrig
//   904          the octave-mode micro-selector
//   905          the velocity-mode micro-selector
//   910          the arp level row, 911 the arp step row
//
// ...and v5's band, 1000+, which clears the whole of v4's and is the last one
// that can be added without renumbering anything (the stepper band tops out at
// 590 and is a function of the widest parameter id, which v5 does not move
// because v5 spends no ids at all):
//
//  1000, 1001    the pen's view tab: WAVE / HARM
//  1002, 1003    the editor's oscillator target, A / B
//  1004          the TALLER / SHORTER chip, which drives the detail splitter
//  1010..1017    the eight frame operations, in the contract's own order:
//                Clear, Copy, Paste, Duplicate, Insert, Delete, Morph, Re-phase
//  1020..1023    the morph endpoints: a's two arrows, then b's two
//  1030          the name field, 1031 the RENAME chip
//  1040..1043    UNDO, PREVIEW, COMMIT, REVERT
//  1050          THE CANVAS -- one hot rect for the whole drawing surface,
//                whichever pen is holding it, exactly as a drawable level row
//                is one hot rect for sixteen steps
//  1051          the 32-frame strip, on the same terms
//  1060..1062    the uncommitted-changes ask: COMMIT & GO, DISCARD, STAY
// ---------------------------------------------------------------------------

void App::drawSpectraPanel(const Rect& box, DeviceModel& dm, const Col& tc) {
    PluginInstance* inst = dm.inst.get();
    if (!inst) return;
    const f32 s = win_.dpiScale();
    const int pc = inst->paramCount();
    const bool real = isSpectra(inst);
    Input& in = win_.input();
    const int uidKey = (int)(dm.uid & 0xffffu);
    const int np = inst->presetCount();

    // THE 16-PIXEL FLOOR, WRITTEN AS A NUMBER RATHER THAN LEFT AS A HABIT.
    //
    // This panel packs a hundred controls into a dock 200 logical pixels tall,
    // so a row is 16 px or it is nothing -- and the measurement pass found rows
    // of 14, chips of 11 and troughs of 9. Two answers, in this order: the rows
    // themselves grew (subH is 16 now, and the title band 18), and what is
    // still short takes the difference in HIT SLOP.
    //
    // The slop is the DEFICIT and never more. widgets.h warns that past three
    // pixels neighbours start stealing each other's hover, and a segmented
    // cluster is the case it is warning about: pad two adjacent segments by
    // three each and the right-hand one swallows three pixels of the left-hand
    // one's own face, because last setHot() wins. Handing a control exactly
    // what it is short of means a control that already clears the floor is
    // padded by zero, which is the only value that cannot steal anything.
    const auto slop = [&](const Rect& r0) {
        const f32 want = 16.f * s;
        const f32 need = std::max(want - r0.w, want - r0.h);
        return clampv(need * 0.5f, 0.f, 3.f * s);
    };

    // --- the popover's frame prologue --------------------------------------
    //
    // All of the popover's *decisions* happen up here, before a single widget
    // is drawn, because two of them change how the rest of the panel behaves
    // this frame:
    //
    //   MODALITY. While the list is open the panel underneath must be deaf --
    //   a click meant for a preset row must not also turn a knob that happens
    //   to sit under it. Immediate mode has no z-order for input, so the
    //   shield below parks the pointer off-screen for every widget drawn
    //   between here and the popover block, which processes input against the
    //   restored coordinates. Keyboard modality rides the mechanism the app
    //   already has: ui_.editId gates handleShortcuts() ("typing takes
    //   precedence"), so the popover latches it with its own id while open --
    //   Escape then reaches this panel instead of stopping the transport. If
    //   something else takes the latch (a text field cannot, being shielded,
    //   but belt and braces), the popover concedes and closes.
    const u64 dropId = uiId(UiSpectraPanel, 63, uidKey);
    if (g_pageUid != dm.uid) {                   // a different panel: fresh state
        g_pageUid = dm.uid;
        g_page = 0;
        g_drop.open = false;
        g_assign = SpectraAssign{};
        g_gridDrag = SpectraGrid{};
        g_arpDrag = SpectraArpStep{};
        g_save.open = false;
        g_wt.err.clear();
        if (ui_.editId == dropId) ui_.editId = 0;
        // THE ONE PLACE A DRAWING CAN GO WITHOUT BEING ASKED ABOUT, and it is
        // said out loud rather than hidden. The two USER gestures that end an
        // editor -- leaving the page and closing the panel -- both ask. This
        // branch is not a gesture: the panel has been pointed at a DIFFERENT
        // device (the chain changed under it, or the debug hook moved it), and
        // there is no editor left to ask on behalf of. So the working copy is
        // dropped and the status bar says a drawing was dropped, which is the
        // most this case can honestly offer.
        if (g_edit.open) {
            const bool lost = g_edit.anyDirty();
            g_edit.release();
            if (lost)
                status_ = "Spectra: the panel moved to another device - the "
                          "uncommitted drawing was discarded";
        }
    }
    // A gesture in flight belongs to the surface it started on. The drag ghost
    // is cleared here rather than at the end of the frame it ended on so that
    // nothing below has to remember it might be stale.
    bool tuneDrawn = false, tuneTouched = false;
    const auto openDrop = [&] {
        g_drop.open = true;
        g_drop.justOpened = true;
        g_drop.cursor = -1;                      // resolve to the current preset
        g_drop.scroll = -1.f;
        ui_.editId = dropId;
    };
    const auto closeDrop = [&] {
        g_drop.open = false;
        if (ui_.editId == dropId) ui_.editId = 0;
    };
    if (g_drop.pending) {                        // the debug hook's deferred click
        g_drop.pending = false;
        if (np > 0 && g_page == 0) openDrop();
    }
    if (g_drop.open && (np <= 0 || g_page != 0)) closeDrop();
    if (g_drop.open && ui_.editId != dropId) g_drop.open = false;

    // The shield. RAII so the early returns below cannot leave the pointer
    // parked in Narnia; released explicitly before the popover block.
    struct Shield {
        Input* in; f32 mx, my; bool on;
        Shield(Input* i, bool o) : in(i), mx(i->mx), my(i->my), on(o) {
            if (on) in->mx = in->my = -1e6f;
        }
        void release() { if (on) { in->mx = mx; in->my = my; on = false; } }
        ~Shield() { release(); }
    } shield(&in, g_drop.open);

    // --- the card ----------------------------------------------------------
    // The one surface in the strip that carries the lit edge and an elevation,
    // for the rack panel's reason: it is open, and open is a state worth seeing
    // from across the screen.
    const f32 rad = nx::radiusSm * s;
    rend_.shadow(box, rad, nx::shadow);
    rend_.gradRect(box, rad, nx::glass1);
    rend_.gradStroke(box, rad, s, nx::edgeLit, 1.f);
    rend_.roundRectOutline(box, rad, std::max(1.f, nx::snapPx(s)), nx::violet.alpha(0.55f));

    // --- title -------------------------------------------------------------
    // 20 logical pixels and not 16, and the extra four are spent on ONE
    // control. The page tab lives in this band and it is the most important
    // control in the panel -- it is how half the contract is reached -- and
    // tabPill is the one widget hit slop cannot rescue: it insets the rect it
    // is given by 2 on every side and then hit-tests ONE RECT PER SLOT in a
    // loop, so grab() (consumed by the first setHot after it) would pad MAIN
    // and leave MOD short. Measured at the old 13px band the slots were 12
    // logical pixels tall. A 20px band makes them 16. The two knob rows pay
    // one pixel each and the close button gets to be square while it is here.
    // The proper fix is tabPill hit-testing the UN-inset slot; filed as
    // ui-filed-src-ui-widgets.h.diff.
    Rect title{box.x, box.y, box.w, 16 * s};
    rend_.rect({title.x + 3 * s, title.y + 4 * s, std::max(1.f, nx::snapPx(3 * s)),
                title.h - 8 * s}, tc);

    Rect closeR{title.right() - 18 * s, title.y + 2 * s, 16 * s, 16 * s};
    if (ui_.grab(slop(closeR)).button(uiId(UiSpectraPanel, 0, 0), closeR, "")) {
        // The same obligation the page tab answers, at the other exit. A close
        // with uncommitted frames does not close: it turns to the DRAW page and
        // asks, because the drawing is not in the set and closing loses it.
        if (g_edit.open && g_edit.uid == dm.uid && g_edit.anyDirty()) {
            g_page = 3;
            g_edit.askWhat = 2;
            status_ = "Spectra: the drawing has uncommitted frames - commit it, "
                      "discard it, or stay. Closing would lose it.";
        } else {
            closeDrop();
            if (g_edit.open && g_edit.uid == dm.uid) {
                wtCancelPreview(inst, g_edit.osc);
                if (probeOn())
                    LOGI("NXTAKT_DEBUG_PROBE: spectra editor cancelPreview osc "
                         "%s (panel closed)", g_edit.osc ? "B" : "A");
                g_edit.release();
            }
            spectraOpenUid_ = 0;
            spectraForced_ = false;
        }
    }
    {
        const f32 k = 3.f * s;
        rend_.line(closeR.cx() - k, closeR.cy() - k, closeR.cx() + k, closeR.cy() + k,
                   1.2f * s, nx::muted);
        rend_.line(closeR.cx() - k, closeR.cy() + k, closeR.cx() + k, closeR.cy() - k,
                   1.2f * s, nx::muted);
    }
    if (ui_.hovered(closeR)) ui_.tip = "Close the Spectra panel";

    ui_.microIn(fSmall_, {title.x + 10 * s, title.y, 120 * s, title.h}, "SPECTRA",
                nx::violetSoft, Align::Left, 0);

    // The page tab: §5's one sliding indicator between equal slots. Two faces,
    // because the dock's 200 logical pixels are not negotiable -- see the file
    // comment. Switching pages closes the popover: the chip it hangs from is
    // a MAIN-page control.
    {
        static const char* const kPages[4] = {"MAIN", "MOD", "ARP", "DRAW"};
        // 16 tall, and the slots ARE 16: tabPill's 2px inset is a drawing
        // inset for the sliding indicator and is no longer taken out of the
        // hit test (widgets.cpp). This band paid four pixels to work around
        // that and has them back — in a 200 logical pixel dock, four pixels is
        // a knob row's breathing room.
        //
        // 108 AND NOT 88, WHICH IS THE THIRD PAGE'S FIRST BILL. Three slots in
        // the old 88px band would be 29 logical pixels wide apiece; they would
        // still clear the floor (29 > 16, and the 13px height is what grabTo16
        // pads), but a tab you have to aim at is a tab you stop using. Twenty
        // pixels out of the amber note beside it -- which has 900 and needs 300
        // -- buys 36 a slot, which is close enough to the 44 two tabs had that
        // the hand does not notice the difference.
        //
        // 132 AND NOT 108, WHICH IS THE FOURTH PAGE'S FIRST BILL, and it is
        // v4's bill again at the same rate. Four slots in the 108px band would
        // be 27 logical pixels apiece -- over the 16px floor, and still a tab
        // you have to aim at, which is the reason v4 refused 29. Twenty-four
        // more pixels out of the amber note beside it (which has 872 and needs
        // about 300) buys 33 a slot.
        Rect tabR{title.x + 76 * s, title.y + 1.5f * s, 132 * s, 13 * s};
        int page = g_page;
        // grabTo16 because the band is 13 drawn: tabPill re-arms the slop for
        // every slot, so all FOUR tabs come out at the 16px floor rather than
        // the first one being padded and the rest left short (widgets.cpp says
        // why that asymmetry is worse than uniformly small).
        if (ui_.grabTo16(tabR).tabPill(uiId(UiSpectraPanel, 2, uidKey), tabR, kPages, 4, &page) &&
            page != g_page) {
            // THE ONE PAGE TURN THAT CAN BE REFUSED. The contract makes it an
            // obligation rather than a nicety: a preview is not identity and a
            // project saved mid-edit does not contain the drawing, so "an
            // editor closing with uncommitted changes MUST ask". Leaving the
            // DRAW page is that close, and the ask is drawn by the page it is
            // refusing to leave -- which is why g_page does not move here.
            if (g_page == 3 && g_edit.open && g_edit.anyDirty()) {
                g_edit.askWhat = 1;
                g_edit.askArg  = page;
                status_ = "Spectra: the drawing has uncommitted frames - commit "
                          "it, discard it, or stay. A preview is not saved and a "
                          "project saved now would not contain it.";
            } else {
                closeDrop();
                g_assign = SpectraAssign{};  // a gesture does not survive the turn
                g_arpDrag = SpectraArpStep{};
                g_gridDrag = SpectraGrid{};
                // LEAVING THE PAGE ENDS THE EDITOR, and a standing preview is
                // the EDITOR's, not the set's. Without this a user who
                // previewed, undid back to clean and then turned the page would
                // leave the oscillator playing a table that is in no file, no
                // hash and no state record, with nothing on screen saying so.
                // cancelPreview() is idempotent, so it costs nothing when there
                // was no preview.
                if (g_page == 3 && g_edit.open) {
                    wtCancelPreview(inst, g_edit.osc);
                    if (probeOn())
                        LOGI("NXTAKT_DEBUG_PROBE: spectra editor cancelPreview "
                             "osc %s (page turned)", g_edit.osc ? "B" : "A");
                    g_edit.release();
                }
                g_page = page;
            }
        }
        if (ui_.hovered(tabR))
            ui_.tip = "MAIN is the v1 face; MOD is the parity push - sub & noise, "
                      "warp, LFO 2/3, ENV 3, the matrix, macros, voice mode; ARP "
                      "is v4's arpeggiator and its sixteen-step pattern; DRAW is "
                      "v5's wavetable editor - two pens over 32 frames";
    }

    // §9: say what happened, in amber, in one line. Three states can be on
    // screen and each names itself: forced onto a non-Spectra (the debug
    // hook's doing), or a real Spectra whose DSP predates part of the contract
    // -- in which case every missing id draws as an inert socket below and
    // this line is the reason why.
    {
        // 216 and not 192: the page band grew by 24 for its fourth slot and the
        // note is where the 24 came from. It had 872 logical pixels for a
        // sentence that measures about 300.
        const Rect noteR{title.x + 216 * s, title.y, closeR.x - title.x - 222 * s, title.h};
        char note[128];
        if (!real) {
            snprintf(note, sizeof note, "panel forced onto %s - %d of %d parameters",
                     dm.desc.name.c_str(), pc, (int)pCountV4);
            microFit(ui_, fSmall_, noteR, note, nx::amber.alpha(0.9f), Align::Left, 0);
        } else if (pc < (int)pCountV4) {
            snprintf(note, sizeof note,
                     "DSP has %d of %d parameters - newer controls are inert sockets",
                     pc, (int)pCountV4);
            microFit(ui_, fSmall_, noteR, note, nx::amber.alpha(0.9f), Align::Left, 0);
        }
    }
    rend_.hairlineH(title.x + nx::sp1 * s, title.right() - nx::sp1 * s, title.bottom());

    // --- the column grid ---------------------------------------------------
    //
    // Everything below is on the 8px grid: the column widths, the gaps, the row
    // heights and the pads are all multiples of 4 at half-step and 8 otherwise.
    //
    // A DOCK 200 LOGICAL PIXELS TALL is the constraint the whole layout is cut
    // to, and it is not negotiable from here: detailH_ has no splitter, so the
    // panel gets a 155px box and a hundred-parameter contract to put in it.
    // That is why this is a wide band of seven columns rather than the square
    // face a standalone synth wears, and why the contract's second half lives
    // on a second page rather than a second row -- every section is one
    // column, and the whole thing lands inside the strip's width with the file
    // browser closed (Ctrl+B). Wider than the strip it simply scrolls, like
    // every other device box beside it.
    Rect body{box.x + lay::spectraPad * s, title.bottom() + 3 * s,
              box.w - lay::spectraPad * 2.f * s,
              box.bottom() - title.bottom() - 9 * s};
    if (body.w < 48 * s || body.h < 48 * s) return;

    rend_.pushClip(box);

    const f32 headH = 11 * s;                  // the uppercase micro-label
    // A SELECTOR ROW IS 16 AND NOT 14. Every segmented cluster, every stepper,
    // every matrix selector and every bool toggle in this panel is exactly this
    // tall, so this one number is what put nineteen controls over the usability
    // floor at once -- and it did it by making the rows bigger rather than by
    // padding them, which is the only cure that does not let a segment steal
    // its neighbour's face. It costs one pixel off each of the two knob rows.
    const f32 subH  = 16 * s;                  // a selector / cluster row
    const f32 gap   = 4 * s;
    f32 rowH = (body.h - headH - subH - gap * 3.f) * 0.5f;
    rowH = clampv(rowH, 34 * s, 62 * s);
    const f32 lblH = 11 * s;                   // the knob's own name

    // Seven sections, seven columns — lay::spectraColW, which is also what the
    // device strip reserves the panel's width from. BOTH pages are cut on this
    // one grid: the MOD page reuses the same colX[], and its widest section
    // (the matrix) spans columns 5 and 6 -- so the seam between them, drawn
    // below for every page, falls exactly on the matrix's own middle and the
    // panel's bones stand still when the page turns.
    constexpr int   kCols  = lay::spectraCols;
    const     f32*  kColW  = lay::spectraColW;
    const     f32   colGap = lay::spectraColGap * s;
    f32 colX[kCols];
    {
        f32 x = body.x;
        for (int i = 0; i < kCols; ++i) { colX[i] = x; x += kColW[i] * s + colGap; }
    }
    const auto col = [&](int i) { return Rect{colX[i], body.y, kColW[i] * s, body.h}; };
    // Seams. §11: no solid dividers anywhere -- a hairline that fades at both
    // ends is the only legal one in the system.
    //
    // THE ARP PAGE DRAWS TWO OF THE SIX, and that is a judgment rather than an
    // omission. MOD's matrix spans columns 5 and 6 and KEEPS its seam, because
    // the seam falls on the matrix's own middle and is the divider its two
    // banks of four would otherwise have to draw. The arp's pattern spans FIVE
    // columns and is one control: four hairlines through the middle of a
    // sixteen-step grid would be structure that is not there.
    //
    // THE DRAW PAGE DRAWS ONE OF THE SIX, for the same reason and harder. Its
    // controls are columns 0 and 1 and its canvas is columns 2..6 -- so the
    // seam at column 2 is the one real edge on the page, the seam INSIDE the
    // control block would cut a six-row stack in half, and the four inside the
    // canvas would be hairlines through the middle of a drawing.
    for (int i = 1; i < kCols; ++i) {
        if (g_page == 2 && i > 2) continue;
        if (g_page == 3 && i != 2) continue;
        rend_.hairlineV(colX[i] - colGap * 0.5f, body.y + 2 * s, body.bottom() - 2 * s);
    }

    // --- the parameter access layer ---------------------------------------
    //
    // Three closures, and every control in the panel goes through them. This is
    // where the guard lives, once: nothing below this point indexes the
    // instance directly, so there is no path by which a control can reach a
    // parameter the device does not have.
    const auto has = [&](int id) { return id >= 0 && id < pc; };
    const auto get = [&](int id, f32 fallback) {
        return has(id) ? inst->getParam(id) : fallback;
    };
    const bool ownTrack = ownIsTrack(devOwner_);
    const auto commit = [&](int id, f32 v, u64 gesture, const char* what) {
        if (!has(id)) return;
        // The undo entry is taken BEFORE the model changes, and coalesced on
        // the widget's id -- so one knob drag is one entry, exactly as a device
        // knob and a rack macro already are. The capture is unconditional and
        // only for a track chain, for the reason app_devices.cpp gives: a
        // return's or the master's knob has no clip envelope to record into.
        undoPoint(what);
        inst->setParam(id, v);
        // NXTAKT_DEBUG_PROBE, the other half of the gap writeState() closes.
        // knobNx announces its own writes from inside the widget layer, but a
        // segButton, a stepper and a micro-selector all reach the model through
        // HERE and announced nothing -- so every bool and every enum in this
        // panel was a gesture that could only be asserted from a picture.
        if (probeOn())
            LOGI("NXTAKT_DEBUG_PROBE: spectra param %d (%s) -> %g", id, what,
                 (double)v);
        if (ownTrack)
            autoCapture(addr::deviceParam(ses_.tracks[devOwner_].uid, dm.uid,
                                          inst->paramInfo(id).id), v, gesture);
    };

    // =======================================================================
    // v3's shared machinery: the state string, the mod reach, drag-assign.
    // Everything below the parameter access layer and above the widgets,
    // because every widget in both pages reaches for some of it.
    // =======================================================================

    // How wide an enum the DEVICE actually registered. The filter cluster's
    // rule, hoisted: v3 widens Table (0..7 -> 0..8), LFO Shape (0..4 -> 0..5)
    // and Matrix Source (0..13 -> 0..16), and this panel asks rather than
    // asserts -- so it draws three enums at their v2 width against a v2 DSP and
    // at their v3 width against a v3 one, with no edit here.
    const auto enumMax = [&](int id, int v2Max) {
        return has(id) ? clampv((int)std::lround(inst->paramInfo(id).max), 0, 64)
                       : v2Max;
    };
    const int srcMax = enumMax(pM1Src, 13);

    // THE v4 GUARD, MADE ONCE.
    //
    // `has(id)` is the wrong question for the arp block and the enum comment
    // above says why: 109 and 110 were RESERVED in v3, so a v3 DSP registers
    // them at 0..1 with default 0 -- the same range and the same default the
    // real Arp On and Arp Mode have. A panel that trusted has() would draw a
    // live on/off switch wired to a hidden placeholder, which is the one
    // failure mode worse than drawing nothing: it lies.
    //
    // So the honest test is the COUNT, exactly as it is for the three v3
    // controls whose ids v2 had reserved, and it is made here rather than at
    // fourteen call sites.
    const bool arpLive = pc >= (int)pCountV4;

    // ONE SENTENCE FOR EVERY INERT SOCKET, AND IT NAMES THE RIGHT ABSENCE.
    // "This device has no parameter 109" is FALSE against a v3 DSP -- it has
    // one, it just means nothing -- so a control gated on the count says that
    // instead. Every guarded control in the panel now goes through here, so
    // there is one place where the reason is written down.
    const auto absentWhy = [&](int id, int needPc) {
        char t[288];
        if (needPc && pc < needPc && has(id))
            snprintf(t, sizeof t,
                     "id %d belongs to the v4 arp block (109..124) - this DSP has "
                     "%d of %d parameters. Ids 109 and 110 were RESERVED in v3, so "
                     "they exist here and mean nothing; the count is the honest test.",
                     id, pc, needPc);
        else if (needPc && pc < needPc)
            snprintf(t, sizeof t,
                     "id %d belongs to the v4 arp block (109..124) - this DSP has "
                     "%d of %d parameters", id, pc, needPc);
        else
            snprintf(t, sizeof t, "This device has no parameter %d", id);
        return std::string(t);
    };

    // THE STATE STRING, read once a frame. Read once because stateString()
    // builds a fresh string every call (engine_handle.cpp says so at length),
    // and because a panel that asked twice could get two answers if anything
    // between the two asks wrote.
    SpecState sstate;
    const bool stateOk = sstate.parse(inst->stateString());

    // ...and written only ever from a user edit. The undo point is taken
    // BEFORE the device changes and coalesces on the active widget, so one
    // paint stroke is one entry -- the same contract commit() has for a knob.
    // `gesture` is not decoration. undoCoalesce() falls back to ui_.active when
    // it is not given, and Ui::endFrame() drops `active` on any frame the LEFT
    // button is not held -- which is every frame of a RIGHT-drag. So the erase
    // sweep has to name its own gesture or it takes one undo entry per step it
    // crosses. The paint stroke can leave it 0 because a held left button is
    // exactly what keeps `active` alive.
    const auto writeState = [&](const char* what, u64 gesture = 0) {
        if (!stateOk) return;
        const std::string s = sstate.emit();
        undoPoint(what, gesture);
        if (probeOn())
            LOGI("NXTAKT_DEBUG_PROBE: spectra state \"%s\" -> %s", what,
                 s.empty() ? "(all blocks default)" : s.c_str());
        if (!inst->setStateString(s)) {
            status_ = "Spectra refused that state string - nothing was stored";
        } else if (inst->stateString() != s) {
            // The base class's accept-anything-remember-nothing default. Saying
            // so is the difference between a control that does not work and a
            // control that lies about working.
            status_ = "This DSP keeps no state string - the drawn grid is not stored";
        }
    };

    // THE MOD RING'S NUMBER.
    //
    // What the matrix can do to a destination, AT REST -- and "at rest" is the
    // whole design of it, so it is stated here rather than in the release
    // notes. The reach is computed from PARAMETERS ONLY: the eight slots'
    // source, destination and amount, plus the three LFO shape parameters (a
    // Custom LFO is unipolar and reaches one way; the other five shapes are
    // bipolar and reach both). It never reads a running LFO, an envelope, a
    // held note or anything else the audio thread owns, and there is nothing
    // for it to read -- the panel is on the far side of the PluginInstance
    // boundary and the daemon's process besides.
    //
    // THIS RING MUST NEVER BECOME A METER. The moving value of a modulated
    // control is the knob's own position, which the DSP does not write back and
    // should not; the ring is the REACH, it changes when you turn an amount and
    // at no other time, and the moment it is fed a live signal it becomes a
    // sixty-hertz repaint of the whole panel for an animation nobody asked for.
    // The plan's open question asks for exactly this sentence and here it is.
    struct ModReach { f32 lo = 0.f, hi = 0.f; int slots = 0; };
    const auto shapeOf = [&](int shapeId) {
        return has(shapeId) ? clampv((int)std::lround(get(shapeId, 0.f)), 0, 5) : 0;
    };
    const auto modReach = [&](int d) {
        ModReach mr;
        if (d <= 0 || d >= kDstCount) return mr;
        for (int k = 0; k < 8; ++k) {
            const int sid = pM1Src + 3 * k;
            if (!has(sid) || !has(sid + 1) || !has(sid + 2)) continue;
            const int src = clampv((int)std::lround(get(sid, 0.f)), 0, kSrcCount - 1);
            const int dst = clampv((int)std::lround(get(sid + 1, 0.f)), 0, kDstCount - 1);
            if (src == 0 || dst != d) continue;
            const f32 amt = get(sid + 2, 0.f) * kDestSpan[d];
            if (std::fabs(amt) < 1e-4f) continue;
            bool bip = srcBipolarStatic(src);
            if (src >= 1 && src <= 3) bip = shapeOf(kLfoShapeId[src - 1]) != 5;
            if (bip)                  { mr.lo -= std::fabs(amt); mr.hi += std::fabs(amt); }
            else if (amt >= 0.f)      { mr.hi += amt; }
            else                      { mr.lo += amt; }
            ++mr.slots;
        }
        return mr;
    };
    // Which destination a panel control IS. -1 for every control the matrix
    // cannot reach, which is most of them -- an envelope time, a glide, a
    // unison count -- and that -1 is what a refused drop explains.
    const auto destOfParam = [&](int id) {
        for (int d = 1; d < kDstCount; ++d)
            if (kDestParam[d] == id) return d;
        return -1;
    };

    // THE ASSIGNMENT ITSELF, spelled once because there are now two ways in:
    // the drag (source in hand, looking for a knob) and the control menu's
    // "Assign to matrix slot" (knob in hand, looking for a source). Both end
    // here, so both take the same slot, the same +0.30, the same single undo
    // entry and the same amount mode afterwards -- and neither can drift.
    //
    // Returns the slot it took, or -1 when all eight are in use, which is the
    // one refusal this verb has and which both callers say out loud.
    const auto firstFreeSlot = [&] {
        for (int k = 0; k < 8; ++k) {
            const int sid = pM1Src + 3 * k;
            if (has(sid) && has(sid + 1) && has(sid + 2) &&
                std::lround(get(sid, 0.f)) == 0)
                return k;
        }
        return -1;
    };
    const auto assignSlot = [&](int src, int d, int id, const char* label) {
        const int slot = firstFreeSlot();
        if (slot < 0) {
            status_ = std::string("Spectra: all eight matrix slots are in use - ") +
                      kMatrixSrc[clampv(src, 0, kSrcCount - 1)] + " was not assigned to " +
                      label;
            return -1;
        }
        const int sid = pM1Src + 3 * slot;
        // ONE undo entry for the whole assignment: three setParam calls that
        // are one edit to the user, so they coalesce on one gesture id the way
        // a knob drag's frames do.
        const u64 gest = uiId(UiSpectraPanel, 830 + src, uidKey);
        commit(sid + 0, (f32)src, gest, "assign modulation");
        commit(sid + 1, (f32)d,   gest, "assign modulation");
        commit(sid + 2, 0.30f,    gest, "assign modulation");
        g_assign.tuneSlot  = slot;
        g_assign.tuneDest  = d;
        g_assign.tuneParam = id;
        g_assign.tuneFresh = true;
        char msg[192];
        snprintf(msg, sizeof msg,
                 "Spectra: %s -> %s at +0.30 in M%d - the knob is its amount "
                 "until you click elsewhere", kMatrixSrc[clampv(src, 0, kSrcCount - 1)],
                 kMatrixDst[clampv(d, 0, kDstCount - 1)], slot + 1);
        status_ = msg;
        return slot;
    };

    // DRAG-ASSIGN, the drop half. Called by every control that can be a
    // destination, with the rect the pointer has to be inside.
    //
    // FRAME BY FRAME, and this is the whole interaction:
    //   press on a source handle   g_assign.src is set; nothing is written yet
    //   move                       every modulatable control rings itself in
    //                              violet at 0.35 -- quiet, and only while a
    //                              drag is actually in flight, which is the
    //                              sampler's "accepts samples" rule; the one
    //                              under the pointer takes the lit edge and the
    //                              Add badge and says what it will do
    //   release over a control     the first slot whose source is Off takes
    //                              src -> dest at +0.30, in one undo entry, and
    //                              that control becomes the slot's AMOUNT knob
    //                              until the next click elsewhere
    //   release anywhere else      nothing is written and the status bar says
    //                              which of the two refusals it was
    const auto dropTarget = [&](const Rect& r0, int id, const char* label) {
        if (g_assign.src < 0 || !has(id)) return;
        const int d = destOfParam(id);
        if (d < 0) return;                       // not a destination: no framing
        const f32 rad0 = nx::radiusXs * s;
        if (!r0.contains(in.mx, in.my)) {
            rend_.roundRectOutline(r0, rad0, std::max(1.f, nx::snapPx(s)),
                                   nx::violet.alpha(0.35f));
            return;
        }
        rend_.gradStroke(r0, rad0, s, nx::edgeLit, 1.f);
        // Assign and not Add: the drop creates no control, it wires two that
        // are already on screen.
        ui_.badge = Badge::Assign;
        // The slot this would take, worked out now so the tip can name it.
        const int slot = firstFreeSlot();
        char t[144];
        if (slot < 0)
            snprintf(t, sizeof t,
                     "All eight matrix slots are in use - free one before "
                     "assigning %s to %s", kMatrixSrc[g_assign.src], label);
        else
            snprintf(t, sizeof t, "Assign %s -> %s at +0.30 in slot M%d",
                     kMatrixSrc[g_assign.src], kMatrixDst[d], slot + 1);
        ui_.tip = t;
        if (!in.released[0]) return;

        const int src = g_assign.src;
        g_assign.src = -1;
        assignSlot(src, d, id, label);
    };

    // THE CONTROL MENU'S TWO OPT-IN ITEMS, offered by every knob and trough in
    // the panel that can honestly answer them (uw-WIDGET-API.md §2).
    //
    // ASSIGN runs drag-assign BACKWARDS. The menu is opened on the destination,
    // so the source is the half that is missing, and rather than inventing one
    // the panel arms the same pick the drag already has: every source handle on
    // screen is live, the ghost follows the pointer, Escape cancels, and the
    // next click on a handle completes the wire. One machine, entered from
    // either end.
    //
    // LEARN is the app's own device-parameter MIDI learn -- the identical verb
    // the generic parameter grid puts on a right-click (app_devices.cpp), on the
    // identical address. A synth panel that invented a second MIDI-learn beside
    // it would be a second place for a binding to live.
    const auto menuOfferFor = [&](int id, int dst) {
        Ui::MenuOffer o;
        if (dst > 0) o.items |= Ui::MenuAssign;
        if (ownTrack && has(id)) o.items |= Ui::MenuLearn;
        return o;
    };
    const auto menuHandle = [&](u64 wid, int id, int dst, const char* label) {
        if (ui_.menuFired(wid, Ui::MenuAssign) && dst > 0) {
            g_assign.src = -1;
            g_assign.wantDest  = dst;
            g_assign.wantParam = id;
            snprintf(g_assign.wantLabel, sizeof g_assign.wantLabel, "%s", label);
            char msg[176];
            snprintf(msg, sizeof msg,
                     "Spectra: %s is waiting for a source - click any grip handle "
                     "(the LFOs, ENV 2/3, the macros, wheel/bend/cc, the arp "
                     "pattern). Escape cancels.",
                     kMatrixDst[clampv(dst, 0, kDstCount - 1)]);
            status_ = msg;
        }
        if (ui_.menuFired(wid, Ui::MenuLearn)) {
            if (!ownTrack)
                status_ = "Only a track's devices can be MIDI-mapped - the address "
                          "space has no return or master scope";
            else
                cycleMidiLearn(addr::deviceParam(ses_.tracks[devOwner_].uid, dm.uid,
                                                 inst->paramInfo(id).id));
        }
    };

    // THE SOURCE GRAB HANDLE. Three stacked dashes in a box barely wider than
    // they are -- the grip glyph every list in the program uses for "this is
    // draggable" -- muted at rest, violet under the pointer. It claims no hot
    // rectangle when the device cannot offer the source, so a v2 DSP's panel
    // has three MIDI handles the pointer passes straight through.
    const auto srcHandle = [&](const Rect& hr, int srcVal) {
        const bool live = has(pM1Src) && srcVal <= srcMax;
        const u64 wid = uiId(UiSpectraPanel, 830 + srcVal, uidKey);
        const bool waiting = g_assign.wantDest > 0;
        bool over = false;
        if (live) {
            over = ui_.grab(slop(hr)).setHot(wid, hr) && ui_.isHot(wid);
            // A DESTINATION IS WAITING: the handles are the pick, so they say
            // so before the pointer reaches one. Violet at 0.35 is the framing
            // every modulatable knob wears while a drag is in flight -- the
            // same quiet ring, on the other end of the same gesture.
            if (waiting)
                rend_.roundRectOutline(hr, nx::radiusXs * s,
                                       std::max(1.f, nx::snapPx(s)),
                                       nx::violet.alpha(over ? 0.9f : 0.35f));
            if (over) {
                ui_.cursor = Cursor::Grab;
                char t[176];
                if (waiting) {
                    ui_.badge = Badge::Assign;
                    snprintf(t, sizeof t, "Patch %s -> %s at +0.30 in the first "
                             "free matrix slot", kMatrixSrc[srcVal],
                             kMatrixDst[clampv(g_assign.wantDest, 0, kDstCount - 1)]);
                } else {
                    snprintf(t, sizeof t,
                             "Drag %s onto any knob with a mod ring to patch it "
                             "into the first free matrix slot", kMatrixSrc[srcVal]);
                }
                ui_.tip = t;
                if (in.pressed[0]) {
                    if (waiting) {
                        const int d = g_assign.wantDest, pid = g_assign.wantParam;
                        char lb[24];
                        snprintf(lb, sizeof lb, "%s", g_assign.wantLabel);
                        g_assign.wantDest = g_assign.wantParam = -1;
                        assignSlot(srcVal, d, pid, lb);
                    } else {
                        g_assign.src = srcVal;
                        g_assign.tuneSlot = -1;
                        g_assign.tuneParam = -1;
                    }
                }
            }
        } else if (ui_.hovered(hr)) {
            char t[160];
            snprintf(t, sizeof t,
                     "%s is a %s matrix source - this DSP's source enum stops "
                     "at %s", kMatrixSrc[srcVal],
                     srcVal >= kSrcArpStep ? "v4" : "v3",
                     kMatrixSrc[clampv(srcMax, 0, kSrcCount - 1)]);
            ui_.tip = t;
        }
        const Col ink = !live ? nx::muted.alpha(0.30f)
                      : (g_assign.src == srcVal) ? nx::violetSoft
                      : over ? nx::text
                      : waiting ? nx::muted
                                : nx::muted.alpha(0.70f);
        const f32 x0 = hr.cx() - 3.f * s, x1 = hr.cx() + 3.f * s;
        for (int i = -1; i <= 1; ++i)
            rend_.line(x0, std::round(hr.cy() + (f32)i * 2.5f * s),
                       x1, std::round(hr.cy() + (f32)i * 2.5f * s),
                       std::max(1.f, nx::snapPx(s)), ink);
    };

    // A recessed micro-selector over an integer parameter: click steps forward,
    // the wheel scrubs both ways. The matrix's own control, hoisted to the
    // shared layer in v3 because a second caller appeared -- LFO 1's sync,
    // which has to shrink from a 92px stepper to one cell when the drawn grid
    // takes the row it was standing in. Two call sites, one control.
    //
    // RIGHT-CLICK ERASES, AND THAT IS A BINDING CHANGED ON PURPOSE. It used to
    // step BACKWARDS, which is the idiom Ui::selector keeps and which this
    // control inherited from it. On a MATRIX SLOT that binding is spent on the
    // wrong verb: a slot is a routing, right-click is how a routing is deleted
    // everywhere in FL Studio, and step-backwards already has a home on this
    // very control -- the wheel, which scrubs both directions and is the more
    // natural way to walk a twenty-entry list anyway. So `clearSlot` >= 0 means
    // "this selector belongs to matrix slot k, and a right-click clears the
    // whole slot": source, destination, amount and curve, in ONE undo entry,
    // announced. LFO 1's sync selector passes -1 and keeps step-back, because
    // a sync division is not a routing and there is nothing there to erase.
    //
    // `needPc` is the count gate the v4 block needs and v3's did not: see
    // absentWhy() above. 0 means "has() is the whole question", which is every
    // caller that predates v4.
    const auto enumSel = [&](int sub, const Rect& r0, int id,
                             const char* const* names, int count,
                             const char* what, f32 dim, int clearSlot = -1,
                             int needPc = 0) {
        const bool live = has(id) && count > 0 && (!needPc || pc >= needPc);
        rend_.well(r0, nx::radiusXs * s, false);
        int idx = live ? clampv((int)std::lround(get(id, 0.f)), 0, count - 1) : 0;
        if (live) {
            const u64 wid = uiId(UiSpectraPanel, sub, uidKey);
            if (ui_.grab(slop(r0)).setHot(wid, r0) && ui_.isHot(wid)) {
                ui_.cursor = Cursor::Hand;
                int d = 0;
                if (in.pressed[0]) d = +1;
                if (in.pressed[2] && clearSlot < 0) d = -1;
                if (in.wheel != 0.f) {
                    d = in.wheel > 0.f ? +1 : -1;
                    in.wheel = 0.f;              // not the strip's notch to spend
                }
                if (d) commit(id, (f32)(((idx + d) % count + count) % count), wid, what);
                if (in.pressed[2] && clearSlot >= 0) {
                    const int sid0 = pM1Src + 3 * clearSlot;
                    const int wasSrc = clampv((int)std::lround(get(sid0, 0.f)), 0,
                                              kSrcCount - 1);
                    const int wasDst = has(sid0 + 1)
                        ? clampv((int)std::lround(get(sid0 + 1, 0.f)), 0, kDstCount - 1) : 0;
                    if (wasSrc == 0 && wasDst == 0 &&
                        std::fabs(get(sid0 + 2, 0.f)) < 1e-6f) {
                        char t2[96];
                        snprintf(t2, sizeof t2, "Spectra: M%d is already empty",
                                 clearSlot + 1);
                        status_ = t2;
                    } else {
                        // ONE gesture id for all four writes, so clearing a slot
                        // is one Ctrl+Z and not four.
                        const u64 gest = uiId(UiSpectraPanel, 860 + clearSlot, uidKey);
                        commit(sid0 + 0, 0.f, gest, "clear matrix slot");
                        commit(sid0 + 1, 0.f, gest, "clear matrix slot");
                        commit(sid0 + 2, 0.f, gest, "clear matrix slot");
                        commit(pM1Curve + clearSlot, 0.f, gest, "clear matrix slot");
                        char t2[176];
                        snprintf(t2, sizeof t2,
                                 "Spectra: M%d cleared (was %s -> %s) - Ctrl+Z puts "
                                 "it back", clearSlot + 1, kMatrixSrc[wasSrc],
                                 kMatrixDst[wasDst]);
                        status_ = t2;
                    }
                }
                char t[176];
                if (clearSlot >= 0)
                    snprintf(t, sizeof t, "%s: %s - click next, wheel steps, "
                             "right-click clears M%d", what, names[idx], clearSlot + 1);
                else
                    snprintf(t, sizeof t, "%s: %s - click next, right-click back, "
                             "wheel steps", what, names[idx]);
                ui_.tip = t;
            }
        } else if (ui_.hovered(r0)) {
            ui_.tip = std::string(what) + ": " + absentWhy(id, needPc);
        }
        const Col ink = !live ? nx::muted.alpha(0.40f)
                      : idx == 0 ? nx::muted.alpha(0.55f * dim + 0.30f)
                                 : nx::text.alpha(dim);
        microFit(ui_, fSmall_, r0, live ? names[idx] : "-", ink, Align::Center);
        return idx;
    };

    // A one-segment toggle over a 0/1 parameter, showing the state it is IN
    // rather than the state it would go to -- the noise-track toggle's rule.
    // v3's only caller is the per-LFO mode (ids 100/60/61): Loop is v2's
    // instance-wide generator, One-shot makes the LFO a per-voice envelope.
    const auto modeToggle = [&](int sub, const Rect& r0, int id, const char* what) {
        // L2 Mode (60) and L3 Mode (61) are v2 RESERVED ids spent by v3 at the
        // same range, so `has()` cannot tell a v3 mode from a v2 placeholder.
        // L1 Mode (100) can be told apart and is covered by has() alone; the
        // combined test is written once and is right for all three.
        const bool live = has(id) && pc >= (int)pCountV3;
        const int  m = live ? clampv((int)std::lround(get(id, 0.f)), 0, 1) : 0;
        ui_.segCluster(r0);
        Rect g = r0;
        if (live) {
            const u64 wid = uiId(UiSpectraPanel, sub, uidKey);
            if (ui_.grab(slop(r0)).segButton(wid, r0, m == 1, nx::violet))
                commit(id, m ? 0.f : 1.f, wid, what);
            g = ui_.lastRect;
        }
        microFit(ui_, fSmall_, g, live ? kLfoMode[m] : "-",
                 (live && m ? nx::text : nx::muted).alpha(live ? 0.85f : 0.40f),
                 Align::Center);
        if (ui_.hovered(r0) && g_assign.src < 0)
            ui_.tip = live
                ? "Loop is one instance-wide generator; One-shot gives every "
                  "voice its own, run once from note-on and held - which is "
                  "what makes any LFO an envelope"
                : std::string("This device has no parameter ") + std::to_string(id);
    };

    // A knob cell: the control, its readout (drawn by the widget) and its name
    // underneath. `st` carries the contract's own range, curve and centre.
    //
    // `needPc` is for every control whose id was RESERVED in an earlier
    // revision -- v3's L2 Mode (60), L3 Mode (61) and Bend Range (99), and now
    // v4's Arp On (109) and Arp Mode (110). `has(id)` cannot tell those apart
    // from the real thing: the older DSP registers them, at 0..1 with default 0,
    // exactly as the reserved rule instructs, so the slot is filled and the
    // guard says "present" about a parameter that means nothing. Several cannot
    // even be told apart by their range. The honest test is the one the title
    // bar already makes -- does this DSP have that revision's block at all --
    // so the caller passes the count and the socket is drawn for the right
    // reason. It was a bool through v3 and is a NUMBER now, because there are
    // two revisions to be short of and "true" no longer says which.
    const auto knob = [&](const Rect& cell, int id, const char* label,
                          Ui::KnobStyle st, f32 dim, int needPc = 0) {
        st.dim = dim;
        st.absent = !has(id) || (needPc && pc < needPc);
        // THE UNIT COMES FROM THE DEVICE, not from this file. The contract
        // fixes the range and the curve, so those are spelled out at the call
        // sites -- but "st", "ct", "Hz", "ms", "dB" are the plugin's own
        // metadata, and a panel that hard-coded them would be a second place
        // for them to be wrong. Appended with no space so a five-digit cutoff
        // and its unit still clear a 46px cell.
        char fmtbuf[32];
        if (!st.absent && st.fmt && !st.text) {
            const std::string& u = inst->paramInfo(id).unit;
            if (!u.empty() && u.size() < 5 && u.find('%') == std::string::npos) {
                snprintf(fmtbuf, sizeof fmtbuf, "%s%s", st.fmt, u.c_str());
                st.fmt = fmtbuf;
            }
        }
        f32 v = has(id) ? inst->getParam(id) : st.def;
        const u64 wid = uiId(UiSpectraKnob, id, uidKey);
        const Rect kr{cell.x, cell.y, cell.w, cell.h - lblH};
        const int  dst = st.absent ? -1 : destOfParam(id);

        // AMOUNT MODE -- the third state of drag-assign, and the answer to
        // "opens its amount for immediate drag" in a UI whose gesture ended
        // when the button came up. The knob you just dropped on stops being
        // itself: it SHOWS and EDITS the new slot's depth, ringed and named so
        // it cannot be mistaken for its own value, until you click anywhere
        // else or press Escape. No page turn, no hunting for the slot, and the
        // control under your hand is the one whose depth you want.
        if (g_assign.tuneParam == id && g_assign.tuneSlot >= 0 && !st.absent) {
            tuneDrawn = true;
            if (ui_.hovered(kr)) tuneTouched = true;
            const int aid = pM1Src + 3 * g_assign.tuneSlot + 2;
            Ui::KnobStyle as;
            as.lo = -1.f; as.hi = 1.f; as.def = 0.f; as.bipolar = true;
            as.arc = nx::cyan; as.fmt = "%+.2f"; as.dim = dim; as.absent = !has(aid);
            f32 av = get(aid, 0.f);
            const u64 awid = uiId(UiSpectraKnob, aid, uidKey);
            char what[24];
            snprintf(what, sizeof what, "M%d amount", g_assign.tuneSlot + 1);
            if (ui_.knobNx(awid, kr, &av, as)) commit(aid, av, awid, what);
            rend_.roundRectOutline(cell, nx::radiusXs * s,
                                   std::max(1.f, nx::snapPx(s)), nx::cyan.alpha(0.55f));
            char lb[32];
            snprintf(lb, sizeof lb, "M%d amt", g_assign.tuneSlot + 1);
            microFit(ui_, fSmall_, {cell.x, cell.bottom() - lblH, cell.w, lblH}, lb,
                     nx::cyan.alpha(0.9f), Align::Center);
            if (ui_.hovered(kr)) {
                char t[160];
                snprintf(t, sizeof t,
                         "M%d depth: %s -> %s. Drag to set it; click anywhere "
                         "else (or Escape) and this knob is %s again.",
                         g_assign.tuneSlot + 1,
                         kMatrixSrc[clampv((int)std::lround(
                             get(pM1Src + 3 * g_assign.tuneSlot, 0.f)), 0, kSrcCount - 1)],
                         kMatrixDst[clampv(g_assign.tuneDest, 0, kDstCount - 1)], label);
                ui_.tip = t;
            }
            return;
        }

        // THE CONTROL MENU, OPTED INTO. Right-click is FL Studio's "link to
        // controller" gesture and it is the single most valuable one a synth
        // has; the widget layer draws the sheet and performs Reset and Type-in
        // itself, and this panel adds the two items only it can answer -- the
        // matrix assignment and the device-parameter MIDI learn. Offered per
        // knob rather than blanket, so a control that is not a matrix
        // destination does not grow an Assign item that would refuse.
        // The offer is EMPTY for a socket, and that matters more than it looks.
        // knobNx does not call setHot() when it is absent -- an absent control
        // claims no rectangle at all, by design -- and setHot() is what consumes
        // a pending offer (uw-WIDGET-API 1.3). So an offer handed to a socket
        // would survive onto the NEXT knob in the frame. With has(109) true on a
        // v3 DSP that is not hypothetical: an Assign or Learn item would appear
        // on an unrelated control, offering a verb about a parameter that means
        // nothing.
        if (ui_.offer(st.absent ? Ui::MenuOffer{} : menuOfferFor(id, dst))
               .knobNx(wid, kr, &v, st))
            commit(id, v, wid, label);
        menuHandle(wid, id, dst, label);

        // THE MOD RING. One thin cyan arc outside the value arc, spanning what
        // the matrix can add to this control and subtract from it -- the reach,
        // not the value. See modReach() above for why it is static and must
        // stay static. Cyan because §1 makes cyan the light INSIDE a material:
        // the ring is not a second control, it is this control lit from behind
        // by the patch.
        if (dst > 0 && !st.absent) {
            const ModReach mr = modReach(dst);
            if (mr.slots)
                ui_.knobRing(kr, st, v, mr.lo, mr.hi, nx::cyan.alpha(0.55f * dim));
        }
        dropTarget(kr, id, label);

        microFit(ui_, fSmall_, {cell.x, cell.bottom() - lblH, cell.w, lblH}, label,
                 nx::muted.alpha((st.absent ? 0.40f : 0.85f) * dim), Align::Center);
        // The label is cut short to fit 46 logical pixels, so the tooltip is
        // where the parameter says its whole name, its range and its unit.
        if (ui_.hovered(kr) && g_assign.src < 0) {
            char t[192];
            if (st.absent) {
                ui_.tip = std::string(label) + ": " + absentWhy(id, needPc);
                return;
            } else {
                const ModReach mr = dst > 0 ? modReach(dst) : ModReach{};
                char reach[64] = {};
                if (mr.slots)
                    snprintf(reach, sizeof reach, "  -  mod reach %+.2f..%+.2f (%d slot%s)",
                             (double)mr.lo, (double)mr.hi, mr.slots,
                             mr.slots == 1 ? "" : "s");
                snprintf(t, sizeof t, "%s  %g .. %g %s%s%s  (id %d)%s", label,
                         (double)st.lo, (double)st.hi,
                         inst->paramInfo(id).unit.c_str(), st.log ? " log" : "",
                         st.bipolar ? " bipolar" : "", id, reach);
            }
            ui_.tip = t;
        }
    };

    // A prev / name / next stepper over an integer parameter. Cycles, because
    // tables, divisions and warp modes are rings rather than ranges. When the
    // parameter is absent it draws the same geometry at the disabled weight
    // and claims NO hot rectangle -- the guard note at the top of the file,
    // applied: the v1 stepper let its arrows take hover over a socket, and
    // that was the one place the panel's own doctrine was not being followed.
    const auto stepper = [&](const Rect& r0, int id, int count,
                             const char* const* names, f32 dim,
                             const char* absentText, const char* what,
                             const char* tip, int needPc = 0) {
        ui_.segCluster(r0);
        // 16 and not 14: an arrow is 16 logical px on its short side or it is
        // under the floor, and the name between them can spare the four.
        const f32 bw = 16 * s;
        Rect lb{r0.x, r0.y, bw, r0.h}, rb{r0.right() - bw, r0.y, bw, r0.h};
        rend_.hairlineV(lb.right(), r0.y + 2 * s, r0.bottom() - 2 * s);
        rend_.hairlineV(rb.x, r0.y + 2 * s, r0.bottom() - 2 * s);
        const bool live = has(id) && count > 0 && (!needPc || pc >= needPc);
        int idx = live ? clampv((int)std::lround(get(id, 0.f)), 0, count - 1) : 0;
        const auto step = [&](int d) {
            if (!live) return;
            const int n = ((idx + d) % count + count) % count;
            commit(id, (f32)n, uiId(UiSpectraPanel, 100 + id * 4, uidKey), what);
        };
        // THE WHEEL STEPS THE RING, which is the one gesture this control was
        // missing and which its sibling enumSel has had since v3. A stepper and
        // a micro-selector are the same control at two widths; a wheel that
        // walked the tables here and scrolled the device strip six inches away
        // was the panel's own inconsistency, not the user's mistake. Consumed,
        // for the reason enumSel consumes it: the notch is not the strip's to
        // spend once a control has answered it.
        if (live && ui_.hovered(r0) && in.wheel != 0.f) {
            step(in.wheel > 0.f ? +1 : -1);
            in.wheel = 0.f;
        }
        // Chevrons drawn rather than lettered: at twelve pixels the body font
        // ellipsises even a single character (app_devices.cpp's finding).
        const auto chev = [&](const Rect& b, bool leftward) {
            const f32 k = 2.6f * s, d = leftward ? -1.f : 1.f;
            const Col c = live ? nx::muted : nx::muted.alpha(0.4f);
            rend_.line(b.cx() - k * d * 0.6f, b.cy() - k, b.cx() + k * d * 0.6f, b.cy(),
                       1.1f * s, c);
            rend_.line(b.cx() - k * d * 0.6f, b.cy() + k, b.cx() + k * d * 0.6f, b.cy(),
                       1.1f * s, c);
        };
        if (live) {
            if (ui_.grab(slop(lb)).segButton(uiId(UiSpectraPanel, 100 + id * 4 + 1,
                                             uidKey), lb, false, nx::violet)) step(-1);
            chev(ui_.lastRect, true);
            if (ui_.grab(slop(rb)).segButton(uiId(UiSpectraPanel, 100 + id * 4 + 2,
                                             uidKey), rb, false, nx::violet)) step(+1);
            chev(ui_.lastRect, false);
        } else {
            chev(lb, true);
            chev(rb, false);
        }
        const Rect nameR{lb.right(), r0.y, rb.x - lb.right(), r0.h};
        microFit(ui_, fSmall_, nameR, live ? names[idx] : absentText,
                 (live ? nx::text : nx::muted.alpha(0.45f)).alpha(dim), Align::Center);
        if (tip && ui_.hovered(r0))
            ui_.tip = live ? std::string(tip)
                           : std::string(what) + ": " + absentWhy(id, needPc);
        return idx;
    };

    // A BOOL IN A KNOB CELL. §5: a bit is a toggle and not a knob with two
    // positions -- the noise-track rule, and v4 has three more of them. The
    // geometry is the sync selector's: one 16px control centred in the knob
    // band with its name underneath where a knob's name would be, so a row of
    // toggles lines up with a row of knobs across the seven columns.
    const auto boolCell = [&](const Rect& cell, int sub, int id, const char* label,
                              const char* onText, const char* offText, f32 dim,
                              int needPc, const char* tip) {
        const bool live = has(id) && (!needPc || pc >= needPc);
        const bool on   = live && get(id, 0.f) > 0.5f;
        const Rect tg{cell.x + 2 * s, cell.y + (cell.h - lblH - subH) * 0.5f,
                      std::max(8 * s, cell.w - 4 * s), subH};
        ui_.segCluster(tg);
        Rect g = tg;
        if (live) {
            const u64 wid = uiId(UiSpectraPanel, sub, uidKey);
            if (ui_.grab(slop(tg)).segButton(wid, tg, on, nx::violet))
                commit(id, on ? 0.f : 1.f, wid, label);
            g = ui_.lastRect;
        }
        microFit(ui_, fSmall_, g, live ? (on ? onText : offText) : "-",
                 (on ? nx::text : nx::muted).alpha(live ? dim : 0.40f), Align::Center);
        microFit(ui_, fSmall_, {cell.x, cell.bottom() - lblH, cell.w, lblH}, label,
                 nx::muted.alpha(live ? 0.85f * dim : 0.40f), Align::Center);
        if (ui_.hovered(tg) && g_assign.src < 0)
            ui_.tip = live ? std::string(tip)
                           : std::string(label) + ": " + absentWhy(id, needPc);
        return on;
    };

    // ...and an enum in a knob cell, on the same geometry: enumSel's recessed
    // micro-selector where the cap would be, its name where the knob's name
    // would be. Three values in 46 logical pixels is a selector and not a
    // three-segment cluster -- 15px a segment is under the floor, and the words
    // do not survive it either.
    const auto enumCell = [&](const Rect& cell, int sub, int id,
                              const char* const* names, int count,
                              const char* label, const char* what, f32 dim,
                              int needPc) {
        const bool live = has(id) && (!needPc || pc >= needPc);
        const Rect sr{cell.x + 2 * s, cell.y + (cell.h - lblH - subH) * 0.5f,
                      std::max(8 * s, cell.w - 4 * s), subH};
        const int idx = enumSel(sub, sr, id, names, count, what, dim, -1, needPc);
        microFit(ui_, fSmall_, {cell.x, cell.bottom() - lblH, cell.w, lblH}, label,
                 nx::muted.alpha(live ? 0.85f * dim : 0.40f), Align::Center);
        return idx;
    };

    // A section's uppercase micro-label. §5/§7: 10px, wide tracking, muted.
    const auto sect = [&](const Rect& c, const char* label, f32 dim) {
        ui_.microIn(fSmall_, {c.x, c.y, c.w, headH}, label,
                    nx::muted.alpha(dim), Align::Left, 0);
        return c.y + headH;
    };

    // One envelope row: the curve well and the four time knobs. Shared by
    // ENV1/ENV2 on the MAIN page and ENV3 on the MOD page -- the contract says
    // ENV3 is "the exact shape of ENV2's block", and one lambda is how two
    // pages stay the exact shape. The defaults are arguments because the
    // contract gives ENV3 its own (2/300/0/150 against ENV1/2's 5/200/0.7/300):
    // an absent envelope draws at ITS OWN defaults, not another envelope's.
    // `srcVal` is the matrix source this envelope IS (4 for ENV2, 5 for ENV3),
    // or -1 for ENV1, which the contract hardwires to amplitude and gives no
    // source value at all. It is what puts a grab handle on the two envelopes
    // that can be dragged into a slot and none on the one that cannot.
    const auto envRow = [&](const Rect& c, f32 y, const char* name, int base,
                            f32 dA, f32 dD, f32 dS, f32 dR, int srcVal = -1) {
        const f32 curveW = 48 * s;
        // The curve, drawn from the four values that are actually there --
        // a missing one falls back to its contract default, and the well
        // says so by staying at the disabled weight.
        const Rect cw0{c.x, y, curveW, rowH - lblH};
        rend_.well(cw0, nx::radiusXs * s, true);
        const bool any = has(base) || has(base + 1) || has(base + 2) || has(base + 3);
        const f32 atk = get(base + 0, dA), dec = get(base + 1, dD);
        const f32 sus = clampv(get(base + 2, dS), 0.f, 1.f);
        const f32 rel = get(base + 3, dR);
        if (any) {
            const Rect p = cw0.insetXY(3 * s, 3 * s);
            const int n = clampv((int)(p.w / std::max(1.f, 1.2f * s)), 8, 128);
            f32 px = p.x, py = p.bottom() - envAt(0.f, atk, dec, sus, rel) * p.h;
            for (int i = 1; i <= n; ++i) {
                const f32 u = (f32)i / (f32)n;
                const f32 qx = p.x + p.w * u;
                const f32 qy = p.bottom() - envAt(u, atk, dec, sus, rel) * p.h;
                rend_.line(px, py, qx, qy, 1.2f * s, nx::violetSoft);
                px = qx; py = qy;
            }
        } else {
            rend_.textIn(fSmall_, cw0, "-", nx::muted.alpha(0.4f), Align::Center);
        }
        microFit(ui_, fSmall_, {c.x, y + rowH - lblH, curveW, lblH}, name,
                 nx::muted.alpha(0.85f), Align::Center);
        // The grab handle rides the curve well's top-right corner -- the one
        // corner of an ADSR trace that is always empty, because a released
        // envelope has come down by then.
        if (srcVal >= 0)
            // 12 x 10 and not 11 x 9: with the 3px of slop `slop()` grants a
            // short control that is 18 x 16, and 16 is the floor. The corner it
            // rides is the one corner of an ADSR trace that is always empty, so
            // the extra pixel in each direction costs the drawing nothing.
            srcHandle({cw0.right() - 13 * s, cw0.y + 1 * s, 12 * s, 10 * s}, srcVal);

        const f32 kw = (c.w - curveW) / 4.f;
        Ui::KnobStyle st;
        st.log = true; st.fmt = "%.0f";
        st.lo = 0.1f; st.hi = 5000.f; st.def = dA;
        knob({c.x + curveW, y, kw, rowH}, base + 0, "att", st, 1.f);
        st.lo = 1.f; st.hi = 5000.f; st.def = dD;
        knob({c.x + curveW + kw, y, kw, rowH}, base + 1, "dec", st, 1.f);
        st.log = false; st.lo = 0.f; st.hi = 1.f; st.def = dS; st.fmt = "%.2f";
        knob({c.x + curveW + kw * 2.f, y, kw, rowH}, base + 2, "sus", st, 1.f);
        st.log = true; st.lo = 1.f; st.hi = 8000.f; st.def = dR; st.fmt = "%.0f";
        knob({c.x + curveW + kw * 3.f, y, kw, rowH}, base + 3, "rel", st, 1.f);
    };

    // The five LFO shapes as one cluster of drawn icons, shared by all three
    // LFOs -- the contract says LFO2/3 take "the shape list of id 37,
    // verbatim", so they take the CLUSTER of id 37, verbatim. Lettered names
    // would not fit at 27px a segment and would say less than the shape does.
    //
    // SIX SEGMENTS SINCE v3, and the sixth is the file's guard doctrine applied
    // to an enum rather than to an id. The contract widened LFO Shape 0..4 ->
    // 0..5 (5 = the drawn grid), so the cluster always cuts itself into six and
    // asks paramInfo(id).max how many of them the DEVICE has: against a v2 DSP
    // the Custom segment is an INERT SOCKET at the disabled weight that claims
    // no hot rectangle and says why when hovered, and against a v3 DSP it is a
    // segment like the other five, with no edit here.
    const auto shapeCluster = [&](const Rect& shR, int id, int segBase,
                                  const char* what) {
        ui_.segCluster(shR);
        constexpr int kSeg = 6;
        const f32 sw = shR.w / (f32)kSeg;
        const bool live = has(id);
        const int  top  = live ? clampv(enumMax(id, 4), 0, 5) : 4;
        const int shape = live ? clampv((int)std::lround(get(id, 0.f)), 0, top) : -1;
        for (int k = 0; k < kSeg; ++k) {
            const Rect seg{shR.x + sw * (f32)k, shR.y, sw, shR.h};
            if (k) rend_.hairlineV(seg.x, shR.y + 2 * s, shR.bottom() - 2 * s);
            const bool on = k == shape;
            const bool segLive = live && k <= top;
            Rect g = seg;
            if (segLive) {
                const u64 wid = uiId(UiSpectraPanel, segBase + k, uidKey);
                if (ui_.grab(slop(seg)).segButton(wid, seg, on, nx::violet))
                    commit(id, (f32)k, wid, what);
                g = ui_.lastRect;
            } else if (ui_.hovered(seg) && k == 5) {
                ui_.tip = live
                    ? "Custom is v3's drawn 16-step grid - this DSP's LFO shape "
                      "enum stops at S&H"
                    : std::string("This device has no parameter ") + std::to_string(id);
            }
            const Col ic = on ? nx::text : nx::muted.alpha(segLive ? 0.85f : 0.40f);
            const f32 w2 = 5.f * s, h2 = 3.2f * s, th = 1.1f * s;
            const f32 gx = g.cx(), gy = g.cy();
            switch (k) {
            case 0: {                                   // sine
                f32 px = gx - w2, py = gy;
                for (int i = 1; i <= 8; ++i) {
                    const f32 u = (f32)i / 8.f;
                    const f32 qx = gx - w2 + 2.f * w2 * u;
                    const f32 qy = gy - std::sin(kTwoPi * u) * h2;
                    rend_.line(px, py, qx, qy, th, ic);
                    px = qx; py = qy;
                }
                break;
            }
            case 1:                                     // triangle
                rend_.line(gx - w2, gy + h2, gx, gy - h2, th, ic);
                rend_.line(gx, gy - h2, gx + w2, gy + h2, th, ic);
                break;
            case 2:                                     // saw
                rend_.line(gx - w2, gy + h2, gx + w2, gy - h2, th, ic);
                rend_.line(gx + w2, gy - h2, gx + w2, gy + h2, th, ic);
                break;
            case 3:                                     // square
                rend_.line(gx - w2, gy + h2, gx - w2, gy - h2, th, ic);
                rend_.line(gx - w2, gy - h2, gx, gy - h2, th, ic);
                rend_.line(gx, gy - h2, gx, gy + h2, th, ic);
                rend_.line(gx, gy + h2, gx + w2, gy + h2, th, ic);
                rend_.line(gx + w2, gy + h2, gx + w2, gy - h2, th, ic);
                break;
            case 4:                                     // sample & hold
                rend_.line(gx - w2, gy + h2, gx - w2 * 0.33f, gy + h2, th, ic);
                rend_.line(gx - w2 * 0.33f, gy - h2, gx + w2 * 0.33f, gy - h2, th, ic);
                rend_.line(gx + w2 * 0.33f, gy + h2 * 0.3f, gx + w2, gy + h2 * 0.3f, th, ic);
                break;
            default: {                                  // custom: a drawn staircase
                const f32 lv[4] = {0.9f, 0.1f, 0.6f, -0.7f};
                const f32 bw = w2 * 0.5f;
                for (int i = 0; i < 4; ++i) {
                    const f32 bx = gx - w2 + bw * (f32)i;
                    rend_.line(bx, gy - h2 * lv[i], bx + bw * 0.8f, gy - h2 * lv[i],
                               th, ic);
                }
                break;
            }
            }
        }
        if (ui_.hovered(shR) && g_assign.src < 0)
            ui_.tip = live ? std::string(what) + ": " +
                             (shape >= 0 ? kShapeName[shape] : "?")
                           : std::string("This device has no parameter ") +
                             std::to_string(id);
        return shape;
    };

    // =======================================================================
    // THE x0x STROKE, SPELLED ONCE.
    //
    // There are FOUR drawable level rows in this panel since v4 -- LFO 1/2/3's
    // grids and the arp's -- and the brief for the fourth was "identical in
    // feel to the LFO grid you already ship". A second copy of fifty lines is
    // how two things stop being identical, quietly, three months later. So the
    // stroke lives here and the four call sites differ only in which sixteen
    // integers they hand it and what their tooltip says.
    //
    // THE GESTURES, and they are the three a step sequencer has had since the
    // Roland x0x, plus the one the FL pass added:
    //   drag        paints the level under the pointer into the step under it,
    //               filling every step the stroke flew over
    //   shift-drag  interpolates from where the press landed to where the
    //               pointer is now, so a ramp is one stroke and not sixteen
    //   right-drag  clears every step the stroke crosses
    //   wheel       nudges the step under the pointer by one digit, CONSUMED
    //
    // THE WHEEL IS NEW AND IT IS NOT DECORATION. uw-WIDGET-API §0 made the
    // wheel adjust every control in the program and consume the notch; a
    // drawable row is a control, and the arp's is EIGHT HUNDRED logical pixels
    // wide. A notch that fell through it would scroll the device strip out from
    // under a hand that never moved, and the next notch -- aimed at the same
    // step -- would land somewhere else. That is the coexistence rule's exact
    // failure, at the largest target in the panel. The three LFO grids get it
    // too, because they are the same control.
    //
    // Level is quantised to the contract's sixteen digits on the way in, so
    // what is on screen is exactly what is in the string -- there is no finer
    // value being kept behind the bar you can see.
    struct LevelHit { bool hot = false; int step = 0; int level = 0; };
    const auto levelRowGesture = [&](int* row, const Rect& gr, const Rect& p,
                                     u64 wid, int slot, bool editable,
                                     const char* drawWhat, const char* eraseWhat) {
        LevelHit h;
        const f32 cw16 = p.w / (f32)kGridSteps;
        const auto stepAt = [&](f32 x) {
            return clampv((int)std::floor((x - p.x) / std::max(1e-3f, cw16)),
                          0, kGridSteps - 1);
        };
        const auto levelAt = [&](f32 y) {
            return clampv((int)std::lround((1.f - (y - p.y) / std::max(1e-3f, p.h)) *
                                           (f32)(kGridLevels - 1)), 0, kGridLevels - 1);
        };
        h.step  = stepAt(in.mx);
        h.level = row[h.step];

        // A CLICK IS A STROKE OF ONE STEP, and it has to be handled as one:
        // a fast click delivers its press and its release in the SAME frame
        // (the popover learned this the hard way -- see SpectraDrop::justOpened)
        // and a paint loop that tested `in.down[0]` first would drop it on the
        // floor, so a step you clicked would stay where it was.
        bool pressedNow = false, erasedNow = false;
        if (editable && ui_.setHot(wid, gr) && ui_.isHot(wid)) {
            h.hot = true;
            ui_.cursor = Cursor::Hand;
            // Draw at rest, Delete with the right button down: the badge is the
            // answer to "what will a click do here?", and while the erase
            // stroke is in flight the answer is not "draw".
            ui_.badge = (in.down[2] || in.pressed[2]) ? Badge::Delete : Badge::Draw;
            if (in.pressed[2]) {
                g_gridDrag.erase = slot;
                g_gridDrag.eraseStep = h.step;
                erasedNow = true;
            }
            if (in.pressed[0]) {
                ui_.active = wid;                // so one stroke is one undo entry
                g_gridDrag.lfo = slot;
                g_gridDrag.startStep  = h.step;
                g_gridDrag.startLevel = levelAt(in.my);
                pressedNow = true;
            }
            // The wheel, and it names its own gesture id: `active` is dropped on
            // any frame the left button is not held, so a wheel nudge that
            // leaned on it would take one undo entry per notch.
            if (in.wheel != 0.f && g_gridDrag.lfo < 0) {
                const int lv = clampv(row[h.step] + (in.wheel > 0.f ? +1 : -1), 0, 15);
                in.wheel = 0.f;
                if (lv != row[h.step]) {
                    row[h.step] = lv;
                    h.level = lv;
                    writeState(drawWhat, wid);
                }
            }
        }

        // THE ERASE SWEEP. FL Studio's right-drag over a step sequencer clears
        // every step the stroke flies over, and a right-click that cleared only
        // the step it happened to land on was this grid's half of that gesture:
        // it worked, and it made clearing sixteen steps sixteen aimed clicks.
        // One stroke now, span-filled between frames the same way the paint
        // stroke is, and coalesced on the same widget id -- so an erase sweep
        // is ONE undo entry however many steps it crossed.
        if (g_gridDrag.erase == slot) {
            if (in.down[2] || erasedNow) {
                const int s1 = stepAt(in.mx);
                bool changed = false;
                for (int i = std::min(g_gridDrag.eraseStep, s1);
                     i <= std::max(g_gridDrag.eraseStep, s1); ++i)
                    if (row[i]) { row[i] = 0; changed = true; }
                g_gridDrag.eraseStep = s1;
                if (changed) writeState(eraseWhat, wid);
            }
            if (!in.down[2]) g_gridDrag.erase = -1;
        }
        if (g_gridDrag.lfo == slot) {
            if (in.down[0] || pressedNow) {
                const int s1 = stepAt(in.mx), l1 = levelAt(in.my);
                bool changed = false;
                if (in.shift()) {
                    const int a = g_gridDrag.startStep, b = s1;
                    const int la = g_gridDrag.startLevel, lb = l1;
                    for (int i = std::min(a, b); i <= std::max(a, b); ++i) {
                        const f32 u = a == b ? 1.f : (f32)(i - a) / (f32)(b - a);
                        const int lv = clampv((int)std::lround((f32)la +
                                              ((f32)lb - (f32)la) * u), 0, 15);
                        if (row[i] != lv) { row[i] = lv; changed = true; }
                    }
                } else {
                    // A FREE STROKE FILLS THE STEPS IT FLEW OVER. The pointer
                    // is sampled once a frame and a hand moves faster than
                    // that, so painting only the step under the cursor leaves
                    // holes in a quick sweep -- and a sequencer that drops
                    // steps when you draw fast is a sequencer you stop drawing
                    // on. The span from the last sample to this one is filled
                    // with the same interpolation shift-drag uses; a stroke
                    // that stayed in one step is the one-step case of it.
                    const int a  = clampv(g_gridDrag.startStep, 0, kGridSteps - 1);
                    const int la = clampv(g_gridDrag.startLevel, 0, 15);
                    for (int i = std::min(a, s1); i <= std::max(a, s1); ++i) {
                        const f32 u = a == s1 ? 1.f : (f32)(i - a) / (f32)(s1 - a);
                        const int lv = clampv((int)std::lround((f32)la +
                                              ((f32)l1 - (f32)la) * u), 0, 15);
                        if (row[i] != lv) { row[i] = lv; changed = true; }
                    }
                    // ...and the next frame's span starts where this one ended.
                    // (Shift-drag deliberately does NOT do this: a line has one
                    // anchor, and moving it would make the line follow the hand
                    // instead of standing where it was started.)
                    g_gridDrag.startStep  = s1;
                    g_gridDrag.startLevel = l1;
                }
                if (changed) writeState(drawWhat);
            }
            if (!in.down[0]) g_gridDrag.lfo = -1;
        }
        h.level = row[h.step];
        return h;
    };

    // A DRAWABLE LEVEL ROW'S BARS. The other half of the same control, also
    // shared: sixteen bars growing from the floor, a hairline on every fourth
    // seam so the row reads as four beats of four without a number on screen,
    // and §1's one lamp on the TOP of each bar -- the edge that moves, so light
    // rides the value here as it does on the knobs.
    //
    // `beyondFrom` is v4's addition and costs the LFO grids nothing (they pass
    // 16): the arp has a pattern LENGTH, and steps past it are still stored --
    // the contract truncates the READ and never the storage -- so they are
    // drawn, faintly, rather than blanked. A grid that hid the steps it was not
    // playing would make shortening a pattern look like deleting one.
    const auto levelRowBars = [&](const int* row, const Rect& gr, const Rect& p,
                                  bool editable, int beyondFrom) {
        const f32 cw16 = p.w / (f32)kGridSteps;
        for (int i = 1; i < kGridSteps; ++i)
            if (i % 4 == 0)
                rend_.hairlineV(std::round(p.x + cw16 * (f32)i), gr.y + 2 * s,
                                gr.bottom() - 2 * s);
        for (int i = 0; i < kGridSteps; ++i) {
            const f32 a = (i >= beyondFrom) ? 0.30f : 1.f;
            const int d = clampv(row[i], 0, 15);
            const f32 bx = p.x + cw16 * (f32)i + 0.5f * s;
            const f32 bw = std::max(1.f, cw16 - 1.f * s);
            const f32 bh = p.h * (f32)d / 15.f;
            if (bh <= 0.5f) {
                rend_.rect({bx, p.bottom() - std::max(1.f, nx::snapPx(s)), bw,
                            std::max(1.f, nx::snapPx(s))}, nx::line.alpha(0.55f * a));
                continue;
            }
            const Rect bar{bx, p.bottom() - bh, bw, bh};
            rend_.rect(bar, nx::violet.alpha((editable ? 0.55f : 0.30f) * a));
            rend_.rect({bar.x, bar.y, bar.w, std::max(1.f, nx::snapPx(s))},
                       nx::cyan.alpha((editable ? 0.75f : 0.30f) * a));
        }
    };

    // =======================================================================
    // THE DRAWN LFO -- a 16-step grid and its smooth, and the one control in
    // this panel that is NOT a parameter.
    //
    // The contract puts the grid in the state string rather than in 51 ids, and
    // says what that costs in the same breath: a drawn grid cannot be automated
    // and cannot be a matrix destination. It is SHAPE, like a table is shape.
    // So this block reads and writes through stateString()/setStateString(),
    // carrying every record it does not understand across untouched -- see
    // SpecState for why that half is the important half.
    //
    // THE GESTURES are levelRowGesture()'s, above -- this block is the well,
    // the smooth trough and the sentence, and nothing else.
    //
    // WHERE IT SITS. A grid wants two of a column's three cells and a row of
    // its own, which LFO 2 and LFO 3 have free (the contract gives them no
    // fixed routings, so their depth cells are empty) and LFO 1 does not. LFO
    // 1's column therefore re-cuts when Custom is chosen -- see the call site,
    // which is where the cost of that is written down.
    const auto lfoGridBlock = [&](const Rect& blk, int n) {
        const f32 smH = 13 * s;
        const Rect gr{blk.x, blk.y, blk.w, std::max(14 * s, blk.h - smH - 2 * s)};
        rend_.well(gr, nx::radiusXs * s, true);
        const Rect p = gr.insetXY(2 * s, 2 * s);
        const bool editable = stateOk;
        const u64 wid = uiId(UiSpectraPanel, 810 + n, uidKey);
        const LevelHit lh = levelRowGesture(sstate.grid[n], gr, p, wid, n, editable,
                                            "draw LFO steps", "erase LFO steps");
        if (lh.hot) {
            char t[192];
            snprintf(t, sizeof t,
                     "LFO %d steps: drag to paint, shift-drag for a line, "
                     "right-DRAG to erase, wheel nudges. Step %d of 16, level "
                     "%.2f - the whole cycle is the sync division.",
                     n + 1, lh.step + 1, (double)lh.level / 15.0);
            ui_.tip = t;
        }
        levelRowBars(sstate.grid[n], gr, p, editable, kGridSteps);
        if (!editable && ui_.hovered(gr))
            ui_.tip = "This device's state string does not parse - the grid is "
                      "read-only rather than overwrite what it could not read";

        // The smooth control, in the hero's own label / trough / value idiom.
        const Rect smr{blk.x, blk.bottom() - smH, blk.w, smH};
        // The smooth row's label is a GLYPH and not the word, for the reason the
        // filter and shape clusters carry glyphs: the row is 90 logical pixels
        // wide and "smooth" is thirty-eight of them, which would leave a 0..1
        // control twenty-six pixels to be dragged in. So the label is what the
        // control DOES -- a step with its corner rounded off -- and the tooltip
        // is the sentence.
        const Rect lr{smr.x, smr.y, 13 * s, smH};
        const Rect vr{smr.right() - 24 * s, smr.y, 24 * s, smH};
        // The trough takes the WHOLE row band rather than a 9px ribbon inside
        // it. A 13px row with 2px of air above and below left a 9-logical-pixel
        // control to aim at, five under the floor and beyond what hit slop is
        // allowed to make up; the air is what it was spending, so the air is
        // what it gets back. 13 tall, plus the 1.5px of slop `slop()` grants a
        // control that is three short, is 16.
        const Rect tr{lr.right() + 2 * s, smr.y,
                      std::max(4 * s, vr.x - lr.right() - 6 * s), smH};
        {
            const Col gc = nx::muted.alpha(editable ? 0.80f : 0.35f);
            const f32 gt = std::max(1.f, nx::snapPx(s));
            const f32 gx = lr.x + 1 * s, gy = lr.cy();
            rend_.line(gx, gy + 2.5f * s, gx + 3.5f * s, gy + 2.5f * s, gt, gc);
            rend_.line(gx + 3.5f * s, gy + 2.5f * s, gx + 5.5f * s, gy + 0.5f * s, gt, gc);
            rend_.line(gx + 5.5f * s, gy + 0.5f * s, gx + 7.f * s, gy - 2.5f * s, gt, gc);
            rend_.line(gx + 7.f * s, gy - 2.5f * s, gx + 10.5f * s, gy - 2.5f * s, gt, gc);
        }
        f32 sv = (f32)clampv(sstate.smooth[n], 0, 1000) / 1000.f;
        if (editable) {
            const u64 swid = uiId(UiSpectraPanel, 820 + n, uidKey);
            // The trailing 0.f is the DEFAULT, and it is what makes a
            // double-click and the control menu's Reset work here: a smooth of
            // zero is the hard staircase this control starts life at. Without
            // it the widget layer has nothing to reset to and says so instead
            // of doing nothing, which is honest but useless on a control whose
            // default is obvious.
            if (ui_.grab(slop(tr)).trough(swid, tr, &sv, 0.f, 1.f, nx::violetSoft,
                                          1.f, 0.f)) {
                sstate.smooth[n] = clampv((int)std::lround(sv * 1000.f), 0, 1000);
                writeState("LFO smooth", swid);
            }
            if (ui_.hovered(tr))
                ui_.tip = "Step smoothing: 0 is a hard staircase (a selected "
                          "branch, bit-exact); above it a one-pole lag toward "
                          "each step, stored as thousandths. Wheel steps it, "
                          "double-click is back to 0.";
        } else {
            rend_.well(tr, nx::radiusXs * s, true);
        }
        char sb[16];
        snprintf(sb, sizeof sb, "%.2f", (double)sv);
        rend_.textIn(fSmall_, vr, editable ? sb : "-",
                     nx::muted.alpha(editable ? 1.f : 0.4f), Align::Right, 0);
    };

    // --- shared state several sections read --------------------------------
    const f32 bLevel = get(pBLevel, 0.f);
    // §5's disabled rule: an osc contributing nothing is not doing anything,
    // and it says so at 40% rather than being greyed or hidden.
    const f32 dimB = (has(pBLevel) && bLevel > 1e-4f) ? 1.f : 0.4f;
    // Voice mode, read once for both pages: MOD draws its cluster, MAIN dims
    // the voices knob when the mode ignores it (contract: modes 1-2 do).
    const int vmode = has(pVoiceMode)
                    ? clampv((int)std::lround(get(pVoiceMode, 0.f)), 0, 2) : 0;

    // Where the preset row landed this frame, for the popover to hang from.
    Rect presetRowR{};

    if (g_page == 0) {
    // =======================================================================
    // MAIN -- the v1 face
    // =======================================================================

    // =======================================================================
    // 1. THE WAVETABLE DISPLAY -- the hero
    // =======================================================================
    {
        const Rect c = col(0);
        const f32 y0 = sect(c, "WAVETABLE", 1.f);
        const f32 slidH = 13 * s;
        // v3 gave the hero column a selector band, and it takes the SAME band
        // every other column's selector row sits in -- rows that line up across
        // seven columns are most of what makes a panel read as built, and the
        // hero was the one column that had nothing in that band. The well pays
        // for it out of its own height, which is the only budget there is.
        const Rect impR{c.x, y0, c.w, subH};
        const Rect dispR{c.x, y0 + subH + gap, c.w,
                         std::max(24 * s, c.bottom() - (y0 + subH + gap) -
                                          (slidH * 2.f + 8 * s))};

        // =====================================================================
        // THE IMPORT -- pillar 1's whole surface in this file.
        //
        // THE DROP TARGET IS THE HERO ITSELF, and the language is the sampler's
        // word for word: the lit edge arrives when a drag is over the target,
        // the Add badge says what a release will do, and the INVITATION IS
        // QUIET AND CONDITIONAL -- there is no chip shouting "DROP A WAVETABLE
        // HERE" at rest, because the owner was right about the first cut of the
        // sampler's ("looks goofy and unprofessional") and this is not the
        // place to re-loudify it. At rest the row says which table each
        // oscillator is on. While a browser drag is in flight it says whether
        // it will take it. That is all.
        //
        // WHICH OSCILLATOR gets the file is a visible choice and not a guess:
        // the A / B pair at the left of the row is the target, it is UI state
        // (not a parameter -- "which am I loading" is a question about the
        // pointer), and the badge's tooltip names it before the button comes
        // up. The alternative -- inferring the target from which half of the
        // well the pointer is over -- would be a rule nobody can see.
        // =====================================================================
        const bool wtLive  = wtSupported(inst);
        const int  wtOsc   = clampv(g_wt.osc, 0, 1);
        const int  wtTblId = wtOsc ? pBTable : pATable;
        const bool wtCustom = wtLive && wtHasCustom(inst, wtOsc);
        const Rect wtDropR{c.x, impR.y, c.w, dispR.bottom() - impR.y};

        // REVERT TO FACTORY, spelled once and reached two ways: the ✕ chip at
        // the end of the import row, and a RIGHT-CLICK anywhere on the hero.
        // The chip is the discoverable route and the right-click is the one a
        // hand already knows -- in FL Studio the button that loads a slot and
        // the button that empties it are the two buttons on the mouse, and a
        // wavetable this panel imported by dropping is a wavetable it should
        // drop by right-clicking. Both take one undo point and both say what
        // happened; the refusal (there is nothing imported to revert) says so
        // too rather than being a click that did nothing.
        const auto revertWt = [&] {
            if (!wtLive) {
                status_ = "Spectra: this build's plugin contract has no "
                          "wavetable() - there is no import to revert";
                return;
            }
            if (!wtCustom) {
                char m[160];
                snprintf(m, sizeof m,
                         "Spectra: osc %s is already on factory table %s - "
                         "there is nothing to revert", wtOsc ? "B" : "A",
                         kTables[clampv(has(wtTblId)
                             ? (int)std::lround(get(wtTblId, 0.f)) : 0, 0, 7)]);
                status_ = m;
                return;
            }
            undoPoint("clear custom wavetable");
            wtClear(inst, wtOsc);
            g_wt.err.clear();
            if (has(wtTblId) && std::lround(get(wtTblId, 0.f)) >= 8) {
                inst->setParam(wtTblId, 0.f);
                if (ownTrack)
                    autoCapture(addr::deviceParam(ses_.tracks[devOwner_].uid,
                                dm.uid, inst->paramInfo(wtTblId).id), 0.f, 0);
            }
            status_ = std::string("Spectra: osc ") + (wtOsc ? "B" : "A") +
                      " back on factory table " + kTables[0] + " - Ctrl+Z puts "
                      "the import back";
        };

        // NXTAKT_DEBUG_SPECTRAWTDRAG, re-armed here and not once in the seed:
        // app_session.cpp clears any drag the mouse is not holding, which is
        // right for a real drag and leaves a headless one alive for one frame.
        if (!g_wt.dragHold.empty()) {
            drag_.kind  = DragState::Kind::BrowserFile;
            drag_.path  = g_wt.dragHold;
            drag_.armed = true;
        }
        const bool fileDrag  = drag_.kind == DragState::Kind::BrowserFile && drag_.armed;
        const bool dragWav   = fileDrag && looksLikeWav(drag_.path);
        const bool dragHere  = fileDrag && wtDropR.contains(in.mx, in.my);
        const bool dropReady = dragHere && dragWav && wtLive;

        if (dragHere) {
            rend_.gradStroke(wtDropR, nx::radiusSm * s, s,
                             dropReady ? nx::edgeLit : nx::edge, 1.f);
            if (dropReady) {
                ui_.badge = Badge::Add;
                char t[176];
                snprintf(t, sizeof t, "Drop to import %s into osc %s",
                         baseNameOf(drag_.path.c_str()), wtOsc ? "B" : "A");
                ui_.tip = t;
            } else if (!wtLive) {
                ui_.tip = "This device has no wavetable import - the panel's "
                          "PluginInstance does not answer wavetable()";
            } else {
                ui_.tip = "A wavetable is imported from a .wav";
            }
            if (in.released[0] && dropReady) {
                // ONE undo entry for the whole verb. The Table parameter is
                // written below WITHOUT commit(), which would take a second
                // point for what is one edit to the user; everything else about
                // the write is commit()'s body, line for line.
                undoPoint("import wavetable");
                if (wtImport(inst, wtOsc, drag_.path.c_str())) {
                    g_wt.err.clear();
                    // An import that did not move the oscillator onto its
                    // custom slot would be a silent no-op with a filename
                    // attached, so the drop DOES what dropping means.
                    if (has(wtTblId) && enumMax(wtTblId, 7) >= 8) {
                        inst->setParam(wtTblId, 8.f);
                        if (ownTrack)
                            autoCapture(addr::deviceParam(ses_.tracks[devOwner_].uid,
                                        dm.uid, inst->paramInfo(wtTblId).id), 8.f, 0);
                    }
                    char msg[224];
                    snprintf(msg, sizeof msg, "Spectra: osc %s plays %s (%d frames)",
                             wtOsc ? "B" : "A", wtName(inst, wtOsc),
                             wtFrames(inst, wtOsc));
                    status_ = msg;
                } else {
                    // §9, and the refusal idiom: the importer's own sentence,
                    // never a silent no-op. It stays on the row in amber until
                    // the next import or the next target change.
                    g_wt.err = wtError(inst);
                    status_ = std::string("Spectra: ") + g_wt.err;
                }
                drag_ = DragState{};
                // A DROPPED DRAG IS OVER, including a headless one. The hook
                // re-arms `drag_` from dragHold on every frame -- which is what
                // keeps a mouseless drag alive long enough to be photographed
                // -- and nothing used to spend it, so after a scripted drop the
                // panel believed a browser drag was still in flight forever.
                // That is not what a real drag does, and it made the erase
                // gesture below unreachable under the harness: every right-click
                // landed on a frame the panel thought was mid-drag. Clearing it
                // here is the hook modelling the mouse it stands in for.
                g_wt.dragHold.clear();
            }
        }

        // ...and the erase half of the same pair. It is tested BEFORE the row's
        // own widgets are drawn, so a right-click on the ✕ chip or the A / B
        // pair means the hero and not the chip under the pointer: this gesture
        // is about the imported table, and every pixel of the hero says the
        // same thing about it.
        // No badge for it, and that is widgets.h's rule rather than an
        // omission: a badge appears only where the answer is not otherwise
        // available, and a Delete glyph parked over the wavetable trace every
        // time the pointer crossed it would be shouting at the one surface in
        // this panel whose whole job is to be looked at. The sentence lives in
        // the tooltips below, where it is read once and asked for.
        if (!dragHere && in.pressed[2] && wtDropR.contains(in.mx, in.my))
            revertWt();

        {
            ui_.segCluster(impR);
            const f32 tgW = 16 * s;
            const f32 clrW = wtCustom ? 16 * s : 0.f;
            const Rect abR{impR.x, impR.y, tgW * 2.f, impR.h};
            const Rect brR{impR.right() - 19 * s - clrW, impR.y, 19 * s, impR.h};
            const Rect clR{impR.right() - clrW, impR.y, std::max(0.f, clrW), impR.h};
            const Rect nmR{abR.right() + 2 * s, impR.y,
                           std::max(4 * s, brR.x - abR.right() - 4 * s), impR.h};
            rend_.hairlineV(abR.right(), impR.y + 2 * s, impR.bottom() - 2 * s);
            rend_.hairlineV(brR.x, impR.y + 2 * s, impR.bottom() - 2 * s);

            for (int k = 0; k < 2; ++k) {
                const Rect seg{abR.x + tgW * (f32)k, abR.y, tgW, abR.h};
                if (k) rend_.hairlineV(seg.x, abR.y + 2 * s, abR.bottom() - 2 * s);
                Rect g = seg;
                if (wtLive) {
                    const u64 wid = uiId(UiSpectraPanel, 801 + k, uidKey);
                    if (ui_.grab(slop(seg)).segButton(wid, seg, k == wtOsc, nx::violet)) {
                        g_wt.osc = k;
                        g_wt.err.clear();
                    }
                    g = ui_.lastRect;
                }
                ui_.microIn(fSmall_, g, k ? "B" : "A",
                            k == wtOsc && wtLive ? nx::text
                                                 : nx::muted.alpha(wtLive ? 0.85f : 0.40f),
                            Align::Center);
            }

            // THE NAME. A custom table is named by its basename, which is the
            // only thing about it a person recognises; a factory slot is named
            // by the contract's own label. An import that was refused replaces
            // both with its reason, in amber, until it is answered.
            const int  tblIdx = has(wtTblId)
                ? clampv((int)std::lround(get(wtTblId, 0.f)), 0, 8) : 0;
            const char* nm; Col nink;
            if (!g_wt.err.empty()) {
                nm = g_wt.err.c_str(); nink = nx::amber.alpha(0.95f);
            } else if (!wtLive) {
                // Short, because the row is 144px and the sentence is 60 -- the
                // tooltip is where the whole of it lives, which is this file's
                // rule for every label cut to a cell.
                nm = "no import"; nink = nx::muted.alpha(0.45f);
            } else if (wtCustom) {
                nm = wtName(inst, wtOsc);
                nink = tblIdx >= 8 ? nx::cyan.alpha(0.95f) : nx::muted.alpha(0.75f);
            } else {
                nm = kTables[clampv(tblIdx, 0, 7)]; nink = nx::muted.alpha(0.80f);
            }
            microFit(ui_, fSmall_, nmR, nm, nink, Align::Center);
            if (ui_.hovered(nmR) && g_assign.src < 0) {
                if (!g_wt.err.empty()) ui_.tip = g_wt.err;
                else if (!wtLive)
                    ui_.tip = "Custom wavetables need PluginInstance::wavetable(), "
                              "which this build's plugin contract does not have";
                else if (wtCustom) {
                    char t[256];
                    snprintf(t, sizeof t,
                             "Osc %s: %s, %d frames%s. Right-click the well to "
                             "revert to a factory table.", wtOsc ? "B" : "A",
                             wtName(inst, wtOsc), wtFrames(inst, wtOsc),
                             tblIdx >= 8 ? " - playing"
                                         : " - imported, but Table is on a factory slot");
                    ui_.tip = t;
                } else
                    ui_.tip = "Osc " + std::string(wtOsc ? "B" : "A") +
                              " is on a factory table - drop a .wav here to import one";
            }

            // BROWSE. There is no modal file chooser in this program and there
            // should not be one: the file browser IS the browse, and the chip
            // opens it (Ctrl+B's other half) rather than inventing a second
            // way to find a file.
            if (wtLive) {
                if (ui_.grab(slop(brR)).segButton(uiId(UiSpectraPanel, 803, uidKey),
                                                  brR, false, nx::violet)) {
                    showBrowser_ = true;
                    status_ = "Spectra: drag a .wav from the browser onto the "
                              "wavetable well";
                }
                ui_.microIn(fSmall_, ui_.lastRect, "...", nx::muted, Align::Center);
                if (ui_.hovered(brR))
                    ui_.tip = "Browse: opens the file browser - drag a .wav from "
                              "it onto the well above";
            } else {
                ui_.microIn(fSmall_, brR, "...", nx::muted.alpha(0.35f), Align::Center);
            }

            // REVERT TO FACTORY. Present only when there IS a custom table to
            // revert from, because a control that undoes nothing is a control
            // that has to be explained.
            if (wtCustom) {
                const u64 wid = uiId(UiSpectraPanel, 804, uidKey);
                const bool hit = ui_.grab(slop(clR)).segButton(wid, clR, false,
                                                              nx::violet);
                const Rect g = ui_.lastRect;
                const f32 k = 3.f * s;
                rend_.line(g.cx() - k, g.cy() - k, g.cx() + k, g.cy() + k, 1.2f * s,
                           nx::muted);
                rend_.line(g.cx() - k, g.cy() + k, g.cx() + k, g.cy() - k, 1.2f * s,
                           nx::muted);
                if (ui_.hovered(clR))
                    ui_.tip = "Drop the imported table and put this oscillator "
                              "back on a factory one - or right-click anywhere "
                              "on the wavetable well, which is the same verb";
                if (hit) revertWt();
            }
        }

        rend_.well(dispR, nx::radiusSm * s, true);

        const bool haveA = has(pATable) && has(pAPos);
        const f32 posA = clampv(get(pAPos, 0.f), 0.f, 1.f);
        const f32 posB = clampv(get(pBPos, 0.f), 0.f, 1.f);

        if (!haveA) {
            rend_.textIn(fSmall_, dispR, "No wavetable parameters on this device",
                         nx::muted.alpha(0.7f), Align::Center);
        } else {
            // §1's light-rides-motion rule, and the one specular in the panel:
            // the band's phase IS the Position value, so it slides across the
            // glass as the morph moves and stands still when the morph does.
            // Nothing here is triggered and nothing is on a clock.
            rend_.sheen(dispR, nx::radiusSm * s, posA, 0.30f);
            rend_.hairlineH(dispR.x + 4 * s, dispR.right() - 4 * s, std::round(dispR.cy()));

            const Rect plot = dispR.insetXY(6 * s, 7 * s);
            const int cols = clampv((int)(plot.w / std::max(1.f, 1.4f * s)), 24, 512);
            // THE TABLES THEMSELVES, or null in a process that has never made a
            // Spectra. Resolved once for the whole display so the two traces
            // and the label below cannot disagree about which one they are.
            const detail::SpectraTableSet* tset = detail::spectraTables();
            // The illustration is sampled at the real frame length, so the two
            // paths decimate identically and only the source differs.
            constexpr int kIllusN = 2048;

            // B first, so A's trace reads on top of it: A is the one that is
            // always sounding.
            const auto trace = [&](int table, f32 pos, const Col& c0, f32 th) {
                if (tset) {
                    // The real frame pair, morphed at the same blend the voice
                    // morphs at, at mip 0 -- the widest level, which is what a
                    // display wants: it is the frame before any octave has been
                    // taken off it for a note that has not been played yet.
                    const detail::SpectraFrameView fv = tset->morph(table, pos);
                    if (!fv.valid()) return;
                    decimateFrame(spectraWave_, cols, fv.len,
                                  [&](int i) { return fv.at(i); });
                } else {
                    decimateFrame(spectraWave_, cols, kIllusN, [&](int i) {
                        return specimenSample(table, pos, (f32)i / (f32)kIllusN);
                    });
                    normaliseTrace(spectraWave_);
                }
                const size_t np2 = spectraWave_.size() / 2;
                if (np2 < 2) return;
                f32 px = plot.x + plot.w * spectraWave_[0];
                f32 py = plot.cy() - spectraWave_[1] * plot.h * 0.46f;
                for (size_t k = 1; k < np2; ++k) {
                    const f32 qx = plot.x + plot.w * spectraWave_[k * 2];
                    const f32 qy = plot.cy() - spectraWave_[k * 2 + 1] * plot.h * 0.46f;
                    rend_.line(px, py, qx, qy, th, c0);
                    px = qx; py = qy;
                }
            };
            // v3 WIDENED THE TABLE ENUM, and slot 8 is a table this side has no
            // view of: detail::spectraTables() publishes the eight procedural
            // sets and nothing else, and an imported table lives in the
            // instrument's own memory behind a control interface that hands out
            // a name, a frame count and no samples. So an oscillator on its
            // custom slot draws NO TRACE and its legend letter goes amber --
            // which is the display keeping the honesty it has kept since v2,
            // when warp made it say "pre-warp" rather than draw a guess.
            const int tblA = has(pATable) ? clampv((int)std::lround(get(pATable, 0.f)), 0, 8) : 0;
            const int tblB = has(pBTable) ? clampv((int)std::lround(get(pBTable, 0.f)), 0, 8) : 0;
            const bool bShown = has(pBTable) && has(pBPos) && bLevel > 1e-4f;
            if (bShown && tblB < 8)
                trace(tblB, posB, nx::violetSoft.alpha(0.75f), 1.2f * s);
            // §1: cyan is the light inside the material. This is the live shape
            // the instrument is standing on, so it is the one cyan thing here.
            if (tblA < 8) trace(tblA, posA, nx::cyan, 1.5f * s);

            // The legend, so the two traces are named rather than guessed at.
            ui_.microIn(fSmall_, {dispR.x + 6 * s, dispR.y + 1 * s, 40 * s, 10 * s},
                        "A", (tblA >= 8 ? nx::amber : nx::cyan).alpha(0.9f),
                        Align::Left, 0);
            if (bShown)
                ui_.microIn(fSmall_, {dispR.x + 16 * s, dispR.y + 1 * s, 40 * s, 10 * s},
                            "B", (tblB >= 8 ? nx::amber : nx::violetSoft).alpha(0.9f),
                            Align::Left, 0);
            if (tblA >= 8 || (bShown && tblB >= 8))
                rend_.textIn(fSmall_, dispR,
                             tblA >= 8 && bShown && tblB >= 8
                                 ? "both oscillators play imported tables"
                                 : (tblA >= 8 ? "osc A plays an imported table"
                                              : "osc B plays an imported table"),
                             nx::muted.alpha(0.55f), Align::Center);
            // And the label, which is where this display keeps its honesty.
            // With the tables in hand there is nothing left to disclaim, so it
            // says something useful instead: WHERE ON THE FRAME AXIS the morph
            // is. Position reads 0.00 to 1.00 and the axis is 32 frames long,
            // and this panel is the only place those two facts meet.
            //
            // v2 gave the label a second clause: when a warp is audibly
            // reshaping an oscillator, the trace on screen is the table BEFORE
            // the warp -- see the display comment up top for why an exact
            // warped preview is not on offer -- and "pre-warp" is the display
            // saying so rather than hoping nobody notices.
            //
            // Without the tables it goes back to the disclaimer. The corner
            // has room for about twenty characters and the whole sentence is
            // longer than that, so the label carries the WORD and the tooltip
            // carries the sentence -- and the states cannot be confused for
            // each other either way.
            const bool warpOn =
                (has(pAWarp) && std::lround(get(pAWarp, 0.f)) != 0 &&
                 get(pAWarpAmt, 0.f) > 1e-4f) ||
                (has(pBWarp) && bLevel > 1e-4f &&
                 std::lround(get(pBWarp, 0.f)) != 0 && get(pBWarpAmt, 0.f) > 1e-4f);
            char wlabel[96];
            if (tblA >= 8) {
                // THE HERO LABELS THE IMPORT BY BASENAME. A hash is the table's
                // identity and a path is a recovery hint (the contract says
                // both); neither is a thing a person recognises across a room,
                // and the file's own name is.
                const char* cn = wtLive ? wtName(inst, 0) : "";
                snprintf(wlabel, sizeof wlabel, "%s · %d frames%s",
                         (cn && *cn) ? cn : "custom table",
                         wtLive ? wtFrames(inst, 0) : 0,
                         warpOn ? " · pre-warp" : "");
            } else if (tset)
                snprintf(wlabel, sizeof wlabel, "frame %.1f / %d%s",
                         (double)(posA * (f32)(tset->frames - 1)), tset->frames,
                         warpOn ? " · pre-warp" : "");
            else
                snprintf(wlabel, sizeof wlabel, "illustration only");
            microFit(ui_, fSmall_,
                     {dispR.x, dispR.bottom() - 11 * s, dispR.w - 6 * s, 10 * s},
                     wlabel, nx::muted.alpha(0.35f), Align::Right, 0);
            if (ui_.hovered(dispR)) {
                ui_.tip = tset
                    ? (warpOn
                       ? "The wavetable frames themselves, morphed at Position - "
                         "drawn BEFORE the warp stage reshapes the read phase"
                       : "The wavetable frames themselves, morphed between the two "
                         "Position falls between - the same read the voices make")
                    : "No wavetable set in this process, so this is a drawing of "
                      "the same family of shapes and not the tables";
                // The erase verb, said on the surface it applies to and only
                // while there is something for it to erase.
                if (wtCustom)
                    ui_.tip += ". Drop a .wav to import; right-click to revert "
                               "this oscillator to a factory table.";
                else if (wtLive)
                    ui_.tip += ". Drop a .wav here to import one.";
            }
        }

        // The two Position troughs. THE control, per the contract, so it gets a
        // trough of its own rather than a knob in the osc column.
        const auto posRow = [&](f32 y, int id, const char* label, const Col& ink, f32 dim) {
            const Rect lr{c.x, y, 12 * s, slidH};
            const Rect vr{c.right() - 26 * s, y, 26 * s, slidH};
            // The whole row band, for the LFO smooth trough's reason: 9 logical
            // pixels was five under the floor and no amount of hit slop is
            // allowed to make up five.
            const Rect tr{lr.right() + 2 * s, y,
                          vr.x - lr.right() - 6 * s, slidH};
            microFit(ui_, fSmall_, lr, label, ink.alpha(dim), Align::Left, 0);
            if (!has(id)) {
                rend_.well(tr, nx::radiusXs * s, true);
                rend_.textIn(fSmall_, vr, "-", nx::muted.alpha(0.4f), Align::Right, 0);
                return;
            }
            f32 v = clampv(inst->getParam(id), 0.f, 1.f);
            const u64 wid = uiId(UiSpectraPos, id, uidKey);
            const int posDst = destOfParam(id);
            // Position is the wavetable's own control and the matrix's first
            // destination, so it takes the same two menu items its neighbours
            // in the osc columns take. It is the one non-circular control in
            // the panel that does -- and the reason it must is that a knob and
            // a trough are the same THING to the person patching, whatever
            // shape the panel drew them in.
            if (ui_.offer(menuOfferFor(id, posDst))
                   .grab(slop(tr))
                   .trough(wid, tr, &v, 0.f, 1.f, nx::cyan, dim, 0.f))
                commit(id, v, wid, "Position");
            menuHandle(wid, id, posDst, label);
            // THE MOD RING, on a control that is not a circle. Position is a
            // trough, so its reach is a BRACKET along the trough's own top edge
            // -- the same number, the same rule (static, never a meter), drawn
            // in the shape the control actually has.
            {
                const ModReach mr = modReach(destOfParam(id));
                if (mr.slots) {
                    const f32 x0 = tr.x + tr.w * clampv(v + mr.lo, 0.f, 1.f);
                    const f32 x1 = tr.x + tr.w * clampv(v + mr.hi, 0.f, 1.f);
                    const f32 yb = tr.y - 1.f * s;
                    const f32 th = std::max(1.f, nx::snapPx(s));
                    rend_.rect({std::min(x0, x1), yb, std::max(th, std::fabs(x1 - x0)), th},
                               nx::cyan.alpha(0.55f * dim));
                    rend_.rect({x0 - th * 0.5f, yb - 1.5f * s, th, 3.f * s},
                               nx::cyan.alpha(0.55f * dim));
                    rend_.rect({x1 - th * 0.5f, yb - 1.5f * s, th, 3.f * s},
                               nx::cyan.alpha(0.55f * dim));
                }
            }
            dropTarget(tr, id, label);
            char buf[24];
            snprintf(buf, sizeof buf, "%.2f", (double)v);
            rend_.textIn(fSmall_, vr, buf,
                         (ui_.hovered(tr) ? nx::text : nx::muted).alpha(dim), Align::Right, 0);
            if (ui_.hovered(tr))
                ui_.tip = std::string("Osc ") + label +
                          " frame morph position - the wavetable's own control";
        };
        posRow(dispR.bottom() + 4 * s, pAPos, "A", nx::cyan, 1.f);
        posRow(dispR.bottom() + 4 * s + slidH + 2 * s, pBPos, "B", nx::violetSoft, dimB);
    }

    // =======================================================================
    // 2. OSC A / OSC B
    // =======================================================================
    const auto oscColumn = [&](int ci, const char* label, int base, f32 dim) {
        const Rect c = col(ci);
        const f32 y0 = sect(c, label, dim);
        // THE TABLE ENUM ASKS THE DEVICE HOW WIDE IT IS. The contract widened
        // id 0 / id 8 from 0..7 to 0..8, where 8 is THIS oscillator's imported
        // table -- so the ring the arrows walk is eight long against a v2 DSP
        // and nine long against a v3 one, and slot 8 wears the imported file's
        // own basename rather than the word "custom", because that is the name
        // the person who imported it will look for.
        const int  tblId = base + 0;
        const int  nTbl  = clampv(enumMax(tblId, 7) + 1, 1, 9);
        const int  oscN  = base == pBTable ? 1 : 0;
        const char* cName = wtSupported(inst) ? wtName(inst, oscN) : "";
        const char* tblNames[9] = {kTables[0], kTables[1], kTables[2], kTables[3],
                                   kTables[4], kTables[5], kTables[6], kTables[7],
                                   (cName && *cName) ? cName : "no import"};
        stepper({c.x, y0, c.w, subH}, tblId, nTbl, tblNames, dim, "no table",
                "Table", nTbl > 8
                    ? "The oscillator's wavetable - eight factory tables and, "
                      "at the end of the ring, the one imported onto this "
                      "oscillator"
                    : "The oscillator's wavetable - the arrows cycle the eight");

        const f32 cw = c.w / 3.f;
        const f32 r1 = y0 + subH + gap, r2 = r1 + rowH + gap;
        Ui::KnobStyle st;
        st.arc = nx::violet;

        st.lo = -24.f; st.hi = 24.f; st.def = 0.f; st.bipolar = true; st.fmt = "%.0f";
        knob({c.x, r1, cw, rowH}, base + 2, "coarse", st, dim);
        st.lo = -100.f; st.hi = 100.f; st.fmt = "%.0f";
        knob({c.x + cw, r1, cw, rowH}, base + 3, "fine", st, dim);
        st.bipolar = false;
        st.lo = 0.f; st.hi = 1.f; st.def = (base == pATable ? 1.f : 0.f); st.fmt = "%.2f";
        knob({c.x + cw * 2.f, r1, cw, rowH}, base + 4, "level", st, dim);

        st.lo = 1.f; st.hi = 7.f; st.def = 1.f; st.fmt = "%.0f";
        knob({c.x, r2, cw, rowH}, base + 5, "unison", st, dim);
        st.lo = 0.f; st.hi = 100.f; st.def = 0.f; st.fmt = "%.0f";
        knob({c.x + cw, r2, cw, rowH}, base + 6, "detune", st, dim);
        st.lo = 0.f; st.hi = 1.f; st.fmt = "%.2f";
        knob({c.x + cw * 2.f, r2, cw, rowH}, base + 7, "spread", st, dim);
    };
    oscColumn(1, "OSC A", pATable, 1.f);
    oscColumn(2, "OSC B", pBTable, dimB);

    // =======================================================================
    // 3. FILTER
    // =======================================================================
    {
        const Rect c = col(3);
        const f32 y0 = sect(c, "FILTER", 1.f);

        // The types are ONE cluster, not separate capsules: exactly one is
        // chosen (app_devices.cpp's segmented rule). HOW MANY is the device's
        // answer, not this file's: the contract widened id 20 from three types
        // to six (LP24/HP24/Notch appended), strictly compatibly, so the
        // cluster reads paramInfo(20).max and cuts itself into that many
        // segments -- three against the v1 DSP, six against v2, with no edit
        // here when the DSP lands. At six segments a 138px column gives 23px
        // apiece, which no label survives, so the segments carry drawn
        // RESPONSE CURVES instead (the LFO cluster's precedent: at that size
        // an icon says more than letters) -- slope down, hump, slope up; the
        // 24 dB pair visibly steeper; the notch a dip. The tooltip says the
        // words.
        const Rect ftR{c.x, y0, c.w, subH};
        ui_.segCluster(ftR);
        const bool ftLive = has(pFType);
        const int nTypes = ftLive
            ? clampv((int)std::lround(inst->paramInfo(pFType).max) + 1, 2, 6) : 3;
        static const char* const kTypeName[6] = {"LP 12", "BP 12", "HP 12",
                                                 "LP 24", "HP 24", "Notch"};
        const f32 segW = ftR.w / (f32)nTypes;
        const int ftype = ftLive
            ? clampv((int)std::lround(get(pFType, 0.f)), 0, nTypes - 1) : -1;
        for (int k = 0; k < nTypes; ++k) {
            const Rect seg{ftR.x + segW * (f32)k, ftR.y, segW, ftR.h};
            if (k) rend_.hairlineV(seg.x, ftR.y + 2 * s, ftR.bottom() - 2 * s);
            const bool on = k == ftype;
            Rect g = seg;
            if (ftLive) {
                const u64 wid = uiId(UiSpectraPanel, 600 + k, uidKey);
                if (ui_.grab(slop(seg)).segButton(wid, seg, on, nx::violet))
                    commit(pFType, (f32)k, wid, "Filter Type");
                g = ui_.lastRect;
            }
            // The response glyph. yT is the passband, yB the floor; the 24s
            // fall over a quarter of the width where the 12s take half.
            const Col ic = on ? nx::text : nx::muted.alpha(ftLive ? 0.85f : 0.45f);
            const f32 th = 1.1f * s;
            const f32 x0 = g.cx() - 7.f * s, x1 = g.cx() + 7.f * s;
            const f32 yT = g.cy() - 2.6f * s, yB = g.cy() + 3.2f * s;
            const f32 w12 = 7.f * s, w24 = 3.5f * s;
            switch (k) {
            case 0:                                     // LP 12
                rend_.line(x0, yT, x1 - w12, yT, th, ic);
                rend_.line(x1 - w12, yT, x1, yB, th, ic);
                break;
            case 1:                                     // BP 12
                rend_.line(x0, yB, g.cx() - 1.5f * s, yT, th, ic);
                rend_.line(g.cx() - 1.5f * s, yT, g.cx() + 1.5f * s, yT, th, ic);
                rend_.line(g.cx() + 1.5f * s, yT, x1, yB, th, ic);
                break;
            case 2:                                     // HP 12
                rend_.line(x0, yB, x0 + w12, yT, th, ic);
                rend_.line(x0 + w12, yT, x1, yT, th, ic);
                break;
            case 3:                                     // LP 24 -- the steep one
                rend_.line(x0, yT, x1 - w24, yT, th, ic);
                rend_.line(x1 - w24, yT, x1, yB, th, ic);
                break;
            case 4:                                     // HP 24
                rend_.line(x0, yB, x0 + w24, yT, th, ic);
                rend_.line(x0 + w24, yT, x1, yT, th, ic);
                break;
            default:                                    // Notch
                rend_.line(x0, yT, g.cx() - 2.2f * s, yT, th, ic);
                rend_.line(g.cx() - 2.2f * s, yT, g.cx(), yB, th, ic);
                rend_.line(g.cx(), yB, g.cx() + 2.2f * s, yT, th, ic);
                rend_.line(g.cx() + 2.2f * s, yT, x1, yT, th, ic);
                break;
            }
        }
        if (ui_.hovered(ftR)) {
            char t[96];
            if (ftLive)
                snprintf(t, sizeof t, "Filter type: %s (%d of %d on this DSP)",
                         ftype >= 0 ? kTypeName[ftype] : "?", ftype + 1, nTypes);
            else
                snprintf(t, sizeof t, "This device has no filter type parameter");
            ui_.tip = t;
        }

        const f32 cw = c.w / 3.f;
        const f32 r1 = y0 + subH + gap, r2 = r1 + rowH + gap;
        Ui::KnobStyle st;
        st.lo = 20.f; st.hi = 20000.f; st.def = 20000.f; st.log = true; st.fmt = "%.0f";
        knob({c.x, r1, cw, rowH}, pCutoff, "cutoff", st, 1.f);
        st.log = false;
        st.lo = 0.f; st.hi = 1.f; st.def = 0.f; st.fmt = "%.2f";
        knob({c.x + cw, r1, cw, rowH}, pReso, "reso", st, 1.f);
        st.lo = 0.f; st.hi = 24.f; st.fmt = "%.1f";
        knob({c.x + cw * 2.f, r1, cw, rowH}, pDrive, "drive", st, 1.f);

        st.lo = 0.f; st.hi = 1.f; st.fmt = "%.2f";
        knob({c.x, r2, cw, rowH}, pKeytrack, "keytrk", st, 1.f);
        // The two ENV2 depths sit together, and deliberately: they are the same
        // control -- how far the mod envelope reaches -- pointed at two
        // destinations, and a panel that split them across two sections would
        // make the pair look like a coincidence. §5's bipolar detent is drawn
        // on both, so "off" is a place the knob can be seen to be at.
        st.lo = -1.f; st.hi = 1.f; st.def = 0.f; st.bipolar = true;
        st.arc = nx::violetSoft;
        knob({c.x + cw, r2, cw, rowH}, pE2Cutoff, "e2>cut", st, 1.f);
        knob({c.x + cw * 2.f, r2, cw, rowH}, pE2Pos, "e2>pos", st, 1.f);
    }

    // =======================================================================
    // 4. ENV 1 / ENV 2
    // =======================================================================
    {
        const Rect c = col(4);
        const f32 y0 = sect(c, "ENVELOPES", 1.f);
        // This column has no selector row, so its two knob rows sit where every
        // other column's do -- the band the others use for a stepper is left
        // empty rather than stealing it, because rows that line up across seven
        // columns are most of what makes a panel read as built.
        const f32 er1 = y0 + subH + gap, er2 = er1 + rowH + gap;
        envRow(c, er1, "ENV 1", pAttack,   5.f, 200.f, 0.7f, 300.f);
        rend_.hairlineH(c.x, c.right(), er2 - 2 * s);
        envRow(c, er2, "ENV 2", pE2Attack, 5.f, 200.f, 0.7f, 300.f, 4);
    }

    // =======================================================================
    // 5. LFO
    // =======================================================================
    {
        const Rect c = col(5);
        const f32 y0 = sect(c, "LFO", 1.f);
        const int shape = shapeCluster({c.x, y0, c.w, subH}, pLfoShape, 50,
                                       "LFO Shape");
        const bool drawn = shape == 5;

        const f32 cw = c.w / 3.f;
        const f32 r1 = y0 + subH + gap, r2 = r1 + rowH + gap;

        // THE HEADER BAND CARRIES THIS LFO'S OWN CHROME: the mode toggle, the
        // grab handle that makes it a draggable source, and -- only when the
        // drawn grid is on -- the sync selector, compacted.
        //
        // WHY SYNC MOVES, stated where the cost is paid. A 16-step grid wants
        // two of a column's three cells and a row of its own. LFO 2 and LFO 3
        // have exactly that free, because the contract gives them no fixed
        // routings; LFO 1 does not -- its six cells hold rate, three fixed
        // depths and a two-cell sync selector, and there is no arrangement of
        // six cells that also holds a grid. So choosing Custom moves sync into
        // the header band, which is the one band in this column with room, and
        // the grid takes the two cells it vacated. NOTHING IS HIDDEN: every
        // control that was on screen before the shape changed is still on
        // screen after it, which is the property that mattered.
        {
            const f32 hy = c.y, hh = headH;
            const Rect handR{c.right() - 12 * s, hy, 12 * s, hh};
            srcHandle(handR, 1);
            const Rect modeR{handR.x - 32 * s, hy + 0.5f * s, 30 * s, hh - 1 * s};
            modeToggle(880, modeR, pL1Mode, "L1 Mode");
            if (drawn)
                enumSel(870, {modeR.x - 46 * s, hy + 0.5f * s, 44 * s, hh - 1 * s},
                        pLfoSync, kSyncDiv, 10, "Sync", 1.f);
        }

        // The sync selector takes two cells of the second row and is a
        // 14px control inside a 48px band, so it is centred in it rather than
        // hung off the top -- the row is what the eye aligns on.
        int sync = has(pLfoSync) ? clampv((int)std::lround(get(pLfoSync, 0.f)), 0, 9) : 0;
        if (drawn) {
            lfoGridBlock({c.x, r2 + 1 * s, cw * 2.f - 2 * s, rowH - 2 * s}, 0);
        } else {
            const Rect syncR{c.x, r2 + (rowH - lblH - subH) * 0.5f, cw * 2.f, subH};
            sync = stepper(syncR, pLfoSync, 10, kSyncDiv, 1.f, "no sync",
                           "Sync",
                           "LFO sync: free-running, or a division of the "
                           "transport's tempo");
            microFit(ui_, fSmall_, {c.x, r2 + rowH - lblH, cw * 2.f, lblH}, "sync",
                     nx::muted.alpha(0.85f), Align::Center);
        }

        Ui::KnobStyle st;
        // THE RATE KNOB SWAPS ITS READOUT. When the LFO is synced the number in
        // hertz is not what the LFO is doing, and printing it anyway is the
        // kind of small lie a panel never recovers from -- so the readout
        // becomes the division and the knob drops to the disabled weight.
        const bool freeRun = !has(pLfoSync) || sync == 0;
        st.lo = 0.01f; st.hi = 40.f; st.def = 2.f; st.log = true; st.fmt = "%.2f";
        if (!freeRun) st.text = kSyncDiv[clampv(sync, 0, 9)];
        knob({c.x, r1, cw, rowH}, pLfoRate, "rate", st, freeRun ? 1.f : 0.55f);
        st.text = nullptr; st.log = false;
        st.lo = -1.f; st.hi = 1.f; st.def = 0.f; st.bipolar = true; st.arc = nx::violetSoft;
        knob({c.x + cw, r1, cw, rowH}, pLfoPos, "l>pos", st, 1.f);
        knob({c.x + cw * 2.f, r1, cw, rowH}, pLfoCut, "l>cut", st, 1.f);
        st.bipolar = false; st.lo = 0.f; st.hi = 100.f; st.def = 0.f; st.fmt = "%.0f";
        st.arc = nx::violet;
        knob({c.x + cw * 2.f, r2, cw, rowH}, pLfoPitch, "l>ptch", st, 1.f);
    }

    // =======================================================================
    // 6. GLOBAL + PRESETS
    // =======================================================================
    {
        const Rect c = col(6);
        // --- the preset row, which is also this section's selector band -----
        // ABSENT, not disabled, when the device declares no presets: a row of
        // arrows around an empty name is a promise the device has not made, and
        // until the three virtuals exist on PluginInstance no device can make
        // it. The section's own header says so too -- it does not advertise a
        // control that is not there.
        const f32 y0 = sect(c, np > 0 ? "GLOBAL / PRESET" : "GLOBAL", 1.f);

        // =====================================================================
        // SAVE -- pillar 3's whole surface in this file.
        //
        // A chip in the section header, and then the arrangement's
        // marker-rename idiom exactly: the thing you are naming BECOMES the
        // field. The preset chip is what the popover hangs from and what the
        // name is read from, so the preset chip is what you type into -- there
        // is no dialog, no second surface, and nowhere for the name to appear
        // that is not where the name lives.
        //
        // GUARDED AT THE CONTRACT, not at the device. savePreset()'s own
        // default returns false, so a device that cannot save presets answers
        // for itself and the chip stays live to hear the refusal; what the chip
        // cannot survive is the METHOD not existing, which is a compile
        // question and is answered by psSupported<>() -- inert, and the tooltip
        // says which of the two absences it is.
        // =====================================================================
        const u64 saveFieldId = uiId(UiSpectraPanel, 851, uidKey);
        const bool saveLive = psSupported<PluginInstance>();
        {
            const Rect chip{c.right() - 34 * s, c.y, 34 * s, headH};
            if (saveLive && !g_save.open) {
                if (ui_.grab(slop(chip)).button(uiId(UiSpectraPanel, 850, uidKey),
                                                chip, "") ||
                    g_save.pending) {
                    g_save.pending = false;
                    g_save.open = true;
                    g_save.justOpened = true;
                    closeDrop();
                    g_save.buf = np > 0 ? presetNameOf(*inst, spectraPreset_) : "";
                    // beginEdit()'s body, done from outside because the click
                    // that opens the field lands on the CHIP and not on the
                    // field -- the marker rename gets this for free only
                    // because its double-click lands on the flag the field
                    // replaces.
                    ui_.editId = saveFieldId;
                    ui_.editBuf = g_save.buf;
                    ui_.caret = (int)ui_.editBuf.size();
                    ui_.active = saveFieldId;
                }
                ui_.microIn(fSmall_, ui_.lastRect, "SAVE",
                            ui_.hovered(chip) ? nx::text : nx::muted.alpha(0.85f),
                            Align::Center);
            } else {
                ui_.microIn(fSmall_, chip, "SAVE",
                            g_save.open ? nx::violetSoft : nx::muted.alpha(0.35f),
                            Align::Center);
            }
            if (ui_.hovered(chip) && g_assign.src < 0)
                ui_.tip = saveLive
                    ? "Save this patch to the user bank - the name field opens "
                      "in the preset row; Enter saves, Escape cancels"
                    : "User presets need PluginInstance::savePreset(), which "
                      "this build's plugin contract does not have";
        }

        if (np > 0) {
            spectraPreset_ = clampv(spectraPreset_, 0, np - 1);
            const Rect pr{c.x, y0, c.w, subH};
            presetRowR = pr;
            ui_.segCluster(pr);
            const f32 bw = 16 * s;
            const Rect lb{pr.x, pr.y, bw, pr.h}, rb{pr.right() - bw, pr.y, bw, pr.h};
            rend_.hairlineV(lb.right(), pr.y + 2 * s, pr.bottom() - 2 * s);
            rend_.hairlineV(rb.x, pr.y + 2 * s, pr.bottom() - 2 * s);
            // A preset rewrites every parameter at once, so it is one undo
            // entry like any other edit -- and a one-shot, so it takes a point
            // every time rather than coalescing. Shared with the popover: the
            // arrows, a row click and the Enter key are one verb, spelled once.
            const auto loadIdx = [&](int n) {
                n = ((n % np) + np) % np;
                undoPoint("load preset");
                spectraPreset_ = n;
                inst->loadPreset(n);
                status_ = std::string("Spectra: ") + presetNameOf(*inst, n);
            };
            const auto chev = [&](const Rect& b, bool leftward) {
                const f32 k = 2.6f * s, d = leftward ? -1.f : 1.f;
                rend_.line(b.cx() - k * d * 0.6f, b.cy() - k,
                           b.cx() + k * d * 0.6f, b.cy(), 1.1f * s, nx::muted);
                rend_.line(b.cx() - k * d * 0.6f, b.cy() + k,
                           b.cx() + k * d * 0.6f, b.cy(), 1.1f * s, nx::muted);
            };
            // The arrows stay beside the dropdown: one-hand browsing is a
            // different gesture from picking by name, and it costs 32px.
            // ...and they stand down while the name field is open: a click on
            // an arrow would commit the typed name (textField's own rule) and
            // load a different preset in the same frame, which is two verbs for
            // one click and the second one is a surprise.
            if (!g_save.open) {
                if (ui_.grab(slop(lb)).segButton(uiId(UiSpectraPanel, 60, uidKey), lb,
                                                false, nx::violet))
                    loadIdx(spectraPreset_ - 1);
                chev(ui_.lastRect, true);
                if (ui_.grab(slop(rb)).segButton(uiId(UiSpectraPanel, 61, uidKey), rb,
                                                false, nx::violet))
                    loadIdx(spectraPreset_ + 1);
                chev(ui_.lastRect, false);
            } else {
                chev(lb, true);
                chev(rb, false);
            }

            // THE CHIP -- the dropdown the preset name always wanted to be.
            // The name area is a segment of the same cluster; clicking it
            // opens the popover (drawn at the end of the frame so it floats
            // over everything), and the segment's ON state is the popover
            // being open -- the §5 affordance for "this control is engaged".
            // While the popover is open this chip is shielded like everything
            // else, and the click that would land on it falls to the popover's
            // click-outside rule, which closes -- so the chip toggles without
            // owning a second gesture.
            const Rect nameR{lb.right(), pr.y, rb.x - lb.right(), pr.h};

            // THE NAME FIELD. The preset chip, being typed into.
            if (g_save.open) {
                const bool birth = g_save.justOpened;
                g_save.justOpened = false;
                const bool done = ui_.textField(saveFieldId, nameR, &g_save.buf,
                                                nx::panel2, nx::text, Align::Left,
                                                false) && !birth;
                // ...and if the birth frame's stray press took the caret with
                // it on the way past, hand it straight back. beginEdit()'s body
                // again, for the same reason it is here at all.
                if (birth && ui_.editId != saveFieldId) {
                    ui_.editId  = saveFieldId;
                    ui_.editBuf = g_save.buf;
                    ui_.caret   = (int)ui_.editBuf.size();
                    ui_.active  = saveFieldId;
                }
                if (done) {
                    g_save.open = false;
                    // The contract's own refusals, checked HERE for the two it
                    // can name better than a bool can: an empty name and an
                    // over-long one. Everything else savePreset() refuses for
                    // its own reasons and the status bar carries the general
                    // form, because a UI that invented a reason would be
                    // guessing at a failure it did not see.
                    std::string nm = g_save.buf;
                    while (!nm.empty() && (nm.back() == ' ' || nm.back() == '\t')) nm.pop_back();
                    size_t lead = 0;
                    while (lead < nm.size() && nm[lead] == ' ') ++lead;
                    nm.erase(0, lead);
                    if (nm.empty()) {
                        status_ = "Spectra: a preset needs a name - nothing was saved";
                    } else if (nm.size() > 64) {
                        status_ = "Spectra: a preset name is at most 64 bytes - "
                                  "nothing was saved";
                    } else {
                        // Was there already a user preset under this display
                        // name? The contract keeps ONE generation of the
                        // overwritten file as <name>.nxp.bak and does not enter
                        // undo -- presets are files, not session state -- so the
                        // announcement is the only record the editor makes of it.
                        const int fc0 = clampv(psFactoryCount(inst), 0, np);
                        bool replaced = false;
                        for (int i = fc0; i < np && !replaced; ++i)
                            replaced = nm == presetNameOf(*inst, i);
                        char msg[176];
                        if (psSave(inst, nm.c_str())) {
                            const int np2 = inst->presetCount();
                            const int fc2 = clampv(psFactoryCount(inst), 0, np2);
                            for (int i = fc2; i < np2; ++i)
                                if (nm == presetNameOf(*inst, i)) { spectraPreset_ = i; break; }
                            snprintf(msg, sizeof msg,
                                     replaced ? "Spectra: preset '%s' replaced - the "
                                                "previous file is kept as .nxp.bak"
                                              : "Spectra: preset '%s' saved to the "
                                                "user bank",
                                     nm.c_str());
                        } else {
                            snprintf(msg, sizeof msg,
                                     "Spectra: '%s' could not be written - check "
                                     "$XDG_CONFIG_HOME/nxtakt/presets",
                                     nm.c_str());
                        }
                        status_ = msg;
                    }
                } else if (ui_.editId != saveFieldId) {
                    g_save.open = false;         // Escape, or a press elsewhere
                    status_ = "Spectra: preset not saved";
                }
                if (ui_.hovered(nameR))
                    ui_.tip = "Name this patch. Enter saves it to the user bank; "
                              "Escape leaves the bank alone.";
            } else {
            if (ui_.grab(slop(nameR)).segButton(uiId(UiSpectraPanel, 62, uidKey),
                                               nameR, g_drop.open, nx::violet) &&
                !g_drop.open)
                openDrop();
            const Rect nr = ui_.lastRect;
            {   // the caret, drawn: a name with a caret is a menu, everywhere.
                const f32 k = 2.2f * s;
                const f32 cx = nr.right() - 8 * s, cy = nr.cy() - 0.5f * s;
                const Col cc = g_drop.open ? nx::text : nx::muted;
                rend_.line(cx - k, cy, cx, cy + k, 1.1f * s, cc);
                rend_.line(cx, cy + k, cx + k, cy, 1.1f * s, cc);
            }
            microFit(ui_, fSmall_, {nr.x + 2 * s, nr.y, nr.w - 16 * s, nr.h},
                     presetNameOf(*inst, spectraPreset_),
                     g_drop.open ? nx::text : nx::text.alpha(0.92f), Align::Center);
            if (ui_.hovered(pr) && g_assign.src < 0) {
                const int fc = clampv(psFactoryCount(inst), 0, np);
                char t[128];
                snprintf(t, sizeof t,
                         "Preset %d of %d (%s) - click the name for the list, "
                         "arrows step", spectraPreset_ + 1, np,
                         spectraPreset_ >= fc ? "user bank" : "factory bank");
                ui_.tip = t;
            }
            }   // the name field / name chip branch
        }

        const f32 cw = c.w / 3.f;
        const f32 r1 = y0 + subH + gap, r2 = r1 + rowH + gap;
        Ui::KnobStyle st;
        // GLIDE -- the second widening. The contract stretched id 38 from
        // 0..500 to 0..2000 ms without moving a stored value, so the knob asks
        // the device for its ceiling instead of asserting one: 500 against the
        // v1 DSP, 2000 against v2, clamped to the contract's own bounds in
        // case the hook has pointed the panel at something weird.
        st.lo = 0.f;
        st.hi = has(pGlide) ? clampv(inst->paramInfo(pGlide).max, 500.f, 2000.f) : 500.f;
        st.def = 0.f; st.fmt = "%.0f";
        knob({c.x, r1, cw, rowH}, pGlide, "glide", st, 1.f);
        // Voices is ignored under Mono/Legato (contract, id 98), and a control
        // being ignored says so the way the synced rate knob does: the
        // disabled weight, with the value still readable.
        st.lo = 1.f; st.hi = 16.f; st.def = 8.f;
        knob({c.x + cw, r1, cw, rowH}, pVoices, "voices", st, vmode ? 0.55f : 1.f);
        st.lo = 0.f; st.hi = 1.5f; st.def = 1.f; st.fmt = "%.2f";
        knob({c.x + cw * 2.f, r1, cw, rowH}, pMaster, "master", st, 1.f);
        // Noise and Sub LEVELS live here with the global mix, as v1 put them;
        // their v2 shaping (shape, octave, color, tracking) is the MOD page's
        // first column. Levels are what you reach for while mixing, shaping is
        // what you reach for while designing, and the pages split on exactly
        // that line.
        st.lo = 0.f; st.hi = 1.f; st.def = 0.f;
        knob({c.x, r2, cw, rowH}, pNoise, "noise", st, 1.f);
        knob({c.x + cw, r2, cw, rowH}, pSub, "sub", st, 1.f);
        // BEND RANGE (id 99), and it belongs here rather than beside an LFO:
        // it is not modulation, it is the calibration of a thing on the desk,
        // which is what this column is for. It is also v3's ONE non-inert
        // default -- the contract says so out loud: 2 semitones, not 0, because
        // a build that ignored the pitch wheel was a broken instrument. The two
        // half-cells this row used to waste are what pays for it.
        st.lo = 0.f; st.hi = 24.f; st.def = 2.f; st.fmt = "%.0f";
        knob({c.x + cw * 2.f, r2, cw, rowH}, pBendRange, "bend", st, 1.f,
             (int)pCountV3);
    }

    } else if (g_page == 1) {
    // =======================================================================
    // MOD -- the parity push's face. Same grid, same guard, same idioms: a
    // column is header / selector band / two knob rows, absent ids draw as
    // sockets, and nothing here is reachable when the pointer is shielded.
    // =======================================================================

    // =======================================================================
    // M1. SUB & NOISE (+ VOICE) -- ids 42..45, 98
    // =======================================================================
    {
        const Rect c = col(0);
        const f32 y0 = sect(c, "SUB & NOISE", 1.f);

        // Sub shape: three segments, labelled -- at 48px a segment the words
        // fit, so no icons needed here.
        {
            const Rect shR{c.x, y0, c.w, subH};
            ui_.segCluster(shR);
            const bool live = has(pSubShape);
            const f32 sw = shR.w / 3.f;
            const int cur = live ? clampv((int)std::lround(get(pSubShape, 0.f)), 0, 2) : -1;
            for (int k = 0; k < 3; ++k) {
                const Rect seg{shR.x + sw * (f32)k, shR.y, sw, shR.h};
                if (k) rend_.hairlineV(seg.x, shR.y + 2 * s, shR.bottom() - 2 * s);
                const bool on = k == cur;
                Rect g = seg;
                if (live) {
                    const u64 wid = uiId(UiSpectraPanel, 650 + k, uidKey);
                    if (ui_.grab(slop(seg)).segButton(wid, seg, on, nx::violet))
                        commit(pSubShape, (f32)k, wid, "Sub Shape");
                    g = ui_.lastRect;
                }
                ui_.microIn(fSmall_, g, kSubShape[k],
                            on ? nx::text : nx::muted.alpha(live ? 0.85f : 0.45f),
                            Align::Center);
            }
            if (ui_.hovered(shR))
                ui_.tip = live ? "Sub oscillator shape - follows osc A's post-glide "
                                 "pitch, shifted by the octave knob"
                               : "This device has no sub shape parameter";
        }

        const f32 cw = c.w / 3.f;
        const f32 r1 = y0 + subH + gap, r2 = r1 + rowH + gap;
        Ui::KnobStyle st;
        st.lo = -2.f; st.hi = 0.f; st.def = -1.f; st.fmt = "%.0f";
        knob({c.x, r1, cw, rowH}, pSubOct, "oct", st, 1.f);
        st.lo = 0.f; st.hi = 1.f; st.def = 1.f; st.fmt = "%.2f";
        knob({c.x + cw, r1, cw, rowH}, pNoiseColor, "color", st, 1.f);
        // Noise Track is a bit, and a bit is a toggle, not a knob: one segment
        // that is on or off, centred in the cell the third knob would take.
        {
            const Rect tg{c.x + cw * 2.f + 4 * s, r1 + (rowH - lblH - subH) * 0.5f,
                          cw - 8 * s, subH};
            const bool live = has(pNoiseTrack);
            const bool on = live && get(pNoiseTrack, 0.f) > 0.5f;
            ui_.segCluster(tg);
            if (live) {
                const u64 wid = uiId(UiSpectraPanel, 670, uidKey);
                if (ui_.grab(slop(tg)).segButton(wid, tg, on, nx::violet))
                    commit(pNoiseTrack, on ? 0.f : 1.f, wid, "Noise Track");
                ui_.microIn(fSmall_, ui_.lastRect, "TRK",
                            on ? nx::text : nx::muted.alpha(0.85f), Align::Center);
            } else {
                ui_.microIn(fSmall_, tg, "-", nx::muted.alpha(0.4f), Align::Center);
            }
            microFit(ui_, fSmall_, {c.x + cw * 2.f, r1 + rowH - lblH, cw, lblH},
                     "track", nx::muted.alpha(live ? 0.85f : 0.40f), Align::Center);
            if (ui_.hovered(tg))
                ui_.tip = live ? "Noise Track: the color filter follows the played "
                                 "note (C4 reference), post-glide"
                               : "This device has no noise track parameter";
        }

        // VOICE MODE shares the column: the sub is the one source that follows
        // glide, and glide is what mono/legato are for -- the two belong in
        // eyeshot of each other. Centred in the second row band like the sync
        // selector, with its label underneath.
        {
            rend_.hairlineH(c.x, c.right(), r2 - 2 * s);
            const Rect vc{c.x, r2 + (rowH - lblH - subH) * 0.5f, c.w, subH};
            ui_.segCluster(vc);
            const bool live = has(pVoiceMode);
            const f32 sw = vc.w / 3.f;
            for (int k = 0; k < 3; ++k) {
                const Rect seg{vc.x + sw * (f32)k, vc.y, sw, vc.h};
                if (k) rend_.hairlineV(seg.x, vc.y + 2 * s, vc.bottom() - 2 * s);
                const bool on = live && k == vmode;
                Rect g = seg;
                if (live) {
                    const u64 wid = uiId(UiSpectraPanel, 660 + k, uidKey);
                    if (ui_.grab(slop(seg)).segButton(wid, seg, on, nx::violet))
                        commit(pVoiceMode, (f32)k, wid, "Voice Mode");
                    g = ui_.lastRect;
                }
                ui_.microIn(fSmall_, g, kVoiceMode[k],
                            on ? nx::text : nx::muted.alpha(live ? 0.85f : 0.45f),
                            Align::Center);
            }
            microFit(ui_, fSmall_, {c.x, r2 + rowH - lblH, c.w, lblH}, "voice mode",
                     nx::muted.alpha(live ? 0.85f : 0.40f), Align::Center);
            if (ui_.hovered(vc))
                ui_.tip = live
                    ? "Poly: id 39 caps voices. Mono: retriggers and glides. "
                      "Legato: overlapped notes glide without retriggering."
                    : "This device has no voice mode parameter";
        }
    }

    // =======================================================================
    // M2. WARP -- ids 48..51. One mode + depth per oscillator, phase-domain.
    // =======================================================================
    {
        const Rect c = col(1);
        const f32 y0 = sect(c, "WARP", 1.f);
        stepper({c.x, y0, c.w, subH}, pAWarp, 8, kWarpMode, 1.f, "no warp",
                "A Warp",
                "Osc A warp mode - reshapes the read phase per unison voice; "
                "at amount 0 every mode is bit-identical to Off");
        const f32 cw = c.w / 3.f;
        const f32 r1 = y0 + subH + gap, r2 = r1 + rowH + gap;
        Ui::KnobStyle st;
        st.lo = 0.f; st.hi = 1.f; st.def = 0.f; st.fmt = "%.2f";
        knob({c.x + cw * 0.5f, r1, cw, rowH}, pAWarpAmt, "a amt", st, 1.f);
        // B's warp stays at full weight even when B is silent: an osc whose
        // level is 0 still feeds the other's FM/RM tap (the contract is
        // explicit about it), so its warp is not "doing nothing".
        knob({c.x + cw * 1.5f, r1, cw, rowH}, pBWarpAmt, "b amt", st, 1.f);
        const Rect bR{c.x, r2 + (rowH - lblH - subH) * 0.5f, c.w, subH};
        stepper(bR, pBWarp, 8, kWarpMode, 1.f, "no warp", "B Warp",
                "Osc B warp mode - as A's; FM/RM read the OTHER oscillator "
                "through a one-sample delay");
        microFit(ui_, fSmall_, {c.x, r2 + rowH - lblH, c.w, lblH}, "b mode",
                 nx::muted.alpha(0.85f), Align::Center);
    }

    // =======================================================================
    // M3/M4. LFO 2 and LFO 3 -- ids 54..59. LFO1's column with the depth
    // knobs deliberately absent: the contract gives these two NO fixed
    // routings, so the cells LFO1 spends on l>pos / l>cut / l>ptch stay empty
    // here -- the matrix two columns over is where their reach is patched, and
    // an empty cell is the layout saying so.
    // =======================================================================
    // `n` is the LFO's 0-based number, which is what the state string's grid
    // block is keyed on and what the matrix source value derives from (source
    // 1..3 is LFO 1..3, so this LFO's source is n + 1).
    const auto lfoColumn = [&](int ci, int n, const char* label, int rateId,
                               int syncId, int shapeId, int modeId, int segBase,
                               const char* whatShape, const char* whatMode) {
        const Rect c = col(ci);
        const f32 y0 = sect(c, label, 1.f);
        const int shape = shapeCluster({c.x, y0, c.w, subH}, shapeId, segBase,
                                       whatShape);

        const f32 cw = c.w / 3.f;
        const f32 r1 = y0 + subH + gap, r2 = r1 + rowH + gap;
        // The same header chrome LFO 1 wears, and it needs no re-cut here: the
        // contract gives LFO 2 and LFO 3 no fixed routings, so the two cells
        // LFO 1 spends on l>pos / l>cut are empty in this column -- which is
        // exactly the two cells a drawn grid wants. The empty cell was the
        // layout saying "their reach is patched in the matrix"; when the grid
        // is on it says what the grid is.
        {
            const Rect handR{c.right() - 12 * s, c.y, 12 * s, headH};
            srcHandle(handR, n + 1);
            modeToggle(880 + n, {handR.x - 32 * s, c.y + 0.5f * s, 30 * s,
                                 headH - 1 * s}, modeId, whatMode);
        }
        if (shape == 5)
            lfoGridBlock({c.x + cw + 1 * s, r1 + 1 * s, cw * 2.f - 2 * s,
                          rowH - 2 * s}, n);
        const Rect syncR{c.x, r2 + (rowH - lblH - subH) * 0.5f, cw * 2.f, subH};
        const int sync = stepper(syncR, syncId, 10, kSyncDiv, 1.f, "no sync",
                                 "Sync",
                                 "Free-running, or a division of the transport's "
                                 "tempo - the table of id 33, verbatim");
        microFit(ui_, fSmall_, {c.x, r2 + rowH - lblH, cw * 2.f, lblH}, "sync",
                 nx::muted.alpha(0.85f), Align::Center);

        Ui::KnobStyle st;
        const bool freeRun = !has(syncId) || sync == 0;
        st.lo = 0.01f; st.hi = 40.f; st.def = 2.f; st.log = true; st.fmt = "%.2f";
        if (!freeRun) st.text = kSyncDiv[clampv(sync, 0, 9)];
        knob({c.x, r1, cw, rowH}, rateId, "rate", st, freeRun ? 1.f : 0.55f);
    };
    lfoColumn(2, 1, "LFO 2", pL2Rate, pL2Sync, pL2Shape, pL2Mode, 630,
              "L2 Shape", "L2 Mode");
    lfoColumn(3, 2, "LFO 3", pL3Rate, pL3Sync, pL3Shape, pL3Mode, 640,
              "L3 Shape", "L3 Mode");

    // =======================================================================
    // M5. ENV 3 + MACROS -- ids 62..65, 94..97. One column, two sections, the
    // ENVELOPES column's own precedent: ENV3 takes the top row in the exact
    // row idiom ENV1/2 wear, the four macros take the bottom. A macro does
    // nothing until a matrix slot reads it, so the knobs sit one column away
    // from the matrix on purpose.
    // =======================================================================
    {
        const Rect c = col(4);
        const f32 y0 = sect(c, "ENV 3 / MACROS", 1.f);
        const f32 er1 = y0 + subH + gap, er2 = er1 + rowH + gap;
        envRow(c, er1, "ENV 3", pE3Attack, 2.f, 300.f, 0.f, 150.f, 5);
        rend_.hairlineH(c.x, c.right(), er2 - 2 * s);
        const f32 kw = c.w / 4.f;
        Ui::KnobStyle st;
        st.lo = 0.f; st.hi = 1.f; st.def = 0.f; st.fmt = "%.2f";
        static const char* const kMacroLbl[4] = {"macro 1", "macro 2",
                                                 "macro 3", "macro 4"};
        for (int m = 0; m < 4; ++m) {
            knob({c.x + kw * (f32)m, er2, kw, rowH}, pMacro1 + m, kMacroLbl[m], st, 1.f);
            // The grab handle sits in the cell's top-left corner, which is dead
            // space beside a round cap in every knob cell in the panel and the
            // only 10px a macro cell has to spare. Macro sources are 9..12.
            // "The only dead 10px a macro has" was the note this handle was
            // built to, and 10 x 9 plus 3 of slop is 16 x 15 -- one pixel short
            // on the axis that matters. The cap of a 51px cell is 31px wide and
            // centred, so the corner has 10 clear pixels of WIDTH and the whole
            // row height above the arc: 12 x 10 still lands entirely outside
            // the circle, and with slop it is 18 x 16.
            srcHandle({c.x + kw * (f32)m + 0.5f * s, er2 + 0.5f * s, 12 * s, 10 * s},
                      9 + m);
        }
    }

    // =======================================================================
    // M6. THE MOD MATRIX -- ids 68..93. Eight slots of source / destination /
    // bipolar depth, as two banks of four spanning the last two columns; the
    // page's standing seam between columns 5 and 6 falls exactly between the
    // banks, so the grid inherits its one divider instead of drawing one.
    //
    // A slot row is two recessed micro-selectors and a small bipolar knob --
    // the selectors are wells because they are inputs (§5), and they step
    // rather than pop a list because at 50px a list would be the only thing
    // on screen. Click steps forward, right-click back, the wheel scrubs; the
    // tooltip carries the long names. A slot whose source or destination is
    // Off contributes nothing (the contract's own words) and says so at the
    // disabled weight -- empty slots are QUIET.
    // =======================================================================
    {
        const Rect c{colX[5], body.y, kColW[5] * s + colGap + kColW[6] * s, body.h};
        const f32 y0 = sect(c, "MOD MATRIX", 1.f);
        const f32 top = y0 + 2 * s;
        const f32 rh = (c.bottom() - top) / 4.f;

        // THE THREE MIDI SOURCES LIVE IN THIS HEADER, and they live here
        // because they are the only sources in the contract with no section of
        // their own -- a wheel is not a control this panel owns, it is a thing
        // on the desk. The matrix header band is where they belong: it is the
        // one place in the panel that is ABOUT sources, it had 200 free pixels,
        // and a source you can only reach by stepping a selector is a source
        // drag-assign cannot offer. Guarded like everything else: against a v2
        // DSP whose source enum stops at Random, all three are inert and say so.
        //
        // v4 ADDS A FOURTH, AND IT IS HERE FOR A DIFFERENT REASON. Arp Step
        // (source 17) HAS a section of its own -- the arp's level row, which is
        // literally what the source reads -- and it carries a grab handle
        // there. But the arp lives on a PAGE OF ITS OWN, and a drag cannot
        // survive a page turn (the tab clears g_assign, deliberately: a gesture
        // belongs to the surface it started on). So a handle that existed only
        // on the ARP page could never be dropped on anything, because every
        // modulatable knob in this instrument is on MAIN or MOD. Two handles,
        // one source, never on screen at the same time -- so they share the id
        // 830 + 17 without ever colliding, and the wire can be made from either
        // page.
        {
            static const char* const kSrcLbl[4] = {"wheel", "bend", "cc", "arp"};
            // 54 apiece from 82 lands the last one exactly on the block's right
            // edge, which is what four have to be to fit where three were: the
            // row GREW into the space it had rather than the labels shrinking,
            // because a 12px grip beside a 38px name is already the smallest
            // either half can be.
            for (int i = 0; i < 4; ++i) {
                const Rect g{c.x + 82 * s + 54 * s * (f32)i, c.y, 54 * s, headH};
                const bool live = has(pM1Src) && (14 + i) <= srcMax;
                microFit(ui_, fSmall_, {g.x, g.y, 38 * s, g.h}, kSrcLbl[i],
                         nx::muted.alpha(live ? 0.80f : 0.35f), Align::Right, 0);
                srcHandle({g.x + 40 * s, g.y, 12 * s, g.h}, 14 + i);
            }
        }

        for (int k = 0; k < 8; ++k) {
            const int bank = k / 4;                      // 0 left, 1 right
            const Rect sr{colX[5 + bank], top + rh * (f32)(k % 4),
                          kColW[5 + bank] * s, rh};
            const int sid = pM1Src + 3 * k, did = sid + 1, aid = sid + 2;
            const int cid = pM1Curve + k;                // v3: this slot's curve
            const bool absent = !has(sid);
            const int src = absent ? 0
                : clampv((int)std::lround(get(sid, 0.f)), 0, kSrcCount - 1);
            const int dst = !has(did) ? 0
                : clampv((int)std::lround(get(did, 0.f)), 0, kDstCount - 1);
            // Quiet unless patched: both ends connected is what "in use" means.
            const f32 sdim = absent ? 0.4f : (src > 0 && dst > 0 ? 1.f : 0.62f);

            const f32 selH = std::min(subH, rh - 4 * s);
            const f32 selY = sr.y + (rh - selH) * 0.5f;
            const f32 knW  = rh - 2 * s;
            const f32 selW = (sr.w - knW - 10 * s) * 0.5f;
            const Rect srcR{sr.x + 2 * s, selY, selW, selH};
            const Rect dstR{srcR.right() + 3 * s, selY, selW, selH};
            const Rect knR{dstR.right() + 3 * s, sr.y + 1 * s, knW, rh - 2 * s};

            char wsrc[16], wdst[16], wamt[16];
            snprintf(wsrc, sizeof wsrc, "M%d source", k + 1);
            snprintf(wdst, sizeof wdst, "M%d dest", k + 1);
            snprintf(wamt, sizeof wamt, "M%d amount", k + 1);
            enumSel(700 + k, srcR, sid, kMatrixSrc, clampv(srcMax + 1, 1, kSrcCount),
                    wsrc, sdim, k);
            enumSel(720 + k, dstR, did, kMatrixDst, kDstCount, wdst, sdim, k);

            // The depth: a small bipolar knob with no readout of its own (a
            // 26px cell has no line to spare) -- the tooltip is the readout.
            Ui::KnobStyle st;
            st.lo = -1.f; st.hi = 1.f; st.def = 0.f; st.bipolar = true;
            st.arc = nx::violetSoft; st.fmt = nullptr;
            st.dim = sdim; st.absent = !has(aid);
            f32 v = has(aid) ? inst->getParam(aid) : 0.f;
            const u64 wid = uiId(UiSpectraKnob, aid, uidKey);
            // THE CURVE MOVED HOUSE, and it had to. The amount knob's
            // right-click used to cycle the slot's response curve; the widget
            // layer now spends a knob's right-click on the control menu and
            // CONSUMES the press (uw-WIDGET-API.md §2), so a hand-rolled
            // pressed[2] beside a knobNx stops firing. The gesture is not lost
            // -- it is the same right-click, one row further in, and it now
            // carries its own label instead of being a thing you had to have
            // read the tooltip to know about.
            const bool curveLive0 = has(pM1Curve + k);
            const int  curve0 = curveLive0
                ? clampv((int)std::lround(get(pM1Curve + k, 0.f)), 0, 2) : 0;
            char curveLbl[40];
            snprintf(curveLbl, sizeof curveLbl, "Response: %s -> %s",
                     kCurveName[curve0], kCurveName[(curve0 + 1) % 3]);
            Ui::MenuOffer mo;
            if (curveLive0) { mo.items |= Ui::MenuCustom; mo.custom = curveLbl; }
            if (ownTrack && has(aid)) mo.items |= Ui::MenuLearn;
            if (ui_.offer(mo).knobNx(wid, knR, &v, st)) commit(aid, v, wid, wamt);
            if (ui_.menuFired(wid, Ui::MenuCustom) && curveLive0)
                commit(pM1Curve + k, (f32)((curve0 + 1) % 3),
                       uiId(UiSpectraPanel, 860 + k, uidKey), "matrix curve");
            if (ui_.menuFired(wid, Ui::MenuLearn)) {
                if (!ownTrack)
                    status_ = "Only a track's devices can be MIDI-mapped - the "
                              "address space has no return or master scope";
                else
                    cycleMidiLearn(addr::deviceParam(ses_.tracks[devOwner_].uid,
                                   dm.uid, inst->paramInfo(aid).id));
            }

            // THE PER-SLOT CURVE (ids 101..108) COSTS NO PIXELS, and that is
            // exactly why it is here. A slot row is two selectors and a knob in
            // 138 logical pixels; a fourth control would take twenty from the
            // two selectors, which already ellipsise "Macro 1". But the amount
            // knob IS the slot's response -- the curve reshapes the source
            // before Amt multiplies it -- so the knob's RIGHT-CLICK cycles it
            // and a six-pixel glyph in the corner of the cell says which of the
            // three is on. Linear draws at the disabled weight because linear
            // is the default and the default is "nothing is happening here".
            const bool curveLive = curveLive0;
            const int  curve = curveLive
                ? clampv((int)std::lround(get(cid, 0.f)), 0, 2) : 0;
            if (curveLive) {
                const f32 gx0 = knR.x + 1 * s, gy1 = knR.bottom() - 1.5f * s;
                const f32 gw = 7 * s, gh = 5 * s, th = std::max(1.f, nx::snapPx(s));
                const Col gc = nx::muted.alpha(curve ? 0.85f : 0.30f);
                if (curve == 0) {
                    rend_.line(gx0, gy1, gx0 + gw, gy1 - gh, th, gc);
                } else if (curve == 1) {
                    rend_.line(gx0, gy1, gx0 + gw * 0.62f, gy1 - gh * 0.28f, th, gc);
                    rend_.line(gx0 + gw * 0.62f, gy1 - gh * 0.28f, gx0 + gw, gy1 - gh,
                               th, gc);
                } else {
                    rend_.line(gx0, gy1, gx0 + gw * 0.36f, gy1 - gh * 0.12f, th, gc);
                    rend_.line(gx0 + gw * 0.36f, gy1 - gh * 0.12f,
                               gx0 + gw * 0.64f, gy1 - gh * 0.88f, th, gc);
                    rend_.line(gx0 + gw * 0.64f, gy1 - gh * 0.88f, gx0 + gw, gy1 - gh,
                               th, gc);
                }
            }

            if (ui_.hovered(knR) && g_assign.src < 0) {
                char t[208];
                if (st.absent)
                    snprintf(t, sizeof t, "%s: this device has no parameter %d",
                             wamt, aid);
                else if (curveLive)
                    snprintf(t, sizeof t,
                             "%s %+.2f  (%s -> %s)  curve %s - right-click for "
                             "Response (lin x, exp x*x, S smoothstep), Reset, "
                             "Type in",
                             wamt, (double)v, kMatrixSrc[src], kMatrixDst[dst],
                             kCurveName[curve]);
                else
                    snprintf(t, sizeof t, "%s %+.2f  (%s -> %s)", wamt, (double)v,
                             kMatrixSrc[src], kMatrixDst[dst]);
                ui_.tip = t;
            }
        }
    }

    } else if (g_page == 2) {
    // =======================================================================
    // ARP -- v4's face, and the only page in this panel that is not seven
    // sections in seven columns.
    //
    // The arithmetic that decided the cut is in the file header: fourteen
    // parameters fill exactly TWO of these columns, band and both knob rows,
    // with no cell left over -- so columns 0 and 1 are the instrument and
    // columns 2..6 are the pattern, which is the feature. Same colX[], same
    // guard, same idioms; four of the six seams are left undrawn because a
    // hairline through the middle of a sixteen-step grid is structure that is
    // not there.
    //
    // ONE PAGE-WIDE RULE, and it is §5's disabled weight rather than a new
    // idea: with Arp On at 0 the contract says every id below it is READ BY
    // NOTHING, so every control here except the switch draws at 0.55 -- the
    // same weight the synced rate knob and the ignored Voices knob already
    // wear. 0.55 and not 0.40, because 0.40 is what ABSENT looks like in this
    // panel and these controls are present, they are just not doing anything.
    //
    // THE PATTERN GRID IS THE EXCEPTION and stays at full weight, because it is
    // authored BEFORE the switch is thrown as often as after -- and because
    // dimming eight hundred pixels of the page's one hero would read as broken
    // rather than as quiet.
    // =======================================================================
    const bool arpOn  = arpLive && get(pArpOn, 0.f) > 0.5f;
    const f32  adim   = (arpLive && !arpOn) ? 0.55f : 1.f;
    const int  nSteps = arpLive
        ? clampv((int)std::lround(get(pArpSteps, 16.f)), 1, kGridSteps) : kGridSteps;

    // =======================================================================
    // A1. ARP -- the switch, the note order, the pattern's dimensions
    // =======================================================================
    {
        const Rect c = col(0);
        // ARPEGGIATOR and not ARP: the first control under this header is the
        // on/off switch whose own label is "arp", and a header that repeated it
        // would make the column read "ARP / arp / on".
        const f32 y0 = sect(c, "ARPEGGIATOR", 1.f);
        // The note order gets the selector band: it is the one control that
        // decides what the arp IS, and the only one in the column whose names
        // need 112 logical pixels to be words instead of abbreviations.
        stepper({c.x, y0, c.w, subH}, pArpMode, 10, kArpMode, adim, "no arp",
                "Arp Mode",
                "The note order over the set you are holding: Up, Down, Up-Down "
                "inclusive (both ends repeated) and exclusive (played once), "
                "Down-Up, As Played, Random, Chord (every note on every step), "
                "Thumb and Pinky. Up-Down bounces INSIDE each octave in turn - "
                "the step row's octave lane is how you write the full-span "
                "bounce. Wheel steps, arrows cycle.", (int)pCountV4);

        const f32 cw = c.w / 3.f;
        const f32 r1 = y0 + subH + gap, r2 = r1 + rowH + gap;

        boolCell({c.x, r1, cw, rowH}, 900, pArpOn, "arp", "on", "off", 1.f,
                 (int)pCountV4,
                 "The switch, and the revision's bit-identity gate: at 0 the notes "
                 "you play reach the voices exactly as they did in v3 and nothing "
                 "else on this page is read at all - which is why everything else "
                 "on this page dims while it is off.");
        boolCell({c.x + cw, r1, cw, rowH}, 901, pArpHold, "hold", "held", "off",
                 adim, (int)pCountV4,
                 "Latch. On, the arp keeps playing after every key is released; a "
                 "chord STARTED FROM SILENCE replaces the latched set, and notes "
                 "added before you let go join it. Turning it off drops the latch "
                 "and the arp plays what is physically held, which may be nothing.");
        boolCell({c.x + cw * 2.f, r1, cw, rowH}, 902, pArpRetrig, "retrig",
                 "on", "free", adim, (int)pCountV4,
                 "On, a chord started from silence restarts the pattern at step 0 "
                 "on that note's own stamped sample; adding a finger to a chord "
                 "already down does not. Off is free-run - the position is a pure "
                 "function of the clock and no note-on ever moves it, which is the "
                 "setting that welds the pattern to the bar line.");

        Ui::KnobStyle st;
        st.lo = 1.f; st.hi = 4.f; st.def = 1.f; st.fmt = "%.0f";
        const Rect octCell{c.x, r2, cw, rowH};
        knob(octCell, pArpOctaves, "octs", st, adim, (int)pCountV4);
        if (arpLive && g_assign.src < 0 &&
            ui_.hovered({octCell.x, octCell.y, octCell.w, octCell.h - lblH}))
            ui_.tip = "How many octaves the cycle spans. The NOTE counter advances "
                      "first, so the arp completes one traversal of the note cycle "
                      "before the octave moves - and 1 keeps the arp in the octave "
                      "you played.";
        enumCell({c.x + cw, r2, cw, rowH}, 904, pArpOctMode, kArpOctMode, 3,
                 "oct dir", "Arp Oct Mode", adim, (int)pCountV4);
        st.lo = 1.f; st.hi = 16.f; st.def = 16.f;
        const Rect stepsCell{c.x + cw * 2.f, r2, cw, rowH};
        knob(stepsCell, pArpSteps, "steps", st, adim, (int)pCountV4);
        if (arpLive && g_assign.src < 0 &&
            ui_.hovered({stepsCell.x, stepsCell.y, stepsCell.w, stepsCell.h - lblH})) {
            char t[320];
            snprintf(t, sizeof t,
                     "Pattern length, 1..16 - now %d. It truncates the READ and "
                     "never the storage: both rows are always sixteen entries long, "
                     "so shortening and re-lengthening a pattern is lossless. The "
                     "steps past it are drawn faintly rather than blanked, and the "
                     "violet line in the grid is where the pattern loops.", nSteps);
            ui_.tip = t;
        }
    }

    // =======================================================================
    // A2. CLOCK / VELOCITY -- the step clock on the top row, what a step
    // sounds like on the bottom, with the hairline this panel uses whenever
    // one column carries two sections.
    // =======================================================================
    {
        const Rect c = col(1);
        const f32 y0 = sect(c, "CLOCK / VELOCITY", 1.f);
        const int sync = stepper({c.x, y0, c.w, subH}, pArpSync, 10, kSyncDiv, adim,
                                 "no sync", "Arp Sync",
                                 "The division table of id 33, verbatim - but read "
                                 "differently, and this is the single most "
                                 "confusable thing in v4: here the division is the "
                                 "length of ONE STEP, not of the whole cycle. It "
                                 "has to be, because Steps is variable, and a "
                                 "length knob that was secretly a tempo knob would "
                                 "not be a length knob. Index 0 is free-running and "
                                 "the rate knob owns the readout.", (int)pCountV4);

        const f32 cw = c.w / 3.f;
        const f32 r1 = y0 + subH + gap, r2 = r1 + rowH + gap;
        Ui::KnobStyle st;
        // THE RATE KNOB SWAPS ITS READOUT, exactly as the three LFO rate knobs
        // do: when the arp is synced the number in hertz is not what it is
        // doing, so the readout becomes the division and the knob drops to the
        // disabled weight. Same range, default and curve as LFO Rate, which the
        // contract says is deliberate so that this readout can be reused.
        const bool freeRun = !arpLive || sync == 0;
        st.lo = 0.01f; st.hi = 40.f; st.def = 2.f; st.log = true; st.fmt = "%.2f";
        if (!freeRun) st.text = kSyncDiv[clampv(sync, 0, 9)];
        knob({c.x, r1, cw, rowH}, pArpRate, "rate", st,
             adim * (freeRun ? 1.f : 0.55f), (int)pCountV4);
        st.text = nullptr; st.log = false;
        st.lo = 1.f; st.hi = 200.f; st.def = 50.f; st.fmt = "%.0f";
        const Rect gateCell{c.x + cw, r1, cw, rowH};
        knob(gateCell, pArpGate, "gate", st, adim, (int)pCountV4);
        if (arpLive && g_assign.src < 0 &&
            ui_.hovered({gateCell.x, gateCell.y, gateCell.w, gateCell.h - lblH}))
            ui_.tip = "Sounding length as a percentage of the NOMINAL step - swing "
                      "moves a note, it does not stretch it. Past 100% a step is "
                      "still sounding when the next one starts, which stacks "
                      "DIFFERENT notes and cleanly re-attacks the same one; under "
                      "Mono or Legato it is a glide rather than a stack.";
        st.lo = 0.f; st.hi = 100.f; st.def = 0.f;
        const Rect swCell{c.x + cw * 2.f, r1, cw, rowH};
        knob(swCell, pArpSwing, "swing", st, adim, (int)pCountV4);
        if (arpLive && g_assign.src < 0 &&
            ui_.hovered({swCell.x, swCell.y, swCell.w, swCell.h - lblH}))
            ui_.tip = "Delays odd-numbered steps by Swing/300 of a step, so 100% is "
                      "exactly a third - the 2:1 triplet feel. It counts ABSOLUTE "
                      "steps and not pattern index, so an odd-length pattern does "
                      "not flip the swing every loop. 0 selects a no-offset branch "
                      "outright and is bit-exact.";

        rend_.hairlineH(c.x, c.right(), r2 - 2 * s);
        // The selector's RETURN, not a second read of the parameter: a click
        // that changed the mode has already happened by the time enumCell
        // returns, so the Fixed knob's weight follows it on the same frame
        // rather than one behind. The sync stepper above returns its value for
        // exactly the same reason.
        const int avmode = enumCell({c.x, r2, cw, rowH}, 905, pArpVelMode,
                                    kArpVelMode, 3, "vel", "Arp Vel Mode", adim,
                                    (int)pCountV4);
        st.lo = 1.f; st.hi = 127.f; st.def = 100.f;
        // Fixed Vel is IGNORED unless Vel Mode is Fixed, and says so the way
        // every ignored control in this panel says it. Its floor is 1 and not
        // 0 because this device reads a note-on at velocity 0 as a note-off,
        // so a generated 0 would be a generated silence.
        knob({c.x + cw, r2, cw, rowH}, pArpFixedVel, "fixed", st,
             adim * (avmode == 1 ? 1.f : 0.55f), (int)pCountV4);
        st.lo = 0.f; st.hi = 100.f; st.def = 100.f;
        const Rect chCell{c.x + cw * 2.f, r2, cw, rowH};
        knob(chCell, pArpChance, "chance", st, adim, (int)pCountV4);
        if (arpLive && g_assign.src < 0 &&
            ui_.hovered({chCell.x, chCell.y, chCell.w, chCell.h - lblH}))
            ui_.tip = "The probability that a step which would sound a NEW note "
                      "actually does. A rest is already silent and a tie is not a "
                      "new note, so neither is tested; a step that loses its draw "
                      "is silent but still advances the pattern. 100% selects the "
                      "branch out entirely - no hash is computed and the render is "
                      "bit-exact.";
    }

    // =======================================================================
    // A3. THE PATTERN -- two rows over the same sixteen columns, spanning
    // columns 2..6, and the centrepiece of the whole feature.
    //
    // IT IS STATE AND NOT PARAMETERS, on the LFO grid's own terms and at the
    // same stated cost: a drawn pattern cannot be automated and cannot be a
    // matrix destination. It is shape. So it is read and written through
    // stateString()/setStateString() as the `arpl` and `arps` records, and
    // every record and every reserved BIT this build does not understand is
    // carried across untouched -- see SpecState, where that half is argued.
    //
    // WHY THE STEP ROW IS ON TOP. It answers the first question -- which steps
    // sound -- and the level row answers the second. That is the order every
    // step sequencer since the x0x has drawn them in, and FL's own step row
    // sits above its velocity graph. The two rows share one column grid, one
    // set of beat seams and one twenty-pixel lane gutter, so a step and its
    // level are the same x on screen.
    //
    // WHY THE STEP ROW IS A FIVE-LANE MINI PIANO ROLL, WHICH IS THE WHOLE
    // INTERACTION ARGUMENT:
    //
    //   A cell carries TWO edits of two different KINDS. Rest/sound/tie is a
    //   three-state CYCLE; the octave is a small ORDERED range, -2..+2. A
    //   cycle wants a click. An ordered range wants a drag along the axis it
    //   is drawn on -- and once the octave is DRAWN AS A POSITION inside the
    //   cell, that drag is direct manipulation and needs no rule at all: the
    //   block ends up where you left it, and sixteen of them read as a
    //   contour, which is "which steps jump an octave" answered by looking.
    //
    //   So the two edits are separated by the stroke's AXIS, not by a modifier:
    //     click            cycle rest -> sound -> tie -> rest
    //     drag sideways    stamp the state the press produced across every step
    //                      the stroke flies over (FL's paint, and the level
    //                      row's own stroke one row down)
    //     drag up / down   move THIS step between the five octave lanes; the
    //                      press's cycle is reverted first, so the stroke is
    //                      the one edit you meant
    //     wheel            nudge the octave by one lane, consumed
    //     right-drag       clear every step the stroke crosses to a plain rest
    //
    //   THE THREE ALTERNATIVES, and why each lost:
    //
    //   * A MODIFIER (Ctrl-drag = octave) is the cheap answer and it costs a
    //     rule nobody can see. This panel refused that argument once already,
    //     about inferring the wavetable drop target from which half of the well
    //     the pointer was over. Ctrl is also spoken for: the FL pass gave it to
    //     FINE on every knob and trough in the program, and a modifier meaning
    //     "precision" on one control and "a different parameter" on the one
    //     beside it is worse than no modifier at all.
    //   * A SECOND SUB-ROW halves the target. At DPI 1.0 a cell here is 50 x 48
    //     logical pixels; split it and you get 50 x 32 and 50 x 16, and the
    //     sixteen of them are shoulder to shoulder in BOTH axes -- so hit slop
    //     cannot make up the difference without a cell stealing its neighbour's
    //     face, which is exactly what widgets.h warns about.
    //   * A SEPARATE OCTAVE STRIP under the grid is that cost plus a third row
    //     in a 200-pixel dock, and it breaks the property that was actually
    //     asked for: the two facts about a step would be in two places, so no
    //     single glance could answer both.
    //
    //   WHAT THE AXIS RULE COSTS, said out loud rather than left to be found:
    //   a stroke meant to paint that starts with a wobble can lock vertical.
    //   The lock needs four logical pixels and HORIZONTAL WINS A TIE -- paint
    //   is the common stroke and a paint that became a pitch edit is the
    //   expensive mistake, where the reverse is a nudge you can see. And the
    //   whole stroke is ONE undo entry, so a mislock is one Ctrl+Z.
    //
    // A TIE IS DRAWN AT THE LANE OF THE NOTE IT HOLDS, never at its own stored
    // octave: the contract says a tie's octave is ignored because there is no
    // new note to offset, so drawing it anywhere else would put a number on
    // screen the instrument does not read. A run of ties therefore draws as ONE
    // long bar. A tie with nothing to hold is drawn hollow and AMBER, because
    // the contract says it is silent and amber is this program's word for
    // refused -- the grid tells you the pattern is wrong before you play it.
    // =======================================================================
    {
        const Rect c{colX[2], body.y, colX[6] + kColW[6] * s - colX[2], body.h};
        const f32 y0 = sect(c, "ARP PATTERN", 1.f);

        // The source handle, in the matrix header's own idiom -- a right-aligned
        // name and the grip beside it -- because that is how every draggable
        // source in this panel says what it is. Its twin is in the matrix header
        // on the MOD page; see the note there for why one source needs two.
        {
            const bool srcLive = has(pM1Src) && kSrcArpStep <= srcMax;
            const Rect hr{c.right() - 12 * s, c.y, 12 * s, headH};
            microFit(ui_, fSmall_, {hr.x - 62 * s, c.y, 60 * s, headH}, "arp step",
                     nx::muted.alpha(srcLive ? 0.80f : 0.35f), Align::Right, 0);
            srcHandle(hr, kSrcArpStep);
        }

        const Rect blk{c.x, y0, c.w, std::max(32 * s, c.bottom() - y0)};
        const bool editable = stateOk && arpLive;

        // THE LANE GUTTER, twenty logical pixels out of eight hundred. Both rows
        // give it up so their sixteen columns still line up exactly, and it is
        // what makes the two rows self-labelling: "+2 / 0 / -2" beside the step
        // row says what the five lanes are without a legend parked somewhere
        // else, and "lvl" beside the other says which row is which.
        const f32  gut   = 26 * s;
        // THE STEP ROW TAKES THE LARGER SHARE, and the first driven shot is
        // why. The level row's default is sixteen digits of `f` -- a full-height
        // bar on every step -- so at an even split the pattern read as a slab of
        // violet with a thin strip of steps above it, which is the wrong way
        // round for a control whose first question is "which steps sound". The
        // step row also has five lanes to spend its height on and the level row
        // has one axis; 56/44 gives the lanes 13 pixels each instead of 9.
        const f32  stepH = clampv(blk.h * 0.56f, 30 * s, 74 * s);
        const Rect sRow{blk.x, blk.y, blk.w, stepH};
        const Rect lRow{blk.x, sRow.bottom() + 3 * s, blk.w,
                        std::max(14 * s, blk.bottom() - sRow.bottom() - 3 * s)};
        rend_.well(sRow, nx::radiusXs * s, true);
        rend_.well(lRow, nx::radiusXs * s, true);
        const Rect sIn = sRow.insetXY(2 * s, 2 * s);
        const Rect lIn = lRow.insetXY(2 * s, 2 * s);
        const Rect pS{sIn.x + gut, sIn.y, std::max(16 * s, sIn.w - gut), sIn.h};
        const Rect pL{lIn.x + gut, lIn.y, std::max(16 * s, lIn.w - gut), lIn.h};
        const f32  cwS   = pS.w / (f32)kGridSteps;
        const f32  laneH = pS.h / 5.f;
        const f32  th1   = std::max(1.f, nx::snapPx(s));
        const auto laneY  = [&](int lane) { return pS.y + laneH * ((f32)lane + 0.5f); };
        const auto laneOf = [&](int oct)  { return 2 - clampv(oct, -2, 2); };
        const auto stepAtS = [&](f32 x) {
            return clampv((int)std::floor((x - pS.x) / std::max(1e-3f, cwS)),
                          0, kGridSteps - 1);
        };
        const auto laneAtY = [&](f32 y) {
            return clampv((int)std::floor((y - pS.y) / std::max(1e-3f, laneH)), 0, 4);
        };
        const int hoverStep = stepAtS(in.mx);

        // --- the step row's stroke -----------------------------------------
        const u64 swid = uiId(UiSpectraPanel, 911, uidKey);
        bool sHot = false, sPressed = false, sErased = false;
        if (editable && ui_.setHot(swid, sRow) && ui_.isHot(swid)) {
            sHot = true;
            ui_.cursor = Cursor::Hand;
            ui_.badge = (in.down[2] || in.pressed[2]) ? Badge::Delete : Badge::Draw;
            if (in.pressed[2]) {
                g_arpDrag.erase = true;
                g_arpDrag.eraseStep = hoverStep;
                sErased = true;
            }
            if (in.pressed[0]) {
                ui_.active = swid;               // so one stroke is one undo entry
                g_arpDrag.active = true;
                g_arpDrag.axis = 0;
                g_arpDrag.cell = hoverStep;
                g_arpDrag.last = hoverStep;
                g_arpDrag.px = in.mx;
                g_arpDrag.py = in.my;
                g_arpDrag.raw0 = sstate.arpRaw[hoverStep];
                // THE PRESS CYCLES ON THE FRAME IT HAPPENED, for two reasons and
                // both are about honesty: a click that waited for its release
                // would be a control with no feedback under the finger, and the
                // paint stroke needs a VALUE to stamp -- which is exactly "the
                // state this click produced". A stroke that turns out to be
                // vertical puts this back before it touches the octave, and both
                // edits ride ONE undo entry, so the cycle nobody meant never
                // survives the gesture.
                const int next = !sstate.arpOn(hoverStep) ? 1
                               : (!sstate.arpTie(hoverStep) ? 2 : 0);
                g_arpDrag.paint = next;
                sstate.arpSet(hoverStep, next != 0, next == 2,
                              sstate.arpOct(hoverStep));
                writeState("arp step");
                sPressed = true;
            }
            // The wheel is the precise route to the octave and the cheap one:
            // one notch, one lane, consumed. It names its own gesture id because
            // `active` is dropped on any frame the left button is not held, and
            // a nudge that leaned on it would take one undo entry per notch.
            if (in.wheel != 0.f && !g_arpDrag.active) {
                const int d = in.wheel > 0.f ? +1 : -1;
                in.wheel = 0.f;                  // not the strip's notch to spend
                if (!sstate.arpOn(hoverStep))
                    ui_.refusal = "That step is a rest - click it to place a note "
                                  "before offsetting one";
                else if (sstate.arpTie(hoverStep))
                    ui_.refusal = "A tie holds the previous note, so it has no "
                                  "octave of its own - the contract ignores it";
                else {
                    const int o = clampv(sstate.arpOct(hoverStep) + d, -2, 2);
                    if (o != sstate.arpOct(hoverStep)) {
                        sstate.arpSet(hoverStep, true, false, o);
                        writeState("arp step octave", swid);
                    }
                }
            }
        }

        // The erase sweep, the level row's verb on the other row: FL's
        // right-drag clears everything the stroke flies over, in one undo entry
        // however many steps it crossed.
        if (g_arpDrag.erase) {
            if (in.down[2] || sErased) {
                const int s1 = stepAtS(in.mx);
                bool changed = false;
                for (int i = std::min(g_arpDrag.eraseStep, s1);
                     i <= std::max(g_arpDrag.eraseStep, s1); ++i) {
                    // A cleared step is a PLAIN rest: no note, no tie, no offset.
                    // The reserved bits are the one thing that survives, for the
                    // reason SpecState gives.
                    const int want = (sstate.arpRaw[i] & 0xE0) | 0x04;
                    if (sstate.arpRaw[i] != want) {
                        sstate.arpRaw[i] = want;
                        changed = true;
                    }
                }
                g_arpDrag.eraseStep = s1;
                if (changed) writeState("clear arp steps", swid);
            }
            if (!in.down[2]) g_arpDrag.erase = false;
        }
        if (g_arpDrag.active) {
            if (in.down[0] || sPressed) {
                // THE AXIS LOCK. Four logical pixels, decided once per stroke and
                // never revisited -- an axis that could flip mid-stroke is a
                // gesture that changes verb under your hand.
                const f32 lock = 4.f * s;
                if (g_arpDrag.axis == 0) {
                    if (std::fabs(in.mx - g_arpDrag.px) >= lock)      g_arpDrag.axis = 1;
                    else if (std::fabs(in.my - g_arpDrag.py) >= lock) g_arpDrag.axis = 2;
                }
                bool changed = false;
                if (g_arpDrag.axis == 1) {
                    const int s1 = stepAtS(in.mx);
                    for (int i = std::min(g_arpDrag.last, s1);
                         i <= std::max(g_arpDrag.last, s1); ++i) {
                        // Painting is a RHYTHM edit and each step keeps its OWN
                        // octave: the stroke said nothing about pitch, so it
                        // writes nothing about pitch.
                        const int before = sstate.arpRaw[i];
                        sstate.arpSet(i, g_arpDrag.paint != 0, g_arpDrag.paint == 2,
                                      sstate.arpOct(i));
                        if (sstate.arpRaw[i] != before) changed = true;
                    }
                    g_arpDrag.last = s1;
                } else if (g_arpDrag.axis == 2) {
                    // THE REVERT AND THE SET ARE ONE STEP, tested against the
                    // NET result: putting the press's cycle back and then
                    // writing the octave would otherwise report a change on
                    // every frame of a stroke that had not moved, and every
                    // frame of a mouse held still would be a state write.
                    const int i = g_arpDrag.cell;
                    const int before = sstate.arpRaw[i];
                    sstate.arpRaw[i] = g_arpDrag.raw0;       // the press's cycle, undone
                    if (sstate.arpTie(i))
                        ui_.refusal = "A tie holds the previous note, so it has no "
                                      "octave of its own - the contract ignores it";
                    else
                        // A vertical drag on a REST places a note at that lane,
                        // which is what the gesture looks like it is doing.
                        // Refusing it would be a rule nobody can see.
                        sstate.arpSet(i, true, false, 2 - laneAtY(in.my));
                    if (sstate.arpRaw[i] != before) changed = true;
                }
                if (changed) writeState("arp steps");
            }
            if (!in.down[0]) { g_arpDrag.active = false; g_arpDrag.axis = 0; }
        }

        // --- the step row, drawn -------------------------------------------
        {
            const Col gc = nx::muted.alpha(editable ? 0.50f : 0.28f);
            const f32 gx = sIn.x, gw2 = std::max(4 * s, gut - 4 * s);
            // Three of the five lanes are labelled and not five: at twelve
            // pixels a glyph and nine a lane, five would collide -- and +2 / 0 /
            // -2 is enough to read the other two off.
            microFit(ui_, fSmall_, {gx, laneY(0) - 5.5f * s, gw2, 11 * s}, "+2",
                     gc, Align::Right, 0);
            microFit(ui_, fSmall_, {gx, laneY(2) - 5.5f * s, gw2, 11 * s}, "0",
                     gc, Align::Right, 0);
            microFit(ui_, fSmall_, {gx, laneY(4) - 5.5f * s, gw2, 11 * s}, "-2",
                     gc, Align::Right, 0);
        }
        // The lanes themselves are QUIET AT REST: only the zero lane is drawn,
        // because that is the one a reader needs to tell +1 from -1. All five
        // appear while an octave stroke is actually in flight -- the control
        // showing its own scale exactly while it is being used, and never on a
        // timer.
        {
            const bool octLive = g_arpDrag.active && g_arpDrag.axis == 2;
            for (int lane = 0; lane < 5; ++lane) {
                if (lane != 2 && !octLive) continue;
                rend_.hairlineH(pS.x + 2 * s, pS.right() - 2 * s,
                                std::round(laneY(lane)),
                                nx::line.alpha(lane == 2 ? 0.55f : 0.42f));
            }
        }
        for (int i = 1; i < kGridSteps; ++i)
            if (i % 4 == 0)
                rend_.hairlineV(std::round(pS.x + cwS * (f32)i), sRow.y + 2 * s,
                                sRow.bottom() - 2 * s);
        {
            const f32 blockH = clampv(laneH - 3.f * s, 3.f * s, 11.f * s);
            int heldLane = -1;                  // the lane of the note a tie holds
            for (int i = 0; i < kGridSteps; ++i) {
                const f32 a = (editable ? 1.f : 0.55f) * (i >= nSteps ? 0.30f : 1.f);
                // FOUR PIXELS OF AIR EACH SIDE, and not one. A row of sixteen
                // blocks all sitting on the zero lane -- which is what a fresh
                // pattern IS -- merges into a single horizontal line at a 1px
                // gap, and a step sequencer whose sixteen steps read as one bar
                // is a step sequencer nobody can count. The blocks are 42 wide
                // in a 50-wide cell; the hit cell is still the whole 50.
                const f32 bx = pS.x + cwS * (f32)i + 4.f * s;
                const f32 bw = std::max(2.f, cwS - 8.f * s);
                if (!sstate.arpOn(i)) {
                    // A REST IS DRAWN, faintly, on the zero lane: an empty cell
                    // and a cell that is not there would otherwise be one picture.
                    rend_.rect({bx, std::round(laneY(2)) - th1 * 0.5f, bw, th1},
                               nx::line.alpha(0.55f * a));
                    heldLane = -1;              // a rest ends the note it followed
                    continue;
                }
                if (sstate.arpTie(i)) {
                    if (heldLane < 0) {
                        const Rect b{bx, laneY(2) - blockH * 0.5f, bw, blockH};
                        rend_.roundRectOutline(b, nx::radiusXs * s, th1,
                                               nx::amber.alpha(0.80f * a));
                    } else {
                        // Bridged LEFT across the seam, so a run of ties is one
                        // bar rather than a row of separate blocks.
                        const Rect b{bx - 8.f * s, laneY(heldLane) - blockH * 0.5f,
                                     bw + 8.f * s, blockH};
                        // NO CYAN CAP, and that is the tell rather than a
                        // shade: §1's lamp marks the edge that MOVES, and a tie
                        // has no attack and no octave of its own to move. So a
                        // sounding step wears the lamp, a held one does not, and
                        // the two are told apart at a glance by light and by the
                        // seam they do or do not close.
                        rend_.rect(b, nx::violet.alpha(0.34f * a));
                        rend_.rect({b.x, b.y, b.w, th1}, nx::violet.alpha(0.75f * a));
                        rend_.rect({b.x, b.bottom() - th1, b.w, th1},
                                   nx::violet.alpha(0.75f * a));
                    }
                    continue;
                }
                const int lane = laneOf(sstate.arpOct(i));
                const Rect b{bx, laneY(lane) - blockH * 0.5f, bw, blockH};
                rend_.rect(b, nx::violet.alpha(0.62f * a));
                // §1's one lamp again: the lit edge is the top of the block,
                // which is the edge that moves when the octave changes.
                rend_.rect({b.x, b.y, b.w, th1}, nx::cyan.alpha(0.85f * a));
                heldLane = lane;
            }
        }

        // --- the level row -------------------------------------------------
        microFit(ui_, fSmall_, {lIn.x, lRow.cy() - 5.5f * s,
                                std::max(4 * s, gut - 4 * s), 11 * s}, "lvl",
                 nx::muted.alpha(editable ? 0.50f : 0.28f), Align::Right, 0);
        const u64 lwid = uiId(UiSpectraPanel, 910, uidKey);
        const LevelHit alh = levelRowGesture(sstate.arpLvl, lRow, pL, lwid, 3,
                                             editable, "draw arp levels",
                                             "erase arp levels");
        levelRowBars(sstate.arpLvl, lRow, pL, editable, nSteps);

        // WHERE THE PATTERN LOOPS, on both rows, because Steps is a control two
        // columns away and a number in a knob cell is not an answer to "what is
        // this grid doing".
        if (nSteps < kGridSteps) {
            const f32 mx = std::round(pS.x + cwS * (f32)nSteps);
            rend_.rect({mx - th1 * 0.5f, sRow.y + 2 * s, th1, sRow.h - 4 * s},
                       nx::violet.alpha(0.75f));
            rend_.rect({mx - th1 * 0.5f, lRow.y + 2 * s, th1, lRow.h - 4 * s},
                       nx::violet.alpha(0.75f));
        }

        // --- the sentences -------------------------------------------------
        if (sHot) {
            const char* what = !sstate.arpOn(hoverStep) ? "rest"
                             : sstate.arpTie(hoverStep) ? "tie" : "sound";
            char oct[32] = {};
            if (sstate.arpTie(hoverStep))
                snprintf(oct, sizeof oct, ", held over");
            else if (sstate.arpOn(hoverStep))
                snprintf(oct, sizeof oct, ", octave %+d", sstate.arpOct(hoverStep));
            char t[320];
            snprintf(t, sizeof t,
                     "Step %d of 16: %s%s. Click cycles rest - sound - tie; drag "
                     "SIDEWAYS paints that state across the steps you cross; drag "
                     "UP/DOWN moves this step between the five octave lanes "
                     "(-2..+2); the wheel nudges it; right-DRAG clears to rests.",
                     hoverStep + 1, what, oct);
            ui_.tip = t;
        }
        if (alh.hot) {
            char t[320];
            snprintf(t, sizeof t,
                     "Arp level row, step %d of 16 at %.2f: drag to paint, "
                     "shift-drag for a line, right-DRAG to erase, wheel nudges. "
                     "It is the velocity when Vel Mode is Pattern AND it is matrix "
                     "source 17 (Arp Step) whenever the arp is on - so unlike an "
                     "LFO grid it is never dead, whatever Vel Mode says.",
                     alh.step + 1, (double)alh.level / 15.0);
            ui_.tip = t;
        }
        if (!editable && (ui_.hovered(sRow) || ui_.hovered(lRow))) {
            char t[320];
            if (!arpLive)
                snprintf(t, sizeof t,
                         "The arp pattern is v4 STATE - the arpl and arps records "
                         "of the state string, not parameters, so it cannot be "
                         "automated and cannot be a matrix destination. This DSP "
                         "has %d of %d parameters, so the arp block (109..124) is "
                         "not there yet and the grid is inert.", pc, (int)pCountV4);
            else
                snprintf(t, sizeof t,
                         "This device's state string does not parse - the grid is "
                         "read-only rather than overwrite what it could not read");
            ui_.tip = t;
        }
    }

    } else {
    // =======================================================================
    // DRAW -- v5's face: the wavetable editor, and the first surface in this
    // panel whose quality is a function of the dock height.
    //
    // THE CUT. Columns 0 and 1 are the editor's controls -- six rows of
    // sixteen logical pixels, which is exactly what a 130px body holds at the
    // dock's default -- and columns 2..6 are the frame strip and the canvas.
    // Same colX[] as the other three pages, so the bones stand still when the
    // page turns; one seam drawn, for the reason the seam loop gives.
    //
    // WHAT IS AND IS NOT IDENTITY, restated here because every control below
    // depends on it. The working copy is 32 x 2048 floats that live in this
    // file. Drawing writes them. A PREVIEW publishes them into the engine's
    // recycled arena and touches no hash, no file and no state record -- what
    // stateString() names does not move. A COMMIT is the nine-step
    // canonicalisation on the far side of commitFrames(), and it is the only
    // thing here that makes a table. That split exists because
    // spBuildCustomMips() allocates 1.31 MB per distinct hash into a store
    // that is never freed and is capped at 32: an editor that committed per
    // stroke would be unusable in a minute. The user-visible cost is that a
    // project saved mid-edit does not contain the drawing -- which is why both
    // exits from this page ASK.
    //
    // THE 2048 SAMPLES ARE THE CANONICAL REPRESENTATION. Both pens read them
    // and both pens write them; the harmonic view is DERIVED and is discarded
    // the moment a stroke touches the time domain.
    // =======================================================================
    SpectraEdit& e = g_edit;

    // The guard, made once, in three parts -- and the FIRST is a compile-time
    // question about this build's host.h, which is why it is a constant and not
    // a call. See the concepts above.
    constexpr bool weContract = kWtEditor<PluginInstance>;
    const bool weDevice = wtSupported(inst);
    const bool weLive   = weContract && weDevice;

    // ---- geometry ---------------------------------------------------------
    const Rect ctl{colX[0], body.y, kColW[0] * s + colGap + kColW[1] * s, body.h};
    const Rect can{colX[2], body.y,
                   colX[kCols - 1] + kColW[kCols - 1] * s - colX[2], body.h};
    const f32  cy0 = sect(ctl, "WAVETABLE EDITOR", 1.f);
    const int  kRows = 6;
    f32 rgap = (ctl.bottom() - cy0 - (f32)kRows * subH) / (f32)(kRows - 1);
    rgap = clampv(rgap, 2.f * s, 6.f * s);
    const auto rowY = [&](int i) { return cy0 + (subH + rgap) * (f32)i; };
    const Rect stripR{can.x, can.y, can.w, subH};
    // THE CANVAS TAKES EVERYTHING THAT IS LEFT, and everything the splitter
    // ever hands over. It is the only rect in this panel with no fixed height.
    const Rect canR{can.x, stripR.bottom() + gap, can.w,
                    std::max(24 * s, can.bottom() - stripR.bottom() - gap)};

    // ---- opening ----------------------------------------------------------
    //
    // The editor reads the oscillator's RESOLVED table, stretched to 32 frames
    // by the seam, and says what it got. readFrames() answers false for an
    // oscillator with no custom table -- a factory table is not readable
    // through this contract and the eight of them are not the editor's to edit
    // -- and that case is not an error: it is a BLANK 32-frame table, which is
    // what "draw a wavetable from nothing" has to start from and is the
    // premise of the whole revision.
    const auto weOpen = [&](int osc, const char* why) {
        e.release();
        e.alloc();
        e.open = true;
        e.uid  = dm.uid;
        e.osc  = clampv(osc, 0, 1);
        e.fromDevice = weLive && wtReadFrames(inst, e.osc, e.f.data());
        e.srcFrames  = weLive ? wtFrames(inst, e.osc) : 0;
        e.stretched  = e.fromDevice && e.srcFrames > 0 && e.srcFrames < kWeFrames;
        e.was = e.f;
        if (g_weSeedView  >= 0) { e.view = g_weSeedView;  g_weSeedView  = -1; }
        if (g_weSeedFrame >= 0) { e.cur  = g_weSeedFrame; g_weSeedFrame = -1; }
        // THE NAME FIELD OPENS EMPTY, ALWAYS, and that is not laziness. After
        // v5's widening customName() answers the wtname record, then
        // basename(wtpath), then the bare hash -- and this side cannot tell
        // which of the three it got. Seeding the field from it and committing
        // would turn a FILENAME into a display name the user never typed.
        // Empty means absence, absence is the one spelling of "no name", and
        // the placeholder beside the field says what the table is called now.
        e.name.clear();
        if (e.stretched) {
            char t[176];
            snprintf(t, sizeof t,
                     "opened %d source frames, stretched to 32 - committing "
                     "will make a DIFFERENT hash, because the content differs",
                     e.srcFrames);
            e.say(t, 2);
        } else if (e.fromDevice) {
            e.say("opened this oscillator's custom table - 32 frames", 1);
        } else if (weLive) {
            e.say("blank: this oscillator has no custom table to read", 0);
        }
        if (why) LOGI("NXTAKT_DEBUG_PROBE: spectra editor opened osc %s (%s), "
                      "readFrames=%s", e.osc ? "B" : "A", why,
                      e.fromDevice ? "true" : "false");
    };
    if (!e.open || e.uid != dm.uid) weOpen(g_wt.osc, "entered the DRAW page");

    // NOT const: the frame strip is drawn between the operations and the
    // canvas, and a click on it moves the cursor THIS frame. Without the
    // refresh below, the pen would spend one frame writing into the frame the
    // cursor had just left -- a one-frame window, but the kind that turns into
    // "sometimes my stroke lands on the wrong frame".
    int        curF = clampv(e.cur, 0, kWeFrames - 1);
    const bool wave = e.view == 0;

    // ---- the arithmetic the page's verbs are made of ----------------------
    const auto weMoveCursor = [&](int to, const char* how) {
        const int n = clampv(to, 0, kWeFrames - 1);      // NO WRAP at either end
        if (n == e.cur) return;
        e.cur = n;
        e.specOk = false;                                // the held spectrum is
        e.stroke = 0;                                    // this frame's, only
        if (probeOn())
            LOGI("NXTAKT_DEBUG_PROBE: spectra editor frame %d (%s)", n, how);
    };

    // The set-wide peak and what the commit's one scalar would be. This is the
    // whole of what normalisation has to show: it changes no shape and no
    // inter-frame relationship, so there is nothing to watch except a level.
    const f32 wePk = e.open ? e.peak() : 0.f;

    const auto wePreview = [&](const char* why) {
        if (!weLive) return;
        // THE INTERVAL IS COMPUTED FROM WHAT prepare() WAS GIVEN, not hard-
        // coded: max(50 ms, 2 x maxBlock / sampleRate). The recycle proof is
        // four buffers against one block period, and the driver's own numbers
        // are what the engine was prepared with.
        const f64 sr = eng_.driverSampleRate() > 0.0 ? eng_.driverSampleRate() : 48000.0;
        const f64 bl = eng_.driverBufferSize() > 0 ? (f64)eng_.driverBufferSize() : 256.0;
        const f64 iv = std::max(0.050, 2.0 * bl / sr);
        const f64 t  = nowSeconds();
        if (t - e.prevAt < iv) {
            char m[144];
            snprintf(m, sizeof m, "preview held - the minimum interval is %.0f ms",
                     iv * 1000.0);
            e.say(m, 2);
            return;
        }
        e.prevAt = t;
        if (wtPreviewFrames(inst, e.osc, e.f.data())) {
            // A SUCCESSFUL PREVIEW SAYS NOTHING HERE, deliberately. The one
            // sentence slot on the canvas belongs to whatever the user just
            // DID -- and an operation that previews as a side effect would
            // otherwise erase its own explanation, which is how "the fill
            // re-phased, the endpoints did not" stopped being on screen the
            // first time this was driven. Being previewed is a STATE, not an
            // event, so it is a lamp on the PREVIEW button and a word in the
            // footer instead.
            e.previewing = true;
            if (probeOn())
                LOGI("NXTAKT_DEBUG_PROBE: spectra editor preview osc %s (%s)",
                     e.osc ? "B" : "A", why ? why : "edit");
        } else {
            e.say("this device refused the preview", 2);
        }
    };

    // The path the commit wrote, read back off the record the commit rewrote.
    // The contract puts the sentence in the editor -- "the editor reports
    // 'Saved to <path>' with the real path, once, on every commit" -- and the
    // path is not a return value, so it comes from wtpathA/wtpathB, which
    // step 9 has just made point at the drawn file.
    const auto weSavedPath = [&](int osc) {
        SpecState after;
        if (!after.parse(inst->stateString())) return std::string();
        const std::string* p = after.find(osc ? "wtpathB" : "wtpathA");
        return p ? weUnescape(*p) : std::string();
    };

    const auto weCommit = [&]() {
        if (!weLive) {
            status_ = weContract
                ? "Spectra: this device answers no wavetable() - there is "
                  "nothing to commit a drawing to"
                : "Spectra: this build's plugin contract has no "
                  "WavetableControl::commitFrames - the editor cannot save";
            e.say("commit refused: the contract is not here", 2);
            return false;
        }
        // Step 1 of the nine, on this side too, because the sentence is better
        // here: a non-finite sample is a drawing the user can still fix.
        if (!e.allFinite()) {
            status_ = "Spectra: a frame holds a non-finite sample - the whole "
                      "commit is refused and nothing changed";
            e.say("commit refused: a sample is not finite", 2);
            return false;
        }
        // ...and step 4, for the same reason. The import path maps a zero peak
        // to a gain of 1 and carries on; a DRAWING of silence is a mistake and
        // letting it through would burn an identity on it forever.
        if (wePk <= kWeSilentPk) {
            status_ = "Spectra: this table is silent - a drawing of nothing is "
                      "refused rather than given an identity forever";
            e.say("commit refused: this table is silent", 2);
            return false;
        }
        undoPoint("commit wavetable");
        if (!wtCommitFrames(inst, e.osc, e.f.data(),
                            e.name.empty() ? nullptr : e.name.c_str())) {
            g_wt.err = wtError(inst);
            status_ = std::string("Spectra: the commit was refused - ") + g_wt.err;
            e.say("commit refused - nothing changed", 2);
            return false;
        }
        g_wt.err.clear();
        // A COMMIT THAT DOES NOT MOVE THE OSCILLATOR ONTO ITS CUSTOM SLOT is a
        // saved table nobody can hear -- the import drop's own rule, and one
        // edit to the user, so it rides the commit's undo point rather than
        // taking a second.
        const int tid = e.osc ? pBTable : pATable;
        if (has(tid) && enumMax(tid, 7) >= 8 &&
            std::lround(get(tid, 0.f)) != 8) {
            inst->setParam(tid, 8.f);
            if (ownTrack)
                autoCapture(addr::deviceParam(ses_.tracks[devOwner_].uid, dm.uid,
                                              inst->paramInfo(tid).id), 8.f, 0);
        }
        // The committed table is the NORMALISED one, and the editor now shows
        // exactly that. Re-reading is how "nothing you drew moved, the level
        // did" becomes something on screen rather than a claim.
        if (!wtReadFrames(inst, e.osc, e.f.data())) { /* keep what we sent */ }
        e.was = e.f;
        e.specOk = false;
        e.previewing = false;            // the committed table IS what plays now
        e.savedPath = weSavedPath(e.osc);
        char m[400];
        if (!e.savedPath.empty())
            snprintf(m, sizeof m, "Spectra: saved to %s", e.savedPath.c_str());
        else
            snprintf(m, sizeof m,
                     "Spectra: committed - the device did not report a path");
        status_ = m;
        e.say(e.savedPath.empty() ? "committed"
                                  : "committed and saved - see the status bar", 3);
        if (probeOn())
            LOGI("NXTAKT_DEBUG_PROBE: spectra editor commit osc %s name=\"%s\" "
                 "path=%s", e.osc ? "B" : "A", e.name.c_str(),
                 e.savedPath.empty() ? "(none)" : e.savedPath.c_str());
        return true;
    };

    // ---- the ask, resolved at the very end of the block --------------------
    int weAnswer = -1;                   // 0 stay, 1 discard, 2 commit and go

    // =======================================================================
    // ROW 0 -- who is being edited, which pen, and the dock
    // =======================================================================
    {
        const Rect r0{ctl.x, rowY(0), ctl.w, subH};
        const f32 tgW = 16 * s;
        const Rect oscR{r0.x, r0.y, tgW * 2.f, r0.h};
        const Rect viewR{oscR.right() + 6 * s, r0.y, 92 * s, r0.h};
        const Rect tallR{r0.right() - 48 * s, r0.y, 48 * s, r0.h};
        const Rect chipR{viewR.right() + 6 * s, r0.y,
                         std::max(8 * s, tallR.x - viewR.right() - 12 * s), r0.h};

        // --- A / B, the hero's own pair, on the hero's own terms ------------
        ui_.segCluster(oscR);
        for (int k = 0; k < 2; ++k) {
            const Rect seg{oscR.x + tgW * (f32)k, oscR.y, tgW, oscR.h};
            if (k) rend_.hairlineV(seg.x, oscR.y + 2 * s, oscR.bottom() - 2 * s);
            Rect g = seg;
            if (weLive) {
                const u64 wid = uiId(UiSpectraPanel, 1002 + k, uidKey);
                if (ui_.grab(slop(seg)).segButton(wid, seg, k == e.osc, nx::violet) &&
                    k != e.osc) {
                    // Switching oscillators throws the working copy away, so it
                    // is one of the three exits the contract makes ask.
                    if (e.anyDirty()) { e.askWhat = 3; e.askArg = k; }
                    else              { weOpen(k, "target switched"); g_wt.osc = k; }
                }
                g = ui_.lastRect;
            }
            ui_.microIn(fSmall_, g, k ? "B" : "A",
                        k == e.osc && weLive ? nx::text
                                             : nx::muted.alpha(weLive ? 0.85f : 0.40f),
                        Align::Center);
        }
        if (ui_.hovered(oscR))
            ui_.tip = "Which oscillator's table the pens are editing. Switching "
                      "reads the other one and throws this drawing away, so it "
                      "asks first.";

        // --- WAVE / HARM ---------------------------------------------------
        //
        // A SEGMENTED PAIR AND NOT A TAB PILL: a tab pill is §5's page idiom
        // and this is not a page, it is two views of ONE frame. The
        // distinction matters because opening the harmonic view NEVER modifies
        // the frame -- switching is free, and a control that looks like a page
        // turn would suggest it costs something.
        ui_.segCluster(viewR);
        {
            static const char* const kViews[2] = {"WAVE", "HARM"};
            const f32 vw = viewR.w * 0.5f;
            for (int k = 0; k < 2; ++k) {
                const Rect seg{viewR.x + vw * (f32)k, viewR.y, vw, viewR.h};
                if (k) rend_.hairlineV(seg.x, viewR.y + 2 * s, viewR.bottom() - 2 * s);
                Rect g = seg;
                if (weLive) {
                    const u64 wid = uiId(UiSpectraPanel, 1000 + k, uidKey);
                    if (ui_.grab(slop(seg)).segButton(wid, seg, k == e.view, nx::violet) &&
                        k != e.view) {
                        e.view = k;
                        e.stroke = 0;
                        if (k == 1) {
                            // The analysis, and the sentence that says it cost
                            // the frame nothing. This is the round trip's first
                            // rule made visible at the moment it applies.
                            const int before = e.nAnalyse;
                            weEnsureSpectrum(e);
                            e.say(e.nAnalyse > before
                                      ? "analysed - the frame itself is unchanged"
                                      : "the held spectrum is still this frame's", 1);
                        } else {
                            e.say("waveform view - the analysis is still held", 1);
                        }
                    }
                    g = ui_.lastRect;
                }
                ui_.microIn(fSmall_, g, kViews[k],
                            k == e.view && weLive ? nx::text
                                                  : nx::muted.alpha(weLive ? 0.85f : 0.40f),
                            Align::Center);
            }
        }
        if (ui_.hovered(viewR))
            ui_.tip = "WAVE draws the 2048 samples freehand; HARM draws 256 "
                      "harmonic magnitudes in dB. Opening HARM never modifies "
                      "the frame - the view is a forward transform and nothing "
                      "else. Touching a bar rewrites that harmonic's phase to "
                      "sine; a WAVE stroke drops the analysis, because a "
                      "time-domain stroke touches every harmonic.";

        // --- THE ROUND-TRIP CHIP, which is this page's honesty in 100px -----
        //
        // Three states and each one names itself, because the contract's whole
        // lossiness statement is three sentences and a user has to be able to
        // see which of them has happened.
        {
            char t[64];
            Col ink = nx::muted.alpha(0.55f);
            // Short enough to fit a 100px chip at DPI 1.0 -- chip() has no
            // ellipsis logic, so a long string here is a string cut in half by
            // the panel's own clip. The sentence lives in the tooltip.
            if (!weLive)                       snprintf(t, sizeof t, "INERT");
            else if (!e.specOk)                snprintf(t, sizeof t, "NO ANALYSIS");
            else if (e.specFrame != e.cur)     snprintf(t, sizeof t, "STALE");
            else if (e.nTouched > 0) {
                snprintf(t, sizeof t, "PHASE %d/256", e.nTouched);
                ink = nx::violet;
            } else { snprintf(t, sizeof t, "UNMODIFIED"); ink = nx::cyan.alpha(0.85f); }
            ui_.chip(chipR, t, ink);
            if (ui_.hovered(chipR)) {
                char tip[420];
                snprintf(tip, sizeof tip,
                         "The round trip, on screen. %d forward analysis%s and %d "
                         "inverse synthes%s so far on this frame - the contract "
                         "makes that a GATE: N consecutive bar edits perform "
                         "exactly ONE analysis, because fifty FFT round trips "
                         "accumulate f32 drift that is audible before it is "
                         "visible. %d of 256 bars carry the pen's sine phase; the "
                         "rest, and every harmonic from 257 to 1023, keep their "
                         "analysed complex value.",
                         e.nAnalyse, e.nAnalyse == 1 ? "" : "es", e.nSynth,
                         e.nSynth == 1 ? "is" : "es", e.nTouched);
                ui_.tip = tip;
            }
        }

        // --- TALLER / SHORTER ----------------------------------------------
        //
        // THE ONE CONTROL IN THIS PANEL THAT REACHES OUTSIDE IT, and it is here
        // because the thing it reaches for was added for exactly this. Three
        // comments in this file call the 200px dock "not negotiable"; the
        // detail panel grew a splitter last release and they are now false. A
        // canvas is the only surface here that is BETTER with more pixels, so
        // it gets the one chip that says so -- once, reversibly, out loud, and
        // restoring precisely the height it replaced.
        {
            f32& dH = detailHFor(view_);
            const f32 maxH = std::max(160.f, (f32)win_.height() / s - 180.f);
            const bool grown = g_weGrewFrom > 0.f;
            const u64 wid = uiId(UiSpectraPanel, 1004, uidKey);
            // Inert with the rest of the page. The splitter is not the
            // editor's, but a chip that grows a dock so a canvas can say "no
            // wavetable editor in this build" in a larger font is a control
            // that works and does nothing -- which is the one thing this panel
            // has never shipped.
            if (weLive && ui_.grab(slop(tallR)).button(wid, tallR, "")) {
                if (!grown) {
                    g_weGrewFrom = dH;
                    dH = clampv(std::max(dH, 380.f), 120.f, maxH);
                    char m[220];
                    snprintf(m, sizeof m,
                             "Spectra: the dock is %.0f logical px - the canvas "
                             "took all of it. The splitter above the panel does "
                             "the same by hand; this chip puts it back.",
                             (double)dH);
                    status_ = m;
                } else {
                    dH = clampv(g_weGrewFrom, 120.f, maxH);
                    g_weGrewFrom = 0.f;
                    status_ = "Spectra: the dock is back where it was";
                }
            }
            ui_.microIn(fSmall_, weLive ? ui_.lastRect : tallR,
                        grown ? "SHORTER" : "TALLER",
                        !weLive ? nx::muted.alpha(0.40f)
                                : (grown ? nx::violet : nx::muted), Align::Center);
            if (ui_.hovered(tallR) && !weLive) {
                ui_.tip = "The dock's height is the canvas's resolution, and "
                          "this chip is the detail panel's splitter from in "
                          "here - but there is no editor in this build to give "
                          "the pixels to.";
            } else if (ui_.hovered(tallR)) {
                char t[400];
                snprintf(t, sizeof t,
                         "The canvas is %.0f logical pixels tall, which is %.0f "
                         "pixels per unit of amplitude. A pen wants more of them "
                         "than a knob row does, and the detail panel has a "
                         "splitter now - this is that splitter, from in here. "
                         "%s",
                         (double)(canR.h / s), (double)(canR.h / s * 0.25f),
                         grown ? "Click to put the dock back where it was."
                               : "Click to take the dock to 380.");
                ui_.tip = t;
            }
        }
    }

    // =======================================================================
    // ROWS 1 AND 2 -- the eight frame operations, in the contract's order
    //
    // DESTRUCTIVE FIRST IN EVERY ROW, which is widgets.h's rule for a place
    // where slop is unavoidable -- and here it is not even needed (every
    // segment is 72 x 16, over the floor on both axes, so slop() hands out
    // zero), so the ordering costs nothing and stands as the habit anyway:
    // Clear leads row 1 and Insert / Delete lead row 2.
    //
    // INSERT AND DELETE SAY WHAT THEY DROPPED, EVERY TIME. The frame count is
    // fixed at 32, so there is no way to spell "insert" that does not lose
    // something at one end: Insert drops what was frame 31, Delete duplicates
    // the new last frame. The contract calls them destructive and says the
    // editor must not let a frame vanish silently, so both write a sentence
    // naming the exact slot, into the status bar AND onto the canvas.
    // =======================================================================
    //
    // A DISABLED OPERATION SAYS WHY IT IS DISABLED, which is a different
    // sentence from what it would do -- the panel's rule for every inert socket
    // in it. `whyOff` is that sentence; the contract-absent case has one of its
    // own, because "the clipboard is empty" would be a lie about a build that
    // has no editor at all.
    const auto weOp = [&](const Rect& r0, int sub, const char* label,
                          bool enabled, const char* tip, const char* whyOff,
                          const auto& fn) {
        const bool live = weLive && enabled;
        Rect g = r0;
        if (live) {
            const u64 wid = uiId(UiSpectraPanel, sub, uidKey);
            if (ui_.grab(slop(r0)).segButton(wid, r0, false, nx::violet)) fn();
            g = ui_.lastRect;
        }
        microFit(ui_, fSmall_, g, label,
                 live ? nx::text.alpha(0.92f) : nx::muted.alpha(0.40f), Align::Center);
        if (ui_.hovered(r0))
            ui_.tip = live ? std::string(tip)
                           : (weLive ? std::string(label) + ": " +
                                       (whyOff && *whyOff ? whyOff : tip)
                                     : std::string(label) + " needs the five v5 "
                                       "WavetableControl methods, which this "
                                       "build's plugin contract does not have");
    };
    {
        const f32 ow = ctl.w * 0.25f;
        const Rect r1{ctl.x, rowY(1), ctl.w, subH};
        const Rect r2{ctl.x, rowY(2), ctl.w, subH};
        ui_.segCluster(r1);
        ui_.segCluster(r2);
        for (int k = 1; k < 4; ++k) {
            rend_.hairlineV(r1.x + ow * (f32)k, r1.y + 2 * s, r1.bottom() - 2 * s);
            rend_.hairlineV(r2.x + ow * (f32)k, r2.y + 2 * s, r2.bottom() - 2 * s);
        }
        const auto cell = [&](const Rect& r, int k) {
            return Rect{r.x + ow * (f32)k, r.y, ow, r.h};
        };

        weOp(cell(r1, 0), 1010, "CLEAR", true,
             "The cursor frame becomes 2048 zeros. Undo is this editor's own "
             "button, not Ctrl+Z.", "", [&] {
            e.push(curF);
            f32* fr = e.frame(curF);
            for (int i = 0; i < kWeCycle; ++i) fr[i] = 0.f;
            e.specOk = false;
            char m[96];
            snprintf(m, sizeof m, "frame %d cleared to 2048 zeros", curF);
            e.say(m, 3);
            status_ = std::string("Spectra: ") + m;
            wePreview("clear");
        });
        weOp(cell(r1, 1), 1011, "COPY", true,
             "Copy this frame's 2048 samples to the editor's own clipboard.", "", [&] {
            const f32* fr = e.frame(curF);
            for (int i = 0; i < kWeCycle; ++i) e.clip[(size_t)i] = fr[i];
            e.clipOk = true;
            char m[96];
            snprintf(m, sizeof m, "frame %d copied", curF);
            e.say(m, 1);
            status_ = std::string("Spectra: ") + m;
        });
        weOp(cell(r1, 2), 1012, "PASTE", e.clipOk,
             "Paste the clipboard over this frame.",
             "the editor's clipboard is empty - COPY a frame into it first", [&] {
            e.push(curF);
            f32* fr = e.frame(curF);
            for (int i = 0; i < kWeCycle; ++i) fr[i] = e.clip[(size_t)i];
            e.specOk = false;
            char m[96];
            snprintf(m, sizeof m, "pasted over frame %d", curF);
            e.say(m, 3);
            status_ = std::string("Spectra: ") + m;
            wePreview("paste");
        });
        weOp(cell(r1, 3), 1013, "DUP", curF < kWeFrames - 1,
             "Copy this frame into the NEXT slot, pushing the tail down. Frame "
             "31 is dropped, and it says which.",
             "frame 31 has no next slot to be duplicated into", [&] {
            e.push(-1);
            for (int k = kWeFrames - 1; k >= curF + 2; --k)
                for (int i = 0; i < kWeCycle; ++i) e.frame(k)[i] = e.frame(k - 1)[i];
            for (int i = 0; i < kWeCycle; ++i) e.frame(curF + 1)[i] = e.frame(curF)[i];
            e.specOk = false;
            char m[176];
            snprintf(m, sizeof m,
                     "frame %d duplicated into %d - what was frame 31 is gone, "
                     "because the table always has exactly 32", curF, curF + 1);
            e.say(m, 2);
            status_ = std::string("Spectra: ") + m;
            wePreview("duplicate");
        });

        weOp(cell(r2, 0), 1014, "INSERT", true,
             "Insert a copy of this frame AT the cursor, pushing the tail down. "
             "DESTRUCTIVE: frame 31 is dropped, and it says so.", "", [&] {
            e.push(-1);
            for (int k = kWeFrames - 1; k >= curF + 1; --k)
                for (int i = 0; i < kWeCycle; ++i) e.frame(k)[i] = e.frame(k - 1)[i];
            e.specOk = false;
            char m[176];
            snprintf(m, sizeof m,
                     "inserted at frame %d - what was frame 31 was DROPPED. A "
                     "fixed 32-frame array cannot grow.", curF);
            e.say(m, 2);
            status_ = std::string("Spectra: ") + m;
            wePreview("insert");
        });
        weOp(cell(r2, 1), 1015, "DELETE", true,
             "Remove this frame, pulling the tail up. DESTRUCTIVE: the new last "
             "frame is duplicated into slot 31 to keep the count, and it says "
             "so.", "", [&] {
            e.push(-1);
            for (int k = curF; k <= kWeFrames - 2; ++k)
                for (int i = 0; i < kWeCycle; ++i) e.frame(k)[i] = e.frame(k + 1)[i];
            for (int i = 0; i < kWeCycle; ++i)
                e.frame(kWeFrames - 1)[i] = e.frame(kWeFrames - 2)[i];
            e.specOk = false;
            char m[176];
            snprintf(m, sizeof m,
                     "frame %d removed - frame 30 was DUPLICATED into 31 to keep "
                     "the count at 32", curF);
            e.say(m, 2);
            status_ = std::string("Spectra: ") + m;
            wePreview("delete");
        });

        // --- MORPH ----------------------------------------------------------
        //
        // THE DOMAIN IS HARMONIC AND THE CONTRACT ARGUES IT AT LENGTH: a time-
        // domain fill is exactly what A Position already computes between
        // adjacent frames, so it would write thirty frames that sound like
        // having drawn two -- a no-op you can hear. Magnitudes are interpolated
        // per harmonic over 1..1023 (the fill is not the pen and has no screen
        // to fit in) and the result is synthesised at the pen's sine phase,
        // ascending in k and ascending in h.
        weOp(cell(r2, 2), 1016, "MORPH", e.mb > e.ma + 1,
             "Fill frames a+1..b-1 by interpolating harmonic MAGNITUDES and "
             "re-synthesising at sine phase. a and b are not touched.",
             "b has to be more than one frame past a - there is nothing between "
             "them to fill", [&] {
            const int a = clampv(e.ma, 0, kWeFrames - 1);
            const int b = clampv(e.mb, 0, kWeFrames - 1);
            if (b <= a + 1) {
                status_ = "Spectra: Morph needs b > a + 1 - there is nothing "
                          "between those two frames";
                e.say("morph: nothing between a and b", 2);
                return;
            }
            e.push(-1);
            std::vector<f32> ar(kWeCycle), ai(kWeCycle), br(kWeCycle), bi(kWeCycle);
            std::vector<f32> mA(kWeCycle / 2, 0.f), mB(kWeCycle / 2, 0.f),
                             mK(kWeCycle / 2, 0.f);
            const auto ana = [&](int k, std::vector<f32>& re, std::vector<f32>& im,
                                 std::vector<f32>& mg) {
                const f32* fr = e.frame(k);
                for (int i = 0; i < kWeCycle; ++i) { re[(size_t)i] = fr[i]; im[(size_t)i] = 0.f; }
                weFft(re.data(), im.data(), false);
                for (int h = 1; h <= kWeHarm; ++h)
                    mg[(size_t)h] = weMagOf(re.data(), im.data(), h);
            };
            ana(a, ar, ai, mA);
            ana(b, br, bi, mB);
            for (int k = a + 1; k < b; ++k) {                       // ASCENDING in k
                const f32 t = (f32)(k - a) / (f32)(b - a);
                for (int h = 1; h <= kWeHarm; ++h)                  // ASCENDING in h
                    mK[(size_t)h] = (1.f - t) * mA[(size_t)h] + t * mB[(size_t)h];
                weSynth(mK.data(), nullptr, nullptr, nullptr, e.frame(k),
                        e.tre.data(), e.tim.data());
                ++e.nSynth;
            }
            e.specOk = false;
            // THE HONEST COST, SAID RATHER THAN FIXED BEHIND THE USER'S BACK.
            // The fill is at sine phase; the endpoints keep whatever phase they
            // were drawn with. If they are not already sine there is a phase
            // step at a->a+1 or b-1->b and it is audible as a click at exactly
            // that position. Re-phase endpoints is the fix and it is a separate,
            // named, user-initiated operation -- never applied silently,
            // because a tool that quietly rewrites the two frames the user
            // actually drew is a tool the user stops trusting.
            const bool aSine = weIsSinePhase(ar.data(), ai.data());
            const bool bSine = weIsSinePhase(br.data(), bi.data());
            e.rephaseOffered = !(aSine && bSine);
            char m[120], full[400];
            if (aSine && bSine) {
                snprintf(m, sizeof m,
                         "morph filled %d..%d - endpoints already in sine phase",
                         a + 1, b - 1);
                snprintf(full, sizeof full,
                         "Spectra: morph filled frames %d..%d by interpolating "
                         "harmonic magnitudes over 1..1023 and re-synthesising at "
                         "sine phase. Frames %d and %d were not touched and were "
                         "already in the convention, so the fill joins them with "
                         "no step.", a + 1, b - 1, a, b);
            } else {
                snprintf(m, sizeof m,
                         "morph filled %d..%d - endpoint%s off sine phase: RE-PHASE",
                         a + 1, b - 1, (!aSine && !bSine) ? "s" : "");
                snprintf(full, sizeof full,
                         "Spectra: morph filled frames %d..%d at sine phase. "
                         "Frame%s %s did NOT move and %s not in sine phase, so "
                         "there is an audible step at that boundary. Re-phase "
                         "endpoints rewrites them to the convention with their "
                         "magnitudes untouched - it is offered and never applied "
                         "on your behalf.",
                         a + 1, b - 1, (!aSine && !bSine) ? "s" : "",
                         !aSine && !bSine ? "a and b" : (!aSine ? "a" : "b"),
                         !aSine && !bSine ? "are" : "is");
            }
            e.say(m, (aSine && bSine) ? 3 : 2);
            status_ = full;
            wePreview("morph");
        });

        weOp(cell(r2, 3), 1017, "RE-PHASE", true,
             "Rewrite frames a and b to the pen's sine phase, magnitudes "
             "untouched. This is the fix for a Morph's boundary step and it is "
             "never applied on your behalf.", "", [&] {
            const int a = clampv(e.ma, 0, kWeFrames - 1);
            const int b = clampv(e.mb, 0, kWeFrames - 1);
            e.push(-1);
            std::vector<f32> re(kWeCycle), im(kWeCycle), mg(kWeCycle / 2, 0.f);
            int moved = 0;
            const int ends[2] = {a, b};
            for (int q = 0; q < 2; ++q) {
                const int k = ends[q];
                if (q == 1 && b == a) break;
                const f32* fr = e.frame(k);
                for (int i = 0; i < kWeCycle; ++i) { re[(size_t)i] = fr[i]; im[(size_t)i] = 0.f; }
                weFft(re.data(), im.data(), false);
                if (weIsSinePhase(re.data(), im.data())) continue;
                for (int h = 1; h <= kWeHarm; ++h)
                    mg[(size_t)h] = weMagOf(re.data(), im.data(), h);
                weSynth(mg.data(), nullptr, nullptr, nullptr, e.frame(k),
                        e.tre.data(), e.tim.data());
                ++e.nSynth;
                ++moved;
            }
            e.specOk = false;
            e.rephaseOffered = false;
            char m[220];
            if (!moved)
                snprintf(m, sizeof m,
                         "frames %d and %d were already in sine phase - nothing "
                         "was rewritten", a, b);
            else
                snprintf(m, sizeof m,
                         "%d endpoint%s rewritten to sine phase; every "
                         "magnitude is exactly what it was", moved,
                         moved == 1 ? "" : "s");
            e.say(m, moved ? 3 : 1);
            status_ = std::string("Spectra: ") + m;
            if (moved) wePreview("re-phase");
        });
        // THE OFFER, on the control. Amber is this program's "refused / needs
        // an answer", and an un-answered phase step at a morph boundary is
        // exactly that: the fill is done, it is audible, and there is one
        // gesture that fixes it.
        if (weLive && e.rephaseOffered)
            rend_.roundRectOutline(cell(r2, 3), nx::radiusXs * s,
                                   std::max(1.f, nx::snapPx(s)), nx::amber.alpha(0.85f));
    }

    // =======================================================================
    // ROW 3 -- the morph's two endpoints, and the peak the commit will scale by
    // =======================================================================
    {
        const Rect r3{ctl.x, rowY(3), ctl.w, subH};
        const f32 sw = 88 * s;
        const auto endpoint = [&](const Rect& r0, int sub, const char* label,
                                  int* v, int lo, int hi) {
            ui_.segCluster(r0);
            const f32 bw = 16 * s;
            const Rect lb{r0.x, r0.y, bw, r0.h}, rb{r0.right() - bw, r0.y, bw, r0.h};
            rend_.hairlineV(lb.right(), r0.y + 2 * s, r0.bottom() - 2 * s);
            rend_.hairlineV(rb.x, r0.y + 2 * s, r0.bottom() - 2 * s);
            const auto chev = [&](const Rect& b, bool leftward) {
                const f32 k = 2.6f * s, d = leftward ? -1.f : 1.f;
                const Col c = weLive ? nx::muted : nx::muted.alpha(0.4f);
                rend_.line(b.cx() - k * d * 0.6f, b.cy() - k, b.cx() + k * d * 0.6f, b.cy(),
                           1.1f * s, c);
                rend_.line(b.cx() - k * d * 0.6f, b.cy() + k, b.cx() + k * d * 0.6f, b.cy(),
                           1.1f * s, c);
            };
            if (weLive) {
                if (ui_.grab(slop(lb)).segButton(uiId(UiSpectraPanel, sub, uidKey),
                                                 lb, false, nx::violet))
                    *v = clampv(*v - 1, lo, hi);
                chev(ui_.lastRect, true);
                if (ui_.grab(slop(rb)).segButton(uiId(UiSpectraPanel, sub + 1, uidKey),
                                                 rb, false, nx::violet))
                    *v = clampv(*v + 1, lo, hi);
                chev(ui_.lastRect, false);
                if (ui_.hovered(r0) && in.wheel != 0.f) {
                    *v = clampv(*v + (in.wheel > 0.f ? 1 : -1), lo, hi);
                    in.wheel = 0.f;          // the strip must not also scroll
                }
            } else { chev(lb, true); chev(rb, false); }
            char t[24];
            snprintf(t, sizeof t, "%s %d", label, *v);
            microFit(ui_, fSmall_, {lb.right(), r0.y, rb.x - lb.right(), r0.h}, t,
                     (weLive ? nx::text : nx::muted.alpha(0.45f)), Align::Center);
        };
        endpoint({r3.x, r3.y, sw, r3.h}, 1020, "a", &e.ma, 0, kWeFrames - 1);
        endpoint({r3.x + sw + 4 * s, r3.y, sw, r3.h}, 1022, "b", &e.mb, 0, kWeFrames - 1);
        if (ui_.hovered({r3.x, r3.y, sw * 2.f + 4 * s, r3.h}))
            ui_.tip = "The morph's two endpoints. Right-click a cell in the "
                      "frame strip to set whichever of the two is nearer to it. "
                      "Morph replaces a+1..b-1 and never touches a or b.";

        // THE PEAK READOUT. Set normalisation is one scalar over all 32 frames
        // at commit; it changes no shape and no inter-frame relationship, so
        // there is nothing to watch happen -- and this number is the honest
        // version of "you can see it happen", because it is the whole of what
        // the step does, legible before it runs.
        const Rect pkR{r3.x + sw * 2.f + 12 * s, r3.y,
                       std::max(8 * s, r3.right() - (r3.x + sw * 2.f + 12 * s)), r3.h};
        rend_.well(pkR, nx::radiusXs * s, true);
        char pkT[48];
        if (!weLive)                     snprintf(pkT, sizeof pkT, "peak -");
        else if (wePk <= kWeSilentPk)    snprintf(pkT, sizeof pkT, "silent");
        else snprintf(pkT, sizeof pkT, "pk %.2f  x%.2f", (double)wePk,
                      (double)(1.f / wePk));
        microFit(ui_, fSmall_, pkR, pkT,
                 (!weLive ? nx::muted.alpha(0.40f)
                          : (wePk <= kWeSilentPk ? nx::amber.alpha(0.95f)
                                                 : nx::muted.alpha(0.85f))),
                 Align::Center);
        if (ui_.hovered(pkR))
            ui_.tip = wePk <= kWeSilentPk
                ? "The set peak is zero, so a commit would be refused: a drawing "
                  "of silence is a mistake, and letting it through would burn an "
                  "identity on it forever."
                : "The set-wide peak over all 32 x 2048 samples, and the single "
                  "gain a commit will multiply every sample by. It is ONE scalar "
                  "for the whole table: nothing you drew moves, the level does.";
    }

    // =======================================================================
    // ROW 4 -- the name
    // =======================================================================
    {
        const Rect r4{ctl.x, rowY(4), ctl.w, subH};
        const Rect nmR{r4.x, r4.y, r4.w - 64 * s, r4.h};
        const Rect rnR{r4.right() - 60 * s, r4.y, 60 * s, r4.h};
        const u64 nid = uiId(UiSpectraPanel, 1030, uidKey);
        rend_.well(nmR, nx::radiusXs * s, true);
        if (weLive) {
            // activateOnDouble = FALSE. A double-click is right for an inline
            // RENAME over a label that is normally something else (the marker
            // idiom, and g_save's preset chip); this is a dedicated, always-
            // visible field in a row of its own, and a field you have to
            // double-click is a field people report as broken.
            if (ui_.textField(nid, nmR, &e.name, nx::panel2.alpha(0.f), nx::text,
                              Align::Left, /*activateOnDouble=*/false)) {
                if (e.name.size() > 64) {
                    e.name.resize(64);
                    ui_.refusal = "a wavetable name is at most 64 bytes";
                }
            }
        }
        if (e.name.empty() && ui_.editId != nid) {
            // The placeholder is the table's CURRENT display name, which is
            // customName()'s three-deep fallback and may be a hash. Saying it
            // here is what makes "empty means no name" readable rather than
            // mysterious.
            const char* cn = weLive ? wtName(inst, e.osc) : "";
            char ph[96];
            snprintf(ph, sizeof ph, "%s", (cn && *cn) ? cn : "no name");
            microFit(ui_, fSmall_, nmR.inset(4 * s), ph, nx::muted.alpha(0.40f),
                     Align::Left, 0);
        }
        if (ui_.hovered(nmR))
            ui_.tip = "The table's display name, at most 64 bytes. It is NEVER "
                      "identity, never consulted when a table is resolved and "
                      "never sent over the wire. Empty means no name at all - "
                      "which is the only spelling of it - and the grey text is "
                      "what this table is called now.";
        {
            const bool live = weLive && wtHasCustom(inst, e.osc);
            Rect g = rnR;
            if (live) {
                const u64 wid = uiId(UiSpectraPanel, 1031, uidKey);
                if (ui_.grab(slop(rnR)).segButton(wid, rnR, false, nx::violet)) {
                    if (wtSetCustomName(inst, e.osc,
                                        e.name.empty() ? nullptr : e.name.c_str())) {
                        status_ = e.name.empty()
                            ? "Spectra: the name was cleared - the content did "
                              "not change, so the hash did not either"
                            : std::string("Spectra: renamed to \"") + e.name +
                              "\" - a rename writes no file and makes no new hash";
                        e.say("renamed - identity unchanged", 3);
                    } else {
                        status_ = "Spectra: the rename was refused - a name is at "
                                  "most 64 bytes and holds no control byte";
                        e.say("rename refused", 2);
                    }
                }
                g = ui_.lastRect;
            }
            ui_.microIn(fSmall_, g, "RENAME",
                        live ? nx::text.alpha(0.92f) : nx::muted.alpha(0.40f),
                        Align::Center);
            if (ui_.hovered(rnR))
                ui_.tip = live
                    ? "Set or clear the display name of the table this "
                      "oscillator already has. Content is unchanged, so IDENTITY "
                      "is unchanged: no file is written and no new hash is made."
                    : "There is no committed table on this oscillator to rename "
                      "yet - COMMIT takes the name in the field with it.";
        }
    }

    // =======================================================================
    // ROW 5 -- undo, preview, commit, revert
    // =======================================================================
    {
        const f32 ow = ctl.w * 0.25f;
        const Rect r5{ctl.x, rowY(5), ctl.w, subH};
        ui_.segCluster(r5);
        for (int k = 1; k < 4; ++k)
            rend_.hairlineV(r5.x + ow * (f32)k, r5.y + 2 * s, r5.bottom() - 2 * s);
        const auto cell = [&](int k) { return Rect{r5.x + ow * (f32)k, r5.y, ow, r5.h}; };

        // UNDO IS A BUTTON AND NOT Ctrl+Z, and that is a decision worth stating.
        // The set's undo runs in App::handleShortcuts() at the TOP of the frame,
        // before any view draws, and the editor's working copy is not in the
        // set at all -- so a Ctrl+Z here would undo some unrelated edit and
        // leave the drawing exactly where it was. A key that does the wrong
        // thing is worse than a key that does nothing, and worse again than a
        // button that does the right one.
        weOp(cell(0), 1040, "UNDO", !e.undo.empty(),
             "Step back through this editor's own history. Ctrl+Z is the SET's "
             "undo and runs before this panel draws; the working copy is not in "
             "the set, so it has its own.",
             "nothing has been done to this table yet", [&] {
            if (e.pop()) {
                char m[64];
                snprintf(m, sizeof m, "undone - %d step%s left",
                         (int)e.undo.size(), e.undo.size() == 1 ? "" : "s");
                e.say(m, 1);
                wePreview("undo");
            }
        });
        weOp(cell(1), 1041, "PREVIEW", true,
             "Publish the working copy into the oscillator's preview arena so "
             "you can hear it. It touches no hash, no file and no state record: "
             "what a save would name does not move.", "", [&] {
            wePreview("button");
        });
        weOp(cell(2), 1042, "COMMIT", true,
             "Canonicalise, hash, write the drawn file, adopt it and point this "
             "oscillator at it. THIS is the step that makes a table, and it is "
             "the only one that survives closing the editor.", "", [&] {
            weCommit();
        });
        weOp(cell(3), 1043, "REVERT", true,
             "Drop the preview, republish the committed table and re-read it "
             "into the editor. Everything drawn since the last commit is gone.", "", [&] {
            wtCancelPreview(inst, e.osc);
            if (probeOn())
                LOGI("NXTAKT_DEBUG_PROBE: spectra editor cancelPreview osc %s "
                     "(revert)", e.osc ? "B" : "A");
            weOpen(e.osc, "revert");     // release() clears `previewing` with it
            status_ = "Spectra: the committed table is playing again and the "
                      "editor was re-read from it";
        });
        // ...and the standing preview is a LAMP rather than a sentence: it is a
        // state that persists until a commit or a revert ends it, and §1 puts
        // cyan on the thing that is happening.
        if (weLive && e.previewing) {
            const Rect pl = cell(1);
            rend_.rect({pl.x + 3 * s, pl.cy() - 1.5f * s, 3 * s, 3 * s},
                       nx::cyan.alpha(0.9f));
        }
        // COMMIT wears the one violet in the row when there is something to
        // save: §1's rule that violet is a thing you set, and the only control
        // on this page that sets anything outside the editor.
        if (weLive && e.anyDirty())
            rend_.roundRectOutline(cell(2), nx::radiusXs * s,
                                   std::max(1.f, nx::snapPx(s)), nx::violet.alpha(0.8f));
    }

    // =======================================================================
    // THE FRAME STRIP -- 32 cells, the cursor, the dirty marks, Position
    //
    // ONE HOT RECT for the whole strip, exactly as a drawable level row is one
    // for sixteen steps: a click selects, a drag scrubs, the wheel steps and is
    // CONSUMED (a notch that fell through an 800px surface would slide the
    // device strip out from under a hand that never moved -- the coexistence
    // rule's exact failure at the largest target on the page), and a
    // right-click sets whichever morph endpoint is nearer.
    //
    // THE CURSOR IS NOT A Position. Position (id 1) is continuous, automatable
    // and modulatable by four different things; the cursor is a place to look.
    // The strip draws Position as a marker and leaves the cursor where the user
    // put it, because a cursor that followed Position would let an automation
    // lane drag the pen around mid-stroke.
    // =======================================================================
    {
        rend_.well(stripR, nx::radiusXs * s, true);
        const Rect sp = stripR.insetXY(2 * s, 2 * s);
        const f32 cw = sp.w / (f32)kWeFrames;
        const u64 sid = uiId(UiSpectraPanel, 1051, uidKey);
        const auto cellAt = [&](f32 x) {
            return clampv((int)std::floor((x - sp.x) / std::max(1e-3f, cw)), 0,
                          kWeFrames - 1);
        };
        bool stripHot = false;
        if (weLive && ui_.setHot(sid, stripR) && ui_.isHot(sid)) {
            stripHot = true;
            ui_.cursor = Cursor::Hand;
            if (in.pressed[0] || (in.down[0] && ui_.active == sid)) {
                ui_.active = sid;
                weMoveCursor(cellAt(in.mx), "strip");
            }
            if (in.pressed[2]) {
                const int k = cellAt(in.mx);
                // Whichever endpoint is nearer, so one gesture reaches both and
                // neither needs a modifier.
                const bool nearA = std::abs(k - e.ma) <= std::abs(k - e.mb);
                if (nearA) e.ma = k; else e.mb = k;
                char m[120];
                snprintf(m, sizeof m, "morph endpoint %s = frame %d",
                         nearA ? "a" : "b", k);
                e.say(m, 3);
                status_ = std::string("Spectra: ") + m;
            }
            if (in.wheel != 0.f) {
                weMoveCursor(e.cur + (in.wheel > 0.f ? 1 : -1), "wheel");
                in.wheel = 0.f;
            }
        }
        // the morph's span, faint, so a and b are not two numbers in a row
        if (e.mb > e.ma + 1)
            rend_.rect({sp.x + cw * (f32)(e.ma + 1), sp.y,
                        cw * (f32)(e.mb - e.ma - 1), sp.h},
                       nx::violet.alpha(weLive ? 0.14f : 0.06f));
        for (int k = 0; k < kWeFrames; ++k) {
            const Rect c0{sp.x + cw * (f32)k, sp.y, cw, sp.h};
            if (k % 4 == 0 && k)
                rend_.hairlineV(std::round(c0.x), stripR.y + 2 * s, stripR.bottom() - 2 * s);
            const bool isCur = k == e.cur;
            const bool dirty = weLive && e.frameDirty(k);
            if (isCur) {
                rend_.rect(c0.inset(1.f * s), nx::violet.alpha(weLive ? 0.45f : 0.18f));
                rend_.gradStroke(c0.inset(0.5f * s), nx::radiusXs * s, s, nx::edgeLit, 1.f);
            }
            if (dirty)
                rend_.rect({c0.cx() - 1.5f * s, c0.bottom() - 3.f * s, 3.f * s, 2.f * s},
                           nx::cyan.alpha(0.9f));
            if (k == e.ma || k == e.mb)
                rend_.rect({c0.x + 1.f * s, c0.y, std::max(1.f, nx::snapPx(s)), c0.h},
                           nx::violet.alpha(0.9f));
            if (k % 4 == 0 || isCur) {
                char n[8];
                snprintf(n, sizeof n, "%d", k);
                microFit(ui_, fSmall_, c0, n,
                         (isCur ? nx::text : nx::muted).alpha(weLive ? 0.8f : 0.35f),
                         Align::Center);
            }
        }
        // Position, drawn and never followed.
        if (has(e.osc ? pBPos : pAPos)) {
            const f32 pv = clampv(get(e.osc ? pBPos : pAPos, 0.f), 0.f, 1.f);
            const f32 px = sp.x + cw * (pv * (f32)(kWeFrames - 1) + 0.5f);
            rend_.rect({px - 1.f * s, stripR.y + 1.f * s, std::max(1.f, nx::snapPx(2 * s)),
                        stripR.h - 2.f * s}, nx::cyan.alpha(0.75f));
        }
        if (stripHot) {
            char t[400];
            snprintf(t, sizeof t,
                     "Frame %d of 32. Click or drag to move the cursor, wheel "
                     "steps it, [ and ] step it from the keyboard, right-click "
                     "sets the nearer morph endpoint. The cyan bar is A "
                     "Position, which the cursor deliberately does NOT follow: "
                     "Position is automatable and a cursor that followed it "
                     "would let an automation lane drag the pen mid-stroke. A "
                     "cyan dot marks a frame that differs from what was read.",
                     e.cur);
            ui_.tip = t;
        } else if (!weLive && ui_.hovered(stripR)) {
            ui_.tip = weContract
                ? "This device answers no wavetable(), so there are no frames "
                  "to point a cursor at"
                : "The 32-frame cursor needs WavetableControl's v5 methods, "
                  "which this build's plugin contract does not have";
        }
    }

    // =======================================================================
    // THE CANVAS -- one surface, two pens, and the page's whole point
    // =======================================================================
    curF = clampv(e.cur, 0, kWeFrames - 1);      // the strip may have just moved it
    {
        rend_.well(canR, nx::radiusSm * s, true);
        const Rect p = canR.insetXY(4 * s, 4 * s);
        const u64 cid = uiId(UiSpectraPanel, 1050, uidKey);
        const f32 th1 = std::max(1.f, nx::snapPx(s));
        const bool editable = weLive && e.open;

        if (!editable) {
            // INERT AND EXPLAINED, which is the acceptance test. The canvas is
            // a well with a sentence and it claims no hot rect at all, so the
            // pointer passes straight through it and the cursor never becomes a
            // pen over a surface that cannot be drawn on.
            // The LABEL carries the word and the TOOLTIP carries the sentence,
            // which is this file's rule for every string cut to a cell.
            const char* line1 = !weContract ? "no wavetable editor in this build"
                                            : "this device has no wavetable control";
            const char* line2 = !weContract
                ? "host.h has none of the five v5 WavetableControl methods:"
                : "wavetable() answered null, so there are no frames to read";
            const char* line3 = !weContract
                // 65 bytes, and the count matters: microFit's own buffer is 72,
                // so a longer line is silently truncated with no ellipsis to
                // say it happened. The five names are the whole point of the
                // line, so the separators pay for them.
                ? "readFrames/previewFrames/commitFrames/cancelPreview/"
                  "setCustomName"
                : "";
            const char* why = !weContract
                ? "This build's plugin contract has no WavetableControl::"
                  "readFrames, previewFrames, commitFrames, cancelPreview or "
                  "setCustomName - the five v5 methods the editor is written "
                  "against. They are append-only additions with defaults, so "
                  "this page comes alive the moment the header lands, with no "
                  "edit here."
                : "This device answers no wavetable(), so there is no table to "
                  "edit and nothing to commit one to.";
            rend_.textIn(fSmall_, {p.x, p.cy() - 20 * s, p.w, 11 * s}, line1,
                         nx::amber.alpha(0.9f), Align::Center);
            microFit(ui_, fSmall_, {p.x + p.w * 0.04f, p.cy() - 4 * s, p.w * 0.92f, 11 * s},
                     line2, nx::muted.alpha(0.55f), Align::Center);
            if (*line3)
                microFit(ui_, fSmall_,
                         {p.x + p.w * 0.04f, p.cy() + 9 * s, p.w * 0.92f, 11 * s},
                         line3, nx::muted.alpha(0.40f), Align::Center);
            if (ui_.hovered(canR)) ui_.tip = why;
        } else if (wave) {
            // ===============================================================
            // THE WAVEFORM PEN
            //
            // x is the sample index, y is the value CLAMPED TO +-1 -- the pen
            // cannot draw past full scale. The canvas shows +-2, because DC
            // removal at stroke end can push a curve past +-1 and a curve that
            // vanished off the top of its own canvas would be a curve the user
            // cannot see or fix. It NEVER clips a sample for display: the range
            // opens further when a frame needs it (the harmonic pen can build
            // one that does) and the footer says so.
            //
            // Sparse strokes interpolate LINEARLY in index order across the
            // span the stroke crossed. Linear and not a spline, and that is a
            // decision: a spline overshoots, an overshoot is a sample the user
            // did not draw, and the pen has to be able to draw a hard vertical
            // step, which no interpolating spline can express. A stroke does
            // NOT wrap -- the cycle is a ring to the oscillator and a line to
            // the pen, and a discontinuity at the wrap is what a sawtooth IS.
            // ===============================================================
            f32 fpk = 0.f;
            {
                const f32* fr = e.frame(curF);
                for (int i = 0; i < kWeCycle; ++i) {
                    const f32 a = std::fabs(fr[i]);
                    if (a > fpk) fpk = a;
                }
            }
            const f32 rng = std::max(2.f, std::ceil(fpk));
            const auto vToY = [&](f32 v) { return p.cy() - v / rng * p.h * 0.5f; };
            const auto yToV = [&](f32 y) {
                return clampv((p.cy() - y) / (p.h * 0.5f) * rng, -1.f, 1.f);
            };
            const auto xToI = [&](f32 x) {
                return clampv((int)std::lround((x - p.x) / std::max(1e-3f, p.w) *
                                               (f32)(kWeCycle - 1)), 0, kWeCycle - 1);
            };

            // the grid: zero, the +-1 pair, and the headroom band beyond it
            rend_.rect({p.x, vToY(rng), p.w, vToY(1.f) - vToY(rng)},
                       nx::line.alpha(0.20f));
            rend_.rect({p.x, vToY(-1.f), p.w, vToY(-rng) - vToY(-1.f)},
                       nx::line.alpha(0.20f));
            rend_.hairlineH(p.x, p.right(), std::round(vToY(1.f)));
            rend_.hairlineH(p.x, p.right(), std::round(vToY(-1.f)));
            rend_.rect({p.x, std::round(p.cy()), p.w, th1}, nx::line.alpha(0.85f));
            // ...and the three of them named, because "which line is full scale"
            // is the one question this canvas cannot answer by looking.
            for (int q = 0; q < 3; ++q) {
                const f32 v = q == 0 ? 1.f : (q == 1 ? 0.f : -1.f);
                ui_.microIn(fSmall_, {p.x + 1 * s, vToY(v) - 5 * s, 22 * s, 10 * s},
                            q == 0 ? "+1" : (q == 1 ? "0" : "-1"),
                            nx::muted.alpha(0.30f), Align::Left, 0);
            }

            // The GESTURE, before the trace, so what is drawn is this frame's
            // result and not the last one's.
            bool hot = false;
            if (ui_.setHot(cid, canR) && ui_.isHot(cid)) {
                hot = true;
                ui_.cursor = Cursor::Hand;
                ui_.badge = (in.down[2] || in.pressed[2]) ? Badge::Delete : Badge::Draw;
                if (in.pressed[2] || in.pressed[0]) {
                    e.push(curF);
                    e.stroke = in.pressed[2] ? 2 : 1;
                    e.anchI = e.lastI = xToI(in.mx);
                    e.anchV = e.lastV = in.pressed[2] ? 0.f : yToV(in.my);
                    e.strokeChanged = false;
                    if (in.pressed[0]) ui_.active = cid;
                    e.frame(curF)[e.lastI] = e.lastV;
                    e.strokeChanged = true;
                    if (probeOn())
                        LOGI("NXTAKT_DEBUG_PROBE: spectra pen down frame %d "
                             "index %d value %.3f", curF, e.lastI, (double)e.lastV);
                }
                if (in.wheel != 0.f) {
                    weMoveCursor(e.cur + (in.wheel > 0.f ? 1 : -1), "canvas wheel");
                    in.wheel = 0.f;
                }
            }
            if (e.stroke) {
                const bool held = e.stroke == 1 ? in.down[0] : in.down[2];
                if (held) {
                    const int i1 = xToI(in.mx);
                    const f32 v1 = e.stroke == 2 ? 0.f : yToV(in.my);
                    f32* fr = e.frame(curF);
                    int i0 = e.lastI; f32 v0 = e.lastV;
                    if (in.shift() && e.stroke == 1) {
                        // The line tool: two points and the same rule. It is a
                        // rubber band, so the frame goes back to what it was at
                        // the press before the line is laid down.
                        if (!e.undo.empty() && e.undo.back().frame == curF &&
                            (int)e.undo.back().data.size() == kWeCycle)
                            for (int i = 0; i < kWeCycle; ++i)
                                fr[i] = e.undo.back().data[(size_t)i];
                        i0 = e.anchI; v0 = e.anchV;
                    }
                    if (i1 == i0) {
                        if (fr[i0] != v1) { fr[i0] = v1; e.strokeChanged = true; }
                    } else {
                        const int lo = i0 < i1 ? i0 : i1, hi = i0 < i1 ? i1 : i0;
                        for (int i = lo; i <= hi; ++i) {       // ASCENDING in index
                            const f32 u = (f32)(i - i0) / (f32)(i1 - i0);
                            const f32 v = v0 + (v1 - v0) * u;
                            if (fr[i] != v) { fr[i] = v; e.strokeChanged = true; }
                        }
                    }
                    if (!(in.shift() && e.stroke == 1)) { e.lastI = i1; e.lastV = v1; }
                } else {
                    // STROKE END, and the user watches DC removal happen: the
                    // curve slides vertically the moment the pointer lifts.
                    // Applied during the stroke the curve would crawl under the
                    // cursor; applied at commit, the last thing the user saw
                    // would not be the thing that got saved.
                    const int was = e.stroke;
                    e.stroke = 0;
                    if (e.strokeChanged) {
                        // THE PROBE'S HALF OF THE PROOF. A screenshot shows a
                        // curve; this says which samples the stroke actually
                        // wrote, which is the only way to assert "a stroke from
                        // 400 to 900 changes 501 samples and nothing else".
                        if (probeOn()) {
                            const f32* fr = e.frame(curF);
                            const f32* wf = e.wasFrame(curF);
                            int lo = -1, hi = -1, n = 0;
                            for (int i = 0; i < kWeCycle; ++i)
                                if (fr[i] != wf[i]) { if (lo < 0) lo = i; hi = i; ++n; }
                            LOGI("NXTAKT_DEBUG_PROBE: spectra pen up frame %d "
                                 "wrote %d samples, span %d..%d", curF, n, lo, hi);
                        }
                        weDcRemove(e.frame(curF));
                        // A TIME-DOMAIN STROKE INVALIDATES THE ANALYSIS. It
                        // touches every harmonic, so there is nothing to
                        // preserve and nothing to pretend about; the bars will
                        // show what the stroke actually made.
                        e.specOk = false;
                        e.say(was == 2 ? "erased to zero - DC removed, analysis dropped"
                                       : "stroke ended - DC removed, analysis dropped", 3);
                        wePreview("stroke");
                    }
                }
            }

            // ---- the trace -------------------------------------------------
            //
            // MIN/MAX PER COLUMN rather than a polyline, because 2048 samples
            // across 800 pixels is two and a half samples a column and the
            // honest picture of that is the span they cover. It also draws a
            // hard vertical step as a hard vertical step, which is the one
            // shape this pen exists to be able to make.
            const int cols = clampv((int)(p.w / std::max(1.f, s)), 32, 2048);
            rend_.pushClip(canR);
            const auto plot = [&](const f32* fr, const Col& c0, f32 minTh) {
                for (int cx = 0; cx < cols; ++cx) {
                    const int i0 = (int)((f32)cx * (f32)kWeCycle / (f32)cols);
                    int i1 = (int)((f32)(cx + 1) * (f32)kWeCycle / (f32)cols);
                    if (i1 <= i0) i1 = i0 + 1;
                    if (i1 > kWeCycle) i1 = kWeCycle;
                    f32 lo = fr[i0], hi = fr[i0];
                    for (int i = i0 + 1; i < i1; ++i) {
                        if (fr[i] < lo) lo = fr[i];
                        if (fr[i] > hi) hi = fr[i];
                    }
                    const f32 yA = vToY(hi), yB = vToY(lo);
                    rend_.rect({p.x + p.w * (f32)cx / (f32)cols, yA,
                                std::max(minTh, p.w / (f32)cols),
                                std::max(minTh, yB - yA)}, c0);
                }
            };
            // What was READ, behind, when this frame has moved -- so "what
            // changed" is on the same axes as what it changed from.
            if (e.frameDirty(curF)) plot(e.wasFrame(curF), nx::muted.alpha(0.22f), th1);
            plot(e.frame(curF), nx::cyan.alpha(0.95f), th1);
            rend_.popClip();

            if (hot) {
                const int hi = xToI(in.mx);
                char t[440];
                snprintf(t, sizeof t,
                         "Waveform pen, frame %d, sample %d of 2048 (%.3f). Drag "
                         "to draw, shift-drag for a straight line, right-DRAG to "
                         "flatten to zero, wheel steps the frame. A stroke fills "
                         "every index it crossed by linear interpolation and "
                         "leaves the rest alone - it does not wrap, because a "
                         "discontinuity at the cycle boundary is what a sawtooth "
                         "IS. DC is removed when the pointer lifts, and you watch "
                         "it happen.",
                         curF, hi, (double)e.frame(curF)[hi]);
                ui_.tip = t;
            }
            // The label, where this display keeps its honesty -- the hero's own
            // idiom, and here it carries the number the shape argument turns on.
            char lb[128];
            snprintf(lb, sizeof lb, "%.0f px canvas - %.0f px per unit%s%s",
                     (double)(canR.h / s), (double)(canR.h / s * 0.5f / rng),
                     rng > 2.f ? " - range opened" : "",
                     e.previewing ? " - PREVIEWING" : "");
            microFit(ui_, fSmall_,
                     {canR.x + 6 * s, canR.bottom() - 11 * s, canR.w * 0.40f, 10 * s},
                     lb, nx::muted.alpha(0.35f), Align::Left, 0);
        } else {
            // ===============================================================
            // THE HARMONIC PEN
            //
            // 256 bars, 0 dB at the top (magnitude 1.0 under the 2/N analysis
            // scaling) and a HARD ZERO at the -80 dB floor: a bar dragged to
            // the bottom sets the magnitude to exactly 0.0f, because "drag it
            // away" has to mean the harmonic is gone and a -80 dB residue on
            // 256 harmonics is a floor of hiss no gesture can remove. The scale
            // is dB and not linear because a fifth harmonic at -40 dB is one
            // four-hundredth of a linear canvas, and the harmonics that give a
            // wavetable its character live between -20 and -60.
            //
            // A TOUCHED BAR IS VIOLET AND AN UNTOUCHED ONE IS CYAN, which is
            // the round trip drawn per harmonic: violet is §1's "a thing you
            // set", and what a touched bar sets is the magnitude AND the phase.
            // Everything still cyan kept its analysed complex value, and so did
            // every harmonic from 257 to 1023, which no bar can show and which
            // the footer counts.
            // ===============================================================
            weEnsureSpectrum(e);
            const f32 floorPx = std::max(3.f * s, p.h * 0.05f);
            const auto dbToY = [&](f32 db) {
                return p.bottom() - p.h * (db - kWeFloorDb) / (-kWeFloorDb);
            };
            const auto yToDb = [&](f32 y) {
                if (y >= p.bottom() - floorPx) return kWeFloorDb;   // the snap band
                const f32 u = clampv(1.f - (y - p.y) / std::max(1e-3f, p.h), 0.f, 1.f);
                return clampv(kWeFloorDb * (1.f - u), kWeFloorDb, 0.f);
            };
            const auto xToBar = [&](f32 x) {
                return clampv((int)std::floor((x - p.x) / std::max(1e-3f, p.w) *
                                              (f32)kWeBars) + 1, 1, kWeBars);
            };

            for (int d = -20; d > -80; d -= 20)
                rend_.hairlineH(p.x, p.right(), std::round(dbToY((f32)d)));
            rend_.rect({p.x, p.bottom() - floorPx, p.w, floorPx},
                       nx::line.alpha(0.28f));
            rend_.rect({p.x, std::round(p.bottom() - floorPx), p.w, th1},
                       nx::amber.alpha(0.45f));

            bool hot = false;
            const auto barSet = [&](int h, f32 db) {
                const f32 m = weDbToMag(db);
                if (e.mag[(size_t)h] == m) return;
                e.mag[(size_t)h] = m;
                if (!e.touched[h]) { e.touched[h] = true; ++e.nTouched; }
                e.strokeChanged = true;
            };
            if (ui_.setHot(cid, canR) && ui_.isHot(cid)) {
                hot = true;
                ui_.cursor = Cursor::Hand;
                ui_.badge = (in.down[2] || in.pressed[2]) ? Badge::Delete : Badge::Draw;
                if (in.pressed[2] || in.pressed[0]) {
                    e.push(curF);
                    for (size_t i = 0; i < e.mag.size(); ++i) e.magWas[i] = e.mag[i];
                    for (int b = 0; b <= kWeBars; ++b) e.touchWas[(size_t)b] = (char)e.touched[b];
                    e.stroke = in.pressed[2] ? 2 : 1;
                    e.anchI = e.lastI = xToBar(in.mx);
                    e.anchV = e.lastV = in.pressed[2] ? kWeFloorDb : yToDb(in.my);
                    e.strokeChanged = false;
                    if (in.pressed[0]) ui_.active = cid;
                    barSet(e.lastI, e.lastV);
                    if (e.strokeChanged) weResynth(e);
                }
                if (in.wheel != 0.f) {
                    weMoveCursor(e.cur + (in.wheel > 0.f ? 1 : -1), "canvas wheel");
                    in.wheel = 0.f;
                }
            }
            if (e.stroke) {
                const bool held = e.stroke == 1 ? in.down[0] : in.down[2];
                if (held) {
                    const int b1 = xToBar(in.mx);
                    const f32 d1 = e.stroke == 2 ? kWeFloorDb : yToDb(in.my);
                    int b0 = e.lastI; f32 d0 = e.lastV;
                    const bool line = in.shift() && e.stroke == 1;
                    if (line) {
                        for (size_t i = 0; i < e.mag.size(); ++i) e.mag[i] = e.magWas[i];
                        e.nTouched = 0;
                        for (int b = 0; b <= kWeBars; ++b) {
                            e.touched[b] = e.touchWas[(size_t)b] != 0;
                            if (b >= 1 && e.touched[b]) ++e.nTouched;
                        }
                        b0 = e.anchI; d0 = e.anchV;
                    }
                    const bool was = e.strokeChanged;
                    e.strokeChanged = false;
                    if (b1 == b0) {
                        barSet(b0, d1);
                    } else {
                        const int lo = b0 < b1 ? b0 : b1, hi = b0 < b1 ? b1 : b0;
                        for (int b = lo; b <= hi; ++b) {       // ASCENDING in h
                            const f32 u = (f32)(b - b0) / (f32)(b1 - b0);
                            barSet(b, d0 + (d1 - d0) * u);
                        }
                    }
                    if (e.strokeChanged || line) weResynth(e);
                    e.strokeChanged = e.strokeChanged || was;
                    if (!line) { e.lastI = b1; e.lastV = d1; }
                } else {
                    e.stroke = 0;
                    if (e.strokeChanged) {
                        // NO DC REMOVAL HERE, AND IT IS NOT AN OMISSION. A sum
                        // of sines is odd-symmetric about i=0, so its mean is
                        // zero by construction and the removal is a no-op --
                        // which is the whole reason the phase convention is
                        // all-sine. The harmonic pen never moves what the user
                        // drew; the waveform pen has to buy that with a jump.
                        char m[128];
                        snprintf(m, sizeof m,
                                 "%d of 256 bars now carry the sine phase; the "
                                 "rest kept theirs", e.nTouched);
                        e.say(m, 3);
                        wePreview("bars");
                    }
                }
            }

            // ---- the bars --------------------------------------------------
            const f32 bw = p.w / (f32)kWeBars;
            rend_.pushClip(canR);
            for (int h = 1; h <= kWeBars; ++h) {
                const f32 m = e.mag[(size_t)h];
                const f32 bx = p.x + bw * (f32)(h - 1);
                const f32 w0 = std::max(th1, bw - 0.4f * s);
                if (m <= 0.f) {                       // GONE, and it looks gone
                    rend_.rect({bx, p.bottom() - th1, w0, th1},
                               nx::line.alpha(0.55f));
                    continue;
                }
                const f32 y = dbToY(weMagToDb(m));
                const Col c0 = e.touched[h] ? nx::violet.alpha(0.85f)
                                            : nx::cyan.alpha(0.55f);
                rend_.rect({bx, y, w0, std::max(th1, p.bottom() - y)}, c0);
                rend_.rect({bx, y, w0, th1},
                           (e.touched[h] ? nx::violetSoft : nx::cyan).alpha(0.95f));
            }
            rend_.popClip();

            // What the bars cannot show, counted rather than quietly dropped.
            f32 tailPk = 0.f;
            for (int h = kWeBars + 1; h <= kWeHarm; ++h)
                if (e.mag[(size_t)h] > tailPk) tailPk = e.mag[(size_t)h];
            char tailT[16] = "silent";
            if (tailPk > 0.f)
                snprintf(tailT, sizeof tailT, "%.0f dB", (double)weMagToDb(tailPk));
            char lb[160];
            snprintf(lb, sizeof lb,
                     "h 257..1023 preserved, peak %s%s   -   %.0f px canvas",
                     tailT, e.previewing ? " - PREVIEWING" : "",
                     (double)(canR.h / s));
            microFit(ui_, fSmall_,
                     {canR.x + 6 * s, canR.bottom() - 11 * s, canR.w * 0.40f, 10 * s},
                     lb, nx::muted.alpha(0.35f), Align::Left, 0);

            if (hot) {
                const int hb = xToBar(in.mx);
                char db[16];
                if (e.mag[(size_t)hb] <= 0.f) snprintf(db, sizeof db, "gone");
                else snprintf(db, sizeof db, "%.1f dB", (double)weMagToDb(e.mag[(size_t)hb]));
                char t[460];
                snprintf(t, sizeof t,
                         "Harmonic pen, frame %d, harmonic %d of 256: %s%s. Drag "
                         "to draw, shift-drag for a slope, right-DRAG to remove, "
                         "wheel steps the frame. The bottom band is a HARD ZERO "
                         "and not -80 dB - dragging a bar there means the "
                         "harmonic is gone. Touching a bar rewrites that "
                         "harmonic's phase to sine and leaves every other "
                         "harmonic's complex value exactly alone.",
                         curF, hb, db, e.touched[hb] ? " (re-phased)" : "");
                ui_.tip = t;
            }
        }

        // ---- the sentence, on the surface it is about ----------------------
        if (!e.note.empty()) {
            const Col ink = e.noteInk == 1 ? nx::cyan.alpha(0.75f)
                          : e.noteInk == 2 ? nx::amber.alpha(0.90f)
                          : e.noteInk == 3 ? nx::violetSoft.alpha(0.80f)
                                           : nx::muted.alpha(0.55f);
            microFit(ui_, fSmall_,
                     {canR.x + canR.w * 0.44f, canR.bottom() - 11 * s,
                      canR.w * 0.56f - 6 * s, 10 * s},
                     e.note.c_str(), ink, Align::Right, 0);
        }
    }

    // =======================================================================
    // THE ASK -- the contract's own obligation, drawn over the controls
    //
    // "An editor closing with uncommitted changes MUST ask." It is in the
    // contract because the alternative is a user losing an hour of drawing to
    // a window close, and no amount of implementation care fixes a design that
    // permits it. It is drawn over the CONTROL column and not over the canvas
    // deliberately: what is at stake is what is on the canvas, so the canvas
    // stays visible while the question is answered.
    // =======================================================================
    if (e.askWhat) {
        const Rect ask{ctl.x, rowY(1), ctl.w, ctl.bottom() - rowY(1)};
        rend_.rect(ask, nx::panel2.alpha(0.94f));
        rend_.roundRectOutline(ask, nx::radiusSm * s, std::max(1.f, nx::snapPx(s)),
                               nx::amber.alpha(0.75f));
        const char* what = e.askWhat == 1 ? "leaving this page"
                         : e.askWhat == 2 ? "closing the panel"
                                          : "reading the other oscillator";
        char m[220];
        snprintf(m, sizeof m, "%s discards the drawing", what);
        ui_.microIn(fSmall_, {ask.x + 6 * s, ask.y + 4 * s, ask.w - 12 * s, 11 * s},
                    "UNCOMMITTED FRAMES", nx::amber, Align::Left, 0);
        microFit(ui_, fSmall_, {ask.x + 6 * s, ask.y + 17 * s, ask.w - 12 * s, 11 * s},
                 m, nx::text.alpha(0.9f), Align::Left, 0);
        microFit(ui_, fSmall_, {ask.x + 6 * s, ask.y + 29 * s, ask.w - 12 * s, 11 * s},
                 "a preview is not saved with the project",
                 nx::muted.alpha(0.7f), Align::Left, 0);
        const Rect br{ask.x + 4 * s, ask.bottom() - subH - 4 * s, ask.w - 8 * s, subH};
        const f32 bwid = br.w / 3.f;
        ui_.segCluster(br);
        static const char* const kAsk[3] = {"COMMIT & GO", "DISCARD", "STAY"};
        for (int k = 0; k < 3; ++k) {
            const Rect seg{br.x + bwid * (f32)k, br.y, bwid, br.h};
            if (k) rend_.hairlineV(seg.x, br.y + 2 * s, br.bottom() - 2 * s);
            const u64 wid = uiId(UiSpectraPanel, 1060 + k, uidKey);
            // DESTRUCTIVE FIRST is inverted here on purpose and it is the same
            // rule: the harmful button is DISCARD, so it is the one in the
            // MIDDLE, where a slip from either neighbour lands on a commit or
            // on doing nothing rather than on losing the drawing.
            if (ui_.grab(slop(seg)).segButton(wid, seg, false,
                                              k == 1 ? nx::amber : nx::violet))
                weAnswer = k == 0 ? 2 : (k == 1 ? 1 : 0);
            microFit(ui_, fSmall_, ui_.lastRect, kAsk[k],
                     (k == 1 ? nx::amber : nx::text).alpha(0.95f), Align::Center);
            if (ui_.hovered(seg))
                ui_.tip = k == 0 ? "Save the drawing as a table, then go."
                        : k == 1 ? "Throw the drawing away and go. It is not in "
                                   "the set and nothing can bring it back."
                                 : "Stay here.";
        }
    }

    // ---- the keyboard, and the two keys that are actually free -------------
    //
    // The contract offers three ways to move the cursor: the strip, [ / ], and
    // the arrow keys. TWO of them are here and the third is REFUSED, with the
    // reason on the strip's own tooltip: App::handleShortcuts() runs at the TOP
    // of the frame, before any view draws, and it already spends Left and Right
    // on the track selection and the playhead. A key that did two things at
    // once would be worse than a key that did one, so the arrows keep the job
    // they had and the brackets -- which nothing in this program binds -- take
    // this one.
    if (weLive && !ui_.keysOwned()) {
        if (in.keyPressed['[']) { weMoveCursor(e.cur - 1, "key ["); in.keyPressed['['] = false; }
        if (in.keyPressed[']']) { weMoveCursor(e.cur + 1, "key ]"); in.keyPressed[']'] = false; }
    }

    // ---- the ask, answered ------------------------------------------------
    // Last in the block, because two of the three answers RELEASE the working
    // copy and nothing above may still be holding a pointer into it.
    if (weAnswer >= 0) {
        const int what = e.askWhat, arg = e.askArg;
        bool go = true;
        if (weAnswer == 2) go = weCommit();          // a refusal keeps the ask up
        else if (weAnswer == 0) go = false;
        if (weAnswer == 0 || (weAnswer == 2 && !go)) {
            if (weAnswer == 0) e.askWhat = 0;
        } else {
            e.askWhat = 0;
            if (weAnswer == 1) {
                wtCancelPreview(inst, e.osc);
                if (probeOn())
                    LOGI("NXTAKT_DEBUG_PROBE: spectra editor cancelPreview osc %s "
                         "(discarded)", e.osc ? "B" : "A");
            }
            if (what == 1)      { g_page = arg; e.release(); }
            else if (what == 2) { closeDrop(); e.release();
                                  spectraOpenUid_ = 0; spectraForced_ = false; }
            else if (what == 3) { g_wt.osc = arg; weOpen(arg, "target switched"); }
        }
    }

    } // page branch

    // =======================================================================
    // THE PRESET POPOVER -- drawn after everything so it floats over the
    // panel, which is the entire point of it being a tier-2 sheet (§4: menus
    // are one of the few surfaces entitled to overlap content). The panel
    // underneath spent this frame shielded, so every gesture below is the
    // popover's alone.
    //
    // The interaction model, in full:
    //   open    click the name chip (or NXTAKT_DEBUG_SPECTRADROP standing in)
    //   pick    click a row, or Down/Up (Home/End) and Enter -- headers are
    //           skipped, the highlight follows the mouse only when the mouse
    //           moves, so the keyboard is never fighting a parked pointer
    //   scroll  the wheel over the list (consumed -- the strip must not also
    //           scroll), or the highlight walking off an edge
    //   close   Escape, click anywhere outside, picking, or the chip again
    //           (which is just "outside" while shielded)
    // =======================================================================
    shield.release();
    if (g_drop.open && np > 0 && g_page == 0) {
        // This popover owns the keyboard while it is up: Up/Down/Home/End/Enter
        // and Escape are ITS keys. The shell's shortcuts run at the TOP of the
        // frame, before this draws, so without the flag Enter both picks a
        // preset and launches the selected clip, and Escape both closes the
        // list and stops every clip in the set. Carried one frame by Ui, which
        // is right: a popover opened by a click was opened after the shortcuts
        // had already run anyway.
        ui_.keyModal = true;
        // The rows: presets in bank order, a header wherever the category tag
        // changes (see presetCatOf for how a NAME maps to a CATEGORY -- the
        // contract's "<TAG> <Name>" rule, nothing else). Rebuilt per frame:
        // fifty small structs, and the bank can change under us on reload.
        //
        // THE LIST GROUPS; IT NO LONGER SEGMENTS. v1 and v2 drew a header
        // "wherever the tag changes", which was correct while there was one
        // bank: the contract orders the file by category, so a tag change WAS a
        // category boundary. v3 ships a second factory bank of 48 appended
        // after the first -- the user-preset contract freezes factory ORDER, so
        // bank 2 cannot interleave -- and walking the list in index order would
        // then draw BASS · LEAD · PAD · KEYS · PLUCK · FX · SEQUENCE and then
        // all seven again. Two of every header is not a list, it is a bug
        // report.
        //
        // So the rows are BUCKETED: each category is drawn once, in the
        // contract's own tag order, with every preset that carries that tag
        // under it whichever bank it came from. Each row keeps its own bank
        // INDEX, which is the only thing loadPreset() cares about, and the
        // keyboard walks the drawn order rather than the index order because
        // it walks this vector -- the same property that made the v2 headers
        // skippable is what makes the regrouping free.
        //
        // v3 ADDS ONE MORE HEADER AND IT COMES FROM A NUMBER, not from a name.
        // The user bank has no naming rule and must not grow one -- a preset a
        // person named is a preset a person named -- so the boundary is
        // factoryPresetCount(), the one index the contract adds for exactly
        // this question. Everything at or past it files under "USER" and under
        // no tag, which is the contract's own instruction: "v3's popover draws
        // every user preset under one User header".
        struct Row { int preset; int cat; };            // preset < 0: a header
        constexpr int kUserCat = 7;                     // ...and cat 7 is "USER"
        const int fc = clampv(psFactoryCount(inst), 0, np);
        std::vector<Row> rows;
        rows.reserve((size_t)np + 9);
        // Untagged factory names first and headerless -- "Init", which the
        // contract exempts from the naming rule, and the v1 demo list, which
        // predates it. Bank order inside, because they have no other.
        for (int i = 0; i < fc; ++i)
            if (presetCatOf(presetNameOf(*inst, i)) < 0) rows.push_back({i, -1});
        for (int cIdx = 0; cIdx < 7; ++cIdx) {
            bool opened = false;
            for (int i = 0; i < fc; ++i) {
                if (presetCatOf(presetNameOf(*inst, i)) != cIdx) continue;
                if (!opened) { rows.push_back({-1, cIdx}); opened = true; }
                rows.push_back({i, cIdx});
            }
        }
        if (fc < np) {
            rows.push_back({-1, kUserCat});
            for (int i = fc; i < np; ++i) rows.push_back({i, -1});
        }
        const int nr = (int)rows.size();
        const auto isSel = [&](int i) { return i >= 0 && i < nr && rows[i].preset >= 0; };

        // Geometry: hung under the preset row, right-aligned to it (the row
        // lives in the last column, so growing leftward is growing inward),
        // clipped to the card -- a menu taller than its card scrolls, it does
        // not escape the panel.
        // 16 and not 15: a menu row is a click target like any other, and
        // this list is where a preset is picked by name -- the one gesture the
        // arrows beside it cannot do.
        const f32 rowHd = 16 * s;
        const f32 padY  = 3 * s;
        const f32 listW = (lay::spectraColW[6] + nx::sp4) * s;
        Rect listR{presetRowR.right() - listW, presetRowR.bottom() + 2 * s, listW, 0};
        listR.h = box.bottom() - 4 * s - listR.y;
        if (listR.h < rowHd * 3.f || presetRowR.w <= 0.f) {
            closeDrop();
        } else {
            const f32 contentH = padY * 2.f + rowHd * (f32)nr;
            const f32 maxScroll = std::max(0.f, contentH - listR.h);

            // The keyboard cursor. -1 is the open sentinel: resolve it to the
            // current preset so the list opens looking at where you are.
            if (!isSel(g_drop.cursor)) {
                g_drop.cursor = 0;
                for (int i = 0; i < nr; ++i)
                    if (rows[i].preset == spectraPreset_) { g_drop.cursor = i; break; }
                if (!isSel(g_drop.cursor))
                    for (int i = 0; i < nr; ++i) if (isSel(i)) { g_drop.cursor = i; break; }
            }
            const auto seek = [&](int from, int dir) {
                for (int i = from + dir; i >= 0 && i < nr; i += dir)
                    if (isSel(i)) return i;
                return from;
            };
            bool moved = false;
            if (in.keyPressed[KeyDown]) { g_drop.cursor = seek(g_drop.cursor, +1); moved = true; }
            if (in.keyPressed[KeyUp])   { g_drop.cursor = seek(g_drop.cursor, -1); moved = true; }
            if (in.keyPressed[KeyHome]) { g_drop.cursor = seek(-1, +1);            moved = true; }
            if (in.keyPressed[KeyEnd])  { g_drop.cursor = seek(nr, -1);            moved = true; }

            // Keep the highlight on screen: init centres the current preset,
            // a key move slides just enough.
            const auto rowY = [&](int i) { return padY + rowHd * (f32)i; };
            if (g_drop.scroll < 0.f)
                g_drop.scroll = rowY(g_drop.cursor) - listR.h * 0.5f + rowHd * 0.5f;
            if (moved) {
                if (rowY(g_drop.cursor) < g_drop.scroll)
                    g_drop.scroll = rowY(g_drop.cursor);
                if (rowY(g_drop.cursor) + rowHd > g_drop.scroll + listR.h)
                    g_drop.scroll = rowY(g_drop.cursor) + rowHd - listR.h;
            }
            if (ui_.hovered(listR) && in.wheel != 0.f) {
                g_drop.scroll -= in.wheel * rowHd * 3.f;
                in.wheel = 0.f;                     // consumed; see the strip
            }
            g_drop.scroll = clampv(g_drop.scroll, 0.f, maxScroll);

            // One verb for every way of picking, shared with the arrows above
            // via the device itself: the popover reloads through the same
            // loadPreset path, takes the same undo point, sets the same status.
            const auto pick = [&](int preset) {
                undoPoint("load preset");
                spectraPreset_ = clampv(preset, 0, np - 1);
                inst->loadPreset(spectraPreset_);
                status_ = std::string("Spectra: ") + presetNameOf(*inst, spectraPreset_);
                closeDrop();
            };

            if (in.keyPressed[KeyEscape]) closeDrop();
            if (g_drop.open && in.keyPressed[KeyEnter] && isSel(g_drop.cursor))
                pick(rows[g_drop.cursor].preset);
            if (g_drop.open && in.pressed[2]) closeDrop();

            if (g_drop.open) {
                // The sheet. Legibility rides the fill alpha (§4's menu note),
                // the finish is the lit edge and the sheet shadow.
                const f32 lrad = nx::radiusSm * s;
                rend_.shadow(listR, lrad, nx::shadowSheet);
                rend_.roundRect(listR, lrad, nx::panel2.alpha(0.96f));
                rend_.gradRect(listR, lrad, nx::glass2);
                rend_.gradStroke(listR, lrad, s, nx::edgeLit, 1.f);

                const bool mouseMoved = in.dx != 0.f || in.dy != 0.f;
                bool clickedInside = false;
                rend_.pushClip(listR);
                for (int i = 0; i < nr; ++i) {
                    const f32 y = listR.y + rowY(i) - g_drop.scroll;
                    if (y + rowHd < listR.y || y > listR.bottom()) continue;
                    const Rect rr{listR.x, y, listR.w, rowHd};
                    if (rows[i].preset < 0) {
                        // A category header: the tag's long form, and a
                        // hairline running out to the edge -- a shelf, not a row.
                        const char* cl = rows[i].cat == kUserCat
                            ? "USER"
                            : kPresetCat[clampv(rows[i].cat, 0, 6)].label;
                        ui_.microIn(fSmall_, {rr.x + 8 * s, rr.y, 80 * s, rr.h}, cl,
                                    nx::muted.alpha(0.75f), Align::Left, 0);
                        rend_.hairlineH(rr.x + 12 * s + ui_.microWidth(fSmall_, cl),
                                        rr.right() - 8 * s, std::round(rr.cy()));
                        continue;
                    }
                    if (mouseMoved && ui_.hovered(rr)) g_drop.cursor = i;
                    const bool cur = i == g_drop.cursor;
                    if (cur)
                        rend_.gradRect(rr.insetXY(3 * s, 0.5f * s), nx::radiusXs * s,
                                       nx::glassChip);
                    const bool loaded = rows[i].preset == spectraPreset_;
                    if (loaded)
                        rend_.rect({rr.x + 4 * s, rr.y + 3 * s,
                                    std::max(1.f, nx::snapPx(2 * s)), rr.h - 6 * s},
                                   nx::violet);
                    rend_.textIn(fSmall_, {rr.x + 16 * s, rr.y, rr.w - 22 * s, rr.h},
                                 presetNameOf(*inst, rows[i].preset),
                                 loaded ? nx::violetSoft
                                        : (cur ? nx::text : nx::muted.alpha(0.92f)),
                                 Align::Left, 0);
                    if (in.pressed[0] && ui_.hovered(rr)) {
                        clickedInside = true;
                        pick(rows[i].preset);
                    }
                }
                rend_.popClip();

                // The scroll's own evidence, when there is anything to scroll:
                // a 2px thumb in the right gutter, drawn not draggable -- the
                // wheel and the keys are the gesture, this is the map.
                if (maxScroll > 0.f && g_drop.open) {
                    const f32 gx = listR.right() - 4 * s;
                    const f32 th = listR.h * (listR.h / contentH);
                    const f32 ty = listR.y +
                        (listR.h - th) * (g_drop.scroll / maxScroll);
                    rend_.rect({gx, listR.y + 2 * s, 2 * s, listR.h - 4 * s},
                               nx::line.alpha(0.5f));
                    rend_.rect({gx, ty + 2 * s, 2 * s, std::max(8 * s, th) - 4 * s},
                               nx::violet.alpha(0.45f));
                }

                // Click-outside closes -- including the chip, which is how the
                // chip "toggles" while shielded. The click has already done
                // nothing else in the panel (everything was shielded), which is
                // what makes this dismissal and not dismissal-plus-surprise.
                // Never on the birth frame: see SpectraDrop::justOpened.
                if (!clickedInside && in.pressed[0] && !ui_.hovered(listR) &&
                    !g_drop.justOpened)
                    closeDrop();
                g_drop.justOpened = false;
            }
        }
    }

    // =======================================================================
    // DRAG-ASSIGN, the last two frames of it.
    //
    // Everything that CONSUMES the gesture is above: dropTarget() cleared
    // g_assign.src on a release over a control it could take. So a source still
    // in flight here is one whose release landed on nothing that could take it,
    // and §9's rule applies -- a gesture that did nothing says why it did
    // nothing rather than evaporating. The ghost is drawn last for the same
    // reason the popover is: it floats, and nothing under it should be able to
    // draw over the thing that is following the pointer.
    // =======================================================================
    //
    // THE REVERSE GESTURE ENDS THE SAME WAY. "Assign to matrix slot" leaves a
    // destination waiting for a source, and a wait with no way out is a mode --
    // the one thing this panel has refused to grow anywhere else. Escape, a
    // right-click and a click that landed on no source handle all end it, and
    // each of them says so; the ghost that follows the pointer is the drag's
    // own, carrying the DESTINATION's name instead of the source's, because
    // that is what is in your hand.
    if (g_assign.wantDest > 0) {
        const char* dn = kMatrixDst[clampv(g_assign.wantDest, 0, kDstCount - 1)];
        if (in.keyPressed[KeyEscape] || in.pressed[2]) {
            status_ = std::string("Spectra: ") + dn + " was not assigned";
            g_assign.wantDest = g_assign.wantParam = -1;
        } else if (in.pressed[0]) {
            // A press that reached here is a press no source handle took --
            // srcHandle() clears wantDest on the frame it completes one.
            char msg[192];
            snprintf(msg, sizeof msg,
                     "Spectra: %s was not assigned - the source is a grip handle "
                     "(the LFOs, ENV 2/3, the macros, wheel/bend/cc, the arp "
                     "pattern)", dn);
            status_ = msg;
            g_assign.wantDest = g_assign.wantParam = -1;
        } else {
            ui_.cursor = Cursor::Grab;
            const f32 gw = ui_.microWidth(fSmall_, dn) + 14 * s, gh = 14 * s;
            const Rect ghost{in.mx + 11 * s, in.my + 11 * s, gw, gh};
            const f32 grad = nx::radiusXs * s;
            rend_.shadow(ghost, grad, nx::shadowSheet);
            rend_.roundRect(ghost, grad, nx::panel2.alpha(0.96f));
            rend_.gradRect(ghost, grad, nx::glassChip);
            rend_.gradStroke(ghost, grad, s, nx::edgeLit, 1.f);
            ui_.microIn(fSmall_, ghost, dn, nx::cyan, Align::Center);
        }
    }
    if (g_assign.src >= 0) {
        const char* nm = kMatrixSrc[clampv(g_assign.src, 0, kSrcCount - 1)];
        if (in.keyPressed[KeyEscape] || in.pressed[2]) {
            g_assign.src = -1;
            status_ = std::string("Spectra: ") + nm + " was not assigned";
        } else if (in.released[0]) {
            char msg[176];
            snprintf(msg, sizeof msg,
                     "Spectra: %s was not assigned - a modulation target is a "
                     "knob with a mod ring, and Pan is matrix-only", nm);
            status_ = msg;
            g_assign.src = -1;
        } else {
            ui_.cursor = Cursor::Grab;
            const f32 gw = ui_.microWidth(fSmall_, nm) + 14 * s, gh = 14 * s;
            const Rect ghost{in.mx + 11 * s, in.my + 11 * s, gw, gh};
            const f32 grad = nx::radiusXs * s;
            rend_.shadow(ghost, grad, nx::shadowSheet);
            rend_.roundRect(ghost, grad, nx::panel2.alpha(0.96f));
            rend_.gradRect(ghost, grad, nx::glassChip);
            rend_.gradStroke(ghost, grad, s, nx::edgeLit, 1.f);
            ui_.microIn(fSmall_, ghost, nm, nx::text, Align::Center);
        }
    }
    // AMOUNT MODE ends the moment your attention does: a click that was not on
    // the knob, an Escape, a page turn that took the knob off screen. It never
    // ends on a timer, because a control that stops being what it says it is
    // after two seconds is worse than one that never said it.
    if (g_assign.tuneFresh) {
        g_assign.tuneFresh = false;              // the drop's own frame, exempt
    } else if (g_assign.tuneParam >= 0 &&
               (!tuneDrawn || in.keyPressed[KeyEscape] ||
                (in.pressed[0] && !tuneTouched))) {
        g_assign.tuneParam = -1;
        g_assign.tuneSlot = -1;
        g_assign.tuneDest = -1;
    }

    rend_.popClip();
}

// ---------------------------------------------------------------------------
// The headless hooks
//
// Nothing inside gamescope can click an "edit" chip, and this panel is
// otherwise unreachable from a screenshot. Both hooks drive the ORDINARY code
// path -- the same uid the mouse would set, the same commit() the trough calls
// -- so what they prove is what the mouse would do, not a back door beside it.
//
//   NXTAKT_DEBUG_SPECTRA=1        open the panel on the first device of the
//                                 chain the DEVICES tab is on, WHATEVER that
//                                 device is. Pointed at a Spectra it is the
//                                 real panel; pointed at anything else it is
//                                 the guarded state, which is a screenshot
//                                 worth having in its own right.
//   NXTAKT_DEBUG_SPECTRAPOS=<t>   write A Position through the slider's own
//                                 path (undo point, setParam, autoCapture).
//   NXTAKT_DEBUG_SPECTRASET=      the same writer, for any list of contract
//     id:value,id:value,...       ids -- which is how the states a screenshot
//                                 needs (osc B audible, a depth off its
//                                 detent) get made without a mouse.
//   NXTAKT_DEBUG_SPECTRAPRESET=   load a factory preset by index or by any
//     <index | name fragment>     part of its name, through the preset row's
//                                 own path.
//   NXTAKT_DEBUG_SPECTRAPAGE=<n>  start on page n (0 MAIN, 1 MOD, 2 ARP) --
//                                 the tab the mouse would click.
//   NXTAKT_DEBUG_SPECTRADROP=1    open the preset popover on the first frame,
//                                 through the chip's own openDrop path, so the
//                                 keyboard navigation the screenshots assert
//                                 (Down Down, Enter, Escape) runs against the
//                                 exact state a click would have made.
// ---------------------------------------------------------------------------
void App::debugSeedSpectra() {
    if (spectraSeeded_) return;
    const char* want = env("DEBUG_SPECTRA");
    if (!want || !*want || want[0] == '0') return;
    spectraSeeded_ = true;

    ChainOwner co = chainOwner(devOwner_);
    if (!co.devices || co.devices->empty()) {
        LOGW("NXTAKT_DEBUG_SPECTRA: no device on %s to open a panel on",
             ownerName(devOwner_).c_str());
        return;
    }
    DeviceModel& d = (*co.devices)[0];
    if (!d.inst) {
        LOGW("NXTAKT_DEBUG_SPECTRA: the first device has no instance");
        return;
    }
    spectraOpenUid_ = d.uid;
    spectraForced_  = !isSpectra(d.inst.get());
    spectraScrollTo_ = true;
    selDevice_ = 0;
    paramScroll_ = 0.f;
    // Ctrl+B's other half, done for the shot: the panel is as wide as the strip
    // and the file browser is 210 logical pixels of it. This is the hook
    // standing in for a mouse, exactly as the rack seed stands in for eight
    // double-clicks -- it presses a key the user has.
    showBrowser_ = false;
    LOGI("NXTAKT_DEBUG_SPECTRA: panel open on %s (%d parameters)%s",
         d.desc.name.c_str(), d.inst->paramCount(),
         spectraForced_ ? " - not a Spectra, so the guards are what is on screen" : "");
    status_ = spectraForced_ ? "NXTAKT_DEBUG_SPECTRA: panel forced onto " + d.desc.name
                             : "NXTAKT_DEBUG_SPECTRA: Spectra panel open";

    // The page and the popover, before anything is drawn: g_pageUid is primed
    // with the panel's uid so the first frame's freshness check does not reset
    // what the hook just chose.
    if (const char* p = env("DEBUG_SPECTRAPAGE")) {
        g_pageUid = d.uid;
        g_page = clampv(atoi(p), 0, 3);          // 0 MAIN, 1 MOD, 2 ARP, 3 DRAW
        LOGI("NXTAKT_DEBUG_SPECTRAPAGE: page %d", g_page);
    }
    // v5's two, on the same terms as every hook above: each one presses a
    // control the mouse has, and neither is a back door beside one. There is
    // deliberately no hook that WRITES frames -- the pens are the pens, and a
    // seeded drawing would prove that this file can fill an array.
    if (const char* p = env("DEBUG_SPECTRAWTVIEW")) {
        g_weSeedView = clampv(atoi(p), 0, 1);
        LOGI("NXTAKT_DEBUG_SPECTRAWTVIEW: the editor opens on the %s pen",
             g_weSeedView ? "harmonic" : "waveform");
    }
    if (const char* p = env("DEBUG_SPECTRAWTFRAME")) {
        g_weSeedFrame = clampv(atoi(p), 0, kWeFrames - 1);
        LOGI("NXTAKT_DEBUG_SPECTRAWTFRAME: the frame cursor starts at %d", g_weSeedFrame);
    }
    if (const char* p = env("DEBUG_SPECTRADROP")) {
        if (*p && *p != '0') {
            g_pageUid = d.uid;
            g_drop.pending = true;
            LOGI("NXTAKT_DEBUG_SPECTRADROP: popover will open on first draw");
        }
    }

    // --- v3's own hooks, on the same terms: each one presses a control the
    // --- mouse has, and none of them is a back door beside one.
    if (const char* p = env("DEBUG_SPECTRAWTOSC")) {
        g_wt.osc = clampv(atoi(p), 0, 1);
        LOGI("NXTAKT_DEBUG_SPECTRAWTOSC: the import row targets osc %s",
             g_wt.osc ? "B" : "A");
    }
    if (const char* p = env("DEBUG_SPECTRAWTDRAG")) {
        g_wt.dragHold = p;                       // the panel re-arms it every frame
        LOGI("NXTAKT_DEBUG_SPECTRAWTDRAG: a browser drag holding %s is in flight", p);
    }
    if (const char* p = env("DEBUG_SPECTRASAVE")) {
        if (*p && *p != '0') {
            g_pageUid = d.uid;
            g_save.pending = true;
            LOGI("NXTAKT_DEBUG_SPECTRASAVE: the name field will open on first draw");
        }
    }
    // A drag-assign in flight, held the way the mouse would hold it: the source
    // is armed and the pointer is wherever xdotool put it, so what the shot
    // photographs is the target framing the real gesture produces.
    if (const char* p = env("DEBUG_SPECTRAASSIGN")) {
        g_assign.src = clampv(atoi(p), 0, kSrcCount - 1);
        LOGI("NXTAKT_DEBUG_SPECTRAASSIGN: dragging matrix source %d (%s)",
             g_assign.src, kMatrixSrc[g_assign.src]);
    }

    // Both writers go through here, which is the trough's commit() line for
    // line: undo point, setParam, capture. A hook that wrote the parameter
    // directly would prove the plugin accepts a value and nothing about the
    // path the mouse takes to it.
    const auto write = [&](int id, f32 v, const char* what) {
        if (id < 0 || id >= d.inst->paramCount()) {
            LOGW("NXTAKT_DEBUG_SPECTRA: this device has no parameter %d", id);
            return;
        }
        undoPoint(what);
        d.inst->setParam(id, v);
        if (ownIsTrack(devOwner_))
            autoCapture(addr::deviceParam(ses_.tracks[devOwner_].uid, d.uid,
                                          d.inst->paramInfo(id).id), v, 0);
        LOGI("NXTAKT_DEBUG_SPECTRA: %s (id %d) -> %g, reads back %g", what, id,
             (double)v, (double)d.inst->getParam(id));
    };

    // The Position sweep, through the trough's own code path.
    if (const char* p = env("DEBUG_SPECTRAPOS"))
        write(pAPos, clampv((f32)atof(p), 0.f, 1.f), "A Position");

    // A preset, by index or by any part of its name. The panel reads every
    // control off getParam each frame, so a load that rewrites the whole
    // parameter block needs nothing refreshed -- which is the immediate-mode
    // part of this design paying for itself, and the property this hook is
    // here to show rather than assert.
    if (const char* p = env("DEBUG_SPECTRAPRESET")) {
        const int np = d.inst->presetCount();
        int want2 = -1;
        if (np <= 0) {
            LOGW("NXTAKT_DEBUG_SPECTRAPRESET: %s declares no presets", d.desc.name.c_str());
        } else if (*p >= '0' && *p <= '9') {
            want2 = clampv(atoi(p), 0, np - 1);
        } else {
            for (int i = 0; i < np && want2 < 0; ++i)
                if (icontains(presetNameOf(*d.inst, i), p)) want2 = i;
            if (want2 < 0) LOGW("NXTAKT_DEBUG_SPECTRAPRESET: no preset matching \"%s\"", p);
        }
        if (want2 >= 0) {
            undoPoint("load preset");
            spectraPreset_ = want2;
            d.inst->loadPreset(want2);
            LOGI("NXTAKT_DEBUG_SPECTRAPRESET: %d/%d \"%s\" loaded", want2 + 1, np,
                 presetNameOf(*d.inst, want2));
            status_ = std::string("Spectra: ") + presetNameOf(*d.inst, want2);
        }
    }

    // ...and the general form, for the states a screenshot needs and a mouse
    // would otherwise have to be there to make: "id:value,id:value,...".
    if (const char* set = env("DEBUG_SPECTRASET")) {
        std::string list = set;
        size_t start = 0;
        while (start < list.size()) {
            const size_t comma = list.find(',', start);
            const std::string one =
                list.substr(start, comma == std::string::npos ? std::string::npos
                                                              : comma - start);
            start = comma == std::string::npos ? list.size() : comma + 1;
            const size_t colon = one.find(':');
            if (colon == std::string::npos) {
                LOGW("NXTAKT_DEBUG_SPECTRASET: \"%s\" is not id:value", one.c_str());
                continue;
            }
            write(atoi(one.c_str()), (f32)atof(one.c_str() + colon + 1), "debug set");
        }
    }
}

} // namespace lat
