// Piano roll: the MIDI note editor shown in the CLIP tab.
//
// Layout, top to bottom: a ruler (bar.beat numbers, FOLD, loop length), the
// note grid with a keyboard column on its left, then the velocity lane. Every
// pixel <-> musical conversion goes through the two axis structs below, so the
// drawing pass and the hit testing can never disagree about where a note is —
// which is the entire reason the editing code can be this short.
//
// Everything above `PianoRoll::draw` is pure: no Ui, no Renderer, no member
// state. That is deliberate — the grid mapping, the fold row set and the edit
// clamps are the parts that are worth testing without a window.
#include "pianoroll.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace lat {
namespace {

// ---------------------------------------------------------------------------
// NXTAKT_DEBUG_PROBE -- the roll's half of the headless drive
//
// The same read-back the arrangement has, for the two things only the roll
// owns: the notes (beat, pitch, length, velocity, chance, range) and the
// breakpoints of whichever envelope the lane is showing. Printed on a frame
// that already reported a change, so a drive script can tie a line to a
// gesture; one cached bool when the variable is unset.
// ---------------------------------------------------------------------------
bool probeOn() {
    static const bool on = std::getenv("NXTAKT_DEBUG_PROBE") != nullptr;
    return on;
}

// ---------------------------------------------------------------------------
// constants (logical px unless noted; multiply by the DPI scale)
// ---------------------------------------------------------------------------

// kGridStep, kBeatsPerBar, kPxPerBeatMin/Max and kZoomMin/Max/PerNotch moved
// into timeaxis.h with TimeAxis itself (docs/ARRANGEMENT.md §7.2): they are
// about *time*, and the arrangement needs the identical numbers. What stays
// here is everything that is about a piano roll rather than about a timeline.
constexpr int kMinFoldRows = 8;     // a one-note clip still needs room to click
constexpr f32 kKeyW       = 46.f;
constexpr f32 kRulerH     = 16.f;
constexpr f32 kLaneH      = 54.f;
constexpr f32 kRowH       = 12.f;
constexpr int kCentrePitch  = 60;   // C4, the middle of the default C3..C5 view
constexpr f64 kMaxLoopBeats = 64.0; // ceiling for Ctrl+U, 16 bars in 4/4
// The note's right-hand RESIZE band, and the share of a short note it may take.
// It was a flat 4 logical px -- 4.0 device px at scale 1.0, half the 8 px floor
// -- and every miss on it is a MOVE, which is the loudest possible wrong answer
// because it takes the note somewhere else instead of doing nothing. 8, capped
// at a share of the note so a 1/32 at low zoom is still grabbable to move.
// There is no left-edge resize in this roll and this pass does not add one: a
// note's start is what a move sets, and two edges on a 12 px row would leave a
// body too small to grab.
//
// The share was 0.35 and is 0.5 as of the FL pass, which is the finding the
// brief predicted: at the fit zoom a 1/16 is 32 px and the band is the full 8,
// but a 1/32 at 128 px/beat is 16 px wide and got 5.6 -- under the 8 px floor
// -- and a note drawn at the 3 px minimum got ONE PIXEL of tail against two of
// body. Grabbing what looks like the end of a short note therefore MOVED it,
// silently, which is exactly the class of bug the last audit found on the
// arrangement's fades. Half is the honest answer at every width: the right half
// of a note resizes, the left half moves, and no note is ever mostly-move with
// a tail nobody can hit.
constexpr f32 kNoteEdgeGrab  = 8.f;
constexpr f32 kNoteEdgeShare = 0.5f;
// How finely a Paint or Erase sweep samples the segment it travelled since the
// last frame, in DEVICE px. Small enough that it cannot step over a grid cell
// (the narrowest a cell is ever drawn is kGridStep * kZoomMin = 2 px) and over
// a note row (12 logical px), and capped so a pointer teleport -- a warp, a
// dropped frame under load -- costs a bounded walk rather than a stall.
constexpr f32 kSweepStep   = 2.f;
constexpr int kSweepMaxHits = 512;
// How far from a stem's x the lane will pick it up. 6 gave a 12 logical px
// band; 8 gives 16, which is the floor. It is a nearest-match, so a wider band
// never picks the wrong stem -- it only stops picking NONE.
constexpr f32 kStemGrab = 8.f;
// The undo labels the caller reads back off lastEdit().
constexpr const char* kEditNote = "note edit";
constexpr const char* kEditAuto = "automation edit";

// ---------------------------------------------------------------------------
// axes
//
// The TIME axis moved to timeaxis.h. The PITCH axis did not: nothing but a
// piano roll has one.
// ---------------------------------------------------------------------------

struct PitchAxis {
    f32 y0 = 0, rowH = 12.f, view = 0;
};
inline f32 rowToY(const PitchAxis& a, int row) { return a.y0 - a.view + (f32)row * a.rowH; }
inline int yToRow(const PitchAxis& a, f32 y) {
    return (int)std::floor((y - a.y0 + a.view) / a.rowH);
}

// ---------------------------------------------------------------------------
// row set (fold)
// ---------------------------------------------------------------------------

// Row 0 is the top row = the highest pitch. Unfolded this is just 127 - pitch;
// folded it collapses the gaps, which is why every pitch lookup has to go
// through here instead of doing the arithmetic inline.
struct RowMap {
    int pitchOf[128]{};
    int rowOfP[128]{};
    int count = 0;
    int pitchAt(int row) const { return (row >= 0 && row < count) ? pitchOf[row] : -1; }
    int rowOf(int pitch) const { return (pitch >= 0 && pitch < 128) ? rowOfP[pitch] : -1; }
};

// `keepPitch` is the pitch a move drag started on: it stays on screen for the
// whole gesture even after the note leaves it, otherwise the row under the
// cursor would renumber mid-drag and the note would jump.
//
// FoldMode::Key is deliberately NOT "Used, restricted to the scale". It ignores
// the clip's contents entirely and admits every in-scale pitch across the whole
// range, because the case it exists for is the empty clip: fold to the key and
// every row you can click is a right note. Padding to kMinFoldRows is therefore
// only meaningful for Used — a scale has at least five degrees per octave and
// so always fills the axis.
//
// Both folds fall back to the full chromatic range when they have nothing to
// stand on (a clip with no notes, a session with no scale). A blank editor is
// never the right answer to "show me less".
RowMap buildRows(const std::vector<NoteModel>& notes, FoldMode fold, const ScaleKey& key,
                 int keepPitch) {
    RowMap m;
    for (int p = 0; p < 128; ++p) m.rowOfP[p] = -1;

    bool used[128]{};
    int n = 0;
    if (fold == FoldMode::Used) {
        for (const NoteModel& nt : notes)
            if (nt.pitch < 128 && !used[nt.pitch]) { used[nt.pitch] = true; ++n; }
    } else if (fold == FoldMode::Key && key.active()) {
        for (int p = 0; p < 128; ++p)
            if (key.contains(p)) { used[p] = true; ++n; }
    }
    // The row the hand is on survives whatever the fold says, in both modes: a
    // drag that carried a note onto a pitch the fold hides would otherwise
    // renumber the axis out from under the gesture.
    if (n > 0 && keepPitch >= 0 && keepPitch < 128 && !used[keepPitch]) {
        used[keepPitch] = true;
        ++n;
    }
    if (n == 0) {
        m.count = 128;
        for (int i = 0; i < 128; ++i) { m.pitchOf[i] = 127 - i; m.rowOfP[127 - i] = i; }
        return m;
    }
    if (fold == FoldMode::Used) {
        int lo = 0;   while (!used[lo]) ++lo;
        int hi = 127; while (!used[hi]) --hi;
        // Pad downwards first (a melody sits above its padding, like a keyboard).
        while (n < kMinFoldRows && (lo > 0 || hi < 127)) {
            if (lo > 0) { used[--lo] = true; }
            else        { used[++hi] = true; }
            ++n;
        }
    }
    for (int p = 127; p >= 0; --p)
        if (used[p]) { m.pitchOf[m.count] = p; m.rowOfP[p] = m.count; ++m.count; }
    return m;
}

inline bool isBlackKey(int pitch) {
    const int pc = ((pitch % 12) + 12) % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

// ---------------------------------------------------------------------------
// edit primitives
// ---------------------------------------------------------------------------

// quantFloor / quantNear moved to timeaxis.h with the grid step they are
// measured in: a snap is a statement about the time axis and nothing else.

// Snapped note start, kept inside the clip at both ends.
inline f64 clampBeat(f64 raw, f64 len, f64 lengthBeats) {
    const f64 hi = std::max(0.0, lengthBeats - len);
    return clampv(quantNear(raw), 0.0, hi);
}

// Length from a dragged right edge: whole grid steps, never under one step and
// never past the end of the clip.
//
// `snap` is false while Alt (FL) or Shift is held, and then the length is what
// the pointer says to the pixel. The floor stays a whole grid step either way:
// a note shorter than the smallest thing the grid can express is a note nobody
// can see, find or grab again, and "free" was never a licence to make one.
inline f64 clampLen(f64 rawRight, f64 beat, f64 lengthBeats, bool snap = true) {
    const f64 want = snap ? quantNear(rawRight - beat) : (rawRight - beat);
    const f64 len  = std::max(kGridStep, want);
    const f64 room = std::max(kGridStep, lengthBeats - beat);
    return std::min(len, room);
}

inline bool noteLess(const NoteModel& a, const NoteModel& b) {
    return a.beat != b.beat ? a.beat < b.beat : a.pitch < b.pitch;
}
// Two notes are interchangeable when EVERY field matches, the generative pair
// included. That is not fussiness: this is what re-finds a note after a sort,
// and a selection holding "the note at beat 1 pitch 60" would otherwise be able
// to land on its twin with a different chance and then edit the wrong one.
inline bool sameNote(const NoteModel& a, const NoteModel& b) {
    return a.beat == b.beat && a.pitch == b.pitch && a.len == b.len && a.vel == b.vel &&
           a.chance == b.chance && a.velTo == b.velTo;
}

// ---------------------------------------------------------------------------
// the per-note lanes
//
// One gesture edits three different fields, so the field is data rather than a
// branch in the drag: the lane asks for a normalized height and writes it back
// through the same pair of functions the drawing reads it with, which is why a
// stem can never be drawn at a height a drag would not reproduce.
//
// Every one of the three is an ABSOLUTE 0..1 of its own full range, so the
// bottom of the lane means the same thing in all three: velocity 1 (never 0 —
// that is a note-off on the wire), chance 0, and "no velocity range at all".
// ---------------------------------------------------------------------------

inline f32 noteLaneValue(const NoteModel& n, NoteLane which) {
    switch (which) {
    case NoteLane::Chance:   return (f32)n.chance / 100.f;
    case NoteLane::VelRange: return (f32)n.velTo / 127.f;
    default:                 return (f32)n.vel / 127.f;
    }
}

// Returns true when the note actually moved, so a drag over stems that are
// already at the cursor's height reports no change and leaves the undo history
// alone.
inline bool setNoteLaneValue(NoteModel& n, NoteLane which, f32 t) {
    t = clampv(t, 0.f, 1.f);
    switch (which) {
    case NoteLane::Chance: {
        const u8 v = (u8)clampv((int)std::lround(t * 100.f), 0, 100);
        if (n.chance == v) return false;
        n.chance = v;
        return true;
    }
    case NoteLane::VelRange: {
        // 0 is the sentinel for "no range", and it is also the bottom of the
        // drag — which is exactly the affordance wanted: pulling a range stem
        // all the way down turns the range off rather than pinning the note to
        // velocity 1.
        const u8 v = (u8)clampv((int)std::lround(t * 127.f), 0, 127);
        if (n.velTo == v) return false;
        n.velTo = v;
        return true;
    }
    default: {
        const u8 v = (u8)clampv((int)std::lround(t * 127.f), 1, 127);
        if (n.vel == v) return false;
        n.vel = v;
        return true;
    }
    }
}

// Restores the sorted-by-beat invariant and reports where `key` ended up. An
// index is the only handle we have on a note, so any edit that can reorder the
// vector has to re-find the note it just touched. Two notes that compare equal
// on all four fields are interchangeable, so picking the first match is safe.
int sortTracking(std::vector<NoteModel>& v, const NoteModel& key) {
    std::sort(v.begin(), v.end(), noteLess);
    for (size_t i = 0; i < v.size(); ++i)
        if (sameNote(v[i], key)) return (int)i;
    return -1;
}

// ---------------------------------------------------------------------------
// the selection, as a set
// ---------------------------------------------------------------------------

// A selection expressed as *notes* rather than indices — the only form of it
// that survives a re-sort. `primary` is a slot in `notes`, or -1.
struct SelKeys {
    std::vector<NoteModel> notes;
    int primary = -1;
};

// sortTracking for a whole set. Two notes that compare equal on all four
// fields are interchangeable, but they are still two notes: each key claims a
// slot of its own, so a selection holding both never collapses onto one index
// (and then moves that one note twice as far on the next drag). The search
// starts at the sorted position of the key, so this stays near-linear even
// when the whole clip is selected and a group drag re-runs it every frame.
//
// `outSel` comes back sorted ascending; a key whose note no longer exists is
// simply dropped, which is what makes this safe to run after a trim.
void sortTrackingSet(std::vector<NoteModel>& v, const SelKeys& keys, IndexSel& out) {
    std::sort(v.begin(), v.end(), noteLess);
    out.clear();
    const size_t n = v.size(), k = keys.notes.size();
    if (k == 0 || n == 0) return;
    std::vector<bool> taken(n, false);
    for (size_t j = 0; j < k; ++j) {
        const NoteModel& key = keys.notes[j];
        size_t i = (size_t)(std::lower_bound(v.begin(), v.end(), key, noteLess) - v.begin());
        for (; i < n; ++i) {
            if (noteLess(key, v[i])) break;          // past every equal-ordering note
            if (taken[i] || !sameNote(v[i], key)) continue;
            taken[i] = true;
            out.items.push_back((int)i);
            if ((int)j == keys.primary) out.primary = (int)i;
            break;
        }
    }
    std::sort(out.items.begin(), out.items.end());
}

// How far a selection can travel before one of its members hits a wall: the
// start of the clip, its end, pitch 0 or pitch 127. Clamping the *group* this
// way rather than each note on its own is the whole difference between a group
// move that keeps its shape and one that piles up against the edge.
struct GroupRoom {
    f64  left = 0.0, right = 0.0;   // beats: most negative / most positive move
    int  down = 0, up = 0;          // semitones
    bool empty = true;
};
GroupRoom groupRoom(const std::vector<NoteModel>& notes, const std::vector<int>& sel,
                    f64 lengthBeats) {
    GroupRoom g;
    f64 minBeat = 0.0, maxEnd = 0.0;
    int minP = 0, maxP = 0;
    for (int i : sel) {
        if (i < 0 || i >= (int)notes.size()) continue;
        const NoteModel& nt = notes[(size_t)i];
        if (g.empty) {
            minBeat = nt.beat; maxEnd = nt.beat + nt.len;
            minP = maxP = (int)nt.pitch;
            g.empty = false;
        } else {
            minBeat = std::min(minBeat, nt.beat);
            maxEnd  = std::max(maxEnd, nt.beat + nt.len);
            minP    = std::min(minP, (int)nt.pitch);
            maxP    = std::max(maxP, (int)nt.pitch);
        }
    }
    if (g.empty) return g;
    g.left  = -minBeat;
    // A group that already hangs past the end of the clip (an over-long note,
    // or a loop dragged shorter under it) has *negative* room to the right, and
    // taking that as the clamp pulls it back inside — which is exactly what the
    // single-note clamp does. It can never pull further left than beat 0.
    g.right = std::max(g.left, lengthBeats - maxEnd);
    g.down  = -minP;
    g.up    = 127 - maxP;
    return g;
}

// A group move, already clamped. Both fields are deltas, not destinations.
struct GroupDelta {
    f64 beats = 0.0;
    int semis = 0;
};
GroupDelta clampGroupDelta(const std::vector<NoteModel>& notes, const std::vector<int>& sel,
                           f64 dBeats, int dSemis, f64 lengthBeats) {
    GroupDelta d;
    const GroupRoom g = groupRoom(notes, sel, lengthBeats);
    if (g.empty) return d;
    d.beats = clampv(dBeats, g.left, g.right);
    d.semis = clampv(dSemis, g.down, g.up);
    return d;
}

// Applies a clamped delta to every selected note and restores the sorted-by-
// beat invariant, re-deriving `sel` and `primary` through it. Returns false
// when the delta was zero (a group already against both walls), in which case
// nothing was touched and the caller must not report a change.
//
// Note that only the group's *extremes* were clamped: the members are moved by
// the same delta, so the shape of a chord or a riff is preserved exactly.
// `key` is the session's scale. When it is snapping, the delta is applied first
// and each moved pitch is then pulled onto the nearest degree, biased by the
// direction of travel. Applying the delta first and snapping after — rather
// than computing a per-note "next degree" delta — is what keeps a chord a chord:
// every member takes the same chromatic step and only then rounds, so a shape
// survives a transposition through a scale with uneven steps instead of
// collapsing onto whichever degrees happen to be nearest.
bool applyGroupDelta(std::vector<NoteModel>& notes, IndexSel& sel, const GroupDelta& d,
                     const ScaleKey& key) {
    if (d.beats == 0.0 && d.semis == 0) return false;
    const bool snap = key.snap && key.active();
    const int dir = d.semis > 0 ? 1 : (d.semis < 0 ? -1 : 0);
    SelKeys keys;
    keys.notes.reserve(sel.items.size());
    bool moved = false;
    for (int i : sel.items) {
        if (i < 0 || i >= (int)notes.size()) continue;
        NoteModel& nt = notes[(size_t)i];
        const NoteModel was = nt;
        nt.beat  = nt.beat + d.beats;
        int p = clampv((int)nt.pitch + d.semis, 0, 127);
        if (snap) p = key.snapPitch(p, dir);
        nt.pitch = (u8)p;
        if (!sameNote(nt, was)) moved = true;
        if (i == sel.primary) keys.primary = (int)keys.notes.size();
        keys.notes.push_back(nt);
    }
    if (keys.notes.empty()) return false;
    sortTrackingSet(notes, keys, sel);
    // A snap can swallow the whole delta -- nudging a note up a semitone inside
    // a scale that does not admit the pitch above it and does not admit the one
    // above that either, at the top of the range. Reporting "changed" then would
    // leave an undo entry that undoes nothing.
    return moved;
}

// Keyboard nudge: `steps` grid steps along time, `semis` semitones of pitch,
// applied to every selected note. Both clamped — into the clip at both ends,
// into 0..127 — once for the group.
//
// The time nudge goes through the same snap as a mouse move, measured on the
// PRIMARY note: nudging an off-grid note (one that arrived by MIDI recording)
// pulls it onto the grid rather than carrying the offset along forever, and a
// selection is pulled onto the grid by its anchor while keeping its internal
// spacing. For a one-note selection this is exactly clampBeat, note for note.
//
// `changed` is false when the nudge changed nothing at all (a group already
// against the clamp); otherwise `sel` and `primary` come back re-derived.
struct NudgeResult {
    bool changed = false;
    bool pitchChanged = false;
};
NudgeResult nudgeGroup(std::vector<NoteModel>& notes, IndexSel& sel,
                       int steps, int semis, f64 lengthBeats, const ScaleKey& key) {
    NudgeResult res;
    if (sel.empty()) return res;
    const int anchor = (sel.primary >= 0 && sel.primary < (int)notes.size()) ? sel.primary
                                                                            : sel.items.front();
    if (anchor < 0 || anchor >= (int)notes.size()) return res;
    const f64 aBeat = notes[(size_t)anchor].beat;
    const f64 want = steps != 0 ? quantNear(aBeat + (f64)steps * kGridStep) - aBeat : 0.0;
    const GroupDelta d = clampGroupDelta(notes, sel.items, want, semis, lengthBeats);
    if (!applyGroupDelta(notes, sel, d, key)) return res;
    res.changed = true;
    res.pitchChanged = d.semis != 0;
    return res;
}

// Live's duplicate-loop: the loop doubles and everything in it is copied one
// old-length later, so a bar of material becomes two bars of it. The selection
// follows into the *copy* — the whole set does, note for note — because the
// copy is what the user is about to edit.
//
// The cap is a length, not a factor: doubling a 40-beat loop gives 64 and the
// copies that would start past the new end are simply not made (a note that
// straddles the end is trimmed). A selected note whose copy was not made keeps
// the selection on the original, so the set never silently shrinks. Nothing
// happens at all once the loop is already at the cap — a no-op that reports
// false, so the caller does not re-push an unchanged clip.
struct DupResult {
    bool changed = false;
    IndexSel sel;
};
DupResult duplicateLoopNotes(std::vector<NoteModel>& notes, f64& lengthBeats,
                             const IndexSel& selected) {
    DupResult res;
    const f64 oldLen = std::max(kGridStep, lengthBeats);
    if (oldLen >= kMaxLoopBeats) return res;
    const f64 newLen = std::min(kMaxLoopBeats, oldLen * 2.0);

    // The notes the selection should end on, tracked as notes rather than
    // indices: a bare sort would leave the caller holding indices into the old
    // order. Each starts as the original and is overwritten by its copy if one
    // gets made.
    const size_t n = notes.size();
    SelKeys keys;
    std::vector<int> slotOf(n, -1);          // note index -> slot in keys
    for (int i : selected.items) {
        if (i < 0 || i >= (int)n) continue;
        slotOf[(size_t)i] = (int)keys.notes.size();
        if (i == selected.primary) keys.primary = (int)keys.notes.size();
        keys.notes.push_back(notes[(size_t)i]);
    }
    for (size_t i = 0; i < n; ++i) {
        NoteModel c = notes[i];
        c.beat += oldLen;
        if (c.beat >= newLen - 1e-9) continue;
        c.len = std::min(c.len, newLen - c.beat);
        if (slotOf[i] >= 0) keys.notes[(size_t)slotOf[i]] = c;
        notes.push_back(c);
    }
    lengthBeats = newLen;
    res.changed = true;
    // The vector is two sorted runs (originals, then copies, each in order and
    // the second entirely later), which the re-sort inside here restores.
    sortTrackingSet(notes, keys, res.sel);
    return res;
}

// Screen span of a note along the time axis, including the minimum width the
// grid draws a very short note at. The hit tests and the drawing have to agree
// about where a note is, so both come through here.
inline void noteSpanX(const NoteModel& nt, const TimeAxis& ta, f32 minW, f32& x0, f32& x1) {
    x0 = beatToX(ta, nt.beat);
    x1 = std::max(beatToX(ta, nt.beat + nt.len), x0 + minW);
}

// Every note whose block touches `band`, in index order. Touching counts: a
// band that grazes an edge takes the note, and a band with no height (dragged
// straight along one row, which is the commonest way to sweep a line of notes)
// still takes the row it is on. Notes on a pitch the row map does not contain
// — folded away — cannot be banded, since they are not on screen to be swept.
void notesInBand(const std::vector<NoteModel>& notes, const RowMap& rows,
                 const TimeAxis& ta, const PitchAxis& pa, const Rect& band, f32 minW,
                 std::vector<int>& out) {
    out.clear();
    for (size_t i = 0; i < notes.size(); ++i) {
        const int row = rows.rowOf(notes[i].pitch);
        if (row < 0) continue;
        f32 x0 = 0.f, x1 = 0.f;
        noteSpanX(notes[i], ta, minW, x0, x1);
        if (x1 < band.x || x0 > band.right()) continue;
        const f32 y0 = rowToY(pa, row), y1 = y0 + pa.rowH;
        if (y1 < band.y || y0 > band.bottom()) continue;
        out.push_back((int)i);
    }
}

// Topmost note under a point, or -1. Later notes win so the hit order matches
// the draw order.
int noteAt(const std::vector<NoteModel>& notes, const RowMap& rows,
           const TimeAxis& ta, const PitchAxis& pa, f32 mx, f32 my, f32 minW) {
    const int pitch = rows.pitchAt(yToRow(pa, my));
    if (pitch < 0) return -1;
    int found = -1;
    for (size_t i = 0; i < notes.size(); ++i) {
        if ((int)notes[i].pitch != pitch) continue;
        f32 x0 = 0.f, x1 = 0.f;
        noteSpanX(notes[i], ta, minW, x0, x1);
        if (mx >= x0 && mx < x1) found = (int)i;
    }
    return found;
}

// Is there already a note sounding this pitch at this beat? The brush's
// duplicate guard, and it asks about the MUSIC rather than about the cell: a
// half-note already covering the cell under the pointer means there is nothing
// to add there, even though the cell it *starts* in is a different one. Without
// that, sweeping along a row of long notes buries each of them under a stack of
// sixteenths nobody asked for and nobody can see.
bool pitchBusyAt(const std::vector<NoteModel>& notes, int pitch, f64 beat) {
    for (const NoteModel& nt : notes)
        if ((int)nt.pitch == pitch && beat >= nt.beat - 1e-9 &&
            beat < nt.beat + nt.len - 1e-9)
            return true;
    return false;
}

// Walks the SEGMENT a sweep travelled since the last frame and calls `fn` at
// every sample along it, ends included. A brush that only tested the point the
// frame landed on would skip a cell at any speed a hand actually moves: 60 fps
// and a leisurely 500 px/s is 8 px a frame, and one grid cell at low zoom is 2.
//
// Returns the number of samples taken, which is only interesting to the cap.
template <class F>
int sweepSegment(f32 x0, f32 y0, f32 x1, f32 y1, F&& fn) {
    const f32 dx = x1 - x0, dy = y1 - y0;
    const f32 dist = std::sqrt(dx * dx + dy * dy);
    int steps = (int)std::ceil(dist / kSweepStep);
    if (steps < 1) steps = 1;
    if (steps > kSweepMaxHits) steps = kSweepMaxHits;
    for (int i = 0; i <= steps; ++i) {
        const f32 t = (f32)i / (f32)steps;
        fn(x0 + dx * t, y0 + dy * t);
    }
    return steps;
}

// ---------------------------------------------------------------------------
// the automation lane
//
// It moved. ValAxis, ptLess/samePt, PtKeys, sortTrackingPts, the PtDelta clamp
// and its apply, ptScreen, pointAt, pointsInBand and insertPoint are now
// src/ui/autolane.cpp, together with the lane's own drag and selection state
// and the slice of draw() that used them (docs/ARRANGEMENT.md §7.3, which is
// docs/AUTOMATION.md §6.5 finally paid). The roll is the lane's FIRST caller
// and keeps its chooser, drawLaneKey, which is roll-specific: it is what *adds*
// a lane to a clip. The lane is handed the roll's own TimeAxis, so the property
// that made it live here in the first place is now a parameter.
// ---------------------------------------------------------------------------

// dpiOf moved to autolane.h with the lane: the arrangement needs the same
// answer, and two ways of recovering the DPI scale would be two answers.

} // namespace

// IndexSel: its implementation moved to autolane.cpp with its declaration.

// ---------------------------------------------------------------------------
// PianoRoll: the lane's key block
//
// Where the static "VEL" label used to be (decision #9 in
// docs/audits/WAVE7-DECISIONS.md): three stacked rows in the 46 px gutter under
// the keyboard column — what the lane shows, what "+" would add, and the shown
// lane's on/off toggle beside the "+" itself.
//
// `selector` rather than a new dropdown widget because the codebase has no
// dropdown and inventing one is a separate piece of work: click cycles forward,
// right-click cycles back, wheel scrubs, which is the documented idiom
// everywhere else in the program. The gutter is too narrow for a plugin's
// parameter name, so the full text goes to the tooltip and the row shows what
// fits.
// ---------------------------------------------------------------------------

void PianoRoll::drawLaneKey(Ui& ui, const Rect& b, ClipModel& clip,
                            const AutoTargets& targets, f32 s, bool& changed) {
    if (!ui.r) return;
    Renderer& rr = *ui.r;
    rr.pushClip(b);

    const f32 rowH = std::max(9.f * s, (b.h - 4.f * s) / 3.f);
    const Rect r0{b.x + 2.f * s, b.y + 1.f * s, b.w - 4.f * s, rowH};
    const Rect r1{r0.x, r0.bottom() + 1.f * s, r0.w, rowH};
    const Rect r2{r0.x, r1.bottom() + 1.f * s, r0.w, rowH};

    // What the lane shows: the per-note fields first (three on a pattern, one
    // inert placeholder on a sample, which is what envLaneBase counts), then
    // one entry per envelope the clip has. The names are the target's short
    // label where the address resolves and the address itself where it does not
    // — a lane naming a missing device must still be findable.
    std::vector<std::string> names;
    if (clip.kind == ClipKind::Midi)
        for (int i = 0; i < kNoteLaneCount; ++i) names.push_back(kNoteLaneNames[i]);
    else
        names.push_back("--");
    for (const AutoLane& l : clip.envelopes) {
        const AutoTargets::Entry* e = targets.find(l.address);
        names.push_back(e ? e->label : l.address);
    }
    std::vector<const char*> ptrs;
    ptrs.reserve(names.size());
    for (const std::string& n : names) ptrs.push_back(n.c_str());

    laneSel_ = clampv(laneSel_, 0, (int)ptrs.size() - 1);
    int shown = laneSel_;
    // THE KEY BLOCK'S FOUR TARGETS ARE SHORT. Three rows share whatever height
    // the lane has, and the lane is a third of a 151 px editor: 14.8 device px
    // each at DPI 1.0, under the 16 px floor, and they cannot be made taller
    // from here -- the editor cannot be resized (ur-filed-app.cpp.diff), and at
    // the 54 px lane a taller panel would give it these rows are 16.7 and pass.
    //
    // grabTo16 is the widget layer's own spelling of the floor (uw-WIDGET-API
    // §1.8): exactly enough slop to reach 16 device px, halved because slop
    // grows both sides, and ZERO once the rect already clears it -- so at DPI
    // 1.5, where these rows are 18.7 tall, nothing is padded at all.
    //
    // Only the two SELECTORS get it. The rows are stacked a pixel apart, so a
    // pad on one reaches into its neighbour and the last setHot to test a pixel
    // keeps it; padding the pair sends the shared pixel to the row below, and
    // both of them merely CYCLE a name, so nothing is spent either way. The "+"
    // button under them is deliberately left unpadded -- see there.
    if (ui.grabTo16(r0).selector(uiId(UiRollLaneRow, 0), r0, &shown, ptrs.data(),
                                 (int)ptrs.size())) {
        laneSel_ = shown;
        // The point indices belong to the lane that was on show, so switching
        // lanes drops them rather than carrying a stale set across.
        lane_.detach();
    }
    const int envBase = envLaneBase(clip);
    if (ui.hovered(r0)) {
        if (const AutoLane* l = shownLane(clip)) ui.tip = l->address;
        else if (clip.kind == ClipKind::Midi)
            switch (shownNoteLane()) {
            case NoteLane::Chance:
                ui.tip = "per-note chance - how often each note sounds, rolled every "
                         "time round the loop";
                break;
            case NoteLane::VelRange:
                ui.tip = "per-note velocity range - the far end of the span each "
                         "sounding is drawn from; at the bottom there is no range";
                break;
            default:
                ui.tip = "note velocity";
                break;
            }
    }

    // What "+" would automate, with the button that does it beside the name so
    // the pair reads as one control. A dot marks a target this clip already has
    // a lane for, so the list says where the automation in a set actually is
    // rather than making the user cycle it to find out.
    const Rect addR{r2.x, r2.y, 18.f * s, r2.h};
    const Rect tgtR = r1;
    if (!targets.entries.empty()) {
        std::vector<const char*> tnames;
        tnames.reserve(targets.entries.size());
        for (const AutoTargets::Entry& e : targets.entries) tnames.push_back(e.label.c_str());
        targetSel_ = clampv(targetSel_, 0, (int)tnames.size() - 1);
        ui.grabTo16(tgtR).selector(uiId(UiRollLaneRow, 1), tgtR, &targetSel_,
                                   tnames.data(), (int)tnames.size());
        const AutoTargets::Entry& sel = targets.entries[(size_t)targetSel_];
        if (sel.automated)
            rr.circle(tgtR.right() - 3.5f * s, tgtR.y + 3.5f * s, 1.8f * s, nx::violetSoft);
        if (ui.hovered(tgtR)) ui.tip = sel.group + " " + sel.label + "  " + sel.address;
    } else if (ui.fSmall) {
        rr.textIn(*ui.fSmall, tgtR, "no targets", nx::muted.alpha(0.55f), Align::Center, 0);
    }

    // The toggle is a lit dot rather than a word: at 46 px wide, "OFF" beside a
    // "+" ellipsises, and the dot is the same control the device strip already
    // uses for bypass — a lit plate means the thing is doing something.
    const Rect onR{addR.right() + 2.f * s, r2.y, r2.right() - addR.right() - 2.f * s, r2.h};
    // NO PAD ON "+", and none on the toggle beside it. Slop is claimed by the
    // LAST setHot to test a pixel, so a pad here would let the button that ADDS
    // AN ENVELOPE steal the bottom row of pixels from the target selector above
    // it -- a click aimed at "which parameter" landing on "automate it". The
    // theft has to run the other way: the two harmless cyclers are padded, the
    // consequential button is not, and the pixel they share goes to the cycler.
    // The cost is that this row measures 20.0 x 14.8 device px at DPI 1.0 and
    // fails the 16 px floor on its short side. It cannot be fixed from here:
    // three rows share the lane's height, the lane is 32% of a 151 px editor,
    // and the editor cannot be resized (ur-filed-app.cpp.diff). At the 54 px
    // lane a taller panel would give it, the row is 16.7 and passes.
    if (!targets.entries.empty() && ui.button(uiId(UiRollLaneRow, 2), addR, "+")) {
        const std::string& addr = targets.entries[(size_t)targetSel_].address;
        int found = -1;
        for (size_t i = 0; i < clip.envelopes.size(); ++i)
            if (clip.envelopes[i].address == addr) { found = (int)i; break; }
        if (found >= 0) {
            laneSel_ = envBase + found;       // already there: show it
            lane_.detach();
        } else if ((int)clip.envelopes.size() < kMaxClipLanes) {
            AutoLane l;
            l.address = addr;
            clip.envelopes.push_back(std::move(l));
            laneSel_ = envBase + (int)clip.envelopes.size() - 1;
            lane_.detach();
            changed = true;
            lastEdit_ = kEditAuto;
        }
    }
    // Live's "deactivate envelope": the lane keeps its points and stops driving
    // anything. Only meaningful with a lane on show.
    if (AutoLane* env = shownLane(clip)) {
        AutoLane& l = *env;
        bool on = l.enabled;
        if (ui.squareToggle(uiId(UiRollLaneRow, 3), onR, "", &on, nx::violet)) {
            l.enabled = on;
            changed = true;
            lastEdit_ = kEditAuto;
        }
        // Cyan for a lane that is driving something -- the same live/inert
        // reading the playhead and the meters use.
        rr.circle(onR.cx(), onR.cy(), 3.f * s,
                  l.enabled ? nx::cyan : nx::muted.alpha(0.45f));
        if (ui.hovered(onR))
            ui.tip = l.enabled ? "envelope active - click to deactivate it"
                               : "envelope deactivated - click to put it back in charge";
    }
    rr.popClip();
}

// ---------------------------------------------------------------------------
// PianoRoll
// ---------------------------------------------------------------------------

bool PianoRoll::draw(Ui& ui, const Rect& r, ClipModel& clip, const AutoTargets& targets,
                     const ScaleKey& key, f64 playheadBeats, bool playing) {
    if (!ui.r || !ui.in) return false;
    Renderer& rr = *ui.r;
    Input& in = *ui.in;
    const f32 s = dpiOf(ui);
    bool changed = false;
    // Remembered for the keyboard API, which runs before the NEXT draw and has
    // to snap into the same scale this one is drawing.
    key_ = key;
    const bool snapping = key.snap && key.active();
    // An audio clip has no notes and no pitch axis, but it has envelopes and a
    // time axis — so it gets the same editor with its waveform where the note
    // grid would be. Everything below that is about notes is gated on this.
    const bool midiClip = clip.kind == ClipKind::Midi;

    // The roll is a WELL, like the arrangement and for the same reason: the
    // material the music sits in, recessed below the chrome (docs/DESIGN.md
    // §4). The keyboard column and the ruler are painted back up to panel tone
    // below, so what stays recessed is exactly the working area.
    rr.well(r, 0.f, true);
    if (r.w < 140.f * s || r.h < 70.f * s) return false;     // too small to be useful

    // --- layout ------------------------------------------------------------
    const f32 keyW = kKeyW * s, rowH = kRowH * s;
    const f32 laneH = std::min(kLaneH * s, r.h * 0.32f);
    const Rect ruler{r.x, r.y, r.w, kRulerH * s};
    const Rect body{r.x, ruler.bottom(), r.w, r.h - ruler.h - laneH - 1.f * s};
    const Rect keys{body.x, body.y, keyW, body.h};
    const Rect grid{body.x + keyW, body.y, body.w - keyW, body.h};
    const Rect lane{grid.x, body.bottom() + 1.f * s, grid.w, laneH};
    const Rect laneKey{r.x, lane.y, keyW, laneH};
    if (grid.w < 24.f * s || grid.h < rowH) return false;

    // The caller swaps the clip under us between frames (selecting another
    // slot), and everything the roll remembers — a selection index, where the
    // view sits, how far it is zoomed in — is about one particular clip. So the
    // whole lot resets when the identity changes, and the new clip is shown the
    // way a clip is first shown: fit to the width, nothing selected.
    if (clip.uid != clipUid_) {
        clipUid_ = clip.uid;
        sel_.clear();
        lane_.detach();
        bandBase_.clear();
        dragNote_ = -1;
        drag_ = Drag::None;
        panning_ = false;
        shiftClone_ = false;
        paintPitch_ = -1;
        paintBeat_ = -1.0;
        scrollX_ = scrollY_ = 0.f;
        zoom_ = 0.f;                 // -> fit to width below
        addedLastPress_ = false;
        followSel_ = false;
        preview_.clear();            // an audition for a clip nobody is looking at
        targetSel_ = 0;
        // An audio clip has no per-note stems, so the lane opens on its first
        // envelope rather than on an entry that would draw nothing. A choice
        // made from outside for this clip (showLane) outranks both.
        laneSel_ = pendingLane_ >= 0 ? pendingLane_
                                     : ((!midiClip && !clip.envelopes.empty())
                                            ? envLaneBase(clip) : 0);
        pendingLane_ = -1;
    }

    // A note count can still change under a live selection (undo, a MIDI take
    // finishing), so indices are re-checked on every frame regardless.
    const int noteCount = midiClip ? (int)clip.notes.size() : 0;
    sel_.prune(noteCount);
    if (dragNote_ >= noteCount) {
        dragNote_ = -1;
        // A lane drag holds no note index and is AutoLaneView's anyway, so it is
        // not this check's business.
        drag_ = Drag::None;
    }

    // Same for the lane: a lane and its points can go away under a live
    // selection (undo, a project load), and laneSel_ is an index like any other.
    if (laneSel_ >= envLaneBase(clip) + (int)clip.envelopes.size()) {
        laneSel_ = 0;
        lane_.detach();
    }

    // The lane's key block, drawn (and clicked) FIRST, before anything takes a
    // pointer into clip.envelopes: it can add a lane, and a push_back moves the
    // vector. It sits in its own rect at the bottom left and overlaps nothing,
    // so being early costs it neither hover nor paint order.
    rr.rect(laneKey, tl::panelFill);
    drawLaneKey(ui, laneKey, clip, targets, s, changed);

    AutoLane* const env = shownLane(clip);
    // A lane and its points can go away under a live selection (undo, a project
    // load), and an index is only ever as good as the frame it was made in.
    if (env) lane_.prune((int)env->points.size());
    else     lane_.detach();
    // The target this lane names, and therefore its value axis. A lane whose
    // address the set cannot resolve today (a deleted device, a set opened on
    // another machine) keeps its points and draws greyed against a plain 0..1 —
    // losing a filter sweep because a plugin is missing is the worst bug this
    // feature could have.
    const AutoTargets::Entry* const tgt = env ? targets.find(env->address) : nullptr;
    // `targets.inert` is indexed by ENVELOPE, not by chooser slot, so the note
    // lanes in front of them have to be subtracted off before the bit is read.
    const int envIdx = env ? laneSel_ - envLaneBase(clip) : -1;
    const bool laneInert = env && envIdx >= 0 && envIdx < 32 &&
                           ((targets.inert & (1u << (u32)envIdx)) != 0u);
    // "Switched off" and "therefore drawn greyed" are AutoLaneView's own
    // conclusions now: it is handed `enabled`, `inert` and `resolved` and works
    // out the rest, which is the same three facts this used to fold by hand.

    // --- axes --------------------------------------------------------------
    const int keepPitch = (drag_ == Drag::Move && dragNote_ >= 0) ? dragPitch_ : -1;
    const RowMap rows = buildRows(clip.notes, fold_, key, keepPitch);

    const f64 lenBeats = std::max(1.0, clip.lengthBeats);
    // First sight of a clip: fit the loop to the width, so a pattern opens as
    // itself rather than as a window onto part of itself. From the first
    // Ctrl+wheel on, the zoom is the user's and nothing takes it back — not
    // even dragging the loop longer, which would otherwise re-fit under the
    // hand that was editing.
    if (zoom_ <= 0.f)
        zoom_ = clampv((f32)(grid.w / lenBeats) / s, kPxPerBeatMin, kPxPerBeatMax);
    f32 pxPerBeat = zoom_ * s;

    const u64 gridId = uiId(UiRollSurface, 0), laneId = uiId(UiRollSurface, 1);
    const bool overBody = (grid.contains(in.mx, in.my) || lane.contains(in.mx, in.my) ||
                           keys.contains(in.mx, in.my)) &&
                          rr.currentClip().contains(in.mx, in.my);
    if (overBody && in.wheel != 0.f) {
        if (in.ctrl()) {
            // Zoom about the cursor. Exponential in the wheel so a notch is the
            // same *proportion* everywhere, which is the only way one gesture
            // covers 8..512 px/beat without feeling geared wrong at one end.
            const f32 nz = clampv(zoom_ * std::pow(2.f, in.wheel * kZoomPerNotch),
                                  kZoomMin, kZoomMax);
            if (nz != zoom_) {
                const TimeAxis prev{grid.x, pxPerBeat, scrollX_};
                zoom_ = nz;
                pxPerBeat = zoom_ * s;
                // Off in the keyboard column there is no beat under the cursor,
                // so the left edge of the grid anchors instead.
                scrollX_ = zoomView(prev, pxPerBeat, clampv(in.mx, grid.x, grid.right()),
                                    lenBeats, grid.w);
            }
        } else if (in.shift() || !midiClip) {
            // An audio clip has no pitch axis to scroll, so a plain wheel there
            // means the thing it does have: time.
            scrollX_ -= in.wheel * pxPerBeat * 0.5f;
        } else {
            scrollY_ -= in.wheel * rowH * 3.f;
        }
    }

    // MIDDLE-DRAG PANS, both axes (FL, and every map on the internet). It is
    // handled HERE, with the wheel and before the axes are built, because a pan
    // is a scroll: routing it through `drag_` down in the interaction block
    // would apply this frame's hand movement to next frame's axes and lag the
    // content behind the pointer by a frame at every speed.
    //
    // It starts anywhere over the editor -- grid, lane or keyboard column --
    // because the whole surface is the thing being moved, and the keyboard
    // column is exactly where a hand goes to shove the pitch axis around.
    if (in.pressed[1] && overBody) panning_ = true;
    if (panning_ && !in.down[1])   panning_ = false;
    if (panning_) {
        scrollX_ -= in.dx;
        // The pitch axis only exists for a pattern; on a waveform the vertical
        // half of the gesture has nothing to move and is simply not taken.
        if (midiClip) scrollY_ -= in.dy;
    }

    const f32 contentW = (f32)(lenBeats * (f64)pxPerBeat);
    const f32 contentH = (f32)rows.count * rowH;

    scrollX_ = clampv(scrollX_, 0.f, std::max(0.f, contentW - grid.w));
    // scrollY_ is an offset from the *default* view rather than from the top of
    // the content: 0 then means "centred on C3..C5" (or on the folded rows) with
    // no first-frame flag, which the frozen header has no room for. The clamp is
    // written back so scrolling never has a dead zone at either end.
    // A fold has no C4 to centre on in general (Key folds always contain one of
    // its neighbours but not necessarily it), so a folded axis centres on its
    // own middle row instead.
    const int centreRow = (fold_ != FoldMode::All && rows.count < 128)
                              ? rows.count / 2 : rows.rowOf(kCentrePitch);
    const f32 anchorY = (f32)std::max(0, centreRow) * rowH + rowH * 0.5f - grid.h * 0.5f;
    f32 viewY = clampv(anchorY + scrollY_, 0.f, std::max(0.f, contentH - grid.h));

    // A keyboard edit can push the selected note out of the view — an octave
    // nudge usually does — and a note the user cannot see is a note they have
    // lost. The view follows it, by the smallest move that puts it back on
    // screen (clamping to a window it is already inside is a no-op). It follows
    // the *primary* note: a group can be taller and longer than the view, and
    // chasing all of it would mean choosing which part to lose anyway.
    if (followSel_) {
        followSel_ = false;
        if (sel_.primary >= 0 && sel_.primary < noteCount) {
            const NoteModel& sel = clip.notes[(size_t)sel_.primary];
            const int row = rows.rowOf(sel.pitch);
            if (row >= 0) {
                const f32 top = (f32)row * rowH;
                viewY = clampv(viewY, top + rowH - grid.h, top);
                viewY = clampv(viewY, 0.f, std::max(0.f, contentH - grid.h));
            }
            const f32 nx0 = (f32)(sel.beat * (f64)pxPerBeat);
            const f32 nx1 = (f32)((sel.beat + sel.len) * (f64)pxPerBeat);
            // For a note wider than the view the two bounds cross; either way
            // out of clampv lands on part of the note, which is all that is
            // promised here.
            scrollX_ = clampv(scrollX_, nx1 - grid.w, nx0);
            scrollX_ = clampv(scrollX_, 0.f, std::max(0.f, contentW - grid.w));
        }
    }
    scrollY_ = viewY - anchorY;

    const TimeAxis ta{grid.x, pxPerBeat, scrollX_};
    const PitchAxis pa{grid.y, rowH, viewY};
    // The lane's own value axis is AutoLaneView's, built from the same rect the
    // velocity stems use — so switching the lane from VEL to an envelope changes
    // what is drawn and not where it is drawn.
    const f32 minNoteW = 3.f * s;

    // --- interaction -------------------------------------------------------
    ui.setHot(gridId, grid);
    ui.setHot(laneId, lane);
    const bool hotGrid = ui.isHot(gridId), hotLane = ui.isHot(laneId);

    auto eraseNote = [&](int i) {
        if (i < 0 || i >= (int)clip.notes.size()) return;
        clip.notes.erase(clip.notes.begin() + i);
        sel_.erased(i);
        if (dragNote_ == i) { dragNote_ = -1; drag_ = Drag::None; }
        else if (dragNote_ > i) --dragNote_;
        changed = true;
        lastEdit_ = kEditNote;
    };
    // Note-lane drags are absolute: the stem follows the cursor height, so a
    // plain click on the lane also sets the value, like clicking a fader track.
    // The height is normalized here and turned into whichever field the lane is
    // showing by setNoteLaneValue, so all three lanes share one gesture and one
    // geometry — and a stem can never be drawn where a drag would not put it.
    auto laneT = [&](f32 y) {
        return clampv((lane.bottom() - 2.f * s - y) / std::max(1.f, lane.h - 6.f * s), 0.f, 1.f);
    };
    const NoteLane noteLane = shownNoteLane();
    // Applies the lane's value to the whole selection and reports whether
    // anything moved. Shared by the press and by the drag, so the two cannot
    // disagree about what a click does.
    auto applyLaneValue = [&](f32 t) {
        bool any = false;
        for (int i : sel_.items) {
            if (i < 0 || i >= (int)clip.notes.size()) continue;
            if (setNoteLaneValue(clip.notes[(size_t)i], noteLane, t)) any = true;
        }
        if (any) {
            // Only velocity seeds the "next note added" default; a chance or a
            // range is a per-note decision and inheriting one silently would put
            // dice on notes nobody asked to gamble with.
            if (noteLane == NoteLane::Velocity && sel_.primary >= 0 &&
                sel_.primary < (int)clip.notes.size())
                lastVel_ = clip.notes[(size_t)sel_.primary].vel;
            changed = true;
            lastEdit_ = kEditNote;
        }
        return any;
    };
    // Which note the LANE is pointing at, at this x.
    //
    // It used to be "the nearest stem within kStemGrab", full stop, which made
    // the lane a row of 16 px targets with the rest of it dead: at the zoom a
    // four-beat clip opens at, eight sixteenth-notes gave eight 16 px bands and
    // 48 px of nothing between each pair, and a press in the nothing did
    // nothing and said nothing. A note's stem stands for the NOTE, and the note
    // occupies its own span of the time axis, so that span is the target --
    // which is also what makes sweeping the lane paint the velocities of the
    // notes it passes, the way FL does. The nearest-stem rule stays as the
    // fallback for the gaps between notes, so nothing that used to be grabbable
    // stopped being.
    auto laneNoteAt = [&](f32 mx) -> int {
        const f64 b = xToBeat(ta, mx);
        int found = -1;
        for (size_t i = 0; i < clip.notes.size(); ++i) {
            const NoteModel& nt = clip.notes[i];
            if (b >= nt.beat - 1e-9 && b < nt.beat + nt.len - 1e-9) found = (int)i;
        }
        if (found >= 0) return found;                 // later notes win, as they draw
        int best = -1;
        f32 bestD = kStemGrab * s;
        for (size_t i = 0; i < clip.notes.size(); ++i) {
            const f32 d = std::fabs(mx - beatToX(ta, clip.notes[i].beat));
            if (d <= bestD) { bestD = d; best = (int)i; }
        }
        return best;
    };

    // The rubber band, when one is in flight: computed with the interaction and
    // drawn later, inside the grid's clip. The LANE's band is the lane's own,
    // and is drawn by AutoLaneView.
    Rect bandRect{};
    bool showBand = false;

    // ALT FREES THE SNAP while a note is moved or resized -- FL's modifier, and
    // the documented one here. Shift does it too, because this roll already
    // meant "off the grid" by Shift in places and the brief says to keep both
    // spellings alive; the one exception is a drag that is already a
    // SHIFT-CLONE, where Shift is spoken for and using it twice would mean a
    // clone could never land on the grid.
    const bool freeSnap = in.alt() || (in.shift() && !shiftClone_);

    // The undecided shift/ctrl press on a note, resolved. It is checked before
    // the general drag block because its RELEASE is the interesting half: every
    // other drag ends by simply forgetting itself, and this one has to do the
    // click it was holding in reserve.
    if (drag_ == Drag::Pending) {
        if (dragNote_ < 0 || dragNote_ >= (int)clip.notes.size()) {
            drag_ = Drag::None;
            dragNote_ = -1;
            if (ui.active == gridId) ui.active = 0;
        } else if (!in.down[0]) {
            // Released where it started: the click it always was. Shift toggles
            // membership (this roll's rule, and every other DAW's); Ctrl, which
            // has no click meaning of its own here, selects.
            if (pendShift_) {
                sel_.toggle(dragNote_);
                if (sel_.has(dragNote_)) preview_.push((int)clip.notes[(size_t)dragNote_].pitch);
            } else if (!sel_.has(dragNote_)) {
                sel_.one(dragNote_);
                preview_.push((int)clip.notes[(size_t)dragNote_].pitch);
            } else {
                sel_.primary = dragNote_;
            }
            drag_ = Drag::None;
            dragNote_ = -1;
            if (ui.active == gridId) ui.active = 0;
        } else if (std::fabs(in.mx - pendX_) + std::fabs(in.my - pendY_) > 3.f * s) {
            // Moved: SHIFT+DRAG CLONES (FL), and so does Ctrl+drag. Every
            // selected note is copied where it stands and the COPIES become the
            // selection, so what the hand goes on to drag is the copy and the
            // original stays where it was put.
            if (!sel_.has(dragNote_)) sel_.one(dragNote_);
            const NoteModel anchor = clip.notes[(size_t)dragNote_];
            SelKeys keys;
            keys.notes.reserve(sel_.items.size());
            for (int i : sel_.items) {
                if (i < 0 || i >= (int)clip.notes.size()) continue;
                if (i == dragNote_) keys.primary = (int)keys.notes.size();
                keys.notes.push_back(clip.notes[(size_t)i]);
            }
            if (keys.notes.empty()) {
                drag_ = Drag::None;
                dragNote_ = -1;
            } else {
                for (const NoteModel& c : keys.notes) clip.notes.push_back(c);
                // A copy is identical to its original in every field, so which
                // of the pair the set lands on is a distinction without a
                // difference -- one of them moves, one of them stays, and they
                // were the same note a moment ago.
                sortTrackingSet(clip.notes, keys, sel_);
                dragNote_ = sel_.primary;
                if (dragNote_ < 0) {
                    drag_ = Drag::None;
                } else {
                    // Measured from the PRESS, not from here: the copy has to
                    // sit under the hand the way the original did, or it jumps
                    // by the three pixels that armed the clone.
                    drag_ = Drag::Move;
                    shiftClone_ = pendShift_;
                    dragBeat_ = xToBeat(ta, pendX_) - anchor.beat;
                    dragY_ = pendY_;
                    dragPitch_ = (int)anchor.pitch;
                    changed = true;
                    lastEdit_ = kEditNote;
                }
            }
        }
    } else if (drag_ != Drag::None) {
        // Band is the one drag with nothing under it; Erase is the one held on
        // the RIGHT button.
        const bool needsNote = drag_ == Drag::Move || drag_ == Drag::Resize ||
                               drag_ == Drag::NoteVal;
        const bool held = drag_ == Drag::Erase ? in.down[2] : in.down[0];
        if (!held ||
            (needsNote && (dragNote_ < 0 || dragNote_ >= (int)clip.notes.size()))) {
            drag_ = Drag::None;
            dragNote_ = -1;
            bandBase_.clear();
            shiftClone_ = false;
            paintPitch_ = -1;
            paintBeat_ = -1.0;
            if (ui.active == gridId) ui.active = 0;
        } else if (drag_ == Drag::Erase) {
            // RIGHT-DRAG ERASES: every note the pointer crossed since the last
            // frame, not merely the one it is over now. Back to front is not
            // needed -- eraseNote renumbers what follows and the next sample is
            // a fresh hit test either way.
            sweepSegment(sweepX_, sweepY_, in.mx, in.my, [&](f32 x, f32 y) {
                if (!grid.contains(x, y)) return;
                const int hit = noteAt(clip.notes, rows, ta, pa, x, y, minNoteW);
                if (hit >= 0) eraseNote(hit);
            });
            sweepX_ = in.mx;
            sweepY_ = in.my;
        } else if (drag_ == Drag::Paint) {
            // LEFT-DRAG PAINTS: one note per grid cell the pointer sweeps
            // through, snapped exactly as a written note is and skipping any
            // cell whose pitch is already sounding there. The cell the brush
            // last wrote into is remembered, so holding still writes once.
            sweepSegment(sweepX_, sweepY_, in.mx, in.my, [&](f32 x, f32 y) {
                if (!grid.contains(x, y)) return;
                int pitch = rows.pitchAt(yToRow(pa, y));
                if (pitch < 0) return;
                if (snapping) pitch = key.snapPitch(pitch, 0);
                const f64 b = quantFloor(xToBeat(ta, x));
                if (b < 0.0 || b >= clip.lengthBeats) return;
                if (pitch == paintPitch_ && b == paintBeat_) return;
                paintPitch_ = pitch;
                paintBeat_ = b;
                if (pitchBusyAt(clip.notes, pitch, b)) return;
                NoteModel nn;
                nn.beat = b;
                nn.len = kGridStep;
                nn.pitch = (u8)pitch;
                nn.vel = lastVel_;
                clip.notes.push_back(nn);
                const int idx = sortTracking(clip.notes, nn);
                sel_.one(idx);
                preview_.push(pitch);
                changed = true;
                lastEdit_ = kEditNote;
            });
            sweepX_ = in.mx;
            sweepY_ = in.my;
        } else if (drag_ == Drag::Band) {
            // Live, not on release: the selection is whatever the band touches
            // *now*, so dragging back over a note un-takes it and there is no
            // moment where what is highlighted and what is selected disagree.
            // The anchor is in content space, so a wheel mid-band leaves the
            // corner on the material it was put on rather than on a pixel.
            const f32 ax = beatToX(ta, bandBeat_);
            const f32 ay = grid.y - viewY + bandY_;
            bandRect = Rect{std::min(ax, in.mx), std::min(ay, in.my),
                            std::fabs(in.mx - ax), std::fabs(in.my - ay)};
            showBand = true;
            std::vector<int> hits;
            notesInBand(clip.notes, rows, ta, pa, bandRect, minNoteW, hits);
            // Shift means "and also" here as everywhere, so the band adds to
            // what was selected when it started — which also means a band that
            // touches nothing takes nothing away.
            sel_.adopt(bandBase_);
            for (int i : hits) sel_.add(i);
        } else if (drag_ == Drag::Move) {
            // The note under the hand is always part of what moves. It is put
            // there on the press, but Escape can empty the set from the
            // keyboard between frames while the button is still down, and a
            // drag that silently stopped moving anything would look like a
            // freeze rather than a cancel.
            if (!sel_.has(dragNote_)) sel_.one(dragNote_);
            const NoteModel nt = clip.notes[(size_t)dragNote_];   // copy: we sort below
            // Pitch follows whole rows travelled since the press. Both ends go
            // through the current axis, so wheeling mid-drag can shift the
            // result by at most the one row the sub-row phase moved by, rather
            // than sending the note off with the scroll.
            const int baseRow = rows.rowOf(dragPitch_);
            int np = (int)nt.pitch;
            if (baseRow >= 0) {
                const int row = clampv(baseRow + yToRow(pa, in.my) - yToRow(pa, dragY_),
                                       0, rows.count - 1);
                const int p = rows.pitchAt(row);
                if (p >= 0) np = p;
            }
            // Snapping is applied to the DESTINATION rather than to the delta,
            // so a drag across an out-of-scale row lands on the nearest degree
            // instead of refusing to move. Under FoldMode::Key it is already a
            // no-op — every row on screen is in the scale — which is why the two
            // are independent controls rather than one.
            if (snapping) np = key.snapPitch(np, 0);
            // The gesture is measured on the note under the hand and applied to
            // the whole selection as one delta, so a chord keeps its shape and
            // the group stops when its extreme member reaches a wall. With one
            // note selected this is the old clampBeat/pitch clamp exactly.
            // Alt (FL) or Shift hands the beat back to the pointer, to the
            // pixel. Only the TIME half: the pitch axis is rows and there is
            // nothing between two of them to be freed onto.
            const f64 wantBeat = xToBeat(ta, in.mx) - dragBeat_;
            const GroupDelta d = clampGroupDelta(
                clip.notes, sel_.items,
                (freeSnap ? wantBeat : quantNear(wantBeat)) - nt.beat,
                np - (int)nt.pitch, clip.lengthBeats);
            if (d.beats != 0.0 || d.semis != 0) {
                // Dragging across rows plays what is under the note, the way a
                // note dragged in Live does: the pitch is the thing being
                // chosen, and choosing it by eye alone is guesswork. Only the
                // note under the hand is auditioned — thirty at once is noise,
                // not a chord.
                if (d.semis != 0) preview_.push((int)nt.pitch + d.semis);
                // The key is passed as-is: applyGroupDelta re-snaps every moved
                // note, which for the note under the hand is a second, idempotent
                // application of the snap above and for the rest of a group is
                // the only one they get.
                if (applyGroupDelta(clip.notes, sel_, d, key)) { changed = true; lastEdit_ = kEditNote; }
                dragNote_ = sel_.primary;
                if (dragNote_ < 0) drag_ = Drag::None;
            }
        } else if (drag_ == Drag::Resize) {
            // Length stays a one-note edit: a group resize has to choose
            // between absolute and proportional lengths, and neither is what
            // the hand on one note's right edge asked for.
            NoteModel& nt = clip.notes[(size_t)dragNote_];
            const f64 nl = clampLen(xToBeat(ta, in.mx), nt.beat, clip.lengthBeats,
                                    !freeSnap);
            if (nl != nt.len) { nt.len = nl; changed = true; lastEdit_ = kEditNote; }
        } else if (drag_ == Drag::NoteVal) {
            // Absolute, and for the whole selection: every selected stem takes
            // the value under the cursor. Relative (each stem keeping its
            // offset from the one being dragged) is the other defensible
            // answer, but absolute is what a group drag does in Live and it is
            // the one a user can aim. None of the three fields reorders the
            // vector, so no re-sort.
            //
            // ...and it PAINTS while it sweeps, which is the FL half. A drag
            // that keeps re-applying to the note it started on while the
            // pointer is three stems further along is editing something the
            // hand is no longer pointing at -- so the drag simply keeps doing
            // what the press did, note by note, as it crosses them.
            //
            // Only while the selection is one note or none. A MULTI-note
            // selection is a deliberate group edit ("flatten these eight to
            // here") and re-targeting mid-sweep would collapse it the moment
            // the pointer wandered off its last member, which is the one thing
            // a group drag must never do.
            if (sel_.count() <= 1) {
                const int over = laneNoteAt(in.mx);
                if (over >= 0 && over != dragNote_) {
                    sel_.one(over);
                    dragNote_ = over;
                }
            }
            if (!sel_.has(dragNote_)) sel_.one(dragNote_);
            applyLaneValue(laneT(in.my));
        }
    } else if (hotGrid && midiClip && (in.pressed[0] || in.pressed[2])) {
        const int hit = noteAt(clip.notes, rows, ta, pa, in.mx, in.my, minNoteW);
        // Clicking empty space adds, so without this the second click of a
        // double-click on empty space would delete what the first click made.
        const bool prevAdded = addedLastPress_;
        addedLastPress_ = false;

        if (in.pressed[2]) {
            // RIGHT-CLICK ERASES, and the press ARMS THE SWEEP whether or not
            // there was a note under it: in FL the eraser is a brush, and
            // starting it on empty space and dragging into a run of notes is
            // how the gesture is actually made.
            if (hit >= 0) eraseNote(hit);
            drag_ = Drag::Erase;
            dragNote_ = -1;
            sweepX_ = in.mx;
            sweepY_ = in.my;
        } else if (hit >= 0 && in.dblClick && !prevAdded) {
            eraseNote(hit);                               // double-click deletes
        } else if (hit >= 0 && (in.shift() || in.ctrl())) {
            // Ambiguous until it moves: a click toggles membership (Shift) or
            // selects (Ctrl); a drag clones. Nothing is decided here -- see the
            // Drag::Pending block above, which is where both endings live.
            drag_ = Drag::Pending;
            dragNote_ = hit;
            pendShift_ = in.shift();
            pendX_ = in.mx;
            pendY_ = in.my;
            ui.active = gridId;
        } else if (hit >= 0) {
            // A plain click on a note that is already part of a multi-selection
            // keeps the set, so the same press can start a group drag; on
            // anything else it reduces the selection to that one note. Standard
            // DAW behaviour, and the reason clicking inside a chord to move it
            // does not scatter the chord first.
            if (!sel_.has(hit)) sel_.one(hit);
            else              sel_.primary = hit;
            ui.active = gridId;
            const NoteModel& nt = clip.notes[(size_t)hit];
            preview_.push((int)nt.pitch);          // clicking a note plays it
            f32 x0 = 0.f, x1 = 0.f;
            noteSpanX(nt, ta, minNoteW, x0, x1);
            const f32 edge = std::min(kNoteEdgeGrab * s, (x1 - x0) * kNoteEdgeShare);
            dragNote_ = hit;
            dragY_ = in.my;
            dragPitch_ = (int)nt.pitch;
            if (in.mx >= x1 - edge) { drag_ = Drag::Resize; dragBeat_ = 0.0; }
            else { drag_ = Drag::Move; dragBeat_ = xToBeat(ta, in.mx) - nt.beat; }
        } else if (in.shift() || in.ctrl()) {
            // Rubber band. Plain empty-drag PAINTS notes now, so the band is
            // what a modifier buys on empty space: Shift, which is what this
            // roll has always used, and Ctrl, which is where FL puts its
            // marquee -- both, because a hand that reaches for either is
            // reaching for the same thing. The anchor is kept in content space
            // so scrolling or zooming mid-band does not drag the corner with it.
            drag_ = Drag::Band;
            dragNote_ = -1;
            bandBeat_ = xToBeat(ta, in.mx);
            bandY_ = in.my - grid.y + viewY;
            bandBase_ = sel_.items;
            ui.active = gridId;
        } else {
            int pitch = rows.pitchAt(yToRow(pa, in.my));
            // A note WRITTEN into the grid is snapped too, not just one dragged
            // there: the whole promise of a key with snapping on is that every
            // note you make is in it.
            if (pitch >= 0 && snapping) pitch = key.snapPitch(pitch, 0);
            const f64 b = quantFloor(xToBeat(ta, in.mx));
            if (pitch >= 0 && b >= 0.0 && b < clip.lengthBeats) {
                NoteModel nn;
                nn.beat = b;
                nn.len = kGridStep;
                nn.pitch = (u8)pitch;
                nn.vel = lastVel_;
                clip.notes.push_back(nn);
                const int idx = sortTracking(clip.notes, nn);
                sel_.one(idx);                   // a fresh note is the selection
                dragNote_ = idx;
                // THE BRUSH IS DOWN. Dragging on from here writes a note into
                // every further cell the pointer sweeps -- FL's paint. This is
                // where press-drag-add used to grab the fresh note and let you
                // place it; the two gestures want the same pixels and painting
                // is the one the owner's hands know. Placing is still a click
                // and then a drag, which it always was.
                drag_ = Drag::Paint;
                sweepX_ = in.mx;
                sweepY_ = in.my;
                paintPitch_ = pitch;
                paintBeat_ = b;
                dragBeat_ = xToBeat(ta, in.mx) - nn.beat;
                dragY_ = in.my;
                dragPitch_ = pitch;
                addedLastPress_ = true;
                preview_.push(pitch);            // hear what was just written
                ui.active = gridId;
                changed = true;
                lastEdit_ = kEditNote;
            }
        }
    } else if (hotLane && !env && midiClip && in.pressed[0]) {
        // The note this x belongs to: its own span first, the nearest stem
        // after. See laneNoteAt.
        const int best = laneNoteAt(in.mx);
        if (best >= 0) {
            // A stem that belongs to the current selection drags the whole set
            // (the same rule the grid uses); any other stem takes the selection
            // over first, so the lane can never edit a note the user cannot see
            // they picked.
            if (!sel_.has(best)) sel_.one(best);
            else                 sel_.primary = best;
            dragNote_ = best;
            drag_ = Drag::NoteVal;
            // The lane deliberately does not audition: a value drag would
            // retrigger the note on every pixel, and the value being edited is
            // not the pitch anyway.
            addedLastPress_ = false;
            ui.active = laneId;
            applyLaneValue(laneT(in.my));
        }
    }

    const Col base = pal::clipColors[((clip.colorIdx % pal::clipColorCount) + pal::clipColorCount) %
                                     pal::clipColorCount];
    const int firstRow = std::max(0, yToRow(pa, grid.y));
    const int lastRow  = std::min(rows.count - 1, yToRow(pa, grid.bottom()));

    // --- ruler -------------------------------------------------------------
    // The boundary between chrome and work: panel-toned, hairline-underlined.
    rr.rect(ruler, tl::panelFill);
    rr.hairlineH(ruler.x, ruler.right(), ruler.bottom() - 1.f * s);

    const Rect lenBox{ruler.right() - 70.f * s, ruler.y + 2.f * s, 66.f * s, ruler.h - 4.f * s};
    if (ui.fSmall) {
        rr.pushClip({grid.x, ruler.y, std::max(0.f, std::min(grid.right(), lenBox.x) - grid.x), ruler.h});
        // The shared ruler (timeaxis.h). Same loop, same numbers, same colours —
        // now also the arrangement's, which is the point of the extraction.
        drawRulerLabels(rr, *ui.fSmall, ta, grid.x, grid.right(),
                        ruler.y + (ruler.h - ui.fSmall->height()) * 0.5f, s,
                        tl::rulerOnBar, tl::rulerOffBar);
        rr.popClip();
    }
    const Rect foldBox{ruler.x + 3.f * s, ruler.y + 2.f * s, keyW - 6.f * s, ruler.h - 4.f * s};
    if (midiClip) {
        // Three states, so a selector rather than the toggle this used to be:
        // click cycles ALL -> FOLD -> KEY, right-click cycles back, which is the
        // idiom every other multi-state control in the program uses. KEY is
        // offered even with no scale set -- it falls back to ALL and the tooltip
        // says why, which is more discoverable than a control that is not there.
        int fm = (int)fold_;
        // Both of the ruler's controls are ruler.h - 4 = 12 logical px tall,
        // which is under the 16 px floor at every scale and cannot grow: the
        // ruler is 16 px and they are already nearly all of it. They get the
        // 3 px of aim on every side instead, into the ruler's own margin and
        // the grid line under it -- neither of which is a target.
        ui.grab(3.f * s);
        if (ui.selector(uiId(UiRollFold, 0), foldBox, &fm, kFoldModeNames, kFoldModeCount))
            fold_ = (FoldMode)clampv(fm, 0, kFoldModeCount - 1);
        if (ui.hovered(foldBox.inset(-3.f * s)))
            ui.tip = fold_ == FoldMode::Key
                         ? (key.active() ? "showing only the rows in " + key.label()
                                         : std::string("fold to key - the set has no scale set, "
                                                       "so every row is shown"))
                         : (fold_ == FoldMode::Used ? std::string("showing only the pitches this "
                                                                  "clip uses")
                                                    : std::string("showing every pitch"));
        // Whole beats only: a loop length between beats is a tempo problem.
        if (ui.hovered(lenBox.inset(-3.f * s)) && ui.tip.empty())
            ui.tip = "drag up or down to set the clip's loop length in beats; "
                     "Shift drags finer";
        ui.grab(3.f * s);
        if (ui.dragNumber(uiId(UiRollLength, 0), lenBox, &clip.lengthBeats, 1.0, 512.0, 0.06, "%.0f beats",
                          Align::Right, nullptr, 1.0)) {
            changed = true;
            lastEdit_ = kEditNote;
        }
    } else if (ui.fSmall) {
        // An audio clip has no pitches to fold and its length belongs to the
        // sample and the warp settings on the panel beside this, not to a drag
        // in here — so both read out instead of editing.
        tl::microLabel(rr, *ui.fSmall, foldBox.x + 4.f * s,
                       foldBox.y + (foldBox.h - ui.fSmall->height()) * 0.5f,
                       "wave", nx::muted.alpha(0.70f), foldBox.w - 6.f * s);
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.0f beats", clip.lengthBeats);
        rr.textIn(*ui.fSmall, lenBox, buf, nx::muted.alpha(0.60f), Align::Right, 0);
    }

    // --- keyboard column ---------------------------------------------------
    rr.pushClip(keys);
    // Chrome-adjacent: the keyboard is the frame you read a pitch off, not the
    // surface you work on, so it takes the panel treatment and is separated
    // from the roll by a hairline rather than by a rule.
    rr.rect(keys, tl::panelFill);
    // With a scale on, the keyboard column says which key we are in the way a
    // keyboard does: the notes of the scale keep their plate, everything else is
    // pushed back into the background, and the ROOT is lit. That is the same
    // information the grid rows carry, drawn twice on purpose -- the column is
    // where the eye goes to find a pitch and the grid is where the hand goes to
    // place one.
    const bool showKey = key.active();
    for (int i = firstRow; i <= lastRow && midiClip; ++i) {
        const int p = rows.pitchAt(i);
        if (p < 0) continue;
        const Rect kr{keys.x, rowToY(pa, i), keys.w, rowH};
        const bool inKey = !showKey || key.contains(p);
        // Re-derived from the NX palette: the plates are --panel and --panel-2,
        // the key lift is violet-family, and an out-of-scale row drops to the
        // field rather than turning grey. Recession is the language.
        Col kc = isBlackKey(p) ? tl::panelFill : tl::panelAlt;
        if (!inKey)                        kc = nx::bgTop.alpha(0.74f);
        else if (showKey && key.isRoot(p)) kc = kc.mix(nx::violet, 0.44f);
        else if (showKey)                  kc = kc.mix(nx::violetSoft, 0.10f);
        rr.rect(kr, kc);
        rr.hairlineH(kr.x, kr.right(), kr.bottom() - 1.f * s,
                     nx::hairlineInk.alpha(0.09f));
        // The octave label is C every twelve semitones; with a scale on, the
        // ROOT is labelled too, because "which row is the tonic" is the one
        // question a key is meant to answer at a glance.
        if (ui.fSmall && (p % 12 == 0 || (showKey && key.isRoot(p)))) {
            char buf[16];
            if (p % 12 == 0) std::snprintf(buf, sizeof buf, "C%d", p / 12 - 1);
            else             std::snprintf(buf, sizeof buf, "%s%d",
                                           kPitchNames[p % 12], p / 12 - 1);
            rr.textIn(*ui.fSmall, kr, buf,
                      (showKey && key.isRoot(p)) ? nx::text : nx::muted, Align::Left, 5.f * s);
        }
    }
    if (hotGrid && midiClip) {
        const int hr = yToRow(pa, in.my);
        if (hr >= 0 && hr < rows.count)
            rr.rect({keys.x, rowToY(pa, hr), keys.w, rowH}, nx::violet.alpha(0.18f));
    }
    if (!midiClip && ui.fSmall) {
        // The column stays: it is what the lane's key block hangs under, and an
        // empty gutter beside a waveform reads as a missing piece.
        tl::microLabel(rr, *ui.fSmall, keys.x + 6.f * s, keys.y + 4.f * s, "audio",
                       nx::muted.alpha(0.70f), keys.w - 8.f * s);
        if (clip.sample) {
            char buf[24];
            std::snprintf(buf, sizeof buf, "%d ch", clip.sample->channels);
            rr.textIn(*ui.fSmall, {keys.x, keys.y + 16.f * s, keys.w, 12.f * s}, buf,
                      nx::muted.alpha(0.55f), Align::Center, 0);
        }
    }
    rr.hairlineV(keys.right() - 1.f * s, keys.y, keys.bottom());
    rr.popClip();

    // --- grid: note rows for a pattern, the waveform for a sample -----------
    rr.pushClip(grid);
    // An audio clip's canvas is the same well the note grid sits in, with the
    // same faint lift a white-key row gets -- one surface family, so a waveform
    // and a pattern are read against the same material.
    if (!midiClip) rr.rect(grid, tl::stripeLift);
    for (int i = firstRow; i <= lastRow && midiClip; ++i) {
        const int p = rows.pitchAt(i);
        if (p < 0) continue;
        const f32 y = rowToY(pa, i);
        // The key, as row brightness. In-scale rows are lifted and the root is
        // lifted further; out-of-scale rows are pushed down to the panel
        // background so they read as "not here" without being invisible -- a
        // note already sitting on one must still be findable and draggable, which
        // is the difference between a highlight and a fold.
        // The row banding is a LIFT on the well, so the canvas stays one
        // material: a black-key row is the bare well and a white-key row is the
        // same well with the stripe on it. The key tint is violet-family and
        // re-derived from the NX palette; an out-of-scale row recesses instead
        // of greying, which is what keeps a note sitting on one findable.
        if (!isBlackKey(p)) rr.rect({grid.x, y, grid.w, rowH}, tl::stripeLift);
        if (showKey) {
            if (key.isRoot(p))
                rr.rect({grid.x, y, grid.w, rowH}, nx::violet.alpha(0.26f));
            else if (key.contains(p))
                rr.rect({grid.x, y, grid.w, rowH}, nx::violetSoft.alpha(0.055f));
            else
                rr.rect({grid.x, y, grid.w, rowH}, tl::deadZone.alpha(0.34f));
        }
        // One separator per octave keeps the eye anchored without banding; with
        // a scale on it moves to the root, which is where an octave of THIS
        // piece actually begins.
        const bool divide = showKey ? key.isRoot(p) : (p % 12 == 0);
        if (divide) rr.hairlineH(grid.x, grid.right(), y + rowH - 1.f * s);
    }
    drawTimeGrid(rr, ta, grid, s);          // the shared grid (timeaxis.h)
    {   // Past the loop length is not editable, so dim it like Live does.
        const f32 endX = beatToX(ta, clip.lengthBeats);
        if (endX < grid.right()) {
            const f32 x = std::max(grid.x, endX);
            rr.rect({x, grid.y, grid.right() - x, grid.h}, tl::deadZone);
            rr.hairlineV(x, grid.y, grid.bottom(), nx::violetSoft.alpha(0.22f));
        }
    }
    if (!midiClip && clip.sample && clip.sample->peakBuckets > 0) {
        // The sample, drawn against the SAME time axis the lane below uses, so
        // a breakpoint can be put on a transient and stay on it through every
        // zoom and scroll. One column per pixel, like App::drawWaveform — the
        // peak buckets are what makes that cheap.
        const SampleBuffer& sb = *clip.sample;
        const f32 mid = grid.cy(), halfH = grid.h * 0.5f - 3.f * s;
        const Col wc = base.scale(0.85f);
        for (f32 x = grid.x; x < grid.right(); x += 1.f) {
            const f64 u = xToBeat(ta, x) / lenBeats;
            if (u < 0.0 || u >= 1.0) continue;
            const int bk = clampv((int)(u * (f64)sb.peakBuckets), 0, sb.peakBuckets - 1);
            const f32 lo = sb.peaks[(size_t)bk * 2 + 0];
            const f32 hi = sb.peaks[(size_t)bk * 2 + 1];
            const f32 y0 = mid - hi * halfH;
            rr.rect({x, y0, 1.f, std::max(1.f, (mid - lo * halfH) - y0)}, wc);
        }
    }
    for (size_t i = 0; midiClip && i < clip.notes.size(); ++i) {
        const NoteModel& nt = clip.notes[i];
        const int row = rows.rowOf(nt.pitch);
        if (row < firstRow || row > lastRow) continue;
        f32 x0 = 0.f, x1 = 0.f;
        noteSpanX(nt, ta, minNoteW, x0, x1);
        if (x1 < grid.x || x0 > grid.right()) continue;
        const Rect nr{x0, rowToY(pa, row) + 1.f * s, std::max(minNoteW, x1 - x0 - 1.f * s), rowH - 2.f * s};
        // Velocity reads as brightness, so a part's dynamics are visible in the
        // note block itself and not only down in the lane.
        const Col nc = base.scale(0.55f + 0.45f * (f32)nt.vel / 127.f);
        // A note that may not sound is drawn FADED, and the fraction of it that
        // is drawn solid is its chance. Both at once, deliberately: the fade is
        // what the eye picks up while scanning a pattern ("something in here is
        // uncertain") and the solid fraction is what it reads once it looks
        // ("about a third of the time"), and neither on its own does both jobs.
        if (nt.chance < 100) {
            // MUTED (§1: halve anything that shouts). The ghost is quieter than
            // it was, because the solid fraction beside it is the part that
            // carries the number and two loud marks compete.
            rr.roundRect(nr, 2.f * s, nc.alpha(0.22f));
            const f32 fw = nr.w * ((f32)nt.chance / 100.f);
            if (fw > 0.5f) rr.roundRect({nr.x, nr.y, fw, nr.h}, 2.f * s, nc);
        } else {
            rr.roundRect(nr, 2.f * s, nc);
        }
        // The 1px darkened edge. Crisp beats pretty here: without it two notes
        // that touch are one long note, which is a reading error, not a taste
        // one. Skipped under 4 px, where an edge would be the whole note.
        if (nr.w > 4.f * s)
            rr.roundRectOutline(nr, 2.f * s, std::max(1.f, s), base.scale(0.28f));
        // A velocity range is a band, not a level, so it is drawn as one: a
        // hairline across the block. It is deliberately quiet -- a range changes
        // how a note feels, not whether it happens.
        if (nt.velTo != 0 && nt.velTo != nt.vel && nr.h > 4.f * s)
            rr.rect({nr.x + 1.f * s, nr.cy() - 0.5f * s, std::max(1.f, nr.w - 2.f * s), 1.f * s},
                    nx::inkOn(nc).alpha(0.50f));
        if (sel_.has((int)i))
            rr.roundRectOutline(nr, 2.f * s, std::max(1.f, s), nx::violetSoft);
    }
    // The band goes over the notes it is taking, translucent enough to leave
    // them readable underneath.
    if (showBand) {
        rr.rect(bandRect, nx::violet.alpha(0.12f));
        rr.roundRectOutline(bandRect, 0.f, std::max(1.f, s), nx::violetSoft);
    }
    if (playing) {
        const f32 px = beatToX(ta, playheadBeats);
        if (px >= grid.x && px <= grid.right())
            tl::drawPlayhead(rr, px, grid.y, grid.h, s, true);
    }
    rr.popClip();

    // --- the lane: velocity stems or one envelope ---------------------------
    // A well of its own, one step deeper than the grid: the stems are read
    // against a floor, and a floor has to look like one.
    rr.hairlineH(r.x, r.right(), body.bottom());
    rr.well(lane, 0.f, true);
    rr.pushClip(lane);
    if (env) {
        // The lane, which is now a component and no longer a slice of this
        // function. It is handed the roll's OWN TimeAxis -- the whole property
        // the lane lived in here to preserve, now stated as a parameter -- and
        // the plain view of what its address names. Everything else about it,
        // including its selection and its two drags, is its own.
        //
        // Its interaction runs here rather than up in the interaction block,
        // which is the one ordering the move changes: nothing drawn between the
        // two points reads a breakpoint, and the frame still sees this frame's
        // edit.
        if (lane_.draw(ui, lane, env->points, ta, tgt ? tgt->lo : 0.f,
                       tgt ? tgt->hi : 1.f, tgt ? tgt->unit.c_str() : nullptr,
                       tgt ? tgt->def : 0.f, env->enabled, laneInert,
                       clip.lengthBeats, 0.0, tgt != nullptr,
                       targets.inertWhy.empty() ? nullptr : targets.inertWhy.c_str())) {
            changed = true;
            lastEdit_ = kEditAuto;
        }
    } else if (midiClip) {
        const f32 travel = std::max(1.f, lane.h - 6.f * s);
        const f32 foot = lane.bottom() - 2.f * s;
        for (size_t i = 0; i < clip.notes.size(); ++i) {
            const NoteModel& nt = clip.notes[i];
            const f32 x = std::round(beatToX(ta, nt.beat));
            if (x < lane.x - 2.f * s || x > lane.right()) continue;
            const f32 top = foot - noteLaneValue(nt, noteLane) * travel;
            // Every selected stem is accented, since a lane drag moves all of
            // them; the primary keeps the full accent so the note the gesture
            // is anchored on is still findable inside a large selection.
            const Col c = !sel_.has((int)i)        ? base.scale(0.8f)
                          : (int)i == sel_.primary ? nx::violetSoft
                                                   : nx::violet.mix(nx::violetSoft, 0.45f);
            // In the RANGE lane the note's own velocity is drawn behind the
            // stem, because a range is meaningless without the value it ranges
            // FROM: the pair of marks is the span the engine will draw from, and
            // a range stem alone would be a number with no unit.
            if (noteLane == NoteLane::VelRange) {
                const f32 vy = foot - (f32)nt.vel / 127.f * travel;
                rr.rect({x, std::min(vy, top), std::max(1.f, 1.f * s),
                         std::fabs(top - vy)}, base.scale(0.34f));
                rr.circle(x + 0.5f * s, vy, 1.8f * s, base.scale(0.55f));
                if (nt.velTo == 0) continue;      // no range: nothing to draw
            }
            rr.rect({x, top, std::max(1.f, 1.f * s), foot - top}, c);
            rr.circle(x + 0.5f * s, top, 2.5f * s, c);
        }
    } else if (ui.fSmall) {
        rr.textIn(*ui.fSmall, lane, "pick a target and press + to automate it",
                  nx::muted.alpha(0.60f), Align::Center);
    }
    if (playing) {
        const f32 px = beatToX(ta, playheadBeats);
        if (px >= lane.x && px <= lane.right())
            tl::drawPlayhead(rr, px, lane.y, lane.h, s, true);
    }
    rr.popClip();
    rr.hairlineH(lane.x, lane.right(), lane.y);

    // --- cursor ------------------------------------------------------------
    // The lane REPORTS rather than sets, so its answer keeps the place it had
    // in this ordered block instead of overtaking a grid drag that outranks it.
    if (panning_)                     ui.cursor = Cursor::Grab;
    else if (drag_ == Drag::Resize)   ui.cursor = Cursor::ResizeH;
    else if (drag_ == Drag::Move)     ui.cursor = Cursor::Grab;
    else if (drag_ == Drag::Paint)    ui.cursor = Cursor::Hand;
    else if (drag_ == Drag::Erase)    ui.cursor = Cursor::Hand;
    else if (lane_.dragging())        ui.cursor = Cursor::Grab;
    else if (drag_ == Drag::NoteVal)  ui.cursor = Cursor::ResizeV;
    else if (hotLane && env)          ui.cursor = lane_.pointHovered() ? Cursor::Grab
                                                                      : Cursor::Hand;
    else if (hotLane)                 ui.cursor = Cursor::ResizeV;
    else if (hotGrid && midiClip) {
        const int hover = noteAt(clip.notes, rows, ta, pa, in.mx, in.my, minNoteW);
        if (hover >= 0) {
            const NoteModel& nt = clip.notes[(size_t)hover];
            const f32 x0 = beatToX(ta, nt.beat);
            const f32 x1 = std::max(beatToX(ta, nt.beat + nt.len), x0 + minNoteW);
            ui.cursor = (in.mx >= x1 - std::min(kNoteEdgeGrab * s,
                                                (x1 - x0) * kNoteEdgeShare))
                            ? Cursor::ResizeH
                            : Cursor::Grab;
        }
    }

    // --- the badge ---------------------------------------------------------
    // Same block, same order, and after the cursor for the same reason: the two
    // are one answer and a lane hover must not outrank a grid drag.
    //
    // The rule is what keeps this from being clutter. Three places in this
    // editor cannot say what a click does, and they are exactly the three that
    // look like read-outs: EMPTY grid (a click writes a note), the PER-NOTE
    // lane (a click sets a velocity on the selected notes, from a strip that
    // looks like a bar chart of them), and an ENVELOPE lane (AutoLaneView says
    // that one itself). A note under the pointer gets nothing -- there is a
    // block there and a Grab cursor over it -- and neither does a drag in
    // flight, which has already answered the question by happening.
    if (drag_ == Drag::None && !lane_.dragging()) {
        if (hotGrid && midiClip &&
            noteAt(clip.notes, rows, ta, pa, in.mx, in.my, minNoteW) < 0) {
            ui.badge = Badge::Add;
            if (ui.tip.empty())
                ui.tip = "drag to paint notes; right-drag erases; "
                         "Shift or Ctrl+drag selects a block";
        } else if (hotLane && !env && midiClip) {
            // The lane's own refusal, said rather than performed. A press in a
            // gap between two notes has nothing to set and used to do nothing
            // at all -- no mark, no message, no reason.
            const bool onNote = laneNoteAt(in.mx) >= 0;
            ui.badge = onNote ? Badge::Draw : Badge::None;
            if (ui.tip.empty())
                ui.tip = onNote
                             ? std::string("drag to paint ") +
                                   kNoteLaneNames[(int)noteLane] +
                                   " across the notes you sweep; with several "
                                   "selected they all take the height instead"
                             : std::string("no note here - each stem stands under "
                                           "a note in the grid above");
        }
    }
    if (probeOn() && changed) {
        LOGI("NXTAKT_DEBUG_PROBE: roll uid=%llu notes=%zu len=%.4f lane=%d sel=%d/%d",
             (unsigned long long)clip.uid, clip.notes.size(), clip.lengthBeats,
             laneSel_, sel_.count(), sel_.primary);
        for (size_t i = 0; i < clip.notes.size(); ++i) {
            const NoteModel& n = clip.notes[i];
            LOGI("NXTAKT_DEBUG_PROBE: note i=%zu beat=%.4f pitch=%d len=%.4f vel=%d "
                 "velTo=%d chance=%d%s",
                 i, n.beat, (int)n.pitch, n.len, (int)n.vel, (int)n.velTo,
                 (int)n.chance, sel_.has((int)i) ? " SEL" : "");
        }
        if (const AutoLane* e = shownLane(clip))
            for (size_t p = 0; p < e->points.size(); ++p)
                LOGI("NXTAKT_DEBUG_PROBE: rollauto p=%zu beat=%.4f val=%.5f",
                     p, e->points[p].beat, (double)e->points[p].value);
    }
    return changed;
}

// ---------------------------------------------------------------------------
// keyboard API
//
// These run from App::handleShortcuts, i.e. *before* this frame's draw(), and
// they act on the state the last draw left behind. Hence the identity check on
// every one of them: the clip in front of the roll can have been swapped since,
// and an index into the wrong clip's notes is an edit to the wrong note.
// ---------------------------------------------------------------------------

// A set of any size answers yes, and one index out of range condemns the lot:
// the set is only ever rebuilt as a whole, so a stale member means the clip
// changed under it and nothing in it can be trusted. (sel_ is sorted, so the
// last element is the only one that has to be checked.)
// True for a note selection OR a breakpoint selection: the caller asks one
// question ("does the editor own this key?") and the answer has to cover both
// halves of the editor, or Delete in the lane would clear the whole clip.
bool PianoRoll::hasSelection(const ClipModel& clip) const {
    if (!owns(clip)) return false;
    if (!sel_.empty() && sel_.items.back() < (int)clip.notes.size()) return true;
    const AutoLane* env = shownLane(clip);
    return env && lane_.hasSelection((int)env->points.size());
}

bool PianoRoll::clearSelection() {
    const bool any = !sel_.empty() || lane_.hasSelection();
    if (!any) return false;
    sel_.clear();
    lane_.clearSelection();
    return true;
}

// The lane takes the keys while it has a selection of its own: the arrows then
// nudge breakpoints by one grid step and one kValueNudge of the target's range
// instead of nudging notes, which is what the hand that just clicked a
// breakpoint means by "left".
bool PianoRoll::nudgeSelected(ClipModel& clip, int gridSteps, int semitones) {
    if (!owns(clip)) return false;
    AutoLane* env = shownLane(clip);
    if (env && lane_.hasSelection((int)env->points.size())) {
        if (!lane_.nudgeSelected(env->points, gridSteps, (f32)semitones, clip.lengthBeats))
            return false;
        lastEdit_ = kEditAuto;
        return true;
    }
    if (!hasSelection(clip) || sel_.empty()) return false;
    // key_ is what the last draw was handed. The alternative -- taking a key
    // parameter here -- would put the burden on App::handleShortcuts to know
    // which session the roll in front of it belongs to, and the roll already
    // refuses to act on a clip it has not drawn (owns()), so it is exactly as
    // fresh as every other piece of state these entry points read.
    const NudgeResult res = nudgeGroup(clip.notes, sel_, gridSteps, semitones,
                                       clip.lengthBeats, key_);
    if (!res.changed) return false;                // already against a clamp
    followSel_ = true;
    // One audition for the group: the primary note. A held arrow key on a
    // thirty-note chord would otherwise be a wall of retriggers.
    if (res.pitchChanged && sel_.primary >= 0 && sel_.primary < (int)clip.notes.size())
        preview_.push((int)clip.notes[(size_t)sel_.primary].pitch);
    lastEdit_ = kEditNote;
    return true;
}

bool PianoRoll::deleteSelected(ClipModel& clip) {
    if (!owns(clip)) return false;
    // Back to front in both cases: an index into a vector survives only until
    // something earlier than it is removed, and both sets are sorted.
    AutoLane* env = shownLane(clip);
    if (env && lane_.deleteSelected(env->points)) {
        lastEdit_ = kEditAuto;
        return true;
    }
    if (!hasSelection(clip) || sel_.empty()) return false;
    for (size_t k = sel_.items.size(); k-- > 0;) {
        const int i = sel_.items[k];
        if (i >= 0 && i < (int)clip.notes.size())
            clip.notes.erase(clip.notes.begin() + i);
    }
    sel_.clear();
    // A drag cannot be in flight (this arrives from the keyboard), but the
    // index would be stale if one ever were.
    dragNote_ = -1;
    drag_ = Drag::None;
    lastEdit_ = kEditNote;
    return true;
}

// ---------------------------------------------------------------------------
// note tools
//
// The four of them share one question -- which notes am I about to change? --
// and one answer, so it is written once here. A tool with a selection acts on
// it; a tool without one acts on the whole clip, which is what makes any of
// them useful straight after a MIDI take.
//
// `owns(clip)` gates only the SELECTION, not the tool: the roll's index set is
// meaningless for a clip it has not drawn, but "every note in this clip" is a
// perfectly good target for one it has never seen.
// ---------------------------------------------------------------------------

std::vector<int> PianoRoll::toolTargets(const ClipModel& clip) const {
    std::vector<int> out;
    const int n = (int)clip.notes.size();
    if (owns(clip) && !sel_.empty() && sel_.items.back() < n) {
        for (int i : sel_.items) if (i >= 0 && i < n) out.push_back(i);
        if (!out.empty()) return out;
    }
    out.reserve((size_t)n);
    for (int i = 0; i < n; ++i) out.push_back(i);
    return out;
}

bool PianoRoll::quantizeSelected(ClipModel& clip) {
    const std::vector<int> tg = toolTargets(clip);
    if (tg.empty()) return false;
    const f64 g = quantGrid_ > 1e-6 ? quantGrid_ : 0.25;
    const f32 t = clampv(quantStrength_, 0.f, 1.f);
    if (t <= 0.f) return false;

    // Tracked as notes, not indices: moving starts reorders the vector, and an
    // index into the old order is an edit to the wrong note afterwards.
    SelKeys keys;
    const bool hadSel = owns(clip) && !sel_.empty();
    bool moved = false;
    for (int i : tg) {
        NoteModel& nt = clip.notes[(size_t)i];
        const f64 target = std::round(nt.beat / g) * g;
        // Partway there, and clamped into the clip at both ends exactly as a
        // drag is -- a quantize must not push a note past the loop end.
        f64 b = nt.beat + (target - nt.beat) * (f64)t;
        b = clampv(b, 0.0, std::max(0.0, clip.lengthBeats - nt.len));
        if (b != nt.beat) { nt.beat = b; moved = true; }
        if (hadSel) {
            if (i == sel_.primary) keys.primary = (int)keys.notes.size();
            keys.notes.push_back(nt);
        }
    }
    if (!moved) return false;
    if (hadSel) sortTrackingSet(clip.notes, keys, sel_);
    else        std::sort(clip.notes.begin(), clip.notes.end(), noteLess);
    followSel_ = true;
    lastEdit_ = kEditNote;
    return true;
}

bool PianoRoll::legatoSelected(ClipModel& clip) {
    const std::vector<int> tg = toolTargets(clip);
    if (tg.empty()) return false;
    // The next start after each note, over the WHOLE clip and not only over the
    // targets: a selected note followed by an unselected one must still stop
    // where that one begins, or legato on half a phrase would bury the rest.
    // The vector is sorted by beat, so one pass finds every answer.
    bool changed = false;
    for (int i : tg) {
        NoteModel& nt = clip.notes[(size_t)i];
        f64 next = clip.lengthBeats;
        for (size_t k = 0; k < clip.notes.size(); ++k) {
            const f64 b = clip.notes[k].beat;
            if (b > nt.beat + 1e-9 && b < next) next = b;
        }
        const f64 len = std::max(kGridStep * 0.25, next - nt.beat);
        if (std::fabs(len - nt.len) > 1e-9) { nt.len = len; changed = true; }
    }
    if (!changed) return false;
    lastEdit_ = kEditNote;
    return true;
}

bool PianoRoll::duplicateSelected(ClipModel& clip) {
    const std::vector<int> tg = toolTargets(clip);
    if (tg.empty()) return false;
    // How far the copies go: the span the targets occupy, rounded UP to a whole
    // grid step so a two-and-a-bit-beat phrase repeats on the grid rather than
    // a sixteenth off it.
    f64 lo = 1e300, hi = -1e300;
    for (int i : tg) {
        const NoteModel& nt = clip.notes[(size_t)i];
        lo = std::min(lo, nt.beat);
        hi = std::max(hi, nt.beat + nt.len);
    }
    f64 span = std::ceil((hi - lo) / kGridStep - 1e-9) * kGridStep;
    if (!(span > 0.0)) span = kGridStep;

    SelKeys keys;
    std::vector<NoteModel> made;
    made.reserve(tg.size());
    for (int i : tg) {
        NoteModel c = clip.notes[(size_t)i];
        c.beat += span;
        if (c.beat >= clip.lengthBeats - 1e-9) continue;   // past the end: dropped
        c.len = std::min(c.len, clip.lengthBeats - c.beat);
        if (i == sel_.primary) keys.primary = (int)keys.notes.size();
        keys.notes.push_back(c);
        made.push_back(c);
    }
    if (made.empty()) return false;                        // nothing fitted
    for (const NoteModel& c : made) clip.notes.push_back(c);
    // The copies become the selection: the copy is what is about to be edited.
    sortTrackingSet(clip.notes, keys, sel_);
    followSel_ = true;
    lastEdit_ = kEditNote;
    return true;
}

bool PianoRoll::transposeSelected(ClipModel& clip, int semitones) {
    if (semitones == 0) return false;
    const std::vector<int> tg = toolTargets(clip);
    if (tg.empty()) return false;
    // With a selection this is nudgeSelected's pitch half exactly. Without one
    // the whole clip becomes the group for the length of the call, and the
    // selection that comes back out of it is dropped again -- transposing a
    // pattern must not silently leave every note in it selected.
    const bool hadSel = owns(clip) && !sel_.empty() &&
                        sel_.items.back() < (int)clip.notes.size();
    IndexSel all;
    IndexSel& use = hadSel ? sel_ : all;
    if (!hadSel) {
        all.items = tg;
        all.primary = tg.front();
    }
    const GroupDelta d = clampGroupDelta(clip.notes, use.items, 0.0, semitones,
                                         clip.lengthBeats);
    if (!applyGroupDelta(clip.notes, use, d, key_)) return false;
    if (hadSel) {
        followSel_ = true;
        if (sel_.primary >= 0 && sel_.primary < (int)clip.notes.size())
            preview_.push((int)clip.notes[(size_t)sel_.primary].pitch);
    }
    lastEdit_ = kEditNote;
    return true;
}

bool PianoRoll::duplicateLoop(ClipModel& clip) {
    if (!owns(clip)) return false;
    const DupResult res = duplicateLoopNotes(clip.notes, clip.lengthBeats, sel_);
    if (!res.changed) return false;                // already at the cap
    sel_ = res.sel;
    followSel_ = true;
    lastEdit_ = kEditNote;
    return true;
}

} // namespace lat
