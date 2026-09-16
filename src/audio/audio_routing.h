#pragma once

#include <array>
#include <string>
#include <vector>

namespace lat {

struct AudioPortChoice {
    std::string value;
    std::string label;
};

struct AudioRouteSnapshot {
    std::vector<AudioPortChoice> outputs;
    std::vector<AudioPortChoice> inputs;
    std::array<std::string, 4> current{};
    std::string error;
    bool connected = false;
};

AudioRouteSnapshot inspectAudioRoutes(int enginePid);
bool applyAudioRoutes(int enginePid, const std::array<std::string, 4>& requested,
                      std::string& error);
std::vector<AudioPortChoice> listAlsaDevices(bool input);

} // namespace lat
