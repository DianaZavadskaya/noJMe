/*
 * Headless SDL Backend for Testing
 * No display, no audio - just executes bytecode
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#define LIBRETRO  /* Use stubs */
#include "sdl_backend.h"
#include "midp.h"

/* Global context */
static SdlContext g_headless_ctx = {0};

SdlContext* sdl_get_global_context(void) { return &g_headless_ctx; }

void sdl_set_global_context(SdlContext* ctx) {
    if (ctx) g_headless_ctx = *ctx;
    else memset(&g_headless_ctx, 0, sizeof(SdlContext));
}

int sdl_init(JVM* jvm, int width, int height, int scale, bool headless) {
    (void)scale; (void)headless;
    
    if (width <= 0) width = 240;
    if (height <= 0) height = 320;
    
    g_headless_ctx.jvm = jvm;
    g_headless_ctx.width = width;
    g_headless_ctx.height = height;
    g_headless_ctx.scale = 1;
    g_headless_ctx.target_fps = 30;
    g_headless_ctx.running = true;
    g_headless_ctx.headless = true;
    
    /* Allocate framebuffer */
    g_headless_ctx.framebuffer = (uint32_t*)calloc(width * height, sizeof(uint32_t));
    if (!g_headless_ctx.framebuffer) {
        fprintf(stderr, "[Headless] Failed to allocate framebuffer\n");
        return -1;
    }
    
    fprintf(stderr, "[Headless] Initialized %dx%d\n", width, height);
    return 0;
}

void sdl_destroy(SdlContext* ctx) {
    if (ctx && ctx->framebuffer) {
        free(ctx->framebuffer);
        ctx->framebuffer = NULL;
    }
}

void sdl_run(SdlContext* ctx) {
    if (!ctx) return;
    
    /* In headless mode, process timers and repaints (no window rendering) */
    fprintf(stderr, "[Headless] Running headless mode (waiting for MIDlet)...\n");
    
    /* Force running flags - after startApp() returns, the JVM should still be 
     * considered "running" to process timers, repaints, etc. */
    ctx->running = true;
    if (ctx->jvm) {
        ctx->jvm->running = true;
    }
    
    int frames = 0;
    int max_frames = 60000; /* Safety limit */
    
    extern void midp_process_repaints(JVM* jvm);
    extern void jvm_process_timers(JVM* jvm);
    extern void midp_process_call_serially_queue(JVM* jvm);
    extern bool midp_check_alert_timeout(JVM* jvm);
    extern void midp_clear_pending_repaint(void);
    extern void midp_repaint_current(JVM* jvm);
    extern bool midp_handle_soft_button(JVM* jvm, int button_index);
    
    struct timespec start_ts, current_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);
    uint64_t last_auto_redraw = (uint64_t)start_ts.tv_sec * 1000 + start_ts.tv_nsec / 1000000;
    uint64_t start_time_ms = last_auto_redraw;
    
    /* HEADLESS KEY INJECTION: Simulate key presses to advance past loading/menu screens.
     * Many J2ME games wait for user input (FIRE/SELECT key) before starting gameplay.
     * In headless mode, we inject key presses after a delay to allow the game to load.
     * MIDP key codes: FIRE=5, SELECT=-5, LEFT=-2, RIGHT=-4, UP=-1, DOWN=-3
     * Game action codes are negative: FIRE=-5, LEFT=-2, RIGHT=-4, UP=-1, DOWN=-6 */
    extern void midp_call_keyPressed(JVM* jvm, int keycode);
    extern void midp_call_keyReleased(JVM* jvm, int keycode);
    extern int headless_check_text_activity(void);
    
    /* Key injection schedule: inject keys aggressively in the first phase,
     * then switch to soft button presses and idle detection. */
    int keys_to_try[] = {5, -5, -6, 4, 8, 35, 42, 48, 49, 50, 51, 52, 53, 55, 56, 57};
    int num_keys = sizeof(keys_to_try) / sizeof(keys_to_try[0]);
    int key_fire_frame = 100;  /* Start injecting after ~1 second */
    int key_interval = 50;     /* Inject every 0.5 seconds */
    int last_key_frame = key_fire_frame + (num_keys - 1) * key_interval; /* Last key frame */
    
    /* Soft button injection: after regular keys, try soft buttons (left=0, right=1)
     * This handles Form/List displayables with commands (like Exit/OK). */
    int soft_button_frames[] = {
        last_key_frame + key_interval,     /* Left soft button */
        last_key_frame + key_interval * 2, /* Right soft button */
        last_key_frame + key_interval * 3, /* Left soft button again */
        last_key_frame + key_interval * 4, /* Right soft button again */
    };
    int num_soft_keys = sizeof(soft_button_frames) / sizeof(soft_button_frames[0]);
    int soft_button_ids[] = {0, 1, 0, 1}; /* left, right, left, right */
    int last_activity_frame = 0; /* Track last frame with meaningful activity */
    
    /* Idle detection: if no activity for this many frames after key injection phase,
     * the MIDlet is considered idle and we exit cleanly. */
    int idle_timeout_frames = 300; /* 3 seconds at 10ms/frame */
    
    while (ctx->running && ctx->jvm && ctx->jvm->running) {
        frames++;
        int had_activity = 0;
        
        /* Phase 1: Inject regular key presses */
        for (int ki = 0; ki < num_keys; ki++) {
            int target_frame = key_fire_frame + ki * key_interval;
            if (frames == target_frame) {
                fprintf(stderr, "[Headless] Injecting key code %d (frame %d)\n", keys_to_try[ki], frames);
                midp_call_keyPressed(ctx->jvm, keys_to_try[ki]);
                midp_call_keyReleased(ctx->jvm, keys_to_try[ki]);
                had_activity = 1;
            }
        }
        
        /* Phase 2: Inject soft button presses (for Form/List Exit/OK commands) */
        for (int si = 0; si < num_soft_keys; si++) {
            if (frames == soft_button_frames[si]) {
                fprintf(stderr, "[Headless] Injecting soft button %d (frame %d)\n", 
                        soft_button_ids[si], frames);
                midp_handle_soft_button(ctx->jvm, soft_button_ids[si]);
                had_activity = 1;
            }
        }
        
        /* Process timers */
        jvm_process_timers(ctx->jvm);
        
        /* Process callSerially queue */
        midp_process_call_serially_queue(ctx->jvm);
        
        /* Check Alert timeout */
        midp_check_alert_timeout(ctx->jvm);
        
        /* Auto-redraw at ~60 FPS */
        clock_gettime(CLOCK_MONOTONIC, &current_ts);
        uint64_t current_time = (uint64_t)current_ts.tv_sec * 1000 + current_ts.tv_nsec / 1000000;
        if (current_time - last_auto_redraw >= 16) {
            last_auto_redraw = current_time;
            midp_process_repaints(ctx->jvm);
            midp_clear_pending_repaint();
        }
        
        /* Check for error display */
        if (sdl_has_error()) {
            fprintf(stderr, "[Headless] Error detected, stopping.\n");
            break;
        }
        
        /* Track activity - also count text drawing as activity */
        if (had_activity || headless_check_text_activity()) {
            last_activity_frame = frames;
        }
        
        /* Idle detection: after all key injections are complete,
         * if no activity for idle_timeout_frames, exit cleanly.
         * This handles MIDlets that never call notifyDestroyed(). */
        if (frames > soft_button_frames[num_soft_keys - 1] + 50) {
            if (frames - last_activity_frame >= idle_timeout_frames) {
                fprintf(stderr, "[Headless] Idle timeout: no activity for %d frames, exiting cleanly.\n",
                        idle_timeout_frames);
                break;
            }
        }
        
        /* Safety: max time limit (60 seconds) */
        uint64_t elapsed_ms = current_time - start_time_ms;
        if (elapsed_ms > 60000) {
            fprintf(stderr, "[Headless] Max time limit reached (60s), exiting.\n");
            break;
        }
        
        usleep(10000); /* 10ms per iteration */
    }
    
    if (frames >= max_frames) {
        fprintf(stderr, "[Headless] Reached max iterations\n");
    } else {
        fprintf(stderr, "[Headless] Stopped after %d iterations\n", frames);
    }
    
    /* Analyze framebuffer content */
    if (ctx && ctx->framebuffer) {
        int non_zero = 0;
        int total = ctx->width * ctx->height;
        for (int i = 0; i < total; i++) {
            if ((ctx->framebuffer[i] & 0x00FFFFFF) != 0) non_zero++;
        }
        fprintf(stderr, "[Headless] Framebuffer: %d/%d non-zero pixels (%.1f%%)\n",
                non_zero, total, 100.0 * non_zero / total);
        /* Save framebuffer to PPM for visual inspection */
        sdl_save_framebuffer_to_file(ctx, "/tmp/bounce_framebuffer.ppm");
        fprintf(stderr, "[Headless] Framebuffer saved to /tmp/bounce_framebuffer.ppm\n");
    }
    
    fprintf(stderr, "[Headless] Finished\n");
}

void sdl_set_fullscreen(SdlContext* ctx, bool fullscreen) { (void)ctx; (void)fullscreen; }
uint64_t sdl_get_ticks(SdlContext* ctx) { (void)ctx; return 0; }
void sdl_delay(uint32_t ms) { usleep(ms * 1000); }

void sdl_update_screen(SdlContext* ctx) { (void)ctx; }
void sdl_handle_events(SdlContext* ctx) { (void)ctx; }

bool sdl_key_pressed(SdlContext* ctx, int key) { (void)ctx; (void)key; return false; }
MidpGraphics* sdl_get_graphics(SdlContext* ctx) { (void)ctx; return NULL; }

int sdl_audio_init_simple(uint32_t sample_rate) { (void)sample_rate; return 0; }
void sdl_audio_shutdown(void) {}
void sdl_audio_queue_samples(const int16_t* samples, size_t count) { (void)samples; (void)count; }
size_t sdl_audio_get_queued_size(void) { return 0; }

void sdl_present(SdlContext* ctx) { (void)ctx; }

/* Headless mode needs to process repaints */
static bool g_headless_needs_redraw = false;

void sdl_request_redraw(void) {
    g_headless_needs_redraw = true;
}

bool sdl_needs_redraw(SdlContext* ctx) { 
    (void)ctx; 
    return g_headless_needs_redraw; 
}

void sdl_clear_redraw(SdlContext* ctx) { 
    (void)ctx; 
    g_headless_needs_redraw = false; 
}

void sdl_process_events_minimal(void) {
    /* Process timers (e.g. java.util.Timer, TimerTask) */
    extern void jvm_process_timers(JVM* jvm);
    if (g_headless_ctx.jvm && g_headless_ctx.jvm->running) {
        jvm_process_timers(g_headless_ctx.jvm);
    }
    
    /* Process any pending repaints */
    extern void midp_process_repaints(JVM* jvm);
    if (g_headless_needs_redraw && g_headless_ctx.jvm) {
        midp_process_repaints(g_headless_ctx.jvm);
        g_headless_needs_redraw = false;
    }
}

void sdl_update_texture(SdlContext* ctx) { (void)ctx; }

void sdl_clear(SdlContext* ctx, uint32_t color) {
    if (!ctx || !ctx->framebuffer) return;
    for (int i = 0; i < ctx->width * ctx->height; i++)
        ctx->framebuffer[i] = color;
}

int sdl_resize(SdlContext* ctx, int width, int height) { (void)ctx; (void)width; (void)height; return 0; }
int sdl_screenshot(SdlContext* ctx, const char* filename) { (void)ctx; (void)filename; return -1; }
void sdl_frame_end(SdlContext* ctx) { (void)ctx; }
void sdl_sleep(uint32_t ms) { usleep(ms * 1000); }
void sdl_set_title(SdlContext* ctx, const char* title) { (void)ctx; (void)title; }
void sdl_toggle_fullscreen(SdlContext* ctx) { (void)ctx; }
void sdl_get_window_size(SdlContext* ctx, int* width, int* height) {
    if (width && ctx) *width = ctx->width;
    if (height && ctx) *height = ctx->height;
}
void sdl_stop(SdlContext* ctx) { if (ctx) ctx->running = false; }
MidpPlatformCallbacks* sdl_get_platform_callbacks(SdlContext* ctx) { (void)ctx; return NULL; }
bool sdl_is_screen_graphics(MidpGraphics* gfx) { (void)gfx; return true; }

int sdl_audio_init(SdlContext* ctx, int frequency, int channels, int samples) {
    (void)ctx; (void)frequency; (void)channels; (void)samples;
    return 0;
}

int sdl_audio_queue(SdlContext* ctx, const void* data, size_t length) {
    (void)ctx; (void)data; (void)length;
    return 0;
}

uint32_t sdl_audio_queued_size(SdlContext* ctx) { (void)ctx; return 0; }
void sdl_audio_clear(SdlContext* ctx) { (void)ctx; }
void sdl_process_events(SdlContext* ctx) { (void)ctx; }
int sdl_key_to_midp(int sdl_key) { (void)sdl_key; return 0; }
int sdl_key_to_game_action(int sdl_key) { (void)sdl_key; return 0; }
void sdl_get_pointer(SdlContext* ctx, int* x, int* y) { if (x) *x = 0; if (y) *y = 0; (void)ctx; }
bool sdl_pointer_pressed(SdlContext* ctx) { (void)ctx; return false; }
void sdl_dump_info(SdlContext* ctx) { (void)ctx; }
void sdl_save_framebuffer_to_file(SdlContext* ctx, const char* filename) {
    if (!ctx || !ctx->framebuffer || !filename) return;
    FILE* f = fopen(filename, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", ctx->width, ctx->height);
    for (int i = 0; i < ctx->width * ctx->height; i++) {
        uint32_t pixel = ctx->framebuffer[i];
        uint8_t r = (pixel >> 16) & 0xFF;
        uint8_t g = (pixel >> 8) & 0xFF;
        uint8_t b = pixel & 0xFF;
        fputc(r, f); fputc(g, f); fputc(b, f);
    }
    fclose(f);
}

/* JAR resource loading - provided by main.c */
extern uint8_t* load_jar_resource(const char* path, size_t* size);

/* Error display stubs for headless mode */
static char g_error_title[512] = {0};
static char g_error_message[2048] = {0};
static char g_error_stack[8192] = {0};
static char g_error_extra[2048] = {0};
static bool g_has_error = false;

void sdl_set_error_info(const char* title, const char* message, const char* stack_trace) {
    g_has_error = true;
    if (title) {
        strncpy(g_error_title, title, sizeof(g_error_title) - 1);
        g_error_title[sizeof(g_error_title) - 1] = '\0';
    }
    if (message) {
        strncpy(g_error_message, message, sizeof(g_error_message) - 1);
        g_error_message[sizeof(g_error_message) - 1] = '\0';
    }
    if (stack_trace) {
        strncpy(g_error_stack, stack_trace, sizeof(g_error_stack) - 1);
        g_error_stack[sizeof(g_error_stack) - 1] = '\0';
    }
    
    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "  [J2ME UNCAUGHT EXCEPTION]\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "  Exception: %s\n", g_error_title);
    if (g_error_message[0]) fprintf(stderr, "  Message:   %s\n", g_error_message);
    if (g_error_extra[0]) fprintf(stderr, "  Details:   %s\n", g_error_extra);
    if (g_error_stack[0]) fprintf(stderr, "  Stack:\n%s", g_error_stack);
    fprintf(stderr, "========================================\n\n");
    fflush(stderr);
}

void sdl_set_error_extra(const char* extra) {
    if (extra) {
        strncpy(g_error_extra, extra, sizeof(g_error_extra) - 1);
        g_error_extra[sizeof(g_error_extra) - 1] = '\0';
    } else {
        g_error_extra[0] = '\0';
    }
}

bool sdl_has_error(void) {
    return g_has_error;
}

void sdl_clear_error(void) {
    g_has_error = false;
    g_error_title[0] = '\0';
    g_error_message[0] = '\0';
    g_error_stack[0] = '\0';
    g_error_extra[0] = '\0';
}

void sdl_draw_error_screen(SdlContext* ctx) {
    /* In headless mode, just print to stderr */
    (void)ctx;
    fprintf(stderr, "\n========== ERROR SCREEN ==========\n");
    fprintf(stderr, "Error: %s\n", g_error_title);
    if (g_error_message[0]) {
        fprintf(stderr, "%s\n", g_error_message);
    }
    if (g_error_stack[0]) {
        fprintf(stderr, "\nStack:\n%s\n", g_error_stack);
    }
    fprintf(stderr, "==================================\n");
    fflush(stderr);
}
