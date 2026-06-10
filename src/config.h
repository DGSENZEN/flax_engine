#ifndef FLAX_CONFIG_H
#define FLAX_CONFIG_H

// ---------------------------------------------------------------------------
// Flax Engine - shared compile-time configuration.
//
// One home for the constants that more than one translation unit needs, so
// the window size and world limits can never drift out of sync between the
// editor, the renderer and main. Keep this lean: editor-only or renderer-only
// constants stay in their own .c file.
// ---------------------------------------------------------------------------

// Window
#define SCREEN_WIDTH   1460
#define SCREEN_HEIGHT  820

// Editor 2D point + height -> world 3D. Single source of the map-to-world
// scale; SectorToWorld() in the renderer is the only place that applies it.
#define WORLD_SCALE    0.06f

// Static world capacity. Flat, fixed arrays in the 90s style: no per-frame
// allocation, indices are stable handles, bounds are known up front.
#define MAX_SECTORS    1000
#define MAX_WALLS      10000
#define MAX_VERTICES   100000
#define MAX_ENTITIES   256

#endif // FLAX_CONFIG_H
