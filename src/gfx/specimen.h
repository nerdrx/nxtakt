// The NX specimen sheet: every primitive in the vocabulary, on one screen,
// over the real background, drawn over whatever the app is showing.
//
// Gated on NXTAKT_DEBUG_GLASS=1 and drawn from Renderer::end(), so it needs no
// call site anywhere in src/ui -- which is the point. It is what screenshots
// gate on, and what a view being re-skinned should be read side by side with.
//
// It also carries the half of the judgment that is easy to forget: the
// right-hand column ends with a WORKING SURFACE patch -- flat cells, a well, a
// cyan playhead, no glass anywhere -- because the clip grid, the arrangement
// and the piano roll get the palette and the recessed-well language and
// nothing else. A frosted piano roll is a usability bug wearing a costume.
#pragma once
#include "../core/common.h"

namespace lat {

class Renderer;

void drawSpecimen(Renderer& r, f64 timeSeconds);

// Releases the fonts the specimen loaded lazily. Called from
// Renderer::shutdown(); a no-op if the specimen never ran.
void specimenShutdown();

} // namespace lat
