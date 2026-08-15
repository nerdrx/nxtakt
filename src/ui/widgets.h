// Immediate-mode widget layer. Each widget owns a stable id so hot/active
// tracking survives layout changes between frames.
//
// THE NX SKIN (docs/DESIGN.md §5). Everything below is drawn in the design
// language: buttons are glass pills, fields are recessed wells that take a
// violet focus ring, badges are --glass-chip with uppercase micro-labels. The
// existing signatures are unchanged and every existing call site keeps working
// -- a view gets the new language for free and only reaches for the additions
// (pillRect, tabPill, chip, microLabel, fieldWell) when it wants a shape the
// old vocabulary could not say.
#pragma once
#include "../gfx/renderer.h"
#include "window.h"
#include <string>

namespace lat {

inline u64 uiId(int kind, int a = 0, int b = 0) {
    u64 h = 0x9E3779B97F4A7C15ull;
    h ^= (u64)(u32)kind * 0x100000001B3ull;
    h = (h << 7) | (h >> 57);
    h ^= (u64)(u32)a * 0xC2B2AE3D27D4EB4Full;
    h = (h << 11) | (h >> 53);
    h ^= (u64)(u32)b * 0x165667B19E3779F9ull;
    return h ? h : 1;
}

// ---------------------------------------------------------------------------
// §6  Motion, in an immediate-mode UI
//
// A widget here is drawn from scratch every frame, so there is no object to
// hang a transition on: `hover` and `press` are booleans that flip, and §5 asks
// for a lift and a scale that take 150 ms to arrive. Ui::motion() is the memory
// that makes that possible -- see the flat table at the top of widgets.cpp.
//
// Both weights run 0..1 and are already eased; a caller multiplies rather than
// branches. Under NXTAKT_REDUCED_MOTION they are exactly 0 or 1 on the frame
// the state changes, which is how every animation in the program collapses
// without one call site knowing about it.
// ---------------------------------------------------------------------------
struct UiMotion {
    f32 hover = 0.f;
    f32 press = 0.f;
};

// §5's pill treatments. The fill hue is passed separately, because "primary"
// is a *role* -- the transport's play button and a dialog's confirm button are
// the same material in different hues, and RECORD is Danger not because it is
// an error but because §1 reserves red for the destructive-adjacent.
enum class Pill {
    Secondary,   // --glass-chip fill, --edge border. The default.
    Primary,     // tinted fill with the inner top highlight and a soft glow.
    Danger,      // the same, in --danger. Genuinely destructive only.
    Ghost,       // no fill at rest; the glass arrives with the hover.
};

struct Ui {
    Renderer* r = nullptr;
    Input* in = nullptr;
    Font* fSmall = nullptr;    // 10px, labels
    Font* fBody  = nullptr;    // 11px, general
    Font* fBold  = nullptr;    // 11px bold, headers
    Font* fBig   = nullptr;    // 15px, tempo / position readouts

    u64  hot = 0, active = 0;
    u64  hotNext = 0;
    Cursor cursor = Cursor::Arrow;
    f32  dragAccum = 0.f;
    f64  dragStart = 0.0;

    // Inline text editing.
    u64  editId = 0;
    std::string editBuf;
    int  caret = 0;
    bool editCommitted = false;

    // Tooltip requested this frame.
    std::string tip;

    // Where the last button()/squareToggle() actually drew, after §5's hover
    // lift and press scale. A caller that paints its own glyph inside a button
    // -- the transport's play triangle, the record circle -- draws into THIS
    // rect rather than the one it passed in, so the glyph travels with the pill
    // instead of standing still while the pill moves under it.
    Rect lastRect{};

    // The deferred-label queue (see beginDeferText below). Fixed size and
    // inline: it is written on almost every widget call, so it may not
    // allocate, and a job that will not fit is simply drawn immediately rather
    // than dropped -- overflowing costs the draw call it was trying to save,
    // never a missing label.
    struct TextJob {
        const Font* f = nullptr;
        Rect b{};
        Col  c{};
        Align a = Align::Left;
        f32  padX = 0.f;
        bool micro = false;
        char s[56] = {};
    };
    static constexpr int kMaxTextJobs = 64;
    TextJob textJobs[kMaxTextJobs];
    int  textJobN = 0;
    bool deferText = false;

    void beginFrame() { hotNext = 0; cursor = Cursor::Arrow; tip.clear(); }
    // flushText() first, as a backstop: a view that opens a deferral window and
    // returns before closing it would otherwise lose its labels for the frame.
    // It is a no-op when nothing is queued, which is every frame that plays by
    // the rules.
    void endFrame()   { flushText(); hot = hotNext; if (!in->down[0] && active && active != editId) active = 0; }

    bool hovered(const Rect& b) const { return b.contains(in->mx, in->my); }
    bool setHot(u64 id, const Rect& b) {
        if (hovered(b) && r->currentClip().contains(in->mx, in->my)) { hotNext = id; return true; }
        return false;
    }
    bool isHot(u64 id) const { return hot == id; }

    // --- widgets ----------------------------------------------------------
    // Returns true on click (release inside).
    //
    // `radius` defaults to -1, which means "the pill radius this widget is
    // entitled to" (§5) rather than a number a call site had to know. Passing a
    // radius still overrides it, for the handful of places that need to match a
    // neighbour's corner; nothing in the program passes one today.
    bool button(u64 id, const Rect& b, const char* label, bool on = false,
                Col onCol = pal::accent, f32 radius = -1.f);
    // Small square toggle, Live's M / S / arm buttons.
    bool squareToggle(u64 id, const Rect& b, const char* label, bool* value, Col onCol);
    // Circular knob with an arc; `v` in [lo,hi]. Returns true while changing.
    bool knob(u64 id, const Rect& b, f32* v, f32 lo, f32 hi, f32 def, const char* fmt = nullptr);
    // Vertical fader with a stepped scale, like Live's mixer.
    bool vFader(u64 id, const Rect& b, f32* t);
    // Draggable numeric readout (tempo, gain, ...). Vertical drag.
    // `zeroLabel`, when given, replaces the formatted number at exactly zero:
    // a DAW is full of fields where 0 means "follow something else" and reads
    // as "Auto" or "Off" rather than as a quantity. `step`, when > 0, snaps the
    // value to a multiple of itself, which also keeps such a field landing on
    // an exact 0 instead of drifting past it.
    bool dragNumber(u64 id, const Rect& b, f64* v, f64 lo, f64 hi, f64 perPixel,
                    const char* fmt, Align align = Align::Center,
                    const char* zeroLabel = nullptr, f64 step = 0.0);
    // Click cycles through `options`; right-click steps backwards.
    bool selector(u64 id, const Rect& b, int* idx, const char* const* options, int count);
    // Editable text. Returns true when the value was committed.
    bool textField(u64 id, const Rect& b, std::string* value, Col bg, Col fg,
                   Align align = Align::Left, bool activateOnDouble = true);
    // What `id` currently has in the edit buffer, or null when it does not own
    // the caret. textField only writes back on commit, so a field whose owner
    // has to react per keystroke (a filter narrowing as you type) reads the
    // live text through here rather than reaching into editBuf itself.
    const std::string* liveText(u64 id) const { return editId == id ? &editBuf : nullptr; }

    // --- the NX vocabulary (§5) -------------------------------------------
    //
    // Additive: everything above keeps its signature and is restyled from
    // underneath, and these are the shapes the old vocabulary could not say.

    // The hover/press weights for `id` this frame. Call it once per widget per
    // frame with the states the widget has already worked out; calling it twice
    // in a frame is harmless but the second call sees the first one's clock.
    UiMotion motion(u64 id, bool hot, bool held);

    // §5's transform-only feedback, as geometry: hover lifts 1-2px, press
    // scales to 0.96 about the centre. Immediate mode has no transform stack,
    // so "lift" is the rect drawn higher and "scale" is the rect drawn smaller
    // -- which is the same pixels a compositor transform would have produced,
    // and costs nothing.
    Rect liftPress(const Rect& b, const UiMotion& m) const;

    // A pill, drawn and nothing else -- no hit testing, no label. This is what
    // button() paints with, exposed so a view that owns its own interaction
    // (the transport's glyph buttons) draws the same material.
    void pillRect(const Rect& b, f32 radius, Pill kind, const Col& tint,
                  const UiMotion& m) const;

    // §5's tab pill: ONE indicator that slides between equal slots on
    // --ease-spring, never two toggled backgrounds. `*idx` is both the current
    // slot and where a click writes; returns true when it changed.
    //
    // The indicator interpolates from wherever it was standing, so a click
    // during a slide is picked up mid-flight instead of restarting.
    bool tabPill(u64 id, const Rect& b, const char* const* labels, int count, int* idx);

    // §5/§7's uppercase micro-label: 10-11px, 0.12em tracking. The renderer has
    // no tracking parameter, so the string is drawn glyph by glyph -- about one
    // extra pixel per character at 10px, and the whole difference between a
    // chip that reads as a label and one that reads as small body text.
    f32  microLabel(const Font& f, f32 x, f32 y, const char* s, const Col& c);
    f32  microWidth(const Font& f, const char* s) const;
    void microIn(const Font& f, const Rect& b, const char* s, const Col& c,
                 Align a = Align::Center, f32 padX = 0.f);

    // A status chip (§5): --glass-chip, pill radius, uppercase micro-label.
    // cyan = live, amber = pending attention, muted = inert.
    void chip(const Rect& b, const char* label, const Col& ink);

    // --- text batching ----------------------------------------------------
    //
    // THE COST THIS EXISTS TO REMOVE. The batcher flushes whenever the bound
    // texture changes, so a row of controls that each draw a shape and then a
    // label costs one draw call per control. That was true before the re-skin
    // and merely wasteful; it is expensive now, because an NX shape is a
    // gradient and every batch carrying one re-uploads the gradient table --
    // measured at roughly 8 us of driver synchronisation apiece on this
    // machine, whatever the batch actually draws.
    //
    // Between these two calls every label a widget would have drawn is queued
    // instead. The shapes then land in one batch and the labels in one more:
    // twenty alternations become two.
    //
    // TWO RULES, and they are not enforceable, so a view that cannot promise
    // both simply does not call these and pays what it always paid:
    //   * nothing inside the window may overlap anything else in it -- queued
    //     text is drawn above every shape drawn after it was queued;
    //   * no pushClip/popClip inside the window -- the queue is drawn under
    //     whatever clip is current when it is flushed, not when it was queued.
    // A transport bar satisfies both trivially. A scrolling list does not.
    void beginDeferText();
    void flushText();

    // Text as the widgets draw it: queued when a deferral window is open,
    // straight through to the renderer when it is not. A view drawing its own
    // labels inside a window has to go through here for them to be batched.
    void drawTextIn(const Font& f, const Rect& b, const char* s, const Col& c,
                    Align a = Align::Left, f32 padX = 4.f);

    // A recessed field (§5 inputs). `focus` in 0..1 runs the --line border to
    // violet and blooms the focus ring; never a bare outline.
    void fieldWell(const Rect& b, f32 focus, bool deep = true) const;

    // --- drawing helpers --------------------------------------------------
    void meterV(const Rect& b, f32 lvl, f32 peak);   // vertical peak meter
    void arc(f32 cx, f32 cy, f32 rad, f32 a0, f32 a1, f32 th, const Col& c);
    void bevel(const Rect& b, f32 radius, const Col& fill, f32 lightness = 0.06f);
    void playTriangle(const Rect& b, const Col& c);
    void stopSquare(const Rect& b, const Col& c);
};

} // namespace lat
