#include "core/profiler.h"
#include "renderer/sectors.h"
#include "raylib.h"
#include <raymath.h>
#include <string.h>

#define MAX_ZONES   24
#define HISTORY_LEN 240          // 4 seconds of history at 60 fps
#define SMOOTH      0.05f        // EMA factor for the per-zone readout

typedef struct {
    const char *name;
    double start;                // GetTime() at ProfBegin
    double acc_ms;               // accumulated this frame
    int    calls;
    float  smooth_ms;            // exponential moving average across frames
} Zone;

static Zone   g_zones[MAX_ZONES];
static int    g_zone_count = 0;

static float  g_history[HISTORY_LEN];   // wall frame ms ring buffer
static int    g_history_head = 0;
static double g_last_frame_start = 0.0;
static float  g_frame_ms = 0.0f;        // wall time of the previous frame
static float  g_logic_ms = 0.0f;        // sum of zone time of the previous frame

static bool   g_visible = false;
static int    g_fps_cap = 60;

static Zone *FindZone(const char *name) {
    for (int i = 0; i < g_zone_count; i++)
        if (g_zones[i].name == name || strcmp(g_zones[i].name, name) == 0)
            return &g_zones[i];
    if (g_zone_count >= MAX_ZONES) return NULL;
    g_zones[g_zone_count] = (Zone){ .name = name };
    return &g_zones[g_zone_count++];
}

void ProfFrameStart(void) {
    double now = GetTime();
    if (g_last_frame_start > 0.0) {
        g_frame_ms = (float)((now - g_last_frame_start) * 1000.0);
        g_history[g_history_head] = g_frame_ms;
        g_history_head = (g_history_head + 1) % HISTORY_LEN;
    }
    g_last_frame_start = now;

    g_logic_ms = 0.0f;
    for (int i = 0; i < g_zone_count; i++) {
        Zone *z = &g_zones[i];
        g_logic_ms += (float)z->acc_ms;
        z->smooth_ms = Lerp(z->smooth_ms, (float)z->acc_ms, SMOOTH);
        z->acc_ms = 0.0;
        z->calls = 0;
    }
}

void ProfBegin(const char *zone) {
    Zone *z = FindZone(zone);
    if (z) z->start = GetTime();
}

void ProfEnd(const char *zone) {
    Zone *z = FindZone(zone);
    if (z && z->start > 0.0) {
        z->acc_ms += (GetTime() - z->start) * 1000.0;
        z->calls++;
        z->start = 0.0;
    }
}

void ProfToggle(void)  { g_visible = !g_visible; }
bool ProfVisible(void) { return g_visible; }

int ProfCycleFpsCap(void) {
    g_fps_cap = (g_fps_cap == 60) ? 0 : 60;   // 60 <-> uncapped
    return g_fps_cap;
}

void ProfDraw(void) {
    if (!g_visible) return;

    const int X = 10, Y = 90, W = 380, GRAPH_H = 64, ROW = 18;
    int rows = g_zone_count + 4;
    int H = 8 + 20 + GRAPH_H + 8 + rows * ROW + 8;

    DrawRectangle(X - 4, Y - 4, W + 8, H + 8, (Color){ 0, 0, 0, 190 });
    DrawRectangleLines(X - 4, Y - 4, W + 8, H + 8, (Color){ 90, 150, 200, 255 });

    // headline: fps + frame vs logic time = the CPU/GPU-bound verdict
    float avg = 0, peak = 0;
    for (int i = 0; i < HISTORY_LEN; i++) {
        avg += g_history[i];
        if (g_history[i] > peak) peak = g_history[i];
    }
    avg /= HISTORY_LEN;
    DrawText(TextFormat("%d FPS%s   frame %5.2fms   logic %5.2fms",
             GetFPS(), g_fps_cap == 0 ? " (uncapped)" : "", g_frame_ms, g_logic_ms),
             X, Y, 18, (Color){ 190, 255, 210, 255 });

    // frame-time graph; the 16.7ms line is the 60fps budget
    int gy = Y + 24;
    DrawRectangle(X, gy, W, GRAPH_H, (Color){ 10, 14, 18, 255 });
    float scale = (float)GRAPH_H / (peak > 20.0f ? peak : 20.0f);
    int budgetY = gy + GRAPH_H - (int)(16.7f * scale);
    DrawLine(X, budgetY, X + W, budgetY, (Color){ 200, 70, 70, 200 });
    for (int i = 0; i < HISTORY_LEN; i++) {
        float ms = g_history[(g_history_head + i) % HISTORY_LEN];
        int h = (int)(ms * scale);
        if (h > GRAPH_H) h = GRAPH_H;
        int px = X + i * W / HISTORY_LEN;
        DrawLine(px, gy + GRAPH_H - h, px, gy + GRAPH_H,
                 ms > 16.7f ? (Color){ 220, 120, 80, 255 } : (Color){ 90, 170, 120, 255 });
    }

    // zone table with budget bars
    int ty = gy + GRAPH_H + 8;
    DrawText(TextFormat("avg %5.2fms   peak %5.2fms", avg, peak), X, ty, 16, (Color){ 150, 220, 180, 255 });
    ty += ROW;
    for (int i = 0; i < g_zone_count; i++) {
        Zone *z = &g_zones[i];
        int bar = (int)(z->smooth_ms / 16.7f * (W - 190));
        if (bar > W - 190) bar = W - 190;
        DrawRectangle(X + 180, ty + 3, bar, ROW - 7, (Color){ 90, 150, 200, 160 });
        DrawText(TextFormat("%-14s %6.2fms", z->name, z->smooth_ms), X, ty, 16,
                 (Color){ 150, 220, 180, 255 });
        ty += ROW;
    }

    int meshes, tris;
    SectorWorldStats(&meshes, &tris);
    DrawText(TextFormat("world: %d meshes  %d tris", meshes, tris), X, ty, 16,
             (Color){ 110, 150, 128, 255 });
    ty += ROW;
    DrawText("F1 hide   F2 toggle fps cap", X, ty, 16, (Color){ 110, 150, 128, 255 });
}
