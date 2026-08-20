// Registry and per-format dispatch, and the generic user-preset bank.
// Everything here is GUI-thread only.
#include "host.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace lat {

// CLAP backend entry points. Declared here rather than alongside the LV2 pair
// in host.h so that adding a backend keeps touching exactly one file.
namespace detail {
    void scanCLAP(std::vector<PluginDesc>& out);
    std::unique_ptr<PluginInstance> instantiateCLAP(const PluginDesc& d, f64 sampleRate, int maxBlock);
}

const char* formatName(PluginFormat f) {
    switch (f) {
        case PluginFormat::LV2:  return "LV2";
        case PluginFormat::CLAP: return "CLAP";
        case PluginFormat::VST3: return "VST3";
        case PluginFormat::Internal: return "Internal";
    }
    return "?";
}

const char* kindName(PluginKind k) {
    switch (k) {
        case PluginKind::Effect:     return "effect";
        case PluginKind::Instrument: return "instrument";
        case PluginKind::Unknown:    return "unknown";
    }
    return "?";
}

void PluginRegistry::scan() {
    plugins_.clear();

    detail::scanInternal(plugins_);              // stock devices, no filesystem
    detail::scanLV2(plugins_);
    detail::scanCLAP(plugins_);                  // $CLAP_PATH, ~/.clap, /usr/lib/clap
    // TODO(vst3): detail::scanVST3(plugins_);   ~/.vst3, /usr/lib/vst3

    // Stable, case-insensitive order so the browser list does not reshuffle
    // between scans just because the filesystem walk changed. Stock devices sort
    // ahead of everything else: they are the ones a user reaches for without
    // knowing a name, and there are a handful of them against hundreds of
    // third-party plugins, so alphabetical order would bury them.
    std::sort(plugins_.begin(), plugins_.end(), [](const PluginDesc& a, const PluginDesc& b) {
        const bool ai = a.format == PluginFormat::Internal;
        const bool bi = b.format == PluginFormat::Internal;
        if (ai != bi) return ai;

        auto lower = [](const std::string& s) {
            std::string r = s;
            std::transform(r.begin(), r.end(), r.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            return r;
        };
        const std::string la = lower(a.name), lb = lower(b.name);
        if (la != lb) return la < lb;
        return a.uri < b.uri;
    });

    LOGI("plugin scan: %zu plugins", plugins_.size());
}

// URI aliases. One entry per scheme the project format has ever written for a
// plugin we still ship; the value is the scheme that means the same thing now.
//
// Only `lattice:` -> `nxtakt:` today, from the rename. Stock devices are named
// in every saved set by URI (see internal_devices.cpp), so this table is what
// keeps a set written before the rename able to find its own devices. It is
// append-only and permanent: a row may never be deleted, because the files it
// serves are the user's, they are not going to be migrated, and there is no
// version of "your Saturator is gone" that is acceptable.
//
// A scheme swap rather than a per-URI table so a device added later inherits
// the compatibility for free -- and it is safe to apply blindly because
// `lattice:` was never a scheme any third-party LV2 or CLAP plugin used; it
// only ever named our own.
namespace {
struct UriAlias { const char* from; const char* to; };
constexpr UriAlias kUriAliases[] = {
    { "lattice:", "nxtakt:" },
};
} // namespace

const PluginDesc* PluginRegistry::find(const std::string& uri) const {
    for (const PluginDesc& d : plugins_)
        if (d.uri == uri) return &d;

    // Exact match failed. Retry once per alias, so an old set resolves to the
    // canonical descriptor -- which is what the caller then instantiates and
    // what serializeDevices later writes back, upgrading the URI in place.
    for (const UriAlias& a : kUriAliases) {
        const size_t n = std::strlen(a.from);
        if (uri.compare(0, n, a.from) != 0) continue;
        const std::string canonical = std::string(a.to) + uri.substr(n);
        for (const PluginDesc& d : plugins_)
            if (d.uri == canonical) return &d;
    }
    return nullptr;
}

std::unique_ptr<PluginInstance> PluginRegistry::instantiate(const PluginDesc& d,
                                                            f64 sampleRate, int maxBlock) {
    switch (d.format) {
        case PluginFormat::LV2:
            return detail::instantiateLV2(d, sampleRate, maxBlock);
        case PluginFormat::CLAP:
            return detail::instantiateCLAP(d, sampleRate, maxBlock);
        case PluginFormat::Internal:
            // `this` is how a rack reaches the registry to build its own chain.
            // It is the only backend that needs it, and it needs it on the GUI
            // thread only -- see RackControl in host.h.
            return detail::instantiateInternal(d, sampleRate, maxBlock, this);
        case PluginFormat::VST3:
            // TODO(vst3): return detail::instantiateVST3(d, sampleRate, maxBlock);
            LOGE("VST3 hosting not implemented (%s)", d.name.c_str());
            return nullptr;
    }
    return nullptr;
}

// ===========================================================================
// THE USER PRESET BANK
//
// docs/SPECTRA-PARAMS.md, "The user-preset contract (host.h)" is the
// specification and this is its whole implementation. It is on PluginInstance
// and it is generic: it reads paramCount()/paramInfo()/getParam(), it writes
// setParam(), it carries stateString()/setStateString() verbatim, and it knows
// nothing about any particular device.
//
// A NOTE ON NUMBERS AND LOCALE, because this file writes a format a user can
// edit. `fmtParam` below is project.cpp's `fmtF32` — the shortest decimal that
// reads back bit-identical — copied rather than shared because host.cpp links
// into `nxtaktd`, which does not link src/core/project.cpp and should not start
// to for eight lines. Both writer and reader go through the C locale, which
// main.cpp pins with `setlocale(LC_NUMERIC, "C")` for exactly this reason: a
// preset written under de_DE that said `0,35` would be a preset no other
// machine could read.
// ===========================================================================

namespace {

constexpr u64 kMaxPresetFileBytes = 256ull * 1024ull;   // the contract's cap
constexpr size_t kMaxPresetName   = 64;
constexpr int kMaxCollisionTries  = 99;
constexpr const char* kNxpTag     = "nxp1";

std::string fmtParam(f32 v) {
    if (!std::isfinite(v)) v = 0.f;
    char buf[64];
    for (int p = 4; p <= 9; ++p) {
        std::snprintf(buf, sizeof buf, "%.*g", p, (f64)v);
        if ((f32)std::strtod(buf, nullptr) == v) break;
    }
    return buf;
}

// $XDG_CONFIG_HOME, else $HOME/.config, else the passwd entry's home /.config,
// else /tmp — the ladder src/control/learn.cpp's defaultMapPath() walks,
// because this tree has one answer to "where does nxtakt keep a user file".
std::string configDir() {
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return x;
    if (const char* h = std::getenv("HOME"); h && *h) return std::string(h) + "/.config";
    if (passwd* pw = ::getpwuid(::getuid())) return std::string(pw->pw_dir) + "/.config";
    return "/tmp";
}

// mkdir -p at 0755, learn.cpp's ensureParentDir with the last component kept.
bool ensureDir(const std::string& dir) {
    if (dir.empty()) return false;
    for (size_t i = 1; i <= dir.size(); ++i) {
        if (i != dir.size() && dir[i] != '/') continue;
        const std::string part = dir.substr(0, i);
        if (::mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    return true;
}

bool fileExists(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// The device's URI with every byte outside [A-Za-z0-9._-] replaced by `-`.
std::string uriSlug(const std::string& uri) {
    std::string s = uri;
    for (char& c : s) {
        const unsigned char u = (unsigned char)c;
        const bool ok = (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') ||
                        (u >= '0' && u <= '9') || u == '.' || u == '_' || u == '-';
        if (!ok) c = '-';
    }
    return s.empty() ? std::string("device") : s;
}

// The display name with every byte outside [A-Za-z0-9 ._-] replaced by `_`,
// leading and trailing spaces and dots trimmed, capped at 64 bytes, and
// `preset` if nothing survives. THE SLUG IS A FILENAME AND NOTHING ELSE — the
// display name is the file's `name` header, which is authoritative.
std::string nameSlug(const std::string& name) {
    std::string s;
    for (char c : name) {
        const unsigned char u = (unsigned char)c;
        const bool ok = (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') ||
                        (u >= '0' && u <= '9') || u == ' ' || u == '.' ||
                        u == '_' || u == '-';
        s += ok ? c : '_';
        if (s.size() >= kMaxPresetName) break;
    }
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '.')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '.')) --e;
    s = s.substr(b, e - b);
    return s.empty() ? std::string("preset") : s;
}

// One parsed `.nxp`. Refusal is ALL OR NOTHING, exactly like setStateString():
// a bad version tag, a `uri` mismatch, a missing or duplicated `name`, a
// malformed `param` or an oversized file means the caller gets false and
// applies nothing.
struct Nxp {
    std::string name;
    std::string category;
    std::string state;
    bool        hasState = false;
    std::vector<std::pair<u32, f32>> params;
};

bool parseNxp(const std::string& file, const std::string& wantUri, Nxp& out) {
    out = Nxp{};
    FILE* f = std::fopen(file.c_str(), "rb");
    if (!f) return false;
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return false; }
    const long end = std::ftell(f);
    if (end < 0 || (u64)end > kMaxPresetFileBytes) { std::fclose(f); return false; }
    std::string text((size_t)end, '\0');
    std::rewind(f);
    const size_t got = std::fread(text.data(), 1, text.size(), f);
    std::fclose(f);
    if (got != text.size()) return false;

    // Line-oriented UTF-8, LF, with a trailing \r stripped from every line so a
    // file that visited Windows still loads.
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i != text.size() && text[i] != '\n') continue;
        std::string ln = text.substr(start, i - start);
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        lines.push_back(std::move(ln));
        start = i + 1;
    }
    if (lines.empty() || lines[0] != kNxpTag) return false;

    int seenUri = 0, seenName = 0, seenCat = 0, seenState = 0;
    for (size_t i = 1; i < lines.size(); ++i) {
        const std::string& ln = lines[i];
        if (ln.empty()) continue;
        const size_t sp = ln.find(' ');
        if (sp == std::string::npos) return false;
        const std::string key = ln.substr(0, sp);
        const std::string val = ln.substr(sp + 1);      // the REST of the line
        if (key == "uri") {
            if (++seenUri > 1) return false;
            if (val != wantUri) return false;           // never half-load
        } else if (key == "name") {
            if (++seenName > 1) return false;
            if (val.empty() || val.size() > kMaxPresetName) return false;
            for (char c : val) {
                const unsigned char u = (unsigned char)c;
                if (u < 0x20 || u == 0x7F) return false;
            }
            out.name = val;
        } else if (key == "category") {
            if (++seenCat > 1) return false;
            out.category = val;
        } else if (key == "state") {
            if (++seenState > 1) return false;
            out.state = val;
            out.hasState = true;
        } else if (key == "param") {
            const size_t sp2 = val.find(' ');
            if (sp2 == std::string::npos) return false;
            char* endp = nullptr;
            const long id = std::strtol(val.c_str(), &endp, 10);
            if (endp != val.c_str() + sp2 || id < 0 || id > 0x7fffffffl) return false;
            const std::string num = val.substr(sp2 + 1);
            if (num.empty()) return false;
            endp = nullptr;
            const double v = std::strtod(num.c_str(), &endp);
            if (endp != num.c_str() + num.size() || !std::isfinite(v)) return false;
            out.params.emplace_back((u32)id, (f32)v);
        }
        // Unknown keys are SKIPPED: forward compatibility, the same rule the
        // state string's unknown records get.
    }
    return seenUri == 1 && seenName == 1;
}

} // namespace

void PluginInstance::invalidateUserPresets() {
    userScanned_ = false;
    userPresets_.clear();
}

void PluginInstance::scanUserPresetsIfNeeded() const {
    if (userScanned_) return;
    userScanned_ = true;
    userPresets_.clear();

    const std::string uri = desc().uri;
    const std::string dir = configDir() + "/nxtakt/presets/" + uriSlug(uri);
    DIR* d = ::opendir(dir.c_str());
    if (!d) return;                       // no directory is not an error: the
                                          // first save is what creates it.
    while (dirent* e = ::readdir(d)) {
        const std::string fn = e->d_name;
        // Files whose WHOLE TAIL is `.nxp` only, so `.nxp.bak` is never
        // enumerated.
        if (fn.size() < 5 || fn.compare(fn.size() - 4, 4, ".nxp") != 0) continue;
        const std::string full = dir + "/" + fn;
        if (!fileExists(full)) continue;
        Nxp p;
        if (!parseNxp(full, uri, p)) {
            // One log line, and the others still load: a corrupt file in a
            // directory must not cost the user the bank.
            LOGW("preset: %s did not parse; skipping it", full.c_str());
            continue;
        }
        UserPreset up;
        up.name     = p.name;
        up.category = p.category;
        up.file     = full;
        userPresets_.push_back(std::move(up));
    }
    ::closedir(d);

    // Byte-wise ascending by display name — memcmp, not locale collation, so
    // the bank looks the same on the user's laptop and in a test under de_DE.
    // std::string's comparison is char_traits<char>::compare, which is memcmp.
    std::sort(userPresets_.begin(), userPresets_.end(),
              [](const UserPreset& a, const UserPreset& b) {
                  if (a.name != b.name) return a.name < b.name;
                  return a.file < b.file;
              });
}

int PluginInstance::userPresetCount() const {
    scanUserPresetsIfNeeded();
    return (int)userPresets_.size();
}

const char* PluginInstance::userPresetName(int i) const {
    scanUserPresetsIfNeeded();
    if (i < 0 || i >= (int)userPresets_.size()) return nullptr;
    return userPresets_[(size_t)i].name.c_str();
}

bool PluginInstance::loadUserPreset(int i) {
    scanUserPresetsIfNeeded();
    if (i < 0 || i >= (int)userPresets_.size()) return false;
    Nxp p;
    if (!parseNxp(userPresets_[(size_t)i].file, desc().uri, p)) {
        LOGW("preset: %s did not parse; the device is unchanged",
             userPresets_[(size_t)i].file.c_str());
        return false;
    }

    // REFUSAL IS ALL-OR-NOTHING and `setStateString()` is the one step that can
    // still say no after the parse succeeded, so the device is snapshotted and
    // rolled back rather than left half-loaded. Nothing else here can fail.
    const int n = paramCount();
    std::vector<f32> before((size_t)(n > 0 ? n : 0), 0.f);
    for (int k = 0; k < n; ++k) before[(size_t)k] = getParam(k);
    const std::string stateBefore = stateString();

    // PARAMETERS FIRST, THEN STATE — host.h's own load ordering, and the trap
    // docs/RACKS.md documents: a state string is allowed to move parameters, so
    // a param write after it would overwrite what the state just restored.
    //
    // Every id is reset to its default first, because A PRESET IS COMPLETE
    // HOWEVER SHORT IT IS: an id the file omits reads as its default, not as
    // whatever the previous patch left there.
    for (int k = 0; k < n; ++k) setParam(k, paramInfo(k).def);
    for (const std::pair<u32, f32>& pv : p.params)
        for (int k = 0; k < n; ++k)
            if (paramInfo(k).id == pv.first) { setParam(k, pv.second); break; }

    // And the state block resets too — an ABSENT `state` key means the empty
    // state, which is what a fresh instance answers.
    if (!setStateString(p.hasState ? p.state : std::string())) {
        LOGW("preset: %s carries a state this device refuses; nothing applied",
             userPresets_[(size_t)i].file.c_str());
        for (int k = 0; k < n; ++k) setParam(k, before[(size_t)k]);
        setStateString(stateBefore);
        return false;
    }
    return true;
}

bool PluginInstance::saveUserPreset(const char* name) {
    if (!name || !*name) return false;
    const std::string display = name;
    if (display.size() > kMaxPresetName) return false;
    for (char c : display) {
        const unsigned char u = (unsigned char)c;
        if (u < 0x20 || u == 0x7F) return false;
    }

    const std::string uri = desc().uri;
    const std::string dir = configDir() + "/nxtakt/presets/" + uriSlug(uri);
    if (!ensureDir(dir)) {
        LOGE("preset: could not create %s", dir.c_str());
        return false;
    }

    // COLLISION. `<slug>.nxp`, then `-2`, `-3`, ... up to `-99`, then refuse —
    // unless an existing file carries the SAME display name, in which case that
    // file is the target and this is an overwrite.
    const std::string slug = nameSlug(display);
    std::string target;
    bool overwrite = false;
    for (int n = 1; n <= kMaxCollisionTries; ++n) {
        std::string cand = dir + "/" + slug;
        if (n > 1) cand += "-" + std::to_string(n);
        cand += ".nxp";
        if (!fileExists(cand)) { target = cand; break; }
        Nxp p;
        if (parseNxp(cand, uri, p) && p.name == display) {
            target = cand;
            overwrite = true;
            break;
        }
    }
    if (target.empty()) {
        LOGE("preset: %s already has 99 files whose names slug to '%s'",
             dir.c_str(), slug.c_str());
        return false;
    }

    // ONE GENERATION OF `.bak`, taken BEFORE the write. Presets are files, not
    // session state, so an overwrite does not enter undo; this is what "undo"
    // means here, and the editor announces it in the status bar.
    if (overwrite && std::rename(target.c_str(), (target + ".bak").c_str()) != 0)
        LOGW("preset: could not keep a .bak of %s", target.c_str());

    std::string text;
    text += kNxpTag;   text += '\n';
    text += "uri ";    text += uri;      text += '\n';
    text += "name ";   text += display;  text += '\n';
    text += "category User\n";
    // EVERY parameter id, not just the non-defaults. A hundred short lines is
    // not a cost worth optimising and a file that lists everything is a file a
    // user can read and edit.
    const int n = paramCount();
    for (int k = 0; k < n; ++k) {
        text += "param ";
        text += std::to_string(paramInfo(k).id);
        text += ' ';
        text += fmtParam(getParam(k));
        text += '\n';
    }
    const std::string st = stateString();
    if (!st.empty()) { text += "state "; text += st; text += '\n'; }

    // Temp-and-rename in the same directory: atomic, learn.cpp's discipline, so
    // a crash mid-save cannot leave a half-written preset where a whole one was.
    const std::string tmp = target + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) { LOGE("preset: could not open %s", tmp.c_str()); return false; }
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size() &&
                    std::fflush(f) == 0;
    std::fclose(f);
    if (!ok || std::rename(tmp.c_str(), target.c_str()) != 0) {
        std::remove(tmp.c_str());
        LOGE("preset: could not write %s", target.c_str());
        return false;
    }

    // Re-scan before returning, so presetCount() and presetName() already
    // include the new preset — and every pointer presetName() handed out before
    // now is stale, which is the one documented weakening of its lifetime.
    invalidateUserPresets();
    scanUserPresetsIfNeeded();
    LOGI("preset: wrote %s%s", target.c_str(), overwrite ? " (replaced)" : "");
    return true;
}

} // namespace lat
