#include "app_internal.h"
#include <algorithm>
#include <cstdio>

namespace lat {

void App::openStudioPatternPicker(const Rect& anchor) {
    studioToolsOpen_=false;
    studioPatternPickerOpen_=true;
    studioPatternAnchor_=anchor;
    studioPatternScroll_=std::max(0,selSlot_-2)*48*win_.dpiScale();
}

void App::closeStudioPatternPicker(bool commit) {
    const u64 field=uiId(UiStudioShelf,150);
    if(ui_.editId==field) {
        if(commit && selSlot_>=0 && selSlot_<(int)ses_.scenes.size() && ui_.editBuf.find_first_not_of(" \t\r\n")!=std::string::npos) {
            auto& name=ses_.scenes[(size_t)selSlot_].name;
            const auto before=name;
            name=ui_.editBuf;
            if(name!=before) undoPointWith("rename pattern",name,before);
        }
        ui_.editId=0;ui_.active=0;ui_.textSelectAll=false;
    }
    studioPatternPickerOpen_=false;
}

Rect App::studioPatternMenuRect() const {
    const f32 s=win_.dpiScale(),W=win_.width(),H=win_.height();
    const f32 width=std::min(340*s,W-16*s);
    const f32 top=(W<880*s?92.f:54.f)*s+engineBannerH()*s+8*s;
    const f32 height=std::min(392*s,std::max(0.f,H-top-lay::statusH*s-8*s));
    return {clampv(studioPatternAnchor_.x,8*s,std::max(8*s,W-width-8*s)),
            clampv(studioPatternAnchor_.bottom()+6*s,top,std::max(top,H-height-lay::statusH*s-8*s)),width,height};
}

void App::drawStudioPatternPicker() {
    const f32 s=win_.dpiScale();
    const Rect r=studioPatternMenuRect();
    Input& in=win_.input();
    ui_.keyModal=true;
    if(!ui_.editId && !ses_.scenes.empty()) {
        if(in.keyPressed[KeyUp] || in.keyPressed[KeyDown]) {
            selectStudioPattern(clampv(selSlot_+(in.keyPressed[KeyDown]?1:-1),0,(int)ses_.scenes.size()-1));
            studioPatternScroll_=std::max(0,selSlot_-2)*48*s;
        }
        if(in.keyPressed[KeyEnter]) {closeStudioPatternPicker();return;}
    }
    rend_.roundRect({r.x-3*s,r.y+4*s,r.w+6*s,r.h+5*s},9*s,rgb(0x000000).alpha(.7f));
    rend_.roundRect(r,8*s,rgb(0x10141A));
    rend_.roundRectOutline(r,8*s,s,rgb(0x303843));
    rend_.pushClip(r);
    rend_.textIn(fBold_,{r.x+16*s,r.y+10*s,r.w-62*s,24*s},"Patterns",nx::text,Align::Left,0);
    const Rect close{r.right()-42*s,r.y+8*s,28*s,28*s};
    const bool dismissed=ui_.button(uiId(UiStudioShelf,151),close,"");
    rend_.line(close.cx()-4*s,close.cy()-4*s,close.cx()+4*s,close.cy()+4*s,s,nx::muted);
    rend_.line(close.cx()+4*s,close.cy()-4*s,close.cx()-4*s,close.cy()+4*s,s,nx::muted);
    if(selSlot_>=0 && selSlot_<(int)ses_.scenes.size()) {
        auto& name=ses_.scenes[(size_t)selSlot_].name;
        const auto before=name;
        rend_.textIn(fSmall_,{r.x+16*s,r.y+36*s,r.w-32*s,14*s},"Pattern name",nx::muted,Align::Left,0);
        const Rect field{r.x+16*s,r.y+52*s,r.w-32*s,32*s};
        if(ui_.textField(uiId(UiStudioShelf,150),field,&name,rgb(0x090B0E),nx::text,Align::Left,false)) {
            if(name.find_first_not_of(" \t\r\n")==std::string::npos) name=before;
            if(name!=before) undoPointWith("rename pattern",name,before);
        }
        if(ui_.hovered(field)) ui_.tip="Rename this pattern · Ctrl+A selects its name";
    }
    const Rect list{r.x+12*s,r.y+96*s,r.w-24*s,std::max(0.f,r.h-150*s)};
    const f32 maxScroll=std::max(0.f,(f32)ses_.scenes.size()*48*s-list.h);
    if(list.contains(in.mx,in.my)) studioPatternScroll_-=in.wheel*48*s;
    studioPatternScroll_=clampv(studioPatternScroll_,0.f,maxScroll);
    rend_.pushClip(list);
    int selected=-1;
    for(int i=0;i<(int)ses_.scenes.size();++i) {
        const Rect row{list.x,list.y+i*48*s-studioPatternScroll_,list.w,44*s};
        if(row.bottom()<=list.y || row.y>=list.bottom()) continue;
        if(ui_.button(uiId(UiStudioShelf,160+i),row,"",i==selSlot_,rgb(0x303449))) selected=i;
        char number[12];snprintf(number,sizeof number,"%02d",i+1);
        rend_.textIn(fSmall_,{row.x+8*s,row.y,28*s,row.h},number,nx::muted,Align::Center,0);
        int clips=0;for(const auto& track:ses_.tracks) clips+=track.slots[i].valid()?1:0;
        const auto name=studioPatternName(i);
        rend_.textIn(fBold_,{row.x+44*s,row.y+3*s,row.w-100*s,21*s},name.c_str(),nx::text,Align::Left,0);
        char info[48];snprintf(info,sizeof info,clips?"%d channel%s":"Empty pattern",clips,clips==1?"":"s");
        rend_.textIn(fSmall_,{row.x+44*s,row.y+23*s,row.w-64*s,18*s},info,nx::muted,Align::Left,0);
        if(i==selSlot_) rend_.roundRect({row.x+3*s,row.y+9*s,3*s,row.h-18*s},s,nx::cyan);
    }
    rend_.popClip();
    const f32 bw=(r.w-36*s)/2;
    const Rect create{r.x+16*s,r.bottom()-42*s,bw,30*s};
    if(ui_.button(uiId(UiStudioShelf,152),create,"+ New pattern")) {closeStudioPatternPicker();newStudioPattern();}
    if(ui_.button(uiId(UiStudioShelf,153),{create.right()+4*s,create.y,bw,create.h},"Clone pattern")) {
        closeStudioPatternPicker();cloneStudioPattern();
    }
    rend_.popClip();
    if(selected>=0) {selectStudioPattern(selected);closeStudioPatternPicker();}
    if(dismissed) closeStudioPatternPicker();
}
} // namespace lat
