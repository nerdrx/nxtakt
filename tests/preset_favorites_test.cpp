#include "../src/ui/preset_favorites.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir = std::filesystem::temp_directory_path() /
                     ("nxtakt-preset-favorites-test-" + std::to_string(stamp));
    assert(std::filesystem::create_directory(dir));
    const auto path = dir / "favorites.conf";

    {
        lat::PresetFavorites favorites(path);
        assert(favorites.toggle("factory:Bright \"Lead\""));
        assert(favorites.toggle("user:My, Preset"));
        assert(favorites.contains("factory:Bright \"Lead\""));
    }
    {
        lat::PresetFavorites favorites(path);
        assert(favorites.contains("factory:Bright \"Lead\""));
        assert(favorites.contains("user:My, Preset"));
        assert(favorites.toggle("factory:Bright \"Lead\""));
        assert(!favorites.contains("factory:Bright \"Lead\""));
    }

    {
        std::ofstream out(path, std::ios::trunc);
        out << "not-a-preset\n\"factory:\"\n\"user:\"\n\"factory:Kept\" trailing\n\"user:Valid\"\n";
    }
    lat::PresetFavorites malformed(path);
    assert(!malformed.contains("not-a-preset"));
    assert(!malformed.contains("factory:"));
    assert(!malformed.contains("user:"));
    assert(malformed.contains("user:Valid"));

    std::ofstream(path, std::ios::trunc) << "\"factory:Old\"\n";
    std::filesystem::create_directory(path.string() + ".tmp");
    lat::PresetFavorites failed(path); // The temp path is blocked by a directory.
    assert(!failed.toggle("factory:New"));
    assert(failed.contains("factory:Old"));
    assert(!failed.contains("factory:New"));
    std::ifstream preserved(path);
    std::string oldLine;
    std::getline(preserved, oldLine);
    assert(oldLine == "\"factory:Old\"");
    std::filesystem::remove_all(dir);
}
