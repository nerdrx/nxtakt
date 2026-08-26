// Devices: chain-owner addressing, add / remove / publish chains, the plugin
// scan, and the whole DEVICES tab (browser + strip + param knobs). Moved
// verbatim from app.cpp.
//
#include "app.h"
#include "app_internal.h"
#include "pianoroll.h"
#include "../core/project.h"
#include "../gfx/gl.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <new>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>

namespace lat {

// ---------------------------------------------------------------------------
// THE JUDGMENT, for this file (docs/DESIGN.md §4).
//
// This whole tab is chrome, so it takes the tier table properly: the browser is
// a WELL recessed into the detail panel, every device is a small CARD with the
// faked glass fill and the 1px lit edge, and the rack panel is the one card
// that carries the lit-violet edge because it is the inside of the box beside
// it. Nothing here is a working surface, so nothing here has to stay flat --
// but §4's cardinal rule still holds: cards fake their glass. A chain of eight
// devices with a rack open is a dozen glass surfaces on screen, which is
// exactly the count that makes real blur a slideshow.
//
// One structural note. textIn() already truncates with an ellipsis to fit the
// rect it is given, so the pushClip/popClip pairs that used to fence every name
// in here bought nothing and cost two draw calls each -- the scissor is GL
// state and setting it flushes the batch. They are gone; where a name needs to
// stop early it is textIn's rect that stops it.
// ---------------------------------------------------------------------------

namespace {

// A micro-label cut to fit its box.
//
// Ui::microIn draws glyph by glyph -- that is how it gets §5's tracking -- so
// unlike textIn it has no ellipsis logic, and a plugin name is arbitrary text
// off disk. Trimming here is what keeps a long name inside its card without a
// scissor around it, and a scissor is two draw calls every time it is set.
void microFit(Ui& ui, const Font& f, const Rect& b, const char* s, const Col& c,
              Align a = Align::Left, f32 pad = 0.f) {
    if (!s || !*s) return;
    const f32 avail = b.w - pad * 2.f;
    if (avail <= 1.f) return;
    char buf[72];
    snprintf(buf, sizeof buf, "%s", s);
    if (ui.microWidth(f, buf) > avail) {
        const f32 dots = ui.microWidth(f, "..");
        size_t n = strlen(buf);
        while (n > 1 && ui.microWidth(f, buf) + dots > avail) buf[--n] = 0;
        if (n + 2 < sizeof buf) { buf[n] = '.'; buf[n + 1] = '.'; buf[n + 2] = 0; }
    }
    ui.microIn(f, b, buf, c, a, pad);
}

// What a plugin's format chip says. `formatName` spells Internal in full, which
// is right in a log line and four characters too long for a badge -- at 10px
// over 0.12em of tracking it does not fit beside a name and a vendor, and a
// badge that ellipsises its own word is worse than no badge. NxTakt's own
// devices wear the brand instead, which is also more use than the word: it says
// whose plugin this is, not which loader will open it.
const char* formatBadge(PluginFormat f) {
    return f == PluginFormat::Internal ? "NX" : formatName(f);
}

// ---------------------------------------------------------------------------
// NXTAKT_DEBUG_DEVRECTS=1 -- the measuring tape, in the NXTAKT_DEBUG_* family.
//
// A usability pass has to be able to say how big every clickable thing on this
// tab actually IS, at the DPI the user runs, and reading a rect out of the
// source is how a pass measures the code it wishes it had written: the number
// in the expression is not the number on the screen once a parent rect, a hit
// pad or a layout branch has had a turn. This logs what was really passed to
// the widget, with its short side already worked out, and the pad the call site
// asked for beside it -- so the 16 px floor is checked against the program.
//
// It re-dumps every ~3 seconds rather than once, because half the rects on this
// tab do not exist until something is opened: a rack panel, a selected device's
// parameter grid and a drag in flight are all states a one-shot dump at startup
// would never see.
//
// Inert without the variable, and free with it: one predicate per rect.
bool devRectLog = false;                    // set once a frame in drawDeviceDetail

void devRect(const char* what, const Rect& b, f32 pad = 0.f) {
    if (!devRectLog) return;
    const f32 sw = std::min(b.w, b.h), sh = std::min(b.w + pad * 2.f, b.h + pad * 2.f);
    LOGI("DEVRECT %-20s x=%7.1f y=%7.1f w=%6.1f h=%6.1f  short=%5.1f  pad=%.1f  aim=%5.1f%s",
         what, (f64)b.x, (f64)b.y, (f64)b.w, (f64)b.h, (f64)sw, (f64)pad, (f64)sh,
         sh + 0.01f < 16.f ? "  UNDER-16" : "");
}

// NXTAKT_DEBUG_PROBE -- the DEVICES tab's half of the headless drive, in the
// shape pianoroll.cpp and arrange.cpp already set.
//
// Every gesture this tab owns is a CHAIN EDIT, and a chain edit is the one
// thing a screenshot is worst at: two devices of the same kind look identical,
// the strip may have scrolled, and "the card moved" and "the card was removed
// and another added" draw the same. This prints the whole path in the order the
// engine will run it, from the one function every structural edit goes through.
void probeChain(const char* who, const std::vector<DeviceModel>& devices) {
    static const bool on = std::getenv("NXTAKT_DEBUG_PROBE") != nullptr;
    if (!on) return;
    std::string line;
    for (size_t i = 0; i < devices.size(); ++i) {
        if (i) line += " > ";
        line += devices[i].desc.name;
        if (devices[i].bypass) line += " (bypassed)";
        if (!devices[i].inst)  line += " (not installed)";
    }
    LOGI("NXTAKT_DEBUG_PROBE: chain %s [%zu] %s", who, devices.size(),
         line.empty() ? "(empty)" : line.c_str());
}

// ---------------------------------------------------------------------------
// A DRAG IN FLIGHT ON THIS TAB.
//
// FL Studio's two chain gestures are both drags -- a device is dragged along
// its chain to reorder it, and a plugin is dragged out of the browser onto the
// slot it should take rather than always onto the end -- and NxTakt had
// neither: the top-level chain had no reorder at ALL (the rack's inside has
// had up/down chevrons since it shipped), and the browser could only append.
//
// It is a file-local rather than a member of App::drag_ (DragState, session.h)
// for two reasons. The enum there says None/BrowserFile/Clip and that file
// belongs to another owner; and a gesture on this tab cannot outlive the tab,
// because the only thing that can end it -- the mouse button coming up -- is
// polled every frame whether the tab drew or not (see devDragTick).
//
// `insertAt` and `caretX` are OUTPUTS of the strip's layout pass rather than
// state: the strip alone knows where the cards are, so it answers "which slot
// is under the pointer" fresh every frame and clears it at the top of the next.
struct DevDrag {
    enum class Kind { None, Card, Plugin } kind = Kind::None;
    PluginDesc desc;             // Plugin: what was picked up, by VALUE -- the
                                 // browser's list is rebuilt every frame and
                                 // swaps whole when the catalog lands.
    int  from  = -1;             // Card: the slot it was picked up from
    int  owner = -1;             // the chain it was picked up on / for
    f32  x0 = 0, y0 = 0;
    bool armed = false;          // past the movement threshold
    int  insertAt = -1;          // where a drop would land, this frame
};
DevDrag devDrag;

// How far the open rack's chain list is scrolled. File-local for the same
// reason devDrag is, and for one more: exactly one rack is open at a time
// (rackOpenUid_ is a single uid, and rackPath_ walks INTO that one), so there
// is no second list for a second value to belong to.
f32 rackChainScroll = 0.f;

// Once a frame, before either half of the tab draws.
//
// The threshold is app_session.cpp's -- 5 px, squared -- because a drag that
// arms at a different distance in a different panel is a program that feels
// like two programs. The last line is what makes this self-healing: the button
// has been up for a whole frame, so whatever was in flight either landed on
// the frame it was released or was released somewhere that does not take
// drops, and either way there is nothing left to hold.
void devDragTick(const Input& in) {
    if (devDrag.kind == DevDrag::Kind::None) return;
    if (!devDrag.armed) {
        const f32 dx = in.mx - devDrag.x0, dy = in.my - devDrag.y0;
        if ((dx * dx + dy * dy) > 25.f) devDrag.armed = true;
    }
    devDrag.insertAt = -1;
    if (!in.down[0] && !in.released[0]) devDrag = DevDrag{};
}

} // namespace

// ---------------------------------------------------------------------------
// device chains
//
// See the lifecycle comment in app.h: the GUI owns every RtChain and every
// PluginInstance, the audio thread only borrows them, and the handshake below
// is the only path on which anything is freed while audio runs.
// ---------------------------------------------------------------------------

// Where one owner's chain lives. The three cases differ in nothing else, which
// is the whole point: past this function no code below knows what a return is.
App::ChainOwner App::chainOwner(int owner) {
    ChainOwner co;
    if (owner == kOwnMaster) {
        co.devices   = &ses_.masterDevices;
        co.saved     = &ses_.masterSavedDevices;
        co.published = &publishedMaster_;
        co.cmd       = Cmd::SetMasterChain;
        co.addr      = -1;                      // as the retirement event says it
    } else if (ownIsReturn(owner)) {
        ReturnModel& rm = ses_.returns[owner - kOwnReturn0];
        co.devices   = &rm.devices;
        co.saved     = &rm.savedDevices;
        co.published = &publishedReturn_[owner - kOwnReturn0];
        co.cmd       = Cmd::SetReturnChain;
        co.addr      = owner - kOwnReturn0;
    } else if (ownIsTrack(owner)) {
        co.published = &published_[owner];
        co.cmd       = Cmd::SetChain;
        co.addr      = owner;
        // A published slot outlives the track model: a set that shrank leaves
        // the engine running a chain for an index the session no longer has.
        if (owner < (int)ses_.tracks.size()) {
            co.devices = &ses_.tracks[owner].devices;
            co.saved   = &ses_.tracks[owner].savedDevices;
        }
    }
    return co;
}

std::string App::ownerName(int owner) const {
    if (owner == kOwnMaster) return "Master";
    if (ownIsReturn(owner)) return std::string("Return ") + kReturnLetter[owner - kOwnReturn0];
    if (owner >= 0 && owner < (int)ses_.tracks.size()) return ses_.tracks[owner].name;
    char buf[32];
    snprintf(buf, sizeof buf, "track %d", owner);
    return buf;
}

std::vector<int> App::modelOwners() const {
    std::vector<int> v;
    v.reserve(ses_.tracks.size() + kMaxReturns + 1);
    for (int t = 0; t < (int)ses_.tracks.size(); ++t) v.push_back(t);
    for (int i = 0; i < kMaxReturns; ++i) v.push_back(ownReturn(i));
    v.push_back(kOwnMaster);
    return v;
}

void App::publishChain(int owner) {
    ChainOwner co = chainOwner(owner);
    if (!co.valid() || !co.devices) return;

    RtChain* chain = new RtChain();
    int n = 0;
    for (const DeviceModel& d : *co.devices) {
        if (!d.inst) continue;
        if (n >= kMaxChainFx) {
            LOGW("%s has more than %d devices - the extras will not sound",
                 ownerName(owner).c_str(), kMaxChainFx);
            break;
        }
        // Bypassed devices stay in the chain: the instance itself short-circuits
        // in process(), which keeps the chain stable across a bypass toggle.
        chain->fx[n++] = d.inst.get();
    }
    chain->count = n;

    Command c;
    c.type = co.cmd;
    c.a = co.addr;
    c.p = chain;
    if (!eng_.pushCommand(c)) {
        // The ring is full, so the engine never saw this chain. It is still
        // solely ours, and the previously published one is still live: drop the
        // new one and leave every piece of state exactly as it was.
        LOGW("command ring full - chain for %s not published", ownerName(owner).c_str());
        delete chain;
        return;
    }

    if (*co.published) retiring_.push_back(RetiredChain{*co.published, {}});
    *co.published = chain;

    // A device-param lane names its target by chain SLOT, so a chain edit can
    // move the parameter an arrangement lane was resolved against — or remove
    // it. Republishing re-resolves every lane on this track against the chain
    // that now exists; without it, an automation lane keeps driving whatever
    // device inherited its slot. Clip envelopes re-resolve through pushClip on
    // their own schedule; the track's arrangement lanes have no such trigger.
    if (ownIsTrack(owner)) publishArrangeAutos(owner);

    // The one choke point every structural edit passes through, which is why
    // the probe lives here and not at the five call sites that edit a chain.
    probeChain(ownerName(owner).c_str(), *co.devices);
}

void App::addDevice(int owner, const PluginDesc& d) {
    ChainOwner co = chainOwner(owner);
    if (!co.valid() || !co.devices) return;
    std::vector<DeviceModel>& devices = *co.devices;
    if ((int)devices.size() >= kMaxChainFx) {
        status_ = "Chain is full";
        return;
    }

    // instantiate() already calls prepare() on the instance (see the tail of
    // instantiateLV2/instantiateCLAP), so a non-null return is ready to run.
    //
    // §12.7(1)'s caveat, still true in this wave: the browser may list the
    // DAEMON's catalog, but the model's instance is built HERE by the local
    // registry -- so a row only the daemon can see would be listed and then
    // fail on this line. Both processes scan the same machine, so that is the
    // divergence the catalog exists to make visible rather than a new failure;
    // it disappears when DeviceModel::inst does (§5 step 4).
    std::unique_ptr<PluginInstance> inst =
        registry_.instantiate(d, eng_.sampleRate(), kMaxBlock);
    if (!inst) {
        status_ = "Could not load " + d.name;
        return;
    }

    DeviceModel dm;
    dm.uid = ses_.newUid();
    dm.desc = d;
    dm.inst = std::move(inst);
    devices.push_back(std::move(dm));

    const RtChain* before = *co.published;
    publishChain(owner);
    if (*co.published == before) {
        // Publish failed. The engine never referenced this instance, so it is
        // safe to destroy right here and leave the model matching the engine.
        devices.pop_back();
        status_ = "Engine busy - device not added";
        return;
    }
    selDevice_ = (int)devices.size() - 1;
    paramScroll_ = 0.f;
    status_ = "Added " + d.name;
}

void App::removeDevice(int owner, int idx) {
    ChainOwner co = chainOwner(owner);
    if (!co.valid() || !co.devices) return;
    std::vector<DeviceModel>& devices = *co.devices;
    if (idx < 0 || idx >= (int)devices.size()) return;

    // Move the instance out of the model rather than letting erase() destroy
    // it: the audio thread is still running the *outgoing* chain, which points
    // straight at it. It may only die once that chain comes back to us.
    DeviceModel dead = std::move(devices[idx]);
    devices.erase(devices.begin() + idx);

    const RtChain* outgoing = *co.published;
    publishChain(owner);

    if (*co.published == outgoing) {
        // Publish failed; the engine still runs the old chain, so the device
        // has to go back where it was or the model would lie about what sounds.
        devices.insert(devices.begin() + idx, std::move(dead));
        status_ = "Engine busy - device not removed";
        return;
    }
    if (outgoing) {
        // publishChain() just appended the entry for `outgoing`; the instance
        // rides along in it and is freed when Ev::ChainRetired arrives.
        retiring_.back().dying.push_back(std::move(dead.inst));
    }
    // Otherwise nothing was ever published, so nothing borrowed the instance
    // and it is freed as `dead` goes out of scope.

    if (devices.empty())                        selDevice_ = -1;
    else if (selDevice_ >= (int)devices.size()) selDevice_ = (int)devices.size() - 1;
    paramScroll_ = 0.f;
    status_ = "Removed " + dead.desc.name;
}

// ---------------------------------------------------------------------------
// racks
//
// A rack is a device like any other -- that is the whole design (docs/RACKS.md)
// -- so everything above already works on one. What is here is only the two
// things a container needs that a plain device does not: a way to see and edit
// what is inside it, and the two lifetime obligations §1 and §2 name.
// ---------------------------------------------------------------------------

// Depth-first, children before their parent, so a nested rack's retired
// devices go before the rack that owns them. Bounded by kRackMaxDepth.
static void reclaimRackTree(PluginInstance* p) {
    if (!p) return;
    // A sampler retires its displaced SampleBuffers under the same discipline
    // a rack retires layouts and sub-devices, and this walk is the same proof
    // at the same moment: retiring_ empty means the audio thread has
    // acknowledged every chain this process ever published, so nothing can
    // still be reading a buffer the instrument unlinked. Until this call
    // existed the sampler held displaced buffers to prepare() or destruction
    // and warned at 32 -- filed by its own author, wired here.
    if (SamplerControl* sc = p->sampler()) sc->reclaim();
    RackControl* rc = p->rack();
    if (!rc) return;
    for (int i = 0; i < rc->deviceCount(); ++i) reclaimRackTree(rc->device(i));
    rc->reclaim();
}

// docs/RACKS.md §2: removing a sub-device UNLINKS it realtime-safely, but
// nothing inside a PluginInstance can know when the audio thread last
// dereferenced the pointer, so the instance is retired rather than freed and
// somebody has to say when it may go.
//
// This is that somebody, and the condition is `retiring_` being empty. Every
// chain this process publishes goes into that list and comes out again when
// Ev::ChainRetired says the audio thread has swapped it out and will never look
// at it again. Empty therefore means: every chain we have ever published has
// been acknowledged, so the engine has crossed at least one block boundary
// since each of them. A rack publishes its own topology (one release store into
// its ring) BEFORE the chain that carries the rack is published, so an
// acknowledged chain implies an acknowledged layout -- and no process() call
// can still be holding the old one.
//
// Fail-closed by construction: while anything is outstanding this does nothing
// at all, and the worst case is that unlinked sub-devices live a little longer.
// The rack refuses new devices at 64 retired and says so, which is the loud
// failure that would tell us this is not firing often enough.
// Walks the three containers directly rather than through modelOwners(),
// because this runs once a frame while the DEVICES tab is open and that helper
// builds a vector every time it is asked.
void App::reclaimRacks() {
    if (!retiring_.empty()) return;
    for (TrackModel& t : ses_.tracks)
        for (DeviceModel& d : t.devices) reclaimRackTree(d.inst.get());
    for (ReturnModel& r : ses_.returns)
        for (DeviceModel& d : r.devices) reclaimRackTree(d.inst.get());
    for (DeviceModel& d : ses_.masterDevices) reclaimRackTree(d.inst.get());
}

// docs/RACKS.md §1, the one caller obligation the rack cannot honour from the
// inside. A rack's latencyFrames() is the SUM of its chain, so a structural
// edit changes it -- but engine.cpp reads a chain's latency exactly once, when
// the chain is published, which is correct per the PluginInstance contract
// ("constant after prepare()"). The track's device list has not changed and the
// rack is the same pointer; what changed is what it reports. Without this the
// engine keeps compensating for the latency the rack had a moment ago and every
// parallel path in the set drifts by the difference.
void App::rackChainEdited() {
    publishChain(devOwner_);
    reclaimRacks();
}

// Headless verification hook, in the pattern app.cpp's NXTAKT_DEBUG_ADDFX and
// the arrangement's seeds already set. Nothing inside gamescope can click, so
// NXTAKT_DEBUG_RACK=<plugin substrings, comma separated> finds the first rack
// in the chain the DEVICES tab is on, opens it, fills it with those plugins and
// maps macro 1 across the first two parameters it finds -- the second inverted,
// so a screenshot shows a partial range, an inverted one and the mapping list
// all at once. Runs once; inert without the variable.
//
// It goes through the ordinary editing API, republish included, so the hook
// exercises the same path the mouse does rather than a back door.
void App::debugSeedRack() {
    const char* want = env("DEBUG_RACK");
    if (!want) return;

    ChainOwner co = chainOwner(devOwner_);
    if (!co.devices) return;
    RackControl* rc = nullptr;
    for (DeviceModel& d : *co.devices) {
        if (!d.inst || !d.inst->rack()) continue;
        rc = d.inst->rack();
        rackOpenUid_ = d.uid;
        rackPath_.clear();
        break;
    }
    if (!rc) { LOGW("NXTAKT_DEBUG_RACK: no rack in the chain on %s",
                    ownerName(devOwner_).c_str()); return; }

    ensurePluginScan();
    std::string list = want, one;
    size_t start = 0;
    while (start <= list.size()) {
        const size_t comma = list.find(',', start);
        one = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        start = comma == std::string::npos ? list.size() + 1 : comma + 1;
        if (one.empty()) continue;
        const PluginDesc* hit = nullptr;
        for (const PluginDesc& d : registry_.plugins())
            if (icontains(d.name, one)) { hit = &d; break; }
        if (!hit) { LOGW("NXTAKT_DEBUG_RACK: no plugin matching \"%s\"", one.c_str()); continue; }
        if (rc->addDevice(*hit)) {
            rackChainEdited();
            LOGI("NXTAKT_DEBUG_RACK: %s -> slot %d, rack latency %d frames",
                 hit->name.c_str(), rc->deviceCount(), rackLatencyOf(rc));
        }
    }

    // A rack inside the rack gets something in it too, so the nested case is
    // reachable from a screenshot: NXTAKT_DEBUG_RACKPATH=<slot> then opens it.
    for (int i = 0; i < rc->deviceCount(); ++i) {
        RackControl* inner = rc->device(i) ? rc->device(i)->rack() : nullptr;
        if (!inner) continue;
        for (const PluginDesc& d : registry_.plugins())
            if (d.uri == "nxtakt:saturator") { inner->addDevice(d); break; }
        for (const PluginDesc& d : registry_.plugins())
            if (d.uri == "nxtakt:delay") { inner->addDevice(d); break; }
        if (inner->deviceCount() > 0 && inner->device(0)->paramCount() > 0) {
            const ParamInfo& p = inner->device(0)->paramInfo(0);
            RackMapping m;
            m.macro = 3; m.device = 0; m.param = p.id; m.min = p.max; m.max = p.min;
            inner->addMapping(m);
        }
        rackChainEdited();
    }
    if (const char* into = env("DEBUG_RACKPATH")) {
        const int slot = atoi(into);
        if (slot >= 0 && slot < rc->deviceCount() && rc->device(slot)->rack()) {
            rackPath_.push_back(slot);
            LOGI("NXTAKT_DEBUG_RACKPATH: opened the rack in slot %d", slot + 1);
        } else {
            LOGW("NXTAKT_DEBUG_RACKPATH: slot %s is not a rack", into);
        }
    }

    for (int i = 0; i < rc->deviceCount() && i < 2; ++i) {
        PluginInstance* sub = rc->device(i);
        if (!sub || sub->paramCount() == 0) continue;
        const ParamInfo& p = sub->paramInfo(0);
        RackMapping m;
        m.macro = 0; m.device = i; m.param = p.id;
        // First mapping: the top half of the range. Second: inverted, so both
        // shapes are visible at once.
        if (i == 0) { m.min = p.min + (p.max - p.min) * 0.5f; m.max = p.max; }
        else        { m.min = p.max; m.max = p.min; }
        if (rc->addMapping(m) < 0) LOGW("NXTAKT_DEBUG_RACK: mapping %d refused", i);
    }
    rackSel_ = 0;
    rackMacro_ = 0;
    rackTgtDev_ = rc->deviceCount() > 1 ? 1 : 0;
    rackTgtParam_ = 0;
    rackRangeHeld_ = false;
    selDevice_ = -1;
    for (size_t i = 0; i < co.devices->size(); ++i)
        if ((*co.devices)[i].uid == rackOpenUid_) selDevice_ = (int)i;
    status_ = "NXTAKT_DEBUG_RACK: rack seeded and opened";

    // LAST, and that is not tidiness. It calls undo(), which goes through
    // adoptSession and MOVE-ASSIGNS ses_ -- so every DeviceModel* and every
    // ChainOwner taken above this line is dangling the moment it returns.
    // (`rackOpenUid_` survives it because it is a uid: openRack() re-resolves
    // the whole path every frame, which is the reason it is a uid.) ASan found
    // this the first time it ran; the ordinary UI path is not exposed to it,
    // because nothing that draws holds a chain across an undo.
    debugRackUndoCheck(rc);
}

// The rack half of the undo self-test, in app_undo.cpp's shape and for the
// same reason: nothing inside gamescope can press Ctrl+Z, and this is the one
// property of format v8 that only shows up when the whole snapshot / restore
// path runs. A structural rack edit must survive undo AND redo, which is true
// only because SavedDevice::state carries the rack's contents into the snapshot
// and materializeDevices puts them back -- parameters first.
//
// Runs when NXTAKT_DEBUG_RACK and NXTAKT_DEBUG_UNDO are both set, and leaves
// the set exactly as it found it.
void App::debugRackUndoCheck(RackControl* rc) {
    if (!env("DEBUG_UNDO") || !rc) return;
    const auto text = [this]() {
        std::string t;
        std::vector<ClipSample> s;
        snapshotSession(t, s);
        return t;
    };
    const PluginDesc* sat = nullptr;
    for (const PluginDesc& d : registry_.plugins())
        if (d.uri == "nxtakt:saturator") { sat = &d; break; }
    if (!sat) { LOGW("rack undo check: no saturator to add"); return; }

    const std::string before = text();
    undoPoint("rack undo check");
    if (!rc->addDevice(*sat)) { LOGW("rack undo check: the rack refused the device"); return; }
    rackChainEdited();
    const std::string after = text();

    int bad = 0;
    // `rc` is deliberately not touched past this point: undo() replaces the
    // session, and although the rack instance is CARRIED rather than rebuilt,
    // the check has no business depending on that.
    if (after == before) {
        LOGE("rack undo check: the snapshot did not change - the rack's contents "
             "are not reaching SavedDevice::state");
        ++bad;
    }
    undo();
    if (text() != before) { LOGE("rack undo check: undo did not restore the rack"); ++bad; }
    redo();
    if (text() != after)  { LOGE("rack undo check: redo did not restore the rack"); ++bad; }
    undo();
    if (text() != before) { LOGE("rack undo check: the second undo did not restore the rack"); ++bad; }

    if (bad == 0) LOGI("rack undo check: PASS - a structural rack edit survives undo and redo");
    else          LOGE("rack undo check: FAIL - %d problem(s)", bad);
}

// The rack's own reported latency, for the log line above.
int App::rackLatencyOf(RackControl* rc) const {
    (void)rc;
    ChainOwner co = const_cast<App*>(this)->chainOwner(devOwner_);
    if (!co.devices) return 0;
    for (DeviceModel& d : *co.devices)
        if (d.inst && d.inst->rack() == rc) return d.inst->latencyFrames();
    return 0;
}

RackControl* App::openRack() {
    if (!rackOpenUid_) return nullptr;
    ChainOwner co = chainOwner(devOwner_);
    if (!co.devices) { rackOpenUid_ = 0; rackPath_.clear(); return nullptr; }

    PluginInstance* inst = nullptr;
    for (DeviceModel& d : *co.devices)
        if (d.uid == rackOpenUid_ && d.inst) { inst = d.inst.get(); break; }
    RackControl* rc = inst ? inst->rack() : nullptr;
    if (!rc) { rackOpenUid_ = 0; rackPath_.clear(); return nullptr; }

    // Walk in, and TRUNCATE the path at the first step that no longer lands on
    // a rack. A sub-device can be removed or moved while the panel is open, and
    // the honest response is to surface the level that does still exist rather
    // than to fold the whole panel or, worse, to follow the index onto whatever
    // device inherited it.
    for (size_t i = 0; i < rackPath_.size(); ++i) {
        PluginInstance* sub = rc->device(rackPath_[i]);
        RackControl* inner = sub ? sub->rack() : nullptr;
        if (!inner) { rackPath_.resize(i); break; }
        rc = inner;
    }
    return rc;
}

void App::ensurePluginScan() {
    // Daemon mode first: the list that matters is what the ENGINE's process can
    // instantiate, and the daemon scans on its own machine-time. requestScan()
    // is idempotent -- and once the daemon reports ScanDone it is also the call
    // that reads the catalog out of the control region, so it is asked every
    // frame until catalogReady() answers yes. The browser draws its scanning
    // state off scanRunning() and swaps to the catalog when it lands. The local
    // registry still scans below as the fallback -- and because addDevice()
    // still instantiates locally, so a local instance must exist for anything
    // the user actually adds.
    if (eng_.remoteOpen() && !eng_.catalogReady()) eng_.requestScan();
    if (registryScanned_) return;
    // lilv walks every bundle on the system and a CLAP scan dlopens each
    // binary, which costs the better part of a second. Deferring it to the
    // first time the DEVICES tab opens keeps startup snappy for anyone who
    // never touches a plugin.
    status_ = "Scanning plugins...";
    registry_.scan();
    registryScanned_ = true;
    char buf[48];
    snprintf(buf, sizeof buf, "%zu plugins", registry_.plugins().size());
    status_ = buf;
}


// ---------------------------------------------------------------------------
// device view: plugin browser on the left, the selected track's chain right
// ---------------------------------------------------------------------------

void App::drawDeviceDetail(const Rect& r) {
    const f32 s = win_.dpiScale();
    // The scan is lazy, and the tab can also be reached by restoring a session
    // with the tab already active, so make sure it has happened.
    ensurePluginScan();

    devDragTick(win_.input());

    // The measuring tape, armed for one frame in every ~180 (see devRect).
    {
        static int tick = 0;
        devRectLog = env("DEBUG_DEVRECTS") && (++tick % 180) == 1;
        if (devRectLog) LOGI("DEVRECT --- dpiScale=%.2f panel=%.0fx%.0f ---", (f64)s,
                             (f64)r.w, (f64)r.h);
    }

    const f32 listW = 236 * s;
    Rect list{r.x, r.y, listW, r.h};
    Rect strip{list.right() + 1 * s, r.y, r.right() - list.right() - 1 * s, r.h};
    drawPluginBrowser(list);      // draws the hairline down its own right edge
    drawDeviceStrip(strip);
}

void App::drawPluginBrowser(const Rect& r) {
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    // A list inside the detail panel's surface, so it recesses. §4: a content
    // region inside glass is a well, never a second frosted layer -- glass
    // inside glass reads as fog and takes the hierarchy with it.
    rend_.well(r, 0.f);
    rend_.hairlineV(r.right(), r.y, r.bottom());

    // In daemon mode the browsable list is the DAEMON's catalog -- what the
    // engine's process proved it can instantiate -- not what this process's
    // registry found. The two are the same machine today, but the catalog is
    // the honest source: a plugin only one side can see is exactly the
    // divergence the catalog exists to make visible.
    const bool fromCatalog = eng_.remoteOpen() && eng_.catalogReady();
    const bool scanning    = eng_.remoteOpen() && !fromCatalog && eng_.scanRunning();
    const u32  catalogCut  = fromCatalog ? eng_.catalogTruncated() : 0;
    const std::vector<PluginDesc>& all =
        fromCatalog ? eng_.catalog() : registry_.plugins();

    // --- header: the §5 chip language, 10px uppercase over wide tracking ----
    Rect head{r.x + nx::sp1 * s, r.y + 4 * s, r.w - nx::sp1 * 2.f * s, 12 * s};
    ui_.microIn(fSmall_, head, "BROWSER", nx::muted, Align::Left, 0);
    if (scanning) {
        // The daemon is still walking its bundles, so the rows below are this
        // process's own scan standing in. Quiet, not a banner: the list is
        // usable meanwhile and swaps to the catalog the frame it lands.
        ui_.microIn(fSmall_, head, "scanning...", nx::muted.alpha(0.8f), Align::Right, 0);
        if (ui_.hovered(head))
            ui_.tip = "The engine is scanning its plugins - showing this "
                      "process's own scan until the catalog arrives";
    }
    // The count is drawn AFTER the filter has run (see below): a header that
    // says 424 over a list saying "No match" is a header describing a list
    // nobody is looking at.

    // --- filter ---
    const u64 fid = uiId(10, 0);
    Rect filter{r.x + 6 * s, head.bottom() + 4 * s, r.w - 12 * s, 17 * s};
    // The field is recessed at rest and takes the violet border and the focus
    // ring the moment the caret arrives (§5: never a bare outline). textField
    // paints the focused state itself, so this is only the resting well.
    devRect("browser.filter", filter);
    ui_.fieldWell(filter, 0.f);
    ui_.textField(fid, filter, &pluginFilter_, Col(0, 0, 0, 0), nx::text, Align::Left, false);
    // textField only writes back on commit, but a filter has to narrow as you
    // type, so read the live edit buffer while this field owns the caret.
    const std::string* live = ui_.liveText(fid);
    const std::string& query = live ? *live : pluginFilter_;
    if (query.empty())
        rend_.textIn(fSmall_, filter, "Filter plugins", nx::muted.alpha(0.6f), Align::Left, 6 * s);

    // --- filtered index, rebuilt each frame: a few hundred string compares ---
    static std::vector<int> shown;                  // reused to avoid churn
    shown.clear();
    for (int i = 0; i < (int)all.size(); ++i)
        if (icontains(all[i].name, query)) shown.push_back(i);

    // The count, now that the filter has had its say -- and BEFORE the list's
    // clip is pushed, which is where the first cut of this put it: microIn into
    // the header under a scissor set to the list draws nothing at all, and the
    // header simply lost its number. A header that says 424 over a list saying
    // "No match" is a header describing a list nobody is looking at, so a
    // filtered browser says both numbers.
    if (!scanning) {
        char cnt[48];
        if (query.empty()) snprintf(cnt, sizeof cnt, "%u", (unsigned)all.size());
        else               snprintf(cnt, sizeof cnt, "%u / %u", (unsigned)shown.size(),
                                    (unsigned)all.size());
        ui_.microIn(fSmall_, head, cnt, nx::muted.alpha(0.6f), Align::Right, 0);
    }

    const f32 rowH = 17 * s;
    Rect listR{r.x, filter.bottom() + 6 * s, r.w, r.bottom() - filter.bottom() - 6 * s};
    // A truncated catalog MUST be drawn (engine_handle.h): the list yields one
    // row's height and the footer under it says how many the wire could not
    // carry. Amber -- attention, not damage; a silently short list is the lie.
    if (catalogCut) listR.h -= 13 * s;
    rend_.hairlineH(r.x + nx::sp1 * s, r.right() - nx::sp1 * s, listR.y - 3 * s);
    rend_.pushClip(listR);

    devRect("browser.list", listR);
    if (ui_.setHot(uiId(10, 1), listR) && in.wheel != 0.f) {
        pluginScroll_ -= in.wheel * rowH * 3.f;
    }
    const f32 maxScroll = std::max(0.f, shown.size() * rowH - listR.h);
    pluginScroll_ = clampv(pluginScroll_, 0.f, maxScroll);

    if (shown.empty()) {
        // §5's empty state: one short line, one muted sentence, centred.
        const f32 lh = fBody_.height();
        rend_.textIn(fBold_, {listR.x, listR.y + nx::sp4 * s, listR.w, lh},
                     all.empty() ? "No plugins found" : "No match", nx::text, Align::Center);
        rend_.textIn(fSmall_, {listR.x, listR.y + nx::sp4 * s + lh + nx::sp1 * s, listR.w, lh},
                     all.empty() ? "Install an LV2 or CLAP plugin and restart."
                                 : "Clear the filter to see every plugin.",
                     nx::muted, Align::Center);
    }

    // Shapes for every row, then every row's text. The batcher binds the glyph
    // atlas for text and unbinds it for shapes, and each switch is a draw call;
    // a list that drew each row complete paid four of them per row.
    const f32 rowRad = nx::radiusXs * s;
    f32 y = listR.y - pluginScroll_;
    for (size_t k = 0; k < shown.size(); ++k) {
        Rect row{listR.x, y, listR.w, rowH};
        y += rowH;
        if (row.bottom() < listR.y || row.y > listR.bottom()) continue;

        const int pi = shown[k];
        const PluginDesc& d = all[pi];
        if (k == 0) devRect("browser.row", row);
        const u64 id = uiId(10, 100 + pi);
        const bool hot = ui_.setHot(id, row) && ui_.isHot(id);
        const Rect chipR{row.x + 4 * s, row.y + 1 * s, row.w - 8 * s, row.h - 2 * s};
        if (pi == pluginSel_) {
            rend_.gradRect(chipR, rowRad, nx::glassChip, 0.85f);
            rend_.rect({nx::snapPx(chipR.x), chipR.y, std::max(1.f, nx::snapPx(2 * s)), chipR.h},
                       nx::violet);
        } else if (hot) {
            rend_.gradRect(chipR, rowRad, nx::glassChip, 0.45f);
        }
        if (hot) ui_.cursor = devDrag.armed ? Cursor::Grab : Cursor::Hand;

        Rect tag{row.right() - 46 * s, row.cy() - 6 * s, 40 * s, 12 * s};
        // --radius-xs, not h*0.5: a hand-rolled capsule is exactly the
        // roundness the owner called cheap, and the token is 2px now.
        rend_.gradRect(tag, nx::radiusXs * s, nx::glassChip, 0.9f);

        if (hot && in.pressed[0]) {
            pluginSel_ = pi;
            // FL drags a plugin out of the browser onto the exact slot it
            // should take. The double-click below still appends, which is the
            // fast path for when the position does not matter, and the press
            // that starts a drag is the same press that selects -- so a click
            // that never moves is still just a click.
            devDrag = DevDrag{};
            devDrag.kind  = DevDrag::Kind::Plugin;
            devDrag.desc  = d;
            devDrag.owner = devOwner_;
            devDrag.x0 = in.mx;
            devDrag.y0 = in.my;
        }
        // Double-click loads, matching how the file browser drops a sample.
        // The entry is taken here rather than inside addDevice, which
        // init() also calls through the NXTAKT_DEBUG_ADDFX hook: nothing that
        // happens while the app is starting up belongs in the history.
        //
        // While a rack is OPEN the drop lands inside it instead, which is what
        // an opened container means everywhere else and what the header above
        // the strip says it will do. The rack's own add button does the same
        // thing from the other end, so neither is the only way in.
        if (hot && openRack()) ui_.badge = Badge::Add;   // a rack is open: double-click adds INTO it
        if (hot && in.dblClick) {
            if (RackControl* rc = openRack()) {
                undoPoint("add device to rack");
                if (rc->addDevice(d)) {
                    rackChainEdited();
                    status_ = "Added " + d.name + " to the rack";
                } else {
                    status_ = "Could not add " + d.name + " to the rack";
                }
            } else {
                undoPoint("add device");
                addDevice(devOwner_, d);
            }
        }
    }

    // Pass two: every row's text, in one bind. textIn truncates to its rect, so
    // a long name stops with an ellipsis where the vendor column begins and no
    // scissor is needed to make it.
    y = listR.y - pluginScroll_;
    for (size_t k = 0; k < shown.size(); ++k) {
        Rect row{listR.x, y, listR.w, rowH};
        y += rowH;
        if (row.bottom() < listR.y || row.y > listR.bottom()) continue;

        const int pi = shown[k];
        const PluginDesc& d = all[pi];
        const bool hot = ui_.isHot(uiId(10, 100 + pi));
        const bool sel = pi == pluginSel_;

        Rect tag{row.right() - 46 * s, row.cy() - 6 * s, 40 * s, 12 * s};
        microFit(ui_, fSmall_, tag, formatBadge(d.format), nx::muted.alpha(0.85f),
                 Align::Center);

        Rect vendor{tag.x - 70 * s, row.y, 66 * s, row.h};
        if (!d.vendor.empty())
            rend_.textIn(fSmall_, vendor, d.vendor.c_str(), nx::muted.alpha(0.7f),
                         Align::Right, 0);

        Rect name{row.x + 10 * s, row.y, vendor.x - row.x - 14 * s, row.h};
        rend_.textIn(fBody_, name, d.name.c_str(),
                     sel || hot ? nx::text : nx::muted, Align::Left, 0);
    }
    rend_.popClip();

    if (catalogCut) {
        char cut[64];
        snprintf(cut, sizeof cut, "...and %u more this build cannot list", catalogCut);
        Rect cutR{r.x + nx::sp1 * s, listR.bottom() + 1 * s, r.w - nx::sp1 * 2.f * s, 11 * s};
        microFit(ui_, fSmall_, cutR, cut, pal::meterAmber.alpha(0.9f), Align::Left, 0);
        if (ui_.hovered(cutR))
            ui_.tip = "The engine found more plugins than the catalog table "
                      "can carry - the rest are loadable but not listable";
    }
}

void App::drawDeviceStrip(const Rect& r) {
    const f32 s = win_.dpiScale();
    Input& in = win_.input();
    // No fill: this is the detail panel's own surface, and the device cards
    // below are what floats on it. A second glass layer here would be the fog
    // §4 warns about.

    // The chain being edited belongs to a track, a return or the master; past
    // this point the only difference is the colour of the identity chip.
    ChainOwner co = chainOwner(devOwner_);
    if (!co.devices) {                       // the target went away under us
        devOwner_ = selTrack_;
        co = chainOwner(devOwner_);
        if (!co.devices) return;             // nothing is clipped yet
    }
    // Once, and only when NXTAKT_DEBUG_RACK is set. Here rather than in init()
    // because it needs the plugin scan the DEVICES tab triggers and the chain
    // the tab is parked on.
    static bool seeded = false;
    if (!seeded) {
        seeded = true;
        debugSeedRack();
        // After debugSeedRack, never before: that one calls undo(), which
        // move-assigns ses_ and dangles every DeviceModel* taken above it.
        debugSeedSpectra();
        debugSeedSampler();
        co = chainOwner(devOwner_);
    }
    if (!co.devices) return;

    // Once a frame, and free unless something is actually waiting: an undo that
    // rebuilt a rack unlinks its old contents through setState, and nothing in
    // that path has any way to know when they may be freed. This does.
    reclaimRacks();

    std::vector<DeviceModel>& devices = *co.devices;
    const Col tc = ownIsTrack(devOwner_)
                 ? pal::clipColors[ses_.tracks[devOwner_].colorIdx % pal::clipColorCount]
                 : (ownIsReturn(devOwner_) ? pal::soloBlue : nx::violet);

    Rect head{r.x, r.y, r.w, 16 * s};
    rend_.rect({head.x, head.y + 3 * s, std::max(1.f, nx::snapPx(3 * s)), head.h - 6 * s},
               tc);                                       // owner identity chip
    rend_.hairlineH(head.x + nx::sp1 * s, head.right() - nx::sp1 * s, head.bottom());
    // The hint claims its space first and the NAME takes what is left, bounded
    // by 220 as before. Both used to be drawn against the whole head rect --
    // one left, one right -- which is fine for "Kick" and overprints the moment
    // the chain belongs to "Strings Section Ensemble Legato Sustain" on a strip
    // the file browser has narrowed. A name that has to be cut says itself in
    // full in the status bar (§11: no truncated name without a tip).
    const char* const hint = rackOpenUid_
        ? "A rack is open - double-click or drag a plugin to add it inside"
        : "Double-click a plugin to add it, or drag it onto a slot";
    const f32 hintW = fSmall_.measure(hint) + 16 * s;
    // WHERE THE LATENCY IS. Nothing on this tab said it before, and "what is my
    // sound made of" is not answered by a chain that will not admit it is
    // running eleven milliseconds behind the one beside it. The sum is over the
    // devices that are actually in the path -- a bypassed device short-circuits
    // in process() and reports nothing -- and it is the same number the engine
    // compensates with, because it comes from the same latencyFrames().
    //
    // Muted, not cyan: latency is a FACT about the chain, not something running
    // or something the user set, and §1 spends its two loud colours on those.
    // Zero draws nothing at all, which is the common case and the quiet one.
    int latFrames = 0;
    for (const DeviceModel& dl : *co.devices)
        if (dl.inst && !dl.bypass) latFrames += dl.inst->latencyFrames();
    char latTag[40] = "";
    if (latFrames > 0) {
        const f64 ms = 1000.0 * (f64)latFrames / (f64)std::max(1, (int)eng_.sampleRate());
        snprintf(latTag, sizeof latTag, "%.1f ms latency", ms);
    }
    const f32 latW = latTag[0] ? fSmall_.measure(latTag) + 14 * s : 0.f;
    // deviceStatesRefused() non-zero is an instrument that is drawn loaded --
    // sample name on the card and all -- and plays NOTHING: the daemon refused
    // the state blob that carries what its parameters cannot say. Surfaced
    // here because this tab is where that instrument is being looked at, in
    // the amber counter shape the status bar gives the other refusal families
    // (the filed app_chrome diff adds it to that aggregate too). Zero, and
    // local mode, draw nothing at all: quiet at rest.
    u64 statesRefused = eng_.remoteOpen() ? eng_.deviceStatesRefused() : 0;
    // Headless verification hook in the NXTAKT_DEBUG_* family (debugSeedRack's
    // pattern): a real state refusal needs the daemon to reject a pool blob,
    // which nothing inside gamescope can provoke on cue, so
    // NXTAKT_DEBUG_STATEREFUSED=<n> forces the tag visible for a screenshot.
    // Inert without the variable.
    if (const char* f = env("DEBUG_STATEREFUSED")) statesRefused = (u64)atoll(f);
    char stTag[44] = "";
    if (statesRefused > 0)
        snprintf(stTag, sizeof stTag, "%llu state updates refused",
                 (unsigned long long)statesRefused);
    const f32 stW = stTag[0] ? fSmall_.measure(stTag) + 14 * s : 0.f;
    const std::string owner = ownerName(devOwner_);
    const Rect nameR{head.x + 10 * s, head.y,
                     std::max(40 * s, std::min(220 * s, head.w - hintW - stW - latW - 18 * s)), head.h};
    rend_.textIn(fBold_, nameR, owner.c_str(), nx::text, Align::Left, 0);
    if (ui_.hovered(nameR) && textTruncated(fBold_, owner.c_str(), nameR.w))
        ui_.tip = owner;
    rend_.textIn(fSmall_, head, hint,
                 rackOpenUid_ ? nx::violetSoft : nx::muted.alpha(0.7f), Align::Right,
                 8 * s + stW + latW);
    if (latTag[0]) {
        rend_.textIn(fSmall_, head, latTag, nx::muted.alpha(0.85f), Align::Right, 8 * s + stW);
        const f32 lw = fSmall_.measure(latTag);
        const Rect latR{head.right() - 8 * s - stW - lw, head.y, lw, head.h};
        devRect("head.latency", latR);
        if (ui_.setHot(uiId(UiDeviceTip, 2698), latR) && ui_.isHot(uiId(UiDeviceTip, 2698)))
            ui_.tip = "How far behind this chain runs, added up over the devices "
                      "that are not bypassed. The engine compensates for it; the "
                      "card that causes it says so on its own second line.";
    }
    if (stTag[0]) {
        rend_.textIn(fSmall_, head, stTag, pal::meterAmber, Align::Right, 8 * s);
        const f32 tw = fSmall_.measure(stTag);
        Rect tagR{head.right() - 8 * s - tw, head.y, tw, head.h};
        devRect("head.refusedTag", tagR);
        if (ui_.setHot(uiId(UiDeviceTip, 2699), tagR) && ui_.isHot(uiId(UiDeviceTip, 2699)))
            ui_.tip = "The engine refused a device's state - an instrument may "
                      "be drawn loaded and play nothing. The log says which "
                      "device and why.";
    }

    Rect area{r.x, head.bottom(), r.w, r.bottom() - head.bottom()};
    rend_.pushClip(area);

    // Keep the selection honest: the target can be switched under it, and a
    // device can have been removed since the last frame.
    if (devices.empty()) selDevice_ = -1;
    else selDevice_ = clampv(selDevice_ < 0 ? 0 : selDevice_, 0, (int)devices.size() - 1);

    if (devices.empty()) {
        // §5's empty state: one short bold line, one muted sentence, centred,
        // and an invitation rather than an apology (§9).
        char msg[80];
        snprintf(msg, sizeof msg, "No devices on %s", ownerName(devOwner_).c_str());
        const f32 lh = fBody_.height();
        rend_.textIn(fBold_, {area.x, area.cy() - lh - nx::sp1 * 0.5f * s, area.w, lh},
                     msg, nx::text, Align::Center);
        rend_.textIn(fSmall_, {area.x, area.cy() + nx::sp1 * 0.5f * s, area.w, lh},
                     "Double-click a plugin in the browser to add one, or drag it here.",
                     nx::muted, Align::Center);
        // An empty chain still takes a drop: the caret has nowhere to stand, so
        // the lit edge stands in for it -- the same affordance the sampler card
        // wears, and mid-drag only.
        if (devDrag.kind == DevDrag::Kind::Plugin && devDrag.armed && ui_.hovered(area)) {
            rend_.roundRectOutline(area.inset(6 * s), nx::radiusSm * s,
                                   std::max(1.f, nx::snapPx(s)), nx::violet.alpha(0.7f));
            ui_.badge = Badge::Add;
            if (in.released[0]) {
                undoPoint("add device");
                addDevice(devOwner_, devDrag.desc);
                devDrag = DevDrag{};
            }
        }
        rend_.popClip();
        return;
    }

    const f32 boxW = 150 * s, gap = 5 * s, rackW = 448 * s;
    // Spectra's editor opens the same way a rack does and is laid out to the
    // same constraint -- a dock 200 logical pixels tall -- so it is wide and
    // short. Its width is its own columns added up (lay::spectraPanelW), and it
    // fits the strip with the file browser closed; wider than the strip it
    // simply scrolls, like everything else here.
    const f32 specW = lay::spectraPanelW * s;
    // The Sampler's editor opens the same way and is cut to the same 200px
    // dock. Its width is its own columns added up (lay::samplerPanelW) -- a
    // third of it is the waveform, which is the one control on it that gets
    // wider the more room it is given.
    const f32 smpW = lay::samplerPanelW * s;
    // A rack that is open puts its inside into the strip, immediately after the
    // box it belongs to. Resolved before the layout because it widens it.
    RackControl* openRc = openRack();
    int openIdx = -1;
    if (openRc)
        for (size_t i = 0; i < devices.size(); ++i)
            if (devices[i].uid == rackOpenUid_) { openIdx = (int)i; break; }
    if (openIdx < 0) openRc = nullptr;
    const int specIdx = spectraOpenIdx(devices);
    const int smpIdx  = samplerOpenIdx(devices);

    const f32 total = devices.size() * (boxW + gap) + (openRc ? rackW + gap : 0.f)
                    + (specIdx >= 0 ? specW + gap : 0.f)
                    + (smpIdx  >= 0 ? smpW  + gap : 0.f) + 6 * s;
    const f32 maxScroll = std::max(0.f, total - area.w);
    stripScroll_ = clampv(stripScroll_, 0.f, maxScroll);
    bool wheelUsed = false;
    // ...and a second, weaker claim on the notch. wheelUsed means "a control
    // took it"; this means "this notch may not MOVE the surface", which is a
    // different sentence and the only one with a Shift escape.
    bool panelWheelHold_ = false;

    // Does the drag in flight want THIS chain? A card belongs to the chain it
    // was picked up on -- dragging a device off one track and onto another is a
    // different feature with a different undo story -- while a plugin from the
    // browser is happy anywhere.
    const bool ptrInArea = ui_.hovered(area);
    const bool dragWants = devDrag.armed && ptrInArea &&
                           (devDrag.kind == DevDrag::Kind::Plugin ||
                            (devDrag.kind == DevDrag::Kind::Card && devDrag.owner == devOwner_));
    f32  caretX   = 0.f;
    bool overPanel = false;      // an open editor swallows the drop; see below

    f32 x = area.x + 6 * s - stripScroll_;
    for (size_t i = 0; i < devices.size(); ++i) {
        DeviceModel& d = devices[i];
        Rect box{x, area.y + 4 * s, boxW, area.h - 9 * s};
        x += boxW + gap;
        Rect rackBox{x, box.y, rackW, box.h};
        if ((int)i == openIdx) x += rackW + gap;
        Rect specBox{x, box.y, specW, box.h};
        if ((int)i == specIdx) {
            x += specW + gap;
            // A panel four device boxes wide that opens off the right edge of
            // the strip has not opened as far as the user is concerned. One
            // scroll, on the frame after it appears (the geometry is only known
            // here), brings its left edge to the left edge of the strip.
            if (spectraScrollTo_) {
                spectraScrollTo_ = false;
                stripScroll_ = clampv(stripScroll_ + (specBox.x - area.x - 4 * s),
                                      0.f, maxScroll);
            }
        }
        Rect smpBox{x, box.y, smpW, box.h};
        if ((int)i == smpIdx) {
            x += smpW + gap;
            if (samplerScrollTo_) {               // the same one scroll, same reason
                samplerScrollTo_ = false;
                stripScroll_ = clampv(stripScroll_ + (smpBox.x - area.x - 4 * s),
                                      0.f, maxScroll);
            }
        }
        // WHERE A DROP WOULD LAND, decided against the card's MIDPOINT: left of
        // it means before this card. Resolved here rather than in the visible
        // half of the loop, so a chain scrolled halfway off the strip still
        // answers for the cards that are off screen.
        if (dragWants && devDrag.insertAt < 0 && in.mx < box.cx()) {
            devDrag.insertAt = (int)i;
            caretX = box.x - gap * 0.5f;
        }
        // An OPEN editor is not a gap in the chain. A drop that landed on the
        // rack panel would otherwise insert into the top-level chain at
        // whatever slot the panel happens to sit over, which is the one place
        // the caret would be lying about where the device goes.
        if (((int)i == openIdx && rackBox.contains(in.mx, in.my)) ||
            ((int)i == specIdx && specBox.contains(in.mx, in.my)) ||
            ((int)i == smpIdx  && smpBox.contains(in.mx, in.my)))
            overPanel = true;

        // The panel is drawn at the bottom of this iteration, so an off-screen
        // device box must not skip it.
        const bool boxVisible = !(box.right() < area.x || box.x > area.right());
        const bool rackVisible = (int)i == openIdx &&
                                 !(rackBox.right() < area.x || rackBox.x > area.right());
        const bool specVisible = (int)i == specIdx &&
                                 !(specBox.right() < area.x || specBox.x > area.right());
        const bool smpVisible = (int)i == smpIdx &&
                                !(smpBox.right() < area.x || smpBox.x > area.right());
        if (!boxVisible && !rackVisible && !specVisible && !smpVisible) continue;
        // Drawn at the END of this device's iteration, whichever way the body
        // leaves it, so the panel's own widgets take `hot` back from the box
        // beside them -- last setHot() of the frame wins, as everywhere here.
        // A device is a rack, a Spectra or a Sampler and never two of them, so
        // the three are mutually exclusive in practice and the layout does not
        // have to care.
        const auto panel = [&] {
            if (rackVisible) drawRackPanel(rackBox, *openRc, tc);
            // For the wheel rule at the bottom of this function: the pointer is
            // inside a surface that is an INSTRUMENT, not a list. A 60px jump
            // under a pointer aimed at a knob is the difference between a wheel
            // that adjusts the knob and one that moves the knob away.
            if (specVisible && specBox.contains(in.mx, in.my)) panelWheelHold_ = true;
            if (smpVisible  && smpBox.contains(in.mx, in.my))  panelWheelHold_ = true;
            if (specVisible) drawSpectraPanel(specBox, d, tc);
            if (smpVisible)  drawSamplerPanel(smpBox, d, tc);
        };
        if (!boxVisible) { panel(); continue; }

        const bool sel = (int)i == selDevice_;
        // Claim hot for the whole box first so the controls drawn afterwards
        // can take it back — last setHot() of the frame wins.
        const u64 bid = uiId(11, (int)i, 2);
        if (i == 0) devRect("card.box", box);
        const bool hotBox = ui_.setHot(bid, box) && ui_.isHot(bid);
        // A device is a card: --glass-1, a 1px lit edge, --radius-sm. Faked,
        // like every card in the system. §5's disabled rule does the bypass:
        // a bypassed device is not doing anything, and it says so at 40%.
        // The card in the air recedes WHOLE -- name, chip, knobs and all --
        // rather than only losing some of its glass. The first cut dimmed the
        // fill alone and a screenshot of the gesture could not tell the dragged
        // card from its neighbours: a --glass-1 fill at 35% over this
        // background is a difference of about four levels, and the text on top
        // of it was still at full strength. §5's disabled weight is the
        // vocabulary the strip already has for "this is not participating", so
        // the drag borrows it instead of inventing a second one.
        const bool dragSrc = devDrag.armed && devDrag.kind == DevDrag::Kind::Card &&
                             devDrag.owner == devOwner_ && (int)i == devDrag.from;
        const f32 dim = (d.bypass ? 0.4f : 1.f) * (dragSrc ? 0.45f : 1.f);
        const f32 rad = nx::radiusSm * s;
        // A sampler card is a DROP TARGET for a browser file -- the missing
        // half of the instrument: v0.5.0 shipped a sampler that state strings
        // could feed and a user could not. The drag machinery already exists
        // for slots and the timeline; this is the same gesture landing on the
        // device that plays the file chromatically instead of the slot that
        // plays it as a clip.
        SamplerControl* smp = d.inst ? d.inst->sampler() : nullptr;
        const bool fileDragHere = smp && drag_.kind == DragState::Kind::BrowserFile &&
                                  drag_.armed && box.contains(in.mx, in.my);
        // An immediate-mode strip has nothing to lift off the surface, so the
        // card in the air stays where it LIVES and goes quiet; the caret in the
        // gap says where it lands. Together those are what a floating card
        // would have said, for two multiplications.
        rend_.gradRect(box, rad, nx::glass1, (sel ? 1.f : 0.8f) * (d.bypass ? 0.55f : 1.f)
                                             * (dragSrc ? 0.4f : 1.f));
        if (fileDragHere) {
            // The drop affordance is the lit edge arriving early, plus the
            // add badge -- the same two words every other drop target says.
            rend_.gradStroke(box, rad, s, nx::edgeLit, 1.f);
            ui_.badge = Badge::Add;
            ui_.tip = "Drop to load into the sampler";
            if (in.released[0]) {
                undoPoint("load sample");
                if (smp->loadFile(drag_.path)) {
                    status_ = "Loaded " + drag_.path.substr(drag_.path.rfind('/') + 1);
                } else {
                    status_ = "Could not load " + drag_.path + " - the sampler is unchanged";
                }
                drag_ = DragState{};
            }
        }
        if (sel && !dragSrc) {
            rend_.gradStroke(box, rad, s, nx::edgeLit, 0.9f);
            rend_.roundRectOutline(box, rad, std::max(1.f, nx::snapPx(s)),
                                   nx::violet.alpha(0.75f));
        } else {
            rend_.gradStroke(box, rad, s, nx::edge, 0.85f * dim);
        }

        // WHICH WAY THE SIGNAL GOES. Left to right is only obvious to somebody
        // who already knows; a chain of five cards in a row says nothing about
        // whether the reverb is before or after the compressor unless the
        // reading order is stated. One chevron in each gap states it, at a
        // quarter of muted -- the quietest mark in the strip, and meant to be
        // read only when the eye is already asking the question.
        if (i + 1 < devices.size()) {
            const f32 gx = box.right() + gap * 0.5f, gy = box.cy(), k = 1.6f * s;
            const Col fc = nx::muted.alpha(0.25f);
            rend_.line(gx - k, gy - k * 1.4f, gx + k, gy, 1.f * s, fc);
            rend_.line(gx + k, gy, gx - k, gy + k * 1.4f, 1.f * s, fc);
        }

        // 20, not 16. THE CARD'S CONTROLS WERE UNDER THE FLOOR: a 16px title
        // bar can only hold a 12px button, and a 14x12 remove button is a
        // target the pass measured at 12 device pixels on its short side --
        // four under docs/DESIGN.md's own minimum at DPI 1.0, where most of
        // this program's users are. Ui::grab() was the cheap fix and the wrong
        // one here: the three segments sit shoulder to shoulder, so 3px of
        // slop on each makes the destructive one steal six pixels of the
        // bypass beside it (last setHot of the frame wins, and remove is
        // drawn last). Four pixels of title bar buys all three of them a real
        // 16px edge with no overlap at all, and the body below still fits its
        // three rows of knobs.
        Rect title{box.x, box.y, box.w, 20 * s};
        rend_.rect({title.x + 3 * s, title.y + 5 * s, std::max(1.f, nx::snapPx(3 * s)),
                    title.h - 10 * s}, tc.alpha(dim));

        // Both controls are glyph-drawn rather than lettered: at this size the
        // font ellipsises anything longer than a character or two.
        Rect xr{title.right() - 17 * s, title.y + 2 * s, 16 * s, 16 * s};
        Rect br{xr.x - 20 * s, title.y + 2 * s, 20 * s, 16 * s};

        // "This device has an inside." Only a rack answers rack() non-null, so
        // this is the whole test -- and it is a virtual call and not a
        // dynamic_cast precisely so a future CLAP container could answer it too
        // (host.h). The panel opens beside the box rather than inside it: eight
        // devices and eight macro mappings do not fit in 150 logical pixels,
        // and Live opens a rack alongside its chain for the same reason.
        const bool isRack = d.inst && d.inst->rack();
        // "This device has a panel of its own." A rack answers rack(); Spectra
        // answers its URI, because an editor is not a container and inventing a
        // second virtual for one device would be the larger change. When
        // another instrument grows one, this is the line that becomes a lookup.
        const bool isSpec = isSpectra(d.inst.get());
        // ...and the Sampler, which answers a VIRTUAL rather than a URI: `smp`
        // is already resolved above for the drop target, and "this device plays
        // a file" is the same kind of question as "this device has an inside".
        // Two instruments now have editors, so the next one is the one that
        // turns these three lines into a lookup rather than a chain of elses.
        const bool isSmp = smp != nullptr;
        // 38, not 34: "CHAIN" at §5's 0.12em tracking is five glyphs and four
        // gaps, and 34 left it with nothing on either side. The box is 150 wide
        // and the name beside it is a micro-label that already fits itself, so
        // the four pixels come out of slack rather than out of the name.
        Rect kr{br.x, title.y, 0, title.h};
        if (isRack)                 kr = Rect{br.x - 38 * s, title.y + 2 * s, 38 * s, 16 * s};
        else if (isSpec || isSmp)   kr = Rect{br.x - 32 * s, title.y + 2 * s, 32 * s, 16 * s};
        const bool hasPanel = isRack || isSpec || isSmp;

        // The card's controls are ONE cluster, not two or three little capsules
        // adrift in the title bar: chain or edit (only where there is one),
        // bypass, remove. The plate and the lit edge belong to the group; the
        // segments are seams.
        const Rect ctrls{hasPanel ? kr.x : br.x, br.y,
                         xr.right() - (hasPanel ? kr.x : br.x), br.h};
        if (i == 0) { devRect("card.chip", kr); devRect("card.bypass", br);
                      devRect("card.remove", xr); }
        ui_.segCluster(ctrls);
        // ONE seam per boundary, and a seam at EVERY boundary. kr.right() and
        // br.x are the same coordinate by construction, so the old pair drew the
        // chain/edit seam twice -- a hairline at double alpha, brighter than
        // every other seam in the strip -- while the bypass/remove boundary at
        // xr.x had none at all. Three segments, two seams; a cluster whose
        // dividers do not match is the "misaligned by a pixel that reads as
        // sloppiness" case, at the one place two of them sit side by side.
        if (hasPanel) rend_.hairlineV(br.x, ctrls.y + 2 * s, ctrls.bottom() - 2 * s);
        rend_.hairlineV(xr.x, ctrls.y + 2 * s, ctrls.bottom() - 2 * s);

        if (isRack) {
            const bool open = (int)i == openIdx;
            // Lettered rather than glyphed, unlike its two neighbours: bypass
            // and remove are universal, "there is a chain in here" is not, and
            // the word is the only thing that says so without being clicked.
            const bool tog = ui_.segButton(uiId(11, (int)i, 3), kr, open, nx::violet);
            // microFIT, like the Spectra chip beside it. Plain microIn draws
            // glyph by glyph with §5's tracking and no idea how wide its box is,
            // so "CHAIN" ran flush into both borders of its 34px cell -- the
            // letters touching the seam, with nothing between them and the
            // enable dot. Fit-with-a-pad is what the "edit" chip already did,
            // and two chips a pixel apart should not be drawn two ways.
            microFit(ui_, fSmall_, ui_.lastRect, "chain",
                     open ? nx::text : nx::muted, Align::Center, 2 * s);
            if (tog) {
                if (open) { rackOpenUid_ = 0; rackPath_.clear(); }
                else {
                    rackOpenUid_ = d.uid;
                    rackPath_.clear();
                    rackChainScroll = 0.f;
                    rackSel_ = -1;
                    rackTgtDev_ = rackTgtParam_ = 0;
                    rackRangeHeld_ = false;
                    selDevice_ = (int)i;
                    paramScroll_ = 0.f;
                }
                panel();
                rend_.popClip();
                return;                   // the strip's layout just changed width
            }
            if (ui_.hovered(kr))
                ui_.tip = open ? "Close the rack" : "Open this rack: its chain and its macro mappings";
        } else if (isSpec) {
            // The rack's "chain" chip, for an instrument: lettered, because
            // "this device has an editor" is not a universal glyph either.
            const bool open = (int)i == specIdx;
            if (ui_.segButton(uiId(11, (int)i, 4), kr, open, nx::violet)) {
                if (open) { spectraOpenUid_ = 0; spectraForced_ = false; }
                else {
                    spectraOpenUid_ = d.uid;
                    spectraForced_  = false;
                    spectraScrollTo_ = true;
                    selDevice_ = (int)i;
                    paramScroll_ = 0.f;
                }
                panel();
                rend_.popClip();
                return;                   // the strip's layout just changed width
            }
            microFit(ui_, fSmall_, ui_.lastRect, "edit",
                     open ? nx::text : nx::muted, Align::Center, 2 * s);
            if (ui_.hovered(kr))
                ui_.tip = open ? "Close the Spectra panel"
                               : "Open Spectra's editor: the wavetable, the oscillators, "
                                 "the filter, the envelopes and the LFO";
        } else if (isSmp) {
            // The same chip, the same word, the same width: two instruments
            // with editors must not have two different ways of being opened.
            const bool open = (int)i == smpIdx;
            if (ui_.segButton(uiId(11, (int)i, 5), kr, open, nx::violet)) {
                if (open) { samplerOpenUid_ = 0; samplerForced_ = false; }
                else {
                    samplerOpenUid_ = d.uid;
                    samplerForced_  = false;
                    samplerScrollTo_ = true;
                    selDevice_ = (int)i;
                    paramScroll_ = 0.f;
                }
                panel();
                rend_.popClip();
                return;                   // the strip's layout just changed width
            }
            microFit(ui_, fSmall_, ui_.lastRect, "edit",
                     open ? nx::text : nx::muted, Align::Center, 2 * s);
            if (ui_.hovered(kr))
                ui_.tip = open ? "Close the Sampler panel"
                               : "Open the Sampler's editor: the waveform, the region "
                                 "handles, the loop and the filter";
        }

        // The card's title is a micro-label (§5): 10px, uppercase, wide
        // tracking. A device name is an identity, not a sentence.
        Rect nameR{title.x + 10 * s, title.y, (hasPanel ? kr.x : br.x) - title.x - 12 * s, title.h};
        // What the ENGINE made of this slot -- §12.7(3). A device the daemon
        // refused, or has not confirmed yet, is not an ordinary device that
        // happens to be silent, and drawing it as one was the lie: the card
        // sat in the strip looking healthy while the engine ran nothing.
        // rd is null in local mode, where the instance in this process IS the
        // engine's; null in daemon mode means the add has not even landed in
        // the mirror yet, which is the same not-confirmed-yet truth as !live.
        const RemoteDevice* rd = eng_.remoteDevice(d.inst.get());
        const bool remote  = eng_.remoteOpen();
        const bool refused = remote && rd && rd->failed;     // EvDeviceFailed; rd->error says why
        const bool loading = remote && !refused && (!rd || !rd->live);
        const u32  parCut  = (remote && rd && rd->live) ? rd->paramsTruncated : 0;
        // Amber name = attention (refused); a loading name sits quieter than
        // its neighbours -- §5's disabled rule, because it is not sounding yet.
        microFit(ui_, fSmall_, nameR, d.desc.name.c_str(),
                 refused ? pal::meterAmber.alpha(dim)
                         : (sel ? nx::text : nx::muted).alpha(loading ? 0.6f * dim : dim),
                 Align::Left, 0);
        if (refused && ui_.setHot(uiId(UiDeviceTip, (int)i + 900), nameR) && ui_.isHot(uiId(UiDeviceTip, (int)i + 900)))
            ui_.tip = "Engine refused this device: " + rd->error;

        // The sampler names its file. Cyan basename = a live data readout,
        // full path on hover. The empty state is a QUIET muted "no sample" --
        // not an amber banner: an empty sampler is a fact, not an alert, and
        // the first cut of this chip shouted in caps ("looks so goofy and
        // unprofessional" -- the owner, correctly). The invitation appears
        // only while a browser drag is actually in flight, where it is the
        // answer to a question being asked; copy rides the motion the same
        // way the light does.
        const f32 chipH = 11 * s;
        if (smp) {
            Rect fileR{title.x + 10 * s, title.bottom() + s, box.w - 20 * s, chipH};
            const bool fileDrag = drag_.kind == DragState::Kind::BrowserFile && drag_.armed;
            if (fileDrag) {
                rend_.textIn(fSmall_, fileR, fileDragHere ? "drop to load" : "accepts samples",
                             (fileDragHere ? nx::live : nx::muted).alpha(dim),
                             Align::Left, 0);
            } else if (smp->hasSample()) {
                const std::string& fp = smp->samplePath();
                const std::string base = fp.empty() ? "recorded take"
                                        : fp.substr(fp.rfind('/') + 1);
                rend_.textIn(fSmall_, fileR, base.c_str(),
                             nx::live.alpha(0.8f * dim), Align::Left, 0);
                if (ui_.setHot(uiId(UiDeviceTip, (int)i + 1800), fileR) &&
                    ui_.isHot(uiId(UiDeviceTip, (int)i + 1800)) && !fp.empty())
                    ui_.tip = fp;
            } else {
                rend_.textIn(fSmall_, fileR, "no sample",
                             nx::muted.alpha(0.6f * dim), Align::Left, 0);
            }
        }

        // The engine's word on the slot, said ON the card, in the band the
        // sampler's file chip already reserves (stacked under it when both
        // exist). One quiet line: the daemon's own reason for a refusal in
        // amber, or "loading..." while the add is still in flight. At rest --
        // local mode, or a device that is live and whole -- the band does not
        // exist at all.
        const bool stateBand = refused || loading;
        if (stateBand) {
            Rect stR{title.x + 10 * s, title.bottom() + s + (smp ? chipH + s : 0.f),
                     box.w - 20 * s, chipH};
            const u64 sid = uiId(UiDeviceTip, (int)i + 2700);
            if (refused) {
                microFit(ui_, fSmall_, stR,
                         rd->error.empty() ? "engine refused this device"
                                           : rd->error.c_str(),
                         pal::meterAmber.alpha(dim), Align::Left, 0);
                if (ui_.setHot(sid, stR) && ui_.isHot(sid))
                    ui_.tip = "Engine refused this device: " +
                              (rd->error.empty() ? std::string("the log says why")
                                                 : rd->error);
            } else {
                rend_.textIn(fSmall_, stR, "loading...",
                             nx::muted.alpha(0.6f * dim), Align::Left, 0);
                if (ui_.setHot(sid, stR) && ui_.isHot(sid))
                    ui_.tip = "Waiting for the engine to confirm this device";
            }
        }

        // Bypass lives on the instance, so the chain does not have to be
        // republished; setBypassed() is GUI-safe per the host contract.
        const bool wasBypass = d.bypass;
        if (ui_.segButton(uiId(11, (int)i, 0), br, d.bypass, pal::meterAmber)) {
            d.bypass = !d.bypass;
            undoPointWith("bypass", d.bypass, wasBypass);
            if (d.inst) d.inst->setBypassed(d.bypass);
        }
        // Lit cyan = this device is in the signal path; dark = bypassed. §1
        // again: cyan is the light a running thing gives off.
        rend_.circle(ui_.lastRect.cx(), ui_.lastRect.cy(), 3.5f * s,
                     d.bypass ? nx::inkOn(pal::meterAmber) : nx::live);
        const bool xHot = ui_.segButton(uiId(11, (int)i, 1), xr, false, nx::danger);
        {
            const Rect g = ui_.lastRect;
            const f32 k = 3.f * s;
            const Col xc = nx::muted;
            rend_.line(g.cx() - k, g.cy() - k, g.cx() + k, g.cy() + k, 1.2f * s, xc);
            rend_.line(g.cx() - k, g.cy() + k, g.cx() + k, g.cy() - k, 1.2f * s, xc);
        }
        if (xHot) {
            // The instance is retired with the outgoing chain, so undoing this
            // loads the plugin again and applies the parameters the snapshot
            // carries - see materializeDevices. What a plugin holds beyond its
            // parameters does not survive, which is the same trade a saved set
            // makes and is documented as such in app.h.
            undoPoint("remove device");
            removeDevice(devOwner_, (int)i);
            rend_.popClip();
            return;                       // the device list changed under us
        }
        if (hotBox && in.pressed[0]) {
            selDevice_ = (int)i;
            paramScroll_ = 0.f;
            // THE CARD IS THE HANDLE (FL's chain reorder). `hotBox` is already
            // false wherever a control owns the pointer, so this can never
            // start on the bypass button or on a knob -- and on the SELECTED
            // card it is narrowed to the title bar as well, because that card's
            // body is a parameter grid and a press that misses a knob by two
            // pixels must not turn into a reorder.
            if (!sel || title.contains(in.mx, in.my)) {
                devDrag = DevDrag{};
                devDrag.kind  = DevDrag::Kind::Card;
                devDrag.from  = (int)i;
                devDrag.owner = devOwner_;
                devDrag.x0 = in.mx;
                devDrag.y0 = in.my;
            }
        }

        // Sampler cards carry the file chip between title and knobs, and a
        // loading or refused card carries the engine-state band; the body
        // yields to both so neither is stamped over the first knob row.
        const f32 bodyTop = title.bottom() + 2 * s + (smp ? chipH + s : 0.f)
                          + (stateBand ? chipH + s : 0.f);
        Rect body{box.x + 4 * s, bodyTop, box.w - 8 * s,
                  box.bottom() - bodyTop - 4 * s};

        // -------------------------------------------------------------------
        // RIGHT-CLICK REMOVES. FL Studio's cardinal rule, and this chain did
        // not have it: removal meant finding a 14px cross in the corner of the
        // title bar. The cross stays -- it is the discoverable half, and taking
        // it away would trade one hidden gesture for another -- but the whole
        // card now answers the button an FL user reaches for first.
        //
        // THE ONE EXCEPTION, and it is a real collision rather than an
        // oversight: the parameter grid below already spends the right button
        // on MIDI learn (see the block that draws it, which explains why this
        // program has no popup menus to spend it on instead). So the card takes
        // right-click everywhere the grid is not -- title bar, name, the file
        // chip, the state band, and the whole of a collapsed card, which is
        // every card the user is not currently editing.
        //
        // Geometric rather than `hotBox`, deliberately: hot belongs to whatever
        // control the pointer is over, so a right-click ON the cross or ON the
        // bypass button would otherwise do nothing at all -- and those are two
        // of the likelier places to aim.
        const bool overCard = box.contains(in.mx, in.my) &&
                              rend_.currentClip().contains(in.mx, in.my);
        const bool overGrid = sel && d.inst && d.inst->paramCount() > 0 &&
                              body.contains(in.mx, in.my);
        if (overCard && !overGrid && in.pressed[2]) {
            undoPoint("remove device");
            removeDevice(devOwner_, (int)i);
            rend_.popClip();
            return;                       // the device list changed under us
        }
        // Said once, on the quiet half of the card, and only when nothing more
        // urgent (a refusal, a truncation, a sample path) has claimed the tip.
        if (overCard && !overGrid && ui_.tip.empty() && !devDrag.armed)
            ui_.tip = d.desc.name + " - drag to reorder, right-click to remove";
        if (!d.inst) {
            // A device restored from a set whose plugin is not installed here.
            // It holds its place and its saved values (see DeviceModel), so the
            // chain comes back intact on a machine that has the plugin.
            rend_.textIn(fSmall_, {body.x, body.y + 2 * s, body.w, 12 * s},
                         "Plugin not installed", nx::danger, Align::Left, 0);
            rend_.textIn(fSmall_, {body.x, body.y + 15 * s, body.w, 12 * s},
                         d.desc.uri.c_str(), nx::muted.alpha(0.7f), Align::Left, 0);
            panel();
            continue;
        }

        if (!sel) {
            // Unselected devices stay compact; only one chain slot is edited at
            // a time, like Live collapsing the devices you are not touching.
            // A device the wire could not carry whole says so even collapsed:
            // the params line goes amber and counts what is out of reach.
            char buf[64];
            // WHICH device the header's total came from. A latency of zero is
            // the common case and says nothing; a device that delays the chain
            // is the one fact about it worth a line, and until now the only
            // place it appeared was the engine's own startup log.
            const int lat = d.bypass ? 0 : d.inst->latencyFrames();
            char latB[24] = "";
            if (lat > 0)
                snprintf(latB, sizeof latB, " - %.1f ms", 1000.0 * (f64)lat /
                         (f64)std::max(1, (int)eng_.sampleRate()));
            if (parCut)
                snprintf(buf, sizeof buf, "%d params - %u out of reach",
                         d.inst->paramCount(), parCut);
            else
                snprintf(buf, sizeof buf, "%d params%s", d.inst->paramCount(), latB);
            if (!d.desc.vendor.empty())
                rend_.textIn(fSmall_, {body.x, body.y + 2 * s, body.w, 12 * s},
                             d.desc.vendor.c_str(), nx::muted.alpha(0.7f * dim),
                             Align::Left, 0);
            rend_.textIn(fSmall_, {body.x, body.y + 15 * s, body.w, 12 * s}, buf,
                         parCut ? pal::meterAmber.alpha(0.8f * dim)
                                : nx::muted.alpha(0.7f * dim), Align::Left, 0);
            panel();
            continue;
        }

        // --- parameters of the selected device ---
        const int n = d.inst->paramCount();
        const int cols = 3;
        // 43px is knob (32) + label (11): three rows land exactly inside the
        // panel, so a device with nine or fewer controls never has to scroll.
        const f32 cw = body.w / (f32)cols, chh = 43 * s;
        const int rows = (n + cols - 1) / cols;
        // A truncated device's grid ends with §1.6's sentence, so the scroll
        // range grows by the line that carries it.
        const f32 pMax = std::max(0.f, rows * chh + (parCut ? 13 * s : 0.f) - body.h);
        // THE WHEEL BELONGS TO THE CONTROL UNDER THE POINTER FIRST (FL, and
        // every other DAW): a notch over a knob moves that knob, and only a
        // notch over the space between knobs scrolls the grid. This grid used
        // to take the notch before the knobs were even drawn, so a knob that
        // grew wheel support in the widget layer would have been fighting the
        // grid for every notch -- both would have moved, on the same gesture.
        //
        // Resolved by ORDER rather than by geometry, which is exactly what
        // uw-WIDGET-API §0 asks of a scrolling surface: every widget that acts
        // on a notch ZEROES in->wheel, so a surface that reads the wheel AFTER
        // its controls have drawn is left with the notches none of them wanted.
        // This grid read it three hundred lines too early and was one of the
        // two sites the widget pass filed against; the read now happens at the
        // bottom of the loop, and one frame of scroll lag on a list is a price
        // nobody can see.
        paramScroll_ = clampv(paramScroll_, 0.f, pMax);

        rend_.pushClip(body);
        if (n == 0)
            rend_.textIn(fSmall_, body, "No parameters", nx::muted, Align::Center);
        for (int p = 0; p < n; ++p) {
            Rect cell{body.x + (p % cols) * cw, body.y - paramScroll_ + (p / cols) * chh, cw, chh};
            if (cell.bottom() < body.y || cell.y > body.bottom()) continue;
            const ParamInfo& info = d.inst->paramInfo(p);
            Rect lbl{cell.x, cell.bottom() - 11 * s, cell.w, 10 * s};

            // Both controls edit a copy and hand the result to the instance, so
            // the value the snapshot reads (serializeDevices asks the instance)
            // is still the old one when the entry is taken. A knob drag
            // coalesces on the widget's id, as everywhere else.
            // Both also report the move to autoCapture (docs/AUTOMATION.md
            // §5.1). Unconditionally, and only for a TRACK chain: a clip
            // envelope may only automate its own track's devices (§4.2 step 2,
            // decision #2), so a return's or the master's knob has no clip to
            // record into and no address a lane could name. Everything else —
            // the arm, the transport, which clip is playing, the thinning —
            // is autoCapture's decision, kept in one place on purpose. The
            // value is the plugin's own units (§2.3), and the knob's id is the
            // gesture, so one drag is one pass and one undo entry.
            const u64 wid = uiId(12, (int)i * 256 + p, 0);
            const bool ownTrack = ownIsTrack(devOwner_);
            // Hoisted out of the two branches because the MIDI-learn affordance
            // below has to decorate whichever control this parameter got.
            const Rect tg{cell.cx() - 11 * s, cell.y + 8 * s, 22 * s, 14 * s};
            const Rect kr{cell.cx() - 16 * s, cell.y + 2 * s, 32 * s, 32 * s};
            const Rect ctrlR = info.isBool ? tg : kr;
            if (p == 0) { devRect("param.cell", cell);
                          devRect(info.isBool ? "param.toggle" : "param.knob", ctrlR,
                                  info.isBool ? 2.f * s : 0.f); }
            if (info.isBool) {
                bool on = d.inst->getParam(p) > 0.5f;
                if (ui_.grab(2.f * s).squareToggle(wid, tg, "", &on, nx::violet)) {
                    undoPoint(info.name.c_str());
                    const f32 nv = on ? info.max : info.min;
                    d.inst->setParam(p, nv);
                    if (ownTrack)
                        autoCapture(addr::deviceParam(ses_.tracks[devOwner_].uid, d.uid, info.id),
                                    nv, wid);
                }
            } else {
                f32 v = d.inst->getParam(p);
                // THE CONTROL MENU (uw-WIDGET-API §2). Right-click on a knob is
                // the widget layer's now -- Reset and Type in value are its
                // own, and it consumes the right press -- so this grid asks it
                // to carry the one item that is THIS surface's: Learn MIDI CC.
                // Offered only on a track's chain, because that is the only
                // scope the address grammar has a spelling for; on a return or
                // the master the menu simply has two items instead of three,
                // which is a better answer than a third item that refuses.
                if (ownTrack) ui_.offer({Ui::MenuLearn});
                if (ui_.knob(wid, kr, &v, info.min, info.max,
                             info.def, info.isInt ? "%.0f" : "%.2f")) {
                    undoPoint(info.name.c_str());
                    d.inst->setParam(p, v);
                    if (ownTrack)
                        autoCapture(addr::deviceParam(ses_.tracks[devOwner_].uid, d.uid, info.id),
                                    v, wid);
                }
                // Read immediately after the control, which is the contract:
                // true on exactly the one frame the row is picked, on a frame
                // where the menu is already closed and the pointer unshielded.
                if (ownTrack && ui_.menuFired(wid, Ui::MenuLearn))
                    cycleMidiLearn(addr::deviceParam(ses_.tracks[devOwner_].uid,
                                                     d.uid, info.id));
            }

            // --- MIDI learn ------------------------------------------------
            // THE STATE, DRAWN. The cycle -- unmapped -> learning -> mapped ->
            // unmapped -- is unchanged; what moved is how it is ENTERED.
            //
            // It used to be a bare right-click on the cell, and the comment
            // here defended that at length on the grounds that the program had
            // no popup machinery to spend the button on instead. It has now:
            // the widget layer grew a control menu in this same pass, every
            // knob in the program answers the right button with it, and a knob
            // in THIS grid that answered the same button with something else
            // would be the one control in NxTakt where right-click means two
            // things. Learn is a row in that menu (see the offer() above); the
            // cycle is what the row calls.
            //
            // A BOOL PARAMETER STILL CYCLES ON A BARE RIGHT-CLICK, and that is
            // not an oversight: squareToggle has no menu -- a checkbox has no
            // default to reset to and no number to type -- so it does not
            // consume the right press and there is nothing here to defer to.
            //
            // Only a TRACK's devices can be mapped: the address grammar has no
            // return or master scope, so a return's knob has no address to bind
            // and says so rather than doing nothing.
            const bool overCell = ui_.hovered(cell) && rend_.currentClip().contains(in.mx, in.my);
            // Costs a string only when there is something to say: no bindings,
            // nothing listening and no pointer here means no address is built.
            // `learning()` joined the test because the menu parks the pointer
            // while it is open and the user's hand leaves the knob to reach a
            // hardware fader -- the pulsing ring has to survive both, and under
            // the old condition an empty map made it vanish the moment the
            // pointer moved off the cell it was armed on.
            if (ownTrack && (midiMap_.size() || midiMap_.learning() || overCell)) {
                const std::string pa =
                    addr::deviceParam(ses_.tracks[devOwner_].uid, d.uid, info.id);
                const bool learning = midiMap_.learningFor(pa);
                const bool bound = midiMap_.findAddress(pa) >= 0;
                if (learning) {
                    // Accent purple, pulsing — the same light the MAP chip in
                    // the control bar wears while it is armed.
                    // Lit, not pulsing, under reduced motion (§6): "listening"
                    // is said by the ring being there at all.
                    const f32 pulse = nx::reducedMotion()
                        ? 1.f : 0.5f + 0.5f * (f32)std::sin(nowSeconds() * 6.2831853 * 1.6);
                    rend_.roundRectOutline(ctrlR.insetXY(-3 * s, -3 * s), 5 * s, 1.5f * s,
                                           nx::violetSoft.alpha(0.30f + 0.70f * pulse));
                } else if (bound) {
                    rend_.circle(ctrlR.right() + 1 * s, ctrlR.y + 1 * s, 2.2f * s, nx::violetSoft);
                }
                if (overCell) {
                    ui_.tip = learning ? "MIDI learn: move a control on your surface "
                                         "(right-click the knob again to cancel)"
                            : bound    ? "MIDI-mapped - right-click the knob to clear it"
                                       : "Right-click the knob for Reset, Type in "
                                         "value and Learn MIDI CC";
                    // The knob's own menu carries Learn; a BOOL has no menu, so
                    // its cell keeps the bare right-click that always worked.
                    if (info.isBool && in.pressed[2]) cycleMidiLearn(pa);
                }
            } else if (!ownTrack && overCell && in.pressed[2]) {
                status_ = "Only a track's devices can be MIDI-mapped - the address "
                          "space has no return or master scope";
            }

            // A rack's parameters ARE its macros, so they wear the brand: §1,
            // violet is identity, and these eight knobs are the rack's face.
            rend_.textIn(fSmall_, lbl, info.name.c_str(),
                         (isRack ? nx::violetSoft : nx::muted).alpha(dim), Align::Center, 0);
        }
        if (parCut) {
            // §1.6's sentence, drawn where the unreachable region begins: the
            // wire mirrors ipc::kMaxDevParams controls and this device has
            // more. The knobs past the cut are the LAST parCut of the grid --
            // they still edit the local instance, but in daemon mode the
            // engine cannot hear them move, and a grid that drew them like
            // their neighbours would be the same lie as a silently short list.
            char cut[80];
            snprintf(cut, sizeof cut,
                     "...and %u more controls this build cannot reach", parCut);
            const Rect cutR{body.x, body.y - paramScroll_ + rows * chh,
                            body.w, 12 * s};
            rend_.textIn(fSmall_, cutR, cut, pal::meterAmber.alpha(0.9f),
                         Align::Center, 0);
            // The sentence is wider than a 150px card, so textIn cuts it --
            // §11: anything truncated says itself in full on hover.
            if (ui_.hovered(cutR) && rend_.currentClip().contains(in.mx, in.my))
                ui_.tip = cut;
        }
        if (ui_.hovered(body)) {
            // Claimed whether or not there is a notch left: a pointer inside a
            // parameter grid is not pointing at the strip, and the strip must
            // not slide sideways because a knob has just eaten the notch.
            wheelUsed = true;
            if (in.wheel != 0.f)
                paramScroll_ = clampv(paramScroll_ - in.wheel * chh * 0.5f, 0.f, pMax);
        }
        rend_.popClip();
        panel();
    }

    // -----------------------------------------------------------------------
    // THE CARET, AND THE DROP
    //
    // After the loop and not inside it, because both drops mutate `devices` and
    // a DeviceModel& taken in the loop would be dangling the moment the vector
    // rotated or grew. Every branch that changes anything returns.
    // -----------------------------------------------------------------------
    if (dragWants && !overPanel) {
        // Past the last card's midpoint: the drop appends.
        if (devDrag.insertAt < 0) {
            devDrag.insertAt = (int)devices.size();
            caretX = x - gap * 0.5f;
        }
        // §1: violet is the thing you SET, and the slot this device is about to
        // take is exactly that. It exists for the length of a gesture and not
        // one frame longer -- the owner's verdict on a drop target that
        // advertises itself at rest was "goofy and unprofessional", and it was
        // the right one.
        rend_.rect({nx::snapPx(caretX - 1 * s), area.y + 2 * s,
                    std::max(2.f, nx::snapPx(2 * s)), area.h - 5 * s}, nx::violet);
        ui_.cursor = Cursor::Grab;
        // A plugin drop MAKES a device, so it takes the add badge; a card being
        // reordered is a thing already in the user's hand, and §5's badge rule
        // is explicit that those get nothing.
        if (devDrag.kind == DevDrag::Kind::Plugin) ui_.badge = Badge::Add;

        if (in.released[0]) {
            const int at = clampv(devDrag.insertAt, 0, (int)devices.size());
            const DevDrag::Kind kind = devDrag.kind;
            const int from = devDrag.from;
            const PluginDesc desc = devDrag.desc;
            devDrag = DevDrag{};

            if (kind == DevDrag::Kind::Card && from >= 0 && from < (int)devices.size()) {
                // The slot it LANDS in, once its own removal has closed the gap
                // behind it. Dropping either side of the card you are holding
                // is a no-op, which is what makes a mis-aimed drag harmless.
                int to = at > from ? at - 1 : at;
                to = clampv(to, 0, (int)devices.size() - 1);
                if (to != from) {
                    undoPoint("reorder devices");
                    DeviceModel moved = std::move(devices[from]);
                    devices.erase(devices.begin() + from);
                    devices.insert(devices.begin() + to, std::move(moved));
                    const RtChain* before = *co.published;
                    publishChain(devOwner_);
                    if (*co.published == before) {
                        // The ring was full, so the engine still runs the old
                        // ORDER. Put the model back where the engine thinks it
                        // is: a strip that draws an order the engine is not
                        // playing is the same lie the refusal tags exist to
                        // stop. Nothing was freed and nothing was published, so
                        // this is a pure rotation back.
                        DeviceModel back = std::move(devices[to]);
                        devices.erase(devices.begin() + to);
                        devices.insert(devices.begin() + from, std::move(back));
                        status_ = "Engine busy - devices not reordered";
                    } else {
                        selDevice_ = to;
                        char m[96];
                        snprintf(m, sizeof m, "Moved %s to slot %d",
                                 devices[to].desc.name.c_str(), to + 1);
                        status_ = m;
                    }
                }
            } else if (kind == DevDrag::Kind::Plugin) {
                undoPoint("add device");
                const size_t was = devices.size();
                addDevice(devOwner_, desc);          // appends, and says why not
                if (devices.size() > was && at < (int)devices.size() - 1) {
                    // addDevice() has already published the chain with the new
                    // device on the END; this is the second publish that puts
                    // it where the caret was. Two publishes rather than a
                    // bespoke insert, so the load-and-refuse path stays the one
                    // audited path it has always been.
                    DeviceModel moved = std::move(devices.back());
                    devices.pop_back();
                    devices.insert(devices.begin() + at, std::move(moved));
                    const RtChain* before = *co.published;
                    publishChain(devOwner_);
                    if (*co.published == before) {
                        DeviceModel back = std::move(devices[at]);
                        devices.erase(devices.begin() + at);
                        devices.push_back(std::move(back));
                        status_ = "Engine busy - added at the end instead";
                    } else {
                        selDevice_ = at;
                    }
                }
            }
            rend_.popClip();
            return;
        }
    }
    // Released anywhere that does not take a drop: nothing happens, quietly.
    if (!in.down[0] && devDrag.kind != DevDrag::Kind::None) devDrag = DevDrag{};

    // The strip scrolls horizontally on a plain wheel, unless the pointer was
    // over a parameter grid that wanted the notch for itself.
    if (!wheelUsed && (!panelWheelHold_ || in.shift()) &&
        maxScroll > 0.f && ui_.hovered(area) && in.wheel != 0.f)
            stripScroll_ = clampv(stripScroll_ - in.wheel * 60.f * s, 0.f, maxScroll);

    rend_.popClip();
}


// ---------------------------------------------------------------------------
// the rack panel
//
// Drawn in the device strip, immediately to the right of the rack it belongs
// to, in the same box-and-title-bar language every device already wears -- one
// wider box with an accent outline, because it IS the inside of the box beside
// it. Two halves: what the rack contains on the left, what its macros do on the
// right.
//
// Everything structural on the left goes through rackChainEdited(), which is
// the §1 obligation; the mapping editor on the right never does, because a
// mapping changes no device's latency.
// ---------------------------------------------------------------------------
void App::drawRackPanel(const Rect& box, RackControl& rc, const Col& tc) {
    const f32 s = win_.dpiScale();
    Input& in = win_.input();

    // The rack panel IS the inside of the box beside it, so it is the one card
    // in the strip that carries the lit-violet edge and an elevation: it is
    // open, and open is a state worth seeing from across the screen.
    const f32 rad = nx::radiusSm * s;
    rend_.shadow(box, rad, nx::shadow);
    rend_.gradRect(box, rad, nx::glass1);
    rend_.gradStroke(box, rad, s, nx::edgeLit, 1.f);
    rend_.roundRectOutline(box, rad, std::max(1.f, nx::snapPx(s)), nx::violet.alpha(0.55f));

    // A plugin dragged out of the browser and released ON this panel lands
    // INSIDE the rack -- which is exactly what the strip's header already
    // promises a double-click does while a rack is open, said by the other
    // gesture. The strip suppresses its own insertion caret over this box for
    // this reason and no other.
    if (devDrag.kind == DevDrag::Kind::Plugin && devDrag.armed &&
        box.contains(in.mx, in.my)) {
        rend_.roundRectOutline(box, rad, std::max(1.f, nx::snapPx(1.5f * s)), nx::violet);
        ui_.badge = Badge::Add;
        if (in.released[0]) {
            const PluginDesc desc = devDrag.desc;
            devDrag = DevDrag{};
            if (rc.deviceCount() >= kRackMaxDevices) {
                status_ = "The rack is full - " + desc.name + " was not added";
            } else {
                undoPoint("add device to rack");
                if (rc.addDevice(desc)) {
                    rackChainEdited();
                    rackSel_ = rc.deviceCount() - 1;
                    status_ = "Added " + desc.name + " to the rack";
                } else {
                    status_ = "Could not add " + desc.name + " to the rack";
                }
            }
            return;
        }
    }

    // --- title: where we are, and the way back out -------------------------
    // 20 to the card's 20. The panel is the INSIDE of the box beside it, so a
    // title bar that did not line up with that box's would read as two
    // unrelated things that happen to be adjacent -- and the same four pixels
    // buy the close and back buttons a 16px edge, which they were four and
    // four under at DPI 1.0.
    Rect title{box.x, box.y, box.w, 20 * s};
    rend_.rect({title.x + 3 * s, title.y + 5 * s, std::max(1.f, nx::snapPx(3 * s)),
                title.h - 10 * s}, tc);

    Rect closeR{title.right() - 18 * s, title.y + 2 * s, 16 * s, 16 * s};
    devRect("rack.close", closeR);
    if (ui_.button(uiId(UiRackPanel, 0, 0), closeR, "")) { rackOpenUid_ = 0; rackPath_.clear(); }
    {
        const f32 k = 3.f * s;
        rend_.line(closeR.cx() - k, closeR.cy() - k, closeR.cx() + k, closeR.cy() + k, 1.2f * s, nx::muted);
        rend_.line(closeR.cx() - k, closeR.cy() + k, closeR.cx() + k, closeR.cy() - k, 1.2f * s, nx::muted);
    }
    Rect backR{closeR.x - 20 * s, title.y + 2 * s, 18 * s, 16 * s};
    if (!rackPath_.empty()) {
        devRect("rack.back", backR);
        if (ui_.button(uiId(UiRackPanel, 0, 1), backR, "<")) { rackPath_.pop_back(); rackSel_ = -1; }
        if (ui_.hovered(backR)) ui_.tip = "Back to the rack that contains this one";
    }
    // The breadcrumb is the whole of "where am I": a rack in a rack in a rack
    // is a place, and the only thing that says so.
    std::string crumb = "RACK";
    for (size_t i = 0; i < rackPath_.size(); ++i) crumb += " / RACK";
    char head[96];
    snprintf(head, sizeof head, "%s   %d/%d", crumb.c_str(), rc.deviceCount(), kRackMaxDevices);
    microFit(ui_, fSmall_, {title.x + 10 * s, title.y, backR.x - title.x - 12 * s, title.h},
             head, nx::violetSoft, Align::Left, 0);

    const f32 colW = 156 * s;
    Rect left{box.x + 4 * s, title.bottom() + 2 * s, colW, box.bottom() - title.bottom() - 6 * s};
    Rect right{left.right() + 6 * s, left.y, box.right() - left.right() - 10 * s, left.h};
    rend_.hairlineV(left.right() + 2 * s, left.y, left.bottom());

    // -----------------------------------------------------------------------
    // left: the chain, in processing order
    //
    // THE LIST SCROLLS, and the add row is pinned under it. It did neither
    // before, and the arithmetic says what that cost: the column is about 136
    // device pixels tall at DPI 1.0, a row is 16 of them and the heading takes
    // 13, so the seventh device pushed the add row off the bottom and the
    // eighth pushed itself off -- a rack that holds kRackMaxDevices could not
    // SHOW kRackMaxDevices, and the way to add the last two was to go back to
    // the browser and double-click, which is the path the add row exists to
    // save. Pinning costs one row of list height and makes the panel's own
    // capacity legible at every size the dock can be.
    // -----------------------------------------------------------------------
    ui_.microIn(fSmall_, {left.x, left.y, left.w, 11 * s}, "CHAIN", nx::muted, Align::Left, 0);

    // 16, not 15-plus-a-gap. Same pitch to the pixel, and the row now meets
    // DESIGN.md's 16px floor on its short side instead of missing it by one.
    const f32 rowH = 16 * s;
    const int n = rc.deviceCount();
    Rect addR{left.x, left.bottom() - rowH, left.w, rowH};
    Rect listR{left.x, left.y + 13 * s, left.w,
               std::max(rowH, addR.y - 2 * s - (left.y + 13 * s))};

    const f32 chainMax = std::max(0.f, (f32)n * rowH - listR.h);
    if (ui_.hovered(listR) && in.wheel != 0.f) rackChainScroll -= in.wheel * rowH * 2.f;
    rackChainScroll = clampv(rackChainScroll, 0.f, chainMax);

    // What the row loop decided, acted on AFTER the clip is popped: every one
    // of these rebuilds the list under the loop, and an early return with a
    // clip still pushed would leave the renderer's scissor stack one deep for
    // the rest of the frame.
    enum class RowAct { None, Up, Down, Remove, Open } act = RowAct::None;
    int actIdx = -1;

    rend_.pushClip(listR);
    f32 y = listR.y - rackChainScroll;
    for (int i = 0; i < n; ++i) {
        PluginInstance* sub = rc.device(i);
        if (!sub) continue;
        Rect row{left.x, y, left.w, rowH};
        y += rowH;
        if (row.bottom() < listR.y || row.y > listR.bottom()) continue;

        const bool sel = i == rackSel_;
        if (i == 0) devRect("rack.row", row);
        const u64 rid = uiId(UiRackPanel, 1, i);
        const bool hot = ui_.setHot(rid, row) && ui_.isHot(rid);
        // Well rows with the specimen's hover treatment: nothing at rest, the
        // glass chip under the pointer, a violet marker on the selected one.
        if (sel) {
            rend_.gradRect(row, nx::radiusXs * s, nx::glassChip, 0.85f);
            rend_.rect({nx::snapPx(row.x), row.y, std::max(1.f, nx::snapPx(2 * s)), row.h},
                       nx::violet);
        } else if (hot) {
            rend_.gradRect(row, nx::radiusXs * s, nx::glassChip, 0.45f);
        }

        // Reorder and remove are one cluster of three seams, not three little
        // buttons in a row: they act on the same device and they are the only
        // controls this row has.
        Rect xr{row.right() - 14 * s, row.y + 1 * s, 12 * s, rowH - 2 * s};
        Rect dn{xr.x - 12 * s, row.y + 1 * s, 12 * s, rowH - 2 * s};
        Rect up{dn.x - 12 * s, row.y + 1 * s, 12 * s, rowH - 2 * s};
        // The plate arrives with the pointer: eight rows each wearing a
        // permanent chip would be eight competing surfaces in a 156px column.
        const Rect rowCtrls{up.x, up.y, xr.right() - up.x, up.h};
        if (i == 0) { devRect("rack.up", up, 2 * s); devRect("rack.down", dn, 2 * s);
                      devRect("rack.rowRemove", xr, 2 * s); }
        if (hot || sel) {
            ui_.segCluster(rowCtrls);
            rend_.hairlineV(dn.x, rowCtrls.y + 1 * s, rowCtrls.bottom() - 1 * s);
            rend_.hairlineV(xr.x, rowCtrls.y + 1 * s, rowCtrls.bottom() - 1 * s);
        }

        char idx[16];
        snprintf(idx, sizeof idx, "%d", i + 1);
        rend_.textIn(fSmall_, {row.x + 5 * s, row.y, 12 * s, row.h}, idx,
                     nx::muted.alpha(0.6f), Align::Left, 0);
        Rect nameR{row.x + 16 * s, row.y, up.x - row.x - 18 * s, row.h};
        // A rack inside a rack is marked, because it is the one row that has
        // somewhere to go when you double-click it.
        const bool nested = sub->rack() != nullptr;
        rend_.textIn(fSmall_, nameR, sub->desc().name.c_str(),
                     nested ? nx::violetSoft : (sel ? nx::text : nx::muted), Align::Left, 0);

        // Reorder. Two buttons rather than a drag: the chain is at most eight
        // long, the strip already scrolls under the pointer, and a drag would
        // be the only one in this panel.
        // Chevrons drawn rather than lettered, for the reason the box's own
        // bypass and remove controls are: at twelve pixels the body font
        // ellipsises even a single character.
        const auto chevron = [&](const Rect& b, bool upward, bool live) {
            const f32 k = 2.6f * s, d = upward ? -1.f : 1.f;
            // §5's disabled rule: an end-of-chain arrow is at 40%, not greyed.
            const Col c = live ? nx::muted : nx::muted.alpha(0.4f);
            rend_.line(b.cx() - k, b.cy() - k * d * 0.6f, b.cx(), b.cy() + k * d * 0.6f, 1.1f * s, c);
            rend_.line(b.cx() + k, b.cy() - k * d * 0.6f, b.cx(), b.cy() + k * d * 0.6f, 1.1f * s, c);
        };
        const bool canUp = i > 0, canDn = i + 1 < n;
        // THE AIM SLOP, and the ORDER IT IS RESOLVED IN. These three are 12x14
        // and sit shoulder to shoulder, so every one of them was under the 16px
        // floor and none of them could grow without eating a neighbour. 2px on
        // each side clears the floor on both axes; the overlap it creates is
        // resolved by testing the DESTRUCTIVE one first, because the last
        // setHot() of a frame wins -- so the arrows steal from the cross rather
        // than the cross from the arrows, and the two pixels a slip costs are
        // two pixels of "the wrong device moved" instead of "the wrong device
        // is gone".
        if (ui_.grab(2.f * s).segButton(uiId(UiRackPanel, 4, i), xr, false, nx::danger))
            { act = RowAct::Remove; actIdx = i; break; }
        {
            const f32 k = 2.5f * s;
            rend_.line(xr.cx() - k, xr.cy() - k, xr.cx() + k, xr.cy() + k, 1.1f * s, nx::muted);
            rend_.line(xr.cx() - k, xr.cy() + k, xr.cx() + k, xr.cy() - k, 1.1f * s, nx::muted);
        }
        if (ui_.grab(2.f * s).segButton(uiId(UiRackPanel, 3, i), dn, false, nx::violet) && canDn)
            { act = RowAct::Down; actIdx = i; break; }
        chevron(dn, false, canDn);
        if (ui_.grab(2.f * s).segButton(uiId(UiRackPanel, 2, i), up, false, nx::violet) && canUp)
            { act = RowAct::Up; actIdx = i; break; }
        chevron(up, true, canUp);

        if (hot && in.pressed[0]) rackSel_ = i;
        if (hot && in.dblClick && nested) { act = RowAct::Open; actIdx = i; break; }
        // RIGHT-CLICK CLEARS THE SLOT, the same verb the device card outside
        // this panel now answers -- one gesture for "get rid of this", whether
        // the chain it is in is the track's or a rack's. Geometric rather than
        // `hot`, so the button the pointer happens to be over cannot swallow it.
        if (row.contains(in.mx, in.my) && rend_.currentClip().contains(in.mx, in.my)
            && in.pressed[2]) { act = RowAct::Remove; actIdx = i; break; }
        if (hot) ui_.tip = nested
            ? "Double-click to open this rack; right-click removes it"
            : "The arrows reorder, the cross or a right-click removes";
    }
    rend_.popClip();

    if (act != RowAct::None) {
        switch (act) {
        case RowAct::Up:
            undoPoint("move device in rack");
            rc.moveDevice(actIdx, actIdx - 1);
            rackChainEdited();
            rackSel_ = actIdx - 1;
            break;
        case RowAct::Down:
            undoPoint("move device in rack");
            rc.moveDevice(actIdx, actIdx + 1);
            rackChainEdited();
            rackSel_ = actIdx + 1;
            break;
        case RowAct::Remove:
            undoPoint("remove device from rack");
            rc.removeDevice(actIdx);
            rackChainEdited();
            if (rackSel_ >= rc.deviceCount()) rackSel_ = rc.deviceCount() - 1;
            // If what went was itself an open nested rack, openRack() truncates
            // the path on the next frame -- it re-walks it every frame for
            // exactly this reason, so there is nothing to fix up here.
            status_ = "Removed from rack";
            break;
        case RowAct::Open:
            rackPath_.push_back(actIdx);
            rackSel_ = -1;
            rackChainScroll = 0.f;
            break;
        case RowAct::None: break;
        }
        return;                              // the list changed under us
    }

    // The add row. It uses the plugin browser's selection, and the browser's
    // double-click does the same thing while a rack is open -- one place to
    // pick a plugin, two ways to land it. Pinned to the bottom of the column
    // rather than laid after the last row, so it is reachable at every count.
    {
        // The SAME list the browser draws -- the daemon's catalog when it is
        // ready -- because pluginSel_ is an index into whatever the browser
        // showed. Resolving it against the local registry while the browser
        // lists the catalog would land a different plugin than the row the
        // user picked.
        const std::vector<PluginDesc>& all =
            (eng_.remoteOpen() && eng_.catalogReady()) ? eng_.catalog()
                                                       : registry_.plugins();
        const bool have = pluginSel_ >= 0 && pluginSel_ < (int)all.size();
        const bool full = n >= kRackMaxDevices;
        char label[80];
        if (full)       snprintf(label, sizeof label, "Rack is full");
        else if (have)  snprintf(label, sizeof label, "+ %s", all[pluginSel_].name.c_str());
        else            snprintf(label, sizeof label, "+ Pick a plugin on the left");
        devRect("rack.add", addR);
        if (ui_.button(uiId(UiRackPanel, 5, 0), addR, label, false, nx::violet) && have && !full) {
            undoPoint("add device to rack");
            if (rc.addDevice(all[pluginSel_])) {
                rackChainEdited();
                rackSel_ = rc.deviceCount() - 1;
                rackChainScroll = chainMax + rowH;   // clamped next frame: show it
                status_ = "Added " + all[pluginSel_].name + " to the rack";
            } else {
                status_ = "Could not add " + all[pluginSel_].name + " to the rack";
            }
            return;
        }
        // A full rack said "Rack is full" on the button and nothing when it was
        // pressed. §9: a refusal explains.
        if (full && ui_.hovered(addR))
            ui_.tip = "This rack already holds the most devices it can "
                      "(docs/RACKS.md caps it); remove one to make room";
    }

    // -----------------------------------------------------------------------
    // right: the macro mapping editor
    //
    // One sentence, laid out as one: macro <- device / parameter, over
    // min .. max. min > max is legal and inverts; both are in the TARGET's own
    // units and are read back CLAMPED, so what the list shows is what the macro
    // will really do rather than what was typed at it.
    // -----------------------------------------------------------------------
    ui_.microIn(fSmall_, {right.x, right.y, right.w, 11 * s}, "MACRO", nx::muted, Align::Left, 0);

    // Eight macros, four to a row, each row ONE segmented cluster: this is a
    // chooser -- exactly one macro is being edited -- and a chooser drawn as
    // eight separate capsules is the "buttons that don't belong together" look.
    const f32 mw = right.w / 4.f, mh = 13 * s;
    for (int rowN = 0; rowN < 2; ++rowN) {
        const f32 ry0 = right.y + 12 * s + (f32)rowN * (mh + 2 * s);
        ui_.segCluster({right.x, ry0, right.w, mh});
        for (int c = 0; c < 4; ++c) {
            const int m = rowN * 4 + c;
            Rect b{right.x + (f32)c * mw, ry0, mw, mh};
            if (!m) devRect("rack.macro", b, 1.5f * s);
            if (c) rend_.hairlineV(b.x, ry0 + 2 * s, ry0 + mh - 2 * s);
            char lbl[8];
            snprintf(lbl, sizeof lbl, "M%d", m + 1);
            // The macros are the rack's identity, so the selected one is violet.
            // THE AIM SLOP ON THE MACRO SIDE. Every control in this column is
            // 13px tall because that is what two rows of macros plus two
            // selectors plus a range row plus a mapping list add up to inside a
            // 200px dock -- three under the floor, and none of them can grow
            // without taking a row off the list that says what the macro
            // already does. 1.5px on each side clears 16 exactly. Horizontally
            // the segments are ~70px wide so the overlap is noise; vertically
            // the 2px gaps leave 1px of overlap, which resolves to the row
            // drawn later and costs a click on the wrong MACRO -- a selection,
            // never a value.
            if (ui_.grab(1.5f * s).segButton(uiId(UiRackPanel, 6, m), b, m == rackMacro_, nx::violet)) {
                rackMacro_ = m;
                rackListScroll_ = 0.f;
            }
            ui_.microIn(fSmall_, ui_.lastRect, lbl,
                        m == rackMacro_ ? nx::text : nx::muted, Align::Center);
        }
    }

    f32 ry = right.y + 12 * s + 2 * (mh + 2 * s) + 3 * s;
    if (n == 0) {
        // §5's empty state, sized for the space it has: one line that says what
        // to do next rather than what is missing.
        rend_.textIn(fSmall_, {right.x, ry, right.w, 12 * s},
                     "Put a device in the rack to map a macro to it.",
                     nx::muted, Align::Left, 0);
        return;
    }

    // The two choosers. Names are rebuilt each frame -- a dozen short strings
    // for the one rack that is open, the same order of cost as the labels
    // beside them.
    rackTgtDev_ = clampv(rackTgtDev_, 0, n - 1);
    std::vector<std::string> devNames;
    std::vector<const char*> devPtrs;
    devNames.reserve((size_t)n);
    for (int i = 0; i < n; ++i) {
        char b[80];
        snprintf(b, sizeof b, "%d  %s", i + 1, rc.device(i) ? rc.device(i)->desc().name.c_str() : "?");
        devNames.push_back(b);
    }
    for (const std::string& d : devNames) devPtrs.push_back(d.c_str());

    PluginInstance* tgt = rc.device(rackTgtDev_);
    const int pc = tgt ? tgt->paramCount() : 0;
    rackTgtParam_ = pc > 0 ? clampv(rackTgtParam_, 0, pc - 1) : 0;

    Rect devR{right.x, ry, right.w, mh};
    Rect parR{right.x, ry + mh + 2 * s, right.w, mh};
    devRect("rack.tgtDevice", devR, 1.5f * s);
    devRect("rack.tgtParam", parR, 1.5f * s);
    const int wasDev = rackTgtDev_, wasPar = rackTgtParam_;
    ui_.grab(1.5f * s).selector(uiId(UiRackPanel, 7, 0), devR, &rackTgtDev_,
                                devPtrs.data(), (int)devPtrs.size());
    if (rackTgtDev_ != wasDev) { rackTgtParam_ = 0; rackRangeHeld_ = false; }
    if (ui_.hovered(devR)) ui_.tip = "The device this macro drives (right-click steps back)";

    tgt = rc.device(rackTgtDev_);
    const int pc2 = tgt ? tgt->paramCount() : 0;
    if (pc2 == 0) {
        rend_.well(parR, nx::radiusXs * s);
        rend_.textIn(fSmall_, parR, "This device has no parameters", nx::muted, Align::Center, 0);
        return;
    }
    rackTgtParam_ = clampv(rackTgtParam_, 0, pc2 - 1);
    std::vector<std::string> parNames;
    std::vector<const char*> parPtrs;
    parNames.reserve((size_t)pc2);
    for (int i = 0; i < pc2; ++i) parNames.push_back(tgt->paramInfo(i).name);
    for (const std::string& p : parNames) parPtrs.push_back(p.c_str());
    ui_.grab(1.5f * s).selector(uiId(UiRackPanel, 8, 0), parR, &rackTgtParam_,
                                parPtrs.data(), (int)parPtrs.size());
    if (rackTgtParam_ != wasPar || rackTgtDev_ != wasDev) rackRangeHeld_ = false;
    if (ui_.hovered(parR)) ui_.tip = "The parameter this macro drives";

    const ParamInfo& info = tgt->paramInfo(rackTgtParam_);
    const f32 plo = info.min < info.max ? info.min : info.max;
    const f32 phi = info.min < info.max ? info.max : info.min;
    // Until the range is touched it FOLLOWS the target, so picking a parameter
    // proposes its full sweep -- the mapping people want nine times in ten --
    // and dialling either end in is what turns it into a partial or an inverted
    // one. Held from then on, so switching parameters to compare does not throw
    // the numbers away.
    if (!rackRangeHeld_) { rackMin_ = (f64)info.min; rackMax_ = (f64)info.max; }

    Rect minR{right.x, parR.bottom() + 2 * s, right.w * 0.34f, mh};
    Rect maxR{minR.right() + 3 * s, minR.y, right.w * 0.34f, mh};
    Rect mapR{maxR.right() + 3 * s, minR.y, right.right() - maxR.right() - 3 * s, mh};
    devRect("rack.rangeMin", minR, 1.5f * s);
    devRect("rack.rangeMax", maxR, 1.5f * s);
    devRect("rack.map", mapR, 1.5f * s);
    const char* nf = info.isInt ? "%.0f" : "%.2f";
    const f64 per = (f64)(phi - plo) / 160.0;
    if (ui_.grab(1.5f * s).dragNumber(uiId(UiRackPanel, 9, 0), minR, &rackMin_, plo, phi,
                       per, nf, Align::Center, nullptr, 0.0, /*def=*/plo)) rackRangeHeld_ = true;
    if (ui_.grab(1.5f * s).dragNumber(uiId(UiRackPanel, 9, 1), maxR, &rackMax_, plo, phi,
                       per, nf, Align::Center, nullptr, 0.0, /*def=*/phi)) rackRangeHeld_ = true;
    if (ui_.hovered(minR)) ui_.tip = "Value at macro 0, in the target's own units";
    if (ui_.hovered(maxR))
        ui_.tip = "Value at macro 1 - set it BELOW the other end to invert the macro";

    if (ui_.grab(1.5f * s).button(uiId(UiRackPanel, 10, 0), mapR, "MAP", false, nx::violet)) {
        undoPoint("map macro");
        RackMapping m;
        m.macro  = rackMacro_;
        m.device = rackTgtDev_;
        m.param  = info.id;
        m.min    = (f32)rackMin_;
        m.max    = (f32)rackMax_;
        const int at = rc.addMapping(m);
        if (at < 0) {
            status_ = "Could not map that parameter";
        } else {
            // Read back what was STORED, not what was asked for: addMapping
            // clamps each endpoint into the target's range independently, which
            // preserves inversion. Showing the stored pair is the only way the
            // editor renders what the macro will actually do.
            const RackMapping& got = rc.mapping(at);
            rackMin_ = (f64)got.min;
            rackMax_ = (f64)got.max;
            rackRangeHeld_ = true;
            char buf[128];
            snprintf(buf, sizeof buf, "Macro %d -> %s  %.2f..%.2f%s", rackMacro_ + 1,
                     info.name.c_str(), (f64)got.min, (f64)got.max,
                     got.min > got.max ? "  (inverted)" : "");
            status_ = buf;
        }
        return;
    }

    // --- what this macro already does --------------------------------------
    Rect list{right.x, minR.bottom() + 3 * s, right.w, right.bottom() - minR.bottom() - 3 * s};
    if (list.h < 12 * s) return;
    rend_.pushClip(list);

    int shown = 0;
    for (int i = 0; i < rc.mappingCount(); ++i) if (rc.mapping(i).macro == rackMacro_) ++shown;

    // "CLEAR", not "clear": it sits directly under MAP, does the opposite of
    // it, and the two were spelled two different ways -- the one place in the
    // panel where an inconsistent capitalisation is a pixel from its own
    // counter-example. Widened by 4 so the longer word keeps its padding.
    // 14 tall, not 11. This is the one control on the tab that needed more
    // than 3 device pixels of slop to clear the floor -- 2.5 logical is 3.75 at
    // DPI 1.5, past what widgets.h says a pad may be before neighbours start
    // stealing each other's hover, and the neighbour immediately below it is
    // the unmap cross, which is destructive too. Three pixels of height and
    // three of top margin on the list below cost one row of a list that
    // scrolls anyway, and buy an honest 16.
    Rect clr{list.right() - 48 * s, list.y, 48 * s, 14 * s};
    char cap[48];
    snprintf(cap, sizeof cap, "MACRO %d DRIVES %d", rackMacro_ + 1, shown);
    microFit(ui_, fSmall_, {list.x, list.y, list.w - 52 * s, 14 * s}, cap,
             nx::muted, Align::Left, 0);
    devRect("rack.clearMacro", clr, 1.f * s);
    if (shown > 0 && ui_.grab(1.f * s).button(uiId(UiRackPanel, 11, 0), clr, "CLEAR")) {
        undoPoint("clear macro");
        rc.clearMacro(rackMacro_);
        status_ = "Macro cleared";
        rend_.popClip();
        return;
    }

    // 16, not 13: the unmap cross is the row's only control and it was 11x11.
    // A pad big enough to clear the floor at 13px pitch would have made every
    // cross overlap the one below it, which on a DESTRUCTIVE control is the one
    // overlap that must not exist -- so the pitch grew instead and the pad is
    // the 1px the cross still needs.
    const f32 lrow = 16 * s;
    const f32 lmax = std::max(0.f, shown * lrow - (list.h - 16 * s));
    if (ui_.hovered(list) && in.wheel != 0.f) rackListScroll_ -= in.wheel * lrow * 2.f;
    rackListScroll_ = clampv(rackListScroll_, 0.f, lmax);

    f32 ly = list.y + 16 * s - rackListScroll_;
    for (int i = 0; i < rc.mappingCount(); ++i) {
        const RackMapping& m = rc.mapping(i);
        if (m.macro != rackMacro_) continue;
        Rect row{list.x, ly, list.w, lrow};
        ly += lrow;
        if (row.bottom() < list.y || row.y > list.bottom()) continue;

        PluginInstance* md = rc.device(m.device);
        const char* pn = "?";
        if (md) {
            for (int p = 0; p < md->paramCount(); ++p)
                if (md->paramInfo(p).id == m.param) { pn = md->paramInfo(p).name.c_str(); break; }
        }
        Rect xr{row.right() - 15 * s, row.y + 1 * s, 14 * s, lrow - 2 * s};
        if (!i) devRect("rack.unmap", xr, 1 * s);
        char line[160];
        // The arrow points the way the value moves, so an inverted mapping is
        // legible at a glance rather than by comparing two numbers.
        snprintf(line, sizeof line, "%d/%s   %.2f %s %.2f", m.device + 1, pn,
                 (f64)m.min, m.min > m.max ? "\\" : "/", (f64)m.max);
        rend_.textIn(fSmall_, {row.x, row.y, row.w - 17 * s, row.h}, line,
                     m.min > m.max ? nx::violetSoft : nx::muted, Align::Left, 0);
        if (ui_.grab(1.f * s).button(uiId(UiRackPanel, 12, i), xr, "")) {
            undoPoint("unmap macro");
            rc.removeMapping(i);
            rend_.popClip();
            return;
        }
        const f32 k = 2.2f * s;
        rend_.line(xr.cx() - k, xr.cy() - k, xr.cx() + k, xr.cy() + k, 1.f * s, nx::muted.alpha(0.7f));
        rend_.line(xr.cx() - k, xr.cy() + k, xr.cx() + k, xr.cy() - k, 1.f * s, nx::muted.alpha(0.7f));
    }
    rend_.popClip();
}

} // namespace lat
