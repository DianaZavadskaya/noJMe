/*
 * J2ME Emulator - Class File Loader
 * Parses Java .class files according to JVM Specification
 */

#ifndef CLASSFILE_H
#define CLASSFILE_H

#include "jvm.h"

/*
 * Class file magic number
 */
#define CLASS_FILE_MAGIC 0xCAFEBABE

/*
 * Class file versions
 */
#define CLASS_VERSION_MIN        45    /* JDK 1.0.2 */
#define CLASS_VERSION_1_1        45
#define CLASS_VERSION_1_2        46
#define CLASS_VERSION_1_3        47    /* J2ME target */
#define CLASS_VERSION_1_4        48
#define CLASS_VERSION_5_0        49
#define CLASS_VERSION_6_0        50
#define CLASS_VERSION_7_0        51
#define CLASS_VERSION_8_0        52

/*
 * Class file parsing context
 */
typedef struct {
    const uint8_t* data;
    size_t length;
    size_t position;
    
    /* Error information */
    const char* error;
    size_t error_position;
} ClassFileReader;

/*
 * Attribute types we recognize
 */
typedef enum {
    ATTR_SourceFile,
    ATTR_InnerClasses,
    ATTR_Synthetic,
    ATTR_Deprecated,
    ATTR_Code,
    ATTR_Exceptions,
    ATTR_LineNumberTable,
    ATTR_LocalVariableTable,
    ATTR_LocalVariableTypeTable,
    ATTR_ConstantValue,
    ATTR_Signature,
    ATTR_RuntimeVisibleAnnotations,
    ATTR_RuntimeInvisibleAnnotations,
    ATTR_Unknown
} AttributeType;

/*
 * Class file parsing functions
 */

/* Create a class file reader */
ClassFileReader* classfile_reader_create(const uint8_t* data, size_t length);

/* Destroy a class file reader */
void classfile_reader_destroy(ClassFileReader* reader);

/* Read a Java class from raw bytes */
JavaClass* classfile_parse(JVM* jvm, const uint8_t* data, size_t length);

/* Parse class from file */
JavaClass* classfile_parse_file(JVM* jvm, const char* filename);

/* Parse class from JAR/ZIP */
JavaClass* classfile_parse_from_jar(JVM* jvm, const char* jar_path, const char* class_name);

/* Free a parsed class */
void classfile_free(JavaClass* clazz);

/* Resolve all symbolic references in a class */
int classfile_resolve(JVM* jvm, JavaClass* clazz);

/*
 * Reading functions
 */

/* Read a single byte */
int classfile_read_u1(ClassFileReader* reader, uint8_t* value);

/* Read a 16-bit value (big-endian) */
int classfile_read_u2(ClassFileReader* reader, uint16_t* value);

/* Read a 32-bit value (big-endian) */
int classfile_read_u4(ClassFileReader* reader, uint32_t* value);

/* Read bytes into buffer */
int classfile_read_bytes(ClassFileReader* reader, void* buffer, size_t count);

/* Skip bytes */
int classfile_skip(ClassFileReader* reader, size_t count);

/* Get current position */
size_t classfile_position(ClassFileReader* reader);

/* Check remaining bytes */
size_t classfile_remaining(ClassFileReader* reader);

/*
 * Constant pool parsing
 */
int classfile_parse_constant_pool(ClassFileReader* reader, JavaClass* clazz);

/* Get UTF8 string from constant pool */
const char* classfile_get_utf8(JavaClass* clazz, uint16_t index);

/* Get class name from constant pool */
const char* classfile_get_class_name(JavaClass* clazz, uint16_t index);

/* Get name and type from constant pool */
int classfile_get_name_and_type(JavaClass* clazz, uint16_t index, 
                                const char** name, const char** descriptor);

/* Get integer constant */
int classfile_get_integer(JavaClass* clazz, uint16_t index, jint* value);

/* Get float constant */
int classfile_get_float(JavaClass* clazz, uint16_t index, jfloat* value);

/* Get long constant */
int classfile_get_long(JavaClass* clazz, uint16_t index, jlong* value);

/* Get double constant */
int classfile_get_double(JavaClass* clazz, uint16_t index, jdouble* value);

/* Get string constant (creates Java String object) */
JavaString* classfile_get_string(JVM* jvm, JavaClass* clazz, uint16_t index);

/*
 * Field and method parsing
 */
int classfile_parse_fields(ClassFileReader* reader, JavaClass* clazz);
int classfile_parse_methods(ClassFileReader* reader, JavaClass* clazz);
int classfile_parse_attributes(ClassFileReader* reader, JavaClass* clazz, 
                               AttributeInfo** attributes, uint16_t count);

/*
 * Code attribute parsing
 */
int classfile_parse_code_attribute(ClassFileReader* reader, JavaMethod* method,
                                   uint32_t length);

/*
 * Utility functions
 */

/* Convert descriptor to size in words (1 for 32-bit, 2 for 64-bit) */
int descriptor_to_size(const char* descriptor);

/* Parse method descriptor */
int parse_method_descriptor(const char* descriptor, 
                           char** param_types, int* param_count,
                           char* return_type);

/* Get human-readable class name (convert slashes to dots) */
char* class_name_to_java(const char* internal_name);

/* Get internal class name (convert dots to slashes) */
char* class_name_to_internal(const char* java_name);

/*
 * Debug functions
 */
void classfile_dump_constant_pool(JavaClass* clazz);
void classfile_dump_fields(JavaClass* clazz);
void classfile_dump_methods(JavaClass* clazz);

#endif /* CLASSFILE_H */
