#include "raylib.h"
#include "rlgl.h"
#include "renderer/sectors.h"
#include "renderer/textures.h"
#include "map_editor/map.h"
#include "core/log.h"
#include "config.h"
#include <raymath.h>

// ===========================================================================
// Flax Engine - sector world mesh builder.
//
// Bakes the sector/wall world into one static Model: one Mesh per distinct
// GPU texture, each paired with its own Material, so the whole world draws
// in a handful of batched calls with zero per-frame work. Two passes:
//
//   PASS A counts triangles per texture (exact allocation, no realloc).
//   PASS B emits vertices. The two passes must walk and skip identically.
//
// Lighting is 90s flat shading baked into vertex colors: the default raylib
// shader multiplies vertexColor * texture, which is exactly the sector-light
// look - no shader work, runs on anything.
// ===========================================================================

// World units per texture repeat. One grid cell (50 editor units * 0.06
// WORLD_SCALE) is 3 world units, so a texture tiles once per cell.
#define TEX_TILE 3.0f

static Model g_world;
static bool  g_built = false;

Vector3 SectorToWorld(Vector2 p, float y) {
    return (Vector3){ p.x * WORLD_SCALE, y, p.y * WORLD_SCALE };
}

// Per-texture write cursor: next vertex slot in that texture's mesh.
static int g_cursor[MAX_TEXNAMES];

static void PushVertex(Mesh *m, int idx, Vector3 pos, Vector3 n, Vector2 uv, Color c) {
    m->vertices [idx * 3 + 0] = pos.x;
    m->vertices [idx * 3 + 1] = pos.y;
    m->vertices [idx * 3 + 2] = pos.z;
    m->normals  [idx * 3 + 0] = n.x;
    m->normals  [idx * 3 + 1] = n.y;
    m->normals  [idx * 3 + 2] = n.z;
    m->texcoords[idx * 2 + 0] = uv.x;
    m->texcoords[idx * 2 + 1] = uv.y;
    m->colors   [idx * 4 + 0] = c.r;
    m->colors   [idx * 4 + 1] = c.g;
    m->colors   [idx * 4 + 2] = c.b;
    m->colors   [idx * 4 + 3] = c.a;
}

static Color ShadeFlat(Vector3 normal, Color base) {
    Vector3 lightDir = Vector3Normalize((Vector3){ -0.5f, 1.0f, -0.3f });
    float d = Vector3DotProduct(normal, lightDir);
    float brightness = Lerp(0.35f, 1.0f, (d + 1.0f) * 0.5f);
    return (Color){
        (unsigned char)(base.r * brightness),
        (unsigned char)(base.g * brightness),
        (unsigned char)(base.b * brightness),
        255
    };
}

// One vertical quad from edge (a,b), spanning yBottom..yTop, auto-faced
// toward the sector centroid. UVs: u runs along the wall in world units,
// v is anchored to absolute height - adjacent steps stay aligned.
static void EmitWallQuad(Mesh *m, int *cursor,
                         Vector2 a, Vector2 b, float yBottom, float yTop,
                         Vector3 centroid) {
    Vector3 bl = SectorToWorld(a, yBottom), br = SectorToWorld(b, yBottom);
    Vector3 tr = SectorToWorld(b, yTop),    tl = SectorToWorld(a, yTop);

    Vector3 along = Vector3Subtract(br, bl);
    Vector3 nrm   = Vector3Normalize(Vector3CrossProduct(along, (Vector3){ 0, 1, 0 }));
    Vector3 mid   = Vector3Scale(Vector3Add(bl, br), 0.5f);
    bool flip = Vector3DotProduct(nrm, Vector3Subtract(centroid, mid)) < 0.0f;
    if (flip) nrm = Vector3Negate(nrm);
    Color col = ShadeFlat(nrm, WHITE);

    float u1 = Vector3Length(along) / TEX_TILE;
    float vB = -yBottom / TEX_TILE, vT = -yTop / TEX_TILE;
    Vector2 uvBL = { 0, vB }, uvBR = { u1, vB }, uvTR = { u1, vT }, uvTL = { 0, vT };

    Vector3 P[6]; Vector2 T[6];
    if (!flip) {
        P[0]=bl; P[1]=br; P[2]=tr; P[3]=bl; P[4]=tr; P[5]=tl;
        T[0]=uvBL; T[1]=uvBR; T[2]=uvTR; T[3]=uvBL; T[4]=uvTR; T[5]=uvTL;
    } else {
        P[0]=bl; P[1]=tr; P[2]=br; P[3]=bl; P[4]=tl; P[5]=tr;
        T[0]=uvBL; T[1]=uvTR; T[2]=uvBR; T[3]=uvBL; T[4]=uvTL; T[5]=uvTR;
    }
    for (int i = 0; i < 6; i++) PushVertex(m, (*cursor)++, P[i], nrm, T[i], col);
}

// Flat triangle (floor/ceiling fan slice) with planar world-space UVs, so the
// texture grid lines up across every sector regardless of polygon shape.
static void EmitFlatTri(Mesh *m, int *cursor,
                        Vector3 a, Vector3 b, Vector3 c, Vector3 n, Color col) {
    Vector3 P[3] = { a, b, c };
    for (int i = 0; i < 3; i++) {
        Vector2 uv = { P[i].x / TEX_TILE, P[i].z / TEX_TILE };
        PushVertex(m, (*cursor)++, P[i], n, uv, col);
    }
}

// Resolve a wall's GPU texture index, with portal steps reusing the wall's
// texture (Build engine does the same: the riser inherits the wall).
static int WallTex(int w)   { return TexRegistryResolve(walls[w].texture_id); }
static int FloorTex(int s)  { return TexRegistryResolve(sectors[s].floor_texture); }
static int CeilTex(int s)   { return TexRegistryResolve(sectors[s].ceiling_texture); }

// Clear material texture references before UnloadModel: raylib's
// UnloadMaterial unloads any non-default texture it still points at, which
// would destroy the shared registry textures on every rebuild.
static void ReleaseWorldModel(void) {
    if (!g_built) return;
    for (int i = 0; i < g_world.materialCount; i++) {
        g_world.materials[i].maps[MATERIAL_MAP_DIFFUSE].texture = (Texture2D){
            rlGetTextureIdDefault(), 1, 1, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8
        };
    }
    UnloadModel(g_world);
    g_built = false;
}

void BuildSectorMeshes(void) {
    double t0 = GetTime();
    ReleaseWorldModel();

    int texCount = TexRegistryCount();
    if (texCount <= 0) return;

    // ---- PASS A: triangles per texture ------------------------------------
    static int tris[MAX_TEXNAMES];
    for (int t = 0; t < texCount; t++) tris[t] = 0;

    for (int s = 0; s < sector_counter; s++) {
        int n = sectors[s].wall_count;
        if (n < 3) continue;                              // skip degenerate sectors
        tris[FloorTex(s)] += n - 2;
        tris[CeilTex(s)]  += n - 2;

        int start = sectors[s].wall_start, end = start + n;
        for (int w = start; w < end; w++) {
            if (walls[w].next_sector == NO_LINK) {
                tris[WallTex(w)] += 2;                    // solid: full quad
            } else {
                Sector nb = sectors[walls[w].next_sector];
                if (nb.floor_z  > sectors[s].floor_z)  tris[WallTex(w)] += 2;  // lower step
                if (nb.ceilingz < sectors[s].ceilingz) tris[WallTex(w)] += 2;  // upper step
            }
        }
    }

    // ---- allocate one mesh per used texture --------------------------------
    static int slot[MAX_TEXNAMES];     // texture index -> mesh slot, or -1
    int meshCount = 0;
    for (int t = 0; t < texCount; t++) slot[t] = (tris[t] > 0) ? meshCount++ : -1;
    if (meshCount == 0) return;

    g_world = (Model){ 0 };
    g_world.transform     = MatrixIdentity();
    g_world.meshCount     = meshCount;
    g_world.materialCount = meshCount;
    g_world.meshes        = MemAlloc(meshCount * sizeof(Mesh));
    g_world.materials     = MemAlloc(meshCount * sizeof(Material));
    g_world.meshMaterial  = MemAlloc(meshCount * sizeof(int));

    for (int t = 0; t < texCount; t++) {
        if (slot[t] < 0) continue;
        Mesh *m = &g_world.meshes[slot[t]];
        m->triangleCount = tris[t];
        m->vertexCount   = tris[t] * 3;
        m->vertices  = MemAlloc(m->vertexCount * 3 * sizeof(float));
        m->normals   = MemAlloc(m->vertexCount * 3 * sizeof(float));
        m->texcoords = MemAlloc(m->vertexCount * 2 * sizeof(float));
        m->colors    = MemAlloc(m->vertexCount * 4 * sizeof(unsigned char));

        g_world.materials[slot[t]] = LoadMaterialDefault();
        g_world.materials[slot[t]].maps[MATERIAL_MAP_DIFFUSE].texture = TexRegistryTexture(t);
        g_world.meshMaterial[slot[t]] = slot[t];
        g_cursor[t] = 0;
    }

    // ---- PASS B: emit (must mirror PASS A's walk exactly) ------------------
    for (int s = 0; s < sector_counter; s++) {
        int n = sectors[s].wall_count;
        if (n < 3) continue;
        Sector sec = sectors[s];
        int start = sec.wall_start, end = start + n;

        Vector3 centroid = { 0 };
        Vector2 corner[n];
        for (int i = 0; i < n; i++) {
            corner[i] = vertices[walls[start + i].point_start].points;
            centroid = Vector3Add(centroid, SectorToWorld(corner[i], 0.0f));
        }
        centroid = Vector3Scale(centroid, 1.0f / n);

        for (int w = start; w < end; w++) {
            Vector2 a = vertices[walls[w].point_start].points;
            Vector2 b = vertices[walls[w].point_end].points;
            Mesh *m = &g_world.meshes[slot[WallTex(w)]];
            int  *cur = &g_cursor[WallTex(w)];

            if (walls[w].next_sector == NO_LINK) {
                EmitWallQuad(m, cur, a, b, sec.floor_z, sec.ceilingz, centroid);
            } else {
                Sector nb = sectors[walls[w].next_sector];
                if (nb.floor_z  > sec.floor_z)
                    EmitWallQuad(m, cur, a, b, sec.floor_z, nb.floor_z, centroid);
                if (nb.ceilingz < sec.ceilingz)
                    EmitWallQuad(m, cur, a, b, nb.ceilingz, sec.ceilingz, centroid);
            }
        }

        // winding test on the floor polygon decides fan orientation
        Vector3 f0 = SectorToWorld(corner[0], sec.floor_z);
        Vector3 f1 = SectorToWorld(corner[1], sec.floor_z);
        Vector3 f2 = SectorToWorld(corner[2], sec.floor_z);
        bool reverse = Vector3CrossProduct(Vector3Subtract(f1, f0),
                                           Vector3Subtract(f2, f0)).y < 0.0f;

        Vector3 up = { 0, 1, 0 }, down = { 0, -1, 0 };
        Color floorCol = ShadeFlat(up,   WHITE);
        Color ceilCol  = ShadeFlat(down, WHITE);
        Mesh *fm = &g_world.meshes[slot[FloorTex(s)]];
        Mesh *cm = &g_world.meshes[slot[CeilTex(s)]];

        for (int i = 1; i < n - 1; i++) {
            Vector3 a = SectorToWorld(corner[0],     sec.floor_z);
            Vector3 b = SectorToWorld(corner[i],     sec.floor_z);
            Vector3 c = SectorToWorld(corner[i + 1], sec.floor_z);
            if (!reverse) EmitFlatTri(fm, &g_cursor[FloorTex(s)], a, b, c, up, floorCol);
            else          EmitFlatTri(fm, &g_cursor[FloorTex(s)], a, c, b, up, floorCol);

            Vector3 d = SectorToWorld(corner[0],     sec.ceilingz);
            Vector3 e = SectorToWorld(corner[i],     sec.ceilingz);
            Vector3 g = SectorToWorld(corner[i + 1], sec.ceilingz);
            if (!reverse) EmitFlatTri(cm, &g_cursor[CeilTex(s)], d, g, e, down, ceilCol);
            else          EmitFlatTri(cm, &g_cursor[CeilTex(s)], d, e, g, down, ceilCol);
        }
    }

    for (int i = 0; i < meshCount; i++) UploadMesh(&g_world.meshes[i], false);
    g_built = true;

    int totalTris = 0;
    for (int t = 0; t < texCount; t++) totalTris += tris[t];
    FLOG_INFO(LOGCAT_RENDER, "world bake: %d sectors -> %d meshes, %d tris in %.2fms",
              sector_counter, meshCount, totalTris, (GetTime() - t0) * 1000.0);
}

void DrawSectorWorld(void) { if (g_built) DrawModel(g_world, (Vector3){ 0 }, 1.0f, WHITE); }

void UnloadSectorMeshes(void) { ReleaseWorldModel(); }

void SectorWorldStats(int *mesh_count, int *triangle_count) {
    int tris = 0;
    if (g_built)
        for (int i = 0; i < g_world.meshCount; i++) tris += g_world.meshes[i].triangleCount;
    *mesh_count = g_built ? g_world.meshCount : 0;
    *triangle_count = tris;
}
