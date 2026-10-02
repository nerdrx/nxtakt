// One workspace, existing editors. Window rectangles are logical pixels;
// only the front surface receives pointer input, including raw-hit-test views.
#include "app_internal.h"
#include <algorithm>
#include <fstream>
#include <filesystem>

namespace lat {
namespace {
std::string workspaceLayoutPath() {
    const char* config=std::getenv("XDG_CONFIG_HOME");
    return (config && *config ? std::string(config) : homeDir()+"/.config")+"/nxtakt/workspace-layout.txt";
}
}

void App::saveStudioLayout() {
    const auto path=workspaceLayoutPath();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(),ec);
    if(ec) {status_="Could not save workspace layout";return;}
    std::ofstream out(path+".tmp");
    out<<"1 "<<showChannelRack_<<' '<<showStudioMixer_<<' '<<showBrowser_<<'\n';
    for(const auto& w:studioWindows_) out<<w.bounds.x<<' '<<w.bounds.y<<' '<<w.bounds.w<<' '<<w.bounds.h<<' '<<w.minimized<<' '<<w.maximized<<'\n';
    for(int i:studioOrder_) out<<i<<' ';
    out<<'\n';out.close();
    if(!out) {status_="Could not save workspace layout";return;}
    std::filesystem::rename(path+".tmp",path,ec);
    if(ec) status_="Could not save workspace layout";
    if(std::getenv("NXTAKT_DEBUG_PROBE")) LOGI("NXTAKT_DEBUG_PROBE: workspace layout saved rack=%d mixer=%d",showChannelRack_,showStudioMixer_);
    studioLayoutDirty_=false;
}


void App::activateStudioWindow(int index) {
    auto pos=std::find(studioOrder_.begin(),studioOrder_.end(),index);
    if(pos==studioOrder_.end()) return;
    std::rotate(pos,pos+1,studioOrder_.end());
    studioFocus_=index;
    studioWindows_[index].minimized=false;
    studioLayoutDirty_=true;
}

Rect App::studioWindowRect(int index) const {
    const auto& w = studioWindows_[index];
    const f32 s = win_.dpiScale();
    if (w.maximized) return studioArea_;
    Rect r{w.bounds.x*s, w.bounds.y*s, w.bounds.w*s, w.bounds.h*s};
    if (w.minimized) r.h = 34*s;
    return r;
}

void App::prepareStudioWindows(const Rect& area) {
    const bool firstLayout=!studioLayoutLoaded_;
    studioArea_ = area;
    const f32 s = win_.dpiScale();
    const Rect logical{area.x/s, area.y/s, area.w/s, area.h/s};
    const Rect defaults[] = {
        {logical.x+80, logical.y+std::max(100.f,logical.h-487), 660, 330},
        {logical.x+50, logical.y+70, 860, 440},
        {logical.x+(logical.w>1450?logical.w*.44f:std::max(20.f,logical.w-662)), logical.y+std::max(100.f,logical.h-650), 650, 500},
        {logical.right()-392, logical.y+22, 380, 500}
    };
    if(!studioLayoutLoaded_) {
        studioLayoutLoaded_=true;
        std::ifstream file(workspaceLayoutPath());
        int version=0;bool rack=true,mixer=true,browser=true;
        std::array<StudioWindow,4> loaded{};
        std::array<int,4> order{};
        bool valid=bool(file>>version>>rack>>mixer>>browser) && version==1;
        for(auto& w:loaded) {
            valid=bool(file>>w.bounds.x>>w.bounds.y>>w.bounds.w>>w.bounds.h>>w.minimized>>w.maximized) && valid;
            valid=valid && std::isfinite(w.bounds.x) && std::isfinite(w.bounds.y) && std::isfinite(w.bounds.w) && std::isfinite(w.bounds.h) && w.bounds.w>0 && w.bounds.h>0;
            w.initialized=true;
        }
        bool seen[4]{};
        for(int& i:order) {
            valid=bool(file>>i) && valid;
            if(i<0 || i>=4 || seen[clampv(i,0,3)]) valid=false;
            else seen[i]=true;
        }
        if(valid) {studioWindows_=loaded;studioOrder_=order;showChannelRack_=rack;showStudioMixer_=mixer;showBrowser_=browser;}
    }
    const f32 minW[] = {640, 760, 600, 340};
    const f32 minH[] = {240, 320, 440, 340};
    for (int i=0;i<4;++i) {
        auto& w=studioWindows_[i];
        if(!w.initialized) {w.bounds=defaults[i];w.initialized=true;}
        w.bounds.w=clampv(w.bounds.w,std::min(minW[i],logical.w),logical.w);
        w.bounds.h=clampv(w.bounds.h,std::min(minH[i],logical.h),logical.h);
        w.bounds.x=clampv(w.bounds.x,logical.x,logical.right()-w.bounds.w);
        w.bounds.y=clampv(w.bounds.y,logical.y,logical.bottom()-w.bounds.h);
    }
    const bool visible[]={showChannelRack_,showDetail_ && (detailTab_==DetailTab::Clip || !spectraOpenUid_ || spectraForced_),
                          spectraOpenUid_ && !spectraForced_,showAudioSettings_};
    for(int i=0;i<4;++i) {
        if(visible[i] && !studioWasVisible_[i] && !firstLayout) {
            auto pos=std::find(studioOrder_.begin(),studioOrder_.end(),i);
            std::rotate(pos,pos+1,studioOrder_.end());
            studioFocus_=i;
            studioWindows_[i].minimized=false;
        }
        studioWasVisible_[i]=visible[i];
    }
    studioRawInput_=win_.input();
    if(studioCapture_>=0 && !visible[studioCapture_]) {studioCapture_=-1;studioGesture_=0;}
    studioPointerOwner_=-1;
    for(int i:studioOrder_) if(visible[i] && studioWindowRect(i).contains(studioRawInput_.mx,studioRawInput_.my)) studioPointerOwner_=i;
    if(studioToolsOpen_ && Rect{area.x,area.y,area.w,112*s}.contains(studioRawInput_.mx,studioRawInput_.my))
        studioPointerOwner_=-1; // the expanded toolbar paints above floating tools
    if(studioCapture_>=0) studioPointerOwner_=studioCapture_;
    if(studioRawInput_.pressed[0]) {
        studioFocus_=studioPointerOwner_;
        if(std::getenv("NXTAKT_DEBUG_PROBE")) LOGI("NXTAKT_DEBUG_PROBE: workspace focus %d",studioFocus_);
        if(studioPointerOwner_>=0) {
            auto pos=std::find(studioOrder_.begin(),studioOrder_.end(),studioPointerOwner_);
            std::rotate(pos,pos+1,studioOrder_.end());
            studioLayoutDirty_=true;
            studioCapture_=studioPointerOwner_;
        }
    }
    if(studioFocus_>=0 && !visible[studioFocus_]) studioFocus_=-1;
}

void App::studioInput(int layer) {
    Input& in=win_.input();
    in=studioRawInput_;
    if(layer==-2) return;
    if(studioPointerOwner_!=layer) {
        in.mx=in.my=-1e6f;in.dx=in.dy=in.wheel=0;in.dblClick=false;
        for(int i=0;i<3;++i) in.pressed[i]=in.released[i]=in.down[i]=false;
    }
    if(layer!=studioFocus_) {
        for(int i=0;i<KeyCount;++i) in.keyPressed[i]=in.keyStarted[i]=false;
        in.textInput.clear();
    }
}

void App::drawStudioWindows(const Rect&) {
    const f32 s=win_.dpiScale();
    const std::string soundOwner=ownerName(focusedSpectraOwner());
    const std::string spectraTitle=soundOwner=="Spectra"?"Spectra":"Spectra / "+(soundOwner.rfind("Spectra ",0)==0?soundOwner.substr(8):soundOwner);
    const char* titles[]={"Channel Rack",detailTab_==DetailTab::Clip?"Piano / Sample editor":"Instrument chain",spectraTitle.c_str(),"Audio settings"};
    const bool visible[]={showChannelRack_,showDetail_ && (detailTab_==DetailTab::Clip || !spectraOpenUid_ || spectraForced_),
                          spectraOpenUid_ && !spectraForced_,showAudioSettings_};
    for(int index:studioOrder_) {
        if(!visible[index]) continue;
        studioInput(index);
        Input& in=win_.input();
        auto& w=studioWindows_[index];
        Rect r=studioWindowRect(index);
        if(studioCapture_==index && studioGesture_ && in.down[0] && !w.maximized) {
            if(studioGesture_==1) {w.bounds.x+=in.dx/s;w.bounds.y+=in.dy/s;}
            else {w.bounds.w+=in.dx/s;w.bounds.h+=in.dy/s;}
            studioLayoutDirty_=true;
            // Clamping is applied at the beginning of every frame.
            r=studioWindowRect(index);
        }
        rend_.roundRect({r.x-3*s,r.y+4*s,r.w+6*s,r.h+6*s},7*s,rgb(0x000000).alpha(.65f));
        rend_.roundRect(r,6*s,pal::panel);
        rend_.roundRectOutline(r,6*s,s,studioFocus_==index?rgb(0x39414D):pal::divider);
        rend_.pushClip(r);
        ui_.setHot(uiId(UiStudioWindow,index,0),r); // shields previous paint layers
        const Rect title{r.x+1*s,r.y+1*s,r.w-2*s,32*s};
        rend_.rect(title,studioFocus_==index?rgb(0x10141A):rgb(0x0B0E12));
        const Rect drag{title.x,title.y,title.w-108*s,title.h};
        if(drag.contains(in.mx,in.my)) {
            ui_.cursor=Cursor::Grab;
            ui_.tip="Drag to move - double-click to maximize";
            if(in.dblClick) {w.maximized=!w.maximized;w.minimized=false;studioLayoutDirty_=true;}
            else if(in.pressed[0]) studioGesture_=1;
        }
        for(int row=0;row<3;++row) for(int col=0;col<2;++col)
            rend_.circle(title.x+(10+col*5)*s,title.y+(10+row*5)*s,1.1f*s,nx::muted);
        rend_.textIn(fBody_,{drag.x+28*s,drag.y,drag.w-28*s,drag.h},titles[index],nx::text,Align::Left,0);
        if(ui_.segButton(uiId(UiStudioWindow,index,1),{title.right()-102*s,title.y+2*s,30*s,28*s},false,pal::accent)) {w.minimized=!w.minimized;if(w.minimized) {ui_.editId=0;ui_.active=0;}}
        if(ui_.segButton(uiId(UiStudioWindow,index,2),{title.right()-68*s,title.y+2*s,30*s,28*s},false,pal::accent)) {w.maximized=!w.maximized;w.minimized=false;}
        if(ui_.segButton(uiId(UiStudioWindow,index,3),{title.right()-34*s,title.y+2*s,30*s,28*s},false,pal::accent)) {
            ui_.editId=0;ui_.active=0;
            if(index==0) showChannelRack_=false;
            else if(index==3) showAudioSettings_=false;
            else if(index==2) closeFocusedSpectra();
            else showDetail_=false;
        }
        // Native window glyphs keep the chrome quiet, with full button hit areas.
        const f32 gy=title.cy();
        rend_.line(title.right()-92*s,gy+3*s,title.right()-82*s,gy+3*s,s,nx::muted);
        rend_.roundRectOutline({title.right()-58*s,gy-5*s,10*s,10*s},s,s,nx::muted);
        rend_.line(title.right()-24*s,gy-5*s,title.right()-14*s,gy+5*s,s,nx::muted);
        rend_.line(title.right()-24*s,gy+5*s,title.right()-14*s,gy-5*s,s,nx::muted);
        rend_.hairlineH(title.x,title.right(),title.bottom(),nx::hairlineInk,s);
        if(!w.minimized) {
            const Rect grip{r.right()-16*s,r.bottom()-16*s,16*s,16*s};
            if(!w.maximized && grip.contains(in.mx,in.my)) {
                ui_.cursor=Cursor::ResizeH;
                ui_.tip="Drag to resize this tool";
                if(in.pressed[0]) studioGesture_=2;
            }
            const Rect content{r.x+5*s,title.bottom()+4*s,r.w-10*s,r.bottom()-title.bottom()-9*s};
            // Title drag and resize gestures never reach editor controls.
            if(studioGesture_) {in.mx=in.my=-1e6f;for(int b=0;b<3;++b) in.pressed[b]=in.down[b]=in.released[b]=false;}
            rend_.pushClip(content);
            if(index==0) drawChannelRack(content);
            else if(index==1) drawDetailPanel(content);
            else if(index==2) drawFocusedSpectra(content);
            else drawAudioSettings(content);
            rend_.popClip();
            studioInput(index);
            for(int i=0;i<3;++i) rend_.line(r.right()-(4+i*4)*s,r.bottom()-4*s,r.right()-4*s,r.bottom()-(4+i*4)*s,s,pal::textFaint);
        }
        rend_.popClip();
    }
    if(!studioRawInput_.down[0]) {studioCapture_=-1;studioGesture_=0;if(studioLayoutDirty_) saveStudioLayout();}
    studioInput(-2);
}

} // namespace lat
