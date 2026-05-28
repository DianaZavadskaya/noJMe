import { readFileSync } from 'node:fs';
import { inflateRawSync } from 'node:zlib';

import {
    jvm_create,
    jvm_destroy,
    jvm_init,
    jvm_set_jar_data,
    jvm_load_class,
    JNI_OK,
} from '../jvm/jvm.mjs';

import {
    jvm_run_midlet,
    execute_frame,
} from '../jvm/execute.mjs';

import {
    sdl_init,
    sdl_set_error_info,
    sdl_set_error_extra,
    sdl_has_error,
    sdl_draw_error_screen,
    sdl_get_global_context,
    libretro_set_sample_rate,
    libretro_set_fps,
    libretro_process_audio,
    libretro_begin_frame,
    libretro_end_frame,
    libretro_get_display_buffer,
} from './sdl_backend_stubs.mjs';

import {
    midp_set_screen_dimensions,
} from '../midp/graphics.mjs';

import {
    midp_rms_set_save_path,
    midp_rms_save_all,
} from '../midp/rms.mjs';

import {
    midp_check_alert_timeout,
} from '../midp/form.mjs';

export const J2ME_BUILD_ID = '2024.01.30.V15-REAL-TIME';

export const RETRO_API_VERSION = 1;

export const RETRO_PIXEL_FORMAT_0RGB1555 = 0;
export const RETRO_PIXEL_FORMAT_XRGB8888 = 1;
export const RETRO_PIXEL_FORMAT_RGB565   = 2;

export const RETRO_ENVIRONMENT_GET_LOG_INTERFACE     = 27;
export const RETRO_ENVIRONMENT_GET_VARIABLE          = 15;
export const RETRO_ENVIRONMENT_SET_VARIABLES         = 16;
export const RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE   = 17;
export const RETRO_ENVIRONMENT_SET_PIXEL_FORMAT      = 10;
export const RETRO_ENVIRONMENT_SET_CONTROLLER_INFO   = 35;
export const RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS = 11;
export const RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME   = 18;
export const RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY    = 31;

export const RETRO_LOG_DEBUG = 0;
export const RETRO_LOG_INFO  = 1;
export const RETRO_LOG_WARN  = 2;
export const RETRO_LOG_ERROR = 3;

export const RETRO_DEVICE_JOYPAD   = 1;
export const RETRO_DEVICE_KEYBOARD = 3;

export const RETRO_DEVICE_ID_JOYPAD_B      = 0;
export const RETRO_DEVICE_ID_JOYPAD_Y      = 1;
export const RETRO_DEVICE_ID_JOYPAD_SELECT = 2;
export const RETRO_DEVICE_ID_JOYPAD_START  = 3;
export const RETRO_DEVICE_ID_JOYPAD_UP     = 4;
export const RETRO_DEVICE_ID_JOYPAD_DOWN   = 5;
export const RETRO_DEVICE_ID_JOYPAD_LEFT   = 6;
export const RETRO_DEVICE_ID_JOYPAD_RIGHT  = 7;
export const RETRO_DEVICE_ID_JOYPAD_A      = 8;
export const RETRO_DEVICE_ID_JOYPAD_X      = 9;
export const RETRO_DEVICE_ID_JOYPAD_L      = 10;
export const RETRO_DEVICE_ID_JOYPAD_R      = 11;
export const RETRO_DEVICE_ID_JOYPAD_L2     = 12;
export const RETRO_DEVICE_ID_JOYPAD_R2     = 13;

export const RETROK_0        = 48;
export const RETROK_6        = 54;
export const RETROK_7        = 55;
export const RETROK_8        = 56;
export const RETROK_9        = 57;
export const RETROK_ASTERISK = 42;
export const RETROK_HASH     = 35;

export const RETRO_REGION_NTSC = 0;
export const RETRO_REGION_PAL  = 1;

const KEY_REPEAT_INITIAL_DELAY_MS = 400;
const KEY_REPEAT_INTERVAL_MS      = 80;
const KEY_REPEAT_MAX_BIT          = 17;

const KEY_MAP = [
    { bit:  1, keycode:  -1, gameAction: 1 },
    { bit:  2, keycode:  -3, gameAction: 2 },
    { bit:  5, keycode:  -4, gameAction: 5 },
    { bit:  6, keycode:  -2, gameAction: 6 },
    { bit:  8, keycode:  -5, gameAction: 8 },
    { bit:  9, keycode:  -6, gameAction: 0 },
    { bit: 10, keycode:  -7, gameAction: 0 },
    { bit: 11, keycode:  -8, gameAction: 0 },
    { bit: 12, keycode:  -9, gameAction: 0 },
    { bit: 13, keycode:  49, gameAction: 0 },
    { bit: 14, keycode:  50, gameAction: 0 },
    { bit: 15, keycode:  51, gameAction: 0 },
    { bit: 16, keycode:  52, gameAction: 0 },
    { bit: 17, keycode:  53, gameAction: 0 },
];

const KB_KEY_MAP = [
    { bit: 0, retro_key: RETROK_0,        keycode: 48 },
    { bit: 1, retro_key: RETROK_6,        keycode: 54 },
    { bit: 2, retro_key: RETROK_7,        keycode: 55 },
    { bit: 3, retro_key: RETROK_8,        keycode: 56 },
    { bit: 4, retro_key: RETROK_9,        keycode: 57 },
    { bit: 5, retro_key: RETROK_ASTERISK, keycode: 42 },
    { bit: 6, retro_key: RETROK_HASH,     keycode: 35 },
];

const CORE_VARIABLES = [
    { key: 'j2me_resolution', value: 'Screen Resolution; 240x320|240x136|480x272|320x480|360x640|480x800|176x208|128x128' },
    { key: 'j2me_fps',        value: 'Frame Rate; 30|60|15|20' },
    { key: 'j2me_audio_rate', value: 'Audio Sample Rate; 22050|44100|11025' },
    { key: 'j2me_scaling',    value: 'Screen Scaling; Aspect|Integer|Stretch' },
];

const INPUT_DESCRIPTORS = [
    { port: 0, device: RETRO_DEVICE_JOYPAD, index: 0, id: RETRO_DEVICE_ID_JOYPAD_UP,     description: 'Up' },
    { port: 0, device: RETRO_DEVICE_JOYPAD, index: 0, id: RETRO_DEVICE_ID_JOYPAD_DOWN,   description: 'Down' },
    { port: 0, device: RETRO_DEVICE_JOYPAD, index: 0, id: RETRO_DEVICE_ID_JOYPAD_LEFT,   description: 'Left' },
    { port: 0, device: RETRO_DEVICE_JOYPAD, index: 0, id: RETRO_DEVICE_ID_JOYPAD_RIGHT,  description: 'Right' },
    { port: 0, device: RETRO_DEVICE_JOYPAD, index: 0, id: RETRO_DEVICE_ID_JOYPAD_A,      description: 'Fire' },
    { port: 0, device: RETRO_DEVICE_JOYPAD, index: 0, id: RETRO_DEVICE_ID_JOYPAD_B,      description: 'Game C' },
    { port: 0, device: RETRO_DEVICE_JOYPAD, index: 0, id: RETRO_DEVICE_ID_JOYPAD_X,      description: 'Game D' },
    { port: 0, device: RETRO_DEVICE_JOYPAD, index: 0, id: RETRO_DEVICE_ID_JOYPAD_Y,      description: 'Key 1' },
    { port: 0, device: RETRO_DEVICE_JOYPAD, index: 0, id: RETRO_DEVICE_ID_JOYPAD_SELECT, description: 'Left Soft' },
    { port: 0, device: RETRO_DEVICE_JOYPAD, index: 0, id: RETRO_DEVICE_ID_JOYPAD_START,  description: 'Right Soft' },
    { port: 0, device: RETRO_DEVICE_JOYPAD, index: 0, id: RETRO_DEVICE_ID_JOYPAD_L,      description: 'Key 2' },
    { port: 0, device: RETRO_DEVICE_JOYPAD, index: 0, id: RETRO_DEVICE_ID_JOYPAD_R,      description: 'Key 3' },
    { port: 0, device: RETRO_DEVICE_JOYPAD, index: 0, id: RETRO_DEVICE_ID_JOYPAD_L2,     description: 'Key 4' },
    { port: 0, device: RETRO_DEVICE_JOYPAD, index: 0, id: RETRO_DEVICE_ID_JOYPAD_R2,     description: 'Key 5' },
];

let g_jvm             = null;
let g_jar_data        = null;
let g_jar_size        = 0;
let g_midlet_class    = '';
let g_game_name       = '';

let g_framebuffer      = null;
let g_rgb565_buffer    = null;
let g_screen_width     = 240;
let g_screen_height    = 320;
let g_screen_pitch     = 240 * 4;
let g_framebuffer_size = 0;
let g_use_rgb565       = true;

export let g_target_fps     = 30;
let g_audio_sample_rate     = 22050;

let g_key_states         = 0;
let g_prev_key_states    = 0;
let g_kb_key_states      = 0;
let g_kb_prev_key_states = 0;

const g_key_repeat = {
    press_time:       new Array(KEY_REPEAT_MAX_BIT + 1).fill(0),
    last_repeat_time: new Array(KEY_REPEAT_MAX_BIT + 1).fill(0),
    is_held:          new Array(KEY_REPEAT_MAX_BIT + 1).fill(false),
};

export let environ_cb     = null;
export let video_cb       = null;
export let audio_batch_cb = null;
export let input_poll_cb  = null;
export let input_state_cb = null;
let log_cb = null;

let last_frame_time   = 0;
let frame_accumulator = 0;

const g_stored_options = new Map();

let g_midp_callbacks = null;

export function set_midp_callbacks(cbs) {
    g_midp_callbacks = cbs;
}

function store_option_value(key, value) {
    if (key != null && value != null) g_stored_options.set(key, String(value));
}

function get_stored_option_value(key) {
    return g_stored_options.get(key) ?? null;
}

function millis() {
    return Date.now();
}

function log_message(level, msg) {
    if (log_cb) {
        log_cb(level, msg);
    } else {
        process.stderr.write(msg);
    }
}

export function libretro_get_framebuffer() {
    return g_framebuffer;
}

export function libretro_get_screen_width() {
    return g_screen_width;
}

export function libretro_get_screen_height() {
    return g_screen_height;
}

export function libretro_set_key_states(states) {
    g_key_states = states;
}

export function libretro_get_key_states() {
    return g_key_states;
}

function key_repeat_reset(bit) {
    if (bit >= 0 && bit <= KEY_REPEAT_MAX_BIT) {
        g_key_repeat.is_held[bit]          = false;
        g_key_repeat.press_time[bit]       = 0;
        g_key_repeat.last_repeat_time[bit] = 0;
    }
}

function read_u16(buf, offset) {
    return buf[offset] | (buf[offset + 1] << 8);
}

function read_u32(buf, offset) {
    return (buf[offset] | (buf[offset + 1] << 8) | (buf[offset + 2] << 16) | (buf[offset + 3] << 24)) >>> 0;
}

function jar_find_eocd(data, size) {
    if (size < 22) return 0;
    const maxComment  = 65535 + 22;
    const searchStart = size > maxComment ? size - maxComment : 0;
    for (let i = size - 22; i >= searchStart && i > 0; i--) {
        if (data[i] === 0x50 && data[i + 1] === 0x4B &&
            data[i + 2] === 0x05 && data[i + 3] === 0x06) {
            return i;
        }
    }
    return 0;
}

function jar_extract_file(jarData, jarSize, filename) {
    const eocdOffset = jar_find_eocd(jarData, jarSize);
    if (eocdOffset === 0) return null;

    const cdStart   = read_u32(jarData, eocdOffset + 16);
    const cdEntries = read_u16(jarData, eocdOffset + 10);

    let cdeOffset = cdStart;
    for (let i = 0; i < cdEntries && cdeOffset < eocdOffset; i++) {
        const sig = read_u32(jarData, cdeOffset);
        if (sig !== 0x02014B50) break;

        const compression    = read_u16(jarData, cdeOffset + 10);
        const compSize       = read_u32(jarData, cdeOffset + 20);
        const uncompSize     = read_u32(jarData, cdeOffset + 24);
        const filenameLen    = read_u16(jarData, cdeOffset + 28);
        const extraLen       = read_u16(jarData, cdeOffset + 30);
        const commentLen     = read_u16(jarData, cdeOffset + 32);
        const localHdrOffset = read_u32(jarData, cdeOffset + 42);

        const entryBytes = jarData.slice(cdeOffset + 46, cdeOffset + 46 + filenameLen);
        const entryName  = Buffer.from(entryBytes).toString('utf8');
        const isDir      = filenameLen > 0 && entryName[entryName.length - 1] === '/';

        if (!isDir && entryName === filename) {
            const localFnLen    = read_u16(jarData, localHdrOffset + 26);
            const localExtraLen = read_u16(jarData, localHdrOffset + 28);
            const dataOffset    = localHdrOffset + 30 + localFnLen + localExtraLen;

            if (compression === 0) {
                return jarData.slice(dataOffset, dataOffset + uncompSize);
            } else if (compression === 8) {
                if (compSize === 0 || uncompSize === 0) return null;
                const compressed = jarData.slice(dataOffset, dataOffset + compSize);
                try {
                    return new Uint8Array(inflateRawSync(compressed));
                } catch {
                    return null;
                }
            }
            return null;
        }

        cdeOffset += 46 + filenameLen + extraLen + commentLen;
    }
    return null;
}

function find_midlet_class_in_jar(jarData, jarSize) {
    let manifest = jar_extract_file(jarData, jarSize, 'META-INF/MANIFEST.MF');
    if (!manifest) {
        manifest = jar_extract_file(jarData, jarSize, 'META-INF/manifest.mf');
    }
    if (!manifest) return null;

    const text  = Buffer.from(manifest).toString('utf8');
    const lines = text.split(/\r\n|\r|\n/);

    for (const line of lines) {
        if (line.startsWith('MIDlet-1:') || line.startsWith('MIDlet-1 :')) {
            const colon = line.indexOf(':');
            if (colon < 0) continue;
            const rest  = line.slice(colon + 1);
            const parts = rest.split(',');
            if (parts.length >= 2) {
                const className = parts.length >= 3 ? parts[2].trim() : parts[1].trim();
                if (className) return className;
            }
        }
    }
    return null;
}

function convert_xrgb8888_to_rgb565(src, dst, width, height) {
    const size = width * height;
    for (let i = 0; i < size; i++) {
        const pixel = src[i] >>> 0;
        const r = (pixel >>> 16) & 0xFF;
        const g = (pixel >>> 8) & 0xFF;
        const b = pixel & 0xFF;
        dst[i] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    }
}

function build_exception_title(exception) {
    if (!exception || !exception.header || !exception.header.clazz) {
        return 'Unknown Exception';
    }
    const cls      = exception.header.clazz;
    const name     = cls.class_name || 'Unknown';
    const slash    = name.lastIndexOf('/');
    const shortName = slash >= 0 ? name.slice(slash + 1) : name;

    const chainParts = [];
    let super_ = cls.super_class;
    let depth  = 0;
    while (super_ && depth < 3) {
        const sn = super_.class_name || '';
        if (sn && sn !== 'java/lang/Object' && sn !== 'java/lang/Throwable' &&
            sn !== 'java/lang/Exception' && sn !== 'java/lang/Error' &&
            sn !== 'java/lang/Runtime') {
            const ss = sn.lastIndexOf('/');
            chainParts.push(ss >= 0 ? sn.slice(ss + 1) : sn);
            depth++;
            super_ = super_.super_class;
        } else {
            break;
        }
    }

    return chainParts.length > 0
        ? `${shortName} [${chainParts.join(' > ')}]`
        : shortName;
}

function build_stack_trace(thread) {
    if (!thread || !thread.current_frame) return '';
    let frame = thread.current_frame;
    let out   = '';
    let count = 0;
    while (frame && count < 25) {
        const clsName    = (frame.clazz && frame.clazz.class_name) ? frame.clazz.class_name : '?';
        const methodName = (frame.method && frame.method.name) ? frame.method.name : '?';
        const descriptor = (frame.method && frame.method.descriptor) ? frame.method.descriptor : '';
        if (clsName === '?' && methodName === '?') { frame = frame.prev; continue; }
        const slash    = clsName.lastIndexOf('/');
        const shortCls = slash >= 0 ? clsName.slice(slash + 1) : clsName;
        if (count === 0) {
            out += `>${shortCls}.${methodName}(${descriptor}) PC=${frame.throwing_pc || 0}\n`;
        } else {
            out += `  at ${shortCls}.${methodName}(${descriptor})\n`;
        }
        frame = frame.prev;
        count++;
    }
    return out;
}

function handle_exception(thread) {
    const exception = thread.pending_exception;
    if (!exception) {
        sdl_set_error_info('Execution Failed', 'MIDlet returned error code', null);
        return;
    }

    const excTitle = build_exception_title(exception);
    let message    = '';
    let stackTrace = '';
    let frames     = 0;

    if (thread.exception_stack_trace && thread.exception_stack_trace.length > 0) {
        stackTrace = thread.exception_stack_trace;
        for (const ch of stackTrace) { if (ch === '\n') frames++; }
    } else {
        stackTrace = build_stack_trace(thread);
        for (const ch of stackTrace) { if (ch === '\n') frames++; }
    }

    const threadName = thread.name || 'main';
    let extra;
    if (thread.exception_throw_info && thread.exception_throw_info.length > 0) {
        extra = `Thread: ${threadName} | ${frames} frames | thrown at ${thread.exception_throw_info}`;
    } else {
        extra = `Thread: ${threadName} | ${frames} frames`;
    }
    sdl_set_error_extra(extra);

    log_message(RETRO_LOG_ERROR,
        `[J2ME] Uncaught ${excTitle} in thread '${threadName}': ${message || '(no message)'}\n`);
    if (stackTrace) {
        log_message(RETRO_LOG_ERROR,
            `[J2ME] Stack trace (${frames} frames):\n${stackTrace}\n`);
    }
    sdl_set_error_info(excTitle, message || null, stackTrace || null);
}

function update_variables(_gamePath) {
    if (!environ_cb) return;

    const oldWidth  = g_screen_width;
    const oldHeight = g_screen_height;
    const oldFps    = g_target_fps;
    const oldAudio  = g_audio_sample_rate;

    let varObj = { key: 'j2me_resolution', value: null };
    environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, varObj);
    if (!varObj.value) varObj.value = get_stored_option_value('j2me_resolution');

    if (varObj.value) {
        const res = varObj.value;
        if      (res === '240x136') { g_screen_width = 240; g_screen_height = 136; }
        else if (res === '480x272') { g_screen_width = 480; g_screen_height = 272; }
        else if (res === '320x480') { g_screen_width = 320; g_screen_height = 480; }
        else if (res === '360x640') { g_screen_width = 360; g_screen_height = 640; }
        else if (res === '480x800') { g_screen_width = 480; g_screen_height = 800; }
        else if (res === '176x208') { g_screen_width = 176; g_screen_height = 208; }
        else if (res === '128x128') { g_screen_width = 128; g_screen_height = 128; }
        else                        { g_screen_width = 240; g_screen_height = 320; }
        store_option_value('j2me_resolution', res);
    }

    varObj = { key: 'j2me_fps', value: null };
    environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, varObj);
    if (!varObj.value) varObj.value = get_stored_option_value('j2me_fps');
    if (varObj.value) {
        let fps = parseInt(varObj.value, 10);
        if (fps < 15) fps = 15;
        if (fps > 60) fps = 60;
        g_target_fps = fps;
        store_option_value('j2me_fps', varObj.value);
    }

    varObj = { key: 'j2me_audio_rate', value: null };
    environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, varObj);
    if (!varObj.value) varObj.value = get_stored_option_value('j2me_audio_rate');
    if (varObj.value) {
        let rate = parseInt(varObj.value, 10);
        if (rate < 11025) rate = 11025;
        if (rate > 44100) rate = 44100;
        g_audio_sample_rate = rate;
        store_option_value('j2me_audio_rate', varObj.value);
    }

    g_screen_pitch = g_screen_width * 4;
    midp_set_screen_dimensions(g_screen_width, g_screen_height);

    if (g_jvm && g_jvm.running &&
        (g_screen_width !== oldWidth || g_screen_height !== oldHeight)) {
        g_screen_width  = oldWidth;
        g_screen_height = oldHeight;
        midp_set_screen_dimensions(oldWidth, oldHeight);
    }

    if (oldFps !== g_target_fps || oldAudio !== g_audio_sample_rate) {
        libretro_set_sample_rate(g_audio_sample_rate);
        libretro_set_fps(g_target_fps);
    }
}

function send_video_frame() {
    if (!video_cb) return;

    let displayBuffer = null;
    let fbWidth       = 0;
    let fbHeight      = 0;

    const dbResult = libretro_get_display_buffer();
    if (dbResult) {
        displayBuffer = dbResult.buffer;
        fbWidth       = dbResult.width;
        fbHeight      = dbResult.height;
    }

    if (displayBuffer && fbWidth > 0 && fbHeight > 0) {
        if (g_use_rgb565) {
            const needed = fbWidth * fbHeight;
            if (!g_rgb565_buffer || g_rgb565_buffer.length < needed) {
                g_rgb565_buffer = new Uint16Array(needed);
            }
            convert_xrgb8888_to_rgb565(displayBuffer, g_rgb565_buffer, fbWidth, fbHeight);
            video_cb(g_rgb565_buffer, fbWidth, fbHeight, fbWidth * 2);
        } else {
            video_cb(displayBuffer, fbWidth, fbHeight, fbWidth * 4);
        }
    } else if (g_framebuffer && g_screen_width > 0 && g_screen_height > 0) {
        if (g_use_rgb565) {
            convert_xrgb8888_to_rgb565(g_framebuffer, g_rgb565_buffer, g_screen_width, g_screen_height);
            video_cb(g_rgb565_buffer, g_screen_width, g_screen_height, g_screen_width * 2);
        } else {
            video_cb(g_framebuffer, g_screen_width, g_screen_height, g_screen_pitch);
        }
    } else {
        video_cb(null, 1, 1, 0);
    }
}

function init_emulator() {
    log_message(RETRO_LOG_INFO, '[J2ME] Initializing emulator...\n');

    g_jvm = jvm_create();
    if (!g_jvm) {
        log_message(RETRO_LOG_ERROR, '[J2ME] Failed to create JVM\n');
        return false;
    }

    g_jvm.config.heap_size    = 16 * 1024 * 1024;
    g_jvm.config.stack_size   = 64 * 1024;
    g_jvm.config.max_threads  = 8;
    g_jvm.config.verbose_class = false;

    if (jvm_init(g_jvm) !== JNI_OK) {
        log_message(RETRO_LOG_ERROR, '[J2ME] JVM init failed\n');
        jvm_destroy(g_jvm);
        g_jvm = null;
        return false;
    }

    if (sdl_init(g_jvm, g_screen_width, g_screen_height, 1, true) !== 0) {
        log_message(RETRO_LOG_ERROR, '[J2ME] sdl_init failed\n');
        jvm_destroy(g_jvm);
        g_jvm = null;
        return false;
    }

    const cb = g_midp_callbacks;
    if (cb && typeof cb.native_init === 'function') {
        if (cb.native_init(g_jvm) !== JNI_OK) {
            log_message(RETRO_LOG_ERROR, '[J2ME] native_init failed\n');
            jvm_destroy(g_jvm);
            g_jvm = null;
            return false;
        }
    }

    if (cb && typeof cb.midp_init === 'function') {
        if (cb.midp_init(g_jvm) !== JNI_OK) {
            log_message(RETRO_LOG_ERROR, '[J2ME] midp_init failed\n');
            jvm_destroy(g_jvm);
            g_jvm = null;
            return false;
        }
    }

    if (cb && typeof cb.opcodes_init === 'function') {
        cb.opcodes_init();
    }

    log_message(RETRO_LOG_INFO, '[J2ME] Emulator initialized\n');
    return true;
}

function run_midlet() {
    if (!g_jvm || !g_jar_data) return false;

    log_message(RETRO_LOG_INFO, `[J2ME] Starting MIDlet: ${g_midlet_class}\n`);

    jvm_set_jar_data(g_jvm, g_jar_data, g_jar_size, 'game.jar');

    const className = g_midlet_class.replace(/\./g, '/');

    const mainClass = jvm_load_class(g_jvm, className);
    if (!mainClass) {
        log_message(RETRO_LOG_ERROR, `[J2ME] Failed to load class ${className}\n`);
        sdl_set_error_info('Class Not Found', className,
            'The MIDlet main class could not be loaded from the JAR file.');
        return false;
    }

    const result = jvm_run_midlet(g_jvm, mainClass);
    if (result !== 0) {
        log_message(RETRO_LOG_ERROR, `[J2ME] MIDlet execution failed: ${result}\n`);
        const mainThread = g_jvm.main_thread;
        if (mainThread) {
            handle_exception(mainThread);
        } else {
            sdl_set_error_info('Execution Failed', 'MIDlet returned error code', null);
        }
        return false;
    }

    log_message(RETRO_LOG_INFO, '[J2ME] MIDlet started\n');
    return true;
}

export function retro_api_version() {
    return RETRO_API_VERSION;
}

export function retro_init() {
    g_framebuffer_size = g_screen_width * g_screen_height * 4;
    g_framebuffer      = new Uint32Array(g_screen_width * g_screen_height);
    g_screen_pitch     = g_screen_width * 4;

    g_rgb565_buffer = new Uint16Array(g_screen_width * g_screen_height);

    last_frame_time   = millis();
    frame_accumulator = 0;
}

export function retro_deinit() {
    if (g_jvm) {
        jvm_destroy(g_jvm);
        g_jvm = null;
    }
    g_jar_data        = null;
    g_jar_size        = 0;
    g_framebuffer     = null;
    g_rgb565_buffer   = null;
    g_framebuffer_size = 0;
}

export function retro_set_controller_port_device(_port, _device) {
}

export function retro_get_system_info(info) {
    info.library_name    = 'J2ME';
    info.library_version = '1.0';
    info.need_fullpath   = true;
    info.valid_extensions = 'jar';
}

export function retro_get_system_av_info(info) {
    info.timing = {
        fps:         g_target_fps,
        sample_rate: g_audio_sample_rate,
    };
    info.geometry = {
        base_width:   g_screen_width,
        base_height:  g_screen_height,
        max_width:    640,
        max_height:   800,
        aspect_ratio: g_screen_width / g_screen_height,
    };
}

export function retro_set_environment(cb) {
    environ_cb = cb;
    if (!cb) return;

    const logging = { log: null };
    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, logging) && logging.log) {
        log_cb = logging.log;
    }

    cb(RETRO_ENVIRONMENT_SET_VARIABLES, CORE_VARIABLES);

    const controllers = [{ types: [{ name: 'RetroPad', id: RETRO_DEVICE_JOYPAD }] }];
    cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, controllers);
    cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, INPUT_DESCRIPTORS);

    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, false);
}

export function retro_set_video_refresh(cb) {
    video_cb = cb;
}

export function retro_set_audio_sample(_cb) {
}

export function retro_set_audio_sample_batch(cb) {
    audio_batch_cb = cb;
}

export function retro_set_input_poll(cb) {
    input_poll_cb = cb;
}

export function retro_set_input_state(cb) {
    input_state_cb = cb;
}

export function retro_reset() {
    g_key_states      = 0;
    g_prev_key_states = 0;
    g_key_repeat.press_time.fill(0);
    g_key_repeat.last_repeat_time.fill(0);
    g_key_repeat.is_held.fill(false);
}

export function retro_load_game(info) {
    if (!info || !info.path) return false;

    update_variables(info.path);

    const requiredSize = g_screen_width * g_screen_height * 4;
    if (g_framebuffer_size !== requiredSize && g_framebuffer) {
        g_framebuffer_size = requiredSize;
        g_framebuffer      = new Uint32Array(g_screen_width * g_screen_height);
        g_screen_pitch     = g_screen_width * 4;
        g_rgb565_buffer    = new Uint16Array(g_screen_width * g_screen_height);
    }

    let jarBuf;
    try {
        jarBuf = readFileSync(info.path);
    } catch {
        return false;
    }
    if (!jarBuf) return false;

    g_jar_data = new Uint8Array(jarBuf.buffer, jarBuf.byteOffset, jarBuf.byteLength);
    g_jar_size = g_jar_data.length;

    const midletClass = find_midlet_class_in_jar(g_jar_data, g_jar_size);
    g_midlet_class    = midletClass || 'MIDlet';

    const pathStr = info.path;
    let baseName  = pathStr;
    const slashIdx = Math.max(pathStr.lastIndexOf('/'), pathStr.lastIndexOf('\\'));
    if (slashIdx >= 0) baseName = pathStr.slice(slashIdx + 1);
    const dotIdx = baseName.lastIndexOf('.');
    if (dotIdx >= 0) baseName = baseName.slice(0, dotIdx);
    g_game_name = baseName;

    if (environ_cb) {
        const saveDirObj = { value: null };
        if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, saveDirObj) && saveDirObj.value) {
            midp_rms_set_save_path(saveDirObj.value, g_game_name);
        }
    }

    const fmtObj = { value: RETRO_PIXEL_FORMAT_RGB565 };
    if (!environ_cb || !environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, fmtObj)) {
        fmtObj.value = RETRO_PIXEL_FORMAT_XRGB8888;
        if (!environ_cb || !environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, fmtObj)) {
            return false;
        }
        g_use_rgb565 = false;
    } else {
        g_use_rgb565 = true;
    }

    libretro_set_sample_rate(g_audio_sample_rate);
    libretro_set_fps(g_target_fps);

    if (!init_emulator()) return false;

    run_midlet();
    return true;
}

export function retro_load_game_special(_type, _info, _num) {
    return false;
}

export function retro_unload_game() {
    midp_rms_save_all();
}

function get_key_states_from_input() {
    let keys = 0;
    if (!input_state_cb) return keys;
    const s = input_state_cb;
    const JP = RETRO_DEVICE_JOYPAD;
    if (s(0, JP, 0, RETRO_DEVICE_ID_JOYPAD_UP))     keys |= (1 << 1);
    if (s(0, JP, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))   keys |= (1 << 2);
    if (s(0, JP, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))  keys |= (1 << 5);
    if (s(0, JP, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))   keys |= (1 << 6);
    if (s(0, JP, 0, RETRO_DEVICE_ID_JOYPAD_A))      keys |= (1 << 8);
    if (s(0, JP, 0, RETRO_DEVICE_ID_JOYPAD_SELECT)) keys |= (1 << 9);
    if (s(0, JP, 0, RETRO_DEVICE_ID_JOYPAD_START))  keys |= (1 << 10);
    if (s(0, JP, 0, RETRO_DEVICE_ID_JOYPAD_B))      keys |= (1 << 11);
    if (s(0, JP, 0, RETRO_DEVICE_ID_JOYPAD_X))      keys |= (1 << 12);
    if (s(0, JP, 0, RETRO_DEVICE_ID_JOYPAD_Y))      keys |= (1 << 13);
    if (s(0, JP, 0, RETRO_DEVICE_ID_JOYPAD_L))      keys |= (1 << 14);
    if (s(0, JP, 0, RETRO_DEVICE_ID_JOYPAD_R))      keys |= (1 << 15);
    if (s(0, JP, 0, RETRO_DEVICE_ID_JOYPAD_L2))     keys |= (1 << 16);
    if (s(0, JP, 0, RETRO_DEVICE_ID_JOYPAD_R2))     keys |= (1 << 17);
    return keys;
}

function get_kb_key_states_from_input() {
    let keys = 0;
    if (!input_state_cb) return keys;
    const KB = RETRO_DEVICE_KEYBOARD;
    for (const entry of KB_KEY_MAP) {
        if (input_state_cb(0, KB, 0, entry.retro_key)) {
            keys |= (1 << entry.bit);
        }
    }
    return keys;
}

function process_key_events() {
    if (!g_jvm || !g_jvm.running) return;

    const cb      = g_midp_callbacks;
    const pressed  = g_key_states & ~g_prev_key_states;
    const released = ~g_key_states & g_prev_key_states;
    const held     = g_key_states & g_prev_key_states;
    const now      = millis();

    for (const entry of KEY_MAP) {
        const bit     = entry.bit;
        const keycode = entry.keycode;

        if (pressed & (1 << bit)) {
            if (bit <= KEY_REPEAT_MAX_BIT) {
                g_key_repeat.is_held[bit]          = true;
                g_key_repeat.press_time[bit]       = now;
                g_key_repeat.last_repeat_time[bit] = now;
            }
            if (keycode === -6) {
                if (cb && typeof cb.midp_handle_soft_button === 'function') {
                    if (cb.midp_handle_soft_button(g_jvm, 0)) continue;
                }
            } else if (keycode === -7) {
                if (cb && typeof cb.midp_handle_soft_button === 'function') {
                    if (cb.midp_handle_soft_button(g_jvm, 1)) continue;
                }
            }
            if (cb && typeof cb.midp_call_keyPressed === 'function') {
                cb.midp_call_keyPressed(g_jvm, keycode);
            }
        } else if (released & (1 << bit)) {
            key_repeat_reset(bit);
            if (cb && typeof cb.midp_call_keyReleased === 'function') {
                cb.midp_call_keyReleased(g_jvm, keycode);
            }
        } else if ((held & (1 << bit)) && bit <= KEY_REPEAT_MAX_BIT && g_key_repeat.is_held[bit]) {
            const holdDuration = now - g_key_repeat.press_time[bit];
            const sinceLast    = now - g_key_repeat.last_repeat_time[bit];
            if (holdDuration >= KEY_REPEAT_INITIAL_DELAY_MS &&
                sinceLast    >= KEY_REPEAT_INTERVAL_MS) {
                g_key_repeat.last_repeat_time[bit] = now;
                if (keycode === -6 || keycode === -7) continue;
                if (cb && typeof cb.midp_call_keyPressed === 'function') {
                    cb.midp_call_keyPressed(g_jvm, keycode);
                }
            }
        }
    }

    g_prev_key_states = g_key_states;

    const kbPressed  = g_kb_key_states & ~g_kb_prev_key_states;
    const kbReleased = ~g_kb_key_states & g_kb_prev_key_states;

    for (const entry of KB_KEY_MAP) {
        const bit     = entry.bit;
        const keycode = entry.keycode;
        if (kbPressed & (1 << bit)) {
            if (cb && typeof cb.midp_call_keyPressed === 'function') {
                cb.midp_call_keyPressed(g_jvm, keycode);
            }
        }
        if (kbReleased & (1 << bit)) {
            if (cb && typeof cb.midp_call_keyReleased === 'function') {
                cb.midp_call_keyReleased(g_jvm, keycode);
            }
        }
    }

    g_kb_prev_key_states = g_kb_key_states;
}

export function retro_run() {
    if (environ_cb) {
        const updatedObj = { value: false };
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, updatedObj) && updatedObj.value) {
            update_variables(null);
        }
    }

    if (input_poll_cb) input_poll_cb();
    g_key_states    = get_key_states_from_input();
    g_kb_key_states = get_kb_key_states_from_input();

    libretro_begin_frame();

    if (sdl_has_error()) {
        const ctx = sdl_get_global_context();
        if (ctx && ctx.framebuffer) {
            sdl_draw_error_screen(ctx);
        }
        libretro_end_frame();
        send_video_frame();
        if (g_jvm && (g_key_states & (1 << 9))) {
            g_jvm.running = false;
        }
        return;
    }

    process_key_events();

    if (g_jvm && g_jvm.running) {
        const thread = g_jvm.main_thread;
        if (thread && thread.current_frame) {
            let instructionsPerFrame = 30000;
            if (g_target_fps === 60)      instructionsPerFrame = 15000;
            else if (g_target_fps === 20) instructionsPerFrame = 45000;
            else if (g_target_fps === 15) instructionsPerFrame = 60000;

            for (let i = 0; i < instructionsPerFrame && g_jvm.running; i++) {
                if (execute_frame(g_jvm, thread) !== 0) {
                    handle_exception(thread);
                    break;
                }
            }
        }

        const cb = g_midp_callbacks;
        if (cb && typeof cb.jvm_process_timers === 'function') {
            cb.jvm_process_timers(g_jvm);
        }
        if (cb && typeof cb.midp_process_call_serially_queue === 'function') {
            cb.midp_process_call_serially_queue(g_jvm);
        }
        midp_check_alert_timeout(g_jvm);
        if (cb && typeof cb.midp_process_repaints === 'function') {
            cb.midp_process_repaints(g_jvm);
        }
    }

    libretro_end_frame();
    send_video_frame();
    libretro_process_audio();
}

export function retro_serialize_size() {
    return 0;
}

export function retro_serialize(_data, _size) {
    return false;
}

export function retro_unserialize(_data, _size) {
    return false;
}

export function retro_cheat_reset() {
}

export function retro_cheat_set(_index, _enabled, _code) {
}

export function retro_get_region() {
    return RETRO_REGION_NTSC;
}

export function retro_get_memory_data(_id) {
    return null;
}

export function retro_get_memory_size(_id) {
    return 0;
}
