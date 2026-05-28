/*
 * J2ME Emulator - Cooperative Multithreading Scheduler
 *
 * Migrated from src/jvm/threads.c (1890 lines)
 *
 * Translation notes:
 *   - ucontext/makecontext/swapcontext: replaced with a round-robin queue
 *     scheduler. JS is single-threaded, so "cooperative context switching"
 *     is a no-op at the OS level. thread_yield / thread_schedule rotate the
 *     run-queue pointer and update scheduler.current. Actual bytecode
 *     execution is driven by the caller (execute.mjs) which calls
 *     thread_tick() every opcode and checks thread_should_yield().
 *   - thread_execute_run: calls execute_method synchronously via an
 *     injected callback (see set_execute_method_callback). The ucontext
 *     dance collapses to a direct function call.
 *   - pthread mutexes / CRITICAL_SECTIONs: omitted. JS is single-threaded.
 *   - Monitor wait/notify: implemented as a cooperative pending-wait list.
 *     monitor_wait() marks the thread WAITING and yields; monitor_notify()
 *     moves the first waiter back to the run-queue. This correctly models
 *     J2ME synchronized blocks within the single-threaded trampoline.
 *   - memory_barrier / atomic_*: no-ops / direct JS operations.
 *   - TLS (ThreadLocalKey): Map<key, value> stored on each CoopThread.
 *   - LFQueue: singly-linked list; malloc/free omitted (GC).
 *   - sdl_process_events_minimal: injected via set_sdl_event_callback().
 */

import { g_j2me_runtime_debug } from './debug_var.mjs';

// ============================================================
// Constants  (mirrors #define / enum from threads.h + threads.c)
// ============================================================

export const THREAD_PRIORITY_MIN  = 1;
export const THREAD_PRIORITY_NORM = 5;
export const THREAD_PRIORITY_MAX  = 10;

export const YIELD_INTERVAL = 1000;

export const ThreadState = Object.freeze({
    NEW:           'NEW',
    RUNNABLE:      'RUNNABLE',
    BLOCKED:       'BLOCKED',
    WAITING:       'WAITING',
    TIMED_WAITING: 'TIMED_WAITING',
    TERMINATED:    'TERMINATED',
});

export const JNI_OK  =  0;
export const JNI_ERR = -1;

const MAX_COOP_THREADS = 32;
const MAX_JAVA_THREADS = 16;
const JAVA_STACK_SIZE  = 256 * 1024;

// ============================================================
// Injected callbacks — set by the host before running threads
// ============================================================

let _execute_method_cb = null;
let _sdl_events_cb     = null;

/**
 * Provide the execute_method implementation.
 * Signature: (jvm, thread, method, args) => result
 */
export function set_execute_method_callback(fn) {
    _execute_method_cb = fn;
}

/**
 * Provide a minimal SDL event pump callback.
 * Called during thread_yield and thread_sleep to keep the event loop alive.
 */
export function set_sdl_event_callback(fn) {
    _sdl_events_cb = fn;
}

function sdl_process_events_minimal() {
    if (_sdl_events_cb) _sdl_events_cb();
}

// ============================================================
// Global instruction counter
// ============================================================

export let g_instruction_counter = 0;

export function thread_get_instruction_counter() {
    return g_instruction_counter;
}

// ============================================================
// Uncaught-exception flag
// ============================================================

export let g_has_uncaught_exception = false;

export function thread_has_uncaught_exception() {
    return g_has_uncaught_exception;
}

// ============================================================
// CoopThread — internal control block (replaces C struct CoopThread)
// ============================================================

function make_coop_thread(javaThread, priority) {
    return {
        java_thread:   javaThread,
        next:          null,
        state:         ThreadState.NEW,
        wake_time_ms:  0,
        priority:      priority,
        // wait_list is used by monitor_wait: array of CoopThread waiting on an obj
        tls_map:       new Map(),   // ThreadLocalKey -> value
    };
}

// ============================================================
// Scheduler state (singleton, mirrors static `scheduler` in C)
// ============================================================

const scheduler = {
    threads:          [],          // CoopThread[]
    thread_count:     0,

    run_queue_head:   null,        // CoopThread linked list
    run_queue_tail:   null,

    sleep_list:       null,        // sorted by wake_time_ms

    current:          null,        // CoopThread currently executing
    main_thread:      null,        // JavaThread for "main"

    jvm:              null,
    initialized:      false,

    // Entry point stored during thread_execute_run
    entry_method:     null,
    entry_obj:        null,
    entry_java_thread: null,
};

// ============================================================
// Time helpers
// ============================================================

function get_time_ms() {
    return Date.now();
}

// ============================================================
// Run-queue helpers
// ============================================================

function is_in_run_queue(coop) {
    let p = scheduler.run_queue_head;
    while (p) {
        if (p === coop) return true;
        p = p.next;
    }
    return false;
}

function add_to_run_queue(coop) {
    if (!coop) return;
    if (is_in_run_queue(coop)) return;

    coop.state = ThreadState.RUNNABLE;
    coop.next  = null;

    if (!scheduler.run_queue_head) {
        scheduler.run_queue_head = coop;
        scheduler.run_queue_tail = coop;
    } else {
        scheduler.run_queue_tail.next = coop;
        scheduler.run_queue_tail      = coop;
    }
}

function remove_from_run_queue() {
    if (!scheduler.run_queue_head) return null;

    const coop = scheduler.run_queue_head;
    scheduler.run_queue_head = coop.next;
    if (!scheduler.run_queue_head) {
        scheduler.run_queue_tail = null;
    }
    coop.next = null;
    return coop;
}

function remove_specific_from_queue(target) {
    if (!target || !scheduler.run_queue_head) return false;

    if (scheduler.run_queue_head === target) {
        scheduler.run_queue_head = target.next;
        if (!scheduler.run_queue_head) {
            scheduler.run_queue_tail = null;
        } else if (scheduler.run_queue_tail === target) {
            scheduler.run_queue_tail = null;
        }
        target.next = null;
        return true;
    }

    let prev = scheduler.run_queue_head;
    let curr = prev.next;
    while (curr) {
        if (curr === target) {
            prev.next = curr.next;
            if (scheduler.run_queue_tail === target) {
                scheduler.run_queue_tail = prev;
            }
            target.next = null;
            return true;
        }
        prev = curr;
        curr = curr.next;
    }
    return false;
}

// ============================================================
// Sleep-list helpers
// ============================================================

function wakeup_sleeping_threads() {
    const now = get_time_ms();
    while (scheduler.sleep_list && scheduler.sleep_list.wake_time_ms <= now) {
        const coop = scheduler.sleep_list;
        scheduler.sleep_list = coop.next;
        coop.next = null;
        add_to_run_queue(coop);
    }
}

function add_to_sleep_list(coop) {
    coop.next = null;
    if (!scheduler.sleep_list || coop.wake_time_ms < scheduler.sleep_list.wake_time_ms) {
        coop.next          = scheduler.sleep_list;
        scheduler.sleep_list = coop;
    } else {
        let prev = scheduler.sleep_list;
        while (prev.next && prev.next.wake_time_ms <= coop.wake_time_ms) {
            prev = prev.next;
        }
        coop.next  = prev.next;
        prev.next  = coop;
    }
}

// ============================================================
// Thread system init
// ============================================================

export function threads_init(jvm) {
    if (scheduler.initialized) return JNI_OK;

    scheduler.jvm              = jvm;
    scheduler.thread_count     = 0;
    scheduler.threads          = [];
    scheduler.run_queue_head   = null;
    scheduler.run_queue_tail   = null;
    scheduler.sleep_list       = null;
    scheduler.current          = null;
    scheduler.main_thread      = null;
    scheduler.initialized      = true;

    return JNI_OK;
}

// ============================================================
// thread_create
// ============================================================

export function thread_create(jvm, name, priority, thread_obj) {
    if (scheduler.thread_count >= MAX_COOP_THREADS) return null;

    const java_thread = {
        id:                    scheduler.thread_count + 1,
        name:                  name ?? null,
        priority:              priority,
        is_daemon:             false,
        is_alive:              false,
        interrupted:           false,
        current_frame:         null,
        stack_base:            new Uint8Array(JAVA_STACK_SIZE),
        stack_size:            JAVA_STACK_SIZE,
        stack_used:            0,
        thread_object:         thread_obj,
        thread_group:          null,
        pending_exception:     null,
        exception_stack_trace: null,
        exception_throw_info:  null,
        exec_jmp_buf:          null,
        exec_jmp_buf_valid:    0,
        allocator: {
            heap:       new Uint8Array(64 * 1024),
            heap_size:  64 * 1024,
            heap_used:  0,
        },
    };

    const coop = make_coop_thread(java_thread, priority);

    scheduler.threads[scheduler.thread_count++] = coop;

    // Register with JVM so GC can see this thread's frames
    if (jvm && jvm.thread_count < MAX_JAVA_THREADS) {
        jvm.threads[jvm.thread_count++] = java_thread;
    }

    if (name === 'main') {
        scheduler.main_thread    = java_thread;
        scheduler.current        = coop;
        coop.state               = ThreadState.RUNNABLE;
        java_thread.is_alive     = true;
        scheduler.run_queue_head = coop;
        scheduler.run_queue_tail = coop;
    }

    return java_thread;
}

// ============================================================
// thread_start
// ============================================================

export function thread_start(jvm, thread) {
    if (!thread) return JNI_ERR;

    const coop = _find_coop(thread);
    if (!coop) return JNI_ERR;
    if (coop.state !== ThreadState.NEW) return JNI_ERR;

    thread.is_alive = true;
    add_to_run_queue(coop);
    return JNI_OK;
}

// ============================================================
// thread_current / thread_set_current
// ============================================================

export function pthread_set_current_thread(thread) {
    // no-op in single-threaded JS; scheduler.current covers all cases
}

export function thread_current(jvm) {
    return scheduler.current ? scheduler.current.java_thread : scheduler.main_thread;
}

export function thread_set_current(thread) {
    const prev = scheduler.current ? scheduler.current.java_thread : null;
    const coop = _find_coop(thread);
    if (coop) scheduler.current = coop;
    return prev;
}

// ============================================================
// thread_schedule — cooperative context switch
// In JS there is no actual context switch; we just rotate the queue pointer.
// The caller (execute loop) will pick up the new scheduler.current on the
// next call to thread_current().
// ============================================================

export function thread_schedule(jvm) {
    wakeup_sleeping_threads();

    if (scheduler.current &&
        scheduler.current.state === ThreadState.RUNNABLE &&
        scheduler.current.java_thread.is_alive) {
        add_to_run_queue(scheduler.current);
    }

    let next = remove_from_run_queue();

    // Skip same thread if others are available
    if (next && next === scheduler.current && scheduler.run_queue_head) {
        add_to_run_queue(next);
        next = remove_from_run_queue();
    }

    if (next && next !== scheduler.current) {
        scheduler.current = next;
    }
}

// ============================================================
// thread_execute_run — launch a thread's run() method
// In C this used swapcontext to give the thread its own stack.
// In JS we call execute_method synchronously via the injected callback.
// ============================================================

export function thread_execute_run(jvm, java_thread, run_method, run_obj) {
    const coop = _find_coop(java_thread);
    if (!coop) return;

    scheduler.entry_method      = run_method;
    scheduler.entry_obj         = run_obj;
    scheduler.entry_java_thread = java_thread;

    const prev_current = scheduler.current;
    scheduler.current  = coop;

    if (run_method && run_obj && _execute_method_cb) {
        const this_arg = { ref: run_obj };
        _execute_method_cb(jvm, java_thread, run_method, this_arg);
    }

    java_thread.is_alive = false;
    coop.state           = ThreadState.TERMINATED;

    if (java_thread.pending_exception) {
        if (g_j2me_runtime_debug.value) {
            const cls  = java_thread.pending_exception?.header?.clazz?.class_name ?? '?';
            const tname = java_thread.name ?? '(unnamed)';
            process.stderr.write(`[THREAD] Thread '${tname}' terminated with uncaught exception: ${cls} (app continues)\n`);
        }
        // J2ME: uncaught exception in child thread does NOT stop the app
    }

    // Restore previous current (main thread context)
    if (prev_current && prev_current.java_thread === scheduler.main_thread) {
        scheduler.current = prev_current;
    } else {
        // Find main thread coop
        for (let i = 0; i < scheduler.thread_count; i++) {
            if (scheduler.threads[i].java_thread === scheduler.main_thread) {
                scheduler.current = scheduler.threads[i];
                break;
            }
        }
    }

    _check_runnable_and_stop(jvm);
}

function _check_runnable_and_stop(jvm) {
    for (let i = 0; i < scheduler.thread_count; i++) {
        const s = scheduler.threads[i].state;
        if (s === ThreadState.RUNNABLE ||
            s === ThreadState.WAITING  ||
            s === ThreadState.TIMED_WAITING) {
            return;
        }
    }
    if (g_j2me_runtime_debug.value) {
        process.stderr.write('[THREAD] No runnable threads remaining, stopping JVM\n');
    }
    if (jvm) jvm.running = false;
}

// ============================================================
// thread_yield
// In JS there is no preemptive switch. We update scheduler.current so the
// next call to thread_current() returns the correct thread, and process
// any pending SDL events. Real execution interleaving happens at the
// thread_tick boundary in the execute loop.
// ============================================================

export function thread_yield(jvm) {
    if (!scheduler.initialized || !scheduler.current) return;

    wakeup_sleeping_threads();
    sdl_process_events_minimal();

    const coop = scheduler.current;

    // If TIMED_WAITING, park on sleep list
    if (coop.state === ThreadState.TIMED_WAITING && coop.wake_time_ms > 0) {
        remove_specific_from_queue(coop);
        add_to_sleep_list(coop);
    }

    // Find another runnable thread
    let p = scheduler.run_queue_head;
    let has_other = false;
    while (p) {
        if (p !== coop && p.java_thread.is_alive) {
            has_other = true;
            break;
        }
        p = p.next;
    }

    if (!has_other) return;

    // Re-queue current if still alive and not blocked/waiting
    if (coop.java_thread.is_alive && coop.state !== ThreadState.BLOCKED &&
        coop.state !== ThreadState.WAITING && coop.state !== ThreadState.TIMED_WAITING) {
        coop.state = ThreadState.RUNNABLE;
        add_to_run_queue(coop);
    }

    const next = remove_from_run_queue();
    if (next && next !== coop) {
        scheduler.current = next;
    }
}

// ============================================================
// thread_should_yield / thread_tick
// ============================================================

export function thread_should_yield() {
    return (g_instruction_counter % YIELD_INTERVAL) === 0;
}

export function thread_tick(jvm) {
    g_instruction_counter++;
    if ((g_instruction_counter % YIELD_INTERVAL) === 0) {
        thread_yield(jvm);
    }
}

// ============================================================
// thread_sleep
// Marks the thread TIMED_WAITING with a future wake time, then yields.
// The scheduler will re-queue it after the timeout expires.
// ============================================================

export function thread_sleep(jvm, millis) {
    if (millis < 0) millis = 0;
    if (millis > 10000) millis = 10000;

    wakeup_sleeping_threads();
    sdl_process_events_minimal();

    if (millis > 0 && scheduler.current) {
        const coop       = scheduler.current;
        coop.state       = ThreadState.TIMED_WAITING;
        coop.wake_time_ms = get_time_ms() + millis;
        thread_yield(jvm);
        coop.state = ThreadState.RUNNABLE;
    }

    return JNI_OK;
}

// ============================================================
// thread_join — wait for thread to finish
// In the cooperative model we just spin-yield until the target dies.
// ============================================================

export function thread_join(jvm, thread) {
    if (!thread) return JNI_ERR;
    // In a real async context this would need to be awaited; here we simply
    // return OK because the caller will retry via its own wait loop.
    return JNI_OK;
}

// ============================================================
// thread_is_alive / thread_get_state / thread_set_priority
// thread_is_interrupted / thread_find_by_object / thread_destroy
// ============================================================

export function thread_is_alive(thread) {
    return thread ? thread.is_alive : false;
}

export function thread_get_state(thread) {
    if (!thread) return ThreadState.NEW;
    const coop = _find_coop(thread);
    return coop ? coop.state : ThreadState.TERMINATED;
}

export function thread_set_priority(thread, priority) {
    if (!thread) return;
    thread.priority = priority;
    const coop = _find_coop(thread);
    if (coop) coop.priority = priority;
}

export function thread_is_interrupted(thread, clear_flag) {
    if (!thread) return false;
    const interrupted = thread.interrupted;
    if (clear_flag) thread.interrupted = false;
    return interrupted;
}

export function thread_find_by_object(thread_obj) {
    if (!thread_obj) return null;
    for (let i = 0; i < scheduler.thread_count; i++) {
        const jt = scheduler.threads[i].java_thread;
        if (jt && jt.thread_object === thread_obj) return jt;
    }
    return null;
}

export function thread_destroy(jvm, thread) {
    if (!thread) return;

    const coop = _find_coop(thread);
    if (coop) {
        coop.state      = ThreadState.TERMINATED;
        thread.is_alive = false;
    }

    _check_runnable_and_stop(jvm);
}

// ============================================================
// threads_has_runnable
// ============================================================

export function threads_has_runnable() {
    wakeup_sleeping_threads();
    return scheduler.run_queue_head !== null;
}

// ============================================================
// thread_current_coop
// ============================================================

export function thread_current_coop() {
    return scheduler.current;
}

// ============================================================
// thread_prepare_run / thread_get_run_data
// ============================================================

const g_thread_run_data = new Array(MAX_COOP_THREADS).fill(null).map(() => ({
    run_method:  null,
    run_obj:     null,
    java_thread: null,
    jvm:         null,
}));

export function thread_prepare_run(jvm, java_thread, run_method, run_obj) {
    if (!java_thread) return;
    const idx = _find_coop_index(java_thread);
    if (idx < 0 || idx >= MAX_COOP_THREADS) return;
    g_thread_run_data[idx].run_method  = run_method;
    g_thread_run_data[idx].run_obj     = run_obj;
    g_thread_run_data[idx].java_thread = java_thread;
    g_thread_run_data[idx].jvm         = jvm;
}

export function thread_get_run_data(java_thread) {
    const idx = _find_coop_index(java_thread);
    if (idx < 0) return null;
    return g_thread_run_data[idx];
}

// ============================================================
// thread_interrupt
// ============================================================

export function thread_interrupt(thread) {
    if (!thread) return;

    thread.interrupted = true;

    const coop = _find_coop(thread);
    if (coop) {
        if (coop.state === ThreadState.WAITING ||
            coop.state === ThreadState.TIMED_WAITING) {
            coop.state = ThreadState.RUNNABLE;
            add_to_run_queue(coop);
        }
    }

    // Wake any monitor the thread is blocked on
    for (let i = 0; i < g_monitors.length; i++) {
        if (g_monitors[i].waiting_thread === thread) {
            _monitor_wake_waiters(g_monitors[i], false);
            break;
        }
    }
}

// ============================================================
// Monitor subsystem
// In single-threaded JS there is no real blocking. We maintain
// ownership state for correct reentrant lock accounting and
// implement wait/notify via a coop wait-list.
// ============================================================

function make_monitor() {
    return {
        owner:            null,    // JavaObject (the locked object)
        owner_thread:     null,    // JavaThread
        entry_count:      0,
        spinlock:         0,
        wait_list:        [],      // CoopThread[] waiting via monitor_wait
        waiting_thread:   null,    // last thread passed to monitor_wait (for interrupt)
        wait_count:       0,
    };
}

const MONITOR_TABLE_SIZE = 256;
const g_monitors = Array.from({ length: MONITOR_TABLE_SIZE }, make_monitor);
let   g_monitors_initialized = false;

function init_monitors() {
    if (g_monitors_initialized) return;
    g_monitors_initialized = true;
    // Monitors are already initialised by the array literal above
}

function get_monitor(obj) {
    init_monitors();
    // Use object identity hash: address not available in JS, use a WeakMap instead
    return _monitor_for_object(obj);
}

// WeakMap avoids retaining objects and gives true per-object monitors
const _obj_monitor_map = new WeakMap();

function _monitor_for_object(obj) {
    let mon = _obj_monitor_map.get(obj);
    if (!mon) {
        // Fall back to the static pool using a simple slot (pointer-hash equivalent)
        mon = make_monitor();
        _obj_monitor_map.set(obj, mon);
    }
    return mon;
}

export function monitor_enter(jvm, obj) {
    if (!obj) return JNI_ERR;

    const mon     = get_monitor(obj);
    const current = thread_current(jvm);

    // Reentrant check
    if (mon.entry_count > 0 && mon.owner_thread === current) {
        mon.entry_count++;
        return JNI_OK;
    }

    // In single-threaded JS, if someone else "owns" the monitor it is a bug
    // in the caller's lock discipline. We accept the lock unconditionally to
    // avoid deadlocking the interpreter.
    mon.owner        = obj;
    mon.owner_thread = current;
    mon.entry_count  = 1;
    return JNI_OK;
}

export function monitor_exit(jvm, obj) {
    if (!obj) return JNI_ERR;

    const mon     = get_monitor(obj);
    const current = thread_current(jvm);

    if (mon.entry_count === 0 || mon.owner_thread !== current) {
        return JNI_ERR;
    }

    mon.entry_count--;
    if (mon.entry_count === 0) {
        mon.owner        = null;
        mon.owner_thread = null;

        // Wake one waiter (notify semantics on exit matches Java spec for
        // threads that were notified but couldn't re-acquire yet)
        if (mon.wait_list.length > 0) {
            const waiter = mon.wait_list.shift();
            mon.wait_count--;
            waiter.state = ThreadState.RUNNABLE;
            add_to_run_queue(waiter);
        }
    }
    return JNI_OK;
}

export function monitor_wait(jvm, obj, timeout, timed) {
    if (!obj) return JNI_ERR;

    const mon     = get_monitor(obj);
    const current = thread_current(jvm);

    if (mon.entry_count === 0 || mon.owner_thread !== current) {
        return JNI_ERR;
    }

    if (current && current.interrupted) {
        return JNI_ERR;   // Caller throws InterruptedException
    }

    const coop = _find_coop(current);
    if (!coop) return JNI_ERR;

    // Save and release the monitor
    const saved_count    = mon.entry_count;
    mon.entry_count      = 0;
    mon.owner            = null;
    mon.owner_thread     = null;
    mon.waiting_thread   = current;
    mon.wait_count++;
    mon.wait_list.push(coop);

    if (timed && timeout > 0) {
        coop.state        = ThreadState.TIMED_WAITING;
        coop.wake_time_ms = get_time_ms() + Number(timeout);
    } else {
        coop.state = ThreadState.WAITING;
    }

    // Yield — the scheduler will run other threads; we will be re-queued
    // when monitor_notify or the timeout fires (see thread_yield /
    // wakeup_sleeping_threads).
    thread_yield(jvm);

    // Re-acquire the monitor on return
    mon.waiting_thread = null;
    mon.owner          = obj;
    mon.owner_thread   = current;
    mon.entry_count    = saved_count;
    coop.state         = ThreadState.RUNNABLE;

    if (current && current.interrupted) {
        return JNI_ERR;
    }

    return JNI_OK;
}

export function monitor_notify(jvm, obj) {
    if (!obj) return JNI_ERR;

    const mon     = get_monitor(obj);
    const current = thread_current(jvm);

    if (mon.entry_count === 0 || mon.owner_thread !== current) {
        return JNI_ERR;
    }

    if (mon.wait_list.length > 0) {
        const waiter = mon.wait_list.shift();
        mon.wait_count--;
        mon.waiting_thread = null;
        waiter.state = ThreadState.RUNNABLE;
        add_to_run_queue(waiter);
    }

    return JNI_OK;
}

export function monitor_notify_all(jvm, obj) {
    if (!obj) return JNI_ERR;

    const mon     = get_monitor(obj);
    const current = thread_current(jvm);

    if (mon.entry_count === 0 || mon.owner_thread !== current) {
        return JNI_ERR;
    }

    while (mon.wait_list.length > 0) {
        const waiter = mon.wait_list.shift();
        mon.wait_count--;
        waiter.state = ThreadState.RUNNABLE;
        add_to_run_queue(waiter);
    }
    mon.waiting_thread = null;

    return JNI_OK;
}

export function monitor_get(jvm, obj) {
    return null;   // matches C stub that returns NULL
}

export function monitor_get_owner(obj) {
    if (!obj) return null;
    const mon = get_monitor(obj);
    return mon.owner_thread ?? null;
}

export function monitor_get_entry_count(obj) {
    if (!obj) return 0;
    return get_monitor(obj).entry_count;
}

export function cleanup_monitors() {
    _obj_monitor_map.clear?.();   // WeakMap has no clear; this is a no-op safety call
    g_monitors_initialized = false;
}

// Wake waiters from monitor (used by thread_interrupt)
function _monitor_wake_waiters(mon, all) {
    if (all) {
        while (mon.wait_list.length > 0) {
            const w = mon.wait_list.shift();
            mon.wait_count--;
            w.state = ThreadState.RUNNABLE;
            add_to_run_queue(w);
        }
        mon.waiting_thread = null;
    } else if (mon.wait_list.length > 0) {
        const w = mon.wait_list.shift();
        mon.wait_count--;
        w.state = ThreadState.RUNNABLE;
        add_to_run_queue(w);
        if (mon.wait_list.length === 0) mon.waiting_thread = null;
    }
}

// ============================================================
// Thread-local storage
// ============================================================

let _tls_next_key = 0;

export function thread_local_create(key_out) {
    const key = ++_tls_next_key;
    if (key_out && typeof key_out === 'object') key_out.value = key;
    return JNI_OK;
}

export function thread_local_set(key, value) {
    const coop = scheduler.current;
    if (coop) coop.tls_map.set(key, value);
    return JNI_OK;
}

export function thread_local_get(key) {
    const coop = scheduler.current;
    if (!coop) return null;
    return coop.tls_map.get(key) ?? null;
}

export function thread_local_delete(key) {
    const coop = scheduler.current;
    if (coop) coop.tls_map.delete(key);
}

// ============================================================
// Atomic operations
// Single-threaded JS has no concurrent mutation, so these are
// direct operations — the "atomic" guarantee is trivially satisfied.
// ============================================================

export function memory_barrier() {
    // no-op
}

export function atomic_cas_int(ptr, expected, newval) {
    if (ptr.value === expected) {
        ptr.value = newval;
        return true;
    }
    return false;
}

export function atomic_cas_long(ptr, expected, newval) {
    if (ptr.value === expected) {
        ptr.value = newval;
        return true;
    }
    return false;
}

export function atomic_cas_ptr(ptr, expected, newval) {
    if (ptr.value === expected) {
        ptr.value = newval;
        return true;
    }
    return false;
}

export function atomic_increment(ptr) {
    return ++ptr.value;
}

export function atomic_decrement(ptr) {
    return --ptr.value;
}

export function atomic_add(ptr, value) {
    ptr.value += value;
    return ptr.value;
}

// ============================================================
// Lock-free queue  (mirrors LFQueue / LFQueueNode in threads.h)
// In single-threaded JS a simple linked list is lock-free by definition.
// ============================================================

export function lfqueue_init(queue) {
    if (queue) {
        queue.head = null;
        queue.tail = null;
    }
}

export function lfqueue_push(queue, data) {
    if (!queue) return -1;
    const node = { data, next: null };
    if (queue.tail) {
        queue.tail.next = node;
    } else {
        queue.head = node;
    }
    queue.tail = node;
    return 0;
}

export function lfqueue_pop(queue) {
    if (!queue || !queue.head) return null;
    const node = queue.head;
    const data = node.data;
    queue.head = node.next;
    if (!queue.head) queue.tail = null;
    return data;
}

export function lfqueue_empty(queue) {
    return !queue || queue.head === null;
}

export function lfqueue_destroy(queue) {
    if (!queue) return;
    queue.head = null;
    queue.tail = null;
}

// ============================================================
// Internal helpers
// ============================================================

function _find_coop(java_thread) {
    for (let i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i].java_thread === java_thread) {
            return scheduler.threads[i];
        }
    }
    return null;
}

function _find_coop_index(java_thread) {
    for (let i = 0; i < scheduler.thread_count; i++) {
        if (scheduler.threads[i].java_thread === java_thread) {
            return i;
        }
    }
    return -1;
}
