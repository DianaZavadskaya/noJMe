import { g_j2me_runtime_debug } from './debug_var.mjs';

import {
    jvm_load_class,
    jvm_new_object,
    jvm_resolve_method,
    jvm_current_thread,
    jvm_throw_by_name,
    jvm_exception_clear,
    jvm_recalculate_instance_size,
} from './jvm.mjs';

import {
    classfile_get_class_name,
} from './classfile.mjs';

import {
    object_instance_of,
} from './heap.mjs';

import {
    thread_yield,
    monitor_enter,
    monitor_exit,
    YIELD_INTERVAL,
    JNI_OK,
} from './threads.mjs';

// ─────────────────────────────────────────────────────────────
// ACC flags (mirrors opcodes.h)
// ─────────────────────────────────────────────────────────────
const ACC_STATIC       = 0x0008;
const ACC_SYNCHRONIZED = 0x0020;

// ─────────────────────────────────────────────────────────────
// Instruction counter — we keep our own copy that mirrors
// threads.mjs g_instruction_counter via the shared object
// injected through set_execute_callbacks.
// ─────────────────────────────────────────────────────────────
let _g_ic = { value: 0 };

// ─────────────────────────────────────────────────────────────
// Instruction limit (mirrors J2ME_MAX_INSTRUCTIONS env var)
// ─────────────────────────────────────────────────────────────
let _total_instructions = 0;
let _max_instructions   = 0;  // 0 = unlimited
let _env_checked        = false;

function _check_env() {
    if (_env_checked) return;
    _env_checked = true;
    const envVal = process.env['J2ME_MAX_INSTRUCTIONS'];
    if (envVal) {
        _max_instructions = parseInt(envVal, 10) || 0;
    }
}

// ─────────────────────────────────────────────────────────────
// Injected callbacks — break circular deps with native/opcode modules
// ─────────────────────────────────────────────────────────────

let _native_call      = null;  // (jvm, thread, method, args, result_out) => int
let _native_find      = null;  // (jvm, className, methodName, descriptor) => handler|null
let _method_arg_count = null;  // (method) => int
let _opcode_table     = null;  // OpcodeInfo[256] — { name, handler }

/**
 * Inject callbacks from the native/opcode modules to avoid circular imports.
 *
 * Expected shape:
 *   {
 *     nativeCall:          (jvm, thread, method, args, resultOut) => int,
 *     nativeFind:          (jvm, className, methodName, descriptor) => fn | null,
 *     methodArgCount:      (method) => int,
 *     opcodeTable:         Array(256) of { name, handler },
 *     gInstructionCounter: { value: number }  // shared mutable ref
 *   }
 */
export function set_execute_callbacks(cbs) {
    if (cbs.nativeCall)           _native_call       = cbs.nativeCall;
    if (cbs.nativeFind)           _native_find       = cbs.nativeFind;
    if (cbs.methodArgCount)       _method_arg_count  = cbs.methodArgCount;
    if (cbs.opcodeTable)          _opcode_table      = cbs.opcodeTable;
    if (cbs.gInstructionCounter)  _g_ic              = cbs.gInstructionCounter;
}

// ─────────────────────────────────────────────────────────────
// Frame pool — avoids repeated allocation (mirrors C FramePool)
// ─────────────────────────────────────────────────────────────
const FRAME_POOL_SIZE = 256;

const frame_pool = {
    frames:         new Array(FRAME_POOL_SIZE).fill(null),
    max_locals_arr: new Array(FRAME_POOL_SIZE).fill(0),
    max_stack_arr:  new Array(FRAME_POOL_SIZE).fill(0),
    count:          0,
};

// ─────────────────────────────────────────────────────────────
// frame_create
// ─────────────────────────────────────────────────────────────
function frame_create(jvm, method, clazz) {
    if (!method || !clazz) return null;

    const max_locals = method.code.max_locals || 0;
    const max_stack  = (method.code.max_stack  || 0) + 4;  // +4 safety buffer, mirrors C

    // Try pool reuse
    if (frame_pool.count > 0) {
        const idx   = frame_pool.count - 1;
        const frame = frame_pool.frames[idx];
        if (frame &&
            frame_pool.max_locals_arr[idx] >= max_locals &&
            frame_pool.max_stack_arr[idx]  >= max_stack) {
            frame_pool.count--;

            for (let i = 0; i < max_locals; i++) frame.locals[i] = { raw: 0n };
            for (let i = 0; i < max_stack;  i++) frame.stack[i]  = { raw: 0n };

            frame.pc                     = 0;
            frame.stack_top              = -1;
            frame.prev                   = null;
            frame.clazz                  = method.clazz || clazz;
            frame.method                 = method;
            frame.code                   = method.code.code;
            frame.code_length            = method.code.code_length;
            frame.max_locals             = max_locals;
            frame.max_stack              = max_stack;
            frame.throwing_pc            = 0;
            frame.exception_table        = method.code.exception_table;
            frame.exception_table_length = method.code.exception_table_length || 0;
            return frame;
        }
    }

    // Allocate fresh frame — GC owns it
    const locals = new Array(max_locals > 0 ? max_locals : 0);
    for (let i = 0; i < locals.length; i++) locals[i] = { raw: 0n };

    const stack = new Array(max_stack > 0 ? max_stack : 0);
    for (let i = 0; i < stack.length; i++) stack[i] = { raw: 0n };

    return {
        method,
        clazz:                  method.clazz || clazz,
        pc:                     0,
        code:                   method.code.code,
        code_length:            method.code.code_length,
        locals,
        max_locals,
        stack,
        stack_top:              -1,
        max_stack,
        exception_table:        method.code.exception_table,
        exception_table_length: method.code.exception_table_length || 0,
        throwing_pc:            0,
        prev:                   null,
    };
}

// ─────────────────────────────────────────────────────────────
// frame_destroy — return to pool or let GC reclaim
// ─────────────────────────────────────────────────────────────
function frame_destroy(frame) {
    if (!frame) return;
    if (frame_pool.count < FRAME_POOL_SIZE) {
        const idx = frame_pool.count++;
        frame_pool.frames[idx]         = frame;
        frame_pool.max_locals_arr[idx] = frame.max_locals;
        frame_pool.max_stack_arr[idx]  = frame.max_stack;
    }
}

// ─────────────────────────────────────────────────────────────
// push_frame / pop_frame
// ─────────────────────────────────────────────────────────────
function push_frame(jvm, thread, frame) {
    if (!thread || !frame) return -1;
    frame.prev           = thread.current_frame;
    thread.current_frame = frame;
    return 0;
}

function pop_frame(jvm, thread) {
    if (!thread || !thread.current_frame) return null;
    const frame          = thread.current_frame;
    thread.current_frame = frame.prev;
    return frame;
}

// ─────────────────────────────────────────────────────────────
// Argument count helpers
// ─────────────────────────────────────────────────────────────

function _count_args_from_descriptor(descriptor) {
    if (!descriptor) return 0;
    let i = 0;
    if (descriptor[i] === '(') i++;
    let count = 0;
    while (i < descriptor.length && descriptor[i] !== ')') {
        const ch = descriptor[i];
        if (ch === 'J' || ch === 'D') {
            count++; i++;
        } else if (ch === 'B' || ch === 'C' || ch === 'F' || ch === 'I' ||
                   ch === 'S' || ch === 'Z') {
            count++; i++;
        } else if (ch === 'L') {
            count++;
            while (i < descriptor.length && descriptor[i] !== ';') i++;
            if (i < descriptor.length) i++;
        } else if (ch === '[') {
            while (i < descriptor.length && descriptor[i] === '[') i++;
            if (i < descriptor.length && descriptor[i] === 'L') {
                while (i < descriptor.length && descriptor[i] !== ';') i++;
                if (i < descriptor.length) i++;
            } else {
                if (i < descriptor.length) i++;
            }
            count++;
        } else {
            i++;
        }
    }
    return count;
}

function get_arg_count(method) {
    if (_method_arg_count) return _method_arg_count(method);
    return _count_args_from_descriptor(method.descriptor);
}

// ─────────────────────────────────────────────────────────────
// execute_method — primary public entry point
// Mirrors int execute_method() in execute.c
// ─────────────────────────────────────────────────────────────

let recursion_depth = 0;
const MAX_RECURSION_DEPTH = 500;

export function execute_method(jvm, thread, method, args, result_out) {
    if (!jvm || !thread || !method) {
        process.stderr.write('[EXEC] execute_method: NULL parameter\n');
        return -1;
    }

    recursion_depth++;
    if (recursion_depth > MAX_RECURSION_DEPTH) {
        const cur = jvm_current_thread(jvm);
        if (cur && cur.current_frame) {
            process.stderr.write(`[RECURSION-ERROR] Call stack (depth=${recursion_depth}):\n`);
            let f = cur.current_frame;
            let shown = 0;
            while (f && shown < 20) {
                const cn = f.clazz ? (f.clazz.class_name || '?') : '?';
                const mn = f.method ? (f.method.name || '?') : '?';
                const md = f.method ? (f.method.descriptor || '') : '';
                process.stderr.write(`  [${shown}] ${cn}.${mn}${md}\n`);
                f = f.prev;
                shown++;
            }
        }
        recursion_depth--;
        process.stderr.write(`[EXEC] execute_method: Maximum recursion depth exceeded (${MAX_RECURSION_DEPTH})\n`);
        jvm_throw_by_name(jvm, 'java/lang/StackOverflowError', null);
        return -1;
    }

    // Native methods
    if (method.is_native) {
        recursion_depth--;
        if (!_native_call) {
            process.stderr.write(`[EXEC] native_call not injected for ${method.name}${method.descriptor}\n`);
            return -1;
        }
        return _native_call(jvm, thread, method, args, result_out);
    }

    // Stub-class native registry check (even if is_native flag is not set)
    if (_native_find && method.clazz && method.clazz.class_name &&
        _native_find(jvm, method.clazz.class_name, method.name, method.descriptor)) {
        recursion_depth--;
        return _native_call(jvm, thread, method, args, result_out);
    }

    if (!method.code || !method.code.code || method.code.code_length === 0) {
        process.stderr.write(`[EXEC] Method ${method.name} has no code\n`);
        recursion_depth--;
        return -1;
    }

    // Synchronized method — acquire monitor before execution
    const is_synchronized = (method.access_flags & ACC_SYNCHRONIZED) !== 0;
    const is_static        = (method.access_flags & ACC_STATIC) !== 0;
    let sync_obj = null;

    if (is_synchronized) {
        sync_obj = is_static
            ? (method.clazz || null)
            : (args && args[0] ? (args[0].ref || null) : null);

        if (sync_obj && monitor_enter(jvm, sync_obj) !== JNI_OK) {
            process.stderr.write(`[EXEC] Failed to enter monitor for ${method.name}\n`);
            recursion_depth--;
            return -1;
        }
    }

    const frame = frame_create(jvm, method, method.clazz);
    if (!frame) {
        process.stderr.write(`[EXEC] Failed to create frame for ${method.name}\n`);
        if (is_synchronized && sync_obj) monitor_exit(jvm, sync_obj);
        recursion_depth--;
        return -1;
    }

    // Copy arguments into frame locals, respecting wide types (J/D → 2 local slots)
    {
        let local_idx = 0;

        if (!is_static && args) {
            frame.locals[local_idx++] = args[0] || { raw: 0n };
        }

        const args_start = is_static ? 0 : 1;
        let arg_idx = args_start;
        const desc  = method.descriptor || '';
        let di      = 0;
        if (desc[di] === '(') di++;

        while (di < desc.length && desc[di] !== ')') {
            if (args && arg_idx < args.length) {
                frame.locals[local_idx] = args[arg_idx] || { raw: 0n };
            }

            const ch = desc[di];
            if (ch === 'J' || ch === 'D') {
                local_idx += 2; arg_idx++; di++;
            } else if (ch === 'B' || ch === 'C' || ch === 'F' || ch === 'I' ||
                       ch === 'S' || ch === 'Z') {
                local_idx++; arg_idx++; di++;
            } else if (ch === 'L') {
                local_idx++; arg_idx++;
                while (di < desc.length && desc[di] !== ';') di++;
                if (di < desc.length) di++;
            } else if (ch === '[') {
                local_idx++; arg_idx++;
                while (di < desc.length && desc[di] === '[') di++;
                if (di < desc.length && desc[di] === 'L') {
                    while (di < desc.length && desc[di] !== ';') di++;
                }
                if (di < desc.length) di++;
            } else {
                di++;
            }
        }
    }

    push_frame(jvm, thread, frame);

    const exec_result = interpret(jvm, thread, frame);

    // Return value: for non-native methods the op_*return handler pushes the
    // return value onto the caller's stack. After popping the callee frame the
    // caller is the new current_frame.
    if (result_out && exec_result === 0 && !method.is_native) {
        // Pop first so we can inspect the caller's stack
        pop_frame(jvm, thread);
        const caller = thread.current_frame;
        if (caller && caller.stack_top > 0) {
            Object.assign(result_out, caller.stack[caller.stack_top - 1]);
        }
    } else {
        pop_frame(jvm, thread);
    }

    if (is_synchronized && sync_obj) monitor_exit(jvm, sync_obj);

    frame_destroy(frame);
    recursion_depth--;
    return exec_result;
}

// ─────────────────────────────────────────────────────────────
// interpret — main bytecode dispatch loop
// No setjmp/longjmp; exceptions propagate via return values.
// Uses a labelled outer loop to emulate C's goto continue_execution.
// ─────────────────────────────────────────────────────────────
function interpret(jvm, thread, frame) {
    if (!frame || !frame.code) {
        process.stderr.write('[EXEC] interpret: NULL frame or code\n');
        return -1;
    }

    _check_env();

    outer: for (;;) {  // labelled loop — continue outer re-enters after exception redirect

        while (frame.pc < frame.code_length && jvm.running) {

            _total_instructions++;
            if (_max_instructions > 0 && _total_instructions > _max_instructions) {
                process.stderr.write(`[JVM] Instruction limit reached (${_total_instructions} instructions). Stopping.\n`);
                jvm.running = false;
                return -1;
            }

            frame.throwing_pc = frame.pc;
            const opcode = frame.code[frame.pc++];

            // Cooperative yield
            _g_ic.value++;
            if (_g_ic.value >= YIELD_INTERVAL) {
                _g_ic.value = 0;
                thread_yield(jvm);
            }

            if (g_j2me_runtime_debug) {
                const op_info     = _opcode_table ? _opcode_table[opcode] : null;
                const op_name     = op_info ? op_info.name : 'unknown';
                const method_name = frame.method ? (frame.method.name || '?') : '?';
                const class_name  = (frame.clazz && frame.clazz.class_name) ? frame.clazz.class_name : '?';
                process.stderr.write(
                    `[OPCODE] ${class_name}.${method_name} PC=${frame.pc - 1}: ${op_name} (0x${opcode.toString(16).padStart(2, '0')}) [stack=${frame.stack_top + 1}/${frame.max_stack}]\n`
                );
            }

            if (!_opcode_table) {
                process.stderr.write('[EXEC] opcode_table not injected\n');
                return -1;
            }

            const op_info = _opcode_table[opcode];
            const handler = op_info ? op_info.handler : null;
            if (!handler) {
                const cn = frame.clazz ? (frame.clazz.class_name || '?') : '?';
                const mn = frame.method ? (frame.method.name || '?') : '?';
                process.stderr.write(
                    `[EXEC] Unknown opcode: 0x${opcode.toString(16).padStart(2, '0')} at PC=${frame.pc - 1} in ${cn}.${mn}\n`
                );
                jvm_throw_by_name(jvm, 'java/lang/VirtualMachineError', 'Unknown opcode');
                return -1;
            }

            const result = handler(jvm, thread, frame);

            if (result > 0) {
                return 0;  // Normal method return (op_*return set result > 0)
            }

            if (result < 0) {
                if (!thread.pending_exception) {
                    const on = (_opcode_table && _opcode_table[opcode]) ? (_opcode_table[opcode].name || '?') : '?';
                    const cn = frame.clazz ? (frame.clazz.class_name || '?') : '?';
                    const mn = frame.method ? (frame.method.name || '?') : '?';
                    process.stderr.write(
                        `[JVM] OPCODE FAIL: opcode 0x${opcode.toString(16).padStart(2,'0')} (${on}) at PC=${frame.pc - 1} in ${cn}.${mn} returned ${result} WITHOUT exception!\n`
                    );
                    return -1;
                }

                // Exception dispatch
                const exception       = thread.pending_exception;
                const exception_class = (exception && exception.header) ? exception.header.clazz : null;

                if (g_j2me_runtime_debug) {
                    const ecn = exception_class ? (exception_class.class_name || '??') : '??';
                    const cn  = frame.clazz ? (frame.clazz.class_name || '?') : '?';
                    const mn  = frame.method ? (frame.method.name || '?') : '?';
                    process.stderr.write(
                        `[EXCEPTION] Exception ${ecn} occurred at PC=${frame.throwing_pc} in ${cn}.${mn}\n`
                    );
                    if (frame.exception_table) {
                        process.stderr.write(
                            `[EX-SEARCH] Throwing PC=${frame.throwing_pc} in ${cn}.${mn}, scanning ${frame.exception_table_length} entries\n`
                        );
                    } else {
                        process.stderr.write(
                            `[EX-SEARCH] Throwing PC=${frame.throwing_pc} in ${cn}.${mn}, NO exception_table (NULL)\n`
                        );
                    }
                }

                let handler_found = false;

                if (frame.exception_table && frame.exception_table_length > 0) {
                    for (let i = 0; i < frame.exception_table_length; i++) {
                        const entry = frame.exception_table[i];

                        if (g_j2me_runtime_debug) {
                            const catch_cls = (entry.catch_type !== 0)
                                ? classfile_get_class_name(frame.clazz, entry.catch_type)
                                : '<any/finally>';
                            const in_range = (frame.throwing_pc >= entry.start_pc &&
                                              frame.throwing_pc <  entry.end_pc);
                            process.stderr.write(
                                `[EX-SEARCH]   entry[${i}]: PC=${frame.throwing_pc} in [${entry.start_pc},${entry.end_pc})? ${in_range ? 'YES' : 'NO'} | handler_pc=${entry.handler_pc} catch_type=${entry.catch_type} (${catch_cls || '?'})\n`
                            );
                        }

                        if (frame.throwing_pc >= entry.start_pc &&
                            frame.throwing_pc <  entry.end_pc) {

                            if (entry.catch_type === 0) {
                                handler_found = true;
                            } else {
                                const catch_name = classfile_get_class_name(frame.clazz, entry.catch_type);
                                if (catch_name) {
                                    const catch_class = jvm_load_class(jvm, catch_name);
                                    if (catch_class && object_instance_of(exception, catch_class)) {
                                        handler_found = true;
                                    } else if (g_j2me_runtime_debug) {
                                        process.stderr.write(
                                            `[EX-SEARCH]     -> catch_type resolved to ${catch_name}, instance_of=${(catch_class && object_instance_of(exception, catch_class)) ? 'true' : 'false'}\n`
                                        );
                                    }
                                } else if (g_j2me_runtime_debug) {
                                    process.stderr.write(
                                        `[EX-SEARCH]     -> catch_type index ${entry.catch_type} could not be resolved!\n`
                                    );
                                }
                            }

                            if (handler_found) {
                                if (g_j2me_runtime_debug) {
                                    const ecn = exception_class ? (exception_class.class_name || '??') : '??';
                                    process.stderr.write(
                                        `[EXCEPTION] Handler found at PC=${entry.handler_pc} for ${ecn}\n`
                                    );
                                }

                                // Clear operand stack and push exception reference
                                frame.stack_top = 0;
                                frame.stack[0]  = { ref: exception };
                                thread.pending_exception     = null;
                                thread.exception_stack_trace = null;
                                thread.exception_throw_info  = null;

                                frame.pc = entry.handler_pc;
                                continue outer;  // Re-enter main loop from handler PC
                            }
                        }
                    }
                }

                if (!handler_found) {
                    // Build stack trace (only once per exception propagation)
                    if (!thread.exception_stack_trace) {
                        let trace = '';
                        let f     = frame;
                        let count = 0;
                        while (f && count < 20) {
                            const cls   = f.clazz && f.clazz.class_name ? f.clazz.class_name : '?';
                            const meth  = f.method && f.method.name ? f.method.name : '?';
                            const mdesc = f.method && f.method.descriptor ? f.method.descriptor : '';
                            const slash = cls.lastIndexOf('/');
                            const short_cls = slash >= 0 ? cls.slice(slash + 1) : cls;

                            if (count === 0) {
                                trace += `>${short_cls}.${meth}${mdesc} at PC=${f.throwing_pc}\n`;
                            } else {
                                trace += `  at ${short_cls}.${meth}${mdesc}\n`;
                            }
                            f = f.prev;
                            count++;
                        }
                        thread.exception_stack_trace = trace;

                        const throw_cls  = frame.clazz && frame.clazz.class_name ? frame.clazz.class_name : '?';
                        const throw_meth = frame.method && frame.method.name ? frame.method.name : '?';
                        const slash2     = throw_cls.lastIndexOf('/');
                        const short2     = slash2 >= 0 ? throw_cls.slice(slash2 + 1) : throw_cls;
                        thread.exception_throw_info = `${short2}.${throw_meth} at PC=${frame.throwing_pc}`;
                    }

                    if (g_j2me_runtime_debug) {
                        const ecn = exception_class ? (exception_class.class_name || '??') : '??';
                        const cn  = frame.clazz ? (frame.clazz.class_name || '?') : '?';
                        const mn  = frame.method ? (frame.method.name || '?') : '?';
                        process.stderr.write(
                            `[EXCEPTION] No handler found in ${cn}.${mn}, propagating exception ${ecn}\n`
                        );
                    }

                    return -1;  // Caller handles unwinding
                }

                return -1;  // Unreachable but satisfies JS linter
            }
        }

        // Reached end of bytecode normally
        return 0;
    }
}

// ─────────────────────────────────────────────────────────────
// jvm_invoke_virtual
// ─────────────────────────────────────────────────────────────
export function jvm_invoke_virtual(jvm, obj, name, descriptor, args, result_out) {
    if (!obj || !name || !descriptor) {
        process.stderr.write('[EXEC] jvm_invoke_virtual: NULL parameter\n');
        return -1;
    }

    const clazz = (obj.header && obj.header.clazz) ? obj.header.clazz : null;
    if (!clazz) {
        process.stderr.write('[EXEC] jvm_invoke_virtual: Object has no class\n');
        return -1;
    }

    const method = jvm_resolve_method(jvm, clazz, name, descriptor);
    if (!method) {
        process.stderr.write(`[EXEC] Method not found: ${name}${descriptor} in ${clazz.class_name}\n`);
        return -1;
    }

    const arg_count = get_arg_count(method);
    const full_args = new Array(arg_count + 1);
    full_args[0] = { ref: obj };
    if (args) {
        for (let i = 0; i < arg_count; i++) full_args[i + 1] = args[i] || { raw: 0n };
    }

    const thread = jvm_current_thread(jvm);
    return execute_method(jvm, thread, method, full_args, result_out);
}

// ─────────────────────────────────────────────────────────────
// jvm_invoke_static
// ─────────────────────────────────────────────────────────────
export function jvm_invoke_static(jvm, clazz, name, descriptor, args, result_out) {
    if (!clazz || !name || !descriptor) {
        process.stderr.write('[EXEC] jvm_invoke_static: NULL parameter\n');
        return -1;
    }

    const method = jvm_resolve_method(jvm, clazz, name, descriptor);
    if (!method) {
        process.stderr.write(`[EXEC] Static method not found: ${name}${descriptor} in ${clazz.class_name}\n`);
        return -1;
    }

    const thread = jvm_current_thread(jvm);
    return execute_method(jvm, thread, method, args, result_out);
}

// ─────────────────────────────────────────────────────────────
// jvm_invoke_special
// ─────────────────────────────────────────────────────────────
export function jvm_invoke_special(jvm, obj, clazz, name, descriptor, args, result_out) {
    if (!clazz || !name || !descriptor) {
        process.stderr.write('[EXEC] jvm_invoke_special: NULL parameter\n');
        return -1;
    }

    const method = jvm_resolve_method(jvm, clazz, name, descriptor);
    if (!method) {
        process.stderr.write(`[EXEC] Special method not found: ${name}${descriptor} in ${clazz.class_name}\n`);
        return -1;
    }

    const arg_count = get_arg_count(method);
    const full_args = new Array(arg_count + 1);
    full_args[0] = { ref: obj };
    if (args) {
        for (let i = 0; i < arg_count; i++) full_args[i + 1] = args[i] || { raw: 0n };
    }

    const thread = jvm_current_thread(jvm);
    return execute_method(jvm, thread, method, full_args, result_out);
}

// ─────────────────────────────────────────────────────────────
// jvm_new_object_with_constructor
// ─────────────────────────────────────────────────────────────
export function jvm_new_object_with_constructor(jvm, clazz, descriptor, args) {
    if (!clazz) {
        process.stderr.write('[EXEC] jvm_new_object_with_constructor: NULL class\n');
        return null;
    }

    const obj = jvm_new_object(jvm, clazz);
    if (!obj) {
        process.stderr.write('[EXEC] Failed to allocate object\n');
        return null;
    }

    if (descriptor) {
        const init = jvm_resolve_method(jvm, clazz, '<init>', descriptor);
        if (init) {
            const arg_count = get_arg_count(init);
            const full_args = new Array(arg_count + 1);
            full_args[0] = { ref: obj };
            if (args) {
                for (let i = 0; i < arg_count; i++) full_args[i + 1] = args[i] || { raw: 0n };
            }

            const thread = jvm_current_thread(jvm);
            execute_method(jvm, thread, init, full_args, {});
        }
    }

    return obj;
}

// ─────────────────────────────────────────────────────────────
// jvm_init_class — initialize a class and its superclass chain
// Mirrors jvm_init_class() in execute.c (JVMS §5.5)
// ─────────────────────────────────────────────────────────────
export function jvm_init_class(jvm, clazz) {
    if (!clazz) return -1;

    if (clazz.initialized) return 0;

    const current = jvm_current_thread(jvm);
    if (!current) return -1;

    // Re-entrancy guard (JVMS §5.5): allow same thread to proceed
    if (clazz.initializing && clazz.initializing_thread === current) return 0;
    if (clazz.initializing) return 0;

    // Initialize superclass first
    if (clazz.super_class && !clazz.super_class.initialized) {
        if (jvm_init_class(jvm, clazz.super_class) !== 0) return -1;
    }

    // Recalculate instance_size now that superclass is guaranteed loaded
    jvm_recalculate_instance_size(jvm, clazz);

    // Allocate static field storage
    if (clazz.fields && clazz.fields_count > 0) {
        let static_count = 0;
        for (let i = 0; i < clazz.fields_count; i++) {
            if (clazz.fields[i] && (clazz.fields[i].access_flags & ACC_STATIC)) static_count++;
        }

        if (static_count > 0) {
            if (!clazz.static_fields) {
                const capacity = Math.max(static_count, 16);
                clazz.static_fields = [];
                for (let k = 0; k < capacity; k++) {
                    clazz.static_fields.push({ name: null, descriptor: null, value: { raw: 0n } });
                }
                clazz.static_fields_capacity = capacity;
                clazz.static_fields_count    = 0;
            }

            for (let i = 0; i < clazz.fields_count; i++) {
                const field = clazz.fields[i];
                if (!field || !(field.access_flags & ACC_STATIC)) continue;

                let exists = false;
                for (let j = 0; j < clazz.static_fields_count; j++) {
                    const sf = clazz.static_fields[j];
                    if (sf && sf.name === field.name && sf.descriptor === field.descriptor) {
                        exists = true;
                        break;
                    }
                }

                if (!exists) {
                    if (clazz.static_fields_count >= clazz.static_fields_capacity) {
                        const new_cap = clazz.static_fields_capacity + 16;
                        for (let k = clazz.static_fields_capacity; k < new_cap; k++) {
                            clazz.static_fields.push({ name: null, descriptor: null, value: { raw: 0n } });
                        }
                        clazz.static_fields_capacity = new_cap;
                    }

                    const slot = clazz.static_fields_count++;
                    clazz.static_fields[slot].name       = field.name        || null;
                    clazz.static_fields[slot].descriptor = field.descriptor  || null;
                    clazz.static_fields[slot].value      = { raw: 0n };
                }
            }
        }
    }

    clazz.initializing        = true;
    clazz.initializing_thread = current;

    const clinit = jvm_resolve_method(jvm, clazz, '<clinit>', '()V');
    if (clinit) {
        const ret = execute_method(jvm, current, clinit, null, {});
        if (ret !== 0) {
            // Per JVMS §5.5: prevent infinite retry loops after failed init
            clazz.initializing        = false;
            clazz.initializing_thread = null;
            clazz.initialized         = true;
            process.stderr.write(
                `[EXEC] ExceptionInInitializerError for ${clazz.class_name} (marked initialized to prevent retries)\n`
            );
            return -1;
        }
    }

    clazz.initializing        = false;
    clazz.initializing_thread = null;
    clazz.initialized         = true;

    // SPECIAL CASE: AlertType static fields need actual object instances
    if (clazz.class_name === 'javax/microedition/lcdui/AlertType') {
        if (clazz.static_fields && clazz.static_fields_count >= 5) {
            for (let i = 0; i < 5; i++) {
                const alert_obj = jvm_new_object(jvm, clazz);
                if (alert_obj) {
                    clazz.static_fields[i].value = { ref: alert_obj };
                }
            }
        }
    }

    return 0;
}

// ─────────────────────────────────────────────────────────────
// jvm_run_midlet — top-level MIDlet lifecycle driver
// ─────────────────────────────────────────────────────────────
export function jvm_run_midlet(jvm, main_class) {
    if (!jvm || !main_class) {
        process.stderr.write('[MIDLET] jvm_run_midlet: NULL parameter\n');
        return -1;
    }

    const class_name = main_class.class_name || '?';
    process.stderr.write(`[MIDLET] ========== STARTING MIDLET: ${class_name} ==========\n`);

    process.stderr.write('[MIDLET] Initializing class...\n');
    if (jvm_init_class(jvm, main_class) !== 0) {
        process.stderr.write(`[MIDLET] Failed to initialize class ${class_name}\n`);
        return -1;
    }
    process.stderr.write('[MIDLET] Class initialized OK\n');

    process.stderr.write('[MIDLET] Finding constructor...\n');
    const constructor = jvm_resolve_method(jvm, main_class, '<init>', '()V');
    if (!constructor) {
        process.stderr.write(`[MIDLET] No default constructor in ${class_name}\n`);
        return -1;
    }
    process.stderr.write(
        `[MIDLET] Found constructor: ${constructor.name || '?'}${constructor.descriptor || '?'} (native=${constructor.is_native ? 1 : 0})\n`
    );

    process.stderr.write('[MIDLET] Creating MIDlet instance...\n');
    const midlet = jvm_new_object(jvm, main_class);
    if (!midlet) {
        process.stderr.write('[MIDLET] Failed to create MIDlet instance\n');
        return -1;
    }
    process.stderr.write('[MIDLET] Created MIDlet instance\n');

    const this_arg = { ref: midlet };
    const thread   = jvm_current_thread(jvm);

    process.stderr.write('[MIDLET] Calling constructor...\n');
    if (execute_method(jvm, thread, constructor, [this_arg], {}) !== 0) {
        process.stderr.write('[MIDLET] Constructor failed\n');
        if (thread.pending_exception) {
            const ec = thread.pending_exception.header ? thread.pending_exception.header.clazz : null;
            process.stderr.write(`[MIDLET] Exception in constructor: ${ec ? (ec.class_name || '?') : '?'}\n`);
        }
        return -1;
    }
    process.stderr.write('[MIDLET] Constructor completed OK\n');

    process.stderr.write('[MIDLET] Finding startApp()...\n');
    const startApp = jvm_resolve_method(jvm, main_class, 'startApp', '()V');
    if (!startApp) {
        process.stderr.write(`[MIDLET] No startApp method in ${class_name}\n`);
        return 0;  // Not fatal
    }
    process.stderr.write(
        `[MIDLET] Found startApp: ${startApp.name || '?'}${startApp.descriptor || '?'} (native=${startApp.is_native ? 1 : 0}, code_len=${startApp.code ? startApp.code.code_length : 0})\n`
    );

    process.stderr.write('[MIDLET] Calling startApp()...\n');
    if (execute_method(jvm, thread, startApp, [this_arg], {}) !== 0) {
        process.stderr.write('[MIDLET] startApp failed\n');
        if (thread.pending_exception) {
            const ec = thread.pending_exception.header ? thread.pending_exception.header.clazz : null;
            process.stderr.write(`[MIDLET] Exception in startApp: ${ec ? (ec.class_name || '?') : '?'}\n`);
        }
        if (thread.exception_throw_info) {
            process.stderr.write(`[MIDLET] Exception thrown at: ${thread.exception_throw_info}\n`);
        }
        if (thread.exception_stack_trace) {
            process.stderr.write(`[MIDLET] Stack trace at exception:\n${thread.exception_stack_trace}`);
        }

        // DRM bypass: many J2ME games throw on first run in untrusted environments
        process.stderr.write('[MIDLET] startApp() threw exception - clearing and continuing (DRM bypass)\n');
        if (thread.pending_exception) jvm_exception_clear(jvm);
        thread.pending_exception = null;
        return 0;
    }
    process.stderr.write('[MIDLET] startApp() completed OK\n');
    process.stderr.write('[MIDLET] ========== MIDLET STARTED ==========\n');
    return 0;
}

// ─────────────────────────────────────────────────────────────
// execute_frame — single-instruction step (for libretro driver)
// Returns: 0 = continue, >0 = method returned, <0 = error/exception
// ─────────────────────────────────────────────────────────────
export function execute_frame(jvm, thread) {
    if (!jvm || !thread) return -1;

    const frame = thread.current_frame;
    if (!frame || !frame.code) return -1;

    if (thread.pending_exception) return -1;

    if (frame.pc >= frame.code_length) return 1;

    if (!jvm.running) return -1;

    frame.throwing_pc = frame.pc;
    const opcode = frame.code[frame.pc++];

    if (g_j2me_runtime_debug) {
        const op_info     = _opcode_table ? _opcode_table[opcode] : null;
        const op_name     = op_info ? op_info.name : 'unknown';
        const method_name = frame.method ? (frame.method.name || '?') : '?';
        const class_name  = (frame.clazz && frame.clazz.class_name) ? frame.clazz.class_name : '?';
        process.stderr.write(
            `[OPCODE] ${class_name}.${method_name} PC=${frame.pc - 1}: ${op_name} (0x${opcode.toString(16).padStart(2, '0')}) [stack=${frame.stack_top + 1}/${frame.max_stack}, locals=${frame.max_locals}]\n`
        );
    }

    if (!_opcode_table) return -1;

    const op_info = _opcode_table[opcode];
    const handler = op_info ? op_info.handler : null;
    if (!handler) {
        process.stderr.write(
            `[EXEC] Unknown opcode: 0x${opcode.toString(16).padStart(2, '0')} at PC=${frame.pc - 1}\n`
        );
        return -1;
    }

    return handler(jvm, thread, frame);
}
