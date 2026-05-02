/* Mock SDL2 header for compilation testing */
#ifndef MOCK_SDL_H
#define MOCK_SDL_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

/* SDL2 types */
typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;
typedef struct SDL_Surface SDL_Surface;
typedef uint32_t SDL_AudioDeviceID;
typedef int32_t SDL_Keycode;
typedef int32_t SDL_Scancode;
typedef uint8_t Uint8;
typedef uint32_t Uint32;

/* SDL init flags */
#define SDL_INIT_VIDEO      0x00000020
#define SDL_INIT_AUDIO      0x00000010
#define SDL_INIT_EVENTS     0x00004000

/* Window flags */
#define SDL_WINDOW_SHOWN    0x00000004
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0x00001001
#define SDL_WINDOW_RESIZABLE 0x00000020

/* Window position */
#define SDL_WINDOWPOS_CENTERED 0x2FFF0000

/* Renderer flags */
#define SDL_RENDERER_ACCELERATED 0x00000002
#define SDL_RENDERER_PRESENTVSYNC 0x00000004
#define SDL_RENDERER_SOFTWARE 0x00000001

/* SDL hints */
#define SDL_HINT_RENDER_DRIVER "SDL_RENDER_DRIVER"
#define SDL_HINT_FRAMEBUFFER_ACCELERATION "SDL_FRAMEBUFFER_ACCELERATION"
#define SDL_HINT_RENDER_VSYNC "SDL_RENDER_VSYNC"

/* Texture format */
#define SDL_PIXELFORMAT_ARGB8888 0x16362004

/* Texture access */
#define SDL_TEXTUREACCESS_STREAMING 0x00000001

/* Key codes */
#define SDLK_UP    0x40000052
#define SDLK_DOWN  0x40000054
#define SDLK_LEFT  0x40000050
#define SDLK_RIGHT 0x4000004F
#define SDLK_RETURN 0x0D
#define SDLK_SPACE  0x20
#define SDLK_ESCAPE 0x1B
#define SDLK_F1     0x4000003A
#define SDLK_F2     0x4000003B
#define SDLK_F3     0x4000003C
#define SDLK_F4     0x4000003D
#define SDLK_F5     0x4000003E
#define SDLK_F6     0x4000003F
#define SDLK_F7     0x40000040
#define SDLK_F8     0x40000041
#define SDLK_F9     0x40000042
#define SDLK_F10    0x40000043
#define SDLK_F11    0x40000044
#define SDLK_F12    0x40000045
#define SDLK_0      0x30
#define SDLK_1      0x31
#define SDLK_2      0x32
#define SDLK_3      0x33
#define SDLK_4      0x34
#define SDLK_5      0x35
#define SDLK_6      0x36
#define SDLK_7      0x37
#define SDLK_8      0x38
#define SDLK_9      0x39
#define SDLK_a      0x61
#define SDLK_b      0x62
#define SDLK_c      0x63
#define SDLK_d      0x64
#define SDLK_ASTERISK 0x2A
#define SDLK_HASH     0x23

/* Scancode constants for SDL_GetKeyboardState */
#define SDL_SCANCODE_UNKNOWN 0
#define SDL_SCANCODE_A 4
#define SDL_SCANCODE_B 5
#define SDL_SCANCODE_C 6
#define SDL_SCANCODE_D 7
#define SDL_SCANCODE_0 39
#define SDL_SCANCODE_1 30
#define SDL_SCANCODE_2 31
#define SDL_SCANCODE_3 32
#define SDL_SCANCODE_4 33
#define SDL_SCANCODE_5 34
#define SDL_SCANCODE_6 35
#define SDL_SCANCODE_7 36
#define SDL_SCANCODE_8 37
#define SDL_SCANCODE_9 38
#define SDL_SCANCODE_RETURN 40
#define SDL_SCANCODE_SPACE 44
#define SDL_SCANCODE_UP    82
#define SDL_SCANCODE_DOWN  81
#define SDL_SCANCODE_LEFT  80
#define SDL_SCANCODE_RIGHT 79

/* Event types */
#define SDL_KEYDOWN     0x300
#define SDL_KEYUP       0x301
#define SDL_QUIT        0x100
#define SDL_MOUSEBUTTONDOWN 0x401
#define SDL_MOUSEBUTTONUP   0x402
#define SDL_MOUSEMOTION     0x400
#define SDL_WINDOWEVENT     0x200

/* Window events */
#define SDL_WINDOWEVENT_RESIZED 0x05

/* Audio format */
#define AUDIO_S16SYS 0x8010
#define AUDIO_S16    0x8010

/* Audio change flags */
#define SDL_AUDIO_ALLOW_FREQUENCY_CHANGE 0x01
#define SDL_AUDIO_ALLOW_CHANNELS_CHANGE  0x02

/* Keyboard event structure */
typedef struct SDL_Keysym {
    SDL_Keycode sym;
    uint16_t mod;
    uint32_t scancode;
} SDL_Keysym;

typedef struct SDL_KeyboardEvent {
    uint32_t type;
    uint32_t timestamp;
    uint8_t state;
    uint8_t repeat;
    uint8_t padding2;
    uint8_t padding3;
    SDL_Keysym keysym;
} SDL_KeyboardEvent;

typedef struct SDL_MouseButtonEvent {
    uint32_t type;
    uint32_t timestamp;
    uint32_t windowID;
    uint8_t which;
    uint8_t button;
    uint8_t state;
    uint8_t clicks;
    int32_t x;
    int32_t y;
} SDL_MouseButtonEvent;

typedef struct SDL_MouseMotionEvent {
    uint32_t type;
    uint32_t timestamp;
    uint32_t windowID;
    int32_t x;
    int32_t y;
} SDL_MouseMotionEvent;

typedef struct SDL_WindowEvent {
    uint32_t type;
    uint32_t timestamp;
    uint32_t windowID;
    uint8_t event;
    uint8_t padding1;
    uint8_t padding2;
    uint8_t padding3;
    int32_t data1;
    int32_t data2;
} SDL_WindowEvent;

typedef union SDL_Event {
    uint32_t type;
    SDL_KeyboardEvent key;
    SDL_MouseButtonEvent button;
    SDL_MouseMotionEvent motion;
    SDL_WindowEvent window;
} SDL_Event;

/* Audio format */
typedef struct SDL_AudioSpec {
    int freq;
    uint16_t format;
    uint8_t channels;
    uint8_t silence;
    uint16_t samples;
    uint16_t padding;
    uint32_t size;
    void (*callback)(void* userdata, Uint8* stream, int len);
    void* userdata;
} SDL_AudioSpec;

/* Use real SDL2 functions instead of mock stubs */
extern int SDL_Init(uint32_t flags);
extern void SDL_Quit(void);
extern const char* SDL_GetError(void);
extern int SDL_SetHint(const char* name, const char* value);
extern SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int w, int h, uint32_t flags);
extern void SDL_DestroyWindow(SDL_Window* window);
extern SDL_Renderer* SDL_CreateRenderer(SDL_Window* window, int index, uint32_t flags);
extern void SDL_DestroyRenderer(SDL_Renderer* renderer);
extern int SDL_RenderSetLogicalSize(SDL_Renderer* renderer, int w, int h);
extern SDL_Texture* SDL_CreateTexture(SDL_Renderer* renderer, uint32_t format, int access, int w, int h);
extern void SDL_DestroyTexture(SDL_Texture* texture);
extern int SDL_LockTexture(SDL_Texture* texture, void* rect, void** pixels, int* pitch);
extern void SDL_UnlockTexture(SDL_Texture* texture);
extern int SDL_RenderCopy(SDL_Renderer* renderer, SDL_Texture* texture, void* src, void* dst);
extern void SDL_RenderPresent(SDL_Renderer* renderer);
extern int SDL_RenderClear(SDL_Renderer* renderer);
extern int SDL_PollEvent(SDL_Event* event);
extern void SDL_PumpEvents(void);

/* Static keyboard state for mock */
static Uint8 mock_keyboard_state[512] = {0};
extern const Uint8* SDL_GetKeyboardState(int* numkeys); 

static inline uint64_t SDL_GetPerformanceCounter(void) { return 0; }
static inline uint64_t SDL_GetPerformanceFrequency(void) { return 1000000; }
static inline void SDL_Delay(uint32_t ms) { (void)ms; }
static inline uint32_t SDL_GetTicks(void) { return 0; }
static inline int SDL_UpdateTexture(SDL_Texture* texture, void* rect, const void* pixels, int pitch) {
    (void)texture; (void)rect; (void)pixels; (void)pitch; return 0;
}

/* Window functions */
static inline void SDL_SetWindowTitle(SDL_Window* window, const char* title) { (void)window; (void)title; }
static inline int SDL_SetWindowFullscreen(SDL_Window* window, uint32_t flags) { (void)window; (void)flags; return 0; }
static inline uint32_t SDL_GetWindowFlags(SDL_Window* window) { (void)window; return 0; }
static inline void SDL_GetWindowSize(SDL_Window* window, int* w, int* h) { (void)window; *w = 240; *h = 320; }

/* Audio - use real SDL2 functions, not mock stubs */
/* The mock stubs were preventing real SDL2 audio from working */
extern SDL_AudioDeviceID SDL_OpenAudioDevice(const char* device, int iscapture,
    const SDL_AudioSpec* desired, SDL_AudioSpec* obtained, int allowed_changes);
extern void SDL_CloseAudioDevice(SDL_AudioDeviceID dev);
extern int SDL_QueueAudio(SDL_AudioDeviceID dev, const void* data, uint32_t len);
extern uint32_t SDL_GetQueuedAudioSize(SDL_AudioDeviceID dev);
extern void SDL_ClearQueuedAudio(SDL_AudioDeviceID dev);
extern void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on);

/* Surface */
struct SDL_Surface {
    int w, h;
    int pitch;
    void* pixels;
};
static inline SDL_Surface* SDL_CreateRGBSurface(uint32_t flags, int w, int h, int depth,
    uint32_t rmask, uint32_t gmask, uint32_t bmask, uint32_t amask) {
    (void)flags; (void)depth; (void)rmask; (void)gmask; (void)bmask; (void)amask;
    SDL_Surface* s = (SDL_Surface*)malloc(sizeof(SDL_Surface));
    if (s) { s->w = w; s->h = h; s->pitch = w * 4; s->pixels = malloc(w * h * 4); }
    return s;
}
static inline void SDL_FreeSurface(SDL_Surface* surface) { 
    if (surface) { free(surface->pixels); free(surface); }
}
static inline int SDL_RenderReadPixels(SDL_Renderer* renderer, void* rect, uint32_t format, void* pixels, int pitch) {
    (void)renderer; (void)rect; (void)format; (void)pixels; (void)pitch; return 0;
}
static inline int SDL_SaveBMP(SDL_Surface* surface, const char* file) { (void)surface; (void)file; return 0; }

/* Video driver */
static inline const char* SDL_GetCurrentVideoDriver(void) { return "mock"; }

#endif /* MOCK_SDL_H */
