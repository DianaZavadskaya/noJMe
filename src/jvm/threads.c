/*
 * J2ME Emulator - Cooperative Multithreading Implementation
 * User-space thread scheduling with real context switching
 * 
 * Uses ucontext/makecontext/swapcontext for proper thread switching.
 * Each thread has its own stack, allowing true cooperative multitasking.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifdef MEM_FREE
#undef MEM_FREE
#endif
#else
#include <pthread.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdbool.h>

#ifndef _WIN32
#include <ucontext.h>
#include <sched.h>
#endif

#include "threads.h"
#include "jvm.h"
#include "debug.h"
#include "debug_macros.h"

/* 
 * Global instruction counter for time-slicing 
 */
volatile uint64_t g_instruction_counter = 0;

#ifdef _WIN32
/* Windows Thread-Local Storage for current JavaThread */
static DWORD g_win_tls_index = TLS_OUT_OF_INDEXES;
static bool g_win_tls_initialized = false;

static void win_tls_init(void) {
    if (!g_win_tls_initialized) {
        g_win_tls_index = TlsAlloc();
        g_win_tls_initialized = true;
    }
}
#endif

/* Yield interval - switch threads after this many opcodes */
#define YIELD_INTERVAL 1000

/* Forward declarations */
static void memory_barrier_acquire(void);
static void memory_barrier_release(void);

/* Maximum number of cooperative threads */
#define MAX_COOP_THREADS 32

/* Stack size for each thread (64KB) */
#define THREAD_STACK_SIZE (64 * 1024)

/* Thread control block */
typedef struct CoopThread {
    JavaThread* java_thread;
    struct CoopThread* next;
    ThreadState state;
    jlong wake_time_ms;
    int priority;
    
#ifndef _WIN32
    ucontext_t context;       /* Thread context */
    void* stack;              /* Thread stack */
    int context_valid;        /* Whether context is initialized */
#endif
} CoopThread;

/* Forward declaration for thread wrapper */
static void thread_entry_wrapper(void);
static void init_monitors(void);

/* Forward declaration for pthread_set_current_thread (used on Windows too) */
extern void pthread_set_current_thread(JavaThread* thread);

/* Global flag for uncaught exception - checked by main loop */
bool g_has_uncaught_exception = false;

bool thread_has_uncaught_exception(void) {
    return g_has_uncaught_exception;
}

/* Global scheduler state */
static struct {
    CoopThread* threads[MAX_COOP_THREADS];
    int thread_count;
    
    CoopThread* run_queue_head;
    CoopThread* run_queue_tail;
    
    CoopThread* sleep_list;
    
    CoopThread* current;
    JavaThread* main_thread;
    
    JVM* jvm;
    bool initialized;
    
#ifndef _WIN32
    ucontext_t main_context;    /* Main/scheduler context */
    ucontext_t* return_context; /* Context to return to after thread exits */
#endif
    
    /* Entry point for new threads */
    JavaMethod* entry_method;
    JavaObject* entry_obj;
    JavaThread* entry_java_thread;
} scheduler = {0};

/*
 * Thread-local storage to identify if current thread is a pthread
 * Used by monitor functions to determine synchronization method
 */
#ifndef _WIN32
static __thread int g_is_pthread = -1;  /* -1 = not set, 0 = cooperative, 1 = pthread */
#endif

/* Get current time in milliseconds */
static jlong get_time_ms(void) {
#ifdef _WIN32
    return (jlong)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (jlong)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* Initialize thread system */
int threads_init(JVM* jvm) {
    if (scheduler.initialized) return JNI_OK;
    
#ifdef _WIN32
    /* Initialize Windows TLS */
    win_tls_init();
#endif
    
    scheduler.jvm = jvm;
    scheduler.thread_count = 0;
    scheduler.run_queue_head = NULL;
    scheduler.run_queue_tail = NULL;
    scheduler.sleep_list = NULL;
    scheduler.current = NULL;
    scheduler.main_thread = NULL;
    scheduler.initialized = true;
    
    return JNI_OK;
}

/* Check if thread is in run queue */
static bool is_in_run_queue(CoopThread* coop) {
    CoopThread* p = scheduler.run_queue_head;
    while (p) {
        if (p == coop) return true;
        p = p->next;
    }
    return false;
}

/* Add thread to run queue */
static void add_to_run_queue(CoopThread* coop) {
    if (!coop) return;
    
    if (is_in_run_queue(coop)) {
        return;
    }
    
    coop->state = THREAD_STATE_RUNNABLE;
    coop->next = NULL;
    
    if (!scheduler.run_queue_head) {
        scheduler.run_queue_head = coop;
        scheduler.run_queue_tail = coop;
    } else {
        scheduler.run_queue_tail->next = coop;
        scheduler.run_queue_tail = coop;
    }
}

/* Remove thread from run queue head */
static CoopThread* remove_from_run_queue(void) {
    if (!scheduler.run_queue_head) return NULL;
    
    CoopThread* coop = scheduler.run_queue_head;
    scheduler.run_queue_head = coop->next;
    
    if (!scheduler.run_queue_head) {
        scheduler.run_queue_tail = NULL;
    }
    
    coop->next = NULL;
    return coop;
}

/* Remove specific thread from queue */
static bool remove_specific_from_queue(CoopThread* target) {
    if (!target || !scheduler.run_queue_head) return false;
    
    if (scheduler.run_queue_head == target) {
        scheduler.run_queue_head = target->next;
        if (!scheduler.run_queue_head) {
            scheduler.run_queue_tail = NULL;
        } else if (scheduler.run_queue_tail == target) {
            scheduler.run_queue_tail = NULL;
        }
        target->next = NULL;
        return true;
    }
    
    CoopThread* prev = scheduler.run_queue_head;
    CoopThread* curr = prev->next;
    
    while (curr) {
        if (curr == target) {
            prev->next = curr->next;
            if (scheduler.run_queue_tail == target) {
                scheduler.run_queue_tail = prev;
            }
            target->next = NULL;
            return true;
        }
        prev = curr;
        curr = curr->next;
    }
    
    return false;
}

/* Wake up sleeping threads */
static void wakeup_sleeping_threads(void) {
    jlong now = get_time_ms();
    
    while (scheduler.sleep_list && scheduler.sleep_list->wake_time_ms <= now) {
        CoopThread* coop = scheduler.sleep_list;
        scheduler.sleep_list = coop->next;
        coop->next = NULL;
        add_to_run_queue(coop);
    }
}

/* Create new Java thread */
JavaThread* thread_create(JVM* jvm, const char* name, jint priority, JavaObject* thread_obj) {
    (void)jvm;
    
    if (scheduler.thread_count >= MAX_COOP_THREADS) {
        return NULL;
    }
    
    JavaThread* java_thread = (JavaThread*)calloc(1, sizeof(JavaThread));
    if (!java_thread) return NULL;
    
    java_thread->id = scheduler.thread_count + 1;
    java_thread->name = name ? strdup(name) : NULL;
    java_thread->priority = priority;
    java_thread->is_daemon = false;
    java_thread->is_alive = false;
    java_thread->thread_object = thread_obj;
    java_thread->current_frame = NULL;
    java_thread->pending_exception = NULL;
    java_thread->stack_size = JAVA_STACK_SIZE;
    java_thread->exec_jmp_buf = NULL;
    java_thread->exec_jmp_buf_valid = 0;
    
    /* Allocate stack for thread-local allocator */
    java_thread->stack_base = malloc(JAVA_STACK_SIZE);
    if (!java_thread->stack_base) {
        free(java_thread->name);
        free(java_thread);
        return NULL;
    }
    java_thread->stack_used = 0;
    
    java_thread->allocator.heap_size = 64 * 1024;
    java_thread->allocator.heap = malloc(java_thread->allocator.heap_size);
    java_thread->allocator.heap_used = 0;
    
    /* Create CoopThread wrapper */
    CoopThread* coop = (CoopThread*)calloc(1, sizeof(CoopThread));
    if (!coop) {
        free(java_thread->stack_base);
        free(java_thread->name);
        free(java_thread);
        return NULL;
    }
    
    coop->java_thread = java_thread;
    coop->state = THREAD_STATE_NEW;
    coop->priority = priority;
    coop->wake_time_ms = 0;
    coop->next = NULL;
    
#ifndef _WIN32
    /* Allocate stack for context switching */
    coop->stack = malloc(THREAD_STACK_SIZE);
    if (!coop->stack) {
        free(coop);
        free(java_thread->stack_base);
        free(java_thread->name);
        free(java_thread);
        return NULL;
    }
    coop->context_valid = 0;
#endif
    
    scheduler.threads[scheduler.thread_count++] = coop;
    
    /* CRITICAL FIX: Also add to jvm->threads[] so GC can see this thread's frames!
     * Without this, threads created after the main thread are invisible to GC,
     * and objects stored in their local variables will be incorrectly freed.
     */
    if (jvm && jvm->thread_count < MAX_JAVA_THREADS) {
        jvm->threads[jvm->thread_count++] = java_thread;
    }
    
    /* Store main thread reference */
    if (name && strcmp(name, "main") == 0) {
        scheduler.main_thread = java_thread;
        scheduler.current = coop;
        coop->state = THREAD_STATE_RUNNABLE;
        java_thread->is_alive = true;
        
        scheduler.run_queue_head = coop;
        scheduler.run_queue_tail = coop;
        
#ifdef _WIN32
        /* Set TLS for main thread on Windows */
        pthread_set_current_thread(java_thread);
#else
        /* Mark this thread as cooperative (not pthread) */
        g_is_pthread = 0;
#endif
    }
    
    return java_thread;
}

/* Start a thread */
int thread_start(JVM* jvm, JavaThread* thread) {
    (void)jvm;
    if (!thread) return JNI_ERR;
    
    CoopThread* coop = NULL;
    for (int i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i]->java_thread == thread) {
            coop = scheduler.threads[i];
            break;
        }
    }
    
    if (!coop) {
        return JNI_ERR;
    }
    
    if (coop->state != THREAD_STATE_NEW) {
        return JNI_ERR;
    }
    
    thread->is_alive = true;
    add_to_run_queue(coop);
    
    return JNI_OK;
}

#ifndef _WIN32
/* Thread-local storage for current JavaThread in pthread threads */
static __thread JavaThread* tls_current_thread = NULL;

/* Set current thread for pthread */
void pthread_set_current_thread(JavaThread* thread) {
    tls_current_thread = thread;
}
#else
/* Windows TLS-based thread current setting */
void pthread_set_current_thread(JavaThread* thread) {
    win_tls_init();
    if (g_win_tls_index != TLS_OUT_OF_INDEXES) {
        TlsSetValue(g_win_tls_index, thread);
    }
}
#endif

/* Get current thread */
JavaThread* thread_current(JVM* jvm) {
    (void)jvm;
#ifdef _WIN32
    /* Windows: check TLS first for native threads */
    win_tls_init();
    if (g_win_tls_index != TLS_OUT_OF_INDEXES) {
        JavaThread* tls_thread = (JavaThread*)TlsGetValue(g_win_tls_index);
        if (tls_thread) {
            return tls_thread;
        }
    }
    /* Fallback to scheduler.current for cooperative threads */
    return scheduler.current ? scheduler.current->java_thread : scheduler.main_thread;
#else
    /* POSIX: First check TLS for pthread threads */
    if (tls_current_thread) {
        return tls_current_thread;
    }
#endif
    return scheduler.current ? scheduler.current->java_thread : scheduler.main_thread;
}

/* Set current thread */
JavaThread* thread_set_current(JavaThread* thread) {
    JavaThread* prev = scheduler.current ? scheduler.current->java_thread : NULL;
    
    for (int i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i]->java_thread == thread) {
            scheduler.current = scheduler.threads[i];
            return prev;
        }
    }
    
    return prev;
}

/* Schedule next thread - cooperative context switch */
void thread_schedule(JVM* jvm) {
    (void)jvm;
    
    wakeup_sleeping_threads();
    
    /* If current thread is still runnable, put it back in queue */
    if (scheduler.current && 
        scheduler.current->state == THREAD_STATE_RUNNABLE &&
        scheduler.current->java_thread->is_alive) {
        add_to_run_queue(scheduler.current);
    }
    
    /* Get next thread from run queue */
    CoopThread* next = remove_from_run_queue();
    
    /* Skip if it's the same thread and there are others */
    if (next && next == scheduler.current && scheduler.run_queue_head) {
        add_to_run_queue(next);
        next = remove_from_run_queue();
    }
    
    if (next && next != scheduler.current) {
        CoopThread* prev = scheduler.current;
        scheduler.current = next;
        
#ifndef _WIN32
        if (prev && prev->context_valid) {
            /* Switch to next thread */
            /* JMM FIX: Memory barrier before context switch to ensure all writes are visible */
            memory_barrier();
            swapcontext(&prev->context, &next->context);
            /* JMM FIX: Memory barrier after context switch to see all writes from other thread */
            memory_barrier();
        } else if (next->context_valid) {
            /* First time switching from main */
            memory_barrier();
            setcontext(&next->context);
        }
#endif
    }
}

/* Thread entry wrapper - calls run() method */
static void thread_entry_wrapper(void) {
    if (!scheduler.current) return;
    
    JavaThread* java_thread = scheduler.current->java_thread;
    JavaMethod* method = scheduler.entry_method;
    JavaObject* obj = scheduler.entry_obj;
    
    if (method && obj) {
        extern int execute_method(JVM* jvm, JavaThread* thread, JavaMethod* method, 
                                  JavaValue* args, JavaValue* result);
        JavaValue this_arg = { .ref = obj };
        JavaValue result;
        execute_method(scheduler.jvm, java_thread, method, &this_arg, &result);
    }
    
    /* Thread finished - mark as terminated */
    java_thread->is_alive = false;
    scheduler.current->state = THREAD_STATE_TERMINATED;
    
    /* Check for uncaught exception - in J2ME, this should NOT stop the app!
     * Only the thread dies, the rest of the application continues.
     * This matches the behavior of real J2ME implementations.
     */
    if (java_thread->pending_exception) {
        DEBUG_LOG("[THREAD] Thread '%s' terminated with uncaught exception: %s (app continues)",
                java_thread->name ? java_thread->name : "(unnamed)",
                java_thread->pending_exception->header.clazz ? 
                java_thread->pending_exception->header.clazz->class_name : "?");
        /* DO NOT set g_has_uncaught_exception = true - app should continue */
    }
    
    /* Check if any threads are still runnable */
    bool has_runnable = false;
    for (int i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i]->state == THREAD_STATE_RUNNABLE ||
            scheduler.threads[i]->state == THREAD_STATE_WAITING ||
            scheduler.threads[i]->state == THREAD_STATE_TIMED_WAITING) {
            has_runnable = true;
            break;
        }
    }
    
    if (!has_runnable) {
        if (g_j2me_runtime_debug) LOG_SAFE("[THREAD] No runnable threads remaining, stopping JVM\n");
        if (scheduler.jvm) {
            scheduler.jvm->running = false;
        }
    }
    
    /* Switch back to scheduler/main context */
#ifndef _WIN32
    if (scheduler.return_context) {
        setcontext(scheduler.return_context);
    }
#endif
}

/* Execute a thread's run method in its own context */
void thread_execute_run(JVM* jvm, JavaThread* java_thread, JavaMethod* run_method, JavaObject* run_obj) {
#ifndef _WIN32
    CoopThread* coop = NULL;
    for (int i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i]->java_thread == java_thread) {
            coop = scheduler.threads[i];
            break;
        }
    }
    
    if (!coop) return;
    
    /* Set up entry point */
    scheduler.entry_method = run_method;
    scheduler.entry_obj = run_obj;
    scheduler.entry_java_thread = java_thread;
    
    /* Initialize thread context */
    getcontext(&coop->context);
    
    coop->context.uc_stack.ss_sp = coop->stack;
    coop->context.uc_stack.ss_size = THREAD_STACK_SIZE;
    coop->context.uc_link = &scheduler.main_context;  /* Return to main when done */
    
    makecontext(&coop->context, thread_entry_wrapper, 0);
    coop->context_valid = 1;
    
    /* Save current context and switch to new thread */
    scheduler.return_context = &scheduler.main_context;
    
    /* CRITICAL: Set current to the new thread BEFORE swapcontext,
     * so thread_entry_wrapper sees the correct current thread */
    scheduler.current = coop;
    
    swapcontext(&scheduler.main_context, &coop->context);
    
    /* Restore current thread pointer */
    for (int i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i]->java_thread == scheduler.main_thread) {
            scheduler.current = scheduler.threads[i];
            break;
        }
    }
#else
    /* Windows: just execute synchronously */
    extern int execute_method(JVM* jvm, JavaThread* thread, JavaMethod* method, 
                              JavaValue* args, JavaValue* result);
    JavaValue this_arg = { .ref = run_obj };
    JavaValue result;
    execute_method(jvm, java_thread, run_method, &this_arg, &result);
    java_thread->is_alive = false;
    
    /* Check for uncaught exception on Windows - in J2ME, this should NOT stop the app! */
    if (java_thread->pending_exception) {
        DEBUG_LOG("[THREAD] Thread '%s' terminated with uncaught exception: %s (app continues)",
                java_thread->name ? java_thread->name : "(unnamed)",
                java_thread->pending_exception->header.clazz ? 
                java_thread->pending_exception->header.clazz->class_name : "?");
        /* DO NOT set g_has_uncaught_exception = true - app should continue */
    }
    
    /* Check if any threads are still runnable on Windows */
    bool has_runnable = false;
    for (int i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i] && 
            (scheduler.threads[i]->state == THREAD_STATE_RUNNABLE ||
             scheduler.threads[i]->state == THREAD_STATE_WAITING ||
             scheduler.threads[i]->state == THREAD_STATE_TIMED_WAITING)) {
            has_runnable = true;
            break;
        }
    }
    
    if (!has_runnable) {
        if (g_j2me_runtime_debug) LOG_SAFE("[THREAD] No runnable threads remaining, stopping JVM\n");
        jvm->running = false;
    }
#endif
}

/* Yield to other threads */
void thread_yield(JVM* jvm) {
    (void)jvm;
    
#ifdef _WIN32
    /* Windows: use native threads only, no cooperative switching */
    /* Just yield to OS scheduler */
    Sleep(0);
    return;
#else
    /* POSIX: cooperative threading with ucontext */
    if (!scheduler.initialized || !scheduler.current) return;
    
    wakeup_sleeping_threads();
    
    /* Process SDL events */
    extern void sdl_process_events_minimal(void);
    sdl_process_events_minimal();
    
    /* Handle TIMED_WAITING threads - add to sleep list for wakeup */
    if (scheduler.current->state == THREAD_STATE_TIMED_WAITING && 
        scheduler.current->wake_time_ms > 0) {
        /* Add to sleep list sorted by wake time */
        CoopThread* coop = scheduler.current;
        coop->next = NULL;
        
        if (!scheduler.sleep_list || coop->wake_time_ms < scheduler.sleep_list->wake_time_ms) {
            coop->next = scheduler.sleep_list;
            scheduler.sleep_list = coop;
        } else {
            CoopThread* prev = scheduler.sleep_list;
            while (prev->next && prev->next->wake_time_ms <= coop->wake_time_ms) {
                prev = prev->next;
            }
            coop->next = prev->next;
            prev->next = coop;
        }
    }
    
    /* Check if there are other runnable threads */
    bool has_other = false;
    CoopThread* p = scheduler.run_queue_head;
    while (p) {
        if (p != scheduler.current && p->java_thread->is_alive) {
            has_other = true;
            break;
        }
        p = p->next;
    }
    
    if (!has_other) {
        /* No other cooperative threads, but yield to OS scheduler for pthreads */
        /* This is critical for busy-wait patterns where main thread waits for child pthread */
    #ifdef __linux__
        sched_yield();
    #elif defined(__APPLE__)
        pthread_yield_np();
    #elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
        pthread_yield();
    #else
        /* Fallback to nanosleep for minimal yield */
        struct timespec ts = {0, 1};
        nanosleep(&ts, NULL);
    #endif
        return;
    }
    
    /* Save current context */
    if (scheduler.current->context_valid) {
        /* Put current thread back in queue ONLY if it's not blocked */
        if (scheduler.current->java_thread->is_alive && 
            scheduler.current->state != THREAD_STATE_BLOCKED) {
            scheduler.current->state = THREAD_STATE_RUNNABLE;
            add_to_run_queue(scheduler.current);
        }
        
        /* Get next thread */
        CoopThread* next = remove_from_run_queue();
        if (next && next != scheduler.current) {
            /* CRITICAL: Save prev BEFORE updating current, so we save the correct context */
            CoopThread* prev = scheduler.current;
            scheduler.current = next;
            
            /* Main thread's context is in scheduler.main_context, not CoopThread.context */
            ucontext_t* next_ctx = (next->java_thread == scheduler.main_thread) 
                                   ? &scheduler.main_context 
                                   : &next->context;
            
            /* JMM FIX: Memory barrier before context switch */
            memory_barrier();
            swapcontext(&prev->context, next_ctx);
            /* JMM FIX: Memory barrier after context switch */
            memory_barrier();
        }
    } else {
        /* First yield - save context and then switch */
        CoopThread* prev = scheduler.current;
        if (getcontext(&prev->context) == 0) {
            prev->context_valid = 1;
            
            /* Now do the actual context switch */
            /* Put current thread back in queue ONLY if not blocked */
            if (prev->java_thread->is_alive && 
                prev->state != THREAD_STATE_BLOCKED) {
                prev->state = THREAD_STATE_RUNNABLE;
                add_to_run_queue(prev);
            }
            
            /* Get next thread */
            CoopThread* next = remove_from_run_queue();
            if (next && next != prev) {
                scheduler.current = next;
                
                /* Main thread's context is in scheduler.main_context */
                ucontext_t* next_ctx = (next->java_thread == scheduler.main_thread) 
                                       ? &scheduler.main_context 
                                       : &next->context;
                
                /* JMM FIX: Memory barrier before context switch */
                memory_barrier();
                setcontext(next_ctx);
            }
        }
    }
#endif
}

/* Check if it's time to yield */
bool thread_should_yield(void) {
    return (g_instruction_counter % YIELD_INTERVAL) == 0;
}

/* Increment counter and yield if needed */
void thread_tick(JVM* jvm) {
    g_instruction_counter++;
    
    if ((g_instruction_counter % YIELD_INTERVAL) == 0) {
        thread_yield(jvm);
    }
}

/* Get instruction counter */
uint64_t thread_get_instruction_counter(void) {
    return g_instruction_counter;
}

/* Sleep for milliseconds - COOPERATIVE VERSION
 * 
 * CRITICAL FIX: This is now cooperative! Instead of blocking nanosleep,
 * we mark the thread as TIMED_WAITING and yield to other threads.
 * The thread will be woken up after the timeout by the scheduler.
 */
int thread_sleep(JVM* jvm, jlong millis) {
    (void)jvm;
    
    if (millis < 0) millis = 0;
    if (millis > 10000) millis = 10000;
    
    wakeup_sleeping_threads();
    
    extern void sdl_process_events_minimal(void);
    sdl_process_events_minimal();
    
    if (millis > 0) {
        /* COOPERATIVE FIX: Mark current thread as TIMED_WAITING and set wake time */
        if (scheduler.current) {
            CoopThread* coop = scheduler.current;
            coop->state = THREAD_STATE_TIMED_WAITING;
            coop->wake_time_ms = get_time_ms() + millis;
            
            /* Yield to other threads - they can run while we "sleep" */
            thread_yield(jvm);
            
            /* When we return, the sleep time has elapsed */
            coop->state = THREAD_STATE_RUNNABLE;
        } else {
            /* Fallback for non-cooperative context (main thread before scheduler init) */
#ifdef _WIN32
            Sleep((DWORD)millis);
#else
            struct timespec ts = {
                .tv_sec = millis / 1000,
                .tv_nsec = (millis % 1000) * 1000000
            };
            nanosleep(&ts, NULL);
#endif
        }
    }
    
    return JNI_OK;
}

/* Check if thread is alive */
bool thread_is_alive(JavaThread* thread) {
    if (!thread) return false;
    return thread->is_alive;
}

/* Get thread state */
ThreadState thread_get_state(JavaThread* thread) {
    if (!thread) return THREAD_STATE_NEW;
    
    for (int i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i]->java_thread == thread) {
            return scheduler.threads[i]->state;
        }
    }
    
    return THREAD_STATE_TERMINATED;
}

/* Set thread priority */
void thread_set_priority(JavaThread* thread, jint priority) {
    if (!thread) return;
    
    thread->priority = priority;
    
    for (int i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i]->java_thread == thread) {
            scheduler.threads[i]->priority = priority;
            break;
        }
    }
}

/* Check if thread is interrupted */
bool thread_is_interrupted(JavaThread* thread, bool clear_flag) {
    if (!thread) return false;
    bool interrupted = thread->interrupted;
    if (clear_flag) thread->interrupted = false;
    return interrupted;
}

/* Find thread by thread object */
JavaThread* thread_find_by_object(JavaObject* thread_obj) {
    if (!thread_obj) return NULL;
    
    for (int i = 0; i < scheduler.thread_count; i++) {
        JavaThread* jt = scheduler.threads[i]->java_thread;
        if (jt && jt->thread_object == thread_obj) {
            return jt;
        }
    }
    return NULL;
}

/* Destroy a thread */
void thread_destroy(JVM* jvm, JavaThread* thread) {
    (void)jvm;
    if (!thread) return;
    
    for (int i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i]->java_thread == thread) {
            CoopThread* coop = scheduler.threads[i];
            coop->state = THREAD_STATE_TERMINATED;
            thread->is_alive = false;
            
#ifndef _WIN32
            if (coop->stack) {
                free(coop->stack);
                coop->stack = NULL;
            }
#endif
            break;
        }
    }
    
    /* Check if any threads are still runnable */
    bool has_runnable = false;
    for (int i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i] &&
            (scheduler.threads[i]->state == THREAD_STATE_RUNNABLE ||
             scheduler.threads[i]->state == THREAD_STATE_WAITING ||
             scheduler.threads[i]->state == THREAD_STATE_TIMED_WAITING)) {
            has_runnable = true;
            break;
        }
    }
    
    if (!has_runnable && jvm) {
        if (g_j2me_runtime_debug) LOG_SAFE("[THREAD] thread_destroy: No runnable threads, stopping JVM\n");
        jvm->running = false;
    }
    
    free(thread->name);
    free(thread->stack_base);
    if (thread->allocator.heap) {
        free(thread->allocator.heap);
    }
    if (thread->exception_stack_trace) {
        free(thread->exception_stack_trace);
    }
    if (thread->exception_throw_info) {
        free(thread->exception_throw_info);
    }
    free(thread);
}

/* Check if there are runnable threads */
bool threads_has_runnable(void) {
    wakeup_sleeping_threads();
    return scheduler.run_queue_head != NULL;
}

/* Get current CoopThread */
void* thread_current_coop(void) {
    return scheduler.current;
}

/* Thread preparation data for deferred execution */
typedef struct {
    JavaMethod* run_method;
    JavaObject* run_obj;
    JavaThread* java_thread;
    JVM* jvm;
} ThreadRunData;

static ThreadRunData g_thread_run_data[MAX_COOP_THREADS];

/* Prepare a thread for execution (stores run info for later) */
void thread_prepare_run(JVM* jvm, JavaThread* java_thread, 
                        JavaMethod* run_method, JavaObject* run_obj) {
    if (!java_thread) return;
    
    int idx = -1;
    for (int i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i]->java_thread == java_thread) {
            idx = i;
            break;
        }
    }
    
    if (idx < 0 || idx >= MAX_COOP_THREADS) return;
    
    g_thread_run_data[idx].run_method = run_method;
    g_thread_run_data[idx].run_obj = run_obj;
    g_thread_run_data[idx].java_thread = java_thread;
    g_thread_run_data[idx].jvm = jvm;
}

/* Get thread run data */
ThreadRunData* thread_get_run_data(JavaThread* java_thread) {
    for (int i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i]->java_thread == java_thread) {
            return &g_thread_run_data[i];
        }
    }
    return NULL;
}

/*
 * Monitor functions
 * Supports both pthread-based threads and cooperative threads
 */

typedef struct CoopThread CoopThread;

typedef struct {
    JavaObject* owner;
    JavaThread* owner_thread;       /* For cooperative threads */
#ifndef _WIN32
    pthread_t owner_pthread;        /* For pthread threads */
    pthread_mutex_t mutex;          /* Mutex for pthread synchronization */
    pthread_cond_t entry_cond;      /* Condition variable for monitor entry/exit */
    pthread_cond_t wait_cond;       /* Condition variable for wait/notify */
    int wait_count;                 /* Number of pthreads waiting on wait_cond */
    int entry_wait_count;           /* Number of pthreads waiting on entry_cond */
    JavaThread* waiting_thread;     /* Thread currently waiting (for interrupt) */
#else
    /* Windows synchronization primitives */
    CRITICAL_SECTION cs;            /* Critical section for mutex */
    CONDITION_VARIABLE entry_cond;  /* Condition variable for monitor entry/exit */
    CONDITION_VARIABLE wait_cond;   /* Condition variable for wait/notify */
    DWORD owner_thread_id;         /* Owner thread ID (GetCurrentThreadId()) */
    volatile int wait_count;        /* Number of threads waiting on wait_cond */
    volatile int entry_wait_count;  /* Number of threads waiting on entry_cond */
    JavaThread* waiting_thread;     /* Thread currently waiting (for interrupt) */
#endif
    jint entry_count;
    volatile jint spinlock;         /* Fast spinlock for cooperative threads */
    int is_pthread;                 /* True if owned by pthread/native thread */
    CoopThread* wait_list;          /* Wait list for cooperative threads */
} CoopMonitor;

static CoopMonitor g_monitors[256];
static volatile jint g_monitors_initialized = 0;

static void init_monitors(void) {
    if (atomic_cas_int(&g_monitors_initialized, 0, 1)) {
        memset(g_monitors, 0, sizeof(g_monitors));
#ifndef _WIN32
        /* Initialize mutex and condition variables for POSIX */
        for (int i = 0; i < 256; i++) {
            pthread_mutex_init(&g_monitors[i].mutex, NULL);
            pthread_cond_init(&g_monitors[i].entry_cond, NULL);
            pthread_cond_init(&g_monitors[i].wait_cond, NULL);
        }
#else
        /* Initialize critical sections and condition variables for Windows */
        for (int i = 0; i < 256; i++) {
            InitializeCriticalSection(&g_monitors[i].cs);
            InitializeConditionVariable(&g_monitors[i].entry_cond);
            InitializeConditionVariable(&g_monitors[i].wait_cond);
        }
#endif
    }
}

/* Interrupt a thread */
void thread_interrupt(JavaThread* thread) {
    if (!thread) return;
    
    /* Set interrupted flag with memory barrier for cross-thread visibility */
    thread->interrupted = true;
    memory_barrier();  /* Ensure interrupted flag is visible to other threads */
    
    /* Find the CoopThread and wake it if it's waiting */
    for (int i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i]->java_thread == thread) {
            CoopThread* coop = scheduler.threads[i];
            
            /* If thread is waiting or timed waiting, add to run queue */
            if (coop->state == THREAD_STATE_WAITING || 
                coop->state == THREAD_STATE_TIMED_WAITING) {
                coop->state = THREAD_STATE_RUNNABLE;
                add_to_run_queue(coop);
            }
            break;
        }
    }
    
    /* For native threads: find monitor where this thread is waiting and signal it */
    init_monitors();
    for (int i = 0; i < 256; i++) {
        if (g_monitors[i].waiting_thread == thread) {
            /* Signal the wait condition variable to wake up the waiting thread */
#ifndef _WIN32
            pthread_mutex_lock(&g_monitors[i].mutex);
            pthread_cond_signal(&g_monitors[i].wait_cond);
            pthread_mutex_unlock(&g_monitors[i].mutex);
#else
            /* Windows: wake the wait condition variable */
            EnterCriticalSection(&g_monitors[i].cs);
            WakeConditionVariable(&g_monitors[i].wait_cond);
            LeaveCriticalSection(&g_monitors[i].cs);
#endif
            break;
        }
    }
}

static void monitor_spinlock_acquire(CoopMonitor* mon) {
    while (atomic_cas_int(&mon->spinlock, 0, 1) == false) {
#ifdef _WIN32
        YieldProcessor();
#elif defined(__GNUC__)
        __builtin_ia32_pause();
#endif
    }
    memory_barrier_acquire();
}

static void monitor_spinlock_release(CoopMonitor* mon) {
    memory_barrier_release();
    mon->spinlock = 0;
}

static CoopMonitor* get_monitor(JavaObject* obj) {
    init_monitors();
    size_t idx = ((size_t)obj >> 4) & 0xFF;
    return &g_monitors[idx];
}

int monitor_enter(JVM* jvm, JavaObject* obj) {
    (void)jvm;
    if (!obj) return JNI_ERR;
    
    CoopMonitor* mon = get_monitor(obj);
    
#ifndef _WIN32
    pthread_t self = pthread_self();
    
    /* Always use pthread mutex for synchronization */
    pthread_mutex_lock(&mon->mutex);
    
    /* Check if we already own it (reentrant) */
    if (mon->is_pthread && pthread_equal(mon->owner_pthread, self)) {
        mon->entry_count++;
        pthread_mutex_unlock(&mon->mutex);
        return JNI_OK;
    }
    
    /* Wait until monitor is free - use entry_cond for monitor entry contention */
    mon->entry_wait_count++;
    while (mon->entry_count > 0) {
        pthread_cond_wait(&mon->entry_cond, &mon->mutex);
    }
    mon->entry_wait_count--;
    
    /* Now we own the monitor */
    mon->owner = obj;
    mon->owner_pthread = self;
    mon->is_pthread = 1;
    mon->entry_count = 1;
    
    pthread_mutex_unlock(&mon->mutex);
    return JNI_OK;
#else
    /* Windows: use CRITICAL_SECTION for native threads */
    EnterCriticalSection(&mon->cs);
    
    DWORD self_id = GetCurrentThreadId();
    
    /* Check if we already own it (reentrant) */
    if (mon->is_pthread && mon->owner_thread_id == self_id) {
        mon->entry_count++;
        LeaveCriticalSection(&mon->cs);
        return JNI_OK;
    }
    
    /* Wait until monitor is free - use entry_cond for monitor entry contention */
    mon->entry_wait_count++;
    while (mon->entry_count > 0) {
        /* Use entry_cond for monitor entry, NOT wait_cond */
        SleepConditionVariableCS(&mon->entry_cond, &mon->cs, INFINITE);
    }
    mon->entry_wait_count--;
    
    /* Now we own the monitor */
    mon->owner = obj;
    mon->owner_thread_id = self_id;
    mon->is_pthread = 1;
    mon->entry_count = 1;
    
    LeaveCriticalSection(&mon->cs);
    return JNI_OK;
#endif
}

int monitor_exit(JVM* jvm, JavaObject* obj) {
    (void)jvm;
    if (!obj) return JNI_ERR;
    
    CoopMonitor* mon = get_monitor(obj);
    
#ifndef _WIN32
    pthread_t self = pthread_self();
    
    pthread_mutex_lock(&mon->mutex);
    
    /* Verify we own the monitor */
    if (!mon->is_pthread || !pthread_equal(mon->owner_pthread, self)) {
        pthread_mutex_unlock(&mon->mutex);
        return JNI_ERR;
    }
    
    mon->entry_count--;
    if (mon->entry_count == 0) {
        /* Release the monitor */
        mon->owner = NULL;
        mon->owner_pthread = (pthread_t)0;
        mon->is_pthread = 0;
        
        /* Wake up one thread waiting to enter the monitor - use entry_cond */
        if (mon->entry_wait_count > 0) {
            pthread_cond_signal(&mon->entry_cond);
        }
    }
    
    pthread_mutex_unlock(&mon->mutex);
    return JNI_OK;
#else
    /* Windows: use CRITICAL_SECTION for native threads */
    DWORD self_id = GetCurrentThreadId();
    
    EnterCriticalSection(&mon->cs);
    
    /* Verify we own the monitor */
    if (!mon->is_pthread || mon->owner_thread_id != self_id) {
        LeaveCriticalSection(&mon->cs);
        return JNI_ERR;
    }
    
    mon->entry_count--;
    if (mon->entry_count == 0) {
        /* Release the monitor */
        mon->owner = NULL;
        mon->owner_thread_id = 0;
        mon->is_pthread = 0;
        
        /* Wake up one thread waiting to enter the monitor - use entry_cond */
        if (mon->entry_wait_count > 0) {
            WakeConditionVariable(&mon->entry_cond);
        }
    }
    
    LeaveCriticalSection(&mon->cs);
    return JNI_OK;
#endif
}

int monitor_wait(JVM* jvm, JavaObject* obj, jlong timeout, bool timed) {
    (void)jvm;
    if (!obj) return JNI_ERR;
    
    CoopMonitor* mon = get_monitor(obj);
    
#ifndef _WIN32
    /* Pthread-based wait for Linux/Unix */
    pthread_t self = pthread_self();
    
    pthread_mutex_lock(&mon->mutex);
    
    /* Verify we own the monitor */
    if (!mon->is_pthread || !pthread_equal(mon->owner_pthread, self)) {
        pthread_mutex_unlock(&mon->mutex);
        return JNI_ERR;
    }
    
    /* Get current JavaThread for interrupt checking */
    extern JavaThread* thread_current(JVM* jvm);
    JavaThread* current_java_thread = thread_current(jvm);
    
    /* Check if already interrupted */
    if (current_java_thread && current_java_thread->interrupted) {
        /* Don't clear the flag - let caller handle it */
        pthread_mutex_unlock(&mon->mutex);
        return JNI_ERR;  /* Caller should throw InterruptedException */
    }
    
    /* Save entry count and release monitor */
    jint saved_count = mon->entry_count;
    mon->owner = NULL;
    mon->owner_pthread = (pthread_t)0;
    mon->is_pthread = 0;
    mon->entry_count = 0;
    mon->wait_count++;
    
    /* Signal entry_cond so threads waiting to enter the monitor can proceed */
    if (mon->entry_wait_count > 0) {
        pthread_cond_signal(&mon->entry_cond);
    }
    
    /* Store waiting thread for interrupt support */
    mon->waiting_thread = current_java_thread;
    
    /* Wait on condition variable - use timed wait with periodic interrupt checks */
    int wait_result = 0;
    jlong remaining = timeout;
    jlong check_interval = 50;  /* Check for interrupt every 50ms */
    
    while (1) {
        if (timed && timeout > 0) {
            jlong wait_time = (remaining < check_interval) ? remaining : check_interval;
            if (wait_time <= 0) {
                wait_result = ETIMEDOUT;
                break;
            }
            
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += wait_time / 1000;
            ts.tv_nsec += (wait_time % 1000) * 1000000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            wait_result = pthread_cond_timedwait(&mon->wait_cond, &mon->mutex, &ts);
            
            /* ALWAYS check for interrupt after waking */
            if (current_java_thread && current_java_thread->interrupted) {
                mon->waiting_thread = NULL;
                mon->wait_count--;
                
                /* Re-acquire monitor */
                while (mon->entry_count > 0) {
                    struct timespec ts2;
                    clock_gettime(CLOCK_REALTIME, &ts2);
                    ts2.tv_nsec += 10000000;  /* 10ms */
                    if (ts2.tv_nsec >= 1000000000) {
                        ts2.tv_sec++;
                        ts2.tv_nsec -= 1000000000;
                    }
                    pthread_cond_timedwait(&mon->entry_cond, &mon->mutex, &ts2);
                }
                
                mon->owner = obj;
                mon->owner_pthread = self;
                mon->is_pthread = 1;
                mon->entry_count = saved_count;
                pthread_mutex_unlock(&mon->mutex);
                
                return JNI_ERR;  /* Interrupted */
            }
            
            if (wait_result == ETIMEDOUT) {
                remaining -= wait_time;
                if (remaining <= 0) {
                    break;  /* Full timeout elapsed */
                }
                /* Continue waiting */
                continue;
            }
            
            /* Got a real signal - break out */
            if (wait_result == 0) {
                break;
            }
        } else {
            /* Non-timed wait - use timed wait with interrupt checks */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += check_interval * 1000000;  /* 50ms */
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            wait_result = pthread_cond_timedwait(&mon->wait_cond, &mon->mutex, &ts);
            
            /* Check for interrupt */
            if (current_java_thread && current_java_thread->interrupted) {
                /* Don't clear interrupted flag - let caller handle it */
                mon->waiting_thread = NULL;
                mon->wait_count--;
                
                /* Re-acquire monitor */
                while (mon->entry_count > 0) {
                    struct timespec ts2;
                    clock_gettime(CLOCK_REALTIME, &ts2);
                    ts2.tv_nsec += 10000000;  /* 10ms */
                    if (ts2.tv_nsec >= 1000000000) {
                        ts2.tv_sec++;
                        ts2.tv_nsec -= 1000000000;
                    }
                    pthread_cond_timedwait(&mon->entry_cond, &mon->mutex, &ts2);
                }
                
                mon->owner = obj;
                mon->owner_pthread = self;
                mon->is_pthread = 1;
                mon->entry_count = saved_count;
                pthread_mutex_unlock(&mon->mutex);
                
                return JNI_ERR;  /* Interrupted */
            }
            
            /* If we got a real signal (not timeout), we're done */
            if (wait_result == 0) {
                break;
            }
            /* ETIMEDOUT means we just need to check again */
            continue;
        }
        break;
    }
    
    mon->waiting_thread = NULL;
    mon->wait_count--;
    
    /* Re-acquire monitor */
    while (mon->entry_count > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 10000000;  /* 10ms */
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&mon->entry_cond, &mon->mutex, &ts);
    }
    
    mon->owner = obj;
    mon->owner_pthread = self;
    mon->is_pthread = 1;
    mon->entry_count = saved_count;
    
    pthread_mutex_unlock(&mon->mutex);
    
    return JNI_OK;
#else
    /* Windows: use CRITICAL_SECTION and CONDITION_VARIABLE for native threads */
    DWORD self_id = GetCurrentThreadId();
    
    EnterCriticalSection(&mon->cs);
    
    /* Verify we own the monitor */
    if (!mon->is_pthread || mon->owner_thread_id != self_id) {
        LeaveCriticalSection(&mon->cs);
        return JNI_ERR;
    }
    
    /* Get current JavaThread for interrupt checking */
    extern JavaThread* thread_current(JVM* jvm);
    JavaThread* current_java_thread = thread_current(jvm);
    
    /* Find CoopThread for state updates */
    CoopThread* current_coop = NULL;
    for (int i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i]->java_thread == current_java_thread) {
            current_coop = scheduler.threads[i];
            break;
        }
    }
    
    /* Check if already interrupted */
    if (current_java_thread && current_java_thread->interrupted) {
        LeaveCriticalSection(&mon->cs);
        return JNI_ERR;  /* Caller should throw InterruptedException */
    }
    
    /* Update thread state to WAITING or TIMED_WAITING */
    if (current_coop) {
        current_coop->state = timed ? THREAD_STATE_TIMED_WAITING : THREAD_STATE_WAITING;
    }
    
    /* Save entry count and release monitor */
    jint saved_count = mon->entry_count;
    mon->owner = NULL;
    mon->owner_thread_id = 0;
    mon->is_pthread = 0;
    mon->entry_count = 0;
    mon->wait_count++;
    
    /* Signal entry_cond so threads waiting to enter the monitor can proceed */
    if (mon->entry_wait_count > 0) {
        WakeConditionVariable(&mon->entry_cond);
    }
    
    /* Store waiting thread for interrupt support */
    mon->waiting_thread = current_java_thread;
    
    /* Wait on condition variable - use timed wait with periodic interrupt checks */
    BOOL wait_result = TRUE;
    jlong check_interval = 50;  /* Check for interrupt every 50ms */
    DWORD timeout_ms = INFINITE;
    
    /* Track elapsed time for timed waits */
    jlong start_time = 0;
    if (timed && timeout > 0) {
        start_time = (jlong)GetTickCount64();
    }
    
    while (1) {
        /* Check for interrupt at the start of each iteration */
        if (current_java_thread && current_java_thread->interrupted) {
            mon->waiting_thread = NULL;
            mon->wait_count--;
            
            /* Re-acquire monitor - use entry_cond */
            while (mon->entry_count > 0) {
                SleepConditionVariableCS(&mon->entry_cond, &mon->cs, 10);
            }
            
            mon->owner = obj;
            mon->owner_thread_id = self_id;
            mon->is_pthread = 1;
            mon->entry_count = saved_count;
            LeaveCriticalSection(&mon->cs);
            
            return JNI_ERR;  /* Interrupted */
        }
        
        if (timed && timeout > 0) {
            /* Calculate remaining time */
            jlong elapsed = (jlong)GetTickCount64() - start_time;
            jlong remaining = timeout - elapsed;
            
            if (remaining <= 0) {
                /* Timeout has elapsed */
                wait_result = FALSE;
                break;
            }
            
            /* Wait for the shorter of remaining time or check interval */
            jlong wait_time = (remaining < check_interval) ? remaining : check_interval;
            timeout_ms = (DWORD)wait_time;
            
            wait_result = SleepConditionVariableCS(&mon->wait_cond, &mon->cs, timeout_ms);
            
            /* If we got a signal (not timeout), check if it's a real notify or spurious */
            if (wait_result) {
                /* Got a signal - could be notify or spurious wakeup */
                /* For timed wait, we break on signal (notify was called) */
                break;
            }
            
            /* Timeout or error - continue loop to check elapsed time */
            continue;
        } else {
            /* Non-timed wait - use timed wait with interrupt checks */
            timeout_ms = (DWORD)check_interval;
            wait_result = SleepConditionVariableCS(&mon->wait_cond, &mon->cs, timeout_ms);
            
            /* If we got a real signal (not timeout), we're done */
            if (wait_result) {
                break;
            }
            /* ERROR_TIMEOUT means we just need to check again for interrupt */
            continue;
        }
    }
    
    mon->waiting_thread = NULL;
    mon->wait_count--;
    
    /* Re-acquire monitor - use entry_cond */
    while (mon->entry_count > 0) {
        SleepConditionVariableCS(&mon->entry_cond, &mon->cs, 10);
    }
    
    mon->owner = obj;
    mon->owner_thread_id = self_id;
    mon->is_pthread = 1;
    mon->entry_count = saved_count;
    
    /* Restore thread state to RUNNABLE */
    if (current_coop) {
        current_coop->state = THREAD_STATE_RUNNABLE;
    }
    
    LeaveCriticalSection(&mon->cs);
    
    /* CRITICAL: Check if we were interrupted after breaking out of wait loop.
     * The interrupt may have arrived after we broke out but before we returned.
     * This ensures InterruptedException is thrown when interrupted.
     */
    if (current_java_thread && current_java_thread->interrupted) {
        return JNI_ERR;
    }
    
    return JNI_OK;
#endif
}

int monitor_notify(JVM* jvm, JavaObject* obj) {
    (void)jvm;
    if (!obj) return JNI_ERR;
    
    CoopMonitor* mon = get_monitor(obj);
    
#ifndef _WIN32
    /* Pthread-based notify for Linux/Unix */
    pthread_t self = pthread_self();
    
    pthread_mutex_lock(&mon->mutex);
    
    /* Verify we own the monitor */
    if (!mon->is_pthread || !pthread_equal(mon->owner_pthread, self)) {
        pthread_mutex_unlock(&mon->mutex);
        return JNI_ERR;
    }
    
    /* Signal one waiting thread - use wait_cond */
    if (mon->wait_count > 0) {
        pthread_cond_signal(&mon->wait_cond);
    }
    
    pthread_mutex_unlock(&mon->mutex);
    return JNI_OK;
#else
    /* Windows: use CRITICAL_SECTION for native threads */
    DWORD self_id = GetCurrentThreadId();
    
    EnterCriticalSection(&mon->cs);
    
    /* Verify we own the monitor */
    if (!mon->is_pthread || mon->owner_thread_id != self_id) {
        LeaveCriticalSection(&mon->cs);
        return JNI_ERR;
    }
    
    /* Signal one waiting thread - use wait_cond */
    if (mon->wait_count > 0) {
        WakeConditionVariable(&mon->wait_cond);
    }
    
    LeaveCriticalSection(&mon->cs);
    return JNI_OK;
#endif
}

int monitor_notify_all(JVM* jvm, JavaObject* obj) {
    (void)jvm;
    if (!obj) return JNI_ERR;
    
    CoopMonitor* mon = get_monitor(obj);
    
#ifndef _WIN32
    /* Pthread-based notify all for Linux/Unix */
    pthread_t self = pthread_self();
    
    pthread_mutex_lock(&mon->mutex);
    
    /* Verify we own the monitor */
    if (!mon->is_pthread || !pthread_equal(mon->owner_pthread, self)) {
        pthread_mutex_unlock(&mon->mutex);
        return JNI_ERR;
    }
    
    /* Signal all waiting threads - use wait_cond */
    if (mon->wait_count > 0) {
        pthread_cond_broadcast(&mon->wait_cond);
    }
    
    pthread_mutex_unlock(&mon->mutex);
    return JNI_OK;
#else
    /* Windows: use CRITICAL_SECTION for native threads */
    DWORD self_id = GetCurrentThreadId();
    
    EnterCriticalSection(&mon->cs);
    
    /* Verify we own the monitor */
    if (!mon->is_pthread || mon->owner_thread_id != self_id) {
        LeaveCriticalSection(&mon->cs);
        return JNI_ERR;
    }
    
    /* Signal all waiting threads - use wait_cond */
    if (mon->wait_count > 0) {
        WakeAllConditionVariable(&mon->wait_cond);
    }
    
    LeaveCriticalSection(&mon->cs);
    return JNI_OK;
#endif
}

JavaMonitor* monitor_get(JVM* jvm, JavaObject* obj) {
    (void)jvm; (void)obj;
    return NULL;
}

JavaThread* monitor_get_owner(JavaObject* obj) {
    if (!obj) return NULL;
    CoopMonitor* mon = get_monitor(obj);
#ifndef _WIN32
    pthread_mutex_lock(&mon->mutex);
    JavaThread* owner = mon->is_pthread ? NULL : mon->owner_thread;
    pthread_mutex_unlock(&mon->mutex);
#else
    EnterCriticalSection(&mon->cs);
    JavaThread* owner = mon->is_pthread ? NULL : mon->owner_thread;
    LeaveCriticalSection(&mon->cs);
#endif
    return owner;
}

jint monitor_get_entry_count(JavaObject* obj) {
    if (!obj) return 0;
    CoopMonitor* mon = get_monitor(obj);
    monitor_spinlock_acquire(mon);
    jint count = mon->entry_count;
    monitor_spinlock_release(mon);
    return count;
}

/*
 * Thread-local storage
 */
int thread_local_create(ThreadLocalKey* key) {
#ifdef _WIN32
    DWORD idx = TlsAlloc();
    if (idx == TLS_OUT_OF_INDEXES) {
        return JNI_ERR;
    }
    *key = (ThreadLocalKey)idx;
#else
    static int next_key = 0;
    *key = ++next_key;
#endif
    return JNI_OK;
}

int thread_local_set(ThreadLocalKey key, void* value) {
#ifdef _WIN32
    TlsSetValue((DWORD)key, value);
#else
    (void)key; (void)value;
    /* For POSIX, use pthread_setspecific if needed */
#endif
    return JNI_OK;
}

void* thread_local_get(ThreadLocalKey key) {
#ifdef _WIN32
    return TlsGetValue((DWORD)key);
#else
    (void)key;
    return NULL;
#endif
}

void thread_local_delete(ThreadLocalKey key) {
#ifdef _WIN32
    TlsFree((DWORD)key);
#else
    (void)key;
#endif
}

/*
 * Atomic operations
 */
void memory_barrier(void) {
#ifdef _WIN32
    MemoryBarrier();
#elif defined(__GNUC__)
    __sync_synchronize();
#endif
}

void memory_barrier_acquire(void) {
    memory_barrier();
}

void memory_barrier_release(void) {
    memory_barrier();
}

bool atomic_cas_int(volatile jint* ptr, jint expected, jint newval) {
#ifdef _WIN32
    return InterlockedCompareExchange((LONG volatile*)ptr, newval, expected) == expected;
#elif defined(__GNUC__)
    return __sync_bool_compare_and_swap(ptr, expected, newval);
#else
    if (*ptr == expected) {
        *ptr = newval;
        return true;
    }
    return false;
#endif
}

bool atomic_cas_long(volatile jlong* ptr, jlong expected, jlong newval) {
#ifdef _WIN32
    return InterlockedCompareExchange64((LONG64 volatile*)ptr, newval, expected) == expected;
#elif defined(__GNUC__)
    return __sync_bool_compare_and_swap(ptr, expected, newval);
#else
    if (*ptr == expected) {
        *ptr = newval;
        return true;
    }
    return false;
#endif
}

bool atomic_cas_ptr(void* volatile* ptr, void* expected, void* newval) {
#ifdef _WIN32
    return InterlockedCompareExchangePointer(ptr, newval, expected) == expected;
#elif defined(__GNUC__)
    return __sync_bool_compare_and_swap(ptr, expected, newval);
#else
    if (*ptr == expected) {
        *ptr = newval;
        return true;
    }
    return false;
#endif
}

jint atomic_increment(volatile jint* ptr) {
#ifdef _WIN32
    return InterlockedIncrement((LONG volatile*)ptr);
#elif defined(__GNUC__)
    return __sync_add_and_fetch(ptr, 1);
#else
    return ++(*ptr);
#endif
}

jint atomic_decrement(volatile jint* ptr) {
#ifdef _WIN32
    return InterlockedDecrement((LONG volatile*)ptr);
#elif defined(__GNUC__)
    return __sync_sub_and_fetch(ptr, 1);
#else
    return --(*ptr);
#endif
}

jint atomic_add(volatile jint* ptr, jint value) {
#ifdef _WIN32
    return InterlockedAdd((LONG volatile*)ptr, value);
#elif defined(__GNUC__)
    return __sync_add_and_fetch(ptr, value);
#else
    *ptr += value;
    return *ptr;
#endif
}

/*
 * Lock-free queue
 */
void lfqueue_init(LFQueue* queue) {
    if (queue) {
        queue->head = NULL;
        queue->tail = NULL;
    }
}

int lfqueue_push(LFQueue* queue, void* data) {
    if (!queue) return -1;
    
    LFQueueNode* node = (LFQueueNode*)malloc(sizeof(LFQueueNode));
    if (!node) return -1;
    
    node->data = data;
    node->next = NULL;
    
    if (queue->tail) {
        queue->tail->next = node;
    } else {
        queue->head = node;
    }
    queue->tail = node;
    
    return 0;
}

void* lfqueue_pop(LFQueue* queue) {
    if (!queue || !queue->head) return NULL;
    
    LFQueueNode* node = queue->head;
    void* data = node->data;
    queue->head = node->next;
    
    if (!queue->head) {
        queue->tail = NULL;
    }
    
    free(node);
    return data;
}

bool lfqueue_empty(LFQueue* queue) {
    return !queue || queue->head == NULL;
}

void lfqueue_destroy(LFQueue* queue) {
    if (!queue) return;
    
    while (queue->head) {
        LFQueueNode* node = queue->head;
        queue->head = node->next;
        free(node);
    }
    queue->tail = NULL;
}

/*
 * Cleanup all monitor resources on JVM shutdown
 * This prevents resource leaks with pthread mutex and condition variables
 */
void cleanup_monitors(void) {
    if (!g_monitors_initialized) return;
    
#ifndef _WIN32
    /* Destroy pthread mutex and condition variables */
    for (int i = 0; i < 256; i++) {
        pthread_mutex_destroy(&g_monitors[i].mutex);
        pthread_cond_destroy(&g_monitors[i].entry_cond);
        pthread_cond_destroy(&g_monitors[i].wait_cond);
    }
#else
    /* Windows: CRITICAL_SECTION and CONDITION_VARIABLE don't need explicit destroy */
    /* They are cleaned up automatically when the process exits */
#endif
    
    g_monitors_initialized = 0;
}