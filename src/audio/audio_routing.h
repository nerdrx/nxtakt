#pragma once

#include <array>
#include <string>
#include <vector>

namespace lat {

struct AudioPortChoice {
    std::string value;
    std::string label;
};

struct AudioDeviceChoice {
    std::string label;
    std::string left;
    std::string right;
};

struct AudioRouteSnapshot {
    std::vector<AudioPortChoice> outputs;
    std::vector<AudioPortChoice> inputs;
    std::array<std::string, 4> current{};
    std::string error;
    bool connected = false;
    bool discordBusConnected = false;
};

AudioRouteSnapshot inspectAudioRoutes(int enginePid);
bool applyAudioRoutes(int enginePid, const std::array<std::string, 4>& requested,
                      std::string& error, bool discordCaptureBus = false);
std::vector<AudioDeviceChoice> listJackDevices(bool input);
std::vector<AudioPortChoice> listAlsaDevices(bool input);

} // namespace lat
