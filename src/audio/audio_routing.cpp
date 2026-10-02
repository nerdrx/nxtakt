#include "audio_routing.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstring>
#include <map>
#include <poll.h>
#include <set>
#include <signal.h>
#include <sstream>

#ifdef __linux__
#include <alsa/asoundlib.h>
#include <jack/jack.h>
#include <sys/types.h>
#include <sys/wait.h>
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

static bool runPactl(const std::vector<std::string>& args, std::string& output) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return false;
    const pid_t child = fork();
    if (child < 0) { close(pipefd[0]); close(pipefd[1]); return false; }
    if (child == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        std::vector<char*> argv;
        argv.emplace_back(const_cast<char*>("pactl"));
        for (const auto& arg : args) argv.emplace_back(const_cast<char*>(arg.c_str()));
        argv.emplace_back(nullptr);
        execvp("pactl", argv.data());
        _exit(127);
    }
    close(pipefd[1]);
    output.clear();
    char buffer[512];
    bool eof = false;
    int status = 0;
    bool exited = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (!eof || !exited) {
        pollfd ready{pipefd[0], POLLIN | POLLHUP, 0};
        const int polled = poll(&ready, 1, 100);
        if (polled > 0 && (ready.revents & (POLLIN | POLLHUP))) {
            const ssize_t count = read(pipefd[0], buffer, sizeof buffer);
            if (count > 0) output.append(buffer, (size_t)count);
            else if (!count) eof = true;
        }
        if (!exited) {
            const pid_t waited = waitpid(child, &status, WNOHANG);
            exited = waited == child;
            if (waited < 0 && errno != EINTR) { eof = true; break; }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(child, SIGTERM);
            const auto stopDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            while (std::chrono::steady_clock::now() < stopDeadline && waitpid(child, &status, WNOHANG) == 0)
                poll(nullptr, 0, 10);
            if (waitpid(child, &status, WNOHANG) == 0) { kill(child, SIGKILL); waitpid(child, &status, 0); }
            close(pipefd[0]);
            return false;
        }
    }
    close(pipefd[0]);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool ensureDiscordSink(bool enabled, std::string& error) {
    std::string modules;
    if (!runPactl({"list", "short", "modules"}, modules)) {
        if (!enabled) return true;
        error = "PipeWire/PulseAudio control unavailable (pactl not found or server unavailable)";
        return false;
    }
    bool exists = false;
    std::istringstream rows(modules);
    std::string row;
    while (std::getline(rows, row))
        if (row.find("module-null-sink") != std::string::npos &&
            row.find("sink_name=nxtakt_discord_capture") != std::string::npos) exists = true;
    if (!enabled) return true;
    if (!exists) {
        std::string id;
        if (!runPactl({"load-module", "module-null-sink", "sink_name=nxtakt_discord_capture",
                       "sink_properties=device.description=NxTakt_Discord_Capture"}, id)) {
            error = "Could not create the isolated Discord capture sink";
            return false;
        }
    }
    return true;
}

static bool removeDiscordSink(std::string& error) {
    std::string modules;
    if (!runPactl({"list", "short", "modules"}, modules)) return true;
    std::istringstream lines(modules);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.find("module-null-sink") == std::string::npos ||
            line.find("sink_name=nxtakt_discord_capture") == std::string::npos ||
            line.find("device.description=NxTakt_Discord_Capture") == std::string::npos) continue;
        std::istringstream row(line);
        std::string id;
        row >> id;
        if (!id.empty() && !runPactl({"unload-module", id}, line)) {
            error = "Could not remove the NxTakt Discord capture sink";
            return false;
        }
        break;
    }
    return true;
}

static int channelSide(const std::string& name) {
    std::string text = name;
    for (char& ch : text) ch = (char)std::tolower((unsigned char)ch);
    if (text.find("front-left") != std::string::npos || text.find("front left") != std::string::npos ||
        text.ends_with("_fl") || text.ends_with(".fl") || text.ends_with("-fl") ||
        text.ends_with("_left") || text.ends_with(".left") || text.ends_with("-left") ||
        text.ends_with("_out_l") || text.ends_with("_in_l") || text.ends_with("_l")) return 0;
    if (text.find("front-right") != std::string::npos || text.find("front right") != std::string::npos ||
        text.ends_with("_fr") || text.ends_with(".fr") || text.ends_with("-fr") ||
        text.ends_with("_right") || text.ends_with(".right") || text.ends_with("-right") ||
        text.ends_with("_out_r") || text.ends_with("_in_r") || text.ends_with("_r")) return 1;
    if (text.ends_with("_1") || text.ends_with(".1") || text.ends_with("-1")) return 0;
    if (text.ends_with("_2") || text.ends_with(".2") || text.ends_with("-2")) return 1;
    return -1;
}

static bool isDiscordBusPort(const std::string& name) {
    std::string normalized;
    for (const unsigned char ch : name)
        if (std::isalnum(ch)) normalized.push_back((char)std::tolower(ch));
    return normalized.find("nxtaktdiscordcapture") != std::string::npos;
}

static std::vector<std::string> busPorts(jack_client_t* c) {
    std::array<std::string, 2> stereo{};
    const char** names = jack_get_ports(c, nullptr, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput);
    if (names) {
        for (const char** p = names; *p; ++p) {
            if (!isDiscordBusPort(*p)) continue;
            const int side = channelSide(*p);
            if (side >= 0 && jack_port_by_name(c, *p)) stereo[(size_t)side] = *p;
        }
        jack_free(names);
    }
    if (stereo[0].empty() || stereo[1].empty()) return {};
    return {stereo[0], stereo[1]};
}

static std::vector<AudioDeviceChoice> jackDevices(jack_client_t* c, bool input) {
    std::map<std::string, std::vector<std::string>> groups;
    const unsigned direction = input ? JackPortIsOutput : JackPortIsInput;
    const char** names = jack_get_ports(c, nullptr, JACK_DEFAULT_AUDIO_TYPE, direction | JackPortIsPhysical);
    if (!names) return {};
    for (const char** p = names; *p; ++p) {
        const char* colon = std::strchr(*p, ':');
        if (!colon) continue;
        const std::string client(*p, (size_t)(colon - *p));
        if (isNxTaktClient(client) || client.rfind("NxTakt-audio-control-", 0) == 0) continue;
        if (jack_port_by_name(c, *p)) groups[client].emplace_back(*p);
    }
    jack_free(names);
    std::vector<AudioDeviceChoice> result;
    for (auto& [client, ports] : groups) {
        std::sort(ports.begin(), ports.end());
        AudioDeviceChoice item;
        item.label = client;
        for (const auto& name : ports) {
            const int side = channelSide(name);
            if (side < 0) continue;
            if (side == 0 && item.left.empty()) item.left = name;
            if (side == 1 && item.right.empty()) item.right = name;
        }
        if (item.left.empty() || item.right.empty()) continue;
        if (auto* first = jack_port_by_name(c, item.left.c_str())) item.label = displayName(first);
        if (auto* second = jack_port_by_name(c, item.right.c_str())) {
            const std::string right = displayName(second);
            if (right != item.label) item.label += " / " + right;
        }
        result.push_back(std::move(item));
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.label < b.label; });
    return result;
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
        auto links = connections(control.client, own[i]);
        if (i < 2) {
            for (auto it = links.begin(); it != links.end();) {
                if (isDiscordBusPort(*it)) {
                    out.discordBusConnected = true;
                    it = links.erase(it);
                } else ++it;
            }
        }
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
                      std::string& error, bool discordCaptureBus) {
    error.clear();
    if (enginePid <= 0) { error = "Invalid NxTakt engine PID"; return false; }
    if (!ensureDiscordSink(discordCaptureBus, error)) return false;
    JackControl control{openControl(enginePid, error)};
    if (!control.client) return false;
    const auto own = ownPorts(control.client, enginePid, error);
    if (own[0].empty()) return false;
    auto resolved = requested;
    const auto extraBus = discordCaptureBus ? busPorts(control.client) : std::vector<std::string>{};
    if (discordCaptureBus && extraBus.size() < 2) {
        error = "Discord capture sink is ready but its stereo ports are not visible yet; Refresh and apply again";
        return false;
    }
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
    if (discordCaptureBus) {
        for (size_t i = 0; i < 2; ++i) {
            if (jack_connect(control.client, own[i].c_str(), extraBus[i].c_str())) {
                error = "Failed to connect NxTakt output to Discord capture bus";
                rollback(); return false;
            }
        }
    }
    for (size_t i = 0; i < 4; ++i) {
        const auto now = connections(control.client, own[i]);
        const bool primary = resolved[i] == "-" ? now.empty() : std::find(now.begin(), now.end(), resolved[i]) != now.end();
        const bool bus = i < 2 && discordCaptureBus && std::find(now.begin(), now.end(), extraBus[i]) != now.end();
        const size_t expectedCount = (resolved[i] == "-" ? 0u : 1u) + (i < 2 && discordCaptureBus ? 1u : 0u);
        if (!primary || now.size() != expectedCount || (i < 2 && discordCaptureBus && !bus)) {
            error = "JACK route verification failed"; rollback(); return false;
        }
    }
    if (!discordCaptureBus && !removeDiscordSink(error)) return false;
    return true;
}

std::vector<AudioDeviceChoice> listJackDevices(bool input) {
    std::string error;
    JackControl control{openControl(0, error)};
    if (!control.client) return {};
    return jackDevices(control.client, input);
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
std::vector<AudioDeviceChoice> listJackDevices(bool) { return {}; }
std::vector<AudioPortChoice> listAlsaDevices(bool) { return {}; }
} // namespace lat
#endif
