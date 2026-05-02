/*
 * J2ME Emulator - Native Methods
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L  /* For strdup, clock_gettime, pthread - only on POSIX */
#define _DEFAULT_SOURCE   /* For usleep */
#endif

#include <stdio.h>
#include "debug.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <stdint.h>  /* For uint32_t, uint8_t */

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

#include "native.h"
#include "jvm.h"
#include "heap.h"
#include "threads.h"
#include "opcodes.h"
#include "classfile.h"
#include "sdl_backend.h"
#include "midp.h"  /* For load_jar_resource */

/* String field indices for JavaObject-based strings - must match heap.c */
#define STRING_FIELD_VALUE  0   /* char[] array */
#define STRING_FIELD_OFF   1   /* int offset */
#define STRING_FIELD_COUNT  2   /* int length */
#define STRING_FIELD_HASH   3   /* int hash */

/* Forward declarations */
void init_java_util_random(JVM* jvm);
void init_java_util_timer(JVM* jvm);
void init_java_lang_stringbuffer(JVM* jvm);
void init_java_lang_stringbuilder(JVM* jvm);
void init_java_util_vector(JVM* jvm);
void init_java_util_enumeration(JVM* jvm);
void init_java_util_hashtable(JVM* jvm);
void init_java_io_bytearrayinputstream(JVM* jvm);
void init_java_io_bytearrayoutputstream(JVM* jvm);
void init_java_io_printstream(JVM* jvm);
void init_java_io_dataoutputstream(JVM* jvm);
void init_java_io_datainputstream(JVM* jvm);
void init_java_io_inputstreamreader(JVM* jvm);
void init_java_io_bufferedreader(JVM* jvm);
void init_nokia_sound(JVM* jvm);
void init_nokia_ui(JVM* jvm);
void init_nokia_direct_graphics(JVM* jvm);
void init_nokia_m3d_impl(JVM* jvm);
void init_nokia_misc(JVM* jvm);
void init_javax_microedition_midlet_MIDlet(JVM* jvm);
void init_javax_microedition_rms(JVM* jvm);
void init_javax_microedition_lcdui_graphics(JVM* jvm);
void init_javax_microedition_lcdui_image(JVM* jvm);
void init_javax_microedition_lcdui_display(JVM* jvm);
void init_javax_microedition_lcdui_form(JVM* jvm);
void init_javax_microedition_media(JVM* jvm);
void init_java_lang_integer(JVM* jvm);
void init_java_lang_long(JVM* jvm);
void init_java_lang_boolean(JVM* jvm);
void init_javax_microedition_io(JVM* jvm);
void init_java_util_calendar(JVM* jvm);
void init_java_util_date(JVM* jvm);
void init_java_util_timezone(JVM* jvm);
void init_java_lang_character(JVM* jvm);
void init_java_lang_byte_short(JVM* jvm);
void init_java_lang_float_double(JVM* jvm);
void init_javax_microedition_media_manager(JVM* jvm);
void init_vendor_extensions(JVM* jvm);
void init_javax_microedition_m3g(JVM* jvm);  /* JSR 184 Mobile 3D Graphics */

/* Native method registry */
#define MAX_NATIVE_METHODS 1024

static struct {
    NativeMethodEntry entries[MAX_NATIVE_METHODS];
    int count;
} native_registry;

/* Native method hash table for O(1) lookup */
#define NATIVE_HASH_SIZE 1024  /* Must be power of 2 */
#define NATIVE_HASH_MASK (NATIVE_HASH_SIZE - 1)

typedef struct NativeHashEntry {
    const char* class_name;
    const char* method_name;
    const char* descriptor;
    NativeMethod handler;
    struct NativeHashEntry* next;  /* For chaining */
} NativeHashEntry;

static NativeHashEntry* native_hash_table[NATIVE_HASH_SIZE];
static int native_hash_initialized = 0;

static uint32_t native_method_hash(const char* class_name, const char* method_name, const char* descriptor) {
    uint32_t h = 2166136261u;
    while (*class_name) { h ^= (uint8_t)*class_name++; h *= 16777619u; }
    h ^= (uint8_t)'/';
    while (*method_name) { h ^= (uint8_t)*method_name++; h *= 16777619u; }
    h ^= (uint8_t)'(';
    while (*descriptor) { h ^= (uint8_t)*descriptor++; h *= 16777619u; }
    return h;
}

static void native_hash_build(void) {
    memset(native_hash_table, 0, sizeof(native_hash_table));
    for (int i = 0; i < native_registry.count; i++) {
        NativeMethodEntry* e = &native_registry.entries[i];
        uint32_t idx = native_method_hash(e->class_name, e->method_name, e->descriptor) & NATIVE_HASH_MASK;
        NativeHashEntry* he = (NativeHashEntry*)malloc(sizeof(NativeHashEntry));
        he->class_name = e->class_name;
        he->method_name = e->method_name;
        he->descriptor = e->descriptor;
        he->handler = e->handler;
        he->next = native_hash_table[idx];
        native_hash_table[idx] = he;
    }
    native_hash_initialized = 1;
}

/* Forward declaration for execute_method (used by Class.newInstance) */
extern int execute_method(JVM* jvm, JavaThread* thread, JavaMethod* method, 
                          JavaValue* args, JavaValue* result);

/* Initialize native methods */
int native_init(JVM* jvm) {
    if (g_j2me_runtime_debug) {
        fprintf(stderr, "### NATIVE_INIT CALLED ###\n");
        fflush(stderr);
    }
    
    /* Initialize DRM bypass system (like KEmulator) */
    extern void drm_bypass_init(void);
    drm_bypass_init();
    
    native_registry.count = 0;
    
    /* Initialize standard Java native methods */
    init_java_lang_object(jvm);
    init_java_lang_class(jvm);
    init_java_lang_string(jvm);
    init_java_lang_system(jvm);
    init_java_lang_runtime(jvm);
    init_java_lang_math(jvm);
    init_java_lang_thread(jvm);
    init_java_lang_throwable(jvm);
    
    /* Initialize MIDlet lifecycle */
    init_javax_microedition_midlet_MIDlet(jvm);
    
    /* Initialize StringBuffer and StringBuilder */
    init_java_lang_stringbuffer(jvm);
    init_java_lang_stringbuilder(jvm);
    
    /* Initialize java.util collection classes */
    init_java_util_random(jvm);
    init_java_util_vector(jvm);
    init_java_util_enumeration(jvm);
    init_java_util_hashtable(jvm);
    
    /* Initialize java.util.Timer */
    init_java_util_timer(jvm);
    
    /* Initialize ByteArrayInputStream */
    init_java_io_bytearrayinputstream(jvm);
    
    /* Initialize ByteArrayOutputStream */
    init_java_io_bytearrayoutputstream(jvm);
    
    /* Initialize PrintStream (System.out/err) */
    init_java_io_printstream(jvm);
    
    /* Initialize DataOutputStream */
    init_java_io_dataoutputstream(jvm);
    
    /* Initialize DataInputStream */
    init_java_io_datainputstream(jvm);
    
    /* Initialize InputStreamReader and BufferedReader */
    init_java_io_inputstreamreader(jvm);
    init_java_io_bufferedreader(jvm);
    
    /* Initialize Nokia Sound */
    init_nokia_sound(jvm);
    
    /* Initialize Nokia UI API */
    init_nokia_ui(jvm);
    init_nokia_direct_graphics(jvm);
    
    /* Initialize Nokia M3D software 3D renderer */
    init_nokia_m3d_impl(jvm);

    /* Initialize Nokia misc APIs */
    init_nokia_misc(jvm);
    
    /* Initialize RMS (RecordStore) */
    init_javax_microedition_rms(jvm);
    
    /* Initialize LCDUI */
    init_javax_microedition_lcdui_graphics(jvm);
    init_javax_microedition_lcdui_image(jvm);
    init_javax_microedition_lcdui_display(jvm);
    init_javax_microedition_lcdui_form(jvm);
    
    /* Initialize Font (Sprite/GameCanvas/LayerManager/TiledLayer) */
    init_javax_microedition_lcdui_font(jvm);
    init_javax_microedition_lcdui_game_gamecanvas(jvm);
    
    /* Initialize Media */
    init_javax_microedition_media(jvm);
    
    /* Initialize wrapper classes */
    init_java_lang_integer(jvm);
    init_java_lang_long(jvm);
    init_java_lang_boolean(jvm);
    init_java_lang_character(jvm);
    init_java_lang_byte_short(jvm);
    init_java_lang_float_double(jvm);
    
    /* Initialize J2ME IO (GCF) */
    init_javax_microedition_io(jvm);
    
    /* Initialize Calendar, TimeZone and Date */
    init_java_util_timezone(jvm);
    init_java_util_calendar(jvm);
    init_java_util_date(jvm);
    
    /* Initialize Media Manager */
    init_javax_microedition_media_manager(jvm);
    
    /* Initialize vendor-specific extensions */
    init_vendor_extensions(jvm);
    
    /* Initialize JSR 184 Mobile 3D Graphics API */
    init_javax_microedition_m3g(jvm);
    
    /* Build hash table for O(1) native method lookup */
    native_hash_build();
    
    return JNI_OK;
}

/* Register a native method */
int native_register(JVM* jvm, const char* class_name, const char* method_name,
                    const char* descriptor, NativeMethod handler) {
    (void)jvm;
    
    if (native_registry.count >= MAX_NATIVE_METHODS) {
        return JNI_ERR;
    }
    
    NativeMethodEntry* entry = &native_registry.entries[native_registry.count++];
    entry->class_name = class_name;
    entry->method_name = method_name;
    entry->descriptor = descriptor;
    entry->handler = handler;
    
    return JNI_OK;
}

/* Register multiple native methods */
int native_register_methods(JVM* jvm, const NativeMethodEntry* methods, int count) {
    for (int i = 0; i < count; i++) {
        if (native_register(jvm, methods[i].class_name, methods[i].method_name,
                           methods[i].descriptor, methods[i].handler) != JNI_OK) {
            return JNI_ERR;
        }
    }
    return JNI_OK;
}

/* Find native method - ИСПРАВЛЕНО: ищем в родительских классах, КРОМЕ конструкторов */
NativeMethod native_find(JVM* jvm, const char* class_name, const char* method_name,
                         const char* descriptor) {
    /* Fast path: hash table lookup for exact match */
    if (native_hash_initialized) {
        uint32_t idx = native_method_hash(class_name, method_name, descriptor) & NATIVE_HASH_MASK;
        for (NativeHashEntry* he = native_hash_table[idx]; he; he = he->next) {
            if (strcmp(he->class_name, class_name) == 0 &&
                strcmp(he->method_name, method_name) == 0 &&
                strcmp(he->descriptor, descriptor) == 0) {
                return he->handler;
            }
        }
    } else {
        /* Fallback: linear scan (before hash table is built) */
        for (int i = 0; i < native_registry.count; i++) {
            NativeMethodEntry* entry = &native_registry.entries[i];
            if (strcmp(entry->class_name, class_name) == 0 &&
                strcmp(entry->method_name, method_name) == 0 &&
                strcmp(entry->descriptor, descriptor) == 0) {
                return entry->handler;
            }
        }
    }
    
    /* ВАЖНО: Для конструкторов (<init>) и статических инициализаторов (<clinit>) 
     * НЕ ищем в родительских классах!
     * Каждый класс имеет свой конструктор/инициализатор, который должен выполняться.
     * Поиск в суперклассах нужен только для обычных методов (например, notifyDestroyed).
     */
    if (method_name && (strcmp(method_name, "<init>") == 0 || strcmp(method_name, "<clinit>") == 0)) {
        return NULL;
    }
    
    /* ИСПРАВЛЕНО: Если не нашли, ищем в родительских классах */
    /* Ищем класс в загруженных классах JVM */
    if (jvm) {
        JavaClass* clazz = NULL;
        
        /* Search in loaded classes */
        for (int i = 0; i < jvm->class_loader.count; i++) {
            JavaClass* c = jvm->class_loader.classes[i];
            if (c && c->class_name && strcmp(c->class_name, class_name) == 0) {
                clazz = c;
                break;
            }
        }
        
        /* Walk up the inheritance chain */
        while (clazz && clazz->super_class) {
            clazz = clazz->super_class;
            if (clazz->class_name) {
                if (native_hash_initialized) {
                    uint32_t idx = native_method_hash(clazz->class_name, method_name, descriptor) & NATIVE_HASH_MASK;
                    for (NativeHashEntry* he = native_hash_table[idx]; he; he = he->next) {
                        if (strcmp(he->class_name, clazz->class_name) == 0 &&
                            strcmp(he->method_name, method_name) == 0 &&
                            strcmp(he->descriptor, descriptor) == 0) {
                            return he->handler;
                        }
                    }
                } else {
                    for (int i = 0; i < native_registry.count; i++) {
                        NativeMethodEntry* entry = &native_registry.entries[i];
                        if (strcmp(entry->class_name, clazz->class_name) == 0 &&
                            strcmp(entry->method_name, method_name) == 0 &&
                            strcmp(entry->descriptor, descriptor) == 0) {
                            return entry->handler;
                        }
                    }
                }
            }
        }
    }

    return NULL;
}

/* Utility functions */
const char* native_get_string_utf8(JVM* jvm, JavaValue* args, int index) {
    JavaString* str = (JavaString*)args[index].ref;
    if (!str) return NULL;
    return string_utf8(jvm, str);
}

/* Exception helpers */
void native_throw_npe(JVM* jvm, JavaThread* thread) {
    static int npe_log_count = 0;
    if (npe_log_count < 5) {
        JavaFrame* frame = thread && thread->current_frame ? thread->current_frame : NULL;
        if (frame && frame->method && frame->clazz) {
            int pc = (int)frame->pc;
            fprintf(stderr, "[NPE] %s.%s at PC=%d\n",
                    frame->clazz->class_name ? frame->clazz->class_name : "?",
                    frame->method->name ? frame->method->name : "?", pc);
        }
        npe_log_count++;
    }
    jvm_throw_by_name(jvm, "java/lang/NullPointerException", NULL);
    thread->pending_exception = jvm_exception_pending(jvm);
}

void native_throw_aioobe(JVM* jvm, JavaThread* thread, jint index) {
    char msg[64];
    snprintf(msg, sizeof(msg), "Array index out of range: %d", index);
    jvm_throw_by_name(jvm, "java/lang/ArrayIndexOutOfBoundsException", msg);
    thread->pending_exception = jvm_exception_pending(jvm);
}

void native_throw_cnfe(JVM* jvm, JavaThread* thread, const char* name) {
    jvm_throw_by_name(jvm, "java/lang/ClassNotFoundException", name);
    thread->pending_exception = jvm_exception_pending(jvm);
}

void native_throw_oome(JVM* jvm, JavaThread* thread) {
    jvm_throw_by_name(jvm, "java/lang/OutOfMemoryError", NULL);
    thread->pending_exception = jvm_exception_pending(jvm);
}

void native_throw_iae(JVM* jvm, JavaThread* thread, const char* message) {
    jvm_throw_by_name(jvm, "java/lang/IllegalArgumentException", message);
    thread->pending_exception = jvm_exception_pending(jvm);
}

/* Throw NegativeArraySizeException */
void native_throw_negative_array_size(JVM* jvm, JavaThread* thread) {
    jvm_throw_by_name(jvm, "java/lang/NegativeArraySizeException", NULL);
    thread->pending_exception = jvm_exception_pending(jvm);
}

/* Throw ArrayStoreException */
void native_throw_array_store_exception(JVM* jvm, JavaThread* thread) {
    jvm_throw_by_name(jvm, "java/lang/ArrayStoreException", NULL);
    thread->pending_exception = jvm_exception_pending(jvm);
}

/* Throw IOException */
void native_throw_ioe(JVM* jvm, JavaThread* thread, const char* message) {
    jvm_throw_by_name(jvm, "java/io/IOException", message);
    thread->pending_exception = jvm_exception_pending(jvm);
}

/* Throw InterruptedException */
void native_throw_interrupted(JVM* jvm, JavaThread* thread, const char* message) {
    jvm_throw_by_name(jvm, "java/lang/InterruptedException", message);
    thread->pending_exception = jvm_exception_pending(jvm);
}

/* === КРИТИЧЕСКОЕ ИСПРАВЛЕНИЕ: Правильный расчет слотов полей ===
 * 
 * Проблема: native-код использовал индекс поля в clazz->fields[] напрямую
 * как слот в obj->fields[], но это неправильно потому что:
 * 1. Статические поля не хранятся в экземпляре
 * 2. Поля суперкласса должны быть учтены (они идут первыми)
 * 3. Long/double занимают 2 слота
 * 
 * Эта функция вычисляет правильный слот для поля по его имени.
 * Возвращает -1 если поле не найдено.
 */

/* Helper: count instance fields in a class (excluding static) */
static int native_count_instance_fields(JavaClass* clazz) {
    int count = 0;
    if (clazz->fields) {
        for (int i = 0; i < clazz->fields_count; i++) {
            if (!(clazz->fields[i].access_flags & ACC_STATIC)) {
                count++;
                /* Long and double take 2 slots */
                if (clazz->fields[i].descriptor &&
                    (clazz->fields[i].descriptor[0] == 'J' || clazz->fields[i].descriptor[0] == 'D')) {
                    count++;
                }
            }
        }
    }
    return count;
}

/* Helper: count instance fields before a given field index */
static int native_count_instance_fields_before(JavaClass* clazz, int field_index) {
    int count = 0;
    if (clazz->fields) {
        for (int i = 0; i < field_index; i++) {
            if (!(clazz->fields[i].access_flags & ACC_STATIC)) {
                count++;
                /* Long and double take 2 slots */
                if (clazz->fields[i].descriptor &&
                    (clazz->fields[i].descriptor[0] == 'J' || clazz->fields[i].descriptor[0] == 'D')) {
                    count++;
                }
            }
        }
    }
    return count;
}

/* Build class hierarchy array from Object to obj_class */
static int native_build_hierarchy(JavaClass* obj_class, JavaClass** hierarchy, int max_depth) {
    int depth = 0;
    JavaClass* c = obj_class;
    
    while (c && depth < max_depth) {
        hierarchy[depth++] = c;
        c = c->super_class;
    }
    
    /* Reverse to get Object first */
    for (int i = 0; i < depth / 2; i++) {
        JavaClass* tmp = hierarchy[i];
        hierarchy[i] = hierarchy[depth - 1 - i];
        hierarchy[depth - 1 - i] = tmp;
    }
    
    return depth;
}

/* Find field slot in object instance - CORRECT VERSION */
typedef struct {
    JavaClass* defining_class;
    int field_index;
    int slot;
} NativeFieldLookupResult;

static NativeFieldLookupResult native_find_field_slot(JavaClass* obj_class, 
                                                       const char* field_name,
                                                       const char* descriptor) {
    NativeFieldLookupResult result = { NULL, -1, -1 };
    
    if (!obj_class || !field_name) return result;
    
    /* Build hierarchy from Object to obj_class */
    JavaClass* hierarchy[64];
    int depth = native_build_hierarchy(obj_class, hierarchy, 64);
    
    /* Search for field in hierarchy (from Object to obj_class) */
    for (int h = 0; h < depth; h++) {
        JavaClass* current_class = hierarchy[h];
        
        if (current_class->fields) {
            for (int i = 0; i < current_class->fields_count; i++) {
                JavaField* field = &current_class->fields[i];
                
                /* Skip static fields */
                if (field->access_flags & ACC_STATIC) continue;
                
                /* Match by name */
                if (!field->name || strcmp(field->name, field_name) != 0) continue;
                
                /* Match by descriptor if provided */
                if (descriptor && field->descriptor) {
                    if (strcmp(field->descriptor, descriptor) != 0) continue;
                }
                
                /* Found the field! */
                result.defining_class = current_class;
                result.field_index = i;
                
                /* Calculate slot */
                result.slot = 0;
                
                /* Add fields from all classes above current_class */
                for (int j = 0; j < h; j++) {
                    result.slot += native_count_instance_fields(hierarchy[j]);
                }
                
                /* Add fields before this one in current_class */
                result.slot += native_count_instance_fields_before(current_class, i);
                
                return result;
            }
        }
    }
    
    return result;
}

/* Convenience function to get field slot by name only */
int native_get_field_slot(JavaClass* clazz, const char* field_name) {
    NativeFieldLookupResult result = native_find_field_slot(clazz, field_name, NULL);
    if (result.slot < 0) {
        NATIVE_DEBUG("native_get_field_slot: field '%s' not found in class %s",
                field_name, clazz && clazz->class_name ? clazz->class_name : "?");
    }
    return result.slot;
}

/* Convenience function to get field value from object by name */
JavaValue native_get_field_value(JavaObject* obj, const char* field_name) {
    JavaValue v = { .raw = 0 };
    if (!obj || !obj->header.clazz || !field_name) return v;
    
    int slot = native_get_field_slot(obj->header.clazz, field_name);
    if (slot < 0) return v;
    
    int max_slots = (obj->header.clazz->instance_size - sizeof(ObjectHeader)) / sizeof(JavaValue);
    if (slot >= max_slots) {
        NATIVE_DEBUG("ERROR: field '%s' slot %d out of bounds (max %d)",
                field_name, slot, max_slots);
        return v;
    }
    
    return obj->fields[slot];
}

/* Convenience function to set field value in object by name */
void native_set_field_value(JavaObject* obj, const char* field_name, JavaValue value) {
    if (!obj || !obj->header.clazz || !field_name) return;
    
    int slot = native_get_field_slot(obj->header.clazz, field_name);
    if (slot < 0) {
        NATIVE_DEBUG("WARNING: field '%s' not found in %s",
                field_name, obj->header.clazz->class_name ? obj->header.clazz->class_name : "?");
        return;
    }
    
    int max_slots = (obj->header.clazz->instance_size - sizeof(ObjectHeader)) / sizeof(JavaValue);
    if (slot >= max_slots) {
        NATIVE_DEBUG("ERROR: field '%s' slot %d out of bounds (max %d)",
                field_name, slot, max_slots);
        return;
    }
    
    obj->fields[slot] = value;
}

/*
 * java.lang.Object native methods
 */

static JavaValue native_object_getClass(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = NATIVE_ARG_OBJECT(args, 0);
    return NATIVE_RETURN_OBJECT(obj ? obj->header.clazz : NULL);
}

static JavaValue native_object_hashCode(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = NATIVE_ARG_OBJECT(args, 0);
    return NATIVE_RETURN_INT(obj ? obj->header.hashcode : 0);
}

static JavaValue native_object_clone(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = NATIVE_ARG_OBJECT(args, 0);
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    JavaClass* clazz = obj->header.clazz;
    if (!clazz) {
        return NATIVE_RETURN_NULL();
    }
    
    /* Check if class implements Cloneable - use resolved interface_classes */
    bool implements_cloneable = false;
    
    /* Check interface_classes array (resolved interfaces) */
    if (clazz->interface_classes) {
        for (int i = 0; i < clazz->interfaces_count; i++) {
            JavaClass* iface = clazz->interface_classes[i];
            if (iface && iface->class_name && 
                strcmp(iface->class_name, "java/lang/Cloneable") == 0) {
                implements_cloneable = true;
                break;
            }
        }
    }
    
    /* Also check superclasses for Cloneable */
    JavaClass* super_check = clazz->super_class;
    while (!implements_cloneable && super_check) {
        if (super_check->interface_classes) {
            for (int i = 0; i < super_check->interfaces_count; i++) {
                JavaClass* iface = super_check->interface_classes[i];
                if (iface && iface->class_name &&
                    strcmp(iface->class_name, "java/lang/Cloneable") == 0) {
                    implements_cloneable = true;
                    break;
                }
            }
        }
        super_check = super_check->super_class;
    }
    
    if (!implements_cloneable) {
        jvm_throw_by_name(jvm, "java/lang/CloneNotSupportedException", 
                          "Class does not implement Cloneable");
        return NATIVE_RETURN_NULL();
    }
    
    /* Check if object is an array */
    if (object_is_array(obj)) {
        JavaArray* arr = (JavaArray*)obj;
        jsize len = arr->length;
        uint8_t elem_type = arr->element_type;
        JavaClass* elem_class = arr->element_class;
        
        /* Allocate new array */
        JavaArray* new_arr = heap_alloc_array(jvm, elem_type, len, elem_class);
        if (!new_arr) {
            jvm_throw_by_name(jvm, "java/lang/OutOfMemoryError", "Failed to clone array");
            return NATIVE_RETURN_NULL();
        }
        
        /* Copy array data */
        size_t elem_size = 1;
        switch (elem_type) {
            case DESC_BYTE:
            case DESC_BOOLEAN: elem_size = 1; break;
            case DESC_CHAR:
            case DESC_SHORT: elem_size = 2; break;
            case DESC_INT:
            case DESC_FLOAT: elem_size = 4; break;
            case DESC_LONG:
            case DESC_DOUBLE: elem_size = 8; break;
            case DESC_OBJECT:
            case DESC_ARRAY: elem_size = sizeof(void*); break;
        }
        
        memcpy(array_data(new_arr), array_data(arr), len * elem_size);
        
        return NATIVE_RETURN_OBJECT(new_arr);
    }
    
    /* Regular object clone */
    JavaObject* new_obj = heap_alloc_object(jvm, clazz);
    if (!new_obj) {
        jvm_throw_by_name(jvm, "java/lang/OutOfMemoryError", "Failed to clone object");
        return NATIVE_RETURN_NULL();
    }
    
    /* Copy all instance fields */
    int num_fields = OBJECT_NUM_FIELDS(obj);
    for (int i = 0; i < num_fields; i++) {
        new_obj->fields[i] = obj->fields[i];
    }
    
    /* Generate new hashcode for cloned object */
    new_obj->header.hashcode = (jint)(uintptr_t)new_obj ^ 0x12345678;
    
    return NATIVE_RETURN_OBJECT(new_obj);
}

static JavaValue native_object_notify(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* obj = NATIVE_ARG_OBJECT(args, 0);
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    int result = monitor_notify(jvm, obj);
    if (result == JNI_ERR) {
        /* Thread doesn't own the monitor - throw IllegalMonitorStateException */
        jvm_throw_by_name(jvm, "java/lang/IllegalMonitorStateException", NULL);
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue native_object_notifyAll(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* obj = NATIVE_ARG_OBJECT(args, 0);
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    int result = monitor_notify_all(jvm, obj);
    if (result == JNI_ERR) {
        /* Thread doesn't own the monitor - throw IllegalMonitorStateException */
        jvm_throw_by_name(jvm, "java/lang/IllegalMonitorStateException", NULL);
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue native_object_wait(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    for (int i = 0; i < arg_count && i < 5; i++) {
    }
    
    JavaObject* obj = NATIVE_ARG_OBJECT(args, 0);
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    jlong timeout = 0;
    bool timed = false;
    
    /* For wait(J)V: arg_count=3 (this + 2 long slots) on most platforms
     * For wait()V: arg_count=1 (just this)
     * The long value is in args[1] (first slot) when timed wait
     * 
     * CRITICAL: Long values occupy 2 stack slots, but when passed to native
     * methods, we receive them as a single jlong in args[1]
     */
    if (arg_count >= 2) {
        /* Timed wait - long is in args[1] as a jlong */
        timeout = args[1].j;
        timed = true;
    }
    
    int result = monitor_wait(jvm, obj, timeout, timed);
    
    /* Check if interrupted - need to throw InterruptedException */
    if (result == JNI_ERR) {
        if (thread && thread->interrupted) {
            /* Clear the interrupted flag */
            thread->interrupted = false;
            
            /* Create and throw InterruptedException */
            extern JavaObject* jvm_new_object(JVM* jvm, JavaClass* clazz);
            extern JavaClass* jvm_load_class(JVM* jvm, const char* name);
            JavaClass* exc_class = jvm_load_class(jvm, "java/lang/InterruptedException");
            if (exc_class) {
                JavaObject* exc = jvm_new_object(jvm, exc_class);
                if (exc) {
                    thread->pending_exception = exc;
                }
            }
        } else {
            /* Thread doesn't own the monitor - throw IllegalMonitorStateException */
            jvm_throw_by_name(jvm, "java/lang/IllegalMonitorStateException", NULL);
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Object.<init>() - base constructor, does nothing */
static JavaValue native_object_init(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* Object constructor does nothing - just return */
    return NATIVE_RETURN_VOID();
}

/* Object.equals(Object) - reference equality comparison */
static JavaValue native_object_equals(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = NATIVE_ARG_OBJECT(args, 0);
    JavaObject* other_obj = NATIVE_ARG_OBJECT(args, 1);
    
    /* Default Object.equals() returns true only if same reference */
    return NATIVE_RETURN_INT(this_obj == other_obj ? 1 : 0);
}

/* Object.toString() - returns getClass().getName() + "@" + Integer.toHexString(hashCode()) */
static JavaValue native_object_toString(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = NATIVE_ARG_OBJECT(args, 0);
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Get class name */
    JavaClass* clazz = obj->header.clazz;
    if (!clazz || !clazz->class_name) {
        return NATIVE_RETURN_NULL();
    }
    
    /* Convert to Java format (dots instead of slashes) */
    char* name = class_name_to_java(clazz->class_name);
    
    /* Create result: className@hexHash */
    char result[256];
    snprintf(result, sizeof(result), "%s@%x", name, obj->header.hashcode);
    free(name);
    
    JavaString* str = jvm_new_string(jvm, result);
    return NATIVE_RETURN_OBJECT(str);
}

/* Throwable.toString() - returns getClass().getName() + ": " + getMessage() if message != null */
static JavaValue native_throwable_toString(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = NATIVE_ARG_OBJECT(args, 0);
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Get class name */
    JavaClass* clazz = obj->header.clazz;
    if (!clazz || !clazz->class_name) {
        return NATIVE_RETURN_NULL();
    }
    
    /* Convert to Java format */
    char* name = class_name_to_java(clazz->class_name);
    
    /* Get detailMessage field */
    JavaString* msg = (JavaString*)native_get_field_value(obj, "detailMessage").ref;
    
    char* result;
    if (msg) {
        const char* msg_utf8 = string_utf8(jvm, msg);
        size_t len = strlen(name) + 2 + strlen(msg_utf8 ? msg_utf8 : "") + 1;
        result = (char*)malloc(len);
        snprintf(result, len, "%s: %s", name, msg_utf8 ? msg_utf8 : "");
    } else {
        result = strdup(name);
    }
    free(name);
    
    JavaString* str = jvm_new_string(jvm, result);
    free(result);
    
    return NATIVE_RETURN_OBJECT(str);
}

void init_java_lang_object(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/lang/Object", "<init>", "()V", native_object_init},
        {"java/lang/Object", "getClass", "()Ljava/lang/Class;", native_object_getClass},
        {"java/lang/Object", "hashCode", "()I", native_object_hashCode},
        {"java/lang/Object", "clone", "()Ljava/lang/Object;", native_object_clone},
        {"java/lang/Object", "notify", "()V", native_object_notify},
        {"java/lang/Object", "notifyAll", "()V", native_object_notifyAll},
        {"java/lang/Object", "wait", "()V", native_object_wait},      /* No-arg wait */
        {"java/lang/Object", "wait", "(J)V", native_object_wait},    /* Timed wait */
        {"java/lang/Object", "toString", "()Ljava/lang/String;", native_object_toString},
        {"java/lang/Object", "equals", "(Ljava/lang/Object;)Z", native_object_equals},
        /* Throwable.toString() - all exceptions inherit from Throwable */
        {"java/lang/Throwable", "toString", "()Ljava/lang/String;", native_throwable_toString},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.lang.Class native methods
 */

static JavaValue native_class_forName(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)arg_count;
    /* STATIC METHOD - no 'this' argument!
     * args[0] = class name (String)
     */
    const char* name = native_get_string_utf8(jvm, args, 0);
    if (!name) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Convert dots to slashes */
    char* internal_name = strdup(name);
    for (char* p = internal_name; *p; p++) {
        if (*p == '.') *p = '/';
    }
    
    JavaClass* clazz = jvm_load_class(jvm, internal_name);
    free(internal_name);
    
    if (!clazz) {
        native_throw_cnfe(jvm, thread, name);
        return NATIVE_RETURN_NULL();
    }
    
    return NATIVE_RETURN_OBJECT(clazz);
}

static JavaValue native_class_getName(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaClass* clazz = (JavaClass*)args[0].ref;
    if (!clazz || !clazz->class_name) return NATIVE_RETURN_NULL();
    
    char* name = class_name_to_java(clazz->class_name);
    JavaString* str = jvm_new_string(jvm, name);
    free(name);
    
    return NATIVE_RETURN_OBJECT(str);
}

static JavaValue native_class_isInstance(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaClass* clazz = (JavaClass*)args[0].ref;
    JavaObject* obj = (JavaObject*)args[1].ref;
    
    return NATIVE_RETURN_INT(object_instance_of(obj, clazz) ? 1 : 0);
}

/* Class.getResourceAsStream(String) - load resource from JAR */
static JavaValue native_class_getResourceAsStream(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    
    /* args[0] is the Class object (which is a JavaClass* in our implementation) */
    JavaClass* clazz = (JavaClass*)args[0].ref;
    JavaString* name_str = (JavaString*)args[1].ref;
    
    /* ALWAYS LOG: Track resource loading for game debugging */
    if (g_j2me_runtime_debug) fprintf(stderr, "[RESOURCE] getResourceAsStream called (clazz=%s)\n",
            (clazz && clazz->class_name) ? clazz->class_name : "?");
    fflush(stderr);
    
    if (!name_str) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[RESOURCE] FAILED: name_str is NULL\n");
        fflush(stderr);
        return NATIVE_RETURN_NULL();
    }
    
    /* Get the resource name */
    const char* name = string_utf8(jvm, name_str);
    if (!name) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[RESOURCE] FAILED: string_utf8 returned NULL\n");
        fflush(stderr);
        return NATIVE_RETURN_NULL();
    }
    
    if (g_j2me_runtime_debug) fprintf(stderr, "[RESOURCE] Looking for: '%s'\n", name);
    fflush(stderr);
    
    /* Skip leading slash if present */
    const char* resource_name = name;
    if (resource_name[0] == '/') {
        resource_name++;
    }
    
    /* Load the resource from JAR */
    size_t data_size;
    uint8_t* data = load_jar_resource(resource_name, &data_size);
    
    if (!data) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[RESOURCE] NOT FOUND: '%s'\n", resource_name);
        fflush(stderr);
        return NATIVE_RETURN_NULL();
    }
    
    if (g_j2me_runtime_debug) fprintf(stderr, "[RESOURCE] LOADED: '%s' (%zu bytes)\n", resource_name, data_size);
    fflush(stderr);
    
    /* Create a byte array to hold the data */
    JavaArray* byte_array = jvm_new_array(jvm, T_BYTE, (jsize)data_size, NULL);
    if (!byte_array) {
        free(data);
        return NATIVE_RETURN_NULL();
    }
    
    /* Copy data into byte array */
    memcpy(array_data(byte_array), data, data_size);
    free(data);
    
    /* Create ByteArrayInputStream object */
    JavaClass* bais_class = jvm_load_class(jvm, "java/io/ByteArrayInputStream");
    if (!bais_class) {
        CLASS_DEBUG("Failed to load ByteArrayInputStream class");
        return NATIVE_RETURN_NULL();
    }
    
    JavaObject* bais_obj = jvm_new_object(jvm, bais_class);
    if (!bais_obj) {
        CLASS_DEBUG("Failed to create ByteArrayInputStream object");
        return NATIVE_RETURN_NULL();
    }
    
    /* Set fields: buf = byte_array, pos = 0, count = data_size
     * ИСПРАВЛЕНО: Используем native_set_field_value вместо прямого доступа по индексу */
    JavaValue buf_val = { .ref = byte_array };
    JavaValue pos_val = { .i = 0 };
    JavaValue count_val = { .i = (jint)data_size };
    
    native_set_field_value(bais_obj, "buf", buf_val);
    native_set_field_value(bais_obj, "pos", pos_val);
    native_set_field_value(bais_obj, "count", count_val);
    
    CLASS_DEBUG("Created ByteArrayInputStream: buf=%p, pos=0, count=%zu",
            (void*)byte_array, data_size);
    
    return NATIVE_RETURN_OBJECT(bais_obj);
}

/* ByteArrayInputStream.<init>(byte[]) - constructor with byte array */
static JavaValue native_bytearrayinputstream_init_bytes(JVM* jvm, JavaThread* thread,
                                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* buf = (JavaArray*)args[1].ref;
    
    if (g_j2me_runtime_debug) fprintf(stderr, "[BAIS_INIT] obj=%p, buf=%p, buf_len=%d\n", 
            (void*)obj, (void*)buf, buf ? (int)buf->length : -1);
    
    if (!obj) {
        return NATIVE_RETURN_VOID();
    }
    
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue buf_val = { .ref = buf };
    JavaValue pos_val = { .i = 0 };
    JavaValue count_val = { .i = buf ? (jint)buf->length : 0 };
    
    native_set_field_value(obj, "buf", buf_val);
    native_set_field_value(obj, "pos", pos_val);
    native_set_field_value(obj, "count", count_val);
    
    /* Verify fields were set correctly */
    JavaValue v_buf = native_get_field_value(obj, "buf");
    JavaValue v_pos = native_get_field_value(obj, "pos");
    JavaValue v_count = native_get_field_value(obj, "count");
    if (g_j2me_runtime_debug) fprintf(stderr, "[BAIS_INIT] Verify: buf=%p, pos=%d, count=%d\n", 
            (void*)v_buf.ref, v_pos.i, v_count.i);
    
    return NATIVE_RETURN_VOID();
}

/* ByteArrayInputStream.<init>(byte[], int, int) - constructor with offset and length */
static JavaValue native_bytearrayinputstream_init_bytes_offset(JVM* jvm, JavaThread* thread,
                                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* buf = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint length = args[3].i;
    
    if (!obj) {
        return NATIVE_RETURN_VOID();
    }
    
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue buf_val = { .ref = buf };
    JavaValue pos_val = { .i = offset };
    JavaValue count_val = { .i = offset + length };
    
    native_set_field_value(obj, "buf", buf_val);
    native_set_field_value(obj, "pos", pos_val);
    native_set_field_value(obj, "count", count_val);
    
    return NATIVE_RETURN_VOID();
}

/* ByteArrayInputStream.read() - read single byte */
static JavaValue native_bytearrayinputstream_read(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        return NATIVE_RETURN_INT(-1);
    }
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    JavaArray* buf = (JavaArray*)native_get_field_value(obj, "buf").ref;
    jint pos = native_get_field_value(obj, "pos").i;
    jint count = native_get_field_value(obj, "count").i;
    
    if (!buf || pos >= count) {
        return NATIVE_RETURN_INT(-1);  /* EOF */
    }
    
    /* Read byte from buffer */
    uint8_t* data = (uint8_t*)array_data(buf);
    jbyte val = (jbyte)data[pos];
    
    /* Update position */
    JavaValue new_pos = { .i = pos + 1 };
    native_set_field_value(obj, "pos", new_pos);
    
    return NATIVE_RETURN_INT(val & 0xFF);  /* Return as unsigned */
}

/* ByteArrayInputStream.read(byte[], int, int) - read into buffer */
static JavaValue native_bytearrayinputstream_read_array(JVM* jvm, JavaThread* thread,
                                                         JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* dest = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint len = args[3].i;
    
    if (!obj) {
        return NATIVE_RETURN_INT(-1);
    }
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    JavaArray* buf = (JavaArray*)native_get_field_value(obj, "buf").ref;
    jint pos = native_get_field_value(obj, "pos").i;
    jint count = native_get_field_value(obj, "count").i;
    
    if (!buf || !dest) {
        return NATIVE_RETURN_INT(-1);
    }
    
    if (pos >= count) {
        return NATIVE_RETURN_INT(-1);  /* EOF */
    }
    
    /* Calculate bytes to read */
    jint available = count - pos;
    jint to_read = (len < available) ? len : available;
    
    /* Check bounds */
    if (offset < 0 || len < 0 || offset + len > (jint)dest->length) {
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Copy data */
    uint8_t* src_data = (uint8_t*)array_data(buf);
    uint8_t* dest_data = (uint8_t*)array_data(dest);
    memcpy(dest_data + offset, src_data + pos, to_read);
    
    /* Update position */
    JavaValue new_pos = { .i = pos + to_read };
    native_set_field_value(obj, "pos", new_pos);
    
    return NATIVE_RETURN_INT(to_read);
}

/* ByteArrayInputStream.read(byte[]) - read into entire buffer */
static JavaValue native_bytearrayinputstream_read_array_full(JVM* jvm, JavaThread* thread,
                                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* dest = (JavaArray*)args[1].ref;
    
    if (!obj || !dest) {
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Call read(byte[], 0, length) */
    JavaValue new_args[4];
    new_args[0].ref = args[0].ref;  /* this */
    new_args[1].ref = args[1].ref;  /* buffer */
    new_args[2].i = 0;               /* offset */
    new_args[3].i = dest->length;    /* length */
    
    return native_bytearrayinputstream_read_array(jvm, thread, new_args, 4);
}

/* ByteArrayInputStream.available() - bytes available to read */
static JavaValue native_bytearrayinputstream_available(JVM* jvm, JavaThread* thread,
                                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    jint pos = native_get_field_value(obj, "pos").i;
    jint count = native_get_field_value(obj, "count").i;
    
    jint available = count - pos;
    return NATIVE_RETURN_INT(available > 0 ? available : 0);
}

/* Class.newInstance() - create a new instance of the class */
static JavaValue native_class_newInstance(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    
    /* args[0] is the Class object (JavaClass*) */
    JavaClass* clazz = (JavaClass*)args[0].ref;
    
    if (!clazz) {
        CLASS_DEBUG("newInstance: clazz is NULL");
        return NATIVE_RETURN_NULL();
    }
    
    CLASS_DEBUG("newInstance: creating instance of %s", clazz->class_name);
    
    /* Check if class is abstract or interface */
    if (clazz->access_flags & (ACC_ABSTRACT | ACC_INTERFACE)) {
        CLASS_DEBUG("newInstance: cannot instantiate abstract class or interface %s", clazz->class_name);
        return NATIVE_RETURN_NULL();
    }
    
    /* Create new instance */
    JavaObject* obj = jvm_new_object(jvm, clazz);
    if (!obj) {
        CLASS_DEBUG("newInstance: failed to create object for %s", clazz->class_name);
        return NATIVE_RETURN_NULL();
    }
    
    /* Call default constructor if it exists */
    JavaMethod* constructor = jvm_resolve_method(jvm, clazz, "<init>", "()V");
    if (constructor) {
        CLASS_DEBUG("newInstance: calling default constructor for %s", clazz->class_name);
        JavaValue ctor_args[1];
        ctor_args[0].ref = obj;
        JavaValue result;
        execute_method(jvm, thread, constructor, ctor_args, &result);
    } else {
        CLASS_DEBUG("newInstance: no default constructor for %s (using default init)", clazz->class_name);
    }
    
    return NATIVE_RETURN_OBJECT(obj);
}

/* Class.isArray() - check if class is an array class */
static JavaValue native_class_isArray(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    
    JavaClass* clazz = (JavaClass*)args[0].ref;
    if (!clazz || !clazz->class_name) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Array class names start with '[' */
    return NATIVE_RETURN_INT(clazz->class_name[0] == '[' ? 1 : 0);
}

/* Class.isAssignableFrom(Class) - check if this class is assignable from another */
static JavaValue native_class_isAssignableFrom(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    
    JavaClass* this_class = (JavaClass*)args[0].ref;
    JavaClass* other_class = (JavaClass*)args[1].ref;
    
    if (!this_class || !other_class) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Simple check: if classes are the same */
    if (this_class == other_class) {
        return NATIVE_RETURN_INT(1);
    }
    
    /* Check if other_class extends this_class */
    JavaClass* current = other_class;
    while (current) {
        if (current == this_class) {
            return NATIVE_RETURN_INT(1);
        }
        current = current->super_class;
    }
    
    return NATIVE_RETURN_INT(0);
}

/* Class.getSuperclass() - get the superclass */
static JavaValue native_class_getSuperclass(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    
    JavaClass* clazz = (JavaClass*)args[0].ref;
    if (!clazz) {
        return NATIVE_RETURN_NULL();
    }
    
    /* Return superclass (for Object, this will be NULL) */
    return NATIVE_RETURN_OBJECT(clazz->super_class);
}

/* Class.getComponentType() - get component type for array classes */
static JavaValue native_class_getComponentType(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    
    JavaClass* clazz = (JavaClass*)args[0].ref;
    if (!clazz || !clazz->class_name || clazz->class_name[0] != '[') {
        return NATIVE_RETURN_NULL();
    }
    
    /* Get component type name (skip the '[' prefix) */
    const char* component_name = clazz->class_name + 1;
    
    /* Handle primitive array types */
    if (component_name[0] != 'L') {
        /* Primitive type - return special class */
        JavaClass* component_class = NULL;
        switch (component_name[0]) {
            case 'Z': component_class = jvm_load_class(jvm, "java/lang/Boolean"); break;
            case 'B': component_class = jvm_load_class(jvm, "java/lang/Byte"); break;
            case 'C': component_class = jvm_load_class(jvm, "java/lang/Character"); break;
            case 'S': component_class = jvm_load_class(jvm, "java/lang/Short"); break;
            case 'I': component_class = jvm_load_class(jvm, "java/lang/Integer"); break;
            case 'J': component_class = jvm_load_class(jvm, "java/lang/Long"); break;
            case 'F': component_class = jvm_load_class(jvm, "java/lang/Float"); break;
            case 'D': component_class = jvm_load_class(jvm, "java/lang/Double"); break;
        }
        return NATIVE_RETURN_OBJECT(component_class);
    }
    
    /* Object array - component name is like "Ljava/lang/Object;" */
    /* Skip 'L' and ';' */
    char* name_copy = strdup(component_name + 1);
    size_t len = strlen(name_copy);
    if (len > 0 && name_copy[len - 1] == ';') {
        name_copy[len - 1] = '\0';
    }
    
    JavaClass* component_class = jvm_load_class(jvm, name_copy);
    free(name_copy);
    
    return NATIVE_RETURN_OBJECT(component_class);
}

void init_java_lang_class(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/lang/Class", "forName", "(Ljava/lang/String;)Ljava/lang/Class;", native_class_forName},
        {"java/lang/Class", "getName", "()Ljava/lang/String;", native_class_getName},
        {"java/lang/Class", "isInstance", "(Ljava/lang/Object;)Z", native_class_isInstance},
        {"java/lang/Class", "getResourceAsStream", "(Ljava/lang/String;)Ljava/io/InputStream;", native_class_getResourceAsStream},
        {"java/lang/Class", "newInstance", "()Ljava/lang/Object;", native_class_newInstance},
        {"java/lang/Class", "isArray", "()Z", native_class_isArray},
        {"java/lang/Class", "isAssignableFrom", "(Ljava/lang/Class;)Z", native_class_isAssignableFrom},
        {"java/lang/Class", "getSuperclass", "()Ljava/lang/Class;", native_class_getSuperclass},
        {"java/lang/Class", "getComponentType", "()Ljava/lang/Class;", native_class_getComponentType},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/* ByteArrayInputStream.skip(long) - skips n bytes */
static JavaValue native_bytearrayinputstream_skip(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jlong n = args[1].j;
    
    if (!obj || n <= 0) {
        return NATIVE_RETURN_LONG(0);
    }
    
    /* Get current position and count */
    jint pos = native_get_field_value(obj, "pos").i;
    jint count = native_get_field_value(obj, "count").i;
    
    /* Calculate how many bytes we can actually skip */
    jlong available = count - pos;
    jlong to_skip = (n < available) ? n : available;
    
    if (to_skip < 0) to_skip = 0;
    
    /* Update position */
    JavaValue new_pos = { .i = pos + (jint)to_skip };
    native_set_field_value(obj, "pos", new_pos);
    
    NATIVE_DEBUG("ByteArrayInputStream.skip(%ld): skipped %ld bytes, pos=%d->%d, count=%d",
            (long)n, (long)to_skip, pos, new_pos.i, count);
    
    return NATIVE_RETURN_LONG(to_skip);
}

/* ByteArrayInputStream native methods */
void init_java_io_bytearrayinputstream(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/io/ByteArrayInputStream", "<init>", "([B)V", native_bytearrayinputstream_init_bytes},
        {"java/io/ByteArrayInputStream", "<init>", "([BII)V", native_bytearrayinputstream_init_bytes_offset},
        {"java/io/ByteArrayInputStream", "read", "()I", native_bytearrayinputstream_read},
        {"java/io/ByteArrayInputStream", "read", "([B)I", native_bytearrayinputstream_read_array_full},
        {"java/io/ByteArrayInputStream", "read", "([BII)I", native_bytearrayinputstream_read_array},
        {"java/io/ByteArrayInputStream", "available", "()I", native_bytearrayinputstream_available},
        {"java/io/ByteArrayInputStream", "skip", "(J)J", native_bytearrayinputstream_skip},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.io.ByteArrayOutputStream native methods
 */

/* ByteArrayOutputStream buffer is stored in a native peer pointer */

static JavaValue native_bytearrayoutputstream_init(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* Allocate a dynamic buffer - start with 32 bytes */
    int capacity = 32;
    
    /* Create a Java byte array to hold our data */
    JavaArray* buf_array = jvm_new_array(jvm, T_BYTE, capacity, NULL);
    if (buf_array) {
        /* ИСПРАВЛЕНО: Используем native_set_field_value */
        JavaValue buf_val = { .ref = buf_array };
        JavaValue count_val = { .i = 0 };
        native_set_field_value(obj, "buf", buf_val);
        native_set_field_value(obj, "count", count_val);
    }
    
    return NATIVE_RETURN_VOID();
}

/* ByteArrayOutputStream.<init>(int size) - constructor with initial buffer size */
static JavaValue native_bytearrayoutputstream_init_size(JVM* jvm, JavaThread* thread,
                                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* Use specified initial size, minimum 32 */
    int capacity = args[1].i;
    if (capacity < 32) capacity = 32;
    
    /* Create a Java byte array to hold our data */
    JavaArray* buf_array = jvm_new_array(jvm, T_BYTE, capacity, NULL);
    if (buf_array) {
        JavaValue buf_val = { .ref = buf_array };
        JavaValue count_val = { .i = 0 };
        native_set_field_value(obj, "buf", buf_val);
        native_set_field_value(obj, "count", count_val);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_bytearrayoutputstream_tobytearray(JVM* jvm, JavaThread* thread,
                                                          JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) return NATIVE_RETURN_NULL();
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    JavaArray* buf_array = (JavaArray*)native_get_field_value(obj, "buf").ref;
    jint count = native_get_field_value(obj, "count").i;
    
    if (g_j2me_runtime_debug) fprintf(stderr, "[BAOS_TOARRAY] obj=%p, buf=%p, count=%d, buf_len=%d\n",
            (void*)obj, (void*)buf_array, count, buf_array ? (int)buf_array->length : -1);
    
    /* Print first bytes for debug */
    if (buf_array && count > 0 && g_j2me_runtime_debug) {
        uint8_t* data = (uint8_t*)array_data(buf_array);
        fprintf(stderr, "[BAOS_TOARRAY] first bytes: ");
        for (int i = 0; i < count && i < 20; i++) {
            fprintf(stderr, "%02x ", data[i]);
        }
        fprintf(stderr, "\n");
    }
    
    if (!buf_array || count <= 0) {
        /* Return empty array */
        JavaArray* empty = jvm_new_array(jvm, T_BYTE, 0, NULL);
        return NATIVE_RETURN_OBJECT(empty);
    }
    
    /* Create result array with actual size */
    JavaArray* result = jvm_new_array(jvm, T_BYTE, count, NULL);
    if (result && buf_array) {
        void* src_data = array_data(buf_array);
        void* dst_data = array_data(result);
        if (src_data && dst_data) {
            memcpy(dst_data, src_data, count);
        }
    }
    
    return NATIVE_RETURN_OBJECT(result);
}

static JavaValue native_bytearrayoutputstream_write(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint b = args[1].i;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: Используем native_get/set_field_value */
    JavaArray* buf_array = (JavaArray*)native_get_field_value(obj, "buf").ref;
    jint count = native_get_field_value(obj, "count").i;
    
    /* Need to expand buffer? */
    if (!buf_array || count >= (jint)buf_array->length) {
        int new_capacity = buf_array ? buf_array->length * 2 : 32;
        if (new_capacity < count + 1) new_capacity = count + 32;
        
        JavaArray* new_array = jvm_new_array(jvm, T_BYTE, new_capacity, NULL);
        if (new_array) {
            void* src = array_data(buf_array);
            void* dst = array_data(new_array);
            if (buf_array && src && dst && count > 0) {
                memcpy(dst, src, count);
            }
            buf_array = new_array;
            JavaValue buf_val = { .ref = buf_array };
            native_set_field_value(obj, "buf", buf_val);
        }
    }
    
    /* Write byte */
    if (buf_array) {
        uint8_t* data = (uint8_t*)array_data(buf_array);
        if (data) {
            data[count] = (uint8_t)b;
            count++;
        
            /* Update count field */
            JavaValue count_val = { .i = count };
            native_set_field_value(obj, "count", count_val);
        }
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_bytearrayoutputstream_write_array(JVM* jvm, JavaThread* thread,
                                                          JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* data = (JavaArray*)args[1].ref;
    jint off = args[2].i;
    jint len = args[3].i;
    
    if (!obj || !data) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: Используем native_get/set_field_value */
    JavaArray* buf_array = (JavaArray*)native_get_field_value(obj, "buf").ref;
    jint count = native_get_field_value(obj, "count").i;
    
    /* Need to expand buffer? */
    int needed = count + len;
    if (!buf_array || needed > (jint)buf_array->length) {
        int new_capacity = buf_array ? buf_array->length * 2 : 32;
        while (new_capacity < needed) new_capacity *= 2;
        
        JavaArray* new_array = jvm_new_array(jvm, T_BYTE, new_capacity, NULL);
        if (new_array) {
            void* src = array_data(buf_array);
            void* dst = array_data(new_array);
            if (buf_array && src && dst && count > 0) {
                memcpy(dst, src, count);
            }
            buf_array = new_array;
            JavaValue buf_val = { .ref = buf_array };
            native_set_field_value(obj, "buf", buf_val);
        }
    }
    
    /* Write bytes */
    if (buf_array && data) {
        uint8_t* dst = (uint8_t*)array_data(buf_array);
        uint8_t* src = (uint8_t*)array_data(data);
        if (dst && src) {
            memcpy(dst + count, src + off, len);
            count += len;
        
            JavaValue count_val = { .i = count };
            native_set_field_value(obj, "count", count_val);
        }
    }
    
    return NATIVE_RETURN_VOID();
}

void init_java_io_bytearrayoutputstream(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/io/ByteArrayOutputStream", "<init>", "()V", native_bytearrayoutputstream_init},
        {"java/io/ByteArrayOutputStream", "<init>", "(I)V", native_bytearrayoutputstream_init_size},
        {"java/io/ByteArrayOutputStream", "toByteArray", "()[B", native_bytearrayoutputstream_tobytearray},
        {"java/io/ByteArrayOutputStream", "write", "(I)V", native_bytearrayoutputstream_write},
        {"java/io/ByteArrayOutputStream", "write", "([BII)V", native_bytearrayoutputstream_write_array},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.io.DataOutputStream native methods
 * DataOutputStream wraps an OutputStream and writes data to it
 */

/* Forward declaration */
static void dos_write_byte(JVM* jvm, JavaObject* dos_obj, uint8_t b);
static void dos_write_bytes(JVM* jvm, JavaObject* dos_obj, uint8_t* data, int len);

static JavaValue native_dataoutputstream_init(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* out = (JavaObject*)args[1].ref;
    
    if (g_j2me_runtime_debug) fprintf(stderr, "[DOS_INIT] obj=%p, out=%p\n", (void*)obj, (void*)out);
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* Debug: print class info */
    if (obj->header.clazz) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[DOS_INIT] class=%s, fields_count=%d, instance_size=%d\n",
                obj->header.clazz->class_name ? obj->header.clazz->class_name : "?",
                obj->header.clazz->fields_count,
                (int)obj->header.clazz->instance_size);
        
        /* Print all fields */
        for (int i = 0; i < obj->header.clazz->fields_count; i++) {
            JavaField* f = &obj->header.clazz->fields[i];
            if (g_j2me_runtime_debug) fprintf(stderr, "[DOS_INIT] field[%d]: name='%s', descriptor='%s'\n",
                    i, f->name ? f->name : "?", f->descriptor ? f->descriptor : "?");
        }
    }
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue out_val = { .ref = out };
    native_set_field_value(obj, "out", out_val);
    
    /* Verify */
    JavaValue verify = native_get_field_value(obj, "out");
    if (g_j2me_runtime_debug) fprintf(stderr, "[DOS_INIT] Set 'out' field, verify: ref=%p (expected %p)\n",
            (void*)verify.ref, (void*)out);
    
    return NATIVE_RETURN_VOID();
}

/* Write a single byte to the underlying output stream */
static void dos_write_byte(JVM* jvm, JavaObject* dos_obj, uint8_t b) {
    if (!dos_obj) return;
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    JavaObject* out = (JavaObject*)native_get_field_value(dos_obj, "out").ref;
    
    static int debug_count = 0;
    if (debug_count < 20) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[DOS_WRITE_BYTE] dos=%p, out=%p, byte=%02x\n", 
                (void*)dos_obj, (void*)out, b);
        debug_count++;
    }
    
    if (!out) return;
    
    /* Check if out is a ByteArrayOutputStream */
    JavaClass* out_class = out->header.clazz;
    if (out_class && out_class->class_name && 
        strcmp(out_class->class_name, "java/io/ByteArrayOutputStream") == 0) {
        
        /* ИСПРАВЛЕНО: Используем native_get/set_field_value */
        JavaArray* buf_array = (JavaArray*)native_get_field_value(out, "buf").ref;
        jint count = native_get_field_value(out, "count").i;
        
        /* Expand buffer if needed */
        if (!buf_array || count >= (jint)buf_array->length) {
            int new_capacity = buf_array ? buf_array->length * 2 : 32;
            JavaArray* new_array = jvm_new_array(jvm, T_BYTE, new_capacity, NULL);
            if (new_array) {
                void* src = array_data(buf_array);
                void* dst = array_data(new_array);
                if (buf_array && src && dst && count > 0) {
                    memcpy(dst, src, count);
                }
                buf_array = new_array;
                JavaValue buf_val = { .ref = buf_array };
                native_set_field_value(out, "buf", buf_val);
            }
        }
        
        /* Write byte */
        if (buf_array) {
            uint8_t* data = (uint8_t*)array_data(buf_array);
            if (data) {
                data[count] = b;
                count++;
                JavaValue count_val = { .i = count };
                native_set_field_value(out, "count", count_val);
            }
        }
    }
}

static void dos_write_bytes(JVM* jvm, JavaObject* dos_obj, uint8_t* data, int len) {
    for (int i = 0; i < len; i++) {
        dos_write_byte(jvm, dos_obj, data[i]);
    }
}

static JavaValue native_dataoutputstream_write(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint b = args[1].i;
    
    dos_write_byte(jvm, obj, (uint8_t)b);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_dataoutputstream_writeint(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint v = args[1].i;
    
    uint8_t bytes[4];
    bytes[0] = (v >> 24) & 0xFF;
    bytes[1] = (v >> 16) & 0xFF;
    bytes[2] = (v >> 8) & 0xFF;
    bytes[3] = v & 0xFF;
    
    dos_write_bytes(jvm, obj, bytes, 4);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_dataoutputstream_writelong(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jlong v = args[1].j;
    
    uint8_t bytes[8];
    bytes[0] = (v >> 56) & 0xFF;
    bytes[1] = (v >> 48) & 0xFF;
    bytes[2] = (v >> 40) & 0xFF;
    bytes[3] = (v >> 32) & 0xFF;
    bytes[4] = (v >> 24) & 0xFF;
    bytes[5] = (v >> 16) & 0xFF;
    bytes[6] = (v >> 8) & 0xFF;
    bytes[7] = v & 0xFF;
    
    dos_write_bytes(jvm, obj, bytes, 8);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_dataoutputstream_writebyte(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint v = args[1].i;
    
    dos_write_byte(jvm, obj, (uint8_t)v);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_dataoutputstream_writeshort(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint v = args[1].i;
    
    uint8_t bytes[2];
    bytes[0] = (v >> 8) & 0xFF;
    bytes[1] = v & 0xFF;
    
    dos_write_bytes(jvm, obj, bytes, 2);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_dataoutputstream_writeboolean(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jboolean v = args[1].i;
    
    dos_write_byte(jvm, obj, v ? 1 : 0);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_dataoutputstream_writeutf(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaString* str = (JavaString*)args[1].ref;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    if (!str) {
        /* Write 0 length for null string */
        dos_write_byte(jvm, obj, 0);
        dos_write_byte(jvm, obj, 0);
        return NATIVE_RETURN_VOID();
    }
    
    /* Get UTF-8 representation */
    const char* utf8 = string_utf8(jvm, str);
    if (!utf8) {
        dos_write_byte(jvm, obj, 0);
        dos_write_byte(jvm, obj, 0);
        return NATIVE_RETURN_VOID();
    }
    
    size_t len = strlen(utf8);
    
    /* Write length as 2 bytes (big-endian) */
    dos_write_byte(jvm, obj, (len >> 8) & 0xFF);
    dos_write_byte(jvm, obj, len & 0xFF);
    
    /* Write UTF-8 bytes */
    for (size_t i = 0; i < len; i++) {
        dos_write_byte(jvm, obj, (uint8_t)utf8[i]);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_dataoutputstream_writefloat(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jfloat v = args[1].f;
    
    /* Convert float to int bits, then write as 4 bytes */
    jint bits = *(jint*)&v;
    
    uint8_t bytes[4];
    bytes[0] = (bits >> 24) & 0xFF;
    bytes[1] = (bits >> 16) & 0xFF;
    bytes[2] = (bits >> 8) & 0xFF;
    bytes[3] = bits & 0xFF;
    
    dos_write_bytes(jvm, obj, bytes, 4);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_dataoutputstream_writedouble(JVM* jvm, JavaThread* thread,
                                                      JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jdouble v = args[1].d;
    
    /* Convert double to long bits, then write as 8 bytes */
    jlong bits = *(jlong*)&v;
    
    uint8_t bytes[8];
    bytes[0] = (bits >> 56) & 0xFF;
    bytes[1] = (bits >> 48) & 0xFF;
    bytes[2] = (bits >> 40) & 0xFF;
    bytes[3] = (bits >> 32) & 0xFF;
    bytes[4] = (bits >> 24) & 0xFF;
    bytes[5] = (bits >> 16) & 0xFF;
    bytes[6] = (bits >> 8) & 0xFF;
    bytes[7] = bits & 0xFF;
    
    dos_write_bytes(jvm, obj, bytes, 8);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_dataoutputstream_writechar(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint v = args[1].i;
    
    /* Write char as 2 bytes in big-endian order (UTF-16) */
    uint8_t bytes[2];
    bytes[0] = (v >> 8) & 0xFF;
    bytes[1] = v & 0xFF;
    
    dos_write_bytes(jvm, obj, bytes, 2);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_dataoutputstream_writebytes(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaString* str = (JavaString*)args[1].ref;
    
    if (!str) return NATIVE_RETURN_VOID();
    
    /* Write string as bytes (high byte discarded) */
    const jchar* chars = string_chars(str);
    if (chars) {
        for (int i = 0; i < string_length(str); i++) {
            uint16_t ch = chars[i];
            dos_write_byte(jvm, obj, ch & 0xFF);
        }
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_dataoutputstream_flush(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* No-op for now */
    return NATIVE_RETURN_VOID();
}

static JavaValue native_dataoutputstream_close(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* No-op for now */
    return NATIVE_RETURN_VOID();
}

void init_java_io_dataoutputstream(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/io/DataOutputStream", "<init>", "(Ljava/io/OutputStream;)V", native_dataoutputstream_init},
        {"java/io/DataOutputStream", "write", "(I)V", native_dataoutputstream_write},
        {"java/io/DataOutputStream", "writeInt", "(I)V", native_dataoutputstream_writeint},
        {"java/io/DataOutputStream", "writeLong", "(J)V", native_dataoutputstream_writelong},
        {"java/io/DataOutputStream", "writeByte", "(I)V", native_dataoutputstream_writebyte},
        {"java/io/DataOutputStream", "writeShort", "(I)V", native_dataoutputstream_writeshort},
        {"java/io/DataOutputStream", "writeBoolean", "(Z)V", native_dataoutputstream_writeboolean},
        {"java/io/DataOutputStream", "writeFloat", "(F)V", native_dataoutputstream_writefloat},
        {"java/io/DataOutputStream", "writeDouble", "(D)V", native_dataoutputstream_writedouble},
        {"java/io/DataOutputStream", "writeChar", "(I)V", native_dataoutputstream_writechar},
        {"java/io/DataOutputStream", "writeUTF", "(Ljava/lang/String;)V", native_dataoutputstream_writeutf},
        {"java/io/DataOutputStream", "writeBytes", "(Ljava/lang/String;)V", native_dataoutputstream_writebytes},
        {"java/io/DataOutputStream", "flush", "()V", native_dataoutputstream_flush},
        {"java/io/DataOutputStream", "close", "()V", native_dataoutputstream_close},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * DataInputStream native methods
 */

/* REMOVED: native_datainputstream_read_byte_array - unused, see native_datainputstream_read_array below */

/* REMOVED: find_field_index - use native_get_field_slot instead */

/* DataInputStream.<init>(InputStream) - constructor */
static JavaValue native_datainputstream_init(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* in_stream = (JavaObject*)args[1].ref;
    
    if (g_j2me_runtime_debug) fprintf(stderr, "[DIS_INIT] obj=%p, in_stream=%p\n", (void*)obj, (void*)in_stream);
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue in_val = { .ref = in_stream };
    native_set_field_value(obj, "in", in_val);
    
    /* Verify the field was set correctly */
    JavaValue verify = native_get_field_value(obj, "in");
    if (g_j2me_runtime_debug) fprintf(stderr, "[DIS_INIT] Set 'in' field, verify: ref=%p (expected %p)\n", 
            (void*)verify.ref, (void*)in_stream);
    
    return NATIVE_RETURN_VOID();
}

/* Helper to read a single byte from underlying InputStream with proper position tracking */
static jint dis_read_byte(JVM* jvm, JavaObject* dis_obj) {
    (void)jvm;
    if (!dis_obj) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[DIS_READ] dis_obj is NULL\n");
        return -1;
    }
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value для доступа к полю "in" */
    JavaObject* in_stream = (JavaObject*)native_get_field_value(dis_obj, "in").ref;
    
    if (!in_stream) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[DIS_READ] in_stream is NULL\n");
        return -1;
    }
    
    /* Check if it's a ByteArrayInputStream */
    JavaClass* in_class = in_stream->header.clazz;
    if (!in_class || !in_class->class_name) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[DIS_READ] in_class or class_name is NULL\n");
        return -1;
    }
    
    if (strcmp(in_class->class_name, "java/io/ByteArrayInputStream") != 0) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[DIS_READ] Wrong class: %s (expected ByteArrayInputStream)\n", in_class->class_name);
        return -1;
    }
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    JavaArray* buf = (JavaArray*)native_get_field_value(in_stream, "buf").ref;
    jint pos = native_get_field_value(in_stream, "pos").i;
    jint count = native_get_field_value(in_stream, "count").i;
    
    if (!buf) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[DIS_READ] buf is NULL\n");
        return -1;
    }
    
    if (pos >= count) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[DIS_READ] EOF: pos(%d) >= count(%d)\n", pos, count);
        return -1;  /* EOF */
    }
    
    /* Read single byte */
    uint8_t* data = (uint8_t*)array_data(buf);
    jbyte val = (jbyte)data[pos];
    
    /* CRITICAL: Update position in the underlying stream */
    JavaValue new_pos = { .i = pos + 1 };
    native_set_field_value(in_stream, "pos", new_pos);
    
    return val & 0xFF;
}

/* DataInputStream.read() - reads a single byte */
static JavaValue native_datainputstream_read(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(-1);
    }
    
    jint val = dis_read_byte(jvm, obj);
    
    if (val < 0) {
        return NATIVE_RETURN_INT(-1);  /* EOF */
    }
    
    return NATIVE_RETURN_INT(val);
}

/* DataInputStream.read(byte[]) - reads into byte array */
static JavaValue native_datainputstream_read_array(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* buffer = (JavaArray*)args[1].ref;
    
    if (!obj || !buffer) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(-1);
    }
    
    uint8_t* data = (uint8_t*)array_data(buffer);
    int max_read = buffer->length;
    int bytes_read = 0;
    
    for (int i = 0; i < max_read; i++) {
        jint b = dis_read_byte(jvm, obj);
        if (b < 0) {
            break;
        }
        data[i] = (uint8_t)b;
        bytes_read++;
    }
    
    return NATIVE_RETURN_INT(bytes_read > 0 ? bytes_read : -1);
}

/* DataInputStream.read(byte[], int, int) - reads with offset/length */
static JavaValue native_datainputstream_read_offset(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* buffer = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint length = args[3].i;
    
    if (!obj || !buffer) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Bounds checking */
    if (offset < 0 || length < 0 || offset + length > (jint)buffer->length) {
        native_throw_aioobe(jvm, thread, offset);
        return NATIVE_RETURN_INT(-1);
    }
    
    uint8_t* data = (uint8_t*)array_data(buffer);
    int bytes_read = 0;
    
    for (int i = 0; i < length; i++) {
        jint b = dis_read_byte(jvm, obj);
        if (b < 0) {
            break;
        }
        data[offset + i] = (uint8_t)b;
        bytes_read++;
    }
    
    return NATIVE_RETURN_INT(bytes_read > 0 ? bytes_read : -1);
}

/* DataInputStream.skip(int) - skips bytes */
static JavaValue native_datainputstream_skip(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jlong n = args[1].j;
    
    if (!obj || n <= 0) {
        return NATIVE_RETURN_LONG(0);
    }
    
    long skipped = 0;
    for (int i = 0; i < n; i++) {
        jint b = dis_read_byte(jvm, obj);
        if (b < 0) break;
        skipped++;
    }
    
    return NATIVE_RETURN_LONG(skipped);
}

/* DataInputStream.available() - returns bytes available */
static JavaValue native_datainputstream_available(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    JavaObject* in_stream = (JavaObject*)native_get_field_value(obj, "in").ref;
    
    if (!in_stream) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Check if it's ByteArrayInputStream */
    JavaClass* in_class = in_stream->header.clazz;
    if (in_class && in_class->class_name && 
        strcmp(in_class->class_name, "java/io/ByteArrayInputStream") == 0) {
        
        /* ИСПРАВЛЕНО: Используем native_get_field_value */
        jint pos = native_get_field_value(in_stream, "pos").i;
        jint count = native_get_field_value(in_stream, "count").i;
        return NATIVE_RETURN_INT(count - pos);
    }
    
    return NATIVE_RETURN_INT(0);
}

/* DataInputStream.close() */
static JavaValue native_datainputstream_close(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* Nothing to do - underlying stream will be GC'd */
    return NATIVE_RETURN_VOID();
}

/* DataInputStream.readBoolean() */
static JavaValue native_datainputstream_readBoolean(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    jint b = dis_read_byte(jvm, obj);
    if (b < 0) {
        jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_INT(0);
    }
    
    return NATIVE_RETURN_INT(b != 0 ? 1 : 0);
}

/* DataInputStream.readUnsignedShort() */
static JavaValue native_datainputstream_readUnsignedShort(JVM* jvm, JavaThread* thread,
                                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    jint b1 = dis_read_byte(jvm, obj);
    jint b2 = dis_read_byte(jvm, obj);
    
    if (b1 < 0 || b2 < 0) {
        jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_INT(0);
    }
    
    return NATIVE_RETURN_INT((b1 << 8) | b2);
}

/* DataInputStream.readChar() */
static JavaValue native_datainputstream_readChar(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    jint b1 = dis_read_byte(jvm, obj);
    jint b2 = dis_read_byte(jvm, obj);
    
    if (b1 < 0 || b2 < 0) {
        jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_INT(0);
    }
    
    return NATIVE_RETURN_INT((b1 << 8) | b2);
}

/* DataInputStream.readUTF() - reads a UTF-8 encoded string */
static JavaValue native_datainputstream_readUTF(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Read length (unsigned short) */
    jint b1 = dis_read_byte(jvm, obj);
    jint b2 = dis_read_byte(jvm, obj);
    
    if (b1 < 0 || b2 < 0) {
        jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_NULL();
    }
    
    jint utf_length = (b1 << 8) | b2;
    
    if (utf_length == 0) {
        JavaString* empty = jvm_new_string(jvm, "");
        return NATIVE_RETURN_OBJECT(empty);
    }
    
    /* Read UTF-8 bytes - allocate extra byte for null terminator */
    uint8_t* utf_bytes = (uint8_t*)malloc(utf_length + 1);
    if (!utf_bytes) {
        native_throw_oome(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    int bytes_read = 0;
    for (int i = 0; i < utf_length; i++) {
        jint b = dis_read_byte(jvm, obj);
        if (b < 0) {
            free(utf_bytes);
            jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
            thread->pending_exception = jvm_exception_pending(jvm);
            return NATIVE_RETURN_NULL();
        }
        utf_bytes[i] = (uint8_t)b;
        bytes_read++;
    }
    
    /* Null-terminate the string! */
    utf_bytes[utf_length] = '\0';
    
    /* Convert to Java String */
    JavaString* str = jvm_new_string(jvm, (char*)utf_bytes);
    free(utf_bytes);
    
    return NATIVE_RETURN_OBJECT(str);
}

/* DataInputStream.readUTF(DataInput) - static version */
static JavaValue native_datainputstream_readUTF_static(JVM* jvm, JavaThread* thread,
                                                        JavaValue* args, int arg_count) {
    (void)arg_count;
    /* This is a static method that takes a DataInput */
    JavaObject* input = (JavaObject*)args[0].ref;
    
    if (!input) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Delegate to readUTF on the DataInput object */
    /* But since DataInput is an interface, we need to call the method dynamically */
    /* For simplicity, we'll assume it's a DataInputStream */
    
    /* Reuse the instance method implementation */
    JavaValue new_args[1] = { { .ref = input } };
    return native_datainputstream_readUTF(jvm, thread, new_args, 1);
}

/* DataInputStream.mark(int) - mark current position */
static JavaValue native_datainputstream_mark(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* TODO: Implement mark/reset if needed */
    return NATIVE_RETURN_VOID();
}

/* DataInputStream.reset() - reset to marked position */
static JavaValue native_datainputstream_reset(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* TODO: Implement mark/reset if needed */
    return NATIVE_RETURN_VOID();
}

/* DataInputStream.markSupported() */
static JavaValue native_datainputstream_markSupported(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* ByteArrayInputStream supports mark/reset */
    return NATIVE_RETURN_INT(1);
}

/* DataInputStream.readByte() - reads a single byte */
static JavaValue native_datainputstream_readbyte(JVM* jvm, JavaThread* thread,
JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    
    jint val = dis_read_byte(jvm, obj);
    
    //fprintf(stderr, "[DIS.readByte] Result: %d\n", val);
    
    if (val < 0) {
        /* Throw EOFException - критически важно для остановки цикла! */
        jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
        thread->pending_exception = jvm_exception_pending(jvm);
        
        /* Return zero value but exception will be detected by native_call() */
        return NATIVE_RETURN_INT(0);
    }
    
    return NATIVE_RETURN_INT((jbyte)val);
}

/* DataInputStream.readUnsignedByte() - reads an unsigned byte */
static JavaValue native_datainputstream_readunsignedbyte(JVM* jvm, JavaThread* thread,
                                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    
    jint val = dis_read_byte(jvm, obj);
    if (val < 0) {
        jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_INT(0);
    }
    
    return NATIVE_RETURN_INT(val);
}

/* DataInputStream.readShort() - reads 2 bytes as short */
static JavaValue native_datainputstream_readshort(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    
    jint b1 = dis_read_byte(jvm, obj);
    jint b2 = dis_read_byte(jvm, obj);
    
    if (b1 < 0 || b2 < 0) {
        jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_INT(0);
    }
    
    jshort val = (jshort)((b1 << 8) | b2);
    return NATIVE_RETURN_INT(val);
}

/* DataInputStream.readInt() - reads 4 bytes as int */
static JavaValue native_datainputstream_readint(JVM* jvm, JavaThread* thread,
JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    
    /* Read 4 bytes sequentially, each call advances position */
    jint b1 = dis_read_byte(jvm, obj);
    if (b1 < 0) {
        /* Throw EOFException на первом байте */
        jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_INT(0);
    }
    
    jint b2 = dis_read_byte(jvm, obj);
    jint b3 = dis_read_byte(jvm, obj);
    jint b4 = dis_read_byte(jvm, obj);
    
    if (b2 < 0 || b3 < 0 || b4 < 0) {
        jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_INT(0);
    }
    
    jint val = (b1 << 24) | (b2 << 16) | (b3 << 8) | b4;
    return NATIVE_RETURN_INT(val);
}


/* DataInputStream.readLong() - reads 8 bytes as long */
static JavaValue native_datainputstream_readlong(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_LONG(0);
    }
    
    jlong val = 0;
    for (int i = 0; i < 8; i++) {
        jint b = dis_read_byte(jvm, obj);
        if (b < 0) {
            jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
            thread->pending_exception = jvm_exception_pending(jvm);
            return NATIVE_RETURN_LONG(0);
        }
        val = (val << 8) | b;
    }
    
    return NATIVE_RETURN_LONG(val);
}

/* DataInputStream.readFully([B)V - reads bytes to fill array */
static JavaValue native_datainputstream_readfully(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* buffer = (JavaArray*)args[1].ref;
    
    if (!obj || !buffer) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    uint8_t* data = (uint8_t*)array_data(buffer);
    if (!data) {
        return NATIVE_RETURN_VOID();
    }
    
    for (int i = 0; i < (int)buffer->length; i++) {
        jint b = dis_read_byte(jvm, obj);
        if (b < 0) {
            jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
            thread->pending_exception = jvm_exception_pending(jvm);
            return NATIVE_RETURN_VOID();
        }
        data[i] = (uint8_t)b;
    }
    
    return NATIVE_RETURN_VOID();
}

/* DataInputStream.readFully([BII)V - with bounds checking */
static JavaValue native_datainputstream_readfully_offset(JVM* jvm, JavaThread* thread,
                                                          JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* buffer = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint len = args[3].i;
    
    if (!obj || !buffer) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    /* Bounds checking */
    if (offset < 0 || len < 0 || offset + len > (jint)buffer->length) {
        native_throw_aioobe(jvm, thread, offset);
        return NATIVE_RETURN_VOID();
    }
    
    uint8_t* data = (uint8_t*)array_data(buffer);
    if (!data) {
        return NATIVE_RETURN_VOID();
    }
    
    for (int i = 0; i < len; i++) {
        jint b = dis_read_byte(jvm, obj);
        if (b < 0) {
            /* Throw EOFException if we couldn't read all bytes */
            jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
            thread->pending_exception = jvm_exception_pending(jvm);
            return NATIVE_RETURN_VOID();
        }
        data[offset + i] = (uint8_t)b;
    }
    
    return NATIVE_RETURN_VOID();
}

/* DataInputStream.skipBytes(int) - skips n bytes */
static JavaValue native_datainputstream_skipbytes(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint n = args[1].i;
    
    if (!obj || n <= 0) {
        return NATIVE_RETURN_INT(0);
    }
    
    int skipped = 0;
    for (int i = 0; i < n; i++) {
        jint b = dis_read_byte(jvm, obj);
        if (b < 0) break;
        skipped++;
    }
    
    return NATIVE_RETURN_INT(skipped);
}

/* DataInputStream.readFloat() */
static JavaValue native_datainputstream_readfloat(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    /* Read 4 bytes as int, then convert to float */
    jint bits = 0;
    for (int i = 0; i < 4; i++) {
        jint b = dis_read_byte(jvm, obj);
        if (b < 0) {
            jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
            thread->pending_exception = jvm_exception_pending(jvm);
            return NATIVE_RETURN_FLOAT(0.0f);
        }
        bits = (bits << 8) | b;
    }
    
    jfloat val = *(jfloat*)&bits;
    return NATIVE_RETURN_FLOAT(val);
}

/* DataInputStream.readDouble() */
static JavaValue native_datainputstream_readdouble(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    /* Read 8 bytes as long, then convert to double */
    jlong bits = 0;
    for (int i = 0; i < 8; i++) {
        jint b = dis_read_byte(jvm, obj);
        if (b < 0) {
            jvm_throw_by_name(jvm, "java/io/EOFException", NULL);
            thread->pending_exception = jvm_exception_pending(jvm);
            return NATIVE_RETURN_DOUBLE(0.0);
        }
        bits = (bits << 8) | b;
    }
    
    jdouble val = *(jdouble*)&bits;
    return NATIVE_RETURN_DOUBLE(val);
}

void init_java_io_datainputstream(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/io/DataInputStream", "<init>", "(Ljava/io/InputStream;)V", native_datainputstream_init},
        {"java/io/DataInputStream", "read", "()I", native_datainputstream_read},
        {"java/io/DataInputStream", "read", "([B)I", native_datainputstream_read_array},
        {"java/io/DataInputStream", "read", "([BII)I", native_datainputstream_read_offset},
        {"java/io/DataInputStream", "readByte", "()B", native_datainputstream_readbyte},
        {"java/io/DataInputStream", "readUnsignedByte", "()I", native_datainputstream_readunsignedbyte},
        {"java/io/DataInputStream", "readShort", "()S", native_datainputstream_readshort},
        {"java/io/DataInputStream", "readUnsignedShort", "()I", native_datainputstream_readUnsignedShort},
        {"java/io/DataInputStream", "readChar", "()C", native_datainputstream_readChar},
        {"java/io/DataInputStream", "readInt", "()I", native_datainputstream_readint},
        {"java/io/DataInputStream", "readLong", "()J", native_datainputstream_readlong},
        {"java/io/DataInputStream", "readFloat", "()F", native_datainputstream_readfloat},
        {"java/io/DataInputStream", "readDouble", "()D", native_datainputstream_readdouble},
        {"java/io/DataInputStream", "readBoolean", "()Z", native_datainputstream_readBoolean},
        {"java/io/DataInputStream", "readUTF", "()Ljava/lang/String;", native_datainputstream_readUTF},
        {"java/io/DataInputStream", "readUTF", "(Ljava/io/DataInput;)Ljava/lang/String;", native_datainputstream_readUTF_static},
        {"java/io/DataInputStream", "readFully", "([B)V", native_datainputstream_readfully},
        {"java/io/DataInputStream", "readFully", "([BII)V", native_datainputstream_readfully_offset},
        {"java/io/DataInputStream", "skipBytes", "(I)I", native_datainputstream_skipbytes},
        {"java/io/DataInputStream", "skip", "(J)J", native_datainputstream_skip},
        {"java/io/DataInputStream", "available", "()I", native_datainputstream_available},
        {"java/io/DataInputStream", "close", "()V", native_datainputstream_close},
        {"java/io/DataInputStream", "mark", "(I)V", native_datainputstream_mark},
        {"java/io/DataInputStream", "reset", "()V", native_datainputstream_reset},
        {"java/io/DataInputStream", "markSupported", "()Z", native_datainputstream_markSupported},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered java/io/DataInputStream native methods (%zu)", 
            sizeof(methods) / sizeof(methods[0]));
}

/*
 * com.nokia.mid.sound.Sound native methods
 * Nokia's sound API for playing audio
 */

/* Sound stores audio data and native handle */
static JavaValue native_sound_init(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* data = (JavaArray*)args[1].ref;
    jint type = args[2].i;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue data_val = { .ref = data };
    JavaValue type_val = { .i = type };
    native_set_field_value(obj, "data", data_val);
    native_set_field_value(obj, "soundType", type_val);
    JavaValue state_val = { .i = 1 };  /* SOUND_STOPPED */
    native_set_field_value(obj, "state", state_val);
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_sound_play(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    JavaArray* data = (JavaArray*)native_get_field_value(obj, "data").ref;
    
    if (data && data->length > 0) {
        /* For now, just log - actual audio playback would require audio library */
        
        /* TODO: Implement actual audio playback using SDL audio or similar */
        /* The data is likely AMR format which needs decoding */
    }
    
    if (obj) {
        JavaValue state_val = { .i = 0 };  /* SOUND_PLAYING */
        native_set_field_value(obj, "state", state_val);
    }
    
    return NATIVE_RETURN_VOID();
}

/* Sound.play(int) - play with loop count */
static JavaValue native_sound_play_loop(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint loop = args[1].i;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    JavaArray* data = (JavaArray*)native_get_field_value(obj, "data").ref;
    
    if (data && data->length > 0) {
    }
    
    if (obj) {
        JavaValue state_val = { .i = 0 };  /* SOUND_PLAYING */
        native_set_field_value(obj, "state", state_val);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_sound_stop(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    /* TODO: Stop audio playback */
    if (obj) {
        JavaValue state_val = { .i = 1 };  /* SOUND_STOPPED */
        native_set_field_value(obj, "state", state_val);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_sound_getstate(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_INT(1);  /* SOUND_STOPPED */
    
    /* Nokia Sound states: SOUND_PLAYING=0, SOUND_STOPPED=1, SOUND_UNINITIALIZED=3 */
    JavaValue state_val = native_get_field_value(obj, "state");
    return NATIVE_RETURN_INT(state_val.i);
}

static JavaValue native_sound_setgain(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint gain = args[1].i;
    
    /* TODO: Set volume */
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_sound_release(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    /* TODO: Release audio resources */
    if (obj) {
        JavaValue state_val = { .i = 3 };  /* SOUND_UNINITIALIZED */
        native_set_field_value(obj, "state", state_val);
    }
    
    return NATIVE_RETURN_VOID();
}

/* Sound.getGain() - returns current gain level */
static JavaValue native_sound_getGain(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    (void)args;
    /* Return -1 to indicate gain control is not supported */
    return NATIVE_RETURN_INT(-1);
}

/* Sound.resume() - resume playing */
static JavaValue native_sound_resume(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    return NATIVE_RETURN_VOID();
}

/* Sound.getDuration() - get duration in milliseconds */
static JavaValue native_sound_getDuration(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    (void)args;
    /* Return -1 to indicate unknown duration */
    return NATIVE_RETURN_INT(-1);
}

/* Sound.setSoundListener(SoundListener) - set event listener */
static JavaValue native_sound_setSoundListener(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* listener = (JavaObject*)args[1].ref;
    
    if (obj) {
        JavaValue listener_val = { .ref = listener };
        native_set_field_value(obj, "soundListener", listener_val);
    }
    
    return NATIVE_RETURN_VOID();
}

/* Sound.init(byte[], int, int) - initialize with data, type and frames */
static JavaValue native_sound_init_frames(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* data = (JavaArray*)args[1].ref;
    jint type = args[2].i;
    jint frames = args[3].i;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue data_val = { .ref = data };
    JavaValue type_val = { .i = type };
    native_set_field_value(obj, "data", data_val);
    native_set_field_value(obj, "soundType", type_val);
    JavaValue state_val = { .i = 1 };  /* SOUND_STOPPED */
    native_set_field_value(obj, "state", state_val);
    
    (void)frames;
    return NATIVE_RETURN_VOID();
}

/* Sound default constructor ()V */
static JavaValue native_sound_init_default(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue null_val = { .ref = NULL };
    JavaValue zero_val = { .i = 0 };
    native_set_field_value(obj, "data", null_val);
    native_set_field_value(obj, "soundType", zero_val);
    JavaValue state_val = { .i = 1 };  /* SOUND_STOPPED */
    native_set_field_value(obj, "state", state_val);
    
    return NATIVE_RETURN_VOID();
}

/* Sound constructor with byte array only ([B)V */
static JavaValue native_sound_init_bytes(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* data = (JavaArray*)args[1].ref;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue data_val = { .ref = data };
    JavaValue zero_val = { .i = 0 };
    native_set_field_value(obj, "data", data_val);
    native_set_field_value(obj, "soundType", zero_val);
    JavaValue state_val = { .i = 1 };  /* SOUND_STOPPED */
    native_set_field_value(obj, "state", state_val);
    
    return NATIVE_RETURN_VOID();
}

/* Sound constructor with int and long (IJ)V - for tone sounds */
static JavaValue native_sound_init_int_long(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint type = args[1].i;
    jlong data = args[2].j;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue null_val = { .ref = NULL };
    JavaValue type_val = { .i = type };
    native_set_field_value(obj, "data", null_val);
    native_set_field_value(obj, "soundType", type_val);
    JavaValue state_val = { .i = 1 };  /* SOUND_STOPPED */
    native_set_field_value(obj, "state", state_val);
    
    return NATIVE_RETURN_VOID();
}

/* Sound constructor with int and double (ID)V - for tone sounds */
static JavaValue native_sound_init_int_double(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint type = args[1].i;
    jdouble data = args[2].d;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue null_val = { .ref = NULL };
    JavaValue type_val = { .i = type };
    native_set_field_value(obj, "data", null_val);
    native_set_field_value(obj, "soundType", type_val);
    JavaValue state_val = { .i = 1 };  /* SOUND_STOPPED */
    native_set_field_value(obj, "state", state_val);
    
    return NATIVE_RETURN_VOID();
}

/* Sound constructor with int only (I)V */
static JavaValue native_sound_init_int(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint type = args[1].i;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue null_val = { .ref = NULL };
    JavaValue type_val = { .i = type };
    native_set_field_value(obj, "data", null_val);
    native_set_field_value(obj, "soundType", type_val);
    JavaValue state_val = { .i = 1 };  /* SOUND_STOPPED */
    native_set_field_value(obj, "state", state_val);
    
    return NATIVE_RETURN_VOID();
}

/* Sound.getConcurrentSoundCount(int) - returns max concurrent sounds for a type */
static JavaValue native_sound_getConcurrentSoundCount(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* Return 1 - only one sound at a time supported */
    return NATIVE_RETURN_INT(1);
}

/* Sound.getSupportedFormats() - returns array of supported format constants */
static JavaValue native_sound_getSupportedFormats(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;
    /* Return int array {FORMAT_TONE=1, FORMAT_WAV=5} */
    JavaArray* arr = jvm_new_array(jvm, T_INT, 2, NULL);
    if (arr) {
        jint* elems = (jint*)array_data(arr);
        if (elems) {
            elems[0] = 1;  /* FORMAT_TONE */
            elems[1] = 5;  /* FORMAT_WAV */
        }
    }
    JavaValue result = { .ref = arr };
    return result;
}

void init_nokia_sound(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"com/nokia/mid/sound/Sound", "<init>", "()V", native_sound_init_default},
        {"com/nokia/mid/sound/Sound", "<init>", "([B)V", native_sound_init_bytes},
        {"com/nokia/mid/sound/Sound", "<init>", "([BI)V", native_sound_init},
        {"com/nokia/mid/sound/Sound", "<init>", "([BII)V", native_sound_init_frames},
        {"com/nokia/mid/sound/Sound", "<init>", "(I)V", native_sound_init_int},
        {"com/nokia/mid/sound/Sound", "<init>", "(IJ)V", native_sound_init_int_long},
        {"com/nokia/mid/sound/Sound", "<init>", "(ID)V", native_sound_init_int_double},
        {"com/nokia/mid/sound/Sound", "play", "()V", native_sound_play},
        {"com/nokia/mid/sound/Sound", "play", "(I)V", native_sound_play_loop},
        {"com/nokia/mid/sound/Sound", "stop", "()V", native_sound_stop},
        {"com/nokia/mid/sound/Sound", "getState", "()I", native_sound_getstate},
        {"com/nokia/mid/sound/Sound", "setGain", "(I)V", native_sound_setgain},
        {"com/nokia/mid/sound/Sound", "getGain", "()I", native_sound_getGain},
        {"com/nokia/mid/sound/Sound", "resume", "()V", native_sound_resume},
        {"com/nokia/mid/sound/Sound", "getDuration", "()I", native_sound_getDuration},
        {"com/nokia/mid/sound/Sound", "setSoundListener", "(Lcom/nokia/mid/sound/SoundListener;)V", native_sound_setSoundListener},
        {"com/nokia/mid/sound/Sound", "release", "()V", native_sound_release},
        {"com/nokia/mid/sound/Sound", "getConcurrentSoundCount", "(I)I", native_sound_getConcurrentSoundCount},
        {"com/nokia/mid/sound/Sound", "getSupportedFormats", "()[I", native_sound_getSupportedFormats},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered Nokia Sound native methods (%zu)", sizeof(methods) / sizeof(methods[0]));
}

/*
 * com.nokia.mid.ui.DirectUtils native methods
 * Nokia's utility class for creating images and getting DirectGraphics
 */

/* DirectUtils.createImage(int width, int height) - creates a mutable image */
static JavaValue native_directutils_createImage(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jint width = args[0].i;
    jint height = args[1].i;
    
    NATIVE_DEBUG("DirectUtils.createImage(%d, %d)", width, height);
    
    /* Create native image using midp_image_create from display.c */
    extern MidpImage* midp_image_create(int width, int height, bool mutable);
    MidpImage* img = midp_image_create(width, height, true);
    if (!img) {
        NATIVE_DEBUG("Failed to create native image");
        return NATIVE_RETURN_NULL();
    }
    
    /* Diagnostic: track image creation */
    if (g_j2me_runtime_debug) fprintf(stderr, "[DirectUtils.createImage] Created %dx%d mutable image, native=%p, pixels=%p\n",
            width, height, (void*)img, (void*)img->pixels);
    fflush(stderr);
    
    /* Create Image object using native factory */
    JavaClass* image_class = jvm_load_class(jvm, "javax/microedition/lcdui/Image");
    if (!image_class) {
        free(img);
        return NATIVE_RETURN_NULL();
    }
    
    /* Ensure class has space for nativePeer */
    extern void ensure_native_peer_field(JavaClass* clazz);
    ensure_native_peer_field(image_class);
    
    JavaObject* image = jvm_new_object(jvm, image_class);
    if (!image) {
        free(img);
        return NATIVE_RETURN_NULL();
    }
    
    /* Store the MidpImage* in the nativePeer field */
    extern void set_object_field_ref(JavaObject* obj, const char* name, JavaObject* value);
    set_object_field_ref(image, "nativePeer", (JavaObject*)img);
    
    if (g_j2me_runtime_debug) fprintf(stderr, "[DirectUtils.createImage] Java Image obj=%p -> native=%p\n", (void*)image, (void*)img);
    fflush(stderr);
    
    return NATIVE_RETURN_OBJECT(image);
}

/* DirectUtils.createImage(int width, int height, int color) - creates image with background */
static JavaValue native_directutils_createImage_color(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jint width = args[0].i;
    jint height = args[1].i;
    jint color = args[2].i;
    
    /* Create native image using midp_image_create from display.c */
    extern MidpImage* midp_image_create(int width, int height, bool mutable);
    MidpImage* img = midp_image_create(width, height, true);
    if (!img) return NATIVE_RETURN_NULL();
    
    /* Fill with color if specified */
    if (img->pixels) {
        uint32_t argb = (uint32_t)color;
        uint8_t a = (argb >> 24) & 0xFF;
        
        if (a == 0) {
            for (int i = 0; i < width * height; i++) {
                img->pixels[i] = 0;
            }
        } else {
            for (int i = 0; i < width * height; i++) {
                img->pixels[i] = argb;
            }
        }
    }
    
    JavaClass* image_class = jvm_load_class(jvm, "javax/microedition/lcdui/Image");
    if (!image_class) {
        free(img);
        return NATIVE_RETURN_NULL();
    }
    
    /* Ensure class has space for nativePeer */
    extern void ensure_native_peer_field(JavaClass* clazz);
    ensure_native_peer_field(image_class);
    
    JavaObject* image = jvm_new_object(jvm, image_class);
    if (!image) {
        free(img);
        return NATIVE_RETURN_NULL();
    }
    
    /* Store the MidpImage* in the nativePeer field */
    extern void set_object_field_ref(JavaObject* obj, const char* name, JavaObject* value);
    set_object_field_ref(image, "nativePeer", (JavaObject*)img);
    
    return NATIVE_RETURN_OBJECT(image);
}

/* DirectUtils.getDirectGraphics(Graphics g) - returns DirectGraphics adapter */
static JavaValue native_directutils_getDirectGraphics(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* graphics = (JavaObject*)args[0].ref;
    
    /* VERY FIRST - unbuffered marker */
    if (g_j2me_runtime_debug) {
        fprintf(stderr, "=== GET_DIRECT_GRAPHICS_CALLED ===\n");
        fflush(stderr);
    }
    
    if (g_j2me_runtime_debug) fprintf(stderr, "[getDirectGraphics] graphics_obj=%p\n", (void*)graphics);
    fflush(stderr);
    
    if (!graphics) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[getDirectGraphics] RETURN NULL: graphics is NULL\n");
        fflush(stderr);
        return NATIVE_RETURN_NULL();
    }
    
    /* Get the native MidpGraphics to log its details */
    extern MidpGraphics* get_graphics_from_object(JavaObject* obj);
    MidpGraphics* gfx = get_graphics_from_object(graphics);
    if (g_j2me_runtime_debug) fprintf(stderr, "[getDirectGraphics] native gfx=%p (%dx%d, pixels=%p)\n",
            (void*)gfx, gfx ? gfx->width : 0, gfx ? gfx->height : 0, gfx ? (void*)gfx->pixels : NULL);
    fflush(stderr);
    
    /* Create DirectGraphics object */
    JavaClass* dg_class = jvm_load_class(jvm, "com/nokia/mid/ui/DirectGraphics");
    if (!dg_class) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[getDirectGraphics] RETURN NULL: failed to load DirectGraphics class\n");
        fflush(stderr);
        return NATIVE_RETURN_NULL();
    }
    
    /* NOTE: DirectGraphics class already has 'graphics' field from stubs.c - don't call ensure_native_peer_field */
    
    JavaObject* dg = jvm_new_object(jvm, dg_class);
    
    if (dg) {
        /* Store the Graphics object in 'graphics' field (as defined in stubs.c) */
        extern void set_object_field_ref(JavaObject* obj, const char* name, JavaObject* value);
        set_object_field_ref(dg, "graphics", graphics);
        if (g_j2me_runtime_debug) fprintf(stderr, "[getDirectGraphics] Created DirectGraphics obj=%p wrapping Graphics %p\n",
                (void*)dg, (void*)graphics);
        fflush(stderr);
    } else {
        if (g_j2me_runtime_debug) fprintf(stderr, "[getDirectGraphics] RETURN NULL: failed to create DirectGraphics object\n");
        fflush(stderr);
    }
    
    return NATIVE_RETURN_OBJECT(dg);
}

/* DeviceControl.setLights(int num, int level) - control device lights */
static JavaValue native_devicecontrol_setLights(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    /* STATIC METHOD - no 'this' argument!
     * args[0] = num (light number)
     * args[1] = level (brightness level)
     */
    jint num = args[0].i;
    jint level = args[1].i;
    
    
    /* This is a stub - in real implementation this would control device backlight */
    return NATIVE_RETURN_VOID();
}

/* DeviceControl.startVibra(int frequency, long duration) - vibrate the device */
static JavaValue native_devicecontrol_startVibra(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    (void)args;
    /* No vibration support in libretro - no-op */
    return NATIVE_RETURN_VOID();
}

/* DeviceControl.stopVibra() - stop vibration */
static JavaValue native_devicecontrol_stopVibra(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    (void)args;
    /* No vibration support in libretro - no-op */
    return NATIVE_RETURN_VOID();
}

/* DeviceControl.flashLights(long duration) - flash device lights */
static JavaValue native_devicecontrol_flashLights(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    (void)args;
    /* No light control in libretro - no-op */
    return NATIVE_RETURN_VOID();
}

/* DeviceControl.getUserInactivityTime() - returns time of last user interaction */
static JavaValue native_devicecontrol_getUserInactivityTime(JVM* jvm, JavaThread* thread,
                                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    (void)args;
    /* Return 0 to indicate no inactivity tracking */
    return NATIVE_RETURN_INT(0);
}

/* DeviceControl.resetUserInactivityTime() - reset the inactivity timer */
static JavaValue native_devicecontrol_resetUserInactivityTime(JVM* jvm, JavaThread* thread,
                                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    (void)args;
    /* No-op */
    return NATIVE_RETURN_VOID();
}

void init_nokia_ui(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"com/nokia/mid/ui/DirectUtils", "createImage", "(II)Ljavax/microedition/lcdui/Image;", native_directutils_createImage},
        {"com/nokia/mid/ui/DirectUtils", "createImage", "(III)Ljavax/microedition/lcdui/Image;", native_directutils_createImage_color},
        {"com/nokia/mid/ui/DirectUtils", "getDirectGraphics", "(Ljavax/microedition/lcdui/Graphics;)Lcom/nokia/mid/ui/DirectGraphics;", native_directutils_getDirectGraphics},
        {"com/nokia/mid/ui/DeviceControl", "setLights", "(II)V", native_devicecontrol_setLights},
        {"com/nokia/mid/ui/DeviceControl", "startVibra", "(IJ)V", native_devicecontrol_startVibra},
        {"com/nokia/mid/ui/DeviceControl", "stopVibra", "()V", native_devicecontrol_stopVibra},
        {"com/nokia/mid/ui/DeviceControl", "flashLights", "(J)V", native_devicecontrol_flashLights},
        {"com/nokia/mid/ui/DeviceControl", "getUserInactivityTime", "()I", native_devicecontrol_getUserInactivityTime},
        {"com/nokia/mid/ui/DeviceControl", "resetUserInactivityTime", "()V", native_devicecontrol_resetUserInactivityTime},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered Nokia UI native methods");
}

/* Nokia misc API stubs: Clipboard */
void init_nokia_misc(JVM* jvm) {
    /* Clipboard stubs - no OS clipboard support in libretro */
    NativeMethodEntry methods[] = {
        {"com/nokia/mid/ui/Clipboard", "copyToClipboard", "(Ljava/lang/String;)V", NULL},  /* no-op */
        {"com/nokia/mid/ui/Clipboard", "copyFromClipboard", "()Ljava/lang/String;", NULL},  /* returns null */
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    
    /* Nokia M3D is now handled by init_nokia_m3d_impl() in nokia_m3d.c */
    
    NATIVE_DEBUG("Registered Nokia misc native methods");
}

/*
 * com.nokia.mid.ui.DirectGraphics native methods
 * Nokia's extended graphics API
 */

/* DirectGraphics.drawImage(Image img, int x, int y, int anchor, int manipulation) */
static JavaValue native_directgraphics_drawImage(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    
    /* Debug marker to verify function is called */
    if (g_j2me_runtime_debug) {
        fprintf(stderr, "=== DG_DRAW_IMAGE_CALLED ===\n");
        fflush(stderr);
    }
    
    JavaObject* dg = (JavaObject*)args[0].ref;
    JavaObject* image = (JavaObject*)args[1].ref;
    jint x = args[2].i;
    jint y = args[3].i;
    jint anchor = args[4].i;
    jint manipulation = args[5].i;
    
    if (g_j2me_runtime_debug) fprintf(stderr, "[DG.drawImage] dg=%p, image=%p, x=%d, y=%d, anchor=%d, manip=%d\n",
            (void*)dg, (void*)image, x, y, anchor, manipulation);
    fflush(stderr);
    
    if (!dg || !image) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[DG.drawImage] EARLY RETURN: dg or image is NULL\n");
        fflush(stderr);
        return NATIVE_RETURN_VOID();
    }
    
    /* Get the wrapped Graphics object from DirectGraphics.graphics field */
    JavaObject* graphics_obj = get_object_field_ref(dg, "graphics");
    if (g_j2me_runtime_debug) fprintf(stderr, "[DG.drawImage] graphics_obj (from 'graphics' field) = %p\n", (void*)graphics_obj);
    if (!graphics_obj) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[DG.drawImage] EARLY RETURN: graphics_obj is NULL\n");
        return NATIVE_RETURN_VOID();
    }
    
    /* Get the native MidpGraphics from Graphics.nativePeer */
    extern MidpGraphics* get_graphics_from_object(JavaObject* obj);
    MidpGraphics* gfx = get_graphics_from_object(graphics_obj);
    if (g_j2me_runtime_debug) fprintf(stderr, "[DG.drawImage] gfx=%p (width=%d, height=%d, pixels=%p)\n",
            (void*)gfx, gfx ? gfx->width : 0, gfx ? gfx->height : 0, gfx ? (void*)gfx->pixels : NULL);
    
    /* Check if this is screen graphics or offscreen image */
    extern MidpGraphics* sdl_get_graphics(SdlContext* ctx);
    SdlContext* sdl_ctx = sdl_get_global_context();
    MidpGraphics* screen_gfx = sdl_ctx ? sdl_get_graphics(sdl_ctx) : NULL;
    bool is_screen = (gfx == screen_gfx);
    
    if (!gfx || !gfx->pixels) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[DG.drawImage] EARLY RETURN: gfx or gfx->pixels is NULL\n");
        return NATIVE_RETURN_VOID();
    }
    
    /* Get the MidpImage from Image.nativePeer */
    extern MidpImage* get_image_from_object(JavaObject* obj);
    MidpImage* img = get_image_from_object(image);
    if (g_j2me_runtime_debug) fprintf(stderr, "[DG.drawImage] img=%p (%dx%d, pixels=%p)\n",
            (void*)img, img ? img->width : 0, img ? img->height : 0, img ? (void*)img->pixels : NULL);
    if (!img || !img->pixels) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[DG.drawImage] EARLY RETURN: img or img->pixels is NULL\n");
        return NATIVE_RETURN_VOID();
    }
    
    if (g_j2me_runtime_debug) fprintf(stderr, "[DG.drawImage] Drawing %dx%d image to %dx%d graphics at (%d,%d)\n",
            img->width, img->height, gfx->width, gfx->height, x, y);
    
    /* Nokia DirectGraphics manipulation values:
     * 0 = NONE
     * 90 = ROTATE_90 (clockwise)
     * 180 = ROTATE_180
     * 270 = ROTATE_270 (clockwise, or 90 counter-clockwise)
     * 0x2000 (8192) = FLIP_HORIZONTAL
     * 0x4000 (16384) = FLIP_VERTICAL
     * 
     * MIDP Sprite transforms:
     * 0 = TRANS_NONE
     * 1 = TRANS_MIRROR_ROT180 (mirror horizontal)
     * 2 = TRANS_MIRROR (mirror vertical)
     * 3 = TRANS_ROT180
     * 4 = TRANS_MIRROR_ROT270
     * 5 = TRANS_ROT90
     * 6 = TRANS_ROT270
     * 7 = TRANS_MIRROR_ROT90
     */
    int transform = 0;
    switch (manipulation) {
        case 0:     transform = 0; break;  /* NONE */
        case 90:    transform = 5; break;  /* ROTATE_90 -> TRANS_ROT90 */
        case 180:   transform = 3; break;  /* ROTATE_180 -> TRANS_ROT180 */
        case 270:   transform = 6; break;  /* ROTATE_270 -> TRANS_ROT270 */
        case 8192:  transform = 2; break;  /* FLIP_HORIZONTAL -> TRANS_MIRROR (vertical flip in MIDP) */
        case 16384: transform = 1; break;  /* FLIP_VERTICAL -> TRANS_MIRROR_ROT180 (horizontal flip in MIDP) */
        case 24576: transform = 3; break;  /* FLIP_HORIZONTAL | FLIP_VERTICAL = ROTATE_180 */
        default:    
            if (g_j2me_runtime_debug) fprintf(stderr, "[DG.drawImage] Unknown manipulation %d, using NONE\n", manipulation);
            transform = 0; 
            break;
    }
    
    if (g_j2me_runtime_debug) fprintf(stderr, "[DG.drawImage] manip %d -> MIDP transform %d\n", manipulation, transform);
    
    /* Use midp_graphics_draw_region with full image */
    extern void midp_graphics_draw_region(MidpGraphics* gfx, MidpImage* img,
                                          int x_src, int y_src, int w, int h,
                                          int transform, int x_dest, int y_dest, int anchor);
    midp_graphics_draw_region(gfx, img, 0, 0, img->width, img->height, transform, x, y, anchor);
    
    /* If drawing to screen, request redraw */
    if (is_screen) {
        extern void sdl_request_redraw(void);
        sdl_request_redraw();
    }
    
    return NATIVE_RETURN_VOID();
}

/* DirectGraphics.setARGBColor(int argb) */
static JavaValue native_directgraphics_setARGBColor(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* dg = (JavaObject*)args[0].ref;
    jint argb = args[1].i;
    
    if (!dg) return NATIVE_RETURN_VOID();
    
    /* Get the wrapped Graphics object from DirectGraphics.graphics field */
    JavaObject* graphics_obj = get_object_field_ref(dg, "graphics");
    if (!graphics_obj) return NATIVE_RETURN_VOID();
    
    /* Get the native graphics context */
    extern MidpGraphics* get_graphics_from_object(JavaObject* obj);
    MidpGraphics* gfx = get_graphics_from_object(graphics_obj);
    if (!gfx) return NATIVE_RETURN_VOID();
    
    /* Set ARGB color - extract RGB and alpha */
    gfx->rgb_color = argb & 0x00FFFFFF;  /* RGB part */
    gfx->alpha = (argb >> 24) & 0xFF;     /* Alpha part */
    
    /* Store alpha component for getAlphaComponent() */
    JavaValue alpha_val = { .i = (argb >> 24) & 0xFF };
    native_set_field_value(dg, "alphaComponent", alpha_val);
    
    return NATIVE_RETURN_VOID();
}

/* DirectGraphics.getAlphaComponent() - returns alpha channel */
static JavaValue native_directgraphics_getAlphaComponent(JVM* jvm, JavaThread* thread,
                                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* dg = (JavaObject*)args[0].ref;
    if (!dg) return NATIVE_RETURN_INT(255);
    
    /* Return stored alpha component */
    JavaValue alpha_val = native_get_field_value(dg, "alphaComponent");
    return NATIVE_RETURN_INT(alpha_val.i);
}

/* DirectGraphics.getNativePixelFormat() - returns pixel format */
static JavaValue native_directgraphics_getNativePixelFormat(JVM* jvm, JavaThread* thread,
                                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    /* Return TYPE_INT_8888_ARGB = 8888 (Nokia DirectGraphics constant) */
    return NATIVE_RETURN_INT(8888);
}

/* DirectGraphics.drawPixels(int[] pixels, boolean transparency, int offset, int scanlength, 
                              int x, int y, int width, int height, int manipulation, int format) */
static JavaValue native_directgraphics_drawPixels(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* dg = (JavaObject*)args[0].ref;
    JavaArray* pixels = (JavaArray*)args[1].ref;
    jboolean transparency = args[2].i;
    jint offset = args[3].i;
    jint scanlength = args[4].i;
    jint x = args[5].i;
    jint y = args[6].i;
    jint width = args[7].i;
    jint height = args[8].i;
    jint manipulation = args[9].i;
    jint format = args[10].i;
    
    if (!dg || !pixels || width <= 0 || height <= 0) return NATIVE_RETURN_VOID();
    
    /* Get the wrapped Graphics object from DirectGraphics.graphics field */
    JavaObject* graphics_obj = get_object_field_ref(dg, "graphics");
    if (!graphics_obj) return NATIVE_RETURN_VOID();
    
    /* Get the native graphics context */
    extern MidpGraphics* get_graphics_from_object(JavaObject* obj);
    MidpGraphics* gfx = get_graphics_from_object(graphics_obj);
    if (!gfx || !gfx->pixels) return NATIVE_RETURN_VOID();
    
    /* Get pixel data from array */
    if (pixels->element_type != T_INT) return NATIVE_RETURN_VOID();
    jint* pixel_data = (jint*)array_data(pixels);
    if (!pixel_data) return NATIVE_RETURN_VOID();
    
    /* Default scanlength to width if not specified */
    if (scanlength <= 0) scanlength = width;
    
    /* Draw pixels with manipulation support */
    for (int py = 0; py < height; py++) {
        for (int px = 0; px < width; px++) {
            int src_idx = offset + py * scanlength + px;
            if (src_idx < 0 || src_idx >= (int)pixels->length) continue;
            
            uint32_t src_color = (uint32_t)pixel_data[src_idx];
            
            /* Handle transparency */
            uint8_t alpha = (src_color >> 24) & 0xFF;
            if (transparency && alpha == 0) continue;
            
            /* Calculate destination coordinates with Nokia manipulation constants */
            int rotation = manipulation & 0x0FFF;
            int flip_h = (manipulation & 0x2000) != 0;  /* FLIP_HORIZONTAL = 8192 = 0x2000 */
            int flip_v = (manipulation & 0x4000) != 0;  /* FLIP_VERTICAL = 16384 = 0x4000 */
            
            int dx, dy;
            /* Nokia rotation is counter-clockwise, convert to coordinate mapping */
            switch (rotation) {
                case 0:   dx = px; dy = py; break;
                case 90:  dx = height - 1 - py; dy = px; break;
                case 180: dx = width - 1 - px; dy = height - 1 - py; break;
                case 270: dx = py; dy = width - 1 - px; break;
                default:  dx = px; dy = py; break;
            }
            
            if (flip_h) dx = width - 1 - dx;
            if (flip_v) dy = height - 1 - dy;
            
            int dst_x = x + dx + gfx->translate_x;
            int dst_y = y + dy + gfx->translate_y;
            
            /* Clip check */
            if (dst_x < gfx->clip_x || dst_x >= gfx->clip_x + gfx->clip_width) continue;
            if (dst_y < gfx->clip_y || dst_y >= gfx->clip_y + gfx->clip_height) continue;
            if (dst_x < 0 || dst_x >= gfx->width || dst_y < 0 || dst_y >= gfx->height) continue;
            
            /* Alpha blending */
            if (alpha == 255 || !transparency) {
                gfx->pixels[dst_y * gfx->width + dst_x] = src_color | 0xFF000000;
            } else if (alpha > 0) {
                uint32_t dst_color = gfx->pixels[dst_y * gfx->width + dst_x];
                uint8_t inv_alpha = 255 - alpha;
                
                uint8_t r = ((src_color >> 16) & 0xFF) * alpha / 255 + 
                           ((dst_color >> 16) & 0xFF) * inv_alpha / 255;
                uint8_t g = ((src_color >> 8) & 0xFF) * alpha / 255 +
                           ((dst_color >> 8) & 0xFF) * inv_alpha / 255;
                uint8_t b = (src_color & 0xFF) * alpha / 255 +
                           (dst_color & 0xFF) * inv_alpha / 255;
                
                gfx->pixels[dst_y * gfx->width + dst_x] = (0xFF << 24) | (r << 16) | (g << 8) | b;
            }
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* DirectGraphics.drawPolygon(int[] xPoints, int xOffset, int[] yPoints, int yOffset, 
                               int nPoints, int argbColor) */
static JavaValue native_directgraphics_drawPolygon(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* dg = (JavaObject*)args[0].ref;
    JavaArray* xPoints = (JavaArray*)args[1].ref;
    jint xOffset = args[2].i;
    JavaArray* yPoints = (JavaArray*)args[3].ref;
    jint yOffset = args[4].i;
    jint nPoints = args[5].i;
    jint argbColor = args[6].i;
    
    if (!dg || !xPoints || !yPoints || nPoints < 2) return NATIVE_RETURN_VOID();
    
    /* Get graphics context */
    JavaObject* graphics_obj = get_object_field_ref(dg, "graphics");
    if (!graphics_obj) return NATIVE_RETURN_VOID();
    
    extern MidpGraphics* get_graphics_from_object(JavaObject* obj);
    MidpGraphics* gfx = get_graphics_from_object(graphics_obj);
    if (!gfx || !gfx->pixels) return NATIVE_RETURN_VOID();
    
    /* Get point arrays */
    if (xPoints->element_type != T_INT || yPoints->element_type != T_INT) return NATIVE_RETURN_VOID();
    jint* x_data = (jint*)array_data(xPoints);
    jint* y_data = (jint*)array_data(yPoints);
    if (!x_data || !y_data) return NATIVE_RETURN_VOID();
    
    /* Draw polygon edges using line drawing */
    extern void midp_graphics_draw_line(MidpGraphics* gfx, int x1, int y1, int x2, int y2);
    
    /* Store original color and set new color */
    uint32_t orig_color = gfx->rgb_color;
    uint8_t orig_alpha = gfx->alpha;
    gfx->rgb_color = argbColor & 0x00FFFFFF;
    gfx->alpha = (argbColor >> 24) & 0xFF;
    if (gfx->alpha == 0) gfx->alpha = 255;
    
    for (int i = 0; i < nPoints - 1; i++) {
        int x1 = x_data[xOffset + i] + gfx->translate_x;
        int y1 = y_data[yOffset + i] + gfx->translate_y;
        int x2 = x_data[xOffset + i + 1] + gfx->translate_x;
        int y2 = y_data[yOffset + i + 1] + gfx->translate_y;
        midp_graphics_draw_line(gfx, x1, y1, x2, y2);
    }
    /* Close the polygon */
    if (nPoints > 2) {
        int x1 = x_data[xOffset + nPoints - 1] + gfx->translate_x;
        int y1 = y_data[yOffset + nPoints - 1] + gfx->translate_y;
        int x2 = x_data[xOffset] + gfx->translate_x;
        int y2 = y_data[yOffset] + gfx->translate_y;
        midp_graphics_draw_line(gfx, x1, y1, x2, y2);
    }
    
    /* Restore color */
    gfx->rgb_color = orig_color;
    gfx->alpha = orig_alpha;
    
    return NATIVE_RETURN_VOID();
}

/* DirectGraphics.fillPolygon(int[] xPoints, int xOffset, int[] yPoints, int yOffset, 
                               int nPoints, int argbColor) */
static JavaValue native_directgraphics_fillPolygon(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* dg = (JavaObject*)args[0].ref;
    JavaArray* xPoints = (JavaArray*)args[1].ref;
    jint xOffset = args[2].i;
    JavaArray* yPoints = (JavaArray*)args[3].ref;
    jint yOffset = args[4].i;
    jint nPoints = args[5].i;
    jint argbColor = args[6].i;
    
    if (!dg || !xPoints || !yPoints || nPoints < 3) return NATIVE_RETURN_VOID();
    
    /* Get graphics context */
    JavaObject* graphics_obj = get_object_field_ref(dg, "graphics");
    if (!graphics_obj) return NATIVE_RETURN_VOID();
    
    extern MidpGraphics* get_graphics_from_object(JavaObject* obj);
    MidpGraphics* gfx = get_graphics_from_object(graphics_obj);
    if (!gfx || !gfx->pixels) return NATIVE_RETURN_VOID();
    
    /* Get point arrays */
    if (xPoints->element_type != T_INT || yPoints->element_type != T_INT) return NATIVE_RETURN_VOID();
    jint* x_data = (jint*)array_data(xPoints);
    jint* y_data = (jint*)array_data(yPoints);
    if (!x_data || !y_data) return NATIVE_RETURN_VOID();
    
    /* Simple scanline fill algorithm */
    /* Find bounding box */
    int min_y = y_data[yOffset], max_y = y_data[yOffset];
    for (int i = 1; i < nPoints; i++) {
        int y = y_data[yOffset + i];
        if (y < min_y) min_y = y;
        if (y > max_y) max_y = y;
    }
    
    uint32_t fill_color = (uint32_t)argbColor;  /* Preserve full ARGB including alpha */
    
    /* Scanline fill */
    for (int y = min_y; y <= max_y; y++) {
        int intersections[32];  /* Max 32 intersections per scanline */
        int num_intersections = 0;
        
        /* Find intersections with all edges */
        for (int i = 0; i < nPoints; i++) {
            int j = (i + 1) % nPoints;
            int y1 = y_data[yOffset + i];
            int y2 = y_data[yOffset + j];
            int x1 = x_data[xOffset + i];
            int x2 = x_data[xOffset + j];
            
            if ((y1 <= y && y < y2) || (y2 <= y && y < y1)) {
                /* Edge intersects scanline */
                int x = x1 + (y - y1) * (x2 - x1) / (y2 - y1);
                if (num_intersections < 32) {
                    intersections[num_intersections++] = x;
                }
            }
        }
        
        /* Sort intersections */
        for (int i = 0; i < num_intersections - 1; i++) {
            for (int j = i + 1; j < num_intersections; j++) {
                if (intersections[i] > intersections[j]) {
                    int tmp = intersections[i];
                    intersections[i] = intersections[j];
                    intersections[j] = tmp;
                }
            }
        }
        
        /* Fill between pairs of intersections */
        for (int i = 0; i < num_intersections - 1; i += 2) {
            int x_start = intersections[i] + gfx->translate_x;
            int x_end = intersections[i + 1] + gfx->translate_x;
            int draw_y = y + gfx->translate_y;
            
            for (int x = x_start; x <= x_end; x++) {
                if (x >= gfx->clip_x && x < gfx->clip_x + gfx->clip_width &&
                    draw_y >= gfx->clip_y && draw_y < gfx->clip_y + gfx->clip_height &&
                    x >= 0 && x < gfx->width && draw_y >= 0 && draw_y < gfx->height) {
                    gfx->pixels[draw_y * gfx->width + x] = fill_color;
                }
            }
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* DirectGraphics.setPixels(int[] pixels, int offset, int scanlength, int x, int y, 
                             int width, int height, int format) */
static JavaValue native_directgraphics_setPixels(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* dg = (JavaObject*)args[0].ref;
    JavaArray* pixels = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint scanlength = args[3].i;
    jint x = args[4].i;
    jint y = args[5].i;
    jint width = args[6].i;
    jint height = args[7].i;
    jint format = args[8].i;
    
    if (!dg || !pixels || width <= 0 || height <= 0) return NATIVE_RETURN_VOID();
    
    /* Get graphics context */
    JavaObject* graphics_obj = get_object_field_ref(dg, "graphics");
    if (!graphics_obj) return NATIVE_RETURN_VOID();
    
    extern MidpGraphics* get_graphics_from_object(JavaObject* obj);
    MidpGraphics* gfx = get_graphics_from_object(graphics_obj);
    if (!gfx || !gfx->pixels) return NATIVE_RETURN_VOID();
    
    /* Get pixel data */
    if (pixels->element_type != T_INT) return NATIVE_RETURN_VOID();
    jint* pixel_data = (jint*)array_data(pixels);
    if (!pixel_data) return NATIVE_RETURN_VOID();
    
    if (scanlength <= 0) scanlength = width;
    
    /* Copy pixels directly to graphics buffer */
    for (int py = 0; py < height; py++) {
        int dst_y = y + py + gfx->translate_y;
        if (dst_y < gfx->clip_y || dst_y >= gfx->clip_y + gfx->clip_height) continue;
        if (dst_y < 0 || dst_y >= gfx->height) continue;
        
        for (int px = 0; px < width; px++) {
            int src_idx = offset + py * scanlength + px;
            if (src_idx < 0 || src_idx >= (int)pixels->length) continue;
            
            int dst_x = x + px + gfx->translate_x;
            if (dst_x < gfx->clip_x || dst_x >= gfx->clip_x + gfx->clip_width) continue;
            if (dst_x < 0 || dst_x >= gfx->width) continue;
            
            uint32_t color = (uint32_t)pixel_data[src_idx];
            gfx->pixels[dst_y * gfx->width + dst_x] = color | 0xFF000000;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* DirectGraphics.getPixels(int[] pixels, int offset, int scanlength, int x, int y, 
                             int width, int height, int format) */
static JavaValue native_directgraphics_getPixels(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* dg = (JavaObject*)args[0].ref;
    JavaArray* pixels = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint scanlength = args[3].i;
    jint x = args[4].i;
    jint y = args[5].i;
    jint width = args[6].i;
    jint height = args[7].i;
    jint format = args[8].i;
    
    if (!dg || !pixels || width <= 0 || height <= 0) return NATIVE_RETURN_VOID();
    
    /* Get graphics context */
    JavaObject* graphics_obj = get_object_field_ref(dg, "graphics");
    if (!graphics_obj) return NATIVE_RETURN_VOID();
    
    extern MidpGraphics* get_graphics_from_object(JavaObject* obj);
    MidpGraphics* gfx = get_graphics_from_object(graphics_obj);
    if (!gfx || !gfx->pixels) return NATIVE_RETURN_VOID();
    
    /* Get pixel data array */
    if (pixels->element_type != T_INT) return NATIVE_RETURN_VOID();
    jint* pixel_data = (jint*)array_data(pixels);
    if (!pixel_data) return NATIVE_RETURN_VOID();
    
    if (scanlength <= 0) scanlength = width;
    
    /* Copy pixels from graphics buffer to array */
    for (int py = 0; py < height; py++) {
        int src_y = y + py + gfx->translate_y;
        
        for (int px = 0; px < width; px++) {
            int dst_idx = offset + py * scanlength + px;
            if (dst_idx < 0 || dst_idx >= (int)pixels->length) continue;
            
            int src_x = x + px + gfx->translate_x;
            
            if (src_x >= 0 && src_x < gfx->width && src_y >= 0 && src_y < gfx->height) {
                pixel_data[dst_idx] = (jint)gfx->pixels[src_y * gfx->width + src_x];
            } else {
                pixel_data[dst_idx] = 0;
            }
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* DirectGraphics.drawTriangle(int x1, int y1, int x2, int y2, int x3, int y3, int argbColor) */
static JavaValue native_directgraphics_drawTriangle(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* dg = (JavaObject*)args[0].ref;
    jint x1 = args[1].i;
    jint y1 = args[2].i;
    jint x2 = args[3].i;
    jint y2 = args[4].i;
    jint x3 = args[5].i;
    jint y3 = args[6].i;
    jint argbColor = args[7].i;
    
    if (!dg) return NATIVE_RETURN_VOID();
    
    /* Get graphics context */
    JavaObject* graphics_obj = get_object_field_ref(dg, "graphics");
    if (!graphics_obj) return NATIVE_RETURN_VOID();
    
    extern MidpGraphics* get_graphics_from_object(JavaObject* obj);
    MidpGraphics* gfx = get_graphics_from_object(graphics_obj);
    if (!gfx || !gfx->pixels) return NATIVE_RETURN_VOID();
    
    /* Draw triangle edges */
    extern void midp_graphics_draw_line(MidpGraphics* gfx, int x1, int y1, int x2, int y2);
    
    /* Store and set color and alpha */
    uint32_t orig_color = gfx->rgb_color;
    uint8_t orig_alpha = gfx->alpha;
    gfx->rgb_color = argbColor & 0x00FFFFFF;
    gfx->alpha = (argbColor >> 24) & 0xFF;
    if (gfx->alpha == 0) gfx->alpha = 255;
    
    x1 += gfx->translate_x; y1 += gfx->translate_y;
    x2 += gfx->translate_x; y2 += gfx->translate_y;
    x3 += gfx->translate_x; y3 += gfx->translate_y;
    
    midp_graphics_draw_line(gfx, x1, y1, x2, y2);
    midp_graphics_draw_line(gfx, x2, y2, x3, y3);
    midp_graphics_draw_line(gfx, x3, y3, x1, y1);
    
    gfx->rgb_color = orig_color;
    gfx->alpha = orig_alpha;
    
    return NATIVE_RETURN_VOID();
}

/* DirectGraphics.fillTriangle(int x1, int y1, int x2, int y2, int x3, int y3, int argbColor) */
static JavaValue native_directgraphics_fillTriangle(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* dg = (JavaObject*)args[0].ref;
    jint x1 = args[1].i;
    jint y1 = args[2].i;
    jint x2 = args[3].i;
    jint y2 = args[4].i;
    jint x3 = args[5].i;
    jint y3 = args[6].i;
    jint argbColor = args[7].i;
    
    if (!dg) return NATIVE_RETURN_VOID();
    
    /* Get graphics context */
    JavaObject* graphics_obj = get_object_field_ref(dg, "graphics");
    if (!graphics_obj) return NATIVE_RETURN_VOID();
    
    extern MidpGraphics* get_graphics_from_object(JavaObject* obj);
    MidpGraphics* gfx = get_graphics_from_object(graphics_obj);
    if (!gfx || !gfx->pixels) return NATIVE_RETURN_VOID();
    
    uint32_t fill_color = (uint32_t)argbColor;  /* Preserve full ARGB including alpha */
    
    x1 += gfx->translate_x; y1 += gfx->translate_y;
    x2 += gfx->translate_x; y2 += gfx->translate_y;
    x3 += gfx->translate_x; y3 += gfx->translate_y;
    
    /* Sort vertices by Y coordinate */
    if (y1 > y2) { int tmp; tmp = x1; x1 = x2; x2 = tmp; tmp = y1; y1 = y2; y2 = tmp; }
    if (y2 > y3) { int tmp; tmp = x2; x2 = x3; x3 = tmp; tmp = y2; y2 = y3; y3 = tmp; }
    if (y1 > y2) { int tmp; tmp = x1; x1 = x2; x2 = tmp; tmp = y1; y1 = y2; y2 = tmp; }
    
    /* Fill triangle using scanline algorithm */
    for (int y = y1; y <= y3; y++) {
        int xl, xr;
        
        if (y < y2) {
            /* Upper half */
            if (y2 != y1) {
                xl = x1 + (y - y1) * (x2 - x1) / (y2 - y1);
            } else {
                xl = x1;
            }
            if (y3 != y1) {
                xr = x1 + (y - y1) * (x3 - x1) / (y3 - y1);
            } else {
                xr = x3;
            }
        } else {
            /* Lower half */
            if (y3 != y2) {
                xl = x2 + (y - y2) * (x3 - x2) / (y3 - y2);
            } else {
                xl = x2;
            }
            if (y3 != y1) {
                xr = x1 + (y - y1) * (x3 - x1) / (y3 - y1);
            } else {
                xr = x3;
            }
        }
        
        if (xl > xr) { int tmp = xl; xl = xr; xr = tmp; }
        
        /* Draw scanline */
        for (int x = xl; x <= xr; x++) {
            if (x >= gfx->clip_x && x < gfx->clip_x + gfx->clip_width &&
                y >= gfx->clip_y && y < gfx->clip_y + gfx->clip_height &&
                x >= 0 && x < gfx->width && y >= 0 && y < gfx->height) {
                gfx->pixels[y * gfx->width + x] = fill_color;
            }
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Stub implementations for missing DirectGraphics variants */

static JavaValue native_directgraphics_drawPixels_byte(JVM* jvm, JavaThread* thread,
                                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    /* TODO: byte[] 1-bit grayscale pixel drawing */
    return NATIVE_RETURN_VOID();
}

static JavaValue native_directgraphics_drawPixels_short(JVM* jvm, JavaThread* thread,
                                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    /* TODO: short[] 4444/565 pixel drawing */
    return NATIVE_RETURN_VOID();
}

static JavaValue native_directgraphics_getPixels_byte(JVM* jvm, JavaThread* thread,
                                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_directgraphics_getPixels_short(JVM* jvm, JavaThread* thread,
                                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_directgraphics_fillTriangle_nocolor(JVM* jvm, JavaThread* thread,
                                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* dg = (JavaObject*)args[0].ref;
    if (!dg) return NATIVE_RETURN_VOID();
    JavaObject* graphics_obj = get_object_field_ref(dg, "graphics");
    if (!graphics_obj) return NATIVE_RETURN_VOID();
    extern MidpGraphics* get_graphics_from_object(JavaObject* obj);
    MidpGraphics* gfx = get_graphics_from_object(graphics_obj);
    if (!gfx || !gfx->pixels) return NATIVE_RETURN_VOID();
    jint x1 = args[1].i, y1 = args[2].i, x2 = args[3].i, y2 = args[4].i, x3 = args[5].i, y3 = args[6].i;
    uint32_t fill_color = (gfx->alpha << 24) | gfx->rgb_color;
    x1 += gfx->translate_x; y1 += gfx->translate_y;
    x2 += gfx->translate_x; y2 += gfx->translate_y;
    x3 += gfx->translate_x; y3 += gfx->translate_y;
    if (y1 > y2) { int t; t=x1; x1=x2; x2=t; t=y1; y1=y2; y2=t; }
    if (y2 > y3) { int t; t=x2; x2=x3; x3=t; t=y2; y2=y3; y3=t; }
    if (y1 > y2) { int t; t=x1; x1=x2; x2=t; t=y1; y1=y2; y2=t; }
    for (int y = y1; y <= y3; y++) {
        int xl, xr;
        if (y < y2) {
            xl = (y2!=y1) ? x1 + (y-y1)*(x2-x1)/(y2-y1) : x1;
            xr = (y3!=y1) ? x1 + (y-y1)*(x3-x1)/(y3-y1) : x3;
        } else {
            xl = (y3!=y2) ? x2 + (y-y2)*(x3-x2)/(y3-y2) : x2;
            xr = (y3!=y1) ? x1 + (y-y1)*(x3-x1)/(y3-y1) : x3;
        }
        if (xl > xr) { int t = xl; xl = xr; xr = t; }
        for (int x = xl; x <= xr; x++) {
            if (x>=0 && x<gfx->width && y>=0 && y<gfx->height &&
                x>=gfx->clip_x && x<gfx->clip_x+gfx->clip_width &&
                y>=gfx->clip_y && y<gfx->clip_y+gfx->clip_height)
                gfx->pixels[y*gfx->width+x] = fill_color;
        }
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue native_directgraphics_drawTriangle_nocolor(JVM* jvm, JavaThread* thread,
                                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    /* TODO: draw triangle outline using current color */
    return NATIVE_RETURN_VOID();
}

void init_nokia_direct_graphics(JVM* jvm) {
    /* MARKER: This proves the code was recompiled */
    if (g_j2me_runtime_debug) {
        fprintf(stderr, "### DIRECTGRAPHICS NATIVE METHODS REGISTERING (v2) ###\n");
        fflush(stderr);
    }
    
    NativeMethodEntry methods[] = {
        {"com/nokia/mid/ui/DirectGraphics", "drawImage", "(Ljavax/microedition/lcdui/Image;IIII)V", native_directgraphics_drawImage},
        {"com/nokia/mid/ui/DirectGraphics", "setARGBColor", "(I)V", native_directgraphics_setARGBColor},
        {"com/nokia/mid/ui/DirectGraphics", "getAlphaComponent", "()I", native_directgraphics_getAlphaComponent},
        {"com/nokia/mid/ui/DirectGraphics", "getNativePixelFormat", "()I", native_directgraphics_getNativePixelFormat},
        {"com/nokia/mid/ui/DirectGraphics", "drawPixels", "([IZIIIIIIII)V", native_directgraphics_drawPixels},
        {"com/nokia/mid/ui/DirectGraphics", "drawPolygon", "([II[IIII)V", native_directgraphics_drawPolygon},
        {"com/nokia/mid/ui/DirectGraphics", "fillPolygon", "([II[IIII)V", native_directgraphics_fillPolygon},
        {"com/nokia/mid/ui/DirectGraphics", "setPixels", "([IIIIIIII)V", native_directgraphics_setPixels},
        {"com/nokia/mid/ui/DirectGraphics", "getPixels", "([IIIIIIII)V", native_directgraphics_getPixels},
        {"com/nokia/mid/ui/DirectGraphics", "drawTriangle", "(IIIIIII)V", native_directgraphics_drawTriangle},
        {"com/nokia/mid/ui/DirectGraphics", "fillTriangle", "(IIIIIII)V", native_directgraphics_fillTriangle},

        /* DirectGraphics.drawPixels with byte[] pixels - Nokia format 1/2 */
        {"com/nokia/mid/ui/DirectGraphics", "drawPixels", "([B[BIIIIIIII)V", native_directgraphics_drawPixels_byte},
        /* DirectGraphics.drawPixels with short[] pixels - Nokia format 4444/565 etc. */
        {"com/nokia/mid/ui/DirectGraphics", "drawPixels", "([SZIIIIIIII)V", native_directgraphics_drawPixels_short},
        /* DirectGraphics.getPixels with byte[] pixels */
        {"com/nokia/mid/ui/DirectGraphics", "getPixels", "([B[BIIIIII)V", native_directgraphics_getPixels_byte},
        /* DirectGraphics.getPixels with short[] pixels */
        {"com/nokia/mid/ui/DirectGraphics", "getPixels", "([SIIIIII)V", native_directgraphics_getPixels_short},

        /* DirectGraphics.fillTriangle without argbColor (uses current color) */
        {"com/nokia/mid/ui/DirectGraphics", "fillTriangle", "(IIIIII)V", native_directgraphics_fillTriangle_nocolor},
        /* DirectGraphics.drawTriangle without argbColor (uses current color) */
        {"com/nokia/mid/ui/DirectGraphics", "drawTriangle", "(IIIIII)V", native_directgraphics_drawTriangle_nocolor},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered DirectGraphics native methods (%zu)", 
            sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.lang.String native methods
 */

/* Forward declaration for case conversion helper */
static jchar to_lower_for_compare(jchar c);

/* === String field access helpers using proper field slot calculation === */

/* Cache for String field slots */
static int string_field_value_slot = -1;
static int string_field_offset_slot = -1;
static int string_field_count_slot = -1;
static int string_field_hash_slot = -1;
static bool string_fields_initialized = false;

/* Initialize String field slot cache */
static void string_init_field_slots(JavaClass* string_class) {
    if (string_fields_initialized || !string_class) return;
    
    string_field_value_slot = native_get_field_slot(string_class, "value");
    string_field_offset_slot = native_get_field_slot(string_class, "offset");
    string_field_count_slot = native_get_field_slot(string_class, "count");
    string_field_hash_slot = native_get_field_slot(string_class, "hash");
    
    NATIVE_DEBUG("String field slots: value=%d, offset=%d, count=%d, hash=%d",
            string_field_value_slot, string_field_offset_slot, 
            string_field_count_slot, string_field_hash_slot);
    
    string_fields_initialized = true;
}

/* Get String class and initialize field slots */
static JavaClass* string_get_class(JVM* jvm) {
    JavaClass* string_class = jvm_load_class(jvm, "java/lang/String");
    if (string_class && !string_fields_initialized) {
        string_init_field_slots(string_class);
    }
    return string_class;
}

/* Helper: set String value field (char[]) */
static void string_set_value(JavaObject* str, JavaArray* value) {
    if (!str || string_field_value_slot < 0) return;
    JavaValue v = { .ref = value };
    str->fields[string_field_value_slot] = v;
}

/* Helper: set String offset field (int) */
static void string_set_offset(JavaObject* str, jint offset) {
    if (!str || string_field_offset_slot < 0) return;
    JavaValue v = { .i = offset };
    str->fields[string_field_offset_slot] = v;
}

/* Helper: set String count field (int) */
static void string_set_count(JavaObject* str, jint count) {
    if (!str || string_field_count_slot < 0) return;
    JavaValue v = { .i = count };
    str->fields[string_field_count_slot] = v;
}

/* Helper: set String hash field (int) */
static void string_set_hash(JavaObject* str, jint hash) {
    if (!str || string_field_hash_slot < 0) return;
    JavaValue v = { .i = hash };
    str->fields[string_field_hash_slot] = v;
}

/* REMOVED: string_get_value - unused helper */

/* REMOVED: string_get_count - unused helper */

/* Public accessor functions for heap.c to use */
int native_get_string_value_slot(JVM* jvm) {
    string_get_class(jvm);  /* Initialize slots if needed */
    return string_field_value_slot;
}

int native_get_string_count_slot(JVM* jvm) {
    string_get_class(jvm);  /* Initialize slots if needed */
    return string_field_count_slot;
}

int native_get_string_hash_slot(JVM* jvm) {
    string_get_class(jvm);  /* Initialize slots if needed */
    return string_field_hash_slot;
}

int native_get_string_offset_slot(JVM* jvm) {
    string_get_class(jvm);  /* Initialize slots if needed */
    return string_field_offset_slot;
}

static JavaValue native_string_hashCode(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    return NATIVE_RETURN_INT(str ? string_hash(str) : 0);
}

/* === String Intern Pool (hash-based for O(1) lookup) === */
#define INTERN_POOL_SIZE 1024
#define INTERN_HASH_SIZE 1024
#define INTERN_HASH_MASK (INTERN_HASH_SIZE - 1)

/* NON-STATIC: intern_pool must be accessible to GC in heap.c for marking */
JavaString* intern_pool[INTERN_POOL_SIZE];
int intern_pool_count = 0;

/* Hash table for fast intern lookup (keyed by string hash + length) */
typedef struct InternHashEntry {
    juint hash;
    jsize length;
    struct InternHashEntry* next;
    JavaString* str;
} InternHashEntry;

static InternHashEntry* intern_hash_table[INTERN_HASH_SIZE];

/* CRITICAL: Check if a string is still valid (not freed by GC) */
static bool is_valid_intern_string(JavaString* str) {
    if (!str) return false;
    
    /* Check if pointer is in heap range */
    extern void* g_heap_start;
    extern void* g_heap_end;
    if ((void*)str < g_heap_start || (void*)str >= g_heap_end) {
        return false;
    }
    
    /* Check GC header type - must be OBJ_TYPE_STRING (2) or OBJ_TYPE_OBJECT (1) for Java Strings */
    /* GCObjectHeader is defined in heap.h which is already included */
    GCObjectHeader* header = (GCObjectHeader*)((uint8_t*)str - sizeof(GCObjectHeader));
    
    /* Check if type is STRING (2) or OBJECT (1) - both are valid for intern pool */
    /* OBJ_TYPE_STRING = native strings, OBJ_TYPE_OBJECT = Java String objects */
    if (header->type != OBJ_TYPE_STRING && header->type != OBJ_TYPE_OBJECT) {
        /* Silently return false - no spam in logs */
        return false;
    }
    
    /* Check if object was freed */
    if (header->type == OBJ_TYPE_FREE) {
        return false;
    }
    
    return true;
}

/* Clean up freed strings from intern pool - called from native_gc_notify */
static void cleanup_intern_pool(void) {
    /* Remove freed strings from pool and rebuild hash table */
    int write_idx = 0;
    for (int read_idx = 0; read_idx < intern_pool_count; read_idx++) {
        if (is_valid_intern_string(intern_pool[read_idx])) {
            intern_pool[write_idx++] = intern_pool[read_idx];
        }
    }
    intern_pool_count = write_idx;
    
    /* Rebuild hash table */
    memset(intern_hash_table, 0, sizeof(intern_hash_table));
    for (int i = 0; i < intern_pool_count; i++) {
        JavaString* str = intern_pool[i];
        jsize len = string_length(str);
        const jchar* chars = string_chars(str);
        juint h = 0;
        if (chars && len > 0) {
            for (jsize j = 0; j < len; j++) h = h * 31 + chars[j];
        }
        InternHashEntry* he = (InternHashEntry*)malloc(sizeof(InternHashEntry));
        if (he) {
            he->hash = (juint)h;
            he->length = len;
            he->str = str;
            uint32_t idx = (juint)h & INTERN_HASH_MASK;
            he->next = intern_hash_table[idx];
            intern_hash_table[idx] = he;
        }
    }
}

/* Check if two strings are equal */
static bool string_equals_intern(JavaString* s1, JavaString* s2) {
    if (s1 == s2) return true;
    if (!s1 || !s2) return false;
    
    jsize len1 = string_length(s1);
    jsize len2 = string_length(s2);
    if (len1 != len2) return false;
    
    const jchar* c1 = string_chars(s1);
    const jchar* c2 = string_chars(s2);
    
    /* CRITICAL FIX: Check for NULL pointers before accessing char data */
    if (!c1 || !c2) {
        return false;
    }
    
    for (jsize i = 0; i < len1; i++) {
        if (c1[i] != c2[i]) return false;
    }
    return true;
}

/* Intern a string - returns the canonical representation (public API) */
JavaString* native_intern_string(JVM* jvm, JavaString* str) {
    (void)jvm;
    if (!str) return NULL;

    jsize len = string_length(str);
    const jchar* chars = string_chars(str);
    
    /* Compute hash from UTF-16 chars */
    juint h = 0;
    if (chars && len > 0) {
        for (jsize i = 0; i < len; i++) {
            h = h * 31 + chars[i];
        }
    }
    
    /* Fast path: check hash table first */
    uint32_t idx = h & INTERN_HASH_MASK;
    for (InternHashEntry* e = intern_hash_table[idx]; e; e = e->next) {
        if (e->hash == (juint)h && e->length == len && is_valid_intern_string(e->str)) {
            if (string_equals_intern(str, e->str)) {
                return e->str;
            }
        }
    }
    
    /* Fallback: also check the flat array for any entries not in hash table */
    for (int i = 0; i < intern_pool_count; i++) {
        if (!is_valid_intern_string(intern_pool[i])) continue;
        if (string_equals_intern(str, intern_pool[i])) {
            return intern_pool[i];
        }
    }
    
    /* Add to pool and hash table if space available */
    if (intern_pool_count < INTERN_POOL_SIZE) {
        intern_pool[intern_pool_count++] = str;
        
        /* Add to hash table */
        InternHashEntry* he = (InternHashEntry*)malloc(sizeof(InternHashEntry));
        if (he) {
            he->hash = (juint)h;
            he->length = len;
            he->str = str;
            he->next = intern_hash_table[idx];
            intern_hash_table[idx] = he;
        }
    }
    
    return str;
}

/* Look up an interned string by UTF-8 content — returns existing interned string or NULL */
JavaString* native_intern_find_by_utf8(const char* utf8, jsize utf8_len) {
    if (!utf8 || utf8_len <= 0) return NULL;
    
    /* Decode UTF-8 to compute hash and compare */
    juint h = 0;
    int pos = 0;
    while (pos < utf8_len) {
        unsigned char c = (unsigned char)utf8[pos];
        jchar ch;
        if (c < 0x80) {
            ch = c;
            pos++;
        } else if ((c & 0xE0) == 0xC0) {
            ch = ((c & 0x1F) << 6) | ((unsigned char)utf8[pos+1] & 0x3F);
            pos += 2;
        } else {
            ch = ((c & 0x0F) << 12) | (((unsigned char)utf8[pos+1] & 0x3F) << 6) | ((unsigned char)utf8[pos+2] & 0x3F);
            pos += 3;
        }
        h = h * 31 + ch;
    }
    
    /* Compute char length */
    jsize char_len = 0;
    pos = 0;
    while (pos < utf8_len) {
        unsigned char c = (unsigned char)utf8[pos];
        if (c < 0x80) pos++;
        else if ((c & 0xE0) == 0xC0) pos += 2;
        else pos += 3;
        char_len++;
    }
    
    /* Search hash table */
    uint32_t idx = h & INTERN_HASH_MASK;
    for (InternHashEntry* e = intern_hash_table[idx]; e; e = e->next) {
        if (e->hash == (juint)h && e->length == char_len && is_valid_intern_string(e->str)) {
            /* Compare content */
            const jchar* ech = string_chars(e->str);
            if (!ech) continue;
            
            pos = 0;
            int match = 1;
            for (jsize i = 0; i < char_len; i++) {
                unsigned char c = (unsigned char)utf8[pos];
                jchar ch;
                if (c < 0x80) { ch = c; pos++; }
                else if ((c & 0xE0) == 0xC0) { ch = ((c & 0x1F) << 6) | ((unsigned char)utf8[pos+1] & 0x3F); pos += 2; }
                else { ch = ((c & 0x0F) << 12) | (((unsigned char)utf8[pos+1] & 0x3F) << 6) | ((unsigned char)utf8[pos+2] & 0x3F); pos += 3; }
                if (ech[i] != ch) { match = 0; break; }
            }
            if (match) return e->str;
        }
    }
    
    return NULL;
}

static JavaValue native_string_intern(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;

    if (!str) return NATIVE_RETURN_NULL();

    /* Use the public intern function */
    JavaString* interned = native_intern_string(jvm, str);
    return NATIVE_RETURN_OBJECT(interned);
}

/* String.toString() - returns the string itself */
static JavaValue native_string_toString(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;

    /* toString() returns the string itself (this) */
    return NATIVE_RETURN_OBJECT(str);
}

/* String.length() - returns the length of the string */
static JavaValue native_string_length(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    
    return NATIVE_RETURN_INT(string_length(str));
}

/* String.charAt(int) - returns character at index */
static JavaValue native_string_charAt(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    jint index = args[1].i;
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    
    if (index < 0 || index >= string_length(str)) {
        native_throw_aioobe(jvm, thread, index);
        return NATIVE_RETURN_INT(0);
    }
    
    const jchar* chars = string_chars(str);
    return NATIVE_RETURN_INT((jint)chars[index]);
}

/* String.isEmpty() - returns true if length is 0 */
static JavaValue native_string_isEmpty(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    
    return NATIVE_RETURN_INT(str ? (string_length(str) == 0 ? 1 : 0) : 1);
}

/* String.startsWith(String) - check if string starts with prefix */
static JavaValue native_string_startsWith(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    JavaString* prefix = (JavaString*)args[1].ref;
    
    if (!str || !prefix) {
        return NATIVE_RETURN_INT(0);
    }
    
    if (string_length(prefix) > string_length(str)) {
        return NATIVE_RETURN_INT(0);
    }
    
    const jchar* str_chars = string_chars(str);
    const jchar* prefix_chars = string_chars(prefix);
    
    for (jsize i = 0; i < string_length(prefix); i++) {
        if (str_chars[i] != prefix_chars[i]) {
            return NATIVE_RETURN_INT(0);
        }
    }
    
    return NATIVE_RETURN_INT(1);
}

/* String.startsWith(String, int) - check if string starts with prefix at offset */
static JavaValue native_string_startsWith_offset(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    JavaString* prefix = (JavaString*)args[1].ref;
    jint offset = args[2].i;
    
    if (!str || !prefix) {
        return NATIVE_RETURN_INT(0);
    }
    
    jsize str_len = string_length(str);
    jsize prefix_len = string_length(prefix);
    
    /* Check if offset is valid */
    if (offset < 0 || offset > str_len) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Check if there's enough room for prefix after offset */
    if (prefix_len > str_len - offset) {
        return NATIVE_RETURN_INT(0);
    }
    
    const jchar* str_chars = string_chars(str);
    const jchar* prefix_chars = string_chars(prefix);
    
    /* Compare characters starting at offset */
    for (jsize i = 0; i < prefix_len; i++) {
        if (str_chars[offset + i] != prefix_chars[i]) {
            return NATIVE_RETURN_INT(0);
        }
    }
    
    return NATIVE_RETURN_INT(1);
}

/* String.endsWith(String) - check if string ends with suffix */
static JavaValue native_string_endsWith(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    JavaString* suffix = (JavaString*)args[1].ref;
    
    if (!str || !suffix) {
        return NATIVE_RETURN_INT(0);
    }
    
    if (string_length(suffix) > string_length(str)) {
        return NATIVE_RETURN_INT(0);
    }
    
    const jchar* str_chars = string_chars(str);
    const jchar* suffix_chars = string_chars(suffix);
    
    jsize start = string_length(str) - string_length(suffix);
    for (jsize i = 0; i < string_length(suffix); i++) {
        if (str_chars[start + i] != suffix_chars[i]) {
            return NATIVE_RETURN_INT(0);
        }
    }
    
    return NATIVE_RETURN_INT(1);
}

/* String.regionMatches(boolean, int, String, int, int) - compare regions of strings */
static JavaValue native_string_regionMatches(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    jboolean ignoreCase = (jboolean)args[1].i;
    jint toffset = args[2].i;
    JavaString* other = (JavaString*)args[3].ref;
    jint ooffset = args[4].i;
    jint len = args[5].i;
    
    /* Null check */
    if (!str || !other) {
        return NATIVE_RETURN_INT(0);
    }
    
    jsize str_len = string_length(str);
    jsize other_len = string_length(other);
    
    /* Bounds check */
    if (toffset < 0 || ooffset < 0 || len < 0 ||
        toffset + len > str_len || ooffset + len > other_len) {
        return NATIVE_RETURN_INT(0);
    }
    
    const jchar* str_chars = string_chars(str);
    const jchar* other_chars = string_chars(other);
    
    /* Compare regions */
    for (jint i = 0; i < len; i++) {
        jchar c1 = str_chars[toffset + i];
        jchar c2 = other_chars[ooffset + i];
        
        if (ignoreCase) {
            /* Case-insensitive comparison */
            c1 = to_lower_for_compare(c1);
            c2 = to_lower_for_compare(c2);
        }
        
        if (c1 != c2) {
            return NATIVE_RETURN_INT(0);
        }
    }
    
    return NATIVE_RETURN_INT(1);
}

/* String.indexOf(int) - find first occurrence of character */
static JavaValue native_string_indexOf(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    jchar ch = (jchar)args[1].i;
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(-1);
    }
    
    const jchar* chars = string_chars(str);
    
    for (jsize i = 0; i < string_length(str); i++) {
        if (chars[i] == ch) {
            return NATIVE_RETURN_INT((jint)i);
        }
    }
    
    return NATIVE_RETURN_INT(-1);
}

/* String.indexOf(int, int) - find character starting from index */
static JavaValue native_string_indexOf_from(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    jchar ch = (jchar)args[1].i;
    jint fromIndex = args[2].i;
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(-1);
    }
    
    if (fromIndex < 0) fromIndex = 0;
    
    const jchar* chars = string_chars(str);
    
    for (jsize i = fromIndex; i < string_length(str); i++) {
        if (chars[i] == ch) {
            return NATIVE_RETURN_INT((jint)i);
        }
    }
    
    return NATIVE_RETURN_INT(-1);
}

/* String.indexOf(String) - find first occurrence of substring */
static JavaValue native_string_indexOf_string(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    JavaString* sub = (JavaString*)args[1].ref;
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(-1);
    }
    
    /* indexOf(null) should throw NPE */
    if (!sub) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(-1);
    }
    
    if (string_length(sub) == 0) {
        return NATIVE_RETURN_INT(0);
    }
    
    if (string_length(sub) > string_length(str)) {
        return NATIVE_RETURN_INT(-1);
    }
    
    const jchar* str_chars = string_chars(str);
    const jchar* sub_chars = string_chars(sub);
    
    /* Simple substring search */
    for (jsize i = 0; i <= string_length(str) - string_length(sub); i++) {
        bool found = true;
        for (jsize j = 0; j < string_length(sub); j++) {
            if (str_chars[i + j] != sub_chars[j]) {
                found = false;
                break;
            }
        }
        if (found) {
            return NATIVE_RETURN_INT((jint)i);
        }
    }
    
    return NATIVE_RETURN_INT(-1);
}

/* String.indexOf(String, int) - find substring starting from index */
static JavaValue native_string_indexOf_string_from(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    JavaString* sub = (JavaString*)args[1].ref;
    jint fromIndex = args[2].i;
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(-1);
    }
    
    /* indexOf(null, from) should throw NPE */
    if (!sub) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(-1);
    }
    
    if (fromIndex < 0) fromIndex = 0;
    
    if (string_length(sub) == 0) {
        return NATIVE_RETURN_INT(fromIndex < string_length(str) ? fromIndex : string_length(str));
    }
    
    if (string_length(sub) > string_length(str)) {
        return NATIVE_RETURN_INT(-1);
    }
    
    const jchar* str_chars = string_chars(str);
    const jchar* sub_chars = string_chars(sub);
    
    for (jsize i = fromIndex; i <= string_length(str) - string_length(sub); i++) {
        bool found = true;
        for (jsize j = 0; j < string_length(sub); j++) {
            if (str_chars[i + j] != sub_chars[j]) {
                found = false;
                break;
            }
        }
        if (found) {
            return NATIVE_RETURN_INT((jint)i);
        }
    }
    
    return NATIVE_RETURN_INT(-1);
}

/* String.lastIndexOf(int) - find last occurrence of character */
static JavaValue native_string_lastIndexOf(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    jchar ch = (jchar)args[1].i;
    
    if (!str) {
        return NATIVE_RETURN_INT(-1);
    }
    
    const jchar* chars = string_chars(str);
    
    for (jsize i = string_length(str) - 1; i >= 0; i--) {
        if (chars[i] == ch) {
            return NATIVE_RETURN_INT((jint)i);
        }
    }
    
    return NATIVE_RETURN_INT(-1);
}

/* String.lastIndexOf(int, int) - find last occurrence of character from index */
static JavaValue native_string_lastIndexOf_from(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    jchar ch = (jchar)args[1].i;
    jint fromIndex = args[2].i;
    
    if (!str) {
        return NATIVE_RETURN_INT(-1);
    }
    
    jsize len = string_length(str);
    
    /* Clamp fromIndex to valid range */
    if (fromIndex < 0) {
        fromIndex = 0;
    } else if (fromIndex >= len) {
        fromIndex = len - 1;
    }
    
    const jchar* chars = string_chars(str);
    
    /* Search backwards from fromIndex */
    for (jint i = fromIndex; i >= 0; i--) {
        if (chars[i] == ch) {
            return NATIVE_RETURN_INT((jint)i);
        }
    }
    
    return NATIVE_RETURN_INT(-1);
}

/* String.substring(int) - extract substring from start index */
static JavaValue native_string_substring(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    jint start = args[1].i;
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    if (start < 0 || start > string_length(str)) {
        native_throw_aioobe(jvm, thread, start);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    const jchar* chars = string_chars(str);
    jsize len = string_length(str) - start;
    JavaString* result = jvm_new_string_utf16(jvm, chars + start, len);
    
    return NATIVE_RETURN_OBJECT(result);
}

/* String.substring(int, int) - extract substring between indices */
static JavaValue native_string_substring_range(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    jint start = args[1].i;
    jint end = args[2].i;
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    if (start < 0 || end > string_length(str) || start > end) {
        native_throw_aioobe(jvm, thread, start);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    const jchar* chars = string_chars(str);
    jsize len = end - start;
    JavaString* result = jvm_new_string_utf16(jvm, chars + start, len);
    
    return NATIVE_RETURN_OBJECT(result);
}

/* String.trim() - remove leading and trailing whitespace */
static JavaValue native_string_trim(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    
    if (!str) {
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    const jchar* chars = string_chars(str);
    jint start = 0;
    jint end = string_length(str);
    
    /* Find first non-whitespace */
    while (start < end && (chars[start] <= ' ')) {
        start++;
    }
    
    /* Find last non-whitespace */
    while (end > start && (chars[end - 1] <= ' ')) {
        end--;
    }
    
    if (start == 0 && end == string_length(str)) {
        return NATIVE_RETURN_OBJECT(str);  /* No change */
    }
    
    jsize len = end - start;
    JavaString* result = jvm_new_string_utf16(jvm, chars + start, len);
    
    return NATIVE_RETURN_OBJECT(result);
}

/* String.toLowerCase() - convert to lowercase */
static JavaValue native_string_toLowerCase(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    
    if (!str) {
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    const jchar* chars = string_chars(str);
    JavaString* result = jvm_new_string_utf16(jvm, chars, string_length(str));
    
    if (result) {
        jchar* result_chars = (jchar*)string_chars(result);
        for (jsize i = 0; i < string_length(str); i++) {
            jchar c = chars[i];
            if (c >= 'A' && c <= 'Z') {
                result_chars[i] = c + ('a' - 'A');
            } else {
                result_chars[i] = c;
            }
        }
    }
    
    return NATIVE_RETURN_OBJECT(result);
}

/* String.toUpperCase() - convert to uppercase */
static JavaValue native_string_toUpperCase(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    
    if (!str) {
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    const jchar* chars = string_chars(str);
    JavaString* result = jvm_new_string_utf16(jvm, chars, string_length(str));
    
    if (result) {
        jchar* result_chars = (jchar*)string_chars(result);
        for (jsize i = 0; i < string_length(str); i++) {
            jchar c = chars[i];
            if (c >= 'a' && c <= 'z') {
                result_chars[i] = c - ('a' - 'A');
            } else {
                result_chars[i] = c;
            }
        }
    }
    
    return NATIVE_RETURN_OBJECT(result);
}

/* String.compareTo(String) - compare strings lexicographically */
static JavaValue native_string_compareTo(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    JavaString* other = (JavaString*)args[1].ref;
    
    if (!str) {
        return NATIVE_RETURN_INT(other ? -1 : 0);
    }
    
    if (!other) {
        return NATIVE_RETURN_INT(1);
    }
    
    const jchar* str_chars = string_chars(str);
    const jchar* other_chars = string_chars(other);
    
    jsize min_len = string_length(str) < string_length(other) ? string_length(str) : string_length(other);
    
    for (jsize i = 0; i < min_len; i++) {
        if (str_chars[i] != other_chars[i]) {
            return NATIVE_RETURN_INT((jint)str_chars[i] - (jint)other_chars[i]);
        }
    }
    
    return NATIVE_RETURN_INT((jint)string_length(str) - (jint)string_length(other));
}

/* String.replace(char, char) - replace characters */
static JavaValue native_string_replace(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    jchar oldChar = (jchar)args[1].i;
    jchar newChar = (jchar)args[2].i;
    
    if (!str) {
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    const jchar* chars = string_chars(str);
    JavaString* result = jvm_new_string_utf16(jvm, chars, string_length(str));
    
    if (result) {
        jchar* result_chars = (jchar*)string_chars(result);
        for (jsize i = 0; i < string_length(str); i++) {
            if (chars[i] == oldChar) {
                result_chars[i] = newChar;
            } else {
                result_chars[i] = chars[i];
            }
        }
    }
    
    return NATIVE_RETURN_OBJECT(result);
}

/* String.equals(Object) - compare strings */
static JavaValue native_string_equals(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    JavaString* other = (JavaString*)args[1].ref;
    
    /* NPE if this is null (shouldn't happen in normal Java, but test expects it) */
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    
    if (str == other) {
        return NATIVE_RETURN_INT(1);
    }
    
    if (!other) {
        return NATIVE_RETURN_INT(0);
    }
    
    if (string_length(str) != string_length(other)) {
        return NATIVE_RETURN_INT(0);
    }
    
    const jchar* str_chars = string_chars(str);
    const jchar* other_chars = string_chars(other);
    
    for (jsize i = 0; i < string_length(str); i++) {
        if (str_chars[i] != other_chars[i]) {
            return NATIVE_RETURN_INT(0);
        }
    }
    
    return NATIVE_RETURN_INT(1);
}

/*
 * java.lang.String native methods - ADD toCharArray
 */

/* String.toCharArray() - converts string to char array */
static JavaValue native_string_toCharArray(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Get string characters */
    const jchar* chars = string_chars(str);
    jsize length = string_length(str);
    
    /* Create char array of same length */
    JavaArray* char_array = jvm_new_array(jvm, T_CHAR, length, NULL);
    if (!char_array) {
        native_throw_oome(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Copy characters to array */
    if (length > 0) {
        jchar* array_data_ptr = (jchar*)array_data(char_array);
        memcpy(array_data_ptr, chars, length * sizeof(jchar));
    }
    
    
    return NATIVE_RETURN_OBJECT(char_array);
}

/* String.getBytes() - converts string to byte array using platform encoding */
static JavaValue native_string_getBytes(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Get UTF-8 representation (simplified - in real Java it uses platform encoding) */
    const char* utf8 = string_utf8(jvm, str);
    if (!utf8) {
        return NATIVE_RETURN_NULL();
    }
    
    jsize length = (jsize)strlen(utf8);
    
    /* Create byte array */
    JavaArray* byte_array = jvm_new_array(jvm, T_BYTE, length, NULL);
    if (!byte_array) {
        native_throw_oome(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Copy bytes */
    if (length > 0) {
        uint8_t* array_data_ptr = (uint8_t*)array_data(byte_array);
        memcpy(array_data_ptr, utf8, length);
    }
    
    
    return NATIVE_RETURN_OBJECT(byte_array);
}

/* String.getBytes(int, int, byte[], int) - old method (deprecated but used in some apps) */
static JavaValue native_string_getBytes_old(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    jint srcBegin = args[1].i;
    jint srcEnd = args[2].i;
    JavaArray* dst = (JavaArray*)args[3].ref;
    jint dstBegin = args[4].i;
    
    if (!str || !dst) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    /* Bounds checking */
    if (srcBegin < 0 || srcBegin > srcEnd || srcEnd > string_length(str)) {
        native_throw_aioobe(jvm, thread, srcBegin);
        return NATIVE_RETURN_VOID();
    }
    
    if (dstBegin < 0 || dstBegin + (srcEnd - srcBegin) > (jint)dst->length) {
        native_throw_aioobe(jvm, thread, dstBegin);
        return NATIVE_RETURN_VOID();
    }
    
    /* Get string characters */
    const jchar* chars = string_chars(str);
    
    /* Convert to bytes (ASCII only - high byte discarded) */
    uint8_t* dst_data = (uint8_t*)array_data(dst);
    for (int i = srcBegin; i < srcEnd; i++) {
        dst_data[dstBegin + (i - srcBegin)] = (uint8_t)(chars[i] & 0xFF);
    }
    
    return NATIVE_RETURN_VOID();
}

/* String.getChars(int srcBegin, int srcEnd, char[] dst, int dstBegin) */
static JavaValue native_string_getChars(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    jint srcBegin = args[1].i;
    jint srcEnd = args[2].i;
    JavaArray* dst = (JavaArray*)args[3].ref;
    jint dstBegin = args[4].i;
    
    if (!str || !dst) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    int len = string_length(str);
    
    /* Bounds checking */
    if (srcBegin < 0 || srcEnd < 0 || srcBegin > srcEnd || srcEnd > len) {
        native_throw_aioobe(jvm, thread, srcBegin);
        return NATIVE_RETURN_VOID();
    }
    
    if (dstBegin < 0 || dstBegin + (srcEnd - srcBegin) > (jint)dst->length) {
        native_throw_aioobe(jvm, thread, dstBegin);
        return NATIVE_RETURN_VOID();
    }
    
    /* Get string characters and copy into char[] */
    const jchar* chars = string_chars(str);
    jchar* dst_data = (jchar*)array_data(dst);
    int count = srcEnd - srcBegin;
    for (int i = 0; i < count; i++) {
        dst_data[dstBegin + i] = chars[srcBegin + i];
    }
    
    return NATIVE_RETURN_VOID();
}

/* String.getBytes(String charsetName) */
static JavaValue native_string_getBytes_charset(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    JavaString* charset_str = (JavaString*)args[1].ref;
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Get charset name (ignore for now, just use UTF-8) */
    const char* charset = charset_str ? string_utf8(jvm, charset_str) : "UTF-8";
    (void)charset; /* Ignore in simplified implementation */
    
    /* Get UTF-8 representation */
    const char* utf8 = string_utf8(jvm, str);
    if (!utf8) {
        return NATIVE_RETURN_NULL();
    }
    
    jsize length = (jsize)strlen(utf8);
    
    /* Create byte array */
    JavaArray* byte_array = jvm_new_array(jvm, T_BYTE, length, NULL);
    if (!byte_array) {
        native_throw_oome(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Copy bytes */
    if (length > 0) {
        uint8_t* array_data_ptr = (uint8_t*)array_data(byte_array);
        memcpy(array_data_ptr, utf8, length);
    }
    
    return NATIVE_RETURN_OBJECT(byte_array);
}

/* String.valueOf(C) - convert char to String */
static JavaValue native_string_valueOf_char(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jchar c = (jchar)args[0].i;
    
    /* Create a new String with one character */
    char utf8[8];
    int len = 0;
    
    /* Convert jchar (UTF-16) to UTF-8 */
    if (c < 0x80) {
        utf8[len++] = (char)c;
    } else if (c < 0x800) {
        utf8[len++] = (char)(0xC0 | (c >> 6));
        utf8[len++] = (char)(0x80 | (c & 0x3F));
    } else {
        utf8[len++] = (char)(0xE0 | (c >> 12));
        utf8[len++] = (char)(0x80 | ((c >> 6) & 0x3F));
        utf8[len++] = (char)(0x80 | (c & 0x3F));
    }
    utf8[len] = '\0';
    
    JavaString* str = jvm_new_string(jvm, utf8);
    return NATIVE_RETURN_OBJECT(str);
}

/* String.valueOf(I) - convert int to String */
static JavaValue native_string_valueOf_int(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jint value = args[0].i;
    
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%d", value);
    
    JavaString* str = jvm_new_string(jvm, buffer);
    return NATIVE_RETURN_OBJECT(str);
}

/* String.valueOf(J) - convert long to String */
static JavaValue native_string_valueOf_long(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jlong value = args[0].j;
    
    char buffer[24];
    snprintf(buffer, sizeof(buffer), "%lld", (long long)value);
    
    JavaString* str = jvm_new_string(jvm, buffer);
    return NATIVE_RETURN_OBJECT(str);
}

/* String.valueOf(Z) - convert boolean to String */
static JavaValue native_string_valueOf_boolean(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jboolean value = args[0].i;
    
    JavaString* str = jvm_new_string(jvm, value ? "true" : "false");
    return NATIVE_RETURN_OBJECT(str);
}

/* String.valueOf(F) - convert float to String */
static JavaValue native_string_valueOf_float(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jfloat value = args[0].f;
    
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%g", (double)value);
    
    JavaString* str = jvm_new_string(jvm, buffer);
    return NATIVE_RETURN_OBJECT(str);
}

/* String.valueOf(D) - convert double to String */
static JavaValue native_string_valueOf_double(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jdouble value = args[0].d;
    
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%g", value);
    
    JavaString* str = jvm_new_string(jvm, buffer);
    return NATIVE_RETURN_OBJECT(str);
}

/* String.valueOf(Object) - convert object to String */
static JavaValue native_string_valueOf_object(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = args[0].ref;
    
    if (!obj) {
        JavaString* str = jvm_new_string(jvm, "null");
        return NATIVE_RETURN_OBJECT(str);
    }
    
    /* TODO: Call obj.toString() */
    JavaString* str = jvm_new_string(jvm, "Object");
    return NATIVE_RETURN_OBJECT(str);
}

/* String.valueOf([C) - convert char array to String (STATIC METHOD) */
static JavaValue native_string_valueOf_chararray(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    /* STATIC METHOD - args[0] is the char array directly (no 'this') */
    JavaArray* char_array = (JavaArray*)args[0].ref;
    
    if (!char_array) {
        JavaString* str = jvm_new_string(jvm, "null");
        return NATIVE_RETURN_OBJECT(str);
    }
    
    /* Create String from char array */
    jchar* chars = (jchar*)array_data(char_array);
    jsize length = char_array->length;
    
    JavaString* result = jvm_new_string_utf16(jvm, chars, length);
    return NATIVE_RETURN_OBJECT(result);
}

/* String.concat(String) - concatenate two strings */
static JavaValue native_string_concat(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    JavaString* other = (JavaString*)args[1].ref;
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* CRITICAL: If other is null, throw NullPointerException per Java spec */
    if (!other) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Get this string's data */
    jsize this_len = string_length(str);
    const jchar* this_chars = string_chars(str);
    
    /* Get other string's data */
    jsize other_len = string_length(other);
    const jchar* other_chars = string_chars(other);
    
    /* Create result buffer */
    jsize total_len = this_len + other_len;
    jchar* result_chars = (jchar*)malloc((total_len > 0 ? total_len : 1) * sizeof(jchar));
    if (!result_chars) {
        native_throw_oome(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Copy this string */
    if (this_len > 0 && this_chars) {
        memcpy(result_chars, this_chars, this_len * sizeof(jchar));
    }
    
    /* Copy other string */
    if (other_len > 0 && other_chars) {
        memcpy(result_chars + this_len, other_chars, other_len * sizeof(jchar));
    }
    
    JavaString* result = jvm_new_string_utf16(jvm, result_chars, total_len);
    free(result_chars);
    
    return NATIVE_RETURN_OBJECT(result);
}

/* Helper function to convert a character to lowercase for case-insensitive comparison */
static jchar to_lower_for_compare(jchar c) {
    /* ASCII uppercase */
    if (c >= 'A' && c <= 'Z') {
        return c + ('a' - 'A');
    }
    /* Latin-1 Supplement uppercase letters (U+00C0 - U+00DE, excluding U+00D7) */
    if (c >= 0x00C0 && c <= 0x00DE && c != 0x00D7) {
        return c + 0x0020;
    }
    /* Greek uppercase (U+0391 - U+03A9, excluding U+03A2) */
    if (c >= 0x0391 && c <= 0x03A9 && c != 0x03A2) {
        return c + 0x0020;
    }
    /* Cyrillic uppercase (U+0410 - U+042F) */
    if (c >= 0x0410 && c <= 0x042F) {
        return c + 0x0020;
    }
    /* Cyrillic uppercase extended (U+0400 - Ux040F) */
    if (c >= 0x0400 && c <= 0x040F) {
        return c + 0x0050;
    }
    return c;
}

/* String.equalsIgnoreCase(String) - compare strings ignoring case */
static JavaValue native_string_equalsIgnoreCase(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    JavaString* other = (JavaString*)args[1].ref;
    
    /* Same reference? */
    if (str == other) {
        return NATIVE_RETURN_INT(1);
    }
    
    /* If either is null, they're not equal (but both null would be caught above) */
    if (!str || !other) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Different lengths? */
    if (string_length(str) != string_length(other)) {
        return NATIVE_RETURN_INT(0);
    }
    
    const jchar* str_chars = string_chars(str);
    const jchar* other_chars = string_chars(other);
    
    /* Compare character by character, ignoring case */
    for (jsize i = 0; i < string_length(str); i++) {
        jchar c1 = to_lower_for_compare(str_chars[i]);
        jchar c2 = to_lower_for_compare(other_chars[i]);
        
        if (c1 != c2) {
            return NATIVE_RETURN_INT(0);
        }
    }
    
    return NATIVE_RETURN_INT(1);
}

/* String.<init>() - default constructor creates empty string */
static JavaValue native_string_init(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* str = (JavaObject*)args[0].ref;
    
    /* Initialize field slots on first use */
    string_get_class(jvm);
    
    if (str) {
        /* Initialize as empty string using proper field accessors */
        string_set_value(str, NULL);
        string_set_offset(str, 0);
        string_set_count(str, 0);
        string_set_hash(str, 0);
    }
    
    return NATIVE_RETURN_VOID();
}

/* String.<init>(char[]) - construct from char array */
static JavaValue native_string_init_chars(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* str = (JavaObject*)args[0].ref;
    JavaArray* char_array = (JavaArray*)args[1].ref;
    
    /* Initialize field slots on first use */
    string_get_class(jvm);
    
    if (!str) {
        return NATIVE_RETURN_VOID();
    }
    
    if (char_array && char_array->length > 0) {
        /* Create a copy of the char array */
        JavaArray* new_array = jvm_new_array(jvm, T_CHAR, char_array->length, NULL);
        if (new_array) {
            jchar* src = (jchar*)array_data(char_array);
            jchar* dst = (jchar*)array_data(new_array);
            memcpy(dst, src, char_array->length * sizeof(jchar));
            
            string_set_value(str, new_array);
            string_set_offset(str, 0);
            string_set_count(str, char_array->length);
            string_set_hash(str, 0);
            
            DEBUG_LOG("String.<init>(char[]): created string with length=%d, value=%p",
                      char_array->length, new_array);
        }
    } else {
        /* Empty or null char array */
        string_set_value(str, NULL);
        string_set_offset(str, 0);
        string_set_count(str, 0);
        string_set_hash(str, 0);
        DEBUG_LOG("String.<init>(char[]): created empty string");
    }
    
    return NATIVE_RETURN_VOID();
}

/* String.<init>(byte[]) - construct from byte array */
static JavaValue native_string_init_bytes(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* str = (JavaObject*)args[0].ref;
    JavaArray* byte_array = (JavaArray*)args[1].ref;
    
    /* Initialize field slots on first use */
    string_get_class(jvm);
    
    if (!str) {
        return NATIVE_RETURN_VOID();
    }
    
    if (byte_array && byte_array->length > 0) {
        /* Convert bytes to chars (assuming ASCII/ISO-8859-1) */
        JavaArray* new_array = jvm_new_array(jvm, T_CHAR, byte_array->length, NULL);
        if (new_array) {
            uint8_t* src = (uint8_t*)array_data(byte_array);
            jchar* dst = (jchar*)array_data(new_array);
            for (jsize i = 0; i < byte_array->length; i++) {
                dst[i] = (jchar)(src[i] & 0xFF);
            }
            
            string_set_value(str, new_array);
            string_set_offset(str, 0);
            string_set_count(str, byte_array->length);
            string_set_hash(str, 0);
        }
    } else {
        string_set_value(str, NULL);
        string_set_offset(str, 0);
        string_set_count(str, 0);
        string_set_hash(str, 0);
    }
    
    return NATIVE_RETURN_VOID();
}

/* String.<init>(String) - copy constructor */
static JavaValue native_string_init_string(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* str = (JavaObject*)args[0].ref;
    JavaString* other = (JavaString*)args[1].ref;
    
    /* Initialize field slots on first use */
    string_get_class(jvm);
    
    if (!str) {
        return NATIVE_RETURN_VOID();
    }
    
    if (other) {
        jsize other_len = string_length(other);
        const jchar* other_chars = string_chars(other);
        
        if (other_len > 0 && other_chars) {
            /* Create a copy of the char array */
            JavaArray* new_array = jvm_new_array(jvm, T_CHAR, other_len, NULL);
            if (new_array) {
                jchar* dst = (jchar*)array_data(new_array);
                memcpy(dst, other_chars, other_len * sizeof(jchar));
                
                string_set_value(str, new_array);
                string_set_offset(str, 0);
                string_set_count(str, other_len);
                string_set_hash(str, 0);
            }
        } else {
            string_set_value(str, NULL);
            string_set_offset(str, 0);
            string_set_count(str, 0);
            string_set_hash(str, 0);
        }
    } else {
        /* null string - create empty */
        string_set_value(str, NULL);
        string_set_offset(str, 0);
        string_set_count(str, 0);
        string_set_hash(str, 0);
    }
    
    return NATIVE_RETURN_VOID();
}

/* String.<init>(char[], int, int) - construct from char array with offset */
static JavaValue native_string_init_chars_offset(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* str = (JavaObject*)args[0].ref;
    JavaArray* char_array = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint count = args[3].i;
    
    /* Initialize field slots on first use */
    string_get_class(jvm);
    
    if (!str) {
        return NATIVE_RETURN_VOID();
    }
    
    if (char_array && offset >= 0 && count >= 0 && offset + count <= (jint)char_array->length) {
        if (count > 0) {
            /* Create a new char array with the substring */
            JavaArray* new_array = jvm_new_array(jvm, T_CHAR, count, NULL);
            if (new_array) {
                jchar* src = (jchar*)array_data(char_array) + offset;
                jchar* dst = (jchar*)array_data(new_array);
                memcpy(dst, src, count * sizeof(jchar));
                
                string_set_value(str, new_array);
                string_set_offset(str, 0);
                string_set_count(str, count);
                string_set_hash(str, 0);
            }
        } else {
            /* count == 0, empty string */
            string_set_value(str, NULL);
            string_set_offset(str, 0);
            string_set_count(str, 0);
            string_set_hash(str, 0);
        }
    } else {
        string_set_value(str, NULL);
        string_set_offset(str, 0);
        string_set_count(str, 0);
        string_set_hash(str, 0);
    }
    
    return NATIVE_RETURN_VOID();
}

/* String.<init>(byte[], int, int) - construct from byte array with offset */
static JavaValue native_string_init_bytes_offset(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* str = (JavaObject*)args[0].ref;
    JavaArray* byte_array = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint count = args[3].i;
    
    /* Initialize field slots on first use */
    string_get_class(jvm);
    
    if (!str) {
        return NATIVE_RETURN_VOID();
    }
    
    if (byte_array && offset >= 0 && count >= 0 && offset + count <= (jint)byte_array->length) {
        if (count > 0) {
            /* Convert bytes to chars */
            JavaArray* new_array = jvm_new_array(jvm, T_CHAR, count, NULL);
            if (new_array) {
                uint8_t* src = (uint8_t*)array_data(byte_array) + offset;
                jchar* dst = (jchar*)array_data(new_array);
                for (jint i = 0; i < count; i++) {
                    dst[i] = (jchar)(src[i] & 0xFF);
                }
                
                string_set_value(str, new_array);
                string_set_offset(str, 0);
                string_set_count(str, count);
                string_set_hash(str, 0);
            }
        } else {
            string_set_value(str, NULL);
            string_set_offset(str, 0);
            string_set_count(str, 0);
            string_set_hash(str, 0);
        }
    } else {
        string_set_value(str, NULL);
        string_set_offset(str, 0);
        string_set_count(str, 0);
        string_set_hash(str, 0);
    }
    
    return NATIVE_RETURN_VOID();
}

/* String.<init>(byte[], String charsetName) - construct from byte array with charset */
static JavaValue native_string_init_bytes_charset(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* str = (JavaObject*)args[0].ref;
    JavaArray* byte_array = (JavaArray*)args[1].ref;
    /* args[2].ref = charset name string — ignored, we treat everything as ISO-8859-1/UTF-8 */

    /* Initialize field slots on first use */
    string_get_class(jvm);

    if (!str) {
        return NATIVE_RETURN_VOID();
    }

    if (byte_array && byte_array->length > 0) {
        JavaArray* new_array = jvm_new_array(jvm, T_CHAR, byte_array->length, NULL);
        if (new_array) {
            uint8_t* src = (uint8_t*)array_data(byte_array);
            jchar* dst = (jchar*)array_data(new_array);
            for (jsize i = 0; i < byte_array->length; i++) {
                dst[i] = (jchar)(src[i] & 0xFF);
            }

            string_set_value(str, new_array);
            string_set_offset(str, 0);
            string_set_count(str, byte_array->length);
            string_set_hash(str, 0);
        }
    } else {
        string_set_value(str, NULL);
        string_set_offset(str, 0);
        string_set_count(str, 0);
        string_set_hash(str, 0);
    }

    return NATIVE_RETURN_VOID();
}

/* String.<init>(byte[], int, int, String charsetName) - bytes with offset+count+charset */
static JavaValue native_string_init_bytes_offset_charset(JVM* jvm, JavaThread* thread,
                                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* str = (JavaObject*)args[0].ref;
    JavaArray* byte_array = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint count = args[3].i;
    /* args[4].ref = charset name string — ignored */

    /* Initialize field slots on first use */
    string_get_class(jvm);

    if (!str) {
        return NATIVE_RETURN_VOID();
    }

    if (byte_array && offset >= 0 && count >= 0 && offset + count <= (jint)byte_array->length) {
        if (count > 0) {
            JavaArray* new_array = jvm_new_array(jvm, T_CHAR, count, NULL);
            if (new_array) {
                uint8_t* src = (uint8_t*)array_data(byte_array) + offset;
                jchar* dst = (jchar*)array_data(new_array);
                for (jint i = 0; i < count; i++) {
                    dst[i] = (jchar)(src[i] & 0xFF);
                }

                string_set_value(str, new_array);
                string_set_offset(str, 0);
                string_set_count(str, count);
                string_set_hash(str, 0);
            }
        } else {
            string_set_value(str, NULL);
            string_set_offset(str, 0);
            string_set_count(str, 0);
            string_set_hash(str, 0);
        }
    } else {
        string_set_value(str, NULL);
        string_set_offset(str, 0);
        string_set_count(str, 0);
        string_set_hash(str, 0);
    }

    return NATIVE_RETURN_VOID();
}

void init_java_lang_string(JVM* jvm) {
    NativeMethodEntry methods[] = {
        /* Constructors */
        {"java/lang/String", "<init>", "()V", native_string_init},
        {"java/lang/String", "<init>", "([C)V", native_string_init_chars},
        {"java/lang/String", "<init>", "([B)V", native_string_init_bytes},
        {"java/lang/String", "<init>", "([BLjava/lang/String;)V", native_string_init_bytes_charset},
        {"java/lang/String", "<init>", "(Ljava/lang/String;)V", native_string_init_string},
        {"java/lang/String", "<init>", "([CII)V", native_string_init_chars_offset},
        {"java/lang/String", "<init>", "([BII)V", native_string_init_bytes_offset},
        {"java/lang/String", "<init>", "([BIILjava/lang/String;)V", native_string_init_bytes_offset_charset},
        
        /* Basic methods */
        {"java/lang/String", "length", "()I", native_string_length},
        {"java/lang/String", "charAt", "(I)C", native_string_charAt},
        {"java/lang/String", "isEmpty", "()Z", native_string_isEmpty},
        
        /* Comparison methods */
        {"java/lang/String", "equals", "(Ljava/lang/Object;)Z", native_string_equals},
        {"java/lang/String", "hashCode", "()I", native_string_hashCode},
        {"java/lang/String", "compareTo", "(Ljava/lang/String;)I", native_string_compareTo},
        {"java/lang/String", "startsWith", "(Ljava/lang/String;)Z", native_string_startsWith},
        {"java/lang/String", "startsWith", "(Ljava/lang/String;I)Z", native_string_startsWith_offset},
        {"java/lang/String", "endsWith", "(Ljava/lang/String;)Z", native_string_endsWith},
        {"java/lang/String", "equalsIgnoreCase", "(Ljava/lang/String;)Z", native_string_equalsIgnoreCase},
        {"java/lang/String", "regionMatches", "(ZILjava/lang/String;II)Z", native_string_regionMatches},
        
        /* Search methods */
        {"java/lang/String", "indexOf", "(I)I", native_string_indexOf},
        {"java/lang/String", "indexOf", "(II)I", native_string_indexOf_from},
        {"java/lang/String", "indexOf", "(Ljava/lang/String;)I", native_string_indexOf_string},
        {"java/lang/String", "indexOf", "(Ljava/lang/String;I)I", native_string_indexOf_string_from},
        {"java/lang/String", "lastIndexOf", "(I)I", native_string_lastIndexOf},
        {"java/lang/String", "lastIndexOf", "(II)I", native_string_lastIndexOf_from},
        
        /* Substring methods */
        {"java/lang/String", "substring", "(I)Ljava/lang/String;", native_string_substring},
        {"java/lang/String", "substring", "(II)Ljava/lang/String;", native_string_substring_range},
        
        /* Conversion methods */
        {"java/lang/String", "toCharArray", "()[C", native_string_toCharArray},
        {"java/lang/String", "getBytes", "()[B", native_string_getBytes},
        {"java/lang/String", "getBytes", "(II[BI)V", native_string_getBytes_old},
        {"java/lang/String", "getChars", "(II[CI)V", native_string_getChars},
        {"java/lang/String", "trim", "()Ljava/lang/String;", native_string_trim},
        {"java/lang/String", "toLowerCase", "()Ljava/lang/String;", native_string_toLowerCase},
        {"java/lang/String", "toUpperCase", "()Ljava/lang/String;", native_string_toUpperCase},
        {"java/lang/String", "replace", "(CC)Ljava/lang/String;", native_string_replace},
        {"java/lang/String", "intern", "()Ljava/lang/String;", native_string_intern},
        {"java/lang/String", "toString", "()Ljava/lang/String;", native_string_toString},
        
        /* Concatenation */
        {"java/lang/String", "concat", "(Ljava/lang/String;)Ljava/lang/String;", native_string_concat},
        
        /* String.valueOf static methods */
        {"java/lang/String", "valueOf", "(C)Ljava/lang/String;", native_string_valueOf_char},
        {"java/lang/String", "valueOf", "([C)Ljava/lang/String;", native_string_valueOf_chararray},
        {"java/lang/String", "valueOf", "(I)Ljava/lang/String;", native_string_valueOf_int},
        {"java/lang/String", "valueOf", "(J)Ljava/lang/String;", native_string_valueOf_long},
        {"java/lang/String", "valueOf", "(Z)Ljava/lang/String;", native_string_valueOf_boolean},
        {"java/lang/String", "valueOf", "(F)Ljava/lang/String;", native_string_valueOf_float},
        {"java/lang/String", "valueOf", "(D)Ljava/lang/String;", native_string_valueOf_double},
        {"java/lang/String", "valueOf", "(Ljava/lang/Object;)Ljava/lang/String;", native_string_valueOf_object},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered java/lang/String native methods (%zu total)",
            sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.lang.Integer native methods
 */

static JavaValue native_integer_valueOf(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jint value = args[0].i;
    
    JavaClass* int_class = jvm_load_class(jvm, "java/lang/Integer");
    if (!int_class) {
        return NATIVE_RETURN_NULL();
    }
    
    JavaObject* int_obj = jvm_new_object(jvm, int_class);
    if (!int_obj) {
        return NATIVE_RETURN_NULL();
    }
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue val = { .i = value };
    native_set_field_value(int_obj, "value", val);
    
    return NATIVE_RETURN_OBJECT(int_obj);
}

static JavaValue native_integer_valueOf_string(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    const char* utf8 = string_utf8(jvm, str);
    if (!utf8) {
        return NATIVE_RETURN_NULL();
    }
    
    /* Parse integer */
    jint value = 0;
    int sign = 1;
    const char* p = utf8;
    
    if (*p == '-') {
        sign = -1;
        p++;
    } else if (*p == '+') {
        p++;
    }
    
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (*p - '0');
        p++;
    }
    
    value *= sign;
    
    JavaClass* int_class = jvm_load_class(jvm, "java/lang/Integer");
    if (!int_class) {
        return NATIVE_RETURN_NULL();
    }
    
    JavaObject* int_obj = jvm_new_object(jvm, int_class);
    if (!int_obj) {
        return NATIVE_RETURN_NULL();
    }
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue val = { .i = value };
    native_set_field_value(int_obj, "value", val);
    
    return NATIVE_RETURN_OBJECT(int_obj);
}

static JavaValue native_integer_parseInt(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    int radix = 10;
    
    /* Handle parseInt(String, int) variant - args[1] is the radix */
    if (arg_count >= 2) {
        radix = args[1].i;
    }
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    
    const char* utf8 = string_utf8(jvm, str);
    if (!utf8) {
        jvm_throw_by_name(jvm, "java/lang/NumberFormatException", "null string");
        return NATIVE_RETURN_INT(0);
    }
    
    /* Skip whitespace */
    while (*utf8 == ' ' || *utf8 == '\t' || *utf8 == '\n' || *utf8 == '\r') utf8++;
    
    /* Parse with radix support */
    jint value = 0;
    int sign = 1;
    const char* p = utf8;
    int has_digits = 0;
    
    if (*p == '-') {
        sign = -1;
        p++;
    } else if (*p == '+') {
        p++;
    }
    
    /* Check for empty string or sign only */
    if (*p == '\0') {
        jvm_throw_by_name(jvm, "java/lang/NumberFormatException", utf8);
        return NATIVE_RETURN_INT(0);
    }
    
    if (radix == 16 && *p == '0' && (*(p + 1) == 'x' || *(p + 1) == 'X')) {
        p += 2;  /* Skip 0x prefix */
    }
    
    while (*p != '\0') {
        int digit;
        if (*p >= '0' && *p <= '9') {
            digit = *p - '0';
        } else if (*p >= 'a' && *p <= 'f') {
            digit = *p - 'a' + 10;
        } else if (*p >= 'A' && *p <= 'F') {
            digit = *p - 'A' + 10;
        } else {
            break;  /* Invalid character for number */
        }
        
        if (digit >= radix) {
            jvm_throw_by_name(jvm, "java/lang/NumberFormatException", utf8);
            return NATIVE_RETURN_INT(0);
        }
        
        has_digits = 1;
        value = value * radix + digit;
        p++;
    }
    
    /* Check for trailing invalid characters or no digits */
    if (*p != '\0') {
        /* Allow trailing whitespace */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p != '\0') {
            jvm_throw_by_name(jvm, "java/lang/NumberFormatException", utf8);
            return NATIVE_RETURN_INT(0);
        }
    }
    
    if (!has_digits) {
        jvm_throw_by_name(jvm, "java/lang/NumberFormatException", utf8);
        return NATIVE_RETURN_INT(0);
    }
    
    return NATIVE_RETURN_INT(value * sign);
}

static JavaValue native_integer_toString(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jint value = args[0].i;
    
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%d", value);
    
    JavaString* str = jvm_new_string(jvm, buffer);
    return NATIVE_RETURN_OBJECT(str);
}

static JavaValue native_integer_intValue(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* int_obj = (JavaObject*)args[0].ref;
    
    if (!int_obj) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    return NATIVE_RETURN_INT(native_get_field_value(int_obj, "value").i);
}

/* Integer.equals(Object) - compares int values */
static JavaValue native_integer_equals(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* int_obj = (JavaObject*)args[0].ref;
    JavaObject* other = (JavaObject*)args[1].ref;
    
    if (int_obj == other) return NATIVE_RETURN_INT(1);
    if (!int_obj || !other) return NATIVE_RETURN_INT(0);
    
    /* Check if other is an Integer instance */
    JavaClass* other_class = other->header.clazz;
    if (!other_class) return NATIVE_RETURN_INT(0);
    
    /* Walk up class hierarchy to check if it's Integer */
    int is_integer = 0;
    JavaClass* cls = other_class;
    while (cls) {
        if (cls->class_name && strcmp(cls->class_name, "java/lang/Integer") == 0) {
            is_integer = 1;
            break;
        }
        cls = cls->super_class;
    }
    
    if (!is_integer) return NATIVE_RETURN_INT(0);
    
    /* Compare int values */
    int this_val = native_get_field_value(int_obj, "value").i;
    int other_val = native_get_field_value(other, "value").i;
    return NATIVE_RETURN_INT(this_val == other_val ? 1 : 0);
}

/* Integer(int) constructor */
static JavaValue native_integer_init(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* int_obj = (JavaObject*)args[0].ref;
    jint value = args[1].i;
    
    if (!int_obj) {
        return NATIVE_RETURN_VOID();
    }
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue val = { .i = value };
    native_set_field_value(int_obj, "value", val);
    
    return NATIVE_RETURN_VOID();
}

/* Integer.toHexString(int) - convert int to hexadecimal string */
static JavaValue native_integer_toHexString(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jint value = args[0].i;
    
    char buffer[12];  /* Enough for 32-bit hex: 8 chars + "0x" + null */
    snprintf(buffer, sizeof(buffer), "%x", (unsigned int)value);
    
    JavaString* str = jvm_new_string(jvm, buffer);
    return NATIVE_RETURN_OBJECT(str);
}

/* Integer.toHexString(long) - convert long to hexadecimal string (for Long) */
static JavaValue native_integer_toHexString_long(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jlong value = args[0].j;
    
    char buffer[20];  /* Enough for 64-bit hex: 16 chars + null */
    snprintf(buffer, sizeof(buffer), "%llx", (unsigned long long)value);
    
    JavaString* str = jvm_new_string(jvm, buffer);
    return NATIVE_RETURN_OBJECT(str);
}

/* Integer.toOctalString(int) - convert int to octal string */
static JavaValue native_integer_toOctalString(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jint value = args[0].i;
    
    char buffer[16];  /* Enough for 32-bit octal */
    snprintf(buffer, sizeof(buffer), "%o", (unsigned int)value);
    
    JavaString* str = jvm_new_string(jvm, buffer);
    return NATIVE_RETURN_OBJECT(str);
}

/* Integer.toBinaryString(int) - convert int to binary string */
static JavaValue native_integer_toBinaryString(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jint value = args[0].i;
    
    /* Convert to binary string manually */
    char buffer[33];  /* 32 bits + null */
    buffer[32] = '\0';
    
    unsigned int uval = (unsigned int)value;
    for (int i = 31; i >= 0; i--) {
        buffer[i] = (uval & 1) ? '1' : '0';
        uval >>= 1;
    }
    
    /* Skip leading zeros */
    char* start = buffer;
    while (*start == '0' && *(start + 1) != '\0') {
        start++;
    }
    
    JavaString* str = jvm_new_string(jvm, start);
    return NATIVE_RETURN_OBJECT(str);
}

void init_java_lang_integer(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/lang/Integer", "<init>", "(I)V", native_integer_init},
        {"java/lang/Integer", "valueOf", "(I)Ljava/lang/Integer;", native_integer_valueOf},
        {"java/lang/Integer", "valueOf", "(Ljava/lang/String;)Ljava/lang/Integer;", native_integer_valueOf_string},
        {"java/lang/Integer", "parseInt", "(Ljava/lang/String;)I", native_integer_parseInt},
        {"java/lang/Integer", "parseInt", "(Ljava/lang/String;I)I", native_integer_parseInt},
        {"java/lang/Integer", "toString", "(I)Ljava/lang/String;", native_integer_toString},
        {"java/lang/Integer", "toHexString", "(I)Ljava/lang/String;", native_integer_toHexString},
        {"java/lang/Integer", "toOctalString", "(I)Ljava/lang/String;", native_integer_toOctalString},
        {"java/lang/Integer", "toBinaryString", "(I)Ljava/lang/String;", native_integer_toBinaryString},
        {"java/lang/Integer", "intValue", "()I", native_integer_intValue},
        {"java/lang/Integer", "equals", "(Ljava/lang/Object;)Z", native_integer_equals},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.lang.Long native methods
 */

/* Long.longValue() - returns long value with null-safety */
static JavaValue native_long_longValue(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* long_obj = (JavaObject*)args[0].ref;
    
    /* NULL-SAFETY: Return 0 for null instead of NPE */
    if (!long_obj) {
        NATIVE_DEBUG("Long.longValue: null object, returning 0");
        return NATIVE_RETURN_LONG(0);
    }
    
    /* Get the value field - Long stores value as long (J type) */
    jlong val = native_get_field_value(long_obj, "value").j;
    return NATIVE_RETURN_LONG(val);
}

/* Long.valueOf(long) - creates Long object from long value */
static JavaValue native_long_valueOf(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jlong value = args[0].j;
    
    JavaClass* long_class = jvm_load_class(jvm, "java/lang/Long");
    if (!long_class) {
        return NATIVE_RETURN_NULL();
    }
    
    JavaObject* long_obj = jvm_new_object(jvm, long_class);
    if (!long_obj) {
        return NATIVE_RETURN_NULL();
    }
    
    /* Set the value field */
    JavaValue val = { .j = value };
    native_set_field_value(long_obj, "value", val);
    
    return NATIVE_RETURN_OBJECT(long_obj);
}

/* Long.<init>(long) - constructor */
static JavaValue native_long_init(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* long_obj = (JavaObject*)args[0].ref;
    jlong value = args[1].j;
    
    if (!long_obj) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Set the value field */
    JavaValue val = { .j = value };
    native_set_field_value(long_obj, "value", val);
    
    return NATIVE_RETURN_VOID();
}

/* Long.parseLong(String) - parse string to long */
static JavaValue native_long_parseLong(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_LONG(0);
    }
    
    const char* utf8 = string_utf8(jvm, str);
    if (!utf8) {
        return NATIVE_RETURN_LONG(0);
    }
    
    char* end;
    jlong value = strtoll(utf8, &end, 10);
    if (*end != '\0') {
        /* Invalid number format */
        jvm_throw_by_name(jvm, "java/lang/NumberFormatException", utf8);
        thread->pending_exception = jvm_exception_pending(jvm);
        return NATIVE_RETURN_LONG(0);
    }
    
    return NATIVE_RETURN_LONG(value);
}

/* Long.toString(long) - convert long to string */
static JavaValue native_long_toString(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jlong value = args[0].j;
    
    char buffer[24];  /* Enough for 64-bit signed: -9223372036854775808 */
    snprintf(buffer, sizeof(buffer), "%lld", (long long)value);
    
    JavaString* str = jvm_new_string(jvm, buffer);
    return NATIVE_RETURN_OBJECT(str);
}

void init_java_lang_long(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/lang/Long", "<init>", "(J)V", native_long_init},
        {"java/lang/Long", "valueOf", "(J)Ljava/lang/Long;", native_long_valueOf},
        {"java/lang/Long", "parseLong", "(Ljava/lang/String;)J", native_long_parseLong},
        {"java/lang/Long", "toString", "(J)Ljava/lang/String;", native_long_toString},
        {"java/lang/Long", "toHexString", "(J)Ljava/lang/String;", native_integer_toHexString_long},
        {"java/lang/Long", "longValue", "()J", native_long_longValue},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.lang.Boolean native methods
 */

static JavaValue native_boolean_valueOf(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jboolean value = args[0].i != 0;
    
    JavaClass* bool_class = jvm_load_class(jvm, "java/lang/Boolean");
    if (!bool_class) {
        return NATIVE_RETURN_NULL();
    }
    
    JavaObject* bool_obj = jvm_new_object(jvm, bool_class);
    if (!bool_obj) {
        return NATIVE_RETURN_NULL();
    }
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue val = { .i = value ? 1 : 0 };
    native_set_field_value(bool_obj, "value", val);
    
    return NATIVE_RETURN_OBJECT(bool_obj);
}

/* Boolean.<init>(boolean) - constructor */
static JavaValue native_boolean_init(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* bool_obj = (JavaObject*)args[0].ref;
    jboolean value = args[1].i != 0;
    
    if (!bool_obj) {
        return NATIVE_RETURN_VOID();
    }
    
    JavaValue val = { .i = value ? 1 : 0 };
    native_set_field_value(bool_obj, "value", val);
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_boolean_parseBoolean(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    
    if (!str) {
        return NATIVE_RETURN_INT(0);
    }
    
    const char* utf8 = string_utf8(jvm, str);
    if (utf8 && strcmp(utf8, "true") == 0) {
        return NATIVE_RETURN_INT(1);
    }
    
    return NATIVE_RETURN_INT(0);
}

/* Boolean.equals(Object) - compares boolean values */
static JavaValue native_boolean_equals(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* bool_obj = (JavaObject*)args[0].ref;
    JavaObject* other = (JavaObject*)args[1].ref;
    
    if (bool_obj == other) return NATIVE_RETURN_INT(1);
    if (!bool_obj || !other) return NATIVE_RETURN_INT(0);
    
    /* Check if other is a Boolean instance */
    JavaClass* other_class = other->header.clazz;
    if (!other_class) return NATIVE_RETURN_INT(0);
    
    int is_boolean = 0;
    JavaClass* cls = other_class;
    while (cls) {
        if (cls->class_name && strcmp(cls->class_name, "java/lang/Boolean") == 0) {
            is_boolean = 1;
            break;
        }
        cls = cls->super_class;
    }
    
    if (!is_boolean) return NATIVE_RETURN_INT(0);
    
    /* Compare boolean values */
    int this_val = native_get_field_value(bool_obj, "value").i;
    int other_val = native_get_field_value(other, "value").i;
    return NATIVE_RETURN_INT(this_val == other_val ? 1 : 0);
}

void init_java_lang_boolean(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/lang/Boolean", "<init>", "(Z)V", native_boolean_init},
        {"java/lang/Boolean", "valueOf", "(Z)Ljava/lang/Boolean;", native_boolean_valueOf},
        {"java/lang/Boolean", "parseBoolean", "(Ljava/lang/String;)Z", native_boolean_parseBoolean},
        {"java/lang/Boolean", "booleanValue", "()Z", native_integer_intValue},
        {"java/lang/Boolean", "equals", "(Ljava/lang/Object;)Z", native_boolean_equals},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    
    /* Initialize Boolean.TRUE and Boolean.FALSE static fields */
    JavaClass* bool_class = jvm_load_class(jvm, "java/lang/Boolean");
    if (bool_class) {
        /* Ensure static fields exist */
        if (bool_class->static_fields_count == 0) {
            bool_class->static_fields = (JavaStaticField*)calloc(2, sizeof(JavaStaticField));
            if (bool_class->static_fields) {
                bool_class->static_fields[0].name = strdup("TRUE");
                bool_class->static_fields[0].descriptor = strdup("Ljava/lang/Boolean;");
                memset(&bool_class->static_fields[0].value, 0, sizeof(JavaValue));
                bool_class->static_fields[1].name = strdup("FALSE");
                bool_class->static_fields[1].descriptor = strdup("Ljava/lang/Boolean;");
                memset(&bool_class->static_fields[1].value, 0, sizeof(JavaValue));
                bool_class->static_fields_count = 2;
                bool_class->static_fields_capacity = 2;
            }
        }
        
        /* Create Boolean.TRUE = new Boolean(true) */
        if (bool_class->static_fields_count >= 1 && !bool_class->static_fields[0].value.ref) {
            JavaObject* true_obj = jvm_new_object(jvm, bool_class);
            if (true_obj) {
                JavaValue val_true = { .i = 1 };
                native_set_field_value(true_obj, "value", val_true);
                bool_class->static_fields[0].value.ref = true_obj;
            }
        }
        
        /* Create Boolean.FALSE = new Boolean(false) */
        if (bool_class->static_fields_count >= 2 && !bool_class->static_fields[1].value.ref) {
            JavaObject* false_obj = jvm_new_object(jvm, bool_class);
            if (false_obj) {
                JavaValue val_false = { .i = 0 };
                native_set_field_value(false_obj, "value", val_false);
                bool_class->static_fields[1].value.ref = false_obj;
            }
        }
    }
}

/*
 * java.lang.System native methods
 */

/* System.getProperty(String key) - получить системное свойство */
static JavaValue native_system_getProperty(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)arg_count;
    
    /* STATIC METHOD - no 'this' argument!
     * args[0] = key string
     */
    JavaString* key_str = (JavaString*)args[0].ref;
    
    if (!key_str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    const char* key = string_utf8(jvm, key_str);
    if (!key) {
        return NATIVE_RETURN_NULL();
    }
    
    NATIVE_DEBUG("getProperty: '%s'", key);
    
    /* DRM BYPASS: Check if property should be hidden (hideEmulation mode) */
    extern bool drm_should_hide_property(const char* key);
    if (drm_should_hide_property(key)) {
        NATIVE_DEBUG("getProperty: '%s' -> NULL (hidden by DRM bypass)", key);
        return NATIVE_RETURN_NULL();
    }
    
    /* DRM BYPASS: Check for spoofed properties */
    extern const char* drm_get_spoofed_property(const char* key);
    const char* spoofed = drm_get_spoofed_property(key);
    if (spoofed) {
        NATIVE_DEBUG("getProperty: '%s' -> '%s' (spoofed)", key, spoofed);
        return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, spoofed));
    }
    
    /* Стандартные J2ME системные свойства */
    const char* value = NULL;
    
    if (strcmp(key, "microedition.platform") == 0) {
        value = "NokiaN73-1";  /* Default, may be overridden by drm_get_spoofed_property */
    } else if (strcmp(key, "microedition.encoding") == 0) {
        value = "UTF-8";
    } else if (strcmp(key, "microedition.configuration") == 0) {
        value = "CLDC-1.1";
    } else if (strcmp(key, "microedition.profiles") == 0) {
        value = "MIDP-2.0";
    } else if (strcmp(key, "microedition.locale") == 0) {
        value = "en";
    } else if (strcmp(key, "supports.mixing") == 0) {
        value = "true";
    } else if (strcmp(key, "supports.audio.capture") == 0) {
        value = "false";
    } else if (strcmp(key, "supports.video.capture") == 0) {
        value = "false";
    } else if (strcmp(key, "supports.recording") == 0) {
        value = "false";
    } else if (strcmp(key, "microedition.hostname") == 0) {
        value = "localhost";
    } else if (strcmp(key, "microedition.commports") == 0) {
        value = "";
    }
    
    /* Nokia-specific properties for DRM bypass */
    else if (strcmp(key, "com.nokia.mid.imei") == 0 ||
             strcmp(key, "device.imei") == 0 ||
             strcmp(key, "phone.imei") == 0) {
        value = "000000000000000";  /* Spoofed IMEI */
    } else if (strcmp(key, "com.nokia.mid.imsi") == 0) {
        value = "000000000000000";  /* Spoofed IMSI */
    }
    
    /* Memory properties - spoof like KEmulator */
    else if (strcmp(key, "totalMemory") == 0 ||
             strcmp(key, "java.runtime.totalMemory") == 0) {
        /* Return fake memory value */
        return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, "1048576"));
    } else if (strcmp(key, "freeMemory") == 0 ||
               strcmp(key, "java.runtime.freeMemory") == 0) {
        return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, "524288"));
    }
    
    if (value) {
        NATIVE_DEBUG("getProperty: '%s' -> '%s'", key, value);
        return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, value));
    }
    
    /* Свойство не найдено - возвращаем null */
    NATIVE_DEBUG("getProperty: '%s' not found, returning null", key);
    return NATIVE_RETURN_NULL();
}

static JavaValue native_system_arraycopy(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)arg_count;
    /* Static method: args[0] is first parameter (src), not 'this' */
    JavaArray* src = (JavaArray*)args[0].ref;
    jint src_pos = args[1].i;
    JavaArray* dest = (JavaArray*)args[2].ref;
    jint dest_pos = args[3].i;
    jint length = args[4].i;
    
    if (!src || !dest) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    if (src_pos < 0 || dest_pos < 0 || length < 0 ||
        src_pos + length > src->length ||
        dest_pos + length > dest->length) {
        native_throw_aioobe(jvm, thread, -1);
        return NATIVE_RETURN_VOID();
    }
    
    /* Copy elements */
    size_t elem_size = 4;  /* Default to int */
    switch (src->element_type) {
        case T_BOOLEAN:
        case T_BYTE:    elem_size = 1; break;
        case T_CHAR:
        case T_SHORT:   elem_size = 2; break;
        case T_INT:
        case T_FLOAT:   elem_size = 4; break;
        case T_LONG:
        case T_DOUBLE:  elem_size = 8; break;
        case DESC_OBJECT:
        case DESC_ARRAY: elem_size = sizeof(void*); break;
    }
    
    uint8_t* dest_data = (uint8_t*)array_data(dest);
    uint8_t* src_data = (uint8_t*)array_data(src);
    
    memmove(dest_data + dest_pos * elem_size,
            src_data + src_pos * elem_size,
            length * elem_size);
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_system_currentTimeMillis(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    
    jlong ms;
    
#ifdef _WIN32
    /* Windows implementation */
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    ms = (counter.QuadPart * 1000) / freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ms = (jlong)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
    
    return NATIVE_RETURN_LONG(ms);
}

static JavaValue native_system_gc(JVM* jvm, JavaThread* thread,
                                  JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;
    gc_collect(jvm);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_system_getProperties(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)args; (void)arg_count;
    
    NATIVE_DEBUG("System.getProperties() called");
    
    /* Create a Hashtable and populate with standard MIDP system properties */
    JavaClass* ht_class = jvm_load_class(jvm, "java/util/Hashtable");
    if (!ht_class) {
        NATIVE_DEBUG("System.getProperties: Hashtable class not found!");
        return NATIVE_RETURN_NULL();
    }
    
    JavaObject* ht = jvm_new_object(jvm, ht_class);
    if (!ht) {
        NATIVE_DEBUG("System.getProperties: failed to create Hashtable");
        return NATIVE_RETURN_NULL();
    }
    
    /* Standard MIDP/CLDC system properties */
    static const struct { const char* key; const char* value; } props[] = {
        /* CLDC required properties */
        { "microedition.platform",       "Nokia" },
        { "microedition.encoding",       "ISO8859-1" },
        { "microedition.configuration",  "CLDC-1.1" },
        /* MIDP required properties */
        { "microedition.profiles",       "MIDP-2.1" },
        { "microedition.locale",         "en-US" },
        { "microedition.profiles",       "MIDP-2.1" },
        /* Optional/common properties */
        { "microedition.hostname",       "localhost" },
        { "microedition.smartcardslots", "0" },
        { "microedition.commports",      "0" },
        { "microedition.pim.version",    "1.0" },
        { "microedition.io.file.FileConnection.version", "1.0" },
        /* Standard Java properties that some midlets expect */
        { "java.version",                "1.4.2" },
        { "java.vendor",                 "Nokia" },
        { "java.vm.name",                "CLDC-HI" },
        { "java.vm.version",             "1.1" },
        { "java.vm.specification.name",  "CLDC" },
        { "java.vm.specification.version", "1.1" },
        { "java.specification.name",     "Java Platform, Micro Edition" },
        { "java.specification.version",  "CLDC-1.1" },
        { "os.name",                     "SymbianOS" },
        { "os.version",                  "9.4" },
        { "os.arch",                     "ARM" },
        { "file.separator",              "/" },
        { "path.separator",              ";" },
        { "line.separator",              "\n" },
        { "user.agent",                  "Nokia" },
        { "device.model",                "Nokia" },
        { "supports.mixing",             "true" },
        { "supports.audio.capture",      "true" },
        { "supports.video.capture",      "true" },
        { "supports.recording",          "true" },
        { "wireless.messaging.sms",      "true" },
        { "wireless.messaging.mms",      "true" },
        { "wireless.messaging.cbs",      "true" },
        { "screen.width",                "240" },
        { "screen.height",               "320" },
        { "screen.isColor",              "true" },
        { "screen.bitsPerPixel",         "16" },
    };
    
    /* Find Hashtable.put() method */
    JavaMethod* put_method = jvm_resolve_method(jvm, ht_class, "put", 
                              "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    if (!put_method) {
        /* Can't populate the hashtable - return empty one */
        NATIVE_DEBUG("System.getProperties: Hashtable.put() not found, returning empty table");
        return NATIVE_RETURN_OBJECT(ht);
    }
    
    /* Populate all properties */
    JavaThread* thr = jvm_current_thread(jvm);
    for (int i = 0; i < (int)(sizeof(props) / sizeof(props[0])); i++) {
        JavaString* key_str = jvm_new_string(jvm, props[i].key);
        JavaString* val_str = jvm_new_string(jvm, props[i].value);
        if (key_str && val_str) {
            JavaValue put_args[3];
            put_args[0].ref = ht;
            put_args[1].ref = key_str;
            put_args[2].ref = val_str;
            JavaValue put_result;
            execute_method(jvm, thr, put_method, put_args, &put_result);
        }
    }
    
    NATIVE_DEBUG("System.getProperties() → Hashtable with %d properties",
                 (int)(sizeof(props) / sizeof(props[0])));
    return NATIVE_RETURN_OBJECT(ht);
}

/*
 * java.io.PrintStream native methods
 */

/* PrintStream.println(String) - print to stderr for debugging
 * NOTE: args[0]=this (PrintStream), args[1]=String argument for instance methods */
static JavaValue native_printstream_println(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread;
    /* For instance methods: args[0]=this, args[1]=first param */
    if (arg_count >= 2 && args[1].ref) {
        const char* s = native_get_string_utf8(jvm, args + 1, 0);
        if (s && s[0] != '\0') {
            fprintf(stderr, "[SYSOUT] %s\n", s);
        } else {
            fprintf(stderr, "[SYSOUT] (empty string)\n");
        }
    } else if (arg_count >= 2 && !args[1].ref) {
        fprintf(stderr, "[SYSOUT] null\n");
    } else {
        fprintf(stderr, "[SYSOUT]\n");
    }
    return NATIVE_RETURN_VOID();
}

/* PrintStream.println() - no args */
static JavaValue native_printstream_println_empty(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    fprintf(stderr, "\n");
    return NATIVE_RETURN_VOID();
}

/* PrintStream.println(int) — args[0]=this, args[1]=int */
static JavaValue native_printstream_println_int(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread;
    int val = (arg_count >= 2) ? args[1].i : 0;
    fprintf(stderr, "[SYSOUT] %d\n", val);
    return NATIVE_RETURN_VOID();
}

/* PrintStream.print(String) - no newline, args[0]=this, args[1]=String */
static JavaValue native_printstream_print(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread;
    if (arg_count >= 2 && args[1].ref) {
        const char* s = native_get_string_utf8(jvm, args + 1, 0);
        if (s) {
            fprintf(stderr, "%s", s);
        }
    }
    return NATIVE_RETURN_VOID();
}

void init_java_io_printstream(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/io/PrintStream", "println", "()V", native_printstream_println_empty},
        {"java/io/PrintStream", "println", "(Ljava/lang/String;)V", native_printstream_println},
        {"java/io/PrintStream", "println", "(I)V", native_printstream_println_int},
        {"java/io/PrintStream", "println", "(Z)V", native_printstream_println_int},
        {"java/io/PrintStream", "print", "(Ljava/lang/String;)V", native_printstream_print},
        {"java/io/PrintStream", "print", "(I)V", native_printstream_println_int},
        {"java/io/PrintStream", "flush", "()V", native_printstream_println_empty},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

void init_java_lang_system(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/lang/System", "arraycopy", "(Ljava/lang/Object;ILjava/lang/Object;II)V", native_system_arraycopy},
        {"java/lang/System", "currentTimeMillis", "()J", native_system_currentTimeMillis},
        {"java/lang/System", "gc", "()V", native_system_gc},
        {"java/lang/System", "getProperty", "(Ljava/lang/String;)Ljava/lang/String;", native_system_getProperty},
        {"java/lang/System", "getProperties", "()Ljava/util/Hashtable;", native_system_getProperties},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    
    /* Initialize System.in, System.out, System.err */
    JavaClass* system_class = jvm_load_class(jvm, "java/lang/System");
    if (system_class && system_class->static_fields) {
        /* Find PrintStream class */
        JavaClass* ps_class = jvm_load_class(jvm, "java/io/PrintStream");
        JavaClass* is_class = jvm_load_class(jvm, "java/io/InputStream");
        
        for (int i = 0; i < system_class->static_fields_count; i++) {
            if (system_class->static_fields[i].name) {
                /* System.out */
                if (strcmp(system_class->static_fields[i].name, "out") == 0 && ps_class) {
                    JavaObject* out_obj = jvm_new_object(jvm, ps_class);
                    if (out_obj) {
                        system_class->static_fields[i].value.ref = out_obj;
                        NATIVE_DEBUG("Initialized System.out");
                    }
                }
                /* System.err */
                else if (strcmp(system_class->static_fields[i].name, "err") == 0 && ps_class) {
                    JavaObject* err_obj = jvm_new_object(jvm, ps_class);
                    if (err_obj) {
                        system_class->static_fields[i].value.ref = err_obj;
                        NATIVE_DEBUG("Initialized System.err");
                    }
                }
                /* System.in */
                else if (strcmp(system_class->static_fields[i].name, "in") == 0 && is_class) {
                    JavaObject* in_obj = jvm_new_object(jvm, is_class);
                    if (in_obj) {
                        system_class->static_fields[i].value.ref = in_obj;
                        NATIVE_DEBUG("Initialized System.in");
                    }
                }
            }
        }
    }
}

/*
 * java.lang.Runtime native methods
 */

/* Singleton Runtime instance */
static JavaObject* g_runtime_instance = NULL;

static JavaValue native_runtime_getRuntime(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;
    
    /* Return singleton Runtime instance */
    if (!g_runtime_instance) {
        JavaClass* runtime_class = jvm_load_class(jvm, "java/lang/Runtime");
        if (runtime_class) {
            g_runtime_instance = jvm_new_object(jvm, runtime_class);
            /* Register as GC root to prevent collection */
            gc_add_root(jvm, (void**)&g_runtime_instance);
        }
    }
    
    return NATIVE_RETURN_OBJECT(g_runtime_instance);
}

static JavaValue native_runtime_freeMemory(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;
    HeapStats stats = heap_get_stats(jvm);
    return NATIVE_RETURN_LONG((jlong)stats.free_size);
}

static JavaValue native_runtime_totalMemory(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;
    HeapStats stats = heap_get_stats(jvm);
    return NATIVE_RETURN_LONG((jlong)stats.total_size);
}

static JavaValue native_runtime_gc(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;
    jvm_gc(jvm);
    return NATIVE_RETURN_VOID();
}

void init_java_lang_runtime(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/lang/Runtime", "getRuntime", "()Ljava/lang/Runtime;", native_runtime_getRuntime},
        {"java/lang/Runtime", "freeMemory", "()J", native_runtime_freeMemory},
        {"java/lang/Runtime", "totalMemory", "()J", native_runtime_totalMemory},
        {"java/lang/Runtime", "gc", "()V", native_runtime_gc},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.lang.Math native methods
 */

static JavaValue native_math_sqrt(JVM* jvm, JavaThread* thread,
                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jdouble value = args[0].d;
    return NATIVE_RETURN_DOUBLE(sqrt(value));
}

static JavaValue native_math_sin(JVM* jvm, JavaThread* thread,
                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jdouble value = args[0].d;
    return NATIVE_RETURN_DOUBLE(sin(value));
}

static JavaValue native_math_cos(JVM* jvm, JavaThread* thread,
                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jdouble value = args[0].d;
    return NATIVE_RETURN_DOUBLE(cos(value));
}

static JavaValue native_math_tan(JVM* jvm, JavaThread* thread,
                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jdouble value = args[0].d;
    return NATIVE_RETURN_DOUBLE(tan(value));
}

static JavaValue native_math_abs_int(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint value = args[0].i;
    return NATIVE_RETURN_INT(value < 0 ? -value : value);
}

static JavaValue native_math_abs_long(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jlong value = args[0].j;
    return NATIVE_RETURN_LONG(value < 0 ? -value : value);
}

static JavaValue native_math_abs_float(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jfloat value = args[0].f;
    return NATIVE_RETURN_FLOAT(value < 0 ? -value : value);
}

static JavaValue native_math_abs_double(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jdouble value = args[0].d;
    return NATIVE_RETURN_DOUBLE(value < 0 ? -value : value);
}

static JavaValue native_math_max_int(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint a = args[0].i;
    jint b = args[1].i;
    return NATIVE_RETURN_INT(a > b ? a : b);
}

static JavaValue native_math_min_int(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint a = args[0].i;
    jint b = args[1].i;
    return NATIVE_RETURN_INT(a < b ? a : b);
}

static JavaValue native_math_max_long(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jlong a = args[0].j;
    jlong b = args[1].j;
    return NATIVE_RETURN_LONG(a > b ? a : b);
}

static JavaValue native_math_min_long(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jlong a = args[0].j;
    jlong b = args[1].j;
    return NATIVE_RETURN_LONG(a < b ? a : b);
}

static JavaValue native_math_floor(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jdouble value = args[0].d;
    return NATIVE_RETURN_DOUBLE(floor(value));
}

static JavaValue native_math_ceil(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jdouble value = args[0].d;
    return NATIVE_RETURN_DOUBLE(ceil(value));
}

void init_java_lang_math(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/lang/Math", "sqrt", "(D)D", native_math_sqrt},
        {"java/lang/Math", "sin", "(D)D", native_math_sin},
        {"java/lang/Math", "cos", "(D)D", native_math_cos},
        {"java/lang/Math", "tan", "(D)D", native_math_tan},
        {"java/lang/Math", "abs", "(I)I", native_math_abs_int},
        {"java/lang/Math", "abs", "(J)J", native_math_abs_long},
        {"java/lang/Math", "abs", "(F)F", native_math_abs_float},
        {"java/lang/Math", "abs", "(D)D", native_math_abs_double},
        {"java/lang/Math", "max", "(II)I", native_math_max_int},
        {"java/lang/Math", "min", "(II)I", native_math_min_int},
        {"java/lang/Math", "max", "(JJ)J", native_math_max_long},
        {"java/lang/Math", "min", "(JJ)J", native_math_min_long},
        {"java/lang/Math", "floor", "(D)D", native_math_floor},
        {"java/lang/Math", "ceil", "(D)D", native_math_ceil},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.lang.Thread native methods
 */

static JavaValue native_thread_currentThread(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)args; (void)arg_count;
    return NATIVE_RETURN_OBJECT(thread ? thread->thread_object : NULL);
}

static JavaValue native_thread_sleep(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)arg_count;
    jlong millis = args[0].j;
    
    static int sleep_log_count = 0;
    sleep_log_count++;
    if (sleep_log_count <= 20) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[SLEEP] Thread.sleep(%ld) called (call #%d)\n", (long)millis, sleep_log_count);
        fflush(stderr);
    }
    
    /* Check for interruption before sleep */
    if (thread && thread->interrupted) {
        thread->interrupted = false;
        native_throw_interrupted(jvm, thread, "sleep interrupted");
        return NATIVE_RETURN_VOID();
    }
    
    /* For short sleeps, just do the sleep */
    if (millis <= 0) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Cap sleep time */
    if (millis > 10000) millis = 10000;
    
    /* ИСПРАВЛЕНО: Для pthread-based threading НЕ обрабатываем таймеры и repaints
     * внутри sleep, потому что это вызывает рекурсивное выполнение байт-кода.
     * Таймеры и repaints должны обрабатываться только в главном цикле.
     * 
     * Проблема была: jvm_process_timers() вызывал run() TimerTask'а,
     * который вызывал repaint()/serviceRepaints(), что приводило к
     * рекурсивному выполнению paint() и другим проблемам.
     * 
     * Теперь мы только обрабатываем SDL события для отзывчивости UI.
     */
    extern void sdl_process_events_minimal(void);
    
    jlong remaining = millis;
    while (remaining > 0) {
        jlong chunk = (remaining > 50) ? 50 : remaining;  /* 50ms chunks */
        
        struct timespec ts = {
            .tv_sec = chunk / 1000,
            .tv_nsec = (chunk % 1000) * 1000000
        };
        nanosleep(&ts, NULL);
        remaining -= chunk;
        
        /* Process SDL events to keep the emulator responsive */
        sdl_process_events_minimal();
        
        /* Check for interruption */
        if (thread && thread->interrupted) {
            thread->interrupted = false;
            native_throw_interrupted(jvm, thread, "sleep interrupted");
            return NATIVE_RETURN_VOID();
        }
    }
    
    if (sleep_log_count <= 20) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[SLEEP] Thread.sleep(%ld) completed (call #%d)\n", (long)millis, sleep_log_count);
        fflush(stderr);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_thread_yield(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;
    thread_yield(jvm);
    return NATIVE_RETURN_VOID();
}

/* External functions for method resolution and execution */
extern JavaMethod* jvm_resolve_method(JVM* jvm, JavaClass* clazz, const char* name, const char* descriptor);
extern int execute_method(JVM* jvm, JavaThread* thread, JavaMethod* method, 
                          JavaValue* args, JavaValue* result);

/* Thread.start() - platform-specific threading implementation */
#ifndef _WIN32
#include <pthread.h>
#include <unistd.h>
#endif

/* Forward declarations */
static JavaValue native_thread_start(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count);

/* Platform-specific thread storage */
#define MAX_PTHREADS 256

#ifdef _WIN32
/* Windows-specific threading */
static HANDLE g_win_threads[MAX_PTHREADS];
static HANDLE g_win_thread_events[MAX_PTHREADS];  /* For join() */
static CRITICAL_SECTION g_win_thread_cs;
static volatile bool g_pthread_running[MAX_PTHREADS];
static bool g_win_thread_initialized = false;

static void win_thread_init(void) {
    if (!g_win_thread_initialized) {
        InitializeCriticalSection(&g_win_thread_cs);
        for (int i = 0; i < MAX_PTHREADS; i++) {
            g_win_threads[i] = NULL;
            g_win_thread_events[i] = NULL;
            g_pthread_running[i] = false;
        }
        g_win_thread_initialized = true;
    }
}
#else
/* POSIX threading */
static pthread_t g_pthreads[MAX_PTHREADS];
static volatile bool g_pthread_running[MAX_PTHREADS];
static pthread_mutex_t g_pthread_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pthread_cond[MAX_PTHREADS];
#endif

/* Thread wrapper args */
typedef struct {
    JVM* jvm;
    JavaThread* java_thread;
    JavaMethod* run_method;
    JavaObject* run_obj;
} PthreadRunArgs;

#ifdef _WIN32
/* Windows thread runner */
static DWORD WINAPI win_thread_runner(LPVOID arg) {
    PthreadRunArgs* args = (PthreadRunArgs*)arg;
    int thread_id = args->java_thread->id;
    
    
    /* Set TLS for this thread */
    extern void pthread_set_current_thread(JavaThread* thread);
    pthread_set_current_thread(args->java_thread);
    
    /* Verify TLS was set correctly */
    extern JavaThread* thread_current(JVM* jvm);
    JavaThread* verify = thread_current(args->jvm);
    
    g_win_threads[thread_id] = GetCurrentThread();
    g_pthread_running[thread_id] = true;
    
    /* Execute the run() method */
    extern int execute_method(JVM* jvm, JavaThread* thread, JavaMethod* method, 
                              JavaValue* args, JavaValue* result);
    JavaValue this_arg = { .ref = args->run_obj };
    JavaValue result;
    execute_method(args->jvm, args->java_thread, args->run_method, &this_arg, &result);
    
    
    /* Mark thread as terminated */
    args->java_thread->is_alive = false;
    args->java_thread->interrupted = false;
    g_pthread_running[thread_id] = false;
    
    /* CRITICAL: Memory barrier before signaling completion.
     * This ensures all writes done by this thread (including field modifications)
     * are visible to other threads before they see is_alive=false or receive
     * the join signal. This provides the happens-before relationship required
     * by the Java Memory Model for Thread.join().
     */
    memory_barrier();
    
    /* Signal any waiting join() */
    if (g_win_thread_events[thread_id]) {
        SetEvent(g_win_thread_events[thread_id]);
    }
    
    /* Clear TLS before exiting */
    pthread_set_current_thread(NULL);
    
    /* Free the args */
    free(args);
    
    return 0;
}
#else
/* POSIX thread runner */
static void* pthread_thread_runner(void* arg) {
    PthreadRunArgs* args = (PthreadRunArgs*)arg;
    int thread_id = args->java_thread->id;
    
    fprintf(stderr, "[THREAD] pthread started: id=%d %s\n", 
            thread_id, args->run_obj && args->run_obj->header.clazz && args->run_obj->header.clazz->class_name ? args->run_obj->header.clazz->class_name : "?");
    fflush(stderr);
    
    /* Set TLS for this pthread so thread_current() works correctly */
    extern void pthread_set_current_thread(JavaThread* thread);
    pthread_set_current_thread(args->java_thread);
    
    /* Store pthread_t for join */
    g_pthreads[thread_id] = pthread_self();
    g_pthread_running[thread_id] = true;
    
    fprintf(stderr, "[THREAD] Calling run() id=%d %s\n",
            thread_id, args->run_obj && args->run_obj->header.clazz && args->run_obj->header.clazz->class_name ? args->run_obj->header.clazz->class_name : "?");
    fflush(stderr);
    
    /* Execute the run() method */
    extern int execute_method(JVM* jvm, JavaThread* thread, JavaMethod* method, 
                              JavaValue* args, JavaValue* result);
    JavaValue this_arg = { .ref = args->run_obj };
    JavaValue result;
    execute_method(args->jvm, args->java_thread, args->run_method, &this_arg, &result);
    
    fprintf(stderr, "[THREAD] run() done id=%d exc=%p\n", thread_id, (void*)args->java_thread->pending_exception);
    fflush(stderr);
    
    /* Check for uncaught exception */
    if (args->java_thread->pending_exception) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[THREAD] Thread '%s' terminated with uncaught exception: %s\n",
                args->java_thread->name ? args->java_thread->name : "(unnamed)",
                args->java_thread->pending_exception->header.clazz ? 
                args->java_thread->pending_exception->header.clazz->class_name : "?");
        fflush(stderr);
        /* Set global flag so main loop can detect this */
        extern bool g_has_uncaught_exception;
        g_has_uncaught_exception = true;
    }
    
    /* Mark thread as terminated */
    args->java_thread->is_alive = false;
    args->java_thread->interrupted = false;
    g_pthread_running[thread_id] = false;
    
    /* CRITICAL: Memory barrier before signaling completion.
     * This ensures all writes done by this thread (including field modifications)
     * are visible to other threads before they see is_alive=false or receive
     * the join signal. This provides the happens-before relationship required
     * by the Java Memory Model for Thread.join().
     */
    memory_barrier();
    
    /* Signal any waiting join() */
    pthread_mutex_lock(&g_pthread_mutex);
    pthread_cond_broadcast(&g_pthread_cond[thread_id]);
    pthread_mutex_unlock(&g_pthread_mutex);
    
    /* Clear TLS before exiting */
    pthread_set_current_thread(NULL);
    
    /* Free the args */
    free(args);
    
    return NULL;
}
#endif

static JavaValue native_thread_start(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* thread_obj = (JavaObject*)args[0].ref;
    
    static int thread_start_count = 0;
    thread_start_count++;
    
    if (!thread_obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    /* Get the Runnable target */
    JavaObject* run_target = (JavaObject*)native_get_field_value(thread_obj, "target").ref;
    
    /* Determine which object's run() to call */
    JavaObject* run_obj = run_target ? run_target : thread_obj;
    JavaClass* run_class = run_obj->header.clazz;
    
    fprintf(stderr, "[THREAD] Thread.start() (#%d): %s\n", 
            thread_start_count, run_class && run_class->class_name ? run_class->class_name : "?");
    
    /* Find run() method */
    JavaMethod* run_method = jvm_resolve_method(jvm, run_class, "run", "()V");
    if (!run_method) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Get thread name from Java field */
    char thread_name[64];
    JavaString* name_str = (JavaString*)native_get_field_value(thread_obj, "name").ref;
    if (name_str && name_str->utf8) {
        strncpy(thread_name, name_str->utf8, sizeof(thread_name) - 1);
        thread_name[sizeof(thread_name) - 1] = '\0';
    } else {
        snprintf(thread_name, sizeof(thread_name), "Thread-%d", jvm->thread_count + 1);
    }
    
    /* Get thread priority from Java field */
    jint priority = native_get_field_value(thread_obj, "priority").i;
    if (priority < 1 || priority > 10) {
        priority = THREAD_PRIORITY_NORM;  /* Default to 5 if invalid */
    }
    
    JavaThread* new_java_thread = thread_create(jvm, thread_name, priority, thread_obj);
    if (!new_java_thread) {
        return NATIVE_RETURN_VOID();
    }
    
    new_java_thread->is_alive = true;
    
    /* Create thread args */
    PthreadRunArgs* pthread_args = (PthreadRunArgs*)malloc(sizeof(PthreadRunArgs));
    if (!pthread_args) {
        return NATIVE_RETURN_VOID();
    }
    
    pthread_args->jvm = jvm;
    pthread_args->java_thread = new_java_thread;
    pthread_args->run_method = run_method;
    pthread_args->run_obj = run_obj;
    
#ifdef _WIN32
    /* Windows thread creation */
    win_thread_init();
    
    /* Create event for join() */
    g_win_thread_events[new_java_thread->id] = CreateEvent(NULL, TRUE, FALSE, NULL);
    
    /* Create Windows thread */
    HANDLE hThread = CreateThread(NULL, 0, win_thread_runner, pthread_args, 0, NULL);
    if (hThread == NULL) {
        free(pthread_args);
        if (g_win_thread_events[new_java_thread->id]) {
            CloseHandle(g_win_thread_events[new_java_thread->id]);
            g_win_thread_events[new_java_thread->id] = NULL;
        }
        new_java_thread->is_alive = false;
        return NATIVE_RETURN_VOID();
    }
    g_win_threads[new_java_thread->id] = hThread;
    
    /* Give the new thread a chance to start */
    Sleep(1);
#else
    /* POSIX thread creation */
    pthread_t pthread_id;
    int rc = pthread_create(&pthread_id, NULL, pthread_thread_runner, pthread_args);
    if (rc != 0) {
        free(pthread_args);
        new_java_thread->is_alive = false;
        return NATIVE_RETURN_VOID();
    }
    
    /* Detach the thread so it cleans up automatically */
    pthread_detach(pthread_id);
    
    /* Give the new thread a chance to start */
    usleep(1000);  /* 1ms */
#endif
    
    return NATIVE_RETURN_VOID();
}

/* Thread.<init>() - default constructor */
static JavaValue native_thread_init(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* thread_obj = (JavaObject*)args[0].ref;
    
    if (!thread_obj) {
        return NATIVE_RETURN_VOID();
    }
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue null_val = { .ref = NULL };
    native_set_field_value(thread_obj, "target", null_val);
    THREAD_DEBUG("<init>(): target field initialized to NULL");
    
    return NATIVE_RETURN_VOID();
}

/* Thread.<init>(Runnable) - constructor with Runnable target */
static JavaValue native_thread_init_runnable(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* thread_obj = (JavaObject*)args[0].ref;
    JavaObject* runnable = (JavaObject*)args[1].ref;
    
    if (!thread_obj) {
        return NATIVE_RETURN_VOID();
    }
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    JavaValue runnable_val = { .ref = runnable };
    native_set_field_value(thread_obj, "target", runnable_val);
    THREAD_DEBUG("<init>(Runnable): target field set to %p", (void*)runnable);
    
    return NATIVE_RETURN_VOID();
}

/* Thread.close() - called when thread terminates */
static JavaValue native_thread_close(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* thread_obj = (JavaObject*)args[0].ref;
    
    if (thread_obj) {
        THREAD_DEBUG("close() called for thread object %p", (void*)thread_obj);
    }
    
    return NATIVE_RETURN_VOID();
}

/* Thread.isAlive() - check if thread is alive */
static JavaValue native_thread_isAlive(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* thread_obj = (JavaObject*)args[0].ref;
    
    if (!thread_obj) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Find the JavaThread and check if alive */
    extern JavaThread* thread_find_by_object(JavaObject* thread_obj);
    JavaThread* target = thread_find_by_object(thread_obj);
    
    
    if (target && target->is_alive) {
        return NATIVE_RETURN_INT(1);
    }
    
    return NATIVE_RETURN_INT(0);
}

/* Thread.join() - wait for thread to terminate */
static JavaValue native_thread_join(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* thread_obj = (JavaObject*)args[0].ref;
    
    if (!thread_obj) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Find the JavaThread */
    extern JavaThread* thread_find_by_object(JavaObject* thread_obj);
    JavaThread* target = thread_find_by_object(thread_obj);
    
    if (!target) {
        return NATIVE_RETURN_VOID();  /* Thread already terminated or never started */
    }
    
    int thread_id = target->id;
    
    /* Check if already terminated */
    if (!target->is_alive && !g_pthread_running[thread_id]) {
        return NATIVE_RETURN_VOID();
    }
    
#ifdef _WIN32
    /* Windows: wait on event with timeout */
    /* CRITICAL: Use || (OR) not && (AND). We wait while EITHER flag indicates
     * the thread is running. With &&, if one flag becomes false before the other
     * becomes true (race condition during thread startup), join returns immediately.
     */
    while (g_pthread_running[thread_id] || target->is_alive) {
        if (g_win_thread_events[thread_id]) {
            DWORD result = WaitForSingleObject(g_win_thread_events[thread_id], 50);
            if (result == WAIT_OBJECT_0) {
                break;  /* Thread terminated */
            }
        } else {
            Sleep(10);  /* Fallback polling */
        }
    }
#else
    /* POSIX: wait on condition variable with timeout */
    pthread_mutex_lock(&g_pthread_mutex);
    /* CRITICAL: Use || (OR) not && (AND). We wait while EITHER flag indicates
     * the thread is running. With &&, if one flag becomes false before the other
     * becomes true (race condition during thread startup), join returns immediately.
     */
    while (g_pthread_running[thread_id] || target->is_alive) {
        /* Use timed wait to avoid infinite blocking */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50000000;  /* 50ms */
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&g_pthread_cond[thread_id], &g_pthread_mutex, &ts);
    }
    pthread_mutex_unlock(&g_pthread_mutex);
#endif

    /* CRITICAL: Add memory barrier after join to ensure visibility of all writes
     * done by the joined thread. This provides the happens-before relationship
     * guaranteed by Thread.join() in the Java Memory Model.
     */
    memory_barrier();
    
    return NATIVE_RETURN_VOID();
}

/* Thread.interrupt() - interrupt thread */
static JavaValue native_thread_interrupt(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* thread_obj = (JavaObject*)args[0].ref;
    
    if (thread_obj) {
        /* Find the JavaThread for this thread object */
        extern JavaThread* thread_find_by_object(JavaObject* thread_obj);
        JavaThread* target = thread_find_by_object(thread_obj);
        if (target) {
            thread_interrupt(target);
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Thread.isInterrupted() - check if thread is interrupted */
static JavaValue native_thread_isInterrupted(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(0);
}

/* Thread.interrupted() - static method to check and clear interrupted status */
static JavaValue native_thread_interrupted(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_INT(0);
}

/* Thread.setPriority(int) */
static JavaValue native_thread_setPriority(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* thread_obj = (JavaObject*)args[0].ref;
    jint priority = args[1].i;
    
    if (thread_obj) {
        /* ИСПРАВЛЕНО: Используем native_set_field_value */
        JavaValue priority_val = { .i = priority };
        native_set_field_value(thread_obj, "priority", priority_val);
    }
    
    return NATIVE_RETURN_VOID();
}

/* Thread.getPriority() */
static JavaValue native_thread_getPriority(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* thread_obj = (JavaObject*)args[0].ref;
    
    if (!thread_obj) {
        return NATIVE_RETURN_INT(5);  /* NORM_PRIORITY */
    }
    
    jint priority = native_get_field_value(thread_obj, "priority").i;
    
    /* Return NORM_PRIORITY (5) if field is not set (0) */
    if (priority == 0) {
        priority = 5;
    }
    
    return NATIVE_RETURN_INT(priority);
}

/* Thread.setName(String) */
static JavaValue native_thread_setName(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    /* Just ignore - name is stored in Java field */
    return NATIVE_RETURN_VOID();
}

/* Thread.getName() */
static JavaValue native_thread_getName(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* thread_obj = (JavaObject*)args[0].ref;
    
    if (!thread_obj) {
        return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, "Thread-0"));
    }
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    JavaString* name = (JavaString*)native_get_field_value(thread_obj, "name").ref;
    if (name) {
        return NATIVE_RETURN_OBJECT(name);
    }
    
    return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, "Thread-?"));
}

/* Thread.activeCount() - static */
static JavaValue native_thread_activeCount(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_INT(1);  /* Just main thread */
}

/* Thread.holdsLock(Object) - static */
static JavaValue native_thread_holdsLock(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_INT(0);
}

void init_java_lang_thread(JVM* jvm) {
    /* Initialize condition variables for join (POSIX only) */
#ifndef _WIN32
    for (int i = 0; i < MAX_PTHREADS; i++) {
        pthread_cond_init(&g_pthread_cond[i], NULL);
        g_pthread_running[i] = false;
    }
#else
    /* Windows: initialize thread system */
    win_thread_init();
#endif
    
    NativeMethodEntry methods[] = {
        {"java/lang/Thread", "currentThread", "()Ljava/lang/Thread;", native_thread_currentThread},
        {"java/lang/Thread", "sleep", "(J)V", native_thread_sleep},
        {"java/lang/Thread", "yield", "()V", native_thread_yield},
        {"java/lang/Thread", "start", "()V", native_thread_start},
        {"java/lang/Thread", "join", "()V", native_thread_join},
        {"java/lang/Thread", "close", "()V", native_thread_close},
        {"java/lang/Thread", "isAlive", "()Z", native_thread_isAlive},
        {"java/lang/Thread", "interrupt", "()V", native_thread_interrupt},
        {"java/lang/Thread", "isInterrupted", "()Z", native_thread_isInterrupted},
        {"java/lang/Thread", "interrupted", "()Z", native_thread_interrupted},
        {"java/lang/Thread", "setPriority", "(I)V", native_thread_setPriority},
        {"java/lang/Thread", "getPriority", "()I", native_thread_getPriority},
        {"java/lang/Thread", "setName", "(Ljava/lang/String;)V", native_thread_setName},
        {"java/lang/Thread", "getName", "()Ljava/lang/String;", native_thread_getName},
        {"java/lang/Thread", "activeCount", "()I", native_thread_activeCount},
        {"java/lang/Thread", "holdsLock", "(Ljava/lang/Object;)Z", native_thread_holdsLock},
        {"java/lang/Thread", "<init>", "()V", native_thread_init},
        {"java/lang/Thread", "<init>", "(Ljava/lang/Runnable;)V", native_thread_init_runnable},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * javax.microedition.midlet.MIDlet native methods
 * 
 * MIDlet lifecycle management:
 * - notifyDestroyed(): Called by MIDlet to tell AMS it wants to be destroyed
 * - notifyPaused(): Called by MIDlet to tell AMS it wants to be paused
 * These methods are protected in MIDlet class and called from startApp/pauseApp/destroyApp
 */

/* Global MIDlet state */
static bool g_midlet_destroyed = false;
static bool g_midlet_paused = false;

/* Global manifest data for getAppProperty */
static char* g_manifest_data = NULL;
static size_t g_manifest_size = 0;

/* Set manifest data for getAppProperty - called from main.c */
void midlet_set_manifest(const char* manifest_data, size_t size) {
    if (g_manifest_data) {
        free(g_manifest_data);
    }
    g_manifest_data = (char*)malloc(size + 1);
    if (g_manifest_data) {
        memcpy(g_manifest_data, manifest_data, size);
        g_manifest_data[size] = '\0';
        g_manifest_size = size;
    }
}

/* Append JAD data to manifest for getAppProperty - called from main.c */
void midlet_append_manifest(const char* data) {
    if (!data) return;
    
    size_t data_len = strlen(data);
    if (data_len == 0) return;
    
    if (!g_manifest_data) {
        midlet_set_manifest(data, data_len);
        return;
    }
    
    /* Append new data with newline separator */
    size_t new_size = g_manifest_size + 1 + data_len;  /* +1 for newline */
    char* new_manifest = (char*)malloc(new_size + 1);
    if (new_manifest) {
        memcpy(new_manifest, g_manifest_data, g_manifest_size);
        new_manifest[g_manifest_size] = '\n';
        memcpy(new_manifest + g_manifest_size + 1, data, data_len);
        new_manifest[new_size] = '\0';
        
        free(g_manifest_data);
        g_manifest_data = new_manifest;
        g_manifest_size = new_size;
        DEBUG_LOG("[MIDlet] Manifest appended, new size: %zu", g_manifest_size);
    }
}

/* Add a single property to manifest */
void midlet_add_property(const char* key, const char* value) {
    if (!key || !value) return;
    
    size_t key_len = strlen(key);
    size_t value_len = strlen(value);
    size_t line_len = key_len + 2 + value_len;  /* key: value */
    
    char* line = (char*)malloc(line_len + 1);
    if (!line) return;
    
    snprintf(line, line_len + 1, "%s: %s", key, value);
    midlet_append_manifest(line);
    free(line);
}

/* MIDlet.notifyDestroyed() - tell AMS the MIDlet wants to be destroyed */
static JavaValue native_midlet_notifyDestroyed(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;
    
    if (g_j2me_runtime_debug) fprintf(stderr, "[NATIVE] MIDlet.notifyDestroyed() called! Stack trace:\n");
    fflush(stderr);
    
    /* Print simple stack trace */
    if (thread && thread->current_frame) {
        JavaFrame* frame = thread->current_frame;
        int depth = 0;
        while (frame && depth < 10) {
            if (frame->method && frame->method->clazz && frame->method->clazz->class_name) {
                fprintf(stderr, "  [%d] %s.%s\n", depth, 
                        frame->method->clazz->class_name,
                        frame->method->name ? frame->method->name : "?");
            }
            frame = frame->prev;
            depth++;
        }
        fflush(stderr);
    }
    
    g_midlet_destroyed = true;
    g_midlet_paused = false;
    
    /* Stop the JVM main loop */
    if (jvm) {
        jvm->running = false;
    }
    
    return NATIVE_RETURN_VOID();
}

/* MIDlet.notifyPaused() - tell AMS the MIDlet wants to be paused */
static JavaValue native_midlet_notifyPaused(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    
    g_midlet_paused = true;
    
    return NATIVE_RETURN_VOID();
}

/* MIDlet.resumeApp() - called by AMS to resume a paused MIDlet 
 * This is protected, called by the system, not by the MIDlet itself */
static JavaValue native_midlet_resumeApp(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    
    g_midlet_paused = false;
    
    /* Find and call startApp on the MIDlet */
    JavaObject* midlet = (JavaObject*)args[0].ref;
    if (midlet && midlet->header.clazz) {
        JavaMethod* startApp = jvm_resolve_method(jvm, midlet->header.clazz, "startApp", "()V");
        if (startApp) {
            JavaValue result;
            execute_method(jvm, thread, startApp, args, &result);
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Check if MIDlet is destroyed */
bool midlet_is_destroyed(void) {
    return g_midlet_destroyed;
}

/* Check if MIDlet is paused */
bool midlet_is_paused(void) {
    return g_midlet_paused;
}

/* Reset MIDlet state (for new MIDlet load) */
void midlet_reset_state(void) {
    g_midlet_destroyed = false;
    g_midlet_paused = false;
}

/* MIDlet.getAppProperty(String key) - get property from manifest */
static JavaValue native_midlet_getAppProperty(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    
    JavaObject* key_str = (JavaObject*)args[1].ref;  /* args[0] is this, args[1] is key */
    if (!key_str) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[MIDlet.getAppProperty] key_str is NULL\n");
        fflush(stderr);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    /* Get the key string */
    const char* key = string_utf8(jvm, (JavaString*)key_str);
    if (!key) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[MIDlet.getAppProperty] key is NULL (string conversion failed)\n");
        fflush(stderr);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    if (!g_manifest_data) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[MIDlet.getAppProperty] WARNING: g_manifest_data is NULL!\n");
        fflush(stderr);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    if (g_j2me_runtime_debug) fprintf(stderr, "[MIDlet.getAppProperty] '%s'\n", key);
    fflush(stderr);
    
    /* Search for key in manifest */
    char* result = NULL;
    char* manifest_copy = strdup(g_manifest_data);
    if (!manifest_copy) {
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    char* line = strtok(manifest_copy, "\r\n");
    while (line) {
        /* Skip leading whitespace */
        while (*line == ' ' || *line == '\t') line++;
        
        /* Check if line starts with the key followed by ':' */
        size_t key_len = strlen(key);
        if (strncmp(line, key, key_len) == 0 && line[key_len] == ':') {
            /* Found the key, extract the value */
            char* value = line + key_len + 1;
            
            /* Skip leading whitespace after colon */
            while (*value == ' ' || *value == '\t') value++;
            
            /* Handle line continuation (lines starting with space) */
            char* full_value = strdup(value);
            char* next_line = NULL;
            
            /* Check for continuation lines */
            char* saveptr;
            while ((next_line = strtok(NULL, "\r\n")) != NULL) {
                if (next_line[0] == ' ' || next_line[0] == '\t') {
                    /* Continuation line */
                    char* new_full_value = (char*)malloc(strlen(full_value) + strlen(next_line) + 1);
                    if (new_full_value) {
                        strcpy(new_full_value, full_value);
                        strcat(new_full_value, next_line);
                        free(full_value);
                        full_value = new_full_value;
                    }
                } else {
                    /* Not a continuation, put back and stop */
                    break;
                }
            }
            
            result = full_value;
            break;
        }
        
        line = strtok(NULL, "\r\n");
    }
    
    free(manifest_copy);
    
    if (result) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[MIDlet.getAppProperty] '%s' = '%s'\n", key, result);
        fflush(stderr);
        JavaObject* result_str = (JavaObject*)jvm_new_string(jvm, result);
        free(result);
        return NATIVE_RETURN_OBJECT(result_str);
    }
    
    if (g_j2me_runtime_debug) fprintf(stderr, "[MIDlet.getAppProperty] '%s' = NULL\n", key);
    fflush(stderr);
    return NATIVE_RETURN_OBJECT(NULL);
}

void init_javax_microedition_midlet_MIDlet(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"javax/microedition/midlet/MIDlet", "notifyDestroyed", "()V", native_midlet_notifyDestroyed},
        {"javax/microedition/midlet/MIDlet", "notifyPaused", "()V", native_midlet_notifyPaused},
        {"javax/microedition/midlet/MIDlet", "resumeApp", "()V", native_midlet_resumeApp},
        {"javax/microedition/midlet/MIDlet", "getAppProperty", "(Ljava/lang/String;)Ljava/lang/String;", native_midlet_getAppProperty},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.lang.Throwable native methods
 */

static JavaValue native_throwable_fillInStackTrace(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* throwable = NATIVE_ARG_OBJECT(args, 0);
    
    if (!throwable) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Count stack frames */
    int frame_count = 0;
    JavaFrame* frame = thread ? thread->current_frame : NULL;
    while (frame) {
        frame_count++;
        frame = frame->prev;
    }
    
    /* Find or create StackTraceElement class */
    JavaClass* ste_class = jvm_load_class(jvm, "java/lang/StackTraceElement");
    if (!ste_class) {
        return NATIVE_RETURN_OBJECT(throwable);
    }
    
    /* Create StackTraceElement[] array */
    JavaArray* ste_array = heap_alloc_array(jvm, DESC_OBJECT, frame_count, ste_class);
    if (!ste_array) {
        return NATIVE_RETURN_OBJECT(throwable);
    }
    
    /* Fill in stack trace elements */
    frame = thread ? thread->current_frame : NULL;
    for (int i = 0; i < frame_count && frame; i++) {
        JavaObject* ste = heap_alloc_object(jvm, ste_class);
        if (!ste) break;
        
        /* Set fields: declaringClass, methodName, fileName, lineNumber */
        int field_idx = 0;
        
        /* declaringClass (String) */
        const char* class_name = frame->clazz ? frame->clazz->class_name : "Unknown";
        JavaString* class_str = jvm_new_string(jvm, class_name);
        if (ste_class->fields_count > field_idx && class_str) {
            ste->fields[field_idx].ref = (JavaObject*)class_str;
        }
        field_idx++;
        
        /* methodName (String) */
        const char* method_name = frame->method ? frame->method->name : "unknown";
        JavaString* method_str = jvm_new_string(jvm, method_name);
        if (ste_class->fields_count > field_idx && method_str) {
            ste->fields[field_idx].ref = (JavaObject*)method_str;
        }
        field_idx++;
        
        /* fileName (String) - we don't have this info, use null */
        if (ste_class->fields_count > field_idx) {
            ste->fields[field_idx].ref = NULL;
        }
        field_idx++;
        
        /* lineNumber (int) */
        if (ste_class->fields_count > field_idx) {
            /* Try to compute line number from PC */
            jint line_num = -1;
            if (frame->method && frame->method->code.code && frame->pc > 0) {
                /* Simple approximation: use PC as line number indicator */
                line_num = (jint)frame->pc;
            }
            ste->fields[field_idx].i = line_num;
        }
        
        /* Store in array */
        ((JavaObject**)array_data(ste_array))[i] = ste;
        
        frame = frame->prev;
    }
    
    /* Set the stackTrace field in Throwable */
    /* Find the stackTrace field */
    JavaClass* throwable_class = throwable->header.clazz;
    if (throwable_class) {
        for (int i = 0; i < throwable_class->fields_count; i++) {
            if (throwable_class->fields[i].name && 
                strcmp(throwable_class->fields[i].name, "stackTrace") == 0) {
                throwable->fields[i].ref = (JavaObject*)ste_array;
                break;
            }
        }
    }
    
    return NATIVE_RETURN_OBJECT(throwable);
}

/* Throwable.printStackTrace() - prints stack trace to stderr */
static JavaValue native_throwable_printStackTrace(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* throwable = NATIVE_ARG_OBJECT(args, 0);
    
    /* Rate limit exception printing */
    static int printStackTrace_count = 0;
    if (printStackTrace_count >= 10) {
        return NATIVE_RETURN_VOID();
    }
    printStackTrace_count++;
    if (printStackTrace_count == 10) {
        fprintf(stderr, "[EXCEPTION] Further printStackTrace() output suppressed\n");
    }
    
    if (!throwable) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Get class name */
    JavaClass* clazz = throwable->header.clazz;
    const char* class_name = clazz ? clazz->class_name : "Unknown";
    
    /* Get message */
    JavaString* msg = (JavaString*)native_get_field_value(throwable, "detailMessage").ref;
    
    /* Print header: ExceptionName: message */
    fprintf(stderr, "%s", class_name);
    if (msg && string_length(msg) > 0) {
        fprintf(stderr, ": %.*s", string_length(msg), (char*)string_chars(msg));
    }
    fprintf(stderr, "\n");
    
    /* Get stack trace array */
    JavaArray* ste_array = (JavaArray*)native_get_field_value(throwable, "stackTrace").ref;
    if (ste_array && object_is_array((JavaObject*)ste_array)) {
        int len = ste_array->length;
        JavaObject** elements = (JavaObject**)array_data(ste_array);
        
        for (int i = 0; i < len; i++) {
            JavaObject* ste = elements[i];
            if (!ste) continue;
            
            /* Get StackTraceElement fields */
            JavaString* decl_class = (JavaString*)native_get_field_value(ste, "declaringClass").ref;
            JavaString* method = (JavaString*)native_get_field_value(ste, "methodName").ref;
            JavaString* file = (JavaString*)native_get_field_value(ste, "fileName").ref;
            jint line = native_get_field_value(ste, "lineNumber").i;
            
            fprintf(stderr, "    at ");
            if (decl_class && string_length(decl_class) > 0) {
                fprintf(stderr, "%.*s", string_length(decl_class), (char*)string_chars(decl_class));
            }
            fprintf(stderr, ".");
            if (method && string_length(method) > 0) {
                fprintf(stderr, "%.*s", string_length(method), (char*)string_chars(method));
            }
            
            if (file && string_length(file) > 0) {
                if (line > 0) {
                    fprintf(stderr, "(%.*s:%d)\n", string_length(file), (char*)string_chars(file), line);
                } else {
                    fprintf(stderr, "(%.*s)\n", string_length(file), (char*)string_chars(file));
                }
            } else {
                fprintf(stderr, "(Unknown Source)\n");
            }
        }
    }
    
    fflush(stderr);
    return NATIVE_RETURN_VOID();
}

void init_java_lang_throwable(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/lang/Throwable", "fillInStackTrace", "()Ljava/lang/Throwable;", native_throwable_fillInStackTrace},
        {"java/lang/Throwable", "printStackTrace", "()V", native_throwable_printStackTrace},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.util.Random native methods
 * 
 * Random uses a simple linear congruential generator:
 * seed = (seed * 0x5DEECE66DL + 0xBL) & ((1L << 48) - 1)
 * 
 * We store the seed in the first field of the Random object.
 */

/* Helper to get seed from Random object - ИСПРАВЛЕНО: используем native_get_field_value */
static jlong random_get_seed(JavaObject* obj) {
    if (!obj) return 0;
    return native_get_field_value(obj, "seed").j;
}

/* Helper to set seed in Random object - ИСПРАВЛЕНО: используем native_set_field_value */
static void random_set_seed(JavaObject* obj, jlong seed) {
    if (!obj) return;
    JavaValue val = { .j = seed };
    native_set_field_value(obj, "seed", val);
}

/* Random() - constructor with time-based seed */
static JavaValue native_random_init(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    /* Initialize seed with current time */
    jlong seed;
#ifdef _WIN32
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    seed = counter.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    seed = (jlong)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
    
    /* Apply the initial scramble: seed ^ 0x5DEECE66DL */
    seed ^= 0x5DEECE66DLL;
    
    random_set_seed(obj, seed);
    
    NATIVE_DEBUG("<init>: obj=%p, seed=%lld", (void*)obj, (long long)seed);
    
    return NATIVE_RETURN_VOID();
}

/* Random(long seed) - constructor with explicit seed */
static JavaValue native_random_init_seed(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jlong seed = args[1].j;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    /* Apply the initial scramble: seed ^ 0x5DEECE66DL */
    seed ^= 0x5DEECE66DLL;
    
    random_set_seed(obj, seed);
    
    NATIVE_DEBUG("<init>(seed): obj=%p, seed=%lld", (void*)obj, (long long)seed);
    
    return NATIVE_RETURN_VOID();
}

/* Random.next(int bits) - generate next random value */
static JavaValue native_random_next(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint bits = args[1].i;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    
    /* ИСПРАВЛЕНО: используем random_get_seed/random_set_seed вместо указателя */
    jlong seed = random_get_seed(obj);
    
    /* seed = (seed * 0x5DEECE66DL + 0xBL) & ((1L << 48) - 1) */
    seed = (seed * 0x5DEECE66DLL + 0xBL) & ((1LL << 48) - 1);
    random_set_seed(obj, seed);
    
    /* return (int)(seed >>> (48 - bits)) */
    jint result = (jint)(seed >> (48 - bits));
    
    return NATIVE_RETURN_INT(result);
}

/* Random.nextInt() - random integer */
static JavaValue native_random_nextInt(JVM* jvm, JavaThread* thread,
JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    
    /* ИСПРАВЛЕНО: используем random_get_seed/random_set_seed */
    jlong seed = random_get_seed(obj);
    
    /* Generate next random value (32 bits) */
    seed = (seed * 0x5DEECE66DLL + 0xBL) & ((1LL << 48) - 1);
    random_set_seed(obj, seed);
    
    jint result = (jint)(seed >> 16);
    
    NATIVE_DEBUG("nextInt(): result=%d, seed=%lld", result, (long long)seed);
    
    return NATIVE_RETURN_INT(result);
}

/* Random.nextInt(int bound) - random integer in [0, bound) */
static JavaValue native_random_nextIntBound(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint bound = args[1].i;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    
    if (bound <= 0) {
        native_throw_iae(jvm, thread, "bound must be positive");
        return NATIVE_RETURN_INT(0);
    }
    
    /* ИСПРАВЛЕНО: используем random_get_seed/random_set_seed */
    jlong seed = random_get_seed(obj);
    
    jint result;
    if ((bound & -bound) == bound) {
        /* bound is a power of 2 */
        seed = (seed * 0x5DEECE66DLL + 0xBL) & ((1LL << 48) - 1);
        result = (jint)((bound * (seed >> 17)) >> 31);
    } else {
        /* Reject samples that would produce bias */
        jint val;
        do {
            seed = (seed * 0x5DEECE66DLL + 0xBL) & ((1LL << 48) - 1);
            val = (jint)(seed >> 17);
            jint r = val % bound;
            /* Check for bias */
            while (val - r + (bound - 1) < 0) {
                seed = (seed * 0x5DEECE66DLL + 0xBL) & ((1LL << 48) - 1);
                val = (jint)(seed >> 17);
                r = val % bound;
            }
            result = r;
            break;  /* Simplified - just take the first value */
        } while (0);
    }
    
    random_set_seed(obj, seed);
    
    NATIVE_DEBUG("nextInt(%d): result=%d, seed=%lld", bound, result, (long long)seed);
    
    return NATIVE_RETURN_INT(result);
}

/* Random.nextLong() - random long */
static JavaValue native_random_nextLong(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_LONG(0);
    }
    
    /* ИСПРАВЛЕНО: используем random_get_seed/random_set_seed */
    jlong seed = random_get_seed(obj);
    
    /* Generate two 32-bit values and combine */
    seed = (seed * 0x5DEECE66DLL + 0xBL) & ((1LL << 48) - 1);
    jint high = (jint)(seed >> 16);
    seed = (seed * 0x5DEECE66DLL + 0xBL) & ((1LL << 48) - 1);
    jint low = (jint)(seed >> 16);
    random_set_seed(obj, seed);
    
    jlong result = ((jlong)high << 32) | (low & 0xFFFFFFFFLL);
    
    return NATIVE_RETURN_LONG(result);
}

/* Random.nextFloat() - random float in [0, 1) */
static JavaValue native_random_nextFloat(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_FLOAT(0.0f);
    }
    
    /* ИСПРАВЛЕНО: используем random_get_seed/random_set_seed */
    jlong seed = random_get_seed(obj);
    seed = (seed * 0x5DEECE66DLL + 0xBL) & ((1LL << 48) - 1);
    random_set_seed(obj, seed);
    
    jfloat result = (jfloat)(seed >> 24) / (jfloat)(1LL << 24);
    
    return NATIVE_RETURN_FLOAT(result);
}

/* Random.nextDouble() - random double in [0, 1) */
static JavaValue native_random_nextDouble(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_DOUBLE(0.0);
    }
    
    /* ИСПРАВЛЕНО: используем random_get_seed/random_set_seed */
    jlong seed = random_get_seed(obj);
    seed = (seed * 0x5DEECE66DLL + 0xBL) & ((1LL << 48) - 1);
    jint high = (jint)(seed >> 16);
    seed = (seed * 0x5DEECE66DLL + 0xBL) & ((1LL << 48) - 1);
    jint low = (jint)(seed >> 16);
    random_set_seed(obj, seed);
    
    jlong combined = ((jlong)high << 27) | (low >> 5);
    jdouble result = (jdouble)combined / (jdouble)(1LL << 53);
    
    return NATIVE_RETURN_DOUBLE(result);
}

/* Random.setSeed(long) - set the seed */
static JavaValue native_random_setSeed(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jlong seed = args[1].j;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    /* Apply the initial scramble: seed ^ 0x5DEECE66DL */
    seed ^= 0x5DEECE66DLL;
    
    random_set_seed(obj, seed);
    
    return NATIVE_RETURN_VOID();
}

/*
 * java.lang.StringBuffer native methods
 * 
 * StringBuffer is used for building strings. We need to implement append() 
 * which returns 'this' for method chaining, and toString().
 */

/* StringBuffer buffer field access */
#define STRINGBUFFER_INITIAL_CAPACITY 16
#define STRINGBUFFER_INITIAL_ENTRIES 64

/* ИСПРАВЛЕНИЕ: Fallback storage for StringBuffer objects that don't have enough fields.
 * Some JAR files may have StringBuffer class with fewer than 3 instance fields.
 * We use dynamic array to store buffer state for such objects.
 */
typedef struct {
    void* obj_ptr;
    char* buffer;
    int length;
    int capacity;
    uint64_t alloc_id;  /* Unique ID for tracking (currently unused, kept for future use) */
} StringBufferFallback;

static StringBufferFallback* g_stringbuffer_fallback = NULL;
static int g_stringbuffer_fallback_count = 0;
static int g_stringbuffer_fallback_capacity = 0;


/* Helper: Check if object has enough instance fields for inline storage
 * ИСПРАВЛЕНО: Всегда возвращаем false чтобы использовать fallback механизм,
 * так как прямое использование field[0/1/2] может конфликтовать с реальными полями Java
 */
static bool stringbuffer_has_enough_fields(JavaObject* obj) {
    (void)obj;
    /* Всегда используем fallback для безопасности */
    return false;
}


/* Helper: Find or create fallback entry for object.
 * ИСПРАВЛЕНО: Используем identity hashcode объекта вместо truncated allocation ID.
 * Identity hashcode уже хранится в объекте и не усекается.
 */
static StringBufferFallback* stringbuffer_get_fallback(JavaObject* obj) {
    if (!obj) return NULL;
    
    /* Use the object's identity hashcode as unique ID - this is already stored in the object
     * header and doesn't have the truncation issue that the reserved byte had. */
    uint64_t obj_id = (uint64_t)(uintptr_t)obj;  /* Use pointer as unique ID */
    
    StringBufferFallback* free_slot = NULL;  /* Remember any free slot for reuse */
    
    /* Search for existing entry with matching pointer */
    for (int i = 0; i < g_stringbuffer_fallback_count; i++) {
        if (g_stringbuffer_fallback[i].obj_ptr == obj) {
            /* Found entry with matching pointer - this is the same object instance */
            
            /* Validate class type for safety */
            bool class_valid = false;
            if (obj->header.clazz && obj->header.clazz->class_name) {
                const char* class_name = obj->header.clazz->class_name;
                if (strcmp(class_name, "java/lang/StringBuffer") == 0 || 
                    strcmp(class_name, "java/lang/StringBuilder") == 0) {
                    class_valid = true;
                }
            }
            
            if (class_valid) {
                /* Same object - return existing entry */
                return &g_stringbuffer_fallback[i];
            }
            
            /* Class changed! This shouldn't happen, but handle it */
            
            if (g_stringbuffer_fallback[i].buffer) {
                free(g_stringbuffer_fallback[i].buffer);
            }
            g_stringbuffer_fallback[i].obj_ptr = NULL;
            g_stringbuffer_fallback[i].buffer = NULL;
            g_stringbuffer_fallback[i].length = 0;
            g_stringbuffer_fallback[i].capacity = 0;
            g_stringbuffer_fallback[i].alloc_id = 0;
            free_slot = &g_stringbuffer_fallback[i];  /* Remember for reuse */
            break;
        }
        /* Remember first free slot */
        if (g_stringbuffer_fallback[i].obj_ptr == NULL && free_slot == NULL) {
            free_slot = &g_stringbuffer_fallback[i];
        }
    }
    
    /* If we found a free slot, reuse it */
    if (free_slot) {
        free_slot->obj_ptr = obj;
        free_slot->buffer = NULL;
        free_slot->length = 0;
        free_slot->capacity = 0;
        free_slot->alloc_id = obj_id;
        return free_slot;
    }
    
    /* Initialize dynamic array if needed */
    if (g_stringbuffer_fallback == NULL) {
        g_stringbuffer_fallback_capacity = STRINGBUFFER_INITIAL_ENTRIES;
        g_stringbuffer_fallback = (StringBufferFallback*)calloc(g_stringbuffer_fallback_capacity, sizeof(StringBufferFallback));
        if (!g_stringbuffer_fallback) return NULL;
    }
    
    /* Expand array if needed */
    if (g_stringbuffer_fallback_count >= g_stringbuffer_fallback_capacity) {
        int new_capacity = g_stringbuffer_fallback_capacity * 2;
        StringBufferFallback* new_array = (StringBufferFallback*)realloc(g_stringbuffer_fallback, new_capacity * sizeof(StringBufferFallback));
        if (!new_array) return NULL;
        
        /* Zero-initialize new entries */
        memset(new_array + g_stringbuffer_fallback_capacity, 0, 
               (new_capacity - g_stringbuffer_fallback_capacity) * sizeof(StringBufferFallback));
        
        g_stringbuffer_fallback = new_array;
        g_stringbuffer_fallback_capacity = new_capacity;
    }
    
    /* Create new entry */
    StringBufferFallback* entry = &g_stringbuffer_fallback[g_stringbuffer_fallback_count++];
    entry->obj_ptr = obj;
    entry->buffer = NULL;
    entry->length = 0;
    entry->capacity = 0;
    entry->alloc_id = obj_id;
    return entry;
}

/* Helper: Reset StringBuffer to empty state (called from constructor)
 * ИСПРАВЛЕНО: Всегда очищаем буфер при reset, так как это вызывается из конструктора
 */
static void stringbuffer_reset(JavaObject* obj) {
    if (!obj) return;
    
    if (stringbuffer_has_enough_fields(obj)) {
        /* Reset inline fields */
        if (obj->fields[0].ref) {
            /* Keep buffer but reset length */
            obj->fields[1].i = 0;
        }
    } else {
        /* Reset fallback storage */
        StringBufferFallback* fallback = stringbuffer_get_fallback(obj);
        if (fallback) {
            /* ВАЖНО: Конструктор всегда должен начинать с чистого буфера.
             * Если буфер уже существует, очищаем его для повторного использования.
             * Это исправляет проблему накопления строк при повторном использовании
             * объектов StringBuffer по тому же адресу памяти. */
            fallback->length = 0;
            if (fallback->buffer) {
                fallback->buffer[0] = '\0';  /* Null-terminate empty string */
            }
        }
    }
    NATIVE_DEBUG("StringBuffer reset: obj=%p", (void*)obj);
}

static char* stringbuffer_get_buffer(JavaObject* obj) {
    if (!obj) return NULL;
    
    /* Check if object has enough fields for inline storage */
    if (stringbuffer_has_enough_fields(obj)) {
        /* Use inline fields */
        if (obj->fields[0].ref == NULL) {
            obj->fields[0].ref = calloc(1, 1024);
            obj->fields[1].i = 0;
            obj->fields[2].i = 1024;
        }
        return (char*)obj->fields[0].ref;
    } else {
        /* Use fallback storage */
        StringBufferFallback* fallback = stringbuffer_get_fallback(obj);
        if (!fallback) return NULL;
        
        if (fallback->buffer == NULL) {
            fallback->buffer = calloc(1, 1024);
            fallback->length = 0;
            fallback->capacity = 1024;
        }
        return fallback->buffer;
    }
}

static int* stringbuffer_get_length_ptr(JavaObject* obj) {
    if (!obj) return NULL;
    
    if (stringbuffer_has_enough_fields(obj)) {
        return &obj->fields[1].i;
    } else {
        StringBufferFallback* fallback = stringbuffer_get_fallback(obj);
        return fallback ? &fallback->length : NULL;
    }
}

static int* stringbuffer_get_capacity_ptr(JavaObject* obj) {
    if (!obj) return NULL;
    
    if (stringbuffer_has_enough_fields(obj)) {
        return &obj->fields[2].i;
    } else {
        StringBufferFallback* fallback = stringbuffer_get_fallback(obj);
        return fallback ? &fallback->capacity : NULL;
    }
}

/* Helper: Update buffer pointer after realloc */
static void stringbuffer_set_buffer(JavaObject* obj, char* buffer) {
    if (!obj) return;
    
    if (stringbuffer_has_enough_fields(obj)) {
        obj->fields[0].ref = buffer;
    } else {
        StringBufferFallback* fallback = stringbuffer_get_fallback(obj);
        if (fallback) fallback->buffer = buffer;
    }
}

/* StringBuffer.append(String) */
static JavaValue native_stringbuffer_append_string(JVM* jvm, JavaThread* thread,
JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaString* str = (JavaString*)args[1].ref;

    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }

    char* buffer = stringbuffer_get_buffer(obj);
    int* len_ptr = stringbuffer_get_length_ptr(obj);
    int* cap_ptr = stringbuffer_get_capacity_ptr(obj);
    
    if (!buffer || !len_ptr || !cap_ptr) {
        return NATIVE_RETURN_OBJECT(obj);
    }

    /* Java spec: StringBuffer.append((String)null) appends "null" */
    const char* utf8;
    int str_len;
    if (str) {
        utf8 = string_utf8(jvm, str);
        if (!utf8) utf8 = "";
        str_len = strlen(utf8);
    } else {
        utf8 = "null";
        str_len = 4;
    }
    
    /* Expand buffer if needed */
    if (*len_ptr + str_len >= *cap_ptr) {
        int new_cap = (*cap_ptr + str_len) * 2;
        if (new_cap < 16) new_cap = 16;
        char* new_buffer = (char*)realloc(buffer, new_cap);
        if (!new_buffer) {
            native_throw_oome(jvm, thread);
            return NATIVE_RETURN_OBJECT(NULL);
        }
        stringbuffer_set_buffer(obj, new_buffer);
        buffer = new_buffer;
        *cap_ptr = new_cap;
    }
    
    memcpy(buffer + *len_ptr, utf8, str_len);
    *len_ptr += str_len;
    buffer[*len_ptr] = '\0';
    
    /* Return this for method chaining */
    return NATIVE_RETURN_OBJECT(obj);
}

/* Helper: Ensure buffer has enough capacity for additional bytes */
static bool stringbuffer_ensure_capacity(JavaObject* obj, int additional_bytes) {
    char* buffer = stringbuffer_get_buffer(obj);
    int* len_ptr = stringbuffer_get_length_ptr(obj);
    int* cap_ptr = stringbuffer_get_capacity_ptr(obj);
    
    if (!buffer || !len_ptr || !cap_ptr) return false;
    
    if (*len_ptr + additional_bytes >= *cap_ptr) {
        int new_cap = (*cap_ptr + additional_bytes) * 2;
        if (new_cap < 256) new_cap = 256;
        char* new_buffer = (char*)realloc(buffer, new_cap);
        if (!new_buffer) return false;
        stringbuffer_set_buffer(obj, new_buffer);
        *cap_ptr = new_cap;
    }
    return true;
}

/* StringBuffer.append(int) */
static JavaValue native_stringbuffer_append_int(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint value = args[1].i;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    /* Debug: verify object's class pointer */
    if (obj->header.clazz && obj->header.clazz->class_name) {
        NATIVE_DEBUG("StringBuffer.append(%d): obj=%p, class=%s", 
                value, (void*)obj, obj->header.clazz->class_name);
    } else {
        NATIVE_DEBUG("StringBuffer.append(%d): obj=%p, INVALID CLASS POINTER!",
                value, (void*)obj);
    }
    
    char num_buf[32];
    int written = snprintf(num_buf, sizeof(num_buf), "%d", value);
    
    if (written > 0 && stringbuffer_ensure_capacity(obj, written)) {
        char* buffer = stringbuffer_get_buffer(obj);
        int* len_ptr = stringbuffer_get_length_ptr(obj);
        if (buffer && len_ptr) {
            memcpy(buffer + *len_ptr, num_buf, written);
            *len_ptr += written;
            buffer[*len_ptr] = '\0';
        }
    }
    
    return NATIVE_RETURN_OBJECT(obj);
}

/* StringBuffer.append(long) */
static JavaValue native_stringbuffer_append_long(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jlong value = args[1].j;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    char num_buf[32];
    int written = snprintf(num_buf, sizeof(num_buf), "%lld", (long long)value);
    
    if (written > 0 && stringbuffer_ensure_capacity(obj, written)) {
        char* buffer = stringbuffer_get_buffer(obj);
        int* len_ptr = stringbuffer_get_length_ptr(obj);
        if (buffer && len_ptr) {
            memcpy(buffer + *len_ptr, num_buf, written);
            *len_ptr += written;
            buffer[*len_ptr] = '\0';
        }
    }
    
    return NATIVE_RETURN_OBJECT(obj);
}

/* StringBuffer.append(float) */
static JavaValue native_stringbuffer_append_float(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jfloat value = args[1].f;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    char num_buf[32];
    int written = snprintf(num_buf, sizeof(num_buf), "%g", (double)value);
    
    if (written > 0 && stringbuffer_ensure_capacity(obj, written)) {
        char* buffer = stringbuffer_get_buffer(obj);
        int* len_ptr = stringbuffer_get_length_ptr(obj);
        if (buffer && len_ptr) {
            memcpy(buffer + *len_ptr, num_buf, written);
            *len_ptr += written;
            buffer[*len_ptr] = '\0';
        }
    }
    
    return NATIVE_RETURN_OBJECT(obj);
}

/* StringBuffer.append(double) */
static JavaValue native_stringbuffer_append_double(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jdouble value = args[1].d;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    char num_buf[32];
    int written = snprintf(num_buf, sizeof(num_buf), "%g", value);
    
    if (written > 0 && stringbuffer_ensure_capacity(obj, written)) {
        char* buffer = stringbuffer_get_buffer(obj);
        int* len_ptr = stringbuffer_get_length_ptr(obj);
        if (buffer && len_ptr) {
            memcpy(buffer + *len_ptr, num_buf, written);
            *len_ptr += written;
            buffer[*len_ptr] = '\0';
        }
    }
    
    return NATIVE_RETURN_OBJECT(obj);
}

/* StringBuffer.append(boolean) */
static JavaValue native_stringbuffer_append_boolean(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jboolean value = args[1].i;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    const char* str = value ? "true" : "false";
    int str_len = strlen(str);
    
    if (stringbuffer_ensure_capacity(obj, str_len)) {
        char* buffer = stringbuffer_get_buffer(obj);
        int* len_ptr = stringbuffer_get_length_ptr(obj);
        if (buffer && len_ptr) {
            memcpy(buffer + *len_ptr, str, str_len);
            *len_ptr += str_len;
            buffer[*len_ptr] = '\0';
        }
    }
    
    return NATIVE_RETURN_OBJECT(obj);
}

/* StringBuffer.append(char) - ИСПРАВЛЕНО: Properly encode all Unicode characters as UTF-8 */
static JavaValue native_stringbuffer_append_char(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jchar value = (jchar)args[1].i;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    char* buffer = stringbuffer_get_buffer(obj);
    int* len_ptr = stringbuffer_get_length_ptr(obj);
    int* cap_ptr = stringbuffer_get_capacity_ptr(obj);
    
    if (!buffer || !len_ptr || !cap_ptr) {
        return NATIVE_RETURN_OBJECT(obj);
    }
    
    /* Calculate UTF-8 encoding size for this character:
     * - ASCII (U+0000 to U+007F): 1 byte
     * - U+0080 to U+07FF: 2 bytes
     * - U+0800 to U+FFFF: 3 bytes (includes Cyrillic: U+0400 to U+04FF)
     */
    int utf8_len;
    if (value < 0x80) {
        utf8_len = 1;
    } else if (value < 0x800) {
        utf8_len = 2;
    } else {
        utf8_len = 3;
    }
    
    /* Ensure buffer has enough capacity */
    if (*len_ptr + utf8_len >= *cap_ptr) {
        int new_cap = (*cap_ptr + utf8_len) * 2;
        if (new_cap < 256) new_cap = 256;
        char* new_buffer = (char*)realloc(buffer, new_cap);
        if (!new_buffer) {
            native_throw_oome(jvm, thread);
            return NATIVE_RETURN_OBJECT(NULL);
        }
        stringbuffer_set_buffer(obj, new_buffer);
        buffer = new_buffer;
        *cap_ptr = new_cap;
    }
    
    /* Encode character as UTF-8 */
    if (value < 0x80) {
        /* 1-byte: 0xxxxxxx */
        buffer[*len_ptr] = (char)value;
        (*len_ptr)++;
    } else if (value < 0x800) {
        /* 2-byte: 110xxxxx 10xxxxxx */
        buffer[*len_ptr] = (char)(0xC0 | (value >> 6));
        buffer[*len_ptr + 1] = (char)(0x80 | (value & 0x3F));
        (*len_ptr) += 2;
    } else {
        /* 3-byte: 1110xxxx 10xxxxxx 10xxxxxx */
        buffer[*len_ptr] = (char)(0xE0 | (value >> 12));
        buffer[*len_ptr + 1] = (char)(0x80 | ((value >> 6) & 0x3F));
        buffer[*len_ptr + 2] = (char)(0x80 | (value & 0x3F));
        (*len_ptr) += 3;
    }
    
    buffer[*len_ptr] = '\0';
    
    return NATIVE_RETURN_OBJECT(obj);
}

/* StringBuffer.append(Object) */
static JavaValue native_stringbuffer_append_object(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* value = (JavaObject*)args[1].ref;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    const char* str;
    int str_len;
    
    if (value) {
        /* Try to get proper string representation of the object */
        if (value->header.clazz) {
            const char* cname = value->header.clazz->class_name;
            if (cname && (strcmp(cname, "java/lang/StringBuffer") == 0 ||
                           strcmp(cname, "java/lang/StringBuilder") == 0)) {
                /* StringBuffer/StringBuilder: read its internal buffer */
                char* inner_buf = stringbuffer_get_buffer(value);
                int* inner_len = stringbuffer_get_length_ptr(value);
                if (inner_buf && inner_len && *inner_len > 0) {
                    str = inner_buf;
                    str_len = *inner_len;
                } else {
                    str = "";
                    str_len = 0;
                }
            } else if (cname && strcmp(cname, "java/lang/String") == 0) {
                /* String: convert from UTF-16 to UTF-8 */
                const jchar* chars = string_chars((JavaString*)value);
                int slen = string_length((JavaString*)value);
                if (chars && slen > 0) {
                    /* Convert UTF-16 to UTF-8 (ASCII fast path) */
                    static char utf8_buf[2048];
                    int u8_len = 0;
                    for (int ci = 0; ci < slen && u8_len < (int)sizeof(utf8_buf) - 4; ci++) {
                        jchar ch = chars[ci];
                        if (ch < 0x80) {
                            utf8_buf[u8_len++] = (char)ch;
                        } else if (ch < 0x800) {
                            utf8_buf[u8_len++] = (char)(0xC0 | (ch >> 6));
                            utf8_buf[u8_len++] = (char)(0x80 | (ch & 0x3F));
                        } else {
                            utf8_buf[u8_len++] = (char)(0xE0 | (ch >> 12));
                            utf8_buf[u8_len++] = (char)(0x80 | ((ch >> 6) & 0x3F));
                            utf8_buf[u8_len++] = (char)(0x80 | (ch & 0x3F));
                        }
                    }
                    str = utf8_buf;
                    str_len = u8_len;
                } else {
                    str = "";
                    str_len = 0;
                }
            } else {
                /* Fallback: append class name */
                str = cname;
                str_len = strlen(cname);
            }
        } else {
            str = "Object";
            str_len = strlen(str);
        }
    } else {
        str = "null";
        str_len = strlen(str);
    }
    
    /* ИСПРАВЛЕНО: Проверяем capacity перед записью */
    if (stringbuffer_ensure_capacity(obj, str_len + 1)) {
        char* buffer = stringbuffer_get_buffer(obj);
        int* len_ptr = stringbuffer_get_length_ptr(obj);
        if (buffer && len_ptr) {
            memcpy(buffer + *len_ptr, str, str_len);
            *len_ptr += str_len;
            buffer[*len_ptr] = '\0';
        }
    }
    
    return NATIVE_RETURN_OBJECT(obj);
}

/* StringBuffer.toString() */
static JavaValue native_stringbuffer_toString(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    char* buffer = stringbuffer_get_buffer(obj);
    int* len_ptr = stringbuffer_get_length_ptr(obj);
    
    /* DEBUG: Log current state */
    
    if (buffer && len_ptr && *len_ptr >= 0) {
        /* Truncate to actual length */
        char saved = buffer[*len_ptr];
        buffer[*len_ptr] = '\0';
        JavaString* str = jvm_new_string(jvm, buffer);
        buffer[*len_ptr] = saved;
        
        /* NOTE: Do NOT clear the buffer after toString().
         * In standard Java, toString() does NOT modify the StringBuffer.
         * Games like Bounce Tales call toString() and then continue appending
         * to the same buffer. Clearing here causes ArrayIndexOutOfBoundsException
         * downstream when the expected content is missing. */
        /* *len_ptr = 0; buffer[0] = '\0'; -- DISABLED: was breaking games */
        
        return NATIVE_RETURN_OBJECT(str);
    }
    
    return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, ""));
}

/* StringBuffer.length() */
static JavaValue native_stringbuffer_length(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        return NATIVE_RETURN_INT(0);
    }
    
    int* len_ptr = stringbuffer_get_length_ptr(obj);
    return NATIVE_RETURN_INT(len_ptr ? *len_ptr : 0);
}

/* StringBuffer.setLength(int) */
static JavaValue native_stringbuffer_setlength(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint new_length = args[1].i;
    
    if (!obj) {
        return NATIVE_RETURN_VOID();
    }
    
    if (new_length < 0) {
        /* StringIndexOutOfBoundsException - but for simplicity, just ignore */
        return NATIVE_RETURN_VOID();
    }
    
    int* len_ptr = stringbuffer_get_length_ptr(obj);
    char* buffer = stringbuffer_get_buffer(obj);
    int* cap_ptr = stringbuffer_get_capacity_ptr(obj);
    
    if (len_ptr && buffer && cap_ptr) {
        if (new_length > *cap_ptr) {
            /* Need to expand buffer */
            int new_cap = new_length * 2;
            char* new_buffer = (char*)realloc(buffer, new_cap);
            if (new_buffer) {
                stringbuffer_set_buffer(obj, new_buffer);
                buffer = new_buffer;
                *cap_ptr = new_cap;
            }
        }
        
        /* If growing, fill with nulls */
        if (new_length > *len_ptr) {
            memset(buffer + *len_ptr, 0, new_length - *len_ptr);
        }
        
        *len_ptr = new_length;
        if (buffer) buffer[*len_ptr] = '\0';
    }
    
    return NATIVE_RETURN_VOID();
}

/* StringBuffer.<init>() - default constructor */
static JavaValue native_stringbuffer_init(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        return NATIVE_RETURN_VOID();
    }
    
    /* ALWAYS log StringBuffer creation for debugging */
    
    /* Reset the StringBuffer to empty state */
    stringbuffer_reset(obj);
    
    return NATIVE_RETURN_VOID();
}

/* StringBuffer.<init>(int) - constructor with initial capacity */
static JavaValue native_stringbuffer_init_capacity(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint capacity = args[1].i;
    
    if (!obj) {
        return NATIVE_RETURN_VOID();
    }
    
    NATIVE_DEBUG("StringBuffer.<init>(%d): obj=%p, resetting to empty", capacity, (void*)obj);
    
    /* Reset the StringBuffer to empty state */
    stringbuffer_reset(obj);
    
    /* Pre-allocate buffer with specified capacity */
    if (capacity > 0) {
        StringBufferFallback* fallback = stringbuffer_get_fallback(obj);
        if (fallback) {
            /* Allocate new buffer with specified capacity */
            if (fallback->buffer) {
                free(fallback->buffer);
            }
            fallback->buffer = (char*)calloc(1, capacity);
            if (fallback->buffer) {
                fallback->capacity = capacity;
                fallback->length = 0;
                fallback->buffer[0] = '\0';
            }
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* StringBuffer.<init>(String) - constructor with initial string */
static JavaValue native_stringbuffer_init_string(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaString* str = (JavaString*)args[1].ref;
    
    if (!obj) {
        return NATIVE_RETURN_VOID();
    }
    
    NATIVE_DEBUG("StringBuffer.<init>(String): obj=%p, str=%p", (void*)obj, (void*)str);
    
    /* Reset the StringBuffer to empty state */
    stringbuffer_reset(obj);
    
    /* Append the initial string if provided */
    if (str) {
        JavaValue append_args[2];
        append_args[0].ref = obj;
        append_args[1].ref = str;
        native_stringbuffer_append_string(jvm, thread, append_args, 2);
    }
    
    return NATIVE_RETURN_VOID();
}

/* StringBuffer.reverse() - reverses the character sequence */
static JavaValue native_stringbuffer_reverse(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    char* buffer = stringbuffer_get_buffer(obj);
    int* len_ptr = stringbuffer_get_length_ptr(obj);
    
    if (buffer && len_ptr && *len_ptr > 0) {
        int len = *len_ptr;
        for (int i = 0; i < len / 2; i++) {
            char tmp = buffer[i];
            buffer[i] = buffer[len - 1 - i];
            buffer[len - 1 - i] = tmp;
        }
    }
    
    return NATIVE_RETURN_OBJECT(obj);
}

/* StringBuffer.capacity() - returns the current capacity */
static JavaValue native_stringbuffer_capacity(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        return NATIVE_RETURN_INT(0);
    }
    
    int* cap_ptr = stringbuffer_get_capacity_ptr(obj);
    return NATIVE_RETURN_INT(cap_ptr ? *cap_ptr : 0);
}

/* StringBuffer.charAt(int) - returns character at index */
static JavaValue native_stringbuffer_charAt(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    
    if (!obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    
    char* buffer = stringbuffer_get_buffer(obj);
    int* len_ptr = stringbuffer_get_length_ptr(obj);
    
    if (!buffer || !len_ptr) {
        return NATIVE_RETURN_INT(0);
    }
    
    if (index < 0 || index >= *len_ptr) {
        /* StringIndexOutOfBoundsException - for simplicity return 0 */
        return NATIVE_RETURN_INT(0);
    }
    
    return NATIVE_RETURN_INT((jint)(unsigned char)buffer[index]);
}

/* StringBuffer.setCharAt(int, char) - sets character at index */
static JavaValue native_stringbuffer_setCharAt(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    jchar ch = (jchar)args[2].i;
    
    if (!obj) {
        return NATIVE_RETURN_VOID();
    }
    
    char* buffer = stringbuffer_get_buffer(obj);
    int* len_ptr = stringbuffer_get_length_ptr(obj);
    
    if (buffer && len_ptr && index >= 0 && index < *len_ptr) {
        buffer[index] = (char)ch;
    }
    
    return NATIVE_RETURN_VOID();
}

/* StringBuffer.insert(int, String) - inserts string at position */
static JavaValue native_stringbuffer_insert_string(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint offset = args[1].i;
    JavaString* str = (JavaString*)args[2].ref;
    
    if (!obj) {
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    char* buffer = stringbuffer_get_buffer(obj);
    int* len_ptr = stringbuffer_get_length_ptr(obj);
    int* cap_ptr = stringbuffer_get_capacity_ptr(obj);
    
    if (!buffer || !len_ptr || !cap_ptr) {
        return NATIVE_RETURN_OBJECT(obj);
    }
    
    /* Get string content */
    const char* str_utf8 = str ? string_utf8(jvm, str) : "null";
    int str_len = str_utf8 ? strlen(str_utf8) : 0;
    
    /* Validate offset */
    if (offset < 0 || offset > *len_ptr) {
        return NATIVE_RETURN_OBJECT(obj);
    }
    
    /* Ensure capacity */
    int new_len = *len_ptr + str_len;
    if (new_len > *cap_ptr) {
        int new_cap = new_len * 2;
        char* new_buffer = (char*)realloc(buffer, new_cap);
        if (new_buffer) {
            stringbuffer_set_buffer(obj, new_buffer);
            buffer = new_buffer;
            *cap_ptr = new_cap;
        }
    }
    
    /* Shift existing content right */
    if (str_len > 0) {
        memmove(buffer + offset + str_len, buffer + offset, *len_ptr - offset);
        memcpy(buffer + offset, str_utf8, str_len);
        *len_ptr = new_len;
        buffer[*len_ptr] = '\0';
    }
    
    return NATIVE_RETURN_OBJECT(obj);
}

/* StringBuffer.delete(int, int) - deletes characters from start to end */
static JavaValue native_stringbuffer_delete(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint start = args[1].i;
    jint end = args[2].i;
    
    if (!obj) {
        return NATIVE_RETURN_OBJECT(NULL);
    }
    
    char* buffer = stringbuffer_get_buffer(obj);
    int* len_ptr = stringbuffer_get_length_ptr(obj);
    
    if (!buffer || !len_ptr) {
        return NATIVE_RETURN_OBJECT(obj);
    }
    
    int len = *len_ptr;
    
    /* Validate bounds */
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (start >= end) {
        return NATIVE_RETURN_OBJECT(obj);
    }
    
    /* Shift content left */
    int del_len = end - start;
    memmove(buffer + start, buffer + end, len - end);
    *len_ptr = len - del_len;
    buffer[*len_ptr] = '\0';
    
    return NATIVE_RETURN_OBJECT(obj);
}

void init_java_lang_stringbuffer(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/lang/StringBuffer", "<init>", "()V", native_stringbuffer_init},
        {"java/lang/StringBuffer", "<init>", "(I)V", native_stringbuffer_init_capacity},
        {"java/lang/StringBuffer", "<init>", "(Ljava/lang/String;)V", native_stringbuffer_init_string},
        {"java/lang/StringBuffer", "append", "(Ljava/lang/String;)Ljava/lang/StringBuffer;", native_stringbuffer_append_string},
        {"java/lang/StringBuffer", "append", "(I)Ljava/lang/StringBuffer;", native_stringbuffer_append_int},
        {"java/lang/StringBuffer", "append", "(J)Ljava/lang/StringBuffer;", native_stringbuffer_append_long},
        {"java/lang/StringBuffer", "append", "(F)Ljava/lang/StringBuffer;", native_stringbuffer_append_float},
        {"java/lang/StringBuffer", "append", "(D)Ljava/lang/StringBuffer;", native_stringbuffer_append_double},
        {"java/lang/StringBuffer", "append", "(Z)Ljava/lang/StringBuffer;", native_stringbuffer_append_boolean},
        {"java/lang/StringBuffer", "append", "(C)Ljava/lang/StringBuffer;", native_stringbuffer_append_char},
        {"java/lang/StringBuffer", "append", "(Ljava/lang/Object;)Ljava/lang/StringBuffer;", native_stringbuffer_append_object},
        {"java/lang/StringBuffer", "toString", "()Ljava/lang/String;", native_stringbuffer_toString},
        {"java/lang/StringBuffer", "length", "()I", native_stringbuffer_length},
        {"java/lang/StringBuffer", "setLength", "(I)V", native_stringbuffer_setlength},
        {"java/lang/StringBuffer", "reverse", "()Ljava/lang/StringBuffer;", native_stringbuffer_reverse},
        {"java/lang/StringBuffer", "capacity", "()I", native_stringbuffer_capacity},
        {"java/lang/StringBuffer", "charAt", "(I)C", native_stringbuffer_charAt},
        {"java/lang/StringBuffer", "setCharAt", "(IC)V", native_stringbuffer_setCharAt},
        {"java/lang/StringBuffer", "insert", "(ILjava/lang/String;)Ljava/lang/StringBuffer;", native_stringbuffer_insert_string},
        {"java/lang/StringBuffer", "delete", "(II)Ljava/lang/StringBuffer;", native_stringbuffer_delete},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered java/lang/StringBuffer native methods");
}

/*
 * java.lang.StringBuilder native methods
 * Same implementation as StringBuffer (uses StringBuffer handlers)
 */

void init_java_lang_stringbuilder(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/lang/StringBuilder", "<init>", "()V", native_stringbuffer_init},
        {"java/lang/StringBuilder", "<init>", "(I)V", native_stringbuffer_init_capacity},
        {"java/lang/StringBuilder", "<init>", "(Ljava/lang/String;)V", native_stringbuffer_init_string},
        {"java/lang/StringBuilder", "append", "(Ljava/lang/String;)Ljava/lang/StringBuilder;", native_stringbuffer_append_string},
        {"java/lang/StringBuilder", "append", "(I)Ljava/lang/StringBuilder;", native_stringbuffer_append_int},
        {"java/lang/StringBuilder", "append", "(J)Ljava/lang/StringBuilder;", native_stringbuffer_append_long},
        {"java/lang/StringBuilder", "append", "(F)Ljava/lang/StringBuilder;", native_stringbuffer_append_float},
        {"java/lang/StringBuilder", "append", "(D)Ljava/lang/StringBuilder;", native_stringbuffer_append_double},
        {"java/lang/StringBuilder", "append", "(Z)Ljava/lang/StringBuilder;", native_stringbuffer_append_boolean},
        {"java/lang/StringBuilder", "append", "(C)Ljava/lang/StringBuilder;", native_stringbuffer_append_char},
        {"java/lang/StringBuilder", "append", "(Ljava/lang/Object;)Ljava/lang/StringBuilder;", native_stringbuffer_append_object},
        {"java/lang/StringBuilder", "toString", "()Ljava/lang/String;", native_stringbuffer_toString},
        {"java/lang/StringBuilder", "length", "()I", native_stringbuffer_length},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered java/lang/StringBuilder native methods");
}

/*
 * java.util.Vector native methods
 */

static void vector_init_internal(JVM* jvm, JavaObject* vec, int initialCapacity) {
    if (!vec) return;
    
    /* ИСПРАВЛЕНО: Используем native_set_field_value */
    int capacity = initialCapacity > 0 ? initialCapacity : 10;
    JavaArray* arr = jvm_new_array(jvm, DESC_OBJECT, capacity, NULL);
    
    JavaValue arr_val = { .ref = arr };
    JavaValue count_val = { .i = 0 };
    
    native_set_field_value(vec, "elementData", arr_val);
    native_set_field_value(vec, "elementCount", count_val);
    
    NATIVE_DEBUG("init: initialized elementData with capacity %d", capacity);
}

static JavaValue native_vector_init(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    NATIVE_DEBUG("<init>(): initializing vector %p", (void*)vec);
    vector_init_internal(jvm, vec, 10);
    return NATIVE_RETURN_VOID();
}

/* Vector(int initialCapacity) constructor */
static JavaValue native_vector_init_int(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    jint initialCapacity = args[1].i;
    NATIVE_DEBUG("<init>(%d): initializing vector %p", initialCapacity, (void*)vec);
    vector_init_internal(jvm, vec, initialCapacity);
    return NATIVE_RETURN_VOID();
}

/* Vector(int initialCapacity, int capacityIncrement) constructor */
static JavaValue native_vector_init_int_int(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    jint initialCapacity = args[1].i;
    jint capacityIncrement = args[2].i;
    (void)capacityIncrement; /* We don't use this yet */
    NATIVE_DEBUG("<init>(%d, %d): initializing vector %p", initialCapacity, capacityIncrement, (void*)vec);
    vector_init_internal(jvm, vec, initialCapacity);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_vector_add(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    JavaValue element = args[1];
    
    if (!vec) return NATIVE_RETURN_INT(1);
    
    /* ИСПРАВЛЕНО: Используем native_get/set_field_value */
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    if (arr) {
        /* Grow array if needed */
        if (count >= arr->length) {
            JavaArray* new_arr = jvm_new_array(jvm, DESC_OBJECT, arr->length * 2 + 1, NULL);
            if (new_arr) {
                /* CRITICAL FIX: Object arrays store void* pointers, not JavaValue structs */
                memcpy(array_data(new_arr), array_data(arr), arr->length * sizeof(void*));
                arr = new_arr;
                /* Update elementData field */
                JavaValue arr_val = { .ref = arr };
                native_set_field_value(vec, "elementData", arr_val);
            }
        }
        if (count < arr->length) {
            /* CRITICAL FIX: Use array_set_ref for object arrays */
            array_set_ref(arr, count, element.ref);
            JavaValue count_val = { .i = count + 1 };
            native_set_field_value(vec, "elementCount", count_val);
        }
    }
    
    return NATIVE_RETURN_INT(1);
}

/* addElement is the same as add but returns void */
static JavaValue native_vector_add_element(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    JavaValue element = args[1];
    
    NATIVE_DEBUG("addElement: obj=%p, element=%p", (void*)vec, element.ref);
    
    if (!vec) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: Используем native_get/set_field_value */
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    /* Auto-initialize if elementData is NULL (constructor might not have been called) */
    if (!arr) {
        NATIVE_DEBUG("addElement: auto-initializing elementData");
        arr = jvm_new_array(jvm, DESC_OBJECT, 10, NULL);
        JavaValue arr_val = { .ref = arr };
        native_set_field_value(vec, "elementData", arr_val);
    }
    
    if (arr) {
        /* Grow array if needed */
        if (count >= arr->length) {
            int new_size = arr->length * 2 + 1;
            JavaArray* new_arr = jvm_new_array(jvm, DESC_OBJECT, new_size, NULL);
            if (new_arr) {
                /* CRITICAL FIX: Object arrays store void* pointers, not JavaValue structs */
                memcpy(array_data(new_arr), array_data(arr), arr->length * sizeof(void*));
                arr = new_arr;
                /* Update elementData field */
                JavaValue arr_val = { .ref = arr };
                native_set_field_value(vec, "elementData", arr_val);
            }
        }
        if (count < arr->length) {
            /* CRITICAL FIX: Use array_set_ref for object arrays */
            array_set_ref(arr, count, element.ref);
            JavaValue count_val = { .i = count + 1 };
            native_set_field_value(vec, "elementCount", count_val);
            NATIVE_DEBUG("addElement: stored element at index %d, new count=%d", count, count + 1);
        }
    } else {
        NATIVE_DEBUG("addElement: ERROR - arr is NULL");
    }
    
    return NATIVE_RETURN_VOID();
}

/* Vector.insertElementAt(Object obj, int index) - insert at specific position */
static JavaValue native_vector_insertElementAt(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    JavaValue element = args[1];
    jint index = args[2].i;
    
    NATIVE_DEBUG("insertElementAt: obj=%p, index=%d, element=%p", 
            (void*)vec, index, element.ref);
    
    if (!vec) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: Используем native_get/set_field_value */
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    if (!arr) return NATIVE_RETURN_VOID();
    
    /* Bounds check */
    if (index < 0 || index > count) {
        /* Should throw ArrayIndexOutOfBoundsException */
        return NATIVE_RETURN_VOID();
    }
    
    /* Grow array if needed */
    if (count >= arr->length) {
        JavaArray* new_arr = jvm_new_array(jvm, DESC_OBJECT, arr->length * 2 + 1, NULL);
        if (new_arr) {
            memcpy(array_data(new_arr), array_data(arr), arr->length * sizeof(void*));
            arr = new_arr;
            JavaValue arr_val = { .ref = arr };
            native_set_field_value(vec, "elementData", arr_val);
        }
    }
    
    /* Shift elements to make room */
    if (index < count) {
        void** data = (void**)array_data(arr);
        for (int i = count; i > index; i--) {
            data[i] = data[i - 1];
        }
    }
    
    /* Insert element */
    array_set_ref(arr, index, element.ref);
    JavaValue count_val = { .i = count + 1 };
    native_set_field_value(vec, "elementCount", count_val);
    
    return NATIVE_RETURN_VOID();
}

/* Vector.setElementAt(Object obj, int index) - replace element at position */
static JavaValue native_vector_setElementAt(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    JavaValue element = args[1];
    jint index = args[2].i;
    
    if (!vec) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    if (arr && index >= 0 && index < count) {
        array_set_ref(arr, index, element.ref);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_vector_get(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    
    if (!vec) {
        NATIVE_DEBUG("Vector.get: null vector");
        return NATIVE_RETURN_NULL();
    }
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    NATIVE_DEBUG("Vector.get: index=%d, count=%d, arr=%p", index, count, (void*)arr);
    
    if (index < 0 || index >= count) {
        /* Throw ArrayIndexOutOfBoundsException */
        native_throw_aioobe(jvm, thread, index);
        return NATIVE_RETURN_NULL();
    }
    
    if (arr) {
        /* CRITICAL FIX: Use array_get_ref for object arrays */
        void* elem = array_get_ref(arr, index);
        NATIVE_DEBUG("Vector.get: returning element at index %d = %p", index, elem);
        return NATIVE_RETURN_OBJECT(elem);
    }
    
    NATIVE_DEBUG("Vector.get: null array");
    return NATIVE_RETURN_NULL();
}

static JavaValue native_vector_size(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    
    if (!vec) return NATIVE_RETURN_INT(0);
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    jint count = native_get_field_value(vec, "elementCount").i;
    return NATIVE_RETURN_INT(count);
}

static JavaValue native_vector_isEmpty(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    
    if (!vec) return NATIVE_RETURN_INT(1);
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    return NATIVE_RETURN_INT(native_get_field_value(vec, "elementCount").i == 0 ? 1 : 0);
}

/* Vector.removeElement(Object obj) - removes first occurrence of element */
static bool vector_objects_equal(JavaObject* a, JavaObject* b) {
    /* Same reference */
    if (a == b) return true;
    /* Both null */
    if (!a && !b) return true;
    /* One null */
    if (!a || !b) return false;
    
    /* Check if both are Strings - use string_equals */
    /* Native strings (OBJ_TYPE_STRING) have clazz = NULL, so check type first */
    extern void* g_heap_start;
    extern void* g_heap_end;
    
    bool a_is_string = false;
    bool b_is_string = false;
    
    /* Check if a is a string */
    if ((void*)a >= g_heap_start && (void*)a < g_heap_end) {
        GCObjectHeader* a_header = (GCObjectHeader*)((uint8_t*)a - sizeof(GCObjectHeader));
        if (a_header->type == OBJ_TYPE_STRING) {
            a_is_string = true;
        } else if (a_header->type == OBJ_TYPE_OBJECT) {
            JavaClass* a_class = a->header.clazz;
            if (a_class && a_class->class_name && 
                strcmp(a_class->class_name, "java/lang/String") == 0) {
                a_is_string = true;
            }
        }
    }
    
    /* Check if b is a string */
    if ((void*)b >= g_heap_start && (void*)b < g_heap_end) {
        GCObjectHeader* b_header = (GCObjectHeader*)((uint8_t*)b - sizeof(GCObjectHeader));
        if (b_header->type == OBJ_TYPE_STRING) {
            b_is_string = true;
        } else if (b_header->type == OBJ_TYPE_OBJECT) {
            JavaClass* b_class = b->header.clazz;
            if (b_class && b_class->class_name && 
                strcmp(b_class->class_name, "java/lang/String") == 0) {
                b_is_string = true;
            }
        }
    }
    
    /* If both are strings, compare them using string_equals */
    if (a_is_string && b_is_string) {
        return string_equals((JavaString*)a, (JavaString*)b);
    }
    
    /* For other objects, use reference comparison (default Object.equals) */
    return false;
}

static JavaValue native_vector_removeElement(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    JavaObject* elem = (JavaObject*)args[1].ref;
    
    if (!vec) return NATIVE_RETURN_INT(0);
    
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    if (!arr || count == 0) return NATIVE_RETURN_INT(0);
    
    /* Search for element using equals comparison */
    void** data = (void**)array_data(arr);
    for (int i = 0; i < count; i++) {
        if (vector_objects_equal((JavaObject*)data[i], elem)) {
            /* Found - shift remaining elements left */
            for (int j = i; j < count - 1; j++) {
                data[j] = data[j + 1];
            }
            data[count - 1] = NULL;
            
            /* Update count */
            int new_count = count - 1;
            JavaValue count_val = { .i = new_count };
            native_set_field_value(vec, "elementCount", count_val);
            
            return NATIVE_RETURN_INT(1);  /* true - element was removed */
        }
    }
    
    return NATIVE_RETURN_INT(0);  /* false - element not found */
}

/* Vector.removeAllElements() - removes all elements */
static JavaValue native_vector_removeAllElements(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    
    if (!vec) return NATIVE_RETURN_VOID();
    
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    if (arr && count > 0) {
        /* Clear all elements */
        void** data = (void**)array_data(arr);
        for (int i = 0; i < count; i++) {
            data[i] = NULL;
        }
    }
    
    /* Set count to 0 */
    JavaValue count_val = { .i = 0 };
    native_set_field_value(vec, "elementCount", count_val);
    
    return NATIVE_RETURN_VOID();
}

/* Vector.contains(Object elem) - checks if element exists */
static JavaValue native_vector_contains(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    JavaObject* elem = (JavaObject*)args[1].ref;
    
    if (!vec) return NATIVE_RETURN_INT(0);
    
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    if (!arr || count == 0) return NATIVE_RETURN_INT(0);
    
    /* Search for element using equals comparison */
    void** data = (void**)array_data(arr);
    for (int i = 0; i < count; i++) {
        if (vector_objects_equal((JavaObject*)data[i], elem)) {
            return NATIVE_RETURN_INT(1);  /* true - found */
        }
    }
    
    return NATIVE_RETURN_INT(0);  /* false - not found */
}

/* Vector.copyInto(Object[] array) - copies elements into array */
static JavaValue native_vector_copyInto(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    JavaArray* dest_array = (JavaArray*)args[1].ref;
    
    if (!vec || !dest_array) {
        NATIVE_DEBUG("copyInto: null parameter (vec=%p, array=%p)", 
                (void*)vec, (void*)dest_array);
        return NATIVE_RETURN_VOID();
    }
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    JavaArray* src_array = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int element_count = native_get_field_value(vec, "elementCount").i;
    
    if (!src_array) {
        NATIVE_DEBUG("copyInto: no elementData field found");
        return NATIVE_RETURN_VOID();
    }
    
    int copy_count = element_count;
    if (copy_count > (int)dest_array->length) {
        copy_count = dest_array->length;
    }
    
    NATIVE_DEBUG("copyInto: copying %d elements (vector size=%d, array len=%u)",
            copy_count, element_count, dest_array->length);
    
    /* Copy elements from source array to destination array using helper functions */
    for (int i = 0; i < copy_count; i++) {
        void* ref = array_get_ref(src_array, i);
        array_set_ref(dest_array, i, ref);
    }
    
    return NATIVE_RETURN_VOID();
}

/* Vector.elements() - returns an Enumeration over the vector elements */
extern JavaClass* get_or_create_stub_class(JVM* jvm, const char* class_name);

static JavaValue native_vector_elements(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    
    if (!vec) {
        NATIVE_DEBUG("elements: null vector");
        return NATIVE_RETURN_NULL();
    }
    
    /* ИСПРАВЛЕНО: Используем native_get_field_value */
    JavaArray* src_array = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int element_count = native_get_field_value(vec, "elementCount").i;
    
    NATIVE_DEBUG("elements: src_array=%p, element_count=%d", 
            (void*)src_array, element_count);
    
    if (!src_array) {
        NATIVE_DEBUG("elements: WARNING - elementData is NULL!");
        /* Return an empty enumeration */
        element_count = 0;
    }
    
    /* ИСПРАВЛЕНО: Используем конкретный класс вместо интерфейса Enumeration.
     * java/util/Enumeration - это интерфейс, и создание объекта с ним вызывает проблемы.
     * Создаем специальный класс VectorEnumeration.
     */
    JavaClass* iter_class = get_or_create_stub_class(jvm, "java/util/VectorEnumeration");
    if (!iter_class) {
        ERROR_LOG("Failed to create VectorEnumeration class");
        return NATIVE_RETURN_NULL();
    }
    
    /* Ensure the class has fields for enumeration state */
    if (iter_class->fields_count == 0) {
        iter_class->fields = (JavaField*)calloc(3, sizeof(JavaField));
        iter_class->fields[0].name = strdup("elements");
        iter_class->fields[0].descriptor = strdup("[Ljava/lang/Object;");
        iter_class->fields[1].name = strdup("count");
        iter_class->fields[1].descriptor = strdup("I");
        iter_class->fields[2].name = strdup("index");
        iter_class->fields[2].descriptor = strdup("I");
        iter_class->fields_count = 3;
        iter_class->instance_size = sizeof(ObjectHeader) + 3 * sizeof(JavaValue);
        
        /* Mark as initialized to avoid <clinit> calls */
        iter_class->initialized = true;
    }
    
    JavaObject* iter_obj = jvm_new_object(jvm, iter_class);
    if (iter_obj && iter_class->fields_count >= 3) {
        /* ИСПРАВЛЕНО: Используем native_set_field_value для установки полей */
        JavaValue arr_val = { .ref = src_array };
        JavaValue count_val = { .i = element_count };
        JavaValue index_val = { .i = 0 };
        native_set_field_value(iter_obj, "elements", arr_val);
        native_set_field_value(iter_obj, "count", count_val);
        native_set_field_value(iter_obj, "index", index_val);
    }
    
    NATIVE_DEBUG("elements: returning enumeration object %p with %d elements (class=%s)", 
            (void*)iter_obj, element_count, iter_class->class_name);
    return NATIVE_RETURN_OBJECT(iter_obj);
}

/* Vector.indexOf(Object) - find first occurrence of element */
static JavaValue native_vector_indexOf(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    JavaObject* elem = (JavaObject*)args[1].ref;
    
    if (!vec) return NATIVE_RETURN_INT(-1);
    
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    if (!arr || count == 0) return NATIVE_RETURN_INT(-1);
    
    void** data = (void**)array_data(arr);
    for (int i = 0; i < count; i++) {
        if (vector_objects_equal((JavaObject*)data[i], elem)) {
            return NATIVE_RETURN_INT(i);
        }
    }
    
    return NATIVE_RETURN_INT(-1);
}

/* Vector.indexOf(Object, int) - find first occurrence of element starting from index */
static JavaValue native_vector_indexOf_from(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    JavaObject* elem = (JavaObject*)args[1].ref;
    jint start_index = args[2].i;
    
    if (!vec) return NATIVE_RETURN_INT(-1);
    
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    if (!arr || count == 0 || start_index < 0) return NATIVE_RETURN_INT(-1);
    
    if (start_index >= count) return NATIVE_RETURN_INT(-1);
    
    void** data = (void**)array_data(arr);
    for (int i = start_index; i < count; i++) {
        if (vector_objects_equal((JavaObject*)data[i], elem)) {
            return NATIVE_RETURN_INT(i);
        }
    }
    
    return NATIVE_RETURN_INT(-1);
}

/* Vector.lastIndexOf(Object) - find last occurrence of element */
static JavaValue native_vector_lastIndexOf(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    JavaObject* elem = (JavaObject*)args[1].ref;
    
    if (!vec) return NATIVE_RETURN_INT(-1);
    
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    if (!arr || count == 0) return NATIVE_RETURN_INT(-1);
    
    void** data = (void**)array_data(arr);
    for (int i = count - 1; i >= 0; i--) {
        if (vector_objects_equal((JavaObject*)data[i], elem)) {
            return NATIVE_RETURN_INT(i);
        }
    }
    
    return NATIVE_RETURN_INT(-1);
}

/* Vector.firstElement() - return first element */
static JavaValue native_vector_firstElement(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    
    if (!vec) return NATIVE_RETURN_NULL();
    
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    if (!arr || count == 0) {
        /* Should throw NoSuchElementException, return null for now */
        return NATIVE_RETURN_NULL();
    }
    
    return NATIVE_RETURN_OBJECT(array_get_ref(arr, 0));
}

/* Vector.lastElement() - return last element */
static JavaValue native_vector_lastElement(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    
    if (!vec) return NATIVE_RETURN_NULL();
    
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    if (!arr || count == 0) {
        /* Should throw NoSuchElementException, return null for now */
        return NATIVE_RETURN_NULL();
    }
    
    return NATIVE_RETURN_OBJECT(array_get_ref(arr, count - 1));
}

/* Vector.removeElementAt(int) - remove element at index */
static JavaValue native_vector_removeElementAt(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    
    if (!vec) return NATIVE_RETURN_VOID();
    
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    if (!arr) return NATIVE_RETURN_VOID();
    
    /* Bounds check */
    if (index < 0 || index >= count) {
        /* Should throw ArrayIndexOutOfBoundsException */
        return NATIVE_RETURN_VOID();
    }
    
    void** data = (void**)array_data(arr);
    
    /* Shift elements left */
    for (int i = index; i < count - 1; i++) {
        data[i] = data[i + 1];
    }
    data[count - 1] = NULL;
    
    /* Update count */
    JavaValue count_val = { .i = count - 1 };
    native_set_field_value(vec, "elementCount", count_val);
    
    return NATIVE_RETURN_VOID();
}

/* Vector.trimToSize() - trim capacity to size */
static JavaValue native_vector_trimToSize(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    
    if (!vec) return NATIVE_RETURN_VOID();
    
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    if (!arr || arr->length == (uint32_t)count) {
        /* Already at correct size */
        return NATIVE_RETURN_VOID();
    }
    
    /* Create new array with exact size */
    JavaArray* new_arr = jvm_new_array(jvm, DESC_OBJECT, count, NULL);
    if (new_arr && count > 0) {
        memcpy(array_data(new_arr), array_data(arr), count * sizeof(void*));
        JavaValue arr_val = { .ref = new_arr };
        native_set_field_value(vec, "elementData", arr_val);
    }
    
    return NATIVE_RETURN_VOID();
}

/* Vector.ensureCapacity(int) - ensure minimum capacity */
static JavaValue native_vector_ensureCapacity(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    jint min_capacity = args[1].i;
    
    if (!vec || min_capacity <= 0) return NATIVE_RETURN_VOID();
    
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    if (!arr) {
        arr = jvm_new_array(jvm, DESC_OBJECT, min_capacity, NULL);
        if (arr) {
            JavaValue arr_val = { .ref = arr };
            native_set_field_value(vec, "elementData", arr_val);
        }
        return NATIVE_RETURN_VOID();
    }
    
    if ((int)arr->length < min_capacity) {
        JavaArray* new_arr = jvm_new_array(jvm, DESC_OBJECT, min_capacity, NULL);
        if (new_arr) {
            memcpy(array_data(new_arr), array_data(arr), count * sizeof(void*));
            JavaValue arr_val = { .ref = new_arr };
            native_set_field_value(vec, "elementData", arr_val);
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Vector.setSize(int) - set new size */
static JavaValue native_vector_setSize(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* vec = (JavaObject*)args[0].ref;
    jint new_size = args[1].i;
    
    if (!vec || new_size < 0) return NATIVE_RETURN_VOID();
    
    JavaArray* arr = (JavaArray*)native_get_field_value(vec, "elementData").ref;
    int count = native_get_field_value(vec, "elementCount").i;
    
    if (new_size == count) {
        return NATIVE_RETURN_VOID();
    }
    
    if (!arr) {
        arr = jvm_new_array(jvm, DESC_OBJECT, new_size > 10 ? new_size : 10, NULL);
        if (arr) {
            JavaValue arr_val = { .ref = arr };
            native_set_field_value(vec, "elementData", arr_val);
        }
    } else if ((int)arr->length < new_size) {
        /* Need to grow */
        JavaArray* new_arr = jvm_new_array(jvm, DESC_OBJECT, new_size, NULL);
        if (new_arr) {
            memcpy(array_data(new_arr), array_data(arr), count * sizeof(void*));
            JavaValue arr_val = { .ref = new_arr };
            native_set_field_value(vec, "elementData", arr_val);
            arr = new_arr;
        }
    } else if (new_size < count) {
        /* Shrinking - clear excess elements */
        void** data = (void**)array_data(arr);
        for (int i = new_size; i < count; i++) {
            data[i] = NULL;
        }
    }
    
    /* Update count */
    JavaValue count_val = { .i = new_size };
    native_set_field_value(vec, "elementCount", count_val);
    
    return NATIVE_RETURN_VOID();
}

void init_java_util_vector(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/util/Vector", "<init>", "()V", native_vector_init},
        {"java/util/Vector", "<init>", "(I)V", native_vector_init_int},
        {"java/util/Vector", "<init>", "(II)V", native_vector_init_int_int},
        {"java/util/Vector", "add", "(Ljava/lang/Object;)Z", native_vector_add},
        {"java/util/Vector", "addElement", "(Ljava/lang/Object;)V", native_vector_add_element},
        {"java/util/Vector", "insertElementAt", "(Ljava/lang/Object;I)V", native_vector_insertElementAt},
        {"java/util/Vector", "setElementAt", "(Ljava/lang/Object;I)V", native_vector_setElementAt},
        {"java/util/Vector", "get", "(I)Ljava/lang/Object;", native_vector_get},
        {"java/util/Vector", "elementAt", "(I)Ljava/lang/Object;", native_vector_get},
        {"java/util/Vector", "size", "()I", native_vector_size},
        {"java/util/Vector", "isEmpty", "()Z", native_vector_isEmpty},
        {"java/util/Vector", "removeElement", "(Ljava/lang/Object;)Z", native_vector_removeElement},
        {"java/util/Vector", "removeAllElements", "()V", native_vector_removeAllElements},
        {"java/util/Vector", "contains", "(Ljava/lang/Object;)Z", native_vector_contains},
        {"java/util/Vector", "copyInto", "([Ljava/lang/Object;)V", native_vector_copyInto},
        {"java/util/Vector", "elements", "()Ljava/util/Enumeration;", native_vector_elements},
        {"java/util/Vector", "indexOf", "(Ljava/lang/Object;)I", native_vector_indexOf},
        {"java/util/Vector", "indexOf", "(Ljava/lang/Object;I)I", native_vector_indexOf_from},
        {"java/util/Vector", "lastIndexOf", "(Ljava/lang/Object;)I", native_vector_lastIndexOf},
        {"java/util/Vector", "firstElement", "()Ljava/lang/Object;", native_vector_firstElement},
        {"java/util/Vector", "lastElement", "()Ljava/lang/Object;", native_vector_lastElement},
        {"java/util/Vector", "removeElementAt", "(I)V", native_vector_removeElementAt},
        {"java/util/Vector", "trimToSize", "()V", native_vector_trimToSize},
        {"java/util/Vector", "ensureCapacity", "(I)V", native_vector_ensureCapacity},
        {"java/util/Vector", "setSize", "(I)V", native_vector_setSize},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered java/util/Vector native methods");
}

/* Enumeration methods - used by Vector.elements() */
static JavaValue native_enumeration_hasMoreElements(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* enum_obj = (JavaObject*)args[0].ref;
    
    if (!enum_obj) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* ИСПРАВЛЕНО: используем native_get_field_value */
    JavaArray* elements = (JavaArray*)native_get_field_value(enum_obj, "elements").ref;
    jint count = native_get_field_value(enum_obj, "count").i;
    jint index = native_get_field_value(enum_obj, "index").i;
    
    jboolean result = (elements && index < count) ? JNI_TRUE : JNI_FALSE;
    return NATIVE_RETURN_INT(result);
}

static JavaValue native_enumeration_nextElement(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* enum_obj = (JavaObject*)args[0].ref;
    
    if (!enum_obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* ИСПРАВЛЕНО: используем native_get_field_value */
    JavaArray* elements = (JavaArray*)native_get_field_value(enum_obj, "elements").ref;
    jint count = native_get_field_value(enum_obj, "count").i;
    jint index = native_get_field_value(enum_obj, "index").i;
    
    JavaClass* clazz = enum_obj->header.clazz;
    
    if (!elements || index >= count) {
        /* Should throw NoSuchElementException, but for compatibility return null */
        return NATIVE_RETURN_NULL();
    }
    
    /* Get the element at current index using array_get_ref for object arrays */
    void* elem_ref = array_get_ref(elements, index);
    
    /* ИСПРАВЛЕНО: используем native_set_field_value для обновления index */
    JavaValue new_index = { .i = index + 1 };
    native_set_field_value(enum_obj, "index", new_index);
    
    /* Debug: check the element */
    if (elem_ref) {
        JavaObject* elem_obj = (JavaObject*)elem_ref;
        JavaClass* elem_class = elem_obj->header.clazz;
    } else {
    }
    
    return NATIVE_RETURN_OBJECT(elem_ref);
}

void init_java_util_enumeration(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/util/Enumeration", "hasMoreElements", "()Z", native_enumeration_hasMoreElements},
        {"java/util/Enumeration", "nextElement", "()Ljava/lang/Object;", native_enumeration_nextElement},
        /* ИСПРАВЛЕНО: Also register for VectorEnumeration class */
        {"java/util/VectorEnumeration", "hasMoreElements", "()Z", native_enumeration_hasMoreElements},
        {"java/util/VectorEnumeration", "nextElement", "()Ljava/lang/Object;", native_enumeration_nextElement},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered java/util/Enumeration and VectorEnumeration native methods");
}

/*
 * java.util.Hashtable native methods
 * 
 * ИСПРАВЛЕНО: Полноценная реализация Hashtable с поддержкой Integer/String/Long ключей
 * 
 * Проблема race condition: Игра вызывает get() до того, как put() завершился.
 * Решение: Возвращаем null-safe значения вместо NPE.
 */

/* Hashtable entry structure - stored as pairs in object array */
typedef struct {
    JavaObject* key;
    JavaObject* value;
    jint hash;
} HashtableEntry;

/* Native peer structure for Hashtable */
typedef struct {
    JavaObject* ht_obj;     /* Back-reference to Java Hashtable object for lookup */
    HashtableEntry* entries;
    int capacity;
    int count;
} HashtablePeer;

/* Helper: compute hash code for any key object */
static jint hashtable_compute_hash(JavaObject* key) {
    if (!key) return 0;
    
    JavaClass* key_class = key->header.clazz;
    if (!key_class || !key_class->class_name) return (jint)(intptr_t)key;
    
    /* Integer: use the int value directly */
    if (strcmp(key_class->class_name, "java/lang/Integer") == 0) {
        return native_get_field_value(key, "value").i;
    }
    
    /* Long: use the long value (xor high and low bits) */
    if (strcmp(key_class->class_name, "java/lang/Long") == 0) {
        jlong val = native_get_field_value(key, "value").j;
        return (jint)(val ^ (val >> 32));
    }
    
    /* String: use string hash */
    if (strcmp(key_class->class_name, "java/lang/String") == 0) {
        return string_hash((JavaString*)key);
    }
    
    /* Default: use object hashcode */
    return key->header.hashcode ? key->header.hashcode : (jint)(intptr_t)key;
}

/* Helper: compare two key objects for equality */
static bool hashtable_keys_equal(JavaObject* key1, JavaObject* key2) {
    if (key1 == key2) return true;
    if (!key1 || !key2) return false;
    
    JavaClass* class1 = key1->header.clazz;
    JavaClass* class2 = key2->header.clazz;
    
    if (!class1 || !class2) return false;
    
    /* Different classes can't be equal (except both are Numbers with same value) */
    const char* name1 = class1->class_name;
    const char* name2 = class2->class_name;
    
    if (!name1 || !name2) return false;
    
    /* Integer comparison */
    if (strcmp(name1, "java/lang/Integer") == 0 && strcmp(name2, "java/lang/Integer") == 0) {
        jint val1 = native_get_field_value(key1, "value").i;
        jint val2 = native_get_field_value(key2, "value").i;
        return val1 == val2;
    }
    
    /* Long comparison */
    if (strcmp(name1, "java/lang/Long") == 0 && strcmp(name2, "java/lang/Long") == 0) {
        jlong val1 = native_get_field_value(key1, "value").j;
        jlong val2 = native_get_field_value(key2, "value").j;
        return val1 == val2;
    }
    
    /* String comparison */
    if (strcmp(name1, "java/lang/String") == 0 && strcmp(name2, "java/lang/String") == 0) {
        return string_equals((JavaString*)key1, (JavaString*)key2);
    }
    
    return false;
}

/* Global hashtable peer storage (simple approach)
 * CRITICAL: These must NOT be static because the GC in heap.c needs to
 * access them to mark objects stored in native Hashtable entries.
 * This fixes the use-after-free bug where strings stored as Hashtable keys
 * were being garbage collected because GC didn't see them.
 */
#define MAX_HASHTABLES 4096
HashtablePeer g_hashtables[MAX_HASHTABLES];
int g_hashtable_count = 0;

/* Track which peers are alive (not freed) */
bool g_hashtable_peer_alive[MAX_HASHTABLES] = {false};

/* Helper function to find a free peer slot */
static int hashtable_find_free_slot(void) {
    for (int i = 0; i < MAX_HASHTABLES; i++) {
        if (!g_hashtable_peer_alive[i]) {
            return i;
        }
    }
    return -1;
}

/* Get or create peer for hashtable */
static HashtablePeer* hashtable_get_peer(JavaObject* ht) {
    /* First, search for existing peer by object pointer 
     * This is more reliable than using threshold field which Java may overwrite */
    for (int i = 0; i < g_hashtable_count; i++) {
        if (g_hashtable_peer_alive[i] && g_hashtables[i].ht_obj == ht) {
            return &g_hashtables[i];
        }
    }
    
    /* Find a free slot */
    int idx = hashtable_find_free_slot();
    if (idx < 0) {
        ERROR_LOG("No free Hashtable peer slots");
        return NULL;
    }
    
    /* Create new peer */
    g_hashtables[idx].ht_obj = ht;  /* Store object pointer for lookup */
    g_hashtables[idx].capacity = 16;
    g_hashtables[idx].count = 0;
    g_hashtables[idx].entries = (HashtableEntry*)calloc(16, sizeof(HashtableEntry));
    g_hashtable_peer_alive[idx] = true;
    
    /* Update g_hashtable_count to track highest used index */
    if (idx >= g_hashtable_count) {
        g_hashtable_count = idx + 1;
    }
    
    return &g_hashtables[idx];
}

/* Get peer for hashtable - uses object address for lookup */
static HashtablePeer* hashtable_ensure_peer(JavaObject* ht) {
    if (!ht) return NULL;
    
    /* Always search by object pointer first - this is the most reliable method */
    for (int i = 0; i < g_hashtable_count; i++) {
        if (g_hashtable_peer_alive[i] && g_hashtables[i].ht_obj == ht) {
            return &g_hashtables[i];
        }
    }
    
    /* Not found - create new peer */
    return hashtable_get_peer(ht);
}

/* Free native peer for a Hashtable - called by GC when Hashtable is collected */
void hashtable_free_peer(int idx) {
    if (idx < 0 || idx >= MAX_HASHTABLES || !g_hashtable_peer_alive[idx]) {
        return;
    }
    
    if (g_j2me_runtime_debug) fprintf(stderr, "[NATIVE] Freeing native Hashtable peer at idx %d\n", idx);
    
    if (g_hashtables[idx].entries) {
        free(g_hashtables[idx].entries);
        g_hashtables[idx].entries = NULL;
    }
    g_hashtables[idx].capacity = 0;
    g_hashtables[idx].count = 0;
    g_hashtable_peer_alive[idx] = false;
}

static JavaValue native_hashtable_init(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ht = (JavaObject*)args[0].ref;
    if (!ht) return NATIVE_RETURN_VOID();
    
    /* Create native peer */
    HashtablePeer* peer = hashtable_get_peer(ht);
    if (peer) {
        NATIVE_DEBUG("Hashtable init: peer created, capacity=%d", peer->capacity);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_hashtable_put(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* ht = (JavaObject*)args[0].ref;
    JavaObject* key = (JavaObject*)args[1].ref;
    JavaObject* value = (JavaObject*)args[2].ref;
    
    if (!ht) return NATIVE_RETURN_NULL();
    
    /* Hashtable throws NullPointerException for null key or null value */
    if (!key || !value) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Get or create peer */
    HashtablePeer* peer = hashtable_ensure_peer(ht);
    if (!peer) return NATIVE_RETURN_NULL();
    if (!peer->entries) {
        peer->capacity = 16;
        peer->entries = (HashtableEntry*)calloc(16, sizeof(HashtableEntry));
    }
    
    /* Compute hash */
    jint hash = hashtable_compute_hash(key);
    
    /* Find existing entry or empty slot */
    jint idx = (hash & 0x7FFFFFFF) % peer->capacity;
    
    /* Linear probing for collision resolution */
    for (int i = 0; i < peer->capacity; i++) {
        jint probe_idx = (idx + i) % peer->capacity;
        HashtableEntry* entry = &peer->entries[probe_idx];
        
        if (entry->key == NULL) {
            /* Empty slot - add new entry */
            entry->key = key;
            entry->value = value;
            entry->hash = hash;
            peer->count++;
            
            /* Update count field in Java object */
            JavaValue count_val = { .i = peer->count };
            native_set_field_value(ht, "count", count_val);
            
            NATIVE_DEBUG("Hashtable put: new entry at %d, key=%p, hash=0x%X", probe_idx, (void*)key, hash);
            return NATIVE_RETURN_NULL();
        }
        
        if (hashtable_keys_equal(entry->key, key)) {
            /* Found existing key - replace value */
            JavaObject* old_value = entry->value;
            entry->value = value;
            
            NATIVE_DEBUG("Hashtable put: replaced at %d, old=%p, new=%p", probe_idx, (void*)old_value, (void*)value);
            JavaValue ret = { .ref = old_value };
            return ret;
        }
    }
    
    /* Table full - expand (simple approach) */
    int new_capacity = peer->capacity * 2;
    HashtableEntry* new_entries = (HashtableEntry*)calloc(new_capacity, sizeof(HashtableEntry));
    if (new_entries) {
        /* Rehash all entries */
        for (int i = 0; i < peer->capacity; i++) {
            if (peer->entries[i].key) {
                jint new_idx = (peer->entries[i].hash & 0x7FFFFFFF) % new_capacity;
                while (new_entries[new_idx].key) {
                    new_idx = (new_idx + 1) % new_capacity;
                }
                new_entries[new_idx] = peer->entries[i];
            }
        }
        free(peer->entries);
        peer->entries = new_entries;
        peer->capacity = new_capacity;
        
        /* Retry put */
        return native_hashtable_put(jvm, thread, args, arg_count);
    }
    
    return NATIVE_RETURN_NULL();
}

static JavaValue native_hashtable_get(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* ht = (JavaObject*)args[0].ref;
    JavaObject* key = (JavaObject*)args[1].ref;
    
    if (!ht) return NATIVE_RETURN_NULL();
    
    /* Hashtable throws NullPointerException for null key */
    if (!key) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    HashtablePeer* peer = hashtable_get_peer(ht);
    if (!peer || !peer->entries || peer->count == 0) {
        return NATIVE_RETURN_NULL();
    }
    
    /* Compute hash */
    jint hash = hashtable_compute_hash(key);
    
    /* Find entry */
    jint idx = (hash & 0x7FFFFFFF) % peer->capacity;
    
    /* Linear probing */
    for (int i = 0; i < peer->capacity; i++) {
        jint probe_idx = (idx + i) % peer->capacity;
        HashtableEntry* entry = &peer->entries[probe_idx];
        
        if (entry->key == NULL) {
            /* Empty slot - key not found */
            break;
        }
        
        if (hashtable_keys_equal(entry->key, key)) {
            /* Found! */
            NATIVE_DEBUG("Hashtable get: found at %d, value=%p", probe_idx, (void*)entry->value);
            JavaValue ret = { .ref = entry->value };
            return ret;
        }
    }
    
    NATIVE_DEBUG("Hashtable get: key not found, hash=0x%X", hash);
    return NATIVE_RETURN_NULL();
}

static JavaValue native_hashtable_size(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ht = (JavaObject*)args[0].ref;
    
    if (!ht) return NATIVE_RETURN_INT(0);
    
    HashtablePeer* peer = hashtable_get_peer(ht);
    if (!peer) return NATIVE_RETURN_INT(0);
    
    return NATIVE_RETURN_INT(peer->count);
}

/* Hashtable.isEmpty() - check if table is empty */
static JavaValue native_hashtable_isEmpty(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ht = (JavaObject*)args[0].ref;
    
    if (!ht) return NATIVE_RETURN_INT(1);  /* null is empty */
    
    HashtablePeer* peer = hashtable_get_peer(ht);
    if (!peer) return NATIVE_RETURN_INT(1);
    
    return NATIVE_RETURN_INT(peer->count == 0 ? 1 : 0);
}

/* Hashtable.containsKey(Object) - check if key exists */
static JavaValue native_hashtable_containsKey(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ht = (JavaObject*)args[0].ref;
    JavaObject* key = (JavaObject*)args[1].ref;
    
    if (!ht || !key) return NATIVE_RETURN_INT(0);
    
    HashtablePeer* peer = hashtable_get_peer(ht);
    if (!peer || !peer->entries || peer->count == 0) {
        return NATIVE_RETURN_INT(0);
    }
    
    jint hash = hashtable_compute_hash(key);
    jint idx = (hash & 0x7FFFFFFF) % peer->capacity;
    
    for (int i = 0; i < peer->capacity; i++) {
        jint probe_idx = (idx + i) % peer->capacity;
        HashtableEntry* entry = &peer->entries[probe_idx];
        
        if (entry->key == NULL) break;
        
        if (hashtable_keys_equal(entry->key, key)) {
            return NATIVE_RETURN_INT(1);
        }
    }
    
    return NATIVE_RETURN_INT(0);
}

/* Hashtable.remove(Object) - remove key and return old value */
static JavaValue native_hashtable_remove(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ht = (JavaObject*)args[0].ref;
    JavaObject* key = (JavaObject*)args[1].ref;
    
    if (!ht || !key) return NATIVE_RETURN_NULL();
    
    HashtablePeer* peer = hashtable_get_peer(ht);
    if (!peer || !peer->entries || peer->count == 0) {
        return NATIVE_RETURN_NULL();
    }
    
    jint hash = hashtable_compute_hash(key);
    jint idx = (hash & 0x7FFFFFFF) % peer->capacity;
    
    for (int i = 0; i < peer->capacity; i++) {
        jint probe_idx = (idx + i) % peer->capacity;
        HashtableEntry* entry = &peer->entries[probe_idx];
        
        if (entry->key == NULL) break;
        
        if (hashtable_keys_equal(entry->key, key)) {
            JavaObject* old_value = entry->value;
            entry->key = NULL;
            entry->value = NULL;
            entry->hash = 0;
            peer->count--;
            
            JavaValue count_val = { .i = peer->count };
            native_set_field_value(ht, "count", count_val);
            
            JavaValue ret = { .ref = old_value };
            return ret;
        }
    }
    
    return NATIVE_RETURN_NULL();
}

/* Hashtable.contains(Object value) - check if value exists in the hashtable */
static JavaValue native_hashtable_contains(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ht = (JavaObject*)args[0].ref;
    JavaObject* value = (JavaObject*)args[1].ref;
    
    if (!ht) return NATIVE_RETURN_INT(0);
    
    HashtablePeer* peer = hashtable_get_peer(ht);
    if (!peer || !peer->entries || peer->count == 0) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Search for value using equals comparison */
    for (int i = 0; i < peer->capacity; i++) {
        HashtableEntry* entry = &peer->entries[i];
        if (entry->key != NULL) {
            if (vector_objects_equal(entry->value, value)) {
                return NATIVE_RETURN_INT(1);  /* true - found */
            }
        }
    }
    
    return NATIVE_RETURN_INT(0);  /* false - not found */
}

/* Hashtable.clear() - remove all entries */
static JavaValue native_hashtable_clear(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ht = (JavaObject*)args[0].ref;
    
    if (!ht) return NATIVE_RETURN_VOID();
    
    HashtablePeer* peer = hashtable_get_peer(ht);
    if (peer && peer->entries) {
        /* Clear all entries */
        memset(peer->entries, 0, peer->capacity * sizeof(HashtableEntry));
        peer->count = 0;
    }
    
    /* Update count field */
    JavaValue count_val = { .i = 0 };
    native_set_field_value(ht, "count", count_val);
    
    return NATIVE_RETURN_VOID();
}

/* Hashtable.keys() - return enumeration of keys */
static JavaValue native_hashtable_keys(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* ht = (JavaObject*)args[0].ref;
    
    if (!ht) return NATIVE_RETURN_NULL();
    
    HashtablePeer* peer = hashtable_get_peer(ht);
    
    if (!peer || !peer->entries || peer->count == 0) {
        /* Return empty enumeration */
        JavaArray* empty_keys = jvm_new_array(jvm, DESC_OBJECT, 0, NULL);
        if (!empty_keys) return NATIVE_RETURN_NULL();
        
        JavaClass* iter_class = get_or_create_stub_class(jvm, "java/util/VectorEnumeration");
        if (!iter_class) return NATIVE_RETURN_NULL();
        
        /* Ensure the class has fields for enumeration state */
        if (iter_class->fields_count == 0) {
            iter_class->fields = (JavaField*)calloc(3, sizeof(JavaField));
            iter_class->fields[0].name = strdup("elements");
            iter_class->fields[0].descriptor = strdup("[Ljava/lang/Object;");
            iter_class->fields[1].name = strdup("count");
            iter_class->fields[1].descriptor = strdup("I");
            iter_class->fields[2].name = strdup("index");
            iter_class->fields[2].descriptor = strdup("I");
            iter_class->fields_count = 3;
            iter_class->instance_size = sizeof(ObjectHeader) + 3 * sizeof(JavaValue);
            iter_class->initialized = true;
        }
        
        JavaObject* iter_obj = jvm_new_object(jvm, iter_class);
        if (!iter_obj) return NATIVE_RETURN_NULL();
        
        JavaValue keys_val = { .ref = empty_keys };
        JavaValue count_val = { .i = 0 };
        JavaValue index_val = { .i = 0 };
        
        native_set_field_value(iter_obj, "elements", keys_val);
        native_set_field_value(iter_obj, "count", count_val);
        native_set_field_value(iter_obj, "index", index_val);
        
        return NATIVE_RETURN_OBJECT(iter_obj);
    }
    
    /* Create array of keys */
    JavaArray* keys_array = jvm_new_array(jvm, DESC_OBJECT, peer->count, NULL);
    if (!keys_array) return NATIVE_RETURN_NULL();
    
    /* Collect all keys */
    int key_idx = 0;
    for (int i = 0; i < peer->capacity && key_idx < peer->count; i++) {
        HashtableEntry* entry = &peer->entries[i];
        if (entry->key != NULL) {
            array_set_ref(keys_array, key_idx++, entry->key);
        }
    }
    
    /* Create enumeration object using get_or_create_stub_class like native_vector_elements */
    JavaClass* iter_class = get_or_create_stub_class(jvm, "java/util/VectorEnumeration");
    if (!iter_class) {
        return NATIVE_RETURN_NULL();
    }
    
    /* Ensure the class has fields for enumeration state */
    if (iter_class->fields_count == 0) {
        iter_class->fields = (JavaField*)calloc(3, sizeof(JavaField));
        iter_class->fields[0].name = strdup("elements");
        iter_class->fields[0].descriptor = strdup("[Ljava/lang/Object;");
        iter_class->fields[1].name = strdup("count");
        iter_class->fields[1].descriptor = strdup("I");
        iter_class->fields[2].name = strdup("index");
        iter_class->fields[2].descriptor = strdup("I");
        iter_class->fields_count = 3;
        iter_class->instance_size = sizeof(ObjectHeader) + 3 * sizeof(JavaValue);
        iter_class->initialized = true;
    }
    
    JavaObject* iter_obj = jvm_new_object(jvm, iter_class);
    if (!iter_obj) {
        return NATIVE_RETURN_NULL();
    }
    
    JavaValue keys_val = { .ref = keys_array };
    JavaValue count_val = { .i = peer->count };
    JavaValue index_val = { .i = 0 };
    
    native_set_field_value(iter_obj, "elements", keys_val);
    native_set_field_value(iter_obj, "count", count_val);
    native_set_field_value(iter_obj, "index", index_val);
    
    return NATIVE_RETURN_OBJECT(iter_obj);
}

/* Hashtable.elements() - return enumeration of values */
static JavaValue native_hashtable_elements(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* ht = (JavaObject*)args[0].ref;
    
    if (!ht) return NATIVE_RETURN_NULL();
    
    HashtablePeer* peer = hashtable_get_peer(ht);
    
    if (!peer || !peer->entries || peer->count == 0) {
        /* Return empty enumeration */
        JavaArray* empty_values = jvm_new_array(jvm, DESC_OBJECT, 0, NULL);
        if (!empty_values) return NATIVE_RETURN_NULL();
        
        JavaClass* iter_class = get_or_create_stub_class(jvm, "java/util/VectorEnumeration");
        if (!iter_class) return NATIVE_RETURN_NULL();
        
        /* Ensure the class has fields for enumeration state */
        if (iter_class->fields_count == 0) {
            iter_class->fields = (JavaField*)calloc(3, sizeof(JavaField));
            iter_class->fields[0].name = strdup("elements");
            iter_class->fields[0].descriptor = strdup("[Ljava/lang/Object;");
            iter_class->fields[1].name = strdup("count");
            iter_class->fields[1].descriptor = strdup("I");
            iter_class->fields[2].name = strdup("index");
            iter_class->fields[2].descriptor = strdup("I");
            iter_class->fields_count = 3;
            iter_class->instance_size = sizeof(ObjectHeader) + 3 * sizeof(JavaValue);
            iter_class->initialized = true;
        }
        
        JavaObject* iter_obj = jvm_new_object(jvm, iter_class);
        if (!iter_obj) return NATIVE_RETURN_NULL();
        
        JavaValue values_val = { .ref = empty_values };
        JavaValue count_val = { .i = 0 };
        JavaValue index_val = { .i = 0 };
        
        native_set_field_value(iter_obj, "elements", values_val);
        native_set_field_value(iter_obj, "count", count_val);
        native_set_field_value(iter_obj, "index", index_val);
        
        return NATIVE_RETURN_OBJECT(iter_obj);
    }
    
    /* Create array of values */
    JavaArray* values_array = jvm_new_array(jvm, DESC_OBJECT, peer->count, NULL);
    if (!values_array) return NATIVE_RETURN_NULL();
    
    /* Collect all values */
    int val_idx = 0;
    for (int i = 0; i < peer->capacity && val_idx < peer->count; i++) {
        HashtableEntry* entry = &peer->entries[i];
        if (entry->key != NULL) {
            array_set_ref(values_array, val_idx++, entry->value);
        }
    }
    
    /* Create enumeration object */
    JavaClass* iter_class = get_or_create_stub_class(jvm, "java/util/VectorEnumeration");
    if (!iter_class) {
        return NATIVE_RETURN_NULL();
    }
    
    /* Ensure the class has fields for enumeration state */
    if (iter_class->fields_count == 0) {
        iter_class->fields = (JavaField*)calloc(3, sizeof(JavaField));
        iter_class->fields[0].name = strdup("elements");
        iter_class->fields[0].descriptor = strdup("[Ljava/lang/Object;");
        iter_class->fields[1].name = strdup("count");
        iter_class->fields[1].descriptor = strdup("I");
        iter_class->fields[2].name = strdup("index");
        iter_class->fields[2].descriptor = strdup("I");
        iter_class->fields_count = 3;
        iter_class->instance_size = sizeof(ObjectHeader) + 3 * sizeof(JavaValue);
        iter_class->initialized = true;
    }
    
    JavaObject* iter_obj = jvm_new_object(jvm, iter_class);
    if (!iter_obj) {
        return NATIVE_RETURN_NULL();
    }
    
    JavaValue values_val = { .ref = values_array };
    JavaValue count_val = { .i = peer->count };
    JavaValue index_val = { .i = 0 };
    
    native_set_field_value(iter_obj, "elements", values_val);
    native_set_field_value(iter_obj, "count", count_val);
    native_set_field_value(iter_obj, "index", index_val);
    
    return NATIVE_RETURN_OBJECT(iter_obj);
}

/* Hashtable.finalize() - called by GC before collecting the object
 * This ensures native peer memory is freed even if GC sweep misses it 
 * 
 * FIX: Now uses object pointer search instead of threshold field,
 * because Java code can modify threshold and break the linkage.
 */
static JavaValue native_hashtable_finalize(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ht = (JavaObject*)args[0].ref;
    
    if (!ht) return NATIVE_RETURN_VOID();
    
    /* FIX: Find peer by object pointer, not by threshold field */
    int peer_idx = -1;
    for (int i = 0; i < g_hashtable_count; i++) {
        if (g_hashtable_peer_alive[i] && g_hashtables[i].ht_obj == ht) {
            peer_idx = i;
            break;
        }
    }
    
    if (peer_idx >= 0 && peer_idx < MAX_HASHTABLES) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[NATIVE] Hashtable.finalize() called, freeing peer idx %d\n", peer_idx);
        hashtable_free_peer(peer_idx);
    }
    
    return NATIVE_RETURN_VOID();
}

/*
 * java.io.InputStreamReader native methods
 */

/* Helper to find the underlying InputStream in InputStreamReader */
/* ИСПРАВЛЕНО: используем native_get_field_value */
static JavaObject* get_inputstream_reader_stream(JavaObject* isr_obj) {
    if (!isr_obj) return NULL;
    
    JavaClass* clazz = isr_obj->header.clazz;
    if (!clazz) return NULL;
    
    
    /* Try to get 'in' field directly */
    JavaObject* stream = (JavaObject*)native_get_field_value(isr_obj, "in").ref;
    if (stream) {
        return stream;
    }
    
    /* If no 'in' field found, try to find InputStream by descriptor type */
    for (int i = 0; i < clazz->fields_count; i++) {
        const char* desc = clazz->fields[i].descriptor;
        const char* name = clazz->fields[i].name;
        if (desc && strstr(desc, "InputStream") && name) {
            /* InputStream reference field - use helper function */
            stream = (JavaObject*)native_get_field_value(isr_obj, name).ref;
            if (stream) {
                return stream;
            }
        }
    }
    
    return NULL;
}

static JavaValue native_inputstreamreader_read(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* isr = (JavaObject*)args[0].ref;
    
    if (!isr) {
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Get underlying InputStream */
    JavaObject* stream = get_inputstream_reader_stream(isr);
    if (!stream) {
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Check if it's a ByteArrayInputStream */
    JavaClass* stream_class = stream->header.clazz;
    if (stream_class && stream_class->class_name && 
        strstr(stream_class->class_name, "ByteArrayInputStream")) {
        
        /* ИСПРАВЛЕНО: используем native_get_field_value */
        JavaArray* buf = (JavaArray*)native_get_field_value(stream, "buf").ref;
        jint pos = native_get_field_value(stream, "pos").i;
        jint count = native_get_field_value(stream, "count").i;
        
        if (buf && pos < count) {
            /* Read byte and convert to char (ISO-8859-1) */
            uint8_t* data = (uint8_t*)array_data(buf);
            jint result = data[pos];
            
            /* ИСПРАВЛЕНО: используем native_set_field_value для обновления pos */
            JavaValue new_pos = { .i = pos + 1 };
            native_set_field_value(stream, "pos", new_pos);
            
            return NATIVE_RETURN_INT(result);
        }
    }
    
    /* Default: end of stream */
    return NATIVE_RETURN_INT(-1);
}

static JavaValue native_inputstreamreader_read_array(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* isr = (JavaObject*)args[0].ref;
    JavaArray* cbuf = (JavaArray*)args[1].ref;
    
    if (!isr || !cbuf) return NATIVE_RETURN_INT(-1);
    
    /* Get underlying InputStream */
    JavaObject* stream = get_inputstream_reader_stream(isr);
    if (!stream) return NATIVE_RETURN_INT(-1);
    
    /* Check if it's a ByteArrayInputStream */
    JavaClass* stream_class = stream->header.clazz;
    if (stream_class && stream_class->class_name && 
        strstr(stream_class->class_name, "ByteArrayInputStream")) {
        
        /* ИСПРАВЛЕНО: используем native_get_field_value */
        JavaArray* buf = (JavaArray*)native_get_field_value(stream, "buf").ref;
        jint pos = native_get_field_value(stream, "pos").i;
        jint count = native_get_field_value(stream, "count").i;
        
        if (buf && pos < count) {
            uint8_t* src_data = (uint8_t*)array_data(buf);
            uint16_t* dst_data = (uint16_t*)array_data(cbuf);
            
            int to_read = count - pos;
            if (to_read > cbuf->length) to_read = cbuf->length;
            
            /* Copy bytes as chars (ISO-8859-1) */
            for (int i = 0; i < to_read; i++) {
                dst_data[i] = src_data[pos + i];
            }
            
            /* ИСПРАВЛЕНО: используем native_set_field_value для обновления pos */
            JavaValue new_pos = { .i = pos + to_read };
            native_set_field_value(stream, "pos", new_pos);
            
            return NATIVE_RETURN_INT(to_read);
        }
    }
    
    return NATIVE_RETURN_INT(-1);
}

static JavaValue native_inputstreamreader_close(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* No-op for now */
    return NATIVE_RETURN_VOID();
}

/*
 * java.io.BufferedReader native methods
 */

/* Helper to find the underlying Reader in BufferedReader */
/* ИСПРАВЛЕНО: используем native_get_field_value */
static JavaObject* get_bufferedreader_reader(JavaObject* br_obj) {
    if (!br_obj) return NULL;
    
    /* Try 'in' field first */
    JavaObject* reader = (JavaObject*)native_get_field_value(br_obj, "in").ref;
    if (reader) return reader;
    
    /* Try 'reader' field */
    reader = (JavaObject*)native_get_field_value(br_obj, "reader").ref;
    if (reader) return reader;
    
    /* Try 'lock' field as fallback */
    reader = (JavaObject*)native_get_field_value(br_obj, "lock").ref;
    return reader;
}

static JavaValue native_bufferedreader_read(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* br = (JavaObject*)args[0].ref;
    
    if (!br) return NATIVE_RETURN_INT(-1);
    
    /* Get underlying Reader (InputStreamReader) */
    JavaObject* reader = get_bufferedreader_reader(br);
    if (!reader) return NATIVE_RETURN_INT(-1);
    
    /* Call read() on the underlying InputStreamReader */
    JavaClass* reader_class = reader->header.clazz;
    JavaMethod* read_method = jvm_resolve_method(jvm, reader_class, "read", "()I");
    
    if (read_method) {
        JavaValue reader_arg = { .ref = reader };
        JavaValue result;
        execute_method(jvm, thread, read_method, &reader_arg, &result);
        return result;
    }
    
    return NATIVE_RETURN_INT(-1);
}

static JavaValue native_bufferedreader_readline(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* br = (JavaObject*)args[0].ref;
    
    if (!br) return NATIVE_RETURN_NULL();
    
    /* Get underlying Reader */
    JavaObject* reader = get_bufferedreader_reader(br);
    if (!reader) return NATIVE_RETURN_NULL();
    
    /* Build string by reading characters */
    char buffer[4096];
    int len = 0;
    
    JavaClass* reader_class = reader->header.clazz;
    JavaMethod* read_method = jvm_resolve_method(jvm, reader_class, "read", "()I");
    
    if (!read_method) return NATIVE_RETURN_NULL();
    
    while (len < (int)sizeof(buffer) - 1) {
        JavaValue reader_arg = { .ref = reader };
        JavaValue result;
        execute_method(jvm, thread, read_method, &reader_arg, &result);
        
        jint ch = result.i;
        
        if (ch < 0) {
            /* End of stream */
            break;
        }
        
        if (ch == '\n') {
            break;
        }
        
        if (ch == '\r') {
            /* Check for \r\n */
            execute_method(jvm, thread, read_method, &reader_arg, &result);
            jint next = result.i;
            if (next >= 0 && next != '\n') {
                /* Put back - for simplicity, we just skip */
            }
            break;
        }
        
        buffer[len++] = (char)ch;
    }
    
    if (len == 0) {
        return NATIVE_RETURN_NULL();
    }
    
    /* Create String */
    buffer[len] = '\0';
    JavaString* str = jvm_new_string(jvm, buffer);
    JavaValue ret = { .ref = (JavaObject*)str };
    return ret;
}

static JavaValue native_bufferedreader_close(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* No-op for now */
    return NATIVE_RETURN_VOID();
}

void init_java_io_inputstreamreader(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/io/InputStreamReader", "read", "()I", native_inputstreamreader_read},
        {"java/io/InputStreamReader", "read", "([C)I", native_inputstreamreader_read_array},
        {"java/io/InputStreamReader", "read", "([CII)I", native_inputstreamreader_read_array},
        {"java/io/InputStreamReader", "close", "()V", native_inputstreamreader_close},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered java/io/InputStreamReader native methods");
}

void init_java_io_bufferedreader(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/io/BufferedReader", "read", "()I", native_bufferedreader_read},
        {"java/io/BufferedReader", "readLine", "()Ljava/lang/String;", native_bufferedreader_readline},
        {"java/io/BufferedReader", "close", "()V", native_bufferedreader_close},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered java/io/BufferedReader native methods");
}

void init_java_util_hashtable(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/util/Hashtable", "<init>", "()V", native_hashtable_init},
        {"java/util/Hashtable", "<init>", "(I)V", native_hashtable_init},          /* initial capacity */
        {"java/util/Hashtable", "<init>", "(IF)V", native_hashtable_init},         /* capacity + load factor */
        {"java/util/Hashtable", "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", native_hashtable_put},
        {"java/util/Hashtable", "get", "(Ljava/lang/Object;)Ljava/lang/Object;", native_hashtable_get},
        {"java/util/Hashtable", "size", "()I", native_hashtable_size},
        {"java/util/Hashtable", "isEmpty", "()Z", native_hashtable_isEmpty},
        {"java/util/Hashtable", "containsKey", "(Ljava/lang/Object;)Z", native_hashtable_containsKey},
        {"java/util/Hashtable", "contains", "(Ljava/lang/Object;)Z", native_hashtable_contains},
        {"java/util/Hashtable", "remove", "(Ljava/lang/Object;)Ljava/lang/Object;", native_hashtable_remove},
        {"java/util/Hashtable", "clear", "()V", native_hashtable_clear},
        {"java/util/Hashtable", "keys", "()Ljava/util/Enumeration;", native_hashtable_keys},
        {"java/util/Hashtable", "elements", "()Ljava/util/Enumeration;", native_hashtable_elements},
        {"java/util/Hashtable", "finalize", "()V", native_hashtable_finalize},     /* GC cleanup */
    };
    int count = sizeof(methods) / sizeof(methods[0]);
    native_register_methods(jvm, methods, count);
    NATIVE_DEBUG("Registered java/util/Hashtable native methods");
}

void init_java_util_random(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/util/Random", "<init>", "()V", native_random_init},
        {"java/util/Random", "<init>", "(J)V", native_random_init_seed},
        {"java/util/Random", "next", "(I)I", native_random_next},
        {"java/util/Random", "nextInt", "()I", native_random_nextInt},
        {"java/util/Random", "nextInt", "(I)I", native_random_nextIntBound},
        {"java/util/Random", "nextLong", "()J", native_random_nextLong},
        {"java/util/Random", "nextFloat", "()F", native_random_nextFloat},
        {"java/util/Random", "nextDouble", "()D", native_random_nextDouble},
        {"java/util/Random", "setSeed", "(J)V", native_random_setSeed},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered java/util/Random native methods");
}

/*
 * java.util.Timer native methods
 * SIMPLIFIED IMPLEMENTATION - runs synchronously in main thread
 * This avoids threading issues with non-thread-safe JVM
 */

/* Timer task entry */
typedef struct {
    JavaObject* timer_task;
    jlong next_run_time;    /* When to run (in ms since start) */
    jlong period_ms;        /* Period for repeating tasks, 0 for one-shot */
    bool active;
} TimerEntry;

#define MAX_TIMERS 16
static TimerEntry g_timers[MAX_TIMERS];
static int g_timer_count = 0;
static uint64_t g_start_time_ms = 0;

/* Get current time in milliseconds */
static uint64_t get_time_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* Process all pending timers - call from main loop */
void jvm_process_timers(JVM* jvm) {
    if (g_timer_count == 0) return;
    
    uint64_t now = get_time_ms() - g_start_time_ms;
    
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!g_timers[i].active || !g_timers[i].timer_task) continue;
        
        /* Check if it's time to run */
        if ((jlong)now >= g_timers[i].next_run_time) {
            fprintf(stderr, "[TIMER] Firing timer[%d]: now=%lu, scheduled=%lld (class=%s)\n",
                    i, now, g_timers[i].next_run_time,
                    g_timers[i].timer_task->header.clazz ? g_timers[i].timer_task->header.clazz->class_name : "(null)");
            
            /* Get the run method */
            JavaClass* task_class = g_timers[i].timer_task->header.clazz;
            if (!task_class) continue;
            
            JavaMethod* run_method = jvm_resolve_method(jvm, task_class, "run", "()V");
            if (!run_method) {
                fprintf(stderr, "[TIMER] WARNING: no run() method found on %s\n", task_class->class_name);
                continue;
            }
            
            /* Execute in main thread */
            JavaValue task_arg = { .ref = g_timers[i].timer_task };
            JavaValue result;
            JavaThread* thread = jvm_current_thread(jvm);
            if (thread && jvm->running) {
                execute_method(jvm, thread, run_method, &task_arg, &result);
                fprintf(stderr, "[TIMER] Timer[%d] run() completed\n", i);
            }
            
            /* Handle periodic vs one-shot */
            if (g_timers[i].period_ms > 0) {
                /* Schedule next run */
                g_timers[i].next_run_time = (jlong)now + g_timers[i].period_ms;
            } else {
                /* One-shot task done */
                g_timers[i].active = false;
                g_timer_count--;
            }
        }
    }
}

static JavaValue native_timer_schedule_task_delay(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)arg_count;
    (void)thread;
    JavaObject* timer_task = (JavaObject*)args[1].ref;
    jlong delay_ms = args[2].j;
    
    if (!timer_task) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    /* Initialize start time on first use */
    if (g_start_time_ms == 0) {
        g_start_time_ms = get_time_ms();
    }
    
    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!g_timers[i].active) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) {
        NATIVE_DEBUG("No free timer slots!");
        return NATIVE_RETURN_VOID();
    }
    
    uint64_t now = get_time_ms() - g_start_time_ms;
    g_timers[slot].timer_task = timer_task;
    g_timers[slot].next_run_time = (jlong)now + delay_ms;
    g_timers[slot].period_ms = 0;
    g_timers[slot].active = true;
    g_timer_count++;
    
    fprintf(stderr, "[TIMER] Scheduled one-shot task at slot %d, delay=%lld ms, now=%lu, fire_at=%lld (class=%s)\n",
            slot, delay_ms, now, g_timers[slot].next_run_time,
            timer_task->header.clazz ? timer_task->header.clazz->class_name : "(null)");
    return NATIVE_RETURN_VOID();
}

static JavaValue native_timer_schedule_task_delay_period(JVM* jvm, JavaThread* thread,
                                                          JavaValue* args, int arg_count) {
    (void)arg_count;
    (void)thread;
    JavaObject* timer_task = (JavaObject*)args[1].ref;
    jlong delay_ms = args[2].j;
    jlong period_ms = args[4].j;
    
    if (!timer_task) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    /* Initialize start time on first use */
    if (g_start_time_ms == 0) {
        g_start_time_ms = get_time_ms();
    }
    
    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!g_timers[i].active) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0) {
        NATIVE_DEBUG("No free timer slots!");
        return NATIVE_RETURN_VOID();
    }
    
    uint64_t now = get_time_ms() - g_start_time_ms;
    g_timers[slot].timer_task = timer_task;
    g_timers[slot].next_run_time = (jlong)now + delay_ms;
    g_timers[slot].period_ms = period_ms;
    g_timers[slot].active = true;
    g_timer_count++;
    
    NATIVE_DEBUG("Scheduled periodic task at slot %d, delay=%ld ms, period=%ld ms", 
            slot, (long)delay_ms, (long)period_ms);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_timer_cancel(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    
    /* Cancel all timers */
    for (int i = 0; i < MAX_TIMERS; i++) {
        g_timers[i].active = false;
        g_timers[i].timer_task = NULL;
    }
    g_timer_count = 0;
    
    NATIVE_DEBUG("All timers cancelled");
    return NATIVE_RETURN_VOID();
}

void init_java_util_timer(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/util/Timer", "schedule", "(Ljava/util/TimerTask;J)V", native_timer_schedule_task_delay},
        {"java/util/Timer", "schedule", "(Ljava/util/TimerTask;JJ)V", native_timer_schedule_task_delay_period},
        {"java/util/Timer", "scheduleAtFixedRate", "(Ljava/util/TimerTask;JJ)V", native_timer_schedule_task_delay_period},
        {"java/util/Timer", "cancel", "()V", native_timer_cancel},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered java/util/Timer native methods");
}

/*
 * Call a native method
 */
int native_call(JVM* jvm, JavaThread* thread, JavaMethod* method, 
                JavaValue* args, JavaValue* result) {
    if (!method || !method->clazz) {
        NATIVE_DEBUG("native_call: NULL method or class");
        return -1;
    }
    
    /* CRITICAL FIX: If there's a pending exception from a previous context,
     * clear it before executing this native method. This handles the case where
     * an exception was thrown but not properly cleared after being handled.
     */
    if (thread->pending_exception) {
        JavaClass* ex_class = thread->pending_exception->header.clazz;
        NATIVE_DEBUG("WARNING: clearing stale pending_exception before %s.%s%s (exception: %s)",
                method->clazz->class_name, method->name, method->descriptor,
                ex_class ? ex_class->class_name : "??");
        thread->pending_exception = NULL;  /* Clear it */
    }
    
    /* Find native method handler */
    NativeMethod handler = native_find(jvm, 
        method->clazz->class_name, method->name, method->descriptor);
    
    /* DEBUG: Log native lookup for Exception methods */
    if (method->name && strstr(method->name, "toString")) {
        if (g_j2me_runtime_debug) fprintf(stderr, "[DEBUG] native_call: looking for %s.%s%s, handler=%p\n",
                method->clazz->class_name, method->name, method->descriptor, (void*)handler);
    }
    
    if (!handler) {
        NATIVE_DEBUG("Unimplemented native method: %s.%s%s",
                method->clazz->class_name, method->name, method->descriptor);
        
        /* Return default value instead of crashing */
        if (result) {
            memset(result, 0, sizeof(JavaValue));
        }
        return 0;  /* Don't fail, just return default */
    }
    
    /* Count arguments */
    int arg_count = count_args(method->descriptor);
    
    /* For non-static methods, args[0] is 'this' reference */
    int is_static = (method->access_flags & ACC_STATIC) != 0;
    int total_args = is_static ? arg_count : arg_count + 1;
    
    /* Call the native method */
    JavaValue ret = handler(jvm, thread, args, total_args);
    
    if (result) {
        *result = ret;
    }
    
    /* Check for exception */
    if (thread->pending_exception) {
        return -1;
    }
    
    return 0;
}

/*
 * javax.microedition.io.Connector native methods
 * GCF (Generic Connection Framework) implementation
 */

static JavaValue native_connector_open(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    /* args[0] = name (String), args[1] = mode (int), args[2] = timeouts (boolean) */
    JavaString* name_str = (JavaString*)args[0].ref;
    const char* name = name_str ? string_utf8(jvm, name_str) : NULL;
    jint mode = args[1].i;
    jboolean timeouts = args[2].i;
    
    (void)mode; (void)timeouts;
    
    if (!name) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Parse protocol */
    if (strncmp(name, "http://", 7) == 0 || strncmp(name, "https://", 8) == 0) {
        /* HTTP connection - return stub */
        JavaClass* http_class = jvm_load_class(jvm, "javax/microedition/io/HttpConnection");
        if (http_class) {
            JavaObject* conn = jvm_new_object(jvm, http_class);
            return NATIVE_RETURN_OBJECT(conn);
        }
    } else if (strncmp(name, "socket://", 9) == 0) {
        JavaClass* socket_class = jvm_load_class(jvm, "javax/microedition/io/SocketConnection");
        if (socket_class) {
            JavaObject* conn = jvm_new_object(jvm, socket_class);
            return NATIVE_RETURN_OBJECT(conn);
        }
    } else if (strncmp(name, "file://", 7) == 0) {
        JavaClass* file_class = jvm_load_class(jvm, "javax/microedition/io/FileConnection");
        if (!file_class) {
            /* Create stub class dynamically */
            file_class = get_or_create_stub_class(jvm, "javax/microedition/io/FileConnection");
        }
        if (file_class) {
            JavaObject* conn = jvm_new_object(jvm, file_class);
            return NATIVE_RETURN_OBJECT(conn);
        }
    }
    
    /* Generic StreamConnection for other protocols */
    JavaClass* stream_class = jvm_load_class(jvm, "javax/microedition/io/StreamConnection");
    if (stream_class) {
        JavaObject* conn = jvm_new_object(jvm, stream_class);
        return NATIVE_RETURN_OBJECT(conn);
    }
    
    return NATIVE_RETURN_NULL();
}

static JavaValue native_connector_openInputStream(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* conn = (JavaObject*)args[0].ref;
    
    
    /* Create ByteArrayInputStream stub */
    JavaClass* bais_class = jvm_load_class(jvm, "java/io/ByteArrayInputStream");
    if (bais_class) {
        JavaObject* stream = jvm_new_object(jvm, bais_class);
        return NATIVE_RETURN_OBJECT(stream);
    }
    
    return NATIVE_RETURN_NULL();
}

static JavaValue native_connector_openOutputStream(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* conn = (JavaObject*)args[0].ref;
    
    
    /* Create ByteArrayOutputStream stub */
    JavaClass* baos_class = jvm_load_class(jvm, "java/io/ByteArrayOutputStream");
    if (baos_class) {
        JavaObject* stream = jvm_new_object(jvm, baos_class);
        return NATIVE_RETURN_OBJECT(stream);
    }
    
    return NATIVE_RETURN_NULL();
}

/*
 * HttpConnection interface methods (stub implementations - no real network)
 * These return error codes to indicate network access is not available
 */

/* HttpConnection.setRequestMethod(String method) - no-op stub */
static JavaValue native_http_setRequestMethod(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    /* args[0] = this, args[1] = method String */
    /* Just ignore - no actual network connection */
    return NATIVE_RETURN_VOID();
}

/* HttpConnection.setRequestProperty(String key, String value) - no-op stub */
static JavaValue native_http_setRequestProperty(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    /* args[0] = this, args[1] = key, args[2] = value */
    /* Just ignore - no actual network connection */
    return NATIVE_RETURN_VOID();
}

/* HttpConnection.getResponseCode() - return error code */
static JavaValue native_http_getResponseCode(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    /* args[0] = this */
    /* Return 503 Service Unavailable to indicate network not available */
    return NATIVE_RETURN_INT(503);
}

/* ContentConnection.getLength() - return unknown length */
static JavaValue native_http_getLength(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    /* args[0] = this */
    /* Return -1 to indicate unknown length */
    JavaValue result;
    result.j = -1;  /* long value */
    return result;
}

/* HttpConnection.getHeaderField(String name) - return null */
static JavaValue native_http_getHeaderField(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    /* args[0] = this, args[1] = name */
    /* Return null - no headers available */
    return NATIVE_RETURN_NULL();
}

/* ContentConnection.getType() - return null */
static JavaValue native_http_getType(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    /* args[0] = this */
    /* Return null - no content type available */
    return NATIVE_RETURN_NULL();
}

/* Connection.close() - no-op stub */
static JavaValue native_http_close(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    /* args[0] = this */
    /* No-op - nothing to close */
    return NATIVE_RETURN_VOID();
}

void init_javax_microedition_io(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"javax/microedition/io/Connector", "open", "(Ljava/lang/String;)Ljavax/microedition/io/Connection;", native_connector_open},
        {"javax/microedition/io/Connector", "open", "(Ljava/lang/String;IZ)Ljavax/microedition/io/Connection;", native_connector_open},
        {"javax/microedition/io/StreamConnection", "openInputStream", "()Ljava/io/InputStream;", native_connector_openInputStream},
        {"javax/microedition/io/StreamConnection", "openOutputStream", "()Ljava/io/OutputStream;", native_connector_openOutputStream},
        {"javax/microedition/io/ContentConnection", "openInputStream", "()Ljava/io/InputStream;", native_connector_openInputStream},
        {"javax/microedition/io/ContentConnection", "openOutputStream", "()Ljava/io/OutputStream;", native_connector_openOutputStream},
        {"javax/microedition/io/ContentConnection", "getLength", "()J", native_http_getLength},
        {"javax/microedition/io/ContentConnection", "getType", "()Ljava/lang/String;", native_http_getType},
        {"javax/microedition/io/HttpConnection", "openInputStream", "()Ljava/io/InputStream;", native_connector_openInputStream},
        {"javax/microedition/io/HttpConnection", "openOutputStream", "()Ljava/io/OutputStream;", native_connector_openOutputStream},
        {"javax/microedition/io/HttpConnection", "setRequestMethod", "(Ljava/lang/String;)V", native_http_setRequestMethod},
        {"javax/microedition/io/HttpConnection", "setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V", native_http_setRequestProperty},
        {"javax/microedition/io/HttpConnection", "getResponseCode", "()I", native_http_getResponseCode},
        {"javax/microedition/io/HttpConnection", "getLength", "()J", native_http_getLength},
        {"javax/microedition/io/HttpConnection", "getHeaderField", "(Ljava/lang/String;)Ljava/lang/String;", native_http_getHeaderField},
        {"javax/microedition/io/HttpConnection", "getType", "()Ljava/lang/String;", native_http_getType},
        {"javax/microedition/io/Connection", "close", "()V", native_http_close},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered javax.microedition.io native methods");
}

/*
 * java.util.TimeZone native methods
 */

static JavaValue native_timezone_getTimeZone(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    
    /* STATIC METHOD - args[0] = timezone ID string */
    JavaString* id_str = (JavaString*)args[0].ref;
    const char* id = id_str ? string_utf8(jvm, id_str) : NULL;
    
    NATIVE_DEBUG("TimeZone.getTimeZone('%s')", id ? id : "null");
    
    /* Create a TimeZone object */
    JavaClass* tz_class = jvm_load_class(jvm, "java/util/TimeZone");
    if (!tz_class) {
        NATIVE_DEBUG("TimeZone.getTimeZone: TimeZone class not found!");
        return NATIVE_RETURN_NULL();
    }
    
    JavaObject* tz = jvm_new_object(jvm, tz_class);
    if (!tz) {
        return NATIVE_RETURN_NULL();
    }
    
    /* Set the ID field */
    if (id) {
        native_set_field_value(tz, "ID", (JavaValue){ .ref = id_str });
    } else {
        JavaString* default_id = jvm_new_string(jvm, "GMT");
        native_set_field_value(tz, "ID", (JavaValue){ .ref = default_id });
    }
    
    /* Set rawOffset — we'll always use GMT for simplicity.
     * Real offset requires timezone database parsing.
     * Most midlets only need timezone for display, not calculation. */
    /* Determine offset from the requested timezone */
    int raw_offset = 0;  /* default: GMT */
    if (id) {
        /* Simple heuristic for common timezone names */
        if (strstr(id, "Europe/")) {
            /* Try to use system timezone */
            time_t now = time(NULL);
            struct tm local_tm = {0}, gmt_tm = {0};
#ifdef _WIN32
            localtime_s(&local_tm, &now);
            gmtime_s(&gmt_tm, &now);
#else
            localtime_r(&now, &local_tm);
            gmtime_r(&now, &gmt_tm);
#endif
            raw_offset = (int)((mktime(&local_tm) - mktime(&gmt_tm)) * 1000);
        } else if (strstr(id, "GMT") || strstr(id, "UTC")) {
            /* Parse GMT+hh:mm format */
            const char* p = id;
            while (*p && *p != '+' && *p != '-') p++;
            if (*p) {
                int sign = (*p == '-') ? -1 : 1;
                int hours = 0, minutes = 0;
                if (sscanf(p + 1, "%d:%d", &hours, &minutes) >= 1) {
                    raw_offset = sign * (hours * 3600 + minutes * 60) * 1000;
                }
            }
        }
    }
    
    native_set_field_value(tz, "rawOffset", (JavaValue){ .i = raw_offset });
    
    NATIVE_DEBUG("TimeZone.getTimeZone('%s') → TimeZone{offset=%d}", id ? id : "GMT", raw_offset);
    return NATIVE_RETURN_OBJECT(tz);
}

static JavaValue native_timezone_getDefault(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)args; (void)arg_count;
    
    NATIVE_DEBUG("TimeZone.getDefault() called");
    
    /* Use system timezone */
    JavaString* tz_id = jvm_new_string(jvm, "GMT");
    JavaValue tz_args[1];
    tz_args[0].ref = tz_id;
    return native_timezone_getTimeZone(jvm, thread, tz_args, 1);
}

/*
 * java.util.Calendar native methods
 */

static JavaValue native_calendar_getInstance(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)thread;
    
    /* getInstance() — no args or getInstance(TimeZone) — 1 arg
     * We ignore the timezone for simplicity and use current system time */
    (void)arg_count;
    (void)args;
    
    
    JavaClass* cal_class = jvm_load_class(jvm, "java/util/Calendar");
    if (!cal_class) {
        return NATIVE_RETURN_NULL();
    }
    
    JavaObject* cal = jvm_new_object(jvm, cal_class);
    if (cal) {
        /* ИСПРАВЛЕНО: используем native_set_field_value */
        time_t now = time(NULL);
        struct tm* tm_info = localtime(&now);
        
        JavaValue time_val = { .j = (jlong)now * 1000 };
        native_set_field_value(cal, "time", time_val);
        
        JavaValue year_val = { .i = tm_info->tm_year + 1900 };
        native_set_field_value(cal, "year", year_val);
        
        JavaValue month_val = { .i = tm_info->tm_mon };
        native_set_field_value(cal, "month", month_val);
        
        JavaValue day_val = { .i = tm_info->tm_mday };
        native_set_field_value(cal, "day", day_val);
        native_set_field_value(cal, "dayOfMonth", day_val);
        
        JavaValue hour_val = { .i = tm_info->tm_hour };
        native_set_field_value(cal, "hour", hour_val);
        native_set_field_value(cal, "hourOfDay", hour_val);
        
        JavaValue min_val = { .i = tm_info->tm_min };
        native_set_field_value(cal, "minute", min_val);
        
        JavaValue sec_val = { .i = tm_info->tm_sec };
        native_set_field_value(cal, "second", sec_val);
    }
    
    return NATIVE_RETURN_OBJECT(cal);
}

static JavaValue native_calendar_getTime(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* cal = (JavaObject*)args[0].ref;
    
    jlong millis = 0;
    
    if (cal) {
        /* ИСПРАВЛЕНО: используем native_get_field_value */
        millis = native_get_field_value(cal, "time").j;
    }
    
    /* Create Date object */
    JavaClass* date_class = jvm_load_class(jvm, "java/util/Date");
    if (date_class) {
        JavaObject* date = jvm_new_object(jvm, date_class);
        if (date) {
            /* ИСПРАВЛЕНО: используем native_set_field_value */
            JavaValue val = { .j = millis };
            native_set_field_value(date, "fastTime", val);
        }
        return NATIVE_RETURN_OBJECT(date);
    }
    
    return NATIVE_RETURN_NULL();
}

void init_java_util_calendar(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/util/Calendar", "getInstance", "()Ljava/util/Calendar;", native_calendar_getInstance},
        {"java/util/Calendar", "getInstance", "(Ljava/util/TimeZone;)Ljava/util/Calendar;", native_calendar_getInstance},
        {"java/util/Calendar", "getTime", "()Ljava/util/Date;", native_calendar_getTime},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.util.TimeZone native methods registration
 */

void init_java_util_timezone(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/util/TimeZone", "getTimeZone", "(Ljava/lang/String;)Ljava/util/TimeZone;", native_timezone_getTimeZone},
        {"java/util/TimeZone", "getDefault", "()Ljava/util/TimeZone;", native_timezone_getDefault},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.util.Date native methods
 */

static JavaValue native_date_init(JVM* jvm, JavaThread* thread,
                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* date = (JavaObject*)args[0].ref;
    
    if (date) {
        /* ИСПРАВЛЕНО: используем native_set_field_value */
        time_t now = time(NULL);
        JavaValue val = { .j = (jlong)now * 1000 };
        native_set_field_value(date, "fastTime", val);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_date_getTime(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* date = (JavaObject*)args[0].ref;
    
    if (date) {
        /* ИСПРАВЛЕНО: используем native_get_field_value */
        jlong time = native_get_field_value(date, "fastTime").j;
        return NATIVE_RETURN_LONG(time);
    }
    
    return NATIVE_RETURN_LONG(0);
}

static JavaValue native_date_toString(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* date = (JavaObject*)args[0].ref;
    
    /* ИСПРАВЛЕНО: используем native_get_field_value */
    jlong millis = 0;
    if (date) {
        millis = native_get_field_value(date, "fastTime").j;
    }
    
    time_t t = (time_t)(millis / 1000);
    struct tm* tm_info = localtime(&t);
    char buf[64];
    strftime(buf, sizeof(buf), "%a %b %d %H:%M:%S %Y", tm_info);
    
    return NATIVE_RETURN_OBJECT(jvm_new_string(jvm, buf));
}

void init_java_util_date(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/util/Date", "<init>", "()V", native_date_init},
        {"java/util/Date", "getTime", "()J", native_date_getTime},
        {"java/util/Date", "toString", "()Ljava/lang/String;", native_date_toString},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.lang.Character native methods
 */

static JavaValue native_character_isDigit(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint c = args[0].i;
    return NATIVE_RETURN_INT(c >= '0' && c <= '9' ? 1 : 0);
}

static JavaValue native_character_isLetter(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint c = args[0].i;
    return NATIVE_RETURN_INT((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ? 1 : 0);
}

static JavaValue native_character_isLetterOrDigit(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint c = args[0].i;
    int is_letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    int is_digit = c >= '0' && c <= '9';
    return NATIVE_RETURN_INT(is_letter || is_digit ? 1 : 0);
}

static JavaValue native_character_isUpperCase(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint c = args[0].i;
    return NATIVE_RETURN_INT(c >= 'A' && c <= 'Z' ? 1 : 0);
}

static JavaValue native_character_isLowerCase(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint c = args[0].i;
    return NATIVE_RETURN_INT(c >= 'a' && c <= 'z' ? 1 : 0);
}

static JavaValue native_character_isWhitespace(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint c = args[0].i;
    return NATIVE_RETURN_INT(c == ' ' || c == '\t' || c == '\n' || c == '\r' ? 1 : 0);
}

static JavaValue native_character_toUpperCase(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint c = args[0].i;
    if (c >= 'a' && c <= 'z') {
        return NATIVE_RETURN_INT(c - 'a' + 'A');
    }
    return NATIVE_RETURN_INT(c);
}

static JavaValue native_character_toLowerCase(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint c = args[0].i;
    if (c >= 'A' && c <= 'Z') {
        return NATIVE_RETURN_INT(c - 'A' + 'a');
    }
    return NATIVE_RETURN_INT(c);
}

static JavaValue native_character_digit(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint c = args[0].i;
    jint radix = args[1].i;
    
    int val = -1;
    if (c >= '0' && c <= '9') {
        val = c - '0';
    } else if (c >= 'a' && c <= 'z') {
        val = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'Z') {
        val = c - 'A' + 10;
    }
    
    if (val >= 0 && val < radix) {
        return NATIVE_RETURN_INT(val);
    }
    return NATIVE_RETURN_INT(-1);
}

static JavaValue native_character_forDigit(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint digit = args[0].i;
    jint radix = args[1].i;
    
    if (digit >= 0 && digit < radix) {
        if (digit < 10) {
            return NATIVE_RETURN_INT('0' + digit);
        } else {
            return NATIVE_RETURN_INT('a' + digit - 10);
        }
    }
    return NATIVE_RETURN_INT(0);
}

void init_java_lang_character(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/lang/Character", "isDigit", "(C)Z", native_character_isDigit},
        {"java/lang/Character", "isLetter", "(C)Z", native_character_isLetter},
        {"java/lang/Character", "isLetterOrDigit", "(C)Z", native_character_isLetterOrDigit},
        {"java/lang/Character", "isUpperCase", "(C)Z", native_character_isUpperCase},
        {"java/lang/Character", "isLowerCase", "(C)Z", native_character_isLowerCase},
        {"java/lang/Character", "isWhitespace", "(C)Z", native_character_isWhitespace},
        {"java/lang/Character", "toUpperCase", "(C)C", native_character_toUpperCase},
        {"java/lang/Character", "toLowerCase", "(C)C", native_character_toLowerCase},
        {"java/lang/Character", "digit", "(CI)I", native_character_digit},
        {"java/lang/Character", "forDigit", "(II)C", native_character_forDigit},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.lang.Byte and Short native methods
 */

static JavaValue native_byte_parseByte(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    const char* s = str ? string_utf8(jvm, str) : NULL;
    
    if (!s) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    
    return NATIVE_RETURN_INT((jbyte)atoi(s));
}

static JavaValue native_byte_valueOf(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jbyte b = (jbyte)args[0].i;
    
    JavaClass* byte_class = jvm_load_class(jvm, "java/lang/Byte");
    if (byte_class) {
        JavaObject* obj = jvm_new_object(jvm, byte_class);
        if (obj) {
            /* ИСПРАВЛЕНО: используем native_set_field_value */
            JavaValue val = { .i = b };
            native_set_field_value(obj, "value", val);
        }
        return NATIVE_RETURN_OBJECT(obj);
    }
    return NATIVE_RETURN_NULL();
}

static JavaValue native_short_parseShort(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    const char* s = str ? string_utf8(jvm, str) : NULL;
    
    if (!s) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(0);
    }
    
    return NATIVE_RETURN_INT((jshort)atoi(s));
}

static JavaValue native_short_valueOf(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jshort s = (jshort)args[0].i;
    
    JavaClass* short_class = jvm_load_class(jvm, "java/lang/Short");
    if (short_class) {
        JavaObject* obj = jvm_new_object(jvm, short_class);
        if (obj) {
            /* ИСПРАВЛЕНО: используем native_set_field_value */
            JavaValue val = { .i = s };
            native_set_field_value(obj, "value", val);
        }
        return NATIVE_RETURN_OBJECT(obj);
    }
    return NATIVE_RETURN_NULL();
}

void init_java_lang_byte_short(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/lang/Byte", "parseByte", "(Ljava/lang/String;)B", native_byte_parseByte},
        {"java/lang/Byte", "valueOf", "(B)Ljava/lang/Byte;", native_byte_valueOf},
        {"java/lang/Short", "parseShort", "(Ljava/lang/String;)S", native_short_parseShort},
        {"java/lang/Short", "valueOf", "(S)Ljava/lang/Short;", native_short_valueOf},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/*
 * java.lang.Float native methods
 */

static JavaValue native_float_valueOf(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jfloat value = args[0].f;
    
    JavaClass* float_class = jvm_load_class(jvm, "java/lang/Float");
    if (!float_class) return NATIVE_RETURN_NULL();
    
    JavaObject* obj = jvm_new_object(jvm, float_class);
    if (obj) {
        /* ИСПРАВЛЕНО: используем native_set_field_value */
        JavaValue val = { .f = value };
        native_set_field_value(obj, "value", val);
    }
    
    return NATIVE_RETURN_OBJECT(obj);
}

static JavaValue native_float_parseFloat(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_FLOAT(0.0f);
    }
    
    const char* cstr = string_utf8(jvm, str);
    float value = 0.0f;
    if (cstr) {
        value = (float)atof(cstr);
    }
    
    return NATIVE_RETURN_FLOAT(value);
}

static JavaValue native_float_floatValue(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) return NATIVE_RETURN_FLOAT(0.0f);
    
    /* ИСПРАВЛЕНО: используем native_get_field_value */
    jfloat value = native_get_field_value(obj, "value").f;
    return NATIVE_RETURN_FLOAT(value);
}

static JavaValue native_float_toString(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jfloat value = args[0].f;
    
    char buf[32];
    snprintf(buf, sizeof(buf), "%g", (double)value);
    
    JavaString* str = jvm_new_string(jvm, buf);
    return NATIVE_RETURN_OBJECT(str);
}

/*
 * java.lang.Double native methods
 */

static JavaValue native_double_valueOf(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jdouble value = args[0].d;
    
    JavaClass* double_class = jvm_load_class(jvm, "java/lang/Double");
    if (!double_class) return NATIVE_RETURN_NULL();
    
    JavaObject* obj = jvm_new_object(jvm, double_class);
    if (obj) {
        /* ИСПРАВЛЕНО: используем native_set_field_value */
        JavaValue val = { .d = value };
        native_set_field_value(obj, "value", val);
    }
    
    return NATIVE_RETURN_OBJECT(obj);
}

static JavaValue native_double_parseDouble(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaString* str = (JavaString*)args[0].ref;
    
    if (!str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_DOUBLE(0.0);
    }
    
    const char* cstr = string_utf8(jvm, str);
    double value = 0.0;
    if (cstr) {
        value = atof(cstr);
    }
    
    return NATIVE_RETURN_DOUBLE(value);
}

static JavaValue native_double_doubleValue(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) return NATIVE_RETURN_DOUBLE(0.0);
    
    /* ИСПРАВЛЕНО: используем native_get_field_value */
    jdouble value = native_get_field_value(obj, "value").d;
    return NATIVE_RETURN_DOUBLE(value);
}

static JavaValue native_double_toString(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jdouble value = args[0].d;
    
    char buf[32];
    snprintf(buf, sizeof(buf), "%g", value);
    
    JavaString* str = jvm_new_string(jvm, buf);
    return NATIVE_RETURN_OBJECT(str);
}

void init_java_lang_float_double(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"java/lang/Float", "valueOf", "(F)Ljava/lang/Float;", native_float_valueOf},
        {"java/lang/Float", "parseFloat", "(Ljava/lang/String;)F", native_float_parseFloat},
        {"java/lang/Float", "floatValue", "()F", native_float_floatValue},
        {"java/lang/Float", "toString", "(F)Ljava/lang/String;", native_float_toString},
        {"java/lang/Double", "valueOf", "(D)Ljava/lang/Double;", native_double_valueOf},
        {"java/lang/Double", "parseDouble", "(Ljava/lang/String;)D", native_double_parseDouble},
        {"java/lang/Double", "doubleValue", "()D", native_double_doubleValue},
        {"java/lang/Double", "toString", "(D)Ljava/lang/String;", native_double_toString},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered java/lang/Float and Double native methods");
}

/*
 * javax.microedition.media.Manager native methods
 */

static JavaValue native_manager_createPlayer(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    (void)args;
    
    
    /* Create PlayerImpl (concrete implementation), not Player (interface) */
    JavaClass* player_class = jvm_load_class(jvm, "javax/microedition/media/PlayerImpl");
    if (!player_class) {
        player_class = jvm_load_class(jvm, "javax/microedition/media/Player");
    }
    
    if (player_class) {
        JavaObject* player = jvm_new_object(jvm, player_class);
        return NATIVE_RETURN_OBJECT(player);
    }
    
    return NATIVE_RETURN_NULL();
}

static JavaValue native_manager_playTone(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint note = args[0].i;
    jint duration = args[1].i;
    jint volume = args[2].i;
    
    /* TODO: Implement actual tone playback */
    
    return NATIVE_RETURN_VOID();
}

/*
 * Player state constants (JSR-135)
 */
#define PLAYER_UNREALIZED  100
#define PLAYER_REALIZED    200
#define PLAYER_PREFETCHED  300
#define PLAYER_STARTED     400
#define PLAYER_CLOSED      0

/* Helper: get Player state field */
static jint player_get_state(JavaObject* player) {
    if (!player || !player->header.clazz) return PLAYER_CLOSED;
    
    /* Field 1 is 'state' in PlayerImpl */
    if (OBJECT_HAS_FIELDS(player, 2)) {
        return player->fields[1].i;
    }
    return PLAYER_CLOSED;
}

/* Helper: set Player state field */
static void player_set_state(JavaObject* player, jint state) {
    if (!player || !player->header.clazz) return;
    
    /* Field 1 is 'state' in PlayerImpl */
    if (OBJECT_HAS_FIELDS(player, 2)) {
        player->fields[1].i = state;
    }
}

/* Player.getState() */
static JavaValue native_player_getState(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* player = (JavaObject*)args[0].ref;
    
    jint state = player_get_state(player);
    
    JavaValue result = { .i = state };
    return result;
}

/* Player.start() */
static JavaValue native_player_start(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* player = (JavaObject*)args[0].ref;
    
    player_set_state(player, PLAYER_STARTED);
    
    return NATIVE_RETURN_VOID();
}

/* Player.stop() */
static JavaValue native_player_stop(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* player = (JavaObject*)args[0].ref;
    
    player_set_state(player, PLAYER_PREFETCHED);
    
    return NATIVE_RETURN_VOID();
}

/* Player.close() */
static JavaValue native_player_close(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* player = (JavaObject*)args[0].ref;
    
    player_set_state(player, PLAYER_CLOSED);
    
    return NATIVE_RETURN_VOID();
}

/* Player.deallocate() */
static JavaValue native_player_deallocate(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* player = (JavaObject*)args[0].ref;
    
    player_set_state(player, PLAYER_REALIZED);
    
    return NATIVE_RETURN_VOID();
}

/* Player.realize() */
static JavaValue native_player_realize(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* player = (JavaObject*)args[0].ref;
    
    player_set_state(player, PLAYER_REALIZED);
    
    return NATIVE_RETURN_VOID();
}

/* Player.prefetch() */
static JavaValue native_player_prefetch(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* player = (JavaObject*)args[0].ref;
    
    player_set_state(player, PLAYER_PREFETCHED);
    
    return NATIVE_RETURN_VOID();
}

void init_javax_microedition_media_manager(JVM* jvm) {
    NativeMethodEntry methods[] = {
        /* Manager methods */
        {"javax/microedition/media/Manager", "createPlayer", "(Ljava/lang/String;)Ljavax/microedition/media/Player;", native_manager_createPlayer},
        {"javax/microedition/media/Manager", "playTone", "(III)V", native_manager_playTone},
        
        /* Player methods - registered for both interface and implementation */
        {"javax/microedition/media/Player", "getState", "()I", native_player_getState},
        {"javax/microedition/media/Player", "start", "()V", native_player_start},
        {"javax/microedition/media/Player", "stop", "()V", native_player_stop},
        {"javax/microedition/media/Player", "close", "()V", native_player_close},
        {"javax/microedition/media/Player", "deallocate", "()V", native_player_deallocate},
        {"javax/microedition/media/Player", "realize", "()V", native_player_realize},
        {"javax/microedition/media/Player", "prefetch", "()V", native_player_prefetch},
        
        /* Also register for PlayerImpl */
        {"javax/microedition/media/PlayerImpl", "getState", "()I", native_player_getState},
        {"javax/microedition/media/PlayerImpl", "start", "()V", native_player_start},
        {"javax/microedition/media/PlayerImpl", "stop", "()V", native_player_stop},
        {"javax/microedition/media/PlayerImpl", "close", "()V", native_player_close},
        {"javax/microedition/media/PlayerImpl", "deallocate", "()V", native_player_deallocate},
        {"javax/microedition/media/PlayerImpl", "realize", "()V", native_player_realize},
        {"javax/microedition/media/PlayerImpl", "prefetch", "()V", native_player_prefetch},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered javax.microedition.media.Manager and Player native methods");
}

/*
 * Vendor-specific extensions (Siemens, Samsung, Motorola)
 */

/* Siemens Light control */
static JavaValue native_siemens_light_setColor(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint color = args[0].i;
    return NATIVE_RETURN_VOID();
}

/* Siemens Vibrator */
static JavaValue native_siemens_vibrator_start(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint duration = args[0].i;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_siemens_vibrator_stop(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

/* Generic helper functions for DRM bypass */
static JavaValue native_return_true(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_INT(1);  /* boolean true */
}

static JavaValue native_return_false(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_INT(0);  /* boolean false */
}

static JavaValue native_return_null(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_NULL();
}

/* Return empty string for DRM bypass - allows games to continue in demo mode */
static JavaValue native_return_ok_string(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;
    /* Return empty string - compareTo("ok") will return -2, triggering demo mode */
    JavaString* str = jvm_new_string(jvm, "");
    return NATIVE_RETURN_OBJECT(str);
}

static JavaValue native_return_void(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_return_zero(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_INT(0);
}

static JavaValue native_return_empty_string(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;
    JavaString* str = jvm_new_string(jvm, "");
    return NATIVE_RETURN_OBJECT(str);
}

static JavaValue native_return_identity_bytearray(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    /* Pass through args[2] as the return value (identity for byte array operations) */
    return args[2];
}

static JavaValue native_return_int_100(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_INT(100);
}

/* Samsung Vibration */
static JavaValue native_samsung_vibration_start(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint duration = args[0].i;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_samsung_vibration_stop(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

void init_vendor_extensions(JVM* jvm) {
    NativeMethodEntry methods[] = {
        /* Siemens */
        {"com/siemens/mp/game/Light", "setColor", "(I)V", native_siemens_light_setColor},
        {"com/siemens/mp/game/Vibrator", "start", "(I)V", native_siemens_vibrator_start},
        {"com/siemens/mp/game/Vibrator", "stop", "()V", native_siemens_vibrator_stop},
        /* Samsung */
        {"com/samsung/util/Vibration", "start", "(I)V", native_samsung_vibration_start},
        {"com/samsung/util/Vibration", "stop", "()V", native_samsung_vibration_stop},
        /* GlomoReg - DRM bypass (always return success) */
        {"GlomoReg/GlomoRegistrator", "CheckMidletsSecutiry", "(Ljava/lang/String;)Z", native_return_true},
        {"GlomoReg/GlomoRegistrator", "CheckMidletsSecutiry", "(Ljava/lang/String;Ljava/lang/String;)Z", native_return_true},
        {"GlomoReg/GlomoRegistrator", "CheckMidletsSecutiry", "(Ljavax/microedition/midlet/MIDlet;[Ljava/lang/String;)Ljava/lang/String;", native_return_ok_string},
        {"GlomoReg/GlomoRegistrator", "RegistrateMidlet", "(Ljava/lang/String;)V", native_return_void},
        {"GlomoReg/GlomoRegistrator", "RegistrateMidlet", "(Ljava/lang/String;Ljava/lang/String;)V", native_return_void},
        {"GlomoReg/GlomoRegistrator", "getRegDate", "()Ljava/lang/String;", native_return_null},
        {"GlomoReg/GlomoRegistrator", "getRegName", "()Ljava/lang/String;", native_return_null},
        {"GlomoReg/GlomoRegistrator", "getRegKey", "()Ljava/lang/String;", native_return_null},
        {"GlomoReg/GlomoRegistrator", "isValid", "()Z", native_return_true},
        {"GlomoReg/GlomoRegistrator", "isTrial", "()Z", native_return_false},

        /* Samsung LCDLight */
        {"com/samsung/util/LCDLight", "isSupported", "()Z", native_return_true},
        {"com/samsung/util/LCDLight", "off", "()V", native_return_void},
        {"com/samsung/util/LCDLight", "on", "(I)V", native_return_void},

        /* Samsung SMS */
        {"com/samsung/util/SMS", "isSupported", "()Z", native_return_true},
        {"com/samsung/util/SMS", "send", "(Lcom/samsung/util/SM;)V", native_return_void},

        /* Samsung SM */
        {"com/samsung/util/SM", "<init>", "()V", native_return_void},
        {"com/samsung/util/SM", "<init>", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", native_return_void},
        {"com/samsung/util/SM", "getCallbackAddress", "()Ljava/lang/String;", native_return_empty_string},
        {"com/samsung/util/SM", "getData", "()Ljava/lang/String;", native_return_empty_string},
        {"com/samsung/util/SM", "getDestAddress", "()Ljava/lang/String;", native_return_empty_string},
        {"com/samsung/util/SM", "setCallbackAddress", "(Ljava/lang/String;)V", native_return_void},
        {"com/samsung/util/SM", "setData", "(Ljava/lang/String;)V", native_return_void},
        {"com/samsung/util/SM", "setDestAddress", "(Ljava/lang/String;)V", native_return_void},

        /* Samsung AudioClip */
        {"com/samsung/util/AudioClip", "<init>", "(I[BIILjava/lang/String;)V", native_return_void},
        {"com/samsung/util/AudioClip", "<init>", "(ILjava/lang/String;)V", native_return_void},
        {"com/samsung/util/AudioClip", "isSupported", "()Z", native_return_false},
        {"com/samsung/util/AudioClip", "pause", "()V", native_return_void},
        {"com/samsung/util/AudioClip", "play", "(II)V", native_return_void},
        {"com/samsung/util/AudioClip", "resume", "()V", native_return_void},
        {"com/samsung/util/AudioClip", "stop", "()V", native_return_void},

        /* Siemens game APIs */
        {"com/siemens/mp/game/Light", "setLightOn", "()V", native_return_void},
        {"com/siemens/mp/game/Light", "setLightOff", "()V", native_return_void},
        {"com/siemens/mp/game/Vibrator", "startVibrator", "()V", native_return_void},
        {"com/siemens/mp/game/Vibrator", "stopVibrator", "()V", native_return_void},
        {"com/siemens/mp/game/Sound", "playTone", "(II)V", native_return_void},
        {"com/siemens/mp/game/Melody", "stop", "()V", native_return_void},
        {"com/siemens/mp/game/MelodyComposer", "maxLength", "()I", native_return_int_100},
        {"com/siemens/mp/game/GraphicObjectManager", "createTextureBits", "(II[B)[B", native_return_identity_bytearray},

        /* Siemens game.Sprite */
        {"com/siemens/mp/game/Sprite", "<init>", "([BIILcom/siemens/mp/game/ExtendedImage;Lcom/siemens/mp/game/ExtendedImage;I)V", native_return_void},
        {"com/siemens/mp/game/Sprite", "<init>", "(Lcom/siemens/mp/game/ExtendedImage;Lcom/siemens/mp/game/ExtendedImage;I)V", native_return_void},
        {"com/siemens/mp/game/Sprite", "<init>", "(Ljavax/microedition/lcdui/Image;Ljavax/microedition/lcdui/Image;I)V", native_return_void},
        {"com/siemens/mp/game/Sprite", "getFrame", "()I", native_return_zero},
        {"com/siemens/mp/game/Sprite", "getXPosition", "()I", native_return_zero},
        {"com/siemens/mp/game/Sprite", "getYPosition", "()I", native_return_zero},
        {"com/siemens/mp/game/Sprite", "isCollidingWith", "(Lcom/siemens/mp/game/Sprite;)Z", native_return_false},
        {"com/siemens/mp/game/Sprite", "isCollidingWithPos", "(II)Z", native_return_false},
        {"com/siemens/mp/game/Sprite", "setCollisionRectangle", "(IIII)V", native_return_void},
        {"com/siemens/mp/game/Sprite", "setFrame", "(I)V", native_return_void},
        {"com/siemens/mp/game/Sprite", "setPosition", "(II)V", native_return_void},

        /* Siemens game.MelodyComposer - additional methods */
        {"com/siemens/mp/game/MelodyComposer", "<init>", "()V", native_return_void},
        {"com/siemens/mp/game/MelodyComposer", "<init>", "([II)V", native_return_void},
        {"com/siemens/mp/game/MelodyComposer", "appendNote", "(II)V", native_return_void},
        {"com/siemens/mp/game/MelodyComposer", "length", "()I", native_return_zero},
        {"com/siemens/mp/game/MelodyComposer", "resetMelody", "()V", native_return_void},
        {"com/siemens/mp/game/MelodyComposer", "setBPM", "(I)V", native_return_void},
        {"com/siemens/mp/game/MelodyComposer", "getMelody", "()Lcom/siemens/mp/game/Melody;", native_return_null},

        /* Siemens game.Melody - additional methods */
        {"com/siemens/mp/game/Melody", "<init>", "()V", native_return_void},
        {"com/siemens/mp/game/Melody", "play", "()V", native_return_void},

        /* Siemens game.GraphicObject - visibility methods */
        {"com/siemens/mp/game/GraphicObject", "getVisible", "()Z", native_return_true},
        {"com/siemens/mp/game/GraphicObject", "setVisible", "(Z)V", native_return_void},

        /* Siemens game.GraphicObjectManager - additional methods */
        {"com/siemens/mp/game/GraphicObjectManager", "<init>", "()V", native_return_void},
        {"com/siemens/mp/game/GraphicObjectManager", "addObject", "(Lcom/siemens/mp/game/GraphicObject;)V", native_return_void},
        {"com/siemens/mp/game/GraphicObjectManager", "insertObject", "(Lcom/siemens/mp/game/GraphicObject;I)V", native_return_void},
        {"com/siemens/mp/game/GraphicObjectManager", "deleteObject", "(Lcom/siemens/mp/game/GraphicObject;)V", native_return_void},
        {"com/siemens/mp/game/GraphicObjectManager", "getObjectAt", "(I)Lcom/siemens/mp/game/GraphicObject;", native_return_null},
        {"com/siemens/mp/game/GraphicObjectManager", "getObjectPosition", "(Lcom/siemens/mp/game/GraphicObject;)I", native_return_zero},
        {"com/siemens/mp/game/GraphicObjectManager", "paint", "(Lcom/siemens/mp/game/ExtendedImage;II)V", native_return_void},
        {"com/siemens/mp/game/GraphicObjectManager", "paint", "(Ljavax/microedition/lcdui/Image;II)V", native_return_void},

        /* Siemens game.ExtendedImage - additional methods */
        {"com/siemens/mp/game/ExtendedImage", "<init>", "(Ljavax/microedition/lcdui/Image;)V", native_return_void},
        {"com/siemens/mp/game/ExtendedImage", "getImage", "()Ljavax/microedition/lcdui/Image;", native_return_null},
        {"com/siemens/mp/game/ExtendedImage", "getPixel", "(II)I", native_return_zero},
        {"com/siemens/mp/game/ExtendedImage", "setPixel", "(IIB)V", native_return_void},
        {"com/siemens/mp/game/ExtendedImage", "getPixelBytes", "([BIIII)V", native_return_void},
        {"com/siemens/mp/game/ExtendedImage", "setPixels", "([BIIII)V", native_return_void},
        {"com/siemens/mp/game/ExtendedImage", "clear", "(B)V", native_return_void},
        {"com/siemens/mp/game/ExtendedImage", "blitToScreen", "(II)V", native_return_void},

        /* Siemens game.Sprite - paint method */
        {"com/siemens/mp/game/Sprite", "paint", "(Ljavax/microedition/lcdui/Graphics;)V", native_return_void},

        /* Nokia Sound - init methods (non-constructor reinitialization) */
        {"com/nokia/mid/sound/Sound", "init", "([BI)V", native_return_void},
        {"com/nokia/mid/sound/Sound", "init", "(IJ)V", native_return_void},

        /* Nokia M3D Texture - constructor */
        {"com/nokia/mid/m3d/Texture", "<init>", "(ILjavax/microedition/lcdui/Image;)V", native_return_void},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    NATIVE_DEBUG("Registered vendor-specific extensions including GlomoReg DRM bypass");
}

/* Count stack slots needed for method arguments (long/double take 2 slots) */
int count_args(const char* descriptor) {
    if (!descriptor) return 0;
    
    int count = 0;
    const char* p = descriptor;
    
    /* Skip return type, find args */
    if (*p != '(') return 0;
    p++;
    
    while (*p && *p != ')') {
        switch (*p) {
            case 'B': case 'C': case 'D': case 'F':
            case 'I': case 'J': case 'S': case 'Z':
                count++;
                if (*p == 'J' || *p == 'D') count++;  /* Long/double take 2 slots */
                p++;
                break;
            case 'L':
                count++;
                while (*p && *p != ';') p++;
                if (*p == ';') p++;
                break;
            case '[':
                count++;
                while (*p == '[') p++;
                if (*p == 'L') {
                    while (*p && *p != ';') p++;
                    if (*p == ';') p++;
                } else {
                    p++;
                }
                break;
            default:
                p++;
                break;
        }
    }
    
    return count;
}

/* Cached version of count_args for JavaMethod */
int method_arg_count(JavaMethod* method) {
    if (!method || !method->descriptor) return 0;
    if (method->cached_arg_count >= 0) return method->cached_arg_count;
    method->cached_arg_count = (int8_t)count_args(method->descriptor);
    return method->cached_arg_count;
}

/* ИСПРАВЛЕНО: Count actual Java arguments (not stack slots) 
 * long/double count as 1 argument (not 2 stack slots) */
int count_java_args(const char* descriptor) {
    if (!descriptor) return 0;
    
    int count = 0;
    const char* p = descriptor;
    
    /* Skip return type, find args */
    if (*p != '(') return 0;
    p++;
    
    while (*p && *p != ')') {
        switch (*p) {
            case 'B': case 'C': case 'D': case 'F':
            case 'I': case 'J': case 'S': case 'Z':
                count++;  /* Каждый тип - 1 аргумент */
                p++;
                break;
            case 'L':
                count++;
                while (*p && *p != ';') p++;
                if (*p == ';') p++;
                break;
            case '[':
                count++;
                while (*p == '[') p++;
                if (*p == 'L') {
                    while (*p && *p != ';') p++;
                    if (*p == ';') p++;
                } else {
                    p++;
                }
                break;
            default:
                p++;
                break;
        }
    }
    
    return count;
}

/* Cleanup StringBuffer fallback buffers - call during JVM shutdown */
void native_stringbuffer_cleanup(void) {
    for (int i = 0; i < g_stringbuffer_fallback_count; i++) {
        if (g_stringbuffer_fallback[i].buffer != NULL) {
            free(g_stringbuffer_fallback[i].buffer);
            g_stringbuffer_fallback[i].buffer = NULL;
        }
    }
    
    /* Free the dynamic array itself */
    if (g_stringbuffer_fallback != NULL) {
        free(g_stringbuffer_fallback);
        g_stringbuffer_fallback = NULL;
    }
    
    g_stringbuffer_fallback_count = 0;
    g_stringbuffer_fallback_capacity = 0;
    NATIVE_DEBUG("Cleaned up StringBuffer fallback entries");
}

/* Cleanup all native fallback buffers */
void native_cleanup_fallbacks(void) {
    native_stringbuffer_cleanup();
    NATIVE_DEBUG("All native fallback buffers cleaned up");
}

/* GC notification - called after each GC cycle to clean up stale fallback entries.
 * This fixes the bug where StringBuffer fallback entries persist after their
 * associated objects are garbage collected, leading to string corruption when
 * new objects are allocated at the same memory addresses.
 */
void native_gc_notify(void) {
    extern bool is_heap_ptr_check(void* ptr);
    extern JVM* g_jvm_for_instanceof;
    
    int cleaned = 0;
    
    /* Clean StringBuffer fallback entries for objects that are no longer valid.
     * An object is invalid if:
     * 1. Its pointer is not in heap bounds, OR
     * 2. Its GC header indicates it's a free block or has invalid type
     */
    for (int i = 0; i < g_stringbuffer_fallback_count; i++) {
        StringBufferFallback* entry = &g_stringbuffer_fallback[i];
        
        if (entry->obj_ptr == NULL) continue;
        
        /* Check if object is still in heap */
        if (!is_heap_ptr_check(entry->obj_ptr)) {
            NATIVE_DEBUG("GC_NOTIFY: StringBuffer entry %d has invalid ptr %p (not in heap)", 
                        i, entry->obj_ptr);
            /* Free the buffer and mark entry as unused */
            if (entry->buffer) {
                free(entry->buffer);
            }
            entry->obj_ptr = NULL;
            entry->buffer = NULL;
            entry->length = 0;
            entry->capacity = 0;
            entry->alloc_id = 0;
            cleaned++;
            continue;
        }
        
        /* Check if object's GC header is valid (not freed) */
        GCObjectHeader* header = (GCObjectHeader*)entry->obj_ptr - 1;
        if (!is_heap_ptr_check(header)) {
            NATIVE_DEBUG("GC_NOTIFY: StringBuffer entry %d has invalid header %p", 
                        i, (void*)header);
            entry->obj_ptr = NULL;
            if (entry->buffer) {
                free(entry->buffer);
            }
            entry->buffer = NULL;
            entry->length = 0;
            entry->capacity = 0;
            entry->alloc_id = 0;
            cleaned++;
            continue;
        }
        
        /* Check if object is marked as free or has invalid type */
        if (header->type == OBJ_TYPE_FREE || header->type > OBJ_TYPE_CLASS) {
            NATIVE_DEBUG("GC_NOTIFY: StringBuffer entry %d object was freed (type=%d)", 
                        i, header->type);
            entry->obj_ptr = NULL;
            if (entry->buffer) {
                free(entry->buffer);
            }
            entry->buffer = NULL;
            entry->length = 0;
            entry->capacity = 0;
            entry->alloc_id = 0;
            cleaned++;
            continue;
        }
        
        /* Check if object's class is StringBuffer or StringBuilder */
        JavaObject* obj = (JavaObject*)entry->obj_ptr;
        if (!obj->header.clazz || !obj->header.clazz->class_name) {
            NATIVE_DEBUG("GC_NOTIFY: StringBuffer entry %d object has no class", i);
            entry->obj_ptr = NULL;
            if (entry->buffer) {
                free(entry->buffer);
            }
            entry->buffer = NULL;
            entry->length = 0;
            entry->capacity = 0;
            entry->alloc_id = 0;
            cleaned++;
            continue;
        }
        
        /* Verify the object is still a StringBuffer/StringBuilder */
        const char* class_name = obj->header.clazz->class_name;
        if (strcmp(class_name, "java/lang/StringBuffer") != 0 && 
            strcmp(class_name, "java/lang/StringBuilder") != 0) {
            NATIVE_DEBUG("GC_NOTIFY: StringBuffer entry %d object is now '%s' (was recycled!)", 
                        i, class_name);
            entry->obj_ptr = NULL;
            if (entry->buffer) {
                free(entry->buffer);
            }
            entry->buffer = NULL;
            entry->length = 0;
            entry->capacity = 0;
            entry->alloc_id = 0;
            cleaned++;
        }
    }
    
    if (cleaned > 0) {
        NATIVE_DEBUG("GC_NOTIFY: Cleaned %d stale StringBuffer fallback entries", cleaned);
    }
    
    /* Also clean up intern string pool */
    cleanup_intern_pool();
}
