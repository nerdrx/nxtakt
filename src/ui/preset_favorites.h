#pragma once

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace lat {

// Small, deliberately boring persistent set for factory: and user: preset ids.
// The caller owns the path, so XDG policy stays outside this helper.
class PresetFavorites {
public:
    explicit PresetFavorites(std::filesystem::path path) : path_(std::move(path)) {
        load();
    }

    bool contains(const std::string& key) const { return favorites_.count(key) != 0; }

    // Returns true only when the new set was persisted. A failed save leaves the
    // in-memory set and the previous file unchanged.
    bool toggle(const std::string& key) {
        if (!validKey(key)) return false;
        if (!contains(key) && favorites_.size() >= kMaxEntries) return false;
        const auto old = favorites_;
        if (favorites_.erase(key) == 0) favorites_.insert(key);
        if (save()) return true;
        favorites_ = old;
        return false;
    }

private:
    static constexpr std::size_t kMaxEntries = 4096;
    static constexpr std::size_t kMaxKeyBytes = 4096;

    static bool validKey(const std::string& key) {
        const bool prefixed = key.rfind("factory:", 0) == 0 || key.rfind("user:", 0) == 0;
        const auto colon = key.find(':');
        return key.size() <= kMaxKeyBytes && prefixed && colon + 1 < key.size() &&
               key.find('\n') == std::string::npos && key.find('\r') == std::string::npos;
    }

    void load() {
        std::ifstream in(path_);
        if (!in) return;
        std::set<std::string> loaded;
        std::string line;
        while (loaded.size() < kMaxEntries && std::getline(in, line)) {
            if (line.size() > kMaxKeyBytes * 2 + 8) continue;
            std::istringstream parsed(line);
            std::string key, extra;
            if (!(parsed >> std::quoted(key)) || (parsed >> extra) || !validKey(key)) continue;
            loaded.insert(std::move(key));
        }
        favorites_ = std::move(loaded);
    }

    bool save() const {
        std::error_code ec;
        if (path_.has_parent_path())
            std::filesystem::create_directories(path_.parent_path(), ec);
        if (ec) return false;
        const auto temp = path_.string() + ".tmp";
        std::ofstream out(temp, std::ios::trunc);
        if (!out) return false;
        for (const auto& key : favorites_) out << std::quoted(key) << '\n';
        out.flush();
        if (!out) {
            out.close();
            std::filesystem::remove(temp, ec);
            return false;
        }
        out.close();
        std::filesystem::rename(temp, path_, ec);
        const bool renamed = !ec;
        if (!renamed) {
            std::error_code cleanup;
            std::filesystem::remove(temp, cleanup);
        }
        return renamed;
    }

    std::filesystem::path path_;
    std::set<std::string> favorites_;
};

} // namespace lat
