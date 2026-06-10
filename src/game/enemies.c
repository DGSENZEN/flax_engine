#include "game/enemies.h"
#include "game/player.h"
#include "world/world.h"
#include "world/raycast.h"
#include "renderer/sectors.h"
#include "renderer/billboard.h"
#include "core/log.h"
#include "config.h"
#include "rlgl.h"
#include <raymath.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Sprite sheet geometry (assets/sprites/zombie.png, 2033x1333). Measured
// from the sheet's teal grid: 127px columns, 159px row stride, 1px dark
// separators (color-keyed out with the cyan background at load).
//
//   rows 0-4   walk cols 0-5, attack cols 6-12, pain col 14, per direction
//              (0 = facing camera .. 4 = facing away; left side mirrors)
//   row y=826  headshot death cols 0-6 (gibs live at 8-15, unused yet)
//   row y=1000 knocked-down death cols 9-14
// ---------------------------------------------------------------------------
#define CELL_W       127
#define CELL_H       158
#define DIR_ROW_Y(r) (16 + 159 * (r))
#define WALK_FRAMES  6
#define ATK_COL0     6
#define ATK_FRAMES   7
#define PAIN_COL     14
#define HEADSHOT_Y   826
#define HEADSHOT_FRAMES 7
#define DEATH_Y      1000
#define DEATH_COL0   9
#define DEATH_FRAMES 6

#define MAX_ENEMIES    32

#define Z_SPEED        8.0f     // world units/s (player runs 27)
#define Z_WANDER_SPEED 4.5f
#define Z_RADIUS       1.1f
#define Z_HEIGHT       4.8f     // collision cylinder
#define Z_DRAW_H       5.4f     // billboard height
#define Z_HP           60.0f
#define Z_STEP         1.8f
#define Z_ATTACK_RANGE 3.4f
#define Z_DAMAGE       12.0f
#define Z_PAIN_TIME    0.4f
#define WALK_FPS       8.0f
#define ATK_FPS        8.0f
#define DEATH_FPS      10.0f
#define HEADSHOT_MULT  3.0f

// Perception: a vision cone gated by an actual wall raycast, plus a short
// "hearing" radius that ignores facing (you can't tiptoe up their back).
#define Z_SIGHT_RANGE  70.0f
#define Z_FOV_HALF     (60.0f * DEG2RAD)    // 120 degree cone
#define Z_HEAR_RANGE   6.0f
#define Z_EYE          3.8f
#define Z_DEAGGRO_DIST 110.0f
#define Z_LOST_TIME    6.0f                 // seconds without sight -> give up

typedef enum { ZS_IDLE, ZS_WANDER, ZS_CHASE, ZS_ATTACK, ZS_PAIN, ZS_DIE, ZS_DEAD } ZombieState;

typedef struct {
    bool    active;
    Vector3 pos;            // feet, world units
    float   yaw;            // facing, world xz
    int     sector;
    float   health;
    int     state;
    float   anim_t;         // seconds in current state (drives animation)
    float   ai_t;           // idle/wander phase countdown
    float   attack_cd;      // staggered swings: can't re-attack until 0
    float   lost_t;         // chase: seconds since the player was last seen
    float   wander_yaw;
    bool    headshot_death; // picks the decapitation row
    bool    swung;          // this attack already dealt its damage
} Zombie;

static float Rand01(void) { return (float)GetRandomValue(0, 1000) / 1000.0f; }

static bool Live(const Zombie *z) {
    return z->active && z->state != ZS_DIE && z->state != ZS_DEAD;
}

static Zombie    g_z[MAX_ENEMIES];
static Texture2D g_tex;

void EnemiesInit(void) {
    for (int i = 0; i < MAX_ENEMIES; i++) g_z[i].active = false;

    if (g_tex.id == 0) {
        Image img = LoadImage(FLAX_ASSET_DIR "/sprites/zombie.png");
        if (img.data) {
            // the sheet ships as 24-bit RGB: add the alpha channel BEFORE
            // keying, or the replaced pixels come out black instead of clear
            ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            ImageColorReplace(&img, (Color){ 0, 255, 255, 255 }, BLANK);  // cell bg
            ImageColorReplace(&img, (Color){ 0, 128, 128, 255 }, BLANK);  // grid lines
            g_tex = LoadTextureFromImage(img);
            SetTextureFilter(g_tex, TEXTURE_FILTER_POINT);
            UnloadImage(img);
            FLOG_INFO(LOGCAT_GAME, "zombie sheet loaded (%dx%d)", g_tex.width, g_tex.height);
        } else {
            FLOG_WARN(LOGCAT_GAME, "zombie sheet missing: assets/sprites/zombie.png");
        }
    }

    int spawned = 0;
    for (int e = 0; e < entity_counter; e++)
        if (entities[e].type == ENT_SPAWN && EnemySpawn(entities[e].pos)) spawned++;
    if (spawned) FLOG_INFO(LOGCAT_GAME, "spawned %d enemies from map", spawned);
}

bool EnemySpawn(Vector2 map_pos) {
    int s = WorldSectorAt(map_pos);
    if (s < 0) return false;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (g_z[i].active) continue;
        g_z[i] = (Zombie){
            .active = true,
            .pos = SectorToWorld(map_pos, WorldFloorZAt(s, map_pos)),
            .yaw = Rand01() * 2.0f * PI,
            .sector = s,
            .health = Z_HP,
            .state = ZS_IDLE,
            .ai_t = 1.0f + 2.0f * Rand01(),
        };
        return true;
    }
    return false;
}

// Circle-vs-wall push-out, the player's collision rules at zombie scale:
// portals pass when the far floor is within step reach and there is room.
static void CollideWalls(Zombie *z) {
    Vector2 p = { z->pos.x, z->pos.z };
    for (int w = 0; w < wall_counter; w++) {
        Vector2 a = Vector2Scale(vertices[walls[w].point_start].points, WORLD_SCALE);
        Vector2 b = Vector2Scale(vertices[walls[w].point_end].points, WORLD_SCALE);
        Vector2 ab = Vector2Subtract(b, a);
        float lenSq = Vector2LengthSqr(ab);
        if (lenSq == 0.0f) continue;
        float t = Clamp(Vector2DotProduct(Vector2Subtract(p, a), ab) / lenSq, 0.0f, 1.0f);
        Vector2 closest = Vector2Add(a, Vector2Scale(ab, t));
        Vector2 away = Vector2Subtract(p, closest);
        float dist = Vector2Length(away);
        if (dist >= Z_RADIUS) continue;

        int ns = walls[w].next_sector;
        if (ns != NO_LINK) {
            Vector2 cm = Vector2Scale(closest, 1.0f / WORLD_SCALE);
            float nf = WorldFloorZAt(ns, cm), nc = WorldCeilZAt(ns, cm);
            if (nf <= z->pos.y + Z_STEP && nc - fmaxf(z->pos.y, nf) >= Z_HEIGHT)
                continue;                                  // passable portal
        }
        Vector2 n = dist > 0.0001f ? Vector2Scale(away, 1.0f / dist)
                                   : Vector2Normalize((Vector2){ -ab.y, ab.x });
        p = Vector2Add(p, Vector2Scale(n, Z_RADIUS - dist + 0.001f));
    }
    z->pos.x = p.x;
    z->pos.z = p.y;
}

static void SnapFloor(Zombie *z) {
    Vector2 mp = { z->pos.x / WORLD_SCALE, z->pos.z / WORLD_SCALE };
    int s = WorldSectorAt(mp);
    if (s >= 0) {
        z->sector = s;
        z->pos.y = WorldFloorZAt(s, mp);
    }
}

// Can this zombie perceive the player? Distance, then the vision cone
// (skipped when already alerted), then a wall raycast eye-to-eye - sector
// geometry genuinely blocks sight lines.
static bool SeesPlayer(const Zombie *z, Vector3 pl, bool use_fov) {
    float dx = pl.x - z->pos.x, dz = pl.z - z->pos.z;
    float dist = sqrtf(dx * dx + dz * dz);
    if (dist > Z_SIGHT_RANGE) return false;

    if (use_fov && dist > Z_HEAR_RANGE) {
        float rel = atan2f(dz, dx) - z->yaw;
        while (rel >  PI) rel -= 2.0f * PI;
        while (rel < -PI) rel += 2.0f * PI;
        if (fabsf(rel) > Z_FOV_HALF) return false;
    }

    Vector3 eye = { z->pos.x, z->pos.y + Z_EYE, z->pos.z };
    Vector3 d   = Vector3Subtract((Vector3){ pl.x, pl.y + 4.0f, pl.z }, eye);
    float len = Vector3Length(d);
    RayHit hit;
    return !WorldRaycast(eye, d, len - 0.5f, &hit);
}

static void StartChase(Zombie *z) {
    z->state = ZS_CHASE;
    z->anim_t = 0.0f;
    z->lost_t = 0.0f;
}

void EnemiesAlertAt(Vector3 pos, float radius) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Zombie *z = &g_z[i];
        if (!Live(z)) continue;
        if (z->state != ZS_IDLE && z->state != ZS_WANDER) continue;
        float dx = pos.x - z->pos.x, dz = pos.z - z->pos.z;
        if (dx * dx + dz * dz < radius * radius) StartChase(z);
    }
}

void EnemiesUpdate(float dt) {
    Vector3 pl = PlayerPosition();

    for (int i = 0; i < MAX_ENEMIES; i++) {
        Zombie *z = &g_z[i];
        if (!z->active) continue;
        z->anim_t += dt;
        if (z->attack_cd > 0.0f) z->attack_cd -= dt;

        switch (z->state) {
        case ZS_DEAD:
            break;

        case ZS_DIE: {
            int frames = z->headshot_death ? HEADSHOT_FRAMES : DEATH_FRAMES;
            if (z->anim_t * DEATH_FPS >= (float)frames) z->state = ZS_DEAD;
            break;
        }

        case ZS_PAIN:
            if (z->anim_t >= Z_PAIN_TIME) StartChase(z);   // being shot aggros
            break;

        case ZS_IDLE:
            if (SeesPlayer(z, pl, true)) { StartChase(z); break; }
            z->ai_t -= dt;
            if (z->ai_t <= 0.0f) {                          // stretch the legs
                z->state = ZS_WANDER;
                z->anim_t = 0.0f;
                z->ai_t = 2.0f + 3.0f * Rand01();
                z->wander_yaw = Rand01() * 2.0f * PI;
            }
            break;

        case ZS_WANDER: {
            if (SeesPlayer(z, pl, true)) { StartChase(z); break; }
            z->ai_t -= dt;
            if (z->ai_t <= 0.0f) {                          // pause and look around
                z->state = ZS_IDLE;
                z->anim_t = 0.0f;
                z->ai_t = 1.5f + 2.5f * Rand01();
                break;
            }
            z->yaw = z->wander_yaw;
            float ox = z->pos.x, oz = z->pos.z;
            z->pos.x += cosf(z->yaw) * Z_WANDER_SPEED * dt;
            z->pos.z += sinf(z->yaw) * Z_WANDER_SPEED * dt;
            CollideWalls(z);
            // mostly stopped by a wall: turn somewhere else
            float mx = z->pos.x - ox, mz = z->pos.z - oz;
            float want = Z_WANDER_SPEED * dt;
            if (mx * mx + mz * mz < want * want * 0.16f)
                z->wander_yaw = Rand01() * 2.0f * PI;
            SnapFloor(z);
            break;
        }

        case ZS_CHASE: {
            float dx = pl.x - z->pos.x, dz = pl.z - z->pos.z;
            float dist = sqrtf(dx * dx + dz * dz);
            z->yaw = atan2f(dz, dx);

            if (SeesPlayer(z, pl, false)) z->lost_t = 0.0f;
            else                          z->lost_t += dt;
            if (dist > Z_DEAGGRO_DIST || z->lost_t > Z_LOST_TIME) {
                z->state = ZS_IDLE;                         // shrug, go back to roaming
                z->anim_t = 0.0f;
                z->ai_t = 1.0f + 2.0f * Rand01();
                break;
            }

            if (dist < Z_ATTACK_RANGE && fabsf(pl.y - z->pos.y) < 4.0f &&
                z->attack_cd <= 0.0f) {
                z->state = ZS_ATTACK;
                z->anim_t = 0.0f;
                z->swung = false;
                break;
            }
            if (dist > Z_RADIUS + 1.4f) {     // don't crowd into the player
                z->pos.x += cosf(z->yaw) * Z_SPEED * dt;
                z->pos.z += sinf(z->yaw) * Z_SPEED * dt;
                CollideWalls(z);
            }
            SnapFloor(z);
            break;
        }

        case ZS_ATTACK: {
            int frame = (int)(z->anim_t * ATK_FPS);
            if (!z->swung && frame >= 3) {    // the axe comes down
                z->swung = true;
                float dx = pl.x - z->pos.x, dz = pl.z - z->pos.z;
                if (sqrtf(dx * dx + dz * dz) < Z_ATTACK_RANGE + 0.6f)
                    PlayerDamage(Z_DAMAGE + (float)GetRandomValue(0, 6));
            }
            if (frame >= ATK_FRAMES) {
                StartChase(z);
                z->attack_cd = 0.4f + 0.6f * Rand01();      // stagger the mob
            }
            break;
        }
        }
    }

    // separation: live zombies shove each other (and never the player's
    // space) apart, so a pack spreads into a front instead of a stack
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Zombie *a = &g_z[i];
        if (!Live(a)) continue;

        for (int j = i + 1; j < MAX_ENEMIES; j++) {
            Zombie *b = &g_z[j];
            if (!Live(b)) continue;
            float dx = b->pos.x - a->pos.x, dz = b->pos.z - a->pos.z;
            float d2 = dx * dx + dz * dz, min = 2.0f * Z_RADIUS;
            if (d2 >= min * min) continue;
            float d = sqrtf(d2);
            float nx, nz;
            if (d > 0.001f) { nx = dx / d; nz = dz / d; }
            else { float r = Rand01() * 2.0f * PI; nx = cosf(r); nz = sinf(r); d = 0.0f; }
            float push = (min - d) * 0.5f;
            a->pos.x -= nx * push; a->pos.z -= nz * push;
            b->pos.x += nx * push; b->pos.z += nz * push;
        }

        float dx = a->pos.x - pl.x, dz = a->pos.z - pl.z;
        float d2 = dx * dx + dz * dz, min = Z_RADIUS + 1.3f;
        if (d2 < min * min && d2 > 0.0001f) {
            float d = sqrtf(d2);
            a->pos.x = pl.x + dx / d * min;
            a->pos.z = pl.z + dz / d * min;
        }

        CollideWalls(a);     // pushes must not shove anyone through a wall
        SnapFloor(a);
    }
}

int EnemiesRayHit(Vector3 origin, Vector3 dir, float max_dist,
                  float *dist, bool *headshot) {
    int best = -1;
    float bestT = max_dist;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Zombie *z = &g_z[i];
        if (!z->active || z->state == ZS_DIE || z->state == ZS_DEAD) continue;

        // 2D ray vs circle in xz, then the height gate
        float ox = origin.x - z->pos.x, oz = origin.z - z->pos.z;
        float a = dir.x * dir.x + dir.z * dir.z;
        if (a < 1e-8f) continue;
        float b = 2.0f * (ox * dir.x + oz * dir.z);
        float c = ox * ox + oz * oz - Z_RADIUS * Z_RADIUS * 1.44f;  // forgiving hitbox
        float disc = b * b - 4.0f * a * c;
        if (disc < 0.0f) continue;
        float t = (-b - sqrtf(disc)) / (2.0f * a);
        if (t < 0.0f) t = (-b + sqrtf(disc)) / (2.0f * a);
        if (t < 0.0f || t >= bestT) continue;

        float y = origin.y + dir.y * t;
        if (y < z->pos.y || y > z->pos.y + Z_HEIGHT) continue;

        best = i;
        bestT = t;
        *headshot = y > z->pos.y + Z_HEIGHT * 0.72f;
    }
    if (best >= 0) *dist = bestT;
    return best;
}

void EnemyDamage(int idx, float dmg, bool headshot) {
    if (idx < 0 || idx >= MAX_ENEMIES) return;
    Zombie *z = &g_z[idx];
    if (!z->active || z->state == ZS_DIE || z->state == ZS_DEAD) return;

    z->health -= dmg * (headshot ? HEADSHOT_MULT : 1.0f);
    if (z->health <= 0.0f) {
        z->state = ZS_DIE;
        z->anim_t = 0.0f;
        z->headshot_death = headshot;
        FLOG_INFO(LOGCAT_GAME, "zombie %d killed%s", idx, headshot ? " (headshot)" : "");
    } else {
        z->state = ZS_PAIN;
        z->anim_t = 0.0f;
    }
}

int EnemiesAlive(void) {
    int n = 0;
    for (int i = 0; i < MAX_ENEMIES; i++)
        if (g_z[i].active && g_z[i].state != ZS_DIE && g_z[i].state != ZS_DEAD) n++;
    return n;
}

void EnemiesDraw3D(Camera camera) {
    if (g_tex.id == 0) return;

    // Sort far-to-near and draw with depth WRITES off (test stays on).
    // Alpha-blended quads otherwise stamp their whole rectangle - invisible
    // pixels included - into the depth buffer, and every sprite drawn later
    // gets rectangular holes cut wherever sprites overlap on screen.
    int   order[MAX_ENEMIES];
    float depth[MAX_ENEMIES];
    int n = 0;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!g_z[i].active) continue;
        float dx = g_z[i].pos.x - camera.position.x;
        float dy = g_z[i].pos.y - camera.position.y;
        float dz = g_z[i].pos.z - camera.position.z;
        order[n] = i;
        depth[n] = dx * dx + dy * dy + dz * dz;
        n++;
    }
    for (int i = 1; i < n; i++) {              // insertion sort, descending
        int oi = order[i];
        float di = depth[i];
        int j = i - 1;
        while (j >= 0 && depth[j] < di) {
            order[j + 1] = order[j];
            depth[j + 1] = depth[j];
            j--;
        }
        order[j + 1] = oi;
        depth[j + 1] = di;
    }

    rlDisableDepthMask();
    for (int k = 0; k < n; k++) {
        Zombie *z = &g_z[order[k]];

        Rectangle src = { 0, 0, CELL_W - 1, CELL_H };
        bool mirror = false;

        if (z->state == ZS_DIE || z->state == ZS_DEAD) {
            int frames = z->headshot_death ? HEADSHOT_FRAMES : DEATH_FRAMES;
            int f = (int)(z->anim_t * DEATH_FPS);
            if (f >= frames) f = frames - 1;
            src.x = (float)((z->headshot_death ? f : DEATH_COL0 + f) * CELL_W);
            src.y = z->headshot_death ? HEADSHOT_Y : DEATH_Y;
        } else {
            // Build-style 8-angle pick: 5 drawn rows, left side mirrored
            float toCam = atan2f(camera.position.z - z->pos.z,
                                 camera.position.x - z->pos.x);
            float rel = toCam - z->yaw;
            while (rel >  PI) rel -= 2.0f * PI;
            while (rel < -PI) rel += 2.0f * PI;
            int row = (int)roundf(fabsf(rel) / (PI / 4.0f));
            if (row > 4) row = 4;
            mirror = (rel < 0.0f) && row > 0 && row < 4;

            int col;
            if (z->state == ZS_ATTACK)
                col = ATK_COL0 + (int)(z->anim_t * ATK_FPS) % ATK_FRAMES;
            else if (z->state == ZS_PAIN)
                col = PAIN_COL;
            else if (z->state == ZS_IDLE)
                col = ATK_COL0;                 // standing-with-axe pose
            else
                col = (int)(z->anim_t * WALK_FPS) % WALK_FRAMES;

            src.x = (float)(col * CELL_W);
            src.y = (float)DIR_ROW_Y(row);
        }

        // corpses lie low; everything else stands cell-height tall
        float h = Z_DRAW_H;
        float w = h * ((float)CELL_W / (float)CELL_H);
        Vector3 center = { z->pos.x, z->pos.y + h * 0.5f, z->pos.z };

        // match the world's sector light so sprites sit in the scene
        float light = (z->sector >= 0) ? sectors[z->sector].light : 1.0f;
        unsigned char g = (unsigned char)(Clamp(light, 0.0f, 1.0f) * 255.0f);
        DrawSpriteBillboard(camera, g_tex, src, center, (Vector2){ w, h },
                            mirror, (Color){ g, g, g, 255 });
    }
    rlEnableDepthMask();
}
