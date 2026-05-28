import { readFileSync, existsSync } from 'node:fs';
import { inflateRawSync } from 'node:zlib';
import { fileURLToPath } from 'node:url';

import {
    jvm_create,
    jvm_destroy,
    jvm_init,
    jvm_set_jar_data,
    jvm_load_class,
    jvm_current_thread,
    JNI_OK,
} from './jvm/jvm.mjs';

import {
    jvm_run_midlet,
} from './jvm/execute.mjs';

import {
    sdl_set_global_context,
    sdl_init,
    sdl_destroy,
    sdl_run,
    sdl_set_fullscreen,
    sdl_set_error_info,
    sdl_has_error,
} from './sdl/sdl_headless.mjs';

// ─────────────────────────────────────────────────────────────
// Constants (mirrors jvm.h / midp.h #define)
// ─────────────────────────────────────────────────────────────
export const J2ME_EMULATOR_VERSION = '1.0.0';
export const MIDP_DEFAULT_WIDTH    = 240;
export const MIDP_DEFAULT_HEIGHT   = 320;
export const JAVA_STACK_SIZE       = 256 * 1024;
export const MAX_JAVA_THREADS      = 16;

// ─────────────────────────────────────────────────────────────
// Module-level globals (mirrors C file-scoped statics)
// ─────────────────────────────────────────────────────────────
let g_sdl_ctx = null;
let g_jvm     = null;

let g_jar_data = null;
let g_jar_size = 0;

// ─────────────────────────────────────────────────────────────
// get_jar_data — public API used by resource loading
// ─────────────────────────────────────────────────────────────
export function get_jar_data(sizeRef) {
    if (sizeRef != null) sizeRef.value = g_jar_size;
    return g_jar_data;
}

// ─────────────────────────────────────────────────────────────
// ZIP / JAR helpers — little-endian reads, Central Directory walk
// ─────────────────────────────────────────────────────────────
function read_u16(buf, offset) {
    return buf[offset] | (buf[offset + 1] << 8);
}

function read_u32(buf, offset) {
    return (buf[offset]
        | (buf[offset + 1] << 8)
        | (buf[offset + 2] << 16)
        | (buf[offset + 3] << 24)) >>> 0;
}

function find_end_of_central_directory(jarData, jarSize) {
    if (jarSize < 22) return 0;
    const maxComment  = 65535 + 22;
    const searchStart = jarSize > maxComment ? jarSize - maxComment : 0;
    for (let i = jarSize - 22; i >= searchStart && i > 0; i--) {
        if (jarData[i] === 0x50 && jarData[i + 1] === 0x4B &&
            jarData[i + 2] === 0x05 && jarData[i + 3] === 0x06) {
            return i;
        }
    }
    return 0;
}

/**
 * Find and extract a named file from a raw JAR/ZIP Uint8Array.
 * Returns a Buffer with uncompressed data, or null if not found.
 */
export function jar_find_file(jarData, jarSize, filename) {
    const eocdOffset = find_end_of_central_directory(jarData, jarSize);
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

        const entryName = Buffer.from(
            jarData.buffer, jarData.byteOffset + cdeOffset + 46, filenameLen
        ).toString('utf8');
        const isDir = filenameLen > 0 && entryName[filenameLen - 1] === '/';

        if (!isDir && entryName === filename) {
            const localFnLen    = read_u16(jarData, localHdrOffset + 26);
            const localExtraLen = read_u16(jarData, localHdrOffset + 28);
            const dataOffset    = localHdrOffset + 30 + localFnLen + localExtraLen;

            if (compression === 0) {
                return Buffer.from(
                    jarData.buffer, jarData.byteOffset + dataOffset, uncompSize
                );
            } else if (compression === 8) {
                if (compSize === 0 || uncompSize === 0) return null;
                const compressed = Buffer.from(
                    jarData.buffer, jarData.byteOffset + dataOffset, compSize
                );
                try {
                    return inflateRawSync(compressed);
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

// ─────────────────────────────────────────────────────────────
// load_jar_resource — public API referenced by midp.h consumers
// ─────────────────────────────────────────────────────────────
export function load_jar_resource(filename, sizeRef) {
    const buf = jar_find_file(g_jar_data, g_jar_size, filename);
    if (sizeRef != null) sizeRef.value = buf ? buf.length : 0;
    return buf;
}

// ─────────────────────────────────────────────────────────────
// midlet_generate_drm_properties — scan JAR for well-known DRM file patterns
// ─────────────────────────────────────────────────────────────
export function midlet_generate_drm_properties(jarData, jarSize, addPropertyFn) {
    if (!jarData || jarSize === 0) return;
    if (typeof addPropertyFn !== 'function') return;

    process.stderr.write('[MIDlet] Auto-generating DRM properties from JAR contents...\n');

    let dchocIndex = 1;

    if (jar_find_file(jarData, jarSize, 'p')) {
        addPropertyFn(`DCHOC-${dchocIndex++}`, 'p');
    }

    for (let i = 1; i <= 99 && dchocIndex <= 50; i++) {
        const value = `r${i}`;
        if (jar_find_file(jarData, jarSize, value)) {
            addPropertyFn(`DCHOC-${dchocIndex++}`, value);
        }
    }

    for (let level = 0; level <= 9 && dchocIndex <= 80; level++) {
        for (let sub = 0; sub <= 9; sub++) {
            const value = `l${level}_${sub}`;
            if (jar_find_file(jarData, jarSize, value)) {
                addPropertyFn(`DCHOC-${dchocIndex++}`, value);
            }
        }
    }

    let otherIndex = 1;
    for (let i = 1; i <= 20; i++) {
        const value = `s${i}`;
        if (jar_find_file(jarData, jarSize, value)) {
            addPropertyFn(`SIE-${otherIndex++}`, value);
        }
    }

    otherIndex = 1;
    for (let i = 1; i <= 20; i++) {
        const value = `g${i}`;
        if (jar_find_file(jarData, jarSize, value)) {
            addPropertyFn(`GL-${otherIndex++}`, value);
        }
    }

    otherIndex = 1;
    for (let i = 1; i <= 20; i++) {
        const value = `ea${i}`;
        if (jar_find_file(jarData, jarSize, value)) {
            addPropertyFn(`EA-${otherIndex++}`, value);
        }
    }
}

// ─────────────────────────────────────────────────────────────
// find_midlet_class — parse MANIFEST.MF for MIDlet-1:
// ─────────────────────────────────────────────────────────────
export function find_midlet_class(jarData, jarSize) {
    let manifest = jar_find_file(jarData, jarSize, 'META-INF/MANIFEST.MF');
    if (!manifest) {
        manifest = jar_find_file(jarData, jarSize, 'META-INF/manifest.mf');
    }
    if (!manifest) return null;

    const text  = manifest.toString('utf8');
    const lines = text.split(/\r\n|\r|\n/);

    for (const line of lines) {
        if (line.startsWith('MIDlet-1:') || line.startsWith('MIDlet-1 :')) {
            const colon = line.indexOf(':');
            if (colon < 0) continue;
            const rest  = line.slice(colon + 1);
            const parts = rest.split(',');
            if (parts.length >= 3) {
                const className = parts[2].trim();
                if (className) return className;
            }
        }
    }
    return null;
}

// ─────────────────────────────────────────────────────────────
// load_jar_file — read JAR from filesystem into Uint8Array
// ─────────────────────────────────────────────────────────────
function load_jar_file(filename) {
    try {
        const buf = readFileSync(filename);
        return new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength);
    } catch (e) {
        process.stderr.write(`[Main] Cannot open file: '${filename}': ${e.message}\n`);
        return null;
    }
}

// ─────────────────────────────────────────────────────────────
// print_usage
// ─────────────────────────────────────────────────────────────
function print_usage(program) {
    process.stdout.write(
        `J2ME Emulator v${J2ME_EMULATOR_VERSION} - MIDP2 Mobile Java Emulator\n\n` +
        `Usage: ${program} [options] <midlet.jar> [midlet-class]\n\n` +
        `Options:\n` +
        `  -w, --width <width>      Screen width (default: 240)\n` +
        `  -h, --height <height>    Screen height (default: 320)\n` +
        `  -s, --scale <scale>      Display scale factor (default: 2)\n` +
        `  -c, --classpath <path>   Additional classpath\n` +
        `  -m, --midlet <class>     MIDlet class name\n` +
        `  -f, --fullscreen         Start in fullscreen mode\n` +
        `  -v, --verbose            Verbose output\n` +
        `  --verbose-class          Verbose class loading\n` +
        `  --verbose-gc             Verbose garbage collection\n` +
        `  --heap-size <size>       Heap size in MB (default: 16)\n` +
        `  --headless               Run without display (for testing)\n` +
        `  --help                   Show this help\n` +
        `\nExamples:\n` +
        `  ${program} game.jar\n` +
        `  ${program} -w 320 -h 480 app.jar com.example.MyMIDlet\n` +
        `  ${program} -s 3 --fullscreen game.jar\n` +
        `  ${program} --headless game.jar  # For testing without display\n`
    );
}

// ─────────────────────────────────────────────────────────────
// parse_args — returns an Options object or null on error/--help
// ─────────────────────────────────────────────────────────────
export function parse_args(argv) {
    const opts = {
        jar_file:      null,
        midlet_class:  null,
        width:         MIDP_DEFAULT_WIDTH,
        height:        MIDP_DEFAULT_HEIGHT,
        scale:         2,
        fullscreen:    false,
        verbose:       false,
        verbose_class: false,
        verbose_gc:    false,
        headless:      false,
        heap_size_mb:  16,
        classpath:     null,
    };

    for (let i = 0; i < argv.length; i++) {
        const arg = argv[i];
        if (arg === '-w' || arg === '--width') {
            if (++i >= argv.length) return null;
            opts.width = parseInt(argv[i], 10);
        } else if (arg === '-h' || arg === '--height') {
            if (++i >= argv.length) return null;
            opts.height = parseInt(argv[i], 10);
        } else if (arg === '-s' || arg === '--scale') {
            if (++i >= argv.length) return null;
            opts.scale = parseInt(argv[i], 10);
        } else if (arg === '-m' || arg === '--midlet') {
            if (++i >= argv.length) return null;
            opts.midlet_class = argv[i];
        } else if (arg === '-c' || arg === '--classpath') {
            if (++i >= argv.length) return null;
            opts.classpath = argv[i];
        } else if (arg === '-f' || arg === '--fullscreen') {
            opts.fullscreen = true;
        } else if (arg === '-v' || arg === '--verbose') {
            opts.verbose       = true;
            opts.verbose_class = true;
            opts.verbose_gc    = true;
        } else if (arg === '--verbose-class') {
            opts.verbose_class = true;
        } else if (arg === '--verbose-gc') {
            opts.verbose_gc = true;
        } else if (arg === '--heap-size') {
            if (++i >= argv.length) return null;
            opts.heap_size_mb = parseInt(argv[i], 10);
        } else if (arg === '--headless') {
            opts.headless = true;
        } else if (arg === '--help') {
            return null;
        } else if (!arg.startsWith('-')) {
            if (!opts.jar_file) {
                opts.jar_file = arg;
            } else if (!opts.midlet_class) {
                opts.midlet_class = arg;
            }
        } else {
            process.stderr.write(`Unknown option: ${arg}\n`);
            return null;
        }
    }

    if (!opts.jar_file) return null;
    return opts;
}

// ─────────────────────────────────────────────────────────────
// init_emulator — create JVM and SDL context
// ─────────────────────────────────────────────────────────────
async function init_emulator(opts) {
    process.stderr.write('=== Initializing J2ME Emulator ===\n');
    process.stderr.write(`Configuration: ${opts.width}x${opts.height}, scale=${opts.scale}, heap=${opts.heap_size_mb}MB, headless=${opts.headless ? 'yes' : 'no'}\n`);

    g_jvm = jvm_create();
    if (!g_jvm) {
        process.stderr.write('[Main] Failed to create JVM\n');
        return false;
    }

    g_jvm.config.heap_size     = opts.heap_size_mb * 1024 * 1024;
    g_jvm.config.stack_size    = JAVA_STACK_SIZE;
    g_jvm.config.max_threads   = MAX_JAVA_THREADS;
    g_jvm.config.verbose_class = opts.verbose_class;
    g_jvm.config.verbose_gc    = opts.verbose_gc;

    if (jvm_init(g_jvm) !== JNI_OK) {
        process.stderr.write('[Main] Failed to initialize JVM\n');
        return false;
    }
    process.stderr.write('[Main] JVM initialized\n');

    // Build headless SDL context
    g_sdl_ctx = {
        window:     null,
        renderer:   null,
        texture:    null,
        framebuffer: null,
        width:       opts.width,
        height:      opts.height,
        scale:       opts.scale,
        input: {
            key_states:      new Array(256).fill(false),
            pointer_x:       0,
            pointer_y:       0,
            pointer_pressed: false,
        },
        audio: {
            device:    0,
            frequency: 0,
            channels:  0,
            samples:   0,
            enabled:   false,
        },
        start_time:   0n,
        frame_time:   0n,
        target_fps:   30,
        vsync:        false,
        running:      false,
        minimized:    false,
        fullscreen:   false,
        headless:     true,
        needs_redraw: false,
        jvm:          g_jvm,
    };

    sdl_set_global_context(g_sdl_ctx);

    if (opts.headless) {
        const fbSize = opts.width * opts.height;
        g_sdl_ctx.framebuffer = new Uint32Array(fbSize).fill(0xFF000000);
        g_sdl_ctx.running = true;
        process.stderr.write(`[Main] Headless mode initialized (${opts.width}x${opts.height})\n`);
    } else {
        const sdlResult = sdl_init(g_jvm, opts.width, opts.height, opts.scale, false);
        if (sdlResult !== 0) {
            process.stderr.write(`[Main] Failed to initialize SDL (${sdlResult})\n`);
            g_sdl_ctx = null;
            return false;
        }
        if (!g_sdl_ctx.target_fps || !g_sdl_ctx.framebuffer) {
            process.stderr.write('[Main] SDL context not properly initialized\n');
            return false;
        }
        process.stderr.write(`[Main] SDL initialized (${opts.width}x${opts.height}, scale: ${opts.scale})\n`);
    }

    g_sdl_ctx.jvm = g_jvm;
    sdl_set_global_context(g_sdl_ctx);

    if (opts.fullscreen) {
        sdl_set_fullscreen(g_sdl_ctx, true);
    }

    process.stderr.write('=== Emulator initialized successfully ===\n');
    return true;
}

// ─────────────────────────────────────────────────────────────
// run_midlet — load JAR, parse manifest, load class, start MIDlet
// ─────────────────────────────────────────────────────────────
async function run_midlet(opts) {
    process.stderr.write('=== Loading MIDlet ===\n');

    const jarData = load_jar_file(opts.jar_file);
    if (!jarData) {
        process.stderr.write(`[Main] Failed to load JAR: ${opts.jar_file}\n`);
        return false;
    }
    const jarSize = jarData.length;
    process.stderr.write(`[Main] JAR loaded: ${jarSize} bytes\n`);

    g_jar_data = jarData;
    g_jar_size = jarSize;

    jvm_set_jar_data(g_jvm, jarData, jarSize, opts.jar_file);

    // Load manifest for getAppProperty support
    const manifestBuf = jar_find_file(jarData, jarSize, 'META-INF/MANIFEST.MF')
        || jar_find_file(jarData, jarSize, 'META-INF/manifest.mf');
    if (manifestBuf) {
        process.stderr.write(`[Main] Manifest loaded: ${manifestBuf.length} bytes\n`);
        // Manifest is stored in the JVM class_loader for getAppProperty lookups
        g_jvm.class_loader._manifest = manifestBuf.toString('utf8');
    }

    // Try to load JAD file for additional properties
    const jarPath = opts.jar_file;
    let jadPath = null;
    if (jarPath.endsWith('.jar') || jarPath.endsWith('.JAR')) {
        jadPath = jarPath.slice(0, -4) + (jarPath.endsWith('.JAR') ? '.JAD' : '.jad');
    } else {
        jadPath = jarPath + '.jad';
    }
    try {
        const jadBuf = readFileSync(jadPath, 'utf8');
        process.stderr.write(`[Main] JAD loaded: ${jadPath}\n`);
        // Append JAD properties
        if (!g_jvm.class_loader._manifest) {
            g_jvm.class_loader._manifest = jadBuf;
        } else {
            g_jvm.class_loader._manifest += '\n' + jadBuf;
        }
    } catch {
        // JAD is optional
    }

    // Find MIDlet class
    let midletClass = opts.midlet_class;
    if (!midletClass) {
        midletClass = find_midlet_class(jarData, jarSize);
        if (!midletClass) {
            process.stderr.write('[Main] No MIDlet class specified or found in manifest\n');
            return false;
        }
        process.stderr.write(`[Main] Found MIDlet class in manifest: ${midletClass}\n`);
    } else {
        process.stderr.write(`[Main] Using specified MIDlet class: ${midletClass}\n`);
    }

    // Convert dot notation to slash notation (com.example.Foo → com/example/Foo)
    const className = midletClass.replace(/\./g, '/');

    process.stderr.write(`[Main] Loading class '${className}'...\n`);
    const mainClass = jvm_load_class(g_jvm, className);
    if (!mainClass) {
        process.stderr.write(`[Main] Failed to load class: ${className}\n`);
        return false;
    }
    process.stderr.write(`[Main] Class loaded: ${mainClass.class_name || '(unnamed)'} (${mainClass.methods_count} methods)\n`);

    // Execute MIDlet
    process.stderr.write('[Main] Starting MIDlet execution...\n');
    const result = jvm_run_midlet(g_jvm, mainClass);

    if (result !== 0) {
        process.stderr.write(`[Main] MIDlet execution failed with code ${result}\n`);

        // Report exception info if available
        const mainThread = jvm_current_thread(g_jvm);
        if (mainThread && mainThread.pending_exception) {
            const exc     = mainThread.pending_exception;
            const excClass = exc.header ? exc.header.clazz : null;
            const excName  = excClass ? (excClass.class_name || 'Unknown') : 'Unknown';
            sdl_set_error_info(excName, null, mainThread.exception_stack_trace || null);
        } else {
            sdl_set_error_info('Execution Failed', 'MIDlet returned error code', null);
        }
    } else {
        process.stderr.write('[Main] MIDlet started successfully\n');
    }

    // Force running flags
    if (g_sdl_ctx && !g_sdl_ctx.running) {
        process.stderr.write('[Main] WARNING: g_sdl_ctx.running was false, forcing to true\n');
        g_sdl_ctx.running = true;
    }
    if (g_jvm && !g_jvm.running) {
        process.stderr.write('[Main] WARNING: g_jvm.running was false, forcing to true\n');
        g_jvm.running = true;
    }

    if (!g_sdl_ctx || !g_sdl_ctx.target_fps) {
        process.stderr.write('[Main] Context invalid, cannot run main loop\n');
        return false;
    }

    process.stderr.write('[Main] Entering main event loop...\n');
    await sdl_run(g_sdl_ctx);

    process.stderr.write('=== MIDlet finished ===\n');
    return true;
}

// ─────────────────────────────────────────────────────────────
// cleanup — mirrors C cleanup()
// ─────────────────────────────────────────────────────────────
function cleanup() {
    if (g_sdl_ctx) {
        sdl_destroy(g_sdl_ctx);
        g_sdl_ctx = null;
    }
    if (g_jvm) {
        jvm_destroy(g_jvm);
        g_jvm = null;
    }
}

// ─────────────────────────────────────────────────────────────
// main — top-level entry point (mirrors int main(argc, argv))
// ─────────────────────────────────────────────────────────────
export async function main(argv) {
    process.stdout.write(`=== J2ME Emulator v${J2ME_EMULATOR_VERSION} ===\n`);
    process.stdout.write('Platform: Node.js\n');

    const opts = parse_args(argv);
    if (!opts) {
        print_usage(argv[0] || 'j2me.mjs');
        return 1;
    }

    process.stderr.write(`[Main] JAR file: ${opts.jar_file}\n`);
    process.stderr.write(`[Main] MIDlet class: ${opts.midlet_class || '(auto-detect)'}\n`);

    // Check JAR file exists
    if (!existsSync(opts.jar_file)) {
        process.stderr.write(`[Main] JAR file not found: ${opts.jar_file}\n`);
        return 1;
    }

    // Banner
    process.stdout.write('\n');
    process.stdout.write('╔═══════════════════════════════════════════════════════════╗\n');
    process.stdout.write('║           J2ME Emulator - MIDP2 Mobile Java               ║\n');
    process.stdout.write(`║                    Version ${J2ME_EMULATOR_VERSION}                          ║\n`);
    process.stdout.write('╠═══════════════════════════════════════════════════════════╣\n');
    process.stdout.write('║  Node.js ESM  │  MIDP2 API  │  Full Opcode Support        ║\n');
    process.stdout.write('╚═══════════════════════════════════════════════════════════╝\n');
    process.stdout.write('\n');

    // Signal handlers (mirrors C signal(SIGINT/SIGTERM, ...))
    process.on('SIGINT', () => {
        process.stderr.write('[Main] SIGINT received, shutting down...\n');
        if (g_jvm)     g_jvm.running     = false;
        if (g_sdl_ctx) g_sdl_ctx.running = false;
    });
    process.on('SIGTERM', () => {
        process.stderr.write('[Main] SIGTERM received, shutting down...\n');
        if (g_jvm)     g_jvm.running     = false;
        if (g_sdl_ctx) g_sdl_ctx.running = false;
    });

    if (!await init_emulator(opts)) {
        process.stderr.write('[Main] Failed to initialize emulator\n');
        cleanup();
        return 1;
    }

    const success = await run_midlet(opts);

    cleanup();

    process.stderr.write(`=== J2ME Emulator Exiting (success: ${success ? 1 : 0}) ===\n`);
    return success ? 0 : 1;
}

// ─────────────────────────────────────────────────────────────
// ESM entry point guard
// ─────────────────────────────────────────────────────────────
if (process.argv[1] === fileURLToPath(import.meta.url)) {
    main(process.argv.slice(2)).then(code => {
        process.exitCode = code;
    }).catch(err => {
        process.stderr.write(`[Main] Unhandled error: ${err.stack || err}\n`);
        process.exitCode = 1;
    });
}
