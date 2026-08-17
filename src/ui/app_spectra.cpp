// Spectra: the editor for the wavetable instrument.
//
// THE CONTRACT, and nothing but the contract. Every id, range, log flag, table
// name and sync division below is read off docs/SPECTRA-PARAMS.md; this file
// has never seen src/plugin/spectra.cpp and must never need to. That is not
// purity for its own sake -- the two halves were written at the same time by
// two people, and the frozen table is the only thing that could keep them
// agreeing without one waiting for the other.
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
// Light comes from the upper left in every one of it: the knob caps are
// --glass-1 circles with the 1px --edge stroke, the troughs are lit along the
// same 147deg axis, and the one specular in the panel is bound to the POSITION
// VALUE rather than to a clock (§1, "light rides motion"). Nothing in this
// panel animates on a timer, so there is nothing here for reduced motion to
// freeze -- and the two things that could (the trough sheen, the widgets' hover
// weights) already collapse on their own.
//
// ---------------------------------------------------------------------------
// WHY THE PANEL OPENS BESIDE THE DEVICE BOX
//
// The same reason the rack panel does: a device box is 150 logical pixels wide
// and this instrument has forty-two controls. The alternative is a floating
// editor window, which would be the first one in the program -- a whole second
// window system, its own focus rules and its own DPI story, for one device.
// Beside the box it is the same strip, the same scroll, the same hit testing.
//
// ---------------------------------------------------------------------------
// THE GUARD, and why every single lookup has one
//
// The panel draws against whatever PluginInstance it is pointed at. Normally
// that is a Spectra with forty-two parameters; under NXTAKT_DEBUG_SPECTRA it
// is deliberately whatever device happens to be first in the chain, and during
// the days before the DSP landed it was ALWAYS something else. So a parameter
// is a *slot* that may or may not be filled: `has(id)` is `paramCount() > id`,
// an unfilled slot draws as an empty socket at 40% with a dash for a value, and
// it claims no hot rectangle at all -- so the pointer passes straight through
// it and the cursor never changes over a control that does not exist. A panel
// that crashed, or drew a knob wired to nothing, would be the worse of the two
// failures by a distance: the second one lies.
//
#include "app.h"
#include "app_internal.h"
#include "../gfx/gl.h"
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
static_assert(pCount == 42, "docs/SPECTRA-PARAMS.md is frozen at 42 parameters");

// The eight tables, by index. UI labels, per the contract's own wording.
const char* const kTables[8] = {"Basic", "PWM", "Harmonic", "Formant",
                                "Bell",  "Digital", "Vox",  "Fold"};
const char* const kFilterType[3] = {"LP", "BP", "HP"};
// LFO Sync, 0..9. 0 is free-run and the rate knob owns the readout then; every
// other entry is a division and the knob shows it instead of a frequency.
const char* const kSyncDiv[10] = {"free", "4 bars", "2 bars", "1 bar", "1/2",
                                  "1/4",  "1/8",    "1/16",   "1/4T",  "1/8T"};
const char* const kShapeName[5] = {"sine", "tri", "saw", "square", "S&H"};

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
// THE DISPLAYED WAVEFORM IS AN ILLUSTRATION, NOT A TAP
//
// Say it plainly, because a picture of a waveform in a synth implies it came
// from the synth. It did not. The real tables are 32 frames x 2048 samples
// generated inside the plugin at prepare(), they live on the far side of the
// PluginInstance boundary, and that boundary has exactly three ways through it
// -- paramCount, paramInfo, getParam. There is no sample port, and inventing
// one for a picture would be a realtime obligation taken on for decoration.
//
// So what is drawn is the SAME FAMILY of shapes, generated here from the same
// two numbers the DSP morphs with: the table index and the Position value. It
// tells the truth about what Position does -- which shape you are between, and
// which way it is heading -- and it is honestly wrong about the exact spectrum,
// because the plugin's tables are band-limited per octave and these are not.
//
// If the plugin ever grows a way to publish its current frame, this function is
// the one thing to delete.
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

// One frame, sampled and normalised to fit the well. Normalising is honest
// here: what the display is about is SHAPE, and an un-normalised PWM frame at
// 0.95 is a hairline against the top of the box.
void buildFrame(std::vector<f32>& out, int n, int table, f32 pos) {
    out.resize((size_t)n);
    f32 peak = 1e-6f;
    for (int i = 0; i < n; ++i) {
        const f32 v = specimenSample(table, pos, (f32)i / (f32)n);
        out[(size_t)i] = v;
        peak = std::max(peak, std::fabs(v));
    }
    const f32 k = 1.f / peak;
    for (f32& v : out) v *= k;
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
// ---------------------------------------------------------------------------

void App::drawSpectraPanel(const Rect& box, DeviceModel& dm, const Col& tc) {
    PluginInstance* inst = dm.inst.get();
    if (!inst) return;
    const f32 s = win_.dpiScale();
    const int pc = inst->paramCount();
    const bool real = isSpectra(inst);

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
    if (ui_.button(uiId(40, 0, 0), closeR, "")) {
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
    if (!real) {
        // §9: say what happened. The panel is open on something that is not a
        // Spectra, which is a state only the debug hook can reach -- and amber
        // means attention, which is exactly what it is.
        char note[128];
        snprintf(note, sizeof note, "panel forced onto %s - %d of 42 parameters",
                 dm.desc.name.c_str(), pc);
        microFit(ui_, fSmall_, {title.x + 78 * s, title.y, closeR.x - title.x - 84 * s,
                                title.h}, note, nx::amber.alpha(0.9f), Align::Left, 0);
    }
    rend_.hairlineH(title.x + nx::sp1 * s, title.right() - nx::sp1 * s, title.bottom());

    // --- the column grid ---------------------------------------------------
    //
    // Everything below is on the 8px grid: the column widths, the gaps, the row
    // heights and the pads are all multiples of 4 at half-step and 8 otherwise.
    //
    // A DOCK 200 LOGICAL PIXELS TALL is the constraint the whole layout is cut
    // to, and it is not negotiable from here: detailH_ has no splitter, so the
    // panel gets a 155px box and forty-two controls to put in it. That is why
    // this is a wide band of seven columns rather than the square face a
    // standalone synth wears -- every section is one column, three of them are
    // header / selector / two knob rows, and the whole thing lands inside the
    // strip's width with the file browser closed (Ctrl+B). Wider than the strip
    // it simply scrolls, like every other device box beside it.
    Rect body{box.x + 6 * s, title.bottom() + 3 * s, box.w - 12 * s,
              box.bottom() - title.bottom() - 9 * s};
    if (body.w < 48 * s || body.h < 48 * s) return;

    rend_.pushClip(box);

    const f32 headH = 11 * s;                  // the uppercase micro-label
    const f32 subH  = 14 * s;                  // a selector / cluster row
    const f32 gap   = 4 * s;
    f32 rowH = (body.h - headH - subH - gap * 3.f) * 0.5f;
    rowH = clampv(rowH, 34 * s, 62 * s);
    const f32 lblH = 11 * s;                   // the knob's own name

    // Seven sections, seven columns. The sum plus six 8px gaps plus the two 6px
    // pads is kSpectraPanelW in app_devices.cpp -- if one of these changes, so
    // does that, and the panel would otherwise draw past its own card.
    static const f32 kColW[7] = {144, 138, 138, 138, 204, 138, 152};
    const f32 colGap = 8 * s;
    f32 colX[7];
    {
        f32 x = body.x;
        for (int i = 0; i < 7; ++i) { colX[i] = x; x += kColW[i] * s + colGap; }
    }
    const auto col = [&](int i) { return Rect{colX[i], body.y, kColW[i] * s, body.h}; };
    // Seams. §11: no solid dividers anywhere -- a hairline that fades at both
    // ends is the only legal one in the system.
    for (int i = 1; i < 7; ++i)
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
    const int uidKey = (int)(dm.uid & 0xffffu);

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
        const u64 wid = uiId(41, id, uidKey);
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
    // eight tables and ten divisions are rings rather than ranges.
    const auto stepper = [&](const Rect& r0, int id, int count,
                             const char* const* names, f32 dim, const char* absentText) {
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
            const u64 wid = uiId(40, 30 + id, uidKey);
            commit(id, (f32)n, wid, names == kTables ? "Table" : "Sync");
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
        if (ui_.segButton(uiId(40, 31 + id, uidKey), lb, false, nx::violet)) step(-1);
        chev(lb, true);
        if (ui_.segButton(uiId(40, 32 + id, uidKey), rb, false, nx::violet)) step(+1);
        chev(rb, false);
        const Rect nameR{lb.right(), r0.y, rb.x - lb.right(), r0.h};
        microFit(ui_, fSmall_, nameR, live ? names[idx] : absentText,
                 (live ? nx::text : nx::muted.alpha(0.45f)).alpha(dim), Align::Center);
        return idx;
    };

    // A section's uppercase micro-label. §5/§7: 10px, wide tracking, muted.
    const auto sect = [&](const Rect& c, const char* label, f32 dim) {
        ui_.microIn(fSmall_, {c.x, c.y, c.w, headH}, label,
                    nx::muted.alpha(dim), Align::Left, 0);
        return c.y + headH;
    };

    // --- what osc B is doing, which several sections need ------------------
    const f32 bLevel = get(pBLevel, 0.f);
    // §5's disabled rule: an osc contributing nothing is not doing anything,
    // and it says so at 40% rather than being greyed or hidden.
    const f32 dimB = (has(pBLevel) && bLevel > 1e-4f) ? 1.f : 0.4f;

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
            const int n = clampv((int)(plot.w / std::max(1.f, 1.4f * s)), 24, 512);
            // B first, so A's trace reads on top of it: A is the one that is
            // always sounding.
            const auto trace = [&](int table, f32 pos, const Col& c0, f32 th) {
                buildFrame(spectraWave_, n, table, pos);
                f32 px = plot.x, py = plot.cy() - spectraWave_[0] * plot.h * 0.46f;
                for (int i = 1; i < n; ++i) {
                    const f32 qx = plot.x + plot.w * ((f32)i / (f32)(n - 1));
                    const f32 qy = plot.cy() - spectraWave_[(size_t)i] * plot.h * 0.46f;
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
            // And the honesty label. It is an illustration of Position, not a
            // tap of the audio path, and the panel says so where it is drawn.
            ui_.microIn(fSmall_, {dispR.x, dispR.bottom() - 11 * s, dispR.w - 6 * s, 10 * s},
                        "illustration", nx::muted.alpha(0.35f), Align::Right, 0);
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
            const u64 wid = uiId(42, id, uidKey);
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
        stepper({c.x, y0, c.w, subH}, base + 0, 8, kTables, dim, "no table");

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

        // The three types are ONE cluster, not three capsules: exactly one is
        // chosen, and a chooser drawn as separate buttons is the "buttons that
        // don't belong together" look (app_devices.cpp's segmented rule).
        const Rect ftR{c.x, y0, c.w, subH};
        ui_.segCluster(ftR);
        const f32 segW = ftR.w / 3.f;
        const int ftype = has(pFType) ? clampv((int)std::lround(get(pFType, 0.f)), 0, 2) : -1;
        for (int k = 0; k < 3; ++k) {
            const Rect seg{ftR.x + segW * (f32)k, ftR.y, segW, ftR.h};
            if (k) rend_.hairlineV(seg.x, ftR.y + 2 * s, ftR.bottom() - 2 * s);
            const bool on = k == ftype;
            const u64 wid = uiId(40, 40 + k, uidKey);
            if (ui_.segButton(wid, seg, on, nx::violet) && has(pFType))
                commit(pFType, (f32)k, wid, "Filter Type");
            ui_.microIn(fSmall_, ui_.lastRect, kFilterType[k],
                        on ? nx::text : nx::muted.alpha(ftype < 0 ? 0.45f : 1.f),
                        Align::Center);
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
        const f32 curveW = 48 * s;
        // This column has no selector row, so its two knob rows sit where every
        // other column's do -- the band the others use for a stepper is left
        // empty rather than stealing it, because rows that line up across seven
        // columns are most of what makes a panel read as built.
        const f32 er1 = y0 + subH + gap, er2 = er1 + rowH + gap;

        const auto envRow = [&](f32 y, const char* name, int base) {
            // The curve, drawn from the four values that are actually there --
            // a missing one falls back to its contract default, and the well
            // says so by staying at the disabled weight.
            const Rect cw0{c.x, y, curveW, rowH - lblH};
            rend_.well(cw0, nx::radiusXs * s, true);
            const bool any = has(base) || has(base + 1) || has(base + 2) || has(base + 3);
            const f32 atk = get(base + 0, 5.f), dec = get(base + 1, 200.f);
            const f32 sus = clampv(get(base + 2, 0.7f), 0.f, 1.f);
            const f32 rel = get(base + 3, 300.f);
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
            st.lo = 0.1f; st.hi = 5000.f; st.def = 5.f;
            knob({c.x + curveW, y, kw, rowH}, base + 0, "att", st, 1.f);
            st.lo = 1.f; st.hi = 5000.f; st.def = 200.f;
            knob({c.x + curveW + kw, y, kw, rowH}, base + 1, "dec", st, 1.f);
            st.log = false; st.lo = 0.f; st.hi = 1.f; st.def = 0.7f; st.fmt = "%.2f";
            knob({c.x + curveW + kw * 2.f, y, kw, rowH}, base + 2, "sus", st, 1.f);
            st.log = true; st.lo = 1.f; st.hi = 8000.f; st.def = 300.f; st.fmt = "%.0f";
            knob({c.x + curveW + kw * 3.f, y, kw, rowH}, base + 3, "rel", st, 1.f);
        };
        envRow(er1, "ENV 1", pAttack);
        rend_.hairlineH(c.x, c.right(), er2 - 2 * s);
        envRow(er2, "ENV 2", pE2Attack);
    }

    // =======================================================================
    // 5. LFO
    // =======================================================================
    {
        const Rect c = col(5);
        const f32 y0 = sect(c, "LFO", 1.f);

        // Five shapes as one cluster of drawn icons. Lettered names would not
        // fit at 27px a segment and would say less than the shape does.
        const Rect shR{c.x, y0, c.w, subH};
        ui_.segCluster(shR);
        const f32 sw = shR.w / 5.f;
        const int shape = has(pLfoShape)
                        ? clampv((int)std::lround(get(pLfoShape, 0.f)), 0, 4) : -1;
        for (int k = 0; k < 5; ++k) {
            const Rect seg{shR.x + sw * (f32)k, shR.y, sw, shR.h};
            if (k) rend_.hairlineV(seg.x, shR.y + 2 * s, shR.bottom() - 2 * s);
            const bool on = k == shape;
            const u64 wid = uiId(40, 50 + k, uidKey);
            if (ui_.segButton(wid, seg, on, nx::violet) && has(pLfoShape))
                commit(pLfoShape, (f32)k, wid, "LFO Shape");
            const Rect g = ui_.lastRect;
            const Col ic = on ? nx::text : nx::muted.alpha(shape < 0 ? 0.4f : 0.85f);
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
            ui_.tip = has(pLfoShape) ? std::string("LFO shape: ") +
                                       (shape >= 0 ? kShapeName[shape] : "?")
                                     : std::string("This device has no LFO shape");

        const f32 cw = c.w / 3.f;
        const f32 r1 = y0 + subH + gap, r2 = r1 + rowH + gap;

        // The sync selector takes two cells of the second row and is a
        // 14px control inside a 48px band, so it is centred in it rather than
        // hung off the top -- the row is what the eye aligns on.
        const Rect syncR{c.x, r2 + (rowH - lblH - subH) * 0.5f, cw * 2.f, subH};
        const int sync = stepper(syncR, pLfoSync, 10, kSyncDiv, 1.f, "no sync");
        microFit(ui_, fSmall_, {c.x, r2 + rowH - lblH, cw * 2.f, lblH}, "sync",
                 nx::muted.alpha(0.85f), Align::Center);
        if (ui_.hovered(syncR))
            ui_.tip = "LFO sync: free-running, or a division of the transport's tempo";

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
        const int np = inst->presetCount();
        const f32 y0 = sect(c, np > 0 ? "GLOBAL / PRESET" : "GLOBAL", 1.f);
        if (np > 0) {
            spectraPreset_ = clampv(spectraPreset_, 0, np - 1);
            const Rect pr{c.x, y0, c.w, subH};
            ui_.segCluster(pr);
            const f32 bw = 16 * s;
            const Rect lb{pr.x, pr.y, bw, pr.h}, rb{pr.right() - bw, pr.y, bw, pr.h};
            rend_.hairlineV(lb.right(), pr.y + 2 * s, pr.bottom() - 2 * s);
            rend_.hairlineV(rb.x, pr.y + 2 * s, pr.bottom() - 2 * s);
            const auto load = [&](int d) {
                const int n = ((spectraPreset_ + d) % np + np) % np;
                // A preset rewrites every parameter at once, so it is one undo
                // entry like any other edit -- and a one-shot, so it takes a
                // point every time rather than coalescing.
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
            if (ui_.segButton(uiId(40, 60, uidKey), lb, false, nx::violet)) load(-1);
            chev(lb, true);
            if (ui_.segButton(uiId(40, 61, uidKey), rb, false, nx::violet)) load(+1);
            chev(rb, false);
            microFit(ui_, fSmall_, {lb.right(), pr.y, rb.x - lb.right(), pr.h},
                     presetNameOf(*inst, spectraPreset_), nx::text, Align::Center);
            if (ui_.hovered(pr)) {
                char t[96];
                snprintf(t, sizeof t, "Preset %d of %d - the arrows step through them",
                         spectraPreset_ + 1, np);
                ui_.tip = t;
            }
        }

        const f32 cw = c.w / 3.f;
        const f32 r1 = y0 + subH + gap, r2 = r1 + rowH + gap;
        Ui::KnobStyle st;
        st.lo = 0.f; st.hi = 500.f; st.def = 0.f; st.fmt = "%.0f";
        knob({c.x, r1, cw, rowH}, pGlide, "glide", st, 1.f);
        st.lo = 1.f; st.hi = 16.f; st.def = 8.f;
        knob({c.x + cw, r1, cw, rowH}, pVoices, "voices", st, 1.f);
        st.lo = 0.f; st.hi = 1.5f; st.def = 1.f; st.fmt = "%.2f";
        knob({c.x + cw * 2.f, r1, cw, rowH}, pMaster, "master", st, 1.f);
        // Noise and Sub are the two sources that are neither oscillator, so
        // they sit with the global mix rather than getting a section of their
        // own for a pair -- and the contract puts them between the oscillators
        // and the filter for the same reason.
        st.lo = 0.f; st.hi = 1.f; st.def = 0.f;
        knob({c.x + cw * 0.5f, r2, cw, rowH}, pNoise, "noise", st, 1.f);
        knob({c.x + cw * 1.5f, r2, cw, rowH}, pSub, "sub", st, 1.f);
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
    // control off getParam each frame, so a load that rewrites all forty-two
    // parameters needs nothing refreshed -- which is the immediate-mode part of
    // this design paying for itself, and the property this hook is here to
    // show rather than assert.
    if (const char* p = env("DEBUG_SPECTRAPRESET")) {
        const int np = d.inst->presetCount();
        int want = -1;
        if (np <= 0) {
            LOGW("NXTAKT_DEBUG_SPECTRAPRESET: %s declares no presets", d.desc.name.c_str());
        } else if (*p >= '0' && *p <= '9') {
            want = clampv(atoi(p), 0, np - 1);
        } else {
            for (int i = 0; i < np && want < 0; ++i)
                if (icontains(presetNameOf(*d.inst, i), p)) want = i;
            if (want < 0) LOGW("NXTAKT_DEBUG_SPECTRAPRESET: no preset matching \"%s\"", p);
        }
        if (want >= 0) {
            undoPoint("load preset");
            spectraPreset_ = want;
            d.inst->loadPreset(want);
            LOGI("NXTAKT_DEBUG_SPECTRAPRESET: %d/%d \"%s\" loaded", want + 1, np,
                 presetNameOf(*d.inst, want));
            status_ = std::string("Spectra: ") + presetNameOf(*d.inst, want);
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
