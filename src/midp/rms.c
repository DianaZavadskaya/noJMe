/*
 * J2ME Emulator - MIDP2 RMS (Record Management System)
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include "debug.h"
#include "debug_macros.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#define mkdir_p(path) _mkdir(path)
#else
#include <unistd.h>
#define mkdir_p(path) mkdir(path, 0755)
#endif

#include "midp.h"
#include "jvm.h"
#include "native.h"
#include "heap.h"
#include "opcodes.h"

/* Maximum record stores */
#define MAX_RECORD_STORES 64
#define MAX_RECORDS_PER_STORE 1024
#define MAX_RECORD_SIZE 65536  /* 64KB max per record */

/* Record structure */
typedef struct {
    int id;
    uint8_t* data;
    int size;
    bool valid;
} Record;

/* Record store structure */
typedef struct {
    char* name;
    Record records[MAX_RECORDS_PER_STORE];
    int next_id;
    bool open;
    int ref_count;
    int version;           /* Incremented on every add/set/delete operation */
    jlong last_modified;   /* Timestamp of last mutation (ms since epoch) */
} NativeRecordStore;

/* Forward declaration */
static jlong rms_current_time_ms(void);

/* Record stores */
static NativeRecordStore record_stores[MAX_RECORD_STORES];
static int record_store_count = 0;
static bool rms_initialized = false;

/* Pre-loaded records for protection bypass */
typedef struct {
    char store_name[64];
    int record_id;
    uint8_t data[256];
    int data_len;
    bool valid;
} PreloadedRecord;

#define MAX_PRELOADED 32
static PreloadedRecord preloaded_records[MAX_PRELOADED];
static int preloaded_count = 0;

/* ============================================
 * RMS Disk Persistence (libretro save paths)
 * ============================================ */

/* RMS save directory and game name */
static char rms_save_dir[512] = {0};   /* e.g. "/home/user/.config/retroarch/saves/J2ME" */
static char rms_game_name[256] = {0};  /* e.g. "game" (from game.jar filename) */

/* Magic bytes and version for RMS save file format */
#define RMS_FILE_MAGIC  0x524D5300  /* "RMS\0" */
#define RMS_FILE_VERSION 1

/* Helper: create directory tree recursively (up to 3 levels deep) */
static void rms_ensure_dir(const char* dir) {
    if (!dir || !dir[0]) return;
    
    /* Try creating the directory - if it exists, that's fine */
    if (mkdir_p(dir) == 0) return;
    
    /* If it failed, try creating parent directories */
    char tmp[512];
    strncpy(tmp, dir, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    
    /* Find last separator and try creating parent */
    for (int i = strlen(tmp) - 1; i > 0; i--) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            tmp[i] = '\0';
            rms_ensure_dir(tmp);
            break;
        }
    }
    
    /* Try again */
    mkdir_p(dir);
}

/* Build full path for a record store file: <save_dir>/<game_name>/<store_name>.rms */
static void rms_build_filepath(const char* store_name, char* out, int out_size) {
    if (out_size > 0) out[0] = '\0';
    if (!rms_save_dir[0] || !rms_game_name[0]) return;
    
    snprintf(out, out_size, "%s/%s/%s.rms", rms_save_dir, rms_game_name, store_name);
    
    /* Sanitize: replace characters not safe for filenames */
    for (char* p = out; *p; p++) {
        if (*p == '\\' || *p == ':') *p = '_';
    }
}

/* Write a 32-bit little-endian value */
static void rms_write_u32(FILE* f, uint32_t val) {
    uint8_t buf[4] = {
        (uint8_t)(val & 0xFF),
        (uint8_t)((val >> 8) & 0xFF),
        (uint8_t)((val >> 16) & 0xFF),
        (uint8_t)((val >> 24) & 0xFF)
    };
    fwrite(buf, 1, 4, f);
}

/* Read a 32-bit little-endian value */
static uint32_t rms_read_u32(FILE* f) {
    uint8_t buf[4];
    if (fread(buf, 1, 4, f) != 4) return 0;
    return buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
}

/* Save a single record store to disk */
static void rms_save_store_to_disk(int handle) {
    if (handle < 0 || handle >= MAX_RECORD_STORES) return;
    if (!rms_save_dir[0] || !rms_game_name[0]) return;
    
    NativeRecordStore* store = &record_stores[handle];
    if (!store->name) return;
    
    /* Build file path */
    char filepath[1024];
    rms_build_filepath(store->name, filepath, sizeof(filepath));
    if (!filepath[0]) return;
    
    /* Ensure directory exists */
    char dirpath[1024];
    snprintf(dirpath, sizeof(dirpath), "%s/%s", rms_save_dir, rms_game_name);
    rms_ensure_dir(dirpath);
    
    /* Open file for writing */
    FILE* f = fopen(filepath, "wb");
    if (!f) {
        RMS_DEBUG("RMS SAVE: Failed to open file for writing: %s", filepath);
        return;
    }
    
    /* Write header */
    rms_write_u32(f, RMS_FILE_MAGIC);
    rms_write_u32(f, RMS_FILE_VERSION);
    rms_write_u32(f, (uint32_t)store->next_id);
    
    /* Count and write records */
    uint32_t record_count = 0;
    for (int i = 1; i < MAX_RECORDS_PER_STORE; i++) {
        if (store->records[i].valid) record_count++;
    }
    rms_write_u32(f, record_count);
    
    /* Write each record */
    for (int i = 1; i < MAX_RECORDS_PER_STORE; i++) {
        if (!store->records[i].valid) continue;
        
        rms_write_u32(f, (uint32_t)store->records[i].id);
        rms_write_u32(f, (uint32_t)store->records[i].size);
        
        if (store->records[i].size > 0 && store->records[i].data) {
            fwrite(store->records[i].data, 1, store->records[i].size, f);
        }
    }
    
    fclose(f);
    RMS_DEBUG("RMS SAVE: Saved store '%s' with %u records to %s", 
            store->name, record_count, filepath);
}

/* Load a single record store from disk */
static bool rms_load_store_from_disk(NativeRecordStore* store) {
    if (!store || !store->name) return false;
    if (!rms_save_dir[0] || !rms_game_name[0]) return false;
    
    /* Build file path */
    char filepath[1024];
    rms_build_filepath(store->name, filepath, sizeof(filepath));
    if (!filepath[0]) return false;
    
    /* Open file for reading */
    FILE* f = fopen(filepath, "rb");
    if (!f) {
        RMS_DEBUG("RMS LOAD: No saved file for store '%s' at %s", store->name, filepath);
        return false;
    }
    
    /* Read and verify header */
    uint32_t magic = rms_read_u32(f);
    if (magic != RMS_FILE_MAGIC) {
        RMS_DEBUG("RMS LOAD: Bad magic in %s (got 0x%08X)", filepath, magic);
        fclose(f);
        return false;
    }
    
    uint32_t version = rms_read_u32(f);
    if (version != RMS_FILE_VERSION) {
        RMS_DEBUG("RMS LOAD: Unsupported version %u in %s", version, filepath);
        fclose(f);
        return false;
    }
    
    uint32_t next_id = rms_read_u32(f);
    uint32_t record_count = rms_read_u32(f);
    
    RMS_DEBUG("RMS LOAD: Store '%s': next_id=%u, records=%u", 
            store->name, next_id, record_count);
    
    /* Read records */
    uint32_t loaded = 0;
    for (uint32_t r = 0; r < record_count && r < (uint32_t)MAX_RECORDS_PER_STORE; r++) {
        uint32_t rec_id = rms_read_u32(f);
        uint32_t rec_size = rms_read_u32(f);
        
        if (rec_id < 1 || rec_id >= MAX_RECORDS_PER_STORE || rec_size > MAX_RECORD_SIZE) {
            RMS_DEBUG("RMS LOAD: Skipping bad record id=%u size=%u", rec_id, rec_size);
            /* Try to skip the data */
            if (rec_size <= MAX_RECORD_SIZE) {
                fseek(f, rec_size, SEEK_CUR);
            }
            continue;
        }
        
        /* Allocate and read data */
        uint8_t* data = NULL;
        if (rec_size > 0) {
            data = (uint8_t*)malloc(rec_size);
            if (!data) {
                fseek(f, rec_size, SEEK_CUR);
                continue;
            }
            if (fread(data, 1, rec_size, f) != rec_size) {
                free(data);
                continue;
            }
        }
        
        /* Free existing record if any (e.g. from preloaded) */
        if (store->records[rec_id].valid) {
            free(store->records[rec_id].data);
        }
        
        store->records[rec_id].id = (int)rec_id;
        store->records[rec_id].data = data;
        store->records[rec_id].size = (int)rec_size;
        store->records[rec_id].valid = true;
        
        loaded++;
    }
    
    fclose(f);
    
    /* Update next_id from disk if higher */
    if ((int)next_id > store->next_id) {
        store->next_id = (int)next_id;
    }
    
    RMS_DEBUG("RMS LOAD: Loaded %u records for store '%s'", loaded, store->name);
    return loaded > 0;
}

/* Delete a record store file from disk */
static void rms_delete_store_file(const char* name) {
    if (!name || !rms_save_dir[0] || !rms_game_name[0]) return;
    
    char filepath[1024];
    rms_build_filepath(name, filepath, sizeof(filepath));
    if (filepath[0]) {
        remove(filepath);
        RMS_DEBUG("RMS: Deleted file %s", filepath);
    }
}

/* Set the save directory and game name (called from libretro core) */
void midp_rms_set_save_path(const char* save_dir, const char* game_name) {
    if (save_dir) {
        strncpy(rms_save_dir, save_dir, sizeof(rms_save_dir) - 1);
        rms_save_dir[sizeof(rms_save_dir) - 1] = '\0';
    }
    if (game_name) {
        strncpy(rms_game_name, game_name, sizeof(rms_game_name) - 1);
        rms_game_name[sizeof(rms_game_name) - 1] = '\0';
    }
    RMS_DEBUG("RMS: save_dir='%s', game_name='%s'", rms_save_dir, rms_game_name);
}

/* Save all record stores to disk (called on game unload) */
void midp_rms_save_all(void) {
    if (!rms_save_dir[0]) return;
    
    int saved = 0;
    for (int i = 0; i < MAX_RECORD_STORES; i++) {
        if (record_stores[i].name && record_stores[i].open) {
            rms_save_store_to_disk(i);
            saved++;
        }
    }
    
    RMS_DEBUG("RMS: Saved %d stores to disk", saved);
}

/* Convert hex string to bytes */
static int hex_to_bytes(const char* hex, uint8_t* out, int max_len) {
    int len = 0;
    while (hex[0] && hex[1] && len < max_len) {
        if (hex[0] == ' ' || hex[0] == '\t') { hex++; continue; }
        int val = 0;
        for (int i = 0; i < 2; i++) {
            val <<= 4;
            char c = hex[i];
            if (c >= '0' && c <= '9') val |= (c - '0');
            else if (c >= 'a' && c <= 'f') val |= (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') val |= (c - 'A' + 10);
            else return len;
        }
        out[len++] = val;
        hex += 2;
    }
    return len;
}

/* Load preloaded records from config file */
static void load_preloaded_records(void) {
    FILE* f = fopen("rms_bypass.conf", "r");
    if (!f) return;
    
    char line[512];
    while (fgets(line, sizeof(line), f) && preloaded_count < MAX_PRELOADED) {
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        
        // Parse: store_name.record_id = hex_data
        char* eq = strchr(line, '=');
        if (!eq) continue;
        
        *eq = '\0';
        char* store_part = line;
        char* hex_data = eq + 1;
        
        // Trim whitespace
        while (*store_part == ' ' || *store_part == '\t') store_part++;
        while (*hex_data == ' ' || *hex_data == '\t') hex_data++;
        
        // Find the dot separator
        char* dot = strchr(store_part, '.');
        if (!dot) continue;
        
        *dot = '\0';
        char* id_str = dot + 1;
        
        // Remove trailing whitespace/newlines from hex_data
        char* end = hex_data + strlen(hex_data) - 1;
        while (end > hex_data && (*end == '\n' || *end == '\r' || *end == ' ')) *end-- = '\0';
        
        // Store the record
        PreloadedRecord* pr = &preloaded_records[preloaded_count];
        strncpy(pr->store_name, store_part, sizeof(pr->store_name) - 1);
        pr->record_id = atoi(id_str);
        pr->data_len = hex_to_bytes(hex_data, pr->data, sizeof(pr->data));
        pr->valid = true;
        
        RMS_DEBUG("Preloaded: store='%s' id=%d len=%d", 
                pr->store_name, pr->record_id, pr->data_len);
        
        preloaded_count++;
    }
    
    fclose(f);
    RMS_DEBUG("Loaded %d preloaded records from config", preloaded_count);
}

/* Initialize RMS */
static void rms_init(void) {
    if (rms_initialized) return;
    
    memset(record_stores, 0, sizeof(record_stores));
    load_preloaded_records();
    rms_initialized = true;
}

/* Open/create record store - returns native handle */
static int rms_open(const char* name, bool create_if_necessary) {
    rms_init();
    
    if (!name) return -1;
    
    /* Find existing store */
    for (int i = 0; i < MAX_RECORD_STORES; i++) {
        if (record_stores[i].name && strcmp(record_stores[i].name, name) == 0) {
            record_stores[i].ref_count++;
            record_stores[i].open = true;
            RMS_DEBUG("Reopened existing store '%s' (handle=%d, refs=%d)", 
                    name, i, record_stores[i].ref_count);
            return i;
        }
    }
    
    if (!create_if_necessary) {
        RMS_DEBUG("Store '%s' not found and create=false", name);
        return -1;  /* RecordStoreNotFoundException */
    }
    
    /* Find a free slot (not just at the end, since we don't shift anymore) */
    int handle = -1;
    for (int i = 0; i < MAX_RECORD_STORES; i++) {
        if (!record_stores[i].name) {
            handle = i;
            break;
        }
    }
    
    if (handle < 0) {
        RMS_DEBUG("Max stores reached (%d)", MAX_RECORD_STORES);
        return -2;  /* RecordStoreFullException */
    }
    
    NativeRecordStore* store = &record_stores[handle];
    
    store->name = strdup(name);
    if (!store->name) {
        RMS_DEBUG("Out of memory for store name");
        return -2;
    }
    
    store->next_id = 1;
    store->open = true;
    store->ref_count = 1;
    
    memset(store->records, 0, sizeof(store->records));
    
    /* Apply preloaded records for this store */
    for (int i = 0; i < preloaded_count; i++) {
        PreloadedRecord* pr = &preloaded_records[i];
        if (strcmp(pr->store_name, name) == 0 && pr->valid) {
            int rid = pr->record_id;
            if (rid > 0 && rid < MAX_RECORDS_PER_STORE) {
                store->records[rid].id = rid;
                store->records[rid].data = malloc(pr->data_len);
                if (store->records[rid].data) {
                    memcpy(store->records[rid].data, pr->data, pr->data_len);
                    store->records[rid].size = pr->data_len;
                    store->records[rid].valid = true;
                    RMS_DEBUG("Applied preloaded record: store='%s' id=%d len=%d",
                            name, rid, pr->data_len);
                }
                if (rid >= store->next_id) {
                    store->next_id = rid + 1;
                }
            }
        }
    }
    
    /* Load record store from disk if saved data exists.
     * Disk data takes priority over preloaded records. */
    rms_load_store_from_disk(store);
    
    /* Update count for high-water mark */
    if (handle >= record_store_count) {
        record_store_count = handle + 1;
    }
    
    RMS_DEBUG("Created new store '%s' with handle %d", name, handle);
    
    return handle;
}

/* Get native record store from Java RecordStore object */
static NativeRecordStore* get_native_store(JavaObject* rs_obj) {
    if (!rs_obj) {
        RMS_DEBUG("get_native_store: NULL object");
        return NULL;
    }
    
    /* Проверка валидности объекта */
    if (!rs_obj->header.clazz) {
        RMS_DEBUG("get_native_store: Object has NULL class!");
        return NULL;
    }
    
    JavaClass* clazz = rs_obj->header.clazz;
    
    /* Find nativeHandle field */
    for (int i = 0; i < clazz->fields_count; i++) {
        if (clazz->fields[i].name && strcmp(clazz->fields[i].name, "nativeHandle") == 0) {
            int handle = rs_obj->fields[i].i;
            
            /* Проверка валидности handle */
            if (handle >= 0 && handle < MAX_RECORD_STORES) {
                NativeRecordStore* store = &record_stores[handle];
                if (store->name && store->open) {
                    return store;
                } else if (!store->name) {
                    RMS_DEBUG("get_native_store: store %d has been deleted", handle);
                    return NULL;
                } else {
                    RMS_DEBUG("get_native_store: store %d is closed", handle);
                    return NULL;
                }
            } else {
                RMS_DEBUG("get_native_store: invalid handle %d (max=%d)", 
                        handle, MAX_RECORD_STORES);
                return NULL;
            }
        }
    }
    
    RMS_DEBUG("get_native_store: nativeHandle field not found in class %s",
            clazz->class_name ? clazz->class_name : "?");
    return NULL;
}

/* Get handle by store pointer */
static int rms_get_handle(NativeRecordStore* store) {
    if (!store) return -1;
    for (int i = 0; i < MAX_RECORD_STORES; i++) {
        if (&record_stores[i] == store) return i;
    }
    return -1;
}

/*
 * Native methods for RecordStore
 */

/* RecordStore.openRecordStore(String, boolean) - returns RecordStore object */
static JavaValue native_rms_openRecordStore(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)arg_count;
    
    /* STATIC METHOD - no 'this' argument!
     * args[0] - name (String)
     * args[1] - create (boolean)
     */
    JavaString* name_str = (JavaString*)args[0].ref;
    jboolean create = args[1].i;
    
    if (!name_str) {
        RMS_DEBUG("openRecordStore: name is NULL");
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    const char* name = string_utf8(jvm, name_str);
    RMS_DEBUG("openRecordStore: '%s', create=%d", name, create);
    
    int handle = rms_open(name, create != 0);
    
    if (handle < 0) {
        /* Бросаем соответствующее исключение */
        if (handle == -1) {
            jvm_throw_by_name(jvm, "javax/microedition/rms/RecordStoreNotFoundException", name);
        } else {
            jvm_throw_by_name(jvm, "javax/microedition/rms/RecordStoreFullException", NULL);
        }
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_NULL();
    }
    
    /* Load RecordStore class */
    JavaClass* rs_class = jvm_load_class(jvm, "javax/microedition/rms/RecordStore");
    if (!rs_class) {
        RMS_DEBUG("Failed to load RecordStore class");
        return NATIVE_RETURN_NULL();
    }
    
    /* Create RecordStore Java object */
    JavaObject* rs_obj = jvm_new_object(jvm, rs_class);
    if (!rs_obj) {
        RMS_DEBUG("Failed to create RecordStore object");
        return NATIVE_RETURN_NULL();
    }
    
    /* Store native handle in nativeHandle field */
    bool found = false;
    for (int i = 0; i < rs_class->fields_count; i++) {
        if (rs_class->fields[i].name && strcmp(rs_class->fields[i].name, "nativeHandle") == 0) {
            rs_obj->fields[i].i = handle;
            RMS_DEBUG("Set nativeHandle=%d in field %d", handle, i);
            found = true;
            break;
        }
    }
    
    if (!found) {
        RMS_DEBUG("WARNING: nativeHandle field not found in RecordStore class");
    }
    
    RMS_DEBUG("Returning RecordStore object: %p", (void*)rs_obj);
    return NATIVE_RETURN_OBJECT(rs_obj);
}

/* RecordStore.closeRecordStore() */
static JavaValue native_rms_closeRecordStore(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* rs_obj = (JavaObject*)args[0].ref;
    
    if (!rs_obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    NativeRecordStore* store = get_native_store(rs_obj);
    if (store) {
        store->ref_count--;
        RMS_DEBUG("closeRecordStore: refs now %d", store->ref_count);
        
        if (store->ref_count <= 0) {
            store->open = false;
            RMS_DEBUG("Store closed");
            
            /* Save to disk when last reference is closed */
            int h = rms_get_handle(store);
            if (h >= 0) rms_save_store_to_disk(h);
        }
    } else {
        RMS_DEBUG("closeRecordStore: invalid store");
    }
    
    return NATIVE_RETURN_VOID();
}

/* RecordStore.addRecord(byte[], int, int) - returns record ID */
static JavaValue native_rms_addRecord(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* rs_obj = (JavaObject*)args[0].ref;
    JavaArray* data = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint length = args[3].i;
    
    RMS_DEBUG("addRecord: offset=%d, length=%d", offset, length);
    
    if (!rs_obj || !data) {
        RMS_DEBUG("addRecord: NULL parameter");
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Проверка длины */
    if (length <= 0) {
        RMS_DEBUG("addRecord: invalid length %d", length);
        return NATIVE_RETURN_INT(-1);
    }
    
    if (length > MAX_RECORD_SIZE) {
        RMS_DEBUG("addRecord: record too large (%d > %d)", 
                length, MAX_RECORD_SIZE);
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Проверка границ массива */
    if (offset < 0 || length < 0 || offset + length > (jint)data->length) {
        RMS_DEBUG("addRecord: array bounds error: offset=%d, length=%d, array_len=%d",
                offset, length, data->length);
        native_throw_aioobe(jvm, thread, offset);
        return NATIVE_RETURN_INT(-1);
    }
    
    NativeRecordStore* store = get_native_store(rs_obj);
    if (!store) {
        RMS_DEBUG("addRecord: invalid store");
        jvm_throw_by_name(jvm, "javax/microedition/rms/RecordStoreNotOpenException", NULL);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_INT(-1);
    }
    
    if (!store->open) {
        RMS_DEBUG("addRecord: store is closed");
        jvm_throw_by_name(jvm, "javax/microedition/rms/RecordStoreNotOpenException", NULL);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Find free slot */
    int id = -1;
    for (int i = 1; i < MAX_RECORDS_PER_STORE; i++) {
        if (!store->records[i].valid) {
            id = i;
            break;
        }
    }
    
    if (id < 0) {
        RMS_DEBUG("addRecord: no free slots");
        jvm_throw_by_name(jvm, "javax/microedition/rms/RecordStoreFullException", NULL);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Allocate and copy data */
    uint8_t* record_data = (uint8_t*)malloc(length);
    if (!record_data) {
        RMS_DEBUG("addRecord: out of memory");
        jvm_throw_by_name(jvm, "java/lang/OutOfMemoryError", NULL);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_INT(-1);
    }
    
    uint8_t* src_data = (uint8_t*)array_data(data);
    memcpy(record_data, src_data + offset, length);
    
    /* Store record */
    store->records[id].id = id;
    store->records[id].data = record_data;
    store->records[id].size = length;
    store->records[id].valid = true;
    
    /* Update next_id if needed */
    if (id >= store->next_id) {
        store->next_id = id + 1;
    }
    
    /* Update version and lastModified */
    store->version++;
    store->last_modified = rms_current_time_ms();
    
    RMS_DEBUG("addRecord: SUCCESS - id=%d, size=%d, next_id=%d", 
            id, length, store->next_id);
    
    /* Save to disk */
    int handle = rms_get_handle(store);
    if (handle >= 0) rms_save_store_to_disk(handle);
    
    return NATIVE_RETURN_INT(id);
}

/* RecordStore.getRecord(int) - returns byte[] */
static JavaValue native_rms_getRecord(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* rs_obj = (JavaObject*)args[0].ref;
    jint record_id = args[1].i;
    
    RMS_DEBUG("getRecord: record_id=%d", record_id);
    
    if (!rs_obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    NativeRecordStore* store = get_native_store(rs_obj);
    if (!store) {
        RMS_DEBUG("getRecord: invalid store");
        jvm_throw_by_name(jvm, "javax/microedition/rms/RecordStoreNotOpenException", NULL);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_NULL();
    }
    
    if (!store->open) {
        RMS_DEBUG("getRecord: store is closed");
        jvm_throw_by_name(jvm, "javax/microedition/rms/RecordStoreNotOpenException", NULL);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_NULL();
    }
    
    if (record_id < 1 || record_id >= MAX_RECORDS_PER_STORE) {
        RMS_DEBUG("getRecord: invalid record_id=%d", record_id);
        jvm_throw_by_name(jvm, "javax/microedition/rms/InvalidRecordIDException", NULL);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_NULL();
    }
    
    if (!store->records[record_id].valid) {
        RMS_DEBUG("getRecord: record %d not found - throwing InvalidRecordIDException", record_id);
        jvm_throw_by_name(jvm, "javax/microedition/rms/InvalidRecordIDException", NULL);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_NULL();
    }
    
    int length = store->records[record_id].size;
    uint8_t* data = store->records[record_id].data;
    
    RMS_DEBUG("getRecord: record %d has size=%d", record_id, length);
    
    /* Create byte array */
    JavaArray* byte_array = jvm_new_array(jvm, T_BYTE, (jsize)length, NULL);
    if (!byte_array) {
        RMS_DEBUG("getRecord: failed to allocate byte array");
        jvm_throw_by_name(jvm, "java/lang/OutOfMemoryError", NULL);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_NULL();
    }
    
    memcpy(array_data(byte_array), data, length);
    
    RMS_DEBUG("getRecord: returning %d bytes", length);
    return NATIVE_RETURN_OBJECT(byte_array);
}

/* RecordStore.getRecord(int, byte[], int) - returns size */
static JavaValue native_rms_getRecord_array(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* rs_obj = (JavaObject*)args[0].ref;
    jint record_id = args[1].i;
    JavaArray* dest = (JavaArray*)args[2].ref;
    jint offset = args[3].i;
    
    if (!rs_obj || !dest) {
        return NATIVE_RETURN_INT(-1);
    }
    
    NativeRecordStore* store = get_native_store(rs_obj);
    if (!store || !store->open) {
        return NATIVE_RETURN_INT(-1);
    }
    
    if (record_id < 1 || record_id >= MAX_RECORDS_PER_STORE) {
        return NATIVE_RETURN_INT(-1);
    }
    
    if (!store->records[record_id].valid) {
        /* Return 0 (empty) instead of -1 for missing records (emulator compat) */
        return NATIVE_RETURN_INT(0);
    }
    
    int length = store->records[record_id].size;
    
    if (length == 0) return NATIVE_RETURN_INT(0);
    
    if (offset < 0 || length > (jint)dest->length - offset) {
        return NATIVE_RETURN_INT(-1);
    }
    
    memcpy((uint8_t*)array_data(dest) + offset, store->records[record_id].data, length);
    
    return NATIVE_RETURN_INT(length);
}

/* RecordStore.getRecordSize(int) - returns size */
static JavaValue native_rms_getRecordSize(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* rs_obj = (JavaObject*)args[0].ref;
    jint record_id = args[1].i;
    
    if (!rs_obj) {
        return NATIVE_RETURN_INT(-1);
    }
    
    NativeRecordStore* store = get_native_store(rs_obj);
    if (!store || !store->open) {
        return NATIVE_RETURN_INT(-1);
    }
    
    if (record_id < 1 || record_id >= MAX_RECORDS_PER_STORE) {
        return NATIVE_RETURN_INT(-1);
    }
    
    if (!store->records[record_id].valid) {
        /* Return 0 for missing records (emulator compat) */
        return NATIVE_RETURN_INT(0);
    }
    
    return NATIVE_RETURN_INT(store->records[record_id].size);
}

/* RecordStore.setRecord(int, byte[], int, int) */
static JavaValue native_rms_setRecord(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* rs_obj = (JavaObject*)args[0].ref;
    jint record_id = args[1].i;
    JavaArray* data = (JavaArray*)args[2].ref;
    jint offset = args[3].i;
    jint length = args[4].i;
    
    if (!rs_obj || !data) {
        return NATIVE_RETURN_VOID();
    }
    
    NativeRecordStore* store = get_native_store(rs_obj);
    if (!store || !store->open) {
        return NATIVE_RETURN_VOID();
    }
    
    if (record_id < 1 || record_id >= MAX_RECORDS_PER_STORE) {
        return NATIVE_RETURN_VOID();
    }
    
    if (!store->records[record_id].valid) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Check bounds */
    if (offset < 0 || length < 0 || offset + length > (jint)data->length) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Allocate new data */
    uint8_t* new_data = (uint8_t*)malloc(length);
    if (!new_data) {
        return NATIVE_RETURN_VOID();
    }
    
    memcpy(new_data, (uint8_t*)array_data(data) + offset, length);
    
    /* Replace old data */
    free(store->records[record_id].data);
    store->records[record_id].data = new_data;
    store->records[record_id].size = length;
    
    /* Update version and lastModified */
    store->version++;
    store->last_modified = rms_current_time_ms();
    
    /* Save to disk */
    {
        int h = rms_get_handle(store);
        if (h >= 0) rms_save_store_to_disk(h);
    }
    
    return NATIVE_RETURN_VOID();
}

/* RecordStore.deleteRecord(int) */
static JavaValue native_rms_deleteRecord(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* rs_obj = (JavaObject*)args[0].ref;
    jint record_id = args[1].i;
    
    if (!rs_obj) {
        return NATIVE_RETURN_VOID();
    }
    
    NativeRecordStore* store = get_native_store(rs_obj);
    if (!store || !store->open) {
        return NATIVE_RETURN_VOID();
    }
    
    if (record_id < 1 || record_id >= MAX_RECORDS_PER_STORE) {
        return NATIVE_RETURN_VOID();
    }
    
    if (store->records[record_id].valid) {
        free(store->records[record_id].data);
        memset(&store->records[record_id], 0, sizeof(Record));
        
        /* Update version and lastModified */
        store->version++;
        store->last_modified = rms_current_time_ms();
        
        /* Save to disk */
        int h = rms_get_handle(store);
        if (h >= 0) rms_save_store_to_disk(h);
    }
    
    return NATIVE_RETURN_VOID();
}

/* RecordStore.getNumRecords() */
static JavaValue native_rms_getNumRecords(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* rs_obj = (JavaObject*)args[0].ref;
    
    if (!rs_obj) {
        return NATIVE_RETURN_INT(0);
    }
    
    NativeRecordStore* store = get_native_store(rs_obj);
    if (!store || !store->open) {
        return NATIVE_RETURN_INT(0);
    }
    
    int count = 0;
    for (int i = 1; i < MAX_RECORDS_PER_STORE; i++) {
        if (store->records[i].valid) count++;
    }
    
    return NATIVE_RETURN_INT(count);
}

/* RecordStore.enumerateRecords - возвращает RecordEnumeration объект */
static JavaValue native_rms_enumerateRecords(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* rs_obj = (JavaObject*)args[0].ref;
    JavaObject* filter_obj = (JavaObject*)args[1].ref;
    JavaObject* comparator_obj = (JavaObject*)args[2].ref;
    (void)args[3]; /* keepUpdated */
    
    RMS_DEBUG("enumerateRecords called (filter=%p, comparator=%p)", 
              (void*)filter_obj, (void*)comparator_obj);
    
    if (!rs_obj) {
        return NATIVE_RETURN_NULL();
    }
    
    NativeRecordStore* store = get_native_store(rs_obj);
    if (!store || !store->open) {
        return NATIVE_RETURN_NULL();
    }
    
    /* Collect valid record IDs, applying filter if present */
    int record_ids[MAX_RECORDS_PER_STORE];
    int count = 0;
    
    for (int i = 1; i < MAX_RECORDS_PER_STORE; i++) {
        if (!store->records[i].valid) continue;
        
        /* If filter is provided, call filter.matches(byte[]) */
        if (filter_obj) {
            /* Create byte array from record data */
            JavaArray* byte_arr = jvm_new_array(jvm, T_BYTE, store->records[i].size, NULL);
            if (byte_arr && store->records[i].data) {
                memcpy(array_data(byte_arr), store->records[i].data, store->records[i].size);
            }
            
            /* Call filter.matches(byte[]) -> boolean */
            JavaValue filter_args[1];
            filter_args[0].ref = byte_arr;
            JavaValue filter_result = { .i = 0 };
            int invoke_res = jvm_invoke_virtual(jvm, filter_obj, "matches",
                "([B)Z", filter_args, &filter_result);
            
            if (invoke_res != 0 || filter_result.i == 0) {
                continue;
            }
        }
        
        record_ids[count++] = i;
    }
    
    RMS_DEBUG("enumerateRecords: %d records passed filter", count);
    
    /* Sort using comparator if provided */
    if (comparator_obj && count > 1) {
        /* Insertion sort with JVM callback for comparison */
        for (int i = 1; i < count; i++) {
            int key_id = record_ids[i];
            int j = i - 1;
            
            while (j >= 0) {
                /* Get data for records being compared */
                int id_a = record_ids[j];
                int id_b = key_id;
                
                JavaArray* arr_a = jvm_new_array(jvm, T_BYTE, store->records[id_a].size, NULL);
                JavaArray* arr_b = jvm_new_array(jvm, T_BYTE, store->records[id_b].size, NULL);
                if (arr_a && store->records[id_a].data) {
                    memcpy(array_data(arr_a), store->records[id_a].data, store->records[id_a].size);
                }
                if (arr_b && store->records[id_b].data) {
                    memcpy(array_data(arr_b), store->records[id_b].data, store->records[id_b].size);
                }
                
                /* Call comparator.compare(byte[], byte[]) -> int
                 * Returns: EQUIVALENT=0, FOLLOWS=1, PRECEDES=-1 */
                JavaValue cmp_args[2];
                cmp_args[0].ref = arr_a;
                cmp_args[1].ref = arr_b;
                JavaValue cmp_result = { .i = 0 };
                jvm_invoke_virtual(jvm, comparator_obj, "compare",
                    "([B[B)I", cmp_args, &cmp_result);
                
                /* FOLLOWS (1) means rec1 should come after rec2,
                 * so we should shift. PRECEDES (-1) or EQUIVALENT (0) means stop. */
                if (cmp_result.i > 0) {
                    record_ids[j + 1] = record_ids[j];
                    j--;
                } else {
                    break;
                }
            }
            record_ids[j + 1] = key_id;
        }
        
        RMS_DEBUG("enumerateRecords: sorted %d records using comparator", count);
    }
    
    /* Load RecordEnumerationImpl class (concrete implementation) */
    JavaClass* enum_class = jvm_load_class(jvm, "javax/microedition/rms/RecordEnumerationImpl");
    if (!enum_class) {
        RMS_DEBUG("Failed to load RecordEnumerationImpl class");
        return NATIVE_RETURN_NULL();
    }
    
    /* Create RecordEnumerationImpl object */
    JavaObject* enum_obj = heap_alloc_object(jvm, enum_class);
    if (!enum_obj) {
        RMS_DEBUG("Failed to allocate RecordEnumeration");
        return NATIVE_RETURN_NULL();
    }
    
    /* Create int array with record IDs - store in first field */
    JavaArray* ids = jvm_new_array(jvm, T_INT, count > 0 ? count : 0, NULL);
    if (!ids && count > 0) {
        RMS_DEBUG("Failed to allocate IDs array");
        return NATIVE_RETURN_NULL();
    }
    
    if (count > 0 && ids) {
        jint* ids_data = (jint*)array_data(ids);
        for (int idx = 0; idx < count; idx++) {
            ids_data[idx] = record_ids[idx];
        }
    }
    
    /* Store IDs array in field 0, current index in field 1, store handle in field 2 */
    /* Ensure the class has enough fields */
    if (enum_class->instance_size < sizeof(ObjectHeader) + 3 * sizeof(JavaValue)) {
        enum_class->instance_size = sizeof(ObjectHeader) + 3 * sizeof(JavaValue);
    }
    
    /* Get the store handle from the RecordStore object */
    jint store_handle = -1;
    JavaClass* rs_class = rs_obj->header.clazz;
    if (rs_class && rs_class->fields) {
        for (int i = 0; i < rs_class->fields_count; i++) {
            if (rs_class->fields[i].name && strcmp(rs_class->fields[i].name, "nativeHandle") == 0) {
                store_handle = rs_obj->fields[i].i;
                break;
            }
        }
    }
    
    enum_obj->fields[0].ref = ids;       /* recordIds array */
    enum_obj->fields[1].i = 0;           /* currentIndex */
    enum_obj->fields[2].i = store_handle; /* store handle for nextRecord */
    
    RMS_DEBUG("Created RecordEnumeration with %d records, store_handle=%d", count, store_handle);
    
    return NATIVE_RETURN_OBJECT(enum_obj);
}

/* RecordStore.deleteRecordStore(String) - static method */
static JavaValue native_rms_deleteRecordStore(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)arg_count;
    /* STATIC METHOD - no 'this' argument!
     * args[0] - name (String)
     */
    JavaString* name_str = (JavaString*)args[0].ref;
    
    if (!name_str) {
        return NATIVE_RETURN_VOID();
    }
    
    const char* name = string_utf8(jvm, name_str);
    RMS_DEBUG("deleteRecordStore: '%s'", name);
    
    rms_init();
    
    /* Find store */
    for (int i = 0; i < record_store_count; i++) {
        if (record_stores[i].name && strcmp(record_stores[i].name, name) == 0) {
            /* Free all records */
            for (int j = 1; j < MAX_RECORDS_PER_STORE; j++) {
                if (record_stores[i].records[j].valid) {
                    free(record_stores[i].records[j].data);
                }
            }
            
            free(record_stores[i].name);
            record_stores[i].name = NULL;
            record_stores[i].open = false;
            record_stores[i].ref_count = 0;
            
            /* Delete the file from disk */
            rms_delete_store_file(name);
            
            /* DON'T shift the array - this would invalidate handles!
             * Just mark the slot as free by setting name to NULL.
             * The handle indices must remain stable for existing RecordStore objects.
             */
            RMS_DEBUG("Store deleted (slot %d marked free)", i);
            break;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* RecordStore.listRecordStores() - returns String[] */
static JavaValue native_rms_listRecordStores(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)args; (void)arg_count;
    
    rms_init();
    
    /* Always return an array (even if empty), never null 
     * This matches some game expectations better than returning null */
    int count = 0;
    for (int i = 0; i < record_store_count; i++) {
        if (record_stores[i].name /* && record_stores[i].open */) {
            count++;
        }
    }
    
    /* Create String array */
    JavaArray* array = jvm_new_array(jvm, DESC_OBJECT, count, NULL);
    if (!array) {
        return NATIVE_RETURN_NULL();
    }
    
    int idx = 0;
    for (int i = 0; i < record_store_count && idx < count; i++) {
        if (record_stores[i].name /* && record_stores[i].open */) {
            JavaString* str = jvm_new_string(jvm, record_stores[i].name);
            array_set_ref(array, idx++, str);
        }
    }
    
    return NATIVE_RETURN_OBJECT(array);
}

/* RecordStore.getNextRecordID() */
static JavaValue native_rms_getNextRecordID(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* rs_obj = (JavaObject*)args[0].ref;
    
    if (!rs_obj) {
        return NATIVE_RETURN_INT(-1);
    }
    
    NativeRecordStore* store = get_native_store(rs_obj);
    if (!store || !store->open) {
        return NATIVE_RETURN_INT(-1);
    }
    
    return NATIVE_RETURN_INT(store->next_id);
}

/* RecordStore.getVersion() - returns version counter incremented on mutations */
static JavaValue native_rms_getVersion(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* rs_obj = (JavaObject*)args[0].ref;
    
    if (!rs_obj) {
        return NATIVE_RETURN_INT(0);
    }
    
    NativeRecordStore* store = get_native_store(rs_obj);
    if (!store) {
        return NATIVE_RETURN_INT(0);
    }
    
    return NATIVE_RETURN_INT(store->version);
}

/* Helper: get current time in milliseconds since epoch */
static jlong rms_current_time_ms(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    jlong ll = ((jlong)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (ll - 116444736000000000LL) / 10000;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (jlong)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* RecordStore.getLastModified() - returns timestamp of last mutation */
static JavaValue native_rms_getLastModified(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* rs_obj = (JavaObject*)args[0].ref;
    
    if (!rs_obj) {
        return NATIVE_RETURN_LONG(0);
    }
    
    NativeRecordStore* store = get_native_store(rs_obj);
    if (!store) {
        return NATIVE_RETURN_LONG(0);
    }
    
    return NATIVE_RETURN_LONG(store->last_modified);
}

/* RecordStore.getSize() - returns fixed 32767 to match FreeJ2ME behavior */
static JavaValue native_rms_getSize(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count; (void)args;
    /* FreeJ2ME returns constant 32767; games may rely on this */
    return NATIVE_RETURN_INT(32767);
}

/* RecordStore.getSizeAvailable() - returns 65536 to match FreeJ2ME */
static JavaValue native_rms_getSizeAvailable(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count; (void)args;
    /* FreeJ2ME returns 65536; games may check this */
    return NATIVE_RETURN_INT(65536);
}

/* RecordStore.getName() - имя хранилища */
static JavaValue native_rms_getName(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* rs_obj = (JavaObject*)args[0].ref;
    
    if (!rs_obj) {
        return NATIVE_RETURN_NULL();
    }
    
    NativeRecordStore* store = get_native_store(rs_obj);
    if (!store || !store->name) {
        return NATIVE_RETURN_NULL();
    }
    
    return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, store->name));
}

/* RecordEnumeration.hasNextElement() */
static JavaValue native_enumeration_hasNextElement(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* enum_obj = (JavaObject*)args[0].ref;
    
    if (!enum_obj) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Field 0 = recordIds array, Field 1 = currentIndex */
    JavaArray* ids = (JavaArray*)enum_obj->fields[0].ref;
    jint current = enum_obj->fields[1].i;
    
    if (!ids) {
        return NATIVE_RETURN_INT(0);
    }
    
    jboolean has_next = (current < (jint)ids->length) ? JNI_TRUE : JNI_FALSE;
    RMS_DEBUG("hasNextElement: current=%d, length=%d, result=%d", 
            current, ids->length, has_next);
    return NATIVE_RETURN_INT(has_next);
}

/* RecordEnumeration.nextRecordId() */
static JavaValue native_enumeration_nextRecordId(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* enum_obj = (JavaObject*)args[0].ref;
    
    if (!enum_obj) {
        jvm_throw_by_name(jvm, "javax/microedition/rms/InvalidRecordIDException", NULL);
        return NATIVE_RETURN_INT(-1);
    }
    
    JavaArray* ids = (JavaArray*)enum_obj->fields[0].ref;
    jint current = enum_obj->fields[1].i;
    
    if (!ids || current >= (jint)ids->length) {
        jvm_throw_by_name(jvm, "javax/microedition/rms/InvalidRecordIDException", NULL);
        return NATIVE_RETURN_INT(-1);
    }
    
    jint* ids_data = (jint*)array_data(ids);
    jint record_id = ids_data[current];
    
    /* Advance index */
    enum_obj->fields[1].i = current + 1;
    
    RMS_DEBUG("nextRecordId: returning %d", record_id);
    return NATIVE_RETURN_INT(record_id);
}

/* RecordEnumeration.nextRecord() - returns byte[] of next record */
static JavaValue native_enumeration_nextRecord(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* enum_obj = (JavaObject*)args[0].ref;
    
    if (!enum_obj) {
        jvm_throw_by_name(jvm, "javax/microedition/rms/InvalidRecordIDException", NULL);
        return NATIVE_RETURN_NULL();
    }
    
    JavaArray* ids = (JavaArray*)enum_obj->fields[0].ref;
    jint current = enum_obj->fields[1].i;
    
    if (!ids || current >= (jint)ids->length) {
        jvm_throw_by_name(jvm, "javax/microedition/rms/InvalidRecordIDException", NULL);
        return NATIVE_RETURN_NULL();
    }
    
    jint* ids_data = (jint*)array_data(ids);
    jint record_id = ids_data[current];
    
    /* Advance index */
    enum_obj->fields[1].i = current + 1;
    
    /* Get the record store handle from field 2 */
    jint store_handle = enum_obj->fields[2].i;
    
    if (store_handle < 0 || store_handle >= record_store_count) {
        jvm_throw_by_name(jvm, "javax/microedition/rms/InvalidRecordIDException", NULL);
        return NATIVE_RETURN_NULL();
    }
    
    NativeRecordStore* store = &record_stores[store_handle];
    
    /* Find the record */
    for (int i = 0; i < MAX_RECORDS_PER_STORE; i++) {
        if (store->records[i].valid && store->records[i].id == record_id) {
            Record* rec = &store->records[i];
            
            /* Create byte array */
            JavaArray* data = jvm_new_array(jvm, T_BYTE, rec->size, NULL);
            if (!data) {
                jvm_throw_by_name(jvm, "java/lang/OutOfMemoryError", NULL);
                return NATIVE_RETURN_NULL();
            }
            
            memcpy(array_data(data), rec->data, rec->size);
            
            RMS_DEBUG("nextRecord: returning %d bytes for record %d", rec->size, record_id);
            return NATIVE_RETURN_OBJECT(data);
        }
    }
    
    jvm_throw_by_name(jvm, "javax/microedition/rms/InvalidRecordIDException", NULL);
    return NATIVE_RETURN_NULL();
}

/* RecordEnumeration.destroy() - cleanup enumeration */
static JavaValue native_enumeration_destroy(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* enum_obj = (JavaObject*)args[0].ref;
    
    if (enum_obj) {
        /* Clear the ids array reference */
        enum_obj->fields[0].ref = NULL;
        enum_obj->fields[1].i = 0;
    }
    
    return NATIVE_RETURN_VOID();
}

/* RecordEnumeration.numRecords() */
static JavaValue native_enumeration_numRecords(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* enum_obj = (JavaObject*)args[0].ref;
    
    if (!enum_obj) {
        return NATIVE_RETURN_INT(0);
    }
    
    JavaArray* ids = (JavaArray*)enum_obj->fields[0].ref;
    return NATIVE_RETURN_INT(ids ? ids->length : 0);
}

/* RecordEnumeration.hasPreviousElement() */
static JavaValue native_enumeration_hasPreviousElement(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* enum_obj = (JavaObject*)args[0].ref;
    
    if (!enum_obj) {
        return NATIVE_RETURN_INT(0);
    }
    
    jint current = enum_obj->fields[1].i;
    return NATIVE_RETURN_INT(current > 0 ? 1 : 0);
}

/* RecordEnumeration.previousRecordId() */
static JavaValue native_enumeration_previousRecordId(JVM* jvm, JavaThread* thread,
                                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* enum_obj = (JavaObject*)args[0].ref;
    
    if (!enum_obj) {
        jvm_throw_by_name(jvm, "javax/microedition/rms/InvalidRecordIDException", NULL);
        return NATIVE_RETURN_INT(-1);
    }
    
    JavaArray* ids = (JavaArray*)enum_obj->fields[0].ref;
    jint current = enum_obj->fields[1].i;
    
    if (!ids || current <= 0) {
        /* FreeJ2ME wraps around: if index<0, set to count-1 */
        if (ids && ids->length > 0) {
            enum_obj->fields[1].i = ids->length - 1;
            jint* ids_data = (jint*)array_data(ids);
            return NATIVE_RETURN_INT(ids_data[ids->length - 1]);
        }
        jvm_throw_by_name(jvm, "javax/microedition/rms/InvalidRecordIDException", NULL);
        return NATIVE_RETURN_INT(-1);
    }
    
    jint* ids_data = (jint*)array_data(ids);
    /* Decrement index first (previousRecordId returns current then moves back) */
    current--;
    enum_obj->fields[1].i = current;
    return NATIVE_RETURN_INT(ids_data[current]);
}

/* RecordEnumeration.previousRecord() - returns byte[] of previous record */
static JavaValue native_enumeration_previousRecord(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* enum_obj = (JavaObject*)args[0].ref;
    
    if (!enum_obj) {
        jvm_throw_by_name(jvm, "javax/microedition/rms/InvalidRecordIDException", NULL);
        return NATIVE_RETURN_NULL();
    }
    
    JavaArray* ids = (JavaArray*)enum_obj->fields[0].ref;
    jint current = enum_obj->fields[1].i;
    
    if (!ids) {
        jvm_throw_by_name(jvm, "javax/microedition/rms/InvalidRecordIDException", NULL);
        return NATIVE_RETURN_NULL();
    }
    
    /* Decrement index */
    current--;
    if (current < 0) {
        /* FreeJ2ME wraps around */
        current = ids->length - 1;
    }
    enum_obj->fields[1].i = current;
    
    jint* ids_data = (jint*)array_data(ids);
    jint record_id = ids_data[current];
    
    /* Get the record store handle from field 2 */
    jint store_handle = enum_obj->fields[2].i;
    
    if (store_handle < 0 || store_handle >= record_store_count) {
        jvm_throw_by_name(jvm, "javax/microedition/rms/InvalidRecordIDException", NULL);
        return NATIVE_RETURN_NULL();
    }
    
    NativeRecordStore* store = &record_stores[store_handle];
    
    for (int i = 0; i < MAX_RECORDS_PER_STORE; i++) {
        if (store->records[i].valid && store->records[i].id == record_id) {
            Record* rec = &store->records[i];
            JavaArray* data = jvm_new_array(jvm, T_BYTE, rec->size, NULL);
            if (!data) {
                jvm_throw_by_name(jvm, "java/lang/OutOfMemoryError", NULL);
                return NATIVE_RETURN_NULL();
            }
            memcpy(array_data(data), rec->data, rec->size);
            return NATIVE_RETURN_OBJECT(data);
        }
    }
    
    jvm_throw_by_name(jvm, "javax/microedition/rms/InvalidRecordIDException", NULL);
    return NATIVE_RETURN_NULL();
}

/* RecordEnumeration.reset() - reset index to beginning */
static JavaValue native_enumeration_reset(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* enum_obj = (JavaObject*)args[0].ref;
    
    if (enum_obj) {
        enum_obj->fields[1].i = 0;
    }
    
    return NATIVE_RETURN_VOID();
}

/* RecordEnumeration.rebuild() - rebuild enumeration (no-op for now) */
static JavaValue native_enumeration_rebuild(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count; (void)args;
    /* Currently no-op; would need filter/comparator support for full impl */
    return NATIVE_RETURN_VOID();
}

/* RecordEnumeration.isKeptUpdated() - returns false */
static JavaValue native_enumeration_isKeptUpdated(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count; (void)args;
    return NATIVE_RETURN_INT(0);
}

/* RecordEnumeration.keepUpdated(boolean) - no-op */
static JavaValue native_enumeration_keepUpdated(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count; (void)args;
    return NATIVE_RETURN_VOID();
}

/* RecordStore.setMode(int, boolean) - no-op, matches FreeJ2ME */
static JavaValue native_rms_setMode(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count; (void)args;
    /* FreeJ2ME: empty implementation */
    return NATIVE_RETURN_VOID();
}

/* RecordStore.addRecordListener - no-op stub */
static JavaValue native_rms_addRecordListener(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count; (void)args;
    /* Listener support not needed for most games */
    return NATIVE_RETURN_VOID();
}

/* RecordStore.removeRecordListener - no-op stub */
static JavaValue native_rms_removeRecordListener(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count; (void)args;
    return NATIVE_RETURN_VOID();
}

/* openRecordStore(String, boolean, int, boolean) - authmode overload */
static JavaValue native_rms_openRecordStore_authmode(JVM* jvm, JavaThread* thread,
                                                      JavaValue* args, int arg_count) {
    (void)arg_count;
    /* Static: args[0]=name, args[1]=create, args[2]=authmode, args[3]=writable */
    JavaString* name_str = (JavaString*)args[0].ref;
    jboolean create = args[1].i;
    /* authmode and writable are ignored, same as FreeJ2ME */
    
    if (!name_str) {
        RMS_DEBUG("openRecordStore(authmode): name is NULL");
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    const char* name = string_utf8(jvm, name_str);
    RMS_DEBUG("openRecordStore(authmode): '%s', create=%d", name, create);
    
    int handle = rms_open(name, create != 0);
    
    if (handle < 0) {
        if (handle == -1) {
            jvm_throw_by_name(jvm, "javax/microedition/rms/RecordStoreNotFoundException", name);
        } else {
            jvm_throw_by_name(jvm, "javax/microedition/rms/RecordStoreFullException", NULL);
        }
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_NULL();
    }
    
    JavaClass* rs_class = jvm_load_class(jvm, "javax/microedition/rms/RecordStore");
    if (!rs_class) return NATIVE_RETURN_NULL();
    
    JavaObject* rs_obj = jvm_new_object(jvm, rs_class);
    if (!rs_obj) return NATIVE_RETURN_NULL();
    
    for (int i = 0; i < rs_class->fields_count; i++) {
        if (rs_class->fields[i].name && strcmp(rs_class->fields[i].name, "nativeHandle") == 0) {
            rs_obj->fields[i].i = handle;
            break;
        }
    }
    
    return NATIVE_RETURN_OBJECT(rs_obj);
}

/* openRecordStore(String, String, String) - vendor/suite overload */
static JavaValue native_rms_openRecordStore_vendor(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)arg_count;
    /* Static: args[0]=name, args[1]=vendorName, args[2]=suiteName */
    JavaString* name_str = (JavaString*)args[0].ref;
    
    if (!name_str) {
        RMS_DEBUG("openRecordStore(vendor): name is NULL");
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    const char* name = string_utf8(jvm, name_str);
    RMS_DEBUG("openRecordStore(vendor): '%s'", name);
    
    /* FreeJ2ME: calls new RecordStore(name, false) - don't create */
    int handle = rms_open(name, false);
    
    if (handle < 0) {
        jvm_throw_by_name(jvm, "javax/microedition/rms/RecordStoreNotFoundException", name);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_NULL();
    }
    
    JavaClass* rs_class = jvm_load_class(jvm, "javax/microedition/rms/RecordStore");
    if (!rs_class) return NATIVE_RETURN_NULL();
    
    JavaObject* rs_obj = jvm_new_object(jvm, rs_class);
    if (!rs_obj) return NATIVE_RETURN_NULL();
    
    for (int i = 0; i < rs_class->fields_count; i++) {
        if (rs_class->fields[i].name && strcmp(rs_class->fields[i].name, "nativeHandle") == 0) {
            rs_obj->fields[i].i = handle;
            break;
        }
    }
    
    return NATIVE_RETURN_OBJECT(rs_obj);
}

void init_javax_microedition_rms(JVM* jvm) {
    NativeMethodEntry methods[] = {
        /* Static methods */
        {"javax/microedition/rms/RecordStore", "openRecordStore", 
         "(Ljava/lang/String;Z)Ljavax/microedition/rms/RecordStore;", native_rms_openRecordStore},
        {"javax/microedition/rms/RecordStore", "openRecordStore", 
         "(Ljava/lang/String;ZIZ)Ljavax/microedition/rms/RecordStore;", native_rms_openRecordStore_authmode},
        {"javax/microedition/rms/RecordStore", "openRecordStore", 
         "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljavax/microedition/rms/RecordStore;", native_rms_openRecordStore_vendor},
        {"javax/microedition/rms/RecordStore", "deleteRecordStore", 
         "(Ljava/lang/String;)V", native_rms_deleteRecordStore},
        {"javax/microedition/rms/RecordStore", "listRecordStores", 
         "()[Ljava/lang/String;", native_rms_listRecordStores},
        
        /* Instance methods */
        {"javax/microedition/rms/RecordStore", "closeRecordStore", 
         "()V", native_rms_closeRecordStore},
        {"javax/microedition/rms/RecordStore", "addRecord", 
         "([BII)I", native_rms_addRecord},
        {"javax/microedition/rms/RecordStore", "getRecord", 
         "(I)[B", native_rms_getRecord},
        {"javax/microedition/rms/RecordStore", "getRecord", 
         "(I[BI)I", native_rms_getRecord_array},
        {"javax/microedition/rms/RecordStore", "getRecordSize", 
         "(I)I", native_rms_getRecordSize},
        {"javax/microedition/rms/RecordStore", "setRecord", 
         "(I[BII)V", native_rms_setRecord},
        {"javax/microedition/rms/RecordStore", "deleteRecord", 
         "(I)V", native_rms_deleteRecord},
        {"javax/microedition/rms/RecordStore", "getNumRecords", 
         "()I", native_rms_getNumRecords},
        {"javax/microedition/rms/RecordStore", "enumerateRecords", 
         "(Ljavax/microedition/rms/RecordFilter;Ljavax/microedition/rms/RecordComparator;Z)Ljavax/microedition/rms/RecordEnumeration;",
         native_rms_enumerateRecords},
        {"javax/microedition/rms/RecordStore", "getNextRecordID", 
         "()I", native_rms_getNextRecordID},
        {"javax/microedition/rms/RecordStore", "getVersion", 
         "()I", native_rms_getVersion},
        {"javax/microedition/rms/RecordStore", "getLastModified", 
         "()J", native_rms_getLastModified},
        {"javax/microedition/rms/RecordStore", "getSize", 
         "()I", native_rms_getSize},
        {"javax/microedition/rms/RecordStore", "getSizeAvailable", 
         "()I", native_rms_getSizeAvailable},
        {"javax/microedition/rms/RecordStore", "getName", 
         "()Ljava/lang/String;", native_rms_getName},
        {"javax/microedition/rms/RecordStore", "setMode", 
         "(IZ)V", native_rms_setMode},
        {"javax/microedition/rms/RecordStore", "addRecordListener", 
         "(Ljavax/microedition/rms/RecordListener;)V", native_rms_addRecordListener},
        {"javax/microedition/rms/RecordStore", "removeRecordListener", 
         "(Ljavax/microedition/rms/RecordListener;)V", native_rms_removeRecordListener},
        
        /* RecordEnumeration interface methods */
        {"javax/microedition/rms/RecordEnumeration", "hasNextElement", 
         "()Z", native_enumeration_hasNextElement},
        {"javax/microedition/rms/RecordEnumeration", "hasPreviousElement", 
         "()Z", native_enumeration_hasPreviousElement},
        {"javax/microedition/rms/RecordEnumeration", "nextRecordId", 
         "()I", native_enumeration_nextRecordId},
        {"javax/microedition/rms/RecordEnumeration", "nextRecord", 
         "()[B", native_enumeration_nextRecord},
        {"javax/microedition/rms/RecordEnumeration", "previousRecordId", 
         "()I", native_enumeration_previousRecordId},
        {"javax/microedition/rms/RecordEnumeration", "previousRecord", 
         "()[B", native_enumeration_previousRecord},
        {"javax/microedition/rms/RecordEnumeration", "destroy", 
         "()V", native_enumeration_destroy},
        {"javax/microedition/rms/RecordEnumeration", "numRecords", 
         "()I", native_enumeration_numRecords},
        {"javax/microedition/rms/RecordEnumeration", "reset", 
         "()V", native_enumeration_reset},
        {"javax/microedition/rms/RecordEnumeration", "rebuild", 
         "()V", native_enumeration_rebuild},
        {"javax/microedition/rms/RecordEnumeration", "isKeptUpdated", 
         "()Z", native_enumeration_isKeptUpdated},
        {"javax/microedition/rms/RecordEnumeration", "keepUpdated", 
         "(Z)V", native_enumeration_keepUpdated},
        
        /* RecordEnumerationImpl concrete class methods (same implementations) */
        {"javax/microedition/rms/RecordEnumerationImpl", "hasNextElement", 
         "()Z", native_enumeration_hasNextElement},
        {"javax/microedition/rms/RecordEnumerationImpl", "hasPreviousElement", 
         "()Z", native_enumeration_hasPreviousElement},
        {"javax/microedition/rms/RecordEnumerationImpl", "nextRecordId", 
         "()I", native_enumeration_nextRecordId},
        {"javax/microedition/rms/RecordEnumerationImpl", "nextRecord", 
         "()[B", native_enumeration_nextRecord},
        {"javax/microedition/rms/RecordEnumerationImpl", "previousRecordId", 
         "()I", native_enumeration_previousRecordId},
        {"javax/microedition/rms/RecordEnumerationImpl", "previousRecord", 
         "()[B", native_enumeration_previousRecord},
        {"javax/microedition/rms/RecordEnumerationImpl", "destroy", 
         "()V", native_enumeration_destroy},
        {"javax/microedition/rms/RecordEnumerationImpl", "numRecords", 
         "()I", native_enumeration_numRecords},
        {"javax/microedition/rms/RecordEnumerationImpl", "reset", 
         "()V", native_enumeration_reset},
        {"javax/microedition/rms/RecordEnumerationImpl", "rebuild", 
         "()V", native_enumeration_rebuild},
        {"javax/microedition/rms/RecordEnumerationImpl", "isKeptUpdated", 
         "()Z", native_enumeration_isKeptUpdated},
        {"javax/microedition/rms/RecordEnumerationImpl", "keepUpdated", 
         "(Z)V", native_enumeration_keepUpdated},
    };
    
    int count = sizeof(methods) / sizeof(methods[0]);
    native_register_methods(jvm, methods, count);
    
    RMS_DEBUG("Registered %d native methods", count);
}