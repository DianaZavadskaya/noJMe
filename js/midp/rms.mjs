import { readFileSync, writeFileSync, mkdirSync, rmSync, existsSync } from 'fs';
import { join, dirname } from 'path';

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

export const MAX_RECORD_STORES    = 64;
export const MAX_RECORDS_PER_STORE = 1024;
export const MAX_RECORD_SIZE       = 65536;  // 64 KB per record
export const MAX_PRELOADED         = 32;

const RMS_FILE_MAGIC   = 0x524D5300;  // "RMS\0"
const RMS_FILE_VERSION = 1;

// ---------------------------------------------------------------------------
// State  (module-level, mirrors C file-scope statics)
// ---------------------------------------------------------------------------

// Each slot: { name: string|null, records: Array(MAX_RECORDS_PER_STORE),
//              next_id: int, open: bool, ref_count: int,
//              version: int, last_modified: BigInt }
// records[i]: null  OR  { id: int, data: Uint8Array, size: int, valid: bool }
const record_stores = new Array(MAX_RECORD_STORES).fill(null).map(() => _make_store_slot());
let record_store_count = 0;
let rms_initialized = false;

let rms_save_dir  = '';
let rms_game_name = '';

// Each slot: { store_name: string, record_id: int, data: Uint8Array, data_len: int, valid: bool }
const preloaded_records = [];

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

function _make_store_slot() {
    return {
        name:          null,
        records:       new Array(MAX_RECORDS_PER_STORE).fill(null),
        next_id:       1,
        open:          false,
        ref_count:     0,
        version:       0,
        last_modified: 0n,
    };
}

function rms_current_time_ms() {
    return BigInt(Date.now());
}

// ---------------------------------------------------------------------------
// Disk persistence helpers
// ---------------------------------------------------------------------------

function rms_ensure_dir(dir) {
    if (!dir) return;
    try {
        mkdirSync(dir, { recursive: true });
    } catch (_) {
        // ignore; if it already exists or cannot be created, callers handle it
    }
}

function rms_build_filepath(store_name) {
    if (!rms_save_dir || !rms_game_name) return '';
    // Sanitize: replace characters unsafe for filenames
    const safe_name = store_name.replace(/[\\:]/g, '_');
    return join(rms_save_dir, rms_game_name, `${safe_name}.rms`);
}

function rms_write_u32_le(buf, offset, val) {
    const v = val >>> 0;
    buf[offset]     =  v        & 0xFF;
    buf[offset + 1] = (v >>  8) & 0xFF;
    buf[offset + 2] = (v >> 16) & 0xFF;
    buf[offset + 3] = (v >> 24) & 0xFF;
}

function rms_read_u32_le(buf, offset) {
    return (buf[offset] | (buf[offset+1] << 8) | (buf[offset+2] << 16) | (buf[offset+3] << 24)) >>> 0;
}

function rms_save_store_to_disk(handle) {
    if (handle < 0 || handle >= MAX_RECORD_STORES) return;
    if (!rms_save_dir || !rms_game_name) return;

    const store = record_stores[handle];
    if (!store.name) return;

    const filepath = rms_build_filepath(store.name);
    if (!filepath) return;

    rms_ensure_dir(dirname(filepath));

    // Count valid records
    let record_count = 0;
    for (let i = 1; i < MAX_RECORDS_PER_STORE; i++) {
        if (store.records[i]?.valid) record_count++;
    }

    // Compute total byte size for the buffer:
    // Header: 4 magic + 4 version + 4 next_id + 4 record_count = 16 bytes
    // Each record: 4 id + 4 size + <size> data bytes
    let total = 16;
    for (let i = 1; i < MAX_RECORDS_PER_STORE; i++) {
        const rec = store.records[i];
        if (rec?.valid) total += 8 + rec.size;
    }

    const buf = new Uint8Array(total);
    let pos = 0;

    rms_write_u32_le(buf, pos, RMS_FILE_MAGIC);   pos += 4;
    rms_write_u32_le(buf, pos, RMS_FILE_VERSION); pos += 4;
    rms_write_u32_le(buf, pos, store.next_id);    pos += 4;
    rms_write_u32_le(buf, pos, record_count);     pos += 4;

    for (let i = 1; i < MAX_RECORDS_PER_STORE; i++) {
        const rec = store.records[i];
        if (!rec?.valid) continue;
        rms_write_u32_le(buf, pos, rec.id);   pos += 4;
        rms_write_u32_le(buf, pos, rec.size); pos += 4;
        if (rec.size > 0 && rec.data) {
            buf.set(rec.data.subarray(0, rec.size), pos);
        }
        pos += rec.size;
    }

    try {
        writeFileSync(filepath, buf);
    } catch (_) {
        process.stderr.write(`RMS SAVE: Failed to write ${filepath}\n`);
    }
}

function rms_load_store_from_disk(store) {
    if (!store?.name) return false;
    if (!rms_save_dir || !rms_game_name) return false;

    const filepath = rms_build_filepath(store.name);
    if (!filepath) return false;

    let raw;
    try {
        raw = readFileSync(filepath);
    } catch (_) {
        return false;
    }

    const buf = new Uint8Array(raw.buffer, raw.byteOffset, raw.byteLength);
    if (buf.length < 16) return false;

    let pos = 0;
    const magic   = rms_read_u32_le(buf, pos); pos += 4;
    const version = rms_read_u32_le(buf, pos); pos += 4;
    if (magic !== RMS_FILE_MAGIC || version !== RMS_FILE_VERSION) return false;

    const next_id      = rms_read_u32_le(buf, pos); pos += 4;
    const record_count = rms_read_u32_le(buf, pos); pos += 4;

    let loaded = 0;
    for (let r = 0; r < record_count && r < MAX_RECORDS_PER_STORE; r++) {
        if (pos + 8 > buf.length) break;
        const rec_id   = rms_read_u32_le(buf, pos); pos += 4;
        const rec_size = rms_read_u32_le(buf, pos); pos += 4;

        if (rec_id < 1 || rec_id >= MAX_RECORDS_PER_STORE || rec_size > MAX_RECORD_SIZE) {
            if (rec_size <= MAX_RECORD_SIZE) pos += rec_size;
            continue;
        }

        if (pos + rec_size > buf.length) break;

        const data = rec_size > 0 ? new Uint8Array(rec_size) : null;
        if (data) data.set(buf.subarray(pos, pos + rec_size));
        pos += rec_size;

        store.records[rec_id] = {
            id:    rec_id,
            data:  data,
            size:  rec_size,
            valid: true,
        };
        loaded++;
    }

    if (next_id > store.next_id) store.next_id = next_id;
    return loaded > 0;
}

function rms_delete_store_file(name) {
    if (!name || !rms_save_dir || !rms_game_name) return;
    const filepath = rms_build_filepath(name);
    if (!filepath) return;
    try {
        rmSync(filepath);
    } catch (_) {
        // already gone or unreadable
    }
}

// ---------------------------------------------------------------------------
// Pre-loaded records (rms_bypass.conf)
// ---------------------------------------------------------------------------

function hex_to_bytes(hex) {
    const bytes = [];
    let i = 0;
    while (i + 1 < hex.length) {
        const ch = hex[i];
        if (ch === ' ' || ch === '\t') { i++; continue; }
        const hi = parseInt(hex[i],   16);
        const lo = parseInt(hex[i+1], 16);
        if (isNaN(hi) || isNaN(lo)) break;
        bytes.push((hi << 4) | lo);
        i += 2;
    }
    return new Uint8Array(bytes);
}

function load_preloaded_records() {
    let text;
    try {
        text = readFileSync('rms_bypass.conf', 'utf8');
    } catch (_) {
        return;
    }

    for (const raw_line of text.split('\n')) {
        if (preloaded_records.length >= MAX_PRELOADED) break;
        const line = raw_line.trimEnd();
        if (!line || line[0] === '#') continue;

        const eq_idx = line.indexOf('=');
        if (eq_idx < 0) continue;

        const store_part = line.slice(0, eq_idx).trim();
        const hex_data   = line.slice(eq_idx + 1).trim();

        const dot_idx = store_part.indexOf('.');
        if (dot_idx < 0) continue;

        const store_name = store_part.slice(0, dot_idx);
        const record_id  = parseInt(store_part.slice(dot_idx + 1), 10);
        if (isNaN(record_id)) continue;

        const data = hex_to_bytes(hex_data);
        preloaded_records.push({
            store_name,
            record_id,
            data,
            data_len: data.length,
            valid: true,
        });
    }
}

// ---------------------------------------------------------------------------
// Core RMS logic
// ---------------------------------------------------------------------------

function rms_init() {
    if (rms_initialized) return;
    load_preloaded_records();
    rms_initialized = true;
}

function rms_open(name, create_if_necessary) {
    rms_init();
    if (!name) return -1;

    // Find existing store
    for (let i = 0; i < MAX_RECORD_STORES; i++) {
        if (record_stores[i].name === name) {
            record_stores[i].ref_count++;
            record_stores[i].open = true;
            return i;
        }
    }

    if (!create_if_necessary) return -1;

    // Find free slot
    let handle = -1;
    for (let i = 0; i < MAX_RECORD_STORES; i++) {
        if (record_stores[i].name === null) {
            handle = i;
            break;
        }
    }
    if (handle < 0) return -2;

    const store = record_stores[handle];
    store.name      = name;
    store.next_id   = 1;
    store.open      = true;
    store.ref_count = 1;
    store.version   = 0;
    store.last_modified = 0n;
    // Clear records array
    for (let i = 0; i < MAX_RECORDS_PER_STORE; i++) store.records[i] = null;

    // Apply pre-loaded records
    for (const pr of preloaded_records) {
        if (pr.store_name === name && pr.valid) {
            const rid = pr.record_id;
            if (rid > 0 && rid < MAX_RECORDS_PER_STORE) {
                store.records[rid] = {
                    id:    rid,
                    data:  pr.data.slice(),
                    size:  pr.data_len,
                    valid: true,
                };
                if (rid >= store.next_id) store.next_id = rid + 1;
            }
        }
    }

    // Disk data takes priority over pre-loaded records
    rms_load_store_from_disk(store);

    if (handle >= record_store_count) record_store_count = handle + 1;

    return handle;
}

function get_native_store_by_handle(handle) {
    if (handle < 0 || handle >= MAX_RECORD_STORES) return null;
    const store = record_stores[handle];
    if (!store.name || !store.open) return null;
    return store;
}

// Extract nativeHandle from a JavaObject representing a RecordStore.
// In C this scans the class fields for "nativeHandle"; here the JS JVM
// stores it at obj.fields[i].i where the field name is "nativeHandle".
function get_native_store(rs_obj) {
    if (!rs_obj) return null;
    const clazz = rs_obj.header?.clazz;
    if (!clazz) return null;
    const fields = clazz.fields ?? [];
    for (let i = 0; i < fields.length; i++) {
        if (fields[i]?.name === 'nativeHandle') {
            const handle = rs_obj.fields?.[i]?.i ?? -1;
            return get_native_store_by_handle(handle);
        }
    }
    return null;
}

function get_handle_for_store(store) {
    for (let i = 0; i < MAX_RECORD_STORES; i++) {
        if (record_stores[i] === store) return i;
    }
    return -1;
}

function set_native_handle(rs_obj, handle) {
    const clazz = rs_obj.header?.clazz;
    if (!clazz) return;
    const fields = clazz.fields ?? [];
    for (let i = 0; i < fields.length; i++) {
        if (fields[i]?.name === 'nativeHandle') {
            if (!rs_obj.fields) rs_obj.fields = [];
            if (!rs_obj.fields[i]) rs_obj.fields[i] = {};
            rs_obj.fields[i].i = handle;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Native method implementations
// These mirror the C native_rms_* functions.  They receive (jvm, thread, args)
// and return a JavaValue-shaped plain object { i, j, f, d, ref, raw }.
// ---------------------------------------------------------------------------

export function native_rms_openRecordStore(jvm, thread, args) {
    // Static: args[0] = name (String), args[1] = create (boolean)
    const name_str = args[0]?.ref;
    const create   = args[1]?.i;

    if (!name_str) {
        native_throw_npe(jvm, thread);
        return { ref: null };
    }

    const name = string_utf8(jvm, name_str);
    const handle = rms_open(name, !!create);

    if (handle < 0) {
        if (handle === -1) {
            jvm_throw_by_name(jvm, 'javax/microedition/rms/RecordStoreNotFoundException', name);
        } else {
            jvm_throw_by_name(jvm, 'javax/microedition/rms/RecordStoreFullException', null);
        }
        if (thread) thread.pending_exception = jvm_exception_pending(jvm);
        return { ref: null };
    }

    const rs_class = jvm_load_class(jvm, 'javax/microedition/rms/RecordStore');
    if (!rs_class) return { ref: null };

    const rs_obj = jvm_new_object(jvm, rs_class);
    if (!rs_obj) return { ref: null };

    set_native_handle(rs_obj, handle);
    return { ref: rs_obj };
}

export function native_rms_closeRecordStore(jvm, thread, args) {
    const rs_obj = args[0]?.ref;
    if (!rs_obj) { native_throw_npe(jvm, thread); return { raw: 0 }; }

    const store = get_native_store(rs_obj);
    if (store) {
        store.ref_count--;
        if (store.ref_count <= 0) {
            store.open = false;
            const h = get_handle_for_store(store);
            if (h >= 0) rms_save_store_to_disk(h);
        }
    }
    return { raw: 0 };
}

export function native_rms_addRecord(jvm, thread, args) {
    const rs_obj = args[0]?.ref;
    const data   = args[1]?.ref;
    const offset = args[2]?.i ?? 0;
    const length = args[3]?.i ?? 0;

    if (!rs_obj || !data) {
        native_throw_npe(jvm, thread);
        return { i: -1 };
    }
    if (length <= 0 || length > MAX_RECORD_SIZE) return { i: -1 };
    if (offset < 0 || offset + length > (data.length ?? 0)) {
        native_throw_aioobe(jvm, thread, offset);
        return { i: -1 };
    }

    const store = get_native_store(rs_obj);
    if (!store?.open) {
        jvm_throw_by_name(jvm, 'javax/microedition/rms/RecordStoreNotOpenException', null);
        if (thread) thread.pending_exception = jvm_exception_pending(jvm);
        return { i: -1 };
    }

    // Find free slot starting at 1
    let id = -1;
    for (let i = 1; i < MAX_RECORDS_PER_STORE; i++) {
        if (!store.records[i]?.valid) { id = i; break; }
    }
    if (id < 0) {
        jvm_throw_by_name(jvm, 'javax/microedition/rms/RecordStoreFullException', null);
        if (thread) thread.pending_exception = jvm_exception_pending(jvm);
        return { i: -1 };
    }

    const src = array_data(data);
    const record_data = new Uint8Array(length);
    record_data.set(src.subarray(offset, offset + length));

    store.records[id] = { id, data: record_data, size: length, valid: true };
    if (id >= store.next_id) store.next_id = id + 1;

    store.version++;
    store.last_modified = rms_current_time_ms();

    const h = get_handle_for_store(store);
    if (h >= 0) rms_save_store_to_disk(h);

    return { i: id };
}

export function native_rms_getRecord(jvm, thread, args) {
    const rs_obj   = args[0]?.ref;
    const record_id = args[1]?.i ?? -1;

    if (!rs_obj) { native_throw_npe(jvm, thread); return { ref: null }; }

    const store = get_native_store(rs_obj);
    if (!store?.open) {
        jvm_throw_by_name(jvm, 'javax/microedition/rms/RecordStoreNotOpenException', null);
        if (thread) thread.pending_exception = jvm_exception_pending(jvm);
        return { ref: null };
    }

    if (record_id < 1 || record_id >= MAX_RECORDS_PER_STORE) {
        jvm_throw_by_name(jvm, 'javax/microedition/rms/InvalidRecordIDException', null);
        if (thread) thread.pending_exception = jvm_exception_pending(jvm);
        return { ref: null };
    }

    const rec = store.records[record_id];
    if (!rec?.valid) {
        jvm_throw_by_name(jvm, 'javax/microedition/rms/InvalidRecordIDException', null);
        if (thread) thread.pending_exception = jvm_exception_pending(jvm);
        return { ref: null };
    }

    const byte_array = jvm_new_array(jvm, T_BYTE, rec.size, null);
    if (!byte_array) {
        jvm_throw_by_name(jvm, 'java/lang/OutOfMemoryError', null);
        if (thread) thread.pending_exception = jvm_exception_pending(jvm);
        return { ref: null };
    }

    array_data(byte_array).set(rec.data.subarray(0, rec.size));
    return { ref: byte_array };
}

export function native_rms_getRecord_array(jvm, thread, args) {
    const rs_obj   = args[0]?.ref;
    const record_id = args[1]?.i ?? -1;
    const dest     = args[2]?.ref;
    const offset   = args[3]?.i ?? 0;

    if (!rs_obj || !dest) return { i: -1 };

    const store = get_native_store(rs_obj);
    if (!store?.open) return { i: -1 };
    if (record_id < 1 || record_id >= MAX_RECORDS_PER_STORE) return { i: -1 };

    const rec = store.records[record_id];
    if (!rec?.valid) return { i: 0 };

    const length = rec.size;
    if (length === 0) return { i: 0 };
    if (offset < 0 || length > (dest.length ?? 0) - offset) return { i: -1 };

    array_data(dest).set(rec.data.subarray(0, length), offset);
    return { i: length };
}

export function native_rms_getRecordSize(jvm, thread, args) {
    const rs_obj   = args[0]?.ref;
    const record_id = args[1]?.i ?? -1;

    if (!rs_obj) return { i: -1 };

    const store = get_native_store(rs_obj);
    if (!store?.open) return { i: -1 };
    if (record_id < 1 || record_id >= MAX_RECORDS_PER_STORE) return { i: -1 };

    const rec = store.records[record_id];
    if (!rec?.valid) return { i: 0 };
    return { i: rec.size };
}

export function native_rms_setRecord(jvm, thread, args) {
    const rs_obj   = args[0]?.ref;
    const record_id = args[1]?.i ?? -1;
    const data     = args[2]?.ref;
    const offset   = args[3]?.i ?? 0;
    const length   = args[4]?.i ?? 0;

    if (!rs_obj || !data) return { raw: 0 };

    const store = get_native_store(rs_obj);
    if (!store?.open) return { raw: 0 };
    if (record_id < 1 || record_id >= MAX_RECORDS_PER_STORE) return { raw: 0 };

    const rec = store.records[record_id];
    if (!rec?.valid) return { raw: 0 };
    if (offset < 0 || length < 0 || offset + length > (data.length ?? 0)) return { raw: 0 };

    const new_data = new Uint8Array(length);
    new_data.set(array_data(data).subarray(offset, offset + length));

    rec.data = new_data;
    rec.size = length;

    store.version++;
    store.last_modified = rms_current_time_ms();

    const h = get_handle_for_store(store);
    if (h >= 0) rms_save_store_to_disk(h);

    return { raw: 0 };
}

export function native_rms_deleteRecord(jvm, thread, args) {
    const rs_obj   = args[0]?.ref;
    const record_id = args[1]?.i ?? -1;

    if (!rs_obj) return { raw: 0 };

    const store = get_native_store(rs_obj);
    if (!store?.open) return { raw: 0 };
    if (record_id < 1 || record_id >= MAX_RECORDS_PER_STORE) return { raw: 0 };

    if (store.records[record_id]?.valid) {
        store.records[record_id] = null;
        store.version++;
        store.last_modified = rms_current_time_ms();

        const h = get_handle_for_store(store);
        if (h >= 0) rms_save_store_to_disk(h);
    }

    return { raw: 0 };
}

export function native_rms_getNumRecords(jvm, thread, args) {
    const rs_obj = args[0]?.ref;
    if (!rs_obj) return { i: 0 };

    const store = get_native_store(rs_obj);
    if (!store?.open) return { i: 0 };

    let count = 0;
    for (let i = 1; i < MAX_RECORDS_PER_STORE; i++) {
        if (store.records[i]?.valid) count++;
    }
    return { i: count };
}

export function native_rms_enumerateRecords(jvm, thread, args) {
    const rs_obj        = args[0]?.ref;
    const filter_obj    = args[1]?.ref;
    const comparator_obj = args[2]?.ref;
    // args[3] = keepUpdated — ignored

    if (!rs_obj) return { ref: null };

    const store = get_native_store(rs_obj);
    if (!store?.open) return { ref: null };

    // Collect valid record IDs, optionally applying filter
    let record_ids = [];
    for (let i = 1; i < MAX_RECORDS_PER_STORE; i++) {
        const rec = store.records[i];
        if (!rec?.valid) continue;

        if (filter_obj) {
            const byte_arr = jvm_new_array(jvm, T_BYTE, rec.size, null);
            if (byte_arr && rec.data) {
                array_data(byte_arr).set(rec.data.subarray(0, rec.size));
            }
            const filter_result = { i: 0 };
            jvm_invoke_virtual(jvm, filter_obj, 'matches', '([B)Z', [{ ref: byte_arr }], filter_result);
            if (!filter_result.i) continue;
        }

        record_ids.push(i);
    }

    // Sort using comparator if provided (insertion sort matching C)
    if (comparator_obj && record_ids.length > 1) {
        for (let i = 1; i < record_ids.length; i++) {
            const key_id = record_ids[i];
            let j = i - 1;
            while (j >= 0) {
                const id_a = record_ids[j];
                const id_b = key_id;
                const rec_a = store.records[id_a];
                const rec_b = store.records[id_b];

                const arr_a = jvm_new_array(jvm, T_BYTE, rec_a.size, null);
                const arr_b = jvm_new_array(jvm, T_BYTE, rec_b.size, null);
                if (arr_a && rec_a.data) array_data(arr_a).set(rec_a.data.subarray(0, rec_a.size));
                if (arr_b && rec_b.data) array_data(arr_b).set(rec_b.data.subarray(0, rec_b.size));

                const cmp_result = { i: 0 };
                jvm_invoke_virtual(jvm, comparator_obj, 'compare', '([B[B)I',
                    [{ ref: arr_a }, { ref: arr_b }], cmp_result);

                if (cmp_result.i > 0) {
                    record_ids[j + 1] = record_ids[j];
                    j--;
                } else {
                    break;
                }
            }
            record_ids[j + 1] = key_id;
        }
    }

    const enum_class = jvm_load_class(jvm, 'javax/microedition/rms/RecordEnumerationImpl');
    if (!enum_class) return { ref: null };

    const enum_obj = heap_alloc_object(jvm, enum_class);
    if (!enum_obj) return { ref: null };

    const ids_arr = jvm_new_array(jvm, T_INT, record_ids.length, null);
    if (!ids_arr && record_ids.length > 0) return { ref: null };

    if (record_ids.length > 0 && ids_arr) {
        const ids_data = array_data_int32(ids_arr);
        for (let idx = 0; idx < record_ids.length; idx++) {
            ids_data[idx] = record_ids[idx];
        }
    }

    // Get store handle from RecordStore object
    let store_handle = -1;
    const rs_class = rs_obj.header?.clazz;
    if (rs_class?.fields) {
        for (let i = 0; i < rs_class.fields.length; i++) {
            if (rs_class.fields[i]?.name === 'nativeHandle') {
                store_handle = rs_obj.fields?.[i]?.i ?? -1;
                break;
            }
        }
    }

    if (!enum_obj.fields) enum_obj.fields = [];
    enum_obj.fields[0] = { ref: ids_arr };
    enum_obj.fields[1] = { i: 0 };
    enum_obj.fields[2] = { i: store_handle };

    return { ref: enum_obj };
}

export function native_rms_deleteRecordStore(jvm, thread, args) {
    // Static: args[0] = name (String)
    const name_str = args[0]?.ref;
    if (!name_str) return { raw: 0 };

    const name = string_utf8(jvm, name_str);
    rms_init();

    for (let i = 0; i < record_store_count; i++) {
        if (record_stores[i].name === name) {
            record_stores[i].records.fill(null);
            rms_delete_store_file(name);
            // Null out the slot — do NOT shift, that would invalidate existing handles
            record_stores[i] = _make_store_slot();
            break;
        }
    }

    return { raw: 0 };
}

export function native_rms_listRecordStores(jvm, thread, args) {
    rms_init();

    let count = 0;
    for (let i = 0; i < record_store_count; i++) {
        if (record_stores[i].name) count++;
    }

    const array = jvm_new_array(jvm, DESC_OBJECT, count, null);
    if (!array) return { ref: null };

    let idx = 0;
    for (let i = 0; i < record_store_count && idx < count; i++) {
        if (record_stores[i].name) {
            const str = jvm_new_string(jvm, record_stores[i].name);
            array_set_ref(array, idx++, str);
        }
    }

    return { ref: array };
}

export function native_rms_getNextRecordID(jvm, thread, args) {
    const rs_obj = args[0]?.ref;
    if (!rs_obj) return { i: -1 };

    const store = get_native_store(rs_obj);
    if (!store?.open) return { i: -1 };

    return { i: store.next_id };
}

export function native_rms_getVersion(jvm, thread, args) {
    const rs_obj = args[0]?.ref;
    if (!rs_obj) return { i: 0 };

    const store = get_native_store(rs_obj);
    if (!store) return { i: 0 };

    return { i: store.version };
}

export function native_rms_getLastModified(jvm, thread, args) {
    const rs_obj = args[0]?.ref;
    if (!rs_obj) return { j: 0n };

    const store = get_native_store(rs_obj);
    if (!store) return { j: 0n };

    return { j: store.last_modified };
}

// FreeJ2ME compatibility: return constant values so games that check available
// space always see "enough room"
export function native_rms_getSize(jvm, thread, args) {
    return { i: 32767 };
}

export function native_rms_getSizeAvailable(jvm, thread, args) {
    return { i: 65536 };
}

export function native_rms_getName(jvm, thread, args) {
    const rs_obj = args[0]?.ref;
    if (!rs_obj) return { ref: null };

    const store = get_native_store(rs_obj);
    if (!store?.name) return { ref: null };

    return { ref: jvm_new_string(jvm, store.name) };
}

export function native_rms_setMode(jvm, thread, args) {
    return { raw: 0 };  // FreeJ2ME: empty implementation
}

export function native_rms_addRecordListener(jvm, thread, args) {
    return { raw: 0 };
}

export function native_rms_removeRecordListener(jvm, thread, args) {
    return { raw: 0 };
}

export function native_rms_openRecordStore_authmode(jvm, thread, args) {
    // Static: args[0]=name, args[1]=create, args[2]=authmode, args[3]=writable
    // authmode and writable are ignored (same as FreeJ2ME)
    return native_rms_openRecordStore(jvm, thread, [args[0], args[1]]);
}

export function native_rms_openRecordStore_vendor(jvm, thread, args) {
    // Static: args[0]=name, args[1]=vendorName, args[2]=suiteName
    // FreeJ2ME: opens without creating (create=false)
    const name_str = args[0]?.ref;
    if (!name_str) {
        native_throw_npe(jvm, thread);
        return { ref: null };
    }

    const name = string_utf8(jvm, name_str);
    const handle = rms_open(name, false);

    if (handle < 0) {
        jvm_throw_by_name(jvm, 'javax/microedition/rms/RecordStoreNotFoundException', name);
        if (thread) thread.pending_exception = jvm_exception_pending(jvm);
        return { ref: null };
    }

    const rs_class = jvm_load_class(jvm, 'javax/microedition/rms/RecordStore');
    if (!rs_class) return { ref: null };

    const rs_obj = jvm_new_object(jvm, rs_class);
    if (!rs_obj) return { ref: null };

    set_native_handle(rs_obj, handle);
    return { ref: rs_obj };
}

// ---------------------------------------------------------------------------
// RecordEnumeration native methods
// ---------------------------------------------------------------------------

export function native_enumeration_hasNextElement(jvm, thread, args) {
    const enum_obj = args[0]?.ref;
    if (!enum_obj) return { i: 0 };

    const ids     = enum_obj.fields?.[0]?.ref;
    const current = enum_obj.fields?.[1]?.i ?? 0;
    if (!ids) return { i: 0 };

    return { i: current < (ids.length ?? 0) ? 1 : 0 };
}

export function native_enumeration_nextRecordId(jvm, thread, args) {
    const enum_obj = args[0]?.ref;
    if (!enum_obj) {
        jvm_throw_by_name(jvm, 'javax/microedition/rms/InvalidRecordIDException', null);
        return { i: -1 };
    }

    const ids     = enum_obj.fields?.[0]?.ref;
    const current = enum_obj.fields?.[1]?.i ?? 0;

    if (!ids || current >= (ids.length ?? 0)) {
        jvm_throw_by_name(jvm, 'javax/microedition/rms/InvalidRecordIDException', null);
        return { i: -1 };
    }

    const record_id = array_data_int32(ids)[current];
    enum_obj.fields[1] = { i: current + 1 };
    return { i: record_id };
}

export function native_enumeration_nextRecord(jvm, thread, args) {
    const enum_obj = args[0]?.ref;
    if (!enum_obj) {
        jvm_throw_by_name(jvm, 'javax/microedition/rms/InvalidRecordIDException', null);
        return { ref: null };
    }

    const ids     = enum_obj.fields?.[0]?.ref;
    const current = enum_obj.fields?.[1]?.i ?? 0;

    if (!ids || current >= (ids.length ?? 0)) {
        jvm_throw_by_name(jvm, 'javax/microedition/rms/InvalidRecordIDException', null);
        return { ref: null };
    }

    const record_id    = array_data_int32(ids)[current];
    enum_obj.fields[1] = { i: current + 1 };

    const store_handle = enum_obj.fields?.[2]?.i ?? -1;
    if (store_handle < 0 || store_handle >= record_store_count) {
        jvm_throw_by_name(jvm, 'javax/microedition/rms/InvalidRecordIDException', null);
        return { ref: null };
    }

    const store = record_stores[store_handle];
    const rec   = store.records[record_id];
    if (!rec?.valid) {
        jvm_throw_by_name(jvm, 'javax/microedition/rms/InvalidRecordIDException', null);
        return { ref: null };
    }

    const data = jvm_new_array(jvm, T_BYTE, rec.size, null);
    if (!data) {
        jvm_throw_by_name(jvm, 'java/lang/OutOfMemoryError', null);
        return { ref: null };
    }
    array_data(data).set(rec.data.subarray(0, rec.size));
    return { ref: data };
}

export function native_enumeration_destroy(jvm, thread, args) {
    const enum_obj = args[0]?.ref;
    if (enum_obj) {
        if (!enum_obj.fields) enum_obj.fields = [];
        enum_obj.fields[0] = { ref: null };
        enum_obj.fields[1] = { i: 0 };
    }
    return { raw: 0 };
}

export function native_enumeration_numRecords(jvm, thread, args) {
    const enum_obj = args[0]?.ref;
    if (!enum_obj) return { i: 0 };
    const ids = enum_obj.fields?.[0]?.ref;
    return { i: ids ? (ids.length ?? 0) : 0 };
}

export function native_enumeration_hasPreviousElement(jvm, thread, args) {
    const enum_obj = args[0]?.ref;
    if (!enum_obj) return { i: 0 };
    const current = enum_obj.fields?.[1]?.i ?? 0;
    return { i: current > 0 ? 1 : 0 };
}

export function native_enumeration_previousRecordId(jvm, thread, args) {
    const enum_obj = args[0]?.ref;
    if (!enum_obj) {
        jvm_throw_by_name(jvm, 'javax/microedition/rms/InvalidRecordIDException', null);
        return { i: -1 };
    }

    const ids     = enum_obj.fields?.[0]?.ref;
    const current = enum_obj.fields?.[1]?.i ?? 0;

    if (!ids || current <= 0) {
        // FreeJ2ME wraps around: if index<=0, set to last element
        if (ids && (ids.length ?? 0) > 0) {
            const last = (ids.length ?? 0) - 1;
            enum_obj.fields[1] = { i: last };
            return { i: array_data_int32(ids)[last] };
        }
        jvm_throw_by_name(jvm, 'javax/microedition/rms/InvalidRecordIDException', null);
        return { i: -1 };
    }

    const new_idx = current - 1;
    enum_obj.fields[1] = { i: new_idx };
    return { i: array_data_int32(ids)[new_idx] };
}

export function native_enumeration_previousRecord(jvm, thread, args) {
    const enum_obj = args[0]?.ref;
    if (!enum_obj) {
        jvm_throw_by_name(jvm, 'javax/microedition/rms/InvalidRecordIDException', null);
        return { ref: null };
    }

    const ids     = enum_obj.fields?.[0]?.ref;
    let   current = enum_obj.fields?.[1]?.i ?? 0;

    if (!ids) {
        jvm_throw_by_name(jvm, 'javax/microedition/rms/InvalidRecordIDException', null);
        return { ref: null };
    }

    // Decrement; FreeJ2ME wraps around
    current--;
    if (current < 0) current = (ids.length ?? 0) - 1;
    enum_obj.fields[1] = { i: current };

    const record_id    = array_data_int32(ids)[current];
    const store_handle = enum_obj.fields?.[2]?.i ?? -1;

    if (store_handle < 0 || store_handle >= record_store_count) {
        jvm_throw_by_name(jvm, 'javax/microedition/rms/InvalidRecordIDException', null);
        return { ref: null };
    }

    const store = record_stores[store_handle];
    const rec   = store.records[record_id];
    if (!rec?.valid) {
        jvm_throw_by_name(jvm, 'javax/microedition/rms/InvalidRecordIDException', null);
        return { ref: null };
    }

    const data = jvm_new_array(jvm, T_BYTE, rec.size, null);
    if (!data) {
        jvm_throw_by_name(jvm, 'java/lang/OutOfMemoryError', null);
        return { ref: null };
    }
    array_data(data).set(rec.data.subarray(0, rec.size));
    return { ref: data };
}

export function native_enumeration_reset(jvm, thread, args) {
    const enum_obj = args[0]?.ref;
    if (enum_obj) {
        if (!enum_obj.fields) enum_obj.fields = [];
        enum_obj.fields[1] = { i: 0 };
    }
    return { raw: 0 };
}

export function native_enumeration_rebuild(jvm, thread, args) {
    return { raw: 0 };  // no-op; full filter/comparator rebuild not needed
}

export function native_enumeration_isKeptUpdated(jvm, thread, args) {
    return { i: 0 };
}

export function native_enumeration_keepUpdated(jvm, thread, args) {
    return { raw: 0 };
}

// ---------------------------------------------------------------------------
// Public C-API equivalents (called from midp.h declarations)
// ---------------------------------------------------------------------------

export function midp_rms_set_save_path(save_dir, game_name) {
    if (save_dir  != null) rms_save_dir  = save_dir;
    if (game_name != null) rms_game_name = game_name;
}

export function midp_rms_save_all() {
    if (!rms_save_dir) return;
    for (let i = 0; i < MAX_RECORD_STORES; i++) {
        if (record_stores[i].name && record_stores[i].open) {
            rms_save_store_to_disk(i);
        }
    }
}

export function midp_rms_open_record_store(name, create_if_necessary) {
    return rms_open(name, create_if_necessary);
}

export function midp_rms_close_record_store(handle) {
    const store = get_native_store_by_handle(handle);
    if (!store) return;
    store.ref_count--;
    if (store.ref_count <= 0) {
        store.open = false;
        rms_save_store_to_disk(handle);
    }
}

export function midp_rms_add_record(handle, data, offset, length) {
    const store = get_native_store_by_handle(handle);
    if (!store) return -1;

    let id = -1;
    for (let i = 1; i < MAX_RECORDS_PER_STORE; i++) {
        if (!store.records[i]?.valid) { id = i; break; }
    }
    if (id < 0) return -1;

    const record_data = new Uint8Array(length);
    record_data.set(data.subarray(offset, offset + length));

    store.records[id] = { id, data: record_data, size: length, valid: true };
    if (id >= store.next_id) store.next_id = id + 1;
    store.version++;
    store.last_modified = rms_current_time_ms();
    rms_save_store_to_disk(handle);
    return id;
}

export function midp_rms_set_record(handle, record_id, data, offset, length) {
    const store = get_native_store_by_handle(handle);
    if (!store) return;
    const rec = store.records[record_id];
    if (!rec?.valid) return;

    const new_data = new Uint8Array(length);
    new_data.set(data.subarray(offset, offset + length));
    rec.data = new_data;
    rec.size = length;
    store.version++;
    store.last_modified = rms_current_time_ms();
    rms_save_store_to_disk(handle);
}

export function midp_rms_get_record(handle, record_id) {
    const store = get_native_store_by_handle(handle);
    if (!store) return null;
    const rec = store.records[record_id];
    if (!rec?.valid) return null;
    return { data: rec.data, length: rec.size };
}

export function midp_rms_delete_record(handle, record_id) {
    const store = get_native_store_by_handle(handle);
    if (!store) return;
    if (store.records[record_id]?.valid) {
        store.records[record_id] = null;
        store.version++;
        store.last_modified = rms_current_time_ms();
        rms_save_store_to_disk(handle);
    }
}

export function midp_rms_get_num_records(handle) {
    const store = get_native_store_by_handle(handle);
    if (!store) return 0;
    let count = 0;
    for (let i = 1; i < MAX_RECORDS_PER_STORE; i++) {
        if (store.records[i]?.valid) count++;
    }
    return count;
}

export function midp_rms_get_record_ids(handle) {
    const store = get_native_store_by_handle(handle);
    if (!store) return [];
    const ids = [];
    for (let i = 1; i < MAX_RECORDS_PER_STORE; i++) {
        if (store.records[i]?.valid) ids.push(i);
    }
    return ids;
}

export function midp_rms_delete_record_store(name) {
    rms_init();
    for (let i = 0; i < record_store_count; i++) {
        if (record_stores[i].name === name) {
            record_stores[i].records.fill(null);
            rms_delete_store_file(name);
            record_stores[i] = _make_store_slot();
            break;
        }
    }
}

export function midp_rms_list_record_stores() {
    rms_init();
    const result = [];
    for (let i = 0; i < record_store_count; i++) {
        if (record_stores[i].name) result.push(record_stores[i].name);
    }
    return result;
}

// ---------------------------------------------------------------------------
// init_javax_microedition_rms
// Registers all native methods into the JVM's native dispatch table.
// ---------------------------------------------------------------------------

export function init_javax_microedition_rms(jvm) {
    const methods = [
        // Static methods
        { class_name: 'javax/microedition/rms/RecordStore', method_name: 'openRecordStore',
          descriptor: '(Ljava/lang/String;Z)Ljavax/microedition/rms/RecordStore;',
          handler: native_rms_openRecordStore },
        { class_name: 'javax/microedition/rms/RecordStore', method_name: 'openRecordStore',
          descriptor: '(Ljava/lang/String;ZIZ)Ljavax/microedition/rms/RecordStore;',
          handler: native_rms_openRecordStore_authmode },
        { class_name: 'javax/microedition/rms/RecordStore', method_name: 'openRecordStore',
          descriptor: '(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljavax/microedition/rms/RecordStore;',
          handler: native_rms_openRecordStore_vendor },
        { class_name: 'javax/microedition/rms/RecordStore', method_name: 'deleteRecordStore',
          descriptor: '(Ljava/lang/String;)V',
          handler: native_rms_deleteRecordStore },
        { class_name: 'javax/microedition/rms/RecordStore', method_name: 'listRecordStores',
          descriptor: '()[Ljava/lang/String;',
          handler: native_rms_listRecordStores },

        // Instance methods
        { class_name: 'javax/microedition/rms/RecordStore', method_name: 'closeRecordStore',
          descriptor: '()V', handler: native_rms_closeRecordStore },
        { class_name: 'javax/microedition/rms/RecordStore', method_name: 'addRecord',
          descriptor: '([BII)I', handler: native_rms_addRecord },
        { class_name: 'javax/microedition/rms/RecordStore', method_name: 'getRecord',
          descriptor: '(I)[B', handler: native_rms_getRecord },
        { class_name: 'javax/microedition/rms/RecordStore', method_name: 'getRecord',
          descriptor: '(I[BI)I', handler: native_rms_getRecord_array },
        { class_name: 'javax/microedition/rms/RecordStore', method_name: 'getRecordSize',
          descriptor: '(I)I', handler: native_rms_getRecordSize },
        { class_name: 'javax/microedition/rms/RecordStore', method_name: 'setRecord',
          descriptor: '(I[BII)V', handler: native_rms_setRecord },
        { class_name: 'javax/microedition/rms/RecordStore', method_name: 'deleteRecord',
          descriptor: '(I)V', handler: native_rms_deleteRecord },
        { class_name: 'javax/microedition/rms/RecordStore', method_name: 'getNumRecords',
          descriptor: '()I', handler: native_rms_getNumRecords },
        { class_name: 'javax/microedition/rms/RecordStore', method_name: 'enumerateRecords',
          descriptor: '(Ljavax/microedition/rms/RecordFilter;Ljavax/microedition/rms/RecordComparator;Z)Ljavax/microedition/rms/RecordEnumeration;',
          handler: native_rms_enumerateRecords },
        { class_name: 'javax/microedition/rms/RecordStore', method_name: 'getNextRecordID',
          descriptor: '()I', handler: native_rms_getNextRecordID },
        { class_name: 'javax/microedition/rms/RecordStore', method_name: 'getVersion',
          descriptor: '()I', handler: native_rms_getVersion },
        { class_name: 'javax/microedition/rms/RecordStore', method_name: 'getLastModified',
          descriptor: '()J', handler: native_rms_getLastModified },
        { class_name: 'javax/microedition/rms/RecordStore', method_name: 'getSize',
          descriptor: '()I', handler: native_rms_getSize },
        { class_name: 'javax/microedition/rms/RecordStore', method_name: 'getSizeAvailable',
          descriptor: '()I', handler: native_rms_getSizeAvailable },
        { class_name: 'javax/microedition/rms/RecordStore', method_name: 'getName',
          descriptor: '()Ljava/lang/String;', handler: native_rms_getName },
        { class_name: 'javax/microedition/rms/RecordStore', method_name: 'setMode',
          descriptor: '(IZ)V', handler: native_rms_setMode },
        { class_name: 'javax/microedition/rms/RecordStore', method_name: 'addRecordListener',
          descriptor: '(Ljavax/microedition/rms/RecordListener;)V', handler: native_rms_addRecordListener },
        { class_name: 'javax/microedition/rms/RecordStore', method_name: 'removeRecordListener',
          descriptor: '(Ljavax/microedition/rms/RecordListener;)V', handler: native_rms_removeRecordListener },
    ];

    // RecordEnumeration interface + RecordEnumerationImpl concrete class share handlers
    const enum_methods_base = [
        { method_name: 'hasNextElement',     descriptor: '()Z',   handler: native_enumeration_hasNextElement },
        { method_name: 'hasPreviousElement', descriptor: '()Z',   handler: native_enumeration_hasPreviousElement },
        { method_name: 'nextRecordId',       descriptor: '()I',   handler: native_enumeration_nextRecordId },
        { method_name: 'nextRecord',         descriptor: '()[B',  handler: native_enumeration_nextRecord },
        { method_name: 'previousRecordId',   descriptor: '()I',   handler: native_enumeration_previousRecordId },
        { method_name: 'previousRecord',     descriptor: '()[B',  handler: native_enumeration_previousRecord },
        { method_name: 'destroy',            descriptor: '()V',   handler: native_enumeration_destroy },
        { method_name: 'numRecords',         descriptor: '()I',   handler: native_enumeration_numRecords },
        { method_name: 'reset',              descriptor: '()V',   handler: native_enumeration_reset },
        { method_name: 'rebuild',            descriptor: '()V',   handler: native_enumeration_rebuild },
        { method_name: 'isKeptUpdated',      descriptor: '()Z',   handler: native_enumeration_isKeptUpdated },
        { method_name: 'keepUpdated',        descriptor: '(Z)V',  handler: native_enumeration_keepUpdated },
    ];

    for (const cls of [
        'javax/microedition/rms/RecordEnumeration',
        'javax/microedition/rms/RecordEnumerationImpl',
    ]) {
        for (const m of enum_methods_base) {
            methods.push({ class_name: cls, ...m });
        }
    }

    native_register_methods(jvm, methods);
}

// ---------------------------------------------------------------------------
// Stubs for JVM bridge functions used above.
//
// In the full migration these will be imported from the respective modules.
// They are defined here as stubs so this module is self-contained and
// testable without the rest of the JVM being migrated.
// ---------------------------------------------------------------------------

// T_BYTE / T_INT / DESC_OBJECT — type constants from opcodes.h
const T_BYTE   = 8;
const T_INT    = 10;
const DESC_OBJECT = 'L'.charCodeAt(0);  // 76

function string_utf8(jvm, str_obj) {
    if (!str_obj) return '';
    if (str_obj.utf8 != null) return str_obj.utf8;
    if (str_obj.chars) {
        // UTF-16 to UTF-8 via Node.js string
        return String.fromCharCode(...str_obj.chars);
    }
    return '';
}

function jvm_load_class(jvm, class_name) {
    if (!jvm) return null;
    if (typeof jvm.load_class === 'function') return jvm.load_class(class_name);
    // Fallback: look in class_loader.classes array
    const classes = jvm.class_loader?.classes ?? [];
    return classes.find(c => c?.class_name === class_name) ?? null;
}

function jvm_new_object(jvm, clazz) {
    if (!jvm || !clazz) return null;
    if (typeof jvm.new_object === 'function') return jvm.new_object(clazz);
    // Minimal plain-object fallback
    return { header: { clazz }, fields: new Array(clazz.fields_count ?? 0).fill(null).map(() => ({ i: 0, ref: null, j: 0n, raw: 0 })) };
}

function jvm_new_array(jvm, type, length, element_class) {
    if (!jvm) return null;
    if (typeof jvm.new_array === 'function') return jvm.new_array(type, length, element_class);
    // Minimal fallback: array-like object with a typed data buffer
    const arr = {
        header: { clazz: null },
        length,
        element_type: type,
        element_class,
        _data: type === T_INT ? new Int32Array(length) : new Uint8Array(length),
    };
    return arr;
}

function array_data(arr) {
    if (!arr) return new Uint8Array(0);
    if (arr._data) return arr._data instanceof Uint8Array ? arr._data : new Uint8Array(arr._data.buffer);
    // JVM stores data after the struct; in JS this is exposed via a _data field
    return new Uint8Array(0);
}

function array_data_int32(arr) {
    if (!arr) return new Int32Array(0);
    if (arr._data instanceof Int32Array) return arr._data;
    if (arr._data) return new Int32Array(arr._data.buffer);
    return new Int32Array(0);
}

function array_set_ref(arr, idx, val) {
    if (!arr) return;
    if (typeof arr.set_ref === 'function') { arr.set_ref(idx, val); return; }
    if (!arr._refs) arr._refs = [];
    arr._refs[idx] = val;
}

function heap_alloc_object(jvm, clazz) {
    return jvm_new_object(jvm, clazz);
}

function jvm_new_string(jvm, utf8) {
    if (!jvm) return null;
    if (typeof jvm.new_string === 'function') return jvm.new_string(utf8);
    return { header: { clazz: null }, length: utf8.length, utf8, chars: null };
}

function jvm_throw_by_name(jvm, class_name, message) {
    if (!jvm) return;
    if (typeof jvm.throw_by_name === 'function') { jvm.throw_by_name(class_name, message); return; }
    if (!jvm._pending_exception) jvm._pending_exception = { class_name, message };
}

function jvm_exception_pending(jvm) {
    if (!jvm) return null;
    if (typeof jvm.exception_pending === 'function') return jvm.exception_pending();
    return jvm._pending_exception ?? null;
}

function jvm_invoke_virtual(jvm, obj, method_name, descriptor, args, result) {
    if (!jvm) return -1;
    if (typeof jvm.invoke_virtual === 'function') {
        return jvm.invoke_virtual(obj, method_name, descriptor, args, result);
    }
    return -1;
}

function native_throw_npe(jvm, thread) {
    jvm_throw_by_name(jvm, 'java/lang/NullPointerException', null);
    if (thread) thread.pending_exception = jvm_exception_pending(jvm);
}

function native_throw_aioobe(jvm, thread, index) {
    jvm_throw_by_name(jvm, 'java/lang/ArrayIndexOutOfBoundsException', String(index));
    if (thread) thread.pending_exception = jvm_exception_pending(jvm);
}

function native_register_methods(jvm, methods) {
    if (!jvm) return;
    if (typeof jvm.register_methods === 'function') { jvm.register_methods(methods); return; }
    // Fallback: store in jvm._native_methods map
    if (!jvm._native_methods) jvm._native_methods = [];
    jvm._native_methods.push(...methods);
}
