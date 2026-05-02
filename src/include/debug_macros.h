/*
 * J2ME Emulator - Thread-safe debug macros
 * 
 * CRITICAL: This file MUST be included AFTER debug.h.
 * It forcibly overrides ALL debug macros with thread-safe versions
 * that use log_lock()/log_unlock() to prevent interleaved output.
 * 
 *   #include "debug.h"
 *   #include "debug_macros.h"
 */

#ifndef J2ME_DEBUG_MACROS_H
#define J2ME_DEBUG_MACROS_H

#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>

/* Runtime debug flag (extern - defined in jvm/debug_var.c) */
extern int g_j2me_runtime_debug;

/* Thread-safe logging mutex (extern - defined in jvm/debug_var.c) */
extern void log_lock(void);
extern void log_unlock(void);
extern void log_mutex_init(void);

/* Thread-safe fprintf replacement for stderr.
 * ALWAYS use this instead of fprintf(stderr, ...). */
#ifndef LOG_SAFE
#define LOG_SAFE(fmt, ...) do { \
    log_lock(); \
    fprintf(stderr, fmt, ##__VA_ARGS__); \
    fflush(stderr); \
    log_unlock(); \
} while(0)
#endif

/* Debug-only thread-safe log — only prints when g_j2me_runtime_debug != 0 */
#ifndef VERBOSE_LOG
#define VERBOSE_LOG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, fmt, ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)
#endif

/* ===== Core logging macros =====
 * Each macro is #undef'd first to override any definition from debug.h,
 * then redefined with log_lock()/log_unlock() for thread safety. */

#undef DEBUG_LOG
#define DEBUG_LOG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, "[J2ME] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

#undef DEBUG_LOG_RAW
#define DEBUG_LOG_RAW(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, fmt, ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

#undef ERROR_LOG
#define ERROR_LOG(fmt, ...) do { \
    log_lock(); \
    fprintf(stderr, "[J2ME] [ERROR] " fmt "\n", ##__VA_ARGS__); \
    fflush(stderr); \
    log_unlock(); \
} while(0)

#undef WARN_LOG
#define WARN_LOG(fmt, ...) do { \
    log_lock(); \
    fprintf(stderr, "[J2ME] [WARN] " fmt "\n", ##__VA_ARGS__); \
    fflush(stderr); \
    log_unlock(); \
} while(0)

#undef INFO_LOG
#define INFO_LOG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        printf("[INFO] " fmt "\n", ##__VA_ARGS__); \
        fflush(stdout); \
    } \
} while(0)

/* ===== Module-specific debug macros ===== */

#undef JVM_DEBUG
#define JVM_DEBUG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, "[JVM] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

#undef EXEC_DEBUG
#define EXEC_DEBUG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, "[EXEC] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

#undef CLASS_DEBUG
#define CLASS_DEBUG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, "[CLASS] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

#undef GC_DEBUG
#define GC_DEBUG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, "[GC] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

#undef NATIVE_DEBUG
#define NATIVE_DEBUG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, "[NATIVE] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

#undef MIDP_DEBUG
#define MIDP_DEBUG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, "[MIDP] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

#undef GFX_DEBUG
#define GFX_DEBUG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, "[GFX] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

#undef DISP_DEBUG
#define DISP_DEBUG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, "[DISP] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

#undef FORM_DEBUG
#define FORM_DEBUG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, "[FORM] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

#undef MEDIA_DEBUG
#define MEDIA_DEBUG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, "[MEDIA] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

#undef RMS_DEBUG
#define RMS_DEBUG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, "[RMS] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

#undef THREAD_DEBUG
#define THREAD_DEBUG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, "[THREAD] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

#undef OPCODE_DEBUG
#define OPCODE_DEBUG(fmt, ...) do { \
    if (g_j2me_runtime_debug) { \
        log_lock(); \
        fprintf(stderr, "[OP] " fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
        log_unlock(); \
    } \
} while(0)

/* ===== Utility macros ===== */

#undef WH_DEBUG
#define WH_DEBUG(...) do { } while(0)

#undef SM_DEBUG
#define SM_DEBUG(...) do { } while(0)

/* Hex dump utility */
#ifndef DEBUG_HEXDUMP_DEFINED
#define DEBUG_HEXDUMP_DEFINED
#undef HEXDUMP
static inline void _debug_hexdump(const char* label, const void* data, size_t len) {
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
#define HEXDUMP(label, data, len) _debug_hexdump(label, data, len)
#endif

#endif /* J2ME_DEBUG_MACROS_H */
