#ifndef FLAX_DECALS_H
#define FLAX_DECALS_H

#include "raylib.h"

// ===========================================================================
// Flax Engine - runtime decals (bullet holes, scorch marks).
//
// A fixed ring buffer of textured quads glued to world surfaces, in the
// 90s style: no allocation, oldest decal silently recycled when the pool
// wraps. These are gameplay residue, not map data - hand-authored decals
// stay ENT_DECAL in the entity lump.
//
// Spawn takes the impact point and surface normal straight from a RayHit;
// the quad is pushed a hair off the surface to avoid z-fighting and given
// a random spin so repeated hits don't tile.
// ===========================================================================

void DecalsClear(void);

// pos/normal in world space (normal unit length, facing the shooter).
// size is the quad edge in world units, registry_tex a texture registry
// index (see TexRegistryFind).
void DecalSpawn(Vector3 pos, Vector3 normal, float size, int registry_tex);

// Draw all live decals. Call inside BeginMode3D, after the world mesh.
void DecalsDraw(void);

#endif // FLAX_DECALS_H
