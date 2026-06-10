#ifndef FLAX_FLAXMAP_H
#define FLAX_FLAXMAP_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// ===========================================================================
// Flax Engine - map serialization (the "flaxmap" formats).
//
// Quake-style source -> compiler -> runtime split, two formats for one map:
//
//   .map   TEXT SOURCE.  Hand-editable, diff-able, what the editor saves.
//          Stores only *authored* data: vertices, wall loops with texture
//          names, sector heights, player start. Derived data (portal links)
//          is never written here - it is recomputed on load/bake, exactly
//          like Quake recompiles a .map into a .bsp.
//
//   .fmap  BAKED BINARY.  What the engine loads at runtime. Versioned
//          lump-directory layout (the Quake .bsp / Doom WAD pattern): a
//          fixed header, then a directory of {type, offset, length} entries,
//          then the lumps themselves. Readers seek straight to the lumps
//          they understand and skip the rest, so old engines can read new
//          files and lumps can be added without a version bump.
//
// Use `flaxpack` (tools/flaxpack.c) to convert between them from the shell:
//   flaxpack pack   dev.map  dev.fmap     text  -> binary (compiles portals)
//   flaxpack unpack dev.fmap dev.map      binary -> text  (for debugging/editing)
//   flaxpack dump   dev.fmap              print header, lump table, contents
//
// --------------------------- TEXT GRAMMAR (.map) ---------------------------
//
//   # comment - blank lines ignored. One statement per line:
//   flaxmap <version>
//   player  <x> <y> <yaw_radians>
//   vertex  <index> <x> <y>                          indices must be 0,1,2,...
//   wall    <index> <v_start> <v_end> <texname> [flags tag]
//   sector  <index> <wall_start> <wall_count> <floor_z> <ceil_z> <flrtex> <ceiltex>
//                   [floor_slope ceil_slope kind tag]
//   entity  <index> <TYPE> <x> <y> <z> <yaw> <sx> <sy> <texname> <data>
//
// Bracketed fields are optional and default to 0 - every v1 .map file is a
// valid v2 file. TYPE is DECAL / SPAWN / BRIDGE. Unknown keywords are warned
// about and skipped (forward compatibility).
//
// -------------------------- BINARY LAYOUT (.fmap) --------------------------
//
// All integers little-endian (the only byte order shipped hardware has used
// since the 486; Quake made the same call). Floats are IEEE-754 binary32.
//
//   FmapHeader      { char magic[4]="FMAP"; u32 version; u32 lump_count; }
//   FmapLumpEntry   { u32 type; u32 offset; u32 length; }   x lump_count
//   ...lump payloads at their recorded offsets...
//
// Lump payloads (record counts are implied by length / record size):
//
//   LUMP_TEXNAMES   N x char[TEXNAME_LEN]      NUL-padded texture names
//   LUMP_VERTICES   N x { f32 x, y }
//   LUMP_WALLS      N x { i32 v_start, v_end, next_wall, next_sector,
//                          portal_wall, texture, flags, tag }
//   LUMP_SECTORS    N x { i32 wall_start, wall_count; f32 floor_z, ceil_z;
//                          i32 floor_tex, ceil_tex;
//                          f32 floor_slope, ceil_slope; i32 kind, tag }
//   LUMP_PLAYER     1 x { f32 x, y, yaw }
//   LUMP_ENTITIES   N x { i32 type; f32 x, y, z, yaw, sx, sy;
//                          i32 texture, data }
//
// Version history (the loader reads all of them; the writer emits latest):
//   v1  walls without flags/tag, sectors without slopes/kind/tag, no
//       LUMP_ENTITIES. Old record sizes are kept below as Fmap*V1.
//   v2  current.
// ===========================================================================

#define FLAXMAP_MAGIC   "FMAP"
#define FLAXMAP_VERSION 2

typedef enum {
    LUMP_TEXNAMES = 0,
    LUMP_VERTICES = 1,
    LUMP_WALLS    = 2,
    LUMP_SECTORS  = 3,
    LUMP_PLAYER   = 4,
    LUMP_ENTITIES = 5,
    LUMP_COUNT
} FmapLumpType;

// On-disk records. These mirror the runtime structs but with explicit fixed
// widths - the runtime structs are free to grow editor-only fields without
// touching the format. All fields are naturally aligned so the structs have
// no padding on any sane compiler; keep it that way when adding fields.
typedef struct { char magic[4]; uint32_t version; uint32_t lump_count; } FmapHeader;
typedef struct { uint32_t type, offset, length; } FmapLumpEntry;
typedef struct { float x, y; } FmapVertex;
typedef struct { int32_t v_start, v_end, next_wall, next_sector, portal_wall, texture,
                 flags, tag; } FmapWall;
typedef struct { int32_t wall_start, wall_count; float floor_z, ceil_z;
                 int32_t floor_tex, ceil_tex; float floor_slope, ceil_slope;
                 int32_t kind, tag; } FmapSector;
typedef struct { float x, y, yaw; } FmapPlayer;
typedef struct { int32_t type; float x, y, z, yaw, sx, sy; int32_t texture, data; } FmapEntity;

// Frozen v1 record layouts, read-only back-compat.
typedef struct { int32_t v_start, v_end, next_wall, next_sector, portal_wall, texture; } FmapWallV1;
typedef struct { int32_t wall_start, wall_count; float floor_z, ceil_z;
                 int32_t floor_tex, ceil_tex; } FmapSectorV1;

// --- text source ------------------------------------------------------------
bool MapSourceSave(const char *path);   // world -> .map text
bool MapSourceLoad(const char *path);   // .map text -> world (+ compiles portals)

// --- baked binary -----------------------------------------------------------
bool MapBakedSave(const char *path);    // world -> .fmap (compiles portals first)
bool MapBakedLoad(const char *path);    // .fmap -> world, validated
bool MapBakedDump(const char *path, FILE *out);  // header/lumps/contents, human-readable

// The "compiler" pass: link walls that share an edge in opposite directions
// into portals (fills next_sector / portal_wall). Idempotent; runs on every
// bake and after every load of the text source.
void MapCompilePortals(void);

// Canonical map locations under the asset dir (only built when the target
// defines FLAX_ASSET_DIR; flaxpack takes explicit paths instead).
#ifdef FLAX_ASSET_DIR
#define FLAXMAP_SOURCE_PATH FLAX_ASSET_DIR "/maps/dev.map"
#define FLAXMAP_BAKED_PATH  FLAX_ASSET_DIR "/maps/dev.fmap"
#endif

#endif // FLAX_FLAXMAP_H
