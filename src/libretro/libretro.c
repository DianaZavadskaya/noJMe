/*
 * J2ME Emulator - Libretro Core Implementation
 * Compatible with MinArch frontend
 */

/* BUILD_ID - increment this when making changes to verify correct version is running */
#define J2ME_BUILD_ID "2024.01.30.V15-REAL-TIME"

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>

#include "debug.h"
#include "debug_macros.h"
#include "libretro.h"
#include "libretro_shared.h"
#include "jvm.h"
#include "midp.h"
#include "native.h"
#include "opcodes.h"
#include "sdl_backend.h"
#include "miniz.h"  /* For JAR decompression */

/* Forward declaration for step-by-step execution */
extern int execute_frame(JVM* jvm, JavaThread* thread);

/* Forward declaration for audio processing */
extern void libretro_set_sample_rate(uint32_t rate);
extern void libretro_set_fps(int fps);
extern void libretro_process_audio(void);

/* Forward declaration for frame management */
extern void libretro_begin_frame(void);
extern void libretro_end_frame(void);
extern bool libretro_get_display_buffer(uint32_t** buffer, int* width, int* height);

/* Forward declarations for MIDP key events */
extern void midp_call_keyPressed(JVM* jvm, int keycode);
extern void midp_call_keyReleased(JVM* jvm, int keycode);

/* Forward declarations for RMS persistence */
extern void midp_rms_set_save_path(const char* save_dir, const char* game_name);
extern void midp_rms_save_all(void);

/* CRITICAL: External functions for J2ME event loop */
extern void midp_process_repaints(JVM* jvm);

/* ============================================
 * JAR/ZIP reading functions
 * ============================================ */

static uint16_t jar_read_u16(const uint8_t* p) {
    return p[0] | (p[1] << 8);
}

static uint32_t jar_read_u32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

static size_t jar_find_eocd(const uint8_t* data, size_t size) {
    if (size < 22) return 0;
    size_t max_comment = 65535 + 22;
    size_t search_start = size > max_comment ? size - max_comment : 0;
    
    for (size_t i = size - 22; i >= search_start && i > 0; i--) {
        if (data[i] == 0x50 && data[i+1] == 0x4B &&
            data[i+2] == 0x05 && data[i+3] == 0x06) {
            return i;
        }
    }
    return 0;
}

static uint8_t* jar_extract_file(const uint8_t* jar_data, size_t jar_size,
                                  const char* filename, size_t* out_size) {
    *out_size = 0;
    
    size_t eocd_offset = jar_find_eocd(jar_data, jar_size);
    if (eocd_offset == 0) return NULL;
    
    uint32_t cd_start = jar_read_u32(jar_data + eocd_offset + 16);
    uint16_t cd_entries = jar_read_u16(jar_data + eocd_offset + 10);
    
    size_t cde_offset = cd_start;
    for (int i = 0; i < cd_entries && cde_offset < eocd_offset; i++) {
        uint32_t cd_sig = jar_read_u32(jar_data + cde_offset);
        if (cd_sig != 0x02014B50) break;
        
        uint16_t compression = jar_read_u16(jar_data + cde_offset + 10);
        uint32_t comp_size = jar_read_u32(jar_data + cde_offset + 20);
        uint32_t uncomp_size = jar_read_u32(jar_data + cde_offset + 24);
        uint16_t filename_len = jar_read_u16(jar_data + cde_offset + 28);
        uint16_t extra_len = jar_read_u16(jar_data + cde_offset + 30);
        uint16_t comment_len = jar_read_u16(jar_data + cde_offset + 32);
        uint32_t local_header_offset = jar_read_u32(jar_data + cde_offset + 42);
        
        const char* entry_name = (const char*)(jar_data + cde_offset + 46);
        bool is_dir = (filename_len > 0 && entry_name[filename_len - 1] == '/');
        
        if (!is_dir && filename_len == strlen(filename) &&
            memcmp(entry_name, filename, filename_len) == 0) {
            
            uint16_t local_filename_len = jar_read_u16(jar_data + local_header_offset + 26);
            uint16_t local_extra_len = jar_read_u16(jar_data + local_header_offset + 28);
            size_t data_offset = local_header_offset + 30 + local_filename_len + local_extra_len;
            
            if (compression == 0) {
                *out_size = uncomp_size;
                uint8_t* data = (uint8_t*)malloc(*out_size + 1);
                if (data) {
                    memcpy(data, jar_data + data_offset, *out_size);
                    data[*out_size] = '\0';
                }
                return data;
            } else if (compression == 8) {
                uint8_t* data = (uint8_t*)malloc(uncomp_size + 1);
                if (!data) return NULL;
                
                z_stream stream;
                memset(&stream, 0, sizeof(stream));
                
                int ret = inflateInit2(&stream, -MAX_WBITS);
                if (ret != Z_OK) {
                    free(data);
                    return NULL;
                }
                
                stream.next_in = (Bytef*)(jar_data + data_offset);
                stream.avail_in = comp_size;
                stream.next_out = data;
                stream.avail_out = uncomp_size;
                
                ret = inflate(&stream, Z_FINISH);
                inflateEnd(&stream);
                
                if (ret != Z_STREAM_END) {
                    free(data);
                    return NULL;
                }
                
                data[uncomp_size] = '\0';
                *out_size = uncomp_size;
                return data;
            }
        }
        
        cde_offset += 46 + filename_len + extra_len + comment_len;
    }
    
    return NULL;
}

static char* find_midlet_class_in_jar(const uint8_t* jar_data, size_t jar_size) {
    size_t manifest_size;
    uint8_t* manifest = jar_extract_file(jar_data, jar_size, "META-INF/MANIFEST.MF", &manifest_size);
    
    if (!manifest) {
        manifest = jar_extract_file(jar_data, jar_size, "META-INF/manifest.mf", &manifest_size);
    }
    
    if (!manifest) return NULL;
    
    char* result = NULL;
    char* manifest_str = (char*)manifest;
    
    char* line = strtok(manifest_str, "\r\n");
    while (line) {
        if (strncmp(line, "MIDlet-1:", 9) == 0 || strncmp(line, "MIDlet-1 :", 10) == 0) {
            char* start = strchr(line, ':');
            if (start) {
                start++;
                char* comma1 = strchr(start, ',');
                if (comma1) {
                    /* Skip whitespace after first comma */
                    char* after_comma1 = comma1 + 1;
                    while (*after_comma1 == ' ') after_comma1++;
                    
                    /* Try to find a second comma (format: Name, Icon, Class)
                     * If not found, use text after first comma (format: Name, Class) */
                    char* comma2 = strchr(after_comma1, ',');
                    char* class_start;
                    if (comma2) {
                        /* 3-field format: Name, Icon, Class */
                        class_start = comma2 + 1;
                        while (*class_start == ' ') class_start++;
                    } else {
                        /* 2-field format: Name, Class (no icon) */
                        class_start = after_comma1;
                    }
                    
                    char* end = class_start;
                    while (*end && *end != '\r' && *end != '\n' && *end != ',') end++;
                    while (end > class_start && (end[-1] == ' ' || end[-1] == '\t')) end--;
                    
                    size_t len = end - class_start;
                    if (len > 0) {
                        result = (char*)malloc(len + 1);
                        if (result) {
                            memcpy(result, class_start, len);
                            result[len] = '\0';
                        }
                    }
                    break;
                }
            }
        }
        line = strtok(NULL, "\r\n");
    }
    
    free(manifest);
    return result;
}

/* Define missing constants */
#ifndef RETRO_REGION_NTSC
#define RETRO_REGION_NTSC 0
#endif

#ifndef RETRO_REGION_PAL
#define RETRO_REGION_PAL 1
#endif

/* ============================================
 * Global State
 * ============================================ */

JVM* g_jvm = NULL;  /* Non-static for sdl_backend_stubs.c */
static uint8_t* g_jar_data = NULL;
static size_t g_jar_size = 0;
static char g_midlet_class[256] = {0};
static char g_game_name[256] = {0};  /* Base name of loaded JAR for RMS saves */

/* Framebuffer - dynamic allocation for flexible screen sizes */
static uint32_t* g_framebuffer = NULL;  /* XRGB8888 format (internal) */
static uint16_t* g_rgb565_buffer = NULL;  /* RGB565 format (for frontend) */
static int g_screen_width = 240;
static int g_screen_height = 320;
static int g_screen_pitch = 240 * 4;  /* 240 * 4 bytes per pixel (XRGB8888) */
static size_t g_framebuffer_size = 0;
static bool g_use_rgb565 = true;  /* Default to RGB565 for MinArch */

/* Runtime settings - configurable */
int g_target_fps = 30;  /* Non-static for sdl_backend_stubs.c */
static int g_audio_sample_rate = 22050;

/* Keys */
static int g_key_states = 0;
static int g_prev_key_states = 0;  /* Previous frame key states for edge detection */

/* Keyboard key states for numeric/special keys not mapped to joypad */
static int g_kb_key_states = 0;       /* Current keyboard extra keys */
static int g_kb_prev_key_states = 0;  /* Previous frame */

/* Key repeat support - on real J2ME phones, holding a key generates repeated
 * keyPressed events after an initial delay. Many Canvas games rely on this
 * for movement instead of polling getKeyStates(). */
#define KEY_REPEAT_INITIAL_DELAY_MS  400   /* Delay before first repeat (ms) */
#define KEY_REPEAT_INTERVAL_MS        80   /* Interval between repeats (ms) */
#define KEY_REPEAT_MAX_BIT           17    /* Highest bit number in key_map */

static struct {
    uint64_t press_time[KEY_REPEAT_MAX_BIT + 1];     /* Timestamp of initial press */
    uint64_t last_repeat_time[KEY_REPEAT_MAX_BIT + 1]; /* Timestamp of last repeat */
    bool is_held[KEY_REPEAT_MAX_BIT + 1];            /* Key is currently held down */
} g_key_repeat = {{0}, {0}, {false}};

/* Reset key repeat state for a specific bit */
static void key_repeat_reset(int bit) {
    if (bit >= 0 && bit <= KEY_REPEAT_MAX_BIT) {
        g_key_repeat.is_held[bit] = false;
        g_key_repeat.press_time[bit] = 0;
        g_key_repeat.last_repeat_time[bit] = 0;
    }
}

/* Keyboard extra key mapping: bit position -> {retro_key, midp_keycode} */
static const struct {
    int bit;
    unsigned retro_key;   /* RETROK_* constant */
    int keycode;           /* MIDP keyCode (ASCII value) */
} kb_key_map[] = {
    { 0, RETROK_0,       48},  /* '0' */
    { 1, RETROK_6,       54},  /* '6' */
    { 2, RETROK_7,       55},  /* '7' */
    { 3, RETROK_8,       56},  /* '8' */
    { 4, RETROK_9,       57},  /* '9' */
    { 5, RETROK_ASTERISK, 42},  /* '*' */
    { 6, RETROK_HASH,     35},  /* '#' */
};
#define KB_KEY_MAP_SIZE (sizeof(kb_key_map) / sizeof(kb_key_map[0]))

/* Key mapping: bit position -> MIDP keyCode */
static const struct {
    int bit;
    int keycode;
    int game_action;
} key_map[] = {
    { 1,  -1, 1},   /* UP -> keyCode -1, gameAction UP=1 */
    { 2,  -3, 2},   /* LEFT -> keyCode -3, gameAction LEFT=2 */
    { 5,  -4, 5},   /* RIGHT -> keyCode -4, gameAction RIGHT=5 */
    { 6,  -2, 6},   /* DOWN -> keyCode -2, gameAction DOWN=6 */
    { 8,  -5, 8},   /* FIRE -> keyCode -5, gameAction FIRE=8 */
    { 9,  -6, 0},   /* SELECT -> Left soft key -6 */
    { 10, -7, 0},   /* START -> Right soft key -7 */
    { 11, -8, 0},   /* B button -> GAME_C -8 */
    { 12, -9, 0},   /* X button -> GAME_D -9 */
    { 13, 49, 0},   /* Y button -> Key '1' (keyCode 49) */
    { 14, 50, 0},   /* L1 button -> Key '2' (keyCode 50) */
    { 15, 51, 0},   /* R1 button -> Key '3' (keyCode 51) */
    { 16, 52, 0},   /* L2 button -> Key '4' (keyCode 52) */
    { 17, 53, 0},   /* R2 button -> Key '5' (keyCode 53) */
};
#define KEY_MAP_SIZE (sizeof(key_map) / sizeof(key_map[0]))

/* Libretro callbacks */
retro_environment_t environ_cb = NULL;
retro_video_refresh_t video_cb = NULL;
retro_audio_sample_batch_t audio_batch_cb = NULL;
retro_input_poll_t input_poll_cb = NULL;
retro_input_state_t input_state_cb = NULL;
static retro_log_printf_t log_cb = NULL;

/* ============================================
 * Option Values Local Cache
 * ============================================ */
#define MAX_OPTIONS 16
typedef struct {
    char key[64];
    char value[128];
} StoredOption;

static StoredOption g_stored_options[MAX_OPTIONS];
static int g_stored_options_count = 0;

/* Store an option value locally */
static void store_option_value(const char* key, const char* value) {
    if (!key || !value) return;
    
    LOG_SAFE("[J2ME] CACHE: Storing '%s' = '%s'\n", key, value);
    
    /* Check if key already exists */
    for (int i = 0; i < g_stored_options_count; i++) {
        if (strcmp(g_stored_options[i].key, key) == 0) {
            strncpy(g_stored_options[i].value, value, sizeof(g_stored_options[i].value) - 1);
            g_stored_options[i].value[sizeof(g_stored_options[i].value) - 1] = '\0';
            return;
        }
    }
    
    /* Add new option */
    if (g_stored_options_count < MAX_OPTIONS) {
        strncpy(g_stored_options[g_stored_options_count].key, key, sizeof(g_stored_options[0].key) - 1);
        g_stored_options[g_stored_options_count].key[sizeof(g_stored_options[0].key) - 1] = '\0';
        strncpy(g_stored_options[g_stored_options_count].value, value, sizeof(g_stored_options[0].value) - 1);
        g_stored_options[g_stored_options_count].value[sizeof(g_stored_options[0].value) - 1] = '\0';
        g_stored_options_count++;
    }
}

/* Get a stored option value */
static const char* get_stored_option_value(const char* key) {
    if (!key) return NULL;
    
    for (int i = 0; i < g_stored_options_count; i++) {
        if (strcmp(g_stored_options[i].key, key) == 0) {
            return g_stored_options[i].value;
        }
    }
    return NULL;
}

/* Time */
static uint64_t last_frame_time = 0;
static uint64_t frame_accumulator = 0;

/* ============================================
 * RGB565 Conversion
 * ============================================ */

static void convert_xrgb8888_to_rgb565(const uint32_t* src, uint16_t* dst, int width, int height) {
    int size = width * height;
    for (int i = 0; i < size; i++) {
        uint32_t pixel = src[i];
        uint8_t r = (pixel >> 16) & 0xFF;
        uint8_t g = (pixel >> 8) & 0xFF;
        uint8_t b = pixel & 0xFF;
        dst[i] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    }
}

/* ============================================
 * Time Functions
 * ============================================ */

static uint64_t millis(void) {
    struct timeval time;
    gettimeofday(&time, NULL);
    return time.tv_sec * 1000 + time.tv_usec / 1000;
}

/* ============================================
 * Logging
 * ============================================ */

static void fallback_log(enum retro_log_level level, const char *fmt, ...) {
    (void)level;
    va_list va;
    va_start(va, fmt);
    vfprintf(stderr, fmt, va);
    va_end(va);
}

static void log_message(enum retro_log_level level, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    
    if (log_cb) {
        char buffer[1024];
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        log_cb(level, "%s", buffer);
    } else {
        vfprintf(stderr, fmt, args);
    }
    
    va_end(args);
}

/* ============================================
 * Functions for stubs access
 * ============================================ */

static void update_variables(const char* game_path);

uint32_t* libretro_get_framebuffer(void) {
    return g_framebuffer;
}

int libretro_get_screen_width(void) {
    return g_screen_width;
}

int libretro_get_screen_height(void) {
    return g_screen_height;
}

void libretro_set_key_states(int states) {
    g_key_states = states;
}

int libretro_get_key_states(void) {
    return g_key_states;
}

/* ============================================
 * JVM Initialization
 * ============================================ */

static bool init_emulator(void) {
    log_message(RETRO_LOG_INFO, "[J2ME] Initializing emulator...\n");
    
    g_jvm = jvm_create();
    LOG_SAFE("[J2ME] init_emulator: jvm_create() returned %p\n", (void*)g_jvm);
    if (!g_jvm) {
        log_message(RETRO_LOG_ERROR, "[J2ME] Failed to create JVM\n");
        return false;
    }
    
    g_jvm->config.heap_size = 16 * 1024 * 1024;
    g_jvm->config.stack_size = 64 * 1024;
    g_jvm->config.max_threads = 8;
    g_jvm->config.verbose_class = false;
    
    LOG_SAFE("[J2ME] init_emulator: calling jvm_init()...\n");
    if (jvm_init(g_jvm) != JNI_OK) {
        log_message(RETRO_LOG_ERROR, "[J2ME] JVM init failed\n");
        jvm_destroy(g_jvm);
        g_jvm = NULL;
        return false;
    }
    LOG_SAFE("[J2ME] init_emulator: jvm_init() OK\n");
    
    LOG_SAFE("[J2ME] Calling sdl_init with %dx%d\n", g_screen_width, g_screen_height);
    if (sdl_init(g_jvm, g_screen_width, g_screen_height, 1, true) != 0) {
        log_message(RETRO_LOG_ERROR, "[J2ME] sdl_init failed\n");
        jvm_destroy(g_jvm);
        g_jvm = NULL;
        return false;
    }
    LOG_SAFE("[J2ME] sdl_init completed\n");
    
    LOG_SAFE("[J2ME] init_emulator: calling native_init()...\n");
    if (native_init(g_jvm) != JNI_OK) {
        log_message(RETRO_LOG_ERROR, "[J2ME] native_init failed\n");
        jvm_destroy(g_jvm);
        g_jvm = NULL;
        return false;
    }
    LOG_SAFE("[J2ME] init_emulator: native_init() OK\n");
    
    LOG_SAFE("[J2ME] init_emulator: calling midp_init()...\n");
    if (midp_init(g_jvm) != JNI_OK) {
        log_message(RETRO_LOG_ERROR, "[J2ME] midp_init failed\n");
        jvm_destroy(g_jvm);
        g_jvm = NULL;
        return false;
    }
    LOG_SAFE("[J2ME] init_emulator: midp_init() OK\n");
    
    LOG_SAFE("[J2ME] init_emulator: calling opcodes_init()...\n");
    opcodes_init();
    LOG_SAFE("[J2ME] init_emulator: opcodes_init() OK\n");
    
    log_message(RETRO_LOG_INFO, "[J2ME] Emulator initialized\n");
    return true;
}

/* ============================================
 * MIDlet Execution
 * ============================================ */

/* Forward declarations for error display */
extern void sdl_set_error_info(const char* title, const char* message, const char* stack_trace);
extern void sdl_set_error_extra(const char* extra);

/* Build an informative exception title with full class hierarchy */
static void build_exception_title(JavaObject* exception, char* out, int out_size) {
    if (!exception || !exception->header.clazz) {
        strncpy(out, "Unknown Exception", out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }
    
    JavaClass* cls = exception->header.clazz;
    const char* name = cls->class_name ? cls->class_name : "Unknown";
    
    /* Convert java/lang/NullPointerException to NullPointerException */
    const char* slash = strrchr(name, '/');
    const char* short_name = slash ? slash + 1 : name;
    
    /* Try to walk superclass chain for additional context */
    char chain[512] = {0};
    JavaClass* super = cls->super_class;
    int depth = 0;
    while (super && depth < 3) {
        if (super->class_name && strcmp(super->class_name, "java/lang/Object") != 0 &&
            strcmp(super->class_name, "java/lang/Throwable") != 0 &&
            strcmp(super->class_name, "java/lang/Exception") != 0 &&
            strcmp(super->class_name, "java/lang/Error") != 0 &&
            strcmp(super->class_name, "java/lang/Runtime") != 0) {
            const char* ss = strrchr(super->class_name, '/');
            const char* sn = ss ? ss + 1 : super->class_name;
            if (chain[0]) strncat(chain, " > ", sizeof(chain) - strlen(chain) - 1);
            strncat(chain, sn, sizeof(chain) - strlen(chain) - 1);
            depth++;
            super = super->super_class;
        } else {
            break;
        }
    }
    
    if (chain[0]) {
        snprintf(out, out_size, "%s [%s]", short_name, chain);
    } else {
        strncpy(out, short_name, out_size - 1);
        out[out_size - 1] = '\0';
    }
}

/* Build detailed stack trace with PC and descriptor info */
static int build_stack_trace(JavaThread* thread, char* out, int out_size) {
    if (!thread || !thread->current_frame) {
        out[0] = '\0';
        return 0;
    }
    
    JavaFrame* frame = thread->current_frame;
    int pos = 0;
    int frame_count = 0;
    int max_frames = 25;
    
    /* First frame is the one where exception was thrown - mark it with >>> */
    while (frame && frame_count < max_frames && pos < out_size - 2) {
        const char* cls_name = frame->clazz && frame->clazz->class_name ? frame->clazz->class_name : "?";
        const char* method_name = frame->method && frame->method->name ? frame->method->name : "?";
        const char* descriptor = frame->method && frame->method->descriptor ? frame->method->descriptor : "";
        
        /* Skip internal/vm methods for clarity */
        if (cls_name[0] == '?' && method_name[0] == '?') {
            frame = frame->prev;
            continue;
        }
        
        /* Convert class name: java/lang/String -> String */
        const char* slash = strrchr(cls_name, '/');
        const char* short_cls = slash ? slash + 1 : cls_name;
        
        int written;
        if (frame_count == 0) {
            /* Throwing frame - highlight */
            written = snprintf(out + pos, out_size - pos - 1,
                    ">%s.%s(%s) PC=%d\n",
                    short_cls, method_name, descriptor, frame->throwing_pc);
        } else {
            written = snprintf(out + pos, out_size - pos - 1,
                    "  at %s.%s(%s)\n",
                    short_cls, method_name, descriptor);
        }
        
        if (written > 0 && pos + written < out_size - 1) {
            pos += written;
        } else {
            break;
        }
        
        frame = frame->prev;
        frame_count++;
    }
    
    out[pos] = '\0';
    return frame_count;
}

static bool run_midlet(void) {
    if (!g_jvm || !g_jar_data) return false;
    
    log_message(RETRO_LOG_INFO, "[J2ME] Starting MIDlet: %s\n", g_midlet_class);
    
    LOG_SAFE("[J2ME] run_midlet: calling jvm_set_jar_data() (jar_size=%zu)...\n", g_jar_size);
    jvm_set_jar_data(g_jvm, g_jar_data, g_jar_size, "game.jar");
    LOG_SAFE("[J2ME] run_midlet: jvm_set_jar_data() OK\n");
    
    char class_name[256];
    strncpy(class_name, g_midlet_class, sizeof(class_name) - 1);
    class_name[sizeof(class_name) - 1] = '\0';
    
    for (char* p = class_name; *p; p++) {
        if (*p == '.') *p = '/';
    }
    
    LOG_SAFE("[J2ME] run_midlet: loading class '%s'...\n", class_name);
    JavaClass* main_class = jvm_load_class(g_jvm, class_name);
    LOG_SAFE("[J2ME] run_midlet: jvm_load_class() returned %p\n", (void*)main_class);
    if (!main_class) {
        log_message(RETRO_LOG_ERROR, "[J2ME] Failed to load class %s\n", class_name);
        sdl_set_error_info("Class Not Found", class_name, "The MIDlet main class could not be loaded from the JAR file.");
        return false;
    }
    
    LOG_SAFE("[J2ME] run_midlet: calling jvm_run_midlet()...\n");
    int result = jvm_run_midlet(g_jvm, main_class);
    LOG_SAFE("[J2ME] run_midlet: jvm_run_midlet() returned %d\n", result);
    if (result != 0) {
        log_message(RETRO_LOG_ERROR, "[J2ME] MIDlet execution failed: %d\n", result);
        
        /* Check for pending exception and extract error info */
        JavaThread* main_thread = g_jvm->main_thread;
        if (main_thread && main_thread->pending_exception) {
            JavaObject* exception = main_thread->pending_exception;
            char exc_title[512] = {0};
            char message[512] = {0};
            char stack_trace[8192] = {0};
            
            /* Build informative exception title with hierarchy */
            build_exception_title(exception, exc_title, sizeof(exc_title));
            
            /* Look for detailMessage field in exception object */
            JavaClass* exc_class = exception->header.clazz;
            if (exc_class && exc_class->fields) {
                for (uint16_t i = 0; i < exc_class->fields_count; i++) {
                    JavaField* field = &exc_class->fields[i];
                    if (field->name && strcmp(field->name, "detailMessage") == 0) {
                        JavaValue* field_val = (JavaValue*)((uint8_t*)exception + sizeof(ObjectHeader) + i * sizeof(JavaValue));
                        if (field_val && field_val->ref) {
                            JavaObject* str_obj = (JavaObject*)field_val->ref;
                            if (str_obj->header.clazz && str_obj->header.clazz->class_name &&
                                strcmp(str_obj->header.clazz->class_name, "java/lang/String") == 0) {
                                JavaValue* value_field = (JavaValue*)((uint8_t*)str_obj + sizeof(ObjectHeader));
                                JavaValue* count_field = (JavaValue*)((uint8_t*)str_obj + sizeof(ObjectHeader) + sizeof(JavaValue));
                                if (value_field->ref && count_field->i > 0) {
                                    JavaObject* char_array = (JavaObject*)value_field->ref;
                                    jchar* chars = (jchar*)((uint8_t*)char_array + sizeof(ObjectHeader));
                                    int len = count_field->i < 250 ? count_field->i : 250;
                                    for (int j = 0; j < len; j++) {
                                        message[j] = (char)(chars[j] & 0xFF);
                                    }
                                    message[len] = '\0';
                                }
                            }
                        }
                        break;
                    }
                }
            }
            
            /* Build detailed stack trace - prefer saved trace over current frame chain */
            int frames = 0;
            if (main_thread->exception_stack_trace && main_thread->exception_stack_trace[0]) {
                /* Use the saved stack trace from when exception was first thrown */
                strncpy(stack_trace, main_thread->exception_stack_trace, sizeof(stack_trace) - 1);
                stack_trace[sizeof(stack_trace) - 1] = '\0';
                /* Count frames in the trace */
                for (const char* p = stack_trace; *p; p++) {
                    if (*p == '\n') frames++;
                }
            } else {
                /* Fallback: try to build from current frame chain */
                frames = build_stack_trace(main_thread, stack_trace, sizeof(stack_trace));
            }
            
            /* Set extra info: thread, frame count, and throw location */
            char extra[512];
            if (main_thread->exception_throw_info && main_thread->exception_throw_info[0]) {
                snprintf(extra, sizeof(extra), "Thread: main | %d frames | thrown at %s", 
                         frames, main_thread->exception_throw_info);
            } else {
                snprintf(extra, sizeof(extra), "Thread: main | %d frames", frames);
            }
            sdl_set_error_extra(extra);
            
            log_message(RETRO_LOG_ERROR, "[J2ME] Uncaught %s: %s\n", exc_title, message[0] ? message : "(no message)");
            
            /* Log the stack trace to stderr for debugging */
            if (stack_trace[0]) {
                LOG_SAFE("  Stack trace:\n%s", stack_trace);
            }
            
            sdl_set_error_info(exc_title, message[0] ? message : NULL, stack_trace[0] ? stack_trace : NULL);
        } else {
            sdl_set_error_info("Execution Failed", "MIDlet returned error code", NULL);
            log_message(RETRO_LOG_ERROR, "[J2ME] MIDlet failed with code %d (no exception object)\n", result);
        }
        
        return false;
    }
    
    log_message(RETRO_LOG_INFO, "[J2ME] MIDlet started\n");
    return true;
}

/* ============================================
 * Libretro API
 * ============================================ */

unsigned retro_api_version(void) {
    return RETRO_API_VERSION;
}

void retro_init(void) {
    /* Initialize logging mutex FIRST (before any LOG_SAFE calls) */
    log_mutex_init();
    
    LOG_SAFE("[J2ME] ========================================\n");
    LOG_SAFE("[J2ME] BUILD_ID: %s\n", J2ME_BUILD_ID);
    LOG_SAFE("[J2ME] ========================================\n");
    LOG_SAFE("[J2ME] retro_init\n");
    
    g_framebuffer_size = g_screen_width * g_screen_height * sizeof(uint32_t);
    g_framebuffer = (uint32_t*)malloc(g_framebuffer_size);
    if (!g_framebuffer) {
        LOG_SAFE("[J2ME] Failed to allocate framebuffer\n");
        return;
    }
    memset(g_framebuffer, 0, g_framebuffer_size);
    g_screen_pitch = g_screen_width * 4;
    
    size_t rgb565_size = g_screen_width * g_screen_height * sizeof(uint16_t);
    g_rgb565_buffer = (uint16_t*)malloc(rgb565_size);
    if (!g_rgb565_buffer) {
        LOG_SAFE("[J2ME] Failed to allocate RGB565 buffer\n");
        free(g_framebuffer);
        g_framebuffer = NULL;
        return;
    }
    memset(g_rgb565_buffer, 0, rgb565_size);
    
    LOG_SAFE("[J2ME] Buffers allocated: XRGB8888=%p, RGB565=%p\n", 
            (void*)g_framebuffer, (void*)g_rgb565_buffer);
    
    uint64_t current_time = millis();
    last_frame_time = current_time;
    frame_accumulator = 0;
}

void retro_deinit(void) {
    LOG_SAFE("[J2ME] retro_deinit\n");
    
    if (g_jvm) {
        jvm_destroy(g_jvm);
        g_jvm = NULL;
    }
    
    if (g_jar_data) {
        free(g_jar_data);
        g_jar_data = NULL;
    }
    
    if (g_framebuffer) {
        free(g_framebuffer);
        g_framebuffer = NULL;
    }
    
    if (g_rgb565_buffer) {
        free(g_rgb565_buffer);
        g_rgb565_buffer = NULL;
    }
    
    g_framebuffer_size = 0;
}

void retro_set_controller_port_device(unsigned port, unsigned device) {
    (void)port;
    (void)device;
}

void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof(*info));
    info->library_name = "J2ME";
    info->library_version = "1.0";
    info->need_fullpath = true;
    info->valid_extensions = "jar";
}

void retro_get_system_av_info(struct retro_system_av_info *info) {
    info->timing.fps = (double)g_target_fps;
    info->timing.sample_rate = (double)g_audio_sample_rate;
    
    info->geometry.base_width = g_screen_width;
    info->geometry.base_height = g_screen_height;
    info->geometry.max_width = 640;
    info->geometry.max_height = 800;
    info->geometry.aspect_ratio = (float)g_screen_width / (float)g_screen_height;
}

/* ============================================
 * Core Options - Legacy Format for MinArch
 * ============================================ */

/* Legacy format for MinArch frontend */
/* Added support for 240x136 and 480x272 resolutions */
static struct retro_variable g_core_variables[] = {
    { "j2me_resolution", "Screen Resolution; 240x320|240x136|480x272|320x480|360x640|480x800|176x208|128x128" },
    { "j2me_fps", "Frame Rate; 30|60|15|20" },
    { "j2me_audio_rate", "Audio Sample Rate; 22050|44100|11025" },
    { "j2me_scaling", "Screen Scaling; Aspect|Integer|Stretch" },
    { NULL, NULL }
};

/* Controller info */
static const struct retro_controller_description g_controllers[] = {
    { "RetroPad", RETRO_DEVICE_JOYPAD },
    { NULL, 0 }
};

static const struct retro_controller_info g_ports[] = {
    { g_controllers, 1 },
    { NULL, 0 }
};

/* Input descriptors */
static const struct retro_input_descriptor g_input_desc[] = {
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Up" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Down" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Left" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Right" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "Fire" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Game C" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Game D" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Key 1" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Left Soft" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Right Soft" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Key 2" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Key 3" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "Key 4" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Key 5" },
    { 0, 0, 0, 0, NULL }
};

/* Helper function to set core options - LEGACY ONLY for MinArch */
static void libretro_set_core_options(retro_environment_t cb) {
    if (!cb) return;

    LOG_SAFE("[J2ME] Using LEGACY SET_VARIABLES (cmd 16) - MinArch compatible\n");

    if (cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)g_core_variables)) {
        LOG_SAFE("[J2ME] SET_VARIABLES succeeded\n");
    } else {
        LOG_SAFE("[J2ME] WARNING: SET_VARIABLES failed!\n");
    }
}

void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;

    LOG_SAFE("[J2ME] retro_set_environment called, cb=%p\n", (void*)cb);

    /* 1. Get log interface */
    struct retro_log_callback logging;
    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)) {
        log_cb = logging.log;
        LOG_SAFE("[J2ME] Log interface acquired\n");
    }

    /* 2. Set core options */
    libretro_set_core_options(cb);
    LOG_SAFE("[J2ME] Core options set\n");

    /* 3. Set controller info */
    cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)g_ports);
    LOG_SAFE("[J2ME] Controller info set\n");

    /* 4. Set input descriptors */
    cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void*)g_input_desc);
    LOG_SAFE("[J2ME] Input descriptors set\n");

    /* 5. No content mode */
    bool no_content = false;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_content);

    LOG_SAFE("[J2ME] retro_set_environment complete\n");
}

/* Update variables from frontend */
static void update_variables(const char* game_path) {
    struct retro_variable var = {0};
    (void)game_path;
    int old_width = g_screen_width;
    int old_height = g_screen_height;
    int old_fps = g_target_fps;
    int old_audio = g_audio_sample_rate;

    if (!environ_cb) {
        LOG_SAFE("[J2ME] ERROR: environ_cb is NULL!\n");
        return;
    }

    /* Resolution */
    var.key = "j2me_resolution";
    var.value = NULL;

    bool result = environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);
    LOG_SAFE("[J2ME] GET_VARIABLE(j2me_resolution) returned %d, value='%s'\n",
            result ? 1 : 0, var.value ? var.value : "(null)");

    /* Check cache if GET_VARIABLE failed */
    if (!var.value) {
        var.value = get_stored_option_value("j2me_resolution");
        LOG_SAFE("[J2ME] Using cached value: '%s'\n", var.value ? var.value : "(null)");
    }

    if (var.value) {
        if (strcmp(var.value, "240x136") == 0) {
            g_screen_width = 240; g_screen_height = 136;
        } else if (strcmp(var.value, "480x272") == 0) {
            g_screen_width = 480; g_screen_height = 272;
        } else if (strcmp(var.value, "320x480") == 0) {
            g_screen_width = 320; g_screen_height = 480;
        } else if (strcmp(var.value, "360x640") == 0) {
            g_screen_width = 360; g_screen_height = 640;
        } else if (strcmp(var.value, "480x800") == 0) {
            g_screen_width = 480; g_screen_height = 800;
        } else if (strcmp(var.value, "176x208") == 0) {
            g_screen_width = 176; g_screen_height = 208;
        } else if (strcmp(var.value, "128x128") == 0) {
            g_screen_width = 128; g_screen_height = 128;
        } else {
            g_screen_width = 240; g_screen_height = 320;
        }
        store_option_value("j2me_resolution", var.value);
    }

    /* Frame rate */
    var.key = "j2me_fps";
    var.value = NULL;
    result = environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);
    LOG_SAFE("[J2ME] GET_VARIABLE(j2me_fps) = %d, value='%s'\n",
            result ? 1 : 0, var.value ? var.value : "(null)");

    if (!var.value) var.value = get_stored_option_value("j2me_fps");

    if (var.value) {
        g_target_fps = atoi(var.value);
        if (g_target_fps < 15) g_target_fps = 15;
        if (g_target_fps > 60) g_target_fps = 60;
        store_option_value("j2me_fps", var.value);
    }

    /* Audio sample rate */
    var.key = "j2me_audio_rate";
    var.value = NULL;
    result = environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var);
    LOG_SAFE("[J2ME] GET_VARIABLE(j2me_audio_rate) = %d, value='%s'\n",
            result ? 1 : 0, var.value ? var.value : "(null)");

    if (!var.value) var.value = get_stored_option_value("j2me_audio_rate");

    if (var.value) {
        g_audio_sample_rate = atoi(var.value);
        if (g_audio_sample_rate < 11025) g_audio_sample_rate = 11025;
        if (g_audio_sample_rate > 44100) g_audio_sample_rate = 44100;
        store_option_value("j2me_audio_rate", var.value);
    }

    g_screen_pitch = g_screen_width * 4;

    /* Update MIDP screen dimensions so getWidth()/getHeight() return correct values */
    midp_set_screen_dimensions(g_screen_width, g_screen_height);

    LOG_SAFE("[J2ME] Final settings: %dx%d, fps=%d, audio=%d\n",
            g_screen_width, g_screen_height, g_target_fps, g_audio_sample_rate);

    /* Note: Resolution changes during runtime require game restart */
    if (g_jvm && g_jvm->running && (g_screen_width != old_width || g_screen_height != old_height)) {
        LOG_SAFE("[J2ME] NOTE: Resolution change from %dx%d to %dx%d will take effect on next game load\n",
                old_width, old_height, g_screen_width, g_screen_height);
        /* Revert to old resolution for this session - cannot resize while running */
        g_screen_width = old_width;
        g_screen_height = old_height;
        midp_set_screen_dimensions(old_width, old_height);
    }

    /* Handle FPS/audio change */
    if (old_fps != g_target_fps || old_audio != g_audio_sample_rate) {
        libretro_set_sample_rate(g_audio_sample_rate);
        libretro_set_fps(g_target_fps);
    }
}

void retro_set_audio_sample(retro_audio_sample_t cb) {
    (void)cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) {
    audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb) {
    LOG_SAFE("[J2ME] retro_set_input_poll called, cb=%p\n", (void*)cb);
    input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb) {
    LOG_SAFE("[J2ME] retro_set_input_state called, cb=%p\n", (void*)cb);
    input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb) {
    video_cb = cb;
}

void retro_reset(void) {
    LOG_SAFE("[J2ME] retro_reset\n");
    g_key_states = 0;
    g_prev_key_states = 0;
    /* Clear key repeat state on reset */
    memset(&g_key_repeat, 0, sizeof(g_key_repeat));
}

bool retro_load_game(const struct retro_game_info *info) {
    LOG_SAFE("[J2ME] retro_load_game: %s\n", info ? info->path : "NULL");
    
    if (!info || !info->path) {
        LOG_SAFE("[J2ME] No game path\n");
        return false;
    }
    
    /* Read settings */
    update_variables(info->path);
    
    LOG_SAFE("[J2ME] Settings from frontend: resolution=%dx%d, fps=%d, audio=%d\n",
            g_screen_width, g_screen_height, g_target_fps, g_audio_sample_rate);
    
    /* Reallocate buffers if resolution changed */
    size_t required_size = g_screen_width * g_screen_height * sizeof(uint32_t);
    if (g_framebuffer_size != required_size && g_framebuffer) {
        LOG_SAFE("[J2ME] Reallocating buffers for %dx%d\n", g_screen_width, g_screen_height);
        
        free(g_framebuffer);
        free(g_rgb565_buffer);
        
        g_framebuffer_size = required_size;
        g_framebuffer = (uint32_t*)malloc(g_framebuffer_size);
        if (!g_framebuffer) {
            LOG_SAFE("[J2ME] Failed to reallocate framebuffer\n");
            return false;
        }
        memset(g_framebuffer, 0, g_framebuffer_size);
        g_screen_pitch = g_screen_width * 4;
        
        size_t rgb565_size = g_screen_width * g_screen_height * sizeof(uint16_t);
        g_rgb565_buffer = (uint16_t*)malloc(rgb565_size);
        if (!g_rgb565_buffer) {
            LOG_SAFE("[J2ME] Failed to reallocate RGB565 buffer\n");
            free(g_framebuffer);
            g_framebuffer = NULL;
            return false;
        }
        memset(g_rgb565_buffer, 0, rgb565_size);
        
        LOG_SAFE("[J2ME] Buffers reallocated\n");
    }
    
    /* Load JAR file */
    FILE* f = fopen(info->path, "rb");
    if (!f) {
        LOG_SAFE("[J2ME] Cannot open file\n");
        return false;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    g_jar_data = malloc(size);
    if (!g_jar_data) {
        fclose(f);
        return false;
    }
    
    if (fread(g_jar_data, 1, size, f) != (size_t)size) {
        free(g_jar_data);
        fclose(f);
        return false;
    }
    fclose(f);
    g_jar_size = size;
    
    LOG_SAFE("[J2ME] JAR loaded: %ld bytes\n", size);
    
    /* Find MIDlet class */
    char* midlet_class = find_midlet_class_in_jar(g_jar_data, g_jar_size);
    if (midlet_class) {
        strncpy(g_midlet_class, midlet_class, sizeof(g_midlet_class) - 1);
        g_midlet_class[sizeof(g_midlet_class) - 1] = '\0';
        free(midlet_class);
        LOG_SAFE("[J2ME] Found MIDlet class: %s\n", g_midlet_class);
    } else {
        strcpy(g_midlet_class, "MIDlet");
        LOG_SAFE("[J2ME] Warning: Could not find MIDlet class in manifest\n");
    }
    
    /* Extract game name from JAR path for RMS save directory */
    {
        const char* path = info->path;
        const char* base = strrchr(path, '/');
        if (!base) base = strrchr(path, '\\');
        if (base) base++; else base = path;
        
        strncpy(g_game_name, base, sizeof(g_game_name) - 1);
        g_game_name[sizeof(g_game_name) - 1] = '\0';
        
        /* Strip .jar extension if present */
        char* dot = strrchr(g_game_name, '.');
        if (dot) *dot = '\0';
        
        LOG_SAFE("[J2ME] Game name for RMS: '%s'\n", g_game_name);
    }
    
    /* Get save directory from libretro frontend and set up RMS persistence */
    if (environ_cb) {
        const char* save_dir = NULL;
        if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir) && save_dir) {
            LOG_SAFE("[J2ME] Save directory: '%s'\n", save_dir);
            midp_rms_set_save_path(save_dir, g_game_name);
        } else {
            LOG_SAFE("[J2ME] No save directory available, RMS disk persistence disabled\n");
        }
    }
    
    /* Set pixel format */
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        LOG_SAFE("[J2ME] RGB565 not supported, trying XRGB8888\n");
        fmt = RETRO_PIXEL_FORMAT_XRGB8888;
        if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
            LOG_SAFE("[J2ME] Neither RGB565 nor XRGB8888 supported\n");
            return false;
        }
        g_use_rgb565 = false;
        LOG_SAFE("[J2ME] Using XRGB8888 format\n");
    } else {
        LOG_SAFE("[J2ME] Using RGB565 format\n");
    }
    
    libretro_set_sample_rate(g_audio_sample_rate);
    libretro_set_fps(g_target_fps);

    /* Initialize emulator */
    LOG_SAFE("[J2ME] retro_load_game: calling init_emulator()...\n");
    if (!init_emulator()) {
        LOG_SAFE("[J2ME] Emulator init failed\n");
        return false;
    }
    LOG_SAFE("[J2ME] retro_load_game: init_emulator() OK\n");
    
    /* Run MIDlet */
    LOG_SAFE("[J2ME] retro_load_game: calling run_midlet()...\n");
    run_midlet();
    LOG_SAFE("[J2ME] retro_load_game: run_midlet() returned\n");
    
    LOG_SAFE("[J2ME] Game loaded successfully\n");
    return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num) {
    (void)type;
    (void)info;
    (void)num;
    return false;
}

void retro_unload_game(void) {
    LOG_SAFE("[J2ME] retro_unload_game\n");
    
    /* Save all RMS record stores to disk before unloading */
    midp_rms_save_all();
}

/* Key states */
static int get_key_states(void) {
    int keys = 0;
    if (input_state_cb) {
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))    keys |= (1 << 1);
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))  keys |= (1 << 6);
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))  keys |= (1 << 2);
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT)) keys |= (1 << 5);
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A)) keys |= (1 << 8);
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B)) keys |= (1 << 11);
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X)) keys |= (1 << 12);
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y)) keys |= (1 << 13);
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT)) keys |= (1 << 9);
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START))  keys |= (1 << 10);
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L)) keys |= (1 << 14);
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R)) keys |= (1 << 15);
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2)) keys |= (1 << 16);
        if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2)) keys |= (1 << 17);
    }
    return keys;
}

/* Poll keyboard for extra J2ME keys (0, 6-9, *, #) via RETRO_DEVICE_KEYBOARD */
static int get_kb_key_states(void) {
    int keys = 0;
    if (input_state_cb) {
        for (size_t i = 0; i < KB_KEY_MAP_SIZE; i++) {
            if (input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, kb_key_map[i].retro_key))
                keys |= (1 << kb_key_map[i].bit);
        }
    }
    return keys;
}

/* Process key events with key repeat support.
 * On real J2ME phones, holding a key generates repeated keyPressed events
 * after an initial delay (~400ms) at ~12Hz rate. Many Canvas-based games
 * rely on this for continuous movement instead of polling getKeyStates().
 * GameCanvas games with suppressKeyEvents=true use getKeyStates() polling
 * and don't need repeat, but the repeat events are harmlessly suppressed. */
static void process_key_events(void) {
    if (!g_jvm || !g_jvm->running) return;
    
    int pressed = g_key_states & ~g_prev_key_states;
    int released = ~g_key_states & g_prev_key_states;
    int held = g_key_states & g_prev_key_states;  /* Keys still held from last frame */
    uint64_t now = millis();
    
    extern bool midp_handle_soft_button(JVM* jvm, int button_index);
    
    for (size_t i = 0; i < KEY_MAP_SIZE; i++) {
        int bit = key_map[i].bit;
        int keycode = key_map[i].keycode;
        
        if (pressed & (1 << bit)) {
            /* New key press - record time and fire initial keyPressed */
            if (bit <= KEY_REPEAT_MAX_BIT) {
                g_key_repeat.is_held[bit] = true;
                g_key_repeat.press_time[bit] = now;
                g_key_repeat.last_repeat_time[bit] = now;
            }
            
            /* Intercept soft keys (SELECT=-6, START=-7) for Command handling */
            if (keycode == -6) {
                if (midp_handle_soft_button(g_jvm, 0)) continue;
            } else if (keycode == -7) {
                if (midp_handle_soft_button(g_jvm, 1)) continue;
            }
            midp_call_keyPressed(g_jvm, keycode);
        } else if (released & (1 << bit)) {
            /* Key released - clear repeat state and fire keyReleased */
            key_repeat_reset(bit);
            midp_call_keyReleased(g_jvm, keycode);
        } else if ((held & (1 << bit)) && bit <= KEY_REPEAT_MAX_BIT && g_key_repeat.is_held[bit]) {
            /* Key is still held - check if repeat should fire */
            uint64_t hold_duration = now - g_key_repeat.press_time[bit];
            uint64_t since_last = now - g_key_repeat.last_repeat_time[bit];
            
            if (hold_duration >= KEY_REPEAT_INITIAL_DELAY_MS &&
                since_last >= KEY_REPEAT_INTERVAL_MS) {
                /* Generate repeat keyPressed event */
                g_key_repeat.last_repeat_time[bit] = now;
                
                /* Skip repeat for soft keys - they should not repeat */
                if (keycode == -6 || keycode == -7) continue;
                
                midp_call_keyPressed(g_jvm, keycode);
            }
        }
    }
    
    g_prev_key_states = g_key_states;
    
    /* Process keyboard extra keys (0, 6-9, *, #) */
    int kb_pressed = g_kb_key_states & ~g_kb_prev_key_states;
    int kb_released = ~g_kb_key_states & g_kb_prev_key_states;
    
    for (size_t i = 0; i < KB_KEY_MAP_SIZE; i++) {
        int bit = kb_key_map[i].bit;
        int keycode = kb_key_map[i].keycode;
        
        if (kb_pressed & (1 << bit)) {
            midp_call_keyPressed(g_jvm, keycode);
        }
        if (kb_released & (1 << bit)) {
            midp_call_keyReleased(g_jvm, keycode);
        }
        /* Note: keyboard extra keys don't need repeat since they are
         * typically used for text input where repeat is handled differently */
    }
    
    g_kb_prev_key_states = g_kb_key_states;
}

/* Check for variable updates */
static void check_variable_updates(void) {
    if (!environ_cb) return;
    
    bool updated = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
        LOG_SAFE("[J2ME] Variables updated, re-reading...\n");
        update_variables(NULL);
    }
}

void retro_run(void) {
    static int first_run = 1;

    if (first_run) {
        LOG_SAFE("[J2ME] retro_run started, screen=%dx%d, fps=%d\n",
                g_screen_width, g_screen_height, g_target_fps);
        first_run = 0;
    }

    /* Check for variable updates */
    check_variable_updates();
    
    /* Poll input */
    if (input_poll_cb) {
        input_poll_cb();
    }
    g_key_states = get_key_states();
    g_kb_key_states = get_kb_key_states();
    
    /* Begin frame */
    libretro_begin_frame();
    
    /* Check for error display mode */
    extern bool sdl_has_error(void);
    extern void sdl_draw_error_screen(SdlContext* ctx);
    extern SdlContext* sdl_get_global_context(void);
    
    if (sdl_has_error()) {
        /* Draw error screen on the framebuffer */
        SdlContext* ctx = sdl_get_global_context();
        if (ctx && ctx->framebuffer) {
            sdl_draw_error_screen(ctx);
        }
        
        /* End frame and send video */
        libretro_end_frame();
        
        if (video_cb) {
            uint32_t* display_buffer = NULL;
            int fb_width = 0, fb_height = 0;
            
            libretro_get_display_buffer(&display_buffer, &fb_width, &fb_height);
            
            if (display_buffer && fb_width > 0 && fb_height > 0) {
                if (g_use_rgb565) {
                    size_t rgb565_size = fb_width * fb_height * sizeof(uint16_t);
                    if (!g_rgb565_buffer || rgb565_size > g_framebuffer_size) {
                        if (g_rgb565_buffer) free(g_rgb565_buffer);
                        g_rgb565_buffer = (uint16_t*)malloc(rgb565_size);
                    }
                    if (g_rgb565_buffer) {
                        convert_xrgb8888_to_rgb565(display_buffer, g_rgb565_buffer, fb_width, fb_height);
                        video_cb(g_rgb565_buffer, fb_width, fb_height, fb_width * 2);
                    } else {
                        video_cb(display_buffer, fb_width, fb_height, fb_width * 4);
                    }
                } else {
                    video_cb(display_buffer, fb_width, fb_height, fb_width * 4);
                }
            } else if (g_framebuffer && g_screen_width > 0 && g_screen_height > 0) {
                if (g_use_rgb565) {
                    convert_xrgb8888_to_rgb565(g_framebuffer, g_rgb565_buffer, g_screen_width, g_screen_height);
                    video_cb(g_rgb565_buffer, g_screen_width, g_screen_height, g_screen_width * 2);
                } else {
                    video_cb(g_framebuffer, g_screen_width, g_screen_height, g_screen_pitch);
                }
            } else {
                video_cb(NULL, 1, 1, 0);
            }
        }
        
        /* Check for ESC to exit error screen */
        if (g_key_states & (1 << 9)) { /* SELECT button acts as ESC */
            g_jvm->running = false;
        }
        
        return;
    }
    
    /* Process key events */
    process_key_events();
    
    /* Execute JVM */
    if (g_jvm && g_jvm->running) {
        if (g_jvm->main_thread && g_jvm->main_thread->current_frame) {
            JavaThread* thread = g_jvm->main_thread;
            int instructions_per_frame = 30000;
            if (g_target_fps == 60) instructions_per_frame = 15000;
            else if (g_target_fps == 20) instructions_per_frame = 45000;
            else if (g_target_fps == 15) instructions_per_frame = 60000;
            
            for (int i = 0; i < instructions_per_frame && g_jvm->running; i++) {
                if (execute_frame(g_jvm, thread) != 0) {
                    /* Exception occurred during execution */
                    if (thread->pending_exception) {
                        /* Extract exception info using shared helpers */
                        JavaObject* exception = thread->pending_exception;
                        char exc_title[512] = {0};
                        char message[512] = {0};
                        char stack_trace[8192] = {0};
                        
                        /* Build informative exception title */
                        build_exception_title(exception, exc_title, sizeof(exc_title));
                        
                        /* Get message from exception detailMessage field */
                        JavaClass* exc_class = exception->header.clazz;
                        if (exc_class && exc_class->fields) {
                            for (uint16_t fi = 0; fi < exc_class->fields_count; fi++) {
                                JavaField* field = &exc_class->fields[fi];
                                if (field->name && strcmp(field->name, "detailMessage") == 0) {
                                    JavaValue* field_val = (JavaValue*)((uint8_t*)exception + sizeof(ObjectHeader) + fi * sizeof(JavaValue));
                                    if (field_val && field_val->ref) {
                                        JavaObject* str_obj = (JavaObject*)field_val->ref;
                                        if (str_obj->header.clazz && str_obj->header.clazz->class_name &&
                                            strcmp(str_obj->header.clazz->class_name, "java/lang/String") == 0) {
                                            JavaValue* value_field = (JavaValue*)((uint8_t*)str_obj + sizeof(ObjectHeader));
                                            JavaValue* count_field = (JavaValue*)((uint8_t*)str_obj + sizeof(ObjectHeader) + sizeof(JavaValue));
                                            if (value_field->ref && count_field->i > 0) {
                                                JavaObject* char_array = (JavaObject*)value_field->ref;
                                                jchar* chars = (jchar*)((uint8_t*)char_array + sizeof(ObjectHeader));
                                                int msg_len = count_field->i < 250 ? count_field->i : 250;
                                                for (int j = 0; j < msg_len; j++) {
                                                    message[j] = (char)(chars[j] & 0xFF);
                                                }
                                                message[msg_len] = '\0';
                                            }
                                        }
                                    }
                                    break;
                                }
                            }
                        }
                        
                        /* Build detailed stack trace - prefer saved trace */
                        int frames = 0;
                        if (thread->exception_stack_trace && thread->exception_stack_trace[0]) {
                            strncpy(stack_trace, thread->exception_stack_trace, sizeof(stack_trace) - 1);
                            stack_trace[sizeof(stack_trace) - 1] = '\0';
                            for (const char* p = stack_trace; *p; p++) {
                                if (*p == '\n') frames++;
                            }
                        } else {
                            frames = build_stack_trace(thread, stack_trace, sizeof(stack_trace));
                        }
                        
                        /* Set extra info with thread name, frame count, and throw location */
                        const char* thread_name = thread->name ? thread->name : "main";
                        char extra[512];
                        if (thread->exception_throw_info && thread->exception_throw_info[0]) {
                            snprintf(extra, sizeof(extra), "Thread: %s | %d frames | thrown at %s", 
                                     thread_name, frames, thread->exception_throw_info);
                        } else {
                            snprintf(extra, sizeof(extra), "Thread: %s | %d frames", thread_name, frames);
                        }
                        sdl_set_error_extra(extra);
                        
                        /* Log via libretro logging system */
                        log_message(RETRO_LOG_ERROR, "[J2ME] Uncaught %s in thread '%s': %s\n",
                                   exc_title, thread_name, message[0] ? message : "(no message)");
                        log_message(RETRO_LOG_ERROR, "[J2ME] Stack trace (%d frames):\n%s\n", frames, stack_trace);
                        
                        /* Set error info for display */
                        sdl_set_error_info(exc_title, message[0] ? message : NULL, stack_trace[0] ? stack_trace : NULL);
                    }
                    break;
                }
            }
        }
        
        /* Process timers (e.g. java.util.Timer, TimerTask) */
        extern void jvm_process_timers(JVM* jvm);
        jvm_process_timers(g_jvm);
        
        /* Process callSerially queue */
        extern void midp_process_call_serially_queue(JVM* jvm);
        midp_process_call_serially_queue(g_jvm);
        
        /* Check Alert timeout */
        extern bool midp_check_alert_timeout(JVM* jvm);
        midp_check_alert_timeout(g_jvm);
        
        midp_process_repaints(g_jvm);
    }
    
    /* End frame */
    libretro_end_frame();
    
    /* Send video */
    if (video_cb) {
        uint32_t* display_buffer = NULL;
        int fb_width = 0, fb_height = 0;
        
        libretro_get_display_buffer(&display_buffer, &fb_width, &fb_height);
        
        if (display_buffer && fb_width > 0 && fb_height > 0) {
            if (g_use_rgb565) {
                size_t rgb565_size = fb_width * fb_height * sizeof(uint16_t);
                if (!g_rgb565_buffer || rgb565_size > g_framebuffer_size) {
                    if (g_rgb565_buffer) free(g_rgb565_buffer);
                    g_rgb565_buffer = (uint16_t*)malloc(rgb565_size);
                }
                if (g_rgb565_buffer) {
                    convert_xrgb8888_to_rgb565(display_buffer, g_rgb565_buffer, fb_width, fb_height);
                    video_cb(g_rgb565_buffer, fb_width, fb_height, fb_width * 2);
                } else {
                    video_cb(display_buffer, fb_width, fb_height, fb_width * 4);
                }
            } else {
                video_cb(display_buffer, fb_width, fb_height, fb_width * 4);
            }
        } else if (g_framebuffer && g_screen_width > 0 && g_screen_height > 0) {
            if (g_use_rgb565) {
                convert_xrgb8888_to_rgb565(g_framebuffer, g_rgb565_buffer, g_screen_width, g_screen_height);
                video_cb(g_rgb565_buffer, g_screen_width, g_screen_height, g_screen_width * 2);
            } else {
                video_cb(g_framebuffer, g_screen_width, g_screen_height, g_screen_pitch);
            }
        } else {
            video_cb(NULL, 1, 1, 0);
        }
    }
    
    /* Send audio */
    libretro_process_audio();
}

/* Save states */
size_t retro_serialize_size(void) {
    return 0;
}

bool retro_serialize(void *data, size_t size) {
    (void)data;
    (void)size;
    return false;
}

bool retro_unserialize(const void *data, size_t size) {
    (void)data;
    (void)size;
    return false;
}

void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char* code) {
    (void)index;
    (void)enabled;
    (void)code;
}

unsigned retro_get_region(void) {
    return RETRO_REGION_NTSC;
}

void *retro_get_memory_data(unsigned id) {
    (void)id;
    return NULL;
}

size_t retro_get_memory_size(unsigned id) {
    (void)id;
    return 0;
}
