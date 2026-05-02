/*
 * J2ME Emulator - JVM Core Implementation
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L  /* For strdup - only on POSIX systems */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "miniz.h"

#include "jvm.h"
#include "classfile.h"
#include "heap.h"
#include "threads.h"
#include "opcodes.h"
#include "native.h"
#include "debug.h"
#include "debug_macros.h"

/* Helper functions for JAR reading */
static uint16_t read_u16_jar(const uint8_t* p) {
    return p[0] | (p[1] << 8);
}

static uint32_t read_u32_jar(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

/* Find file in JAR by name - uses Central Directory for reliable reading */
static uint8_t* jar_find_file_internal(const uint8_t* jar_data, size_t jar_size, 
                                       const char* filename, size_t* out_size) {
    *out_size = 0;
    
    /* Find End of Central Directory record */
    size_t eocd_offset = 0;
    for (size_t i = jar_size - 22; i > 0; i--) {
        if (jar_data[i] == 0x50 && jar_data[i+1] == 0x4B &&
            jar_data[i+2] == 0x05 && jar_data[i+3] == 0x06) {
            eocd_offset = i;
            break;
        }
    }
    
    if (eocd_offset == 0) {
        return NULL;
    }
    
    /* Get Central Directory start offset */
    uint32_t cd_start = read_u32_jar(jar_data + eocd_offset + 16);
    uint16_t cd_entries = read_u16_jar(jar_data + eocd_offset + 10);
    
    /* Scan Central Directory entries */
    size_t cde_offset = cd_start;
    for (int i = 0; i < cd_entries && cde_offset < eocd_offset; i++) {
        uint32_t cd_sig = read_u32_jar(jar_data + cde_offset);
        if (cd_sig != 0x02014B50) break;
        
        uint16_t cd_filename_len = read_u16_jar(jar_data + cde_offset + 28);
        uint16_t cd_extra_len = read_u16_jar(jar_data + cde_offset + 30);
        uint16_t cd_comment_len = read_u16_jar(jar_data + cde_offset + 32);
        uint32_t cd_comp_size = read_u32_jar(jar_data + cde_offset + 20);
        uint32_t cd_uncomp_size = read_u32_jar(jar_data + cde_offset + 24);
        uint16_t compression = read_u16_jar(jar_data + cde_offset + 10);
        uint32_t local_header_offset = read_u32_jar(jar_data + cde_offset + 42);
        
        const char* entry_name = (const char*)(jar_data + cde_offset + 46);
        bool is_dir = (cd_filename_len > 0 && entry_name[cd_filename_len - 1] == '/');
        
        if (!is_dir && cd_filename_len == strlen(filename) &&
            memcmp(entry_name, filename, cd_filename_len) == 0) {
            
            /* Calculate data offset from local header */
            size_t local_offset = local_header_offset;
            uint16_t local_filename_len = read_u16_jar(jar_data + local_offset + 26);
            uint16_t local_extra_len = read_u16_jar(jar_data + local_offset + 28);
            size_t data_offset = local_offset + 30 + local_filename_len + local_extra_len;
            
            if (compression == 0) {
                /* Stored (uncompressed) */
                *out_size = cd_uncomp_size;
                uint8_t* data = (uint8_t*)malloc(*out_size + 1);
                if (data) {
                    memcpy(data, jar_data + data_offset, *out_size);
                    data[*out_size] = '\0';
                }
                return data;
            } else if (compression == 8) {
                /* Deflate compressed */
                if (cd_comp_size == 0 || cd_uncomp_size == 0) {
                    return NULL;
                }
                
                uint8_t* data = (uint8_t*)malloc(cd_uncomp_size + 1);
                if (!data) return NULL;
                
                z_stream stream;
                memset(&stream, 0, sizeof(stream));
                
                int ret = inflateInit2(&stream, -MAX_WBITS);
                if (ret != Z_OK) {
                    free(data);
                    return NULL;
                }
                
                stream.next_in = (Bytef*)(jar_data + data_offset);
                stream.avail_in = cd_comp_size;
                stream.next_out = data;
                stream.avail_out = cd_uncomp_size;
                
                ret = inflate(&stream, Z_FINISH);
                inflateEnd(&stream);
                
                if (ret != Z_STREAM_END) {
                    free(data);
                    return NULL;
                }
                
                data[cd_uncomp_size] = '\0';
                *out_size = cd_uncomp_size;
                return data;
            }
        }
        
        cde_offset += 46 + cd_filename_len + cd_extra_len + cd_comment_len;
    }
    
    return NULL;
}

/* Load class from JAR */
JavaClass* jvm_load_class_from_jar(JVM* jvm, const char* class_name) {
    if (!jvm || !class_name) return NULL;
    if (!jvm->class_loader.jar_data || jvm->class_loader.jar_size == 0) return NULL;
    
    /* Build filename */
    char filename[512];
    snprintf(filename, sizeof(filename), "%s.class", class_name);
    
    size_t class_size;
    uint8_t* class_data = jar_find_file_internal(jvm->class_loader.jar_data, 
                                                   jvm->class_loader.jar_size, 
                                                   filename, &class_size);
    if (!class_data) {
        fprintf(stderr, "[JAR-DBG] Class '%s' (file='%s') NOT found in JAR (jar_size=%zu)\n",
                class_name, filename, jvm->class_loader.jar_size);
        return NULL;
    }
    fprintf(stderr, "[JAR-DBG] Class '%s' found in JAR, size=%zu, parsing...\n", class_name, class_size);
    
    JavaClass* clazz = classfile_parse(jvm, class_data, class_size);
    free(class_data);
    
    if (!clazz) {
        fprintf(stderr, "[JAR-DBG] Class '%s': classfile_parse FAILED\n", class_name);
        return NULL;
    }
    fprintf(stderr, "[JAR-DBG] Class '%s': parsed OK, methods_count=%d, fields_count=%d\n",
            class_name, (int)clazz->methods_count, (int)clazz->fields_count);
    
    /* Resolve super_class reference */
    if (clazz->super_class == NULL && clazz->super_class_name != NULL) {
        JVM_DEBUG("Resolving super_class %s for %s", 
                clazz->super_class_name, clazz->class_name);
        /* Try to load super class */
        JavaClass* super = jvm_load_class(jvm, clazz->super_class_name);
        if (super) {
            clazz->super_class = super;
            JVM_DEBUG("Resolved super_class: %s -> %s", 
                    clazz->class_name, super->class_name);
        } else {
            ERROR_LOG("Failed to resolve super_class %s for %s",
                    clazz->super_class_name, clazz->class_name);
        }
    }
    
    /* === CRITICAL: Recalculate instance_size after superclass is loaded ===
     * This must be done BEFORE any instance is created.
     * The instance_size must include all inherited fields from superclasses.
     */
    jvm_recalculate_instance_size(jvm, clazz);
    
    /* Add to loaded classes */
    if (jvm->class_loader.count >= jvm->class_loader.capacity) {
        /* ИСПРАВЛЕНО: Проверка переполнения capacity */
        size_t new_capacity = jvm->class_loader.capacity * 2;
        if (new_capacity <= jvm->class_loader.capacity) {
            /* Overflow check */
            ERROR_LOG("Class loader capacity overflow");
            return NULL;
        }
        if (new_capacity > 100000) {
            /* Sanity check - max 100K classes */
            ERROR_LOG("Class loader capacity limit reached");
            return NULL;
        }
        
        JavaClass** new_classes = (JavaClass**)realloc(
            jvm->class_loader.classes, 
            new_capacity * sizeof(JavaClass*)
        );
        if (!new_classes) {
            ERROR_LOG("Failed to expand class loader array");
            return NULL;
        }
        jvm->class_loader.classes = new_classes;
        jvm->class_loader.capacity = new_capacity;
    }
    
    jvm->class_loader.classes[jvm->class_loader.count++] = clazz;
    
    /* Add to hash table for fast lookup */
    class_hash_add(clazz);

    /* Initialize header for Class object support */
    /* Find java/lang/Class and set header.clazz to it */
    JavaClass* class_class = NULL;
    for (size_t i = 0; i < jvm->class_loader.count; i++) {
        if (jvm->class_loader.classes[i]->class_name &&
            strcmp(jvm->class_loader.classes[i]->class_name, "java/lang/Class") == 0) {
            class_class = jvm->class_loader.classes[i];
            break;
        }
    }
    if (class_class) {
        clazz->header.clazz = class_class;
        clazz->header.hashcode = (jint)(uintptr_t)clazz ^ 0x5A5A5A5A;
    }

    if (jvm->config.verbose_class) {
        INFO_LOG("Loaded class from JAR: %s (super: %s)]", class_name, 
               clazz->super_class ? clazz->super_class->class_name : 
               (clazz->super_class_name ? clazz->super_class_name : "(none)"));
    }
    
    /* Log all methods for Canvas subclasses to help debug */
    if (clazz->class_name && strstr(clazz->class_name, "Canvas") != NULL) {
        JVM_DEBUG("Canvas Class %s methods (%d total):", 
                clazz->class_name, clazz->methods_count);
        for (int i = 0; i < clazz->methods_count; i++) {
            JavaMethod* m = &clazz->methods[i];
            JVM_DEBUG("  [%d] %s%s (code: %u bytes, native: %s)",
                    i, m->name, m->descriptor, m->code.code_length,
                    m->is_native ? "yes" : "no");
        }
        JVM_DEBUG("Canvas Fields (%d total):", clazz->fields_count);
        for (int i = 0; i < clazz->fields_count; i++) {
            JavaField* f = &clazz->fields[i];
            JVM_DEBUG("  [%d] %s %s", i, f->descriptor, f->name);
        }
    }
    
    return clazz;
}

/* Create JVM instance */
JVM* jvm_create(void) {
    JVM* jvm = (JVM*)calloc(1, sizeof(JVM));
    if (!jvm) return NULL;
    
    /* Set default configuration */
    jvm->config.heap_size = HEAP_INITIAL_SIZE;
    jvm->config.stack_size = JAVA_STACK_SIZE;
    jvm->config.max_threads = MAX_JAVA_THREADS;
    
    return jvm;
}

/* Destroy JVM instance */
void jvm_destroy(JVM* jvm) {
    if (!jvm) return;
    
    jvm->running = false;
    
    /* Cleanup method cache and class hash table */
    method_cache_cleanup();
    
    /* Destroy all threads */
    for (int i = 0; i < jvm->thread_count; i++) {
        if (jvm->threads[i]) {
            thread_destroy(jvm, jvm->threads[i]);
        }
    }
    
    /* Free class loader */
    if (jvm->class_loader.classes) {
        for (size_t i = 0; i < jvm->class_loader.count; i++) {
            JavaClass* clazz = jvm->class_loader.classes[i];
            if (!clazz) continue;
            
            /* For stub classes, free only what was allocated */
            if (clazz->is_stub) {
                /* Free class_name and super_class_name allocated by strdup */
                free((void*)clazz->class_name);
                free((void*)clazz->super_class_name);
                
                /* Free constant pool UTF8 entries */
                if (clazz->constant_pool) {
                    for (uint16_t j = 1; j < clazz->constant_pool_count; j++) {
                        if (clazz->constant_pool[j].tag == CONSTANT_Utf8) {
                            free(clazz->constant_pool[j].info.utf8.bytes);
                        }
                    }
                    free(clazz->constant_pool);
                }
                
                /* Free methods - stub methods have allocated name/descriptor */
                if (clazz->methods) {
                    for (uint16_t j = 0; j < clazz->methods_count; j++) {
                        free((void*)clazz->methods[j].name);
                        free((void*)clazz->methods[j].descriptor);
                        free(clazz->methods[j].code.code);
                    }
                    free(clazz->methods);
                }
                
                /* Free fields */
                if (clazz->fields) {
                    for (uint16_t j = 0; j < clazz->fields_count; j++) {
                        free((void*)clazz->fields[j].name);
                        free((void*)clazz->fields[j].descriptor);
                    }
                    free(clazz->fields);
                }
                
                free(clazz);
            } else {
                /* Use regular classfile_free for non-stub classes */
                classfile_free(clazz);
            }
        }
        free(jvm->class_loader.classes);
    }
    
    if (jvm->class_loader.class_path) {
        for (size_t i = 0; i < jvm->class_loader.class_path_count; i++) {
            free(jvm->class_loader.class_path[i]);
        }
        free(jvm->class_loader.class_path);
    }
    
    /* Free jar_path if allocated */
    free((void*)jvm->class_loader.jar_path);
    
    /* Free string pool - only free utf8 as chars is part of heap allocation */
    if (jvm->string_pool.strings) {
        for (size_t i = 0; i < jvm->string_pool.count; i++) {
            JavaString* str = jvm->string_pool.strings[i];
            if (str) {
                /* utf8 is allocated separately with strdup */
                free(str->utf8);
                /* chars is part of the heap allocation, don't free separately */
                /* The whole JavaString struct is freed by heap_destroy */
            }
        }
        free(jvm->string_pool.strings);
    }
    
    /* Destroy heap - this frees all objects including JavaString structs */
    heap_destroy(jvm);
    
    /* Cleanup native fallback buffers */
    native_cleanup_fallbacks();
    
    /* Cleanup pthread monitor resources */
    cleanup_monitors();
    
    free(jvm);
}

/* Initialize JVM */
int jvm_init(JVM* jvm) {
    if (!jvm) return JNI_ERR;
    
    /* Set global JVM pointer for object_instance_of */
    extern JVM* g_jvm_for_instanceof;
    g_jvm_for_instanceof = jvm;
    
    /* Initialize method cache and class hash table */
    method_cache_init();
    
    /* Initialize heap */
    if (heap_init(jvm, jvm->config.heap_size, HEAP_MAX_SIZE) != JNI_OK) {
        ERROR_LOG("Failed to initialize heap");
        return JNI_ERR;
    }
    
    /* Initialize thread system */
    if (threads_init(jvm) != JNI_OK) {
        ERROR_LOG("Failed to initialize threads");
        return JNI_ERR;
    }
    
    /* Create main thread */
    jvm->main_thread = thread_create(jvm, "main", THREAD_PRIORITY_NORM, NULL);
    if (!jvm->main_thread) {
        ERROR_LOG("Failed to create main thread");
        return JNI_ERR;
    }
    
    jvm->threads[0] = jvm->main_thread;
    jvm->thread_count = 1;
    
    /* Initialize class loader */
    jvm->class_loader.capacity = 256;
    jvm->class_loader.classes = (JavaClass**)calloc(jvm->class_loader.capacity, sizeof(JavaClass*));
    if (!jvm->class_loader.classes) {
        return JNI_ERR;
    }
    
    /* Initialize string pool */
    jvm->string_pool.capacity = 256;
    jvm->string_pool.strings = (JavaString**)calloc(jvm->string_pool.capacity, sizeof(JavaString*));
    if (!jvm->string_pool.strings) {
        return JNI_ERR;
    }
    
    /* Initialize built-in stub classes for J2ME API */
    int stub_count = init_stub_classes(jvm);
    if (jvm->config.verbose_class) {
        printf("[JVM] Initialized %d stub classes\n", stub_count);
    }
    
    jvm->running = true;
    
    return JNI_OK;
}

/* Set JAR data for class loading */
void jvm_set_jar_data(JVM* jvm, uint8_t* data, size_t size, const char* path) {
    if (!jvm) return;
    jvm->class_loader.jar_data = data;
    jvm->class_loader.jar_size = size;
    jvm->class_loader.jar_path = path ? strdup(path) : NULL;
}

/* Load a class by name - OPTIMIZED with hash table */
JavaClass* jvm_load_class(JVM* jvm, const char* class_name) {
    if (!jvm || !class_name) return NULL;
    
    /* Step 1: Check hash table first (O(1) average) */
    JavaClass* clazz = class_hash_lookup(class_name);
    if (clazz) {
        /* Debug: log stub lookups for key game classes (throttled) */
        static int dbg_count = 0;
        if (strcmp(class_name, "ap") == 0 || strcmp(class_name, "w") == 0 ||
            strcmp(class_name, "t") == 0 || strcmp(class_name, "as") == 0) {
            if (dbg_count++ < 5)
                fprintf(stderr, "[CLASS-DBG] '%s' found in hash table (methods=%d, fields=%d, is_stub=%d)\n",
                        class_name, (int)clazz->methods_count, (int)clazz->fields_count,
                        clazz->is_stub ? 1 : 0);
        }
        /* Resolve super_class if not already done */
        if (clazz->super_class == NULL && clazz->super_class_name != NULL) {
            clazz->super_class = jvm_load_class(jvm, clazz->super_class_name);
        }
        return clazz;
    }
    
    /* Step 2: Fallback to linear search (should rarely happen) */
    for (size_t i = 0; i < jvm->class_loader.count; i++) {
        JavaClass* c = jvm->class_loader.classes[i];
        if (c->class_name && strcmp(c->class_name, class_name) == 0) {
            /* Add to hash table for future lookups */
            class_hash_add(c);
            return c;
        }
    }
    
    /* Try to load from JAR first if JAR data is available */
    if (jvm->class_loader.jar_data && jvm->class_loader.jar_size > 0) {
        clazz = jvm_load_class_from_jar(jvm, class_name);
        if (clazz) return clazz;
    }
    
    /* Convert class name to path */
    char* path = class_name_to_internal(class_name);
    if (!path) return NULL;
    
    /* Try to load from current directory */
    char filename[1024];
    snprintf(filename, sizeof(filename), "%s.class", path);
    
    clazz = classfile_parse_file(jvm, filename);
    
    free(path);
    
    if (!clazz) {
        /* Class not found in filesystem - create or get a stub class */
        fprintf(stderr, "[CLASS-DBG] Class '%s' not found in JAR or filesystem, creating stub\n", class_name);
        clazz = get_or_create_stub_class(jvm, class_name);
        if (!clazz) {
            WARN_LOG("Class not found and stub creation failed: %s", class_name);
            return NULL;
        }
        return clazz;  /* Stub already added to class loader */
    }
    
    /* Add to loaded classes */
    if (jvm->class_loader.count >= jvm->class_loader.capacity) {
        size_t new_capacity = jvm->class_loader.capacity * 2;
        if (new_capacity <= jvm->class_loader.capacity || new_capacity > 100000) {
            ERROR_LOG("Class loader capacity overflow or limit reached");
            free(clazz);
            return NULL;
        }
        
        JavaClass** new_classes = (JavaClass**)realloc(
            jvm->class_loader.classes, 
            new_capacity * sizeof(JavaClass*)
        );
        if (!new_classes) {
            ERROR_LOG("Failed to expand class loader array");
            free(clazz);
            return NULL;
        }
        jvm->class_loader.classes = new_classes;
        jvm->class_loader.capacity = new_capacity;
    }
    
    jvm->class_loader.classes[jvm->class_loader.count++] = clazz;
    
    /* Add to hash table for future lookups */
    class_hash_add(clazz);

    if (jvm->config.verbose_class) {
        INFO_LOG("Loaded class: %s", class_name);
    }
    
    return clazz;
}

/* Define a class from raw bytes */
JavaClass* jvm_define_class(JVM* jvm, const uint8_t* data, size_t length) {
    if (!jvm || !data || length == 0) return NULL;
    
    JavaClass* clazz = classfile_parse(jvm, data, length);
    if (!clazz) return NULL;
    
    /* Resolve class name */
    if (clazz->this_class > 0) {
        clazz->class_name = (char*)classfile_get_class_name(clazz, clazz->this_class);
    }
    
    /* Add to loaded classes */
    if (jvm->class_loader.count >= jvm->class_loader.capacity) {
        /* ИСПРАВЛЕНО: Проверка переполнения capacity */
        size_t new_capacity = jvm->class_loader.capacity * 2;
        if (new_capacity <= jvm->class_loader.capacity || new_capacity > 100000) {
            ERROR_LOG("Class loader capacity overflow or limit reached");
            classfile_free(clazz);
            return NULL;
        }
        
        JavaClass** new_classes = (JavaClass**)realloc(
            jvm->class_loader.classes, 
            new_capacity * sizeof(JavaClass*)
        );
        if (!new_classes) {
            ERROR_LOG("Failed to expand class loader array");
            classfile_free(clazz);
            return NULL;
        }
        jvm->class_loader.classes = new_classes;
        jvm->class_loader.capacity = new_capacity;
    }
    
    jvm->class_loader.classes[jvm->class_loader.count++] = clazz;
    
    /* Recalculate instance size based on fields and superclass */
    jvm_recalculate_instance_size(jvm, clazz);
    
    return clazz;
}

/* Resolve a method in a class - uses optimized cache lookup */
JavaMethod* jvm_resolve_method(JVM* jvm, JavaClass* clazz, const char* name, const char* descriptor) {
    if (!clazz || !name || !descriptor) return NULL;
    
    /* Use fast method cache lookup */
    JavaMethod* method = method_cache_lookup(clazz, name, descriptor);
    if (method) return method;
    
    /* Linear search in class methods */
    for (int i = 0; i < clazz->methods_count; i++) {
        JavaMethod* m = &clazz->methods[i];
        if (m->name && m->descriptor &&
            strcmp(m->name, name) == 0 && 
            strcmp(m->descriptor, descriptor) == 0) {
            /* Cache the result */
            method_cache_store(clazz, name, descriptor, m);
            return m;
        }
    }
    
    /* Search in superclass */
    if (clazz->super_class) {
        method = jvm_resolve_method(jvm, clazz->super_class, name, descriptor);
        if (method) {
            /* Cache the result for the original class too */
            method_cache_store(clazz, name, descriptor, method);
        }
        return method;
    }
    
    return NULL;
}

/* Resolve a field in a class */
JavaField* jvm_resolve_field(JVM* jvm, JavaClass* clazz, const char* name, const char* descriptor) {
    if (!clazz || !name || !descriptor) return NULL;
    
    /* Search in class fields */
    for (int i = 0; i < clazz->fields_count; i++) {
        JavaField* field = &clazz->fields[i];
        if (field->name && field->descriptor &&
            strcmp(field->name, name) == 0 &&
            strcmp(field->descriptor, descriptor) == 0) {
            return field;
        }
    }
    
    /* Search in superclass */
    if (clazz->super_class) {
        return jvm_resolve_field(jvm, clazz->super_class, name, descriptor);
    }
    
    return NULL;
}

/* Create new object */
JavaObject* jvm_new_object(JVM* jvm, JavaClass* clazz) {
    if (!jvm || !clazz) return NULL;
    
    /* Initialize class if needed - recalculate instance size from superclass chain
     * but do NOT run <clinit> here (we may be in a context where we can't safely
     * execute bytecode, e.g. during exception creation). Just ensure the size is
     * correct so the allocation doesn't corrupt the heap. */
    if (!clazz->initialized && !clazz->initializing) {
        jvm_recalculate_instance_size(jvm, clazz);
        /* Do NOT set clazz->initialized = true without running <clinit>! */
    }
    
    if (jvm->config.verbose_class) {
        printf("[JVM] Creating object of class: %s\n", clazz->class_name);
    }
    
    return heap_alloc_object(jvm, clazz);
}

/* Create new array */
JavaArray* jvm_new_array(JVM* jvm, uint8_t type, jsize length, JavaClass* element_class) {
    return heap_alloc_array(jvm, type, length, element_class);
}

/* Create new string */
JavaString* jvm_new_string(JVM* jvm, const char* utf8) {
    if (!jvm || !utf8) return NULL;
    
    size_t utf8_len = strlen(utf8);
    
    /* First pass: count UTF-16 code units */
    size_t utf16_len = 0;
    for (size_t i = 0; i < utf8_len; ) {
        uint8_t c = (uint8_t)utf8[i];
        if (c < 0x80) {
            /* ASCII: 1 byte -> 1 UTF-16 code unit */
            utf16_len++;
            i++;
        } else if ((c & 0xE0) == 0xC0) {
            /* 2-byte UTF-8 sequence -> 1 UTF-16 code unit */
            utf16_len++;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            /* 3-byte UTF-8 sequence -> 1 UTF-16 code unit */
            utf16_len++;
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            /* 4-byte UTF-8 sequence -> 2 UTF-16 code units (surrogate pair) */
            utf16_len += 2;
            i += 4;
        } else {
            /* Invalid UTF-8 byte, skip */
            i++;
        }
    }
    
    /* Allocate UTF-16 buffer */
    jchar* chars = (jchar*)malloc(utf16_len * sizeof(jchar));
    if (!chars) return NULL;
    
    /* Second pass: decode UTF-8 to UTF-16 */
    size_t j = 0;
    for (size_t i = 0; i < utf8_len && j < utf16_len; ) {
        uint8_t c = (uint8_t)utf8[i];
        if (c < 0x80) {
            /* ASCII */
            chars[j++] = (jchar)c;
            i++;
        } else if ((c & 0xE0) == 0xC0) {
            /* 2-byte sequence: 110xxxxx 10xxxxxx */
            if (i + 1 < utf8_len) {
                uint32_t cp = ((c & 0x1F) << 6) | ((uint8_t)utf8[i+1] & 0x3F);
                chars[j++] = (jchar)cp;
                i += 2;
            } else {
                i++;
            }
        } else if ((c & 0xF0) == 0xE0) {
            /* 3-byte sequence: 1110xxxx 10xxxxxx 10xxxxxx */
            if (i + 2 < utf8_len) {
                uint32_t cp = ((c & 0x0F) << 12) | 
                             (((uint8_t)utf8[i+1] & 0x3F) << 6) | 
                             ((uint8_t)utf8[i+2] & 0x3F);
                chars[j++] = (jchar)cp;
                i += 3;
            } else {
                i++;
            }
        } else if ((c & 0xF8) == 0xF0) {
            /* 4-byte sequence: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx */
            if (i + 3 < utf8_len) {
                uint32_t cp = ((c & 0x07) << 18) | 
                             (((uint8_t)utf8[i+1] & 0x3F) << 12) | 
                             (((uint8_t)utf8[i+2] & 0x3F) << 6) | 
                             ((uint8_t)utf8[i+3] & 0x3F);
                /* Encode as UTF-16 surrogate pair */
                cp -= 0x10000;
                chars[j++] = (jchar)(0xD800 | (cp >> 10));
                chars[j++] = (jchar)(0xDC00 | (cp & 0x3FF));
                i += 4;
            } else {
                i++;
            }
        } else {
            /* Invalid UTF-8 byte, skip */
            i++;
        }
    }
    
    JavaString* str = jvm_new_string_utf16(jvm, chars, (jsize)utf16_len);
    free(chars);
    
    /* CRITICAL FIX: Only set utf8 cache for native strings (OBJ_TYPE_STRING).
     * For Java String objects (OBJ_TYPE_OBJECT), the JavaString struct fields
     * (length, hash, utf8, chars) overlap with the fields[] array!
     * Setting str->utf8 would corrupt the value/offset/count/hash fields.
     * 
     * How to detect: native strings have clazz=NULL, Java strings have clazz=java/lang/String
     */
    if (str && str->header.clazz == NULL) {
        /* Native string (no java/lang/String class loaded) - safe to use struct fields */
        str->utf8 = strdup(utf8);
    }
    
    return str;
}

/* Create new string from UTF-16
 * 
 * CRITICAL FIX: Правильная модель Java String!
 * Если класс java/lang/String загружен, создаем OBJ_TYPE_OBJECT с полями и отдельный char[] массив для value.
 * Иначе использу inline хранилище как раньше.
 */
JavaString* jvm_new_string_utf16(JVM* jvm, const jchar* chars, jsize length) {
    if (!jvm || !chars || length < 0) return NULL;
    
    /* Проверить, загружен ли класс java/lang/String */
    JavaClass* str_class = NULL;
    for (size_t i = 0; i < jvm->class_loader.count; i++) {
        JavaClass* c = jvm->class_loader.classes[i];
        if (c->class_name && strcmp(c->class_name, "java/lang/String") == 0) {
            str_class = c;
            break;
        }
    }
    
    if (str_class) {
        /* Java String: создаем как OBJ_TYPE_OBJECT с полями value, offset, count, hash
         * КРИТИЧЕСКИ ВАЖНО: Используем OBJ_TYPE_OBJECT, так как у нас есть fields[]
         * OBJ_TYPE_STRING используется только для нативных строк без fields[]
         * 
         * ВАЖНО: Для Java строк мы НЕ можем использовать поля JavaString (length, hash, utf8, chars),
         * так как они перекрываются с fields[]! Мы должны использовать только fields[].
         */
        JavaObject* str = (JavaObject*)heap_alloc(jvm, str_class->instance_size, str_class, OBJ_TYPE_OBJECT);
        if (!str) return NULL;
        
        /* Создаем char[] массив для value field */
        JavaArray* char_array = heap_alloc_array(jvm, T_CHAR, length, NULL);
        if (!char_array) {
            return NULL;
        }
        
        /* Копируем данные в массив */
        jchar* array_data = (jchar*)((uint8_t*)char_array + sizeof(JavaArray));
        memcpy(array_data, chars, length * sizeof(jchar));
        
        /* Устанавливаем поля через функции из native.c */
        int value_slot = native_get_string_value_slot(jvm);
        int offset_slot = native_get_string_offset_slot(jvm);
        int count_slot = native_get_string_count_slot(jvm);
        int hash_slot = native_get_string_hash_slot(jvm);
        
        if (value_slot >= 0) {
            JavaValue v = { .ref = char_array };
            str->fields[value_slot] = v;
        }
        if (offset_slot >= 0) {
            JavaValue v = { .i = 0 };
            str->fields[offset_slot] = v;
        }
        if (count_slot >= 0) {
            JavaValue v = { .i = (jint)length };
            str->fields[count_slot] = v;
        }
        if (hash_slot >= 0) {
            JavaValue v = { .i = 0 };
            str->fields[hash_slot] = v;
        }
        
        /* НЕ устанавливаем str->length, str->chars и т.д. - они перекрываются с fields[]!
         * Используем только fields[] для доступа к данным Java строки.
         * string_length() и string_chars() знают как работать с OBJ_TYPE_OBJECT. */
        
        return (JavaString*)str;
    } else {
        /* Нативная строка (класс не загружен) */
        JavaString* str = (JavaString*)heap_alloc_string(jvm, length);
        if (!str) return NULL;
        
        /* Копируем данные inline после JavaString */
        jchar* str_chars = (jchar*)((uint8_t*)str + sizeof(JavaString));
        memcpy(str_chars, chars, length * sizeof(jchar));
        str->length = length;
        str->chars = NULL;  /* Native strings use inline storage */
        
        return str;
    }
}

/* Throw an exception */
void jvm_throw(JVM* jvm, JavaObject* exception) {
    if (!jvm || !exception) return;
    
    JavaThread* thread = jvm_current_thread(jvm);
    if (thread) {
        thread->pending_exception = exception;
    }
}

/* Throw exception by class name */
void jvm_throw_by_name(JVM* jvm, const char* class_name, const char* message) {
    if (!jvm || !class_name) return;
    
    JavaClass* clazz = jvm_load_class(jvm, class_name);
    if (!clazz) {
        ERROR_LOG("Exception class not found: %s", class_name);
        return;
    }
    
    JavaObject* exception = jvm_new_object(jvm, clazz);
    if (!exception) return;
    
    /* TODO: Set message field */
    (void)message;
    
    jvm_throw(jvm, exception);
}

/* Check for pending exception */
JavaObject* jvm_exception_pending(JVM* jvm) {
    JavaThread* thread = jvm_current_thread(jvm);
    return thread ? thread->pending_exception : NULL;
}

/* Clear pending exception */
JavaObject* jvm_exception_clear(JVM* jvm) {
    JavaThread* thread = jvm_current_thread(jvm);
    if (!thread) return NULL;
    
    JavaObject* ex = thread->pending_exception;
    thread->pending_exception = NULL;
    return ex;
}

/* Get current thread */
JavaThread* jvm_current_thread(JVM* jvm) {
    if (!jvm) return NULL;
    /* Use the scheduler's current thread instead of always returning main_thread */
    return thread_current(jvm);
}

/* Allocate memory */
void* jvm_alloc(JVM* jvm, size_t size) {
    return heap_alloc(jvm, size, NULL, OBJ_TYPE_OBJECT);
}

/* Run garbage collection */
void jvm_gc(JVM* jvm) {
    gc_collect(jvm);
}

/* Add GC root */
void jvm_add_root(JVM* jvm, JavaObject* obj) {
    gc_add_root(jvm, (void**)&obj);
}

/* Remove GC root */
void jvm_remove_root(JVM* jvm, JavaObject* obj) {
    gc_remove_root(jvm, (void**)&obj);
}

/* Convert string to UTF-8 */
char* jvm_string_to_utf8(JVM* jvm, JavaString* str) {
    (void)jvm;
    if (!str) return NULL;
    
    if (str->utf8) return strdup(str->utf8);
    
    /* Get chars from inline storage */
    const jchar* chars = (const jchar*)((uint8_t*)str + sizeof(JavaString));
    
    /* Convert UTF-16 to UTF-8 */
    char* result = (char*)malloc(str->length + 1);
    if (!result) return NULL;
    
    for (jsize i = 0; i < str->length; i++) {
        result[i] = (char)(chars[i] & 0xFF);
    }
    result[str->length] = '\0';
    
    return result;
}

/* Utility functions */
uint16_t jvm_read_u16(const uint8_t* data) {
    return ((uint16_t)data[0] << 8) | data[1];
}

uint32_t jvm_read_u32(const uint8_t* data) {
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | data[3];
}

uint64_t jvm_read_u64(const uint8_t* data) {
    return ((uint64_t)data[0] << 56) | ((uint64_t)data[1] << 48) |
           ((uint64_t)data[2] << 40) | ((uint64_t)data[3] << 32) |
           ((uint64_t)data[4] << 24) | ((uint64_t)data[5] << 16) |
           ((uint64_t)data[6] << 8) | data[7];
}

int16_t jvm_read_s16(const uint8_t* data) {
    return (int16_t)jvm_read_u16(data);
}

int32_t jvm_read_s32(const uint8_t* data) {
    return (int32_t)jvm_read_u32(data);
}

int64_t jvm_read_s64(const uint8_t* data) {
    return (int64_t)jvm_read_u64(data);
}

/* Class name conversion */
char* class_name_to_java(const char* internal_name) {
    if (!internal_name) return NULL;
    
    size_t len = strlen(internal_name);
    char* result = (char*)malloc(len + 1);
    if (!result) return NULL;
    
    for (size_t i = 0; i < len; i++) {
        result[i] = (internal_name[i] == '/') ? '.' : internal_name[i];
    }
    result[len] = '\0';
    
    return result;
}

char* class_name_to_internal(const char* java_name) {
    if (!java_name) return NULL;
    
    size_t len = strlen(java_name);
    char* result = (char*)malloc(len + 1);
    if (!result) return NULL;
    
    for (size_t i = 0; i < len; i++) {
        result[i] = (java_name[i] == '.') ? '/' : java_name[i];
    }
    result[len] = '\0';
    
    return result;
}
