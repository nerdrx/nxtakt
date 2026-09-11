#pragma once
#include "host.h"

namespace lat::detail {

// Original synthesized drum kits; no recordings or external assets are used.
// Kit 0/1/2: 808-style / 707-style / 909-style.
// MIDI: 36 kick, 38 snare, 39 clap, 42 closed hat, 46 open hat,
//       45 tom, 37 rim, 56 cowbell. Notes are one-shots.
// Parameters: 0 kit, 1 output dB; 2 + voice*3 = level dB, tune st, decay.
PluginDesc drumMachineDesc();
std::unique_ptr<PluginInstance> makeDrumMachine(const PluginDesc& desc);

} // namespace lat::detail
