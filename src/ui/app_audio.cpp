#include "app.h"
#include "../audio/audio_settings.h"
#include "../audio/audio_routing.h"
#include <algorithm>
#include <cstdio>

namespace lat {
void App::drawAudioSettings(const Rect& bounds) {
    static AudioSettings settings;
    static AudioRouteSnapshot routes;
    static std::vector<AudioPortChoice> alsaInputs, alsaOutputs;
    static std::vector<AudioDeviceChoice> jackOutputs, jackInputs;
    static int tab = 0, picker = -1;
    static bool advanced = false;
    static f32 pickerScroll = 0, bodyScroll = 0;
    static std::string message;

    const f32 s = win_.dpiScale();
    const f32 pad = 10 * s;
    const Rect content{bounds.x + pad, bounds.y + pad,
                       std::max(0.f, bounds.w - 2 * pad),
                       std::max(0.f, bounds.h - 2 * pad)};
    auto id = [](int n) { return uiId(UiAudioSettings, n); };
    auto refreshLists = [&] {
        routes = inspectAudioRoutes(eng_.enginePid());
        jackOutputs = listJackDevices(false);
        jackInputs = listJackDevices(true);
        alsaInputs = listAlsaDevices(true);
        alsaOutputs = listAlsaDevices(false);
    };
    if (refreshAudioSettings_) {
        settings = loadAudioSettings();
        message.clear();
        refreshLists();
        message = routes.error;
        tab = eng_.driverName() && std::string(eng_.driverName()).find("ALSA") != std::string::npos ? 1 : 0;
        picker = -1;
        pickerScroll = bodyScroll = 0;
        refreshAudioSettings_ = false;
    }

#ifdef _WIN32
    rend_.textIn(fSmall_, content, "Windows uses the system audio device. Change it in Windows sound settings.",
                 nx::muted, Align::Left, 0);
    return;
#endif

    if (picker >= 0) {
        std::vector<AudioPortChoice> choices;
        std::vector<AudioDeviceChoice> devices;
        std::string current;
        const bool paired = tab == 0 && picker < 2;
        if (paired) {
            devices = picker == 0 ? jackOutputs : jackInputs;
            const int base = picker == 0 ? 0 : 2;
            current = settings.jackPorts[base] + "\n" + settings.jackPorts[base + 1];
            choices = {{"", "Automatic device"}, {"-", "Disconnected"}};
        } else if (picker < 2) {
            choices = picker == 0 ? alsaOutputs : alsaInputs;
            current = picker == 0 ? settings.alsaOutput : settings.alsaInput;
        } else if (picker < 6) {
            const int port = picker - 2;
            choices = port < 2 ? routes.outputs : routes.inputs;
            choices.insert(choices.begin(), {"-", "Disconnected"});
            current = settings.jackPorts[port];
        } else {
            choices = picker == 6 ? alsaOutputs : alsaInputs;
            current = picker == 6 ? settings.alsaOutput : settings.alsaInput;
        }
        const char* title = picker == 0 ? "Output device" : picker == 1 ? "Input device" :
            picker < 4 ? "Advanced output port" : picker < 6 ? "Advanced input port" :
            picker == 6 ? "ALSA output device" : "ALSA input device";
        rend_.textIn(fSmall_, {content.x, content.y, content.w - 60 * s, 24 * s}, title, nx::text, Align::Left, 0);
        Rect back{content.right() - 54 * s, content.y, 54 * s, 24 * s};
        if (ui_.button(id(99), back, "Back")) { picker = -1; pickerScroll = 0; bodyScroll = 0; }
        Rect list{content.x, content.y + 28 * s, content.w, std::max(0.f, content.h - 28 * s)};
        const f32 rowH = 38 * s;
        if (list.contains(win_.input().mx, win_.input().my)) pickerScroll -= win_.input().wheel * rowH;
        const std::size_t count = paired ? devices.size() + 2 : choices.size();
        pickerScroll = clampv(pickerScroll, 0.f, std::max(0.f, rowH * count - list.h));
        rend_.pushClip(list);
        for (std::size_t row = 0; row < count; ++row) {
            Rect item{list.x, list.y + row * rowH - pickerScroll, list.w, 34 * s};
            if (item.bottom() < list.y || item.y > list.bottom()) continue;
            std::string label, value;
            if (paired && row >= 2) {
                const auto& device = devices[row - 2];
                label = device.label;
                value = device.left + "\n" + device.right;
            } else {
                const auto& choice = choices[row];
                label = choice.label;
                value = choice.value;
            }
            if (ui_.button(id(100 + (int)row), item, label.c_str(), value == current)) {
                if (paired) {
                    const int base = picker == 0 ? 0 : 2;
                    if (row < 2) settings.jackPorts[base] = settings.jackPorts[base + 1] = value;
                    else {
                        settings.jackPorts[base] = devices[row - 2].left;
                        settings.jackPorts[base + 1] = devices[row - 2].right;
                    }
                } else if (picker >= 2 && picker < 6) settings.jackPorts[picker - 2] = value;
                else if (picker == 6) settings.alsaOutput = value;
                else settings.alsaInput = value;
                picker = -1;
                pickerScroll = 0;
                ui_.editId = 0;
                message = "Selection changed. Apply and save, or restart the engine.";
                break;
            }
            if (ui_.hovered(item) && !paired && !value.empty()) ui_.tip = value;
        }
        if (!count) rend_.textIn(fSmall_, list, "No paired devices found. Use advanced port routing.", nx::muted, Align::Left, 0);
        rend_.popClip();
        return;
    }

    const f32 footerTop = content.bottom() - 58 * s;
    const f32 bodyTop = content.y;
    const Rect bodyClip{content.x, bodyTop, content.w, std::max(0.f, footerTop - bodyTop - 5 * s)};
    const std::string driver = eng_.driverName() ? eng_.driverName() : "Unavailable";
    std::string backend = driver;
    if (driver.find("JACK") != std::string::npos) backend = "JACK / PipeWire";
    else if (driver.find("ALSA") != std::string::npos) backend = "ALSA";
    const f32 bodyHeight = tab == 0
        ? 28 * s + 7 * s + 37 * s + 6 * s + 37 * s + 8 * s + 38 * s + 14 * s + 1 * s +
          9 * s + 27 * s + 5 * s + 34 * s + 3 * s + 17 * s + 7 * s + 27 * s +
          (advanced ? 4 * 31 * s + 3 * 33 * s : 0)
        : 28 * s + 7 * s + 37 * s + 6 * s + 37 * s + 8 * s + 34 * s + 9 * s + 26 * s + 6 * s + 27 * s;
    if (bodyClip.contains(win_.input().mx, win_.input().my)) bodyScroll -= win_.input().wheel * 34 * s;
    bodyScroll = clampv(bodyScroll, 0.f, std::max(0.f, bodyHeight - bodyClip.h));
    rend_.pushClip(bodyClip);
    f32 y = bodyTop - bodyScroll;

    rend_.textIn(fSmall_, {content.x, y + 5 * s, 66 * s, 20 * s}, "Engine", nx::muted, Align::Left, 0);
    rend_.textIn(fBody_, {content.x + 68 * s, y + 3 * s, content.w - 174 * s, 24 * s}, backend.c_str(), nx::text, Align::Left, 0);
    Rect fallback{content.right() - 100 * s, y, 100 * s, 26 * s};
    if (ui_.button(id(2), fallback, tab == 0 ? "ALSA fallback" : "JACK / PipeWire", tab != 0)) {
        tab = tab == 0 ? 1 : 0;
        bodyScroll = 0;
        message.clear();
    }
    y += 35 * s;

    const auto displayDevice = [&](bool input) {
        if (tab != 0) {
            const std::string& value = input ? settings.alsaInput : settings.alsaOutput;
            const auto& choices = input ? alsaInputs : alsaOutputs;
            for (const auto& choice : choices) if (choice.value == value) return choice.label;
            return value;
        }
        const int base = input ? 2 : 0;
        const std::string& left = settings.jackPorts[base];
        const std::string& right = settings.jackPorts[base + 1];
        if (left.empty() && right.empty()) return std::string("Automatic device");
        if (left == "-" && right == "-") return std::string("Disconnected");
        const auto& devices = input ? jackInputs : jackOutputs;
        for (const auto& device : devices)
            if (device.left == left && device.right == right) return device.label;
        return std::string("Custom or unavailable ports");
    };
    const char* deviceNames[] = {"Output device", "Input device"};
    for (int i = 0; i < 2; ++i) {
        rend_.textIn(fSmall_, {content.x, y + 8 * s, 92 * s, 18 * s}, deviceNames[i], nx::text, Align::Left, 0);
        Rect pick{content.x + 94 * s, y, content.w - 94 * s, 34 * s};
        const std::string label = displayDevice(i == 1);
        if (ui_.button(id(10 + i), pick, "")) { picker = i; pickerScroll = 0; }
        rend_.textIn(fBody_,{pick.x+10*s,pick.y,pick.w-32*s,pick.h},label.c_str(),nx::text,Align::Left,0);
        const f32 arrowX=pick.right()-15*s;
        rend_.line(arrowX-3*s,pick.cy()-2*s,arrowX,pick.cy()+1*s,s,nx::muted);
        rend_.line(arrowX,pick.cy()+1*s,arrowX+3*s,pick.cy()-2*s,s,nx::muted);
        y += 41 * s;
    }

    const f32 readoutY = y;
    const f32 half = (content.w - 6 * s) * .5f;
    rend_.roundRect({content.x, readoutY, half, 34 * s}, 5 * s, nx::panel2);
    rend_.roundRect({content.x + half + 6 * s, readoutY, half, 34 * s}, 5 * s, nx::panel2);
    char rate[32], block[40];
    std::snprintf(rate, sizeof rate, "%.0f kHz", eng_.driverSampleRate() / 1000.0);
    std::snprintf(block, sizeof block, "%d samples", eng_.driverBufferSize());
    rend_.textIn(fSmall_, {content.x + 10 * s, readoutY, half - 12 * s, 34 * s}, rate, nx::text, Align::Left, 0);
    rend_.textIn(fSmall_, {content.x + half + 16 * s, readoutY, half - 20 * s, 34 * s}, block, nx::text, Align::Left, 0);
    y += 46 * s;
    rend_.line(content.x, y, content.right(), y, s, pal::divider);
    y += 10 * s;

    rend_.textIn(fSmall_, {content.x, y + 4 * s, 104 * s, 19 * s}, "Stream routing", nx::text, Align::Left, 0);
    rend_.textIn(fSmall_, {content.x + 104 * s, y + 4 * s, content.w - 150 * s, 19 * s},
                 "Send master to stream bus", nx::muted, Align::Left, 0);
    Rect toggle{content.right() - 38 * s, y + 2 * s, 38 * s, 22 * s};
    if (ui_.button(id(30), toggle, "", settings.discordCaptureBus, settings.discordCaptureBus ? nx::violet : nx::panel2)) {
        settings.discordCaptureBus = !settings.discordCaptureBus;
        message = "Stream routing changed. Apply and save or restart the engine.";
    }
    rend_.roundRect(toggle, 11 * s, settings.discordCaptureBus ? nx::violet : pal::divider);
    const f32 knobX = settings.discordCaptureBus ? toggle.right() - 20 * s : toggle.x + 3 * s;
    rend_.roundRect({knobX, toggle.y + 3 * s, 16 * s, 16 * s}, 8 * s, nx::text);
    y += 32 * s;
    rend_.textIn(fSmall_, {content.x, y + 4 * s, 82 * s, 18 * s}, "Destination", nx::muted, Align::Left, 0);
    rend_.textIn(fBody_, {content.x + 84 * s, y + 3 * s, content.w - 84 * s, 20 * s},
                 "NxTakt Stream", nx::text, Align::Left, 0);
    y += 25 * s;
    rend_.textIn(fSmall_, {content.x + 84 * s, y, content.w - 84 * s, 18 * s},
                 "Select this monitor in your capture app.", nx::muted, Align::Left, 0);
    y += 24 * s;

    if (tab == 1) {
        rend_.textIn(fSmall_, {content.x, y, content.w, 20 * s},
                     "Fallback device preferences; active backend stays unchanged.", nx::muted, Align::Left, 0);
        y += 23 * s;
    }
    Rect advancedToggle{content.x, y, content.w, 27 * s};
    if (ui_.button(id(12), advancedToggle, advanced ? "Advanced routing  −" : "Advanced routing  +", advanced)) {
        advanced = !advanced;
        bodyScroll = 0;
    }
    y += 32 * s;
    if (advanced) {
        if (tab == 0) {
            const char* labels[] = {"Output L", "Output R", "Input L", "Input R"};
            for (int i = 0; i < 4; ++i) {
                rend_.textIn(fSmall_, {content.x, y + 6 * s, 76 * s, 18 * s}, labels[i], nx::muted, Align::Left, 0);
                Rect pick{content.x + 78 * s, y, content.w - 78 * s, 29 * s};
                const std::string value = settings.jackPorts[i].empty() ? "Automatic" : settings.jackPorts[i] == "-" ? "Disconnected" : settings.jackPorts[i];
                if (ui_.button(id(20 + i), pick, value.c_str())) { picker = i + 2; pickerScroll = 0; }
                y += 31 * s;
            }
        } else {
            rend_.textIn(fSmall_, {content.x, y + 5 * s, content.w, 18 * s},
                         "ALSA devices apply only when ALSA is the active engine.", nx::muted, Align::Left, 0);
            y += 25 * s;
        }
        Rect apply{content.x, y, 112 * s, 30 * s};
        Rect refreshButton{apply.right() + 7 * s, y, 74 * s, 30 * s};
        if (ui_.button(id(40), apply, "Apply and save", false, nx::panel2)) {
            std::string error;
            const bool applied = tab != 0 || applyAudioRoutes(eng_.enginePid(), settings.jackPorts, error, settings.discordCaptureBus);
            if (!applied) message = error;
            else if (!saveAudioSettings(settings)) message = "Could not save audio preferences.";
            else message = tab == 0 ? "Routing applied and saved." : "Fallback devices saved.";
            if (tab == 0 && applied) routes = inspectAudioRoutes(eng_.enginePid());
        }
        if (ui_.button(id(41), refreshButton, "Refresh")) {
            const AudioSettings unsaved = settings;
            refreshLists();
            settings = unsaved;
            message = "Device list refreshed; unsaved selections kept.";
        }
    }
    rend_.popClip();

    if (!message.empty()) rend_.textIn(fSmall_, {content.x, footerTop, content.w, 18 * s}, message.c_str(), nx::muted, Align::Left, 0);
    const bool connected = eng_.link() == EngineLink::Live;
    const f32 rowY = footerTop + 21 * s;
    rend_.roundRect({content.x, rowY + 12 * s, 8 * s, 8 * s}, 4 * s, connected ? pal::meterGreen : nx::danger);
    rend_.textIn(fSmall_, {content.x + 14 * s, rowY, std::max(0.f, content.w - 174 * s), 32 * s},
                 connected ? "Audio engine connected" : "Audio engine unavailable", connected ? nx::text : nx::amber, Align::Left, 0);
    Rect restart{content.right() - 154 * s, rowY, 154 * s, 34 * s};
    if (ui_.button(id(42), restart, "Restart audio engine", true, nx::violet)) {
        if (!saveAudioSettings(settings)) message = "Could not save audio preferences. Engine was not restarted.";
        else if (!eng_.restartEngine()) message = "Audio engine restart failed. Check the log.";
        else {
            std::string error;
            const bool routed = tab != 0 || applyAudioRoutes(eng_.enginePid(), settings.jackPorts, error, settings.discordCaptureBus);
            routes = inspectAudioRoutes(eng_.enginePid());
            message = routed ? "Audio engine restarted. Project restored; transport is stopped."
                             : "Audio engine restarted, but routing failed: " + error;
        }
    }
}
} // namespace lat
