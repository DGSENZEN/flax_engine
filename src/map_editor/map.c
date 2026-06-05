#include "map.h"
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <stdio.h>
#include <stdint.h>

#define SCREEN_HEIGHT 820
#define SCREEN_WIDTH 1460

#define INVALID_ID 0

#define GRID_SPACING 50.0f
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


static Camera2D editorCamera = { 0 };

Vector2 SnapVertexToGrid(Vector2 vertex, float gridSize) {
    Vector2 snappedVertexVec;
    snappedVertexVec.x = roundf(vertex.x / gridSize) * gridSize;
    snappedVertexVec.y = roundf(vertex.y / gridSize) * gridSize;
    return snappedVertexVec;
}

uint32_t makeUniqueId(){
    static uint32_t current_id = INVALID_ID;
    if (current_id == UINT32_MAX) {
        fprintf(stderr, "Error: Max number of unique IDs reached!");
        return INVALID_ID;
    }

    current_id++;
    return current_id;
}


void ResetMapEditorCamera() {
    editorCamera.target = (Vector2){0.0f, 0.0f};
    editorCamera.offset = (Vector2){GetScreenWidth() / 2.0f, GetScreenHeight() / 2.0f};
    editorCamera.rotation = 0.0f;
    editorCamera.zoom = 1.0f;
}

void DrawMapEditor() {

    bool isClickedOn = false;
    static int draggedVertex = -1;
    Vector2 mouseScreenPos = GetMousePosition();
    Vector2 mouseWorldPos = GetScreenToWorld2D(mouseScreenPos, editorCamera);
    Vector2 snappedVertex = SnapVertexToGrid(mouseWorldPos, GRID_SPACING);
    
    Vector2 topLeft = GetScreenToWorld2D((Vector2){0, 0}, editorCamera);
    Vector2 bottomRight = GetScreenToWorld2D((Vector2){SCREEN_WIDTH, SCREEN_HEIGHT}, editorCamera);

    float startX = floorf(topLeft.x / GRID_SPACING) * GRID_SPACING;
    float endX = ceilf(bottomRight.x / GRID_SPACING) * GRID_SPACING;
    float startY = floor(topLeft.y / GRID_SPACING) * GRID_SPACING;
    float endY = ceilf(bottomRight.y / GRID_SPACING) * GRID_SPACING;

    if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
        Vector2 delta = GetMouseDelta();
        delta = Vector2Scale(delta, -1.0f/editorCamera.zoom);
        editorCamera.target = Vector2Add(editorCamera.target, delta);
    }

    float wheel = GetMouseWheelMove();

    //handle scroll behaviour
    if (wheel != 0) {
        Vector2 mouseWorldPos = GetScreenToWorld2D(GetMousePosition(), editorCamera);
        editorCamera.offset = GetMousePosition();
        editorCamera.target = mouseWorldPos;
        editorCamera.zoom += wheel * 0.01f;
        if (editorCamera.zoom < 0.2f) editorCamera.zoom = 0.2f;
        if (editorCamera.zoom > 5.0f) editorCamera.zoom = 5.0f;
    }

        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            isClickedOn = true;
        }

        //begin drawing
        BeginMode2D(editorCamera);

        //drawing the grid
        for (float x = startX; x <= endX; x += GRID_SPACING) {
            Color lineColor = (x == 0.0f) ? DARKGRAY : GetColor(0x2A2A2AFF);
            DrawLineV((Vector2){x, startY}, (Vector2){x, endY}, lineColor);
        }

        for (float y = startY; y <= endY; y += GRID_SPACING) {
            Color lineColor = (y == 0.0f) ? DARKGRAY : GetColor(0x2A2A2AFF);
            DrawLineV((Vector2){startX, y}, (Vector2){endX, y}, lineColor);
        }

        float vertexRadius = 4.0f / editorCamera.zoom;

        //grab an existing vertex instead of creating a new one
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            for (int i = 0; i < vertex_counter; i++) {
                if (CheckCollisionPointCircle(mouseWorldPos, vertices[i].points, vertexRadius + 2.0f)) {
                    draggedVertex = i;
                    isClickedOn = false;
                    break;
                }
            }
        }

        //vertex struct data behaviour
        if (!isClickedOn) {
            DrawCircleV(snappedVertex, vertexRadius, GREEN);
        } else if (isClickedOn && vertex_counter < MAX_VERTICES) {
            int idx = vertex_counter;
            DrawCircleV(snappedVertex, vertexRadius, Fade(GREEN, 0.6f));
            Vertex v = {
                .id = makeUniqueId(),
                .points = (Vector2){.x = snappedVertex.x, .y = snappedVertex.y},
            };
            vertices[vertex_counter++] = v;
            if (idx > 0 && wall_counter < MAX_WALLS) {
                Wall w = {
                    .point_start = idx - 1,
                    .point_end = idx,
                    .next_wall = INVALID_ID,
                    .next_sector = INVALID_ID,
                    .texture_id = INVALID_ID,
                };
                walls[wall_counter++] = w;
            }
        }

        //drag the grabbed vertex, then release on mouse up
        if (draggedVertex != -1 && IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
            vertices[draggedVertex].points = SnapVertexToGrid(mouseWorldPos, GRID_SPACING);
        }
        if (IsMouseButtonReleased(MOUSE_LEFT_BUTTON)) {
            draggedVertex = -1;
        }
        
        // render the points clicked on
        for (int i = 0; i < vertex_counter; i++) {
            DrawCircleV((Vector2){vertices[i].points.x, vertices[i].points.y}, CheckCollisionPointCircle(mouseWorldPos, (Vector2){.x = vertices[i].points.x, .y= vertices[i].points.y}, vertexRadius + 2.0f)? vertexRadius + 4 : vertexRadius, GRAY);
        }

        for (int i = 0; i < wall_counter; i++) {
            Vector2 a = vertices[walls[i].point_start].points;
            Vector2 b = vertices[walls[i].point_end].points;
            DrawLineV(a, b, RAYWHITE);
            printf("a: (%f, %f), b: (%f, %f)", a.x, a.y, b.x, b.y);
        }

        EndMode2D();
}