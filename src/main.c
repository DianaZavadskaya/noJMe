/*
 * J2ME Emulator - Main Entry Point
 * Cross-platform J2ME/MIDP2 emulator
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L  /* For strdup - only on POSIX systems */
#include <sys/stat.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include "miniz.h"

/* SDL2 on Windows needs SDL_main for proper initialization */
#ifdef _WIN32
#ifdef __has_include
#if __has_include(<SDL2/SDL_main.h>)
#include <SDL2/SDL_main.h>
#define SDL_MAIN_NEEDED 1
#endif
#endif
#endif

#include "jvm.h"
#include "classfile.h"
#include "opcodes.h"
#include "heap.h"
#include "native.h"
#include "midp.h"
#include "sdl_backend.h"
#include "debug.h"
#include "debug_macros.h"

/* Global runtime debug flag - defined here, declared in debug.h
 * Default is 0 (OFF) for release builds. Press F12 to toggle at runtime.
 * Set to 1 only when you need verbose debug logging. */
/* g_j2me_runtime_debug defined in jvm/debug_var.c */

/* Global context */
static SdlContext* g_sdl_ctx = NULL;
static JVM* g_jvm = NULL;

/* Global JAR data for resource loading */
static uint8_t* g_jar_data = NULL;
static size_t g_jar_size = 0;

/* Get JAR data for resource loading */
const uint8_t* get_jar_data(size_t* size) {
    if (size) *size = g_jar_size;
    return g_jar_data;
}

/* SDL2 detection for logging */
#if defined(HAVE_SDL2) || defined(__has_include)
#  if __has_include(<SDL2/SDL_log.h>)
#    include <SDL2/SDL_log.h>
#  elif __has_include(<SDL_log.h>)
#    include <SDL_log.h>
#  endif
#endif

/* Signal handler for clean shutdown */
static void signal_handler(int sig) {
    (void)sig;
    DEBUG_LOG("Signal received, shutting down...");
    if (g_jvm) {
        g_jvm->running = false;
    }
    if (g_sdl_ctx) {
        g_sdl_ctx->running = false;
    }
}

/* Print usage */
static void print_usage(const char* program) {
    printf("J2ME Emulator v%s - MIDP2 Mobile Java Emulator\n\n", J2ME_EMULATOR_VERSION);
    printf("Usage: %s [options] <midlet.jar> [midlet-class]\n\n", program);
    printf("Options:\n");
    printf("  -w, --width <width>      Screen width (default: 240)\n");
    printf("  -h, --height <height>    Screen height (default: 320)\n");
    printf("  -s, --scale <scale>      Display scale factor (default: 2)\n");
    printf("  -c, --classpath <path>   Additional classpath\n");
    printf("  -m, --midlet <class>     MIDlet class name\n");
    printf("  -f, --fullscreen         Start in fullscreen mode\n");
    printf("  -v, --verbose            Verbose output\n");
    printf("  --verbose-class          Verbose class loading\n");
    printf("  --verbose-gc             Verbose garbage collection\n");
    printf("  --heap-size <size>       Heap size in MB (default: 16)\n");
    printf("  --headless               Run without display (for testing)\n");
    printf("  --help                   Show this help\n");
    printf("\nExamples:\n");
    printf("  %s game.jar\n", program);
    printf("  %s -w 320 -h 480 app.jar com.example.MyMIDlet\n", program);
    printf("  %s -s 3 --fullscreen game.jar\n", program);
    printf("  %s --headless game.jar  # For testing without display\n", program);
}

/* Parse command line arguments */
typedef struct {
    const char* jar_file;
    const char* midlet_class;
    int width;
    int height;
    int scale;
    bool fullscreen;
    bool verbose;
    bool verbose_class;
    bool verbose_gc;
    bool headless;
    size_t heap_size_mb;
    const char* classpath;
} Options;

static bool parse_args(int argc, char** argv, Options* opts) {
    memset(opts, 0, sizeof(Options));
    opts->width = MIDP_DEFAULT_WIDTH;
    opts->height = MIDP_DEFAULT_HEIGHT;
    opts->scale = 2;
    opts->heap_size_mb = 16;
    opts->verbose = false;        /* Enable verbose by default for debugging */
    opts->verbose_class = false;  /* Enable class loading info by default */
    
#ifdef J2ME_HEADLESS
    /* Headless binary always runs in headless mode */
    opts->headless = true;
#endif
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--width") == 0) {
            if (++i >= argc) return false;
            opts->width = atoi(argv[i]);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--height") == 0) {
            if (++i >= argc) return false;
            opts->height = atoi(argv[i]);
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--scale") == 0) {
            if (++i >= argc) return false;
            opts->scale = atoi(argv[i]);
        } else if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--midlet") == 0) {
            if (++i >= argc) return false;
            opts->midlet_class = argv[i];
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--classpath") == 0) {
            if (++i >= argc) return false;
            opts->classpath = argv[i];
        } else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--fullscreen") == 0) {
            opts->fullscreen = true;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            opts->verbose = true;
            opts->verbose_class = true;
            opts->verbose_gc = true;
        } else if (strcmp(argv[i], "--verbose-class") == 0) {
            opts->verbose_class = true;
        } else if (strcmp(argv[i], "--verbose-gc") == 0) {
            opts->verbose_gc = true;
        } else if (strcmp(argv[i], "--heap-size") == 0) {
            if (++i >= argc) return false;
            opts->heap_size_mb = (size_t)atoi(argv[i]);
        } else if (strcmp(argv[i], "--headless") == 0) {
            opts->headless = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            return false;
        } else if (argv[i][0] != '-') {
            if (!opts->jar_file) {
                opts->jar_file = argv[i];
            } else if (!opts->midlet_class) {
                opts->midlet_class = argv[i];
            }
        } else {
            LOG_SAFE("Unknown option: %s\n", argv[i]);
            return false;
        }
    }
    
    return opts->jar_file != NULL;
}

/* Simple ZIP/JAR reading - read fields manually to avoid alignment issues */

/* Read little-endian values */
static uint16_t read_u16(const uint8_t* p) {
    return p[0] | (p[1] << 8);
}

static uint32_t read_u32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

/* Find End of Central Directory record */
static size_t find_end_of_central_directory(const uint8_t* jar_data, size_t jar_size) {
    /* EOCD is at least 22 bytes from the end */
    if (jar_size < 22) return 0;
    
    /* Search backwards for EOCD signature (0x06054B50 = 'PK\x05\x06') */
    /* EOCD can have a variable-length comment, so we need to search */
    size_t max_comment = 65535 + 22;  /* Max comment length + EOCD size */
    size_t search_start = jar_size > max_comment ? jar_size - max_comment : 0;
    
    for (size_t i = jar_size - 22; i >= search_start && i > 0; i--) {
        if (jar_data[i] == 0x50 && jar_data[i+1] == 0x4B &&
            jar_data[i+2] == 0x05 && jar_data[i+3] == 0x06) {
            return i;
        }
    }
    return 0;
}

/* Find file in JAR by name - uses Central Directory for reliable metadata */
static uint8_t* jar_find_file(const uint8_t* jar_data, size_t jar_size, 
                               const char* filename, size_t* out_size) {
    DEBUG_LOG("Searching for '%s' in JAR (size: %zu)", filename, jar_size);
    *out_size = 0;
    
    /* Step 1: Find End of Central Directory */
    size_t eocd_offset = find_end_of_central_directory(jar_data, jar_size);
    if (eocd_offset == 0) {
        DEBUG_LOG("End of Central Directory not found");
        return NULL;
    }
    
    DEBUG_LOG("EOCD found at offset %zu", eocd_offset);
    
    /* Step 2: Get Central Directory location */
    uint32_t cd_start = read_u32(jar_data + eocd_offset + 16);
    uint16_t cd_entries = read_u16(jar_data + eocd_offset + 10);
    uint32_t cd_size = read_u32(jar_data + eocd_offset + 12);
    
    DEBUG_LOG("Central Directory: start=%u, entries=%u, size=%u", cd_start, cd_entries, cd_size);
    
    /* Step 3: Search Central Directory for the file */
    size_t cde_offset = cd_start;
    for (int i = 0; i < cd_entries && cde_offset < eocd_offset; i++) {
        /* Central Directory entry signature: 0x02014B50 */
        uint32_t cd_sig = read_u32(jar_data + cde_offset);
        if (cd_sig != 0x02014B50) {
            DEBUG_LOG("Invalid CDE signature at offset %zu: 0x%08X", cde_offset, cd_sig);
            break;
        }
        
        /* Parse Central Directory entry */
        uint16_t cd_compression = read_u16(jar_data + cde_offset + 10);
        uint32_t cd_comp_size = read_u32(jar_data + cde_offset + 20);
        uint32_t cd_uncomp_size = read_u32(jar_data + cde_offset + 24);
        uint16_t cd_filename_len = read_u16(jar_data + cde_offset + 28);
        uint16_t cd_extra_len = read_u16(jar_data + cde_offset + 30);
        uint16_t cd_comment_len = read_u16(jar_data + cde_offset + 32);
        uint32_t local_header_offset = read_u32(jar_data + cde_offset + 42);
        
        const char* entry_name = (const char*)(jar_data + cde_offset + 46);
        
        DEBUG_LOG("CD Entry %d: '%.*s' (method: %u, comp: %u, uncomp: %u, offset: %u)", 
                  i + 1, cd_filename_len, entry_name, cd_compression, 
                  cd_comp_size, cd_uncomp_size, local_header_offset);
        
        /* Skip directories */
        bool is_dir = (cd_filename_len > 0 && entry_name[cd_filename_len - 1] == '/');
        
        /* Check if this is the file we want */
        if (!is_dir && cd_filename_len == strlen(filename) &&
            memcmp(entry_name, filename, cd_filename_len) == 0) {
            
            DEBUG_LOG("Found '%s' in Central Directory", filename);
            
            /* Step 4: Get data offset from local header */
            /* Local header: 30 bytes + filename_len + extra_len + data */
            uint16_t local_filename_len = read_u16(jar_data + local_header_offset + 26);
            uint16_t local_extra_len = read_u16(jar_data + local_header_offset + 28);
            size_t data_offset = local_header_offset + 30 + local_filename_len + local_extra_len;
            
            DEBUG_LOG("Local header at %u, data at %zu", local_header_offset, data_offset);
            
            /* Step 5: Extract the data */
            if (cd_compression == 0) {
                /* STORED (uncompressed) - use uncompressed size */
                *out_size = cd_uncomp_size;
                DEBUG_LOG("STORED file, size: %zu", *out_size);
                
                uint8_t* data = (uint8_t*)malloc(*out_size + 1);
                if (data) {
                    memcpy(data, jar_data + data_offset, *out_size);
                    data[*out_size] = '\0';  /* Null terminate for text files */
                }
                return data;
                
            } else if (cd_compression == 8) {
                /* DEFLATE compressed */
                DEBUG_LOG("DEFLATE file: %u -> %u bytes", cd_comp_size, cd_uncomp_size);
                
                if (cd_comp_size == 0 || cd_uncomp_size == 0) {
                    DEBUG_LOG("Invalid sizes for compressed file");
                    *out_size = 0;
                    return NULL;
                }
                
                uint8_t* data = (uint8_t*)malloc(cd_uncomp_size + 1);
                if (!data) {
                    DEBUG_LOG("Failed to allocate %u bytes", cd_uncomp_size);
                    *out_size = 0;
                    return NULL;
                }
                
                /* Use zlib to decompress (raw deflate, no zlib header) */
                z_stream stream;
                memset(&stream, 0, sizeof(stream));
                
                int ret = inflateInit2(&stream, -MAX_WBITS);
                if (ret != Z_OK) {
                    DEBUG_LOG("inflateInit2 failed: %d", ret);
                    free(data);
                    *out_size = 0;
                    return NULL;
                }
                
                stream.next_in = (Bytef*)(jar_data + data_offset);
                stream.avail_in = cd_comp_size;
                stream.next_out = data;
                stream.avail_out = cd_uncomp_size;
                
                ret = inflate(&stream, Z_FINISH);
                inflateEnd(&stream);
                
                if (ret != Z_STREAM_END) {
                    DEBUG_LOG("inflate failed: %d (%s)", ret, stream.msg ? stream.msg : "unknown");
                    free(data);
                    *out_size = 0;
                    return NULL;
                }
                
                data[cd_uncomp_size] = '\0';
                *out_size = cd_uncomp_size;
                DEBUG_LOG("Decompressed successfully: %zu bytes", *out_size);
                return data;
                
            } else {
                DEBUG_LOG("Unknown compression method: %u", cd_compression);
                *out_size = 0;
                return NULL;
            }
        }
        
        /* Move to next Central Directory entry */
        cde_offset += 46 + cd_filename_len + cd_extra_len + cd_comment_len;
    }
    
    DEBUG_LOG("File '%s' not found in JAR", filename);
    return NULL;
}

/* Public function to load a resource from the current JAR */
uint8_t* load_jar_resource(const char* filename, size_t* out_size) {
    return jar_find_file(g_jar_data, g_jar_size, filename, out_size);
}

/* Auto-generate DRM properties from JAR contents
 * This bypasses the need for a JAD file with DCHOC-* properties
 * by scanning the JAR for resource files and generating properties automatically.
 * Supports: Digital Chocolate (DCHOC-*), Siemens (SIE-*), and other DRM schemes.
 */
static void midlet_generate_drm_properties(const uint8_t* jar_data, size_t jar_size) {
    if (!jar_data || jar_size == 0) return;
    
    INFO_LOG("[MIDlet] Auto-generating DRM properties from JAR contents...");
    
    int dchoc_index = 1;
    char key[32], value[32];
    size_t out_size;
    
    /* Digital Chocolate games: "p" is the properties file, should be DCHOC-1 */
    if (jar_find_file(jar_data, jar_size, "p", &out_size)) {
        snprintf(key, sizeof(key), "DCHOC-%d", dchoc_index++);
        midlet_add_property(key, "p");
        DEBUG_LOG("[MIDlet] Generated %s = p", key);
    }
    
    /* Scan for r1, r2, ..., r99 (resource files - common in Digital Chocolate games) */
    for (int i = 1; i <= 99 && dchoc_index <= 50; i++) {
        snprintf(value, sizeof(value), "r%d", i);
        if (jar_find_file(jar_data, jar_size, value, &out_size)) {
            snprintf(key, sizeof(key), "DCHOC-%d", dchoc_index++);
            midlet_add_property(key, value);
            DEBUG_LOG("[MIDlet] Generated %s = %s", key, value);
        }
    }
    
    /* Scan for l0_0, l1_0, ..., l9_9 (level files) */
    for (int level = 0; level <= 9 && dchoc_index <= 80; level++) {
        for (int sub = 0; sub <= 9; sub++) {
            snprintf(value, sizeof(value), "l%d_%d", level, sub);
            if (jar_find_file(jar_data, jar_size, value, &out_size)) {
                snprintf(key, sizeof(key), "DCHOC-%d", dchoc_index++);
                midlet_add_property(key, value);
                DEBUG_LOG("[MIDlet] Generated %s = %s", key, value);
            }
        }
    }
    
    /* Scan for common resource patterns used by other publishers */
    /* Siemens and others: s1, s2, ... */
    int other_index = 1;
    for (int i = 1; i <= 20; i++) {
        snprintf(value, sizeof(value), "s%d", i);
        if (jar_find_file(jar_data, jar_size, value, &out_size)) {
            snprintf(key, sizeof(key), "SIE-%d", other_index++);
            midlet_add_property(key, value);
        }
    }
    
    /* Gameloft pattern: g1, g2, ... */
    other_index = 1;
    for (int i = 1; i <= 20; i++) {
        snprintf(value, sizeof(value), "g%d", i);
        if (jar_find_file(jar_data, jar_size, value, &out_size)) {
            snprintf(key, sizeof(key), "GL-%d", other_index++);
            midlet_add_property(key, value);
        }
    }
    
    /* EA pattern: ea1, ea2, ... */
    other_index = 1;
    for (int i = 1; i <= 20; i++) {
        snprintf(value, sizeof(value), "ea%d", i);
        if (jar_find_file(jar_data, jar_size, value, &out_size)) {
            snprintf(key, sizeof(key), "EA-%d", other_index++);
            midlet_add_property(key, value);
        }
    }
    
    INFO_LOG("[MIDlet] Generated %d DCHOC properties and other DRM properties", dchoc_index - 1);
}

/* Find MIDlet class from JAR manifest */
static char* find_midlet_class(const uint8_t* jar_data, size_t jar_size) {
    DEBUG_LOG("Looking for MIDlet class in manifest...");
    
    size_t manifest_size;
    uint8_t* manifest = jar_find_file(jar_data, jar_size, "META-INF/MANIFEST.MF", &manifest_size);
    
    if (!manifest) {
        /* Try lowercase */
        manifest = jar_find_file(jar_data, jar_size, "META-INF/manifest.mf", &manifest_size);
    }
    
    if (!manifest) {
        DEBUG_LOG("Manifest not found in JAR");
        return NULL;
    }
    
    DEBUG_LOG("Manifest found, size: %zu", manifest_size);
    
    /* Parse manifest to find MIDlet-n */
    char* result = NULL;
    char* manifest_str = (char*)malloc(manifest_size + 1);
    if (!manifest_str) {
        free(manifest);
        return NULL;
    }
    
    memcpy(manifest_str, manifest, manifest_size);
    manifest_str[manifest_size] = '\0';
    
    /* Look for MIDlet-1: line */
    char* line = strtok(manifest_str, "\r\n");
    while (line) {
        DEBUG_LOG("Manifest line: '%s'", line);
        
        if (strncmp(line, "MIDlet-1:", 9) == 0 || 
            strncmp(line, "MIDlet-1 :", 10) == 0) {
            /* Format: MIDlet-1: Name, Icon, Class */
            char* start = strchr(line, ':');
            if (start) {
                start++;
                /* Skip to class name (after second comma) */
                char* comma1 = strchr(start, ',');
                if (comma1) {
                    char* comma2 = strchr(comma1 + 1, ',');
                    if (comma2) {
                        comma2++;
                        while (*comma2 == ' ') comma2++;
                        
                        /* Copy class name */
                        char* end = comma2;
                        while (*end && *end != '\r' && *end != '\n' && *end != ',') end++;
                        
                        /* Trim trailing whitespace */
                        while (end > comma2 && (end[-1] == ' ' || end[-1] == '\t')) end--;
                        
                        size_t len = end - comma2;
                        result = (char*)malloc(len + 1);
                        if (result) {
                            memcpy(result, comma2, len);
                            result[len] = '\0';
                        }
                        
                        DEBUG_LOG("Found MIDlet class: '%s'", result);
                        break;
                    }
                }
            }
        }
        
        line = strtok(NULL, "\r\n");
    }
    
    free(manifest_str);
    free(manifest);
    return result;
}

/* Load JAR file into memory */
static uint8_t* load_jar_file(const char* filename, size_t* out_size) {
    DEBUG_LOG("Loading JAR file: '%s'", filename);
    
    FILE* f = fopen(filename, "rb");
    if (!f) {
        ERROR_LOG("Cannot open file: '%s'", filename);
        return NULL;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (size <= 0) {
        ERROR_LOG("Invalid file size: %ld", size);
        fclose(f);
        return NULL;
    }
    
    DEBUG_LOG("File size: %ld bytes", size);
    
    uint8_t* data = (uint8_t*)malloc(size);
    if (!data) {
        ERROR_LOG("Cannot allocate %ld bytes", size);
        fclose(f);
        return NULL;
    }
    
    if (fread(data, 1, size, f) != (size_t)size) {
        ERROR_LOG("Failed to read file");
        free(data);
        fclose(f);
        return NULL;
    }
    
    fclose(f);
    *out_size = size;
    
    DEBUG_LOG("JAR loaded successfully");
    return data;
}

/* Initialize emulator */
static bool init_emulator(Options* opts) {
    INFO_LOG("=== Initializing J2ME Emulator ===");
    INFO_LOG("Platform: %s", 
#ifdef _WIN32
        "Windows"
#elif defined(__linux__)
        "Linux"
#elif defined(__APPLE__)
        "macOS"
#else
        "Unknown"
#endif
    );
    INFO_LOG("Configuration: %dx%d, scale=%d, heap=%zuMB, headless=%s", 
             opts->width, opts->height, opts->scale, opts->heap_size_mb,
             opts->headless ? "yes" : "no");
    
    DEBUG_LOG("Step 1: Creating JVM instance...");
    g_jvm = jvm_create();
    if (!g_jvm) {
        ERROR_LOG("Failed to create JVM - memory allocation failed");
        return false;
    }
    INFO_LOG("JVM instance created");
    
    /* Configure JVM */
    DEBUG_LOG("Step 2: Configuring JVM...");
    g_jvm->config.heap_size = opts->heap_size_mb * 1024 * 1024;
    g_jvm->config.stack_size = JAVA_STACK_SIZE;
    g_jvm->config.max_threads = MAX_JAVA_THREADS;
    g_jvm->config.verbose_class = opts->verbose_class;
    g_jvm->config.verbose_gc = opts->verbose_gc;
    INFO_LOG("JVM configured (heap: %zu bytes)", g_jvm->config.heap_size);
    
    /* Initialize JVM */
    DEBUG_LOG("Step 3: Initializing JVM subsystems...");
    if (jvm_init(g_jvm) != JNI_OK) {
        ERROR_LOG("Failed to initialize JVM - jvm_init returned error");
        return false;
    }
    INFO_LOG("JVM subsystems initialized (heap, threads, class loader)");
    
    /* Initialize SDL2 backend (or headless framebuffer) */
    DEBUG_LOG("Step 4: Initializing %s...", opts->headless ? "headless mode" : "SDL backend");
    g_sdl_ctx = malloc(sizeof(SdlContext));
    if (!g_sdl_ctx) {
        ERROR_LOG("Failed to allocate SDL context - memory allocation failed");
        return false;
    }
    memset(g_sdl_ctx, 0, sizeof(SdlContext));
    
    /* IMPORTANT: Set global context BEFORE sdl_init */
    DEBUG_LOG("Setting global SDL context: %p", (void*)g_sdl_ctx);
    sdl_set_global_context(g_sdl_ctx);
    
    if (opts->headless) {
        /* Headless mode: allocate framebuffer without SDL */
        g_sdl_ctx->framebuffer = (uint32_t*)malloc(opts->width * opts->height * sizeof(uint32_t));
        if (!g_sdl_ctx->framebuffer) {
            ERROR_LOG("Failed to allocate headless framebuffer");
            free(g_sdl_ctx);
            g_sdl_ctx = NULL;
            return false;
        }
        /* ИСПРАВЛЕНО: Инициализируем чёрным непрозрачным цветом (ARGB: 0xFF000000) */
        for (int i = 0; i < opts->width * opts->height; i++) {
            g_sdl_ctx->framebuffer[i] = 0xFF000000;
        }
        g_sdl_ctx->width = opts->width;
        g_sdl_ctx->height = opts->height;
        g_sdl_ctx->scale = opts->scale;
        g_sdl_ctx->target_fps = 30;
        g_sdl_ctx->running = true;
        g_sdl_ctx->headless = true;
        INFO_LOG("Headless mode initialized (%dx%d)", opts->width, opts->height);
    } else {
        int sdl_result = sdl_init(g_jvm, opts->width, opts->height, opts->scale, false);
        if (sdl_result != 0) {
            ERROR_LOG("Failed to initialize SDL - sdl_init returned %d", sdl_result);
            free(g_sdl_ctx);
            g_sdl_ctx = NULL;
            return false;
        }
        
        /* Verify SDL context was properly initialized */
        if (g_sdl_ctx->target_fps <= 0 || g_sdl_ctx->framebuffer == NULL) {
            ERROR_LOG("SDL context not properly initialized after sdl_init()!");
            ERROR_LOG("  target_fps=%d, framebuffer=%p", g_sdl_ctx->target_fps, (void*)g_sdl_ctx->framebuffer);
            return false;
        }
        INFO_LOG("SDL backend initialized (%dx%d, scale: %d, fps: %d)", 
                 opts->width, opts->height, opts->scale, g_sdl_ctx->target_fps);
    }
    
    g_sdl_ctx->jvm = g_jvm;
    
    /* CRITICAL FIX: Re-sync global context after framebuffer/dimensions are set.
     * sdl_set_global_context copies by value, so g_headless_ctx.framebuffer was NULL
     * from the first call before allocation. This second call makes the framebuffer
     * available to create_graphics_object() for M3G rendering. */
    sdl_set_global_context(g_sdl_ctx);
    DEBUG_LOG("Re-synced global SDL context: fb=%p, %dx%d", 
              (void*)g_sdl_ctx->framebuffer, g_sdl_ctx->width, g_sdl_ctx->height);
    
    if (opts->fullscreen) {
        sdl_set_fullscreen(g_sdl_ctx, true);
        INFO_LOG("Fullscreen mode enabled");
    }
    
    /* Initialize native methods */
    DEBUG_LOG("Step 5: Registering native methods...");
    if (native_init(g_jvm) != JNI_OK) {
        ERROR_LOG("Failed to initialize native methods");
        return false;
    }
    INFO_LOG("Native methods registered (java.lang.*)");
    
    /* Initialize MIDP2 API */
    DEBUG_LOG("Step 6: Initializing MIDP2 API...");
    if (midp_init(g_jvm) != JNI_OK) {
        ERROR_LOG("Failed to initialize MIDP2 API");
        return false;
    }
    INFO_LOG("MIDP2 API initialized");
    
    /* Initialize opcodes */
    DEBUG_LOG("Step 7: Initializing opcode handlers...");
    opcodes_init();
    INFO_LOG("Opcode handlers initialized (256 opcodes)");
    
    INFO_LOG("=== Emulator initialized successfully ===");
    return true;
}

/* Load JAR and find/start MIDlet */
static bool run_midlet(Options* opts) {
    INFO_LOG("=== Loading MIDlet ===");
    DEBUG_LOG("run_midlet: Starting...");
    
    /* Load JAR file */
    DEBUG_LOG("Step 1: Loading JAR file '%s'", opts->jar_file);
    size_t jar_size;
    uint8_t* jar_data = load_jar_file(opts->jar_file, &jar_size);
    if (!jar_data) {
        ERROR_LOG("Failed to load JAR: %s", opts->jar_file);
        return false;
    }
    INFO_LOG("JAR loaded: %zu bytes", jar_size);
    
    /* Store JAR data globally for resource loading */
    g_jar_data = jar_data;
    g_jar_size = jar_size;
    
    /* Set JAR data in JVM class loader - enables automatic class loading from JAR */
    jvm_set_jar_data(g_jvm, jar_data, jar_size, opts->jar_file);
    
    /* Load manifest for getAppProperty support */
    size_t manifest_size;
    uint8_t* manifest = jar_find_file(jar_data, jar_size, "META-INF/MANIFEST.MF", &manifest_size);
    if (!manifest) {
        manifest = jar_find_file(jar_data, jar_size, "META-INF/manifest.mf", &manifest_size);
    }
    if (manifest) {
        INFO_LOG("Manifest loaded for getAppProperty: %zu bytes", manifest_size);
        midlet_set_manifest((const char*)manifest, manifest_size);
        free(manifest);
    } else {
        DEBUG_LOG("No manifest found in JAR");
    }
    
    /* DISABLED: Auto-generate DRM properties from JAR contents 
     * This was causing games to fail because they expect DCHOC-* properties to be NULL.
     * KEmulator returns NULL for all DCHOC-* properties and games work fine.
     * Only enable this if you have a specific game that requires auto-generated DRM.
     */
    /* midlet_generate_drm_properties(jar_data, jar_size); */
    
    /* Optional: Load JAD file for additional properties (can override auto-generated) */
    char* jad_path = strdup(opts->jar_file);
    if (jad_path) {
        /* Replace .jar with .jad */
        size_t jad_path_len = strlen(jad_path);
        if (jad_path_len > 4 && strcmp(jad_path + jad_path_len - 4, ".jar") == 0) {
            strcpy(jad_path + jad_path_len - 4, ".jad");
        } else if (jad_path_len > 4 && strcmp(jad_path + jad_path_len - 4, ".JAR") == 0) {
            strcpy(jad_path + jad_path_len - 4, ".JAD");
        } else {
            /* Append .jad */
            char* new_path = (char*)malloc(jad_path_len + 5);
            if (new_path) {
                strcpy(new_path, jad_path);
                strcat(new_path, ".jad");
                free(jad_path);
                jad_path = new_path;
            }
        }
        
        FILE* jad_file = fopen(jad_path, "r");
        if (jad_file) {
            fseek(jad_file, 0, SEEK_END);
            long jad_size = ftell(jad_file);
            fseek(jad_file, 0, SEEK_SET);
            
            if (jad_size > 0) {
                char* jad_data = (char*)malloc(jad_size + 1);
                if (jad_data) {
                    size_t read_size = fread(jad_data, 1, jad_size, jad_file);
                    jad_data[read_size] = '\0';
                    INFO_LOG("JAD loaded (optional): %s (%ld bytes)", jad_path, jad_size);
                    
                    /* Append JAD properties to manifest for getAppProperty */
                    midlet_append_manifest(jad_data);
                    free(jad_data);
                }
            }
            fclose(jad_file);
        }
        free(jad_path);
    }
    
    /* Find MIDlet class */
    DEBUG_LOG("Step 2: Finding MIDlet class...");
    if (!opts->midlet_class) {
        DEBUG_LOG("No MIDlet class specified, searching manifest...");
        char* found = find_midlet_class(jar_data, jar_size);
        if (found) {
            opts->midlet_class = found;
            INFO_LOG("Found MIDlet class in manifest: %s", opts->midlet_class);
        } else {
            ERROR_LOG("No MIDlet class specified or found in manifest");
            free(jar_data);
            return false;
        }
    } else {
        INFO_LOG("Using specified MIDlet class: %s", opts->midlet_class);
    }
    
    /* Convert class name from dot notation to slash notation */
    char* class_name = strdup(opts->midlet_class);
    for (char* p = class_name; *p; p++) {
        if (*p == '.') *p = '/';
    }
    
    DEBUG_LOG("Step 3: Loading main class '%s'...", class_name);
    
    /* Load the main class - now uses jvm_load_class which checks JAR */
    JavaClass* main_class = jvm_load_class(g_jvm, class_name);
    if (!main_class) {
        ERROR_LOG("Failed to load class: %s", class_name);
        free(class_name);
        free(jar_data);
        return false;
    }
    INFO_LOG("Main class loaded: %s (version %d.%d, %d methods)", 
             main_class->class_name ? main_class->class_name : "(unnamed)",
             main_class->major_version, main_class->minor_version,
             main_class->methods_count);
    
    /* Dump class info if verbose */
    if (opts->verbose_class) {
        jvm_dump_class(main_class);
    }
    
    /* Execute the MIDlet */
    INFO_LOG("Step 4: Starting MIDlet execution...");
    int result = jvm_run_midlet(g_jvm, main_class);
    
    if (result != 0) {
        ERROR_LOG("MIDlet execution failed with code %d", result);
        
        /* Check for pending exception and display error screen */
        JavaThread* main_thread = jvm_current_thread(g_jvm);
        if (main_thread && main_thread->pending_exception) {
            JavaObject* exception = main_thread->pending_exception;
            JavaClass* exc_class = exception->header.clazz;
            
            /* Get exception class name */
            const char* exc_name = exc_class ? exc_class->class_name : "Unknown Exception";
            
            /* Try to get exception message from detailMessage field */
            char message[512] = {0};
            char stack_trace[2048] = {0};
            
            /* Look for detailMessage field in exception object */
            if (exc_class && exc_class->fields) {
                for (uint16_t i = 0; i < exc_class->fields_count; i++) {
                    JavaField* field = &exc_class->fields[i];
                    if (field->name && strcmp(field->name, "detailMessage") == 0) {
                        /* Found message field - get its value */
                        JavaValue* field_val = (JavaValue*)((uint8_t*)exception + sizeof(ObjectHeader) + i * sizeof(JavaValue));
                        if (field_val && field_val->ref) {
                            /* It's a String object - get its UTF8 value */
                            JavaObject* str_obj = (JavaObject*)field_val->ref;
                            if (str_obj->header.clazz && str_obj->header.clazz->class_name &&
                                strcmp(str_obj->header.clazz->class_name, "java/lang/String") == 0) {
                                /* Try to get the char array */
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
            
            /* Build stack trace from current frame */
            if (main_thread->current_frame) {
                JavaFrame* frame = main_thread->current_frame;
                int pos = 0;
                int frame_count = 0;
                
                while (frame && frame_count < 15) {
                    const char* cls_name = frame->clazz && frame->clazz->class_name ? frame->clazz->class_name : "?";
                    const char* method_name = frame->method && frame->method->name ? frame->method->name : "?";
                    
                    int written = snprintf(stack_trace + pos, sizeof(stack_trace) - pos - 1,
                            "  at %s.%s (PC=%d)\n", cls_name, method_name, frame->throwing_pc);
                    if (written > 0 && pos + written < (int)sizeof(stack_trace) - 1) {
                        pos += written;
                    }
                    
                    frame = frame->prev;
                    frame_count++;
                }
            }
            
            /* Set error info for display */
            sdl_set_error_info(exc_name, message[0] ? message : NULL, stack_trace[0] ? stack_trace : NULL);
            
            INFO_LOG("Displaying error screen for: %s: %s", exc_name, message);
        } else {
            /* No exception object, just show generic error */
            sdl_set_error_info("Execution Failed", "MIDlet returned error code", NULL);
        }
        
        /* Don't exit - keep running to show error screen */
        /* Fall through to main loop which will display the error */
    } else {
        INFO_LOG("MIDlet started successfully");
    }
    
    /* Run SDL main loop or headless execution */
    INFO_LOG("Step 5: Entering main event loop...");
    DEBUG_LOG("Starting main loop...");
    DEBUG_LOG("g_sdl_ctx: %p, target_fps: %d, framebuffer: %p", 
              (void*)g_sdl_ctx, g_sdl_ctx ? g_sdl_ctx->target_fps : -1,
              g_sdl_ctx ? (void*)g_sdl_ctx->framebuffer : NULL);
    
    /* CRITICAL DEBUG: Check running status BEFORE sdl_run */
    LOG_SAFE("[MAIN] BEFORE sdl_run: g_sdl_ctx->running=%d, g_jvm->running=%d\n",
            g_sdl_ctx ? g_sdl_ctx->running : -1,
            g_jvm ? g_jvm->running : -1);
    
    /* ИСПРАВЛЕНО: Force running to true if needed */
    if (g_sdl_ctx && !g_sdl_ctx->running) {
        LOG_SAFE("[MAIN] WARNING: g_sdl_ctx->running was false, forcing to true\n");
        g_sdl_ctx->running = true;
    }
    if (g_jvm && !g_jvm->running) {
        LOG_SAFE("[MAIN] WARNING: g_jvm->running was false, forcing to true\n");
        g_jvm->running = true;
    }
    
    LOG_SAFE("[MAIN] AFTER fix: g_sdl_ctx->running=%d, g_jvm->running=%d\n",
            g_sdl_ctx ? g_sdl_ctx->running : -1,
            g_jvm ? g_jvm->running : -1);
    
    if (!g_sdl_ctx || g_sdl_ctx->target_fps <= 0) {
        ERROR_LOG("Context invalid, cannot run main loop");
        free(class_name);
        free(jar_data);
        return false;
    }
    
    sdl_run(g_sdl_ctx);
    
    free(class_name);
    free(jar_data);
    
    INFO_LOG("=== MIDlet finished ===");
    return true;
}

/* Cleanup */
static void cleanup(void) {
    DEBUG_LOG("Cleaning up...");
    
    if (g_sdl_ctx) {
        sdl_destroy(g_sdl_ctx);
        free(g_sdl_ctx);
        g_sdl_ctx = NULL;
    }
    
    if (g_jvm) {
        jvm_destroy(g_jvm);
        g_jvm = NULL;
    }
    
    DEBUG_LOG("Cleanup complete");
}

/* Main entry point */
int main(int argc, char** argv) {
    /* Initialize logging mutex FIRST */
    log_mutex_init();
    
    /* Immediate output for MinGW debugging */
    LOG_SAFE("=== J2ME Emulator v%s ===\n", J2ME_EMULATOR_VERSION);
    LOG_SAFE("Platform: ");
#ifdef _WIN32
    LOG_SAFE("Windows");
#ifdef __MINGW32__
    LOG_SAFE(" (MinGW)");
#endif
#elif defined(__linux__)
    LOG_SAFE("Linux");
#elif defined(__APPLE__)
    LOG_SAFE("macOS");
#else
    LOG_SAFE("Unknown");
#endif
    LOG_SAFE("\n");
    
    /* Also print to stdout */
    printf("=== J2ME Emulator v%s ===\n", J2ME_EMULATOR_VERSION);
    printf("Platform: ");
#ifdef _WIN32
    printf("Windows");
#ifdef __MINGW32__
    printf(" (MinGW)");
#endif
#elif defined(__linux__)
    printf("Linux");
#elif defined(__APPLE__)
    printf("macOS");
#else
    printf("Unknown");
#endif
    printf("\n");
    fflush(stdout);
    
    DEBUG_LOG("=== J2ME Emulator Starting ===");
    DEBUG_LOG("argc: %d", argc);
    for (int i = 0; i < argc; i++) {
        DEBUG_LOG("argv[%d]: '%s'", i, argv[i]);
    }
    
    Options opts;
    
    /* Parse arguments */
    if (!parse_args(argc, argv, &opts)) {
        print_usage(argv[0]);
        return 1;
    }
    
    DEBUG_LOG("JAR file: %s", opts.jar_file);
    DEBUG_LOG("MIDlet class: %s", opts.midlet_class ? opts.midlet_class : "(auto-detect)");
    
    /* Enable debug mode if verbose flag is set */
    LOG_SAFE("[DEBUG] opts.verbose = %d\n", opts.verbose);
    if (opts.verbose) {
        g_j2me_runtime_debug = 1;
        LOG_SAFE("[J2ME] Debug mode enabled via --verbose flag\n");
    }
    
    /* Check if JAR file exists */
    FILE* jar_test = fopen(opts.jar_file, "rb");
    if (!jar_test) {
        ERROR_LOG("JAR file not found: %s", opts.jar_file);
        ERROR_LOG("Current directory: ");
#ifdef _WIN32
        system("cd");
#else
        system("pwd");
#endif
        return 1;
    }
    fclose(jar_test);
    DEBUG_LOG("JAR file exists and is readable");
    
    /* Print banner */
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║           J2ME Emulator - MIDP2 Mobile Java               ║\n");
    printf("║                    Version %s                          ║\n", J2ME_EMULATOR_VERSION);
    printf("╠═══════════════════════════════════════════════════════════╣\n");
    printf("║  SDL2 Graphics  │  MIDP2 API  │  Full Opcode Support     ║\n");
    printf("╠═══════════════════════════════════════════════════════════╣\n");
    printf("║  Press F12 to toggle debug mode at runtime                ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n");
    printf("\n");
    
    /* Print debug mode status */
    LOG_SAFE("[J2ME] Debug mode: %s (Press F12 to toggle)\n", 
            g_j2me_runtime_debug ? "ON" : "OFF");
    
    /* Install signal handlers */
#ifdef _WIN32
    /* Windows uses signal() directly */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#else
    /* POSIX systems */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
#endif
    
    /* Initialize */
    if (!init_emulator(&opts)) {
        ERROR_LOG("Failed to initialize emulator");
        cleanup();
        return 1;
    }
    
    /* Run MIDlet */
    bool success = run_midlet(&opts);
    
    /* Cleanup */
    cleanup();
    
    DEBUG_LOG("=== J2ME Emulator Exiting (success: %d) ===", success);
    return success ? 0 : 1;
}
