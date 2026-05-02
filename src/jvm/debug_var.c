/*
 * J2ME Emulator - Debug runtime variables and logging mutex
 */

#include <stdio.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static CRITICAL_SECTION g_log_cs;
static int g_log_initialized = 0;
#else
#include <pthread.h>
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

int g_j2me_runtime_debug = 0;

void log_mutex_init(void) {
#ifdef _WIN32
    if (!g_log_initialized) {
        InitializeCriticalSection(&g_log_cs);
        g_log_initialized = 1;
    }
#endif
}

void log_lock(void) {
#ifdef _WIN32
    if (!g_log_initialized) {
        InitializeCriticalSection(&g_log_cs);
        g_log_initialized = 1;
    }
    EnterCriticalSection(&g_log_cs);
#else
    pthread_mutex_lock(&g_log_mutex);
#endif
}

void log_unlock(void) {
#ifdef _WIN32
    LeaveCriticalSection(&g_log_cs);
#else
    pthread_mutex_unlock(&g_log_mutex);
#endif
}
