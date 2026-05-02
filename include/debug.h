/*
 * J2ME Emulator - Debug Logging System
 * Conditional compilation for debug output
 * 
 * Usage:
 *   - Set J2ME_DEBUG=1 for verbose debug output (default)
 *   - Set J2ME_DEBUG=0 for release mode (errors only)
 *   - Can be defined in Makefile: CFLAGS += -DJ2ME_DEBUG=0
 *   - Press F12 at runtime to toggle debug mode (works in both modes)
 */

#ifndef DEBUG_H
#define DEBUG_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

/* Debug level - can be overridden via compiler flag */
#ifndef J2ME_DEBUG
#define J2ME_DEBUG 0
#endif

/* Error prefix for all log messages */
#define LOG_PREFIX "[J2ME]"

/* ==============================================
 * THREAD-SAFE LOGGING INFRASTRUCTURE
 * ==============================================
 * All log output (both macro-based and direct fprintf)
 * goes through a global mutex to prevent thread interleaving.
 */

/* Mutex functions - implemented in debug_var.c */
extern void log_lock(void);
extern void log_unlock(void);
extern void log_mutex_init(void);

/* Thread-safe fprintf replacement for stderr.
 * Use this instead of fprintf(stderr, ...) throughout the codebase.
 * Includes automatic fflush after each message.
 */
#define LOG_SAFE(fmt, ...) do { \
    log_lock(); \
    fprintf(stderr, fmt, ##__VA_ARGS__); \
    fflush(stderr); \
    log_unlock(); \
} while(0)

/* Debug-only thread-safe log — only prints when g_j2me_runtime_debug != 0.
 * Use this for verbose informational messages that should be silent in
 * normal operation but visible when debugging (F12 toggle at runtime). */
#define VERBOSE_LOG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, fmt, ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

/* ==============================================
 * RUNTIME DEBUG TOGGLE SUPPORT
 * ==============================================
 * Runtime debug flag - allows toggling debug mode with F12
 * Even when compiled with J2ME_DEBUG=0, debug output can be
 * enabled at runtime by pressing F12.
 */

/* Global runtime debug flag - initialized based on compile-time setting */
extern int g_j2me_runtime_debug;

/* Initialize runtime debug state (called once at startup) */
static inline void j2me_debug_init(void) {
    g_j2me_runtime_debug = J2ME_DEBUG;
}

/* Toggle runtime debug mode - returns new state */
static inline bool j2me_debug_toggle(void) {
    g_j2me_runtime_debug = !g_j2me_runtime_debug;
    LOG_SAFE(LOG_PREFIX " [DEBUG] Debug mode %s\n", 
            g_j2me_runtime_debug ? "ENABLED" : "DISABLED");
    return g_j2me_runtime_debug;
}

/* Check if debug mode is enabled (runtime or compile-time) */
static inline bool j2me_debug_enabled(void) {
    return g_j2me_runtime_debug != 0;
}

/* Enable/disable debug mode programmatically */
static inline void j2me_set_debug(bool enabled) {
    g_j2me_runtime_debug = enabled ? 1 : 0;
}

/* ==============================================
 * DEBUG LOGGING MACROS
 * ==============================================
 * These macros now check the runtime flag, allowing
 * toggling debug mode even in release builds.
 * All use log_lock()/log_unlock() for thread safety.
 */

/* Main debug log macro - checks runtime flag */
#define DEBUG_LOG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, LOG_PREFIX " " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

#define DEBUG_LOG_RAW(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, fmt, ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

/* Error log - ALWAYS enabled, even in release mode */
#define ERROR_LOG(fmt, ...) do { \
    log_lock(); \
    fprintf(stderr, LOG_PREFIX " [ERROR] " fmt "\n", ##__VA_ARGS__); \
    fflush(stderr); \
    log_unlock(); \
} while(0)

/* Warning log - ALWAYS enabled */
#define WARN_LOG(fmt, ...) do { \
    log_lock(); \
    fprintf(stderr, LOG_PREFIX " [WARN] " fmt "\n", ##__VA_ARGS__); \
    fflush(stderr); \
    log_unlock(); \
} while(0)

/* Info log - checks runtime flag */
#define INFO_LOG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        printf("[INFO] " fmt "\n", ##__VA_ARGS__); \
        fflush(stdout); \
    } \
} while(0)

/* ==============================================
 * Module-specific debug macros for finer control
 * All check runtime flag for runtime toggle support
 * All use log_lock()/log_unlock() for thread safety.
 * ============================================== */

/* JVM Core */
#define JVM_DEBUG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, "[JVM] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

/* Execution/Interpreter */
#define EXEC_DEBUG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, "[EXEC] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

/* Class loading */
#define CLASS_DEBUG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, "[CLASS] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

/* Heap/GC */
#define GC_DEBUG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, "[GC] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

/* Native methods */
#define NATIVE_DEBUG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, "[NATIVE] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

/* MIDP */
#define MIDP_DEBUG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, "[MIDP] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

/* Graphics */
#define GFX_DEBUG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, "[GFX] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

/* Display */
#define DISP_DEBUG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, "[DISP] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

/* Form/UI */
#define FORM_DEBUG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, "[FORM] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

/* Media */
#define MEDIA_DEBUG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, "[MEDIA] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

/* RMS (Record Management System) */
#define RMS_DEBUG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, "[RMS] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

/* Threading */
#define THREAD_DEBUG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, "[THREAD] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

/* Opcode tracing (very verbose - use DEBUG_EXECUTION flag) */
#define OPCODE_DEBUG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, "[OP] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

/* ==============================================
 * Hex dump utility
 * ============================================== */
static inline void debug_hexdump(const char* label, const void* data, size_t len) {
    if (!g_j2me_runtime_debug) return;
    
    const uint8_t* bytes = (const uint8_t*)data;
    log_lock();
    fprintf(stderr, "[HEX] %s (%zu bytes): ", label, len);
    for (size_t i = 0; i < len && i < 64; i++) {
        fprintf(stderr, "%02X ", bytes[i]);
    }
    if (len > 64) fprintf(stderr, "...");
    fprintf(stderr, "\n");
    fflush(stderr);
    log_unlock();
}
#define HEXDUMP(label, data, len) debug_hexdump(label, data, len)

/* ==============================================
 * Memory tracking
 * ============================================== */
#define J2ME_MEM_ALLOC(ptr, size) DEBUG_LOG("ALLOC: %p (%zu bytes)", ptr, size)
#define J2ME_MEM_FREE(ptr) DEBUG_LOG("FREE: %p", ptr)

/* ==============================================
 * Performance timing
 * ============================================== */
#include <time.h>
#define PERF_START() clock_t _perf_start = clock()
#define PERF_END(label) do { \
    if (g_j2me_runtime_debug) { \
        clock_t _perf_end = clock(); \
        double _perf_ms = ((double)(_perf_end - _perf_start) / CLOCKS_PER_SEC) * 1000.0; \
        DEBUG_LOG("[PERF] %s: %.2f ms", label, _perf_ms); \
    } \
} while(0)

#endif /* DEBUG_H */
