/*
 * J2ME Emulator - MIDI Synthesizer Implementation
 * FM synthesis-based MIDI player
 * Migrated from src/midi/midi.c
 */

// ---------------------------------------------------------------------------
// Constants (from midi.h #define / enum)
// ---------------------------------------------------------------------------

export const MIDI_NOTE_COUNT = 128;
export const MIDI_CHANNELS   = 16;

// MIDI controller numbers
export const MIDI_CTRL_BANK_SELECT  =  0;
export const MIDI_CTRL_MODULATION   =  1;
export const MIDI_CTRL_VOLUME       =  7;
export const MIDI_CTRL_PAN          = 10;
export const MIDI_CTRL_EXPRESSION   = 11;
export const MIDI_CTRL_SUSTAIN      = 64;
export const MIDI_CTRL_REVERB       = 91;
export const MIDI_CTRL_CHORUS       = 93;

// MIDI event types
export const MIDI_EVENT_NOTE_OFF         = 0x80;
export const MIDI_EVENT_NOTE_ON          = 0x90;
export const MIDI_EVENT_KEY_PRESSURE     = 0xA0;
export const MIDI_EVENT_CONTROL_CHANGE   = 0xB0;
export const MIDI_EVENT_PROGRAM_CHANGE   = 0xC0;
export const MIDI_EVENT_CHANNEL_PRESSURE = 0xD0;
export const MIDI_EVENT_PITCH_BEND       = 0xE0;
export const MIDI_EVENT_SYSTEM           = 0xF0;

// MidiParseState enum
export const MidiParseState = Object.freeze({
    MIDI_PARSE_HEADER: 0,
    MIDI_PARSE_TRACK:  1,
    MIDI_PARSE_EVENT:  2,
    MIDI_PARSE_SYSEX:  3,
    MIDI_PARSE_META:   4,
});

// ---------------------------------------------------------------------------
// Internal constants
// ---------------------------------------------------------------------------

const M_PI              = Math.PI;
const MAX_POLYPHONY     = 32;
const DEFAULT_TEMPO     = 500000; // microseconds per beat (120 BPM)
const DEFAULT_TICKS_PER_BEAT = 480;

// ---------------------------------------------------------------------------
// Module-level global state
// ---------------------------------------------------------------------------

let g_sample_rate      = 44100;
let g_active_sequencer = null;

// Note frequency table — Float32Array, initialised by midi_init()
const g_note_frequencies = new Float32Array(MIDI_NOTE_COUNT);

// ---------------------------------------------------------------------------
// FM patch table (General MIDI program map)
// Each entry: { attack, decay, sustain, release, mod_index, mod_ratio }
// ---------------------------------------------------------------------------

const g_patches = [
    /* 0:  Piano              */ { attack:0.01,   decay:0.1,   sustain:0.7,  release:0.3,  mod_index:1.0,  mod_ratio:1.0  },
    /* 1:  Bright Piano       */ { attack:0.005,  decay:0.15,  sustain:0.6,  release:0.25, mod_index:1.5,  mod_ratio:2.0  },
    /* 2:  Electric Grand     */ { attack:0.005,  decay:0.2,   sustain:0.5,  release:0.2,  mod_index:2.0,  mod_ratio:1.0  },
    /* 3:  Honky Tonk         */ { attack:0.01,   decay:0.1,   sustain:0.6,  release:0.3,  mod_index:0.5,  mod_ratio:0.5  },
    /* 4:  Electric Piano 1   */ { attack:0.001,  decay:0.3,   sustain:0.4,  release:0.3,  mod_index:3.0,  mod_ratio:0.5  },
    /* 5:  Electric Piano 2   */ { attack:0.001,  decay:0.25,  sustain:0.5,  release:0.25, mod_index:2.5,  mod_ratio:0.25 },
    /* 6:  Harpsichord        */ { attack:0.001,  decay:0.1,   sustain:0.8,  release:0.1,  mod_index:0.3,  mod_ratio:2.0  },
    /* 7:  Clavinet           */ { attack:0.001,  decay:0.15,  sustain:0.7,  release:0.15, mod_index:0.5,  mod_ratio:3.0  },
    /* 8:  Celesta            */ { attack:0.001,  decay:0.3,   sustain:0.3,  release:0.5,  mod_index:2.0,  mod_ratio:3.0  },
    /* 9:  Glockenspiel       */ { attack:0.001,  decay:0.4,   sustain:0.2,  release:0.6,  mod_index:1.0,  mod_ratio:4.0  },
    /* 10: Music Box          */ { attack:0.001,  decay:0.2,   sustain:0.4,  release:0.4,  mod_index:1.5,  mod_ratio:3.0  },
    /* 11: Vibraphone         */ { attack:0.001,  decay:0.3,   sustain:0.5,  release:0.4,  mod_index:2.0,  mod_ratio:2.0  },
    /* 12: Marimba            */ { attack:0.001,  decay:0.2,   sustain:0.6,  release:0.3,  mod_index:0.5,  mod_ratio:1.0  },
    /* 13: Xylophone          */ { attack:0.001,  decay:0.15,  sustain:0.5,  release:0.4,  mod_index:0.3,  mod_ratio:2.0  },
    /* 14: Tubular Bells      */ { attack:0.001,  decay:0.5,   sustain:0.3,  release:0.7,  mod_index:3.0,  mod_ratio:1.5  },
    /* 15: Dulcimer           */ { attack:0.001,  decay:0.2,   sustain:0.6,  release:0.3,  mod_index:1.0,  mod_ratio:1.0  },
    /* 16: Drawbar Organ      */ { attack:0.01,   decay:0.05,  sustain:0.9,  release:0.1,  mod_index:0.5,  mod_ratio:1.0  },
    /* 17: Percussive Organ   */ { attack:0.01,   decay:0.1,   sustain:0.8,  release:0.1,  mod_index:0.7,  mod_ratio:2.0  },
    /* 18: Rock Organ         */ { attack:0.01,   decay:0.05,  sustain:0.85, release:0.1,  mod_index:1.0,  mod_ratio:0.5  },
    /* 19: Church Organ       */ { attack:0.02,   decay:0.1,   sustain:0.8,  release:0.2,  mod_index:0.3,  mod_ratio:0.5  },
    /* 20: Reed Organ         */ { attack:0.02,   decay:0.15,  sustain:0.7,  release:0.15, mod_index:0.4,  mod_ratio:1.0  },
    /* 21: Accordion          */ { attack:0.01,   decay:0.1,   sustain:0.8,  release:0.1,  mod_index:0.6,  mod_ratio:0.5  },
    /* 22: Harmonica          */ { attack:0.01,   decay:0.1,   sustain:0.75, release:0.15, mod_index:0.8,  mod_ratio:1.0  },
    /* 23: Tango Accordion    */ { attack:0.01,   decay:0.1,   sustain:0.8,  release:0.1,  mod_index:1.0,  mod_ratio:0.5  },
    /* 24: Nylon Guitar       */ { attack:0.002,  decay:0.15,  sustain:0.5,  release:0.3,  mod_index:0.8,  mod_ratio:1.0  },
    /* 25: Steel Guitar       */ { attack:0.002,  decay:0.1,   sustain:0.6,  release:0.25, mod_index:1.2,  mod_ratio:0.5  },
    /* 26: Jazz Guitar        */ { attack:0.002,  decay:0.15,  sustain:0.55, release:0.2,  mod_index:1.5,  mod_ratio:1.0  },
    /* 27: Clean Guitar       */ { attack:0.002,  decay:0.1,   sustain:0.65, release:0.2,  mod_index:0.5,  mod_ratio:0.5  },
    /* 28: Muted Guitar       */ { attack:0.001,  decay:0.1,   sustain:0.3,  release:0.2,  mod_index:1.0,  mod_ratio:2.0  },
    /* 29: Overdrive Guitar   */ { attack:0.001,  decay:0.1,   sustain:0.7,  release:0.15, mod_index:2.0,  mod_ratio:1.0  },
    /* 30: Distortion Guitar  */ { attack:0.001,  decay:0.1,   sustain:0.7,  release:0.15, mod_index:3.0,  mod_ratio:0.5  },
    /* 31: Guitar Harmonics   */ { attack:0.001,  decay:0.2,   sustain:0.5,  release:0.3,  mod_index:2.0,  mod_ratio:2.0  },
    /* 32: Acoustic Bass      */ { attack:0.002,  decay:0.2,   sustain:0.4,  release:0.25, mod_index:0.5,  mod_ratio:0.5  },
    /* 33: Electric Bass(f)   */ { attack:0.002,  decay:0.15,  sustain:0.5,  release:0.2,  mod_index:1.0,  mod_ratio:0.5  },
    /* 34: Electric Bass(p)   */ { attack:0.001,  decay:0.1,   sustain:0.5,  release:0.15, mod_index:1.5,  mod_ratio:0.5  },
    /* 35: Fretless Bass      */ { attack:0.005,  decay:0.3,   sustain:0.4,  release:0.2,  mod_index:0.3,  mod_ratio:1.0  },
    /* 36: Slap Bass 1        */ { attack:0.001,  decay:0.05,  sustain:0.6,  release:0.1,  mod_index:2.0,  mod_ratio:1.0  },
    /* 37: Slap Bass 2        */ { attack:0.001,  decay:0.08,  sustain:0.55, release:0.12, mod_index:2.5,  mod_ratio:1.0  },
    /* 38: Synth Bass 1       */ { attack:0.002,  decay:0.1,   sustain:0.6,  release:0.15, mod_index:3.0,  mod_ratio:0.5  },
    /* 39: Synth Bass 2       */ { attack:0.002,  decay:0.15,  sustain:0.5,  release:0.2,  mod_index:2.5,  mod_ratio:1.0  },
    /* 40-47: Strings         */ { attack:0.05,   decay:0.2,   sustain:0.7,  release:0.3,  mod_index:0.3,  mod_ratio:1.0  },
    /* 48-55: Ensemble        */ { attack:0.05,   decay:0.15,  sustain:0.75, release:0.25, mod_index:0.5,  mod_ratio:0.5  },
    /* 56-63: Brass           */ { attack:0.02,   decay:0.15,  sustain:0.6,  release:0.2,  mod_index:1.0,  mod_ratio:1.0  },
    /* 64-71: Reed            */ { attack:0.02,   decay:0.15,  sustain:0.65, release:0.15, mod_index:0.8,  mod_ratio:1.0  },
    /* 72-79: Pipe            */ { attack:0.02,   decay:0.2,   sustain:0.6,  release:0.2,  mod_index:0.5,  mod_ratio:1.5  },
    /* 80-87: Synth Lead      */ { attack:0.005,  decay:0.1,   sustain:0.7,  release:0.15, mod_index:2.0,  mod_ratio:1.0  },
    /* 88-95: Synth Pad       */ { attack:0.1,    decay:0.3,   sustain:0.6,  release:0.4,  mod_index:1.5,  mod_ratio:0.5  },
    /* 96-103: Synth Effects  */ { attack:0.05,   decay:0.3,   sustain:0.5,  release:0.3,  mod_index:2.5,  mod_ratio:2.0  },
    /* 104-111: Ethnic        */ { attack:0.01,   decay:0.2,   sustain:0.5,  release:0.3,  mod_index:1.0,  mod_ratio:1.0  },
    /* 112-119: Percussive    */ { attack:0.001,  decay:0.3,   sustain:0.3,  release:0.4,  mod_index:3.0,  mod_ratio:3.0  },
    /* 120-127: Sound Effects */ { attack:0.001,  decay:0.2,   sustain:0.4,  release:0.4,  mod_index:4.0,  mod_ratio:2.0  },
];

const g_default_patch = { attack:0.01, decay:0.1, sustain:0.7, release:0.2, mod_index:1.0, mod_ratio:1.0 };

// ---------------------------------------------------------------------------
// Helper: get FM patch for a program number
// ---------------------------------------------------------------------------

function getPatch(program) {
    if (program < g_patches.length) {
        return g_patches[program];
    }
    return g_default_patch;
}

// ---------------------------------------------------------------------------
// Helper: compute all note frequencies
// ---------------------------------------------------------------------------

function initNoteFrequencies() {
    // A4 = 440 Hz at MIDI note 69
    for (let i = 0; i < MIDI_NOTE_COUNT; i++) {
        g_note_frequencies[i] = 440.0 * Math.pow(2.0, (i - 69) / 12.0);
    }
}

// ---------------------------------------------------------------------------
// Helper: read MIDI variable-length value from a Uint8Array
// pos is an object { value: number } (simulates a C uint32_t* pointer)
// ---------------------------------------------------------------------------

function readVarLen(data, pos, max) {
    let value = 0;
    let byte_;
    do {
        if (pos.value >= max) return value;
        byte_ = data[pos.value++];
        value = ((value << 7) | (byte_ & 0x7F)) >>> 0;
    } while (byte_ & 0x80);
    return value;
}

// ---------------------------------------------------------------------------
// Helper: make a zeroed MidiNote object
// ---------------------------------------------------------------------------

function makeMidiNote() {
    return {
        active:          false,
        channel:         0,
        note:            0,
        velocity:        0,
        frequency:       0.0,
        phase:           0.0,
        envelope:        0.0,
        envelope_target: 0.0,
        mod_phase:       0.0,
        mod_index:       0.0,
    };
}

// ---------------------------------------------------------------------------
// Helper: make a zeroed MidiChannel object
// ---------------------------------------------------------------------------

function makeMidiChannel() {
    return {
        program:    0,
        volume:     100,
        pan:        64,
        expression: 127,
        sustain:    0,
        pitch_bend: 1.0,
        bank:       0,
        modulation: 0,
    };
}

// ---------------------------------------------------------------------------
// Internal: parse MIDI file header
// pos is { value: number }
// ---------------------------------------------------------------------------

function parseHeader(midi, pos) {
    if (midi.size < 14) return -1;

    // Check "MThd"
    if (midi.data[0] !== 0x4D || midi.data[1] !== 0x54 ||
        midi.data[2] !== 0x68 || midi.data[3] !== 0x64) {
        return -1;
    }

    const headerLen = ((midi.data[4] << 24) | (midi.data[5] << 16) |
                       (midi.data[6] <<  8) |  midi.data[7]) >>> 0;

    const hdr = midi.sequencer.header;
    hdr.format      = (midi.data[8]  << 8) | midi.data[9];
    hdr.track_count = (midi.data[10] << 8) | midi.data[11];
    hdr.division    = (midi.data[12] << 8) | midi.data[13];

    if (hdr.division & 0x8000) {
        hdr.division_type = 1; // SMPTE
        midi.sequencer.ticks_per_beat = 480;
    } else {
        hdr.division_type = 0; // ticks per beat
        midi.sequencer.ticks_per_beat = hdr.division & 0x7FFF;
    }

    pos.value = 8 + headerLen;
    return 0;
}

// ---------------------------------------------------------------------------
// Internal: parse a single track chunk
// ---------------------------------------------------------------------------

function parseTrack(midi, trackNum, pos) {
    if (pos.value + 8 > midi.size) return -1;

    const data = midi.data;
    const base = pos.value;

    // Check "MTrk"
    if (data[base]   !== 0x4D || data[base+1] !== 0x54 ||
        data[base+2] !== 0x72 || data[base+3] !== 0x6B) {
        return -1;
    }

    const length = ((data[base+4] << 24) | (data[base+5] << 16) |
                    (data[base+6] <<  8) |  data[base+7]) >>> 0;

    const track = midi.sequencer.tracks[trackNum];
    track.data   = data.subarray(base + 8, base + 8 + length);
    track.length = length;

    const tpos = { value: 0 };
    track.delta_time     = readVarLen(track.data, tpos, length);
    track.position       = tpos.value;
    track.running_status = 0;
    track.ended          = false;

    pos.value = base + 8 + length;
    return 0;
}

// ---------------------------------------------------------------------------
// Internal: find a free note slot (or steal slot 0)
// ---------------------------------------------------------------------------

function findFreeNote(seq) {
    for (let i = 0; i < seq.max_active_notes; i++) {
        if (!seq.active_notes[i].active) return i;
    }
    return 0; // steal oldest
}

// ---------------------------------------------------------------------------
// Internal: start a note
// ---------------------------------------------------------------------------

function startNote(seq, channel, note, velocity) {
    if (note >= MIDI_NOTE_COUNT) return;

    const slot = findFreeNote(seq);
    const n    = seq.active_notes[slot];

    n.active          = true;
    n.channel         = channel;
    n.note            = note;
    n.velocity        = velocity;
    n.frequency       = g_note_frequencies[note];
    n.phase           = 0.0;
    n.mod_phase       = 0.0;
    n.envelope        = 0.0;
    n.envelope_target = velocity / 127.0;

    const patch  = getPatch(seq.channels[channel].program);
    n.mod_index  = patch.mod_index;

    seq.active_note_count++;
}

// ---------------------------------------------------------------------------
// Internal: stop a note (begin release phase)
// ---------------------------------------------------------------------------

function stopNote(seq, channel, note) {
    for (let i = 0; i < seq.max_active_notes; i++) {
        const n = seq.active_notes[i];
        if (n.active && n.channel === channel && n.note === note) {
            n.envelope_target = 0.0;
        }
    }
}

// ---------------------------------------------------------------------------
// Internal: process one MIDI event from a track
// ---------------------------------------------------------------------------

function processEvent(seq, track) {
    const data = track.data;
    let   pos  = track.position;
    const len  = track.length;

    if (pos >= len) {
        track.ended = true;
        return;
    }

    let status = data[pos++];

    if (status < 0x80) {
        // Running status
        status = track.running_status;
        pos--;
    } else {
        track.running_status = status;
    }

    const eventType = status & 0xF0;
    const channel   = status & 0x0F;

    switch (eventType) {
        case MIDI_EVENT_NOTE_OFF:
            if (pos + 1 < len) {
                const d1 = data[pos++];
                /* data2 = */ pos++;
                stopNote(seq, channel, d1);
            }
            break;

        case MIDI_EVENT_NOTE_ON:
            if (pos + 1 < len) {
                const d1 = data[pos++];
                const d2 = data[pos++];
                if (d2 === 0) {
                    stopNote(seq, channel, d1);
                } else {
                    startNote(seq, channel, d1, d2);
                }
            }
            break;

        case MIDI_EVENT_CONTROL_CHANGE:
            if (pos + 1 < len) {
                const controller = data[pos++];
                const value      = data[pos++];
                switch (controller) {
                    case MIDI_CTRL_VOLUME:
                        seq.channels[channel].volume = value;
                        break;
                    case MIDI_CTRL_PAN:
                        seq.channels[channel].pan = value;
                        break;
                    case MIDI_CTRL_EXPRESSION:
                        seq.channels[channel].expression = value;
                        break;
                    case MIDI_CTRL_SUSTAIN:
                        seq.channels[channel].sustain = value;
                        break;
                    case MIDI_CTRL_BANK_SELECT:
                        seq.channels[channel].bank = value;
                        break;
                }
            }
            break;

        case MIDI_EVENT_PROGRAM_CHANGE:
            if (pos < len) {
                seq.channels[channel].program = data[pos++];
            }
            break;

        case MIDI_EVENT_PITCH_BEND:
            if (pos + 1 < len) {
                const d1   = data[pos++];
                const d2   = data[pos++];
                // Combine 14-bit value then subtract 8192 for signed
                const bend = (((d2 << 7) | d1) - 8192);
                seq.channels[channel].pitch_bend = Math.pow(2.0, bend / 8192.0 / 6.0);
            }
            break;

        case MIDI_EVENT_SYSTEM:
            if (status === 0xFF) {
                // Meta event
                if (pos >= len) break;
                const metaType = data[pos++];
                const metaPos  = { value: pos };
                const metaLen  = readVarLen(data, metaPos, len);
                pos = metaPos.value;

                if (metaType === 0x51 && metaLen === 3) {
                    // Tempo change
                    seq.microseconds_per_beat = ((data[pos] << 16) |
                                                 (data[pos+1] << 8) |
                                                  data[pos+2]) >>> 0;
                    const secondsPerBeat = seq.microseconds_per_beat / 1000000.0;
                    seq.samples_per_tick = (seq.sample_rate * secondsPerBeat) /
                                           seq.ticks_per_beat;
                }
                pos += metaLen;
            } else if (status === 0xF0 || status === 0xF7) {
                // SysEx — skip to end marker
                while (pos < len && data[pos] !== 0xF7) pos++;
                if (pos < len) pos++;
            }
            break;

        default:
            // Skip unknown events (up to 2 data bytes)
            if (pos < len) pos++;
            if (pos < len) pos++;
            break;
    }

    track.position = pos;

    // Read next delta time
    if (track.position < track.length) {
        const tpos = { value: track.position };
        track.delta_time = readVarLen(track.data, tpos, track.length);
        track.position   = tpos.value;
    } else {
        track.ended = true;
    }
}

// ===========================================================================
// Public API
// ===========================================================================

/**
 * Initialize the MIDI subsystem.
 * @param {number} sample_rate - Audio sample rate in Hz.
 * @returns {number} 0 on success.
 */
export function midi_init(sample_rate) {
    g_sample_rate = sample_rate;
    initNoteFrequencies();
    return 0;
}

/**
 * Shutdown the MIDI subsystem.
 */
export function midi_shutdown() {
    g_active_sequencer = null;
}

/**
 * Load a MIDI file from a Uint8Array buffer.
 * @param {Uint8Array} data
 * @param {number} size
 * @returns {object|null} MidiFile object or null on failure.
 */
export function midi_load(data, size) {
    if (!data || size < 14) return null;

    // Copy data so we own the buffer
    const buf = new Uint8Array(size);
    buf.set(data.subarray(0, size));

    // Build sequencer
    const seq = {
        header: {
            format:        0,
            track_count:   0,
            division:      0,
            division_type: 0,
        },
        tracks:                 null,
        channels:               Array.from({ length: MIDI_CHANNELS }, () => makeMidiChannel()),
        active_notes:           null,
        max_active_notes:       MAX_POLYPHONY,
        active_note_count:      0,
        ticks_per_beat:         DEFAULT_TICKS_PER_BEAT,
        microseconds_per_beat:  DEFAULT_TEMPO,
        current_tempo:          120.0,
        position:               0,
        playing:                false,
        loop:                   false,
        volume:                 1.0,
        sample_rate:            g_sample_rate,
        samples_per_tick:       0.0,
        sample_accumulator:     0.0,
        last_real_time_us:      0,
        tick_accumulator:       0.0,
    };

    const midi = { data: buf, size, sequencer: seq };

    // Parse header
    const pos = { value: 0 };
    if (parseHeader(midi, pos) !== 0) return null;

    // Allocate track objects
    seq.tracks = Array.from({ length: seq.header.track_count }, () => ({
        data:           null,
        length:         0,
        position:       0,
        delta_time:     0,
        running_status: 0,
        ended:          false,
    }));

    // Parse each track
    for (let i = 0; i < seq.header.track_count; i++) {
        if (parseTrack(midi, i, pos) !== 0) {
            return null;
        }
    }

    // Calculate samples per tick
    const secondsPerBeat = seq.microseconds_per_beat / 1000000.0;
    seq.samples_per_tick = (g_sample_rate * secondsPerBeat) / seq.ticks_per_beat;

    // Allocate active note pool
    seq.active_notes = Array.from({ length: MAX_POLYPHONY }, () => makeMidiNote());

    return midi;
}

/**
 * Free a previously loaded MidiFile (no-op in JS — GC handles it).
 * Kept for API compatibility.
 * @param {object} midi
 */
export function midi_free(midi) {
    // GC handles cleanup; nothing to do
}

/**
 * Start playback of a loaded MIDI file.
 * @param {object} midi
 */
export function midi_play(midi) {
    if (!midi || !midi.sequencer) return;
    midi.sequencer.playing = true;
    g_active_sequencer = midi.sequencer;
}

/**
 * Stop playback and silence all notes.
 * @param {object} midi
 */
export function midi_stop(midi) {
    if (!midi || !midi.sequencer) return;
    midi.sequencer.playing = false;
    for (let i = 0; i < midi.sequencer.max_active_notes; i++) {
        midi.sequencer.active_notes[i].active = false;
    }
    midi.sequencer.active_note_count = 0;
}

/**
 * Pause playback (notes continue to ring out).
 * @param {object} midi
 */
export function midi_pause(midi) {
    if (!midi || !midi.sequencer) return;
    midi.sequencer.playing = false;
}

/**
 * Resume playback after a pause.
 * @param {object} midi
 */
export function midi_resume(midi) {
    if (!midi || !midi.sequencer) return;
    midi.sequencer.playing = true;
}

/**
 * Set loop mode.
 * @param {object} midi
 * @param {boolean} loop
 */
export function midi_set_loop(midi, loop) {
    if (!midi || !midi.sequencer) return;
    midi.sequencer.loop = loop;
}

/**
 * Set master volume (0.0 – 1.0).
 * @param {object} midi
 * @param {number} volume
 */
export function midi_set_volume(midi, volume) {
    if (!midi || !midi.sequencer) return;
    if (volume < 0.0) volume = 0.0;
    if (volume > 1.0) volume = 1.0;
    midi.sequencer.volume = volume;
}

/**
 * Get current playback position in seconds.
 * @param {object} midi
 * @returns {number}
 */
export function midi_get_position(midi) {
    if (!midi || !midi.sequencer) return 0.0;
    const seq = midi.sequencer;
    if (seq.ticks_per_beat > 0 && seq.microseconds_per_beat > 0) {
        const secondsPerTick = seq.microseconds_per_beat / seq.ticks_per_beat / 1000000.0;
        return seq.position * secondsPerTick;
    }
    return 0.0;
}

/**
 * Get total file length in seconds (estimated from track byte lengths).
 * @param {object} midi
 * @returns {number}
 */
export function midi_get_length(midi) {
    if (!midi || !midi.sequencer) return 0.0;
    const seq = midi.sequencer;
    let totalTicks = 0;
    for (let i = 0; i < seq.header.track_count; i++) {
        if (seq.tracks[i].length > totalTicks) {
            totalTicks = seq.tracks[i].length;
        }
    }
    if (seq.ticks_per_beat > 0 && seq.microseconds_per_beat > 0) {
        const secondsPerTick = seq.microseconds_per_beat / seq.ticks_per_beat / 1000000.0;
        return totalTicks * secondsPerTick;
    }
    return 0.0;
}

/**
 * Generate interleaved stereo 16-bit PCM audio into a pre-allocated Int16Array.
 * The buffer must hold at least `samples * 2` elements.
 * @param {object}     midi
 * @param {Int16Array} buffer
 * @param {number}     samples  Number of sample frames (each frame = L + R).
 */
export function midi_generate_samples(midi, buffer, samples) {
    if (!midi || !midi.sequencer || !buffer) return;

    const seq = midi.sequencer;

    // Zero the output buffer (stereo)
    buffer.fill(0, 0, samples * 2);

    if (!seq.playing) return;

    const TWO_PI = 2.0 * M_PI;

    for (let s = 0; s < samples; s++) {
        let left  = 0.0;
        let right = 0.0;

        for (let i = 0; i < seq.max_active_notes; i++) {
            const note = seq.active_notes[i];
            if (!note.active) continue;

            const ch    = seq.channels[note.channel];
            const patch = getPatch(ch.program);

            // Envelope
            const envDiff = note.envelope_target - note.envelope;
            if (note.envelope_target > note.envelope) {
                note.envelope += envDiff * patch.attack;   // attack
            } else {
                note.envelope += envDiff * patch.release;  // release
            }

            if (note.envelope < 0.001 && note.envelope_target === 0.0) {
                note.active = false;
                seq.active_note_count--;
                continue;
            }

            // Frequency with pitch bend
            const freq    = note.frequency * ch.pitch_bend;
            const modFreq = freq * patch.mod_ratio;

            const modWave  = Math.sin(note.mod_phase);
            const phaseInc    = (TWO_PI * freq)    / seq.sample_rate;
            const modPhaseInc = (TWO_PI * modFreq) / seq.sample_rate;

            // FM carrier
            const carrier = Math.sin(note.phase + note.mod_index * modWave);

            // Apply envelope, velocity, channel volume & expression
            let sample = carrier *
                         note.envelope *
                         (note.velocity / 127.0) *
                         (ch.volume     / 127.0) *
                         (ch.expression / 127.0);

            // Pan (0..127 mapped to 0..1)
            const pan  = ch.pan / 127.0;
            left  += sample * (1.0 - pan);
            right += sample * pan;

            // Advance phases, wrap to [0, 2*PI)
            note.phase     += phaseInc;
            note.mod_phase += modPhaseInc;
            if (note.phase     > TWO_PI) note.phase     -= TWO_PI;
            if (note.mod_phase > TWO_PI) note.mod_phase -= TWO_PI;
        }

        // Master volume and hard clip to [-1, 1]
        left  *= seq.volume;
        right *= seq.volume;
        if (left  >  1.0) left  =  1.0;
        if (left  < -1.0) left  = -1.0;
        if (right >  1.0) right =  1.0;
        if (right < -1.0) right = -1.0;

        // Convert to 16-bit signed PCM
        buffer[s * 2]     = (left  * 32767) | 0;
        buffer[s * 2 + 1] = (right * 32767) | 0;
    }
}

/**
 * Advance the sequencer by a real-time interval for time-based synchronisation.
 * @param {object} midi
 * @param {number} elapsed_us  Elapsed microseconds (use BigInt-free numeric value).
 */
export function midi_advance_time(midi, elapsed_us) {
    if (!midi || !midi.sequencer || !midi.sequencer.playing) return;

    const seq = midi.sequencer;

    const ticks = (elapsed_us / seq.microseconds_per_beat) * seq.ticks_per_beat;
    seq.tick_accumulator += ticks;

    while (seq.tick_accumulator >= 1.0) {
        seq.tick_accumulator -= 1.0;
        midi_process_tick(seq);
    }
}

/**
 * Process one sequencer tick across all tracks.
 * @param {object} seq  MidiSequencer object.
 */
export function midi_process_tick(seq) {
    if (!seq || !seq.playing) return;

    let allEnded = true;

    for (let i = 0; i < seq.header.track_count; i++) {
        const track = seq.tracks[i];
        if (track.ended) continue;
        allEnded = false;

        // Drain all zero-delta events
        while (track.delta_time === 0 && !track.ended) {
            processEvent(seq, track);
        }

        if (!track.ended) {
            track.delta_time--;
        }
    }

    if (allEnded) {
        if (seq.loop) {
            // Restart all tracks
            for (let i = 0; i < seq.header.track_count; i++) {
                const track = seq.tracks[i];
                const tpos  = { value: 0 };
                track.delta_time     = readVarLen(track.data, tpos, track.length);
                track.position       = tpos.value;
                track.running_status = 0;
                track.ended          = false;
            }
        } else {
            seq.playing = false;
        }
    }
}

// ---------------------------------------------------------------------------
// Direct MIDI command API (operates on g_active_sequencer)
// ---------------------------------------------------------------------------

/**
 * Send a Note On to the active sequencer.
 * @param {number} channel
 * @param {number} note
 * @param {number} velocity
 */
export function midi_note_on(channel, note, velocity) {
    if (g_active_sequencer) {
        startNote(g_active_sequencer, channel, note, velocity);
    }
}

/**
 * Send a Note Off to the active sequencer.
 * @param {number} channel
 * @param {number} note
 */
export function midi_note_off(channel, note) {
    if (g_active_sequencer) {
        stopNote(g_active_sequencer, channel, note);
    }
}

/**
 * Send a Control Change to the active sequencer.
 * @param {number} channel
 * @param {number} controller
 * @param {number} value
 */
export function midi_control_change(channel, controller, value) {
    if (!g_active_sequencer) return;
    switch (controller) {
        case MIDI_CTRL_VOLUME:
            g_active_sequencer.channels[channel].volume = value;
            break;
        case MIDI_CTRL_PAN:
            g_active_sequencer.channels[channel].pan = value;
            break;
        case MIDI_CTRL_EXPRESSION:
            g_active_sequencer.channels[channel].expression = value;
            break;
    }
}

/**
 * Send a Program Change to the active sequencer.
 * @param {number} channel
 * @param {number} program
 */
export function midi_program_change(channel, program) {
    if (g_active_sequencer) {
        g_active_sequencer.channels[channel].program = program;
    }
}

/**
 * Send a Pitch Bend to the active sequencer.
 * @param {number} channel
 * @param {number} value  Signed 14-bit bend value (-8192 .. +8191).
 */
export function midi_pitch_bend(channel, value) {
    if (g_active_sequencer) {
        g_active_sequencer.channels[channel].pitch_bend =
            Math.pow(2.0, value / 8192.0 / 6.0);
    }
}

/**
 * Return the currently active MidiSequencer, or null.
 * @returns {object|null}
 */
export function midi_get_active_sequencer() {
    return g_active_sequencer;
}

// ---------------------------------------------------------------------------
// Additional functions declared in midi.h (used by media.c)
// ---------------------------------------------------------------------------

/**
 * Get total duration in milliseconds.
 * @param {object} midi
 * @returns {number}
 */
export function midi_get_duration_ms(midi) {
    if (!midi || !midi.sequencer) return 0;
    const seq = midi.sequencer;
    let totalTicks = 0;
    for (let i = 0; i < seq.header.track_count; i++) {
        if (seq.tracks[i].length > totalTicks) {
            totalTicks = seq.tracks[i].length;
        }
    }
    const ticksPerMs = seq.ticks_per_beat / (seq.microseconds_per_beat / 1000.0);
    return (totalTicks / ticksPerMs) | 0;
}

/**
 * Get current playback position in milliseconds.
 * @param {object} midi
 * @returns {number}
 */
export function midi_get_position_ms(midi) {
    if (!midi || !midi.sequencer) return 0;
    const seq       = midi.sequencer;
    const ticksPerMs = seq.ticks_per_beat / (seq.microseconds_per_beat / 1000.0);
    return (seq.position / ticksPerMs) | 0;
}

/**
 * Seek to a position in milliseconds.
 * Resets all track byte-positions to zero (no byte-accurate seek).
 * @param {object} midi
 * @param {number} position_ms
 */
export function midi_set_position_ms(midi, position_ms) {
    if (!midi || !midi.sequencer) return;
    const seq        = midi.sequencer;
    const ticksPerMs = seq.ticks_per_beat / (seq.microseconds_per_beat / 1000.0);
    seq.position     = (position_ms * ticksPerMs) | 0;
    for (let i = 0; i < seq.header.track_count; i++) {
        seq.tracks[i].position = 0;
        seq.tracks[i].ended    = false;
    }
}

/**
 * Get the volume of a channel from the active sequencer.
 * @param {number} channel
 * @returns {number}
 */
export function midi_get_channel_volume(channel) {
    if (!g_active_sequencer || channel < 0 || channel >= MIDI_CHANNELS) return 100;
    return g_active_sequencer.channels[channel].volume;
}

/**
 * Get the program (instrument) number for a channel.
 * @param {number} channel
 * @returns {number}
 */
export function midi_get_program(channel) {
    if (!g_active_sequencer || channel < 0 || channel >= MIDI_CHANNELS) return 0;
    return g_active_sequencer.channels[channel].program;
}
