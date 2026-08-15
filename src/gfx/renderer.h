// Batched 2D renderer. Everything the UI draws is a quad; rounded corners and
// outlines come from a signed-distance field in the fragment shader, so there
// is exactly one shader and (usually) one draw call per frame.
#pragma once
#include "../core/common.h"
#include "color.h"
#include "font.h"
#include "theme.h"
#include <vector>

namespace lat {

struct Rect {
    f32 x = 0, y = 0, w = 0, h = 0;
    bool contains(f32 px, f32 py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    Rect inset(f32 d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    Rect insetXY(f32 dx, f32 dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
    f32  right()  const { return x + w; }
    f32  bottom() const { return y + h; }
    f32  cx() const { return x + w * 0.5f; }
    f32  cy() const { return y + h * 0.5f; }
    Rect intersect(const Rect& o) const {
        const f32 x0 = std::max(x, o.x), y0 = std::max(y, o.y);
        const f32 x1 = std::min(right(), o.right()), y1 = std::min(bottom(), o.bottom());
        return {x0, y0, std::max(0.f, x1 - x0), std::max(0.f, y1 - y0)};
    }
};

enum class Align { Left, Center, Right };

class Renderer {
public:
    bool init();
    void shutdown();

    void begin(int w, int h, f32 dpiScale);
    void end();

    // Shapes
    void rect(const Rect& r, const Col& c);
    void roundRect(const Rect& r, f32 radius, const Col& c);
    void roundRectOutline(const Rect& r, f32 radius, f32 thickness, const Col& c);
    void circle(f32 cx, f32 cy, f32 radius, const Col& c);
    void line(f32 x0, f32 y0, f32 x1, f32 y1, f32 thickness, const Col& c);
    void triangle(f32 ax, f32 ay, f32 bx, f32 by, f32 cx_, f32 cy_, const Col& c);
    // Vertical gradient; used for faders and meter bodies.
    void vgrad(const Rect& r, const Col& top, const Col& bottom);
    // Composites an RGBA texture -- plugin editors, cached waveforms, or an
    // offscreen 3D pass. `flipY` for textures produced by an FBO.
    void image(const Rect& r, unsigned tex, const Col& tint = Col(1, 1, 1, 1), bool flipY = false);

    // Fence around drawing that uses its own shaders and GL state. Any module
    // rendering outside this batcher must sit between these two calls.
    void beginForeignPass();
    void endForeignPass();

    // -----------------------------------------------------------------------
    // The NX design language (docs/DESIGN.md). theme.h holds the tokens; these
    // are the brush strokes that put them on screen.
    //
    // Every one of these is a rounded-rect quad in the same batch as everything
    // above -- no extra draw call, no state change, no second shader. A card
    // with a shadow, a fill, a lit edge and a sheen costs five quads and zero
    // draw calls of its own.
    //
    // WHERE THESE BELONG: the chrome. Transport bar, browser, device panels,
    // modals, menus, toasts. The working surfaces -- clip grid, arrangement,
    // piano roll, meters, waveforms -- take the palette and the well language
    // from theme.h and stay flat. `well()` and `hairline*()` are for them;
    // `glass()` and `sheen()` are not.
    // -----------------------------------------------------------------------

    // Rounded rect filled with a linear or radial gradient. `alphaMul` scales
    // the whole gradient, for fading a surface in without rebuilding it.
    void gradRect(const Rect& r, f32 radius, const nx::Grad& g, f32 alphaMul = 1.f);

    // The lit edge: a rounded-rect stroke whose colour runs bright top-left to
    // dark bottom-right. This is the signature of the whole language. The rect
    // is snapped to whole device pixels and the thickness rounded to an integer
    // so it stays crisp at 1px under fractional DPI scale -- pass the rect you
    // filled and it will align itself.
    void gradStroke(const Rect& r, f32 radius, f32 thickness, const nx::Grad& g,
                    f32 alphaMul = 1.f);

    // Dividers. A gradient hairline that fades to transparent at both ends;
    // after this exists, a solid grey divider anywhere is a bug (§11).
    // `ink` gives both the hue and, through its alpha, the peak of the ramp.
    // `px` is the thickness in device pixels, snapped to a whole pixel.
    void hairlineH(f32 x0, f32 x1, f32 y, const Col& ink = nx::hairlineInk, f32 px = 1.f);
    void hairlineV(f32 x, f32 y0, f32 y1, const Col& ink = nx::hairlineInk, f32 px = 1.f);

    // Soft drop shadow under a rounded rect: one quad, SDF falloff, no blur
    // pass. `s.blur` is the CSS blur radius and `s.spread` grows or shrinks the
    // cast shape first, so the spec's shadow lists translate literally.
    void shadow(const Rect& r, f32 radius, const nx::ShadowSpec& s);
    void shadow(const Rect& r, f32 radius, const nx::Elevation& e);

    // §5's masked diagonal highlight. `phase` in [0,1] slides the band across
    // the surface; it is implemented by shifting gradient stops, so animating
    // it costs nothing beyond the one quad it already was. Frozen at reduced
    // motion -- the band simply does not draw.
    void sheen(const Rect& r, f32 radius, f32 phase, f32 intensity = 1.f);

    // A complete tier-4 surface in one call: elevation, fill, edge, in that
    // order. This is what a view should reach for; the pieces above are for the
    // cases the table does not cover.
    void glass(const Rect& r, const nx::GlassStyle& st);
    void glass(const Rect& r, nx::Tier t) { glass(r, nx::glass(t)); }
    void glass(const Rect& r, nx::Tier t, f32 radius) {
        nx::GlassStyle st = nx::glass(t);
        st.radius = radius;
        glass(r, st);
    }

    // Recessed regions inside glass. Cheap, flat, and the correct answer to
    // "this area needs to look contained" -- a second frosted layer reads as
    // fog and destroys the hierarchy (§4).
    void well(const Rect& r, f32 radius = nx::radiusSm, bool deep = false);

    // §5: never a bare outline.
    void focusRing(const Rect& r, f32 radius);

    // §3, the living background. Draws the field gradient, the drifting nebula
    // blobs, a sparse starfield and the vignette -- six quads, one batch, no
    // texture. Replaces a flat clear entirely.
    //
    // begin() calls this itself unless it has been switched off, so the app
    // needs no call site; setBackground(false) restores a flat field for a
    // surface that wants to own its own backdrop.
    void background(f64 timeSeconds);
    void setBackground(bool on) { bgOn_ = on; }
    bool backgroundOn() const { return bgOn_; }

    // Seconds since the renderer came up. The clock the background and the
    // specimen animate on; a view is welcome to use it for a sheen phase.
    f64 time() const;

    // §4's blur, synthesised. THIS IS NOT A COMPOSITOR BLUR and does not
    // pretend to be one: `realBlur()` returns false, permanently, and the
    // reasoning is written out at frost()'s definition in renderer.cpp.
    //
    // What it does is the job §4 actually assigns to blur on a floating
    // surface -- deepen what is behind it so text stays legible over busy
    // content. glass() applies it for the tiers whose blurPx is non-zero, so a
    // view normally never calls this directly.
    void frost(const Rect& r, f32 radius, f32 strengthPx);
    static bool realBlur() { return false; }
    int  frostRegions() const { return frostCount_; }

    // Text. `y` is the top of the line box; the baseline is derived.
    f32  text(const Font& f, f32 x, f32 y, const char* s, const Col& c, int len = -1);
    f32  textIn(const Font& f, const Rect& r, const char* s, const Col& c,
                Align a = Align::Left, f32 padX = 4.f);

    // Scissor stack. Pushing intersects with the current clip.
    void pushClip(const Rect& r);
    void popClip();
    Rect currentClip() const { return clips_.empty() ? Rect{0, 0, (f32)vw_, (f32)vh_} : clips_.back(); }

    int  width()  const { return vw_; }
    int  height() const { return vh_; }
    f32  dpiScale() const { return dpi_; }
    int  drawCalls() const { return drawCalls_; }
    // Quads emitted this frame. The other half of the status bar's perf
    // instrument: draw calls say how well the batch is holding together, this
    // says how much work the batch is actually carrying.
    int  quads() const { return quadsFrame_; }

private:
    // k0/k1 are two mode-specific extras. Only the drop shadow uses them so
    // far -- it needs its element's spread and x-offset to reconstruct the
    // border box it must not paint inside -- but they cost 8 bytes on a
    // 64-byte vertex and they are the slack that kept the shadow from needing
    // a second shader.
    struct Vtx { f32 x, y, u, v, r, g, b, a, lx, ly, hw, hh, rad, mode, k0, k1; };

    void flush();
    void quad(const Rect& r, f32 rad, f32 mode, const Col& c,
              f32 u0 = 0, f32 v0 = 0, f32 u1 = 0, f32 v1 = 0,
              f32 k0 = 0, f32 k1 = 0);
    // As quad(), but with an explicit outward padding in pixels. The SDF modes
    // need the geometry to extend past the shape they describe, and a drop
    // shadow needs it to extend by its whole blur radius rather than the one
    // pixel an antialiased edge wants.
    void quadPad(const Rect& r, f32 pad, f32 rad, f32 mode, const Col& c,
                 f32 u0 = 0, f32 v0 = 0, f32 u1 = 0, f32 v1 = 0,
                 f32 k0 = 0, f32 k1 = 0);
    void useTexture(unsigned t);

    // Interns a gradient into the GPU-side parameter table and returns its row.
    // Identical gradients share a row, so a token used two hundred times in a
    // frame costs one row and no extra state.
    int  gradSlot(const nx::Grad& g);

    std::vector<Vtx> verts_;
    std::vector<Rect> clips_;
    unsigned vao_ = 0, vbo_ = 0, prog_ = 0, curTex_ = 0;
    int vw_ = 0, vh_ = 0;
    int uViewport_ = -1, uTex_ = -1, uGrad_ = -1;
    f32 dpi_ = 1.f;
    int drawCalls_ = 0;
    int quadsFrame_ = 0;

    // --- gradient parameter table ------------------------------------------
    // Eight RGBA32F texels per gradient, one gradient per row, sampled with
    // texelFetch from texture unit 1. Unit 1 rather than a uniform block
    // because the batcher already owns unit 0 for the glyph atlas, and rather
    // than per-vertex attributes because carrying five stops on every vertex
    // would triple the size of every quad in the program to serve the handful
    // that are gradients.
    static constexpr int kGradTexels = 8;
    static constexpr int kMaxGrads   = 256;
    std::vector<f32> gradData_;
    std::vector<u64> gradHash_;
    unsigned gradTex_ = 0;
    int gradCount_ = 0;

    bool bgOn_ = true;
    int  frostCount_ = 0;
};

} // namespace lat
