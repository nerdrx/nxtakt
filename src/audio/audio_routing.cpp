#include "audio_routing.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <sstream>

#ifdef __linux__
#include <alsa/asoundlib.h>
#include <jack/jack.h>
#include <unistd.h>

namespace lat {
namespace {

struct JackControl {
    jack_client_t* client = nullptr;
    ~JackControl() { if (client) jack_client_close(client); }
};

static bool isNxTaktClient(const std::string& client) {
    return client == "NxTakt" || client.rfind("NxTakt-", 0) == 0;
}

static int clientPid(const char* client) {
    int (*getPid)(const char*) = jack_get_client_pid;
    return getPid ? getPid(client) : -1;
}

static bool isOwned(const char* name, int pid) {
    if (!name) return false;
    const char* colon = std::strchr(name, ':');
    if (!colon) return false;
    const std::string client(name, (size_t)(colon - name));
    if (!isNxTaktClient(client)) return false;
    if (client == "NxTakt-" + std::to_string(pid)) return true;
    return pid > 0 && clientPid(client.c_str()) == pid;
}

static bool isControl(const char* name, jack_client_t* control) {
    return control && name && std::strcmp(name, jack_get_client_name(control)) == 0;
}

static std::string displayName(jack_port_t* port) {
    const int size = jack_port_name_size();
    std::vector<char> alias0((size_t)size), alias1((size_t)size);
    char* aliases[2] = {alias0.data(), alias1.data()};
    const int count = jack_port_get_aliases(port, aliases);
    if (count > 0 && aliases[0] && aliases[0][0]) {
        std::string out = aliases[0];
        if (aliases[1] && aliases[1][0]) out += " (" + std::string(aliases[1]) + ")";
        return out;
    }
    return jack_port_name(port);
}

static std::vector<AudioPortChoice> ports(jack_client_t* c, int pid, unsigned flags) {
    std::vector<AudioPortChoice> result;
    const char** names = jack_get_ports(c, nullptr, JACK_DEFAULT_AUDIO_TYPE, flags);
    if (!names) return result;
    for (const char** p = names; *p; ++p) {
        const char* colon = std::strchr(*p, ':');
        const std::string client = colon ? std::string(*p, (size_t)(colon - *p)) : std::string();
        if (isOwned(*p, pid) || isNxTaktClient(client) || isControl(*p, c)) continue;
        if (auto* port = jack_port_by_name(c, *p)) result.push_back({*p, displayName(port)});
    }
    jack_free(names);
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.label < b.label; });
    return result;
}

static jack_client_t* openControl(int, std::string& error) {
    jack_status_t status{};
    const std::string name = "NxTakt-audio-control-" + std::to_string((long long)getpid());
    auto* c = jack_client_open(name.c_str(), (jack_options_t)(JackUseExactName | JackNoStartServer), &status);
    if (!c) error = "JACK unavailable";
    return c;
}

static std::array<std::string, 4> ownPorts(jack_client_t* c, int pid, std::string& error) {
    std::array<std::string, 4> result{};
    std::string clientName = "NxTakt-" + std::to_string(pid);
    if (pid > 0) {
        const char** candidates = jack_get_ports(c, "NxTakt:*", JACK_DEFAULT_AUDIO_TYPE, 0);
        if (candidates) {
            for (const char** candidate = candidates; *candidate; ++candidate) {
                const char* colon = std::strchr(*candidate, ':');
                if (!colon) continue;
                const std::string candidateClient(*candidate, (size_t)(colon - *candidate));
                if (candidateClient == "NxTakt" && clientPid(candidateClient.c_str()) == pid) {
                    clientName = candidateClient;
                    break;
                }
            }
            jack_free(candidates);
        }
    }
    const std::string prefix = clientName + ":";
    const char* names[] = {"out_L", "out_R", "in_L", "in_R"};
    for (size_t i = 0; i < 4; ++i) {
        const std::string full = prefix + names[i];
        if (!jack_port_by_name(c, full.c_str())) {
            error = "Audio ports unavailable. Close and reopen Takt after updating, then Refresh.";
            return {};
        }
        result[i] = full;
    }
    return result;
}

static void freeConnections(const char** links) { if (links) jack_free(links); }

static std::vector<std::string> connections(jack_client_t* c, const std::string& own) {
    std::vector<std::string> result;
    auto* p = jack_port_by_name(c, own.c_str());
    if (!p) return result;
    const char** links = jack_port_get_all_connections(c, p);
    if (links) {
        for (const char** link = links; *link; ++link) result.emplace_back(*link);
    }
    freeConnections(links);
    return result;
}

static bool validTarget(jack_client_t* c, const std::string& name, unsigned direction, int pid) {
    if (name.empty() || name == "-") return true;
    auto* p = jack_port_by_name(c, name.c_str());
    if (!p || isOwned(name.c_str(), pid) || isControl(name.c_str(), c)) return false;
    const unsigned flags = jack_port_flags(p);
    return std::strcmp(jack_port_type(p), JACK_DEFAULT_AUDIO_TYPE) == 0 &&
           (((flags & JackPortIsInput) && direction == JackPortIsInput) ||
            ((flags & JackPortIsOutput) && direction == JackPortIsOutput));
}

static std::string physicalChannel(jack_client_t* c, unsigned flags, size_t channel, bool monoFallback) {
    const char** names = jack_get_ports(c, nullptr, JACK_DEFAULT_AUDIO_TYPE, flags | JackPortIsPhysical);
    std::vector<std::string> choices;
    if(names)for(const char** p=names;*p;++p)choices.emplace_back(*p);
    std::string result=channel<choices.size()?choices[channel]:monoFallback && !choices.empty()?choices.front():"";
    if (names) jack_free(names);
    return result.empty() ? "-" : result;
}

} // namespace

AudioRouteSnapshot inspectAudioRoutes(int enginePid) {
    AudioRouteSnapshot out;
    if (enginePid <= 0) { out.error = "Invalid NxTakt engine PID"; return out; }
    std::string error;
    JackControl control{openControl(enginePid, error)};
    if (!control.client) { out.error = error; return out; }
    const auto own = ownPorts(control.client, enginePid, out.error);
    if (own[0].empty()) return out;
    out.outputs = ports(control.client, enginePid, JackPortIsInput);
    out.inputs = ports(control.client, enginePid, JackPortIsOutput);
    for (size_t i = 0; i < own.size(); ++i) {
        const auto links = connections(control.client, own[i]);
        if (links.size() > 1) {
            out.error = "Multiple JACK connections on " + own[i];
            out.current[i] = links.front();
        } else if (!links.empty()) out.current[i] = links.front();
        else out.current[i] = "-";
    }
    out.connected = true;
    return out;
}

bool applyAudioRoutes(int enginePid, const std::array<std::string, 4>& requested,
                      std::string& error) {
    error.clear();
    if (enginePid <= 0) { error = "Invalid NxTakt engine PID"; return false; }
    JackControl control{openControl(enginePid, error)};
    if (!control.client) return false;
    const auto own = ownPorts(control.client, enginePid, error);
    if (own[0].empty()) return false;
    auto resolved = requested;
    for (size_t i = 0; i < 4; ++i) {
        if (resolved[i].empty()) {
            resolved[i] = i < 2 ? physicalChannel(control.client, JackPortIsInput, i, false)
                                : physicalChannel(control.client, JackPortIsOutput, i-2, true);
        }
        if (!resolved[i].empty() && resolved[i] != "-" &&
            !validTarget(control.client, resolved[i], i < 2 ? JackPortIsInput : JackPortIsOutput, enginePid)) {
            error = "JACK port unavailable or wrong direction: " + resolved[i];
            return false;
        }
    }
    std::array<std::vector<std::string>, 4> old;
    for (size_t i = 0; i < 4; ++i) old[i] = connections(control.client, own[i]);
    bool rollbackOk = true;
    auto rollback = [&] {
        for (size_t i = 0; i < 4; ++i) {
            for (const auto& link : connections(control.client, own[i])) {
                if (jack_disconnect(control.client, i < 2 ? own[i].c_str() : link.c_str(), i < 2 ? link.c_str() : own[i].c_str())) rollbackOk = false;
            }
            for (const auto& link : old[i]) {
                if (jack_connect(control.client, i < 2 ? own[i].c_str() : link.c_str(), i < 2 ? link.c_str() : own[i].c_str())) rollbackOk = false;
            }
        }
        if (!rollbackOk) error += "; JACK rollback failed";
    };
    for (size_t i = 0; i < 4; ++i) {
        for (const auto& link : old[i]) {
            if (jack_disconnect(control.client, i < 2 ? own[i].c_str() : link.c_str(), i < 2 ? link.c_str() : own[i].c_str())) {
                error = "Failed to remove existing JACK connection"; rollback(); return false;
            }
        }
    }
    for (size_t i = 0; i < 4; ++i) if (resolved[i] != "-") {
        const char* source = i < 2 ? own[i].c_str() : resolved[i].c_str();
        const char* dest = i < 2 ? resolved[i].c_str() : own[i].c_str();
        if (jack_connect(control.client, source, dest)) {
            error = "Failed to connect JACK port " + resolved[i]; rollback(); return false;
        }
    }
    for (size_t i = 0; i < 4; ++i) {
        const auto now = connections(control.client, own[i]);
        if ((resolved[i] == "-" && !now.empty()) || (resolved[i] != "-" && (now.size() != 1 || now[0] != resolved[i]))) {
            error = "JACK route verification failed"; rollback(); return false;
        }
    }
    return true;
}

std::vector<AudioPortChoice> listAlsaDevices(bool input) {
    std::vector<AudioPortChoice> result{{"default", "default"}};
    std::set<std::string> seen{"default"};
    void** hints = nullptr;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0) return result;
    for (void** it = hints; *it; ++it) {
        char* name = snd_device_name_get_hint(*it, "NAME");
        char* ioid = snd_device_name_get_hint(*it, "IOID");
        char* desc = snd_device_name_get_hint(*it, "DESC");
        const bool matches = !ioid || !*ioid || (input ? std::strcmp(ioid, "Input") == 0 : std::strcmp(ioid, "Output") == 0);
        if (name && matches && seen.insert(name).second) {
            std::string label = desc && *desc ? desc : name;
            for (char& c : label) if (c == '\n' || c == '\r') c = ' ';
            result.push_back({name, label});
        }
        if (name) free(name);
        if (ioid) free(ioid);
        if (desc) free(desc);
    }
    snd_device_name_free_hint(hints);
    return result;
}

} // namespace lat
#else
namespace lat {
AudioRouteSnapshot inspectAudioRoutes(int) { AudioRouteSnapshot s; s.error = "JACK routing unsupported on this platform"; return s; }
bool applyAudioRoutes(int, const std::array<std::string, 4>&, std::string& error) { error = "JACK routing unsupported on this platform"; return false; }
std::vector<AudioPortChoice> listAlsaDevices(bool) { return {}; }
} // namespace lat
#endif
