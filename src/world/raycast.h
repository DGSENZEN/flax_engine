#ifndef FLAX_RAYCAST_H
#define FLAX_RAYCAST_H

#include "raylib.h"
#include <stdbool.h>

// ===========================================================================
// Flax Engine - world raycast.
//
// The one query everything ballistic sits on: bullets, decals, line of
// sight, editor picking, and (later) lightmap shadow rays. Pure geometry
// over the sector world - no GPU, no filesystem - so offline tools can
// link it exactly like world.c.
//
// The walk is sector-native, the same idea as Build's hitscan: start in
// the sector containing the ray origin, find where the ray's 2D shadow
// exits the wall loop, test the floor/ceiling planes inside that span
// (slope-aware), then either stop at a solid wall / step face or carry
// on through the portal's open window into the next sector.
// ===========================================================================

typedef enum {
    RAY_HIT_NONE = 0,
    RAY_HIT_WALL,
    RAY_HIT_FLOOR,
    RAY_HIT_CEILING
} RayHitKind;

typedef struct RayHit {
    int     kind;       // RayHitKind
    float   dist;       // along the ray, world units
    Vector3 pos;        // world-space impact point
    Vector3 normal;     // unit length, faces back toward the ray origin
    int     sector;     // sector owning the hit surface
    int     wall;       // wall index for RAY_HIT_WALL, else -1
} RayHit;

// Cast from a world-space origin along dir (any length, normalized inside)
// up to max_dist world units. Returns true and fills *hit on impact; returns
// false when the ray escapes the map, exceeds max_dist, or starts outside
// every sector.
bool WorldRaycast(Vector3 origin, Vector3 dir, float max_dist, RayHit *hit);

#endif // FLAX_RAYCAST_H
