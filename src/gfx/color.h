// Colour type plus the NxTakt palette, tuned against Live's dark theme.
#pragma once
#include "../core/common.h"

namespace lat {

struct Col {
    f32 r = 0, g = 0, b = 0, a = 1;
    constexpr Col() = default;
    constexpr Col(f32 R, f32 G, f32 B, f32 A = 1.f) : r(R), g(G), b(B), a(A) {}
    Col alpha(f32 A) const { return Col(r, g, b, A); }
    Col scale(f32 s) const { return Col(r * s, g * s, b * s, a); }
    Col mix(const Col& o, f32 t) const {
        return Col(lerpf(r, o.r, t), lerpf(g, o.g, t), lerpf(b, o.b, t), lerpf(a, o.a, t));
    }
    u32 packed() const {
        return ((u32)(clampv(r, 0.f, 1.f) * 255) << 16) |
               ((u32)(clampv(g, 0.f, 1.f) * 255) << 8) |
                (u32)(clampv(b, 0.f, 1.f) * 255);
    }
};

constexpr Col rgb(u32 hex) {
    return Col(((hex >> 16) & 0xFF) / 255.f, ((hex >> 8) & 0xFF) / 255.f, (hex & 0xFF) / 255.f, 1.f);
}
constexpr Col rgba(u32 hex, f32 a) {
    return Col(((hex >> 16) & 0xFF) / 255.f, ((hex >> 8) & 0xFF) / 255.f, (hex & 0xFF) / 255.f, a);
}

// ---------------------------------------------------------------------------
// The palette, retuned onto the NX design language (docs/DESIGN.md, theme.h).
//
// WHY THIS FILE MOVED AND theme.h DID NOT REPLACE IT
//
// theme.h holds the tokens; this holds the *legacy surface names* the existing
// views spell. Roughly three hundred call sites across src/ui say `pal::panel`
// and `pal::textDim`, and they will keep saying it until each view is
// re-skinned onto the tokens directly. Retuning the values here is what lets
// the whole program change palette in one commit instead of fifteen, and it is
// why the very first screenshot of the unmodified app already reads as NX
// rather than as grey boxes floating over a nebula nobody can see.
//
// TRANSLUCENCY IS THE POINT, not a side effect. The old values were opaque
// greys, and an opaque surface over §3's living background hides it completely
// -- the field, the nebula and the stars all render underneath and none of
// them reach the eye. The background-most tokens are therefore translucent
// now: the field shows through the empty half of the clip grid and around the
// edges of every panel, which is the entire mechanism by which "faked glass"
// works (§4). Anything that must stay readable stays near-opaque.
//
// Hue: every grey is gone. Surfaces sit on the violet axis, so violet
// dominates by construction rather than by decoration (§1).
//
// A view being re-skinned should migrate to `nx::` and drop its `pal::` use;
// until it does, it gets the new palette for free and looks intentional.
// ---------------------------------------------------------------------------
namespace pal {
// Surfaces. Alphas are over the §3 field, which is #0a0714 -> #12091f, so a
// 0.55 fill still lands near #14 0e 22 -- dark enough to read text on.
// Darker and less violet than the first tuning: at 0x171028-class fills over
// the nebula the whole app carried a purple wash and read as a toy. Expensive
// is 90% darkness with small luminous accents, so the base surfaces move
// toward the field (#0a0714) and keep only a BREATH of violet; saturated
// color is for clips, accents and live elements, never for square metres.
constexpr Col appBg        = rgba(0x0A0712, 0.62f);   // the "nothing here" fill
constexpr Col panel        = rgba(0x100B1D, 0.92f);   // chrome panels
constexpr Col panelAlt     = rgba(0x150E26, 0.93f);   // controls on panels
constexpr Col gridBg       = rgba(0x0C0918, 0.58f);   // the clip grid's empty field
constexpr Col slotEmpty    = rgba(0x120C20, 0.62f);
constexpr Col slotHover    = rgba(0x2A1E48, 0.90f);
// Not a divider colour any more: `divider` is used both for separators and for
// recessed troughs, and only the separators are wrong. It is now the --line
// token, which is a violet-black rather than a grey. Real dividers should move
// to Renderer::hairlineH/V -- a solid line is a §11 finding.
constexpr Col divider      = rgba(0x2A1F45, 0.85f);
constexpr Col ridge        = rgb(0x4B3A6E);           // raised edges, handles

// Text
constexpr Col text         = rgb(0xEFEAFF);
constexpr Col textDim      = rgb(0x9A8FC0);
constexpr Col textFaint    = rgb(0x6B5F92);
constexpr Col textOnClip   = rgb(0x160E28);

// Accents. The brand accent is the user's chosen electric purple. It is a
// dark hue, so two rules keep it readable: as small TEXT on dark surfaces use
// accentHi (a lifted tint), and anything drawn ON an accent fill uses light
// text — widgets.cpp picks per-luminance.
constexpr Col accent       = rgb(0x7700FF);   // selection / focus / brand
constexpr Col accentHi     = rgb(0xA875FF);   // accent as text on dark
// Transport and status. Play moves to cyan: §1 reserves cyan for "light inside
// materials" -- live values, playheads, running state -- which is exactly what
// a play indicator is, and a lime green on a violet field was the one colour in
// the program that belonged to no palette at all. Record stays red because red
// means record everywhere and a DAW does not get to be clever about that.
constexpr Col playGreen    = rgb(0x00E5FF);
constexpr Col recRed       = rgb(0xFF5470);
constexpr Col armRed       = rgb(0xB03048);
constexpr Col soloBlue     = rgb(0x66C4FF);
// Meters keep the green/amber/red ladder. It is a measurement instrument, read
// at a glance and often in peripheral vision, and every engineer alive already
// knows what its colours mean; restyling it would be branding at the cost of
// the one thing on screen that has to be unambiguous.
constexpr Col meterGreen   = rgb(0x3FD07A);
constexpr Col meterAmber   = rgb(0xFFB300);
constexpr Col meterRed     = rgb(0xFF5470);

// The eight-colour clip strip Live cycles through for new tracks.
constexpr Col clipColors[] = {
    rgb(0xFF94A6), rgb(0xFFA529), rgb(0xCC9B54), rgb(0xF7F47C),
    rgb(0xBFFB00), rgb(0x1AFF2F), rgb(0x25FFA8), rgb(0x5CFFE8),
    rgb(0x8BC5FF), rgb(0x5480E4), rgb(0x92A7FF), rgb(0xD86CE4),
    rgb(0xE553A0), rgb(0xFFFFFF), rgb(0xFF3636), rgb(0xF66C03),
};
constexpr int clipColorCount = 16;
} // namespace pal

} // namespace lat
