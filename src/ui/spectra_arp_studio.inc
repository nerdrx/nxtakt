// Modern arpeggiator overview, included inside drawSpectraStudio(). All pattern
// changes use SpecState so imported wavetables and unknown state records survive.
{
    const bool arpEnabled = get(pArpOn) >= 0.5f;
    const Rect heading{content.x,content.y,content.w,40*s};
    rend_.textIn(fBold_,{heading.x,heading.y,heading.w-150*s,20*s},
                 "Notes into rhythm",nx::text,Align::Left,0);
    rend_.textIn(fSmall_,{heading.x,heading.y+22*s,heading.w-150*s,18*s},
                 arpEnabled ? "Hold a note or chord to play the pattern."
                            : "Enable the arpeggiator, then hold a note or chord.",
                 nx::muted,Align::Left,0);
    if (pArpOn < pc && ui_.button(id(3200),{heading.right()-136*s,heading.y,136*s,36*s},
                                  arpEnabled?"Arp enabled":"Arp off",arpEnabled,nx::violet))
        commit(pArpOn,arpEnabled?0.f:1.f);

    const Rect viewport{content.x,heading.bottom()+8*s,content.w,
                        std::max(1.f,content.bottom()-heading.bottom()-8*s)};
    const int columns = content.w >= 1000*s ? 16 : 8;
    const int patternRows = 16/columns;
    const f32 controlsH = 218*s, gap = 12*s;
    const f32 patternH = (86+patternRows*112)*s;
    const f32 needed = controlsH+gap+patternH;
    const f32 maxScroll = std::max(0.f,needed-viewport.h);
    st.scroll=clampv(st.scroll,0.f,maxScroll);
    const Rect canvas{viewport.x,viewport.y-st.scroll,
                      viewport.w-(maxScroll>0.f?28*s:0.f),needed};
    rend_.pushClip({viewport.x,viewport.y,canvas.w,viewport.h});

    // Consistent labeled fields make every arp parameter directly reachable.
    const Rect controls{canvas.x,canvas.y,canvas.w,controlsH};
    card(controls,"Performance","Timing, note order and how the pattern responds to your playing");
    auto arpNumber = [&](int param,const Rect& bounds,const char* format,f64 step) {
        if(param<0 || param>=pc) return;
        const ParamInfo& info=inst->paramInfo(param);
        f64 value=get(param);
        ui_.fieldWell(bounds,0.f);
        const f64 speed=std::max(0.001,(f64)(info.max-info.min)/180.0);
        if(ui_.dragNumber(id(100+param),bounds,&value,info.min,info.max,speed,
                          format,Align::Center,nullptr,step,info.def))
            commit(param,(f32)value);
        if(ui_.hovered(bounds)) ui_.tip=info.name+" - drag or scroll to adjust; right-click to type or reset";
    };
    const f32 fieldGap=8*s;
    const f32 firstW=(controls.w-28*s-4*fieldGap)/5.f;
    const char* firstLabels[5]={"Note order","Clock","Octaves","Octave order","Velocity"};
    const char* octaveModes[3]={"Up","Down","Alternating"};
    const char* velocityModes[3]={"Played","Fixed","Pattern"};
    for(int k=0;k<5;++k) {
        const f32 x=controls.x+14*s+k*(firstW+fieldGap);
        rend_.textIn(fSmall_,{x,controls.y+56*s,firstW,17*s},firstLabels[k],nx::muted,Align::Left,0);
        const Rect field{x,controls.y+77*s,firstW,32*s};
        if(k==0) choose(pArpMode,field,kArpMode,10);
        else if(k==1) choose(pArpSync,field,kSyncDiv,10);
        else if(k==2) arpNumber(pArpOctaves,field,"%.0f",1.0);
        else if(k==3) choose(pArpOctMode,field,octaveModes,3);
        else choose(pArpVelMode,field,velocityModes,3);
    }
    const f32 secondW=(controls.w-28*s-5*fieldGap)/6.f;
    const int secondParams[6]={pArpRate,pArpGate,pArpSwing,pArpFixedVel,pArpSteps,pArpChance};
    const char* secondLabels[6]={"Free rate","Gate","Swing","Fixed velocity","Steps","Chance"};
    const char* formats[6]={"%.2f Hz","%.0f%%","%.0f%%","%.0f","%.0f","%.0f%%"};
    for(int k=0;k<6;++k) {
        const f32 x=controls.x+14*s+k*(secondW+fieldGap);
        rend_.textIn(fSmall_,{x,controls.y+117*s,secondW,17*s},secondLabels[k],nx::muted,Align::Left,0);
        const Rect field{x,controls.y+138*s,secondW,32*s};
        if(k==0 && get(pArpSync)>0.5f) {
            rend_.roundRect(field,5*s,nx::bgTop);
            rend_.textIn(fSmall_,field,"Tempo sync",nx::muted,Align::Center,0);
            if(ui_.hovered(field)) ui_.tip="Choose Free in Clock to set the arpeggiator rate in Hz.";
        } else if(k==3 && (int)std::lround(get(pArpVelMode))!=1) {
            rend_.roundRect(field,5*s,nx::bgTop);
            rend_.textIn(fSmall_,field,"Not fixed",nx::muted,Align::Center,0);
            if(ui_.hovered(field)) ui_.tip="Choose Fixed velocity to use this value for every step.";
        } else arpNumber(secondParams[k],field,formats[k],k==0?0.01:1.0);
    }
    const bool hold=get(pArpHold)>0.5f, retrigger=get(pArpRetrig)>0.5f;
    if(pArpHold<pc && ui_.button(id(3201),{controls.x+14*s,controls.y+180*s,136*s,32*s},
                                hold?"Hold on":"Hold off",hold,nx::violet))
        commit(pArpHold,hold?0.f:1.f);
    if(pArpRetrig<pc && ui_.button(id(3202),{controls.x+158*s,controls.y+180*s,158*s,32*s},
                                   retrigger?"Retrigger on":"Retrigger off",retrigger,nx::violet))
        commit(pArpRetrig,retrigger?0.f:1.f);
    rend_.textIn(fSmall_,{controls.x+330*s,controls.y+180*s,controls.w-344*s,32*s},
                 hold?"Hold keeps the last notes after release.":"Retrigger restarts the pattern with new notes.",
                 nx::muted,Align::Left,0);

    SpecState arpState;
    const bool writable=arpState.parse(inst->stateString());
    auto writePattern = [&](const char* description,u64 gesture) {
        if(!writable) return;
        const std::string state=arpState.emit();
        if(state==inst->stateString()) return;
        undoPoint(description,gesture);
        if(inst->setStateString(state)) status_=description;
        else status_="The arpeggiator pattern could not be saved";
    };
    const Rect pattern{canvas.x,controls.bottom()+gap,canvas.w,patternH};
    card(pattern,"Step pattern",writable ? "Click: On / Tie / Rest. Scroll a step: octave. Drag the lower bars: velocity."
                                          : "This patch state cannot be edited safely; the pattern is read-only.");
    const int activeSteps=clampv((int)std::lround(get(pArpSteps)),1,16);
    const f32 cellGap=6*s;
    const f32 cellW=(pattern.w-28*s-(columns-1)*cellGap)/(f32)columns;
    for(int step=0;step<16;++step) {
        const Rect cell{pattern.x+14*s+(step%columns)*(cellW+cellGap),
                        pattern.y+64*s+(step/columns)*112*s,cellW,106*s};
        if(cell.bottom()<viewport.y || cell.y>viewport.bottom()) continue;
        const bool inLength=step<activeSteps;
        const f32 dim=inLength?1.f:0.45f;
        const bool on=arpState.arpOn(step),tie=arpState.arpTie(step);
        char label[24]; std::snprintf(label,sizeof label,"%02d",step+1);
        rend_.textIn(fSmall_,{cell.x,cell.y,cell.w,16*s},label,nx::muted.alpha(dim),Align::Center,0);
        const Rect stateButton{cell.x,cell.y+20*s,cell.w,32*s};
        const u64 stateId=id(3220+step);
        if(writable) {
            if(ui_.button(stateId,stateButton,!on?"Rest":tie?"Tie":"On",on,tie?nx::cyan:nx::violet)) {
                // Active -> tie -> rest -> active, preserving octave/reserved bits.
                arpState.arpSet(step,!on || !tie,on && !tie,arpState.arpOct(step));
                writePattern("Edit arpeggiator step",stateId);
            }
            if(viewport.contains(in.mx,in.my) && ui_.hovered(stateButton) && in.wheel!=0.f) {
                const int octave=clampv(arpState.arpOct(step)+(in.wheel>0.f?1:-1),-2,2);
                arpState.arpSet(step,arpState.arpOn(step),arpState.arpTie(step),octave);
                writePattern("Transpose arpeggiator step",stateId);
                in.wheel=0.f;
            }
            if(viewport.contains(in.mx,in.my) && ui_.hovered(stateButton))
                ui_.tip=inLength?"Click cycles On, Tie and Rest. Scroll changes this step's octave (-2 to +2)."
                                :"This step is outside the current Steps count. Its settings are kept for later.";
        } else {
            rend_.roundRect(stateButton,5*s,nx::panel2);
            rend_.textIn(fSmall_,stateButton,!on?"Rest":tie?"Tie":"On",nx::muted,Align::Center,0);
        }
        std::snprintf(label,sizeof label,"%+d oct",arpState.arpOct(step));
        rend_.textIn(fSmall_,{cell.x,cell.y+54*s,cell.w,16*s},label,nx::muted.alpha(dim),Align::Center,0);
        const Rect velocity{cell.x,cell.y+74*s,cell.w,30*s};
        const u64 velocityId=id(3260+step);
        const bool hot=writable && ui_.setHot(velocityId,velocity) && ui_.isHot(velocityId);
        if(hot && in.pressed[0]) ui_.active=velocityId;
        if(writable && ui_.active==velocityId && in.down[0]) {
            const int level=clampv((int)std::lround((velocity.bottom()-in.my)/velocity.h*15.f),0,15);
            if(arpState.arpLvl[step]!=level) {
                arpState.arpLvl[step]=level;
                writePattern("Edit arpeggiator velocity",velocityId);
            }
        }
        if(ui_.active==velocityId && in.released[0]) ui_.active=0;
        rend_.roundRect(velocity,4*s,nx::bgTop);
        const f32 fillH=(velocity.h-4*s)*arpState.arpLvl[step]/15.f;
        if(fillH>0.f)
            rend_.roundRect({velocity.x+2*s,velocity.bottom()-2*s-fillH,velocity.w-4*s,fillH},
                             2*s,nx::cyan.alpha((on?0.55f:0.22f)*dim));
        std::snprintf(label,sizeof label,"%d%%",(int)std::round(arpState.arpLvl[step]*100.0/15));
        rend_.textIn(fSmall_,velocity,label,nx::text.alpha(dim),Align::Center,0);
        if(hot || ui_.active==velocityId) {
            ui_.cursor=Cursor::ResizeV;
            ui_.tip="Drag to set step velocity. Choose Pattern velocity above to hear the bars.";
        }
    }
    rend_.textIn(fSmall_,{pattern.x+14*s,pattern.bottom()-20*s,pattern.w-28*s,18*s},
                 (int)std::lround(get(pArpVelMode))==2?"Velocity bars are active. Tie extends the previous note."
                    :"Velocity bars take effect when Velocity is set to Pattern. Tie extends the previous note.",
                 nx::muted,Align::Left,0);
    rend_.popClip();

    if(viewport.contains(in.mx,in.my) && in.wheel!=0.f) {
        st.scroll=clampv(st.scroll-in.wheel*44*s,0.f,maxScroll);
        in.wheel=0.f;
    }
    if(maxScroll>0.f) {
        const Rect rail{viewport.right()-24*s,viewport.y,24*s,viewport.h};
        const f32 thumbH=std::max(40*s,viewport.h*viewport.h/needed);
        const f32 travel=std::max(1.f,rail.h-thumbH);
        const Rect thumb{rail.x,rail.y+travel*st.scroll/maxScroll,rail.w,thumbH};
        const u64 scrollId=id(3290);
        const bool hot=ui_.setHot(scrollId,rail) && ui_.isHot(scrollId);
        if(hot && in.pressed[0]) {
            ui_.active=scrollId;
            if(!thumb.contains(in.mx,in.my))
                st.scroll=clampv((in.my-rail.y-thumbH*.5f)/travel*maxScroll,0.f,maxScroll);
            ui_.dragStart=st.scroll; ui_.dragAccum=0.f;
        }
        if(ui_.active==scrollId && in.down[0]) {
            ui_.dragAccum+=in.dy;
            st.scroll=clampv((f32)ui_.dragStart+ui_.dragAccum/travel*maxScroll,0.f,maxScroll);
        }
        if(ui_.active==scrollId && in.released[0]) ui_.active=0;
        rend_.roundRect({rail.cx()-3*s,rail.y+travel*st.scroll/maxScroll,6*s,thumbH},3*s,
                         hot || ui_.active==scrollId?nx::violetSoft:nx::muted.alpha(.6f));
        if(hot || ui_.active==scrollId) ui_.cursor=Cursor::ResizeV;
    }
}
