// Focused Spectra workspace. Included by app_spectra.cpp so the established
// wavetable drawing/state tools remain the same implementation.
namespace {
struct SpectraStudioState {
    u64 uid = 0;
    int page = 0, category = 0, selected = -1, popup = -1, assignDest = -1;
    f32 scroll = 0, presetScroll = 0, advancedScroll = 0;
    bool browser = false, files = false, motionTools = false, soundTools = false, toolsOpen = false, favoritesOnly = false;
    f64 variationAmount = 35;
    u32 variationSeed = 0x4e585350;
    std::string query, lastQuery, saveName, baselineState;
    std::vector<f32> baseline;
    std::vector<std::string> options;
} g_studio;
std::string studioLower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
}

void App::addSpectraTrack() {
    if(ses_.tracks.size()>=kMaxTracks) {status_="Track limit reached";return;}
    if(!closeFocusedSpectra()) return;
    if(g_edit.open && g_edit.anyDirty()) {status_="Apply or discard the wavetable drawing first";return;}
    stopPreviews();
    if(g_edit.open) {
        ChainOwner co=chainOwner(devOwner_);
        const int index=co.devices?spectraOpenIdx(*co.devices):-1;
        if(index>=0) wtCancelPreview((*co.devices)[index].inst.get(),g_edit.osc);
        g_edit.release();
    }
    std::vector<PluginDesc> stock;
    detail::scanInternal(stock);
    for(const auto& desc:stock) if(desc.uri=="nxtakt:spectra") {
        undoPoint("add Spectra track");
        addTrack();
        const int track=(int)ses_.tracks.size()-1;
        ses_.tracks[track].name="Spectra";ses_.tracks[track].colorIdx=11;
        addDevice(track,desc);
        if(ses_.tracks[track].devices.empty()) return;
        if(ses_.scenes.empty()) addScene();
        const int slot=clampv(selSlot_,0,(int)ses_.scenes.size()-1);
        createMidiClip(track,slot,false);
        ses_.tracks[track].slots[slot].name="Synth pattern";
        selectTrack(track);selectChainOwner(track);selDevice_=0;
        spectraOpenUid_=ses_.tracks[track].devices[0].uid;
        showDetail_=true;detailTab_=DetailTab::Devices;
        activateStudioWindow(2);
        status_="Spectra ready - choose a preset, then play your keyboard";
        return;
    }
}

int App::focusedSpectraOwner() {
    for(int t=0;t<(int)ses_.tracks.size();++t) {
        auto co=chainOwner(t);
        if(co.devices && spectraOpenIdx(*co.devices)>=0) return t;
    }
    for(int r=0;r<kMaxReturns;++r) {
        auto co=chainOwner(ownReturn(r));
        if(co.devices && spectraOpenIdx(*co.devices)>=0) return ownReturn(r);
    }
    auto master=chainOwner(kOwnMaster);
    if(master.devices && spectraOpenIdx(*master.devices)>=0) return kOwnMaster;
    return kMaxTracks; // invalid chain owner
}

bool App::closeFocusedSpectra() {
    if (!spectraOpenUid_) return true;
    if (g_edit.open && g_edit.anyDirty()) {
        status_="Apply or discard the wavetable drawing before closing";
        return false;
    }
    const int ownerKey=(int)(spectraOpenUid_ & 0xffff);
    if(ui_.editId==uiId(UiSpectraStudio,8,ownerKey)) g_studio.query=ui_.editBuf;
    if(ui_.editId==uiId(UiSpectraStudio,10,ownerKey)) g_studio.saveName=ui_.editBuf;
    ui_.editId=0;ui_.active=0;g_studio.popup=-1;
    ChainOwner co=chainOwner(focusedSpectraOwner());
    const int idx=co.devices?spectraOpenIdx(*co.devices):-1;
    if(g_edit.open && idx>=0) wtCancelPreview((*co.devices)[idx].inst.get(),g_edit.osc);
    g_edit.release();
    stopPreviews();
    spectraOpenUid_=0;
    return true;
}

bool App::drawFocusedSpectra(const Rect& bounds) {
    if (!spectraOpenUid_ || spectraForced_) return false;
    const int owner=focusedSpectraOwner();
    ChainOwner co=chainOwner(owner);
    const int idx=co.devices?spectraOpenIdx(*co.devices):-1;
    if(idx<0) {closeFocusedSpectra();return false;}
    reclaimRacks();
    const int savedOwner=devOwner_, savedSelection=selTrack_;
    devOwner_=owner;
    drawSpectraStudio(bounds,(*co.devices)[idx]);
    // Audition intentionally selects/arms this instrument. Ordinary editing
    // keeps the main workspace selection and its device chain independent.
    if(selTrack_==savedSelection) devOwner_=savedOwner;
    return true;
}

void App::drawSpectraStudio(const Rect& r, DeviceModel& dm) {
    auto* inst = dm.inst.get();
    if (!inst) return;
    auto& st = g_studio;
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    const int pc = inst->paramCount();
    static PresetFavorites favorites([] {
        const char* xdg = std::getenv("XDG_CONFIG_HOME");
        return (xdg && *xdg ? std::string(xdg) : homeDir()+"/.config") + "/nxtakt/spectra-favorites.txt";
    }());
    auto favoriteKey = [&](int p) {
        return std::string(p < inst->factoryPresetCount() ? "factory:" : "user:") + presetNameOf(*inst,p);
    };
    if (st.uid != dm.uid) {
        st = SpectraStudioState{};
        st.uid = dm.uid;
        st.variationSeed ^= (u32)(nowSeconds()*1000.0) ^ (u32)dm.uid;
        g_drop.open = false;
        if (g_edit.open) g_edit.release();
        // A restored project may be an edited sound, so never call it Init.
        spectraPreset_ = -1;
    }
    auto id = [&](int n) { return uiId(UiSpectraStudio, n, (int)(dm.uid & 0xffff)); };
    auto get = [&](int p) { return p >= 0 && p < pc ? inst->getParam(p) : 0.f; };
    auto commit = [&](int p, f32 value) {
        if (p < 0 || p >= pc) return;
        undoPoint(inst->paramInfo(p).name.c_str());
        inst->setParam(p, value);
        if (ownIsTrack(devOwner_))
            autoCapture(addr::deviceParam(ses_.tracks[devOwner_].uid, dm.uid,
                        inst->paramInfo(p).id), value, id(100 + p));
        if (probeOn()) LOGI("NXTAKT_DEBUG_PROBE: spectra studio param %d -> %g", p, (double)value);
    };
    auto snapshot = [&] {
        st.baseline.resize((size_t)pc);
        for (int p = 0; p < pc; ++p) st.baseline[(size_t)p] = get(p);
        st.baselineState = inst->stateString();
    };
    auto load = [&](int p) {
        if (p < 0 || p >= inst->presetCount()) return;
        if (g_edit.open && g_edit.anyDirty()) {
            status_ = "Apply or discard the wavetable drawing before loading a preset";
            return;
        }
        stopPreviews();
        if(g_edit.open) {wtCancelPreview(inst,g_edit.osc);g_edit.release();}
        undoPoint("load Spectra preset");
        inst->loadPreset(p);
        st.selected = spectraPreset_ = p;
        snapshot();
        status_ = std::string("Spectra: ") + presetNameOf(*inst, p);
    };
    auto matchesPreset = [&](int p) {
        const char* name=presetNameOf(*inst,p);
        const int category=p>=inst->factoryPresetCount()?8:presetCatOf(name)+1;
        if(st.category && category!=st.category) return false;
        if(st.favoritesOnly && !favorites.contains(favoriteKey(p))) return false;
        const std::string query=studioLower(ui_.editId==id(8)?ui_.editBuf:st.query);
        return query.empty() || studioLower(name).find(query)!=std::string::npos;
    };
    auto stepPreset = [&](int direction) {
        const int count=inst->presetCount();
        const int start=st.selected<0?(direction>0?-1:0):st.selected;
        for(int step=1;step<=count;++step) {
            const int p=((start+step*direction)%count+count)%count;
            if(matchesPreset(p)) {load(p);return;}
        }
        status_="No presets match the current filters";
    };
    bool modified = st.selected >= 0 && st.baseline.size() == (size_t)pc;
    if (modified) {
        modified = inst->stateString() != st.baselineState;
        for (int p = 0; p < pc && !modified; ++p) modified = get(p) != st.baseline[(size_t)p];
    }
    auto finishSidebarEdit = [&] {
        if(ui_.editId==id(8)) st.query=ui_.editBuf;
        else if(ui_.editId==id(10)) st.saveName=ui_.editBuf;
        else return;
        ui_.editId=0; ui_.active=0;
    };
    const f32 realMx = in.mx, realMy = in.my;
    if(studioFocus_!=2) st.popup=-1;
    const bool popupWasOpen = st.popup >= 0;
    if (popupWasOpen) { in.mx = in.my = -1e6f; ui_.keyModal = true; }
    rend_.rect(r, nx::bgTop);
    const Rect header{r.x + 8*s, r.y + 3*s, r.w - 16*s, 30*s};
    const f32 tailW=222*s;
    const Rect patch{header.x,header.y,std::max(120*s,header.w-tailW),28*s};
    const std::string patchName=st.selected>=0?presetNameOf(*inst,st.selected):"Current patch";
    if(ui_.button(id(22),patch,"",st.browser)) {finishSidebarEdit();st.browser=!st.browser;st.files=false;}
    rend_.textIn(fBody_,{patch.x+9*s,patch.y,patch.w-28*s,patch.h},patchName.c_str(),nx::text,Align::Left,0);
    rend_.textIn(fSmall_,{patch.right()-22*s,patch.y,16*s,patch.h},"v",nx::muted,Align::Center,0);
    f32 hx=patch.right()+5*s;
    if(ui_.button(id(1),{hx,header.y,24*s,28*s},"<")) stepPreset(-1);
    hx+=27*s;
    if(ui_.button(id(2),{hx,header.y,24*s,28*s},">")) stepPreset(1);
    hx+=27*s;
    const bool compactWindow=r.w<900*s;
    const bool compactPickerOpen=compactWindow && (st.browser || st.files);
    const char* presetAction=compactPickerOpen?"Sound":"Presets";
    if(ui_.button(id(3),{hx,header.y,52*s,28*s},presetAction,st.browser)) {
        finishSidebarEdit();
        if(compactPickerOpen) {st.browser=false;st.files=false;}
        else {st.browser=true;st.files=false;}
    }
    hx+=55*s;
    if(ui_.button(id(12),{hx,header.y,46*s,28*s},"Files",st.files)) {finishSidebarEdit();st.files=!st.files;st.browser=false;}
    hx+=49*s;
    if(ui_.button(id(4),{hx,header.y,std::max(34*s,header.right()-hx),28*s},"Chain")) {
        if(g_edit.open && g_edit.anyDirty()) status_="Apply or discard the wavetable drawing before closing";
        else {finishSidebarEdit();stopPreviews();spectraOpenUid_=0;if(g_edit.open)wtCancelPreview(inst,g_edit.osc);g_edit.release();showDetail_=true;detailTab_=DetailTab::Devices;activateStudioWindow(1);}
    }
    const Rect nav{r.x + 8*s, header.bottom()+4*s, r.w-16*s, 27*s};
    const char* tabs[]={"Sound","Mod","Effects","Arp","Tables"};
    int pageTab=st.page==1?2:st.page==2?1:st.page;
    ui_.tabPill(id(5),{nav.x,nav.y,std::max(240*s,nav.w-78*s),nav.h},tabs,5,&pageTab);
    const int page=pageTab==1?2:pageTab==2?1:pageTab;
    if(page!=st.page) {
        if(g_edit.open && g_edit.anyDirty()) status_="Apply or discard the wavetable drawing before changing pages";
        else {st.page=page;st.soundTools=false;st.scroll=st.advancedScroll=0;if(g_edit.open)wtCancelPreview(inst,g_edit.osc);g_edit.release();}
    }
    if(ui_.button(id(21),{nav.right()-72*s,nav.y,72*s,nav.h},"Tools",st.toolsOpen)) st.toolsOpen=!st.toolsOpen;
    Rect content{r.x + 16*s, nav.bottom() + 14*s, r.w - 32*s, r.bottom() - nav.bottom() - 28*s};
    if(r.w<900*s && st.files && drag_.kind==DragState::Kind::BrowserFile && drag_.armed)
        st.files=false;
    const bool compactPicker=r.w<900*s && (st.browser || st.files);
    if(st.files && compactPicker) drawBrowser(content);
    else if(st.files) {
        const f32 bw=r.w<1150*s?220*s:254*s;
        drawBrowser({content.x,content.y,bw,content.h});
        content.x+=bw+14*s;content.w-=bw+14*s;
    }
    // Compact windows use the whole work area for browsing; the header and
    // page controls remain visible so the user can return without a modal.
    if(st.browser && compactPicker && !st.files) {
        rend_.roundRect(content,6*s,nx::panel);
        char count[64];snprintf(count,sizeof count,"%d SOUNDS",inst->presetCount());
        const f32 top=content.y+5*s;
        rend_.textIn(fSmall_,{content.x+8*s,top,72*s,26*s},count,nx::cyan,Align::Left,0);
        if(ui_.button(id(16),{content.x+80*s,top,78*s,26*s},"Favorites",st.favoritesOnly)) {
            st.favoritesOnly=!st.favoritesOnly;st.presetScroll=0;
        }
        const Rect search{content.x+164*s,top,content.w-172*s,26*s};
        const std::string oldQuery=st.query;
        ui_.textField(id(8),search,&st.query,nx::bgTop,nx::text,Align::Left,false);
        if(st.query.empty() && ui_.editId!=id(8)) rend_.textIn(fSmall_,search,"Search presets...",nx::muted,Align::Left,8*s);
        if(oldQuery!=st.query) st.presetScroll=0;
        static const char* cats[]={"All","Bass","Lead","Pad","Keys","Pluck","FX","Sequence","User"};
        const f32 catY=top+31*s, catW=(content.w-16*s)/5.f;
        for(int c=0;c<9;++c) {
            const int row=c/5,col=c%5;
            if(ui_.button(id(50+c),{content.x+8*s+col*catW,catY+row*24*s,catW-4*s,22*s},cats[c],st.category==c)) {
                st.category=c;st.presetScroll=0;
            }
        }
        const f32 listY=catY+50*s;
        const Rect save{content.x+8*s,content.bottom()-34*s,content.w-16*s,28*s};
        const Rect list{content.x+6*s,listY,content.w-12*s,std::max(0.f,save.y-listY-6*s)};
        std::vector<int> matches;
        const std::string query=studioLower(ui_.editId==id(8)?ui_.editBuf:st.query);
        if(query!=st.lastQuery) {st.presetScroll=0;st.lastQuery=query;}
        for(int p=0;p<inst->presetCount();++p) if(matchesPreset(p)) matches.push_back(p);
        const f32 rowH=32*s;
        const f32 maxScroll=std::max(0.f,matches.size()*rowH-list.h);
        if(list.contains(in.mx,in.my) && in.wheel) {st.presetScroll-=in.wheel*rowH*3;in.wheel=0;}
        st.presetScroll=clampv(st.presetScroll,0.f,maxScroll);
        rend_.pushClip(list);
        for(size_t i=0;i<matches.size();++i) {
            const Rect row{list.x,list.y+i*rowH-st.presetScroll,list.w-4*s,rowH-2*s};
            if(row.bottom()<list.y || row.y>list.bottom()) continue;
            const int p=matches[i];
            const Rect star{row.right()-28*s,row.y,28*s,row.h};
            if(ui_.button(id(1000+p),{row.x,row.y,row.w-32*s,row.h},presetNameOf(*inst,p),p==st.selected)) load(p);
            const bool saved=favorites.contains(favoriteKey(p));
            if(ui_.button(id(5000+p),star,"",saved))
                status_=favorites.toggle(favoriteKey(p))?(saved?"Removed from favorites":"Added to favorites"):"Could not save favorites - check your configuration folder";
            if(ui_.hovered(star)) ui_.tip=saved?"Remove this preset from favorites":"Add this preset to favorites";
            for(int point=0;point<10;++point) {
                const f32 a=point*.62831853f-1.57079633f,b=(point+1)*.62831853f-1.57079633f;
                const f32 ra=(point%2?3.8f:8.f)*s,rb=(point%2?8.f:3.8f)*s;
                const f32 ax=star.cx()+std::cos(a)*ra,ay=star.cy()+std::sin(a)*ra;
                const f32 bx=star.cx()+std::cos(b)*rb,by=star.cy()+std::sin(b)*rb;
                if(saved) rend_.triangle(star.cx(),star.cy(),ax,ay,bx,by,nx::text);
                else rend_.line(ax,ay,bx,by,1.3f*s,nx::muted);
            }
        }
        if(matches.empty()) rend_.textIn(fSmall_,list,st.favoritesOnly?"No matching favorites":"No matching sounds",nx::muted,Align::Center,0);
        rend_.popClip();
        if(maxScroll>0) {
            const f32 thumb=std::max(18*s,list.h*list.h/(matches.size()*rowH));
            rend_.roundRect({list.right()-3*s,list.y+(list.h-thumb)*st.presetScroll/maxScroll,3*s,thumb},1.5f*s,nx::muted);
        }
        const f32 saveW=content.w*.64f;
        const Rect saveName{save.x,save.y,saveW-4*s,save.h};
        const Rect saveButton{saveName.right()+4*s,save.y,save.right()-saveName.right()-4*s,save.h};
        ui_.textField(id(10),saveName,&st.saveName,nx::bgTop,nx::text,Align::Left,false);
        if(st.saveName.empty() && ui_.editId!=id(10)) rend_.textIn(fSmall_,saveName,"Name sound...",nx::muted,Align::Left,7*s);
        if(ui_.button(id(11),saveButton,"Save preset")) {
            bool exists=false;
            for(int p=inst->factoryPresetCount();p<inst->presetCount();++p)
                exists |= studioLower(presetNameOf(*inst,p))==studioLower(st.saveName);
            if(g_edit.open && g_edit.anyDirty()) status_="Apply or discard the wavetable drawing before saving a preset";
            else if(st.saveName.empty() || exists) status_="Give your sound a new, unique name";
            else if(inst->savePreset(st.saveName.c_str())) {
                st.selected=-1;
                for(int p=inst->factoryPresetCount();p<inst->presetCount();++p)
                    if(st.saveName==presetNameOf(*inst,p)) st.selected=p;
                spectraPreset_=st.selected;snapshot();status_="Spectra preset saved";
            } else status_="Could not save preset - check its name and preset folder";
        }
    }
    // Wide layouts keep the browser docked beside the controls.
    if (st.browser && !compactPicker) {
        const f32 bw = r.w < 1150*s ? 220*s : 254*s;
        const Rect browser{content.x, content.y, bw, content.h};
        rend_.roundRect(browser, 9*s, nx::panel);
        char count[64]; snprintf(count, sizeof count, "%d SOUNDS", inst->presetCount());
        rend_.textIn(fSmall_, {browser.x + 12*s, browser.y + 8*s, browser.w-118*s, 20*s}, count, nx::cyan, Align::Left, 0);
        if(ui_.button(id(16),{browser.right()-106*s,browser.y+5*s,96*s,26*s},"Favorites",st.favoritesOnly)) {
            st.favoritesOnly=!st.favoritesOnly;st.presetScroll=0;
        }
        const Rect search{browser.x + 10*s, browser.y + 36*s, browser.w - 20*s, 34*s};
        const std::string oldQuery = st.query;
        ui_.textField(id(8), search, &st.query, nx::bgTop, nx::text, Align::Left, false);
        if (st.query.empty() && ui_.editId != id(8)) rend_.textIn(fSmall_, search, "Search presets...", nx::muted, Align::Left, 10*s);
        if (oldQuery != st.query) st.presetScroll = 0;
        static const char* cats[] = {"All", "Bass", "Lead", "Pad", "Keys", "Pluck", "FX", "Sequence", "User"};
        const f32 catW=(browser.w-24*s)/3;
        for(int c=0;c<9;++c)
            if(ui_.button(id(50+c),{browser.x+10*s+(c%3)*catW,search.bottom()+6*s+(c/3)*28*s,catW-2*s,26*s},cats[c],st.category==c)) {st.category=c;st.presetScroll=0;}
        const Rect list{browser.x+6*s, search.bottom()+96*s, browser.w-12*s, browser.h-262*s};
        std::vector<int> matches;
        const std::string query = studioLower(ui_.editId==id(8)?ui_.editBuf:st.query);
        if(query!=st.lastQuery) {st.presetScroll=0;st.lastQuery=query;}
        for (int p = 0; p < inst->presetCount(); ++p) {
            if (!matchesPreset(p)) continue;
            matches.push_back(p);
        }
        const f32 rowH = 34*s;
        const f32 maxScroll = std::max(0.f, matches.size()*rowH - list.h);
        if (list.contains(in.mx,in.my) && in.wheel) { st.presetScroll -= in.wheel*rowH*3; in.wheel=0; }
        st.presetScroll = clampv(st.presetScroll, 0.f, maxScroll);
        rend_.pushClip(list);
        for (size_t i=0; i<matches.size(); ++i) {
            const Rect row{list.x, list.y+i*rowH-st.presetScroll, list.w-6*s, rowH-2*s};
            if (row.bottom()<list.y || row.y>list.bottom()) continue;
            const int p = matches[i];
            const Rect star{row.right()-32*s,row.y,32*s,row.h};
            if (ui_.button(id(1000+p), {row.x,row.y,row.w-36*s,row.h}, presetNameOf(*inst,p), p==st.selected)) load(p);
            const bool saved=favorites.contains(favoriteKey(p));
            if(ui_.button(id(5000+p),star,"",saved)) {
                status_=favorites.toggle(favoriteKey(p)) ? (saved?"Removed from favorites":"Added to favorites") : "Could not save favorites - check your configuration folder";
            }
            if(ui_.hovered(star)) ui_.tip=saved?"Remove this preset from favorites":"Add this preset to favorites";
            // Draw the star natively; the font atlas does not include Unicode icons.
            for(int point=0;point<10;++point) {
                const f32 a=point*0.62831853f-1.57079633f, b=(point+1)*0.62831853f-1.57079633f;
                const f32 ra=(point%2?3.8f:8.f)*s, rb=(point%2?8.f:3.8f)*s;
                const f32 ax=star.cx()+std::cos(a)*ra, ay=star.cy()+std::sin(a)*ra;
                const f32 bx=star.cx()+std::cos(b)*rb, by=star.cy()+std::sin(b)*rb;
                if(saved) rend_.triangle(star.cx(),star.cy(),ax,ay,bx,by,nx::text);
                else rend_.line(ax,ay,bx,by,1.3f*s,nx::muted);
            }
        }
        if (matches.empty()) rend_.textIn(fSmall_, list, st.favoritesOnly?"No matching favorites":"No matching sounds", nx::muted, Align::Center, 0);
        rend_.popClip();
        if (maxScroll>0) {
            const f32 th = std::max(20*s,list.h*list.h/(matches.size()*rowH));
            rend_.roundRect({list.right()-3*s,list.y+(list.h-th)*st.presetScroll/maxScroll,3*s,th},1.5*s,nx::muted);
        }
        const Rect save{browser.x+10*s,browser.bottom()-88*s,browser.w-20*s,32*s};
        ui_.textField(id(10),save,&st.saveName,nx::bgTop,nx::text,Align::Left,false);
        if(st.saveName.empty() && ui_.editId!=id(10)) rend_.textIn(fSmall_,save,"Name your sound...",nx::muted,Align::Left,8*s);
        if(ui_.button(id(11),{save.x,save.bottom()+6*s,save.w,34*s},"Save new preset")) {
            bool exists=false;
            for(int p=inst->factoryPresetCount();p<inst->presetCount();++p)
                exists |= studioLower(presetNameOf(*inst,p)) == studioLower(st.saveName);
            if(g_edit.open && g_edit.anyDirty()) status_="Apply or discard the wavetable drawing before saving a preset";
            else if(st.saveName.empty() || exists) status_="Give this sound a new, unique name";
            else if(inst->savePreset(st.saveName.c_str())) {
                st.selected=-1;
                for(int p=inst->factoryPresetCount();p<inst->presetCount();++p)
                    if(st.saveName==presetNameOf(*inst,p)) st.selected=p;
                spectraPreset_=st.selected; snapshot(); status_="Spectra preset saved";
            } else status_="Could not save preset - check its name and preset folder";
        }
        content.x += bw+14*s; content.w -= bw+14*s;
    }
    if(st.toolsOpen && !compactPicker) {
    // Fixed snapshots stay independent of undo and ordinary knob edits.
    {
        const Rect bar{content.x,content.y,content.w,44*s};
        rend_.roundRect(bar,8*s,nx::panel);
        rend_.textIn(fSmall_,{bar.x+12*s,bar.y,82*s,bar.h},"COMPARE",nx::muted,Align::Left,0);
        const auto current=captureSpectraSound(*inst);
        const bool smallCompare=content.w<500*s;
        for(int slot=0;slot<2;++slot) {
            auto& saved=spectraComparisons_[dm.uid][slot];
            const f32 x=bar.x+((smallCompare?84:106)+slot*(smallCompare?122:164))*s;
            const Rect store{x,bar.y+6*s,(smallCompare?78:100)*s,32*s};
            const Rect recall{x+(smallCompare?84:106)*s,store.y,(smallCompare?32:44)*s,32*s};
            const bool matches=saved.valid && saved.params==current.params && saved.state==current.state;
            if(ui_.button(id(24+slot*2),store,slot?"Store B":"Store A")) {
                if(g_edit.open && g_edit.anyDirty()) status_="Apply or discard the wavetable drawing before storing a comparison";
                else {saved=current;status_=slot?"Spectra B stored - click B to recall":"Spectra A stored - click A to recall";}
            }
            if(ui_.hovered(store)) ui_.tip="Store the current sound for this instrument. Slots survive track changes until you open a project or close NxTakt. Does not write a preset file.";
            if(ui_.button(id(25+slot*2),recall,slot?"B":"A",matches)) {
                if(!saved.valid) status_=slot?"Store B first":"Store A first";
                else if(g_edit.open && g_edit.anyDirty()) status_="Apply or discard the wavetable drawing before comparing sounds";
                else if(!matches) {
                    stopPreviews();
                    if(g_edit.open) {wtCancelPreview(inst,g_edit.osc);g_edit.release();}
                    undoPoint(slot?"recall Spectra B":"recall Spectra A");
                    if(restoreSpectraSound(*inst,saved)) {
                        st.selected=spectraPreset_=-1;
                        st.baseline.clear();st.baselineState.clear();
                        status_=slot?"Spectra B recalled - Ctrl+Z restores the previous sound":"Spectra A recalled - Ctrl+Z restores the previous sound";
                    } else status_="Could not restore comparison - previous sound restored";
                }
            }
            if(ui_.hovered(recall)) ui_.tip=saved.valid
                ? "Recall this stored sound, including modulation and wavetable state. Edits are stored only when you press Store. Undo restores the previous sound."
                : "Store a sound in this slot first";
        }
        content.y+=54*s;content.h-=54*s;
    }
    }
    // Variations edit the patch through the same undo and automation path as knobs.
    if(st.toolsOpen && !compactPicker && st.page==0 && !st.soundTools) {
        const Rect bar{content.x,content.y,content.w,44*s};
        rend_.roundRect(bar,8*s,nx::panel);
        rend_.textIn(fSmall_,{bar.x+8*s,bar.y,58*s,bar.h},"VARY",nx::muted,Align::Left,0);
        const bool smallVariation=content.w<600*s;
        const bool sound=ui_.button(id(17),{bar.x+66*s,bar.y+6*s,(smallVariation?96:132)*s,32*s},smallVariation?"Randomize":"Randomize sound");
        const bool macros=ui_.button(id(18),{bar.x+(smallVariation?167:204)*s,bar.y+6*s,(smallVariation?78:116)*s,32*s},smallVariation?"Macros":"Vary macros");
        rend_.textIn(fSmall_,{bar.x+(smallVariation?252:328)*s,bar.y,40*s,bar.h},"Amt",nx::muted,Align::Left,0);
        ui_.dragNumber(id(19),{bar.x+(smallVariation?292:370)*s,bar.y+6*s,68*s,32*s},&st.variationAmount,0,100,1,"%.0f%%");
        if(ui_.hovered(bar)) ui_.tip="Create a variation of this patch. Amount controls the change. Ctrl+Z restores the previous sound.";
        if(sound || macros) {
            if(g_edit.open && g_edit.anyDirty()) status_="Apply or discard the wavetable drawing before making a variation";
            else {
                SpectraParamValues base{};
                for(int p=0;p<std::min(pc,kSpectraVariationParamCount);++p) base[(size_t)p]=get(p);
                auto changes=macros?makeSpectraMacroVariation(base,++st.variationSeed,(f32)st.variationAmount/100.f)
                                   :makeSpectraVariation(base,++st.variationSeed,(f32)st.variationAmount/100.f);
                if(macros) {
                    changes.erase(std::remove_if(changes.begin(),changes.end(),[&](const SpectraParamChange& change) {
                        for(int m=0;m<8;++m) if((int)get(68+3*m)==change.id-94+9 && get(69+3*m)>0) return false;
                        return true;
                    }),changes.end());
                }
                if(changes.empty()) status_=st.variationAmount==0?"Raise Amount to create a variation":"This patch has no assigned macros - choose a mapped preset or add routes on Modulation";
                else {
                    if(g_edit.open) {wtCancelPreview(inst,g_edit.osc);g_edit.release();}
                    undoPoint(macros?"vary Spectra macros":"randomize Spectra sound",id(sound?17:18));
                    for(const auto& change:changes) {
                        if(change.id>=pc) continue;
                        const auto& info=inst->paramInfo(change.id);
                        f32 value=clampv(change.value,info.min,info.max);
                        if(info.isInt) value=std::round(value);
                        inst->setParam(change.id,value);
                        if(ownIsTrack(devOwner_)) autoCapture(addr::deviceParam(ses_.tracks[devOwner_].uid,dm.uid,info.id),value,id(sound?17:18));
                    }
                    status_=macros?"New macro variation - Ctrl+Z restores the previous sound":"New sound variation - tuning, custom tables and routing preserved; Ctrl+Z to restore";
                }
            }
        }
        content.y+=54*s;content.h-=54*s;
    }
    if(st.toolsOpen && !compactPicker) {
        const Rect bar{content.x,content.y,content.w,40*s};
        rend_.roundRect(bar,8*s,nx::panel);
        if(ui_.button(id(13),{bar.x+8*s,bar.y+5*s,70*s,30*s},"Silence")) {
            stopPreviews();eng_.pushMidi(MidiMsg{0xB0,120,0,0,0});status_="Armed instruments silenced";
        }
        if(ui_.button(id(6),{bar.x+84*s,bar.y+5*s,76*s,30*s},"Hear C3")) {
            if(ownIsTrack(devOwner_)) {selectTrack(devOwner_);startPreview(48,dm.uid);for(auto& p:previews_)p.offAt=nowSeconds()+2.0;}
            else status_="Select a track to audition this instrument";
        }
        if(ui_.button(id(7),{bar.x+164*s,bar.y+5*s,94*s,30*s},"Hear chord")) {
            if(ownIsTrack(devOwner_)) {selectTrack(devOwner_);for(int n:{48,55,60,63})startPreview(n,dm.uid);for(auto& p:previews_)p.offAt=nowSeconds()+2.0;}
        }
        content.y+=48*s;content.h-=48*s;
    }
    // Shared controls use the instrument's parameter metadata, so ranges,
    // logarithmic drags and automation stay aligned with the sound engine.
    auto knob = [&](int p, const char* label, Rect b, Col color=nx::violet) {
        if(p>=pc) return;
        const ParamInfo& info=inst->paramInfo(p);
        f32 value=get(p);
        Ui::KnobStyle style;
        style.lo=info.min; style.hi=info.max; style.def=info.def;
        style.log=info.isLogarithmic; style.bipolar=info.min<0; style.arc=color;
        char readout[64];
        if(info.unit=="Hz") {
            if(value>=1000.f) std::snprintf(readout,sizeof readout,"%.1f kHz",(double)(value/1000.f));
            else std::snprintf(readout,sizeof readout,"%.0f Hz",(double)value);
        } else if(info.unit=="ms") {
            if(value>=1000.f) std::snprintf(readout,sizeof readout,"%.1f s",(double)(value/1000.f));
            else std::snprintf(readout,sizeof readout,"%.0f ms",(double)value);
        } else if(info.min==0.f && info.max==1.f) {
            std::snprintf(readout,sizeof readout,"%.0f%%",(double)(value*100.f));
        } else if(!info.unit.empty()) {
            std::snprintf(readout,sizeof readout,info.isInt?"%.0f %s":"%.1f %s",(double)value,info.unit.c_str());
        } else {
            std::snprintf(readout,sizeof readout,info.isInt?"%.0f":"%.2f",(double)value);
        }
        style.fmt=nullptr; style.text=readout;
        Rect dial{b.x+3*s,b.y,std::max(20*s,b.w-6*s),std::max(40*s,b.h-20*s)};
        int destination=-1;
        for(int d=1;d<kDstCount;++d) if(kDestParam[d]==p) destination=d;
        ui_.offer({(ownIsTrack(devOwner_)?(u32)Ui::MenuLearn:0u) | (destination>0?(u32)Ui::MenuAssign:0u)});
        if(ui_.grab(7*s).knobNx(id(100+p),dial,&value,style)) commit(p,info.isInt?std::round(value):value);
        if(destination>0) {
            f32 lo=0,hi=0;
            for(int m=0;m<8;++m) {
                const int source=(int)get(68+3*m);
                if(source==0 || (int)get(69+3*m)!=destination) continue;
                const f32 amount=get(70+3*m)*kDestSpan[destination];
                bool bipolar=srcBipolarStatic(source);
                if(source>=1 && source<=3) bipolar=(int)get(kLfoShapeId[source-1])!=5;
                if(bipolar) {lo-=std::fabs(amount);hi+=std::fabs(amount);}
                else if(amount>0) hi+=amount; else lo+=amount;
            }
            ui_.knobRing(dial,style,value,lo,hi,nx::cyan.alpha(.7f));
            if(ui_.menuFired(id(100+p),Ui::MenuAssign)) {
                int slot=-1;
                for(int m=0;m<8;++m) if(get(68+3*m)==0 || get(69+3*m)==0) {slot=m;break;}
                if(slot<0) status_="All eight modulation routes are used - edit a route on Modulation";
                else {
                    st.popup=68+3*slot;st.assignDest=destination;st.options.clear();
                    for(const char* source:kMatrixSrc) st.options.emplace_back(source);
                }
            }
        }
        if(ownIsTrack(devOwner_) && ui_.menuFired(id(100+p),Ui::MenuLearn))
            cycleMidiLearn(addr::deviceParam(ses_.tracks[devOwner_].uid,dm.uid,info.id));
        rend_.textIn(fSmall_,{b.x,b.bottom()-20*s,b.w,20*s},label,nx::muted,Align::Center,0);
        if(ui_.hovered(b)) ui_.tip=info.name+" - drag to adjust; double-click to reset; right-click to type a value or learn MIDI";
    };
    auto choose = [&](int p, Rect b, const char* const* options, int n) {
        if(p>=pc) return;
        const int value=clampv((int)std::round(get(p)),0,n-1);
        if(ui_.button(id(100+p),b,options[value])) {
            st.popup=p; st.assignDest=-1; st.options.clear();
            for(int i=0;i<n;++i) st.options.emplace_back(options[i]);
        }
    };
    auto card = [&](Rect b,const char* name,const char* hint) {
        rend_.roundRect(b,5*s,nx::panel);
        rend_.roundRectOutline(b,5*s,s,nx::line.alpha(.55f));
        rend_.textIn(fBody_,{b.x+12*s,b.y+8*s,b.w-24*s,20*s},name,nx::text,Align::Left,0);
        rend_.textIn(fSmall_,{b.x+12*s,b.y+30*s,b.w-24*s,18*s},hint,nx::muted,Align::Left,0);
    };
    if(!compactPicker && st.page<=1 && !st.soundTools) {
        const f32 needed=(st.page==0?330.f:470.f)*s;
        st.scroll=clampv(st.scroll,0.f,std::max(0.f,needed-content.h));
        rend_.pushClip(content);
        Rect canvas=content; canvas.y-=st.scroll;
        if(needed>content.h) canvas.w-=28*s;
        if(st.page==0) {
            const f32 gap=8*s, cw=(canvas.w-2*gap)/3.f;
            const f32 topH=172*s, lowerY=canvas.y+topH+9*s, lowerH=std::max(148*s,canvas.bottom()-lowerY);
            auto waveCard = [&](int osc, Rect c, Col color, const char* title, int knobA, const char* labelA, int knobB, const char* labelB) {
                const int base=osc*8;
                card(c,title,"");
                const char* waveNames[9];
                for(int t=0;t<8;++t) waveNames[t]=kTables[t];
                const std::string customName=std::string("Custom: ")+wtName(inst,osc);
                waveNames[8]=customName.c_str();
                choose(base,{c.x+8*s,c.y+30*s,c.w-16*s,22*s},waveNames,wtHasCustom(inst,osc)?9:8);
                const Rect wave{c.x+8*s,c.y+56*s,c.w-16*s,42*s};
                rend_.roundRect(wave,5*s,nx::bgTop);
                if(drag_.kind==DragState::Kind::BrowserFile && drag_.armed && wave.contains(in.mx,in.my)) {
                    rend_.roundRectOutline(wave,5*s,1.5f*s,nx::cyan);
                    ui_.tip="Drop WAV to import a wavetable into this oscillator";
                    if(in.released[0]) {
                        undoPoint("import Spectra wavetable");
                        if(wtImport(inst,osc,drag_.path.c_str())) {
                            inst->setParam(base,8.f);
                            if(ownIsTrack(devOwner_)) autoCapture(addr::deviceParam(ses_.tracks[devOwner_].uid,dm.uid,inst->paramInfo(base).id),8.f,0);
                            status_=std::string("Imported wavetable: ")+wtName(inst,osc);
                        } else status_=std::string("Wavetable import: ")+wtError(inst);
                        drag_=DragState{};
                    }
                }
                if(ui_.hovered(wave)) ui_.tip="Waveform at the current position / drop a WAV to import";
                rend_.line(wave.x+4*s,wave.cy(),wave.right()-4*s,wave.cy(),s,nx::line);
                const detail::SpectraTableSet* tables=detail::spectraTables();
                const bool custom=wtHasCustom(inst,osc) && get(base)>=8;
                const f32* customData=nullptr;
                if(custom) {spectraWave_.resize(32*2048);if(wtReadFrames(inst,osc,spectraWave_.data()))customData=spectraWave_.data();}
                const auto frame=tables?tables->morph((int)get(base),get(base+1)):detail::SpectraFrameView{};
                for(int i=1;i<100;++i) {
                    auto sample=[&](int j) {
                        if(customData && !spectraWave_.empty()) {const float pos=get(base+1)*31;const int f=std::min(30,(int)pos);const int at=j*2047/99;return customData[f*2048+at]*(1-(pos-f))+customData[(f+1)*2048+at]*(pos-f);}
                        return frame.valid()?frame.at(j*(frame.len-1)/99):0.f;
                    };
                    rend_.line(wave.x+4*s+(wave.w-8*s)*(i-1)/99.f,wave.cy()-sample(i-1)*wave.h*.37f,
                               wave.x+4*s+(wave.w-8*s)*i/99.f,wave.cy()-sample(i)*wave.h*.37f,1.5f*s,color);
                }
                const f32 kw=(c.w-16*s)*.5f;
                knob(knobA,labelA,{c.x+8*s,c.y+96*s,kw,74*s},color);
                knob(knobB,labelB,{c.x+8*s+kw,c.y+96*s,kw,74*s},color);
            };
            Rect oscA{canvas.x,canvas.y,cw,topH};
            Rect oscB{canvas.x+cw+gap,canvas.y,cw,topH};
            Rect filter{canvas.x+2*(cw+gap),canvas.y,cw,topH};
            waveCard(0,oscA,nx::cyan,"OSC A",1,"Position",49,"Warp");
            waveCard(1,oscB,pal::meterAmber,"OSC B",13,"Unison",14,"Detune");

            card(filter,"Filter","");
            static const char* filters[]={"Low-pass 12","Band-pass 12","High-pass 12","Low-pass 24","High-pass 24","Notch"};
            choose(20,{filter.right()-88*s,filter.y+7*s,80*s,21*s},filters,6);
            const Rect graph{filter.x+8*s,filter.y+34*s,filter.w-16*s,64*s};
            rend_.roundRect(graph,5*s,nx::bgTop);
            for(int i=1;i<4;++i) rend_.line(graph.x,graph.y+graph.h*i/4.f,graph.right(),graph.y+graph.h*i/4.f,0.7f*s,nx::line.alpha(.55f));
            for(int i=1;i<5;++i) rend_.line(graph.x+graph.w*i/5.f,graph.y,graph.x+graph.w*i/5.f,graph.bottom(),0.7f*s,nx::line.alpha(.45f));
            const auto& cutoffInfo=inst->paramInfo(18);
            f32 cutoffNorm=clampv((get(18)-cutoffInfo.min)/std::max(1e-6f,cutoffInfo.max-cutoffInfo.min),.08f,.92f);
            if(cutoffInfo.isLogarithmic && cutoffInfo.min>0 && cutoffInfo.max>cutoffInfo.min)
                cutoffNorm=clampv(std::log(get(18)/cutoffInfo.min)/std::log(cutoffInfo.max/cutoffInfo.min),.08f,.92f);
            const f32 resonance=clampv(get(19),0.f,1.f);
            const int filterType=clampv((int)std::round(get(20)),0,5);
            auto responseY=[&](f32 x) {
                const f32 knee=1.f/(1.f+std::exp((x-cutoffNorm)*36.f));
                const f32 distance=(x-cutoffNorm)*22.f;
                const f32 peak=std::exp(-distance*distance)*resonance*.22f;
                f32 level=.12f+.72f*(1.f-knee);       // low-pass
                if(filterType==1) {
                    const f32 band=std::exp(-distance*distance*.42f);
                    level=.12f+.72f*(1.f-band)-peak;
                } else if(filterType==2) {
                    level=.12f+.72f*knee-peak;
                } else if(filterType==4) {
                    const f32 steep=1.f/(1.f+std::exp((x-cutoffNorm)*60.f));
                    level=.12f+.72f*steep-peak;
                } else if(filterType==5) {
                    const f32 notch=std::exp(-distance*distance*.55f);
                    level=.12f+.72f*notch;
                } else if(filterType==3) {
                    const f32 steep=1.f/(1.f+std::exp((x-cutoffNorm)*60.f));
                    level=.12f+.72f*(1.f-steep)-peak;
                }
                return graph.y+graph.h*clampv(level,.04f,.96f);
            };
            f32 px=graph.x, py=responseY(0.f);
            const Col filterFill=pal::soloBlue.alpha(.13f);
            for(int i=1;i<72;++i) {
                const f32 x=(f32)i/71.f;
                const f32 y=responseY(x);
                const f32 nxp=graph.x+x*graph.w;
                rend_.triangle(px,graph.bottom(),nxp,graph.bottom(),px,py,filterFill);
                rend_.triangle(nxp,graph.bottom(),px,py,nxp,y,filterFill);
                rend_.line(px,py,nxp,y,1.5f*s,pal::soloBlue);
                px=nxp;py=y;
            }
            const f32 fw=(filter.w-20*s)/3.f;
            knob(18,"Cutoff",{filter.x+5*s,filter.y+96*s,fw,74*s},pal::soloBlue);
            knob(19,"Resonance",{filter.x+7*s+fw,filter.y+96*s,fw,74*s},pal::soloBlue);
            knob(21,"Drive",{filter.x+9*s+2*fw,filter.y+96*s,fw,74*s},pal::soloBlue);

            const f32 envW=(canvas.w-gap)*.58f;
            Rect envelope{canvas.x,lowerY,envW,lowerH};
            Rect macros{envelope.right()+gap,lowerY,canvas.right()-envelope.right()-gap,lowerH};
            card(envelope,"Amp Envelope","");
            const Rect envGraph{envelope.x+8*s,envelope.y+32*s,envelope.w-16*s,std::max(38*s,lowerH-110*s)};
            rend_.roundRect(envGraph,4*s,nx::bgTop);
            const f32 attack=envGraph.x+envGraph.w*(.025f+.20f*clampv(get(24)/5000.f,0.f,1.f));
            const f32 decay=attack+envGraph.w*(.08f+.18f*clampv(get(25)/5000.f,0.f,1.f));
            const f32 sustainY=envGraph.bottom()-envGraph.h*.86f*clampv(get(26),0.f,1.f);
            const f32 release=envGraph.x+envGraph.w*.78f;
            const Col envelopeFill=pal::recRed.alpha(.16f);
            const f32 baseY=envGraph.bottom()-2*s;
            const f32 startX=envGraph.x+2*s, peakY=envGraph.y+3*s;
            rend_.triangle(startX,baseY,attack,peakY,decay,sustainY,envelopeFill);
            rend_.triangle(startX,baseY,decay,sustainY,release,sustainY,envelopeFill);
            rend_.triangle(startX,baseY,release,sustainY,envGraph.right()-2*s,baseY,envelopeFill);
            rend_.line(startX,baseY,attack,peakY,1.6f*s,pal::recRed);
            rend_.line(attack,peakY,decay,sustainY,1.6f*s,pal::recRed);
            rend_.line(decay,sustainY,release,sustainY,1.6f*s,pal::recRed);
            rend_.line(release,sustainY,envGraph.right()-2*s,envGraph.bottom()-2*s,1.6f*s,pal::recRed);
            const f32 ew=(envelope.w-20*s)/4.f;
            const char* el[]={"Attack","Decay","Sustain","Release"};
            for(int j=0;j<4;++j) knob(24+j,el[j],{envelope.x+4*s+j*ew,envelope.bottom()-78*s,ew,72*s},pal::recRed);

            card(macros,"Macros","");
            if(ui_.button(id(14),{macros.right()-54*s,macros.y+5*s,46*s,22*s},"More")) st.soundTools=true;
            const f32 mw=(macros.w-12*s)/4.f;
            const char* ml[]={"Bright","Move","Space","Texture"};
            for(int j=0;j<4;++j) knob(94+j,ml[j],{macros.x+3*s+j*mw,macros.bottom()-78*s,mw,72*s},nx::cyan);
        } else {
            const char* names[]={"CHORUS","PING-PONG DELAY","REVERB"};
            const char* hints[]={"Width and motion","Rhythmic stereo echoes","Space around your sound"};
            const int params[3][4]={{125,126,127,128},{129,130,132,131},{133,134,135,136}};
            const char* labels[3][4]={{"Mix","Rate","Depth","Width"},{"Mix","Time","Feedback","Sync"},{"Mix","Size","Decay","Damping"}};
            for(int k=0;k<3;++k) {
                Rect c{canvas.x,canvas.y+k*152*s,canvas.w,140*s};
                card(c,names[k],hints[k]);
                const f32 left=std::min(210*s,c.w*.3f), w=(c.w-left-20*s)/4;
                for(int j=0;j<4;++j) {
                    Rect b{c.x+left+j*w,c.y+22*s,w,104*s};
                    if(params[k][j]==131) {
                        choose(131,{b.x+8*s,b.y+28*s,b.w-16*s,34*s},kSyncDiv,10);
                        rend_.textIn(fSmall_,{b.x,b.bottom()-20*s,b.w,20*s},"Tempo sync",nx::muted,Align::Center,0);
                    }
                    else if(params[k][j]==130 && get(131)>0) {
                        rend_.textIn(fBold_,{b.x,b.y+25*s,b.w,32*s},kSyncDiv[clampv((int)get(131),0,9)],nx::cyan,Align::Center,0);
                        rend_.textIn(fSmall_,{b.x,b.bottom()-20*s,b.w,20*s},"Song tempo",nx::muted,Align::Center,0);
                    }
                    else knob(params[k][j],labels[k][j],b,k==1?nx::cyan:nx::violet);
                }
            }
        }
        rend_.popClip();
        if(content.contains(in.mx,in.my) && in.wheel) {st.scroll-=in.wheel*40*s; in.wheel=0;}
        const f32 maxScroll=std::max(0.f,needed-content.h);
    if (maxScroll > 0.f) {
        const Rect rail{content.right()-24*s,content.y,24*s,content.h};
        const f32 thumbH = std::max(40*s,content.h*content.h/needed);
        const f32 travel = std::max(1.f,rail.h-thumbH);
        const Rect thumb{rail.x,rail.y+travel*st.scroll/maxScroll,rail.w,thumbH};
        const u64 scrollId = id(15);
        const bool hot = ui_.setHot(scrollId,rail) && ui_.isHot(scrollId);
        if (hot && in.pressed[0]) {
            ui_.active = scrollId;
            if (!thumb.contains(in.mx,in.my))
                st.scroll = clampv((in.my-rail.y-thumbH*0.5f)/travel*maxScroll,0.f,maxScroll);
            ui_.dragStart = st.scroll;
            ui_.dragAccum = 0.f;
        }
        if (ui_.active == scrollId && in.down[0]) {
            ui_.dragAccum += in.dy;
            st.scroll = clampv((f32)ui_.dragStart+ui_.dragAccum/travel*maxScroll,0.f,maxScroll);
        }
        if (ui_.active == scrollId && in.released[0]) ui_.active = 0;
        rend_.roundRect({rail.cx()-3*s,rail.y+travel*st.scroll/maxScroll,6*s,thumbH},
                         3*s,hot || ui_.active==scrollId ? nx::violetSoft : nx::muted.alpha(0.6f));
        if (hot || ui_.active==scrollId) ui_.cursor = Cursor::ResizeV;
    }
    } else if(!compactPicker && st.page==3) {
#include "spectra_arp_studio.inc"
    } else if(!compactPicker && st.page==2 && !st.motionTools) {
#include "spectra_motion.inc"
    } else if(!compactPicker) {
        if(st.page==0) {
            if(ui_.button(id(3003),{content.x,content.y,160*s,34*s},"Back to Sound")) st.soundTools=false;
            content.y+=42*s;content.h-=42*s;
        }
        if(st.page==2) {
            if(ui_.button(id(3002),{content.x,content.y,160*s,34*s},"Back to modulation")) st.motionTools=false;
            content.y+=42*s;content.h-=42*s;
        }
        // The advanced tools retain their state and editing gestures, now in a
        // full-height workspace. Narrow windows pan explicitly with Shift+wheel.
        const int legacyPage=st.page==0?0:st.page==2?1:3;
        g_pageUid=dm.uid; g_page=legacyPage;
        const f32 width=std::max(content.w,lay::spectraPanelW*s);
        st.advancedScroll=clampv(st.advancedScroll,0.f,width-content.w);
        if(content.contains(in.mx,in.my) && in.shift() && in.wheel) {st.advancedScroll-=in.wheel*60*s;in.wheel=0;}
        rend_.pushClip(content);
        const u64 presetRevision=g_legacyPresetRevision;
        drawSpectraPanel({content.x-st.advancedScroll,content.y,width,content.h-24*s},dm,nx::violet,true);
        if(presetRevision!=g_legacyPresetRevision) {st.selected=spectraPreset_;snapshot();}
        rend_.popClip();
        rend_.textIn(fSmall_,{content.x,content.bottom()-20*s,content.w,20*s},
                    width>content.w?"Hide Presets for more space / Shift + scroll to move across tools":"Drag a modulation source onto a destination / Right-click controls for more actions",nx::muted,Align::Left,0);
    }
    in.mx=realMx; in.my=realMy;
    // Enum choices use one large direct picker: wavetable, filter or sync can
    // be selected in one click instead of cycling through a narrow value.
    if(st.popup>=0 && popupWasOpen) {
        ui_.keyModal=true;
        const int count=(int)st.options.size();
        Rect modal{r.cx()-230*s,r.cy()-((count+1)/2*40+68)*s*.5f,460*s,((count+1)/2*40+68)*s};
        rend_.rect(r,nx::bgTop.alpha(.82f));
        rend_.roundRect(modal,10*s,nx::panel2);
        rend_.textIn(fBold_,{modal.x+16*s,modal.y+12*s,modal.w-70*s,26*s},inst->paramInfo(st.popup).name.c_str(),nx::text,Align::Left,0);
        bool close=ui_.button(id(20),{modal.right()-46*s,modal.y+8*s,34*s,32*s},"X") || in.keyPressed[KeyEscape];
        if(in.pressed[0] && !modal.contains(in.mx,in.my)) close=true;
        for(int i=0;i<count;++i) {
            Rect b{modal.x+12*s+(i%2)*220*s,modal.y+52*s+(i/2)*40*s,212*s,34*s};
            if(ui_.button(id(2000+i),b,st.options[i].c_str(),i==(int)get(st.popup))) {
                if(st.assignDest>=0) {
                    undoPoint("assign Spectra modulation");
                    const f32 values[3]={(f32)i,(f32)st.assignDest,.3f};
                    for(int k=0;k<3;++k) {
                        inst->setParam(st.popup+k,values[k]);
                        if(ownIsTrack(devOwner_)) autoCapture(addr::deviceParam(ses_.tracks[devOwner_].uid,dm.uid,inst->paramInfo(st.popup+k).id),values[k],id(2000+i));
                    }
                    status_="Modulation assigned at +0.30 - adjust its depth on Modulation";
                } else commit(st.popup,(f32)i);
                close=true;
            }
        }
        if(close) {st.popup=-1;st.assignDest=-1;st.options.clear();}
    }
}
