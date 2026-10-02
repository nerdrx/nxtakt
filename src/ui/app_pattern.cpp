// Pattern painting copies the selected scene's source clips into the timeline.
#include "app_internal.h"
#include <algorithm>
#include <cmath>

namespace lat {

bool App::paintPatternAt(f64 beat, u64 gesture) {
    if (!std::isfinite(beat) || beat < 0.0 || selSlot_ < 0 ||
        selSlot_ >= (int)ses_.scenes.size()) return false;

    constexpr f64 kSameBeatEpsilon = 1e-6;
    bool painted = false;
    for (size_t ti = 0; ti < ses_.tracks.size(); ++ti) {
        TrackModel& track = ses_.tracks[ti];
        if (selSlot_ >= kMaxScenes) continue;
        const ClipModel& source = track.slots[(size_t)selSlot_];
        if (!source.valid()) continue;

        const bool exists = std::any_of(track.arrange.begin(), track.arrange.end(),
            [&](const ArrangeClip& item) {
                return item.sourceUid == source.uid &&
                       std::fabs(item.start - beat) <= kSameBeatEpsilon;
            });
        if (exists || (int)track.arrange.size() >= kMaxArrItems) continue;

        if (!painted) undoPoint("paint pattern", gesture);
        ArrangeClip item;
        item.uid = ses_.newUid();
        item.start = beat;
        item.length = std::isfinite(source.lengthBeats) &&
                      source.lengthBeats > kMinArrBeats
                          ? source.lengthBeats : 4.0;
        item.sourceUid = source.uid;
        item.src = source;
        item.src.uid = item.uid;
        track.arrange.push_back(std::move(item));
        arrangeRepair(track.arrange);
        publishArrangement((int)ti);
        painted = true;
    }
    return painted;
}

f64 App::patternPaintLength() const {
    const f64 bar0 = ses_.beatOfBar(0.0);
    const f64 bar1 = ses_.beatOfBar(1.0);
    const f64 beatsPerBar = bar1 - bar0;
    if (!std::isfinite(beatsPerBar) || beatsPerBar <= 0.0) return 4.0;

    f64 longest = 0.0;
    if (selSlot_ >= 0 && selSlot_ < (int)ses_.scenes.size()) {
        for (const TrackModel& track : ses_.tracks) {
            if (selSlot_ >= kMaxScenes) continue;
            const ClipModel& clip = track.slots[(size_t)selSlot_];
            if (!clip.valid() || !std::isfinite(clip.lengthBeats)) continue;
            longest = std::max(longest, clip.lengthBeats);
        }
    }

    const f64 bars = std::max(1.0, std::ceil(longest / beatsPerBar - 1e-9));
    const f64 length = ses_.beatOfBar(bars) - bar0;
    return std::isfinite(length) && length > 0.0 ? length : bars * beatsPerBar;
}

void App::togglePatternPaint() {
    studioPatternPaint_ = !studioPatternPaint_;
    studioPatternStroke_ = false;
    if (studioPatternPaint_) {
        if (!studioSongMode_ && es_.playing) { send(Cmd::SetPlaying, 0); autoRecFinish(); }
        studioSongMode_ = true;
        view_ = MainView::Arrangement;
        status_ = "Paint: drag the selected pattern across the Playlist. Escape returns to editing.";
    }
}

} // namespace lat
