// FL-inspired channel rack. Patterns are the session's scene slots and every
// step edits the same ClipModel that the piano roll and engine already use.
#include "app.h"
#include "app_internal.h"
#include <algorithm>
#include <cstdio>

namespace lat {

void App::drawChannelRack(const Rect& r) {
    if (r.w <= 0.f || r.h <= 0.f) return;
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    if (!ses_.scenes.empty()) selSlot_ = clampv(selSlot_, 0, (int)ses_.scenes.size() - 1);
    else selSlot_ = 0;
    rend_.pushClip(r);

    const f32 pad = 8.f * s;
    Rect head{r.x + pad, r.y + 4.f * s, r.w - 2.f * pad, 24.f * s};
    Rect add{head.right() - 28.f*s, head.y, 26.f*s, 22.f*s};
    if (ui_.button(uiId(UiChannelRack, 0), add, "+") &&
        ses_.scenes.size() < kMaxScenes) {
        newStudioPattern();
    }
    Rect place{add.x - 54.f*s, add.y, 50.f*s, add.h};
    if (ui_.button(uiId(UiChannelRack, 4), place, "Place") && !ses_.scenes.empty()) {
        // Place the whole selected session pattern at the next free bar. Each
        // arrangement item owns a copy of the same ClipModel, just like a drop
        // from the session grid into the Playlist.
        f64 start = std::max(0.0, es_.beat);
        for (const TrackModel& t : ses_.tracks)
            for (const ArrangeClip& item : t.arrange)
                start = std::max(start, item.end());
        const f64 bar = std::ceil(ses_.barOfBeat(start) - 1e-9);
        start = ses_.beatOfBar(bar);
        bool placed = false;
        for (size_t ti = 0; ti < ses_.tracks.size(); ++ti) {
            TrackModel& t = ses_.tracks[ti];
            const ClipModel& src = t.slots[(size_t)selSlot_];
            if (!src.valid() || (int)t.arrange.size() >= kMaxArrItems) continue;
            if (!placed) undoPoint("place pattern");
            ArrangeClip item;
            item.uid = ses_.newUid();
            item.start = start;
            item.length = src.lengthBeats > kMinArrBeats ? src.lengthBeats : 4.0;
            item.sourceUid = src.uid;
            item.src = src;
            item.src.uid = item.uid;
            t.arrange.push_back(std::move(item));
            arrangeRepair(t.arrange);
            publishArrangement((int)ti);
            placed = true;
        }
        status_ = placed ? "Pattern placed in the Playlist" : "Pattern has no clips to place";
        if (placed) {
            view_ = MainView::Arrangement;
            if(!studioSongMode_ && es_.playing) {send(Cmd::SetPlaying,0);autoRecFinish();}
            studioSongMode_ = true;
        }
    }
    if (ui_.hovered(place))
        ui_.tip = "Place this pattern's clips in the Playlist at the next free bar";

    const f32 navW = 22.f*s;
    Rect next{place.x - navW - 4.f*s, add.y, navW, add.h};
    Rect prev{next.x - navW - 3.f*s, next.y, navW, add.h};
    if (ui_.button(uiId(UiChannelRack, 1), prev, "<") && !ses_.scenes.empty())
        selectStudioPattern((selSlot_+(int)ses_.scenes.size()-1)%(int)ses_.scenes.size());
    if (ui_.button(uiId(UiChannelRack, 2), next, ">") && !ses_.scenes.empty())
        selectStudioPattern((selSlot_+1)%(int)ses_.scenes.size());

    Rect pattern{head.x,head.y+1.f*s,150.f*s,22.f*s};
    if (ses_.scenes.empty()) {
        rend_.textIn(fSmall_, pattern, "No patterns", nx::muted, Align::Center, 0);
    } else {
        selSlot_ = clampv(selSlot_, 0, (int)ses_.scenes.size() - 1);
        const std::string patternLabel=studioPatternName(selSlot_);
        if(ui_.button(uiId(UiChannelRack,3),pattern,patternLabel.c_str())) openStudioPatternPicker(pattern);
        if(ui_.hovered(pattern)) ui_.tip="Choose, rename or clone a pattern";
    }
    Rect addChannel{pattern.right()+5.f*s,head.y,100.f*s,22.f*s};
    if(ui_.button(uiId(UiChannelRack,5),addChannel,"+ Sound")) {
        studioToolsOpen_ = true;
        studioMenuCreate_ = true;
    }
    if(ui_.hovered(addChannel)) ui_.tip="Create a track, instrument, or drum channel";
    const Rect brush{addChannel.right()+8*s,head.y,64*s,22*s};
    if(ui_.button(uiId(UiChannelRack,6),brush,"Paint",studioPatternPaint_,rgb(0x007A85))) togglePatternPaint();
    if(ui_.hovered(brush)) ui_.tip="Drag repeated copies of this pattern into the Playlist";
    rend_.hairlineH(r.x + pad, r.right() - pad, head.bottom() + 2.f * s,
                    nx::hairlineInk.alpha(0.32f));

    const f32 rowTop = head.bottom() + 10.f * s;
    const f32 rowH = 34.f * s;
    const f32 contentH = std::max(0.f, r.bottom() - rowTop - pad);
    const f32 totalH = (f32)ses_.tracks.size() * rowH;
    const f32 maxScroll = std::max(0.f, totalH - contentH);
    if (r.contains(in.mx, in.my) && in.wheel != 0.f)
        channelRackScrollY_ = clampv(channelRackScrollY_ - in.wheel * rowH, 0.f, maxScroll);
    else channelRackScrollY_ = clampv(channelRackScrollY_, 0.f, maxScroll);

    rend_.pushClip({r.x + 2.f * s, rowTop, r.w - 4.f * s, contentH});
    for (size_t ti = 0; ti < ses_.tracks.size(); ++ti) {
        TrackModel& track = ses_.tracks[ti];
        const f32 y = rowTop + (f32)ti * rowH - channelRackScrollY_;
        Rect row{r.x + pad, y, r.w - 2.f * pad, rowH - 2.f * s};
        if (row.bottom() < rowTop || row.y > r.bottom()) continue;
        const Col trackTint = pal::clipColors[track.colorIdx % pal::clipColorCount];
        rend_.rect(row,(int)ti==selTrack_?trackTint.alpha(.07f):pal::appBg);
        rend_.hairlineH(row.x+2*s,row.right()-2*s,row.bottom(),nx::hairlineInk.alpha(.24f));
        rend_.roundRect({row.x,row.y+4*s,2*s,row.h-8*s},s,trackTint);
        const f32 nameY=row.y+3*s;
        char number[24]; std::snprintf(number,sizeof number,"%zu",ti+1);
        rend_.textIn(fSmall_,{row.x+3*s,nameY,14*s,28*s},number,nx::muted,Align::Left,0);
        rend_.circle(row.x+22*s,row.cy(),3.5f*s,trackTint);
        Rect name{row.x+30*s,nameY,90*s,28*s};
        rend_.textIn(fSmall_,name,track.name.c_str(),(int)ti==selTrack_?trackTint:nx::text,Align::Left,0);
        const u64 nameId=uiId(UiChannelRack,10,(int)ti);
        const bool nameHot=ui_.setHot(nameId,name)&&ui_.isHot(nameId);
        auto openSound=[&]() {
            const bool sameSound=!track.devices.empty()&&track.devices.front().uid==spectraOpenUid_;
            if(!sameSound&&!closeFocusedSpectra()) return;
            selectTrack((int)ti); detailTab_=DetailTab::Devices; showDetail_=true;
            selDevice_=track.devices.empty()?-1:0; activateStudioWindow(1); ensurePluginScan();
            if(!track.devices.empty()&&track.devices.front().inst&&isSpectra(track.devices.front().inst.get())) {
                spectraOpenUid_=track.devices.front().uid; spectraForced_=false;
                spectraScrollTo_=true; activateStudioWindow(2);
            }
        };
        if(nameHot&&ui_.hotNext==nameId&&ui_.editId!=uiId(UiChannelRack,18,(int)ti)) {
            ui_.cursor=Cursor::Hand;
            if(in.pressed[0]) selectTrack((int)ti);
            if(in.dblClick) openSound();
            ui_.tip="Select channel; double-click to open its Sound editor";
        }
        const u64 renameId=uiId(UiChannelRack,18,(int)ti);
        Rect rename{row.x+121*s,nameY+6*s,16*s,18*s};
        static std::string oldName;
        if(ui_.editId==renameId) {
            if(ui_.textField(renameId,name,&track.name,pal::panelAlt,nx::text,Align::Left,false))
                undoPointWith("rename track",track.name,oldName);
        } else {
            if(ui_.button(renameId,rename,"")) {
                oldName=track.name;ui_.editId=renameId;ui_.editBuf=track.name;
                ui_.caret=(int)track.name.size();ui_.active=renameId;
            }
            rend_.line(rename.x+4*s,rename.y+12*s,rename.x+11*s,rename.y+5*s,s,nx::muted);
            rend_.line(rename.x+3*s,rename.y+13*s,rename.x+6*s,rename.y+12*s,s,nx::muted);
        }
        if(ui_.hovered(rename)) ui_.tip="Rename channel";

        Rect piano{row.x+141*s,row.y+7*s,18*s,18*s};
        if (ui_.button(uiId(UiChannelRack, 12, (int)ti), piano, "P")) {
            if (ses_.scenes.empty()) {
                status_ = "Add a pattern before opening its notes";
            } else {
                selectTrack((int)ti);
                selSlot_ = clampv(selSlot_, 0, (int)ses_.scenes.size() - 1);
                ClipModel& selectedClip = track.slots[selSlot_];
                if (!selectedClip.valid() && !trackHasNoteDevice((int)ti)) {
                    status_="No clip here - choose another pattern or add an instrument with Sound";
                } else {
                    if (!selectedClip.valid()) createMidiClip((int)ti, selSlot_);
                    detailPattern_ = true;
                    showDetail_ = true;
                    detailTab_ = DetailTab::Clip;
                    midiInspectorPage_ = 0;
                    if (drumDeviceFor((int)ti)) drumEditor_ = 1;
                    activateStudioWindow(1);
                }
            }
        }

        if(ui_.hovered(piano)) ui_.tip="Edit this pattern's notes in Piano Roll";
        Rect mute{row.x+161*s,row.y+7*s,22*s,20*s};
        Rect solo{mute.right()+2*s,row.y+7*s,22*s,20*s};
        const bool wasMute = track.mute;
        if (ui_.segButton(uiId(UiChannelRack, 13, (int)ti), mute,
                          track.mute, pal::meterAmber)) {
            track.mute = !track.mute;
            undoPointWith("mute", track.mute, wasMute, uiId(UiChannelRack, 13, (int)ti));
            send(Cmd::TrackMute, (int)ti, track.mute ? 1 : 0);
        }
        ui_.microIn(fSmall_, mute, "M", track.mute ? nx::inkOn(pal::meterAmber) : nx::muted,
                    Align::Center, 0);
        const bool wasSolo = track.solo;
        if (ui_.segButton(uiId(UiChannelRack, 14, (int)ti), solo,
                          track.solo, pal::soloBlue)) {
            track.solo = !track.solo;
            undoPointWith("solo", track.solo, wasSolo, uiId(UiChannelRack, 14, (int)ti));
            send(Cmd::TrackSolo, (int)ti, track.solo ? 1 : 0);
        }
        ui_.microIn(fSmall_, solo, "S", track.solo ? nx::inkOn(pal::soloBlue) : nx::muted,
                    Align::Center, 0);

        Rect arm{solo.right()+3*s,row.y+9*s,15*s,15*s};
        const bool wasArm=track.arm;
        if(ui_.segButton(uiId(UiChannelRack,17,(int)ti),arm,track.arm,pal::armRed)) {
            track.arm=!track.arm; undoPointWith("arm",track.arm,wasArm);
            send(Cmd::TrackArm,(int)ti,track.arm?1:0);
            if((int)ti==autoArmed_) autoArmed_=-1;
        }
        rend_.circle(ui_.lastRect.cx(),ui_.lastRect.cy(),2.5f*s,
                     track.arm?nx::text:pal::recRed.scale(.55f));

        Rect vol{arm.right()+4*s,row.y+5*s,24*s,24*s};
        const f32 beforeVol = track.fader;
        if (ui_.knob(uiId(UiChannelRack, 15, (int)ti), vol, &track.fader,0.f,1.f,.85f)) {
            undoPointWith("volume", track.fader, beforeVol,
                          uiId(UiChannelRack, 15, (int)ti));
            send(Cmd::TrackVol, (int)ti, 0, faderToGain(track.fader));
        }

        if(ui_.hovered(vol)) ui_.tip="Channel volume - double-click to reset";
        const f32 stepX=vol.right()+7*s;
        const f32 gap=2*s;
        const f32 stepW=clampv((row.right()-stepX-15*gap)/16.f,18.f*s,20.f*s);
        ClipModel& clip = track.slots[selSlot_];
        const bool sampleOnly = !trackHasNoteDevice((int)ti) && clip.kind != ClipKind::Midi;
        if(sampleOnly || (clip.kind==ClipKind::Audio && clip.valid())) {
            const Rect preview{stepX,row.y+5*s,std::max(0.f,row.right()-stepX),24*s};
            rend_.roundRect(preview,3*s,trackTint.alpha(.06f));
            if(clip.kind==ClipKind::Audio&&clip.valid()&&clip.sample&&clip.sample->peakBuckets>0) {
                const int bars=std::max(1,(int)(preview.w/(3*s)));
                f32 peak=.001f;
                for(int b=0;b<bars;++b) {
                    const int src=clampv((int)((f64)b/bars*clip.sample->peakBuckets),0,clip.sample->peakBuckets-1);
                    peak=std::max(peak,std::max(std::abs(clip.sample->peaks[(size_t)src*2]),std::abs(clip.sample->peaks[(size_t)src*2+1])));
                }
                const f32 height=9*s/peak;
                for(int b=0;b<bars;++b) {
                    const int src=clampv((int)((f64)b/bars*clip.sample->peakBuckets),0,clip.sample->peakBuckets-1);
                    const f32 lo=clip.sample->peaks[(size_t)src*2],hi=clip.sample->peaks[(size_t)src*2+1];
                    const f32 xw=preview.x+2*s+b*3*s;
                    rend_.line(xw,preview.cy()-hi*height,xw,preview.cy()-lo*height,s,trackTint.alpha(.5f));
                }
            }
            const f32 textW=std::min(120*s,preview.w);
            rend_.roundRect({preview.x,preview.y,textW,preview.h},3*s,pal::appBg.alpha(.85f));
            rend_.textIn(fSmall_,{preview.x+7*s,preview.y,textW-12*s,preview.h},
                          clip.valid()?clip.name.c_str():"No sound",nx::muted,Align::Left,0);
            continue;
        }
        for (int step = 0; step < 16; ++step) {
            const f64 beat = step * 0.25;
            const u8 pitch = drumDeviceFor((int)ti) ? 36 : 60;
            bool on = false;
            if (clip.kind == ClipKind::Midi)
                for (const NoteModel& n : clip.notes)
                    if (n.pitch == pitch && std::abs(n.beat - beat) < 1e-5) { on = true; break; }
            Rect padR{stepX + step * (stepW + gap), row.y + 11.f*s,stepW,18.f*s};
            const u64 id = uiId(UiChannelRack, 16, (int)ti, step);
            if (ses_.scenes.empty()) {
                rend_.roundRect(padR, nx::radiusXs * s, pal::panel);
                continue;
            }
            if(!on) {
                rend_.roundRect(padR,3*s,(step/4)%2?rgb(0x22272E):rgb(0x181D24));
                rend_.hairlineH(padR.x+3*s,padR.right()-3*s,padR.y+1*s,rgb(0x39414D).alpha(.45f),s);
            }
            if (!ui_.segButton(id, padR, on, trackTint)) continue;
            const u64 gesture = id;
            undoPoint("step edit", gesture);
            if (!clip.valid()) {
                clip = ClipModel{};
                clip.uid = ses_.newUid();
                clip.kind = ClipKind::Midi;
                char label[32];
                std::snprintf(label, sizeof label, "MIDI %d", midiClipNo_++);
                clip.name = label;
                clip.colorIdx = track.colorIdx;
                clip.lengthBeats = 4.0;
                clip.loop = true;
            }
            auto found = std::find_if(clip.notes.begin(), clip.notes.end(),
                [&](const NoteModel& n) {
                    return n.pitch == pitch && std::abs(n.beat - beat) < 1e-5;
                });
            if (found == clip.notes.end()) {
                NoteModel note;
                note.beat = beat;
                note.len = 0.25;
                note.pitch = pitch;
                clip.notes.push_back(note);
                std::stable_sort(clip.notes.begin(), clip.notes.end(),
                    [](const NoteModel& a, const NoteModel& b) { return a.beat < b.beat; });
            } else clip.notes.erase(found);
            pushClip((int)ti, selSlot_);
        }
        for (int beat = 0; beat < 4; ++beat) {
            const f32 x = stepX + beat * 4 * (stepW + gap);
            rend_.textIn(fSmall_, {x, row.y + 1.f * s, stepW * 3.f, 10.f * s},
                         (beat == 0 ? "1" : beat == 1 ? "2" : beat == 2 ? "3" : "4"),
                         nx::muted.alpha(0.8f), Align::Left, 0);
        }
    }
    rend_.popClip();
    rend_.popClip();
}

} // namespace lat
