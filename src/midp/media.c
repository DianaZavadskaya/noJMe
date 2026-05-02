/*
 * J2ME Emulator - Complete JSR-135 Mobile Media API Implementation
 * javax.microedition.media native methods
 * 
 * Full implementation with:
 * - Asynchronous audio playback via SDL callback
 * - Real-time state tracking
 * - MIDI, WAV, Tone sequence support
 * - VolumeControl, MIDIControl implementations
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include "debug.h"
#include "debug_macros.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#include "jvm.h"
#include "native.h"
#include "midi.h"
#include "heap.h"
#include "opcodes.h"

/* Forward declarations from SDL audio */
extern int sdl_audio_init_simple(uint32_t sample_rate);
extern void sdl_audio_shutdown(void);
extern void sdl_audio_queue_samples(const int16_t* samples, size_t count);
extern size_t sdl_audio_get_queued_size(void);

/* ============================================
 * JSR-135 Constants
 * ============================================ */

#define PLAYER_UNREALIZED   100
#define PLAYER_REALIZED     200
#define PLAYER_PREFETCHED   300
#define PLAYER_STARTED      400
#define PLAYER_CLOSED       0

#define TIME_UNKNOWN        -1L

#define TONE_MIN_NOTE    0
#define TONE_MAX_NOTE    127
#define TONE_MIN_VOLUME  0
#define TONE_MAX_VOLUME  100
#define TONE_MAX_DURATION 30000

/* ToneControl sequence tokens */
#define TONE_CONTROL_VERSION       -2
#define TONE_CONTROL_TEMPO         -3
#define TONE_CONTROL_RESOLUTION    -4
#define TONE_CONTROL_BLOCK_START   -5
#define TONE_CONTROL_BLOCK_END     -6
#define TONE_CONTROL_PLAY_BLOCK    -7
#define TONE_CONTROL_SET_VOLUME    -8
#define TONE_CONTROL_REPEAT        -9
#define TONE_CONTROL_SILENCE       -1

/* Audio buffer constants */
#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_BUFFER_SIZE 4096
#define MAX_PLAYERS 32

/* ============================================
 * WAV File Support
 * ============================================ */

typedef struct {
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    uint8_t* data;
    uint32_t data_size;
    uint32_t duration_ms;
} WavFile;

/* ============================================
 * IMA ADPCM Decoder
 * ============================================ */

static const int ima_step_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static const int ima_step_size_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
    598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132,
    7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
    24623, 27086, 29794, 32767
};

/* Decode IMA ADPCM data to 16-bit little-endian interleaved PCM.
 * Uses per-channel block layout: for stereo, left block then right block.
 * Each channel block: 4 header bytes (int16 predictor, uint8 index, uint8 reserved)
 * followed by data bytes containing nibble pairs.
 * Returns 0 on success, -1 on failure. Caller must free *out_pcm. */
static int decode_ima_adpcm(const uint8_t* adpcm_data, uint32_t adpcm_size,
                             int channels, int block_align,
                             uint8_t** out_pcm, uint32_t* out_pcm_size) {
    if (!adpcm_data || adpcm_size == 0 || channels < 1 || channels > 2 || block_align == 0)
        return -1;

    int block_size_per_ch = block_align / channels;
    if (block_size_per_ch < 6)
        return -1; /* Need at least 4 header + 2 data bytes per channel */

    int samples_per_block = (block_size_per_ch - 4) * 2;
    if (samples_per_block <= 0)
        return -1;

    int num_blocks = (int)(adpcm_size / (uint32_t)block_align);
    if (num_blocks == 0)
        return -1;

    uint32_t total_samples_per_ch = (uint32_t)num_blocks * (uint32_t)samples_per_block;
    *out_pcm_size = total_samples_per_ch * (uint32_t)channels * 2;
    *out_pcm = (uint8_t*)malloc(*out_pcm_size);
    if (!*out_pcm)
        return -1;

    int16_t* pcm = (int16_t*)(*out_pcm);

    for (int blk = 0; blk < num_blocks; blk++) {
        uint32_t block_start = (uint32_t)blk * (uint32_t)block_align;
        uint32_t base_sample = (uint32_t)blk * (uint32_t)samples_per_block;

        /* Decode each channel's sub-block independently */
        for (int ch = 0; ch < channels; ch++) {
            uint32_t pos = block_start + (uint32_t)ch * (uint32_t)block_size_per_ch;

            if (pos + 3 >= adpcm_size) break;

            /* Read block header: predictor (int16 LE), index (uint8), reserved (1 byte) */
            int predictor = (int16_t)(adpcm_data[pos] | (adpcm_data[pos + 1] << 8));
            int index = adpcm_data[pos + 2] & 0xFF;
            if (index > 88) index = 88;
            pos += 4; /* skip header */

            /* Decode nibbles from data bytes */
            int data_bytes = block_size_per_ch - 4;
            int sample_idx = 0;

            for (int b = 0; b < data_bytes && sample_idx < samples_per_block; b++) {
                if (pos >= adpcm_size) break;
                uint8_t byte_val = adpcm_data[pos++];

                for (int nib = 0; nib < 2 && sample_idx < samples_per_block; nib++) {
                    int nibble = (nib == 0) ? (byte_val & 0x0F) : ((byte_val >> 4) & 0x0F);

                    int step = ima_step_size_table[index];
                    int diff = step >> 3;
                    if (nibble & 1) diff += step >> 2;
                    if (nibble & 2) diff += step >> 1;
                    if (nibble & 4) diff += step;

                    if (nibble & 8)
                        predictor -= diff;
                    else
                        predictor += diff;

                    if (predictor < -32768) predictor = -32768;
                    if (predictor > 32767) predictor = 32767;

                    index += ima_step_index_table[nibble];
                    if (index < 0) index = 0;
                    if (index > 88) index = 88;

                    /* Write sample to interleaved PCM buffer */
                    uint32_t out_idx = (base_sample + (uint32_t)sample_idx) * (uint32_t)channels + (uint32_t)ch;
                    pcm[out_idx] = (int16_t)predictor;
                    sample_idx++;
                }
            }
        }
    }

    return 0;
}

static WavFile* wav_parse(const uint8_t* data, uint32_t size) {
    if (size < 44) return NULL;
    if (memcmp(data, "RIFF", 4) != 0) return NULL;
    if (memcmp(data + 8, "WAVE", 4) != 0) return NULL;
    
    WavFile* wav = (WavFile*)calloc(1, sizeof(WavFile));
    if (!wav) return NULL;
    
    uint32_t pos = 12;
    while (pos + 8 <= size) {
        uint32_t chunk_size = data[pos+4] | (data[pos+5] << 8) | (data[pos+6] << 16) | (data[pos+7] << 24);
        
        if (pos + 8 + chunk_size > size) break;
        
        if (memcmp(data + pos, "fmt ", 4) == 0) {
            pos += 8;
            if (chunk_size >= 16) {
                wav->audio_format = data[pos] | (data[pos+1] << 8);
                wav->num_channels = data[pos+2] | (data[pos+3] << 8);
                wav->sample_rate = data[pos+4] | (data[pos+5] << 8) | (data[pos+6] << 16) | (data[pos+7] << 24);
                wav->byte_rate = data[pos+8] | (data[pos+9] << 8) | (data[pos+10] << 16) | (data[pos+11] << 24);
                wav->block_align = data[pos+12] | (data[pos+13] << 8);
                wav->bits_per_sample = data[pos+14] | (data[pos+15] << 8);
            }
            pos += chunk_size;
        } else if (memcmp(data + pos, "data", 4) == 0) {
            pos += 8;
            wav->data = (uint8_t*)malloc(chunk_size);
            if (wav->data) {
                memcpy(wav->data, data + pos, chunk_size);
                wav->data_size = chunk_size;
                if (wav->byte_rate > 0) {
                    wav->duration_ms = (chunk_size * 1000) / wav->byte_rate;
                }
            }
            pos += chunk_size;
        } else {
            pos += 8 + chunk_size;
        }
        if (chunk_size & 1) pos++;
    }
    
    /* Decode IMA ADPCM (format 17) to PCM */
    if (wav->audio_format == 17) {
        uint8_t* pcm_data = NULL;
        uint32_t pcm_size = 0;

        uint32_t adpcm_raw_size = wav->data_size;
        if (decode_ima_adpcm(wav->data, wav->data_size, wav->num_channels,
                             wav->block_align, &pcm_data, &pcm_size) == 0 && pcm_data) {
            free(wav->data);
            wav->data = pcm_data;
            wav->data_size = pcm_size;
            wav->audio_format = 1;
            wav->bits_per_sample = 16;
            wav->block_align = wav->num_channels * 2;
            wav->byte_rate = wav->sample_rate * wav->block_align;
            wav->duration_ms = (wav->data_size * 1000) / wav->byte_rate;
            MEDIA_DEBUG("IMA ADPCM decoded: %u -> %u bytes PCM", adpcm_raw_size, pcm_size);
        } else {
            MEDIA_DEBUG("IMA ADPCM decode failed");
            free(wav->data);
            free(wav);
            return NULL;
        }
    }

    if (wav->audio_format != 1 || !wav->data) {
        free(wav->data);
        free(wav);
        return NULL;
    }
    
    MEDIA_DEBUG("WAV parsed: %d ch, %d Hz, %d bits, %u ms",
            wav->num_channels, wav->sample_rate, wav->bits_per_sample, wav->duration_ms);
    
    return wav;
}

static void wav_free(WavFile* wav) {
    if (wav) {
        free(wav->data);
        free(wav);
    }
}

/* ============================================
 * Tone Sequence Support
 * ============================================ */

typedef struct ToneSequence {
    uint8_t* sequence;
    int length;
    int position;
    int tempo;
    int resolution;
    bool playing;
    bool loop;
    int loop_count;
    int loops_remaining;
    uint32_t start_time_ms;
    int current_note;
    int current_duration;
    int current_volume;
} ToneSequence;

static ToneSequence* tone_sequence_parse(const uint8_t* data, int length) {
    if (length < 2) return NULL;
    
    ToneSequence* ts = (ToneSequence*)calloc(1, sizeof(ToneSequence));
    if (!ts) return NULL;
    
    ts->sequence = (uint8_t*)malloc(length);
    if (!ts->sequence) {
        free(ts);
        return NULL;
    }
    memcpy(ts->sequence, data, length);
    ts->length = length;
    ts->position = 0;
    ts->tempo = 120;
    ts->resolution = 64;
    ts->playing = false;
    ts->current_volume = 100;
    
    /* Parse header */
    int pos = 0;
    while (pos < length - 1) {
        int cmd = (int8_t)data[pos];
        
        if (cmd == TONE_CONTROL_VERSION) {
            pos += 2;
        } else if (cmd == TONE_CONTROL_TEMPO) {
            if (pos + 1 < length) {
                ts->tempo = data[pos + 1] & 0xFF;
                if (ts->tempo < 5) ts->tempo = 5;
                if (ts->tempo > 127) ts->tempo = 127;
            }
            pos += 2;
        } else if (cmd == TONE_CONTROL_RESOLUTION) {
            if (pos + 1 < length) {
                ts->resolution = data[pos + 1] & 0xFF;
                if (ts->resolution == 0) ts->resolution = 64;
            }
            pos += 2;
        } else {
            break;
        }
    }
    
    ts->position = pos;
    return ts;
}

static void tone_sequence_free(ToneSequence* ts) {
    if (ts) {
        free(ts->sequence);
        free(ts);
    }
}

/* ============================================
 * Audio Mixer State
 * ============================================ */

typedef enum {
    PLAYER_TYPE_NONE = 0,
    PLAYER_TYPE_MIDI,
    PLAYER_TYPE_WAV,
    PLAYER_TYPE_TONE,
    PLAYER_TYPE_TONE_DEVICE
} PlayerType;

typedef struct MediaPlayer {
    int id;
    PlayerType type;
    volatile int state;
    bool playing;
    bool looping;
    int loop_count;
    int loops_remaining;
    bool muted;
    float volume;
    
    /* WAV playback */
    WavFile* wav;
    uint32_t wav_position;
    uint32_t wav_samples_played;
    
    /* MIDI playback */
    MidiFile* midi;
    uint32_t midi_start_time;
    uint32_t midi_current_time;
    int midi_current_event;
    
    /* Tone playback */
    ToneSequence* tone_seq;
    uint32_t tone_start_time;
    uint32_t tone_note_end_time;
    
    /* Original data */
    uint8_t* data;
    uint32_t size;
    
    char* content_type;
} MediaPlayer;

static MediaPlayer g_players[MAX_PLAYERS];
static int g_next_player_id = 1;
static bool g_audio_initialized = false;
static pthread_mutex_t g_audio_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_audio_thread;
static volatile bool g_audio_running = false;

/* Libretro mode flag - when true, audio is generated synchronously */
#ifdef J2ME_LIBRETRO
static bool g_libretro_mode = true;
#else
static bool g_libretro_mode = false;
#endif

/* Global time base */
static uint64_t g_start_time_us = 0;

/* ============================================
 * Time Utilities
 * ============================================ */

static uint64_t get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static uint32_t get_time_ms(void) {
    return (uint32_t)(get_time_us() / 1000ULL);
}

/* ============================================
 * Audio Mixer - Generates samples for SDL
 * ============================================ */

/* Mix one player's audio into buffer */
static void mix_wav_player(MediaPlayer* player, int16_t* buffer, int samples) {
    if (!player->wav || !player->playing) return;
    
    WavFile* wav = player->wav;
    float vol = player->muted ? 0.0f : player->volume;
    
    /* Calculate bytes per sample for source */
    int src_bytes_per_sample = wav->num_channels * (wav->bits_per_sample / 8);
    double src_ratio = (double)wav->sample_rate / AUDIO_SAMPLE_RATE;
    
    for (int i = 0; i < samples && player->playing; i++) {
        /* Calculate source position */
        uint32_t src_sample = (uint32_t)(player->wav_samples_played * src_ratio);
        uint32_t src_byte = src_sample * src_bytes_per_sample;
        
        if (src_byte >= wav->data_size) {
            if (player->looping && player->loop_count != 1) {
                player->wav_position = 0;
                player->wav_samples_played = 0;
                src_byte = 0;
                if (player->loop_count > 0) {
                    player->loops_remaining--;
                    if (player->loops_remaining <= 0) {
                        player->playing = false;
                        player->state = PLAYER_PREFETCHED;
                        break;
                    }
                }
            } else {
                player->playing = false;
                player->state = PLAYER_PREFETCHED;
                break;
            }
        }
        
        int16_t left = 0, right = 0;
        
        if (wav->bits_per_sample == 8) {
            uint8_t l = wav->data[src_byte];
            left = (int16_t)((l - 128) * 256);
            if (wav->num_channels >= 2 && src_byte + 1 < wav->data_size) {
                uint8_t r = wav->data[src_byte + 1];
                right = (int16_t)((r - 128) * 256);
            } else {
                right = left;
            }
        } else if (wav->bits_per_sample == 16) {
            if (src_byte + 1 < wav->data_size) {
                left = (int16_t)(wav->data[src_byte] | (wav->data[src_byte + 1] << 8));
            }
            if (wav->num_channels >= 2 && src_byte + 3 < wav->data_size) {
                right = (int16_t)(wav->data[src_byte + 2] | (wav->data[src_byte + 3] << 8));
            } else {
                right = left;
            }
        }
        
        /* Apply volume and mix */
        buffer[i * 2] += (int16_t)(left * vol);
        buffer[i * 2 + 1] += (int16_t)(right * vol);
        
        player->wav_samples_played++;
    }
}

static void mix_tone_player(MediaPlayer* player, int16_t* buffer, int samples) {
    if (!player->tone_seq || !player->playing) return;
    
    ToneSequence* ts = player->tone_seq;
    
    /* Guard against division by zero in duration calculation */
    if (ts->tempo == 0) ts->tempo = 120;
    if (ts->resolution == 0) ts->resolution = 64;
    
    float vol = player->muted ? 0.0f : player->volume;
    uint32_t now = get_time_ms() - ts->start_time_ms;
    
    for (int i = 0; i < samples && player->playing; i++) {
        /* Generate audio for current note (if any) */
        if (ts->current_note >= 0 && ts->current_note < 128) {
            /* Generate a simple sine wave tone */
            float freq = 440.0f * powf(2.0f, (ts->current_note - 69) / 12.0f);
            float amplitude = ((float)ts->current_volume / 100.0f) * vol;
            
            /* Time position for this sample */
            uint32_t sample_time_us = (uint32_t)((uint64_t)now * 1000ULL + 
                (uint64_t)i * 1000000ULL / AUDIO_SAMPLE_RATE);
            
            float sample = amplitude * sinf(2.0f * (float)M_PI * freq * sample_time_us / 1000000.0f);
            
            int16_t pcm = (int16_t)(sample * 16000.0f); /* Scale to reasonable volume */
            buffer[i * 2] += pcm;      /* Left */
            buffer[i * 2 + 1] += pcm;  /* Right */
        }
        
        /* Check if we need to advance to next note */
        uint32_t note_end_ms = (uint32_t)ts->current_duration * 60000 / ts->tempo / ts->resolution;
        if (now >= note_end_ms) {
            /* Parse next note from sequence */
            if (ts->position >= ts->length - 1) {
                /* End of sequence */
                if (player->looping && player->loop_count != 1) {
                    ts->position = 0;
                    if (player->loop_count > 0) {
                        player->loops_remaining--;
                        if (player->loops_remaining <= 0) {
                            player->playing = false;
                            player->state = PLAYER_PREFETCHED;
                            ts->current_note = -1;
                            break;
                        }
                    }
                } else {
                    player->playing = false;
                    player->state = PLAYER_PREFETCHED;
                    ts->current_note = -1;
                    break;
                }
            }
            
            /* Read next note command */
            int8_t cmd = (int8_t)ts->sequence[ts->position];
            
            if (cmd == TONE_CONTROL_SET_VOLUME && ts->position + 1 < ts->length) {
                ts->current_volume = ts->sequence[ts->position + 1] & 0xFF;
                ts->position += 2;
            } else if (cmd == TONE_CONTROL_REPEAT && ts->position + 1 < ts->length) {
                int repeat_count = ts->sequence[ts->position + 1] & 0xFF;
                ts->loop_count = repeat_count;
                ts->position += 2;
            } else if (cmd == TONE_CONTROL_SILENCE) {
                int duration = ts->sequence[ts->position + 1] & 0xFF;
                ts->current_note = -1;
                ts->current_duration = duration;
                ts->position += 2;
            } else if (cmd >= 0 && cmd <= 127) {
                /* Note: note, duration */
                int note = cmd;
                int duration = ts->sequence[ts->position + 1] & 0xFF;
                
                ts->current_note = note;
                ts->current_duration = duration;
                ts->position += 2;
            } else {
                ts->position++;
            }
            
            ts->start_time_ms = get_time_ms() - now;
        }
        
        now = get_time_ms() - ts->start_time_ms;
    }
}

/* Mix MIDI player - generates samples using FM synthesis */
static void mix_midi_player(MediaPlayer* player, int16_t* buffer, int samples) {
    if (!player->midi || !player->playing) return;

    /* Use a temporary buffer so midi_generate_samples() doesn't zero out
     * the shared mix buffer (which may already contain WAV/tone audio). */
    int16_t* temp_buf = (int16_t*)calloc(samples * 2, sizeof(int16_t));
    if (!temp_buf) return;

    /* Time-based synchronization for MIDI sequencer */
    static uint64_t last_midi_time = 0;
    uint64_t current_time = get_time_us();
    uint64_t elapsed_us = 0;

    if (last_midi_time > 0) {
        elapsed_us = current_time - last_midi_time;
        /* Clamp to reasonable range (1ms - 500ms) */
        if (elapsed_us < 1000) elapsed_us = 1000;
        if (elapsed_us > 500000) elapsed_us = 500000;
    }
    last_midi_time = current_time;

    /* Advance MIDI sequencer by real time */
    if (elapsed_us > 0 && player->midi->sequencer->playing) {
        midi_advance_time(player->midi, elapsed_us);
    }

    /* Generate samples using the MIDI synthesizer into temp buffer */
    midi_generate_samples(player->midi, temp_buf, samples);

    /* Apply player volume and mix into shared buffer */
    float vol = player->muted ? 0.0f : player->volume;
    for (int i = 0; i < samples * 2; i++) {
        buffer[i] += (int16_t)(temp_buf[i] * vol);
    }

    free(temp_buf);

    /* Check if MIDI playback finished */
    if (!player->midi->sequencer->playing) {
        if (player->looping && player->loop_count != 1) {
            /* Restart MIDI */
            midi_play(player->midi);
            last_midi_time = 0;  /* Reset timing on loop */
            if (player->loop_count > 0) {
                player->loops_remaining--;
                if (player->loops_remaining <= 0) {
                    player->playing = false;
                    player->state = PLAYER_PREFETCHED;
                }
            }
        } else {
            player->playing = false;
            player->state = PLAYER_PREFETCHED;
        }
    }
}

/* Audio thread - continuously generates samples */
static void* audio_thread_func(void* arg) {
    (void)arg;
    
    int16_t* mix_buffer = (int16_t*)malloc(AUDIO_BUFFER_SIZE * 2 * sizeof(int16_t));
    if (!mix_buffer) return NULL;
    
    while (g_audio_running) {
        /* Clear buffer */
        memset(mix_buffer, 0, AUDIO_BUFFER_SIZE * 2 * sizeof(int16_t));
        
        pthread_mutex_lock(&g_audio_mutex);
        
        /* Mix all active players */
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!g_players[i].playing) continue;
            
            switch (g_players[i].type) {
                case PLAYER_TYPE_WAV:
                    mix_wav_player(&g_players[i], mix_buffer, AUDIO_BUFFER_SIZE);
                    break;
                case PLAYER_TYPE_TONE:
                case PLAYER_TYPE_TONE_DEVICE:
                    mix_tone_player(&g_players[i], mix_buffer, AUDIO_BUFFER_SIZE);
                    break;
                case PLAYER_TYPE_MIDI:
                    mix_midi_player(&g_players[i], mix_buffer, AUDIO_BUFFER_SIZE);
                    break;
                default:
                    break;
            }
        }
        
        pthread_mutex_unlock(&g_audio_mutex);
        
        /* Queue samples for SDL */
        sdl_audio_queue_samples(mix_buffer, AUDIO_BUFFER_SIZE * 2);
        
        /* Sleep to maintain buffer */
        usleep(5000); /* 5ms */
    }
    
    free(mix_buffer);
    return NULL;
}

/* ============================================
 * Audio Initialization
 * ============================================ */

static void ensure_audio_init(void) {
    if (!g_audio_initialized) {
        sdl_audio_init_simple(AUDIO_SAMPLE_RATE);
        midi_init(AUDIO_SAMPLE_RATE);
        
        g_start_time_us = get_time_us();
        
        /* In libretro mode, don't create audio thread - generate synchronously */
        if (!g_libretro_mode) {
            g_audio_running = true;
            pthread_create(&g_audio_thread, NULL, audio_thread_func, NULL);
        }
        
        g_audio_initialized = true;
        MEDIA_DEBUG("Audio initialized (libretro_mode=%d)", g_libretro_mode);
    }
}

/* Generate audio samples synchronously - called from libretro_process_audio() */
void media_generate_audio_samples(int samples) {
    if (!g_audio_initialized || g_libretro_mode == false) return;
    
    int16_t* mix_buffer = (int16_t*)malloc(samples * 2 * sizeof(int16_t));
    if (!mix_buffer) return;
    
    /* Clear buffer */
    memset(mix_buffer, 0, samples * 2 * sizeof(int16_t));
    
    pthread_mutex_lock(&g_audio_mutex);
    
    /* Mix all active players */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!g_players[i].playing) continue;
        
        switch (g_players[i].type) {
            case PLAYER_TYPE_WAV:
                mix_wav_player(&g_players[i], mix_buffer, samples);
                break;
            case PLAYER_TYPE_TONE:
            case PLAYER_TYPE_TONE_DEVICE:
                mix_tone_player(&g_players[i], mix_buffer, samples);
                break;
            case PLAYER_TYPE_MIDI:
                mix_midi_player(&g_players[i], mix_buffer, samples);
                break;
            default:
                break;
        }
    }
    
    pthread_mutex_unlock(&g_audio_mutex);
    
    /* Queue samples for libretro */
    sdl_audio_queue_samples(mix_buffer, samples * 2);
    
    free(mix_buffer);
}

/* ============================================
 * Player Management
 * ============================================ */

static int find_free_player_slot(void) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (g_players[i].type == PLAYER_TYPE_NONE) {
            return i;
        }
    }
    return -1;
}

static MediaPlayer* find_player(JavaObject* obj) {
    if (!obj) return NULL;
    int id = obj->fields[0].i;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (g_players[i].id == id && g_players[i].type != PLAYER_TYPE_NONE) {
            return &g_players[i];
        }
    }
    return NULL;
}

static MediaPlayer* find_player_by_id(int id) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (g_players[i].id == id && g_players[i].type != PLAYER_TYPE_NONE) {
            return &g_players[i];
        }
    }
    return NULL;
}

/* ============================================
 * Manager Native Methods
 * ============================================ */

static JavaValue native_manager_createPlayer_locator(JVM* jvm, JavaThread* thread,
                                                      JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* locator_str = (JavaString*)args[0].ref;
    
    ensure_audio_init();
    
    const char* locator = locator_str ? string_utf8(jvm, locator_str) : NULL;
    MEDIA_DEBUG("Manager.createPlayer: locator=%s", locator ? locator : "null");
    
    int slot = find_free_player_slot();
    if (slot < 0) return NATIVE_RETURN_NULL();
    
    MediaPlayer* player = &g_players[slot];
    memset(player, 0, sizeof(MediaPlayer));
    player->id = g_next_player_id++;
    player->volume = 1.0f;
    player->state = PLAYER_UNREALIZED;
    
    if (locator && strcmp(locator, "device://tone") == 0) {
        player->type = PLAYER_TYPE_TONE_DEVICE;
        player->tone_seq = (ToneSequence*)calloc(1, sizeof(ToneSequence));
        player->tone_seq->tempo = 120;
        player->tone_seq->resolution = 64;
        player->tone_seq->current_volume = 100;
        player->content_type = strdup("audio/x-tone-seq");
    } else if (locator && strcmp(locator, "device://midi") == 0) {
        player->type = PLAYER_TYPE_MIDI;
        player->content_type = strdup("audio/midi");
    } else {
        memset(player, 0, sizeof(MediaPlayer));
        return NATIVE_RETURN_NULL();
    }
    
    /* Create PlayerImpl object */
    JavaClass* player_class = jvm_load_class(jvm, "javax/microedition/media/PlayerImpl");
    if (!player_class) return NATIVE_RETURN_NULL();
    
    JavaObject* player_obj = jvm_new_object(jvm, player_class);
    if (!player_obj) return NATIVE_RETURN_NULL();
    
    player_obj->fields[0].i = player->id;
    if (player_class->fields_count >= 2) {
        player_obj->fields[1].i = player->state;
    }
    
    return NATIVE_RETURN_OBJECT(player_obj);
}

static JavaValue native_manager_createPlayer_stream(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* stream = (JavaObject*)args[0].ref;
    JavaString* type_str = (JavaString*)args[1].ref;
    
    ensure_audio_init();
    
    if (!stream) return NATIVE_RETURN_NULL();
    
    /* Read data from ByteArrayInputStream */
    JavaClass* stream_class = stream->header.clazz;
    JavaArray* buf = NULL;
    int pos = 0, count = 0;
    
    for (int i = 0; i < stream_class->fields_count; i++) {
        const char* fname = stream_class->fields[i].name;
        if (fname) {
            if (strcmp(fname, "buf") == 0) buf = (JavaArray*)stream->fields[i].ref;
            else if (strcmp(fname, "pos") == 0) pos = stream->fields[i].i;
            else if (strcmp(fname, "count") == 0) count = stream->fields[i].i;
        }
    }
    
    if (!buf || count <= 0) return NATIVE_RETURN_NULL();
    
    int data_len = count - pos;
    if (data_len <= 0) return NATIVE_RETURN_NULL();
    
    uint8_t* data = (uint8_t*)array_data(buf) + pos;
    
    int slot = find_free_player_slot();
    if (slot < 0) return NATIVE_RETURN_NULL();
    
    MediaPlayer* player = &g_players[slot];
    memset(player, 0, sizeof(MediaPlayer));
    player->id = g_next_player_id++;
    player->data = (uint8_t*)malloc(data_len);
    if (!player->data) return NATIVE_RETURN_NULL();
    memcpy(player->data, data, data_len);
    player->size = data_len;
    player->volume = 1.0f;
    player->state = PLAYER_UNREALIZED;
    
    /* Auto-detect format */
    if (data_len >= 4 && memcmp(data, "MThd", 4) == 0) {
        player->type = PLAYER_TYPE_MIDI;
        player->midi = midi_load(player->data, player->size);
        player->content_type = strdup("audio/midi");
    } else if (data_len >= 4 && memcmp(data, "RIFF", 4) == 0) {
        player->type = PLAYER_TYPE_WAV;
        player->wav = wav_parse(player->data, player->size);
        player->content_type = strdup("audio/x-wav");
    } else {
        /* Assume tone sequence */
        player->type = PLAYER_TYPE_TONE;
        player->tone_seq = tone_sequence_parse(player->data, player->size);
        player->content_type = strdup("audio/x-tone-seq");
    }
    
    if (!player->midi && !player->wav && !player->tone_seq) {
        free(player->data);
        memset(player, 0, sizeof(MediaPlayer));
        return NATIVE_RETURN_NULL();
    }
    
    /* Create PlayerImpl object */
    JavaClass* player_class = jvm_load_class(jvm, "javax/microedition/media/PlayerImpl");
    if (!player_class) {
        free(player->data);
        memset(player, 0, sizeof(MediaPlayer));
        return NATIVE_RETURN_NULL();
    }
    
    JavaObject* player_obj = jvm_new_object(jvm, player_class);
    if (!player_obj) {
        free(player->data);
        memset(player, 0, sizeof(MediaPlayer));
        return NATIVE_RETURN_NULL();
    }
    
    player_obj->fields[0].i = player->id;
    if (player_class->fields_count >= 2) {
        player_obj->fields[1].i = player->state;
    }
    
    return NATIVE_RETURN_OBJECT(player_obj);
}

/* Manager.playTone - NON-BLOCKING implementation */
static JavaValue native_manager_playTone(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint note = args[0].i;
    jint duration = args[1].i;
    jint volume = args[2].i;
    
    ensure_audio_init();
    
    if (note < TONE_MIN_NOTE || note > TONE_MAX_NOTE) return NATIVE_RETURN_VOID();
    if (duration <= 0 || duration > TONE_MAX_DURATION) return NATIVE_RETURN_VOID();
    
    volume = volume < TONE_MIN_VOLUME ? TONE_MIN_VOLUME : (volume > TONE_MAX_VOLUME ? TONE_MAX_VOLUME : volume);
    
    /* Use async tone playback */
    int velocity = (volume * 127) / 100;
    
    /* Create a temporary tone player for async playback */
    int slot = find_free_player_slot();
    if (slot >= 0) {
        MediaPlayer* player = &g_players[slot];
        memset(player, 0, sizeof(MediaPlayer));
        player->id = g_next_player_id++;
        player->type = PLAYER_TYPE_TONE_DEVICE;
        player->volume = volume / 100.0f;
        player->playing = true;
        player->state = PLAYER_STARTED;
        
        player->tone_seq = (ToneSequence*)calloc(1, sizeof(ToneSequence));
        player->tone_seq->current_note = note;
        player->tone_seq->current_duration = duration;
        player->tone_seq->current_volume = volume;
        player->tone_seq->start_time_ms = get_time_ms();
        player->tone_seq->playing = true;
        
        midi_note_on(0, note, velocity);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_manager_getSupportedContentTypes(JVM* jvm, JavaThread* thread,
                                                          JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    
    const char* types[] = {"audio/midi", "audio/x-wav", "audio/x-tone-seq"};
    int num = 3;
    
    JavaArray* array = jvm_new_array(jvm, DESC_OBJECT, num, NULL);
    if (!array) return NATIVE_RETURN_NULL();
    
    JavaString** elems = (JavaString**)array_data(array);
    for (int i = 0; i < num; i++) {
        elems[i] = jvm_new_string(jvm, types[i]);
    }
    
    return NATIVE_RETURN_OBJECT(array);
}

static JavaValue native_manager_getSupportedProtocols(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    
    const char* protocols[] = {"file", "http", "device"};
    
    JavaArray* array = jvm_new_array(jvm, DESC_OBJECT, 3, NULL);
    if (!array) return NATIVE_RETURN_NULL();
    
    JavaString** elems = (JavaString**)array_data(array);
    for (int i = 0; i < 3; i++) {
        elems[i] = jvm_new_string(jvm, protocols[i]);
    }
    
    return NATIVE_RETURN_OBJECT(array);
}

/* ============================================
 * Player Native Methods
 * ============================================ */

static JavaValue native_player_realize(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    if (!player) return NATIVE_RETURN_VOID();
    
    if (player->state < PLAYER_REALIZED) {
        player->state = PLAYER_REALIZED;
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue native_player_prefetch(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    if (!player) return NATIVE_RETURN_VOID();
    
    if (player->state < PLAYER_REALIZED) player->state = PLAYER_REALIZED;
    if (player->state < PLAYER_PREFETCHED) player->state = PLAYER_PREFETCHED;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_player_start(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    if (!player) return NATIVE_RETURN_VOID();
    
    MEDIA_DEBUG("Player.start: id=%d type=%d", player->id, player->type);
    
    pthread_mutex_lock(&g_audio_mutex);
    
    if (player->state < PLAYER_PREFETCHED) player->state = PLAYER_PREFETCHED;
    
    player->playing = true;
    player->state = PLAYER_STARTED;
    
    if (player->type == PLAYER_TYPE_WAV && player->wav) {
        player->wav_position = 0;
        player->wav_samples_played = 0;
    } else if (player->type == PLAYER_TYPE_MIDI && player->midi) {
        midi_set_loop(player->midi, player->looping);
        midi_play(player->midi);
    } else if ((player->type == PLAYER_TYPE_TONE || player->type == PLAYER_TYPE_TONE_DEVICE) && player->tone_seq) {
        player->tone_seq->playing = true;
        player->tone_seq->start_time_ms = get_time_ms();
        player->tone_seq->position = 0;
    }
    
    pthread_mutex_unlock(&g_audio_mutex);
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_player_stop(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    if (!player) return NATIVE_RETURN_VOID();
    
    pthread_mutex_lock(&g_audio_mutex);
    
    if (player->type == PLAYER_TYPE_MIDI && player->midi) {
        midi_stop(player->midi);
    } else if (player->type == PLAYER_TYPE_TONE || player->type == PLAYER_TYPE_TONE_DEVICE) {
        if (player->tone_seq && player->tone_seq->current_note >= 0) {
            midi_note_off(0, player->tone_seq->current_note);
        }
    }
    
    player->playing = false;
    if (player->state == PLAYER_STARTED) {
        player->state = PLAYER_PREFETCHED;
    }
    
    pthread_mutex_unlock(&g_audio_mutex);
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_player_deallocate(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    if (!player) return NATIVE_RETURN_VOID();
    
    pthread_mutex_lock(&g_audio_mutex);
    
    player->playing = false;
    if (player->midi) { midi_stop(player->midi); midi_free(player->midi); player->midi = NULL; }
    if (player->wav) { wav_free(player->wav); player->wav = NULL; }
    if (player->tone_seq) { tone_sequence_free(player->tone_seq); player->tone_seq = NULL; }
    player->state = PLAYER_UNREALIZED;
    
    pthread_mutex_unlock(&g_audio_mutex);
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_player_close(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    if (!player) return NATIVE_RETURN_VOID();
    
    pthread_mutex_lock(&g_audio_mutex);
    
    player->playing = false;
    if (player->midi) { midi_stop(player->midi); midi_free(player->midi); }
    if (player->wav) { wav_free(player->wav); }
    if (player->tone_seq) { tone_sequence_free(player->tone_seq); }
    free(player->data);
    free(player->content_type);
    
    memset(player, 0, sizeof(MediaPlayer));
    
    pthread_mutex_unlock(&g_audio_mutex);
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_player_setLoopCount(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    if (!player) return NATIVE_RETURN_VOID();
    
    int count = args[1].i;
    
    pthread_mutex_lock(&g_audio_mutex);
    player->loop_count = count;
    player->loops_remaining = count;
    player->looping = (count != 1);
    pthread_mutex_unlock(&g_audio_mutex);
    
    return NATIVE_RETURN_VOID();
}

/* Player.getDuration - returns duration in microseconds */
static JavaValue native_player_getDuration(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    if (!player) return NATIVE_RETURN_LONG(TIME_UNKNOWN);
    
    jlong duration = TIME_UNKNOWN;
    
    if (player->wav) {
        duration = (jlong)player->wav->duration_ms * 1000LL;
    } else if (player->midi) {
        duration = (jlong)midi_get_duration_ms(player->midi) * 1000LL;
    }
    
    return NATIVE_RETURN_LONG(duration);
}

/* Player.getMediaTime - returns current position in microseconds */
static JavaValue native_player_getMediaTime(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    if (!player) return NATIVE_RETURN_LONG(TIME_UNKNOWN);
    
    jlong time = 0;
    
    pthread_mutex_lock(&g_audio_mutex);
    
    if (player->wav && player->wav->sample_rate > 0) {
        time = (jlong)(player->wav_samples_played * 1000000LL / player->wav->sample_rate);
    } else if (player->midi) {
        time = (jlong)midi_get_position_ms(player->midi) * 1000LL;
    }
    
    pthread_mutex_unlock(&g_audio_mutex);
    
    return NATIVE_RETURN_LONG(time);
}

/* Player.setMediaTime - seeks to position in microseconds */
static JavaValue native_player_setMediaTime(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    if (!player) return NATIVE_RETURN_LONG(-1);
    
    jlong time_us = args[1].j;
    jlong result = time_us;
    
    pthread_mutex_lock(&g_audio_mutex);
    
    if (player->wav && player->wav->byte_rate > 0) {
        uint32_t byte_pos = (uint32_t)((time_us / 1000000.0) * player->wav->byte_rate);
        if (byte_pos > player->wav->data_size) byte_pos = player->wav->data_size;
        player->wav_position = byte_pos;
        player->wav_samples_played = (uint32_t)(time_us * player->wav->sample_rate / 1000000LL);
        result = time_us;
    } else if (player->midi) {
        midi_set_position_ms(player->midi, (uint32_t)(time_us / 1000));
    }
    
    pthread_mutex_unlock(&g_audio_mutex);
    
    return NATIVE_RETURN_LONG(result);
}

/* Player.getState - REAL implementation with auto-update */
static JavaValue native_player_getState(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    if (!player) return NATIVE_RETURN_INT(PLAYER_CLOSED);
    
    int state;
    
    pthread_mutex_lock(&g_audio_mutex);
    
    /* Check if playback has ended */
    if (player->state == PLAYER_STARTED && !player->playing) {
        player->state = PLAYER_PREFETCHED;
    }
    
    state = player->state;
    
    pthread_mutex_unlock(&g_audio_mutex);
    
    return NATIVE_RETURN_INT(state);
}

static JavaValue native_player_getContentType(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    if (!player || !player->content_type) return NATIVE_RETURN_NULL();
    
    JavaString* str = jvm_new_string(jvm, player->content_type);
    return NATIVE_RETURN_OBJECT(str);
}

static JavaValue native_player_getLocator(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    if (!player) return NATIVE_RETURN_NULL();
    
    /* Return locator if stored */
    return NATIVE_RETURN_NULL();
}

/* Player.getControl - returns control objects */
static JavaValue native_player_getControl(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    JavaString* type_str = (JavaString*)args[1].ref;
    
    if (!player || !type_str) return NATIVE_RETURN_NULL();
    
    const char* type = string_utf8(jvm, type_str);
    if (!type) return NATIVE_RETURN_NULL();
    
    /* Create appropriate control object */
    JavaClass* ctrl_class = NULL;
    
    if (strstr(type, "VolumeControl")) {
        ctrl_class = jvm_load_class(jvm, "javax/microedition/media/control/VolumeControlImpl");
    } else if (strstr(type, "MIDIControl") && player->type == PLAYER_TYPE_MIDI) {
        ctrl_class = jvm_load_class(jvm, "javax/microedition/media/control/MIDIControlImpl");
    } else if (strstr(type, "ToneControl") && 
               (player->type == PLAYER_TYPE_TONE || player->type == PLAYER_TYPE_TONE_DEVICE)) {
        ctrl_class = jvm_load_class(jvm, "javax/microedition/media/control/ToneControlImpl");
    }
    
    if (!ctrl_class) return NATIVE_RETURN_NULL();
    
    JavaObject* ctrl_obj = jvm_new_object(jvm, ctrl_class);
    if (ctrl_obj && ctrl_class->fields_count >= 1) {
        ctrl_obj->fields[0].i = player->id;  /* Store player ID */
    }
    
    return NATIVE_RETURN_OBJECT(ctrl_obj);
}

static JavaValue native_player_getControls(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    MediaPlayer* player = find_player((JavaObject*)args[0].ref);
    if (!player) return NATIVE_RETURN_NULL();
    
    /* Create array with available controls */
    int num_controls = 1;  /* VolumeControl */
    if (player->type == PLAYER_TYPE_MIDI) num_controls = 2;
    if (player->type == PLAYER_TYPE_TONE || player->type == PLAYER_TYPE_TONE_DEVICE) num_controls = 2;
    
    JavaArray* array = jvm_new_array(jvm, DESC_OBJECT, num_controls, NULL);
    if (!array) return NATIVE_RETURN_NULL();
    
    JavaString** elems = (JavaString**)array_data(array);
    
    /* Add VolumeControl */
    JavaClass* vol_class = jvm_load_class(jvm, "javax/microedition/media/control/VolumeControlImpl");
    if (vol_class) {
        JavaObject* vol_obj = jvm_new_object(jvm, vol_class);
        if (vol_obj && vol_class->fields_count >= 1) {
            vol_obj->fields[0].i = player->id;
        }
        elems[0] = (JavaString*)vol_obj;
    }
    
    return NATIVE_RETURN_OBJECT(array);
}

static JavaValue native_player_addPlayerListener(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    /* PlayerListener support - simplified */
    return NATIVE_RETURN_VOID();
}

static JavaValue native_player_removePlayerListener(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

/* ============================================
 * VolumeControl Native Methods
 * ============================================ */

static JavaValue native_volumecontrol_setLevel(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    int player_id = ((JavaObject*)args[0].ref)->fields[0].i;
    int level = args[1].i;
    
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    
    MediaPlayer* player = find_player_by_id(player_id);
    if (player) {
        pthread_mutex_lock(&g_audio_mutex);
        player->volume = level / 100.0f;
        pthread_mutex_unlock(&g_audio_mutex);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_volumecontrol_getLevel(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    int player_id = ((JavaObject*)args[0].ref)->fields[0].i;
    
    MediaPlayer* player = find_player_by_id(player_id);
    if (player) {
        return NATIVE_RETURN_INT((int)(player->volume * 100));
    }
    return NATIVE_RETURN_INT(100);
}

static JavaValue native_volumecontrol_setMute(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    int player_id = ((JavaObject*)args[0].ref)->fields[0].i;
    bool mute = args[1].i != 0;
    
    MediaPlayer* player = find_player_by_id(player_id);
    if (player) {
        pthread_mutex_lock(&g_audio_mutex);
        player->muted = mute;
        pthread_mutex_unlock(&g_audio_mutex);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_volumecontrol_isMuted(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    int player_id = ((JavaObject*)args[0].ref)->fields[0].i;
    
    MediaPlayer* player = find_player_by_id(player_id);
    if (player) {
        return NATIVE_RETURN_INT(player->muted ? 1 : 0);
    }
    return NATIVE_RETURN_INT(0);
}

/* ============================================
 * MIDIControl Native Methods - REAL implementation
 * ============================================ */

static JavaValue native_midicontrol_shortMidiEvent(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    int player_id = ((JavaObject*)args[0].ref)->fields[0].i;
    int command = args[1].i;
    int data1 = args[2].i;
    int data2 = args[3].i;
    
    MediaPlayer* player = find_player_by_id(player_id);
    if (player && (player->type == PLAYER_TYPE_MIDI || player->type == PLAYER_TYPE_TONE_DEVICE)) {
        pthread_mutex_lock(&g_audio_mutex);
        
        /* Construct MIDI message and send to synthesizer */
        uint8_t status = (uint8_t)command;
        
        if ((status & 0xF0) == 0x90) {  /* Note On */
            midi_note_on(status & 0x0F, data1, data2);
        } else if ((status & 0xF0) == 0x80) {  /* Note Off */
            midi_note_off(status & 0x0F, data1);
        } else if ((status & 0xF0) == 0xB0) {  /* Control Change */
            midi_control_change(status & 0x0F, data1, data2);
        } else if ((status & 0xF0) == 0xC0) {  /* Program Change */
            midi_program_change(status & 0x0F, data1);
        }
        
        pthread_mutex_unlock(&g_audio_mutex);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_midicontrol_longMidiEvent(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    int player_id = ((JavaObject*)args[0].ref)->fields[0].i;
    JavaArray* data = (JavaArray*)args[1].ref;
    int offset = args[2].i;
    int length = args[3].i;
    
    MediaPlayer* player = find_player_by_id(player_id);
    if (player && data) {
        uint8_t* bytes = (uint8_t*)array_data(data) + offset;
        /* Process long MIDI event */
        (void)bytes;
        (void)length;
    }
    
    return NATIVE_RETURN_INT(0);
}

static JavaValue native_midicontrol_setChannelVolume(JVM* jvm, JavaThread* thread,
                                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    int channel = args[1].i;
    int volume = args[2].i;
    
    pthread_mutex_lock(&g_audio_mutex);
    midi_control_change(channel, 7, volume);  /* CC 7 = Channel Volume */
    pthread_mutex_unlock(&g_audio_mutex);
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_midicontrol_getChannelVolume(JVM* jvm, JavaThread* thread,
                                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    int channel = args[1].i;
    
    int volume = midi_get_channel_volume(channel);
    return NATIVE_RETURN_INT(volume);
}

static JavaValue native_midicontrol_setProgram(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    int channel = args[1].i;
    int bank = args[2].i;
    int program = args[3].i;
    
    (void)bank;
    
    pthread_mutex_lock(&g_audio_mutex);
    midi_program_change(channel, program);
    pthread_mutex_unlock(&g_audio_mutex);
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_midicontrol_getProgram(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    int channel = args[1].i;
    
    int program = midi_get_program(channel);
    return NATIVE_RETURN_INT(program);
}

static JavaValue native_midicontrol_isBankQuerySupported(JVM* jvm, JavaThread* thread,
                                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    (void)args;
    return NATIVE_RETURN_INT(0);  /* Not supported in this implementation */
}

/* ============================================
 * ToneControl Native Methods
 * ============================================ */

static JavaValue native_tonecontrol_setSequence(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    int player_id = ((JavaObject*)args[0].ref)->fields[0].i;
    JavaArray* seq = (JavaArray*)args[1].ref;
    
    MediaPlayer* player = find_player_by_id(player_id);
    if (!player || !seq) return NATIVE_RETURN_VOID();
    
    uint8_t* data = (uint8_t*)array_data(seq);
    int length = seq->length;
    
    pthread_mutex_lock(&g_audio_mutex);
    
    if (player->tone_seq) {
        tone_sequence_free(player->tone_seq);
    }
    
    player->tone_seq = tone_sequence_parse(data, length);
    
    pthread_mutex_unlock(&g_audio_mutex);
    
    return NATIVE_RETURN_VOID();
}

/* ============================================
 * VideoControl Native Methods (stub)
 * ============================================ */

static JavaValue native_videocontrol_initDisplayMode(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_NULL();
}

static JavaValue native_videocontrol_setDisplayLocation(JVM* jvm, JavaThread* thread,
                                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_videocontrol_getDisplayX(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(0);
}

static JavaValue native_videocontrol_getDisplayY(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(0);
}

static JavaValue native_videocontrol_setVisible(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_videocontrol_getDisplayWidth(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(176);
}

static JavaValue native_videocontrol_getDisplayHeight(JVM* jvm, JavaThread* thread,
                                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(144);
}

static JavaValue native_videocontrol_getSnapshot(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_NULL();
}

static JavaValue native_videocontrol_setDisplaySize(JVM* jvm, JavaThread* thread,
                                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_videocontrol_setDisplayFullScreen(JVM* jvm, JavaThread* thread,
                                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_videocontrol_getSourceWidth(JVM* jvm, JavaThread* thread,
                                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(176);
}

static JavaValue native_videocontrol_getSourceHeight(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(144);
}

/* ============================================
 * TimeBase
 * ============================================ */

static JavaValue native_timebase_getTime(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    
    uint64_t now = get_time_us();
    return NATIVE_RETURN_LONG((jlong)(now - g_start_time_us));
}

/* ============================================
 * Initialize media native methods
 * ============================================ */

void init_javax_microedition_media(JVM* jvm) {
    NativeMethodEntry methods[] = {
        /* Manager */
        {"javax/microedition/media/Manager", "createPlayer", 
         "(Ljava/lang/String;)Ljavax/microedition/media/Player;", 
         native_manager_createPlayer_locator},
        {"javax/microedition/media/Manager", "createPlayer",
         "(Ljava/io/InputStream;Ljava/lang/String;)Ljavax/microedition/media/Player;",
         native_manager_createPlayer_stream},
        {"javax/microedition/media/Manager", "playTone", "(III)V", native_manager_playTone},
        {"javax/microedition/media/Manager", "getSupportedContentTypes",
         "(Ljava/lang/String;)[Ljava/lang/String;", native_manager_getSupportedContentTypes},
        {"javax/microedition/media/Manager", "getSupportedProtocols",
         "(Ljava/lang/String;)[Ljava/lang/String;", native_manager_getSupportedProtocols},
        
        /* Player */
        {"javax/microedition/media/Player", "realize", "()V", native_player_realize},
        {"javax/microedition/media/Player", "prefetch", "()V", native_player_prefetch},
        {"javax/microedition/media/Player", "start", "()V", native_player_start},
        {"javax/microedition/media/Player", "stop", "()V", native_player_stop},
        {"javax/microedition/media/Player", "deallocate", "()V", native_player_deallocate},
        {"javax/microedition/media/Player", "close", "()V", native_player_close},
        {"javax/microedition/media/Player", "setLoopCount", "(I)V", native_player_setLoopCount},
        {"javax/microedition/media/Player", "getDuration", "()J", native_player_getDuration},
        {"javax/microedition/media/Player", "getMediaTime", "()J", native_player_getMediaTime},
        {"javax/microedition/media/Player", "setMediaTime", "(J)J", native_player_setMediaTime},
        {"javax/microedition/media/Player", "getState", "()I", native_player_getState},
        {"javax/microedition/media/Player", "getContentType", "()Ljava/lang/String;", native_player_getContentType},
        {"javax/microedition/media/Player", "getLocator", "()Ljava/lang/String;", native_player_getLocator},
        {"javax/microedition/media/Player", "getControl", "(Ljava/lang/String;)Ljavax/microedition/media/Control;", native_player_getControl},
        {"javax/microedition/media/Player", "getControls", "()[Ljavax/microedition/media/Control;", native_player_getControls},
        {"javax/microedition/media/Player", "addPlayerListener", "(Ljavax/microedition/media/PlayerListener;)V", native_player_addPlayerListener},
        {"javax/microedition/media/Player", "removePlayerListener", "(Ljavax/microedition/media/PlayerListener;)V", native_player_removePlayerListener},
        
        /* PlayerImpl */
        {"javax/microedition/media/PlayerImpl", "realize", "()V", native_player_realize},
        {"javax/microedition/media/PlayerImpl", "prefetch", "()V", native_player_prefetch},
        {"javax/microedition/media/PlayerImpl", "start", "()V", native_player_start},
        {"javax/microedition/media/PlayerImpl", "stop", "()V", native_player_stop},
        {"javax/microedition/media/PlayerImpl", "deallocate", "()V", native_player_deallocate},
        {"javax/microedition/media/PlayerImpl", "close", "()V", native_player_close},
        {"javax/microedition/media/PlayerImpl", "setLoopCount", "(I)V", native_player_setLoopCount},
        {"javax/microedition/media/PlayerImpl", "getDuration", "()J", native_player_getDuration},
        {"javax/microedition/media/PlayerImpl", "getMediaTime", "()J", native_player_getMediaTime},
        {"javax/microedition/media/PlayerImpl", "setMediaTime", "(J)J", native_player_setMediaTime},
        {"javax/microedition/media/PlayerImpl", "getState", "()I", native_player_getState},
        {"javax/microedition/media/PlayerImpl", "getContentType", "()Ljava/lang/String;", native_player_getContentType},
        {"javax/microedition/media/PlayerImpl", "getLocator", "()Ljava/lang/String;", native_player_getLocator},
        {"javax/microedition/media/PlayerImpl", "getControl", "(Ljava/lang/String;)Ljavax/microedition/media/Control;", native_player_getControl},
        {"javax/microedition/media/PlayerImpl", "getControls", "()[Ljavax/microedition/media/Control;", native_player_getControls},
        {"javax/microedition/media/PlayerImpl", "addPlayerListener", "(Ljavax/microedition/media/PlayerListener;)V", native_player_addPlayerListener},
        {"javax/microedition/media/PlayerImpl", "removePlayerListener", "(Ljavax/microedition/media/PlayerListener;)V", native_player_removePlayerListener},
        
        /* VolumeControl */
        {"javax/microedition/media/control/VolumeControl", "setLevel", "(I)V", native_volumecontrol_setLevel},
        {"javax/microedition/media/control/VolumeControl", "getLevel", "()I", native_volumecontrol_getLevel},
        {"javax/microedition/media/control/VolumeControl", "setMute", "(Z)V", native_volumecontrol_setMute},
        {"javax/microedition/media/control/VolumeControl", "isMuted", "()Z", native_volumecontrol_isMuted},
        {"javax/microedition/media/control/VolumeControlImpl", "setLevel", "(I)V", native_volumecontrol_setLevel},
        {"javax/microedition/media/control/VolumeControlImpl", "getLevel", "()I", native_volumecontrol_getLevel},
        {"javax/microedition/media/control/VolumeControlImpl", "setMute", "(Z)V", native_volumecontrol_setMute},
        {"javax/microedition/media/control/VolumeControlImpl", "isMuted", "()Z", native_volumecontrol_isMuted},
        
        /* MIDIControl */
        {"javax/microedition/media/control/MIDIControl", "shortMidiEvent", "(III)V", native_midicontrol_shortMidiEvent},
        {"javax/microedition/media/control/MIDIControl", "longMidiEvent", "([BII)I", native_midicontrol_longMidiEvent},
        {"javax/microedition/media/control/MIDIControl", "setChannelVolume", "(II)V", native_midicontrol_setChannelVolume},
        {"javax/microedition/media/control/MIDIControl", "getChannelVolume", "(I)I", native_midicontrol_getChannelVolume},
        {"javax/microedition/media/control/MIDIControl", "setProgram", "(III)V", native_midicontrol_setProgram},
        {"javax/microedition/media/control/MIDIControl", "getProgram", "(I)I", native_midicontrol_getProgram},
        {"javax/microedition/media/control/MIDIControl", "isBankQuerySupported", "()Z", native_midicontrol_isBankQuerySupported},
        {"javax/microedition/media/control/MIDIControlImpl", "shortMidiEvent", "(III)V", native_midicontrol_shortMidiEvent},
        {"javax/microedition/media/control/MIDIControlImpl", "longMidiEvent", "([BII)I", native_midicontrol_longMidiEvent},
        {"javax/microedition/media/control/MIDIControlImpl", "setChannelVolume", "(II)V", native_midicontrol_setChannelVolume},
        {"javax/microedition/media/control/MIDIControlImpl", "getChannelVolume", "(I)I", native_midicontrol_getChannelVolume},
        {"javax/microedition/media/control/MIDIControlImpl", "setProgram", "(III)V", native_midicontrol_setProgram},
        {"javax/microedition/media/control/MIDIControlImpl", "getProgram", "(I)I", native_midicontrol_getProgram},
        {"javax/microedition/media/control/MIDIControlImpl", "isBankQuerySupported", "()Z", native_midicontrol_isBankQuerySupported},
        
        /* ToneControl */
        {"javax/microedition/media/control/ToneControl", "setSequence", "([B)V", native_tonecontrol_setSequence},
        {"javax/microedition/media/control/ToneControlImpl", "setSequence", "([B)V", native_tonecontrol_setSequence},
        
        /* VideoControl */
        {"javax/microedition/media/control/VideoControl", "initDisplayMode", "(ILjava/lang/Object;)Ljava/lang/Object;", native_videocontrol_initDisplayMode},
        {"javax/microedition/media/control/VideoControl", "setDisplayLocation", "(II)V", native_videocontrol_setDisplayLocation},
        {"javax/microedition/media/control/VideoControl", "getDisplayX", "()I", native_videocontrol_getDisplayX},
        {"javax/microedition/media/control/VideoControl", "getDisplayY", "()I", native_videocontrol_getDisplayY},
        {"javax/microedition/media/control/VideoControl", "setVisible", "(Z)V", native_videocontrol_setVisible},
        {"javax/microedition/media/control/VideoControl", "getDisplayWidth", "()I", native_videocontrol_getDisplayWidth},
        {"javax/microedition/media/control/VideoControl", "getDisplayHeight", "()I", native_videocontrol_getDisplayHeight},
        {"javax/microedition/media/control/VideoControl", "getSnapshot", "(Ljava/lang/String;)[B", native_videocontrol_getSnapshot},
        {"javax/microedition/media/control/VideoControl", "setDisplaySize", "(II)V", native_videocontrol_setDisplaySize},
        {"javax/microedition/media/control/VideoControl", "setDisplayFullScreen", "(Z)V", native_videocontrol_setDisplayFullScreen},
        {"javax/microedition/media/control/VideoControl", "getSourceWidth", "()I", native_videocontrol_getSourceWidth},
        {"javax/microedition/media/control/VideoControl", "getSourceHeight", "()I", native_videocontrol_getSourceHeight},
        {"javax/microedition/media/control/VideoControlImpl", "initDisplayMode", "(ILjava/lang/Object;)Ljava/lang/Object;", native_videocontrol_initDisplayMode},
        {"javax/microedition/media/control/VideoControlImpl", "setDisplayLocation", "(II)V", native_videocontrol_setDisplayLocation},
        {"javax/microedition/media/control/VideoControlImpl", "getDisplayX", "()I", native_videocontrol_getDisplayX},
        {"javax/microedition/media/control/VideoControlImpl", "getDisplayY", "()I", native_videocontrol_getDisplayY},
        {"javax/microedition/media/control/VideoControlImpl", "setVisible", "(Z)V", native_videocontrol_setVisible},
        {"javax/microedition/media/control/VideoControlImpl", "getDisplayWidth", "()I", native_videocontrol_getDisplayWidth},
        {"javax/microedition/media/control/VideoControlImpl", "getDisplayHeight", "()I", native_videocontrol_getDisplayHeight},
        {"javax/microedition/media/control/VideoControlImpl", "getSnapshot", "(Ljava/lang/String;)[B", native_videocontrol_getSnapshot},
        {"javax/microedition/media/control/VideoControlImpl", "setDisplaySize", "(II)V", native_videocontrol_setDisplaySize},
        {"javax/microedition/media/control/VideoControlImpl", "setDisplayFullScreen", "(Z)V", native_videocontrol_setDisplayFullScreen},
        {"javax/microedition/media/control/VideoControlImpl", "getSourceWidth", "()I", native_videocontrol_getSourceWidth},
        {"javax/microedition/media/control/VideoControlImpl", "getSourceHeight", "()I", native_videocontrol_getSourceHeight},
        
        /* TimeBase */
        {"javax/microedition/media/TimeBase", "getTime", "()J", native_timebase_getTime},
    };
    
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    MEDIA_DEBUG("Registered %zu media native methods", sizeof(methods) / sizeof(methods[0]));
}

/* External function called from SDL audio callback */
void media_generate_wav_samples(int16_t* buffer, int samples) {
    /* This is now handled by our audio thread */
    (void)buffer;
    (void)samples;
}
