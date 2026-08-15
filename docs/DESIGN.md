<!-- Vendored from nerdrx/nx-hub docs/DESIGN.md (v1.0). That file is canonical;
     update this copy when it changes. NxTakt is the §10 'native desktop' case:
     the tokens are the contract, the SDF renderer is the technology. -->

# The NX Design Language

**Version 1.0 · extracted from NX Hub v0.3.3 (the reference implementation)**

This document is the canonical specification of the NX visual language —
"liquid glass on deep space." It is written to be dropped into any project's
context (Electron, web, Android, or native) and applied without access to the
original codebase. Where this document and an implementation disagree, NX Hub's
`src/renderer/styles.css` is ground truth; update this file when it changes.

The one-sentence version: **dark violet space, frosted glass floating above a
living nebula, light behaving physically, motion behaving like liquid — and
restraint everywhere.**

---

## 1. Identity

| Anchor | Value |
| --- | --- |
| Brand primary | **NX Violet `#7700FF`** — actions, focus, identity. |
| Brand secondary | **Cyan `#00e5ff`** — light *inside* materials: edges, live status, progress. Never a competing surface color. |
| Field | Deep space `#0a0714 → #12091f` vertical gradient, never flat black. |
| Signal colors | Amber `#ffb300` = update/attention. Red `#ff5470` = danger only. |
| The mark | A beveled glass crystal hexagon (pointy-top, always) with a sculpted geometric monogram. |
| Type | System UI stack. No webfonts. Weight and spacing do the branding. |

Rules that make it feel expensive:

- Violet **dominates**; cyan is subordinate — a light source, not a paint.
- Everything translucent is **low-alpha**. If a gradient is visible from across
  the room, halve it.
- No solid gray lines anywhere. Dividers are gradient hairlines that fade at
  both ends.
- One light source: **upper-left**, in every gradient, bevel, and edge. Light
  consistency is why surfaces read as one physical world.

## 2. Design tokens

Copy these verbatim into `:root` (CSS) or mirror them as resources (Android
§10). They are the entire system; components are compositions of tokens.

```css
:root {
  /* brand (frozen — never restyle these) */
  --bg-top: #0a0714;
  --bg-bottom: #12091f;
  --panel: #171028;
  --panel-2: #1d1433;
  --violet: #7700ff;
  --violet-soft: #9a3cff;
  --cyan: #00e5ff;
  --amber: #ffb300;
  --text: #efeaff;
  --muted: #9a8fc0;
  --line: #2a1f45;
  --danger: #ff5470;

  /* geometry */
  --radius: 18px;      /* cards, sheets */
  --radius-sm: 12px;   /* rows, wells, inputs */
  --radius-xs: 8px;    /* chips, code */
  --pill: 999px;       /* buttons, tabs, badges */
  --font: system-ui, -apple-system, "Segoe UI", Roboto, "Noto Sans", Cantarell, sans-serif;
  --mono: ui-monospace, "JetBrains Mono", "Fira Code", Consolas, monospace;

  /* glass fills — light collects top-left and drains to a cool shadow */
  --glass-bar: linear-gradient(180deg, rgba(46, 30, 78, 0.62) 0%, rgba(18, 11, 34, 0.72) 100%);
  --glass-1: linear-gradient(157deg, rgba(255, 255, 255, 0.09) 0%, rgba(255, 255, 255, 0.026) 34%,
      rgba(23, 16, 40, 0.34) 100%);
  --glass-2: linear-gradient(158deg, rgba(255, 255, 255, 0.1) 0%, rgba(255, 255, 255, 0.03) 30%,
      rgba(19, 12, 34, 0.66) 100%);
  --glass-chip: linear-gradient(180deg, rgba(255, 255, 255, 0.09) 0%, rgba(255, 255, 255, 0.028) 100%);
  --well: linear-gradient(180deg, rgba(7, 4, 16, 0.5) 0%, rgba(7, 4, 16, 0.32) 100%);
  --well-deep: linear-gradient(180deg, rgba(4, 2, 10, 0.62) 0%, rgba(4, 2, 10, 0.46) 100%);

  /* blur strengths — ONLY these three exist */
  --blur-bar: blur(22px) saturate(170%);
  --blur-sheet: blur(34px) saturate(185%);
  --blur-chip: blur(16px) saturate(160%);

  /* lit edges — 1px gradient borders, bright top-left → dark bottom-right */
  --edge: linear-gradient(147deg, rgba(255, 255, 255, 0.34) 0%, rgba(255, 255, 255, 0.09) 24%,
      rgba(255, 255, 255, 0.015) 52%, rgba(0, 0, 0, 0.34) 100%);
  --edge-lit: linear-gradient(147deg, rgba(226, 200, 255, 0.62) 0%, rgba(154, 60, 255, 0.28) 30%,
      rgba(0, 229, 255, 0.1) 58%, rgba(0, 0, 0, 0.3) 100%);
  --edge-top: rgba(255, 255, 255, 0.18);
  --hairline: linear-gradient(90deg, rgba(255, 255, 255, 0) 0%, rgba(255, 255, 255, 0.09) 18%,
      rgba(255, 255, 255, 0.13) 50%, rgba(255, 255, 255, 0.09) 82%, rgba(255, 255, 255, 0) 100%);
  --sheen: linear-gradient(112deg, rgba(255, 255, 255, 0) 30%, rgba(255, 255, 255, 0.085) 45%,
      rgba(214, 190, 255, 0.05) 52%, rgba(255, 255, 255, 0) 68%);

  /* elevation */
  --shadow: 0 14px 34px -12px rgba(0, 0, 0, 0.72), 0 2px 8px rgba(0, 0, 0, 0.3);
  --shadow-lift: 0 26px 54px -16px rgba(0, 0, 0, 0.8), 0 0 40px -8px rgba(119, 0, 255, 0.34);
  --shadow-bar: 0 20px 44px -24px rgba(0, 0, 0, 0.9), 0 1px 0 rgba(255, 255, 255, 0.04);
  --shadow-sheet: 0 48px 96px -32px rgba(0, 0, 0, 0.86), 0 0 0 1px rgba(255, 255, 255, 0.06);
  --focus-ring: 0 0 0 2px rgba(119, 0, 255, 0.6), 0 0 0 5px rgba(119, 0, 255, 0.2);

  /* motion */
  --ease-spring: cubic-bezier(0.32, 1.35, 0.42, 1);  /* overshoots — pills, tab indicator */
  --ease-soft: cubic-bezier(0.2, 0.8, 0.2, 1);       /* default interactive */
  --ease-out: cubic-bezier(0.16, 1, 0.3, 1);         /* entrances */
  --dur-fast: 150ms;
  --dur: 220ms;
  --dur-slow: 320ms;

  /* 8px rhythm */
  --sp-1: 8px;
  --sp-2: 16px;
  --sp-3: 24px;
  --sp-4: 32px;
}
```

## 3. The living background

Glass is only convincing when there is light behind it to refract. Every NX
surface sits above two fixed, full-viewport layers:

1. **Nebula** — two to three enormous radial-gradient blobs (violet upper-left,
   cyan lower-right, optionally a deep magenta third) at very low alpha over
   the `--bg-top → --bg-bottom` field, drifting on `transform`-only keyframe
   animations with **periods of 60–110 seconds**, alternating direction. Add a
   soft vignette so edges stay darker than center.
2. **Starfield** — sparse, tiny, static or near-static points at low opacity.
   Decoration, not attraction.

Both layers pause when the document is hidden and freeze entirely under
`prefers-reduced-motion`. If you build one custom canvas, budget it: two
layers, transform-only, `requestAnimationFrame` parked when not visible.

## 4. The glass tier system

**The cardinal performance rule: real `backdrop-filter` is a budget, not a
default.** Reserve it for the few floating surfaces that overlap other content.
Everything else *synthesizes* glass from the translucent gradient fills — the
nebula glowing through low-alpha fills reads as frosted glass at a fraction of
the cost.

| Tier | Surfaces | Fill | Blur | Edge | Shadow |
| --- | --- | --- | --- | --- | --- |
| **Bar** | app header, floating toolbars | `--glass-bar` | `--blur-bar` (real) | 1px `--edge-top` top highlight | `--shadow-bar` |
| **1 — Card** | content cards, tiles | `--glass-1` | **none — faked** | `--edge` gradient border | `--shadow` |
| **2 — Sheet** | modals, slide-overs, menus, toasts | `--glass-2` | `--blur-sheet` / `--blur-chip` (real) | `--edge-lit` | `--shadow-sheet` |
| **Well** | recessed regions *inside* glass (list rows, code, logs) | `--well` / `--well-deep` | never | none or `--line` | inset only |

Implementation notes:

- The 1px gradient edge is a `border-image` or a masked pseudo-element painting
  `--edge` — brighter top-left, darker bottom-right, matching the global light.
- **Glass inside glass reads as fog.** Content regions inside a card are wells
  (recessed dark surfaces), never a second frosted layer. This is what keeps
  hierarchy legible.
- Menus and toasts carry legibility in their **fill alpha (≥0.9)**; blur is
  finish, not the mechanism. Text over blur alone becomes unreadable over busy
  content.
- Keep simultaneous real-blur elements at roughly **≤10 visible**; on a screen
  with dozens of cards, that is exactly why cards fake it.
- Give cards `isolation: isolate` so internal z-ordering can't leak, and
  elevate any card with an open menu above its siblings.

## 5. Components

**Buttons** are glass pills (`--pill` radius). Primary: violet fill with an
inner top highlight and a soft violet glow; hover lifts 1–2px
(`translateY(-1px)`) and blooms the glow; press scales to `0.96`. Secondary:
`--glass-chip` fill with `--edge` border. Danger uses `--danger` only for
genuinely destructive actions. Amber is reserved for "update available" class
actions. Disabled: 40% opacity, no hover response.

**The tab pill**: navigation tabs share one sliding indicator — a single
element translated between equal-width tab slots with `--ease-spring`, never
per-tab background toggles. (CSS-only via `:has()` on the active tab.)

**Cards**: tier-1 glass, `--radius`, `--sp-3` internal padding. Hover: lift
`translateY(-2px)`, `--shadow-lift`, and slide the `--sheen` band across via a
masked pseudo-element (background-position or transform only). Card grids flow
as **masonry** (CSS `columns`, `break-inside: avoid`) so mixed heights pack
tightly — top-aligned grid rows with ragged holes read as fragmentation.

**Chips / badges**: uppercase micro-labels, 10–11px, `letter-spacing: 0.12em+`,
`--glass-chip` fill, `--pill` radius. Status chips: cyan = live/connected,
amber = pending attention, muted = inert.

**Progress**: a recessed well trough with a luminous violet→cyan liquid fill
and a slow moving sheen. Indeterminate = full-width pulsing sheen.

**Inputs**: well-recessed fields, `--radius-sm`, 1px `--line` border that
transitions to violet on focus with `--focus-ring`. Never a bare `outline`.

**Sheets & modals**: rise from below with `--ease-out` at `--dur-slow` while a
scrim dims and blurs in; the sheet itself is tier-2. Escape and scrim-click
dismiss.

**Toasts**: small tier-2 chips, bottom-right stack with 6–10px offsets,
slide+fade in, auto-dismiss non-errors.

**Empty states**: one short bold line, one muted sentence, one primary action.
Centered, generous whitespace, no illustration clutter.

## 6. Motion

Motion is liquid, brief, and interruptible.

- Animate **transform and opacity only**. Never width/height/top/left.
- Interaction feedback at `--dur-fast`, view changes at `--dur` to
  `--dur-slow`. Nothing interactive exceeds 320ms.
- `--ease-spring` (with overshoot) is for playful, identity-bearing moves: the
  tab pill, a tile press. `--ease-soft` is the workhorse. `--ease-out` for
  entrances.
- View switches: 180ms crossfade + ~8px slide. No layout jank; keep it
  interruptible.
- **`prefers-reduced-motion: reduce` is non-negotiable**: nebula frozen,
  sheens off, springs replaced, every transition collapses to opacity.

## 7. Typography & rhythm

- System stack (`--font`); code and version strings in `--mono`.
- Scale: 20–22px bold titles · 14px body · 12–13px secondary (`--muted`) ·
  10–11px uppercase micro-labels with wide tracking. Weights 600–700 for
  headings, 400–500 body.
- Body text is always `--text` on dark surfaces — verify contrast stays
  WCAG-comfortable over glass. If a fill fights the text, darken the fill.
- **Everything sits on the 8px grid** (`--sp-*`). When in doubt: 16 inside,
  16–24 between siblings, 24–32 between sections.
- Numbers, sizes, and dates format **locale-independently** (host machines may
  run any locale) — no bare `toLocaleString` in logic paths.

## 8. Iconography & the mark

- The NX mark is a **pointy-top hexagon** everywhere. Never flat-top.
- Three size variants exist and are mandatory — one file cannot span
  16→512px: the **master** (full bevel + well + refraction, ≥48px), the
  **small** variant (wider cut, no bevel, flat white monogram, ≤32px), and the
  **tray** variant (flat brand violet, knocked-out monogram — survives OS
  tinting on light and dark trays).
- Masters live in NX Hub's `assets/` (`icon.svg`, `icon-small.svg`,
  `tray.svg`); derive all rasters from the *correct variant per size*. Never
  scale the master below 48px.
- UI glyphs: stroked, geometric, 1.5–2px stroke at 16–20px box, `currentColor`,
  inline SVG. No emoji as UI iconography, no icon fonts.
- App tiles without a real icon get a **deterministic monogram tile**: 1–2
  letters on a radial gradient whose hue is hashed from the app's id, clamped
  to the cyan→violet band (187–290°). Never substitute the NX brand mark for a
  third-party app's identity.

## 9. Voice

UI copy is English, short, and concrete. Sentence case everywhere except
micro-label chips (uppercase). Errors say what happened *and what to do next*
("Could not reach 192.168.1.50 — check that the device is on the same
network"). No exclamation marks, no cutesy mascot voice, no jargon in
user-facing strings. Empty states invite the next action rather than apologize.
Destructive and irreversible actions always confirm, stating the consequence
plainly.

## 10. Applying it outside the web stack

**Android (Views or Compose)** — map tokens to resources: `nx_violet #7700FF`,
`nx_cyan #00E5FF`, `nx_bg #0A0714`, `nx_panel #171028`, `nx_text #EFEAFF`,
`nx_muted #9A8FC0`, `nx_amber #FFB300`. Cards: `--panel`-toned surfaces with
subtle top-light gradient (real blur is rarely worth it on mobile GPUs — fake
tier-1 exactly like the desktop cards); wells as darker inset containers; pill
buttons with violet fill; 8dp rhythm; the adaptive launcher icon derives from
the mark's foreground/background split with a monochrome variant. Motion via
`OvershootInterpolator`-class curves at the same durations.

**Native desktop (Qt/GTK/imgui)** — the tier table survives translation: bar
and sheet surfaces get real blur where the toolkit offers it, content surfaces
use the gradient-fill fake, wells recess. The tokens are the contract; the
technology is negotiable.

**Terminal/CLI apps** — violet primary accents, cyan for live values, amber
for warnings, muted lavender for secondary text; uppercase wide-tracked section
labels echo the chip language.

## 11. The review checklist

Before shipping any NX-branded surface, verify:

- [ ] Light comes from the upper-left in every gradient and edge.
- [ ] Real blur count on a busy screen ≤ ~10; cards fake it.
- [ ] No solid gray dividers; hairlines fade at both ends.
- [ ] Violet leads, cyan accents, amber only means "attention."
- [ ] All spacing lands on the 8px grid.
- [ ] Hover lifts, press scales, springs only where identity lives.
- [ ] `prefers-reduced-motion` fully honored.
- [ ] Text contrast comfortable over every fill it sits on.
- [ ] Mixed-height collections pack (masonry), never leave ragged holes.
- [ ] The mark is pointy-top, correct variant for the size.
- [ ] Copy is sentence-case, concrete, and tells the user what to do next.

---

*Reference implementation: [nerdrx/nx-hub](https://github.com/nerdrx/nx-hub) —
`src/renderer/styles.css` (tokens & components), `src/renderer/views/`
(component markup), `assets/` (the mark).*
