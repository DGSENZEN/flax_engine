#ifndef FLAX_LIGHTS_H
#define FLAX_LIGHTS_H

#include "raylib.h"

// ===========================================================================
// Flax Engine - dynamic lights, computed entirely on the CPU.
//
// No shaders. This module only *tracks* light sources; the lighting itself
// happens in SectorWorldRelight() (renderer/sectors.c), which rewrites the
// world mesh's vertex colors in C each frame:
//
//     final = baseColor (flat shade * sector light) + sum of point lights
//
// and re-uploads the color buffer. The default raylib shader multiplies
// vertexColor * texture, so the GPU never needs custom code - per-vertex
// lighting exactly the way Quake 2 lit entities and PS1 games lit worlds.
// The world mesh is tessellated at bake time so lights always have vertices
// near enough to land on.
//
// Two ways to feed it, both fixed-size and allocation-free:
//   DlightPush  - this frame only; callers re-push every frame for moving
//                 sources (projectiles, muzzle flash).
//   DlightSpawn - timed flash that decays over ttl (impacts, explosions).
// ===========================================================================

#define MAX_DLIGHTS 8      // active lights per frame; extra pushes drop

void LightsInit(void);

// Frame protocol: BeginFrame ages timed lights and clears the frame list,
// callers push, then SectorWorldRelight() gathers and applies.
void LightsBeginFrame(float dt);
void DlightPush(Vector3 pos, Vector3 color, float radius);
void DlightSpawn(Vector3 pos, Vector3 color, float radius, float ttl);

// The combined light set for this frame (pushes first, then timed lights
// scaled by remaining ttl). Fills the arrays, returns the count (<= max).
int LightsGather(Vector3 *pos, Vector3 *color, float *radius, int max);

#endif // FLAX_LIGHTS_H
