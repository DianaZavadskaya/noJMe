/*
 * J2ME Emulator - Utility Functions
 */

#include <stdio.h>
#include "debug.h"
#include "debug_macros.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "jvm.h"
#include "heap.h"

/* Parse method descriptor */
int parse_method_descriptor(const char* descriptor, char** param_types, int* param_count, char* return_type) {
    if (!descriptor || !return_type) return -1;
    
    const char* p = descriptor;
    int count = 0;
    
    if (*p != '(') return -1;
    p++;
    
    /* Parse parameters */
    while (*p && *p != ')') {
        if (param_types && count < *param_count) {
            switch (*p) {
                case 'B':
                case 'C':
                case 'D':
                case 'F':
                case 'I':
                case 'J':
                case 'S':
                case 'Z':
                    param_types[count] = (char*)malloc(2);
                    param_types[count][0] = *p;
                    param_types[count][1] = '\0';
                    count++;
                    p++;
                    break;
                    
                case 'L': {
                    const char* start = p;
                    while (*p && *p != ';') p++;
                    if (*p == ';') {
                        size_t len = p - start + 1;
                        param_types[count] = (char*)malloc(len + 1);
                        strncpy(param_types[count], start, len);
                        param_types[count][len] = '\0';
                        count++;
                        p++;
                    }
                    break;
                }
                    
                case '[':
                    /* Array type - keep scanning */
                    p++;
                    if (*p == 'L') {
                        while (*p && *p != ';') p++;
                        if (*p == ';') p++;
                    } else {
                        p++;
                    }
                    break;
                    
                default:
                    return -1;
            }
        } else {
            /* Skip parameter */
            switch (*p) {
                case 'B': case 'C': case 'D': case 'F':
                case 'I': case 'J': case 'S': case 'Z':
                    p++;
                    break;
                case 'L':
                    while (*p && *p != ';') p++;
                    if (*p == ';') p++;
                    break;
                case '[':
                    p++;
                    break;
                default:
                    return -1;
            }
        }
    }
    
    if (*p != ')') return -1;
    p++;
    
    /* Parse return type */
    if (*p == 'V') {
        *return_type = 'V';
    } else if (*p == 'B' || *p == 'C' || *p == 'D' || *p == 'F' ||
               *p == 'I' || *p == 'J' || *p == 'S' || *p == 'Z') {
        *return_type = *p;
    } else if (*p == 'L' || *p == '[') {
        *return_type = 'L';  /* Reference type */
    } else {
        return -1;
    }
    
    if (param_count) *param_count = count;
    
    return 0;
}

/* Descriptor to size */
int descriptor_to_size(const char* descriptor) {
    if (!descriptor) return 0;
    
    switch (descriptor[0]) {
        case 'J':
        case 'D':
            return 2;  /* Long and double take 2 slots */
        default:
            return 1;
    }
}

/* Debug: dump class */
void jvm_dump_class(JavaClass* clazz) {
    if (!clazz) return;
    
    printf("=== Class: %s ===\n", clazz->class_name ? clazz->class_name : "(anonymous)");
    printf("  Version: %d.%d\n", clazz->major_version, clazz->minor_version);
    printf("  Access flags: 0x%04X\n", clazz->access_flags);
    printf("  Super class: %s\n", clazz->super_class_name ? clazz->super_class_name : "(none)");
    printf("  Interfaces: %d\n", clazz->interfaces_count);
    printf("  Fields: %d\n", clazz->fields_count);
    printf("  Methods: %d\n", clazz->methods_count);
    
    printf("\n  Methods:\n");
    for (int i = 0; i < clazz->methods_count; i++) {
        printf("    %s%s\n", clazz->methods[i].name, clazz->methods[i].descriptor);
    }
}

/* Debug: dump method */
void jvm_dump_method(JavaMethod* method) {
    if (!method) return;
    
    printf("=== Method: %s%s ===\n", method->name, method->descriptor);
    printf("  Access flags: 0x%04X\n", method->access_flags);
    printf("  Max stack: %d\n", method->code.max_stack);
    printf("  Max locals: %d\n", method->code.max_locals);
    printf("  Code length: %d\n", method->code.code_length);
    
    if (method->code.code) {
        printf("  Code:\n    ");
        for (uint32_t i = 0; i < method->code.code_length && i < 50; i++) {
            printf("%02X ", method->code.code[i]);
            if ((i + 1) % 16 == 0) printf("\n    ");
        }
        printf("\n");
    }
}

/* Debug: dump stack */
void jvm_dump_stack(JavaThread* thread) {
    if (!thread) return;
    
    printf("=== Thread Stack ===\n");
    
    JavaFrame* frame = thread->current_frame;
    int depth = 0;
    
    while (frame && depth < 10) {
        printf("  Frame %d: %s.%s @ PC=%d\n", depth,
               frame->clazz ? frame->clazz->class_name : "?",
               frame->method ? frame->method->name : "?",
               frame->pc);
        
        printf("    Stack top: %d\n", frame->stack_top);
        printf("    Locals: %d\n", frame->max_locals);
        
        frame = frame->prev;
        depth++;
    }
}

/* Debug: dump heap */
void jvm_dump_heap(JVM* jvm) {
    heap_dump(jvm);
}
