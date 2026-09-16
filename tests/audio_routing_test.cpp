#include "audio/audio_routing.h"

#ifdef __linux__

#include <jack/jack.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

namespace {

struct Client {
    jack_client_t* value = nullptr;
    ~Client() { if (value) jack_client_close(value); }
};

int process(jack_nframes_t frames, void* opaque) {
    const auto& outputs=*static_cast<const std::vector<jack_port_t*>*>(opaque);
    for(auto* port:outputs)std::memset(jack_port_get_buffer(port,frames),0,frames*sizeof(float));
    return 0;
}

bool check(bool condition, const std::string& message) {
    if (condition) return true;
    std::cerr << "FAIL: " << message << '\n';
    return false;
}

std::vector<std::string> links(jack_client_t* client, const std::string& name) {
    std::vector<std::string> result;
    auto* port = jack_port_by_name(client, name.c_str());
    if (!port) return result;
    const char** names = jack_port_get_all_connections(client, port);
    if (!names) return result;
    for (const char** name = names; *name; ++name) result.emplace_back(*name);
    jack_free(names);
    return result;
}

bool registerPort(jack_client_t* client, const std::string& name, unsigned flags) {
    return jack_port_register(client, name.c_str(), JACK_DEFAULT_AUDIO_TYPE, flags, 0) != nullptr;
}

} // namespace

int main(int argc, char** argv) {
    if(argc==4 && std::string(argv[1])=="--verify") {
        const auto state=lat::inspectAudioRoutes(std::stoi(argv[2]));
        std::ifstream in(argv[3]);std::array<std::string,4> expected;
        for(auto& port:expected)std::getline(in,port);
        if(!check(state.connected && state.current==expected,"saved routes match actual graph: "+state.error))return 1;
        std::cout<<"PASS: saved routes match actual JACK graph"<<std::endl;
        return 0;
    }
    const std::string suffix = std::to_string(static_cast<long long>(getpid()));
    const std::string engineName = "NxTakt-" + suffix;
    const std::string fixtureName = "NxTaktAudioFixture-" + suffix;
    const std::string sentinelName = "NxTaktAudioSentinel-" + suffix;
    jack_status_t status{};

    Client engine{jack_client_open(engineName.c_str(),
                                   static_cast<jack_options_t>(JackUseExactName | JackNoStartServer),
                                   &status)};
    if (!engine.value) {
        if (status & (JackServerFailed | JackServerError)) {
            std::cout << "SKIP: JACK server unavailable\n";
            return 77;
        }
        std::cerr << "FAIL: cannot open engine JACK client, status=" << status << '\n';
        return 1;
    }
    Client fixture{jack_client_open(fixtureName.c_str(),
                                    static_cast<jack_options_t>(JackUseExactName | JackNoStartServer),
                                    &status)};
    Client sentinel{jack_client_open(sentinelName.c_str(),
                                     static_cast<jack_options_t>(JackUseExactName | JackNoStartServer),
                                     &status)};
    if (!check(fixture.value && sentinel.value, "cannot open fixture JACK clients")) return 1;

    const std::array<std::string, 4> own = {
        engineName + ":out_L", engineName + ":out_R", engineName + ":in_L", engineName + ":in_R"};
    const std::array<std::string, 4> target = {
        fixtureName + ":sinkL", fixtureName + ":sinkR", fixtureName + ":sourceL", fixtureName + ":sourceR"};
    const std::string sentinelOut = sentinelName + ":sentinelOut";
    const std::string sentinelIn = sentinelName + ":sentinelIn";

    if (!check(registerPort(engine.value, "out_L", JackPortIsOutput) &&
               registerPort(engine.value, "out_R", JackPortIsOutput) &&
               registerPort(engine.value, "in_L", JackPortIsInput) &&
               registerPort(engine.value, "in_R", JackPortIsInput), "register engine ports")) return 1;
    if (!check(registerPort(fixture.value, "sinkL", JackPortIsInput) &&
               registerPort(fixture.value, "sinkR", JackPortIsInput) &&
               registerPort(fixture.value, "sourceL", JackPortIsOutput) &&
               registerPort(fixture.value, "sourceR", JackPortIsOutput), "register fixture ports")) return 1;
    if (!check(registerPort(sentinel.value, "sentinelOut", JackPortIsOutput) &&
               registerPort(sentinel.value, "sentinelIn", JackPortIsInput), "register sentinel ports")) return 1;
    std::vector<jack_port_t*> engineOutputs{jack_port_by_name(engine.value,own[0].c_str()),jack_port_by_name(engine.value,own[1].c_str())};
    std::vector<jack_port_t*> fixtureOutputs{jack_port_by_name(fixture.value,target[2].c_str()),jack_port_by_name(fixture.value,target[3].c_str())};
    std::vector<jack_port_t*> sentinelOutputs{jack_port_by_name(sentinel.value,sentinelOut.c_str())};
    if (!check(!jack_set_process_callback(engine.value, process, &engineOutputs) &&
               !jack_set_process_callback(fixture.value, process, &fixtureOutputs) &&
               !jack_set_process_callback(sentinel.value, process, &sentinelOutputs), "set JACK callbacks")) return 1;
    struct StopCallbacks {
        jack_client_t* a;jack_client_t* b;jack_client_t* c;
        ~StopCallbacks(){jack_deactivate(a);jack_deactivate(b);jack_deactivate(c);}
    } stop{engine.value,fixture.value,sentinel.value};
    if (!check(!jack_activate(engine.value) && !jack_activate(fixture.value) &&
               !jack_activate(sentinel.value), "activate JACK clients")) return 1;
    if (!check(!jack_connect(sentinel.value, sentinelOut.c_str(), sentinelIn.c_str()),
               "create sentinel route")) return 1;

    if(argc==3 && std::string(argv[1])=="--fixture") {
        std::ofstream out(argv[2]);
        for(const auto& port:target)out<<port<<'\n';
        out.close();
        std::cout<<"Virtual audio fixture ready; press Enter to stop."<<std::endl;
        std::cin.get();
        return 0;
    }

    const auto inspected = lat::inspectAudioRoutes(static_cast<int>(getpid()));
    if (!check(inspected.connected, "inspectAudioRoutes reports connected")) return 1;

    const std::array<std::string, 4> requested = target;
    std::string error;
    const bool applied = lat::applyAudioRoutes(static_cast<int>(getpid()), requested, error);
    if (!check(applied, "apply four routes: " + error)) return 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for (std::size_t i = 0; i < own.size(); ++i) {
        for(const auto& link:links(engine.value,own[i]))std::cout<<"LINK "<<own[i]<<" -> "<<link<<"\n";
        if (!check(links(engine.value, own[i]) == std::vector<std::string>{target[i]},
                   "route mismatch for " + own[i])) return 1;
    }
    if (!check(links(sentinel.value, sentinelOut) == std::vector<std::string>{sentinelIn},
               "sentinel route changed")) return 1;

    const auto beforeBad = std::array<std::vector<std::string>, 4>{
        links(engine.value, own[0]), links(engine.value, own[1]),
        links(engine.value, own[2]), links(engine.value, own[3])};
    auto wrongDirection = requested;
    wrongDirection[0] = target[2];
    const bool wrongDirectionApplied = lat::applyAudioRoutes(
        static_cast<int>(getpid()), wrongDirection, error);
    if (!check(!wrongDirectionApplied && !error.empty(),
               "wrong-direction route rejected: " + error)) return 1;
    for (std::size_t i = 0; i < own.size(); ++i)
        if (!check(links(engine.value, own[i]) == beforeBad[i], "wrong-direction changed routes")) return 1;
    auto missingTarget = requested;
    missingTarget[3] = fixtureName + ":missing";
    const bool missingTargetApplied = lat::applyAudioRoutes(
        static_cast<int>(getpid()), missingTarget, error);
    if (!check(!missingTargetApplied && !error.empty(),
               "missing target route rejected: " + error)) return 1;
    for (std::size_t i = 0; i < own.size(); ++i)
        if (!check(links(engine.value, own[i]) == beforeBad[i], "missing target changed routes")) return 1;

    const bool disconnected = lat::applyAudioRoutes(
        static_cast<int>(getpid()), {"-", "-", "-", "-"}, error);
    if (!check(disconnected, "disconnect all routes: " + error)) return 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for (const auto& name : own)
        if (!check(links(engine.value, name).empty(), "disconnected route remains")) return 1;
    if (!check(links(sentinel.value, sentinelOut) == std::vector<std::string>{sentinelIn},
               "sentinel route not preserved after disconnect")) return 1;
    std::cout << "PASS: JACK audio routing integration\n";
    return 0;
}

#else

#include <iostream>
#include <fstream>
int main() {
    std::cout << "SKIP: JACK routing test requires Linux JACK\n";
    return 77;
}

#endif
