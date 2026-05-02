/*
 * J2ME Emulator - Native Methods
 * Native method implementation for Java classes
 */

#ifndef NATIVE_H
#define NATIVE_H

#include "jvm.h"

/*
 * Native method signature
 */
typedef JavaValue (*NativeMethod)(JVM* jvm, JavaThread* thread, 
                                   JavaValue* args, int arg_count);

/*
 * Native method registration
 */
typedef struct {
    const char* class_name;
    const char* method_name;
    const char* descriptor;
    NativeMethod handler;
} NativeMethodEntry;

/*
 * Initialize native methods
 */
int native_init(JVM* jvm);

/* Register a native method */
int native_register(JVM* jvm, const char* class_name, 
                    const char* method_name, const char* descriptor,
                    NativeMethod handler);

/* Register multiple native methods */
int native_register_methods(JVM* jvm, const NativeMethodEntry* methods, int count);

/* Find native method */
NativeMethod native_find(JVM* jvm, const char* class_name,
                         const char* method_name, const char* descriptor);

/*
 * Standard Java native methods
 */

/* java.lang.Object */
void init_java_lang_object(JVM* jvm);

/* java.lang.Class */
void init_java_lang_class(JVM* jvm);

/* java.lang.String */
void init_java_lang_string(JVM* jvm);

/* java.lang.System */
void init_java_lang_system(JVM* jvm);

/* java.lang.Runtime */
void init_java_lang_runtime(JVM* jvm);

/* java.lang.Math */
void init_java_lang_math(JVM* jvm);

/* java.lang.Thread */
void init_java_lang_thread(JVM* jvm);

/* java.lang.Throwable */
void init_java_lang_throwable(JVM* jvm);

/*
 * Utility functions for native methods
 */

/* Get argument at index */
#define NATIVE_ARG(args, index, type) ((type)(args[index].raw))

/* Get object argument */
#define NATIVE_ARG_OBJECT(args, index) ((JavaObject*)args[index].ref)

/* Get string argument (converted to UTF-8) */
const char* native_get_string_utf8(JVM* jvm, JavaValue* args, int index);

/* Return void */
#define NATIVE_RETURN_VOID() (JavaValue){ .raw = 0 }

/* Return int */
#define NATIVE_RETURN_INT(val) (JavaValue){ .i = (val) }

/* Return long */
#define NATIVE_RETURN_LONG(val) (JavaValue){ .j = (val) }

/* Return float */
#define NATIVE_RETURN_FLOAT(val) (JavaValue){ .f = (val) }

/* Return double */
#define NATIVE_RETURN_DOUBLE(val) (JavaValue){ .d = (val) }

/* Return object */
#define NATIVE_RETURN_OBJECT(obj) (JavaValue){ .ref = (obj) }

/* Return null */
#define NATIVE_RETURN_NULL() (JavaValue){ .ref = NULL }

/*
 * Exception throwing helpers for native methods
 */

/* Throw NullPointerException */
void native_throw_npe(JVM* jvm, JavaThread* thread);

/* Throw ArrayIndexOutOfBoundsException */
void native_throw_aioobe(JVM* jvm, JavaThread* thread, jint index);

/* Throw ClassNotFoundException */
void native_throw_cnfe(JVM* jvm, JavaThread* thread, const char* name);

/* Throw OutOfMemoryError */
void native_throw_oome(JVM* jvm, JavaThread* thread);

/* Throw IllegalArgumentException */
void native_throw_iae(JVM* jvm, JavaThread* thread, const char* message);

/* Throw NegativeArraySizeException */
void native_throw_negative_array_size(JVM* jvm, JavaThread* thread);

/* Throw ArrayStoreException */
void native_throw_array_store_exception(JVM* jvm, JavaThread* thread);

/* Throw IOException */
void native_throw_ioe(JVM* jvm, JavaThread* thread, const char* message);

/*
 * Call a native method
 */
int native_call(JVM* jvm, JavaThread* thread, JavaMethod* method, 
                JavaValue* args, JavaValue* result);

/*
 * Stub class generation for J2ME API
 */

/* Initialize all built-in stub classes */
int init_stub_classes(JVM* jvm);

/* Get or create a stub class by name */
JavaClass* get_or_create_stub_class(JVM* jvm, const char* class_name);

/*
 * Count arguments in method descriptor
 */
int count_args(const char* descriptor);

/*
 * Cached argument count for JavaMethod (uses cached_arg_count field)
 */
int method_arg_count(JavaMethod* method);

/*
 * Field access helpers - properly calculate field slots accounting for:
 * - Static fields not stored in instances
 * - Superclass fields coming first
 * - Long/double taking 2 slots
 */

/* Get field value by name */
JavaValue native_get_field_value(JavaObject* obj, const char* field_name);

/* Set field value by name */
void native_set_field_value(JavaObject* obj, const char* field_name, JavaValue value);

/* Find field slot by name - returns -1 if not found */
int native_get_field_slot(JavaClass* clazz, const char* field_name);

/*
 * String field slot accessors - for use by heap.c string functions
 * These initialize the field slots on first call
 */

/* Get String value field slot (char[] value) */
int native_get_string_value_slot(JVM* jvm);

/* Get String count field slot (int count) */
int native_get_string_count_slot(JVM* jvm);

/* Get String hash field slot (int hash) */
int native_get_string_hash_slot(JVM* jvm);

/* Get String offset field slot (int offset) */
int native_get_string_offset_slot(JVM* jvm);

/*
 * Cleanup functions - call during JVM shutdown
 */

/* Cleanup all native fallback buffers (StringBuffer, StringBuilder, etc.) */
void native_cleanup_fallbacks(void);

/*
 * MIDlet manifest support
 */

/* Set manifest data for getAppProperty - call after loading JAR */
void midlet_set_manifest(const char* manifest_data, size_t size);

/* Append JAD data to manifest - call after loading JAD */
void midlet_append_manifest(const char* data);

/* Add a single property to manifest */
void midlet_add_property(const char* key, const char* value);

/* GC notification - called after each GC cycle to clean up stale fallback entries.
 * This fixes the bug where StringBuffer fallback entries persist after their
 * associated objects are garbage collected, leading to string corruption.
 */
void native_gc_notify(void);

/*
 * String interning - for automatic interning of string literals
 */

/* Intern a string - returns the canonical representation */
JavaString* native_intern_string(JVM* jvm, JavaString* str);

/* Look up an interned string by UTF-8 content — returns existing interned string or NULL */
JavaString* native_intern_find_by_utf8(const char* utf8, jsize utf8_len);

#endif /* NATIVE_H */
