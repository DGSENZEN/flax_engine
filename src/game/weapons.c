#include "game/weapons.h"
#include "game/player.h"
#include "game/enemies.h"
#include "world/world.h"
#include "world/raycast.h"
#include "renderer/decals.h"
#include "renderer/textures.h"
#include "renderer/lights.h"
#include "renderer/billboard.h"
#include "core/log.h"
#include "config.h"
#include <raymath.h>
#include <string.h>
#include <stdio.h>

// ===========================================================================
// Weapons. Three test guns over the same primitives:
//
//   1 PITCHFORK  melee ray, quiet
//   2 SAWED-OFF  7-pellet spread, slow, loud, reload animation
//   3 TOMMY GUN  fast single hitscan, auto
//   RMB          glowing bolt projectile (any weapon, test projectile)
//
// HUD anchoring: GunLayout() computes the gun's screen rect and a screen-
// space muzzle anchor ON the sprite each frame; the flash draws at that
// anchor, and the 3D muzzle (tracer/bolt origin, dlight) is that anchor
// UNPROJECTED into the world. The gun owns the muzzle - the flash cannot
// float off it, and the shots leave from where the flash is.
// ===========================================================================

// --- sheet rects (assets/sprites/weapons.png, measured) ---------------------
static const Rectangle TOMMY[6] = {
    {   0, 420, 129, 110 }, { 131, 420, 129, 110 }, { 262, 420, 129, 110 },
    { 393, 418, 133, 112 }, { 528, 418, 133, 112 }, { 663, 419, 128, 111 },
};
#define TOMMY_FIRE_A 3
#define TOMMY_FIRE_B 4
static const Rectangle TOMMY_FLASH[2] = {
    { 793, 419, 68, 57 }, { 865, 419, 65, 55 },
};

static const Rectangle SG_IDLE  = {  71, 227, 155,  96 };
static const Rectangle SG_OPEN  = { 228, 248, 129,  75 };   // barrels broken open
static const Rectangle SG_LOAD  = { 359, 233, 118,  90 };   // shells going in
static const Rectangle SG_CLOSE = { 479, 224, 157,  99 };   // snapping shut
static const Rectangle SG_FLASH[2] = {
    { 75, 325, 71, 62 }, { 163, 325, 48, 49 },
};

static const Rectangle FORK[3] = {                          // raise sequence
    { 242, 42, 74, 104 }, { 318, 26, 69, 120 }, { 389, 12, 73, 134 },
};

// --- tuning ------------------------------------------------------------------
#define HITSCAN_RANGE   600.0f
#define MUZZLE_DEPTH    1.6f     // world distance of the unprojected muzzle

#define TG_RATE         0.12f
#define TG_SPREAD       0.012f
#define TG_DMG          14.0f
#define TG_START_AMMO   90
#define TG_MAX_AMMO     240

#define SG_RATE         1.1f
#define SG_PELLETS      7
#define SG_SPREAD       0.05f
#define SG_DMG          9.0f
#define SG_AMMO_PER     2
#define SG_START_AMMO   18
#define SG_MAX_AMMO     64

#define PF_RATE         0.55f
#define PF_RANGE        4.5f
#define PF_DMG          26.0f

#define HOLE_SIZE       0.45f
#define BOLT_RATE       0.35f
#define BOLT_SPEED      70.0f
#define BOLT_TTL        6.0f
#define BOLT_HOLE_SIZE  1.0f
#define DRY_RATE        0.16f

#define MAX_BOLTS    32
#define MAX_PUFFS    64
#define MAX_TRACERS  32

typedef enum { WPN_PITCHFORK = 0, WPN_SHOTGUN, WPN_TOMMY } WeaponId;

typedef struct { Vector3 pos, vel; float ttl; bool active; } Bolt;
typedef struct { Vector3 pos; float age, ttl, size; Color tint; bool active; } Puff;
typedef struct { Vector3 a, b; float age, ttl; bool active; } Tracer;

static Bolt   g_bolts[MAX_BOLTS];
static Puff   g_puffs[MAX_PUFFS];
static Tracer g_tracers[MAX_TRACERS];
static int    g_bolt_head, g_puff_head, g_tracer_head;

static int   g_weapon = WPN_TOMMY;
static float g_cooldown, g_bolt_cooldown;
static float g_flash;               // action envelope: muzzle flash / fork thrust
static bool  g_fire_alt;
static int   g_ammo[AMMO_TYPE_COUNT];
static int   g_tex_hole, g_tex_puff;
static Texture2D g_sheet;
static Vector3   g_muzzle;          // world-space shot origin, set per frame

void WeaponsInit(void) {
    memset(g_bolts,   0, sizeof(g_bolts));
    memset(g_puffs,   0, sizeof(g_puffs));
    memset(g_tracers, 0, sizeof(g_tracers));
    g_bolt_head = g_puff_head = g_tracer_head = 0;
    g_cooldown = g_bolt_cooldown = g_flash = 0.0f;
    g_ammo[AMMO_SHELLS] = SG_START_AMMO;
    g_ammo[AMMO_BULLETS] = TG_START_AMMO;
    g_tex_hole = TexRegistryFind("BULLETHOLE");
    g_tex_puff = TexRegistryFind("PUFF");
    if (g_tex_hole < 0 || g_tex_puff < 0)
        FLOG_WARN(LOGCAT_GAME, "weapons: missing effect textures (hole=%d puff=%d)",
                  g_tex_hole, g_tex_puff);

    if (g_sheet.id == 0) {
        g_sheet = LoadTexture(FLAX_ASSET_DIR "/sprites/weapons.png");
        if (g_sheet.id != 0) SetTextureFilter(g_sheet, TEXTURE_FILTER_POINT);
        else FLOG_WARN(LOGCAT_GAME, "weapon sheet missing: assets/sprites/weapons.png");
    }
}

static int AmmoMax(int ammo_type) {
    switch (ammo_type) {
    case AMMO_SHELLS:  return SG_MAX_AMMO;
    case AMMO_BULLETS: return TG_MAX_AMMO;
    default:           return 0;
    }
}

bool WeaponsGiveAmmo(int ammo_type, int amount) {
    if (ammo_type < 0 || ammo_type >= AMMO_TYPE_COUNT || amount <= 0) return false;
    int before = g_ammo[ammo_type];
    g_ammo[ammo_type] = (int)Clamp((float)(before + amount), 0.0f, (float)AmmoMax(ammo_type));
    return g_ammo[ammo_type] != before;
}

int WeaponsAmmo(int ammo_type) {
    return (ammo_type >= 0 && ammo_type < AMMO_TYPE_COUNT) ? g_ammo[ammo_type] : 0;
}

int WeaponsAmmoMax(int ammo_type) {
    return AmmoMax(ammo_type);
}

const char *WeaponAmmoText(void) {
    static char buf[32];
    switch (g_weapon) {
    case WPN_SHOTGUN:
        snprintf(buf, sizeof buf, "%02d/%02d SHELLS", g_ammo[AMMO_SHELLS], SG_MAX_AMMO);
        break;
    case WPN_TOMMY:
        snprintf(buf, sizeof buf, "%03d/%03d BULLETS", g_ammo[AMMO_BULLETS], TG_MAX_AMMO);
        break;
    default:
        snprintf(buf, sizeof buf, "MELEE");
        break;
    }
    return buf;
}

static bool TakeAmmo(int ammo_type, int amount) {
    if (amount <= 0) return true;
    if (ammo_type < 0 || ammo_type >= AMMO_TYPE_COUNT || g_ammo[ammo_type] < amount) {
        g_cooldown = DRY_RATE;
        return false;
    }
    g_ammo[ammo_type] -= amount;
    return true;
}

const char *WeaponName(void) {
    switch (g_weapon) {
    case WPN_PITCHFORK: return "PITCHFORK";
    case WPN_SHOTGUN:   return "SAWED-OFF";
    default:            return "TOMMY GUN";
    }
}

// --- effects -----------------------------------------------------------------

static void SpawnPuff(Vector3 pos, float ttl, float size, Color tint) {
    g_puffs[g_puff_head] = (Puff){ .pos = pos, .ttl = ttl, .size = size,
                                   .tint = tint, .active = true };
    g_puff_head = (g_puff_head + 1) % MAX_PUFFS;
}

static void SpawnTracer(Vector3 a, Vector3 b, float ttl) {
    g_tracers[g_tracer_head] = (Tracer){ .a = a, .b = b, .ttl = ttl, .active = true };
    g_tracer_head = (g_tracer_head + 1) % MAX_TRACERS;
}

static void Impact(const RayHit *hit, float hole_size, float puff_size) {
    DecalSpawn(hit->pos, hit->normal, hole_size, g_tex_hole);
    SpawnPuff(Vector3Add(hit->pos, Vector3Scale(hit->normal, 0.12f)), 0.22f, puff_size,
              (Color){ 230, 220, 200, 255 });
    DlightSpawn(Vector3Add(hit->pos, Vector3Scale(hit->normal, 0.6f)),
                (Vector3){ 1.0f, 0.7f, 0.3f }, 5.0f * puff_size, 0.15f);
}

static void BloodBurst(Vector3 pos, bool headshot) {
    SpawnPuff(pos, 0.3f, headshot ? 0.9f : 0.6f, (Color){ 165, 15, 15, 255 });
}

static float Spread(float amount) {
    return (float)GetRandomValue(-1000, 1000) / 1000.0f * amount;
}

// One hitscan pellet: enemies soak it before the wall does. Returns the
// visual endpoint for the tracer.
static Vector3 FirePellet(Vector3 eye, Vector3 aim, float dmg, float hole) {
    RayHit hit;
    bool wall = WorldRaycast(eye, aim, HITSCAN_RANGE, &hit);
    float reach = wall ? hit.dist : HITSCAN_RANGE;

    float edist;
    bool headshot = false;
    int ei = EnemiesRayHit(eye, aim, reach, &edist, &headshot);
    if (ei >= 0) {
        Vector3 end = Vector3Add(eye, Vector3Scale(aim, edist));
        BloodBurst(end, headshot);
        EnemyDamage(ei, dmg + (float)GetRandomValue(0, 6), headshot);
        return end;
    }
    if (wall) {
        Impact(&hit, hole, 0.5f);
        return hit.pos;
    }
    return Vector3Add(eye, Vector3Scale(aim, HITSCAN_RANGE));
}

// --- HUD layout (shared by update and draw, so they can't disagree) ----------

typedef struct {
    Rectangle frame;            // sheet rect
    Rectangle dest;             // screen rect
    Vector2   flash;            // screen muzzle anchor (also unprojected to 3D)
    const Rectangle *flashSpr;  // NULL: no flash this frame
    float     flashScale;
} HudLayout;

static HudLayout GunLayout(void) {
    HudLayout L = { 0 };
    Vector2 bob = { 0 };
    if (PlayerOnGround() && PlayerSpeedXZ() > 1.0f) {
        bob.x = sinf((float)GetTime() * 10.0f) * 5.0f;
        bob.y = fabsf(cosf((float)GetTime() * 10.0f)) * 7.0f;
    }
    bool firing = g_flash > 0.45f;

    switch (g_weapon) {
    case WPN_TOMMY: {
        const Rectangle *f = firing ? (g_fire_alt ? &TOMMY[TOMMY_FIRE_A]
                                                  : &TOMMY[TOMMY_FIRE_B])
                                    : &TOMMY[0];
        float s = 4.0f, w = f->width * s, h = f->height * s;
        float x = (SCREEN_WIDTH - w) * 0.5f + bob.x;
        float y = SCREEN_HEIGHT - h + bob.y + g_flash * 14.0f + 8.0f;
        L.frame = *f;
        L.dest = (Rectangle){ x, y, w, h };
        float fx = firing ? 0.48f : 0.46f;
        L.flash = (Vector2){ x + w * fx, y + h * 0.03f };
        if (firing) {
            L.flashSpr = g_fire_alt ? &TOMMY_FLASH[0] : &TOMMY_FLASH[1];
            L.flashScale = s * 0.78f;
        }
        break;
    }
    case WPN_SHOTGUN: {
        // cooldown doubles as the reload animation clock
        const Rectangle *f = &SG_IDLE;
        if (g_cooldown > 0.0f) {
            float phase = 1.0f - g_cooldown / SG_RATE;     // 0 fired -> 1 ready
            if      (phase < 0.30f) f = &SG_IDLE;          // recoil + flash
            else if (phase < 0.55f) f = &SG_OPEN;
            else if (phase < 0.80f) f = &SG_LOAD;
            else                    f = &SG_CLOSE;
        }
        float s = 4.0f, w = f->width * s, h = f->height * s;
        float x = (SCREEN_WIDTH - w) * 0.5f + 64.0f + bob.x;
        float y = SCREEN_HEIGHT - h + bob.y + g_flash * 22.0f + 14.0f;
        L.frame = *f;
        L.dest = (Rectangle){ x, y, w, h };
        L.flash = (Vector2){ x + w * 0.14f, y + h * 0.075f };  // barrel tips
        if (firing) {
            L.flashSpr = g_fire_alt ? &SG_FLASH[0] : &SG_FLASH[1];
            L.flashScale = s * 0.88f;
        }
        break;
    }
    default: {   // WPN_PITCHFORK: tucked low, lunges up on the stab
        const Rectangle *f = g_flash > 0.55f ? &FORK[2]
                           : g_flash > 0.25f ? &FORK[1] : &FORK[0];
        float s = 3.8f, w = f->width * s, h = f->height * s;
        float x = SCREEN_WIDTH * 0.5f - w * 0.5f + 42.0f + bob.x;
        float y = SCREEN_HEIGHT - h + 24.0f + bob.y - g_flash * 95.0f;
        L.frame = *f;
        L.dest = (Rectangle){ x, y, w, h };
        L.flash = (Vector2){ x + w * 0.5f, y };      // tine tips: stab origin
        break;
    }
    }
    return L;
}

// --- tick ---------------------------------------------------------------------

void WeaponsUpdate(float dt, Camera camera) {
    Vector3 eye = camera.position;
    Vector3 dir = Vector3Normalize(Vector3Subtract(camera.target, camera.position));

    g_flash = fmaxf(0.0f, g_flash - dt * 10.0f);
    g_cooldown -= dt;
    g_bolt_cooldown -= dt;

    // weapon select; tiny raise delay so it isn't a free reload skip
    int want = -1;
    if (IsKeyPressed(KEY_ONE))   want = WPN_PITCHFORK;
    if (IsKeyPressed(KEY_TWO))   want = WPN_SHOTGUN;
    if (IsKeyPressed(KEY_THREE)) want = WPN_TOMMY;
    if (want >= 0 && want != g_weapon) {
        g_weapon = want;
        g_cooldown = 0.35f;
        g_flash = 0.0f;
        FLOG_INFO(LOGCAT_GAME, "weapon: %s", WeaponName());
    }

    Vector3 right = Vector3Normalize(Vector3CrossProduct(dir, (Vector3){ 0, 1, 0 }));
    Vector3 up    = Vector3CrossProduct(right, dir);

    // the 3D muzzle IS the gun sprite's flash anchor, pushed into the world
    Ray mr = GetScreenToWorldRay(GunLayout().flash, camera);
    g_muzzle = Vector3Add(mr.position, Vector3Scale(mr.direction, MUZZLE_DEPTH));

    if (g_flash > 0.0f && g_weapon != WPN_PITCHFORK)
        DlightPush(g_muzzle,
                   (Vector3){ 1.0f * g_flash, 0.85f * g_flash, 0.45f * g_flash }, 14.0f);

    // --- fire ---
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && g_cooldown <= 0.0f) {
        bool fired = false;

        switch (g_weapon) {
        case WPN_TOMMY: {
            if (!TakeAmmo(AMMO_BULLETS, 1)) break;
            fired = true;
            g_cooldown = TG_RATE;
            EnemiesAlertAt(eye, 45.0f);
            Vector3 aim = Vector3Normalize(Vector3Add(dir,
                Vector3Add(Vector3Scale(right, Spread(TG_SPREAD)),
                           Vector3Scale(up, Spread(TG_SPREAD)))));
            SpawnTracer(g_muzzle, FirePellet(eye, aim, TG_DMG, HOLE_SIZE), 0.07f);
            break;
        }
        case WPN_SHOTGUN: {
            if (!TakeAmmo(AMMO_SHELLS, SG_AMMO_PER)) break;
            fired = true;
            g_cooldown = SG_RATE;
            EnemiesAlertAt(eye, 55.0f);
            for (int p = 0; p < SG_PELLETS; p++) {
                Vector3 aim = Vector3Normalize(Vector3Add(dir,
                    Vector3Add(Vector3Scale(right, Spread(SG_SPREAD)),
                               Vector3Scale(up, Spread(SG_SPREAD)))));
                SpawnTracer(g_muzzle, FirePellet(eye, aim, SG_DMG, 0.3f), 0.055f);
            }
            break;
        }
        case WPN_PITCHFORK: {
            fired = true;
            g_cooldown = PF_RATE;
            EnemiesAlertAt(eye, 12.0f);          // stabbing is quiet
            float edist;
            bool headshot = false;
            int ei = EnemiesRayHit(eye, dir, PF_RANGE, &edist, &headshot);
            if (ei >= 0) {
                BloodBurst(Vector3Add(eye, Vector3Scale(dir, edist)), headshot);
                EnemyDamage(ei, PF_DMG + (float)GetRandomValue(0, 8), headshot);
            } else {
                RayHit hit;
                if (WorldRaycast(eye, dir, PF_RANGE, &hit)) {
                    DecalSpawn(hit.pos, hit.normal, 0.25f, g_tex_hole);
                    SpawnPuff(Vector3Add(hit.pos, Vector3Scale(hit.normal, 0.1f)),
                              0.18f, 0.35f, (Color){ 230, 220, 200, 255 });
                }
            }
            break;
        }
        }

        if (fired) {
            g_flash = 1.0f;
            g_fire_alt = !g_fire_alt;
        }
    }

    // --- projectile bolt (any weapon, RMB) ---
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && g_bolt_cooldown <= 0.0f) {
        g_bolt_cooldown = BOLT_RATE;
        g_flash = 1.0f;
        EnemiesAlertAt(eye, 45.0f);
        g_bolts[g_bolt_head] = (Bolt){
            .pos = g_muzzle,
            .vel = Vector3Scale(dir, BOLT_SPEED),
            .ttl = BOLT_TTL, .active = true
        };
        g_bolt_head = (g_bolt_head + 1) % MAX_BOLTS;
    }

    // --- simulate bolts: one segment raycast per tick ---
    for (int i = 0; i < MAX_BOLTS; i++) {
        Bolt *b = &g_bolts[i];
        if (!b->active) continue;
        b->ttl -= dt;
        if (b->ttl <= 0.0f) { b->active = false; continue; }

        float step = Vector3Length(b->vel) * dt;
        Vector3 bdir = Vector3Normalize(b->vel);

        float edist;
        bool headshot = false;
        int ei = EnemiesRayHit(b->pos, bdir, step, &edist, &headshot);
        if (ei >= 0) {
            Vector3 at = Vector3Add(b->pos, Vector3Scale(bdir, edist));
            BloodBurst(at, headshot);
            EnemyDamage(ei, 40.0f, headshot);
            DlightSpawn(at, (Vector3){ 1.0f, 0.6f, 0.25f }, 9.0f, 0.18f);
            b->active = false;
            continue;
        }

        RayHit hit;
        if (WorldRaycast(b->pos, b->vel, step, &hit)) {
            Impact(&hit, BOLT_HOLE_SIZE, 1.4f);
            b->active = false;
        } else {
            b->pos = Vector3Add(b->pos, Vector3Scale(b->vel, dt));
            DlightPush(b->pos, (Vector3){ 1.0f, 0.55f, 0.2f }, 10.0f);   // glow in flight
        }
    }

    for (int i = 0; i < MAX_PUFFS; i++)
        if (g_puffs[i].active && (g_puffs[i].age += dt) >= g_puffs[i].ttl)
            g_puffs[i].active = false;
    for (int i = 0; i < MAX_TRACERS; i++)
        if (g_tracers[i].active && (g_tracers[i].age += dt) >= g_tracers[i].ttl)
            g_tracers[i].active = false;
}

void WeaponsDraw3D(Camera camera) {
    for (int i = 0; i < MAX_TRACERS; i++) {
        const Tracer *t = &g_tracers[i];
        if (!t->active) continue;
        float a = 1.0f - t->age / t->ttl;
        DrawLine3D(t->a, t->b, Fade((Color){ 255, 232, 140, 255 }, a));
    }

    Texture2D puffTex = TexRegistryTexture(g_tex_puff);
    Rectangle full = { 0, 0, (float)puffTex.width, (float)puffTex.height };

    for (int i = 0; i < MAX_BOLTS; i++)
        if (g_bolts[i].active)
            DrawSpriteBillboard(camera, puffTex, full, g_bolts[i].pos,
                                (Vector2){ 0.7f, 0.7f }, false,
                                (Color){ 255, 170, 60, 255 });

    for (int i = 0; i < MAX_PUFFS; i++) {
        const Puff *p = &g_puffs[i];
        if (!p->active) continue;
        float k = p->age / p->ttl;                       // 0 -> 1 over life
        float size = p->size * (1.0f + 1.8f * k);        // expand
        DrawSpriteBillboard(camera, puffTex, full, p->pos,
                            (Vector2){ size, size }, false, Fade(p->tint, 1.0f - k));
    }
}

void WeaponsDrawHUD(void) {
    if (g_sheet.id == 0) return;
    HudLayout L = GunLayout();

    if (L.flashSpr) {
        DrawCircleGradient((int)L.flash.x, (int)L.flash.y, 55.0f + 40.0f * g_flash,
                           Fade((Color){ 255, 220, 120, 255 }, 0.45f * g_flash),
                           Fade((Color){ 255, 140, 40, 255 }, 0.0f));
        float fw = L.flashSpr->width * L.flashScale;
        float fh = L.flashSpr->height * L.flashScale;
        DrawTexturePro(g_sheet, *L.flashSpr,
                       (Rectangle){ L.flash.x - fw * 0.5f, L.flash.y - fh * 0.6f, fw, fh },
                       (Vector2){ 0, 0 }, 0.0f, WHITE);
    }

    DrawTexturePro(g_sheet, L.frame, L.dest, (Vector2){ 0, 0 }, 0.0f, WHITE);
}
