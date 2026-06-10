#include "renderer/decals.h"
#include "renderer/textures.h"
#include "rlgl.h"
#include <raymath.h>
#include <math.h>

#define MAX_DECALS 256

// Basis vectors are baked at spawn (already scaled to the half-size), so
// drawing is four rlVertex calls per decal with zero per-frame math.
typedef struct {
    Vector3 pos;
    Vector3 t, b;     // half-extent tangent / bitangent in the surface plane
    int     tex;      // texture registry index
    bool    active;
} Decal;

static Decal g_decals[MAX_DECALS];
static int   g_head = 0;

void DecalsClear(void) {
    for (int i = 0; i < MAX_DECALS; i++) g_decals[i].active = false;
    g_head = 0;
}

void DecalSpawn(Vector3 pos, Vector3 normal, float size, int registry_tex) {
    Vector3 up = fabsf(normal.y) < 0.99f ? (Vector3){ 0, 1, 0 }
                                         : (Vector3){ 1, 0, 0 };
    Vector3 tan = Vector3Normalize(Vector3CrossProduct(up, normal));
    Vector3 bit = Vector3CrossProduct(normal, tan);

    float spin = (float)GetRandomValue(0, 628) / 100.0f;
    tan = Vector3RotateByAxisAngle(tan, normal, spin);
    bit = Vector3RotateByAxisAngle(bit, normal, spin);

    // Stagger the lift-off per slot so overlapping decals don't z-fight
    // each other on the same plane.
    float lift = 0.015f + (float)(g_head % 8) * 0.002f;

    Decal *d = &g_decals[g_head];
    g_head = (g_head + 1) % MAX_DECALS;

    d->pos    = Vector3Add(pos, Vector3Scale(normal, lift));
    d->t      = Vector3Scale(tan, size * 0.5f);
    d->b      = Vector3Scale(bit, size * 0.5f);
    d->tex    = registry_tex;
    d->active = true;
}

void DecalsDraw(void) {
    rlDisableBackfaceCulling();   // glued flat on walls; winding varies with spin
    rlDisableDepthMask();         // the wall behind already owns the depth
    for (int i = 0; i < MAX_DECALS; i++) {
        const Decal *d = &g_decals[i];
        if (!d->active) continue;

        rlSetTexture(TexRegistryTexture(d->tex).id);
        rlBegin(RL_QUADS);
        rlColor4ub(255, 255, 255, 255);
        rlTexCoord2f(0, 0);
        rlVertex3f(d->pos.x - d->t.x - d->b.x, d->pos.y - d->t.y - d->b.y, d->pos.z - d->t.z - d->b.z);
        rlTexCoord2f(1, 0);
        rlVertex3f(d->pos.x + d->t.x - d->b.x, d->pos.y + d->t.y - d->b.y, d->pos.z + d->t.z - d->b.z);
        rlTexCoord2f(1, 1);
        rlVertex3f(d->pos.x + d->t.x + d->b.x, d->pos.y + d->t.y + d->b.y, d->pos.z + d->t.z + d->b.z);
        rlTexCoord2f(0, 1);
        rlVertex3f(d->pos.x - d->t.x + d->b.x, d->pos.y - d->t.y + d->b.y, d->pos.z - d->t.z + d->b.z);
        rlEnd();
    }
    rlSetTexture(0);
    rlEnableDepthMask();
    rlEnableBackfaceCulling();
}
