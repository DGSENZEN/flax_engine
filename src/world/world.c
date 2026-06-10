#include "world/world.h"
#include "config.h"
#include <string.h>
#include <ctype.h>
#include <math.h>

// World storage. Static capacity in the 90s style: no per-frame allocation,
// indices are stable handles, bounds are known up front (see config.h).

Sector sectors[MAX_SECTORS];
int sector_counter = 0;

Wall walls[MAX_WALLS];
int wall_counter = 0;

Vertex vertices[MAX_VERTICES];
int vertex_counter = 0;

Entity entities[MAX_ENTITIES];
int entity_counter = 0;

Vector2 playerStart    = { 0.0f, 0.0f };
float   playerStartYaw = 0.0f;

static const char *ENT_NAMES[ENT_TYPE_COUNT] = { "DECAL", "SPAWN", "BRIDGE" };

const char *EntityTypeName(int type) {
    return (type >= 0 && type < ENT_TYPE_COUNT) ? ENT_NAMES[type] : "?";
}

int EntityTypeFromName(const char *name) {
    for (int t = 0; t < ENT_TYPE_COUNT; t++)
        if (strcmp(ENT_NAMES[t], name) == 0) return t;
    return -1;
}

char world_texnames[MAX_TEXNAMES][TEXNAME_LEN];
int  texname_counter = 0;

// Entry 0 must always exist so texture_id 0 is a safe default everywhere.
static void EnsureDefaultTexname(void) {
    if (texname_counter == 0) {
        strcpy(world_texnames[0], "DEFAULT");
        texname_counter = 1;
    }
}

// Normalize into the fixed-size record: uppercase, no spaces, truncated.
// Names are the identity that survives serialization, so they must be
// canonical before they ever reach the table.
static void NormalizeTexname(char out[TEXNAME_LEN], const char *name) {
    int n = 0;
    for (const char *c = name; *c && n < TEXNAME_LEN - 1; c++) {
        out[n++] = (*c == ' ') ? '_' : (char)toupper((unsigned char)*c);
    }
    out[n] = '\0';
    if (n == 0) strcpy(out, "DEFAULT");
}

int WorldTexnameId(const char *name) {
    EnsureDefaultTexname();
    char canon[TEXNAME_LEN];
    NormalizeTexname(canon, name);

    for (int i = 0; i < texname_counter; i++) {
        if (strcmp(world_texnames[i], canon) == 0) return i;
    }
    if (texname_counter >= MAX_TEXNAMES) return 0;
    strcpy(world_texnames[texname_counter], canon);
    return texname_counter++;
}

const char *WorldTexname(int id) {
    EnsureDefaultTexname();
    if (id < 0 || id >= texname_counter) return world_texnames[0];
    return world_texnames[id];
}

// Even-odd crossing test over one sector's wall loop. Plain C so the offline
// tool can use it too (no raylib collision functions).
static bool PointInSector(Vector2 p, int s) {
    int start = sectors[s].wall_start, n = sectors[s].wall_count;
    bool in = false;
    for (int i = 0; i < n; i++) {
        Vector2 a = vertices[walls[start + i].point_start].points;
        Vector2 b = vertices[walls[start + i].point_end].points;
        if ((a.y > p.y) != (b.y > p.y) &&
            p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
            in = !in;
    }
    return in;
}

int WorldSectorAt(Vector2 p) {
    for (int s = 0; s < sector_counter; s++) {
        if (sectors[s].wall_count < 3) continue;
        if (PointInSector(p, s)) return s;
    }
    return -1;
}

// Signed perpendicular distance (map units) from the sector's hinge line -
// the line through its first wall. Positive on the wall's left, looking
// start->end. This is the input the slope fields multiply.
static float HingeDistance(int s, Vector2 p) {
    Vector2 a = vertices[walls[sectors[s].wall_start].point_start].points;
    Vector2 b = vertices[walls[sectors[s].wall_start].point_end].points;
    float dx = b.x - a.x, dy = b.y - a.y;
    float len = sqrtf(dx * dx + dy * dy);
    if (len == 0.0f) return 0.0f;
    return (dx * (p.y - a.y) - dy * (p.x - a.x)) / len;
}

float WorldFloorZAt(int s, Vector2 p) {
    if (s < 0 || s >= sector_counter) return 0.0f;
    float z = sectors[s].floor_z;
    if (sectors[s].floor_slope != 0.0f && sectors[s].wall_count > 0)
        z += sectors[s].floor_slope * HingeDistance(s, p);
    return z;
}

float WorldCeilZAt(int s, Vector2 p) {
    if (s < 0 || s >= sector_counter) return 1000.0f;
    float z = sectors[s].ceilingz;
    if (sectors[s].ceil_slope != 0.0f && sectors[s].wall_count > 0)
        z += sectors[s].ceil_slope * HingeDistance(s, p);
    return z;
}

void WorldClear(void) {
    sector_counter = 0;
    wall_counter   = 0;
    vertex_counter = 0;
    entity_counter = 0;
    texname_counter = 0;
    playerStart    = (Vector2){ 0.0f, 0.0f };
    playerStartYaw = 0.0f;
    EnsureDefaultTexname();
}
