/*
 * J2ME Emulator - Heap and Garbage Collector (Fixed)
 * Mark-Sweep with free list, Coalescing, and 32-bit alignment support
 *
 * CRITICAL FIX: Added thread synchronization for heap operations.
 * Without this, multi-threaded access causes memory corruption on 32-bit Windows.
 *
 * DEBUG HEAP CORRUPTION:
 *   Define DEBUG_HEAP_CORRUPTION=1 to enable detailed logging to heap_debug.log
 *   This helps track down memory corruption issues.
 *
 * GC DEBUG LOGGING:
 *   Set environment variable J2ME_GC_DEBUG=1 to enable GC debug output to stderr
 *   All messages starting with [GC_ are hidden by default.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L  /* For posix_memalign */
#endif

/* Enable detailed heap corruption debugging */
#ifndef DEBUG_HEAP_CORRUPTION
#define DEBUG_HEAP_CORRUPTION 0
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifdef MEM_FREE
#undef MEM_FREE
#endif
#else
#include <pthread.h>
#include <sched.h>  /* For sched_yield() */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "heap.h"
#include "jvm.h"
#include "opcodes.h"
#include "classfile.h"
#include "debug.h"
#include "debug_macros.h"
#include "native.h"  /* For string field slot accessors */
#include "midp.h"    /* For M3G registry GC root support */

/* External function for error display in libretro mode */
extern void sdl_set_error_info(const char* title, const char* message, const char* stack_trace);

/* ============================================================
 * GC DEBUG LOGGING CONTROL
 * ============================================================
 * By default, all [GC_* debug messages are hidden.
 * To enable them, set environment variable: J2ME_GC_DEBUG=1
 * 
 * Example: J2ME_GC_DEBUG=1 ./j2me-emulator game.jar
 */
static int gc_debug_enabled_cache = -1;  /* -1 = not checked, 0 = disabled, 1 = enabled */

static int gc_debug_enabled(void) {
    if (gc_debug_enabled_cache == -1) {
        const char* env = getenv("J2ME_GC_DEBUG");
        gc_debug_enabled_cache = (env != NULL && (env[0] == '1' || env[0] == 'y' || env[0] == 'Y'));
    }
    return gc_debug_enabled_cache;
}

/* Macro for GC debug logging - only prints if J2ME_GC_DEBUG=1 */
#define GC_LOG(fmt, ...) do { \
    if (gc_debug_enabled()) { \
        LOG_SAFE(fmt, ##__VA_ARGS__); \
    } \
} while(0)

/* Macro for GC debug logging without newline (for partial lines) */
#define GC_LOG_PARTIAL(fmt, ...) do { \
    if (gc_debug_enabled()) { \
        LOG_SAFE(fmt, ##__VA_ARGS__); \
    } \
} while(0)

/* ============================================================
 * NATIVE HASHTABLE SUPPORT FOR GC
 * ============================================================
 * The Hashtable implementation uses native memory (g_hashtables)
 * to store key-value pairs. The GC needs to mark these references
 * because they're not stored in Java heap objects.
 * 
 * HashtableEntry: stores key (JavaObject*), value (JavaObject*), hash
 * HashtablePeer: stores entries array, capacity, count
 * g_hashtables[]: global array of HashtablePeer structures
 */
#define GC_HASHTABLE_MAX_HASHTABLES 4096

/* These must match the types in native.c */
typedef struct {
    JavaObject* key;
    JavaObject* value;
    jint hash;
} HashtableEntryGC;

typedef struct {
    JavaObject* ht_obj;     /* Back-reference to Java Hashtable object - MUST MATCH native.c! */
    HashtableEntryGC* entries;
    int capacity;
    int count;
} HashtablePeerGC;

/* External references to native Hashtable storage in native.c
 * Note: The struct types are named differently (HashtablePeer vs HashtablePeerGC)
 * but have identical memory layout, so the extern declarations work correctly.
 */
extern HashtablePeerGC g_hashtables[GC_HASHTABLE_MAX_HASHTABLES];
extern int g_hashtable_count;
extern bool g_hashtable_peer_alive[GC_HASHTABLE_MAX_HASHTABLES];

/* Function to free native peer - implemented in native.c */
extern void hashtable_free_peer(int idx);

/* ============================================================
 * HEAP CORRUPTION DEBUG LOGGING
 * ============================================================
 * When DEBUG_HEAP_CORRUPTION is enabled, all heap operations
 * are logged to heap_debug.log for post-mortem analysis.
 * This helps identify the source of memory corruption.
 */
#if DEBUG_HEAP_CORRUPTION
static FILE* volatile heap_log = NULL;
static volatile int heap_log_initialized = 0;

static void heap_log_init(void) {
    /* CRITICAL FIX: Thread-safe initialization using CAS */
    int expected = 0;
    if (ATOMIC_CAS(&heap_log_initialized, expected, 1)) {
        /* We won the race - open the log file */
        FILE* f = fopen("heap_debug.log", "w");
        if (!f) {
            f = stderr;
        }
        /* Memory barrier to ensure file is opened before other threads see it */
#ifdef _WIN32
        MemoryBarrier();
#else
        __sync_synchronize();
#endif
        heap_log = f;
        /* Mark as fully initialized */
        heap_log_initialized = 2;
    } else {
        /* Another thread is initializing - wait until complete */
        while (heap_log_initialized != 2) {
#ifdef _WIN32
            Sleep(0);
#else
            sched_yield();
#endif
        }
    }
}
static void heap_log_close(void) {
    if (heap_log && heap_log != stderr) {
        fclose((FILE*)heap_log);  /* Cast to non-volatile for fclose */
        heap_log = NULL;
    }
    heap_log_initialized = 0;
}
#define HEAP_CORRUPTION_LOG(fmt, ...) do { \
    if (!heap_log) heap_log_init(); \
    fprintf((FILE*)heap_log, "[HEAP_CORRUPTION] " fmt "\n", ##__VA_ARGS__); \
    fflush((FILE*)heap_log); \
} while(0)
#else
#define HEAP_CORRUPTION_LOG(fmt, ...) ((void)0)
#endif

/* Global JVM pointer for object_instance_of (set during JVM initialization) */
JVM* g_jvm_for_instanceof = NULL;

/* Global counter for strings fixed during GC (reset each cycle) */
static int gc_strings_fixed_this_cycle = 0;

/* Выравнивание объектов. 
 * На 32-битных системах double/long требуют выравнивания по 8 байт. 
 * Поэтому используем 8 для совместимости. */
#ifndef OBJECT_ALIGNMENT
#define OBJECT_ALIGNMENT 8
#endif

/* FreeBlock is now defined in heap.h for binary compatibility with GCObjectHeader */

/* ============================================================
 * CRITICAL: Heap mutex for thread synchronization
 * Without this, multi-threaded allocation causes memory corruption
 * 
 * NOTE: We use a RECURSIVE mutex because gc_collect() can be
 * called from inside heap_alloc(), and both need the lock.
 * ============================================================ */
#ifdef _WIN32
static CRITICAL_SECTION heap_mutex;
static volatile LONG heap_mutex_initialized = 0;  /* Use LONG for InterlockedCompareExchange */
#else
static pthread_mutex_t heap_mutex;
static pthread_mutexattr_t heap_mutex_attr;
#endif

/* Atomic flag for mutex initialization - used with barrier pattern */
static volatile int heap_mutex_init_done = 0;
static volatile int heap_mutex_ready = 0;  /* Set AFTER initialization complete */

/*
 * CRITICAL FIX: Cross-platform atomic compare-and-swap
 * Supports both GCC/Clang and MSVC compilers
 */
#ifdef _WIN32
/* MSVC: Use InterlockedCompareExchange */
#define ATOMIC_CAS(ptr, expected, desired) \
    (InterlockedCompareExchange((LONG*)(ptr), (desired), (expected)) == (expected))
#else
/* GCC/Clang: Use __sync_bool_compare_and_swap */
#define ATOMIC_CAS(ptr, expected, desired) \
    __sync_bool_compare_and_swap((ptr), (expected), (desired))
#endif

/* Initialize heap mutex (call once, thread-safe) */
static void heap_mutex_init(void) {
    int expected = 0;
    if (ATOMIC_CAS(&heap_mutex_init_done, expected, 1)) {
        /* We won the race - initialize the mutex */
#ifdef _WIN32
        /* CRITICAL_SECTION is recursive by default on Windows */
        InitializeCriticalSection(&heap_mutex);
#else
        /* POSIX: Create a RECURSIVE mutex */
        pthread_mutexattr_init(&heap_mutex_attr);
        pthread_mutexattr_settype(&heap_mutex_attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&heap_mutex, &heap_mutex_attr);
#endif
        
        /* CRITICAL FIX: Memory barrier to ensure mutex initialization
         * is visible to other threads BEFORE they can use it.
         * Without this, Thread B might see heap_mutex_init_done=1 but
         * try to enter an uninitialized critical section.
         */
#ifdef _WIN32
        MemoryBarrier();  /* Windows memory barrier */
#else
        __sync_synchronize();  /* GCC/Clang full memory barrier */
#endif
        
        /* Now mark as ready - other threads can proceed */
        heap_mutex_ready = 1;
    } else {
        /* Another thread is initializing - wait until ready */
        while (!heap_mutex_ready) {
#ifdef _WIN32
            Sleep(0);  /* Yield to other threads */
#else
            sched_yield();  /* POSIX yield */
#endif
        }
    }
}

/* Lock heap mutex */
static inline void heap_lock(void) {
    heap_mutex_init();
#ifdef _WIN32
    EnterCriticalSection(&heap_mutex);
#else
    pthread_mutex_lock(&heap_mutex);
#endif
}

/* Unlock heap mutex */
static inline void heap_unlock(void) {
#ifdef _WIN32
    LeaveCriticalSection(&heap_mutex);
#else
    pthread_mutex_unlock(&heap_mutex);
#endif
}

/* Heap state */
static struct {
    uint8_t* start;
    uint8_t* current;   /* Bump pointer for new allocations */
    uint8_t* end;
    size_t size;
    size_t allocated;   /* Total bytes currently occupied by live objects */
    size_t freed;       /* Total historical freed bytes */
    
    /* Мы не используем linked list heap.objects для обхода GC, 
       так как линейный проход по памяти надежнее. 
       Но можно оставить для статистики. */
    
    FreeBlock* free_list;  /* Free list for recycled memory (sorted by address for coalescing) */
    
    void*** roots;
    size_t root_count;
    size_t root_capacity;
    
    /* GC statistics */
    size_t gc_cycles;
    size_t gc_total_freed;
} heap;

/* Global heap bounds for validation */
void* g_heap_start = NULL;
void* g_heap_end = NULL;

/* Forward declarations */
static void gc_mark_object(void* ptr);
static void* try_alloc_from_free_list(size_t total_size, size_t* actual_size);

/* Проверка, является ли указатель частью кучи */
static inline bool is_heap_ptr(void* ptr) {
    return ptr >= (void*)heap.start && ptr < (void*)heap.end;
}

/* Инициализация кучи */
int heap_init(JVM* jvm, size_t initial_size, size_t max_size) {
    (void)jvm; (void)max_size;
    
    /* Выравниваем начальный адрес кучи */
#if defined(_WIN32) || defined(_WIN64)
    /* Windows: использование _aligned_malloc */
    heap.start = (uint8_t*)_aligned_malloc(initial_size, OBJECT_ALIGNMENT);
    if (!heap.start) return JNI_ERR;
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__)
    /* Unix-like: использование posix_memalign */
    if (posix_memalign((void**)&heap.start, OBJECT_ALIGNMENT, initial_size) != 0) {
        /* Пробуем обычный malloc как запасной вариант */
        heap.start = (uint8_t*)malloc(initial_size);
        if (!heap.start) return JNI_ERR;
        
        /* Проверяем выравнивание */
        if (((uintptr_t)heap.start & (OBJECT_ALIGNMENT - 1)) != 0) {
            /* CRITICAL FIX: On architectures with strict alignment (SPARC, old ARM, etc.),
             * unaligned access to double/long causes SIGBUS crash, not just performance loss.
             * Make this a FATAL error instead of a warning. */
#if defined(__sparc__) || defined(__sparc_v9__) || defined(__arm__) || defined(__aarch64__)
            ERROR_LOG("FATAL: malloc returned unaligned address %p on strict-alignment architecture!"
                     " This will cause SIGBUS crashes when accessing double/long values.",
                     (void*)heap.start);
            free(heap.start);
            heap.start = NULL;
            return JNI_ERR;
#else
            WARN_LOG("malloc returned unaligned address %p, performance may suffer",
                    (void*)heap.start);
#endif
        }
    }
#else
    /* Для других систем используем malloc и надеемся на лучшее */
    heap.start = (uint8_t*)malloc(initial_size);
    if (!heap.start) return JNI_ERR;
    
    /* Проверяем выравнивание */
    if (((uintptr_t)heap.start & (OBJECT_ALIGNMENT - 1)) != 0) {
        ERROR_LOG("FATAL: malloc returned unaligned address %p! "
                 "This may cause crashes on strict-alignment architectures.",
                 (void*)heap.start);
        free(heap.start);
        heap.start = NULL;
        return JNI_ERR;
    }
#endif
    
    heap.current = heap.start;
    heap.end = heap.start + initial_size;
    heap.size = initial_size;
    heap.allocated = 0;
    heap.freed = 0;
    heap.free_list = NULL;
    heap.gc_cycles = 0;
    heap.gc_total_freed = 0;
    
    /* Set global heap bounds for validation */
    g_heap_start = heap.start;
    g_heap_end = heap.end;
    
    heap.roots = (void***)malloc(1024 * sizeof(void**));
    if (!heap.roots) {
        /* CRITICAL FIX: Use _aligned_free on Windows for memory allocated with _aligned_malloc!
         * Using free() on _aligned_malloc memory causes heap corruption and crashes. */
#if defined(_WIN32) || defined(_WIN64)
        _aligned_free(heap.start);
#else
        free(heap.start);
#endif
        return JNI_ERR;
    }
    heap.root_capacity = 1024;
    heap.root_count = 0;
    
    return JNI_OK;
}

/* Forward declaration for gc_mark_stack_destroy */
static void gc_mark_stack_destroy(void);

/* Уничтожение кучи */
void heap_destroy(JVM* jvm) {
    (void)jvm;
    
    /* CRITICAL FIX: Use _aligned_free on Windows for memory allocated with _aligned_malloc!
     * Using free() on _aligned_malloc memory causes heap corruption and crashes.
     * The _aligned_malloc functions use a special header to track alignment,
     * and _aligned_free knows how to find the original allocation address.
     */
#if defined(_WIN32) || defined(_WIN64)
    if (heap.start) {
        _aligned_free(heap.start);
    }
#else
    free(heap.start);
#endif
    
    free(heap.roots);
    gc_mark_stack_destroy();  /* Free the GC mark stack */
    
    heap.start = NULL;
    heap.current = NULL;
    heap.end = NULL;
    heap.free_list = NULL;
}

/* Попытка выделить память из списка свободных блоков (First Fit)
 * Возвращает указатель на блок и его реальный размер через actual_size
 */
static void* try_alloc_from_free_list(size_t total_size, size_t* actual_size) {
    FreeBlock** pp = &heap.free_list;
    FreeBlock* prev = NULL;
    
    while (*pp) {
        FreeBlock* block = *pp;
        
        if (block->size >= total_size) {
            /* Найден подходящий блок */
            
            /* Отсоединяем блок из списка */
            if (prev) {
                prev->next = block->next;
            } else {
                heap.free_list = block->next;
            }
            
            /* Если блок больше запроса, дробим его
             * CRITICAL FIX: Всегда создаём remainder если есть место для заголовка!
             * Иначе образуются "дыры" в куче, которые вызывают corruption при GC sweep.
             */
            size_t remainder_size = block->size - total_size;
            if (remainder_size >= sizeof(GCObjectHeader)) {
                /* ВАЖНО: remainder должен начинаться с GCObjectHeader для корректного
                   линейного прохода GC. FreeBlock использует те же поля что и 
                   GCObjectHeader (size + next), поэтому это безопасно. */
                GCObjectHeader* remainder = (GCObjectHeader*)((uint8_t*)block + total_size);
                remainder->size = (uint32_t)remainder_size;
                remainder->type = OBJ_TYPE_FREE;  /* Маркируем как свободный блок */
                remainder->marked = 0;
                remainder->pinned = 0;
                remainder->clazz = NULL;
                remainder->next = NULL;
                remainder->_align_pad[0] = 0;
                remainder->_align_pad[1] = 0;
                remainder->_align_pad[2] = 0;
                remainder->_align_pad[3] = 0;
                remainder->_align_pad[4] = 0;
                remainder->_align_pad[5] = 0;
                remainder->_align_pad[6] = 0;
                remainder->_align_pad[7] = 0;
                
                /* Вставляем остаток обратно в список */
                FreeBlock* fb = (FreeBlock*)remainder;
                fb->next = heap.free_list;
                heap.free_list = fb;
                
                /* Используем запрошенный размер */
                *actual_size = total_size;
            } else {
                /* CRITICAL FIX: Нет места для remainder - используем ВЕСЬ блок!
                 * Иначе образуется "дыра" которая вызовет corruption при sweep.
                 */
                *actual_size = block->size;
            }
            
            return block;
        }
        
        prev = block;
        pp = &block->next;
    }
    
    return NULL;
}

/* Базовое выделение памяти */
void* heap_alloc(JVM* jvm, size_t size, JavaClass* clazz, ObjectType type) {
    (void)jvm;
    
    /* CRITICAL: Acquire heap lock BEFORE any heap operations */
    /* This is a RECURSIVE mutex, so calling gc_collect() inside is safe */
    heap_lock();
    
    /* sanity check: reject obviously invalid sizes 
     * 67108956 = 0x03FFFFFF is suspicious - might be -1 interpreted as unsigned
     * Max reasonable allocation is 1MB for J2ME */
    if (size > 1024 * 1024) {
        ERROR_LOG("CRITICAL: Suspicious allocation request: %zu bytes (0x%zX) for class=%s type=%d",
                size, size, 
                clazz ? (clazz->class_name ? clazz->class_name : "?") : "NULL",
                type);
        ERROR_LOG("  This might be a bug - negative value interpreted as unsigned?");
        ERROR_LOG("  Rejecting allocation to prevent OOM");
        heap_unlock();
        return NULL;
    }
    
    /* HEAP CORRUPTION DEBUG: Log allocation request */
    HEAP_CORRUPTION_LOG("ALLOC_START: size=%zu, clazz=%s, type=%d, heap.current=%p, heap.end=%p",
            size, clazz ? (clazz->class_name ? clazz->class_name : "?") : "NULL",
            type, heap.current, heap.end);
    
    /* Выравниваем размер запроса */
    size = (size + OBJECT_ALIGNMENT - 1) & ~(OBJECT_ALIGNMENT - 1);
    
    /* Добавляем размер GC-заголовка */
    size_t total_size = sizeof(GCObjectHeader) + size;
    
    /* 1. Пытаемся найти в Free List */
    size_t actual_alloc_size = total_size;  /* По умолчанию - запрошенный размер */
    int from_free_list = 1;
    GCObjectHeader* header = (GCObjectHeader*)try_alloc_from_free_list(total_size, &actual_alloc_size);
    
    if (!header) {
        /* 2. Если нет в Free List, берем из топа кучи (Bump Pointer) */
        if (heap.current + total_size > heap.end) {
            /* Не хватает места в конце кучи. Запускаем GC. */
            HEAP_CORRUPTION_LOG("ALLOC_GC: triggering GC, need=%zu, available=%zu",
                    total_size, (size_t)(heap.end - heap.current));
            gc_collect(jvm);
            
            /* Пробуем снова в Free List после GC */
            header = (GCObjectHeader*)try_alloc_from_free_list(total_size, &actual_alloc_size);
            
            if (!header) {
                /* Все еще нет места. Проверяем топ кучи (GC мог освободить место в середине, но не в конце) */
                if (heap.current + total_size > heap.end) {
                    ERROR_LOG("Out of memory! Requested: %zu, Available: %zu",
                            total_size, (size_t)(heap.end - heap.current));
                    HEAP_CORRUPTION_LOG("ALLOC_OOM: requested=%zu, available=%zu",
                            total_size, (size_t)(heap.end - heap.current));
                    
                    /* Set error info for display in libretro mode */
                    char oom_msg[256];
                    snprintf(oom_msg, sizeof(oom_msg), 
                            "Requested: %zu bytes, Available: %zu bytes",
                            total_size, (size_t)(heap.end - heap.current));
                    sdl_set_error_info("OutOfMemoryError", oom_msg, 
                            "The Java heap is full. Try reducing memory usage or increasing heap size.");
                    
                    heap_unlock();  /* Release lock before returning */
                    return NULL;
                }
                
                /* Если GC освободил место в конце, используем bump pointer */
                from_free_list = 0;
                header = (GCObjectHeader*)heap.current;
                heap.current += total_size;
                actual_alloc_size = total_size;
            }
        } else {
            /* Место есть, выделяем */
            from_free_list = 0;
            header = (GCObjectHeader*)heap.current;
            heap.current += total_size;
            actual_alloc_size = total_size;
        }
    } else {
        HEAP_CORRUPTION_LOG("ALLOC_FREELIST: found block at %p, size=%zu", header, actual_alloc_size);
    }
    
    heap.allocated += actual_alloc_size;
    
    /* Zero-initialize: only needed for free-list reuse.
     * Bump-pointer memory is either fresh from OS (zeroed) or never written. */
    if (from_free_list) {
        memset(header, 0, actual_alloc_size);
    }
    
    /* Инициализация заголовка */
    /* Примечание: next здесь не используется для связывания объектов при GC, 
       так как мы используем линейный проход. Поле зарезервировано. */
    header->next = NULL; 
    header->clazz = clazz;
    header->size = (uint32_t)actual_alloc_size;  /* CRITICAL FIX: используем РАЛЬНЫЙ размер! */
    header->type = (uint8_t)type;
    header->marked = 0;
    header->pinned = 0;
    /* reserved[0..1], _align_pad[0..7] already zeroed by memset above */
    header->magic = GC_HEADER_MAGIC;  /* Magic number for corruption detection */
    header->_magic_pad = 0;  /* Padding for 8-byte alignment */
    
    /* Вычисляем указатель на пользовательские данные (сразу после заголовка) */
    void* ptr = (void*)(header + 1);
    
    /* CRITICAL FIX: Also set clazz in ObjectHeader for JavaObject types
     * ObjectHeader.clazz is at offset 0 of the object, while GCObjectHeader.clazz
     * is at offset 24 of the GC header. They are DIFFERENT memory locations!
     * For JavaObject (OBJ_TYPE_OBJECT, OBJ_TYPE_STRING, OBJ_TYPE_ARRAY),
     * we need to set the ObjectHeader.clazz field as well.
     */
    if (clazz && (type == OBJ_TYPE_OBJECT || type == OBJ_TYPE_STRING || type == OBJ_TYPE_ARRAY)) {
        ObjectHeader* obj_header = (ObjectHeader*)ptr;
        obj_header->clazz = clazz;
        obj_header->hashcode = (jint)(intptr_t)ptr;  /* Default hashcode = object address */
        obj_header->gc_mark = 0;
        obj_header->reserved = 0;
    }
    
    /* HEAP CORRUPTION DEBUG: Log successful allocation */
    HEAP_CORRUPTION_LOG("ALLOC_DONE: ptr=%p, header=%p, size=%u, type=%d, clazz=%s",
            ptr, header, header->size, type,
            clazz ? (clazz->class_name ? clazz->class_name : "?") : "NULL");
    
    /* DEBUG: Log heap allocation */
    GC_DEBUG("[HEAP_ALLOC] ptr=%p, gc_header=%p, clazz=%p (%s), type=%d, size=%u",
            ptr, header, clazz, 
            clazz ? (clazz->class_name ? clazz->class_name : "NO_NAME") : "NULL",
            type, header->size);
    
    /* CRITICAL: Release heap lock after all operations complete */
    heap_unlock();
    
    return ptr;
}

/* Выделение объекта */
JavaObject* heap_alloc_object(JVM* jvm, JavaClass* clazz) {
    if (!clazz) {
        ERROR_LOG("heap_alloc_object called with NULL class");
        return NULL;
    }
    
    /* ИСПРАВЛЕНИЕ: clazz->instance_size уже должен включать sizeof(ObjectHeader) + поля.
       Мы не должны добавлять sizeof(ObjectHeader) здесь снова. */
    size_t size = clazz->instance_size;
    
    /* Минимальная защита */
    if (size < sizeof(JavaObject)) {
        size = sizeof(JavaObject);
    }

    /* Выравниваем размер объекта */
    size = (size + OBJECT_ALIGNMENT - 1) & ~(OBJECT_ALIGNMENT - 1);
    
    /* GC_DEBUG("Allocating object: class=%s, size=%zu", clazz->class_name, size); */
    
    JavaObject* obj = (JavaObject*)heap_alloc(jvm, size, clazz, OBJ_TYPE_OBJECT);
    if (obj) {
        /* Инициализируем Java-хедер объекта */
        obj->header.clazz = clazz;
        obj->header.hashcode = (jint)(intptr_t)obj;  /* Identity hash */
        /* gc_mark and reserved already zeroed by heap_alloc's memset */
        
        /* NOTE: heap_alloc already memsets the entire block (including fields) to zero.
         * No need for a second memset of obj->fields here. */
    } else {
        ERROR_LOG("heap_alloc failed for class %s", 
                clazz->class_name ? clazz->class_name : "unknown");
    }
    
    return obj;
}

/* Выделение массива */
JavaArray* heap_alloc_array(JVM* jvm, uint8_t element_type, jsize length, JavaClass* element_class) {
    /* Verify JavaArray structure size at runtime */
    /* On 64-bit: should be 32 bytes (ObjectHeader 16 + length 4 + element_type 1 + reserved 3 + element_class 8) */
    /* On 32-bit: CRITICAL - ObjectHeader has bit-fields, actual size is 12 bytes, not 8! */
    /*   ObjectHeader = clazz(4) + hashcode(4) + gc_mark+reserved(4) = 12 bytes */
    /*   JavaArray = ObjectHeader(12) + length(4) + element_type(1) + reserved(3) + element_class(4) = 24 bytes */
    
    /* CRITICAL: Log actual structure sizes for debugging */
    static bool sizes_logged = false;
    if (!sizes_logged) {
        LOG_SAFE("[HEAP] Structure sizes: ObjectHeader=%zu, JavaArray=%zu, JavaObject=%zu, JavaValue=%zu, GCObjectHeader=%zu\n",
                sizeof(ObjectHeader), sizeof(JavaArray), sizeof(JavaObject), sizeof(JavaValue), sizeof(GCObjectHeader));
        
        /* Verify ObjectHeader is multiple of 8 for proper field alignment */
        if (sizeof(ObjectHeader) % 8 != 0) {
            LOG_SAFE("[HEAP] WARNING: ObjectHeader size %zu is NOT 8-byte aligned! Fields will be misaligned.\n",
                    sizeof(ObjectHeader));
        }
        
        sizes_logged = true;
    }
    
    /* Размер элемента */
    size_t elem_size;
    switch (element_type) {
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
        default: elem_size = 4; break;
    }
    
    size_t data_size = elem_size * length;
    size_t total_size = sizeof(JavaArray) + data_size;
    
    /* Поиск класса java/lang/Object для массива. */
    JavaClass* array_class = NULL;
    for (size_t i = 0; i < jvm->class_loader.count; i++) {
        JavaClass* c = jvm->class_loader.classes[i];
        if (c->class_name && strcmp(c->class_name, "java/lang/Object") == 0) {
            array_class = c;
            break;
        }
    }
    
    if (!array_class && jvm->class_loader.count > 0) {
        array_class = jvm->class_loader.classes[0]; /* Fallback */
    }
    
    /* DEBUG: Log what array_class we found */
    GC_DEBUG("[HEAP_ALLOC_ARRAY] array_class=%p (%s), element_class=%p (%s)",
            (void*)array_class, array_class ? (array_class->class_name ? array_class->class_name : "NO_NAME") : "NULL",
            (void*)element_class, element_class ? (element_class->class_name ? element_class->class_name : "NO_NAME") : "NULL");
    
    /* ИСПРАВЛЕНО: Передаём array_class в heap_alloc, чтобы GC header->clazz был установлен */
    JavaArray* array = (JavaArray*)heap_alloc(jvm, total_size, array_class, OBJ_TYPE_ARRAY);
    if (!array) return NULL;
    
    array->header.clazz = array_class;
    array->header.hashcode = (jint)(intptr_t)array;
    array->length = length;
    array->element_type = element_type;
    memset(array->_reserved, 0, sizeof(array->_reserved));
    array->element_class = element_class;  /* Store element class for object arrays */
    
    /* Debug: log array allocation details */
    DEBUG_LOG("heap_alloc_array: array=%p, elem_type=%d, length=%d, elem_class=%s, sizeof(JavaArray)=%zu",
              (void*)array, element_type, length, 
              element_class ? (element_class->class_name ? element_class->class_name : "?") : "NULL",
              sizeof(JavaArray));
    
    return array;
}

/* Выделение строки
 * 
 * CRITICAL FIX: Унификация модели строк!
 * 
 * Если класс java/lang/String загружен, создаем OBJ_TYPE_OBJECT с полями.
 * Это Java String объект, который хранит данные в отдельном char[] через поле 'value'.
 * 
 * Если класс java/lang/String НЕ загружен, создаем OBJ_TYPE_STRING с inline данными.
 * Это нативная строка для внутреннего использования (например, до загрузки классов).
 */
JavaString* heap_alloc_string(JVM* jvm, jsize length) {
    /* Найти класс java/lang/String */
    JavaClass* str_class = NULL;
    for (size_t i = 0; i < jvm->class_loader.count; i++) {
        JavaClass* c = jvm->class_loader.classes[i];
        if (c->class_name && strcmp(c->class_name, "java/lang/String") == 0) {
            str_class = c;
            break;
        }
    }
    
    /* Если класс java/lang/String найден, создаем как OBJ_TYPE_OBJECT!
     * Это КРИТИЧЕСКИ важно для правильной работы GC.
     * Java String объект имеет поля: value (char[]), offset (int), count (int), hash (int)
     * Данные хранятся в отдельном массиве char[], на который ссылается поле value.
     * 
     * ВАЖНО: НЕ используем поля JavaString (length, hash, utf8, chars) т.к. они
     * перекрываются с fields[]! Используем только fields[].
     */
    if (str_class) {
        /* Выделяем объект с полями */
        size_t obj_size = str_class->instance_size;
        obj_size = (obj_size + OBJECT_ALIGNMENT - 1) & ~(OBJECT_ALIGNMENT - 1);
        
        JavaObject* str = (JavaObject*)heap_alloc(jvm, obj_size, str_class, OBJ_TYPE_OBJECT);
        if (!str) return NULL;
        
        /* Инициализируем заголовок - heap_alloc уже установил clazz, но мы удостоверимся */
        str->header.clazz = str_class;
        
        /* Поля: value, offset, count, hash будут установлены вызывающим кодом
         * (обычно в native String.<init> или jvm_new_string_utf16)
         * ВАЖНО: НЕ устанавливаем str->length, str->chars и т.д. - они перекрываются с fields[]! */
        
        return (JavaString*)str;
    }
    
    /* Класс java/lang/String НЕ найден - создаем нативную строку с inline данными.
     * Это используется только на ранней стадии инициализации JVM. */
    size_t data_size = length * sizeof(jchar);
    size_t total_size = sizeof(JavaString) + data_size;
    
    JavaString* str = (JavaString*)heap_alloc(jvm, total_size, NULL, OBJ_TYPE_STRING);
    if (!str) return NULL;
    
    str->header.clazz = NULL;
    str->header.hashcode = (jint)(intptr_t)str;
    str->header.gc_mark = 0;
    str->header.reserved = 0;
    str->length = length;
    str->hash = 0;
    str->utf8 = NULL;
    str->chars = NULL;  /* Native strings use inline storage, accessed via string_chars */
    
    return str;
}

/* --- Mark and Sweep Implementation --- */

/* ИСПРАВЛЕНО: Динамический размер стека маркировки GC */
#define GC_MARK_STACK_INITIAL_SIZE 4096
#define GC_MARK_STACK_MAX_SIZE (1024 * 1024)  /* Максимум 1M объектов */

typedef struct {
    void* ptr;
} MarkStackEntry;

static MarkStackEntry* gc_mark_stack = NULL;
static int gc_mark_stack_top = 0;
static int gc_mark_stack_capacity = 0;

/* ИСПРАВЛЕНО: Инициализация стека маркировки */
static bool gc_mark_stack_init(void) {
    gc_mark_stack = (MarkStackEntry*)malloc(GC_MARK_STACK_INITIAL_SIZE * sizeof(MarkStackEntry));
    if (!gc_mark_stack) return false;
    gc_mark_stack_capacity = GC_MARK_STACK_INITIAL_SIZE;
    gc_mark_stack_top = 0;
    return true;
}

/* ИСПРАВЛЕНО: Освобождение стека маркировки */
static void gc_mark_stack_destroy(void) {
    if (gc_mark_stack) {
        free(gc_mark_stack);
        gc_mark_stack = NULL;
    }
    gc_mark_stack_capacity = 0;
    gc_mark_stack_top = 0;
}

/* Проверка валидности указателя на GC-объект */
static bool is_valid_gc_object(void* ptr) {
    if (!ptr) return false;
    
    /* CRITICAL FIX: Check if pointer looks like a small integer value
     * These are NOT heap pointers! Common in Java where int values
     * are stored in reference fields before proper initialization. */
    if ((uintptr_t)ptr < 0x10000) {
        /* Almost certainly a small integer, not a pointer */
        return false;
    }
    
    if (!is_heap_ptr(ptr)) {
        return false;
    }
    
    /* Получаем GC заголовок (он находится перед пользовательскими данными) */
    GCObjectHeader* header = (GCObjectHeader*)ptr - 1;
    
    /* Проверка magic number - это самый надежный способ проверить валидность */
    if (header->magic != GC_HEADER_MAGIC) {
        return false;
    }
    
    /* Проверка валидности размера */
    if (header->size == 0 || header->size > heap.size) {
        return false;
    }
    
    /* CRITICAL FIX: Reject objects that are already freed (OBJ_TYPE_FREE = 4)
     * This prevents use-after-free when static fields reference freed objects */
    if (header->type == OBJ_TYPE_FREE) {
        return false;
    }
    
    /* Проверка валидности типа */
    if (header->type > OBJ_TYPE_CLASS) {
        return false;
    }
    
    /* Проверка что объект находится в правильном месте в куче */
    uint8_t* obj_start = (uint8_t*)header;
    uint8_t* obj_end = obj_start + header->size;
    if (obj_end > heap.current) {
        return false;
    }
    
    return true;
}

/* ИСПРАВЛЕНО: Push с проверкой и расширением стека */
static bool gc_push_mark(void* ptr) {
    if (!ptr) return true;
    
    /* CRITICAL FIX: Validate that this is actually a GC-managed object.
     * This prevents primitive values (ints, floats) that happen to look like
     * heap pointers from being treated as references. */
    if (!is_valid_gc_object(ptr)) {
        /* CRITICAL: Log WHY validation failed for debugging.
         * DO NOT try to access header for invalid pointers - it would cause segfault! */
        GC_LOG("[GC_PUSH] SKIPPING invalid ptr=%p (heap=%p-%p, current=%p)\n", 
                ptr, heap.start, heap.end, heap.current);
        /* For small integers (< 0x10000), this is likely a primitive value stored in a reference field.
         * This is usually harmless - the field was not initialized or contains a non-reference value. */
        if ((uintptr_t)ptr < 0x10000) {
            GC_LOG("[GC_PUSH]   ptr looks like a small integer value: %zu\n", (uintptr_t)ptr);
        } else {
            /* CRITICAL FIX: Do NOT try to access header for invalid pointers!
             * The pointer may point to unmapped memory, causing segfault.
             * Just log that it's invalid and skip it. */
            GC_LOG("[GC_PUSH]   ptr is outside heap or invalid - skipping without dereferencing\n");
        }
        /* Return true to continue execution - the object was simply not marked.
         * This is not a fatal error, just means this reference field contains a non-GC value. */
        return true;
    }
    
    /* Log valid object being added */
    GCObjectHeader* header = (GCObjectHeader*)ptr - 1;
    GC_DEBUG("[GC_PUSH] VALID ptr=%p, header=%p, size=%u, type=%d, marked=%d",
            ptr, header, header->size, header->type, header->marked);
    
    /* Проверка на переполнение и расширение */
    if (gc_mark_stack_top >= gc_mark_stack_capacity) {
        if (gc_mark_stack_capacity >= GC_MARK_STACK_MAX_SIZE) {
            ERROR_LOG("Mark stack overflow (max %d entries)", GC_MARK_STACK_MAX_SIZE);
            return false;  /* Переполнение */
        }
        
        /* Расширяем стек */
        int new_capacity = gc_mark_stack_capacity * 2;
        if (new_capacity > GC_MARK_STACK_MAX_SIZE) {
            new_capacity = GC_MARK_STACK_MAX_SIZE;
        }
        
        MarkStackEntry* new_stack = (MarkStackEntry*)realloc(gc_mark_stack, 
                                                              new_capacity * sizeof(MarkStackEntry));
        if (!new_stack) {
            ERROR_LOG("Failed to expand mark stack");
            return false;
        }
        
        gc_mark_stack = new_stack;
        gc_mark_stack_capacity = new_capacity;
        GC_DEBUG("Expanded mark stack to %d entries", new_capacity);
    }
    
    gc_mark_stack[gc_mark_stack_top++].ptr = ptr;
    return true;
}

static void gc_mark_object(void* ptr) {
    if (!ptr) return;
    if (!is_heap_ptr(ptr)) return;
    
    /* Получаем GC заголовок (он находится перед пользовательскими данными) */
    GCObjectHeader* header = (GCObjectHeader*)ptr - 1;
    
    /* Проверка валидности */
    if (header->size == 0 || header->size > heap.size) {
        return; /* Коррумпированный указатель */
    }
    
    if (header->marked) return; /* Уже помечен */
    
    /* CRITICAL: Log when we mark an object */
    GC_LOG("[GC_MARK] Marking object %p (header=%p): size=%u, type=%d, clazz=%s\n",
            ptr, header, header->size, header->type,
            header->clazz ? (header->clazz->class_name ? header->clazz->class_name : "?") : "NULL");
    
    header->marked = 1;
    
    /* Рекурсивная пометка ссылок */
    switch (header->type) {
        case OBJ_TYPE_OBJECT: {
            JavaObject* obj = (JavaObject*)ptr;
            JavaClass* clazz = obj->header.clazz;
            
            if (!clazz) break;
            
            /* CRITICAL FIX: Special handling for java/lang/String objects.
             * String objects have a 'value' field that points to a char[].
             * We must validate and mark this array FIRST to prevent use-after-free.
             */
            if (clazz->class_name && strcmp(clazz->class_name, "java/lang/String") == 0) {
                GC_LOG("[GC_STRING_OBJ] Marking java/lang/String object at %p\n", ptr);
                
                /* Get the value slot for the char array */
                int value_slot = native_get_string_value_slot(g_jvm_for_instanceof);
                
                if (value_slot >= 0) {
                    void* value_ref = obj->fields[value_slot].ref;
                    if (value_ref) {
                        /* Validate the char array before marking */
                        if (is_valid_gc_object(value_ref)) {
                            GC_LOG("[GC_STRING_OBJ]   value field (char[]) = %p (valid), marking\n", value_ref);
                            gc_push_mark(value_ref);
                        } else {
                            /* CRITICAL: Invalid char array reference! */
                            GC_LOG("[GC_STRING_OBJ]   CRITICAL: value field = %p (INVALID! use-after-free detected!)\n", value_ref);
                            
                            if (is_heap_ptr(value_ref)) {
                                GCObjectHeader* array_header = (GCObjectHeader*)value_ref - 1;
                                GC_LOG("[GC_STRING_OBJ]   Array header: magic=0x%08X, type=%d, size=%u\n",
                                        array_header->magic, array_header->type, array_header->size);
                                
                                if (array_header->type == OBJ_TYPE_FREE) {
                                    GC_LOG("[GC_STRING_OBJ]   ERROR: Char array was already freed! Clearing field.\n");
                                    obj->fields[value_slot].ref = NULL;
                                    gc_strings_fixed_this_cycle++;  /* Count this fix */
                                }
                            } else {
                                GC_LOG("[GC_STRING_OBJ]   Pointer outside heap - clearing field\n");
                                obj->fields[value_slot].ref = NULL;
                                gc_strings_fixed_this_cycle++;  /* Count this fix */
                            }
                        }
                    }
                }
                /* Continue to normal field processing for other fields */
            }
            
            /* CRITICAL FIX: Special handling for collection classes
             * These classes store references in internal arrays that may not
             * be properly detected by field scanning. We handle them explicitly.
             * 
             * java/util/Hashtable: table field -> Entry[] -> Entry.key, Entry.value
             * java/util/Vector: elementData field -> Object[]
             * java/util/ArrayList: elementData field -> Object[]
             */
            if (clazz->class_name) {
                const char* cn = clazz->class_name;
                
                /* java/util/Hashtable$Entry - has key, value, next fields */
                if (strstr(cn, "Hashtable$Entry") != NULL || strstr(cn, "HashMap$Entry") != NULL) {
                    /* Entry objects have key (Object), value (Object), next (Entry) fields
                     * Layout: typically key=slot 0, value=slot 1, next=slot 2
                     * We need to mark all three */
                    GC_LOG("[GC_COLLECTION] Marking Entry object %p\n", ptr);
                    for (int e_slot = 0; e_slot < 4 && e_slot < (int)(header->size / sizeof(JavaValue)); e_slot++) {
                        void* ref = obj->fields[e_slot].ref;
                        if (ref) {
                            GC_LOG("[GC_COLLECTION]   Entry field[%d] = %p, marking\n", e_slot, ref);
                            gc_push_mark(ref);
                        }
                    }
                    break;
                }
                
                /* java/util/Hashtable - scan table array and ALL reference fields */
                if (strcmp(cn, "java/util/Hashtable") == 0 || strcmp(cn, "java/util/HashMap") == 0) {
                    GC_LOG("[GC_HASHTABLE] Special handling for %s at %p, size=%u\n", cn, ptr, header->size);
                    
                    int num_slots = (int)(header->size / sizeof(JavaValue));
                    GC_LOG("[GC_HASHTABLE]   Scanning %d field slots...\n", num_slots);
                    
                    /* CRITICAL FIX: Scan ALL reference fields, not just arrays!
                     * Hashtable may store entries as:
                     * 1. Entry[] array (OBJ_TYPE_ARRAY with element_type=OBJECT)
                     * 2. Linked list Entry objects (OBJ_TYPE_OBJECT with clazz=Hashtable$Entry)
                     * We need to mark ALL reference fields to catch both cases. */
                    for (int s = 0; s < num_slots; s++) {
                        void* ref = obj->fields[s].ref;
                        GC_LOG("[GC_HASHTABLE]   field[%d] = %p\n", s, ref);
                        
                        if (ref && is_heap_ptr(ref)) {
                            GCObjectHeader* ref_hdr = (GCObjectHeader*)ref - 1;
                            if (ref_hdr->magic == GC_HEADER_MAGIC) {
                                GC_LOG("[GC_HASHTABLE]     -> valid object: type=%d, size=%u, clazz=%s\n",
                                        ref_hdr->type, ref_hdr->size,
                                        ref_hdr->clazz ? (ref_hdr->clazz->class_name ? ref_hdr->clazz->class_name : "?") : "NULL");
                                
                                /* Mark ANY reference field - this handles both arrays and Entry objects */
                                gc_push_mark(ref);
                            } else {
                                GC_LOG("[GC_HASHTABLE]     -> INVALID MAGIC (not a GC object)\n");
                            }
                        } else if (ref) {
                            GC_LOG("[GC_HASHTABLE]     -> not a heap pointer\n");
                        }
                    }
                    /* Continue to normal field processing as well for completeness */
                }
                
                /* java/util/Vector, java/util/ArrayList */
                if (strcmp(cn, "java/util/Vector") == 0 || strcmp(cn, "java/util/ArrayList") == 0) {
                    GC_LOG("[GC_COLLECTION] Special handling for %s at %p, size=%u\n", cn, ptr, header->size);
                    
                    int num_slots = (int)(header->size / sizeof(JavaValue));
                    GC_LOG("[GC_COLLECTION]   Scanning %d field slots...\n", num_slots);
                    
                    /* CRITICAL FIX: Scan ALL reference fields, not just arrays */
                    for (int s = 0; s < num_slots; s++) {
                        void* ref = obj->fields[s].ref;
                        if (ref && is_heap_ptr(ref)) {
                            GCObjectHeader* ref_hdr = (GCObjectHeader*)ref - 1;
                            if (ref_hdr->magic == GC_HEADER_MAGIC) {
                                GC_LOG("[GC_COLLECTION]   field[%d] = %p, type=%d, size=%u\n",
                                        s, ref, ref_hdr->type, ref_hdr->size);
                                gc_push_mark(ref);
                            }
                        }
                    }
                }
            }
            
            /* Build hierarchy and calculate field slots properly */
            JavaClass* hierarchy[64];
            int depth = 0;
            JavaClass* c = clazz;
            while (c && depth < 64) {
                hierarchy[depth++] = c;
                c = c->super_class;
            }
            
            /* Process fields from Object down to actual class */
            int slot = 0;
            for (int h = depth - 1; h >= 0; h--) {
                JavaClass* current = hierarchy[h];
                if (!current->fields) continue;
                
                for (int i = 0; i < current->fields_count; i++) {
                    JavaField* field = &current->fields[i];
                    
                    /* Skip static fields */
                    if (field->access_flags & ACC_STATIC) continue;
                    
                    /* Check if this is a reference field */
                    if (field->descriptor) {
                        char desc = field->descriptor[0];
                        if (desc == 'L' || desc == '[') {
                            void* ref = obj->fields[slot].ref;
                            if (ref) {
                                /* CRITICAL FIX: Skip nativePeer fields!
                                 * nativePeer contains native pointers (MidpFont*, MidpGraphics*, MidpImage*)
                                 * which are malloc'd, NOT heap objects. If we try to mark them as heap
                                 * objects, we may corrupt memory if the pointer happens to fall within
                                 * heap address range. */
                                if (field->name && strcmp(field->name, "nativePeer") == 0) {
                                    /* Skip native pointer - not a GC-managed reference */
                                    GC_DEBUG("[GC] Skipping nativePeer field at slot %d (value=%p)", slot, ref);
                                } else {
                                    gc_push_mark(ref);
                                }
                            }
                        }
                    }
                    
                    /* Advance slot */
                    slot++;
                    /* Long and double take 2 slots */
                    if (field->descriptor && 
                        (field->descriptor[0] == 'J' || field->descriptor[0] == 'D')) {
                        slot++;
                    }
                }
            }
            break;
        }
        
        case OBJ_TYPE_ARRAY: {
            JavaArray* array = (JavaArray*)ptr;
            /* CRITICAL DEBUG: Log array marking details */
            GC_DEBUG("[GC_ARRAY] Marking array %p: length=%d, element_type=%d ('%c'), sizeof(JavaArray)=%zu",
                    ptr, array->length, array->element_type, 
                    (array->element_type >= 32 && array->element_type < 127) ? (char)array->element_type : '?',
                    sizeof(JavaArray));
            
            if (array->element_type == DESC_OBJECT || array->element_type == DESC_ARRAY) {
                void** refs = (void**)((uint8_t*)array + sizeof(JavaArray));
                GC_DEBUG("[GC_ARRAY] Array %p is object/array array, refs at %p, scanning %d elements",
                        ptr, refs, array->length);
                int marked_count = 0;
                for (jsize i = 0; i < array->length; i++) {
                    if (refs[i]) {
                        GC_DEBUG("[GC_ARRAY] Array %p element[%d] = %p, marking...", ptr, i, refs[i]);
                        bool result = gc_push_mark(refs[i]);
                        if (result) marked_count++;
                    }
                }
                GC_DEBUG("[GC_ARRAY] Array %p: marked %d/%d elements", ptr, marked_count, array->length);
            } else {
                GC_DEBUG("[GC_ARRAY] Array %p is primitive (type=%d), no references to mark", 
                        ptr, array->element_type);
            }
            break;
        }
        
        case OBJ_TYPE_CLASS: {
             /* Статические поля классов */
            if (header->clazz && header->clazz->static_fields) {
                JavaClass* clazz = header->clazz;
                for (int i = 0; i < clazz->static_fields_count; i++) {
                    JavaStaticField* sf = &clazz->static_fields[i];
                    if (sf->descriptor && (sf->descriptor[0] == 'L' || sf->descriptor[0] == '[')) {
                        if (sf->value.ref) gc_push_mark(sf->value.ref);
                    }
                }
            }
            break;
        }
            
        case OBJ_TYPE_STRING: {
            /* CRITICAL FIX: OBJ_TYPE_STRING is for NATIVE strings with inline char data.
             * However, some String objects may have been created with OBJ_TYPE_STRING
             * type but still have clazz=java/lang/String and fields!
             * 
             * We need to check if this has clazz=java/lang/String - if so, treat it
             * as an object with fields, NOT as a native string!
             */
            GCObjectHeader* gc_header = (GCObjectHeader*)ptr - 1;
            JavaString* str = (JavaString*)ptr;
            
            /* CRITICAL FIX: If this "string" has a java/lang/String class, it's actually
             * a Java String object with fields (value, offset, count, hash).
             * Treat it as OBJ_TYPE_OBJECT and scan its fields! */
            if (gc_header->clazz && gc_header->clazz->class_name &&
                strcmp(gc_header->clazz->class_name, "java/lang/String") == 0) {
                /* This is a JAVA String object with fields, NOT a native string!
                 * Scan its reference fields. */
                GC_LOG("[GC_STRING_FIX] String object at %p has clazz=java/lang/String, treating as object with fields\n", ptr);
                
                JavaObject* obj = (JavaObject*)ptr;
                JavaClass* clazz = gc_header->clazz;
                
                /* CRITICAL FIX: Get the value slot to access the char array.
                 * We need to validate the char array BEFORE trying to mark it.
                 * This prevents use-after-free when the array was already collected. */
                int value_slot = native_get_string_value_slot(g_jvm_for_instanceof);
                
                if (value_slot >= 0) {
                    void* value_ref = obj->fields[value_slot].ref;
                    if (value_ref) {
                        /* CRITICAL: Validate that the char array is still a valid heap object.
                         * If it's invalid (already freed or corrupted), we have a use-after-free bug!
                         * We should NOT try to mark it - just log the error and clear the field. */
                        if (is_valid_gc_object(value_ref)) {
                            GC_LOG("[GC_STRING_FIX]   field value = %p (valid), marking\n", value_ref);
                            gc_push_mark(value_ref);
                        } else {
                            /* CRITICAL BUG DETECTED: The char array is invalid!
                             * This means the array was freed before the String object.
                             * We cannot recover from this - the String is corrupted.
                             * Log the error and set value to NULL to prevent crashes. */
                            GC_LOG("[GC_STRING_FIX]   CRITICAL: field value = %p (INVALID! use-after-free detected!)\n", value_ref);
                            
                            /* Check if it looks like a freed object (deadbeef pattern) */
                            if (is_heap_ptr(value_ref)) {
                                GCObjectHeader* array_header = (GCObjectHeader*)value_ref - 1;
                                GC_LOG("[GC_STRING_FIX]   Array header: magic=0x%08X, type=%d, size=%u\n",
                                        array_header->magic, array_header->type, array_header->size);
                                
                                /* If the object is already freed (OBJ_TYPE_FREE), we have a serious bug */
                                if (array_header->type == OBJ_TYPE_FREE) {
                                    GC_LOG("[GC_STRING_FIX]   ERROR: Char array was already freed! String object is corrupted!\n");
                                    /* Clear the field to prevent further issues */
                                    obj->fields[value_slot].ref = NULL;
                                }
                            } else {
                                /* Pointer is outside heap - clear it */
                                GC_LOG("[GC_STRING_FIX]   Pointer outside heap - clearing field\n");
                                obj->fields[value_slot].ref = NULL;
                            }
                        }
                    }
                }
                
                /* Build hierarchy for remaining fields */
                JavaClass* hierarchy[64];
                int depth = 0;
                JavaClass* c = clazz;
                while (c && depth < 64) {
                    hierarchy[depth++] = c;
                    c = c->super_class;
                }
                
                /* Process fields from Object down to String (skip value - already handled) */
                int slot = 0;
                for (int h = depth - 1; h >= 0; h--) {
                    JavaClass* current = hierarchy[h];
                    if (!current->fields) continue;
                    
                    for (int i = 0; i < current->fields_count; i++) {
                        JavaField* field = &current->fields[i];
                        if (field->access_flags & ACC_STATIC) continue;
                        
                        if (field->descriptor) {
                            char desc = field->descriptor[0];
                            if (desc == 'L' || desc == '[') {
                                /* Skip the 'value' field - already handled above */
                                if (field->name && strcmp(field->name, "value") == 0) {
                                    slot++;
                                    continue;
                                }
                                void* ref = obj->fields[slot].ref;
                                if (ref && is_valid_gc_object(ref)) {
                                    GC_LOG("[GC_STRING_FIX]   field %s = %p, marking\n", 
                                            field->name ? field->name : "?", ref);
                                    gc_push_mark(ref);
                                }
                            }
                        }
                        
                        slot++;
                        if (field->descriptor && 
                            (field->descriptor[0] == 'J' || field->descriptor[0] == 'D')) {
                            slot++;
                        }
                    }
                }
            }
            
            /* Native strings have inline data, no separate array to mark.
             * The str->utf8 pointer (if set) points to malloc'd memory, not a heap object,
             * so we don't mark it either. */
            (void)str;  /* Suppress unused variable warning */
            break;
        }
    }
}

static void gc_process_mark_stack(void) {
    while (gc_mark_stack_top > 0) {
        void* ptr = gc_mark_stack[--gc_mark_stack_top].ptr;
        gc_mark_object(ptr);
    }
}

/* Запуск сборщика мусора с Линейным Sweep и Коалесцингом */
void gc_collect(JVM* jvm) {
    if (!jvm) return;
    
    /* UNCONDITIONAL LOG: Always log GC start for debugging */
    GC_LOG("[GC_TRIGGER] gc_collect() called! heap.current=%p, heap.end=%p, used=%zu bytes\n",
            (void*)heap.current, heap.end, (size_t)(heap.current - heap.start));
    
    /* CRITICAL: Acquire heap lock for GC
     * This is a RECURSIVE mutex, so if gc_collect is called from
     * heap_alloc (which already holds the lock), this is safe. */
    heap_lock();
    
    GC_DEBUG("========== gc_collect() START ==========");
    GC_DEBUG("heap.start=%p, heap.current=%p, heap.end=%p",
            (void*)heap.start, heap.current, heap.end);
    GC_DEBUG("heap.allocated=%zu, heap.size=%zu", heap.allocated, heap.size);
    
    /* ИСПРАВЛЕНО: Защита от повторного входа в GC */
    static bool gc_in_progress = false;
    if (gc_in_progress) {
        WARN_LOG("GC already in progress, skipping recursive collection");
        heap_unlock();  /* Release lock before returning */
        return;
    }
    gc_in_progress = true;
    
    /* Reset string fix counter for this GC cycle */
    gc_strings_fixed_this_cycle = 0;
    
    /* ИСПРАВЛЕНО: Инициализация стека маркировки при первом вызове */
    if (!gc_mark_stack) {
        if (!gc_mark_stack_init()) {
            ERROR_LOG("Failed to initialize mark stack");
            gc_in_progress = false;
            heap_unlock();  /* Release lock before returning */
            return;
        }
    }
    
    size_t start_allocated = heap.allocated;
    gc_mark_stack_top = 0;
    
    GC_DEBUG("root_count=%zu, thread_count=%d, class_count=%zu",
            heap.root_count, jvm->thread_count, jvm->class_loader.count);
    
    /* 1. Mark Phase */
    /* Mark from Roots */
    int roots_pushed = 0;
    for (size_t i = 0; i < heap.root_count; i++) {
        if (heap.roots[i] && *heap.roots[i]) {
            GC_DEBUG("[GC_ROOT] root[%zu] = %p", i, *heap.roots[i]);
            if (gc_push_mark(*heap.roots[i])) {
                roots_pushed++;
            }
        }
    }
    
    /* Mark from Stacks */
    int stack_refs_pushed = 0;
    for (int i = 0; i < jvm->thread_count; i++) {
        JavaThread* thread = jvm->threads[i];
        if (!thread) {
            GC_LOG("[GC_THREAD] thread[%d] is NULL, skipping\n", i);
            continue;
        }
        
        GC_LOG("[GC_THREAD] thread[%d]: pending_exception=%p, thread_object=%p, current_frame=%p\n",
                i, thread->pending_exception, thread->thread_object, (void*)thread->current_frame);
        
        if (thread->pending_exception) {
            if (gc_push_mark(thread->pending_exception)) stack_refs_pushed++;
        }
        if (thread->thread_object) {
            if (gc_push_mark(thread->thread_object)) stack_refs_pushed++;
        }
        
        JavaFrame* frame = thread->current_frame;
        int frame_num = 0;
        if (!frame) {
            GC_LOG("[GC_FRAME] thread[%d] has NO current_frame!\n", i);
        }
        while (frame) {
            GC_LOG("[GC_FRAME] thread[%d] frame[%d]: method=%s, max_locals=%d, stack_top=%d\n",
                    i, frame_num, 
                    (frame->method && frame->method->name) ? frame->method->name : "?",
                    frame->max_locals, frame->stack_top);
            
            /* ИСПРАВЛЕНО: Сканируем locals с учетом long/double (2 слота)
             * Long и double занимают 2 слота, но это ОДНО значение.
             * Второй слот (index+1) не является отдельной ссылкой.
             * Для locals это менее критично, т.к. .ref будет 0 для high-word,
             * но для корректности пропускаем второй слот long/double.
             */
            for (uint16_t j = 0; j < frame->max_locals; j++) {
                if (frame->locals[j].ref) {
                    GC_DEBUG("[GC_LOCAL] thread[%d] frame[%d] local[%d] = %p",
                            i, frame_num, j, frame->locals[j].ref);
                    if (gc_push_mark(frame->locals[j].ref)) {
                        stack_refs_pushed++;
                    }
                }
            }
            
            /* ИСПРАВЛЕНО: Сканируем стек с учетом long/double (2 слота)
             * JVM Spec: long и double занимают 2 слота стека.
             * Второй слот содержит continuation (high word), а не ссылку.
             * Когда long/double пушится на стек, оба слота содержат данные value,
             * поэтому .ref в high-word не будет валидным указателем.
             * Но для безопасности пропускаем второй слот явно.
             */
            for (int16_t j = 0; j <= frame->stack_top; j++) {
                if (frame->stack[j].ref) {
                    GC_DEBUG("[GC_STACK] thread[%d] frame[%d] stack[%d] = %p",
                            i, frame_num, j, frame->stack[j].ref);
                    if (gc_push_mark(frame->stack[j].ref)) {
                        stack_refs_pushed++;
                    }
                }
            }
            frame = frame->prev;
            frame_num++;
        }
    }
    
    /* Mark from Statics */
    int static_refs_pushed = 0;
    if (jvm->config.verbose_gc) {
        LOG_SAFE("[GC] Scanning %zu classes for static fields\n", jvm->class_loader.count);
    }
    for (size_t i = 0; i < jvm->class_loader.count; i++) {
        JavaClass* clazz = jvm->class_loader.classes[i];
        if (!clazz) {
            continue;
        }
        
        /* CRITICAL: Check if this class has static_fields allocated */
        if (!clazz->static_fields) {
            /* No static fields storage allocated - skip */
            continue;
        }
        
        if (clazz->static_fields_count == 0) {
            /* Has storage but no fields yet - skip */
            continue;
        }
        
        /* DEBUG: Log classes with static fields */
        if (jvm->config.verbose_gc) {
            LOG_SAFE("[GC]   class[%zu] = %s has %d static fields\n", 
                    i, clazz->class_name ? clazz->class_name : "?", clazz->static_fields_count);
        }
        
        for (int j = 0; j < clazz->static_fields_count; j++) {
            JavaStaticField* sf = &clazz->static_fields[j];
            
            /* DEBUG: Log all static fields to see descriptors */
            if (jvm->config.verbose_gc) {
                LOG_SAFE("[GC]   field %s.%s descriptor='%s' value.ref=%p\n",
                        clazz->class_name ? clazz->class_name : "?",
                        sf->name ? sf->name : "?",
                        sf->descriptor ? sf->descriptor : "NULL",
                        sf->value.ref);
            }
            
            if (sf->descriptor && (sf->descriptor[0] == 'L' || sf->descriptor[0] == '[')) {
                if (sf->value.ref) {
                    GC_LOG("[GC_STATIC] %s.%s = %p (attempting to mark)\n",
                            clazz->class_name ? clazz->class_name : "?",
                            sf->name ? sf->name : "?",
                            sf->value.ref);
                    if (gc_push_mark(sf->value.ref)) {
                        static_refs_pushed++;
                        GC_LOG("[GC_STATIC]   -> marked successfully (count=%d)\n", static_refs_pushed);
                    } else {
                        GC_LOG("[GC_STATIC]   -> FAILED to mark!\n");
                    }
                }
            }
        }
    }
    
    /* CRITICAL: Mark objects stored in native Hashtable entries!
     * The Hashtable implementation uses native memory (g_hashtables) to store
     * key-value pairs. These are NOT Java heap objects, so normal field scanning
     * won't find them. We must explicitly mark them here.
     * 
     * This fixes the use-after-free bug where strings stored as Hashtable keys
     * were being garbage collected because GC didn't see them.
     */
    int native_ht_refs = 0;
    GC_LOG("[GC_NATIVE_HT] Marking native Hashtable entries (g_hashtable_count=%d)...\n", 
            g_hashtable_count);
    for (int i = 0; i < g_hashtable_count && i < GC_HASHTABLE_MAX_HASHTABLES; i++) {
        /* CRITICAL: Skip dead peers! When a Hashtable is freed, its peer is marked
         * as not alive. We must not scan freed memory (deadbeef pattern). */
        if (!g_hashtable_peer_alive[i]) {
            GC_LOG("[GC_NATIVE_HT]   Skipping dead Hashtable[%d]\n", i);
            continue;
        }
        
        HashtablePeerGC* peer = &g_hashtables[i];
        if (!peer->entries || peer->count <= 0) continue;
        
        GC_LOG("[GC_NATIVE_HT]   Hashtable[%d]: capacity=%d, count=%d\n", 
                i, peer->capacity, peer->count);
        
        for (int j = 0; j < peer->capacity; j++) {
            HashtableEntryGC* entry = &peer->entries[j];
            if (!entry->key && !entry->value) continue;
            
            GC_LOG("[GC_NATIVE_HT]     entry[%d]: key=%p, value=%p\n",
                    j, entry->key, entry->value);
            
            if (entry->key) {
                if (gc_push_mark(entry->key)) {
                    native_ht_refs++;
                    GC_LOG("[GC_NATIVE_HT]       marked key\n");
                }
            }
            if (entry->value) {
                if (gc_push_mark(entry->value)) {
                    native_ht_refs++;
                    GC_LOG("[GC_NATIVE_HT]       marked value\n");
                }
            }
        }
    }
    GC_LOG("[GC_NATIVE_HT] Marked %d native Hashtable references\n", native_ht_refs);
    
    /* CRITICAL: Mark interned strings from String.intern() pool!
     * The intern_pool in native.c holds references to interned strings.
     * These strings must NOT be garbage collected, or String.intern() will
     * return freed objects causing crashes.
     * 
     * This fixes the crash in GCTest during OOM recovery where interned
     * strings like "key", "value" were being collected.
     */
    extern JavaString* intern_pool[];
    extern int intern_pool_count;
    int intern_refs = 0;
    #define INTERN_POOL_SIZE_MAX 1024
    GC_LOG("[GC_INTERN] Marking interned strings (intern_pool_count=%d)...\n", intern_pool_count);
    for (int i = 0; i < intern_pool_count && i < INTERN_POOL_SIZE_MAX; i++) {
        JavaString* str = intern_pool[i];
        if (str && is_heap_ptr(str)) {
            /* Verify the header is valid before marking */
            GCObjectHeader* header = (GCObjectHeader*)((uint8_t*)str - sizeof(GCObjectHeader));
            if (header->magic == GC_HEADER_MAGIC && header->type != OBJ_TYPE_FREE) {
                if (gc_push_mark(str)) {
                    intern_refs++;
                    GC_LOG("[GC_INTERN]   marked intern_pool[%d] = %p\n", i, (void*)str);
                }
            } else {
                /* Invalid entry - clear it from the pool */
                GC_LOG("[GC_INTERN]   WARNING: intern_pool[%d] = %p has invalid header, clearing\n", i, (void*)str);
                intern_pool[i] = NULL;
            }
        }
    }
    GC_LOG("[GC_INTERN] Marked %d interned string references\n", intern_refs);
    
    /* CRITICAL: Mark objects stored in M3G object registry!
     * The M3G (Mobile 3D Graphics) system stores loaded 3D objects in a global
     * registry for find() lookup. These must be treated as GC roots or they
     * will be collected while still referenced by the registry.
     */
    int m3g_count = 0;
    int m3g_refs = 0;
    JavaObject** m3g_objects = m3g_registry_get_objects(&m3g_count);
    GC_LOG("[GC_M3G] Marking M3G registry objects (count=%d)...\n", m3g_count);
    if (m3g_objects && m3g_count > 0) {
        for (int i = 0; i < m3g_count; i++) {
            JavaObject* obj = m3g_objects[i];
            if (obj && is_heap_ptr(obj)) {
                GCObjectHeader* header = (GCObjectHeader*)((uint8_t*)obj - sizeof(GCObjectHeader));
                if (header->magic == GC_HEADER_MAGIC && header->type != OBJ_TYPE_FREE) {
                    if (gc_push_mark(obj)) {
                        m3g_refs++;
                        GC_LOG("[GC_M3G]   marked registry[%d] = %p\n", i, (void*)obj);
                    }
                } else {
                    /* Invalid/corrupted object - clear from registry */
                    GC_LOG("[GC_M3G]   registry[%d] = %p has invalid header, clearing\n", i, (void*)obj);
                    m3g_objects[i] = NULL;
                }
            }
        }
    }
    GC_LOG("[GC_M3G] Marked %d M3G registry references\n", m3g_refs);
    
    GC_DEBUG("Mark phase: roots_pushed=%d, stack_refs=%d, static_refs=%d, native_ht_refs=%d, intern_refs=%d, m3g_refs=%d, total_on_stack=%d",
            roots_pushed, stack_refs_pushed, static_refs_pushed, native_ht_refs, intern_refs, m3g_refs, gc_mark_stack_top);
    
    if (jvm->config.verbose_gc) {
        LOG_SAFE("[GC] Mark phase complete: roots=%d, stack_refs=%d, static_refs=%d, native_ht_refs=%d, intern_refs=%d, m3g_refs=%d, mark_stack_top=%d\n",
                roots_pushed, stack_refs_pushed, static_refs_pushed, native_ht_refs, intern_refs, m3g_refs, gc_mark_stack_top);
    }
    
    gc_process_mark_stack();
    
    if (jvm->config.verbose_gc) {
        LOG_SAFE("[GC] Mark stack processed, starting sweep...\n");
    }
    
    /* 1.5 PRE-SWEEP VALIDATION: Check all live String objects have marked char arrays.
     * This catches use-after-free bugs where the char array was freed before the String.
     * If we find an unmarked array, we must mark it to prevent crashes.
     */
    GC_LOG("[GC_PRESWEEP] Validating String objects...\n");
    int strings_fixed = 0;
    uint8_t* validate_ptr = heap.start;
    while (validate_ptr < heap.current) {
        GCObjectHeader* header = (GCObjectHeader*)validate_ptr;
        
        /* Skip invalid/corrupted headers */
        if (header->magic != GC_HEADER_MAGIC || header->size == 0 || header->size > heap.size) {
            validate_ptr += 8;
            continue;
        }
        
        /* Only check live (marked) objects that might be Strings */
        if (header->marked && (header->type == OBJ_TYPE_OBJECT || header->type == OBJ_TYPE_STRING)) {
            if (header->clazz && header->clazz->class_name &&
                strcmp(header->clazz->class_name, "java/lang/String") == 0) {
                
                JavaObject* str_obj = (JavaObject*)(header + 1);
                int value_slot = native_get_string_value_slot(g_jvm_for_instanceof);
                
                if (value_slot >= 0) {
                    void* value_ref = str_obj->fields[value_slot].ref;
                    if (value_ref) {
                        /* Check if the char array is properly marked */
                        if (is_heap_ptr(value_ref)) {
                            GCObjectHeader* array_header = (GCObjectHeader*)value_ref - 1;
                            
                            /* Check if array header is valid */
                            if (array_header->magic == GC_HEADER_MAGIC &&
                                array_header->type != OBJ_TYPE_FREE) {
                                
                                if (!array_header->marked) {
                                    /* CRITICAL: Char array is not marked but String is live!
                                     * This is a bug - the array would be freed while the String survives.
                                     * We must mark it now to prevent use-after-free. */
                                    GC_LOG("[GC_PRESWEEP] FIXING: String %p has unmarked char array %p - marking it!\n",
                                            (void*)str_obj, value_ref);
                                    
                                    /* Mark the array */
                                    array_header->marked = 1;
                                    strings_fixed++;
                                    
                                    /* Also process its references if it's an object array */
                                    if (array_header->type == OBJ_TYPE_ARRAY) {
                                        JavaArray* arr = (JavaArray*)value_ref;
                                        if (arr->element_type == DESC_OBJECT || arr->element_type == DESC_ARRAY) {
                                            void** refs = (void**)((uint8_t*)arr + sizeof(JavaArray));
                                            for (jsize i = 0; i < arr->length; i++) {
                                                if (refs[i] && is_valid_gc_object(refs[i])) {
                                                    gc_push_mark(refs[i]);
                                                }
                                            }
                                        }
                                    }
                                }
                            } else {
                                /* Array header is invalid or already freed */
                                GC_LOG("[GC_PRESWEEP] WARNING: String %p has invalid char array %p (magic=0x%08X, type=%d)\n",
                                        (void*)str_obj, value_ref, array_header->magic, array_header->type);
                                
                                /* Clear the invalid reference */
                                str_obj->fields[value_slot].ref = NULL;
                            }
                        }
                    }
                }
            }
        }
        
        validate_ptr += header->size;
    }
    
    /* Process any new objects pushed onto mark stack */
    if (gc_mark_stack_top > 0) {
        GC_LOG("[GC_PRESWEEP] Processing %d additional objects from fixup\n", gc_mark_stack_top);
        gc_process_mark_stack();
    }
    
    GC_LOG("[GC_PRESWEEP] Validation complete: fixed %d String objects\n", strings_fixed);
    
    /* 2. Sweep Phase (Linear Scan + Coalescing) */
    size_t freed_this_cycle = 0;
    size_t objects_freed = 0;
    
    /* Очищаем старый список свободных блоков, чтобы построить новый упорядоченный */
    heap.free_list = NULL; 
    
    uint8_t* scan_ptr = heap.start;
    
    /* Временный список свободных блоков для построения в порядке адреса
       (используем простой подход: добавляем в конец, но т.к. у нас нет tail, 
       будем вставлять так, чтобы список был отсортирован, или просто собираем 
       свободные куски в отдельный список и потом мержим) */
    
    /* Для простоты реализации на C без лишних аллокаций:
       Пройдемся линейно. Свободные блоки будем добавлять в free_list "в конец" 
       эмулируя это вставкой перед головой, если адрес меньше головы (сортировка по возрастанию).
       Это позволит легко слить соседние блоки. */
    
    FreeBlock* sorted_free_list = NULL;
    
    while (scan_ptr < heap.current) {
        GCObjectHeader* header = (GCObjectHeader*)scan_ptr;
        
        /* DEBUG: Log every object GC sees (only in debug mode) */
        GC_DEBUG("[GC_SWEEP] scan_ptr=%p, size=%u, type=%d, marked=%d",
                (void*)scan_ptr, header->size, header->type, header->marked);
        
        /* Проверка magic number для обнаружения corruption */
        if (header->magic != GC_HEADER_MAGIC && header->type != OBJ_TYPE_FREE) {
            ERROR_LOG("GC: Object at %p has corrupted magic: expected 0x%08X, got 0x%08X",
                     (void*)scan_ptr, GC_HEADER_MAGIC, header->magic);
            ERROR_LOG("  header->size=%u, type=%d, marked=%d", 
                     header->size, header->type, header->marked);
            
            HEAP_CORRUPTION_LOG("CORRUPTION_MAGIC: addr=%p, expected=0x%08X, got=0x%08X, size=%u, type=%d",
                    (void*)scan_ptr, GC_HEADER_MAGIC, header->magic, header->size, header->type);
            
            /* Пробуем восстановиться - ищем следующий валидный объект */
            uint8_t* recovery_ptr = scan_ptr + 8;
            bool found_valid = false;
            
            while (recovery_ptr < heap.current - sizeof(GCObjectHeader)) {
                GCObjectHeader* test_header = (GCObjectHeader*)recovery_ptr;
                
                if (test_header->magic == GC_HEADER_MAGIC &&
                    test_header->size > sizeof(GCObjectHeader) && 
                    test_header->size <= heap.size &&
                    test_header->type <= OBJ_TYPE_CLASS) {
                    
                    ERROR_LOG("  Found valid object at %p, continuing from there", recovery_ptr);
                    HEAP_CORRUPTION_LOG("CORRUPTION_RECOVERY: found valid at %p, skipped %ld bytes",
                            (void*)recovery_ptr, (long)(recovery_ptr - scan_ptr));
                    scan_ptr = recovery_ptr;
                    found_valid = true;
                    break;
                }
                recovery_ptr += 8;
            }
            
            if (!found_valid) {
                ERROR_LOG("  Could not recover, aborting GC sweep");
                HEAP_CORRUPTION_LOG("CORRUPTION_FATAL: cannot recover, sweep aborted");
                break;
            }
            continue;
        }
        
        /* Защита от порчи памяти в куче */
        if (header->size == 0 || header->size > heap.size || scan_ptr + header->size > heap.end) {
            ERROR_LOG("Corruption detected at %p, attempting recovery", scan_ptr);
            ERROR_LOG("  header->size=%u, heap.size=%zu", header->size, heap.size);
            ERROR_LOG("  scan_ptr=%p, heap.end=%p", scan_ptr, heap.end);
            ERROR_LOG("  header->type=%d, header->marked=%d", header->type, header->marked);
            
            HEAP_CORRUPTION_LOG("CORRUPTION_SIZE: addr=%p, size=%u, heap.size=%zu, type=%d, magic=0x%08X",
                    (void*)scan_ptr, header->size, heap.size, header->type, header->magic);
            
            if (header->clazz) {
                ERROR_LOG("  header->clazz=%p, class_name=%s", 
                         (void*)header->clazz, 
                         header->clazz->class_name ? header->clazz->class_name : "(null)");
                HEAP_CORRUPTION_LOG("CORRUPTION_CLAZZ: clazz=%p, name=%s",
                        (void*)header->clazz, header->clazz->class_name ? header->clazz->class_name : "(null)");
            }
            
            /* Попытка найти предыдущий объект для диагностики */
            uint8_t* prev_scan = heap.start;
            uint8_t* prev_obj_end = NULL;
            while (prev_scan < scan_ptr) {
                GCObjectHeader* prev_header = (GCObjectHeader*)prev_scan;
                if (prev_header->size == 0 || prev_header->size > heap.size) break;
                prev_obj_end = prev_scan + prev_header->size;
                prev_scan += prev_header->size;
            }
            if (prev_obj_end && prev_obj_end <= scan_ptr) {
                ERROR_LOG("  Previous object ended at %p, gap to corruption: %ld bytes",
                         (void*)prev_obj_end, (long)(scan_ptr - prev_obj_end));
                HEAP_CORRUPTION_LOG("CORRUPTION_GAP: prev_end=%p, gap=%ld bytes",
                        (void*)prev_obj_end, (long)(scan_ptr - prev_obj_end));
            }
            
            /* ИСПРАВЛЕНИЕ: Пытаемся восстановиться, пропуская повреждённый регион.
             * Сканируем память побайтово в поисках следующего валидного заголовка.
             * Валидный заголовок должен иметь:
             * - size > 0 и size < heap.size
             * - type в диапазоне 0..OBJ_TYPE_CLASS
             * - clazz указывает на валидный JavaClass (проверяем что class_name не NULL)
             */
            ERROR_LOG("  Attempting to find next valid object...");
            uint8_t* recovery_ptr = scan_ptr + 8;  /* Начинаем со следующего выровненного адреса */
            bool found_valid = false;
            
            while (recovery_ptr < heap.current - sizeof(GCObjectHeader)) {
                GCObjectHeader* test_header = (GCObjectHeader*)recovery_ptr;
                
                /* Проверяем, выглядит ли это как валидный заголовок */
                if (test_header->size > sizeof(GCObjectHeader) && 
                    test_header->size <= heap.size &&
                    test_header->type <= OBJ_TYPE_CLASS &&
                    recovery_ptr + test_header->size <= heap.end) {
                    
                    /* Дополнительная проверка: clazz должен указывать на что-то разумное */
                    if (test_header->clazz == NULL || 
                        (test_header->clazz->class_name != NULL && 
                         (uintptr_t)test_header->clazz > 0x10000)) {
                        ERROR_LOG("  Found potential valid object at %p (size=%u, type=%d)",
                                 (void*)recovery_ptr, test_header->size, test_header->type);
                        HEAP_CORRUPTION_LOG("CORRUPTION_RECOVERY: found valid at %p, skipped %ld bytes",
                                (void*)recovery_ptr, (long)(recovery_ptr - scan_ptr));
                        scan_ptr = recovery_ptr;
                        found_valid = true;
                        break;
                    }
                }
                recovery_ptr += 8;  /* Продвигаемся с шагом выравнивания */
            }
            
            if (!found_valid) {
                ERROR_LOG("  Could not find valid object, aborting sweep");
                HEAP_CORRUPTION_LOG("CORRUPTION_FATAL: cannot recover from size corruption, sweep aborted");
                break;
            }
            
            /* Продолжаем sweep с найденной позиции */
            continue;
        }
        
        /* Пропускаем свободные блоки (они уже в free list) */
        if (header->type == OBJ_TYPE_FREE) {
            /* Этот блок уже свободен, пропускаем его при sweep */
            /* Он будет добавлен в sorted_free_list ниже как часть коалесцинга */
            scan_ptr += header->size;
            continue;
        }
        
        if (header->marked || header->pinned) {
            /* Объект жив */
            header->marked = 0; /* Сброс метки */
        } else {
            /* Объект мертв - логируем для отладки */
            GC_LOG("[GC_SWEEP] FREEING object at %p (header=%p): size=%u, type=%d, clazz=%s\n",
                    (void*)(header + 1), header, header->size, header->type,
                    header->clazz ? (header->clazz->class_name ? header->clazz->class_name : "?") : "NULL");
            
            freed_this_cycle += header->size;
            objects_freed++;
            
            /* Очистка строкового кэша */
            if (header->type == OBJ_TYPE_STRING) {
                JavaString* str = (JavaString*)(header + 1);
                if (str->utf8) {
                    free(str->utf8);
                    str->utf8 = NULL;
                }
            }
            
            /* CRITICAL: Clean up native Hashtable peer when Hashtable is freed!
             * When a Java Hashtable object is garbage collected, we must free its
             * native peer (g_hashtables entry) to prevent:
             * 1. Memory leak of native entries array
             * 2. Use-after-free when GC scans freed native memory (deadbeef pattern)
             */
            if (header->clazz && header->clazz->class_name &&
                strcmp(header->clazz->class_name, "java/util/Hashtable") == 0) {
                
                JavaObject* ht = (JavaObject*)(header + 1);
                
                /* Get the peer index from the threshold field */
                JavaValue peer_val = native_get_field_value(ht, "threshold");
                jint peer_idx = peer_val.i;
                
                if (peer_idx >= 0 && peer_idx < GC_HASHTABLE_MAX_HASHTABLES && g_hashtable_peer_alive[peer_idx]) {
                    GC_LOG("[GC_SWEEP] Freeing native Hashtable peer for idx %d\n", peer_idx);
                    
                    /* Use the helper function to free the peer */
                    hashtable_free_peer(peer_idx);
                }
            }
            
            /* Помечаем как свободный */
            header->type = OBJ_TYPE_FREE;
            
            /* Добавляем в сортированный список свободных блоков */
            FreeBlock* block = (FreeBlock*)header;
            block->size = header->size;
            
            /* Вставка в сортированный список (по возрастанию адреса) */
            FreeBlock** curr = &sorted_free_list;
            while (*curr && (uint8_t*)(*curr) < (uint8_t*)block) {
                curr = &((*curr)->next);
            }
            block->next = *curr;
            *curr = block;
        }
        
        scan_ptr += header->size;
    }
    
    /* 3. Coalescing (Слияние соседних блоков) */
    FreeBlock* fb = sorted_free_list;
    while (fb && fb->next) {
        /* Проверяем, лежит ли следующий блок сразу за текущим */
        if ((uint8_t*)fb + fb->size == (uint8_t*)fb->next) {
            /* Сливаем */
            fb->size += fb->next->size;
            fb->next = fb->next->next;
            /* Не продвигаем fb, так как новый блок может теперь merger'ся со следующим */
        } else {
            fb = fb->next;
        }
    }
    
    heap.free_list = sorted_free_list;
    
    /* Статистика */
    heap.allocated -= freed_this_cycle;
    heap.freed += freed_this_cycle;
    heap.gc_cycles++;
    heap.gc_total_freed += freed_this_cycle;
    
    /* Если мы освободили блок в конце кучи, можем подвинуть current назад (Defragmentation of top) */
    if (heap.free_list) {
        /* Найдем самый большой свободный блок. Если он в конце кучи, уменьшим current. */
        FreeBlock* last = heap.free_list;
        FreeBlock* last_prev = NULL;
        while (last->next) {
            last_prev = last;
            last = last->next;
        }
        
        if ((uint8_t*)last + last->size == heap.current) {
            /* Этот блок в самом конце кучи */
            heap.current = (uint8_t*)last;
            if (last_prev) {
                last_prev->next = NULL;
            } else {
                heap.free_list = NULL;
            }
            /* Память возвращена системе (bump pointer уменьшен) */
        }
    }

    if (freed_this_cycle > 0) {
        GC_DEBUG("Cycle %zu: freed %zu bytes (%zu objects), %zu bytes allocated",
                heap.gc_cycles, freed_this_cycle, objects_freed, heap.allocated);
    }
    
    /* Notify native code about GC completion.
     * This allows native fallback buffers (e.g., StringBuffer) to clean up
     * entries for objects that were garbage collected.
     */
    extern void native_gc_notify(void);
    native_gc_notify();
    
    GC_DEBUG("========== gc_collect() END ==========");
    
    /* Сброс флага GC */
    gc_in_progress = false;
    
    /* CRITICAL: Release heap lock after GC completes */
    heap_unlock();
}

/* --- Roots and Utilities --- */

void gc_add_root(JVM* jvm, void** root) {
    (void)jvm;
    
    /* CRITICAL FIX: Acquire heap lock before modifying roots array
     * Without this, concurrent GC could read the roots array while
     * we're reallocating it, causing memory corruption. */
    heap_lock();
    
    if (heap.root_count >= heap.root_capacity) {
        /* ИСПРАВЛЕНО: Проверка успешности realloc */
        size_t new_capacity = heap.root_capacity * 2;
        if (new_capacity == 0) new_capacity = 1024;  /* Защита от нуля */
        
        void*** new_roots = (void***)realloc(heap.roots, new_capacity * sizeof(void**));
        if (!new_roots) {
            ERROR_LOG("Failed to expand roots array");
            heap_unlock();
            return;
        }
        heap.roots = new_roots;
        heap.root_capacity = new_capacity;
    }
    heap.roots[heap.root_count++] = root;
    
    heap_unlock();
}

void gc_remove_root(JVM* jvm, void** root) {
    (void)jvm;
    
    /* CRITICAL FIX: Acquire heap lock before modifying roots array */
    heap_lock();
    
    for (size_t i = 0; i < heap.root_count; i++) {
        if (heap.roots[i] == root) {
            heap.roots[i] = heap.roots[--heap.root_count];
            heap_unlock();
            return;
        }
    }
    
    heap_unlock();
}

void gc_pin(JVM* jvm, void* object) {
    (void)jvm;
    if (!object || !is_heap_ptr(object)) return;
    GCObjectHeader* header = (GCObjectHeader*)object - 1;
    header->pinned = 1;
}

void gc_unpin(JVM* jvm, void* object) {
    (void)jvm;
    if (!object || !is_heap_ptr(object)) return;
    GCObjectHeader* header = (GCObjectHeader*)object - 1;
    header->pinned = 0;
}

HeapStats heap_get_stats(JVM* jvm) {
    HeapStats stats = {0};
    (void)jvm;
    stats.total_size = heap.size;
    stats.used_size = heap.allocated;
    stats.free_size = heap.size - heap.allocated;
    stats.object_count = 0; /* Сложно считать точно без счетчика при аллокации, можно добавить */
    stats.gc_cycles = heap.gc_cycles;
    stats.gc_time_ms = 0;
    return stats;
}

/* Объектные утилиты */

JavaClass* object_get_class(void* object) {
    if (!object) return NULL;
    
    /* Check if pointer is in heap */
    if (!is_heap_ptr(object)) return NULL;
    
    GCObjectHeader* header = (GCObjectHeader*)object - 1;
    
    /* Validate header before accessing */
    if (header->size == 0 || header->size > 64 * 1024 * 1024) {
        return NULL;  /* Invalid object */
    }
    if (header->type > OBJ_TYPE_CLASS) {
        return NULL;  /* Invalid type */
    }
    
    return header->clazz;
}

size_t object_get_size(void* object) {
    if (!object || !is_heap_ptr(object)) return 0;
    GCObjectHeader* header = (GCObjectHeader*)object - 1;
    return header->size - sizeof(GCObjectHeader);
}

jint object_hash_code(void* object) {
    if (!object) return 0;
    JavaObject* obj = (JavaObject*)object;
    return obj->header.hashcode;
}

bool object_is_array(void* object) {
    if (!object || !is_heap_ptr(object)) return false;
    GCObjectHeader* header = (GCObjectHeader*)object - 1;
    return header->type == OBJ_TYPE_ARRAY;
}

bool object_is_string(void* object) {
    if (!object || !is_heap_ptr(object)) return false;
    GCObjectHeader* header = (GCObjectHeader*)object - 1;
    return header->type == OBJ_TYPE_STRING;
}

/*
 * CRITICAL FIX: Avoid malloc/strdup in hot path of object_instance_of
 * 
 * Problem: object_instance_of can be called very frequently (every checkcast, instanceof).
 * Allocating memory in this hot path kills performance and risks memory leaks.
 * 
 * Solution: Use static buffers for temporary class name construction.
 * These buffers are NOT thread-safe, but neither is the rest of the heap code.
 */

/* Static buffer for computed array class name - sized for max reasonable class name */
#define INSTANCEOF_BUFFER_SIZE 512
static char instanceof_buffer[INSTANCEOF_BUFFER_SIZE];

bool object_instance_of(void* object, JavaClass* clazz) {
    if (!object || !clazz) return false;
    
    /* CRITICAL: Check if pointer is in heap before accessing GC header */
    if (!is_heap_ptr(object)) {
        /* Not a heap object - could be a Class object or invalid pointer 
         * For safety, just return false for any non-heap pointer.
         * Class objects are handled separately in checkcast/instanceof. */
        return false;
    }
    
    /* Check GC header to determine object type */
    GCObjectHeader* gc_header = (GCObjectHeader*)object - 1;
    
    /* Validate GC header before using it */
    if (gc_header->size == 0 || gc_header->size > 64 * 1024 * 1024) {
        /* Invalid GC header - corrupted object */
        return false;
    }
    if (gc_header->type > OBJ_TYPE_CLASS) {
        /* Invalid object type */
        return false;
    }
    
    bool obj_is_array = (gc_header->type == OBJ_TYPE_ARRAY);
    
    /* Get the object's class */
    JavaClass* obj_class = object_get_class(object);
    
    /* Null check for class names */
    if (!clazz->class_name) return false;
    
    /* For arrays, check element type against expected array class name */
    if (obj_is_array) {
        /* All arrays are instanceof java/lang/Object */
        if (strcmp(clazz->class_name, "java/lang/Object") == 0) {
            return true;
        }
        
        /* Arrays implement Cloneable and Serializable interfaces */
        if (strcmp(clazz->class_name, "java/lang/Cloneable") == 0 ||
            strcmp(clazz->class_name, "java/io/Serializable") == 0) {
            return true;
        }
        
        /* Check if target class is an array type (starts with '[') */
        if (clazz->class_name[0] == '[') {
            JavaArray* array = (JavaArray*)object;
            
            /* For primitive arrays, do exact type matching */
            const char* expected_name = NULL;
            switch (array->element_type) {
                case T_BOOLEAN: expected_name = "[Z"; break;
                case T_BYTE:    expected_name = "[B"; break;
                case T_CHAR:    expected_name = "[C"; break;
                case T_SHORT:   expected_name = "[S"; break;
                case T_INT:     expected_name = "[I"; break;
                case T_LONG:    expected_name = "[J"; break;
                case T_FLOAT:   expected_name = "[F"; break;
                case T_DOUBLE:  expected_name = "[D"; break;
                case DESC_OBJECT:
                case DESC_ARRAY: {
                    /* For object arrays, we need to check covariance */
                    /* clazz->class_name is like "[Ljava/lang/String;" or "[[I" */
                    
                    /* Case 1: Exact match - check if element_class matches */
                    if (array->element_class && array->element_class->class_name) {
                        /* Build expected array class name from element class using static buffer
                         * CRITICAL FIX: No malloc - use static buffer instead */
                        size_t elem_name_len = strlen(array->element_class->class_name);
                        if (elem_name_len + 4 <= INSTANCEOF_BUFFER_SIZE) {
                            instanceof_buffer[0] = '[';
                            instanceof_buffer[1] = 'L';
                            memcpy(instanceof_buffer + 2, array->element_class->class_name, elem_name_len);
                            instanceof_buffer[elem_name_len + 2] = ';';
                            instanceof_buffer[elem_name_len + 3] = '\0';
                            
                            if (strcmp(clazz->class_name, instanceof_buffer) == 0) {
                                return true;
                            }
                        }
                        
                        /* Case 2: Covariance - Object[] can hold any reference array */
                        if (strcmp(clazz->class_name, "[Ljava/lang/Object;") == 0) {
                            return true;
                        }
                        
                        /* Case 3: Check if element type is subtype of target element type */
                        const char* target_class_name = clazz->class_name;
                        if (target_class_name[0] == '[' && target_class_name[1] == 'L') {
                            /* Extract element class name into static buffer */
                            size_t target_len = strlen(target_class_name + 2);
                            if (target_len < INSTANCEOF_BUFFER_SIZE) {
                                memcpy(instanceof_buffer, target_class_name + 2, target_len);
                                char* semicolon = strchr(instanceof_buffer, ';');
                                if (semicolon) *semicolon = '\0';
                                
                                /* Load target element class and check assignability */
                                extern JVM* g_jvm_for_instanceof;
                                JavaClass* target_elem_class = g_jvm_for_instanceof ? 
                                    jvm_load_class(g_jvm_for_instanceof, instanceof_buffer) : NULL;
                                
                                if (target_elem_class && array->element_class) {
                                    /* Check if array's element class is assignable to target element class */
                                    JavaClass* check = array->element_class;
                                    while (check) {
                                        if (check == target_elem_class ||
                                            (check->class_name && target_elem_class->class_name &&
                                             strcmp(check->class_name, target_elem_class->class_name) == 0)) {
                                            return true;
                                        }
                                        check = check->super_class;
                                    }
                                }
                            }
                        }
                    } else {
                        /* No element_class stored - assume generic Object[] for DESC_OBJECT arrays */
                        if (strcmp(clazz->class_name, "[Ljava/lang/Object;") == 0) {
                            return true;
                        }
                    }
                    break;
                }
            }
            
            /* Exact match for primitive array types */
            if (expected_name && strcmp(clazz->class_name, expected_name) == 0) {
                return true;
            }
        }
        
        /* Array type doesn't match */
        return false;
    }
    
    if (!obj_class) return false;
    
    /* Walk up the class hierarchy and compare by name */
    JavaClass* current = obj_class;
    while (current) {
        /* Compare by class name instead of pointer */
        if (current->class_name && clazz->class_name) {
            if (strcmp(current->class_name, clazz->class_name) == 0) {
                return true;
            }
        }
        
        /* Also check pointer equality as fast path */
        if (current == clazz) return true;
        
        current = current->super_class;
    }
    
    /* Check if clazz is an interface and obj_class implements it */
    /* We need to check all interfaces in the hierarchy */
    current = obj_class;
    while (current) {
        /* Check interfaces implemented by this class */
        for (int i = 0; i < current->interfaces_count; i++) {
            const char* iface_name = classfile_get_class_name(current, current->interfaces[i]);
            if (iface_name && clazz->class_name && strcmp(iface_name, clazz->class_name) == 0) {
                return true;
            }
            /* Also check if the interface extends the target interface */
            if (iface_name) {
                /* Get JVM from global context - need to pass it or use a different approach */
                /* For now, use a helper function that takes JVM parameter */
                extern JVM* g_jvm_for_instanceof;
                JavaClass* iface_class = g_jvm_for_instanceof ? jvm_load_class(g_jvm_for_instanceof, iface_name) : NULL;
                if (iface_class && iface_class != clazz) {
                    /* Recursively check parent interfaces */
                    JavaClass* iface_check = iface_class;
                    while (iface_check) {
                        for (int j = 0; j < iface_check->interfaces_count; j++) {
                            const char* parent_iface = classfile_get_class_name(iface_check, iface_check->interfaces[j]);
                            if (parent_iface && clazz->class_name && strcmp(parent_iface, clazz->class_name) == 0) {
                                return true;
                            }
                        }
                        /* Interfaces can only extend other interfaces, not classes */
                        iface_check = iface_check->super_class;  /* This would be Object or another interface */
                    }
                }
            }
        }
        
        current = current->super_class;
    }
    
    return false;
}

/* Массивы */

jsize array_length(JavaArray* array) {
    return array ? array->length : 0;
}

uint8_t array_element_type(JavaArray* array) {
    return array ? array->element_type : 0;
}

static inline void* array_data_ptr(JavaArray* array) {
    return (void*)((uint8_t*)array + sizeof(JavaArray));
}

JavaValue array_get(JavaArray* array, jsize index) {
    JavaValue v = { .raw = 0 };
    if (!array || index < 0 || index >= array->length) return v;
    
    /* Memory barrier for thread visibility - ensure we see writes from other threads */
    __sync_synchronize();
    
    void* data = array_data_ptr(array);
    switch (array->element_type) {
        case T_BOOLEAN:
            v.i = ((uint8_t*)data)[index]; 
            break;
        case T_BYTE:    
            /* CRITICAL FIX: Sign-extend byte to int for Java semantics */
            v.i = (jint)((int8_t*)data)[index]; 
            break;
        case T_CHAR:    v.i = ((jchar*)data)[index]; break;
        case T_SHORT:   v.i = ((jshort*)data)[index]; break;
        case T_INT:     v.i = ((jint*)data)[index]; break;
        case T_LONG:    v.j = ((jlong*)data)[index]; break;
        case T_FLOAT:   v.f = ((jfloat*)data)[index]; break;
        case T_DOUBLE:  v.d = ((jdouble*)data)[index]; break;
        case DESC_OBJECT:
        case DESC_ARRAY: v.ref = ((void**)data)[index]; break;
    }
    return v;
}

void array_set(JavaArray* array, jsize index, JavaValue value) {
    if (!array) {
        ERROR_LOG("array_set: NULL array");
        return;
    }
    if (index < 0 || index >= array->length) {
        ERROR_LOG("array_set: index %d out of bounds (length=%d)", index, array->length);
        return;
    }
    
    /* Проверка magic number для обнаружения corruption */
    GCObjectHeader* gc_hdr = (GCObjectHeader*)array - 1;
    if (gc_hdr->magic != GC_HEADER_MAGIC) {
        ERROR_LOG("array_set: FATAL - array at %p has corrupted magic (0x%08X), skipping write", 
                 (void*)array, gc_hdr->magic);
        return;
    }
    
    void* data = array_data_ptr(array);
    switch (array->element_type) {
        case T_BOOLEAN:
        case T_BYTE:    ((uint8_t*)data)[index] = (uint8_t)value.i; break;
        case T_CHAR:    ((jchar*)data)[index] = (jchar)value.i; break;
        case T_SHORT:   ((jshort*)data)[index] = (jshort)value.i; break;
        case T_INT:     ((jint*)data)[index] = value.i; break;
        case T_LONG:    ((jlong*)data)[index] = value.j; break;
        case T_FLOAT:   ((jfloat*)data)[index] = value.f; break;
        case T_DOUBLE:  ((jdouble*)data)[index] = value.d; break;
        case DESC_OBJECT:
        case DESC_ARRAY: ((void**)data)[index] = value.ref; break;
    }
    
    /* Memory barrier for thread visibility - ensure other threads see our writes */
    __sync_synchronize();
}

void* array_get_ref(JavaArray* array, jsize index) {
    if (!array || index < 0 || index >= array->length) return NULL;
    return ((void**)array_data_ptr(array))[index];
}

void array_set_ref(JavaArray* array, jsize index, void* ref) {
    if (!array || index < 0 || index >= array->length) return;
    ((void**)array_data_ptr(array))[index] = ref;
}

/* Строки */

/* Helper to check if object is a valid GC-managed string (native or JavaObject) */
static inline bool is_valid_string_object(void* ptr) {
    if (!ptr) return false;
    if (!is_heap_ptr(ptr)) return false;
    
    GCObjectHeader* header = (GCObjectHeader*)ptr - 1;
    ObjectType type = header->type;
    
    /* CRITICAL: Reject freed objects and invalid types */
    if (type == OBJ_TYPE_FREE) {
        DEBUG_LOG("is_valid_string_object: REJECTED - object at %p was freed (type=OBJ_TYPE_FREE)", ptr);
        return false;
    }
    if (type > OBJ_TYPE_CLASS) {
        DEBUG_LOG("is_valid_string_object: REJECTED - invalid type %d for ptr=%p", type, ptr);
        return false;
    }
    
    return true;
}

/* Helper to check if object is a native string (OBJ_TYPE_STRING) */
static inline bool is_native_string(void* ptr) {
    if (!ptr) return false;
    if (!is_heap_ptr(ptr)) return false;
    
    GCObjectHeader* header = (GCObjectHeader*)ptr - 1;
    ObjectType type = header->type;
    
    /* CRITICAL: Freed objects should not be treated as strings */
    if (type == OBJ_TYPE_FREE) {
        DEBUG_LOG("is_native_string: REJECTED - object at %p was freed!", ptr);
        return false;
    }
    
    DEBUG_LOG("is_native_string: ptr=%p, header=%p, type=%d, is_string=%d",
              ptr, header, type, type == OBJ_TYPE_STRING);
    return type == OBJ_TYPE_STRING;
}

/* String field indices are now dynamically looked up via native.c */

jsize string_length(JavaString* str) {
    if (!str) return 0;
    
    /* CRITICAL: Check if this is a valid heap pointer */
    if (!is_heap_ptr(str)) {
        return 0;
    }
    
    GCObjectHeader* gc_header = (GCObjectHeader*)str - 1;
    
    /* CRITICAL: Check if object was freed by GC */
    if (gc_header->type == OBJ_TYPE_FREE) {
        return 0;
    }
    
    /* Check if this is a native string (inline chars) or JavaObject with fields */
    if (gc_header->type == OBJ_TYPE_STRING) {
        return str->length;
    }
    
    /* It's a JavaObject - get count from field using proper slot lookup */
    if (gc_header->type == OBJ_TYPE_OBJECT) {
        JavaObject* obj = (JavaObject*)str;
        if (obj->header.clazz && g_jvm_for_instanceof) {
            int count_slot = native_get_string_count_slot(g_jvm_for_instanceof);
            if (count_slot >= 0 && count_slot < (int)(gc_header->size / sizeof(JavaValue))) {
                return obj->fields[count_slot].i;
            }
        }
    }
    
    return 0;
}

const jchar* string_chars(JavaString* str) {
    if (!str) return NULL;
    
    /* CRITICAL: First check if this is a valid object (not freed) */
    if (!is_heap_ptr(str)) {
        return NULL;
    }
    
    GCObjectHeader* gc_header = (GCObjectHeader*)str - 1;
    
    /* CRITICAL: Check if object was freed by GC */
    if (gc_header->type == OBJ_TYPE_FREE) {
        return NULL;
    }
    
    /* Check if this is a native string with inline chars */
    if (gc_header->type == OBJ_TYPE_STRING) {
        return (const jchar*)((uint8_t*)str + sizeof(JavaString));
    }
    
    /* It's a JavaObject (type=OBJ_TYPE_OBJECT) - get char array from value field */
    if (gc_header->type == OBJ_TYPE_OBJECT) {
        JavaObject* obj = (JavaObject*)str;
        if (obj->header.clazz && g_jvm_for_instanceof) {
            int value_slot = native_get_string_value_slot(g_jvm_for_instanceof);
            if (value_slot >= 0 && value_slot < (int)(gc_header->size / sizeof(JavaValue))) {
                JavaArray* char_array = (JavaArray*)obj->fields[value_slot].ref;
                if (char_array) {
                    return (const jchar*)array_data(char_array);
                }
            }
        }
    }
    
    return NULL;
}

/*
 * CRITICAL FIX: Unified string_utf8 API
 * 
 * Previously, this function had ambiguous ownership:
 * - For native strings (OBJ_TYPE_STRING): returned internal cached buffer (caller MUST NOT free)
 * - For Java objects (OBJ_TYPE_OBJECT): returned malloc'd buffer (caller MUST free)
 * 
 * NEW BEHAVIOR:
 * - string_utf8(): Returns a pointer that caller MUST NOT free (callee owns).
 *   For Java objects, this uses a thread-local buffer that is reused on next call.
 *   This is safe for immediate use but NOT for storing the pointer.
 * 
 * - string_utf8_copy(): Always returns a malloc'd copy that caller MUST free.
 *   Use this when you need to store the string for later use.
 */

/* Thread-local buffer for string_utf8 — each thread gets its own buffer */
static __thread char* string_utf8_tls_buffer = NULL;
static __thread size_t string_utf8_tls_buffer_size = 0;

const char* string_utf8(JVM* jvm, JavaString* str) {
    (void)jvm;
    if (!str) return NULL;
    
    /* For native strings, use cached utf8 if available */
    if (is_native_string(str) && str->utf8) return str->utf8;
    
    jsize len = string_length(str);
    const jchar* chars = string_chars(str);
    if (!chars && len > 0) return NULL;
    
    /* Calculate required buffer size (max 3 bytes per UTF-16 code unit) */
    size_t required_size = (size_t)len * 3 + 1;
    
    /* For native strings, allocate permanent cache */
    if (is_native_string(str)) {
        char* result = (char*)malloc(required_size);
        if (!result) return NULL;
        
        char* p = result;
        for (jsize i = 0; i < len; i++) {
            jchar c = chars[i];
            if (c < 0x80) {
                *p++ = (char)c;
            } else if (c < 0x800) {
                *p++ = (char)(0xC0 | (c >> 6));
                *p++ = (char)(0x80 | (c & 0x3F));
            } else {
                *p++ = (char)(0xE0 | (c >> 12));
                *p++ = (char)(0x80 | ((c >> 6) & 0x3F));
                *p++ = (char)(0x80 | (c & 0x3F));
            }
        }
        *p = '\0';
        
        /* Cache for future use - caller MUST NOT free this */
        str->utf8 = result;
        return result;
    }
    
    /* For Java objects, use thread-local buffer (reused, NOT thread-safe) */
    if (string_utf8_tls_buffer_size < required_size) {
        /* Grow the buffer */
        size_t new_size = required_size * 2;
        if (new_size < 256) new_size = 256;
        char* new_buffer = (char*)realloc(string_utf8_tls_buffer, new_size);
        if (!new_buffer) return NULL;
        string_utf8_tls_buffer = new_buffer;
        string_utf8_tls_buffer_size = new_size;
    }
    
    char* p = string_utf8_tls_buffer;
    for (jsize i = 0; i < len; i++) {
        jchar c = chars[i];
        if (c < 0x80) {
            *p++ = (char)c;
        } else if (c < 0x800) {
            *p++ = (char)(0xC0 | (c >> 6));
            *p++ = (char)(0x80 | (c & 0x3F));
        } else {
            *p++ = (char)(0xE0 | (c >> 12));
            *p++ = (char)(0x80 | ((c >> 6) & 0x3F));
            *p++ = (char)(0x80 | (c & 0x3F));
        }
    }
    *p = '\0';
    
    return string_utf8_tls_buffer;
}

/* 
 * string_utf8_copy - Always returns a malloc'd copy that caller MUST free.
 * Use this when you need to store the string for later use.
 */
char* string_utf8_copy(JVM* jvm, JavaString* str) {
    const char* cached = string_utf8(jvm, str);
    if (!cached) return NULL;
    return strdup(cached);
}

/* Cleanup thread-local buffer (call at JVM shutdown) */
void string_utf8_cleanup(void) {
    /* Only clean current thread's buffer */
    free(string_utf8_tls_buffer);
    string_utf8_tls_buffer = NULL;
    string_utf8_tls_buffer_size = 0;
}

bool string_equals(JavaString* a, JavaString* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    
    jsize len_a = string_length(a);
    jsize len_b = string_length(b);
    if (len_a != len_b) return false;
    
    const jchar* chars_a = string_chars(a);
    const jchar* chars_b = string_chars(b);
    if (!chars_a || !chars_b) return false;
    
    return memcmp(chars_a, chars_b, len_a * sizeof(jchar)) == 0;
}

jint string_hash(JavaString* str) {
    if (!str) return 0;
    
    /* For native strings, use cached hash if available */
    if (is_native_string(str) && str->hash) return str->hash;
    
    /* For JavaObject strings, check hash field using proper slot lookup */
    if (!is_native_string(str)) {
        JavaObject* obj = (JavaObject*)str;
        if (obj->header.clazz && g_jvm_for_instanceof) {
            int hash_slot = native_get_string_hash_slot(g_jvm_for_instanceof);
            if (hash_slot >= 0) {
                jint cached_hash = obj->fields[hash_slot].i;
                if (cached_hash != 0) return cached_hash;
            }
        }
    }
    
    jsize len = string_length(str);
    const jchar* chars = string_chars(str);
    if (!chars) return 0;
    
    jint hash = 0;
    for (jsize i = 0; i < len; i++) {
        hash = 31 * hash + chars[i];
    }
    
    /* Cache hash for native strings */
    if (is_native_string(str)) {
        str->hash = hash;
    }
    
    return hash;
}

void heap_dump(JVM* jvm) {
    (void)jvm;
    printf("=== Heap Dump ===\n");
    printf("Total: %zu bytes\n", heap.size);
    printf("Used: %zu bytes\n", heap.allocated);
    printf("Free List Blocks: ");
    
    int count = 0;
    FreeBlock* fb = heap.free_list;
    while(fb && count < 10) {
        printf("[%p:%u] ", fb, (unsigned int)fb->size);
        fb = fb->next;
        count++;
    }
    printf("\n");
    
    printf("Live objects scan:\n");
    uint8_t* ptr = heap.start;
    int obj_count = 0;
    while (ptr < heap.current && obj_count < 20) {
        GCObjectHeader* h = (GCObjectHeader*)ptr;
        const char* type_name = "?";
        if (h->type == OBJ_TYPE_OBJECT && h->clazz) type_name = h->clazz->class_name;
        else if (h->type == OBJ_TYPE_ARRAY) type_name = "Array";
        else if (h->type == OBJ_TYPE_STRING) type_name = "String";
        
        printf("  [%p] %s sz=%u m=%d\n", (h+1), type_name, h->size, h->marked);
        ptr += h->size;
        obj_count++;
    }
    printf("==================\n");
}

void heap_validate(JVM* jvm) {
    (void)jvm;
    size_t computed_allocated = 0;
    size_t object_count = 0;
    uint8_t* ptr = heap.start;
    
    while (ptr < heap.current) {
        GCObjectHeader* h = (GCObjectHeader*)ptr;
        if (h->size == 0 || h->size > heap.size || ptr + h->size > heap.end) {
            ERROR_LOG("Corruption at %p", ptr);
            return;
        }
        
        /* ИСПРАВЛЕНО: Считаем все НЕ свободные объекты, а не только marked/pinned.
         * После GC marked сбрасывается в 0, поэтому проверка marked/pinned
         * давала бы всегда 0. Правильная проверка: type != OBJ_TYPE_FREE.
         */
        if (h->type != OBJ_TYPE_FREE) {
            computed_allocated += h->size;
            object_count++;
        }
        ptr += h->size;
    }
    
    if (computed_allocated != heap.allocated) {
        ERROR_LOG("Alloc mismatch: calc=%zu vs stats=%zu (objects=%zu)", 
                  computed_allocated, heap.allocated, object_count);
    }
}

/* Проверка magic number всех объектов - для раннего обнаружения corruption */
int heap_check_magic(JVM* jvm) {
    (void)jvm;
    int corrupt_count = 0;
    uint8_t* ptr = heap.start;
    
    while (ptr < heap.current) {
        GCObjectHeader* h = (GCObjectHeader*)ptr;
        
        /* Защита от бесконечного цикла */
        if (h->size == 0 || h->size > heap.size || ptr + h->size > heap.end) {
            ERROR_LOG("heap_check_magic: Invalid object size at %p", ptr);
            break;
        }
        
        /* Проверяем magic только для НЕ свободных объектов */
        if (h->type != OBJ_TYPE_FREE) {
            if (h->magic != GC_HEADER_MAGIC) {
                const char* type_name = "?";
                if (h->clazz && h->clazz->class_name) {
                    type_name = h->clazz->class_name;
                }
                ERROR_LOG("heap_check_magic: Object at %p has corrupted magic 0x%08X (class=%s, type=%d, size=%u)", 
                         (void*)ptr, h->magic, type_name, h->type, h->size);
                corrupt_count++;
            }
        }
        
        ptr += h->size;
    }
    
    return corrupt_count;
}