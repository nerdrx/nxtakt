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

// The eight tables, by index. UI labels, per the contract's own wording.
const char* const kTables[8] = {"Basic", "PWM", "Harmonic", "Formant",
                                "Bell",  "Digital", "Vox",  "Fold"};
// LFO Sync, 0..9. 0 is free-run and the rate knob owns the readout then; every
// other entry is a division and the knob shows it instead of a frequency.
// Shared verbatim by LFO2 and LFO3 -- the contract says "division table of
// id 33, verbatim", and one array is how two things stay verbatim.
const char* const kSyncDiv[10] = {"free", "4 bars", "2 bars", "1 bar", "1/2",
                                  "1/4",  "1/8",    "1/16",   "1/4T",  "1/8T"};
const char* const kShapeName[5] = {"sine", "tri", "saw", "square", "S&H"};

// v2 enums, straight off the contract's tables.
const char* const kSubShape[3]  = {"sine", "tri", "sqr"};
const char* const kWarpMode[8]  = {"Off",      "Sync", "Bend+", "Bend-",
                                   "Mirror",   "Quantize", "FM", "RM"};
const char* const kVoiceMode[3] = {"poly", "mono", "leg"};
// Matrix sources 0..13 and destinations 0..19, in the contract's order. The
// spellings are display labels cut to what a 50px selector can carry; the
// tooltip says the long form.
const char* const kMatrixSrc[14] = {
    "Off",     "LFO 1",   "LFO 2",   "LFO 3",   "ENV 2",   "ENV 3",  "Velo",
    "KeyTrk",  "AfterT",  "Macro 1", "Macro 2", "Macro 3", "Macro 4", "Random"};
const char* const kMatrixDst[20] = {
    "Off",     "A Pos",   "B Pos",   "A Warp",  "B Warp",  "A Level", "B Level",
    "A Pitch", "B Pitch", "Sub",     "Noise",   "Cutoff",  "Reso",    "Drive",
    "A Det",   "B Det",   "Pan",     "L1 Rate", "L2 Rate", "L3 Rate"};

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

// PRESETS. host.h's contract says presetName() may return null out of range,
// and "out of range" includes every index on every device that has no presets
// -- which is all of them but this one. So the null is handled here, once, and
// nothing below has to remember that a name can be absent.
const char* presetNameOf(const PluginInstance& p, int i) {
    const char* n = p.presetName(i);
    return n ? n : "?";
}

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
//     2          the MAIN/MOD page tab (tabPill derives its slot ids itself)
//    60..63      the preset row: prev, next, the name chip, the popover latch
//   100+id*4+k   a stepper on param `id` (k: 0 gesture, 1 prev, 2 next)
//   600..605     the filter-type segments
//    50..54      LFO1's shape segments (v1's numbers, kept)
//   630..644     LFO2 (630+) and LFO3 (640+) shape segments
//   650..652     the sub shape segments
//   660..662     the voice mode segments
//   670          the noise-track toggle
//   700..707     matrix source selectors, 720..727 destination selectors
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
        if (ui_.editId == dropId) ui_.editId = 0;
    }
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
    Rect title{box.x, box.y, box.w, 16 * s};
    rend_.rect({title.x + 3 * s, title.y + 4 * s, std::max(1.f, nx::snapPx(3 * s)),
                title.h - 8 * s}, tc);

    Rect closeR{title.right() - 17 * s, title.y + 2 * s, 14 * s, 12 * s};
    if (ui_.button(uiId(UiSpectraPanel, 0, 0), closeR, "")) {
        closeDrop();
        spectraOpenUid_ = 0;
        spectraForced_ = false;
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
        static const char* const kPages[2] = {"MAIN", "MOD"};
        Rect tabR{title.x + 76 * s, title.y + 1.5f * s, 88 * s, 13 * s};
        int page = g_page;
        if (ui_.tabPill(uiId(UiSpectraPanel, 2, uidKey), tabR, kPages, 2, &page) &&
            page != g_page) {
            closeDrop();
            g_page = page;
        }
        if (ui_.hovered(tabR))
            ui_.tip = "MAIN is the v1 face; MOD is the parity push - sub & noise, "
                      "warp, LFO 2/3, ENV 3, the matrix, macros, voice mode";
    }

    // §9: say what happened, in amber, in one line. Three states can be on
    // screen and each names itself: forced onto a non-Spectra (the debug
    // hook's doing), or a real Spectra whose DSP predates part of the contract
    // -- in which case every missing id draws as an inert socket below and
    // this line is the reason why.
    {
        const Rect noteR{title.x + 172 * s, title.y, closeR.x - title.x - 178 * s, title.h};
        char note[128];
        if (!real) {
            snprintf(note, sizeof note, "panel forced onto %s - %d of %d parameters",
                     dm.desc.name.c_str(), pc, (int)pCountV2);
            microFit(ui_, fSmall_, noteR, note, nx::amber.alpha(0.9f), Align::Left, 0);
        } else if (pc < (int)pCountV2) {
            snprintf(note, sizeof note,
                     "DSP has %d of %d parameters - newer controls are inert sockets",
                     pc, (int)pCountV2);
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
    const f32 subH  = 14 * s;                  // a selector / cluster row
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
    for (int i = 1; i < kCols; ++i)
        rend_.hairlineV(colX[i] - colGap * 0.5f, body.y + 2 * s, body.bottom() - 2 * s);

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
        if (ownTrack)
            autoCapture(addr::deviceParam(ses_.tracks[devOwner_].uid, dm.uid,
                                          inst->paramInfo(id).id), v, gesture);
    };

    // A knob cell: the control, its readout (drawn by the widget) and its name
    // underneath. `st` carries the contract's own range, curve and centre.
    const auto knob = [&](const Rect& cell, int id, const char* label,
                          Ui::KnobStyle st, f32 dim) {
        st.dim = dim;
        st.absent = !has(id);
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
        if (ui_.knobNx(wid, kr, &v, st)) commit(id, v, wid, label);
        microFit(ui_, fSmall_, {cell.x, cell.bottom() - lblH, cell.w, lblH}, label,
                 nx::muted.alpha((st.absent ? 0.40f : 0.85f) * dim), Align::Center);
        // The label is cut short to fit 46 logical pixels, so the tooltip is
        // where the parameter says its whole name, its range and its unit.
        if (ui_.hovered(kr)) {
            char t[128];
            if (st.absent)
                snprintf(t, sizeof t, "%s: this device has no parameter %d", label, id);
            else
                snprintf(t, sizeof t, "%s  %g .. %g %s%s%s  (id %d)", label,
                         (double)st.lo, (double)st.hi,
                         inst->paramInfo(id).unit.c_str(), st.log ? " log" : "",
                         st.bipolar ? " bipolar" : "", id);
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
                             const char* tip) {
        ui_.segCluster(r0);
        const f32 bw = 14 * s;
        Rect lb{r0.x, r0.y, bw, r0.h}, rb{r0.right() - bw, r0.y, bw, r0.h};
        rend_.hairlineV(lb.right(), r0.y + 2 * s, r0.bottom() - 2 * s);
        rend_.hairlineV(rb.x, r0.y + 2 * s, r0.bottom() - 2 * s);
        const bool live = has(id) && count > 0;
        int idx = live ? clampv((int)std::lround(get(id, 0.f)), 0, count - 1) : 0;
        const auto step = [&](int d) {
            if (!live) return;
            const int n = ((idx + d) % count + count) % count;
            commit(id, (f32)n, uiId(UiSpectraPanel, 100 + id * 4, uidKey), what);
        };
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
            if (ui_.segButton(uiId(UiSpectraPanel, 100 + id * 4 + 1, uidKey), lb,
                              false, nx::violet)) step(-1);
            chev(ui_.lastRect, true);
            if (ui_.segButton(uiId(UiSpectraPanel, 100 + id * 4 + 2, uidKey), rb,
                              false, nx::violet)) step(+1);
            chev(ui_.lastRect, false);
        } else {
            chev(lb, true);
            chev(rb, false);
        }
        const Rect nameR{lb.right(), r0.y, rb.x - lb.right(), r0.h};
        microFit(ui_, fSmall_, nameR, live ? names[idx] : absentText,
                 (live ? nx::text : nx::muted.alpha(0.45f)).alpha(dim), Align::Center);
        if (tip && ui_.hovered(r0)) {
            if (live) ui_.tip = tip;
            else ui_.tip = std::string("This device has no parameter ") +
                           std::to_string(id);
        }
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
    const auto envRow = [&](const Rect& c, f32 y, const char* name, int base,
                            f32 dA, f32 dD, f32 dS, f32 dR) {
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
    const auto shapeCluster = [&](const Rect& shR, int id, int segBase,
                                  const char* what) {
        ui_.segCluster(shR);
        const f32 sw = shR.w / 5.f;
        const bool live = has(id);
        const int shape = live ? clampv((int)std::lround(get(id, 0.f)), 0, 4) : -1;
        for (int k = 0; k < 5; ++k) {
            const Rect seg{shR.x + sw * (f32)k, shR.y, sw, shR.h};
            if (k) rend_.hairlineV(seg.x, shR.y + 2 * s, shR.bottom() - 2 * s);
            const bool on = k == shape;
            Rect g = seg;
            if (live) {
                const u64 wid = uiId(UiSpectraPanel, segBase + k, uidKey);
                if (ui_.segButton(wid, seg, on, nx::violet))
                    commit(id, (f32)k, wid, what);
                g = ui_.lastRect;
            }
            const Col ic = on ? nx::text : nx::muted.alpha(live ? 0.85f : 0.40f);
            const f32 w2 = 6.f * s, h2 = 3.2f * s, th = 1.1f * s;
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
            default:                                    // sample & hold
                rend_.line(gx - w2, gy + h2, gx - w2 * 0.33f, gy + h2, th, ic);
                rend_.line(gx - w2 * 0.33f, gy - h2, gx + w2 * 0.33f, gy - h2, th, ic);
                rend_.line(gx + w2 * 0.33f, gy + h2 * 0.3f, gx + w2, gy + h2 * 0.3f, th, ic);
                break;
            }
        }
        if (ui_.hovered(shR))
            ui_.tip = live ? std::string(what) + ": " +
                             (shape >= 0 ? kShapeName[shape] : "?")
                           : std::string("This device has no parameter ") +
                             std::to_string(id);
        return shape;
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
        const Rect dispR{c.x, y0 + 2 * s, c.w,
                         std::max(24 * s, c.bottom() - (y0 + 2 * s) - (slidH * 2.f + 10 * s))};
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
            if (has(pBTable) && has(pBPos) && bLevel > 1e-4f)
                trace(clampv((int)std::lround(get(pBTable, 0.f)), 0, 7), posB,
                      nx::violetSoft.alpha(0.75f), 1.2f * s);
            // §1: cyan is the light inside the material. This is the live shape
            // the instrument is standing on, so it is the one cyan thing here.
            trace(clampv((int)std::lround(get(pATable, 0.f)), 0, 7), posA,
                  nx::cyan, 1.5f * s);

            // The legend, so the two traces are named rather than guessed at.
            ui_.microIn(fSmall_, {dispR.x + 6 * s, dispR.y + 1 * s, 40 * s, 10 * s},
                        "A", nx::cyan.alpha(0.9f), Align::Left, 0);
            if (has(pBTable) && bLevel > 1e-4f)
                ui_.microIn(fSmall_, {dispR.x + 16 * s, dispR.y + 1 * s, 40 * s, 10 * s},
                            "B", nx::violetSoft.alpha(0.9f), Align::Left, 0);
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
            char wlabel[48];
            if (tset)
                snprintf(wlabel, sizeof wlabel, "frame %.1f / %d%s",
                         (double)(posA * (f32)(tset->frames - 1)), tset->frames,
                         warpOn ? " · pre-warp" : "");
            else
                snprintf(wlabel, sizeof wlabel, "illustration only");
            microFit(ui_, fSmall_,
                     {dispR.x, dispR.bottom() - 11 * s, dispR.w - 6 * s, 10 * s},
                     wlabel, nx::muted.alpha(0.35f), Align::Right, 0);
            if (ui_.hovered(dispR))
                ui_.tip = tset
                    ? (warpOn
                       ? "The wavetable frames themselves, morphed at Position - "
                         "drawn BEFORE the warp stage reshapes the read phase"
                       : "The wavetable frames themselves, morphed between the two "
                         "Position falls between - the same read the voices make")
                    : "No wavetable set in this process, so this is a drawing of "
                      "the same family of shapes and not the tables";
        }

        // The two Position troughs. THE control, per the contract, so it gets a
        // trough of its own rather than a knob in the osc column.
        const auto posRow = [&](f32 y, int id, const char* label, const Col& ink, f32 dim) {
            const Rect lr{c.x, y, 12 * s, slidH};
            const Rect vr{c.right() - 26 * s, y, 26 * s, slidH};
            const Rect tr{lr.right() + 2 * s, y + 2 * s,
                          vr.x - lr.right() - 6 * s, slidH - 4 * s};
            microFit(ui_, fSmall_, lr, label, ink.alpha(dim), Align::Left, 0);
            if (!has(id)) {
                rend_.well(tr, nx::radiusXs * s, true);
                rend_.textIn(fSmall_, vr, "-", nx::muted.alpha(0.4f), Align::Right, 0);
                return;
            }
            f32 v = clampv(inst->getParam(id), 0.f, 1.f);
            const u64 wid = uiId(UiSpectraPos, id, uidKey);
            if (ui_.trough(wid, tr, &v, 0.f, 1.f, nx::cyan, dim))
                commit(id, v, wid, "Position");
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
        stepper({c.x, y0, c.w, subH}, base + 0, 8, kTables, dim, "no table",
                "Table", "The oscillator's wavetable - the arrows cycle the eight");

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
                if (ui_.segButton(wid, seg, on, nx::violet))
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
        envRow(c, er2, "ENV 2", pE2Attack, 5.f, 200.f, 0.7f, 300.f);
    }

    // =======================================================================
    // 5. LFO
    // =======================================================================
    {
        const Rect c = col(5);
        const f32 y0 = sect(c, "LFO", 1.f);
        shapeCluster({c.x, y0, c.w, subH}, pLfoShape, 50, "LFO Shape");

        const f32 cw = c.w / 3.f;
        const f32 r1 = y0 + subH + gap, r2 = r1 + rowH + gap;

        // The sync selector takes two cells of the second row and is a
        // 14px control inside a 48px band, so it is centred in it rather than
        // hung off the top -- the row is what the eye aligns on.
        const Rect syncR{c.x, r2 + (rowH - lblH - subH) * 0.5f, cw * 2.f, subH};
        const int sync = stepper(syncR, pLfoSync, 10, kSyncDiv, 1.f, "no sync",
                                 "Sync",
                                 "LFO sync: free-running, or a division of the "
                                 "transport's tempo");
        microFit(ui_, fSmall_, {c.x, r2 + rowH - lblH, cw * 2.f, lblH}, "sync",
                 nx::muted.alpha(0.85f), Align::Center);

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
            if (ui_.segButton(uiId(UiSpectraPanel, 60, uidKey), lb, false, nx::violet))
                loadIdx(spectraPreset_ - 1);
            chev(ui_.lastRect, true);
            if (ui_.segButton(uiId(UiSpectraPanel, 61, uidKey), rb, false, nx::violet))
                loadIdx(spectraPreset_ + 1);
            chev(ui_.lastRect, false);

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
            if (ui_.segButton(uiId(UiSpectraPanel, 62, uidKey), nameR, g_drop.open,
                              nx::violet) && !g_drop.open)
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
            if (ui_.hovered(pr)) {
                char t[96];
                snprintf(t, sizeof t,
                         "Preset %d of %d - click the name for the list, arrows step",
                         spectraPreset_ + 1, np);
                ui_.tip = t;
            }
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
        knob({c.x + cw * 0.5f, r2, cw, rowH}, pNoise, "noise", st, 1.f);
        knob({c.x + cw * 1.5f, r2, cw, rowH}, pSub, "sub", st, 1.f);
    }

    } else {
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
                    if (ui_.segButton(wid, seg, on, nx::violet))
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
                if (ui_.segButton(wid, tg, on, nx::violet))
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
                    if (ui_.segButton(wid, seg, on, nx::violet))
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
    const auto lfoColumn = [&](int ci, const char* label, int rateId, int syncId,
                               int shapeId, int segBase, const char* whatShape) {
        const Rect c = col(ci);
        const f32 y0 = sect(c, label, 1.f);
        shapeCluster({c.x, y0, c.w, subH}, shapeId, segBase, whatShape);

        const f32 cw = c.w / 3.f;
        const f32 r1 = y0 + subH + gap, r2 = r1 + rowH + gap;
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
    lfoColumn(2, "LFO 2", pL2Rate, pL2Sync, pL2Shape, 630, "L2 Shape");
    lfoColumn(3, "LFO 3", pL3Rate, pL3Sync, pL3Shape, 640, "L3 Shape");

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
        envRow(c, er1, "ENV 3", pE3Attack, 2.f, 300.f, 0.f, 150.f);
        rend_.hairlineH(c.x, c.right(), er2 - 2 * s);
        const f32 kw = c.w / 4.f;
        Ui::KnobStyle st;
        st.lo = 0.f; st.hi = 1.f; st.def = 0.f; st.fmt = "%.2f";
        static const char* const kMacroLbl[4] = {"macro 1", "macro 2",
                                                 "macro 3", "macro 4"};
        for (int m = 0; m < 4; ++m)
            knob({c.x + kw * (f32)m, er2, kw, rowH}, pMacro1 + m, kMacroLbl[m], st, 1.f);
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

        // The micro-selector. `sub` is the panel sub-id, `what` the undo label.
        const auto enumSel = [&](int sub, const Rect& r0, int id,
                                 const char* const* names, int count,
                                 const char* what, f32 dim) {
            const bool live = has(id);
            rend_.well(r0, nx::radiusXs * s, false);
            int idx = live ? clampv((int)std::lround(get(id, 0.f)), 0, count - 1) : 0;
            if (live) {
                const u64 wid = uiId(UiSpectraPanel, sub, uidKey);
                if (ui_.setHot(wid, r0) && ui_.isHot(wid)) {
                    ui_.cursor = Cursor::Hand;
                    int d = 0;
                    if (in.pressed[0]) d = +1;
                    if (in.pressed[2]) d = -1;
                    if (in.wheel != 0.f) {
                        d = in.wheel > 0.f ? +1 : -1;
                        in.wheel = 0.f;          // not the strip's notch to spend
                    }
                    if (d) commit(id, (f32)(((idx + d) % count + count) % count),
                                  wid, what);
                    char t[112];
                    snprintf(t, sizeof t,
                             "%s: %s - click next, right-click back, wheel steps",
                             what, names[idx]);
                    ui_.tip = t;
                }
            } else if (ui_.hovered(r0)) {
                char t[80];
                snprintf(t, sizeof t, "%s: this device has no parameter %d", what, id);
                ui_.tip = t;
            }
            const Col ink = !live ? nx::muted.alpha(0.40f)
                          : idx == 0 ? nx::muted.alpha(0.55f * dim + 0.30f)
                                     : nx::text.alpha(dim);
            microFit(ui_, fSmall_, r0, live ? names[idx] : "-", ink, Align::Center);
            return idx;
        };

        for (int k = 0; k < 8; ++k) {
            const int bank = k / 4;                      // 0 left, 1 right
            const Rect sr{colX[5 + bank], top + rh * (f32)(k % 4),
                          kColW[5 + bank] * s, rh};
            const int sid = pM1Src + 3 * k, did = sid + 1, aid = sid + 2;
            const bool absent = !has(sid);
            const int src = absent ? 0
                : clampv((int)std::lround(get(sid, 0.f)), 0, 13);
            const int dst = !has(did) ? 0
                : clampv((int)std::lround(get(did, 0.f)), 0, 19);
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
            enumSel(700 + k, srcR, sid, kMatrixSrc, 14, wsrc, sdim);
            enumSel(720 + k, dstR, did, kMatrixDst, 20, wdst, sdim);

            // The depth: a small bipolar knob with no readout of its own (a
            // 26px cell has no line to spare) -- the tooltip is the readout.
            Ui::KnobStyle st;
            st.lo = -1.f; st.hi = 1.f; st.def = 0.f; st.bipolar = true;
            st.arc = nx::violetSoft; st.fmt = nullptr;
            st.dim = sdim; st.absent = !has(aid);
            f32 v = has(aid) ? inst->getParam(aid) : 0.f;
            const u64 wid = uiId(UiSpectraKnob, aid, uidKey);
            if (ui_.knobNx(wid, knR, &v, st)) commit(aid, v, wid, wamt);
            if (ui_.hovered(knR)) {
                char t[112];
                if (st.absent)
                    snprintf(t, sizeof t, "%s: this device has no parameter %d",
                             wamt, aid);
                else
                    snprintf(t, sizeof t, "%s %+.2f  (%s -> %s)", wamt, (double)v,
                             kMatrixSrc[src], kMatrixDst[dst]);
                ui_.tip = t;
            }
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
        // The rows: presets in bank order, a header wherever the category tag
        // changes (see presetCatOf for how a NAME maps to a CATEGORY -- the
        // contract's "<TAG> <Name>" rule, nothing else). Rebuilt per frame:
        // fifty small structs, and the bank can change under us on reload.
        struct Row { int preset; int cat; };            // preset < 0: a header
        std::vector<Row> rows;
        rows.reserve((size_t)np + 8);
        int prevCat = -1;
        for (int i = 0; i < np; ++i) {
            const int cat = presetCatOf(presetNameOf(*inst, i));
            if (cat >= 0 && cat != prevCat) rows.push_back({-1, cat});
            if (cat >= 0) prevCat = cat;
            rows.push_back({i, cat});
        }
        const int nr = (int)rows.size();
        const auto isSel = [&](int i) { return i >= 0 && i < nr && rows[i].preset >= 0; };

        // Geometry: hung under the preset row, right-aligned to it (the row
        // lives in the last column, so growing leftward is growing inward),
        // clipped to the card -- a menu taller than its card scrolls, it does
        // not escape the panel.
        const f32 rowHd = 15 * s;
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
                        const char* cl = kPresetCat[rows[i].cat].label;
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
//   NXTAKT_DEBUG_SPECTRAPAGE=<n>  start on page n (0 MAIN, 1 MOD) -- the tab
//                                 the mouse would click.
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
        g_page = clampv(atoi(p), 0, 1);
        LOGI("NXTAKT_DEBUG_SPECTRAPAGE: page %d", g_page);
    }
    if (const char* p = env("DEBUG_SPECTRADROP")) {
        if (*p && *p != '0') {
            g_pageUid = d.uid;
            g_drop.pending = true;
            LOGI("NXTAKT_DEBUG_SPECTRADROP: popover will open on first draw");
        }
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
