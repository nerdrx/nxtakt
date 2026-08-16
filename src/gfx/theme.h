// The NX design language, translated.
//
// docs/DESIGN.md §2 is the contract; this header is that contract expressed as
// typed constants so a view never spells a hex literal, an alpha or a duration
// by hand. Everything here is `constexpr` and lives in a header on purpose: the
// tokens are values, not state, and a view that reaches for `nx::violet` should
// pay nothing for it.
//
// Two things this header deliberately does NOT do:
//
//   * It does not touch `pal::` (color.h). That palette is the *old* skin and
//     every view still references it; `pal::accent` is already #7700FF, which
//     is why the brand anchor survives the transition untouched. Views migrate
//     token by token, and until the last one does, both namespaces compile.
//
//   * It does not decide *where* glass goes. §4's tier table is encoded here as
//     data (`nx::glass(Tier::Card)`), but the judgment -- chrome gets glass,
//     working surfaces stay flat and fast -- lives in the views. A frosted
//     piano roll is a usability bug wearing a costume.
//
// Coordinate/angle conventions, so the numbers below can be read straight off
// the CSS in the spec:
//
//   * Gradient angles follow CSS: 0deg points to the TOP, 90deg to the right,
//     180deg to the bottom. So the spec's 157deg and 147deg both run down-and-
//     right, which is what puts the light in the upper-left. Every angle in
//     this file is between 90 and 200 for exactly that reason -- if you ever
//     add one outside that band, you have moved the sun.
//
//   * Stop positions are 0..1, not percentages.
//
//   * Shadow offsets are +y downward (screen space), matching CSS.
#pragma once
#include "../core/common.h"
#include "color.h"

namespace lat {
namespace nx {

// ---------------------------------------------------------------------------
// §1 / §2  Colour
// ---------------------------------------------------------------------------

inline constexpr Col bgTop      = rgb(0x0A0714);   // field, top
inline constexpr Col bgBottom   = rgb(0x12091F);   // field, bottom
inline constexpr Col panel      = rgb(0x171028);
inline constexpr Col panel2     = rgb(0x1D1433);

// The brand anchor. Identical to pal::accent by construction, and the one
// value in this file that is frozen: actions, focus, identity.
inline constexpr Col violet     = rgb(0x7700FF);
inline constexpr Col violetSoft = rgb(0x9A3CFF);

// Light *inside* materials -- live values, meters, playheads, progress, edges.
// Never a surface colour: the moment a panel is cyan, violet has stopped
// leading and the whole thing reads as a different product.
inline constexpr Col cyan       = rgb(0x00E5FF);

inline constexpr Col amber      = rgb(0xFFB300);   // update / attention, only
inline constexpr Col danger     = rgb(0xFF5470);   // destructive, only
inline constexpr Col text       = rgb(0xEFEAFF);
inline constexpr Col muted      = rgb(0x9A8FC0);
inline constexpr Col line       = rgb(0x2A1F45);

// Semantic aliases. Same values; these are what a view should reach for when
// it means the *role* rather than the hue, because roles survive a retune.
inline constexpr Col live       = cyan;    // playing, connected, metering
inline constexpr Col attention  = amber;
inline constexpr Col brand      = violet;

// ---------------------------------------------------------------------------
// §2  Geometry -- radii and the 8px rhythm
// ---------------------------------------------------------------------------

// GEOMETRY — one notch tighter than the hub's numbers, inside the spec's band.
// History: v1.0 of the spec said 18/12/8/capsule; the owner's verdict on a
// dense professional instrument was "a toy", and spec v1.1 then codified the
// correction globally — "angular, never rounded", radii 3–6px, pills banned
// (the hub itself ships 6/4/3/5 now). NxTakt sits at 6/3/2/3: same band,
// tuned for an instrument surface with one shared control height.
inline constexpr f32 radius   = 6.f;    // cards, sheets, panels
inline constexpr f32 radiusSm = 3.f;    // rows, wells, inputs
inline constexpr f32 radiusXs = 2.f;    // chips, code
inline constexpr f32 pill     = 3.f;    // "pill" now means a squared control; the
                                        // name survives so call sites don't churn

inline constexpr f32 sp1 = 8.f;
inline constexpr f32 sp2 = 16.f;
inline constexpr f32 sp3 = 24.f;
inline constexpr f32 sp4 = 32.f;
inline constexpr f32 gridStep = 8.f;

// Rounds a logical-pixel measurement onto the 8px grid. Use it on layout, not
// on glyph metrics -- text is measured, not gridded.
inline f32 snapGrid(f32 v) { return std::round(v / gridStep) * gridStep; }

// Rounds a device-pixel coordinate onto a whole pixel. The renderer works in
// device pixels (App multiplies by dpiScale before it calls in), so a 1px lit
// edge is only crisp if its rect lands on integers -- which under fractional
// scale it otherwise never does.
inline f32 snapPx(f32 v) { return std::round(v); }

// ---------------------------------------------------------------------------
// §7  Type scale
//
// Pixel sizes at dpiScale 1; a view multiplies by the scale exactly as it does
// for every other measurement. These are the sizes to load fonts at, not a
// styling API -- the renderer takes a Font&, and picking the right one is the
// view's job.
// ---------------------------------------------------------------------------

inline constexpr int fsTitle  = 21;   // 20-22 bold
inline constexpr int fsBody   = 14;
inline constexpr int fsSecond = 12;   // 12-13, muted
inline constexpr int fsMicro  = 10;   // 10-11 uppercase, wide tracking

// Extra letter-spacing for the micro-label chips, in ems. The renderer has no
// tracking parameter; a view that wants it draws glyph by glyph or accepts the
// default. Kept here so the number is not invented twice.
inline constexpr f32 microTracking = 0.12f;

// §7's contrast requirement, measured rather than asserted. Sampled off the
// rendered specimen sheet (NXTAKT_DEBUG_GLASS=1) and run through the WCAG
// relative-luminance formula, `text` and `muted` over every fill in the system:
//
//   fill            --text    --muted
//   --glass-1       15.6      6.2
//   --glass-2       15.6      6.2
//   --glass-bar     14.6      5.8
//   --glass-chip    14.4      5.7
//   --well          16.8      6.6
//   --well-deep     17.1      6.8
//   bare field      16.7      6.6
//   VIOLET FILL      4.8      1.9   <-- the one exception
//
// So: `text` clears AAA (7:1) on every translucent fill, `muted` clears AA
// (4.5:1) on every one, and neither fill needs darkening.
//
// The exception is the rule worth remembering: on a violet fill, `text` is
// still fine at 4.8 but `muted` is 1.9 and is illegible. Nothing secondary
// goes on a primary button. `inkOn()` picks correctly for both.
inline Col inkOn(const Col& fill) {
    const f32 luma = 0.299f * fill.r + 0.587f * fill.g + 0.114f * fill.b;
    return luma > 0.45f ? Col(0.086f, 0.055f, 0.157f, 1.f) : text;
}

// ---------------------------------------------------------------------------
// §6  Motion
//
// The three easings are the spec's cubic-beziers, solved properly: given the
// linear progress t, invert x(u) = t by Newton (with a bisection fallback for
// the near-flat stretches Newton walks off) and return y(u). Approximating
// these with a smoothstep would quietly delete the overshoot, which is the
// entire point of ease-spring.
// ---------------------------------------------------------------------------

namespace detail {
// Cubic Bezier with P0 = (0,0) and P3 = (1,1); a1/a2 are the control values on
// the axis being evaluated.
inline f32 bez(f32 t, f32 a1, f32 a2) {
    const f32 u = 1.f - t;
    return 3.f * u * u * t * a1 + 3.f * u * t * t * a2 + t * t * t;
}
inline f32 bezD(f32 t, f32 a1, f32 a2) {
    const f32 u = 1.f - t;
    return 3.f * a1 * u * (1.f - 3.f * t) + 3.f * a2 * t * (2.f - 3.f * t) + 3.f * t * t;
}
} // namespace detail

struct Ease {
    f32 x1, y1, x2, y2;

    // t in [0,1] -> eased progress. May exceed 1 (spring overshoot) by design.
    f32 operator()(f32 t) const {
        if (t <= 0.f) return 0.f;
        if (t >= 1.f) return 1.f;
        f32 u = t;
        for (int i = 0; i < 6; ++i) {
            const f32 e = detail::bez(u, x1, x2) - t;
            if (std::fabs(e) < 1e-5f) break;
            const f32 d = detail::bezD(u, x1, x2);
            if (std::fabs(d) < 1e-5f) break;
            u = clampv(u - e / d, 0.f, 1.f);
        }
        if (std::fabs(detail::bez(u, x1, x2) - t) > 1e-4f) {
            f32 lo = 0.f, hi = 1.f;
            u = t;
            for (int i = 0; i < 26; ++i) {
                const f32 x = detail::bez(u, x1, x2);
                if (std::fabs(x - t) < 1e-6f) break;
                if (x < t) lo = u; else hi = u;
                u = (lo + hi) * 0.5f;
            }
        }
        return detail::bez(u, y1, y2);
    }

    // Convenience: interpolate a value over an elapsed/duration pair.
    f32 at(f32 elapsed, f32 duration) const {
        return (*this)(duration > 0.f ? clampv(elapsed / duration, 0.f, 1.f) : 1.f);
    }
};

// Overshoots past 1 and settles back. Identity-bearing moves only: the tab
// pill, a tile press. Used anywhere else it reads as a bug.
inline constexpr Ease easeSpring{0.32f, 1.35f, 0.42f, 1.00f};
// The workhorse: every ordinary interactive transition.
inline constexpr Ease easeSoft{0.20f, 0.80f, 0.20f, 1.00f};
// Entrances -- sheets rising, toasts arriving.
inline constexpr Ease easeOut{0.16f, 1.00f, 0.30f, 1.00f};

// Seconds, because every clock in this program is seconds. The millisecond
// spellings are here so a reader can match them against the spec at a glance.
inline constexpr f32 durFast = 0.150f;
inline constexpr f32 dur     = 0.220f;
inline constexpr f32 durSlow = 0.320f;
inline constexpr int durFastMs = 150, durMs = 220, durSlowMs = 320;

// §6: "prefers-reduced-motion is non-negotiable." There is no such thing as a
// desktop-wide preference we can portably read here, so the switch is an
// environment knob, read once. env() takes the NXTAKT_/LATTICE_ pair.
//
// What honouring it means, concretely: the nebula freezes at t = 0, sheens do
// not slide, springs collapse to `easeSoft`, and view transitions become plain
// opacity. The renderer enforces the first two; a view must check this before
// it animates anything itself.
inline bool reducedMotion() {
    static const bool v = [] {
        const char* s = env("REDUCED_MOTION");
        return s && *s && s[0] != '0';
    }();
    return v;
}

// The easing to actually use for an identity move, after reduced motion.
inline const Ease& spring() { return reducedMotion() ? easeSoft : easeSpring; }

// ---------------------------------------------------------------------------
// Gradients
//
// One type covers every fill, edge, hairline, sheen and nebula blob in the
// system, because in CSS they are all one type too. Five stops is not an
// arbitrary cap: --hairline has five, and nothing in §2 has more.
//
// A Grad is a plain aggregate so the tokens below can be `constexpr` and cost
// nothing; the renderer hashes one into a slot of a small GPU-side table, so
// the same token used two hundred times in a frame occupies one row.
// ---------------------------------------------------------------------------

struct Stop {
    Col c{};
    f32 pos = 0.f;
};

struct Grad {
    static constexpr int kMaxStops = 5;

    Stop stops[kMaxStops]{};
    int  n = 0;
    f32  angleDeg = 180.f;      // CSS convention: 0 = to top, 90 = to right
    bool radial = false;
    // Radial only, as fractions of the rect: centre and the radii at which the
    // last stop lands. 0.5 => the ellipse touches the rect's edges.
    f32  cx = 0.5f, cy = 0.5f, rx = 0.5f, ry = 0.5f;

    // Scale every stop's alpha. §1's "if a gradient is visible from across the
    // room, halve it" is literally `g.faded(0.5f)`.
    Grad faded(f32 k) const {
        Grad o = *this;
        for (int i = 0; i < kMaxStops; ++i) o.stops[i].c.a *= k;
        return o;
    }
    // Keep the alpha ramp, replace the hue. This is how a hairline becomes a
    // violet hairline without inventing a second five-stop token.
    Grad tinted(const Col& c) const {
        Grad o = *this;
        for (int i = 0; i < kMaxStops; ++i) {
            o.stops[i].c.r = c.r; o.stops[i].c.g = c.g; o.stops[i].c.b = c.b;
        }
        return o;
    }
    // Slide every stop along the gradient axis. Stops may leave [0,1]; the
    // shader holds the end colours beyond the ends, so a band simply travels
    // off the surface. This is what animates the sheen without touching layout.
    Grad shifted(f32 d) const {
        Grad o = *this;
        for (int i = 0; i < kMaxStops; ++i) o.stops[i].pos += d;
        return o;
    }
    Grad rotated(f32 deg) const { Grad o = *this; o.angleDeg = deg; return o; }
};

// Convenience for the very common two-stop case.
inline constexpr Grad linear2(f32 angleDeg, Col a, Col b) {
    return Grad{{{a, 0.f}, {b, 1.f}}, 2, angleDeg, false, 0.5f, 0.5f, 0.5f, 0.5f};
}

// --- §2 glass fills: light collects top-left and drains to a cool shadow ----

inline constexpr Grad glassBar = {
    {{rgba(0x2E1E4E, 0.62f), 0.f}, {rgba(0x120B22, 0.72f), 1.f}}, 2, 180.f};

inline constexpr Grad glass1 = {
    {{rgba(0xFFFFFF, 0.090f), 0.00f},
     {rgba(0xFFFFFF, 0.026f), 0.34f},
     {rgba(0x171028, 0.340f), 1.00f}}, 3, 157.f};

inline constexpr Grad glass2 = {
    {{rgba(0xFFFFFF, 0.100f), 0.00f},
     {rgba(0xFFFFFF, 0.030f), 0.30f},
     {rgba(0x130C22, 0.660f), 1.00f}}, 3, 158.f};

inline constexpr Grad glassChip = {
    {{rgba(0xFFFFFF, 0.090f), 0.f}, {rgba(0xFFFFFF, 0.028f), 1.f}}, 2, 180.f};

// Wells are the answer to "glass inside glass reads as fog": a region inside a
// card recesses, it does not frost again.
inline constexpr Grad well = {
    {{rgba(0x070410, 0.50f), 0.f}, {rgba(0x070410, 0.32f), 1.f}}, 2, 180.f};

inline constexpr Grad wellDeep = {
    {{rgba(0x04020A, 0.62f), 0.f}, {rgba(0x04020A, 0.46f), 1.f}}, 2, 180.f};

// --- §2 lit edges: 1px gradient borders, bright top-left -> dark bottom-right

inline constexpr Grad edge = {
    {{rgba(0xFFFFFF, 0.340f), 0.00f},
     {rgba(0xFFFFFF, 0.090f), 0.24f},
     {rgba(0xFFFFFF, 0.015f), 0.52f},
     {rgba(0x000000, 0.340f), 1.00f}}, 4, 147.f};

inline constexpr Grad edgeLit = {
    {{rgba(0xE2C8FF, 0.620f), 0.00f},
     {rgba(0x9A3CFF, 0.280f), 0.30f},
     {rgba(0x00E5FF, 0.100f), 0.58f},
     {rgba(0x000000, 0.300f), 1.00f}}, 4, 147.f};

inline constexpr Col edgeTop = rgba(0xFFFFFF, 0.18f);

// The only legal divider in the system. Fades to nothing at both ends; a solid
// grey line anywhere is a bug (§11).
inline constexpr Grad hairline = {
    {{rgba(0xFFFFFF, 0.000f), 0.00f},
     {rgba(0xFFFFFF, 0.090f), 0.18f},
     {rgba(0xFFFFFF, 0.130f), 0.50f},
     {rgba(0xFFFFFF, 0.090f), 0.82f},
     {rgba(0xFFFFFF, 0.000f), 1.00f}}, 5, 90.f};

// The ramp's peak, and the default ink a divider is drawn in. A caller passes a
// colour whose alpha is the peak it wants -- `nx::violet.alpha(0.3f)` for an
// accented separator -- and the renderer rescales the whole ramp to match, so
// the fade-at-both-ends shape survives any recolouring.
inline constexpr f32 hairlinePeak = 0.13f;
inline constexpr Col hairlineInk  = rgba(0xFFFFFF, hairlinePeak);

// The masked diagonal highlight that slides across a card on hover (§5). The
// band lives between 0.30 and 0.68 of the gradient axis; the renderer slides it
// by shifting stop positions, which is transform-equivalent -- no layout, no
// per-frame geometry.
inline constexpr Grad sheenBand = {
    {{rgba(0xFFFFFF, 0.000f), 0.30f},
     {rgba(0xFFFFFF, 0.085f), 0.45f},
     {rgba(0xD6BEFF, 0.050f), 0.52f},
     {rgba(0xFFFFFF, 0.000f), 0.68f}}, 4, 112.f};

// --- §3 the living background ----------------------------------------------

inline constexpr Grad field = linear2(180.f, bgTop, bgBottom);

// Radial, drawn last over everything: edges stay darker than centre. rx/ry of
// 0.62 push the fully-dark stop past the corners so the falloff is gentle
// rather than a visible ring.
inline constexpr Grad vignette = {
    {{rgba(0x000000, 0.00f), 0.00f},
     {rgba(0x000000, 0.10f), 0.62f},
     {rgba(0x000000, 0.46f), 1.00f}}, 3, 0.f, true, 0.5f, 0.5f, 0.62f, 0.62f};

// The blobs. Three stops rather than two so the falloff is a bell and not a
// cone -- a two-stop radial has a visible hard centre. Alphas are already
// halved from what "looked right" in isolation, per §1.
inline constexpr Grad nebulaViolet = {
    {{rgba(0x7700FF, 0.200f), 0.00f},
     {rgba(0x7700FF, 0.082f), 0.42f},
     {rgba(0x7700FF, 0.000f), 1.00f}}, 3, 0.f, true};

inline constexpr Grad nebulaCyan = {
    {{rgba(0x00E5FF, 0.085f), 0.00f},
     {rgba(0x00E5FF, 0.034f), 0.42f},
     {rgba(0x00E5FF, 0.000f), 1.00f}}, 3, 0.f, true};

inline constexpr Grad nebulaMagenta = {
    {{rgba(0xC03CFF, 0.060f), 0.00f},
     {rgba(0xC03CFF, 0.024f), 0.42f},
     {rgba(0xC03CFF, 0.000f), 1.00f}}, 3, 0.f, true};

inline constexpr Col starTint = rgba(0xD8D0FF, 0.55f);

// --- component fills --------------------------------------------------------

// §5 progress: a luminous violet -> cyan liquid fill in a recessed trough.
inline constexpr Grad liquid = {
    {{rgba(0x7700FF, 0.95f), 0.f},
     {rgba(0x9A3CFF, 0.95f), 0.45f},
     {rgba(0x00E5FF, 0.90f), 1.f}}, 3, 90.f};

// Primary button: violet with an inner top highlight, per §5.
inline constexpr Grad violetFill = {
    {{rgba(0x9A3CFF, 1.00f), 0.00f},
     {rgba(0x7700FF, 1.00f), 0.55f},
     {rgba(0x5C00C4, 1.00f), 1.00f}}, 3, 170.f};

// ---------------------------------------------------------------------------
// §2  Elevation
//
// A CSS box-shadow list becomes an Elevation of up to three specs, each one
// quad. `spread` shrinks or grows the cast shape before blurring, exactly as
// CSS does, which is what makes `-12px` in --shadow read as a shadow tucked
// under the card rather than a halo around it. blur = 0 with a positive spread
// degenerates into a crisp ring, which is how --shadow-sheet's `0 0 0 1px`
// hairline ring is expressed without a second mechanism.
// ---------------------------------------------------------------------------

struct ShadowSpec {
    f32 dx = 0, dy = 0, blur = 0, spread = 0;
    Col c{};
};

struct Elevation {
    ShadowSpec layers[3]{};
    int n = 0;
};

inline constexpr Elevation shadow = {
    {{0.f, 14.f, 34.f, -12.f, rgba(0x000000, 0.72f)},
     {0.f,  2.f,  8.f,   0.f, rgba(0x000000, 0.30f)}}, 2};

// Hover state for a card: deeper, and blooming a violet glow. The glow layer is
// the one place a shadow is not black, and it is why a lifted card reads as lit
// from within rather than merely raised.
inline constexpr Elevation shadowLift = {
    {{0.f, 26.f, 54.f, -16.f, rgba(0x000000, 0.80f)},
     {0.f,  0.f, 40.f,  -8.f, rgba(0x7700FF, 0.34f)}}, 2};

inline constexpr Elevation shadowBar = {
    {{0.f, 20.f, 44.f, -24.f, rgba(0x000000, 0.90f)},
     {0.f,  1.f,  0.f,   0.f, rgba(0xFFFFFF, 0.04f)}}, 2};

inline constexpr Elevation shadowSheet = {
    {{0.f, 48.f, 96.f, -32.f, rgba(0x000000, 0.86f)},
     {0.f,  0.f,  0.f,   1.f, rgba(0xFFFFFF, 0.06f)}}, 2};

// Two concentric violet rings. §5: never a bare outline.
inline constexpr Elevation focusRing = {
    {{0.f, 0.f, 0.f, 5.f, rgba(0x7700FF, 0.20f)},
     {0.f, 0.f, 0.f, 2.f, rgba(0x7700FF, 0.60f)}}, 2};

inline constexpr Elevation noShadow = {};

// ---------------------------------------------------------------------------
// §4  The glass tier system
//
// The table from the spec, as data. A view says `r.glass(rect, nx::Tier::Card)`
// and gets the fill, the edge and the elevation that tier is entitled to -- and
// crucially, cannot accidentally give a card real blur, because Card's
// `blurPx` is zero and there is no way to pass one in.
//
// THE CARDINAL RULE, enforced structurally rather than by comment: cards fake
// it. On a screen with dozens of cards, real backdrop blur is the difference
// between a UI and a slideshow.
// ---------------------------------------------------------------------------

enum class Tier {
    Bar,        // app header, transport bar, floating toolbars
    Card,       // content cards and tiles -- the common case, always faked
    Sheet,      // modals, slide-overs, menus, toasts
    Chip,       // buttons, badges, small floating controls
    Well,       // recessed regions INSIDE glass: list rows, logs, code
    WellDeep,   // the same, one step further down: waveform troughs, meters
};

// How a surface's 1px border is painted.
enum class EdgeKind {
    None,
    Gradient,   // the full lit edge -- bright top-left, dark bottom-right
    TopOnly,    // a single bright row across the top (the Bar tier)
    Flat,       // a plain --line stroke (wells that need a boundary at all)
};

struct GlassStyle {
    const Grad* fill = nullptr;
    const Grad* edgeGrad = nullptr;
    EdgeKind    edgeKind = EdgeKind::None;
    Col         edgeFlat{};
    Elevation   elev{};
    f32         radius = radiusSm;
    // §4's --blur-*: the strength this tier is allowed to ask the compositor
    // for, in CSS pixels. Zero means "synthesise it from the fill" -- which is
    // what Card does, always, and what every tier falls back to when the real
    // path is unavailable.
    f32         blurPx = 0.f;
};

inline GlassStyle glass(Tier t) {
    switch (t) {
    case Tier::Bar:
        return {&glassBar, nullptr, EdgeKind::TopOnly, edgeTop, shadowBar, radiusSm, 22.f};
    case Tier::Card:
        return {&glass1, &edge, EdgeKind::Gradient, {}, shadow, radius, 0.f};
    case Tier::Sheet:
        return {&glass2, &edgeLit, EdgeKind::Gradient, {}, shadowSheet, radius, 34.f};
    case Tier::Chip:
        // blurPx 0, deliberately. §4 lists --blur-chip under the SHEET row --
        // it is for small floating tier-2 things, toasts and popovers, not for
        // every button and badge on the screen. Buttons are as numerous as
        // cards and fake it for the same reason cards do; a toast wants
        // Tier::Sheet with a small radius.
        return {&glassChip, &edge, EdgeKind::Gradient, {}, noShadow, pill, 0.f};
    case Tier::Well:
        return {&well, nullptr, EdgeKind::None, {}, noShadow, radiusSm, 0.f};
    case Tier::WellDeep:
        return {&wellDeep, nullptr, EdgeKind::None, {}, noShadow, radiusSm, 0.f};
    }
    return {&glass1, &edge, EdgeKind::Gradient, {}, shadow, radius, 0.f};
}

// §4: "Keep simultaneous real-blur elements at roughly <= 10 visible."
inline constexpr int kMaxRealBlurRegions = 10;

// The debug specimen sheet, gated on NXTAKT_DEBUG_GLASS=1. Read once; the
// renderer checks it in end().
inline bool specimenEnabled() {
    static const bool v = [] {
        const char* s = env("DEBUG_GLASS");
        return s && *s && s[0] != '0';
    }();
    return v;
}

} // namespace nx
} // namespace lat
