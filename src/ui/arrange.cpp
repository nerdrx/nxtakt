// The arrangement editor (docs/ARRANGEMENT.md §7.4, §7.5).
//
// Layout, left to right: a header column carrying each track's name, colour,
// arm and disclosure triangle, then the lanes themselves under a shared bar
// ruler. Every pixel <-> beat conversion goes through timeaxis.h, which is the
// same TimeAxis the piano roll uses — so a note inside an item and the item
// itself can never disagree about where a beat is, at any zoom, after any
// scroll. That is the whole reason the axis was extracted rather than copied.
//
// WHEN THE MODEL MOVES, AND WHEN IT IS REPAIRED. A drag mutates the model live,
// so the item follows the hand, but reports nothing and repairs nothing until
// the button comes up. Two things fall out of that and both are deliberate:
// the engine never sees a lane that has not been through arrangeRepair, and a
// sweep across a neighbour does not eat it a frame at a time — the invariant is
// restored once, against the position the hand actually finished on.
#include "arrange.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lat {
namespace {

// Undo labels, which is what the caller reads back off lastEdit().
constexpr const char* kEditMove  = "move clip";
constexpr const char* kEditTrim  = "trim clip";
constexpr const char* kEditFade  = "clip fade";
constexpr const char* kEditDup   = "duplicate clip";
constexpr const char* kEditDel   = "delete clip";
constexpr const char* kEditSplit = "split clip";
constexpr const char* kEditAuto  = "automation edit";
constexpr const char* kEditLayout = "lane height";
constexpr const char* kEditMarkerMove = "move marker";
// FL's two sweeps. Both are ONE undo entry for the whole stroke, coalesced on
// the gesture id, because that is what the hand did: a sweep is one movement
// and undoing it one item at a time would be the worst possible reading of it.
constexpr const char* kEditErase = "erase clips";
constexpr const char* kEditPaint = "paint clips";
constexpr const char* kEditAutoErase = "erase automation points";

// WIDGET IDS IN THIS FILE. Everything here hashes under UiArrange (plus
// UiArrangeLane and UiArrangeLaneHead for the automation lanes), and the
// markers take three FRESH SUB-IDS inside it rather than a kind of their own:
//
//     (UiArrange, 9)            the marker band's hot rect
//     (UiArrange, 10, <uid>)    a flag's drag gesture
//     (UiArrange, 11, <uid>)    a flag's inline rename field
//     (UiArrange, 12)           the right-drag ERASE stroke (items)
//     (UiArrange, 13)           the left-drag PAINT stroke
//     (UiArrange, 14, <track>)  the right-drag erase stroke over a lane's points
//
// These used to be raw 24/25/26 with a paragraph here explaining why a named
// UiKind was not available: the registry lived in app_internal.h, App's
// private glue, which this file is on the wrong side of the view seam to
// include. The registry now lives in widgets.h beside uiId itself, so the
// tidier answer that paragraph asked for is simply taken. Sub-ids are additive
// and RENUMBER NOTHING. The old 24/25 collision with app_detail.cpp is gone
// (UiDetailKeyRow/UiDetailNotes); these three kinds belong to this file alone.

// How far past the last thing on the timeline the view may scroll. A view that
// stopped dead at the last item would give nowhere to drop the next one.
constexpr f64 kArrTailBeats = 32.0;
constexpr f64 kArrMinContent = 64.0;      // 16 bars, so an empty set has a ruler

// A MIDI item's note preview is drawn against a fixed pitch window rather than
// the notes' own range: an item that happens to hold one note would otherwise
// draw it in the middle of a lane that says nothing about the part's register.
constexpr int kPreviewLoPitch = 24;
constexpr int kPreviewHiPitch = 96;
// A looping item repeats its material; the preview stops after this many, which
// is far past what is legible at any zoom a hand works at.
constexpr int kMaxPreviewReps = 128;
// Segments in a fade's drawn curve. Fixed rather than per-pixel: the shaded
// wedge under it is already smooth, and this is only the line that reads as the
// curve's shape. Eight is where the joints stop being visible at the widths a
// fade is actually dragged to.
constexpr int kFadeCurveSegs = 8;

// The colour an item draws in. An item on an OVERRIDDEN track is desaturated,
// which is the same visual grammar an overridden automation lane already uses
// (docs/AUTOMATION.md §6.3): the material is still there, it is just not what
// you are hearing.
Col itemColour(int colorIdx, bool overridden) {
    const int n = pal::clipColorCount;
    const Col c = pal::clipColors[((colorIdx % n) + n) % n];
    if (!overridden) return c;
    const f32 lum = 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
    return c.mix(Col(lum, lum, lum, c.a), 0.72f).scale(0.7f);
}

// Where a beat inside an item lands in its source clip, as a fraction of the
// clip's length. Returns false when the item has run off the end of a
// non-looping clip, which is silence and must not draw a waveform.
bool srcFraction(const ClipModel& src, f64 clipBeat, f64& outU) {
    const f64 len = src.lengthBeats > 1e-9 ? src.lengthBeats : 1.0;
    f64 b = clipBeat;
    if (src.loop) {
        b = std::fmod(b, len);
        if (b < 0.0) b += len;
    }
    if (b < 0.0 || b >= len) return false;
    outU = b / len;
    return true;
}

// A fresh identity for an item the view has just made. Zero when the caller did
// not hand the counter over, which every path below then treats as "unstamped"
// and leaves for the caller's assignUids.
u64 newUid(ArrangeContext& ctx) { return ctx.nextUid ? (*ctx.nextUid)++ : 0; }

// ---------------------------------------------------------------------------
// NXTAKT_DEBUG_PROBE -- the arrangement's half of the headless drive
//
// One line per lane that changed, listing every item's uid, start, length and
// fades, plus the loop and the selection. A gesture driven from outside the
// window is only a CHECK if the model can be read back and compared afterwards,
// and this is the read-back for everything ArrangeView owns.
//
// Read-only and additive: it takes the context by const reference, it is
// printed only on a frame that already reported a change, and with the variable
// unset it is one cached bool. It is deliberately not a per-frame dump -- what
// a drive script needs is "this gesture moved these values", which means the
// line has to be tied to the change and not to the clock.
// ---------------------------------------------------------------------------
bool probeOn() {
    static const bool on = std::getenv("NXTAKT_DEBUG_PROBE") != nullptr;
    return on;
}

// A marker's label, truncated to fit `maxW`.
//
// ASCII ONLY for the tail, and that is not laziness: the glyph atlas is 32..126
// (gfx/font.h), so a U+2026 ellipsis renders as three invisible bytes -- the
// exact failure the status bar's separators had before they became a hyphen.
// Two dots is legible and is in the atlas.
//
// The back-off loop refuses to cut a UTF-8 sequence in half. Those bytes have no
// glyph either way, but half a sequence is worse than none of it: it is what
// turns a name into mojibake in a copy-paste or a later font.
std::string markerLabel(const Font& f, const std::string& name, f32 maxW) {
    if (name.empty()) return std::string();
    if (f.measure(name.c_str()) <= maxW) return name;
    const f32 dots = f.measure("..");
    std::string out;
    for (size_t i = 0; i < name.size(); ++i) {
        out.push_back(name[i]);
        if (f.measure(out.c_str()) + dots > maxW) { out.pop_back(); break; }
    }
    while (!out.empty() && ((unsigned char)out.back() & 0xC0) == 0x80) out.pop_back();
    if (!out.empty() && ((unsigned char)out.back() & 0x80)) out.pop_back();
    return out + "..";
}

// NXTAKT_DEBUG_ARRHIT -- the hit-zone ruler.
//
// A drag edge's real width cannot be read out of the source: it is the product
// of a constant, a DPI scale, an item's width on screen and the order of an
// else-if chain, and every previous audit that "measured" one by reading the
// header measured the wrong thing. This logs the resolved answer under the
// pointer -- the zone, the item, the rect, the scale -- and it logs it only when
// the answer CHANGES, so sweeping the pointer along a row of pixels prints the
// zone map directly and one line per boundary.
//
// Off unless the variable is set; one cached bool when it is not.
bool hitProbeOn() {
    static const bool on = std::getenv("NXTAKT_DEBUG_ARRHIT") != nullptr;
    return on;
}
void probeHit(const char* what, f32 mx, f32 my, f32 s, f32 x0, f32 x1,
              f32 scrollX, f32 scrollY) {
    if (!hitProbeOn()) return;
    static std::string last;
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s span=%.1f..%.1f", what, (double)x0, (double)x1);
    if (last == buf) return;
    last = buf;
    // The scroll is OUTSIDE the dedupe key on purpose: it is what a pan moves,
    // and a line that only reprinted when the zone changed could never show it.
    LOGI("NXTAKT_DEBUG_ARRHIT: at %.0f,%.0f s=%.2f scroll=%.1f,%.1f -> %s",
         (double)mx, (double)my, (double)s, (double)scrollX, (double)scrollY, buf);
}

void probeArrange(const ArrangeContext& ctx, u32 changed, int selTrack, u64 selItem) {
    // THE REQUESTS, and the blind spot they close. wantDelete / wantSplit /
    // wantCreate are honoured by the CALLER, after this function has run, and
    // none of them sets a bit in `changed` -- so a right-click that deleted an
    // item left no trace in the probe log at all, and a headless run could only
    // see it by comparing screenshots. They are logged on their own trigger, the
    // same way the markers' requests already are.
    if (probeOn() && (ctx.wantDelete || ctx.wantSplit || ctx.wantCreate))
        LOGI("NXTAKT_DEBUG_PROBE: arrreq del=%d split=%d create=%d t=%d beat=%.4f "
             "sel=%d/%llu",
             (int)ctx.wantDelete, (int)ctx.wantSplit, (int)ctx.wantCreate,
             ctx.createTrack, ctx.createBeat, selTrack, (unsigned long long)selItem);
    if (!probeOn() || !changed) return;
    LOGI("NXTAKT_DEBUG_PROBE: arr changed=0x%02x sel=%d/%llu loop=%.4f..%.4f %s",
         changed, selTrack, (unsigned long long)selItem,
         ctx.loopStart ? *ctx.loopStart : -1.0, ctx.loopEnd ? *ctx.loopEnd : -1.0,
         (ctx.loopOn && *ctx.loopOn) ? "on" : "off");
    for (size_t i = 0; i < ctx.lanes.size(); ++i) {
        const std::vector<ArrangeClip>* v = ctx.lanes[i].items;
        if (!v) continue;
        for (size_t k = 0; k < v->size(); ++k) {
            const ArrangeClip& c = (*v)[k];
            LOGI("NXTAKT_DEBUG_PROBE: arr t=%zu i=%zu uid=%llu start=%.4f len=%.4f "
                 "off=%.4f fin=%.4f fout=%.4f",
                 i, k, (unsigned long long)c.uid, c.start, c.length, c.offset,
                 c.fadeIn, c.fadeOut);
        }
        const std::vector<AutoLane>* a = ctx.lanes[i].autos;
        if (!a) continue;
        for (size_t j = 0; j < a->size(); ++j)
            for (size_t p = 0; p < (*a)[j].points.size(); ++p)
                LOGI("NXTAKT_DEBUG_PROBE: arrauto t=%zu l=%zu p=%zu beat=%.4f val=%.5f",
                     i, j, p, (*a)[j].points[p].beat, (double)(*a)[j].points[p].value);
    }
}

// The markers' own probe, on its own trigger. probeArrange fires on `changed`,
// and a marker gesture changes NOTHING in that mask -- markers are not items,
// not lanes and not the brace -- so a marker edit driven inside gamescope would
// otherwise leave no trace at all. This logs the request the band produced and,
// with it, the list as the view was drawing it, which is what lets a headless
// run assert a gesture rather than eyeball a screenshot of it.
void probeMarkers(const std::vector<Marker>* v, const ArrangeView::MarkerReq& q,
                  u64 sel, u64 queued) {
    if (!probeOn() || q.kind == ArrangeView::MarkerReq::Kind::None) return;
    static const char* kName[] = {"none", "add", "remove", "move", "rename", "jump"};
    LOGI("NXTAKT_DEBUG_PROBE: mkreq %s uid=%llu beat=%.4f name='%s' gesture=%llu "
         "sel=%llu queued=%llu",
         kName[(int)q.kind], (unsigned long long)q.uid, q.beat, q.name.c_str(),
         (unsigned long long)q.gesture, (unsigned long long)sel,
         (unsigned long long)queued);
    if (!v) return;
    for (size_t i = 0; i < v->size(); ++i)
        LOGI("NXTAKT_DEBUG_PROBE: mkr %zu uid=%llu beat=%.4f col=%d name='%s'",
             i, (unsigned long long)(*v)[i].uid, (*v)[i].beat, (*v)[i].colorIdx,
             (*v)[i].name.c_str());
}

} // namespace

int ArrangeView::indexOf(const std::vector<ArrangeClip>& v, u64 uid) {
    if (!uid) return -1;
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i].uid == uid) return (int)i;
    return -1;
}

// ---------------------------------------------------------------------------
// the one-shot verbs
//
// Each is what a keyboard or a right-click asks for, and each leaves the lane
// repaired and named in ctx.dirty. The caller has already taken its undo point:
// these are one-shot, so there is no gesture to coalesce on and no way for the
// view to warn ahead.
// ---------------------------------------------------------------------------

u32 ArrangeView::deleteSelected(ArrangeContext& ctx) {
    if (selTrack_ < 0 || selTrack_ >= (int)ctx.lanes.size()) return Changed::None;
    std::vector<ArrangeClip>* items = ctx.lanes[(size_t)selTrack_].items;
    if (!items) return Changed::None;
    const int i = indexOf(*items, selItem_);
    if (i < 0) return Changed::None;
    items->erase(items->begin() + i);
    arrangeRepair(*items);
    ctx.dirty.push_back(selTrack_);
    selItem_ = 0;
    ctx.selItem = 0;
    lastEdit_ = kEditDel;
    return Changed::Items | Changed::Selection;
}

u32 ArrangeView::splitSelected(ArrangeContext& ctx) {
    if (selTrack_ < 0 || selTrack_ >= (int)ctx.lanes.size()) return Changed::None;
    std::vector<ArrangeClip>* items = ctx.lanes[(size_t)selTrack_].items;
    if (!items) return Changed::None;
    const int i = indexOf(*items, selItem_);
    if (i < 0) return Changed::None;
    ArrangeClip& a = (*items)[(size_t)i];
    const f64 at = quantNear(cursorBeat_);
    // A split that would leave either half unreachable is not a split. The
    // floor is kMinArrBeats on BOTH halves, because arrangeRepair would delete
    // whichever fell under it and the user would have asked for two items and
    // been given one, shorter than the one they started with.
    if (at <= a.start + kMinArrBeats || at >= a.end() - kMinArrBeats) return Changed::None;

    ArrangeClip b = a;                      // the copy is the whole item, §2.2
    b.uid = newUid(ctx);                    // stamped now: see ArrangeContext
    const f64 cut = at - a.start;
    b.start  = at;
    b.offset = a.offset + cut;              // the head trim rule, applied to a split
    b.length = a.length - cut;
    // The fades belong to the outer edges: the new inner edges are a butt joint,
    // which is exactly what R3 requires to be inaudible (§3.5).
    b.fadeIn = 0.0;
    a.length = cut;
    a.fadeOut = 0.0;
    if (a.fadeIn > a.length) a.fadeIn = a.length;
    if (b.fadeOut > b.length) b.fadeOut = b.length;
    const u64 keep = a.uid;
    items->insert(items->begin() + (long)i + 1, std::move(b));
    arrangeRepair(*items);
    selItem_ = keep;                        // the head keeps the selection
    ctx.selItem = keep;
    ctx.dirty.push_back(selTrack_);
    lastEdit_ = kEditSplit;
    return Changed::Items;
}

u32 ArrangeView::duplicateSelected(ArrangeContext& ctx) {
    if (selTrack_ < 0 || selTrack_ >= (int)ctx.lanes.size()) return Changed::None;
    std::vector<ArrangeClip>* items = ctx.lanes[(size_t)selTrack_].items;
    if (!items || (int)items->size() >= kMaxArrItems) return Changed::None;
    const int i = indexOf(*items, selItem_);
    if (i < 0) return Changed::None;
    ArrangeClip b = (*items)[(size_t)i];
    b.uid = newUid(ctx);
    b.start = b.end();
    const u64 copy = b.uid;
    items->insert(items->begin() + (long)i + 1, std::move(b));
    arrangeRepair(*items);
    ctx.dirty.push_back(selTrack_);
    // The selection follows into the COPY, for duplicateLoop's reason: the copy
    // is what the user is about to move. It can only do so because the copy has
    // an identity the moment it is made.
    if (copy) { selItem_ = copy; ctx.selItem = copy; }
    lastEdit_ = kEditDup;
    return Changed::Items | Changed::Selection;
}

// ---------------------------------------------------------------------------
// draw
// ---------------------------------------------------------------------------

u32 ArrangeView::draw(Ui& ui, const Rect& r, ArrangeContext& ctx) {
    if (!ui.r || !ui.in) return Changed::None;
    Renderer& rr = *ui.r;
    Input& in = *ui.in;
    const f32 s = dpiOf(ui);
    u32 changed = Changed::None;

    // ALT BYPASSES SNAP, and it is the primary spelling because it is FL's --
    // and Live's, and Bitwig's, and this editor's own automation lane's, which
    // has taken Alt for exactly this since it was extracted (autolane.cpp).
    // Before this pass the timeline took SHIFT for it and Alt did nothing, so a
    // hand that knew the modifier from anywhere else got a quantized answer and
    // no explanation.
    //
    // SHIFT IS STILL BOUND, and where it is not it is because it collides:
    // FL spends Shift+drag on CLONING an item, which is the louder of the two
    // meanings and the one a hand reaches for more often. So Shift frees the
    // grid on every drag on this surface EXCEPT a clip MOVE, where it makes a
    // copy instead; Alt frees the grid on all of them including that one. Every
    // gesture therefore has an unquantized spelling, and no gesture has two
    // meanings at once. This is the one place the two conventions could not both
    // be honoured, and it is written down here rather than discovered.
    const auto snapBeat = [&](f64 b) {
        return (in.alt() || in.shift()) ? b : quantNear(b);
    };

    // The caller owns the selection between frames (it is what the detail panel
    // reads), so it is adopted here rather than assumed: a project load or an
    // undo clears it, and a view that kept its own copy would go on drawing an
    // outline around an item the set no longer has.
    selTrack_ = ctx.selTrack;
    selItem_  = ctx.selItem;

    // The whole editor is a WELL: the timeline is the material the music sits
    // in, recessed below the chrome that frames it (docs/DESIGN.md §4). The
    // header column and the ruler are painted back up to panel tone below, so
    // what is left recessed is exactly the working area.
    rr.well(r, 0.f, true);
    const f32 headW = kArrHeaderW * s;
    if (r.w < headW + 80.f * s || r.h < 60.f * s) return changed;

    // `ruler` is BOTH bands and is what the body sits below; `mband` is the
    // markers' strip on top and `bruler` is the bar strip that was the whole of
    // the ruler until this wave. Everything the old code called `ruler` is now
    // `bruler` -- the bar numbers, the signature tags, the brace, the locate
    // click -- and the only two things that still span both are the panel fill
    // and the playhead, because those are about the ruler as an object rather
    // than about either band's contents.
    const Rect ruler{r.x + headW, r.y, r.w - headW, kArrRulerTotal * s};
    const Rect mband{ruler.x, ruler.y, ruler.w, kArrMarkerH * s};
    const Rect bruler{ruler.x, mband.bottom(), ruler.w, kArrRulerH * s};
    const Rect corner{r.x, r.y, headW, ruler.h};
    const Rect body{r.x, ruler.bottom(), r.w, r.h - ruler.h};
    const Rect heads{body.x, body.y, headW, body.h};
    const Rect lanes{body.x + headW, body.y, body.w - headW, body.h};

    // --- geometry ----------------------------------------------------------
    // One pass over the lanes to work out how tall the stack is and where each
    // track's band starts. Done before anything is drawn or hit-tested, so the
    // two can never disagree about which lane a y is in.
    struct Row { f32 y = 0, h = 0, autoY = 0, autoH = 0; int autos = 0; };
    std::vector<Row> rows(ctx.lanes.size());
    f32 contentH = 0.f;
    for (size_t i = 0; i < ctx.lanes.size(); ++i) {
        const ArrangeContext::Lane& L = ctx.lanes[i];
        Row& row = rows[i];
        row.y = contentH;
        row.h = clampv(L.height ? *L.height : kArrHeightDefault,
                       kArrMinLaneH, kArrMaxLaneH) * s;
        // One row per existing lane plus one for the chooser that adds the next.
        row.autos = (L.expanded && *L.expanded && L.autos)
                        ? (int)L.autos->size() + 1
                        : 0;
        row.autoY = row.y + row.h;
        row.autoH = (f32)row.autos * kArrAutoLaneH * s;
        contentH += row.h + row.autoH + 1.f * s;
    }

    f64 contentBeats = kArrMinContent;
    for (const ArrangeContext::Lane& L : ctx.lanes)
        if (L.items && !L.items->empty())
            contentBeats = std::max(contentBeats, L.items->back().end() + kArrTailBeats);
    if (ctx.loopEnd) contentBeats = std::max(contentBeats, *ctx.loopEnd + kArrTailBeats);
    contentBeats = std::max(contentBeats, ctx.playhead + kArrTailBeats);

    // THE LAYOUT, ONCE, for whatever is driving this from outside the window.
    // A gesture script has to aim at a pixel, and every band's y depends on
    // things the script cannot see -- the engine banner appears and disappears
    // with the link state and moves the whole editor 24 px down. Two runs of the
    // same script therefore aimed at two different bands, which is a false
    // finding factory. Logged on the first frame and whenever it moves.
    if (hitProbeOn()) {
        static char lastLayout[256] = {0};
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      "rect=%.0f,%.0f %.0fx%.0f marker=%.0f..%.0f bars=%.0f..%.0f "
                      "lanes.x=%.0f lanes.y=%.0f rows=%zu",
                      (double)r.x, (double)r.y, (double)r.w, (double)r.h,
                      (double)mband.y, (double)mband.bottom(),
                      (double)bruler.y, (double)bruler.bottom(),
                      (double)lanes.x, (double)lanes.y, rows.size());
        if (std::strcmp(buf, lastLayout) != 0) {
            std::snprintf(lastLayout, sizeof lastLayout, "%s", buf);
            LOGI("NXTAKT_DEBUG_ARRHIT: layout %s", buf);
            for (size_t i = 0; i < rows.size(); ++i)
                LOGI("NXTAKT_DEBUG_ARRHIT: row %zu y=%.0f..%.0f auto=%.0f..%.0f n=%d",
                     i, (double)(lanes.y + rows[i].y),
                     (double)(lanes.y + rows[i].y + rows[i].h),
                     (double)(lanes.y + rows[i].autoY),
                     (double)(lanes.y + rows[i].autoY + rows[i].autoH), rows[i].autos);
        }
    }

    // --- wheel -------------------------------------------------------------
    //
    // ONE NOTCH, ONE ANSWER. The header column scrolls the lane stack on a
    // plain wheel; the chooser row inside it holds a `selector`, and a selector
    // spends the wheel on stepping its option. Both used to fire on the same
    // notch -- the automation target advanced AND the stack scrolled under it --
    // because this block runs before the header is drawn and neither knew about
    // the other. The chooser row's geometry is already known here (the layout
    // pass above ran first), so the wheel simply declines it.
    //
    // The pre-wheel scroll offset is the right one to test against: it is where
    // the row was when the notch arrived, which is what the hand was aiming at.
    const f32 preTopY = lanes.y - scrollY_;
    bool overChooser = false;
    if (heads.contains(in.mx, in.my)) {
        for (size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].autos <= 0) continue;
            const f32 y0 = preTopY + rows[i].autoY +
                           (f32)(rows[i].autos - 1) * kArrAutoLaneH * s;
            if (in.my >= y0 && in.my < y0 + kArrAutoLaneH * s) { overChooser = true; break; }
        }
    }
    const bool overBody = (lanes.contains(in.mx, in.my) ||
                           (heads.contains(in.mx, in.my) && !overChooser) ||
                           ruler.contains(in.mx, in.my)) &&
                          rr.currentClip().contains(in.mx, in.my);
    f32 pxPerBeat = zoom_ * s;
    if (overBody && in.wheel != 0.f) {
        if (in.ctrl()) {
            // Zoom about the cursor, through the roll's own zoomView: the beat
            // under the hand stays under the hand, and a notch is the same
            // proportion at every scale. Identical feel in both editors is not a
            // nicety — it is the reason timeaxis.h exists.
            const f32 nz = clampv(zoom_ * std::pow(2.f, in.wheel * kZoomPerNotch),
                                  kZoomMin, kZoomMax);
            if (nz != zoom_) {
                const TimeAxis prev{lanes.x, pxPerBeat, scrollX_};
                zoom_ = nz;
                pxPerBeat = zoom_ * s;
                scrollX_ = zoomView(prev, pxPerBeat, clampv(in.mx, lanes.x, lanes.right()),
                                    contentBeats, lanes.w);
            }
        } else if (in.shift()) {
            scrollX_ -= in.wheel * pxPerBeat * 0.5f;
        } else {
            scrollY_ -= in.wheel * 40.f * s;
        }
    }

    // --- MIDDLE-DRAG PANS ---------------------------------------------------
    //
    // BOTH AXES, from anywhere on the editor, and it is the gesture a hand that
    // has used FL, Live, Bitwig or a map reaches for without being told. Until
    // this pass the middle button did nothing at all on this surface: the only
    // way along the timeline was the scrollbar-less wheel (vertical) or
    // Shift+wheel (horizontal), which is two gestures for one movement and
    // neither of them diagonal.
    //
    // It claims `drag_`, which is what makes it exclusive: every "is anything
    // held" guard below -- the brace grab, the flag hit test, the item hit test,
    // the paint and erase strokes -- is already written against drag_ == None,
    // so a pan cannot also select, trim or erase anything it passes over.
    //
    // The header column pans too, and only vertically, because that is the one
    // axis it has: it scrolls with the lane stack and never with time.
    const bool overEditor = ruler.contains(in.mx, in.my) || body.contains(in.mx, in.my);
    if (in.pressed[1] && drag_ == Drag::None && overEditor &&
        rr.currentClip().contains(in.mx, in.my)) {
        drag_ = Drag::Pan;
        gesture_ = 0;                // a pan is not an edit: nothing to undo
        moved_ = true;
        panGrabX_ = in.mx;  panGrabY_ = in.my;
        panOrigX_ = scrollX_; panOrigY_ = scrollY_;
    }
    if (drag_ == Drag::Pan) {
        if (!in.down[1]) {
            drag_ = Drag::None;
        } else {
            // Absolute against the press, not integrated: see the members' note.
            // The header column has no time axis, so a pan started in it moves
            // only the stack -- otherwise the lanes would slide under a hand
            // that never left the names.
            if (!heads.contains(panGrabX_, panGrabY_))
                scrollX_ = panOrigX_ - (in.mx - panGrabX_);
            scrollY_ = panOrigY_ - (in.my - panGrabY_);
        }
    }

    const f32 contentW = (f32)(contentBeats * (f64)pxPerBeat);
    scrollX_ = clampv(scrollX_, 0.f, std::max(0.f, contentW - lanes.w));
    scrollY_ = clampv(scrollY_, 0.f, std::max(0.f, contentH - lanes.h));

    const TimeAxis ta{lanes.x, pxPerBeat, scrollX_};
    const f32 topY = lanes.y - scrollY_;

    // The clip lanes claim their hot rect HERE, before the automation lanes are
    // drawn: setHot is last-writer-wins, so an expanded lane's own rect has to
    // be claimed after this one or it could never be hot.
    const u64 lanesId = uiId(UiArrange, 7);
    ui.setHot(lanesId, lanes);
    const bool hotLanes = ui.isHot(lanesId);
    if (lanes.contains(in.mx, in.my)) cursorBeat_ = std::max(0.0, xToBeat(ta, in.mx));
    else if (!lanes.contains(in.mx, in.my) && drag_ == Drag::None) cursorBeat_ = ctx.playhead;

    // Which track a y lands in, and -1 for the gaps. Both the drop target and a
    // cross-track move go through this, so a move cannot land somewhere the eye
    // says is a different lane.
    const auto trackAtY = [&](f32 y) {
        for (size_t i = 0; i < rows.size(); ++i) {
            const f32 y0 = topY + rows[i].y;
            if (y >= y0 && y < y0 + rows[i].h + rows[i].autoH) return (int)i;
        }
        return -1;
    };

    // --- ruler: bar numbers, the loop brace, the playhead -------------------
    // THE RULER IS THE BOUNDARY between chrome and work: panel-toned, so it
    // belongs to the frame, and underlined with a hairline rather than a rule,
    // because a solid grey divider is a §11 finding.
    rr.rect(ruler, tl::panelFill);
    rr.rect(corner, tl::panelFill);
    rr.hairlineH(ruler.x, ruler.right(), ruler.bottom() - 1.f * s);
    rr.hairlineV(corner.right() - 1.f * s, corner.y, corner.bottom());
    // The seam between the two bands. A hairline and not a rule, for the reason
    // the ruler's own underline is one -- and it is what makes the marker band
    // read as a strip of its own rather than as headroom above the bar numbers,
    // which is what tells a hand that the two halves answer to different clicks.
    rr.hairlineH(ruler.x, ruler.right(), bruler.y, nx::hairlineInk, 1.f * s);

    // The hot rect is the BAR band only. Everything below -- the brace grab, the
    // locate click, the signature right-click -- therefore stops at the seam and
    // cannot be triggered from the marker band, which is the disambiguation
    // kArrMarkerH's note describes.
    const u64 rulerId = uiId(UiArrange, 0);
    ui.setHot(rulerId, bruler);
    const bool hotRuler = ui.isHot(rulerId);

    // THE BRACE'S ENDS, which until this pass had a zone of exactly zero px.
    // The brace draws two 1.5 px uprights that look like handles and were not:
    // every press on this ruler started a fresh brace from the press point, so
    // adjusting one end meant redrawing both and the two uprights were pure
    // decoration. -1 for neither end, 0 for the start, 1 for the end.
    //
    // Grabbing an end is expressed as the SAME Drag::Loop with the anchor put
    // on the other end and loopMoved_ pre-set: the brace already exists, so
    // there is no "was this a locate or a drag" question left to answer, and
    // the whole drag body below is reused rather than duplicated.
    int braceEnd = -1;
    if (hotRuler && drag_ == Drag::None && ctx.loopStart && ctx.loopEnd &&
        *ctx.loopEnd > *ctx.loopStart) {
        const f32 g = kArrLoopGrab * s;
        const f32 bx0 = beatToX(ta, *ctx.loopStart), bx1 = beatToX(ta, *ctx.loopEnd);
        const f32 d0 = std::fabs(in.mx - bx0), d1 = std::fabs(in.mx - bx1);
        // Nearest wins, so a brace dragged down to a couple of pixels wide
        // still hands each half of itself to the end it is nearer.
        if (d0 <= g || d1 <= g) braceEnd = (d0 <= d1) ? 0 : 1;
        if (braceEnd >= 0) {
            const f32 bx = braceEnd == 0 ? bx0 : bx1;
            char what[96];
            std::snprintf(what, sizeof what, "brace end %d grab=%.1f", braceEnd, (double)g);
            probeHit(what, in.mx, in.my, s, bx - g, bx + g, scrollX_, scrollY_);
        } else if (hotRuler) {
            probeHit("bar band (locate / new brace)", in.mx, in.my, s, bruler.x,
                     bruler.right(), scrollX_, scrollY_);
        }
    }

    if (in.pressed[0] && hotRuler && drag_ == Drag::None) {
        drag_ = Drag::Loop;
        gesture_ = rulerId;
        if (braceEnd >= 0) {
            loopAnchor_ = braceEnd == 0 ? *ctx.loopEnd : *ctx.loopStart;
            loopMoved_ = true;
        } else {
            loopAnchor_ = std::max(0.0, snapBeat(xToBeat(ta, in.mx)));
            loopMoved_ = false;
        }
        moved_ = true;              // the brace is not an edit to any lane
        ui.active = rulerId;
    }
    // THE SIGNATURE EDITOR, half one: right-click on the ruler adds a change at
    // the bar under the cursor, or removes the one already there. The NEAREST
    // bar line and not the bar the cursor is inside -- a signature change is a
    // thing that sits on a bar line, and "the bar I clicked in" would make the
    // right half of every bar unable to reach the line on its right.
    //
    // Right-click, because both of the left button's jobs on this ruler are
    // already spoken for (a click locates, a drag is the loop brace) and because
    // it is the button this program already spends on "the other verb" -- a
    // right-click deletes an item in the lanes below.
    if (in.pressed[2] && hotRuler && drag_ == Drag::None) {
        const f64 cb = std::max(0.0, (f64)xToBeat(ta, in.mx));
        i64 bar = (i64)std::floor(ctx.sig.barOfBeat(cb) + 0.5);
        ctx.sigBar = bar < 0 ? 0 : bar;
    }

    if (drag_ == Drag::Loop) {
        if (!in.down[0]) {
            if (!loopMoved_) ctx.locateBeat = loopAnchor_;
            drag_ = Drag::None;
            gesture_ = 0;
            if (ui.active == rulerId) ui.active = 0;
        } else {
            const f64 b = std::max(0.0, snapBeat(xToBeat(ta, in.mx)));
            if (!loopMoved_ &&
                std::fabs(beatToX(ta, b) - beatToX(ta, loopAnchor_)) > 3.f * s)
                loopMoved_ = true;
            if (loopMoved_ && ctx.loopStart && ctx.loopEnd && ctx.loopOn) {
                *ctx.loopStart = std::min(loopAnchor_, b);
                *ctx.loopEnd   = std::max(loopAnchor_, b);
                *ctx.loopOn    = true;
                changed |= Changed::Loop;
            }
        }
    }

    // --- the marker band: locators ------------------------------------------
    //
    // The geometry is built ONCE, here, and is what both the hit test below and
    // the draw further down read. Two passes that each worked out where a flag
    // is would be two answers to "did I click it", which is the same trap the
    // lane layout pass at the top of this function exists to avoid.
    struct Flag {
        size_t i; u64 uid; f32 x;
        Rect box;               // what is drawn
        Rect hit;               // what is clickable -- the floor, plus slop
        std::string label;
    };
    std::vector<Flag> flags;
    if (markers_ && ui.fSmall) {
        for (size_t i = 0; i < markers_->size(); ++i) {
            const Marker& m = (*markers_)[i];
            const f32 fx = beatToX(ta, m.beat);
            // The list is sorted by beat, so the first flag past the right edge
            // is the last one worth looking at.
            if (fx > mband.right() + kArrMarkerSlop * s) break;
            std::string lab = markerLabel(*ui.fSmall, m.name, kArrMarkerMaxW * s);
            const f32 tw = lab.empty() ? 0.f : ui.fSmall->measure(lab.c_str());
            // THE EIGHT-PIXEL FLOOR. A flag's drawn width is its name, and a
            // name can be one letter or none at all; the zone is the greater of
            // what it draws and what a hand can hit.
            const f32 w = std::max(kArrMarkerGrab * s, tw + 8.f * s);
            const Rect box{fx, mband.y + 1.f * s, w, mband.h - 3.f * s};
            if (box.right() < mband.x - kArrMarkerSlop * s) continue;
            const Rect hit{box.x - kArrMarkerSlop * s, mband.y,
                           box.w + 2.f * kArrMarkerSlop * s, mband.h};
            flags.push_back({i, m.uid, fx, box, hit, std::move(lab)});
        }
    }

    const u64 bandId = uiId(UiArrange, 9);
    ui.setHot(bandId, mband);
    const bool hotBand = ui.isHot(bandId);

    // NEAREST POLE WINS inside an overlap, which is the brace ends' rule applied
    // to a row of things rather than to two: at a zoom where two flags' slop
    // rectangles meet, the one whose pole the pointer is closer to is the one
    // the hand is aiming at.
    // Computed during an erase stroke as well as at rest: the stroke's whole job
    // is to keep asking "what is under the pointer NOW".
    int hitFlag = -1;
    if (hotBand && (drag_ == Drag::None || drag_ == Drag::EraseMarker)) {
        f32 best = 1e9f;
        for (size_t k = 0; k < flags.size(); ++k) {
            if (!flags[k].hit.contains(in.mx, in.my)) continue;
            const f32 d = std::fabs(in.mx - flags[k].x);
            if (d < best) { best = d; hitFlag = (int)k; }
        }
        if (hitFlag >= 0) {
            const Flag& hf = flags[(size_t)hitFlag];
            char what[128];
            std::snprintf(what, sizeof what, "flag uid=%llu box=%.1fx%.1f hit=%.1fx%.1f",
                          (unsigned long long)hf.uid, (double)hf.box.w, (double)hf.box.h,
                          (double)hf.hit.w, (double)hf.hit.h);
            probeHit(what, in.mx, in.my, s, hf.hit.x, hf.hit.right(), scrollX_, scrollY_);
        } else {
            probeHit("marker band (empty)", in.mx, in.my, s, mband.y, mband.bottom(),
                     scrollX_, scrollY_);
        }
    }

    // The gestures. Right-click deletes and double-click creates, which are the
    // same two verbs the lanes below already answer with the same two buttons --
    // and they cannot be confused with the bar band's right-click, because that
    // band's hot rect stops at the seam.
    if (hotBand && drag_ == Drag::None && renameMarker_ == 0) {
        if (in.pressed[2]) {
            // RIGHT-CLICK ERASES, AND RIGHT-DRAG KEEPS ERASING. A press on a
            // flag removes it, as it always did; the press also arms a stroke,
            // so sweeping the button across a row of flags takes every one it
            // crosses. That is FL's gesture and it is the reason a right-click
            // that removed exactly one thing per press always felt broken here:
            // the hand does not lift the button between two flags a bar apart.
            //
            // Armed even when the press lands on EMPTY band, because the sweep
            // that clears a stretch of ruler usually starts beside the first
            // flag rather than on it.
            drag_ = Drag::EraseMarker;
            gesture_ = bandId;
            moved_ = true;              // no lane of items is going to move
            if (hitFlag >= 0) {
                markerReq_.kind = MarkerReq::Kind::Remove;
                markerReq_.uid  = flags[(size_t)hitFlag].uid;
                if (selMarker_ == markerReq_.uid) selMarker_ = 0;
            }
        } else if (in.pressed[0] && hitFlag >= 0) {
            const Flag& f = flags[(size_t)hitFlag];
            selMarker_ = f.uid;
            if (in.dblClick) {
                // The rename. The FIRST click of the double already jumped, and
                // that is the right order rather than a wart: a jump is harmless
                // and instantaneous, and making the rename wait for a gesture
                // that does not also jump would cost it its only obvious home.
                renameMarker_ = f.uid;
                renameBuf_    = (*markers_)[f.i].name;
            } else {
                // A click JUMPS. The caller decides whether that is a locate now
                // or a queued one, because whether the transport is running is
                // its business and not the ruler's.
                markerReq_.kind = MarkerReq::Kind::Jump;
                markerReq_.uid  = f.uid;
                drag_       = Drag::Marker;
                gesture_    = uiId(UiArrange, 10, (int)(u32)f.uid, (int)++strokeSeq_);
                dragMarker_ = f.uid;
                markerOrig_ = (*markers_)[f.i].beat;
                markerGrab_ = (f64)xToBeat(ta, in.mx) - markerOrig_;
                moved_      = false;
                ui.active   = gesture_;
            }
        } else if (in.pressed[0] && in.dblClick) {
            // Empty band. The same double-click that fills an empty lane with a
            // note block fills an empty ruler with a marker.
            markerReq_.kind = MarkerReq::Kind::Add;
            markerReq_.beat = std::max(0.0, snapBeat(xToBeat(ta, in.mx)));
        }
    }

    // The marker erase stroke. One Remove per frame, because MarkerReq carries
    // one request and one pointer can only be over one flag at a time; the
    // caller removes it, the list is rebound next frame without it, and the
    // stroke simply asks again. There is no "already erased" bookkeeping to get
    // wrong for exactly that reason.
    if (drag_ == Drag::EraseMarker) {
        if (!in.down[2]) {
            drag_ = Drag::None;
            gesture_ = 0;
        } else if (hitFlag >= 0 && markerReq_.kind == MarkerReq::Kind::None) {
            markerReq_.kind = MarkerReq::Kind::Remove;
            markerReq_.uid  = flags[(size_t)hitFlag].uid;
            if (selMarker_ == markerReq_.uid) selMarker_ = 0;
        }
    }

    if (drag_ == Drag::Marker) {
        if (!in.down[0]) {
            if (ui.active == gesture_) ui.active = 0;
            drag_ = Drag::None;
            gesture_ = 0;
            dragMarker_ = 0;
        } else {
            // Measured from the beat the flag had at the press, exactly as every
            // item drag is, so a drag is absolute against its own start and
            // cannot integrate its own rounding. ALT (and, still, Shift) frees
            // the grid -- see snapBeat at the top of this function for why both
            // and why Alt is the one documented first.
            const f64 raw = (f64)xToBeat(ta, in.mx) - markerGrab_;
            const f64 b = std::max(0.0, snapBeat(raw));
            if (!moved_ &&
                std::fabs(beatToX(ta, b) - beatToX(ta, markerOrig_)) > 3.f * s) {
                // THE UNDO HANDSHAKE, verbatim: the edit is named on the frame
                // the drag first travels and BEFORE anything moves, the caller
                // takes its point, and the move below is what it took it against.
                moved_ = true;
                pendingEdit_ = kEditMarkerMove;
            }
            if (moved_) {
                markerReq_.kind    = MarkerReq::Kind::Move;
                markerReq_.uid     = dragMarker_;
                markerReq_.beat    = b;
                markerReq_.gesture = gesture_;
            }
        }
    }

    rr.pushClip(ruler);
    if (ui.fSmall)
        drawRulerLabels(rr, *ui.fSmall, ta, bruler.x, bruler.right(),
                        bruler.y + (bruler.h - ui.fSmall->height()) * 0.5f, s,
                        tl::rulerOnBar, tl::rulerOffBar, ctx.sig, 44.f * s);
    // The signature markers. Drawn ONLY for a map that has more than one entry,
    // which is not a shortcut: a set in one signature has nothing to mark -- the
    // control bar already says what it is -- and a tag on bar 1 of every existing
    // set would be a change to a ruler this wave promised not to change.
    //
    // Over the bar number rather than beside it, because at the bar a signature
    // starts, "7/8" is the more useful of the two things that want that space.
    if (ui.fSmall && ctx.sig.count() > 1) {
        for (int i = 0; i < ctx.sig.count(); ++i) {
            const RtSig sg = ctx.sig.entry(i);
            const f32 mx = beatToX(ta, ctx.sig.beatOfBar((f64)sg.bar));
            if (mx < bruler.x - 40.f * s || mx > bruler.right()) continue;
            char buf[24];
            std::snprintf(buf, sizeof buf, "%d/%d", sg.num, sg.den);
            const f32 tw = ui.fSmall->measure(buf);
            const Rect tag{mx + 1.f * s, bruler.y + 3.f * s,
                           tw + 7.f * s, bruler.h - 6.f * s};
            // The one accent tag on the ruler, and it keeps its violet fill:
            // a signature change is identity, not decoration. inkOn picks the
            // ink -- §7's table says `text` clears 4.8:1 on a violet fill and
            // `muted` is illegible on one, so nothing secondary may go here.
            rr.roundRect(tag, 2.f * s, nx::violet.alpha(0.92f));
            rr.textIn(*ui.fSmall, tag, buf, nx::inkOn(nx::violet), Align::Center, 0.f);
            // The tag's own line stops at the seam. A signature change belongs
            // to the bar band; running it up through the marker band would put
            // a second vertical rule beside every flag that shares its bar and
            // make the two look like one thing.
            rr.rect({nx::snapPx(mx), bruler.y, std::max(1.f, s), bruler.h},
                    nx::violetSoft.alpha(0.85f));
        }
    }
    // The brace. A disabled loop still remembers where it was (session.h), so
    // it is drawn faintly rather than not at all: a brace that vanished when it
    // was switched off would make the toggle look like it erased something.
    if (ctx.loopStart && ctx.loopEnd && *ctx.loopEnd > *ctx.loopStart) {
        const f32 x0 = beatToX(ta, *ctx.loopStart), x1 = beatToX(ta, *ctx.loopEnd);
        const bool on = ctx.loopOn && *ctx.loopOn;
        // The brace is VIOLET -- the accent, because it is a thing the user set
        // rather than a thing that is happening. Cyan is reserved for the one
        // element on this surface that is live, and that is the playhead.
        const Col c = nx::violetSoft.alpha(on ? 0.95f : 0.34f);
        rr.rect({x0, bruler.y, std::max(1.f * s, x1 - x0), 3.f * s}, c);
        rr.rect({x0, bruler.y, 1.5f * s, bruler.h - 1.f * s}, c);
        rr.rect({x1 - 1.5f * s, bruler.y, 1.5f * s, bruler.h - 1.f * s}, c);
    }
    // THE FLAGS. A pole at the marker's beat and a labelled block hanging off
    // it to the right -- the shape a locator has had since Live invented it, and
    // the shape that makes "this names THAT beat" readable at a glance.
    //
    // VIOLET, because a marker is a thing the user SET, which is the same
    // argument the loop brace's colour note makes one screen up. Cyan stays
    // reserved for what is happening -- and that is exactly why a QUEUED jump
    // gets a cyan ring: at that moment the flag has stopped being only a place
    // and has become something the transport is about to do.
    if (ui.fSmall) {
        for (const Flag& f : flags) {
            const Marker& m = (*markers_)[f.i];
            // Colour 0 is the accent; anything else indexes the clip palette,
            // wrapped, so a file from a build with a wider palette draws a
            // sensible colour here instead of reading off the end of ours.
            const Col base = m.colorIdx <= 0
                                 ? nx::violet
                                 : pal::clipColors[(m.colorIdx - 1) % pal::clipColorCount];
            if (renameMarker_ == m.uid) {
                // The inline rename. A field wide enough to type a name into,
                // even when the flag it replaces is four pixels of "A".
                const Rect fr{f.box.x, mband.y + 1.f * s,
                              std::max(f.box.w, 104.f * s), mband.h - 3.f * s};
                const u64 fid = uiId(UiArrange, 11, (int)(u32)m.uid);
                if (ui.textField(fid, fr, &renameBuf_, nx::panel2, nx::text,
                                 Align::Left, true)) {
                    markerReq_.kind = MarkerReq::Kind::Rename;
                    markerReq_.uid  = m.uid;
                    markerReq_.name = renameBuf_;
                    renameMarker_ = 0;
                } else if (ui.editId != fid) {
                    renameMarker_ = 0;      // Escape, or a press somewhere else
                }
                continue;
            }
            const bool sel = m.uid == selMarker_;
            const Col fill = base.alpha(sel ? 0.98f : 0.80f);
            rr.rect({nx::snapPx(f.x), mband.y, std::max(1.f, s), mband.h},
                    base.mix(nx::violetSoft, 0.35f).alpha(sel ? 0.98f : 0.8f));
            rr.roundRect(f.box, 2.f * s, fill);
            if (!f.label.empty())
                rr.textIn(*ui.fSmall, f.box, f.label.c_str(), nx::inkOn(fill),
                          Align::Center, 0.f);
            // The selected flag is RINGED and not merely brighter: brightness
            // alone is a difference somebody has to be shown twice to see, and
            // the ring is what the focus vocabulary already uses everywhere else.
            if (sel) rr.roundRectOutline(f.box, 2.f * s, 1.f * s, nx::text.alpha(0.85f));
            if (m.uid == queuedMarker_)
                rr.roundRectOutline(f.box.inset(-1.5f * s), 3.f * s, 1.5f * s,
                                    nx::live.alpha(0.95f));
        }
    }
    {   // The playhead's head, and it spans BOTH bands: it is the one thing on
        // this ruler that belongs to neither, and a head that stopped at the
        // seam would leave the marker band looking like a different widget.
        const f32 px = beatToX(ta, ctx.playhead);
        if (px >= ruler.x && px <= ruler.right())
            tl::drawPlayhead(rr, px, ruler.y, ruler.h, s * 1.5f, true);
    }
    rr.popClip();

    if (ui.fSmall) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.0f px/beat", (double)zoom_);
        rr.textIn(*ui.fSmall, corner, buf, nx::muted.alpha(0.62f), Align::Left, 6.f * s);
    }

    // --- the lanes ---------------------------------------------------------
    rr.pushClip(lanes);
    rr.well(lanes, 0.f, true);
    drawTimeGrid(rr, ta, lanes, s, ctx.sig, kArrMinGridPx * s);
    // Past the loop end is still editable, so it is NOT dimmed the way the roll
    // dims past a clip's length: an arrangement has no end until something is
    // put there.

    for (size_t i = 0; i < ctx.lanes.size(); ++i) {
        const ArrangeContext::Lane& L = ctx.lanes[i];
        const Row& row = rows[i];
        const Rect band{lanes.x, topY + row.y, lanes.w, row.h};
        if (band.bottom() < lanes.y || band.y > lanes.bottom()) continue;

        // The stripe is a LIFT on the well, not a second fill: the canvas is
        // already there, and odd lanes only need enough violet to be countable.
        //
        // Which is also why the grid is no longer redrawn per band. It used to
        // be, because an opaque band fill painted the canvas-wide grid out; a 3%
        // violet does not, so one pass over `lanes` now serves every lane. On a
        // full arrangement that is the difference between one set of bar lines
        // and one per track.
        if (i % 2) rr.rect(band, tl::stripeLift);
        rr.hairlineH(band.x, band.right(), band.bottom() + row.autoH);

        if (!L.items) continue;
        const Col base = itemColour(L.colorIdx, L.overridden);
        for (const ArrangeClip& it : *L.items) {
            const f32 x0 = beatToX(ta, it.start);
            const f32 x1 = beatToX(ta, it.end());
            if (x1 < lanes.x - 2.f || x0 > lanes.right() + 2.f) continue;
            const Rect box{x0, band.y + 1.f * s, std::max(2.f * s, x1 - x0), band.h - 2.f * s};
            const bool sel = (int)i == selTrack_ && it.uid == selItem_ && it.uid != 0;

            // The item, in three flat quads. The colour stays SATURATED because
            // it is data -- which track this material came from -- and the only
            // thing added is a 1px darkened edge, so two items that touch read
            // as two items rather than as one wide one. No gradient, no shadow:
            // at a hundred items on screen either would be a slideshow.
            rr.roundRect(box, 2.f * s, base.scale(0.34f));
            rr.rect({box.x, box.y, box.w, std::min(box.h, 12.f * s)}, base.scale(0.66f));
            rr.roundRectOutline(box, 2.f * s, std::max(1.f, s), base.scale(0.16f));

            rr.pushClip(box.intersect(lanes));
            // The material, drawn against the SHARED axis: a waveform for a
            // sample, note stems for a pattern. Both honour `offset`, so an item
            // that begins a bar into its clip shows the bar it begins on.
            const f32 mid = box.y + 12.f * s + (box.h - 12.f * s) * 0.5f;
            const f32 halfH = std::max(2.f * s, (box.h - 12.f * s) * 0.5f - 2.f * s);
            if (it.src.kind == ClipKind::Audio && it.src.sample &&
                it.src.sample->peakBuckets > 0) {
                const SampleBuffer& sb = *it.src.sample;
                const Col wc = base.scale(0.95f);
                const f32 xa = std::max(box.x, lanes.x), xb = std::min(box.right(), lanes.right());
                for (f32 x = xa; x < xb; x += 1.f) {
                    f64 u = 0.0;
                    if (!srcFraction(it.src, it.offset + (xToBeat(ta, x) - it.start), u)) continue;
                    const int bk = clampv((int)(u * (f64)sb.peakBuckets), 0, sb.peakBuckets - 1);
                    const f32 lo = sb.peaks[(size_t)bk * 2 + 0];
                    const f32 hi = sb.peaks[(size_t)bk * 2 + 1];
                    const f32 y0 = mid - hi * halfH;
                    rr.rect({x, y0, 1.f, std::max(1.f, (mid - lo * halfH) - y0)}, wc);
                }
            } else if (it.src.kind == ClipKind::Midi && !it.src.notes.empty()) {
                const f64 clen = it.src.lengthBeats > 1e-9 ? it.src.lengthBeats : 1.0;
                const int k0 = it.src.loop ? (int)std::floor(it.offset / clen) : 0;
                const int k1 = it.src.loop
                                   ? (int)std::floor((it.offset + it.length) / clen)
                                   : 0;
                const f32 top = box.y + 13.f * s;
                const f32 hgt = std::max(3.f * s, box.bottom() - top - 2.f * s);
                const Col nc = base.scale(1.0f);
                for (int k = k0; k <= k1 && k - k0 < kMaxPreviewReps; ++k) {
                    for (const NoteModel& n : it.src.notes) {
                        const f64 b = it.start + ((f64)k * clen + n.beat - it.offset);
                        if (b + n.len <= it.start || b >= it.end()) continue;
                        const f32 nx0 = std::max(beatToX(ta, std::max(b, it.start)), box.x);
                        const f32 nx1 = std::min(beatToX(ta, std::min(b + n.len, it.end())),
                                                 box.right());
                        if (nx1 <= nx0) continue;
                        const f32 t = clampv(((f32)n.pitch - (f32)kPreviewLoPitch) /
                                                 (f32)(kPreviewHiPitch - kPreviewLoPitch),
                                             0.f, 1.f);
                        const f32 y = top + (1.f - t) * (hgt - 2.f * s);
                        rr.rect({nx0, y, std::max(1.f, nx1 - nx0), 2.f * s}, nc);
                    }
                }
            }

            // The fades, as CURVES rather than as ramps. The shaded wedge is
            // still one column per pixel -- that is what makes the edge smooth
            // at any zoom and it is the same loop the waveform above already
            // runs -- but the profile is now a raised cosine, which is what a
            // fade sounds like and therefore what it should look like. The edge
            // itself is a short polyline over the same curve, capped at
            // kFadeCurveSegs segments so a fade the width of the screen costs
            // the same handful of quads as one the width of a beat.
            const Col shade = nx::bgTop.alpha(0.80f);
            const Col curveInk = nx::text.alpha(0.34f);
            // Raised cosine, 0 at t=0 and 1 at t=1: the fraction of the item's
            // height the shade still covers.
            const auto profile = [](f32 t) {
                return 0.5f - 0.5f * std::cos(clampv(t, 0.f, 1.f) * 3.14159265f);
            };
            const auto curveEdge = [&](f32 xa, f32 xb, bool rising) {
                if (xb <= xa) return;
                // Segments scale with the width the curve is actually drawn at.
                // A fade eight pixels wide has no visible curvature to spend
                // eight quads on, and on a full arrangement most of them are.
                const int segs = clampv((int)((xb - xa) / (8.f * s)), 1, kFadeCurveSegs);
                f32 pxx = xa, pyy = box.y + (rising ? 0.f : box.h);
                for (int k = 1; k <= segs; ++k) {
                    const f32 t = (f32)k / (f32)segs;
                    const f32 x = xa + (xb - xa) * t;
                    const f32 y = box.y + box.h * (rising ? profile(t) : 1.f - profile(t));
                    rr.line(pxx, pyy, x, y, 1.f * s, curveInk);
                    pxx = x; pyy = y;
                }
            };
            if (it.fadeIn > 0.0) {
                const f32 fx1 = beatToX(ta, std::min(it.start + it.fadeIn, it.end()));
                for (f32 x = std::max(box.x, lanes.x); x < std::min(fx1, lanes.right()); x += 1.f) {
                    const f32 t = clampv((x - box.x) / std::max(1.f, fx1 - box.x), 0.f, 1.f);
                    rr.rect({x, box.y, 1.f, box.h * (1.f - profile(t))}, shade);
                }
                curveEdge(box.x, fx1, true);
            }
            if (it.fadeOut > 0.0) {
                const f32 fx0 = beatToX(ta, std::max(it.end() - it.fadeOut, it.start));
                for (f32 x = std::max(fx0, lanes.x); x < std::min(box.right(), lanes.right()); x += 1.f) {
                    const f32 t = clampv((x - fx0) / std::max(1.f, box.right() - fx0), 0.f, 1.f);
                    rr.rect({x, box.y, 1.f, box.h * profile(t)}, shade);
                }
                curveEdge(fx0, box.right(), false);
            }

            // The name as a MICRO-LABEL (§5): uppercase, wide-tracked, in the
            // ink §7 picks for the strip it sits on. It is a chip identifying
            // the material, not a sentence, and at 9 px the tracking is what
            // makes it read as one.
            if (ui.fSmall && box.w > 24.f * s)
                tl::microLabel(rr, *ui.fSmall, box.x + 4.f * s,
                               box.y + (12.f * s - ui.fSmall->height()) * 0.5f,
                               it.src.name.c_str(), nx::inkOn(base.scale(0.66f)),
                               box.w - 6.f * s);
            rr.popClip();
            (void)sel;
        }
        // The selection outline in a SECOND pass, after every item on the lane.
        // A crossfade pair overlaps by design, so the later item's body is drawn
        // over the earlier one -- and an outline drawn inside the first pass
        // would be painted out by exactly the neighbour that makes the pair
        // interesting.
        if ((int)i != selTrack_) continue;
        for (const ArrangeClip& it : *L.items) {
            if (it.uid == 0 || it.uid != selItem_) continue;
            const f32 x0 = beatToX(ta, it.start), x1 = beatToX(ta, it.end());
            if (x1 < lanes.x - 2.f || x0 > lanes.right() + 2.f) continue;
            const Rect sr{x0, band.y + 1.f * s, std::max(2.f * s, x1 - x0),
                          band.h - 2.f * s};
            // THE ONE ELEVATION CUE ON THIS SURFACE, and it is barely there: a
            // violet glow tucked under the selected item, light arriving from
            // the upper-left so the shadow falls down. It is permitted because
            // it is chrome-adjacent feedback -- "this is the one you are
            // holding" -- and it is drawn once, for one item, never per item.
            rr.shadow(sr, 2.f * s, nx::ShadowSpec{0.f, 2.f * s, 10.f * s, -1.f * s,
                                                  nx::violet.alpha(0.42f)});
            rr.roundRectOutline(sr, 2.f * s, std::max(1.f, s), nx::violet);
            rr.roundRectOutline(sr.inset(-1.f * s), 3.f * s, std::max(1.f, s),
                                nx::violetSoft.alpha(0.35f));
        }
    }

    // The playhead, over everything the lanes hold.
    {
        const f32 px = beatToX(ta, ctx.playhead);
        if (px >= lanes.x && px <= lanes.right())
            tl::drawPlayhead(rr, px, lanes.y, lanes.h, s, ctx.playing);
    }
    rr.popClip();

    // --- the automation lanes ----------------------------------------------
    // Drawn after the clip lanes and outside their clip, because each is its own
    // AutoLaneView with its own hot rect. Every one is handed the SAME TimeAxis
    // the items above it are drawn against, which is the property §7.3 exists
    // to guarantee.
    laneViews_.resize(ctx.lanes.size());
    targetSel_.resize(ctx.lanes.size(), 0);
    for (size_t i = 0; i < ctx.lanes.size(); ++i) {
        const ArrangeContext::Lane& L = ctx.lanes[i];
        const Row& row = rows[i];
        if (row.autos <= 0 || !L.autos) continue;
        std::vector<AutoLaneView>& views = laneViews_[i];
        if (views.size() < L.autos->size()) views.resize(L.autos->size());

        for (size_t j = 0; j < L.autos->size(); ++j) {
            const Rect lr{lanes.x, topY + row.autoY + (f32)j * kArrAutoLaneH * s,
                          lanes.w, kArrAutoLaneH * s};
            if (lr.bottom() < lanes.y || lr.y > lanes.bottom()) continue;
            AutoLane& al = (*L.autos)[j];
            const AutoTargets::Entry* tgt = L.targets ? L.targets->find(al.address) : nullptr;

            // A SHALLOWER well than the clip lanes above it: the envelope is
            // still working surface, but it sits a step nearer the surface than
            // the material it shapes.
            //
            // Which is a LIFT and not a second well, because the canvas under it
            // is already --well-deep and wells are translucent black: painting
            // --well on top of --well-deep makes it deeper, not shallower. This
            // alpha is the one that lands the result where --well over the bare
            // field lands, so the lane really is one step up rather than merely
            // a different dark.
            rr.rect(lr, nx::panel2.alpha(0.09f));
            rr.pushClip(lr.intersect(lanes));
            drawTimeGrid(rr, ta, lr, s, ctx.sig, kArrMinGridPx * s);
            AutoLaneView& v = views[j];
            v.setId(uiId(UiArrangeLane, (int)i, (int)j));

            // RIGHT-DRAG ERASES BREAKPOINTS, and it lives HERE rather than
            // inside AutoLaneView because the lane is shared with the piano
            // roll: a change in there would be a change to the roll's surface,
            // which this pass does not own. The lane's own right-click already
            // removes the point under the PRESS (autolane.cpp); this is the rest
            // of the stroke, and between them the gesture is FL's -- sweep the
            // right button and the envelope clears under it.
            //
            // The hit test is autoLanePointAt, declared beside kPtGrab in
            // autolane.h precisely so that this second caller and the lane's own
            // pointAt cannot come to different answers about where a point is.
            //
            // ONE UNDO ENTRY for the whole sweep, and the mechanism is subtle
            // enough to say out loud: the Autos bit is what makes
            // App::arrangeCommitAutos take a point, and it coalesces on
            // ui.active -- which the widget layer force-clears on every frame the
            // LEFT button is up, i.e. on every frame of a right-drag. So the bit
            // is raised on the FIRST erase only, where the frame's snapshot still
            // holds the lane as it was; every later erase reports itself through
            // ctx.dirty alone, which is what republishes the lane without asking
            // for a second entry.
            if (drag_ == Drag::ErasePoint && in.down[2] && !in.pressed[2] &&
                lr.contains(in.mx, in.my) && !al.points.empty()) {
                const f32 plo = tgt ? tgt->lo : 0.f, phi = tgt ? tgt->hi : 1.f;
                const int k = autoLanePointAt(al.points, ta, lr, s, plo, phi,
                                              in.mx, in.my, kPtGrab * s);
                if (k >= 0) {
                    al.points.erase(al.points.begin() + k);
                    // The lane holds indices into the vector that just shrank
                    // and has no "a point was erased under you" hook, so the
                    // selection goes rather than being left pointing one past
                    // whatever the user can see.
                    v.clearSelection();
                    ctx.dirty.push_back((int)i);
                    if (!moved_) { moved_ = true; changed |= Changed::Autos; }
                    lastEdit_ = kEditAutoErase;
                }
            }
            if (in.pressed[2] && drag_ == Drag::None && lr.contains(in.mx, in.my)) {
                // Armed on the press, so the sweep is live from the next frame.
                // The press itself is the lane's own to answer, and it does.
                drag_ = Drag::ErasePoint;
                gesture_ = uiId(UiArrange, 14, (int)i, (int)++strokeSeq_);
                // "The press already erased one." The lane answers the press
                // itself and raises the Autos bit for it, so the stroke must not
                // raise a second one for the same entry -- moved_ is the latch
                // that says the entry has been taken.
                const f32 plo = tgt ? tgt->lo : 0.f, phi = tgt ? tgt->hi : 1.f;
                moved_ = autoLanePointAt(al.points, ta, lr, s, plo, phi,
                                         in.mx, in.my, kPtGrab * s) >= 0;
            }

            v.prune((int)al.points.size());
            // beatBase 0: an arrangement lane's points are ALREADY absolute
            // timeline beats (session.h, TrackModel::arrangeAutos), which is the
            // one thing that differs from a clip envelope.
            if (v.draw(ui, lr, al.points, ta, tgt ? tgt->lo : 0.f, tgt ? tgt->hi : 1.f,
                       tgt ? tgt->unit.c_str() : nullptr, tgt ? tgt->def : 0.f,
                       al.enabled, false, contentBeats, 0.0, tgt != nullptr,
                       nullptr)) {
                changed |= Changed::Autos;
                ctx.dirty.push_back((int)i);
                lastEdit_ = kEditAuto;
            }
            const f32 px = beatToX(ta, ctx.playhead);
            if (px >= lr.x && px <= lr.right())
                tl::drawPlayhead(rr, px, lr.y, lr.h, s, ctx.playing);
            rr.popClip();
            rr.hairlineH(lr.x, lr.right(), lr.y);
        }
    }
    // The breakpoint sweep ends when the button does, wherever the pointer has
    // wandered to -- including off the lanes entirely, which is why this is out
    // here and not inside the loop that only runs for lanes that are on screen.
    if (drag_ == Drag::ErasePoint && !in.down[2]) {
        drag_ = Drag::None;
        gesture_ = 0;
    }

    // Whichever header control the pointer actually won, and the rect it won
    // with. The header column is where this surface's controls sit shoulder to
    // shoulder, and shoulder-to-shoulder plus a symmetric hit pad is how a
    // neighbour ends up owning pixels somebody aimed at the control beside it --
    // so the answer is logged rather than reasoned about.
    const auto probeRect = [&](const char* name, u64 id, const Rect& hit) {
        if (!hitProbeOn() || !ui.isHot(id)) return;
        char b[128];
        std::snprintf(b, sizeof b, "%s hit=%.1fx%.1f y=%.1f..%.1f", name,
                      (double)hit.w, (double)hit.h, (double)hit.y,
                      (double)hit.bottom());
        probeHit(b, in.mx, in.my, s, hit.x, hit.right(), scrollX_, scrollY_);
    };

    // --- the header column -------------------------------------------------
    // Scrolls vertically with the lanes and never horizontally, the same
    // relationship drawTrackHeaders has with the clip grid.
    rr.pushClip(heads);
    rr.rect(heads, tl::panelFill);
    for (size_t i = 0; i < ctx.lanes.size(); ++i) {
        ArrangeContext::Lane& L = ctx.lanes[i];
        const Row& row = rows[i];
        const Rect hb{heads.x, topY + row.y, heads.w, row.h};
        if (hb.bottom() < heads.y || hb.y > heads.bottom()) continue;

        if (i % 2) rr.rect(hb, tl::panelAlt);
        // The colour strip is DATA -- it is how a lane is found without reading
        // its name -- so it keeps the clip colour at full saturation.
        rr.rect({hb.x, hb.y, 3.f * s, hb.h}, itemColour(L.colorIdx, false));

        // The disclosure triangle. A filled triangle rather than a glyph: the
        // atlas has no arrows, and a rotated triangle is the same control
        // everywhere it appears.
        const Rect tri{hb.x + 6.f * s, hb.y + 3.f * s, 12.f * s, 12.f * s};
        const bool open = L.expanded && *L.expanded;
        const u64 triId = uiId(UiArrange, 1, (int)i);
        // 12x12 logical is 12.0 device px at scale 1.0, under the 16 px floor
        // for a thing that is CLICKED rather than dragged. The triangle is a
        // drawn shape and the drawing may not move, so the aim grows instead:
        // 18x18 to hit, 12x12 to look at.
        //
        // AN EXPLICIT CONTAINER, and not `ui.grab(3)`, which is what it was.
        // A symmetric pad is the wrong tool where two controls sit shoulder to
        // shoulder: setHot is LAST-WRITER-WINS, so the pad does not share the
        // contested pixels, it hands all of them to whichever widget is drawn
        // second. The override chip below sits 3 logical px under this triangle
        // and is drawn after it, so a 3 px pad on both put the chip's rect over
        // the bottom quarter of the triangle's -- and a hand aiming at the
        // disclosure arrow and landing two pixels low sent Back to Arrangement
        // instead, which is a transport command and not an undoable edit.
        //
        // So the two rects are written out, edge to edge and not overlapping:
        // this one owns hb.y .. hb.y+18, the chip owns hb.y+18 .. hb.y+35. Both
        // clear the 16 px floor on their short side and neither can steal.
        const Rect triHit{tri.x - 3.f * s, hb.y, tri.w + 6.f * s, 18.f * s};
        const bool hotTri = ui.setHot(triId, triHit) && ui.isHot(triId);
        probeRect("track disclosure triangle", triId, triHit);
        const Col tc = hotTri ? nx::text : nx::muted;
        if (open) rr.triangle(tri.x + 1.f * s, tri.cy() - 2.5f * s,
                              tri.x + 11.f * s, tri.cy() - 2.5f * s,
                              tri.cx(), tri.cy() + 4.f * s, tc);
        else      rr.triangle(tri.x + 2.5f * s, tri.cy() - 5.f * s,
                              tri.x + 2.5f * s, tri.cy() + 5.f * s,
                              tri.x + 9.5f * s, tri.cy(), tc);
        if (hotTri) {
            ui.cursor = Cursor::Hand;
            ui.tip = open ? "hide this track's automation lanes"
                          : "show this track's automation lanes";
            if (in.pressed[0] && L.expanded) {
                *L.expanded = !open;
                changed |= Changed::Layout;
            }
        }

        if (ui.fBody)
            rr.textIn(*ui.fBody, {tri.right() + 4.f * s, hb.y, hb.w - 46.f * s, 16.f * s},
                      L.name.c_str(), nx::text, Align::Left, 0);
        if (L.armed)
            rr.circle(hb.right() - 10.f * s, hb.y + 9.f * s, 3.5f * s, pal::armRed);

        // The override tint, and the way out of it. An overridden track is
        // playing a session clip instead of its lane; the chip both says so and
        // is the Back to Arrangement gesture for that track.
        if (L.overridden) {
            const Rect ov{hb.x + 6.f * s, hb.y + 18.f * s, 58.f * s, 11.f * s};
            const u64 ovId = uiId(UiArrange, 2, (int)i);
            // 11 logical px tall, which is 11.0 device px at 1.0. Same fix as
            // the triangle, and the same shape of fix: the chip keeps the 58x11
            // it DRAWS and is hit through a container written out here -- 64x17,
            // starting exactly where the triangle's container stops, so the two
            // are edge to edge and neither can take a pixel the other was aimed
            // at. See the note over triHit for what a symmetric pad did here.
            const Rect ovHit{ov.x - 3.f * s, ov.y, ov.w + 6.f * s, ov.h + 6.f * s};
            const bool hotOv = ui.setHot(ovId, ovHit) && ui.isHot(ovId);
            probeRect("override chip", ovId, ovHit);
            // Amber, and §1 means it: this is "attention", the one state on a
            // track header that is not what the arrangement asked for. A pill,
            // because it is a chip and chips are pills (§5).
            rr.roundRect(ov, ov.h * 0.5f, nx::amber.alpha(hotOv ? 0.30f : 0.16f));
            rr.roundRectOutline(ov, ov.h * 0.5f, std::max(1.f, s),
                                nx::amber.alpha(hotOv ? 0.55f : 0.30f));
            if (ui.fSmall)
                tl::microLabel(rr, *ui.fSmall, ov.x + 7.f * s,
                               ov.y + (ov.h - ui.fSmall->height()) * 0.5f, "session",
                               nx::amber, ov.w - 10.f * s);
            if (hotOv) {
                ui.cursor = Cursor::Hand;
                ui.tip = "this track is playing the session - click to give the "
                         "arrangement its lane back";
                if (in.pressed[0]) ctx.backToArrTrack = (int)i;
            }
        }

        // The lane's bottom edge resizes it, which is the only place `arrHeight`
        // can be set: a per-track height is what stops one global one from being
        // either wasted space or an unreadable lane (§7.4).
        const Rect grip{hb.x, hb.bottom() + row.autoH - kArrLaneGrab * s,
                        hb.w, kArrLaneGrab * 2.f * s};
        const u64 gripId = uiId(UiArrange, 3, (int)i);
        const bool hotGrip = ui.setHot(gripId, grip) && ui.isHot(gripId);
        probeRect("lane height grip", gripId, grip);
        if (hotGrip && drag_ == Drag::None) {
            ui.cursor = Cursor::ResizeV;
            // 8 logical px tall, which is exactly the drag floor at scale 1.0
            // and 9.8 at 1.25. It passes, and it is invisible -- nothing is
            // drawn on this seam -- so it gets the word instead of the pixels.
            ui.tip = "drag to set this track's lane height";
            if (in.pressed[0] && L.height) {
                drag_ = Drag::LaneH;
                gesture_ = uiId(UiArrange, 3, (int)i, (int)++strokeSeq_);
                dragTrack_ = (int)i;
                grabY_ = in.my;
                origHeight_ = *L.height;
                moved_ = false;
                ui.active = gripId;
            }
        }

        // The automation lanes' own header rows: what each names, its on/off,
        // and one chooser row that adds the next one.
        if (row.autos > 0 && L.autos) {
            for (size_t j = 0; j < L.autos->size(); ++j) {
                AutoLane& al = (*L.autos)[j];
                const Rect ab{heads.x, topY + row.autoY + (f32)j * kArrAutoLaneH * s,
                              heads.w, kArrAutoLaneH * s};
                if (ab.bottom() < heads.y || ab.y > heads.bottom()) continue;
                rr.rect(ab, tl::panelFill);
                rr.rect(ab, tl::deadZone);          // one step down from its track
                rr.hairlineH(ab.x, ab.right(), ab.y);
                const AutoTargets::Entry* tgt = L.targets ? L.targets->find(al.address) : nullptr;
                if (ui.fSmall) {
                    // The target's short label where the address resolves and the
                    // address itself where it does not: a lane naming a missing
                    // device must still be findable.
                    const std::string& lbl = tgt ? tgt->label : al.address;
                    rr.textIn(*ui.fSmall, {ab.x + 6.f * s, ab.y + 4.f * s, ab.w - 30.f * s, 11.f * s},
                              lbl.c_str(), tgt ? nx::muted : nx::muted.alpha(0.55f),
                              Align::Left, 0);
                }
                const Rect onR{ab.right() - 20.f * s, ab.y + 4.f * s, 14.f * s, 12.f * s};
                const u64 onId = uiId(UiArrangeLaneHead, (int)i, (int)j);
                bool on = al.enabled;
                // 14x12 logical; the short side fails the 16 px floor at both
                // scales. Three pixels of aim on every side makes it 20x18.
                ui.grab(3.f * s);
                probeRect("automation lane enable", onId, onR.inset(-3.f * s));
                if (ui.squareToggle(onId, onR, "", &on, nx::violet)) {
                    al.enabled = on;
                    changed |= Changed::Autos;
                    ctx.dirty.push_back((int)i);
                    lastEdit_ = kEditAuto;
                }
                // A lit plate means the lane is driving something -- cyan for
                // the same reason the playhead is: it is the live one.
                rr.circle(onR.cx(), onR.cy(), 3.f * s,
                          al.enabled ? nx::cyan : nx::muted.alpha(0.45f));
                // THE TOGGLE'S OWN RECT, and the reason it needs one. The row
                // below deletes the lane on a right-click over ANY of itself --
                // including this toggle, which sits inside it. Right-clicking a
                // switch to see what its other button does is a normal thing to
                // try, and it destroyed the lane. The toggle now excludes
                // itself from that, with the same 3 px of slack it is hit with
                // so the exclusion and the target are the same shape.
                const Rect onHit = onR.inset(-3.f * s);
                const bool overOn = ui.hovered(onHit);
                if (overOn) ui.tip = al.address + (al.enabled ? "  (on)" : "  (off)");
                // Removing a lane is the same right-click that removes anything
                // else in this program.
                if (ui.hovered(ab) && !overOn) {
                    ui.badge = Badge::Delete;
                    if (ui.tip.empty())
                        ui.tip = "right-click to remove this automation lane";
                }
                if (ui.hovered(ab) && !overOn && in.pressed[2]) {
                    L.autos->erase(L.autos->begin() + (long)j);
                    if (j < laneViews_[i].size())
                        laneViews_[i].erase(laneViews_[i].begin() + (long)j);
                    changed |= Changed::Autos;
                    ctx.dirty.push_back((int)i);
                    lastEdit_ = kEditAuto;
                    break;
                }
            }
            // The chooser row. `selector` and not a dropdown, for the reason the
            // roll's lane key gives: this codebase has no dropdown, and click
            // cycles / right-click cycles back / wheel scrubs is the idiom
            // everywhere else in it.
            const Rect cb{heads.x, topY + row.autoY + (f32)L.autos->size() * kArrAutoLaneH * s,
                          heads.w, kArrAutoLaneH * s};
            if (cb.bottom() >= heads.y && cb.y <= heads.bottom()) {
                rr.rect(cb, tl::panelFill);
                rr.rect(cb, tl::deadZone);
                rr.hairlineH(cb.x, cb.right(), cb.y);
                if (L.targets && !L.targets->entries.empty()) {
                    std::vector<const char*> names;
                    names.reserve(L.targets->entries.size());
                    for (const AutoTargets::Entry& e : L.targets->entries)
                        names.push_back(e.label.c_str());
                    int& tsel = targetSel_[i];
                    tsel = clampv(tsel, 0, (int)names.size() - 1);
                    // THE GAP IS EIGHT, and it was four. Both of these are hit
                    // through a 3 px pad, so a 4 px gap left their rects
                    // OVERLAPPING by 2 px -- and setHot being last-writer-wins,
                    // the "+" (drawn second) took both of those pixels. Aiming
                    // at the right end of the target chooser therefore ADDED AN
                    // AUTOMATION LANE, which is the more consequential of the
                    // two verbs and the one that must not be reachable by
                    // accident. 8 > 3 + 3, so the two zones cannot touch; the
                    // chooser gives up the 4 px and the row's outer edges do
                    // not move.
                    const Rect selR{cb.x + 6.f * s, cb.y + 5.f * s, cb.w - 38.f * s, 14.f * s};
                    // 14 logical px tall, under the floor at both scales; the
                    // chooser row has 44 px of height above and below to lend.
                    ui.grab(3.f * s);
                    ui.selector(uiId(UiArrange, 5, (int)i), selR, &tsel, names.data(), (int)names.size());
                    probeRect("automation target chooser", uiId(UiArrange, 5, (int)i),
                              selR.inset(-3.f * s));
                    if (ui.hovered(selR.inset(-3.f * s)))
                        ui.tip = L.targets->entries[(size_t)tsel].group + " " +
                                 L.targets->entries[(size_t)tsel].label + "  " +
                                 L.targets->entries[(size_t)tsel].address +
                                 "  --  click cycles, right-click steps back";
                    const Rect addR{selR.right() + 8.f * s, selR.y, 20.f * s, selR.h};
                    if (ui.hovered(addR.inset(-3.f * s)) && ui.tip.empty())
                        ui.tip = "add an automation lane for the chosen target";
                    ui.grab(3.f * s);
                    probeRect("add automation lane +", uiId(UiArrange, 6, (int)i),
                              addR.inset(-3.f * s));
                    if (ui.button(uiId(UiArrange, 6, (int)i), addR, "+") &&
                        (int)L.autos->size() < kMaxArrLanes) {
                        const std::string& addr = L.targets->entries[(size_t)tsel].address;
                        int found = -1;
                        for (size_t k = 0; k < L.autos->size(); ++k)
                            if ((*L.autos)[k].address == addr) { found = (int)k; break; }
                        if (found < 0) {
                            AutoLane nl;
                            nl.address = addr;
                            L.autos->push_back(std::move(nl));
                            changed |= Changed::Autos | Changed::Layout;
                            ctx.dirty.push_back((int)i);
                            lastEdit_ = kEditAuto;
                        }
                    }
                } else if (ui.fSmall) {
                    rr.textIn(*ui.fSmall, cb, "no targets", nx::muted.alpha(0.55f),
                              Align::Center, 0);
                }
            }
        }
    }
    // The seam between the header column and the work. A hairline, so the two
    // read as one surface with a fold in it rather than as two panels.
    rr.hairlineV(heads.right() - 1.f * s, heads.y, heads.bottom());
    rr.popClip();

    // --- item interaction --------------------------------------------------
    // What is under the cursor, and where inside it. One answer, used by the
    // press, the cursor shape and the fade grabs alike, so they cannot disagree.
    // THE FIVE ZONES, and the three things that were wrong with them.
    //
    // 1. The trim band was kArrEdgeGrab = 5 logical px: 5.0 device px at scale
    //    1.0 and 6.1 at 1.25, both under the 8 px floor. It is 8 now.
    // 2. The zones stopped dead at the item's own edge, so the pixels a hand
    //    aiming at that edge actually lands on -- the ones just outside it --
    //    hit empty lane and deselected. The search rect is widened by
    //    kArrEdgeSlop and the trim zones reach into it.
    // 3. The fade corners were a flat 14 logical px wide each. On an item
    //    narrower than 28 logical px they overlapped and the else-if chain gave
    //    the whole overlap to FADE-IN, which left FADE-OUT unreachable at any
    //    zoom on any item under 28 px -- a zone of size zero. Both are now
    //    capped at kArrFadeShare of the item, so the two can never meet.
    //
    // The PRIORITY is unchanged where it was already right (a fade corner
    // outranks the trim band it sits over, because the corner is the smaller,
    // more specific target and the trim band is still 11 px tall underneath
    // it), and the slop belongs to trim rather than to fade for the same
    // reason: outside the item there is no fade to grab.
    //
    // 4. THE OUTSIDE SLOP IS NOW ADAPTIVE, and that is this pass's addition. The
    //    three zones do not fit on a narrow item: two 8 px trim bands plus a
    //    body need 16 px before the body has any width at all, so below ~27 px
    //    the inside band is capped at 30% of the item and shrinks with it. What
    //    does NOT have to shrink is the reach from OUTSIDE, where there is
    //    nothing but empty lane -- so the outside slop grows by exactly what the
    //    inside band lost, and the total reach at an edge stays at the 8 px drag
    //    floor all the way down to a two-pixel item. Measured: at 10 px wide the
    //    old zones gave 3 (inside) + 3 (slop) = 6 px an edge; they now give
    //    3 + 5 = 8.
    // 5. PASS TWO PICKS THE NEAREST EDGE. It used to take the LAST item in the
    //    vector that matched, so two items with a four-pixel gap between them
    //    handed the whole gap to the right-hand one however close the pointer
    //    was to the left one's tail. Nearest-edge-wins is the rule the brace
    //    ends and the marker flags already use inside their overlaps.
    int hitTrack = -1, hitIdx = -1;
    enum class Zone { Body, Left, Right, FadeIn, FadeOut } hitZone = Zone::Body;
    const f32 slop = kArrEdgeSlop * s;
    // How far outside an item of width `w` its edges may be caught from.
    const auto slopFor = [&](f32 w) {
        const f32 inside = std::max(std::min(kArrEdgeGrab * s, w * 0.3f), 1.f);
        return std::max(slop, kArrEdgeGrab * s - inside);
    };
    // Which item a pixel belongs to, as one answer both the zone resolution
    // below and the right-drag erase stroke read.
    const auto itemAt = [&](f32 mx, f32 my, int& outTrack, int& outIdx) {
        outTrack = -1; outIdx = -1;
        const int t = trackAtY(my);
        if (t < 0 || t >= (int)ctx.lanes.size() || !ctx.lanes[(size_t)t].items) return;
        if (my >= topY + rows[(size_t)t].y + rows[(size_t)t].h) return;   // the auto rows
        const std::vector<ArrangeClip>& v = *ctx.lanes[(size_t)t].items;
        // TWO PASSES, and the order is the whole point. The first asks
        // "which item is the pointer IN", the second "which edge is it
        // NEAR". A one-pass search with the slop folded in would let an
        // item steal the three pixels before its butted-up neighbour's
        // right edge -- turning a right-trim into a left-trim of the wrong
        // item, which is the exact class of bug this pass exists to remove.
        for (size_t k = v.size(); k-- > 0;) {
            const f32 x0 = beatToX(ta, v[k].start), x1 = beatToX(ta, v[k].end());
            if (mx < x0 || mx >= x1) continue;
            outTrack = t; outIdx = (int)k;
            return;
        }
        f32 best = 1e9f;
        for (size_t k = v.size(); k-- > 0;) {
            const f32 x0 = beatToX(ta, v[k].start), x1 = beatToX(ta, v[k].end());
            const f32 sl = slopFor(std::max(1.f, x1 - x0));
            if (mx < x0 - sl || mx >= x1 + sl) continue;
            const f32 d = std::min(std::fabs(mx - x0), std::fabs(mx - x1));
            if (d < best) { best = d; outTrack = t; outIdx = (int)k; }
        }
    };
    if (hotLanes && drag_ == Drag::None) {
        const int t = trackAtY(in.my);
        if (t >= 0 && t < (int)ctx.lanes.size() && ctx.lanes[(size_t)t].items &&
            in.my < topY + rows[(size_t)t].y + rows[(size_t)t].h) {
            const std::vector<ArrangeClip>& v = *ctx.lanes[(size_t)t].items;
            itemAt(in.mx, in.my, hitTrack, hitIdx);
            if (hitIdx >= 0) {
                const ArrangeClip& h = v[(size_t)hitIdx];
                const f32 x0 = beatToX(ta, h.start), x1 = beatToX(ta, h.end());
                const f32 w = std::max(1.f, x1 - x0);
                const f32 e = std::max(std::min(kArrEdgeGrab * s, w * 0.3f), 1.f);
                const f32 fw = std::min(kArrFadeGrab * s, w * kArrFadeShare);
                const f32 topBand = topY + rows[(size_t)t].y + kArrFadeBandH * s;
                if (in.mx < x0 || in.mx >= x1)
                    hitZone = in.mx < x0 ? Zone::Left : Zone::Right;   // the slop
                else if (in.my < topBand && in.mx < x0 + fw)        hitZone = Zone::FadeIn;
                else if (in.my < topBand && in.mx > x1 - fw)        hitZone = Zone::FadeOut;
                else if (in.mx < x0 + e)                            hitZone = Zone::Left;
                else if (in.mx > x1 - e)                            hitZone = Zone::Right;
                else                                                hitZone = Zone::Body;
                static const char* kZone[] = {"body", "trimL", "trimR", "fadeIn", "fadeOut"};
                char what[192];
                std::snprintf(what, sizeof what,
                              "item uid=%llu w=%.1f e=%.1f fw=%.1f fadeTop=%.1f "
                              "band=%.1f..%.1f %s",
                              (unsigned long long)h.uid, (double)w, (double)e, (double)fw,
                              (double)topBand, (double)(topY + rows[(size_t)t].y),
                              (double)(topY + rows[(size_t)t].y + rows[(size_t)t].h),
                              kZone[(int)hitZone]);
                probeHit(what, in.mx, in.my, s, x0, x1, scrollX_, scrollY_);
            } else {
                probeHit("empty lane", in.mx, in.my, s, 0.f, 0.f, scrollX_, scrollY_);
            }
        }
    }

    if (drag_ == Drag::None && hotLanes && (in.pressed[0] || in.pressed[2])) {
        if (hitIdx >= 0) {
            std::vector<ArrangeClip>& v = *ctx.lanes[(size_t)hitTrack].items;
            const ArrangeClip& it = v[(size_t)hitIdx];
            if (selTrack_ != hitTrack || selItem_ != it.uid) {
                selTrack_ = hitTrack;
                selItem_  = it.uid;
                changed |= Changed::Selection;
            }
            if (in.pressed[2]) {
                ctx.wantDelete = true;      // right-click deletes, as in the roll
                // ...and the press ARMS THE SWEEP. See the erase arm below: the
                // item under the press goes through the caller's one-shot verb
                // exactly as it always has, and everything the pointer crosses
                // after it goes through the stroke, as one coalesced entry.
                drag_ = Drag::Erase;
                gesture_ = uiId(UiArrange, 12, (int)++strokeSeq_);
                moved_ = false;
                eraseNext_ = 0;
                eraseNextTrack_ = -1;
                eraseLast_ = it.uid;        // already going; do not count it twice
            } else if (in.dblClick) {
                cursorBeat_ = xToBeat(ta, in.mx);
                ctx.wantSplit = true;       // double-click splits under the cursor
            } else {
                drag_ = hitZone == Zone::Left    ? Drag::TrimL
                        : hitZone == Zone::Right ? Drag::TrimR
                        : hitZone == Zone::FadeIn  ? Drag::FadeIn
                        : hitZone == Zone::FadeOut ? Drag::FadeOut
                                                   : Drag::Move;
                gesture_ = uiId(UiArrange, 8, hitTrack, (int)++strokeSeq_);
                dragTrack_ = hitTrack;
                dragUid_ = it.uid;
                grabBeat_ = xToBeat(ta, in.mx) - it.start;
                grabY_ = in.my;
                origStart_ = it.start;
                origOffset_ = it.offset;
                origLength_ = it.length;
                origFadeIn_ = it.fadeIn;
                origFadeOut_ = it.fadeOut;
                moved_ = false;
                dupMade_ = false;
                ui.active = gesture_;
            }
        } else if (in.pressed[0] && in.dblClick) {
            // Empty lane space. Double-click on an item splits it; the same
            // gesture on nothing CREATES -- a one-bar note block at the
            // quantized beat, which is the gesture every Live hand reaches
            // for and, until this branch, found nothing under.
            //
            // `in.pressed[0]` is new and it is a fix, not a tidy-up: dblClick is
            // set for WHICHEVER button was double-clicked, so this branch caught
            // a right-double-click too and answered it by creating a clip. That
            // was survivable while the right button did one thing per press; it
            // is not survivable now that right-drag erases, because clearing two
            // stretches of lane in quick succession IS a right double-click, and
            // the second press would have left a block behind in the hole the
            // first one had just made.
            const int t = trackAtY(in.my);
            if (t >= 0) {
                ctx.wantCreate  = true;
                ctx.createTrack = t;
                ctx.createBeat  = std::max(0.0, snapBeat(xToBeat(ta, in.mx)));
            }
        } else if (in.pressed[2]) {
            // A right press on EMPTY lane arms the sweep too, and it has to: a
            // hand clearing a stretch of timeline starts the stroke in the gap
            // beside the first clip about as often as it starts on it.
            drag_ = Drag::Erase;
            gesture_ = uiId(UiArrange, 12, (int)++strokeSeq_);
            moved_ = false;
            eraseNext_ = 0;
            eraseNextTrack_ = -1;
            eraseLast_ = 0;
        } else if (in.pressed[0]) {
            if (selItem_ != 0) changed |= Changed::Selection;
            selTrack_ = -1;
            selItem_ = 0;
            // LEFT-DRAG PAINTS. The press still only deselects -- a click on
            // empty lane means "nothing is selected" and always did, and a press
            // that laid a block down immediately would fight the double-click
            // that creates one (the second click would land on what the first
            // made and split it). The stroke begins on the first frame the
            // pointer TRAVELS, which is exactly the same threshold every other
            // drag on this surface arms itself with.
            const int t = trackAtY(in.my);
            if (t >= 0 && t < (int)ctx.lanes.size() && ctx.lanes[(size_t)t].items) {
                drag_ = Drag::Paint;
                gesture_ = uiId(UiArrange, 13, (int)++strokeSeq_);
                paintTrack_ = t;
                paintGrabX_ = in.mx;
                paintCell_  = -1e18;
                paintAwait_ = false;
                paintHave_  = false;
                moved_ = false;
            }
        }
    }

    // --- LEFT-DRAG PAINTS ---------------------------------------------------
    //
    // One block per BAR CELL swept, snapped to the bar because that is the
    // length the caller's "fresh note block" has -- ask for a bar and step by a
    // bar, and the stroke lays a contiguous run rather than a ladder of overlaps
    // the repair pass would then eat. The signature map is what says where a bar
    // is, so this is correct in 7/8 without knowing that it is in 7/8.
    //
    // NOTHING STACKS. A cell already holding material is skipped -- silently,
    // because that is the right answer and not a refusal: sweeping back and
    // forth over a stretch is how a hand aims, and a stroke that piled a second
    // block onto every cell it re-crossed would be unusable. It is also what
    // makes the stroke idempotent, which is what lets the hand wander.
    if (drag_ == Drag::Paint) {
        if (!in.down[0]) {
            if (moved_ && paintTrack_ >= 0 && paintTrack_ < (int)ctx.lanes.size() &&
                ctx.lanes[(size_t)paintTrack_].items) {
                arrangeRepair(*ctx.lanes[(size_t)paintTrack_].items);
                ctx.dirty.push_back(paintTrack_);
                changed |= Changed::Items;
            }
            drag_ = Drag::None;
            gesture_ = 0;
            paintHave_ = false;
            paintAwait_ = false;
            paintTemplate_ = ArrangeClip{};
        } else {
            if (!moved_ && std::fabs(in.mx - paintGrabX_) > 3.f * s) moved_ = true;
            // The caller has made the first block by now; adopt it as the thing
            // the rest of the stroke repeats. It is found BY ITS BEAT and not by
            // the selection, which would have been the obvious handle and is the
            // wrong one: App::arrangeCommit reads the fresh item's uid before
            // assignUids has stamped it, so what it selects is uid 0 -- i.e.
            // nothing -- and a stroke that waited for that selection would wait
            // for ever. (That is a real bug in the caller and it is filed; this
            // does not depend on the fix landing.) The cell is a handle the view
            // owns outright.
            if (paintAwait_ && paintTrack_ >= 0 &&
                paintTrack_ < (int)ctx.lanes.size() && ctx.lanes[(size_t)paintTrack_].items) {
                const std::vector<ArrangeClip>& v = *ctx.lanes[(size_t)paintTrack_].items;
                int k = -1;
                for (size_t q = 0; q < v.size(); ++q)
                    if (std::fabs(v[q].start - paintCell_) < 1e-6) { k = (int)q; break; }
                if (k >= 0) {
                    paintTemplate_ = v[(size_t)k];
                    paintHave_ = true;
                    paintAwait_ = false;
                    // THE HANDSHAKE, taken here and not at the first clone,
                    // because here is the last frame on which nothing has been
                    // cloned yet. The caller's point is therefore taken against
                    // "the first block exists" -- so one Ctrl+Z lifts the whole
                    // painted run and a second lifts the block that started it,
                    // which is two honest steps rather than one entry that
                    // half-undoes and one that appears to do nothing.
                    pendingEdit_ = kEditPaint;
                    lastEdit_ = kEditPaint;
                }
            }
            const int t = trackAtY(in.my);
            std::vector<ArrangeClip>* v =
                (moved_ && t >= 0 && t < (int)ctx.lanes.size()) ? ctx.lanes[(size_t)t].items
                                                                : nullptr;
            if (v && t == paintTrack_ && !paintAwait_) {
                const f64 at = std::max(0.0, (f64)xToBeat(ta, in.mx));
                const f64 bar = std::floor(ctx.sig.barOfBeat(at));
                const f64 cell = ctx.sig.beatOfBar(bar);
                const f64 cellEnd = ctx.sig.beatOfBar(bar + 1.0);
                bool occupied = std::fabs(cell - paintCell_) < 1e-9;
                for (size_t k = 0; !occupied && k < v->size(); ++k)
                    occupied = (*v)[k].start < cellEnd - kArrOverlapEps &&
                               (*v)[k].end() > cell + kArrOverlapEps;
                if (!occupied && (int)v->size() >= kMaxArrItems) {
                    // A refusal that SAYS SO. Silence here reads as a dead
                    // patch of lane, and the hand's next move is to press
                    // harder rather than to look at the item count.
                    ui.tip = "this lane is full - " + std::to_string(kMaxArrItems) +
                             " items is the limit";
                } else if (!occupied) {
                    paintCell_ = cell;
                    if (paintHave_) {
                        // A CLONE, which is both what FL paints and what makes
                        // the stroke one undo entry: the caller's one-shot
                        // create took its own point for the first block, and
                        // every block after it rides the gesture named above.
                        ArrangeClip nc = paintTemplate_;
                        nc.uid = newUid(ctx);
                        nc.start = cell;
                        nc.length = std::max(kMinArrBeats, cellEnd - cell);
                        nc.fadeIn = 0.0;
                        nc.fadeOut = 0.0;
                        v->push_back(std::move(nc));
                        ctx.dirty.push_back(t);
                        changed |= Changed::Items;
                        lastEdit_ = kEditPaint;
                    } else {
                        ctx.wantCreate  = true;
                        ctx.createTrack = t;
                        ctx.createBeat  = cell;
                        paintAwait_ = true;
                    }
                }
            }
        }
    }

    // --- RIGHT-DRAG ERASES --------------------------------------------------
    //
    // FL's most-used gesture, and the one this surface was most obviously
    // missing: a right-click removed exactly one item per press, so clearing
    // eight bars was eight presses. The victim is NAMED on one frame and removed
    // on the next, which is the pendingEdit handshake -- the caller has to be
    // able to take its undo point before the first thing disappears, and it can
    // only do that if the view says "I am about to" a frame ahead.
    //
    // The whole stroke is ONE entry, coalesced on the gesture id, because the
    // hand made one movement. The item under the PRESS is the exception and it
    // is not one: it goes through ctx.wantDelete as it always did, under its own
    // "delete clip" entry, so a plain right-click behaves exactly as before.
    if (drag_ == Drag::Erase) {
        if (!in.down[2]) {
            // Whatever was NAMED but not yet removed is simply dropped: the
            // button came up before the frame that would have taken it, which
            // means the hand let go over it and did not mean it.
            drag_ = Drag::None;
            gesture_ = 0;
            eraseNext_ = 0;
            eraseNextTrack_ = -1;
            eraseLast_ = 0;
        } else {
            // The one named last frame, removed now.
            if (eraseNext_ && eraseNextTrack_ >= 0 &&
                eraseNextTrack_ < (int)ctx.lanes.size() &&
                ctx.lanes[(size_t)eraseNextTrack_].items) {
                std::vector<ArrangeClip>& v = *ctx.lanes[(size_t)eraseNextTrack_].items;
                const int k = indexOf(v, eraseNext_);
                if (k >= 0) {
                    v.erase(v.begin() + k);
                    arrangeRepair(v);
                    ctx.dirty.push_back(eraseNextTrack_);
                    changed |= Changed::Items;
                    lastEdit_ = kEditErase;
                    if (selItem_ == eraseNext_) {
                        selItem_ = 0;
                        selTrack_ = -1;
                        changed |= Changed::Selection;
                    }
                }
                eraseLast_ = eraseNext_;
                eraseNext_ = 0;
                eraseNextTrack_ = -1;
            }
            // And the next one named, if the pointer has found one.
            int et = -1, ei = -1;
            itemAt(in.mx, in.my, et, ei);
            if (et >= 0 && ei >= 0) {
                const u64 uid = (*ctx.lanes[(size_t)et].items)[(size_t)ei].uid;
                if (uid && uid != eraseLast_) {
                    if (!moved_) { moved_ = true; pendingEdit_ = kEditErase; }
                    eraseNext_ = uid;
                    eraseNextTrack_ = et;
                }
            }
        }
    }

    if (drag_ == Drag::LaneH) {
        if (!in.down[0]) {
            drag_ = Drag::None;
            gesture_ = 0;
            if (dragTrack_ >= 0) changed |= Changed::Layout;
        } else if (dragTrack_ >= 0 && dragTrack_ < (int)ctx.lanes.size() &&
                   ctx.lanes[(size_t)dragTrack_].height) {
            if (!moved_ && std::fabs(in.my - grabY_) > 2.f * s) {
                moved_ = true;
                pendingEdit_ = kEditLayout;
            } else if (moved_) {
                *ctx.lanes[(size_t)dragTrack_].height =
                    clampv(origHeight_ + (in.my - grabY_) / s, kArrMinLaneH, kArrMaxLaneH);
            }
        }
    } else if (itemDrag(drag_)) {
        // THE ITEM DRAGS, and this arm is now a WHITELIST. It used to read
        // "anything still dragging, minus Loop, minus Marker", so every gesture
        // added to this file had to remember to exclude itself -- and the one
        // time that was forgotten, a marker drag fell in here, indexOf(0)
        // answered -1, and the `idx < 0` branch cancelled the gesture on the
        // frame after the press: the flag jumped on the click and would not
        // move. Four more drags arrive in this pass (Pan, Paint, Erase,
        // EraseMarker) and none of them can repeat that, because the condition
        // now names what belongs here instead of what does not.
        //
        // Every item drag is measured from the values the item had at the press
        // and written absolutely, so it never integrates its own error and a
        // wheel mid-drag cannot walk the item away from the hand.
        if (!in.down[0]) {
            // COMMIT. This is the one place a lane is repaired and the only
            // place a drag reports Items, which is what keeps the engine from
            // ever seeing an unrepaired lane and what stops a sweep from eating
            // a neighbour one frame at a time.
            if (moved_ && dragTrack_ >= 0 && dragTrack_ < (int)ctx.lanes.size() &&
                ctx.lanes[(size_t)dragTrack_].items) {
                arrangeRepair(*ctx.lanes[(size_t)dragTrack_].items);
                ctx.dirty.push_back(dragTrack_);
                changed |= Changed::Items;
            }
            if (ui.active == gesture_) ui.active = 0;
            drag_ = Drag::None;
            gesture_ = 0;
            dragUid_ = 0;
        } else {
            int t = dragTrack_;
            int idx = (t >= 0 && t < (int)ctx.lanes.size() && ctx.lanes[(size_t)t].items)
                          ? indexOf(*ctx.lanes[(size_t)t].items, dragUid_)
                          : -1;
            if (idx < 0) {
                drag_ = Drag::None;
                gesture_ = 0;
            } else if (!moved_) {
                const bool far = std::fabs(in.mx - beatToX(ta, origStart_ + grabBeat_)) > 2.f * s ||
                                 (drag_ == Drag::Move && std::fabs(in.my - grabY_) > 4.f * s);
                if (far) {
                    moved_ = true;
                    pendingEdit_ = drag_ == Drag::Move    ? kEditMove
                                   : drag_ == Drag::FadeIn || drag_ == Drag::FadeOut ? kEditFade
                                                                                     : kEditTrim;
                }
            } else {
                std::vector<ArrangeClip>* items = ctx.lanes[(size_t)t].items;
                // SHIFT+DRAG CLONES, and Ctrl+drag still does: FL spends Shift
                // on this and this program had spent Ctrl, so both are bound
                // rather than one of them being taken away from whoever already
                // has the habit. The copy is made once, on the first frame that
                // actually mutates, so it is taken with the values the item had
                // at the press. It is the COPY that stays behind at the original
                // position and the original that travels, which is what both
                // spellings mean everywhere they exist.
                if (drag_ == Drag::Move && (in.ctrl() || in.shift()) && !dupMade_ &&
                    (int)items->size() < kMaxArrItems) {
                    ArrangeClip copy = (*items)[(size_t)idx];
                    copy.uid = newUid(ctx);
                    items->insert(items->begin() + (long)idx, std::move(copy));
                    // The inserted copy takes index `idx`; the item under the
                    // hand is now one later and still carries dragUid_.
                    dupMade_ = true;
                    idx = indexOf(*items, dragUid_);
                    if (idx < 0) { drag_ = Drag::None; gesture_ = 0; }
                    lastEdit_ = kEditDup;
                }
                if (idx >= 0) {
                    ArrangeClip& it = (*items)[(size_t)idx];
                    const f64 raw = xToBeat(ta, in.mx);
                    // ALT is unquantized -- FL's modifier, and the one the
                    // automation lane eight pixels below has always used. Shift
                    // still is too on everything except a MOVE, where it means
                    // "clone" instead; see snapBeat at the top of this function
                    // for the collision and why it resolves this way.
                    const auto snap = [&](f64 b) {
                        return (in.alt() || (in.shift() && drag_ != Drag::Move))
                                   ? b
                                   : quantNear(b);
                    };
                    switch (drag_) {
                    case Drag::Move: {
                        it.start = std::max(0.0, snap(raw - grabBeat_));
                        // Across tracks: the item is moved bodily into the other
                        // lane, because an ArrangeClip owns its clip (§2.2) and
                        // there is nothing else to carry.
                        const int nt = trackAtY(in.my);
                        if (nt >= 0 && nt != t && nt < (int)ctx.lanes.size() &&
                            ctx.lanes[(size_t)nt].items &&
                            (int)ctx.lanes[(size_t)nt].items->size() < kMaxArrItems) {
                            ArrangeClip moved = std::move((*items)[(size_t)idx]);
                            items->erase(items->begin() + (long)idx);
                            arrangeRepair(*items);
                            ctx.dirty.push_back(t);
                            ctx.lanes[(size_t)nt].items->push_back(std::move(moved));
                            dragTrack_ = nt;
                            selTrack_ = nt;
                            changed |= Changed::Selection | Changed::Items;
                        }
                        break;
                    }
                    case Drag::TrimL: {
                        // The head trim: `start` and `offset` move TOGETHER, so
                        // the audio under the rest of the item does not slide.
                        // Getting this wrong is the classic arrangement-editor
                        // bug, which is why it is one expression and not two.
                        const f64 want = snap(raw - grabBeat_);
                        const f64 lo = std::max(0.0, origStart_ - origOffset_);
                        const f64 hi = origStart_ + origLength_ - kMinArrBeats;
                        const f64 ns = clampv(want, lo, hi);
                        const f64 d = ns - origStart_;
                        it.start  = ns;
                        it.offset = origOffset_ + d;
                        it.length = origLength_ - d;
                        if (it.fadeIn > it.length) it.fadeIn = it.length;
                        if (it.fadeOut > it.length) it.fadeOut = it.length;
                        break;
                    }
                    case Drag::TrimR: {
                        const f64 want = snap(raw);
                        it.length = std::max(kMinArrBeats, want - it.start);
                        if (it.fadeOut > it.length) it.fadeOut = it.length;
                        if (it.fadeIn > it.length) it.fadeIn = it.length;
                        break;
                    }
                    case Drag::FadeIn:
                        it.fadeIn = clampv(raw - it.start, 0.0,
                                           std::max(0.0, it.length - it.fadeOut));
                        break;
                    case Drag::FadeOut:
                        it.fadeOut = clampv(it.end() - raw, 0.0,
                                            std::max(0.0, it.length - it.fadeIn));
                        break;
                    default: break;
                    }
                    // dragUid_ and not `it.uid`: a cross-track move has just
                    // moved that object into another vector and the reference is
                    // dead. The uid is the handle precisely so that it survives.
                    selItem_ = dragUid_;
                }
            }
        }
    }

    // --- the drop target ---------------------------------------------------
    if (ctx.dropActive && hotLanes) {
        const int t = trackAtY(in.my);
        if (t >= 0) {
            ctx.dropTrack = t;
            ctx.dropBeat = std::max(0.0, quantNear(xToBeat(ta, in.mx)));
            const f32 x = beatToX(ta, ctx.dropBeat);
            rr.pushClip(lanes);
            rr.rect({nx::snapPx(x), topY + rows[(size_t)t].y, std::max(1.f, 2.f * s),
                     rows[(size_t)t].h}, nx::violetSoft);
            rr.popClip();
            if (in.released[0]) ctx.dropped = true;
        }
    }

    // --- the cursor --------------------------------------------------------
    if (drag_ == Drag::TrimL || drag_ == Drag::TrimR)      ui.cursor = Cursor::ResizeH;
    else if (drag_ == Drag::Move)                          ui.cursor = Cursor::Grab;
    else if (drag_ == Drag::Loop)                          ui.cursor = Cursor::ResizeH;
    else if (drag_ == Drag::LaneH)                         ui.cursor = Cursor::ResizeV;
    else if (drag_ == Drag::Marker)                        ui.cursor = Cursor::Grab;
    // The four new strokes say what they are while they run. A pan is a Grab
    // because the surface is what moved; the three erases wear the Delete badge,
    // which is the one answer in the vocabulary that means "removal is the verb
    // here" -- and a sweep that is quietly taking things away is exactly where
    // that has to be visible.
    else if (drag_ == Drag::Pan)                           ui.cursor = Cursor::Grab;
    else if (drag_ == Drag::Erase || drag_ == Drag::EraseMarker ||
             drag_ == Drag::ErasePoint) {
        ui.badge = Badge::Delete;
        if (ui.tip.empty())
            ui.tip = "erasing - sweep the right button across what should go";
    }
    else if (drag_ == Drag::Paint) {
        ui.badge = Badge::Add;
        // Not unconditionally: the stroke may already have said something more
        // specific this frame (a lane that has hit its item limit), and a
        // general tip painted over a refusal is a refusal nobody reads.
        if (ui.tip.empty())
            ui.tip = "painting - one block a bar; release to stop";
    }
    else if (hotBand && renameMarker_ == 0) {
        if (hitFlag >= 0) {
            ui.cursor = Cursor::Grab;
            // THE FLAG TIPS ITS NAME, and that is the whole reason the tip is
            // here rather than a badge: a label truncated to "Chorus ver.." is
            // the one thing on this ruler that cannot say what it is, and no
            // badge in the vocabulary means "this is called something".
            const Marker& m = (*markers_)[flags[(size_t)hitFlag].i];
            ui.tip = (m.name.empty() ? std::string("(unnamed)") : m.name) +
                     " - click to jump here, drag to move (Alt: off the grid), "
                     "double-click to rename, right-click to delete - or "
                     "right-drag along the band to clear several";
        } else {
            // A CREATION ZONE, which is the badge rule's own worked example: an
            // empty strip above the bar numbers is indistinguishable from
            // padding, and the double-click that puts a marker on it is
            // invisible until it is found by accident.
            ui.badge = Badge::Add;
            if (ui.tip.empty())
                ui.tip = "double-click to drop a marker here; right-drag along "
                         "the band to clear the ones you cross";
        }
    }
    else if (hotRuler && drag_ == Drag::None) {
        ui.cursor = braceEnd >= 0 ? Cursor::ResizeH : Cursor::Hand;
        if (braceEnd >= 0)
            ui.tip = "drag this end of the loop brace; drag anywhere else on the "
                     "ruler to set a new one, right-click for a signature change";
        else if (ui.tip.empty())
            ui.tip = "click to locate, drag to set the loop brace (Alt: off the "
                     "grid), right-click to add or remove a signature change at "
                     "this bar";
    }
    else if (hitIdx >= 0) {
        ui.cursor = (hitZone == Zone::Left || hitZone == Zone::Right) ? Cursor::ResizeH
                    : (hitZone == Zone::FadeIn || hitZone == Zone::FadeOut) ? Cursor::ResizeH
                                                                            : Cursor::Grab;
        // THE BADGE: what a click does that the cursor cannot say. A Grab
        // cursor over an item's body says "you can move this"; it cannot say
        // that a DOUBLE-click cuts it at the pointer, which is the one verb on
        // this surface with no other way in. The trim and fade corners get no
        // badge -- a resize cursor over an edge is not ambiguous.
        if (hitZone == Zone::Body) {
            ui.badge = Badge::Split;
            if (ui.tip.empty())
                ui.tip = "drag to move (Alt: off the grid), Shift or Ctrl+drag to "
                         "leave a copy, double-click to split here, right-click "
                         "to delete - right-drag erases everything you cross";
        } else if (hitZone == Zone::FadeIn || hitZone == Zone::FadeOut) {
            if (ui.tip.empty()) ui.tip = "drag the top corner to set the fade";
        } else if (ui.tip.empty()) {
            // A resize cursor says "an edge"; it does not say WHICH edit. A head
            // trim moves the material with the edge and a tail trim does not,
            // and that is the difference between the two halves of an item.
            ui.tip = hitZone == Zone::Left
                         ? "drag to trim the start; the material moves with it"
                         : "drag to trim the end";
        }
    } else if (hotLanes && drag_ == Drag::None && trackAtY(in.my) >= 0) {
        // EMPTY TIMELINE LANE -- the exact gap this pass was sent after. An
        // empty lane is indistinguishable from dead space, and the double-click
        // that puts a one-bar note block on it is invisible until it is found
        // by accident. The badge is what makes it findable.
        ui.badge = Badge::Add;
        if (ui.tip.empty())
            ui.tip = "double-click to write a one-bar note block here, or drag "
                     "across the lane to paint a run of them";
    }
    // A Ctrl+drag is making a copy, and the moment the modifier goes down is
    // the moment that stops being a plain move. The badge is the only feedback
    // there is until the button comes up.
    if (drag_ == Drag::Move && in.ctrl()) ui.badge = Badge::Duplicate;

    ctx.selTrack = selTrack_;
    ctx.selItem  = selItem_;
    probeArrange(ctx, changed, selTrack_, selItem_);
    probeMarkers(markers_, markerReq_, selMarker_, queuedMarker_);
    return changed;
}

} // namespace lat
