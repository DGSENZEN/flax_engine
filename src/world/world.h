
#ifndef FLAX_WORLD_H
#define FLAX_WORLD_H

#include "raylib.h"

// ===========================================================================
// Flax Engine - world model.
//
// The single source of truth for map geometry, shared by the editor, the
// renderer and the io/flaxmap serializer. Build/Doom-style data model: flat
// fixed arrays referenced by integer index (a stable handle). Nothing here
// touches the GPU or the filesystem, so the offline tool (flaxpack) can link
// this file without pulling in the editor or renderer.
//
// Textures are referenced *by name* at the world level: `texture_id` fields
// index the `world_texnames` table below, never a GPU texture directly. The
// renderer resolves names to loaded textures at mesh-build time. This is the
// same split Quake makes between the miptex names baked into a .bsp and the
// actual pixels living in a wad - it keeps serialization GPU-free.
//
// SLOPES (Build-engine style): a sector's floor/ceiling can hinge on its
// FIRST wall. floor_slope/ceil_slope is the height change per map-unit of
// signed perpendicular distance from that wall's line (positive on the
// wall's left side, looking a->b). 0 = flat. Evaluate with WorldFloorZAt().
//
// DOORS / SWITCHES (Doom-style): a sector with kind SECTOR_DOOR closes by
// dropping its ceiling to its floor at game start; the mover system animates
// it open. A wall with WALLF_SWITCH set triggers every door sector whose
// tag matches the wall's tag. A door with tag 0 opens by direct use.
// ===========================================================================

#define NO_LINK -1

// Fixed-size texture-name records: 31 chars + NUL. This is an on-disk
// contract (LUMP_TEXNAMES stores raw TEXNAME_LEN-byte rows), so it must
// never shrink between format versions.
#define TEXNAME_LEN  32
#define MAX_TEXNAMES 256

typedef struct Vertex {
    int id;             // editor-local unique id; not serialized
    Vector2 points;
} Vertex;

// Wall flags bitfield
#define WALLF_SWITCH 0x1   // usable: triggers door sectors with matching tag

typedef struct Wall {
    int point_start, point_end;   // indices into vertices[]
    int next_wall, next_sector;   // loop link / portal target (NO_LINK if solid)
    int texture_id, portal_wall;  // world_texnames index / opposing wall index
    int flags, tag;               // WALLF_* / switch target tag
} Wall;

typedef enum { SECTOR_NORMAL = 0, SECTOR_DOOR = 1 } SectorKind;

typedef struct Sector {
    int wall_start, wall_count;   // contiguous run in walls[] (all loops)
    float floor_z, ceilingz;
    int floor_texture, ceiling_texture;   // world_texnames indices
    float floor_slope, ceil_slope;        // rise per map-unit from first wall, 0 = flat
    int kind, tag;                        // SectorKind / trigger tag
    float light;                          // brightness multiplier, 1 = full (Doom light level)
} Sector;

// Entities: everything placed in the world that is not sector geometry.
// One struct for all types, Quake-entity style - the fields a type doesn't
// use stay zero and cost nothing.
typedef enum {
    ENT_DECAL  = 0,    // textured quad glued to the nearest wall
    ENT_SPAWN  = 1,    // enemy spawn point (data = enemy class)
    ENT_BRIDGE = 2,    // free-standing walkable box (the sector-engine bridge)
    ENT_AMMO   = 3,    // pickup: data = AmmoType, sx = amount
    ENT_HEALTH = 4,    // pickup: data = amount
    ENT_TYPE_COUNT
} EntityType;

typedef enum {
    AMMO_SHELLS = 0,
    AMMO_BULLETS = 1,
    AMMO_TYPE_COUNT
} AmmoType;

typedef struct Entity {
    int     type;       // EntityType
    Vector2 pos;        // map coordinates
    float   z;          // type-specific height/offset
    float   yaw;        // radians: bridge rotation, spawn facing
    float   sx, sy;     // half-extents in world units: decal w/h, bridge w/d
    int     texture_id; // world_texnames index (decal/bridge surface)
    int     data;       // type-specific small integer payload
} Entity;

extern Vertex vertices[];
extern Wall   walls[];
extern Sector sectors[];
extern Entity entities[];

extern int sector_counter;
extern int wall_counter;
extern int vertex_counter;
extern int entity_counter;

extern Vector2 playerStart;
extern float   playerStartYaw;

// Texture name table. Entry 0 is always "DEFAULT".
extern char world_texnames[MAX_TEXNAMES][TEXNAME_LEN];
extern int  texname_counter;

// Get-or-add a name (uppercased, truncated to TEXNAME_LEN-1, spaces become
// '_'). Returns the table index, or 0 (DEFAULT) if the table is full.
int  WorldTexnameId(const char *name);

// Name for a texture id; out-of-range ids resolve to "DEFAULT".
const char *WorldTexname(int id);

// Entity type <-> canonical name, for the text format and editor labels.
// Unknown names return -1.
const char *EntityTypeName(int type);
int         EntityTypeFromName(const char *name);

// Reset every counter and re-seed the texture table with DEFAULT. Called by
// the loaders before filling the arrays from a file.
void WorldClear(void);

// Sector containing a 2D map point (even-odd winding test over the wall
// loop), or -1. Map/editor coordinates, not world-scaled ones.
int WorldSectorAt(Vector2 p);

// Floor/ceiling height at a map point, slope-aware (flat sectors just return
// floor_z/ceilingz). Out-of-range sector returns 0 / a tall ceiling.
float WorldFloorZAt(int s, Vector2 p);
float WorldCeilZAt(int s, Vector2 p);

// World-space plane normals, slope-aware: floor points up, ceiling down.
// Shared by the renderer (shading) and the raycaster (hit normals).
Vector3 WorldFloorNormal(int s);
Vector3 WorldCeilNormal(int s);

// Split a sector's wall run into its closed loops by following the
// point_end -> point_start chains. Loop 0 is the outer boundary; later
// loops are holes (columns). Fills loop_counts (walls per loop) and
// returns the loop count, capped at max_loops.
int WorldSectorLoops(int s, int *loop_counts, int max_loops);

#endif // FLAX_WORLD_H
