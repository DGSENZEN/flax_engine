#include "raylib.h"
#include "renderer/sectors.h"
#include "stdlib.h"
#include "map_editor/map.h"
#include <raymath.h>

#define WORLD_SCALE 0.06f

static bool g_built = false;

Vector3 SectorToWorld(Vector2 p, float y) {
    return (Vector3){ p.x * WORLD_SCALE, y, p.y * WORLD_SCALE };
}

static Model g_world;

static void PushVertex(Mesh *m, int *vi, int *ni, int *ci, Vector3 pos, Vector3 n, Color c){
    m->vertices[(*vi)++] = pos.x; m->vertices[(*vi)++] = pos.y; m->vertices[(*vi)++] = pos.z;
    m->normals[(*ni)++] = n.x; m->normals[(*ni)++] = n.y; m->normals[(*ni)++] = n.z;
    m->colors[(*ci)++] = c.r; m->colors[(*ci)++] = c.g; m->colors[(*ci)++] = c.b; m->colors[(*ci)++] = c.a;

}


static Color ShadeFlat(Vector3 normal, Color base){
    Vector3 lightDir = Vector3Normalize((Vector3){-0.5f, 1.0f, -0.3f});
    float d = Vector3DotProduct(normal, lightDir);
    float t = (d + 1) * 0.5;
    float brightness = Lerp(0.35, 1.0, t);

    return (Color){
        (unsigned char)(base.r * brightness),
        (unsigned char)(base.g * brightness),
        (unsigned char)(base.b * brightness),
        255
    };
};


void BuildSectorMeshes(){
    if (g_built) { UnloadModel(g_world); g_built = false; }
    //PASS A
    int triCount = 0;
    for (int s = 0; s < sector_counter; s++) {
        int start = sectors[s].wall_start, end = start + sectors[s].wall_count;
        triCount += 2 * (sectors[s].wall_count - 2);
        for (int w= start; w < end; w++) {
            if (walls[w].next_sector == NO_LINK) {
                triCount += 2;
            }
        }
    }
    if (triCount == 0) return;

    Mesh m = {0};
    m.vertexCount = triCount * 3;
    m.triangleCount = triCount;
    m.vertices = (float *)MemAlloc(m.vertexCount * 3 * sizeof(float));
    m.normals = (float *)MemAlloc(m.vertexCount * 3 * sizeof(float));
    m.colors = (unsigned char *)MemAlloc(m.vertexCount * 4 * sizeof(unsigned char));

    // PASS B
    int vi = 0, ni = 0, ci = 0;
    for (int s = 0; s < sector_counter; s++) {
        Sector sec = sectors[s];
        Vector3 centroid = {0};
        int start = sec.wall_start, end = start + sec.wall_count;
        int n = sectors[s].wall_count;
        Vector2 corner[n];
        for (int i = 0; i < n; i++) {
            corner[i] = vertices[walls[start+i].point_start].points;
            centroid = Vector3Add(centroid, SectorToWorld(corner[i], 0.0f));
        }
        centroid = Vector3Scale(centroid, 1.0f / n);
        for (int w = start; w < end; w++) {
            //this loop handles the walls
            if (walls[w].next_sector != NO_LINK) continue;
            Vector2 a = vertices[walls[w].point_start].points;
            Vector2 b = vertices[walls[w].point_end].points;

            Vector3 bl = SectorToWorld(a, sec.floor_z), br = SectorToWorld(b, sec.floor_z);
            Vector3 tr = SectorToWorld(b, sec.ceilingz), tl = SectorToWorld(a, sec.ceilingz);

            Vector3 along = Vector3Subtract(br, bl);
            Vector3 nrm = Vector3Normalize(Vector3CrossProduct(along, (Vector3){0,1,0}));
            Vector3 mid = Vector3Scale(Vector3Add(bl, br), 0.5f);
            bool flip = Vector3DotProduct(nrm, Vector3Subtract(centroid, mid)) < 0.0f;
            if (flip) nrm = Vector3Negate(nrm);
            Color base = ColorFromHSV(fmodf(w * 137.5f, 360.0f), 0.5f, 0.95);
            Color col = ShadeFlat(nrm, base);

            Vector3 front[6] = {bl, br, tr, bl, tr, tl};
            Vector3 back[6] = {bl, tr, br, bl, tl, tr};
            Vector3 *P = flip ? back : front;
            for(int i = 0; i < 6; i++) PushVertex(&m, &vi, &ni, &ci, P[i], nrm, col);
        }
            Vector3 f0 = SectorToWorld(corner[0], sec.floor_z);
            Vector3 f1 = SectorToWorld(corner[1], sec.floor_z);
            Vector3 f2 = SectorToWorld(corner[2], sec.floor_z);
            Vector3 testN = Vector3CrossProduct(Vector3Subtract(f1, f0), Vector3Subtract(f2, f0));
            bool floorReverse = testN.y < 0.0f;

            Vector3 up = {0,1,0}, down = {0, -1, 0};
            Color floorCol = ShadeFlat(up, (Color){120, 120, 140, 255});
            Color ceilCol = ShadeFlat(down, (Color){90, 80, 70, 255});
        //this loop handles the floor and ceiling rendering
            for (int i = 1; i < n - 1; i++) {
                Vector3 a = SectorToWorld(corner[0], sec.floor_z);
                Vector3 b = SectorToWorld(corner[i], sec.floor_z);
                Vector3 c = SectorToWorld(corner[i+1], sec.floor_z);
                if (!floorReverse) {
                    PushVertex(&m, &vi, &ni, &ci, a, up, floorCol);
                    PushVertex(&m, &vi, &ni, &ci, b, up, floorCol);
                    PushVertex(&m, &vi, &ni, &ci, c, up, floorCol);
                } else {
                    PushVertex(&m, &vi, &ni, &ci, a, up, floorCol);
                    PushVertex(&m, &vi, &ni, &ci, c, up, floorCol);
                    PushVertex(&m, &vi, &ni, &ci, b, up, floorCol);
                }

                Vector3 d = SectorToWorld(corner[0], sec.ceilingz);
                Vector3 e = SectorToWorld(corner[i], sec.ceilingz);
                Vector3 g = SectorToWorld(corner[i+1], sec.ceilingz);
                if (!floorReverse) {
                    PushVertex(&m, &vi, &ni, &ci, d, down, ceilCol);
                    PushVertex(&m, &vi, &ni, &ci, g, down, ceilCol);
                    PushVertex(&m, &vi, &ni, &ci, e, down, ceilCol);
                } else {
                    PushVertex(&m, &vi, &ni, &ci, d, down, ceilCol);
                    PushVertex(&m, &vi, &ni, &ci, e, down, ceilCol);
                    PushVertex(&m, &vi, &ni, &ci, g, down, ceilCol);
                }
            
            }
    }

    UploadMesh(&m, false);
    g_world = LoadModelFromMesh(m);
    g_built = true;
}

void DrawSectorWorld(){if (g_built) DrawModel(g_world, (Vector3){0}, 1.0f, WHITE);};


void UnloadSectorMeshes(){if (g_built) UnloadModel(g_world); g_built = false;};