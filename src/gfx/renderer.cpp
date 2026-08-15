#include "renderer.h"
#include "specimen.h"
#include "gl.h"
#include <chrono>
#include <cstring>

// The gradient table is an RGBA32F texture on unit 1. These enums are GL 1.3 /
// 3.0, but the Windows path includes a 1.1 gl.h and resolves the rest through a
// loader, so define them defensively rather than assume the header has them.
#ifndef GL_TEXTURE1
#define GL_TEXTURE1 0x84C1
#endif
#ifndef GL_RGBA32F
#define GL_RGBA32F 0x8814
#endif

namespace lat {

// Mode encoding carried per-vertex:
//   0 = rounded-rect fill (SDF)
//   1 = glyph (atlas red channel as coverage)
//   2 = flat, no SDF (triangles, gradients, diagonal lines)
//   3 = rounded-rect outline (thickness travels in the u slot)
//   4 = full RGBA texture, tinted by vCol. This is the compositing path for
//       anything rendered outside this batcher -- plugin editors, waveform
//       caches, or a future 3D module drawing into its own FBO.
//
// The NX design language adds four (docs/DESIGN.md). All four are still the
// same quad, the same vertex layout and the same batch -- what they need beyond
// modes 0-4 is *parameters*, and parameters that would not fit on a vertex live
// in a small texture instead of on every vertex in the program:
//
//   5 = rounded-rect filled with a gradient. u.x carries the gradient's row in
//       the parameter table; the table row says linear-or-radial, the angle,
//       and up to five stops. vCol.a scales the whole thing.
//   6 = rounded-rect gradient STROKE -- the lit edge. u.x row, u.y thickness.
//   7 = drop shadow. A smootherstep on the rounded-rect SDF, which is close
//       enough to a Gaussian's integral that the eye cannot tell, for the cost
//       of one quad and no blur pass. u.x carries the falloff half-width.
//   8 = starfield and vignette, together, in one full-screen quad. Together
//       because they composite against each other (stars sit BEHIND the
//       vignette) and doing them separately would cost a second full-screen
//       pass to add light that the next pass immediately takes away.
//
// Modes 5 and 6 are the reason there is a gradient table at all. The
// alternative -- five stop colours, five positions and an angle on every
// vertex -- would take the vertex from 14 floats to 40 and charge every quad in
// the piano roll for a feature only the chrome uses.
static const char* kVert = R"(#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in vec4 aCol;
layout(location=3) in vec4 aLocal;   // lx, ly, halfW, halfH
layout(location=4) in vec4 aParm;    // radius, mode, and two mode-specific extras
uniform vec2 uViewport;
out vec2 vUV; out vec4 vCol; out vec4 vLocal; out vec4 vParm;
void main() {
    vec2 ndc = vec2(aPos.x / uViewport.x * 2.0 - 1.0,
                    1.0 - aPos.y / uViewport.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUV = aUV; vCol = aCol; vLocal = aLocal; vParm = aParm;
}
)";

static const char* kFrag = R"(#version 330 core
in vec2 vUV; in vec4 vCol; in vec4 vLocal; in vec4 vParm;
uniform sampler2D uTex;
uniform sampler2D uGrad;
out vec4 fragColor;

float sdRoundRect(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// Interleaved-gradient noise, +-half a code value. The field gradient crosses
// six 8-bit code values over a thousand pixels; without this it bands into
// stripes you cannot un-see, and banding is exactly what makes a dark UI look
// cheap. One fract chain, no transcendentals.
float dither(vec2 p) {
    return (fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))) - 0.5) / 255.0;
}

// Cheap integer-ish hash. Deliberately NOT the sin(dot(...)) idiom: this runs
// once per screen pixel for the starfield, and three transcendentals per
// fragment across a full-screen quad is real money for a decoration.
float hash1(vec2 p) {
    vec3 q = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}

// Row layout, 8 RGBA32F texels:
//   0..4  stop colours, straight alpha
//   5     stop positions 0..3
//   6     stop position 4 | angle (radians) | radial flag | stop count
//   7     radial centre and radii, as fractions of the rect
//
// Only the two stops bracketing t are fetched, so a five-stop hairline costs
// the same four texture reads as a two-stop well.
vec4 gradAt(float slotf, vec2 p, vec2 hs) {
    int s = int(slotf + 0.5);
    vec4 m = texelFetch(uGrad, ivec2(6, s), 0);
    float t;
    if (m.z > 0.5) {
        vec4 rp = texelFetch(uGrad, ivec2(7, s), 0);
        vec2 c  = (rp.xy - 0.5) * 2.0 * hs;
        vec2 rr = max(rp.zw * 2.0 * hs, vec2(1e-4));
        t = length((p - c) / rr);
    } else {
        // CSS angle convention: 0 points to the top, 90 to the right. The
        // gradient line is scaled to span the box corner to corner, which is
        // what makes a 157deg fill look the same on a wide bar and a tall card.
        vec2 dir = vec2(sin(m.y), -cos(m.y));
        float L = abs(2.0 * hs.x * dir.x) + abs(2.0 * hs.y * dir.y);
        t = 0.5 + dot(p, dir) / max(L, 1e-4);
    }
    vec4 ps = texelFetch(uGrad, ivec2(5, s), 0);
    float pp[5] = float[5](ps.x, ps.y, ps.z, ps.w, m.x);
    int n = int(m.w + 0.5);
    int hi = n - 1;
    for (int i = 1; i < 5; ++i) { if (i < n && t <= pp[i]) { hi = i; break; } }
    int lo = hi - 1;
    if (t <= pp[0] || hi <= 0) { lo = 0; hi = 0; }
    vec4 ca = texelFetch(uGrad, ivec2(lo, s), 0);
    vec4 cb = texelFetch(uGrad, ivec2(hi, s), 0);
    float f = (hi > lo) ? clamp((t - pp[lo]) / max(pp[hi] - pp[lo], 1e-5), 0.0, 1.0) : 0.0;
    // Premultiplied interpolation, as CSS does it. Interpolating straight alpha
    // across a stop that fades to nothing drags that stop's RGB into the
    // result; on --sheen, whose ends are transparent white, that greys the band
    // instead of dissolving it.
    vec4 a = vec4(ca.rgb * ca.a, ca.a);
    vec4 b = vec4(cb.rgb * cb.a, cb.a);
    vec4 res = mix(a, b, f);
    return vec4(res.rgb / max(res.a, 1e-5), res.a);
}

void main() {
    int mode = int(vParm.y + 0.5);
    if (mode == 1) {
        float cov = texture(uTex, vUV).r;
        fragColor = vec4(vCol.rgb, vCol.a * cov);
    } else if (mode == 2) {
        fragColor = vCol;
    } else if (mode == 3) {
        float d = sdRoundRect(vLocal.xy, vLocal.zw, vParm.x);
        float t = vUV.x;
        float ad = abs(d + t * 0.5) - t * 0.5;
        float a = clamp(0.5 - ad, 0.0, 1.0);
        fragColor = vec4(vCol.rgb, vCol.a * a);
    } else if (mode == 4) {
        fragColor = texture(uTex, vUV) * vCol;
    } else if (mode == 5) {
        float d = sdRoundRect(vLocal.xy, vLocal.zw, vParm.x);
        float a = clamp(0.5 - d, 0.0, 1.0);
        vec4 g = gradAt(vUV.x, vLocal.xy, vLocal.zw);
        fragColor = vec4(g.rgb + dither(gl_FragCoord.xy), g.a * a * vCol.a);
    } else if (mode == 6) {
        float d = sdRoundRect(vLocal.xy, vLocal.zw, vParm.x);
        float th = vUV.y;
        float ad = abs(d + th * 0.5) - th * 0.5;
        float a = clamp(0.5 - ad, 0.0, 1.0);
        vec4 g = gradAt(vUV.x, vLocal.xy, vLocal.zw);
        fragColor = vec4(g.rgb, g.a * a * vCol.a);
    } else if (mode == 7) {
        // Smootherstep across +-b of the surface. The integral of a Gaussian
        // is an S-curve of exactly this shape to within a couple of percent,
        // and the difference is invisible under a 72%-black shadow.
        float d = sdRoundRect(vLocal.xy, vLocal.zw, vParm.x);
        float b = max(vUV.x, 0.5);
        float x = clamp(0.5 - d / (2.0 * b), 0.0, 1.0);
        float a = x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
        // CSS clips an outer shadow to OUTSIDE the element's border box, and
        // that clip is not cosmetic here: these fills are 9%-alpha glass, so
        // an unclipped 72%-black layer shows straight through and every card
        // sits in its own dark puddle -- and --shadow-lift's violet glow
        // floods the card it is supposed to be lighting from beneath. The
        // element is reconstructed in the shadow's own frame from the spread
        // and offset the CPU packed alongside.
        float sp = vParm.z;
        vec2 off = vec2(vParm.w, vUV.y);
        float de = sdRoundRect(vLocal.xy + off, max(vLocal.zw - sp, vec2(0.0)),
                               max(vParm.x - sp, 0.0));
        a *= clamp(0.5 + de, 0.0, 1.0);
        fragColor = vec4(vCol.rgb, vCol.a * a);
    } else if (mode == 8) {
        // Stars: one candidate per grid cell, kept or dropped by a hash, so the
        // field is sparse, deterministic and costs no vertices. Jitter is held
        // inside the middle 70% of the cell so a star never gets clipped by the
        // boundary of the only cell that draws it.
        vec2 sp = vLocal.xy + vLocal.zw;
        float cs = max(vUV.x, 4.0);
        vec2 cell = floor(sp / cs);
        // One hash, three uses: keep/drop, jitter, brightness. Three separate
        // hashes read no better and cost three times as much.
        float h = hash1(cell);
        float keep = step(0.82, fract(h * 41.0));
        vec2 jit = vec2(0.15) + 0.7 * vec2(fract(h * 7.0), fract(h * 17.0));
        float bright = 0.30 + 0.70 * fract(h * 3.0);
        float dd = length(sp - (cell + jit) * cs);
        float sa = keep * bright * (1.0 - smoothstep(0.0, 0.55 + bright * 0.75, dd)) * vCol.a;
        // Vignette, over the stars: edges stay darker than centre (§3).
        vec2 q = vLocal.xy / max(vLocal.zw, vec2(1.0));
        float va = vUV.y * smoothstep(0.30, 1.02, length(q) / 1.24);
        float a = va + sa * (1.0 - va);
        fragColor = vec4(vCol.rgb * sa * (1.0 - va) / max(a, 1e-5), a);
    } else {
        float d = sdRoundRect(vLocal.xy, vLocal.zw, vParm.x);
        float a = clamp(0.5 - d, 0.0, 1.0);
        fragColor = vec4(vCol.rgb, vCol.a * a);
    }
    if (fragColor.a <= 0.0) discard;
}
)";

static unsigned compile(unsigned type, const char* src) {
    unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        LOGE("shader compile failed:\n%s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

bool Renderer::init() {
    // No loader call here: on Linux libGL exports the core profile directly,
    // and on Windows the Win32 backend resolves the entry points before this
    // runs. Verify we really have a 3.3 context before touching anything else.
    const char* ver = (const char*)glGetString(GL_VERSION);
    if (!ver) { LOGE("no current OpenGL context"); return false; }

    const unsigned vs = compile(GL_VERTEX_SHADER, kVert);
    const unsigned fs = compile(GL_FRAGMENT_SHADER, kFrag);
    if (!vs || !fs) return false;
    prog_ = glCreateProgram();
    glAttachShader(prog_, vs);
    glAttachShader(prog_, fs);
    glLinkProgram(prog_);
    int ok = 0;
    glGetProgramiv(prog_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(prog_, sizeof log, nullptr, log);
        LOGE("shader link failed:\n%s", log);
        return false;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    uViewport_ = glGetUniformLocation(prog_, "uViewport");
    uTex_      = glGetUniformLocation(prog_, "uTex");
    uGrad_     = glGetUniformLocation(prog_, "uGrad");

    // The gradient parameter table: kMaxGrads rows of kGradTexels RGBA32F
    // texels, NEAREST-filtered because every read is a texelFetch of an exact
    // cell. It is uploaded once per flush, so a frame that draws no gradient
    // uploads nothing.
    gradData_.assign((size_t)kMaxGrads * kGradTexels * 4, 0.f);
    gradHash_.assign((size_t)kMaxGrads, 0);
    glGenTextures(1, &gradTex_);
    glBindTexture(GL_TEXTURE_2D, gradTex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, kGradTexels, kMaxGrads, 0,
                 GL_RGBA, GL_FLOAT, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    const GLsizei stride = sizeof(Vtx);
    auto attr = [&](int idx, int n, size_t off) {
        glEnableVertexAttribArray((GLuint)idx);
        glVertexAttribPointer((GLuint)idx, n, GL_FLOAT, GL_FALSE, stride, (void*)off);
    };
    attr(0, 2, offsetof(Vtx, x));
    attr(1, 2, offsetof(Vtx, u));
    attr(2, 4, offsetof(Vtx, r));
    attr(3, 4, offsetof(Vtx, lx));
    attr(4, 4, offsetof(Vtx, rad));
    glBindVertexArray(0);

    verts_.reserve(64 * 1024);

    // NXTAKT_NX_BACKGROUND=0 restores the flat clear. This exists so the cost
    // of §3 can be measured against its own absence on the same binary rather
    // than estimated -- which is the only way the "handful of quads" claim in
    // background() is worth anything.
    if (const char* s = env("NX_BACKGROUND")) bgOn_ = (s[0] != '0');

    LOGI("renderer up: %s / %s", (const char*)glGetString(GL_RENDERER),
         (const char*)glGetString(GL_VERSION));
    LOGI("nx: background=%d reduced-motion=%d real-blur=%d specimen=%d",
         (int)bgOn_, (int)nx::reducedMotion(), (int)realBlur(), (int)nx::specimenEnabled());
    return true;
}

void Renderer::shutdown() {
    specimenShutdown();
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (prog_) glDeleteProgram(prog_);
    if (gradTex_) glDeleteTextures(1, &gradTex_);
    vbo_ = vao_ = prog_ = gradTex_ = 0;
}

// One monotonic clock for the whole design language: the nebula's drift, the
// sheen's phase and the specimen's animations all read it, so they cannot get
// out of step with each other or restart when a view is rebuilt.
static f64 monoClock() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point t0 = clock::now();
    return std::chrono::duration<f64>(clock::now() - t0).count();
}

f64 Renderer::time() const { return monoClock(); }

// ---------------------------------------------------------------------------
// NXTAKT_GFX_STATS=1 -- the perf instrument for §3 and §4.
//
// The status bar's fps counter is the right instrument for "is the program
// fast", but it is useless for "what does the background cost": under any
// compositor the swap is vsync-capped, so a pass that takes two milliseconds
// and one that takes zero both read 60. This measures the pass directly, with
// glFinish on either side.
//
// glFinish is a blunt instrument -- it drains the pipeline and so inflates the
// FRAME number by whatever parallelism it destroyed. That is fine and it is why
// only the background number is quoted: fencing a pass and timing it end to end
// gives an honest upper bound on that pass, which is the number worth having.
// It is off unless asked for, and costs nothing when off.
// ---------------------------------------------------------------------------
namespace {
struct GfxStats {
    bool on = false;
    int  frames = 0;
    f64  bgSum = 0, frameSum = 0, bgMax = 0;
    long quadSum = 0, drawSum = 0;

    static GfxStats& get() {
        static GfxStats s = [] {
            GfxStats v;
            const char* e = env("GFX_STATS");
            v.on = e && *e && e[0] != '0';
            return v;
        }();
        return s;
    }
    void report(int w, int h) {
        if (frames < 120) return;
        LOGI("gfx: %dx%d  background %.3f ms (peak %.3f)  frame %.3f ms  "
             "%ld quads  %ld draws  [glFinish-fenced, %d frames]",
             w, h, bgSum / frames * 1e3, bgMax * 1e3, frameSum / frames * 1e3,
             quadSum / frames, drawSum / frames, frames);
        frames = 0; bgSum = frameSum = bgMax = 0; quadSum = drawSum = 0;
    }
};
f64 g_frameT0 = 0;
} // namespace

void Renderer::begin(int w, int h, f32 dpiScale) {
    vw_ = w; vh_ = h; dpi_ = dpiScale;
    drawCalls_ = 0;
    quadsFrame_ = 0;
    frostCount_ = 0;
    verts_.clear();
    clips_.clear();
    curTex_ = 0;
    // gradCount_ deliberately NOT reset: the gradient table persists across
    // flushes AND frames. See flush() for why.

    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(prog_);
    glUniform2f(uViewport_, (f32)w, (f32)h);
    glUniform1i(uTex_, 0);
    glUniform1i(uGrad_, 1);

    GfxStats& st = GfxStats::get();
    if (st.on) { glFinish(); g_frameT0 = monoClock(); }

    // §3, first thing in the frame. The background is queued, not drawn, so a
    // caller that still clears the framebuffer after begin() -- as App does --
    // is harmless: the clear hits the framebuffer, these quads flush later and
    // land on top of it, opaque. Dropping that clear is a tidy-up, not a fix.
    if (bgOn_) background(monoClock());

    if (st.on) {
        // Fence the background on its own so the number below is that pass and
        // nothing else. In a normal build these quads would just sit in the
        // batch and cost one more flush at the end of the frame.
        flush();
        glFinish();
        const f64 dt = monoClock() - g_frameT0;
        st.bgSum += dt;
        if (dt > st.bgMax) st.bgMax = dt;
    }
}

void Renderer::end() {
    // NXTAKT_DEBUG_GLASS=1 only. Drawn here rather than from a view because the
    // point of the specimen is to be available over ANY view, unmodified, and
    // because a vocabulary sheet that needs a call site is a vocabulary sheet
    // nobody looks at.
    if (nx::specimenEnabled()) {
        flush();
        glDisable(GL_SCISSOR_TEST);
        clips_.clear();
        drawSpecimen(*this, monoClock());
    }
    flush();
    GfxStats& st = GfxStats::get();
    if (st.on) {
        glFinish();
        st.frameSum += monoClock() - g_frameT0;
        st.quadSum += quadsFrame_;
        st.drawSum += drawCalls_;
        ++st.frames;
        st.report(vw_, vh_);
    }
    glDisable(GL_SCISSOR_TEST);
    glBindVertexArray(0);
    glUseProgram(0);
}

void Renderer::flush() {
    if (verts_.empty()) return;
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts_.size() * sizeof(Vtx)), verts_.data(), GL_STREAM_DRAW);

    // Upload only the gradient rows added since the last upload. The table
    // used to reset every flush, which re-interned and re-uploaded the same
    // handful of theme tokens on every gradient-carrying batch -- measured at
    // ~8 us of driver sync apiece, and after the chrome re-skin that was ~20
    // batches a frame: the whole visible cost of the design language was this
    // one redundant call. The tokens are constants, so a persistent table
    // converges within a frame or two and steady state uploads NOTHING.
    if (gradDirtyFrom_ < gradCount_) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, gradTex_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, gradDirtyFrom_,
                        kGradTexels, gradCount_ - gradDirtyFrom_,
                        GL_RGBA, GL_FLOAT,
                        gradData_.data() + (size_t)gradDirtyFrom_ * kGradTexels * 4);
        gradDirtyFrom_ = gradCount_;
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, curTex_);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)verts_.size());
    verts_.clear();
    ++drawCalls_;
}

// Gradients are interned into a PERSISTENT table: a token used two hundred
// times occupies one row, forever, and only a row's first appearance is ever
// uploaded. The table is small enough that a linear scan of 64-bit hashes
// beats anything with a bucket in it. An animated gradient (a per-frame
// .shifted()/.rotated() derivative) hashes fresh each frame and will fill the
// table over time; the overflow path below makes that a periodic one-flush
// hiccup rather than an error -- but a view that wants an animated look should
// prefer a phase PARAMETER (as sheen() does) over mutating the gradient.
static u64 hashGrad(const nx::Grad& g) {
    u64 h = 1469598103934665603ull;
    auto mix = [&h](f32 v) {
        u32 b = 0;
        std::memcpy(&b, &v, sizeof b);
        h ^= b;
        h *= 1099511628211ull;
    };
    mix((f32)g.n);
    mix(g.angleDeg);
    mix(g.radial ? 1.f : 0.f);
    mix(g.cx); mix(g.cy); mix(g.rx); mix(g.ry);
    for (int i = 0; i < g.n && i < nx::Grad::kMaxStops; ++i) {
        mix(g.stops[i].c.r); mix(g.stops[i].c.g);
        mix(g.stops[i].c.b); mix(g.stops[i].c.a);
        mix(g.stops[i].pos);
    }
    return h;
}

int Renderer::gradSlot(const nx::Grad& g) {
    const u64 h = hashGrad(g);
    for (int i = 0; i < gradCount_; ++i)
        if (gradHash_[(size_t)i] == h) return i;
    // Overflow: flush what is queued, then start the table over. Costs one
    // draw call and one full re-warm; only reachable by animated gradients.
    if (gradCount_ >= kMaxGrads) {
        flush();
        gradCount_ = 0;
        gradDirtyFrom_ = 0;
    }

    const int slot = gradCount_++;
    gradHash_[(size_t)slot] = h;
    f32* d = gradData_.data() + (size_t)slot * kGradTexels * 4;
    const int n = clampv(g.n, 1, nx::Grad::kMaxStops);
    for (int i = 0; i < nx::Grad::kMaxStops; ++i) {
        const Col& c = g.stops[i < n ? i : n - 1].c;
        d[i * 4 + 0] = c.r; d[i * 4 + 1] = c.g; d[i * 4 + 2] = c.b; d[i * 4 + 3] = c.a;
    }
    for (int i = 0; i < 4; ++i) d[20 + i] = g.stops[i < n ? i : n - 1].pos;
    d[24] = g.stops[4 < n ? 4 : n - 1].pos;
    d[25] = g.angleDeg * 0.017453292519943295f;
    d[26] = g.radial ? 1.f : 0.f;
    d[27] = (f32)n;
    d[28] = g.cx; d[29] = g.cy; d[30] = g.rx; d[31] = g.ry;
    return slot;
}

void Renderer::useTexture(unsigned t) {
    if (t != curTex_) { flush(); curTex_ = t; }
}

void Renderer::quad(const Rect& r, f32 rad, f32 mode, const Col& c,
                    f32 u0, f32 v0, f32 u1, f32 v1, f32 k0, f32 k1) {
    const bool sdf = (mode < 0.5f) || (mode > 2.5f);
    quadPad(r, sdf ? 1.f : 0.f, rad, mode, c, u0, v0, u1, v1, k0, k1);
}

void Renderer::quadPad(const Rect& r, f32 pad, f32 rad, f32 mode, const Col& c,
                       f32 u0, f32 v0, f32 u1, f32 v1, f32 k0, f32 k1) {
    if (r.w <= 0.f || r.h <= 0.f) return;
    ++quadsFrame_;
    const f32 hw = r.w * 0.5f, hh = r.h * 0.5f;
    const f32 x0 = r.x - pad, y0 = r.y - pad, x1 = r.right() + pad, y1 = r.bottom() + pad;
    const f32 lx0 = -hw - pad, ly0 = -hh - pad, lx1 = hw + pad, ly1 = hh + pad;
    const f32 rr = clampv(rad, 0.f, std::min(hw, hh));

    const Vtx a{x0, y0, u0, v0, c.r, c.g, c.b, c.a, lx0, ly0, hw, hh, rr, mode, k0, k1};
    const Vtx b{x1, y0, u1, v0, c.r, c.g, c.b, c.a, lx1, ly0, hw, hh, rr, mode, k0, k1};
    const Vtx d{x1, y1, u1, v1, c.r, c.g, c.b, c.a, lx1, ly1, hw, hh, rr, mode, k0, k1};
    const Vtx e{x0, y1, u0, v1, c.r, c.g, c.b, c.a, lx0, ly1, hw, hh, rr, mode, k0, k1};
    verts_.push_back(a); verts_.push_back(b); verts_.push_back(d);
    verts_.push_back(a); verts_.push_back(d); verts_.push_back(e);
}

void Renderer::rect(const Rect& r, const Col& c)                   { useTexture(0); quad(r, 0.f, 0.f, c); }
void Renderer::roundRect(const Rect& r, f32 rad, const Col& c)     { useTexture(0); quad(r, rad, 0.f, c); }
void Renderer::circle(f32 cx, f32 cy, f32 rad, const Col& c) {
    useTexture(0);
    quad({cx - rad, cy - rad, rad * 2, rad * 2}, rad, 0.f, c);
}
void Renderer::roundRectOutline(const Rect& r, f32 rad, f32 th, const Col& c) {
    useTexture(0);
    quad(r, rad, 3.f, c, th, 0.f, th, 0.f);   // thickness rides in u
}

void Renderer::image(const Rect& r, unsigned tex, const Col& tint, bool flipY) {
    useTexture(tex);
    const f32 v0 = flipY ? 1.f : 0.f, v1 = flipY ? 0.f : 1.f;
    quad(r, 0.f, 4.f, tint, 0.f, v0, 1.f, v1);
}

// --- foreign render passes -------------------------------------------------
// A module that draws with its own shaders (a plugin editor, a spectrum view,
// a 3D scene in an FBO) must be fenced off from the UI batch: the pending
// quads have to land first, and the GL state we rely on has to be restored
// afterwards rather than assumed.
void Renderer::beginForeignPass() {
    flush();
    glBindVertexArray(0);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    curTex_ = 0;
}

void Renderer::endForeignPass() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, vw_, vh_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(prog_);
    glUniform2f(uViewport_, (f32)vw_, (f32)vh_);
    glUniform1i(uTex_, 0);
    glUniform1i(uGrad_, 1);
    // Unit 1 is re-bound by the next flush; make sure the active unit is 0
    // again so every glBindTexture above this line still means what it says.
    glActiveTexture(GL_TEXTURE0);
    if (clips_.empty()) {
        glDisable(GL_SCISSOR_TEST);
    } else {
        const Rect& nr = clips_.back();
        glEnable(GL_SCISSOR_TEST);
        glScissor((int)nr.x, (int)((f32)vh_ - nr.bottom()), (int)std::max(0.f, nr.w), (int)std::max(0.f, nr.h));
    }
}

void Renderer::vgrad(const Rect& r, const Col& top, const Col& bot) {
    if (r.w <= 0.f || r.h <= 0.f) return;
    ++quadsFrame_;
    useTexture(0);
    const f32 x0 = r.x, y0 = r.y, x1 = r.right(), y1 = r.bottom();
    auto V = [&](f32 x, f32 y, const Col& c) {
        return Vtx{x, y, 0, 0, c.r, c.g, c.b, c.a, 0, 0, 0, 0, 0, 2.f};
    };
    const Vtx a = V(x0, y0, top), b = V(x1, y0, top), d = V(x1, y1, bot), e = V(x0, y1, bot);
    verts_.push_back(a); verts_.push_back(b); verts_.push_back(d);
    verts_.push_back(a); verts_.push_back(d); verts_.push_back(e);
}

void Renderer::triangle(f32 ax, f32 ay, f32 bx, f32 by, f32 cx_, f32 cy_, const Col& c) {
    ++quadsFrame_;
    useTexture(0);
    auto V = [&](f32 x, f32 y) { return Vtx{x, y, 0, 0, c.r, c.g, c.b, c.a, 0, 0, 0, 0, 0, 2.f}; };
    verts_.push_back(V(ax, ay)); verts_.push_back(V(bx, by)); verts_.push_back(V(cx_, cy_));
}

void Renderer::line(f32 x0, f32 y0, f32 x1, f32 y1, f32 th, const Col& c) {
    if (std::fabs(y1 - y0) < 0.01f) {                     // horizontal fast path
        rect({std::min(x0, x1), y0 - th * 0.5f, std::fabs(x1 - x0), th}, c);
        return;
    }
    if (std::fabs(x1 - x0) < 0.01f) {                     // vertical fast path
        rect({x0 - th * 0.5f, std::min(y0, y1), th, std::fabs(y1 - y0)}, c);
        return;
    }
    const f32 dx = x1 - x0, dy = y1 - y0;
    const f32 len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    const f32 nx = -dy / len * th * 0.5f, ny = dx / len * th * 0.5f;
    ++quadsFrame_;
    useTexture(0);
    auto V = [&](f32 x, f32 y) { return Vtx{x, y, 0, 0, c.r, c.g, c.b, c.a, 0, 0, 0, 0, 0, 2.f}; };
    const Vtx a = V(x0 + nx, y0 + ny), b = V(x1 + nx, y1 + ny);
    const Vtx d = V(x1 - nx, y1 - ny), e = V(x0 - nx, y0 - ny);
    verts_.push_back(a); verts_.push_back(b); verts_.push_back(d);
    verts_.push_back(a); verts_.push_back(d); verts_.push_back(e);
}

// ===========================================================================
// The NX design language (docs/DESIGN.md)
//
// Everything below emits ordinary quads into the same batch as rect() and
// text(). A tier-1 card -- two shadow layers, a gradient fill, a lit edge and a
// sheen -- is five quads and no draw calls of its own.
// ===========================================================================

void Renderer::gradRect(const Rect& r, f32 rad, const nx::Grad& g, f32 alphaMul) {
    if (r.w <= 0.f || r.h <= 0.f || g.n <= 0 || alphaMul <= 0.f) return;
    useTexture(0);
    const f32 slot = (f32)gradSlot(g);
    quad(r, rad, 5.f, Col(1.f, 1.f, 1.f, clampv(alphaMul, 0.f, 1.f)), slot, 0.f, slot, 0.f);
}

void Renderer::gradStroke(const Rect& r0, f32 rad, f32 th, const nx::Grad& g, f32 alphaMul) {
    if (r0.w <= 0.f || r0.h <= 0.f || g.n <= 0 || alphaMul <= 0.f) return;
    // The lit edge is the signature of the language, and a signature that
    // smears is worse than no signature. The SDF puts a 1px stroke across two
    // pixel rows at half coverage each unless the rect's bounds land on whole
    // pixels -- which, under a fractional DPI scale, they otherwise never do.
    // So snap the bounds and round the thickness, here, once, rather than
    // asking every call site to remember.
    const f32 t  = std::max(1.f, std::round(th));
    const f32 x0 = std::round(r0.x), y0 = std::round(r0.y);
    const Rect r{x0, y0,
                 std::max(t * 2.f, std::round(r0.right()) - x0),
                 std::max(t * 2.f, std::round(r0.bottom()) - y0)};
    useTexture(0);
    const f32 slot = (f32)gradSlot(g);
    quad(r, rad, 6.f, Col(1.f, 1.f, 1.f, clampv(alphaMul, 0.f, 1.f)), slot, t, slot, t);
}

// Rescale the hairline ramp so its peak becomes `ink.a`, keeping the shape.
static nx::Grad hairlineFor(const Col& ink, f32 angleDeg) {
    return nx::hairline.tinted(ink)
                       .faded(ink.a / nx::hairlinePeak)
                       .rotated(angleDeg);
}

void Renderer::hairlineH(f32 x0, f32 x1, f32 y, const Col& ink, f32 px) {
    if (x1 < x0) std::swap(x0, x1);
    const f32 a = std::round(x0), b = std::round(x1);
    if (b - a < 1.f || ink.a <= 0.f) return;
    gradRect({a, std::round(y), b - a, std::max(1.f, std::round(px))}, 0.f,
             hairlineFor(ink, 90.f));
}

void Renderer::hairlineV(f32 x, f32 y0, f32 y1, const Col& ink, f32 px) {
    if (y1 < y0) std::swap(y0, y1);
    const f32 a = std::round(y0), b = std::round(y1);
    if (b - a < 1.f || ink.a <= 0.f) return;
    gradRect({std::round(x), a, std::max(1.f, std::round(px)), b - a}, 0.f,
             hairlineFor(ink, 180.f));
}

void Renderer::shadow(const Rect& r, f32 rad, const nx::ShadowSpec& s) {
    if (s.c.a <= 0.f) return;
    // CSS order: spread grows (or shrinks) the cast shape, the offset moves it,
    // then the blur softens it. --shadow's -12px spread is what tucks the
    // shadow under the card instead of haloing it.
    const Rect sr{r.x + s.dx - s.spread, r.y + s.dy - s.spread,
                  r.w + s.spread * 2.f,  r.h + s.spread * 2.f};
    if (sr.w <= 0.f || sr.h <= 0.f) return;
    const f32 b = std::max(s.blur * 0.5f, 0.5f);
    useTexture(0);
    // u = falloff half-width, v = dy, k0 = spread, k1 = dx. The last three are
    // what the shader needs to rebuild the element's border box in the shadow's
    // own frame and refuse to paint inside it.
    quadPad(sr, b + 1.f, std::max(0.f, rad + s.spread), 7.f, s.c,
            b, s.dy, b, s.dy, s.spread, s.dx);
}

void Renderer::shadow(const Rect& r, f32 rad, const nx::Elevation& e) {
    for (int i = 0; i < e.n && i < 3; ++i) shadow(r, rad, e.layers[i]);
}

void Renderer::sheen(const Rect& r, f32 rad, f32 phase, f32 intensity) {
    // §6: sheens off under reduced motion. Not dimmed -- off.
    if (nx::reducedMotion() || intensity <= 0.f) return;
    // The band lives between 0.30 and 0.68 of the gradient axis; shifting every
    // stop carries it from fully off one side to fully off the other. Stops are
    // allowed outside [0,1] -- the shader holds the end colours, both of which
    // are transparent -- so this is a pure parameter change with no geometry
    // and no clipping, which is what §5's "transform or background-position
    // only" means in a renderer that has neither.
    const f32 p = clampv(phase, 0.f, 1.f);
    gradRect(r, rad, nx::sheenBand.shifted(-0.80f + p * 1.62f),
             clampv(intensity, 0.f, 1.f));
}

// ---------------------------------------------------------------------------
// THE BLUR DECISION (§4), stated plainly because it is a deliberate cut.
//
// There is no real backdrop blur here, and `realBlur()` returns false so
// nothing downstream can believe otherwise.
//
// It is not that it could not be built: the foreign-pass fence is exactly the
// seam for it, and the shape is standard -- glCopyTexImage2D the default
// framebuffer, downsample to a quarter, two separable Gaussian passes, then
// composite the result through a rounded-rect mask. Roughly 200 lines, a second
// shader program and two FBOs that have to be resized with the window.
//
// It was cut because against THIS backdrop it buys nothing measurable:
//
//   * A Gaussian blur is a low-pass filter, and §3's backdrop has no high
//     frequencies to remove. It is a vertical gradient plus three radial blobs
//     a thousand pixels across. Blurring it returns it. The only high-frequency
//     component is the starfield, and stars are decoration at 0.55 alpha.
//   * The Bar tier -- the tier most obviously "supposed to" have blur -- sits
//     at the top edge of the window with nothing but that backdrop behind it.
//     For the Bar, real blur is provably a no-op with a framebuffer copy
//     attached.
//   * The Sheet tier does float over busy content, and there the blur would be
//     visible. But §4 already answers that: menus and toasts carry legibility
//     in their fill alpha, and blur is finish, not mechanism. At the >=0.9
//     effective alpha a sheet is required to have, the backdrop contributes
//     under a tenth of the result -- less than the dither this shader already
//     adds to every gradient.
//   * The cost is not amortisable. A copy of a 1600x1000 framebuffer is 6 MB
//     across the bus plus a pipeline stall, every frame a sheet is open, in a
//     program whose status bar advertises its frame rate to the user.
//
// What ships instead is frost(): the part of the effect that actually does
// work. It deepens the region behind a floating surface so text over it stays
// comfortable, with a 165deg ramp so the light still arrives from the upper
// left. It is one quad. If someone later wants the real thing, the seam is
// here and this comment is the specification.
// ---------------------------------------------------------------------------
void Renderer::frost(const Rect& r, f32 rad, f32 strengthPx) {
    if (r.w <= 0.f || r.h <= 0.f) return;
    const f32 k = clampv(strengthPx / 34.f, 0.f, 1.f);
    static constexpr nx::Grad kFrost = {
        {{rgba(0x120B22, 0.30f), 0.f}, {rgba(0x08050F, 0.46f), 1.f}}, 2, 165.f};
    gradRect(r, rad, kFrost, 0.42f + 0.58f * k);
    ++frostCount_;
}

void Renderer::glass(const Rect& r, const nx::GlassStyle& st) {
    if (r.w <= 0.f || r.h <= 0.f) return;
    shadow(r, st.radius, st.elev);
    if (st.blurPx > 0.f) frost(r, st.radius, st.blurPx);
    if (st.fill) gradRect(r, st.radius, *st.fill);
    switch (st.edgeKind) {
    case nx::EdgeKind::Gradient:
        if (st.edgeGrad) gradStroke(r, st.radius, dpi_, *st.edgeGrad);
        break;
    case nx::EdgeKind::TopOnly: {
        // A hairline rather than a solid row, so even the Bar's top highlight
        // fades out at the window edges instead of stopping dead (§11).
        const f32 inset = std::min(st.radius, r.w * 0.5f);
        if (r.w - inset * 2.f > 1.f)
            hairlineH(r.x + inset, r.right() - inset, r.y, st.edgeFlat, dpi_);
        break;
    }
    case nx::EdgeKind::Flat:
        roundRectOutline(r, st.radius, std::max(1.f, std::round(dpi_)), st.edgeFlat);
        break;
    case nx::EdgeKind::None:
        break;
    }
}

void Renderer::well(const Rect& r, f32 rad, bool deep) {
    gradRect(r, rad, deep ? nx::wellDeep : nx::well);
}

void Renderer::focusRing(const Rect& r, f32 rad) {
    // Drawn AFTER the surface it rings, unlike the shadow layers, so a caller
    // does not have to know that a focus ring is a box-shadow in CSS.
    const f32 s = std::max(1.f, dpi_);
    roundRectOutline(r.inset(-3.5f * s), rad + 3.5f * s, 3.f * s, nx::violet.alpha(0.20f));
    roundRectOutline(r.inset(-1.0f * s), rad + 1.0f * s, 2.f * s, nx::violet.alpha(0.60f));
}

// ---------------------------------------------------------------------------
// §3  The living background
//
// Six quads, one batch, no texture, no second pass:
//
//   1  the field gradient, #0a0714 -> #12091f, dithered so it does not band
//   2  violet nebula, upper left, 96 s period, clockwise
//   3  cyan nebula, lower right, 74 s period, anticlockwise
//   4  magenta nebula, upper middle, 110 s period, clockwise
//   5  starfield and vignette, computed together in one full-screen pass
//
// The blobs move by translating their quads, which is the renderer's equivalent
// of a transform-only keyframe: no geometry is rebuilt, no gradient is
// re-interned, nothing is re-uploaded. Under NXTAKT_REDUCED_MOTION=1 the clock
// is pinned to zero and they simply stop.
//
// Violet leads and cyan is subordinate by a factor of well over two in alpha --
// the cyan blob is a light source behind glass, not a second brand colour.
// ---------------------------------------------------------------------------
void Renderer::background(f64 timeSeconds) {
    const f32 W = (f32)vw_, H = (f32)vh_;
    if (W <= 1.f || H <= 1.f) return;
    const Rect full{0.f, 0.f, W, H};

    gradRect(full, 0.f, nx::field);

    const f32 t = nx::reducedMotion() ? 0.f : (f32)timeSeconds;
    const f32 kTau = 6.283185307179586f;
    const f32 D = std::max(W, H);

    auto blob = [&](const nx::Grad& g, f32 fx, f32 fy, f32 rad,
                    f32 periodS, f32 ampX, f32 ampY, f32 phase, f32 dir) {
        const f32 a  = t * kTau / periodS * dir + phase;
        const f32 cx = W * fx + std::sin(a) * W * ampX;
        const f32 cy = H * fy + std::cos(a) * H * ampY;
        gradRect({cx - rad, cy - rad, rad * 2.f, rad * 2.f}, rad, g);
    };
    // Radii are a perf knob as much as a look knob: a blob quad is fragment
    // work over its whole on-screen footprint, and at 0.62 * D the violet one
    // alone covered the entire window. These were measured down (see
    // NXTAKT_GFX_STATS) until the whole pass fit comfortably inside a frame,
    // and the blobs still read as enormous because they are anchored off the
    // corners and only ever show one shoulder.
    blob(nx::nebulaViolet,  0.16f, 0.10f, D * 0.50f,  96.f, 0.055f, 0.045f, 0.0f, +1.f);
    blob(nx::nebulaCyan,    0.88f, 0.92f, D * 0.42f,  74.f, 0.050f, 0.040f, 1.1f, -1.f);
    blob(nx::nebulaMagenta, 0.64f, 0.26f, D * 0.30f, 110.f, 0.045f, 0.035f, 2.3f, +1.f);

    // Stars behind the vignette, in one pass. Cell size scales with DPI so the
    // density is per visual area rather than per pixel.
    useTexture(0);
    const f32 cell = 26.f * std::max(1.f, dpi_);
    quad(full, 0.f, 8.f, nx::starTint, cell, 0.44f, cell, 0.44f);
}

f32 Renderer::text(const Font& f, f32 x, f32 y, const char* s, const Col& c, int len) {
    if (!s || !*s) return x;
    useTexture(f.tex());
    const f32 baseline = std::round(y + f.ascent());
    f32 pen = std::round(x);
    for (int i = 0; (len < 0 ? s[i] != 0 : i < len); ++i) {
        const Glyph& g = f.glyph((u8)s[i]);
        if (g.valid && g.w > 0.f && g.h > 0.f) {
            const Rect r{pen + g.bearingX, baseline - g.bearingY, g.w, g.h};
            quad(r, 0.f, 1.f, c, g.u0, g.v0, g.u1, g.v1);
        }
        pen += g.advance;
    }
    return pen;
}

f32 Renderer::textIn(const Font& f, const Rect& r, const char* s, const Col& c, Align a, f32 padX) {
    if (!s || !*s) return r.x;
    const f32 avail = r.w - padX * 2.f;
    if (avail <= 1.f) return r.x;
    bool ell = false;
    const int n = f.fitLength(s, avail, &ell);
    const f32 w = f.measure(s, n) + (ell ? f.measure("...") : 0.f);
    f32 x = r.x + padX;
    if (a == Align::Center) x = r.x + (r.w - w) * 0.5f;
    else if (a == Align::Right) x = r.right() - padX - w;
    const f32 y = r.y + (r.h - f.height()) * 0.5f;
    f32 end = text(f, x, y, s, c, n);
    if (ell) end = text(f, end, y, "...", c);
    return end;
}

void Renderer::pushClip(const Rect& r) {
    const Rect cur = currentClip();
    const Rect nr = clips_.empty() ? r : r.intersect(cur);
    clips_.push_back(nr);
    flush();
    glEnable(GL_SCISSOR_TEST);
    glScissor((int)nr.x, (int)((f32)vh_ - nr.bottom()), (int)std::max(0.f, nr.w), (int)std::max(0.f, nr.h));
}

void Renderer::popClip() {
    if (clips_.empty()) return;
    clips_.pop_back();
    flush();
    if (clips_.empty()) {
        glDisable(GL_SCISSOR_TEST);
    } else {
        const Rect& nr = clips_.back();
        glScissor((int)nr.x, (int)((f32)vh_ - nr.bottom()), (int)std::max(0.f, nr.w), (int)std::max(0.f, nr.h));
    }
}

} // namespace lat
