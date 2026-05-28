/*
 * Headless SDL Backend for Testing
 * No display, no audio - just executes bytecode
 *
 * Migrated from src/sdl/sdl_headless.c
 */

// ---------------------------------------------------------------------------
// MIDP key code constants (from include/sdl_backend.h)
// ---------------------------------------------------------------------------

export const MIDP_KEY_0          =  48;
export const MIDP_KEY_1          =  49;
export const MIDP_KEY_2          =  50;
export const MIDP_KEY_3          =  51;
export const MIDP_KEY_4          =  52;
export const MIDP_KEY_5          =  53;
export const MIDP_KEY_6          =  54;
export const MIDP_KEY_7          =  55;
export const MIDP_KEY_8          =  56;
export const MIDP_KEY_9          =  57;
export const MIDP_KEY_STAR       =  42;
export const MIDP_KEY_POUND      =  35;
export const MIDP_KEY_UP         =  -1;
export const MIDP_KEY_DOWN       =  -2;
export const MIDP_KEY_LEFT       =  -3;
export const MIDP_KEY_RIGHT      =  -4;
export const MIDP_KEY_SELECT     =  -5;
export const MIDP_KEY_SOFT_LEFT  =  -6;
export const MIDP_KEY_SOFT_RIGHT =  -7;
export const MIDP_KEY_CLEAR      =  -8;
export const MIDP_KEY_SEND       =  -9;
export const MIDP_KEY_END        = -10;

// ---------------------------------------------------------------------------
// SdlContext — plain JS object factory
// Mirrors the C struct SdlContext from include/sdl_backend.h.
// ---------------------------------------------------------------------------

/**
 * Create a zeroed SdlContext plain object.
 * @returns {SdlContext}
 */
function makeSdlContext() {
    return {
        // SDL handles (null — no real window in headless mode)
        window:     null,
        renderer:   null,
        texture:    null,

        // Framebuffer — Uint32Array allocated on init, null until then
        framebuffer: null,
        width:       0,
        height:      0,
        scale:       1,

        // Input state
        input: {
            key_states:      new Array(256).fill(false),
            pointer_x:       0,
            pointer_y:       0,
            pointer_pressed: false,
        },

        // Audio state
        audio: {
            device:   0,
            frequency: 0,
            channels:  0,
            samples:   0,
            enabled:   false,
        },

        // Timing
        start_time: 0n,
        frame_time: 0n,
        target_fps: 30,
        vsync:      false,

        // State flags
        running:      false,
        minimized:    false,
        fullscreen:   false,
        headless:     true,
        needs_redraw: false,

        // JVM reference (opaque object set by caller)
        jvm: null,
    };
}

// ---------------------------------------------------------------------------
// Module-level globals (mirrors C file-scoped statics)
// ---------------------------------------------------------------------------

/** @type {ReturnType<makeSdlContext>} */
const g_headless_ctx = makeSdlContext();

let g_headless_needs_redraw = false;

// Error display state
let g_error_title   = '';
let g_error_message = '';
let g_error_stack   = '';
let g_error_extra   = '';
let g_has_error     = false;

// ---------------------------------------------------------------------------
// Context accessors
// ---------------------------------------------------------------------------

/**
 * Return the module-level headless context.
 * @returns {ReturnType<makeSdlContext>}
 */
export function sdl_get_global_context() {
    return g_headless_ctx;
}

/**
 * Copy fields from ctx into the module-level context, or zero it out.
 * @param {ReturnType<makeSdlContext>|null} ctx
 */
export function sdl_set_global_context(ctx) {
    if (ctx) {
        Object.assign(g_headless_ctx, ctx);
    } else {
        Object.assign(g_headless_ctx, makeSdlContext());
    }
}

// ---------------------------------------------------------------------------
// Initialization / teardown
// ---------------------------------------------------------------------------

/**
 * Initialize the headless context.
 * @param {object}  jvm       - JVM instance (opaque)
 * @param {number}  width     - Screen width  (default 240 if <= 0)
 * @param {number}  height    - Screen height (default 320 if <= 0)
 * @param {number}  scale     - Ignored in headless mode
 * @param {boolean} headless  - Ignored; always true
 * @returns {number} 0 on success, -1 on failure
 */
export function sdl_init(jvm, width, height, scale, headless) {
    // void scale, headless
    if (width  <= 0) width  = 240;
    if (height <= 0) height = 320;

    g_headless_ctx.jvm        = jvm;
    g_headless_ctx.width      = width;
    g_headless_ctx.height     = height;
    g_headless_ctx.scale      = 1;
    g_headless_ctx.target_fps = 30;
    g_headless_ctx.running    = true;
    g_headless_ctx.headless   = true;

    // Allocate framebuffer (replaces calloc — GC handles cleanup)
    try {
        g_headless_ctx.framebuffer = new Uint32Array(width * height);
    } catch (e) {
        process.stderr.write(`[Headless] Failed to allocate framebuffer\n`);
        return -1;
    }

    process.stderr.write(`[Headless] Initialized ${width}x${height}\n`);
    return 0;
}

/**
 * Release framebuffer resources.
 * @param {ReturnType<makeSdlContext>|null} ctx
 */
export function sdl_destroy(ctx) {
    if (ctx && ctx.framebuffer) {
        ctx.framebuffer = null;
    }
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

/**
 * Run the headless event loop.
 *
 * Caller must supply an interface object `callbacks` with the following
 * optional async-callable methods (mirrors the C `extern` declarations):
 *   - midp_process_repaints(jvm)
 *   - jvm_process_timers(jvm)
 *   - midp_process_call_serially_queue(jvm)
 *   - midp_check_alert_timeout(jvm) → boolean
 *   - midp_clear_pending_repaint()
 *   - midp_repaint_current(jvm)
 *   - midp_handle_soft_button(jvm, buttonIndex) → boolean
 *   - midp_call_keyPressed(jvm, keycode)
 *   - midp_call_keyReleased(jvm, keycode)
 *   - headless_check_text_activity() → boolean|number
 *
 * All callbacks are optional; missing ones are silently skipped.
 *
 * @param {ReturnType<makeSdlContext>|null} ctx
 * @param {object} [callbacks={}]
 * @returns {Promise<void>}
 */
export async function sdl_run(ctx, callbacks = {}) {
    if (!ctx) return;

    process.stderr.write(`[Headless] Running headless mode (waiting for MIDlet)...\n`);

    // Force running flags
    ctx.running = true;
    if (ctx.jvm) {
        ctx.jvm.running = true;
    }

    const cb = callbacks;

    // Key injection schedule (mirrors C arrays)
    const keys_to_try = [5, -5, -6, 4, 8, 35, 42, 48, 49, 50, 51, 52, 53, 55, 56, 57];
    const num_keys     = keys_to_try.length;
    const key_fire_frame = 100;   // Start injecting after ~1 second
    const key_interval   = 50;    // Inject every 0.5 seconds
    const last_key_frame = key_fire_frame + (num_keys - 1) * key_interval;

    // Soft button frames
    const soft_button_frames = [
        last_key_frame + key_interval,
        last_key_frame + key_interval * 2,
        last_key_frame + key_interval * 3,
        last_key_frame + key_interval * 4,
    ];
    const soft_button_ids = [0, 1, 0, 1]; // left, right, left, right
    const num_soft_keys   = soft_button_frames.length;

    const idle_timeout_frames = 300; // 3 seconds at 10 ms/frame
    const max_frames          = 60000;

    let frames             = 0;
    let last_activity_frame = 0;
    let last_auto_redraw   = Date.now();
    const start_time_ms    = last_auto_redraw;

    while (ctx.running && ctx.jvm && ctx.jvm.running) {
        frames++;
        let had_activity = 0;

        // Phase 1: Inject regular key presses
        for (let ki = 0; ki < num_keys; ki++) {
            const target_frame = key_fire_frame + ki * key_interval;
            if (frames === target_frame) {
                process.stderr.write(
                    `[Headless] Injecting key code ${keys_to_try[ki]} (frame ${frames})\n`
                );
                if (cb.midp_call_keyPressed)  cb.midp_call_keyPressed(ctx.jvm, keys_to_try[ki]);
                if (cb.midp_call_keyReleased) cb.midp_call_keyReleased(ctx.jvm, keys_to_try[ki]);
                had_activity = 1;
            }
        }

        // Phase 2: Inject soft button presses
        for (let si = 0; si < num_soft_keys; si++) {
            if (frames === soft_button_frames[si]) {
                process.stderr.write(
                    `[Headless] Injecting soft button ${soft_button_ids[si]} (frame ${frames})\n`
                );
                if (cb.midp_handle_soft_button) {
                    cb.midp_handle_soft_button(ctx.jvm, soft_button_ids[si]);
                }
                had_activity = 1;
            }
        }

        // Process timers
        if (cb.jvm_process_timers) cb.jvm_process_timers(ctx.jvm);

        // Process callSerially queue
        if (cb.midp_process_call_serially_queue) cb.midp_process_call_serially_queue(ctx.jvm);

        // Check Alert timeout
        if (cb.midp_check_alert_timeout) cb.midp_check_alert_timeout(ctx.jvm);

        // Auto-redraw at ~60 FPS (every 16 ms)
        const current_time = Date.now();
        if (current_time - last_auto_redraw >= 16) {
            last_auto_redraw = current_time;
            if (cb.midp_process_repaints) cb.midp_process_repaints(ctx.jvm);
            if (cb.midp_clear_pending_repaint) cb.midp_clear_pending_repaint();
        }

        // Check for error display
        if (sdl_has_error()) {
            process.stderr.write(`[Headless] Error detected, stopping.\n`);
            break;
        }

        // Track activity
        const text_activity = cb.headless_check_text_activity
            ? cb.headless_check_text_activity()
            : 0;
        if (had_activity || text_activity) {
            last_activity_frame = frames;
        }

        // Idle detection
        if (frames > soft_button_frames[num_soft_keys - 1] + 50) {
            if (frames - last_activity_frame >= idle_timeout_frames) {
                process.stderr.write(
                    `[Headless] Idle timeout: no activity for ${idle_timeout_frames} frames, exiting cleanly.\n`
                );
                break;
            }
        }

        // Safety: max time limit (60 seconds)
        const elapsed_ms = current_time - start_time_ms;
        if (elapsed_ms > 60000) {
            process.stderr.write(`[Headless] Max time limit reached (60s), exiting.\n`);
            break;
        }

        // 10 ms per iteration (replaces usleep(10000))
        await new Promise(resolve => setTimeout(resolve, 10));
    }

    if (frames >= max_frames) {
        process.stderr.write(`[Headless] Reached max iterations\n`);
    } else {
        process.stderr.write(`[Headless] Stopped after ${frames} iterations\n`);
    }

    // Analyze framebuffer content
    if (ctx && ctx.framebuffer) {
        const total    = ctx.width * ctx.height;
        let   non_zero = 0;
        for (let i = 0; i < total; i++) {
            if ((ctx.framebuffer[i] & 0x00FFFFFF) !== 0) non_zero++;
        }
        const pct = total > 0 ? (100.0 * non_zero / total).toFixed(1) : '0.0';
        process.stderr.write(
            `[Headless] Framebuffer: ${non_zero}/${total} non-zero pixels (${pct}%)\n`
        );
        sdl_save_framebuffer_to_file(ctx, '/tmp/bounce_framebuffer.ppm');
        process.stderr.write(`[Headless] Framebuffer saved to /tmp/bounce_framebuffer.ppm\n`);
    }

    process.stderr.write(`[Headless] Finished\n`);
}

// ---------------------------------------------------------------------------
// Window / display stubs
// ---------------------------------------------------------------------------

/** @param {ReturnType<makeSdlContext>|null} ctx @param {boolean} fullscreen */
export function sdl_set_fullscreen(ctx, fullscreen) { /* no-op */ }

/**
 * Always returns 0 in headless mode (no real clock).
 * @param {ReturnType<makeSdlContext>|null} ctx
 * @returns {bigint}
 */
export function sdl_get_ticks(ctx) { return 0n; }

/**
 * Sleep for ms milliseconds.
 * NOTE: In headless JS, use await-based sleep via sdl_run; this sync variant
 * is provided for interface completeness but cannot truly block the event loop.
 * @param {number} ms
 * @returns {Promise<void>}
 */
export function sdl_delay(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

/** @param {ReturnType<makeSdlContext>|null} ctx */
export function sdl_update_screen(ctx) { /* no-op */ }

/** @param {ReturnType<makeSdlContext>|null} ctx */
export function sdl_handle_events(ctx) { /* no-op */ }

/**
 * @param {ReturnType<makeSdlContext>|null} ctx
 * @param {number} key
 * @returns {boolean}
 */
export function sdl_key_pressed(ctx, key) { return false; }

/**
 * Always returns null in headless mode.
 * @param {ReturnType<makeSdlContext>|null} ctx
 * @returns {null}
 */
export function sdl_get_graphics(ctx) { return null; }

// ---------------------------------------------------------------------------
// Audio stubs (all no-op)
// ---------------------------------------------------------------------------

/** @param {number} sample_rate @returns {number} */
export function sdl_audio_init_simple(sample_rate) { return 0; }

export function sdl_audio_shutdown() { /* no-op */ }

/** @param {Int16Array|null} samples @param {number} count */
export function sdl_audio_queue_samples(samples, count) { /* no-op */ }

/** @returns {number} */
export function sdl_audio_get_queued_size() { return 0; }

/** @param {ReturnType<makeSdlContext>|null} ctx */
export function sdl_present(ctx) { /* no-op */ }

// ---------------------------------------------------------------------------
// Redraw flag
// ---------------------------------------------------------------------------

export function sdl_request_redraw() {
    g_headless_needs_redraw = true;
}

/**
 * @param {ReturnType<makeSdlContext>|null} ctx
 * @returns {boolean}
 */
export function sdl_needs_redraw(ctx) {
    return g_headless_needs_redraw;
}

/** @param {ReturnType<makeSdlContext>|null} ctx */
export function sdl_clear_redraw(ctx) {
    g_headless_needs_redraw = false;
}

// ---------------------------------------------------------------------------
// Minimal event processing
// ---------------------------------------------------------------------------

/**
 * Process timers and any pending repaints.
 * Caller must provide optional callbacks object with:
 *   - jvm_process_timers(jvm)
 *   - midp_process_repaints(jvm)
 * @param {object} [callbacks={}]
 */
export function sdl_process_events_minimal(callbacks = {}) {
    const jvm = g_headless_ctx.jvm;

    if (jvm && jvm.running && callbacks.jvm_process_timers) {
        callbacks.jvm_process_timers(jvm);
    }

    if (g_headless_needs_redraw && jvm && callbacks.midp_process_repaints) {
        callbacks.midp_process_repaints(jvm);
        g_headless_needs_redraw = false;
    }
}

// ---------------------------------------------------------------------------
// Framebuffer operations
// ---------------------------------------------------------------------------

/** @param {ReturnType<makeSdlContext>|null} ctx */
export function sdl_update_texture(ctx) { /* no-op */ }

/**
 * Fill the framebuffer with a solid color.
 * @param {ReturnType<makeSdlContext>|null} ctx
 * @param {number} color  - 32-bit ARGB/RGBA value
 */
export function sdl_clear(ctx, color) {
    if (!ctx || !ctx.framebuffer) return;
    ctx.framebuffer.fill(color >>> 0);
}

// ---------------------------------------------------------------------------
// Misc backend interface stubs
// ---------------------------------------------------------------------------

/**
 * @param {ReturnType<makeSdlContext>|null} ctx
 * @param {number} width
 * @param {number} height
 * @returns {number}
 */
export function sdl_resize(ctx, width, height) { return 0; }

/**
 * @param {ReturnType<makeSdlContext>|null} ctx
 * @param {string} filename
 * @returns {number}
 */
export function sdl_screenshot(ctx, filename) { return -1; }

/** @param {ReturnType<makeSdlContext>|null} ctx */
export function sdl_frame_end(ctx) { /* no-op */ }

/**
 * @param {number} ms
 * @returns {Promise<void>}
 */
export function sdl_sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

/** @param {ReturnType<makeSdlContext>|null} ctx @param {string} title */
export function sdl_set_title(ctx, title) { /* no-op */ }

/** @param {ReturnType<makeSdlContext>|null} ctx */
export function sdl_toggle_fullscreen(ctx) { /* no-op */ }

/**
 * @param {ReturnType<makeSdlContext>|null} ctx
 * @param {{ value: number }|null} widthOut   - object with .value set to ctx.width
 * @param {{ value: number }|null} heightOut  - object with .value set to ctx.height
 *
 * In JS there are no out-pointers; callers receive width/height via return value
 * or by passing wrapper objects. This function also returns [width, height].
 * @returns {[number, number]}
 */
export function sdl_get_window_size(ctx, widthOut, heightOut) {
    const w = (ctx ? ctx.width  : 0);
    const h = (ctx ? ctx.height : 0);
    if (widthOut  != null) widthOut.value  = w;
    if (heightOut != null) heightOut.value = h;
    return [w, h];
}

/** @param {ReturnType<makeSdlContext>|null} ctx */
export function sdl_stop(ctx) {
    if (ctx) ctx.running = false;
}

/**
 * @param {ReturnType<makeSdlContext>|null} ctx
 * @returns {null}
 */
export function sdl_get_platform_callbacks(ctx) { return null; }

/**
 * @param {object|null} gfx  - MidpGraphics object
 * @returns {boolean}
 */
export function sdl_is_screen_graphics(gfx) { return true; }

// ---------------------------------------------------------------------------
// Audio with context (extended API)
// ---------------------------------------------------------------------------

/**
 * @param {ReturnType<makeSdlContext>|null} ctx
 * @param {number} frequency
 * @param {number} channels
 * @param {number} samples
 * @returns {number}
 */
export function sdl_audio_init(ctx, frequency, channels, samples) { return 0; }

/**
 * @param {ReturnType<makeSdlContext>|null} ctx
 * @param {Uint8Array|null} data
 * @param {number} length
 * @returns {number}
 */
export function sdl_audio_queue(ctx, data, length) { return 0; }

/** @param {ReturnType<makeSdlContext>|null} ctx @returns {number} */
export function sdl_audio_queued_size(ctx) { return 0; }

/** @param {ReturnType<makeSdlContext>|null} ctx */
export function sdl_audio_clear(ctx) { /* no-op */ }

/** @param {ReturnType<makeSdlContext>|null} ctx */
export function sdl_process_events(ctx) { /* no-op */ }

/** @param {number} sdl_key @returns {number} */
export function sdl_key_to_midp(sdl_key) { return 0; }

/** @param {number} sdl_key @returns {number} */
export function sdl_key_to_game_action(sdl_key) { return 0; }

/**
 * Get pointer position. Returns [x, y]; also writes into wrapper objects if
 * provided (mirrors the C out-pointer pattern).
 * @param {ReturnType<makeSdlContext>|null} ctx
 * @param {{ value: number }|null} xOut
 * @param {{ value: number }|null} yOut
 * @returns {[number, number]}
 */
export function sdl_get_pointer(ctx, xOut, yOut) {
    if (xOut != null) xOut.value = 0;
    if (yOut != null) yOut.value = 0;
    return [0, 0];
}

/** @param {ReturnType<makeSdlContext>|null} ctx @returns {boolean} */
export function sdl_pointer_pressed(ctx) { return false; }

/** @param {ReturnType<makeSdlContext>|null} ctx */
export function sdl_dump_info(ctx) { /* no-op */ }

// ---------------------------------------------------------------------------
// Framebuffer PPM save
// ---------------------------------------------------------------------------

/**
 * Save the framebuffer to a PPM (P6) file.
 * @param {ReturnType<makeSdlContext>|null} ctx
 * @param {string} filename
 */
export function sdl_save_framebuffer_to_file(ctx, filename) {
    if (!ctx || !ctx.framebuffer || !filename) return;

    // Build PPM binary data in memory
    const header   = `P6\n${ctx.width} ${ctx.height}\n255\n`;
    const headerBuf = Buffer.from(header, 'ascii');
    const pixelBuf  = Buffer.allocUnsafe(ctx.width * ctx.height * 3);

    const fb    = ctx.framebuffer;
    const total = ctx.width * ctx.height;
    let   off   = 0;
    for (let i = 0; i < total; i++) {
        const pixel = fb[i] >>> 0;
        pixelBuf[off++] = (pixel >>> 16) & 0xFF; // R
        pixelBuf[off++] = (pixel >>>  8) & 0xFF; // G
        pixelBuf[off++] =  pixel         & 0xFF; // B
    }

    // Write file synchronously (mirrors fopen/fputc/fclose)
    import('node:fs').then(fs => {
        try {
            fs.writeFileSync(filename, Buffer.concat([headerBuf, pixelBuf]));
        } catch (e) {
            process.stderr.write(`[Headless] Failed to save framebuffer: ${e.message}\n`);
        }
    });
}

// ---------------------------------------------------------------------------
// Error display
// ---------------------------------------------------------------------------

/**
 * Record an uncaught exception and print it to stderr.
 * @param {string|null} title
 * @param {string|null} message
 * @param {string|null} stack_trace
 */
export function sdl_set_error_info(title, message, stack_trace) {
    g_has_error = true;
    if (title       != null) g_error_title   = String(title);
    if (message     != null) g_error_message = String(message);
    if (stack_trace != null) g_error_stack   = String(stack_trace);

    process.stderr.write(`\n========================================\n`);
    process.stderr.write(`  [J2ME UNCAUGHT EXCEPTION]\n`);
    process.stderr.write(`========================================\n`);
    process.stderr.write(`  Exception: ${g_error_title}\n`);
    if (g_error_message) process.stderr.write(`  Message:   ${g_error_message}\n`);
    if (g_error_extra)   process.stderr.write(`  Details:   ${g_error_extra}\n`);
    if (g_error_stack)   process.stderr.write(`  Stack:\n${g_error_stack}`);
    process.stderr.write(`========================================\n\n`);
}

/**
 * Set extra detail string (thread name, PC, etc.).
 * @param {string|null} extra
 */
export function sdl_set_error_extra(extra) {
    g_error_extra = (extra != null) ? String(extra) : '';
}

/** @returns {boolean} */
export function sdl_has_error() {
    return g_has_error;
}

/** Clear all error state. */
export function sdl_clear_error() {
    g_has_error     = false;
    g_error_title   = '';
    g_error_message = '';
    g_error_stack   = '';
    g_error_extra   = '';
}

/**
 * Print the current error to stderr (headless version — no framebuffer rendering).
 * @param {ReturnType<makeSdlContext>|null} ctx
 */
export function sdl_draw_error_screen(ctx) {
    process.stderr.write(`\n========== ERROR SCREEN ==========\n`);
    process.stderr.write(`Error: ${g_error_title}\n`);
    if (g_error_message) process.stderr.write(`${g_error_message}\n`);
    if (g_error_stack)   process.stderr.write(`\nStack:\n${g_error_stack}\n`);
    process.stderr.write(`==================================\n`);
}
