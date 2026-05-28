/**
 * J2ME Emulator — Heap and Garbage Collector
 * Migrated from src/jvm/heap.c (2801 lines)
 *
 * Translation decisions:
 *  - The C bump-pointer + free-list allocator is replaced by plain JS object
 *    construction. JS GC handles memory reclamation for unreachable JS objects.
 *  - The mark-sweep GC traversal logic is preserved: gc_collect() walks the
 *    live-object set (a WeakRef registry) and marks/sweeps Java-heap graph
 *    references so that stale cross-object references inside JavaObject.fields
 *    are nulled. This mirrors the C behaviour of clearing freed char-array refs.
 *  - GCObjectHeader (magic, size, type, pinned, marked) is stored as a hidden
 *    _gc property on every allocated object. Using a Symbol keeps it off the
 *    normal property enumeration path.
 *  - "is_heap_ptr" checks in C become "is_managed_object" checks against the
 *    WeakRef registry + presence of the _gc Symbol.
 *  - pthread_mutex / CRITICAL_SECTION → omitted (JS is single-threaded).
 *  - thread-local string_utf8 TLS buffer → a plain module-level variable
 *    reused between calls (same semantics: do not store the pointer).
 *  - 0xDEADBEEF magic → Symbol-based header; no corruption possible in JS.
 *  - g_heap_start / g_heap_end → exported as null (no raw memory in JS).
 *  - heap_check_magic → always returns 0 (no raw memory to corrupt).
 *  - DEBUG_HEAP_CORRUPTION log path → omitted (no raw memory).
 *  - __sync_synchronize memory barriers → omitted (single-threaded JS).
 *  - native_get_string_value_slot / native_get_string_count_slot / etc. are
 *    injected via set_native_string_accessors() to avoid circular imports.
 *  - m3g_registry_get_objects / native_gc_notify / hashtable accessors are
 *    injected via set_gc_extension_callbacks().
 *  - intern_pool is injected via set_intern_pool_accessor().
 */

// ─────────────────────────────────────────────────────────────
// Object type enum (mirrors ObjectType in heap.h)
// ─────────────────────────────────────────────────────────────
export const ObjectType = Object.freeze({
    OBJ_TYPE_OBJECT: 0,
    OBJ_TYPE_ARRAY:  1,
    OBJ_TYPE_STRING: 2,
    OBJ_TYPE_CLASS:  3,
    OBJ_TYPE_FREE:   4,
});

export const {
    OBJ_TYPE_OBJECT,
    OBJ_TYPE_ARRAY,
    OBJ_TYPE_STRING,
    OBJ_TYPE_CLASS,
    OBJ_TYPE_FREE,
} = ObjectType;

// ─────────────────────────────────────────────────────────────
// Array element-type codes (from opcodes.h / heap.h usage)
// ─────────────────────────────────────────────────────────────
export const T_BOOLEAN  = 4;
export const T_CHAR     = 5;
export const T_FLOAT    = 6;
export const T_DOUBLE   = 7;
export const T_BYTE     = 8;
export const T_SHORT    = 9;
export const T_INT      = 10;
export const T_LONG     = 11;
export const DESC_OBJECT = 0x4C; // 'L'
export const DESC_ARRAY  = 0x5B; // '['

export const ACC_STATIC = 0x0008;

// ─────────────────────────────────────────────────────────────
// GC header Symbol — hidden metadata on every managed object
// ─────────────────────────────────────────────────────────────
const GC = Symbol('GCObjectHeader');

// ─────────────────────────────────────────────────────────────
// Heap statistics (mirrors HeapStats struct)
// ─────────────────────────────────────────────────────────────
const heap = {
    totalSize:   16 * 1024 * 1024,
    allocated:   0,
    freed:       0,
    gcCycles:    0,
    gcTotalFreed:0,
    // The "object registry" tracks all live managed objects by WeakRef so the
    // sweep phase can null stale cross-references without a raw memory scan.
    objects:     new Set(),    // Set<object>  — strong refs (GC root side)
    roots:       [],           // Array<{ ref: object|null }>
};

// Exported stubs — no raw memory in JS.
export let g_heap_start = null;
export let g_heap_end   = null;

// Global JVM reference stored during heap_init (mirrors g_jvm_for_instanceof).
let g_jvm = null;

// ─────────────────────────────────────────────────────────────
// Injected callbacks (to avoid circular imports)
// ─────────────────────────────────────────────────────────────
let _getNativeStringValueSlot  = (_jvm) => -1;
let _getNativeStringCountSlot  = (_jvm) => -1;
let _getNativeStringHashSlot   = (_jvm) => -1;
let _getNativeStringOffsetSlot = (_jvm) => -1;
let _getNativeFieldValue       = (_obj, _name) => ({ raw: 0n });
let _m3gRegistryGetObjects     = () => null;
let _nativeGcNotify            = () => {};
let _getHashtables             = () => ({ hashtables: [], aliveFlags: [] });
let _hashtableFreePeer         = (_idx) => {};
let _getInternPool             = () => [];
let _gcDebugEnabled            = false;

/**
 * Inject native string field-slot accessors.
 * Call once from native.mjs during initialisation to break the circular
 * import dependency (heap → native → heap).
 */
export function set_native_string_accessors(accessors) {
    if (accessors.getValueSlot)  _getNativeStringValueSlot  = accessors.getValueSlot;
    if (accessors.getCountSlot)  _getNativeStringCountSlot  = accessors.getCountSlot;
    if (accessors.getHashSlot)   _getNativeStringHashSlot   = accessors.getHashSlot;
    if (accessors.getOffsetSlot) _getNativeStringOffsetSlot = accessors.getOffsetSlot;
    if (accessors.getFieldValue) _getNativeFieldValue       = accessors.getFieldValue;
}

/**
 * Inject GC extension callbacks (M3G registry, native Hashtables, GC notify).
 */
export function set_gc_extension_callbacks(cbs) {
    if (cbs.m3gRegistryGetObjects) _m3gRegistryGetObjects = cbs.m3gRegistryGetObjects;
    if (cbs.nativeGcNotify)        _nativeGcNotify        = cbs.nativeGcNotify;
    if (cbs.getHashtables)         _getHashtables         = cbs.getHashtables;
    if (cbs.hashtableFreePeer)     _hashtableFreePeer     = cbs.hashtableFreePeer;
}

/**
 * Inject intern-pool accessor.
 */
export function set_intern_pool_accessor(fn) {
    _getInternPool = fn;
}

// ─────────────────────────────────────────────────────────────
// GC debug logging
// ─────────────────────────────────────────────────────────────
function gcLog(...args) {
    if (_gcDebugEnabled || process.env.J2ME_GC_DEBUG === '1') {
        process.stderr.write('[GC] ' + args.join(' ') + '\n');
    }
}

// ─────────────────────────────────────────────────────────────
// Heap initialisation / destruction
// ─────────────────────────────────────────────────────────────

/**
 * heap_init — initialise the heap bookkeeping.
 * In JS there is no raw memory to allocate; we simply record the JVM reference
 * and reset statistics.
 */
export function heap_init(jvm, initialSize, maxSize) {
    g_jvm = jvm;
    heap.totalSize    = initialSize || (16 * 1024 * 1024);
    heap.allocated    = 0;
    heap.freed        = 0;
    heap.gcCycles     = 0;
    heap.gcTotalFreed = 0;
    heap.objects      = new Set();
    heap.roots        = [];
    return 0; // JNI_OK
}

/**
 * heap_destroy — release all bookkeeping state.
 */
export function heap_destroy(jvm) {
    heap.objects.clear();
    heap.roots  = [];
    heap.allocated = 0;
    g_jvm = null;
}

// ─────────────────────────────────────────────────────────────
// Internal: attach / query GC header on a managed object
// ─────────────────────────────────────────────────────────────
function attachHeader(obj, clazz, type, size) {
    obj[GC] = {
        clazz,
        type,
        size:   size || 0,
        marked: false,
        pinned: false,
    };
    heap.objects.add(obj);
    heap.allocated += (size || 0);
    return obj;
}

function getHeader(obj) {
    return obj ? obj[GC] : null;
}

function isManagedObject(obj) {
    if (obj === null || obj === undefined) return false;
    if (typeof obj !== 'object') return false;
    return obj[GC] !== undefined;
}

// ─────────────────────────────────────────────────────────────
// heap_alloc — base allocator
// In JS we create a plain object with GC metadata.
// The C function's 1 MB size limit is preserved as a sanity guard.
// ─────────────────────────────────────────────────────────────
export function heap_alloc(jvm, size, clazz, type) {
    if (size > 1024 * 1024) {
        process.stderr.write(
            `[HEAP] Suspicious allocation: size=${size} class=${clazz ? clazz.class_name : 'null'}\n`
        );
        return null;
    }

    // Trigger GC heuristic when "allocated" bytes exceed 75 % of declared size.
    if (heap.allocated > heap.totalSize * 0.75 && jvm) {
        gc_collect(jvm);
    }

    const obj = Object.create(null);
    attachHeader(obj, clazz, type, size);

    // Mirror ObjectHeader initialisation for OBJ_TYPE_OBJECT / ARRAY / STRING.
    if (clazz && (type === OBJ_TYPE_OBJECT || type === OBJ_TYPE_STRING || type === OBJ_TYPE_ARRAY)) {
        obj.header = {
            clazz,
            hashcode: _objectIdentityHash(obj),
            gc_mark:  0,
            reserved: 0,
        };
    }

    return obj;
}

// Simple identity-hash counter (mirrors "(jint)(intptr_t)ptr" in C).
let _hashCounter = 1;
function _objectIdentityHash(_obj) {
    return (_hashCounter++ | 0);
}

// ─────────────────────────────────────────────────────────────
// heap_alloc_object
// ─────────────────────────────────────────────────────────────
export function heap_alloc_object(jvm, clazz) {
    if (!clazz) {
        process.stderr.write('[HEAP] heap_alloc_object called with null class\n');
        return null;
    }

    const size = clazz.instance_size || 0;
    const obj = heap_alloc(jvm, Math.max(size, 16), clazz, OBJ_TYPE_OBJECT);
    if (!obj) return null;

    obj.header = {
        clazz,
        hashcode: _objectIdentityHash(obj),
        gc_mark:  0,
        reserved: 0,
    };
    // fields[] — dynamic instance storage.  Use a plain Array (sparse is fine).
    obj.fields = [];

    return obj;
}

// ─────────────────────────────────────────────────────────────
// heap_alloc_array
// ─────────────────────────────────────────────────────────────
export function heap_alloc_array(jvm, elementType, length, elementClass) {
    // Compute element size for the byte-budget tracker.
    let elemSize;
    switch (elementType) {
        case T_BOOLEAN: case T_BYTE:             elemSize = 1; break;
        case T_CHAR:    case T_SHORT:            elemSize = 2; break;
        case T_INT:     case T_FLOAT:            elemSize = 4; break;
        case T_LONG:    case T_DOUBLE:           elemSize = 8; break;
        case DESC_OBJECT: case DESC_ARRAY:       elemSize = 8; break; // pointer-sized
        default:                                 elemSize = 4; break;
    }

    const dataSize  = elemSize * length;
    const totalSize = 32 + dataSize; // sizeof(JavaArray) approximation

    // Find java/lang/Object as the array's own class (mirrors C logic).
    let arrayClass = null;
    if (jvm && jvm.class_loader && jvm.class_loader.classes) {
        for (const c of jvm.class_loader.classes) {
            if (c && c.class_name === 'java/lang/Object') { arrayClass = c; break; }
        }
        if (!arrayClass && jvm.class_loader.classes.length > 0) {
            arrayClass = jvm.class_loader.classes[0];
        }
    }

    const array = heap_alloc(jvm, totalSize, arrayClass, OBJ_TYPE_ARRAY);
    if (!array) return null;

    array.header = {
        clazz:    arrayClass,
        hashcode: _objectIdentityHash(array),
        gc_mark:  0,
        reserved: 0,
    };
    array.length        = length;
    array.element_type  = elementType;
    array.element_class = elementClass || null;
    array._reserved     = new Uint8Array(3);

    // data — the typed storage.  Use typed arrays for primitive types;
    // use a plain Array for reference types (matches C void*[] layout).
    array.data = _makeArrayData(elementType, length);

    return array;
}

function _makeArrayData(elementType, length) {
    switch (elementType) {
        case T_BOOLEAN: case T_BYTE:   return new Int8Array(length);
        case T_CHAR:                   return new Uint16Array(length);
        case T_SHORT:                  return new Int16Array(length);
        case T_INT:                    return new Int32Array(length);
        case T_LONG:                   return new BigInt64Array(length);
        case T_FLOAT:                  return new Float32Array(length);
        case T_DOUBLE:                 return new Float64Array(length);
        case DESC_OBJECT: case DESC_ARRAY:
        default:
            // Reference arrays — plain Array of null/object refs.
            return new Array(length).fill(null);
    }
}

// ─────────────────────────────────────────────────────────────
// heap_alloc_string
// ─────────────────────────────────────────────────────────────
export function heap_alloc_string(jvm, length) {
    // If java/lang/String is loaded, create an OBJ_TYPE_OBJECT (Java String).
    let strClass = null;
    if (jvm && jvm.class_loader && jvm.class_loader.classes) {
        for (const c of jvm.class_loader.classes) {
            if (c && c.class_name === 'java/lang/String') { strClass = c; break; }
        }
    }

    if (strClass) {
        const objSize = strClass.instance_size || 32;
        const str = heap_alloc(jvm, objSize, strClass, OBJ_TYPE_OBJECT);
        if (!str) return null;
        str.header = {
            clazz:    strClass,
            hashcode: _objectIdentityHash(str),
            gc_mark:  0,
            reserved: 0,
        };
        str.fields = [];
        return str;
    }

    // Native string (early boot, before java/lang/String is loaded).
    const dataSize  = length * 2; // jchar = 2 bytes
    const totalSize = 32 + dataSize; // sizeof(JavaString) approximation
    const str = heap_alloc(jvm, totalSize, null, OBJ_TYPE_STRING);
    if (!str) return null;

    str.header = {
        clazz:    null,
        hashcode: _objectIdentityHash(str),
        gc_mark:  0,
        reserved: 0,
    };
    str.length = length;
    str.hash   = 0;
    str.utf8   = null;
    str.chars  = null; // inline storage via str._inlineChars
    str._inlineChars = new Uint16Array(length);

    return str;
}

// ─────────────────────────────────────────────────────────────
// array_data — access inline data (mirrors C macro)
// ─────────────────────────────────────────────────────────────
export function array_data(array) {
    return array ? array.data : null;
}

// ─────────────────────────────────────────────────────────────
// Garbage Collection
// ─────────────────────────────────────────────────────────────

let _gcInProgress = false;

export function gc_collect(jvm) {
    if (!jvm) return;
    if (_gcInProgress) return;
    _gcInProgress = true;

    gcLog('gc_collect() START — objects=' + heap.objects.size);

    // Reset mark bits on all tracked objects.
    for (const obj of heap.objects) {
        const hdr = obj[GC];
        if (hdr) hdr.marked = false;
    }

    // ── 1. MARK PHASE ──────────────────────────────────────────

    const markStack = [];

    function pushMark(ref) {
        if (!isManagedObject(ref)) return;
        const hdr = ref[GC];
        if (!hdr || hdr.type === OBJ_TYPE_FREE || hdr.marked) return;
        hdr.marked = true;
        markStack.push(ref);
    }

    function processMarkStack() {
        while (markStack.length > 0) {
            const ptr = markStack.pop();
            _markObject(ptr, pushMark, jvm);
        }
    }

    // Explicit GC roots.
    for (const root of heap.roots) {
        if (root && root.ref) pushMark(root.ref);
    }

    // Thread stacks and thread objects.
    if (jvm.threads) {
        for (const thread of jvm.threads) {
            if (!thread) continue;
            if (thread.pending_exception) pushMark(thread.pending_exception);
            if (thread.thread_object)     pushMark(thread.thread_object);

            let frame = thread.current_frame;
            while (frame) {
                if (frame.locals) {
                    for (let j = 0; j < frame.max_locals; j++) {
                        const loc = frame.locals[j];
                        if (loc && loc.ref) pushMark(loc.ref);
                    }
                }
                if (frame.stack) {
                    for (let j = 0; j <= frame.stack_top; j++) {
                        const sv = frame.stack[j];
                        if (sv && sv.ref) pushMark(sv.ref);
                    }
                }
                frame = frame.prev;
            }
        }
    }

    // Static fields on all loaded classes.
    if (jvm.class_loader && jvm.class_loader.classes) {
        for (const clazz of jvm.class_loader.classes) {
            if (!clazz || !clazz.static_fields) continue;
            for (const sf of clazz.static_fields) {
                if (!sf || !sf.descriptor) continue;
                const d = sf.descriptor[0];
                if ((d === 'L' || d === '[') && sf.value && sf.value.ref) {
                    pushMark(sf.value.ref);
                }
            }
        }
    }

    // Native Hashtable entries (mirrors C's g_hashtables scan).
    const { hashtables, aliveFlags } = _getHashtables();
    for (let i = 0; i < hashtables.length; i++) {
        if (!aliveFlags[i]) continue;
        const peer = hashtables[i];
        if (!peer || !peer.entries) continue;
        for (const entry of peer.entries) {
            if (!entry) continue;
            if (entry.key)   pushMark(entry.key);
            if (entry.value) pushMark(entry.value);
        }
    }

    // Interned strings.
    const internPool = _getInternPool();
    for (const s of internPool) {
        if (s) pushMark(s);
    }

    // M3G registry.
    const m3gObjects = _m3gRegistryGetObjects();
    if (m3gObjects) {
        for (const obj of m3gObjects) {
            if (obj) pushMark(obj);
        }
    }

    processMarkStack();

    // ── 1.5 PRE-SWEEP: ensure live String objects keep their char arrays ──
    // Mirrors the C pre-sweep validation loop.
    for (const obj of heap.objects) {
        const hdr = obj[GC];
        if (!hdr || !hdr.marked) continue;
        if (hdr.type !== OBJ_TYPE_OBJECT && hdr.type !== OBJ_TYPE_STRING) continue;
        if (!hdr.clazz || hdr.clazz.class_name !== 'java/lang/String') continue;

        const valueSlot = _getNativeStringValueSlot(jvm);
        if (valueSlot >= 0 && obj.fields) {
            const valueRef = obj.fields[valueSlot] && obj.fields[valueSlot].ref;
            if (valueRef && isManagedObject(valueRef)) {
                const arrHdr = valueRef[GC];
                if (arrHdr && !arrHdr.marked && arrHdr.type !== OBJ_TYPE_FREE) {
                    gcLog('PRE-SWEEP: fixing unmarked char array for String');
                    arrHdr.marked = true;
                    markStack.push(valueRef);
                }
            } else if (valueRef && !isManagedObject(valueRef)) {
                // Stale reference — clear it.
                obj.fields[valueSlot] = { ref: null };
            }
        }
    }

    if (markStack.length > 0) processMarkStack();

    // ── 2. SWEEP PHASE ─────────────────────────────────────────
    let freedThisCycle = 0;
    let objectsFreed   = 0;

    for (const obj of Array.from(heap.objects)) {
        const hdr = obj[GC];
        if (!hdr) { heap.objects.delete(obj); continue; }

        if (hdr.type === OBJ_TYPE_FREE) {
            heap.objects.delete(obj);
            continue;
        }

        if (hdr.marked || hdr.pinned) {
            hdr.marked = false; // reset for next cycle
            continue;
        }

        // Dead object — sweep it.
        gcLog(`SWEEP: freeing ${hdr.clazz ? hdr.clazz.class_name : '?'} type=${hdr.type}`);

        // Clean up native string cache.
        if (hdr.type === OBJ_TYPE_STRING) {
            if (obj.utf8 != null) obj.utf8 = null;
        }

        // Clean up native Hashtable peer when the Java Hashtable dies.
        if (hdr.clazz && hdr.clazz.class_name === 'java/util/Hashtable') {
            const peerVal = _getNativeFieldValue(obj, 'threshold');
            const peerIdx = peerVal ? (peerVal.i | 0) : -1;
            if (peerIdx >= 0) {
                _hashtableFreePeer(peerIdx);
            }
        }

        hdr.type = OBJ_TYPE_FREE;
        freedThisCycle += hdr.size;
        objectsFreed++;
        heap.objects.delete(obj);
    }

    heap.allocated    -= freedThisCycle;
    heap.freed        += freedThisCycle;
    heap.gcCycles++;
    heap.gcTotalFreed += freedThisCycle;

    gcLog(`gc_collect() END — freed=${objectsFreed} objects, ${freedThisCycle} bytes, remaining=${heap.objects.size}`);

    _nativeGcNotify();
    _gcInProgress = false;
}

/** Internal: mark reachable references from a single object. */
function _markObject(ptr, pushMark, jvm) {
    if (!ptr || !isManagedObject(ptr)) return;
    const hdr = ptr[GC];
    if (!hdr) return;

    switch (hdr.type) {
        case OBJ_TYPE_OBJECT: {
            const obj   = ptr;
            const clazz = obj.header && obj.header.clazz;
            if (!clazz) break;

            // Special handling: java/lang/String value field.
            if (clazz.class_name === 'java/lang/String') {
                const valueSlot = _getNativeStringValueSlot(jvm || g_jvm);
                if (valueSlot >= 0 && obj.fields) {
                    const vref = obj.fields[valueSlot] && obj.fields[valueSlot].ref;
                    if (vref) {
                        if (isManagedObject(vref)) {
                            pushMark(vref);
                        } else {
                            obj.fields[valueSlot] = { ref: null };
                        }
                    }
                }
            }

            // Special handling: Hashtable / HashMap — scan all field slots.
            if (clazz.class_name === 'java/util/Hashtable' ||
                clazz.class_name === 'java/util/HashMap' ||
                clazz.class_name === 'java/util/Vector'  ||
                clazz.class_name === 'java/util/ArrayList') {
                if (obj.fields) {
                    for (let s = 0; s < obj.fields.length; s++) {
                        const fv = obj.fields[s];
                        if (fv && fv.ref && isManagedObject(fv.ref)) pushMark(fv.ref);
                    }
                }
                // fall-through to normal field scanning below (for completeness)
            }

            // Normal field scanning: walk inheritance chain, push reference fields.
            _markObjectFields(obj, clazz, pushMark);
            break;
        }

        case OBJ_TYPE_ARRAY: {
            const array = ptr;
            if (array.element_type === DESC_OBJECT || array.element_type === DESC_ARRAY) {
                if (array.data) {
                    for (let i = 0; i < array.length; i++) {
                        const ref = array.data[i];
                        if (ref) pushMark(ref);
                    }
                }
            }
            break;
        }

        case OBJ_TYPE_STRING: {
            // Native strings have inline data. If they somehow have a java/lang/String class,
            // scan fields like an object (mirrors C OBJ_TYPE_STRING fixup path).
            if (hdr.clazz && hdr.clazz.class_name === 'java/lang/String') {
                const obj = ptr;
                const valueSlot = _getNativeStringValueSlot(jvm || g_jvm);
                if (valueSlot >= 0 && obj.fields) {
                    const vref = obj.fields[valueSlot] && obj.fields[valueSlot].ref;
                    if (vref) {
                        if (isManagedObject(vref)) {
                            pushMark(vref);
                        } else {
                            obj.fields[valueSlot] = { ref: null };
                        }
                    }
                }
                _markObjectFields(obj, hdr.clazz, pushMark);
            }
            break;
        }

        case OBJ_TYPE_CLASS: {
            // Static fields live in clazz.static_fields, already scanned in the root pass.
            break;
        }
    }
}

/** Walk class hierarchy and push reference-typed instance fields. */
function _markObjectFields(obj, clazz, pushMark) {
    // Build hierarchy: most-derived first, then reverse to Object→leaf.
    const hierarchy = [];
    let c = clazz;
    while (c) { hierarchy.push(c); c = c.super_class; }
    hierarchy.reverse();

    let slot = 0;
    for (const current of hierarchy) {
        if (!current.fields) continue;
        for (const field of current.fields) {
            if (!field) { slot++; continue; }
            if (field.access_flags & ACC_STATIC) continue;

            if (field.descriptor) {
                const d = field.descriptor[0];
                if (d === 'L' || d === '[') {
                    // Skip nativePeer — stores raw malloc pointers, not heap refs.
                    if (field.name !== 'nativePeer') {
                        const fv = obj.fields && obj.fields[slot];
                        if (fv && fv.ref && isManagedObject(fv.ref)) {
                            pushMark(fv.ref);
                        }
                    }
                }
            }
            slot++;
            // long / double occupy 2 slots.
            if (field.descriptor &&
                (field.descriptor[0] === 'J' || field.descriptor[0] === 'D')) {
                slot++;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────
// GC roots
// ─────────────────────────────────────────────────────────────

/**
 * gc_add_root — register a root container { ref: object }.
 * In JS, a "root" is any holder whose .ref property should keep the
 * referenced object alive.  Pass the same holder object to gc_remove_root.
 */
export function gc_add_root(jvm, root) {
    heap.roots.push(root);
}

export function gc_remove_root(jvm, root) {
    const idx = heap.roots.indexOf(root);
    if (idx !== -1) heap.roots.splice(idx, 1);
}

export function gc_pin(jvm, object) {
    if (!object || !isManagedObject(object)) return;
    object[GC].pinned = true;
}

export function gc_unpin(jvm, object) {
    if (!object || !isManagedObject(object)) return;
    object[GC].pinned = false;
}

// ─────────────────────────────────────────────────────────────
// Heap statistics
// ─────────────────────────────────────────────────────────────
export function heap_get_stats(jvm) {
    return {
        total_size:    heap.totalSize,
        used_size:     heap.allocated,
        free_size:     Math.max(0, heap.totalSize - heap.allocated),
        object_count:  heap.objects.size,
        gc_cycles:     heap.gcCycles,
        gc_time_ms:    0,
    };
}

// ─────────────────────────────────────────────────────────────
// Object utilities
// ─────────────────────────────────────────────────────────────
export function object_get_class(object) {
    if (!object || !isManagedObject(object)) return null;
    const hdr = object[GC];
    if (!hdr || hdr.size > 64 * 1024 * 1024) return null;
    if (hdr.type > OBJ_TYPE_CLASS) return null;
    return hdr.clazz;
}

export function object_get_size(object) {
    if (!object || !isManagedObject(object)) return 0;
    return object[GC].size;
}

export function object_hash_code(object) {
    if (!object) return 0;
    const header = object.header;
    return header ? (header.hashcode | 0) : 0;
}

export function object_is_array(object) {
    if (!object || !isManagedObject(object)) return false;
    return object[GC].type === OBJ_TYPE_ARRAY;
}

export function object_is_string(object) {
    if (!object || !isManagedObject(object)) return false;
    return object[GC].type === OBJ_TYPE_STRING;
}

export function object_instance_of(object, clazz) {
    if (!object || !clazz) return false;
    if (!isManagedObject(object)) return false;

    const gc_header = object[GC];
    if (!gc_header) return false;

    const objIsArray = (gc_header.type === OBJ_TYPE_ARRAY);

    if (!clazz.class_name) return false;

    if (objIsArray) {
        if (clazz.class_name === 'java/lang/Object') return true;
        if (clazz.class_name === 'java/lang/Cloneable' ||
            clazz.class_name === 'java/io/Serializable') return true;

        if (clazz.class_name[0] === '[') {
            const array = object;
            const et = array.element_type;

            // Primitive array exact match.
            const primitiveMap = {
                [T_BOOLEAN]: '[Z', [T_BYTE]: '[B', [T_CHAR]: '[C',
                [T_SHORT]:   '[S', [T_INT]:  '[I', [T_LONG]: '[J',
                [T_FLOAT]:   '[F', [T_DOUBLE]:'[D',
            };
            if (primitiveMap[et] !== undefined) {
                return clazz.class_name === primitiveMap[et];
            }

            if (et === DESC_OBJECT || et === DESC_ARRAY) {
                if (array.element_class && array.element_class.class_name) {
                    const expectedName = '[L' + array.element_class.class_name + ';';
                    if (clazz.class_name === expectedName) return true;
                    if (clazz.class_name === '[Ljava/lang/Object;') return true;

                    // Covariance: check element class hierarchy.
                    if (clazz.class_name[0] === '[' && clazz.class_name[1] === 'L') {
                        const targetElemName = clazz.class_name.slice(2).replace(/;$/, '');
                        const targetElemClass = g_jvm ? _findClass(g_jvm, targetElemName) : null;
                        if (targetElemClass && array.element_class) {
                            let check = array.element_class;
                            while (check) {
                                if (check === targetElemClass ||
                                    check.class_name === targetElemClass.class_name) return true;
                                check = check.super_class;
                            }
                        }
                    }
                } else {
                    if (clazz.class_name === '[Ljava/lang/Object;') return true;
                }
            }
        }
        return false;
    }

    // Non-array: walk class hierarchy.
    const objClass = object_get_class(object);
    if (!objClass) return false;

    let current = objClass;
    while (current) {
        if (current === clazz) return true;
        if (current.class_name && clazz.class_name &&
            current.class_name === clazz.class_name) return true;
        current = current.super_class;
    }

    // Check interfaces.
    current = objClass;
    while (current) {
        if (current.interfaces_count && current.interfaces_count > 0) {
            for (let i = 0; i < current.interfaces_count; i++) {
                const ifaceName = _getClassNameFromIndex(current, current.interfaces[i]);
                if (ifaceName && clazz.class_name === ifaceName) return true;
            }
        }
        current = current.super_class;
    }

    return false;
}

function _findClass(jvm, name) {
    if (!jvm || !jvm.class_loader || !jvm.class_loader.classes) return null;
    for (const c of jvm.class_loader.classes) {
        if (c && c.class_name === name) return c;
    }
    return null;
}

function _getClassNameFromIndex(clazz, idx) {
    if (!clazz.constant_pool || idx <= 0 || idx >= clazz.constant_pool.length) return null;
    const entry = clazz.constant_pool[idx];
    if (!entry) return null;
    // CONSTANT_Class (tag=7) → name_index → CONSTANT_Utf8 (tag=1)
    if (entry.tag === 7) {
        const nameEntry = clazz.constant_pool[entry.info.class_info.name_index];
        if (nameEntry && nameEntry.tag === 1) return nameEntry.info.utf8.bytes;
    }
    return null;
}

// ─────────────────────────────────────────────────────────────
// Array operations
// ─────────────────────────────────────────────────────────────
export function array_length(array) {
    return array ? (array.length | 0) : 0;
}

export function array_element_type(array) {
    return array ? (array.element_type | 0) : 0;
}

export function array_get(array, index) {
    const zero = { raw: 0n, i: 0, j: 0n, f: 0, d: 0, ref: null, b: 0, s: 0, z: 0 };
    if (!array || index < 0 || index >= array.length) return zero;
    if (!array.data) return zero;

    const v = Object.assign({}, zero);
    switch (array.element_type) {
        case T_BOOLEAN:
            v.i = array.data[index] & 0xFF;
            break;
        case T_BYTE:
            // Sign-extend to match Java byte semantics.
            v.i = (array.data[index] << 24) >> 24;
            break;
        case T_CHAR:
            v.i = array.data[index] & 0xFFFF;
            break;
        case T_SHORT:
            v.i = (array.data[index] << 16) >> 16;
            break;
        case T_INT:
            v.i = array.data[index] | 0;
            break;
        case T_LONG:
            v.j = BigInt.asIntN(64, array.data[index]);
            break;
        case T_FLOAT:
            v.f = array.data[index];
            break;
        case T_DOUBLE:
            v.d = array.data[index];
            break;
        case DESC_OBJECT:
        case DESC_ARRAY:
            v.ref = array.data[index];
            break;
        default:
            v.i = array.data[index] | 0;
    }
    return v;
}

export function array_set(array, index, value) {
    if (!array || index < 0 || index >= array.length) return;
    if (!array.data) return;

    switch (array.element_type) {
        case T_BOOLEAN: case T_BYTE:
            array.data[index] = value.i & 0xFF;
            break;
        case T_CHAR:
            array.data[index] = value.i & 0xFFFF;
            break;
        case T_SHORT:
            array.data[index] = value.i & 0xFFFF;
            break;
        case T_INT:
            array.data[index] = value.i | 0;
            break;
        case T_LONG:
            array.data[index] = value.j;
            break;
        case T_FLOAT:
            array.data[index] = value.f;
            break;
        case T_DOUBLE:
            array.data[index] = value.d;
            break;
        case DESC_OBJECT:
        case DESC_ARRAY:
            array.data[index] = value.ref;
            break;
        default:
            array.data[index] = value.i | 0;
    }
}

export function array_get_ref(array, index) {
    if (!array || index < 0 || index >= array.length) return null;
    if (!array.data) return null;
    return array.data[index] || null;
}

export function array_set_ref(array, index, ref) {
    if (!array || index < 0 || index >= array.length) return;
    if (!array.data) return;
    array.data[index] = ref;
}

// ─────────────────────────────────────────────────────────────
// String operations
// ─────────────────────────────────────────────────────────────

function _isNativeString(str) {
    if (!str || !isManagedObject(str)) return false;
    const hdr = str[GC];
    return hdr && hdr.type === OBJ_TYPE_STRING;
}

export function string_length(str) {
    if (!str || !isManagedObject(str)) return 0;
    const hdr = str[GC];
    if (!hdr || hdr.type === OBJ_TYPE_FREE) return 0;

    if (hdr.type === OBJ_TYPE_STRING) return str.length | 0;

    if (hdr.type === OBJ_TYPE_OBJECT) {
        const jvm = g_jvm;
        if (str.header && str.header.clazz && jvm) {
            const countSlot = _getNativeStringCountSlot(jvm);
            if (countSlot >= 0 && str.fields && str.fields[countSlot] !== undefined) {
                return (str.fields[countSlot].i || 0) | 0;
            }
        }
    }
    return 0;
}

export function string_chars(str) {
    if (!str || !isManagedObject(str)) return null;
    const hdr = str[GC];
    if (!hdr || hdr.type === OBJ_TYPE_FREE) return null;

    // Native string — inline chars.
    if (hdr.type === OBJ_TYPE_STRING) {
        return str._inlineChars || null;
    }

    // Java String object — get char[] from value field.
    if (hdr.type === OBJ_TYPE_OBJECT) {
        const jvm = g_jvm;
        if (str.header && str.header.clazz && jvm) {
            const valueSlot = _getNativeStringValueSlot(jvm);
            if (valueSlot >= 0 && str.fields) {
                const fv = str.fields[valueSlot];
                const charArray = fv && fv.ref;
                if (charArray && charArray.data) {
                    return charArray.data;
                }
            }
        }
    }
    return null;
}

// Module-level reusable buffer (mirrors the thread-local TLS buffer in C).
let _utf8Buffer = '';

export function string_utf8(jvm, str) {
    if (!str) return null;

    // For native strings, use cached utf8 if available.
    if (_isNativeString(str) && str.utf8) return str.utf8;

    const len   = string_length(str);
    const chars = string_chars(str);
    if (!chars && len > 0) return null;

    // Convert UTF-16 chars to a JS string.
    let result = '';
    if (chars && len > 0) {
        // Uint16Array / plain Array — use String.fromCharCode.
        if (chars instanceof Uint16Array || Array.isArray(chars)) {
            result = String.fromCharCode(...Array.from(chars).slice(0, len));
        } else {
            for (let i = 0; i < len; i++) result += String.fromCharCode(chars[i]);
        }
    }

    if (_isNativeString(str)) {
        // Cache permanently on the native string object.
        str.utf8 = result;
        return str.utf8;
    }

    // For Java String objects, use module-level buffer (reused, not safe to store).
    _utf8Buffer = result;
    return _utf8Buffer;
}

export function string_utf8_copy(jvm, str) {
    const cached = string_utf8(jvm, str);
    if (cached === null || cached === undefined) return null;
    return cached; // JS strings are immutable — no need to copy.
}

export function string_utf8_cleanup() {
    _utf8Buffer = '';
}

export function string_equals(a, b) {
    if (a === b) return true;
    if (!a || !b) return false;

    const lenA = string_length(a);
    const lenB = string_length(b);
    if (lenA !== lenB) return false;

    const charsA = string_chars(a);
    const charsB = string_chars(b);
    if (!charsA || !charsB) return false;

    for (let i = 0; i < lenA; i++) {
        if (charsA[i] !== charsB[i]) return false;
    }
    return true;
}

export function string_hash(str) {
    if (!str) return 0;

    if (_isNativeString(str) && str.hash) return str.hash | 0;

    // For Java String objects, check the hash field.
    if (!_isNativeString(str)) {
        const jvm = g_jvm;
        if (str.header && str.header.clazz && jvm) {
            const hashSlot = _getNativeStringHashSlot(jvm);
            if (hashSlot >= 0 && str.fields && str.fields[hashSlot]) {
                const cached = str.fields[hashSlot].i | 0;
                if (cached !== 0) return cached;
            }
        }
    }

    const len   = string_length(str);
    const chars = string_chars(str);
    if (!chars) return 0;

    let hash = 0;
    for (let i = 0; i < len; i++) {
        // Java string hash: h = 31*h + c  (int32 arithmetic).
        hash = (Math.imul(31, hash) + (chars[i] & 0xFFFF)) | 0;
    }

    if (_isNativeString(str)) str.hash = hash;
    return hash;
}

// ─────────────────────────────────────────────────────────────
// Memory debugging (simplified — no raw heap to walk)
// ─────────────────────────────────────────────────────────────
export function heap_dump(jvm) {
    process.stdout.write('=== JS Heap Dump ===\n');
    process.stdout.write(`Objects tracked: ${heap.objects.size}\n`);
    process.stdout.write(`Allocated bytes (approx): ${heap.allocated}\n`);
    process.stdout.write(`GC cycles: ${heap.gcCycles}\n`);
    process.stdout.write('====================\n');
}

export function heap_validate(jvm) {
    // No raw memory to validate in JS — always consistent.
}

/**
 * heap_check_magic — always 0 in JS (no raw memory corruption possible).
 */
export function heap_check_magic(jvm) {
    return 0;
}
