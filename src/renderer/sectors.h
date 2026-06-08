#ifndef SECTOR_RENDERER_H
#define SECTOR_RENDERER_H

#include "raylib.h"

void BuildSectorMeshes();
void DrawSectorWorld();
void UnloadSectorMeshes();

// editor 2D point + height -> world 3D (single source of the scale)
Vector3 SectorToWorld(Vector2 p, float y);
#endif