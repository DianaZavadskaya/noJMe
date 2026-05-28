/*
 * J2ME Emulator - Optimized Method and Class Lookup
 *
 * Migrated from src/jvm/method_cache.c
 *
 * Translation notes:
 *   - FNV-1a hash is implemented directly in JS using BigInt-free 32-bit
 *     arithmetic via unsigned right-shift tricks (>>> 0) so that results
 *     stay in the 32-bit unsigned range without needing BigInt.
 *   - The fixed-size C array g_method_cache[4096] becomes a pre-allocated
 *     plain JS Array of null-initialised entry objects; the slot index
 *     arithmetic is identical to the C original.
 *   - g_class_hash[1024] (open chaining linked list) becomes a plain Array
 *     of Map entries; each bucket is a JS Map keyed by class name so that
 *     the chained-list pointer walk is replaced by Map.get/set/delete,
 *     which preserves semantics with less manual bookkeeping.
 *   - malloc/free for ClassHashEntry nodes are omitted (GC handles memory).
 *   - VTable stub (vtable_get_or_create) returns null, mirroring the C
 *     placeholder that returns NULL.
 *   - All public symbols declared in include/jvm.h are exported.
 */

// ============================================================
// FNV-1a constants
// ============================================================

const FNV_OFFSET_BASIS = 0x811C9DC5;
const FNV_PRIME        = 0x01000193;

// ============================================================
// FNV-1a Hash Function
// ============================================================

/**
 * Compute FNV-1a hash for a string.
 * Returns a 32-bit unsigned integer.
 *
 * @param {string|null} str
 * @returns {number}
 */
function fnv1a_hash(str) {
    if (!str) return 0;

    let hash = FNV_OFFSET_BASIS;
    for (let i = 0; i < str.length; i++) {
        // XOR with the low byte of the char code, then multiply mod 2^32
        hash = (hash ^ (str.charCodeAt(i) & 0xFF));
        // Emulate unsigned 32-bit multiply: split into 16-bit halves
        hash = Math.imul(hash, FNV_PRIME) >>> 0;
    }
    return hash >>> 0;
}

/**
 * Compute combined FNV-1a hash for method name + descriptor.
 *
 * @param {string} name
 * @param {string|null} descriptor
 * @returns {number}
 */
function method_hash(name, descriptor) {
    let hash = fnv1a_hash(name);

    // Separator (mirrors `hash ^= 0xFF; hash *= FNV_PRIME;`)
    hash = (hash ^ 0xFF) >>> 0;
    hash = Math.imul(hash, FNV_PRIME) >>> 0;

    if (descriptor) {
        for (let i = 0; i < descriptor.length; i++) {
            hash = (hash ^ (descriptor.charCodeAt(i) & 0xFF)) >>> 0;
            hash = Math.imul(hash, FNV_PRIME) >>> 0;
        }
    }
    return hash >>> 0;
}

// ============================================================
// Global Method Cache
// Fixed-size direct-mapped cache with one secondary slot for
// collision handling, matching the C implementation exactly.
// ============================================================

const METHOD_CACHE_SIZE = 4096; // must be power of 2
const METHOD_CACHE_MASK = METHOD_CACHE_SIZE - 1;

/**
 * @typedef {Object} MethodCacheEntry
 * @property {Object|null}  clazz      - JavaClass reference
 * @property {string|null}  name
 * @property {string|null}  descriptor
 * @property {Object|null}  method     - JavaMethod reference
 * @property {number}       hash
 */

/** @type {MethodCacheEntry[]} */
const g_method_cache = Array.from({ length: METHOD_CACHE_SIZE }, () => ({
    clazz:      null,
    name:       null,
    descriptor: null,
    method:     null,
    hash:       0,
}));

let g_method_cache_hits   = 0;
let g_method_cache_misses = 0;

/**
 * Fast method lookup in global cache.
 *
 * @param {Object} clazz      - JavaClass object
 * @param {string} name
 * @param {string} descriptor
 * @returns {Object|null}      - JavaMethod object, or null on miss
 */
export function method_cache_lookup(clazz, name, descriptor) {
    const hash = method_hash(name, descriptor);
    const idx  = hash & METHOD_CACHE_MASK;

    // Check primary slot
    let entry = g_method_cache[idx];
    if (entry.method !== null &&
        entry.clazz === clazz &&
        entry.name  === name  &&
        entry.descriptor === descriptor) {
        g_method_cache_hits++;
        return entry.method;
    }

    // Check secondary slot (collision handling)
    const idx2 = (idx ^ (hash >>> 16)) & METHOD_CACHE_MASK;
    entry = g_method_cache[idx2];
    if (entry.method !== null &&
        entry.clazz === clazz &&
        entry.name  === name  &&
        entry.descriptor === descriptor) {
        g_method_cache_hits++;
        return entry.method;
    }

    g_method_cache_misses++;
    return null;
}

/**
 * Store a resolved method in the global cache.
 *
 * @param {Object} clazz      - JavaClass object
 * @param {string} name
 * @param {string} descriptor
 * @param {Object} method     - JavaMethod object
 */
export function method_cache_store(clazz, name, descriptor, method) {
    const hash = method_hash(name, descriptor);
    const idx  = hash & METHOD_CACHE_MASK;

    let entry = g_method_cache[idx];

    // Use primary slot if empty or same key
    if (entry.method === null ||
        (entry.clazz === clazz &&
         entry.name  === name  &&
         entry.descriptor === descriptor)) {
        entry.clazz      = clazz;
        entry.name       = name;
        entry.descriptor = descriptor;
        entry.method     = method;
        entry.hash       = hash;
        return;
    }

    // Use secondary slot for collision
    const idx2 = (idx ^ (hash >>> 16)) & METHOD_CACHE_MASK;
    entry = g_method_cache[idx2];
    entry.clazz      = clazz;
    entry.name       = name;
    entry.descriptor = descriptor;
    entry.method     = method;
    entry.hash       = hash;
}

/**
 * Return cache hit/miss statistics.
 *
 * Mirrors `void method_cache_stats(int* hits, int* misses)`.
 * In JS the values are returned as a plain object instead of
 * out-pointer parameters.
 *
 * @returns {{ hits: number, misses: number }}
 */
export function method_cache_stats() {
    return { hits: g_method_cache_hits, misses: g_method_cache_misses };
}

// ============================================================
// Class Hash Table
// Open-chaining hash table for loaded class lookup by name.
// Each bucket in the C code is a linked list; here each bucket
// is a JS Map<name, clazz> which gives identical semantics with
// O(1) average operations.
// ============================================================

const CLASS_HASH_SIZE = 1024; // must be power of 2
const CLASS_HASH_MASK = CLASS_HASH_SIZE - 1;

/** @type {Map<string, Object>[]} */
const g_class_hash = Array.from({ length: CLASS_HASH_SIZE }, () => new Map());

let g_class_count = 0;

/**
 * Add (or update) a class in the class hash table.
 *
 * @param {Object} clazz - JavaClass with a `class_name` string property
 */
export function class_hash_add(clazz) {
    if (!clazz || !clazz.class_name) return;

    const hash   = fnv1a_hash(clazz.class_name);
    const idx    = hash & CLASS_HASH_MASK;
    const bucket = g_class_hash[idx];

    if (!bucket.has(clazz.class_name)) {
        g_class_count++;
    }
    bucket.set(clazz.class_name, clazz);
}

/**
 * Find a class in the hash table by name.
 *
 * @param {string|null} name
 * @returns {Object|null} - JavaClass, or null if not found
 */
export function class_hash_lookup(name) {
    if (!name) return null;

    const hash   = fnv1a_hash(name);
    const idx    = hash & CLASS_HASH_MASK;
    const bucket = g_class_hash[idx];

    return bucket.get(name) ?? null;
}

/**
 * Remove a class from the hash table by name.
 *
 * @param {string|null} name
 */
export function class_hash_remove(name) {
    if (!name) return;

    const hash   = fnv1a_hash(name);
    const idx    = hash & CLASS_HASH_MASK;
    const bucket = g_class_hash[idx];

    if (bucket.has(name)) {
        bucket.delete(name);
        g_class_count--;
    }
}

/**
 * Clear all entries from the class hash table.
 */
export function class_hash_clear() {
    for (let i = 0; i < CLASS_HASH_SIZE; i++) {
        g_class_hash[i].clear();
    }
    g_class_count = 0;
}

// ============================================================
// Optimized Method Resolution
// Combines method cache + class hierarchy walk.
// ============================================================

/**
 * Internal linear search for a method within a single class.
 *
 * @param {Object}      clazz
 * @param {string}      name
 * @param {string|null} descriptor
 * @returns {Object|null} - JavaMethod, or null
 */
function find_method_in_class(clazz, name, descriptor) {
    if (!clazz || !name) return null;

    const methods = clazz.methods;
    const count   = clazz.methods_count;
    for (let i = 0; i < count; i++) {
        const m = methods[i];
        if (m.name && m.name === name) {
            if (descriptor) {
                if (m.descriptor && m.descriptor === descriptor) {
                    return m;
                }
            } else {
                // No descriptor — return first match (for overloads)
                return m;
            }
        }
    }
    return null;
}

/**
 * Resolve a method using the global cache then the class hierarchy.
 *
 * Mirrors `JavaMethod* jvm_resolve_method_fast(JVM*, JavaClass*, ...)`.
 * The `jvm` parameter is kept for API compatibility but is unused here
 * (the cache and class chain are self-contained).
 *
 * @param {Object}      jvm
 * @param {Object}      clazz
 * @param {string}      name
 * @param {string}      descriptor
 * @returns {Object|null} - JavaMethod, or null
 */
export function jvm_resolve_method_fast(jvm, clazz, name, descriptor) {
    if (!clazz || !name || !descriptor) return null;

    // Step 1: Check global method cache
    const cached = method_cache_lookup(clazz, name, descriptor);
    if (cached) return cached;

    // Step 2: Search class hierarchy
    let searchClass = clazz;
    while (searchClass) {
        const method = find_method_in_class(searchClass, name, descriptor);
        if (method) {
            method_cache_store(clazz, name, descriptor, method);
            return method;
        }
        searchClass = searchClass.super_class ?? null;
    }

    return null;
}

/**
 * Find a loaded class by name using the class hash table.
 *
 * Mirrors `JavaClass* jvm_find_class_fast(JVM*, const char*)`.
 *
 * @param {Object}      jvm   - unused; kept for API compatibility
 * @param {string|null} name
 * @returns {Object|null}     - JavaClass, or null
 */
export function jvm_find_class_fast(jvm, name) {
    if (!jvm || !name) return null;
    return class_hash_lookup(name);
}

// ============================================================
// VTable Support (placeholder)
// Pre-computed virtual method tables are planned but not yet
// implemented in the C source either.
// ============================================================

export const VTABLE_SIZE = 256;

/**
 * Get or create a VTable for a class.
 * Currently a placeholder that returns null, matching the C stub.
 *
 * @param {Object} clazz
 * @returns {null}
 */
export function vtable_get_or_create(clazz) {
    // Placeholder — future implementation stores the vtable on clazz
    return null;
}

// ============================================================
// Initialization / Cleanup
// ============================================================

/**
 * Reset all cache state.
 * Call this when the JVM is first created or restarted.
 */
export function method_cache_init() {
    for (let i = 0; i < METHOD_CACHE_SIZE; i++) {
        const e  = g_method_cache[i];
        e.clazz      = null;
        e.name       = null;
        e.descriptor = null;
        e.method     = null;
        e.hash       = 0;
    }
    for (let i = 0; i < CLASS_HASH_SIZE; i++) {
        g_class_hash[i].clear();
    }
    g_method_cache_hits   = 0;
    g_method_cache_misses = 0;
    g_class_count         = 0;
}

/**
 * Release all resources held by the caches.
 * Mirrors `void method_cache_cleanup(void)`.
 */
export function method_cache_cleanup() {
    class_hash_clear();
}
