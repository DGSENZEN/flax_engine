#include "map.h"
#include "raylib.h"
#include "raymath.h"
#include "raygui.h"
#include "rlgl.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

#define SCREEN_HEIGHT 820
#define SCREEN_WIDTH 1460

#define INVALID_ID 0

#define GRID_SPACING 50.0f
#define MAX_SECTORS 1000
#define MAX_WALLS 10000
#define MAX_VERTICES 100000
#define NO_LINK -1

#define MODE_EDIT 1
#define MODE_DRAG 2

#define SECTOR_VERTEX_MIN 3
#define SECTOR_WALL_MIN 3

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
    int portal_wall;
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

bool is_drawing_sector = false;
bool isClickedOn = false;
int current_sector_start_vertex_id = -1;
int current_sector_start_wall_id = -1;

Vector2 SnapVertexToGrid(Vector2 vertex, float gridSize) {
    Vector2 snappedVertexVec;
    snappedVertexVec.x = roundf(vertex.x / gridSize) * gridSize;
    snappedVertexVec.y = roundf(vertex.y / gridSize) * gridSize;
    return snappedVertexVec;
}

int SectorOfWall(int w){
    for (int i = 0; i < sector_counter; i++) {
        int start = sectors[i].wall_start;
        int count = sectors[i].wall_count;

        if (w >= start && w < (start + count)){
            return i;
        }
    }
    return NO_LINK;
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

int GetOrCreateVertex(Vector2 snappedVertex) {
    for (int i = 0; i < vertex_counter; i++) {
        if (vertices[i].points.x == snappedVertex.x && vertices[i].points.y == snappedVertex.y) {
            return i;
        }
    }
    if (vertex_counter >= MAX_VERTICES) return -1;
    int v_idx = vertex_counter++;
    vertices[v_idx] = (Vertex){ .id = makeUniqueId(), .points = snappedVertex };
    return v_idx;
}

void HandleDeleteBehavior(){
    if (IsKeyPressed(KEY_B)){
        vertex_counter--;
        if (vertex_counter > 0) {
            wall_counter--;
        }
    }
}

void DrawMapGeometry(Vector2 snappedVertex, Vector2 mouseWorldPos){
    if (isClickedOn) {
        if (!is_drawing_sector){
            int v_idx = GetOrCreateVertex(snappedVertex);
            if (v_idx != -1) {
                is_drawing_sector = true;
                current_sector_start_vertex_id = v_idx;
                current_sector_start_wall_id = wall_counter;
            }
        } else {
            Vector2 startVertexPos = vertices[current_sector_start_vertex_id].points;
            float closeDetectionRadius = 6.0f / editorCamera.zoom;

            int wall_count = wall_counter - current_sector_start_wall_id;
            int prev_v_idx = wall_count > 0 ? walls[wall_counter - 1].point_end
                                            : current_sector_start_vertex_id;

            if (CheckCollisionPointCircle(mouseWorldPos, startVertexPos, closeDetectionRadius)) {
                if (wall_count >= SECTOR_VERTEX_MIN - 1 && wall_counter < MAX_WALLS) {
                    int final_w_idx = wall_counter++;

                    walls[final_w_idx] = (Wall) {
                        .point_start = prev_v_idx,
                        .point_end = current_sector_start_vertex_id,
                        .next_wall = NO_LINK,
                        .next_sector = NO_LINK,
                        .portal_wall = NO_LINK,
                        .texture_id = INVALID_ID,
                    };

                    for (int i = current_sector_start_wall_id; i < final_w_idx; i++) {
                        walls[i].next_wall = i + 1;
                    }
                    walls[final_w_idx].next_wall = current_sector_start_wall_id;

                    if (sector_counter < MAX_SECTORS) {
                        sectors[sector_counter++] = (Sector){
                            .wall_start = current_sector_start_wall_id,
                            .wall_count = wall_counter - current_sector_start_wall_id,
                            .floor_z = 0.0f,
                            .ceilingz = 10.0f,
                            .floor_texture = INVALID_ID,
                            .ceiling_texture = INVALID_ID,
                        };
                        int my_wall_start = current_sector_start_wall_id;
                        for (int a = my_wall_start; a < wall_counter; a++) {
                            for (int b = 0; b < wall_counter; b++) {
                                if (b >= my_wall_start) continue;
                                if (walls[b].point_start == walls[a].point_end &&
                                    walls[b].point_end   == walls[a].point_start) {
                                        walls[a].next_sector = SectorOfWall(b);
                                        walls[b].next_sector = SectorOfWall(a);
                                        walls[a].portal_wall = b;
                                        walls[b].portal_wall = a;
                                    }
                            }
                        }
                    
                    }
                    is_drawing_sector = false;
                    current_sector_start_vertex_id = -1;
                    current_sector_start_wall_id = -1;
                }
            } else {
                if (wall_counter < MAX_WALLS) {
                    int new_v_idx = GetOrCreateVertex(snappedVertex);
                    if (new_v_idx != -1 && new_v_idx != prev_v_idx) {
                        walls[wall_counter++] = (Wall) {
                            .point_start = prev_v_idx,
                            .point_end = new_v_idx,
                            .next_wall = NO_LINK,
                            .next_sector = NO_LINK,
                            .portal_wall = NO_LINK,
                            .texture_id = INVALID_ID,
                        };
                    }
                }
            }
        }
        isClickedOn = false;
    }
}


void DrawMapEditor() {

    static int draggedVertex = -1;
    static int editorMode = MODE_EDIT;

    if (IsKeyPressed(KEY_ONE)) editorMode = MODE_EDIT;
    if (IsKeyPressed(KEY_TWO)) editorMode = MODE_DRAG;

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

        if (editorMode == MODE_EDIT && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
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
        float pickRadius = 6.0f / editorCamera.zoom;

        //grab the nearest existing vertex instead of creating a new one
        if (editorMode == MODE_DRAG && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            float bestDistSq = pickRadius * pickRadius;
            for (int i = 0; i < vertex_counter; i++) {
                float distSq = Vector2DistanceSqr(mouseWorldPos, vertices[i].points);
                if (distSq < bestDistSq) {
                    bestDistSq = distSq;
                    draggedVertex = i;
                    isClickedOn = false;
                }
            }
        }

        //vertex struct data behaviour
        DrawMapGeometry(snappedVertex, mouseWorldPos);
        HandleDeleteBehavior();

        //drag the grabbed vertex, then release on mouse up
        if (draggedVertex != -1 && IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
            vertices[draggedVertex].points = SnapVertexToGrid(mouseWorldPos, GRID_SPACING);
        }
        if (IsMouseButtonReleased(MOUSE_LEFT_BUTTON)) {
            int d = draggedVertex;
            for (int i = 0; d != -1 && i < vertex_counter; i++) {
                if (i == d) continue;
                if (vertices[i].points.x != vertices[d].points.x ||
                    vertices[i].points.y != vertices[d].points.y) continue;

                for (int w = 0; w < wall_counter; w++) {
                    if (walls[w].point_start == d) walls[w].point_start = i;
                    if (walls[w].point_end   == d) walls[w].point_end   = i;
                }
                int last = vertex_counter - 1;
                if (d != last) {
                    vertices[d] = vertices[last];
                    for (int w = 0; w < wall_counter; w++) {
                        if (walls[w].point_start == last) walls[w].point_start = d;
                        if (walls[w].point_end   == last) walls[w].point_end   = d;
                    }
                }
                vertex_counter--;
                break;
            }
            draggedVertex = -1;
        }
        
        // render the points clicked on
        for (int i = 0; i < vertex_counter; i++) {
            Vector2 v_point = {vertices[i].points.x, vertices[i].points.y};
            DrawCircleV(v_point, CheckCollisionPointCircle(mouseWorldPos, v_point, pickRadius)? vertexRadius + 4 : vertexRadius, draggedVertex == i ? RAYWHITE : YELLOW);
        }

        for (int i = 0; i < wall_counter; i++) {
            Vector2 a = vertices[walls[i].point_start].points;
            Vector2 b = vertices[walls[i].point_end].points;
            DrawLineV(a, b, walls[i].next_sector != NO_LINK ? RED: RAYWHITE);
        }

        if (is_drawing_sector && vertex_counter > 0) {
            int last_v_idx = wall_counter > current_sector_start_wall_id
                                 ? walls[wall_counter - 1].point_end
                                 : current_sector_start_vertex_id;
            Vector2 lastVertexPos = vertices[last_v_idx].points;

            Vector2 startVertexPos = vertices[current_sector_start_vertex_id].points;
            float closeDetectionRadius = 6.0f / editorCamera.zoom;

            if (CheckCollisionPointCircle(mouseWorldPos, startVertexPos, closeDetectionRadius)) {
                DrawCircleV(startVertexPos, vertexRadius + 4.0f, YELLOW);
                DrawLineEx(lastVertexPos, startVertexPos, 1.0f, YELLOW);
            } else {
                DrawLineEx(lastVertexPos, snappedVertex, 1.0f,GREEN);
            }
        }

        GuiLabel((Rectangle){110, 300, 200, 100}, TextFormat(editorMode == MODE_EDIT ? "Edit Mode" : "Drag Mode"));

        EndMode2D();
}