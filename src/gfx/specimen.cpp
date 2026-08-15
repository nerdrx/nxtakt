#include "specimen.h"
#include "renderer.h"
#include <cmath>
#include <cstdio>

namespace lat {
namespace {

// The specimen owns its fonts. It has to: it draws from inside the renderer,
// below every view, and the whole value of the hook is that it works over an
// unmodified app with no call site to thread a Font& through.
struct Fonts {
    Font micro, second, body, title;
    int  atScale = 0;
    bool ok = false;

    void ensure(f32 dpi) {
        const int key = (int)std::lround(dpi * 100.f);
        if (ok && key == atScale) return;
        destroy();
        atScale = key;
        const std::string reg = findSystemFont(false);
        const std::string bold = findSystemFont(true);
        if (reg.empty()) return;
        const char* b = bold.empty() ? reg.c_str() : bold.c_str();
        auto px = [dpi](int n) { return std::max(7, (int)std::lround((f32)n * dpi)); };
        ok = micro.load(reg.c_str(), px(nx::fsMicro))
          && second.load(reg.c_str(), px(nx::fsSecond))
          && body.load(reg.c_str(), px(nx::fsBody))
          && title.load(b, px(nx::fsTitle));
    }
    void destroy() {
        micro.destroy(); second.destroy(); body.destroy(); title.destroy();
        ok = false; atScale = 0;
    }
};

Fonts g;

// Uppercase micro-label with §7's wide tracking. The renderer has no tracking
// parameter, so the label is drawn glyph by glyph; at 10px over 0.12em that is
// about one extra pixel per character and it is the difference between a chip
// that reads as a label and one that reads as small body text.
f32 microLabel(Renderer& r, const Font& f, f32 x, f32 y, const char* s, const Col& c) {
    f32 pen = std::round(x);
    const f32 track = std::max(1.f, (f32)f.size() * nx::microTracking);
    for (const char* p = s; *p; ++p) {
        char up[2] = {(char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p), 0};
        pen = r.text(f, pen, y, up, c);
        pen += track;
    }
    return pen;
}

void chip(Renderer& r, const Rect& b, const char* label, const Col& ink) {
    r.glass(b, nx::Tier::Chip);
    if (g.ok) {
        const f32 w = g.micro.measure(label) + (f32)label[0] * 0.f;
        const f32 track = std::max(1.f, (f32)g.micro.size() * nx::microTracking);
        const f32 total = w + track * (f32)std::char_traits<char>::length(label);
        microLabel(r, g.micro, b.cx() - total * 0.5f,
                   b.y + (b.h - g.micro.height()) * 0.5f, label, ink);
    }
}

// A pill button in the primary (violet) treatment: §5's inner top highlight
// plus the soft violet glow, with the light still arriving upper-left.
void primaryPill(Renderer& r, const Rect& b, const char* label, f32 dpi) {
    r.shadow(b, nx::pill, nx::ShadowSpec{0.f, 6.f, 18.f, -4.f, nx::violet.alpha(0.45f)});
    r.gradRect(b, nx::pill, nx::violetFill);
    r.gradStroke(b, nx::pill, dpi, nx::edge, 0.9f);
    if (g.ok) r.textIn(g.body, b, label, nx::text, Align::Center, 4.f * dpi);
}

void secondaryPill(Renderer& r, const Rect& b, const char* label, f32 dpi) {
    r.glass(b, nx::Tier::Chip);
    if (g.ok) r.textIn(g.body, b, label, nx::text, Align::Center, 4.f * dpi);
}

} // namespace

void specimenShutdown() { g.destroy(); }

void drawSpecimen(Renderer& r, f64 timeSeconds) {
    const f32 dpi = std::max(1.f, r.dpiScale());
    const f32 W = (f32)r.width(), H = (f32)r.height();
    if (W < 320.f * dpi || H < 240.f * dpi) return;
    g.ensure(dpi);

    const f32 t = nx::reducedMotion() ? 0.f : (f32)timeSeconds;
    const f32 S = nx::sp1 * dpi;              // one grid unit, in device pixels

    // Re-lay the whole background over the app. A specimen sheet has to be read
    // against the real §3 field -- nebula, stars, vignette and all -- and a
    // scrim of the flat field colour would bury exactly the layer the glass is
    // supposed to be refracting.
    r.background(timeSeconds);

    // Everything below lands on the 8px grid (§11).
    const f32 pad   = S * 4.f;
    const Rect page{pad, pad, W - pad * 2.f, H - pad * 2.f};
    const f32 colGap = S * 3.f;
    const f32 colW   = std::floor((page.w - colGap) * 0.5f / S) * S;
    const Rect colL{page.x, page.y, colW, page.h};
    const Rect colR{page.x + colW + colGap, page.y, page.w - colW - colGap, page.h};

    // -----------------------------------------------------------------------
    // Bar tier, across the top of the page.
    // -----------------------------------------------------------------------
    const Rect bar{page.x, page.y, page.w, S * 7.f};
    r.glass(bar, nx::Tier::Bar, nx::radiusSm * dpi);
    if (g.ok) {
        // ASCII only. The atlas covers 32..126, so a typographer's dash or a
        // middot renders as a hole -- which is a trap every view re-skinned
        // against this sheet is about to walk into, and the reason the labels
        // here look plainer than the copy in DESIGN.md does.
        r.textIn(g.title, {bar.x + S * 2.f, bar.y, bar.w * 0.5f, bar.h},
                 "NxTakt / NX specimen", nx::text, Align::Left, 0.f);
        char buf[160];
        std::snprintf(buf, sizeof buf, "%d quads | %d draws | blur %s | %s",
                      r.quads(), r.drawCalls(),
                      Renderer::realBlur() ? "real" : "synthesised",
                      nx::reducedMotion() ? "reduced motion" : "motion on");
        r.textIn(g.second, {bar.cx(), bar.y, bar.w * 0.5f - S * 2.f, bar.h},
                 buf, nx::muted, Align::Right, 0.f);
    }

    f32 yL = bar.bottom() + S * 3.f;
    f32 yR = yL;

    // -----------------------------------------------------------------------
    // Left column: the card, the well, the type scale.
    // -----------------------------------------------------------------------
    {
        const Rect card{colL.x, yL, colL.w, S * 28.f};
        // Hover state on a slow cycle, so a screenshot catches the lift and the
        // sheen at least half the time.
        const f32 hoverPhase = std::fmod(t, 6.f) / 6.f;
        const bool hover = hoverPhase > 0.15f && hoverPhase < 0.85f;
        const f32 lift = hover ? nx::easeSoft(clampv((hoverPhase - 0.15f) / 0.12f, 0.f, 1.f)) : 0.f;
        const Rect cr{card.x, card.y - 2.f * dpi * lift, card.w, card.h};

        nx::GlassStyle st = nx::glass(nx::Tier::Card);
        st.radius = nx::radius * dpi;
        if (hover) st.elev = nx::shadowLift;
        r.glass(cr, st);
        // The sheen crosses once per hover cycle, masked to the card.
        r.sheen(cr, st.radius, clampv((hoverPhase - 0.20f) / 0.45f, 0.f, 1.f), 1.f);

        if (g.ok) {
            const f32 tx = cr.x + S * 3.f;      // --sp-3 internal padding (§5)
            f32 y = cr.y + S * 3.f;
            microLabel(r, g.micro, tx, y, "tier 1 / card", nx::cyan.alpha(0.85f));
            y += g.micro.height() + S;
            r.text(g.title, tx, y, "Liquid glass", nx::text);
            y += g.title.height() + S;
            r.text(g.body, tx, y,
                   "--glass-1 over the nebula, a 1px lit edge, --shadow", nx::text);
            y += g.body.height() + S * 0.5f;
            r.text(g.body, tx, y,
                   "beneath, and a sheen that slides across on hover.", nx::text);
            y += g.body.height() + S * 2.f;
            r.text(g.second, tx, y,
                   "Cards never get real blur. On a screen with dozens", nx::muted);
            y += g.second.height() + S * 0.25f;
            r.text(g.second, tx, y,
                   "of them, that is the whole cardinal rule of section 4.", nx::muted);
        }
        {
            const f32 cw = S * 9.f, ch = S * 3.f;
            const f32 cy = cr.bottom() - S * 3.f - ch;
            chip(r, {cr.x + S * 3.f,                  cy, cw, ch}, "live",    nx::cyan);
            chip(r, {cr.x + S * 3.f + cw + S,         cy, cw, ch}, "pending", nx::amber);
            chip(r, {cr.x + S * 3.f + (cw + S) * 2.f, cy, cw, ch}, "inert",   nx::muted);
        }
        yL = card.bottom() + S * 3.f;
    }

    {
        // A well with rows, separated by hairlines. This is the language the
        // working surfaces get: recessed, flat, cheap.
        const Rect w{colL.x, yL, colL.w, S * 14.f};
        r.well(w, nx::radiusSm * dpi);
        if (g.ok) {
            microLabel(r, g.micro, w.x + S * 2.f, w.y + S * 1.5f, "well / list rows",
                       nx::muted);
        }
        const f32 rowY = w.y + S * 4.f;
        const f32 rowH = S * 3.f;
        static const char* kRows[3] = {"Drum Rack", "Operator", "Reverb"};
        static const char* kVals[3] = {"16 pads", "4 op FM", "2.4 s"};
        for (int i = 0; i < 3; ++i) {
            const Rect row{w.x + S, rowY + rowH * (f32)i, w.w - S * 2.f, rowH};
            if (i == 1) r.gradRect(row, nx::radiusXs * dpi, nx::glassChip, 0.7f);
            if (g.ok) {
                r.textIn(g.body, row, kRows[i], nx::text, Align::Left, S);
                r.textIn(g.second, row, kVals[i],
                         i == 1 ? nx::cyan : nx::muted, Align::Right, S);
            }
            if (i < 2) r.hairlineH(row.x + S, row.right() - S, row.bottom());
        }
        yL = w.bottom() + S * 3.f;
    }

    {
        // Type scale, over the field directly -- the hardest contrast case,
        // because there is no fill lifting the text off the background.
        if (g.ok) {
            f32 y = yL;
            microLabel(r, g.micro, colL.x, y, "type scale", nx::muted);
            y += g.micro.height() + S * 1.5f;
            r.text(g.title, colL.x, y, "Title 21 / weight 600-700", nx::text);
            y += g.title.height() + S;
            r.text(g.body, colL.x, y, "Body 14 / 400-500 / --text on the field", nx::text);
            y += g.body.height() + S * 0.75f;
            r.text(g.second, colL.x, y, "Secondary 12 / --muted", nx::muted);
            y += g.second.height() + S * 0.75f;
            microLabel(r, g.micro, colL.x, y, "micro 10 / 0.12em tracking", nx::muted);
            yL = y + g.micro.height() + S * 2.f;
        }
        // Both hairline orientations, and nothing solid anywhere.
        r.hairlineH(colL.x, colL.right(), yL);
        yL += S * 3.f;
    }

    {
        // Radial gradients: the monogram tiles of §8, hue hashed into the
        // cyan..violet band (187-290 degrees) so an app without an icon still
        // gets an identity that belongs to this palette and not to itself.
        if (g.ok) microLabel(r, g.micro, colL.x, yL, "radial / monogram tiles", nx::muted);
        yL += (g.ok ? g.micro.height() : S) + S * 1.5f;
        const f32 tile = S * 7.f;
        static const char* kNames[5] = {"OP", "DR", "RV", "EQ", "CM"};
        for (int i = 0; i < 5; ++i) {
            const Rect b{colL.x + (tile + S) * (f32)i, yL, tile, tile};
            // Deterministic hue in [187,290] degrees, converted straight to
            // RGB rather than through a colour library: this is the one place
            // the palette is generated instead of quoted.
            const f32 hue = 187.f + (f32)((i * 73 + 29) % 103);
            const f32 hp = hue / 60.f;
            const f32 x = 1.f - std::fabs(std::fmod(hp, 2.f) - 1.f);
            Col c = (hp < 4.f) ? Col(0.f, x, 1.f) : Col(x, 0.f, 1.f);
            const nx::Grad tileGrad = {
                {{Col(c.r * 0.95f + 0.2f, c.g * 0.85f + 0.15f, c.b * 0.9f + 0.2f, 0.95f), 0.f},
                 {Col(c.r * 0.45f, c.g * 0.35f, c.b * 0.75f, 0.95f), 0.55f},
                 {Col(0.06f, 0.03f, 0.13f, 0.95f), 1.f}},
                3, 0.f, true, 0.30f, 0.24f, 0.85f, 0.85f};   // light from upper-left
            r.gradRect(b, nx::radiusSm * dpi, tileGrad);
            r.gradStroke(b, nx::radiusSm * dpi, dpi, nx::edge, 0.9f);
            if (g.ok) r.textIn(g.body, b, kNames[i], nx::text, Align::Center, 0.f);
        }
        yL += tile + S * 3.f;
    }

    {
        // Inputs: a well-recessed field with a --line border, and the same
        // field focused -- violet border plus the two-ring focus halo. Never a
        // bare outline (§5).
        if (g.ok) microLabel(r, g.micro, colL.x, yL, "input / rest and focus", nx::muted);
        yL += (g.ok ? g.micro.height() : S) + S * 1.5f;
        const f32 fw = (colL.w - S * 3.f) * 0.5f, fh = S * 4.f;
        const Rect f1{colL.x, yL, fw, fh};
        const Rect f2{colL.x + fw + S * 3.f, yL, fw, fh};
        r.well(f1, nx::radiusSm * dpi, true);
        r.roundRectOutline(f1, nx::radiusSm * dpi, dpi, nx::line);
        r.well(f2, nx::radiusSm * dpi, true);
        r.roundRectOutline(f2, nx::radiusSm * dpi, dpi, nx::violet);
        r.focusRing(f2, nx::radiusSm * dpi);
        if (g.ok) {
            r.textIn(g.body, f1, "Untitled clip", nx::muted, Align::Left, S * 1.5f);
            r.textIn(g.body, f2, "Bassline 2", nx::text, Align::Left, S * 1.5f);
        }
        yL += fh + S * 3.f;
        // A vertical hairline, so both orientations are on the sheet.
        r.hairlineV(colL.cx(), yL, std::min(yL + S * 8.f, H - S * 2.f));
    }

    // -----------------------------------------------------------------------
    // Right column: sheet, easings, progress, swatches, working surface.
    // -----------------------------------------------------------------------
    {
        const Rect sheet{colR.x, yR, colR.w, S * 13.f};
        r.glass(sheet, nx::Tier::Sheet, nx::radius * dpi);
        if (g.ok) {
            f32 y = sheet.y + S * 2.f;
            microLabel(r, g.micro, sheet.x + S * 3.f, y, "tier 2 / sheet", nx::violetSoft);
            y += g.micro.height() + S;
            r.text(g.title, sheet.x + S * 3.f, y, "Delete 3 clips?", nx::text);
            y += g.title.height() + S * 0.75f;
            r.text(g.second, sheet.x + S * 3.f, y,
                   "They are removed from every scene that uses them.", nx::muted);
        }
        const f32 bw = S * 12.f, bh = S * 4.f;
        secondaryPill(r, {sheet.right() - S * 3.f - bw * 2.f - S * 2.f,
                          sheet.bottom() - S * 3.f - bh, bw, bh}, "Cancel", dpi);
        primaryPill(r, {sheet.right() - S * 3.f - bw,
                        sheet.bottom() - S * 3.f - bh, bw, bh}, "Delete", dpi);
        yR = sheet.bottom() + S * 3.f;
    }

    {
        // The three easings, each carrying a pill across a well trough on the
        // same clock. ease-spring visibly overshoots the end of its travel and
        // settles back; that overshoot is the whole reason it exists, and it is
        // the one thing a smoothstep approximation would have quietly deleted.
        if (g.ok) microLabel(r, g.micro, colR.x, yR, "motion / 150, 220, 320 ms", nx::muted);
        yR += (g.ok ? g.micro.height() : S) + S * 1.5f;

        struct Row { const char* name; const nx::Ease* e; f32 durS; };
        const Row rows[3] = {
            {"ease-spring", &nx::easeSpring, nx::durSlow},
            {"ease-soft",   &nx::easeSoft,   nx::dur},
            {"ease-out",    &nx::easeOut,    nx::durFast},
        };
        // Each easing runs on its own loop, `dur` of travel then a rest, so a
        // still frame catches the three at different points instead of parked
        // together at the end.
        for (int i = 0; i < 3; ++i) {
            const Rect track{colR.x + S * 14.f, yR + (S * 4.f) * (f32)i,
                             colR.w - S * 14.f, S * 3.f};
            r.well(track, track.h * 0.5f, true);
            if (g.ok)
                r.textIn(g.second, {colR.x, track.y, S * 13.f, track.h},
                         rows[i].name, nx::muted, Align::Left, 0.f);

            const f32 dot = track.h - S * 0.75f;
            const f32 travel = track.w - dot - S;
            auto place = [&](f32 u) {
                return track.x + S * 0.5f + travel * clampv(u, -0.12f, 1.14f);
            };

            // The motion trail: 22 ticks at equal time steps, placed by the
            // easing. Where the ticks bunch, the curve is slow; where they
            // spread, it is fast -- so the SHAPE of each easing is legible in a
            // still screenshot, and ease-spring's ticks visibly run past the
            // end of the travel and come back. An eyeballed smoothstep would
            // have no ticks out there at all, which is the whole reason these
            // are solved cubic-beziers and not approximations.
            for (int k = 0; k <= 21; ++k) {
                const f32 u = (*rows[i].e)((f32)k / 21.f);
                const f32 x = place(u);
                r.roundRect({x + dot * 0.5f - 1.5f * dpi, track.cy() - 1.5f * dpi,
                             3.f * dpi, 3.f * dpi},
                            1.5f * dpi, nx::cyan.alpha(u > 1.001f ? 0.90f : 0.38f));
            }

            const f32 loop = rows[i].durS + 0.9f;
            const f32 ph = nx::reducedMotion() ? 1.f : std::fmod(t, loop);
            const f32 u = (*rows[i].e)(clampv(ph / rows[i].durS, 0.f, 1.f));
            const Rect pill{place(u), track.y + S * 0.375f, dot, dot};
            r.shadow(pill, dot * 0.5f, nx::ShadowSpec{0.f, 2.f, 8.f, 0.f,
                                                      nx::violet.alpha(0.5f)});
            r.gradRect(pill, dot * 0.5f, nx::violetFill);
            r.gradStroke(pill, dot * 0.5f, dpi, nx::edge, 0.85f);
        }
        yR += S * 12.f + S;
    }

    {
        // Progress: a recessed trough with a violet -> cyan liquid fill and a
        // slow sheen. Cyan appears as light inside the material, never as a
        // surface colour.
        if (g.ok) microLabel(r, g.micro, colR.x, yR, "progress / violet to cyan", nx::muted);
        yR += (g.ok ? g.micro.height() : S) + S * 1.5f;
        const Rect trough{colR.x, yR, colR.w, S * 2.f};
        r.well(trough, trough.h * 0.5f, true);
        const f32 p = nx::reducedMotion() ? 0.62f
                                          : 0.10f + 0.85f * (0.5f - 0.5f * std::cos(t * 0.9f));
        const Rect fill{trough.x, trough.y, std::max(trough.h, trough.w * p), trough.h};
        r.gradRect(fill, trough.h * 0.5f, nx::liquid);
        r.sheen(fill, trough.h * 0.5f, std::fmod(t * 0.45f, 1.f), 1.2f);
        yR += trough.h + S * 3.f;
    }

    {
        // Swatches, with the ink they are meant to carry drawn on top: this is
        // the text-contrast spot check §7 asks for, made visible.
        if (g.ok) microLabel(r, g.micro, colR.x, yR, "text over every fill", nx::muted);
        yR += (g.ok ? g.micro.height() : S) + S * 1.5f;
        struct Sw { const char* name; int kind; };
        const Sw sw[6] = {{"glass-1", 0}, {"glass-2", 1}, {"glass-bar", 2},
                          {"well", 3}, {"well-deep", 4}, {"violet", 5}};
        const f32 sww = (colR.w - S * 5.f) / 6.f, swh = S * 5.f;
        for (int i = 0; i < 6; ++i) {
            const Rect b{colR.x + (sww + S) * (f32)i, yR, sww, swh};
            switch (sw[i].kind) {
            case 0: r.gradRect(b, nx::radiusXs * dpi, nx::glass1); break;
            case 1: r.gradRect(b, nx::radiusXs * dpi, nx::glass2); break;
            case 2: r.gradRect(b, nx::radiusXs * dpi, nx::glassBar); break;
            case 3: r.well(b, nx::radiusXs * dpi, false); break;
            case 4: r.well(b, nx::radiusXs * dpi, true); break;
            default: r.gradRect(b, nx::radiusXs * dpi, nx::violetFill); break;
            }
            r.gradStroke(b, nx::radiusXs * dpi, dpi, nx::edge, 0.8f);
            if (g.ok) {
                r.textIn(g.second, {b.x, b.y + S * 0.5f, b.w, swh * 0.5f},
                         "Agj 14", nx::text, Align::Center, 0.f);
                r.textIn(g.micro, {b.x, b.cy() + S * 0.25f, b.w, swh * 0.5f},
                         sw[i].name, nx::muted, Align::Center, 0.f);
            }
        }
        yR += swh + S * 3.f;
    }

    {
        // THE OTHER HALF OF THE JUDGMENT. A working surface: flat cells in a
        // deep well, a 1px violet grid, a cyan playhead. No glass fill, no
        // lit edge, no sheen, no shadow. Chrome gets glass; the grid, the
        // arrangement, the roll, the meters and the waveforms do not.
        if (g.ok) microLabel(r, g.micro, colR.x, yR,
                             "working surface / flat, precise, fast", nx::cyan.alpha(0.8f));
        yR += (g.ok ? g.micro.height() : S) + S * 1.5f;
        const Rect grid{colR.x, yR, colR.w, S * 10.f};
        if (grid.bottom() > H - S * 2.f) return;
        r.well(grid, nx::radiusSm * dpi, true);

        const int cols = 8, rowsN = 3;
        const f32 cw = (grid.w - S * 2.f) / (f32)cols;
        const f32 ch = (grid.h - S * 2.f) / (f32)rowsN;
        for (int y = 0; y < rowsN; ++y) {
            for (int x = 0; x < cols; ++x) {
                const Rect cellR{grid.x + S + cw * (f32)x + 1.f,
                                 grid.y + S + ch * (f32)y + 1.f, cw - 2.f, ch - 2.f};
                const bool filled = ((x * 3 + y * 5) % 7) < 3;
                if (!filled) { r.roundRect(cellR, 2.f * dpi, nx::line.alpha(0.35f)); continue; }
                const Col c = pal::clipColors[(x + y * 3) % pal::clipColorCount];
                r.roundRect(cellR, 2.f * dpi, c.alpha(0.85f));
                r.rect({cellR.x, cellR.y, cellR.w, dpi}, c.scale(1.35f).alpha(0.9f));
            }
        }
        const f32 px = grid.x + S + (grid.w - S * 2.f) *
                       (nx::reducedMotion() ? 0.42f : std::fmod(t * 0.22f, 1.f));
        r.rect({std::round(px), grid.y + S, std::max(1.f, dpi), grid.h - S * 2.f},
               nx::cyan.alpha(0.9f));
    }
}

} // namespace lat
