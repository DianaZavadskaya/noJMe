import { readFileSync } from 'fs';
import { g_j2me_runtime_debug } from './debug_var.mjs';

// ============================================================
// Constants from classfile.h
// ============================================================

export const CLASS_FILE_MAGIC = 0xCAFEBABE;

export const CLASS_VERSION_MIN  = 45;
export const CLASS_VERSION_1_1  = 45;
export const CLASS_VERSION_1_2  = 46;
export const CLASS_VERSION_1_3  = 47;
export const CLASS_VERSION_1_4  = 48;
export const CLASS_VERSION_5_0  = 49;
export const CLASS_VERSION_6_0  = 50;
export const CLASS_VERSION_7_0  = 51;
export const CLASS_VERSION_8_0  = 52;

// Attribute type enum (ATTR_*)
export const AttributeType = Object.freeze({
    SourceFile:                      0,
    InnerClasses:                    1,
    Synthetic:                       2,
    Deprecated:                      3,
    Code:                            4,
    Exceptions:                      5,
    LineNumberTable:                 6,
    LocalVariableTable:              7,
    LocalVariableTypeTable:          8,
    ConstantValue:                   9,
    Signature:                       10,
    RuntimeVisibleAnnotations:       11,
    RuntimeInvisibleAnnotations:     12,
    Unknown:                         13,
});

// Constant pool tags (from jvm.h ConstantPoolTag enum)
export const CONSTANT_Utf8               = 1;
export const CONSTANT_Integer            = 3;
export const CONSTANT_Float              = 4;
export const CONSTANT_Long               = 5;
export const CONSTANT_Double             = 6;
export const CONSTANT_Class              = 7;
export const CONSTANT_String             = 8;
export const CONSTANT_Fieldref           = 9;
export const CONSTANT_Methodref          = 10;
export const CONSTANT_InterfaceMethodref = 11;
export const CONSTANT_NameAndType        = 12;
export const CONSTANT_MethodHandle       = 15;
export const CONSTANT_MethodType         = 16;
export const CONSTANT_Dynamic            = 17;
export const CONSTANT_InvokeDynamic      = 18;
export const CONSTANT_Module             = 19;
export const CONSTANT_Package            = 20;

// Access flags (from opcodes.h)
export const ACC_PUBLIC      = 0x0001;
export const ACC_PRIVATE     = 0x0002;
export const ACC_PROTECTED   = 0x0004;
export const ACC_STATIC      = 0x0008;
export const ACC_FINAL       = 0x0010;
export const ACC_SYNCHRONIZED= 0x0020;
export const ACC_SUPER       = 0x0020;
export const ACC_VOLATILE    = 0x0040;
export const ACC_TRANSIENT   = 0x0080;
export const ACC_NATIVE      = 0x0100;
export const ACC_INTERFACE   = 0x0200;
export const ACC_ABSTRACT    = 0x0400;
export const ACC_STRICT      = 0x0800;
export const ACC_SYNTHETIC   = 0x1000;
export const ACC_ANNOTATION  = 0x2000;
export const ACC_ENUM        = 0x4000;

// sizeof(ObjectHeader) in the C code is 16 bytes (padded).
// sizeof(JavaValue) in C is 8 bytes (largest union member is uint64_t/jdouble).
const SIZEOF_OBJECT_HEADER = 16;
const SIZEOF_JAVA_VALUE    = 8;

// ============================================================
// ClassFileReader
// Mirrors ClassFileReader* from classfile.h — plain object here.
// ============================================================

/**
 * Create a ClassFileReader wrapping a Uint8Array.
 *
 * @param {Uint8Array} data
 * @returns {{ data: Uint8Array, length: number, position: number, error: string|null, error_position: number }}
 */
export function classfile_reader_create(data) {
    return {
        data:           data,
        length:         data.length,
        position:       0,
        error:          null,
        error_position: 0,
    };
}

/**
 * No-op in JS — GC handles memory.
 * Exported for API symmetry.
 *
 * @param {Object} _reader
 */
export function classfile_reader_destroy(_reader) {
    // no-op
}

// ============================================================
// Low-level read helpers
// All return 0 on success, -1 on failure (C convention).
// Out-params are returned as { rc, value } to avoid pointer simulation.
// Internal callers use the _ret variants that mutate the reader in place.
// ============================================================

/**
 * Read a single byte.  Returns { rc: 0|(-1), value: number }.
 *
 * @param {Object} reader
 * @returns {{ rc: number, value: number }}
 */
export function classfile_read_u1(reader, outRef) {
    if (reader.position >= reader.length) {
        reader.error          = 'Unexpected end of data';
        reader.error_position = reader.position;
        if (outRef) outRef.value = 0;
        return -1;
    }
    const v = reader.data[reader.position++];
    if (outRef) outRef.value = v;
    return 0;
}

/**
 * Read a big-endian 16-bit unsigned integer.
 *
 * @param {Object} reader
 * @param {{ value: number }} outRef
 * @returns {number} 0 or -1
 */
export function classfile_read_u2(reader, outRef) {
    if (reader.position + 2 > reader.length) {
        reader.error          = 'Unexpected end of data';
        reader.error_position = reader.position;
        if (outRef) outRef.value = 0;
        return -1;
    }
    const d = reader.data;
    const p = reader.position;
    const v = ((d[p] << 8) | d[p + 1]) >>> 0;
    reader.position += 2;
    if (outRef) outRef.value = v;
    return 0;
}

/**
 * Read a big-endian 32-bit unsigned integer.
 *
 * @param {Object} reader
 * @param {{ value: number }} outRef
 * @returns {number} 0 or -1
 */
export function classfile_read_u4(reader, outRef) {
    if (reader.position + 4 > reader.length) {
        reader.error          = 'Unexpected end of data';
        reader.error_position = reader.position;
        if (outRef) outRef.value = 0;
        return -1;
    }
    const d = reader.data;
    const p = reader.position;
    const v = (((d[p] * 0x1000000) + ((d[p+1] << 16) | (d[p+2] << 8) | d[p+3])) >>> 0);
    reader.position += 4;
    if (outRef) outRef.value = v;
    return 0;
}

/**
 * Read `count` bytes into a new Uint8Array.
 * In C this writes into an existing buffer; here we return the slice.
 *
 * @param {Object} reader
 * @param {number} count
 * @returns {{ rc: number, bytes: Uint8Array|null }}
 */
function _read_bytes(reader, count) {
    if (reader.position + count > reader.length) {
        reader.error          = 'Unexpected end of data';
        reader.error_position = reader.position;
        return { rc: -1, bytes: null };
    }
    const bytes = reader.data.slice(reader.position, reader.position + count);
    reader.position += count;
    return { rc: 0, bytes };
}

/**
 * Read bytes into a pre-existing Uint8Array (public API variant).
 *
 * @param {Object}     reader
 * @param {Uint8Array} buffer
 * @param {number}     count
 * @returns {number} 0 or -1
 */
export function classfile_read_bytes(reader, buffer, count) {
    if (reader.position + count > reader.length) {
        reader.error          = 'Unexpected end of data';
        reader.error_position = reader.position;
        return -1;
    }
    buffer.set(reader.data.subarray(reader.position, reader.position + count));
    reader.position += count;
    return 0;
}

/**
 * Skip `count` bytes.
 *
 * @param {Object} reader
 * @param {number} count
 * @returns {number} 0 or -1
 */
export function classfile_skip(reader, count) {
    if (reader.position + count > reader.length) {
        reader.error          = 'Unexpected end of data';
        reader.error_position = reader.position;
        return -1;
    }
    reader.position += count;
    return 0;
}

/**
 * Return current read position.
 *
 * @param {Object} reader
 * @returns {number}
 */
export function classfile_position(reader) {
    return reader.position;
}

/**
 * Return number of bytes remaining.
 *
 * @param {Object} reader
 * @returns {number}
 */
export function classfile_remaining(reader) {
    return reader.length - reader.position;
}

// ============================================================
// Constant Pool
// ============================================================

/**
 * Parse the constant pool from the reader into clazz.
 * Entries are plain JS objects; utf8 bytes are stored as JS strings.
 *
 * @param {Object} reader
 * @param {Object} clazz   - JavaClass plain object (mutated)
 * @returns {number} 0 or -1
 */
export function classfile_parse_constant_pool(reader, clazz) {
    const cpCountRef = { value: 0 };
    if (classfile_read_u2(reader, cpCountRef) !== 0) return -1;

    const count = cpCountRef.value;
    if (g_j2me_runtime_debug.value) {
        process.stderr.write(`[CP DEBUG] constant_pool_count = ${count}\n`);
    }

    clazz.constant_pool_count    = count;
    clazz.constant_pool_capacity = count;

    // Index 0 is unused per JVMS; allocate slot 0 as null placeholder.
    const cp = new Array(count).fill(null);
    clazz.constant_pool = cp;

    const tagRef  = { value: 0 };
    const u2Ref   = { value: 0 };
    const u4Ref   = { value: 0 };
    const u4bRef  = { value: 0 };

    for (let i = 1; i < count; i++) {
        if (classfile_read_u1(reader, tagRef) !== 0) return -1;
        const tag = tagRef.value;

        if (i <= 20 && g_j2me_runtime_debug.value) {
            process.stderr.write(`[CP DEBUG] entry[${i}].tag = ${tag}\n`);
        }

        let entry;
        switch (tag) {
            case CONSTANT_Utf8: {
                if (classfile_read_u2(reader, u2Ref) !== 0) return -1;
                const len = u2Ref.value;
                const { rc, bytes } = _read_bytes(reader, len);
                if (rc !== 0) return -1;
                // Decode as MUTF-8 (for J2ME purposes plain UTF-8 suffices)
                const str = new TextDecoder('utf-8').decode(bytes);
                entry = { tag, info: { utf8: { length: len, bytes: str } } };
                break;
            }
            case CONSTANT_Integer: {
                if (classfile_read_u4(reader, u4Ref) !== 0) return -1;
                // Interpret as signed int32
                const signed = u4Ref.value | 0;
                entry = { tag, info: { integer: { value: signed } } };
                break;
            }
            case CONSTANT_Float: {
                if (classfile_read_u4(reader, u4Ref) !== 0) return -1;
                // Reinterpret bit pattern as IEEE 754 float
                const buf = new ArrayBuffer(4);
                new DataView(buf).setUint32(0, u4Ref.value, false);
                const fval = new DataView(buf).getFloat32(0, false);
                entry = { tag, info: { float_val: { value: fval } } };
                break;
            }
            case CONSTANT_Long: {
                if (classfile_read_u4(reader, u4Ref)  !== 0) return -1;
                if (classfile_read_u4(reader, u4bRef) !== 0) return -1;
                // Store as BigInt to preserve full 64-bit precision
                const high = BigInt(u4Ref.value);
                const low  = BigInt(u4bRef.value);
                const lval = BigInt.asIntN(64, (high << 32n) | low);
                entry = { tag, info: { long_val: { value: lval } } };
                // Long occupies two constant pool slots
                cp[i] = entry;
                i++;
                cp[i] = null;
                continue;
            }
            case CONSTANT_Double: {
                if (classfile_read_u4(reader, u4Ref)  !== 0) return -1;
                if (classfile_read_u4(reader, u4bRef) !== 0) return -1;
                const buf = new ArrayBuffer(8);
                const dv  = new DataView(buf);
                dv.setUint32(0, u4Ref.value,  false);
                dv.setUint32(4, u4bRef.value, false);
                const dval = dv.getFloat64(0, false);
                entry = { tag, info: { double_val: { value: dval } } };
                // Double occupies two constant pool slots
                cp[i] = entry;
                i++;
                cp[i] = null;
                continue;
            }
            case CONSTANT_Class: {
                if (classfile_read_u2(reader, u2Ref) !== 0) return -1;
                entry = { tag, info: { class_info: { name_index: u2Ref.value } } };
                break;
            }
            case CONSTANT_String: {
                if (classfile_read_u2(reader, u2Ref) !== 0) return -1;
                entry = { tag, info: { string_info: { string_index: u2Ref.value } } };
                break;
            }
            case CONSTANT_Fieldref:
            case CONSTANT_Methodref:
            case CONSTANT_InterfaceMethodref: {
                const ciRef  = { value: 0 };
                const natRef = { value: 0 };
                if (classfile_read_u2(reader, ciRef)  !== 0) return -1;
                if (classfile_read_u2(reader, natRef) !== 0) return -1;
                entry = { tag, info: { ref_info: { class_index: ciRef.value, name_and_type_index: natRef.value } } };
                break;
            }
            case CONSTANT_NameAndType: {
                const niRef  = { value: 0 };
                const diRef  = { value: 0 };
                if (classfile_read_u2(reader, niRef) !== 0) return -1;
                if (classfile_read_u2(reader, diRef) !== 0) return -1;
                entry = { tag, info: { name_and_type: { name_index: niRef.value, descriptor_index: diRef.value } } };
                break;
            }
            default:
                process.stderr.write(`Unknown constant pool tag: ${tag} at index ${i}\n`);
                return -1;
        }

        cp[i] = entry;
    }

    return 0;
}

// ============================================================
// Constant pool accessors
// ============================================================

/**
 * Get UTF-8 string from constant pool at index.
 *
 * @param {Object} clazz
 * @param {number} index
 * @returns {string|null}
 */
export function classfile_get_utf8(clazz, index) {
    if (index === 0 || index >= clazz.constant_pool_count) return null;
    const entry = clazz.constant_pool[index];
    if (!entry || entry.tag !== CONSTANT_Utf8) return null;
    return entry.info.utf8.bytes;
}

/**
 * Get the class name string from a CONSTANT_Class entry.
 *
 * @param {Object} clazz
 * @param {number} index
 * @returns {string|null}
 */
export function classfile_get_class_name(clazz, index) {
    if (index === 0 || index >= clazz.constant_pool_count) return null;
    const entry = clazz.constant_pool[index];
    if (!entry || entry.tag !== CONSTANT_Class) return null;
    return classfile_get_utf8(clazz, entry.info.class_info.name_index);
}

/**
 * Get name and type strings from a CONSTANT_NameAndType entry.
 * Returns { rc: 0|(-1), name: string|null, descriptor: string|null }.
 *
 * The C API writes through out-pointers; here we return a result object.
 *
 * @param {Object} clazz
 * @param {number} index
 * @returns {{ rc: number, name: string|null, descriptor: string|null }}
 */
export function classfile_get_name_and_type(clazz, index) {
    if (index === 0 || index >= clazz.constant_pool_count) {
        return { rc: -1, name: null, descriptor: null };
    }
    const entry = clazz.constant_pool[index];
    if (!entry || entry.tag !== CONSTANT_NameAndType) {
        return { rc: -1, name: null, descriptor: null };
    }
    const name       = classfile_get_utf8(clazz, entry.info.name_and_type.name_index);
    const descriptor = classfile_get_utf8(clazz, entry.info.name_and_type.descriptor_index);
    return { rc: 0, name, descriptor };
}

/**
 * Get a CONSTANT_Integer value.
 *
 * @param {Object} clazz
 * @param {number} index
 * @returns {{ rc: number, value: number }}
 */
export function classfile_get_integer(clazz, index) {
    if (index === 0 || index >= clazz.constant_pool_count) return { rc: -1, value: 0 };
    const entry = clazz.constant_pool[index];
    if (!entry || entry.tag !== CONSTANT_Integer) return { rc: -1, value: 0 };
    return { rc: 0, value: entry.info.integer.value };
}

/**
 * Get a CONSTANT_Float value.
 *
 * @param {Object} clazz
 * @param {number} index
 * @returns {{ rc: number, value: number }}
 */
export function classfile_get_float(clazz, index) {
    if (index === 0 || index >= clazz.constant_pool_count) return { rc: -1, value: 0 };
    const entry = clazz.constant_pool[index];
    if (!entry || entry.tag !== CONSTANT_Float) return { rc: -1, value: 0 };
    return { rc: 0, value: entry.info.float_val.value };
}

/**
 * Get a CONSTANT_Long value (as BigInt).
 *
 * @param {Object} clazz
 * @param {number} index
 * @returns {{ rc: number, value: bigint }}
 */
export function classfile_get_long(clazz, index) {
    if (index === 0 || index >= clazz.constant_pool_count) return { rc: -1, value: 0n };
    const entry = clazz.constant_pool[index];
    if (!entry || entry.tag !== CONSTANT_Long) return { rc: -1, value: 0n };
    return { rc: 0, value: entry.info.long_val.value };
}

/**
 * Get a CONSTANT_Double value.
 *
 * @param {Object} clazz
 * @param {number} index
 * @returns {{ rc: number, value: number }}
 */
export function classfile_get_double(clazz, index) {
    if (index === 0 || index >= clazz.constant_pool_count) return { rc: -1, value: 0 };
    const entry = clazz.constant_pool[index];
    if (!entry || entry.tag !== CONSTANT_Double) return { rc: -1, value: 0 };
    return { rc: 0, value: entry.info.double_val.value };
}

/**
 * Get a CONSTANT_String as a JS string (does not create a JavaString object).
 * Returns the UTF-8 string value the constant refers to, or null on error.
 *
 * The C version creates a Java String object; that requires a live JVM and
 * heap allocator.  Callers that need a JavaString should call jvm_new_string
 * on the returned value.
 *
 * @param {Object} _jvm   - unused; kept for API symmetry
 * @param {Object} clazz
 * @param {number} index
 * @returns {string|null}
 */
export function classfile_get_string(_jvm, clazz, index) {
    if (index === 0 || index >= clazz.constant_pool_count) return null;
    const entry = clazz.constant_pool[index];
    if (!entry || entry.tag !== CONSTANT_String) return null;
    return classfile_get_utf8(clazz, entry.info.string_info.string_index);
}

// ============================================================
// Field and method parsing
// ============================================================

/**
 * Parse the fields section.
 *
 * @param {Object} reader
 * @param {Object} clazz
 * @returns {number} 0 or -1
 */
export function classfile_parse_fields(reader, clazz) {
    const ref = { value: 0 };
    if (classfile_read_u2(reader, ref) !== 0) return -1;
    const count = ref.value;
    clazz.fields_count = count;

    if (count === 0) {
        clazz.fields = null;
        return 0;
    }

    const fields = new Array(count);
    clazz.fields = fields;

    for (let i = 0; i < count; i++) {
        const field = {
            access_flags:      0,
            name_index:        0,
            descriptor_index:  0,
            attributes_count:  0,
            attributes:        null,
            name:              null,
            descriptor:        null,
            offset:            0,
            clazz:             clazz,
        };

        const afRef  = { value: 0 };
        const niRef  = { value: 0 };
        const diRef  = { value: 0 };
        const acRef  = { value: 0 };

        if (classfile_read_u2(reader, afRef) !== 0) return -1;
        if (classfile_read_u2(reader, niRef) !== 0) return -1;
        if (classfile_read_u2(reader, diRef) !== 0) return -1;

        field.access_flags     = afRef.value;
        field.name_index       = niRef.value;
        field.descriptor_index = diRef.value;
        field.name             = classfile_get_utf8(clazz, field.name_index);
        field.descriptor       = classfile_get_utf8(clazz, field.descriptor_index);

        if (classfile_read_u2(reader, acRef) !== 0) return -1;
        field.attributes_count = acRef.value;

        const attrRef = { value: null };
        if (classfile_parse_attributes(reader, clazz, attrRef, field.attributes_count) !== 0) {
            return -1;
        }
        field.attributes = attrRef.value;

        fields[i] = field;
    }

    return 0;
}

/**
 * Parse the methods section.
 *
 * @param {Object} reader
 * @param {Object} clazz
 * @returns {number} 0 or -1
 */
export function classfile_parse_methods(reader, clazz) {
    const ref = { value: 0 };
    if (classfile_read_u2(reader, ref) !== 0) return -1;
    const count = ref.value;
    clazz.methods_count    = count;
    clazz.methods_capacity = count;

    if (count === 0) {
        clazz.methods = null;
        return 0;
    }

    const methods = new Array(count);
    clazz.methods = methods;

    for (let i = 0; i < count; i++) {
        const method = _make_empty_method(clazz);

        const afRef  = { value: 0 };
        const niRef  = { value: 0 };
        const diRef  = { value: 0 };
        const acRef  = { value: 0 };

        if (classfile_read_u2(reader, afRef) !== 0) return -1;
        if (classfile_read_u2(reader, niRef) !== 0) return -1;
        if (classfile_read_u2(reader, diRef) !== 0) return -1;

        method.access_flags     = afRef.value;
        method.name_index       = niRef.value;
        method.descriptor_index = diRef.value;
        method.name             = classfile_get_utf8(clazz, method.name_index);
        method.descriptor       = classfile_get_utf8(clazz, method.descriptor_index);
        method.is_native        = (method.access_flags & ACC_NATIVE) !== 0;

        if (classfile_read_u2(reader, acRef) !== 0) return -1;
        const attrCount = acRef.value;

        for (let j = 0; j < attrCount; j++) {
            const nameIdxRef = { value: 0 };
            const lenRef     = { value: 0 };

            if (classfile_read_u2(reader, nameIdxRef) !== 0) return -1;
            if (classfile_read_u4(reader, lenRef)     !== 0) return -1;

            const attrName = classfile_get_utf8(clazz, nameIdxRef.value);

            if (attrName === 'Code') {
                const msRef  = { value: 0 };
                const mlRef  = { value: 0 };
                const clRef  = { value: 0 };

                if (classfile_read_u2(reader, msRef) !== 0) return -1;
                if (classfile_read_u2(reader, mlRef) !== 0) return -1;
                if (classfile_read_u4(reader, clRef) !== 0) return -1;

                method.code.max_stack    = msRef.value;
                method.code.max_locals   = mlRef.value;
                method.code.code_length  = clRef.value;

                if (g_j2me_runtime_debug.value &&
                    method.code.max_locals === 0 && method.code.code_length > 0) {
                    process.stderr.write(
                        `[CLASSFILE-DEBUG] ${clazz.class_name || '?'}.${method.name || '?'}` +
                        `${method.descriptor || '?'} has max_locals=0, ` +
                        `code_len=${method.code.code_length}\n`
                    );
                }

                const { rc: codeRc, bytes: codeBytes } = _read_bytes(reader, method.code.code_length);
                if (codeRc !== 0) return -1;
                method.code.code = codeBytes;

                // Exception table
                const etLenRef = { value: 0 };
                if (classfile_read_u2(reader, etLenRef) !== 0) return -1;
                method.code.exception_table_length = etLenRef.value;

                if (method.code.exception_table_length > 0) {
                    const etLen = method.code.exception_table_length;
                    const table = new Array(etLen);
                    for (let k = 0; k < etLen; k++) {
                        const spRef  = { value: 0 };
                        const epRef  = { value: 0 };
                        const hpRef  = { value: 0 };
                        const ctRef  = { value: 0 };
                        classfile_read_u2(reader, spRef);
                        classfile_read_u2(reader, epRef);
                        classfile_read_u2(reader, hpRef);
                        classfile_read_u2(reader, ctRef);

                        table[k] = {
                            start_pc:   spRef.value,
                            end_pc:     epRef.value,
                            handler_pc: hpRef.value,
                            catch_type: ctRef.value,
                        };

                        if (g_j2me_runtime_debug.value) {
                            const catchCls = ctRef.value !== 0
                                ? classfile_get_class_name(clazz, ctRef.value)
                                : '<any/finally>';
                            process.stderr.write(
                                `[EX-TABLE] ${clazz.class_name || '?'}.${method.name || '?'}` +
                                `${method.descriptor || '?'} entry[${k}]: ` +
                                `start_pc=${spRef.value} end_pc=${epRef.value} ` +
                                `handler_pc=${hpRef.value} catch_type=${ctRef.value} ` +
                                `(${catchCls || '?'})\n`
                            );
                        }
                    }
                    method.code.exception_table = table;
                }

                // Skip code attributes (LineNumberTable, LocalVariableTable, etc.)
                const codeAttrRef = { value: 0 };
                if (classfile_read_u2(reader, codeAttrRef) !== 0) return -1;
                for (let k = 0; k < codeAttrRef.value; k++) {
                    const skipName = { value: 0 };
                    const skipLen  = { value: 0 };
                    classfile_read_u2(reader, skipName);
                    classfile_read_u4(reader, skipLen);
                    classfile_skip(reader, skipLen.value);
                }

                // Fix obfuscated bytecode that declares max_locals=0 but uses
                // local variables.  Same logic as the C source.
                method.code.max_locals = _fix_max_locals(method);

            } else {
                classfile_skip(reader, lenRef.value);
            }
        }

        methods[i] = method;
    }

    return 0;
}

/**
 * Parse `count` attributes from the stream into an array.
 * Writes the array into outRef.value.
 *
 * @param {Object}  reader
 * @param {Object}  clazz
 * @param {{ value: Array|null }} outRef
 * @param {number}  count
 * @returns {number} 0 or -1
 */
export function classfile_parse_attributes(reader, clazz, outRef, count) {
    if (count === 0) {
        outRef.value = null;
        return 0;
    }

    const attrs = new Array(count);

    for (let i = 0; i < count; i++) {
        const niRef  = { value: 0 };
        const lenRef = { value: 0 };

        if (classfile_read_u2(reader, niRef)  !== 0) return -1;
        if (classfile_read_u4(reader, lenRef) !== 0) return -1;

        const attrLen = lenRef.value;
        let info = null;

        if (attrLen > 0) {
            const { rc, bytes } = _read_bytes(reader, attrLen);
            if (rc !== 0) return -1;
            info = bytes;
        }

        attrs[i] = {
            name_index: niRef.value,
            length:     attrLen,
            info:       info,
        };
    }

    outRef.value = attrs;
    return 0;
}

/**
 * No-op stub — code attributes are inlined in classfile_parse_methods.
 * Exported for API symmetry with classfile.h.
 *
 * @param {Object} _reader
 * @param {Object} _method
 * @param {number} _length
 * @returns {number} 0
 */
export function classfile_parse_code_attribute(_reader, _method, _length) {
    return 0;
}

// ============================================================
// Top-level parse
// ============================================================

/**
 * Parse a Java class file from a Uint8Array.
 * Returns a plain JS object representing JavaClass, or null on error.
 *
 * @param {Object|null} jvm    - JVM instance (may be null for standalone parsing)
 * @param {Uint8Array}  data
 * @returns {Object|null}
 */
export function classfile_parse(jvm, data) {
    const length = data.length;
    const reader = classfile_reader_create(data);

    const clazz = _make_empty_class();

    const u4Ref = { value: 0 };
    const u2Ref = { value: 0 };

    // Magic
    if (classfile_read_u4(reader, u4Ref) !== 0) {
        process.stderr.write('Failed to read magic number\n');
        return null;
    }
    clazz.magic = u4Ref.value;

    if (clazz.magic !== CLASS_FILE_MAGIC) {
        process.stderr.write(`Invalid class file magic: 0x${clazz.magic.toString(16).toUpperCase().padStart(8, '0')}\n`);
        return null;
    }

    // Version
    if (classfile_read_u2(reader, u2Ref) !== 0) return null;
    clazz.minor_version = u2Ref.value;

    if (classfile_read_u2(reader, u2Ref) !== 0) return null;
    clazz.major_version = u2Ref.value;

    // Constant pool
    if (classfile_parse_constant_pool(reader, clazz) !== 0) {
        process.stderr.write('Failed to parse constant pool\n');
        return null;
    }

    // Access flags / this_class / super_class
    if (classfile_read_u2(reader, u2Ref) !== 0) return null;
    clazz.access_flags = u2Ref.value;

    if (classfile_read_u2(reader, u2Ref) !== 0) return null;
    clazz.this_class = u2Ref.value;

    if (classfile_read_u2(reader, u2Ref) !== 0) return null;
    clazz.super_class_index = u2Ref.value;

    // Interfaces
    if (classfile_read_u2(reader, u2Ref) !== 0) return null;
    clazz.interfaces_count = u2Ref.value;

    if (clazz.interfaces_count > 0) {
        const ifaces = new Array(clazz.interfaces_count);
        for (let i = 0; i < clazz.interfaces_count; i++) {
            if (classfile_read_u2(reader, u2Ref) !== 0) return null;
            ifaces[i] = u2Ref.value;
        }
        clazz.interfaces = ifaces;
    }

    // Fields
    if (classfile_parse_fields(reader, clazz) !== 0) {
        process.stderr.write('Failed to parse fields\n');
        return null;
    }

    // Methods
    if (classfile_parse_methods(reader, clazz) !== 0) {
        process.stderr.write('Failed to parse methods\n');
        return null;
    }

    // Class attributes
    if (classfile_read_u2(reader, u2Ref) !== 0) return null;
    clazz.attributes_count = u2Ref.value;

    if (clazz.attributes_count > 0) {
        const attrRef = { value: null };
        if (classfile_parse_attributes(reader, clazz, attrRef, clazz.attributes_count) !== 0) {
            return null;
        }
        clazz.attributes = attrRef.value;
    }

    // Resolve names
    clazz.class_name = classfile_get_class_name(clazz, clazz.this_class);

    if (clazz.super_class_index > 0) {
        clazz.super_class_name = classfile_get_class_name(clazz, clazz.super_class_index);
    }

    // Calculate instance_size, mirroring the C logic.
    // instance_size starts from ObjectHeader (16 bytes) and grows for each
    // non-static field (8 bytes per slot, 16 bytes for J/D).
    if (jvm && clazz.super_class_name) {
        if (jvm.class_loader && jvm.class_loader.classes) {
            const classes = jvm.class_loader.classes;
            const cnt     = jvm.class_loader.count;
            for (let i = 0; i < cnt; i++) {
                const sup = classes[i];
                if (sup && sup.class_name === clazz.super_class_name) {
                    clazz.super_class    = sup;
                    clazz.instance_size  = sup.instance_size;
                    break;
                }
            }
        }

        // If superclass was not found (not yet loaded), ask the JVM to create a stub.
        // This mirrors the C call to get_or_create_stub_class().
        if (!clazz.super_class && clazz.super_class_name !== 'java/lang/Object') {
            if (jvm.get_or_create_stub_class) {
                const stub = jvm.get_or_create_stub_class(jvm, clazz.super_class_name);
                if (stub) {
                    clazz.super_class   = stub;
                    clazz.instance_size = stub.instance_size;
                }
            }
        }
    }

    if (clazz.instance_size === 0) {
        clazz.instance_size = SIZEOF_OBJECT_HEADER;
    }

    // Add instance field slots
    if (clazz.fields) {
        for (let i = 0; i < clazz.fields_count; i++) {
            const field = clazz.fields[i];
            if (field.access_flags & ACC_STATIC) continue;

            clazz.instance_size += SIZEOF_JAVA_VALUE;
            if (field.descriptor &&
                (field.descriptor[0] === 'J' || field.descriptor[0] === 'D')) {
                clazz.instance_size += SIZEOF_JAVA_VALUE;
            }
        }
    }

    return clazz;
}

/**
 * Parse a class file from disk.
 *
 * @param {Object|null} jvm
 * @param {string}      filename
 * @returns {Object|null} - JavaClass or null
 */
export function classfile_parse_file(jvm, filename) {
    let buf;
    try {
        buf = readFileSync(filename);
    } catch (_e) {
        return null;
    }

    const size = buf.length;
    if (size <= 0 || size > 10 * 1024 * 1024) return null;

    return classfile_parse(jvm, new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength));
}

/**
 * Parse a class from a JAR/ZIP archive.
 * Returns null — ZIP extraction requires the miniz/utils layer which is not
 * yet migrated to JS.  Callers in the JS runtime should use the utils module
 * once available.
 *
 * @param {Object|null} _jvm
 * @param {string}      _jar_path
 * @param {string}      _class_name
 * @returns {null}
 */
export function classfile_parse_from_jar(_jvm, _jar_path, _class_name) {
    // ZIP extraction not yet implemented; see js/utils/utils.mjs when migrated.
    return null;
}

/**
 * Free a class object.  No-op in JS — GC handles memory.
 * Exported for API symmetry.
 *
 * @param {Object|null} _clazz
 */
export function classfile_free(_clazz) {
    // no-op
}

/**
 * Resolve symbolic references in a class (link phase).
 * Returns 0 unconditionally — resolution is performed lazily by the
 * interpreter (jvm_resolve_method / jvm_resolve_field).
 *
 * @param {Object} _jvm
 * @param {Object} _clazz
 * @returns {number} 0
 */
export function classfile_resolve(_jvm, _clazz) {
    return 0;
}

// ============================================================
// Utility functions
// ============================================================

/**
 * Return the number of operand stack slots used by a field/return descriptor.
 * 'J' (long) and 'D' (double) occupy 2 slots; everything else occupies 1.
 *
 * @param {string|null} descriptor
 * @returns {number}
 */
export function descriptor_to_size(descriptor) {
    if (!descriptor || descriptor.length === 0) return 1;
    const ch = descriptor[0];
    return (ch === 'J' || ch === 'D') ? 2 : 1;
}

/**
 * Parse a method descriptor string into parameter types and return type.
 * Returns { rc, param_types: string[], param_count, return_type: string }.
 *
 * Mirrors: int parse_method_descriptor(const char*, char**, int*, char*)
 *
 * @param {string} descriptor
 * @returns {{ rc: number, param_types: string[], param_count: number, return_type: string }}
 */
export function parse_method_descriptor(descriptor) {
    if (!descriptor || descriptor[0] !== '(') {
        return { rc: -1, param_types: [], param_count: 0, return_type: '' };
    }

    const param_types = [];
    let i = 1; // skip '('

    while (i < descriptor.length && descriptor[i] !== ')') {
        const start = i;
        const ch    = descriptor[i];

        if (ch === 'L') {
            // Object reference — advance to ';'
            while (i < descriptor.length && descriptor[i] !== ';') i++;
            param_types.push(descriptor.slice(start, i + 1));
            i++;
        } else if (ch === '[') {
            // Array — skip dimension markers then the element type
            while (i < descriptor.length && descriptor[i] === '[') i++;
            if (i < descriptor.length && descriptor[i] === 'L') {
                while (i < descriptor.length && descriptor[i] !== ';') i++;
                i++;
            } else {
                i++;
            }
            param_types.push(descriptor.slice(start, i));
        } else {
            param_types.push(ch);
            i++;
        }
    }

    i++; // skip ')'
    const return_type = i < descriptor.length ? descriptor[i] : 'V';

    return { rc: 0, param_types, param_count: param_types.length, return_type };
}

/**
 * Convert an internal class name (slashes) to a Java name (dots).
 *
 * @param {string|null} internal_name
 * @returns {string}
 */
export function class_name_to_java(internal_name) {
    if (!internal_name) return '';
    return internal_name.split('/').join('.');
}

/**
 * Convert a Java class name (dots) to an internal name (slashes).
 *
 * @param {string|null} java_name
 * @returns {string}
 */
export function class_name_to_internal(java_name) {
    if (!java_name) return '';
    return java_name.split('.').join('/');
}

// ============================================================
// Debug dump functions
// ============================================================

/**
 * Dump constant pool to stderr.
 *
 * @param {Object} clazz
 */
export function classfile_dump_constant_pool(clazz) {
    if (!clazz || !clazz.constant_pool) return;
    process.stderr.write(`Constant pool (${clazz.constant_pool_count} entries):\n`);
    for (let i = 1; i < clazz.constant_pool_count; i++) {
        const e = clazz.constant_pool[i];
        if (!e) { process.stderr.write(`  [${i}] (wide slot)\n`); continue; }
        switch (e.tag) {
            case CONSTANT_Utf8:
                process.stderr.write(`  [${i}] Utf8: "${e.info.utf8.bytes}"\n`); break;
            case CONSTANT_Integer:
                process.stderr.write(`  [${i}] Integer: ${e.info.integer.value}\n`); break;
            case CONSTANT_Float:
                process.stderr.write(`  [${i}] Float: ${e.info.float_val.value}\n`); break;
            case CONSTANT_Long:
                process.stderr.write(`  [${i}] Long: ${e.info.long_val.value}\n`); break;
            case CONSTANT_Double:
                process.stderr.write(`  [${i}] Double: ${e.info.double_val.value}\n`); break;
            case CONSTANT_Class:
                process.stderr.write(`  [${i}] Class: #${e.info.class_info.name_index}\n`); break;
            case CONSTANT_String:
                process.stderr.write(`  [${i}] String: #${e.info.string_info.string_index}\n`); break;
            case CONSTANT_Fieldref:
            case CONSTANT_Methodref:
            case CONSTANT_InterfaceMethodref:
                process.stderr.write(`  [${i}] Ref(${e.tag}): class=#${e.info.ref_info.class_index} nat=#${e.info.ref_info.name_and_type_index}\n`); break;
            case CONSTANT_NameAndType:
                process.stderr.write(`  [${i}] NameAndType: name=#${e.info.name_and_type.name_index} desc=#${e.info.name_and_type.descriptor_index}\n`); break;
            default:
                process.stderr.write(`  [${i}] tag=${e.tag}\n`); break;
        }
    }
}

/**
 * Dump fields to stderr.
 *
 * @param {Object} clazz
 */
export function classfile_dump_fields(clazz) {
    if (!clazz) return;
    process.stderr.write(`Fields (${clazz.fields_count}):\n`);
    if (!clazz.fields) return;
    for (let i = 0; i < clazz.fields_count; i++) {
        const f = clazz.fields[i];
        process.stderr.write(`  [${i}] ${f.name} : ${f.descriptor} (flags=0x${f.access_flags.toString(16)})\n`);
    }
}

/**
 * Dump methods to stderr.
 *
 * @param {Object} clazz
 */
export function classfile_dump_methods(clazz) {
    if (!clazz) return;
    process.stderr.write(`Methods (${clazz.methods_count}):\n`);
    if (!clazz.methods) return;
    for (let i = 0; i < clazz.methods_count; i++) {
        const m = clazz.methods[i];
        process.stderr.write(
            `  [${i}] ${m.name}${m.descriptor} ` +
            `(flags=0x${m.access_flags.toString(16)}, ` +
            `max_stack=${m.code.max_stack}, max_locals=${m.code.max_locals}, ` +
            `code_len=${m.code.code_length})\n`
        );
    }
}

// ============================================================
// Private helpers
// ============================================================

function _make_empty_class() {
    return {
        magic:                  0,
        minor_version:          0,
        major_version:          0,
        constant_pool_count:    0,
        constant_pool_capacity: 0,
        constant_pool:          null,
        access_flags:           0,
        this_class:             0,
        super_class_index:      0,
        interfaces_count:       0,
        interfaces:             null,
        fields_count:           0,
        fields:                 null,
        methods_count:          0,
        methods_capacity:       0,
        methods:                null,
        attributes_count:       0,
        attributes:             null,
        class_name:             null,
        super_class_name:       null,
        super_class:            null,
        interface_classes:      null,
        instance_size:          0,
        field_count:            0,
        initialized:            false,
        initializing_thread:    null,
        static_fields:          null,
        static_fields_count:    0,
        static_fields_capacity: 0,
        initializing:           false,
        is_stub:                false,
    };
}

function _make_empty_method(clazz) {
    return {
        access_flags:     0,
        name_index:       0,
        descriptor_index: 0,
        attributes_count: 0,
        attributes:       null,
        code: {
            max_stack:              0,
            max_locals:             0,
            code_length:            0,
            code:                   null,
            exception_table_length: 0,
            exception_table:        null,
            attributes_count:       0,
            attributes:             null,
        },
        name:             null,
        descriptor:       null,
        clazz:            clazz,
        is_native:        false,
        native_func:      null,
        cached_arg_count: -1,
    };
}

/**
 * Scan the method bytecode to determine the actual number of locals used,
 * and correct method.code.max_locals upward if needed.
 *
 * Some obfuscated J2ME bytecode declares max_locals=0 but uses local-variable
 * instructions.  KEmulator silently expands max_locals; we do the same here.
 *
 * @param {Object} method
 * @returns {number} corrected max_locals value
 */
function _fix_max_locals(method) {
    const code     = method.code.code;
    const codeLen  = method.code.code_length;
    const maxLocals= method.code.max_locals;

    // Count parameter slots (this + args)
    let paramSlots = 0;
    const desc = method.descriptor;
    if (desc && desc[0] === '(') {
        let d = 1; // skip '('
        while (d < desc.length && desc[d] !== ')') {
            paramSlots++;
            if (desc[d] === 'J' || desc[d] === 'D') paramSlots++; // 2-slot
            if (desc[d] === 'L') { while (d < desc.length && desc[d] !== ';') d++; }
            if (desc[d] === '[') {
                while (d < desc.length && desc[d] === '[') d++;
                if (d < desc.length && desc[d] === 'L') {
                    while (d < desc.length && desc[d] !== ';') d++;
                }
            }
            d++;
        }
    }
    if (!(method.access_flags & ACC_STATIC)) paramSlots++; // 'this'
    let neededLocals = paramSlots;

    if (!code || codeLen === 0) return maxLocals;

    let pc = 0;
    while (pc < codeLen) {
        const op = code[pc];
        let localIdx = -1;

        if (op === 0xc4 && pc + 1 < codeLen) {
            // wide prefix
            if (pc + 3 < codeLen) {
                localIdx = (code[pc + 2] << 8) | code[pc + 3];
            }
            pc += 4;
        } else {
            // Short-form xload_N / xstore_N
            if      (op >= 0x1a && op <= 0x1d) localIdx = op - 0x1a; // iload_0..3
            else if (op >= 0x1e && op <= 0x21) localIdx = op - 0x1e; // lload_0..3
            else if (op >= 0x22 && op <= 0x25) localIdx = op - 0x22; // fload_0..3
            else if (op >= 0x26 && op <= 0x29) localIdx = op - 0x26; // dload_0..3
            else if (op >= 0x2a && op <= 0x2d) localIdx = op - 0x2a; // aload_0..3
            else if (op >= 0x3b && op <= 0x3e) localIdx = op - 0x3b; // istore_0..3
            else if (op >= 0x3f && op <= 0x42) localIdx = op - 0x3f; // lstore_0..3
            else if (op >= 0x43 && op <= 0x46) localIdx = op - 0x43; // fstore_0..3
            else if (op >= 0x47 && op <= 0x4a) localIdx = op - 0x47; // dstore_0..3
            else if (op >= 0x4b && op <= 0x4e) localIdx = op - 0x4b; // astore_0..3

            if (localIdx < 0) {
                if      (op === 0x84) localIdx = code[pc + 1]; // iinc
                else if (op === 0x15) localIdx = code[pc + 1]; // iload
                else if (op === 0x16) localIdx = code[pc + 1]; // lload
                else if (op === 0x17) localIdx = code[pc + 1]; // fload
                else if (op === 0x18) localIdx = code[pc + 1]; // dload
                else if (op === 0x19) localIdx = code[pc + 1]; // aload
                else if (op === 0x36) localIdx = code[pc + 1]; // istore
                else if (op === 0x37) localIdx = code[pc + 1]; // lstore
                else if (op === 0x38) localIdx = code[pc + 1]; // fstore
                else if (op === 0x39) localIdx = code[pc + 1]; // dstore
                else if (op === 0x3a) localIdx = code[pc + 1]; // astore
                else if (op === 0xa9) localIdx = code[pc + 1]; // ret
            }

            // Advance PC
            switch (op) {
                case 0x10: pc += 2; break; // bipush
                case 0x11: pc += 3; break; // sipush
                case 0x12: pc += 2; break; // ldc
                case 0x13: pc += 3; break; // ldc_w
                case 0x14: pc += 3; break; // ldc2_w
                case 0x15: case 0x16: case 0x17: case 0x18:
                case 0x19:
                case 0x36: case 0x37: case 0x38: case 0x39:
                case 0x3a:
                case 0x84:
                case 0xa9:
                    pc += 2; break;
                case 0x99: case 0x9a: case 0x9b: case 0x9c:
                case 0x9d: case 0x9e: case 0x9f: case 0xa0:
                case 0xa1: case 0xa2: case 0xa3: case 0xa4:
                case 0xa5: case 0xa6:
                case 0xa7:
                case 0xa8:
                    pc += 3; break;
                case 0xaa: { // tableswitch
                    const pad  = (4 - ((pc + 1) % 4)) % 4;
                    const base = pc + 1 + pad;
                    if (base + 12 <= codeLen) {
                        const low  = (code[base+4]  << 24 | code[base+5]  << 16 | code[base+6]  << 8 | code[base+7])  | 0;
                        const high = (code[base+8]  << 24 | code[base+9]  << 16 | code[base+10] << 8 | code[base+11]) | 0;
                        pc = base + 12 + (high - low + 1) * 4;
                    } else {
                        pc = codeLen;
                    }
                    break;
                }
                case 0xab: { // lookupswitch
                    const pad   = (4 - ((pc + 1) % 4)) % 4;
                    const base  = pc + 1 + pad;
                    if (base + 8 <= codeLen) {
                        const npairs = (code[base+4] << 24 | code[base+5] << 16 | code[base+6] << 8 | code[base+7]) | 0;
                        pc = base + 8 + npairs * 8;
                    } else {
                        pc = codeLen;
                    }
                    break;
                }
                case 0xb2: case 0xb3: case 0xb4: case 0xb5:
                case 0xb6: case 0xb7: case 0xb8:
                case 0xbb:
                case 0xc0: case 0xc1:
                case 0xc6: case 0xc7:
                    pc += 3; break;
                case 0xb9: pc += 5; break; // invokeinterface
                case 0xba: pc += 5; break; // invokedynamic
                case 0xbc: pc += 2; break; // newarray
                case 0xbd: pc += 3; break; // anewarray
                case 0xc5: pc += 4; break; // multianewarray
                case 0xc8: pc += 5; break; // goto_w
                case 0xc9: pc += 5; break; // jsr_w
                default:   pc += 1; break;
            }
        }

        if (localIdx >= 0 && localIdx + 1 > neededLocals) {
            neededLocals = localIdx + 1;
        }
    }

    if (g_j2me_runtime_debug.value) {
        const clazz = method.clazz;
        process.stderr.write(
            `[CLASSFILE-SCAN] ${clazz ? clazz.class_name || '?' : '?'}.` +
            `${method.name || '?'}${method.descriptor || '?'} ` +
            `max_locals=${maxLocals} needed=${neededLocals}\n`
        );
    }

    if (neededLocals > maxLocals) {
        process.stderr.write(
            `[CLASSFILE] Fix: ${method.clazz ? method.clazz.class_name || '?' : '?'}.` +
            `${method.name || '?'}${method.descriptor || '?'} ` +
            `max_locals=${maxLocals} but bytecode needs ${neededLocals}, correcting\n`
        );
        return neededLocals;
    }

    return maxLocals;
}
