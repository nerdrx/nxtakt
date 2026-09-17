#include "drum_sequencer.h"
#include "drum_rhythm.h"
#include "autolane.h" // dpiOf
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace lat {
namespace {
constexpr int kLanes = 8, kSteps = 16;
constexpr int kPitches[kLanes] = {36, 38, 39, 42, 46, 45, 37, 56};
constexpr const char* kNames[kLanes] = {
    "Kick", "Snare", "Clap", "Closed hat", "Open hat", "Tom", "Rim", "Cowbell"
};
constexpr double kStepBeat = 0.25;
int stepOf(const NoteModel& n) { return (int)std::floor(n.beat / kStepBeat + 1e-7); }
bool drumPitch(int pitch) {
    for (int p : kPitches) if (p == pitch) return true;
    return false;
}
void addStep(ClipModel& clip, int lane, int step, int velocity = 100) {
    NoteModel note;
    note.pitch = (u8)kPitches[lane];
    note.beat = step * kStepBeat;
    note.len = std::min(kStepBeat, clip.lengthBeats - note.beat);
    note.vel = (u8)velocity;
    if (note.len > 0.0) clip.notes.push_back(note);
}
void sortNotes(ClipModel& clip) {
    std::stable_sort(clip.notes.begin(), clip.notes.end(), [](const NoteModel& a, const NoteModel& b) {
        return a.beat < b.beat;
    });
}
} // namespace

void DrumSequencer::preview(int pitch) {
    if (previewCount_ >= kLanes) return;
    for (int i = 0; i < previewCount_; ++i) if (previews_[i] == pitch) return;
    previews_[previewCount_++] = (u8)pitch;
}
int DrumSequencer::drainPreview(u8* out, int max) {
    const int count = std::min(std::max(0, max), previewCount_);
    for (int i = 0; i < count; ++i) out[i] = previews_[i];
    previewCount_ = 0;
    return count;
}

bool DrumSequencer::draw(Ui& ui, const Rect& r, ClipModel& clip,
                         double playheadBeats, bool playing) {
    if (!ui.r || !ui.in || clip.kind != ClipKind::Midi) return false;
    Renderer& rr = *ui.r;
    Input& in = *ui.in;
    const f32 s = std::max(0.5f, ui.r->dpiScale());
    if (r.w < 320 * s || r.h < (r.w < 600*s ? 190 : 154) * s) return false;
    if (clipUid_ != clip.uid) {
        clipUid_ = clip.uid;
        page_ = 0;
        scrollY_ = scrollX_ = 0.f;
        previewCount_ = 0;
        lengthChoice_ = clip.lengthBeats <= 4.0 ? 0 : clip.lengthBeats <= 8.0 ? 1 : 2;
    }
    bool changed = false;
    rr.pushClip(r);
    rr.roundRect(r, 8 * s, nx::bgTop);
    const Rect toolbar{r.x, r.y, r.w, 34 * s};
    rr.rect(toolbar, nx::panel);
    if (ui.fBold) rr.textIn(*ui.fBold, {r.x + 8 * s, r.y, 108 * s, toolbar.h},
                            "Drum steps", nx::text, Align::Left, 0);

    static const char* lengths[] = {"16 steps", "32 steps", "64 steps"};
    constexpr int lengthSteps[] = {16, 32, 64};
    Rect lengthBox{r.x + 116 * s, r.y + 3 * s, 86 * s, 28 * s};
    ui.selector(uiId(UiDrumSequencer, 0), lengthBox, &lengthChoice_, lengths, 3);
    const double chosenLength = lengthSteps[clampv(lengthChoice_, 0, 2)] * kStepBeat;
    char lengthLabel[32];
    std::snprintf(lengthLabel, sizeof lengthLabel, "%s %d", chosenLength < clip.lengthBeats ? "Trim to" : "Set to",
                  lengthSteps[clampv(lengthChoice_, 0, 2)]);
    Rect setLength{lengthBox.right() + 4 * s, lengthBox.y, 82 * s, lengthBox.h};
    if (ui.button(uiId(UiDrumSequencer, 1), setLength, lengthLabel) &&
        std::fabs(clip.lengthBeats - chosenLength) > 1e-7) {
        clip.lengthBeats = chosenLength;
        clip.notes.erase(std::remove_if(clip.notes.begin(), clip.notes.end(), [&](const NoteModel& n) {
            return n.beat >= chosenLength;
        }), clip.notes.end());
        for (auto& n : clip.notes) n.len = std::min(n.len, chosenLength - n.beat);
        changed = true;
        lastEdit_ = "drum pattern length";
    }
    if (ui.hovered(setLength)) ui.tip = "Set pattern length; trimming removes notes beyond the new end. Undo restores them.";

    const int totalSteps = std::max(1, (int)std::ceil(clip.lengthBeats / kStepBeat - 1e-7));
    const int pages = std::max(1, (totalSteps + kSteps - 1) / kSteps);
    page_ = clampv(page_, 0, pages - 1);
    // Page navigation moves the view only; transport continues independently.
    Rect next{r.right() - 34 * s, r.y + 3 * s, 28 * s, 28 * s};
    Rect pageLabel{next.x - 94 * s, next.y, 90 * s, 28 * s};
    Rect prev{pageLabel.x - 32 * s, next.y, 28 * s, 28 * s};
    const bool compactPaging = prev.x <= setLength.right() + 4 * s;
    if (compactPaging) {
        prev = {r.x + 4 * s, toolbar.bottom() + s, 24 * s, 24 * s};
        pageLabel = {prev.right(), prev.y, 48 * s, 24 * s};
        next = {pageLabel.right(), prev.y, 24 * s, 24 * s};
    }
    {
        if (ui.button(uiId(UiDrumSequencer, 2), prev, "<")) page_ = std::max(0, page_ - 1);
        if (ui.button(uiId(UiDrumSequencer, 3), next, ">")) page_ = std::min(pages - 1, page_ + 1);
        char label[40];
        std::snprintf(label, sizeof label, compactPaging ? "%d/%d" : "Bar %d / %d", page_ + 1, pages);
        if (ui.fSmall) rr.textIn(*ui.fSmall, pageLabel, label, nx::muted, Align::Center, 0);
    }

    const Rect footer{r.x, r.bottom() - 34 * s, r.w, 34 * s};
    const bool narrowRhythm = r.w < 600*s;
    const Rect rhythm{r.x,footer.y-(narrowRhythm?72:36)*s,r.w,(narrowRhythm?72:36)*s};
    const Rect heading{r.x, toolbar.bottom(), r.w, 26 * s};
    const Rect body{r.x, heading.bottom(), r.w, std::max(24 * s, rhythm.y - heading.bottom())};
    const f32 nameW = 108 * s;
    const Rect grid{body.x + nameW, body.y, body.w - nameW - 6 * s, body.h};
    const f32 rowH = std::max(24 * s, std::min(34 * s, body.h / kLanes));
    const f32 cellW = std::max(24 * s, grid.w / kSteps);
    const bool overBody = body.contains(in.mx, in.my) && rr.currentClip().contains(in.mx, in.my);
    if (overBody && in.wheel != 0 && !in.ctrl()) {
        if (in.shift()) scrollX_ -= in.wheel * cellW * 2;
        else scrollY_ -= in.wheel * rowH;
    }
    scrollY_ = clampv(scrollY_, 0.f, std::max(0.f, rowH * kLanes - body.h));
    scrollX_ = clampv(scrollX_, 0.f, std::max(0.f, cellW * kSteps - grid.w));
    rr.pushClip({grid.x, heading.y, grid.w, heading.h});
    for (int step = 0; step < kSteps; ++step) {
        char label[8];
        std::snprintf(label, sizeof label, "%d", step + 1);
        Rect h{grid.x + step * cellW - scrollX_, heading.y, cellW, heading.h};
        if (ui.fSmall) rr.textIn(*ui.fSmall, h, label, step % 4 == 0 ? nx::text : nx::muted.alpha(0.6f), Align::Center, 0);
    }
    rr.popClip();
    const int currentStep = playing && std::isfinite(playheadBeats) && playheadBeats >= 0.0 ?
        (int)std::floor(std::fmod(playheadBeats, std::max(kStepBeat, clip.lengthBeats)) / kStepBeat) : -1;

    rr.pushClip(body);
    for (int lane = 0; lane < kLanes; ++lane) {
        const f32 y = body.y + lane * rowH - scrollY_;
        Rect row{body.x, y, body.w, rowH};
        if (row.bottom() <= body.y || row.y >= body.bottom()) continue;
        Rect name{row.x + 4 * s, y, nameW - 10 * s, rowH};
        // Name targets are at least 24px tall except where the viewport clips.
        if (ui.button(uiId(UiDrumSequencer, 10, lane), name, kNames[lane],rhythmLane_==lane)) {
            rhythmLane_=lane;
            preview(kPitches[lane]);
        }
        if (ui.hovered(name)) ui.tip = std::string("Audition and select ") + kNames[lane] + " for rhythm generation";
        rr.pushClip(grid);
        for (int step = 0; step < kSteps; ++step) {
            const int absolute = page_ * kSteps + step;
            Rect hit{grid.x + step * cellW - scrollX_, y, cellW, rowH};
            if (hit.right() <= grid.x || hit.x >= grid.right()) continue;
            const Rect tile{hit.x + 2 * s, hit.y + 3 * s, hit.w - 4 * s, hit.h - 6 * s};
            int found = -1;
            for (size_t i = 0; i < clip.notes.size(); ++i)
                if (clip.notes[i].pitch == kPitches[lane] && stepOf(clip.notes[i]) == absolute) {
                    found = (int)i; break;
                }
            const bool enabled = absolute < totalSteps;
            const u64 id = uiId(UiDrumSequencer, 20 + lane, absolute);
            const bool hot = enabled && ui.setHot(id, hit) && ui.isHot(id);
            const bool active = found >= 0;
            Col fill = ((step / 4) % 2) ? nx::panel2 : nx::panel;
            if (active) fill = nx::violet.mix(nx::violetSoft, 0.2f + 0.35f * clip.notes[(size_t)found].vel / 127.f);
            if (hot) fill = fill.mix(nx::text, 0.12f);
            rr.roundRect(tile, 4 * s, enabled ? fill : fill.alpha(0.2f));
            if (active) {
                const f32 velocity = clip.notes[(size_t)found].vel / 127.f;
                rr.rect({tile.x + 4 * s, tile.bottom() - 4 * s,
                         std::max(1.f, (tile.w - 8 * s) * velocity), 2 * s}, nx::text.alpha(0.8f));
            }
            if (absolute == currentStep)
                rr.roundRectOutline(tile, 4 * s, std::max(1.f, 2 * s), nx::cyan);
            if (hot && ui.active == 0) {
                ui.cursor = Cursor::Hand;
                char tip[160];
                std::snprintf(tip, sizeof tip, "%s - step %d%s, velocity %d. Click toggles; right-click erases; Ctrl + scroll changes velocity.",
                              kNames[lane], absolute + 1, active ? " on" : " off",
                              active ? (int)clip.notes[(size_t)found].vel : 100);
                ui.tip = tip;
                const bool erase = in.pressed[2] || (in.pressed[0] && active);
                if (erase && active) {
                    clip.notes.erase(std::remove_if(clip.notes.begin(), clip.notes.end(), [&](const NoteModel& n) {
                        return n.pitch == kPitches[lane] && stepOf(n) == absolute;
                    }), clip.notes.end());
                    changed = true;
                    lastEdit_ = "erase drum step";
                } else if (in.pressed[0] && !active) {
                    addStep(clip, lane, absolute);
                    preview(kPitches[lane]);
                    changed = true;
                    lastEdit_ = "add drum step";
                } else if (active && in.ctrl() && in.wheel != 0) {
                    const int velocity = clampv((int)clip.notes[(size_t)found].vel + (in.wheel > 0 ? 5 : -5), 1, 127);
                    if (velocity != clip.notes[(size_t)found].vel) {
                        for (auto& n : clip.notes)
                            if (n.pitch == kPitches[lane] && stepOf(n) == absolute) n.vel = (u8)velocity;
                        changed = true;
                        lastEdit_ = "drum step velocity";
                    }
                }
            }
        }
        rr.popClip();
    }
    rr.popClip();
    // A visible scrollbar makes smaller editors' additional lanes discoverable.
    if (rowH * kLanes > body.h) {
        const f32 thumbH = std::max(16 * s, body.h * body.h / (rowH * kLanes));
        const f32 travel = body.h - thumbH;
        rr.roundRect({r.right() - 4 * s, body.y + travel * scrollY_ / (rowH * kLanes - body.h),
                      3 * s, thumbH}, 1.5f * s, nx::muted.alpha(0.55f));
    }
    rr.rect(rhythm,nx::panel2);
    Rect laneBox{rhythm.x+6*s,rhythm.y+4*s,104*s,28*s};
    ui.selector(uiId(UiDrumSequencer,40),laneBox,&rhythmLane_,kNames,kLanes);
    Rect hitsBox{laneBox.right()+6*s,laneBox.y,82*s,28*s};
    rr.roundRect(hitsBox,4*s,nx::panel);
    ui.dragNumber(uiId(UiDrumSequencer,41),hitsBox,&rhythmHits_,0,16,1,"Hits %.0f",Align::Center,nullptr,1,4);
    Rect rotationBox{hitsBox.right()+6*s,laneBox.y,82*s,28*s};
    rr.roundRect(rotationBox,4*s,nx::panel);
    ui.dragNumber(uiId(UiDrumSequencer,42),rotationBox,&rhythmRotation_,0,15,1,"Shift %.0f",Align::Center,nullptr,1,0);
    Rect velocityBox{narrowRhythm?laneBox.x:rotationBox.right()+6*s,
                     narrowRhythm?laneBox.y+36*s:laneBox.y,82*s,28*s};
    rr.roundRect(velocityBox,4*s,nx::panel);
    ui.dragNumber(uiId(UiDrumSequencer,43),velocityBox,&rhythmVelocity_,1,127,1,"Vel %.0f",Align::Center,nullptr,1,100);
    Rect generate{velocityBox.right()+6*s,velocityBox.y,116*s,28*s};
    if(ui.button(uiId(UiDrumSequencer,44),generate,rhythmHits_<.5?"Clear lane":"Generate rhythm",true,nx::violet)) {
        if(generateDrumRhythm(clip,kPitches[rhythmLane_],page_,(int)std::round(rhythmHits_),
                              (int)std::round(rhythmRotation_),(int)std::round(rhythmVelocity_))) {
            changed=true;
            lastEdit_="generate drum rhythm";
        }
    }
    if(ui.hovered(generate) || ui.hovered(hitsBox) || ui.hovered(rotationBox) || ui.hovered(velocityBox))
        ui.tip="Evenly distribute Hits across 16 steps, rotated by Shift. Replaces only this lane on this bar; zero Hits clears it. One Undo restores it.";
    rr.rect(footer, nx::panel);
    static const char* presets[] = {"808 basic", "House", "Hip hop"};
    Rect presetBox{footer.x + 6 * s, footer.y + 3 * s, 112 * s, 28 * s};
    ui.selector(uiId(UiDrumSequencer, 4), presetBox, &preset_, presets, 3);
    Rect apply{presetBox.right() + 6 * s, presetBox.y, 112 * s, 28 * s};
    if (ui.button(uiId(UiDrumSequencer, 5), apply, "Replace page")) {
        const int first = page_ * kSteps, end = first + kSteps;
        clip.notes.erase(std::remove_if(clip.notes.begin(), clip.notes.end(), [&](const NoteModel& n) {
            return drumPitch(n.pitch) && stepOf(n) >= first && stepOf(n) < end;
        }), clip.notes.end());
        auto hit = [&](int lane, int step, int vel = 100) {
            if (first + step < totalSteps) addStep(clip, lane, first + step, vel);
        };
        if (preset_ == 1) {
            for (int i = 0; i < 16; i += 4) hit(0, i, 112);
            hit(2, 4); hit(2, 12);
            for (int i = 2; i < 16; i += 4) hit(4, i, 84);
        } else if (preset_ == 2) {
            hit(0, 0, 112); hit(0, 6, 96); hit(0, 10, 108);
            hit(1, 4, 112); hit(1, 12, 112);
            for (int i = 0; i < 16; i += 2) hit(3, i, i % 4 ? 65 : 92);
        } else {
            hit(0, 0, 116); hit(0, 8, 108); hit(0, 11, 95);
            hit(1, 4, 110); hit(1, 12, 110);
            for (int i = 0; i < 16; i += 2) hit(3, i, i % 4 ? 68 : 92);
            hit(2, 12, 78);
        }
        changed = true;
        lastEdit_ = "replace drum page";
    }
    if (ui.hovered(apply)) ui.tip = "Replace this page's eight drum lanes with the chosen pattern. Other pages and pitches are kept.";
    if (ui.fSmall && footer.w > 630 * s)
        rr.textIn(*ui.fSmall, {apply.right() + 12 * s, footer.y, footer.right() - apply.right() - 20 * s, footer.h},
                  "Click steps  /  Scroll lanes  /  Ctrl + scroll velocity", nx::muted, Align::Right, 0);
    if (changed) sortNotes(clip);
    rr.popClip();
    return changed;
}

} // namespace lat
