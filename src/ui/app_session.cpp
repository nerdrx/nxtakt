// Session view: track headers, the clip grid, clip slots, the scene column,
// the mixer, return + master strips, drag/drop (and drawDragGhost, kept
// beside the resolution logic it visualises), plus the clip-model helpers
// the grid's mouse handling drives (loadClipInto / createMidiClip /
// selectTrack / addTrack / …). Moved verbatim from app.cpp.
//
#include "app.h"
#include "app_internal.h"
#include "pianoroll.h"
#include "../core/project.h"
#include "../gfx/gl.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <new>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>

namespace lat {

// ---------------------------------------------------------------------------
// THE JUDGMENT, for this file (docs/DESIGN.md §4, and the specimen sheet's
// "WORKING SURFACE / FLAT, PRECISE, FAST" strip).
//
// The clip grid is an instrument, not a card. It stays flat: palette fills,
// wells, hairlines, one quad per cell and no gradient, no sheen and no shadow
// anywhere inside it. Clip colours stay saturated because they are DATA -- the
// thing the eye is scanning for -- and dimming them to be tasteful would be
// branding paid for out of the user's ability to read their own set.
//
// The furniture around it is chrome and takes the tier table in full: the scene
// column, the return and master strips and the mixer band get the glass fills,
// the lit edges and the hairline dividers. That split is the whole of the
// design decision in this file; everything below is bookkeeping.
// ---------------------------------------------------------------------------
namespace {

// A working-surface corner. --radius-sm, which after the owner's 2026-08-15
// geometry call (theme.h) is the instrument-grade 3px rather than the hub's
// 12px: a 20px clip cell with a 12px corner is a lozenge, and a grid of
// lozenges is a toy. Spelled once here so the grid, the stop row and the strip
// heads cannot drift apart.
constexpr f32 kCellRadius = nx::radiusSm;

// A card-tier surface that works over THIS field, as ONE gradient.
//
// Two things are going on here, and both are worth stating.
//
// §2's --glass-1 is a 0.09-alpha white ramp draining to a 0.34-alpha panel
// tone. It is drawn to sit over the bright shoulder of the nebula, where that
// is plenty; the session's furniture sits over the app's own ground, which is
// nearly black, and there the ramp alone adds almost nothing -- the column
// reads as a hole rather than as a surface. §10 says exactly what to do about
// that on a toolkit with no compositor blur: "--panel-toned surfaces with a
// subtle top-light gradient".
//
// The obvious way to write that is a --panel fill with --glass-1 over it. This
// is that, pre-composited into a single three-stop ramp on --glass-1's own
// 157deg axis: same light, same drain to the panel tone in the bottom-right,
// one full-height quad per column instead of two. The columns are 715px tall
// and there are three of them, so the second quad was a third of a megapixel a
// frame for a difference no eye can find.
// Tuned against the palette as it stands, not against the hub's: pal::panel is
// a near-black 0x100B1D at 0.92 now, because expensive is darkness with small
// luminous accents and a violet wash over square metres of surface is what
// reads as a toy. So this is that panel with a BREATH of light collecting in
// the top-left, and nothing more.
inline constexpr nx::Grad panelGlass = {
    {{rgba(0x1F1738, 0.86f), 0.00f},
     {rgba(0x161029, 0.90f), 0.34f},
     {rgba(0x100B1D, 0.94f), 1.00f}}, 3, 157.f};

void panelSurface(Renderer& r, const Rect& b, f32 radius = 0.f) {
    r.gradRect(b, radius, panelGlass);
}

// A rect on whole device pixels. gradStroke() snaps itself; roundRectOutline()
// draws what it is given, and a 1px selection edge that lands on a half pixel
// under fractional DPI scale is a two-pixel smear (§11: crisp, 1px, violet).
inline Rect snapRect(const Rect& r) {
    const f32 x = nx::snapPx(r.x), y = nx::snapPx(r.y);
    return {x, y, nx::snapPx(r.right()) - x, nx::snapPx(r.bottom()) - y};
}

// ---------------------------------------------------------------------------
// The clip grid's text, held back and drawn in one run.
//
// The batcher binds the glyph atlas for text and unbinds it for shapes, and
// every switch costs a draw call (Renderer::useTexture). A grid that drew each
// cell complete therefore paid two draw calls per visible cell -- measured at
// 542 on a 32x32 set. Collecting the labels while the shapes go down and
// flushing them at the end emits exactly the same quads through two binds.
//
// The label is COPIED, not pointed at. Between the collect and the flush this
// same loop handles clicks, and a click can clear a slot, load a sample into
// one or make a new pattern -- every one of which frees the std::string the
// pointer would have come from.
// ---------------------------------------------------------------------------
struct SlotLabel {
    Rect  box;
    Col   ink;
    bool  small = false;
    Align align = Align::Left;
    f32   pad = 0.f;
    char  text[48] = {};
};
std::vector<SlotLabel> g_slotText;

void pushSlotLabel(const Rect& box, const char* s, const Col& ink,
                   bool small = false, Align a = Align::Left, f32 pad = 0.f) {
    if (!s || !*s || box.w <= 0.f) return;
    SlotLabel l;
    l.box = box; l.ink = ink; l.small = small; l.align = a; l.pad = pad;
    std::snprintf(l.text, sizeof l.text, "%s", s);
    g_slotText.push_back(l);
}

// ---------------------------------------------------------------------------
// The chain a bus shows where a track shows its clips.
//
// ONE function, because the return strips and the master strip drew the same
// list twice with a 3px inset and a 4px inset, and put "no fx" at body.y + 6
// against a first device row at body.y + 4. Across the five columns of a busy
// session that reads as a wobble -- the labels of the empty buses sit a couple
// of pixels below the device names beside them, which is precisely the kind of
// thing §11 says is invisible in a diff and obvious in a screenshot.
//
// Geometry is now stated once: rows are 12px tall on a 14px pitch, inset 4px,
// starting 4px down -- and the empty label occupies exactly the rect the first
// row would have, so it lands on that row's baseline by construction rather
// than by two numbers agreeing.
//
// Returns the name that wants a tooltip (a device whose name the column had to
// cut), or an empty string.
// ---------------------------------------------------------------------------

constexpr f32 kChainRowH   = 12.f;
constexpr f32 kChainRowPitch = 14.f;
constexpr f32 kChainInset  = 4.f;

std::string drawChainList(Renderer& r, Ui& ui, const Font& fSmall, const Rect& body,
                          const std::vector<DeviceModel>& devices, f32 s, f32 rad) {
    const Rect first{body.x + kChainInset * s, body.y + kChainInset * s,
                     body.w - kChainInset * 2.f * s, kChainRowH * s};
    if (devices.empty()) {
        // A micro-label chip, in the first row's own rect: inert, and §5's
        // "muted = inert" is the whole of what it has room to say.
        ui.microIn(fSmall, first, "no fx", nx::muted.alpha(0.7f), Align::Center, 0);
        return {};
    }
    std::string tip;
    f32 dy = first.y;
    for (const DeviceModel& d : devices) {
        if (dy + kChainRowH * s > body.bottom()) break;
        const Rect row{first.x, dy, first.w, kChainRowH * s};
        r.well(row, rad);
        r.pushClip(row);
        r.textIn(fSmall, row, d.desc.name.c_str(),
                 d.inst ? nx::muted : nx::danger, Align::Left, 3 * s);
        r.popClip();
        // "EQ Three" in a 54px return strip becomes "EQ Thr..." -- a name the
        // user cannot read and, until now, had no way to.
        if (ui.hovered(row) && textTruncated(fSmall, d.desc.name.c_str(), row.w - 6 * s))
            tip = d.desc.name;
        dy += kChainRowPitch * s;
    }
    return tip;
}

} // namespace

// One decoded file, as a clip. Factored out of loadClipInto because the
// arrangement can be dropped on directly (docs/ARRANGEMENT.md §7.5) and an item
// owns its clip by value, so there is no slot in the middle -- and because two
// places building a clip from a file two ways is how they end up disagreeing
// about the default loop, the guessed tempo or the warp mode.
//
// Deliberately does NOT touch the session: no uid, no undo point, no push. The
// caller decides what the clip becomes.
bool App::makeClipFromFile(const std::string& path, int colorIdx, ClipModel& out) {
    SampleRef sb = loadSample(path, eng_.sampleRate());
    if (!sb) return false;
    out = ClipModel{};
    out.kind = ClipKind::Audio;
    out.sample = sb;
    out.path = path;
    out.name = sb->name;
    const size_t dot = out.name.find_last_of('.');
    if (dot != std::string::npos) out.name = out.name.substr(0, dot);
    out.colorIdx = colorIdx;
    out.clipBpm = sb->guessedBpm;
    out.lengthBeats = sb->guessedBeats;
    out.loopStart = 0;
    out.loopEnd = sb->frames;
    out.gain = 1.f;
    out.warp = Warp::Beats;
    out.loop = true;
    return true;
}

void App::loadClipInto(int track, int slot, const std::string& path) {
    if (track < 0 || track >= (int)ses_.tracks.size()) return;
    if (slot < 0 || slot >= (int)ses_.scenes.size()) return;
    ClipModel fresh;
    if (!makeClipFromFile(path, ses_.tracks[track].colorIdx, fresh)) {
        status_ = "Could not load " + path;
        return;
    }
    // After the decode, so a file that could not be read leaves no history
    // behind, and before the slot is touched.
    undoPoint("load clip");

    ClipModel& m = ses_.tracks[track].slots[slot];
    // A slot that already held a clip keeps its identity: the material behind
    // it changed, but anything pointing at the clip (automation, a controller
    // mapping) still means this clip. Kept across the assignment for the same
    // reason, since `fresh` has no uid of its own.
    const u64 keep = m.uid ? m.uid : ses_.newUid();
    // Dropping a sample onto a pattern turns the slot back into an audio clip;
    // pushClip retires the notes the engine was holding for it.
    m = std::move(fresh);
    m.uid = keep;
    pushClip(track, slot);
    selectTrack(track); selSlot_ = slot;
    status_ = "Loaded " + m.name;
}

void App::clearClip(int track, int slot) {
    ses_.tracks[track].slots[slot] = ClipModel{};
    // Through pushClip rather than a bare ClearClip: an emptied slot still has
    // to hand its note array back before anything frees it.
    pushClip(track, slot);
}

// Note-capable means the chain can be *played*: an instrument, or an effect
// that takes MIDI in (an arpeggiator, a MIDI-controlled filter). Either makes
// the track's empty slots MIDI targets rather than audio ones.
bool App::trackHasNoteDevice(int track) const {
    if (track < 0 || track >= (int)ses_.tracks.size()) return false;
    for (const DeviceModel& d : ses_.tracks[track].devices)
        if (d.desc.kind == PluginKind::Instrument || d.desc.hasMidiIn) return true;
    return false;
}

// An empty MIDI clip is a real, launchable, editable entity — Live's "create
// empty clip", and the only way to get a pattern without playing one in.
void App::createMidiClip(int track, int slot) {
    if (track < 0 || track >= (int)ses_.tracks.size()) return;
    if (slot < 0 || slot >= (int)ses_.scenes.size()) return;

    // Here rather than at the (single) call site: this is the whole edit, and
    // the slot is untouched until the next line.
    undoPoint("new clip");

    ClipModel& m = ses_.tracks[track].slots[slot];
    m = ClipModel{};
    m.uid = ses_.newUid();
    m.kind = ClipKind::Midi;
    char buf[32];
    snprintf(buf, sizeof buf, "MIDI %d", midiClipNo_++);
    m.name = buf;
    m.colorIdx = ses_.tracks[track].colorIdx;
    m.lengthBeats = 4.0;                       // one bar in 4/4, like Live
    m.loop = true;
    pushClip(track, slot);
    selectTrack(track); selSlot_ = slot;
    detailTab_ = DetailTab::Clip;
    status_ = "New " + m.name;
}

// Points the DEVICES tab somewhere. Not an edit and not undoable -- it is the
// same kind of move as selecting a track, which is explicitly outside the
// history (see app.h).
void App::selectChainOwner(int owner) {
    if (!chainOwner(owner).valid()) return;
    if (devOwner_ != owner) {
        selDevice_ = -1;
        stripScroll_ = 0.f;
        paramScroll_ = 0.f;
    }
    devOwner_ = owner;
    // A bus has no clips, so the CLIP tab has nothing to show for it and the
    // panel would sit there looking at the last track's clip instead. Only the
    // tab is switched: a hidden panel stays hidden.
    if (!ownIsTrack(owner) && detailTab_ != DetailTab::Devices) {
        detailTab_ = DetailTab::Devices;
        ensurePluginScan();
    }
}

// Live's exclusive record-arm, which is what makes the computer keyboard and a
// controller play the track you just clicked on without a second gesture. The
// arm this hands out is ours to take back; one the user set by hand is not.
void App::selectTrack(int track) {
    if (track < 0 || track >= (int)ses_.tracks.size()) return;
    selTrack_ = track;
    // The device view follows the selection back off a bus. Guarded so that
    // clicking around the grid on the track already selected does not reset the
    // chain the user is editing every frame.
    if (devOwner_ != track) selectChainOwner(track);
    if (autoArmed_ == track) return;

    if (autoArmed_ >= 0 && autoArmed_ < (int)ses_.tracks.size()) {
        TrackModel& prev = ses_.tracks[autoArmed_];
        if (prev.arm) { prev.arm = false; send(Cmd::TrackArm, autoArmed_, 0); }
    }
    autoArmed_ = -1;

    TrackModel& t = ses_.tracks[track];
    if (t.arm) return;              // armed by hand: leave it, and do not claim it
    t.arm = true;
    send(Cmd::TrackArm, track, 1);
    autoArmed_ = track;
}

void App::addTrack() {
    if (ses_.tracks.size() >= kMaxTracks) return;
    TrackModel t;
    char buf[32];
    snprintf(buf, sizeof buf, "%zu Audio", ses_.tracks.size() + 1);
    t.uid = ses_.newUid();
    t.name = buf;
    t.colorIdx = (int)(ses_.tracks.size() * 3 + 4) % pal::clipColorCount;
    ses_.tracks.push_back(std::move(t));   // TrackModel is move-only (devices)
    pushTrack((int)ses_.tracks.size() - 1);
}

void App::addScene() {
    if (ses_.scenes.size() >= kMaxScenes) return;
    SceneModel s;
    char buf[32];
    snprintf(buf, sizeof buf, "Scene %zu", ses_.scenes.size() + 1);
    s.uid = ses_.newUid();
    s.name = buf;
    ses_.scenes.push_back(s);
}


// ---------------------------------------------------------------------------
// session view
// ---------------------------------------------------------------------------

void App::drawSessionView(const Rect& r) {
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    rend_.rect(r, pal::appBg);

    // Right-hand furniture, in Live's order: the scene launchers stay against
    // the clip grid (their rows line up with it), then the return buses, then
    // the master. Everything the mix ends up in reads left to right.
    const f32 masterW = lay::masterW * s;
    const f32 sceneW  = lay::sceneColW * s;
    const f32 retW    = lay::returnW * s * kMaxReturns;
    Rect masterCol{r.right() - masterW, r.y, masterW, r.h};
    Rect retCol{masterCol.x - retW, r.y, retW, r.h};
    Rect sceneCol{retCol.x - sceneW, r.y, sceneW, r.h};
    Rect tracksCol{r.x, r.y, std::max(0.f, sceneCol.x - r.x), r.h};

    // Horizontal scroll over the track area.
    f32 totalW = 0.f;
    for (const auto& t : ses_.tracks) totalW += t.width * s + lay::gutter * s;
    const f32 maxScroll = std::max(0.f, totalW - tracksCol.w);
    if (tracksCol.contains(in.mx, in.my) && in.wheel != 0.f && in.shift())
        gridScrollX_ = clampv(gridScrollX_ - in.wheel * 60.f * s, 0.f, maxScroll);
    gridScrollX_ = clampv(gridScrollX_, 0.f, maxScroll);

    rend_.pushClip(tracksCol);
    drawTrackHeaders(tracksCol, gridScrollX_);
    drawClipGrid(tracksCol, gridScrollX_);
    drawMixer(tracksCol, gridScrollX_);
    rend_.popClip();

    drawSceneColumn(sceneCol);
    drawReturnStrips(retCol);
    drawMasterStrip(masterCol);
}

void App::drawTrackHeaders(const Rect& r, f32 scrollX) {
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    const f32 h = lay::trackHeadH * s;
    f32 x = r.x - scrollX;

    for (size_t i = 0; i < ses_.tracks.size(); ++i) {
        TrackModel& t = ses_.tracks[i];
        const f32 w = t.width * s;
        Rect cell{x, r.y, w - lay::gutter * s, h};
        x += w;
        if (cell.right() < r.x || cell.x > r.right()) continue;

        const bool sel = (int)i == selTrack_;
        const u64 id = uiId(3, (int)i);
        const bool hot = ui_.setHot(id, cell) && ui_.isHot(id);
        // Chrome, so it takes the tier language: a well at rest, the glass chip
        // under the pointer, and the lit edge only on the track being edited --
        // one surface floating is enough to say which one that is.
        const f32 rad = kCellRadius * s;
        if (sel) {
            rend_.gradRect(cell, rad, nx::glassChip);
            rend_.gradStroke(cell, rad, s, nx::edge, 0.9f);
        } else if (hot) {
            rend_.gradRect(cell, rad, nx::glassChip, 0.55f);
        } else {
            rend_.well(cell, rad);
        }
        // Colour chip so the track's identity reads at a glance, as in Live.
        // Inset by the corner radius so it does not overhang the rounding.
        rend_.rect({cell.x + rad, cell.y, std::max(0.f, cell.w - rad * 2.f),
                    std::max(1.f, nx::snapPx(2 * s))},
                   pal::clipColors[t.colorIdx % pal::clipColorCount]);

        // textField writes the new name and only then says it committed, and it
        // can only commit on a frame where it already owns the caret -- so the
        // old name is captured then, and only then.
        const u64 nameId = uiId(3, 1000 + (int)i);
        std::string wasName;
        if (ui_.editId == nameId) wasName = t.name;
        if (ui_.textField(nameId, cell, &t.name,
                          Col(0, 0, 0, 0), sel ? nx::text : nx::muted, Align::Left))
            undoPointWith("rename track", t.name, wasName);
        // A 94px header cuts most real track names, and a cut name with no way
        // to read it is the §11 defect this fixes: the status bar has the room
        // the header does not. Suppressed while the field is being edited --
        // the caret is already showing the whole string.
        if (hot && ui_.editId != nameId &&
            textTruncated(fBody_, t.name.c_str(), cell.w - 8 * s))
            ui_.tip = t.name;
        if (hot && in.pressed[0]) selectTrack((int)i);
    }

    // "+" to append a track.
    Rect add{x, r.y, 22 * s, h};
    if (add.x < r.right()) {
        if (ui_.button(uiId(3, 900), add, "+")) { undoPoint("add track"); addTrack(); }
    }

    // The header band ends in a hairline, not a painted shelf: §11, no solid
    // grey dividers anywhere.
    rend_.hairlineH(r.x, std::min(std::max(x, r.x + 1.f), r.right()), r.y + h);
}

void App::drawClipGrid(const Rect& r, f32 scrollX) {
    const f32 s = win_.dpiScale();
    const f32 slotH = lay::slotH * s;
    const f32 top = r.y + lay::trackHeadH * s;
    const f32 mixerTop = r.bottom() - lay::mixerH * s;
    const int ns = (int)ses_.scenes.size();

    Rect grid{r.x, top, r.w, mixerTop - top};
    rend_.pushClip(grid);
    // The working surface recesses: one quad behind the whole grid, and the §3
    // field goes on showing through it.
    //
    // FLAT, deliberately. --well-deep ramps 0.62 -> 0.46 alpha of the same
    // near-black, which stretched over a third of the window is a gradient
    // nobody can see -- and the four big gradient fills this view was carrying
    // measured about 0.1 ms a frame together under NXTAKT_GFX_STATS on the
    // software rasteriser the headless harness runs on. §1 says halve a
    // gradient you can see from across the room; a gradient you cannot see up
    // close should simply be a colour. This one is --well-deep's own top stop.
    rend_.rect(grid, rgba(0x04020A, 0.55f));
    g_slotText.clear();

    f32 x = r.x - scrollX;
    for (size_t ti = 0; ti < ses_.tracks.size(); ++ti) {
        const f32 w = ses_.tracks[ti].width * s;
        // The lanes used to be painted bands, two greys apart. They are rules
        // now: the selected track carries a whisper of violet (§1, violet
        // leads) and the boundaries are hairlines that fade at both ends, so
        // the grid reads as ruled rather than as striped.
        const Rect lane{x, top, w - lay::gutter * s, grid.h};
        if (lane.right() >= r.x && lane.x <= r.right()) {
            if ((int)ti == selTrack_) rend_.rect(lane, nx::violet.alpha(0.04f));
            rend_.hairlineV(lane.right(), top, grid.bottom(),
                            nx::hairlineInk.alpha(0.10f));
        }
        for (int si = 0; si < ns; ++si) {
            Rect cell{x, top + si * slotH, w - lay::gutter * s, slotH - lay::gutter * s};
            if (cell.bottom() > mixerTop) break;
            if (cell.right() >= r.x && cell.x <= r.right()) drawClipSlot(cell, (int)ti, si);
        }
        // Per-track stop button, directly under the last scene row.
        Rect stopCell{x, top + ns * slotH, w - lay::gutter * s, slotH - lay::gutter * s};
        if (stopCell.bottom() <= mixerTop && stopCell.right() >= r.x && stopCell.x <= r.right()) {
            const u64 id = uiId(4, 5000 + (int)ti);
            const bool hot = ui_.setHot(id, stopCell) && ui_.isHot(id);
            rend_.roundRect(stopCell, kCellRadius * s, hot ? pal::slotHover : pal::slotEmpty);
            ui_.stopSquare(stopCell, hot ? nx::text : nx::muted);
            if (hot) ui_.cursor = Cursor::Hand;
            if (hot && win_.input().pressed[0]) send(Cmd::StopTrack, (int)ti);
        }
        x += w;
    }

    // Every clip name in the grid, in one run. See SlotLabel: the whole grid
    // now costs a couple of texture binds where it used to cost two per cell.
    for (const SlotLabel& l : g_slotText)
        rend_.textIn(l.small ? fSmall_ : fBody_, l.box, l.text, l.ink, l.align, l.pad);
    g_slotText.clear();
    rend_.popClip();
}

void App::drawClipSlot(const Rect& cell, int ti, int si) {
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    const ClipModel& m = ses_.tracks[ti].slots[si];
    const u64 id = uiId(4, ti, si);
    const bool hot = ui_.setHot(id, cell) && ui_.isHot(id);
    const bool sel = ti == selTrack_ && si == selSlot_;

    // ONE sample of the engine, taken at the top of the frame (engine_state.h).
    // These four used to be four independent relaxed loads made microseconds to
    // milliseconds apart, and two of them could straddle a publish: a slot drawn
    // Playing with activeSlot == -1 is the shape that produced.
    const int state = es_.slotState[ti];
    const int active = es_.activeSlot[ti];
    const int pending = es_.pendingSlot[ti];
    const bool playing = (state == (int)SlotState::Playing || state == (int)SlotState::StopQueued) && active == si;
    const bool queued  = pending == si;

    // Recording truth comes from the engine, not from what we asked for: the
    // start is quantized, so a slot can sit queued for a bar before it captures.
    const int recPhase = es_.recState[ti];
    const bool recHere = recPhase != 0 && es_.recSlotIdx[ti] == si;

    // Flat, one quad, no gradient: 1024 of these are on screen in a big set.
    const f32 rad = kCellRadius * s;
    const f32 hair = std::max(1.f, nx::snapPx(s));

    if (!m.valid()) {
        const bool target = recIntent_ && ses_.tracks[ti].arm;
        if (recHere && recPhase >= 2) {
            // Capturing. Solid red, with the beats it has been running for.
            rend_.roundRect(cell, rad, pal::recRed);
            rend_.circle(cell.x + 8 * s, cell.cy(), 3.5f * s, pal::textOnClip);
            char buf[24];
            snprintf(buf, sizeof buf, "%.1f",
                     std::max(0.0, es_.beat - recStartBeat_[ti]));
            pushSlotLabel({cell.x + 14 * s, cell.y, cell.w - 18 * s, cell.h},
                          buf, pal::textOnClip, true, Align::Right, 0.f);
        } else if (recHere) {
            // Queued: a pulsing ring, the record-side counterpart of the
            // blinking clip a launch shows while it waits for the quantum.
            // §6 is non-negotiable: under reduced motion the ring is simply
            // lit. It still says "queued" -- it just stops moving.
            const f32 ph = nx::reducedMotion()
                         ? 1.f : (f32)(0.5 + 0.5 * std::sin(nowSeconds() * 8.0));
            rend_.roundRect(cell, rad, pal::slotEmpty);
            rend_.roundRectOutline(snapRect(cell), rad, 1.5f * s,
                                   pal::recRed.scale(0.35f + 0.4f * ph));
        } else {
            // An empty slot is a well over the field: `slotEmpty` is
            // translucent, so the nebula goes on breathing through the empty
            // half of the grid. That is the whole mechanism of §4's faked
            // glass, and it costs one quad.
            rend_.roundRect(cell, rad, hot ? pal::slotHover : pal::slotEmpty);
            // Armed track, record intent lit: this slot is a take waiting to
            // happen, so say so before the click rather than after.
            if (target) rend_.circle(cell.x + 8 * s, cell.cy(), 3 * s,
                                     pal::recRed.scale(hot ? 0.9f : 0.55f));
        }
        if (sel) rend_.roundRectOutline(snapRect(cell), rad, hair, nx::violet);
        if (hot) {
            ui_.cursor = Cursor::Hand;
            if (in.pressed[0]) {
                selectTrack(ti); selSlot_ = si;
                if (recHere)      stopRecording(ti);       // second click stops
                else if (target)  startRecording(ti, si);
            }
            // Double-click on an empty slot of a note-capable track makes an
            // empty pattern to draw into. Only when the record button is unlit:
            // with it lit the same slot is a take waiting to happen, and the
            // first click of the double has already started one.
            if (in.dblClick && !recIntent_ && !recHere && trackHasNoteDevice(ti))
                createMidiClip(ti, si);
        }
        return;
    }

    const Col base = pal::clipColors[m.colorIdx % pal::clipColorCount];
    Col fill = base.scale(playing ? 1.0f : (hot ? 0.88f : 0.76f));
    if (queued) {
        // Pulse while waiting for the launch quantum, like Live's blinking
        // slot -- frozen at its bright end under reduced motion (§6).
        const f32 ph = nx::reducedMotion()
                     ? 1.f : (f32)(0.5 + 0.5 * std::sin(nowSeconds() * 8.0));
        fill = base.scale(0.55f + 0.45f * ph);
    }
    rend_.roundRect(cell, rad, fill);
    // One lit pixel along the top edge, the light arriving upper-left exactly
    // as it does in every gradient in the system (§11). Flat, so the grid stays
    // flat: a highlight, not a gradient.
    rend_.rect({cell.x + rad, cell.y, std::max(0.f, cell.w - rad * 2.f), hair},
               fill.scale(1.35f).alpha(0.9f));

    // Launch button zone on the left. Cyan while it plays: §1 reserves cyan for
    // light inside a material -- live values, playheads, running state.
    const f32 btnW = 14 * s;
    Rect btn{cell.x, cell.y, btnW, cell.h};
    if (playing) ui_.playTriangle(btn.insetXY(4.5f * s, 4.5f * s), nx::live);
    else         ui_.playTriangle(btn.insetXY(4.5f * s, 4.5f * s), pal::textOnClip.alpha(0.55f));

    // Recording into a slot that already holds a clip is an overdub, so the
    // slot keeps its playing look and gains the record dot rather than turning
    // solid red the way a slot being captured into from empty does: what is on
    // screen is still the clip, and it is still playing.
    f32 nameW = cell.w - btnW - 2 * s;
    f32 markRight = cell.right();
    if (recHere) {
        // Pulsing while the take waits for its quantum, solid once it is
        // capturing - the same two states the empty-slot look has, said quietly.
        const f32 a = (recPhase >= 2 || nx::reducedMotion())
                    ? 1.f : (f32)(0.45 + 0.45 * std::sin(nowSeconds() * 8.0));
        rend_.circle(markRight - 7 * s, cell.cy(), 3.5f * s, pal::recRed.alpha(a));
        markRight -= 13 * s;
        nameW -= 13 * s;
    }

    // A MIDI clip gets a three-dot mark on the right: at 21px of row height a
    // real piano glyph is a smudge, and the dots read as "notes, not audio"
    // without competing with the name.
    if (m.kind == ClipKind::Midi) {
        const f32 d = 1.6f * s;
        const f32 dx0 = markRight - 12 * s;
        for (int i = 0; i < 3; ++i)
            rend_.rect({dx0 + i * 3.5f * s, cell.cy() - d * 0.5f - (i == 1 ? 2 * s : 0.f), d, d},
                       pal::textOnClip.alpha(0.6f));
        nameW -= 14 * s;
    }
    pushSlotLabel({cell.x + btnW, cell.y, std::max(4 * s, nameW), cell.h},
                  m.name.c_str(), pal::textOnClip, false, Align::Left, 2 * s);

    // Playback progress along the bottom edge, in cyan -- the same light the
    // playhead and the meters carry. The engine publishes clipPhase for a MIDI
    // clip exactly as for an audio one, so this needs no special case.
    if (playing) {
        const f64 ph = clampv(es_.clipPhase[ti], 0.0, 1.0);
        rend_.rect({cell.x, cell.bottom() - 2 * s, cell.w * (f32)ph, 2 * s},
                   nx::live.alpha(0.85f));
    }
    if (sel) rend_.roundRectOutline(snapRect(cell), rad, hair, nx::violet);

    if (hot) {
        ui_.cursor = Cursor::Hand;
        if (in.pressed[0]) {
            selectTrack(ti); selSlot_ = si;
            // With the record button lit, a MIDI clip on an armed track is an
            // overdub target and not just something to launch: the engine
            // relaunches it at the record boundary and captures another pass
            // into it (see the Cmd::RecordMidiSlot contract). A second click
            // stops the take, exactly as on an empty slot. Audio clips are
            // untouched by this - there is no overdub for a sample.
            const bool overdub = recIntent_ && ses_.tracks[ti].arm &&
                                 m.kind == ClipKind::Midi && trackHasNoteDevice(ti);
            if (recHere)       stopRecording(ti);
            else if (overdub)  startRecording(ti, si);
            else               send(Cmd::LaunchClip, ti, si);
            drag_.kind = DragState::Kind::Clip;
            drag_.srcTrack = ti; drag_.srcSlot = si;
            drag_.startX = in.mx; drag_.startY = in.my;
            drag_.armed = false;
        }
        if (in.pressed[2]) {
            selectTrack(ti); selSlot_ = si;
            undoPoint("clear clip");
            clearClip(ti, si);
        }
    }

    // Drop target for a drag in flight.
    if (drag_.kind != DragState::Kind::None && drag_.armed && hot && in.released[0]) {
        if (drag_.kind == DragState::Kind::BrowserFile) {
            loadClipInto(ti, si, drag_.path);   // takes its own entry, after the decode
        } else if (drag_.srcTrack != ti || drag_.srcSlot != si) {
            // One entry for the whole move: the destination write and the
            // source clear are halves of the same edit.
            undoPoint(in.ctrl() ? "copy clip" : "move clip");
            ses_.tracks[ti].slots[si] = ses_.tracks[drag_.srcTrack].slots[drag_.srcSlot];
            if (!in.ctrl()) clearClip(drag_.srcTrack, drag_.srcSlot);
            pushClip(ti, si);
        }
        drag_ = DragState{};
    }
}

void App::drawSceneColumn(const Rect& r) {
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    const f32 slotH = lay::slotH * s;
    const f32 top = r.y + lay::trackHeadH * s;
    const int ns = (int)ses_.scenes.size();

    // Chrome: a card-tier surface, its glass faked from the fill (§4 -- there
    // are a dozen panels on this screen and not one of them is worth a real
    // blur), with a hairline where a solid divider used to be.
    panelSurface(rend_, r);
    rend_.hairlineV(r.x, r.y, r.bottom());

    Rect head{r.x, r.y, r.w, lay::trackHeadH * s};
    ui_.microIn(fSmall_, head, "SCENES", nx::muted, Align::Center);
    rend_.hairlineH(head.x + nx::sp1 * s, head.right() - nx::sp1 * s, head.bottom());

    const f32 rad = kCellRadius * s;
    for (int si = 0; si < ns; ++si) {
        Rect cell{r.x + 2 * s, top + si * slotH, r.w - 4 * s, slotH - lay::gutter * s};
        if (cell.bottom() > r.bottom() - lay::mixerH * s) break;
        const u64 id = uiId(5, si);
        const bool hot = ui_.setHot(id, cell) && ui_.isHot(id);
        const bool sel = si == selSlot_;
        // Well rows, exactly as the specimen sheet has them: nothing at rest,
        // the glass chip under the pointer, and a hairline between neighbours.
        if (sel)      rend_.gradRect(cell, rad, nx::glassChip, 0.85f);
        else if (hot) rend_.gradRect(cell, rad, nx::glassChip, 0.45f);
        if (sel) rend_.rect({nx::snapPx(cell.x), cell.y, std::max(1.f, nx::snapPx(2 * s)),
                             cell.h}, nx::violet);
        if (si + 1 < ns)
            rend_.hairlineH(cell.x + nx::sp1 * s, cell.right() - nx::sp1 * s, cell.bottom(),
                            nx::hairlineInk.alpha(0.08f));

        Rect btn{cell.x, cell.y, 14 * s, cell.h};
        ui_.playTriangle(btn.insetXY(4.5f * s, 4.5f * s), sel ? nx::text : nx::muted);
        // A scene may carry a TEMPO, and a scene that does changes the transport
        // the moment it is launched. Nothing on this column used to say so: the
        // number was in the set, in the file and in the engine, and invisible.
        // It is a data read-out, so it gets the live ink cyan carries elsewhere
        // for "this is what will happen", right-aligned in its own socket with
        // the name field ending where the socket begins.
        //
        // Locale-independent by construction (§7): "%g" on a value the model has
        // already clamped to 20..999, never a formatter that reads LC_NUMERIC.
        char bpm[16] = "";
        f32 bpmW = 0.f;
        if (ses_.scenes[si].tempo > 0.0) {
            snprintf(bpm, sizeof bpm, "%g", ses_.scenes[si].tempo);
            bpmW = fSmall_.measure(bpm) + 6 * s;
        }
        const Rect nameR{cell.x + 14 * s, cell.y, cell.w - 16 * s - bpmW, cell.h};
        const u64 nameId = uiId(5, 1000 + si);
        std::string wasName;                     // see drawTrackHeaders
        if (ui_.editId == nameId) wasName = ses_.scenes[si].name;
        if (ui_.textField(nameId, nameR, &ses_.scenes[si].name, Col(0, 0, 0, 0),
                          sel ? nx::text : nx::muted, Align::Left))
            undoPointWith("rename scene", ses_.scenes[si].name, wasName);
        if (bpm[0])
            rend_.textIn(fSmall_, {nameR.right(), cell.y, bpmW, cell.h}, bpm,
                         nx::live.alpha(sel ? 0.95f : 0.75f), Align::Right, 2 * s);

        if (hot) ui_.cursor = Cursor::Hand;
        // A scene name wide enough to be cut is a name the column cannot show,
        // so the status bar says it in full (§11: no truncated name without a
        // tip). The tempo is named too -- a bare number beside a scene is only
        // obvious once you already know what it does.
        if (hot && ui_.editId != nameId) {
            const bool cut = textTruncated(fBody_, ses_.scenes[si].name.c_str(),
                                           nameR.w - 4 * s);
            if (cut || bpm[0]) {
                ui_.tip = ses_.scenes[si].name;
                if (bpm[0]) ui_.tip += std::string(" - launches at ") + bpm + " BPM";
            }
        }
        if (hot && in.pressed[0]) { selSlot_ = si; send(Cmd::LaunchScene, si); }
    }

    Rect stopAll{r.x + 2 * s, top + ns * slotH, r.w - 4 * s, slotH - lay::gutter * s};
    if (stopAll.bottom() <= r.bottom() - lay::mixerH * s) {
        if (ui_.button(uiId(5, 900), stopAll, "STOP ALL")) send(Cmd::StopAll);
    }

    // "+ SCENE", not "+ Scene": it sits directly under STOP ALL, and two chrome
    // actions in one cluster spelled two different ways is exactly the
    // inconsistent capitalisation §9 rules out. Uppercase is the spelling every
    // other action chip in the program uses (LOOP, APPLY, MAP, STOP ALL).
    Rect add{r.x + 2 * s, stopAll.bottom() + 4 * s, r.w - 4 * s, 18 * s};
    if (add.bottom() <= r.bottom() - lay::mixerH * s) {
        if (ui_.button(uiId(5, 901), add, "+ SCENE")) { undoPoint("add scene"); addScene(); }
    }
}

void App::drawMixer(const Rect& r, f32 scrollX) {
    const f32 s = win_.dpiScale();
    const f32 top = r.bottom() - lay::mixerH * s;
    Rect mix{r.x, top, r.w, lay::mixerH * s};
    rend_.pushClip(mix);
    // A docked band of controls, so it takes the bar fill and a hairline along
    // its top edge rather than the solid rule it used to have.
    rend_.gradRect(mix, 0.f, nx::glassBar);
    rend_.hairlineH(mix.x, mix.right(), mix.y);

    f32 x = r.x - scrollX;
    for (size_t ti = 0; ti < ses_.tracks.size(); ++ti) {
        TrackModel& t = ses_.tracks[ti];
        const f32 w = t.width * s;
        Rect col{x, top, w - lay::gutter * s, mix.h};
        x += w;
        if (col.right() < r.x || col.x > r.right()) continue;
        // The same violet wash the selected lane carries upstairs, so a track
        // reads as one column from its header to its fader.
        if ((int)ti == selTrack_) rend_.rect(col, nx::violet.alpha(0.04f));
        rend_.hairlineV(col.right(), mix.y + nx::sp1 * s, mix.bottom() - nx::sp1 * s,
                        nx::hairlineInk.alpha(0.10f));

        f32 y = col.y + 6 * s;

        // M / S / arm: ONE cluster, not three capsules floating in gaps. Mute,
        // solo and arm are the three switches of one channel strip -- on a
        // desk they are three keys in a single machined block, and that is what
        // the eye should group. The plate and the lit edge belong to the
        // cluster; the segments separate by hairline and only fill when they
        // are on.
        const Rect trio{col.x + 6 * s, y, col.w - 12 * s, 15 * s};
        ui_.segCluster(trio);
        const f32 bw = trio.w / 3.f;
        Rect mr{trio.x, y, bw, trio.h};
        Rect sr{trio.x + bw, y, bw, trio.h};
        Rect ar{trio.x + bw * 2.f, y, trio.w - bw * 2.f, trio.h};
        rend_.hairlineV(sr.x, trio.y + 3 * s, trio.bottom() - 3 * s);
        rend_.hairlineV(ar.x, trio.y + 3 * s, trio.bottom() - 3 * s);
        // Every control in this strip is bound straight to the model and writes
        // before it reports, so each hands its previous value to the entry.
        const bool wasMute = t.mute, wasSolo = t.solo, wasArm = t.arm;
        const f32  wasPan = t.pan, wasFader = t.fader;
        // Every automatable control on this strip reports its move to
        // autoCapture as well as to the engine (docs/AUTOMATION.md §5.1). The
        // call is unconditional by design: whether anything is recorded — the
        // arm, the transport, which clip is playing, the beat, the thinning —
        // is one decision and it lives in autoCapture, so a second copy of it
        // here could only ever come to disagree. The value handed over is what
        // the widget just wrote into the model, in the target's own units
        // (§2.3), and the widget's id is the gesture, so one drag is one pass
        // and one undo entry.
        if (ui_.segButton(uiId(6, (int)ti, 0), mr, t.mute, pal::meterAmber)) {
            t.mute = !t.mute;
            undoPointWith("mute", t.mute, wasMute);
            send(Cmd::TrackMute, (int)ti, t.mute ? 1 : 0);
            // Mute has no AutoTarget yet (it is reserved), so this records into
            // a lane the publisher will skip until it does. Spelled anyway: the
            // call site is the part that is easy to forget when it lands.
            autoCapture(addr::trackField(t.uid, "mute"), t.mute ? 1.f : 0.f,
                        uiId(6, (int)ti, 0));
        }
        ui_.microIn(fSmall_, ui_.lastRect, "M",
                    t.mute ? nx::inkOn(pal::meterAmber) : nx::muted, Align::Center);
        if (ui_.segButton(uiId(6, (int)ti, 1), sr, t.solo, pal::soloBlue)) {
            t.solo = !t.solo;
            undoPointWith("solo", t.solo, wasSolo);
            send(Cmd::TrackSolo, (int)ti, t.solo ? 1 : 0);
        }
        ui_.microIn(fSmall_, ui_.lastRect, "S",
                    t.solo ? nx::inkOn(pal::soloBlue) : nx::muted, Align::Center);
        // Record-arm is a filled dot in Live, and the glyph atlas is ASCII-only,
        // so draw the dot rather than trying to letter it.
        if (ui_.segButton(uiId(6, (int)ti, 2), ar, t.arm, pal::armRed)) {
            t.arm = !t.arm;
            // Arming by hand is an edit; the auto-arm that follows the
            // selection is not, and takes no entry of its own.
            undoPointWith("arm", t.arm, wasArm);
            send(Cmd::TrackArm, (int)ti, t.arm ? 1 : 0);
            // Touched by hand: this arm is the user's now, so selecting another
            // track must not take it away again.
            if ((int)ti == autoArmed_) autoArmed_ = -1;
        }
        rend_.circle(ui_.lastRect.cx(), ui_.lastRect.cy(), 3.5f * s,
                     t.arm ? nx::text : pal::recRed.scale(0.55f));
        y += 20 * s;

        // Sends A-D, above the pan knob as a 2x2 grid. A strip is 94px wide, so
        // four knobs in a row would be 12px across and unusable; two rows of two
        // leave room for a 15px knob with its letter beside it, which is the
        // smallest thing here that still reads as a send and not as a dot.
        // Anything the user has dialled in also shows as an arc, so a track with
        // send on it is visible without hovering.
        {
            const f32 cellW = (col.w - 12 * s) * 0.5f;
            const f32 rowH  = 18 * s;
            for (int rn = 0; rn < kMaxReturns; ++rn) {
                Rect cell{col.x + 6 * s + (rn % 2) * cellW, y + (rn / 2) * rowH, cellW, rowH};
                ui_.microIn(fSmall_, {cell.x, cell.y, 9 * s, cell.h}, kReturnLetter[rn],
                            nx::muted.alpha(0.75f), Align::Left, 0);
                Rect kr{cell.x + 10 * s, cell.y + 1 * s, 15 * s, 15 * s};
                const f32 wasSend = t.sends[rn];
                if (ui_.knob(uiId(6, (int)ti, 10 + rn), kr, &t.sends[rn], 0.f, 1.f, 0.f)) {
                    undoPointWith(kSendUndo[rn], t.sends[rn], wasSend);
                    send(Cmd::SendLevel, (int)ti, rn, t.sends[rn]);
                    autoCapture(addr::trackSend(t.uid, rn), t.sends[rn],
                                uiId(6, (int)ti, 10 + rn));
                }
            }
            y += 2 * rowH + 3 * s;
        }

        // Pan
        Rect pan{col.cx() - 11 * s, y, 22 * s, 22 * s};
        if (ui_.knob(uiId(6, (int)ti, 3), pan, &t.pan, -1.f, 1.f, 0.f)) {
            undoPointWith("pan", t.pan, wasPan);
            send(Cmd::TrackPan, (int)ti, 0, t.pan);
            autoCapture(addr::trackField(t.uid, "pan"), t.pan, uiId(6, (int)ti, 3));
        }
        y += 26 * s;

        // Fader + meter, in a recessed lane. §4: a region inside a glass
        // surface recesses rather than frosting again, and the fader travel and
        // the meter are one instrument, so they share one well.
        const f32 fh = col.bottom() - y - 6 * s;
        Rect fader{col.x + 10 * s, y, 16 * s, fh};
        Rect meter{fader.right() + 5 * s, y, 9 * s, fh};
        rend_.well({fader.x - 4 * s, y - 4 * s, meter.right() - fader.x + 8 * s, fh + 8 * s},
                   nx::radiusXs * s);
        if (ui_.vFader(uiId(6, (int)ti, 4), fader, &t.fader)) {
            undoPointWith("volume", t.fader, wasFader);
            send(Cmd::TrackVol, (int)ti, 0, faderToGain(t.fader));
            // The FADER POSITION, not the gain: the envelope stores what the UI
            // edits and AutoXform::Fader is what turns it into a gain (§2.3).
            autoCapture(addr::trackField(t.uid, "vol"), t.fader, uiId(6, (int)ti, 4));
        }

        const f32 lvl = std::max(es_.meterL[ti], es_.meterR[ti]);
        peakHoldT_[ti] = std::max(lvl, peakHoldT_[ti] * 0.985f);
        ui_.meterV(meter, lvl, peakHoldT_[ti]);
    }
    rend_.popClip();
}

// The A-D buses. No clips, no M/S/arm, no pan: a return is a name, a chain and
// a level, so the strip is a header, the chain's device names where a track has
// its grid, and a fader with its meter. Clicking anywhere that is not a control
// points the DEVICES tab at the bus, which is the only way to edit its chain.
void App::drawReturnStrips(const Rect& r) {
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    if (r.w <= 0.f) return;
    panelSurface(rend_, r);
    rend_.hairlineV(r.x, r.y, r.bottom());

    const f32 colW = r.w / (f32)kMaxReturns;
    const f32 top  = r.bottom() - lay::mixerH * s;
    const f32 rad  = kCellRadius * s;

    for (int i = 0; i < kMaxReturns; ++i) {
        ReturnModel& rt = ses_.returns[i];
        const int owner = ownReturn(i);
        const bool sel  = devOwner_ == owner;
        Rect col{r.x + i * colW, r.y, colW - lay::gutter * s, r.h};

        // Claimed first so the fader and the name field can take hot back --
        // the same last-setHot-wins trick the device boxes use.
        const u64 id = uiId(13, i, 0);
        const bool hot = ui_.setHot(id, col) && ui_.isHot(id);
        if (sel) rend_.rect(col, nx::violet.alpha(0.04f));

        Rect head{col.x, col.y, col.w, lay::trackHeadH * s};
        if (sel) rend_.gradRect(head, rad, nx::glassChip, 0.85f);
        else     rend_.well(head, rad);
        rend_.rect({head.x + rad, head.y, std::max(0.f, head.w - rad * 2.f),
                    std::max(1.f, nx::snapPx(2 * s))}, pal::soloBlue);
        ui_.microIn(fSmall_, {head.x + 3 * s, head.y, 10 * s, head.h}, kReturnLetter[i],
                    sel ? nx::text : nx::muted, Align::Left, 0);
        // The model's placeholder name is "Return" for all four buses, which
        // says nothing in a strip this narrow and would be clipped to "Retu"
        // anyway -- so the letter carries the identity and the field stays
        // blank until the bus is named. A DISPLAY choice, deliberately: writing
        // a letter into the model would make every set on disk carry four
        // return blocks it has no reason to (see project.cpp's `interesting`).
        const u64 nameId = uiId(13, i, 1);
        std::string shown = (rt.name == kReturnPlaceholder) ? std::string() : rt.name;
        if (ui_.textField(nameId, {head.x + 13 * s, head.y, head.w - 15 * s, head.h},
                          &shown, Col(0, 0, 0, 0), sel ? nx::text : nx::muted, Align::Left)) {
            const std::string was = rt.name;
            rt.name = shown.empty() ? std::string(kReturnPlaceholder) : shown;
            undoPointWith("rename return", rt.name, was);
        }

        // What the bus is made of, in the space a track spends on clips. A
        // return with an empty chain is inert, and saying so beats an empty
        // column the user has no reason to click on.
        Rect body{col.x, head.bottom(), col.w, top - head.bottom()};
        rend_.pushClip(body);
        const std::string devTip = drawChainList(rend_, ui_, fSmall_, body, rt.devices, s, rad);
        rend_.popClip();

        Rect mix{col.x, top, col.w, r.bottom() - top};
        rend_.hairlineH(mix.x, mix.right(), mix.y);
        // The same top inset the master strip uses, so the buses and the mix
        // they land in read as one row of faders rather than a staircase.
        f32 y = mix.y + 26 * s;
        const f32 fh = mix.bottom() - y - 6 * s;
        Rect fader{mix.x + 10 * s, y, 15 * s, fh};
        Rect meter{fader.right() + 5 * s, y, 9 * s, fh};
        rend_.well({fader.x - 4 * s, y - 4 * s, meter.right() - fader.x + 8 * s, fh + 8 * s},
                   nx::radiusXs * s);

        const f32 wasFader = rt.fader;
        if (ui_.vFader(uiId(13, i, 2), fader, &rt.fader)) {
            undoPointWith("return volume", rt.fader, wasFader);
            send(Cmd::ReturnVol, i, 0, faderToGain(rt.fader));
        }
        const f32 lvl = std::max(es_.returnMeterL[i], es_.returnMeterR[i]);
        peakHoldR_[i] = std::max(lvl, peakHoldR_[i] * 0.985f);
        ui_.meterV(meter, lvl, peakHoldR_[i]);

        if (sel) rend_.roundRectOutline(snapRect(col), rad, std::max(1.f, nx::snapPx(s)),
                                        nx::violet);
        if (hot) {
            ui_.cursor = Cursor::Hand;
            // The strip is 54px wide: both the bus name and its device names are
            // routinely cut. The tip says whichever the pointer is actually on,
            // the device winning because it is the narrower target.
            if (!devTip.empty()) {
                ui_.tip = devTip;
            } else if (rt.name != kReturnPlaceholder &&
                       textTruncated(fBody_, rt.name.c_str(), head.w - 15 * s) &&
                       ui_.editId != nameId) {
                ui_.tip = rt.name;
            }
            if (in.pressed[0]) selectChainOwner(owner);
        } else if (!devTip.empty()) {
            ui_.tip = devTip;
        }
    }
}

void App::drawMasterStrip(const Rect& r) {
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    const bool sel = devOwner_ == kOwnMaster;
    const f32 rad = kCellRadius * s;
    // Where the whole mix lands, so it is the one column that reads as a
    // surface in its own right: the card fill, and a hairline off the returns.
    panelSurface(rend_, r);
    rend_.hairlineV(r.x, r.y, r.bottom());
    if (sel) rend_.rect(r, nx::violet.alpha(0.04f));

    // Same deal as a return: the strip is the handle for the master chain, so
    // the whole column is a click target that the controls in it take back.
    const u64 id = uiId(7, 10);
    const bool hot = ui_.setHot(id, r) && ui_.isHot(id);

    Rect head{r.x, r.y, r.w, lay::trackHeadH * s};
    if (sel) rend_.gradRect(head, rad, nx::glassChip, 0.85f);
    else     rend_.well(head, rad);
    ui_.microIn(fSmall_, head, "MASTER", nx::text, Align::Center);

    const f32 top = r.bottom() - lay::mixerH * s;
    Rect mix{r.x, top, r.w, lay::mixerH * s};
    rend_.hairlineH(mix.x, mix.right(), mix.y);

    // The master chain, where a return lists its own: this is where a bus
    // compressor or a saturator across the whole mix lives.
    std::string devTip;
    {
        Rect body{r.x, head.bottom(), r.w, top - head.bottom()};
        rend_.pushClip(body);
        devTip = drawChainList(rend_, ui_, fSmall_, body, ses_.masterDevices, s, rad);
        rend_.popClip();
    }

    static f32 masterFader = 0.85f;
    f32 y = mix.y + 26 * s;
    const f32 fh = mix.bottom() - y - 6 * s;
    Rect fader{mix.x + 12 * s, y, 16 * s, fh};
    Rect meterL{fader.right() + 6 * s, y, 9 * s, fh};
    Rect meterR{meterL.right() + 3 * s, y, 9 * s, fh};
    rend_.well({fader.x - 4 * s, y - 4 * s, meterR.right() - fader.x + 8 * s, fh + 8 * s},
               nx::radiusXs * s);

    if (ui_.vFader(uiId(7, 0), fader, &masterFader))
        send(Cmd::MasterVol, 0, 0, faderToGain(masterFader));

    const f32 l = es_.masterMeterL, rr = es_.masterMeterR;
    peakHoldM_[0] = std::max(l, peakHoldM_[0] * 0.985f);
    peakHoldM_[1] = std::max(rr, peakHoldM_[1] * 0.985f);
    ui_.meterV(meterL, l, peakHoldM_[0]);
    ui_.meterV(meterR, rr, peakHoldM_[1]);

    if (sel) rend_.roundRectOutline(snapRect(r), rad, std::max(1.f, nx::snapPx(s)), nx::violet);
    if (!devTip.empty()) ui_.tip = devTip;
    if (hot) {
        ui_.cursor = Cursor::Hand;
        if (in.pressed[0]) selectChainOwner(kOwnMaster);
    }
}


void App::drawDragGhost() {
    Input& in = win_.input();
    if (drag_.kind == DragState::Kind::None) return;
    if (!in.down[0]) { drag_ = DragState{}; return; }

    const f32 dx = in.mx - drag_.startX, dy = in.my - drag_.startY;
    if (!drag_.armed && (dx * dx + dy * dy) > 25.f) drag_.armed = true;
    if (!drag_.armed) return;

    const f32 s = win_.dpiScale();
    ui_.cursor = Cursor::Grab;
    std::string label;
    Col c = nx::violet;
    if (drag_.kind == DragState::Kind::BrowserFile) {
        const size_t sl = drag_.path.find_last_of('/');
        label = sl == std::string::npos ? drag_.path : drag_.path.substr(sl + 1);
    } else {
        const ClipModel& m = ses_.tracks[drag_.srcTrack].slots[drag_.srcSlot];
        label = m.name;
        c = pal::clipColors[m.colorIdx % pal::clipColorCount];
    }
    const f32 w = fBody_.measure(label.c_str()) + 16 * s;
    Rect ghost{in.mx + 10 * s, in.my + 8 * s, w, 18 * s};
    // The one thing in this view that genuinely floats over other content, so
    // it is the one thing that gets an elevation and a lit edge.
    rend_.shadow(ghost, nx::radiusXs * s, nx::shadow);
    rend_.roundRect(ghost, nx::radiusXs * s, c.alpha(0.92f));
    rend_.gradStroke(ghost, nx::radiusXs * s, s, nx::edge, 0.9f);
    rend_.textIn(fBody_, ghost, label.c_str(), pal::textOnClip, Align::Center);
}


} // namespace lat
