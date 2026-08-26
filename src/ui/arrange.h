// The arrangement editor: tracks as lanes, a bar ruler, clips as rectangles on
// a timeline (docs/ARRANGEMENT.md §7).
//
// On the piano roll's seam, and for the roll's reasons. What the editor is
// allowed to see is the plain ArrangeContext below — model pointers and values
// only, no Engine, no Command, no RtClip, no PluginInstance — which is what
// keeps this header compilable against app.h alone and the editor testable
// without a window, exactly as pianoroll.h states for the roll and as
// AutoTargets does for the roll's devices.
//
// The caller (App::drawArrangementView) owns everything the view does not:
// building the context, arrangeRepair, publishArrangementFor, the undo point
// and the transport commands the ruler generates. That division is the roll's,
// and it is why the roll is as short as it is.
#pragma once
#include "autolane.h"
#include "pianoroll.h"      // AutoTargets — the same plain view of a target
#include "timeaxis.h"
#include "widgets.h"
#include <string>
#include <vector>

namespace lat {

// Layout, in logical px (multiply by the DPI scale).
inline constexpr f32 kArrHeaderW   = 138.f;  // the track column on the left
// The ruler is TWO BANDS, and the split is the whole of how the two things that
// want to sit on a timeline position stop fighting over one strip of pixels:
//
//   * the MARKER band, on top, carries the locators' flags;
//   * the BAR band, below, keeps everything it has always had -- the bar
//     numbers, the signature tags, the loop brace, the locate click.
//
// Two bands and therefore two hot rects, which is the disambiguation. A
// right-click in the lower band still adds or removes a signature change, and
// it cannot be confused with the right-click that deletes a flag, because the
// two gestures are not aimed at the same pixels. Nothing is modal and nothing
// asks which one you meant.
inline constexpr f32 kArrRulerH    = 20.f;   // the bar band, unchanged
// 16 and not 15, and the one pixel is the whole of the reason: a flag's hit
// rect is exactly this band's height, and 15 logical px is 15.0 DEVICE px at
// scale 1.0 -- under the 16 px floor for a thing that is CLICKED. A flag is
// clicked (it jumps) as well as dragged, so it answers to the clickable floor
// and not the drag one, and the band it lives in is what supplies the height.
inline constexpr f32 kArrMarkerH   = 16.f;   // the marker band, above it
inline constexpr f32 kArrRulerTotal = kArrRulerH + kArrMarkerH;
// An automation lane needs enough height to aim at and no more (§7.4).
inline constexpr f32 kArrAutoLaneH = 44.f;
inline constexpr f32 kArrMinLaneH  = 26.f;
inline constexpr f32 kArrMaxLaneH  = 320.f;
// The grab bands, in logical px, and the floors they answer to.
//
// A DRAG ZONE HAS TO BE EIGHT PIXELS. That is the measured floor: under it, an
// edge is hit by aiming rather than by pointing, and every miss on an item edge
// is a MOVE -- the loudest possible wrong answer, because it takes the material
// somewhere else instead of doing nothing. kArrEdgeGrab was 5, which is 5 device
// px at 1.0 and 6.1 at 1.25, and it failed at both.
//
// kArrEdgeSlop is the other half of the same fix: the zone reaches OUTSIDE the
// item by this much, so an edge is catchable from the gap beside it. An item is
// a rectangle with nothing to its left but empty lane, and the pixels just
// outside its edge are pixels a hand aiming at that edge lands on.
inline constexpr f32 kArrEdgeGrab  = 8.f;
inline constexpr f32 kArrEdgeSlop  = 3.f;
inline constexpr f32 kArrLaneGrab  = 4.f;
// The fade corner's band: how far in from an item's edge, and how far down from
// its top, the fade handle reaches. Capped at kArrFadeShare of the item's width
// so that the two corners of a SHORT item cannot overlap -- at 14 px flat, an
// item narrower than 28 logical px had its fade-out corner entirely inside its
// fade-in corner, and one of the two was unreachable at any zoom.
inline constexpr f32 kArrFadeGrab  = 14.f;
inline constexpr f32 kArrFadeBandH = 13.f;
inline constexpr f32 kArrFadeShare = 0.4f;
// The loop brace's ends. The brace is DRAWN as two 1.5 px uprights and, until
// this pass, was grabbable by neither: a press anywhere on the ruler started a
// fresh brace from that point, so the only way to move one end was to redraw
// the whole thing. This is the zone those uprights always looked like they had.
inline constexpr f32 kArrLoopGrab  = 5.f;
// A marker flag's zone. THE SIXTEEN-PIXEL FLOOR, applied to a thing whose drawn
// width is its NAME: a flag called "A" is four pixels of text and would be
// unhittable at any zoom, so the zone is widened to the floor around the pole
// rather than being whatever the label happened to measure. The slop reaches
// outside it for kArrEdgeSlop's reason -- the pixels just beside a flag are
// pixels a hand aiming at that flag lands on -- and it is what makes two flags
// a few beats apart still separable: nearest-pole wins inside the overlap.
//
// TEN, and not the eight it was, because the arithmetic is the floor rather
// than a preference: a nameless flag's zone is kArrMarkerGrab wide with
// kArrMarkerSlop on each side, so 8 + 3 + 3 = 14 DEVICE px at scale 1.0 --
// under the 16 px floor for a thing that is CLICKED, which is the same failure
// the disclosure triangle and the override chip were already fixed for.
// 10 + 3 + 3 = 16 lands exactly on it. Nothing about the DRAWN block moves for
// any flag whose name is wider than ten px, which is every flag the auto-namer
// produces; only the nameless worst case grows, and only where it can be hit.
inline constexpr f32 kArrMarkerGrab = 10.f;
inline constexpr f32 kArrMarkerSlop = 3.f;
// How wide one flag may get before its name is truncated. Past this a single
// long name would cover the bars either side of it and hide its neighbours.
inline constexpr f32 kArrMarkerMaxW = 116.f;
// Default zoom: one bar is 64 logical px, which fits about 25 bars in a
// 1600 px window and is the scale an arrangement is usually looked at.
inline constexpr f32 kArrZoomDefault = 16.f;
// The finest grid line the arrangement will draw. 1/16 lines four pixels apart
// are a texture and not a grid, so below this the grid thins to beats and then
// to bars -- the same ladder the ruler's labels thin along, so the two never
// disagree about which lines are the accented ones.
inline constexpr f32 kArrMinGridPx = 7.f;

// What the arrangement editor is allowed to see. Built by
// App::drawArrangementView each frame.
// Forward-declared rather than included: syncSignatures takes it by reference,
// and pulling engine_handle.h in here would put it in every view translation
// unit that draws the timeline.
class EngineHandle;

struct ArrangeContext {
    struct Lane {
        std::string name;
        int   colorIdx = 0;
        std::vector<ArrangeClip>* items  = nullptr;   // edited IN PLACE
        std::vector<AutoLane>*    autos  = nullptr;   // ditto
        f32*  height = nullptr;                       // TrackModel::arrHeight
        const AutoTargets* targets = nullptr;         // what its lanes may name
        bool  overridden = false;                     // Engine::arrOverride bit
        bool  armed = false;
        // A POINTER, where §7.1 has a value: the disclosure triangle is one of
        // the view's own gestures, so it has to be able to write the answer
        // back, and the bit is view state that belongs to no model field 8a
        // froze. The caller owns the storage (App::arrExpanded_).
        bool* expanded = nullptr;                     // automation lanes shown
    };
    std::vector<Lane> lanes;
    // Session::nextUid, so the view can stamp an item it creates -- a split, a
    // duplicate, a Ctrl+drag copy -- in the same breath. §7.1 does not have this
    // and the cost of not having it is a copy with no identity: the selection
    // cannot follow into it, the publisher's envelope table cannot key on it,
    // and the detail panel cannot find it. It is a plain u64 counter, so the
    // view still sees no model type it did not already see.
    u64*  nextUid   = nullptr;
    f64*  loopStart = nullptr;
    f64*  loopEnd   = nullptr;
    bool* loopOn    = nullptr;
    f64   playhead  = 0.0;
    bool  playing   = false;
    // The signature map, BORROWED from the session (timeaxis.h). It replaced a
    // plain `int sigNum`, and the replacement is the whole point of this wave:
    // one number can only describe a set whose bars are all the same width, so a
    // ruler holding one had no way to be wrong and no way to be right either.
    // Still a plain value the view may only read -- no Session crosses the seam.
    SigMap sig;
    // The selected item, as a (track, item uid) pair -- uid and not index,
    // because an insert renumbers indices between frames and a stale index is a
    // wrong-clip edit. Owned by the caller so the detail panel can read it.
    int   selTrack = -1;
    u64   selItem  = 0;

    // --- what the view asks the caller for, written back each frame ---------
    // §7.1 gives the view a Changed mask and nothing else. These four exist
    // because three of the gestures §7.5 lists are not edits to the model at
    // all -- they are requests -- and a mask cannot carry a beat, a track or a
    // drop point.
    //
    // The tracks whose lanes were mutated, so the caller republishes those and
    // not all of them: a lane is up to 1.6 MB, and "republish everything on
    // every edit" would make a nudge cost the whole set.
    std::vector<int> dirty;
    // >= 0: the ruler was clicked at this beat and wants a locate.
    f64  locateBeat = -1.0;
    // >= 0: this track's override tint was clicked and wants Back to Arrangement.
    int  backToArrTrack = -1;
    // >= 0: the ruler was right-clicked at this BAR and wants the signature
    // change there added, or -- if one is already there -- removed. The view
    // reports the bar and not the verb: add and remove are both edits to a map
    // it does not have, the clamps and the dedupe live in session.h, and a view
    // that decided which one it was would have had to keep its own copy of the
    // map to decide with. Bar 0 is reported like any other; the caller refuses
    // to remove it, because Session::removeSignature does.
    i64  sigBar = -1;
    // A drag in flight from elsewhere in the app (the browser, the session
    // grid). The caller sets `dropActive`; the view answers with where it was
    // let go, and the caller decides what to put there -- the view knows
    // nothing about samples, slots or clip loading.
    bool dropActive = false;
    bool dropped    = false;
    int  dropTrack  = -1;
    f64  dropBeat   = 0.0;
    // A one-shot verb the MOUSE asked for (right-click deletes, double-click
    // splits). It is a request and not an edit: the undo point has to be taken
    // before the model moves, and only the caller can take one -- so the view
    // asks, the caller takes its point and calls the verb.
    bool wantDelete = false;
    bool wantSplit  = false;
    // Double-click on EMPTY lane space: create a one-bar MIDI item there.
    // The view reports where; the app decides what a fresh note block is,
    // because the view knows nothing about clips, uids or bar lengths.
    bool wantCreate = false;
    int  createTrack = -1;
    f64  createBeat  = 0.0;
};

class ArrangeView {
public:
    // Draws into `r`, handles every interaction inside it, and returns a mask of
    // what changed so the caller knows what to re-push and what to take an undo
    // point for.
    enum Changed : u32 { None = 0, Items = 1u << 0, Autos = 1u << 1,
                         Loop = 1u << 2, Layout = 1u << 3, Selection = 1u << 4 };
    u32 draw(Ui& ui, const Rect& r, ArrangeContext& ctx);

    // The caller's undo label for whatever last reported a change.
    const char* lastEdit() const { return lastEdit_; }

    // THE UNDO HANDSHAKE. A drag mutates the model over many frames, and an
    // undo entry has to be taken BEFORE the first of them -- but whether a
    // gesture is going to mutate anything is not knowable at the press (a click
    // that only selects must not cost an entry). So the view arms a drag on the
    // press, waits for the cursor to actually move, and on that frame -- before
    // touching the model -- names the edit here. The caller takes its point and
    // the mutation starts on the next frame, which is one frame of latency at
    // the very start of a gesture and the same shape DragState::armed already
    // has. Cleared by the caller through takePendingEdit().
    const char* pendingEdit() const { return pendingEdit_; }
    const char* takePendingEdit() {
        const char* p = pendingEdit_;
        pendingEdit_ = nullptr;
        return p;
    }
    // The gesture id the caller should coalesce its undo entry on, so one drag
    // is one entry. 0 when nothing is being dragged.
    u64 gesture() const { return gesture_; }

    // --- one-shot verbs, for the caller's keyboard routing -----------------
    // Each takes the context rather than trusting the last one drawn: the
    // caller can rebuild it between frames, and a stale pointer is a wrong-lane
    // edit. The caller takes its undo point BEFORE calling (these are one-shot,
    // so there is no gesture to coalesce on) and reads `dirty` afterwards.
    bool hasSelection() const { return selTrack_ >= 0 && selItem_ != 0; }
    u32  deleteSelected(ArrangeContext& ctx);
    // Splits the selected item at the grid-quantized cursor beat. A split
    // exactly on an edge, or one that would leave either half under
    // kMinArrBeats, does nothing.
    u32  splitSelected(ArrangeContext& ctx);
    // Live's duplicate: a copy of the selection one length later, and the
    // selection follows into the copy, because the copy is what the user is
    // about to move.
    u32  duplicateSelected(ArrangeContext& ctx);
    // Where the split lands and where a drop would go. The cursor beat while
    // the pointer is over the lanes, otherwise the playhead as of the last
    // draw -- so Ctrl+E works with the mouse outside the window.
    f64  cursorBeat() const { return cursorBeat_; }

    // --- markers (locators) -------------------------------------------------
    //
    // WHY THESE ARE ON THE VIEW AND NOT IN ArrangeContext, which is where they
    // plainly belong. The context is built and drained in app_chrome.cpp
    // (buildArrangeContext / arrangeCommit), and this wave does not own that
    // file. The precedent is exactly the one the signature publisher set two
    // waves ago and states over syncSignatures below -- state that belongs on
    // App, parked one seam over until one agent owns both files -- and the same
    // note applies: `markers` becomes an ArrangeContext field and the request
    // below becomes three more of them the moment that is true. Nothing else
    // about the design changes when it moves.
    //
    // The list is BORROWED and READ-ONLY, for the reason ctx.sig is borrowed:
    // the view draws it and asks for edits, the caller owns it, and a view that
    // mutated it would have to take the undo point itself. Rebind every frame;
    // an undo replaces the session's contents under it.
    void bindMarkers(const std::vector<Marker>* m) { markers_ = m; }
    // THE QUEUED JUMP. A click on a flag while the transport is running does not
    // move the playhead: it asks for a move AT THE NEXT LAUNCH QUANTUM, which is
    // the same boundary a clip launch waits for and the reason a locator is
    // usable in a performance at all.
    //
    // The pending request is held HERE for the reason markers_ is bound here --
    // app.h is one seam over and this wave does not own it -- and it earns its
    // keep meanwhile by being what the cyan ring is drawn from. The POLICY is
    // entirely the caller's: it decides whether a jump is immediate or queued,
    // it computes the boundary, and it is the only thing that sends a command.
    // This holds three numbers and answers one question about them.
    void queueJump(u64 uid, f64 beat, f64 fireBeat, f64 nowBeat) {
        queuedMarker_ = uid; queuedBeat_ = beat;
        queuedFire_ = fireBeat; queuedFrom_ = nowBeat;
    }
    void clearJump() { queuedMarker_ = 0; }
    u64  queuedMarker() const { return queuedMarker_; }
    f64  queuedFire() const { return queuedFire_; }
    // True ONCE, on the first frame `now` has reached the boundary; the queue is
    // cleared in the same breath so a slow frame cannot fire it twice.
    bool takeDueJump(f64 now, f64* outBeat) {
        if (!queuedMarker_ || now + 1e-9 < queuedFire_) return false;
        if (outBeat) *outBeat = queuedBeat_;
        queuedMarker_ = 0;
        return true;
    }
    // The playhead went BACKWARDS past where the queue was armed -- a loop wrap,
    // another locate, a stop. The boundary this queue is waiting for may now
    // never arrive (a quantum longer than the loop is the ordinary way that
    // happens), and a cyan ring that waits forever is worse than a cancelled
    // jump, because it says something is about to occur and nothing ever does.
    bool jumpOverrun(f64 now) const {
        return queuedMarker_ != 0 && now + 1e-9 < queuedFrom_;
    }

    // A jump asked for by the KEYBOARD rather than by the pointer, routed into
    // the same request the band produces so that "what a jump does" has one
    // implementation and not two. handleShortcuts runs before the view is drawn
    // and the request is drained after it, so both arrive in the same frame.
    void requestJump(u64 uid) {
        markerReq_ = MarkerReq{};
        markerReq_.kind = MarkerReq::Kind::Jump;
        markerReq_.uid  = uid;
    }

    // What the ruler's marker band asked for this frame. AT MOST ONE, because
    // one pointer can only do one thing at a time, and a request rather than an
    // edit for `ctx.sigBar`'s reason: the undo point has to be taken before the
    // model moves and only the caller can take one.
    struct MarkerReq {
        enum class Kind { None, Add, Remove, Move, Rename, Jump };
        Kind kind = Kind::None;
        u64  uid  = 0;          // Remove / Move / Rename / Jump
        f64  beat = 0.0;        // Add / Move
        std::string name;       // Rename
        // Non-zero while a DRAG is producing Moves. The caller coalesces its
        // undo entry on it, so one drag across the ruler is one entry -- the
        // same id the item drags already hand out through gesture().
        u64  gesture = 0;
    };
    MarkerReq takeMarkerReq() {
        MarkerReq r = markerReq_;
        markerReq_ = MarkerReq{};
        return r;
    }
    // The selected flag, so a caller that wants to say something about it can.
    // Settable for the reason selectItem is (§7.7): nothing inside gamescope can
    // click a flag, and a screenshot of a selected one is a check.
    u64  selectedMarker() const { return selMarker_; }
    void selectMarker(u64 uid) { selMarker_ = uid; }

    // --- headless hooks (§7.7) ---------------------------------------------
    // Nothing inside gamescope can click an item, so the selection is settable.
    void selectItem(int track, u64 uid) { selTrack_ = track; selItem_ = uid; }
    int  selectedTrack() const { return selTrack_; }
    u64  selectedItem() const { return selItem_; }
    // The zoom and scroll, so a screenshot can be taken at a known place on the
    // timeline rather than wherever the last gesture left it.
    void setView(f32 pxPerBeat, f32 scrollX) { zoom_ = pxPerBeat; scrollX_ = scrollX; }
    // Where a split would land. Nothing inside gamescope can put a cursor
    // somewhere, and a split at beat 0 is a split the verb correctly refuses.
    void setCursorBeat(f64 b) { cursorBeat_ = b; }

private:
    // Where an item's index lives for a uid, or -1. Everything that edits goes
    // through here: an insert renumbers, and a stale index is a wrong-clip edit.
    static int indexOf(const std::vector<ArrangeClip>& v, u64 uid);

    // One automation lane's view, kept across frames so its own selection and
    // drag survive. Indexed [track][lane]; resized rather than rebuilt, so an
    // edit to a lane does not drop the point the hand is holding.
    std::vector<std::vector<AutoLaneView>> laneViews_;
    // Which target each track's "+" would add a lane for. Per track, because
    // the chooser is per track; kept across frames, because a selector the user
    // has cycled must stay where they left it.
    std::vector<int> targetSel_;

    // The item selection, as the (track, uid) pair the context carries.
    int selTrack_ = -1;
    u64 selItem_  = 0;

    // The view.
    f32 zoom_    = kArrZoomDefault;   // logical px per beat
    f32 scrollX_ = 0.f;               // content px along time
    f32 scrollY_ = 0.f;               // content px down the lane stack
    f64 cursorBeat_ = 0.0;

    // The gestures. Each is a drag on one thing, and every one of them is
    // measured from the values the item had at the press -- so a drag is
    // absolute against its own start and cannot accumulate rounding.
    //
    // Pan / Paint / Erase / EraseMarker / ErasePoint are the four FL gestures
    // this surface was missing. They are Drag states rather than loose bools so
    // that every "is something already held" guard in the file covers them for
    // free -- which is what stops a right-drag erase from also arming a brace,
    // and a middle-drag pan from also selecting an item.
    enum class Drag {
        None, Move, TrimL, TrimR, FadeIn, FadeOut, LaneH, Loop, Marker,
        Pan, Paint, Erase, EraseMarker, ErasePoint
    } drag_ = Drag::None;
    // The five drags that hold one ITEM. The frame arm that runs them used to be
    // spelled "anything still dragging, minus the ones that are not", which is a
    // list that has to be added to every time a gesture is added -- and the one
    // time it was not, a marker drag fell into the item arm and the flag would
    // not move (see the note over that arm). It is a whitelist now.
    static bool itemDrag(Drag d) {
        return d == Drag::Move || d == Drag::TrimL || d == Drag::TrimR ||
               d == Drag::FadeIn || d == Drag::FadeOut;
    }
    bool moved_ = false;              // past the movement threshold; see pendingEdit()
    u64  gesture_ = 0;
    // ONE STROKE, ONE ID, AND NEVER THE SAME ID TWICE. The caller coalesces its
    // undo entry on gesture(), and App::undoCoalesce refuses a second entry for
    // an id it has already seen -- so a gesture id derived only from what was
    // grabbed (the track, the marker's uid) meant that dragging the SAME clip
    // twice in a row with nothing in between took ONE undo entry and the second
    // drag could not be undone at all. Silent, and exactly the shape of loss a
    // user only discovers after the fact. Every stroke below mixes this counter
    // into its id, which costs nothing and makes two strokes distinguishable by
    // construction.
    u32  strokeSeq_ = 0;
    int  dragTrack_ = -1;             // the item's track at the press
    u64  dragUid_ = 0;
    f64  grabBeat_ = 0.0;             // cursor beat - item.start, at the press
    f32  grabY_ = 0.f;
    // The item as it was at the press. A drag reads these and writes absolute
    // values, so it never integrates its own error.
    f64  origStart_ = 0.0, origOffset_ = 0.0, origLength_ = 0.0;
    f64  origFadeIn_ = 0.0, origFadeOut_ = 0.0;
    f32  origHeight_ = 0.f;
    bool dupMade_ = false;            // Ctrl+drag: the copy has been made
    // The loop drag's anchor beat, and whether it has travelled far enough to
    // be a brace rather than a locate.
    f64  loopAnchor_ = 0.0;
    bool loopMoved_ = false;

    // --- MIDDLE-DRAG PANS (FL's playlist, and every other timeline) ----------
    // Absolute against its own start, like every other drag in this file: the
    // scroll is recomputed from where the press was rather than integrated from
    // per-frame deltas, so a dropped frame cannot walk the surface away from the
    // hand and a wheel notch mid-pan cannot fight it.
    f32  panGrabX_ = 0.f, panGrabY_ = 0.f;
    f32  panOrigX_ = 0.f, panOrigY_ = 0.f;

    // --- LEFT-DRAG PAINTS ---------------------------------------------------
    // A sweep across empty lane lays down one block per BAR CELL. The FIRST one
    // goes through ctx.wantCreate, because what a fresh note block is belongs to
    // the caller and must have exactly one implementation; every one after it is
    // a CLONE of what the caller made, which is both cheaper and more faithful
    // (FL paints the same thing along the stroke, not N unrelated blanks).
    int  paintTrack_ = -1;
    f32  paintGrabX_ = 0.f;
    f64  paintCell_  = -1e18;     // the cell last filled, so a jitter cannot restack
    bool paintAwait_ = false;     // a wantCreate is out; adopt its result next frame
    bool paintHave_  = false;     // paintTemplate_ is loaded
    ArrangeClip paintTemplate_;   // what the rest of the stroke clones

    // --- RIGHT-DRAG ERASES --------------------------------------------------
    // The victim is named on one frame and removed on the next, which is the
    // pendingEdit handshake and not an optimisation: the caller has to be able
    // to take its undo point BEFORE the first thing disappears. `eraseLast_` is
    // what stops the item under the press -- already being deleted through
    // ctx.wantDelete -- from being counted twice.
    u64  eraseNext_ = 0;
    int  eraseNextTrack_ = -1;
    u64  eraseLast_ = 0;

    // The markers. `markers_` is borrowed and rebound every frame; everything
    // else here is the view's own and survives across frames because a selection
    // and a drag have to.
    const std::vector<Marker>* markers_ = nullptr;
    u64  selMarker_    = 0;
    u64  queuedMarker_ = 0;      // 0 when no jump is waiting on the quantum
    f64  queuedBeat_   = 0.0;    // where it will land
    f64  queuedFire_   = 0.0;    // the boundary it lands on
    f64  queuedFrom_   = 0.0;    // the playhead when it was armed; see jumpOverrun
    u64  dragMarker_   = 0;      // the flag Drag::Marker is holding
    f64  markerGrab_   = 0.0;    // cursor beat - marker beat, at the press
    f64  markerOrig_   = 0.0;    // where it was at the press; see origStart_
    // The inline rename. The buffer is the view's because Ui::textField writes
    // into whatever it is given and the marker list here is read-only -- so the
    // field edits a copy and the COMMIT is what becomes a request.
    u64  renameMarker_ = 0;
    std::string renameBuf_;
    MarkerReq markerReq_;

    const char* lastEdit_ = "arrangement edit";
    const char* pendingEdit_ = nullptr;
};

// ---------------------------------------------------------------------------
// PUBLISHING THE SIGNATURE MAP  (session.h, publishSignatures)
//
// A set that never publishes plays in 4/4 no matter what the ruler draws, which
// is the one thing about this wave that must not go silently wrong -- so these
// three are declared where both callers can see them rather than left to a call
// site somebody has to remember.
//
// WHERE THE STATE LIVES, and why not on App. The published pointer, the arrays
// still in flight and the last map published are App's business by every other
// precedent in this program (publishedNotes_/retiringNotes_, warpMaps_). They
// are file-static in app_arrange.cpp instead because this wave does not own
// app.h -- exactly the reason warpMaps_ spent a wave in a translation unit
// before it moved, and the same note applies: this belongs on App and should go
// there the moment one agent owns both files.
//
// GUI THREAD ONLY, like everything else in src/ui.
// ---------------------------------------------------------------------------

// The session's map, as a view sees it. ONE place, so a ruler, a readout and a
// grid cannot end up holding three differently-built views of the same vector --
// and one place for the rule that an EMPTY `sigs` is not "4/4" but "one entry,
// sigNum/sigDen, at bar 0", which is what a set that has never been re-barred
// and every v1..v6 file mean (session.h says the same over Session::sigAtBar).
inline SigMap sigMapOf(const Session& s) {
    SigMap m;
    m.v     = s.sigs.empty() ? nullptr : s.sigs.data();
    m.n     = (int)s.sigs.size();
    m.fbNum = s.sigNum;
    m.fbDen = s.sigDen;
    return m;
}

// Publish `s`'s map if it differs from the one last published, and do nothing at
// all if it does not. Called once per frame from the control bar -- which is
// drawn unconditionally and first -- so a load, an undo, a redo and every
// signature edit are all covered by one call site and none of them can be
// forgotten. A publication the ring refuses is simply retried next frame.
//
// Takes the HANDLE and not an Engine: in daemon mode there is no in-process
// Engine, and taking one is what kept this from being called there at all --
// which is why daemon mode played every set in 4/4 however the ruler was drawn.
void syncSignatures(EngineHandle& eng, const Session& s);

// Ev::SigsRetired. Returns true when `p` was an array we published and have now
// freed, false when it is not one of ours -- in which case the caller must leak
// it rather than free a pointer nobody here owns, which is the rule every other
// retirement in this program follows.
bool reapSignatures(const void* p);

// Everything still held, freed. Safe ONLY once the audio thread is joined, i.e.
// after EngineHandle::close(), beside App::shutdown()'s existing sweep over the
// chains and the note arrays.
//
// NOT CALLED YET, and deliberately so rather than by omission: App::shutdown()
// is in app.cpp, which this wave does not own. Nothing leaks meanwhile -- the
// owning object has static storage duration and frees in its destructor, which
// runs after main() returns and therefore after the audio thread is long gone,
// and a leak checker sees nothing. This is the tidier place for it and the one
// line App::shutdown() is owed.
void dropSignatures();

// How many displaced arrays are still waiting for their Ev::SigsRetired, and how
// many have come home. For the headless hook only: a retirement protocol that
// publishes and never reaps looks exactly like one that works, until the leak
// checker or the machine runs out of memory hours later.
int  sigsInFlight();
int  sigsReaped();

} // namespace lat
