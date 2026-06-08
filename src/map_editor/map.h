#ifndef MAP_EDITOR_H
#define MAP_EDITOR_H


#include "raylib.h"

#define NO_LINK -1

typedef struct Vertex {
    int id;
    Vector2 points;
} Vertex;

typedef struct Wall{
    int point_start, point_end;
    int next_wall, next_sector;
    int texture_id, portal_wall;
} Wall;

typedef struct Sector {
    int wall_start, wall_count;
    float floor_z, ceilingz;
    int floor_texture, ceiling_texture;
} Sector;

extern Vertex vertices[];
extern Wall walls[];
extern Sector sectors[];

extern int sector_counter;
extern int wall_counter;
extern int vertex_counter;

extern Vector2 playerStart;
extern float   playerStartYaw;

void ResetMapEditorCamera();
void DrawMapEditor();



#endif