// Standalone scanner. Build it with `make build/plugin_scan` -- the recipe is
// in the Makefile because the source list is not two files any more: the
// registry reaches into the LV2 backend, the CLAP backend and the internal
// devices, so a hand-rolled g++ line goes stale the moment a backend is added,
// and a scanner that does not link every backend reports a short list without
// saying so.
//
//   plugin_scan            list everything
//   plugin_scan <uri>      also instantiate that plugin and dump its params
#include "../src/plugin/host.h"
#include <cmath>
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    lat::PluginRegistry reg;
    reg.scan();

    const auto& list = reg.plugins();
    std::printf("\n%-40s %-4s %-11s %3s %3s %4s %5s  %s\n",
                "NAME", "FMT", "KIND", "IN", "OUT", "MIDI", "PARAM", "URI");
    std::printf("%s\n", std::string(140, '-').c_str());
    for (const lat::PluginDesc& d : list) {
        std::printf("%-40.40s %-4s %-11s %3d %3d %4s %5d  %s\n",
                    d.name.c_str(), lat::formatName(d.format), lat::kindName(d.kind),
                    d.audioIn, d.audioOut, d.hasMidiIn ? "yes" : "-", d.paramCount,
                    d.uri.c_str());
    }

    int fx = 0, instr = 0, midi = 0;
    for (const lat::PluginDesc& d : list) {
        if (d.kind == lat::PluginKind::Effect) ++fx;
        if (d.kind == lat::PluginKind::Instrument) ++instr;
        if (d.hasMidiIn) ++midi;
    }
    std::printf("\n%zu plugins: %d effects, %d instruments, %d with MIDI in\n",
                list.size(), fx, instr, midi);

    if (argc < 2) return 0;

    // Optional smoke test: load one plugin and push a block of silence
    // through it, which exercises port connection and the RT path.
    const lat::PluginDesc* d = reg.find(argv[1]);
    if (!d) { std::printf("\nno such plugin: %s\n", argv[1]); return 1; }

    auto inst = reg.instantiate(*d, 48000.0, 512);
    if (!inst) { std::printf("\ninstantiate failed\n"); return 1; }

    std::printf("\n%s — %d params\n", d->name.c_str(), inst->paramCount());
    for (int i = 0; i < inst->paramCount(); ++i) {
        const lat::ParamInfo& p = inst->paramInfo(i);
        std::printf("  [%2d] %-28.28s %10.4f  [%g .. %g] %-6s%s%s%s\n",
                    i, p.name.c_str(), (double)inst->getParam(i),
                    (double)p.min, (double)p.max, p.unit.c_str(),
                    p.isBool ? " bool" : "", p.isInt ? " int" : "",
                    p.isLogarithmic ? " log" : "");
    }

    // A 440 Hz sine through an in-place stereo buffer, which is exactly how the
    // engine will call it. Comparing RMS before/after proves the plugin ran.
    static float l[512], r[512];
    double rmsIn = 0.0;
    for (int i = 0; i < 512; ++i) {
        l[i] = r[i] = 0.25f * (float)std::sin(2.0 * 3.14159265358979 * 440.0 * i / 48000.0);
        rmsIn += (double)l[i] * l[i];
    }
    float* io[2] = { l, r };
    const float* cio[2] = { l, r };
    inst->process(cio, io, 2, 512);

    double rmsOut = 0.0;
    for (int i = 0; i < 512; ++i) rmsOut += (double)l[i] * l[i];
    std::printf("512 frames in place: rms in %.5f -> out %.5f\n",
                std::sqrt(rmsIn / 512.0), std::sqrt(rmsOut / 512.0));

    inst->setBypassed(true);
    inst->process(cio, io, 2, 512);
    std::printf("bypassed: out[100]=%.5f (expected unchanged)\n", (double)l[100]);
    return 0;
}
