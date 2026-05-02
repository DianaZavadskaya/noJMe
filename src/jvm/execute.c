/*
 * J2ME Emulator - Bytecode Execution Engine
 * Main interpreter loop and method invocation
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <stdbool.h>

#include "jvm.h"
#include "classfile.h"
#include "opcodes.h"
#include "heap.h"
#include "threads.h"
#include "native.h"
/* Performance: direct access to instruction counter for inlined scheduling */
extern volatile uint64_t g_instruction_counter;
#include "debug.h"
#include "debug_macros.h"

/* M3G debug logging - disabled by default */
#ifndef M3G_DEBUG_LOG
#define M3G_DEBUG_LOG 0
#endif

/* Forward declarations */
int execute_method(JVM* jvm, JavaThread* thread, JavaMethod* method, JavaValue* args, JavaValue* result);
static int interpret(JVM* jvm, JavaThread* thread, JavaFrame* frame);

/* Frame pool for fast allocation (avoids repeated malloc/free) */
#define FRAME_POOL_SIZE 256
typedef struct {
        JavaFrame* frames[FRAME_POOL_SIZE];
        JavaValue* locals_blocks[FRAME_POOL_SIZE];
        JavaValue* stack_blocks[FRAME_POOL_SIZE];
        int count;
        int max_locals[FRAME_POOL_SIZE];
        int max_stack[FRAME_POOL_SIZE];
} FramePool;

static FramePool frame_pool = { .count = 0 };

/* === CRITICAL: Recalculate instance_size after superclass is loaded ===
 * This must be called after superclass is guaranteed to be loaded.
 * It properly calculates the total instance size including all inherited fields.
 * 
 * JVM Specification §4.5: Fields are laid out in the order they are declared,
 * with superclass fields appearing before subclass fields.
 */
void jvm_recalculate_instance_size(JVM* jvm, JavaClass* clazz) {
    if (!clazz) return;

    /* GUARD: Skip M3G (JSR-184) stub classes.
     * Their instance_size is managed exclusively by init_m3g_stub_classes()
     * in mobile3d.c. Re-running the calculation here (especially from
     * jvm_init_class at line 902) can produce a different size if the
     * class hierarchy or field count differs from what mobile3d.c set up.
     * This would cause objects allocated with the mobile3d.c size to be
     * accessed with a different size — heap corruption. */
    if (clazz->class_name && strstr(clazz->class_name, "javax/microedition/m3g/") != NULL) {
        return;
    }

    /* DEBUG: Log structure layout once */
    static bool layout_logged = false;
    if (!layout_logged) {
        if (g_j2me_runtime_debug) LOG_SAFE("[LAYOUT DEBUG] sizeof(JavaClass)=%zu, sizeof(ObjectHeader)=%zu\n",
                sizeof(JavaClass), sizeof(ObjectHeader));
        if (g_j2me_runtime_debug) LOG_SAFE("[LAYOUT DEBUG] offset of super_class_name=%zu, super_class=%zu\n",
                offsetof(JavaClass, super_class_name), offsetof(JavaClass, super_class));
        if (g_j2me_runtime_debug) LOG_SAFE("[LAYOUT DEBUG] offset of constant_pool_capacity=%zu\n",
                offsetof(JavaClass, constant_pool_capacity));
        layout_logged = true;
    }
    
    /* DEBUG: Validate clazz pointer */
    if ((uintptr_t)clazz < 0x10000) {
        LOG_SAFE("[FATAL] jvm_recalculate_instance_size: invalid clazz=%p (too low)\n", (void*)clazz);
        return;
    }
    
    /* DEBUG: Check if clazz looks like a string */
    if (clazz->magic != 0xCAFEBABE && clazz->magic != 0xDEADBEEF) {
        LOG_SAFE("[FATAL] jvm_recalculate_instance_size: clazz=%p has invalid magic=0x%08X\n", 
                (void*)clazz, clazz->magic);
        /* Try to detect if this is a string */
        const char* maybe_str = (const char*)clazz;
        LOG_SAFE("[FATAL] Looks like string: %.20s\n", maybe_str);
        return;
    }
    
    /* Note: We used to skip stub classes, but they also need proper instance_size
     * calculation to include inherited fields. The stub class initialization only
     * sets instance_size for its own fields, not inherited ones. */
    
    /* Start with superclass size or ObjectHeader if no superclass */
    size_t size = sizeof(ObjectHeader);  /* Always start with header */
    
    /* CRITICAL: Ensure superclass is loaded and has correct size */
    if (clazz->super_class_name && !clazz->super_class) {
        /* Try to load superclass now */
        clazz->super_class = jvm_load_class(jvm, clazz->super_class_name);
    }
    
    if (clazz->super_class) {
        /* Recursively ensure superclass has correct size */
        jvm_recalculate_instance_size(jvm, clazz->super_class);
        
        /* Superclass should already have correct instance_size including header */
        /* Use >= because a superclass with no fields still has ObjectHeader */
        if (clazz->super_class->instance_size >= sizeof(ObjectHeader)) {
            size = clazz->super_class->instance_size;
        }
    }
    
    /* Add own instance fields */
    int own_field_count = 0;
    if (clazz->fields) {
        for (uint16_t i = 0; i < clazz->fields_count; i++) {
            JavaField* field = &clazz->fields[i];
            
            /* Skip static fields */
            if (field->access_flags & ACC_STATIC) continue;
            
            own_field_count++;
            
            /* Each field takes one JavaValue slot */
            size += sizeof(JavaValue);
            
            /* Long and double take 2 slots */
            if (field->descriptor && 
                (field->descriptor[0] == 'J' || field->descriptor[0] == 'D')) {
                size += sizeof(JavaValue);
            }
        }
    }
    
    /* Log M3G class instance_size recalculations */
    #if M3G_DEBUG_LOG
    if (clazz->class_name && strstr(clazz->class_name, "m3g/") != NULL) {
        LOG_SAFE("[M3G-SIZE] %s: super=%s(%p), old_size=%zu, new_size=%zu, own_fields=%d\n",
                clazz->class_name,
                clazz->super_class ? clazz->super_class->class_name : "NONE",
                (void*)clazz->super_class,
                clazz->instance_size, size, own_field_count);
    }
    #endif
    
    /* Update if different */
    if (clazz->instance_size != size) {
        EXEC_DEBUG("Recalculated instance_size for %s: %zu -> %zu bytes (super=%s, own_fields=%d)",
                   clazz->class_name ? clazz->class_name : "?",
                   clazz->instance_size, size,
                   clazz->super_class ? clazz->super_class->class_name : "none",
                   own_field_count);
        clazz->instance_size = size;
    }
}

/* Create a new frame */
static JavaFrame* frame_create(JVM* jvm, JavaMethod* method, JavaClass* clazz) {
        if (!method || !clazz) {
                ERROR_LOG("frame_create: NULL method or clazz");
                return NULL;
        }
        int max_locals = method->code.max_locals;
        int max_stack = method->code.max_stack + 4;  /* TEMPORARY FIX: Add 4 to max_stack */
        
        /* Try to reuse from pool */
        if (frame_pool.count > 0) {
                int idx = --frame_pool.count;
                JavaFrame* frame = frame_pool.frames[idx];
                if (frame_pool.max_locals[idx] >= max_locals && frame_pool.max_stack[idx] >= max_stack) {
                        frame->locals = frame_pool.locals_blocks[idx];
                        frame->stack = frame_pool.stack_blocks[idx];
                        /* Clear only needed portions (not entire allocation) */
                        memset(frame->locals, 0, max_locals * sizeof(JavaValue));
                        memset(frame->stack, 0, max_stack * sizeof(JavaValue));
                        /* Reset frame state */
                        frame->pc = 0;
                        frame->stack_top = -1;
                        frame->prev = NULL;
                        frame->clazz = method->clazz;
                        frame->method = method;
                        frame->code = method->code.code;
                        frame->code_length = method->code.code_length;
                        frame->max_locals = max_locals;
                        frame->max_stack = max_stack;
                        frame->throwing_pc = 0;
                        frame->exception_table = method->code.exception_table;
                        frame->exception_table_length = method->code.exception_table_length;
                        return frame;
                }
                /* Size mismatch, put back and fall through to alloc */
                frame_pool.count++;
        }
        
        /* Pool miss or empty - allocate new */
        JavaFrame* frame = (JavaFrame*)calloc(1, sizeof(JavaFrame));
        if (!frame) {
                ERROR_LOG("frame_create: Failed to allocate frame");
                return NULL;
        }

        frame->method = method;
        frame->clazz = clazz;
        frame->pc = 0;
        frame->code = method->code.code;
        frame->code_length = method->code.code_length;

        /* Allocate locals */
        frame->max_locals = max_locals;
        if (frame->max_locals > 0) {
                frame->locals = (JavaValue*)calloc(frame->max_locals, sizeof(JavaValue));
                if (!frame->locals) {
                        ERROR_LOG("frame_create: Failed to allocate locals");
                        free(frame);
                        return NULL;
                }
        }

        /* Allocate operand stack */
        frame->max_stack = max_stack;
        frame->stack_top = -1;
        if (frame->max_stack > 0) {
                frame->stack = (JavaValue*)calloc(frame->max_stack, sizeof(JavaValue));
                if (!frame->stack) {
                        ERROR_LOG("frame_create: Failed to allocate stack");
                        free(frame->locals);
                        free(frame);
                        return NULL;
                }
        }

        /* Exception table */
        frame->exception_table = method->code.exception_table;
        frame->exception_table_length = method->code.exception_table_length;

        EXEC_DEBUG("Created frame for %s%s (locals: %d, stack: %d, code: %u bytes)",
                   method->name, method->descriptor,
                   frame->max_locals, frame->max_stack, frame->code_length);

        return frame;
}

/* Free a frame */
static void frame_destroy(JavaFrame* frame) {
    if (!frame) return;
    
    /* Return to pool instead of freeing */
    if (frame_pool.count < FRAME_POOL_SIZE) {
        int idx = frame_pool.count++;
        frame_pool.frames[idx] = frame;
        frame_pool.locals_blocks[idx] = frame->locals;
        frame_pool.stack_blocks[idx] = frame->stack;
        frame_pool.max_locals[idx] = frame->max_locals;
        frame_pool.max_stack[idx] = frame->max_stack;
        return;
    }
    
    /* Pool full - actually free */
    free(frame->locals);
    free(frame->stack);
    free(frame);
}

/* Push frame onto thread stack */
static int push_frame(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    if (!thread || !frame) return -1;

    frame->prev = thread->current_frame;
    thread->current_frame = frame;

    EXEC_DEBUG("Pushed frame, depth now: %d", 
               thread->current_frame ? 
               (thread->current_frame->prev ? 1 : 0) + 1 : 0);

    return 0;
}

/* Pop frame from thread stack */
static JavaFrame* pop_frame(JVM* jvm, JavaThread* thread) {
    if (!thread || !thread->current_frame) return NULL;

    JavaFrame* frame = thread->current_frame;
    thread->current_frame = frame->prev;

    EXEC_DEBUG("Popped frame, depth now: %d",
               thread->current_frame ? 1 : 0);

    return frame;
}

/* Count arguments in method descriptor - uses native.h version */
/* Forward declaration for readability */
extern int count_args(const char* descriptor);

/* Execute a method */
int execute_method(JVM* jvm, JavaThread* thread, JavaMethod* method, 
                   JavaValue* args, JavaValue* result) {
    JavaValue* out_result = result;  /* Save before potential shadowing */
    if (!jvm || !thread || !method) {
        ERROR_LOG("execute_method: NULL parameter");
        return -1;
    }

    /* Check recursion depth to prevent stack overflow */
    static int recursion_depth = 0;
    static const int MAX_RECURSION_DEPTH = 500;
    recursion_depth++;
    if (recursion_depth > MAX_RECURSION_DEPTH) {
        /* Print call stack to identify which method is recursing */
        JavaThread* cur = jvm_current_thread(jvm);
        if (cur && cur->current_frame) {
            LOG_SAFE("[RECURSION-ERROR] Call stack (depth=%d):\n", recursion_depth);
            JavaFrame* f = cur->current_frame;
            int frames_shown = 0;
            while (f && frames_shown < 20) {
                LOG_SAFE("  [%d] %s.%s%s\n", frames_shown,
                    f->clazz ? (f->clazz->class_name ? f->clazz->class_name : "?") : "?",
                    f->method ? f->method->name : "?",
                    f->method ? f->method->descriptor : "");
                f = f->prev;
                frames_shown++;
            }
        }
        recursion_depth--;
        ERROR_LOG("execute_method: Maximum recursion depth exceeded (%d)", recursion_depth);
        jvm_throw_by_name(jvm, "java/lang/StackOverflowError", NULL);
        return -1;
    }

    EXEC_DEBUG("execute_method: %s%s (depth=%d)", method->name, method->descriptor, recursion_depth);

    /* Check if native - first check the flag, then check registry for stub classes */
    if (method->is_native) {
        EXEC_DEBUG("Calling native method: %s%s", method->name, method->descriptor);
        recursion_depth--;
        return native_call(jvm, thread, method, args, result);
    }
    
    /* For stub classes, check if there's a registered native handler even if not marked native */
    extern NativeMethod native_find(JVM* jvm, const char* class_name, const char* method_name,
                                    const char* descriptor);
    if (method->clazz && method->clazz->class_name && 
        native_find(jvm, method->clazz->class_name, method->name, method->descriptor)) {
        EXEC_DEBUG("Calling native method (from registry): %s%s", method->name, method->descriptor);
        recursion_depth--;
        return native_call(jvm, thread, method, args, result);
    }

    /* Check if method has code */
    if (!method->code.code || method->code.code_length == 0) {
        ERROR_LOG("Method %s has no code", method->name);
        recursion_depth--;
        return -1;
    }

    /* Handle synchronized methods - acquire monitor before execution */
    bool is_synchronized = (method->access_flags & ACC_SYNCHRONIZED) != 0;
    bool is_static = (method->access_flags & ACC_STATIC) != 0;
    JavaObject* sync_obj = NULL;
    
    if (is_synchronized) {
        if (is_static) {
            /* For static methods, synchronize on the Class object */
            sync_obj = (JavaObject*)method->clazz;
        } else {
            /* For instance methods, synchronize on 'this' */
            if (args && args[0].ref) {
                sync_obj = (JavaObject*)args[0].ref;
            }
        }
        
        EXEC_DEBUG("[SYNC_METHOD] %s.%s is synchronized, sync_obj=%p",
                method->clazz ? method->clazz->class_name : "?", 
                method->name ? method->name : "?",
                (void*)sync_obj);
        
        if (sync_obj) {
            int mon_result = monitor_enter(jvm, sync_obj);
            if (mon_result != JNI_OK) {
                ERROR_LOG("Failed to enter monitor for synchronized method %s", method->name);
                recursion_depth--;
                return -1;
            }
            EXEC_DEBUG("[SYNC_METHOD] Entered monitor for %s.%s",
                    method->clazz ? method->clazz->class_name : "?",
                    method->name ? method->name : "?");
        } else {
            WARN_LOG("[SYNC_METHOD] sync_obj is NULL for %s.%s!",
                    method->clazz ? method->clazz->class_name : "?",
                    method->name ? method->name : "?");
        }
    }

    /* Create frame */
    JavaFrame* frame = frame_create(jvm, method, method->clazz);
    if (!frame) {
        ERROR_LOG("Failed to create frame for %s", method->name);
        if (is_synchronized && sync_obj) {
            monitor_exit(jvm, sync_obj);
        }
        recursion_depth--;
        return -1;
    }

    /* Copy arguments to locals */
    (void)method_arg_count(method);  /* Verify descriptor is valid and cache count */
    int local_idx = 0;

    if (!is_static && args) {
        /* this reference */
        frame->locals[local_idx++] = args[0];
    }

    /* Copy remaining arguments from descriptor 
     * For non-static: args[0]=this, args[1..]=method args
     * For static: args[0..]=method args
     * IMPORTANT: double/long take 2 slots in locals but only 1 entry in args[]
     */
    int args_start = is_static ? 0 : 1;
    int arg_idx = args_start;
    const char* desc = method->descriptor;
    
    /* Skip past '(' */
    if (*desc == '(') desc++;
    
    while (*desc && *desc != ')') {
        /* Copy argument value to locals */
        frame->locals[local_idx] = args[arg_idx];
        
        switch (*desc) {
            case 'J': case 'D':
                /* Long/double: occupies 2 slots in locals, but is 1 entry in args */
                local_idx += 2;
                arg_idx++;
                desc++;
                break;
            case 'B': case 'C': case 'F': case 'I': case 'S': case 'Z':
                local_idx++;
                arg_idx++;
                desc++;
                break;
            case 'L':
                local_idx++;
                arg_idx++;
                while (*desc && *desc != ';') desc++;
                if (*desc == ';') desc++;
                break;
            case '[':
                local_idx++;
                arg_idx++;
                while (*desc == '[') desc++;
                if (*desc == 'L') while (*desc && *desc != ';') desc++;
                if (*desc) desc++;
                break;
            default:
                desc++;
                break;
        }
    }

    /* Push frame */
    push_frame(jvm, thread, frame);

    /* DEBUG: Log frame relationship */
    EXEC_DEBUG("DEBUG: Pushed frame %p, prev=%p, thread->current_frame=%p",
            (void*)frame, (void*)frame->prev, (void*)thread->current_frame);
    if (frame->prev) {
        EXEC_DEBUG("DEBUG: Caller stack_top before: %d", frame->prev->stack_top);
    }

    /* Execute */
    int exec_result = interpret(jvm, thread, frame);

    /* DEBUG: Log stack state after interpret for create method */
    if (method->name && strcmp(method->name, "create") == 0) {
        if (g_j2me_runtime_debug) LOG_SAFE("[EXECUTE_METHOD] After interpret for %s: result=%d\n", method->name, exec_result);
        if (g_j2me_runtime_debug) LOG_SAFE("[EXECUTE_METHOD] frame=%p, frame->prev=%p\n", (void*)frame, (void*)frame->prev);
        if (frame->prev) {
            if (g_j2me_runtime_debug) LOG_SAFE("[EXECUTE_METHOD] Caller stack_top=%d, stack[top]=%p\n",
                    frame->prev->stack_top,
                    (void*)frame->prev->stack[frame->prev->stack_top].ref);
        }
    }

    /* DEBUG: Log stack state after interpret */
    EXEC_DEBUG("DEBUG: interpret returned %d, frame->prev=%p", exec_result, (void*)frame->prev);
    if (frame->prev) {
        EXEC_DEBUG("DEBUG: Caller stack_top after: %d", frame->prev->stack_top);
        if (frame->prev->stack_top >= 0) {
            EXEC_DEBUG("DEBUG: Caller stack[top] = %p",
                    (void*)frame->prev->stack[frame->prev->stack_top].ref);
        }
    }

    /* Pop frame */
    pop_frame(jvm, thread);

    /* For non-native methods: return value was pushed to caller's stack by
     * op_ireturn/op_areturn/etc. Now that we've popped the method frame,
     * the return value is on the new current_frame's stack. */
    if (out_result && exec_result == 0 && !method->is_native) {
        JavaFrame* caller = thread->current_frame;
        if (caller && caller->stack_top > 0) {
            *out_result = caller->stack[caller->stack_top - 1];
        }
    }

    /* DEBUG: Verify current frame after pop for create method */
    if (method->name && strcmp(method->name, "create") == 0) {
        if (g_j2me_runtime_debug) LOG_SAFE("[EXECUTE_METHOD] After pop_frame: thread->current_frame=%p\n",
                (void*)thread->current_frame);
        if (thread->current_frame) {
            if (g_j2me_runtime_debug) LOG_SAFE("[EXECUTE_METHOD] Current stack_top=%d, stack[top]=%p\n",
                    thread->current_frame->stack_top,
                    (void*)thread->current_frame->stack[thread->current_frame->stack_top].ref);
        }
    }

    /* Get result - already handled above for non-native, 
     * for native methods the result is set by native_call */

    /* Handle synchronized methods - release monitor after execution */
    if (is_synchronized && sync_obj) {
        monitor_exit(jvm, sync_obj);
    }

    /* Cleanup */
    frame_destroy(frame);

    recursion_depth--;
    return exec_result;
}

/* Main bytecode interpreter - NO setjmp/longjmp for thread safety
 * 
 * Each thread has its own call stack, so exceptions are propagated via return values.
 * This avoids the problem where Thread 2's jmp_buf would corrupt Thread 1's exception handling.
 */
static int interpret(JVM* jvm, JavaThread* thread, JavaFrame* frame) {
    if (!frame || !frame->code) {
        ERROR_LOG("interpret: NULL frame or code");
        return -1;
    }

    EXEC_DEBUG("interpret: Starting at PC=0, code_length=%u", frame->code_length);

continue_execution:
    ;  /* Empty statement required before declarations in C */
    /* Main interpreter loop - instruction limit only for debugging/headless mode */
    static uint64_t total_instructions = 0;
    static uint64_t max_instructions = 0;  /* 0 = unlimited (default) */
    static int env_checked = 0;
    if (!env_checked) {
        env_checked = 1;
        const char* max_instr_env = getenv("J2ME_MAX_INSTRUCTIONS");
        if (max_instr_env) {
            max_instructions = strtoull(max_instr_env, NULL, 10);
        }
    }
    
    while (frame->pc < frame->code_length && jvm->running) {
        /* Instruction limit check (only if limit is set) */
        total_instructions++;
        if (max_instructions > 0 && total_instructions > max_instructions) {
            LOG_SAFE("[JVM] Instruction limit reached (%llu instructions). Stopping.\n", 
                    (unsigned long long)total_instructions);
            jvm->running = false;
            return -1;
        }
        
        /* Save the PC of the instruction we are about to execute.
         * This is needed for correct exception table lookup. */
        frame->throwing_pc = frame->pc;

        uint8_t opcode = frame->code[frame->pc++];

        /* Cooperative thread scheduling (inlined for performance) */
        if (++g_instruction_counter >= YIELD_INTERVAL) {
            g_instruction_counter = 0;
            thread_yield(jvm);
        }

        /* Opcode tracing - uses runtime debug flag (toggle with F12) */
        if (g_j2me_runtime_debug) {
            const char* op_name = opcode_table[opcode].name;
            const char* method_name = frame->method ? frame->method->name : "?";
            const char* class_name = (frame->clazz && frame->clazz->class_name) ? frame->clazz->class_name : "?";
            
            LOG_SAFE("[OPCODE] %s.%s PC=%d: %s (0x%02X) [stack=%d/%d]\n",
                    class_name, method_name, frame->pc - 1, 
                    op_name ? op_name : "unknown", opcode,
                    frame->stack_top + 1, frame->max_stack);
        }

        /* Get opcode handler */
        OpcodeHandler handler = opcode_table[opcode].handler;
        if (!handler) {
            ERROR_LOG("Unknown opcode: 0x%02X at PC=%d", opcode, frame->pc - 1);
            jvm_throw_by_name(jvm, "java/lang/VirtualMachineError", "Unknown opcode");
            return -1;
        }

        /* Execute opcode */
        int result = handler(jvm, thread, frame);

        if (result > 0) {
            EXEC_DEBUG("Method returned");
            return 0;  /* Normal return */
        }

        /* Check for exception */
        if (result < 0) {
            if (!thread->pending_exception) {
                /* ERROR: Handler returned -1 but no exception set — log diagnostic */
                LOG_SAFE("[JVM] OPCODE FAIL: opcode 0x%02X (%s) at PC=%d in %s.%s returned %d WITHOUT exception!\n",
                        opcode,
                        (opcode < 256 && opcode_table[opcode].name) ? opcode_table[opcode].name : "?",
                        frame->pc - 1,
                        frame->clazz ? (frame->clazz->class_name ? frame->clazz->class_name : "?") : "?",
                        frame->method ? (frame->method->name ? frame->method->name : "?") : "?",
                        result);
                return -1;
            }
            if (thread->pending_exception) {
                /* Handle exception - search for handler in this frame */
                JavaObject* exception = thread->pending_exception;
                JavaClass* exception_class = exception->header.clazz;

                if (g_j2me_runtime_debug) LOG_SAFE("[EXCEPTION] Exception %s occurred at PC=%d in %s.%s\n",
                           exception_class ? exception_class->class_name : "??",
                           frame->throwing_pc,
                           frame->clazz ? (frame->clazz->class_name ? frame->clazz->class_name : "?") : "?",
                           frame->method ? (frame->method->name ? frame->method->name : "?") : "?");

                /* Look for exception handler */
                bool handler_found = false;
                if (g_j2me_runtime_debug) {
                    if (frame->exception_table) {
                        LOG_SAFE("[EX-SEARCH] Throwing PC=%d in %s.%s, scanning %d entries\n",
                                frame->throwing_pc,
                                frame->clazz ? (frame->clazz->class_name ? frame->clazz->class_name : "?") : "?",
                                frame->method ? (frame->method->name ? frame->method->name : "?") : "?",
                                frame->exception_table_length);
                    } else {
                        LOG_SAFE("[EX-SEARCH] Throwing PC=%d in %s.%s, NO exception_table (NULL)\n",
                                frame->throwing_pc,
                                frame->clazz ? (frame->clazz->class_name ? frame->clazz->class_name : "?") : "?",
                                frame->method ? (frame->method->name ? frame->method->name : "?") : "?");
                    }
                }
                if (frame->exception_table) {
                    for (uint16_t i = 0; i < frame->exception_table_length; i++) {
                        ExceptionTableEntry* handler = &frame->exception_table[i];

                        if (g_j2me_runtime_debug) {
                            const char* catch_cls = (handler->catch_type != 0)
                                ? classfile_get_class_name(frame->clazz, handler->catch_type)
                                : "<any/finally>";
                            int in_range = (frame->throwing_pc >= handler->start_pc &&
                                           frame->throwing_pc < handler->end_pc);
                            LOG_SAFE("[EX-SEARCH]   entry[%d]: PC=%d in [%d,%d)? %s | handler_pc=%d catch_type=%d (%s)\n",
                                    i, frame->throwing_pc,
                                    handler->start_pc, handler->end_pc,
                                    in_range ? "YES" : "NO",
                                    handler->handler_pc, handler->catch_type,
                                    catch_cls ? catch_cls : "?");
                        }

                        if (frame->throwing_pc >= handler->start_pc &&
                            frame->throwing_pc < handler->end_pc) {

                            if (handler->catch_type == 0) {
                                /* catch_type 0 means "any" (finally block) */
                                handler_found = true;
                            } else {
                                const char* catch_name = classfile_get_class_name(frame->clazz, handler->catch_type);
                                if (catch_name) {
                                    JavaClass* catch_class = jvm_load_class(jvm, catch_name);
                                    if (catch_class && object_instance_of(exception, catch_class)) {
                                        handler_found = true;
                                    } else if (g_j2me_runtime_debug) {
                                        LOG_SAFE("[EX-SEARCH]     -> catch_type resolved to %s, instance_of=%s\n",
                                                catch_name,
                                                (catch_class && object_instance_of(exception, catch_class)) ? "true" : "false");
                                    }
                                } else if (g_j2me_runtime_debug) {
                                    LOG_SAFE("[EX-SEARCH]     -> catch_type index %d could not be resolved!\n", handler->catch_type);
                                }
                            }

                            if (handler_found) {
                                if (g_j2me_runtime_debug) LOG_SAFE("[EXCEPTION] Handler found at PC=%d for %s\n", handler->handler_pc,
                                        exception_class ? exception_class->class_name : "??");

                                /* Clear operand stack and push exception */
                                frame->stack_top = 0;
                                JavaValue v = { .ref = exception };
                                frame->stack[0] = v;
                                thread->pending_exception = NULL;

                                /* Clear saved stack trace - exception is now handled */
                                if (thread->exception_stack_trace) {
                                    free(thread->exception_stack_trace);
                                    thread->exception_stack_trace = NULL;
                                }
                                if (thread->exception_throw_info) {
                                    free(thread->exception_throw_info);
                                    thread->exception_throw_info = NULL;
                                }

                                /* Jump to handler */
                                frame->pc = handler->handler_pc;
                                goto continue_execution;
                            }
                        }
                    }
                }

                if (!handler_found) {
                    /* No handler found in this frame, propagate up */
                    if (g_j2me_runtime_debug) LOG_SAFE("[EXCEPTION] No handler found in %s.%s, propagating exception %s\n",
                           frame->clazz ? (frame->clazz->class_name ? frame->clazz->class_name : "?") : "?",
                           frame->method ? (frame->method->name ? frame->method->name : "?") : "?",
                           exception_class ? exception_class->class_name : "??");
                    
                    /* Save stack trace before unwinding - only save once per exception */
                    if (thread->exception_stack_trace == NULL) {
                        /* Build stack trace from current frame chain */
                        char trace_buf[4096];
                        int pos = 0;
                        int frame_count = 0;
                        JavaFrame* f = frame;
                        while (f && frame_count < 20 && pos < (int)sizeof(trace_buf) - 2) {
                            const char* cls = f->clazz && f->clazz->class_name ? f->clazz->class_name : "?";
                            const char* meth = f->method && f->method->name ? f->method->name : "?";
                            const char* desc = f->method && f->method->descriptor ? f->method->descriptor : "";
                            const char* slash = strrchr(cls, '/');
                            const char* short_cls = slash ? slash + 1 : cls;
                            
                            int written;
                            if (frame_count == 0) {
                                written = snprintf(trace_buf + pos, sizeof(trace_buf) - pos - 1,
                                        ">%s.%s%s at PC=%d\n",
                                        short_cls, meth, desc, f->throwing_pc);
                            } else {
                                written = snprintf(trace_buf + pos, sizeof(trace_buf) - pos - 1,
                                        "  at %s.%s%s\n",
                                        short_cls, meth, desc);
                            }
                            if (written > 0 && pos + written < (int)sizeof(trace_buf) - 1) {
                                pos += written;
                            } else {
                                break;
                            }
                            f = f->prev;
                            frame_count++;
                        }
                        trace_buf[pos] = '\0';
                        
                        thread->exception_stack_trace = strdup(trace_buf);
                        
                        /* Save where exception was thrown */
                        const char* throw_cls = frame->clazz && frame->clazz->class_name ? 
                            frame->clazz->class_name : "?";
                        const char* throw_meth = frame->method && frame->method->name ? 
                            frame->method->name : "?";
                        const char* slash2 = strrchr(throw_cls, '/');
                        throw_cls = slash2 ? slash2 + 1 : throw_cls;
                        char info[256];
                        snprintf(info, sizeof(info), "%s.%s at PC=%d", throw_cls, throw_meth, frame->throwing_pc);
                        thread->exception_throw_info = strdup(info);
                    }
                    
                    return -1;  /* Caller will handle */
                }
            }
            /* Error without exception object */
            return -1;
        }
    }

    EXEC_DEBUG("interpret: Reached end of code");
    return 0;  /* Reached end of code */
}

/* Invoke virtual method */
int jvm_invoke_virtual(JVM* jvm, JavaObject* obj, const char* name, 
                       const char* descriptor, JavaValue* args, JavaValue* result) {
    if (!obj || !name || !descriptor) {
        ERROR_LOG("jvm_invoke_virtual: NULL parameter");
        return -1;
    }

    JavaClass* clazz = obj->header.clazz;
    if (!clazz) {
        ERROR_LOG("jvm_invoke_virtual: Object has no class");
        return -1;
    }

    EXEC_DEBUG("jvm_invoke_virtual: %s.%s%s", clazz->class_name, name, descriptor);

    /* Find method in class hierarchy */
    JavaMethod* method = jvm_resolve_method(jvm, clazz, name, descriptor);
    if (!method) {
        ERROR_LOG("Method not found: %s%s in %s", name, descriptor, clazz->class_name);
        return -1;
    }

    /* Prepare args with 'this' reference (use cached arg count) */
    int arg_count = method_arg_count(method);
    JavaValue* full_args = (JavaValue*)malloc((arg_count + 1) * sizeof(JavaValue));
    if (!full_args) return -1;

    full_args[0].ref = obj;
    if (args) {
        memcpy(full_args + 1, args, arg_count * sizeof(JavaValue));
    }

    JavaThread* thread = jvm_current_thread(jvm);
    int ret = execute_method(jvm, thread, method, full_args, result);

    free(full_args);
    return ret;
}

/* Invoke static method */
int jvm_invoke_static(JVM* jvm, JavaClass* clazz, const char* name,
                      const char* descriptor, JavaValue* args, JavaValue* result) {
    if (!clazz || !name || !descriptor) {
        ERROR_LOG("jvm_invoke_static: NULL parameter");
        return -1;
    }

    EXEC_DEBUG("jvm_invoke_static: %s.%s%s", clazz->class_name, name, descriptor);

    /* Find method */
    JavaMethod* method = jvm_resolve_method(jvm, clazz, name, descriptor);
    if (!method) {
        ERROR_LOG("Static method not found: %s%s in %s", name, descriptor, clazz->class_name);
        return -1;
    }

    JavaThread* thread = jvm_current_thread(jvm);
    return execute_method(jvm, thread, method, args, result);
}

/* Invoke special method (constructor, private, super) */
int jvm_invoke_special(JVM* jvm, JavaObject* obj, JavaClass* clazz,
                       const char* name, const char* descriptor,
                       JavaValue* args, JavaValue* result) {
    if (!clazz || !name || !descriptor) {
        ERROR_LOG("jvm_invoke_special: NULL parameter");
        return -1;
    }

    EXEC_DEBUG("jvm_invoke_special: %s.%s%s", clazz->class_name, name, descriptor);

    /* Find method */
    JavaMethod* method = jvm_resolve_method(jvm, clazz, name, descriptor);
    if (!method) {
        ERROR_LOG("Special method not found: %s%s in %s", name, descriptor, clazz->class_name);
        return -1;
    }

    /* Prepare args with 'this' reference (use cached arg count) */
    int arg_count = method_arg_count(method);
    JavaValue* full_args = (JavaValue*)malloc((arg_count + 1) * sizeof(JavaValue));
    if (!full_args) return -1;

    full_args[0].ref = obj;
    if (args) {
        memcpy(full_args + 1, args, arg_count * sizeof(JavaValue));
    }

    JavaThread* thread = jvm_current_thread(jvm);
    int ret = execute_method(jvm, thread, method, full_args, result);

    free(full_args);
    return ret;
}

/* Create new object and call constructor */
JavaObject* jvm_new_object_with_constructor(JVM* jvm, JavaClass* clazz,
                                            const char* descriptor,
                                            JavaValue* args) {
    if (!clazz) {
        ERROR_LOG("jvm_new_object_with_constructor: NULL class");
        return NULL;
    }

    EXEC_DEBUG("Creating object of class %s", clazz->class_name);

    /* Create object */
    JavaObject* obj = jvm_new_object(jvm, clazz);
    if (!obj) {
        ERROR_LOG("Failed to allocate object");
        return NULL;
    }

    /* Find constructor if descriptor provided */
    if (descriptor) {
        JavaMethod* init = jvm_resolve_method(jvm, clazz, "<init>", descriptor);
        if (init) {
            int arg_count = method_arg_count(init);
            JavaValue* full_args = (JavaValue*)malloc((arg_count + 1) * sizeof(JavaValue));
            if (full_args) {
                full_args[0].ref = obj;
                if (args) {
                    memcpy(full_args + 1, args, arg_count * sizeof(JavaValue));
                }

                JavaThread* thread = jvm_current_thread(jvm);
                JavaValue result;
                execute_method(jvm, thread, init, full_args, &result);

                free(full_args);
            }
        }
    }

    return obj;
}

/* Initialize a class and its superclass */
int jvm_init_class(JVM* jvm, JavaClass* clazz) {
    if (!clazz) return -1;

    /* 1. Если класс уже инициализирован */
    if (clazz->initialized) {
        return 0;
    }

    /* 2. Получаем текущий поток */
    JavaThread* current = jvm_current_thread(jvm);
    if (!current) return -1; /* Safety check */

    /* 3. Проверка на рекурсию (JVMS §5.5)
       Если класс уже инициализируется ЭТИМ потоком, разрешаем выполнение (return 0).
       Это происходит, когда <clinit> вызывает методы своего же класса. */
    if (clazz->initializing && clazz->initializing_thread == current) {
        return 0;
    }
    
    /* 4. Если класс инициализируется другим потоком (в однопоточном варианте не должно случаться) */
    if (clazz->initializing) {
        return 0; 
    }

    EXEC_DEBUG("Initializing class %s", clazz->class_name);
    EXEC_DEBUG("Class %s (is_stub=%d, instance_size=%zu)",
            clazz->class_name ? clazz->class_name : "?", clazz->is_stub ? 1 : 0, clazz->instance_size);

    /* 5. Инициализируем суперкласс */
    if (clazz->super_class && !clazz->super_class->initialized) {
        if (jvm_init_class(jvm, clazz->super_class) != 0) {
            return -1;
        }
    }
    
    /* === CRITICAL FIX: Recalculate instance_size after superclass is initialized ===
     * At class load time, superclass might not have been loaded yet.
     * Now that superclass is guaranteed to be loaded and initialized,
     * we can correctly calculate the total instance size.
     */
    jvm_recalculate_instance_size(jvm, clazz);

    /* === CRITICAL: Initialize static fields from class file ===
     * Static fields are parsed from the class file into clazz->fields[],
     * but need to be copied to clazz->static_fields[] for getstatic/putstatic.
     * This must be done BEFORE <clinit> runs.
     * 
     * IMPORTANT: Fields with same name but different descriptors are DIFFERENT fields!
     * This is valid in Java (obfuscated code often uses this).
     */
    if (clazz->fields && clazz->fields_count > 0) {
        /* Count static fields first */
        int static_count = 0;
        for (uint16_t i = 0; i < clazz->fields_count; i++) {
            if (clazz->fields[i].access_flags & ACC_STATIC) {
                static_count++;
            }
        }
        
        /* Allocate static fields storage if needed */
        if (static_count > 0) {
            if (!clazz->static_fields) {
                int capacity = static_count > 16 ? static_count : 16;
                clazz->static_fields = (JavaStaticField*)calloc(capacity, sizeof(JavaStaticField));
                clazz->static_fields_capacity = capacity;
                clazz->static_fields_count = 0;
            }
            
            /* Add each static field */
            for (uint16_t i = 0; i < clazz->fields_count; i++) {
                JavaField* field = &clazz->fields[i];
                if (!(field->access_flags & ACC_STATIC)) continue;
                
                /* Check if already exists */
                bool exists = false;
                for (int j = 0; j < clazz->static_fields_count; j++) {
                    if (clazz->static_fields[j].name && 
                        strcmp(clazz->static_fields[j].name, field->name) == 0 &&
                        clazz->static_fields[j].descriptor && field->descriptor &&
                        strcmp(clazz->static_fields[j].descriptor, field->descriptor) == 0) {
                        exists = true;
                        break;
                    }
                }
                
                if (!exists) {
                    /* Grow if needed */
                    if (clazz->static_fields_count >= clazz->static_fields_capacity) {
                        int new_cap = clazz->static_fields_capacity + 16;
                        JavaStaticField* new_fields = (JavaStaticField*)realloc(
                            clazz->static_fields, new_cap * sizeof(JavaStaticField));
                        if (!new_fields) continue;
                        memset(new_fields + clazz->static_fields_capacity, 0, 
                               16 * sizeof(JavaStaticField));
                        clazz->static_fields = new_fields;
                        clazz->static_fields_capacity = new_cap;
                    }
                    
                    int slot = clazz->static_fields_count++;
                    clazz->static_fields[slot].name = field->name ? strdup(field->name) : NULL;
                    clazz->static_fields[slot].descriptor = field->descriptor ? strdup(field->descriptor) : NULL;
                    clazz->static_fields[slot].value.raw = 0;  /* Default value */
                    
                    EXEC_DEBUG("Static field slot %d: %s %s", slot, 
                            field->name ? field->name : "?",
                            field->descriptor ? field->descriptor : "?");
                }
            }
            
            EXEC_DEBUG("Initialized %d static fields for %s", clazz->static_fields_count, 
                    clazz->class_name ? clazz->class_name : "?");
        }
    }

    /* 6. Устанавливаем флаг ПЕРЕД выполнением <clinit> */
    clazz->initializing = true;
    clazz->initializing_thread = current;

    /* 7. Запускаем <clinit> */
    JavaMethod* clinit = jvm_resolve_method(jvm, clazz, "<clinit>", "()V");
    if (clinit) {
        JavaValue result;
        /* execute_method вернет < 0 при исключении */
        int ret = execute_method(jvm, current, clinit, NULL, &result);
        
        if (ret != 0) {
            /* Error during initialization - mark class as initialized to prevent
             * infinite retry loops. Without this, every subsequent op_new/op_getstatic/
             * op_invokestatic would re-trigger <clinit>, consuming recursion depth
             * until the 500 limit is hit. Per JVMS §5.5, a failed initialization makes
             * the class erroneous; subsequent access should throw an error immediately
             * rather than re-running <clinit>. */
            clazz->initializing = false;
            clazz->initializing_thread = NULL;
            clazz->initialized = true;  /* Prevent retries - class is marked erroneous */
            ERROR_LOG("ExceptionInInitializerError for %s (marked as initialized to prevent retries)", clazz->class_name);
            return -1;
        }
    }

    /* 8. Завершаем инициализацию */
    clazz->initializing = false;
    clazz->initializing_thread = NULL;
    clazz->initialized = true;

    /* 9. SPECIAL CASE: Initialize AlertType static fields with instances */
    if (clazz->class_name && strcmp(clazz->class_name, "javax/microedition/lcdui/AlertType") == 0) {
        /* Create AlertType instances for each static field */
        if (clazz->static_fields && clazz->static_fields_count >= 5) {
            for (int i = 0; i < 5; i++) {
                /* Create a new AlertType object for each static field */
                JavaObject* alert_type_obj = jvm_new_object(jvm, clazz);
                if (alert_type_obj) {
                    clazz->static_fields[i].value.ref = alert_type_obj;
                    EXEC_DEBUG("Initialized AlertType.%s = %p", 
                              clazz->static_fields[i].name, (void*)alert_type_obj);
                }
            }
        }
    }

    return 0;
}

/* Main entry point - execute a MIDlet */
int jvm_run_midlet(JVM* jvm, JavaClass* main_class) {
    if (!jvm || !main_class) {
        ERROR_LOG("jvm_run_midlet: NULL parameter");
        return -1;
    }

    LOG_SAFE("[MIDLET] ========== STARTING MIDLET: %s ==========\n", 
            main_class->class_name ? main_class->class_name : "?");

    EXEC_DEBUG("Running MIDlet: %s", main_class->class_name);

    /* Initialize the class */
    LOG_SAFE("[MIDLET] Initializing class...\n");
    
    if (jvm_init_class(jvm, main_class) != 0) {
        ERROR_LOG("Failed to initialize class %s", main_class->class_name);
        return -1;
    }
    
    LOG_SAFE("[MIDLET] Class initialized OK\n");

    /* Find constructor */
    LOG_SAFE("[MIDLET] Finding constructor...\n");
    JavaMethod* constructor = jvm_resolve_method(jvm, main_class, "<init>", "()V");
    if (!constructor) {
        ERROR_LOG("No default constructor in %s", main_class->class_name);
        return -1;
    }
    
    LOG_SAFE("[MIDLET] Found constructor: %s%s (native=%d)\n", 
            constructor->name ? constructor->name : "?",
            constructor->descriptor ? constructor->descriptor : "?",
            constructor->is_native ? 1 : 0);

    /* Create MIDlet instance */
    LOG_SAFE("[MIDLET] Creating MIDlet instance...\n");
    JavaObject* midlet = jvm_new_object(jvm, main_class);
    if (!midlet) {
        ERROR_LOG("Failed to create MIDlet instance");
        return -1;
    }
    
    LOG_SAFE("[MIDLET] Created MIDlet instance: %p\n", (void*)midlet);

    /* Call constructor */
    JavaValue this_arg = { .ref = midlet };
    JavaThread* thread = jvm_current_thread(jvm);
    JavaValue result;
    
    LOG_SAFE("[MIDLET] Calling constructor...\n");
    
    if (execute_method(jvm, thread, constructor, &this_arg, &result) != 0) {
        ERROR_LOG("Constructor failed");
        if (thread->pending_exception) {
            JavaClass* exc_class = thread->pending_exception->header.clazz;
            LOG_SAFE("[MIDLET] Exception in constructor: %s\n",
                    exc_class ? exc_class->class_name : "?");
        }
        return -1;
    }
    
    LOG_SAFE("[MIDLET] Constructor completed OK\n");

    EXEC_DEBUG("MIDlet instance created successfully");

    /* Find startApp method */
    LOG_SAFE("[MIDLET] Finding startApp()...\n");
    JavaMethod* startApp = jvm_resolve_method(jvm, main_class, "startApp", "()V");
    if (!startApp) {
        ERROR_LOG("No startApp method in %s", main_class->class_name);
        /* This is not fatal - MIDlet might be abstract */
        return 0;
    }
    
    LOG_SAFE("[MIDLET] Found startApp: %s%s (native=%d, code_len=%u)\n",
            startApp->name ? startApp->name : "?",
            startApp->descriptor ? startApp->descriptor : "?",
            startApp->is_native ? 1 : 0,
            startApp->code.code_length);

    LOG_SAFE("[MIDLET] Calling startApp()...\n");
    
    if (execute_method(jvm, thread, startApp, &this_arg, &result) != 0) {
        ERROR_LOG("startApp failed");
        if (thread->pending_exception) {
            JavaClass* exc_class = thread->pending_exception->header.clazz;
            LOG_SAFE("[MIDLET] Exception in startApp: %s\n",
                    exc_class ? exc_class->class_name : "?");
        }
        /* Print saved exception info and stack trace for debugging */
        if (thread->exception_throw_info) {
            LOG_SAFE("[MIDLET] Exception thrown at: %s\n", thread->exception_throw_info);
        }
        if (thread->exception_stack_trace) {
            LOG_SAFE("[MIDLET] Stack trace at exception:\n%s", thread->exception_stack_trace);
        }
        
        /* COMPATIBILITY: Don't fail if startApp throws an exception.
         * Many J2ME games have DRM/copy protection (e.g., GlomoReg) that throws
         * exceptions when running on non-registered devices. In an emulator,
         * we should log the error but continue running the game.
         * The game's own initialization code after the DRM check will still execute
         * if the exception is properly caught/ignored at the JVM level. */
        LOG_SAFE("[MIDLET] startApp() threw exception - clearing and continuing (DRM bypass)\n");
        if (thread->pending_exception) {
            jvm_exception_clear(jvm);
        }
        thread->pending_exception = NULL;
        return 0;
    }
    
    LOG_SAFE("[MIDLET] startApp() completed OK\n");

    EXEC_DEBUG("MIDlet started successfully");
    LOG_SAFE("[MIDLET] ========== MIDLET STARTED ==========\n");
    return 0;
}

/* Execute one instruction from the current frame (for libretro step-by-step execution)
 * Returns: 0 = continue, >0 = method returned, <0 = error/exception
 * This is designed for step-by-step execution in libretro where we need to
 * periodically return control to the frontend.
 */
int execute_frame(JVM* jvm, JavaThread* thread) {
    if (!jvm || !thread) return -1;
    
    JavaFrame* frame = thread->current_frame;
    if (!frame || !frame->code) {
        return -1;  /* No frame to execute */
    }
    
    /* Check for pending exception */
    if (thread->pending_exception) {
        return -1;
    }
    
    /* Check if we've reached end of code */
    if (frame->pc >= frame->code_length) {
        return 1;  /* End of method */
    }
    
    /* Check if JVM is still running */
    if (!jvm->running) {
        return -1;
    }
    
    /* Save the PC for exception handling */
    frame->throwing_pc = frame->pc;
    
    /* Fetch and execute one instruction */
    uint8_t opcode = frame->code[frame->pc++];
    
    /* Opcode tracing - uses runtime debug flag (toggle with F12) */
    if (g_j2me_runtime_debug) {
        const char* op_name = opcode_table[opcode].name;
        const char* method_name = frame->method ? frame->method->name : "?";
        const char* class_name = (frame->clazz && frame->clazz->class_name) ? frame->clazz->class_name : "?";
        if (g_j2me_runtime_debug) LOG_SAFE("[OPCODE] %s.%s PC=%d: %s (0x%02X) [stack=%d/%d, locals=%d]\n",
                class_name, method_name,
                frame->pc - 1, 
                op_name ? op_name : "unknown", 
                opcode,
                frame->stack_top + 1,
                frame->max_stack,
                frame->max_locals);
    }
    
    /* Get opcode handler */
    OpcodeHandler handler = opcode_table[opcode].handler;
    if (!handler) {
        ERROR_LOG("Unknown opcode: 0x%02X at PC=%d", opcode, frame->pc - 1);
        return -1;
    }
    
    /* Execute opcode */
    int result = handler(jvm, thread, frame);
    
    /* result: 0 = continue, >0 = return, <0 = error/exception */
    return result;
}
