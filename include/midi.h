/*
 * J2ME Emulator - MIDI Synthesizer
 * Simple FM synthesis-based MIDI player for J2ME games
 */

#ifndef MIDI_H
#define MIDI_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MIDI note frequencies (A4 = 440Hz) */
#define MIDI_NOTE_COUNT 128

/* MIDI channel count */
#define MIDI_CHANNELS 16

/* MIDI controller numbers */
#define MIDI_CTRL_BANK_SELECT      0
#define MIDI_CTRL_MODULATION       1
#define MIDI_CTRL_VOLUME           7
#define MIDI_CTRL_PAN              10
#define MIDI_CTRL_EXPRESSION       11
#define MIDI_CTRL_SUSTAIN          64
#define MIDI_CTRL_REVERB           91
#define MIDI_CTRL_CHORUS           93

/* MIDI event types */
#define MIDI_EVENT_NOTE_OFF        0x80
#define MIDI_EVENT_NOTE_ON         0x90
#define MIDI_EVENT_KEY_PRESSURE    0xA0
#define MIDI_EVENT_CONTROL_CHANGE  0xB0
#define MIDI_EVENT_PROGRAM_CHANGE  0xC0
#define MIDI_EVENT_CHANNEL_PRESSURE 0xD0
#define MIDI_EVENT_PITCH_BEND      0xE0
#define MIDI_EVENT_SYSTEM          0xF0

/* MIDI file parser state */
typedef enum {
    MIDI_PARSE_HEADER,
    MIDI_PARSE_TRACK,
    MIDI_PARSE_EVENT,
    MIDI_PARSE_SYSEX,
    MIDI_PARSE_META
} MidiParseState;

/* MIDI track information */
typedef struct MidiTrack {
    uint8_t* data;
    uint32_t length;
    uint32_t position;
    uint32_t delta_time;
    uint8_t running_status;
    bool ended;
} MidiTrack;

/* MIDI file header */
typedef struct MidiHeader {
    uint16_t format;
    uint16_t track_count;
    uint16_t division;
    uint8_t division_type;  /* 0 = ticks per beat, 1 = ticks per second */
} MidiHeader;

/* Active note */
typedef struct MidiNote {
    bool active;
    uint8_t channel;
    uint8_t note;
    uint8_t velocity;
    float frequency;
    float phase;
    float envelope;
    float envelope_target;
    /* FM synthesis parameters */
    float mod_phase;
    float mod_index;
} MidiNote;

/* MIDI channel state */
typedef struct MidiChannel {
    uint8_t program;
    uint8_t volume;
    uint8_t pan;
    uint8_t expression;
    uint8_t sustain;
    float pitch_bend;
    uint8_t bank;
    uint8_t modulation;
} MidiChannel;

/* MIDI sequencer state */
typedef struct MidiSequencer {
    MidiHeader header;
    MidiTrack* tracks;
    MidiChannel channels[MIDI_CHANNELS];
    MidiNote* active_notes;
    int max_active_notes;
    int active_note_count;

    uint32_t ticks_per_beat;
    uint32_t microseconds_per_beat;
    float current_tempo;

    uint32_t position;
    bool playing;
    bool loop;
    float volume;

    /* Playback timing */
    uint32_t sample_rate;
    double samples_per_tick;
    double sample_accumulator;

    /* Time-based synchronization */
    uint64_t last_real_time_us;    /* Last real time in microseconds */
    double tick_accumulator;       /* Accumulated ticks from real time */
} MidiSequencer;

/* MIDI file structure */
typedef struct MidiFile {
    uint8_t* data;
    uint32_t size;
    MidiSequencer* sequencer;
} MidiFile;

/* Initialize MIDI subsystem */
int midi_init(uint32_t sample_rate);

/* Shutdown MIDI subsystem */
void midi_shutdown(void);

/* Load MIDI file from memory */
MidiFile* midi_load(const uint8_t* data, uint32_t size);

/* Free MIDI file */
void midi_free(MidiFile* midi);

/* Start playback */
void midi_play(MidiFile* midi);

/* Stop playback */
void midi_stop(MidiFile* midi);

/* Pause playback */
void midi_pause(MidiFile* midi);

/* Resume playback */
void midi_resume(MidiFile* midi);

/* Set loop mode */
void midi_set_loop(MidiFile* midi, bool loop);

/* Set volume (0.0 - 1.0) */
void midi_set_volume(MidiFile* midi, float volume);

/* Get current position in seconds */
float midi_get_position(MidiFile* midi);

/* Get total length in seconds */
float midi_get_length(MidiFile* midi);

/* Generate audio samples */
void midi_generate_samples(MidiFile* midi, int16_t* buffer, int samples);

/* Advance sequencer by real time (microseconds) - for time-based sync */
void midi_advance_time(MidiFile* midi, uint64_t elapsed_us);

/* Direct MIDI commands (for programmatic control) */
void midi_note_on(uint8_t channel, uint8_t note, uint8_t velocity);
void midi_note_off(uint8_t channel, uint8_t note);
void midi_control_change(uint8_t channel, uint8_t controller, uint8_t value);
void midi_program_change(uint8_t channel, uint8_t program);
void midi_pitch_bend(uint8_t channel, int16_t value);

/* Get active sequencer (for audio callback) */
MidiSequencer* midi_get_active_sequencer(void);

/* Process sequencer events */
void midi_process_tick(MidiSequencer* seq);

#ifdef __cplusplus
}
#endif

#endif /* MIDI_H */

/* Additional functions for media.c */
uint32_t midi_get_duration_ms(MidiFile* midi);
uint32_t midi_get_position_ms(MidiFile* midi);
void midi_set_position_ms(MidiFile* midi, uint32_t position_ms);
int midi_get_channel_volume(int channel);
int midi_get_program(int channel);
