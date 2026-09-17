// Focused Spectra workspace. Included by app_spectra.cpp so the established
// wavetable drawing/state tools remain the same implementation.
namespace {
struct SpectraStudioState {
    u64 uid = 0;
    int page = 0, category = 0, selected = -1, popup = -1, assignDest = -1;
    f32 scroll = 0, presetScroll = 0, advancedScroll = 0;
    bool browser = true, files = false, motionTools = false, soundTools = false, favoritesOnly = false;
    f64 variationAmount = 35;
    u32 variationSeed = 0x4e585350;
    std::string query, lastQuery, saveName, baselineState;
    std::vector<f32> baseline;
    SpectraSoundSnapshot compare[2];
    std::vector<std::string> options;
} g_studio;
std::string studioLower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
}

void App::addSpectraTrack() {
    if(ses_.tracks.size()>=kMaxTracks) {status_="Track limit reached";return;}
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
        createMidiClip(track,0,false);
        ses_.tracks[track].slots[0].name="Synth pattern";
        selectTrack(track);selectChainOwner(track);selDevice_=0;
        spectraOpenUid_=ses_.tracks[track].devices[0].uid;
        showDetail_=true;detailTab_=DetailTab::Devices;
        status_="Spectra ready - choose a preset, then play your keyboard";
        return;
    }
}

bool App::drawFocusedSpectra(const Rect& bounds) {
    if (!showDetail_ || detailTab_ != DetailTab::Devices || !spectraOpenUid_ || spectraForced_) return false;
    ChainOwner co = chainOwner(devOwner_);
    if (!co.devices) return false;
    const int idx = spectraOpenIdx(*co.devices);
    if (idx < 0 || !isSpectra((*co.devices)[idx].inst.get())) return false;
    reclaimRacks();
    drawSpectraStudio(bounds, (*co.devices)[idx]);
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
    const bool popupWasOpen = st.popup >= 0;
    if (popupWasOpen) { in.mx = in.my = -1e6f; ui_.keyModal = true; }
    rend_.rect(r, nx::bgTop);
    const Rect header{r.x + 16*s, r.y + 12*s, r.w - 32*s, 50*s};
    rend_.textIn(fBold_, {header.x, header.y, 125*s, 25*s}, "S P E C T R A", nx::text, Align::Left, 0);
    rend_.textIn(fSmall_, {header.x, header.y + 25*s, 150*s, 20*s}, "WAVETABLE INSTRUMENT", nx::muted, Align::Left, 0);
    const f32 nameX = header.x + 172*s;
    const Rect patch{nameX, header.y, std::max(130*s, header.w - 584*s), 42*s};
    rend_.roundRect(patch, 7*s, nx::panel2);
    const std::string patchName = st.selected >= 0 ? presetNameOf(*inst, st.selected) : "Current patch";
    rend_.textIn(fBold_, {patch.x + 12*s, patch.y + 3*s, patch.w - 24*s, 21*s}, patchName.c_str(), nx::text, Align::Left, 0);
    const char* sub = st.selected < 0 ? "Browse a sound or start designing" : modified ? "Modified - save your sound" : "Factory / user preset";
    rend_.textIn(fSmall_, {patch.x + 12*s, patch.y + 23*s, patch.w - 24*s, 16*s}, sub, modified ? nx::cyan : nx::muted, Align::Left, 0);
    f32 hx = patch.right() + 6*s;
    if (ui_.button(id(1), {hx, header.y, 34*s, 42*s}, "<")) stepPreset(-1);
    hx += 40*s;
    if (ui_.button(id(2), {hx, header.y, 34*s, 42*s}, ">")) stepPreset(1);
    hx += 44*s;
    if (ui_.button(id(3), {hx, header.y, 78*s, 42*s}, "Presets", st.browser)) {finishSidebarEdit();st.browser=!st.browser;st.files=false;}
    if (ui_.button(id(12), {header.right()-232*s,header.y,86*s,42*s}, "Files",st.files)) {finishSidebarEdit();st.files=!st.files;st.browser=false;}
    if (ui_.button(id(4), {header.right() - 132*s, header.y, 132*s, 42*s}, "Back to chain")) {
        if (g_edit.open && g_edit.anyDirty()) status_ = "Apply or discard the wavetable drawing before closing";
        else { finishSidebarEdit(); stopPreviews(); spectraOpenUid_ = 0; if(g_edit.open) wtCancelPreview(inst,g_edit.osc); g_edit.release(); }
    }
    const Rect nav{r.x + 16*s, header.bottom() + 8*s, r.w - 32*s, 38*s};
    const char* tabs[] = {"Sound", "Effects", "Modulation", "Arpeggiator", "Wavetable"};
    int page = st.page;
    ui_.tabPill(id(5), {nav.x, nav.y, std::min(650*s, nav.w - 292*s), nav.h}, tabs, 5, &page);
    if (page != st.page) {
        if (g_edit.open && g_edit.anyDirty()) status_ = "Apply or discard the wavetable drawing before changing pages";
        else { st.page = page; st.soundTools=false; st.scroll = st.advancedScroll = 0; if(g_edit.open) wtCancelPreview(inst,g_edit.osc); g_edit.release(); }
    }
    if(ui_.button(id(13), {nav.right()-282*s,nav.y,76*s,nav.h}, "Silence")) {
        stopPreviews();
        eng_.pushMidi(MidiMsg{0xB0,120,0,0,0});
        status_="Armed instruments silenced";
    }
    if (ui_.button(id(6), {nav.right() - 200*s, nav.y, 94*s, nav.h}, "Hear C3")) {
        if (ownIsTrack(devOwner_)) { selectTrack(devOwner_); startPreview(48, dm.uid); for(auto& p:previews_) p.offAt=nowSeconds()+2.0; }
        else status_ = "Select a track to audition this instrument";
    }
    if (ui_.button(id(7), {nav.right() - 100*s, nav.y, 100*s, nav.h}, "Hear chord")) {
        if (ownIsTrack(devOwner_)) {
            selectTrack(devOwner_);
            for (int n : {48, 55, 60, 63}) startPreview(n, dm.uid);
            for(auto& p:previews_) p.offAt=nowSeconds()+2.0;
        }
    }
    Rect content{r.x + 16*s, nav.bottom() + 14*s, r.w - 32*s, r.bottom() - nav.bottom() - 28*s};
    if(st.files) {
        const f32 bw=r.w<1150*s?220*s:254*s;
        drawBrowser({content.x,content.y,bw,content.h});
        content.x+=bw+14*s;content.w-=bw+14*s;
    }
    // Presets are a stable, searchable sidebar, not a tiny popup over knobs.
    if (st.browser) {
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
    // Fixed snapshots stay independent of undo and ordinary knob edits.
    {
        const Rect bar{content.x,content.y,content.w,44*s};
        rend_.roundRect(bar,8*s,nx::panel);
        rend_.textIn(fSmall_,{bar.x+12*s,bar.y,82*s,bar.h},"COMPARE",nx::muted,Align::Left,0);
        const auto current=captureSpectraSound(*inst);
        for(int slot=0;slot<2;++slot) {
            auto& saved=st.compare[slot];
            const f32 x=bar.x+(106+slot*164)*s;
            const Rect store{x,bar.y+6*s,100*s,32*s};
            const Rect recall{x+106*s,store.y,44*s,32*s};
            const bool matches=saved.valid && saved.params==current.params && saved.state==current.state;
            if(ui_.button(id(24+slot*2),store,slot?"Store B":"Store A")) {
                if(g_edit.open && g_edit.anyDirty()) status_="Apply or discard the wavetable drawing before storing a comparison";
                else {saved=current;status_=slot?"Spectra B stored - click B to recall":"Spectra A stored - click A to recall";}
            }
            if(ui_.hovered(store)) ui_.tip="Store the current sound in this temporary comparison slot. Does not write a preset file.";
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
    // Variations edit the patch through the same undo and automation path as knobs.
    if(st.page==0 && !st.soundTools) {
        const Rect bar{content.x,content.y,content.w,44*s};
        rend_.roundRect(bar,8*s,nx::panel);
        rend_.textIn(fSmall_,{bar.x+12*s,bar.y,94*s,bar.h},"VARIATIONS",nx::muted,Align::Left,0);
        const bool sound=ui_.button(id(17),{bar.x+106*s,bar.y+6*s,136*s,32*s},"Randomize sound");
        const bool macros=ui_.button(id(18),{bar.x+250*s,bar.y+6*s,124*s,32*s},"Vary macros");
        rend_.textIn(fSmall_,{bar.x+390*s,bar.y,64*s,bar.h},"Amount",nx::muted,Align::Left,0);
        ui_.dragNumber(id(19),{bar.x+452*s,bar.y+6*s,84*s,32*s},&st.variationAmount,0,100,1,"%.0f%%");
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
    // Shared controls use the instrument's parameter metadata, so ranges,
    // logarithmic drags and automation stay aligned with the sound engine.
    auto knob = [&](int p, const char* label, Rect b, Col color=nx::violet) {
        if(p>=pc) return;
        const ParamInfo& info=inst->paramInfo(p);
        f32 value=get(p);
        Ui::KnobStyle style;
        style.lo=info.min; style.hi=info.max; style.def=info.def;
        style.log=info.isLogarithmic; style.bipolar=info.min<0; style.arc=color;
        style.fmt=info.isInt?"%.0f":info.max>100?"%.0f":info.max>5?"%.1f":"%.2f";
        Rect dial{b.x+7*s,b.y,std::max(20*s,b.w-14*s),std::max(40*s,b.h-22*s)};
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
        rend_.roundRect(b,9*s,nx::panel);
        rend_.roundRectOutline(b,9*s,s,nx::line.alpha(.45f));
        rend_.textIn(fBold_,{b.x+14*s,b.y+10*s,b.w-28*s,22*s},name,nx::text,Align::Left,0);
        rend_.textIn(fSmall_,{b.x+14*s,b.y+33*s,b.w-28*s,18*s},hint,nx::muted,Align::Left,0);
    };
    if(st.page<=1 && !st.soundTools) {
        const f32 needed=(st.page==0?682.f:470.f)*s;
        st.scroll=clampv(st.scroll,0.f,std::max(0.f,needed-content.h));
        rend_.pushClip(content);
        Rect canvas=content; canvas.y-=st.scroll;
        if(needed>content.h) canvas.w-=28*s;
        if(st.page==0) {
            const f32 gap=12*s, cw=(canvas.w-gap)*.5f;
            for(int osc=0;osc<2;++osc) {
                const int base=osc*8;
                Rect c{canvas.x+osc*(cw+gap),canvas.y,cw,330*s};
                card(c,osc?"OSCILLATOR B":"OSCILLATOR A",osc?"Layer, detune, or add a second character":"Morph the waveform to shape your sound");
                const char* waveNames[9];
                for(int t=0;t<8;++t) waveNames[t]=kTables[t];
                const std::string customName=std::string("Custom: ")+wtName(inst,osc);
                waveNames[8]=customName.c_str();
                choose(base,{c.x+12*s,c.y+60*s,c.w-24*s,30*s},waveNames,wtHasCustom(inst,osc)?9:8);
                const Rect wave{c.x+12*s,c.y+98*s,c.w-24*s,64*s};
                rend_.roundRect(wave,6*s,nx::bgTop);
                if(drag_.kind==DragState::Kind::BrowserFile && drag_.armed && wave.contains(in.mx,in.my)) {
                    rend_.roundRectOutline(wave,6*s,2*s,nx::cyan);
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
                if(ui_.hovered(wave)) ui_.tip="Waveform at the current position / Open Files and drop a WAV here to import";
                rend_.line(wave.x+8*s,wave.cy(),wave.right()-8*s,wave.cy(),s,nx::line);
                const detail::SpectraTableSet* tables=detail::spectraTables();
                const bool custom=wtHasCustom(inst,osc) && get(base)>=8;
                const f32* customData=nullptr;
                if(custom) { spectraWave_.resize(32*2048); if(wtReadFrames(inst,osc,spectraWave_.data())) customData=spectraWave_.data(); }
                const auto frame=tables?tables->morph((int)get(base),get(base+1)):detail::SpectraFrameView{};
                for(int i=1;i<180;++i) {
                    auto sample=[&](int j) {
                        if(customData && !spectraWave_.empty()) { const float pos=get(base+1)*31; const int f=std::min(30,(int)pos); const int at=j*2047/179; return customData[f*2048+at]*(1-(pos-f))+customData[(f+1)*2048+at]*(pos-f); }
                        return frame.valid()?frame.at(j*(frame.len-1)/179):0.f;
                    };
                    rend_.line(wave.x+8*s+(wave.w-16*s)*(i-1)/179.f,wave.cy()-sample(i-1)*wave.h*.38f,
                               wave.x+8*s+(wave.w-16*s)*i/179.f,wave.cy()-sample(i)*wave.h*.38f,1.6f*s,osc?nx::cyan:nx::violetSoft);
                }
                rend_.textIn(fSmall_,{c.x+12*s,c.y+170*s,36*s,32*s},"Pitch",nx::muted,Align::Left,0);
                f64 coarse=get(base+2), fine=get(base+3);
                if(ui_.dragNumber(id(100+base+2),{c.x+48*s,c.y+170*s,62*s,32*s},&coarse,-24,24,1,"%.0f st")) commit(base+2,(f32)std::round(coarse));
                if(ui_.dragNumber(id(100+base+3),{c.x+114*s,c.y+170*s,66*s,32*s},&fine,-100,100,1,"%.0f ct")) commit(base+3,(f32)fine);
                choose(osc?50:48,{c.x+186*s,c.y+170*s,c.w-198*s,32*s},kWarpMode,8);
                const int params[6]={base+1,base+4,base+5,base+6,base+7,osc?51:49};
                const char* labels[6]={"Position","Level","Unison","Detune","Width","Warp"};
                for(int j=0;j<6;++j) knob(params[j],labels[j],{c.x+8*s+j*(c.w-16*s)/6,c.y+214*s,(c.w-16*s)/6,98*s},osc?nx::cyan:nx::violet);
            }
            const f32 y=canvas.y+342*s;
            Rect filter{canvas.x,y,cw,170*s}, envelope{canvas.x+cw+gap,y,cw,170*s};
            card(filter,"FILTER","Shape the brightness and movement");
            static const char* filters[]={"Low-pass 12","Band-pass 12","High-pass 12","Low-pass 24","High-pass 24","Notch"};
            choose(20,{filter.right()-140*s,filter.y+10*s,128*s,30*s},filters,6);
            const int fp[]={18,19,21,22}; const char* fl[]={"Cutoff","Resonance","Drive","Envelope"};
            for(int j=0;j<4;++j) knob(fp[j],fl[j],{filter.x+8*s+j*(filter.w-16*s)/4,y+60*s,(filter.w-16*s)/4,100*s});
            card(envelope,"AMPLITUDE ENVELOPE","From a short pluck to a slow bloom");
            const char* el[]={"Attack","Decay","Sustain","Release"};
            for(int j=0;j<4;++j) knob(24+j,el[j],{envelope.x+8*s+j*(envelope.w-16*s)/4,y+60*s,(envelope.w-16*s)/4,100*s},nx::cyan);
            Rect performance{canvas.x,y+182*s,canvas.w,94*s};
            rend_.roundRect(performance,9*s,nx::panel);
            rend_.textIn(fSmall_,{performance.x+12*s,performance.y+8*s,110*s,20*s},"PERFORMANCE",nx::muted,Align::Left,0);
            if(ui_.button(id(14),{performance.x+12*s,performance.y+40*s,92*s,32*s},"More controls")) st.soundTools=true;
            const f32 w=(performance.w-128*s)/7;
            for(int j=0;j<4;++j) {
                char label[48];snprintf(label,sizeof label,"Macro %d",j+1);
                for(int m=0;m<8;++m) if((int)get(68+3*m)==9+j && (int)get(69+3*m)>0) {
                    snprintf(label,sizeof label,"%d %s",j+1,kMatrixDst[clampv((int)get(69+3*m),0,19)]);break;
                }
                knob(94+j,label,{performance.x+120*s+j*w,performance.y+4*s,w,84*s});
            }
            knob(17,"Sub",{performance.x+120*s+4*w,performance.y+4*s,w,84*s});
            knob(16,"Noise",{performance.x+120*s+5*w,performance.y+4*s,w,84*s});
            knob(40,"Output",{performance.x+120*s+6*w,performance.y+4*s,w,84*s});
            Rect voicing{canvas.x,performance.bottom()+12*s,canvas.w,46*s};
            rend_.roundRect(voicing,8*s,nx::panel);
            rend_.textIn(fSmall_,{voicing.x+12*s,voicing.y,70*s,voicing.h},"VOICING",nx::muted,Align::Left,0);
            static const char* modes[]={"Polyphonic","Monophonic","Legato"};
            const f32 unit=(voicing.w-94*s)/4;
            choose(98,{voicing.x+82*s,voicing.y+7*s,unit-8*s,32*s},modes,3);
            const int vparams[]={38,39,99}; const char* vnames[]={"Glide","Voices","Bend"};
            for(int j=0;j<3;++j) {
                const f32 x=voicing.x+82*s+(j+1)*unit;
                rend_.textIn(fSmall_,{x,voicing.y,48*s,voicing.h},vnames[j],nx::muted,Align::Left,0);
                f64 value=get(vparams[j]);const auto& info=inst->paramInfo(vparams[j]);
                if(ui_.dragNumber(id(100+vparams[j]),{x+48*s,voicing.y+7*s,unit-56*s,32*s},&value,info.min,info.max,j?1:5,j?"%.0f":"%.0f ms")) commit(vparams[j],info.isInt?(f32)std::round(value):(f32)value);
            }
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
    } else if(st.page==3) {
#include "spectra_arp_studio.inc"
    } else if(st.page==2 && !st.motionTools) {
#include "spectra_motion.inc"
    } else {
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
