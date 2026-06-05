#include "map.h"
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <stdio.h>

#define GRID_SPACING 25.0f
#define MAX_SECTORS 1000
#define MAX_WALLS 10000
#define MAX_VERTICES 100000

typedef struct Vertex{
    int id;
    Vector2 points;
} Vertex;

typedef struct Wall {
    int point_start;
    int point_end;
    int next_wall;
    int next_sector;
    int texture_id;
} Wall;

typedef struct Sector {
    int wall_start;
    int wall_count;
    float floor_z;
    float ceilingz;
    int floor_texture;
    int ceiling_texture;
} Sector;


// data structures to store the geometry data

Sector sectors[MAX_SECTORS];
int sector_counter = 0;

Wall walls[MAX_WALLS];
int wall_counter = 0;

Vertex vertices[MAX_VERTICES];
int vertex_counter = 0;


static Camera2D editorCamera = { .zoom = 1.0f };

void ResetMapEditorCamera() {
    editorCamera.target = (Vector2){0.0f, 0.0f};
    editorCamera.offset = (Vector2){GetScreenWidth() / 2.0f, GetScreenHeight() / 2.0f};
    editorCamera.rotation = 0.0f;
    editorCamera.zoom = 1.0f;

}

void DrawMapEditor() {

        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
            Vector2 delta = GetMouseDelta();
            delta = Vector2Scale(delta, -1.0f/editorCamera.zoom);
            editorCamera.target = Vector2Add(editorCamera.target, delta);
        }

        Vector2 mouseScreenPos = GetMousePosition();
        Vector2 mouseWorldPos = GetScreenToWorld2D(mouseScreenPos, editorCamera);
        
        //begin drawing
        BeginMode2D(editorCamera);
        rlPushMatrix();
            rlTranslatef(0,  25*50, 0);
            rlRotatef(90, 1, 0, 0);
            DrawGrid(200, GRID_SPACING);
        rlPopMatrix();

        EndMode2D();
}