#include "audio/audio_settings.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

int main() {
    const auto dir = std::filesystem::temp_directory_path() / "nxtakt-audio-settings-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto path = dir / "audio-settings.txt";

    lat::AudioSettings expected;
    expected.jackPorts = {"system:playback_1", "", "-", "port with \"quotes\""};
    expected.alsaOutput = "hw:USB Audio,0";
    expected.alsaInput = "input with spaces";
    assert(lat::saveAudioSettings(expected, path));
    assert(lat::loadAudioSettings(path).jackPorts == expected.jackPorts);
    assert(lat::loadAudioSettings(path).alsaOutput == expected.alsaOutput);
    assert(lat::loadAudioSettings(path).alsaInput == expected.alsaInput);

    auto oversized = expected;
    oversized.alsaOutput = std::string(4097, 'x');
    assert(!lat::saveAudioSettings(oversized, path));
    assert(lat::loadAudioSettings(path).jackPorts == expected.jackPorts);
    assert(lat::loadAudioSettings(path).alsaOutput == expected.alsaOutput);
    assert(lat::loadAudioSettings(path).alsaInput == expected.alsaInput);

    {
        std::ofstream file(path);
        file << "jack0 \"valid\"\nunknown \"ignored\"\njack1 broken\n"
             << "alsa_output \"\"\nalsa_input \"valid input\" trailing\n"
             << "jack2 \"-\"\n";
    }
    const auto invalid = lat::loadAudioSettings(path);
    assert(invalid.jackPorts[0] == "valid");
    assert(invalid.jackPorts[1].empty());
    assert(invalid.jackPorts[2] == "-");
    assert(invalid.alsaOutput == "default");
    assert(invalid.alsaInput == "default");

    const auto missing = lat::loadAudioSettings(dir / "missing.txt");
    assert(missing.alsaOutput == "default" && missing.alsaInput == "default");
    assert((missing.jackPorts == std::array<std::string, 4>{}));

    const auto old = dir / "existing";
    std::filesystem::create_directory(old);
    lat::AudioSettings replacement;
    assert(!lat::saveAudioSettings(replacement, old));
    assert(std::filesystem::is_directory(old));

    std::filesystem::remove_all(dir);
}
