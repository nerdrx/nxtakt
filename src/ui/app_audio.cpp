#include "app.h"
#include "../audio/audio_settings.h"
#include "../audio/audio_routing.h"
#include <algorithm>
#include <cctype>
#include <cstdio>

namespace lat {
void App::drawAudioSettings(const Rect& bounds) {
    static AudioSettings settings;
    static AudioRouteSnapshot routes;
    static std::vector<AudioPortChoice> alsaInputs,alsaOutputs;
    static int tab=0,picker=-1;
    static std::string filter,message;
    static f32 scroll=0;
    const f32 s=win_.dpiScale();
    ui_.keyModal=true;
    auto id=[](int n){return uiId(UiAudioSettings,n);};
    if(refreshAudioSettings_) {
        settings=loadAudioSettings();
        routes=inspectAudioRoutes(eng_.enginePid());
        alsaInputs=listAlsaDevices(true);alsaOutputs=listAlsaDevices(false);
        tab=eng_.driverName() && std::string(eng_.driverName()).find("ALSA")!=std::string::npos?1:0;
        picker=-1;filter.clear();message.clear();scroll=0;
        refreshAudioSettings_=false;
    }
    Rect panel{bounds.x+24*s,bounds.y+16*s,std::min(900*s,bounds.w-48*s),bounds.h-32*s};
    panel.x=bounds.cx()-panel.w*.5f;
    rend_.roundRect(panel,12*s,nx::panel);
    Rect back{panel.x+20*s,panel.y+16*s,110*s,32*s};
    if(ui_.button(id(0),back,picker>=0?"Back":"Back to music")) {
        if(picker>=0) {picker=-1;filter.clear();scroll=0;ui_.editId=0;}
        else showAudioSettings_=false;
    }
    rend_.textIn(fBold_,{back.right()+16*s,back.y,panel.w-170*s,32*s},"Audio input / output",nx::text,Align::Left,0);
    if(win_.input().keyPressed[KeyEscape] && !ui_.editId) {
        if(picker>=0) picker=-1;
        else showAudioSettings_=false;
        win_.input().keyPressed[KeyEscape]=false;
    }
#ifdef _WIN32
    rend_.textIn(fSmall_,{panel.x+20*s,back.bottom()+24*s,panel.w-40*s,28*s},
                 "Windows currently uses the system default audio device. Change it in Windows sound settings.",nx::muted,Align::Left,0);
    return;
#endif
    const f32 left=panel.x+20*s,width=panel.w-40*s;
    if(picker>=0) {
        std::vector<AudioPortChoice> choices;
        if(tab==0) {
            choices={{"","Automatic (first physical port)"},{"-","Disconnected"}};
            const auto& available=picker<2?routes.outputs:routes.inputs;
            choices.insert(choices.end(),available.begin(),available.end());
        } else choices=picker==0?alsaOutputs:alsaInputs;
        const std::string current=tab==0?settings.jackPorts[picker]:picker==0?settings.alsaOutput:settings.alsaInput;
        const char* titles[]={"Choose output left","Choose output right","Choose input left","Choose input right"};
        rend_.textIn(fBold_,{left,back.bottom()+16*s,width,28*s},tab==0?titles[picker]:picker==0?"Choose ALSA output":"Choose ALSA input",nx::text,Align::Left,0);
        Rect search{left,back.bottom()+52*s,width,34*s};
        ui_.textField(id(1),search,&filter,nx::panel2,nx::text,Align::Left,false);
        std::string query=ui_.liveText(id(1))?*ui_.liveText(id(1)):filter;
        auto lower=[](std::string value){for(char& c:value)c=(char)std::tolower((unsigned char)c);return value;};
        query=lower(query);
        if(query.empty() && !ui_.liveText(id(1))) rend_.textIn(fSmall_,search,"Search devices and ports",nx::muted,Align::Left,10*s);
        std::vector<int> matches;
        for(size_t i=0;i<choices.size();++i)
            if(query.empty() || lower(choices[i].label+" "+choices[i].value).find(query)!=std::string::npos) matches.push_back((int)i);
        Rect list{left,search.bottom()+12*s,width,panel.bottom()-search.bottom()-32*s};
        const f32 rowH=48*s;
        if(list.contains(win_.input().mx,win_.input().my)) scroll-=win_.input().wheel*rowH;
        scroll=clampv(scroll,0.f,std::max(0.f,rowH*matches.size()-list.h));
        rend_.pushClip(list);
        for(size_t row=0;row<matches.size();++row) {
            const auto& choice=choices[matches[row]];
            Rect item{list.x,list.y+row*rowH-scroll,list.w-6*s,42*s};
            if(item.bottom()<list.y || item.y>list.bottom()) continue;
            if(ui_.button(id(100+matches[row]),item,choice.label.c_str(),choice.value==current)) {
                if(tab==0)settings.jackPorts[picker]=choice.value;
                else if(picker==0)settings.alsaOutput=choice.value;
                else settings.alsaInput=choice.value;
                picker=-1;filter.clear();scroll=0;ui_.editId=0;
                message="Selection changed. Press Apply and save to use it.";
                break;
            }
            if(ui_.hovered(item))ui_.tip=choice.value;
        }
        if(matches.empty())rend_.textIn(fSmall_,{list.x,list.y,list.w,32*s},"No matching devices. Go back and Refresh to scan again.",nx::muted,Align::Left,0);
        rend_.popClip();
        if(rowH*matches.size()>list.h) {
            const f32 thumb=std::max(20*s,list.h*list.h/(rowH*matches.size()));
            rend_.roundRect({list.right()-3*s,list.y+(list.h-thumb)*scroll/(rowH*matches.size()-list.h),3*s,thumb},1.5f*s,nx::violetSoft);
        }
        return;
    }
    static const char* tabs[]={"JACK / PipeWire","ALSA fallback"};
    Rect tabBox{left,back.bottom()+20*s,width,34*s};
    if(ui_.tabPill(id(2),tabBox,tabs,2,&tab))message.clear();
    char info[160];
    std::snprintf(info,sizeof info,"Active: %s  |  %.1f kHz  |  %d samples",eng_.driverName()?eng_.driverName():"No audio",eng_.driverSampleRate()/1000.0,eng_.driverBufferSize());
    rend_.textIn(fSmall_,{left,tabBox.bottom()+8*s,width,26*s},info,nx::muted,Align::Left,0);
    f32 y=tabBox.bottom()+42*s;
    const char* labels[]={"Output left","Output right","Input left","Input right"};
    const int count=tab==0?4:2;
    for(int i=0;i<count;++i) {
        const std::string value=tab==0?settings.jackPorts[i]:i==0?settings.alsaOutput:settings.alsaInput;
        const auto& choices=tab==0?(i<2?routes.outputs:routes.inputs):(i==0?alsaOutputs:alsaInputs);
        std::string label=value;
        if(tab==0 && value.empty())label="Automatic";
        else if(tab==0 && value=="-")label="Disconnected";
        else {
            bool found=false;
            for(const auto& choice:choices)if(choice.value==value){label=choice.label;found=true;break;}
            if(!found)label="Unavailable: "+value;
        }
        rend_.textIn(fSmall_,{left,y,118*s,34*s},tab==0?labels[i]:i==0?"Output device":"Input device",nx::muted,Align::Left,0);
        Rect pick{left+126*s,y,width-126*s,34*s};
        if(ui_.button(id(10+i),pick,label.c_str())){picker=i;filter.clear();scroll=0;}
        if(tab==0) {
            const std::string actual="Connected: "+(routes.current[i].empty() || routes.current[i]=="-"?std::string("none"):routes.current[i]);
            rend_.textIn(fSmall_,{pick.x,y+36*s,pick.w,20*s},actual.c_str(),nx::muted,Align::Left,0);
        }
        y+=66*s;
    }
    const char* note=tab==0?"Apply changes this engine's routing immediately. Sample rate and buffer size belong to JACK / PipeWire.":
        "ALSA device changes take effect when the audio engine next starts. They do not switch the active backend.";
    rend_.textIn(fSmall_,{left,y+4*s,width,26*s},note,nx::muted,Align::Left,0);
    y+=44*s;
    Rect apply{left,y,148*s,36*s}, refresh{left+160*s,y,108*s,36*s};
    if(ui_.button(id(20),apply,"Apply and save",false,nx::violet)) {
        std::string error;
        if(tab==0 && !applyAudioRoutes(eng_.enginePid(),settings.jackPorts,error))message=error;
        else {
            const bool saved=saveAudioSettings(settings);
            message=saved?(tab==0?"Routing applied and saved.":"ALSA devices saved. They apply on the next audio engine start."):
                          (tab==0?"Routing applied for this session, but preferences could not be saved.":"Could not save audio preferences.");
            if(tab==0)routes=inspectAudioRoutes(eng_.enginePid());
        }
    }
    if(ui_.button(id(21),refresh,"Refresh")) {
        routes=inspectAudioRoutes(eng_.enginePid());alsaInputs=listAlsaDevices(true);alsaOutputs=listAlsaDevices(false);
        message="Available ports refreshed. Unsaved selections kept.";
    }
    if(!message.empty())rend_.textIn(fSmall_,{left,y+46*s,width,26*s},message.c_str(),nx::text,Align::Left,0);
    if(tab==0 && !routes.error.empty())rend_.textIn(fSmall_,{left,y+76*s,width,26*s},routes.error.c_str(),nx::amber,Align::Left,0);
}
} // namespace lat
