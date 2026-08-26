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

// FINE IS CTRL **OR** SHIFT, and the OR is the whole decision.
//
// Ctrl is FL Studio's spelling and the owner's muscle memory; Shift is what
// this program has shipped since the widgets were written, and is Live's. They
// cost one boolean to support together, neither collides with the other on a
// control (a widget reads no other meaning off either modifier), and dropping
// Shift would break a habit for no gain beyond tidiness.
inline bool fineMod(const Input* in) { return in->shift() || in->ctrl(); }
inline f32 fineScale(const Input* in) { return fineMod(in) ? kFineSweep : 1.f; }

// ONE WHEEL NOTCH IS FIVE LOGICAL PIXELS OF DRAG.
//
// Measured in the drag's own units rather than picked as a fraction, so the
// wheel and the hand are the same gesture at two resolutions: five pixels is
// about the smallest deliberate mouse movement, and against kKnobTravel it
// makes a notch 1/30th of a knob's range -- thirty notches end to end, which
// is a flick, and a quarter of that under the fine modifier.
constexpr f32 kWheelPx = 5.f;

// The notch under the pointer, CONSUMED.
//
// Consumption is the point (FL: the control under the cursor gets the wheel and
// the surface beneath does not also scroll), and it happens here rather than at
// thirty call sites so that no widget can forget. A widget that is not hot
// never sees the notch, so exactly one control can spend it.
inline f32 wheelNotch(Input* in, bool hotNow) {
    if (!hotNow || in->wheel == 0.f) return 0.f;
    const f32 w = in->wheel;
    in->wheel = 0.f;
    return w;
}

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
// The same instrument for the things that are not a number changing: a menu
// opening, an item firing, a gesture refused. Without these a right-click that
// did nothing and a right-click whose menu opened off-screen are the same
// picture, and the pass that cannot tell them apart is the pass that ships the
// second one.
void probeSay(const char* what, u64 id, const char* detail) {
    if (!probeOn()) return;
    LOGI("NXTAKT_DEBUG_PROBE: %s id=%016llx %s", what,
         (unsigned long long)id, detail ? detail : "");
}

} // namespace

// ---------------------------------------------------------------------------
// §5 / §6  The NX vocabulary
// ---------------------------------------------------------------------------

// See the rules over the declaration. 16 LOGICAL pixels scaled by the seat's
// DPI, not 16 device pixels: the floor is a physical target size, and a control
// that is comfortable at 1x must not stop being comfortable at 1.5x because the
// number was spelled in the wrong unit.
f32 Ui::slopFor(const Rect& b) const {
    const f32 dpi = r ? std::max(1.f, r->dpiScale()) : 1.f;
    return clampv((16.f * dpi - std::min(b.w, b.h)) * 0.5f, 0.f, 3.f * dpi);
}

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
    //
    // THE SLOT AS DRAWN IS INSIDE THE PAD; THE SLOT AS AIMED AT IS NOT.
    // Filed by the instrument-editor pass, measured through this file's own
    // HOT line: an 88x16 tab bar was handing back 42x12 slots, because `pad`
    // was taken off before the track was cut. `pad` exists so the sliding
    // indicator does not touch the container's edge -- it is a DRAWING inset
    // and never was an aiming one, and charging it to the hit test cost
    // app_spectra.cpp four logical pixels of title band (16 -> 20), two of
    // which came off its knob rows, to get its page tab over the 16 px floor.
    // A tab's target is the whole tab. This changes no drawn pixel.
    //
    // AND THE SLOP IS RE-ARMED PER SLOT, which the filed diff did not cover
    // and is the nastier half of the same bug: setHot() consumes hitPad, so
    // `ui.grab(3).tabPill(...)` padded slot 0 and left every other slot short.
    // Two halves of one control with different targets is worse than both
    // being small, because then the miss depends on which tab you aimed at.
    const f32 pillSlop = hitPad;
    hitPad = 0.f;
    const f32 hitW = b.w / (f32)count;
    int hotSlot = -1;
    for (int i = 0; i < count; ++i) {
        const Rect s{b.x + hitW * (f32)i, b.y, hitW, b.h};
        const u64 sid = id ^ ((u64)(i + 1) * 0x9E3779B97F4A7C15ull);
        if (pillSlop > 0.f) hitPad = pillSlop;
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
    case Badge::Assign: {
        // An arrow arriving at a ring. The ring is the control under the
        // pointer -- a knob, drawn the way the panel draws one -- and the
        // arrow is the thing in your hand landing on it. Deliberately NOT a
        // plus: nothing is created by the drop, the two ends are already
        // there and the gesture only wires them together.
        //
        // ALONG THE DIAGONAL, for the same reason Draw's pen is: the glyph box
        // is about 7 device px square at 1x, and a ring wide enough to read as
        // a ring leaves a horizontal arrow no run at all. The diagonal is half
        // again as long, and it is the direction a drag arrives from anyway.
        const f32 rc = std::round(g.w * 0.30f);        // the ring, bottom-right
        const f32 kx = g.right() - rc, ky = g.bottom() - rc;
        rr.roundRectOutline({kx - rc, ky - rc, rc * 2.f, rc * 2.f}, rc, one, ink);

        const f32 vx = kx - g.x, vy = ky - g.y;
        const f32 len = std::sqrt(vx * vx + vy * vy);
        if (len > 1e-3f) {
            const f32 dx = vx / len, dy = vy / len;    // toward the ring
            // The head is sized off the box and not off `one`: `one` is flat
            // between 1x and 1.25x while the ring is not, and a head measured
            // in `one` eats the whole shaft on the seat where it does not grow.
            const f32 hl = g.w * 0.26f, hw = g.w * 0.20f;
            // The head stops ON the ring's edge -- arriving at the control,
            // which is what the drop does, rather than passing through it.
            const f32 tx = kx - dx * (rc + one * 0.5f);
            const f32 ty = ky - dy * (rc + one * 0.5f);
            const f32 bx = tx - dx * hl, by = ty - dy * hl;
            rr.line(g.x, g.y, bx, by, one * 1.3f, ink);
            rr.triangle(tx, ty, bx - dy * hw, by + dx * hw,
                        bx + dy * hw, by - dx * hw, ink);
        }
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

// ---------------------------------------------------------------------------
// THE CONTROL MENU
//
// The whole of FL's right-click-a-knob, in one place, for every continuous
// control in the program. Three problems had to be solved together:
//
//  1. AN IMMEDIATE-MODE MENU HAS NO Z-ORDER. It must take input before
//     anything else in the frame and draw after everything else, and those are
//     opposite ends of the same frame. So it is split: menuBegin() resolves the
//     click from the geometry it fixed when it opened (nothing about the sheet
//     moves while it is up, so there is no lag), and drawMenu() paints.
//
//  2. EVERYTHING ELSE MUST BE UNREACHABLE. setHot() refuses every id while the
//     shield is up, which stops every widget in the program dead. The handful
//     of surfaces that hit-test with a bare hovered() instead are handled by
//     PARKING THE POINTER off-screen for the body of the frame -- the same
//     idiom app_spectra.cpp's preset popover uses, and for the same reason.
//     endFrame() puts it back before the cursor and the badge are read.
//
//  3. IT MUST NOT DOUBLE-FIRE WITH A CALLER'S OWN RIGHT-CLICK. A widget that
//     opens a menu CONSUMES the right press, so a hand-rolled
//     `hovered(r) && in.pressed[2]` beside a knob stops firing rather than
//     firing under an open menu. Those sites move onto MenuCustom / MenuLearn;
//     the diffs are filed.
// ---------------------------------------------------------------------------

void Ui::menuOpen(u64 id, f64 def, const char* noDefWhy) {
    if (!r || !in) return;
    const f32 dpi = std::max(1.f, r->dpiScale());
    Menu m;
    m.open = true;
    m.ctl = id;
    m.cursor = -1;

    const auto add = [&](u32 item, const char* label, bool live, const char* why) {
        if (m.n >= kMenuRows) return;
        m.item[m.n] = item;
        m.live[m.n] = live;
        std::snprintf(m.label[m.n], sizeof m.label[m.n], "%s", label ? label : "?");
        std::snprintf(m.why[m.n], sizeof m.why[m.n], "%s", why ? why : "");
        ++m.n;
    };

    // The two the widget layer owns, always, and always first: they are the
    // ones a user can rely on being in this menu whatever it is hanging off.
    add(MenuReset, "Reset", std::isfinite(def),
        noDefWhy ? noDefWhy : "This control has no default to reset to");
    add(MenuTypeIn, "Type in value", true, nullptr);
    // Then the caller's, in a fixed order so the menu never reshuffles between
    // two knobs that happen to offer different subsets.
    if (lastOffer.items & MenuAssign) add(MenuAssign, lastOffer.assign, true, nullptr);
    if (lastOffer.items & MenuLearn)  add(MenuLearn,  lastOffer.learn,  true, nullptr);
    if ((lastOffer.items & MenuCustom) && lastOffer.custom)
        add(MenuCustom, lastOffer.custom, true, nullptr);

    // Geometry, fixed now and never recomputed: menuBegin hit-tests against
    // exactly the rect drawMenu paints, which is what makes a one-call-apart
    // split exact instead of one frame stale.
    Font* f = fSmall ? fSmall : fBody;
    m.rowH = std::round(18.f * dpi);          // >= 16 device px at every scale
    f32 w = 96.f * dpi;
    if (f) for (int i = 0; i < m.n; ++i)
        w = std::max(w, f->measure(m.label[i]) + 26.f * dpi);
    const f32 padY = std::round(4.f * dpi);
    const f32 h = padY * 2.f + m.rowH * (f32)m.n;
    // South-east of the pointer, like the badge, then clamped into the WINDOW:
    // a menu opened on the last knob in the bottom-right corner is exactly the
    // one that would otherwise be off screen.
    //
    // The window, and pointedly not r->currentClip(). A menu is a tier-2 sheet
    // and §4 entitles those to overlap content; the clip in force when a knob
    // is drawn is that knob's PANEL, so clamping to it folded the menu back
    // inside a 150 px device card -- which the first driven run of this caught,
    // with the sheet landing ABOVE AND LEFT of the pointer that opened it.
    // drawMenu paints at the end of the frame with no clip at all, so the clip
    // here was not even the region the thing would be drawn in.
    const Rect vp{0.f, 0.f, (f32)r->width(), (f32)r->height()};
    f32 x = in->mx + 6.f * dpi, y = in->my + 6.f * dpi;
    x = clampv(x, vp.x, std::max(vp.x, vp.right() - w));
    y = clampv(y, vp.y, std::max(vp.y, vp.bottom() - h));
    m.box = {std::round(x), std::round(y), std::round(w), std::round(h)};

    menu = m;
    {
        char d[96];
        std::snprintf(d, sizeof d, "menu open rows=%d box=%.0f,%.0f,%.0fx%.0f",
                      m.n, m.box.x, m.box.y, m.box.w, m.box.h);
        probeSay("menu", id, d);
    }
    // The press is spent. Both halves: a caller testing down[2] on the next
    // frame must not see it either.
    in->pressed[2] = false;
    in->down[2] = false;
}

void Ui::menuBegin() {
    menuCtl_ = 0;
    menuItem_ = 0;
    pendingOffer = MenuOffer{};
    lastOffer = MenuOffer{};
    menuShield = false;
    if (!menu.open || !in || !r) return;

    const f32 padY = std::round(4.f * std::max(1.f, r->dpiScale()));
    const auto rowAt = [&](int i) {
        return Rect{menu.box.x, menu.box.y + padY + menu.rowH * (f32)i,
                    menu.box.w, menu.rowH};
    };
    const auto seek = [&](int from, int dir) {
        for (int i = from + dir; i >= 0 && i < menu.n; i += dir)
            if (menu.live[i]) return i;
        return from;
    };
    const auto fire = [&](int i) {
        if (i < 0 || i >= menu.n) return;
        if (!menu.live[i]) { refusal = menu.why[i]; menu.open = false; return; }
        menuCtl_ = menu.ctl;
        menuItem_ = menu.item[i];
        menu.open = false;
        probeSay("menu pick", menu.ctl, menu.label[i]);
    };

    // KEYS FIRST, and consumed: an open menu owns Escape, the arrows and Enter,
    // or Escape would also stop every clip in the set on its way past.
    if (in->keyPressed[KeyEscape]) { menu.open = false; in->keyPressed[KeyEscape] = false; }
    if (menu.open) {
        if (in->keyPressed[KeyDown]) {
            menu.cursor = menu.cursor < 0 ? seek(-1, +1) : seek(menu.cursor, +1);
            in->keyPressed[KeyDown] = false;
        }
        if (in->keyPressed[KeyUp]) {
            menu.cursor = menu.cursor < 0 ? seek(menu.n, -1) : seek(menu.cursor, -1);
            in->keyPressed[KeyUp] = false;
        }
        if (in->keyPressed[KeyEnter]) {
            const int c = menu.cursor;
            in->keyPressed[KeyEnter] = false;
            fire(c);
        }
    }
    // Then the pointer. A press ANYWHERE that is not a row closes it, which is
    // what every menu on every desktop does; both buttons, because a
    // right-click outside is still "not this menu".
    if (menu.open && (in->pressed[0] || in->pressed[2])) {
        int picked = -1;
        for (int i = 0; i < menu.n; ++i)
            if (rowAt(i).contains(in->mx, in->my)) { picked = i; break; }
        in->pressed[0] = in->pressed[2] = false;
        in->down[0] = in->down[2] = false;
        if (picked >= 0) fire(picked);
        else             menu.open = false;
    }

    if (!menu.open) { menu.n = 0; return; }
    // Still up: shield the frame. The pointer goes somewhere no rect contains,
    // and endFrame() brings it back.
    menuParkX = in->mx;
    menuParkY = in->my;
    in->mx = in->my = -1.0e4f;
    menuShield = true;
}

void Ui::menuEnd() {
    // The frame's refusal, logged where every widget has already had its say.
    // A refusal is the half of a gesture a screenshot cannot prove -- the value
    // did not change either way -- so this is what makes "it explained itself"
    // assertable rather than eyeballed.
    if (!refusal.empty()) probeSay("refused", 0, refusal.c_str());
    if (!menuShield || !in) return;
    in->mx = menuParkX;
    in->my = menuParkY;
    // The shield stays LOGICALLY up for the rest of the frame -- drawMenu still
    // has to know it is drawing -- but no setHot() runs after endFrame(), so
    // clearing the flag here would only make the pointer restore conditional on
    // nothing. It is cleared at the top of the next menuBegin().
    if (menu.open) {
        // A live row under the pointer is a click target; say so.
        const f32 padY = std::round(4.f * std::max(1.f, r ? r->dpiScale() : 1.f));
        for (int i = 0; i < menu.n; ++i) {
            const Rect row{menu.box.x, menu.box.y + padY + menu.rowH * (f32)i,
                           menu.box.w, menu.rowH};
            if (row.contains(in->mx, in->my) && menu.live[i]) cursor = Cursor::Hand;
        }
    }
}

void Ui::drawMenu(Renderer& rr, Font& f) {
    if (!menu.open) return;
    const f32 dpi = std::max(1.f, rr.dpiScale());
    const f32 rad = nx::radiusSm * dpi;
    const f32 padY = std::round(4.f * dpi);

    // §4's menu material, exactly as the preset popover mixes it: legibility
    // rides an opaque-ish fill under the glass, then the lit edge and the sheet
    // shadow. One menu material in the program, or it is two menus.
    rr.shadow(menu.box, rad, nx::shadowSheet);
    rr.roundRect(menu.box, rad, nx::panel2.alpha(0.97f));
    rr.gradRect(menu.box, rad, nx::glass2);
    rr.gradStroke(menu.box, rad, dpi, nx::edgeLit, 1.f);

    for (int i = 0; i < menu.n; ++i) {
        const Rect row{menu.box.x, menu.box.y + padY + menu.rowH * (f32)i,
                       menu.box.w, menu.rowH};
        const bool over = menu.live[i] &&
                          (row.contains(in->mx, in->my) || menu.cursor == i);
        if (over)
            rr.gradRect(row.insetXY(3.f * dpi, 0.5f * dpi), nx::radiusXs * dpi,
                        nx::glassChip);
        // A refused row is DRAWN, dimmed, and still clickable -- clicking it is
        // how you find out why. A row that was simply absent would leave the
        // user looking for a Reset that this control does not have.
        const Col ink = !menu.live[i] ? nx::muted.alpha(0.40f)
                                      : (over ? nx::text : nx::muted.alpha(0.95f));
        rr.textIn(f, {row.x + 10.f * dpi, row.y, row.w - 14.f * dpi, row.h},
                  menu.label[i], ink, Align::Left, 0.f);
        // The hairline under the pair the widget layer owns, separating them
        // from whatever the caller added: two groups, one plate.
        if (i == 1 && menu.n > 2)
            rr.hairlineH(row.x + 8.f * dpi, row.right() - 8.f * dpi,
                         std::round(row.bottom()), nx::hairlineInk, dpi);
    }
}

bool Ui::wantReset(u64 id, f64 def, const char* why) {
    if (!menuFired(id, MenuReset)) return false;
    if (std::isfinite(def)) return true;
    refusal = why ? why : "This control has no default to reset to";
    return false;
}

void Ui::beginTypeIn(u64 id, f64 value) {
    char buf[48];
    // %.6g, not the control's own format: a readout may say "1.2 kHz" or round
    // to two places, and a field seeded with a rounded number writes the
    // rounding back the moment you press Enter without touching it.
    std::snprintf(buf, sizeof buf, "%.6g", value);
    typeInId = id;
    typeInBuf = buf;
    typeInFresh = true;                   // opens "selected": see Ui::typeInFresh
    editId = id ^ 0x7479706549u;          // 'typeI' -- UiCtlTypeIn's live id
    editBuf = typeInBuf;
    caret = (int)editBuf.size();
    active = editId;
    editCommitted = false;
}

int Ui::typeInRun(u64 id, const Rect& b, f64 lo, f64 hi, f64* out) {
    if (typeInId != id || !r) return 0;
    const u64 fid = id ^ 0x7479706549u;
    // THE PRE-SELECTION, resolved before textField sees the keystroke. Without
    // it "145" typed into a field showing 120.00 commits 120.00145 -- which is
    // what the first driven run of this actually did.
    if (typeInFresh) {
        if (!in->textInput.empty()) {
            editBuf.clear();
            caret = 0;
            typeInFresh = false;
        } else if (in->keyPressed[KeyBackspace] || in->keyPressed[KeyDelete]) {
            editBuf.clear();
            caret = 0;
            typeInFresh = false;
            in->keyPressed[KeyBackspace] = in->keyPressed[KeyDelete] = false;
        } else if (in->keyPressed[KeyLeft] || in->keyPressed[KeyRight] ||
                   in->keyPressed[KeyHome] || in->keyPressed[KeyEnd]) {
            typeInFresh = false;          // the caret moved: edit in place instead
        }
    }
    // The field is drawn OVER the control, in the well language every other
    // editable number in the program wears. `bg` is transparent because
    // textField draws fieldWell under the caret anyway. Violet ink while the
    // value is still "selected", so the replace-on-type is visible.
    const bool committed = textField(fid, b, &typeInBuf, Col(0.f, 0.f, 0.f, 0.f),
                                     typeInFresh ? nx::violetSoft : nx::text,
                                     Align::Center, /*activateOnDouble=*/false);
    if (committed) {
        typeInId = 0;
        const char* s = typeInBuf.c_str();
        while (*s == ' ') ++s;
        char* end = nullptr;
        const double d = std::strtod(s, &end);
        if (end == s) {
            refusal = "\"" + typeInBuf + "\" is not a number";
            return -1;
        }
        while (*end == ' ') ++end;
        if (*end) {
            // Refused rather than silently taking the leading number: "1.5 kHz"
            // parsed as 1.5 is a control set to the wrong value with no sign
            // that anything went wrong, which is worse than being told no.
            refusal = "\"" + typeInBuf + "\": type the number on its own";
            return -1;
        }
        *out = clampv(d, std::min(lo, hi), std::max(lo, hi));
        return 1;
    }
    // Escape, or a click that landed elsewhere and cancelled the edit.
    if (editId != fid) { typeInId = 0; return -1; }
    return 0;
}

// NXTAKT_DEBUG_HOT. See the note over Ui::hotRect: this is the measurement
// instrument tools/drive-lib.sh's scanx() has always asked for, and the only
// one that can answer "how wide is this drag edge, counting hit slop" -- the
// question the last pass got wrong twice by reading the source.
void Ui::probeHot() const {
    static const bool on = std::getenv("NXTAKT_DEBUG_HOT") != nullptr;
    if (!on) return;
    static u64 last = ~0ull;
    static std::string lastTip = "\x01";
    if (hotNext == last && tip == lastTip) return;
    last = hotNext;
    lastTip = tip;
    if (!hotNext) { LOGI("HOT ui id=0 rect=- tip=-"); return; }
    LOGI("HOT ui id=%016llx rect=%.1f,%.1f,%.1fx%.1f slop=%.1f tip=%s",
         (unsigned long long)hotNext, hotRect.x, hotRect.y, hotRect.w, hotRect.h,
         hotSlop, tip.empty() ? "-" : tip.c_str());
}

void Ui::meterV(const Rect& b, f32 lvl, f32 peak) {
    if (!r || b.w <= 0.f || b.h <= 0.f) return;
    // The trough is a WellDeep, by name: §4's tier table assigns that tier to
    // "waveform troughs, meters", and a flat appBg rect was reading as a hole
    // in the strip rather than as a recessed instrument slot. Radius 0 -- this
    // is a working surface, not a chip.
    r->well(b, 0.f, true);

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
                if (i == 0) {
                    // The decay gradient, anchored to the SCALE rather than to
                    // the bar: luminance is a function of trough position, so
                    // the visible top edge brightens as the level rises --
                    // §1's light-rides-motion, with the level itself as the
                    // driver. The hue ladder is untouched (see color.h: a
                    // meter is a measurement instrument, not a branding
                    // surface); only the green body gets depth. Amber and red
                    // stay flat -- a signal colour must be unambiguous.
                    auto lum = [](f32 p) { return 0.62f + 0.38f * (p / 0.75f); };
                    r->vgrad({b.x, y0, b.w, y1 - y0},
                             cols[0].scale(lum(to)), cols[0].scale(lum(from)));
                } else {
                    r->rect({b.x, y0, b.w, y1 - y0}, cols[i]);
                }
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
    const bool typing = typingIn(id);
    const bool over = setHot(id, b);
    const bool hotNow = isHot(id);
    bool changed = false;

    const auto write = [&](f32 nv) {
        nv = clampv(nv, std::min(lo, hi), std::max(lo, hi));
        if (nv != *v) { probeValue("knob", id, *v, nv); *v = nv; changed = true; }
    };

    if (!typing) {
        if (in->pressed[0] && hotNow) {
            active = id;
            dragAccum = 0.f;
            dragStart = (f64)*v;
        }
        // FL's right-click: Reset / Type in, plus whatever the caller offered.
        // It consumes the press, so a caller's own right-click handler beside
        // this knob stops firing rather than firing under an open menu.
        if (in->pressed[2] && hotNow) menuOpen(id, (f64)def, nullptr);
        if (wantReset(id, (f64)def, nullptr)) { write(def); dragStart = (f64)*v; dragAccum = 0.f; }
        if (menuFired(id, MenuTypeIn)) beginTypeIn(id, (f64)*v);
        if (in->dblClick && over) {
            write(def);
            dragStart = (f64)*v;
            dragAccum = 0.f;
        }
        // The wheel, in the drag's own units: a notch is kWheelPx of travel.
        if (const f32 w = wheelNotch(in, hotNow)) {
            write(*v + (hi - lo) * (kWheelPx / kKnobTravel) * fineScale(in) * w);
            dragStart = (f64)*v;
            dragAccum = 0.f;
        }
        if (active == id && in->dy != 0.f) {
            // Up is more. Accumulate in pixels so a fine-drag modifier can be
            // toggled mid-gesture without the value jumping.
            dragAccum += -in->dy * fineScale(in);
            const f32 travel = kKnobTravel * std::max(1.f, r->dpiScale());
            write((f32)dragStart + (dragAccum / travel) * (hi - lo));
        }
        if (in->released[0] && active == id) active = 0;
    }

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

    if (fmt && vf && !typing) {          // the field draws the number now
        char buf[64];
        std::snprintf(buf, sizeof buf, fmt, (double)*v);
        const Rect tr{b.x, b.bottom() - textH, b.w, textH};
        r->textIn(*vf, tr, buf, (hotNow || active == id) ? pal::text : pal::textDim,
                  Align::Center, 0.f);
    }

    // The type-in goes down LAST, over the control it is editing.
    if (typing) {
        f64 tv = 0.0;
        if (typeInRun(id, b, (f64)lo, (f64)hi, &tv) > 0) write((f32)tv);
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
    const bool typing = typingIn(id);

    const auto write = [&](f32 nv) {
        nv = clampv(nv, lo, hi);
        if (nv != *v) { probeValue("knobNx", id, *v, nv); *v = nv; changed = true; }
    };

    // An absent parameter takes no input at all -- not "takes it and ignores
    // it": it never claims hot, so the pointer passes through to whatever is
    // under it and the cursor never changes over a socket with nothing in it.
    // It gets no menu either, for the same reason: there is nothing to reset,
    // type into, assign or learn.
    // ...and an absent knob must still SPEND a pending offer, even though it
    // claims no rect. setHot() is what consumes it, so skipping setHot() left
    // the offer in front of this control armed for whatever drew NEXT — a
    // live "Assign to matrix slot" row appearing on an unrelated knob. Harmless
    // while callers derived their offer from has(id) (an absent id gave an
    // empty offer), and not harmless the moment a RESERVED id made has() answer
    // true about a parameter that means nothing. "Applies to the next control
    // and is then cleared" is now true in the absent case too.
    if (st.absent) pendingOffer = MenuOffer{};
    if (!st.absent && !typing) {
        const bool over = setHot(id, b);
        hotNow = isHot(id);
        if (in->pressed[0] && hotNow) {
            active = id;
            dragAccum = 0.f;
            dragStart = (f64)knobT(st, *v);
        }
        if (in->pressed[2] && hotNow) menuOpen(id, (f64)st.def, nullptr);
        if (wantReset(id, (f64)st.def, nullptr)) {
            write(st.def);
            dragStart = (f64)knobT(st, *v);
            dragAccum = 0.f;
        }
        if (menuFired(id, MenuTypeIn)) beginTypeIn(id, (f64)*v);
        if (in->dblClick && over) {
            write(st.def);
            dragStart = (f64)knobT(st, *v);
            dragAccum = 0.f;
        }
        // The wheel walks the SAME 0..1 travel the drag does, so a notch on a
        // logarithmic cutoff is the same fraction of an octave wherever it is
        // spent -- which a step in hertz would not be.
        if (const f32 w = wheelNotch(in, hotNow)) {
            const f32 t0 = knobT(st, *v);
            f32 t = clampv(t0 + (kWheelPx / kKnobTravel) * fineScale(in) * w, 0.f, 1.f);
            // The detent catches by CROSSING rather than by proximity: a notch
            // that steps over the middle lands on it, and the next notch from
            // there leaves it. Proximity would make the middle a value the
            // wheel cannot get out of.
            if (st.bipolar) {
                const f32 tc = knobT(st, 0.f);
                if ((t0 - tc) * (t - tc) < 0.f) t = tc;
            }
            write(knobV(st, t));
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
            write(knobV(st, t));
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

    if (showText && !typing) {           // the field draws the number now
        char buf[64];
        if (st.absent)        std::snprintf(buf, sizeof buf, "-");
        else if (st.text)     std::snprintf(buf, sizeof buf, "%s", st.text);
        else                  std::snprintf(buf, sizeof buf, st.fmt, (double)*v);
        const Rect tr{b.x, b.bottom() - textH, b.w, textH};
        const Col c = st.absent ? nx::muted.alpha(0.40f * dim)
                                : (live ? nx::text : nx::muted).alpha(dim);
        drawTextIn(*vf, tr, buf, c, Align::Center, 0.f);
    }

    if (typing) {
        f64 tv = 0.0;
        if (typeInRun(id, b, (f64)lo, (f64)hi, &tv) > 0) write((f32)tv);
    }

    if (live) cursor = Cursor::ResizeV;
    return changed;
}

// The mod ring, and it is HERE for one reason: every number it needs is
// knobNx's. The sweep, the cap radius and the arc thickness are the file
// locals above, and a caller that placed the ring itself would have to mirror
// all three across a module boundary -- which is the mistake this method
// exists to delete. Handed the same rect and the same style as its knob, the
// two are concentric by construction.
//
// It draws and nothing else: no id, no hot rect, no input. The ring is not a
// second control on top of the first, it is the first one annotated.
void Ui::knobRing(const Rect& b, const KnobStyle& st, f32 v, f32 lo, f32 hi,
                  const Col& c) {
    if (!r) return;
    const f32 dpi = std::max(1.f, r->dpiScale());
    Font* vf = fSmall ? fSmall : fBody;
    const bool showText = (st.fmt || st.text) && vf;
    const f32 textH = showText ? vf->height() : 0.f;
    const f32 avail = std::min(b.w, b.h - textH);
    const f32 rad = avail * 0.5f - 2.f * dpi;
    if (rad <= 1.f) return;

    // OUTSIDE the value arc, and outside the bipolar detent's tick as well:
    // the detent starts at aRad + aTh/2 + 0.5px and is 2px long, so the ring
    // clears it by the same 2.2px whether or not the control has a middle.
    const f32 aRad = rad + 2.5f * dpi;
    const f32 aTh  = std::max(1.5f * dpi, rad * 0.17f);
    const f32 ringRad = aRad + aTh * 0.5f + 2.2f * dpi;

    const f32 t  = knobT(st, v);
    const f32 a0 = kKnobA0 + (kKnobA1 - kKnobA0) * clampv(t + lo, 0.f, 1.f);
    const f32 a1 = kKnobA0 + (kKnobA1 - kKnobA0) * clampv(t + hi, 0.f, 1.f);
    if (a1 - a0 <= 1e-4f) return;              // an empty reach draws nothing

    arc(b.cx(), b.y + 2.f * dpi + rad, ringRad, a0, a1,
        std::max(1.f, 1.3f * dpi), c);
}

// ---------------------------------------------------------------------------
// The value trough
// ---------------------------------------------------------------------------

bool Ui::trough(u64 id, const Rect& b, f32* v, f32 lo, f32 hi, const Col& fill, f32 dim,
                f32 def) {
    if (!v || !r || b.w <= 2.f || b.h <= 1.f) return false;
    const f32 dpi = std::max(1.f, r->dpiScale());
    const f32 rad = std::min(nx::radiusXs * dpi, b.h * 0.5f);
    const f32 span = hi - lo;
    const bool typing = typingIn(id);

    setHot(id, b);
    const bool hotNow = isHot(id);
    bool changed = false;
    const auto write = [&](f32 nv) {
        nv = clampv(nv, std::min(lo, hi), std::max(lo, hi));
        if (nv != *v) { probeValue("trough", id, *v, nv); *v = nv; changed = true; }
    };
    static const char* const kNoDef = "This slider has no default to reset to";

    if (!typing) {
        if (in->pressed[0] && hotNow) {
            active = id;
            dragAccum = 0.f;
            // A press JUMPS: a trough this short is a target, not a handle, and
            // hunting for a 1px head to grab would be the worse of the two.
            write(lo + span * clampv((in->mx - b.x) / b.w, 0.f, 1.f));
            dragStart = (f64)*v;
        }
        if (in->pressed[2] && hotNow) menuOpen(id, (f64)def, kNoDef);
        if (wantReset(id, (f64)def, kNoDef)) { write(def); dragStart = (f64)*v; dragAccum = 0.f; }
        if (menuFired(id, MenuTypeIn)) beginTypeIn(id, (f64)*v);
        // Double-click resets, like every other continuous control -- but only
        // where the caller has said what to. Without a `def` this refuses out
        // loud instead of being a gesture that does nothing.
        if (in->dblClick && hotNow) {
            if (std::isfinite(def)) { write(def); dragStart = (f64)*v; dragAccum = 0.f; }
            else                    refusal = kNoDef;
        }
        // The trough's coarse rate is "the span across b.w", so a notch is
        // kWheelPx of THAT -- the same five pixels of hand movement a knob's
        // notch is, measured against this control's own throw.
        if (const f32 w = wheelNotch(in, hotNow)) {
            write(*v + span * (kWheelPx * dpi / b.w) * fineScale(in) * w);
            dragStart = (f64)*v;
            dragAccum = 0.f;
        }
        if (active == id && in->dx != 0.f) {
            dragAccum += in->dx * fineScale(in);
            write((f32)dragStart + span * (dragAccum / b.w));
        }
        if (in->released[0] && active == id) active = 0;
    }

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

    if (typing) {
        f64 tv = 0.0;
        if (typeInRun(id, b, (f64)lo, (f64)hi, &tv) > 0) write((f32)tv);
    }

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
        if (*t != 0.85f) { probeValue("vFader", id, *t, 0.85f); *t = 0.85f; changed = true; }
        dragStart = (f64)*t;              // unity
        dragAccum = 0.f;
    }
    // The wheel, in the fader's own travel. No menu here, deliberately: a
    // fader's stored value is a POSITION on a curve, not the number printed
    // beside it, so "type in value" would be a box that takes 0.85 and not
    // -3.0 dB -- a type-in that lies is worse than none. It gets that the day
    // the fader's dB mapping moves into the widget layer.
    if (const f32 w = wheelNotch(in, hotNow)) {
        const f32 nv = clampv(*t + (kWheelPx * std::max(1.f, r->dpiScale()) / travel) *
                                       fineScale(in) * w, 0.f, 1.f);
        if (nv != *t) { probeValue("vFader", id, *t, nv); *t = nv; changed = true; }
        dragStart = (f64)*t;
        dragAccum = 0.f;
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
                    const char* fmt, Align align, const char* zeroLabel, f64 step,
                    f64 def) {
    if (!v || !r) return false;
    const bool typing = typingIn(id);
    setHot(id, b);
    const bool hotNow = isHot(id);
    bool changed = false;
    static const char* const kNoDef = "This field has no default to reset to";

    const auto write = [&](f64 nv) {
        // Snap before clamping, so the endpoints of the range stay reachable
        // even when they are not multiples of the step.
        if (step > 0.0) nv = std::floor(nv / step + 0.5) * step;
        nv = clampv(nv, lo, hi);
        if (nv != *v) { probeValue("dragNumber", id, *v, nv); *v = nv; changed = true; }
    };

    if (!typing) {
    if (in->pressed[0] && hotNow) {
        active = id;
        dragAccum = 0.f;
        dragStart = *v;
    }
    if (in->pressed[2] && hotNow) menuOpen(id, def, kNoDef);
    if (wantReset(id, def, kNoDef)) { write(def); active = 0; dragStart = *v; dragAccum = 0.f; }
    if (menuFired(id, MenuTypeIn)) beginTypeIn(id, *v);
    if (in->dblClick && hotNow) {
        if (std::isfinite(def)) {
            write(def);
            active = 0;                  // the double-click's press must not also start a drag
            dragStart = *v;
        } else {
            refusal = kNoDef;
        }
    }
    // A notch is ONE STEP where the caller declared one -- a field that snaps
    // to a multiple has no finer value to offer, and the fine modifier cannot
    // invent one -- and kWheelPx of drag where it did not.
    if (const f32 w = wheelNotch(in, hotNow)) {
        const f64 d = step > 0.0
            ? step
            : (f64)(kWheelPx * std::max(1.f, r->dpiScale())) * perPixel *
                  (fineMod(in) ? (f64)kFineNumber : 1.0);
        write(*v + d * (f64)w);
        dragStart = *v;
        dragAccum = 0.f;
    }
    if (active == id && in->dy != 0.f) {
        dragAccum += -in->dy * (fineMod(in) ? kFineNumber : 1.f);   // drag up = increase
        write(dragStart + (f64)dragAccum * perPixel);
    }
    if (in->released[0] && active == id) active = 0;
    }

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
    if (f && !typing) {                  // the type-in field draws the number now
        char buf[80];
        if (zeroLabel && std::fabs(*v) < 1e-9) std::snprintf(buf, sizeof buf, "%s", zeroLabel);
        else                                   std::snprintf(buf, sizeof buf, fmt ? fmt : "%.2f", *v);
        drawTextIn(*f, b, buf, live ? nx::text : pal::textDim, align, 3.f);
    }

    if (typing) {
        f64 tv = 0.0;
        if (typeInRun(id, b, lo, hi, &tv) > 0) write(tv);
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
    // NO CONTROL MENU HERE, and that is the exception the rule needs stating
    // for: click-forward / right-click-back / wheel-scrub is a documented idiom
    // in four files (arrange.cpp, app_spectra.cpp, pianoroll.cpp), a selector
    // has no default worth a Reset and no number to type, and taking its
    // right-click away would break a habit to offer a menu with nothing useful
    // in it.
    //
    // The notch IS consumed now, which it was not: a selector inside a
    // scrolling strip used to step its option AND scroll the strip on one
    // notch, and arrange.cpp carries a whole paragraph working around exactly
    // that. One notch, one answer.
    if (const f32 w = wheelNotch(in, hotNow)) step(w > 0.f ? +1 : -1);

    const bool held = (active == id) && over;
    const UiMotion m = motion(id, hotNow, held);
    const Rect br = liftPress(b, m);
    // The control radius TOKEN, not a capsule. h*0.5 was the one stadium left
    // in the program after the de-pilling -- §2 bans the shape outright, and
    // every sibling control already clamps to nx::pill exactly like this.
    pillRect(br, std::min(nx::pill * std::max(1.f, r->dpiScale()), br.h * 0.5f),
             Pill::Secondary, pal::accent, m);

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
