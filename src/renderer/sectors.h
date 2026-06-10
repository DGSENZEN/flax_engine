#ifndef SECTOR_RENDERER_H
#define SECTOR_RENDERER_H

#include "raylib.h"

void BuildSectorMeshes();
void DrawSectorWorld();
void UnloadSectorMeshes();

// Built-world size for the profiler overlay (0/0 when nothing is built).
void SectorWorldStats(int *mesh_count, int *triangle_count);

// editor 2D point + height -> world 3D (single source of the scale)
Vector3 SectorToWorld(Vector2 p, float y);
#endif