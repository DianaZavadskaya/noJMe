/*
 * J2ME Emulator - Heap and Garbage Collector
 * Memory management for Java objects
 */

#ifndef HEAP_H
#define HEAP_H

#include "jvm.h"

/*
 * Object types for GC
 */
typedef enum {
    OBJ_TYPE_OBJECT,
    OBJ_TYPE_ARRAY,
    OBJ_TYPE_STRING,
    OBJ_TYPE_CLASS,
    OBJ_TYPE_FREE    /* Free block in heap (not a live object) */
} ObjectType;

/*
 * Object header for GC tracking
 * CRITICAL: Layout must be compatible with FreeBlock for recycling!
 * 
 * FreeBlock = { next*, uint32_t size, uint32_t _reserved }
 * On 32-bit: next=4bytes at offset 0, size=4bytes at offset 4
 * On 64-bit: next=8bytes at offset 0, size=4bytes at offset 8, _reserved=4bytes at offset 12
 * 
 * GCObjectHeader MUST have size at the same offset as FreeBlock!
 * The key insight: size field must be at the SAME offset in both structures.
 * 
 * CRITICAL FIX: sizeof(GCObjectHeader) must be multiple of 8!
 * This ensures that user data (ptr = header + 1) is aligned to 8 bytes.
 * 
 * On 32-bit: 32 bytes (next=4, size=4, type=4, marked=1, pinned=1, reserved=2, _align_pad=8, clazz=4, magic=4)
 * On 64-bit: 40 bytes (next=8, size=4+pad=8, type=4, marked=1, pinned=1, reserved=2, _align_pad=8, clazz=8, magic=4+pad=8)
 */
typedef struct GCObjectHeader {
    struct GCObjectHeader* next;    /* Next object in heap - offset 0 */
    uint32_t size;                  /* Object size - offset 4 (32-bit) / 8 (64-bit) */
    ObjectType type;                /* Object type - offset 8 (32-bit) / 12 (64-bit) */
    uint8_t marked;                 /* GC mark flag - offset 12 (32-bit) / 16 (64-bit) */
    uint8_t pinned;                 /* Cannot be moved - offset 13 (32-bit) / 17 (64-bit) */
    uint8_t reserved[2];            /* Padding - offset 14-15 (32-bit) / 18-19 (64-bit) */
    uint8_t _align_pad[8];          /* PADDING for 8-byte alignment - offset 16-23 (32-bit) / 20-27 (64-bit) */
    JavaClass* clazz;               /* Object's class - offset 24 (32-bit) / 28 (64-bit) */
    
    /* MAGIC number for detecting memory corruption */
    uint32_t magic;                 /* Magic: 0xDEADBEEF - if corrupted, memory was overwritten */
    uint32_t _magic_pad;            /* Padding to make total size multiple of 8 */
} GCObjectHeader;                   /* Total: 32 bytes (32-bit) / 40 bytes (64-bit) */

#define GC_HEADER_MAGIC 0xDEADBEEF

/*
 * FreeBlock structure for free list management
 * CRITICAL: Must have size field at SAME offset as GCObjectHeader!
 * 
 * On 32-bit: next(4) at offset 0, size(4) at offset 4 - matches GCObjectHeader
 * On 64-bit: next(8) at offset 0, size(4) at offset 8 - matches GCObjectHeader!
 *           _reserved(4) at offset 12 for alignment
 * 
 * This ensures binary compatibility when casting between FreeBlock* and GCObjectHeader*
 */
typedef struct FreeBlock {
    struct FreeBlock* next;         /* Next free block - offset 0 */
    uint32_t size;                  /* Block size - offset 4 (32-bit) / 8 (64-bit) - MUST match GCObjectHeader! */
    uint32_t _reserved;             /* Reserved for alignment on 64-bit - offset 8 (32-bit) / 12 (64-bit) */
} FreeBlock;

/*
 * Heap statistics
 */
typedef struct {
    size_t total_size;
    size_t used_size;
    size_t free_size;
    size_t object_count;
    size_t gc_cycles;
    size_t gc_time_ms;
} HeapStats;

/*
 * Heap initialization
 */
int heap_init(JVM* jvm, size_t initial_size, size_t max_size);
void heap_destroy(JVM* jvm);

/*
 * Object allocation
 */
void* heap_alloc(JVM* jvm, size_t size, JavaClass* clazz, ObjectType type);

/* Allocate object */
JavaObject* heap_alloc_object(JVM* jvm, JavaClass* clazz);

/* Allocate array - element_class is the class of array elements for object arrays, NULL for primitives */
JavaArray* heap_alloc_array(JVM* jvm, uint8_t element_type, jsize length, JavaClass* element_class);

/* Allocate string */
JavaString* heap_alloc_string(JVM* jvm, jsize length);

/*
 * Garbage Collection
 */

/* Run garbage collection */
void gc_collect(JVM* jvm);

/* Add GC root */
void gc_add_root(JVM* jvm, void** root);

/* Remove GC root */
void gc_remove_root(JVM* jvm, void** root);

/* Pin object (prevent GC from moving it) */
void gc_pin(JVM* jvm, void* object);

/* Unpin object */
void gc_unpin(JVM* jvm, void* object);

/* Get heap statistics */
HeapStats heap_get_stats(JVM* jvm);

/*
 * Object operations
 */

/* Get object class */
JavaClass* object_get_class(void* object);

/* Get object size */
size_t object_get_size(void* object);

/* Get object hash code */
jint object_hash_code(void* object);

/* Check if object is an array */
bool object_is_array(void* object);

/* Check if object is a string */
bool object_is_string(void* object);

/* Check if object is instance of class */
bool object_instance_of(void* object, JavaClass* clazz);

/*
 * Array operations
 */

/* Get pointer to array data (inline after JavaArray structure) */
static inline void* array_data(JavaArray* array) {
    return array ? (void*)((uint8_t*)array + sizeof(JavaArray)) : NULL;
}

/* Get array length */
jsize array_length(JavaArray* array);

/* Get array element type */
uint8_t array_element_type(JavaArray* array);

/* Get array element */
JavaValue array_get(JavaArray* array, jsize index);

/* Set array element */
void array_set(JavaArray* array, jsize index, JavaValue value);

/* Get reference from object array */
void* array_get_ref(JavaArray* array, jsize index);

/* Set reference in object array */
void array_set_ref(JavaArray* array, jsize index, void* ref);

/*
 * String operations
 */

/* Get string length */
jsize string_length(JavaString* str);

/* Get string characters (UTF-16) */
const jchar* string_chars(JavaString* str);

/* Get string as UTF-8
 * IMPORTANT: Returns a pointer that caller MUST NOT free!
 * The returned pointer is either an internal cached buffer (for native strings)
 * or a thread-local buffer that is reused on the next call.
 * Use string_utf8_copy() if you need to store the result for later use.
 */
const char* string_utf8(JVM* jvm, JavaString* str);

/* Get string as UTF-8 copy - caller MUST free the returned pointer!
 * Always returns a malloc'd copy that the caller owns.
 * Use this when you need to store the string for later use.
 */
char* string_utf8_copy(JVM* jvm, JavaString* str);

/* Cleanup thread-local string buffer (call at JVM shutdown) */
void string_utf8_cleanup(void);

/* Compare strings */
bool string_equals(JavaString* a, JavaString* b);

/* String hash code */
jint string_hash(JavaString* str);

/*
 * Memory debugging
 */
void heap_dump(JVM* jvm);
void heap_validate(JVM* jvm);

/* Check if pointer is in heap - for validation 
 * Note: This uses external heap bounds from heap.c */
extern void* g_heap_start;
extern void* g_heap_end;

static inline bool is_heap_ptr_check(void* ptr) {
    extern void* g_heap_start;
    extern void* g_heap_end;
    return ptr >= g_heap_start && ptr < g_heap_end;
}

#endif /* HEAP_H */
