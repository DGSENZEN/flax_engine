#ifndef FLAX_TRIANGULATE_H
#define FLAX_TRIANGULATE_H

#include "raylib.h"

// ===========================================================================
// Flax Engine - polygon triangulation (ear clipping with holes).
//
// Replaces the naive triangle fan, which only works for convex sectors.
// Concave rooms triangulate correctly, and inner loops (columns, pillars)
// are supported by bridging each hole into the outer boundary before
// clipping - the classic earcut approach.
//
// Pure 2D geometry, no GPU: the renderer lifts the result to 3D with
// per-corner heights, and the future lightmap baker can reuse it offline.
// ===========================================================================

// Triangulate one polygon. verts holds every loop's points concatenated;
// loop_counts[k] is the vertex count of loop k. Loop 0 is the outer
// boundary, the rest are holes (any winding - normalized internally).
//
// out receives 3 Vector2 per triangle, wound counter-clockwise in math
// coordinates (positive signed area). Returns the triangle count, 0 on
// degenerate input, capped at max_tris.
int TriangulatePolygon(const Vector2 *verts, const int *loop_counts,
                       int loop_count, Vector2 *out, int max_tris);

#endif // FLAX_TRIANGULATE_H
