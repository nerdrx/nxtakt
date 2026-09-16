#pragma once

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace lat {

struct AudioSettings {
    std::array<std::string, 4> jackPorts{};
    std::string alsaOutput = "default";
    std::string alsaInput = "default";
};

inline std::filesystem::path audioSettingsPath() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "nxtakt" / "audio-settings.txt";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".config" / "nxtakt" / "audio-settings.txt";
    return std::filesystem::path("nxtakt") / "audio-settings.txt";
}

namespace audio_settings_detail {

constexpr std::size_t kMaxFileSize = 1024 * 1024;
constexpr std::size_t kMaxLineSize = 16 * 1024;
constexpr std::size_t kMaxValueSize = 4096;

inline bool validValue(std::string_view value, bool allowEmpty) {
    if (!allowEmpty && value.empty()) return false;
    if (value.size() > kMaxValueSize) return false;
    return value.find('\n') == std::string_view::npos &&
           value.find('\r') == std::string_view::npos &&
           value.find('\0') == std::string_view::npos;
}

inline bool parseLine(std::string_view line, std::string& key, std::string& value) {
    const std::size_t split = line.find_first_of(" \t");
    if (split == std::string_view::npos || split == 0) return false;
    key.assign(line.substr(0, split));
    std::size_t pos = split;
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    if (pos == line.size() || line[pos++] != '"') return false;

    value.clear();
    bool escaped = false;
    for (; pos < line.size(); ++pos) {
        const char c = line[pos];
        if (escaped) {
            if (c != '\\' && c != '"') return false;
            value.push_back(c);
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            ++pos;
            while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
            return pos == line.size();
        } else {
            value.push_back(c);
        }
    }
    return false;
}

inline std::string quote(std::string_view value) {
    std::string result = "\"";
    for (const char c : value) {
        if (c == '\\' || c == '"') result.push_back('\\');
        result.push_back(c);
    }
    result += "\"\n";
    return result;
}

} // namespace audio_settings_detail

inline AudioSettings loadAudioSettings(
    const std::filesystem::path& path = audioSettingsPath()) {
    AudioSettings settings;
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > audio_settings_detail::kMaxFileSize) return settings;

    std::ifstream input(path, std::ios::binary);
    if (!input) return settings;
    std::string line, key, value;
    while (std::getline(input, line)) {
        if (line.size() > audio_settings_detail::kMaxLineSize) continue;
        if (!audio_settings_detail::parseLine(line, key, value)) continue;
        if (key.rfind("jack", 0) == 0 && key.size() == 5 && key[4] >= '0' && key[4] <= '3') {
            if (audio_settings_detail::validValue(value, true)) settings.jackPorts[key[4] - '0'] = value;
        } else if (key == "alsa_output") {
            if (audio_settings_detail::validValue(value, false)) settings.alsaOutput = value;
        } else if (key == "alsa_input") {
            if (audio_settings_detail::validValue(value, false)) settings.alsaInput = value;
        }
    }
    return settings;
}

inline bool saveAudioSettings(
    const AudioSettings& settings,
    const std::filesystem::path& path = audioSettingsPath()) {
    for (const auto& port : settings.jackPorts)
        if (!audio_settings_detail::validValue(port, true)) return false;
    if (!audio_settings_detail::validValue(settings.alsaOutput, false) ||
        !audio_settings_detail::validValue(settings.alsaInput, false)) return false;

    std::error_code ec;
    const auto parent = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    std::filesystem::create_directories(parent, ec);
    if (ec) return false;

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temporary = parent / (path.filename().string() + ".tmp-" + std::to_string(stamp));
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        for (std::size_t i = 0; i < settings.jackPorts.size(); ++i)
            output << "jack" << i << ' ' << audio_settings_detail::quote(settings.jackPorts[i]);
        output << "alsa_output " << audio_settings_detail::quote(settings.alsaOutput)
               << "alsa_input " << audio_settings_detail::quote(settings.alsaInput);
        output.flush();
        if (!output) {
            std::filesystem::remove(temporary, ec);
            return false;
        }
        output.close();
        if (!output) {
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }
    std::filesystem::rename(temporary, path, ec);
    const bool renamed = !ec;
    if (!renamed) std::filesystem::remove(temporary, ec);
    return renamed;
}

} // namespace lat
