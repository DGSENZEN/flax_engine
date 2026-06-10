#ifndef FLAX_TEXTURES_H
#define FLAX_TEXTURES_H

#include "raylib.h"

// ===========================================================================
// Flax Engine - texture registry (the GPU side of texturing).
//
// The world references textures by *name* (see world.h); this registry owns
// the actual GPU textures and resolves names to them. Two sources, in order:
//
//   1. A built-in procedural set (DEFAULT, BRICK, STONE, FLOOR, CEIL, PANEL)
//      generated at startup so the engine always runs, even with no assets.
//   2. assets/textures/*.png - loaded after, by uppercased basename, and
//      they *override* a built-in with the same name. Drop in a BRICK.png
//      and every wall tagged BRICK uses it; no map edits needed.
//
// This is the wad/miptex relationship from Quake: the map stores names,
// the texture store provides pixels, and the two can ship separately.
//
// All registry textures are point-filtered and repeat-wrapped (crunchy
// tiling, the 90s look). Call TexturesInit() once after InitWindow().
// ===========================================================================

void TexturesInit(void);
void TexturesUnload(void);

// Drop-in workflow: re-scan assets/textures without restarting (F5 in the
// editor). World texture ids survive a reload - they are names, not slots.
// The world meshes are rebuilt on the next editor->game switch.
void TexturesReload(void);

int          TexRegistryCount(void);
const char  *TexRegistryName(int index);
Texture2D    TexRegistryTexture(int index);

// Resolve a *world* texture id (an index into world_texnames) to a registry
// index. Unknown names fall back to 0 (DEFAULT) - a missing texture renders
// obviously wrong instead of crashing, the id-software way.
int TexRegistryResolve(int world_texture_id);

#endif // FLAX_TEXTURES_H
