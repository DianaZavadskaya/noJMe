/**
 * js/jvm/opcodes.mjs
 * Complete ESM port of src/jvm/opcodes.c
 * All 256 Java bytecode opcode handlers.
 */

import { g_j2me_runtime_debug } from './debug_var.mjs';

// ── Opcode constants ──────────────────────────────────────────────────────────
export const OPC_NOP            = 0x00;
export const OPC_ACONST_NULL    = 0x01;
export const OPC_ICONST_M1      = 0x02;
export const OPC_ICONST_0       = 0x03;
export const OPC_ICONST_1       = 0x04;
export const OPC_ICONST_2       = 0x05;
export const OPC_ICONST_3       = 0x06;
export const OPC_ICONST_4       = 0x07;
export const OPC_ICONST_5       = 0x08;
export const OPC_LCONST_0       = 0x09;
export const OPC_LCONST_1       = 0x0A;
export const OPC_FCONST_0       = 0x0B;
export const OPC_FCONST_1       = 0x0C;
export const OPC_FCONST_2       = 0x0D;
export const OPC_DCONST_0       = 0x0E;
export const OPC_DCONST_1       = 0x0F;
export const OPC_BIPUSH         = 0x10;
export const OPC_SIPUSH         = 0x11;
export const OPC_LDC            = 0x12;
export const OPC_LDC_W          = 0x13;
export const OPC_LDC2_W         = 0x14;
export const OPC_ILOAD          = 0x15;
export const OPC_LLOAD          = 0x16;
export const OPC_FLOAD          = 0x17;
export const OPC_DLOAD          = 0x18;
export const OPC_ALOAD          = 0x19;
export const OPC_ILOAD_0        = 0x1A;
export const OPC_ILOAD_1        = 0x1B;
export const OPC_ILOAD_2        = 0x1C;
export const OPC_ILOAD_3        = 0x1D;
export const OPC_LLOAD_0        = 0x1E;
export const OPC_LLOAD_1        = 0x1F;
export const OPC_LLOAD_2        = 0x20;
export const OPC_LLOAD_3        = 0x21;
export const OPC_FLOAD_0        = 0x22;
export const OPC_FLOAD_1        = 0x23;
export const OPC_FLOAD_2        = 0x24;
export const OPC_FLOAD_3        = 0x25;
export const OPC_DLOAD_0        = 0x26;
export const OPC_DLOAD_1        = 0x27;
export const OPC_DLOAD_2        = 0x28;
export const OPC_DLOAD_3        = 0x29;
export const OPC_ALOAD_0        = 0x2A;
export const OPC_ALOAD_1        = 0x2B;
export const OPC_ALOAD_2        = 0x2C;
export const OPC_ALOAD_3        = 0x2D;
export const OPC_IALOAD         = 0x2E;
export const OPC_LALOAD         = 0x2F;
export const OPC_FALOAD         = 0x30;
export const OPC_DALOAD         = 0x31;
export const OPC_AALOAD         = 0x32;
export const OPC_BALOAD         = 0x33;
export const OPC_CALOAD         = 0x34;
export const OPC_SALOAD         = 0x35;
export const OPC_ISTORE         = 0x36;
export const OPC_LSTORE         = 0x37;
export const OPC_FSTORE         = 0x38;
export const OPC_DSTORE         = 0x39;
export const OPC_ASTORE         = 0x3A;
export const OPC_ISTORE_0       = 0x3B;
export const OPC_ISTORE_1       = 0x3C;
export const OPC_ISTORE_2       = 0x3D;
export const OPC_ISTORE_3       = 0x3E;
export const OPC_LSTORE_0       = 0x3F;
export const OPC_LSTORE_1       = 0x40;
export const OPC_LSTORE_2       = 0x41;
export const OPC_LSTORE_3       = 0x42;
export const OPC_FSTORE_0       = 0x43;
export const OPC_FSTORE_1       = 0x44;
export const OPC_FSTORE_2       = 0x45;
export const OPC_FSTORE_3       = 0x46;
export const OPC_DSTORE_0       = 0x47;
export const OPC_DSTORE_1       = 0x48;
export const OPC_DSTORE_2       = 0x49;
export const OPC_DSTORE_3       = 0x4A;
export const OPC_ASTORE_0       = 0x4B;
export const OPC_ASTORE_1       = 0x4C;
export const OPC_ASTORE_2       = 0x4D;
export const OPC_ASTORE_3       = 0x4E;
export const OPC_IASTORE        = 0x4F;
export const OPC_LASTORE        = 0x50;
export const OPC_FASTORE        = 0x51;
export const OPC_DASTORE        = 0x52;
export const OPC_AASTORE        = 0x53;
export const OPC_BASTORE        = 0x54;
export const OPC_CASTORE        = 0x55;
export const OPC_SASTORE        = 0x56;
export const OPC_POP            = 0x57;
export const OPC_POP2           = 0x58;
export const OPC_DUP            = 0x59;
export const OPC_DUP_X1         = 0x5A;
export const OPC_DUP_X2         = 0x5B;
export const OPC_DUP2           = 0x5C;
export const OPC_DUP2_X1        = 0x5D;
export const OPC_DUP2_X2        = 0x5E;
export const OPC_SWAP           = 0x5F;
export const OPC_IADD           = 0x60;
export const OPC_LADD           = 0x61;
export const OPC_FADD           = 0x62;
export const OPC_DADD           = 0x63;
export const OPC_ISUB           = 0x64;
export const OPC_LSUB           = 0x65;
export const OPC_FSUB           = 0x66;
export const OPC_DSUB           = 0x67;
export const OPC_IMUL           = 0x68;
export const OPC_LMUL           = 0x69;
export const OPC_FMUL           = 0x6A;
export const OPC_DMUL           = 0x6B;
export const OPC_IDIV           = 0x6C;
export const OPC_LDIV           = 0x6D;
export const OPC_FDIV           = 0x6E;
export const OPC_DDIV           = 0x6F;
export const OPC_IREM           = 0x70;
export const OPC_LREM           = 0x71;
export const OPC_FREM           = 0x72;
export const OPC_DREM           = 0x73;
export const OPC_INEG           = 0x74;
export const OPC_LNEG           = 0x75;
export const OPC_FNEG           = 0x76;
export const OPC_DNEG           = 0x77;
export const OPC_ISHL           = 0x78;
export const OPC_LSHL           = 0x79;
export const OPC_ISHR           = 0x7A;
export const OPC_LSHR           = 0x7B;
export const OPC_IUSHR          = 0x7C;
export const OPC_LUSHR          = 0x7D;
export const OPC_IAND           = 0x7E;
export const OPC_LAND           = 0x7F;
export const OPC_IOR            = 0x80;
export const OPC_LOR            = 0x81;
export const OPC_IXOR           = 0x82;
export const OPC_LXOR           = 0x83;
export const OPC_IINC           = 0x84;
export const OPC_I2L            = 0x85;
export const OPC_I2F            = 0x86;
export const OPC_I2D            = 0x87;
export const OPC_L2I            = 0x88;
export const OPC_L2F            = 0x89;
export const OPC_L2D            = 0x8A;
export const OPC_F2I            = 0x8B;
export const OPC_F2L            = 0x8C;
export const OPC_F2D            = 0x8D;
export const OPC_D2I            = 0x8E;
export const OPC_D2L            = 0x8F;
export const OPC_D2F            = 0x90;
export const OPC_I2B            = 0x91;
export const OPC_I2C            = 0x92;
export const OPC_I2S            = 0x93;
export const OPC_LCMP           = 0x94;
export const OPC_FCMPL          = 0x95;
export const OPC_FCMPG          = 0x96;
export const OPC_DCMPL          = 0x97;
export const OPC_DCMPG          = 0x98;
export const OPC_IFEQ           = 0x99;
export const OPC_IFNE           = 0x9A;
export const OPC_IFLT           = 0x9B;
export const OPC_IFGE           = 0x9C;
export const OPC_IFGT           = 0x9D;
export const OPC_IFLE           = 0x9E;
export const OPC_IF_ICMPEQ      = 0x9F;
export const OPC_IF_ICMPNE      = 0xA0;
export const OPC_IF_ICMPLT      = 0xA1;
export const OPC_IF_ICMPGE      = 0xA2;
export const OPC_IF_ICMPGT      = 0xA3;
export const OPC_IF_ICMPLE      = 0xA4;
export const OPC_IF_ACMPEQ      = 0xA5;
export const OPC_IF_ACMPNE      = 0xA6;
export const OPC_GOTO           = 0xA7;
export const OPC_JSR            = 0xA8;
export const OPC_RET            = 0xA9;
export const OPC_TABLESWITCH    = 0xAA;
export const OPC_LOOKUPSWITCH   = 0xAB;
export const OPC_IRETURN        = 0xAC;
export const OPC_LRETURN        = 0xAD;
export const OPC_FRETURN        = 0xAE;
export const OPC_DRETURN        = 0xAF;
export const OPC_ARETURN        = 0xB0;
export const OPC_RETURN         = 0xB1;
export const OPC_GETSTATIC      = 0xB2;
export const OPC_PUTSTATIC      = 0xB3;
export const OPC_GETFIELD       = 0xB4;
export const OPC_PUTFIELD       = 0xB5;
export const OPC_INVOKEVIRTUAL  = 0xB6;
export const OPC_INVOKESPECIAL  = 0xB7;
export const OPC_INVOKESTATIC   = 0xB8;
export const OPC_INVOKEINTERFACE= 0xB9;
export const OPC_NEW            = 0xBB;
export const OPC_NEWARRAY       = 0xBC;
export const OPC_ANEWARRAY      = 0xBD;
export const OPC_ARRAYLENGTH    = 0xBE;
export const OPC_ATHROW         = 0xBF;
export const OPC_CHECKCAST      = 0xC0;
export const OPC_INSTANCEOF     = 0xC1;
export const OPC_MONITORENTER   = 0xC2;
export const OPC_MONITOREXIT    = 0xC3;
export const OPC_WIDE           = 0xC4;
export const OPC_MULTIANEWARRAY = 0xC5;
export const OPC_IFNULL         = 0xC6;
export const OPC_IFNONNULL      = 0xC7;
export const OPC_GOTO_W         = 0xC8;
export const OPC_JSR_W          = 0xC9;

// Constant pool tags
const CONSTANT_Utf8               = 1;
const CONSTANT_Integer            = 3;
const CONSTANT_Float              = 4;
const CONSTANT_Long               = 5;
const CONSTANT_Double             = 6;
const CONSTANT_Class              = 7;
const CONSTANT_String             = 8;
const CONSTANT_Fieldref           = 9;
const CONSTANT_Methodref          = 10;
const CONSTANT_InterfaceMethodref = 11;
const CONSTANT_NameAndType        = 12;

// Array type codes (mirrors heap.mjs T_* constants)
const T_BOOLEAN = 4;
const T_CHAR    = 5;
const T_FLOAT   = 6;
const T_DOUBLE  = 7;
const T_BYTE    = 8;
const T_SHORT   = 9;
const T_INT     = 10;
const T_LONG    = 11;
const DESC_OBJECT = 0x4C; // 'L'
const DESC_ARRAY  = 0x5B; // '['
const ACC_STATIC  = 0x0008;

// ── Injected callbacks ────────────────────────────────────────────────────────
let _execute_method          = null;
let _native_find             = null;
let _count_args              = null;
let _string_utf8             = null;
let _native_intern_find_by_utf8 = null;
let _native_intern_string    = null;
let _classfile_get_utf8      = null;
let _classfile_get_class_name = null;
let _classfile_get_name_and_type = null;
let _jvm_load_class          = null;
let _jvm_init_class          = null;
let _jvm_new_object          = null;
let _jvm_new_array           = null;
let _jvm_new_string          = null;
let _jvm_throw               = null;
let _jvm_throw_by_name       = null;
let _jvm_exception_pending   = null;
let _jvm_resolve_method      = null;
let _object_instance_of      = null;
let _object_get_class        = null;
let _array_get               = null;
let _array_set               = null;
let _array_set_ref           = null;
let _native_throw_npe        = null;
let _native_throw_aioobe     = null;
let _native_throw_cnfe       = null;
let _native_throw_oome       = null;
let _native_throw_negative_array_size = null;
let _native_throw_array_store_exception = null;
let _native_throw_iae        = null;
let _monitor_enter           = null;
let _monitor_exit            = null;
let _gc_pin                  = null;
let _gc_unpin                = null;
let _drm_is_drm_class        = null;
let _is_managed_object       = null;

export function set_opcodes_callbacks(cbs) {
    if (cbs.execute_method)          _execute_method          = cbs.execute_method;
    if (cbs.native_find)             _native_find             = cbs.native_find;
    if (cbs.count_args)              _count_args              = cbs.count_args;
    if (cbs.string_utf8)             _string_utf8             = cbs.string_utf8;
    if (cbs.native_intern_find_by_utf8) _native_intern_find_by_utf8 = cbs.native_intern_find_by_utf8;
    if (cbs.native_intern_string)    _native_intern_string    = cbs.native_intern_string;
    if (cbs.classfile_get_utf8)      _classfile_get_utf8      = cbs.classfile_get_utf8;
    if (cbs.classfile_get_class_name) _classfile_get_class_name = cbs.classfile_get_class_name;
    if (cbs.classfile_get_name_and_type) _classfile_get_name_and_type = cbs.classfile_get_name_and_type;
    if (cbs.jvm_load_class)          _jvm_load_class          = cbs.jvm_load_class;
    if (cbs.jvm_init_class)          _jvm_init_class          = cbs.jvm_init_class;
    if (cbs.jvm_new_object)          _jvm_new_object          = cbs.jvm_new_object;
    if (cbs.jvm_new_array)           _jvm_new_array           = cbs.jvm_new_array;
    if (cbs.jvm_new_string)          _jvm_new_string          = cbs.jvm_new_string;
    if (cbs.jvm_throw)               _jvm_throw               = cbs.jvm_throw;
    if (cbs.jvm_throw_by_name)       _jvm_throw_by_name       = cbs.jvm_throw_by_name;
    if (cbs.jvm_exception_pending)   _jvm_exception_pending   = cbs.jvm_exception_pending;
    if (cbs.jvm_resolve_method)      _jvm_resolve_method      = cbs.jvm_resolve_method;
    if (cbs.object_instance_of)      _object_instance_of      = cbs.object_instance_of;
    if (cbs.object_get_class)        _object_get_class        = cbs.object_get_class;
    if (cbs.array_get)               _array_get               = cbs.array_get;
    if (cbs.array_set)               _array_set               = cbs.array_set;
    if (cbs.array_set_ref)           _array_set_ref           = cbs.array_set_ref;
    if (cbs.native_throw_npe)        _native_throw_npe        = cbs.native_throw_npe;
    if (cbs.native_throw_aioobe)     _native_throw_aioobe     = cbs.native_throw_aioobe;
    if (cbs.native_throw_cnfe)       _native_throw_cnfe       = cbs.native_throw_cnfe;
    if (cbs.native_throw_oome)       _native_throw_oome       = cbs.native_throw_oome;
    if (cbs.native_throw_negative_array_size) _native_throw_negative_array_size = cbs.native_throw_negative_array_size;
    if (cbs.native_throw_array_store_exception) _native_throw_array_store_exception = cbs.native_throw_array_store_exception;
    if (cbs.native_throw_iae)        _native_throw_iae        = cbs.native_throw_iae;
    if (cbs.monitor_enter)           _monitor_enter           = cbs.monitor_enter;
    if (cbs.monitor_exit)            _monitor_exit            = cbs.monitor_exit;
    if (cbs.gc_pin)                  _gc_pin                  = cbs.gc_pin;
    if (cbs.gc_unpin)                _gc_unpin                = cbs.gc_unpin;
    if (cbs.drm_is_drm_class)        _drm_is_drm_class        = cbs.drm_is_drm_class;
    if (cbs.is_managed_object)       _is_managed_object       = cbs.is_managed_object;
}

// ── Frame helpers ─────────────────────────────────────────────────────────────

function fetch_u1(frame) {
    return frame.code[frame.pc++] & 0xFF;
}

function fetch_u2(frame) {
    const hi = frame.code[frame.pc++] & 0xFF;
    const lo = frame.code[frame.pc++] & 0xFF;
    return (hi << 8) | lo;
}

function fetch_s1(frame) {
    const v = frame.code[frame.pc++] & 0xFF;
    return v >= 0x80 ? v - 0x100 : v;
}

function fetch_s2(frame) {
    const v = fetch_u2(frame);
    return v >= 0x8000 ? v - 0x10000 : v;
}

function read_u32(code, pc) {
    return (((code[pc] & 0xFF) << 24) | ((code[pc+1] & 0xFF) << 16) |
            ((code[pc+2] & 0xFF) << 8)  |  (code[pc+3] & 0xFF)) >>> 0;
}

function push(frame, val) {
    frame.stack[++frame.stack_top] = val;
}

function pop(frame) {
    if (frame.stack_top < 0) return { i: 0, j: 0n, f: 0, d: 0, ref: null };
    return frame.stack[frame.stack_top--];
}

function peek(frame) {
    return frame.stack[frame.stack_top];
}

function load_local(frame, idx) {
    if (idx >= frame.max_locals) return null;
    return frame.locals[idx];
}

function store_local(frame, idx, val) {
    if (idx >= frame.max_locals) return false;
    frame.locals[idx] = val;
    return true;
}

// ── Field hierarchy helpers ───────────────────────────────────────────────────

function count_instance_fields(clazz) {
    let count = 0;
    if (clazz.fields) {
        for (let i = 0; i < clazz.fields.length; i++) {
            const f = clazz.fields[i];
            if (f.access_flags & ACC_STATIC) continue;
            count++;
            if (f.descriptor && (f.descriptor[0] === 'J' || f.descriptor[0] === 'D')) count++;
        }
    }
    return count;
}

function count_instance_fields_before(clazz, field_index) {
    let count = 0;
    if (clazz.fields) {
        for (let i = 0; i < field_index; i++) {
            const f = clazz.fields[i];
            if (f.access_flags & ACC_STATIC) continue;
            count++;
            if (f.descriptor && (f.descriptor[0] === 'J' || f.descriptor[0] === 'D')) count++;
        }
    }
    return count;
}

function build_hierarchy(obj_class) {
    const hierarchy = [];
    let c = obj_class;
    while (c) { hierarchy.push(c); c = c.super_class; }
    hierarchy.reverse();
    return hierarchy;
}

function find_field_in_hierarchy(obj_class, field_name, descriptor) {
    const hierarchy = build_hierarchy(obj_class);
    for (let h = 0; h < hierarchy.length; h++) {
        const current = hierarchy[h];
        if (!current.fields) continue;
        for (let i = 0; i < current.fields.length; i++) {
            const f = current.fields[i];
            if (f.access_flags & ACC_STATIC) continue;
            if (!f.name || f.name !== field_name) continue;
            if (descriptor && f.descriptor && f.descriptor !== descriptor) continue;
            let slot = 0;
            for (let j = 0; j < h; j++) slot += count_instance_fields(hierarchy[j]);
            slot += count_instance_fields_before(current, i);
            return { defining_class: current, field_index: i, slot };
        }
    }
    return { defining_class: null, field_index: -1, slot: -1 };
}

// ── Constant pool helpers ─────────────────────────────────────────────────────

function cp_get_fieldref(frame, index) {
    const clazz = frame.clazz;
    if (!clazz || !clazz.constant_pool) return null;
    if (index <= 0 || index >= clazz.constant_pool_count) return null;
    const entry = clazz.constant_pool[index];
    if (entry.tag !== CONSTANT_Fieldref) return null;
    const class_name = _classfile_get_class_name(clazz, entry.info.class_index);
    const nat = _classfile_get_name_and_type(clazz, entry.info.name_and_type_index);
    return { class_name, field_name: nat.name, descriptor: nat.descriptor };
}

function cp_get_methodref(frame, index, allowed_tags) {
    const clazz = frame.clazz;
    if (!clazz || !clazz.constant_pool) return null;
    if (index <= 0 || index >= clazz.constant_pool_count) return null;
    const entry = clazz.constant_pool[index];
    if (allowed_tags && !allowed_tags.includes(entry.tag)) return null;
    const class_name = _classfile_get_class_name(clazz, entry.info.class_index);
    const nat = _classfile_get_name_and_type(clazz, entry.info.name_and_type_index);
    return { class_name, method_name: nat.name, descriptor: nat.descriptor };
}

// ── Return type helper ────────────────────────────────────────────────────────
function ret_char(descriptor) {
    if (!descriptor) return 'V';
    const idx = descriptor.lastIndexOf(')');
    return idx >= 0 ? descriptor[idx + 1] : 'V';
}

// ── Math helpers ──────────────────────────────────────────────────────────────
function to_int32(v) { return v | 0; }
function to_uint32(v) { return v >>> 0; }
function fmod(a, b) {
    if (b === 0) return NaN;
    return a - Math.trunc(a / b) * b;
}

// ── JavaValue factory ─────────────────────────────────────────────────────────
function jv_i(i)   { return { i: i | 0,    j: 0n,   f: 0, d: 0, ref: null }; }
function jv_j(j)   { return { i: 0,         j: j,    f: 0, d: 0, ref: null }; }
function jv_f(f)   { return { i: 0,         j: 0n,   f: f, d: 0, ref: null }; }
function jv_d(d)   { return { i: 0,         j: 0n,   f: 0, d: d, ref: null }; }
function jv_ref(r) { return { i: 0,         j: 0n,   f: 0, d: 0, ref: r    }; }
function jv_zero() { return { i: 0,         j: 0n,   f: 0, d: 0, ref: null }; }

// ── GC pin/unpin helpers ──────────────────────────────────────────────────────
function pin_args(jvm, args, count) {
    if (!_gc_pin || !_is_managed_object) return;
    for (let i = 0; i < count; i++) {
        if (args[i] && args[i].ref && _is_managed_object(args[i].ref)) _gc_pin(jvm, args[i].ref);
    }
}
function unpin_args(jvm, args, count) {
    if (!_gc_unpin || !_is_managed_object) return;
    for (let i = 0; i < count; i++) {
        if (args[i] && args[i].ref && _is_managed_object(args[i].ref)) _gc_unpin(jvm, args[i].ref);
    }
}

// ── Native dispatch helper ────────────────────────────────────────────────────
function push_return(frame, result, descriptor) {
    const rc = ret_char(descriptor);
    if (rc && rc !== 'V') {
        push(frame, result);
        if (rc === 'J' || rc === 'D') push(frame, result);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Opcode handlers
// ═════════════════════════════════════════════════════════════════════════════

export function op_nop() { return 0; }

export function op_aconst_null(jvm, thread, frame) {
    push(frame, jv_ref(null));
    return 0;
}

export function op_iconst(jvm, thread, frame) {
    const opcode = frame.code[frame.pc - 1] & 0xFF;
    push(frame, jv_i(opcode - OPC_ICONST_0));  // -1 to 5
    return 0;
}

export function op_lconst(jvm, thread, frame) {
    const opcode = frame.code[frame.pc - 1] & 0xFF;
    const v = jv_j(BigInt(opcode - OPC_LCONST_0));
    push(frame, v);
    push(frame, v);
    return 0;
}

export function op_fconst(jvm, thread, frame) {
    const opcode = frame.code[frame.pc - 1] & 0xFF;
    push(frame, jv_f(opcode - OPC_FCONST_0));
    return 0;
}

export function op_dconst(jvm, thread, frame) {
    const opcode = frame.code[frame.pc - 1] & 0xFF;
    const v = jv_d(opcode - OPC_DCONST_0);
    push(frame, v);
    push(frame, v);
    return 0;
}

export function op_bipush(jvm, thread, frame) {
    push(frame, jv_i(fetch_s1(frame)));
    return 0;
}

export function op_sipush(jvm, thread, frame) {
    push(frame, jv_i(fetch_s2(frame)));
    return 0;
}

function _ldc_resolve(jvm, frame, index) {
    const clazz = frame.clazz;
    if (!clazz || !clazz.constant_pool) return null;
    if (index <= 0 || index >= clazz.constant_pool_count) return null;
    const entry = clazz.constant_pool[index];
    switch (entry.tag) {
        case CONSTANT_Integer: return jv_i(entry.info.value | 0);
        case CONSTANT_Float:   return jv_f(entry.info.value);
        case CONSTANT_String: {
            const str = _classfile_get_utf8(clazz, entry.info.string_index);
            if (!str) return null;
            const interned = _native_intern_find_by_utf8 && _native_intern_find_by_utf8(str, str.length);
            if (interned) return jv_ref(interned);
            const ns = _jvm_new_string(jvm, str);
            if (!ns) return null;
            return jv_ref(_native_intern_string ? _native_intern_string(jvm, ns) : ns);
        }
        case CONSTANT_Class: {
            const name = _classfile_get_utf8(clazz, entry.info.name_index);
            if (!name) return null;
            return jv_ref(_jvm_load_class(jvm, name));
        }
        default: return null;
    }
}

export function op_ldc(jvm, thread, frame) {
    const index = fetch_u1(frame);
    const v = _ldc_resolve(jvm, frame, index);
    if (!v) return -1;
    push(frame, v);
    return 0;
}

export function op_ldc_w(jvm, thread, frame) {
    const index = fetch_u2(frame);
    const v = _ldc_resolve(jvm, frame, index);
    if (!v) return -1;
    push(frame, v);
    return 0;
}

export function op_ldc2_w(jvm, thread, frame) {
    const index = fetch_u2(frame);
    const clazz = frame.clazz;
    if (!clazz || !clazz.constant_pool) return -1;
    if (index <= 0 || index >= clazz.constant_pool_count) return -1;
    const entry = clazz.constant_pool[index];
    let v;
    if (entry.tag === CONSTANT_Long)   v = jv_j(BigInt.asIntN(64, BigInt(entry.info.value)));
    else if (entry.tag === CONSTANT_Double) v = jv_d(entry.info.value);
    else return -1;
    push(frame, v);
    push(frame, v);
    return 0;
}

// Load opcodes
export function op_iload(jvm, thread, frame) {
    const idx = fetch_u1(frame);
    const v = load_local(frame, idx);
    if (!v) return -1;
    push(frame, v);
    return 0;
}

export function op_lload(jvm, thread, frame) {
    const idx = fetch_u1(frame);
    const v1 = load_local(frame, idx);
    const v2 = load_local(frame, idx + 1);
    if (!v1 || !v2) return -1;
    push(frame, v1); push(frame, v2);
    return 0;
}

export function op_fload(jvm, thread, frame) { return op_iload(jvm, thread, frame); }
export function op_dload(jvm, thread, frame) { return op_lload(jvm, thread, frame); }
export function op_aload(jvm, thread, frame) { return op_iload(jvm, thread, frame); }

function _load_n(frame, n) {
    const v = load_local(frame, n);
    if (!v) return -1;
    push(frame, v);
    return 0;
}
function _lload_n(frame, n) {
    const v1 = load_local(frame, n);
    const v2 = load_local(frame, n + 1);
    if (!v1 || !v2) return -1;
    push(frame, v1); push(frame, v2);
    return 0;
}

export function op_load_0(jvm, thread, frame) { return _load_n(frame, 0); }
export function op_load_1(jvm, thread, frame) { return _load_n(frame, 1); }
export function op_load_2(jvm, thread, frame) { return _load_n(frame, 2); }
export function op_load_3(jvm, thread, frame) { return _load_n(frame, 3); }

export function op_lload_0(jvm, thread, frame) { return _lload_n(frame, 0); }
export function op_lload_1(jvm, thread, frame) { return _lload_n(frame, 1); }
export function op_lload_2(jvm, thread, frame) { return _lload_n(frame, 2); }
export function op_lload_3(jvm, thread, frame) { return _lload_n(frame, 3); }

export function op_dload_0(jvm, thread, frame) { return _lload_n(frame, 0); }
export function op_dload_1(jvm, thread, frame) { return _lload_n(frame, 1); }
export function op_dload_2(jvm, thread, frame) { return _lload_n(frame, 2); }
export function op_dload_3(jvm, thread, frame) { return _lload_n(frame, 3); }

export function op_array_load(jvm, thread, frame) {
    const opcode = frame.code[frame.pc - 1] & 0xFF;
    const index = pop(frame).i;
    const array = pop(frame).ref;
    if (!array) {
        const caller = frame.clazz && frame.clazz.class_name;
        if (caller && _drm_is_drm_class && _drm_is_drm_class(caller)) {
            push(frame, jv_zero());
            if (opcode === OPC_LALOAD || opcode === OPC_DALOAD) push(frame, jv_zero());
            return 0;
        }
        _native_throw_npe(jvm, thread);
        return -1;
    }
    if (index < 0 || index >= array.length) {
        _native_throw_aioobe(jvm, thread, index);
        return -1;
    }
    const v = _array_get(array, index);
    push(frame, v);
    if (opcode === OPC_LALOAD || opcode === OPC_DALOAD) push(frame, v);
    return 0;
}

// Store opcodes
export function op_store(jvm, thread, frame) {
    const idx = fetch_u1(frame);
    const v = pop(frame);
    return store_local(frame, idx, v) ? 0 : -1;
}

export function op_lstore(jvm, thread, frame) {
    const idx = fetch_u1(frame);
    const high = pop(frame);
    const low  = pop(frame);
    if (!store_local(frame, idx, low)) return -1;
    if (!store_local(frame, idx + 1, high)) return -1;
    return 0;
}

export function op_dstore(jvm, thread, frame) { return op_lstore(jvm, thread, frame); }

function _store_n(frame, n) {
    const v = pop(frame);
    return store_local(frame, n, v) ? 0 : -1;
}
function _lstore_n(frame, n) {
    const v2 = pop(frame);
    const v1 = pop(frame);
    if (!store_local(frame, n + 1, v2)) return -1;
    if (!store_local(frame, n,     v1)) return -1;
    return 0;
}

export function op_store_0(jvm, thread, frame) { return _store_n(frame, 0); }
export function op_store_1(jvm, thread, frame) { return _store_n(frame, 1); }
export function op_store_2(jvm, thread, frame) { return _store_n(frame, 2); }
export function op_store_3(jvm, thread, frame) { return _store_n(frame, 3); }

export function op_lstore_0(jvm, thread, frame) { return _lstore_n(frame, 0); }
export function op_lstore_1(jvm, thread, frame) { return _lstore_n(frame, 1); }
export function op_lstore_2(jvm, thread, frame) { return _lstore_n(frame, 2); }
export function op_lstore_3(jvm, thread, frame) { return _lstore_n(frame, 3); }

export function op_dstore_0(jvm, thread, frame) { return _lstore_n(frame, 0); }
export function op_dstore_1(jvm, thread, frame) { return _lstore_n(frame, 1); }
export function op_dstore_2(jvm, thread, frame) { return _lstore_n(frame, 2); }
export function op_dstore_3(jvm, thread, frame) { return _lstore_n(frame, 3); }

export function op_array_store(jvm, thread, frame) {
    const opcode = frame.code[frame.pc - 1] & 0xFF;
    let value;
    if (opcode === OPC_LASTORE || opcode === OPC_DASTORE) {
        pop(frame); // high slot
        value = pop(frame);
    } else {
        value = pop(frame);
    }
    const index = pop(frame).i;
    const array = pop(frame).ref;
    if (!array) {
        const caller = frame.clazz && frame.clazz.class_name;
        if (caller && _drm_is_drm_class && _drm_is_drm_class(caller)) return 0;
        _native_throw_npe(jvm, thread);
        return -1;
    }
    if (index < 0 || index >= array.length) {
        _native_throw_aioobe(jvm, thread, index);
        return -1;
    }
    if (opcode === OPC_AASTORE && value.ref !== null) {
        const element_class = array.element_class;
        if (element_class && element_class.class_name) {
            const obj = value.ref;
            const value_class = obj ? (obj.header && obj.header.clazz) : null;
            if (value_class && value_class.class_name) {
                if (!_object_instance_of(obj, element_class) &&
                    element_class.class_name !== 'java/lang/Object') {
                    _native_throw_array_store_exception(jvm, thread);
                    return -1;
                }
            }
        }
    }
    _array_set(array, index, value);
    return 0;
}

// Stack operations
export function op_pop(jvm, thread, frame) { pop(frame); return 0; }
export function op_pop2(jvm, thread, frame) { pop(frame); pop(frame); return 0; }

export function op_dup(jvm, thread, frame) {
    const v = peek(frame);
    push(frame, v);
    return 0;
}

export function op_dup_x1(jvm, thread, frame) {
    const v1 = pop(frame);
    const v2 = pop(frame);
    push(frame, v1);
    push(frame, v2);
    push(frame, v1);
    return 0;
}

export function op_dup_x2(jvm, thread, frame) {
    const v1 = pop(frame);
    const v2 = pop(frame);
    const v3 = pop(frame);
    push(frame, v1);
    push(frame, v3);
    push(frame, v2);
    push(frame, v1);
    return 0;
}

export function op_dup2(jvm, thread, frame) {
    const v1 = pop(frame);
    const v2 = pop(frame);
    push(frame, v2);
    push(frame, v1);
    push(frame, v2);
    push(frame, v1);
    return 0;
}

export function op_dup2_x1(jvm, thread, frame) {
    const v1 = pop(frame);
    const v2 = pop(frame);
    const v3 = pop(frame);
    push(frame, v2);
    push(frame, v1);
    push(frame, v3);
    push(frame, v2);
    push(frame, v1);
    return 0;
}

export function op_dup2_x2(jvm, thread, frame) {
    const v1 = pop(frame);
    const v2 = pop(frame);
    const v3 = pop(frame);
    const v4 = pop(frame);
    push(frame, v2);
    push(frame, v1);
    push(frame, v4);
    push(frame, v3);
    push(frame, v2);
    push(frame, v1);
    return 0;
}

export function op_swap(jvm, thread, frame) {
    const v1 = pop(frame);
    const v2 = pop(frame);
    push(frame, v1);
    push(frame, v2);
    return 0;
}

// Arithmetic
export function op_add(jvm, thread, frame) {
    const opcode = frame.code[frame.pc - 1] & 0xFF;
    switch (opcode) {
        case OPC_IADD: { const v2 = pop(frame); const v1 = pop(frame); push(frame, jv_i(v1.i + v2.i)); break; }
        case OPC_LADD: { pop(frame); const v2 = pop(frame); pop(frame); const v1 = pop(frame);
            const r = jv_j(BigInt.asIntN(64, v1.j + v2.j)); push(frame, r); push(frame, r); break; }
        case OPC_FADD: { const v2 = pop(frame); const v1 = pop(frame); push(frame, jv_f(Math.fround(v1.f + v2.f))); break; }
        case OPC_DADD: { pop(frame); const v2 = pop(frame); pop(frame); const v1 = pop(frame);
            const r = jv_d(v1.d + v2.d); push(frame, r); push(frame, r); break; }
    }
    return 0;
}

export function op_sub(jvm, thread, frame) {
    const opcode = frame.code[frame.pc - 1] & 0xFF;
    switch (opcode) {
        case OPC_ISUB: { const v2 = pop(frame); const v1 = pop(frame); push(frame, jv_i(v1.i - v2.i)); break; }
        case OPC_LSUB: { pop(frame); const v2 = pop(frame); pop(frame); const v1 = pop(frame);
            const r = jv_j(BigInt.asIntN(64, v1.j - v2.j)); push(frame, r); push(frame, r); break; }
        case OPC_FSUB: { const v2 = pop(frame); const v1 = pop(frame); push(frame, jv_f(Math.fround(v1.f - v2.f))); break; }
        case OPC_DSUB: { pop(frame); const v2 = pop(frame); pop(frame); const v1 = pop(frame);
            const r = jv_d(v1.d - v2.d); push(frame, r); push(frame, r); break; }
    }
    return 0;
}

export function op_mul(jvm, thread, frame) {
    const opcode = frame.code[frame.pc - 1] & 0xFF;
    switch (opcode) {
        case OPC_IMUL: { const v2 = pop(frame); const v1 = pop(frame); push(frame, jv_i(Math.imul(v1.i, v2.i))); break; }
        case OPC_LMUL: { pop(frame); const v2 = pop(frame); pop(frame); const v1 = pop(frame);
            const r = jv_j(BigInt.asIntN(64, v1.j * v2.j)); push(frame, r); push(frame, r); break; }
        case OPC_FMUL: { const v2 = pop(frame); const v1 = pop(frame); push(frame, jv_f(Math.fround(v1.f * v2.f))); break; }
        case OPC_DMUL: { pop(frame); const v2 = pop(frame); pop(frame); const v1 = pop(frame);
            const r = jv_d(v1.d * v2.d); push(frame, r); push(frame, r); break; }
    }
    return 0;
}

export function op_div(jvm, thread, frame) {
    const opcode = frame.code[frame.pc - 1] & 0xFF;
    switch (opcode) {
        case OPC_IDIV: {
            const v2 = pop(frame); const v1 = pop(frame);
            if (v2.i === 0) { _jvm_throw_by_name(jvm, 'java/lang/ArithmeticException', '/ by zero'); return -1; }
            push(frame, jv_i(Math.trunc(v1.i / v2.i))); break;
        }
        case OPC_LDIV: {
            pop(frame); const v2 = pop(frame); pop(frame); const v1 = pop(frame);
            if (v2.j === 0n) { _jvm_throw_by_name(jvm, 'java/lang/ArithmeticException', '/ by zero'); return -1; }
            const r = jv_j(BigInt.asIntN(64, v1.j / v2.j)); push(frame, r); push(frame, r); break;
        }
        case OPC_FDIV: { const v2 = pop(frame); const v1 = pop(frame); push(frame, jv_f(Math.fround(v1.f / v2.f))); break; }
        case OPC_DDIV: { pop(frame); const v2 = pop(frame); pop(frame); const v1 = pop(frame);
            const r = jv_d(v1.d / v2.d); push(frame, r); push(frame, r); break; }
    }
    return 0;
}

export function op_rem(jvm, thread, frame) {
    const opcode = frame.code[frame.pc - 1] & 0xFF;
    switch (opcode) {
        case OPC_IREM: {
            const v2 = pop(frame); const v1 = pop(frame);
            if (v2.i === 0) { _jvm_throw_by_name(jvm, 'java/lang/ArithmeticException', '/ by zero'); return -1; }
            push(frame, jv_i(v1.i % v2.i)); break;
        }
        case OPC_LREM: {
            pop(frame); const v2 = pop(frame); pop(frame); const v1 = pop(frame);
            if (v2.j === 0n) { _jvm_throw_by_name(jvm, 'java/lang/ArithmeticException', '/ by zero'); return -1; }
            const r = jv_j(BigInt.asIntN(64, v1.j % v2.j)); push(frame, r); push(frame, r); break;
        }
        case OPC_FREM: { const v2 = pop(frame); const v1 = pop(frame); push(frame, jv_f(Math.fround(fmod(v1.f, v2.f)))); break; }
        case OPC_DREM: { pop(frame); const v2 = pop(frame); pop(frame); const v1 = pop(frame);
            const r = jv_d(fmod(v1.d, v2.d)); push(frame, r); push(frame, r); break; }
    }
    return 0;
}

export function op_neg(jvm, thread, frame) {
    const opcode = frame.code[frame.pc - 1] & 0xFF;
    switch (opcode) {
        case OPC_INEG: { const v = pop(frame); push(frame, jv_i(-v.i)); break; }
        case OPC_LNEG: { pop(frame); const v = pop(frame); const r = jv_j(BigInt.asIntN(64, -v.j)); push(frame, r); push(frame, r); break; }
        case OPC_FNEG: { const v = pop(frame); push(frame, jv_f(Math.fround(-v.f))); break; }
        case OPC_DNEG: { pop(frame); const v = pop(frame); const r = jv_d(-v.d); push(frame, r); push(frame, r); break; }
    }
    return 0;
}

export function op_shl(jvm, thread, frame) {
    const opcode = frame.code[frame.pc - 1] & 0xFF;
    const shift = pop(frame).i & 0x3F;
    switch (opcode) {
        case OPC_ISHL: { const v = pop(frame); push(frame, jv_i(v.i << shift)); break; }
        case OPC_LSHL: { pop(frame); const v = pop(frame);
            const r = jv_j(BigInt.asIntN(64, v.j << BigInt(shift & 0x3F))); push(frame, r); push(frame, r); break; }
    }
    return 0;
}

export function op_shr(jvm, thread, frame) {
    const opcode = frame.code[frame.pc - 1] & 0xFF;
    const shift = pop(frame).i & 0x3F;
    switch (opcode) {
        case OPC_ISHR: { const v = pop(frame); push(frame, jv_i(v.i >> shift)); break; }
        case OPC_LSHR: { pop(frame); const v = pop(frame);
            const r = jv_j(v.j >> BigInt(shift & 0x3F)); push(frame, r); push(frame, r); break; }
    }
    return 0;
}

export function op_ushr(jvm, thread, frame) {
    const opcode = frame.code[frame.pc - 1] & 0xFF;
    const shift = pop(frame).i & 0x3F;
    switch (opcode) {
        case OPC_IUSHR: { const v = pop(frame); push(frame, jv_i((v.i >>> shift) | 0)); break; }
        case OPC_LUSHR: { pop(frame); const v = pop(frame);
            const r = jv_j(BigInt.asIntN(64, BigInt.asUintN(64, v.j) >> BigInt(shift & 0x3F)));
            push(frame, r); push(frame, r); break; }
    }
    return 0;
}

export function op_and(jvm, thread, frame) {
    const opcode = frame.code[frame.pc - 1] & 0xFF;
    switch (opcode) {
        case OPC_IAND: { const v2 = pop(frame); const v1 = pop(frame); push(frame, jv_i(v1.i & v2.i)); break; }
        case OPC_LAND: { pop(frame); const v2 = pop(frame); pop(frame); const v1 = pop(frame);
            const r = jv_j(BigInt.asIntN(64, v1.j & v2.j)); push(frame, r); push(frame, r); break; }
    }
    return 0;
}

export function op_or(jvm, thread, frame) {
    const opcode = frame.code[frame.pc - 1] & 0xFF;
    switch (opcode) {
        case OPC_IOR: { const v2 = pop(frame); const v1 = pop(frame); push(frame, jv_i(v1.i | v2.i)); break; }
        case OPC_LOR: { pop(frame); const v2 = pop(frame); pop(frame); const v1 = pop(frame);
            const r = jv_j(BigInt.asIntN(64, v1.j | v2.j)); push(frame, r); push(frame, r); break; }
    }
    return 0;
}

export function op_xor(jvm, thread, frame) {
    const opcode = frame.code[frame.pc - 1] & 0xFF;
    switch (opcode) {
        case OPC_IXOR: { const v2 = pop(frame); const v1 = pop(frame); push(frame, jv_i(v1.i ^ v2.i)); break; }
        case OPC_LXOR: { pop(frame); const v2 = pop(frame); pop(frame); const v1 = pop(frame);
            const r = jv_j(BigInt.asIntN(64, v1.j ^ v2.j)); push(frame, r); push(frame, r); break; }
    }
    return 0;
}

export function op_iinc(jvm, thread, frame) {
    const idx = fetch_u1(frame);
    const c   = fetch_s1(frame);
    const v = load_local(frame, idx);
    if (!v) return -1;
    v.i = (v.i + c) | 0;
    store_local(frame, idx, v);
    return 0;
}

// Conversion
export function op_convert(jvm, thread, frame) {
    const opcode = frame.code[frame.pc - 1] & 0xFF;
    switch (opcode) {
        case OPC_I2L: { const v = pop(frame); const r = jv_j(BigInt(v.i)); push(frame, r); push(frame, r); break; }
        case OPC_I2D: { const v = pop(frame); const r = jv_d(v.i); push(frame, r); push(frame, r); break; }
        case OPC_F2L: { const v = pop(frame); const r = jv_j(BigInt(Math.trunc(v.f))); push(frame, r); push(frame, r); break; }
        case OPC_F2D: { const v = pop(frame); const r = jv_d(v.f); push(frame, r); push(frame, r); break; }
        case OPC_L2I: { pop(frame); const v = pop(frame); push(frame, jv_i(Number(BigInt.asIntN(32, v.j)))); break; }
        case OPC_L2F: { pop(frame); const v = pop(frame); push(frame, jv_f(Math.fround(Number(v.j)))); break; }
        case OPC_D2I: { pop(frame); const v = pop(frame); push(frame, jv_i(Math.trunc(v.d) | 0)); break; }
        case OPC_D2F: { pop(frame); const v = pop(frame); push(frame, jv_f(Math.fround(v.d))); break; }
        case OPC_L2D: { pop(frame); const v = pop(frame); const r = jv_d(Number(v.j)); push(frame, r); push(frame, r); break; }
        case OPC_D2L: { pop(frame); const v = pop(frame); const r = jv_j(BigInt.asIntN(64, BigInt(Math.trunc(v.d)))); push(frame, r); push(frame, r); break; }
        case OPC_I2F: { const v = pop(frame); push(frame, jv_f(Math.fround(v.i))); break; }
        case OPC_F2I: { const v = pop(frame); push(frame, jv_i(Math.trunc(v.f) | 0)); break; }
        case OPC_I2B: { const v = pop(frame); push(frame, jv_i((v.i << 24) >> 24)); break; }
        case OPC_I2C: { const v = pop(frame); push(frame, jv_i(v.i & 0xFFFF)); break; }
        case OPC_I2S: { const v = pop(frame); push(frame, jv_i((v.i << 16) >> 16)); break; }
        default: return -1;
    }
    return 0;
}

// Comparisons
export function op_lcmp(jvm, thread, frame) {
    pop(frame);
    const v2 = pop(frame).j;
    pop(frame);
    const v1 = pop(frame).j;
    push(frame, jv_i(v1 > v2 ? 1 : v1 < v2 ? -1 : 0));
    return 0;
}

export function op_fcmpl(jvm, thread, frame) {
    const v2 = pop(frame).f;
    const v1 = pop(frame).f;
    push(frame, jv_i(isNaN(v1) || isNaN(v2) ? -1 : v1 > v2 ? 1 : v1 < v2 ? -1 : 0));
    return 0;
}

export function op_fcmpg(jvm, thread, frame) {
    const v2 = pop(frame).f;
    const v1 = pop(frame).f;
    push(frame, jv_i(isNaN(v1) || isNaN(v2) ? 1 : v1 > v2 ? 1 : v1 < v2 ? -1 : 0));
    return 0;
}

export function op_dcmpl(jvm, thread, frame) {
    pop(frame); const v2 = pop(frame).d;
    pop(frame); const v1 = pop(frame).d;
    push(frame, jv_i(isNaN(v1) || isNaN(v2) ? -1 : v1 > v2 ? 1 : v1 < v2 ? -1 : 0));
    return 0;
}

export function op_dcmpg(jvm, thread, frame) {
    pop(frame); const v2 = pop(frame).d;
    pop(frame); const v1 = pop(frame).d;
    push(frame, jv_i(isNaN(v1) || isNaN(v2) ? 1 : v1 > v2 ? 1 : v1 < v2 ? -1 : 0));
    return 0;
}

// Branch operations
export function op_if(jvm, thread, frame) {
    const opcode = frame.code[frame.pc - 1] & 0xFF;
    const offset = fetch_s2(frame);
    const value  = pop(frame).i;
    let branch = false;
    switch (opcode) {
        case OPC_IFEQ: branch = (value === 0); break;
        case OPC_IFNE: branch = (value !== 0); break;
        case OPC_IFLT: branch = (value < 0); break;
        case OPC_IFGE: branch = (value >= 0); break;
        case OPC_IFGT: branch = (value > 0); break;
        case OPC_IFLE: branch = (value <= 0); break;
    }
    if (branch) frame.pc += offset - 3;
    return 0;
}

export function op_if_icmp(jvm, thread, frame) {
    const opcode = frame.code[frame.pc - 1] & 0xFF;
    const offset = fetch_s2(frame);
    const v2 = pop(frame).i;
    const v1 = pop(frame).i;
    let branch = false;
    switch (opcode) {
        case OPC_IF_ICMPEQ: branch = (v1 === v2); break;
        case OPC_IF_ICMPNE: branch = (v1 !== v2); break;
        case OPC_IF_ICMPLT: branch = (v1 < v2); break;
        case OPC_IF_ICMPGE: branch = (v1 >= v2); break;
        case OPC_IF_ICMPGT: branch = (v1 > v2); break;
        case OPC_IF_ICMPLE: branch = (v1 <= v2); break;
    }
    if (branch) frame.pc += offset - 3;
    return 0;
}

export function op_if_acmp(jvm, thread, frame) {
    const opcode = frame.code[frame.pc - 1] & 0xFF;
    const offset = fetch_s2(frame);
    const v2 = pop(frame).ref;
    const v1 = pop(frame).ref;
    let branch = false;
    switch (opcode) {
        case OPC_IF_ACMPEQ: branch = (v1 === v2); break;
        case OPC_IF_ACMPNE: branch = (v1 !== v2); break;
    }
    if (branch) frame.pc += offset - 3;
    return 0;
}

export function op_goto(jvm, thread, frame) {
    const offset = fetch_s2(frame);
    const old_pc = frame.pc - 3;
    frame.pc = old_pc + offset;
    return 0;
}

export function op_jsr(jvm, thread, frame) {
    const offset = fetch_s2(frame);
    push(frame, jv_i(frame.pc));
    frame.pc += offset - 3;
    return 0;
}

export function op_ret(jvm, thread, frame) {
    const idx = fetch_u1(frame);
    const v = load_local(frame, idx);
    frame.pc = v ? v.i : 0;
    return 0;
}

export function op_tableswitch(jvm, thread, frame) {
    const start_pc = frame.pc - 1;
    while (frame.pc % 4 !== 0) frame.pc++;
    const default_offset = (read_u32(frame.code, frame.pc)) | 0; frame.pc += 4;
    const low  = (read_u32(frame.code, frame.pc)) | 0; frame.pc += 4;
    const high = (read_u32(frame.code, frame.pc)) | 0; frame.pc += 4;
    const index = pop(frame).i;
    let offset;
    if (index < low || index > high) {
        offset = default_offset;
    } else {
        offset = (read_u32(frame.code, frame.pc + (index - low) * 4)) | 0;
    }
    frame.pc = start_pc + offset;
    return 0;
}

export function op_lookupswitch(jvm, thread, frame) {
    const start_pc = frame.pc - 1;
    while (frame.pc % 4 !== 0) frame.pc++;
    const default_offset = (read_u32(frame.code, frame.pc)) | 0; frame.pc += 4;
    const npairs = (read_u32(frame.code, frame.pc)) | 0; frame.pc += 4;
    const key = pop(frame).i;
    let offset = default_offset;
    for (let i = 0; i < npairs; i++) {
        const match = (read_u32(frame.code, frame.pc)) | 0; frame.pc += 4;
        const jump  = (read_u32(frame.code, frame.pc)) | 0; frame.pc += 4;
        if (match === key) { offset = jump; break; }
    }
    frame.pc = start_pc + offset;
    return 0;
}

// Return operations
export function op_return(jvm, thread, frame) { return 1; }

export function op_ireturn(jvm, thread, frame) {
    if (!frame.prev) return -1;
    const result = pop(frame);
    frame.prev.stack[++frame.prev.stack_top] = result;
    return 1;
}

export function op_lreturn(jvm, thread, frame) {
    if (!frame.prev) return -1;
    const high = pop(frame);
    const low  = pop(frame);
    frame.prev.stack[++frame.prev.stack_top] = low;
    frame.prev.stack[++frame.prev.stack_top] = high;
    return 1;
}

export function op_freturn(jvm, thread, frame) { return op_ireturn(jvm, thread, frame); }

export function op_dreturn(jvm, thread, frame) {
    if (!frame.prev) return -1;
    const high = pop(frame);
    const low  = pop(frame);
    frame.prev.stack[++frame.prev.stack_top] = low;
    frame.prev.stack[++frame.prev.stack_top] = high;
    return 1;
}

export function op_areturn(jvm, thread, frame) { return op_ireturn(jvm, thread, frame); }

// Field access
export function op_getstatic(jvm, thread, frame) {
    const index = fetch_u2(frame);
    let class_name = null, field_name = null, descriptor = null;
    const clazz = frame.clazz;
    if (clazz && clazz.constant_pool && index > 0 && index < clazz.constant_pool_count) {
        const entry = clazz.constant_pool[index];
        if (entry.tag === CONSTANT_Fieldref) {
            class_name = _classfile_get_class_name(clazz, entry.info.class_index);
            const nat = _classfile_get_name_and_type(clazz, entry.info.name_and_type_index);
            field_name = nat.name; descriptor = nat.descriptor;
        }
    }
    if (!class_name || !field_name || !descriptor) {
        push(frame, jv_zero());
        return 0;
    }
    const target_class = _jvm_load_class(jvm, class_name);
    if (!target_class) { push(frame, jv_zero()); if (descriptor[0] === 'J' || descriptor[0] === 'D') push(frame, jv_zero()); return 0; }
    if (!target_class.initialized) {
        if (_jvm_init_class(jvm, target_class) !== 0) return -1;
    }
    if (!target_class.static_fields) {
        target_class.static_fields = [];
        target_class.static_fields_count = 0;
    }
    let v = jv_zero();
    const sf = target_class.static_fields;
    for (let i = 0; i < sf.length; i++) {
        if (sf[i].name === field_name && sf[i].descriptor === descriptor) {
            v = sf[i].value; break;
        }
    }
    push(frame, v);
    if (descriptor[0] === 'J' || descriptor[0] === 'D') push(frame, v);
    return 0;
}

export function op_putstatic(jvm, thread, frame) {
    const index = fetch_u2(frame);
    let class_name = null, field_name = null, descriptor = null;
    const clazz = frame.clazz;
    if (clazz && clazz.constant_pool && index > 0 && index < clazz.constant_pool_count) {
        const entry = clazz.constant_pool[index];
        if (entry.tag === CONSTANT_Fieldref) {
            class_name = _classfile_get_class_name(clazz, entry.info.class_index);
            const nat = _classfile_get_name_and_type(clazz, entry.info.name_and_type_index);
            field_name = nat.name; descriptor = nat.descriptor;
        }
    }
    let v;
    if (descriptor && (descriptor[0] === 'J' || descriptor[0] === 'D')) {
        pop(frame); v = pop(frame);
    } else {
        v = pop(frame);
    }
    if (!class_name || !field_name) return 0;
    const target_class = _jvm_load_class(jvm, class_name);
    if (!target_class) return 0;
    if (!target_class.initialized) {
        if (_jvm_init_class(jvm, target_class) !== 0) return -1;
    }
    if (!target_class.static_fields) { target_class.static_fields = []; target_class.static_fields_count = 0; }
    const sf = target_class.static_fields;
    let slot = -1;
    for (let i = 0; i < sf.length; i++) {
        if (sf[i].name === field_name && sf[i].descriptor === descriptor) { slot = i; break; }
    }
    if (slot === -1) {
        slot = sf.length;
        sf.push({ name: field_name, descriptor: descriptor, value: jv_zero() });
        target_class.static_fields_count = sf.length;
    }
    sf[slot].value = v;
    return 0;
}

export function op_getfield(jvm, thread, frame) {
    const index = fetch_u2(frame);
    let class_name = null, field_name = null, descriptor = null;
    const clazz = frame.clazz;
    if (clazz && clazz.constant_pool && index > 0 && index < clazz.constant_pool_count) {
        const entry = clazz.constant_pool[index];
        if (entry.tag === CONSTANT_Fieldref) {
            class_name = _classfile_get_class_name(clazz, entry.info.class_index);
            const nat = _classfile_get_name_and_type(clazz, entry.info.name_and_type_index);
            field_name = nat.name; descriptor = nat.descriptor;
        }
    }
    const obj = pop(frame).ref;
    if (!obj) {
        if (class_name && _drm_is_drm_class && _drm_is_drm_class(class_name)) {
            push(frame, jv_zero());
            if (descriptor && (descriptor[0] === 'J' || descriptor[0] === 'D')) push(frame, jv_zero());
            return 0;
        }
        _native_throw_npe(jvm, thread);
        return -1;
    }
    if (!obj.header || !obj.header.clazz) {
        _jvm_throw_by_name(jvm, 'java/lang/InternalError', 'Object has no class (getfield)');
        return -1;
    }
    const obj_class = obj.header.clazz;
    const fr = find_field_in_hierarchy(obj_class, field_name, descriptor);
    let v = jv_zero();
    if (fr.defining_class) {
        v = obj.fields[fr.slot] || jv_zero();
    }
    push(frame, v);
    if (descriptor && (descriptor[0] === 'J' || descriptor[0] === 'D')) push(frame, v);
    return 0;
}

export function op_putfield(jvm, thread, frame) {
    const index = fetch_u2(frame);
    let class_name = null, field_name = null, descriptor = null;
    const clazz = frame.clazz;
    if (clazz && clazz.constant_pool && index > 0 && index < clazz.constant_pool_count) {
        const entry = clazz.constant_pool[index];
        if (entry.tag === CONSTANT_Fieldref) {
            class_name = _classfile_get_class_name(clazz, entry.info.class_index);
            const nat = _classfile_get_name_and_type(clazz, entry.info.name_and_type_index);
            field_name = nat.name; descriptor = nat.descriptor;
        }
    }
    let value;
    if (descriptor && (descriptor[0] === 'J' || descriptor[0] === 'D')) {
        pop(frame); value = pop(frame);
    } else {
        value = pop(frame);
    }
    const obj = pop(frame).ref;
    if (!obj) {
        if (class_name && _drm_is_drm_class && _drm_is_drm_class(class_name)) return 0;
        _native_throw_npe(jvm, thread);
        return -1;
    }
    if (!obj.header || !obj.header.clazz) {
        _jvm_throw_by_name(jvm, 'java/lang/InternalError', 'Object has no class (putfield)');
        return -1;
    }
    const obj_class = obj.header.clazz;
    const fr = find_field_in_hierarchy(obj_class, field_name, descriptor);
    if (fr.defining_class) {
        if (!obj.fields) obj.fields = [];
        obj.fields[fr.slot] = value;
    }
    return 0;
}

// ── invokevirtual ─────────────────────────────────────────────────────────────
export function op_invokevirtual(jvm, thread, frame) {
    const index = fetch_u2(frame);
    let class_name = null, method_name = null, descriptor = null;
    const clazz = frame.clazz;
    if (clazz && clazz.constant_pool && index > 0 && index < clazz.constant_pool_count) {
        const entry = clazz.constant_pool[index];
        if (entry.tag === CONSTANT_Methodref) {
            class_name = _classfile_get_class_name(clazz, entry.info.class_index);
            const nat = _classfile_get_name_and_type(clazz, entry.info.name_and_type_index);
            method_name = nat.name; descriptor = nat.descriptor;
        }
    }
    if (!descriptor) return -1;

    const arg_count = _count_args(descriptor);

    // Pad stack if needed
    while (frame.stack_top + 1 < arg_count + 1) push(frame, jv_zero());

    const args = new Array(arg_count + 1);
    for (let i = arg_count; i >= 1; i--) args[i] = pop(frame);
    args[0] = pop(frame);

    const obj = args[0].ref;

    // ── null object compatibility ─────────────────────────────────────────────
    if (!obj) {
        // String.equals → NPE
        if (class_name === 'java/lang/String' && method_name === 'equals' && descriptor === '(Ljava/lang/Object;)Z') {
            _jvm_throw_by_name(jvm, 'java/lang/NullPointerException', null);
            return -1;
        }
        // equals → false for non-String
        if (method_name === 'equals' && descriptor === '(Ljava/lang/Object;)Z') {
            push(frame, jv_i(0)); return 0;
        }
        // Integer.intValue() → 0
        if (class_name === 'java/lang/Integer' && method_name === 'intValue' && descriptor === '()I') {
            push(frame, jv_i(0)); return 0;
        }
        // Long.longValue() → 0
        if (class_name === 'java/lang/Long' && method_name === 'longValue' && descriptor === '()J') {
            const r = jv_j(0n); push(frame, r); return 0;
        }
        // Font null methods
        if (class_name === 'javax/microedition/lcdui/Font') {
            const DEFAULT_W = 5, DEFAULT_H = 7;
            if (method_name === 'stringWidth' && descriptor === '(Ljava/lang/String;)I') {
                let w = 0;
                if (args[1] && args[1].ref) {
                    const s = _string_utf8 ? _string_utf8(jvm, args[1].ref) : null;
                    if (s) w = s.length * (DEFAULT_W + 1);
                }
                push(frame, jv_i(w)); return 0;
            }
            if (method_name === 'charWidth') { push(frame, jv_i(DEFAULT_W)); return 0; }
            if (method_name === 'charsWidth') { push(frame, jv_i(args[3] ? args[3].i * (DEFAULT_W + 1) : 0)); return 0; }
            if (method_name === 'substringWidth') { push(frame, jv_i(args[3] ? args[3].i * (DEFAULT_W + 1) : 0)); return 0; }
            if (method_name === 'getHeight') { push(frame, jv_i(DEFAULT_H)); return 0; }
            if (method_name === 'getBaselinePosition') { push(frame, jv_i(DEFAULT_H - 2)); return 0; }
            if (method_name === 'getFace') { push(frame, jv_i(0)); return 0; }
            if (method_name === 'getStyle') { push(frame, jv_i(0)); return 0; }
            if (method_name === 'getSize') { push(frame, jv_i(8)); return 0; }
            if ((method_name === 'isPlain' || method_name === 'isBold' || method_name === 'isItalic' || method_name === 'isUnderlined') && descriptor === '()Z') {
                push(frame, jv_i(0)); return 0;
            }
            _jvm_throw_by_name(jvm, 'java/lang/NullPointerException', null);
            return -1;
        }
        // String → NPE
        if (class_name === 'java/lang/String') {
            _jvm_throw_by_name(jvm, 'java/lang/NullPointerException', null);
            return -1;
        }
        // Collections/Map null compat
        const isCollection = class_name === 'java/util/Hashtable' || class_name === 'java/util/HashMap' ||
                             class_name === 'java/util/Vector'    || class_name === 'java/util/Stack'   ||
                             class_name === 'java/util/ArrayList';
        if (isCollection) {
            if (method_name === 'get' || method_name === 'elementAt' || method_name === 'remove') { push(frame, jv_ref(null)); return 0; }
            if (method_name === 'size' || method_name === 'getSize') { push(frame, jv_i(0)); return 0; }
            if (method_name === 'isEmpty') { push(frame, jv_i(1)); return 0; }
            if (method_name === 'put' || method_name === 'addElement' || method_name === 'add' || method_name === 'contains' || method_name === 'containsKey') {
                const rc = ret_char(descriptor);
                if (rc === 'Z' || rc === 'I' || rc === 'B' || rc === 'S') push(frame, jv_i(0));
                else push(frame, jv_ref(null));
                return 0;
            }
            if (method_name === 'toString' && descriptor === '()Ljava/lang/String;') { push(frame, jv_ref(_jvm_new_string(jvm, 'null'))); return 0; }
            if (method_name === 'keys' || method_name === 'elements' || method_name === 'iterator') { push(frame, jv_ref(null)); return 0; }
            // other methods: default by return type
            const rc = ret_char(descriptor);
            if (rc === 'L' || rc === '[') push(frame, jv_ref(null));
            else push(frame, jv_i(0));
            return 0;
        }
        // Form/List/Alert null compat
        const isLcdui = class_name === 'javax/microedition/lcdui/Form' ||
                        class_name === 'javax/microedition/lcdui/List' ||
                        class_name === 'javax/microedition/lcdui/Alert';
        if (isLcdui) {
            if (method_name === 'size' && descriptor === '()I') { push(frame, jv_i(0)); return 0; }
            if (method_name === 'append' || method_name === 'delete' || method_name === 'deleteAll' || method_name === 'set' || method_name === 'insert') {
                const rc = ret_char(descriptor);
                if (rc === 'I' || rc === 'Z') push(frame, jv_i(0));
                return 0;
            }
            if (method_name === 'getTitle' || method_name === 'getString' || method_name === 'getSelectedIndex') { push(frame, jv_ref(null)); return 0; }
        }
        // DRM bypass
        if (class_name && _drm_is_drm_class && _drm_is_drm_class(class_name)) {
            const rc = ret_char(descriptor);
            if (rc === ';') push(frame, jv_ref(null));
            else if (rc === 'Z' || rc === 'I' || rc === 'B' || rc === 'C' || rc === 'S') push(frame, jv_i(0));
            else push(frame, jv_ref(null));
            return 0;
        }
        _jvm_throw_by_name(jvm, 'java/lang/NullPointerException', null);
        return -1;
    }

    // Validate managed object
    if (_is_managed_object && !_is_managed_object(obj)) {
        // Check if it's a Class object
        if (obj.header && obj.header.clazz && obj.header.clazz.class_name === 'java/lang/Class') {
            // ok
        } else {
            return -1;
        }
    }

    if (!obj.header || !obj.header.clazz || !obj.header.clazz.class_name) return -1;

    const target_class = obj.header.clazz;
    let method = _jvm_resolve_method(jvm, target_class, method_name, descriptor);

    if (!method) {
        // Try native
        let native_fn = null;
        let sc = target_class;
        while (sc && !native_fn) {
            native_fn = _native_find(jvm, sc.class_name, method_name, descriptor);
            if (!native_fn) sc = sc.super_class;
        }
        if (native_fn) {
            if (thread.pending_exception) thread.pending_exception = null;
            pin_args(jvm, args, arg_count + 1);
            const result = native_fn(jvm, thread, args, arg_count + 1);
            unpin_args(jvm, args, arg_count + 1);
            if (thread.pending_exception) return -1;
            push_return(frame, result, descriptor);
            return 0;
        }
        // stub fallback
        push_return(frame, jv_zero(), descriptor);
        return 0;
    }

    const result = { value: jv_zero() };
    const ret = _execute_method(jvm, thread, method, args, result);

    if (method.is_native && ret === 0) {
        push_return(frame, result.value, descriptor);
    }

    return ret;
}

// ── op_invokespecial ──────────────────────────────────────────────────────────
export function op_invokespecial(jvm, thread, frame) {
    const index = fetch_u2(frame);
    let class_name = null, method_name = null, descriptor = null;

    if (index > 0 && index < frame.clazz.constant_pool.length) {
        const entry = frame.clazz.constant_pool[index];
        if (entry && (entry.tag === 10 || entry.tag === 11)) {
            class_name  = _classfile_get_class_name(frame.clazz, entry.class_index);
            const nat   = _classfile_get_name_and_type(frame.clazz, entry.name_and_type_index);
            method_name = nat ? nat.name : null;
            descriptor  = nat ? nat.descriptor : null;
        }
    }

    // Count Java-level arguments (J/D = 1 Java arg but 2 stack slots)
    let java_args = 0;
    const stack_slots = descriptor ? _count_args(descriptor) : 0;

    if (descriptor) {
        const p = descriptor;
        let i = p[0] === '(' ? 1 : 0;
        while (i < p.length && p[i] !== ')') {
            const ch = p[i];
            if (ch === 'J' || ch === 'D') { java_args++; i++; }
            else if (ch === 'B' || ch === 'C' || ch === 'F' || ch === 'I' || ch === 'S' || ch === 'Z') { java_args++; i++; }
            else if (ch === 'L') { java_args++; while (i < p.length && p[i] !== ';') i++; if (p[i] === ';') i++; }
            else if (ch === '[') {
                java_args++;
                while (i < p.length && p[i] === '[') i++;
                if (i < p.length && p[i] === 'L') while (i < p.length && p[i] !== ';') i++;
                if (i < p.length) i++;
            } else i++;
        }
    }

    // Pop stack_slots values into a temporary buffer (index 0 = lowest)
    const stack_vals = new Array(stack_slots);
    for (let i = stack_slots - 1; i >= 0; i--) stack_vals[i] = frame_pop(frame);

    // Pop 'this' reference
    if (frame.stack_top < 0) {
        if (g_j2me_runtime_debug) console.log('[INVOKESPECIAL] stack underflow: no this ref');
        return -1;
    }
    const this_val = frame_pop(frame);

    // Map stack values to args, merging J/D two-slot pairs
    const args = new Array(java_args + 1);
    args[0] = this_val;
    if (descriptor) {
        let sv = 0, ai = 1;
        const p = descriptor;
        let i = p[0] === '(' ? 1 : 0;
        while (i < p.length && p[i] !== ')') {
            const ch = p[i];
            if (ch === 'J' || ch === 'D') {
                args[ai] = stack_vals[sv]; sv += 2; ai++; i++;
            } else if (ch === 'L') {
                args[ai] = stack_vals[sv]; sv++; ai++;
                while (i < p.length && p[i] !== ';') i++;
                if (p[i] === ';') i++;
            } else if (ch === '[') {
                args[ai] = stack_vals[sv]; sv++; ai++;
                while (i < p.length && p[i] === '[') i++;
                if (i < p.length && p[i] === 'L') while (i < p.length && p[i] !== ';') i++;
                if (i < p.length) i++;
            } else {
                args[ai] = stack_vals[sv]; sv++; ai++; i++;
            }
        }
    }

    const arg_count = java_args;
    const obj = args[0] ? args[0].ref : null;

    if (!obj) {
        if (g_j2me_runtime_debug) console.log('[INVOKESPECIAL] NULL object');
        _native_throw_npe(jvm, thread);
        return -1;
    }

    const target_class = _jvm_load_class(jvm, class_name);
    if (!target_class) {
        if (g_j2me_runtime_debug) console.log('[INVOKESPECIAL] class not found:', class_name);
        return -1;
    }

    let method = _jvm_resolve_method(jvm, target_class, method_name, descriptor);

    if (!method) {
        // Try native — but NOT for constructors
        let native_fn = null;
        if (method_name && method_name !== '<init>' && method_name !== '<clinit>') {
            let sc = target_class;
            while (sc && !native_fn) {
                native_fn = _native_find(jvm, sc.class_name, method_name, descriptor);
                if (!native_fn) sc = sc.super_class;
            }
        }

        if (native_fn) {
            if (thread.pending_exception) thread.pending_exception = null;
            pin_args(jvm, args, arg_count + 1);
            const result = native_fn(jvm, thread, args, arg_count + 1);
            unpin_args(jvm, args, arg_count + 1);
            if (thread.pending_exception) return -1;
            push_return(frame, result, descriptor);
            return 0;
        }

        // Constructor not found fallbacks
        if (method_name === '<init>') {
            if (target_class.class_name === 'java/lang/Object') return 0;

            // Try native constructor with exact signature
            const native_ctor = _native_find(jvm, target_class.class_name, '<init>', descriptor);
            if (native_ctor) {
                if (thread.pending_exception) thread.pending_exception = null;
                native_ctor(jvm, thread, args, arg_count + 1);
                if (thread.pending_exception) return -1;
                return 0;
            }

            // Fall back to default ()V constructor
            const default_init = _jvm_resolve_method(jvm, target_class, '<init>', '()V');
            if (default_init) {
                const def_args = [args[0]];
                const result_box = { value: jv_zero() };
                const ret = _execute_method(jvm, thread, default_init, def_args, result_box);

                // LCDUI field initialization after constructor fallback
                if (ret === 0 && obj && arg_count >= 1) {
                    const cn = target_class.class_name || '';
                    const d = descriptor || '';

                    if ((cn.includes('javax/microedition/lcdui/Form') ||
                         cn.includes('javax/microedition/lcdui/List') ||
                         cn.includes('javax/microedition/lcdui/Alert') ||
                         cn.includes('javax/microedition/lcdui/TextBox')) &&
                        d.startsWith('(Ljava/lang/String;')) {
                        if (obj.fields && obj.fields.length >= 1)
                            obj.fields[0] = { ref: args[1] ? args[1].ref : null };
                    }

                    if (cn.includes('javax/microedition/lcdui/TextField') && arg_count >= 4 &&
                        d.startsWith('(Ljava/lang/String;Ljava/lang/String;II)')) {
                        if (obj.fields && obj.fields.length >= 4) {
                            obj.fields[0] = { ref: args[1] ? args[1].ref : null };
                            obj.fields[1] = { ref: args[2] ? args[2].ref : null };
                            obj.fields[2] = { i: args[3] ? args[3].i : 0 };
                            obj.fields[3] = { i: args[4] ? args[4].i : 0 };
                        }
                    }

                    if (cn.includes('javax/microedition/lcdui/StringItem') && arg_count >= 2 &&
                        d.startsWith('(Ljava/lang/String;Ljava/lang/String;)')) {
                        if (obj.fields && obj.fields.length >= 2) {
                            obj.fields[0] = { ref: args[1] ? args[1].ref : null };
                            obj.fields[1] = { ref: args[2] ? args[2].ref : null };
                        }
                    }

                    if (cn.includes('javax/microedition/lcdui/Alert') && arg_count >= 4 &&
                        d.startsWith('(Ljava/lang/String;Ljava/lang/String;Ljavax/microedition/lcdui/Image;Ljavax/microedition/lcdui/AlertType;')) {
                        if (obj.fields && obj.fields.length >= 4) {
                            obj.fields[0] = { ref: args[1] ? args[1].ref : null };
                            obj.fields[1] = { ref: args[2] ? args[2].ref : null };
                            obj.fields[2] = { ref: args[3] ? args[3].ref : null };
                            obj.fields[3] = { ref: args[4] ? args[4].ref : null };
                        }
                    }

                    if (cn.includes('javax/microedition/lcdui/TextBox') && arg_count >= 4 &&
                        d.startsWith('(Ljava/lang/String;Ljava/lang/String;II)')) {
                        if (obj.fields && obj.fields.length >= 4) {
                            obj.fields[0] = { ref: args[1] ? args[1].ref : null };
                            obj.fields[1] = { ref: args[2] ? args[2].ref : null };
                            obj.fields[2] = { i: args[3] ? args[3].i : 0 };
                            obj.fields[3] = { i: args[4] ? args[4].i : 0 };
                        }
                    }

                    if (cn.includes('javax/microedition/lcdui/Ticker') && arg_count >= 1 &&
                        d.startsWith('(Ljava/lang/String;)')) {
                        if (obj.fields && obj.fields.length >= 1)
                            obj.fields[0] = { ref: args[1] ? args[1].ref : null };
                    }

                    if (cn.includes('javax/microedition/lcdui/Gauge') && arg_count >= 4 &&
                        d.startsWith('(Ljava/lang/String;ZII)')) {
                        if (obj.fields && obj.fields.length >= 4) {
                            obj.fields[0] = { ref: args[1] ? args[1].ref : null };
                            obj.fields[1] = { i: args[2] ? args[2].i : 0 };
                            obj.fields[2] = { i: args[3] ? args[3].i : 0 };
                            obj.fields[3] = { i: args[4] ? args[4].i : 0 };
                        }
                    }

                    if (cn.includes('java/lang/Boolean') && arg_count >= 1 && d === '(Z)V') {
                        if (obj.fields && obj.fields.length >= 1)
                            obj.fields[0] = { i: args[1] ? (args[1].i ? 1 : 0) : 0 };
                    }

                    if (cn.includes('java/lang/Integer') && arg_count >= 1 && d === '(I)V') {
                        if (obj.fields && obj.fields.length >= 1)
                            obj.fields[0] = { i: args[1] ? args[1].i : 0 };
                    }

                    if (cn.includes('java/lang/Long') && arg_count >= 1 && d === '(J)V') {
                        if (obj.fields && obj.fields.length >= 1)
                            obj.fields[0] = { j: args[1] ? args[1].j : 0n };
                    }
                }
                return ret;
            }

            // Constructor truly not found
            _jvm_throw_by_name(jvm, 'java/lang/NoSuchMethodError', 'Constructor not found');
            thread.pending_exception = _jvm_exception_pending(jvm);
            return -1;
        }

        // Stub class fallback for non-constructor methods
        if (target_class.is_stub) {
            push_return(frame, jv_zero(), descriptor);
            return 0;
        }

        if (g_j2me_runtime_debug) console.log('[INVOKESPECIAL] method not found:', class_name, method_name, descriptor);
        return -1;
    }

    // Execute bytecode/native method
    const result_box = { value: jv_zero() };
    const ret = _execute_method(jvm, thread, method, args, result_box);

    // For native methods via invokespecial, push return value ourselves
    if (ret === 0 && method.is_native && descriptor) {
        const ri = descriptor.indexOf(')');
        if (ri >= 0) {
            const rt = descriptor[ri + 1];
            if (rt && rt !== 'V') {
                frame_push(frame, result_box.value);
                if (rt === 'J' || rt === 'D') frame_push(frame, jv_zero());
            }
        }
    }

    return ret;
}

// ── op_invokestatic ───────────────────────────────────────────────────────────
export function op_invokestatic(jvm, thread, frame) {
    const index = fetch_u2(frame);
    let class_name = null, method_name = null, descriptor = null;

    if (index > 0 && index < frame.clazz.constant_pool.length) {
        const entry = frame.clazz.constant_pool[index];
        if (entry && entry.tag === 10) {
            class_name  = _classfile_get_class_name(frame.clazz, entry.class_index);
            const nat   = _classfile_get_name_and_type(frame.clazz, entry.name_and_type_index);
            method_name = nat ? nat.name : null;
            descriptor  = nat ? nat.descriptor : null;
        }
    }

    if (!descriptor) {
        _jvm_throw_by_name(jvm, 'java/lang/InternalError', 'Null descriptor');
        return -1;
    }

    const arg_count = _count_args(descriptor);

    // Pad stack on underflow
    while (frame.stack_top + 1 < arg_count) frame_push(frame, jv_zero());

    const args = new Array(arg_count);
    for (let i = arg_count - 1; i >= 0; i--) args[i] = frame_pop(frame);

    const target_class = _jvm_load_class(jvm, class_name);
    let method = null;

    if (target_class) {
        if (!target_class.initialized) {
            if (_jvm_init_class(jvm, target_class) !== 0) return -1;
        }
        method = _jvm_resolve_method(jvm, target_class, method_name, descriptor);
    }

    if (method) {
        // Check for native override
        const native_fn = _native_find(jvm, class_name, method_name, descriptor);
        if (method.is_native || native_fn) {
            const fn = native_fn;
            if (fn) {
                if (thread.pending_exception) thread.pending_exception = null;
                pin_args(jvm, args, arg_count);
                const result = fn(jvm, thread, args, arg_count);
                unpin_args(jvm, args, arg_count);
                if (thread.pending_exception) return -1;
                push_return(frame, result, descriptor);
                return 0;
            }
        }
        // Java bytecode — return ops push result themselves
        return _execute_method(jvm, thread, method, args, null);
    }

    // Try native
    const native_fn = _native_find(jvm, class_name, method_name, descriptor);
    if (native_fn) {
        if (thread.pending_exception) thread.pending_exception = null;
        pin_args(jvm, args, arg_count);
        const result = native_fn(jvm, thread, args, arg_count);
        unpin_args(jvm, args, arg_count);
        if (thread.pending_exception) return -1;
        push_return(frame, result, descriptor);
        return 0;
    }

    // Not found — return default zero
    if (g_j2me_runtime_debug) console.log('[INVOKESTATIC] not found (stub?):', class_name, method_name, descriptor);
    push_return(frame, jv_zero(), descriptor);
    return 0;
}

// ── op_invokeinterface ────────────────────────────────────────────────────────
export function op_invokeinterface(jvm, thread, frame) {
    const index = fetch_u2(frame);
    fetch_u1(frame);  // count byte — informational
    fetch_u1(frame);  // zero byte

    let class_name = null, method_name = null, descriptor = null;

    if (index > 0 && index < frame.clazz.constant_pool.length) {
        const entry = frame.clazz.constant_pool[index];
        if (entry && entry.tag === 11) {
            class_name  = _classfile_get_class_name(frame.clazz, entry.class_index);
            const nat   = _classfile_get_name_and_type(frame.clazz, entry.name_and_type_index);
            method_name = nat ? nat.name : null;
            descriptor  = nat ? nat.descriptor : null;
        }
    }

    if (!descriptor) {
        if (g_j2me_runtime_debug) console.log('[INVOKEINTERFACE] null descriptor at index', index);
        return -1;
    }

    const arg_count = _count_args(descriptor);

    // Pad on underflow
    while (frame.stack_top + 1 < arg_count + 1) frame_push(frame, jv_zero());

    const args = new Array(arg_count + 1);
    for (let i = arg_count; i >= 1; i--) args[i] = frame_pop(frame);
    args[0] = frame_pop(frame);

    const obj = args[0] ? args[0].ref : null;

    if (!obj) {
        // Null-safe compatibility for common interface methods
        if (class_name && class_name.includes('Player')) {
            if (method_name === 'getMediaTime' || method_name === 'getDuration') {
                const jv = { i: 0, j: -1n, f: 0, d: 0, ref: null };
                frame_push(frame, jv); frame_push(frame, jv); // 2 slots for long
                return 0;
            }
            if (method_name === 'getState') {
                frame_push(frame, { i: 0, j: 0n, f: 0, d: 0, ref: null });
                return 0;
            }
            if (method_name === 'realize' || method_name === 'prefetch' || method_name === 'start' ||
                method_name === 'stop' || method_name === 'deallocate' || method_name === 'close') return 0;
            if (method_name === 'getContentType' || method_name === 'getControl') {
                frame_push(frame, jv_null()); return 0;
            }
        }
        if (class_name && class_name.includes('Enumeration')) {
            if (method_name === 'hasMoreElements') {
                frame_push(frame, { i: 0, j: 0n, f: 0, d: 0, ref: null }); return 0;
            }
            if (method_name === 'nextElement') { frame_push(frame, jv_null()); return 0; }
        }
        _jvm_throw_by_name(jvm, 'java/lang/NullPointerException', null);
        return -1;
    }

    // Resolve actual implementation in object's class hierarchy
    const obj_class = (obj.header && obj.header.clazz) ? obj.header.clazz : (obj.clazz || null);
    let method = null, native_fn = null;

    let sc = obj_class;
    while (sc && !method && !native_fn) {
        method = _jvm_resolve_method(jvm, sc, method_name, descriptor);
        if (!method) native_fn = _native_find(jvm, sc.class_name, method_name, descriptor);
        if (!method && !native_fn) sc = sc.super_class;
    }

    // Try interface class name as last resort
    if (!method && !native_fn && class_name) {
        native_fn = _native_find(jvm, class_name, method_name, descriptor);
    }

    if (method) {
        const result_box = { value: jv_zero() };
        const ret = _execute_method(jvm, thread, method, args, result_box);
        if (ret < 0) return -1;
        if (thread.pending_exception) return -1;
        // For native methods, push result
        if (method.is_native && descriptor) {
            const ri = descriptor.indexOf(')');
            if (ri >= 0) {
                const rt = descriptor[ri + 1];
                if (rt && rt !== 'V') {
                    frame_push(frame, result_box.value);
                    if (rt === 'J' || rt === 'D') frame_push(frame, jv_zero());
                }
            }
        }
    } else if (native_fn) {
        if (thread.pending_exception) thread.pending_exception = null;
        pin_args(jvm, args, arg_count + 1);
        const result = native_fn(jvm, thread, args, arg_count + 1);
        unpin_args(jvm, args, arg_count + 1);
        if (thread.pending_exception) return -1;
        push_return(frame, result, descriptor);
    } else {
        // Not found — return default
        push_return(frame, jv_zero(), descriptor);
    }

    return 0;
}

// ── op_new ───────────────────────────────────────────────────────────────────
export function op_new(jvm, thread, frame) {
    const index = fetch_u2(frame);
    const class_name = _classfile_get_class_name(frame.clazz, index);

    const clazz = _jvm_load_class(jvm, class_name);
    if (!clazz) {
        _native_throw_cnfe(jvm, thread, class_name);
        return -1;
    }

    // Resolve super_class if needed
    if (clazz.super_class === null && clazz.super_class_name) {
        clazz.super_class = _jvm_load_class(jvm, clazz.super_class_name);
    }

    if (!clazz.initialized) {
        if (_jvm_init_class(jvm, clazz) !== 0) return -1;
    }

    const obj = _jvm_new_object(jvm, clazz);
    if (!obj) {
        _native_throw_oome(jvm, thread);
        return -1;
    }

    frame_push(frame, { i: 0, j: 0n, f: 0, d: 0, ref: obj });
    return 0;
}

// ── op_newarray ──────────────────────────────────────────────────────────────
export function op_newarray(jvm, thread, frame) {
    const atype = fetch_u1(frame);
    const count = frame_pop(frame).i;

    if (count < 0) { _native_throw_negative_array_size(jvm, thread); return -1; }
    if (count > 1000000) { _native_throw_iae(jvm, thread, 'Array size too large'); return -1; }

    const array = _jvm_new_array(jvm, atype, count, null);
    if (!array) { _native_throw_oome(jvm, thread); return -1; }

    frame_push(frame, { i: 0, j: 0n, f: 0, d: 0, ref: array });
    return 0;
}

// ── op_anewarray ─────────────────────────────────────────────────────────────
export function op_anewarray(jvm, thread, frame) {
    const index = fetch_u2(frame);
    const count = frame_pop(frame).i;

    if (count < 0) { _native_throw_negative_array_size(jvm, thread); return -1; }

    const element_class_name = _classfile_get_class_name(frame.clazz, index);
    let element_class = null;
    if (element_class_name) element_class = _jvm_load_class(jvm, element_class_name);

    const array = _jvm_new_array(jvm, 0x4C /* DESC_OBJECT */, count, element_class);
    if (!array) { _native_throw_oome(jvm, thread); return -1; }

    frame_push(frame, { i: 0, j: 0n, f: 0, d: 0, ref: array });
    return 0;
}

// ── op_arraylength ────────────────────────────────────────────────────────────
// Semantics: PEEK the top of stack (not POP), overwrite in place with length.
export function op_arraylength(jvm, thread, frame) {
    const top = frame.stack[frame.stack_top];
    const array = top ? top.ref : null;

    if (!array) {
        const caller_class = frame.clazz ? frame.clazz.class_name : null;
        if (caller_class && _drm_is_drm_class(caller_class)) {
            frame_pop(frame);
            frame_push(frame, { i: 0, j: 0n, f: 0, d: 0, ref: null });
            return 0;
        }
        _jvm_throw_by_name(jvm, 'java/lang/NullPointerException', 'null array');
        thread.pending_exception = _jvm_exception_pending(jvm);
        return -1;
    }

    // In-place overwrite (PEEK semantics from C source)
    frame.stack[frame.stack_top] = { i: array.length, j: 0n, f: 0, d: 0, ref: null };
    return 0;
}

// ── op_athrow ────────────────────────────────────────────────────────────────
export function op_athrow(jvm, thread, frame) {
    const exception = frame_pop(frame).ref;
    if (!exception) { _native_throw_npe(jvm, thread); return -1; }
    _jvm_throw(jvm, exception);
    return -1;
}

// ── op_checkcast ─────────────────────────────────────────────────────────────
// Semantics: PEEK (do not pop); throw ClassCastException on failure.
export function op_checkcast(jvm, thread, frame) {
    const index = fetch_u2(frame);
    const peek_val = frame.stack[frame.stack_top];
    const obj = peek_val ? peek_val.ref : null;
    const class_name = _classfile_get_class_name(frame.clazz, index);

    if (obj) {
        if (!_is_managed_object(obj)) {
            // Allow Class objects cast to java/lang/Class or Object
            if (obj.header && obj.header.clazz &&
                obj.header.clazz.class_name === 'java/lang/Class') {
                if (class_name === 'java/lang/Class' || class_name === 'java/lang/Object') return 0;
            }
            _jvm_throw_by_name(jvm, 'java/lang/ClassCastException', 'Object not in heap');
            return -1;
        }
        const target = _jvm_load_class(jvm, class_name);
        if (!_object_instance_of(obj, target)) {
            _jvm_throw_by_name(jvm, 'java/lang/ClassCastException', null);
            return -1;
        }
    }
    return 0;
}

// ── op_instanceof ─────────────────────────────────────────────────────────────
export function op_instanceof(jvm, thread, frame) {
    const index = fetch_u2(frame);
    const obj = frame_pop(frame).ref;
    let result = 0;
    if (obj) {
        const class_name = _classfile_get_class_name(frame.clazz, index);
        const target = _jvm_load_class(jvm, class_name);
        result = _object_instance_of(obj, target) ? 1 : 0;
    }
    frame_push(frame, { i: result, j: 0n, f: 0, d: 0, ref: null });
    return 0;
}

// ── op_monitorenter / op_monitorexit ─────────────────────────────────────────
export function op_monitorenter(jvm, thread, frame) {
    const obj = frame_pop(frame).ref;
    if (!obj) { _native_throw_npe(jvm, thread); return -1; }
    return _monitor_enter(jvm, obj);
}

export function op_monitorexit(jvm, thread, frame) {
    const obj = frame_pop(frame).ref;
    if (!obj) { _native_throw_npe(jvm, thread); return -1; }
    return _monitor_exit(jvm, obj);
}

// ── op_wide ──────────────────────────────────────────────────────────────────
export function op_wide(jvm, thread, frame) {
    const opcode = fetch_u1(frame);
    const index  = fetch_u2(frame);

    switch (opcode) {
        case OPC_ILOAD:
        case OPC_FLOAD:
        case OPC_ALOAD: {
            const v = load_local_v(frame, index);
            if (v === null) return -1;
            frame_push(frame, v);
            break;
        }
        case OPC_LLOAD:
        case OPC_DLOAD: {
            const v1 = load_local_v(frame, index);
            const v2 = load_local_v(frame, index + 1);
            if (v1 === null || v2 === null) return -1;
            frame_push(frame, v1);
            frame_push(frame, v2);
            break;
        }
        case OPC_ISTORE:
        case OPC_FSTORE:
        case OPC_ASTORE: {
            const v = frame_pop(frame);
            if (store_local_v(frame, index, v) !== 0) return -1;
            break;
        }
        case OPC_LSTORE:
        case OPC_DSTORE: {
            const v2 = frame_pop(frame);
            const v1 = frame_pop(frame);
            if (store_local_v(frame, index + 1, v2) !== 0) return -1;
            if (store_local_v(frame, index, v1) !== 0) return -1;
            break;
        }
        case OPC_IINC: {
            // Wide IINC: 2-byte signed constant
            const constant = fetch_s2(frame);
            const v = load_local_v(frame, index);
            if (v === null) return -1;
            v.i = (v.i + constant) | 0;
            if (store_local_v(frame, index, v) !== 0) return -1;
            break;
        }
        case OPC_RET: {
            const v = load_local_v(frame, index);
            if (v === null) return -1;
            frame.pc = v.i;
            break;
        }
        default:
            if (g_j2me_runtime_debug) console.log('[WIDE] unknown opcode: 0x' + opcode.toString(16));
            return -1;
    }
    return 0;
}

// ── create_multidim_array (helper) ────────────────────────────────────────────
function create_multidim_array(jvm, class_name, sizes, dimensions, current_dim) {
    if (current_dim >= dimensions) return null;

    const array = _jvm_new_array(jvm, 0x5B /* DESC_ARRAY */, sizes[current_dim], null);
    if (!array) return null;

    if (current_dim === dimensions - 1) {
        let elem_type = 10; // T_INT default
        if (class_name) {
            let tp = 0;
            while (tp < class_name.length && class_name[tp] === '[') tp++;
            const ch = class_name[tp];
            if (ch === 'L') elem_type = 0x4C;
            else {
                const tm = { 'Z': 4, 'B': 8, 'C': 5, 'S': 9, 'I': 10, 'J': 11, 'F': 6, 'D': 7 };
                if (tm[ch] !== undefined) elem_type = tm[ch];
            }
        }
        return _jvm_new_array(jvm, elem_type, sizes[current_dim], null);
    }

    for (let i = 0; i < sizes[current_dim]; i++) {
        const inner = create_multidim_array(jvm, class_name, sizes, dimensions, current_dim + 1);
        if (inner) _array_set_ref(array, i, inner);
    }
    return array;
}

// ── op_multianewarray ─────────────────────────────────────────────────────────
export function op_multianewarray(jvm, thread, frame) {
    const class_index = fetch_u2(frame);
    const dimensions  = fetch_u1(frame);

    let class_name = null;
    if (class_index > 0 && class_index < frame.clazz.constant_pool.length) {
        class_name = _classfile_get_class_name(frame.clazz, class_index);
    }

    const sizes = new Array(dimensions);
    for (let i = dimensions - 1; i >= 0; i--) {
        sizes[i] = frame_pop(frame).i;
        if (sizes[i] < 0) { _native_throw_negative_array_size(jvm, thread); return -1; }
    }

    // Determine element type
    let elem_type = 10, element_class = null;
    if (class_name) {
        let tp = 0;
        while (tp < class_name.length && class_name[tp] === '[') tp++;
        const ch = class_name[tp];
        if (ch === 'L') {
            elem_type = 0x4C;
            const semi = class_name.indexOf(';', tp);
            if (semi > tp + 1) element_class = _jvm_load_class(jvm, class_name.substring(tp + 1, semi));
        } else if (ch === '[') {
            elem_type = 0x5B;
        } else {
            const tm = { 'Z': 4, 'B': 8, 'C': 5, 'S': 9, 'I': 10, 'J': 11, 'F': 6, 'D': 7 };
            if (tm[ch] !== undefined) elem_type = tm[ch];
        }
    }

    const outer_array = _jvm_new_array(jvm, 0x5B /* DESC_ARRAY */, sizes[0], null);
    if (!outer_array) { frame_push(frame, jv_null()); return 0; }

    if (dimensions === 1) {
        const result = _jvm_new_array(jvm, elem_type, sizes[0], element_class);
        frame_push(frame, { i: 0, j: 0n, f: 0, d: 0, ref: result });
    } else if (dimensions === 2) {
        for (let i = 0; i < sizes[0]; i++) {
            const inner = _jvm_new_array(jvm, elem_type, sizes[1], element_class);
            _array_set_ref(outer_array, i, inner);
        }
        frame_push(frame, { i: 0, j: 0n, f: 0, d: 0, ref: outer_array });
    } else {
        for (let i = 0; i < sizes[0]; i++) {
            const row = _jvm_new_array(jvm, 0x5B, sizes[1], null);
            if (!row) continue;
            if (dimensions === 3) {
                for (let j = 0; j < sizes[1]; j++) {
                    const inner = _jvm_new_array(jvm, elem_type, sizes[2], element_class);
                    _array_set_ref(row, j, inner);
                }
            } else {
                for (let j = 0; j < sizes[1]; j++) {
                    const inner = create_multidim_array(jvm, class_name, sizes, dimensions, 2);
                    _array_set_ref(row, j, inner);
                }
            }
            _array_set_ref(outer_array, i, row);
        }
        frame_push(frame, { i: 0, j: 0n, f: 0, d: 0, ref: outer_array });
    }
    return 0;
}

// ── op_ifnull / op_ifnonnull ──────────────────────────────────────────────────
export function op_ifnull(jvm, thread, frame) {
    const offset = fetch_s2(frame);
    const value  = frame_pop(frame).ref;
    if (value === null || value === undefined) frame.pc += offset - 3;
    return 0;
}

export function op_ifnonnull(jvm, thread, frame) {
    const offset = fetch_s2(frame);
    const value  = frame_pop(frame).ref;
    if (value !== null && value !== undefined) frame.pc += offset - 3;
    return 0;
}

// ── op_goto_w ─────────────────────────────────────────────────────────────────
export function op_goto_w(jvm, thread, frame) {
    // Read signed 32-bit big-endian offset from current PC position
    const u = ((frame.code[frame.pc] & 0xFF) << 24) |
              ((frame.code[frame.pc + 1] & 0xFF) << 16) |
              ((frame.code[frame.pc + 2] & 0xFF) << 8) |
              (frame.code[frame.pc + 3] & 0xFF);
    const offset = u | 0; // to signed
    frame.pc += 4 + offset - 5;
    return 0;
}

// ── op_jsr_w ──────────────────────────────────────────────────────────────────
export function op_jsr_w(jvm, thread, frame) {
    const u = ((frame.code[frame.pc] & 0xFF) << 24) |
              ((frame.code[frame.pc + 1] & 0xFF) << 16) |
              ((frame.code[frame.pc + 2] & 0xFF) << 8) |
              (frame.code[frame.pc + 3] & 0xFF);
    const offset = u | 0;
    // Push return address (pc + 4, i.e. after the 4-byte offset)
    frame_push(frame, { i: frame.pc + 4, j: 0n, f: 0, d: 0, ref: null });
    frame.pc += offset;
    return 0;
}

// ── Local frame helpers (used by opcodes above) ───────────────────────────────
function frame_push(frame, val) {
    frame.stack[++frame.stack_top] = val;
}

function frame_pop(frame) {
    if (frame.stack_top < 0) {
        if (g_j2me_runtime_debug) console.log('[EXEC-UNDERFLOW] stack underflow at PC=' + frame.pc);
        return jv_zero();
    }
    return frame.stack[frame.stack_top--];
}

function load_local_v(frame, idx) {
    if (idx >= frame.locals.length) return null;
    return frame.locals[idx];
}

function store_local_v(frame, idx, v) {
    if (idx >= frame.locals.length) return -1;
    frame.locals[idx] = v;
    return 0;
}

function jv_zero() { return { i: 0, j: 0n, f: 0, d: 0, ref: null }; }
function jv_null() { return { i: 0, j: 0n, f: 0, d: 0, ref: null }; }

// ── Opcode dispatch table (256 entries) ───────────────────────────────────────
export const opcode_table = new Array(256).fill(null).map((_, i) => ({
    name: 'unknown_0x' + i.toString(16),
    handler: null
}));

opcode_table[OPC_NOP]           = { name: 'nop',           handler: op_nop };
opcode_table[OPC_ACONST_NULL]   = { name: 'aconst_null',   handler: op_aconst_null };
opcode_table[OPC_ICONST_M1]     = { name: 'iconst_m1',     handler: op_iconst };
opcode_table[OPC_ICONST_0]      = { name: 'iconst_0',      handler: op_iconst };
opcode_table[OPC_ICONST_1]      = { name: 'iconst_1',      handler: op_iconst };
opcode_table[OPC_ICONST_2]      = { name: 'iconst_2',      handler: op_iconst };
opcode_table[OPC_ICONST_3]      = { name: 'iconst_3',      handler: op_iconst };
opcode_table[OPC_ICONST_4]      = { name: 'iconst_4',      handler: op_iconst };
opcode_table[OPC_ICONST_5]      = { name: 'iconst_5',      handler: op_iconst };
opcode_table[OPC_LCONST_0]      = { name: 'lconst_0',      handler: op_lconst };
opcode_table[OPC_LCONST_1]      = { name: 'lconst_1',      handler: op_lconst };
opcode_table[OPC_FCONST_0]      = { name: 'fconst_0',      handler: op_fconst };
opcode_table[OPC_FCONST_1]      = { name: 'fconst_1',      handler: op_fconst };
opcode_table[OPC_FCONST_2]      = { name: 'fconst_2',      handler: op_fconst };
opcode_table[OPC_DCONST_0]      = { name: 'dconst_0',      handler: op_dconst };
opcode_table[OPC_DCONST_1]      = { name: 'dconst_1',      handler: op_dconst };
opcode_table[OPC_BIPUSH]        = { name: 'bipush',         handler: op_bipush };
opcode_table[OPC_SIPUSH]        = { name: 'sipush',         handler: op_sipush };
opcode_table[OPC_LDC]           = { name: 'ldc',            handler: op_ldc };
opcode_table[OPC_LDC_W]         = { name: 'ldc_w',          handler: op_ldc_w };
opcode_table[OPC_LDC2_W]        = { name: 'ldc2_w',         handler: op_ldc2_w };
opcode_table[OPC_ILOAD]         = { name: 'iload',          handler: op_iload };
opcode_table[OPC_LLOAD]         = { name: 'lload',          handler: op_lload };
opcode_table[OPC_FLOAD]         = { name: 'fload',          handler: op_fload };
opcode_table[OPC_DLOAD]         = { name: 'dload',          handler: op_dload };
opcode_table[OPC_ALOAD]         = { name: 'aload',          handler: op_aload };
opcode_table[OPC_ILOAD_0]       = { name: 'iload_0',        handler: op_load_0 };
opcode_table[OPC_ILOAD_1]       = { name: 'iload_1',        handler: op_load_1 };
opcode_table[OPC_ILOAD_2]       = { name: 'iload_2',        handler: op_load_2 };
opcode_table[OPC_ILOAD_3]       = { name: 'iload_3',        handler: op_load_3 };
opcode_table[OPC_LLOAD_0]       = { name: 'lload_0',        handler: op_lload_0 };
opcode_table[OPC_LLOAD_1]       = { name: 'lload_1',        handler: op_lload_1 };
opcode_table[OPC_LLOAD_2]       = { name: 'lload_2',        handler: op_lload_2 };
opcode_table[OPC_LLOAD_3]       = { name: 'lload_3',        handler: op_lload_3 };
opcode_table[OPC_FLOAD_0]       = { name: 'fload_0',        handler: op_load_0 };
opcode_table[OPC_FLOAD_1]       = { name: 'fload_1',        handler: op_load_1 };
opcode_table[OPC_FLOAD_2]       = { name: 'fload_2',        handler: op_load_2 };
opcode_table[OPC_FLOAD_3]       = { name: 'fload_3',        handler: op_load_3 };
opcode_table[OPC_DLOAD_0]       = { name: 'dload_0',        handler: op_dload_0 };
opcode_table[OPC_DLOAD_1]       = { name: 'dload_1',        handler: op_dload_1 };
opcode_table[OPC_DLOAD_2]       = { name: 'dload_2',        handler: op_dload_2 };
opcode_table[OPC_DLOAD_3]       = { name: 'dload_3',        handler: op_dload_3 };
opcode_table[OPC_ALOAD_0]       = { name: 'aload_0',        handler: op_load_0 };
opcode_table[OPC_ALOAD_1]       = { name: 'aload_1',        handler: op_load_1 };
opcode_table[OPC_ALOAD_2]       = { name: 'aload_2',        handler: op_load_2 };
opcode_table[OPC_ALOAD_3]       = { name: 'aload_3',        handler: op_load_3 };
opcode_table[OPC_IALOAD]        = { name: 'iaload',         handler: op_array_load };
opcode_table[OPC_LALOAD]        = { name: 'laload',         handler: op_array_load };
opcode_table[OPC_FALOAD]        = { name: 'faload',         handler: op_array_load };
opcode_table[OPC_DALOAD]        = { name: 'daload',         handler: op_array_load };
opcode_table[OPC_AALOAD]        = { name: 'aaload',         handler: op_array_load };
opcode_table[OPC_BALOAD]        = { name: 'baload',         handler: op_array_load };
opcode_table[OPC_CALOAD]        = { name: 'caload',         handler: op_array_load };
opcode_table[OPC_SALOAD]        = { name: 'saload',         handler: op_array_load };
opcode_table[OPC_ISTORE]        = { name: 'istore',         handler: op_store };
opcode_table[OPC_LSTORE]        = { name: 'lstore',         handler: op_lstore };
opcode_table[OPC_FSTORE]        = { name: 'fstore',         handler: op_store };
opcode_table[OPC_DSTORE]        = { name: 'dstore',         handler: op_dstore };
opcode_table[OPC_ASTORE]        = { name: 'astore',         handler: op_store };
opcode_table[OPC_ISTORE_0]      = { name: 'istore_0',       handler: op_store_0 };
opcode_table[OPC_ISTORE_1]      = { name: 'istore_1',       handler: op_store_1 };
opcode_table[OPC_ISTORE_2]      = { name: 'istore_2',       handler: op_store_2 };
opcode_table[OPC_ISTORE_3]      = { name: 'istore_3',       handler: op_store_3 };
opcode_table[OPC_LSTORE_0]      = { name: 'lstore_0',       handler: op_lstore_0 };
opcode_table[OPC_LSTORE_1]      = { name: 'lstore_1',       handler: op_lstore_1 };
opcode_table[OPC_LSTORE_2]      = { name: 'lstore_2',       handler: op_lstore_2 };
opcode_table[OPC_LSTORE_3]      = { name: 'lstore_3',       handler: op_lstore_3 };
opcode_table[OPC_FSTORE_0]      = { name: 'fstore_0',       handler: op_store_0 };
opcode_table[OPC_FSTORE_1]      = { name: 'fstore_1',       handler: op_store_1 };
opcode_table[OPC_FSTORE_2]      = { name: 'fstore_2',       handler: op_store_2 };
opcode_table[OPC_FSTORE_3]      = { name: 'fstore_3',       handler: op_store_3 };
opcode_table[OPC_DSTORE_0]      = { name: 'dstore_0',       handler: op_dstore_0 };
opcode_table[OPC_DSTORE_1]      = { name: 'dstore_1',       handler: op_dstore_1 };
opcode_table[OPC_DSTORE_2]      = { name: 'dstore_2',       handler: op_dstore_2 };
opcode_table[OPC_DSTORE_3]      = { name: 'dstore_3',       handler: op_dstore_3 };
opcode_table[OPC_ASTORE_0]      = { name: 'astore_0',       handler: op_store_0 };
opcode_table[OPC_ASTORE_1]      = { name: 'astore_1',       handler: op_store_1 };
opcode_table[OPC_ASTORE_2]      = { name: 'astore_2',       handler: op_store_2 };
opcode_table[OPC_ASTORE_3]      = { name: 'astore_3',       handler: op_store_3 };
opcode_table[OPC_IASTORE]       = { name: 'iastore',        handler: op_array_store };
opcode_table[OPC_LASTORE]       = { name: 'lastore',        handler: op_array_store };
opcode_table[OPC_FASTORE]       = { name: 'fastore',        handler: op_array_store };
opcode_table[OPC_DASTORE]       = { name: 'dastore',        handler: op_array_store };
opcode_table[OPC_AASTORE]       = { name: 'aastore',        handler: op_array_store };
opcode_table[OPC_BASTORE]       = { name: 'bastore',        handler: op_array_store };
opcode_table[OPC_CASTORE]       = { name: 'castore',        handler: op_array_store };
opcode_table[OPC_SASTORE]       = { name: 'sastore',        handler: op_array_store };
opcode_table[OPC_POP]           = { name: 'pop',            handler: op_pop };
opcode_table[OPC_POP2]          = { name: 'pop2',           handler: op_pop2 };
opcode_table[OPC_DUP]           = { name: 'dup',            handler: op_dup };
opcode_table[OPC_DUP_X1]        = { name: 'dup_x1',         handler: op_dup_x1 };
opcode_table[OPC_DUP_X2]        = { name: 'dup_x2',         handler: op_dup_x2 };
opcode_table[OPC_DUP2]          = { name: 'dup2',           handler: op_dup2 };
opcode_table[OPC_DUP2_X1]       = { name: 'dup2_x1',        handler: op_dup2_x1 };
opcode_table[OPC_DUP2_X2]       = { name: 'dup2_x2',        handler: op_dup2_x2 };
opcode_table[OPC_SWAP]          = { name: 'swap',           handler: op_swap };
opcode_table[OPC_IADD]          = { name: 'iadd',           handler: op_add };
opcode_table[OPC_LADD]          = { name: 'ladd',           handler: op_add };
opcode_table[OPC_FADD]          = { name: 'fadd',           handler: op_add };
opcode_table[OPC_DADD]          = { name: 'dadd',           handler: op_add };
opcode_table[OPC_ISUB]          = { name: 'isub',           handler: op_sub };
opcode_table[OPC_LSUB]          = { name: 'lsub',           handler: op_sub };
opcode_table[OPC_FSUB]          = { name: 'fsub',           handler: op_sub };
opcode_table[OPC_DSUB]          = { name: 'dsub',           handler: op_sub };
opcode_table[OPC_IMUL]          = { name: 'imul',           handler: op_mul };
opcode_table[OPC_LMUL]          = { name: 'lmul',           handler: op_mul };
opcode_table[OPC_FMUL]          = { name: 'fmul',           handler: op_mul };
opcode_table[OPC_DMUL]          = { name: 'dmul',           handler: op_mul };
opcode_table[OPC_IDIV]          = { name: 'idiv',           handler: op_div };
opcode_table[OPC_LDIV]          = { name: 'ldiv',           handler: op_div };
opcode_table[OPC_FDIV]          = { name: 'fdiv',           handler: op_div };
opcode_table[OPC_DDIV]          = { name: 'ddiv',           handler: op_div };
opcode_table[OPC_IREM]          = { name: 'irem',           handler: op_rem };
opcode_table[OPC_LREM]          = { name: 'lrem',           handler: op_rem };
opcode_table[OPC_FREM]          = { name: 'frem',           handler: op_rem };
opcode_table[OPC_DREM]          = { name: 'drem',           handler: op_rem };
opcode_table[OPC_INEG]          = { name: 'ineg',           handler: op_neg };
opcode_table[OPC_LNEG]          = { name: 'lneg',           handler: op_neg };
opcode_table[OPC_FNEG]          = { name: 'fneg',           handler: op_neg };
opcode_table[OPC_DNEG]          = { name: 'dneg',           handler: op_neg };
opcode_table[OPC_ISHL]          = { name: 'ishl',           handler: op_shl };
opcode_table[OPC_LSHL]          = { name: 'lshl',           handler: op_shl };
opcode_table[OPC_ISHR]          = { name: 'ishr',           handler: op_shr };
opcode_table[OPC_LSHR]          = { name: 'lshr',           handler: op_shr };
opcode_table[OPC_IUSHR]         = { name: 'iushr',          handler: op_ushr };
opcode_table[OPC_LUSHR]         = { name: 'lushr',          handler: op_ushr };
opcode_table[OPC_IAND]          = { name: 'iand',           handler: op_and };
opcode_table[OPC_LAND]          = { name: 'land',           handler: op_and };
opcode_table[OPC_IOR]           = { name: 'ior',            handler: op_or };
opcode_table[OPC_LOR]           = { name: 'lor',            handler: op_or };
opcode_table[OPC_IXOR]          = { name: 'ixor',           handler: op_xor };
opcode_table[OPC_LXOR]          = { name: 'lxor',           handler: op_xor };
opcode_table[OPC_IINC]          = { name: 'iinc',           handler: op_iinc };
opcode_table[OPC_I2L]           = { name: 'i2l',            handler: op_convert };
opcode_table[OPC_I2F]           = { name: 'i2f',            handler: op_convert };
opcode_table[OPC_I2D]           = { name: 'i2d',            handler: op_convert };
opcode_table[OPC_L2I]           = { name: 'l2i',            handler: op_convert };
opcode_table[OPC_L2F]           = { name: 'l2f',            handler: op_convert };
opcode_table[OPC_L2D]           = { name: 'l2d',            handler: op_convert };
opcode_table[OPC_F2I]           = { name: 'f2i',            handler: op_convert };
opcode_table[OPC_F2L]           = { name: 'f2l',            handler: op_convert };
opcode_table[OPC_F2D]           = { name: 'f2d',            handler: op_convert };
opcode_table[OPC_D2I]           = { name: 'd2i',            handler: op_convert };
opcode_table[OPC_D2L]           = { name: 'd2l',            handler: op_convert };
opcode_table[OPC_D2F]           = { name: 'd2f',            handler: op_convert };
opcode_table[OPC_I2B]           = { name: 'i2b',            handler: op_convert };
opcode_table[OPC_I2C]           = { name: 'i2c',            handler: op_convert };
opcode_table[OPC_I2S]           = { name: 'i2s',            handler: op_convert };
opcode_table[OPC_LCMP]          = { name: 'lcmp',           handler: op_lcmp };
opcode_table[OPC_FCMPL]         = { name: 'fcmpl',          handler: op_fcmpl };
opcode_table[OPC_FCMPG]         = { name: 'fcmpg',          handler: op_fcmpg };
opcode_table[OPC_DCMPL]         = { name: 'dcmpl',          handler: op_dcmpl };
opcode_table[OPC_DCMPG]         = { name: 'dcmpg',          handler: op_dcmpg };
opcode_table[OPC_IFEQ]          = { name: 'ifeq',           handler: op_if };
opcode_table[OPC_IFNE]          = { name: 'ifne',           handler: op_if };
opcode_table[OPC_IFLT]          = { name: 'iflt',           handler: op_if };
opcode_table[OPC_IFGE]          = { name: 'ifge',           handler: op_if };
opcode_table[OPC_IFGT]          = { name: 'ifgt',           handler: op_if };
opcode_table[OPC_IFLE]          = { name: 'ifle',           handler: op_if };
opcode_table[OPC_IF_ICMPEQ]     = { name: 'if_icmpeq',      handler: op_if_icmp };
opcode_table[OPC_IF_ICMPNE]     = { name: 'if_icmpne',      handler: op_if_icmp };
opcode_table[OPC_IF_ICMPLT]     = { name: 'if_icmplt',      handler: op_if_icmp };
opcode_table[OPC_IF_ICMPGE]     = { name: 'if_icmpge',      handler: op_if_icmp };
opcode_table[OPC_IF_ICMPGT]     = { name: 'if_icmpgt',      handler: op_if_icmp };
opcode_table[OPC_IF_ICMPLE]     = { name: 'if_icmple',      handler: op_if_icmp };
opcode_table[OPC_IF_ACMPEQ]     = { name: 'if_acmpeq',      handler: op_if_acmp };
opcode_table[OPC_IF_ACMPNE]     = { name: 'if_acmpne',      handler: op_if_acmp };
opcode_table[OPC_GOTO]          = { name: 'goto',           handler: op_goto };
opcode_table[OPC_JSR]           = { name: 'jsr',            handler: op_jsr };
opcode_table[OPC_RET]           = { name: 'ret',            handler: op_ret };
opcode_table[OPC_TABLESWITCH]   = { name: 'tableswitch',    handler: op_tableswitch };
opcode_table[OPC_LOOKUPSWITCH]  = { name: 'lookupswitch',   handler: op_lookupswitch };
opcode_table[OPC_IRETURN]       = { name: 'ireturn',        handler: op_ireturn };
opcode_table[OPC_LRETURN]       = { name: 'lreturn',        handler: op_lreturn };
opcode_table[OPC_FRETURN]       = { name: 'freturn',        handler: op_freturn };
opcode_table[OPC_DRETURN]       = { name: 'dreturn',        handler: op_dreturn };
opcode_table[OPC_ARETURN]       = { name: 'areturn',        handler: op_areturn };
opcode_table[OPC_RETURN]        = { name: 'return',         handler: op_return };
opcode_table[OPC_GETSTATIC]     = { name: 'getstatic',      handler: op_getstatic };
opcode_table[OPC_PUTSTATIC]     = { name: 'putstatic',      handler: op_putstatic };
opcode_table[OPC_GETFIELD]      = { name: 'getfield',       handler: op_getfield };
opcode_table[OPC_PUTFIELD]      = { name: 'putfield',       handler: op_putfield };
opcode_table[OPC_INVOKEVIRTUAL]   = { name: 'invokevirtual',   handler: op_invokevirtual };
opcode_table[OPC_INVOKESPECIAL]   = { name: 'invokespecial',   handler: op_invokespecial };
opcode_table[OPC_INVOKESTATIC]    = { name: 'invokestatic',    handler: op_invokestatic };
opcode_table[OPC_INVOKEINTERFACE] = { name: 'invokeinterface', handler: op_invokeinterface };
opcode_table[OPC_NEW]             = { name: 'new',             handler: op_new };
opcode_table[OPC_NEWARRAY]        = { name: 'newarray',        handler: op_newarray };
opcode_table[OPC_ANEWARRAY]       = { name: 'anewarray',       handler: op_anewarray };
opcode_table[OPC_ARRAYLENGTH]     = { name: 'arraylength',     handler: op_arraylength };
opcode_table[OPC_ATHROW]          = { name: 'athrow',          handler: op_athrow };
opcode_table[OPC_CHECKCAST]       = { name: 'checkcast',       handler: op_checkcast };
opcode_table[OPC_INSTANCEOF]      = { name: 'instanceof',      handler: op_instanceof };
opcode_table[OPC_MONITORENTER]    = { name: 'monitorenter',    handler: op_monitorenter };
opcode_table[OPC_MONITOREXIT]     = { name: 'monitorexit',     handler: op_monitorexit };
opcode_table[OPC_WIDE]            = { name: 'wide',            handler: op_wide };
opcode_table[OPC_MULTIANEWARRAY]  = { name: 'multianewarray',  handler: op_multianewarray };
opcode_table[OPC_IFNULL]          = { name: 'ifnull',          handler: op_ifnull };
opcode_table[OPC_IFNONNULL]       = { name: 'ifnonnull',       handler: op_ifnonnull };
opcode_table[OPC_GOTO_W]          = { name: 'goto_w',          handler: op_goto_w };
opcode_table[OPC_JSR_W]           = { name: 'jsr_w',           handler: op_jsr_w };

// ── opcodes_init ──────────────────────────────────────────────────────────────
export function opcodes_init() {
    // Opcode table is statically initialized above.
    // Callbacks must be injected via set_opcodes_callbacks() before dispatch.
}
