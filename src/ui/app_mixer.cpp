// Slim dock mixer: one shared channel strip per track, plus a focused chain
// inspector. All edits use the same model, undo and engine paths as the editors.
#include "app_internal.h"

namespace lat {
namespace {
Col mixerPlate() { return pal::panel.alpha(0.22f); }
Col mixerEdge() { return nx::hairlineInk.alpha(0.32f); }

void drawMixerGain(Renderer& r, const Font& f, Rect box, f32 position) {
    const f32 db = gainToDb(faderToGain(position));
    char text[20];
    if (db <= -59.f) std::snprintf(text, sizeof text, "-inf");
    else std::snprintf(text, sizeof text, "%.1f dB", db);
    r.textIn(f, box, text, nx::text, Align::Center, 0);
}
}

void App::drawStudioMixer(const Rect& r) {
    if (r.w <= 0.f || r.h <= 0.f) return;
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    constexpr f32 masterLogicalW = 78.f;
    constexpr f32 stripLogicalW = 80.f;
    constexpr f32 gapLogical = 2.f;
    const f32 headingH = 18.f * s;
    const f32 stripTop = r.y + headingH;
    const f32 stripH = std::max(0.f, r.bottom() - stripTop);
    const f32 masterW = masterLogicalW * s;
    const f32 inspectorW = std::min(202.f * s, r.w * 0.32f);
    const f32 channelsX = r.x + masterW + gapLogical*s;
    const f32 inspectorX = r.right() - inspectorW;
    const Rect channels{channelsX, stripTop,
                        std::max(0.f, inspectorX - gapLogical*s - channelsX), stripH};
    constexpr f32 stripStepLogical = stripLogicalW + gapLogical;
    const size_t count = ses_.tracks.size() + kMaxReturns;
    const f32 contentW = count ? ((f32)count*stripLogicalW + (f32)(count-1)*gapLogical)*s : 0.f;
    const f32 maxScroll = std::max(0.f, contentW - channels.w);
    if (channels.contains(in.mx, in.my) && in.wheel != 0.f)
        studioMixerScrollX_ = clampv(studioMixerScrollX_ - in.wheel*60.f*s, 0.f, maxScroll);
    studioMixerScrollX_ = clampv(studioMixerScrollX_, 0.f, maxScroll);

    rend_.rect(r, pal::appBg);
    rend_.textIn(fBody_, {r.x + 4*s, r.y, 58*s, headingH}, "MIXER", nx::text, Align::Left, 0);
    rend_.textIn(fSmall_, {channels.right() - 116*s, r.y, 112*s, headingH},
                 "Scroll to more channels", nx::muted, Align::Right, 0);
    rend_.hairlineH(r.x, r.right(), stripTop, nx::hairlineInk.alpha(0.45f));

    // Master stays reachable while the channel bank scrolls.
    rend_.pushClip({r.x, stripTop, masterW, stripH});
    const u64 masterId=uiId(UiMixer,399);
    const bool masterHot=ui_.setHot(masterId,{r.x,stripTop,masterW,stripH})&&ui_.isHot(masterId);
    const bool masterSelected = devOwner_ == kOwnMaster;
    const Rect masterPlate{r.x+2*s,stripTop+2*s,masterW-4*s,stripH-3*s};
    rend_.roundRect(masterPlate,4*s,mixerPlate());
    rend_.roundRectOutline(masterPlate,4*s,s,masterSelected?nx::violet.alpha(.72f):mixerEdge());
    rend_.roundRect({r.x+5*s,stripTop+3*s,masterW-10*s,4*s},2*s,nx::violet);
    rend_.textIn(fBody_, {r.x+3*s,stripTop+8*s,masterW-6*s,15*s}, "MASTER",
                 nx::text, Align::Center, 0);
    const f32 masterFaderY = stripTop + 27*s;
    const f32 faderH = std::min(70.f*s, std::max(26.f*s, stripH - 49*s));
    Rect masterL{r.x+8*s,masterFaderY,7*s,faderH};
    Rect masterR{masterL.right()+2*s,masterFaderY,7*s,faderH};
    Rect masterF{r.x+34*s,masterFaderY,16*s,faderH};
    rend_.well({masterL.x-3*s,masterFaderY-3*s,masterF.right()-masterL.x+6*s,faderH+6*s},
               nx::radiusXs*s);
    peakHoldM_[0] = std::max(es_.masterMeterL, peakHoldM_[0]*0.985f);
    peakHoldM_[1] = std::max(es_.masterMeterR, peakHoldM_[1]*0.985f);
    ui_.meterV(masterL,es_.masterMeterL,peakHoldM_[0]);
    ui_.meterV(masterR,es_.masterMeterR,peakHoldM_[1]);
    if (ui_.grab(6*s).vFader(uiId(UiMixer,399,1),masterF,&studioMasterFader_))
        send(Cmd::MasterVol,0,0,faderToGain(studioMasterFader_));
    drawMixerGain(rend_,fSmall_,{r.x+3*s,masterFaderY+faderH+3*s,masterW-6*s,12*s},studioMasterFader_);
    if(masterHot&&ui_.hotNext==masterId&&in.pressed[0]) selectChainOwner(kOwnMaster);
    rend_.popClip();

    // Track and return strips share one compact, wheel-scrollable bank.
    rend_.pushClip(channels);
    f32 x = channels.x - studioMixerScrollX_;
    for (size_t ti=0; ti<ses_.tracks.size(); ++ti, x+=stripStepLogical*s) {
        TrackModel& t=ses_.tracks[ti];
        Rect col{x,stripTop,stripLogicalW*s,stripH};
        if (col.right()<channels.x || col.x>channels.right()) continue;
        const u64 stripId=uiId(UiMixer,100+(int)ti);
        const bool hot=ui_.setHot(stripId,col)&&ui_.isHot(stripId);
        const Col tint=pal::clipColors[t.colorIdx%pal::clipColorCount];
        const bool selected=(int)ti==selTrack_;
        const Rect plate{col.x+2*s,col.y+2*s,col.w-4*s,col.h-3*s};
        rend_.roundRect(plate,4*s,mixerPlate());
        rend_.roundRectOutline(plate,4*s,s,selected?tint.alpha(.62f):mixerEdge());
        rend_.roundRect({col.x+5*s,col.y+3*s,col.w-10*s,4*s},2*s,tint);
        if(selected) rend_.roundRect({col.x+4*s,col.y+8*s,2*s,col.h-12*s},s,tint.alpha(.72f));
        char number[24]; std::snprintf(number,sizeof number,"%zu",ti+1);
        Rect label{col.x+4*s,col.y+8*s,col.w-8*s,16*s};
        rend_.textIn(fSmall_,{label.x,label.y,13*s,label.h},number,nx::muted,Align::Left,0);
        Rect name{label.x+13*s,label.y,label.w-13*s,label.h};
        rend_.textIn(fBody_,name,t.name.c_str(),selected?tint.alpha(1.f):nx::text,Align::Left,0);
        const u64 headId=uiId(UiMixer,1000+(int)ti);
        const bool headHot=ui_.setHot(headId,name)&&ui_.isHot(headId);
        if(headHot&&ui_.hotNext==headId) {
            ui_.cursor=Cursor::Hand;
            if(in.pressed[0]) selectTrack((int)ti);
            if(in.dblClick&&closeFocusedSpectra()) {
                selectTrack((int)ti); detailTab_=DetailTab::Devices; showDetail_=true;
                activateStudioWindow(1); ensurePluginScan();
            }
            ui_.tip="Click to select; double-click to open devices";
        }
        const f32 fy=col.y+26*s;
        const f32 fh=std::min(70*s,std::max(26*s,col.bottom()-fy-32*s));
        Rect meterL{col.x+8*s,fy,7*s,fh}, meterR{meterL.right()+2*s,fy,7*s,fh};
        Rect fader{col.x+34*s,fy,16*s,fh};
        rend_.well({meterL.x-3*s,fy-3*s, fader.right()-meterL.x+6*s,fh+6*s},nx::radiusXs*s);
        const f32 l=es_.meterL[ti], rr=es_.meterR[ti];
        peakHoldT_[ti]=std::max(std::max(l,rr),peakHoldT_[ti]*0.985f);
        ui_.meterV(meterL,l,peakHoldT_[ti]); ui_.meterV(meterR,rr,peakHoldT_[ti]);
        const f32 oldFader=t.fader;
        if(ui_.grab(6*s).vFader(uiId(UiMixer,(int)ti,4),fader,&t.fader)) {
            undoPointWith("volume",t.fader,oldFader);
            send(Cmd::TrackVol,(int)ti,0,faderToGain(t.fader));
            autoCapture(addr::trackField(t.uid,"vol"),t.fader,uiId(UiMixer,(int)ti,4));
        }
        drawMixerGain(rend_,fSmall_,{col.x+2*s,fy+fh+2*s,col.w-4*s,11*s},t.fader);
        const f32 buttonsY=col.bottom()-18*s;
        Rect mute{col.x+5*s,buttonsY,21*s,16*s}, solo{mute.right()+2*s,buttonsY,21*s,16*s};
        Rect arm{solo.right()+3*s,buttonsY+1*s,14*s,14*s};
        rend_.roundRect(mute,8*s,pal::appBg);
        rend_.roundRectOutline(mute,8*s,s,nx::hairlineInk.alpha(.32f));
        rend_.roundRect(solo,8*s,pal::appBg);
        rend_.roundRectOutline(solo,8*s,s,nx::hairlineInk.alpha(.32f));
        const bool oldMute=t.mute,oldSolo=t.solo,oldArm=t.arm;
        if(ui_.segButton(uiId(UiMixer,(int)ti,0),mute,t.mute,pal::meterAmber)) {
            t.mute=!t.mute; undoPointWith("mute",t.mute,oldMute);
            send(Cmd::TrackMute,(int)ti,t.mute?1:0);
            autoCapture(addr::trackField(t.uid,"mute"),t.mute?1.f:0.f,uiId(UiMixer,(int)ti,0));
        }
        ui_.microIn(fSmall_,ui_.lastRect,"M",t.mute?nx::inkOn(pal::meterAmber):nx::muted,Align::Center);
        if(ui_.segButton(uiId(UiMixer,(int)ti,1),solo,t.solo,pal::soloBlue)) {
            t.solo=!t.solo; undoPointWith("solo",t.solo,oldSolo);
            send(Cmd::TrackSolo,(int)ti,t.solo?1:0);
        }
        ui_.microIn(fSmall_,ui_.lastRect,"S",t.solo?nx::inkOn(pal::soloBlue):nx::muted,Align::Center);
        if(ui_.segButton(uiId(UiMixer,(int)ti,2),arm,t.arm,pal::armRed)) {
            t.arm=!t.arm; undoPointWith("arm",t.arm,oldArm); send(Cmd::TrackArm,(int)ti,t.arm?1:0);
            if((int)ti==autoArmed_) autoArmed_=-1;
        }
        rend_.circle(ui_.lastRect.cx(),ui_.lastRect.cy(),2.5f*s,t.arm?nx::text:pal::recRed.scale(0.6f));
        rend_.hairlineV(col.right()-1*s,col.y+5*s,col.bottom()-5*s,nx::hairlineInk.alpha(0.25f));
        if(hot&&ui_.hotNext==stripId&&in.pressed[0]) selectTrack((int)ti);
    }
    for(int ri=0;ri<kMaxReturns;++ri,x+=stripStepLogical*s) {
        ReturnModel& ret=ses_.returns[ri];
        Rect col{x,stripTop,stripLogicalW*s,stripH};
        if(col.right()<channels.x||col.x>channels.right()) continue;
        const int owner=ownReturn(ri);
        const u64 id=uiId(UiMixer,300+ri);
        const bool hot=ui_.setHot(id,col)&&ui_.isHot(id);
        const Rect plate{col.x+2*s,col.y+2*s,col.w-4*s,col.h-3*s};
        rend_.roundRect(plate,4*s,mixerPlate());
        rend_.roundRectOutline(plate,4*s,s,mixerEdge());
        rend_.roundRect({col.x+5*s,col.y+3*s,col.w-10*s,4*s},2*s,pal::soloBlue);
        rend_.textIn(fBody_,{col.x+3*s,col.y+8*s,col.w-6*s,16*s},kReturnLetter[ri],nx::text,Align::Center,0);
        const f32 fy=col.y+26*s, fh=std::min(70*s,std::max(26*s,col.bottom()-fy-24*s));
        Rect ml{col.x+15*s,fy,7*s,fh}, mr{ml.right()+3*s,fy,7*s,fh}, vf{col.x+40*s,fy,13*s,fh};
        rend_.well({ml.x-3*s,fy-3*s,vf.right()-ml.x+6*s,fh+6*s},nx::radiusXs*s);
        peakHoldR_[ri]=std::max(std::max(es_.returnMeterL[ri],es_.returnMeterR[ri]),peakHoldR_[ri]*.985f);
        ui_.meterV(ml,es_.returnMeterL[ri],peakHoldR_[ri]); ui_.meterV(mr,es_.returnMeterR[ri],peakHoldR_[ri]);
        const f32 old=ret.fader;
        if(ui_.grab(6*s).vFader(uiId(UiMixer,300+ri,1),vf,&ret.fader)) {
            undoPointWith("return volume",ret.fader,old); send(Cmd::ReturnVol,ri,0,faderToGain(ret.fader));
        }
        if(hot&&ui_.hotNext==id&&in.pressed[0]) selectChainOwner(owner);
    }
    rend_.popClip();

    // Focused inspector: sends and pan stay out of every strip; the chain rows
    // are the same live devices whose full controls open in the Devices window.
    rend_.pushClip({inspectorX,stripTop,inspectorW,stripH});
    rend_.rect({inspectorX,stripTop,inspectorW,stripH},pal::panel);
    rend_.hairlineV(inspectorX,stripTop,r.bottom(),nx::hairlineInk.alpha(0.45f));
    const int owner=(ownIsTrack(devOwner_)&&devOwner_<(int)ses_.tracks.size())?devOwner_:
                     ownIsReturn(devOwner_)?devOwner_:kOwnMaster;
    const bool trackOwner=ownIsTrack(owner);
    std::string title=trackOwner?ses_.tracks[owner].name:owner==kOwnMaster?"Master FX":
                      std::string("Return ")+kReturnLetter[owner-kOwnReturn0];
    rend_.textIn(fSmall_,{inspectorX+6*s,stripTop+3*s,inspectorW-48*s,16*s},title.c_str(),nx::text,Align::Left,0);
    Rect addFx{r.right()-38*s,stripTop+1*s,34*s,18*s};
    if(ui_.button(uiId(UiMixer,800),addFx,"+FX")) {
        selectChainOwner(owner); detailTab_=DetailTab::Devices; showDetail_=true;
        activateStudioWindow(1); ensurePluginScan();
    }
    f32 chainY=stripTop+23*s;
    if(trackOwner) {
        TrackModel& t=ses_.tracks[owner];
        Rect panBox{inspectorX+5*s,chainY+9*s,28*s,25*s};
        rend_.textIn(fSmall_,{inspectorX+2*s,chainY-2*s,25*s,10*s},"PAN",nx::muted,Align::Left,0);
        const f32 old=t.pan;
        if(ui_.knob(uiId(UiMixer,801),panBox,&t.pan,-1.f,1.f,0.f)) {
            undoPointWith("pan",t.pan,old); send(Cmd::TrackPan,owner,0,t.pan);
            autoCapture(addr::trackField(t.uid,"pan"),t.pan,uiId(UiMixer,801));
        }
        for(int si=0;si<kMaxReturns;++si) {
            const f32 sx=inspectorX+38*s+si*39*s;
            rend_.textIn(fSmall_,{sx,chainY-2*s,36*s,10*s},kReturnLetter[si],nx::muted,Align::Center,0);
            Rect knob{sx+7*s,chainY+10*s,22*s,22*s};
            const f32 before=t.sends[si];
            if(ui_.knob(uiId(UiMixer,802+si),knob,&t.sends[si],0.f,1.f,0.f)) {
                undoPointWith(kSendUndo[si],t.sends[si],before); send(Cmd::SendLevel,owner,si,t.sends[si]);
                autoCapture(addr::trackSend(t.uid,si),t.sends[si],uiId(UiMixer,802+si));
            }
            if(ui_.isHot(uiId(UiMixer,802+si))) {char value[24];std::snprintf(value,sizeof value,"Send %s %.0f%%",kReturnLetter[si],t.sends[si]*100.f);ui_.tip=value;}
        }
        chainY+=40*s;
    }
    ChainOwner co=chainOwner(owner);
    const f32 rowH=16*s;
    int visibleFx=0;
    if(co.devices) for(size_t di=0;di<co.devices->size()&&visibleFx<4;++di,++visibleFx) {
        DeviceModel& d=(*co.devices)[di];
        Rect row{inspectorX+5*s,chainY+visibleFx*rowH,inspectorW-10*s,rowH-1*s};
        const u64 nameId=uiId(UiMixer,830+(int)di);
        const u64 bypassId=uiId(UiMixer,850+(int)di);
        const f32 bypassW=25*s;
        Rect nameR{row.x,row.y,row.w-bypassW-3*s,row.h};
        if(ui_.button(nameId,nameR,d.desc.name.c_str())) {
            selectChainOwner(owner); selDevice_=(int)di; detailTab_=DetailTab::Devices;
            showDetail_=true; activateStudioWindow(1); ensurePluginScan();
        }
        const bool old=d.bypass;
        Rect bypass{row.right()-bypassW,row.y,bypassW,row.h};
        if(ui_.segButton(bypassId,bypass,d.bypass,pal::meterAmber)) {
            d.bypass=!d.bypass; undoPointWith("bypass",d.bypass,old);
            if(d.inst) d.inst->setBypassed(d.bypass);
        }
        ui_.microIn(fSmall_,ui_.lastRect,"B",d.bypass?pal::meterAmber:nx::muted,Align::Center);
    }
    if(co.devices&&co.devices->size()>4)
        rend_.textIn(fSmall_,{inspectorX+5*s,chainY+4*rowH,inspectorW-10*s,12*s},
                     (std::to_string(co.devices->size()-4)+" more in Devices").c_str(),nx::muted,Align::Left,0);
    if(co.devices&&co.devices->empty())
        rend_.textIn(fSmall_,{inspectorX+5*s,chainY,inspectorW-10*s,14*s},"No effects",nx::muted,Align::Left,0);
    rend_.popClip();
}

} // namespace lat
