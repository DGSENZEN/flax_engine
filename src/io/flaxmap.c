#include "io/flaxmap.h"
#include "world/world.h"
#include "config.h"
#include <string.h>

// ===========================================================================
// flaxmap serializer - see flaxmap.h for the format specs. Pure stdio + the
// world arrays; no raylib calls, so this compiles into the offline flaxpack
// tool unchanged.
// ===========================================================================

// ---------------------------------------------------------------------------
// Portal compiler
// ---------------------------------------------------------------------------

static int SectorOfWall(int w) {
    for (int i = 0; i < sector_counter; i++) {
        int start = sectors[i].wall_start;
        if (w >= start && w < start + sectors[i].wall_count) return i;
    }
    return NO_LINK;
}

// Exact-position compare. The editor welds coincident vertices and snaps to
// the grid, and the text format round-trips coordinates at full precision
// (%.9g), so shared edges really are bit-identical - no epsilon needed.
static bool SamePoint(Vector2 a, Vector2 b) { return a.x == b.x && a.y == b.y; }

void MapCompilePortals(void) {
    for (int w = 0; w < wall_counter; w++) {
        walls[w].next_sector = NO_LINK;
        walls[w].portal_wall = NO_LINK;
    }
    // O(n^2) edge matching: fine offline and on the editor's dirty flag.
    for (int a = 0; a < wall_counter; a++) {
        if (walls[a].next_sector != NO_LINK) continue;
        Vector2 a0 = vertices[walls[a].point_start].points;
        Vector2 a1 = vertices[walls[a].point_end].points;
        for (int b = a + 1; b < wall_counter; b++) {
            if (walls[b].next_sector != NO_LINK) continue;
            Vector2 b0 = vertices[walls[b].point_start].points;
            Vector2 b1 = vertices[walls[b].point_end].points;
            if (SamePoint(a0, b1) && SamePoint(a1, b0)) {   // shared edge, opposite dir
                walls[a].next_sector = SectorOfWall(b);
                walls[b].next_sector = SectorOfWall(a);
                walls[a].portal_wall = b;
                walls[b].portal_wall = a;
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Shared validation - both loaders funnel through this so a corrupt file can
// never leave dangling indices in the world arrays.
// ---------------------------------------------------------------------------

static bool ValidateWorld(const char *origin) {
    for (int w = 0; w < wall_counter; w++) {
        if (walls[w].point_start < 0 || walls[w].point_start >= vertex_counter ||
            walls[w].point_end   < 0 || walls[w].point_end   >= vertex_counter) {
            fprintf(stderr, "flaxmap: %s: wall %d references vertex out of range\n", origin, w);
            return false;
        }
        if (walls[w].texture_id < 0 || walls[w].texture_id >= texname_counter)
            walls[w].texture_id = 0;   // soft-fail unknown textures to DEFAULT
    }
    for (int s = 0; s < sector_counter; s++) {
        Sector *sec = &sectors[s];
        if (sec->wall_start < 0 || sec->wall_count < 0 ||
            sec->wall_start + sec->wall_count > wall_counter) {
            fprintf(stderr, "flaxmap: %s: sector %d wall range out of bounds\n", origin, s);
            return false;
        }
        if (sec->floor_texture   < 0 || sec->floor_texture   >= texname_counter) sec->floor_texture   = 0;
        if (sec->ceiling_texture < 0 || sec->ceiling_texture >= texname_counter) sec->ceiling_texture = 0;
        if (sec->kind < 0 || sec->kind > SECTOR_DOOR) sec->kind = SECTOR_NORMAL;
    }
    // drop entities with unknown types (newer file), clamp textures
    int kept = 0;
    for (int e = 0; e < entity_counter; e++) {
        if (entities[e].type < 0 || entities[e].type >= ENT_TYPE_COUNT) {
            fprintf(stderr, "flaxmap: %s: dropping entity %d with unknown type %d\n",
                    origin, e, entities[e].type);
            continue;
        }
        if (entities[e].texture_id < 0 || entities[e].texture_id >= texname_counter)
            entities[e].texture_id = 0;
        entities[kept++] = entities[e];
    }
    entity_counter = kept;
    return true;
}

// ===========================================================================
// Text source (.map)
// ===========================================================================

bool MapSourceSave(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "flaxmap: cannot write %s\n", path); return false; }

    fprintf(f, "# Flax map source. Hand-editable; '#' starts a comment.\n");
    fprintf(f, "# Bake for the engine with: flaxpack pack <this file> <out.fmap>\n");
    fprintf(f, "# Portal links are not stored here - they are recompiled on load.\n");
    fprintf(f, "flaxmap %d\n\n", FLAXMAP_VERSION);

    fprintf(f, "player %.9g %.9g %.9g\n\n", playerStart.x, playerStart.y, playerStartYaw);

    fprintf(f, "# vertex <index> <x> <y>   (%d total)\n", vertex_counter);
    for (int i = 0; i < vertex_counter; i++)
        fprintf(f, "vertex %d %.9g %.9g\n", i, vertices[i].points.x, vertices[i].points.y);

    fprintf(f, "\n# wall <index> <v_start> <v_end> <texture> [flags tag]   (%d total)\n", wall_counter);
    for (int i = 0; i < wall_counter; i++)
        fprintf(f, "wall %d %d %d %s %d %d\n", i, walls[i].point_start, walls[i].point_end,
                WorldTexname(walls[i].texture_id), walls[i].flags, walls[i].tag);

    fprintf(f, "\n# sector <index> <wall_start> <wall_count> <floor_z> <ceil_z> <floortex> <ceiltex>"
               " [floor_slope ceil_slope kind tag]   (%d total)\n", sector_counter);
    for (int i = 0; i < sector_counter; i++)
        fprintf(f, "sector %d %d %d %.9g %.9g %s %s %.9g %.9g %d %d\n", i,
                sectors[i].wall_start, sectors[i].wall_count,
                sectors[i].floor_z, sectors[i].ceilingz,
                WorldTexname(sectors[i].floor_texture),
                WorldTexname(sectors[i].ceiling_texture),
                sectors[i].floor_slope, sectors[i].ceil_slope,
                sectors[i].kind, sectors[i].tag);

    fprintf(f, "\n# entity <index> <type> <x> <y> <z> <yaw> <sx> <sy> <texture> <data>   (%d total)\n",
            entity_counter);
    for (int i = 0; i < entity_counter; i++)
        fprintf(f, "entity %d %s %.9g %.9g %.9g %.9g %.9g %.9g %s %d\n", i,
                EntityTypeName(entities[i].type),
                entities[i].pos.x, entities[i].pos.y, entities[i].z, entities[i].yaw,
                entities[i].sx, entities[i].sy,
                WorldTexname(entities[i].texture_id), entities[i].data);

    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

bool MapSourceLoad(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;

    WorldClear();
    char line[512];
    int lineno = 0;
    bool ok = true;

    while (ok && fgets(line, sizeof line, f)) {
        lineno++;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n' || *p == '#') continue;

        char key[32], tex_a[TEXNAME_LEN], tex_b[TEXNAME_LEN];
        int idx, ia, ib;
        float fa, fb, fc;

        if (sscanf(p, "%31s", key) != 1) continue;

        if (strcmp(key, "flaxmap") == 0) {
            int ver = 0;
            if (sscanf(p, "flaxmap %d", &ver) != 1 || ver > FLAXMAP_VERSION) {
                fprintf(stderr, "flaxmap: %s:%d: unsupported version %d\n", path, lineno, ver);
                ok = false;
            }
        } else if (strcmp(key, "player") == 0) {
            if (sscanf(p, "player %f %f %f", &fa, &fb, &fc) == 3) {
                playerStart = (Vector2){ fa, fb };
                playerStartYaw = fc;
            }
        } else if (strcmp(key, "vertex") == 0) {
            if (sscanf(p, "vertex %d %f %f", &idx, &fa, &fb) != 3 ||
                idx != vertex_counter || vertex_counter >= MAX_VERTICES) {
                fprintf(stderr, "flaxmap: %s:%d: bad vertex line\n", path, lineno);
                ok = false;
            } else {
                vertices[vertex_counter] = (Vertex){ .id = vertex_counter + 1, .points = { fa, fb } };
                vertex_counter++;
            }
        } else if (strcmp(key, "wall") == 0) {
            int flags = 0, tag = 0;          // optional trailing fields (v2)
            int got = sscanf(p, "wall %d %d %d %31s %d %d", &idx, &ia, &ib, tex_a, &flags, &tag);
            if (got < 4 || idx != wall_counter || wall_counter >= MAX_WALLS) {
                fprintf(stderr, "flaxmap: %s:%d: bad wall line\n", path, lineno);
                ok = false;
            } else {
                walls[wall_counter++] = (Wall){
                    .point_start = ia, .point_end = ib,
                    .next_wall = NO_LINK, .next_sector = NO_LINK,
                    .portal_wall = NO_LINK, .texture_id = WorldTexnameId(tex_a),
                    .flags = flags, .tag = tag,
                };
            }
        } else if (strcmp(key, "sector") == 0) {
            float fslope = 0.0f, cslope = 0.0f;   // optional trailing fields (v2)
            int kind = 0, tag = 0;
            int got = sscanf(p, "sector %d %d %d %f %f %31s %31s %f %f %d %d",
                             &idx, &ia, &ib, &fa, &fb, tex_a, tex_b,
                             &fslope, &cslope, &kind, &tag);
            if (got < 7 || idx != sector_counter || sector_counter >= MAX_SECTORS) {
                fprintf(stderr, "flaxmap: %s:%d: bad sector line\n", path, lineno);
                ok = false;
            } else {
                sectors[sector_counter++] = (Sector){
                    .wall_start = ia, .wall_count = ib,
                    .floor_z = fa, .ceilingz = fb,
                    .floor_texture = WorldTexnameId(tex_a),
                    .ceiling_texture = WorldTexnameId(tex_b),
                    .floor_slope = fslope, .ceil_slope = cslope,
                    .kind = kind, .tag = tag,
                };
            }
        } else if (strcmp(key, "entity") == 0) {
            char type_name[TEXNAME_LEN];
            float x, y, z, yaw, sx, sy;
            int data = 0;
            int got = sscanf(p, "entity %d %31s %f %f %f %f %f %f %31s %d",
                             &idx, type_name, &x, &y, &z, &yaw, &sx, &sy, tex_a, &data);
            int type = EntityTypeFromName(type_name);
            if (got < 10 || idx != entity_counter || entity_counter >= MAX_ENTITIES || type < 0) {
                fprintf(stderr, "flaxmap: %s:%d: bad/unknown entity line, skipping\n", path, lineno);
                // skipped, not fatal: an old engine reading a newer map's
                // entity types should still load the geometry
            } else {
                entities[entity_counter++] = (Entity){
                    .type = type, .pos = { x, y }, .z = z, .yaw = yaw,
                    .sx = sx, .sy = sy,
                    .texture_id = WorldTexnameId(tex_a), .data = data,
                };
            }
        } else {
            // unknown keyword: skip, don't fail - lets old engines read new maps
            fprintf(stderr, "flaxmap: %s:%d: skipping unknown keyword '%s'\n", path, lineno, key);
        }
    }
    fclose(f);

    if (ok) ok = ValidateWorld(path);
    if (!ok) { WorldClear(); return false; }

    // rebuild wall loop links (next_wall is derived from sector runs)
    for (int s = 0; s < sector_counter; s++) {
        int start = sectors[s].wall_start, n = sectors[s].wall_count;
        for (int i = 0; i < n; i++)
            walls[start + i].next_wall = start + (i + 1) % n;
    }
    MapCompilePortals();
    return true;
}

// ===========================================================================
// Baked binary (.fmap)
// ===========================================================================

bool MapBakedSave(const char *path) {
    MapCompilePortals();   // the bake step: derived data goes into the binary

    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "flaxmap: cannot write %s\n", path); return false; }

    FmapHeader hdr = { {'F','M','A','P'}, FLAXMAP_VERSION, LUMP_COUNT };
    FmapLumpEntry dir[LUMP_COUNT];

    uint32_t sizes[LUMP_COUNT] = {
        [LUMP_TEXNAMES] = (uint32_t)texname_counter * TEXNAME_LEN,
        [LUMP_VERTICES] = (uint32_t)vertex_counter  * sizeof(FmapVertex),
        [LUMP_WALLS]    = (uint32_t)wall_counter    * sizeof(FmapWall),
        [LUMP_SECTORS]  = (uint32_t)sector_counter  * sizeof(FmapSector),
        [LUMP_PLAYER]   = sizeof(FmapPlayer),
    };
    uint32_t off = sizeof hdr + sizeof dir;
    for (int t = 0; t < LUMP_COUNT; t++) {
        dir[t] = (FmapLumpEntry){ (uint32_t)t, off, sizes[t] };
        off += sizes[t];
    }

    bool ok = fwrite(&hdr, sizeof hdr, 1, f) == 1 &&
              fwrite(dir, sizeof dir, 1, f) == 1;

    for (int i = 0; ok && i < texname_counter; i++)
        ok = fwrite(world_texnames[i], TEXNAME_LEN, 1, f) == 1;

    for (int i = 0; ok && i < vertex_counter; i++) {
        FmapVertex r = { vertices[i].points.x, vertices[i].points.y };
        ok = fwrite(&r, sizeof r, 1, f) == 1;
    }
    for (int i = 0; ok && i < wall_counter; i++) {
        FmapWall r = { walls[i].point_start, walls[i].point_end, walls[i].next_wall,
                       walls[i].next_sector, walls[i].portal_wall, walls[i].texture_id };
        ok = fwrite(&r, sizeof r, 1, f) == 1;
    }
    for (int i = 0; ok && i < sector_counter; i++) {
        FmapSector r = { sectors[i].wall_start, sectors[i].wall_count,
                         sectors[i].floor_z, sectors[i].ceilingz,
                         sectors[i].floor_texture, sectors[i].ceiling_texture };
        ok = fwrite(&r, sizeof r, 1, f) == 1;
    }
    if (ok) {
        FmapPlayer r = { playerStart.x, playerStart.y, playerStartYaw };
        ok = fwrite(&r, sizeof r, 1, f) == 1;
    }
    fclose(f);
    return ok;
}

// Read one directory. Returns lump count read into dir[], or -1 on error.
static int ReadHeaderAndDir(FILE *f, const char *path, FmapLumpEntry dir[], int max_lumps) {
    FmapHeader hdr;
    if (fread(&hdr, sizeof hdr, 1, f) != 1 ||
        memcmp(hdr.magic, FLAXMAP_MAGIC, 4) != 0) {
        fprintf(stderr, "flaxmap: %s: not a flaxmap binary (bad magic)\n", path);
        return -1;
    }
    if (hdr.version > FLAXMAP_VERSION) {
        fprintf(stderr, "flaxmap: %s: version %u is newer than this engine (%d)\n",
                path, hdr.version, FLAXMAP_VERSION);
        return -1;
    }
    if (hdr.lump_count > (uint32_t)max_lumps) {
        fprintf(stderr, "flaxmap: %s: implausible lump count %u\n", path, hdr.lump_count);
        return -1;
    }
    if (fread(dir, sizeof(FmapLumpEntry), hdr.lump_count, f) != hdr.lump_count) {
        fprintf(stderr, "flaxmap: %s: truncated lump directory\n", path);
        return -1;
    }
    return (int)hdr.lump_count;
}

bool MapBakedLoad(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    FmapLumpEntry dir[64];
    int nlumps = ReadHeaderAndDir(f, path, dir, 64);
    if (nlumps < 0) { fclose(f); return false; }

    WorldClear();
    texname_counter = 0;   // the TEXNAMES lump replaces the seeded table wholesale
    bool ok = true;

    // Walk the directory; seek to each lump we understand, skip the rest.
    for (int l = 0; ok && l < nlumps; l++) {
        uint32_t n;
        if (fseek(f, (long)dir[l].offset, SEEK_SET) != 0) { ok = false; break; }

        switch (dir[l].type) {
        case LUMP_TEXNAMES:
            n = dir[l].length / TEXNAME_LEN;
            if (n > MAX_TEXNAMES) { ok = false; break; }
            for (uint32_t i = 0; ok && i < n; i++) {
                ok = fread(world_texnames[i], TEXNAME_LEN, 1, f) == 1;
                world_texnames[i][TEXNAME_LEN - 1] = '\0';
            }
            texname_counter = (int)n;
            break;

        case LUMP_VERTICES:
            n = dir[l].length / sizeof(FmapVertex);
            if (n > MAX_VERTICES) { ok = false; break; }
            for (uint32_t i = 0; ok && i < n; i++) {
                FmapVertex r;
                ok = fread(&r, sizeof r, 1, f) == 1;
                vertices[i] = (Vertex){ .id = (int)i + 1, .points = { r.x, r.y } };
            }
            vertex_counter = (int)n;
            break;

        case LUMP_WALLS:
            n = dir[l].length / sizeof(FmapWall);
            if (n > MAX_WALLS) { ok = false; break; }
            for (uint32_t i = 0; ok && i < n; i++) {
                FmapWall r;
                ok = fread(&r, sizeof r, 1, f) == 1;
                walls[i] = (Wall){ r.v_start, r.v_end, r.next_wall,
                                   r.next_sector, r.texture, r.portal_wall };
            }
            wall_counter = (int)n;
            break;

        case LUMP_SECTORS:
            n = dir[l].length / sizeof(FmapSector);
            if (n > MAX_SECTORS) { ok = false; break; }
            for (uint32_t i = 0; ok && i < n; i++) {
                FmapSector r;
                ok = fread(&r, sizeof r, 1, f) == 1;
                sectors[i] = (Sector){ r.wall_start, r.wall_count,
                                       r.floor_z, r.ceil_z, r.floor_tex, r.ceil_tex };
            }
            sector_counter = (int)n;
            break;

        case LUMP_PLAYER: {
            FmapPlayer r;
            ok = fread(&r, sizeof r, 1, f) == 1;
            playerStart = (Vector2){ r.x, r.y };
            playerStartYaw = r.yaw;
            break;
        }
        default:
            break;   // unknown lump from a newer tool: skip it
        }
    }
    fclose(f);

    if (texname_counter == 0) WorldTexnameId("DEFAULT");   // file had no TEXNAMES lump
    if (ok) ok = ValidateWorld(path);
    if (!ok) {
        fprintf(stderr, "flaxmap: %s: load failed, world cleared\n", path);
        WorldClear();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Debug dump: the unpack-for-humans path. Prints the raw container first
// (header + lump table), then the decoded contents.
// ---------------------------------------------------------------------------

bool MapBakedDump(const char *path, FILE *out) {
    static const char *lump_names[LUMP_COUNT] =
        { "TEXNAMES", "VERTICES", "WALLS", "SECTORS", "PLAYER" };

    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "flaxmap: cannot open %s\n", path); return false; }

    FmapLumpEntry dir[64];
    int nlumps = ReadHeaderAndDir(f, path, dir, 64);
    fclose(f);
    if (nlumps < 0) return false;

    fprintf(out, "%s\n", path);
    fprintf(out, "  magic   FMAP   version %d   lumps %d\n\n", FLAXMAP_VERSION, nlumps);
    fprintf(out, "  %-3s %-10s %8s %8s\n", "id", "lump", "offset", "length");
    for (int l = 0; l < nlumps; l++) {
        const char *nm = dir[l].type < LUMP_COUNT ? lump_names[dir[l].type] : "?UNKNOWN";
        fprintf(out, "  %-3u %-10s %8u %8u\n", dir[l].type, nm, dir[l].offset, dir[l].length);
    }

    if (!MapBakedLoad(path)) return false;

    fprintf(out, "\n  textures %d   vertices %d   walls %d   sectors %d\n",
            texname_counter, vertex_counter, wall_counter, sector_counter);
    fprintf(out, "  player (%.9g, %.9g) yaw %.9g\n\n", playerStart.x, playerStart.y, playerStartYaw);

    for (int i = 0; i < texname_counter; i++)
        fprintf(out, "  tex %3d  %s\n", i, world_texnames[i]);
    fprintf(out, "\n");
    for (int s = 0; s < sector_counter; s++) {
        int portals = 0;
        for (int i = 0; i < sectors[s].wall_count; i++)
            if (walls[sectors[s].wall_start + i].next_sector != NO_LINK) portals++;
        fprintf(out, "  sector %3d  walls %d..%d  floor %.9g ceil %.9g  portals %d  flr %s ceil %s\n",
                s, sectors[s].wall_start, sectors[s].wall_start + sectors[s].wall_count - 1,
                sectors[s].floor_z, sectors[s].ceilingz, portals,
                WorldTexname(sectors[s].floor_texture), WorldTexname(sectors[s].ceiling_texture));
    }
    return true;
}
