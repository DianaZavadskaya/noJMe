import {
    midi_init,
    midi_load,
    midi_free,
    midi_play,
    midi_stop,
    midi_set_loop,
    midi_generate_samples,
    midi_advance_time,
    midi_note_on,
    midi_note_off,
    midi_control_change,
    midi_program_change,
    midi_get_duration_ms,
    midi_get_position_ms,
    midi_set_position_ms,
    midi_get_channel_volume,
    midi_get_program,
} from '../midi/midi.mjs';

// ============================================================
// JSR-135 player state constants
// ============================================================

export const PLAYER_UNREALIZED = 100;
export const PLAYER_REALIZED   = 200;
export const PLAYER_PREFETCHED = 300;
export const PLAYER_STARTED    = 400;
export const PLAYER_CLOSED     = 0;

export const TIME_UNKNOWN = -1n;

// ToneControl sequence tokens
export const TONE_CONTROL_VERSION    = -2;
export const TONE_CONTROL_TEMPO      = -3;
export const TONE_CONTROL_RESOLUTION = -4;
export const TONE_CONTROL_BLOCK_START = -5;
export const TONE_CONTROL_BLOCK_END  = -6;
export const TONE_CONTROL_PLAY_BLOCK = -7;
export const TONE_CONTROL_SET_VOLUME = -8;
export const TONE_CONTROL_REPEAT     = -9;
export const TONE_CONTROL_SILENCE    = -1;

const TONE_MIN_NOTE    = 0;
const TONE_MAX_NOTE    = 127;
const TONE_MIN_VOLUME  = 0;
const TONE_MAX_VOLUME  = 100;
const TONE_MAX_DURATION = 30000;

export const AUDIO_SAMPLE_RATE = 44100;
export const AUDIO_BUFFER_SIZE = 4096;
export const MAX_PLAYERS       = 32;

// ============================================================
// PlayerType enum
// ============================================================

export const PlayerType = Object.freeze({
    NONE:        0,
    MIDI:        1,
    WAV:         2,
    TONE:        3,
    TONE_DEVICE: 4,
});

// ============================================================
// IMA ADPCM step tables
// ============================================================

const IMA_STEP_INDEX_TABLE = new Int8Array([
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
]);

const IMA_STEP_SIZE_TABLE = new Int32Array([
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
    598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132,
    7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
    24623, 27086, 29794, 32767,
]);

// ============================================================
// IMA ADPCM decoder
// ============================================================

function decode_ima_adpcm(adpcm_data, adpcm_size, channels, block_align) {
    if (!adpcm_data || adpcm_size === 0 || channels < 1 || channels > 2 || block_align === 0)
        return null;

    const block_size_per_ch = Math.floor(block_align / channels);
    if (block_size_per_ch < 6) return null;

    const samples_per_block = (block_size_per_ch - 4) * 2;
    if (samples_per_block <= 0) return null;

    const num_blocks = Math.floor(adpcm_size / block_align);
    if (num_blocks === 0) return null;

    const total_samples_per_ch = num_blocks * samples_per_block;
    const pcm = new Int16Array(total_samples_per_ch * channels);

    for (let blk = 0; blk < num_blocks; blk++) {
        const block_start = blk * block_align;
        const base_sample = blk * samples_per_block;

        for (let ch = 0; ch < channels; ch++) {
            let pos = block_start + ch * block_size_per_ch;
            if (pos + 3 >= adpcm_size) break;

            let predictor = (adpcm_data[pos] | (adpcm_data[pos + 1] << 8));
            // Sign-extend 16-bit
            if (predictor > 0x7FFF) predictor -= 0x10000;
            let index = adpcm_data[pos + 2] & 0xFF;
            if (index > 88) index = 88;
            pos += 4;

            const data_bytes = block_size_per_ch - 4;
            let sample_idx = 0;

            for (let b = 0; b < data_bytes && sample_idx < samples_per_block; b++) {
                if (pos >= adpcm_size) break;
                const byte_val = adpcm_data[pos++];

                for (let nib = 0; nib < 2 && sample_idx < samples_per_block; nib++) {
                    const nibble = (nib === 0) ? (byte_val & 0x0F) : ((byte_val >> 4) & 0x0F);

                    const step = IMA_STEP_SIZE_TABLE[index];
                    let diff = step >> 3;
                    if (nibble & 1) diff += step >> 2;
                    if (nibble & 2) diff += step >> 1;
                    if (nibble & 4) diff += step;

                    if (nibble & 8) predictor -= diff;
                    else            predictor += diff;

                    if (predictor < -32768) predictor = -32768;
                    if (predictor >  32767) predictor =  32767;

                    index += IMA_STEP_INDEX_TABLE[nibble];
                    if (index < 0) index = 0;
                    if (index > 88) index = 88;

                    const out_idx = (base_sample + sample_idx) * channels + ch;
                    pcm[out_idx] = predictor;
                    sample_idx++;
                }
            }
        }
    }

    return pcm;
}

// ============================================================
// WAV parser
// ============================================================

function wav_parse(data) {
    if (data.length < 44) return null;
    if (data[0] !== 0x52 || data[1] !== 0x49 || data[2] !== 0x46 || data[3] !== 0x46) return null;
    if (data[8] !== 0x57 || data[9] !== 0x41 || data[10] !== 0x56 || data[11] !== 0x45) return null;

    const wav = {
        audio_format:   0,
        num_channels:   0,
        sample_rate:    0,
        byte_rate:      0,
        block_align:    0,
        bits_per_sample: 0,
        data:           null,
        data_size:      0,
        duration_ms:    0,
    };

    let pos = 12;
    while (pos + 8 <= data.length) {
        const chunk_size = data[pos+4] | (data[pos+5] << 8) | (data[pos+6] << 16) | (data[pos+7] << 24);
        if (pos + 8 + chunk_size > data.length) break;

        const tag = String.fromCharCode(data[pos], data[pos+1], data[pos+2], data[pos+3]);

        if (tag === 'fmt ') {
            pos += 8;
            if (chunk_size >= 16) {
                wav.audio_format    = data[pos]   | (data[pos+1] << 8);
                wav.num_channels    = data[pos+2] | (data[pos+3] << 8);
                wav.sample_rate     = data[pos+4] | (data[pos+5] << 8) | (data[pos+6] << 16) | (data[pos+7] << 24);
                wav.byte_rate       = data[pos+8] | (data[pos+9] << 8) | (data[pos+10] << 16) | (data[pos+11] << 24);
                wav.block_align     = data[pos+12] | (data[pos+13] << 8);
                wav.bits_per_sample = data[pos+14] | (data[pos+15] << 8);
            }
            pos += chunk_size;
        } else if (tag === 'data') {
            pos += 8;
            wav.data      = data.slice(pos, pos + chunk_size);
            wav.data_size = chunk_size;
            if (wav.byte_rate > 0) {
                wav.duration_ms = Math.floor((chunk_size * 1000) / wav.byte_rate);
            }
            pos += chunk_size;
        } else {
            pos += 8 + chunk_size;
        }
        if (chunk_size & 1) pos++;
    }

    // Decode IMA ADPCM (format 17) to PCM
    if (wav.audio_format === 17) {
        const pcm = decode_ima_adpcm(wav.data, wav.data_size, wav.num_channels, wav.block_align);
        if (!pcm) return null;
        wav.data            = new Uint8Array(pcm.buffer);
        wav.data_size       = wav.data.length;
        wav.audio_format    = 1;
        wav.bits_per_sample = 16;
        wav.block_align     = wav.num_channels * 2;
        wav.byte_rate       = wav.sample_rate * wav.block_align;
        wav.duration_ms     = wav.byte_rate > 0 ? Math.floor((wav.data_size * 1000) / wav.byte_rate) : 0;
    }

    if (wav.audio_format !== 1 || !wav.data) return null;

    return wav;
}

// ============================================================
// Tone sequence parser
// ============================================================

function tone_sequence_parse(data, length) {
    if (length < 2) return null;

    const ts = {
        sequence:        new Uint8Array(data.buffer ? data.buffer.slice(data.byteOffset, data.byteOffset + length) : data.slice(0, length)),
        length:          length,
        position:        0,
        tempo:           120,
        resolution:      64,
        playing:         false,
        loop:            false,
        loop_count:      0,
        loops_remaining: 0,
        start_time_ms:   0,
        current_note:    -1,
        current_duration: 0,
        current_volume:  100,
    };

    let pos = 0;
    while (pos < length - 1) {
        // Read as signed byte
        const cmd = (ts.sequence[pos] << 24) >> 24;

        if (cmd === TONE_CONTROL_VERSION) {
            pos += 2;
        } else if (cmd === TONE_CONTROL_TEMPO) {
            if (pos + 1 < length) {
                ts.tempo = ts.sequence[pos + 1] & 0xFF;
                if (ts.tempo < 5)   ts.tempo = 5;
                if (ts.tempo > 127) ts.tempo = 127;
            }
            pos += 2;
        } else if (cmd === TONE_CONTROL_RESOLUTION) {
            if (pos + 1 < length) {
                ts.resolution = ts.sequence[pos + 1] & 0xFF;
                if (ts.resolution === 0) ts.resolution = 64;
            }
            pos += 2;
        } else {
            break;
        }
    }

    ts.position = pos;
    return ts;
}

// ============================================================
// Module-level player state
// ============================================================

const g_players = new Array(MAX_PLAYERS).fill(null).map(() => _make_player());
let g_next_player_id = 1;
let g_audio_initialized = false;

// When true, audio is generated synchronously (libretro mode).
// JS builds default to async (setInterval-based) matching the non-libretro C path.
let g_libretro_mode = false;

let g_start_time_us = 0n;

// Interval handle for async audio pump
let g_audio_interval = null;

// Per-player last_midi_time (was a static inside mix_midi_player in C)
// Keyed by player id.
const g_midi_last_time = new Map();

function _make_player() {
    return {
        id:                  0,
        type:                PlayerType.NONE,
        state:               PLAYER_CLOSED,
        playing:             false,
        looping:             false,
        loop_count:          0,
        loops_remaining:     0,
        muted:               false,
        volume:              1.0,
        wav:                 null,
        wav_position:        0,
        wav_samples_played:  0,
        midi:                null,
        tone_seq:            null,
        data:                null,
        size:                0,
        content_type:        null,
    };
}

function _reset_player(p) {
    p.id               = 0;
    p.type             = PlayerType.NONE;
    p.state            = PLAYER_CLOSED;
    p.playing          = false;
    p.looping          = false;
    p.loop_count       = 0;
    p.loops_remaining  = 0;
    p.muted            = false;
    p.volume           = 1.0;
    p.wav              = null;
    p.wav_position     = 0;
    p.wav_samples_played = 0;
    p.midi             = null;
    p.tone_seq         = null;
    p.data             = null;
    p.size             = 0;
    p.content_type     = null;
}

// ============================================================
// Time utilities
// ============================================================

function get_time_us() {
    // performance.now() gives sub-ms resolution; convert to integer microseconds.
    return BigInt(Math.trunc(performance.now() * 1000));
}

function get_time_ms() {
    return Math.trunc(performance.now());
}

// ============================================================
// Audio mixer — generates Int16Array stereo interleaved output
// ============================================================

function mix_wav_player(player, buffer, samples) {
    if (!player.wav || !player.playing) return;

    const wav = player.wav;
    const vol = player.muted ? 0.0 : player.volume;
    const src_bytes_per_sample = wav.num_channels * (wav.bits_per_sample >> 3);
    const src_ratio = wav.sample_rate / AUDIO_SAMPLE_RATE;

    for (let i = 0; i < samples && player.playing; i++) {
        const src_sample = Math.trunc(player.wav_samples_played * src_ratio);
        let src_byte = src_sample * src_bytes_per_sample;

        if (src_byte >= wav.data_size) {
            if (player.looping && player.loop_count !== 1) {
                player.wav_position       = 0;
                player.wav_samples_played = 0;
                src_byte = 0;
                if (player.loop_count > 0) {
                    player.loops_remaining--;
                    if (player.loops_remaining <= 0) {
                        player.playing = false;
                        player.state   = PLAYER_PREFETCHED;
                        break;
                    }
                }
            } else {
                player.playing = false;
                player.state   = PLAYER_PREFETCHED;
                break;
            }
        }

        let left = 0, right = 0;

        if (wav.bits_per_sample === 8) {
            const l = wav.data[src_byte];
            left = (l - 128) * 256;
            if (wav.num_channels >= 2 && src_byte + 1 < wav.data_size) {
                right = (wav.data[src_byte + 1] - 128) * 256;
            } else {
                right = left;
            }
        } else if (wav.bits_per_sample === 16) {
            if (src_byte + 1 < wav.data_size) {
                left = (wav.data[src_byte] | (wav.data[src_byte + 1] << 8));
                if (left > 0x7FFF) left -= 0x10000;
            }
            if (wav.num_channels >= 2 && src_byte + 3 < wav.data_size) {
                right = (wav.data[src_byte + 2] | (wav.data[src_byte + 3] << 8));
                if (right > 0x7FFF) right -= 0x10000;
            } else {
                right = left;
            }
        }

        // Clamp to int16 range after volume scaling
        let l16 = Math.trunc(left  * vol);
        let r16 = Math.trunc(right * vol);
        if (l16 < -32768) l16 = -32768; if (l16 > 32767) l16 = 32767;
        if (r16 < -32768) r16 = -32768; if (r16 > 32767) r16 = 32767;

        buffer[i * 2]     += l16;
        buffer[i * 2 + 1] += r16;

        player.wav_samples_played++;
    }
}

function mix_tone_player(player, buffer, samples) {
    if (!player.tone_seq || !player.playing) return;

    const ts = player.tone_seq;
    if (ts.tempo === 0)      ts.tempo = 120;
    if (ts.resolution === 0) ts.resolution = 64;

    const vol = player.muted ? 0.0 : player.volume;
    let now = get_time_ms() - ts.start_time_ms;

    for (let i = 0; i < samples && player.playing; i++) {
        if (ts.current_note >= 0 && ts.current_note < 128) {
            const freq = 440.0 * Math.pow(2.0, (ts.current_note - 69) / 12.0);
            const amplitude = (ts.current_volume / 100.0) * vol;
            const sample_time_us = now * 1000 + Math.trunc(i * 1000000 / AUDIO_SAMPLE_RATE);
            const sample = amplitude * Math.sin(2.0 * Math.PI * freq * sample_time_us / 1000000.0);
            const pcm = Math.trunc(sample * 16000);
            buffer[i * 2]     += pcm;
            buffer[i * 2 + 1] += pcm;
        }

        const note_end_ms = Math.trunc(ts.current_duration * 60000 / ts.tempo / ts.resolution);
        if (now >= note_end_ms) {
            if (ts.position >= ts.length - 1) {
                if (player.looping && player.loop_count !== 1) {
                    ts.position = 0;
                    if (player.loop_count > 0) {
                        player.loops_remaining--;
                        if (player.loops_remaining <= 0) {
                            player.playing        = false;
                            player.state          = PLAYER_PREFETCHED;
                            ts.current_note       = -1;
                            break;
                        }
                    }
                } else {
                    player.playing  = false;
                    player.state    = PLAYER_PREFETCHED;
                    ts.current_note = -1;
                    break;
                }
            }

            const cmd = (ts.sequence[ts.position] << 24) >> 24;

            if (cmd === TONE_CONTROL_SET_VOLUME && ts.position + 1 < ts.length) {
                ts.current_volume = ts.sequence[ts.position + 1] & 0xFF;
                ts.position += 2;
            } else if (cmd === TONE_CONTROL_REPEAT && ts.position + 1 < ts.length) {
                ts.loop_count = ts.sequence[ts.position + 1] & 0xFF;
                ts.position  += 2;
            } else if (cmd === TONE_CONTROL_SILENCE) {
                const duration = ts.sequence[ts.position + 1] & 0xFF;
                ts.current_note     = -1;
                ts.current_duration = duration;
                ts.position        += 2;
            } else if (cmd >= 0 && cmd <= 127) {
                const note     = cmd;
                const duration = ts.sequence[ts.position + 1] & 0xFF;
                ts.current_note     = note;
                ts.current_duration = duration;
                ts.position        += 2;
            } else {
                ts.position++;
            }

            ts.start_time_ms = get_time_ms() - now;
        }

        now = get_time_ms() - ts.start_time_ms;
    }
}

function mix_midi_player(player, buffer, samples) {
    if (!player.midi || !player.playing) return;

    const temp_buf = new Int16Array(samples * 2);

    const current_time = get_time_us();
    let elapsed_us = 0n;
    const last = g_midi_last_time.get(player.id) || 0n;

    if (last > 0n) {
        elapsed_us = current_time - last;
        if (elapsed_us < 1000n)   elapsed_us = 1000n;
        if (elapsed_us > 500000n) elapsed_us = 500000n;
    }
    g_midi_last_time.set(player.id, current_time);

    if (elapsed_us > 0n && player.midi.sequencer.playing) {
        midi_advance_time(player.midi, elapsed_us);
    }

    midi_generate_samples(player.midi, temp_buf, samples);

    const vol = player.muted ? 0.0 : player.volume;
    for (let i = 0; i < samples * 2; i++) {
        buffer[i] += Math.trunc(temp_buf[i] * vol);
    }

    if (!player.midi.sequencer.playing) {
        if (player.looping && player.loop_count !== 1) {
            midi_play(player.midi);
            g_midi_last_time.set(player.id, 0n);
            if (player.loop_count > 0) {
                player.loops_remaining--;
                if (player.loops_remaining <= 0) {
                    player.playing = false;
                    player.state   = PLAYER_PREFETCHED;
                }
            }
        } else {
            player.playing = false;
            player.state   = PLAYER_PREFETCHED;
        }
    }
}

// ============================================================
// Audio queuing — external SDL audio backend hook
// ============================================================

// The C code calls sdl_audio_queue_samples() from the audio thread.
// In JS we expose a callback slot; the backend (SDL/libretro JS shim) sets it.
let g_audio_queue_callback = null;

export function media_set_audio_queue_callback(fn) {
    g_audio_queue_callback = fn;
}

function sdl_audio_queue_samples(buffer, count) {
    if (g_audio_queue_callback) {
        g_audio_queue_callback(buffer, count);
    }
}

// ============================================================
// Mix pass — shared by async loop and synchronous generation
// ============================================================

function _do_mix(samples) {
    const mix_buffer = new Int16Array(samples * 2);

    for (let i = 0; i < MAX_PLAYERS; i++) {
        const p = g_players[i];
        if (!p.playing) continue;

        switch (p.type) {
            case PlayerType.WAV:
                mix_wav_player(p, mix_buffer, samples);
                break;
            case PlayerType.TONE:
            case PlayerType.TONE_DEVICE:
                mix_tone_player(p, mix_buffer, samples);
                break;
            case PlayerType.MIDI:
                mix_midi_player(p, mix_buffer, samples);
                break;
            default:
                break;
        }
    }

    return mix_buffer;
}

// ============================================================
// Audio initialisation
// ============================================================

function ensure_audio_init() {
    if (g_audio_initialized) return;

    midi_init(AUDIO_SAMPLE_RATE);
    g_start_time_us = get_time_us();

    if (!g_libretro_mode) {
        // Pump audio every 5 ms (mirrors the usleep(5000) in the C audio thread)
        g_audio_interval = setInterval(() => {
            const mix = _do_mix(AUDIO_BUFFER_SIZE);
            sdl_audio_queue_samples(mix, AUDIO_BUFFER_SIZE * 2);
        }, 5);
    }

    g_audio_initialized = true;
}

// ============================================================
// Synchronous audio generation (libretro / non-thread path)
// ============================================================

export function media_generate_audio_samples(samples) {
    if (!g_audio_initialized || !g_libretro_mode) return;
    const mix = _do_mix(samples);
    sdl_audio_queue_samples(mix, samples * 2);
}

// Called from SDL audio callback — kept as no-op; mixing is now timer-driven.
export function media_generate_wav_samples(buffer, samples) {
    // intentionally empty — audio is generated by the interval pump or
    // media_generate_audio_samples() in libretro mode
}

// ============================================================
// Player slot management
// ============================================================

function find_free_player_slot() {
    for (let i = 0; i < MAX_PLAYERS; i++) {
        if (g_players[i].type === PlayerType.NONE) return i;
    }
    return -1;
}

function find_player(obj) {
    if (!obj) return null;
    const id = obj.fields[0].i;
    return find_player_by_id(id);
}

function find_player_by_id(id) {
    for (let i = 0; i < MAX_PLAYERS; i++) {
        const p = g_players[i];
        if (p.id === id && p.type !== PlayerType.NONE) return p;
    }
    return null;
}

// ============================================================
// Manager native methods
// ============================================================

export function native_manager_createPlayer_locator(jvm, thread, args, arg_count) {
    const locator_str = args[0].ref;
    ensure_audio_init();

    const locator = locator_str ? string_utf8(jvm, locator_str) : null;

    const slot = find_free_player_slot();
    if (slot < 0) return { ref: null };

    const player = g_players[slot];
    _reset_player(player);
    player.id     = g_next_player_id++;
    player.volume = 1.0;
    player.state  = PLAYER_UNREALIZED;

    if (locator === 'device://tone') {
        player.type     = PlayerType.TONE_DEVICE;
        player.tone_seq = {
            sequence: new Uint8Array(0), length: 0, position: 0,
            tempo: 120, resolution: 64, playing: false, loop: false,
            loop_count: 0, loops_remaining: 0, start_time_ms: 0,
            current_note: -1, current_duration: 0, current_volume: 100,
        };
        player.content_type = 'audio/x-tone-seq';
    } else if (locator === 'device://midi') {
        player.type         = PlayerType.MIDI;
        player.content_type = 'audio/midi';
    } else {
        _reset_player(player);
        return { ref: null };
    }

    const player_class = jvm_load_class(jvm, 'javax/microedition/media/PlayerImpl');
    if (!player_class) return { ref: null };

    const player_obj = jvm_new_object(jvm, player_class);
    if (!player_obj) return { ref: null };

    player_obj.fields[0] = { i: player.id };
    if (player_class.fields_count >= 2) {
        player_obj.fields[1] = { i: player.state };
    }

    return { ref: player_obj };
}

export function native_manager_createPlayer_stream(jvm, thread, args, arg_count) {
    const stream   = args[0].ref;
    ensure_audio_init();

    if (!stream) return { ref: null };

    const stream_class = stream.header.clazz;
    let buf = null, pos = 0, count = 0;

    for (let i = 0; i < stream_class.fields_count; i++) {
        const fname = stream_class.fields[i].name;
        if (fname === 'buf')   buf   = stream.fields[i].ref;
        else if (fname === 'pos')   pos   = stream.fields[i].i;
        else if (fname === 'count') count = stream.fields[i].i;
    }

    if (!buf || count <= 0) return { ref: null };

    const data_len = count - pos;
    if (data_len <= 0) return { ref: null };

    const raw = array_data(buf);
    const data = raw.slice(pos, pos + data_len);

    const slot = find_free_player_slot();
    if (slot < 0) return { ref: null };

    const player = g_players[slot];
    _reset_player(player);
    player.id     = g_next_player_id++;
    player.data   = data;
    player.size   = data_len;
    player.volume = 1.0;
    player.state  = PLAYER_UNREALIZED;

    // Auto-detect format by magic bytes
    if (data_len >= 4 && data[0] === 0x4D && data[1] === 0x54 && data[2] === 0x68 && data[3] === 0x64) {
        player.type         = PlayerType.MIDI;
        player.midi         = midi_load(player.data, player.size);
        player.content_type = 'audio/midi';
    } else if (data_len >= 4 && data[0] === 0x52 && data[1] === 0x49 && data[2] === 0x46 && data[3] === 0x46) {
        player.type         = PlayerType.WAV;
        player.wav          = wav_parse(player.data);
        player.content_type = 'audio/x-wav';
    } else {
        player.type         = PlayerType.TONE;
        player.tone_seq     = tone_sequence_parse(player.data, player.size);
        player.content_type = 'audio/x-tone-seq';
    }

    if (!player.midi && !player.wav && !player.tone_seq) {
        _reset_player(player);
        return { ref: null };
    }

    const player_class = jvm_load_class(jvm, 'javax/microedition/media/PlayerImpl');
    if (!player_class) {
        _reset_player(player);
        return { ref: null };
    }

    const player_obj = jvm_new_object(jvm, player_class);
    if (!player_obj) {
        _reset_player(player);
        return { ref: null };
    }

    player_obj.fields[0] = { i: player.id };
    if (player_class.fields_count >= 2) {
        player_obj.fields[1] = { i: player.state };
    }

    return { ref: player_obj };
}

export function native_manager_playTone(jvm, thread, args, arg_count) {
    const note     = args[0].i;
    let   duration = args[1].i;
    let   volume   = args[2].i;

    ensure_audio_init();

    if (note < TONE_MIN_NOTE || note > TONE_MAX_NOTE)      return { raw: 0n };
    if (duration <= 0 || duration > TONE_MAX_DURATION)     return { raw: 0n };

    volume = volume < TONE_MIN_VOLUME ? TONE_MIN_VOLUME : (volume > TONE_MAX_VOLUME ? TONE_MAX_VOLUME : volume);

    const velocity = Math.trunc((volume * 127) / 100);

    const slot = find_free_player_slot();
    if (slot >= 0) {
        const player = g_players[slot];
        _reset_player(player);
        player.id      = g_next_player_id++;
        player.type    = PlayerType.TONE_DEVICE;
        player.volume  = volume / 100.0;
        player.playing = true;
        player.state   = PLAYER_STARTED;
        player.tone_seq = {
            sequence: new Uint8Array(0), length: 0, position: 0,
            tempo: 120, resolution: 64, playing: true, loop: false,
            loop_count: 0, loops_remaining: 0,
            start_time_ms: get_time_ms(),
            current_note: note,
            current_duration: duration,
            current_volume: volume,
        };
        midi_note_on(0, note, velocity);
    }

    return { raw: 0n };
}

export function native_manager_getSupportedContentTypes(jvm, thread, args, arg_count) {
    const types = ['audio/midi', 'audio/x-wav', 'audio/x-tone-seq'];
    const array = jvm_new_array(jvm, 'L', types.length, null);
    if (!array) return { ref: null };

    const elems = array_data(array);
    for (let i = 0; i < types.length; i++) {
        elems[i] = jvm_new_string(jvm, types[i]);
    }

    return { ref: array };
}

export function native_manager_getSupportedProtocols(jvm, thread, args, arg_count) {
    const protocols = ['file', 'http', 'device'];
    const array = jvm_new_array(jvm, 'L', 3, null);
    if (!array) return { ref: null };

    const elems = array_data(array);
    for (let i = 0; i < 3; i++) {
        elems[i] = jvm_new_string(jvm, protocols[i]);
    }

    return { ref: array };
}

// ============================================================
// Player native methods
// ============================================================

export function native_player_realize(jvm, thread, args, arg_count) {
    const player = find_player(args[0].ref);
    if (!player) return { raw: 0n };
    if (player.state < PLAYER_REALIZED) player.state = PLAYER_REALIZED;
    return { raw: 0n };
}

export function native_player_prefetch(jvm, thread, args, arg_count) {
    const player = find_player(args[0].ref);
    if (!player) return { raw: 0n };
    if (player.state < PLAYER_REALIZED)   player.state = PLAYER_REALIZED;
    if (player.state < PLAYER_PREFETCHED) player.state = PLAYER_PREFETCHED;
    return { raw: 0n };
}

export function native_player_start(jvm, thread, args, arg_count) {
    const player = find_player(args[0].ref);
    if (!player) return { raw: 0n };

    if (player.state < PLAYER_PREFETCHED) player.state = PLAYER_PREFETCHED;
    player.playing = true;
    player.state   = PLAYER_STARTED;

    if (player.type === PlayerType.WAV && player.wav) {
        player.wav_position       = 0;
        player.wav_samples_played = 0;
    } else if (player.type === PlayerType.MIDI && player.midi) {
        midi_set_loop(player.midi, player.looping);
        midi_play(player.midi);
    } else if ((player.type === PlayerType.TONE || player.type === PlayerType.TONE_DEVICE) && player.tone_seq) {
        player.tone_seq.playing      = true;
        player.tone_seq.start_time_ms = get_time_ms();
        player.tone_seq.position     = 0;
    }

    return { raw: 0n };
}

export function native_player_stop(jvm, thread, args, arg_count) {
    const player = find_player(args[0].ref);
    if (!player) return { raw: 0n };

    if (player.type === PlayerType.MIDI && player.midi) {
        midi_stop(player.midi);
    } else if ((player.type === PlayerType.TONE || player.type === PlayerType.TONE_DEVICE) && player.tone_seq) {
        if (player.tone_seq.current_note >= 0) {
            midi_note_off(0, player.tone_seq.current_note);
        }
    }

    player.playing = false;
    if (player.state === PLAYER_STARTED) player.state = PLAYER_PREFETCHED;

    return { raw: 0n };
}

export function native_player_deallocate(jvm, thread, args, arg_count) {
    const player = find_player(args[0].ref);
    if (!player) return { raw: 0n };

    player.playing = false;
    if (player.midi)     { midi_stop(player.midi); midi_free(player.midi); player.midi = null; }
    if (player.wav)      player.wav      = null;
    if (player.tone_seq) player.tone_seq = null;
    player.state = PLAYER_UNREALIZED;

    return { raw: 0n };
}

export function native_player_close(jvm, thread, args, arg_count) {
    const player = find_player(args[0].ref);
    if (!player) return { raw: 0n };

    player.playing = false;
    if (player.midi) { midi_stop(player.midi); midi_free(player.midi); }
    g_midi_last_time.delete(player.id);
    _reset_player(player);

    return { raw: 0n };
}

export function native_player_setLoopCount(jvm, thread, args, arg_count) {
    const player = find_player(args[0].ref);
    if (!player) return { raw: 0n };

    const count = args[1].i;
    player.loop_count      = count;
    player.loops_remaining = count;
    player.looping         = (count !== 1);

    return { raw: 0n };
}

export function native_player_getDuration(jvm, thread, args, arg_count) {
    const player = find_player(args[0].ref);
    if (!player) return { j: TIME_UNKNOWN };

    let duration = TIME_UNKNOWN;
    if (player.wav) {
        duration = BigInt(player.wav.duration_ms) * 1000n;
    } else if (player.midi) {
        duration = BigInt(midi_get_duration_ms(player.midi)) * 1000n;
    }

    return { j: duration };
}

export function native_player_getMediaTime(jvm, thread, args, arg_count) {
    const player = find_player(args[0].ref);
    if (!player) return { j: TIME_UNKNOWN };

    let time = 0n;
    if (player.wav && player.wav.sample_rate > 0) {
        time = BigInt(Math.trunc(player.wav_samples_played * 1000000 / player.wav.sample_rate));
    } else if (player.midi) {
        time = BigInt(midi_get_position_ms(player.midi)) * 1000n;
    }

    return { j: time };
}

export function native_player_setMediaTime(jvm, thread, args, arg_count) {
    const player = find_player(args[0].ref);
    if (!player) return { j: -1n };

    const time_us = args[1].j;  // BigInt
    let result = time_us;

    if (player.wav && player.wav.byte_rate > 0) {
        const secs = Number(time_us) / 1000000.0;
        let byte_pos = Math.trunc(secs * player.wav.byte_rate);
        if (byte_pos > player.wav.data_size) byte_pos = player.wav.data_size;
        player.wav_position      = byte_pos;
        player.wav_samples_played = Math.trunc(Number(time_us) * player.wav.sample_rate / 1000000);
        result = time_us;
    } else if (player.midi) {
        midi_set_position_ms(player.midi, Math.trunc(Number(time_us) / 1000));
    }

    return { j: result };
}

export function native_player_getState(jvm, thread, args, arg_count) {
    const player = find_player(args[0].ref);
    if (!player) return { i: PLAYER_CLOSED };

    if (player.state === PLAYER_STARTED && !player.playing) {
        player.state = PLAYER_PREFETCHED;
    }

    return { i: player.state };
}

export function native_player_getContentType(jvm, thread, args, arg_count) {
    const player = find_player(args[0].ref);
    if (!player || !player.content_type) return { ref: null };

    return { ref: jvm_new_string(jvm, player.content_type) };
}

export function native_player_getLocator(jvm, thread, args, arg_count) {
    return { ref: null };
}

export function native_player_getControl(jvm, thread, args, arg_count) {
    const player   = find_player(args[0].ref);
    const type_str = args[1].ref;
    if (!player || !type_str) return { ref: null };

    const type = string_utf8(jvm, type_str);
    if (!type) return { ref: null };

    let class_name = null;
    if (type.includes('VolumeControl')) {
        class_name = 'javax/microedition/media/control/VolumeControlImpl';
    } else if (type.includes('MIDIControl') && player.type === PlayerType.MIDI) {
        class_name = 'javax/microedition/media/control/MIDIControlImpl';
    } else if (type.includes('ToneControl') &&
               (player.type === PlayerType.TONE || player.type === PlayerType.TONE_DEVICE)) {
        class_name = 'javax/microedition/media/control/ToneControlImpl';
    }

    if (!class_name) return { ref: null };

    const ctrl_class = jvm_load_class(jvm, class_name);
    if (!ctrl_class) return { ref: null };

    const ctrl_obj = jvm_new_object(jvm, ctrl_class);
    if (ctrl_obj && ctrl_class.fields_count >= 1) {
        ctrl_obj.fields[0] = { i: player.id };
    }

    return { ref: ctrl_obj };
}

export function native_player_getControls(jvm, thread, args, arg_count) {
    const player = find_player(args[0].ref);
    if (!player) return { ref: null };

    let num_controls = 1;
    if (player.type === PlayerType.MIDI ||
        player.type === PlayerType.TONE ||
        player.type === PlayerType.TONE_DEVICE) {
        num_controls = 2;
    }

    const array = jvm_new_array(jvm, 'L', num_controls, null);
    if (!array) return { ref: null };

    const elems = array_data(array);

    const vol_class = jvm_load_class(jvm, 'javax/microedition/media/control/VolumeControlImpl');
    if (vol_class) {
        const vol_obj = jvm_new_object(jvm, vol_class);
        if (vol_obj && vol_class.fields_count >= 1) {
            vol_obj.fields[0] = { i: player.id };
        }
        elems[0] = vol_obj;
    }

    return { ref: array };
}

export function native_player_addPlayerListener(jvm, thread, args, arg_count) {
    return { raw: 0n };
}

export function native_player_removePlayerListener(jvm, thread, args, arg_count) {
    return { raw: 0n };
}

// ============================================================
// VolumeControl native methods
// ============================================================

export function native_volumecontrol_setLevel(jvm, thread, args, arg_count) {
    const obj       = args[0].ref;
    let   level     = args[1].i;
    if (!obj) return { raw: 0n };

    const player_id = obj.fields[0].i;
    if (level < 0)   level = 0;
    if (level > 100) level = 100;

    const player = find_player_by_id(player_id);
    if (player) player.volume = level / 100.0;

    return { raw: 0n };
}

export function native_volumecontrol_getLevel(jvm, thread, args, arg_count) {
    const obj = args[0].ref;
    if (!obj) return { i: 100 };

    const player = find_player_by_id(obj.fields[0].i);
    return { i: player ? Math.trunc(player.volume * 100) : 100 };
}

export function native_volumecontrol_setMute(jvm, thread, args, arg_count) {
    const obj  = args[0].ref;
    if (!obj) return { raw: 0n };

    const mute = args[1].i !== 0;
    const player = find_player_by_id(obj.fields[0].i);
    if (player) player.muted = mute;

    return { raw: 0n };
}

export function native_volumecontrol_isMuted(jvm, thread, args, arg_count) {
    const obj = args[0].ref;
    if (!obj) return { i: 0 };

    const player = find_player_by_id(obj.fields[0].i);
    return { i: (player && player.muted) ? 1 : 0 };
}

// ============================================================
// MIDIControl native methods
// ============================================================

export function native_midicontrol_shortMidiEvent(jvm, thread, args, arg_count) {
    const obj     = args[0].ref;
    if (!obj) return { raw: 0n };

    const player_id = obj.fields[0].i;
    const command   = args[1].i;
    const data1     = args[2].i;
    const data2     = args[3].i;

    const player = find_player_by_id(player_id);
    if (player && (player.type === PlayerType.MIDI || player.type === PlayerType.TONE_DEVICE)) {
        const status = command & 0xFF;

        if ((status & 0xF0) === 0x90) {
            midi_note_on(status & 0x0F, data1, data2);
        } else if ((status & 0xF0) === 0x80) {
            midi_note_off(status & 0x0F, data1);
        } else if ((status & 0xF0) === 0xB0) {
            midi_control_change(status & 0x0F, data1, data2);
        } else if ((status & 0xF0) === 0xC0) {
            midi_program_change(status & 0x0F, data1);
        }
    }

    return { raw: 0n };
}

export function native_midicontrol_longMidiEvent(jvm, thread, args, arg_count) {
    // SysEx / long MIDI events — not implemented in this synthesizer
    return { i: 0 };
}

export function native_midicontrol_setChannelVolume(jvm, thread, args, arg_count) {
    const channel = args[1].i;
    const volume  = args[2].i;
    midi_control_change(channel, 7, volume);
    return { raw: 0n };
}

export function native_midicontrol_getChannelVolume(jvm, thread, args, arg_count) {
    const channel = args[1].i;
    return { i: midi_get_channel_volume(channel) };
}

export function native_midicontrol_setProgram(jvm, thread, args, arg_count) {
    const channel = args[1].i;
    const program = args[3].i;
    midi_program_change(channel, program);
    return { raw: 0n };
}

export function native_midicontrol_getProgram(jvm, thread, args, arg_count) {
    const channel = args[1].i;
    return { i: midi_get_program(channel) };
}

export function native_midicontrol_isBankQuerySupported(jvm, thread, args, arg_count) {
    return { i: 0 };
}

// ============================================================
// ToneControl native methods
// ============================================================

export function native_tonecontrol_setSequence(jvm, thread, args, arg_count) {
    const obj = args[0].ref;
    if (!obj) return { raw: 0n };

    const player_id = obj.fields[0].i;
    const seq       = args[1].ref;

    const player = find_player_by_id(player_id);
    if (!player || !seq) return { raw: 0n };

    const data   = array_data(seq);
    const length = seq.length;

    player.tone_seq = tone_sequence_parse(data, length);

    return { raw: 0n };
}

// ============================================================
// VideoControl native methods (all stubs)
// ============================================================

export function native_videocontrol_initDisplayMode(jvm, thread, args, arg_count)      { return { ref: null }; }
export function native_videocontrol_setDisplayLocation(jvm, thread, args, arg_count)   { return { raw: 0n }; }
export function native_videocontrol_getDisplayX(jvm, thread, args, arg_count)          { return { i: 0 }; }
export function native_videocontrol_getDisplayY(jvm, thread, args, arg_count)          { return { i: 0 }; }
export function native_videocontrol_setVisible(jvm, thread, args, arg_count)           { return { raw: 0n }; }
export function native_videocontrol_getDisplayWidth(jvm, thread, args, arg_count)      { return { i: 176 }; }
export function native_videocontrol_getDisplayHeight(jvm, thread, args, arg_count)     { return { i: 144 }; }
export function native_videocontrol_getSnapshot(jvm, thread, args, arg_count)          { return { ref: null }; }
export function native_videocontrol_setDisplaySize(jvm, thread, args, arg_count)       { return { raw: 0n }; }
export function native_videocontrol_setDisplayFullScreen(jvm, thread, args, arg_count) { return { raw: 0n }; }
export function native_videocontrol_getSourceWidth(jvm, thread, args, arg_count)       { return { i: 176 }; }
export function native_videocontrol_getSourceHeight(jvm, thread, args, arg_count)      { return { i: 144 }; }

// ============================================================
// TimeBase
// ============================================================

export function native_timebase_getTime(jvm, thread, args, arg_count) {
    return { j: get_time_us() - g_start_time_us };
}

// ============================================================
// Registration entry-point  (mirrors init_javax_microedition_media)
// ============================================================

export function init_javax_microedition_media(jvm) {
    const methods = [
        // Manager
        { class_name: 'javax/microedition/media/Manager', method_name: 'createPlayer',
          descriptor: '(Ljava/lang/String;)Ljavax/microedition/media/Player;',
          handler: native_manager_createPlayer_locator },
        { class_name: 'javax/microedition/media/Manager', method_name: 'createPlayer',
          descriptor: '(Ljava/io/InputStream;Ljava/lang/String;)Ljavax/microedition/media/Player;',
          handler: native_manager_createPlayer_stream },
        { class_name: 'javax/microedition/media/Manager', method_name: 'playTone',
          descriptor: '(III)V', handler: native_manager_playTone },
        { class_name: 'javax/microedition/media/Manager', method_name: 'getSupportedContentTypes',
          descriptor: '(Ljava/lang/String;)[Ljava/lang/String;', handler: native_manager_getSupportedContentTypes },
        { class_name: 'javax/microedition/media/Manager', method_name: 'getSupportedProtocols',
          descriptor: '(Ljava/lang/String;)[Ljava/lang/String;', handler: native_manager_getSupportedProtocols },

        // Player / PlayerImpl — same handlers for both class names
        ...['javax/microedition/media/Player', 'javax/microedition/media/PlayerImpl'].flatMap(cn => [
            { class_name: cn, method_name: 'realize',              descriptor: '()V',  handler: native_player_realize },
            { class_name: cn, method_name: 'prefetch',             descriptor: '()V',  handler: native_player_prefetch },
            { class_name: cn, method_name: 'start',                descriptor: '()V',  handler: native_player_start },
            { class_name: cn, method_name: 'stop',                 descriptor: '()V',  handler: native_player_stop },
            { class_name: cn, method_name: 'deallocate',           descriptor: '()V',  handler: native_player_deallocate },
            { class_name: cn, method_name: 'close',                descriptor: '()V',  handler: native_player_close },
            { class_name: cn, method_name: 'setLoopCount',         descriptor: '(I)V', handler: native_player_setLoopCount },
            { class_name: cn, method_name: 'getDuration',          descriptor: '()J',  handler: native_player_getDuration },
            { class_name: cn, method_name: 'getMediaTime',         descriptor: '()J',  handler: native_player_getMediaTime },
            { class_name: cn, method_name: 'setMediaTime',         descriptor: '(J)J', handler: native_player_setMediaTime },
            { class_name: cn, method_name: 'getState',             descriptor: '()I',  handler: native_player_getState },
            { class_name: cn, method_name: 'getContentType',       descriptor: '()Ljava/lang/String;', handler: native_player_getContentType },
            { class_name: cn, method_name: 'getLocator',           descriptor: '()Ljava/lang/String;', handler: native_player_getLocator },
            { class_name: cn, method_name: 'getControl',           descriptor: '(Ljava/lang/String;)Ljavax/microedition/media/Control;', handler: native_player_getControl },
            { class_name: cn, method_name: 'getControls',          descriptor: '()[Ljavax/microedition/media/Control;', handler: native_player_getControls },
            { class_name: cn, method_name: 'addPlayerListener',    descriptor: '(Ljavax/microedition/media/PlayerListener;)V', handler: native_player_addPlayerListener },
            { class_name: cn, method_name: 'removePlayerListener', descriptor: '(Ljavax/microedition/media/PlayerListener;)V', handler: native_player_removePlayerListener },
        ]),

        // VolumeControl / VolumeControlImpl
        ...['javax/microedition/media/control/VolumeControl', 'javax/microedition/media/control/VolumeControlImpl'].flatMap(cn => [
            { class_name: cn, method_name: 'setLevel', descriptor: '(I)V', handler: native_volumecontrol_setLevel },
            { class_name: cn, method_name: 'getLevel', descriptor: '()I',  handler: native_volumecontrol_getLevel },
            { class_name: cn, method_name: 'setMute',  descriptor: '(Z)V', handler: native_volumecontrol_setMute },
            { class_name: cn, method_name: 'isMuted',  descriptor: '()Z',  handler: native_volumecontrol_isMuted },
        ]),

        // MIDIControl / MIDIControlImpl
        ...['javax/microedition/media/control/MIDIControl', 'javax/microedition/media/control/MIDIControlImpl'].flatMap(cn => [
            { class_name: cn, method_name: 'shortMidiEvent',       descriptor: '(III)V',   handler: native_midicontrol_shortMidiEvent },
            { class_name: cn, method_name: 'longMidiEvent',        descriptor: '([BII)I',  handler: native_midicontrol_longMidiEvent },
            { class_name: cn, method_name: 'setChannelVolume',     descriptor: '(II)V',    handler: native_midicontrol_setChannelVolume },
            { class_name: cn, method_name: 'getChannelVolume',     descriptor: '(I)I',     handler: native_midicontrol_getChannelVolume },
            { class_name: cn, method_name: 'setProgram',           descriptor: '(III)V',   handler: native_midicontrol_setProgram },
            { class_name: cn, method_name: 'getProgram',           descriptor: '(I)I',     handler: native_midicontrol_getProgram },
            { class_name: cn, method_name: 'isBankQuerySupported', descriptor: '()Z',      handler: native_midicontrol_isBankQuerySupported },
        ]),

        // ToneControl / ToneControlImpl
        ...['javax/microedition/media/control/ToneControl', 'javax/microedition/media/control/ToneControlImpl'].map(cn => ({
            class_name: cn, method_name: 'setSequence', descriptor: '([B)V', handler: native_tonecontrol_setSequence,
        })),

        // VideoControl / VideoControlImpl
        ...['javax/microedition/media/control/VideoControl', 'javax/microedition/media/control/VideoControlImpl'].flatMap(cn => [
            { class_name: cn, method_name: 'initDisplayMode',       descriptor: '(ILjava/lang/Object;)Ljava/lang/Object;', handler: native_videocontrol_initDisplayMode },
            { class_name: cn, method_name: 'setDisplayLocation',    descriptor: '(II)V',                                  handler: native_videocontrol_setDisplayLocation },
            { class_name: cn, method_name: 'getDisplayX',           descriptor: '()I',                                    handler: native_videocontrol_getDisplayX },
            { class_name: cn, method_name: 'getDisplayY',           descriptor: '()I',                                    handler: native_videocontrol_getDisplayY },
            { class_name: cn, method_name: 'setVisible',            descriptor: '(Z)V',                                   handler: native_videocontrol_setVisible },
            { class_name: cn, method_name: 'getDisplayWidth',       descriptor: '()I',                                    handler: native_videocontrol_getDisplayWidth },
            { class_name: cn, method_name: 'getDisplayHeight',      descriptor: '()I',                                    handler: native_videocontrol_getDisplayHeight },
            { class_name: cn, method_name: 'getSnapshot',           descriptor: '(Ljava/lang/String;)[B',                 handler: native_videocontrol_getSnapshot },
            { class_name: cn, method_name: 'setDisplaySize',        descriptor: '(II)V',                                  handler: native_videocontrol_setDisplaySize },
            { class_name: cn, method_name: 'setDisplayFullScreen',  descriptor: '(Z)V',                                   handler: native_videocontrol_setDisplayFullScreen },
            { class_name: cn, method_name: 'getSourceWidth',        descriptor: '()I',                                    handler: native_videocontrol_getSourceWidth },
            { class_name: cn, method_name: 'getSourceHeight',       descriptor: '()I',                                    handler: native_videocontrol_getSourceHeight },
        ]),

        // TimeBase
        { class_name: 'javax/microedition/media/TimeBase', method_name: 'getTime',
          descriptor: '()J', handler: native_timebase_getTime },
    ];

    native_register_methods(jvm, methods, methods.length);
}

// ============================================================
// Stubs for JVM host functions referenced above.
// In the real JS host these will be injected from the JVM module;
// these stubs prevent import-time errors when the module is loaded standalone.
// ============================================================

function string_utf8(jvm, str) {
    if (!str) return null;
    if (str.utf8) return str.utf8;
    if (str.chars) return String.fromCharCode(...str.chars);
    return null;
}

function array_data(arr) {
    if (!arr) return null;
    if (arr._data) return arr._data;
    return arr;
}

function jvm_load_class(jvm, name) {
    if (!jvm) return null;
    if (typeof jvm.loadClass === 'function') return jvm.loadClass(name);
    return null;
}

function jvm_new_object(jvm, clazz) {
    if (!jvm || !clazz) return null;
    if (typeof jvm.newObject === 'function') return jvm.newObject(clazz);
    return { header: { clazz }, fields: new Array(64).fill(null).map(() => ({ i: 0, ref: null, j: 0n })) };
}

function jvm_new_array(jvm, type, length, element_class) {
    if (!jvm) return null;
    if (typeof jvm.newArray === 'function') return jvm.newArray(type, length, element_class);
    const arr = { length, element_type: type, _data: new Array(length).fill(null) };
    return arr;
}

function jvm_new_string(jvm, utf8) {
    if (!jvm) return null;
    if (typeof jvm.newString === 'function') return jvm.newString(utf8);
    return { utf8, length: utf8 ? utf8.length : 0 };
}

function native_register_methods(jvm, methods, count) {
    if (!jvm) return;
    if (typeof jvm.registerNativeMethods === 'function') {
        jvm.registerNativeMethods(methods);
    }
}
