#ifndef MAP_EDITOR_H
#define MAP_EDITOR_H

// World data (vertices/walls/sectors, texture name table) lives in
// world/world.h so the serializer and offline tools can use it without
// pulling in the editor.
#include "world/world.h"

void ResetMapEditorCamera(void);
void DrawMapEditor(void);

#endif
