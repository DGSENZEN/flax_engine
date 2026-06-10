#include "game/player.h"
#include "world/world.h"
#include "renderer/sectors.h"
#include "core/profiler.h"
#include "core/log.h"
#include "config.h"
#include <raymath.h>

// ===========================================================================
// Quake-style movement over the sector world.
//
// The movement model is the classic id trio - friction, ground accelerate,
// air accelerate - which is what produces the Quake "feel": instant-ish
// starts, a hard speed cap on the ground, and the low air-accel cap that
// makes strafe-jumping work (air speed gain comes from turning into the
// strafe, not from holding forward).
//
// Collision is sector-native, no BSP needed:
//   - Horizontal: collide-and-slide. The player is a circle in the 2D map;
//     each blocking wall pushes the circle out along its normal and clips
//     velocity to the wall tangent. A wall blocks when it is solid, or when
//     it is a portal whose far floor is too high to step / headroom too low.
//   - Vertical: feet track the current sector's floor. Small rises are
//     stepped up, drops beyond step height start a fall, gravity integrates
//     while airborne, ceilings clamp jumps.
//
// Scale: 1 world unit ~ 0.3m (a 10-unit ceiling is a 3m room). All tuning
// constants below are expressed in that scale, converted from Quake's.
// ===========================================================================

#define PL_HEIGHT      5.0f    // collision height, feet to crown (~1.5m)
#define PL_EYE         4.4f    // eye above feet
#define PL_RADIUS      1.2f    // collision circle (~0.35m)
#define PL_STEP        1.8f    // max step-up (~0.55m, Quake's 18u ratio)

#define PL_SPEED       27.0f   // ground wishspeed cap   (Quake 320 ups)
#define PL_SPRINT      1.5f    // shift multiplier
#define PL_ACCEL       10.0f   // ground acceleration    (Quake 10)
#define PL_AIR_ACCEL   12.0f   // air acceleration
#define PL_AIR_CAP     2.5f    // air wishspeed cap      (Quake 30 ups)
#define PL_FRICTION    6.0f    // ground friction        (Quake 6)
#define PL_GRAVITY     67.0f   // (Quake 800 ups^2)
#define PL_JUMP        23.0f   // jump impulse           (Quake 270 ups)

#define MOUSE_SENS     0.0025f
#define KEY_TURN_SPEED 2.5f    // arrow-key fallback turning, rad/s

#define STEP_SMOOTH_RATE 14.0f // eye-height catch-up after a step snap, 1/s

typedef struct {
    Vector3 pos;         // FEET position, world units
    Vector3 vel;
    float   yaw, pitch;
    float   step_smooth; // eye offset left over from step snaps, decays to 0
    float   health;
    float   hurt_flash;  // 1 on damage, decays - drives the HUD red flash
    bool    on_ground;
    bool    noclip;
    int     sector;      // current sector index, or -1 (off the map)
} GamePlayer;

static GamePlayer g_pl;

// World-scaled position -> map coordinates (the sector polygons' space).
static Vector2 MapPosOf(Vector3 world) {
    return (Vector2){ world.x / WORLD_SCALE, world.z / WORLD_SCALE };
}

// Slope-aware heights under a world position (WorldFloorZAt/WorldCeilZAt
// already fall back to 0 / a tall ceiling for sector -1).
static float FloorUnder(int s, Vector3 world) { return WorldFloorZAt(s, MapPosOf(world)); }
static float CeilOver(int s, Vector3 world)   { return WorldCeilZAt(s, MapPosOf(world)); }

// ---------------------------------------------------------------------------
// Spawn
// ---------------------------------------------------------------------------

void PlayerSpawn(Vector2 map_pos, float yaw) {
    g_pl = (GamePlayer){ 0 };
    g_pl.health = 100.0f;
    g_pl.sector = WorldSectorAt(map_pos);
    float floor = WorldFloorZAt(g_pl.sector, map_pos);
    g_pl.pos = SectorToWorld(map_pos, floor);
    g_pl.yaw = yaw;
    g_pl.on_ground = true;
    FLOG_INFO(LOGCAT_GAME, "player spawn at (%.1f, %.1f) sector %d floor %.1f",
              map_pos.x, map_pos.y, g_pl.sector, floor);
}

// ---------------------------------------------------------------------------
// The id movement trio
// ---------------------------------------------------------------------------

static void ApplyFriction(float dt) {
    Vector2 v = { g_pl.vel.x, g_pl.vel.z };
    float speed = Vector2Length(v);
    if (speed < 0.1f) { g_pl.vel.x = g_pl.vel.z = 0.0f; return; }
    float drop = speed * PL_FRICTION * dt;
    float scale = fmaxf(speed - drop, 0.0f) / speed;
    g_pl.vel.x *= scale;
    g_pl.vel.z *= scale;
}

// Quake's Accelerate: only add speed up to the gap between current velocity
// projected on wishdir and wishspeed. With the tiny air cap, holding forward
// mid-air adds almost nothing - turning into a strafe does. That asymmetry
// IS strafe-jumping.
static void Accelerate(Vector2 wishdir, float wishspeed, float accel, float dt) {
    float cur = g_pl.vel.x * wishdir.x + g_pl.vel.z * wishdir.y;
    float add = wishspeed - cur;
    if (add <= 0.0f) return;
    float speed = fminf(accel * wishspeed * dt, add);
    g_pl.vel.x += speed * wishdir.x;
    g_pl.vel.z += speed * wishdir.y;
}

// ---------------------------------------------------------------------------
// Collision
// ---------------------------------------------------------------------------

// Can the player cross wall w at this feet height? Portals pass when the far
// side is reachable: floor within step height, and enough headroom over the
// higher of the two floors. Heights are evaluated at the contact point so
// sloped neighbors block or admit exactly where the player touches them.
static bool WallBlocks(int w, float feet, Vector2 contact_map) {
    int ns = walls[w].next_sector;
    if (ns == NO_LINK) return true;                          // solid
    float nf = WorldFloorZAt(ns, contact_map);
    float nc = WorldCeilZAt(ns, contact_map);
    if (nf > feet + PL_STEP) return true;                    // ledge too high
    if (nc - fmaxf(feet, nf) < PL_HEIGHT) return true;       // can't fit
    return false;
}

// Push the player circle out of every blocking wall and clip velocity to the
// wall tangent (the "slide"). A few passes settle corners where two pushes
// fight each other.
static void CollideAndSlide(void) {
    for (int pass = 0; pass < 3; pass++) {
        bool touched = false;
        Vector2 p = { g_pl.pos.x, g_pl.pos.z };

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
            if (dist >= PL_RADIUS) continue;

            Vector2 contact_map = Vector2Scale(closest, 1.0f / WORLD_SCALE);
            if (!WallBlocks(w, g_pl.pos.y, contact_map)) continue;

            // push out along the contact normal; degenerate contact (center
            // exactly on the wall) falls back to the wall's left normal
            Vector2 n = dist > 0.0001f ? Vector2Scale(away, 1.0f / dist)
                                       : Vector2Normalize((Vector2){ -ab.y, ab.x });
            p = Vector2Add(p, Vector2Scale(n, PL_RADIUS - dist + 0.001f));

            float into = g_pl.vel.x * n.x + g_pl.vel.z * n.y;
            if (into < 0.0f) {                       // moving into the wall: clip
                g_pl.vel.x -= into * n.x;
                g_pl.vel.z -= into * n.y;
            }
            touched = true;
        }
        g_pl.pos.x = p.x;
        g_pl.pos.z = p.y;
        if (!touched) break;
    }
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

void PlayerUpdate(float dt) {
    PROF_BEGIN("player");

    // --- look: mouse + arrow-key fallback ---
    Vector2 md = GetMouseDelta();
    g_pl.yaw   += md.x * MOUSE_SENS;
    g_pl.pitch -= md.y * MOUSE_SENS;
    if (IsKeyDown(KEY_LEFT))  g_pl.yaw -= KEY_TURN_SPEED * dt;
    if (IsKeyDown(KEY_RIGHT)) g_pl.yaw += KEY_TURN_SPEED * dt;
    g_pl.pitch = Clamp(g_pl.pitch, -1.55f, 1.55f);

    // --- wish direction from WASD, relative to yaw ---
    Vector2 fwd   = { cosf(g_pl.yaw), sinf(g_pl.yaw) };
    Vector2 right = { -fwd.y, fwd.x };
    Vector2 wish = { 0 };
    if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP))   wish = Vector2Add(wish, fwd);
    if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) wish = Vector2Subtract(wish, fwd);
    if (IsKeyDown(KEY_D)) wish = Vector2Add(wish, right);
    if (IsKeyDown(KEY_A)) wish = Vector2Subtract(wish, right);
    bool has_wish = Vector2LengthSqr(wish) > 0.0f;
    if (has_wish) wish = Vector2Normalize(wish);

    float wishspeed = PL_SPEED * (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)
                                  ? PL_SPRINT : 1.0f);

    if (IsKeyPressed(KEY_N)) {
        g_pl.noclip = !g_pl.noclip;
        FLOG_INFO(LOGCAT_GAME, "noclip %s", g_pl.noclip ? "on" : "off");
    }

    // --- noclip: free fly, no physics ---
    if (g_pl.noclip) {
        Vector3 dir = { cosf(g_pl.yaw) * cosf(g_pl.pitch), sinf(g_pl.pitch),
                        sinf(g_pl.yaw) * cosf(g_pl.pitch) };
        Vector3 move = { 0 };
        if (IsKeyDown(KEY_W)) move = Vector3Add(move, dir);
        if (IsKeyDown(KEY_S)) move = Vector3Subtract(move, dir);
        if (IsKeyDown(KEY_D)) move = Vector3Add(move, (Vector3){ right.x, 0, right.y });
        if (IsKeyDown(KEY_A)) move = Vector3Subtract(move, (Vector3){ right.x, 0, right.y });
        if (Vector3LengthSqr(move) > 0.0f)
            g_pl.pos = Vector3Add(g_pl.pos, Vector3Scale(Vector3Normalize(move), wishspeed * dt));
        g_pl.vel = (Vector3){ 0 };
        g_pl.sector = WorldSectorAt(MapPosOf(g_pl.pos));
        PROF_END("player");
        return;
    }

    // --- accelerate (the Quake feel lives here) ---
    if (g_pl.on_ground) {
        ApplyFriction(dt);
        if (has_wish) Accelerate(wish, wishspeed, PL_ACCEL, dt);
        if (IsKeyPressed(KEY_SPACE)) {
            g_pl.vel.y = PL_JUMP;
            g_pl.on_ground = false;
        }
    } else {
        if (has_wish) Accelerate(wish, fminf(wishspeed, PL_AIR_CAP), PL_AIR_ACCEL, dt);
        g_pl.vel.y -= PL_GRAVITY * dt;
    }

    // --- horizontal move + collide ---
    g_pl.pos.x += g_pl.vel.x * dt;
    g_pl.pos.z += g_pl.vel.z * dt;
    PROF_BEGIN("collision");
    CollideAndSlide();
    g_pl.sector = WorldSectorAt(MapPosOf(g_pl.pos));
    PROF_END("collision");

    // --- vertical: floor tracking, stepping, falling, ceiling ---
    float floor = FloorUnder(g_pl.sector, g_pl.pos);
    float ceil  = CeilOver(g_pl.sector, g_pl.pos);

    if (g_pl.on_ground) {
        if (floor >= g_pl.pos.y - PL_STEP && floor <= g_pl.pos.y + PL_STEP) {
            // stick to stairs and ramps, up and down; bank the snap so the
            // camera can glide instead of teleporting with the feet
            g_pl.step_smooth += g_pl.pos.y - floor;
            g_pl.pos.y = floor;
            g_pl.vel.y = 0.0f;
        } else if (floor < g_pl.pos.y - PL_STEP) {
            g_pl.on_ground = false;      // walked off a ledge
        }
    } else {
        g_pl.pos.y += g_pl.vel.y * dt;
        if (g_pl.pos.y <= floor) {       // landed
            g_pl.pos.y = floor;
            g_pl.vel.y = 0.0f;
            g_pl.on_ground = true;
        }
    }
    if (g_pl.pos.y + PL_HEIGHT > ceil) { // bumped the ceiling
        g_pl.pos.y = ceil - PL_HEIGHT;
        if (g_pl.vel.y > 0.0f) g_pl.vel.y = 0.0f;
    }

    g_pl.hurt_flash *= expf(-5.0f * dt);

    // eye smoothing: the banked offset decays exponentially, ~100ms feel
    g_pl.step_smooth *= expf(-STEP_SMOOTH_RATE * dt);
    g_pl.step_smooth = Clamp(g_pl.step_smooth, -PL_STEP, PL_STEP);
    if (fabsf(g_pl.step_smooth) < 0.001f) g_pl.step_smooth = 0.0f;

    PROF_END("player");
}

void PlayerApplyCamera(Camera *camera) {
    Vector3 eye = { g_pl.pos.x, g_pl.pos.y + PL_EYE + g_pl.step_smooth, g_pl.pos.z };
    Vector3 dir = { cosf(g_pl.yaw) * cosf(g_pl.pitch), sinf(g_pl.pitch),
                    sinf(g_pl.yaw) * cosf(g_pl.pitch) };
    camera->position = eye;
    camera->target = Vector3Add(eye, dir);
}

float PlayerSpeedXZ(void) {
    return Vector2Length((Vector2){ g_pl.vel.x, g_pl.vel.z });
}

bool PlayerOnGround(void) { return g_pl.on_ground; }

Vector3 PlayerPosition(void) { return g_pl.pos; }

void PlayerDamage(float dmg) {
    if (g_pl.noclip || g_pl.health <= 0.0f) return;
    g_pl.health -= dmg;
    g_pl.hurt_flash = 1.0f;
    FLOG_INFO(LOGCAT_GAME, "player took %.0f damage -> %.0f hp", dmg, g_pl.health);
    if (g_pl.health <= 0.0f) {
        FLOG_INFO(LOGCAT_GAME, "player died, respawning");
        PlayerSpawn(playerStart, playerStartYaw);
        g_pl.hurt_flash = 1.0f;     // carry the death flash through the respawn
    }
}

bool PlayerGiveHealth(float amount) {
    if (amount <= 0.0f || g_pl.health <= 0.0f || g_pl.health >= 100.0f) return false;
    float before = g_pl.health;
    g_pl.health = fminf(100.0f, g_pl.health + amount);
    return g_pl.health != before;
}

float PlayerHealth(void)    { return g_pl.health; }
float PlayerHurtFlash(void) { return g_pl.hurt_flash; }
