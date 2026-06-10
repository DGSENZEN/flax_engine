#include "core/log.h"
#include "raylib.h"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static FILE        *g_file = NULL;
static FlaxLogLevel g_min_level = FLOG_LVL_DEBUG;
static double       g_boot_time = 0.0;

static const char *LEVEL_TAG[] = { "DEBUG", "INFO ", "WARN ", "ERROR" };
static const char *CAT_TAG[LOGCAT_COUNT] = { "CORE  ", "IO    ", "RENDER", "EDITOR", "GAME  " };

// Monotonic seconds since FlaxLogInit. raylib's GetTime() needs a window, so
// we keep our own clock and work before InitWindow too.
static double NowSeconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void EmitV(FlaxLogLevel level, LogCategory cat, const char *fmt, va_list args) {
    if (level < g_min_level) return;
    if (cat < 0 || cat >= LOGCAT_COUNT) cat = LOGCAT_CORE;

    char msg[1024];
    vsnprintf(msg, sizeof msg, fmt, args);

    double t = NowSeconds() - g_boot_time;
    fprintf(stderr, "[%8.3f] [%s] [%s] %s\n", t, LEVEL_TAG[level], CAT_TAG[cat], msg);
    if (g_file) {
        fprintf(g_file, "[%8.3f] [%s] [%s] %s\n", t, LEVEL_TAG[level], CAT_TAG[cat], msg);
        fflush(g_file);   // per-line flush: a crash never loses the last words
    }
}

void FlaxLog(FlaxLogLevel level, LogCategory cat, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    EmitV(level, cat, fmt, args);
    va_end(args);
}

// raylib TraceLog -> our funnel, under LOGCAT_CORE.
static void RaylibLogAdapter(int logLevel, const char *text, va_list args) {
    FlaxLogLevel lvl = (logLevel >= LOG_ERROR) ? FLOG_LVL_ERROR
                     : (logLevel == LOG_WARNING) ? FLOG_LVL_WARN
                     : (logLevel == LOG_INFO) ? FLOG_LVL_INFO : FLOG_LVL_DEBUG;
    EmitV(lvl, LOGCAT_CORE, text, args);
}

void FlaxLogInit(const char *file_path) {
    if (g_boot_time == 0.0) g_boot_time = NowSeconds();
    if (!g_file && file_path) {
        g_file = fopen(file_path, "w");
        if (!g_file) fprintf(stderr, "log: cannot open %s, console only\n", file_path);
    }
    SetTraceLogCallback(RaylibLogAdapter);
    FLOG_INFO(LOGCAT_CORE, "log started%s%s", file_path ? " -> " : "", file_path ? file_path : "");
}

void FlaxLogShutdown(void) {
    FLOG_INFO(LOGCAT_CORE, "log closed");
    if (g_file) { fclose(g_file); g_file = NULL; }
}

void FlaxLogSetLevel(FlaxLogLevel min_level) { g_min_level = min_level; }
