#include "raylib.h"
#include "rlgl.h"
#include "renderer/sectors.h"
#include "renderer/textures.h"
#include "renderer/lights.h"
#include "world/triangulate.h"
#include "map_editor/map.h"
#include "core/log.h"
#include "core/profiler.h"
#include "config.h"
#include <raymath.h>
#include <string.h>

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

// Tessellation cap for CPU vertex lighting: no edge longer than this many
// world units, so a dynamic light always finds vertices inside its radius.
// Smaller = smoother light falloff, more triangles to relight.
#define LIGHT_TESS 8.0f

static Model g_world;
static bool  g_built = false;

// CPU copy of each mesh's unlit colors (flat shade * sector light). The
// relight pass starts from these and adds the dynamic lights on top.
static unsigned char *g_base[MAX_TEXNAMES];

Vector3 SectorToWorld(Vector2 p, float y) {
    return (Vector3){ p.x * WORLD_SCALE, y, p.y * WORLD_SCALE };
}

// Per-texture write cursor: next vertex slot in that texture's mesh.
static int g_cursor[MAX_TEXNAMES];

static int EmitFlatTriSub(Mesh *m, int *cursor,
                          Vector3 a, Vector3 b, Vector3 c,
                          Vector3 n, Color col, int depth);

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

// Flat shade from the face normal, scaled by the sector's light level.
// Light can exceed 1 (overbright headroom), so channels clamp.
static Color ShadeFlat(Vector3 normal, Color base, float light) {
    Vector3 lightDir = Vector3Normalize((Vector3){ -0.5f, 1.0f, -0.3f });
    float d = Vector3DotProduct(normal, lightDir);
    float brightness = Lerp(0.35f, 1.0f, (d + 1.0f) * 0.5f) * light;
    int r = (int)(base.r * brightness), g = (int)(base.g * brightness);
    int b = (int)(base.b * brightness);
    return (Color){ r > 255 ? 255 : (unsigned char)r,
                    g > 255 ? 255 : (unsigned char)g,
                    b > 255 ? 255 : (unsigned char)b, 255 };
}

// Triangulated floor plan of sector s (all loops: concave outlines, holes).
// Scratch is shared between the two mesh passes; both call this for the
// same sector, so the counts they see are identical by construction.
#define MAX_FLAT_TRIS 2048
static Vector2 g_flat[MAX_FLAT_TRIS * 3];

static int SectorFlatTris(int s) {
    int lc[16];
    int nl = WorldSectorLoops(s, lc, 16);
    if (nl < 1 || lc[0] < 3) return 0;

    static Vector2 pts[512];
    int n = sectors[s].wall_count;
    if (n > 512) return 0;
    for (int i = 0; i < n; i++)
        pts[i] = vertices[walls[sectors[s].wall_start + i].point_start].points;
    return TriangulatePolygon(pts, lc, nl, g_flat, MAX_FLAT_TRIS);
}

// Floor + ceiling of sector s: triangulate, lift to slope-aware heights,
// tessellate. NULL meshes count without emitting; both passes come through
// here, so allocation and emission can never disagree. The triangulator
// winds CCW in map space, which faces down in world space - the floor
// reverses its winding, the ceiling keeps it.
static void SectorFlats(int s, Mesh *fm, int *fcur, Mesh *cm, int *ccur,
                        int *ftris, int *ctris) {
    *ftris = *ctris = 0;
    int ntri = SectorFlatTris(s);
    if (ntri == 0) return;

    Vector3 up   = WorldFloorNormal(s);
    Vector3 down = WorldCeilNormal(s);
    Color fc = ShadeFlat(up,   WHITE, sectors[s].light);
    Color cc = ShadeFlat(down, WHITE, sectors[s].light);

    for (int t = 0; t < ntri; t++) {
        Vector2 p0 = g_flat[t * 3], p1 = g_flat[t * 3 + 1], p2 = g_flat[t * 3 + 2];
        Vector3 f0 = SectorToWorld(p0, WorldFloorZAt(s, p0));
        Vector3 f1 = SectorToWorld(p1, WorldFloorZAt(s, p1));
        Vector3 f2 = SectorToWorld(p2, WorldFloorZAt(s, p2));
        *ftris += EmitFlatTriSub(fm, fcur, f0, f2, f1, up, fc, 0);

        Vector3 c0 = SectorToWorld(p0, WorldCeilZAt(s, p0));
        Vector3 c1 = SectorToWorld(p1, WorldCeilZAt(s, p1));
        Vector3 c2 = SectorToWorld(p2, WorldCeilZAt(s, p2));
        *ctris += EmitFlatTriSub(cm, ccur, c0, c1, c2, down, cc, 0);
    }
}

// A wall face's vertical extent at each endpoint. Slopes make the two ends
// differ; a span degenerate at one end (a0==a1) still emits, its dead
// triangle collapsing to zero area.
typedef struct { float a0, a1, b0, b1; } WallSpan;   // bottom/top at a, at b

// The 0-2 faces wall w of sector s contributes: the full face when solid,
// the lower/upper steps against the neighbor when it's a portal. Heights are
// evaluated per endpoint so sloped sectors meet their neighbors exactly.
// Both mesh passes call this, so counting and emission can never diverge.
static int WallSpans(int s, int w, WallSpan out[2]) {
    Vector2 a = vertices[walls[w].point_start].points;
    Vector2 b = vertices[walls[w].point_end].points;
    float fA = WorldFloorZAt(s, a), fB = WorldFloorZAt(s, b);
    float cA = WorldCeilZAt(s, a),  cB = WorldCeilZAt(s, b);

    int ns = walls[w].next_sector, n = 0;
    if (ns == NO_LINK) {
        if (cA > fA + 0.001f || cB > fB + 0.001f)
            out[n++] = (WallSpan){ fA, cA, fB, cB };
        return n;
    }
    float tA = Clamp(WorldFloorZAt(ns, a), fA, cA);    // lower step: our floor
    float tB = Clamp(WorldFloorZAt(ns, b), fB, cB);    // up to their floor
    if (tA > fA + 0.001f || tB > fB + 0.001f)
        out[n++] = (WallSpan){ fA, tA, fB, tB };

    float bA = Clamp(WorldCeilZAt(ns, a), fA, cA);     // upper step: their
    float bB = Clamp(WorldCeilZAt(ns, b), fB, cB);     // ceiling up to ours
    if (bA < cA - 0.001f || bB < cB - 0.001f)
        out[n++] = (WallSpan){ bA, cA, bB, cB };
    return n;
}

// Tessellation grid for one wall span. Shared by counting and emission.
static void WallGrid(Vector2 a, Vector2 b, WallSpan sp, int *nu, int *nv) {
    float ulen = Vector2Distance(a, b) * WORLD_SCALE;
    float vlen = fmaxf(sp.a1 - sp.a0, sp.b1 - sp.b0);
    *nu = (int)ceilf(ulen / LIGHT_TESS); if (*nu < 1) *nu = 1;
    *nv = (int)ceilf(vlen / LIGHT_TESS); if (*nv < 1) *nv = 1;
}

static int WallSpanTris(Vector2 a, Vector2 b, WallSpan sp) {
    int nu, nv;
    WallGrid(a, b, sp, &nu, &nv);
    return 2 * nu * nv;
}

// One vertical face from edge (a,b) with per-endpoint bottom/top heights,
// tessellated into a LIGHT_TESS grid for the CPU relight pass. Facing:
// toward faceRef for outer-loop walls, away from it for hole walls (faceRef
// is the wall's own loop centroid either way). UVs: u runs along the wall
// in world units, v is anchored to absolute height - steps stay aligned.
static void EmitWallQuad(Mesh *m, int *cursor,
                         Vector2 a, Vector2 b, WallSpan sp,
                         Vector3 faceRef, bool faceAway, float light) {
    Vector3 along = Vector3Subtract(SectorToWorld(b, 0), SectorToWorld(a, 0));
    Vector3 nrm   = Vector3Normalize(Vector3CrossProduct(along, (Vector3){ 0, 1, 0 }));
    Vector3 mid   = Vector3Scale(Vector3Add(SectorToWorld(a, sp.a0),
                                            SectorToWorld(b, sp.b0)), 0.5f);
    bool toward = Vector3DotProduct(nrm, Vector3Subtract(faceRef, mid)) >= 0.0f;
    bool flip = faceAway ? toward : !toward;
    if (flip) nrm = Vector3Negate(nrm);
    Color col = ShadeFlat(nrm, WHITE, light);

    float ulen = Vector3Length(along);
    int nu, nv;
    WallGrid(a, b, sp, &nu, &nv);

    for (int iu = 0; iu < nu; iu++) {
        float f0 = (float)iu / (float)nu, f1 = (float)(iu + 1) / (float)nu;
        Vector2 p0 = Vector2Lerp(a, b, f0), p1 = Vector2Lerp(a, b, f1);
        float bot0 = Lerp(sp.a0, sp.b0, f0), top0 = fmaxf(bot0, Lerp(sp.a1, sp.b1, f0));
        float bot1 = Lerp(sp.a0, sp.b0, f1), top1 = fmaxf(bot1, Lerp(sp.a1, sp.b1, f1));

        for (int iv = 0; iv < nv; iv++) {
            float g0 = (float)iv / (float)nv, g1 = (float)(iv + 1) / (float)nv;
            float y00 = Lerp(bot0, top0, g0), y01 = Lerp(bot0, top0, g1);
            float y10 = Lerp(bot1, top1, g0), y11 = Lerp(bot1, top1, g1);

            Vector3 bl = SectorToWorld(p0, y00), br = SectorToWorld(p1, y10);
            Vector3 tr = SectorToWorld(p1, y11), tl = SectorToWorld(p0, y01);
            float u0 = f0 * ulen / TEX_TILE, u1 = f1 * ulen / TEX_TILE;
            Vector2 uvBL = { u0, -y00 / TEX_TILE }, uvBR = { u1, -y10 / TEX_TILE };
            Vector2 uvTR = { u1, -y11 / TEX_TILE }, uvTL = { u0, -y01 / TEX_TILE };

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
    }
}

// Flat triangle (floor/ceiling) with planar world-space UVs, so the
// texture grid lines up across every sector regardless of polygon shape.
static void EmitFlatTri(Mesh *m, int *cursor,
                        Vector3 a, Vector3 b, Vector3 c, Vector3 n, Color col) {
    Vector3 P[3] = { a, b, c };
    for (int i = 0; i < 3; i++) {
        Vector2 uv = { P[i].x / TEX_TILE, P[i].z / TEX_TILE };
        PushVertex(m, (*cursor)++, P[i], n, uv, col);
    }
}

static float Edge2(Vector3 p, Vector3 q) {
    float dx = q.x - p.x, dy = q.y - p.y, dz = q.z - p.z;
    return dx * dx + dy * dy + dz * dz;
}

// 4-way midpoint subdivision until every edge is under LIGHT_TESS (midpoints
// of a plane stay on the plane, so slopes survive). m == NULL counts without
// emitting - both mesh passes run the identical recursion.
static int EmitFlatTriSub(Mesh *m, int *cursor,
                          Vector3 a, Vector3 b, Vector3 c,
                          Vector3 n, Color col, int depth) {
    float lim = LIGHT_TESS * LIGHT_TESS;
    if (depth >= 8 ||
        (Edge2(a, b) <= lim && Edge2(b, c) <= lim && Edge2(c, a) <= lim)) {
        if (m) EmitFlatTri(m, cursor, a, b, c, n, col);
        return 1;
    }
    Vector3 ab = Vector3Scale(Vector3Add(a, b), 0.5f);
    Vector3 bc = Vector3Scale(Vector3Add(b, c), 0.5f);
    Vector3 ca = Vector3Scale(Vector3Add(c, a), 0.5f);
    return EmitFlatTriSub(m, cursor, a, ab, ca, n, col, depth + 1)
         + EmitFlatTriSub(m, cursor, ab, b, bc, n, col, depth + 1)
         + EmitFlatTriSub(m, cursor, ca, bc, c, n, col, depth + 1)
         + EmitFlatTriSub(m, cursor, ab, bc, ca, n, col, depth + 1);
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
    for (int i = 0; i < g_world.meshCount; i++) {
        if (g_base[i]) { MemFree(g_base[i]); g_base[i] = NULL; }
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
        int ft, ct;
        SectorFlats(s, NULL, NULL, NULL, NULL, &ft, &ct);
        tris[FloorTex(s)] += ft;
        tris[CeilTex(s)]  += ct;

        int start = sectors[s].wall_start, end = start + n;
        for (int w = start; w < end; w++) {
            Vector2 a = vertices[walls[w].point_start].points;
            Vector2 b = vertices[walls[w].point_end].points;
            WallSpan spans[2];
            int count = WallSpans(s, w, spans);
            for (int k = 0; k < count; k++)
                tris[WallTex(w)] += WallSpanTris(a, b, spans[k]);
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
        int start = sec.wall_start;

        // per-loop centroids: outer-loop walls face their loop's center,
        // hole walls face away from theirs (out into the room)
        int lc[16];
        int nl = WorldSectorLoops(s, lc, 16);
        Vector3 loopRef[16];
        int loop_at[16];                    // run offset where each loop starts
        for (int k = 0, off = 0; k < nl; k++) {
            Vector3 c = { 0 };
            for (int i = 0; i < lc[k]; i++)
                c = Vector3Add(c, SectorToWorld(
                        vertices[walls[start + off + i].point_start].points, 0.0f));
            loopRef[k] = Vector3Scale(c, 1.0f / (float)lc[k]);
            loop_at[k] = off;
            off += lc[k];
        }

        for (int i = 0; i < n; i++) {
            int w = start + i;
            int k = 0;
            while (k + 1 < nl && i >= loop_at[k + 1]) k++;

            Vector2 a = vertices[walls[w].point_start].points;
            Vector2 b = vertices[walls[w].point_end].points;
            Mesh *m = &g_world.meshes[slot[WallTex(w)]];
            int  *cur = &g_cursor[WallTex(w)];

            WallSpan spans[2];
            int count = WallSpans(s, w, spans);
            for (int j = 0; j < count; j++)
                EmitWallQuad(m, cur, a, b, spans[j], loopRef[k], k > 0, sec.light);
        }

        int ft, ct;
        SectorFlats(s, &g_world.meshes[slot[FloorTex(s)]], &g_cursor[FloorTex(s)],
                       &g_world.meshes[slot[CeilTex(s)]],  &g_cursor[CeilTex(s)],
                    &ft, &ct);
    }

    // dynamic upload: the relight pass rewrites the color buffer per frame
    for (int i = 0; i < meshCount; i++) UploadMesh(&g_world.meshes[i], true);

    // snapshot the unlit colors the relight pass restores and builds on
    for (int i = 0; i < meshCount; i++) {
        int bytes = g_world.meshes[i].vertexCount * 4;
        g_base[i] = MemAlloc(bytes);
        memcpy(g_base[i], g_world.meshes[i].colors, bytes);
    }
    g_built = true;

    int totalTris = 0;
    for (int t = 0; t < texCount; t++) totalTris += tris[t];
    FLOG_INFO(LOGCAT_RENDER, "world bake: %d sectors -> %d meshes, %d tris in %.2fms",
              sector_counter, meshCount, totalTris, (GetTime() - t0) * 1000.0);
}

// CPU vertex lighting: rebuild every vertex color as base + dlights and
// re-upload the color buffers. With no lights active it restores the bases
// once and then costs nothing per frame.
void SectorWorldRelight(void) {
    if (!g_built) return;

    Vector3 lp[MAX_DLIGHTS], lc[MAX_DLIGHTS];
    float   lr[MAX_DLIGHTS];
    int nl = LightsGather(lp, lc, lr, MAX_DLIGHTS);

    static bool was_lit = false;
    if (nl == 0 && !was_lit) return;
    PROF_BEGIN("relight");

    for (int i = 0; i < g_world.meshCount; i++) {
        Mesh *m = &g_world.meshes[i];
        if (nl == 0) {
            memcpy(m->colors, g_base[i], (size_t)m->vertexCount * 4);
        } else {
            for (int v = 0; v < m->vertexCount; v++) {
                float px = m->vertices[v * 3], py = m->vertices[v * 3 + 1],
                      pz = m->vertices[v * 3 + 2];
                int r = g_base[i][v * 4], g = g_base[i][v * 4 + 1],
                    b = g_base[i][v * 4 + 2];
                for (int L = 0; L < nl; L++) {
                    float dx = px - lp[L].x, dy = py - lp[L].y, dz = pz - lp[L].z;
                    float d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 >= lr[L] * lr[L]) continue;
                    float a = 1.0f - sqrtf(d2) / lr[L];
                    a *= a;
                    r += (int)(lc[L].x * 255.0f * a);
                    g += (int)(lc[L].y * 255.0f * a);
                    b += (int)(lc[L].z * 255.0f * a);
                }
                m->colors[v * 4]     = r > 255 ? 255 : (unsigned char)r;
                m->colors[v * 4 + 1] = g > 255 ? 255 : (unsigned char)g;
                m->colors[v * 4 + 2] = b > 255 ? 255 : (unsigned char)b;
            }
        }
        // buffer 3 = vertex colors in raylib's mesh VBO layout
        UpdateMeshBuffer(*m, 3, m->colors, m->vertexCount * 4, 0);
    }
    was_lit = nl > 0;
    PROF_END("relight");
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
