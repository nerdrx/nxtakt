// The two translation units the Windows GUI cannot compile at all, replaced by
// their "not on this platform" implementations. See README.md.
//
//   src/audio/midi_in.cpp   ALSA sequencer      -> MidiInput never starts
//   src/plugin/lv2_host.cpp lilv + dlfcn + LV2  -> the scan finds no LV2
//
// Both replace a whole file rather than adding #ifdefs to one, because both of
// those files are ALSA/lilv from their first line to their last: there is no
// portable half to preserve. The declarations they satisfy live in
// src/audio/midi_in.h and src/plugin/host.h and are not touched.
//
// This is where a WinMM MIDI input goes when somebody writes it.
#if !defined(_WIN32)
#error "tools/mingw-shim/win is for the Windows cross build only"
#endif

#include "../../../src/audio/backend.h"
#include "../../../src/audio/midi_in.h"
#include "../../../src/plugin/host.h"

namespace lat {

// ---- ALSA's log handler ----------------------------------------------------
// main() installs this before anything can touch ALSA, because plugins drag
// libasound in whether or not it is our audio backend. There is no libasound
// here, so there is nothing to quieten -- but main() is shared source and calls
// it unconditionally, and a no-op is a truer answer than a #ifdef in main.cpp.
void alsaInstallLogHandler() {}

// ---- MIDI input ------------------------------------------------------------
// EngineHandle::openLocalEngine() already treats false as "no MIDI input -
// continuing without it", which is the same thing that happens on a Linux box
// with no snd-seq. Nothing else in the app changes.
bool MidiInput::start(Engine&) {
    LOGW("no MIDI input on Windows yet (no WinMM/WinRT backend) - continuing without it");
    return false;
}

void MidiInput::stop() {}

// Never called: the reader thread is only spawned by a successful start().
// It exists because midi_in.h declares it and a private member function with
// no definition is an error the moment anything takes its address.
void MidiInput::run() {}

// ---- LV2 -------------------------------------------------------------------
// LV2 hosting needs lilv, which has no Windows cross build here. CLAP covers
// the plugin story on Windows and its loader works (see dlfcn.h in this
// directory), so the registry is not empty: internal devices plus any CLAP
// found on disk.
namespace detail {

void scanLV2(std::vector<PluginDesc>&) {
    LOGI("lv2: not available on Windows (no lilv) - internal devices and CLAP only");
}

std::unique_ptr<PluginInstance> instantiateLV2(const PluginDesc& d, f64, int) {
    LOGE("lv2: cannot instantiate '%s' on Windows - this set needs a Linux build "
         "or a CLAP/internal replacement for that device", d.uri.c_str());
    return nullptr;
}

} // namespace detail
} // namespace lat
