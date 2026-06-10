#include "entity/entity.h"
#include "world/world.h"
#include "renderer/sectors.h"
#include "renderer/lights.h"
#include "game/player.h"
#include "game/weapons.h"
#include "config.h"
#include <raymath.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

#define PICKUP_RADIUS      1.55f
#define PICKUP_HEIGHT      1.15f
#define PICKUP_BOB         0.18f
#define PICKUP_SPIN_RATE   1.6f

static bool  g_taken[MAX_ENTITIES];
static float g_time;

void EntitySystemInit(void) {
    memset(g_taken, 0, sizeof g_taken);
    g_time = 0.0f;
}

static int AmmoTypeOf(const Entity *e) {
    return e->data == AMMO_BULLETS ? AMMO_BULLETS : AMMO_SHELLS;
}

static int AmmoAmountOf(const Entity *e) {
    if (e->sx > 0.0f) return (int)(e->sx + 0.5f);
    return AmmoTypeOf(e) == AMMO_SHELLS ? 8 : 40;
}

static int HealthAmountOf(const Entity *e) {
    return e->data > 0 ? e->data : 25;
}

static Vector3 PickupPos(const Entity *e, float bob) {
    int s = WorldSectorAt(e->pos);
    float floor = WorldFloorZAt(s, e->pos);
    return SectorToWorld(e->pos, floor + PICKUP_HEIGHT + e->z + bob);
}

static bool NearPlayer(const Entity *e) {
    Vector3 p = PlayerPosition();
    Vector3 c = PickupPos(e, 0.0f);
    float dx = p.x - c.x, dz = p.z - c.z;
    return dx * dx + dz * dz <= PICKUP_RADIUS * PICKUP_RADIUS &&
           fabsf((p.y + 2.0f) - c.y) <= 4.0f;
}

void EntitySystemUpdate(float dt) {
    g_time += dt;

    for (int i = 0; i < entity_counter; i++) {
        Entity *e = &entities[i];
        if (g_taken[i]) continue;

        bool pickup = e->type == ENT_AMMO || e->type == ENT_HEALTH;
        if (!pickup) continue;

        Vector3 c = PickupPos(e, sinf(g_time * 3.0f + (float)i) * PICKUP_BOB);
        if (e->type == ENT_HEALTH)
            DlightPush(c, (Vector3){ 0.9f, 0.08f, 0.06f }, 4.5f);
        else if (AmmoTypeOf(e) == AMMO_SHELLS)
            DlightPush(c, (Vector3){ 0.9f, 0.55f, 0.10f }, 4.0f);
        else
            DlightPush(c, (Vector3){ 0.15f, 0.35f, 0.9f }, 4.0f);

        if (!NearPlayer(e)) continue;

        bool took = false;
        if (e->type == ENT_HEALTH) {
            took = PlayerGiveHealth((float)HealthAmountOf(e));
        } else {
            took = WeaponsGiveAmmo(AmmoTypeOf(e), AmmoAmountOf(e));
        }
        if (took) g_taken[i] = true;
    }
}

static void DrawHealthPickup(Vector3 c, float spin) {
    Vector3 wide = { 0.82f, 0.28f, 0.28f };
    Vector3 tall = { 0.28f, 0.82f, 0.28f };
    DrawCubeV(c, wide, (Color){ 220, 28, 24, 255 });
    DrawCubeV(c, tall, (Color){ 220, 28, 24, 255 });
    DrawCubeWiresV(c, (Vector3){ 0.86f, 0.86f, 0.32f }, Fade(RAYWHITE, 0.8f));
    DrawCircle3D(Vector3Add(c, (Vector3){ 0, 0.02f, 0 }), 0.7f,
                 (Vector3){ cosf(spin), 0.0f, sinf(spin) }, 90.0f,
                 Fade((Color){ 255, 80, 60, 255 }, 0.7f));
}

static void DrawAmmoPickup(const Entity *e, Vector3 c, float spin) {
    bool shells = AmmoTypeOf(e) == AMMO_SHELLS;
    Color fill = shells ? (Color){ 224, 148, 44, 255 } : (Color){ 54, 104, 220, 255 };
    Color edge = shells ? (Color){ 255, 226, 130, 255 } : (Color){ 150, 205, 255, 255 };
    Vector3 size = shells ? (Vector3){ 0.78f, 0.34f, 0.54f }
                          : (Vector3){ 0.46f, 0.72f, 0.46f };
    DrawCubeV(c, size, fill);
    DrawCubeWiresV(c, Vector3Scale(size, 1.08f), edge);
    DrawCircle3D(Vector3Add(c, (Vector3){ 0, -0.02f, 0 }), 0.55f,
                 (Vector3){ cosf(spin), 0.0f, sinf(spin) }, 90.0f, Fade(edge, 0.75f));
}

void EntitySystemDraw3D(Camera camera) {
    (void)camera;
    for (int i = 0; i < entity_counter; i++) {
        const Entity *e = &entities[i];
        if (g_taken[i] || (e->type != ENT_AMMO && e->type != ENT_HEALTH)) continue;
        float spin = g_time * PICKUP_SPIN_RATE + (float)i;
        Vector3 c = PickupPos(e, sinf(g_time * 3.0f + (float)i) * PICKUP_BOB);
        if (e->type == ENT_HEALTH) DrawHealthPickup(c, spin);
        else                       DrawAmmoPickup(e, c, spin);
    }
}

int EntityPickupsRemaining(void) {
    int n = 0;
    for (int i = 0; i < entity_counter; i++)
        if (!g_taken[i] && (entities[i].type == ENT_AMMO || entities[i].type == ENT_HEALTH))
            n++;
    return n;
}
