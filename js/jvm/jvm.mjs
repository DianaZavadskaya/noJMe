import { readFileSync } from 'fs';
import { inflateRawSync } from 'zlib';

import {
    method_cache_init,
    method_cache_cleanup,
    method_cache_lookup,
    method_cache_store,
    method_cache_stats,
    class_hash_add,
    class_hash_lookup,
    class_hash_remove,
    class_hash_clear,
    jvm_resolve_method_fast,
    jvm_find_class_fast,
} from './method_cache.mjs';

import {
    heap_init,
    heap_destroy,
    heap_alloc,
    heap_alloc_object,
    heap_alloc_array,
    heap_alloc_string,
    gc_collect,
    gc_add_root,
    gc_remove_root,
    OBJ_TYPE_OBJECT,
    OBJ_TYPE_ARRAY,
    OBJ_TYPE_STRING,
    T_CHAR,
} from './heap.mjs';

import {
    threads_init,
    thread_create,
    thread_destroy,
    thread_current,
    cleanup_monitors,
    THREAD_PRIORITY_NORM,
    JNI_OK,
    JNI_ERR,
} from './threads.mjs';

// ─────────────────────────────────────────────────────────────
// Constants (mirrors jvm.h #define)
// ─────────────────────────────────────────────────────────────
export const HEAP_INITIAL_SIZE = 16 * 1024 * 1024;
export const HEAP_MAX_SIZE     = 64 * 1024 * 1024;
export const JAVA_STACK_SIZE   = 256 * 1024;
export const MAX_JAVA_THREADS  = 16;

export { JNI_OK, JNI_ERR };

// ─────────────────────────────────────────────────────────────
// Injected callbacks — break circular deps with classfile/native/stubs
// ─────────────────────────────────────────────────────────────

let _classfile_parse      = null;  // (jvm, data) => JavaClass | null
let _classfile_parse_file = null;  // (jvm, path) => JavaClass | null
let _classfile_free       = null;  // (clazz) => void
let _classfile_get_class_name = null; // (clazz, idx) => string

let _native_get_string_value_slot  = (_jvm) => -1;
let _native_get_string_offset_slot = (_jvm) => -1;
let _native_get_string_count_slot  = (_jvm) => -1;
let _native_get_string_hash_slot   = (_jvm) => -1;
let _native_cleanup_fallbacks      = () => {};
let _init_stub_classes             = (_jvm) => 0;
let _get_or_create_stub_class      = (_jvm, _name) => null;

// Global JVM pointer used by object_instance_of (mirrors g_jvm_for_instanceof)
export let g_jvm_for_instanceof = null;

/**
 * Inject classfile module functions (to avoid circular import classfile → jvm → classfile).
 */
export function set_classfile_callbacks(cbs) {
    if (cbs.parse)         _classfile_parse          = cbs.parse;
    if (cbs.parseFile)     _classfile_parse_file     = cbs.parseFile;
    if (cbs.free)          _classfile_free           = cbs.free;
    if (cbs.getClassName)  _classfile_get_class_name = cbs.getClassName;
}

/**
 * Inject native module functions (to avoid circular import native → jvm → native).
 */
export function set_native_callbacks(cbs) {
    if (cbs.getStringValueSlot)  _native_get_string_value_slot  = cbs.getStringValueSlot;
    if (cbs.getStringOffsetSlot) _native_get_string_offset_slot = cbs.getStringOffsetSlot;
    if (cbs.getStringCountSlot)  _native_get_string_count_slot  = cbs.getStringCountSlot;
    if (cbs.getStringHashSlot)   _native_get_string_hash_slot   = cbs.getStringHashSlot;
    if (cbs.cleanupFallbacks)    _native_cleanup_fallbacks      = cbs.cleanupFallbacks;
    if (cbs.initStubClasses)     _init_stub_classes             = cbs.initStubClasses;
    if (cbs.getOrCreateStub)     _get_or_create_stub_class      = cbs.getOrCreateStub;
}

// ─────────────────────────────────────────────────────────────
// ZIP / JAR helper utilities
// Little-endian reads matching the C helper read_u16_jar / read_u32_jar
// ─────────────────────────────────────────────────────────────

function read_u16_jar(buf, offset) {
    return buf[offset] | (buf[offset + 1] << 8);
}

function read_u32_jar(buf, offset) {
    return (buf[offset]
        | (buf[offset + 1] << 8)
        | (buf[offset + 2] << 16)
        | (buf[offset + 3] << 24)) >>> 0;
}

/**
 * Find and extract a file from raw JAR/ZIP bytes using the Central Directory.
 * Returns a Buffer with the uncompressed data, or null.
 *
 * @param {Uint8Array|Buffer} jarData
 * @param {string} filename
 * @returns {Buffer|null}
 */
function jar_find_file_internal(jarData, filename) {
    const jarSize = jarData.length;
    if (jarSize < 22) return null;

    // Scan backwards for End-of-Central-Directory signature (PK\x05\x06)
    let eocdOffset = -1;
    for (let i = jarSize - 22; i >= 0; i--) {
        if (jarData[i]     === 0x50 && jarData[i + 1] === 0x4B &&
            jarData[i + 2] === 0x05 && jarData[i + 3] === 0x06) {
            eocdOffset = i;
            break;
        }
    }
    if (eocdOffset < 0) return null;

    const cdStart   = read_u32_jar(jarData, eocdOffset + 16);
    const cdEntries = read_u16_jar(jarData, eocdOffset + 10);

    let cdeOffset = cdStart;
    for (let i = 0; i < cdEntries && cdeOffset < eocdOffset; i++) {
        const sig = read_u32_jar(jarData, cdeOffset);
        if (sig !== 0x02014B50) break;

        const compression    = read_u16_jar(jarData, cdeOffset + 10);
        const compSize       = read_u32_jar(jarData, cdeOffset + 20);
        const uncompSize     = read_u32_jar(jarData, cdeOffset + 24);
        const filenameLen    = read_u16_jar(jarData, cdeOffset + 28);
        const extraLen       = read_u16_jar(jarData, cdeOffset + 30);
        const commentLen     = read_u16_jar(jarData, cdeOffset + 32);
        const localHdrOffset = read_u32_jar(jarData, cdeOffset + 42);

        const entryName = Buffer.from(jarData.buffer ?? jarData,
            (jarData.byteOffset ?? 0) + cdeOffset + 46, filenameLen).toString('utf8');
        const isDir = filenameLen > 0 && entryName[filenameLen - 1] === '/';

        if (!isDir && entryName === filename) {
            const localFnLen    = read_u16_jar(jarData, localHdrOffset + 26);
            const localExtraLen = read_u16_jar(jarData, localHdrOffset + 28);
            const dataOffset    = localHdrOffset + 30 + localFnLen + localExtraLen;

            if (compression === 0) {
                // Stored (no compression)
                return Buffer.from(jarData.buffer ?? jarData,
                    (jarData.byteOffset ?? 0) + dataOffset, uncompSize);
            } else if (compression === 8) {
                // Deflate — use Node.js built-in zlib (raw deflate = no zlib header)
                if (compSize === 0 || uncompSize === 0) return null;
                const compressed = Buffer.from(jarData.buffer ?? jarData,
                    (jarData.byteOffset ?? 0) + dataOffset, compSize);
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
// jvm_recalculate_instance_size
// Mirrors the C function of the same name declared in jvm.h.
// instance_size = sizeof(ObjectHeader) + total_field_slots * sizeof(JavaValue)
// C constants: sizeof(ObjectHeader) = 16, sizeof(JavaValue) = 8
// ─────────────────────────────────────────────────────────────
const SIZEOF_OBJECT_HEADER = 16;
const SIZEOF_JAVA_VALUE    = 8;

export function jvm_recalculate_instance_size(jvm, clazz) {
    if (!clazz) return;

    // Collect all fields including superclass chain
    let totalFields = 0;
    let cur = clazz;
    while (cur) {
        if (cur.fields && cur.fields_count > 0) {
            for (let i = 0; i < cur.fields_count; i++) {
                const f = cur.fields[i];
                // Skip static fields — they are not part of instance layout
                if (f && (f.access_flags & 0x0008) === 0) {
                    totalFields++;
                }
            }
        }
        cur = cur.super_class || null;
        if (cur === clazz) break; // guard against cycles
    }

    clazz.instance_size = SIZEOF_OBJECT_HEADER + totalFields * SIZEOF_JAVA_VALUE;
    clazz.field_count   = totalFields;
}

// ─────────────────────────────────────────────────────────────
// jvm_load_class_from_jar
// ─────────────────────────────────────────────────────────────

export function jvm_load_class_from_jar(jvm, class_name) {
    if (!jvm || !class_name) return null;
    if (!jvm.class_loader.jar_data || jvm.class_loader.jar_size === 0) return null;

    const filename = `${class_name}.class`;

    const classData = jar_find_file_internal(jvm.class_loader.jar_data, filename);
    if (!classData) {
        process.stderr.write(
            `[JAR-DBG] Class '${class_name}' (file='${filename}') NOT found in JAR (jar_size=${jvm.class_loader.jar_size})\n`
        );
        return null;
    }
    process.stderr.write(
        `[JAR-DBG] Class '${class_name}' found in JAR, size=${classData.length}, parsing...\n`
    );

    if (!_classfile_parse) {
        process.stderr.write('[JVM] classfile_parse not injected\n');
        return null;
    }

    const clazz = _classfile_parse(jvm, classData, classData.length);
    if (!clazz) {
        process.stderr.write(`[JAR-DBG] Class '${class_name}': classfile_parse FAILED\n`);
        return null;
    }
    process.stderr.write(
        `[JAR-DBG] Class '${class_name}': parsed OK, methods_count=${clazz.methods_count}, fields_count=${clazz.fields_count}\n`
    );

    // Resolve super_class reference
    if (!clazz.super_class && clazz.super_class_name) {
        const super_ = jvm_load_class(jvm, clazz.super_class_name);
        if (super_) {
            clazz.super_class = super_;
        } else {
            process.stderr.write(
                `[JVM] Failed to resolve super_class ${clazz.super_class_name} for ${clazz.class_name}\n`
            );
        }
    }

    jvm_recalculate_instance_size(jvm, clazz);

    // Add to loaded classes array
    _class_loader_add(jvm, clazz);

    // Add to hash table
    class_hash_add(clazz);

    // Set header.clazz to java/lang/Class if available
    const classClass = _find_loaded_class(jvm, 'java/lang/Class');
    if (classClass) {
        if (!clazz.header) clazz.header = {};
        clazz.header.clazz    = classClass;
        clazz.header.hashcode = _mix_hash(clazz);
    }

    if (jvm.config.verbose_class) {
        const superName = clazz.super_class
            ? clazz.super_class.class_name
            : (clazz.super_class_name || '(none)');
        process.stderr.write(`[JVM] Loaded class from JAR: ${class_name} (super: ${superName})\n`);
    }

    // Extra debug logging for Canvas subclasses (mirrors C behaviour)
    if (clazz.class_name && clazz.class_name.includes('Canvas')) {
        process.stderr.write(
            `[JVM] Canvas Class ${clazz.class_name} methods (${clazz.methods_count} total):\n`
        );
        if (clazz.methods) {
            for (let i = 0; i < clazz.methods_count; i++) {
                const m = clazz.methods[i];
                process.stderr.write(
                    `  [${i}] ${m.name}${m.descriptor} (code: ${m.code ? m.code.code_length : 0} bytes, native: ${m.is_native ? 'yes' : 'no'})\n`
                );
            }
        }
    }

    return clazz;
}

// ─────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────

function _find_loaded_class(jvm, name) {
    const loader = jvm.class_loader;
    for (let i = 0; i < loader.count; i++) {
        const c = loader.classes[i];
        if (c && c.class_name === name) return c;
    }
    return null;
}

function _class_loader_add(jvm, clazz) {
    const loader = jvm.class_loader;
    if (loader.count >= loader.capacity) {
        let newCap = loader.capacity * 2;
        if (newCap > 100000) {
            process.stderr.write('[JVM] Class loader capacity limit reached\n');
            return false;
        }
        loader.capacity = newCap;
    }
    loader.classes[loader.count++] = clazz;
    return true;
}

function _mix_hash(obj) {
    // Mirrors C: (jint)(uintptr_t)clazz ^ 0x5A5A5A5A
    // Use a counter-based surrogate since JS has no raw pointers.
    _mix_hash._counter = ((_mix_hash._counter || 0) + 1) & 0xFFFFFFFF;
    return (_mix_hash._counter ^ 0x5A5A5A5A) | 0;
}

// ─────────────────────────────────────────────────────────────
// jvm_create
// ─────────────────────────────────────────────────────────────

export function jvm_create() {
    const jvm = {
        config: {
            heap_size:    HEAP_INITIAL_SIZE,
            stack_size:   JAVA_STACK_SIZE,
            max_threads:  MAX_JAVA_THREADS,
            verbose_gc:   false,
            verbose_class: false,
            verbose_jni:  false,
        },
        running:      false,
        exiting:      false,
        exit_code:    0,
        main_thread:  null,
        threads:      new Array(MAX_JAVA_THREADS).fill(null),
        thread_count: 0,
        class_loader: {
            classes:          [],
            count:            0,
            capacity:         256,
            class_path:       [],
            class_path_count: 0,
            jar_data:         null,
            jar_size:         0,
            jar_path:         null,
        },
        gc: {
            heap_start:      null,
            heap_end:        null,
            heap_ptr:        null,
            heap_size:       0,
            total_allocated: 0,
            total_freed:     0,
            gc_cycles:       0,
            roots:           [],
            roots_count:     0,
            roots_capacity:  0,
        },
        string_pool: {
            strings:  [],
            count:    0,
            capacity: 256,
        },
        midp: {
            display:  null,
            graphics: null,
            rms:      null,
            game:     null,
        },
        callbacks: {
            on_paint:   null,
            on_key:     null,
            on_pointer: null,
            user_data:  null,
        },
    };
    return jvm;
}

// ─────────────────────────────────────────────────────────────
// jvm_destroy
// ─────────────────────────────────────────────────────────────

export function jvm_destroy(jvm) {
    if (!jvm) return;

    jvm.running = false;

    method_cache_cleanup();

    for (let i = 0; i < jvm.thread_count; i++) {
        if (jvm.threads[i]) {
            thread_destroy(jvm, jvm.threads[i]);
        }
    }

    // In JS the GC handles all object memory; we only clear references.
    jvm.class_loader.classes  = [];
    jvm.class_loader.count    = 0;
    jvm.class_loader.jar_data = null;
    jvm.class_loader.jar_path = null;

    jvm.string_pool.strings = [];
    jvm.string_pool.count   = 0;

    heap_destroy(jvm);

    _native_cleanup_fallbacks();
    cleanup_monitors();
}

// ─────────────────────────────────────────────────────────────
// jvm_init
// ─────────────────────────────────────────────────────────────

export function jvm_init(jvm) {
    if (!jvm) return JNI_ERR;

    g_jvm_for_instanceof = jvm;

    method_cache_init();

    if (heap_init(jvm, jvm.config.heap_size, HEAP_MAX_SIZE) !== JNI_OK) {
        process.stderr.write('[JVM] Failed to initialize heap\n');
        return JNI_ERR;
    }

    if (threads_init(jvm) !== JNI_OK) {
        process.stderr.write('[JVM] Failed to initialize threads\n');
        return JNI_ERR;
    }

    jvm.main_thread = thread_create(jvm, 'main', THREAD_PRIORITY_NORM, null);
    if (!jvm.main_thread) {
        process.stderr.write('[JVM] Failed to create main thread\n');
        return JNI_ERR;
    }

    jvm.threads[0]    = jvm.main_thread;
    jvm.thread_count  = 1;

    // class_loader is pre-initialised by jvm_create; capacity already 256
    jvm.class_loader.classes  = new Array(jvm.class_loader.capacity).fill(null);
    jvm.class_loader.count    = 0;

    jvm.string_pool.strings   = new Array(jvm.string_pool.capacity).fill(null);
    jvm.string_pool.count     = 0;

    const stubCount = _init_stub_classes(jvm);
    if (jvm.config.verbose_class) {
        process.stdout.write(`[JVM] Initialized ${stubCount} stub classes\n`);
    }

    jvm.running = true;
    return JNI_OK;
}

// ─────────────────────────────────────────────────────────────
// jvm_set_jar_data
// ─────────────────────────────────────────────────────────────

export function jvm_set_jar_data(jvm, data, size, path) {
    if (!jvm) return;
    jvm.class_loader.jar_data = data;
    jvm.class_loader.jar_size = size;
    jvm.class_loader.jar_path = path || null;
}

// ─────────────────────────────────────────────────────────────
// jvm_load_class  (OPTIMISED with hash table — mirrors C)
// ─────────────────────────────────────────────────────────────

let _dbg_count = 0; // mirrors static int dbg_count in C

export function jvm_load_class(jvm, class_name) {
    if (!jvm || !class_name) return null;

    // Step 1: hash table lookup O(1)
    let clazz = class_hash_lookup(class_name);
    if (clazz) {
        // Throttled debug log for specific key game classes
        if ((class_name === 'ap' || class_name === 'w' ||
             class_name === 't'  || class_name === 'as') && _dbg_count++ < 5) {
            process.stderr.write(
                `[CLASS-DBG] '${class_name}' found in hash table (methods=${clazz.methods_count}, fields=${clazz.fields_count}, is_stub=${clazz.is_stub ? 1 : 0})\n`
            );
        }
        // Lazily resolve super_class
        if (!clazz.super_class && clazz.super_class_name) {
            clazz.super_class = jvm_load_class(jvm, clazz.super_class_name);
        }
        return clazz;
    }

    // Step 2: linear scan fallback (should rarely happen)
    const loader = jvm.class_loader;
    for (let i = 0; i < loader.count; i++) {
        const c = loader.classes[i];
        if (c && c.class_name === class_name) {
            class_hash_add(c);
            return c;
        }
    }

    // Step 3: try JAR
    if (loader.jar_data && loader.jar_size > 0) {
        clazz = jvm_load_class_from_jar(jvm, class_name);
        if (clazz) return clazz;
    }

    // Step 4: try filesystem
    if (_classfile_parse_file) {
        const filename = `${class_name_to_internal(class_name)}.class`;
        clazz = _classfile_parse_file(jvm, filename);
    }

    if (!clazz) {
        // Step 5: fallback to stub
        process.stderr.write(
            `[CLASS-DBG] Class '${class_name}' not found in JAR or filesystem, creating stub\n`
        );
        clazz = _get_or_create_stub_class(jvm, class_name);
        if (!clazz) {
            process.stderr.write(`[JVM] Class not found and stub creation failed: ${class_name}\n`);
            return null;
        }
        return clazz; // stub already added to class loader by get_or_create_stub_class
    }

    if (!_class_loader_add(jvm, clazz)) {
        return null;
    }

    class_hash_add(clazz);

    if (jvm.config.verbose_class) {
        process.stderr.write(`[JVM] Loaded class: ${class_name}\n`);
    }

    return clazz;
}

// ─────────────────────────────────────────────────────────────
// jvm_define_class
// ─────────────────────────────────────────────────────────────

export function jvm_define_class(jvm, data, length) {
    if (!jvm || !data || length === 0) return null;
    if (!_classfile_parse) return null;

    const clazz = _classfile_parse(jvm, data, length);
    if (!clazz) return null;

    // Resolve class name from constant pool
    if (clazz.this_class > 0 && _classfile_get_class_name) {
        clazz.class_name = _classfile_get_class_name(clazz, clazz.this_class);
    }

    if (!_class_loader_add(jvm, clazz)) {
        // In JS there is no classfile_free to call; GC handles it.
        return null;
    }

    jvm_recalculate_instance_size(jvm, clazz);
    return clazz;
}

// ─────────────────────────────────────────────────────────────
// jvm_resolve_method
// ─────────────────────────────────────────────────────────────

export function jvm_resolve_method(jvm, clazz, name, descriptor) {
    if (!clazz || !name || !descriptor) return null;

    const cached = method_cache_lookup(clazz, name, descriptor);
    if (cached) return cached;

    // Linear search in class methods
    if (clazz.methods) {
        for (let i = 0; i < clazz.methods_count; i++) {
            const m = clazz.methods[i];
            if (m && m.name === name && m.descriptor === descriptor) {
                method_cache_store(clazz, name, descriptor, m);
                return m;
            }
        }
    }

    // Recurse into superclass
    if (clazz.super_class) {
        const method = jvm_resolve_method(jvm, clazz.super_class, name, descriptor);
        if (method) {
            method_cache_store(clazz, name, descriptor, method);
        }
        return method;
    }

    return null;
}

// ─────────────────────────────────────────────────────────────
// jvm_resolve_field
// ─────────────────────────────────────────────────────────────

export function jvm_resolve_field(jvm, clazz, name, descriptor) {
    if (!clazz || !name || !descriptor) return null;

    if (clazz.fields) {
        for (let i = 0; i < clazz.fields_count; i++) {
            const f = clazz.fields[i];
            if (f && f.name === name && f.descriptor === descriptor) {
                return f;
            }
        }
    }

    if (clazz.super_class) {
        return jvm_resolve_field(jvm, clazz.super_class, name, descriptor);
    }

    return null;
}

// ─────────────────────────────────────────────────────────────
// jvm_new_object
// ─────────────────────────────────────────────────────────────

export function jvm_new_object(jvm, clazz) {
    if (!jvm || !clazz) return null;

    if (!clazz.initialized && !clazz.initializing) {
        jvm_recalculate_instance_size(jvm, clazz);
        // Do NOT set initialized here — <clinit> must still run.
    }

    if (jvm.config.verbose_class) {
        process.stdout.write(`[JVM] Creating object of class: ${clazz.class_name}\n`);
    }

    return heap_alloc_object(jvm, clazz);
}

// ─────────────────────────────────────────────────────────────
// jvm_new_array
// ─────────────────────────────────────────────────────────────

export function jvm_new_array(jvm, type, length, element_class) {
    return heap_alloc_array(jvm, type, length, element_class);
}

// ─────────────────────────────────────────────────────────────
// jvm_new_string  (UTF-8 → JavaString)
// ─────────────────────────────────────────────────────────────

export function jvm_new_string(jvm, utf8) {
    if (!jvm || utf8 == null) return null;

    // Decode UTF-8 string to UTF-16 code units (same algorithm as C)
    const chars = utf8_to_utf16(utf8);

    const str = jvm_new_string_utf16(jvm, chars, chars.length);

    // Cache utf8 only for native strings (clazz === null means native string).
    if (str && (!str.header || !str.header.clazz)) {
        str.utf8 = utf8;
    }

    return str;
}

/**
 * Decode a UTF-8 JS string (or a Uint8Array of UTF-8 bytes) to a Uint16Array
 * of UTF-16 code units, matching the C manual decoder in jvm.c.
 *
 * @param {string|Uint8Array} input
 * @returns {Uint16Array}
 */
function utf8_to_utf16(input) {
    if (typeof input === 'string') {
        // JS strings are already UTF-16 — convert using charCodeAt
        const out = new Uint16Array(input.length);
        for (let i = 0; i < input.length; i++) {
            out[i] = input.charCodeAt(i);
        }
        return out;
    }

    // Uint8Array path (raw bytes) — mirrors C manual UTF-8 decoder
    const bytes = input;
    const len   = bytes.length;
    const out   = [];
    let i = 0;

    while (i < len) {
        const c = bytes[i];
        if (c < 0x80) {
            out.push(c);
            i++;
        } else if ((c & 0xE0) === 0xC0) {
            if (i + 1 < len) {
                const cp = ((c & 0x1F) << 6) | (bytes[i + 1] & 0x3F);
                out.push(cp);
                i += 2;
            } else { i++; }
        } else if ((c & 0xF0) === 0xE0) {
            if (i + 2 < len) {
                const cp = ((c & 0x0F) << 12) | ((bytes[i + 1] & 0x3F) << 6) | (bytes[i + 2] & 0x3F);
                out.push(cp);
                i += 3;
            } else { i++; }
        } else if ((c & 0xF8) === 0xF0) {
            if (i + 3 < len) {
                let cp = ((c & 0x07) << 18) | ((bytes[i + 1] & 0x3F) << 12)
                       | ((bytes[i + 2] & 0x3F) << 6) | (bytes[i + 3] & 0x3F);
                cp -= 0x10000;
                out.push(0xD800 | (cp >> 10));
                out.push(0xDC00 | (cp & 0x3FF));
                i += 4;
            } else { i++; }
        } else {
            i++;
        }
    }

    return new Uint16Array(out);
}

// ─────────────────────────────────────────────────────────────
// jvm_new_string_utf16
//
// CRITICAL: If java/lang/String is loaded, create a proper Java String
// object (OBJ_TYPE_OBJECT) with fields[] for value/offset/count/hash,
// exactly matching the C dual-path logic.
// ─────────────────────────────────────────────────────────────

export function jvm_new_string_utf16(jvm, chars, length) {
    if (!jvm || !chars || length < 0) return null;

    const strClass = _find_loaded_class(jvm, 'java/lang/String');

    if (strClass) {
        // Java String: allocate as OBJ_TYPE_OBJECT with fields[]
        const str = heap_alloc(jvm, strClass.instance_size, strClass, OBJ_TYPE_OBJECT);
        if (!str) return null;

        // Create char[] array for the value field
        const charArray = heap_alloc_array(jvm, T_CHAR, length, null);
        if (!charArray) return null;

        // Copy char data into the array
        if (!charArray.data) charArray.data = new Uint16Array(length);
        for (let i = 0; i < length; i++) {
            charArray.data[i] = chars[i];
        }

        // Set string fields via injected accessors
        const valueSlot  = _native_get_string_value_slot(jvm);
        const offsetSlot = _native_get_string_offset_slot(jvm);
        const countSlot  = _native_get_string_count_slot(jvm);
        const hashSlot   = _native_get_string_hash_slot(jvm);

        if (!str.fields) str.fields = [];

        if (valueSlot  >= 0) str.fields[valueSlot]  = { ref: charArray };
        if (offsetSlot >= 0) str.fields[offsetSlot] = { i: 0 };
        if (countSlot  >= 0) str.fields[countSlot]  = { i: length };
        if (hashSlot   >= 0) str.fields[hashSlot]   = { i: 0 };

        return str;
    } else {
        // Native string: inline storage
        const str = heap_alloc_string(jvm, length);
        if (!str) return null;

        // Store UTF-16 data inline
        str.length = length;
        str.chars  = null; // inline — access via str.data
        str.data   = new Uint16Array(length);
        for (let i = 0; i < length; i++) {
            str.data[i] = chars[i];
        }

        return str;
    }
}

// ─────────────────────────────────────────────────────────────
// Exception helpers
// ─────────────────────────────────────────────────────────────

export function jvm_throw(jvm, exception) {
    if (!jvm || !exception) return;
    const thread = jvm_current_thread(jvm);
    if (thread) {
        thread.pending_exception = exception;
    }
}

export function jvm_throw_by_name(jvm, class_name, message) {
    if (!jvm || !class_name) return;

    const clazz = jvm_load_class(jvm, class_name);
    if (!clazz) {
        process.stderr.write(`[JVM] Exception class not found: ${class_name}\n`);
        return;
    }

    const exception = jvm_new_object(jvm, clazz);
    if (!exception) return;

    // message is noted but not stored — mirrors the C TODO
    jvm_throw(jvm, exception);
}

export function jvm_exception_pending(jvm) {
    const thread = jvm_current_thread(jvm);
    return thread ? thread.pending_exception : null;
}

export function jvm_exception_clear(jvm) {
    const thread = jvm_current_thread(jvm);
    if (!thread) return null;
    const ex = thread.pending_exception;
    thread.pending_exception = null;
    return ex;
}

// ─────────────────────────────────────────────────────────────
// Thread management
// ─────────────────────────────────────────────────────────────

export function jvm_current_thread(jvm) {
    if (!jvm) return null;
    return thread_current(jvm);
}

// ─────────────────────────────────────────────────────────────
// Memory / GC API (thin wrappers that preserve the C public API)
// ─────────────────────────────────────────────────────────────

export function jvm_alloc(jvm, size) {
    return heap_alloc(jvm, size, null, OBJ_TYPE_OBJECT);
}

export function jvm_gc(jvm) {
    gc_collect(jvm);
}

export function jvm_add_root(jvm, obj) {
    gc_add_root(jvm, obj);
}

export function jvm_remove_root(jvm, obj) {
    gc_remove_root(jvm, obj);
}

// ─────────────────────────────────────────────────────────────
// jvm_string_to_utf8
// ─────────────────────────────────────────────────────────────

export function jvm_string_to_utf8(jvm, str) {
    if (!str) return null;

    if (str.utf8) return str.utf8;

    // Native string: data is stored in str.data (Uint16Array)
    const src = str.data;
    if (!src) return null;

    const len    = str.length || src.length;
    let result = '';
    for (let i = 0; i < len; i++) {
        result += String.fromCharCode(src[i] & 0xFF);
    }
    return result;
}

// ─────────────────────────────────────────────────────────────
// Byte-order read utilities (big-endian, as used in class files)
// ─────────────────────────────────────────────────────────────

export function jvm_read_u16(data) {
    return ((data[0] << 8) | data[1]) >>> 0;
}

export function jvm_read_u32(data) {
    return (((data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3]) >>> 0);
}

export function jvm_read_u64(data) {
    // Return as BigInt to avoid loss of precision for full 64-bit values.
    const hi = jvm_read_u32(data);
    const lo = jvm_read_u32(data.slice ? data.slice(4) : data.subarray(4));
    return (BigInt(hi) << 32n) | BigInt(lo);
}

export function jvm_read_s16(data) {
    return (jvm_read_u16(data) << 16) >> 16;
}

export function jvm_read_s32(data) {
    return jvm_read_u32(data) | 0;
}

export function jvm_read_s64(data) {
    return BigInt.asIntN(64, jvm_read_u64(data));
}

// ─────────────────────────────────────────────────────────────
// Class name conversion utilities
// ─────────────────────────────────────────────────────────────

export function class_name_to_java(internal_name) {
    if (!internal_name) return null;
    return internal_name.replace(/\//g, '.');
}

export function class_name_to_internal(java_name) {
    if (!java_name) return null;
    return java_name.replace(/\./g, '/');
}

// ─────────────────────────────────────────────────────────────
// Re-export cache/hash functions required by the public API in jvm.h
// ─────────────────────────────────────────────────────────────

export {
    method_cache_init,
    method_cache_cleanup,
    method_cache_lookup,
    method_cache_store,
    method_cache_stats,
    class_hash_add,
    class_hash_lookup,
    class_hash_remove,
    class_hash_clear,
    jvm_resolve_method_fast,
    jvm_find_class_fast,
};
