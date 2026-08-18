// Immediate-mode widget implementations. Every widget follows the same
// hot/active protocol: claim hot each frame from its bounds, take `active` on
// a press that lands inside, consume Input::dx/dy only while it owns `active`,
// and let Ui::endFrame() release ownership on mouse-up.
//
// Everything here is drawn in the NX design language (docs/DESIGN.md §5): glass
// pills, recessed wells, gradient hairlines, one light source in the upper
// left. The signatures did not change, so the whole program was re-skinned by
// this file rather than by three hundred call sites.
#include "widgets.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace lat {

// Text that sits ON a colored fill: dark ink on bright fills (the pastel clip
// colors), light ink on dark fills (the purple accent). Rec.601 luma.
static Col inkOn(const Col& fill) {
    const f32 luma = 0.299f * fill.r + 0.587f * fill.g + 0.114f * fill.b;
    return luma > 0.45f ? pal::textOnClip : Col(0.94f, 0.92f, 1.f, 1.f);
}

namespace {

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kDeg = kPi / 180.f;

// ---------------------------------------------------------------------------
// §6  The motion table
//
// One row per widget id that has recently been hovered or pressed, holding the
// moment each of those last CHANGED and the weight it held at that moment. A
// weight is then `from + (to - from) * ease(now - t0)`, which means a hover
// that reverses mid-fade continues from where it actually is instead of
// snapping to 1 and falling again.
//
// Fixed size, flat, file-scope: it is scanned linearly (128 rows is a few
// cache lines and the scan stops at the first hit), it allocates nothing on any
// frame, and a widget that has stopped being drawn is reclaimed as the
// least-recently-seen row. There is no per-frame sweep and nothing to free.
//
// 128 rows against a screen that draws perhaps thirty interactive widgets at
// once: the table only ever holds ids that have been touched, so the working
// set is "widgets the pointer has visited", not "widgets on screen".
// ---------------------------------------------------------------------------
struct MotionSlot {
    u64  id = 0;
    f64  seen = 0;                    // last frame this id asked
    f64  hoverT0 = 0, pressT0 = 0;    // when the target last flipped
    f32  hoverFrom = 0.f, pressFrom = 0.f;
    bool hoverOn = false, pressOn = false;
};

constexpr int kMotionSlots = 128;
MotionSlot g_motion[kMotionSlots];

// One channel of the table: hold the target, ease from wherever we were.
inline f32 rampAt(f64 now, f64 t0, f32 from, bool on) {
    const f32 to = on ? 1.f : 0.f;
    return from + (to - from) * nx::easeSoft.at((f32)(now - t0), nx::durFast);
}

// §5's primary fill in an arbitrary hue: the inner top highlight over the
// tint, draining to a darker bottom-right. Exactly nx::violetFill's ramp,
// generated rather than quoted so that a red RECORD pill, a violet PLAY pill
// and a clip-coloured launch button are visibly the same material lit by the
// same lamp. Interned by value, so one hue costs one gradient row per frame
// however many pills wear it.
nx::Grad fillOf(const Col& c) {
    return {{{c.mix(Col(1.f, 1.f, 1.f, c.a), 0.24f), 0.00f},
             {c,                                     0.55f},
             {c.scale(0.60f),                        1.00f}}, 3, 170.f};
}

// Knob sweep, measured from 12 o'clock: -135deg .. +135deg. The `arc` helper
// works in screen angles where 0 points right and the angle grows clockwise,
// so straight up is -90deg and the sweep becomes -225deg .. +45deg.
constexpr f32 kKnobA0 = -225.f * kDeg;
constexpr f32 kKnobA1 = 45.f * kDeg;
constexpr f32 kKnobTop = -90.f * kDeg;

// LOGICAL pixels of vertical travel that cover a knob's full range. Logical
// and not device: a knob on a 1.25x screen is 25% bigger, and a sweep measured
// in raw device pixels would make the same physical hand movement cover 20%
// less of the same control. Every caller multiplies by the renderer's DPI.
constexpr f32 kKnobTravel = 150.f;

// --- the two fine-drag rates, declared together so the divergence is a
// decision rather than an accident (see the interaction pass's §4 table).
//
// kFineSweep is for a control with a BOUNDED sweep -- a knob, a fader, a
// trough. Its coarse rate is "the whole range in kKnobTravel px", so a quarter
// rate spends 600 logical px on the range: still one screen, still one gesture.
//
// kFineNumber is for dragNumber, whose coarse rate is a caller-supplied
// units-per-pixel and is usually already the useful step (0.1 BPM/px, 1 ms/px).
// A quarter of that lands on a quarter of a BPM, which is not a fine tempo; a
// tenth is. The numbers differ because what they are a tenth OF differs by
// orders of magnitude, and one shared constant would make one of the two wrong.
constexpr f32 kFineSweep  = 0.25f;
constexpr f32 kFineNumber = 0.1f;

inline f32 fineScale(const Input* in) { return in->shift() ? kFineSweep : 1.f; }

inline f32 norm01(f32 v, f32 lo, f32 hi) {
    return (hi - lo) > 1e-9f ? clampv((v - lo) / (hi - lo), 0.f, 1.f) : 0.f;
}

// --- KnobStyle's two mappings ----------------------------------------------
//
// A logarithmic parameter is one whose USEFUL resolution is per-octave: an
// 8 kHz cutoff and a 20 Hz one are one knob apart in music and three orders of
// magnitude apart in hertz, and dragging that range linearly spends the top
// four fifths of the sweep above 4 kHz. So both the drag and the arc live in
// t = log(v/lo)/log(hi/lo) instead, and the flag is honoured only when the
// range can actually carry a logarithm -- a mis-flagged parameter degrades to
// linear rather than to a NaN that would then be written into a plugin.
inline bool knobLog(const Ui::KnobStyle& st) {
    return st.log && st.lo > 1e-9f && st.hi > st.lo;
}
inline f32 knobT(const Ui::KnobStyle& st, f32 v) {
    const f32 lo = std::min(st.lo, st.hi), hi = std::max(st.lo, st.hi);
    if (knobLog(st))
        return clampv(std::log(clampv(v, lo, hi) / st.lo) / std::log(st.hi / st.lo), 0.f, 1.f);
    return norm01(v, lo, hi);
}
inline f32 knobV(const Ui::KnobStyle& st, f32 t) {
    t = clampv(t, 0.f, 1.f);
    if (knobLog(st)) return st.lo * std::exp(t * std::log(st.hi / st.lo));
    return st.lo + (st.hi - st.lo) * t;
}

// A liquid fill in an arbitrary hue: fillOf()'s ramp turned along the 147deg
// axis every lit edge in the system uses, so a trough is lit by the same lamp
// as the glass it sits in.
nx::Grad liquidOf(const Col& c) {
    return {{{c.mix(Col(1.f, 1.f, 1.f, c.a), 0.34f), 0.00f},
             {c,                                     0.55f},
             {c.scale(0.55f),                        1.00f}}, 3, 147.f};
}

// ---------------------------------------------------------------------------
// NXTAKT_DEBUG_PROBE -- the read-only half of the headless drive
//
// Nothing inside gamescope can look at a fader and say what it reads. A gesture
// driven by xdotool is only a check if the model can be read back afterwards,
// and the value a fader carries lives in whatever view owns it -- in five
// different translation units, several of which this pass does not own.
//
// So the probe sits under the WIDGETS instead: every widget in the program that
// writes a continuous value writes it through one of the five functions below,
// and each of them says so here. One line per change, the widget's own id, the
// old value and the new one. It reads nothing it was not already handed, it
// writes nothing, and with the variable unset it costs one already-cached bool
// per changed value and nothing at all on a frame where nothing moved.
//
// Deliberately not a per-frame dump: a drag reports every frame it moves on,
// which is what makes "this gesture moved this control and no other" a thing
// the log can be grepped for.
bool probeOn() {
    static const bool on = std::getenv("NXTAKT_DEBUG_PROBE") != nullptr;
    return on;
}
void probeValue(const char* kind, u64 id, f64 from, f64 to) {
    if (!probeOn()) return;
    LOGI("NXTAKT_DEBUG_PROBE: %s id=%016llx %.6f -> %.6f", kind,
         (unsigned long long)id, from, to);
}

} // namespace

// ---------------------------------------------------------------------------
// §5 / §6  The NX vocabulary
// ---------------------------------------------------------------------------

UiMotion Ui::motion(u64 id, bool hot, bool held) {
    // §6 is non-negotiable: under reduced motion the weights are the endpoints
    // themselves, so every `* m.hover` in this file becomes an instant switch
    // and no lift, scale, glow or slide is ever in an intermediate state.
    if (nx::reducedMotion() || !r) return {hot ? 1.f : 0.f, held ? 1.f : 0.f};

    const f64 now = r->time();
    MotionSlot* s = nullptr;
    MotionSlot* oldest = &g_motion[0];
    for (MotionSlot& m : g_motion) {
        if (m.id == id) { s = &m; break; }
        if (m.seen < oldest->seen) oldest = &m;
    }
    if (!s) {
        // A brand-new id starts settled in whatever state it is already in, so
        // a widget that appears under the pointer does not play its hover-in.
        s = oldest;
        *s = MotionSlot{};
        s->id = id;
        s->hoverT0 = s->pressT0 = now - (f64)nx::durFast;
        s->hoverFrom = s->hoverOn = hot;
        s->pressFrom = s->pressOn = held;
    }
    s->seen = now;

    if (hot != s->hoverOn) {
        s->hoverFrom = rampAt(now, s->hoverT0, s->hoverFrom, s->hoverOn);
        s->hoverOn = hot;
        s->hoverT0 = now;
    }
    if (held != s->pressOn) {
        s->pressFrom = rampAt(now, s->pressT0, s->pressFrom, s->pressOn);
        s->pressOn = held;
        s->pressT0 = now;
    }
    return {rampAt(now, s->hoverT0, s->hoverFrom, s->hoverOn),
            rampAt(now, s->pressT0, s->pressFrom, s->pressOn)};
}

Rect Ui::liftPress(const Rect& b, const UiMotion& m) const {
    const f32 dpi = r ? std::max(1.f, r->dpiScale()) : 1.f;
    // Hover lifts 1.5px (§5 says 1-2). Press takes it back down as it shrinks,
    // so a held button sits into the surface rather than hovering while pressed.
    const f32 y = b.y - 1.5f * dpi * m.hover * (1.f - m.press);
    const f32 k = 1.f - 0.04f * m.press;                 // §5: scale to 0.96
    const f32 dw = b.w * (1.f - k) * 0.5f, dh = b.h * (1.f - k) * 0.5f;
    return {b.x + dw, y + dh, b.w - dw * 2.f, b.h - dh * 2.f};
}

void Ui::pillRect(const Rect& b, f32 radius, Pill kind, const Col& tint,
                  const UiMotion& m) const {
    if (!r || b.w <= 0.f || b.h <= 0.f) return;
    const f32 dpi = std::max(1.f, r->dpiScale());
    // radius<0 = "the control radius": the TOKEN, not a capsule. h*0.5 here
    // is what made every button an unrelated pill -- see theme.h's geometry note.
    const f32 rad = radius < 0.f ? std::min(nx::pill * dpi, b.h * 0.5f) : radius;

    switch (kind) {
    case Pill::Primary:
    case Pill::Danger: {
        // The glow: §5's "soft violet glow, bloomed on hover". It is the one
        // shadow in the language that is not black, and it is what makes a
        // primary action read as lit from within rather than merely filled.
        // Alpha rides the hover weight -- the SPEC's colour, not the gradient,
        // so animating it interns nothing.
        const f32 glow = 0.34f + 0.30f * m.hover;
        r->shadow(b, rad, nx::ShadowSpec{0.f, 3.f * dpi, 14.f * dpi, -2.f * dpi,
                                         tint.alpha(glow)});
        r->gradRect(b, rad, fillOf(tint));
        // The lit edge, brighter while hovered. One light source, upper left.
        r->gradStroke(b, rad, dpi, nx::edge, 0.85f + 0.15f * m.hover);
        break;
    }
    case Pill::Ghost:
        // Nothing at rest; the glass arrives with the pointer. The fill is
        // faded through gradRect's alphaMul rather than through Grad::faded, so
        // a row of ghosts sharing one hover value shares one gradient row.
        if (m.hover > 0.004f || m.press > 0.004f) {
            r->gradRect(b, rad, nx::glassChip,
                        clampv(0.55f * m.hover + 0.45f * m.press, 0.f, 1.f));
            r->gradStroke(b, rad, dpi, nx::edge, 0.75f * m.hover);
        }
        break;
    case Pill::Secondary:
    default:
        r->gradRect(b, rad, nx::glassChip, 0.80f + 0.20f * m.hover);
        r->gradStroke(b, rad, dpi, nx::edge, 0.75f + 0.25f * m.hover);
        // A hovered secondary lifts into a faint violet wash rather than a
        // lighter grey: §1, violet leads even in the small states.
        if (m.hover > 0.004f)
            r->roundRect(b, rad, nx::violet.alpha(0.10f * m.hover));
        break;
    }
}

// ---------------------------------------------------------------------------
// Text batching
// ---------------------------------------------------------------------------

void Ui::beginDeferText() { deferText = true; textJobN = 0; }

void Ui::flushText() {
    deferText = false;                  // before the loop: these draw for real
    if (!r) { textJobN = 0; return; }
    for (int i = 0; i < textJobN; ++i) {
        const TextJob& j = textJobs[i];
        if (!j.f) continue;
        if (j.micro) microIn(*j.f, j.b, j.s, j.c, j.a, j.padX);
        else         r->textIn(*j.f, j.b, j.s, j.c, j.a, j.padX);
    }
    textJobN = 0;
}

// Take a label into the queue, or report that it could not be taken -- because
// the window is closed, the queue is full, or the string is longer than a slot
// holds. A caller that gets `false` draws immediately, so overflowing this
// queue costs the draw call it was trying to save and never a missing label.
static bool queueText(Ui& ui, const Font& f, const Rect& b, const char* s,
                      const Col& c, Align a, f32 padX, bool micro) {
    if (!ui.deferText || ui.textJobN >= Ui::kMaxTextJobs) return false;
    Ui::TextJob& j = ui.textJobs[ui.textJobN];
    size_t n = 0;
    while (s[n] && n < sizeof j.s - 1) { j.s[n] = s[n]; ++n; }
    if (s[n]) return false;             // truncating would be a silent lie
    j.s[n] = 0;
    j.f = &f; j.b = b; j.c = c; j.a = a; j.padX = padX; j.micro = micro;
    ++ui.textJobN;
    return true;
}

void Ui::drawTextIn(const Font& f, const Rect& b, const char* s, const Col& c,
                    Align a, f32 padX) {
    if (!r || !s) return;
    if (!queueText(*this, f, b, s, c, a, padX, false)) r->textIn(f, b, s, c, a, padX);
}

f32 Ui::microLabel(const Font& f, f32 x, f32 y, const char* s, const Col& c) {
    if (!r || !s) return x;
    f32 pen = std::round(x);
    const f32 track = std::max(1.f, (f32)f.size() * nx::microTracking);
    for (const char* p = s; *p; ++p) {
        const char up[2] = {(char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p), 0};
        pen = r->text(f, pen, y, up, c);
        pen += track;
    }
    return pen;
}

f32 Ui::microWidth(const Font& f, const char* s) const {
    if (!s || !*s) return 0.f;
    f32 w = 0.f;
    const f32 track = std::max(1.f, (f32)f.size() * nx::microTracking);
    for (const char* p = s; *p; ++p) {
        const char up[2] = {(char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p), 0};
        w += f.measure(up) + track;
    }
    return w - track;                    // no trailing space after the last glyph
}

void Ui::microIn(const Font& f, const Rect& b, const char* s, const Col& c,
                 Align a, f32 padX) {
    if (!r || !s || !*s) return;
    if (queueText(*this, f, b, s, c, a, padX, true)) return;
    const f32 w = microWidth(f, s);
    f32 x = b.x + padX;
    if (a == Align::Center)     x = b.cx() - w * 0.5f;
    else if (a == Align::Right) x = b.right() - padX - w;
    microLabel(f, x, b.y + (b.h - f.height()) * 0.5f, s, c);
}

void Ui::chip(const Rect& b, const char* label, const Col& ink) {
    if (!r) return;
    const f32 dpi = std::max(1.f, r->dpiScale());
    const f32 chipRad = std::min(nx::pill * dpi, b.h * 0.5f);
    r->gradRect(b, chipRad, nx::glassChip);
    r->gradStroke(b, chipRad, dpi, nx::edge, 0.8f);
    Font* f = fSmall ? fSmall : fBody;
    if (f) microIn(*f, b, label, ink, Align::Center);
}

void Ui::fieldWell(const Rect& b, f32 focus, bool deep) const {
    if (!r || b.w <= 0.f || b.h <= 0.f) return;
    const f32 dpi = std::max(1.f, r->dpiScale());
    const f32 rad = std::min(nx::radiusSm * dpi, b.h * 0.5f);
    r->well(b, rad, deep);
    // The border runs --line -> violet as focus arrives, and the ring blooms
    // with it. §5: never a bare outline.
    const f32 k = clampv(focus, 0.f, 1.f);
    r->roundRectOutline(b, rad, std::max(1.f, std::round(dpi)),
                        nx::line.mix(nx::violet, k));
    if (k > 0.02f) {
        const f32 s = std::max(1.f, dpi);
        r->roundRectOutline(b.inset(-3.5f * s), rad + 3.5f * s, 3.f * s,
                            nx::violet.alpha(0.20f * k));
        r->roundRectOutline(b.inset(-1.0f * s), rad + 1.0f * s, 2.f * s,
                            nx::violet.alpha(0.60f * k));
    }
}

// §5's tab pill. ONE indicator, translated -- not two backgrounds toggled.
//
// The slide is the identity move of the whole language, so it is the one place
// in this file that gets --ease-spring and its overshoot. `nx::spring()` hands
// back --ease-soft under reduced motion, which is what "springs replaced"
// means concretely.
bool Ui::tabPill(u64 id, const Rect& b, const char* const* labels, int count, int* idx) {
    if (!r || !idx || !labels || count <= 0) return false;
    const f32 dpi = std::max(1.f, r->dpiScale());
    const f32 pad = std::max(1.f, std::round(2.f * dpi));
    const Rect track = b.inset(pad);
    const f32 slotW = track.w / (f32)count;

    *idx = clampv(*idx, 0, count - 1);
    const int was = *idx;
    bool changed = false;

    // --- interaction: one id per slot, so hover reads per tab ---------------
    int hotSlot = -1;
    for (int i = 0; i < count; ++i) {
        const Rect s{track.x + slotW * (f32)i, track.y, slotW, track.h};
        const u64 sid = id ^ ((u64)(i + 1) * 0x9E3779B97F4A7C15ull);
        if (setHot(sid, s) && isHot(sid)) {
            hotSlot = i;
            cursor = Cursor::Hand;
            if (in->pressed[0]) active = sid;
            if (in->released[0] && active == sid) {
                if (i != *idx) { *idx = i; changed = true; }
                active = 0;
            }
        }
    }

    // --- the indicator's position, remembered across frames -----------------
    //
    // A flat table keyed on the pill's own id, exactly like the motion table
    // and for the same reason: there is no widget object to hold it. It stores
    // where the slide started and when, so a click landing mid-slide is picked
    // up from where the indicator actually is.
    struct Slide { u64 id = 0; f64 seen = 0, t0 = 0; f32 from = 0.f; int to = 0; };
    static Slide g_slide[8];
    Slide* sl = nullptr;
    Slide* oldest = &g_slide[0];
    for (Slide& s : g_slide) {
        if (s.id == id) { sl = &s; break; }
        if (s.seen < oldest->seen) oldest = &s;
    }
    const f64 now = r->time();
    if (!sl) {
        sl = oldest;
        *sl = Slide{};
        sl->id = id;
        sl->from = (f32)*idx;
        sl->to = *idx;
        sl->t0 = now - (f64)nx::durSlow;
    }
    sl->seen = now;
    const nx::Ease& ease = nx::spring();
    if (sl->to != *idx) {
        sl->from = sl->from + ((f32)sl->to - sl->from) *
                              ease.at((f32)(now - sl->t0), nx::durSlow);
        sl->to = *idx;
        sl->t0 = now;
    }
    const f32 at = sl->from + ((f32)sl->to - sl->from) *
                              ease.at((f32)(now - sl->t0), nx::durSlow);

    // --- draw: the housing, the indicator, then every label ----------------
    // Shapes first and text after, deliberately: the batcher pays a draw call
    // for every shape->text->shape alternation, and a tab strip that drew each
    // slot complete would cost one per tab.
    const f32 tabRad = std::min(nx::pill * dpi, b.h * 0.5f);
    r->gradRect(b, tabRad, nx::glassChip, 0.55f);
    r->gradStroke(b, tabRad, dpi, nx::edge, 0.7f);

    const Rect ind{track.x + slotW * at, track.y, slotW, track.h};
    const UiMotion im = motion(id, hotSlot >= 0, active != 0 && hotSlot >= 0);
    pillRect(ind, std::min(nx::pill * dpi, ind.h * 0.5f), Pill::Primary, nx::violet, im);

    Font* f = fSmall ? fSmall : fBody;
    if (f) {
        for (int i = 0; i < count; ++i) {
            const Rect s{track.x + slotW * (f32)i, track.y, slotW, track.h};
            // The ink follows the indicator rather than the index, so the text
            // brightens as the pill arrives under it instead of flipping.
            const f32 under = clampv(1.f - std::fabs(at - (f32)i), 0.f, 1.f);
            const Col c = nx::muted.mix(nx::text, under)
                                   .mix(nx::text, i == hotSlot ? 0.4f : 0.f);
            microIn(*f, s, labels[i] ? labels[i] : "", c, Align::Center);
        }
    }
    return changed || was != *idx;
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

void Ui::arc(f32 cx, f32 cy, f32 rad, f32 a0, f32 a1, f32 th, const Col& c) {
    if (!r || rad <= 0.f) return;
    const f32 span = a1 - a0;
    if (std::fabs(span) < 1e-4f) return;
    // ~2.5 degrees per segment, so the polyline reads as a curve at any size.
    int segs = (int)std::ceil(std::fabs(span) / (2.5f * kDeg));
    segs = clampv(segs, 2, 256);
    const f32 step = span / (f32)segs;
    f32 px = cx + std::cos(a0) * rad;
    f32 py = cy + std::sin(a0) * rad;
    for (int i = 1; i <= segs; ++i) {
        const f32 a = a0 + step * (f32)i;
        const f32 nx = cx + std::cos(a) * rad;
        const f32 ny = cy + std::sin(a) * rad;
        r->line(px, py, nx, ny, th, c);
        px = nx; py = ny;
    }
}

void Ui::bevel(const Rect& b, f32 radius, const Col& fill, f32 lightness) {
    if (!r || b.w <= 0.f || b.h <= 0.f) return;
    r->roundRect(b, radius, fill);
    // The top highlight, as a HAIRLINE rather than the solid pixel row this
    // used to be: it fades out at both ends instead of stopping dead against
    // the corner radius, which is §11's rule and, at this size, also simply
    // looks like light rather than like a drawn line.
    if (lightness <= 0.f) return;
    const f32 inset = std::min(radius, b.w * 0.5f);
    if (b.w - inset * 2.f <= 1.f) return;
    const f32 dpi = std::max(1.f, r->dpiScale());
    r->hairlineH(b.x + inset, b.right() - inset, b.y,
                 Col(1.f, 1.f, 1.f, clampv(lightness * 2.4f, 0.f, 0.5f)), dpi);
}

void Ui::playTriangle(const Rect& b, const Col& c) {
    if (!r || b.w <= 0.f || b.h <= 0.f) return;
    r->triangle(b.x, b.y, b.x, b.bottom(), b.right(), b.cy(), c);
}

void Ui::stopSquare(const Rect& b, const Col& c) {
    if (!r || b.w <= 0.f || b.h <= 0.f) return;
    const f32 s = std::min(b.w, b.h) * 0.6f;
    r->rect({std::round(b.cx() - s * 0.5f), std::round(b.cy() - s * 0.5f), s, s}, c);
}

// ---------------------------------------------------------------------------
// The cursor badge
//
// Six glyphs, all geometry. Each is drawn inside a square `g` that is the
// backing plate inset by a couple of pixels, so every one of them is the same
// visual weight whatever it says -- a "+" that outsized an "x" beside it would
// read as the more important verb rather than as a different one.
//
// The plate is placed south-east of the hotspot and clamped into the viewport,
// which is the one case where it moves: a badge that ran off the bottom-right
// corner of the window would be a badge nobody ever saw, and the corner is
// exactly where a hand ends up when it is reaching for the last lane.
// ---------------------------------------------------------------------------
void Ui::drawBadge(Renderer& rr, Font& f) const {
    (void)f;
    if (badge == Badge::None || !in) return;
    const f32 dpi = std::max(1.f, rr.dpiScale());
    const f32 side = std::round(13.f * dpi);
    // South-east of the arrow's own bounding box (about 12x19 device px at 1x),
    // so the plate clears the cursor bitmap instead of hiding under it.
    f32 px = std::round(in->mx + 11.f * dpi);
    f32 py = std::round(in->my + 13.f * dpi);
    const Rect vp = rr.currentClip();
    px = clampv(px, vp.x, std::max(vp.x, vp.right() - side));
    py = clampv(py, vp.y, std::max(vp.y, vp.bottom() - side));
    const Rect plate{px, py, side, side};

    const f32 one = std::max(1.f, std::round(dpi));
    // A dark plate rather than a glass one: this thing lands over waveforms,
    // over clip colours and over the star field, and the only backing that
    // works on all three is opaque shadow.
    rr.roundRect(plate, nx::radiusXs * dpi, nx::bgTop.alpha(0.88f));
    rr.roundRectOutline(plate, nx::radiusXs * dpi, one, nx::line.alpha(0.9f));

    const Rect g = plate.inset(std::round(3.f * dpi));
    const Col ink = nx::text;
    const f32 cx = std::round(g.cx()), cy = std::round(g.cy());

    switch (badge) {
    case Badge::Add:
    case Badge::Duplicate: {
        // Duplicate is Add with the copy it is about to leave behind drawn
        // under it: the verb is the same ("one more of these"), and the second
        // outline is what says where the extra one comes from.
        if (badge == Badge::Duplicate)
            rr.roundRectOutline({g.x, g.y, g.w * 0.62f, g.h * 0.62f}, 0.f, one,
                                ink.alpha(0.45f));
        const f32 arm = std::round(g.w * (badge == Badge::Duplicate ? 0.30f : 0.42f));
        const f32 ox  = badge == Badge::Duplicate ? std::round(g.w * 0.16f) : 0.f;
        rr.rect({cx + ox - arm, cy - one * 0.5f + ox, arm * 2.f, one}, ink);
        rr.rect({cx + ox - one * 0.5f, cy + ox - arm, one, arm * 2.f}, ink);
        break;
    }
    case Badge::Draw: {
        // A pen: the shaft along the SW-NE diagonal with a nib triangle at the
        // low end. Two quads and a triangle, and at 13 px it is the silhouette
        // that reads rather than the detail.
        const f32 x0 = g.x + one, y0 = g.bottom() - one;
        const f32 x1 = g.right() - one, y1 = g.y + one;
        rr.line(x0 + g.w * 0.28f, y0 - g.h * 0.28f, x1, y1, one * 1.6f, ink);
        rr.triangle(x0, y0, x0 + g.w * 0.36f, y0 - g.h * 0.16f,
                    x0 + g.w * 0.16f, y0 - g.h * 0.36f, ink);
        break;
    }
    case Badge::Split: {
        // The cut, and the two pieces it leaves: a full-height rule down the
        // middle with a wedge falling away on either side of it.
        rr.rect({cx - one * 0.5f, g.y, one, g.h}, ink);
        const f32 w = std::round(g.w * 0.30f), h = std::round(g.h * 0.26f);
        rr.triangle(cx - one * 1.5f, cy - h, cx - one * 1.5f, cy + h,
                    cx - one * 1.5f - w, cy, ink.alpha(0.8f));
        rr.triangle(cx + one * 1.5f, cy - h, cx + one * 1.5f, cy + h,
                    cx + one * 1.5f + w, cy, ink.alpha(0.8f));
        break;
    }
    case Badge::Delete: {
        // An x, in --danger: this is the one badge whose verb cannot be undone
        // by doing it again, and §1 spends red on exactly that.
        const f32 a = std::round(g.w * 0.36f);
        rr.line(cx - a, cy - a, cx + a, cy + a, one * 1.4f, nx::danger);
        rr.line(cx - a, cy + a, cx + a, cy - a, one * 1.4f, nx::danger);
        break;
    }
    case Badge::None: break;
    }
}

void Ui::meterV(const Rect& b, f32 lvl, f32 peak) {
    if (!r || b.w <= 0.f || b.h <= 0.f) return;
    r->rect(b, pal::appBg);

    auto mapped = [](f32 g) {
        return clampv((gainToDb(clampv(g, 0.f, 8.f)) + 60.f) / 66.f, 0.f, 1.f);
    };

    const f32 n = mapped(lvl);
    if (n > 0.f) {
        // Three bands stacked bottom-up: green, amber, red. Each segment is
        // clipped to how far the level actually reached.
        const f32 bands[3] = {0.75f, 0.92f, 1.f};
        const Col cols[3] = {pal::meterGreen, pal::meterAmber, pal::meterRed};
        f32 from = 0.f;
        for (int i = 0; i < 3; ++i) {
            const f32 to = std::min(n, bands[i]);
            if (to > from) {
                const f32 y0 = b.bottom() - to * b.h;
                const f32 y1 = b.bottom() - from * b.h;
                r->rect({b.x, y0, b.w, y1 - y0}, cols[i]);
            }
            from = bands[i];
            if (n <= bands[i]) break;
        }
    }

    const f32 p = mapped(peak);
    if (p > 0.f) {
        const f32 y = clampv(b.bottom() - p * b.h, b.y, b.bottom() - 1.f);
        const Col pc = p > 0.92f ? pal::meterRed : (p > 0.75f ? pal::meterAmber : pal::text);
        r->rect({b.x, std::round(y), b.w, 1.f}, pc);
    }
}

// ---------------------------------------------------------------------------
// Button
// ---------------------------------------------------------------------------

// §5's glass pill. Primary (violet-filled, inner top highlight, soft glow) when
// `on`, secondary (--glass-chip over --edge) when not; hover lifts 1-2px and
// blooms the glow, press scales to 0.96. `onCol` is the hue the caller means,
// not a flat fill any more -- fillOf() lights it from the upper left like
// everything else, so RECORD's red and the transport's violet are one material.
bool Ui::button(u64 id, const Rect& b, const char* label, bool on, Col onCol, f32 radius) {
    if (!r) return false;
    const bool over = setHot(id, b);
    const bool hotNow = isHot(id);
    bool clicked = false;

    if (in->pressed[0] && hotNow) active = id;
    if (in->released[0] && active == id) {
        if (over) clicked = true;
        active = 0;
    }

    const bool held = (active == id) && over;
    const UiMotion m = motion(id, hotNow, held);
    const Rect br = liftPress(b, m);
    lastRect = br;
    const f32 rad = radius < 0.f
        ? std::min(nx::pill * std::max(1.f, r->dpiScale()), br.h * 0.5f) : radius;

    // Danger is a role, not a colour a caller happened to pick: red arrives
    // here only from the transport's record plate and the delete verbs, both of
    // which §1 admits. Everything else lands on Primary and keeps its own hue.
    const bool danger = onCol.r > 0.6f && onCol.g < 0.45f && onCol.b < 0.55f;
    pillRect(br, rad, on ? (danger ? Pill::Danger : Pill::Primary) : Pill::Secondary,
             onCol, m);

    const Col fg = on ? inkOn(onCol)
                      : nx::muted.mix(nx::text, 0.55f + 0.45f * m.hover);
    if (label && *label && fBody) drawTextIn(*fBody, br, label, fg, Align::Center, 3.f);
    if (hotNow) cursor = Cursor::Hand;
    return clicked;
}

void Ui::segCluster(const Rect& b) const {
    if (!r) return;
    const f32 dpi = std::max(1.f, r->dpiScale());
    const f32 rad = std::min(nx::pill * dpi, b.h * 0.5f);
    r->gradRect(b, rad, nx::glassChip, 0.85f);
    r->gradStroke(b, rad, dpi, nx::edge, 0.7f);
}

bool Ui::segButton(u64 id, const Rect& b, bool on, Col onCol) {
    if (!r) return false;
    const bool over = setHot(id, b);
    const bool hotNow = isHot(id);
    bool clicked = false;

    if (in->pressed[0] && hotNow) active = id;
    if (in->released[0] && active == id) {
        if (over) clicked = true;
        active = 0;
    }

    const bool held = (active == id) && over;
    const UiMotion m = motion(id, hotNow, held);
    // Press scale only, never lift: a segment that lifts out of its cluster
    // reads as the panel coming apart. The cluster is the thing with edges.
    const Rect br = liftPress(b, {0.f, m.press});
    lastRect = br;
    const f32 dpi = std::max(1.f, r->dpiScale());
    const f32 rad = std::min(nx::radiusXs * dpi, br.h * 0.5f);

    const bool danger = onCol.r > 0.6f && onCol.g < 0.45f && onCol.b < 0.55f;
    if (on) {
        pillRect(br, rad, danger ? Pill::Danger : Pill::Primary, onCol, m);
    } else if (m.hover > 0.01f) {
        r->gradRect(br, rad, nx::glassChip, 0.55f * m.hover);
    }
    if (hotNow) cursor = Cursor::Hand;
    return clicked;
}

// ---------------------------------------------------------------------------
// Square toggle (M / S / arm)
// ---------------------------------------------------------------------------

bool Ui::squareToggle(u64 id, const Rect& b, const char* label, bool* value, Col onCol) {
    if (!value) return false;
    const bool over = setHot(id, b);
    const bool hotNow = isHot(id);
    bool changed = false;

    if (in->pressed[0] && hotNow) active = id;
    if (in->released[0] && active == id) {
        if (over) { *value = !*value; changed = true; }
        active = 0;
    }

    const bool held = (active == id) && over;
    const UiMotion m = motion(id, hotNow, held);
    // A badge, not a button: it keeps its footprint (these sit shoulder to
    // shoulder in the mixer, where a lift would read as the row coming apart)
    // and takes only the press scale. §5's chip radius, not the pill's -- an
    // 11px square at pill radius is a dot.
    const Rect br = liftPress(b, {0.f, m.press});
    lastRect = br;
    const f32 rad = std::min(nx::radiusXs * (r ? std::max(1.f, r->dpiScale()) : 1.f),
                             std::min(br.w, br.h) * 0.34f);

    Col fg = pal::textDim;
    if (*value) {
        pillRect(br, rad, Pill::Primary, onCol, m);
        fg = inkOn(onCol);
    } else {
        pillRect(br, rad, Pill::Secondary, onCol, m);
        fg = pal::textDim.mix(nx::text, m.hover);
    }

    if (label && *label) {
        Font* f = fSmall ? fSmall : fBody;
        if (f) drawTextIn(*f, br, label, fg, Align::Center, 1.f);
    }
    if (hotNow) cursor = Cursor::Hand;
    return changed;
}

// ---------------------------------------------------------------------------
// Knob
// ---------------------------------------------------------------------------

bool Ui::knob(u64 id, const Rect& b, f32* v, f32 lo, f32 hi, f32 def, const char* fmt) {
    if (!v || !r) return false;
    const bool over = setHot(id, b);
    const bool hotNow = isHot(id);
    bool changed = false;

    if (in->pressed[0] && hotNow) {
        active = id;
        dragAccum = 0.f;
        dragStart = (f64)*v;
    }
    if (in->dblClick && over) {
        *v = clampv(def, lo, hi);
        dragStart = (f64)*v;
        dragAccum = 0.f;
        changed = true;
    }
    if (active == id && in->dy != 0.f) {
        // Up is more. Accumulate in pixels so a fine-drag modifier can be
        // toggled mid-gesture without the value jumping.
        dragAccum += -in->dy * fineScale(in);
        const f32 travel = kKnobTravel * std::max(1.f, r->dpiScale());
        const f32 nv = (f32)dragStart + (dragAccum / travel) * (hi - lo);
        const f32 cl = clampv(nv, lo, hi);
        if (cl != *v) { probeValue("knob", id, *v, cl); *v = cl; changed = true; }
    }
    if (in->released[0] && active == id) active = 0;

    // --- layout ---
    Font* vf = fSmall ? fSmall : fBody;
    const f32 textH = (fmt && vf) ? vf->height() : 0.f;
    const f32 avail = std::min(b.w, b.h - textH);
    const f32 rad = avail * 0.5f - 1.f;
    if (rad <= 1.f) return changed;
    const f32 cx = b.cx();
    const f32 cy = b.y + 1.f + rad;

    const f32 t = norm01(*v, lo, hi);
    const f32 ang = kKnobA0 + (kKnobA1 - kKnobA0) * t;

    // Body.
    r->circle(cx, cy, rad, pal::panelAlt);
    r->circle(cx, cy, rad - 1.f, pal::panelAlt.scale(0.78f));

    // Track + value arc, drawn just outside the body.
    const f32 aRad = rad - 1.5f;
    const f32 aTh = std::max(1.5f, rad * 0.18f);
    arc(cx, cy, aRad, kKnobA0, kKnobA1, aTh, pal::divider);

    const bool bipolar = (lo < 0.f && hi > 0.f);
    const Col arcCol = (hotNow || active == id) ? pal::accent : pal::accent.scale(0.85f);
    if (bipolar) {
        // Grow out of 12 o'clock in whichever direction the value sits.
        const f32 centre = kKnobA0 + (kKnobA1 - kKnobA0) * norm01(0.f, lo, hi);
        if (std::fabs(ang - centre) > 1e-3f) arc(cx, cy, aRad, centre, ang, aTh, arcCol);
        r->line(cx + std::cos(kKnobTop) * (aRad - aTh * 0.5f),
                cy + std::sin(kKnobTop) * (aRad - aTh * 0.5f),
                cx + std::cos(kKnobTop) * (aRad + aTh * 0.5f),
                cy + std::sin(kKnobTop) * (aRad + aTh * 0.5f), 1.f, pal::ridge);
    } else if (t > 0.001f) {
        arc(cx, cy, aRad, kKnobA0, ang, aTh, arcCol);
    }

    // Indicator from the middle outward.
    const f32 i0 = rad * 0.22f, i1 = rad - aTh - 1.f;
    if (i1 > i0) {
        r->line(cx + std::cos(ang) * i0, cy + std::sin(ang) * i0,
                cx + std::cos(ang) * i1, cy + std::sin(ang) * i1, 1.5f, pal::text);
    }

    if (fmt && vf) {
        char buf[64];
        std::snprintf(buf, sizeof buf, fmt, (double)*v);
        const Rect tr{b.x, b.bottom() - textH, b.w, textH};
        r->textIn(*vf, tr, buf, (hotNow || active == id) ? pal::text : pal::textDim,
                  Align::Center, 0.f);
    }

    if (hotNow || active == id) cursor = Cursor::ResizeV;
    return changed;
}

// ---------------------------------------------------------------------------
// The instrument knob
//
// knob() above and this are deliberately two widgets rather than one with more
// arguments. That one is the generic device-panel control: it takes whatever
// range a scanned plugin declares and drags it linearly, which is right when
// nothing is known about the parameter. This one is for a panel that KNOWS its
// parameters -- their curve, their centre, and whether the device in front of
// it even has them -- and it is skinned in the design language rather than in
// the pre-NX palette: the cap is a --glass-1 surface with the 1px lit edge,
// which is what makes a wall of forty knobs read as one material.
// ---------------------------------------------------------------------------

bool Ui::knobNx(u64 id, const Rect& b, f32* v, const KnobStyle& st) {
    if (!v || !r) return false;
    const f32 dpi = std::max(1.f, r->dpiScale());
    Font* vf = fSmall ? fSmall : fBody;
    const bool showText = (st.fmt || st.text) && vf;
    const f32 textH = showText ? vf->height() : 0.f;
    const f32 avail = std::min(b.w, b.h - textH);
    const f32 rad = avail * 0.5f - 2.f * dpi;
    if (rad <= 1.f) return false;
    const f32 cx = b.cx(), cy = b.y + 2.f * dpi + rad;

    const f32 lo = std::min(st.lo, st.hi), hi = std::max(st.lo, st.hi);
    bool changed = false, hotNow = false;

    // An absent parameter takes no input at all -- not "takes it and ignores
    // it": it never claims hot, so the pointer passes through to whatever is
    // under it and the cursor never changes over a socket with nothing in it.
    if (!st.absent) {
        const bool over = setHot(id, b);
        hotNow = isHot(id);
        if (in->pressed[0] && hotNow) {
            active = id;
            dragAccum = 0.f;
            dragStart = (f64)knobT(st, *v);
        }
        if (in->dblClick && over) {
            const f32 nv = clampv(st.def, lo, hi);
            if (nv != *v) { *v = nv; changed = true; }
            dragStart = (f64)knobT(st, *v);
            dragAccum = 0.f;
        }
        if (active == id && in->dy != 0.f) {
            dragAccum += -in->dy * fineScale(in);          // up is more
            f32 t = clampv((f32)dragStart + dragAccum / (kKnobTravel * dpi), 0.f, 1.f);
            // The detent CATCHES. A bipolar depth is a control whose most
            // useful value is exactly zero, and hitting zero on a 150px sweep
            // by hand is a coin toss; the drag accumulator keeps counting
            // underneath, so leaving the detent costs the same pixels it cost
            // to arrive at it.
            if (st.bipolar) {
                const f32 tc = knobT(st, 0.f);
                if (std::fabs(t - tc) < 0.02f) t = tc;
            }
            const f32 nv = knobV(st, t);
            if (nv != *v) { probeValue("knobNx", id, *v, nv); *v = nv; changed = true; }
        }
        if (in->released[0] && active == id) active = 0;
    }

    // --- draw --------------------------------------------------------------
    const f32 dim  = clampv(st.dim, 0.f, 1.f);
    const bool live = !st.absent && (hotNow || active == id);
    const f32 t   = knobT(st, clampv(*v, lo, hi));
    const f32 ang = kKnobA0 + (kKnobA1 - kKnobA0) * t;

    // The cap. A rounded rect at radius = half its width IS a circle, so the
    // knob gets --glass-1 and the gradient edge rather than the two flat discs
    // the old knob paints -- same lamp, upper-left, as every other surface.
    const Rect cap{cx - rad, cy - rad, rad * 2.f, rad * 2.f};
    r->gradRect(cap, rad, nx::glass1, (st.absent ? 0.30f : 0.95f) * dim);
    r->gradStroke(cap, rad, dpi, nx::edge, (st.absent ? 0.28f : 0.85f) * dim);

    const f32 aRad = rad + 2.5f * dpi;
    const f32 aTh  = std::max(1.5f * dpi, rad * 0.17f);
    arc(cx, cy, aRad, kKnobA0, kKnobA1, aTh, nx::muted.alpha(0.20f * dim));

    if (!st.absent) {
        const Col ac = st.arc.alpha((live ? 1.f : 0.82f) * dim);
        if (st.bipolar) {
            const f32 centre = kKnobA0 + (kKnobA1 - kKnobA0) * knobT(st, 0.f);
            if (std::fabs(ang - centre) > 1e-3f) arc(cx, cy, aRad, centre, ang, aTh, ac);
        } else if (t > 0.001f) {
            arc(cx, cy, aRad, kKnobA0, ang, aTh, ac);
        }
        // The indicator, from the middle of the cap outward.
        const f32 i0 = rad * 0.28f, i1 = rad * 0.88f;
        r->line(cx + std::cos(ang) * i0, cy + std::sin(ang) * i0,
                cx + std::cos(ang) * i1, cy + std::sin(ang) * i1,
                std::max(1.f, 1.5f * dpi), nx::text.alpha(dim));
    }
    // The detent, drawn for every bipolar control whether or not it is sitting
    // at it: it is the mark that says this knob HAS a middle. OUTSIDE the value
    // ring rather than across it -- at the centre the indicator points straight
    // at twelve o'clock, and a tick under the indicator is a tick nobody can
    // see at the one value it exists to mark.
    if (st.bipolar) {
        const f32 da = kKnobA0 + (kKnobA1 - kKnobA0) * knobT(st, 0.f);
        // Two pixels, and no more: this is the one mark in the widget that
        // reaches outside the rect it was given, and the caller's row gap is
        // what it reaches into.
        const f32 t0 = aRad + aTh * 0.5f + 0.5f * dpi, t1 = t0 + 2.f * dpi;
        r->line(cx + std::cos(da) * t0, cy + std::sin(da) * t0,
                cx + std::cos(da) * t1, cy + std::sin(da) * t1,
                std::max(1.f, dpi), nx::muted.alpha(0.85f * dim));
    }

    if (showText) {
        char buf[64];
        if (st.absent)        std::snprintf(buf, sizeof buf, "-");
        else if (st.text)     std::snprintf(buf, sizeof buf, "%s", st.text);
        else                  std::snprintf(buf, sizeof buf, st.fmt, (double)*v);
        const Rect tr{b.x, b.bottom() - textH, b.w, textH};
        const Col c = st.absent ? nx::muted.alpha(0.40f * dim)
                                : (live ? nx::text : nx::muted).alpha(dim);
        drawTextIn(*vf, tr, buf, c, Align::Center, 0.f);
    }

    if (live) cursor = Cursor::ResizeV;
    return changed;
}

// ---------------------------------------------------------------------------
// The value trough
// ---------------------------------------------------------------------------

bool Ui::trough(u64 id, const Rect& b, f32* v, f32 lo, f32 hi, const Col& fill, f32 dim) {
    if (!v || !r || b.w <= 2.f || b.h <= 1.f) return false;
    const f32 dpi = std::max(1.f, r->dpiScale());
    const f32 rad = std::min(nx::radiusXs * dpi, b.h * 0.5f);
    const f32 span = hi - lo;

    setHot(id, b);
    const bool hotNow = isHot(id);
    bool changed = false;
    const auto write = [&](f32 nv) {
        nv = clampv(nv, std::min(lo, hi), std::max(lo, hi));
        if (nv != *v) { probeValue("trough", id, *v, nv); *v = nv; changed = true; }
    };

    if (in->pressed[0] && hotNow) {
        active = id;
        dragAccum = 0.f;
        // A press JUMPS: a trough this short is a target, not a handle, and
        // hunting for a 1px head to grab would be the worse of the two.
        write(lo + span * clampv((in->mx - b.x) / b.w, 0.f, 1.f));
        dragStart = (f64)*v;
    }
    if (active == id && in->dx != 0.f) {
        dragAccum += in->dx * fineScale(in);
        write((f32)dragStart + span * (dragAccum / b.w));
    }
    if (in->released[0] && active == id) active = 0;

    const f32 t = std::fabs(span) > 1e-9f ? clampv((*v - lo) / span, 0.f, 1.f) : 0.f;
    dim = clampv(dim, 0.f, 1.f);

    r->well(b, rad, true);
    if (t > 0.001f) {
        const Rect f{b.x, b.y, std::max(rad * 2.f, b.w * t), b.h};
        r->gradRect(f, rad, liquidOf(fill), dim);
        // §1: the specular is a FUNCTION OF THE VALUE. It rides the fill as the
        // fill grows and holds still when the value does -- no timer anywhere
        // in this widget, which is the difference between light and an effect.
        r->sheen(f, rad, t, 0.55f * dim);
    }
    r->gradStroke(b, rad, dpi, nx::edge, 0.55f * dim);

    const f32 hx = clampv(std::round(b.x + b.w * t) - dpi, b.x, b.right() - dpi);
    r->rect({hx, b.y + dpi, dpi, b.h - 2.f * dpi},
            nx::text.alpha(((hotNow || active == id) ? 0.95f : 0.55f) * dim));

    if (hotNow || active == id) cursor = Cursor::ResizeH;
    return changed;
}

// ---------------------------------------------------------------------------
// Vertical fader
// ---------------------------------------------------------------------------

bool Ui::vFader(u64 id, const Rect& b, f32* t) {
    if (!t || !r) return false;
    const bool over = setHot(id, b);
    const bool hotNow = isHot(id);
    bool changed = false;

    const f32 handleH = 11.f;
    const f32 travel = std::max(1.f, b.h - handleH);
    // t = 1 at the top of the travel.
    auto handleY = [&](f32 tv) { return b.y + (1.f - clampv(tv, 0.f, 1.f)) * travel; };

    if (in->pressed[0] && hotNow) {
        active = id;
        dragAccum = 0.f;
        const Rect h{b.x, handleY(*t), b.w, handleH};
        if (!h.contains(in->mx, in->my)) {
            // Clicking the track jumps the handle under the cursor.
            const f32 nv = clampv(1.f - (in->my - b.y - handleH * 0.5f) / travel, 0.f, 1.f);
            if (nv != *t) { probeValue("vFader", id, *t, nv); *t = nv; changed = true; }
        }
        dragStart = (f64)*t;
    }
    if (in->dblClick && over) {
        *t = 0.85f;                       // unity
        dragStart = (f64)*t;
        dragAccum = 0.f;
        changed = true;
    }
    if (active == id && in->dy != 0.f) {
        dragAccum += -in->dy * fineScale(in);
        const f32 nv = clampv((f32)dragStart + dragAccum / travel, 0.f, 1.f);
        if (nv != *t) { probeValue("vFader", id, *t, nv); *t = nv; changed = true; }
    }
    if (in->released[0] && active == id) active = 0;

    // --- draw ---
    const f32 trackW = std::min(4.f, std::max(2.f, b.w * 0.22f));
    // A trough is a WELL, not a painted slot (§4): recessed, flat, cheap. The
    // 1px grey rule that used to sit down its left edge is gone -- §11.
    const Rect track{std::round(b.cx() - trackW * 0.5f), b.y + handleH * 0.5f,
                     trackW, b.h - handleH};
    r->well(track, trackW * 0.5f, true);

    // Unity tick, as a hairline that fades at both ends.
    const f32 unityY = std::round(handleY(0.85f) + handleH * 0.5f);
    r->hairlineH(b.x, b.right(), unityY, nx::hairlineInk, 1.f);

    const Rect handle{b.x, std::round(handleY(*t)), b.w, handleH};
    Col hc = pal::ridge;
    if (active == id) hc = pal::ridge.scale(1.25f);
    else if (hotNow) hc = pal::ridge.scale(1.12f);
    bevel(handle, 2.f, hc, 0.18f);
    // The grip line across the middle of the cap.
    r->hairlineH(handle.x + 1.f, handle.right() - 1.f, std::round(handle.cy()),
                 rgba(0x000000, 0.55f), 1.f);

    if (hotNow || active == id) cursor = Cursor::ResizeV;
    return changed;
}

// ---------------------------------------------------------------------------
// Draggable number
// ---------------------------------------------------------------------------

bool Ui::dragNumber(u64 id, const Rect& b, f64* v, f64 lo, f64 hi, f64 perPixel,
                    const char* fmt, Align align, const char* zeroLabel, f64 step) {
    if (!v || !r) return false;
    setHot(id, b);
    const bool hotNow = isHot(id);
    bool changed = false;

    if (in->pressed[0] && hotNow) {
        active = id;
        dragAccum = 0.f;
        dragStart = *v;
    }
    if (active == id && in->dy != 0.f) {
        dragAccum += -in->dy * (in->shift() ? kFineNumber : 1.f);   // drag up = increase
        f64 nv = dragStart + (f64)dragAccum * perPixel;
        // Snap before clamping, so the endpoints of the range stay reachable
        // even when they are not multiples of the step.
        if (step > 0.0) nv = std::floor(nv / step + 0.5) * step;
        nv = clampv(nv, lo, hi);
        if (nv != *v) { probeValue("dragNumber", id, *v, nv); *v = nv; changed = true; }
    }
    if (in->released[0] && active == id) active = 0;

    // A scrubbable number is a FIELD, so it recesses rather than lighting up:
    // the glass arrives with the pointer (Pill::Ghost), and while the drag owns
    // it the box drops into a well with a violet hairline under the number --
    // the same "this is being edited" language textField uses, at the weight a
    // control that has no caret can carry.
    const bool live = hotNow || active == id;
    const UiMotion m = motion(id, hotNow, active == id);
    const f32 dpi = std::max(1.f, r->dpiScale());
    const f32 rad = std::min(nx::radiusXs * dpi, b.h * 0.5f);
    if (active == id) {
        r->well(b, rad, true);
        r->hairlineH(b.x + rad, b.right() - rad, b.bottom() - dpi,
                     nx::violet.alpha(0.75f), dpi);
    } else {
        pillRect(b, rad, Pill::Ghost, pal::accent, m);
    }

    Font* f = fBody ? fBody : fSmall;
    if (f) {
        char buf[80];
        if (zeroLabel && std::fabs(*v) < 1e-9) std::snprintf(buf, sizeof buf, "%s", zeroLabel);
        else                                   std::snprintf(buf, sizeof buf, fmt ? fmt : "%.2f", *v);
        drawTextIn(*f, b, buf, live ? nx::text : pal::textDim, align, 3.f);
    }

    if (live) cursor = Cursor::ResizeV;
    return changed;
}

// ---------------------------------------------------------------------------
// Selector
// ---------------------------------------------------------------------------

bool Ui::selector(u64 id, const Rect& b, int* idx, const char* const* options, int count) {
    if (!idx || !options || count <= 0 || !r) return false;
    const bool over = setHot(id, b);
    const bool hotNow = isHot(id);
    bool changed = false;

    auto step = [&](int d) {
        const int n = ((*idx + d) % count + count) % count;
        if (n != *idx) { *idx = n; changed = true; }
    };

    if (in->pressed[0] && hotNow) active = id;
    if (in->released[0] && active == id) {
        if (over) step(+1);
        active = 0;
    }
    if (in->pressed[2] && hotNow) step(-1);
    if (hotNow && in->wheel != 0.f) step(in->wheel > 0.f ? +1 : -1);

    const bool held = (active == id) && over;
    const UiMotion m = motion(id, hotNow, held);
    const Rect br = liftPress(b, m);
    pillRect(br, br.h * 0.5f, Pill::Secondary, pal::accent, m);

    *idx = clampv(*idx, 0, count - 1);
    const char* label = options[*idx] ? options[*idx] : "";
    Font* f = fBody ? fBody : fSmall;
    if (f) drawTextIn(*f, br, label, pal::textDim.mix(nx::text, 0.4f + 0.6f * m.hover),
                      Align::Center, 3.f);

    if (hotNow) cursor = Cursor::Hand;
    return changed;
}

// ---------------------------------------------------------------------------
// Text field
// ---------------------------------------------------------------------------

bool Ui::textField(u64 id, const Rect& b, std::string* value, Col bg, Col fg,
                   Align align, bool activateOnDouble) {
    if (!value || !r) return false;
    static int blink = 0;

    const bool over = setHot(id, b);
    const bool hotNow = isHot(id);
    const bool editing = (editId == id);
    bool committed = false;

    auto beginEdit = [&]() {
        editId = id;
        editBuf = *value;
        caret = (int)editBuf.size();
        active = id;
        blink = 0;
        editCommitted = false;
    };
    auto commit = [&]() {
        *value = editBuf;
        editId = 0;
        if (active == id) active = 0;
        editCommitted = true;
        committed = true;
    };
    auto cancel = [&]() {
        editId = 0;
        if (active == id) active = 0;
        editCommitted = false;
    };

    if (!editing) {
        if (activateOnDouble) {
            if (in->dblClick && over) beginEdit();
        } else if (in->pressed[0] && hotNow) {
            beginEdit();
        }
    } else {
        // A press anywhere else takes the value as typed, matching Live.
        if (in->pressed[0] && !over) commit();
    }

    if (editId == id) {
        ++blink;
        caret = clampv(caret, 0, (int)editBuf.size());

        if (!in->textInput.empty()) {
            std::string filtered;
            filtered.reserve(in->textInput.size());
            for (char c : in->textInput)
                if ((unsigned char)c >= 0x20 && c != 0x7F) filtered.push_back(c);
            if (!filtered.empty()) {
                editBuf.insert((size_t)caret, filtered);
                caret += (int)filtered.size();
                blink = 0;
            }
        }
        if (in->keyPressed[KeyBackspace] && caret > 0) {
            editBuf.erase((size_t)(caret - 1), 1);
            --caret;
            blink = 0;
        }
        if (in->keyPressed[KeyDelete] && caret < (int)editBuf.size()) {
            editBuf.erase((size_t)caret, 1);
            blink = 0;
        }
        if (in->keyPressed[KeyLeft])  { caret = clampv(caret - 1, 0, (int)editBuf.size()); blink = 0; }
        if (in->keyPressed[KeyRight]) { caret = clampv(caret + 1, 0, (int)editBuf.size()); blink = 0; }
        if (in->keyPressed[KeyHome])  { caret = 0; blink = 0; }
        if (in->keyPressed[KeyEnd])   { caret = (int)editBuf.size(); blink = 0; }
        if (in->keyPressed[KeyEscape]) cancel();
        else if (in->keyPressed[KeyEnter]) commit();
    }

    // --- draw ---
    //
    // §5's input: a recessed well, a --line border that runs to violet on
    // focus, and the two-ring focus halo -- never a bare outline. The caller's
    // `bg` is still honoured at rest, because a clip name field wears its clip's
    // colour and that is information, not decoration; the well takes over the
    // moment the caret does.
    const bool nowEditing = (editId == id);
    const UiMotion m = motion(id, hotNow, nowEditing);
    const f32 dpi = std::max(1.f, r->dpiScale());
    const f32 rad = std::min(nx::radiusXs * dpi, b.h * 0.5f);
    if (nowEditing) {
        fieldWell(b, m.press);
    } else {
        if (bg.a > 0.f) r->roundRect(b, rad, bg);
        pillRect(b, rad, Pill::Ghost, pal::accent, m);
    }

    Font* f = fBody ? fBody : fSmall;
    if (f) {
        const char* s = nowEditing ? editBuf.c_str() : value->c_str();
        const Rect inner = b.insetXY(3.f, 0.f);
        if (!nowEditing) {
            r->textIn(*f, b, s, fg, align, 3.f);
        } else {
            r->pushClip(b);
            const f32 tw = f->measure(s);
            f32 tx = inner.x;
            if (align == Align::Center)     tx = b.x + (b.w - tw) * 0.5f;
            else if (align == Align::Right) tx = inner.right() - tw;
            // Keep the caret on screen when the string overflows the box.
            const f32 caretRel = f->measure(s, caret);
            if (tx + caretRel > inner.right()) tx = inner.right() - caretRel;
            if (tx + caretRel < inner.x)       tx = inner.x - caretRel;
            const f32 ty = b.y + (b.h - f->height()) * 0.5f;
            r->text(*f, tx, ty, s, fg);
            if (((blink / 30) & 1) == 0) {
                r->rect({std::round(tx + caretRel), std::round(ty + 1.f), 1.f,
                         std::max(2.f, f->height() - 2.f)}, pal::accent);
            }
            r->popClip();
        }
    }

    if (hotNow) cursor = Cursor::Text;
    return committed;
}

} // namespace lat
