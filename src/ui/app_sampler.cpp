// The Sampler's editor: the waveform, with the playback region living on it.
//
// THE CONTRACT, and nothing but the contract. Every id, range, unit and default
// below is read off the frozen twenty-parameter table at the top of
// src/plugin/sampler.cpp; this file has never compiled against that one and
// must never need to. Two halves, one table between them -- app_spectra.cpp's
// arrangement, for the same reason: neither half has to wait for the other.
//
// ---------------------------------------------------------------------------
// THE JUDGMENT, for this file (docs/DESIGN.md §4)
//
// A plugin editor is chrome, so it takes the tier language in full. The panel
// is ONE card -- the lit-violet edge and an elevation, exactly like the rack
// panel and Spectra's, because it is the inside of the device box beside it.
// Everything *inside* it recesses: the waveform is a well, every knob trough is
// a well, and there is no second frosted layer anywhere (§4: glass inside glass
// reads as fog). Sections are separated by gradient hairlines that fade at both
// ends; there is not a solid rule in here.
//
// THE HERO IS THE WAVEFORM, and the region is what makes it a hero rather than
// a picture. Start and End are two of twenty knobs on the generic strip, where
// they are two numbers between 0 and 1 with nothing to be 0 and 1 *of*. Drawn
// on the file they are edges you take hold of, and the thing they cut is
// visible while you cut it. So: the body of the file is muted, the part between
// Start and End is CYAN -- §1's rule taken literally, the region IS the light
// inside the material -- and the two edges are draggable.
//
// The one specular in the panel is bound to the REGION CENTRE rather than to a
// clock (§1, "light rides motion"). Nothing in this panel animates on a timer,
// so there is nothing here for reduced motion to freeze.
//
// ---------------------------------------------------------------------------
// WHERE THE PICTURE COMES FROM
//
// SamplerControl::sampleBuffer(), which is the buffer the device is ACTUALLY
// PLAYING and not a copy of it -- so the peaks under the region handles are the
// peaks of the audio those handles are cutting, by construction rather than by
// coincidence. It hands back a shared_ptr rather than a raw pointer for the
// reason host.h gives, and this file holds that reference for exactly as long
// as it draws with it (samplerWave_, reassigned every frame and dropped when
// the panel closes).
//
// The alternative, and what this file did for about an hour: re-decode the path
// from samplePath() into a second buffer of its own. That is a second copy of
// the user's sample in memory, a decode inside a draw call, nothing at all to
// draw for a take that was adopt()ed with no path behind it, and two buffers
// that could disagree. The accessor is worth having and it landed.
//
// ---------------------------------------------------------------------------
// THE GUARD, and why every lookup has one
//
// The panel draws against whatever PluginInstance it is pointed at. Normally
// that is a Sampler with twenty parameters; under NXTAKT_DEBUG_SAMPLER it may
// be whatever device happens to be first in the chain. So a parameter is a
// *slot* that may or may not be filled: `has(id)` is `paramCount() > id`, an
// unfilled slot draws as an empty socket at 40% with a dash for a value, and it
// claims no hot rectangle at all. A device with no sampler() at all gets the
// hero's "no sample player here" line and a panel of empty sockets -- legible,
// and honest about what it is looking at.
//
#include "app.h"
#include "app_internal.h"
#include "../audio/sample.h"
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
//
//   id  name          unit  range              default
//    0  Root Note     -     0 .. 127 (int)     60       plays at unity here
//    1  Coarse        st    -24 .. 24 (int)    0
//    2  Fine          ct    -100 .. 100        0
//    3  Start         -     0 .. 1             0        fraction of the file
//    4  End           -     0 .. 1             1        fraction of the file
//    5  Loop          -     bool               off      loops Start..End
//    6  Crossfade     ms    0 .. 50            5        loop crossfade
//    7  Gate          -     bool               on       off = one-shot
//    8  Attack        ms    0.1 .. 5000 (log)  0.5
//    9  Decay         ms    1 .. 5000 (log)    1000
//   10  Sustain       -     0 .. 1             1
//   11  Release       ms    1 .. 8000 (log)    40
//   12  Cutoff        Hz    20 .. 20000 (log)  20000
//   13  Resonance     -     0 .. 1             0.1
//   14  Env>Cutoff    -     -1 .. 1            0
//   15  Keytrack      -     0 .. 1             0
//   16  Vel>Amp       -     0 .. 1             1
//   17  Glide         ms    0 .. 500           0
//   18  Voices        -     1 .. 16 (int)      16
//   19  Master        -     0 .. 2             1        1 is unity
// ---------------------------------------------------------------------------
enum SmpParam : int {
    pRoot = 0, pCoarse, pFine,
    pStart = 3, pEnd, pLoop, pXfade,
    pGate = 7,
    pAttack = 8, pDecay, pSustain, pRelease,
    pCutoff = 12, pReso, pEnvCut, pKeytrack,
    pVel = 16, pGlide, pVoices, pMaster,
    pCount
};
static_assert(pCount == 20, "the sampler's parameter table is frozen at 20");

// The smallest region the panel will let the two handles make. The device
// refuses to sound a region under four frames (sampler.cpp's `endF - startF <
// 4.0`), so a UI that allowed Start past End would be offering a gesture whose
// only outcome is silence. Four frames of a short file is a hair under a
// thousandth, and this is that with a floor for the pathological case.
constexpr f32 kMinRegion = 0.001f;

// PRESETS. host.h's contract says presetName() may return null out of range,
// and "out of range" includes every index on a device with no presets. So the
// null is handled here, once.
const char* presetNameOf(const PluginInstance& p, int i) {
    const char* n = p.presetName(i);
    return n ? n : "?";
}

// A micro-label cut to fit its box -- app_devices.cpp's and app_spectra.cpp's
// helper, and for the same reason: microIn draws glyph by glyph to get §5's
// tracking, so it has no ellipsis logic, and a scissor around every label would
// cost two draw calls apiece.
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

// MIDI note -> name, in the piano roll's spelling and octave numbering
// (pianoroll.cpp: sharps, and 60 is C4). The root note knob reads out as a NOTE
// rather than as the number 60, because "the pitch this file plays back at
// unity" is a note and nobody thinks in program numbers.
void noteName(char* out, size_t n, int midi) {
    midi = clampv(midi, 0, 127);
    snprintf(out, n, "%s%d", kPitchNames[midi % 12], midi / 12 - 1);
}

// Seconds, at whichever precision reads: a 40 ms slice and a four minute file
// are the same field.
void fmtSecs(char* out, size_t n, f64 sec) {
    if (sec < 0.0) sec = 0.0;
    if (sec < 1.0)       snprintf(out, n, "%.0f ms", sec * 1000.0);
    else if (sec < 60.0) snprintf(out, n, "%.2f s", sec);
    else                 snprintf(out, n, "%d:%04.1f", (int)(sec / 60.0),
                                  sec - 60.0 * std::floor(sec / 60.0));
}

} // namespace

// ---------------------------------------------------------------------------
// Identity and panel resolution
// ---------------------------------------------------------------------------

// "This device plays a file." Only the sampler answers sampler() non-null, so
// this is the whole test -- and it is a virtual call rather than a URI compare
// precisely so a future device that plays a file could answer it too. It is the
// same shape the rack's "this device has an inside" test already has.
bool App::isSampler(PluginInstance* p) {
    return p && p->sampler() != nullptr;
}

// Resolved from the uid every frame, and a uid rather than a chain index for
// the reason rackOpenUid_ is one: a chain edit must not slide an open panel
// onto whatever device inherited the slot.
int App::samplerOpenIdx(const std::vector<DeviceModel>& devices) const {
    if (!samplerOpenUid_) return -1;
    for (size_t i = 0; i < devices.size(); ++i) {
        if (devices[i].uid != samplerOpenUid_) continue;
        if (!devices[i].inst) return -1;
        // samplerForced_ is the debug hook standing in for a mouse: it opens
        // the panel on a device that is not a sampler, which is how the guarded
        // state gets a screenshot of its own.
        return (samplerForced_ || isSampler(devices[i].inst.get())) ? (int)i : -1;
    }
    return -1;                                   // the device went away
}

// ---------------------------------------------------------------------------
// The picture's buffer: the device's own, borrowed for the frame that draws it.
//
// The reference is taken every frame rather than cached across frames, and
// deliberately: a load, a preset or an undo can re-point the sampler between
// two frames, and a panel holding last frame's buffer would draw the previous
// file under this file's region handles. Two atomic increments a frame is the
// whole cost of never being able to.
// ---------------------------------------------------------------------------
const SampleBuffer* App::samplerBuffer(SamplerControl* sc) {
    samplerWave_ = sc ? sc->sampleBuffer() : SampleRef{};
    return samplerWave_.get();
}

// ---------------------------------------------------------------------------
// The panel
// ---------------------------------------------------------------------------

void App::drawSamplerPanel(const Rect& box, DeviceModel& dm, const Col& tc) {
    PluginInstance* inst = dm.inst.get();
    if (!inst) return;
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    const int pc = inst->paramCount();
    SamplerControl* smp = inst->sampler();
    const bool real = smp != nullptr;

    // --- the card ----------------------------------------------------------
    const f32 rad = nx::radiusSm * s;
    rend_.shadow(box, rad, nx::shadow);
    rend_.gradRect(box, rad, nx::glass1);

    // THE WHOLE PANEL IS THE DROP TARGET, and it is the card's own glue lifted
    // wholesale (app_devices.cpp, the sampler card): the lit edge arriving
    // early, the Add badge, the same sentence, the same loadFile call, the same
    // undo point and the same two status lines. Two surfaces that accept the
    // same gesture have to answer it the same way, or the second one teaches
    // the user that the first one was a special case.
    // NXTAKT_DEBUG_SAMPLERDRAG, re-armed here rather than once in the seed:
    // app_session.cpp's drag ghost clears any drag the mouse is not holding
    // ("if (!in.down[0]) drag_ = DragState{}"), which is exactly right for a
    // real drag and leaves a headless one alive for a single frame. Held here,
    // the state the panel reads is the state a held mouse would produce.
    if (!samplerDragHold_.empty()) {
        drag_.kind  = DragState::Kind::BrowserFile;
        drag_.path  = samplerDragHold_;
        drag_.armed = true;
    }
    const bool fileDrag = drag_.kind == DragState::Kind::BrowserFile && drag_.armed;
    const bool fileDragHere = real && fileDrag && box.contains(in.mx, in.my);
    if (fileDragHere) {
        rend_.gradStroke(box, rad, s, nx::edgeLit, 1.f);
        ui_.badge = Badge::Add;
        ui_.tip = "Drop to load into the sampler";
        if (in.released[0]) {
            undoPoint("load sample");
            if (smp->loadFile(drag_.path)) {
                status_ = "Loaded " + drag_.path.substr(drag_.path.rfind('/') + 1);
            } else {
                status_ = "Could not load " + drag_.path + " - the sampler is unchanged";
            }
            drag_ = DragState{};
        }
    } else {
        rend_.gradStroke(box, rad, s, nx::edgeLit, 1.f);
    }
    rend_.roundRectOutline(box, rad, std::max(1.f, nx::snapPx(s)), nx::violet.alpha(0.55f));

    // --- title -------------------------------------------------------------
    Rect title{box.x, box.y, box.w, 16 * s};
    rend_.rect({title.x + 3 * s, title.y + 4 * s, std::max(1.f, nx::snapPx(3 * s)),
                title.h - 8 * s}, tc);

    Rect closeR{title.right() - 17 * s, title.y + 2 * s, 14 * s, 12 * s};
    if (ui_.button(uiId(UiSamplerPanel, 0, 0), closeR, "")) {
        samplerOpenUid_ = 0;
        samplerForced_ = false;
        samplerWave_.reset();                    // stop holding the device's buffer
    }
    {
        const f32 k = 3.f * s;
        rend_.line(closeR.cx() - k, closeR.cy() - k, closeR.cx() + k, closeR.cy() + k,
                   1.2f * s, nx::muted);
        rend_.line(closeR.cx() - k, closeR.cy() + k, closeR.cx() + k, closeR.cy() - k,
                   1.2f * s, nx::muted);
    }
    if (ui_.hovered(closeR)) ui_.tip = "Close the Sampler panel";

    ui_.microIn(fSmall_, {title.x + 10 * s, title.y, 120 * s, title.h}, "SAMPLER",
                nx::violetSoft, Align::Left, 0);
    if (!real) {
        // §9: say what happened. The panel is open on something with no sample
        // player, which is a state only the debug hook can reach -- and amber
        // means attention, which is exactly what it is.
        char note[128];
        snprintf(note, sizeof note, "panel forced onto %s - %d of 20 parameters",
                 dm.desc.name.c_str(), pc);
        microFit(ui_, fSmall_, {title.x + 82 * s, title.y, closeR.x - title.x - 88 * s,
                                title.h}, note, nx::amber.alpha(0.9f), Align::Left, 0);
    }
    rend_.hairlineH(title.x + nx::sp1 * s, title.right() - nx::sp1 * s, title.bottom());

    // --- the column grid ---------------------------------------------------
    //
    // Everything below is on the 8px grid: the column widths, the gaps, the row
    // heights and the pads are all multiples of 4 at half-step and 8 otherwise.
    //
    // A DOCK 200 LOGICAL PIXELS TALL is the constraint, exactly as it is for
    // Spectra: detailH_ has no splitter, so the panel gets a 155px box. The
    // hero takes a third of the width and the twenty controls take the rest, in
    // five sections whose rows line up across all of them.
    Rect body{box.x + lay::samplerPad * s, title.bottom() + 3 * s,
              box.w - lay::samplerPad * 2.f * s,
              box.bottom() - title.bottom() - 9 * s};
    if (body.w < 48 * s || body.h < 48 * s) return;

    rend_.pushClip(box);

    const f32 headH = 11 * s;                  // the uppercase micro-label
    const f32 subH  = 14 * s;                  // a selector / cluster row
    const f32 gap   = 4 * s;
    f32 rowH = (body.h - headH - subH - gap * 3.f) * 0.5f;
    rowH = clampv(rowH, 34 * s, 62 * s);
    const f32 lblH = 11 * s;                   // the knob's own name

    // Six sections, six columns — lay::samplerColW, which is also what the
    // device strip reserves the panel's width from.
    constexpr int   kCols  = lay::samplerCols;
    const     f32*  kColW  = lay::samplerColW;
    const     f32   colGap = lay::samplerColGap * s;
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

    // --- the parameter access layer ----------------------------------------
    //
    // Three closures, and every control in the panel goes through them --
    // including the two the waveform's handles drive. This is where the guard
    // lives, once, and it is also what makes a handle drag and a knob drag the
    // same edit: one undo point per gesture, one automation capture, no second
    // path into setParam anywhere in this file.
    const auto has = [&](int id) { return id >= 0 && id < pc; };
    const auto get = [&](int id, f32 fallback) {
        return has(id) ? inst->getParam(id) : fallback;
    };
    const bool ownTrack = ownIsTrack(devOwner_);
    const auto commit = [&](int id, f32 v, u64 gesture, const char* what) {
        if (!has(id)) return;
        // The undo entry is taken BEFORE the model changes, and coalesced on
        // the widget's id -- so one knob drag, and one handle drag, is one
        // entry. The capture is unconditional and only for a track chain, for
        // the reason app_devices.cpp gives: a return's or the master's knob has
        // no clip envelope to record into.
        undoPoint(what, gesture);
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
        // sites -- but "st", "ct", "Hz", "ms" are the plugin's own metadata, and
        // a panel that hard-coded them would be a second place for them to be
        // wrong.
        char fmtbuf[32];
        if (!st.absent && st.fmt && !st.text) {
            const std::string& u = inst->paramInfo(id).unit;
            if (!u.empty() && u.size() < 5 && u.find('%') == std::string::npos) {
                snprintf(fmtbuf, sizeof fmtbuf, "%s%s", st.fmt, u.c_str());
                st.fmt = fmtbuf;
            }
        }
        f32 v = has(id) ? inst->getParam(id) : st.def;
        const u64 wid = uiId(UiSamplerKnob, id, uidKey);
        const Rect kr{cell.x, cell.y, cell.w, cell.h - lblH};
        if (ui_.knobNx(wid, kr, &v, st)) commit(id, v, wid, label);
        microFit(ui_, fSmall_, {cell.x, cell.bottom() - lblH, cell.w, lblH}, label,
                 nx::muted.alpha((st.absent ? 0.40f : 0.85f) * dim), Align::Center);
        if (ui_.hovered(kr)) {
            char t[144];
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

    // A two-segment cluster over a bool parameter. Exactly one is chosen, so it
    // is ONE cluster and not two capsules (app_devices.cpp's segmented rule).
    const auto boolCluster = [&](const Rect& r0, int id, const char* offName,
                                 const char* onName, f32 dim, const char* what) {
        ui_.segCluster(r0);
        const f32 segW = r0.w * 0.5f;
        const int cur = has(id) ? (get(id, 0.f) >= 0.5f ? 1 : 0) : -1;
        for (int k = 0; k < 2; ++k) {
            const Rect seg{r0.x + segW * (f32)k, r0.y, segW, r0.h};
            if (k) rend_.hairlineV(seg.x, r0.y + 2 * s, r0.bottom() - 2 * s);
            const bool on = k == cur;
            const u64 wid = uiId(UiSamplerPanel, 40 + id * 2 + k, uidKey);
            if (ui_.segButton(wid, seg, on, nx::violet) && has(id))
                commit(id, (f32)k, wid, what);
            microFit(ui_, fSmall_, ui_.lastRect, k ? onName : offName,
                     (on ? nx::text : nx::muted.alpha(cur < 0 ? 0.45f : 1.f)).alpha(dim),
                     Align::Center, 2 * s);
        }
        return cur;
    };

    // A section's uppercase micro-label. §5/§7: 10px, wide tracking, muted.
    const auto sect = [&](const Rect& c, const char* label, f32 dim) {
        ui_.microIn(fSmall_, {c.x, c.y, c.w, headH}, label,
                    nx::muted.alpha(dim), Align::Left, 0);
        return c.y + headH;
    };

    // --- what the mode is doing, which the hero and two columns need --------
    const bool gateOn = has(pGate) ? get(pGate, 1.f) >= 0.5f : true;
    const bool loopSet = has(pLoop) && get(pLoop, 0.f) >= 0.5f;
    // ONE-SHOT IGNORES THE LOOP. sampler.cpp: `v.looping = v.gate && b.loop`,
    // and the reason is in its header -- a one-shot never sees a note-off, so a
    // looping one would sound until the transport stopped. The panel must not
    // pretend otherwise: `loopLive` is what the DEVICE will do, and it is what
    // the waveform shades. `loopSet` is only what the switch says.
    const bool loopLive = loopSet && gateOn;

    // =======================================================================
    // 1. THE WAVEFORM -- the hero
    // =======================================================================
    {
        const Rect c = col(0);
        const f32 y0 = sect(c, "SAMPLE", 1.f);
        const f32 readH = 13 * s;
        const Rect well{c.x, y0 + 2 * s, c.w,
                        std::max(24 * s, c.bottom() - (y0 + 2 * s) - (readH + 4 * s))};

        const SampleBuffer* sb = samplerBuffer(smp);
        const i64 frames = smp ? smp->sampleFrames() : 0;
        const f64 rate = sb && sb->rate > 0.0 ? sb->rate : eng_.sampleRate();
        const f64 durSec = (rate > 0.0 && frames > 0) ? (f64)frames / rate : 0.0;

        f32 st0 = clampv(get(pStart, 0.f), 0.f, 1.f);
        f32 en0 = clampv(get(pEnd, 1.f), 0.f, 1.f);
        if (en0 < st0) en0 = st0;                 // a set could carry an inverted pair

        rend_.well(well, nx::radiusSm * s, true);
        // §1's light-rides-motion rule, and the one specular in the panel: the
        // band's phase IS the region's centre, so it slides across the glass as
        // the region moves and stands still when the region does. Nothing here
        // is triggered and nothing is on a clock.
        rend_.sheen(well, nx::radiusSm * s, (st0 + en0) * 0.5f, 0.26f);

        const Rect plot = well.insetXY(6 * s, 5 * s);
        const auto xOf = [&](f32 t) { return plot.x + plot.w * clampv(t, 0.f, 1.f); };

        if (!real) {
            rend_.textIn(fSmall_, well, "This device has no sample player",
                         nx::muted.alpha(0.7f), Align::Center);
        } else if (!smp->hasSample()) {
            // THE EMPTY STATE, and it is the card's chip word for word and ink
            // for ink (app_devices.cpp). "no sample" is a fact, not an alert,
            // and the invitation appears only while a browser drag is actually
            // in flight -- where it is the answer to a question being asked.
            // The first cut of the card's chip shouted in caps and the owner was
            // right about it; this is not the place to re-loudify it.
            const char* word = fileDrag ? (fileDragHere ? "drop to load" : "accepts samples")
                                        : "no sample";
            const Col ink = fileDrag ? (fileDragHere ? nx::live : nx::muted)
                                     : nx::muted.alpha(0.6f);
            rend_.textIn(fSmall_, well, word, ink, Align::Center);
        } else {
            // --- the picture -------------------------------------------------
            rend_.hairlineH(plot.x, plot.right(), std::round(plot.cy()));
            if (sb && sb->peakBuckets > 0) {
                // The body of the file, muted: this is material the device is
                // not playing, and §5's disabled weight is what says so.
                drawWaveform(plot, *sb, nx::muted.alpha(0.34f), 0.0, 1.0);
                // ...and the region, cyan. Same helper, same peaks, a sub-rect
                // and the matching t-range, so the two land on the same columns.
                const f32 rx0 = nx::snapPx(xOf(st0)), rx1 = nx::snapPx(xOf(en0));
                if (rx1 - rx0 >= 1.f) {
                    // Shaded under the trace when the loop is live: the region
                    // is not just what plays, it is what repeats.
                    if (loopLive)
                        rend_.rect({rx0, plot.y, rx1 - rx0, plot.h}, nx::cyan.alpha(0.07f));
                    drawWaveform({rx0, plot.y, rx1 - rx0, plot.h}, *sb, nx::cyan,
                                 (f64)st0, (f64)en0);
                }
            } else {
                // hasSample() is true and there is no peak envelope behind it:
                // a buffer built by something that did not call buildPeaks().
                // §9 -- say what happened rather than drawing an empty well
                // that looks like a bug.
                rend_.textIn(fSmall_, well, "this sample has no peak envelope to draw",
                             nx::amber.alpha(0.8f), Align::Center);
            }

            // --- the crossfade, drawn where it actually happens ---------------
            //
            // sampler.cpp mixes over the LAST `xfade` frames BEFORE End: the
            // outgoing read ramps down while the material a loop-length earlier
            // ramps up, and the two meet at exactly 1 on the wrap. So the slope
            // belongs at the End edge and nowhere else -- and it is drawn with
            // the device's own clamps applied (never past the material before
            // Start, never past half the loop), because a fade drawn longer
            // than the one that will sound is the small lie a panel never
            // recovers from.
            f32 xfT = 0.f;
            if (loopLive && has(pXfade) && frames > 1 && rate > 0.0) {
                const f64 last = (f64)(frames - 1);
                const f64 startF = (f64)st0 * last, endF = (f64)en0 * last;
                f64 xf = (f64)clampv(get(pXfade, 0.f), 0.f, 50.f) * 1e-3 * rate;
                if (xf > startF)                 xf = startF;
                if (xf > (endF - startF) * 0.5)  xf = (endF - startF) * 0.5;
                if (xf < 1.0)                    xf = 0.0;
                xfT = last > 0.0 ? (f32)(xf / last) : 0.f;
            }
            if (xfT > 0.f) {
                const f32 xa = xOf(en0 - xfT), xb = xOf(en0);
                if (xb - xa >= 1.5f) {
                    rend_.rect({xa, plot.y, xb - xa, plot.h}, nx::cyan.alpha(0.05f));
                    // The mix itself: out falling, in rising, crossing at the
                    // half. Two lines, because that is two gains.
                    rend_.line(xa, plot.y + 1.5f * s, xb, plot.bottom() - 1.5f * s,
                               1.1f * s, nx::cyan.alpha(0.5f));
                    rend_.line(xa, plot.bottom() - 1.5f * s, xb, plot.y + 1.5f * s,
                               1.1f * s, nx::cyan.alpha(0.5f));
                }
            }

            // --- the handles -------------------------------------------------
            //
            // HIT ZONES. A drawn edge is one pixel and the usability floor is
            // eight, so each handle claims a 9 logical-pixel band centred on its
            // edge and takes another 3 of slop through Ui::grab() -- 15 device
            // pixels of catch around a 1px line, and the pixels it DRAWS never
            // move. The two bands overlap once the region is under 9px wide, so
            // the NEARER edge is hit-tested last and last setHot() wins: at the
            // degenerate width the handle you are aiming at is the one you get,
            // instead of whichever happened to be drawn second.
            const f32 hw = 4.5f * s;             // half the grab band
            const f32 sx = xOf(st0), ex = xOf(en0);
            const Rect startHit{sx - hw, well.y, hw * 2.f, well.h};
            const Rect endHit  {ex - hw, well.y, hw * 2.f, well.h};
            const u64 idStart = uiId(UiSamplerWave, 1, uidKey);
            const u64 idEnd   = uiId(UiSamplerWave, 2, uidKey);
            const u64 idBody  = uiId(UiSamplerWave, 3, uidKey);

            // The body, first and therefore lowest: a drag here slides the whole
            // region without changing its length. It claims only what the two
            // handles do not.
            const Rect bodyHit{sx + hw, well.y, std::max(0.f, (ex - hw) - (sx + hw)), well.h};
            const bool canEdit = has(pStart) && has(pEnd);
            if (canEdit && bodyHit.w > 2.f) ui_.setHot(idBody, bodyHit);
            if (canEdit) {
                const f32 dS = std::fabs(in.mx - sx), dE = std::fabs(in.mx - ex);
                // Farther first, nearer second.
                if (dS <= dE) {
                    ui_.grab(3.f * s).setHot(idEnd, endHit);
                    ui_.grab(3.f * s).setHot(idStart, startHit);
                } else {
                    ui_.grab(3.f * s).setHot(idStart, startHit);
                    ui_.grab(3.f * s).setHot(idEnd, endHit);
                }
            }

            const bool hotS = ui_.isHot(idStart), hotE = ui_.isHot(idEnd);
            const bool hotB = ui_.isHot(idBody);
            // A press takes `active` for the whole gesture, which is what makes
            // undoCoalesce() fold every frame of one drag into one entry -- the
            // same mechanism a knob gets for free from knobNx().
            if (in.pressed[0] && (hotS || hotE || hotB)) {
                ui_.active = hotS ? idStart : hotE ? idEnd : idBody;
                ui_.dragAccum = 0.f;
                ui_.dragStart = (f64)(hotE ? en0 : st0);
            }
            if (ui_.active == idStart || ui_.active == idEnd || ui_.active == idBody) {
                if (in.dx != 0.f && plot.w > 1.f) {
                    // Shift is the fine sweep here for the same reason it is on
                    // every other continuous control in the program.
                    ui_.dragAccum += in.dx * (in.shift() ? 0.25f : 1.f);
                    const f32 d = ui_.dragAccum / plot.w;
                    if (ui_.active == idStart) {
                        const f32 nv = clampv((f32)ui_.dragStart + d, 0.f,
                                              std::max(0.f, en0 - kMinRegion));
                        commit(pStart, nv, idStart, "Start");
                    } else if (ui_.active == idEnd) {
                        const f32 nv = clampv((f32)ui_.dragStart + d,
                                              std::min(1.f, st0 + kMinRegion), 1.f);
                        commit(pEnd, nv, idEnd, "End");
                    } else {
                        // The length is invariant across the gesture, so it can
                        // be read live rather than stashed: clamping the start
                        // never changes it.
                        const f32 len = en0 - st0;
                        const f32 nv = clampv((f32)ui_.dragStart + d, 0.f,
                                              std::max(0.f, 1.f - len));
                        commit(pStart, nv, idBody, "region");
                        commit(pEnd, clampv(nv + len, 0.f, 1.f), idBody, "region");
                    }
                }
                if (in.released[0]) ui_.active = 0;
            }

            // Drawn after the input, so an edge lands where the drag left it in
            // the same frame rather than one behind.
            const f32 dsx = nx::snapPx(xOf(clampv(get(pStart, 0.f), 0.f, 1.f)));
            const f32 dex = nx::snapPx(xOf(clampv(get(pEnd, 1.f), 0.f, 1.f)));
            const f32 th = std::max(1.f, nx::snapPx(s));
            const auto edge = [&](f32 x, bool live, bool isStart) {
                rend_.rect({x - th * 0.5f, well.y + 2 * s, th, well.h - 4 * s},
                           nx::cyan.alpha(live ? 1.f : 0.75f));
                // The grip: a small flag at the top, on the INSIDE of the
                // region, so the two never collide when the region closes and
                // so each one says which side it owns.
                const f32 fw = 4.f * s, fh = 5.f * s;
                const f32 x0 = isStart ? x : x - fw;
                rend_.rect({x0, well.y + 2 * s, fw, fh}, nx::cyan.alpha(live ? 1.f : 0.7f));
            };
            edge(dsx, hotS || ui_.active == idStart, true);
            edge(dex, hotE || ui_.active == idEnd, false);
            if (hotB || ui_.active == idBody)
                rend_.rect({dsx, well.bottom() - 3 * s, std::max(0.f, dex - dsx), 2 * s},
                           nx::cyan.alpha(0.55f));

            // Cursors and tips. A resize cursor has already said what the
            // handles do, so they get no badge (widgets.h's badge rule); the
            // region body gets the hand that means "this moves".
            if (hotS || hotE || ui_.active == idStart || ui_.active == idEnd)
                ui_.cursor = Cursor::ResizeH;
            else if (hotB || ui_.active == idBody)
                ui_.cursor = Cursor::Hand;
            if (hotS || hotE) {
                char t[160], tm[32];
                const f32 v = hotS ? st0 : en0;
                fmtSecs(tm, sizeof tm, durSec * (f64)v);
                snprintf(t, sizeof t, "%s %.4f - %s into the file. Drag to trim; "
                                      "hold Shift for fine. (id %d)",
                         hotS ? "Start" : "End", (double)v, tm, hotS ? pStart : pEnd);
                ui_.tip = t;
            } else if (hotB) {
                char t[160], tm[32];
                fmtSecs(tm, sizeof tm, durSec * (f64)(en0 - st0));
                snprintf(t, sizeof t, "The playing region: %s. Drag to move it "
                                      "without changing its length.", tm);
                ui_.tip = t;
            }
        }

        // --- the readout row ---------------------------------------------
        // Start, End and what is between them, in seconds as well as in the
        // contract's fractions: 0.317 is not a length anybody can hear.
        {
            const Rect rr{c.x, well.bottom() + 4 * s, c.w, readH};
            const f32 third = rr.w / 3.f;
            char a[80], b[80], d[80], tm[32], whole[32];
            if (real && smp->hasSample()) {
                fmtSecs(tm, sizeof tm, durSec * (f64)st0);
                snprintf(a, sizeof a, "start %.3f  %s", (double)st0, tm);
                fmtSecs(tm, sizeof tm, durSec * (f64)en0);
                snprintf(b, sizeof b, "end %.3f  %s", (double)en0, tm);
                fmtSecs(tm, sizeof tm, durSec * (f64)(en0 - st0));
                fmtSecs(whole, sizeof whole, durSec);
                snprintf(d, sizeof d, "%s of %s", tm, whole);
            } else {
                snprintf(a, sizeof a, "start %.3f", (double)st0);
                snprintf(b, sizeof b, "end %.3f", (double)en0);
                snprintf(d, sizeof d, "-");
            }
            // Small BODY text, not a micro-label: a chip is an identity and
            // these are three numbers that change while you drag. §5's
            // uppercase wide tracking on a live readout would shout, and
            // "2.16 S OF 4.00 S" is exactly what that looks like.
            rend_.textIn(fSmall_, {rr.x, rr.y, third, rr.h}, a,
                         nx::muted.alpha(0.85f), Align::Left, 0);
            rend_.textIn(fSmall_, {rr.x + third, rr.y, third, rr.h}, b,
                         nx::muted.alpha(0.85f), Align::Center);
            rend_.textIn(fSmall_, {rr.x + third * 2.f, rr.y, third, rr.h}, d,
                         (loopLive ? nx::cyan : nx::muted).alpha(0.85f), Align::Right, 0);
        }
    }

    // =======================================================================
    // 2. PITCH
    // =======================================================================
    {
        const Rect c = col(1);
        const f32 y0 = sect(c, "PITCH", 1.f);
        // No selector row: this column's two knob rows sit where every other
        // column's do, and the band the mode columns use for a cluster is left
        // empty rather than stolen -- rows that line up across six columns are
        // most of what makes a panel read as built (app_spectra.cpp's finding).
        const f32 cw = c.w / 3.f;
        const f32 r1 = y0 + subH + gap, r2 = r1 + rowH + gap;
        Ui::KnobStyle st;

        // The root reads out as a NOTE. "60" is a program number; C4 is the key
        // this file plays back at its own speed, which is the whole meaning of
        // the parameter.
        char rootBuf[8];
        noteName(rootBuf, sizeof rootBuf, (int)std::lround(get(pRoot, 60.f)));
        st.lo = 0.f; st.hi = 127.f; st.def = 60.f; st.fmt = "%.0f";
        if (has(pRoot)) st.text = rootBuf;
        knob({c.x, r1, cw, rowH}, pRoot, "root", st, 1.f);
        st.text = nullptr;

        st.lo = -24.f; st.hi = 24.f; st.def = 0.f; st.bipolar = true; st.fmt = "%.0f";
        knob({c.x + cw, r1, cw, rowH}, pCoarse, "coarse", st, 1.f);
        st.lo = -100.f; st.hi = 100.f;
        knob({c.x + cw * 2.f, r1, cw, rowH}, pFine, "fine", st, 1.f);

        st.bipolar = false;
        st.lo = 0.f; st.hi = 500.f; st.def = 0.f;
        knob({c.x + cw, r2, cw, rowH}, pGlide, "glide", st, 1.f);
    }

    // =======================================================================
    // 3. AMP
    // =======================================================================
    {
        const Rect c = col(2);
        const f32 y0 = sect(c, "AMP", 1.f);
        const f32 cw = c.w / 3.f;
        const f32 r1 = y0 + subH + gap, r2 = r1 + rowH + gap;
        Ui::KnobStyle st;
        st.log = true; st.fmt = "%.0f";
        st.lo = 0.1f; st.hi = 5000.f; st.def = 0.5f;
        knob({c.x, r1, cw, rowH}, pAttack, "att", st, 1.f);
        st.lo = 1.f; st.hi = 5000.f; st.def = 1000.f;
        knob({c.x + cw, r1, cw, rowH}, pDecay, "dec", st, 1.f);
        st.log = false; st.lo = 0.f; st.hi = 1.f; st.def = 1.f; st.fmt = "%.2f";
        knob({c.x + cw * 2.f, r1, cw, rowH}, pSustain, "sus", st, 1.f);

        st.log = true; st.lo = 1.f; st.hi = 8000.f; st.def = 40.f; st.fmt = "%.0f";
        knob({c.x + cw * 0.5f, r2, cw, rowH}, pRelease, "rel", st, 1.f);
        // Velocity belongs with the envelope it scales, not in a "global"
        // drawer: it is the other half of how loud a note is.
        st.log = false; st.lo = 0.f; st.hi = 1.f; st.def = 1.f; st.fmt = "%.2f";
        knob({c.x + cw * 1.5f, r2, cw, rowH}, pVel, "vel", st, 1.f);
    }

    // =======================================================================
    // 4. FILTER
    // =======================================================================
    {
        const Rect c = col(3);
        const f32 y0 = sect(c, "FILTER", 1.f);
        const f32 cw = c.w / 3.f;
        const f32 r1 = y0 + subH + gap, r2 = r1 + rowH + gap;
        Ui::KnobStyle st;
        st.lo = 20.f; st.hi = 20000.f; st.def = 20000.f; st.log = true; st.fmt = "%.0f";
        knob({c.x, r1, cw, rowH}, pCutoff, "cutoff", st, 1.f);
        st.log = false;
        st.lo = 0.f; st.hi = 1.f; st.def = 0.1f; st.fmt = "%.2f";
        knob({c.x + cw, r1, cw, rowH}, pReso, "reso", st, 1.f);
        // §5's bipolar detent, so "off" is a place the knob can be seen to be
        // at: the envelope can open the filter or close it, and zero is a stop
        // rather than a corner.
        st.lo = -1.f; st.hi = 1.f; st.def = 0.f; st.bipolar = true;
        st.arc = nx::violetSoft;
        knob({c.x + cw * 2.f, r1, cw, rowH}, pEnvCut, "e>cut", st, 1.f);

        st.bipolar = false; st.arc = nx::violet;
        st.lo = 0.f; st.hi = 1.f; st.def = 0.f;
        knob({c.x + cw, r2, cw, rowH}, pKeytrack, "keytrk", st, 1.f);
    }

    // =======================================================================
    // 5. MODE -- gate, loop, crossfade
    // =======================================================================
    {
        const Rect c = col(4);
        const f32 y0 = sect(c, "MODE", 1.f);

        // Gate is the mode the other two answer to, so it is the selector row.
        const Rect gateR{c.x, y0, c.w, subH};
        boolCluster(gateR, pGate, "one-shot", "gate", 1.f, "Gate");
        if (ui_.hovered(gateR))
            ui_.tip = has(pGate)
                ? "Gate: the key holds the sound. One-shot: the note-off is "
                  "ignored and the region plays out."
                : std::string("This device has no gate parameter");

        const f32 cw = c.w / 3.f;
        const f32 r1 = y0 + subH + gap, r2 = r1 + rowH + gap;

        // THE ONE-SHOT RULE, ON THE FACE OF THE PANEL. With Gate off the device
        // will not loop whatever this switch says (sampler.cpp: `v.looping =
        // v.gate && b.loop`), so the switch goes to §5's disabled weight and the
        // tip says why. It is NOT disabled for input -- the value is real, it
        // persists, and it takes effect the moment Gate comes back on; a control
        // that refused the click would be lying in the other direction.
        const f32 loopDim = gateOn ? 1.f : 0.4f;
        const Rect loopR{c.x, r1 + (rowH - lblH - subH) * 0.5f, c.w, subH};
        // No label under it: "off | loop" already says what it is, and a "LOOP"
        // caption under a segment that reads LOOP is the caption saying it
        // twice. The gate cluster above is unlabelled for the same reason.
        boolCluster(loopR, pLoop, "off", "loop", loopDim, "Loop");
        if (ui_.hovered(loopR))
            ui_.tip = gateOn
                ? std::string("Loop the region between Start and End, with the "
                              "crossfade below hiding the splice.")
                : std::string("One-shot ignores the loop: a one-shot never sees a "
                              "note-off, so a looping one would sound until the "
                              "transport stopped. Switch Gate on to use it.");

        // The crossfade only exists inside a live loop, and it says so at the
        // disabled weight rather than sitting there looking operable.
        Ui::KnobStyle st;
        st.lo = 0.f; st.hi = 50.f; st.def = 5.f; st.fmt = "%.0f";
        knob({c.x + cw, r2, cw, rowH}, pXfade, "xfade", st, loopLive ? 1.f : 0.4f);
    }

    // =======================================================================
    // 6. GLOBAL + PRESETS
    // =======================================================================
    {
        const Rect c = col(5);
        // ABSENT, not disabled, when the device declares no presets: a row of
        // arrows around an empty name is a promise the device has not made. The
        // section's own header says so too.
        const int np = inst->presetCount();
        const f32 y0 = sect(c, np > 0 ? "GLOBAL / PRESET" : "GLOBAL", 1.f);
        if (np > 0) {
            samplerPreset_ = clampv(samplerPreset_, 0, np - 1);
            const Rect pr{c.x, y0, c.w, subH};
            ui_.segCluster(pr);
            const f32 bw = 16 * s;
            const Rect lb{pr.x, pr.y, bw, pr.h}, rb{pr.right() - bw, pr.y, bw, pr.h};
            rend_.hairlineV(lb.right(), pr.y + 2 * s, pr.bottom() - 2 * s);
            rend_.hairlineV(rb.x, pr.y + 2 * s, pr.bottom() - 2 * s);
            const auto load = [&](int d) {
                const int n = ((samplerPreset_ + d) % np + np) % np;
                // A preset rewrites every parameter at once, so it is one undo
                // entry like any other edit -- and a one-shot, so it takes a
                // point every time rather than coalescing.
                undoPoint("load preset");
                samplerPreset_ = n;
                inst->loadPreset(n);
                status_ = std::string("Sampler: ") + presetNameOf(*inst, n);
            };
            const auto chev = [&](const Rect& b, bool leftward) {
                const f32 k = 2.6f * s, d = leftward ? -1.f : 1.f;
                rend_.line(b.cx() - k * d * 0.6f, b.cy() - k,
                           b.cx() + k * d * 0.6f, b.cy(), 1.1f * s, nx::muted);
                rend_.line(b.cx() - k * d * 0.6f, b.cy() + k,
                           b.cx() + k * d * 0.6f, b.cy(), 1.1f * s, nx::muted);
            };
            if (ui_.segButton(uiId(UiSamplerPanel, 60, uidKey), lb, false, nx::violet)) load(-1);
            chev(lb, true);
            if (ui_.segButton(uiId(UiSamplerPanel, 61, uidKey), rb, false, nx::violet)) load(+1);
            chev(rb, false);
            microFit(ui_, fSmall_, {lb.right(), pr.y, rb.x - lb.right(), pr.h},
                     presetNameOf(*inst, samplerPreset_), nx::text, Align::Center, 2 * s);
            if (ui_.hovered(pr)) {
                char t[128];
                snprintf(t, sizeof t, "Preset %d of %d - the arrows step through them. "
                                      "No preset names a file; what it plays is yours.",
                         samplerPreset_ + 1, np);
                ui_.tip = t;
            }
        }

        const f32 cw = c.w / 3.f;
        const f32 r1 = y0 + subH + gap;
        Ui::KnobStyle st;
        st.lo = 1.f; st.hi = 16.f; st.def = 16.f; st.fmt = "%.0f";
        knob({c.x + cw * 0.5f, r1, cw, rowH}, pVoices, "voices", st, 1.f);
        st.lo = 0.f; st.hi = 2.f; st.def = 1.f; st.fmt = "%.2f";
        knob({c.x + cw * 1.5f, r1, cw, rowH}, pMaster, "master", st, 1.f);
    }

    rend_.popClip();
}

// ---------------------------------------------------------------------------
// The headless hooks
//
// Nothing inside gamescope can click an "edit" chip or hold a file over a
// panel, and this panel is otherwise unreachable from a screenshot. Every hook
// drives the ORDINARY code path -- the same uid the mouse would set, the same
// commit() the handles call, the same loadFile() the drop calls -- so what they
// prove is what the mouse would do, not a back door beside it.
//
//   NXTAKT_DEBUG_SAMPLER=1        open the panel on the first device in the
//                                 chain that answers sampler(), or on device 0
//                                 if none does -- which is the guarded state,
//                                 and a screenshot worth having in its own
//                                 right.
//   NXTAKT_DEBUG_SAMPLERFILE=<p>  load a file through SamplerControl::loadFile,
//                                 which is the drop target's own call.
//   NXTAKT_DEBUG_SAMPLERSET=      the parameter writer, for any list of
//     id:value,id:value,...       contract ids -- which is how the loop, the
//                                 crossfade and a trimmed region get made
//                                 without a mouse.
//   NXTAKT_DEBUG_SAMPLERDRAG=<p>  arm a browser-file drag holding <p>, so the
//                                 drop-in-flight state ("accepts samples", and
//                                 the lit edge with the pointer over the panel)
//                                 can be photographed. It is the drag the
//                                 browser arms, not a second one.
// ---------------------------------------------------------------------------
void App::debugSeedSampler() {
    if (samplerSeeded_) return;
    const char* want = env("DEBUG_SAMPLER");
    if (!want || !*want || want[0] == '0') return;
    samplerSeeded_ = true;

    ChainOwner co = chainOwner(devOwner_);
    if (!co.devices || co.devices->empty()) {
        LOGW("NXTAKT_DEBUG_SAMPLER: no device on %s to open a panel on",
             ownerName(devOwner_).c_str());
        return;
    }
    // The first device that actually plays files, if there is one. Spectra's
    // hook takes device 0 whatever it is; this one looks, because a chain with
    // a sampler in it at slot 2 is the common case and a panel forced onto the
    // delay in front of it would be the less useful of the two screenshots.
    int pick = -1;
    for (size_t i = 0; i < co.devices->size(); ++i)
        if ((*co.devices)[i].inst && isSampler((*co.devices)[i].inst.get())) {
            pick = (int)i;
            break;
        }
    if (pick < 0) pick = 0;
    DeviceModel& d = (*co.devices)[(size_t)pick];
    if (!d.inst) {
        LOGW("NXTAKT_DEBUG_SAMPLER: device %d has no instance", pick);
        return;
    }
    samplerOpenUid_ = d.uid;
    samplerForced_  = !isSampler(d.inst.get());
    samplerScrollTo_ = true;
    selDevice_ = pick;
    paramScroll_ = 0.f;
    // Ctrl+B's other half, done for the shot: the panel is as wide as the strip
    // and the file browser is 210 logical pixels of it. The hook presses a key
    // the user has.
    showBrowser_ = false;
    LOGI("NXTAKT_DEBUG_SAMPLER: panel open on %s (%d parameters)%s",
         d.desc.name.c_str(), d.inst->paramCount(),
         samplerForced_ ? " - not a sampler, so the guards are what is on screen" : "");
    status_ = samplerForced_ ? "NXTAKT_DEBUG_SAMPLER: panel forced onto " + d.desc.name
                             : "NXTAKT_DEBUG_SAMPLER: Sampler panel open";

    // The file, through the drop target's own call.
    if (const char* p = env("DEBUG_SAMPLERFILE")) {
        SamplerControl* sc = d.inst->sampler();
        if (!sc) {
            LOGW("NXTAKT_DEBUG_SAMPLERFILE: %s is not a sampler", d.desc.name.c_str());
        } else {
            undoPoint("load sample");
            if (sc->loadFile(p)) {
                LOGI("NXTAKT_DEBUG_SAMPLERFILE: loaded %s (%lld frames)", p,
                     (long long)sc->sampleFrames());
                status_ = std::string("Loaded ") + p;
            } else {
                LOGW("NXTAKT_DEBUG_SAMPLERFILE: could not load %s", p);
                status_ = std::string("Could not load ") + p +
                          " - the sampler is unchanged";
            }
        }
    }

    // The parameter writer: commit()'s three lines, in order. A hook that wrote
    // the parameter directly would prove the plugin accepts a value and nothing
    // about the path the mouse takes to it.
    const auto write = [&](int id, f32 v, const char* what) {
        if (id < 0 || id >= d.inst->paramCount()) {
            LOGW("NXTAKT_DEBUG_SAMPLER: this device has no parameter %d", id);
            return;
        }
        undoPoint(what);
        d.inst->setParam(id, v);
        if (ownIsTrack(devOwner_))
            autoCapture(addr::deviceParam(ses_.tracks[devOwner_].uid, d.uid,
                                          d.inst->paramInfo(id).id), v, 0);
        LOGI("NXTAKT_DEBUG_SAMPLER: %s (id %d) -> %g, reads back %g", what, id,
             (double)v, (double)d.inst->getParam(id));
    };
    if (const char* set = env("DEBUG_SAMPLERSET")) {
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
                LOGW("NXTAKT_DEBUG_SAMPLERSET: \"%s\" is not id:value", one.c_str());
                continue;
            }
            write(atoi(one.c_str()), (f32)atof(one.c_str() + colon + 1), "debug set");
        }
    }

    // ...and the drag, for the one state that is neither loaded nor empty.
    if (const char* p = env("DEBUG_SAMPLERDRAG")) {
        samplerDragHold_ = p;                    // the panel re-arms it every frame
        LOGI("NXTAKT_DEBUG_SAMPLERDRAG: a browser drag holding %s is in flight", p);
    }
}

} // namespace lat
