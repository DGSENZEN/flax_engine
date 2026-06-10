#include "renderer/lights.h"
#include <string.h>

typedef struct { Vector3 pos, color; float radius; } FrameLight;
typedef struct { Vector3 pos, color; float radius, ttl, ttl0; bool active; } TimedLight;

#define MAX_TIMED 16

static FrameLight g_frame[MAX_DLIGHTS];
static int        g_frame_count;
static TimedLight g_timed[MAX_TIMED];
static int        g_timed_head;

void LightsInit(void) {
    memset(g_frame, 0, sizeof(g_frame));
    memset(g_timed, 0, sizeof(g_timed));
    g_frame_count = 0;
    g_timed_head = 0;
}

void LightsBeginFrame(float dt) {
    g_frame_count = 0;
    for (int i = 0; i < MAX_TIMED; i++) {
        if (!g_timed[i].active) continue;
        g_timed[i].ttl -= dt;
        if (g_timed[i].ttl <= 0.0f) g_timed[i].active = false;
    }
}

void DlightPush(Vector3 pos, Vector3 color, float radius) {
    if (g_frame_count >= MAX_DLIGHTS || radius <= 0.0f) return;
    g_frame[g_frame_count++] = (FrameLight){ pos, color, radius };
}

void DlightSpawn(Vector3 pos, Vector3 color, float radius, float ttl) {
    if (ttl <= 0.0f) return;
    g_timed[g_timed_head] = (TimedLight){ pos, color, radius, ttl, ttl, true };
    g_timed_head = (g_timed_head + 1) % MAX_TIMED;
}

int LightsGather(Vector3 *pos, Vector3 *color, float *radius, int max) {
    int n = 0;
    for (int i = 0; i < g_frame_count && n < max; i++, n++) {
        pos[n] = g_frame[i].pos;
        color[n] = g_frame[i].color;
        radius[n] = g_frame[i].radius;
    }
    for (int i = 0; i < MAX_TIMED && n < max; i++) {
        if (!g_timed[i].active) continue;
        float k = g_timed[i].ttl / g_timed[i].ttl0;     // fade with remaining life
        pos[n] = g_timed[i].pos;
        color[n] = (Vector3){ g_timed[i].color.x * k, g_timed[i].color.y * k,
                              g_timed[i].color.z * k };
        radius[n] = g_timed[i].radius;
        n++;
    }
    return n;
}
