#ifndef FLAX_PROFILER_H
#define FLAX_PROFILER_H

#include <stdbool.h>

// ===========================================================================
// Flax Engine - frame profiler.
//
// Named CPU zones + a frame-time history graph, drawn as an in-game overlay
// (F1). Answers the three questions that matter when a level gets slow:
//
//   1. What's the frame rate, and is it stable?  (graph + avg/max ms)
//   2. Is the frame CPU-bound or GPU/vsync-bound?  The overlay shows
//      "logic" (sum of measured CPU zones) next to "frame" (wall time
//      between frames). frame >> logic means the CPU is waiting on the GPU
//      or on vsync - press F2 to uncap the FPS limit and see real headroom.
//   3. Which system eats the budget?  Per-zone table with smoothed ms.
//
// Usage:   PROF_BEGIN("collision");  ...work...  PROF_END("collision");
// Zones are matched by name, may nest, and reset every frame. Overhead is
// two clock reads and a tiny string lookup - fine to leave in release.
// ===========================================================================

void ProfFrameStart(void);            // call once at the top of the main loop

void ProfBegin(const char *zone);
void ProfEnd(const char *zone);
#define PROF_BEGIN(name) ProfBegin(name)
#define PROF_END(name)   ProfEnd(name)

void ProfToggle(void);                // show/hide the overlay (bind to F1)
bool ProfVisible(void);
void ProfDraw(void);                  // draw the overlay; call late in the frame

// F2 helper: returns the new target FPS (0 = uncapped) so main can apply it.
int  ProfCycleFpsCap(void);

#endif // FLAX_PROFILER_H
