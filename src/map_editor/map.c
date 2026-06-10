#include "map.h"
#include "config.h"
#include "io/flaxmap.h"
#include "world/triangulate.h"
#include "renderer/textures.h"
#include "core/log.h"
#include "raylib.h"
#include "raymath.h"
#include "raygui.h"
#include "rlgl.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

// ===========================================================================
// Flax Engine - 2D sector map editor.
//
// Build/Doom-style data model: flat fixed arrays of vertices, walls and
// sectors, referenced by integer index (a stable handle). Nothing here
// allocates per frame. A wall is one edge of a sector loop; two walls that
// share an edge in opposite directions form a portal (a two-sided wall).
//
// The frame runs in three phases - input, update, render - over a single
// EditorState. Keeping mutation (input/update) out of the draw pass is what
// makes the editor easy to reason about and extend.
// ===========================================================================

#define INVALID_ID 0

#define GRID_SPACING 50.0f
#define GRID_MAJOR_EVERY 8        // a brighter grid line every N cells

#define SECTOR_VERTEX_MIN 3       // smallest closed loop we accept

// chrome dimensions (screen space)
#define HEADER_H 28
#define STATUS_H 26
#define PANEL_W  300
#define PAD      12

// editor palette (dark phosphor / DOS feel)
static const Color COL_BG        = { 12, 14, 20, 255 };
static const Color COL_GRID_MIN  = { 28, 34, 42, 255 };
static const Color COL_GRID_MAJ  = { 44, 66, 78, 255 };
static const Color COL_AXIS      = { 70, 120, 90, 255 };
static const Color COL_WALL      = { 210, 220, 230, 255 };
static const Color COL_PORTAL    = { 200, 70, 70, 255 };
static const Color COL_VERTEX    = { 235, 200, 90, 255 };
static const Color COL_VERTEX_HI = { 255, 255, 255, 255 };
static const Color COL_SEL       = { 90, 150, 200, 60 };
static const Color COL_SEL_LINE  = { 120, 190, 235, 255 };
static const Color COL_TEXT      = { 150, 220, 180, 255 };

// chrome: beveled "DOS tool" panels - raised face with a light top/left edge
// and a dark bottom/right edge, sunken insets invert it.
static const Color COL_FACE      = { 22, 27, 36, 255 };   // panel face
static const Color COL_LIGHT     = { 58, 74, 90, 255 };   // bevel highlight
static const Color COL_DARK      = {  4,  6, 10, 255 };   // bevel shadow
static const Color COL_HEADER    = { 26, 44, 52, 255 };   // section / title bar fill
static const Color COL_HOT       = { 190, 255, 210, 255 };// bright phosphor
static const Color COL_DIM       = { 110, 150, 128, 255 };// dim phosphor

// World data (vertices/walls/sectors) is defined in world/world.c and shared
// with the renderer and the io/flaxmap serializer.

// ---------------------------------------------------------------------------
// Editor state - everything the editor mutates lives here, not in scattered
// file globals and function statics.
// ---------------------------------------------------------------------------
typedef enum {
    MODE_EDIT = 0,
    MODE_DRAG,
    MODE_SECTOR,
    MODE_WALL,
    MODE_HOLE,
    MODE_ENTITY
} EditorMode;

typedef struct {
    Camera2D   camera;
    EditorMode mode;

    bool drawing;          // mid-way through laying out a sector loop
    int  start_vertex;     // first vertex of that loop
    int  start_wall;       // wall_counter when the loop began

    bool    rect_dragging; // shift-drag rectangle tool in EDIT mode
    bool    rect_stairs;   // ctrl+shift variant: stamp a staircase instead
    Vector2 rect_anchor;   // first corner of that rectangle (snapped)

    int  dragged_vertex;   // vertex being moved, or -1
    int  dragged_entity;   // entity being moved, or -1
    int  selected_sector;  // sector under inspection, or -1
    int  selected_wall;    // wall under inspection (WALL mode), or -1
    int  selected_entity;  // entity under inspection, or -1
    int  entity_brush_type;

    bool geometry_dirty;   // portals are rebuilt only when this is set

    char  status_msg[64];  // transient save/load feedback in the status bar
    float status_timer;
} EditorState;

static EditorState g_ed = {
    .mode = MODE_EDIT,
    .drawing = false,
    .start_vertex = -1,
    .start_wall = -1,
    .rect_dragging = false,
    .dragged_vertex = -1,
    .dragged_entity = -1,
    .selected_sector = -1,
    .selected_wall = -1,
    .selected_entity = -1,
    .entity_brush_type = ENT_AMMO,
    .geometry_dirty = true,        // force a rebuild on first frame
};

static bool g_style_ready = false;

// ===========================================================================
// World helpers
// ===========================================================================

static void PruneOrphanVertices(void);
static void EditorStatus(EditorState *st, const char *msg);

static uint32_t MakeUniqueId(void) {
    static uint32_t current_id = INVALID_ID;
    if (current_id == UINT32_MAX) {
        fprintf(stderr, "Error: Max number of unique IDs reached!\n");
        return INVALID_ID;
    }
    return ++current_id;
}

static Vector2 SnapVertexToGrid(Vector2 v, float grid) {
    return (Vector2){ roundf(v.x / grid) * grid, roundf(v.y / grid) * grid };
}

// Reuse a vertex at this exact grid point if one exists, else append a new one.
static int GetOrCreateVertex(Vector2 p) {
    for (int i = 0; i < vertex_counter; i++) {
        if (vertices[i].points.x == p.x && vertices[i].points.y == p.y) return i;
    }
    if (vertex_counter >= MAX_VERTICES) return -1;
    int idx = vertex_counter++;
    vertices[idx] = (Vertex){ .id = MakeUniqueId(), .points = p };
    return idx;
}

// Portal linking lives in io/flaxmap.c (MapCompilePortals) - it is the same
// "compile" pass the offline baker runs, so editor and tool can never drift.

static int SectorUnderPoint(Vector2 p) {
    for (int s = 0; s < sector_counter; s++) {
        int start = sectors[s].wall_start, n = sectors[s].wall_count;
        if (n < 3) continue;
        Vector2 pts[MAX_WALLS];                 // bounded scratch; n <= wall_counter
        for (int i = 0; i < n; i++) pts[i] = vertices[walls[start + i].point_start].points;
        if (CheckCollisionPointPoly(p, pts, n)) return s;
    }
    return -1;
}

// How many of a sector's walls are portals (two-sided). Used for the info panel.
static int SectorPortalCount(int s) {
    int start = sectors[s].wall_start, n = sectors[s].wall_count, count = 0;
    for (int i = 0; i < n; i++) if (walls[start + i].next_sector != NO_LINK) count++;
    return count;
}

static float PointSegDistSq(Vector2 p, Vector2 a, Vector2 b) {
    Vector2 ab = Vector2Subtract(b, a);
    float lenSq = Vector2LengthSqr(ab);
    float t = lenSq > 0.0f ? Clamp(Vector2DotProduct(Vector2Subtract(p, a), ab) / lenSq, 0.0f, 1.0f) : 0.0f;
    return Vector2DistanceSqr(p, Vector2Add(a, Vector2Scale(ab, t)));
}

// Nearest wall within radius r of point p, or -1.
static int WallUnderPoint(Vector2 p, float r) {
    int best = -1;
    float bestSq = r * r;
    for (int w = 0; w < wall_counter; w++) {
        float dSq = PointSegDistSq(p, vertices[walls[w].point_start].points,
                                      vertices[walls[w].point_end].points);
        if (dSq < bestSq) { bestSq = dSq; best = w; }
    }
    return best;
}

static int EntityUnderPoint(Vector2 p, float r) {
    int best = -1;
    float bestSq = r * r;
    for (int e = 0; e < entity_counter; e++) {
        float dSq = Vector2DistanceSqr(p, entities[e].pos);
        if (dSq < bestSq) { bestSq = dSq; best = e; }
    }
    return best;
}

static Entity EntityDefault(int type, Vector2 p) {
    Entity e = { .type = type, .pos = p, .texture_id = 0 };
    switch (type) {
    case ENT_DECAL:  e.z = 4.0f; e.sx = 2.0f;  e.sy = 2.0f;  break;
    case ENT_SPAWN:  e.yaw = playerStartYaw;                         break;
    case ENT_BRIDGE: e.z = 2.0f; e.sx = 2.0f;  e.sy = 2.0f;  break;
    case ENT_HEALTH: e.data = 25;                                    break;
    case ENT_AMMO:
    default:         e.type = ENT_AMMO; e.data = AMMO_SHELLS; e.sx = 8.0f; break;
    }
    return e;
}

static void DeleteEntity(EditorState *st, int idx) {
    if (idx < 0 || idx >= entity_counter) return;
    memmove(&entities[idx], &entities[idx + 1], (size_t)(entity_counter - idx - 1) * sizeof(Entity));
    entity_counter--;
    st->selected_entity = -1;
    st->dragged_entity = -1;
    EditorStatus(st, "ENTITY DELETED");
}

// Step a world texture id forward/back through the texture registry. The
// world stores names, so cycling goes registry index -> name -> world id.
static int CycleWorldTexture(int world_id, int dir) {
    int n = TexRegistryCount();
    if (n == 0) return world_id;
    int idx = (TexRegistryResolve(world_id) + dir + n) % n;
    return WorldTexnameId(TexRegistryName(idx));
}

static void EditorStatus(EditorState *st, const char *msg) {
    strncpy(st->status_msg, msg, sizeof st->status_msg - 1);
    st->status_msg[sizeof st->status_msg - 1] = '\0';
    st->status_timer = 3.0f;
}

// Rectangle tool: stamp a 4-wall sector between two snapped corners. Always
// emitted in the same winding, so two adjacent stamped rooms traverse their
// shared edge in opposite directions and the portal compiler links them.
static void CreateRectSectorEx(EditorState *st, Vector2 p, Vector2 q,
                               float floor_z, float ceil_z) {
    if (p.x == q.x || p.y == q.y) return;                      // zero-area
    if (wall_counter + 4 > MAX_WALLS || sector_counter >= MAX_SECTORS) return;

    float x0 = fminf(p.x, q.x), x1 = fmaxf(p.x, q.x);
    float y0 = fminf(p.y, q.y), y1 = fmaxf(p.y, q.y);
    Vector2 c[4] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };

    int v[4];
    for (int i = 0; i < 4; i++) {
        v[i] = GetOrCreateVertex(c[i]);
        if (v[i] == -1) return;
    }
    int start = wall_counter;
    for (int i = 0; i < 4; i++) {
        walls[wall_counter++] = (Wall){
            .point_start = v[i], .point_end = v[(i + 1) % 4],
            .next_wall = start + (i + 1) % 4,
            .next_sector = NO_LINK, .portal_wall = NO_LINK, .texture_id = 0,
        };
    }
    sectors[sector_counter++] = (Sector){
        .wall_start = start, .wall_count = 4,
        .floor_z = floor_z, .ceilingz = ceil_z,
        .floor_texture = 0, .ceiling_texture = 0,
        .light = 1.0f,
    };
    st->geometry_dirty = true;
}

static void CreateRectSector(EditorState *st, Vector2 p, Vector2 q) {
    CreateRectSectorEx(st, p, q, 0.0f, 10.0f);
}

#define STAIR_RISE 1.0f    // per step, comfortably under the player's 1.8 step-up

// Stair tool: slice the dragged rect into one-grid-cell steps along its
// dominant axis, each its own sector, floors ascending from the anchor end.
// Shared edges reuse the same grid vertices, so the portal compiler links
// every riser - the player just walks up via step-up.
static void StampStairs(EditorState *st, Vector2 p, Vector2 q) {
    float dx = q.x - p.x, dy = q.y - p.y;
    if (dx == 0.0f || dy == 0.0f) return;
    bool alongX = fabsf(dx) >= fabsf(dy);
    int steps = (int)roundf((alongX ? fabsf(dx) : fabsf(dy)) / GRID_SPACING);
    if (steps < 2) { CreateRectSector(st, p, q); return; }
    if (wall_counter + 4 * steps > MAX_WALLS ||
        sector_counter + steps > MAX_SECTORS) return;

    float dir = (alongX ? dx : dy) > 0.0f ? 1.0f : -1.0f;
    for (int i = 0; i < steps; i++) {
        float lo = (float)i * GRID_SPACING * dir;
        float hi = (float)(i + 1) * GRID_SPACING * dir;
        Vector2 c0, c1;
        if (alongX) { c0 = (Vector2){ p.x + lo, p.y }; c1 = (Vector2){ p.x + hi, q.y }; }
        else        { c0 = (Vector2){ p.x, p.y + lo }; c1 = (Vector2){ q.x, p.y + hi }; }
        float f = (float)i * STAIR_RISE;
        CreateRectSectorEx(st, c0, c1, f, f + 10.0f);
    }
}

// next_wall is the intra-loop chain; any structural edit (hole insertion,
// wall split, hinge rotation) re-derives every sector's chains from the
// point connectivity.
static void RebuildNextWalls(void) {
    for (int s = 0; s < sector_counter; s++) {
        int lc[16];
        int nl = WorldSectorLoops(s, lc, 16);
        int off = sectors[s].wall_start;
        for (int k = 0; k < nl; k++) {
            for (int i = 0; i < lc[k]; i++)
                walls[off + i].next_wall = off + (i + 1) % lc[k];
            off += lc[k];
        }
    }
}

// Move a sector's hinge (slope reference line) to its next wall by rotating
// the wall records one slot within the sector's FIRST loop (holes keep
// their place). Portal back-references shift, fixed by the usual recompile.
static void RotateSectorHinge(EditorState *st, int s) {
    int lc[16];
    int nl = WorldSectorLoops(s, lc, 16);
    if (nl < 1 || lc[0] < 2) return;
    int start = sectors[s].wall_start, n = lc[0];
    Wall first = walls[start];
    for (int i = 0; i < n - 1; i++) walls[start + i] = walls[start + i + 1];
    walls[start + n - 1] = first;
    RebuildNextWalls();
    st->geometry_dirty = true;
}

static int SectorOwningWall(int w) {
    for (int s = 0; s < sector_counter; s++)
        if (w >= sectors[s].wall_start &&
            w <  sectors[s].wall_start + sectors[s].wall_count) return s;
    return -1;
}

// Insert a vertex into wall w, making two walls in its sector's run.
static bool SplitWallAt(int w, Vector2 p) {
    int s = SectorOwningWall(w);
    if (s < 0 || wall_counter >= MAX_WALLS) return false;
    int nv = GetOrCreateVertex(p);
    if (nv == -1 || nv == walls[w].point_start || nv == walls[w].point_end) return false;

    memmove(&walls[w + 2], &walls[w + 1], (size_t)(wall_counter - (w + 1)) * sizeof(Wall));
    wall_counter++;
    walls[w + 1] = walls[w];                  // second half inherits texture/flags
    walls[w + 1].point_start = nv;
    walls[w + 1].next_sector = NO_LINK;       // links recompile after the edit
    walls[w + 1].portal_wall = NO_LINK;
    walls[w].point_end = nv;
    walls[w].next_sector = NO_LINK;
    walls[w].portal_wall = NO_LINK;

    sectors[s].wall_count++;
    for (int r = 0; r < sector_counter; r++)
        if (r != s && sectors[r].wall_start > w) sectors[r].wall_start++;
    return true;
}

// Split the selected wall at the cursor's projection onto it. A portal's
// partner wall splits at the same point, so both pairs of halves share
// exact edges again and the compiler relinks them.
static void SplitSelectedWall(EditorState *st, Vector2 mouseWorld) {
    int w = st->selected_wall;
    if (w < 0 || w >= wall_counter) return;
    Vector2 a = vertices[walls[w].point_start].points;
    Vector2 b = vertices[walls[w].point_end].points;
    Vector2 ab = Vector2Subtract(b, a);
    float lenSq = Vector2LengthSqr(ab);
    if (lenSq == 0.0f) return;
    float t = Clamp(Vector2DotProduct(Vector2Subtract(mouseWorld, a), ab) / lenSq,
                    0.1f, 0.9f);
    Vector2 p = Vector2Add(a, Vector2Scale(ab, t));

    int partner = walls[w].portal_wall;
    if (!SplitWallAt(w, p)) return;
    if (partner != NO_LINK) {
        if (partner > w) partner++;           // shifted by the first insertion
        SplitWallAt(partner, p);
    }
    RebuildNextWalls();
    st->geometry_dirty = true;
    EditorStatus(st, "WALL SPLIT");
}

// Attach the just-closed loop (walls [lw0, wall_counter)) as a hole in the
// sector enclosing it. The loop's walls move from the end of walls[] into
// the target's run so runs stay contiguous. On failure the loop is undone.
static bool FinalizeHole(EditorState *st, int lw0) {
    int len = wall_counter - lw0;
    Vector2 c = { 0 };
    for (int i = 0; i < len; i++)
        c = Vector2Add(c, vertices[walls[lw0 + i].point_start].points);
    c = Vector2Scale(c, 1.0f / (float)len);

    int t = SectorUnderPoint(c);
    bool ok = (t != -1) && len <= 64;
    for (int i = 0; ok && i < len; i++)
        if (SectorUnderPoint(vertices[walls[lw0 + i].point_start].points) != t)
            ok = false;
    if (!ok) {
        wall_counter = lw0;
        PruneOrphanVertices();
        st->geometry_dirty = true;
        return false;
    }

    int ip = sectors[t].wall_start + sectors[t].wall_count;
    if (ip != lw0) {
        Wall tmp[64];
        memcpy(tmp, &walls[lw0], (size_t)len * sizeof(Wall));
        memmove(&walls[ip + len], &walls[ip], (size_t)(lw0 - ip) * sizeof(Wall));
        memcpy(&walls[ip], tmp, (size_t)len * sizeof(Wall));
        for (int r = 0; r < sector_counter; r++)
            if (r != t && sectors[r].wall_start >= ip) sectors[r].wall_start += len;
    }
    sectors[t].wall_count += len;
    st->selected_wall = -1;
    RebuildNextWalls();
    st->geometry_dirty = true;
    return true;
}

// Weld a dragged vertex onto a coincident existing vertex. Returns true if a
// merge happened. Aborts (leaving geometry untouched) if the weld would
// collapse a wall to zero length. This replaces the old release handler, which
// merged onto the wrong index and silently corrupted geometry.
static bool WeldVertex(int d) {
    if (d < 0 || d >= vertex_counter) return false;
    Vector2 dp = vertices[d].points;

    int t = -1;
    for (int i = 0; i < vertex_counter; i++) {
        if (i != d && Vector2Equals(vertices[i].points, dp)) { t = i; break; }
    }
    if (t == -1) return false;

    for (int w = 0; w < wall_counter; w++) {            // refuse if a wall joins d and t
        bool joins = (walls[w].point_start == d && walls[w].point_end == t) ||
                     (walls[w].point_start == t && walls[w].point_end == d);
        if (joins) return false;
    }

    for (int w = 0; w < wall_counter; w++) {            // repoint d's walls onto t
        if (walls[w].point_start == d) walls[w].point_start = t;
        if (walls[w].point_end   == d) walls[w].point_end   = t;
    }

    int last = vertex_counter - 1;                      // swap-remove d, fixing references
    if (d != last) {
        vertices[d] = vertices[last];
        for (int w = 0; w < wall_counter; w++) {
            if (walls[w].point_start == last) walls[w].point_start = d;
            if (walls[w].point_end   == last) walls[w].point_end   = d;
        }
    }
    vertex_counter--;
    return true;
}

// Compact out any vertex no longer referenced by a wall, remapping wall indices
// as it goes. Vertices are only ever created while drawing walls, so the only
// way one becomes an orphan is a deletion - which is exactly when we call this.
static void PruneOrphanVertices(void) {
    static bool used[MAX_VERTICES];
    static int  remap[MAX_VERTICES];

    for (int i = 0; i < vertex_counter; i++) used[i] = false;
    for (int w = 0; w < wall_counter; w++) {
        used[walls[w].point_start] = true;
        used[walls[w].point_end]   = true;
    }

    int n = 0;
    for (int i = 0; i < vertex_counter; i++) {
        if (!used[i]) { remap[i] = -1; continue; }
        remap[i] = n;
        vertices[n++] = vertices[i];
    }
    for (int w = 0; w < wall_counter; w++) {
        walls[w].point_start = remap[walls[w].point_start];
        walls[w].point_end   = remap[walls[w].point_end];
    }
    vertex_counter = n;
}

// Undo: cancel the in-progress loop, otherwise pop the most recent sector.
// Sectors own a contiguous trailing run of walls, so popping is exact - unlike
// the old blind counter decrement, which left sectors pointing past the array.
// Either way we prune the vertices the deleted walls left behind.
static void EditorUndo(EditorState *st) {
    if (st->drawing) {
        wall_counter = st->start_wall;                 // drop the half-drawn loop's walls
        st->drawing = false;
        st->start_vertex = -1;
        st->start_wall = -1;
        PruneOrphanVertices();
        st->geometry_dirty = true;
        return;
    }
    if (sector_counter > 0) {
        Sector last = sectors[--sector_counter];
        wall_counter = last.wall_start;
        if (st->selected_sector >= sector_counter) st->selected_sector = -1;
        if (st->selected_wall   >= wall_counter)   st->selected_wall = -1;
        PruneOrphanVertices();
        st->geometry_dirty = true;
    }
}

// Save text source + bake the binary in one stroke; load re-reads the text
// source (the editable artifact) and recompiles portals.
static void EditorSave(EditorState *st) {
    bool ok = MapSourceSave(FLAXMAP_SOURCE_PATH) && MapBakedSave(FLAXMAP_BAKED_PATH);
    EditorStatus(st, ok ? "SAVED dev.map + dev.fmap" : "SAVE FAILED (see log)");
    if (ok) FLOG_INFO(LOGCAT_EDITOR, "saved map: %d sectors, %d walls, %d verts",
                      sector_counter, wall_counter, vertex_counter);
    else    FLOG_ERROR(LOGCAT_EDITOR, "map save failed");
}

static void EditorLoad(EditorState *st) {
    if (MapSourceLoad(FLAXMAP_SOURCE_PATH)) {
        st->drawing = false;
        st->start_vertex = st->start_wall = -1;
        st->dragged_vertex = st->selected_sector = st->selected_wall = -1;
        st->dragged_entity = st->selected_entity = -1;
        st->rect_dragging = false;
        st->geometry_dirty = true;
        EditorStatus(st, "LOADED dev.map");
        FLOG_INFO(LOGCAT_EDITOR, "loaded map: %d sectors, %d walls", sector_counter, wall_counter);
    } else {
        EditorStatus(st, "LOAD FAILED (see log)");
        FLOG_ERROR(LOGCAT_EDITOR, "map load failed");
    }
}

// ===========================================================================
// Input / update phases
// ===========================================================================

// Click while editing: begin a loop, extend it, or close it back to the start.
static void EditorPlaceVertex(EditorState *st, Vector2 snapped, Vector2 mouseWorld) {
    if (!st->drawing) {
        int v = GetOrCreateVertex(snapped);
        if (v != -1) {
            st->drawing = true;
            st->start_vertex = v;
            st->start_wall = wall_counter;
        }
        return;
    }

    Vector2 startPos = vertices[st->start_vertex].points;
    float   closeR   = 6.0f / st->camera.zoom;
    int     wallsSoFar = wall_counter - st->start_wall;
    int     prevV = wallsSoFar > 0 ? walls[wall_counter - 1].point_end : st->start_vertex;

    // close the loop -> finalise as a sector (EDIT) or a hole (HOLE mode)
    if (CheckCollisionPointCircle(mouseWorld, startPos, closeR)) {
        if (wallsSoFar >= SECTOR_VERTEX_MIN - 1 &&
            wall_counter < MAX_WALLS && sector_counter < MAX_SECTORS) {
            int finalW = wall_counter++;
            walls[finalW] = (Wall){
                .point_start = prevV, .point_end = st->start_vertex,
                .next_wall = NO_LINK, .next_sector = NO_LINK,
                .portal_wall = NO_LINK, .texture_id = INVALID_ID,
            };

            int lw0 = st->start_wall;
            st->drawing = false;
            st->start_vertex = -1;
            st->start_wall = -1;

            if (st->mode == MODE_HOLE) {
                EditorStatus(st, FinalizeHole(st, lw0)
                                 ? "HOLE ADDED" : "HOLE MUST SIT INSIDE ONE SECTOR");
                return;
            }

            for (int i = lw0; i < finalW; i++) walls[i].next_wall = i + 1;
            walls[finalW].next_wall = lw0;

            sectors[sector_counter++] = (Sector){
                .wall_start = lw0,
                .wall_count = wall_counter - lw0,
                .floor_z = 0.0f, .ceilingz = 10.0f,
                .floor_texture = INVALID_ID, .ceiling_texture = INVALID_ID,
                .light = 1.0f,
            };
            st->geometry_dirty = true;
        }
        return;
    }

    // extend the loop with one more wall
    if (wall_counter < MAX_WALLS) {
        int v = GetOrCreateVertex(snapped);
        if (v != -1 && v != prevV) {
            walls[wall_counter++] = (Wall){
                .point_start = prevV, .point_end = v,
                .next_wall = NO_LINK, .next_sector = NO_LINK,
                .portal_wall = NO_LINK, .texture_id = INVALID_ID,
            };
            st->geometry_dirty = true;
        }
    }
}

// Pan, zoom and global hotkeys. `overGui` suppresses world interaction when the
// cursor is over a panel so clicks and the wheel don't bleed through.
static void EditorHandleInput(EditorState *st, Vector2 mouseWorld, Vector2 snapped, bool overGui) {
    if (IsKeyPressed(KEY_ONE))   st->mode = MODE_EDIT;
    if (IsKeyPressed(KEY_TWO))   st->mode = MODE_DRAG;
    if (IsKeyPressed(KEY_THREE)) st->mode = MODE_SECTOR;
    if (IsKeyPressed(KEY_FOUR))  st->mode = MODE_WALL;
    if (IsKeyPressed(KEY_FIVE))  st->mode = MODE_HOLE;
    if (IsKeyPressed(KEY_SIX))   st->mode = MODE_ENTITY;
    if (IsKeyPressed(KEY_B))     EditorUndo(st);

    // save / load (Ctrl or Cmd)
    bool chord = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) ||
                 IsKeyDown(KEY_LEFT_SUPER)   || IsKeyDown(KEY_RIGHT_SUPER);
    if (chord && IsKeyPressed(KEY_S)) EditorSave(st);
    if (chord && IsKeyPressed(KEY_O)) EditorLoad(st);

    // hot-reload textures dropped into assets/textures (names stay valid)
    if (IsKeyPressed(KEY_F5)) {
        TexturesReload();
        EditorStatus(st, TextFormat("TEXTURES RELOADED (%d)", TexRegistryCount()));
    }

    // while laying out a loop, BACKSPACE retracts the last placed wall
    if (st->drawing && IsKeyPressed(KEY_BACKSPACE)) {
        if (wall_counter > st->start_wall) wall_counter--;
        else { st->drawing = false; st->start_vertex = st->start_wall = -1; }
        st->geometry_dirty = true;
    } else if (st->mode == MODE_ENTITY && st->selected_entity != -1 &&
               (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressed(KEY_DELETE))) {
        DeleteEntity(st, st->selected_entity);
    }

    // place / rotate the player start
    if (IsKeyPressed(KEY_P) && !overGui) playerStart = mouseWorld;
    if (IsKeyDown(KEY_Q)) playerStartYaw -= 2.0f * GetFrameTime();
    if (IsKeyDown(KEY_E)) playerStartYaw += 2.0f * GetFrameTime();

    // pan with right mouse
    if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
        Vector2 delta = Vector2Scale(GetMouseDelta(), -1.0f / st->camera.zoom);
        st->camera.target = Vector2Add(st->camera.target, delta);
    }

    // zoom toward the cursor
    float wheel = GetMouseWheelMove();
    if (wheel != 0 && !overGui) {
        st->camera.offset = GetMousePosition();
        st->camera.target = mouseWorld;
        st->camera.zoom  *= expf(wheel * 0.1f);
        st->camera.zoom   = Clamp(st->camera.zoom, 0.2f, 5.0f);
    }

    // always finish an in-progress drag, even if the cursor wandered over a panel
    if (st->dragged_vertex != -1 && IsMouseButtonReleased(MOUSE_LEFT_BUTTON)) {
        WeldVertex(st->dragged_vertex);
        st->dragged_vertex = -1;
        st->geometry_dirty = true;         // moved/welded geometry -> relink portals
    }
    if (st->dragged_entity != -1 && IsMouseButtonReleased(MOUSE_LEFT_BUTTON))
        st->dragged_entity = -1;

    // finish a rectangle / stair drag wherever the cursor ends up
    if (st->rect_dragging && IsMouseButtonReleased(MOUSE_LEFT_BUTTON)) {
        if (st->rect_stairs) StampStairs(st, st->rect_anchor, snapped);
        else                 CreateRectSector(st, st->rect_anchor, snapped);
        st->rect_dragging = false;
        st->rect_stairs = false;
    }

    if (overGui) return;   // the rest is world interaction; let the panel have the cursor

    switch (st->mode) {
    case MODE_EDIT:
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            // SHIFT+drag stamps a rectangular room, CTRL+SHIFT+drag a
            // staircase; plain click draws a loop
            if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) {
                if (!st->drawing) {
                    st->rect_dragging = true;
                    st->rect_stairs = chord;
                    st->rect_anchor = snapped;
                }
            } else {
                EditorPlaceVertex(st, snapped, mouseWorld);
            }
        }
        break;

    case MODE_DRAG:
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            float pickR = 6.0f / st->camera.zoom;
            float bestSq = pickR * pickR;
            for (int i = 0; i < vertex_counter; i++) {
                float dSq = Vector2DistanceSqr(mouseWorld, vertices[i].points);
                if (dSq < bestSq) { bestSq = dSq; st->dragged_vertex = i; }
            }
        }
        if (st->dragged_vertex != -1 && IsMouseButtonDown(MOUSE_LEFT_BUTTON))
            vertices[st->dragged_vertex].points = SnapVertexToGrid(mouseWorld, GRID_SPACING);
        break;

    case MODE_SECTOR:
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
            st->selected_sector = SectorUnderPoint(mouseWorld);
        if (st->selected_sector != -1) {
            Sector *sec = &sectors[st->selected_sector];
            float step = 0.5f;
            if (IsKeyPressed(KEY_R)) sec->ceilingz += step;
            if (IsKeyPressed(KEY_T)) sec->ceilingz -= step;
            if (IsKeyPressed(KEY_F)) sec->floor_z  += step;
            if (IsKeyPressed(KEY_G)) sec->floor_z  -= step;
            if (sec->floor_z > sec->ceilingz - step) sec->floor_z = sec->ceilingz - step;
        }
        break;

    case MODE_WALL:
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
            st->selected_wall = WallUnderPoint(mouseWorld, 8.0f / st->camera.zoom);
        if (st->selected_wall != -1) {
            Wall *wl = &walls[st->selected_wall];
            if (IsKeyPressed(KEY_RIGHT_BRACKET)) wl->texture_id = CycleWorldTexture(wl->texture_id, +1);
            if (IsKeyPressed(KEY_LEFT_BRACKET))  wl->texture_id = CycleWorldTexture(wl->texture_id, -1);
            if (IsKeyPressed(KEY_X)) SplitSelectedWall(st, mouseWorld);
        }
        break;

    case MODE_HOLE:
        // same loop drawing as EDIT; closure attaches it as a hole
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
            EditorPlaceVertex(st, snapped, mouseWorld);
        break;

    case MODE_ENTITY:
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            int hit = EntityUnderPoint(mouseWorld, 10.0f / st->camera.zoom);
            if (hit != -1) {
                st->selected_entity = hit;
                st->dragged_entity = hit;
            } else if (entity_counter < MAX_ENTITIES) {
                entities[entity_counter] = EntityDefault(st->entity_brush_type, snapped);
                st->selected_entity = entity_counter;
                st->dragged_entity = entity_counter;
                entity_counter++;
                EditorStatus(st, "ENTITY ADDED");
            }
        }
        if (st->dragged_entity != -1 && IsMouseButtonDown(MOUSE_LEFT_BUTTON))
            entities[st->dragged_entity].pos = snapped;
        break;
    }
}

// ===========================================================================
// Render phases
// ===========================================================================

static void EditorRenderGrid(EditorState *st) {
    Vector2 tl = GetScreenToWorld2D((Vector2){0, 0}, st->camera);
    Vector2 br = GetScreenToWorld2D((Vector2){GetScreenWidth(), GetScreenHeight()}, st->camera);

    float startX = floorf(tl.x / GRID_SPACING) * GRID_SPACING;
    float endX   = ceilf (br.x / GRID_SPACING) * GRID_SPACING;
    float startY = floorf(tl.y / GRID_SPACING) * GRID_SPACING;
    float endY   = ceilf (br.y / GRID_SPACING) * GRID_SPACING;

    for (float x = startX; x <= endX; x += GRID_SPACING) {
        int cell = (int)roundf(x / GRID_SPACING);
        Color c = (x == 0.0f) ? COL_AXIS : (cell % GRID_MAJOR_EVERY == 0) ? COL_GRID_MAJ : COL_GRID_MIN;
        DrawLineV((Vector2){x, startY}, (Vector2){x, endY}, c);
    }
    for (float y = startY; y <= endY; y += GRID_SPACING) {
        int cell = (int)roundf(y / GRID_SPACING);
        Color c = (y == 0.0f) ? COL_AXIS : (cell % GRID_MAJOR_EVERY == 0) ? COL_GRID_MAJ : COL_GRID_MIN;
        DrawLineV((Vector2){startX, y}, (Vector2){endX, y}, c);
    }
}

static Color EntityColor(int type) {
    switch (type) {
    case ENT_DECAL:  return (Color){ 170, 140, 255, 255 };
    case ENT_SPAWN:  return (Color){ 255, 95, 70, 255 };
    case ENT_BRIDGE: return (Color){ 90, 190, 230, 255 };
    case ENT_HEALTH: return (Color){ 230, 45, 45, 255 };
    case ENT_AMMO:   return (Color){ 235, 170, 55, 255 };
    default:         return COL_DIM;
    }
}

static void EditorRenderEntities(EditorState *st) {
    float markerR = 8.0f / st->camera.zoom;
    float lineW = 2.0f / st->camera.zoom;
    int font = (int)(15.0f / st->camera.zoom) + 2;

    for (int i = 0; i < entity_counter; i++) {
        const Entity *e = &entities[i];
        Color c = EntityColor(e->type);
        bool selected = st->selected_entity == i;

        if (e->type == ENT_BRIDGE && e->sx > 0.0f && e->sy > 0.0f) {
            float hx = e->sx / WORLD_SCALE, hy = e->sy / WORLD_SCALE;
            DrawRectangleLinesEx((Rectangle){ e->pos.x - hx, e->pos.y - hy, hx * 2.0f, hy * 2.0f },
                                 lineW, selected ? COL_VERTEX_HI : c);
        }

        DrawCircleV(e->pos, markerR, Fade(c, 0.8f));
        DrawCircleLines((int)e->pos.x, (int)e->pos.y, markerR * (selected ? 1.8f : 1.25f),
                        selected ? COL_VERTEX_HI : COL_DARK);

        if (e->type == ENT_SPAWN) {
            Vector2 tip = Vector2Add(e->pos, Vector2Scale((Vector2){ cosf(e->yaw), sinf(e->yaw) },
                                                          markerR * 2.6f));
            DrawLineEx(e->pos, tip, lineW, COL_VERTEX_HI);
        } else if (e->type == ENT_HEALTH) {
            DrawLineEx((Vector2){ e->pos.x - markerR, e->pos.y },
                       (Vector2){ e->pos.x + markerR, e->pos.y }, lineW, COL_VERTEX_HI);
            DrawLineEx((Vector2){ e->pos.x, e->pos.y - markerR },
                       (Vector2){ e->pos.x, e->pos.y + markerR }, lineW, COL_VERTEX_HI);
        }

        if (st->camera.zoom > 0.35f)
            DrawText(EntityTypeName(e->type), (int)(e->pos.x + markerR + 4.0f),
                     (int)(e->pos.y - markerR), font, selected ? COL_VERTEX_HI : c);
    }
}

static void EditorRenderWorld(EditorState *st, Vector2 mouseWorld, Vector2 snapped) {
    float vertexR = 4.0f / st->camera.zoom;
    float pickR   = 6.0f / st->camera.zoom;

    // selected sector: triangulated translucent fill (concave/holed safe)
    // + outline, hinge wall (the slope reference) called out in orange
    if (st->selected_sector != -1) {
        int hs = sectors[st->selected_sector].wall_start;
        int hn = sectors[st->selected_sector].wall_count;
        if (hn >= 3 && hn <= 512) {
            static Vector2 pts[512];
            static Vector2 tri[256 * 3];
            for (int i = 0; i < hn; i++) pts[i] = vertices[walls[hs + i].point_start].points;
            int lc[16];
            int nl = WorldSectorLoops(st->selected_sector, lc, 16);
            int ntris = TriangulatePolygon(pts, lc, nl, tri, 256);
            rlDisableBackfaceCulling();
            for (int t = 0; t < ntris; t++)
                DrawTriangle(tri[t * 3], tri[t * 3 + 1], tri[t * 3 + 2], COL_SEL);
            rlEnableBackfaceCulling();
            for (int i = 0; i < hn; i++)
                DrawLineEx(pts[i], vertices[walls[hs + i].point_end].points,
                           3.0f / st->camera.zoom, COL_SEL_LINE);
            DrawLineEx(pts[0], vertices[walls[hs].point_end].points,
                       5.0f / st->camera.zoom, (Color){ 255, 170, 60, 255 });
        }
    }

    // walls: white one-sided, red two-sided (portal)
    for (int i = 0; i < wall_counter; i++) {
        Vector2 a = vertices[walls[i].point_start].points;
        Vector2 b = vertices[walls[i].point_end].points;
        DrawLineV(a, b, walls[i].next_sector != NO_LINK ? COL_PORTAL : COL_WALL);
    }

    // selected wall (WALL mode): thick highlight over the base line
    if (st->mode == MODE_WALL && st->selected_wall != -1 && st->selected_wall < wall_counter) {
        Vector2 a = vertices[walls[st->selected_wall].point_start].points;
        Vector2 b = vertices[walls[st->selected_wall].point_end].points;
        DrawLineEx(a, b, 4.0f / st->camera.zoom, COL_SEL_LINE);
    }

    // rectangle / stair tool preview while shift-dragging
    if (st->rect_dragging) {
        float x0 = fminf(st->rect_anchor.x, snapped.x), x1 = fmaxf(st->rect_anchor.x, snapped.x);
        float y0 = fminf(st->rect_anchor.y, snapped.y), y1 = fmaxf(st->rect_anchor.y, snapped.y);
        DrawRectangleRec((Rectangle){ x0, y0, x1 - x0, y1 - y0 }, COL_SEL);
        DrawRectangleLinesEx((Rectangle){ x0, y0, x1 - x0, y1 - y0 },
                             2.0f / st->camera.zoom, COL_SEL_LINE);

        if (st->rect_stairs) {
            // step divisions along the dominant drag axis + a step count
            float dx = snapped.x - st->rect_anchor.x, dy = snapped.y - st->rect_anchor.y;
            bool alongX = fabsf(dx) >= fabsf(dy);
            int steps = (int)roundf((alongX ? fabsf(dx) : fabsf(dy)) / GRID_SPACING);
            for (int i = 1; i < steps; i++) {
                float c = (alongX ? x0 : y0) + (float)i * GRID_SPACING;
                if (alongX) DrawLineEx((Vector2){ c, y0 }, (Vector2){ c, y1 },
                                       1.5f / st->camera.zoom, COL_SEL_LINE);
                else        DrawLineEx((Vector2){ x0, c }, (Vector2){ x1, c },
                                       1.5f / st->camera.zoom, COL_SEL_LINE);
            }
            DrawText(TextFormat("%d STEPS  +%.1f", steps, steps * STAIR_RISE),
                     (int)x0, (int)y0 - (int)(20.0f / st->camera.zoom),
                     (int)(16.0f / st->camera.zoom) + 2, COL_HOT);
        }
    }

    // vertices as squares, brighter on hover / drag
    for (int i = 0; i < vertex_counter; i++) {
        Vector2 v = vertices[i].points;
        bool hot = CheckCollisionPointCircle(mouseWorld, v, pickR) || st->dragged_vertex == i;
        float s = hot ? vertexR + 2.0f : vertexR;
        DrawRectangleV((Vector2){v.x - s, v.y - s}, (Vector2){s * 2, s * 2},
                       hot ? COL_VERTEX_HI : COL_VERTEX);
    }

    // rubber band for the loop in progress
    if (st->drawing) {
        int lastV = (wall_counter > st->start_wall) ? walls[wall_counter - 1].point_end
                                                     : st->start_vertex;
        Vector2 lastPos  = vertices[lastV].points;
        Vector2 startPos = vertices[st->start_vertex].points;
        if (CheckCollisionPointCircle(mouseWorld, startPos, 6.0f / st->camera.zoom)) {
            DrawCircleV(startPos, vertexR + 4.0f, YELLOW);
            DrawLineEx(lastPos, startPos, 1.0f, YELLOW);
        } else {
            DrawLineEx(lastPos, snapped, 1.0f, GREEN);
        }
    }

    // snap crosshair at the cursor's grid point
    float ch = 9.0f / st->camera.zoom;
    DrawLineV((Vector2){snapped.x - ch, snapped.y}, (Vector2){snapped.x + ch, snapped.y}, COL_VERTEX);
    DrawLineV((Vector2){snapped.x, snapped.y - ch}, (Vector2){snapped.x, snapped.y + ch}, COL_VERTEX);

    // player start marker + facing
    float markerR = 8.0f / st->camera.zoom;
    Vector2 facing = { cosf(playerStartYaw), sinf(playerStartYaw) };
    Vector2 tip = Vector2Add(playerStart, Vector2Scale(facing, markerR * 2.5f));
    DrawCircleV(playerStart, markerR, GREEN);
    DrawLineEx(playerStart, tip, 2.0f / st->camera.zoom, LIME);

    EditorRenderEntities(st);
}

// One-time retro phosphor styling for the raygui widgets (toggles, sliders,
// buttons). Panels and labels are drawn by hand for the beveled look.
static void EditorSetupStyle(void) {
    GuiLoadStyleDefault();
    GuiSetStyle(DEFAULT, TEXT_SIZE,             16);
    GuiSetStyle(DEFAULT, TEXT_SPACING,          1);
    GuiSetStyle(DEFAULT, BORDER_WIDTH,          2);     // chunky, sharp edges
    GuiSetStyle(DEFAULT, BACKGROUND_COLOR,      0x12161eff);
    GuiSetStyle(DEFAULT, LINE_COLOR,            0x3a4a5aff);
    GuiSetStyle(DEFAULT, BORDER_COLOR_NORMAL,   0x3a4a5aff);
    GuiSetStyle(DEFAULT, BASE_COLOR_NORMAL,     0x10141cff);
    GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL,     0x6e9684ff);
    GuiSetStyle(DEFAULT, BORDER_COLOR_FOCUSED,  0x78beebff);
    GuiSetStyle(DEFAULT, BASE_COLOR_FOCUSED,    0x1a2636ff);
    GuiSetStyle(DEFAULT, TEXT_COLOR_FOCUSED,    0xeafff2ff);
    GuiSetStyle(DEFAULT, BORDER_COLOR_PRESSED,  0xbeffd2ff);
    GuiSetStyle(DEFAULT, BASE_COLOR_PRESSED,    0x2c5a48ff); // "lit" active toggle
    GuiSetStyle(DEFAULT, TEXT_COLOR_PRESSED,    0xeafff2ff);
    GuiSetStyle(SLIDER,  BORDER_WIDTH,          2);
}

// Raised bevel: light top/left, dark bottom/right (a Win95/Borland 3D panel).
static void BevelRaised(Rectangle r, Color face) {
    DrawRectangleRec(r, face);
    DrawRectangleRec((Rectangle){ r.x, r.y, r.width, 2 }, COL_LIGHT);
    DrawRectangleRec((Rectangle){ r.x, r.y, 2, r.height }, COL_LIGHT);
    DrawRectangleRec((Rectangle){ r.x, r.y + r.height - 2, r.width, 2 }, COL_DARK);
    DrawRectangleRec((Rectangle){ r.x + r.width - 2, r.y, 2, r.height }, COL_DARK);
}

// Sunken bevel: the inverse, for recessed insets (mode readout, etc.).
static void BevelSunken(Rectangle r, Color face) {
    DrawRectangleRec(r, face);
    DrawRectangleRec((Rectangle){ r.x, r.y, r.width, 2 }, COL_DARK);
    DrawRectangleRec((Rectangle){ r.x, r.y, 2, r.height }, COL_DARK);
    DrawRectangleRec((Rectangle){ r.x, r.y + r.height - 2, r.width, 2 }, COL_LIGHT);
    DrawRectangleRec((Rectangle){ r.x + r.width - 2, r.y, 2, r.height }, COL_LIGHT);
}

// A filled section header bar with bright label - the DOS-editor group divider.
static void SectionBar(float x, float y, float w, const char *label) {
    DrawRectangleRec((Rectangle){ x, y, w, 18 }, COL_HEADER);
    DrawRectangleRec((Rectangle){ x, y, w, 1 }, COL_LIGHT);
    DrawRectangleRec((Rectangle){ x, y + 17, w, 1 }, COL_DARK);
    DrawText(label, (int)x + 6, (int)y + 3, 16, COL_HOT);
}

// One texture-picker row: "<label>  [<] NAME [>]" + a small swatch. Returns
// the (possibly cycled) world texture id.
static int TexPickerRow(float x, float y, float w, const char *label, int world_id) {
    DrawText(label, (int)x + 2, (int)y + 4, 16, COL_DIM);
    if (GuiButton((Rectangle){ x + 64, y, 22, 22 }, "<"))
        world_id = CycleWorldTexture(world_id, -1);
    if (GuiButton((Rectangle){ x + w - 22, y, 22, 22 }, ">"))
        world_id = CycleWorldTexture(world_id, +1);

    int reg = TexRegistryResolve(world_id);
    Rectangle swatch = { x + 90, y - 1, 24, 24 };
    Texture2D t = TexRegistryTexture(reg);
    DrawTexturePro(t, (Rectangle){ 0, 0, (float)t.width, (float)t.height },
                   swatch, (Vector2){ 0, 0 }, 0.0f, WHITE);
    DrawRectangleLinesEx(swatch, 1, COL_DARK);
    DrawText(WorldTexname(world_id), (int)x + 120, (int)y + 4, 16, COL_HOT);
    return world_id;
}

// Right-hand dock: mode switch, live stats, selected-sector tools, help.
static void EditorRenderGui(EditorState *st, Vector2 snapped) {
    int W = GetScreenWidth(), H = GetScreenHeight();
    int gx = (int)roundf(snapped.x / GRID_SPACING);
    int gy = (int)roundf(snapped.y / GRID_SPACING);

    const char *modeName = st->mode == MODE_EDIT ? "EDIT"
                         : st->mode == MODE_DRAG ? "DRAG"
                         : st->mode == MODE_SECTOR ? "SECTOR"
                         : st->mode == MODE_WALL ? "WALL"
                         : st->mode == MODE_HOLE ? "HOLE" : "ENTITY";

    // ---- header -----------------------------------------------------------
    BevelRaised((Rectangle){ 0, 0, W, HEADER_H }, COL_HEADER);
    DrawText("FLAX", 10, 5, 20, COL_HOT);
    DrawText("EDITOR", 62, 5, 20, COL_TEXT);
    Rectangle modeBox = { 150, 4, 96, HEADER_H - 8 };
    BevelSunken(modeBox, COL_DARK);
    DrawText(modeName, (int)modeBox.x + 10, 6, 18, COL_VERTEX);

    // ---- right dock -------------------------------------------------------
    float px = W - PANEL_W;
    BevelRaised((Rectangle){ px, HEADER_H, PANEL_W, H - HEADER_H - STATUS_H }, COL_FACE);
    float x = px + PAD, w = PANEL_W - PAD * 2, y = HEADER_H + PAD;

    // mode switch
    SectionBar(x, y, w, "MODE   1 2 3 4 5 6");  y += 24;
    int modeIdx = (int)st->mode;
    GuiToggleGroup((Rectangle){ x, y, (w - 20) / 6.0f, 24 }, "EDIT;DRAG;SECT;WALL;HOLE;ENT", &modeIdx);
    st->mode = (EditorMode)modeIdx;       y += 24 + PAD;

    // world stats
    SectionBar(x, y, w, "WORLD");  y += 22;
    DrawText(TextFormat("SECTORS  %4d / %d", sector_counter, MAX_SECTORS), (int)x + 2, (int)y, 16, COL_TEXT); y += 18;
    DrawText(TextFormat("WALLS    %4d / %d", wall_counter, MAX_WALLS),     (int)x + 2, (int)y, 16, COL_TEXT); y += 18;
    DrawText(TextFormat("VERTS    %4d", vertex_counter),                   (int)x + 2, (int)y, 16, COL_TEXT); y += 18;
    DrawText(TextFormat("ENTS     %4d / %d", entity_counter, MAX_ENTITIES), (int)x + 2, (int)y, 16, COL_TEXT); y += 18 + PAD;

    // selected sector tools
    SectionBar(x, y, w, "SELECTED SECTOR");  y += 22;
    if (st->selected_sector != -1) {
        Sector *sec = &sectors[st->selected_sector];
        DrawText(TextFormat("#%d   WALLS %d   PORTALS %d",
                 st->selected_sector, sec->wall_count, SectorPortalCount(st->selected_sector)),
                 (int)x + 2, (int)y, 16, COL_TEXT); y += 22;

        DrawText("FLOOR", (int)x + 2, (int)y + 2, 16, COL_DIM);
        GuiSliderBar((Rectangle){ x + 54, y, w - 108, 20 }, NULL, NULL, &sec->floor_z, -10.0f, 40.0f);
        DrawText(TextFormat("%5.1f", sec->floor_z), (int)(x + w - 46), (int)y + 2, 16, COL_HOT); y += 24;

        DrawText("CEIL", (int)x + 2, (int)y + 2, 16, COL_DIM);
        GuiSliderBar((Rectangle){ x + 54, y, w - 108, 20 }, NULL, NULL, &sec->ceilingz, -10.0f, 40.0f);
        DrawText(TextFormat("%5.1f", sec->ceilingz), (int)(x + w - 46), (int)y + 2, 16, COL_HOT); y += 24;

        if (sec->floor_z > sec->ceilingz - 0.5f) sec->floor_z = sec->ceilingz - 0.5f;

        // slopes, shown as world-unit rise per grid cell about the orange
        // hinge wall (player step-up is 1.8/cell - keep ramps under that)
        float frise = sec->floor_slope * GRID_SPACING;
        DrawText("F SLP", (int)x + 2, (int)y + 2, 16, COL_DIM);
        GuiSliderBar((Rectangle){ x + 54, y, w - 108, 20 }, NULL, NULL, &frise, -2.0f, 2.0f);
        sec->floor_slope = frise / GRID_SPACING;
        DrawText(TextFormat("%+4.2f", frise), (int)(x + w - 46), (int)y + 2, 16, COL_HOT); y += 24;

        float crise = sec->ceil_slope * GRID_SPACING;
        DrawText("C SLP", (int)x + 2, (int)y + 2, 16, COL_DIM);
        GuiSliderBar((Rectangle){ x + 54, y, w - 108, 20 }, NULL, NULL, &crise, -2.0f, 2.0f);
        sec->ceil_slope = crise / GRID_SPACING;
        DrawText(TextFormat("%+4.2f", crise), (int)(x + w - 46), (int)y + 2, 16, COL_HOT); y += 24;

        DrawText("LIGHT", (int)x + 2, (int)y + 2, 16, COL_DIM);
        GuiSliderBar((Rectangle){ x + 54, y, w - 108, 20 }, NULL, NULL, &sec->light, 0.0f, 1.5f);
        DrawText(TextFormat("%4.2f", sec->light), (int)(x + w - 46), (int)y + 2, 16, COL_HOT); y += 24;

        if (GuiButton((Rectangle){ x, y, (w - 6) / 2, 22 }, "HINGE >"))
            RotateSectorHinge(st, st->selected_sector);
        if (GuiButton((Rectangle){ x + (w + 6) / 2, y, (w - 6) / 2, 22 }, "FLATTEN")) {
            sec->floor_slope = 0.0f;
            sec->ceil_slope = 0.0f;
        }
        y += 26;

        sec->floor_texture   = TexPickerRow(x, y, w, "FLR TEX",  sec->floor_texture);   y += 26;
        sec->ceiling_texture = TexPickerRow(x, y, w, "CEIL TEX", sec->ceiling_texture); y += 26;

        if (GuiButton((Rectangle){ x, y, w, 22 }, "DELETE SECTOR   B")) {
            if (st->selected_sector == sector_counter - 1) EditorUndo(st);
            else st->selected_sector = -1;
        }
        y += 22 + PAD;
    } else {
        DrawText("click a sector in", (int)x + 2, (int)y, 16, COL_DIM);      y += 18;
        DrawText("SECTOR mode to edit", (int)x + 2, (int)y, 16, COL_DIM);    y += 18 + PAD;
    }

    // selected wall tools
    SectionBar(x, y, w, "SELECTED WALL");  y += 22;
    if (st->selected_wall != -1 && st->selected_wall < wall_counter) {
        Wall *wl = &walls[st->selected_wall];
        DrawText(TextFormat("#%d   %s", st->selected_wall,
                 wl->next_sector != NO_LINK ? "PORTAL" : "SOLID"),
                 (int)x + 2, (int)y, 16, COL_TEXT); y += 22;
        wl->texture_id = TexPickerRow(x, y, w, "TEX [ ]", wl->texture_id); y += 26 + PAD;
    } else {
        DrawText("click a wall in", (int)x + 2, (int)y, 16, COL_DIM);     y += 18;
        DrawText("WALL mode to paint", (int)x + 2, (int)y, 16, COL_DIM);  y += 18 + PAD;
    }

    // entity tools
    if (st->selected_entity >= entity_counter) st->selected_entity = -1;
    SectionBar(x, y, w, "ENTITY");  y += 22;
    if (st->selected_entity != -1) {
        Entity *e = &entities[st->selected_entity];
        DrawText(TextFormat("#%d   %s", st->selected_entity, EntityTypeName(e->type)),
                 (int)x + 2, (int)y, 16, COL_TEXT); y += 22;

        int type = e->type;
        GuiToggleGroup((Rectangle){ x, y, (w - 16) / 5.0f, 22 }, "DEC;SPN;BRG;AMMO;HP", &type);
        if (type != e->type) *e = EntityDefault(type, e->pos);
        y += 24;

        DrawText(TextFormat("POS   %.0f, %.0f", e->pos.x, e->pos.y), (int)x + 2, (int)y, 16, COL_TEXT); y += 20;

        if (e->type == ENT_AMMO) {
            int ammo = e->data == AMMO_BULLETS ? AMMO_BULLETS : AMMO_SHELLS;
            GuiToggleGroup((Rectangle){ x, y, (w - 4) / 2.0f, 22 }, "SHELL;BULLET", &ammo);
            e->data = ammo;
            y += 24;

            float max = ammo == AMMO_SHELLS ? 32.0f : 120.0f;
            if (e->sx <= 0.0f) e->sx = ammo == AMMO_SHELLS ? 8.0f : 40.0f;
            e->sx = Clamp(e->sx, 1.0f, max);
            DrawText("AMOUNT", (int)x + 2, (int)y + 2, 16, COL_DIM);
            GuiSliderBar((Rectangle){ x + 72, y, w - 124, 20 }, NULL, NULL, &e->sx, 1.0f, max);
            e->sx = roundf(e->sx);
            DrawText(TextFormat("%3.0f", e->sx), (int)(x + w - 42), (int)y + 2, 16, COL_HOT); y += 24;
        } else if (e->type == ENT_HEALTH) {
            float amount = (float)(e->data > 0 ? e->data : 25);
            DrawText("AMOUNT", (int)x + 2, (int)y + 2, 16, COL_DIM);
            GuiSliderBar((Rectangle){ x + 72, y, w - 124, 20 }, NULL, NULL, &amount, 1.0f, 100.0f);
            e->data = (int)roundf(amount);
            DrawText(TextFormat("%3d", e->data), (int)(x + w - 42), (int)y + 2, 16, COL_HOT); y += 24;
        } else {
            DrawText("Z", (int)x + 2, (int)y + 2, 16, COL_DIM);
            GuiSliderBar((Rectangle){ x + 54, y, w - 108, 20 }, NULL, NULL, &e->z, -10.0f, 40.0f);
            DrawText(TextFormat("%5.1f", e->z), (int)(x + w - 46), (int)y + 2, 16, COL_HOT); y += 24;

            float yawDeg = e->yaw * RAD2DEG;
            DrawText("YAW", (int)x + 2, (int)y + 2, 16, COL_DIM);
            GuiSliderBar((Rectangle){ x + 54, y, w - 108, 20 }, NULL, NULL, &yawDeg, -180.0f, 180.0f);
            e->yaw = yawDeg * DEG2RAD;
            DrawText(TextFormat("%4.0f", yawDeg), (int)(x + w - 46), (int)y + 2, 16, COL_HOT); y += 24;

            DrawText("SX", (int)x + 2, (int)y + 2, 16, COL_DIM);
            GuiSliderBar((Rectangle){ x + 54, y, w - 108, 20 }, NULL, NULL, &e->sx, 0.1f, 8.0f);
            DrawText(TextFormat("%4.1f", e->sx), (int)(x + w - 46), (int)y + 2, 16, COL_HOT); y += 24;

            DrawText("SY", (int)x + 2, (int)y + 2, 16, COL_DIM);
            GuiSliderBar((Rectangle){ x + 54, y, w - 108, 20 }, NULL, NULL, &e->sy, 0.1f, 8.0f);
            DrawText(TextFormat("%4.1f", e->sy), (int)(x + w - 46), (int)y + 2, 16, COL_HOT); y += 24;

            if (e->type == ENT_DECAL || e->type == ENT_BRIDGE) {
                e->texture_id = TexPickerRow(x, y, w, "TEX", e->texture_id);
                y += 26;
            }
        }

        if (GuiButton((Rectangle){ x, y, w, 22 }, "DELETE ENTITY   DEL"))
            DeleteEntity(st, st->selected_entity);
        y += 22 + PAD;
    } else {
        int brush = st->entity_brush_type;
        GuiToggleGroup((Rectangle){ x, y, (w - 16) / 5.0f, 22 }, "DEC;SPN;BRG;AMMO;HP", &brush);
        st->entity_brush_type = brush;
        y += 24;
        DrawText("click in ENTITY mode", (int)x + 2, (int)y, 16, COL_DIM); y += 18 + PAD;
    }

    // player start
    SectionBar(x, y, w, "PLAYER START   P Q E");  y += 22;
    DrawText(TextFormat("POS   %.0f, %.0f", playerStart.x, playerStart.y), (int)x + 2, (int)y, 16, COL_TEXT); y += 18;
    DrawText(TextFormat("YAW   %.0f deg", playerStartYaw * RAD2DEG),       (int)x + 2, (int)y, 16, COL_TEXT); y += 18 + PAD;

    // view + reset
    SectionBar(x, y, w, "VIEW");  y += 22;
    DrawText(TextFormat("ZOOM   %.2fx", st->camera.zoom), (int)x + 2, (int)y + 4, 16, COL_TEXT);
    if (GuiButton((Rectangle){ x + w - 96, y, 96, 22 }, "RESET VIEW")) ResetMapEditorCamera();
    y += 24 + PAD;

    // help
    SectionBar(x, y, w, "KEYS");  y += 22;
    DrawText("LMB draw SHIFT rect ^SHIFT str", (int)x + 2, (int)y, 16, COL_DIM); y += 18;
    DrawText("5 HOLE   6 ENTITY   DEL ent",    (int)x + 2, (int)y, 16, COL_DIM); y += 18;
    DrawText("X split wall   LMB place/select", (int)x + 2, (int)y, 16, COL_DIM); y += 18;
    DrawText("R/T ceil  F/G floor  B undo",    (int)x + 2, (int)y, 16, COL_DIM); y += 18;
    DrawText("[ ] wall tex   F5 reload",       (int)x + 2, (int)y, 16, COL_DIM); y += 18;
    DrawText("^S save  ^O load  BKSP wall",    (int)x + 2, (int)y, 16, COL_DIM); y += 18;
    DrawText("RMB pan   WHEEL zoom",           (int)x + 2, (int)y, 16, COL_DIM);

    // ---- status bar -------------------------------------------------------
    BevelRaised((Rectangle){ 0, H - STATUS_H, W, STATUS_H }, COL_HEADER);
    DrawText(TextFormat("CURSOR %d, %d", gx, gy), 10, H - STATUS_H + 5, 16, COL_HOT);
    DrawText(TextFormat("%s", modeName), 200, H - STATUS_H + 5, 16, COL_VERTEX);
    DrawText(TextFormat("S:%d  W:%d  V:%d  E:%d", sector_counter, wall_counter, vertex_counter, entity_counter),
             320, H - STATUS_H + 5, 16, COL_TEXT);
    if (st->status_timer > 0.0f)
        DrawText(st->status_msg, 520, H - STATUS_H + 5, 16, COL_HOT);
}

// ===========================================================================
// Public entry points
// ===========================================================================

void ResetMapEditorCamera(void) {
    g_ed.camera.target   = (Vector2){ 0.0f, 0.0f };
    g_ed.camera.offset   = (Vector2){ GetScreenWidth() / 2.0f, GetScreenHeight() / 2.0f };
    g_ed.camera.rotation = 0.0f;
    g_ed.camera.zoom     = 1.0f;
}

void DrawMapEditor(void) {
    EditorState *st = &g_ed;
    if (!g_style_ready) { EditorSetupStyle(); g_style_ready = true; }

    Vector2 mouseScreen = GetMousePosition();
    Vector2 mouseWorld  = GetScreenToWorld2D(mouseScreen, st->camera);
    Vector2 snapped     = SnapVertexToGrid(mouseWorld, GRID_SPACING);

    // is the cursor over the chrome (header / dock / status)? if so the GUI owns it
    bool overGui = mouseScreen.y < HEADER_H ||
                   mouseScreen.y > GetScreenHeight() - STATUS_H ||
                   mouseScreen.x > GetScreenWidth() - PANEL_W;

    // --- input / update ---
    EditorHandleInput(st, mouseWorld, snapped, overGui);
    if (st->geometry_dirty) { MapCompilePortals(); st->geometry_dirty = false; }
    if (st->status_timer > 0.0f) st->status_timer -= GetFrameTime();

    // --- render ---
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), COL_BG);
    BeginMode2D(st->camera);
        EditorRenderGrid(st);
        EditorRenderWorld(st, mouseWorld, snapped);
    EndMode2D();

    EditorRenderGui(st, snapped);

    // floating cursor coordinate readout
    int gx = (int)roundf(snapped.x / GRID_SPACING);
    int gy = (int)roundf(snapped.y / GRID_SPACING);
    if (!overGui)
        DrawText(TextFormat("%d, %d", gx, gy),
                 (int)mouseScreen.x + 14, (int)mouseScreen.y + 8, 16, COL_TEXT);
}
