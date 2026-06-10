#ifndef FLAX_LOG_H
#define FLAX_LOG_H

// ===========================================================================
// Flax Engine - logging.
//
// One funnel for everything the engine says: every line carries a timestamp
// (seconds since boot), a severity and a subsystem category, and goes to both
// the console and a log file (flushed per line, so a crash never eats the
// evidence - the 90s way of debugging a frozen machine).
//
//   FLOG_INFO(LOGCAT_IO, "loaded %s: %d sectors", path, n);
//   -> [  3.214] [INFO ] [IO    ] loaded dev.fmap: 2 sectors
//
// raylib's own TraceLog output is captured through the same funnel under
// LOGCAT_CORE, so GPU uploads, file accesses etc. land in the same file in
// the same format. Call FlaxLogInit() BEFORE InitWindow() to catch raylib's
// startup chatter.
// ===========================================================================

typedef enum {
    LOGCAT_CORE = 0,   // raylib internals, boot/shutdown
    LOGCAT_IO,         // map + asset serialization
    LOGCAT_RENDER,     // mesh building, textures
    LOGCAT_EDITOR,     // map editor actions
    LOGCAT_GAME,       // player, movement, gameplay
    LOGCAT_COUNT
} LogCategory;

typedef enum {
    FLOG_LVL_DEBUG = 0,
    FLOG_LVL_INFO,
    FLOG_LVL_WARN,
    FLOG_LVL_ERROR,
} FlaxLogLevel;

// Open the log file (truncates), hook raylib's TraceLog. Safe to call once.
void FlaxLogInit(const char *file_path);
void FlaxLogShutdown(void);

// Lines below this level are dropped. Default: FLOG_LVL_DEBUG (everything).
void FlaxLogSetLevel(FlaxLogLevel min_level);

void FlaxLog(FlaxLogLevel level, LogCategory cat, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define FLOG_DEBUG(cat, ...) FlaxLog(FLOG_LVL_DEBUG, cat, __VA_ARGS__)
#define FLOG_INFO(cat, ...)  FlaxLog(FLOG_LVL_INFO,  cat, __VA_ARGS__)
#define FLOG_WARN(cat, ...)  FlaxLog(FLOG_LVL_WARN,  cat, __VA_ARGS__)
#define FLOG_ERROR(cat, ...) FlaxLog(FLOG_LVL_ERROR, cat, __VA_ARGS__)

#endif // FLAX_LOG_H
