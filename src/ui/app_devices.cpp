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
    RackControl* rc = p ? p->rack() : nullptr;
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
    // is idempotent; the browser draws a spinner off scanRunning() and swaps to
    // the catalog when it lands. The local registry still scans below as the
    // fallback -- and because addDevice() still instantiates locally, so a
    // local instance must exist for anything the user actually adds.
    if (!eng_.local() && !eng_.catalogReady()) eng_.requestScan();
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
    const std::vector<PluginDesc>& all =
        (!eng_.local() && eng_.catalogReady()) ? eng_.catalog() : registry_.plugins();

    // --- header: the §5 chip language, 10px uppercase over wide tracking ----
    Rect head{r.x + nx::sp1 * s, r.y + 4 * s, r.w - nx::sp1 * 2.f * s, 12 * s};
    ui_.microIn(fSmall_, head, "BROWSER", nx::muted, Align::Left, 0);
    {
        char cnt[24];
        snprintf(cnt, sizeof cnt, "%zu", all.size());
        ui_.microIn(fSmall_, head, cnt, nx::muted.alpha(0.6f), Align::Right, 0);
    }

    // --- filter ---
    const u64 fid = uiId(10, 0);
    Rect filter{r.x + 6 * s, head.bottom() + 4 * s, r.w - 12 * s, 17 * s};
    // The field is recessed at rest and takes the violet border and the focus
    // ring the moment the caret arrives (§5: never a bare outline). textField
    // paints the focused state itself, so this is only the resting well.
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

    const f32 rowH = 17 * s;
    Rect listR{r.x, filter.bottom() + 6 * s, r.w, r.bottom() - filter.bottom() - 6 * s};
    rend_.hairlineH(r.x + nx::sp1 * s, r.right() - nx::sp1 * s, listR.y - 3 * s);
    rend_.pushClip(listR);

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
        if (hot) ui_.cursor = Cursor::Hand;

        Rect tag{row.right() - 46 * s, row.cy() - 6 * s, 40 * s, 12 * s};
        // --radius-xs, not h*0.5: a hand-rolled capsule is exactly the
        // roundness the owner called cheap, and the token is 2px now.
        rend_.gradRect(tag, nx::radiusXs * s, nx::glassChip, 0.9f);

        if (hot && in.pressed[0]) pluginSel_ = pi;
        // Double-click loads, matching how the file browser drops a sample.
        // The entry is taken here rather than inside addDevice, which
        // init() also calls through the NXTAKT_DEBUG_ADDFX hook: nothing that
        // happens while the app is starting up belongs in the history.
        //
        // While a rack is OPEN the drop lands inside it instead, which is what
        // an opened container means everywhere else and what the header above
        // the strip says it will do. The rack's own add button does the same
        // thing from the other end, so neither is the only way in.
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
    rend_.textIn(fBold_, {head.x + 10 * s, head.y, 220 * s, head.h},
                 ownerName(devOwner_).c_str(), nx::text, Align::Left, 0);
    rend_.textIn(fSmall_, head,
                 rackOpenUid_ ? "A rack is open - a double-click adds the plugin inside it"
                              : "Double-click a plugin to add it to this chain",
                 rackOpenUid_ ? nx::violetSoft : nx::muted.alpha(0.7f), Align::Right, 8 * s);

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
                     "Double-click a plugin in the browser to add one.", nx::muted,
                     Align::Center);
        rend_.popClip();
        return;
    }

    const f32 boxW = 150 * s, gap = 5 * s, rackW = 448 * s;
    // Spectra's editor opens the same way a rack does and is laid out to the
    // same constraint -- a dock 200 logical pixels tall -- so it is wide and
    // short. The number is the seven column widths plus six gaps plus two pads
    // (app_spectra.cpp's kColW), and it fits the strip with the file browser
    // closed; wider than the strip it simply scrolls, like everything else here.
    const f32 specW = 1112 * s;
    // A rack that is open puts its inside into the strip, immediately after the
    // box it belongs to. Resolved before the layout because it widens it.
    RackControl* openRc = openRack();
    int openIdx = -1;
    if (openRc)
        for (size_t i = 0; i < devices.size(); ++i)
            if (devices[i].uid == rackOpenUid_) { openIdx = (int)i; break; }
    if (openIdx < 0) openRc = nullptr;
    const int specIdx = spectraOpenIdx(devices);

    const f32 total = devices.size() * (boxW + gap) + (openRc ? rackW + gap : 0.f)
                    + (specIdx >= 0 ? specW + gap : 0.f) + 6 * s;
    const f32 maxScroll = std::max(0.f, total - area.w);
    stripScroll_ = clampv(stripScroll_, 0.f, maxScroll);
    bool wheelUsed = false;

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
        // The panel is drawn at the bottom of this iteration, so an off-screen
        // device box must not skip it.
        const bool boxVisible = !(box.right() < area.x || box.x > area.right());
        const bool rackVisible = (int)i == openIdx &&
                                 !(rackBox.right() < area.x || rackBox.x > area.right());
        const bool specVisible = (int)i == specIdx &&
                                 !(specBox.right() < area.x || specBox.x > area.right());
        if (!boxVisible && !rackVisible && !specVisible) continue;
        // Drawn at the END of this device's iteration, whichever way the body
        // leaves it, so the panel's own widgets take `hot` back from the box
        // beside them -- last setHot() of the frame wins, as everywhere here.
        // A device is a rack or a Spectra and never both, so the two are
        // mutually exclusive in practice and the layout does not have to care.
        const auto panel = [&] {
            if (rackVisible) drawRackPanel(rackBox, *openRc, tc);
            if (specVisible) drawSpectraPanel(specBox, d, tc);
        };
        if (!boxVisible) { panel(); continue; }

        const bool sel = (int)i == selDevice_;
        // Claim hot for the whole box first so the controls drawn afterwards
        // can take it back — last setHot() of the frame wins.
        const u64 bid = uiId(11, (int)i, 2);
        const bool hotBox = ui_.setHot(bid, box) && ui_.isHot(bid);
        // A device is a card: --glass-1, a 1px lit edge, --radius-sm. Faked,
        // like every card in the system. §5's disabled rule does the bypass:
        // a bypassed device is not doing anything, and it says so at 40%.
        const f32 dim = d.bypass ? 0.4f : 1.f;
        const f32 rad = nx::radiusSm * s;
        rend_.gradRect(box, rad, nx::glass1, (sel ? 1.f : 0.8f) * (d.bypass ? 0.55f : 1.f));
        if (sel) {
            rend_.gradStroke(box, rad, s, nx::edgeLit, 0.9f);
            rend_.roundRectOutline(box, rad, std::max(1.f, nx::snapPx(s)),
                                   nx::violet.alpha(0.75f));
        } else {
            rend_.gradStroke(box, rad, s, nx::edge, 0.85f * dim);
        }

        Rect title{box.x, box.y, box.w, 16 * s};
        rend_.rect({title.x + 3 * s, title.y + 4 * s, std::max(1.f, nx::snapPx(3 * s)),
                    title.h - 8 * s}, tc.alpha(dim));

        // Both controls are glyph-drawn rather than lettered: at this size the
        // font ellipsises anything longer than a character or two.
        Rect xr{title.right() - 16 * s, title.y + 2 * s, 14 * s, 12 * s};
        Rect br{xr.x - 18 * s, title.y + 2 * s, 18 * s, 12 * s};

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
        Rect kr{br.x, title.y, 0, title.h};
        if (isRack)      kr = Rect{br.x - 34 * s, title.y + 2 * s, 34 * s, 12 * s};
        else if (isSpec) kr = Rect{br.x - 30 * s, title.y + 2 * s, 30 * s, 12 * s};
        const bool hasPanel = isRack || isSpec;

        // The card's controls are ONE cluster, not two or three little capsules
        // adrift in the title bar: chain or edit (only where there is one),
        // bypass, remove. The plate and the lit edge belong to the group; the
        // segments are seams.
        const Rect ctrls{hasPanel ? kr.x : br.x, br.y,
                         xr.right() - (hasPanel ? kr.x : br.x), br.h};
        ui_.segCluster(ctrls);
        rend_.hairlineV(br.x, ctrls.y + 2 * s, ctrls.bottom() - 2 * s);
        if (hasPanel) rend_.hairlineV(kr.right(), ctrls.y + 2 * s, ctrls.bottom() - 2 * s);

        if (isRack) {
            const bool open = (int)i == openIdx;
            // Lettered rather than glyphed, unlike its two neighbours: bypass
            // and remove are universal, "there is a chain in here" is not, and
            // the word is the only thing that says so without being clicked.
            const bool tog = ui_.segButton(uiId(11, (int)i, 3), kr, open, nx::violet);
            ui_.microIn(fSmall_, ui_.lastRect, "chain",
                        open ? nx::text : nx::muted, Align::Center);
            if (tog) {
                if (open) { rackOpenUid_ = 0; rackPath_.clear(); }
                else {
                    rackOpenUid_ = d.uid;
                    rackPath_.clear();
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
                     open ? nx::text : nx::muted, Align::Center);
            if (ui_.hovered(kr))
                ui_.tip = open ? "Close the Spectra panel"
                               : "Open Spectra's editor: the wavetable, the oscillators, "
                                 "the filter, the envelopes and the LFO";
        }

        // The card's title is a micro-label (§5): 10px, uppercase, wide
        // tracking. A device name is an identity, not a sentence.
        Rect nameR{title.x + 10 * s, title.y, (hasPanel ? kr.x : br.x) - title.x - 12 * s, title.h};
        // A device the DAEMON refused or has not confirmed is not an ordinary
        // silent device, and drawing it as one was the §12.7(3) debt: the
        // card sat in the strip looking healthy while the engine ran nothing.
        // Amber name = attention; hover says the daemon's own reason.
        const RemoteDevice* rd = eng_.remoteDevice(d.inst.get());
        const bool refused = rd && !rd->error.empty();
        microFit(ui_, fSmall_, nameR, d.desc.name.c_str(),
                 refused ? pal::meterAmber.alpha(dim)
                         : (sel ? nx::text : nx::muted).alpha(dim),
                 Align::Left, 0);
        if (refused && ui_.setHot(uiId(31, (int)i + 900), nameR) && ui_.isHot(uiId(31, (int)i + 900)))
            ui_.tip = "Engine refused this device: " + rd->error;

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
        if (hotBox && in.pressed[0]) { selDevice_ = (int)i; paramScroll_ = 0.f; }

        Rect body{box.x + 4 * s, title.bottom() + 2 * s, box.w - 8 * s,
                  box.bottom() - title.bottom() - 6 * s};
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
            char buf[64];
            snprintf(buf, sizeof buf, "%d params", d.inst->paramCount());
            if (!d.desc.vendor.empty())
                rend_.textIn(fSmall_, {body.x, body.y + 2 * s, body.w, 12 * s},
                             d.desc.vendor.c_str(), nx::muted.alpha(0.7f * dim),
                             Align::Left, 0);
            rend_.textIn(fSmall_, {body.x, body.y + 15 * s, body.w, 12 * s}, buf,
                         nx::muted.alpha(0.7f * dim), Align::Left, 0);
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
        const f32 pMax = std::max(0.f, rows * chh - body.h);
        if (ui_.hovered(body) && in.wheel != 0.f) {
            paramScroll_ -= in.wheel * chh * 0.5f;
            wheelUsed = true;
        }
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
            if (info.isBool) {
                bool on = d.inst->getParam(p) > 0.5f;
                if (ui_.squareToggle(wid, tg, "", &on, nx::violet)) {
                    undoPoint(info.name.c_str());
                    const f32 nv = on ? info.max : info.min;
                    d.inst->setParam(p, nv);
                    if (ownTrack)
                        autoCapture(addr::deviceParam(ses_.tracks[devOwner_].uid, d.uid, info.id),
                                    nv, wid);
                }
            } else {
                f32 v = d.inst->getParam(p);
                if (ui_.knob(wid, kr, &v, info.min, info.max,
                             info.def, info.isInt ? "%.0f" : "%.2f")) {
                    undoPoint(info.name.c_str());
                    d.inst->setParam(p, v);
                    if (ownTrack)
                        autoCapture(addr::deviceParam(ses_.tracks[devOwner_].uid, d.uid, info.id),
                                    v, wid);
                }
            }

            // --- MIDI learn ------------------------------------------------
            // RIGHT-CLICK CYCLES: unmapped -> learning -> mapped -> unmapped.
            // Not a popup menu, and deliberately: this program has no popup
            // machinery at all, and the existing idiom for "the other thing a
            // control can do" is already the right button (Ui::selector steps
            // backwards on it, the piano roll deletes with it). Inventing a
            // menu system for three states would be the larger change and the
            // one that looks foreign. The three states are legible without it
            // — a dot means mapped, a pulsing ring means listening — and the
            // status bar spells out what the next right-click will do.
            //
            // Only a TRACK's devices can be mapped: the address grammar has no
            // return or master scope, so a return's knob has no address to bind
            // and says so rather than doing nothing.
            const bool overCell = ui_.hovered(cell) && rend_.currentClip().contains(in.mx, in.my);
            // Costs a string only when there is something to say: no bindings
            // and no pointer here means no address is ever built.
            if (ownTrack && (midiMap_.size() || overCell)) {
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
                                         "(right-click again to cancel)"
                            : bound    ? "MIDI-mapped — right-click to clear the mapping"
                                       : "Right-click to MIDI-learn this parameter";
                    if (in.pressed[2]) cycleMidiLearn(pa);
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
        rend_.popClip();
        panel();
    }

    // The strip scrolls horizontally on a plain wheel, unless the pointer was
    // over a parameter grid that wanted the notch for itself.
    if (!wheelUsed && maxScroll > 0.f && ui_.hovered(area) && in.wheel != 0.f)
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

    // --- title: where we are, and the way back out -------------------------
    Rect title{box.x, box.y, box.w, 16 * s};
    rend_.rect({title.x + 3 * s, title.y + 4 * s, std::max(1.f, nx::snapPx(3 * s)),
                title.h - 8 * s}, tc);

    Rect closeR{title.right() - 17 * s, title.y + 2 * s, 14 * s, 12 * s};
    if (ui_.button(uiId(13, 0, 0), closeR, "")) { rackOpenUid_ = 0; rackPath_.clear(); }
    {
        const f32 k = 3.f * s;
        rend_.line(closeR.cx() - k, closeR.cy() - k, closeR.cx() + k, closeR.cy() + k, 1.2f * s, nx::muted);
        rend_.line(closeR.cx() - k, closeR.cy() + k, closeR.cx() + k, closeR.cy() - k, 1.2f * s, nx::muted);
    }
    Rect backR{closeR.x - 20 * s, title.y + 2 * s, 18 * s, 12 * s};
    if (!rackPath_.empty()) {
        if (ui_.button(uiId(13, 0, 1), backR, "<")) { rackPath_.pop_back(); rackSel_ = -1; }
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
    // -----------------------------------------------------------------------
    ui_.microIn(fSmall_, {left.x, left.y, left.w, 11 * s}, "CHAIN", nx::muted, Align::Left, 0);

    const f32 rowH = 15 * s;
    f32 y = left.y + 13 * s;
    const int n = rc.deviceCount();
    for (int i = 0; i < n; ++i) {
        PluginInstance* sub = rc.device(i);
        if (!sub) continue;
        Rect row{left.x, y, left.w, rowH};
        y += rowH + 1 * s;
        if (row.bottom() > left.bottom()) break;

        const bool sel = i == rackSel_;
        const u64 rid = uiId(13, 1, i);
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
        Rect xr{row.right() - 14 * s, row.y + 2 * s, 12 * s, rowH - 4 * s};
        Rect dn{xr.x - 12 * s, row.y + 2 * s, 12 * s, rowH - 4 * s};
        Rect up{dn.x - 12 * s, row.y + 2 * s, 12 * s, rowH - 4 * s};
        // The plate arrives with the pointer: eight rows each wearing a
        // permanent chip would be eight competing surfaces in a 156px column.
        const Rect rowCtrls{up.x, up.y, xr.right() - up.x, up.h};
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
        if (ui_.segButton(uiId(13, 2, i), up, false, nx::violet) && canUp) {
            undoPoint("move device in rack");
            rc.moveDevice(i, i - 1);
            rackChainEdited();
            rackSel_ = i - 1;
            return;                          // the list changed under us
        }
        chevron(up, true, canUp);
        if (ui_.segButton(uiId(13, 3, i), dn, false, nx::violet) && canDn) {
            undoPoint("move device in rack");
            rc.moveDevice(i, i + 1);
            rackChainEdited();
            rackSel_ = i + 1;
            return;
        }
        chevron(dn, false, canDn);
        if (ui_.segButton(uiId(13, 4, i), xr, false, nx::danger)) {
            undoPoint("remove device from rack");
            rc.removeDevice(i);
            rackChainEdited();
            if (rackSel_ >= rc.deviceCount()) rackSel_ = rc.deviceCount() - 1;
            // If what went was itself an open nested rack, openRack() truncates
            // the path on the next frame -- it re-walks it every frame for
            // exactly this reason, so there is nothing to fix up here.
            status_ = "Removed from rack";
            return;
        }
        {
            const f32 k = 2.5f * s;
            rend_.line(xr.cx() - k, xr.cy() - k, xr.cx() + k, xr.cy() + k, 1.1f * s, nx::muted);
            rend_.line(xr.cx() - k, xr.cy() + k, xr.cx() + k, xr.cy() - k, 1.1f * s, nx::muted);
        }

        if (hot && in.pressed[0]) rackSel_ = i;
        if (hot && in.dblClick && nested) { rackPath_.push_back(i); rackSel_ = -1; return; }
        if (hot) ui_.tip = nested ? "Double-click to open this rack"
                                  : "Select; ^ v reorder, x removes";
    }

    // The add row. It uses the plugin browser's selection, and the browser's
    // double-click does the same thing while a rack is open -- one place to
    // pick a plugin, two ways to land it.
    if (y + rowH <= left.bottom()) {
        Rect addR{left.x, y, left.w, rowH};
        const std::vector<PluginDesc>& all = registry_.plugins();
        const bool have = pluginSel_ >= 0 && pluginSel_ < (int)all.size();
        const bool full = n >= kRackMaxDevices;
        char label[80];
        if (full)       snprintf(label, sizeof label, "Rack is full");
        else if (have)  snprintf(label, sizeof label, "+ %s", all[pluginSel_].name.c_str());
        else            snprintf(label, sizeof label, "+ pick a plugin on the left");
        if (ui_.button(uiId(13, 5, 0), addR, label, false, nx::violet) && have && !full) {
            undoPoint("add device to rack");
            if (rc.addDevice(all[pluginSel_])) {
                rackChainEdited();
                rackSel_ = rc.deviceCount() - 1;
                status_ = "Added " + all[pluginSel_].name + " to the rack";
            } else {
                status_ = "Could not add " + all[pluginSel_].name + " to the rack";
            }
            return;
        }
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
            if (c) rend_.hairlineV(b.x, ry0 + 2 * s, ry0 + mh - 2 * s);
            char lbl[8];
            snprintf(lbl, sizeof lbl, "M%d", m + 1);
            // The macros are the rack's identity, so the selected one is violet.
            if (ui_.segButton(uiId(13, 6, m), b, m == rackMacro_, nx::violet)) {
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
    const int wasDev = rackTgtDev_, wasPar = rackTgtParam_;
    ui_.selector(uiId(13, 7, 0), devR, &rackTgtDev_, devPtrs.data(), (int)devPtrs.size());
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
    ui_.selector(uiId(13, 8, 0), parR, &rackTgtParam_, parPtrs.data(), (int)parPtrs.size());
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
    const char* nf = info.isInt ? "%.0f" : "%.2f";
    const f64 per = (f64)(phi - plo) / 160.0;
    if (ui_.dragNumber(uiId(13, 9, 0), minR, &rackMin_, plo, phi, per, nf)) rackRangeHeld_ = true;
    if (ui_.dragNumber(uiId(13, 9, 1), maxR, &rackMax_, plo, phi, per, nf)) rackRangeHeld_ = true;
    if (ui_.hovered(minR)) ui_.tip = "Value at macro 0, in the target's own units";
    if (ui_.hovered(maxR))
        ui_.tip = "Value at macro 1 - set it BELOW the other end to invert the macro";

    if (ui_.button(uiId(13, 10, 0), mapR, "MAP", false, nx::violet)) {
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

    Rect clr{list.right() - 44 * s, list.y, 44 * s, 11 * s};
    char cap[48];
    snprintf(cap, sizeof cap, "MACRO %d DRIVES %d", rackMacro_ + 1, shown);
    microFit(ui_, fSmall_, {list.x, list.y, list.w - 48 * s, 11 * s}, cap,
             nx::muted, Align::Left, 0);
    if (shown > 0 && ui_.button(uiId(13, 11, 0), clr, "clear")) {
        undoPoint("clear macro");
        rc.clearMacro(rackMacro_);
        status_ = "Macro cleared";
        rend_.popClip();
        return;
    }

    const f32 lrow = 13 * s;
    const f32 lmax = std::max(0.f, shown * lrow - (list.h - 13 * s));
    if (ui_.hovered(list) && in.wheel != 0.f) rackListScroll_ -= in.wheel * lrow * 2.f;
    rackListScroll_ = clampv(rackListScroll_, 0.f, lmax);

    f32 ly = list.y + 13 * s - rackListScroll_;
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
        Rect xr{row.right() - 12 * s, row.y + 1 * s, 11 * s, lrow - 2 * s};
        char line[160];
        // The arrow points the way the value moves, so an inverted mapping is
        // legible at a glance rather than by comparing two numbers.
        snprintf(line, sizeof line, "%d/%s   %.2f %s %.2f", m.device + 1, pn,
                 (f64)m.min, m.min > m.max ? "\\" : "/", (f64)m.max);
        rend_.textIn(fSmall_, {row.x, row.y, row.w - 14 * s, row.h}, line,
                     m.min > m.max ? nx::violetSoft : nx::muted, Align::Left, 0);
        if (ui_.button(uiId(13, 12, i), xr, "")) {
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
