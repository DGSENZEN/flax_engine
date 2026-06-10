#ifndef FLAX_BILLBOARD_H
#define FLAX_BILLBOARD_H

#include "raylib.h"
#include <stdbool.h>

// ===========================================================================
// Flax Engine - camera-facing sprite quads.
//
// raylib's DrawBillboardRec locks billboards to the world Y axis, so a
// sprite viewed from above or below forshortens toward edge-on and vanishes.
// This one builds the quad from the camera's actual right/up, so it always
// faces the screen at full size regardless of pitch - and mirroring is a
// plain UV swap instead of negative-rectangle API semantics.
// ===========================================================================

// Draw source rect of tex as a screen-facing quad centered on `center`,
// `size` in world units. mirror flips horizontally (Build-style sprite
// direction reuse). Call inside BeginMode3D.
void DrawSpriteBillboard(Camera camera, Texture2D tex, Rectangle source,
                         Vector3 center, Vector2 size, bool mirror, Color tint);

#endif // FLAX_BILLBOARD_H
