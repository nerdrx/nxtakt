// Spacious modulation overview. Included inside drawSpectraStudio(); existing
// state editors remain available through Detailed tools. Parameter indices and
// enum order are shared with the original Spectra editor above.
{
    const f32 gap = 12 * s;
    const Rect heading{content.x, content.y, content.w, 38 * s};
    rend_.textIn(fBold_, {heading.x, heading.y, heading.w - 164*s, 20*s},
                 "Movement & expression", nx::text, Align::Left, 0);
    rend_.textIn(fSmall_, {heading.x, heading.y + 21*s, heading.w - 164*s, 17*s},
                 "Shape a source, then connect it to your sound.", nx::muted, Align::Left, 0);
    if (ui_.button(id(3000), {heading.right()-148*s, heading.y, 148*s, 34*s}, "Detailed tools")) {
        st.motionTools = true;
        st.scroll = 0;
    }
    if (ui_.hovered({heading.right()-148*s, heading.y, 148*s, 34*s}))
        ui_.tip = "Custom LFO drawing, drag-to-assign modulation, macros and tuning";

    const Rect viewport{content.x, heading.bottom()+10*s, content.w,
                        std::max(1.f, content.bottom()-heading.bottom()-10*s)};
    const f32 lfoH = 216*s, envH = 166*s, matrixH = 404*s;
    const f32 needed = lfoH + envH + matrixH + 2*gap;
    const f32 maxScroll = std::max(0.f, needed - viewport.h);
    st.scroll = clampv(st.scroll, 0.f, maxScroll);
    const f32 gutter = maxScroll > 0.f ? 28*s : 0.f;
    const Rect canvas{viewport.x, viewport.y-st.scroll, viewport.w-gutter, needed};
    rend_.pushClip({viewport.x, viewport.y, canvas.w, viewport.h});

    const int rateIds[3] = {pLfoRate, pL2Rate, pL3Rate};
    const int syncIds[3] = {pLfoSync, pL2Sync, pL3Sync};
    const int shapeIds[3] = {pLfoShape, pL2Shape, pL3Shape};
    const int modeIds[3] = {pL1Mode, pL2Mode, pL3Mode};
    const char* lfoTitles[3] = {"LFO 1", "LFO 2", "LFO 3"};
    const char* modeNames[2] = {"Loop", "One shot"};
    const f32 lfoW = (canvas.w - 2*gap)/3.f;
    for (int lfo = 0; lfo < 3; ++lfo) {
        const Rect c{canvas.x+lfo*(lfoW+gap), canvas.y, lfoW, lfoH};
        if (c.bottom() < viewport.y || c.y > viewport.bottom()) continue;
        const Col accent = lfo == 1 ? nx::cyan : nx::violetSoft;
        card(c, lfoTitles[lfo], "Shape / speed / playback");
        rend_.roundRect({c.x+14*s, c.y+53*s, c.w-28*s, 2*s}, s, accent.alpha(0.55f));
        rend_.textIn(fSmall_, {c.x+14*s,c.y+59*s,c.w-28*s,16*s},
                     "Waveform", nx::muted, Align::Left, 0);
        choose(shapeIds[lfo], {c.x+12*s,c.y+79*s,c.w-24*s,32*s}, kShapeName, 6);
        const f32 rateW = std::min(92*s, c.w*0.42f);
        const Rect rate{c.x+8*s, c.y+115*s, rateW, 94*s};
        const int sync = clampv((int)std::lround(get(syncIds[lfo])), 0, 9);
        if (sync == 0) {
            knob(rateIds[lfo], "Rate", rate, accent);
        } else {
            // The free-rate parameter is inactive in sync mode. Show what the
            // engine is actually using instead of offering an inert dial.
            rend_.textIn(fBig_, {rate.x,rate.y+15*s,rate.w,28*s},
                         kSyncDiv[sync], accent, Align::Center, 0);
            rend_.textIn(fSmall_, {rate.x,rate.y+51*s,rate.w,20*s},
                         "Song tempo", nx::muted, Align::Center, 0);
        }
        const f32 rightX = rate.right()+4*s, rightW = c.right()-rightX-12*s;
        rend_.textIn(fSmall_, {rightX,c.y+116*s,rightW,16*s},
                     "Timing", nx::muted, Align::Left, 0);
        choose(syncIds[lfo], {rightX,c.y+136*s,rightW,30*s}, kSyncDiv, 10);
        choose(modeIds[lfo], {rightX,c.y+175*s,rightW,30*s}, modeNames, 2);
        if ((int)std::lround(get(shapeIds[lfo])) == 5 && ui_.hovered(c))
            ui_.tip = "Custom waveform is active. Open Detailed tools to draw its steps.";
    }

    const f32 envY = canvas.y+lfoH+gap;
    const f32 envW = (canvas.w-gap)*0.5f;
    const int envIds[2] = {pE2Attack, pE3Attack};
    const char* envTitles[2] = {"Envelope 2", "Envelope 3"};
    const char* envLabels[4] = {"Attack", "Decay", "Sustain", "Release"};
    for (int e = 0; e < 2; ++e) {
        const Rect c{canvas.x+e*(envW+gap),envY,envW,envH};
        if (c.bottom() < viewport.y || c.y > viewport.bottom()) continue;
        card(c, envTitles[e], e == 0 ? "Filter and wavetable movement" : "An extra contour for your routes");
        const f32 w = (c.w-16*s)/4.f;
        for (int k = 0; k < 4; ++k)
            knob(envIds[e]+k, envLabels[k], {c.x+8*s+k*w,c.y+57*s,w,102*s}, nx::cyan);
    }

    const Rect matrix{canvas.x,envY+envH+gap,canvas.w,matrixH};
    if (matrix.bottom() >= viewport.y && matrix.y <= viewport.bottom()) {
        card(matrix, "Modulation routes", "Pick a source and destination. Amount controls strength and direction.");
        const f32 pad = 14*s, numberW = 28*s, rowGap = 8*s;
        const f32 available = matrix.w - 2*pad - numberW - 3*rowGap;
        const f32 sourceW = available*0.27f, destW = available*0.33f;
        const f32 amountW = available*0.21f, curveW = available-sourceW-destW-amountW;
        const f32 sourceX = matrix.x+pad+numberW;
        const f32 destX = sourceX+sourceW+rowGap;
        const f32 amountX = destX+destW+rowGap;
        const f32 curveX = amountX+amountW+rowGap;
        const f32 labelY = matrix.y+58*s;
        rend_.textIn(fSmall_, {sourceX,labelY,sourceW,18*s}, "Source", nx::muted, Align::Left, 0);
        rend_.textIn(fSmall_, {destX,labelY,destW,18*s}, "Destination", nx::muted, Align::Left, 0);
        rend_.textIn(fSmall_, {amountX,labelY,amountW,18*s}, "Amount", nx::muted, Align::Left, 0);
        rend_.textIn(fSmall_, {curveX,labelY,curveW,18*s}, "Response", nx::muted, Align::Left, 0);
        for (int slot = 0; slot < 8; ++slot) {
            const int src = pM1Src+3*slot, dst = src+1, amt = src+2, curve = pM1Curve+slot;
            const f32 y = matrix.y+81*s+slot*38*s;
            if (y+34*s < viewport.y || y > viewport.bottom()) continue;
            const bool connected = get(src)>0.5f && get(dst)>0.5f;
            const Rect row{matrix.x+8*s,y-2*s,matrix.w-16*s,36*s};
            if (connected) rend_.roundRect(row,5*s,nx::violet.alpha(0.065f));
            char number[8]; std::snprintf(number,sizeof number,"%d",slot+1);
            rend_.textIn(fSmall_,{matrix.x+pad,y,numberW-6*s,32*s},number,
                         connected?nx::cyan:nx::muted,Align::Center,0);
            const int sources = src < pc ? clampv((int)inst->paramInfo(src).max+1,1,18) : 18;
            choose(src,{sourceX,y,sourceW,32*s},kMatrixSrc,sources);
            choose(dst,{destX,y,destW,32*s},kMatrixDst,20);
            if (amt < pc) {
                const ParamInfo& info = inst->paramInfo(amt);
                f64 value = get(amt);
                const Rect amount{amountX,y,amountW,32*s};
                ui_.fieldWell(amount,0.f);
                if (ui_.dragNumber(id(100+amt),amount,&value,info.min,info.max,
                                   0.005,"%+.2f",Align::Center,nullptr,0.01,info.def))
                    commit(amt,(f32)value);
                if (ui_.hovered(amount))
                    ui_.tip = "Modulation amount: positive follows the source; negative reverses it. Zero has no effect.";
            }
            choose(curve,{curveX,y,curveW,32*s},kCurveName,3);
        }
    }
    rend_.popClip();

    // Child controls consume their wheel first. Remaining wheel movement
    // scrolls the overview; the rail is also directly draggable.
    if (viewport.contains(in.mx,in.my) && in.wheel != 0.f) {
        st.scroll = clampv(st.scroll-in.wheel*44*s,0.f,maxScroll);
        in.wheel = 0.f;
    }
    if (maxScroll > 0.f) {
        const Rect rail{viewport.right()-24*s,viewport.y,24*s,viewport.h};
        const f32 thumbH = std::max(40*s,viewport.h*viewport.h/needed);
        const f32 travel = std::max(1.f,rail.h-thumbH);
        const Rect thumb{rail.x,rail.y+travel*st.scroll/maxScroll,rail.w,thumbH};
        const u64 scrollId = id(3001);
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
}
