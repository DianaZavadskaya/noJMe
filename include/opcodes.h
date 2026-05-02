/*
 * J2ME Emulator - Java Bytecode Opcodes
 * All Java bytecode instructions for JVM execution
 */

#ifndef OPCODES_H
#define OPCODES_H

#include <stdint.h>

/*
 * Opcode Definitions (JVMS §6)
 * Each opcode is one byte (0-255)
 */

/* Constants: Push item onto stack */
#define OPC_NOP                 0x00    /* Do nothing */
#define OPC_ACONST_NULL         0x01    /* Push null */
#define OPC_ICONST_M1           0x02    /* Push int -1 */
#define OPC_ICONST_0            0x03    /* Push int 0 */
#define OPC_ICONST_1            0x04    /* Push int 1 */
#define OPC_ICONST_2            0x05    /* Push int 2 */
#define OPC_ICONST_3            0x06    /* Push int 3 */
#define OPC_ICONST_4            0x07    /* Push int 4 */
#define OPC_ICONST_5            0x08    /* Push int 5 */
#define OPC_LCONST_0            0x09    /* Push long 0 */
#define OPC_LCONST_1            0x0A    /* Push long 1 */
#define OPC_FCONST_0            0x0B    /* Push float 0.0 */
#define OPC_FCONST_1            0x0C    /* Push float 1.0 */
#define OPC_FCONST_2            0x0D    /* Push float 2.0 */
#define OPC_DCONST_0            0x0E    /* Push double 0.0 */
#define OPC_DCONST_1            0x0F    /* Push double 1.0 */

#define OPC_BIPUSH              0x10    /* Push byte as int */
#define OPC_SIPUSH              0x11    /* Push short as int */
#define OPC_LDC                 0x12    /* Push item from constant pool (index <= 255) */
#define OPC_LDC_W               0x13    /* Push item from constant pool (wide index) */
#define OPC_LDC2_W              0x14    /* Push long/double from constant pool */

/* Local Variable Load: Load from local variable */
#define OPC_ILOAD               0x15    /* Load int from local variable */
#define OPC_LLOAD               0x16    /* Load long from local variable */
#define OPC_FLOAD               0x17    /* Load float from local variable */
#define OPC_DLOAD               0x18    /* Load double from local variable */
#define OPC_ALOAD               0x19    /* Load reference from local variable */
#define OPC_ILOAD_0             0x1A    /* Load int from local variable 0 */
#define OPC_ILOAD_1             0x1B    /* Load int from local variable 1 */
#define OPC_ILOAD_2             0x1C    /* Load int from local variable 2 */
#define OPC_ILOAD_3             0x1D    /* Load int from local variable 3 */
#define OPC_LLOAD_0             0x1E    /* Load long from local variable 0 */
#define OPC_LLOAD_1             0x1F    /* Load long from local variable 1 */
#define OPC_LLOAD_2             0x20    /* Load long from local variable 2 */
#define OPC_LLOAD_3             0x21    /* Load long from local variable 3 */
#define OPC_FLOAD_0             0x22    /* Load float from local variable 0 */
#define OPC_FLOAD_1             0x23    /* Load float from local variable 1 */
#define OPC_FLOAD_2             0x24    /* Load float from local variable 2 */
#define OPC_FLOAD_3             0x25    /* Load float from local variable 3 */
#define OPC_DLOAD_0             0x26    /* Load double from local variable 0 */
#define OPC_DLOAD_1             0x27    /* Load double from local variable 1 */
#define OPC_DLOAD_2             0x28    /* Load double from local variable 2 */
#define OPC_DLOAD_3             0x29    /* Load double from local variable 3 */
#define OPC_ALOAD_0             0x2A    /* Load reference from local variable 0 */
#define OPC_ALOAD_1             0x2B    /* Load reference from local variable 1 */
#define OPC_ALOAD_2             0x2C    /* Load reference from local variable 2 */
#define OPC_ALOAD_3             0x2D    /* Load reference from local variable 3 */
#define OPC_IALOAD              0x2E    /* Load int from array */
#define OPC_LALOAD              0x2F    /* Load long from array */
#define OPC_FALOAD              0x30    /* Load float from array */
#define OPC_DALOAD              0x31    /* Load double from array */
#define OPC_AALOAD              0x32    /* Load reference from array */
#define OPC_BALOAD              0x33    /* Load byte/boolean from array */
#define OPC_CALOAD              0x34    /* Load char from array */
#define OPC_SALOAD              0x35    /* Load short from array */

/* Local Variable Store: Store into local variable */
#define OPC_ISTORE              0x36    /* Store int into local variable */
#define OPC_LSTORE              0x37    /* Store long into local variable */
#define OPC_FSTORE              0x38    /* Store float into local variable */
#define OPC_DSTORE              0x39    /* Store double into local variable */
#define OPC_ASTORE              0x3A    /* Store reference into local variable */
#define OPC_ISTORE_0            0x3B    /* Store int into local variable 0 */
#define OPC_ISTORE_1            0x3C    /* Store int into local variable 1 */
#define OPC_ISTORE_2            0x3D    /* Store int into local variable 2 */
#define OPC_ISTORE_3            0x3E    /* Store int into local variable 3 */
#define OPC_LSTORE_0            0x3F    /* Store long into local variable 0 */
#define OPC_LSTORE_1            0x40    /* Store long into local variable 1 */
#define OPC_LSTORE_2            0x41    /* Store long from local variable 2 */
#define OPC_LSTORE_3            0x42    /* Store long from local variable 3 */
#define OPC_FSTORE_0            0x43    /* Store float into local variable 0 */
#define OPC_FSTORE_1            0x44    /* Store float into local variable 1 */
#define OPC_FSTORE_2            0x45    /* Store float into local variable 2 */
#define OPC_FSTORE_3            0x46    /* Store float into local variable 3 */
#define OPC_DSTORE_0            0x47    /* Store double into local variable 0 */
#define OPC_DSTORE_1            0x48    /* Store double into local variable 1 */
#define OPC_DSTORE_2            0x49    /* Store double into local variable 2 */
#define OPC_DSTORE_3            0x4A    /* Store double into local variable 3 */
#define OPC_ASTORE_0            0x4B    /* Store reference into local variable 0 */
#define OPC_ASTORE_1            0x4C    /* Store reference into local variable 1 */
#define OPC_ASTORE_2            0x4D    /* Store reference into local variable 2 */
#define OPC_ASTORE_3            0x4E    /* Store reference into local variable 3 */
#define OPC_IASTORE             0x4F    /* Store into int array */
#define OPC_LASTORE             0x50    /* Store into long array */
#define OPC_FASTORE             0x51    /* Store into float array */
#define OPC_DASTORE             0x52    /* Store into double array */
#define OPC_AASTORE             0x53    /* Store into reference array */
#define OPC_BASTORE             0x54    /* Store into byte/boolean array */
#define OPC_CASTORE             0x55    /* Store into char array */
#define OPC_SASTORE             0x56    /* Store into short array */

/* Stack Operations */
#define OPC_POP                 0x57    /* Pop top stack value */
#define OPC_POP2                0x58    /* Pop top two stack values (or one if category 2) */
#define OPC_DUP                 0x59    /* Duplicate top stack value */
#define OPC_DUP_X1              0x5A    /* Duplicate top value and insert two down */
#define OPC_DUP_X2              0x5B    /* Duplicate top value and insert three down */
#define OPC_DUP2                0x5C    /* Duplicate top two values */
#define OPC_DUP2_X1             0x5D    /* Duplicate top two values and insert two down */
#define OPC_DUP2_X2             0x5E    /* Duplicate top two values and insert three down */
#define OPC_SWAP                0x5F    /* Swap top two values */

/* Arithmetic: Integer */
#define OPC_IADD                0x60    /* Add int */
#define OPC_LADD                0x61    /* Add long */
#define OPC_FADD                0x62    /* Add float */
#define OPC_DADD                0x63    /* Add double */
#define OPC_ISUB                0x64    /* Subtract int */
#define OPC_LSUB                0x65    /* Subtract long */
#define OPC_FSUB                0x66    /* Subtract float */
#define OPC_DSUB                0x67    /* Subtract double */
#define OPC_IMUL                0x68    /* Multiply int */
#define OPC_LMUL                0x69    /* Multiply long */
#define OPC_FMUL                0x6A    /* Multiply float */
#define OPC_DMUL                0x6B    /* Multiply double */
#define OPC_IDIV                0x6C    /* Divide int */
#define OPC_LDIV                0x6D    /* Divide long */
#define OPC_FDIV                0x6E    /* Divide float */
#define OPC_DDIV                0x6F    /* Divide double */
#define OPC_IREM                0x70    /* Remainder int */
#define OPC_LREM                0x71    /* Remainder long */
#define OPC_FREM                0x72    /* Remainder float */
#define OPC_DREM                0x73    /* Remainder double */
#define OPC_INEG                0x74    /* Negate int */
#define OPC_LNEG                0x75    /* Negate long */
#define OPC_FNEG                0x76    /* Negate float */
#define OPC_DNEG                0x77    /* Negate double */
#define OPC_ISHL                0x78    /* Shift left int */
#define OPC_LSHL                0x79    /* Shift left long */
#define OPC_ISHR                0x7A    /* Arithmetic shift right int */
#define OPC_LSHR                0x7B    /* Arithmetic shift right long */
#define OPC_IUSHR               0x7C    /* Logical shift right int */
#define OPC_LUSHR               0x7D    /* Logical shift right long */
#define OPC_IAND                0x7E    /* Boolean AND int */
#define OPC_LAND                0x7F    /* Boolean AND long */
#define OPC_IOR                 0x80    /* Boolean OR int */
#define OPC_LOR                 0x81    /* Boolean OR long */
#define OPC_IXOR                0x82    /* Boolean XOR int */
#define OPC_LXOR                0x83    /* Boolean XOR long */
#define OPC_IINC                0x84    /* Increment local variable by constant */

/* Type Conversion */
#define OPC_I2L                 0x85    /* Convert int to long */
#define OPC_I2F                 0x86    /* Convert int to float */
#define OPC_I2D                 0x87    /* Convert int to double */
#define OPC_L2I                 0x88    /* Convert long to int */
#define OPC_L2F                 0x89    /* Convert long to float */
#define OPC_L2D                 0x8A    /* Convert long to double */
#define OPC_F2I                 0x8B    /* Convert float to int */
#define OPC_F2L                 0x8C    /* Convert float to long */
#define OPC_F2D                 0x8D    /* Convert float to double */
#define OPC_D2I                 0x8E    /* Convert double to int */
#define OPC_D2L                 0x8F    /* Convert double to long */
#define OPC_D2F                 0x90    /* Convert double to float */
#define OPC_I2B                 0x91    /* Convert int to byte */
#define OPC_I2C                 0x92    /* Convert int to char */
#define OPC_I2S                 0x93    /* Convert int to short */

/* Comparisons */
#define OPC_LCMP                0x94    /* Compare long */
#define OPC_FCMPL               0x95    /* Compare float (negative on NaN) */
#define OPC_FCMPG               0x96    /* Compare float (positive on NaN) */
#define OPC_DCMPL               0x97    /* Compare double (negative on NaN) */
#define OPC_DCMPG               0x98    /* Compare double (positive on NaN) */

/* Control Flow: Branches */
#define OPC_IFEQ                0x99    /* Branch if int comparison with zero succeeds (==) */
#define OPC_IFNE                0x9A    /* Branch if int comparison with zero succeeds (!=) */
#define OPC_IFLT                0x9B    /* Branch if int comparison with zero succeeds (<) */
#define OPC_IFGE                0x9C    /* Branch if int comparison with zero succeeds (>=) */
#define OPC_IFGT                0x9D    /* Branch if int comparison with zero succeeds (>) */
#define OPC_IFLE                0x9E    /* Branch if int comparison with zero succeeds (<=) */
#define OPC_IF_ICMPEQ           0x9F    /* Branch if int comparison succeeds (==) */
#define OPC_IF_ICMPNE           0xA0    /* Branch if int comparison succeeds (!=) */
#define OPC_IF_ICMPLT           0xA1    /* Branch if int comparison succeeds (<) */
#define OPC_IF_ICMPGE           0xA2    /* Branch if int comparison succeeds (>=) */
#define OPC_IF_ICMPGT           0xA3    /* Branch if int comparison succeeds (>) */
#define OPC_IF_ICMPLE           0xA4    /* Branch if int comparison succeeds (<=) */
#define OPC_IF_ACMPEQ           0xA5    /* Branch if reference comparison succeeds (==) */
#define OPC_IF_ACMPNE           0xA6    /* Branch if reference comparison succeeds (!=) */
#define OPC_GOTO                0xA7    /* Always branch */
#define OPC_JSR                 0xA8    /* Jump subroutine */
#define OPC_RET                 0xA9    /* Return from subroutine */
#define OPC_TABLESWITCH         0xAA    /* Access jump table by index and jump */
#define OPC_LOOKUPSWITCH        0xAB    /* Access jump table by key match and jump */

/* Control Flow: Returns */
#define OPC_IRETURN             0xAC    /* Return int from method */
#define OPC_LRETURN             0xAD    /* Return long from method */
#define OPC_FRETURN             0xAE    /* Return float from method */
#define OPC_DRETURN             0xAF    /* Return double from method */
#define OPC_ARETURN             0xB0    /* Return reference from method */
#define OPC_RETURN              0xB1    /* Return void from method */

/* Field Access */
#define OPC_GETSTATIC           0xB2    /* Get static field */
#define OPC_PUTSTATIC           0xB3    /* Set static field */
#define OPC_GETFIELD            0xB4    /* Get instance field */
#define OPC_PUTFIELD            0xB5    /* Set instance field */

/* Method Invocation */
#define OPC_INVOKEVIRTUAL       0xB6    /* Invoke instance method (virtual dispatch) */
#define OPC_INVOKESPECIAL       0xB7    /* Invoke instance method (direct) */
#define OPC_INVOKESTATIC        0xB8    /* Invoke static method */
#define OPC_INVOKEINTERFACE     0xB9    /* Invoke interface method */
#define OPC_INVOKEDYNAMIC       0xBA    /* Invoke dynamic method (Java 7+, not in J2ME) */

/* Object Creation */
#define OPC_NEW                 0xBB    /* Create new object */
#define OPC_NEWARRAY            0xBC    /* Create new array of primitive type */
#define OPC_ANEWARRAY           0xBD    /* Create new array of reference type */
#define OPC_ARRAYLENGTH         0xBE    /* Get length of array */
#define OPC_ATHROW              0xBF    /* Throw exception or error */
#define OPC_CHECKCAST           0xC0    /* Check whether object is of given type */
#define OPC_INSTANCEOF          0xC1    /* Determine if object is of given type */
#define OPC_MONITORENTER        0xC2    /* Enter monitor for object */
#define OPC_MONITOREXIT         0xC3    /* Exit monitor for object */

/* Wide Instructions */
#define OPC_WIDE                0xC4    /* Extend local variable index */

/* Multi-dimensional Arrays */
#define OPC_MULTIANEWARRAY      0xC5    /* Create new multidimensional array */

/* Quick Instructions (Sun-specific, not standard) */
/* These are used by some JVM implementations for optimization */
#define OPC_IFNULL              0xC6    /* Branch if reference is null */
#define OPC_IFNONNULL           0xC7    /* Branch if reference is not null */
#define OPC_GOTO_W              0xC8    /* Always branch (wide index) */
#define OPC_JSR_W               0xC9    /* Jump subroutine (wide index) */

/* Reserved opcodes */
#define OPC_BREAKPOINT          0xCA    /* Reserved for breakpoint */
/* 0xCB-0xFD are reserved */
#define OPC_IMPDEP1             0xFE    /* Reserved for implementation */
#define OPC_IMPDEP2             0xFF    /* Reserved for implementation */

/*
 * NEWARRAY type codes (JVMS §newarray)
 */
#define T_BOOLEAN   4
#define T_CHAR      5
#define T_FLOAT     6
#define T_DOUBLE    7
#define T_BYTE      8
#define T_SHORT     9
#define T_INT       10
#define T_LONG      11

/*
 * Access flags (JVMS §4.1, §4.5, §4.6)
 */
#define ACC_PUBLIC      0x0001
#define ACC_PRIVATE     0x0002
#define ACC_PROTECTED   0x0004
#define ACC_STATIC      0x0008
#define ACC_FINAL       0x0010
#define ACC_SYNCHRONIZED 0x0020
#define ACC_SUPER       0x0020  /* For classes */
#define ACC_VOLATILE    0x0040
#define ACC_BRIDGE      0x0040  /* For methods */
#define ACC_TRANSIENT   0x0080
#define ACC_VARARGS     0x0080  /* For methods */
#define ACC_NATIVE      0x0100
#define ACC_INTERFACE   0x0200
#define ACC_ABSTRACT    0x0400
#define ACC_STRICT      0x0800
#define ACC_SYNTHETIC   0x1000
#define ACC_ANNOTATION  0x2000
#define ACC_ENUM        0x4000

/*
 * Method descriptor characters
 */
#define DESC_BYTE      'B'
#define DESC_CHAR      'C'
#define DESC_DOUBLE    'D'
#define DESC_FLOAT     'F'
#define DESC_INT       'I'
#define DESC_LONG      'J'
#define DESC_SHORT     'S'
#define DESC_BOOLEAN   'Z'
#define DESC_VOID      'V'
#define DESC_OBJECT    'L'
#define DESC_ARRAY     '['

/*
 * Opcode handler function type
 */
typedef struct JVM JVM;
typedef struct JavaFrame JavaFrame;
typedef struct JavaThread JavaThread;

typedef int (*OpcodeHandler)(JVM* jvm, JavaThread* thread, JavaFrame* frame);

/*
 * Opcode information structure
 */
typedef struct {
    const char* name;
    uint8_t operand_size;      /* Fixed operand size (0 for variable) */
    uint8_t stack_delta;       /* Change in stack size (approximate) */
    OpcodeHandler handler;     /* Handler function */
} OpcodeInfo;

/*
 * Global opcode table (defined in opcodes.c)
 */
extern const OpcodeInfo opcode_table[256];

/*
 * Opcode handler initialization
 */
void opcodes_init(void);

/*
 * Individual opcode handlers (generated)
 */
int op_nop(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_aconst_null(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_iconst(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_lconst(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_fconst(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_dconst(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_bipush(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_sipush(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_ldc(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_ldc_w(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_ldc2_w(JVM* jvm, JavaThread* thread, JavaFrame* frame);

int op_load(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_load_0(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_load_1(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_load_2(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_load_3(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_aload(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_iload(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_lload(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_fload(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_dload(JVM* jvm, JavaThread* thread, JavaFrame* frame);

int op_store(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_store_0(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_store_1(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_store_2(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_store_3(JVM* jvm, JavaThread* thread, JavaFrame* frame);

int op_array_load(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_array_store(JVM* jvm, JavaThread* thread, JavaFrame* frame);

int op_pop(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_pop2(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_dup(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_dup_x1(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_dup_x2(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_dup2(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_dup2_x1(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_dup2_x2(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_swap(JVM* jvm, JavaThread* thread, JavaFrame* frame);

int op_add(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_sub(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_mul(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_div(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_rem(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_neg(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_shl(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_shr(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_ushr(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_and(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_or(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_xor(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_iinc(JVM* jvm, JavaThread* thread, JavaFrame* frame);

int op_convert(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_lcmp(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_fcmpl(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_fcmpg(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_dcmpl(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_dcmpg(JVM* jvm, JavaThread* thread, JavaFrame* frame);

int op_if(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_if_icmp(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_if_acmp(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_goto(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_jsr(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_ret(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_tableswitch(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_lookupswitch(JVM* jvm, JavaThread* thread, JavaFrame* frame);

int op_return(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_ireturn(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_lreturn(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_freturn(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_dreturn(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_areturn(JVM* jvm, JavaThread* thread, JavaFrame* frame);

int op_getstatic(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_putstatic(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_getfield(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_putfield(JVM* jvm, JavaThread* thread, JavaFrame* frame);

int op_invokevirtual(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_invokespecial(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_invokestatic(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_invokeinterface(JVM* jvm, JavaThread* thread, JavaFrame* frame);

int op_new(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_newarray(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_anewarray(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_arraylength(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_athrow(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_checkcast(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_instanceof(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_monitorenter(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_monitorexit(JVM* jvm, JavaThread* thread, JavaFrame* frame);

int op_wide(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_multianewarray(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_ifnull(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_ifnonnull(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_goto_w(JVM* jvm, JavaThread* thread, JavaFrame* frame);
int op_jsr_w(JVM* jvm, JavaThread* thread, JavaFrame* frame);

#endif /* OPCODES_H */
