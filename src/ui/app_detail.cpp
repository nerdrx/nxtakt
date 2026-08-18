// Clip detail: waveform, the detail-panel tab header, and the CLIP tab that
// hosts the PianoRoll. Moved verbatim from app.cpp.
//
#include "app.h"
#include "app_internal.h"
#include "arrange.h"
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
// what a clip may automate
//
// The roll must not see a DeviceModel or a ParamInfo (docs/AUTOMATION.md §6.5),
// so this flattens the selected track's mixer fields and device parameters into
// strings and floats once a frame. It is a few dozen string builds for the one
// track whose clip is on screen — the same order of cost as the panel's own
// labels, and it means the lane's chooser can never name a target the publisher
// would refuse.
//
// Deliberately NOT offered: mute, solo and arm. They are in the address grammar
// and the capture path spells them, but AutoTarget has no case for them yet, so
// a lane built from one would draw and never sound. Offering a control that
// cannot work is worse than not offering it.
// ---------------------------------------------------------------------------

void App::buildAutoTargets(int track, const ClipModel& clip, AutoTargets& out) const {
    out.entries.clear();
    out.inert = 0;
    out.inertWhy.clear();
    if (track < 0 || track >= (int)ses_.tracks.size()) return;
    const TrackModel& t = ses_.tracks[track];

    auto add = [&](const char* group, const char* label, std::string address,
                   const char* unit, f32 lo, f32 hi, f32 def) {
        AutoTargets::Entry e;
        e.group = group;
        e.label = label;
        e.address = std::move(address);
        e.unit = unit;
        e.lo = lo; e.hi = hi; e.def = def;
        for (const AutoLane& l : clip.envelopes)
            if (l.address == e.address) { e.automated = true; break; }
        out.entries.push_back(std::move(e));
    };

    // The mixer, in the order the strip has it. The fader's value is its 0..1
    // POSITION and not a gain -- §2.3, and why AutoXform exists.
    add("Track", "Volume", addr::trackField(t.uid, "vol"), "", 0.f, 1.f, 0.85f);
    add("Track", "Pan",    addr::trackField(t.uid, "pan"), "", -1.f, 1.f, 0.f);
    for (int i = 0; i < kMaxReturns; ++i) {
        char lbl[16];
        snprintf(lbl, sizeof lbl, "Send %s", kReturnLetter[i]);
        add("Track", lbl, addr::trackSend(t.uid, i), "", 0.f, 1.f, 0.f);
    }
    // Every parameter of every loaded device on this track. A device whose
    // plugin is missing contributes nothing -- it has no ParamInfo to describe a
    // range with -- but any lane already naming it keeps its points and says so
    // in the lane instead.
    for (const DeviceModel& d : t.devices) {
        if (!d.inst) continue;
        const int n = d.inst->paramCount();
        for (int p = 0; p < n; ++p) {
            const ParamInfo& info = d.inst->paramInfo(p);
            add(d.desc.name.c_str(), info.name.c_str(),
                addr::deviceParam(t.uid, d.uid, info.id), info.unit.c_str(),
                info.min, info.max, info.def);
        }
    }

    // Which of this clip's lanes the engine has given up on (§3.4). The mask is
    // by MODEL lane index, which is what the roll draws; inertAutos_ is keyed by
    // address, which is what survives a lane being reordered.
    for (size_t i = 0; i < clip.envelopes.size() && i < 32; ++i)
        if (autoLaneInert(clip.uid, clip.envelopes[i].address)) out.inert |= 1u << (u32)i;
    if (out.inert) out.inertWhy = "inert - this device has no realtime parameter path";
}

// ---------------------------------------------------------------------------
// clip detail
// ---------------------------------------------------------------------------

void App::drawWaveform(const Rect& r, const SampleBuffer& sb, const Col& c, f64 t0, f64 t1) {
    if (sb.peakBuckets <= 0) return;
    const f32 mid = r.cy();
    const f32 halfH = r.h * 0.5f - 1.f;
    const int cols = (int)r.w;
    for (int i = 0; i < cols; ++i) {
        const f64 u = t0 + (t1 - t0) * ((f64)i / std::max(1, cols - 1));
        const int b = clampv((int)(u * sb.peakBuckets), 0, sb.peakBuckets - 1);
        const f32 lo = sb.peaks[(size_t)b * 2 + 0];
        const f32 hi = sb.peaks[(size_t)b * 2 + 1];
        const f32 y0 = mid - hi * halfH;
        const f32 y1 = mid - lo * halfH;
        rend_.rect({r.x + i, y0, 1.f, std::max(1.f, y1 - y0)}, c);
    }
}

// The panel chrome: a Live-style tab strip along the top, then whichever view
// the tab selects. Ctrl+D still hides the panel as a whole.
void App::drawDetailPanel(const Rect& r) {
    const f32 s = win_.dpiScale();
    // The panel is a docked band of chrome, so it takes the bar fill and a
    // hairline where the solid rule used to be (§11). Its CONTENTS are what
    // carry the tier language from here: the plugin browser recesses into it,
    // the device boxes float on it as cards.
    rend_.gradRect(r, 0.f, nx::glassBar);
    rend_.hairlineH(r.x, r.right(), r.y);

    Rect head{r.x, r.y + 1 * s, r.w, 19 * s};
    rend_.hairlineH(head.x, head.right(), head.bottom());

    // §5's tab pill: ONE indicator sliding between two equal slots on
    // --ease-spring, not two backgrounds toggling. The enum is the model; the
    // pill speaks in indices, so the conversion happens here and nowhere else.
    const f32 tabW = 62 * s, tabH = 17 * s;
    Rect tabs{head.x + 6 * s, head.y + (head.h - tabH) * 0.5f, tabW * 2.f + 6 * s, tabH};
    static const char* const kTabs[2] = {"CLIP", "DEVICES"};
    int tab = detailTab_ == DetailTab::Clip ? 0 : 1;
    if (ui_.tabPill(uiId(9, 0), tabs, kTabs, 2, &tab)) {
        detailTab_ = tab == 0 ? DetailTab::Clip : DetailTab::Devices;
        if (detailTab_ == DetailTab::Devices) ensurePluginScan();
    }

    // Context label on the right of the tab strip, so the panel says what it is
    // looking at even when the content area is empty.
    {
        char buf[128];
        if (detailTab_ == DetailTab::Clip && view_ == MainView::Arrangement) {
            // In Arrangement view the CLIP tab is about the selected ITEM, so
            // the context label says where on the timeline it is rather than
            // which scene it is in -- it is not in one.
            const ArrangeClip* it = selectedArrItem();
            // Through Session::barOfBeat, not `start / sigNum`: dividing by the
            // bar-0 numerator is only right while a set never changes
            // signature, and from the first change on it prints a wrong bar
            // silently and plausibly. barOfBeat forwards to the same function
            // the engine's metronome and the ruler use, so this label cannot
            // disagree with the grid it is describing.
            if (it) snprintf(buf, sizeof buf, "%s  -  bar %.2f",
                             it->src.name.c_str(), ses_.barOfBeat(it->start) + 1.0);
            else    snprintf(buf, sizeof buf, "No item selected");
        } else if (detailTab_ == DetailTab::Clip) {
            const ClipModel& m = ses_.tracks[selTrack_].slots[selSlot_];
            // ASCII only: the glyph atlas has no dashes or middots.
            // Sentence case (§9), and the same words the empty panel below uses:
            // the label used to say "no clip" while the panel said "No clip
            // selected", which is two spellings of one state on one screen.
            snprintf(buf, sizeof buf, "%s  -  scene %d",
                     m.valid() ? m.name.c_str() : "No clip selected",
                     selSlot_ + 1);
        } else {
            ChainOwner co = chainOwner(devOwner_);
            const size_t n = co.devices ? co.devices->size() : 0;
            snprintf(buf, sizeof buf, "%s  -  %zu device%s", ownerName(devOwner_).c_str(),
                     n, n == 1 ? "" : "s");
        }
        rend_.textIn(fSmall_, head, buf, nx::muted.alpha(0.8f), Align::Right, 8 * s);
    }

    Rect content{r.x, head.bottom(), r.w, r.bottom() - head.bottom()};
    if (detailTab_ != DetailTab::Clip)          drawDeviceDetail(content);
    else if (view_ == MainView::Arrangement)    drawArrangeClipDetail(content);
    else                                        drawClipDetail(content);
}

// ---------------------------------------------------------------------------
// the CLIP tab in Arrangement view (docs/ARRANGEMENT.md §7.6)
//
// The same editor on a different clip, and that is the whole of it: the roll
// edits ArrangeClip::src IN PLACE and knows nothing about the arrangement.
//
// This is Rule 1 paying for itself. Because `src` is BY VALUE, the roll editing
// "the clip" edits precisely the one item the user selected, with no
// possibility of the edit leaking to another placement of the same material and
// no code in the roll that knows an arrangement exists.
//
// What the controls column shows is deliberately NOT drawClipDetail's. Launch
// quantum, probability and follow action are statements about how a clip is
// LAUNCHED, and an item on the timeline is not launched -- it is placed. So the
// column shows the placement instead: where the item starts, how long it is,
// which beat of its clip it begins on, and its two fades.
// ---------------------------------------------------------------------------

void App::drawArrangeClipDetail(const Rect& r) {
    const f32 s = win_.dpiScale();
    ArrangeClip* const it = selectedArrItem();
    if (!it || !it->src.valid()) {
        // §5: one short bold line, one muted sentence, centred, and room around
        // them. §9: it invites the next action instead of apologising.
        const f32 lh = fBody_.height();
        rend_.textIn(fBold_, {r.x, r.cy() - lh - nx::sp1 * s, r.w, lh},
                     "No item selected", nx::text, Align::Center);
        rend_.textIn(fBody_, {r.x, r.cy() + nx::sp1 * s, r.w, lh},
                     "Click a clip on the timeline, or drag one onto it.",
                     nx::muted, Align::Center);
        return;
    }
    ClipModel& m = it->src;
    const int track = arrSelTrack_;

    const Col ccol = pal::clipColors[m.colorIdx % pal::clipColorCount];
    Rect head{r.x, r.y + 1 * s, r.w, 20 * s};
    rend_.rect({head.x, head.y + 3 * s, std::max(1.f, nx::snapPx(3 * s)), head.h - 6 * s}, ccol);
    rend_.textIn(fBold_, {head.x + 10 * s, head.y, 260 * s, head.h}, m.name.c_str(),
                 nx::text, Align::Left, 0);
    rend_.hairlineH(r.x + nx::sp2 * s, r.right() - nx::sp2 * s, head.bottom());

    const f32 panelW = 250 * s;
    Rect ctrl{r.x + 8 * s, head.bottom() + 6 * s, panelW, r.bottom() - head.bottom() - 12 * s};
    f32 y = ctrl.y;
    const f32 rowH = 20 * s, lblW = 62 * s;
    // Field labels are §5 micro-labels: 10px, uppercase, wide tracking -- and
    // they sit on the VALUE's baseline, not on their own centre. See
    // baselineRow(): the label and the widget beside it are two different fonts
    // in one row, and centring each of them independently left the small one a
    // pixel high all the way down the column.
    auto label = [&](const char* tx, const Rect& row) {
        ui_.microIn(fSmall_, baselineRow({row.x, row.y, lblW, row.h}, fSmall_, fBody_),
                    tx, nx::muted, Align::Left, 0);
    };
    // Every one of these is a placement field, so every one of them goes through
    // the same commit: repair the lane, republish that track, one undo entry per
    // drag (the widget's own id is the gesture).
    bool placed = false;
    auto num = [&](int id, const char* lbl, f64* v, f64 lo, f64 hi, const char* fmt) {
        Rect row{ctrl.x, y, ctrl.w, rowH};
        label(lbl, row);
        f64 tmp = *v;
        Rect dn{row.x + lblW, row.y, 90 * s, row.h};
        if (ui_.dragNumber(uiId(UiDetailPlacement, id), dn, &tmp, lo, hi, 0.02, fmt)) {
            undoPoint("clip placement");
            *v = tmp;
            placed = true;
        }
        y += rowH + 4 * s;
    };
    num(0, "START",  &it->start,  0.0, 1e6, "%.3f bt");
    num(1, "LENGTH", &it->length, kMinArrBeats, 1e6, "%.3f bt");
    num(2, "OFFSET", &it->offset, 0.0, 1e6, "%.3f bt");
    num(3, "FADE IN",  &it->fadeIn,  0.0, kMaxOverlapBeats, "%.3f bt");
    num(4, "FADE OUT", &it->fadeOut, 0.0, kMaxOverlapBeats, "%.3f bt");
    {   // Gain and loop, which mean the same thing here as anywhere.
        Rect row{ctrl.x, y, ctrl.w, rowH};
        label("GAIN", row);
        f64 db = gainToDb(m.gain);
        Rect dn{row.x + lblW, row.y, 70 * s, row.h};
        if (ui_.dragNumber(uiId(UiDetailPlacement, 5), dn, &db, -70.0, 12.0, 0.1, "%.1f dB",
                           Align::Center, nullptr, 0.0, /*def=*/0.0)) {
            undoPoint("clip gain");
            m.gain = dbToGain((f32)db);
            placed = true;
        }
        Rect lp{dn.right() + 6 * s, row.y, 52 * s, row.h};
        if (ui_.button(uiId(UiDetailPlacement, 6), lp, "LOOP", m.loop, nx::violet)) {
            undoPoint("clip loop");
            m.loop = !m.loop;
            placed = true;
        }
        y += rowH + 4 * s;
    }
    if (ui_.fSmall) {
        Rect row{ctrl.x, y, ctrl.w, rowH};
        char buf[128];
        snprintf(buf, sizeof buf, "%s  -  %.2f .. %.2f bt", ownerName(track).c_str(),
                 it->start, it->end());
        rend_.textIn(fSmall_, row, buf, nx::muted.alpha(0.8f), Align::Left, 0);
    }

    Rect wave{ctrl.right() + 12 * s, head.bottom() + 6 * s,
              r.right() - ctrl.right() - 20 * s, r.bottom() - head.bottom() - 12 * s};

    if (!arrRoll_) arrRoll_ = std::make_unique<PianoRoll>();
    AutoTargets targets;
    buildAutoTargets(track, m, targets);

    // Where the playhead is INSIDE this item, so the roll's line means the same
    // thing it means for a session clip. Only while the transport is inside the
    // item at all -- an item that is not sounding has no phase to show.
    const f64 beat = es_.beat;
    const bool inside = es_.playing && beat >= it->start && beat < it->end();
    const f64 phase = inside ? (it->offset + (beat - it->start)) : 0.0;

    const ClipModel before = m;
    if (arrRoll_->draw(ui_, wave, m, targets, ses_.scale, phase, inside)) {
        undoPointWith(arrRoll_->lastEdit(), m, before);
        placed = true;
    }
    if (placed) {
        // arrangeRepair after EVERY mutation, which is the rule the whole model
        // rests on: a start or a length typed into this panel can overlap a
        // neighbour exactly as a drag can.
        if (track >= 0 && track < (int)ses_.tracks.size()) {
            arrangeRepair(ses_.tracks[(size_t)track].arrange);
            publishArrangementFor(track);
        }
    }
    u8 pv[PianoRoll::kPreviewMax];
    const int np = arrRoll_->drainPreview(pv, PianoRoll::kPreviewMax);
    for (int i = 0; i < np; ++i) startPreview((int)pv[i], m.uid);
}

void App::drawClipDetail(const Rect& r) {
    const f32 s = win_.dpiScale();

    // Headless verification hook (see app.h): NXTAKT_DEBUG_AUTOLANE=<track>
    // selects that track's first clip, seeds one volume envelope on it and puts
    // the lane on that envelope, once per run. Nothing inside gamescope can
    // work a chooser or drag a breakpoint, so this is what lets a screenshot
    // check that the lane draws real points against the roll's own time axis.
    if (!autoDebugSeeded_) {
        if (const char* want = env("DEBUG_AUTOLANE")) {
            autoDebugSeeded_ = true;
            int t = 0, laneNo = 1;
            sscanf(want, "%d:%d", &t, &laneNo);       // "<track>[:<lane>]"
            if (t > 0 && t < (int)ses_.tracks.size()) { selTrack_ = t; selSlot_ = 0; }
            ClipModel& c = ses_.tracks[selTrack_].slots[selSlot_];
            if (c.valid()) {
                if (c.envelopes.empty()) {
                    AutoLane l;
                    l.address = addr::trackField(ses_.tracks[selTrack_].uid, "vol");
                    const f64 len = c.lengthBeats > 0.0 ? c.lengthBeats : 4.0;
                    l.points.push_back(AutoPoint{0.0,         0.30f, 0, {}});
                    l.points.push_back(AutoPoint{len * 0.25,  0.95f, 0, {}});
                    l.points.push_back(AutoPoint{len * 0.5,   0.55f, 0, {}});
                    l.points.push_back(AutoPoint{len * 0.875, 0.85f, 0, {}});
                    c.envelopes.push_back(std::move(l));
                    pushClip(selTrack_, selSlot_);
                    status_ = "NXTAKT_DEBUG_AUTOLANE: seeded a volume envelope";
                } else {
                    status_ = "NXTAKT_DEBUG_AUTOLANE: showing lane " + std::to_string(laneNo);
                }
                if (!roll_) roll_ = std::make_unique<PianoRoll>();
                // laneNo counts ENVELOPES from 1, which is what it meant before
                // the chooser grew the per-note lanes in front of them; the
                // offset is asked for rather than assumed so this hook does not
                // have to know how many of those there are.
                const int base = PianoRoll::envLaneBase(c);
                roll_->showLane(clampv(base + laneNo - 1, 0,
                                       base + (int)c.envelopes.size() - 1));
            }
        }
    }

    // Headless verification hooks for the piano-roll wave, in the same shape as
    // the one above: nothing inside gamescope can work a selector, cycle a fold
    // or drag a stem, so a screenshot can only check a scale, a fold-to-key and
    // a note with a chance on it if something sets them up once per run.
    //
    //   NXTAKT_DEBUG_SCALE=<root>:<mode>[:<fold>][:<snap>]
    //       puts the session in that key -- root 0..11, mode indexing kScales --
    //       and optionally sets the roll's fold (0 ALL, 1 FOLD, 2 KEY) and the
    //       snap flag. Selects the first MIDI clip it can find, so the shot is
    //       of a pattern rather than of a waveform.
    //   NXTAKT_DEBUG_CHANCE=<pct>[:<velRange>]
    //       stamps that chance (and optionally that velocity range) onto every
    //       other note of the selected clip, so one screenshot shows both the
    //       plain notes and the uncertain ones side by side.
    //
    // Both are one-shot and inert without the variable, which is what keeps them
    // out of the way of an actual session.
    static bool scaleDebugSeeded = false;
    if (!scaleDebugSeeded) {
        const char* wantScale  = env("DEBUG_SCALE");
        const char* wantChance = env("DEBUG_CHANCE");
        if (wantScale || wantChance || env("DEBUG_NOTETOOL")) {
            scaleDebugSeeded = true;
            // The first MIDI clip in the set. A scale over a waveform is a
            // screenshot of nothing.
            for (size_t t = 0; t < ses_.tracks.size(); ++t)
                for (int sl = 0; sl < kMaxScenes; ++sl)
                    if (ses_.tracks[t].slots[sl].kind == ClipKind::Midi &&
                        ses_.tracks[t].slots[sl].valid()) {
                        selTrack_ = (int)t; selSlot_ = sl;
                        t = ses_.tracks.size();     // break both loops
                        break;
                    }
            if (!roll_) roll_ = std::make_unique<PianoRoll>();
            if (wantScale) {
                int root = 0, mode = 0, fold = 0, snap = 0;
                sscanf(wantScale, "%d:%d:%d:%d", &root, &mode, &fold, &snap);
                ses_.scale.root = clScaleRoot(root);
                ses_.scale.mode = clScaleMode(mode);
                ses_.scale.snap = snap != 0;
                roll_->setFoldMode((FoldMode)clampv(fold, 0, kFoldModeCount - 1));
                status_ = "NXTAKT_DEBUG_SCALE: " + ses_.scale.label();
            }
            if (const char* tool = env("DEBUG_NOTETOOL")) {
                // NXTAKT_DEBUG_NOTETOOL=<legato|quantize|dup|up|down>[:<amount>]
                // runs one note tool over the whole clip, since nothing inside
                // gamescope can press a button. `amount` is the quantize
                // strength in percent for `quantize` and the semitone count for
                // `up`/`down`.
                char verb[32] = {};
                int amount = 100;
                std::sscanf(tool, "%31[^:]:%d", verb, &amount);
                ClipModel& c = ses_.tracks[selTrack_].slots[selSlot_];
                bool did = false;
                if (!std::strcmp(verb, "legato"))        did = roll_->legatoSelected(c);
                else if (!std::strcmp(verb, "quantize")) {
                    roll_->setQuantGrid(0.5);
                    roll_->setQuantStrength((f32)clampv(amount, 0, 100) * 0.01f);
                    did = roll_->quantizeSelected(c);
                } else if (!std::strcmp(verb, "dup"))    did = roll_->duplicateSelected(c);
                else if (!std::strcmp(verb, "up"))       did = roll_->transposeSelected(c, amount);
                else if (!std::strcmp(verb, "down"))     did = roll_->transposeSelected(c, -amount);
                if (did) pushClip(selTrack_, selSlot_);
                status_ = std::string("NXTAKT_DEBUG_NOTETOOL: ") + verb +
                          (did ? " applied" : " changed nothing");
            }
            if (wantChance) {
                int pct = 50, range = 0;
                sscanf(wantChance, "%d:%d", &pct, &range);
                ClipModel& c = ses_.tracks[selTrack_].slots[selSlot_];
                for (size_t i = 1; i < c.notes.size(); i += 2) {
                    c.notes[i].chance = (u8)clampv(pct, 0, 100);
                    c.notes[i].velTo  = (u8)clampv(range, 0, 127);
                }
                // The lane is put on CHANCE so the stems are visible too; the
                // grid alone would show the fade but not the number.
                roll_->showLane((int)NoteLane::Chance);
                pushClip(selTrack_, selSlot_);
                status_ = "NXTAKT_DEBUG_CHANCE: " + std::to_string(pct) + "% on every other note";
            }
        }
    }

    ClipModel& m = ses_.tracks[selTrack_].slots[selSlot_];
    if (!m.valid()) {
        // §5's empty state: one short bold line, one muted sentence, centred,
        // with the whitespace it needs. ASCII only -- the em dash this line
        // used to carry is not in the glyph atlas and rendered as a hole, which
        // is exactly the trap the specimen sheet's own labels warn about.
        const f32 lh = fBody_.height();
        rend_.textIn(fBold_, {r.x, r.cy() - lh - nx::sp1 * s, r.w, lh},
                     "No clip selected", nx::text, Align::Center);
        rend_.textIn(fBody_, {r.x, r.cy() + nx::sp1 * s, r.w, lh},
                     "Drag a file from the browser onto a slot, or double-click "
                     "an empty slot on an instrument track.",
                     nx::muted, Align::Center);
        return;
    }
    // A pattern has no sample behind it, so warp, clip tempo and the loop
    // *range* have nothing to act on; everything else on this panel is about
    // launching, which a MIDI clip does exactly like an audio one.
    const bool midi = m.kind == ClipKind::Midi;

    const Col ccol = pal::clipColors[m.colorIdx % pal::clipColorCount];
    Rect head{r.x, r.y + 1 * s, r.w, 20 * s};
    rend_.rect({head.x, head.y + 3 * s, std::max(1.f, nx::snapPx(3 * s)), head.h - 6 * s}, ccol);
    const f32 nameW = std::min(260 * s, fBold_.measure(m.name.c_str()) + 4 * s);
    rend_.textIn(fBold_, {head.x + 10 * s, head.y, nameW, head.h}, m.name.c_str(),
                 nx::text, Align::Left, 0);
    // What kind of material this is, as a status chip (§5) rather than as one
    // more line of prose. Cyan = a pattern, which is the one the editor to the
    // right behaves differently for.
    ui_.chip({head.x + 14 * s + nameW, head.cy() - 6 * s, 44 * s, 13 * s},
             midi ? "midi" : "audio", midi ? nx::cyan : nx::muted);
    rend_.hairlineH(r.x + nx::sp2 * s, r.right() - nx::sp2 * s, head.bottom());

    // --- controls column ---
    const f32 panelW = 250 * s;
    Rect ctrl{r.x + 8 * s, head.bottom() + 6 * s, panelW, r.bottom() - head.bottom() - 12 * s};
    f32 y = ctrl.y;
    const f32 rowH = 20 * s, lblW = 62 * s;

    // A SECOND column, for a pattern only. The panel is a fixed height and the
    // first column was already using all of it; the key and the note tools are
    // three more rows and would simply have fallen off the bottom of it.
    // Widthways there is room to spare -- the roll takes everything to the right
    // of this and is still the widest thing on screen -- so the EDITOR's
    // controls go beside the CLIP's rather than under them, which also happens
    // to be the right grouping: one column is about how this clip launches and
    // sounds, the other is about how notes get written into it. An audio clip
    // has neither and gets the width back.
    Rect ctrl2{ctrl.right() + 10 * s, ctrl.y, panelW, ctrl.h};
    f32 y2 = ctrl2.y;

    // Field labels are §5 micro-labels: 10px, uppercase, wide tracking, sitting
    // on the value column's baseline rather than on their own centre
    // (baselineRow, and the same reason as the arrangement panel's).
    auto label = [&](const char* t, const Rect& row) {
        ui_.microIn(fSmall_, baselineRow({row.x, row.y, lblW, row.h}, fSmall_, fBody_),
                    t, nx::muted, Align::Left, 0);
    };

    {   // Warp mode (audio only) + loop, which both kinds have
        Rect row{ctrl.x, y, ctrl.w, rowH};
        Rect lp{row.x + lblW, row.y, 52 * s, row.h};
        if (!midi) {
            label("WARP", row);
            static const char* warpNames[] = {"Off", "Repitch", "Beats"};
            int wi = (int)m.warp;
            Rect sel{row.x + lblW, row.y, 84 * s, row.h};
            if (ui_.selector(uiId(8, 0), sel, &wi, warpNames, 3)) {
                undoPoint("warp mode");
                m.warp = (Warp)wi;
                send(Cmd::ClipWarp, selTrack_, selSlot_, (f64)wi);
            }
            lp = {sel.right() + 6 * s, row.y, 52 * s, row.h};
        } else {
            label("PLAY", row);
        }
        if (ui_.button(uiId(8, 1), lp, "LOOP", m.loop, nx::violet)) {
            undoPoint("clip loop");
            m.loop = !m.loop;
            send(Cmd::ClipLoop, selTrack_, selSlot_, m.loop ? 1.0 : 0.0);
        }
        y += rowH + 4 * s;
    }
    if (!midi) {   // Clip tempo
        Rect row{ctrl.x, y, ctrl.w, rowH};
        label("CLIP BPM", row);
        f64 bpm = m.clipBpm;
        Rect dn{row.x + lblW, row.y, 70 * s, row.h};
        if (ui_.dragNumber(uiId(8, 2), dn, &bpm, 20.0, 400.0, 0.1, "%.2f")) {
            undoPoint("clip tempo");
            m.clipBpm = bpm;
            pushClip(selTrack_, selSlot_);
        }
        // Halve / double, exactly like Live's :2 and *2 buttons -- and one
        // cluster, because they are one control with two directions.
        Rect h2{dn.right() + 6 * s, row.y, 26 * s, row.h};
        Rect d2{h2.right(), row.y, 26 * s, row.h};
        ui_.segCluster({h2.x, h2.y, d2.right() - h2.x, h2.h});
        rend_.hairlineV(d2.x, h2.y + 3 * s, h2.bottom() - 3 * s);
        if (ui_.segButton(uiId(8, 3), h2, false, nx::violet)) {
            undoPoint("clip tempo");
            m.clipBpm *= 0.5;
            pushClip(selTrack_, selSlot_);
        }
        ui_.microIn(fSmall_, ui_.lastRect, ":2", nx::muted, Align::Center);
        if (ui_.segButton(uiId(8, 4), d2, false, nx::violet)) {
            undoPoint("clip tempo");
            m.clipBpm *= 2.0;
            pushClip(selTrack_, selSlot_);
        }
        ui_.microIn(fSmall_, ui_.lastRect, "*2", nx::muted, Align::Center);
        y += rowH + 4 * s;
    }
    {   // Gain
        Rect row{ctrl.x, y, ctrl.w, rowH};
        label("GAIN", row);
        f64 db = gainToDb(m.gain);
        Rect dn{row.x + lblW, row.y, 70 * s, row.h};
        if (ui_.dragNumber(uiId(8, 5), dn, &db, -70.0, 12.0, 0.1, "%.1f dB",
                           Align::Center, nullptr, 0.0, /*def=*/0.0)) {
            undoPoint("clip gain");
            m.gain = dbToGain((f32)db);
            send(Cmd::ClipGain, selTrack_, selSlot_, m.gain);
        }
        y += rowH + 4 * s;
    }
    {   // Launch quantum override
        Rect row{ctrl.x, y, ctrl.w, rowH};
        label("LAUNCH Q", row);
        static const char* qn[kQuantumCount + 1] = {"Global"};
        static bool qnInit = false;
        if (!qnInit) { for (int i = 0; i < kQuantumCount; ++i) qn[i + 1] = kQuantumNames[i]; qnInit = true; }
        int qi = m.quantumIdx + 1;
        Rect sel{row.x + lblW, row.y, 84 * s, row.h};
        if (ui_.selector(uiId(8, 6), sel, &qi, qn, kQuantumCount + 1)) {
            undoPoint("clip quantum");
            m.quantumIdx = qi - 1;
            pushClip(selTrack_, selSlot_);
        }
        y += rowH + 4 * s;
    }
    {   // Generative launch: probability, follow action, follow length.
        // The engine rolls `prob` on every launch and fires the follow action
        // after `followBeats` of playback, so all three are pure clip state and
        // ride across in the same RtClip as everything else here.
        Rect row{ctrl.x, y, ctrl.w, rowH};
        label("LAUNCH", row);

        f64 pct = m.prob * 100.0;
        Rect pr{row.x + lblW, row.y, 48 * s, row.h};
        if (ui_.dragNumber(uiId(UiDetailLaunch, 0), pr, &pct, 0.0, 100.0, 0.4, "%.0f%%",
                           Align::Center, nullptr, 0.0, /*def=*/100.0)) {
            undoPoint("launch probability");
            m.prob = clampv(pct * 0.01, 0.0, 1.0);
            pushClip(selTrack_, selSlot_);
        }

        int fa = (int)m.followAction;
        Rect fr{pr.right() + 6 * s, row.y, 58 * s, row.h};
        if (ui_.selector(uiId(UiDetailLaunch, 1), fr, &fa, kFollowNames, kFollowCount)) {
            undoPoint("follow action");
            m.followAction = (Follow)clampv(fa, 0, kFollowCount - 1);
            pushClip(selTrack_, selSlot_);
        }

        // 0 beats means "when the clip itself ends", which reads as Auto rather
        // than as a length. Whole beats only: a follow length between beats is
        // a tempo problem, not a musical choice.
        f64 fb = m.followBeats;
        Rect br{fr.right() + 6 * s, row.y, 52 * s, row.h};
        if (ui_.dragNumber(uiId(UiDetailLaunch, 2), br, &fb, 0.0, 128.0, 0.06, "%.0f bt",
                           Align::Center, "Auto", 1.0)) {
            undoPoint("follow length");
            m.followBeats = fb;
            pushClip(selTrack_, selSlot_);
        }
        y += rowH + 4 * s;
    }
    if (midi) {   // The set's KEY: root, scale, and whether edits are held to it.
        // On the clip panel rather than on the control bar, which is where Live
        // puts it, for a reason that is about this program and not about taste:
        // the control bar here is already at the width it can carry and the only
        // thing that consumes a key is the editor twelve pixels to the right of
        // these three widgets. Putting them together means the highlight changes
        // under the eye that is choosing it.
        //
        // It is still the SESSION's key and not the clip's -- every clip in the
        // set sees the same change -- which is why the row is labelled KEY and
        // not, say, CLIP KEY, and why it goes through undoPoint like any other
        // session edit.
        Rect row{ctrl2.x, y2, ctrl2.w, rowH};
        label("KEY", row);
        static const char* rootNames[12] = {};
        static bool rootInit = false;
        if (!rootInit) { for (int i = 0; i < 12; ++i) rootNames[i] = kPitchNames[i]; rootInit = true; }
        static const char* scaleNames[kScaleCount] = {};
        static bool scaleInit = false;
        if (!scaleInit) { for (int i = 0; i < kScaleCount; ++i) scaleNames[i] = kScales[i].name; scaleInit = true; }

        int root = clScaleRoot(ses_.scale.root);
        Rect rr_{row.x + lblW, row.y, 34 * s, row.h};
        if (ui_.selector(uiId(UiDetailKeyRow, 0), rr_, &root, rootNames, 12)) {
            undoPoint("key");
            ses_.scale.root = clScaleRoot(root);
        }
        int mode = clScaleMode(ses_.scale.mode);
        Rect sr{rr_.right() + 6 * s, row.y, 90 * s, row.h};
        if (ui_.selector(uiId(UiDetailKeyRow, 1), sr, &mode, scaleNames, kScaleCount)) {
            undoPoint("scale");
            ses_.scale.mode = clScaleMode(mode);
        }
        Rect nr{sr.right() + 6 * s, row.y, 44 * s, row.h};
        if (ui_.button(uiId(UiDetailKeyRow, 2), nr, "SNAP", ses_.scale.snap, nx::violet)) {
            undoPoint("scale snap");
            ses_.scale.snap = !ses_.scale.snap;
        }
        if (ui_.hovered(nr))
            ui_.tip = ses_.scale.active()
                          ? (ses_.scale.snap ? "Edits are pulled onto the nearest note of "
                                               + ses_.scale.label()
                                             : "Click to pull every edit onto the nearest note of "
                                               + ses_.scale.label())
                          : std::string("Pick a scale first - there is nothing to snap to in "
                                        "Chromatic");
        y2 += rowH + 4 * s;
    }
    if (midi && roll_) {
        // The note tools (PianoRoll's quantize / legato / duplicate / transpose).
        // Buttons rather than shortcuts: the roll's key map is already full, and
        // these are deliberate one-shot gestures rather than things a hand does
        // while it is holding a note.
        //
        // Every one of them acts on the roll's selection when there is one and
        // on the whole clip when there is not, which is why the row is labelled
        // for what it does and not for what it does it to.
        {
            Rect row{ctrl2.x, y2, ctrl2.w, rowH};
            label("QUANTIZE", row);
            // The grid, as a selector over the divisions a sequencer actually
            // uses. Triplets are in the list because a swung part cannot be
            // quantized by a straight grid at any strength.
            static const char* gridNames[] = {"1/4", "1/8T", "1/8", "1/16T", "1/16", "1/32"};
            static const f64   gridBeats[] = {1.0, 1.0 / 3.0, 0.5, 1.0 / 6.0, 0.25, 0.125};
            constexpr int kGridCount = 6;
            int gi = 4;
            for (int i = 0; i < kGridCount; ++i)
                if (std::fabs(gridBeats[i] - roll_->quantGrid()) < 1e-9) { gi = i; break; }
            Rect gr{row.x + lblW, row.y, 52 * s, row.h};
            if (ui_.selector(uiId(UiDetailNotes, 0), gr, &gi, gridNames, kGridCount))
                roll_->setQuantGrid(gridBeats[clampv(gi, 0, kGridCount - 1)]);

            // Strength. Live's "Amount": 100% snaps hard, anything less keeps
            // the part of a performance's timing that makes it one.
            f64 amt = (f64)roll_->quantStrength() * 100.0;
            Rect ar{gr.right() + 6 * s, row.y, 48 * s, row.h};
            if (ui_.dragNumber(uiId(UiDetailNotes, 1), ar, &amt, 0.0, 100.0, 0.4, "%.0f%%",
                               Align::Center, nullptr, 0.0, /*def=*/0.0))
                roll_->setQuantStrength((f32)(amt * 0.01));

            Rect qb{ar.right() + 6 * s, row.y, 44 * s, row.h};
            if (ui_.button(uiId(UiDetailNotes, 2), qb, "APPLY")) {
                const ClipModel was = m;
                if (roll_->quantizeSelected(m)) {
                    undoPointWith("quantize", m, was);
                    pushClip(selTrack_, selSlot_);
                }
            }
            if (ui_.hovered(qb))
                ui_.tip = roll_->hasSelection(m) ? "Quantize the selected notes"
                                                 : "Quantize every note in the clip";
            y2 += rowH + 4 * s;
        }
        {
            Rect row{ctrl2.x, y2, ctrl2.w, rowH};
            label("NOTES", row);
            Rect lg{row.x + lblW, row.y, 52 * s, row.h};
            Rect dp{lg.right() + 6 * s, row.y, 44 * s, row.h};
            // Down and up are one control with two directions, so they are one
            // cluster with a seam rather than two capsules in a gap.
            Rect dn{dp.right() + 6 * s, row.y, 22 * s, row.h};
            Rect up{dn.right(), row.y, 22 * s, row.h};
            ui_.segCluster({dn.x, dn.y, up.right() - dn.x, dn.h});
            rend_.hairlineV(up.x, dn.y + 3 * s, dn.bottom() - 3 * s);

            if (ui_.button(uiId(UiDetailNotes, 3), lg, "LEGATO")) {
                const ClipModel was = m;
                if (roll_->legatoSelected(m)) {
                    undoPointWith("legato", m, was);
                    pushClip(selTrack_, selSlot_);
                }
            }
            if (ui_.hovered(lg))
                ui_.tip = "Stretch each note to where the next one begins";
            if (ui_.button(uiId(UiDetailNotes, 4), dp, "DUP")) {
                const ClipModel was = m;
                if (roll_->duplicateSelected(m)) {
                    undoPointWith("duplicate notes", m, was);
                    pushClip(selTrack_, selSlot_);
                }
            }
            if (ui_.hovered(dp))
                ui_.tip = "Copy the notes one selection-width later (Ctrl+U doubles "
                          "the whole loop instead)";
            // Transpose by a semitone each way. An octave is Shift+Up/Down on the
            // keyboard already, so the buttons cover what the keyboard does not.
            if (ui_.segButton(uiId(UiDetailNotes, 5), dn, false, nx::violet)) {
                const ClipModel was = m;
                if (roll_->transposeSelected(m, -1)) {
                    undoPointWith("transpose", m, was);
                    pushClip(selTrack_, selSlot_);
                }
            }
            ui_.microIn(fSmall_, ui_.lastRect, "-", nx::muted, Align::Center);
            if (ui_.segButton(uiId(UiDetailNotes, 6), up, false, nx::violet)) {
                const ClipModel was = m;
                if (roll_->transposeSelected(m, 1)) {
                    undoPointWith("transpose", m, was);
                    pushClip(selTrack_, selSlot_);
                }
            }
            ui_.microIn(fSmall_, ui_.lastRect, "+", nx::muted, Align::Center);
            if (ui_.hovered(dn) || ui_.hovered(up))
                ui_.tip = ses_.scale.snap && ses_.scale.active()
                              ? "Transpose by one step of " + ses_.scale.label()
                              : std::string("Transpose by a semitone");
            y2 += rowH + 4 * s;
        }
    }
    {   // Read-out of what the engine will actually do
        Rect row{ctrl.x, y, ctrl.w, rowH};
        char buf[96];
        // ASCII ONLY. The glyph atlas is 32..126 (gfx/font.h), so the U+00B7
        // this line used to separate its fields with rendered as the invalid
        // glyph -- an invisible hole two bytes wide, which is why the read-out
        // came out as "8.00 beats    rate 0.984x    1 ch" with two ragged gaps
        // and no separator at all. The same "  -  " the context label above
        // uses, which is the spelling the rest of the panel already had.
        if (midi) {
            snprintf(buf, sizeof buf, "%.2f beats  -  %zu note%s", m.lengthBeats,
                     m.notes.size(), m.notes.size() == 1 ? "" : "s");
        } else {
            const f64 rate = (m.warp == Warp::Off) ? 1.0 : m.clipBpm / ses_.tempo;
            snprintf(buf, sizeof buf, "%.2f beats  -  rate %.3fx  -  %d ch",
                     m.lengthBeats, rate, m.sample->channels);
        }
        rend_.textIn(fSmall_, row, buf, nx::muted.alpha(0.8f), Align::Left, 0);
    }

    // --- the material: the note grid for a pattern, the waveform for a sample,
    //     and under either of them the automation lane -----------------------
    //
    // Both kinds go through the roll now, and the choice is worth stating: an
    // audio clip's envelopes need a TIME axis, and the only correct time axis
    // is the one its material is drawn against. Giving the audio clip its own
    // lane widget under the old fixed-width waveform would have meant two
    // beat<->pixel mappings that agree only while nothing is zoomed. So the
    // roll draws the waveform where the note grid goes (PianoRoll::draw,
    // `midiClip`), the lane keeps the axis it already shares with the ruler,
    // and there is exactly one mapping in the program. The note-specific
    // furniture (FOLD, the keyboard column, the loop-length drag) is suppressed
    // rather than faked.
    // The editor column only exists for a pattern, so an audio clip's waveform
    // starts where it always did.
    const f32 leftEdge = midi ? ctrl2.right() : ctrl.right();
    Rect wave{leftEdge + 12 * s, head.bottom() + 6 * s,
              r.right() - leftEdge - 20 * s, r.bottom() - head.bottom() - 12 * s};

    // Where the clip is, in its own beats, so the grid and the lane draw the
    // same playhead from the same number.
    const bool active = es_.activeSlot[selTrack_] == selSlot_;
    const f64  phase  = clampv(es_.clipPhase[selTrack_], 0.0, 1.0);

    if (!roll_) roll_ = std::make_unique<PianoRoll>();

    AutoTargets targets;
    buildAutoTargets(selTrack_, m, targets);

    // The roll edits m.notes and m.envelopes in place and says whether it
    // touched anything; republishing is ours, and pushClip is what retires the
    // arrays the engine is still reading from.
    //
    // The undo entry therefore needs the clip as it was *before* the call, which
    // is why the copy is taken unconditionally: whether an edit happens is not
    // knowable until draw() returns, and by then m already has it. A clip is a
    // note vector, an envelope vector, two strings and a shared pointer -- cheap
    // enough to copy once a frame, and it buys the one thing that matters here,
    // which is that a click that adds a note or a breakpoint can be undone. The
    // roll owns ui_.active for the length of a drag, so a note or a breakpoint
    // dragged across the editor leaves one entry and not one per frame.
    const ClipModel before = m;
    if (roll_->draw(ui_, wave, m, targets, ses_.scale,
                    active ? phase * m.lengthBeats : 0.0, active)) {
        undoPointWith(roll_->lastEdit(), m, before);
        pushClip(selTrack_, selSlot_);
    }
    // Auditioning is the caller's job: the roll only names the pitches that want
    // to be heard (from this draw, and from any keyboard edit earlier in the
    // frame — handleShortcuts runs first). See previews_ for why these reach the
    // right instrument.
    u8 pv[PianoRoll::kPreviewMax];
    const int np = roll_->drainPreview(pv, PianoRoll::kPreviewMax);
    for (int i = 0; i < np; ++i) startPreview((int)pv[i], m.uid);
}


} // namespace lat
