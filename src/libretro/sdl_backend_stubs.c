/*
 * SDL Backend Stubs for Libretro Build
 * With proper double buffering for multi-threaded mode
 * 
 * MODIFIED: Audio processing moved to separate thread
 * - Audio thread generates samples asynchronously
 * - Main thread retrieves processed samples
 * - Thread-safe ring buffer for audio data exchange
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/time.h>
#include <unistd.h>

#include "debug.h"
#include "debug_macros.h"
#include "libretro.h"
#include "sdl_backend.h"
#include "midp.h"
#include "miniz.h"

/* ============================================
 * External variables from libretro.c
 * ============================================ */

extern retro_video_refresh_t video_cb;
extern retro_input_poll_t input_poll_cb;
extern retro_input_state_t input_state_cb;
extern retro_environment_t environ_cb;

extern uint32_t* libretro_get_framebuffer(void);
extern int libretro_get_screen_width(void);
extern int libretro_get_screen_height(void);

/* ============================================
 * Audio Thread Implementation
 * ============================================ */

/* Audio buffer constants */
#define AUDIO_BUFFER_SIZE 8192
#define AUDIO_SOURCE_RATE 44100

/* Ring buffer for audio data exchange between threads */
typedef struct {
    int16_t* buffer;
    size_t capacity;
    atomic_size_t write_pos;
    atomic_size_t read_pos;
    atomic_size_t count;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} AudioRingBuffer;

static AudioRingBuffer g_audio_ring = {0};
static int16_t g_audio_output_buffer[AUDIO_BUFFER_SIZE * 2];
static bool g_audio_initialized = false;

/* Audio thread state */
static pthread_t g_audio_thread;
static atomic_bool g_audio_thread_running = false;
static atomic_bool g_audio_thread_started = false;

/* Audio parameters */
static uint32_t g_frontend_sample_rate = 22050;
static double g_audio_resample_ratio = 1.0;
static double g_audio_resample_pos = 0.0;

/* Time-based audio control */
static uint64_t g_last_frame_time = 0;
static double g_audio_time_accumulator = 0.0;

/* Forward declaration */
extern void media_generate_audio_samples(int samples);

/* ============================================
 * Ring Buffer Implementation
 * ============================================ */

static bool audio_ring_init(AudioRingBuffer* ring, size_t capacity) {
    ring->buffer = (int16_t*)malloc(capacity * 2 * sizeof(int16_t));
    if (!ring->buffer) return false;
    
    ring->capacity = capacity;
    atomic_store(&ring->write_pos, 0);
    atomic_store(&ring->read_pos, 0);
    atomic_store(&ring->count, 0);
    
    pthread_mutex_init(&ring->mutex, NULL);
    pthread_cond_init(&ring->not_empty, NULL);
    pthread_cond_init(&ring->not_full, NULL);
    
    memset(ring->buffer, 0, capacity * 2 * sizeof(int16_t));
    return true;
}

static void audio_ring_destroy(AudioRingBuffer* ring) {
    if (ring->buffer) {
        free(ring->buffer);
        ring->buffer = NULL;
    }
    pthread_mutex_destroy(&ring->mutex);
    pthread_cond_destroy(&ring->not_empty);
    pthread_cond_destroy(&ring->not_full);
}

/* Write samples to ring buffer (called from audio thread) */
static size_t audio_ring_write(AudioRingBuffer* ring, const int16_t* samples, size_t count) {
    if (!ring->buffer || count == 0) return 0;
    
    pthread_mutex_lock(&ring->mutex);
    
    /* Wait if buffer is full (with timeout) */
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_nsec += 10000000; /* 10ms timeout */
    if (timeout.tv_nsec >= 1000000000) {
        timeout.tv_nsec -= 1000000000;
        timeout.tv_sec++;
    }
    
    while (atomic_load(&ring->count) >= ring->capacity) {
        if (pthread_cond_timedwait(&ring->not_full, &ring->mutex, &timeout) != 0) {
            pthread_mutex_unlock(&ring->mutex);
            return 0; /* Timeout - buffer full */
        }
    }
    
    size_t written = 0;
    for (size_t i = 0; i < count && atomic_load(&ring->count) < ring->capacity; i++) {
        size_t pos = atomic_load(&ring->write_pos);
        ring->buffer[pos * 2] = samples[i * 2];
        ring->buffer[pos * 2 + 1] = samples[i * 2 + 1];
        atomic_store(&ring->write_pos, (pos + 1) % ring->capacity);
        atomic_fetch_add(&ring->count, 1);
        written++;
    }
    
    pthread_cond_signal(&ring->not_empty);
    pthread_mutex_unlock(&ring->mutex);
    
    return written;
}

/* Read samples from ring buffer (called from main thread) */
static size_t audio_ring_read(AudioRingBuffer* ring, int16_t* samples, size_t count) {
    if (!ring->buffer || count == 0) return 0;
    
    size_t read_count = 0;
    
    for (size_t i = 0; i < count && atomic_load(&ring->count) > 0; i++) {
        size_t pos = atomic_load(&ring->read_pos);
        samples[i * 2] = ring->buffer[pos * 2];
        samples[i * 2 + 1] = ring->buffer[pos * 2 + 1];
        atomic_store(&ring->read_pos, (pos + 1) % ring->capacity);
        atomic_fetch_sub(&ring->count, 1);
        read_count++;
    }
    
    if (read_count > 0) {
        pthread_cond_signal(&ring->not_full);
    }
    
    return read_count;
}

/* Get available samples count */
static size_t audio_ring_available(AudioRingBuffer* ring) {
    return atomic_load(&ring->count);
}

/* ============================================
 * Audio Thread Function
 * ============================================ */

/* Audio thread - continuously generates audio samples */
static void* audio_thread_func(void* arg) {
    (void)arg;

    int16_t* temp_buffer = (int16_t*)malloc(AUDIO_BUFFER_SIZE * 2 * sizeof(int16_t));
    if (!temp_buffer) {
        return NULL;
    }
    
    int samples_per_chunk = 512; /* Generate in smaller chunks */
    int chunks_per_cycle = 4;
    int cycle_counter = 0;
    
    while (atomic_load(&g_audio_thread_running)) {
        /* Generate audio samples */
        /* We call media_generate_audio_samples which will call sdl_audio_queue_samples */
        /* But we want to intercept the output */
        
        /* For now, use a simpler approach: generate and write directly */
        memset(temp_buffer, 0, samples_per_chunk * 2 * sizeof(int16_t));
        
        /* Call the media layer to generate samples */
        media_generate_audio_samples(samples_per_chunk);
        
        /* The samples are now in sdl_audio_queue_samples - we need a different approach */
        /* Let's use a different buffer for the audio thread */
        
        cycle_counter++;
        
        /* Sleep to maintain buffer - aim for ~60Hz generation rate */
        usleep(4000); /* 4ms sleep */
        
        /* Periodically check if we should stop */
        if (!atomic_load(&g_audio_thread_running)) break;
    }
    
    free(temp_buffer);
    return NULL;
}

/* ============================================
 * Alternative Audio Thread - Pull Model
 * ============================================ */

/* Audio thread state for pull model */
static atomic_bool g_generate_audio_request = false;
static atomic_int g_samples_requested = 0;
static atomic_bool g_audio_data_ready = false;

/* Secondary buffer for thread communication */
#define THREAD_AUDIO_BUFFER_SIZE 4096
static int16_t g_thread_audio_buffer[THREAD_AUDIO_BUFFER_SIZE * 2];
static atomic_size_t g_thread_audio_count = 0;

/* Audio thread with pull model - generates samples on demand */
static void* audio_thread_pull_func(void* arg) {
    (void)arg;

    while (atomic_load(&g_audio_thread_running)) {
        /* Wait for generation request or timeout */
        int waited = 0;
        while (!atomic_load(&g_generate_audio_request) && waited < 10000) {
            usleep(100);
            waited += 100;
        }
        
        if (!atomic_load(&g_audio_thread_running)) break;
        
        /* Generate samples if requested */
        int samples = atomic_load(&g_samples_requested);
        if (samples > 0) {
            /* Clear request */
            atomic_store(&g_samples_requested, 0);
            atomic_store(&g_generate_audio_request, false);
            
            /* Generate audio */
            if (samples > THREAD_AUDIO_BUFFER_SIZE) {
                samples = THREAD_AUDIO_BUFFER_SIZE;
            }
            
            /* Call the audio generator - this will fill our internal buffer */
            media_generate_audio_samples(samples);
            
            /* Mark data as ready */
            atomic_store(&g_audio_data_ready, true);
        }
        
        /* Always generate some audio to keep the buffer filled */
        usleep(2000); /* 2ms */
    }

    return NULL;
}

/* ============================================
 * Simple Audio Thread - Background Generation
 * ============================================ */

/* This thread continuously generates audio in the background */
static void* audio_thread_background_func(void* arg) {
    (void)arg;

    int samples_per_generate = 256;
    int generate_interval_us = 5000; /* 5ms between generations */

    while (atomic_load(&g_audio_thread_running)) {
        /* Generate samples in background */
        media_generate_audio_samples(samples_per_generate);
        
        /* Sleep to maintain reasonable CPU usage */
        usleep(generate_interval_us);
    }

    return NULL;
}

/* ============================================
 * Audio Buffer (Legacy - for direct queue from media.c)
 * ============================================ */

/* Small buffer - just enough for 2-3 frames to prevent overflow */
static int16_t g_audio_buffer[AUDIO_BUFFER_SIZE * 2];
static size_t g_audio_write_pos = 0;
static size_t g_audio_read_pos = 0;
static size_t g_audio_count = 0;

static uint64_t audio_get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static void audio_buffer_init(void) {
    if (!g_audio_initialized) {
        memset(g_audio_buffer, 0, sizeof(g_audio_buffer));
        g_audio_write_pos = 0;
        g_audio_read_pos = 0;
        g_audio_count = 0;
        g_audio_resample_pos = 0.0;
        g_audio_time_accumulator = 0.0;
        g_last_frame_time = audio_get_time_us();
        
        /* Initialize ring buffer for thread communication */
        if (!g_audio_ring.buffer) {
            audio_ring_init(&g_audio_ring, AUDIO_BUFFER_SIZE);
        }
        
        g_audio_initialized = true;
    }
}

/* Start the audio thread */
static void audio_thread_start(void) {
    if (atomic_load(&g_audio_thread_started)) return;
    
    atomic_store(&g_audio_thread_running, true);
    
    int result = pthread_create(&g_audio_thread, NULL, audio_thread_background_func, NULL);
    if (result == 0) {
        atomic_store(&g_audio_thread_started, true);
    }
}

/* Stop the audio thread */
static void audio_thread_stop(void) {
    if (!atomic_load(&g_audio_thread_started)) return;
    
    atomic_store(&g_audio_thread_running, false);
    
    /* Wait for thread to finish */
    pthread_join(g_audio_thread, NULL);

    atomic_store(&g_audio_thread_started, false);
}

void libretro_set_sample_rate(uint32_t rate) {
    g_frontend_sample_rate = rate;
    g_audio_resample_ratio = (double)AUDIO_SOURCE_RATE / (double)rate;
    g_audio_time_accumulator = 0.0;
    g_last_frame_time = audio_get_time_us();
}

void libretro_set_fps(int fps) {
    (void)fps;
}

static void resample_audio(const int16_t* input, size_t input_samples,
                           int16_t* output, size_t* output_samples,
                           double ratio, double* pos) {
    if (ratio <= 0) ratio = 1.0;
    size_t out_idx = 0;
    double src_pos = *pos;
    
    while (src_pos < (double)(input_samples - 1) && out_idx < *output_samples) {
        int idx0 = (int)src_pos;
        int idx1 = idx0 + 1;
        if (idx1 >= (int)input_samples) idx1 = idx0;
        double frac = src_pos - (double)idx0;
        output[out_idx * 2] = (int16_t)(input[idx0 * 2] * (1.0 - frac) + input[idx1 * 2] * frac);
        output[out_idx * 2 + 1] = (int16_t)(input[idx0 * 2 + 1] * (1.0 - frac) + input[idx1 * 2 + 1] * frac);
        src_pos += ratio;
        out_idx++;
    }
    *pos = src_pos - (double)input_samples;
    *output_samples = out_idx;
}

/* Called from audio thread or media.c to queue samples */
void sdl_audio_queue_samples(const int16_t* samples, size_t count) {
    if (!samples || count == 0) return;
    audio_buffer_init();
    
    /* Use thread-safe ring buffer if audio thread is running */
    if (atomic_load(&g_audio_thread_started)) {
        /* Write to ring buffer for main thread to read */
        audio_ring_write(&g_audio_ring, samples, count / 2);
    }
    
    /* Also write to legacy buffer for compatibility */
    if (g_audio_resample_ratio != 1.0 && g_audio_resample_ratio > 0) {
        size_t max_output = (size_t)(count / g_audio_resample_ratio) + 16;
        int16_t* resampled = (int16_t*)malloc(max_output * 2 * sizeof(int16_t));
        if (resampled) {
            size_t output_count = max_output;
            resample_audio(samples, count / 2, resampled, &output_count, 
                          g_audio_resample_ratio, &g_audio_resample_pos);
            for (size_t i = 0; i < output_count; i++) {
                if (g_audio_count >= AUDIO_BUFFER_SIZE) {
                    g_audio_read_pos = (g_audio_read_pos + 1) % AUDIO_BUFFER_SIZE;
                    g_audio_count--;
                }
                g_audio_buffer[g_audio_write_pos * 2] = resampled[i * 2];
                g_audio_buffer[g_audio_write_pos * 2 + 1] = resampled[i * 2 + 1];
                g_audio_write_pos = (g_audio_write_pos + 1) % AUDIO_BUFFER_SIZE;
                g_audio_count++;
            }
            free(resampled);
            return;
        }
    }
    
    size_t samples_to_write = count / 2;
    for (size_t i = 0; i < samples_to_write; i++) {
        if (g_audio_count >= AUDIO_BUFFER_SIZE) {
            g_audio_read_pos = (g_audio_read_pos + 1) % AUDIO_BUFFER_SIZE;
            g_audio_count--;
        }
        g_audio_buffer[g_audio_write_pos * 2] = samples[i * 2];
        g_audio_buffer[g_audio_write_pos * 2 + 1] = samples[i * 2 + 1];
        g_audio_write_pos = (g_audio_write_pos + 1) % AUDIO_BUFFER_SIZE;
        g_audio_count++;
    }
}

size_t sdl_audio_get_queued_size(void) { 
    /* Return count from ring buffer if audio thread is running */
    if (atomic_load(&g_audio_thread_started) && g_audio_ring.buffer) {
        return audio_ring_available(&g_audio_ring);
    }
    return g_audio_count; 
}

/* Main thread function - retrieves audio from audio thread and sends to frontend */
void libretro_process_audio(void) {
    extern retro_audio_sample_batch_t audio_batch_cb;
    if (!audio_batch_cb) return;
    
    /* Ensure audio thread is running */
    if (!atomic_load(&g_audio_thread_started)) {
        audio_buffer_init();
        audio_thread_start();
    }

    /* TIME-BASED SYNCHRONIZATION - the key to correct audio speed! */
    uint64_t current_time = audio_get_time_us();
    uint64_t elapsed_us = current_time - g_last_frame_time;
    g_last_frame_time = current_time;

    /* Clamp elapsed time to reasonable range (1ms - 100ms) */
    if (elapsed_us < 1000) elapsed_us = 1000;
    if (elapsed_us > 100000) elapsed_us = 100000;

    /* Calculate how many samples are needed for the elapsed time */
    double samples_needed = (double)g_frontend_sample_rate * (double)elapsed_us / 1000000.0;
    g_audio_time_accumulator += samples_needed;

    int samples_to_send = (int)g_audio_time_accumulator;
    if (samples_to_send < 1) return;
    g_audio_time_accumulator -= (double)samples_to_send;

    /* Clamp to reasonable range */
    if (samples_to_send > 2048) samples_to_send = 2048;

    /* Try to get samples from ring buffer first (from audio thread) */
    size_t available = 0;
    if (atomic_load(&g_audio_thread_started) && g_audio_ring.buffer) {
        available = audio_ring_available(&g_audio_ring);
    }
    
    /* Also check legacy buffer */
    size_t legacy_available = g_audio_count;
    
    /* Use whichever buffer has data */
    if (available > 0) {
        /* Read from ring buffer */
        if ((size_t)samples_to_send > available) {
            samples_to_send = (int)available;
        }
        
        /* Read samples from ring buffer */
        size_t read_count = audio_ring_read(&g_audio_ring, g_audio_output_buffer, samples_to_send);
        
        if (read_count > 0) {
            /* Send to frontend */
            audio_batch_cb(g_audio_output_buffer, read_count);
        }
    } else if (legacy_available > 0) {
        /* Fallback to legacy buffer */
        if ((size_t)samples_to_send > legacy_available) {
            samples_to_send = (int)legacy_available;
        }

        /* Send samples in chunks */
        int samples_sent = 0;
        while (samples_sent < samples_to_send) {
            int remaining = samples_to_send - samples_sent;
            int chunk = remaining > 256 ? 256 : remaining;

            size_t contiguous = AUDIO_BUFFER_SIZE - g_audio_read_pos;
            if (contiguous > (size_t)chunk) contiguous = (size_t)chunk;
            if (contiguous > g_audio_count) contiguous = g_audio_count;

            if (contiguous == 0) break;

            audio_batch_cb(&g_audio_buffer[g_audio_read_pos * 2], contiguous);
            g_audio_read_pos = (g_audio_read_pos + contiguous) % AUDIO_BUFFER_SIZE;
            g_audio_count -= contiguous;
            samples_sent += contiguous;
        }
    }
}

/* ============================================
 * Double Buffering - J2ME renders to ONE buffer
 * ============================================ */

static SdlContext g_libretro_context = {0};

/* Double buffer: J2ME always renders to buffer[0], we copy to buffer[1] for display */
#define BUFFER_COUNT 2
static uint32_t* g_buffers[BUFFER_COUNT] = {NULL, NULL};
static atomic_bool g_frame_ready = false;
static atomic_bool g_copying = false;

static int g_buffer_width = 0;
static int g_buffer_height = 0;
static size_t g_buffer_size = 0;

static bool init_buffers(int width, int height) {
    if (width <= 0 || height <= 0) {
        LOG_SAFE("[J2ME] Invalid buffer size: %dx%d\n", width, height);
        return false;
    }

    g_buffer_width = width;
    g_buffer_height = height;
    g_buffer_size = (size_t)width * (size_t)height * sizeof(uint32_t);

    for (int i = 0; i < BUFFER_COUNT; i++) {
        g_buffers[i] = (uint32_t*)malloc(g_buffer_size);
        if (!g_buffers[i]) {
            for (int j = 0; j < i; j++) {
                free(g_buffers[j]);
                g_buffers[j] = NULL;
            }
            LOG_SAFE("[J2ME] Failed to allocate buffer %d\n", i);
            return false;
        }
        memset(g_buffers[i], 0, g_buffer_size);
    }

    atomic_store(&g_frame_ready, false);
    atomic_store(&g_copying, false);

    /* J2ME always renders to buffer[0] */
    g_libretro_context.framebuffer = g_buffers[0];

    LOG_SAFE("[J2ME] Double buffer initialized: %dx%d\n", width, height);
    return true;
}

/* Resize buffers - called when resolution changes at runtime */
bool libretro_resize_buffers(int width, int height) {
    if (width <= 0 || height <= 0) {
        LOG_SAFE("[J2ME] Invalid resize dimensions: %dx%d\n", width, height);
        return false;
    }

    /* Check if size actually changed */
    if (width == g_buffer_width && height == g_buffer_height) {
        return true;  /* No change needed */
    }

    LOG_SAFE("[J2ME] Resizing buffers from %dx%d to %dx%d\n",
            g_buffer_width, g_buffer_height, width, height);

    /* Free old buffers */
    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (g_buffers[i]) {
            free(g_buffers[i]);
            g_buffers[i] = NULL;
        }
    }

    /* Allocate new buffers */
    return init_buffers(width, height);
}

/* Called at START of frame - nothing to do */
void libretro_begin_frame(void) {
    /* J2ME renders incrementally, don't clear */
}

/* Called at END of frame - copy render buffer to display buffer */
void libretro_end_frame(void) {
    if (!g_buffers[0] || !g_buffers[1]) return;
    
    /* Wait if previous copy is still being read */
    while (atomic_load(&g_copying)) {
        /* Busy wait - should be very short */
    }
    
    /* Copy buffer[0] (render) to buffer[1] (display) */
    atomic_store(&g_copying, true);
    atomic_thread_fence(memory_order_release);
    
    memcpy(g_buffers[1], g_buffers[0], g_buffer_size);
    
    atomic_thread_fence(memory_order_release);
    atomic_store(&g_frame_ready, true);
    atomic_store(&g_copying, false);
}

/* Get buffer for display - always return buffer[1] */
bool libretro_get_display_buffer(uint32_t** buffer, int* width, int* height) {
    if (!g_buffers[1]) {
        *buffer = libretro_get_framebuffer();
        *width = g_libretro_context.width > 0 ? g_libretro_context.width : 240;
        *height = g_libretro_context.height > 0 ? g_libretro_context.height : 320;
        return true;
    }
    
    /* Wait for copy to complete if in progress */
    while (atomic_load(&g_copying)) {
        /* Busy wait */
    }
    
    atomic_thread_fence(memory_order_acquire);
    
    /* Always return display buffer[1] */
    *buffer = g_buffers[1];
    *width = g_buffer_width > 0 ? g_buffer_width : 240;
    *height = g_buffer_height > 0 ? g_buffer_height : 320;
    
    return true;
}

SdlContext* sdl_get_global_context(void) { return &g_libretro_context; }

void sdl_set_global_context(SdlContext* ctx) {
    if (ctx) {
        g_libretro_context = *ctx;
    } else {
        memset(&g_libretro_context, 0, sizeof(SdlContext));
    }
}

int sdl_init(JVM* jvm, int width, int height, int scale, bool headless) {
    (void)headless; (void)scale;
    
    if (width <= 0 || height <= 0) {
        LOG_SAFE("[J2ME] Invalid dimensions %dx%d, using defaults\n", width, height);
        width = 240;
        height = 320;
    }
    
    g_libretro_context.jvm = jvm;
    g_libretro_context.width = width;
    g_libretro_context.height = height;
    g_libretro_context.scale = 1;
    g_libretro_context.target_fps = 30;
    g_libretro_context.running = true;
    
    if (!init_buffers(width, height)) {
        uint32_t* fb = (uint32_t*)libretro_get_framebuffer();
        if (fb) {
            g_libretro_context.framebuffer = fb;
            LOG_SAFE("[J2ME] Using single buffer fallback\n");
        } else {
            LOG_SAFE("[J2ME] ERROR: No framebuffer available!\n");
        }
    }
    /* framebuffer already set to g_buffers[0] in init_buffers */
    
    /* Initialize audio and start audio thread */
    audio_buffer_init();
    audio_thread_start();
    
    return 0;
}

void sdl_destroy(SdlContext* ctx) {
    (void)ctx;
    
    /* Stop audio thread */
    audio_thread_stop();
    
    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (g_buffers[i]) free(g_buffers[i]);
        g_buffers[i] = NULL;
    }
    g_buffer_size = 0;
    
    /* Destroy ring buffer */
    audio_ring_destroy(&g_audio_ring);
    g_audio_initialized = false;
    
    LOG_SAFE("[J2ME] Destroyed\n");
}

void sdl_run(SdlContext* ctx) { (void)ctx; }
void sdl_set_fullscreen(SdlContext* ctx, bool fullscreen) { (void)ctx; (void)fullscreen; }
uint64_t sdl_get_ticks(SdlContext* ctx) { (void)ctx; return 0; }
void sdl_delay(uint32_t ms) { (void)ms; }

/* Update screen - called multiple times during frame, DON'T swap here! */
void sdl_update_screen(SdlContext* ctx) {
    /* Do nothing here - we swap only in libretro_end_frame */
    (void)ctx;
}

void sdl_handle_events(SdlContext* ctx) { (void)ctx; }

bool sdl_key_pressed(SdlContext* ctx, int key) {
    (void)ctx;
    extern int libretro_get_key_states(void);
    int keys = libretro_get_key_states();
    
    switch (key) {
        case 1:  return (keys & (1 << 1)) != 0;
        case 6:  return (keys & (1 << 6)) != 0;
        case 2:  return (keys & (1 << 2)) != 0;
        case 5:  return (keys & (1 << 5)) != 0;
        case 8:  return (keys & (1 << 8)) != 0;
        case 9:  return (keys & (1 << 9)) != 0;
        case 10: return (keys & (1 << 10)) != 0;
        case 11: return (keys & (1 << 11)) != 0;
        case 12: return (keys & (1 << 12)) != 0;
        default: return false;
    }
}

MidpGraphics* sdl_get_graphics(SdlContext* ctx) { (void)ctx; return NULL; }
int sdl_audio_init_simple(uint32_t sample_rate) { (void)sample_rate; audio_buffer_init(); return 0; }

void sdl_audio_shutdown(void) {
    audio_thread_stop();
    g_audio_initialized = false;
    g_audio_write_pos = 0;
    g_audio_read_pos = 0;
    g_audio_count = 0;
    audio_ring_destroy(&g_audio_ring);
}

/* JAR resource loading */
uint8_t* load_jar_resource(const char* path, size_t* size) {
    if (size) *size = 0;
    if (!path) return NULL;
    extern JVM* g_jvm;
    if (!g_jvm || !g_jvm->class_loader.jar_data) return NULL;
    
    const uint8_t* jar_data = g_jvm->class_loader.jar_data;
    size_t jar_size = g_jvm->class_loader.jar_size;
    
    size_t eocd = 0;
    for (size_t i = jar_size - 22; i > 0; i--) {
        if (jar_data[i] == 0x50 && jar_data[i+1] == 0x4B &&
            jar_data[i+2] == 0x05 && jar_data[i+3] == 0x06) {
            eocd = i; break;
        }
    }
    if (eocd == 0) return NULL;
    
    uint32_t cd_start = jar_data[eocd + 16] | (jar_data[eocd + 17] << 8) |
                        (jar_data[eocd + 18] << 16) | (jar_data[eocd + 19] << 24);
    uint16_t cd_entries = jar_data[eocd + 10] | (jar_data[eocd + 11] << 8);
    
    size_t cde = cd_start;
    for (int i = 0; i < cd_entries && cde < eocd; i++) {
        uint32_t sig = jar_data[cde] | (jar_data[cde+1] << 8) |
                       (jar_data[cde+2] << 16) | (jar_data[cde+3] << 24);
        if (sig != 0x02014B50) break;
        
        uint16_t compression = jar_data[cde + 10] | (jar_data[cde + 11] << 8);
        uint32_t comp_size = jar_data[cde + 20] | (jar_data[cde + 21] << 8) |
                             (jar_data[cde + 22] << 16) | (jar_data[cde + 23] << 24);
        uint32_t uncomp_size = jar_data[cde + 24] | (jar_data[cde + 25] << 8) |
                               (jar_data[cde + 26] << 16) | (jar_data[cde + 27] << 24);
        uint16_t name_len = jar_data[cde + 28] | (jar_data[cde + 29] << 8);
        uint16_t extra_len = jar_data[cde + 30] | (jar_data[cde + 31] << 8);
        uint16_t comment_len = jar_data[cde + 32] | (jar_data[cde + 33] << 8);
        uint32_t local_off = jar_data[cde + 42] | (jar_data[cde + 43] << 8) |
                             (jar_data[cde + 44] << 16) | (jar_data[cde + 45] << 24);
        
        const char* name = (const char*)(jar_data + cde + 46);
        if (name_len == strlen(path) && memcmp(name, path, name_len) == 0) {
            uint16_t local_name_len = jar_data[local_off + 26] | (jar_data[local_off + 27] << 8);
            uint16_t local_extra_len = jar_data[local_off + 28] | (jar_data[local_off + 29] << 8);
            size_t data_off = local_off + 30 + local_name_len + local_extra_len;
            
            if (compression == 0) {
                *size = uncomp_size;
                uint8_t* data = (uint8_t*)malloc(*size);
                if (data) memcpy(data, jar_data + data_off, *size);
                return data;
            } else if (compression == 8) {
                uint8_t* data = (uint8_t*)malloc(uncomp_size);
                if (!data) return NULL;
                z_stream stream;
                memset(&stream, 0, sizeof(stream));
                if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) { free(data); return NULL; }
                stream.next_in = (Bytef*)(jar_data + data_off);
                stream.avail_in = comp_size;
                stream.next_out = data;
                stream.avail_out = uncomp_size;
                if (inflate(&stream, Z_FINISH) != Z_STREAM_END) {
                    inflateEnd(&stream); free(data); return NULL;
                }
                inflateEnd(&stream);
                *size = uncomp_size;
                return data;
            }
        }
        cde += 46 + name_len + extra_len + comment_len;
    }
    return NULL;
}

void sdl_present(SdlContext* ctx) { (void)ctx; }

/* Stubs */
void sdl_request_redraw(void) { g_libretro_context.needs_redraw = true; }
bool sdl_needs_redraw(SdlContext* ctx) { return ctx ? ctx->needs_redraw : false; }
void sdl_clear_redraw(SdlContext* ctx) { if (ctx) ctx->needs_redraw = false; }
void sdl_process_events_minimal(void) {}
void sdl_update_texture(SdlContext* ctx) { (void)ctx; }

void sdl_clear(SdlContext* ctx, uint32_t color) {
    if (!ctx || !ctx->framebuffer) return;
    int size = ctx->width * ctx->height;
    for (int i = 0; i < size; i++) ctx->framebuffer[i] = color;
}

int sdl_resize(SdlContext* ctx, int width, int height) { (void)ctx; (void)width; (void)height; return 0; }
int sdl_screenshot(SdlContext* ctx, const char* filename) { (void)ctx; (void)filename; return -1; }
void sdl_frame_end(SdlContext* ctx) { (void)ctx; }
void sdl_sleep(uint32_t ms) { (void)ms; }
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
    (void)ctx; (void)frequency; (void)channels; (void)samples; audio_buffer_init(); return 0;
}
int sdl_audio_queue(SdlContext* ctx, const void* data, size_t length) { (void)ctx; (void)data; (void)length; return 0; }
uint32_t sdl_audio_queued_size(SdlContext* ctx) { (void)ctx; return (uint32_t)g_audio_count; }
void sdl_audio_clear(SdlContext* ctx) { (void)ctx; g_audio_read_pos = 0; g_audio_write_pos = 0; g_audio_count = 0; }
void sdl_process_events(SdlContext* ctx) { (void)ctx; }
int sdl_key_to_midp(int sdl_key) { (void)sdl_key; return 0; }
int sdl_key_to_game_action(int sdl_key) { (void)sdl_key; return 0; }
void sdl_get_pointer(SdlContext* ctx, int* x, int* y) { if (x) *x = 0; if (y) *y = 0; (void)ctx; }
bool sdl_pointer_pressed(SdlContext* ctx) { (void)ctx; return false; }
void sdl_dump_info(SdlContext* ctx) { (void)ctx; }

/* g_j2me_runtime_debug defined in jvm/debug_var.c */
void sdl_save_framebuffer_to_file(SdlContext* ctx, const char* filename) { (void)ctx; (void)filename; }

/* ============================================
 * Error Display Implementation for Libretro
 * ============================================ */

#include "../midp/bitmap_font.h"

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
    } else {
        g_error_title[0] = '\0';
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
    
    /* Detailed log output */
    LOG_SAFE("\n========================================\n");
    LOG_SAFE("  [J2ME UNCAUGHT EXCEPTION]\n");
    LOG_SAFE("========================================\n");
    LOG_SAFE("  Exception: %s\n", g_error_title);
    if (g_error_message[0]) {
        LOG_SAFE("  Message:   %s\n", g_error_message);
    }
    if (g_error_extra[0]) {
        LOG_SAFE("  Details:   %s\n", g_error_extra);
    }
    if (g_error_stack[0]) {
        LOG_SAFE("  Stack:\n%s", g_error_stack);
    }
    LOG_SAFE("========================================\n\n");
}

/* Set extra detail info (thread name, PC, etc.) */
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

/* Draw a single character on framebuffer */
static void draw_char(uint32_t* fb, int fb_width, int fb_height, int x, int y, char c, uint32_t color) {
    if (x < 0 || y < 0 || x + FONT_WIDTH >= fb_width || y + FONT_HEIGHT >= fb_height) return;
    
    const uint8_t* char_data = get_char_data(c);
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t row_data = char_data[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (row_data & (1 << (4 - col))) {
                fb[(y + row) * fb_width + (x + col)] = color;
            }
        }
    }
}

/* Draw a UTF-8 string on framebuffer with word wrap */
static void draw_string(uint32_t* fb, int fb_width, int fb_height, int* x, int* y, const char* str, uint32_t color, int max_width) {
    if (!str || !fb) return;
    
    int len = 0;
    while (str[len]) len++;
    
    int i = 0;
    int start_x = *x;
    int char_width = FONT_WIDTH + 1;
    
    while (i < len) {
        int cp = utf8_decode(str, &i, len);
        if (cp < 0) continue;
        
        /* Handle newlines */
        if (cp == '\n') {
            *x = start_x;
            *y += FONT_HEIGHT + 2;
            continue;
        }
        
        /* Word wrap check */
        if (*x + char_width >= start_x + max_width) {
            *x = start_x;
            *y += FONT_HEIGHT + 2;
        }
        
        /* Check if we're still within bounds */
        if (*y + FONT_HEIGHT >= fb_height - 10) break;
        
        /* Draw the character */
        const uint8_t* char_data = get_char_data_unicode(cp);
        for (int row = 0; row < FONT_HEIGHT; row++) {
            uint8_t row_data = char_data[row];
            for (int col = 0; col < FONT_WIDTH; col++) {
                if (row_data & (1 << (4 - col))) {
                    int px = *x + col;
                    int py = *y + row;
                    if (px >= 0 && px < fb_width && py >= 0 && py < fb_height) {
                        fb[py * fb_width + px] = color;
                    }
                }
            }
        }
        
        *x += char_width;
    }
    
    *y += FONT_HEIGHT + 2;
}

/* Draw horizontal line */
static void draw_hline(uint32_t* fb, int fb_width, int y, int x1, int x2, uint32_t color) {
    if (y < 0) return;
    for (int x = x1; x < x2 && x < fb_width; x++) {
        if (x >= 0) fb[y * fb_width + x] = color;
    }
}

void sdl_draw_error_screen(SdlContext* ctx) {
    if (!ctx || !ctx->framebuffer) return;
    
    int width = ctx->width > 0 ? ctx->width : 240;
    int height = ctx->height > 0 ? ctx->height : 320;
    uint32_t* fb = ctx->framebuffer;
    
    /* Colors: ARGB format */
    uint32_t bg_color = 0xFF1A1A2E;    /* Dark blue background */
    uint32_t header_bg = 0xFF16213E;   /* Slightly lighter header */
    uint32_t border_color = 0xFFE94560; /* Red-pink accent */
    uint32_t title_color = 0xFFFFFFFF;  /* White title */
    uint32_t exc_color = 0xFFFF6B6B;    /* Red for exception name */
    uint32_t msg_color = 0xFFFFFF00;    /* Yellow text for message */
    uint32_t detail_color = 0xFF87CEEB; /* Light blue for details */
    uint32_t stack_color = 0xFFCCCCCC;  /* Light gray stack trace */
    uint32_t throw_color = 0xFFFF6B6B;  /* Red for throwing frame */
    uint32_t help_color = 0xFF888888;   /* Dim help text */
    
    /* Fill background */
    for (int i = 0; i < width * height; i++) {
        fb[i] = bg_color;
    }
    
    /* Draw header background (top 12px) */
    for (int hy = 0; hy < 14; hy++) {
        draw_hline(fb, width, hy, 0, width, header_bg);
    }
    
    /* Draw top/bottom border lines */
    draw_hline(fb, width, 0, 0, width, border_color);
    draw_hline(fb, width, 13, 0, width, border_color);
    draw_hline(fb, width, height - 1, 0, width, border_color);
    
    int y_pos = 3;
    int x_pos = 5;
    
    /* Draw header: "!! EXCEPTION !!" */
    draw_string(fb, width, height, &x_pos, &y_pos, "!! EXCEPTION !!", exc_color, width - 10);
    
    y_pos += 4;
    
    /* Draw exception class name (prominently) */
    if (g_error_title[0]) {
        x_pos = 5;
        draw_string(fb, width, height, &x_pos, &y_pos, g_error_title, exc_color, width - 10);
        y_pos += 2;
    }
    
    /* Separator */
    draw_hline(fb, width, y_pos, 2, width - 2, 0xFF333355);
    y_pos += 4;
    
    /* Draw message */
    if (g_error_message[0]) {
        x_pos = 5;
        /* Draw "Message:" label */
        draw_string(fb, width, height, &x_pos, &y_pos, "Msg:", detail_color, width - 10);
        x_pos = 5;
        draw_string(fb, width, height, &x_pos, &y_pos, g_error_message, msg_color, width - 10);
        y_pos += 2;
    }
    
    /* Draw extra details (thread name, etc.) */
    if (g_error_extra[0]) {
        x_pos = 5;
        draw_string(fb, width, height, &x_pos, &y_pos, g_error_extra, detail_color, width - 10);
        y_pos += 2;
    }
    
    /* Separator before stack trace */
    draw_hline(fb, width, y_pos, 2, width - 2, 0xFF333355);
    y_pos += 4;
    
    /* Draw stack trace with throwing frame highlighted */
    if (g_error_stack[0]) {
        x_pos = 5;
        /* Count lines in stack trace to decide layout */
        int stack_lines = 0;
        const char* p = g_error_stack;
        while (*p) {
            if (*p == '\n') stack_lines++;
            p++;
        }
        if (p > g_error_stack) stack_lines++;
        
        /* Draw each line of stack trace */
        /* First line (throwing frame) in red, rest in gray */
        char stack_copy[8192];
        strncpy(stack_copy, g_error_stack, sizeof(stack_copy) - 1);
        stack_copy[sizeof(stack_copy) - 1] = '\0';
        
        char* line = strtok(stack_copy, "\n");
        int line_num = 0;
        while (line && y_pos + FONT_HEIGHT < height - 18) {
            x_pos = 5;
            uint32_t line_color = (line_num == 0) ? throw_color : stack_color;
            draw_string(fb, width, height, &x_pos, &y_pos, line, line_color, width - 10);
            line = strtok(NULL, "\n");
            line_num++;
        }
        
        /* If stack was truncated, show count */
        if (line != NULL && y_pos + FONT_HEIGHT < height - 18) {
            x_pos = 5;
            char more_buf[64];
            snprintf(more_buf, sizeof(more_buf), "  ... +%d more frames", stack_lines - line_num);
            draw_string(fb, width, height, &x_pos, &y_pos, more_buf, help_color, width - 10);
        }
    }
    
    /* Draw help text at bottom */
    y_pos = height - 15;
    x_pos = 5;
    draw_string(fb, width, height, &x_pos, &y_pos, "SELECT=exit", help_color, width - 10);
}
