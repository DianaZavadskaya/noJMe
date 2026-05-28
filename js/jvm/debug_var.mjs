/*
 * J2ME Emulator - Debug runtime variables and logging mutex
 *
 * Migrated from src/jvm/debug_var.c
 *
 * Translation notes:
 *   - `int g_j2me_runtime_debug` becomes a boxed object { value: 0 } so that
 *     importers can hold a reference and observe mutations (JS has no shared
 *     mutable primitive across module boundaries).
 *   - log_lock / log_unlock / log_mutex_init are no-ops: Node.js is
 *     single-threaded and has no need for a logging mutex.
 *   - The Windows CRITICAL_SECTION and POSIX pthread_mutex_t are both omitted.
 */

/**
 * Runtime debug flag.
 *
 * Mirrors `int g_j2me_runtime_debug` from debug_var.c.
 * Use `.value` to read or write:
 *
 *   import { g_j2me_runtime_debug } from './debug_var.mjs';
 *   if (g_j2me_runtime_debug.value) { ... }
 *   g_j2me_runtime_debug.value = 1;  // enable
 */
export const g_j2me_runtime_debug = { value: 0 };

/**
 * Initialize the logging mutex.
 * No-op in JavaScript (single-threaded runtime, no mutex needed).
 * Exported to preserve the symbol declared in debug.h / debug_macros.h.
 */
export function log_mutex_init() {
    // no-op
}

/**
 * Acquire the logging mutex.
 * No-op in JavaScript.
 */
export function log_lock() {
    // no-op
}

/**
 * Release the logging mutex.
 * No-op in JavaScript.
 */
export function log_unlock() {
    // no-op
}
