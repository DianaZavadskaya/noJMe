/*
 * J2ME Emulator - SDL2 Backend
 * Cross-platform graphics, input, and audio using SDL2
 * Falls back to stub implementation if SDL2 is not available
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#endif
#include <time.h>

/* Try to include SDL2 - support multiple header locations */
#if defined(HAVE_SDL2) || \
    defined(__has_include)
#  if __has_include(<SDL2/SDL.h>)
#    define SDL2_AVAILABLE 1
#    include <SDL2/SDL.h>
#  elif __has_include(<SDL.h>)
#    define SDL2_AVAILABLE 1
#    include <SDL.h>
#  else
#    define SDL2_AVAILABLE 0
#  endif
#else
/* Assume SDL2 might be available, try to include */
#  if defined(_WIN32) || defined(__MINGW32__)
#    define SDL2_AVAILABLE 1
#    include <SDL.h>
#  else
#    define SDL2_AVAILABLE 0
#  endif
#endif

#include "sdl_backend.h"
#include "midp.h"
#include "midi.h"
#include "debug.h"
#include "debug_macros.h"

/* Global context */
static SdlContext* g_sdl_ctx_ptr = NULL;

/* Mutex for thread-safe SDL access */
#ifdef _WIN32
#include <windows.h>
static CRITICAL_SECTION g_sdl_mutex;
static bool g_sdl_mutex_initialized = false;
#define SDL_LOCK() EnterCriticalSection(&g_sdl_mutex)
#define SDL_UNLOCK() LeaveCriticalSection(&g_sdl_mutex)
#else
#include <pthread.h>
static pthread_mutex_t g_sdl_mutex = PTHREAD_MUTEX_INITIALIZER;
#define SDL_LOCK() pthread_mutex_lock(&g_sdl_mutex)
#define SDL_UNLOCK() pthread_mutex_unlock(&g_sdl_mutex)
#endif

#if SDL2_AVAILABLE
static SDL_Window* g_window = NULL;
static SDL_Renderer* g_renderer = NULL;
static SDL_Texture* g_texture = NULL;
static SDL_AudioDeviceID g_audio_dev = 0;

/* Key mapping tables
 * 
 * ИСПРАВЛЕНО: Теперь отправляем правильные keyCode по стандарту Nokia FullCanvas:
 * - UP: keyCode = -1 (FULL_UP)
 * - DOWN: keyCode = -2 (FULL_DOWN)
 * - LEFT: keyCode = -3 (FULL_LEFT)
 * - RIGHT: keyCode = -4 (FULL_RIGHT)
 * - FIRE: keyCode = -5 (FULL_FIRE)
 * 
 * game_action используется для getGameAction() конверсии.
 * Многие игры (например Bobby Carrot) проверяют keyCode напрямую,
 * а не через getGameAction(), поэтому keyCode должен быть правильным.
 */
static const struct {
    SDL_Keycode sdl_key;
    int midp_key;       /* keyCode отправляемый в keyPressed() */
    int game_action;    /* game action для getGameAction() */
} key_map[] = {
    /* Цифры - отправляем ASCII коды */
    {SDLK_0, '0', 0}, {SDLK_1, '1', 0}, {SDLK_2, '2', 0},
    {SDLK_3, '3', 0}, {SDLK_4, '4', 0}, {SDLK_5, '5', 0},
    {SDLK_6, '6', 0}, {SDLK_7, '7', 0}, {SDLK_8, '8', 0},
    {SDLK_9, '9', 0}, {SDLK_ASTERISK, '*', 0}, {SDLK_HASH, '#', 0},
    /* Game keys - отправляем keyCode по стандарту Nokia FullCanvas */
    /* Игра Bobby Carrot проверяет: var1 == -5 || var1 == 5 || var1 == 53 для FIRE */
    {SDLK_UP, -1, GAME_UP},      /* keyCode=-1 (Nokia FULL_UP), gameAction=1 */
    {SDLK_DOWN, -2, GAME_DOWN},  /* keyCode=-2 (Nokia FULL_DOWN), gameAction=6 */
    {SDLK_LEFT, -3, GAME_LEFT},  /* keyCode=-3 (Nokia FULL_LEFT), gameAction=2 */
    {SDLK_RIGHT, -4, GAME_RIGHT},/* keyCode=-4 (Nokia FULL_RIGHT), gameAction=5 */
    {SDLK_RETURN, -5, GAME_FIRE},/* keyCode=-5 (Nokia FULL_FIRE), gameAction=8 */
    {SDLK_SPACE, -5, GAME_FIRE}, /* keyCode=-5 (Nokia FULL_FIRE), gameAction=8 */
    /* Soft buttons - F1 = Left Soft, F2 = Right Soft (по стандарту Nokia) */
    /* F1 отправляет keyCode=-6 (стандартный код левой софт-кнопки) */
    /* F2 отправляет keyCode=-7 (стандартный код правой софт-кнопки) */
    {SDLK_F1, -6, 0},            /* Left Soft Key: keyCode=-6 */
    {SDLK_F2, -7, 0},            /* Right Soft Key: keyCode=-7 */
    /* Game A/B/C/D - альтернативные клавиши (используются реже) */
    {SDLK_a, -6, GAME_A},        /* keyCode=-6, gameAction=9 */
    {SDLK_b, -7, GAME_B},        /* keyCode=-7, gameAction=10 */
    {SDLK_c, -8, GAME_C},        /* keyCode=-8, gameAction=11 */
    {SDLK_d, -9, GAME_D},        /* keyCode=-9, gameAction=12 */
    {SDLK_ESCAPE, 0, 0},
};
#define KEY_MAP_SIZE (sizeof(key_map) / sizeof(key_map[0]))

/* Software key repeat for SDL build.
 * SDL2 doesn't generate key repeat events by default.
 * Many J2ME Canvas games rely on repeated keyPressed events for
 * continuous movement. This implements the same repeat behavior as
 * real J2ME phones: initial delay ~400ms, then repeat at ~80ms interval.
 */
#define SDL_KEY_REPEAT_DELAY_MS   400
#define SDL_KEY_REPEAT_INTERVAL_MS 80
#define SDL_KEY_REPEAT_MAX_KEYS   256  /* Based on SDL_Keycode range */

static struct {
    uint64_t press_time[SDL_KEY_REPEAT_MAX_KEYS];      /* When key was first pressed */
    uint64_t last_repeat_time[SDL_KEY_REPEAT_MAX_KEYS]; /* Last repeat event time */
    bool is_held[SDL_KEY_REPEAT_MAX_KEYS];             /* Key is currently held */
} g_sdl_key_repeat = {{0}, {0}, {false}};

/* Convert SDL_Keycode to index for repeat tracking (handles negative values) */
static int sdl_key_to_repeat_idx(SDL_Keycode key) {
    /* SDL_Keycode values can be negative (e.g. SDLK_RETURN = 13)
     * Map them to positive indices by adding an offset */
    int idx = (int)key + 128;  /* Shift to make most keys positive */
    if (idx < 0) idx = 0;
    if (idx >= SDL_KEY_REPEAT_MAX_KEYS) idx = SDL_KEY_REPEAT_MAX_KEYS - 1;
    return idx;
}

/* Get current time in milliseconds */
static uint64_t sdl_get_time_ms(void) {
    return SDL_GetTicks();
}

/* Process software key repeat - call this after event handling */
static void sdl_process_key_repeat(SdlContext* ctx) {
    if (!ctx || !ctx->jvm) return;
    
    uint64_t now = sdl_get_time_ms();
    
    for (size_t i = 0; i < KEY_MAP_SIZE; i++) {
        SDL_Keycode key = key_map[i].sdl_key;
        int midp_key = key_map[i].midp_key;
        int game_action = key_map[i].game_action;
        
        /* Skip soft keys, escape, etc. - they should not repeat */
        if (key == SDLK_F1 || key == SDLK_F2 || key == SDLK_ESCAPE || key == SDLK_F12) {
            continue;
        }
        
        /* Skip if no valid key to send */
        if (!midp_key && !game_action) continue;
        
        int idx = sdl_key_to_repeat_idx(key);
        
        if (g_sdl_key_repeat.is_held[idx]) {
            uint64_t hold_duration = now - g_sdl_key_repeat.press_time[idx];
            uint64_t since_last = now - g_sdl_key_repeat.last_repeat_time[idx];
            
            if (hold_duration >= SDL_KEY_REPEAT_DELAY_MS &&
                since_last >= SDL_KEY_REPEAT_INTERVAL_MS) {
                /* Generate repeat keyPressed event */
                g_sdl_key_repeat.last_repeat_time[idx] = now;
                int keycode = midp_key ? midp_key : game_action;
                midp_call_keyPressed(ctx->jvm, keycode);
            }
        }
    }
}

/* Note: Audio uses SDL_QueueAudio() for queue-based playback.
 * No callback needed - samples are generated in media.c audio thread. */

#endif /* SDL2_AVAILABLE */

/* Initialize SDL */
int sdl_init(JVM* jvm, int width, int height, int scale, bool headless) {
    DEBUG_LOG("[SDL] Initializing %dx%d (scale: %d, headless: %s)", width, height, scale, headless ? "yes" : "no");

#ifdef _WIN32
    /* Initialize mutex for Windows */
    if (!g_sdl_mutex_initialized) {
        InitializeCriticalSection(&g_sdl_mutex);
        g_sdl_mutex_initialized = true;
    }
#endif

    /* Use the global context pointer set by main.c */
    if (!g_sdl_ctx_ptr) {
        DEBUG_LOG("[SDL] ERROR: g_sdl_ctx_ptr is NULL - must be set before sdl_init");
        return -1;
    }
    
    SdlContext* ctx = g_sdl_ctx_ptr;
    memset(ctx, 0, sizeof(SdlContext));
    ctx->width = width;
    ctx->height = height;
    ctx->scale = scale;
    ctx->jvm = jvm;
    ctx->running = true;
    ctx->target_fps = 60;  /* Default FPS */
    ctx->headless = headless;  /* Store headless flag */
    ctx->input.pointer_x = 0;
    ctx->input.pointer_y = 0;
    ctx->input.pointer_pressed = false;
    memset(ctx->input.key_states, 0, sizeof(ctx->input.key_states));

    /* Allocate framebuffer */
    ctx->framebuffer = (uint32_t*)malloc(width * height * sizeof(uint32_t));
    if (!ctx->framebuffer) {
        return -1;
    }
    
    /* ИСПРАВЛЕНО: Инициализируем framebuffer чёрным непрозрачным цветом.
     * calloc инициализирует нулями, что означает alpha=0 (прозрачный).
     * Но для J2ME игр нужен чёрный фон с alpha=255 (непрозрачный).
     * Формат ARGB: 0xFF000000 = чёрный, непрозрачный
     */
    for (int i = 0; i < width * height; i++) {
        ctx->framebuffer[i] = 0xFF000000;  /* Чёрный, непрозрачный */
    }

    /* ИСПРАВЛЕНО: В headless режиме не создаём SDL окно */
    if (headless) {
        DEBUG_LOG("[SDL] Headless mode - skipping SDL window creation");
        return 0;
    }

#if SDL2_AVAILABLE
    /* ИСПРАВЛЕНИЕ: Включаем VSync для устранения мерцания и tearing */
    /* Используем OpenGL или DirectX вместо software renderer */
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");
    
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        DEBUG_LOG("[SDL] Failed to initialize: %s", SDL_GetError());
        free(ctx->framebuffer);
        ctx->framebuffer = NULL;
        return -1;
    }

    g_window = SDL_CreateWindow(
        "J2ME Emulator",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width * scale, height * scale,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );

    if (!g_window) {
        DEBUG_LOG("[SDL] Failed to create window: %s", SDL_GetError());
        SDL_Quit();
        free(ctx->framebuffer);
        ctx->framebuffer = NULL;
        return -1;
    }

    /* ИСПРАВЛЕНО: Пробуем hardware-accelerated renderer с VSync */
    g_renderer = SDL_CreateRenderer(g_window, -1, 
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    if (!g_renderer) {
        /* Fallback: try without VSync */
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    }
    
    if (!g_renderer) {
        /* Last resort: software renderer */
        DEBUG_LOG("[SDL] Hardware acceleration not available, using software renderer");
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    }

    if (!g_renderer) {
        DEBUG_LOG("[SDL] Failed to create renderer: %s", SDL_GetError());
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        free(ctx->framebuffer);
        ctx->framebuffer = NULL;
        return -1;
    }

    SDL_RenderSetLogicalSize(g_renderer, width, height);

    g_texture = SDL_CreateTexture(g_renderer,
        SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        width, height);

    if (!g_texture) {
        DEBUG_LOG("[SDL] Failed to create texture: %s", SDL_GetError());
        SDL_DestroyRenderer(g_renderer);
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        free(ctx->framebuffer);
        ctx->framebuffer = NULL;
        return -1;
    }

    DEBUG_LOG("[SDL] Initialized with real SDL2");
#else
    DEBUG_LOG("[SDL] Running in headless mode (no SDL2)");
    DEBUG_LOG("[SDL] Press Ctrl+C to exit");
#endif

    DEBUG_LOG("[SDL] Context: %p, target_fps: %d", (void*)ctx, ctx->target_fps);
    return 0;
}

/* Destroy SDL */
void sdl_destroy(SdlContext* ctx) {
#if SDL2_AVAILABLE
    if (g_audio_dev) {
        SDL_CloseAudioDevice(g_audio_dev);
        g_audio_dev = 0;
    }
    if (g_texture) { SDL_DestroyTexture(g_texture); g_texture = NULL; }
    if (g_renderer) { SDL_DestroyRenderer(g_renderer); g_renderer = NULL; }
    if (g_window) { SDL_DestroyWindow(g_window); g_window = NULL; }
    SDL_Quit();
#endif

    if (ctx && ctx->framebuffer) {
        free(ctx->framebuffer);
        ctx->framebuffer = NULL;
    }
}

/* Process events */
void sdl_process_events(SdlContext* ctx) {
#if SDL2_AVAILABLE
    SDL_Event event;

    /* Forward declarations for MIDP input functions */
    extern void midp_call_keyPressed(JVM* jvm, int keycode);
    extern void midp_call_keyReleased(JVM* jvm, int keycode);
    extern void midp_call_pointerPressed(JVM* jvm, int x, int y);
    extern void midp_call_pointerReleased(JVM* jvm, int x, int y);
    extern void midp_call_pointerDragged(JVM* jvm, int x, int y);
    
    /* Track previous pointer state for drag detection */
    static int prev_pointer_x = -1;
    static int prev_pointer_y = -1;
    static bool prev_pointer_pressed = false;

    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                ctx->running = false;
                if (ctx->jvm) ctx->jvm->running = false;
                break;

            case SDL_KEYDOWN:
            case SDL_KEYUP:
                {
                    bool pressed = (event.type == SDL_KEYDOWN);
                    SDL_Keycode key = event.key.keysym.sym;
                    
                    /* Track key hold state for software key repeat.
                     * On real J2ME phones, holding a key generates repeated
                     * keyPressed events. We track hold state here and
                     * generate repeats in sdl_process_key_repeat(). */
                    int repeat_idx = sdl_key_to_repeat_idx(key);
                    if (pressed && !event.key.repeat) {
                        /* Initial press - start tracking for repeat */
                        uint64_t now_ts = sdl_get_time_ms();
                        g_sdl_key_repeat.is_held[repeat_idx] = true;
                        g_sdl_key_repeat.press_time[repeat_idx] = now_ts;
                        g_sdl_key_repeat.last_repeat_time[repeat_idx] = now_ts;
                    } else if (!pressed) {
                        /* Key released - clear repeat state */
                        g_sdl_key_repeat.is_held[repeat_idx] = false;
                    }
                    
                    /* Ignore SDL native repeat events - we handle repeat ourselves */
                    if (pressed && event.key.repeat) {
                        break;
                    }
                    
                    int midp_key = 0;
                    int game_action = 0;

                    for (size_t i = 0; i < KEY_MAP_SIZE; i++) {
                        if (key_map[i].sdl_key == key) {
                            midp_key = key_map[i].midp_key;
                            game_action = key_map[i].game_action;
                            
                            /* Сохраняем состояние клавиши для game_action (положительное) */
                            /* midp_key не используем для key_states, т.к. он может быть отрицательным */
                            if (game_action > 0 && game_action < 256) {
                                ctx->input.key_states[game_action] = pressed;
                                GFX_DEBUG("[SDL] Key %s: SDL=%d -> keyCode=%d, gameAction=%d",
                                        pressed ? "DOWN" : "UP", key, midp_key, game_action);
                            }
                            break;
                        }
                    }

                    /* ИСПРАВЛЕНО: Soft button handling (F1/F2) делаем ПЕРВЫМ!
                     * По стандарту MIDP:
                     * - Если displayable имеет Commands, вызываем commandAction()
                     * - Если нет Commands, передаём keyPressed(-6/-7) в игру
                     * F1 = -6 (Left Soft Key), F2 = -7 (Right Soft Key)
                     */
                    if (pressed && ctx->jvm && (key == SDLK_F1 || key == SDLK_F2)) {
                        int soft_button = (key == SDLK_F1) ? 0 : 1;
                        int soft_keycode = (key == SDLK_F1) ? -6 : -7;
                        
                        GFX_DEBUG("[SDL] Soft button: key=%s -> button=%d, keyCode=%d",
                                key == SDLK_F1 ? "F1" : "F2", soft_button, soft_keycode);
                        
                        extern bool midp_handle_soft_button(JVM* jvm, int button_index);
                        if (!midp_handle_soft_button(ctx->jvm, soft_button)) {
                            /* No commands handled - pass keyPressed to game */
                            GFX_DEBUG("[SDL] No commands, passing keyPressed(%d) to game", soft_keycode);
                            midp_call_keyPressed(ctx->jvm, soft_keycode);
                        }
                        break; /* Обработано, не передаём дальше */
                    }

                    /* Call keyPressed/keyReleased on Canvas */
                    if (ctx->jvm && (midp_key || game_action)) {
                        /* Check if menu navigation should be handled first */
                        if (pressed && game_action) {
                            extern bool midp_is_command_menu_open(void);
                            extern bool midp_handle_menu_navigation(int direction);
                            extern bool midp_handle_soft_button(JVM* jvm, int button_index);
                            
                            if (midp_is_command_menu_open()) {
                                if (game_action == 1) { /* UP */
                                    midp_handle_menu_navigation(-1);
                                    GFX_DEBUG("[SDL] Menu navigation UP");
                                    break; /* Don't pass to game */
                                } else if (game_action == 6) { /* DOWN */
                                    midp_handle_menu_navigation(1);
                                    GFX_DEBUG("[SDL] Menu navigation DOWN");
                                    break; /* Don't pass to game */
                                } else if (game_action == 8) { /* FIRE - confirm selection */
                                    /* Right button (index 1) confirms selection */
                                    midp_handle_soft_button(ctx->jvm, 1);
                                    GFX_DEBUG("[SDL] Menu FIRE - confirm selection");
                                    break; /* Don't pass to game */
                                }
                            }
                        }
                        
                        /* ИСПРАВЛЕНО: Отправляем keyCode по стандарту Nokia FullCanvas:
                         * - UP = -1, DOWN = -2, LEFT = -3, RIGHT = -4, FIRE = -5
                         * Игра сама вызовет getGameAction() для конвертации в game action
                         */
                        int keycode = midp_key ? midp_key : game_action;
                        
                        if (pressed) {
                            DEBUG_LOG("[SDL] Key pressed: SDL=%d -> MIDP keyCode=%d (gameAction=%d)", key, keycode, game_action);
                            midp_call_keyPressed(ctx->jvm, keycode);
                        } else {
                            midp_call_keyReleased(ctx->jvm, keycode);
                        }
                    }

                    if (key == SDLK_ESCAPE && pressed) {
                        ctx->running = false;
                        if (ctx->jvm) ctx->jvm->running = false;
                    }
                    
                    /* F12: Toggle debug mode at runtime */
                    if (key == SDLK_F12 && pressed) {
                        bool new_state = j2me_debug_toggle();
                        /* Also update the window title to show debug state */
                        char title[256];
                        snprintf(title, sizeof(title), "J2ME Emulator [Debug: %s]", 
                                 new_state ? "ON" : "OFF");
                        sdl_set_title(ctx, title);
                    }
                }
                break;

            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                {
                    bool pressed = (event.type == SDL_MOUSEBUTTONDOWN);
                    int x = event.button.x / ctx->scale;
                    int y = event.button.y / ctx->scale;
                    
                    ctx->input.pointer_x = x;
                    ctx->input.pointer_y = y;
                    ctx->input.pointer_pressed = pressed;
                    
                    /* Call pointer event handlers */
                    if (ctx->jvm) {
                        if (pressed && !prev_pointer_pressed) {
                            /* pointerPressed: transition from not pressed to pressed */
                            DEBUG_LOG("[SDL] Pointer pressed at (%d, %d)", x, y);
                            midp_call_pointerPressed(ctx->jvm, x, y);
                        } else if (!pressed && prev_pointer_pressed) {
                            /* pointerReleased: transition from pressed to not pressed */
                            DEBUG_LOG("[SDL] Pointer released at (%d, %d)", x, y);
                            midp_call_pointerReleased(ctx->jvm, x, y);
                        }
                    }
                    
                    prev_pointer_x = x;
                    prev_pointer_y = y;
                    prev_pointer_pressed = pressed;
                }
                break;

            case SDL_MOUSEMOTION:
                {
                    int x = event.motion.x / ctx->scale;
                    int y = event.motion.y / ctx->scale;
                    
                    ctx->input.pointer_x = x;
                    ctx->input.pointer_y = y;
                    
                    /* Check for drag: pointer is pressed and moving */
                    if (ctx->jvm && prev_pointer_pressed && 
                        (x != prev_pointer_x || y != prev_pointer_y)) {
                        DEBUG_LOG("[SDL] Pointer dragged to (%d, %d)", x, y);
                        midp_call_pointerDragged(ctx->jvm, x, y);
                    }
                    
                    prev_pointer_x = x;
                    prev_pointer_y = y;
                }
                break;

            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
                    int new_w = event.window.data1;
                    int new_h = event.window.data2;
                    ctx->scale = (new_w / ctx->width < new_h / ctx->height)
                        ? new_w / ctx->width : new_h / ctx->height;
                    if (ctx->scale < 1) ctx->scale = 1;
                }
                break;
        }
    }
    
    /* Process software key repeat after all events are handled.
     * Generates repeated keyPressed events for held game keys. */
    sdl_process_key_repeat(ctx);
#endif
}

/* Minimal event processing for cooperative threading - called from Thread.sleep() */
void sdl_process_events_minimal(void) {
#if SDL2_AVAILABLE
    if (!g_sdl_ctx_ptr) return;
    
    /* Only process events if we have a valid SDL window */
    if (!g_window) return;
    
    /* ИСПРАВЛЕНО: Кооперативные потоки выполняются в контексте главного потока
     * через ucontext, поэтому SDL_PollEvent безопасен для вызова.
     * Обрабатываем события клавиш, чтобы игры могли реагировать на ввод
     * даже во время выполнения своих игровых циклов. */
    SDL_Event event;
    
    /* Forward declarations for MIDP input functions */
    extern void midp_call_keyPressed(JVM* jvm, int keycode);
    extern void midp_call_keyReleased(JVM* jvm, int keycode);
    
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                g_sdl_ctx_ptr->running = false;
                if (g_sdl_ctx_ptr->jvm) g_sdl_ctx_ptr->jvm->running = false;
                break;

            case SDL_KEYDOWN:
            case SDL_KEYUP:
                {
                    bool pressed = (event.type == SDL_KEYDOWN);
                    SDL_Keycode key = event.key.keysym.sym;
                    
                    /* Track key hold state for software key repeat */
                    int repeat_idx2 = sdl_key_to_repeat_idx(key);
                    if (pressed && !event.key.repeat) {
                        uint64_t now_ts2 = sdl_get_time_ms();
                        g_sdl_key_repeat.is_held[repeat_idx2] = true;
                        g_sdl_key_repeat.press_time[repeat_idx2] = now_ts2;
                        g_sdl_key_repeat.last_repeat_time[repeat_idx2] = now_ts2;
                    } else if (!pressed) {
                        g_sdl_key_repeat.is_held[repeat_idx2] = false;
                    }
                    
                    /* Ignore SDL native repeat events - we handle repeat ourselves */
                    if (pressed && event.key.repeat) {
                        break;
                    }
                    
                    int midp_key = 0;
                    int game_action = 0;

                    for (size_t i = 0; i < KEY_MAP_SIZE; i++) {
                        if (key_map[i].sdl_key == key) {
                            midp_key = key_map[i].midp_key;
                            game_action = key_map[i].game_action;
                            break;
                        }
                    }

                    /* ИСПРАВЛЕНО: Soft button handling (F1/F2) делаем ПЕРВЫМ! */
                    if (pressed && g_sdl_ctx_ptr->jvm && (key == SDLK_F1 || key == SDLK_F2)) {
                        int soft_button = (key == SDLK_F1) ? 0 : 1;
                        int soft_keycode = (key == SDLK_F1) ? -6 : -7;
                        
                        extern bool midp_handle_soft_button(JVM* jvm, int button_index);
                        if (!midp_handle_soft_button(g_sdl_ctx_ptr->jvm, soft_button)) {
                            midp_call_keyPressed(g_sdl_ctx_ptr->jvm, soft_keycode);
                        }
                        break; /* Обработано, не передаём дальше */
                    }

                    /* Call keyPressed/keyReleased on Canvas */
                    /* Используем keyCode (midp_key) по стандарту Nokia FullCanvas */
                    if (g_sdl_ctx_ptr->jvm && (midp_key || game_action)) {
                        int keycode = midp_key ? midp_key : game_action;
                        
                        if (pressed) {
                            DEBUG_LOG("[SDL] Minimal: Key pressed SDL=%d -> keyCode=%d", key, keycode);
                            midp_call_keyPressed(g_sdl_ctx_ptr->jvm, keycode);
                        } else {
                            midp_call_keyReleased(g_sdl_ctx_ptr->jvm, keycode);
                        }
                    }

                    if (key == SDLK_ESCAPE && pressed) {
                        g_sdl_ctx_ptr->running = false;
                        if (g_sdl_ctx_ptr->jvm) g_sdl_ctx_ptr->jvm->running = false;
                    }
                }
                break;
        }
    }
    
    /* Process software key repeat for minimal event loop too */
    sdl_process_key_repeat(g_sdl_ctx_ptr);
#endif
}

/* Set global context (called by main.c before sdl_init) */
void sdl_set_global_context(SdlContext* ctx) {
    g_sdl_ctx_ptr = ctx;
    DEBUG_LOG("[SDL] Global context set to %p", (void*)ctx);
}

/* Get global context */
SdlContext* sdl_get_global_context(void) {
    return g_sdl_ctx_ptr;
}

/* ИСПРАВЛЕНО: Thread-safe запрос на перерисовку экрана */
void sdl_request_redraw(void) {
    if (g_sdl_ctx_ptr) {
        atomic_store_explicit(&g_sdl_ctx_ptr->needs_redraw, true, memory_order_release);
        /* DEBUG: Log first 20 calls */
        static int redraw_count = 0;
        redraw_count++;
        if (redraw_count <= 20) {
            LOG_SAFE("[SDL] sdl_request_redraw() #%d: set needs_redraw=true, ptr=%p\n", 
                    redraw_count, (void*)g_sdl_ctx_ptr);
        }
    } else {
        LOG_SAFE("[SDL] WARNING: sdl_request_redraw() called but g_sdl_ctx_ptr is NULL!\n");
    }
}

/* Check if redraw is needed */
bool sdl_needs_redraw(SdlContext* ctx) {
    return ctx ? atomic_load_explicit(&ctx->needs_redraw, memory_order_acquire) : false;
}

/* Clear redraw flag */
void sdl_clear_redraw(SdlContext* ctx) {
    if (ctx) {
        atomic_store_explicit(&ctx->needs_redraw, false, memory_order_release);
    }
}

/* Run main loop */
void sdl_run(SdlContext* ctx) {
    if (!ctx) {
        DEBUG_LOG("[SDL] ERROR: sdl_run called with NULL context");
        return;
    }
    
    /* Check if context was properly initialized */
    if (ctx->target_fps <= 0 || ctx->framebuffer == NULL) {
        DEBUG_LOG("[SDL] ERROR: Context not properly initialized");
        DEBUG_LOG("[SDL] target_fps=%d, framebuffer=%p, width=%d, height=%d",
                ctx->target_fps, (void*)ctx->framebuffer, ctx->width, ctx->height);
        DEBUG_LOG("[SDL] This usually means sdl_init() failed or was not called");
        return;
    }

    DEBUG_LOG("[SDL] Running main loop (FPS: %d, %dx%d)", ctx->target_fps, ctx->width, ctx->height);

    /* External function from display.c for callSerially processing */
    extern void midp_process_call_serially_queue(JVM* jvm);
    extern void jvm_process_timers(JVM* jvm);
    extern bool midp_check_alert_timeout(JVM* jvm);
    
    /* ИСПРАВЛЕНО: Функция для вызова paint() на текущем displayable */
    extern void midp_repaint_current(JVM* jvm);

    /* ИСПРАВЛЕНО: Если running=false, принудительно устанавливаем true */
    if (!ctx->running) {
        LOG_SAFE("[HEADLESS] WARNING: ctx->running was false, forcing to true\n");
        ctx->running = true;
    }
    if (ctx->jvm && !ctx->jvm->running) {
        LOG_SAFE("[HEADLESS] WARNING: ctx->jvm->running was false, forcing to true\n");
        ctx->jvm->running = true;
    }

#if SDL2_AVAILABLE
    /* Headless-style mode: SDL2 available but no window (headless or headless-like) */
    /* ИСПРАВЛЕНО: Проверяем g_window ПЕРЕД основным циклом, а не после */
    if (!g_window) {
        /* Headless mode - simple delay loop (SDL2 available but no window created) */
        
        LOG_SAFE("[HEADLESS] Starting headless main loop (SDL2 available, no window)\n");
        LOG_SAFE("[HEADLESS] ctx=%p, ctx->running=%d, ctx->jvm=%p, ctx->jvm->running=%d\n",
                (void*)ctx, ctx->running, (void*)ctx->jvm, ctx->jvm ? ctx->jvm->running : -1);
        
        /* ИСПРАВЛЕНО: Используем clock_gettime вместо SDL_GetTicks, т.к. SDL не инициализирован */
        uint64_t last_auto_redraw = 0;
        const uint64_t auto_redraw_interval = 16;  /* ~60 FPS */
        int headless_loop_count = 0;
        
        /* Get current time using clock_gettime */
        struct timespec start_ts;
        clock_gettime(CLOCK_MONOTONIC, &start_ts);
        last_auto_redraw = (uint64_t)start_ts.tv_sec * 1000 + start_ts.tv_nsec / 1000000;
        
        while (ctx->running && ctx->jvm && ctx->jvm->running) {
            headless_loop_count++;
            
            /* Check for error display mode */
            if (sdl_has_error()) {
                sdl_draw_error_screen(ctx);
                sdl_present(ctx);
                
                /* Short delay in error mode */
#ifdef _WIN32
                Sleep(16);
#else
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 16000000 };
                nanosleep(&ts, NULL);
#endif
                continue;
            }
            
            /* Process timers */
            jvm_process_timers(ctx->jvm);
            
            /* Process callSerially queue */
            midp_process_call_serially_queue(ctx->jvm);
            
            /* Check Alert timeout */
            midp_check_alert_timeout(ctx->jvm);
            
            /* ИСПРАВЛЕНО: Автоматическая перерисовка в headless режиме */
            struct timespec current_ts;
            clock_gettime(CLOCK_MONOTONIC, &current_ts);
            uint64_t current_time = (uint64_t)current_ts.tv_sec * 1000 + current_ts.tv_nsec / 1000000;
            uint64_t elapsed = current_time - last_auto_redraw;
            
            if (elapsed >= auto_redraw_interval) {
                ctx->needs_redraw = true;
                last_auto_redraw = current_time;
                if (headless_loop_count <= 10) {
                    LOG_SAFE("[HEADLESS] Auto-redraw at iteration %d (elapsed=%lu ms)\n", 
                            headless_loop_count, (unsigned long)elapsed);
                }
            }
            
            /* Process pending repaints */
            if (ctx->needs_redraw) {
                midp_process_repaints(ctx->jvm);
                ctx->needs_redraw = false;
                
                /* ИСПРАВЛЕНО: Сигнализируем что repaint обработан */
                extern void midp_clear_pending_repaint(void);
                midp_clear_pending_repaint();
                
                /* В J2ME, поток с необработанным исключением НЕ останавливает приложение.
                 * Только этот поток завершается, остальные продолжают работать.
                 */
                
                /* ИСПРАВЛЕНО: Сохраняем кадр в файл для отладки */
                if (headless_loop_count <= 5 || headless_loop_count % 30 == 0) {
                    char filename[256];
                    snprintf(filename, sizeof(filename), "/tmp/j2me_frame_%d.ppm", headless_loop_count);
                    sdl_save_framebuffer_to_file(ctx, filename);
                }
            }
            
            /* ИСПРАВЛЕНО: В headless режиме SDL не инициализирован, используем nanosleep */
#ifdef _WIN32
            Sleep(16);
#else
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 16000000 };
            nanosleep(&ts, NULL);
#endif
            
            /* Limit headless execution time */
            if (headless_loop_count > 3000) {
                LOG_SAFE("[HEADLESS] Maximum iterations reached\n");
                break;
            }
        }
        return;  /* Headless mode done */
    }
    
    /* Normal SDL2 mode with window - main loop */
    uint64_t last_time = SDL_GetTicks();
    const uint64_t frame_time = 1000 / ctx->target_fps;
    
    ctx->needs_redraw = true;
    uint64_t last_auto_redraw = SDL_GetTicks();
    const uint64_t auto_redraw_interval = 16;

    int loop_count = 0;
    while (ctx->running && ctx->jvm && ctx->jvm->running) {
        loop_count++;
        
        /* Check for error display mode */
        if (sdl_has_error()) {
            sdl_draw_error_screen(ctx);
            
            SDL_LOCK();
            sdl_update_texture(ctx);
            SDL_RenderClear(g_renderer);
            SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
            SDL_RenderPresent(g_renderer);
            SDL_UNLOCK();
            
            /* Still process events so ESC works */
            sdl_process_events(ctx);
            
            SDL_Delay(16);  /* ~60 FPS in error mode */
            continue;
        }
        
        if (loop_count % 60 == 0) {
            DEBUG_LOG("[MAIN_LOOP] Still running, iteration=%d, needs_redraw=%d", loop_count, ctx->needs_redraw);
        }
        
        sdl_process_events(ctx);
        jvm_process_timers(ctx->jvm);
        midp_process_call_serially_queue(ctx->jvm);
        midp_check_alert_timeout(ctx->jvm);
        
        uint64_t current_time = SDL_GetTicks();
        bool current_needs_redraw = atomic_load_explicit(&ctx->needs_redraw, memory_order_acquire);
        if (!current_needs_redraw && (current_time - last_auto_redraw) >= auto_redraw_interval) {
            atomic_store_explicit(&ctx->needs_redraw, true, memory_order_release);
            last_auto_redraw = current_time;
        }
        
        /* DEBUG: Log first 30 checks */
        if (loop_count <= 30) {
            bool needs_redraw_val = atomic_load_explicit(&ctx->needs_redraw, memory_order_acquire);
            LOG_SAFE("[MAIN_LOOP] iteration=%d, needs_redraw=%d\n", loop_count, needs_redraw_val);
        }
        
        bool needs_redraw_val = atomic_load_explicit(&ctx->needs_redraw, memory_order_acquire);
        if (needs_redraw_val) {
            /* Clear the flag BEFORE processing to avoid race condition */
            atomic_store_explicit(&ctx->needs_redraw, false, memory_order_release);
            
            midp_process_repaints(ctx->jvm);
            extern void midp_clear_pending_repaint(void);
            midp_clear_pending_repaint();
            
            /* В J2ME, поток с необработанным исключением НЕ останавливает приложение.
             * Только этот поток завершается, остальные продолжают работать.
             */
        }
        
        SDL_LOCK();
        sdl_update_texture(ctx);
        SDL_RenderClear(g_renderer);
        SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
        SDL_RenderPresent(g_renderer);
        SDL_UNLOCK();

        current_time = SDL_GetTicks();
        uint64_t elapsed = current_time - last_time;
        if (elapsed < frame_time) {
            SDL_Delay(1);
        }
        last_time = SDL_GetTicks();
    }
#else
    /* Headless mode - simple delay loop (No SDL2) */
    uint64_t last_auto_redraw = 0;
    const uint64_t auto_redraw_interval = 16;  /* ~60 FPS */
    int headless_loop_count = 0;
    
    LOG_SAFE("[HEADLESS] Starting headless main loop (No SDL2)\n");
    
    /* ИСПРАВЛЕНО: Устанавливаем начальную перерисовку */
    ctx->needs_redraw = true;
    last_auto_redraw = 0;
    
    while (ctx->running && ctx->jvm && ctx->jvm->running) {
        headless_loop_count++;
        
        /* Process timers */
        jvm_process_timers(ctx->jvm);
        
        /* Process callSerially queue */
        midp_process_call_serially_queue(ctx->jvm);
        
        /* Check Alert timeout */
        midp_check_alert_timeout(ctx->jvm);
        
        /* ИСПРАВЛЕНО: Автоматическая перерисовка в headless режиме */
        uint64_t current_time = sdl_get_ticks(ctx);
        uint64_t elapsed = current_time - last_auto_redraw;
        
        if (elapsed >= auto_redraw_interval) {
            ctx->needs_redraw = true;
            last_auto_redraw = current_time;
            if (headless_loop_count <= 10) {
                LOG_SAFE("[HEADLESS] Auto-redraw at iteration %d (elapsed=%lu ms)\n", 
                        headless_loop_count, (unsigned long)elapsed);
            }
        }
        
        /* Process pending repaints */
        if (ctx->needs_redraw) {
            midp_process_repaints(ctx->jvm);
            ctx->needs_redraw = false;
            
            /* ИСПРАВЛЕНО: Сигнализируем что repaint обработан */
            extern void midp_clear_pending_repaint(void);
            midp_clear_pending_repaint();
            
            /* В J2ME, поток с необработанным исключением НЕ останавливает приложение.
             * Только этот поток завершается, остальные продолжают работать.
             */
        }
        
#ifdef _WIN32
        Sleep(16);  /* ~60 FPS on Windows */
#else
        usleep(16000);  /* ~60 FPS on POSIX */
#endif
        
        /* Limit headless execution time */
        if (headless_loop_count > 3000) {
            LOG_SAFE("[HEADLESS] Maximum iterations reached\n");
            break;
        }
    }
#endif
}

/* Stop */
void sdl_stop(SdlContext* ctx) {
    if (ctx) ctx->running = false;
}

/* Get framebuffer */
uint32_t* sdl_get_framebuffer(SdlContext* ctx) {
    return ctx ? ctx->framebuffer : NULL;
}

/* Clear */
void sdl_clear(SdlContext* ctx, uint32_t color) {
    if (!ctx || !ctx->framebuffer) return;
    for (int i = 0; i < ctx->width * ctx->height; i++) {
        ctx->framebuffer[i] = color;
    }
}

/* Save framebuffer to file (for headless mode debugging) */
void sdl_save_framebuffer_to_file(SdlContext* ctx, const char* filename) {
    if (!ctx || !ctx->framebuffer) return;
    
    FILE* f = fopen(filename, "wb");
    if (!f) {
        LOG_SAFE("[SDL] Failed to open %s for writing\n", filename);
        return;
    }
    
    /* Write PPM header (P6 = RGB binary) */
    fprintf(f, "P6\n%d %d\n255\n", ctx->width, ctx->height);
    
    /* Write pixels (convert ARGB to RGB) */
    for (int i = 0; i < ctx->width * ctx->height; i++) {
        uint32_t pixel = ctx->framebuffer[i];
        uint8_t r = (pixel >> 16) & 0xFF;
        uint8_t g = (pixel >> 8) & 0xFF;
        uint8_t b = pixel & 0xFF;
        fwrite(&r, 1, 1, f);
        fwrite(&g, 1, 1, f);
        fwrite(&b, 1, 1, f);
    }
    
    fclose(f);
    LOG_SAFE("[SDL] Saved framebuffer to %s (%dx%d)\n", filename, ctx->width, ctx->height);
}

/* Present */
void sdl_present(SdlContext* ctx) {
#if SDL2_AVAILABLE
    if (!ctx || !ctx->framebuffer || !g_texture || !g_renderer) {
        GFX_DEBUG("[SDL] present: invalid context or texture");
        return;
    }
    
    static int present_count = 0;
    present_count++;
    if (present_count <= 10) {
        /* Check pixels at position where title.png should be (y=53) */
        int y = 53;
        GFX_DEBUG("[SDL] present #%d: pixel at (0,0)=0x%08X, pixel at (120,53)=0x%08X, pixel at (0,53)=0x%08X", 
                present_count, ctx->framebuffer[0], 
                ctx->framebuffer[y * ctx->width + 120],
                ctx->framebuffer[y * ctx->width + 0]);
    }
    
    SDL_LOCK();
    sdl_update_texture(ctx);
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
    SDL_RenderPresent(g_renderer);
    SDL_UNLOCK();
#endif
}

/* Update texture */
void sdl_update_texture(SdlContext* ctx) {
#if SDL2_AVAILABLE
    if (ctx && ctx->framebuffer && g_texture) {
        /* Debug: show first few pixels */
        static int update_count = 0;
        update_count++;
        if (update_count <= 5) {
            GFX_DEBUG("[SDL] update_texture #%d: first pixels = 0x%08X 0x%08X 0x%08X", 
                    update_count, ctx->framebuffer[0], ctx->framebuffer[1], ctx->framebuffer[2]);
        }
        
        SDL_UpdateTexture(g_texture, NULL, ctx->framebuffer,
            ctx->width * sizeof(uint32_t));
    }
#endif
}

/* Key mapping */
int sdl_key_to_midp(int sdl_key) {
#if SDL2_AVAILABLE
    for (size_t i = 0; i < KEY_MAP_SIZE; i++) {
        if (key_map[i].sdl_key == sdl_key) return key_map[i].midp_key;
    }
#endif
    return sdl_key;
}

int sdl_key_to_game_action(int sdl_key) {
#if SDL2_AVAILABLE
    for (size_t i = 0; i < KEY_MAP_SIZE; i++) {
        if (key_map[i].sdl_key == sdl_key) return key_map[i].game_action;
    }
#endif
    return 0;
}

/* Key state - uses SDL_GetKeyboardState for real-time state */
bool sdl_key_pressed(SdlContext* ctx, int keycode) {
    (void)ctx;  /* Not needed when using SDL_GetKeyboardState */
    
#if SDL2_AVAILABLE
    /* Pump events to ensure keyboard state is current */
    SDL_PumpEvents();
    
    /* Get the current keyboard state directly from SDL */
    const Uint8* keyboard_state = SDL_GetKeyboardState(NULL);
    if (!keyboard_state) {
        return false;
    }
    
    /* Map Nokia FullCanvas keyCode to SDL scancode */
    /* -1=UP, -2=DOWN, -3=LEFT, -4=RIGHT, -5=FIRE */
    SDL_Scancode scancode = 0;
    switch (keycode) {
        case -1:  /* UP (Nokia FULL_UP) */
        case GAME_UP:
            scancode = SDL_SCANCODE_UP; 
            break;
        case -2:  /* DOWN (Nokia FULL_DOWN) */
        case GAME_DOWN:
            scancode = SDL_SCANCODE_DOWN; 
            break;
        case -3:  /* LEFT (Nokia FULL_LEFT) */
        case GAME_LEFT:
            scancode = SDL_SCANCODE_LEFT; 
            break;
        case -4:  /* RIGHT (Nokia FULL_RIGHT) */
        case GAME_RIGHT:
            scancode = SDL_SCANCODE_RIGHT; 
            break;
        case -5:  /* FIRE (Nokia FULL_FIRE) */
        case GAME_FIRE:
            /* FIRE can be Return or Space */
            if (keyboard_state[SDL_SCANCODE_RETURN] || keyboard_state[SDL_SCANCODE_SPACE]) {
                GFX_DEBUG("[sdl_key_pressed] FIRE (keycode=%d) -> pressed", keycode);
                return true;
            }
            return false;
        case -6:  /* GAME_A */
        case GAME_A:
            scancode = SDL_SCANCODE_A; 
            break;
        case -7:  /* GAME_B */
        case GAME_B:
            scancode = SDL_SCANCODE_B; 
            break;
        case -8:  /* GAME_C */
        case GAME_C:
            scancode = SDL_SCANCODE_C; 
            break;
        case -9:  /* GAME_D */
        case GAME_D:
            scancode = SDL_SCANCODE_D; 
            break;
        default:
            /* For other keycodes (regular keys), check keyboard state directly */
            if (keycode >= 0 && keycode < 256) {
                /* Try to map ASCII to scancode */
                if (keycode >= '0' && keycode <= '9') {
                    scancode = SDL_SCANCODE_0 + (keycode - '0');
                } else if (keycode >= 'a' && keycode <= 'z') {
                    scancode = SDL_SCANCODE_A + (keycode - 'a');
                } else {
                    return false;
                }
            } else {
                return false;
            }
            break;
    }
    
    bool state = keyboard_state[scancode];
    if (state) {
        GFX_DEBUG("[sdl_key_pressed] keycode=%d -> pressed (scancode=%d)", keycode, scancode);
    }
    return state;
#else
    (void)keycode;
    return false;
#endif
}

/* Pointer */
void sdl_get_pointer(SdlContext* ctx, int* x, int* y) {
    if (!ctx) { if (x) *x = 0; if (y) *y = 0; return; }
    if (x) *x = ctx->input.pointer_x;
    if (y) *y = ctx->input.pointer_y;
}

bool sdl_pointer_pressed(SdlContext* ctx) {
    return ctx ? ctx->input.pointer_pressed : false;
}

/* Audio */
int sdl_audio_init(SdlContext* ctx, int frequency, int channels, int samples) {
    (void)ctx; (void)frequency; (void)channels; (void)samples;
#if SDL2_AVAILABLE
    if (g_audio_dev) SDL_CloseAudioDevice(g_audio_dev);

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = frequency;
    want.format = AUDIO_S16;
    want.channels = channels;
    want.samples = samples;
    want.callback = NULL;  /* Use SDL_QueueAudio instead of callback */
    want.userdata = ctx;

    g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have,
        SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE);

    if (g_audio_dev == 0) {
        DEBUG_LOG("[SDL] Failed to open audio: %s", SDL_GetError());
        return -1;
    }

    SDL_PauseAudioDevice(g_audio_dev, 0);
    DEBUG_LOG("[SDL] Audio: %d Hz, %d channels, buffer=%d samples", have.freq, have.channels, have.samples);
    return 0;
#else
    return 0;
#endif
}

void sdl_audio_close(SdlContext* ctx) {
    (void)ctx;
#if SDL2_AVAILABLE
    if (g_audio_dev) { SDL_CloseAudioDevice(g_audio_dev); g_audio_dev = 0; }
#endif
}

int sdl_audio_queue(SdlContext* ctx, const void* data, size_t length) {
    (void)ctx; (void)data; (void)length;
#if SDL2_AVAILABLE
    if (g_audio_dev) return SDL_QueueAudio(g_audio_dev, data, length);
#endif
    return -1;
}

uint32_t sdl_audio_queued_size(SdlContext* ctx) {
    (void)ctx;
#if SDL2_AVAILABLE
    if (g_audio_dev) return SDL_GetQueuedAudioSize(g_audio_dev);
#endif
    return 0;
}

void sdl_audio_clear(SdlContext* ctx) {
    (void)ctx;
#if SDL2_AVAILABLE
    if (g_audio_dev) SDL_ClearQueuedAudio(g_audio_dev);
#endif
}

/* Simple audio init without context */
int sdl_audio_init_simple(uint32_t sample_rate) {
#if SDL2_AVAILABLE
    if (g_audio_dev) return 0;  /* Already initialized */
    
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = sample_rate;
    want.format = AUDIO_S16;
    want.channels = 2;  /* Stereo */
    want.samples = 2048;  /* Larger buffer for queue-based audio */
    want.callback = NULL;  /* Use SDL_QueueAudio instead of callback */
    want.userdata = NULL;
    
    g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have,
        SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
    
    if (g_audio_dev == 0) {
        DEBUG_LOG("[SDL] Failed to open audio: %s", SDL_GetError());
        return -1;
    }
    
    SDL_PauseAudioDevice(g_audio_dev, 0);  /* Start audio */
    DEBUG_LOG("[SDL] Audio initialized: %d Hz, %d channels, buffer=%d samples", 
              have.freq, have.channels, have.samples);
    return 0;
#else
    (void)sample_rate;
    return 0;
#endif
}

void sdl_audio_shutdown(void) {
#if SDL2_AVAILABLE
    if (g_audio_dev) {
        SDL_CloseAudioDevice(g_audio_dev);
        g_audio_dev = 0;
    }
#endif
}

/* Timing */
uint64_t sdl_get_ticks(SdlContext* ctx) {
    (void)ctx;
#if SDL2_AVAILABLE
    return SDL_GetTicks();
#else
#ifdef _WIN32
    return GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
#endif
}

void sdl_sleep(uint32_t ms) {
#if SDL2_AVAILABLE
    SDL_Delay(ms);
#else
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
#endif
}

/* Window */
void sdl_set_title(SdlContext* ctx, const char* title) {
    (void)ctx;
#if SDL2_AVAILABLE
    if (g_window && title) SDL_SetWindowTitle(g_window, title);
#endif
}

void sdl_set_fullscreen(SdlContext* ctx, bool fullscreen) {
    (void)ctx; (void)fullscreen;
#if SDL2_AVAILABLE
    if (g_window) SDL_SetWindowFullscreen(g_window,
        fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
#endif
}

void sdl_toggle_fullscreen(SdlContext* ctx) {
    (void)ctx;
#if SDL2_AVAILABLE
    if (g_window) {
        Uint32 flags = SDL_GetWindowFlags(g_window);
        SDL_SetWindowFullscreen(g_window,
            (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
    }
#endif
}

int sdl_resize(SdlContext* ctx, int width, int height) {
    if (!ctx || width <= 0 || height <= 0) return -1;

    /* ИСПРАВЛЕНО: Сначала выделяем новую память, потом освобождаем старую */
    uint32_t* new_framebuffer = (uint32_t*)malloc(width * height * sizeof(uint32_t));
    if (!new_framebuffer) {
        ERROR_LOG("[SDL] Failed to allocate new framebuffer in sdl_resize");
        return -1;
    }
    
    /* Инициализируем чёрным непрозрачным цветом */
    for (int i = 0; i < width * height; i++) {
        new_framebuffer[i] = 0xFF000000;
    }

#if SDL2_AVAILABLE
    SDL_Texture* new_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!new_texture) {
        ERROR_LOG("[SDL] Failed to create new texture in sdl_resize: %s", SDL_GetError());
        free(new_framebuffer);
        return -1;
    }
#endif

    /* Only free old resources after successful allocation */
    free(ctx->framebuffer);
    ctx->framebuffer = new_framebuffer;
    ctx->width = width;
    ctx->height = height;

#if SDL2_AVAILABLE
    if (g_texture) SDL_DestroyTexture(g_texture);
    g_texture = new_texture;
    SDL_RenderSetLogicalSize(g_renderer, width, height);
#endif
    return 0;
}

int sdl_screenshot(SdlContext* ctx, const char* filename) {
    if (!ctx || !filename) return -1;
#if SDL2_AVAILABLE
    if (g_renderer) {
        SDL_Surface* surface = SDL_CreateRGBSurface(0,
            ctx->width * ctx->scale, ctx->height * ctx->scale, 32,
            0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
        if (surface) {
            SDL_RenderReadPixels(g_renderer, NULL, SDL_PIXELFORMAT_ARGB8888,
                surface->pixels, surface->pitch);
            int result = SDL_SaveBMP(surface, filename);
            SDL_FreeSurface(surface);
            return result;
        }
    }
#endif
    return -1;
}

/* Global graphics context for screen - used by display.c */
static MidpGraphics g_sdl_graphics;

MidpGraphics* sdl_get_graphics(SdlContext* ctx) {
    if (!ctx) {
        DEBUG_LOG("[SDL] WARNING: sdl_get_graphics called with NULL context");
        return NULL;
    }
    if (!ctx->framebuffer) {
        DEBUG_LOG("[SDL] WARNING: sdl_get_graphics - framebuffer is NULL");
        return NULL;
    }
    /* Only re-init if dimensions changed */
    if (g_sdl_graphics.width != ctx->width || g_sdl_graphics.height != ctx->height ||
        g_sdl_graphics.pixels != ctx->framebuffer) {
        midp_graphics_init(&g_sdl_graphics, ctx->framebuffer, ctx->width, ctx->height);
    }
    return &g_sdl_graphics;
}

/* Check if the given graphics context is the screen graphics */
bool sdl_is_screen_graphics(MidpGraphics* gfx) {
    return gfx == &g_sdl_graphics;
}

MidpPlatformCallbacks* sdl_get_platform_callbacks(SdlContext* ctx) {
    (void)ctx;
    return NULL;
}

void sdl_dump_info(SdlContext* ctx) {
    if (!ctx) return;
    DEBUG_LOG("[SDL] Window: %dx%d (scale: %d)", ctx->width, ctx->height, ctx->scale);
#if SDL2_AVAILABLE
    DEBUG_LOG("[SDL] Renderer: %s", SDL_GetCurrentVideoDriver());
    if (g_window) {
        int w, h;
        SDL_GetWindowSize(g_window, &w, &h);
        DEBUG_LOG("[SDL] Window size: %dx%d", w, h);
    }
#else
    DEBUG_LOG("[SDL] Running in headless mode");
#endif
}

/* Queue audio samples directly */
void sdl_audio_queue_samples(const int16_t* samples, size_t count) {
#if SDL2_AVAILABLE
    if (g_audio_dev && samples && count > 0) {
        SDL_QueueAudio(g_audio_dev, samples, count * sizeof(int16_t));
    }
#endif
}

/* Get queued audio size */
size_t sdl_audio_get_queued_size(void) {
#if SDL2_AVAILABLE
    if (g_audio_dev) {
        return SDL_GetQueuedAudioSize(g_audio_dev) / sizeof(int16_t);
    }
#endif
    return 0;
}

/* ============================================================
 * Error Screen Display - Shows unhandled exception info
 * ============================================================ */

/* Include bitmap font for error display */
#include "midp/bitmap_font.h"

/* Draw a single character at position (uses bitmap font) */
static void draw_error_char(uint32_t* framebuffer, int fb_width, int fb_height,
                            int codepoint, int x, int y, uint32_t color) {
    if (!framebuffer || x < 0 || y < 0) return;
    if (x + FONT_WIDTH > fb_width || y + FONT_HEIGHT > fb_height) return;
    
    const uint8_t* char_data = get_char_data_unicode(codepoint);
    if (!char_data) return;
    
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t row_data = char_data[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (row_data & (1 << (4 - col))) {  /* MSB is leftmost */
                int px = x + col;
                int py = y + row;
                if (px >= 0 && px < fb_width && py >= 0 && py < fb_height) {
                    framebuffer[py * fb_width + px] = color;
                }
            }
        }
    }
}

/* Draw a UTF-8 string at position */
static void draw_error_string(uint32_t* framebuffer, int fb_width, int fb_height,
                              const char* str, int x, int y, uint32_t color) {
    if (!framebuffer || !str) return;
    
    int cur_x = x;
    int cur_y = y;
    int len = 0;
    while (str[len]) len++;
    
    int i = 0;
    while (i < len) {
        int codepoint = utf8_decode(str, &i, len);
        if (codepoint < 0) break;
        
        /* Handle newline */
        if (codepoint == '\n') {
            cur_x = x;
            cur_y += FONT_HEIGHT + 2;
            continue;
        }
        
        /* Handle tab */
        if (codepoint == '\t') {
            cur_x += (FONT_WIDTH + 1) * 4;
            continue;
        }
        
        /* Skip if outside screen */
        if (cur_x + FONT_WIDTH > fb_width) {
            cur_x = x;
            cur_y += FONT_HEIGHT + 2;
        }
        
        if (cur_y + FONT_HEIGHT > fb_height) break;
        
        draw_error_char(framebuffer, fb_width, fb_height, codepoint, cur_x, cur_y, color);
        cur_x += FONT_WIDTH + 1;  /* +1 for spacing */
    }
}

/* Word-wrap text to fit width */
static int wrap_text_line(const char* text, int max_chars_per_line, char* buffer, int buffer_size) {
    if (!text || !buffer || buffer_size <= 0) return 0;
    
    int text_len = 0;
    while (text[text_len]) text_len++;
    
    int chars_written = 0;
    int line_pos = 0;
    int i = 0;
    
    while (i < text_len && chars_written < buffer_size - 1) {
        int codepoint = utf8_decode(text, &i, text_len);
        if (codepoint < 0) break;
        
        /* Count character width (simplified - all chars are 1 char width) */
        line_pos++;
        
        if (line_pos > max_chars_per_line && codepoint == ' ') {
            buffer[chars_written++] = '\n';
            line_pos = 0;
        } else {
            /* Convert codepoint to UTF-8 for buffer */
            if (codepoint < 0x80) {
                if (chars_written < buffer_size - 1) {
                    buffer[chars_written++] = (char)codepoint;
                }
            } else if (codepoint < 0x800) {
                if (chars_written < buffer_size - 2) {
                    buffer[chars_written++] = (char)(0xC0 | (codepoint >> 6));
                    buffer[chars_written++] = (char)(0x80 | (codepoint & 0x3F));
                }
            } else if (codepoint < 0x10000) {
                if (chars_written < buffer_size - 3) {
                    buffer[chars_written++] = (char)(0xE0 | (codepoint >> 12));
                    buffer[chars_written++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                    buffer[chars_written++] = (char)(0x80 | (codepoint & 0x3F));
                }
            }
        }
    }
    
    buffer[chars_written] = '\0';
    return chars_written;
}

/* Global error message storage */
static char g_error_title[512] = {0};
static char g_error_message[2048] = {0};
static char g_error_stack[8192] = {0};
static char g_error_extra[2048] = {0};
static bool g_has_error = false;

/* Set error information for display */
void sdl_set_error_info(const char* title, const char* message, const char* stack_trace) {
    g_has_error = true;
    
    if (title) {
        strncpy(g_error_title, title, sizeof(g_error_title) - 1);
        g_error_title[sizeof(g_error_title) - 1] = '\0';
    } else {
        strcpy(g_error_title, "Error");
    }
    
    if (message) {
        strncpy(g_error_message, message, sizeof(g_error_message) - 1);
        g_error_message[sizeof(g_error_message) - 1] = '\0';
    } else {
        g_error_message[0] = '\0';
    }
    
    if (stack_trace) {
        strncpy(g_error_stack, stack_trace, sizeof(g_error_stack) - 1);
        g_error_stack[sizeof(g_error_stack) - 1] = '\0';
    } else {
        g_error_stack[0] = '\0';
    }
    
    /* Log to stderr */
    LOG_SAFE("\n========================================\n");
    LOG_SAFE("  [J2ME UNCAUGHT EXCEPTION]\n");
    LOG_SAFE("========================================\n");
    LOG_SAFE("  Exception: %s\n", g_error_title);
    if (g_error_message[0]) LOG_SAFE("  Message:   %s\n", g_error_message);
    if (g_error_extra[0]) LOG_SAFE("  Details:   %s\n", g_error_extra);
    if (g_error_stack[0]) LOG_SAFE("  Stack:\n%s", g_error_stack);
    LOG_SAFE("========================================\n\n");
}

/* Set extra detail info */
void sdl_set_error_extra(const char* extra) {
    if (extra) {
        strncpy(g_error_extra, extra, sizeof(g_error_extra) - 1);
        g_error_extra[sizeof(g_error_extra) - 1] = '\0';
    } else {
        g_error_extra[0] = '\0';
    }
}

/* Check if there's an error to display */
bool sdl_has_error(void) {
    return g_has_error;
}

/* Clear error state */
void sdl_clear_error(void) {
    g_has_error = false;
    g_error_title[0] = '\0';
    g_error_message[0] = '\0';
    g_error_stack[0] = '\0';
    g_error_extra[0] = '\0';
}

/* Draw the error screen */
void sdl_draw_error_screen(SdlContext* ctx) {
    if (!ctx || !ctx->framebuffer) return;
    
    int width = ctx->width;
    int height = ctx->height;
    uint32_t* fb = ctx->framebuffer;
    
    /* Colors: ARGB format */
    const uint32_t bg_color = 0xFF800000;      /* Dark red background */
    const uint32_t title_color = 0xFFFFFFFF;   /* White title */
    const uint32_t msg_color = 0xFFFFFF00;     /* Yellow message */
    const uint32_t stack_color = 0xFFCCCCCC;   /* Light gray stack trace */
    const uint32_t border_color = 0xFFFF0000;  /* Red border */
    
    /* Fill background */
    for (int i = 0; i < width * height; i++) {
        fb[i] = bg_color;
    }
    
    /* Draw border */
    int border_width = 2;
    for (int y = 0; y < border_width; y++) {
        for (int x = 0; x < width; x++) {
            fb[y * width + x] = border_color;
            fb[(height - 1 - y) * width + x] = border_color;
        }
    }
    for (int x = 0; x < border_width; x++) {
        for (int y = 0; y < height; y++) {
            fb[y * width + x] = border_color;
            fb[y * width + (width - 1 - x)] = border_color;
        }
    }
    
    /* Calculate character width for wrapping */
    int char_width = FONT_WIDTH + 1;
    int max_chars = (width - 20) / char_width;
    if (max_chars < 10) max_chars = 10;
    if (max_chars > 60) max_chars = 60;
    
    /* Draw title with prefix */
    char title_line[300];
    snprintf(title_line, sizeof(title_line), "[ERROR] %s", g_error_title);
    draw_error_string(fb, width, height, title_line, 10, 10, title_color);
    
    /* Draw separator */
    int sep_y = 10 + FONT_HEIGHT + 4;
    for (int x = 10; x < width - 10; x++) {
        fb[sep_y * width + x] = border_color;
    }
    
    /* Draw message */
    int msg_y = sep_y + 10;
    if (g_error_message[0]) {
        char wrapped_msg[3000];
        wrap_text_line(g_error_message, max_chars, wrapped_msg, sizeof(wrapped_msg));
        draw_error_string(fb, width, height, wrapped_msg, 10, msg_y, msg_color);
        msg_y += FONT_HEIGHT * 3;  /* Move down for stack trace */
    }
    
    /* Draw stack trace header */
    if (g_error_stack[0]) {
        draw_error_string(fb, width, height, "Stack trace:", 10, msg_y, title_color);
        msg_y += FONT_HEIGHT + 4;
        
        /* Draw separator */
        for (int x = 10; x < width - 10; x++) {
            fb[msg_y * width + x] = border_color;
        }
        msg_y += 4;
        
        /* Draw stack trace with wrapping */
        char wrapped_stack[6000];
        wrap_text_line(g_error_stack, max_chars, wrapped_stack, sizeof(wrapped_stack));
        draw_error_string(fb, width, height, wrapped_stack, 10, msg_y, stack_color);
    }
    
    /* Draw instructions */
    int inst_y = height - FONT_HEIGHT - 15;
    draw_error_string(fb, width, height, "Press ESC to exit", 10, inst_y, stack_color);
    
    /* Force redraw flag */
    ctx->needs_redraw = true;
}