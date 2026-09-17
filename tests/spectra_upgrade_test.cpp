// Integrated Spectra FX through the public internal-device factory.
#include "../src/plugin/host.h"
#include "../src/ui/spectra_compare.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
using namespace lat;
namespace {
int passed = 0, failed = 0;
void check(bool ok, const char* label) {
    if (ok) ++passed;
    else { ++failed; std::fprintf(stderr, "FAIL: %s\n", label); }
}
std::unique_ptr<PluginInstance> synth(int rate = 48000) {
    PluginDesc desc; desc.uri = "nxtakt:spectra"; desc.format = PluginFormat::Internal;
    auto p = detail::instantiateInternal(desc, rate, 4096);
    if (!p) { std::fprintf(stderr, "Spectra unavailable\n"); std::exit(2); }
    p->setParam(27, 1.f); // Short release isolates effect tails.
    return p;
}
struct Audio { std::vector<float> l, r; explicit Audio(int n) : l(n), r(n) {} };
Audio render(PluginInstance& p, int count, int block = 256, int off = 1200, int panic = -1) {
    const u8 on[]{0x90,60,110}, stop[]{0x80,60,0}, allOff[]{0xb0,120,0};
    p.midi(on,3,0); if (off >= 0) p.midi(stop,3,off); if (panic >= 0) p.midi(allOff,3,panic);
    Audio a(count);
    for (int start = 0; start < count; start += block) {
        f32* out[]{a.l.data()+start,a.r.data()+start};
        p.process(nullptr,out,2,std::min(block,count-start));
    }
    return a;
}
double energy(const Audio& a, int from=0, int to=-1) {
    if (to < 0) to = (int)a.l.size();
    double e=0; for (int i=from;i<to;++i) e+=double(a.l[i])*a.l[i]+double(a.r[i])*a.r[i];
    return e/std::max(1,2*(to-from));
}
bool same(const Audio& a,const Audio& b) {
    return a.l==b.l && a.r==b.r;
}
bool finite(const Audio& a) {
    for (int i=0;i<(int)a.l.size();++i)
        if (!std::isfinite(a.l[i]) || !std::isfinite(a.r[i]) ||
            std::abs(a.l[i])>10 || std::abs(a.r[i])>10) return false;
    return true;
}
void enable(PluginInstance& p) {
    p.setParam(125,.35f); p.setParam(129,.3f); p.setParam(130,80.f);
    p.setParam(133,.35f); p.setParam(135,.8f);
}
}
int main() {
    auto dry=synth();
    check(dry->paramCount()==137,"137 append-only parameters");
    const float defaults[]{0,.35f,.4f,.8f,0,375,0,.35f,0,.55f,.5f,.45f};
    for (int i=0;i<12;++i) check(dry->getParam(125+i)==defaults[i],"FX default matches contract");
    const auto reference=render(*dry,18000);
    auto inactive=synth();
    for (int id:{126,127,128,130,131,132,134,135,136})
        inactive->setParam(id,inactive->paramInfo(id).max);
    check(same(reference,render(*inactive,18000)),"zero mixes preserve exact original audio even at extreme settings");
    check(energy(reference,10000)<1e-12,"dry short release has no late tail");

    for (int effect:{125,129,133}) {
        auto p=synth(); p->setParam(effect,.65f); p->setParam(130,50.f);
        auto a=render(*p,48000);
        check(finite(a),"each module is finite and bounded");
        auto d=synth(); auto da=render(*d,48000);
        check(!same(a,da),"each module audibly changes its input");
        if (effect!=125) check(energy(a,10000)>1e-10,"delay and reverb retain a musical tail");
    }
    auto wide=synth(); wide->setParam(125,1.f); wide->setParam(128,1.f);
    auto wa=render(*wide,12000,256,10000);
    check(wa.l!=wa.r,"chorus creates stereo movement from a centered oscillator");

    // Wet-only first echo must land precisely after Time; no block quantization.
    auto echo=synth(); echo->setParam(129,1.f); echo->setParam(130,20.f); echo->setParam(132,.5f);
    auto ea=render(*echo,5000,127,200);
    check(energy(ea,0,960)==0,"20 ms delay is silent before its first echo");
    check(energy(ea,960,1700)>1e-8,"first echo arrives on time");
    double firstRight=0,secondRight=0;
    for(int i=960;i<1700;++i) firstRight+=ea.r[i]*ea.r[i];
    for(int i=1920;i<2650;++i) secondRight+=ea.r[i]*ea.r[i];
    check(firstRight==0 && secondRight>1e-8,"mono echoes alternate left then right");
    auto sync=synth(); sync->setParam(129,1.f); sync->setParam(131,7.f);
    sync->setTransport(120.,0.,true);
    auto sa=render(*sync,9000,333,200);
    check(energy(sa,0,6000)==0 && energy(sa,6000,6800)>1e-8,"tempo sync sixteenth at 120 BPM is 6000 samples");

    auto regular=synth();enable(*regular);auto ra=render(*regular,36000,256);
    for(int block:{1,7,333,1024}) {
        auto p=synth(); enable(*p);
        check(same(ra,render(*p,36000,block)),"FX are independent of audio block size");
    }
    regular->prepare(48000,4096);
    check(same(ra,render(*regular,36000,256)),"prepare deterministically resets FX histories and phase");
    auto panic=synth(); enable(*panic); auto pa=render(*panic,24000,173,1200,10000);
    check(energy(pa,10000)==0,"CC120 kills every delay and reverb tail at the exact sample");

    for(int rate:{44100,96000}) {
        auto p=synth(rate);enable(*p);
        p->setParam(132,.9f);p->setParam(134,1.f);p->setParam(135,1.f);
        check(finite(render(*p,rate*2,383)),"maximum feedback stays finite at alternate sample rates");
    }
    // Parameter persistence uses the same id/value pairs as project files.
    auto saved=synth();enable(*saved);auto restored=synth();
    for(int id=0;id<saved->paramCount();++id) restored->setParam(id,saved->getParam(id));
    check(same(render(*saved,18000),render(*restored,18000)),"FX parameter roundtrip reproduces the same sound");
    restored->loadPreset(0);
    check(restored->getParam(125)==0.f && restored->getParam(129)==0.f && restored->getParam(133)==0.f,
          "legacy preset loading resets every new effect to bypass");
    for(int id=125;id<=136;++id) {
        restored->setParam(id,1e6f);check(restored->getParam(id)==restored->paramInfo(id).max,"FX upper clamp");
        restored->setParam(id,-1e6f);check(restored->getParam(id)==restored->paramInfo(id).min,"FX lower clamp");
    }
    // Compare actual Spectra patches, including state absent from parameter pairs.
    auto compared=synth();
    const auto patchA=captureSpectraSound(*compared);
    compared->prepare(48000,4096);
    const auto audioA=render(*compared,18000);
    enable(*compared);
    check(compared->setStateString("nxspc1;cc=74;lfo1=0123456789abcdef"),"comparison non-parameter state accepted");
    const auto patchB=captureSpectraSound(*compared);
    check(restoreSpectraSound(*compared,patchA),"comparison recalls A");
    check(compared->stateString()==patchA.state,"comparison clears B modulation state when A has no state");
    compared->prepare(48000,4096);
    check(same(audioA,render(*compared,18000)),"recalled A produces identical audio after phase reset");
    check(restoreSpectraSound(*compared,patchB),"comparison recalls B");
    const auto recalledB=captureSpectraSound(*compared);
    check(recalledB.params==patchB.params && recalledB.state==patchB.state,"comparison restores every B parameter and non-parameter field");
    std::printf("Spectra studio FX: %d passed, %d failed\n",passed,failed);
    return failed ? 1 : 0;
}
