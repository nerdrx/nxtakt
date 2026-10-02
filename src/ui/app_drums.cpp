#include "app.h"
#include "../plugin/drum_machine.h"

namespace lat {

PluginInstance* App::drumDeviceFor(int track) const {
    if (track < 0 || track >= (int)ses_.tracks.size()) return nullptr;
    for (const auto& device : ses_.tracks[(size_t)track].devices)
        if (device.desc.uri == "nxtakt:drums" && device.inst) return device.inst.get();
    return nullptr;
}

void App::addDrumTrack() {
    if(!closeFocusedSpectra()) return;
    if (ses_.tracks.size() >= kMaxTracks) { status_ = "Track limit reached"; return; }
    undoPoint("add drum track");
    addTrack();
    const int track = (int)ses_.tracks.size() - 1;
    ses_.tracks[(size_t)track].name = "Drums";
    ses_.tracks[(size_t)track].colorIdx = 1;
    addDevice(track, detail::drumMachineDesc());
    if (!drumDeviceFor(track)) return;
    if (ses_.scenes.empty()) addScene();
    const int slot=clampv(selSlot_,0,(int)ses_.scenes.size()-1);
    createMidiClip(track, slot, false); // The complete track creation is one undo action.
    ses_.tracks[(size_t)track].slots[slot].name = "Drum pattern";
    selectTrack(track);
    showDetail_ = true;
    detailTab_ = DetailTab::Clip;
    detailPattern_ = true;
    activateStudioWindow(1);
    midiInspectorPage_ = 0;
    drumEditor_ = 0;
    status_ = "Drum Machine ready: click steps, then launch the pattern";
}

} // namespace lat
