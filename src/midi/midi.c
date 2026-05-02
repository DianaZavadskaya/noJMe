/*
 * J2ME Emulator - MIDI Synthesizer Implementation
 * FM synthesis-based MIDI player
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "midi.h"

/* Math constants */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Maximum polyphony */
#define MAX_POLYPHONY 32

/* Default tempo (120 BPM) */
#define DEFAULT_TEMPO 500000  /* microseconds per beat */
#define DEFAULT_TICKS_PER_BEAT 480

/* Global state */
static uint32_t g_sample_rate = 44100;
static MidiSequencer* g_active_sequencer = NULL;

/* Note frequency table (calculated at init) */
static float g_note_frequencies[MIDI_NOTE_COUNT];

/* FM synthesis parameters for different instruments */
typedef struct {
    float attack;
    float decay;
    float sustain;
    float release;
    float mod_index;
    float mod_ratio;
} FMPatch;

/* Simple FM patches for General MIDI instruments */
static const FMPatch g_patches[] = {
    /* 0: Piano */
    {0.01f, 0.1f, 0.7f, 0.3f, 1.0f, 1.0f},
    /* 1: Bright Piano */
    {0.005f, 0.15f, 0.6f, 0.25f, 1.5f, 2.0f},
    /* 2: Electric Grand */
    {0.005f, 0.2f, 0.5f, 0.2f, 2.0f, 1.0f},
    /* 3: Honky Tonk */
    {0.01f, 0.1f, 0.6f, 0.3f, 0.5f, 0.5f},
    /* 4: Electric Piano 1 */
    {0.001f, 0.3f, 0.4f, 0.3f, 3.0f, 0.5f},
    /* 5: Electric Piano 2 */
    {0.001f, 0.25f, 0.5f, 0.25f, 2.5f, 0.25f},
    /* 6: Harpsichord */
    {0.001f, 0.1f, 0.8f, 0.1f, 0.3f, 2.0f},
    /* 7: Clavinet */
    {0.001f, 0.15f, 0.7f, 0.15f, 0.5f, 3.0f},
    /* 8: Celesta */
    {0.001f, 0.3f, 0.3f, 0.5f, 2.0f, 3.0f},
    /* 9: Glockenspiel */
    {0.001f, 0.4f, 0.2f, 0.6f, 1.0f, 4.0f},
    /* 10: Music Box */
    {0.001f, 0.2f, 0.4f, 0.4f, 1.5f, 3.0f},
    /* 11: Vibraphone */
    {0.001f, 0.3f, 0.5f, 0.4f, 2.0f, 2.0f},
    /* 12: Marimba */
    {0.001f, 0.2f, 0.6f, 0.3f, 0.5f, 1.0f},
    /* 13: Xylophone */
    {0.001f, 0.15f, 0.5f, 0.4f, 0.3f, 2.0f},
    /* 14: Tubular Bells */
    {0.001f, 0.5f, 0.3f, 0.7f, 3.0f, 1.5f},
    /* 15: Dulcimer */
    {0.001f, 0.2f, 0.6f, 0.3f, 1.0f, 1.0f},
    /* 16: Drawbar Organ */
    {0.01f, 0.05f, 0.9f, 0.1f, 0.5f, 1.0f},
    /* 17: Percussive Organ */
    {0.01f, 0.1f, 0.8f, 0.1f, 0.7f, 2.0f},
    /* 18: Rock Organ */
    {0.01f, 0.05f, 0.85f, 0.1f, 1.0f, 0.5f},
    /* 19: Church Organ */
    {0.02f, 0.1f, 0.8f, 0.2f, 0.3f, 0.5f},
    /* 20: Reed Organ */
    {0.02f, 0.15f, 0.7f, 0.15f, 0.4f, 1.0f},
    /* 21: Accordion */
    {0.01f, 0.1f, 0.8f, 0.1f, 0.6f, 0.5f},
    /* 22: Harmonica */
    {0.01f, 0.1f, 0.75f, 0.15f, 0.8f, 1.0f},
    /* 23: Tango Accordion */
    {0.01f, 0.1f, 0.8f, 0.1f, 1.0f, 0.5f},
    /* 24: Nylon Guitar */
    {0.002f, 0.15f, 0.5f, 0.3f, 0.8f, 1.0f},
    /* 25: Steel Guitar */
    {0.002f, 0.1f, 0.6f, 0.25f, 1.2f, 0.5f},
    /* 26: Jazz Guitar */
    {0.002f, 0.15f, 0.55f, 0.2f, 1.5f, 1.0f},
    /* 27: Clean Guitar */
    {0.002f, 0.1f, 0.65f, 0.2f, 0.5f, 0.5f},
    /* 28: Muted Guitar */
    {0.001f, 0.1f, 0.3f, 0.2f, 1.0f, 2.0f},
    /* 29: Overdrive Guitar */
    {0.001f, 0.1f, 0.7f, 0.15f, 2.0f, 1.0f},
    /* 30: Distortion Guitar */
    {0.001f, 0.1f, 0.7f, 0.15f, 3.0f, 0.5f},
    /* 31: Guitar Harmonics */
    {0.001f, 0.2f, 0.5f, 0.3f, 2.0f, 2.0f},
    /* 32: Acoustic Bass */
    {0.002f, 0.2f, 0.4f, 0.25f, 0.5f, 0.5f},
    /* 33: Electric Bass (finger) */
    {0.002f, 0.15f, 0.5f, 0.2f, 1.0f, 0.5f},
    /* 34: Electric Bass (pick) */
    {0.001f, 0.1f, 0.5f, 0.15f, 1.5f, 0.5f},
    /* 35: Fretless Bass */
    {0.005f, 0.3f, 0.4f, 0.2f, 0.3f, 1.0f},
    /* 36: Slap Bass 1 */
    {0.001f, 0.05f, 0.6f, 0.1f, 2.0f, 1.0f},
    /* 37: Slap Bass 2 */
    {0.001f, 0.08f, 0.55f, 0.12f, 2.5f, 1.0f},
    /* 38: Synth Bass 1 */
    {0.002f, 0.1f, 0.6f, 0.15f, 3.0f, 0.5f},
    /* 39: Synth Bass 2 */
    {0.002f, 0.15f, 0.5f, 0.2f, 2.5f, 1.0f},
    /* 40-47: Strings */
    {0.05f, 0.2f, 0.7f, 0.3f, 0.3f, 1.0f},
    /* 48-55: Ensemble */
    {0.05f, 0.15f, 0.75f, 0.25f, 0.5f, 0.5f},
    /* 56-63: Brass */
    {0.02f, 0.15f, 0.6f, 0.2f, 1.0f, 1.0f},
    /* 64-71: Reed */
    {0.02f, 0.15f, 0.65f, 0.15f, 0.8f, 1.0f},
    /* 72-79: Pipe */
    {0.02f, 0.2f, 0.6f, 0.2f, 0.5f, 1.5f},
    /* 80-87: Synth Lead */
    {0.005f, 0.1f, 0.7f, 0.15f, 2.0f, 1.0f},
    /* 88-95: Synth Pad */
    {0.1f, 0.3f, 0.6f, 0.4f, 1.5f, 0.5f},
    /* 96-103: Synth Effects */
    {0.05f, 0.3f, 0.5f, 0.3f, 2.5f, 2.0f},
    /* 104-111: Ethnic */
    {0.01f, 0.2f, 0.5f, 0.3f, 1.0f, 1.0f},
    /* 112-119: Percussive */
    {0.001f, 0.3f, 0.3f, 0.4f, 3.0f, 3.0f},
    /* 120-127: Sound Effects */
    {0.001f, 0.2f, 0.4f, 0.4f, 4.0f, 2.0f},
};

/* Default patch for unknown programs */
static const FMPatch g_default_patch = {0.01f, 0.1f, 0.7f, 0.2f, 1.0f, 1.0f};

/* Drum map (MIDI note to frequency ratio) */
static const float g_drum_freq_mult[] = {
    /* 35-81: Standard drum kit */
    0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f,
    0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f,
    0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f,
    0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f,
    0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f,
    0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f
};

/* Calculate note frequency */
static void init_note_frequencies(void) {
    /* A4 = 440 Hz, MIDI note 69 */
    for (int i = 0; i < MIDI_NOTE_COUNT; i++) {
        g_note_frequencies[i] = 440.0f * powf(2.0f, (i - 69) / 12.0f);
    }
}

/* Get patch for program */
static const FMPatch* get_patch(uint8_t program) {
    if (program < sizeof(g_patches) / sizeof(g_patches[0])) {
        return &g_patches[program];
    }
    return &g_default_patch;
}

/* Initialize MIDI subsystem */
int midi_init(uint32_t sample_rate) {
    g_sample_rate = sample_rate;
    init_note_frequencies();
    return 0;
}

/* Shutdown MIDI subsystem */
void midi_shutdown(void) {
    g_active_sequencer = NULL;
}

/* Read variable length value from MIDI data */
static uint32_t read_var_len(uint8_t* data, uint32_t* pos, uint32_t max) {
    uint32_t value = 0;
    uint8_t byte;
    
    do {
        if (*pos >= max) return value;
        byte = data[(*pos)++];
        value = (value << 7) | (byte & 0x7F);
    } while (byte & 0x80);
    
    return value;
}

/* Parse MIDI file header */
static int parse_header(MidiFile* midi, uint32_t* pos) {
    if (midi->size < 14) return -1;
    
    /* Check "MThd" */
    if (memcmp(midi->data, "MThd", 4) != 0) return -1;
    
    uint32_t header_len = (midi->data[4] << 24) | (midi->data[5] << 16) | 
                          (midi->data[6] << 8) | midi->data[7];
    
    MidiHeader* hdr = &midi->sequencer->header;
    hdr->format = (midi->data[8] << 8) | midi->data[9];
    hdr->track_count = (midi->data[10] << 8) | midi->data[11];
    hdr->division = (midi->data[12] << 8) | midi->data[13];
    
    /* Determine division type */
    if (hdr->division & 0x8000) {
        hdr->division_type = 1;  /* SMPTE */
        midi->sequencer->ticks_per_beat = 480;  /* Default */
    } else {
        hdr->division_type = 0;  /* Ticks per beat */
        midi->sequencer->ticks_per_beat = hdr->division & 0x7FFF;
    }
    
    *pos = 8 + header_len;
    return 0;
}

/* Parse track header */
static int parse_track(MidiFile* midi, int track_num, uint32_t* pos) {
    if (*pos + 8 > midi->size) return -1;
    
    uint8_t* data = midi->data + *pos;
    
    /* Check "MTrk" */
    if (memcmp(data, "MTrk", 4) != 0) return -1;
    
    MidiTrack* track = &midi->sequencer->tracks[track_num];
    track->length = (data[4] << 24) | (data[5] << 16) | 
                    (data[6] << 8) | data[7];
    track->data = data + 8;
    track->position = 0;
    track->delta_time = read_var_len(track->data, &track->position, track->length);
    track->running_status = 0;
    track->ended = false;
    
    *pos += 8 + track->length;
    return 0;
}

/* Load MIDI file from memory */
MidiFile* midi_load(const uint8_t* data, uint32_t size) {
    if (!data || size < 14) return NULL;
    
    MidiFile* midi = (MidiFile*)calloc(1, sizeof(MidiFile));
    if (!midi) return NULL;
    
    midi->data = (uint8_t*)malloc(size);
    if (!midi->data) {
        free(midi);
        return NULL;
    }
    memcpy(midi->data, data, size);
    midi->size = size;
    
    midi->sequencer = (MidiSequencer*)calloc(1, sizeof(MidiSequencer));
    if (!midi->sequencer) {
        free(midi->data);
        free(midi);
        return NULL;
    }
    
    /* Initialize channels */
    for (int i = 0; i < MIDI_CHANNELS; i++) {
        midi->sequencer->channels[i].program = 0;
        midi->sequencer->channels[i].volume = 100;
        midi->sequencer->channels[i].pan = 64;
        midi->sequencer->channels[i].expression = 127;
        midi->sequencer->channels[i].pitch_bend = 1.0f;
    }
    
    /* Parse header */
    uint32_t pos = 0;
    if (parse_header(midi, &pos) != 0) {
        free(midi->sequencer);
        free(midi->data);
        free(midi);
        return NULL;
    }
    
    /* Allocate tracks */
    midi->sequencer->tracks = (MidiTrack*)calloc(
        midi->sequencer->header.track_count, sizeof(MidiTrack));
    if (!midi->sequencer->tracks) {
        free(midi->sequencer);
        free(midi->data);
        free(midi);
        return NULL;
    }
    
    /* Parse tracks */
    for (int i = 0; i < midi->sequencer->header.track_count; i++) {
        if (parse_track(midi, i, &pos) != 0) {
            midi_free(midi);
            return NULL;
        }
    }
    
    /* Initialize sequencer */
    midi->sequencer->microseconds_per_beat = DEFAULT_TEMPO;
    midi->sequencer->current_tempo = 120.0f;
    midi->sequencer->sample_rate = g_sample_rate;
    midi->sequencer->volume = 1.0f;
    midi->sequencer->playing = false;
    midi->sequencer->last_real_time_us = 0;
    midi->sequencer->tick_accumulator = 0.0;

    /* Calculate samples per tick */
    float seconds_per_beat = midi->sequencer->microseconds_per_beat / 1000000.0f;
    midi->sequencer->samples_per_tick = (g_sample_rate * seconds_per_beat) /
                                         midi->sequencer->ticks_per_beat;
    
    /* Allocate active notes */
    midi->sequencer->max_active_notes = MAX_POLYPHONY;
    midi->sequencer->active_notes = (MidiNote*)calloc(
        MAX_POLYPHONY, sizeof(MidiNote));
    
    return midi;
}

/* Free MIDI file */
void midi_free(MidiFile* midi) {
    if (!midi) return;
    
    if (midi->sequencer) {
        if (midi->sequencer->tracks) {
            free(midi->sequencer->tracks);
        }
        if (midi->sequencer->active_notes) {
            free(midi->sequencer->active_notes);
        }
        free(midi->sequencer);
    }
    if (midi->data) {
        free(midi->data);
    }
    free(midi);
}

/* Find free note slot */
static int find_free_note(MidiSequencer* seq) {
    for (int i = 0; i < seq->max_active_notes; i++) {
        if (!seq->active_notes[i].active) {
            return i;
        }
    }
    /* All slots used, steal oldest note */
    return 0;
}

/* Start note */
static void start_note(MidiSequencer* seq, uint8_t channel, 
                       uint8_t note, uint8_t velocity) {
    if (note >= MIDI_NOTE_COUNT) return;
    
    int slot = find_free_note(seq);
    MidiNote* n = &seq->active_notes[slot];
    
    n->active = true;
    n->channel = channel;
    n->note = note;
    n->velocity = velocity;
    n->frequency = g_note_frequencies[note];
    n->phase = 0.0f;
    n->mod_phase = 0.0f;
    n->envelope = 0.0f;
    n->envelope_target = velocity / 127.0f;
    
    /* Get FM patch */
    const FMPatch* patch = get_patch(seq->channels[channel].program);
    n->mod_index = patch->mod_index;
    
    seq->active_note_count++;
}

/* Stop note */
static void stop_note(MidiSequencer* seq, uint8_t channel, uint8_t note) {
    for (int i = 0; i < seq->max_active_notes; i++) {
        MidiNote* n = &seq->active_notes[i];
        if (n->active && n->channel == channel && n->note == note) {
            n->envelope_target = 0.0f;  /* Release */
        }
    }
}

/* Process MIDI event */
static void process_event(MidiSequencer* seq, MidiTrack* track) {
    uint8_t status, data1, data2;
    uint8_t* data = track->data;
    uint32_t pos = track->position;
    uint32_t len = track->length;
    
    if (pos >= len) {
        track->ended = true;
        return;
    }
    
    status = data[pos++];
    
    if (status < 0x80) {
        /* Running status */
        status = track->running_status;
        pos--;
    } else {
        track->running_status = status;
    }
    
    uint8_t event_type = status & 0xF0;
    uint8_t channel = status & 0x0F;
    
    switch (event_type) {
        case MIDI_EVENT_NOTE_OFF:
            if (pos + 1 < len) {
                data1 = data[pos++];
                data2 = data[pos++];
                stop_note(seq, channel, data1);
            }
            break;
            
        case MIDI_EVENT_NOTE_ON:
            if (pos + 1 < len) {
                data1 = data[pos++];
                data2 = data[pos++];
                if (data2 == 0) {
                    stop_note(seq, channel, data1);
                } else {
                    start_note(seq, channel, data1, data2);
                }
            }
            break;
            
        case MIDI_EVENT_CONTROL_CHANGE:
            if (pos + 1 < len) {
                data1 = data[pos++];  /* Controller */
                data2 = data[pos++];  /* Value */
                
                switch (data1) {
                    case MIDI_CTRL_VOLUME:
                        seq->channels[channel].volume = data2;
                        break;
                    case MIDI_CTRL_PAN:
                        seq->channels[channel].pan = data2;
                        break;
                    case MIDI_CTRL_EXPRESSION:
                        seq->channels[channel].expression = data2;
                        break;
                    case MIDI_CTRL_SUSTAIN:
                        seq->channels[channel].sustain = data2;
                        break;
                    case MIDI_CTRL_BANK_SELECT:
                        seq->channels[channel].bank = data2;
                        break;
                }
            }
            break;
            
        case MIDI_EVENT_PROGRAM_CHANGE:
            if (pos < len) {
                seq->channels[channel].program = data[pos++];
            }
            break;
            
        case MIDI_EVENT_PITCH_BEND:
            if (pos + 1 < len) {
                data1 = data[pos++];
                data2 = data[pos++];
                int16_t bend = ((data2 << 7) | data1) - 8192;
                seq->channels[channel].pitch_bend = powf(2.0f, bend / 8192.0f / 6.0f);
            }
            break;
            
        case MIDI_EVENT_SYSTEM:
            if (status == 0xFF) {
                /* Meta event */
                if (pos >= len) break;
                uint8_t meta_type = data[pos++];
                uint32_t meta_len = read_var_len(data, &pos, len);
                
                if (meta_type == 0x51 && meta_len == 3) {
                    /* Tempo change */
                    seq->microseconds_per_beat = (data[pos] << 16) | 
                                                  (data[pos+1] << 8) | data[pos+2];
                    float seconds_per_beat = seq->microseconds_per_beat / 1000000.0f;
                    seq->samples_per_tick = (seq->sample_rate * seconds_per_beat) / 
                                            seq->ticks_per_beat;
                }
                pos += meta_len;
            } else if (status == 0xF0 || status == 0xF7) {
                /* SysEx */
                while (pos < len && data[pos] != 0xF7) pos++;
                if (pos < len) pos++;
            }
            break;
            
        default:
            /* Skip unknown events */
            if (pos < len) pos++;
            if (pos < len) pos++;
            break;
    }
    
    track->position = pos;
    
    /* Read next delta time */
    if (track->position < track->length) {
        track->delta_time = read_var_len(track->data, &track->position, track->length);
    } else {
        track->ended = true;
    }
}

/* Process sequencer tick */
void midi_process_tick(MidiSequencer* seq) {
    if (!seq || !seq->playing) return;
    
    /* Process all tracks */
    bool all_ended = true;
    
    for (int i = 0; i < seq->header.track_count; i++) {
        MidiTrack* track = &seq->tracks[i];
        
        if (track->ended) continue;
        all_ended = false;
        
        while (track->delta_time == 0 && !track->ended) {
            process_event(seq, track);
        }
        
        if (!track->ended) {
            track->delta_time--;
        }
    }
    
    /* Check if all tracks ended */
    if (all_ended) {
        if (seq->loop) {
            /* Restart */
            for (int i = 0; i < seq->header.track_count; i++) {
                MidiTrack* track = &seq->tracks[i];
                track->position = 0;
                track->delta_time = read_var_len(track->data, &track->position, track->length);
                track->running_status = 0;
                track->ended = false;
            }
        } else {
            seq->playing = false;
        }
    }
}

/* Generate audio samples using FM synthesis */
void midi_generate_samples(MidiFile* midi, int16_t* buffer, int samples) {
    if (!midi || !midi->sequencer || !buffer) return;

    MidiSequencer* seq = midi->sequencer;

    memset(buffer, 0, samples * 2 * sizeof(int16_t));  /* Stereo */

    if (!seq->playing) return;

    /* NOTE: Tick processing is now done via midi_advance_time() for time-based sync.
     * This function only generates audio samples, does NOT advance the sequencer. */

    for (int s = 0; s < samples; s++) {
        /* Generate audio for all active notes */
        float left = 0.0f, right = 0.0f;
        
        for (int i = 0; i < seq->max_active_notes; i++) {
            MidiNote* note = &seq->active_notes[i];
            if (!note->active) continue;
            
            MidiChannel* ch = &seq->channels[note->channel];
            const FMPatch* patch = get_patch(ch->program);
            
            /* Apply envelope */
            float envelope_diff = note->envelope_target - note->envelope;
            if (note->envelope_target > note->envelope) {
                /* Attack */
                note->envelope += envelope_diff * patch->attack;
            } else {
                /* Release */
                note->envelope += envelope_diff * patch->release;
            }
            
            if (note->envelope < 0.001f && note->envelope_target == 0.0f) {
                note->active = false;
                seq->active_note_count--;
                continue;
            }
            
            /* Calculate frequency with pitch bend */
            float freq = note->frequency * ch->pitch_bend;
            
            /* FM synthesis */
            float mod_freq = freq * patch->mod_ratio;
            float mod_wave = sinf(note->mod_phase);
            
            /* Phase increment */
            float phase_inc = (2.0f * M_PI * freq) / seq->sample_rate;
            float mod_phase_inc = (2.0f * M_PI * mod_freq) / seq->sample_rate;
            
            /* Carrier with FM */
            float carrier = sinf(note->phase + note->mod_index * mod_wave);
            
            /* Apply envelope and velocity */
            float sample = carrier * note->envelope * (note->velocity / 127.0f);
            
            /* Apply channel volume and expression */
            sample *= (ch->volume / 127.0f) * (ch->expression / 127.0f);
            
            /* Pan */
            float pan = ch->pan / 127.0f;
            left += sample * (1.0f - pan);
            right += sample * pan;
            
            /* Update phase */
            note->phase += phase_inc;
            note->mod_phase += mod_phase_inc;
            if (note->phase > 2.0f * M_PI) note->phase -= 2.0f * M_PI;
            if (note->mod_phase > 2.0f * M_PI) note->mod_phase -= 2.0f * M_PI;
        }
        
        /* Apply master volume and clip */
        left *= seq->volume;
        right *= seq->volume;
        
        if (left > 1.0f) left = 1.0f;
        if (left < -1.0f) left = -1.0f;
        if (right > 1.0f) right = 1.0f;
        if (right < -1.0f) right = -1.0f;
        
        /* Output */
        buffer[s * 2] = (int16_t)(left * 32767);
        buffer[s * 2 + 1] = (int16_t)(right * 32767);
    }
}

/* Advance sequencer by real time (microseconds) - for time-based synchronization */
void midi_advance_time(MidiFile* midi, uint64_t elapsed_us) {
    if (!midi || !midi->sequencer || !midi->sequencer->playing) return;

    MidiSequencer* seq = midi->sequencer;

    /* Convert elapsed time to ticks */
    /* ticks = (elapsed_us / microseconds_per_beat) * ticks_per_beat */
    double ticks = ((double)elapsed_us / (double)seq->microseconds_per_beat) * (double)seq->ticks_per_beat;
    seq->tick_accumulator += ticks;

    /* Process ticks */
    while (seq->tick_accumulator >= 1.0) {
        seq->tick_accumulator -= 1.0;
        midi_process_tick(seq);
    }
}

/* Start playback */
void midi_play(MidiFile* midi) {
    if (!midi || !midi->sequencer) return;
    midi->sequencer->playing = true;
    g_active_sequencer = midi->sequencer;
}

/* Stop playback */
void midi_stop(MidiFile* midi) {
    if (!midi || !midi->sequencer) return;
    midi->sequencer->playing = false;
    
    /* Stop all notes */
    for (int i = 0; i < midi->sequencer->max_active_notes; i++) {
        midi->sequencer->active_notes[i].active = false;
    }
    midi->sequencer->active_note_count = 0;
}

/* Pause playback */
void midi_pause(MidiFile* midi) {
    if (!midi || !midi->sequencer) return;
    midi->sequencer->playing = false;
}

/* Resume playback */
void midi_resume(MidiFile* midi) {
    if (!midi || !midi->sequencer) return;
    midi->sequencer->playing = true;
}

/* Set loop mode */
void midi_set_loop(MidiFile* midi, bool loop) {
    if (!midi || !midi->sequencer) return;
    midi->sequencer->loop = loop;
}

/* Set volume */
void midi_set_volume(MidiFile* midi, float volume) {
    if (!midi || !midi->sequencer) return;
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    midi->sequencer->volume = volume;
}

/* Get current position in seconds */
float midi_get_position(MidiFile* midi) {
    if (!midi || !midi->sequencer) return 0.0f;
    
    MidiSequencer* seq = midi->sequencer;
    
    /* Convert position (ticks) to seconds */
    /* Formula: seconds = ticks * (microseconds_per_beat / ticks_per_beat) / 1000000 */
    if (seq->ticks_per_beat > 0 && seq->microseconds_per_beat > 0) {
        float seconds_per_tick = (float)seq->microseconds_per_beat / (float)seq->ticks_per_beat / 1000000.0f;
        return (float)seq->position * seconds_per_tick;
    }
    
    return 0.0f;
}

/* Get total length in seconds */
float midi_get_length(MidiFile* midi) {
    if (!midi || !midi->sequencer) return 0.0f;
    
    MidiSequencer* seq = midi->sequencer;
    uint32_t total_ticks = 0;
    
    /* Find maximum tick position across all tracks */
    for (int i = 0; i < seq->header.track_count; i++) {
        /* Estimate total ticks from track length */
        if (seq->tracks[i].length > total_ticks) {
            total_ticks = seq->tracks[i].length;
        }
    }
    
    /* Convert ticks to seconds */
    if (seq->ticks_per_beat > 0 && seq->microseconds_per_beat > 0) {
        float seconds_per_tick = (float)seq->microseconds_per_beat / (float)seq->ticks_per_beat / 1000000.0f;
        return (float)total_ticks * seconds_per_tick;
    }
    
    return 0.0f;
}

/* Direct note on */
void midi_note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (g_active_sequencer) {
        start_note(g_active_sequencer, channel, note, velocity);
    }
}

/* Direct note off */
void midi_note_off(uint8_t channel, uint8_t note) {
    if (g_active_sequencer) {
        stop_note(g_active_sequencer, channel, note);
    }
}

/* Direct control change */
void midi_control_change(uint8_t channel, uint8_t controller, uint8_t value) {
    if (g_active_sequencer) {
        switch (controller) {
            case MIDI_CTRL_VOLUME:
                g_active_sequencer->channels[channel].volume = value;
                break;
            case MIDI_CTRL_PAN:
                g_active_sequencer->channels[channel].pan = value;
                break;
            case MIDI_CTRL_EXPRESSION:
                g_active_sequencer->channels[channel].expression = value;
                break;
        }
    }
}

/* Direct program change */
void midi_program_change(uint8_t channel, uint8_t program) {
    if (g_active_sequencer) {
        g_active_sequencer->channels[channel].program = program;
    }
}

/* Direct pitch bend */
void midi_pitch_bend(uint8_t channel, int16_t value) {
    if (g_active_sequencer) {
        g_active_sequencer->channels[channel].pitch_bend = 
            powf(2.0f, value / 8192.0f / 6.0f);
    }
}

/* Get active sequencer */
MidiSequencer* midi_get_active_sequencer(void) {
    return g_active_sequencer;
}

/* Get duration in milliseconds */
uint32_t midi_get_duration_ms(MidiFile* midi) {
    if (!midi || !midi->sequencer) return 0;
    
    /* Calculate total duration from ticks and tempo */
    MidiSequencer* seq = midi->sequencer;
    uint32_t total_ticks = 0;
    
    /* Find maximum tick position across all tracks */
    for (int i = 0; i < seq->header.track_count; i++) {
        if (seq->tracks[i].length > total_ticks) {
            total_ticks = seq->tracks[i].length;
        }
    }
    
    /* Convert ticks to milliseconds using default tempo */
    float ticks_per_ms = (float)seq->ticks_per_beat / (seq->microseconds_per_beat / 1000.0f);
    return (uint32_t)(total_ticks / ticks_per_ms);
}

/* Get current position in milliseconds */
uint32_t midi_get_position_ms(MidiFile* midi) {
    if (!midi || !midi->sequencer) return 0;
    
    MidiSequencer* seq = midi->sequencer;
    float ticks_per_ms = (float)seq->ticks_per_beat / (seq->microseconds_per_beat / 1000.0f);
    return (uint32_t)(seq->position / ticks_per_ms);
}

/* Set position in milliseconds */
void midi_set_position_ms(MidiFile* midi, uint32_t position_ms) {
    if (!midi || !midi->sequencer) return;
    
    MidiSequencer* seq = midi->sequencer;
    float ticks_per_ms = (float)seq->ticks_per_beat / (seq->microseconds_per_beat / 1000.0f);
    seq->position = (uint32_t)(position_ms * ticks_per_ms);
    
    /* Update track positions */
    for (int i = 0; i < seq->header.track_count; i++) {
        seq->tracks[i].position = 0;
        seq->tracks[i].ended = false;
    }
}

/* Get channel volume */
int midi_get_channel_volume(int channel) {
    if (!g_active_sequencer || channel < 0 || channel >= MIDI_CHANNELS) return 100;
    return g_active_sequencer->channels[channel].volume;
}

/* Get channel program */
int midi_get_program(int channel) {
    if (!g_active_sequencer || channel < 0 || channel >= MIDI_CHANNELS) return 0;
    return g_active_sequencer->channels[channel].program;
}
