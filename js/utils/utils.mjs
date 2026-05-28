/**
 * J2ME Emulator - Utility Functions
 * Migrated from src/utils/utils.c (189 lines)
 *
 * Translation notes:
 *   - parse_method_descriptor: C used char** out-param + int* param_count +
 *     char* return_type. JS returns { paramTypes: string[], returnType: string }
 *     on success, or null on failure.  Callers that only need param_count and
 *     return_type can destructure the result.
 *   - descriptor_to_size: direct 1-to-1 port.
 *   - jvm_dump_*: printf -> process.stdout.write with template literals.
 *   - jvm_dump_heap: delegates to heap_dump import (lazy-loaded to avoid
 *     circular deps; a no-op stub is used when heap module is unavailable).
 */

// ---------------------------------------------------------------------------
// parse_method_descriptor
//
// Parses a JVM method descriptor such as "(ILjava/lang/String;)V".
//
// Returns:
//   { paramTypes: string[], returnType: string }  on success
//   null                                           on failure
//
// returnType is one of the JVM primitive chars (B C D F I J S Z V) or 'L'
// for any reference/array type (matching the C behaviour which collapses
// both 'L...' and '[...' to 'L').
// ---------------------------------------------------------------------------
export function parse_method_descriptor(descriptor) {
  if (!descriptor) return null;

  let p = 0;                  // cursor into descriptor string
  const paramTypes = [];

  if (descriptor[p] !== '(') return null;
  p++;

  // Parse parameters
  while (p < descriptor.length && descriptor[p] !== ')') {
    const ch = descriptor[p];

    switch (ch) {
      case 'B': case 'C': case 'D': case 'F':
      case 'I': case 'J': case 'S': case 'Z': {
        paramTypes.push(ch);
        p++;
        break;
      }

      case 'L': {
        // Object type: 'L' up to and including ';'
        const start = p;
        while (p < descriptor.length && descriptor[p] !== ';') p++;
        if (descriptor[p] !== ';') return null;   // malformed — no closing ';'
        const token = descriptor.slice(start, p + 1); // e.g. "Ljava/lang/String;"
        paramTypes.push(token);
        p++; // skip ';'
        break;
      }

      case '[': {
        // Array type — advance past the '[' and consume the element descriptor.
        // The C code records the array as part of the count but stores the raw
        // token only when param_types != NULL.  We follow the same logic: skip
        // the whole array-type token without storing it (matches C behaviour
        // where '[' falls through the skip branch even when count < *param_count
        // because the case sets count++ only for B/C/.../L, not '[').
        //
        // Actually re-reading the C source: the '[' branch increments p once,
        // then optionally scans to ';' for object arrays — but it does NOT
        // increment `count`.  So array parameters are silently skipped in the
        // output array.  We replicate that behaviour.
        p++; // skip '['
        if (p < descriptor.length && descriptor[p] === 'L') {
          while (p < descriptor.length && descriptor[p] !== ';') p++;
          if (p < descriptor.length && descriptor[p] === ';') p++;
        } else {
          p++; // single primitive char inside the array type
        }
        break;
      }

      default:
        return null;
    }
  }

  if (descriptor[p] !== ')') return null;
  p++;

  // Parse return type
  let returnType;
  const ret = descriptor[p];
  if (ret === 'V') {
    returnType = 'V';
  } else if ('BCDFIJSZ'.includes(ret)) {
    returnType = ret;
  } else if (ret === 'L' || ret === '[') {
    returnType = 'L'; // reference type (collapsed, matching C)
  } else {
    return null;
  }

  return { paramTypes, returnType };
}

// ---------------------------------------------------------------------------
// descriptor_to_size
//
// Returns the number of operand-stack slots consumed by the described type:
//   2 for long (J) and double (D)
//   1 for everything else
// ---------------------------------------------------------------------------
export function descriptor_to_size(descriptor) {
  if (!descriptor) return 0;
  const ch = typeof descriptor === 'string' ? descriptor[0] : descriptor;
  if (ch === 'J' || ch === 'D') return 2;
  return 1;
}

// ---------------------------------------------------------------------------
// jvm_dump_class
//
// Prints a human-readable summary of a JavaClass object to stdout.
// The JS equivalent uses plain objects; callers must pass an object with the
// same shape as the C JavaClass struct fields accessed here.
// ---------------------------------------------------------------------------
export function jvm_dump_class(clazz) {
  if (!clazz) return;

  const name = clazz.class_name ?? '(anonymous)';
  const superName = clazz.super_class_name ?? '(none)';

  process.stdout.write(`=== Class: ${name} ===\n`);
  process.stdout.write(`  Version: ${clazz.major_version}.${clazz.minor_version}\n`);
  process.stdout.write(`  Access flags: 0x${(clazz.access_flags >>> 0).toString(16).toUpperCase().padStart(4, '0')}\n`);
  process.stdout.write(`  Super class: ${superName}\n`);
  process.stdout.write(`  Interfaces: ${clazz.interfaces_count ?? 0}\n`);
  process.stdout.write(`  Fields: ${clazz.fields_count ?? 0}\n`);
  process.stdout.write(`  Methods: ${clazz.methods_count ?? 0}\n`);

  process.stdout.write('\n  Methods:\n');
  const methods = clazz.methods ?? [];
  for (let i = 0; i < clazz.methods_count; i++) {
    const m = methods[i];
    if (m) {
      process.stdout.write(`    ${m.name ?? ''}${m.descriptor ?? ''}\n`);
    }
  }
}

// ---------------------------------------------------------------------------
// jvm_dump_method
//
// Prints a human-readable summary of a JavaMethod object to stdout,
// including up to 50 bytes of bytecode as a hex dump.
// ---------------------------------------------------------------------------
export function jvm_dump_method(method) {
  if (!method) return;

  process.stdout.write(`=== Method: ${method.name ?? ''}${method.descriptor ?? ''} ===\n`);
  process.stdout.write(`  Access flags: 0x${(method.access_flags >>> 0).toString(16).toUpperCase().padStart(4, '0')}\n`);

  const code = method.code ?? {};
  process.stdout.write(`  Max stack: ${code.max_stack ?? 0}\n`);
  process.stdout.write(`  Max locals: ${code.max_locals ?? 0}\n`);
  process.stdout.write(`  Code length: ${code.code_length ?? 0}\n`);

  if (code.code) {
    // code.code is expected to be a Uint8Array (or array-like of byte values)
    process.stdout.write('  Code:\n    ');
    const limit = Math.min(code.code_length ?? code.code.length, 50);
    let line = '';
    for (let i = 0; i < limit; i++) {
      line += code.code[i].toString(16).toUpperCase().padStart(2, '0') + ' ';
      if ((i + 1) % 16 === 0) {
        process.stdout.write(line + '\n    ');
        line = '';
      }
    }
    if (line.length > 0) process.stdout.write(line);
    process.stdout.write('\n');
  }
}

// ---------------------------------------------------------------------------
// jvm_dump_stack
//
// Walks the JavaThread's frame chain (up to 10 frames) and prints each
// frame's location and stack state to stdout.
// ---------------------------------------------------------------------------
export function jvm_dump_stack(thread) {
  if (!thread) return;

  process.stdout.write('=== Thread Stack ===\n');

  let frame = thread.current_frame ?? null;
  let depth = 0;

  while (frame && depth < 10) {
    const className = frame.clazz?.class_name ?? '?';
    const methodName = frame.method?.name ?? '?';
    process.stdout.write(`  Frame ${depth}: ${className}.${methodName} @ PC=${frame.pc ?? 0}\n`);
    process.stdout.write(`    Stack top: ${frame.stack_top ?? 0}\n`);
    process.stdout.write(`    Locals: ${frame.max_locals ?? 0}\n`);

    frame = frame.prev ?? null;
    depth++;
  }
}

// ---------------------------------------------------------------------------
// jvm_dump_heap
//
// Delegates to heap_dump from the heap module.  The import is done lazily
// (dynamic import) so that this utility module remains usable even before
// js/jvm/heap.mjs exists.  If the heap module cannot be loaded, a warning is
// printed to stderr instead.
// ---------------------------------------------------------------------------
export async function jvm_dump_heap(jvm) {
  try {
    const { heap_dump } = await import('../jvm/heap.mjs');
    heap_dump(jvm);
  } catch {
    process.stderr.write('[WARN] jvm_dump_heap: heap module not available\n');
  }
}
