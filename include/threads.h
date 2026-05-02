/*
 * J2ME Emulator - Thread Management (Simplified/Single-threaded)
 * All threading operations are stubs - no actual threads created
 */

#ifndef THREADS_H
#define THREADS_H

#include "jvm.h"

/*
 * Thread priorities
 */
#define THREAD_PRIORITY_MIN     1
#define THREAD_PRIORITY_NORM    5
#define THREAD_PRIORITY_MAX     10

/* Yield interval - switch threads after this many opcodes */
#define YIELD_INTERVAL 1000

/*
 * Thread states (matches java.lang.Thread.State)
 */
typedef enum {
    THREAD_STATE_NEW,
    THREAD_STATE_RUNNABLE,
    THREAD_STATE_BLOCKED,
    THREAD_STATE_WAITING,
    THREAD_STATE_TIMED_WAITING,
    THREAD_STATE_TERMINATED
} ThreadState;

/*
 * Monitor (synchronization object) - stub
 */
typedef struct JavaMonitor {
    JavaObject* owner;          /* For compatibility with object monitor protocol */
    JavaThread* owner_thread;   /* Actual thread owner - use this for thread checks */
    jint entry_count;
} JavaMonitor;

/*
 * Thread management functions (all simplified)
 */

/* Initialize thread system */
int threads_init(JVM* jvm);

/* Create new Java thread */
JavaThread* thread_create(JVM* jvm, const char* name, jint priority, 
                          JavaObject* thread_obj);

/* Start a thread */
int thread_start(JVM* jvm, JavaThread* thread);

/* Join a thread */
int thread_join(JVM* jvm, JavaThread* thread);

/* Get current thread */
JavaThread* thread_current(JVM* jvm);

/* Yield to other threads */
void thread_yield(JVM* jvm);

/* Check if it's time to yield based on global instruction counter */
bool thread_should_yield(void);

/* Increment instruction counter and yield if needed */
void thread_tick(JVM* jvm);

/* Get global instruction counter */
uint64_t thread_get_instruction_counter(void);

/* Schedule next thread (cooperative context switch) */
void thread_schedule(JVM* jvm);

/* Check if there are runnable threads */
bool threads_has_runnable(void);

/* Sleep for milliseconds */
int thread_sleep(JVM* jvm, jlong millis);

/* Check if thread is alive */
bool thread_is_alive(JavaThread* thread);

/* Check if any thread terminated with uncaught exception */
bool thread_has_uncaught_exception(void);

/* Get thread state */
ThreadState thread_get_state(JavaThread* thread);

/* Set thread priority */
void thread_set_priority(JavaThread* thread, jint priority);

/* Interrupt a thread */
void thread_interrupt(JavaThread* thread);

/* Check if thread is interrupted */
bool thread_is_interrupted(JavaThread* thread, bool clear_flag);

/* Find thread by thread object */
JavaThread* thread_find_by_object(JavaObject* thread_obj);

/* Destroy a thread */
void thread_destroy(JVM* jvm, JavaThread* thread);

/*
 * Monitor functions (synchronization) - all stubs
 */

int monitor_enter(JVM* jvm, JavaObject* obj);
int monitor_exit(JVM* jvm, JavaObject* obj);
int monitor_wait(JVM* jvm, JavaObject* obj, jlong timeout, bool timed);
int monitor_notify(JVM* jvm, JavaObject* obj);
int monitor_notify_all(JVM* jvm, JavaObject* obj);
JavaMonitor* monitor_get(JVM* jvm, JavaObject* obj);
JavaThread* monitor_get_owner(JavaObject* obj);
jint monitor_get_entry_count(JavaObject* obj);

/*
 * Thread-local storage - simplified
 */
typedef jint ThreadLocalKey;

int thread_local_create(ThreadLocalKey* key);
int thread_local_set(ThreadLocalKey key, void* value);
void* thread_local_get(ThreadLocalKey key);
void thread_local_delete(ThreadLocalKey key);

/*
 * Atomic operations - simplified without actual atomics
 */

bool atomic_cas_int(volatile jint* ptr, jint expected, jint newval);
bool atomic_cas_long(volatile jlong* ptr, jlong expected, jlong newval);
bool atomic_cas_ptr(void* volatile* ptr, void* expected, void* newval);
jint atomic_increment(volatile jint* ptr);
jint atomic_decrement(volatile jint* ptr);
jint atomic_add(volatile jint* ptr, jint value);
void memory_barrier(void);

/*
 * Lock-free data structures
 */

typedef struct LFQueueNode {
    void* data;
    struct LFQueueNode* next;
} LFQueueNode;

typedef struct {
    LFQueueNode* head;
    LFQueueNode* tail;
} LFQueue;

void lfqueue_init(LFQueue* queue);
int lfqueue_push(LFQueue* queue, void* data);
void* lfqueue_pop(LFQueue* queue);
bool lfqueue_empty(LFQueue* queue);
void lfqueue_destroy(LFQueue* queue);

/*
 * Cleanup functions
 */

/* Destroy all monitor resources (pthread mutex/cond) - call on JVM shutdown */
void cleanup_monitors(void);

#endif /* THREADS_H */
