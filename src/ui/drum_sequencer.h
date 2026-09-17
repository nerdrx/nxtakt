#pragma once
#include "session.h"
#include "drum_pattern.h"
#include "widgets.h"

namespace lat {

// Edits the clip's ordinary MIDI notes; there is no second pattern or clock.
class DrumSequencer {
public:
    // playheadBeats is the current position within this clip, in beats.
    bool draw(Ui& ui, const Rect& bounds, ClipModel& clip,
              double playheadBeats, bool playing);
    const char* lastEdit() const { return lastEdit_; }
    int drainPreview(u8* out, int max);

private:
    void preview(int pitch);
    inline static DrumPage pageClipboard_;
    u64 clipUid_ = ~u64{0};
    int page_ = 0, lengthChoice_ = 0, preset_ = 0;
    int rhythmLane_=0;
    f64 rhythmHits_=4, rhythmRotation_=0, rhythmVelocity_=100;
    f32 scrollY_ = 0.f, scrollX_ = 0.f;
    u8 previews_[8]{};
    int previewCount_ = 0;
    const char* lastEdit_ = "drum step";
};

} // namespace lat
