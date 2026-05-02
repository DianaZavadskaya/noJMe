/*
 * J2ME Emulator - MIDP2 Display and Game Canvas
 */

#define _GNU_SOURCE  /* For strdup */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>  /* For ETIMEDOUT */

#include "midp.h"
#include "jvm.h"
#include "native.h"
#include "heap.h"
#include "sdl_backend.h"
#include "threads.h"
#include "opcodes.h"  /* For ACC_PRIVATE */
#include "debug.h"
#include "bitmap_font.h"  /* For FONT_WIDTH */

/* ИСПРАВЛЕНИЕ: Мьютекс для синхронизации paint() между потоками
 * SDL2 требует чтобы вся работа с графикой происходила из главного потока.
 * Java-поток может вызывать serviceRepaints(), но paint() должен выполняться
 * только в главном потоке. */
#ifndef _WIN32
#include <pthread.h>
static pthread_mutex_t g_paint_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_paint_cond = PTHREAD_COND_INITIALIZER;
static volatile bool g_paint_pending = false;
static volatile bool g_paint_complete = false;
#else
#include <windows.h>
static CRITICAL_SECTION g_paint_cs;
static CONDITION_VARIABLE g_paint_cv;
static volatile bool g_paint_pending = false;
static volatile bool g_paint_complete = false;
static bool g_paint_cs_initialized = false;
#endif

/* Forward declarations for helper functions */
static jint get_object_field_int(JavaObject* obj, const char* field_name);
static void set_object_field_int(JavaObject* obj, const char* field_name, jint value);
/* get_object_field_ref is now public - declared in midp.h */
void set_object_field_ref(JavaObject* obj, const char* field_name, JavaObject* value);

/* Forward declaration for public helper (defined later) */
MidpImage* get_image_from_object(JavaObject* obj);

/* Sprite transform constants - used by Graphics.drawRegion and Image.createImage */
#define SPRITE_TRANS_NONE           0
#define SPRITE_TRANS_MIRROR_ROT180  1
#define SPRITE_TRANS_MIRROR         2
#define SPRITE_TRANS_ROT180         3
#define SPRITE_TRANS_MIRROR_ROT270  4
#define SPRITE_TRANS_ROT90          5
#define SPRITE_TRANS_ROT270         6
#define SPRITE_TRANS_MIRROR_ROT90   7

/* Forward declaration from execute.c */
extern int execute_method(JVM* jvm, JavaThread* thread, JavaMethod* method, JavaValue* args, JavaValue* result);

/* Forward declarations from mobile3d.c (force-render tracking) */
extern void m3g_reset_paint_tracking(void);
extern bool m3g_needs_force_render(void);
extern void m3g_force_render(JVM* jvm, MidpGraphics* screen_gfx);

/* Global Graphics object - reused for all paint() calls */
static JavaObject* g_graphics_object = NULL;
static MidpGraphics g_screen_graphics;  /* Static Graphics for screen rendering */

/* Current displayable - stored as GC root */
static JavaObject* current_displayable_obj = NULL;

/* Soft button area height */
#define SOFT_BUTTON_HEIGHT 25

/* Full screen mode tracking per displayable */
static bool g_full_screen_mode = false;

/* Command menu state */
static bool g_command_menu_open = false;
static int g_command_menu_selected = 0;

/* Forward declaration */
void ensure_native_peer_field(JavaClass* clazz);

/* Forward declaration for find_field_index */
static int find_field_index(JavaObject* obj, const char* field_name);

/* Get string from Java String object */
static const char* get_string_from_java(JVM* jvm, JavaObject* str_obj) {
    if (!str_obj) return "";
    JavaString* str = (JavaString*)str_obj;
    const char* result = string_utf8(jvm, str);
    return result ? result : "";
}

/* Check if a pointer looks like a valid class pointer */
static bool is_valid_class_ptr(JavaClass* clazz) {
    if (!clazz) return false;
    
    /* Check if the pointer is in a reasonable range (not too small, not kernel space) */
    /* On 32-bit: typical user space is 0x00400000 to 0x7FFFFFFF */
    /* On 64-bit: typical user space is much higher */
    #if defined(_WIN64) || defined(__x86_64__)
    if ((uintptr_t)clazz < 0x10000) return false;  /* Too small */
    #else
    if ((uintptr_t)clazz < 0x10000) return false;  /* Too small for 32-bit */
    #endif
    
    /* Check if class_name is a reasonable pointer */
    if (clazz->class_name) {
        /* class_name should point to static data or heap, not be garbage */
        /* A simple check: if it's too small, it's likely garbage */
        if ((uintptr_t)clazz->class_name < 0x10000) return false;
    }
    
    /* Check fields_count is reasonable */
    if (clazz->fields_count < 0 || clazz->fields_count > 1000) return false;
    
    return true;
}

/* Validate a heap object before accessing it */
static bool validate_heap_object(JavaObject* obj) {
    if (!obj) return false;
    
    /* Check if object is in heap bounds */
    if (!is_heap_ptr_check(obj)) {
        DEBUG_LOG("[VALIDATE] Object %p is NOT in heap bounds [%p, %p)",
                (void*)obj, g_heap_start, g_heap_end);
        return false;
    }
    
    /* Get GC header */
    GCObjectHeader* header = (GCObjectHeader*)obj - 1;
    
    /* Check if GC header is in heap bounds */
    if (!is_heap_ptr_check(header)) {
        DEBUG_LOG("[VALIDATE] GC header %p for object %p is NOT in heap bounds",
                (void*)header, (void*)obj);
        return false;
    }
    
    /* Check header fields for sanity */
    if (header->size == 0 || header->size > 16*1024*1024) {
        DEBUG_LOG("[VALIDATE] Object %p has invalid size: %u",
                (void*)obj, header->size);
        return false;
    }
    
    /* Check if object type is valid */
    if (header->type > OBJ_TYPE_CLASS) {
        DEBUG_LOG("[VALIDATE] Object %p has invalid type: %d",
                (void*)obj, header->type);
        return false;
    }
    
    /* Validate clazz pointer */
    if (!is_valid_class_ptr(header->clazz)) {
        DEBUG_LOG("[VALIDATE] Object %p has invalid clazz pointer: %p",
                (void*)obj, (void*)header->clazz);
        return false;
    }
    
    return true;
}

/* Safely get the label string from a Command object.
 * Returns a default string if the object is invalid or corrupted.
 * Uses soft validation - only checks heap bounds and class pointer. */
static const char* get_command_label_safe(JVM* jvm, JavaObject* cmd) {
    static const char* default_label = "Command";
    
    if (!cmd) return default_label;
    
    /* Soft validation - only check if pointer is in heap */
    if (!is_heap_ptr_check(cmd)) {
        DEBUG_LOG("[CMD_LABEL] Command %p is NOT in heap bounds", (void*)cmd);
        return default_label;
    }
    
    /* Try to access the object's class pointer */
    JavaClass* clazz = cmd->header.clazz;
    if (!clazz) {
        DEBUG_LOG("[CMD_LABEL] Command %p has NULL class", (void*)cmd);
        return default_label;
    }
    
    /* Basic sanity check for class pointer */
    if ((uintptr_t)clazz < 0x10000) {
        DEBUG_LOG("[CMD_LABEL] Command %p has invalid class: %p", (void*)cmd, (void*)clazz);
        return default_label;
    }
    
    /* Try to get label - Command has label at field 0 */
    JavaString* label = NULL;
    
    /* Try to find label field by name first */
    int label_idx = find_field_index(cmd, "label");
    if (label_idx >= 0) {
        label = (JavaString*)cmd->fields[label_idx].ref;
    } else {
        /* Fallback: try field 0 (common case for Command) */
        label = (JavaString*)cmd->fields[0].ref;
    }
    
    if (!label) return default_label;
    
    const char* text = get_string_from_java(jvm, (JavaObject*)label);
    return text ? text : default_label;
}

/* Find field slot by name in class hierarchy.
 * Returns the correct slot index in the object's fields array,
 * or -1 if not found. */
static int find_field_index(JavaObject* obj, const char* field_name) {
    if (!obj) return -1;
    
    /* Soft validation - only check heap bounds */
    if (!is_heap_ptr_check(obj)) {
        DEBUG_LOG("[find_field_index] Object %p is NOT in heap bounds", (void*)obj);
        return -1;
    }
    
    if (!obj->header.clazz) return -1;
    
    JavaClass* clazz = obj->header.clazz;
    
    /* Build class hierarchy from Object to actual class */
    JavaClass* hierarchy[64];
    int depth = 0;
    JavaClass* c = clazz;
    while (c && depth < 64) {
        hierarchy[depth++] = c;
        c = c->super_class;
    }
    
    /* Search for field and calculate slot from Object down to actual class */
    int slot = 0;
    for (int h = depth - 1; h >= 0; h--) {
        JavaClass* current = hierarchy[h];
        
        /* Validate class before accessing */
        if (!is_valid_class_ptr(current)) {
            DEBUG_LOG("[find_field_index] Invalid class pointer in hierarchy for field '%s'",
                    field_name ? field_name : "(null)");
            break;
        }
        
        if (!current->fields) continue;
        
        for (int i = 0; i < current->fields_count; i++) {
            JavaField* field = &current->fields[i];
            
            /* Skip static fields */
            if (field->access_flags & ACC_STATIC) continue;
            
            if (field->name && strcmp(field->name, field_name) == 0) {
                return slot;
            }
            
            slot++;
            /* Long and double take 2 slots */
            if (field->descriptor && 
                (field->descriptor[0] == 'J' || field->descriptor[0] == 'D')) {
                slot++;
            }
        }
    }
    
    return -1;
}

/* Forward declaration for type-based command mapping */
static void get_soft_button_commands(JVM* jvm, JavaObject* displayable,
                                     int* left_idx, int* right_idx);

/* Render soft buttons (commands) at bottom of screen */
static void render_soft_buttons(JVM* jvm, JavaObject* displayable) {
    DISP_DEBUG("[SoftBtn] render_soft_buttons: displayable=%p", (void*)displayable);
    
    if (!displayable) {
        return;
    }
    
    /* Get the screen graphics - ИСПРАВЛЕНО: инициализируем при необходимости */
    MidpGraphics* gfx = &g_screen_graphics;
    
    /* Если pixels не инициализирован, получаем из SDL context */
    if (!gfx->pixels) {
        SdlContext* sdl_ctx = sdl_get_global_context();
        if (sdl_ctx && sdl_ctx->framebuffer) {
            midp_graphics_init(gfx, sdl_ctx->framebuffer, sdl_ctx->width, sdl_ctx->height);
        }
    }
    
    if (!gfx || !gfx->pixels) {
        return;
    }
    
    /* Check if full screen mode is enabled */
    if (g_full_screen_mode) {
        return;  /* Don't draw soft buttons in full screen mode */
    }
    
    /* Find commands field in displayable */
    int commands_idx = find_field_index(displayable, "commands");
    
    if (commands_idx < 0) {
        return;
    }
    
    JavaArray* commands = (JavaArray*)displayable->fields[commands_idx].ref;
    
    if (!commands || commands->length == 0) {
        return;
    }
    
    void** cmd_data = (void**)array_data(commands);
    int cmd_count = commands->length;
    
    //DISP_DEBUG("[SoftBtn] render_soft_buttons: drawing %d commands", cmd_count);
    
    /* Draw soft button bar background */
    int screen_height = gfx->height;
    int screen_width = gfx->width;
    
    /* IMPORTANT: Save current clip state and reset to full screen for soft buttons */
    int saved_clip_x = gfx->clip_x;
    int saved_clip_y = gfx->clip_y;
    int saved_clip_width = gfx->clip_width;
    int saved_clip_height = gfx->clip_height;
    int saved_translate_x = gfx->translate_x;
    int saved_translate_y = gfx->translate_y;
    
    /* Reset clip to full screen area (soft buttons should not be clipped by game) */
    gfx->clip_x = 0;
    gfx->clip_y = 0;
    gfx->clip_width = screen_width;
    gfx->clip_height = screen_height;
    gfx->translate_x = 0;
    gfx->translate_y = 0;
    
    /* If command menu is open, draw the menu overlay */
    if (g_command_menu_open && cmd_count > 2) {
        /* Semi-transparent background overlay */
        midp_graphics_set_color(gfx, 0x000000, 128);
        midp_graphics_fill_rect(gfx, 0, 0, screen_width, screen_height);
        
        /* Menu box */
        int menu_width = screen_width - 20;
        int menu_item_height = 20;
        int menu_height = cmd_count * menu_item_height + 10;
        int menu_x = 10;
        int menu_y = (screen_height - menu_height) / 2;
        
        /* Menu background */
        midp_graphics_set_color(gfx, 0xFFFFFF, 255);
        midp_graphics_fill_rect(gfx, menu_x, menu_y, menu_width, menu_height);
        
        /* Menu border */
        midp_graphics_set_color(gfx, 0x000000, 255);
        midp_graphics_draw_rect(gfx, menu_x, menu_y, menu_width, menu_height);
        
        /* Draw menu items */
        for (int i = 0; i < cmd_count; i++) {
            if (!cmd_data[i]) continue;
            
            JavaObject* cmd = (JavaObject*)cmd_data[i];
            const char* text = get_command_label_safe(jvm, cmd);
            
            int item_y = menu_y + 5 + i * menu_item_height;
            
            /* Highlight selected item */
            if (i == g_command_menu_selected) {
                midp_graphics_set_color(gfx, 0x000080, 255);
                midp_graphics_fill_rect(gfx, menu_x + 2, item_y, menu_width - 4, menu_item_height - 2);
                midp_graphics_set_color(gfx, 0xFFFFFF, 255);
            } else {
                midp_graphics_set_color(gfx, 0x000000, 255);
            }
            
            midp_graphics_draw_string(gfx, text, menu_x + 5, item_y + 2, 0);
        }
        
        /* Draw soft buttons for menu: "Select" and "Cancel" */
        midp_graphics_set_color(gfx, 0xC0C0C0, 255);
        midp_graphics_fill_rect(gfx, 0, screen_height - SOFT_BUTTON_HEIGHT, 
                                screen_width, SOFT_BUTTON_HEIGHT);
        midp_graphics_set_color(gfx, 0x808080, 255);
        midp_graphics_draw_line(gfx, 0, screen_height - SOFT_BUTTON_HEIGHT,
                                screen_width, screen_height - SOFT_BUTTON_HEIGHT);
        
        /* Left button: Cancel */
        midp_graphics_set_color(gfx, 0x000000, 255);
        midp_graphics_draw_string(gfx, "Cancel", 5, screen_height - SOFT_BUTTON_HEIGHT + 5, 0);
        
        /* Right button: Select */
        midp_graphics_set_color(gfx, 0x000000, 255);
        midp_graphics_draw_string(gfx, "Select", screen_width - 45, screen_height - SOFT_BUTTON_HEIGHT + 5, 0);
        
    } else {
        /* Normal soft button bar */
        midp_graphics_set_color(gfx, 0xC0C0C0, 255);  /* Light gray */
        midp_graphics_fill_rect(gfx, 0, screen_height - SOFT_BUTTON_HEIGHT, 
                                screen_width, SOFT_BUTTON_HEIGHT);
        
        /* Draw top border */
        midp_graphics_set_color(gfx, 0x808080, 255);  /* Dark gray */
        midp_graphics_draw_line(gfx, 0, screen_height - SOFT_BUTTON_HEIGHT,
                                screen_width, screen_height - SOFT_BUTTON_HEIGHT);
        
        int button_width = screen_width / 2;  /* Two buttons max: left and right */
        
        if (cmd_count > 2) {
            /* More than 2 commands: show "Menu" on left */
            midp_graphics_set_color(gfx, 0x000000, 255);
            midp_graphics_draw_string(gfx, "Menu", 5, screen_height - SOFT_BUTTON_HEIGHT + 5, 0);
            
            /* Right button: show BACK/EXIT command or first command */
            int left_idx, right_idx;
            get_soft_button_commands(jvm, displayable, &left_idx, &right_idx);
            
            if (right_idx >= 0 && cmd_data[right_idx]) {
                JavaObject* cmd = (JavaObject*)cmd_data[right_idx];
                const char* text = get_command_label_safe(jvm, cmd);
                midp_graphics_set_color(gfx, 0x000000, 255);
                int text_len = strlen(text);
                int text_width = text_len * 6;
                midp_graphics_draw_string(gfx, text, screen_width - text_width - 5, 
                                         screen_height - SOFT_BUTTON_HEIGHT + 5, 0);
            }
        } else {
            /* 1-2 commands: show based on type priority */
            int left_idx, right_idx;
            get_soft_button_commands(jvm, displayable, &left_idx, &right_idx);
            
            /* Left button */
            if (left_idx >= 0 && cmd_data[left_idx]) {
                JavaObject* cmd = (JavaObject*)cmd_data[left_idx];
                const char* text = get_command_label_safe(jvm, cmd);
                midp_graphics_set_color(gfx, 0x000000, 255);
                midp_graphics_draw_string(gfx, text, 5, screen_height - SOFT_BUTTON_HEIGHT + 5, 0);
            }
            
            /* Right button */
            if (right_idx >= 0 && cmd_data[right_idx]) {
                JavaObject* cmd = (JavaObject*)cmd_data[right_idx];
                const char* text = get_command_label_safe(jvm, cmd);
                midp_graphics_set_color(gfx, 0x000000, 255);
                int text_len = strlen(text);
                int text_width = text_len * 6;
                midp_graphics_draw_string(gfx, text, screen_width - text_width - 5, 
                                         screen_height - SOFT_BUTTON_HEIGHT + 5, 0);
            }
        }
    }
    
    /* Restore clip state */
    gfx->clip_x = saved_clip_x;
    gfx->clip_y = saved_clip_y;
    gfx->clip_width = saved_clip_width;
    gfx->clip_height = saved_clip_height;
    gfx->translate_x = saved_translate_x;
    gfx->translate_y = saved_translate_y;
}

/* Call commandAction for a specific command index (0 = left soft button, 1 = right soft button) */
static void call_command_action(JVM* jvm, JavaObject* displayable, int command_index) {
    DISP_DEBUG("[CmdAction] displayable=%p, cmd_index=%d", (void*)displayable, command_index);
    
    if (!displayable) return;
    
    /* Find commands field in displayable */
    int commands_idx = find_field_index(displayable, "commands");
    if (commands_idx < 0) {
        DISP_DEBUG("[CmdAction] commands field not found");
        return;
    }
    
    JavaArray* commands = (JavaArray*)displayable->fields[commands_idx].ref;
    if (!commands || command_index >= commands->length) {
        DISP_DEBUG("[CmdAction] No command at index %d", command_index);
        return;
    }
    
    void** cmd_data = (void**)array_data(commands);
    JavaObject* cmd = (JavaObject*)cmd_data[command_index];
    if (!cmd) {
        DISP_DEBUG("[CmdAction] Command %d is NULL", command_index);
        return;
    }
    
    /* Get command label for debug */
    const char* cmd_label = get_command_label_safe(jvm, cmd);
    DISP_DEBUG("[CmdAction] Command '%s' at index %d", cmd_label, command_index);
    
    /* Find listener field */
    int listener_idx = find_field_index(displayable, "listener");
    DISP_DEBUG("[CmdAction] listener_idx=%d", listener_idx);
    
    if (listener_idx < 0) {
        DISP_DEBUG("[CmdAction] Trying commandListener field...");
        listener_idx = find_field_index(displayable, "commandListener");
    }
    
    if (listener_idx < 0) {
        DISP_DEBUG("[CmdAction] No listener field found");
        return;
    }
    
    JavaObject* listener = (JavaObject*)displayable->fields[listener_idx].ref;
    DISP_DEBUG("[CmdAction] listener=%p", (void*)listener);
    
    if (!listener) {
        DISP_DEBUG("[CmdAction] No listener set");
        return;
    }
    
    if (!listener->header.clazz) {
        DISP_DEBUG("[CmdAction] Listener has NULL class");
        return;
    }
    
    DISP_DEBUG("[CmdAction] Listener class: %s", listener->header.clazz->class_name);
    
    /* Find and call commandAction(Command, Displayable) method */
    JavaClass* listener_class = listener->header.clazz;
    JavaMethod* method = jvm_resolve_method(jvm, listener_class, "commandAction",
        "(Ljavax/microedition/lcdui/Command;Ljavax/microedition/lcdui/Displayable;)V");
    
    DISP_DEBUG("[CmdAction] method=%p", (void*)method);
    
    if (method) {
        DISP_DEBUG("[CmdAction] method is_native=%d", method->is_native);
        
        /* Если код отсутствует, попробуем native lookup */
        if (!method->code.code || method->code.code_length == 0) {
            NativeMethod native_handler = native_find(jvm, listener_class->class_name, "commandAction",
                "(Ljavax/microedition/lcdui/Command;Ljavax/microedition/lcdui/Displayable;)V");
            DISP_DEBUG("[CmdAction] native_handler=%p", (void*)native_handler);
        }
        
        JavaValue args[3];
        args[0].ref = listener;       /* this */
        args[1].ref = cmd;            /* Command */
        args[2].ref = displayable;    /* Displayable */
        
        JavaValue result;
        JavaThread* thread = jvm_current_thread(jvm);
        
        DISP_DEBUG("[CmdAction] Calling execute_method for commandAction...");
        
        int exec_result = execute_method(jvm, thread, method, args, &result);
        
        DISP_DEBUG("[CmdAction] commandAction executed, result=%d", exec_result);
        
        /* Clear any pending exception to prevent it from poisoning future calls.
         * Some midlets throw exceptions intentionally (e.g., unsupported features).
         * Without clearing, the next execute_method() would immediately return -1.
         */
        if (thread && thread->pending_exception) {
            DISP_DEBUG("[CmdAction] Clearing pending exception after commandAction");
            thread->pending_exception = NULL;
        }
        
        /* Проверяем состояние JVM после commandAction.
         * Если был вызван notifyDestroyed(), jvm->running будет false.
         */
        if (jvm && !jvm->running) {
            DISP_DEBUG("[CmdAction] JVM stopped (notifyDestroyed)");
            SdlContext* sdl_ctx = sdl_get_global_context();
            if (sdl_ctx) {
                sdl_ctx->running = false;
            }
        }
        
        /* FALLBACK: Если JVM все еще работает, проверим тип команды.
         * EXIT (type=7), STOP (type=1), BACK (type=2) - останавливаем JVM.
         */
        if (jvm && jvm->running && cmd) {
            int type_idx = find_field_index(cmd, "type");
            if (type_idx >= 0) {
                jint cmd_type = cmd->fields[type_idx].i;
                DISP_DEBUG("[CmdAction] Command type=%d", cmd_type);
                
                if (cmd_type == 7 || cmd_type == 1 || cmd_type == 2) {
                    DISP_DEBUG("[CmdAction] EXIT/STOP/BACK command, stopping");
                    jvm->running = false;
                    SdlContext* sdl_ctx = sdl_get_global_context();
                    if (sdl_ctx) {
                        sdl_ctx->running = false;
                    }
                }
            }
        }
        
        /* Запрашиваем перерисовку после commandAction */
        sdl_request_redraw();
    } else {
        DISP_DEBUG("[CmdAction] commandAction method NOT FOUND in %s", listener_class->class_name);
    }
}

/* Check if current displayable has commands */
bool midp_has_commands(void) {
    if (!current_displayable_obj) {
        return false;
    }
    
    int commands_idx = find_field_index(current_displayable_obj, "commands");
    if (commands_idx < 0) return false;
    
    JavaArray* commands = (JavaArray*)current_displayable_obj->fields[commands_idx].ref;
    if (!commands || commands->length == 0) return false;
    
    return true;
}

/* Check if current displayable is a Canvas subclass (for auto-redraw in main loop) */
bool midp_has_active_canvas(void) {
    if (!current_displayable_obj) {
        return false;
    }
    
    JavaClass* clazz = current_displayable_obj->header.clazz;
    if (!clazz) return false;
    
    /* Check class hierarchy for Canvas */
    JavaClass* check = clazz;
    while (check) {
        if (check->class_name) {
            if (strcmp(check->class_name, "javax/microedition/lcdui/Canvas") == 0) {
                return true;  /* Current displayable is a Canvas or GameCanvas */
            }
        }
        check = check->super_class;
    }
    
    return false;
}

/* Command type constants (from MIDP spec) */
#define CMD_SCREEN  1
#define CMD_BACK    2
#define CMD_CANCEL  3
#define CMD_OK      4
#define CMD_HELP    5
#define CMD_STOP    6
#define CMD_EXIT    7
#define CMD_ITEM    8

/* Find the best left/right command indices based on type priority.
 * Returns: left_idx and right_idx via pointers.
 * Sets right_idx to -1 if no command for right side.
 * For >2 commands, returns left_idx=-1 (meaning: show menu on left). */
static void get_soft_button_commands(JVM* jvm, JavaObject* displayable, 
                                     int* left_idx, int* right_idx) {
    *left_idx = -1;
    *right_idx = -1;
    
    int commands_idx = find_field_index(displayable, "commands");
    if (commands_idx < 0) return;
    
    JavaArray* commands = (JavaArray*)displayable->fields[commands_idx].ref;
    if (!commands || commands->length == 0) return;
    
    void** cmd_data = (void**)array_data(commands);
    int cmd_count = commands->length;
    
    /* Find BACK or EXIT for right button */
    int back_exit_idx = -1;
    int ok_idx = -1;
    
    for (int i = 0; i < cmd_count; i++) {
        JavaObject* cmd = (JavaObject*)cmd_data[i];
        if (!cmd || !OBJECT_HAS_FIELDS(cmd, 3)) continue;
        int cmd_type = cmd->fields[1].i;
        int priority = cmd->fields[2].i;
        
        if ((cmd_type == CMD_BACK || cmd_type == CMD_EXIT) && back_exit_idx < 0) {
            back_exit_idx = i;
        }
        if (cmd_type == CMD_OK && ok_idx < 0) {
            ok_idx = i;
        }
    }
    
    if (cmd_count == 1) {
        /* Single command: show on left */
        *left_idx = 0;
    } else if (cmd_count == 2) {
        /* Two commands: primary left, secondary right */
        if (back_exit_idx >= 0) {
            /* One of them is BACK/EXIT → put on right */
            *right_idx = back_exit_idx;
            *left_idx = (back_exit_idx == 0) ? 1 : 0;
        } else {
            *left_idx = 0;
            *right_idx = 1;
        }
    } else {
        /* 3+ commands: menu on left, right button gets BACK/EXIT or first command */
        if (back_exit_idx >= 0) {
            *right_idx = back_exit_idx;
        } else {
            *right_idx = 0;  /* Default to first command */
        }
        *left_idx = -1;  /* Signal: show "Menu" on left */
    }
}

/* Handle soft button press - called from key handling
 * According to MIDP specification:
 * - If displayable has Commands, call commandAction()
 * - If displayable has no Commands, return false so keyPressed(-6/-7) is passed to game
 */
bool midp_handle_soft_button(JVM* jvm, int button_index) {
    DISP_DEBUG("[SoftBtnHandle] button_index=%d, fullscreen=%d",
            button_index, g_full_screen_mode);
    
    if (g_full_screen_mode) {
        return false;  /* Soft buttons not active in full screen mode */
    }
    
    if (!current_displayable_obj) {
        return false;
    }
    
    /* Get command count */
    int commands_idx = find_field_index(current_displayable_obj, "commands");
    DISP_DEBUG("[SoftBtnHandle] commands_idx=%d", commands_idx);
    
    if (commands_idx < 0) return false;
    
    JavaArray* commands = (JavaArray*)current_displayable_obj->fields[commands_idx].ref;
    if (!commands) {
        return false;
    }
    
    int cmd_count = commands->length;
    DISP_DEBUG("[SoftBtnHandle] cmd_count=%d", cmd_count);
    
    /* No commands - let keyPressed handle it */
    if (cmd_count == 0) {
        return false;
    }
    
    /* If menu is open */
    if (g_command_menu_open && cmd_count > 2) {
        if (button_index == 0) {
            /* Left button: Cancel - close menu */
            g_command_menu_open = false;
            g_command_menu_selected = 0;
            DISP_DEBUG("[SoftBtnHandle] Menu closed (cancel)");
            return true;
        } else if (button_index == 1) {
            /* Right button: Select - execute selected command */
            call_command_action(jvm, current_displayable_obj, g_command_menu_selected);
            g_command_menu_open = false;
            g_command_menu_selected = 0;
            return true;
        }
        return false;
    }
    
    /* Normal mode: determine left/right command mapping */
    int left_idx, right_idx;
    get_soft_button_commands(jvm, current_displayable_obj, &left_idx, &right_idx);
    
    if (left_idx < 0) {
        /* Menu mode (3+ commands): left=menu, right=back/exit */
        if (button_index == 0) {
            g_command_menu_open = true;
            g_command_menu_selected = 0;
            DISP_DEBUG("[SoftButton] Menu opened");
            return true;
        } else if (button_index == 1) {
            if (right_idx >= 0) {
                call_command_action(jvm, current_displayable_obj, right_idx);
                return true;
            }
        }
    } else {
        /* Direct mode (1-2 commands): left and right map directly */
        if (button_index == 0 && left_idx >= 0) {
            call_command_action(jvm, current_displayable_obj, left_idx);
            return true;
        } else if (button_index == 1 && right_idx >= 0) {
            call_command_action(jvm, current_displayable_obj, right_idx);
            return true;
        } else if (button_index == 1 && right_idx < 0 && left_idx >= 0) {
            /* Single command: right button also triggers it */
            call_command_action(jvm, current_displayable_obj, left_idx);
            return true;
        }
    }
    
    return false;
}

/* Handle menu navigation (UP/DOWN) when menu is open */
bool midp_handle_menu_navigation(int direction) {
    if (!g_command_menu_open || !current_displayable_obj) {
        return false;
    }
    
    int commands_idx = find_field_index(current_displayable_obj, "commands");
    if (commands_idx < 0) return false;
    
    JavaArray* commands = (JavaArray*)current_displayable_obj->fields[commands_idx].ref;
    if (!commands) return false;
    
    int cmd_count = commands->length;
    
    if (direction < 0) {
        /* UP */
        g_command_menu_selected--;
        if (g_command_menu_selected < 0) g_command_menu_selected = cmd_count - 1;
    } else {
        /* DOWN */
        g_command_menu_selected++;
        if (g_command_menu_selected >= cmd_count) g_command_menu_selected = 0;
    }
    
    DISP_DEBUG("[SoftButton] Menu selection: %d", g_command_menu_selected);
    return true;
}

/* Check if command menu is open */
bool midp_is_command_menu_open(void) {
    return g_command_menu_open;
}

/* Close command menu */
void midp_close_command_menu(void) {
    g_command_menu_open = false;
    g_command_menu_selected = 0;
}

/* SELECT_COMMAND singleton for List handling */
static JavaObject* g_display_select_command = NULL;

/* Ensure SELECT_COMMAND exists */
static void display_ensure_select_command(JVM* jvm) {
    if (g_display_select_command) return;
    
    JavaClass* cmd_class = jvm_load_class(jvm, "javax/microedition/lcdui/Command");
    if (!cmd_class) return;
    
    g_display_select_command = jvm_new_object(jvm, cmd_class);
    if (g_display_select_command && OBJECT_HAS_FIELDS(g_display_select_command, 3)) {
        JavaString* label = jvm_new_string(jvm, "Select");
        g_display_select_command->fields[0].ref = label;     /* label */
        g_display_select_command->fields[1].i = 4;          /* SCREEN type */
        g_display_select_command->fields[2].i = 0;          /* priority */
    }
    
    DISP_DEBUG("[display] Created SELECT_COMMAND: %p", (void*)g_display_select_command);
}

/* Helper for List FIRE handling - call commandAction with SELECT_COMMAND 
 * This is a fallback when form.c can't find the listener field directly.
 * Returns true if commandAction was called successfully. */
bool call_command_action_for_list(JVM* jvm, JavaObject* list, int selected_index) {
    if (!jvm || !list) return false;
    
    DISP_DEBUG("[call_command_action_for_list] list=%p, selected=%d", (void*)list, selected_index);
    
    JavaClass* list_class = list->header.clazz;
    if (!list_class) return false;
    
    /* Get commands array */
    int commands_idx = find_field_index(list, "commands");
    if (commands_idx < 0) {
        DISP_DEBUG("[call_command_action_for_list] commands field not found");
        return false;
    }
    
    JavaArray* commands = (JavaArray*)list->fields[commands_idx].ref;
    int cmd_count = commands ? commands->length : 0;
    
    DISP_DEBUG("[call_command_action_for_list] found %d commands", cmd_count);
    
    /* Check the list type */
    int listtype_idx = find_field_index(list, "listType");
    int list_type = 4; /* Default to IMPLICIT */
    if (listtype_idx >= 0) {
        list_type = list->fields[listtype_idx].i;
    }
    
    DISP_DEBUG("[call_command_action_for_list] list_type=%d (IMPLICIT=4)", list_type);
    
    /* Find the CommandListener */
    int listener_idx = find_field_index(list, "listener");
    if (listener_idx < 0) {
        listener_idx = find_field_index(list, "commandListener");
    }
    
    if (listener_idx >= 0) {
        JavaObject* listener = (JavaObject*)list->fields[listener_idx].ref;
        if (listener) {
            DISP_DEBUG("[call_command_action_for_list] found listener at idx %d", listener_idx);
            
            /* Create or get SELECT_COMMAND */
            display_ensure_select_command(jvm);
            
            if (g_display_select_command) {
                JavaClass* listener_class = listener->header.clazz;
                JavaMethod* method = jvm_resolve_method(jvm, listener_class, "commandAction",
                    "(Ljavax/microedition/lcdui/Command;Ljavax/microedition/lcdui/Displayable;)V");
                
                if (method) {
                    JavaValue args[3];
                    args[0].ref = listener;
                    args[1].ref = g_display_select_command;
                    args[2].ref = list;
                    
                    JavaValue result;
                    JavaThread* thread = jvm_current_thread(jvm);
                    execute_method(jvm, thread, method, args, &result);
                    DISP_DEBUG("[call_command_action_for_list] commandAction called successfully");
                    return true;
                }
            }
        }
    }
    
    /* Fallback: For IMPLICIT list with commands, call the first command */
    if (list_type == 4 && cmd_count > 0) {
        call_command_action(jvm, list, 0);
        return true;
    }
    
    DISP_DEBUG("[call_command_action_for_list] failed to call commandAction");
    return false;
}

/* Register global objects as GC roots */
static void register_gc_roots(JVM* jvm) {
    /* Register g_graphics_object as a GC root */
    gc_add_root(jvm, (void**)&g_graphics_object);
    /* Register current_displayable_obj as a GC root */
    gc_add_root(jvm, (void**)&current_displayable_obj);
}

/* Create a Java Graphics object bound to screen framebuffer */
static JavaObject* create_graphics_object(JVM* jvm) {
    if (g_graphics_object) {
        /* Reset clip to full screen before reusing */
        g_screen_graphics.clip_x = 0;
        g_screen_graphics.clip_y = 0;
        g_screen_graphics.clip_width = g_screen_graphics.width;
        g_screen_graphics.clip_height = g_screen_graphics.height;
        g_screen_graphics.translate_x = 0;
        g_screen_graphics.translate_y = 0;
        return g_graphics_object;
    }
    
    /* Get the Graphics class */
    JavaClass* gfx_class = jvm_load_class(jvm, "javax/microedition/lcdui/Graphics");
    if (!gfx_class) {
        ERROR_LOG("Failed to load Graphics class");
        return NULL;
    }
    
    /* Ensure class has space for nativePeer */
    ensure_native_peer_field(gfx_class);
    
    /* Create Graphics object */
    g_graphics_object = jvm_new_object(jvm, gfx_class);
    if (!g_graphics_object) {
        ERROR_LOG("Failed to create Graphics object");
        return NULL;
    }
    
    /* Register as GC root to prevent collection */
    gc_add_root(jvm, (void**)&g_graphics_object);
    
    /* Initialize the MidpGraphics structure for screen */
    SdlContext* sdl_ctx = sdl_get_global_context();
    if (sdl_ctx && sdl_ctx->framebuffer) {
        midp_graphics_init(&g_screen_graphics, sdl_ctx->framebuffer, 
                          sdl_ctx->width, sdl_ctx->height);
    }
    
    /* Store the MidpGraphics* in the nativePeer field */
    set_object_field_ref(g_graphics_object, "nativePeer", (JavaObject*)&g_screen_graphics);
    
    DISP_DEBUG("Created Graphics object: %p, MidpGraphics: %p (%dx%d)", 
            (void*)g_graphics_object, (void*)&g_screen_graphics,
            g_screen_graphics.width, g_screen_graphics.height);
    
    return g_graphics_object;
}

/* Get MidpGraphics from a Java Graphics object */
MidpGraphics* get_graphics_from_object(JavaObject* obj) {
    if (!obj) return NULL;
    /* For the screen graphics object, return the global */
    if (obj == g_graphics_object) {
        return &g_screen_graphics;
    }
    /* For other Graphics objects (from Images), get nativePeer field */
    MidpGraphics* gfx = (MidpGraphics*)get_object_field_ref(obj, "nativePeer");
    if (gfx) return gfx;
    /* Fallback to screen graphics */
    return &g_screen_graphics;
}

/* Event queue */
#define MAX_EVENTS 256

static struct {
    MidpEvent events[MAX_EVENTS];
    int head;
    int tail;
    int count;
} event_queue;

/* Repaint queue - MIDP repaint() is asynchronous */
#define MAX_REPAINTS 16
static struct {
    JavaObject* canvas;
    int x, y, w, h;
    bool pending;
} repaint_queue[MAX_REPAINTS];
static int repaint_head = 0;
static int repaint_tail = 0;

/* Flag: Canvas.repaint() or repaint(x,y,w,h) was called.
 * Cleared after midp_process_repaints() calls paint().
 * Prevents calling paint() every frame when the MIDlet didn't request it,
 * which would cause flickering (paint() catches offscreen buffer mid-update). */
static volatile bool g_canvas_repaint_requested = false;

/* Game canvas state */
static struct {
    int key_states;
    MidpGraphics* graphics;
    bool suppress_key_events;
    /* Offscreen buffer for GameCanvas */
    MidpImage* offscreen_buffer;
    int offscreen_width;
    int offscreen_height;
} game_canvas;

/* Platform callbacks */
static MidpPlatformCallbacks platform_callbacks;

/* Forward declarations */
extern int execute_method(JVM* jvm, JavaThread* thread, JavaMethod* method, JavaValue* args, JavaValue* result);

/* Call keyPressed on the current displayable */
void midp_call_keyPressed(JVM* jvm, int keycode) {
    if (!jvm || !current_displayable_obj) {
        DISP_DEBUG("[KEY] keyPressed: no current displayable (jvm=%p, current=%p)",
                (void*)jvm, (void*)current_displayable_obj);
        return;
    }
    
    JavaObject* displayable = current_displayable_obj;
    JavaClass* clazz = displayable->header.clazz;
    
    if (!clazz) {
        DISP_DEBUG("[KEY] keyPressed: displayable has no class");
        return;
    }
    
    /* DEBUG: Log keyPressed for class 'a' (Bobby Carrot) */
    static int key_log_count = 0;
    if (clazz->class_name && strcmp(clazz->class_name, "a") == 0 && key_log_count < 10) {
        DISP_DEBUG("[KEY_EVENT] keyPressed(%d) on class 'a'", keycode);
        key_log_count++;
    }
    
    DISP_DEBUG("[KEY] Current displayable class: %s", 
            clazz->class_name ? clazz->class_name : "NULL");
    
    /* Check if this is a GameCanvas with suppressed key events */
    if (game_canvas.suppress_key_events && keycode < 0) {
        /* Game actions (negative keycodes) are suppressed for GameCanvas */
        bool is_game_canvas = false;
        JavaClass* check = clazz;
        while (check) {
            if (check->class_name && strcmp(check->class_name, "javax/microedition/lcdui/game/GameCanvas") == 0) {
                is_game_canvas = true;
                break;
            }
            check = check->super_class;
        }
        if (is_game_canvas) {
            DISP_DEBUG("[KEY] keyPressed: suppressed for GameCanvas (keycode=%d)", keycode);
            return;
        }
    }
    
    /* Check if this is a high-level UI that needs form key handling */
    if (clazz->class_name) {
        bool is_high_level_ui = false;
        JavaClass* check = clazz;
        while (check) {
            if (check->class_name) {
                if (strcmp(check->class_name, "javax/microedition/lcdui/Form") == 0 ||
                    strcmp(check->class_name, "javax/microedition/lcdui/List") == 0 ||
                    strcmp(check->class_name, "javax/microedition/lcdui/TextBox") == 0 ||
                    strcmp(check->class_name, "javax/microedition/lcdui/Alert") == 0) {
                    is_high_level_ui = true;
                    break;
                }
            }
            check = check->super_class;
        }
        
        if (is_high_level_ui) {
            /* Handle key for high-level UI (forms, lists, virtual keyboard) */
            DISP_DEBUG("keyPressed: high-level UI detected (%s)", clazz->class_name);
            
            /* Convert keycode to MIDP game action constant */
            /* Keycodes: -1=UP, -2=DOWN, -3=LEFT, -4=RIGHT, -5=FIRE, -6=SOFT1, -7=SOFT2 */
            /* Game actions: UP=1, DOWN=6, LEFT=2, RIGHT=5, FIRE=8 */
            int game_action = 0;
            switch (keycode) {
                case -1: game_action = 1; break;  /* GAME_UP */
                case -2: game_action = 6; break;  /* GAME_DOWN */
                case -3: game_action = 2; break;  /* GAME_LEFT */
                case -4: game_action = 5; break;  /* GAME_RIGHT */
                case -5: game_action = 8; break;  /* GAME_FIRE */
                default: game_action = keycode; break;
            }
            
            /* First check if command menu is open (for both SDL and libretro) */
            extern bool midp_is_command_menu_open(void);
            extern bool midp_handle_menu_navigation(int direction);
            if (midp_is_command_menu_open()) {
                if (game_action == 1) { /* UP */
                    midp_handle_menu_navigation(-1);
                    return;
                } else if (game_action == 6) { /* DOWN */
                    midp_handle_menu_navigation(1);
                    return;
                } else if (game_action == 8) { /* FIRE - confirm selection */
                    midp_handle_soft_button(jvm, 1);
                    return;
                }
            }
            
            /* Then check if virtual keyboard is active */
            if (midp_is_vkb_active()) {
                midp_vkb_process_key(jvm, game_action);
                /* Re-render the form/textbox */
                if (midp_is_vkb_active()) {
                    /* Keyboard still active, re-render */
                    if (strstr(clazz->class_name, "Form")) {
                        midp_render_form(jvm, displayable);
                    } else if (strstr(clazz->class_name, "TextBox")) {
                        midp_render_textbox(jvm, displayable, "", 0);
                    }
                } else {
                    /* Keyboard closed, re-render form */
                    midp_render_form(jvm, displayable);
                }
                return;
            }
            
            /* Handle form navigation */
            if (midp_form_handle_key(jvm, game_action)) {
                DISP_DEBUG("Key handled by form handler");
                return;
            }
            
            /* Key not handled, fall through to default behavior */
        }
    }
    
    /* Find keyPressed method */
    JavaMethod* method = jvm_resolve_method(jvm, clazz, "keyPressed", "(I)V");
    if (!method) {
        return;
    }
    
    /* Prepare arguments: this + keycode */
    JavaValue args[2];
    args[0].ref = displayable;       /* this */
    args[1].i = keycode;            /* keyCode */
    
    JavaValue result;
    JavaThread* thread = jvm_current_thread(jvm);
    
    int ret = execute_method(jvm, thread, method, args, &result);
    DISP_DEBUG("[Input] keyPressed returned %d", ret);
    
    /* Repaint the canvas after key press */
    if (ret == 0) {
        DISP_DEBUG("[Input] keyPressed successful, calling repaint...");
        
        /* Call paint to redraw */
        JavaObject* gfx_obj = create_graphics_object(jvm);
        if (gfx_obj) {
            JavaMethod* paint_method = jvm_resolve_method(jvm, clazz, "paint", "(Ljavax/microedition/lcdui/Graphics;)V");
            if (paint_method) {
                DISP_DEBUG("[Input] Calling paint() after keyPressed");
                
#ifdef J2ME_HEADLESS
                extern void headless_reset_text_capture(void);
                extern void headless_print_captured_text(void);
                headless_reset_text_capture();
#endif
                
                JavaValue paint_args[2];
                paint_args[0].ref = displayable;
                paint_args[1].ref = gfx_obj;
                execute_method(jvm, thread, paint_method, paint_args, &result);
                
#ifdef J2ME_HEADLESS
                headless_print_captured_text();
#endif
                
                /* ИСПРАВЛЕНО: Запрашиваем перерисовку вместо прямого present */
                /* Это предотвращает мерцание - SDL обновится в основном цикле */
                sdl_request_redraw();
            } else {
                DISP_DEBUG("[Input] paint() method not found!");
            }
        }
    }
}

/* Call keyReleased on the current displayable */
void midp_call_keyReleased(JVM* jvm, int keycode) {
    if (!jvm || !current_displayable_obj) return;
    
    JavaObject* canvas = current_displayable_obj;
    JavaClass* clazz = canvas->header.clazz;
    if (!clazz) return;
    
    /* Check if this is a GameCanvas with suppressed key events */
    if (game_canvas.suppress_key_events && keycode < 0) {
        /* Game actions (negative keycodes) are suppressed for GameCanvas */
        bool is_game_canvas = false;
        JavaClass* check = clazz;
        while (check) {
            if (check->class_name && strcmp(check->class_name, "javax/microedition/lcdui/game/GameCanvas") == 0) {
                is_game_canvas = true;
                break;
            }
            check = check->super_class;
        }
        if (is_game_canvas) {
            DISP_DEBUG("keyReleased: suppressed for GameCanvas (keycode=%d)", keycode);
            return;
        }
    }
    
    JavaMethod* method = jvm_resolve_method(jvm, clazz, "keyReleased", "(I)V");
    if (!method) return;
    
    DISP_DEBUG("[Input] Calling keyReleased(%d) on %s", keycode, clazz->class_name);
    
    JavaValue args[2];
    args[0].ref = canvas;
    args[1].i = keycode;
    
    JavaValue result;
    JavaThread* thread = jvm_current_thread(jvm);
    execute_method(jvm, thread, method, args, &result);
}

/* Call pointerPressed on the current displayable */
void midp_call_pointerPressed(JVM* jvm, int x, int y) {
    if (!jvm || !current_displayable_obj) return;
    
    JavaObject* canvas = current_displayable_obj;
    JavaClass* clazz = canvas->header.clazz;
    if (!clazz) return;
    
    JavaMethod* method = jvm_resolve_method(jvm, clazz, "pointerPressed", "(II)V");
    if (!method) {
        DISP_DEBUG("[Input] pointerPressed method not found in %s", clazz->class_name);
        return;
    }
    
    DISP_DEBUG("[Input] Calling pointerPressed(%d, %d) on %s", x, y, clazz->class_name);
    
    JavaValue args[3];
    args[0].ref = canvas;
    args[1].i = x;
    args[2].i = y;
    
    JavaValue result;
    JavaThread* thread = jvm_current_thread(jvm);
    execute_method(jvm, thread, method, args, &result);
}

/* Call pointerReleased on the current displayable */
void midp_call_pointerReleased(JVM* jvm, int x, int y) {
    if (!jvm || !current_displayable_obj) return;
    
    JavaObject* canvas = current_displayable_obj;
    JavaClass* clazz = canvas->header.clazz;
    if (!clazz) return;
    
    JavaMethod* method = jvm_resolve_method(jvm, clazz, "pointerReleased", "(II)V");
    if (!method) {
        DISP_DEBUG("[Input] pointerReleased method not found in %s", clazz->class_name);
        return;
    }
    
    DISP_DEBUG("[Input] Calling pointerReleased(%d, %d) on %s", x, y, clazz->class_name);
    
    JavaValue args[3];
    args[0].ref = canvas;
    args[1].i = x;
    args[2].i = y;
    
    JavaValue result;
    JavaThread* thread = jvm_current_thread(jvm);
    execute_method(jvm, thread, method, args, &result);
}

/* Call pointerDragged on the current displayable */
void midp_call_pointerDragged(JVM* jvm, int x, int y) {
    if (!jvm || !current_displayable_obj) return;
    
    JavaObject* canvas = current_displayable_obj;
    JavaClass* clazz = canvas->header.clazz;
    if (!clazz) return;
    
    JavaMethod* method = jvm_resolve_method(jvm, clazz, "pointerDragged", "(II)V");
    if (!method) {
        DISP_DEBUG("[Input] pointerDragged method not found in %s", clazz->class_name);
        return;
    }
    
    DISP_DEBUG("[Input] Calling pointerDragged(%d, %d) on %s", x, y, clazz->class_name);
    
    JavaValue args[3];
    args[0].ref = canvas;
    args[1].i = x;
    args[2].i = y;
    
    JavaValue result;
    JavaThread* thread = jvm_current_thread(jvm);
    execute_method(jvm, thread, method, args, &result);
}

/* Call showNotify on the current displayable */
void midp_call_showNotify(JVM* jvm) {
    if (!jvm || !current_displayable_obj) return;
    
    JavaObject* displayable = current_displayable_obj;
    JavaClass* clazz = displayable->header.clazz;
    if (!clazz) return;
    
    JavaMethod* method = jvm_resolve_method(jvm, clazz, "showNotify", "()V");
    if (!method) return;  /* Not all classes implement this */
    
    fprintf(stderr, "[DISPLAY] Calling showNotify() on %s\n", clazz->class_name);
    
    JavaValue args[1];
    args[0].ref = displayable;
    
    JavaValue result;
    JavaThread* thread = jvm_current_thread(jvm);
    execute_method(jvm, thread, method, args, &result);
    fprintf(stderr, "[DISPLAY] showNotify() completed\n");
}

/* Call hideNotify on the current displayable */
void midp_call_hideNotify(JVM* jvm) {
    if (!jvm || !current_displayable_obj) return;
    
    JavaObject* displayable = current_displayable_obj;
    JavaClass* clazz = displayable->header.clazz;
    if (!clazz) return;
    
    JavaMethod* method = jvm_resolve_method(jvm, clazz, "hideNotify", "()V");
    if (!method) return;  /* Not all classes implement this */
    
    DISP_DEBUG("Calling hideNotify() on %s", clazz->class_name);
    
    JavaValue args[1];
    args[0].ref = displayable;
    
    JavaValue result;
    JavaThread* thread = jvm_current_thread(jvm);
    execute_method(jvm, thread, method, args, &result);
}

/* Call sizeChanged on the current displayable */
void midp_call_sizeChanged(JVM* jvm, int w, int h) {
    if (!jvm || !current_displayable_obj) return;
    
    JavaObject* displayable = current_displayable_obj;
    JavaClass* clazz = displayable->header.clazz;
    if (!clazz) return;
    
    JavaMethod* method = jvm_resolve_method(jvm, clazz, "sizeChanged", "(II)V");
    if (!method) return;  /* Not all classes implement this */
    
    DISP_DEBUG("Calling sizeChanged(%d, %d) on %s", w, h, clazz->class_name);
    
    JavaValue args[3];
    args[0].ref = displayable;
    args[1].i = w;
    args[2].i = h;
    
    JavaValue result;
    JavaThread* thread = jvm_current_thread(jvm);
    execute_method(jvm, thread, method, args, &result);
}

/* Queue event */
void midp_event_queue(MidpEvent* event) {
    if (event_queue.count >= MAX_EVENTS) return;
    
    event_queue.events[event_queue.tail] = *event;
    event_queue.tail = (event_queue.tail + 1) % MAX_EVENTS;
    event_queue.count++;
}

/* Poll event */
bool midp_event_poll(MidpEvent* event) {
    if (event_queue.count == 0) return false;
    
    *event = event_queue.events[event_queue.head];
    event_queue.head = (event_queue.head + 1) % MAX_EVENTS;
    event_queue.count--;
    
    return true;
}

/* Process event */
void midp_event_process(JVM* jvm, MidpEvent* event) {
    if (!jvm || !event) return;
    
    /* TODO: Find the current Displayable and call appropriate methods */
    switch (event->type) {
        case MIDP_EVENT_KEY_PRESSED:
            /* Call keyPressed(int keyCode) */
            break;
            
        case MIDP_EVENT_KEY_RELEASED:
            /* Call keyReleased(int keyCode) */
            break;
            
        case MIDP_EVENT_KEY_REPEATED:
            /* Call keyRepeated(int keyCode) */
            break;
            
        case MIDP_EVENT_POINTER_PRESSED:
            /* Call pointerPressed(int x, int y) */
            break;
            
        case MIDP_EVENT_POINTER_RELEASED:
            /* Call pointerReleased(int x, int y) */
            break;
            
        case MIDP_EVENT_POINTER_DRAGGED:
            /* Call pointerDragged(int x, int y) */
            break;
            
        case MIDP_EVENT_COMMAND:
            /* Call commandAction */
            break;
            
        case MIDP_EVENT_SHOW_NOTIFY:
            /* Call showNotify */
            break;
            
        case MIDP_EVENT_HIDE_NOTIFY:
            /* Call hideNotify */
            break;
            
        case MIDP_EVENT_SIZE_CHANGED:
            /* Call sizeChanged */
            break;
            
        case MIDP_EVENT_REPAINT:
            /* Call paint */
            break;
    }
}

/* Set platform callbacks */
void midp_set_platform_callbacks(MidpPlatformCallbacks* callbacks) {
    if (callbacks) {
        platform_callbacks = *callbacks;
    }
}

/*
 * Display native methods
 */

/* Singleton Display instance */
static JavaObject* g_display_instance = NULL;

static JavaValue native_display_getDisplay(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    (void)args;  /* MIDlet parameter */
    
    DISP_DEBUG("getDisplay() called");
    
    /* Return singleton Display instance */
    if (!g_display_instance) {
        /* Create Display instance */
        JavaClass* display_class = jvm_load_class(jvm, "javax/microedition/lcdui/Display");
        if (display_class) {
            g_display_instance = jvm_new_object(jvm, display_class);
            /* Register as GC root to prevent collection */
            gc_add_root(jvm, (void**)&g_display_instance);
            DISP_DEBUG("Created Display instance: %p", (void*)g_display_instance);
        }
    }
    
    return NATIVE_RETURN_OBJECT(g_display_instance);
}

static JavaValue native_display_setCurrent(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* display = (JavaObject*)args[0].ref;
    JavaObject* displayable = (JavaObject*)args[1].ref;
    
    /* MIDP spec: setCurrent(null) clears the current displayable */
    if (!displayable) {
        DISP_DEBUG("setCurrent(null) — clearing current displayable");
        if (current_displayable_obj) {
            JavaObject* old = current_displayable_obj;
            current_displayable_obj = NULL;
            midp_call_hideNotify(jvm);
            current_displayable_obj = NULL;
            midp_set_current_displayable(NULL);
        }
        return NATIVE_RETURN_VOID();
    }
    
    /* Always log setCurrent for debugging */
    fprintf(stderr, "[DISPLAY] setCurrent called: display=%p, displayable=%p\n", 
            (void*)display, (void*)displayable);
    if (displayable->header.clazz) {
        fprintf(stderr, "[DISPLAY]   displayable class: %s, instance_size=%zu\n", 
                displayable->header.clazz->class_name ? displayable->header.clazz->class_name : "NULL",
                displayable->header.clazz->instance_size);
    } else {
        fprintf(stderr, "[DISPLAY]   displayable has NULL clazz!\n");
    }
    
    
    /* Call hideNotify() on the OLD displayable before switching
     * MIDP spec: hideNotify() must be called when a Displayable is
     * removed from the screen. Must be called BEFORE showNotify
     * on the new displayable. */
    if (current_displayable_obj && current_displayable_obj != displayable) {
        JavaObject* old_displayable = current_displayable_obj;
        DISP_DEBUG("Calling hideNotify() on old displayable %s",
                   old_displayable->header.clazz ? old_displayable->header.clazz->class_name : "(null)");
        /* Temporarily set current_displayable_obj to old for hideNotify */
        current_displayable_obj = old_displayable;
        midp_call_hideNotify(jvm);
    }
    
    /* Store current displayable - register as GC root if first time */
    if (!current_displayable_obj) {
        gc_add_root(jvm, (void**)&current_displayable_obj);
    }
    current_displayable_obj = displayable;
    
    /* Also update form.c's g_current_displayable for key handling */
    midp_set_current_displayable(displayable);
    
    /* Call showNotify() when displayable becomes current
     * MIDP spec: showNotify() must be called when a Displayable is
     * shown on the screen. */
    if (displayable) {
        midp_call_showNotify(jvm);
    }
    
    /* For Canvas, we need to call paint() automatically */
    if (displayable) {
        JavaClass* clazz = displayable->header.clazz;
        if (clazz && clazz->class_name) {
            fprintf(stderr, "[DISPLAY] Displayable class: %s\n", clazz->class_name);
            
            /* Check if this is a Form - ИСПРАВЛЕНО: проверяем только по иерархии классов */
            bool is_form = false;
            bool is_list = false;
            bool is_alert = false;
            bool is_textbox = false;
            bool is_canvas = false;
            
            JavaClass* check = clazz;
            while (check) {
                if (check->class_name) {
                    /* ИСПРАВЛЕНО: Используем только точное сравнение имен классов */
                    if (strcmp(check->class_name, "javax/microedition/lcdui/Form") == 0) {
                        is_form = true;
                        break;
                    }
                    if (strcmp(check->class_name, "javax/microedition/lcdui/List") == 0) {
                        is_list = true;
                        break;
                    }
                    if (strcmp(check->class_name, "javax/microedition/lcdui/Alert") == 0) {
                        is_alert = true;
                        break;
                    }
                    if (strcmp(check->class_name, "javax/microedition/lcdui/TextBox") == 0) {
                        is_textbox = true;
                        break;
                    }
                    if (strcmp(check->class_name, "javax/microedition/lcdui/Canvas") == 0) {
                        is_canvas = true;
                        break;
                    }
                }
                check = check->super_class;
            }
            
            if (is_form) {
                g_full_screen_mode = false;
                fprintf(stderr, "[DISPLAY] Form detected in setCurrent (is_form=true), calling midp_render_form...\n");
                midp_set_current_displayable(displayable);
                midp_render_form(jvm, displayable);
                /* ИСПРАВЛЕНО: Запрашиваем перерисовку */
                sdl_request_redraw();
                fprintf(stderr, "[DISPLAY] Form rendering complete, redraw requested\n");
            } else if (is_list) {
                DISP_DEBUG("List detected, rendering...");
                midp_set_current_displayable(displayable);
                midp_render_list(jvm, displayable, 0);
                /* ИСПРАВЛЕНО: Запрашиваем перерисовку */
                sdl_request_redraw();
            } else if (is_alert) {
                DISP_DEBUG("Alert detected, rendering...");
                midp_set_current_displayable(displayable);
                JavaObject* gfx_obj = create_graphics_object(jvm);
                if (gfx_obj) {
                    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
                    if (gfx) {
                        midp_render_alert(jvm, gfx, displayable);
                        /* ИСПРАВЛЕНО: Запрашиваем перерисовку */
                        sdl_request_redraw();
                    }
                }
            } else if (is_textbox) {
                DISP_DEBUG("TextBox detected, rendering...");
                midp_set_current_displayable(displayable);
                midp_render_textbox(jvm, displayable, "", 0);
                /* ИСПРАВЛЕНО: Запрашиваем перерисовку */
                sdl_request_redraw();
            } else if (is_canvas) {
                DISP_DEBUG("Canvas detected, calling paint()...");
                
                /* Nokia FullCanvas: auto-enable full screen mode.
                 * FullCanvas is a Nokia extension that removes the title/command bar
                 * and gives the game the full screen. The game expects soft keys to
                 * be passed directly to keyPressed(), not intercepted by Command system. */
                bool is_full_canvas = false;
                {
                    JavaClass* fc_check = clazz;
                    while (fc_check) {
                        if (fc_check->class_name &&
                            strcmp(fc_check->class_name, "com/nokia/mid/ui/FullCanvas") == 0) {
                            is_full_canvas = true;
                            break;
                        }
                        fc_check = fc_check->super_class;
                    }
                }
                if (is_full_canvas) {
                    g_full_screen_mode = true;
                    DISP_DEBUG("Nokia FullCanvas detected, full screen mode enabled");
                } else {
                    g_full_screen_mode = false;
                }
                
                /* CRITICAL FIX: Clear screen before Canvas paint()!
                 * When switching from List/Form to Canvas, the old content
                 * remains in the framebuffer. We must clear it first. */
                SdlContext* sdl_ctx = sdl_get_global_context();
                if (sdl_ctx) {
                    sdl_clear(sdl_ctx, 0xFFFFFFFF);  /* Clear to white */
                }
                
                /* Create Java Graphics object bound to screen */
                JavaObject* gfx_obj = create_graphics_object(jvm);
                if (gfx_obj) {
                    /* For Canvas/GameCanvas: game handles its own UI, uses full screen.
                     * The game draws its own soft buttons and handles key events.
                     * We do NOT reduce clip height - game can use entire screen.
                     */
                    
                    DISP_DEBUG("Graphics object: %p, MidpGraphics: %p (%dx%d), clip_height=%d", 
                            (void*)gfx_obj, (void*)&g_screen_graphics, 
                            g_screen_graphics.width, g_screen_graphics.height,
                            g_screen_graphics.clip_height);
                    
                    /* Find and call paint(Graphics g) method */
                    JavaMethod* paint_method = jvm_resolve_method(jvm, clazz, "paint", "(Ljavax/microedition/lcdui/Graphics;)V");
                    if (paint_method) {
                        DISP_DEBUG("Found paint() method, calling...");
                        
                        /* Call paint(gfx) - args[0] = this, args[1] = Graphics */
                        JavaValue paint_args[2];
                        paint_args[0].ref = displayable;  /* this */
                        paint_args[1].ref = gfx_obj;      /* Graphics object */
                        
                        JavaValue result;
                        JavaThread* th = jvm_current_thread(jvm);
                        int ret = execute_method(jvm, th, paint_method, paint_args, &result);
                        
                        DISP_DEBUG("paint() returned %d", ret);
                        
                        /* Render soft buttons (commands) after paint() - only for high-level UI */
                        render_soft_buttons(jvm, displayable);
                        
                        /* ИСПРАВЛЕНО: Запрашиваем перерисовку */
                        sdl_request_redraw();
                    } else {
                        DISP_DEBUG("paint() method not found in %s", clazz->class_name);
                    }
                } else {
                    DISP_DEBUG("Failed to create Graphics object");
                }
            }
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Display.getCurrent() - return current displayable */
static JavaValue native_display_getCurrent(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* display = (JavaObject*)args[0].ref;
    (void)display;  /* Display instance not used */
    
    DISP_DEBUG("getCurrent() -> displayable=%p", (void*)current_displayable_obj);
    
    return NATIVE_RETURN_OBJECT(current_displayable_obj);
}

/*
 * ============================================
 * CallSerially Queue Implementation
 * Stores Runnables for deferred execution on UI thread
 * ============================================
 */

#define CALLSERIALLY_QUEUE_SIZE 64

static struct {
    JavaObject* runnables[CALLSERIALLY_QUEUE_SIZE];
    int head;
    int tail;
    int count;
    bool gc_registered;
} g_call_serially_queue;

/* Queue a Runnable for later execution */
static bool call_serially_enqueue(JVM* jvm, JavaObject* runnable) {
    if (g_call_serially_queue.count >= CALLSERIALLY_QUEUE_SIZE) {
        DISP_DEBUG("[CallSerially] Queue full, cannot enqueue");
        return false;
    }
    
    g_call_serially_queue.runnables[g_call_serially_queue.tail] = runnable;
    g_call_serially_queue.tail = (g_call_serially_queue.tail + 1) % CALLSERIALLY_QUEUE_SIZE;
    g_call_serially_queue.count++;
    
    /* Register all queue slots as GC roots (only once) */
    if (!g_call_serially_queue.gc_registered) {
        for (int i = 0; i < CALLSERIALLY_QUEUE_SIZE; i++) {
            gc_add_root(jvm, (void**)&g_call_serially_queue.runnables[i]);
        }
        g_call_serially_queue.gc_registered = true;
    }
    
    return true;
}

/* Dequeue and execute all pending Runnables - called from event loop */
void midp_process_call_serially_queue(JVM* jvm) {
    JavaThread* thread = jvm_current_thread(jvm);
    
    while (g_call_serially_queue.count > 0) {
        JavaObject* runnable = g_call_serially_queue.runnables[g_call_serially_queue.head];
        g_call_serially_queue.runnables[g_call_serially_queue.head] = NULL;  /* Clear slot for GC */
        g_call_serially_queue.head = (g_call_serially_queue.head + 1) % CALLSERIALLY_QUEUE_SIZE;
        g_call_serially_queue.count--;
        
        if (runnable && runnable->header.clazz) {
            JavaClass* runnable_class = runnable->header.clazz;
            JavaMethod* run_method = jvm_resolve_method(jvm, runnable_class, "run", "()V");
            
            if (run_method) {
                JavaValue run_args[1];
                run_args[0].ref = runnable;
                JavaValue result;
                execute_method(jvm, thread, run_method, run_args, &result);
            }
        }
    }
}

/* Display.callSerially(Runnable) - queue Runnable for later execution */
static JavaValue native_display_callSerially(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* display = (JavaObject*)args[0].ref;
    JavaObject* runnable = (JavaObject*)args[1].ref;
    (void)display;
    
    if (!runnable) {
        DISP_DEBUG("callSerially: Runnable is NULL");
        return NATIVE_RETURN_VOID();
    }
    
    DISP_DEBUG("callSerially: queueing Runnable %p for deferred execution", (void*)runnable);
    fprintf(stderr, "[CALLSERIALLY] queued Runnable class=%s\n",
            runnable && runnable->header.clazz && runnable->header.clazz->class_name ? runnable->header.clazz->class_name : "?");
    
    /* Queue the Runnable instead of executing immediately */
    if (!call_serially_enqueue(jvm, runnable)) {
        DISP_DEBUG("callSerially: failed to queue Runnable");
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_display_getWidth(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    int width, height;
    midp_display_get_dimensions(&width, &height);
    return NATIVE_RETURN_INT(width);
}

/* Canvas.getGameAction(int keyCode) - convert key code to game action */
static JavaValue native_canvas_getGameAction(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    /* args[0] = this (Canvas object), args[1] = keyCode (first method argument) */
    jint keyCode = args[1].i;
    
    /* Конвертируем keyCode в game action:
     * - keyCode=-1 (FULL_UP) → gameAction=1 (UP)
     * - keyCode=-2 (FULL_DOWN) → gameAction=6 (DOWN)
     * - keyCode=-3 (FULL_LEFT) → gameAction=2 (LEFT)
     * - keyCode=-4 (FULL_RIGHT) → gameAction=5 (RIGHT)
     * - keyCode=-5 (FULL_FIRE) → gameAction=8 (FIRE)
     * - keyCode=-6 (SOFT_LEFT) → gameAction=8 (FIRE)
     * - keyCode=-7 (SOFT_RIGHT) → gameAction=8 (FIRE)
     * - keyCode=-8 → gameAction=11 (GAME_C)
     * - keyCode=-9 → gameAction=12 (GAME_D)
     *
     * Note: Soft keys (-6, -7) map to FIRE instead of GAME_A/GAME_B because:
     * 1. Many MIDlets check getGameAction()==FIRE for action/selection
     * 2. When commands exist, midp_handle_soft_button() intercepts -6/-7
     *    so keyPressed() is never called (soft keys work as command keys)
     * 3. When no commands (game mode), soft keys should act as FIRE
     *    matching real phone behavior where left soft = OK/Select
     */
    
    /* Nokia FullCanvas keyCode to game action mapping */
    switch (keyCode) {
        case -1:  /* FULL_UP */
            return NATIVE_RETURN_INT(1);
        case -2:  /* FULL_DOWN */
            return NATIVE_RETURN_INT(6);
        case -3:  /* FULL_LEFT */
            return NATIVE_RETURN_INT(2);
        case -4:  /* FULL_RIGHT */
            return NATIVE_RETURN_INT(5);
        case -5:  /* FULL_FIRE */
            return NATIVE_RETURN_INT(8);
        case -6:
            return NATIVE_RETURN_INT(8);
        case -7:
            return NATIVE_RETURN_INT(8);
        case -8:
            return NATIVE_RETURN_INT(11);
        case -9:
            return NATIVE_RETURN_INT(12);
    }
    
    /* ITU-T key codes (standard MIDP phone keypad):
     * Key 2 (keyCode 50) = UP, Key 4 (52) = LEFT,
     * Key 5 (53) = FIRE, Key 6 (54) = RIGHT, Key 8 (56) = DOWN.
     * Many MIDlets check raw keyCode (n == 53) instead of getGameAction(). */
    switch (keyCode) {
        case 50: /* ITU-T key '2' = UP */
            return NATIVE_RETURN_INT(1);
        case 52: /* ITU-T key '4' = LEFT */
            return NATIVE_RETURN_INT(2);
        case 53: /* ITU-T key '5' = FIRE */
            return NATIVE_RETURN_INT(8);
        case 54: /* ITU-T key '6' = RIGHT */
            return NATIVE_RETURN_INT(5);
        case 56: /* ITU-T key '8' = DOWN */
            return NATIVE_RETURN_INT(6);
    }
    
    /* Для цифр и других клавиш game action не определён */
    /* Но некоторые игры могут передавать gameAction как keyCode */
    if (keyCode >= 1 && keyCode <= 12) {
        return NATIVE_RETURN_INT(keyCode);
    }
    
    /* No game action for this key */
    return NATIVE_RETURN_INT(0);
}

/* Canvas.getKeyCode(int gameAction) - convert game action to key code */
static JavaValue native_canvas_getKeyCode(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    /* args[0] = this (Canvas object), args[1] = gameAction */
    jint gameAction = args[1].i;
    
    /* ИСПРАВЛЕНО: Возвращаем keyCode по стандарту Nokia FullCanvas:
     * UP(1) → keyCode=-1, LEFT(2) → keyCode=-3, RIGHT(5) → keyCode=-4,
     * DOWN(6) → keyCode=-2, FIRE(8) → keyCode=-5
     * GAME_A(9) → keyCode=-6, GAME_B(10) → keyCode=-7,
     * GAME_C(11) → keyCode=-8, GAME_D(12) → keyCode=-9
     */
    switch (gameAction) {
        case 1:  /* UP */
            DISP_DEBUG("getKeyCode(UP) -> -1");
            return NATIVE_RETURN_INT(-1);
        case 2:  /* LEFT */
            DISP_DEBUG("getKeyCode(LEFT) -> -3");
            return NATIVE_RETURN_INT(-3);
        case 5:  /* RIGHT */
            DISP_DEBUG("getKeyCode(RIGHT) -> -4");
            return NATIVE_RETURN_INT(-4);
        case 6:  /* DOWN */
            DISP_DEBUG("getKeyCode(DOWN) -> -2");
            return NATIVE_RETURN_INT(-2);
        case 8:  /* FIRE */
            DISP_DEBUG("getKeyCode(FIRE) -> -5");
            return NATIVE_RETURN_INT(-5);
        case 9:  /* GAME_A */
            DISP_DEBUG("getKeyCode(GAME_A) -> -6");
            return NATIVE_RETURN_INT(-6);
        case 10: /* GAME_B */
            DISP_DEBUG("getKeyCode(GAME_B) -> -7");
            return NATIVE_RETURN_INT(-7);
        case 11: /* GAME_C */
            DISP_DEBUG("getKeyCode(GAME_C) -> -8");
            return NATIVE_RETURN_INT(-8);
        case 12: /* GAME_D */
            DISP_DEBUG("getKeyCode(GAME_D) -> -9");
            return NATIVE_RETURN_INT(-9);
        default:
            DISP_DEBUG("getKeyCode(%d) -> 0 (invalid game action)", gameAction);
            return NATIVE_RETURN_INT(0);
    }
}

/* Canvas.getKeyName(int keyCode) - get name for a key */
static JavaValue native_canvas_getKeyName(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    /* args[0] = this (Canvas object), args[1] = keyCode */
    jint keyCode = args[1].i;
    
    const char* keyName = NULL;
    
    /* Handle game keys (Nokia FullCanvas keyCode values) */
    /* -1=UP, -2=DOWN, -3=LEFT, -4=RIGHT, -5=FIRE */
    /* -6=GAME_A, -7=GAME_B, -8=GAME_C, -9=GAME_D */
    switch (keyCode) {
        case -1:  keyName = "UP"; break;
        case -2:  keyName = "DOWN"; break;
        case -3:  keyName = "LEFT"; break;
        case -4:  keyName = "RIGHT"; break;
        case -5:  keyName = "FIRE"; break;
        case -6:  keyName = "SOFTKEY1"; break;
        case -7:  keyName = "SOFTKEY2"; break;
        case -8:  keyName = "GAME_C"; break;
        case -9:  keyName = "GAME_D"; break;
    }
    
    /* Handle standard ITU-T keys */
    if (!keyName) {
        if (keyCode >= '0' && keyCode <= '9') {
            static char num_name[2] = {0, 0};
            num_name[0] = (char)keyCode;
            keyName = num_name;
        }
        else if (keyCode == '*') {
            keyName = "STAR";
        }
        else if (keyCode == '#') {
            keyName = "POUND";
        }
        else {
            keyName = "Unknown";
        }
    }
    
    DISP_DEBUG("getKeyName(%d) -> \"%s\"", keyCode, keyName);
    
    /* Create Java string */
    JavaString* str = jvm_new_string(jvm, (char*)keyName);
    if (!str) {
        ERROR_LOG("getKeyName: failed to create string");
        str = jvm_new_string(jvm, "Unknown");
        if (!str) {
            /* Fallback - should never happen, but return null */
            return NATIVE_RETURN_NULL();
        }
    }
    
    return NATIVE_RETURN_OBJECT(str);
}

/* Canvas.hasPointerEvents() - check if device supports pointer events */
static JavaValue native_canvas_hasPointerEvents(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* Desktop emulators always support pointer events (mouse) */
    return NATIVE_RETURN_INT(1);  /* true */
}

/* Canvas.hasPointerMotionEvents() - check if device supports pointer drag */
static JavaValue native_canvas_hasPointerMotionEvents(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* Desktop emulators always support pointer motion events (mouse drag) */
    return NATIVE_RETURN_INT(1);  /* true */
}

/* Canvas.hasRepeatEvents() - check if device supports key repeat */
static JavaValue native_canvas_hasRepeatEvents(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* Desktop emulators support key repeat events */
    return NATIVE_RETURN_INT(1);  /* true */
}

/* Canvas.isDoubleBuffered() - check if Canvas is double buffered */
static JavaValue native_canvas_isDoubleBuffered(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    /* Our implementation is always double buffered via SDL */
    return NATIVE_RETURN_INT(1);  /* true */
}

static JavaValue native_display_getHeight(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    int width, height;
    midp_display_get_dimensions(&width, &height);
    return NATIVE_RETURN_INT(height);
}

/* Canvas.getHeight() - returns available drawing height (minus soft buttons when not fullscreen) */
static JavaValue native_canvas_getHeight(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    int width, height;
    midp_display_get_dimensions(&width, &height);
    
    /* If not in fullscreen mode and soft buttons are visible, reduce height */
    if (!g_full_screen_mode && current_displayable_obj) {
        /* Check if current displayable has commands (soft buttons would be shown) */
        int commands_idx = find_field_index(current_displayable_obj, "commands");
        if (commands_idx >= 0) {
            JavaArray* commands = (JavaArray*)current_displayable_obj->fields[commands_idx].ref;
            if (commands && commands->length > 0) {
                /* Soft buttons would be shown, reduce canvas height */
                height -= SOFT_BUTTON_HEIGHT;
            }
        }
    }
    
    return NATIVE_RETURN_INT(height);
}

/* Displayable.isShown() - returns true if this displayable is currently visible */
static JavaValue native_displayable_isShown(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* displayable = (JavaObject*)args[0].ref;
    
    if (!displayable) {
        return NATIVE_RETURN_INT(0);  /* false */
    }
    
    /* Check if this is the current displayable */
    if (displayable == current_displayable_obj) {
        return NATIVE_RETURN_INT(1);  /* true */
    }
    
    return NATIVE_RETURN_INT(0);  /* false */
}

/* Canvas.setFullScreenMode(boolean) - enable/disable fullscreen mode */
static JavaValue native_canvas_setFullScreenMode(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* canvas = (JavaObject*)args[0].ref;
    jboolean mode = args[1].i;
    
    /* Store the mode globally */
    bool old_mode = g_full_screen_mode;
    g_full_screen_mode = mode ? true : false;
    
    /* If mode changed, we need to trigger a sizeChanged event and repaint */
    if (old_mode != g_full_screen_mode && canvas && current_displayable_obj == canvas) {
        /* Call sizeChanged with new dimensions */
        JavaClass* clazz = canvas->header.clazz;
        if (clazz) {
            JavaMethod* method = jvm_resolve_method(jvm, clazz, "sizeChanged", "(II)V");
            if (method) {
                int width, height;
                midp_display_get_dimensions(&width, &height);
                
                /* Adjust height based on fullscreen mode */
                if (!g_full_screen_mode) {
                    height -= SOFT_BUTTON_HEIGHT;
                }
                
                JavaValue size_args[3];
                size_args[0].ref = canvas;
                size_args[1].i = width;
                size_args[2].i = height;
                
                JavaValue result;
                execute_method(jvm, thread, method, size_args, &result);
            }
        }
        
        /* Trigger repaint */
        JavaMethod* repaint_method = jvm_resolve_method(jvm, clazz, "repaint", "()V");
        if (repaint_method) {
            JavaValue repaint_args[1];
            repaint_args[0].ref = canvas;
            JavaValue result;
            execute_method(jvm, thread, repaint_method, repaint_args, &result);
        }
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_display_isColor(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_INT(midp_display_is_color() ? 1 : 0);
}

static JavaValue native_display_numColors(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_INT(midp_display_num_colors());
}

static JavaValue native_display_numAlphaLevels(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_INT(midp_display_num_alpha_levels());
}

static JavaValue native_display_vibrate(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint duration = args[0].i;
    
    if (platform_callbacks.vibrate) {
        platform_callbacks.vibrate(duration);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_display_flashBacklight(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint duration = args[0].i;
    
    if (platform_callbacks.flash_backlight) {
        platform_callbacks.flash_backlight(duration);
    }
    
    return NATIVE_RETURN_VOID();
}

/* Display.getColor(int colorSpecifier) - returns default display colors */
static JavaValue native_display_getColor(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint specifier = args[0].i;
    switch (specifier) {
        case 0: return NATIVE_RETURN_INT(0);          /* COLOR_BACKGROUND */
        case 1: return NATIVE_RETURN_INT(0xFFFFFF);   /* COLOR_FOREGROUND */
        case 2: return NATIVE_RETURN_INT(0xFFFFFF);   /* COLOR_HIGHLIGHTED_BACKGROUND */
        case 3: return NATIVE_RETURN_INT(0);          /* COLOR_HIGHLIGHTED_FOREGROUND */
        case 4: return NATIVE_RETURN_INT(0x808080);   /* COLOR_BORDER */
        case 5: return NATIVE_RETURN_INT(0xFFFFFF);   /* COLOR_HIGHLIGHTED_BORDER */
        default: return NATIVE_RETURN_INT(0);
    }
}

/* Display.getBestImageWidth(int imageType) */
static JavaValue native_display_getBestImageWidth(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count; (void)args;
    int w, h;
    midp_display_get_dimensions(&w, &h);
    return NATIVE_RETURN_INT(w > 0 ? w : 240);
}

/* Display.getBestImageHeight(int imageType) */
static JavaValue native_display_getBestImageHeight(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count; (void)args;
    return NATIVE_RETURN_INT(20);  /* FreeJ2ME: returns 20 */
}

/* Display.getBorderStyle(boolean highlighted) */
static JavaValue native_display_getBorderStyle(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count; (void)args;
    /* FreeJ2ME: always returns SIMPLE = 0 */
    return NATIVE_RETURN_INT(0);
}

/* Canvas.repaint() - schedule a repaint by calling paint() immediately */
/* Canvas.repaint() - schedule a repaint for later (DO NOT call paint() immediately!) */
static JavaValue native_canvas_repaint(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* canvas = (JavaObject*)args[0].ref;
    
    DISP_DEBUG("[REPAINT] native_canvas_repaint() called! canvas=%p", (void*)canvas);
    
    
    /* In MIDP, repaint() is ASYNCHRONOUS - it schedules paint() for later.
     * The game expects paint2Buffer() to be called BEFORE paint().
     * 
     * WRONG: calling paint() immediately breaks this sequence:
     *   1. createNewLevel() - fills buffer with tiles
     *   2. repaint() - SHOULD NOT call paint() yet!
     *   3. Next run() iteration: paint2Buffer() - copies buffer to mFullScreenBuffer
     *   4. repaint() - now paint() can draw the filled mFullScreenBuffer
     *
     * We store the canvas and request a redraw. The main loop will call paint()
     * via midp_process_repaints() after timers have run.
     */
    
    if (!canvas) return NATIVE_RETURN_VOID();
    
    /* Store canvas for later painting */
    current_displayable_obj = canvas;  /* Update current displayable */
    
    /* Mark that repaint was requested - midp_process_repaints() will only
     * call paint() when this flag (or g_paint_pending) is set */
    g_canvas_repaint_requested = true;
    
    fprintf(stderr, "[REPAINT] repaint() called, canvas=%p class=%s\n", 
            (void*)canvas, canvas->header.clazz && canvas->header.clazz->class_name ? canvas->header.clazz->class_name : "?");
    
    /* Request redraw - this will trigger paint() via midp_process_repaints() */
    sdl_request_redraw();
    
    DISP_DEBUG("[REPAINT] native_canvas_repaint() done, sdl_request_redraw() called");
    
    
    return NATIVE_RETURN_VOID();
}

/* Canvas.repaint(int x, int y, int w, int h) - repaint a region */
static JavaValue native_canvas_repaint_region(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)arg_count;
    /* For simplicity, just do a full repaint */
    return native_canvas_repaint(jvm, thread, args, 1);
}

/* Canvas.serviceRepaints() - process any pending repaints immediately.
 *
 * Many J2ME games (including SU-30) use a two-phase rendering pattern:
 *   Phase 1 (inside paint()): bindTarget(Graphics) + clear(Background)
 *   Phase 2 (after serviceRepaints returns): render(World) + releaseTarget()
 *
 * If serviceRepaints() returns without calling paint(), Phase 2 runs before
 * Phase 1 — render/releaseTarget execute before bindTarget was ever called.
 *
 * In libretro mode, this runs inside execute_frame() within the JVM instruction
 * loop. We call midp_process_repaints() synchronously here so paint() executes
 * BEFORE serviceRepaints() returns to Java code. The main loop's later call to
 * midp_process_repaints() becomes a no-op (flags already cleared).
 */
static JavaValue native_canvas_serviceRepaints(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;
    
    g_canvas_repaint_requested = true;
    g_paint_pending = true;
    g_paint_complete = false;
    
    /* Process repaints synchronously so paint() runs before serviceRepaints returns.
     * This is critical for games that split M3G rendering across paint() and a
     * post-serviceRepaints callback (bindTarget in paint, render after). */
    midp_process_repaints(jvm);
    
    return NATIVE_RETURN_VOID();
}

/* Проверка есть ли отложенный repaint - вызывается из главного цикла */
bool midp_has_pending_repaint(void) {
    return g_paint_pending;
}

/* Сброс флага отложенного repaint */
void midp_clear_pending_repaint(void) {
    g_paint_pending = false;
    g_paint_complete = true;
}

/* Process pending repaints - called from main loop after timers */
void midp_process_repaints(JVM* jvm) {
    static int repaint_count = 0;
    
    repaint_count++;
    
    if (!current_displayable_obj) {
        return;
    }
    
    JavaObject* canvas = current_displayable_obj;
    JavaClass* clazz = canvas->header.clazz;
    if (!clazz) return;
    
    /* ИСПРАВЛЕНО: Определяем тип displayable для выбора метода рендеринга.
     * - Canvas/GameCanvas: вызываем paint()
     * - List/Form/TextBox/Alert: вызываем соответствующие render функции
     */
    bool is_game_canvas = false;
    bool is_canvas = false;
    bool is_list = false;
    bool is_form = false;
    bool is_textbox = false;
    bool is_alert = false;
    
    JavaClass* check = clazz;
    while (check) {
        if (check->class_name) {
            if (strcmp(check->class_name, "javax/microedition/lcdui/game/GameCanvas") == 0) {
                is_game_canvas = true;
                break;
            }
            if (strcmp(check->class_name, "javax/microedition/lcdui/Canvas") == 0) {
                is_canvas = true;
                /* Не break - может быть GameCanvas */
            }
            if (strcmp(check->class_name, "javax/microedition/lcdui/List") == 0) {
                is_list = true;
                break;
            }
            if (strcmp(check->class_name, "javax/microedition/lcdui/Form") == 0) {
                is_form = true;
                break;
            }
            if (strcmp(check->class_name, "javax/microedition/lcdui/TextBox") == 0) {
                is_textbox = true;
                break;
            }
            if (strcmp(check->class_name, "javax/microedition/lcdui/Alert") == 0) {
                is_alert = true;
                break;
            }
        }
        check = check->super_class;
    }
    

    
    /* ИСПРАВЛЕНО: Обрабатываем высокоуровневые UI компоненты (List, Form, TextBox, Alert) */
    if (is_list) {
#ifdef J2ME_HEADLESS
        extern void headless_reset_text_capture(void);
        extern void headless_print_captured_text(void);
        headless_reset_text_capture();
#endif
        midp_render_list(jvm, canvas, 0);
        render_soft_buttons(jvm, canvas);
#ifdef J2ME_HEADLESS
        headless_print_captured_text();
#endif
        midp_clear_pending_repaint();
        return;
    }
    
    if (is_form) {
#ifdef J2ME_HEADLESS
        extern void headless_reset_text_capture(void);
        extern void headless_print_captured_text(void);
        headless_reset_text_capture();
#endif
        midp_render_form(jvm, canvas);
        render_soft_buttons(jvm, canvas);
#ifdef J2ME_HEADLESS
        headless_print_captured_text();
#endif
        midp_clear_pending_repaint();
        return;
    }
    
    if (is_textbox) {
#ifdef J2ME_HEADLESS
        extern void headless_reset_text_capture(void);
        extern void headless_print_captured_text(void);
        headless_reset_text_capture();
#endif
        midp_render_textbox(jvm, canvas, "", 0);
        render_soft_buttons(jvm, canvas);
#ifdef J2ME_HEADLESS
        headless_print_captured_text();
#endif
        midp_clear_pending_repaint();
        return;
    }
    
    if (is_alert) {
        JavaObject* gfx_obj = create_graphics_object(jvm);
        if (gfx_obj) {
            MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
            if (gfx) {
#ifdef J2ME_HEADLESS
                extern void headless_reset_text_capture(void);
                extern void headless_print_captured_text(void);
                headless_reset_text_capture();
#endif
                midp_render_alert(jvm, gfx, canvas);
#ifdef J2ME_HEADLESS
                headless_print_captured_text();
#endif
            }
        }
        render_soft_buttons(jvm, canvas);
        sdl_request_redraw();
        midp_clear_pending_repaint();
        return;
    }
    
    /* GameCanvas extends Canvas, so repaint()/paint() must work.
     * Per J2ME spec, calling repaint() on a GameCanvas still triggers paint().
     * flushGraphics() is an additional mechanism, not a replacement.
     * Previously we skipped paint() for GameCanvas assuming games only use
     * flushGraphics(), but many games use the standard Canvas repaint pattern.
     * Fall through to the regular Canvas paint() handling below. */
    
    /* IMPORTANT: Only call paint() if repaint() or serviceRepaints() was called.
     * midp_process_repaints() is invoked every frame by retro_run(). Calling
     * paint() unconditionally would catch the offscreen buffer mid-update by
     * the MIDlet's game thread, causing visible flickering. */
    if (!g_canvas_repaint_requested && !g_paint_pending) {
        return;
    }
    g_canvas_repaint_requested = false;  /* Consume the request */
    
    fprintf(stderr, "[PAINT] Calling paint() on %s\n", 
            clazz && clazz->class_name ? clazz->class_name : "?");
    
    /* Find and call paint(Graphics g) method */
    JavaMethod* paint_method = jvm_resolve_method(jvm, clazz, "paint", "(Ljavax/microedition/lcdui/Graphics;)V");
    if (!paint_method) {
        /* No paint() method found */
        midp_clear_pending_repaint();  /* Still signal completion */
        return;
    }
    
    /* Create Graphics object bound to screen */
    JavaObject* gfx_obj = create_graphics_object(jvm);
    if (!gfx_obj) {
        fprintf(stderr, "[REPAINT] Failed to create Graphics object!\n");
        midp_clear_pending_repaint();  /* Still signal completion */
        return;
    }
    
    /* Call paint() */
    JavaValue paint_args[2];
    paint_args[0].ref = canvas;
    paint_args[1].ref = gfx_obj;
    
    /* Reset M3G force-render tracking before paint() */
    m3g_reset_paint_tracking();
    
#ifdef J2ME_HEADLESS
    /* Reset text capture before paint to only capture new frame's text */
    extern void headless_reset_text_capture(void);
    headless_reset_text_capture();
#endif
    
    JavaValue result;
    JavaThread* th = jvm_current_thread(jvm);
    int exec_result = execute_method(jvm, th, paint_method, paint_args, &result);
    
#ifdef J2ME_HEADLESS
    /* Print captured text after paint completes */
    extern void headless_print_captured_text(void);
    headless_print_captured_text();
#endif
    
    
    /* Render soft buttons after paint() */
    render_soft_buttons(jvm, canvas);
    
    /* M3G force-render: Some games (like SU-30) do M3G scene setup 
     * (setCamera, setBackground, etc.) but never call bindTarget/render.
     * After paint() completes, if scene was set up but not rendered,
     * force a render cycle. */
    {
        extern bool m3g_needs_force_render(void);
        extern void m3g_force_render(JVM* jvm, MidpGraphics* screen_gfx);
        if (m3g_needs_force_render()) {
            MidpGraphics* screen_gfx = get_graphics_from_object(gfx_obj);
            if (screen_gfx) {
                m3g_force_render(jvm, screen_gfx);
            }
        }
    }
    
    /* ИСПРАВЛЕНО: Сигнализируем, что paint() завершён.
     * Это разблокирует serviceRepaints() в Java-потоке.
     * БЕЗ ЭТОГО serviceRepaints() ждёт 100ms таймаут, а потом
     * Java-код продолжает и сбрасывает t.b = false ДО того,
     * как paint() будет вызван!
     */
    midp_clear_pending_repaint();
    
    /* repaint done */
    
}

/*
 * ============================================
 * Helper functions for safe field access
 * ИСПРАВЛЕНО: используем native_get/set_field_value для корректного расчета слотов
 * ============================================
 */

/* Helper: Get field value from object by name */
static jint get_object_field_int(JavaObject* obj, const char* field_name) {
    return native_get_field_value(obj, field_name).i;
}

/* Helper: Set field value in object by name */
static void set_object_field_int(JavaObject* obj, const char* field_name, jint value) {
    JavaValue val = { .i = value };
    native_set_field_value(obj, field_name, val);
}

/* Helper: Get object field reference by name */
JavaObject* get_object_field_ref(JavaObject* obj, const char* field_name) {
    return (JavaObject*)native_get_field_value(obj, field_name).ref;
}

/* Helper: Set object field reference by name */
void set_object_field_ref(JavaObject* obj, const char* field_name, JavaObject* value) {
    JavaValue val = { .ref = value };
    native_set_field_value(obj, field_name, val);
}

/*
 * ============================================
 * Sprite native methods
 * ============================================
 */

/* Sprite.<init>(Image image) */
static JavaValue native_sprite_init_image(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaObject* image_obj = (JavaObject*)args[1].ref;

    if (!this_obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }

    /* COMPATIBILITY: Accept null Image (KEmulator behavior).
     * Some midlets pass null when resource loading fails — throwing NPE
     * here prevents the entire game from reaching 3D rendering. */
    if (!image_obj) {
        set_object_field_ref(this_obj, "image", NULL);
        set_object_field_int(this_obj, "width", 0);
        set_object_field_int(this_obj, "height", 0);
        set_object_field_int(this_obj, "frameWidth", 0);
        set_object_field_int(this_obj, "frameHeight", 0);
        set_object_field_int(this_obj, "rawFrameCount", 0);
        set_object_field_int(this_obj, "currentFrame", 0);
        set_object_field_int(this_obj, "collisionX", 0);
        set_object_field_int(this_obj, "collisionY", 0);
        set_object_field_int(this_obj, "collisionWidth", 0);
        set_object_field_int(this_obj, "collisionHeight", 0);
        set_object_field_int(this_obj, "transform", 0);
        return NATIVE_RETURN_VOID();
    }

    DISP_DEBUG("[Sprite] init(Image): this=%p, image=%p",
            (void*)this_obj, (void*)image_obj);

    /* ИСПРАВЛЕНО: используем helper function */
    set_object_field_ref(this_obj, "image", image_obj);

    /* Init width/height and frame dimensions from image */
    MidpImage* img = get_image_from_object(image_obj);
    if (img) {
        set_object_field_int(this_obj, "width", img->width);
        set_object_field_int(this_obj, "height", img->height);
        /* Single frame = the whole image */
        set_object_field_int(this_obj, "frameWidth", img->width);
        set_object_field_int(this_obj, "frameHeight", img->height);
        set_object_field_int(this_obj, "rawFrameCount", 1);
        set_object_field_int(this_obj, "currentFrame", 0);
        /* Initialize collision rectangle to full sprite bounds */
        set_object_field_int(this_obj, "collisionX", 0);
        set_object_field_int(this_obj, "collisionY", 0);
        set_object_field_int(this_obj, "collisionWidth", img->width);
        set_object_field_int(this_obj, "collisionHeight", img->height);
    }

    return NATIVE_RETURN_VOID();
}

/* Sprite.<init>(Image image, int frameWidth, int frameHeight) */
static JavaValue native_sprite_init_frames(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaObject* image_obj = (JavaObject*)args[1].ref;
    jint frame_width = args[2].i;
    jint frame_height = args[3].i;

    if (!this_obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }

    /* COMPATIBILITY: Accept null Image */
    if (!image_obj) {
        set_object_field_ref(this_obj, "image", NULL);
        set_object_field_int(this_obj, "frameWidth", frame_width);
        set_object_field_int(this_obj, "frameHeight", frame_height);
        set_object_field_int(this_obj, "width", frame_width);
        set_object_field_int(this_obj, "height", frame_height);
        set_object_field_int(this_obj, "rawFrameCount", 0);
        set_object_field_int(this_obj, "currentFrame", 0);
        set_object_field_int(this_obj, "collisionX", 0);
        set_object_field_int(this_obj, "collisionY", 0);
        set_object_field_int(this_obj, "collisionWidth", frame_width);
        set_object_field_int(this_obj, "collisionHeight", frame_height);
        set_object_field_int(this_obj, "transform", 0);
        return NATIVE_RETURN_VOID();
    }

    DISP_DEBUG("[Sprite] init(Image, %d, %d): this=%p, image=%p",
            frame_width, frame_height, (void*)this_obj, (void*)image_obj);

    /* ИСПРАВЛЕНО: используем helper functions */
    set_object_field_ref(this_obj, "image", image_obj);
    set_object_field_int(this_obj, "frameWidth", frame_width);
    set_object_field_int(this_obj, "frameHeight", frame_height);

    /* MIDP2 spec: Sprite(Image, frameWidth, frameHeight) - 
     * width = frameWidth, height = frameHeight */
    set_object_field_int(this_obj, "width", frame_width);
    set_object_field_int(this_obj, "height", frame_height);

    /* Calculate raw frame count from full image dimensions */
    MidpImage* img = get_image_from_object(image_obj);
    if (img) {
        if (frame_width > 0 && frame_height > 0 &&
            img->width >= frame_width && img->height >= frame_height) {
            int cols = img->width / frame_width;
            int rows = img->height / frame_height;
            set_object_field_int(this_obj, "rawFrameCount", cols * rows);
        }
    }

    set_object_field_int(this_obj, "currentFrame", 0);
    /* Initialize collision rectangle to full frame bounds */
    set_object_field_int(this_obj, "collisionX", 0);
    set_object_field_int(this_obj, "collisionY", 0);
    set_object_field_int(this_obj, "collisionWidth", frame_width);
    set_object_field_int(this_obj, "collisionHeight", frame_height);

    return NATIVE_RETURN_VOID();
}

/* Sprite.<init>(Sprite sprite) - copy constructor */
static JavaValue native_sprite_init_copy(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaObject* other_sprite = (JavaObject*)args[1].ref;
    
    if (!this_obj || !other_sprite) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    DISP_DEBUG("[Sprite] init(copy): this=%p, other=%p", 
            (void*)this_obj, (void*)other_sprite);
    
    /* ИСПРАВЛЕНО: Copy fields using helper functions */
    set_object_field_ref(this_obj, "image", get_object_field_ref(other_sprite, "image"));
    set_object_field_int(this_obj, "x", get_object_field_int(other_sprite, "x"));
    set_object_field_int(this_obj, "y", get_object_field_int(other_sprite, "y"));
    set_object_field_int(this_obj, "width", get_object_field_int(other_sprite, "width"));
    set_object_field_int(this_obj, "height", get_object_field_int(other_sprite, "height"));
    set_object_field_int(this_obj, "frameWidth", get_object_field_int(other_sprite, "frameWidth"));
    set_object_field_int(this_obj, "frameHeight", get_object_field_int(other_sprite, "frameHeight"));
    set_object_field_int(this_obj, "currentFrame", get_object_field_int(other_sprite, "currentFrame"));
    set_object_field_int(this_obj, "visible", get_object_field_int(other_sprite, "visible"));
    set_object_field_int(this_obj, "transform", get_object_field_int(other_sprite, "transform"));
    set_object_field_int(this_obj, "rawFrameCount", get_object_field_int(other_sprite, "rawFrameCount"));
    set_object_field_ref(this_obj, "frameSequence", get_object_field_ref(other_sprite, "frameSequence"));
    set_object_field_int(this_obj, "refX", get_object_field_int(other_sprite, "refX"));
    set_object_field_int(this_obj, "refY", get_object_field_int(other_sprite, "refY"));
    set_object_field_int(this_obj, "collisionX", get_object_field_int(other_sprite, "collisionX"));
    set_object_field_int(this_obj, "collisionY", get_object_field_int(other_sprite, "collisionY"));
    set_object_field_int(this_obj, "collisionWidth", get_object_field_int(other_sprite, "collisionWidth"));
    set_object_field_int(this_obj, "collisionHeight", get_object_field_int(other_sprite, "collisionHeight"));
    
    return NATIVE_RETURN_VOID();
}

/* Sprite.setPosition(int x, int y) */
static JavaValue native_sprite_setPosition(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: используем helper functions */
    set_object_field_int(this_obj, "x", x);
    set_object_field_int(this_obj, "y", y);
    
    return NATIVE_RETURN_VOID();
}

/* Sprite.move(int dx, int dy) */
static JavaValue native_sprite_move(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint dx = args[1].i;
    jint dy = args[2].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: используем helper functions */
    jint cur_x = get_object_field_int(this_obj, "x");
    jint cur_y = get_object_field_int(this_obj, "y");
    set_object_field_int(this_obj, "x", cur_x + dx);
    set_object_field_int(this_obj, "y", cur_y + dy);
    
    return NATIVE_RETURN_VOID();
}

/* Sprite.paint(Graphics g) */
static JavaValue native_sprite_paint(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaObject* gfx_obj = (JavaObject*)args[1].ref;
    
    if (!this_obj || !gfx_obj) return NATIVE_RETURN_VOID();
    
    /* Get MidpGraphics from Java Graphics object */
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    if (!gfx) {
        DISP_DEBUG("[Sprite] paint: failed to get MidpGraphics");
        return NATIVE_RETURN_VOID();
    }
    
    /* Get sprite properties */
    jint x = get_object_field_int(this_obj, "x");
    jint y = get_object_field_int(this_obj, "y");
    jint width = get_object_field_int(this_obj, "width");
    jint height = get_object_field_int(this_obj, "height");
    jint frameWidth = get_object_field_int(this_obj, "frameWidth");
    jint frameHeight = get_object_field_int(this_obj, "frameHeight");
    jint currentFrame = get_object_field_int(this_obj, "currentFrame");
    jint visible = get_object_field_int(this_obj, "visible");
    jint transform = get_object_field_int(this_obj, "transform");
    jint refX = get_object_field_int(this_obj, "refX");
    jint refY = get_object_field_int(this_obj, "refY");
    
    /* Skip if not visible */
    if (!visible) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Get image from sprite */
    JavaObject* image_obj = get_object_field_ref(this_obj, "image");
    if (!image_obj) {
        DISP_DEBUG("[Sprite] paint: no image");
        return NATIVE_RETURN_VOID();
    }
    
    /* Get MidpImage from Image object */
    MidpImage* img = (MidpImage*)get_object_field_ref(image_obj, "nativePeer");
    
    if (!img || !img->pixels) {
        DISP_DEBUG("[Sprite] paint: no image data");
        return NATIVE_RETURN_VOID();
    }
    
    /* Calculate frame offset if using frame animation */
    jint src_x = 0, src_y = 0;
    jint src_w = frameWidth > 0 ? frameWidth : img->width;
    jint src_h = frameHeight > 0 ? frameHeight : img->height;

    /* Resolve actual frame index via frameSequence */
    jint actual_frame = currentFrame;
    JavaArray* frame_seq = (JavaArray*)get_object_field_ref(this_obj, "frameSequence");
    if (frame_seq && frame_seq->length > 0) {
        jint* seq_data = (jint*)array_data(frame_seq);
        if (currentFrame >= 0 && currentFrame < (jint)frame_seq->length) {
            actual_frame = seq_data[currentFrame];
        }
    }

    if (frameWidth > 0 && frameHeight > 0) {
        int frames_per_row = img->width / frameWidth;
        if (frames_per_row > 0) {
            src_x = (actual_frame % frames_per_row) * frameWidth;
            src_y = (actual_frame / frames_per_row) * frameHeight;
        }
    }

    /* Transform reference pixel coordinates using current layer dimensions
     * (post-transform width/height), matching J2ME-Loader's getTransformedPtX/Y.
     * width/height are the Sprite's current Layer dimensions which change with transform.
     */
    jint transformed_refX, transformed_refY;
    jint w = width;   /* current (post-transform) width */
    jint h = height;  /* current (post-transform) height */
    switch (transform) {
        case 0: /* TRANS_NONE */
            transformed_refX = refX;
            transformed_refY = refY;
            break;
        case 1: /* TRANS_MIRROR_ROT180 - vertical flip */
            transformed_refX = refX;
            transformed_refY = h - 1 - refY;
            break;
        case 2: /* TRANS_MIRROR - horizontal flip */
            transformed_refX = w - 1 - refX;
            transformed_refY = refY;
            break;
        case 3: /* TRANS_ROT180 */
            transformed_refX = w - 1 - refX;
            transformed_refY = h - 1 - refY;
            break;
        case 4: /* TRANS_MIRROR_ROT270 */
            transformed_refX = refY;
            transformed_refY = refX;
            break;
        case 5: /* TRANS_ROT90 */
            transformed_refX = h - 1 - refY;
            transformed_refY = refX;
            break;
        case 6: /* TRANS_ROT270 */
            transformed_refX = refY;
            transformed_refY = w - 1 - refX;
            break;
        case 7: /* TRANS_MIRROR_ROT90 */
            transformed_refX = h - 1 - refY;
            transformed_refY = w - 1 - refX;
            break;
        default:
            transformed_refX = refX;
            transformed_refY = refY;
            break;
    }

    /* Apply transformed reference pixel offset */
    jint dest_x = x - transformed_refX;
    jint dest_y = y - transformed_refY;

    /* Draw the sprite region to the graphics context */
    midp_graphics_draw_region(gfx, img, src_x, src_y, src_w, src_h,
                               transform, dest_x, dest_y, 0);
    
    return NATIVE_RETURN_VOID();
}

/* Sprite.getX() */
static JavaValue native_sprite_getX(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    /* ИСПРАВЛЕНО: используем helper function */
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "x"));
}

/* Sprite.getY() */
static JavaValue native_sprite_getY(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    /* ИСПРАВЛЕНО: используем helper function */
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "y"));
}

/* Sprite.getWidth() */
static JavaValue native_sprite_getWidth(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    /* ИСПРАВЛЕНО: используем helper function */
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "width"));
}

/* Sprite.getHeight() */
static JavaValue native_sprite_getHeight(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    /* ИСПРАВЛЕНО: используем helper function */
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "height"));
}

/* Sprite.setVisible(boolean visible) */
static JavaValue native_sprite_setVisible(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jboolean visible = args[1].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: используем helper function */
    set_object_field_int(this_obj, "visible", visible);
    
    return NATIVE_RETURN_VOID();
}

/* Sprite.isVisible() */
static JavaValue native_sprite_isVisible(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(1);  /* Default visible */
    
    /* ИСПРАВЛЕНО: используем helper function */
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "visible"));
}

/* Sprite.setFrame(int frame) */
static JavaValue native_sprite_setFrame(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint frame = args[1].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    /* ИСПРАВЛЕНО: используем helper function */
    set_object_field_int(this_obj, "currentFrame", frame);
    
    return NATIVE_RETURN_VOID();
}

/* Sprite.getFrame() */
static JavaValue native_sprite_getFrame(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "currentFrame"));
}

/* Sprite.setTransform(int transform) - rotate/mirror sprite */
static JavaValue native_sprite_setTransform(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint transform = args[1].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    jint old_transform = get_object_field_int(this_obj, "transform");
    if (old_transform == transform) return NATIVE_RETURN_VOID();
    
    jint refX = get_object_field_int(this_obj, "refX");
    jint refY = get_object_field_int(this_obj, "refY");
    jint cur_w = get_object_field_int(this_obj, "width");
    jint cur_h = get_object_field_int(this_obj, "height");
    
    /* Compute reference pixel offset under OLD transform using OLD dimensions */
    jint old_refPtX, old_refPtY;
    switch (old_transform) {
        case 0: old_refPtX = refX; old_refPtY = refY; break;
        case 1: old_refPtX = refX; old_refPtY = cur_h - 1 - refY; break;
        case 2: old_refPtX = cur_w - 1 - refX; old_refPtY = refY; break;
        case 3: old_refPtX = cur_w - 1 - refX; old_refPtY = cur_h - 1 - refY; break;
        case 4: old_refPtX = refY; old_refPtY = refX; break;
        case 5: old_refPtX = cur_h - 1 - refY; old_refPtY = refX; break;
        case 6: old_refPtX = refY; old_refPtY = cur_w - 1 - refX; break;
        case 7: old_refPtX = cur_h - 1 - refY; old_refPtY = cur_w - 1 - refX; break;
        default: old_refPtX = refX; old_refPtY = refY; break;
    }
    
    /* Check if old transform had swapped dimensions and restore original */
    bool old_swapped = (old_transform == SPRITE_TRANS_ROT90 ||
                        old_transform == SPRITE_TRANS_ROT270 ||
                        old_transform == SPRITE_TRANS_MIRROR_ROT90 ||
                        old_transform == SPRITE_TRANS_MIRROR_ROT270);
    if (old_swapped) {
        jint tmp = cur_w; cur_w = cur_h; cur_h = tmp;
    }
    
    /* Check if new transform swaps dimensions */
    bool new_swapped = (transform == SPRITE_TRANS_ROT90 ||
                        transform == SPRITE_TRANS_ROT270 ||
                        transform == SPRITE_TRANS_MIRROR_ROT90 ||
                        transform == SPRITE_TRANS_MIRROR_ROT270);
    
    /* Compute new dimensions */
    jint new_w = new_swapped ? cur_h : cur_w;
    jint new_h = new_swapped ? cur_w : cur_h;
    
    /* Update transform and dimensions */
    set_object_field_int(this_obj, "transform", transform);
    set_object_field_int(this_obj, "width", new_w);
    set_object_field_int(this_obj, "height", new_h);
    
    /* Compute reference pixel offset under NEW transform using NEW dimensions */
    jint new_refPtX, new_refPtY;
    switch (transform) {
        case 0: new_refPtX = refX; new_refPtY = refY; break;
        case 1: new_refPtX = refX; new_refPtY = new_h - 1 - refY; break;
        case 2: new_refPtX = new_w - 1 - refX; new_refPtY = refY; break;
        case 3: new_refPtX = new_w - 1 - refX; new_refPtY = new_h - 1 - refY; break;
        case 4: new_refPtX = refY; new_refPtY = refX; break;
        case 5: new_refPtX = new_h - 1 - refY; new_refPtY = refX; break;
        case 6: new_refPtX = refY; new_refPtY = new_w - 1 - refX; break;
        case 7: new_refPtX = new_h - 1 - refY; new_refPtY = new_w - 1 - refX; break;
        default: new_refPtX = refX; new_refPtY = refY; break;
    }
    
    /* Adjust position to keep reference pixel at same screen location.
     * screen_ref = x + old_refPt (top-left + offset in drawn image)
     * new_x = screen_ref - new_refPt = x + old_refPt - new_refPt
     */
    jint cur_x = get_object_field_int(this_obj, "x");
    jint cur_y = get_object_field_int(this_obj, "y");
    set_object_field_int(this_obj, "x", cur_x + old_refPtX - new_refPtX);
    set_object_field_int(this_obj, "y", cur_y + old_refPtY - new_refPtY);
    
    return NATIVE_RETURN_VOID();
}

/* Sprite.getTransform() */
static JavaValue native_sprite_getTransform(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "transform"));
}

/* Helper to get Sprite image */
static MidpImage* get_sprite_image(JavaObject* sprite_obj) {
    if (!sprite_obj) return NULL;
    
    JavaObject* image_obj = get_object_field_ref(sprite_obj, "image");
    if (image_obj) {
        return get_image_from_object(image_obj);
    }
    return NULL;
}

/* Sprite.collidesWith(Sprite s, boolean pixelLevel) */
static JavaValue native_sprite_collidesWith_sprite(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaObject* other_sprite = (JavaObject*)args[1].ref;
    jboolean pixel_level = args[2].i;
    
    if (!this_obj || !other_sprite) {
        return NATIVE_RETURN_INT(0); /* false */
    }
    
    /* Check visibility - invisible sprites don't collide */
    if (!get_object_field_int(this_obj, "visible") || !get_object_field_int(other_sprite, "visible")) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Use collision rectangles instead of full bounds */
    int this_x = get_object_field_int(this_obj, "x") + get_object_field_int(this_obj, "collisionX");
    int this_y = get_object_field_int(this_obj, "y") + get_object_field_int(this_obj, "collisionY");
    int this_w = get_object_field_int(this_obj, "collisionWidth");
    int this_h = get_object_field_int(this_obj, "collisionHeight");
    
    int other_x = get_object_field_int(other_sprite, "x") + get_object_field_int(other_sprite, "collisionX");
    int other_y = get_object_field_int(other_sprite, "y") + get_object_field_int(other_sprite, "collisionY");
    int other_w = get_object_field_int(other_sprite, "collisionWidth");
    int other_h = get_object_field_int(other_sprite, "collisionHeight");
    
    /* Rectangle intersection test */
    int intersect_x1 = this_x > other_x ? this_x : other_x;
    int intersect_y1 = this_y > other_y ? this_y : other_y;
    int intersect_x2 = (this_x + this_w) < (other_x + other_w) ? (this_x + this_w) : (other_x + other_w);
    int intersect_y2 = (this_y + this_h) < (other_y + other_h) ? (this_y + this_h) : (other_y + other_h);
    
    /* No intersection */
    if (intersect_x1 >= intersect_x2 || intersect_y1 >= intersect_y2) {
        return NATIVE_RETURN_INT(0); /* false */
    }
    
    /* If not pixel-level, bounding box collision is enough */
    if (!pixel_level) {
        return NATIVE_RETURN_INT(1); /* true */
    }
    
    /* Pixel-level collision detection */
    MidpImage* this_img = get_sprite_image(this_obj);
    MidpImage* other_img = get_sprite_image(other_sprite);
    
    if (!this_img || !other_img) {
        return NATIVE_RETURN_INT(0); /* false - no images */
    }
    
    /* Get frame info for this sprite */
    jint this_frameWidth = get_object_field_int(this_obj, "frameWidth");
    jint this_frameHeight = get_object_field_int(this_obj, "frameHeight");
    jint this_transform = get_object_field_int(this_obj, "transform");
    jint this_currentFrame = get_object_field_int(this_obj, "currentFrame");
    JavaArray* this_seq = (JavaArray*)get_object_field_ref(this_obj, "frameSequence");
    jint this_actual_frame = this_currentFrame;
    if (this_seq && this_seq->length > 0 && this_currentFrame >= 0 && this_currentFrame < (jint)this_seq->length) {
        this_actual_frame = ((jint*)array_data(this_seq))[this_currentFrame];
    }
    
    /* Get frame info for other sprite */
    jint other_frameWidth = get_object_field_int(other_sprite, "frameWidth");
    jint other_frameHeight = get_object_field_int(other_sprite, "frameHeight");
    jint other_transform = get_object_field_int(other_sprite, "transform");
    jint other_currentFrame = get_object_field_int(other_sprite, "currentFrame");
    JavaArray* other_seq = (JavaArray*)get_object_field_ref(other_sprite, "frameSequence");
    jint other_actual_frame = other_currentFrame;
    if (other_seq && other_seq->length > 0 && other_currentFrame >= 0 && other_currentFrame < (jint)other_seq->length) {
        other_actual_frame = ((jint*)array_data(other_seq))[other_currentFrame];
    }
    
    /* Calculate source region for this sprite's current frame */
    int this_src_x = 0, this_src_y = 0;
    if (this_frameWidth > 0 && this_frameHeight > 0) {
        int this_fpr = this_img->width / this_frameWidth;
        if (this_fpr > 0) {
            this_src_x = (this_actual_frame % this_fpr) * this_frameWidth;
            this_src_y = (this_actual_frame / this_fpr) * this_frameHeight;
        }
    }
    
    /* Calculate source region for other sprite's current frame */
    int other_src_x = 0, other_src_y = 0;
    if (other_frameWidth > 0 && other_frameHeight > 0) {
        int other_fpr = other_img->width / other_frameWidth;
        if (other_fpr > 0) {
            other_src_x = (other_actual_frame % other_fpr) * other_frameWidth;
            other_src_y = (other_actual_frame / other_fpr) * other_frameHeight;
        }
    }
    
    int this_sx = get_object_field_int(this_obj, "x");
    int this_sy = get_object_field_int(this_obj, "y");
    int other_sx = get_object_field_int(other_sprite, "x");
    int other_sy = get_object_field_int(other_sprite, "y");
    
    /* Check each pixel in intersection region */
    for (int y = intersect_y1; y < intersect_y2; y++) {
        for (int x = intersect_x1; x < intersect_x2; x++) {
            /* Map screen coords to this sprite's source image pixel */
            int this_lx = x - this_sx;  /* local coord in sprite's drawn area */
            int this_ly = y - this_sy;
            
            /* Apply inverse transform to get source pixel */
            int this_fw = (this_transform == 5 || this_transform == 4 || this_transform == 6 || this_transform == 7)
                          ? this_frameHeight : this_frameWidth;
            int this_fh = (this_transform == 5 || this_transform == 4 || this_transform == 6 || this_transform == 7)
                          ? this_frameWidth : this_frameHeight;
            
            /* Simple approach: for untransformed sprites, direct mapping */
            int this_img_x = this_src_x + this_lx;
            int this_img_y = this_src_y + this_ly;
            
            if (this_transform != 0) {
                /* For transformed sprites, map through the transform */
                switch (this_transform) {
                    case 2: this_img_x = this_src_x + this_fw - 1 - this_lx; this_img_y = this_src_y + this_ly; break;
                    case 1: this_img_x = this_src_x + this_lx; this_img_y = this_src_y + this_fh - 1 - this_ly; break;
                    case 3: this_img_x = this_src_x + this_fw - 1 - this_lx; this_img_y = this_src_y + this_fh - 1 - this_ly; break;
                    case 5: this_img_x = this_src_x + this_ly; this_img_y = this_src_y + this_fh - 1 - this_lx; break;
                    case 4: this_img_x = this_src_x + this_ly; this_img_y = this_src_y + this_lx; break;
                    case 6: this_img_x = this_src_x + this_fw - 1 - this_ly; this_img_y = this_src_y + this_lx; break;
                    case 7: this_img_x = this_src_x + this_fw - 1 - this_ly; this_img_y = this_src_y + this_fh - 1 - this_lx; break;
                }
            }
            
            /* Map screen coords to other sprite's source image pixel */
            int other_lx = x - other_sx;
            int other_ly = y - other_sy;
            int other_fw = (other_transform == 5 || other_transform == 4 || other_transform == 6 || other_transform == 7)
                           ? other_frameHeight : other_frameWidth;
            int other_fh = (other_transform == 5 || other_transform == 4 || other_transform == 6 || other_transform == 7)
                           ? other_frameWidth : other_frameHeight;
            
            int other_img_x = other_src_x + other_lx;
            int other_img_y = other_src_y + other_ly;
            
            if (other_transform != 0) {
                switch (other_transform) {
                    case 2: other_img_x = other_src_x + other_fw - 1 - other_lx; other_img_y = other_src_y + other_ly; break;
                    case 1: other_img_x = other_src_x + other_lx; other_img_y = other_src_y + other_fh - 1 - other_ly; break;
                    case 3: other_img_x = other_src_x + other_fw - 1 - other_lx; other_img_y = other_src_y + other_fh - 1 - other_ly; break;
                    case 5: other_img_x = other_src_x + other_ly; other_img_y = other_src_y + other_fh - 1 - other_lx; break;
                    case 4: other_img_x = other_src_x + other_ly; other_img_y = other_src_y + other_lx; break;
                    case 6: other_img_x = other_src_x + other_fw - 1 - other_ly; other_img_y = other_src_y + other_lx; break;
                    case 7: other_img_x = other_src_x + other_fw - 1 - other_ly; other_img_y = other_src_y + other_fh - 1 - other_lx; break;
                }
            }
            
            /* Check bounds in source images */
            if (this_img_x < this_src_x || this_img_x >= this_src_x + this_frameWidth) continue;
            if (this_img_y < this_src_y || this_img_y >= this_src_y + this_frameHeight) continue;
            if (other_img_x < other_src_x || other_img_x >= other_src_x + other_frameWidth) continue;
            if (other_img_y < other_src_y || other_img_y >= other_src_y + other_frameHeight) continue;
            
            /* Check if both pixels are non-transparent */
            uint32_t this_pixel = this_img->pixels[this_img_y * this_img->width + this_img_x];
            uint32_t other_pixel = other_img->pixels[other_img_y * other_img->width + other_img_x];
            
            if (((this_pixel >> 24) & 0xFF) > 0 && ((other_pixel >> 24) & 0xFF) > 0) {
                return NATIVE_RETURN_INT(1); /* true - collision found */
            }
        }
    }
    
    return NATIVE_RETURN_INT(0); /* false */
}

/* Sprite.collidesWith(TiledLayer t, boolean pixelLevel) */
static JavaValue native_sprite_collidesWith_tiledlayer(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaObject* tiledlayer = (JavaObject*)args[1].ref;
    jboolean pixel_level = args[2].i;
    
    if (!this_obj || !tiledlayer) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Get sprite bounds */
    int this_x = get_object_field_int(this_obj, "x");
    int this_y = get_object_field_int(this_obj, "y");
    int this_w = get_object_field_int(this_obj, "width");
    int this_h = get_object_field_int(this_obj, "height");
    
    /* Get tiled layer bounds */
    int tl_x = get_object_field_int(tiledlayer, "x");
    int tl_y = get_object_field_int(tiledlayer, "y");
    int tl_w = get_object_field_int(tiledlayer, "width");
    int tl_h = get_object_field_int(tiledlayer, "height");
    
    /* Rectangle intersection test */
    int intersect_x1 = this_x > tl_x ? this_x : tl_x;
    int intersect_y1 = this_y > tl_y ? this_y : tl_y;
    int intersect_x2 = (this_x + this_w) < (tl_x + tl_w) ? (this_x + this_w) : (tl_x + tl_w);
    int intersect_y2 = (this_y + this_h) < (tl_y + tl_h) ? (this_y + this_h) : (tl_y + tl_h);
    
    /* No intersection */
    if (intersect_x1 >= intersect_x2 || intersect_y1 >= intersect_y2) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* If pixel-level is not requested, bounding box is enough for non-empty cells */
    if (!pixel_level) {
        /* Check if any overlapping cell is non-empty (tile index != 0) */
        int tl_columns = get_object_field_int(tiledlayer, "columns");
        int tl_rows = get_object_field_int(tiledlayer, "rows");
        int tl_tile_w = get_object_field_int(tiledlayer, "tileWidth");
        int tl_tile_h = get_object_field_int(tiledlayer, "tileHeight");
        
        /* Get cell map array */
        JavaArray* cell_map = (JavaArray*)get_object_field_ref(tiledlayer, "cellMap");
        if (!cell_map || cell_map->length == 0) {
            return NATIVE_RETURN_INT(0);
        }
        jint* cells = (jint*)array_data(cell_map);
        
        /* Determine which tiles overlap with the sprite */
        int start_col = (intersect_x1 - tl_x) / tl_tile_w;
        int start_row = (intersect_y1 - tl_y) / tl_tile_h;
        int end_col = (intersect_x2 - tl_x - 1) / tl_tile_w;
        int end_row = (intersect_y2 - tl_y - 1) / tl_tile_h;
        
        if (start_col < 0) start_col = 0;
        if (start_row < 0) start_row = 0;
        if (end_col >= tl_columns) end_col = tl_columns - 1;
        if (end_row >= tl_rows) end_row = tl_rows - 1;
        
        for (int row = start_row; row <= end_row; row++) {
            for (int col = start_col; col <= end_col; col++) {
                int tile_idx = cells[row * tl_columns + col];
                if (tile_idx != 0) {
                    return NATIVE_RETURN_INT(1); /* Collision with non-empty cell */
                }
            }
        }
        return NATIVE_RETURN_INT(0); /* All overlapping cells are empty */
    }
    
    /* Pixel-level collision with tiled layer: just check bounding box overlap
     * (full pixel-level tiled layer collision would require rendering each tile
     * and checking individual pixels, which is very expensive) */
    return NATIVE_RETURN_INT(1);
}

/* Sprite.collidesWith(Image image, int x, int y, boolean pixelLevel) */
static JavaValue native_sprite_collidesWith_image(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaObject* image_obj = (JavaObject*)args[1].ref;
    jint img_x = args[2].i;
    jint img_y = args[3].i;
    jboolean pixel_level = args[4].i;
    
    if (!this_obj || !image_obj) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Check visibility */
    if (!get_object_field_int(this_obj, "visible")) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Use collision rectangle instead of full bounds */
    int this_x = get_object_field_int(this_obj, "x") + get_object_field_int(this_obj, "collisionX");
    int this_y = get_object_field_int(this_obj, "y") + get_object_field_int(this_obj, "collisionY");
    int this_w = get_object_field_int(this_obj, "collisionWidth");
    int this_h = get_object_field_int(this_obj, "collisionHeight");
    
    /* Get image dimensions */
    MidpImage* img = get_image_from_object(image_obj);
    if (!img) return NATIVE_RETURN_INT(0);
    
    /* Rectangle intersection test */
    int intersect_x1 = this_x > img_x ? this_x : img_x;
    int intersect_y1 = this_y > img_y ? this_y : img_y;
    int intersect_x2 = (this_x + this_w) < (img_x + img->width) ? (this_x + this_w) : (img_x + img->width);
    int intersect_y2 = (this_y + this_h) < (img_y + img->height) ? (this_y + this_h) : (img_y + img->height);
    
    if (intersect_x1 >= intersect_x2 || intersect_y1 >= intersect_y2) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* If not pixel-level, bounding box is enough */
    if (!pixel_level) {
        return NATIVE_RETURN_INT(1);
    }
    
    /* Pixel-level collision with the image */
    MidpImage* this_img = get_sprite_image(this_obj);
    if (!this_img) return NATIVE_RETURN_INT(0);
    
    int sprite_sx = get_object_field_int(this_obj, "x");
    int sprite_sy = get_object_field_int(this_obj, "y");
    jint frameWidth = get_object_field_int(this_obj, "frameWidth");
    jint frameHeight = get_object_field_int(this_obj, "frameHeight");
    jint transform = get_object_field_int(this_obj, "transform");
    jint currentFrame = get_object_field_int(this_obj, "currentFrame");
    JavaArray* seq = (JavaArray*)get_object_field_ref(this_obj, "frameSequence");
    jint actual_frame = currentFrame;
    if (seq && seq->length > 0 && currentFrame >= 0 && currentFrame < (jint)seq->length) {
        actual_frame = ((jint*)array_data(seq))[currentFrame];
    }
    
    int src_x = 0, src_y = 0;
    if (frameWidth > 0 && frameHeight > 0) {
        int fpr = this_img->width / frameWidth;
        if (fpr > 0) {
            src_x = (actual_frame % fpr) * frameWidth;
            src_y = (actual_frame / fpr) * frameHeight;
        }
    }
    
    for (int y = intersect_y1; y < intersect_y2; y++) {
        for (int x = intersect_x1; x < intersect_x2; x++) {
            /* Map to sprite source image */
            int lx = x - sprite_sx;
            int ly = y - sprite_sy;
            int s_img_x = src_x + lx;
            int s_img_y = src_y + ly;
            
            if (transform != 0) {
                int fw = (transform == 5 || transform == 4 || transform == 6 || transform == 7)
                         ? frameHeight : frameWidth;
                int fh = (transform == 5 || transform == 4 || transform == 6 || transform == 7)
                         ? frameWidth : frameHeight;
                switch (transform) {
                    case 2: s_img_x = src_x + fw - 1 - lx; s_img_y = src_y + ly; break;
                    case 1: s_img_x = src_x + lx; s_img_y = src_y + fh - 1 - ly; break;
                    case 3: s_img_x = src_x + fw - 1 - lx; s_img_y = src_y + fh - 1 - ly; break;
                    case 5: s_img_x = src_x + ly; s_img_y = src_y + fh - 1 - lx; break;
                    case 4: s_img_x = src_x + ly; s_img_y = src_y + lx; break;
                    case 6: s_img_x = src_x + fw - 1 - ly; s_img_y = src_y + lx; break;
                    case 7: s_img_x = src_x + fw - 1 - ly; s_img_y = src_y + fh - 1 - lx; break;
                }
            }
            
            if (s_img_x < 0 || s_img_x >= this_img->width || s_img_y < 0 || s_img_y >= this_img->height) continue;
            
            int r_img_x = x - img_x;
            int r_img_y = y - img_y;
            if (r_img_x < 0 || r_img_x >= img->width || r_img_y < 0 || r_img_y >= img->height) continue;
            
            uint32_t sp = this_img->pixels[s_img_y * this_img->width + s_img_x];
            uint32_t rp = img->pixels[r_img_y * img->width + r_img_x];
            
            if (((sp >> 24) & 0xFF) > 0 && ((rp >> 24) & 0xFF) > 0) {
                return NATIVE_RETURN_INT(1);
            }
        }
    }
    
    return NATIVE_RETURN_INT(0);
}

/* Sprite.defineCollisionRectangle(int x, int y, int width, int height) */
static JavaValue native_sprite_defineCollisionRectangle(JVM* jvm, JavaThread* thread,
                                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    jint width = args[3].i;
    jint height = args[4].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    /* Store collision rectangle parameters in sprite fields */
    set_object_field_int(this_obj, "collisionX", x);
    set_object_field_int(this_obj, "collisionY", y);
    set_object_field_int(this_obj, "collisionWidth", width);
    set_object_field_int(this_obj, "collisionHeight", height);
    
    return NATIVE_RETURN_VOID();
}

/* Sprite.setFrameSequence(int[] sequence) */
static JavaValue native_sprite_setFrameSequence(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaArray* sequence = (JavaArray*)args[1].ref;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    /* Store frame sequence reference */
    set_object_field_ref(this_obj, "frameSequence", (JavaObject*)sequence);
    
    /* Reset current frame index to 0 (MIDP spec) */
    set_object_field_int(this_obj, "currentFrame", 0);
    
    return NATIVE_RETURN_VOID();
}

/* Sprite.nextFrame() */
static JavaValue native_sprite_nextFrame(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    int current_frame = get_object_field_int(this_obj, "currentFrame");
    
    /* Use frameSequence length if set, otherwise rawFrameCount */
    int max_frames;
    JavaArray* frame_seq = (JavaArray*)get_object_field_ref(this_obj, "frameSequence");
    if (frame_seq && frame_seq->length > 0) {
        max_frames = (int)frame_seq->length;
    } else {
        int raw_frame_count = get_object_field_int(this_obj, "rawFrameCount");
        max_frames = raw_frame_count > 0 ? raw_frame_count : 1;
    }
    
    int new_frame = current_frame + 1;
    if (new_frame >= max_frames) new_frame = 0;
    set_object_field_int(this_obj, "currentFrame", new_frame);
    
    return NATIVE_RETURN_VOID();
}

/* Sprite.prevFrame() */
static JavaValue native_sprite_prevFrame(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    int current_frame = get_object_field_int(this_obj, "currentFrame");
    
    /* Use frameSequence length if set, otherwise rawFrameCount */
    int max_frames;
    JavaArray* frame_seq = (JavaArray*)get_object_field_ref(this_obj, "frameSequence");
    if (frame_seq && frame_seq->length > 0) {
        max_frames = (int)frame_seq->length;
    } else {
        int raw_frame_count = get_object_field_int(this_obj, "rawFrameCount");
        max_frames = raw_frame_count > 0 ? raw_frame_count : 1;
    }
    
    int new_frame = current_frame - 1;
    if (new_frame < 0) new_frame = max_frames - 1;
    set_object_field_int(this_obj, "currentFrame", new_frame);
    
    return NATIVE_RETURN_VOID();
}

/* Sprite.defineReferencePixel(int x, int y) */
static JavaValue native_sprite_defineReferencePixel(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint refX = args[1].i;
    jint refY = args[2].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    set_object_field_int(this_obj, "refX", refX);
    set_object_field_int(this_obj, "refY", refY);
    
    return NATIVE_RETURN_VOID();
}

/* Sprite.setRefPixelPosition(int x, int y) - moves sprite so ref pixel is at (x,y) */
static JavaValue native_sprite_setRefPixelPosition(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    jint refX = get_object_field_int(this_obj, "refX");
    jint refY = get_object_field_int(this_obj, "refY");
    jint transform = get_object_field_int(this_obj, "transform");
    jint w = get_object_field_int(this_obj, "width");
    jint h = get_object_field_int(this_obj, "height");
    
    /* Compute transformed reference pixel offset */
    jint transformed_refX, transformed_refY;
    switch (transform) {
        case 0: transformed_refX = refX; transformed_refY = refY; break;
        case 1: transformed_refX = refX; transformed_refY = h - 1 - refY; break;
        case 2: transformed_refX = w - 1 - refX; transformed_refY = refY; break;
        case 3: transformed_refX = w - 1 - refX; transformed_refY = h - 1 - refY; break;
        case 4: transformed_refX = refY; transformed_refY = refX; break;
        case 5: transformed_refX = h - 1 - refY; transformed_refY = refX; break;
        case 6: transformed_refX = refY; transformed_refY = w - 1 - refX; break;
        case 7: transformed_refX = h - 1 - refY; transformed_refY = w - 1 - refX; break;
        default: transformed_refX = refX; transformed_refY = refY; break;
    }
    
    /* Position sprite so that the reference pixel appears at (x, y) */
    set_object_field_int(this_obj, "x", x - transformed_refX);
    set_object_field_int(this_obj, "y", y - transformed_refY);
    
    return NATIVE_RETURN_VOID();
}

/* Sprite.getRefPixelX() */
static JavaValue native_sprite_getRefPixelX(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    jint refX = get_object_field_int(this_obj, "refX");
    jint refY = get_object_field_int(this_obj, "refY");
    jint transform = get_object_field_int(this_obj, "transform");
    jint w = get_object_field_int(this_obj, "width");
    jint h = get_object_field_int(this_obj, "height");
    jint sprite_x = get_object_field_int(this_obj, "x");
    
    /* Compute transformed reference pixel offset */
    jint transformed_refX;
    switch (transform) {
        case 0: transformed_refX = refX; break;
        case 1: transformed_refX = refX; break;
        case 2: transformed_refX = w - 1 - refX; break;
        case 3: transformed_refX = w - 1 - refX; break;
        case 4: transformed_refX = refY; break;
        case 5: transformed_refX = h - 1 - refY; break;
        case 6: transformed_refX = refY; break;
        case 7: transformed_refX = h - 1 - refY; break;
        default: transformed_refX = refX; break;
    }
    
    return NATIVE_RETURN_INT(sprite_x + transformed_refX);
}

/* Sprite.getRefPixelY() */
static JavaValue native_sprite_getRefPixelY(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    jint refX = get_object_field_int(this_obj, "refX");
    jint refY = get_object_field_int(this_obj, "refY");
    jint transform = get_object_field_int(this_obj, "transform");
    jint w = get_object_field_int(this_obj, "width");
    jint h = get_object_field_int(this_obj, "height");
    jint sprite_y = get_object_field_int(this_obj, "y");
    
    /* Compute transformed reference pixel offset */
    jint transformed_refY;
    switch (transform) {
        case 0: transformed_refY = refY; break;
        case 1: transformed_refY = h - 1 - refY; break;
        case 2: transformed_refY = refY; break;
        case 3: transformed_refY = h - 1 - refY; break;
        case 4: transformed_refY = refX; break;
        case 5: transformed_refY = refX; break;
        case 6: transformed_refY = w - 1 - refX; break;
        case 7: transformed_refY = w - 1 - refX; break;
        default: transformed_refY = refY; break;
    }
    
    return NATIVE_RETURN_INT(sprite_y + transformed_refY);
}

/* Sprite.setImage(Image img, int frameWidth, int frameHeight) */
static JavaValue native_sprite_setImage(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaObject* image_obj = (JavaObject*)args[1].ref;
    jint frame_width = args[2].i;
    jint frame_height = args[3].i;
    
    if (!this_obj || !image_obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    MidpImage* img = get_image_from_object(image_obj);
    if (!img) return NATIVE_RETURN_VOID();
    
    /* Update image */
    set_object_field_ref(this_obj, "image", image_obj);
    set_object_field_int(this_obj, "frameWidth", frame_width);
    set_object_field_int(this_obj, "frameHeight", frame_height);
    
    /* Reset transform since frame dimensions may have changed */
    jint transform = get_object_field_int(this_obj, "transform");
    if (transform != 0) {
        set_object_field_int(this_obj, "transform", 0);
    }
    
    /* Update dimensions based on original frame size (not swapped) */
    set_object_field_int(this_obj, "width", frame_width);
    set_object_field_int(this_obj, "height", frame_height);
    
    /* Recalculate raw frame count */
    if (frame_width > 0 && frame_height > 0 &&
        img->width >= frame_width && img->height >= frame_height) {
        int cols = img->width / frame_width;
        int rows = img->height / frame_height;
        set_object_field_int(this_obj, "rawFrameCount", cols * rows);
    }
    
    /* Reset frame sequence and collision rectangle */
    set_object_field_int(this_obj, "currentFrame", 0);
    set_object_field_ref(this_obj, "frameSequence", NULL);
    set_object_field_int(this_obj, "collisionX", 0);
    set_object_field_int(this_obj, "collisionY", 0);
    set_object_field_int(this_obj, "collisionWidth", frame_width);
    set_object_field_int(this_obj, "collisionHeight", frame_height);
    
    return NATIVE_RETURN_VOID();
}

/* Sprite.getRawFrameCount() */
static JavaValue native_sprite_getRawFrameCount(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    jint count = get_object_field_int(this_obj, "rawFrameCount");
    return NATIVE_RETURN_INT(count > 0 ? count : 1);
}

/* Sprite.getFrameSequenceLength() */
static JavaValue native_sprite_getFrameSequenceLength(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(1);
    
    /* If a custom sequence is set, return its length; otherwise return rawFrameCount */
    JavaArray* sequence = (JavaArray*)get_object_field_ref(this_obj, "frameSequence");
    if (sequence && sequence->length > 0) {
        return NATIVE_RETURN_INT(sequence->length);
    }
    
    jint count = get_object_field_int(this_obj, "rawFrameCount");
    return NATIVE_RETURN_INT(count > 0 ? count : 1);
}

/*
 * ============================================
 * Layer native methods (abstract base class)
 * ============================================
 */

/* Layer.getX() */
static JavaValue native_layer_getX(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "x"));
}

/* Layer.getY() */
static JavaValue native_layer_getY(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "y"));
}

/* Layer.getWidth() */
static JavaValue native_layer_getWidth(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "width"));
}

/* Layer.getHeight() */
static JavaValue native_layer_getHeight(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "height"));
}

/* Layer.setPosition(int x, int y) */
static JavaValue native_layer_setPosition(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    set_object_field_int(this_obj, "x", x);
    set_object_field_int(this_obj, "y", y);
    
    return NATIVE_RETURN_VOID();
}

/* Layer.move(int dx, int dy) */
static JavaValue native_layer_move(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint dx = args[1].i;
    jint dy = args[2].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    jint x = get_object_field_int(this_obj, "x");
    jint y = get_object_field_int(this_obj, "y");
    set_object_field_int(this_obj, "x", x + dx);
    set_object_field_int(this_obj, "y", y + dy);
    
    return NATIVE_RETURN_VOID();
}

/* Layer.setVisible(boolean visible) */
static JavaValue native_layer_setVisible(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jboolean visible = args[1].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    set_object_field_int(this_obj, "visible", visible);
    
    return NATIVE_RETURN_VOID();
}

/* Layer.isVisible() */
static JavaValue native_layer_isVisible(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(1);
    
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "visible"));
}

/*
 * ============================================
 * LayerManager native methods
 * ============================================
 */

/* LayerManager.<init>() */
static JavaValue native_layermanager_init(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    DISP_DEBUG("[LayerManager] init: this=%p", (void*)this_obj);
    
    /* Initialize layers array as empty */
    JavaArray* empty_layers = jvm_new_array(jvm, DESC_OBJECT, 0, NULL);
    if (empty_layers) {
        set_object_field_ref(this_obj, "layers", (JavaObject*)empty_layers);
    }
    
    /* Set default view window to MAX_VALUE dimensions */
    set_object_field_int(this_obj, "viewX", 0);
    set_object_field_int(this_obj, "viewY", 0);
    set_object_field_int(this_obj, "viewWidth", 0x7FFFFFFF);
    set_object_field_int(this_obj, "viewHeight", 0x7FFFFFFF);
    
    return NATIVE_RETURN_VOID();
}

/* LayerManager.append(Layer layer) */
static JavaValue native_layermanager_append(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaObject* layer = (JavaObject*)args[1].ref;
    
    if (!this_obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Get current layers array */
    JavaArray* layers = (JavaArray*)get_object_field_ref(this_obj, "layers");
    int old_count = layers ? layers->length : 0;
    int new_count = old_count + 1;
    
    /* Create new layers array with one more slot */
    JavaArray* new_layers = jvm_new_array(jvm, DESC_OBJECT, new_count, NULL);
    if (!new_layers) {
        DISP_DEBUG("[LayerManager] append: failed to allocate new array");
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Copy old layers */
    if (layers && layers->length > 0) {
        void** old_data = (void**)array_data(layers);
        void** new_data = (void**)array_data(new_layers);
        for (int i = 0; i < old_count; i++) {
            new_data[i] = old_data[i];
        }
    }
    
    /* Add new layer at end */
    void** new_data = (void**)array_data(new_layers);
    new_data[old_count] = layer;
    
    /* Update LayerManager's layers array */
    set_object_field_ref(this_obj, "layers", (JavaObject*)new_layers);
    
    DISP_DEBUG("[LayerManager] append: layer=%p at index %d, count=%d", (void*)layer, old_count, new_count);
    
    return NATIVE_RETURN_INT(old_count);  /* Return layer index */
}

/* LayerManager.insert(Layer layer, int index) */
static JavaValue native_layermanager_insert(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaObject* layer = (JavaObject*)args[1].ref;
    jint index = args[2].i;
    
    if (!this_obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    /* Get current layers array */
    JavaArray* layers = (JavaArray*)get_object_field_ref(this_obj, "layers");
    int old_count = layers ? layers->length : 0;
    int new_count = old_count + 1;
    
    /* Clamp index */
    if (index < 0) index = 0;
    if (index > old_count) index = old_count;
    
    /* Create new layers array with one more slot */
    JavaArray* new_layers = jvm_new_array(jvm, DESC_OBJECT, new_count, NULL);
    if (!new_layers) {
        DISP_DEBUG("[LayerManager] insert: failed to allocate new array");
        return NATIVE_RETURN_VOID();
    }
    
    /* Copy old layers, inserting new layer at index */
    void** old_data = (layers && layers->length > 0) ? (void**)array_data(layers) : NULL;
    void** new_data = (void**)array_data(new_layers);
    for (int i = 0; i < new_count; i++) {
        if (i < index) {
            new_data[i] = old_data ? old_data[i] : NULL;
        } else if (i == index) {
            new_data[i] = layer;
        } else {
            new_data[i] = old_data ? old_data[i - 1] : NULL;
        }
    }
    
    /* Update LayerManager's layers array */
    set_object_field_ref(this_obj, "layers", (JavaObject*)new_layers);
    
    DISP_DEBUG("[LayerManager] insert: layer=%p at %d, count=%d", (void*)layer, index, new_count);
    
    return NATIVE_RETURN_VOID();
}

/* LayerManager.remove(Layer layer) */
static JavaValue native_layermanager_remove(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaObject* layer = (JavaObject*)args[1].ref;
    
    if (!this_obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    /* Get current layers array */
    JavaArray* layers = (JavaArray*)get_object_field_ref(this_obj, "layers");
    if (!layers || layers->length == 0) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Find the layer in the array */
    void** layer_data = (void**)array_data(layers);
    int found_idx = -1;
    for (int i = 0; i < layers->length; i++) {
        if (layer_data[i] == layer) {
            found_idx = i;
            break;
        }
    }
    
    if (found_idx < 0) {
        DISP_DEBUG("[LayerManager] remove: layer %p not found", (void*)layer);
        return NATIVE_RETURN_VOID();
    }
    
    /* Create new array with one less element */
    int new_len = layers->length - 1;
    if (new_len == 0) {
        set_object_field_ref(this_obj, "layers", (JavaObject*)jvm_new_array(jvm, DESC_OBJECT, 0, NULL));
        DISP_DEBUG("[LayerManager] remove: removed last layer");
        return NATIVE_RETURN_VOID();
    }
    
    JavaArray* new_layers = jvm_new_array(jvm, DESC_OBJECT, new_len, NULL);
    if (!new_layers) {
        DISP_DEBUG("[LayerManager] remove: failed to allocate new array");
        return NATIVE_RETURN_VOID();
    }
    
    /* Copy layers, skipping the removed one */
    void** old_data = (void**)array_data(layers);
    void** new_data = (void**)array_data(new_layers);
    for (int i = 0; i < new_len; i++) {
        new_data[i] = old_data[i < found_idx ? i : i + 1];
    }
    
    /* Update LayerManager's layers array */
    set_object_field_ref(this_obj, "layers", (JavaObject*)new_layers);
    
    DISP_DEBUG("[LayerManager] remove: removed layer=%p at index %d, count=%d", (void*)layer, found_idx, new_len);
    
    return NATIVE_RETURN_VOID();
}

/* LayerManager.getLayerAt(int index) */
static JavaValue native_layermanager_getLayerAt(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    
    if (!this_obj) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Get layers array */
    JavaArray* layers = (JavaArray*)get_object_field_ref(this_obj, "layers");
    if (!layers || index < 0 || index >= layers->length) {
        return NATIVE_RETURN_NULL();
    }
    
    void** layer_data = (void**)array_data(layers);
    JavaObject* result = (JavaObject*)layer_data[index];
    
    DISP_DEBUG("[LayerManager] getLayerAt: %d -> %p", index, (void*)result);
    
    JavaValue ret = { .ref = result };
    return ret;
}

/* LayerManager.getSize() */
static JavaValue native_layermanager_getSize(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    /* LayerManager has a layers[] array field */
    JavaArray* layers = (JavaArray*)get_object_field_ref(this_obj, "layers");
    if (layers) {
        return NATIVE_RETURN_INT(layers->length);
    }
    
    return NATIVE_RETURN_INT(0);
}

/* Helper: Paint a single layer (Sprite or TiledLayer) */
static void paint_layer(JVM* jvm, JavaObject* layer, JavaObject* gfx_obj, int x, int y) {
    if (!layer || !gfx_obj) return;
    
    JavaClass* clazz = layer->header.clazz;
    if (!clazz || !clazz->class_name) return;
    
    /* Find the paint method on the layer */
    JavaMethod* paint_method = jvm_resolve_method(jvm, clazz, "paint", "(Ljavax/microedition/lcdui/Graphics;)V");
    if (paint_method) {
        JavaValue paint_args[2];
        paint_args[0].ref = layer;
        paint_args[1].ref = gfx_obj;
        
        JavaValue result;
        JavaThread* th = jvm_current_thread(jvm);
        execute_method(jvm, th, paint_method, paint_args, &result);
    }
}

/* LayerManager.paint(Graphics g, int x, int y) */
static JavaValue native_layermanager_paint(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaObject* gfx_obj = (JavaObject*)args[1].ref;
    jint x = args[2].i;
    jint y = args[3].i;
    
    if (!this_obj || !gfx_obj) return NATIVE_RETURN_VOID();
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    if (!gfx) {
        DISP_DEBUG("[LayerManager] paint: failed to get MidpGraphics");
        return NATIVE_RETURN_VOID();
    }
    
    /* Get view window parameters */
    jint viewX = get_object_field_int(this_obj, "viewX");
    jint viewY = get_object_field_int(this_obj, "viewY");
    jint viewWidth = get_object_field_int(this_obj, "viewWidth");
    jint viewHeight = get_object_field_int(this_obj, "viewHeight");
    
    /* Default view window to screen size if not set */
    if (viewWidth <= 0) viewWidth = gfx->width;
    if (viewHeight <= 0) viewHeight = gfx->height;
    
    /* Get layers array */
    JavaArray* layers = (JavaArray*)get_object_field_ref(this_obj, "layers");
    if (!layers || layers->element_type != DESC_OBJECT) {
        return NATIVE_RETURN_VOID();
    }
    
    void** layer_data = (void**)array_data(layers);
    int layer_count = layers->length;
    
    /* Apply translation for the paint offset */
    int old_tx = gfx->translate_x;
    int old_ty = gfx->translate_y;
    gfx->translate_x += x - viewX;
    gfx->translate_y += y - viewY;
    
    /* Clip to view window */
    int old_clip_x = gfx->clip_x;
    int old_clip_y = gfx->clip_y;
    int old_clip_w = gfx->clip_width;
    int old_clip_h = gfx->clip_height;
    
    midp_graphics_clip_rect(gfx, viewX, viewY, viewWidth, viewHeight);
    
    /* Paint each layer from bottom (index 0) to top */
    for (int i = 0; i < layer_count; i++) {
        JavaObject* layer = (JavaObject*)layer_data[i];
        if (layer) {
            /* Check if layer is visible */
            jint visible = get_object_field_int(layer, "visible");
            if (visible) {
                paint_layer(jvm, layer, gfx_obj, x, y);
            }
        }
    }
    
    /* Restore graphics state */
    gfx->translate_x = old_tx;
    gfx->translate_y = old_ty;
    gfx->clip_x = old_clip_x;
    gfx->clip_y = old_clip_y;
    gfx->clip_width = old_clip_w;
    gfx->clip_height = old_clip_h;
    
    return NATIVE_RETURN_VOID();
}

/* LayerManager.setViewWindow(int x, int y, int width, int height) */
static JavaValue native_layermanager_setViewWindow(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    jint width = args[3].i;
    jint height = args[4].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    /* Store view window in LayerManager fields */
    set_object_field_int(this_obj, "viewX", x);
    set_object_field_int(this_obj, "viewY", y);
    set_object_field_int(this_obj, "viewWidth", width);
    set_object_field_int(this_obj, "viewHeight", height);
    
    return NATIVE_RETURN_VOID();
}

/*
 * ============================================
 * TiledLayer native methods
 * ============================================
 */

/* TiledLayer.<init>(int columns, int rows, Image image, int tileWidth, int tileHeight) */
static JavaValue native_tiledlayer_init(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint columns = args[1].i;
    jint rows = args[2].i;
    JavaObject* image = (JavaObject*)args[3].ref;
    jint tile_width = args[4].i;
    jint tile_height = args[5].i;

    if (!this_obj || !image) {
        DISP_DEBUG("[TiledLayer] init: NPE - this=%p, image=%p",
                (void*)this_obj, (void*)image);
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }

    /* Store properties in TiledLayer fields */
    set_object_field_int(this_obj, "columns", columns);
    set_object_field_int(this_obj, "rows", rows);
    set_object_field_int(this_obj, "tileWidth", tile_width);
    set_object_field_int(this_obj, "tileHeight", tile_height);

    /* Store image reference */
    set_object_field_ref(this_obj, "image", image);
    
    /* Create cell map array */
    JavaArray* cell_map = jvm_new_array(jvm, T_INT, columns * rows, NULL);
    if (cell_map) {
        /* Initialize all cells to 0 (no tile) */
        jint* cells = (jint*)array_data(cell_map);
        for (int i = 0; i < columns * rows; i++) {
            cells[i] = 0;
        }
        /* Store in field */
        set_object_field_ref(this_obj, "cellMap", (JavaObject*)cell_map);
    }
    
    DISP_DEBUG("[TiledLayer] init: %dx%d tiles, image=%p, tile=%dx%d", 
            columns, rows, (void*)image, tile_width, tile_height);
    
    return NATIVE_RETURN_VOID();
}

/* TiledLayer.setCell(int col, int row, int tileIndex) */
static JavaValue native_tiledlayer_setCell(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint col = args[1].i;
    jint row = args[2].i;
    jint tile_index = args[3].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    jint columns = get_object_field_int(this_obj, "columns");
    jint rows = get_object_field_int(this_obj, "rows");
    
    if (col < 0 || col >= columns || row < 0 || row >= rows) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Get cell map array */
    JavaArray* cell_map = (JavaArray*)get_object_field_ref(this_obj, "cellMap");
    
    if (cell_map && cell_map->element_type == T_INT) {
        jint* cells = (jint*)array_data(cell_map);
        cells[row * columns + col] = tile_index;
    }
    
    return NATIVE_RETURN_VOID();
}

/* TiledLayer.getCell(int col, int row) */
static JavaValue native_tiledlayer_getCell(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint col = args[1].i;
    jint row = args[2].i;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    jint columns = get_object_field_int(this_obj, "columns");
    jint rows = get_object_field_int(this_obj, "rows");
    
    if (col < 0 || col >= columns || row < 0 || row >= rows) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Get cell map array */
    JavaArray* cell_map = (JavaArray*)get_object_field_ref(this_obj, "cellMap");
    
    if (cell_map && cell_map->element_type == T_INT) {
        jint* cells = (jint*)array_data(cell_map);
        return NATIVE_RETURN_INT(cells[row * columns + col]);
    }
    
    return NATIVE_RETURN_INT(0);
}

/* TiledLayer.fillCells(int col, int row, int numCols, int numRows, int tileIndex) */
static JavaValue native_tiledlayer_fillCells(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint col = args[1].i;
    jint row = args[2].i;
    jint num_cols = args[3].i;
    jint num_rows = args[4].i;
    jint tile_index = args[5].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    jint columns = get_object_field_int(this_obj, "columns");
    jint rows = get_object_field_int(this_obj, "rows");
    
    /* Validate bounds */
    if (col < 0 || row < 0 || num_cols < 0 || num_rows < 0) {
        return NATIVE_RETURN_VOID();
    }
    if (col + num_cols > columns || row + num_rows > rows) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Get cell map array */
    JavaArray* cell_map = (JavaArray*)get_object_field_ref(this_obj, "cellMap");
    
    if (cell_map && cell_map->element_type == T_INT) {
        jint* cells = (jint*)array_data(cell_map);
        /* Fill the rectangular region */
        for (int r = row; r < row + num_rows; r++) {
            for (int c = col; c < col + num_cols; c++) {
                cells[r * columns + c] = tile_index;
            }
        }
    }
    
    DISP_DEBUG("[TiledLayer] fillCells: (%d,%d) %dx%d = tile %d",
            col, row, num_cols, num_rows, tile_index);
    
    return NATIVE_RETURN_VOID();
}

/* TiledLayer.createAnimatedTile(int staticTileIndex) */
static JavaValue native_tiledlayer_createAnimatedTile(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint static_tile_index = args[1].i;
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    /* Get or create animated tiles array */
    JavaArray* anim_tiles = (JavaArray*)get_object_field_ref(this_obj, "animatedTiles");
    
    /* Create array if not exists (initial capacity: 10) */
    if (!anim_tiles) {
        anim_tiles = jvm_new_array(jvm, T_INT, 10, NULL);
        if (!anim_tiles) return NATIVE_RETURN_INT(0);
        
        /* Store in field */
        set_object_field_ref(this_obj, "animatedTiles", (JavaObject*)anim_tiles);
    }
    
    /* Get current count */
    jint anim_count = get_object_field_int(this_obj, "animatedTileCount");
    
    /* Check if we need to grow the array */
    if (anim_count >= (jint)anim_tiles->length) {
        /* Create larger array */
        int new_size = anim_tiles->length * 2;
        JavaArray* new_array = jvm_new_array(jvm, T_INT, new_size, NULL);
        if (!new_array) return NATIVE_RETURN_INT(0);
        
        /* Copy old data */
        jint* old_data = (jint*)array_data(anim_tiles);
        jint* new_data = (jint*)array_data(new_array);
        for (int i = 0; i < anim_count; i++) {
            new_data[i] = old_data[i];
        }
        
        /* Replace array */
        set_object_field_ref(this_obj, "animatedTiles", (JavaObject*)new_array);
        anim_tiles = new_array;
    }
    
    /* Store static tile index */
    jint* anim_data = (jint*)array_data(anim_tiles);
    anim_data[anim_count] = static_tile_index;
    
    /* Update count */
    anim_count++;
    set_object_field_int(this_obj, "animatedTileCount", anim_count);
    
    /* Return negative animated tile index (-1 for first, -2 for second, etc.) */
    /* Animated tile index is 1-based, so first animated tile is -1 */
    jint result = -anim_count;
    
    DISP_DEBUG("[TiledLayer] createAnimatedTile: static=%d -> animated=%d",
            static_tile_index, result);
    
    return NATIVE_RETURN_INT(result);
}

/* TiledLayer.setAnimatedTile(int animatedTileIndex, int staticTileIndex) */
static JavaValue native_tiledlayer_setAnimatedTile(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint anim_tile_index = args[1].i;  /* Negative value */
    jint static_tile_index = args[2].i;
    
    if (!this_obj) return NATIVE_RETURN_VOID();
    
    /* Convert negative index to 0-based array index */
    /* -1 -> 0, -2 -> 1, etc. */
    if (anim_tile_index >= 0) return NATIVE_RETURN_VOID();  /* Invalid index */
    
    int array_index = -anim_tile_index - 1;
    
    /* Get animated tiles array */
    JavaArray* anim_tiles = (JavaArray*)get_object_field_ref(this_obj, "animatedTiles");
    
    if (!anim_tiles || array_index >= (int)anim_tiles->length) {
        return NATIVE_RETURN_VOID();
    }
    
    jint* anim_data = (jint*)array_data(anim_tiles);
    anim_data[array_index] = static_tile_index;
    
    DISP_DEBUG("[TiledLayer] setAnimatedTile: anim=%d -> static=%d",
            anim_tile_index, static_tile_index);
    
    return NATIVE_RETURN_VOID();
}

/* TiledLayer.getAnimatedTile(int animatedTileIndex) */
static JavaValue native_tiledlayer_getAnimatedTile(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    jint anim_tile_index = args[1].i;  /* Negative value */
    
    if (!this_obj) return NATIVE_RETURN_INT(0);
    
    /* Convert negative index to 0-based array index */
    if (anim_tile_index >= 0) return NATIVE_RETURN_INT(0);  /* Invalid index */
    
    int array_index = -anim_tile_index - 1;
    
    /* Get animated tiles array */
    JavaArray* anim_tiles = (JavaArray*)get_object_field_ref(this_obj, "animatedTiles");
    
    if (!anim_tiles || array_index >= (int)anim_tiles->length) {
        return NATIVE_RETURN_INT(0);
    }
    
    jint* anim_data = (jint*)array_data(anim_tiles);
    return NATIVE_RETURN_INT(anim_data[array_index]);
}

/* TiledLayer.paint(Graphics g) */
static JavaValue native_tiledlayer_paint(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaObject* gfx_obj = (JavaObject*)args[1].ref;
    
    if (!this_obj || !gfx_obj) return NATIVE_RETURN_VOID();
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    if (!gfx) {
        DISP_DEBUG("[TiledLayer] paint: failed to get MidpGraphics");
        return NATIVE_RETURN_VOID();
    }
    
    /* Get TiledLayer properties */
    jint x = get_object_field_int(this_obj, "x");
    jint y = get_object_field_int(this_obj, "y");
    jint columns = get_object_field_int(this_obj, "columns");
    jint rows = get_object_field_int(this_obj, "rows");
    jint tile_width = get_object_field_int(this_obj, "tileWidth");
    jint tile_height = get_object_field_int(this_obj, "tileHeight");
    
    if (columns <= 0 || rows <= 0 || tile_width <= 0 || tile_height <= 0) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Get image */
    JavaObject* image_obj = get_object_field_ref(this_obj, "image");
    if (!image_obj) return NATIVE_RETURN_VOID();
    
    MidpImage* img = (MidpImage*)get_object_field_ref(image_obj, "nativePeer");
    if (!img || !img->pixels) return NATIVE_RETURN_VOID();
    
    /* Get cell map */
    JavaArray* cell_map = (JavaArray*)get_object_field_ref(this_obj, "cellMap");
    
    if (!cell_map || cell_map->element_type != T_INT) {
        return NATIVE_RETURN_VOID();
    }
    
    jint* cells = (jint*)array_data(cell_map);
    
    /* Get animated tiles array for resolving animated tile indices */
    JavaArray* anim_tiles = (JavaArray*)get_object_field_ref(this_obj, "animatedTiles");
    jint* anim_data = anim_tiles ? (jint*)array_data(anim_tiles) : NULL;
    
    /* Calculate tiles per row in source image */
    int tiles_per_row = img->width / tile_width;
    if (tiles_per_row <= 0) tiles_per_row = 1;
    
    /* Paint each cell */
    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < columns; col++) {
            int tile_index = cells[row * columns + col];
            
            if (tile_index != 0) {
                /* Handle animated tiles (negative index) */
                if (tile_index < 0) {
                    /* Convert negative animated index to array index: -1 -> 0, -2 -> 1, etc. */
                    int anim_array_index = -tile_index - 1;
                    if (anim_data && anim_tiles && anim_array_index < (int)anim_tiles->length) {
                        tile_index = anim_data[anim_array_index];
                    } else {
                        tile_index = 0;  /* Invalid animated tile, skip */
                    }
                }
                
                if (tile_index > 0) {
                    /* Tile index is 1-based, convert to 0-based */
                    tile_index--;
                    
                    /* Calculate source position in tile set */
                    int src_col = tile_index % tiles_per_row;
                    int src_row = tile_index / tiles_per_row;
                    int src_x = src_col * tile_width;
                    int src_y = src_row * tile_height;
                    
                    /* Calculate destination position */
                    int dest_x = x + col * tile_width;
                    int dest_y = y + row * tile_height;
                    
                    /* Draw the tile */
                    midp_graphics_draw_region(gfx, img, src_x, src_y, 
                                              tile_width, tile_height,
                                              0, dest_x, dest_y, 0);
                }
            }
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* TiledLayer.getColumns() */
static JavaValue native_tiledlayer_getColumns(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    if (!this_obj) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "columns"));
}

/* TiledLayer.getRows() */
static JavaValue native_tiledlayer_getRows(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    if (!this_obj) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "rows"));
}

/* TiledLayer.getCellWidth() */
static JavaValue native_tiledlayer_getCellWidth(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    if (!this_obj) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "tileWidth"));
}

/* TiledLayer.getCellHeight() */
static JavaValue native_tiledlayer_getCellHeight(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    if (!this_obj) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(get_object_field_int(this_obj, "tileHeight"));
}

static JavaValue native_image_createImage_from_bytes(JVM* jvm, JavaThread* thread,
                                                      JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaArray* data = (JavaArray*)args[0].ref;
    jint offset = args[1].i;
    jint length = args[2].i;
    
    if (!data) {
        /* COMPATIBILITY: Return null instead of throwing NPE for null data */
        GFX_DEBUG("createImage: WARNING - null data, returning null for compatibility");
        return NATIVE_RETURN_NULL();
    }
    
    uint8_t* img_data = (uint8_t*)array_data(data) + offset;
    
    /* Decode image using stb_image */
    MidpImage* img = midp_image_create_from_data(img_data, 0, length);
    if (!img) {
        return NATIVE_RETURN_NULL();
    }
    
    /* Create Java Image object */
    JavaClass* image_class = jvm_load_class(jvm, "javax/microedition/lcdui/Image");
    if (!image_class) {
        GFX_DEBUG("createImage_from_bytes: Failed to load Image class");
        midp_image_destroy(img);
        return NATIVE_RETURN_NULL();
    }
    
    /* Ensure class has space for nativePeer */
    ensure_native_peer_field(image_class);
    
    JavaObject* image_obj = jvm_new_object(jvm, image_class);
    if (!image_obj) {
        GFX_DEBUG("createImage_from_bytes: Failed to create Image object");
        midp_image_destroy(img);
        return NATIVE_RETURN_NULL();
    }
    
    set_object_field_ref(image_obj, "nativePeer", (JavaObject*)img);
    
    GFX_DEBUG("createImage_from_bytes: Created Image %dx%d, obj=%p, native=%p", 
            img->width, img->height, (void*)image_obj, (void*)img);
    
    return NATIVE_RETURN_OBJECT(image_obj);
}

/*
 * GameCanvas native methods
 */

/* GameCanvas key state bit masks */
#define GAME_KEY_UP_PRESSED     (1 << 1)   /* bit 1 = UP */
#define GAME_KEY_DOWN_PRESSED   (1 << 6)   /* bit 6 = DOWN */
#define GAME_KEY_LEFT_PRESSED   (1 << 2)   /* bit 2 = LEFT */
#define GAME_KEY_RIGHT_PRESSED  (1 << 5)   /* bit 5 = RIGHT */
#define GAME_KEY_FIRE_PRESSED   (1 << 8)   /* bit 8 = FIRE */
#define GAME_KEY_A_PRESSED      (1 << 9)   /* bit 9 = GAME_A */
#define GAME_KEY_B_PRESSED      (1 << 10)  /* bit 10 = GAME_B */
#define GAME_KEY_C_PRESSED      (1 << 11)  /* bit 11 = GAME_C */
#define GAME_KEY_D_PRESSED      (1 << 12)  /* bit 12 = GAME_D */

static int compute_key_states(SdlContext* sdl_ctx) {
    int states = 0;
    if (!sdl_ctx) {
        DISP_DEBUG("[getKeyStates] ERROR: sdl_ctx is NULL!");
        return 0;
    }
    
    /* Arrow keys */
    if (sdl_key_pressed(sdl_ctx, GAME_UP))    { states |= GAME_KEY_UP_PRESSED; DISP_DEBUG("[getKeyStates] UP pressed"); }
    if (sdl_key_pressed(sdl_ctx, GAME_DOWN))  { states |= GAME_KEY_DOWN_PRESSED; DISP_DEBUG("[getKeyStates] DOWN pressed"); }
    if (sdl_key_pressed(sdl_ctx, GAME_LEFT))  { states |= GAME_KEY_LEFT_PRESSED; DISP_DEBUG("[getKeyStates] LEFT pressed"); }
    if (sdl_key_pressed(sdl_ctx, GAME_RIGHT)) { states |= GAME_KEY_RIGHT_PRESSED; DISP_DEBUG("[getKeyStates] RIGHT pressed"); }
    if (sdl_key_pressed(sdl_ctx, GAME_FIRE))  { states |= GAME_KEY_FIRE_PRESSED; DISP_DEBUG("[getKeyStates] FIRE pressed"); }
    if (sdl_key_pressed(sdl_ctx, GAME_A))     { states |= GAME_KEY_A_PRESSED; }
    if (sdl_key_pressed(sdl_ctx, GAME_B))     { states |= GAME_KEY_B_PRESSED; }
    if (sdl_key_pressed(sdl_ctx, GAME_C))     { states |= GAME_KEY_C_PRESSED; }
    if (sdl_key_pressed(sdl_ctx, GAME_D))     { states |= GAME_KEY_D_PRESSED; }
    
    if (states != 0) {
        DISP_DEBUG("[getKeyStates] Returning states=0x%08X", states);
    }
    
    return states;
}

static JavaValue native_gamecanvas_getKeyStates(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    
    SdlContext* sdl_ctx = sdl_get_global_context();
    return NATIVE_RETURN_INT(compute_key_states(sdl_ctx));
}

static JavaValue native_gamecanvas_suppressKeyEvents(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jboolean suppress = args[0].i;
    game_canvas.suppress_key_events = suppress != 0;
    DISP_DEBUG("[GameCanvas] suppressKeyEvents(%s) called", suppress ? "true" : "false");
    return NATIVE_RETURN_VOID();
}

/* GameCanvas.getGraphics() - returns Graphics bound to screen framebuffer */
static JavaValue native_gamecanvas_getGraphics(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* gamecanvas_obj = (JavaObject*)args[0].ref;
    
    DISP_DEBUG("[GameCanvas] getGraphics() called");
    
    /* Get the SDL context to determine screen size */
    SdlContext* sdl_ctx = sdl_get_global_context();
    if (!sdl_ctx) {
        DISP_DEBUG("[GameCanvas] getGraphics: no SDL context");
        return NATIVE_RETURN_NULL();
    }
    
    /* Determine the size for offscreen buffer */
    int buffer_width = sdl_ctx->width;
    int buffer_height = sdl_ctx->height;
    
    /* Check if we need to create or resize the offscreen buffer */
    if (!game_canvas.offscreen_buffer || 
        game_canvas.offscreen_width != buffer_width || 
        game_canvas.offscreen_height != buffer_height) {
        
        /* Destroy old buffer if exists */
        if (game_canvas.offscreen_buffer) {
            midp_image_destroy(game_canvas.offscreen_buffer);
        }
        
        /* Create new offscreen buffer */
        game_canvas.offscreen_buffer = midp_image_create(buffer_width, buffer_height, true);
        if (!game_canvas.offscreen_buffer) {
            DISP_DEBUG("[GameCanvas] getGraphics: failed to create offscreen buffer");
            return NATIVE_RETURN_NULL();
        }
        game_canvas.offscreen_width = buffer_width;
        game_canvas.offscreen_height = buffer_height;
        
        DISP_DEBUG("[GameCanvas] getGraphics: created offscreen buffer %dx%d, pixels=%p", 
                buffer_width, buffer_height, (void*)game_canvas.offscreen_buffer->pixels);
    }
    
    /* Create MidpGraphics for the offscreen buffer */
    MidpGraphics* gfx = (MidpGraphics*)malloc(sizeof(MidpGraphics));
    if (!gfx) {
        DISP_DEBUG("[GameCanvas] getGraphics: failed to allocate graphics");
        return NATIVE_RETURN_NULL();
    }
    
    midp_graphics_init(gfx, game_canvas.offscreen_buffer->pixels, buffer_width, buffer_height);
    
    /* Reset clip to available area */
    gfx->clip_x = 0;
    gfx->clip_y = 0;
    gfx->clip_width = buffer_width;
    gfx->clip_height = buffer_height;
    gfx->translate_x = 0;
    gfx->translate_y = 0;
    
    /* Limit clip if soft buttons are shown */
    if (!g_full_screen_mode && current_displayable_obj) {
        int commands_idx = find_field_index(current_displayable_obj, "commands");
        if (commands_idx >= 0) {
            JavaArray* commands = (JavaArray*)current_displayable_obj->fields[commands_idx].ref;
            if (commands && commands->length > 0) {
                gfx->clip_height = buffer_height - SOFT_BUTTON_HEIGHT;
            }
        }
    }
    
    DISP_DEBUG("[GameCanvas] getGraphics: gfx=%p, pixels=%p, %dx%d, clip_height=%d", 
            (void*)gfx, (void*)gfx->pixels, gfx->width, gfx->height, gfx->clip_height);
    
    /* Create a Java Graphics object to wrap the native graphics */
    JavaClass* gfx_class = jvm_load_class(jvm, "javax/microedition/lcdui/Graphics");
    if (!gfx_class) {
        DISP_DEBUG("[GameCanvas] getGraphics: failed to load Graphics class");
        free(gfx);
        return NATIVE_RETURN_NULL();
    }
    
    /* Ensure class has space for nativePeer */
    ensure_native_peer_field(gfx_class);
    
    JavaObject* gfx_obj = jvm_new_object(jvm, gfx_class);
    if (!gfx_obj) {
        DISP_DEBUG("[GameCanvas] getGraphics: failed to create Graphics object");
        free(gfx);
        return NATIVE_RETURN_NULL();
    }
    
    /* Store the MidpGraphics* in the nativePeer field */
    set_object_field_ref(gfx_obj, "nativePeer", (JavaObject*)gfx);
    
    DISP_DEBUG("[GameCanvas] getGraphics: returning Java Graphics obj=%p (native=%p)",
            (void*)gfx_obj, (void*)gfx);
    return NATIVE_RETURN_OBJECT(gfx_obj);
}

/* GameCanvas.flushGraphics() - flush the entire buffer to the screen */
static JavaValue native_gamecanvas_flushGraphics(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* gamecanvas = (JavaObject*)args[0].ref;
    
    DISP_DEBUG("[GameCanvas] flushGraphics() called, offscreen_buffer=%p", 
            (void*)game_canvas.offscreen_buffer);
    
    /* If we have an offscreen buffer, copy it to the screen */
    if (game_canvas.offscreen_buffer && game_canvas.offscreen_buffer->pixels) {
        SdlContext* sdl_ctx = sdl_get_global_context();
        if (sdl_ctx && sdl_ctx->framebuffer) {
            uint32_t* dst_pixels = NULL;
            int dst_width = sdl_ctx->width;
            
            MidpGraphics* screen_gfx = sdl_get_graphics(sdl_ctx);
            if (screen_gfx && screen_gfx->pixels) {
                dst_pixels = screen_gfx->pixels;
                dst_width = screen_gfx->width;
            } else if (sdl_ctx->headless || !screen_gfx) {
                /* Headless mode or missing screen graphics: copy directly to framebuffer */
                dst_pixels = sdl_ctx->framebuffer;
                dst_width = sdl_ctx->width;
            }
            
            if (dst_pixels) {
                /* Copy offscreen buffer to screen */
                int copy_width = game_canvas.offscreen_width < dst_width ? 
                                 game_canvas.offscreen_width : dst_width;
                int copy_height = game_canvas.offscreen_height < (sdl_ctx->height) ? 
                                  game_canvas.offscreen_height : (sdl_ctx->height);
                
                DISP_DEBUG("[GameCanvas] flushGraphics: blitting %dx%d from offscreen to screen",
                        copy_width, copy_height);
                
                for (int y = 0; y < copy_height; y++) {
                    for (int x = 0; x < copy_width; x++) {
                        uint32_t pixel = game_canvas.offscreen_buffer->pixels[y * game_canvas.offscreen_width + x];
                        dst_pixels[y * dst_width + x] = pixel;
                    }
                }
            }
        }
    }
    
    /* Render soft buttons (commands) after game rendering */
    if (gamecanvas && !g_full_screen_mode && gamecanvas == current_displayable_obj) {
        render_soft_buttons(jvm, gamecanvas);
    }
    
    /* Request redraw */
    sdl_request_redraw();
    
    return NATIVE_RETURN_VOID();
}

/* GameCanvas.flushGraphics(int x, int y, int w, int h) - flush a region */
static JavaValue native_gamecanvas_flushGraphicsRegion(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* gamecanvas = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    jint w = args[3].i;
    jint h = args[4].i;
    
    DISP_DEBUG("[GameCanvas] flushGraphics(%d, %d, %d, %d) called", x, y, w, h);
    
    /* If we have an offscreen buffer, copy the region to the screen */
    if (game_canvas.offscreen_buffer && game_canvas.offscreen_buffer->pixels) {
        SdlContext* sdl_ctx = sdl_get_global_context();
        if (sdl_ctx && sdl_ctx->framebuffer) {
            MidpGraphics* screen_gfx = sdl_get_graphics(sdl_ctx);
            if (screen_gfx && screen_gfx->pixels) {
                /* Clip region to valid bounds */
                if (x < 0) { w += x; x = 0; }
                if (y < 0) { h += y; y = 0; }
                if (x + w > game_canvas.offscreen_width) w = game_canvas.offscreen_width - x;
                if (y + h > game_canvas.offscreen_height) h = game_canvas.offscreen_height - y;
                if (x + w > screen_gfx->width) w = screen_gfx->width - x;
                if (y + h > screen_gfx->height) h = screen_gfx->height - y;
                
                if (w > 0 && h > 0) {
                    for (int py = y; py < y + h; py++) {
                        for (int px = x; px < x + w; px++) {
                            uint32_t pixel = game_canvas.offscreen_buffer->pixels[py * game_canvas.offscreen_width + px];
                            screen_gfx->pixels[py * screen_gfx->width + px] = pixel;
                        }
                    }
                }
            }
        }
    }
    
    /* Render soft buttons */
    if (gamecanvas && !g_full_screen_mode && gamecanvas == current_displayable_obj) {
        render_soft_buttons(jvm, gamecanvas);
    }
    
    /* Request redraw */
    sdl_request_redraw();
    
    return NATIVE_RETURN_VOID();
}

/*
 * Image native methods
 */

/* Helper to get MidpImage from Java Image object */
MidpImage* get_image_from_object(JavaObject* obj) {
    if (!obj) {
        return NULL;
    }
    
    if (!obj->header.clazz) {
        return NULL;
    }
    
    /* The MidpImage* is stored in the nativePeer field */
    return (MidpImage*)get_object_field_ref(obj, "nativePeer");
}

/* Ensure a class has at least one field for storing native pointer */
void ensure_native_peer_field(JavaClass* clazz) {
    if (!clazz) return;
    
    /* Minimum size needed: ObjectHeader + 1 field (JavaValue)
     * Note: nativePeer is stored as a reference (1 slot) on our heap,
     * even though Java 'long' would normally take 2 slots. */
    size_t min_size = sizeof(ObjectHeader) + sizeof(JavaValue);
    
    /* 1. Check if "nativePeer" field specifically exists */
    bool has_native_peer = false;
    if (clazz->fields_count > 0 && clazz->fields) {
        for (int i = 0; i < clazz->fields_count; i++) {
            if (clazz->fields[i].name && strcmp(clazz->fields[i].name, "nativePeer") == 0) {
                has_native_peer = true;
                break;
            }
        }
    }
    
    /* 2. If nativePeer already exists and size is sufficient, nothing to do */
    if (has_native_peer && clazz->instance_size >= min_size) {
        return;
    }
    
    /* 3. If no fields at all, add nativePeer */
    if (clazz->fields_count == 0) {
        JavaField* new_fields = (JavaField*)calloc(1, sizeof(JavaField));
        if (!new_fields) return; /* OOM */
        
        clazz->fields = new_fields;
        clazz->fields_count = 1;
        
        clazz->fields[0].name = strdup("nativePeer");
        clazz->fields[0].descriptor = strdup("Ljava/lang/Object;");
        clazz->fields[0].access_flags = ACC_PRIVATE;
    }
    /* 4. If fields exist but no nativePeer, add it (reallocate fields array) */
    else if (!has_native_peer) {
        int old_count = clazz->fields_count;
        int new_count = old_count + 1;
        
        JavaField* new_fields = (JavaField*)realloc(clazz->fields, new_count * sizeof(JavaField));
        if (!new_fields) return; /* OOM */
        
        clazz->fields = new_fields;
        clazz->fields_count = new_count;
        
        /* Initialize the new field */
        memset(&clazz->fields[old_count], 0, sizeof(JavaField));
        clazz->fields[old_count].name = strdup("nativePeer");
        clazz->fields[old_count].descriptor = strdup("Ljava/lang/Object;");
        clazz->fields[old_count].access_flags = ACC_PRIVATE;
    }
    
    /* 5. Update instance_size if needed */
    size_t computed_size = sizeof(ObjectHeader) + clazz->fields_count * sizeof(JavaValue);
    if (computed_size > clazz->instance_size) {
        clazz->instance_size = computed_size;
    }
}

static JavaValue native_image_createImage(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jint width = args[0].i;
    jint height = args[1].i;
    
    GFX_DEBUG("createImage(%d, %d)", width, height);
    
    /* Create the native image */
    MidpImage* img = midp_image_create(width, height, true);
    if (!img) {
        native_throw_oome(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Diagnostic: track image creation */
    DISP_DEBUG("[createImage] Created mutable image: %dx%d, native=%p, pixels=%p",
            width, height, (void*)img, (void*)img->pixels);
    
    
    /* Create a Java Image object to wrap it */
    JavaClass* image_class = jvm_load_class(jvm, "javax/microedition/lcdui/Image");
    if (!image_class) {
        GFX_DEBUG("Failed to load Image class");
        midp_image_destroy(img);
        return NATIVE_RETURN_NULL();
    }
    
    /* Ensure class has space for nativePeer */
    ensure_native_peer_field(image_class);
    
    JavaObject* image_obj = jvm_new_object(jvm, image_class);
    if (!image_obj) {
        GFX_DEBUG("Failed to create Image object");
        midp_image_destroy(img);
        return NATIVE_RETURN_NULL();
    }
    
    /* Store the MidpImage* in the nativePeer field */
    set_object_field_ref(image_obj, "nativePeer", (JavaObject*)img);
    
    DISP_DEBUG("[createImage] Java Image obj=%p -> native=%p", (void*)image_obj, (void*)img);
    

    return NATIVE_RETURN_OBJECT(image_obj);
}

/* Cache for missing resources to avoid repeated failed lookups */
#define MISSING_CACHE_SIZE 64
static char* g_missing_cache[MISSING_CACHE_SIZE];
static int g_missing_cache_count = 0;
static bool g_missing_cache_initialized = false;

/* Check if resource is in missing cache */
static bool is_in_missing_cache(const char* name) {
    for (int i = 0; i < g_missing_cache_count; i++) {
        if (g_missing_cache[i] && strcmp(g_missing_cache[i], name) == 0) {
            return true;
        }
    }
    return false;
}

/* Add to missing cache */
static void add_to_missing_cache(const char* name) {
    if (g_missing_cache_count >= MISSING_CACHE_SIZE) {
        /* Free oldest entry */
        free(g_missing_cache[0]);
        for (int i = 0; i < MISSING_CACHE_SIZE - 1; i++) {
            g_missing_cache[i] = g_missing_cache[i + 1];
        }
        g_missing_cache_count--;
    }
    g_missing_cache[g_missing_cache_count++] = strdup(name);
}

/* Create a placeholder image for missing resources */
static JavaObject* create_placeholder_image(JVM* jvm) {
    /* Create a small 8x8 checkerboard placeholder */
    int w = 8, h = 8;
    MidpImage* img = midp_image_create(w, h, true);
    if (!img) return NULL;
    
    /* Fill with checkerboard pattern (magenta/black) */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t color = ((x + y) % 2 == 0) ? 0xFFFF00FF : 0xFF000000; /* Magenta/Black */
            img->pixels[y * w + x] = color;
        }
    }
    
    /* Create Java Image object */
    JavaClass* image_class = jvm_load_class(jvm, "javax/microedition/lcdui/Image");
    if (!image_class) {
        midp_image_destroy(img);
        return NULL;
    }
    
    ensure_native_peer_field(image_class);
    JavaObject* image_obj = jvm_new_object(jvm, image_class);
    if (!image_obj) {
        midp_image_destroy(img);
        return NULL;
    }
    
    set_object_field_ref(image_obj, "nativePeer", (JavaObject*)img);
    return image_obj;
}

/* Image.createImage(String) - load image from JAR resource */
static JavaValue native_image_createImage_from_string(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* name_str = (JavaString*)args[0].ref;
    
    if (!name_str) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Get the resource name */
    const char* name = string_utf8(jvm, name_str);
    if (!name) {
        return NATIVE_RETURN_NULL();
    }
    
    /* Skip leading slash if present (JAR resources don't have leading slash) */
    const char* resource_name = name;
    if (resource_name[0] == '/') {
        resource_name++;
    }
    
    /* Check missing cache first to avoid repeated failed lookups */
    if (is_in_missing_cache(resource_name)) {
        /* Return placeholder silently without logging */
        JavaObject* placeholder = create_placeholder_image(jvm);
        if (placeholder) {
            return NATIVE_RETURN_OBJECT(placeholder);
        }
        return NATIVE_RETURN_NULL();
    }
    
    /* Load the resource from JAR */
    size_t data_size;
    uint8_t* data = load_jar_resource(resource_name, &data_size);

    /* FALLBACK: If not found and name ends with .jpg, try without .jpg extension */
    if (!data) {
        size_t name_len = strlen(resource_name);
        if (name_len > 4 && strcmp(resource_name + name_len - 4, ".jpg") == 0) {
            char* try_name = strdup(resource_name);
            if (try_name) {
                try_name[name_len - 4] = '\0';
                data = load_jar_resource(try_name, &data_size);
                free(try_name);
            }
        }
    }
    
    /* FALLBACK 2: Try with .png extension */
    if (!data) {
        size_t name_len = strlen(resource_name);
        char* try_name = malloc(name_len + 5);
        if (try_name) {
            strcpy(try_name, resource_name);
            if (name_len > 4 && strcmp(try_name + name_len - 4, ".jpg") == 0) {
                strcpy(try_name + name_len - 4, ".png");
            } else {
                strcat(try_name, ".png");
            }
            data = load_jar_resource(try_name, &data_size);
            free(try_name);
        }
    }

    if (!data) {
        /* Log once and add to missing cache */
        DISP_DEBUG("[Image] Resource not found: %s (using placeholder)", resource_name);
        add_to_missing_cache(resource_name);
        
        /* Return placeholder instead of throwing exception */
        JavaObject* placeholder = create_placeholder_image(jvm);
        if (placeholder) {
            return NATIVE_RETURN_OBJECT(placeholder);
        }
        return NATIVE_RETURN_NULL();
    }

    /* Decode PNG */
    MidpImage* img = midp_image_create_from_data(data, 0, data_size);
    free(data);

    if (!img) {
        DISP_DEBUG("[Image] Failed to decode: %s (using placeholder)", resource_name);
        add_to_missing_cache(resource_name);
        
        JavaObject* placeholder = create_placeholder_image(jvm);
        if (placeholder) {
            return NATIVE_RETURN_OBJECT(placeholder);
        }
        return NATIVE_RETURN_NULL();
    }
    
    /* Create a Java Image object to wrap it */
    JavaClass* image_class = jvm_load_class(jvm, "javax/microedition/lcdui/Image");
    if (!image_class) {
        midp_image_destroy(img);
        return NATIVE_RETURN_NULL();
    }
    
    /* Ensure class has space for nativePeer */
    ensure_native_peer_field(image_class);
    
    JavaObject* image_obj = jvm_new_object(jvm, image_class);
    if (!image_obj) {
        midp_image_destroy(img);
        return NATIVE_RETURN_NULL();
    }
    
    /* Store the MidpImage* in the nativePeer field */
    set_object_field_ref(image_obj, "nativePeer", (JavaObject*)img);
    
    GFX_DEBUG("Created Image %dx%d from %s", img->width, img->height, name);
    
    return NATIVE_RETURN_OBJECT(image_obj);
}

static JavaValue native_image_createRGBImage(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaArray* rgb_array = (JavaArray*)args[0].ref;
    jint width = args[1].i;
    jint height = args[2].i;
    jboolean process_alpha = args[3].i;
    
    if (!rgb_array) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    jint* rgb = (jint*)array_data(rgb_array);
    MidpImage* img = midp_image_create_from_rgb(rgb, width, height, process_alpha != 0);
    
    if (!img) {
        native_throw_oome(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    /* Create a Java Image object to wrap it */
    JavaClass* image_class = jvm_load_class(jvm, "javax/microedition/lcdui/Image");
    if (!image_class) {
        midp_image_destroy(img);
        return NATIVE_RETURN_NULL();
    }
    
    /* Ensure class has space for nativePeer */
    ensure_native_peer_field(image_class);
    
    JavaObject* image_obj = jvm_new_object(jvm, image_class);
    if (!image_obj) {
        midp_image_destroy(img);
        return NATIVE_RETURN_NULL();
    }
    
    /* Store the MidpImage* in the nativePeer field */
    set_object_field_ref(image_obj, "nativePeer", (JavaObject*)img);
    
    return NATIVE_RETURN_OBJECT(image_obj);
}

static JavaValue native_image_getWidth(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    MidpImage* img = get_image_from_object(obj);
    return NATIVE_RETURN_INT(img ? img->width : 0);
}

static JavaValue native_image_getHeight(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    MidpImage* img = get_image_from_object(obj);
    return NATIVE_RETURN_INT(img ? img->height : 0);
}

static JavaValue native_image_isMutable(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    MidpImage* img = get_image_from_object(obj);
    return NATIVE_RETURN_INT(img && img->mutable ? 1 : 0);
}

static JavaValue native_image_getRGB(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    MidpImage* img = get_image_from_object(obj);
    JavaArray* rgb_data = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint scanlength = args[3].i;
    jint x = args[4].i;
    jint y = args[5].i;
    jint width = args[6].i;
    jint height = args[7].i;
    
    if (!img || !rgb_data) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    midp_image_get_rgb(img, (jint*)array_data(rgb_data), offset, scanlength, x, y, width, height);
    
    return NATIVE_RETURN_VOID();
}

/* Image.createImage(Image source, int x, int y, int width, int height, int transform)
 * CRITICAL: Creates a sub-image with optional transformation
 */
static JavaValue native_image_createImage_region(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* src_image_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    jint width = args[3].i;
    jint height = args[4].i;
    jint transform = args[5].i;
    
    if (!src_image_obj) {
        return NATIVE_RETURN_NULL();
    }
    
    MidpImage* src_img = get_image_from_object(src_image_obj);
    if (!src_img) {
        GFX_DEBUG("createImage_region: source image has no native data");
        return NATIVE_RETURN_NULL();
    }
    
    /* Bounds check */
    if (x < 0 || y < 0 || width <= 0 || height <= 0) {
        return NATIVE_RETURN_NULL();
    }
    if (x + width > src_img->width || y + height > src_img->height) {
        GFX_DEBUG("createImage_region: region out of bounds");
        return NATIVE_RETURN_NULL();
    }
    
    /* Calculate output dimensions based on transform */
    int dest_w = width, dest_h = height;
    if (transform == SPRITE_TRANS_ROT90 || transform == SPRITE_TRANS_ROT270 ||
        transform == SPRITE_TRANS_MIRROR_ROT90 || transform == SPRITE_TRANS_MIRROR_ROT270) {
        dest_w = height;
        dest_h = width;
    }
    
    /* Allocate new image */
    MidpImage* new_img = (MidpImage*)calloc(1, sizeof(MidpImage));
    if (!new_img) return NATIVE_RETURN_NULL();
    
    new_img->width = dest_w;
    new_img->height = dest_h;
    new_img->mutable = false;
    new_img->alpha = src_img->alpha;
    new_img->pixels = (uint32_t*)malloc(dest_w * dest_h * sizeof(uint32_t));
    if (!new_img->pixels) {
        free(new_img);
        return NATIVE_RETURN_NULL();
    }
    
    /* Copy pixels with transformation
     * ИСПРАВЛЕНО: Формулы пересчитаны по спецификации MIDP2
     * См. комментарии в graphics.c midp_graphics_draw_region()
     */
    for (int dy = 0; dy < height; dy++) {
        for (int dx = 0; dx < width; dx++) {
            int src_idx = (y + dy) * src_img->width + (x + dx);
            uint32_t pixel = src_img->pixels[src_idx];
            
            int dest_x, dest_y;
            switch (transform) {
                case SPRITE_TRANS_NONE:           /* 0 */
                    dest_x = dx; dest_y = dy; 
                    break;
                case SPRITE_TRANS_MIRROR_ROT180:  /* 1 - вертикальный flip */
                    dest_x = dx; dest_y = height - 1 - dy; 
                    break;
                case SPRITE_TRANS_MIRROR:         /* 2 - горизонтальный flip */
                    dest_x = width - 1 - dx; dest_y = dy; 
                    break;
                case SPRITE_TRANS_ROT180:         /* 3 */
                    dest_x = width - 1 - dx; dest_y = height - 1 - dy; 
                    break;
                case SPRITE_TRANS_MIRROR_ROT270:  /* 4 - transpose */
                    dest_x = dy; dest_y = dx; 
                    break;
                case SPRITE_TRANS_ROT90:          /* 5 */
                    dest_x = height - 1 - dy; dest_y = dx; 
                    break;
                case SPRITE_TRANS_ROT270:         /* 6 */
                    dest_x = dy; dest_y = width - 1 - dx; 
                    break;
                case SPRITE_TRANS_MIRROR_ROT90:   /* 7 */
                    dest_x = height - 1 - dy; dest_y = width - 1 - dx; 
                    break;
                default:
                    dest_x = dx; dest_y = dy; 
                    break;
            }
            
            int dest_idx = dest_y * dest_w + dest_x;
            new_img->pixels[dest_idx] = pixel;
        }
    }
    
    /* Create Java Image object */
    JavaClass* image_class = jvm_load_class(jvm, "javax/microedition/lcdui/Image");
    if (!image_class) {
        free(new_img->pixels);
        free(new_img);
        return NATIVE_RETURN_NULL();
    }
    
    ensure_native_peer_field(image_class);
    JavaObject* image_obj = jvm_new_object(jvm, image_class);
    if (!image_obj) {
        free(new_img->pixels);
        free(new_img);
        return NATIVE_RETURN_NULL();
    }
    
    set_object_field_ref(image_obj, "nativePeer", (JavaObject*)new_img);
    
    GFX_DEBUG("Created region image: %dx%d (transform=%d)", dest_w, dest_h, transform);
    return NATIVE_RETURN_OBJECT(image_obj);
}

/* Image.createImage(Image source) - creates a copy */
static JavaValue native_image_createImage_copy(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* src_image_obj = (JavaObject*)args[0].ref;
    
    if (!src_image_obj) {
        return NATIVE_RETURN_NULL();
    }
    
    MidpImage* src_img = get_image_from_object(src_image_obj);
    if (!src_img) {
        return NATIVE_RETURN_NULL();
    }
    
    /* Create copy with transform=NONE */
    JavaValue region_args[6];
    region_args[0].ref = src_image_obj;
    region_args[1].i = 0;
    region_args[2].i = 0;
    region_args[3].i = src_img->width;
    region_args[4].i = src_img->height;
    region_args[5].i = SPRITE_TRANS_NONE;
    
    return native_image_createImage_region(jvm, thread, region_args, 6);
}

/* Image.createImage(InputStream stream) - load from stream
 * 
 * CRITICAL FIX: Previously this was a stub that returned a 1x1 placeholder image.
 * Now it properly reads all bytes from the InputStream and decodes the image.
 */
static JavaValue native_image_createImage_stream(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)arg_count;
    JavaObject* stream_obj = (JavaObject*)args[0].ref;
    
    if (!stream_obj) {
        GFX_DEBUG("createImage(InputStream): stream is NULL");
        return NATIVE_RETURN_NULL();
    }
    
    GFX_DEBUG("createImage(InputStream): reading from stream...");
    
    /* Get the stream's class */
    JavaClass* stream_class = stream_obj->header.clazz;
    if (!stream_class) {
        GFX_DEBUG("createImage(InputStream): stream has no class");
        return NATIVE_RETURN_NULL();
    }
    
    /* Find the read([BII)I method on the stream */
    JavaMethod* read_method = jvm_resolve_method(jvm, stream_class, "read", "([BII)I");
    bool has_read_3arg = (read_method != NULL);
    
    if (!read_method) {
        /* Fallback: try read([B)I */
        read_method = jvm_resolve_method(jvm, stream_class, "read", "([B)I");
        if (!read_method) {
            GFX_DEBUG("createImage(InputStream): no read method found on %s", 
                    stream_class->class_name ? stream_class->class_name : "?");
            return NATIVE_RETURN_NULL();
        }
    }
    
    /* Allocate a dynamic buffer to hold all image data */
    int buffer_capacity = 4096;
    int buffer_size = 0;
    uint8_t* image_data = (uint8_t*)malloc(buffer_capacity);
    if (!image_data) {
        GFX_DEBUG("createImage(InputStream): failed to allocate buffer");
        return NATIVE_RETURN_NULL();
    }
    
    /* Create a temporary byte array for reading chunks */
    JavaArray* chunk_array = jvm_new_array(jvm, T_BYTE, 4096, NULL);
    if (!chunk_array) {
        free(image_data);
        GFX_DEBUG("createImage(InputStream): failed to create chunk array");
        return NATIVE_RETURN_NULL();
    }
    
    /* Read all bytes from the stream */
    while (1) {
        /* Prepare arguments for read */
        JavaValue read_args[4];
        read_args[0].ref = stream_obj;       /* this */
        read_args[1].ref = (JavaObject*)chunk_array;  /* buffer */
        
        if (has_read_3arg) {
            read_args[2].i = 0;              /* offset */
            read_args[3].i = 4096;           /* length */
        }
        
        JavaValue read_result;
        int ret;
        
        if (has_read_3arg) {
            ret = execute_method(jvm, thread, read_method, read_args, &read_result);
        } else {
            /* read([B)I only takes 2 args */
            ret = execute_method(jvm, thread, read_method, read_args, &read_result);
        }
        
        if (ret != 0 || thread->pending_exception) {
            /* Method execution failed or exception thrown */
            GFX_DEBUG("createImage(InputStream): read() failed or threw exception");
            break;
        }
        
        jint bytes_read = read_result.i;
        
        if (bytes_read <= 0) {
            /* End of stream (-1) or no data (0) */
            break;
        }
        
        /* Grow buffer if needed */
        if (buffer_size + bytes_read > buffer_capacity) {
            buffer_capacity = buffer_capacity * 2 + bytes_read;
            uint8_t* new_data = (uint8_t*)realloc(image_data, buffer_capacity);
            if (!new_data) {
                free(image_data);
                GFX_DEBUG("createImage(InputStream): failed to grow buffer");
                return NATIVE_RETURN_NULL();
            }
            image_data = new_data;
        }
        
        /* Copy chunk to our buffer */
        uint8_t* chunk_data = (uint8_t*)array_data(chunk_array);
        memcpy(image_data + buffer_size, chunk_data, bytes_read);
        buffer_size += bytes_read;
        
        /* Safety limit: 16MB max */
        if (buffer_size > 16 * 1024 * 1024) {
            GFX_DEBUG("createImage(InputStream): image too large (>16MB)");
            free(image_data);
            return NATIVE_RETURN_NULL();
        }
    }
    
    GFX_DEBUG("createImage(InputStream): read %d bytes", buffer_size);
    
    /* Check if we got any data */
    if (buffer_size < 8) {
        GFX_DEBUG("createImage(InputStream): not enough data (%d bytes)", buffer_size);
        free(image_data);
        return NATIVE_RETURN_NULL();
    }
    
    /* Decode the image */
    MidpImage* img = midp_image_create_from_data(image_data, 0, buffer_size);
    free(image_data);
    
    if (!img) {
        GFX_DEBUG("createImage(InputStream): failed to decode image");
        return NATIVE_RETURN_NULL();
    }
    
    /* Create Java Image object */
    JavaClass* image_class = jvm_load_class(jvm, "javax/microedition/lcdui/Image");
    if (!image_class) {
        GFX_DEBUG("createImage(InputStream): failed to load Image class");
        midp_image_destroy(img);
        return NATIVE_RETURN_NULL();
    }
    
    ensure_native_peer_field(image_class);
    JavaObject* image_obj = jvm_new_object(jvm, image_class);
    if (!image_obj) {
        GFX_DEBUG("createImage(InputStream): failed to create Image object");
        midp_image_destroy(img);
        return NATIVE_RETURN_NULL();
    }
    
    set_object_field_ref(image_obj, "nativePeer", (JavaObject*)img);
    
    GFX_DEBUG("createImage(InputStream): SUCCESS - created %dx%d image", img->width, img->height);
    
    return NATIVE_RETURN_OBJECT(image_obj);
}

/* Image.getGraphics() - returns Graphics for mutable images */
static JavaValue native_image_getGraphics(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* image_obj = (JavaObject*)args[0].ref;
    
    /* ALWAYS log getGraphics calls */
    DISP_DEBUG("[getGraphics] CALLED on Image obj=%p", (void*)image_obj);
    
    
    if (!image_obj) {
        DISP_DEBUG("[getGraphics] ERROR: null image object, throwing NPE");
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_NULL();
    }
    
    MidpImage* img = get_image_from_object(image_obj);
    if (!img) {
        DISP_DEBUG("[getGraphics] ERROR: no native image for obj=%p", (void*)image_obj);
        return NATIVE_RETURN_NULL();
    }
    
    DISP_DEBUG("[getGraphics] native img=%p (%dx%d, mutable=%d, pixels=%p)",
            (void*)img, img->width, img->height, img->mutable, (void*)img->pixels);
    
    if (!img->mutable) {
        DISP_DEBUG("[getGraphics] ERROR: image is not mutable");
        /* IllegalStateException for immutable image */
        jvm_throw_by_name(jvm, "java/lang/IllegalStateException", "Image is not mutable");
        return NATIVE_RETURN_NULL();
    }
    
    /* Create a Graphics object for this image */
    MidpGraphics* gfx = midp_image_get_graphics(img);
    if (!gfx) {
        DISP_DEBUG("[getGraphics] ERROR: failed to create graphics context");
        return NATIVE_RETURN_NULL();
    }
    
    /* Diagnostic: log the image-graphics binding */
    DISP_DEBUG("[getGraphics] SUCCESS: Created gfx=%p with pixels=%p for %dx%d image", 
            (void*)gfx, (void*)gfx->pixels, img->width, img->height);
    
    
    /* Create a Java Graphics object */
    JavaClass* gfx_class = jvm_load_class(jvm, "javax/microedition/lcdui/Graphics");
    if (!gfx_class) {
        DISP_DEBUG("[getGraphics] ERROR: failed to load Graphics class");
        free(gfx);
        return NATIVE_RETURN_NULL();
    }
    
    /* Ensure class has space for nativePeer */
    ensure_native_peer_field(gfx_class);
    
    JavaObject* gfx_obj = jvm_new_object(jvm, gfx_class);
    if (!gfx_obj) {
        DISP_DEBUG("[getGraphics] ERROR: failed to create Graphics object");
        free(gfx);
        return NATIVE_RETURN_NULL();
    }
    
    /* Store the MidpGraphics* in the nativePeer field */
    set_object_field_ref(gfx_obj, "nativePeer", (JavaObject*)gfx);
    
    DISP_DEBUG("[getGraphics] Returning Graphics obj=%p (native gfx=%p)", 
            (void*)gfx_obj, (void*)gfx);
    
    
    return NATIVE_RETURN_OBJECT(gfx_obj);
}

/*
 * Graphics native methods
 */

static JavaValue native_graphics_setColor(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint rgb = args[1].i;
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    
    /*fprintf(stderr, "[Graphics] setColor(RGB): obj=%p, gfx=%p, RGB=0x%06X", 
            (void*)gfx_obj, (void*)gfx, rgb & 0xFFFFFF);
    */
    
    if (gfx) {
        midp_graphics_set_color(gfx, rgb, 255);
    }
    
    return NATIVE_RETURN_VOID();
}

/* setColor(int red, int green, int blue) - separate RGB components */
static JavaValue native_graphics_setColorRGB(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint red = args[1].i;
    jint green = args[2].i;
    jint blue = args[3].i;
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    
    jint rgb = ((red & 0xFF) << 16) | ((green & 0xFF) << 8) | (blue & 0xFF);
    
    /*fprintf(stderr, "[Graphics] setColor(R,G,B): obj=%p, gfx=%p, R=%d, G=%d, B=%d -> 0x%06X", 
            (void*)gfx_obj, (void*)gfx, red, green, blue, rgb);
    */
    
    if (gfx) {
        midp_graphics_set_color(gfx, rgb, 255);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_graphics_drawLine(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint x1 = args[1].i;
    jint y1 = args[2].i;
    jint x2 = args[3].i;
    jint y2 = args[4].i;
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    
    /*fprintf(stderr, "[Graphics] drawLine: gfx=%p, (%d,%d)->(%d,%d)", 
            (void*)gfx, x1, y1, x2, y2);
    */
    
    if (gfx) {
        midp_graphics_draw_line(gfx, x1, y1, x2, y2);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_graphics_fillRect(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    jint w = args[3].i;
    jint h = args[4].i;
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    
    /*fprintf(stderr, "[Graphics] fillRect: gfx=%p, (%d,%d) %dx%d", 
            (void*)gfx, x, y, w, h);
    */
    
    if (gfx) {
        midp_graphics_fill_rect(gfx, x, y, w, h);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_graphics_drawRect(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    jint w = args[3].i;
    jint h = args[4].i;
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    
    if (gfx) {
        midp_graphics_draw_rect(gfx, x, y, w, h);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_graphics_drawImage(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    JavaObject* image_obj = (JavaObject*)args[1].ref;
    jint x = args[2].i;
    jint y = args[3].i;
    jint anchor = args[4].i;
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    MidpImage* img = get_image_from_object(image_obj);
    
    if (!gfx || !img) {
        native_throw_npe(jvm, thread);
        return NATIVE_RETURN_VOID();
    }
    
    /* Diagnostic: track image-to-image and image-to-screen drawing */
    static int draw_count = 0;
    draw_count++;
    if (draw_count <= 200) {
        DISP_DEBUG("[GFX.drawImage] #%d: src_img=%p (%dx%d, pixels=%p), dst_gfx=%p (%dx%d, pixels=%p)",
                draw_count, (void*)img, img->width, img->height, (void*)img->pixels,
                (void*)gfx, gfx->width, gfx->height, (void*)gfx->pixels);
        DISP_DEBUG("[GFX.drawImage] #%d: pos=(%d,%d), anchor=%d, first_src_pixel=0x%08X",
                draw_count, x, y, anchor, img->pixels[0]);
        
    }
    
    midp_graphics_draw_image(gfx, img, x, y, anchor);
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_graphics_setClip(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    jint w = args[3].i;
    jint h = args[4].i;
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    
    /*fprintf(stderr, "[Graphics] setClip: (%d,%d) %dx%d, gfx=%p", x, y, w, h, (void*)gfx);
    */
    
    if (gfx) {
        /* Limit clip to available area (excluding soft buttons when not fullscreen) */
        /* Check if this is screen graphics */
        int max_height = gfx->height;
        if (!g_full_screen_mode && sdl_is_screen_graphics(gfx)) {
            /* Screen graphics - limit to area above soft buttons if commands exist */
            if (current_displayable_obj) {
                int commands_idx = find_field_index(current_displayable_obj, "commands");
                if (commands_idx >= 0) {
                    JavaArray* commands = (JavaArray*)current_displayable_obj->fields[commands_idx].ref;
                    if (commands && commands->length > 0) {
                        max_height = gfx->height - SOFT_BUTTON_HEIGHT;
                    }
                }
            }
        }
        
        /* Clamp the clip region */
        if (y + h > max_height) {
            h = max_height - y;
            if (h < 0) h = 0;
        }
        
        midp_graphics_set_clip(gfx, x, y, w, h);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_graphics_translate(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    
    if (gfx) {
        midp_graphics_translate(gfx, x, y);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_graphics_getTranslateX(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    return NATIVE_RETURN_INT(gfx ? gfx->translate_x : 0);
}

static JavaValue native_graphics_getTranslateY(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    return NATIVE_RETURN_INT(gfx ? gfx->translate_y : 0);
}

static JavaValue native_graphics_getClipX(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    return NATIVE_RETURN_INT(gfx ? gfx->clip_x : 0);
}

static JavaValue native_graphics_getClipY(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    return NATIVE_RETURN_INT(gfx ? gfx->clip_y : 0);
}

static JavaValue native_graphics_getClipWidth(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    return NATIVE_RETURN_INT(gfx ? gfx->clip_width : 0);
}

static JavaValue native_graphics_getClipHeight(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    return NATIVE_RETURN_INT(gfx ? gfx->clip_height : 0);
}

static JavaValue native_graphics_getColor(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    return NATIVE_RETURN_INT(gfx ? gfx->rgb_color : 0);
}

/* Forward declaration for get_font_from_object (defined in Font section below) */
static MidpFont* get_font_from_object(JavaObject* obj);

static JavaValue native_graphics_setFont(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    JavaObject* font_obj = (JavaObject*)args[1].ref;
    MidpFont* font = get_font_from_object(font_obj);
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    
    /*fprintf(stderr, "[Graphics] setFont: gfx=%p, font_obj=%p, font=%p", 
            (void*)gfx, (void*)font_obj, (void*)font);
    */
    
    if (gfx) {
        if (font) {
            /* Store font pointer as jint (hack but works) */
            gfx->font = (jint)(intptr_t)font;
        } else {
            /* COMPATIBILITY FIX: If font is NULL, use default font.
             * This happens when Font.getFont() returns NULL or game passes null.
             * J2ME games expect setFont(null) to work like setFont(Font.getDefaultFont()).
             */
            MidpFont* default_font = midp_font_get_default();
            gfx->font = (jint)(intptr_t)default_font;
            NATIVE_DEBUG("setFont(null) -> using default font");
        }
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_graphics_clipRect(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    jint w = args[3].i;
    jint h = args[4].i;
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    
    if (gfx) {
        midp_graphics_clip_rect(gfx, x, y, w, h);
        
        /* Limit clip to available area (excluding soft buttons when not fullscreen) */
        /* Check if this is screen graphics */
        if (!g_full_screen_mode && sdl_is_screen_graphics(gfx)) {
            if (current_displayable_obj) {
                int commands_idx = find_field_index(current_displayable_obj, "commands");
                if (commands_idx >= 0) {
                    JavaArray* commands = (JavaArray*)current_displayable_obj->fields[commands_idx].ref;
                    if (commands && commands->length > 0) {
                        int max_height = gfx->height - SOFT_BUTTON_HEIGHT;
                        if (gfx->clip_y + gfx->clip_height > max_height) {
                            gfx->clip_height = max_height - gfx->clip_y;
                            if (gfx->clip_height < 0) gfx->clip_height = 0;
                        }
                    }
                }
            }
        }
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_graphics_drawRoundRect(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    jint w = args[3].i;
    jint h = args[4].i;
    jint arc_w = args[5].i;
    jint arc_h = args[6].i;
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    
    if (gfx) {
        midp_graphics_draw_round_rect(gfx, x, y, w, h, arc_w, arc_h);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_graphics_fillRoundRect(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    jint w = args[3].i;
    jint h = args[4].i;
    jint arc_w = args[5].i;
    jint arc_h = args[6].i;
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    
    if (gfx) {
        midp_graphics_fill_round_rect(gfx, x, y, w, h, arc_w, arc_h);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_graphics_drawArc(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    jint w = args[3].i;
    jint h = args[4].i;
    jint start = args[5].i;
    jint arc = args[6].i;
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    
    if (gfx) {
        midp_graphics_draw_arc(gfx, x, y, w, h, start, arc);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_graphics_fillArc(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    jint w = args[3].i;
    jint h = args[4].i;
    jint start = args[5].i;
    jint arc = args[6].i;
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    
    if (gfx) {
        midp_graphics_fill_arc(gfx, x, y, w, h, start, arc);
    }
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_graphics_drawString(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    const char* str = native_get_string_utf8(jvm, args, 1);
    jint x = args[2].i;
    jint y = args[3].i;
    jint anchor = args[4].i;
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    
    /* Debug logging disabled for release */
    
    if (gfx && str) {
        midp_graphics_draw_string(gfx, str, x, y, anchor);
    }
    
    return NATIVE_RETURN_VOID();
}

/* Helper to apply transform to source coordinates */
static void transform_coord(int transform, int src_x, int src_y, int src_w, int src_h,
                           int *out_x, int *out_y) {
    switch (transform) {
        case SPRITE_TRANS_NONE:           /* No transform */
            *out_x = src_x;
            *out_y = src_y;
            break;
        case SPRITE_TRANS_MIRROR_ROT180:  /* Mirror vertical + rotate 180 */
            *out_x = src_w - 1 - src_x;
            *out_y = src_h - 1 - src_y;
            break;
        case SPRITE_TRANS_MIRROR:         /* Mirror vertical */
            *out_x = src_x;
            *out_y = src_h - 1 - src_y;
            break;
        case SPRITE_TRANS_ROT180:         /* Rotate 180 */
            *out_x = src_w - 1 - src_x;
            *out_y = src_h - 1 - src_y;
            break;
        case SPRITE_TRANS_MIRROR_ROT270:  /* Mirror + rotate 270 (90 CCW) */
            *out_x = src_h - 1 - src_y;
            *out_y = src_x;
            break;
        case SPRITE_TRANS_ROT90:          /* Rotate 90 CW */
            *out_x = src_h - 1 - src_y;
            *out_y = src_w - 1 - src_x;
            break;
        case SPRITE_TRANS_ROT270:         /* Rotate 270 CW (90 CCW) */
            *out_x = src_y;
            *out_y = src_x;
            break;
        case SPRITE_TRANS_MIRROR_ROT90:   /* Mirror + rotate 90 */
            *out_x = src_y;
            *out_y = src_w - 1 - src_x;
            break;
        default:
            *out_x = src_x;
            *out_y = src_y;
            break;
    }
}

/* Get output dimensions after transform */
static void get_transformed_size(int transform, int src_w, int src_h, int *out_w, int *out_h) {
    switch (transform) {
        case SPRITE_TRANS_ROT90:
        case SPRITE_TRANS_ROT270:
        case SPRITE_TRANS_MIRROR_ROT270:
        case SPRITE_TRANS_MIRROR_ROT90:
            *out_w = src_h;
            *out_h = src_w;
            break;
        default:
            *out_w = src_w;
            *out_h = src_h;
            break;
    }
}

/* Graphics.drawRegion(Image src, int x_src, int y_src, int width, int height, 
 *                    int transform, int x_dest, int y_dest, int anchor)
 * CRITICAL for game compatibility!
 * Delegate to midp_graphics_draw_region which has correct transform mapping.
 */
static JavaValue native_graphics_drawRegion(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    JavaObject* src_image_obj = (JavaObject*)args[1].ref;
    jint x_src = args[2].i;
    jint y_src = args[3].i;
    jint width = args[4].i;
    jint height = args[5].i;
    jint transform = args[6].i;
    jint x_dest = args[7].i;
    jint y_dest = args[8].i;
    jint anchor = args[9].i;
    
    if (!gfx_obj || !src_image_obj) {
        return NATIVE_RETURN_VOID();
    }
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    MidpImage* src_img = get_image_from_object(src_image_obj);
    
    if (!gfx || !src_img || !src_img->pixels) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Bounds check source region */
    if (x_src < 0 || y_src < 0 || width <= 0 || height <= 0) {
        return NATIVE_RETURN_VOID();
    }
    if (x_src + width > src_img->width || y_src + height > src_img->height) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Delegate to the correct implementation in graphics.c */
    midp_graphics_draw_region(gfx, src_img, x_src, y_src, width, height,
                               transform, x_dest, y_dest, anchor);
    
    return NATIVE_RETURN_VOID();
}

/* Graphics.drawImage(Image img, int x, int y, int anchor) - simplified version */
static JavaValue native_graphics_drawImage_anchor(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    JavaObject* image_obj = (JavaObject*)args[1].ref;
    jint x = args[2].i;
    jint y = args[3].i;
    jint anchor = args[4].i;
    
    if (!gfx_obj || !image_obj) {
        return NATIVE_RETURN_VOID();
    }
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    MidpImage* img = get_image_from_object(image_obj);
    
    if (!gfx || !img || !img->pixels) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Calculate anchor offset */
    int anchor_x = 0, anchor_y = 0;
    if (anchor & 0x02) anchor_x = -img->width;      /* RIGHT */
    else if (anchor & 0x01) anchor_x = -img->width/2; /* HCENTER */
    if (anchor & 0x20) anchor_y = -img->height;     /* BOTTOM */
    else if (anchor & 0x10) anchor_y = -img->height/2; /* VCENTER */
    
    int draw_x = x + anchor_x + gfx->translate_x;
    int draw_y = y + anchor_y + gfx->translate_y;
    
    /* Clip to graphics bounds and clip region */
    int clip_x1 = gfx->clip_x;
    int clip_y1 = gfx->clip_y;
    int clip_x2 = gfx->clip_x + gfx->clip_width;
    int clip_y2 = gfx->clip_y + gfx->clip_height;
    
    for (int py = 0; py < img->height; py++) {
        for (int px = 0; px < img->width; px++) {
            int dest_x = draw_x + px;
            int dest_y = draw_y + py;
            
            /* Clip check */
            if (dest_x < clip_x1 || dest_x >= clip_x2 ||
                dest_y < clip_y1 || dest_y >= clip_y2) continue;
            
            /* Bounds check */
            if (dest_x < 0 || dest_x >= gfx->width ||
                dest_y < 0 || dest_y >= gfx->height) continue;
            
            uint32_t pixel = img->pixels[py * img->width + px];
            uint8_t alpha = (pixel >> 24) & 0xFF;
            
            /* Skip transparent pixels */
            if (alpha == 0) continue;
            
            int dest_idx = dest_y * gfx->width + dest_x;
            
            /* Alpha blending */
            if (alpha == 255 || !img->alpha) {
                gfx->pixels[dest_idx] = pixel;
            } else {
                uint32_t dest_pixel = gfx->pixels[dest_idx];
                uint8_t dr = (dest_pixel >> 16) & 0xFF;
                uint8_t dg = (dest_pixel >> 8) & 0xFF;
                uint8_t db = dest_pixel & 0xFF;
                uint8_t sr = (pixel >> 16) & 0xFF;
                uint8_t sg = (pixel >> 8) & 0xFF;
                uint8_t sb = pixel & 0xFF;
                gfx->pixels[dest_idx] = (255 << 24) |
                    (((sr * alpha + dr * (255 - alpha)) / 255) << 16) |
                    (((sg * alpha + dg * (255 - alpha)) / 255) << 8) |
                    ((sb * alpha + db * (255 - alpha)) / 255);
            }
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Graphics.copyArea(int x_src, int y_src, int width, int height, int x_dest, int y_dest, int anchor) */
static JavaValue native_graphics_copyArea(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint x_src = args[1].i;
    jint y_src = args[2].i;
    jint width = args[3].i;
    jint height = args[4].i;
    jint x_dest = args[5].i;
    jint y_dest = args[6].i;
    jint anchor = args[7].i;
    
    if (!gfx_obj) {
        return NATIVE_RETURN_VOID();
    }
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    if (!gfx || !gfx->pixels) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Calculate anchor offset */
    int anchor_x = 0, anchor_y = 0;
    if (anchor & 0x02) anchor_x = -width;
    else if (anchor & 0x01) anchor_x = -width/2;
    if (anchor & 0x20) anchor_y = -height;
    else if (anchor & 0x10) anchor_y = -height/2;
    
    int dest_x = x_dest + anchor_x + gfx->translate_x;
    int dest_y = y_dest + anchor_y + gfx->translate_y;
    
    /* Copy region (need temp buffer for overlapping copy) */
    uint32_t* temp = (uint32_t*)malloc(width * height * sizeof(uint32_t));
    if (!temp) return NATIVE_RETURN_VOID();
    
    /* Copy source to temp */
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int sx = x_src + x;
            int sy = y_src + y;
            if (sx >= 0 && sx < gfx->width && sy >= 0 && sy < gfx->height) {
                temp[y * width + x] = gfx->pixels[sy * gfx->width + sx];
            } else {
                temp[y * width + x] = 0;
            }
        }
    }
    
    /* Copy temp to destination */
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int dx = dest_x + x;
            int dy = dest_y + y;
            if (dx >= 0 && dx < gfx->width && dy >= 0 && dy < gfx->height) {
                gfx->pixels[dy * gfx->width + dx] = temp[y * width + x];
            }
        }
    }
    
    free(temp);
    return NATIVE_RETURN_VOID();
}

/* Graphics.getDisplayColor(int color) - returns the color that will be displayed */
static JavaValue native_graphics_getDisplayColor(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    jint color = args[1].i;
    /* On color display, return the same color */
    return NATIVE_RETURN_INT(color);
}

/* Graphics.setStrokeStyle(int style) - SOLID=0, DOTTED=1 */
static JavaValue native_graphics_setStrokeStyle(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint style = args[1].i;
    
    if (!gfx_obj) return NATIVE_RETURN_VOID();
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    if (gfx) {
        gfx->stroke_style = style;
    }
    
    return NATIVE_RETURN_VOID();
}

/* Graphics.getStrokeStyle() */
static JavaValue native_graphics_getStrokeStyle(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    
    if (!gfx_obj) return NATIVE_RETURN_INT(0);
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    if (gfx) {
        return NATIVE_RETURN_INT(gfx->stroke_style);
    }
    
    return NATIVE_RETURN_INT(0);
}

/* Graphics.setGrayScale(int value) */
static JavaValue native_graphics_setGrayScale(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    jint value = args[1].i;
    
    if (!gfx_obj) return NATIVE_RETURN_VOID();
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    if (gfx) {
        /* Convert grayscale to RGB */
        uint8_t gray = value & 0xFF;
        gfx->rgb_color = 0xFF000000 | (gray << 16) | (gray << 8) | gray;
    }
    
    return NATIVE_RETURN_VOID();
}

/* Graphics.getGrayScale() */
static JavaValue native_graphics_getGrayScale(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    
    if (!gfx_obj) return NATIVE_RETURN_INT(0);
    
    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    if (gfx) {
        uint8_t r = (gfx->rgb_color >> 16) & 0xFF;
        uint8_t g = (gfx->rgb_color >> 8) & 0xFF;
        uint8_t b = gfx->rgb_color & 0xFF;
        /* ITU-R BT.601 luminance: 0.299*R + 0.587*G + 0.114*B, using integer math */
        int gray = (19595 * r + 38470 * g + 7471 * b + 32768) >> 16;
        if (gray > 255) gray = 255;
        return NATIVE_RETURN_INT(gray);
    }
    
    return NATIVE_RETURN_INT(0);
}

/*
 * Font native methods
 */

/* Helper to get MidpFont from Java Font object */
static MidpFont* get_font_from_object(JavaObject* obj) {
    if (!obj) {
        NATIVE_DEBUG("get_font_from_object: NULL object");
        return NULL;
    }
    
    /* Check if object has fields */
    if (!obj->header.clazz) {
        NATIVE_DEBUG("get_font_from_object: object has NULL class");
        return NULL;
    }
    
    JavaClass* clazz = obj->header.clazz;
    
    if (clazz->fields_count == 0) {
        NATIVE_DEBUG("get_font_from_object: class has no fields!");
        return NULL;
    }
    
    /* The MidpFont* is stored in the nativePeer field */
    MidpFont* font = (MidpFont*)get_object_field_ref(obj, "nativePeer");
    return font;
}

/* Cached default Font Java object */
static JavaObject* g_default_font_object = NULL;

/* Helper to create a Java Font object wrapping a MidpFont */
static JavaObject* create_font_object(JVM* jvm, MidpFont* font) {
    if (!font) return NULL;
    
    /* Get the Font class */
    JavaClass* font_class = jvm_load_class(jvm, "javax/microedition/lcdui/Font");
    if (!font_class) {
        NATIVE_DEBUG("Failed to load Font class");
        return NULL;
    }
    
    /* Ensure class has space for nativePeer */
    ensure_native_peer_field(font_class);
    
    /* Create Font object */
    JavaObject* font_obj = jvm_new_object(jvm, font_class);
    if (!font_obj) {
        NATIVE_DEBUG("Failed to create Font object");
        return NULL;
    }
    
    /* Store the MidpFont* in the nativePeer field */
    set_object_field_ref(font_obj, "nativePeer", (JavaObject*)font);
    
    NATIVE_DEBUG("Created Font object: %p, native: %p", (void*)font_obj, (void*)font);
    
    return font_obj;
}

static JavaValue native_font_getDefaultFont(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;
    
    NATIVE_DEBUG("getDefaultFont() called");
    
    /* Return cached default Font object */
    if (!g_default_font_object) {
        MidpFont* default_font = midp_font_get_default();
        g_default_font_object = create_font_object(jvm, default_font);
        /* Register as GC root to prevent collection */
        if (g_default_font_object) {
            gc_add_root(jvm, (void**)&g_default_font_object);
        }
    }
    
    return NATIVE_RETURN_OBJECT(g_default_font_object);
}

static JavaValue native_font_stringWidth(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* font_obj = (JavaObject*)args[0].ref;
    MidpFont* font = get_font_from_object(font_obj);
    const char* str = native_get_string_utf8(jvm, args, 1);
    
    return NATIVE_RETURN_INT(midp_font_string_width(font, str));
}

static JavaValue native_font_height(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* font_obj = (JavaObject*)args[0].ref;
    MidpFont* font = get_font_from_object(font_obj);
    
    NATIVE_DEBUG("getHeight() called, obj=%p, font=%p, height=%d", 
            (void*)font_obj, (void*)font, font ? font->height : 0);
    
    return NATIVE_RETURN_INT(midp_font_height(font));
}

/* Font.charWidth(char ch) */
static JavaValue native_font_charWidth(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* font_obj = (JavaObject*)args[0].ref;
    jchar ch = (jchar)args[1].i;
    
    MidpFont* font = get_font_from_object(font_obj);
    if (!font) return NATIVE_RETURN_INT(0);
    
    /* For bitmap fonts, character width is typically fixed or based on character */
    int width = midp_font_char_width(font, ch);
    return NATIVE_RETURN_INT(width);
}

/* Font.charsWidth(char[] ch, int offset, int length) */
static JavaValue native_font_charsWidth(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* font_obj = (JavaObject*)args[0].ref;
    JavaArray* char_array = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint length = args[3].i;
    
    MidpFont* font = get_font_from_object(font_obj);
    if (!font || !char_array) return NATIVE_RETURN_INT(0);
    
    /* Width includes inter-character spacing, consistent with stringWidth */
    return NATIVE_RETURN_INT(length > 0 ? length * (FONT_WIDTH + 1) - 1 : 0);
}

/* Font.substringWidth(String str, int offset, int len) */
static JavaValue native_font_substringWidth(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* font_obj = (JavaObject*)args[0].ref;
    JavaString* str = (JavaString*)args[1].ref;
    jint offset = args[2].i;
    jint len = args[3].i;
    
    MidpFont* font = get_font_from_object(font_obj);
    if (!font || !str) return NATIVE_RETURN_INT(0);
    
    /* Width includes inter-character spacing, consistent with stringWidth */
    return NATIVE_RETURN_INT(len > 0 ? len * (FONT_WIDTH + 1) - 1 : 0);
}

/* Font.getBaselinePosition() */
static JavaValue native_font_getBaselinePosition(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* font_obj = (JavaObject*)args[0].ref;
    MidpFont* font = get_font_from_object(font_obj);
    
    if (!font) return NATIVE_RETURN_INT(0);
    
    /* Baseline is typically near the bottom of the font height */
    return NATIVE_RETURN_INT(font->baseline);
}

/* Font.getFace() */
static JavaValue native_font_getFace(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* font_obj = (JavaObject*)args[0].ref;
    MidpFont* font = get_font_from_object(font_obj);
    
    if (!font) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(font->face);
}

/* Font.getStyle() */
static JavaValue native_font_getStyle(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* font_obj = (JavaObject*)args[0].ref;
    MidpFont* font = get_font_from_object(font_obj);
    
    if (!font) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(font->style);
}

/* Font.getSize() */
static JavaValue native_font_getSize(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* font_obj = (JavaObject*)args[0].ref;
    MidpFont* font = get_font_from_object(font_obj);
    
    if (!font) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(font->size);
}

/* Font.isPlain(), isBold(), isItalic(), isUnderlined() */
static JavaValue native_font_isBold(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* font_obj = (JavaObject*)args[0].ref;
    MidpFont* font = get_font_from_object(font_obj);
    
    if (!font) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT((font->style & FONT_STYLE_BOLD) != 0);
}

static JavaValue native_font_isItalic(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* font_obj = (JavaObject*)args[0].ref;
    MidpFont* font = get_font_from_object(font_obj);
    
    if (!font) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT((font->style & FONT_STYLE_ITALIC) != 0);
}

static JavaValue native_font_isUnderlined(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* font_obj = (JavaObject*)args[0].ref;
    MidpFont* font = get_font_from_object(font_obj);
    
    if (!font) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT((font->style & FONT_STYLE_UNDERLINED) != 0);
}

static JavaValue native_font_isPlain(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* font_obj = (JavaObject*)args[0].ref;
    MidpFont* font = get_font_from_object(font_obj);
    
    if (!font) return NATIVE_RETURN_INT(1);
    return NATIVE_RETURN_INT(font->style == FONT_STYLE_PLAIN);
}

static JavaValue native_font_getFont(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    jint face = args[0].i;
    jint style = args[1].i;
    jint size = args[2].i;
    
    NATIVE_DEBUG("getFont(%d, %d, %d) called", face, style, size);
    
    /* Get or create native font */
    MidpFont* font = midp_font_get(face, style, size);
    
    /* COMPATIBILITY FIX: Font.getFont() should NEVER return null.
     * If the requested font cannot be created, return the default font.
     * This is consistent with J2ME specification and prevents NPE in games.
     */
    if (!font) {
        NATIVE_DEBUG("getFont: failed to create font, using default");
        font = midp_font_get_default();
    }
    
    /* Create Java Font object to wrap it */
    JavaObject* font_obj = create_font_object(jvm, font);
    
    /* If still NULL, return the cached default font object */
    if (!font_obj) {
        NATIVE_DEBUG("getFont: failed to create Font object, returning default");
        return native_font_getDefaultFont(jvm, thread, args, arg_count);
    }
    
    return NATIVE_RETURN_OBJECT(font_obj);
}

/*
 * Initialize native methods
 */

void init_javax_microedition_lcdui_display(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"javax/microedition/lcdui/Display", "getDisplay", "(Ljavax/microedition/midlet/MIDlet;)Ljavax/microedition/lcdui/Display;", native_display_getDisplay},
        {"javax/microedition/lcdui/Display", "setCurrent", "(Ljavax/microedition/lcdui/Displayable;)V", native_display_setCurrent},
        {"javax/microedition/lcdui/Display", "getCurrent", "()Ljavax/microedition/lcdui/Displayable;", native_display_getCurrent},
        {"javax/microedition/lcdui/Display", "callSerially", "(Ljava/lang/Runnable;)V", native_display_callSerially},
        {"javax/microedition/lcdui/Display", "getWidth", "()I", native_display_getWidth},
        {"javax/microedition/lcdui/Display", "getHeight", "()I", native_display_getHeight},
        {"javax/microedition/lcdui/Display", "isColor", "()Z", native_display_isColor},
        {"javax/microedition/lcdui/Display", "numColors", "()I", native_display_numColors},
        {"javax/microedition/lcdui/Display", "numAlphaLevels", "()I", native_display_numAlphaLevels},
        {"javax/microedition/lcdui/Display", "vibrate", "(I)Z", native_display_vibrate},
        {"javax/microedition/lcdui/Display", "flashBacklight", "(I)Z", native_display_flashBacklight},
        {"javax/microedition/lcdui/Display", "getColor", "(I)I", native_display_getColor},
        {"javax/microedition/lcdui/Display", "getBestImageWidth", "(I)I", native_display_getBestImageWidth},
        {"javax/microedition/lcdui/Display", "getBestImageHeight", "(I)I", native_display_getBestImageHeight},
        {"javax/microedition/lcdui/Display", "getBorderStyle", "(Z)I", native_display_getBorderStyle},
        /* Canvas/Displayable methods */
        {"javax/microedition/lcdui/Canvas", "getWidth", "()I", native_display_getWidth},
        {"javax/microedition/lcdui/Canvas", "getHeight", "()I", native_canvas_getHeight},
        {"javax/microedition/lcdui/Canvas", "getGameAction", "(I)I", native_canvas_getGameAction},
        {"javax/microedition/lcdui/Canvas", "getKeyCode", "(I)I", native_canvas_getKeyCode},
        {"javax/microedition/lcdui/Canvas", "getKeyName", "(I)Ljava/lang/String;", native_canvas_getKeyName},
        {"javax/microedition/lcdui/Canvas", "hasPointerEvents", "()Z", native_canvas_hasPointerEvents},
        {"javax/microedition/lcdui/Canvas", "hasPointerMotionEvents", "()Z", native_canvas_hasPointerMotionEvents},
        {"javax/microedition/lcdui/Canvas", "hasRepeatEvents", "()Z", native_canvas_hasRepeatEvents},
        {"javax/microedition/lcdui/Canvas", "isDoubleBuffered", "()Z", native_canvas_isDoubleBuffered},
        {"javax/microedition/lcdui/Canvas", "setFullScreenMode", "(Z)V", native_canvas_setFullScreenMode},
        {"javax/microedition/lcdui/Canvas", "repaint", "()V", native_canvas_repaint},
        {"javax/microedition/lcdui/Canvas", "repaint", "(IIII)V", native_canvas_repaint_region},
        {"javax/microedition/lcdui/Canvas", "serviceRepaints", "()V", native_canvas_serviceRepaints},
        {"javax/microedition/lcdui/Displayable", "getWidth", "()I", native_display_getWidth},
        {"javax/microedition/lcdui/Displayable", "getHeight", "()I", native_display_getHeight},
        {"javax/microedition/lcdui/Displayable", "isShown", "()Z", native_displayable_isShown},
        {"javax/microedition/lcdui/Displayable", "repaint", "()V", native_canvas_repaint},
        {"javax/microedition/lcdui/Displayable", "repaint", "(IIII)V", native_canvas_repaint_region},
        {"javax/microedition/lcdui/Canvas", "isShown", "()Z", native_displayable_isShown},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

void init_javax_microedition_lcdui_game_gamecanvas(JVM* jvm) {
    NativeMethodEntry methods[] = {
        /* GameCanvas */
        {"javax/microedition/lcdui/game/GameCanvas", "getKeyStates", "()I", native_gamecanvas_getKeyStates},
        {"javax/microedition/lcdui/game/GameCanvas", "suppressKeyEvents", "(Z)V", native_gamecanvas_suppressKeyEvents},
        {"javax/microedition/lcdui/game/GameCanvas", "getGraphics", "()Ljavax/microedition/lcdui/Graphics;", native_gamecanvas_getGraphics},
        {"javax/microedition/lcdui/game/GameCanvas", "flushGraphics", "()V", native_gamecanvas_flushGraphics},
        {"javax/microedition/lcdui/game/GameCanvas", "flushGraphics", "(IIII)V", native_gamecanvas_flushGraphicsRegion},
        
        /* Sprite constructors */
        {"javax/microedition/lcdui/game/Sprite", "<init>", "(Ljavax/microedition/lcdui/Image;)V", native_sprite_init_image},
        {"javax/microedition/lcdui/game/Sprite", "<init>", "(Ljavax/microedition/lcdui/Image;II)V", native_sprite_init_frames},
        {"javax/microedition/lcdui/game/Sprite", "<init>", "(Ljavax/microedition/lcdui/game/Sprite;)V", native_sprite_init_copy},
        
        /* Sprite methods */
        {"javax/microedition/lcdui/game/Sprite", "setPosition", "(II)V", native_sprite_setPosition},
        {"javax/microedition/lcdui/game/Sprite", "move", "(II)V", native_sprite_move},
        {"javax/microedition/lcdui/game/Sprite", "paint", "(Ljavax/microedition/lcdui/Graphics;)V", native_sprite_paint},
        {"javax/microedition/lcdui/game/Sprite", "getX", "()I", native_sprite_getX},
        {"javax/microedition/lcdui/game/Sprite", "getY", "()I", native_sprite_getY},
        {"javax/microedition/lcdui/game/Sprite", "getWidth", "()I", native_sprite_getWidth},
        {"javax/microedition/lcdui/game/Sprite", "getHeight", "()I", native_sprite_getHeight},
        {"javax/microedition/lcdui/game/Sprite", "setVisible", "(Z)V", native_sprite_setVisible},
        {"javax/microedition/lcdui/game/Sprite", "isVisible", "()Z", native_sprite_isVisible},
        {"javax/microedition/lcdui/game/Sprite", "setFrame", "(I)V", native_sprite_setFrame},
        {"javax/microedition/lcdui/game/Sprite", "getFrame", "()I", native_sprite_getFrame},
        /* CRITICAL: Sprite collision and transform methods */
        {"javax/microedition/lcdui/game/Sprite", "setTransform", "(I)V", native_sprite_setTransform},
        {"javax/microedition/lcdui/game/Sprite", "getTransform", "()I", native_sprite_getTransform},
        {"javax/microedition/lcdui/game/Sprite", "collidesWith", "(Ljavax/microedition/lcdui/game/Sprite;Z)Z", native_sprite_collidesWith_sprite},
        {"javax/microedition/lcdui/game/Sprite", "collidesWith", "(Ljavax/microedition/lcdui/game/TiledLayer;Z)Z", native_sprite_collidesWith_tiledlayer},
        {"javax/microedition/lcdui/game/Sprite", "collidesWith", "(Ljavax/microedition/lcdui/Image;IIZ)Z", native_sprite_collidesWith_image},
        {"javax/microedition/lcdui/game/Sprite", "defineCollisionRectangle", "(IIII)V", native_sprite_defineCollisionRectangle},
        {"javax/microedition/lcdui/game/Sprite", "setFrameSequence", "([I)V", native_sprite_setFrameSequence},
        {"javax/microedition/lcdui/game/Sprite", "nextFrame", "()V", native_sprite_nextFrame},
        {"javax/microedition/lcdui/game/Sprite", "prevFrame", "()V", native_sprite_prevFrame},
        {"javax/microedition/lcdui/game/Sprite", "getRawFrameCount", "()I", native_sprite_getRawFrameCount},
        {"javax/microedition/lcdui/game/Sprite", "getFrameSequenceLength", "()I", native_sprite_getFrameSequenceLength},
        {"javax/microedition/lcdui/game/Sprite", "defineReferencePixel", "(II)V", native_sprite_defineReferencePixel},
        {"javax/microedition/lcdui/game/Sprite", "setRefPixelPosition", "(II)V", native_sprite_setRefPixelPosition},
        {"javax/microedition/lcdui/game/Sprite", "getRefPixelX", "()I", native_sprite_getRefPixelX},
        {"javax/microedition/lcdui/game/Sprite", "getRefPixelY", "()I", native_sprite_getRefPixelY},
        {"javax/microedition/lcdui/game/Sprite", "setImage", "(Ljavax/microedition/lcdui/Image;II)V", native_sprite_setImage},
        
        /* Layer methods (inherited by Sprite and TiledLayer) */
        {"javax/microedition/lcdui/game/Layer", "getX", "()I", native_layer_getX},
        {"javax/microedition/lcdui/game/Layer", "getY", "()I", native_layer_getY},
        {"javax/microedition/lcdui/game/Layer", "getWidth", "()I", native_layer_getWidth},
        {"javax/microedition/lcdui/game/Layer", "getHeight", "()I", native_layer_getHeight},
        {"javax/microedition/lcdui/game/Layer", "setPosition", "(II)V", native_layer_setPosition},
        {"javax/microedition/lcdui/game/Layer", "move", "(II)V", native_layer_move},
        {"javax/microedition/lcdui/game/Layer", "setVisible", "(Z)V", native_layer_setVisible},
        {"javax/microedition/lcdui/game/Layer", "isVisible", "()Z", native_layer_isVisible},
        
        /* LayerManager */
        {"javax/microedition/lcdui/game/LayerManager", "<init>", "()V", native_layermanager_init},
        {"javax/microedition/lcdui/game/LayerManager", "append", "(Ljavax/microedition/lcdui/game/Layer;)I", native_layermanager_append},
        {"javax/microedition/lcdui/game/LayerManager", "insert", "(Ljavax/microedition/lcdui/game/Layer;I)V", native_layermanager_insert},
        {"javax/microedition/lcdui/game/LayerManager", "remove", "(Ljavax/microedition/lcdui/game/Layer;)V", native_layermanager_remove},
        {"javax/microedition/lcdui/game/LayerManager", "getLayerAt", "(I)Ljavax/microedition/lcdui/game/Layer;", native_layermanager_getLayerAt},
        {"javax/microedition/lcdui/game/LayerManager", "getSize", "()I", native_layermanager_getSize},
        {"javax/microedition/lcdui/game/LayerManager", "paint", "(Ljavax/microedition/lcdui/Graphics;II)V", native_layermanager_paint},
        {"javax/microedition/lcdui/game/LayerManager", "setViewWindow", "(IIII)V", native_layermanager_setViewWindow},
        
        /* TiledLayer */
        {"javax/microedition/lcdui/game/TiledLayer", "<init>", "(IILjavax/microedition/lcdui/Image;II)V", native_tiledlayer_init},
        {"javax/microedition/lcdui/game/TiledLayer", "setCell", "(III)V", native_tiledlayer_setCell},
        {"javax/microedition/lcdui/game/TiledLayer", "getCell", "(II)I", native_tiledlayer_getCell},
        {"javax/microedition/lcdui/game/TiledLayer", "fillCells", "(IIIII)V", native_tiledlayer_fillCells},
        {"javax/microedition/lcdui/game/TiledLayer", "createAnimatedTile", "(I)I", native_tiledlayer_createAnimatedTile},
        {"javax/microedition/lcdui/game/TiledLayer", "setAnimatedTile", "(II)V", native_tiledlayer_setAnimatedTile},
        {"javax/microedition/lcdui/game/TiledLayer", "getAnimatedTile", "(I)I", native_tiledlayer_getAnimatedTile},
        {"javax/microedition/lcdui/game/TiledLayer", "getColumns", "()I", native_tiledlayer_getColumns},
        {"javax/microedition/lcdui/game/TiledLayer", "getRows", "()I", native_tiledlayer_getRows},
        {"javax/microedition/lcdui/game/TiledLayer", "getCellWidth", "()I", native_tiledlayer_getCellWidth},
        {"javax/microedition/lcdui/game/TiledLayer", "getCellHeight", "()I", native_tiledlayer_getCellHeight},
        {"javax/microedition/lcdui/game/TiledLayer", "paint", "(Ljavax/microedition/lcdui/Graphics;)V", native_tiledlayer_paint},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

void init_javax_microedition_lcdui_image(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"javax/microedition/lcdui/Image", "createImage", "(II)Ljavax/microedition/lcdui/Image;", native_image_createImage},
        {"javax/microedition/lcdui/Image", "createImage", "(Ljava/lang/String;)Ljavax/microedition/lcdui/Image;", native_image_createImage_from_string},
        {"javax/microedition/lcdui/Image", "createImage", "([BII)Ljavax/microedition/lcdui/Image;", native_image_createImage_from_bytes},
        /* CRITICAL: createImage with transform and from Image source */
        {"javax/microedition/lcdui/Image", "createImage", "(Ljavax/microedition/lcdui/Image;IIIII)Ljavax/microedition/lcdui/Image;", native_image_createImage_region},
        {"javax/microedition/lcdui/Image", "createImage", "(Ljavax/microedition/lcdui/Image;)Ljavax/microedition/lcdui/Image;", native_image_createImage_copy},
        {"javax/microedition/lcdui/Image", "createImage", "(Ljava/io/InputStream;)Ljavax/microedition/lcdui/Image;", native_image_createImage_stream},
        {"javax/microedition/lcdui/Image", "createRGBImage", "([IIIZ)Ljavax/microedition/lcdui/Image;", native_image_createRGBImage},
        {"javax/microedition/lcdui/Image", "getWidth", "()I", native_image_getWidth},
        {"javax/microedition/lcdui/Image", "getHeight", "()I", native_image_getHeight},
        {"javax/microedition/lcdui/Image", "isMutable", "()Z", native_image_isMutable},
        {"javax/microedition/lcdui/Image", "getRGB", "([IIIIII)V", native_image_getRGB},
        {"javax/microedition/lcdui/Image", "getGraphics", "()Ljavax/microedition/lcdui/Graphics;", native_image_getGraphics},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

/* Graphics.drawRGB(int[] rgbData, int offset, int scanlength, int x, int y, int width, int height, boolean processAlpha) */
static JavaValue native_graphics_drawRGB(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* gfx_obj = (JavaObject*)args[0].ref;
    JavaArray* rgb_array = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint scanlength = args[3].i;
    jint x = args[4].i;
    jint y = args[5].i;
    jint width = args[6].i;
    jint height = args[7].i;
    jint processAlpha = args[8].i;

    if (!gfx_obj) return NATIVE_RETURN_VOID();

    MidpGraphics* gfx = get_graphics_from_object(gfx_obj);
    if (!gfx) return NATIVE_RETURN_VOID();

    if (!rgb_array || width <= 0 || height <= 0) return NATIVE_RETURN_VOID();

    jint* rgb_data = (jint*)array_data(rgb_array);
    if (!rgb_data) return NATIVE_RETURN_VOID();

    /* Clip once OUTSIDE the loops (matching Java FreeJ2ME reference) */
    int startX = 0, startY = 0, endX = width, endY = height;
    int tx = x + gfx->translate_x;
    int ty = y + gfx->translate_y;

    if (tx < gfx->clip_x) { startX = gfx->clip_x - tx; tx = gfx->clip_x; }
    if (ty < gfx->clip_y) { startY = gfx->clip_y - ty; ty = gfx->clip_y; }
    if (tx + width > gfx->clip_x + gfx->clip_width) endX = gfx->clip_x + gfx->clip_width - tx;
    if (ty + height > gfx->clip_y + gfx->clip_height) endY = gfx->clip_y + gfx->clip_height - ty;
    if (tx < 0) { startX = -tx; tx = 0; }
    if (ty < 0) { startY = -ty; ty = 0; }
    if (tx + width > gfx->width) endX = gfx->width - tx;
    if (ty + height > gfx->height) endY = gfx->height - ty;

    if (endX <= startX || endY <= startY) return NATIVE_RETURN_VOID();

    int visibleWidth = endX - startX;
    int visibleHeight = endY - startY;
    int dstStride = gfx->width;
    int dstStart = ty * dstStride + tx;

    if (!processAlpha) {
        /* Fast path: no alpha, force opaque, direct buffer write */
        for (int py = 0; py < visibleHeight; py++) {
            int srcRow = offset + (startY + py) * scanlength + startX;
            int dstRow = dstStart + py * dstStride;
            for (int px = 0; px < visibleWidth; px++) {
                gfx->pixels[dstRow + px] = (uint32_t)(0xFF000000 | (rgb_data[srcRow + px] & 0x00FFFFFF));
            }
        }
    } else {
        /* Alpha blending path */
        for (int py = 0; py < visibleHeight; py++) {
            int srcRow = offset + (startY + py) * scanlength + startX;
            int dstRow = dstStart + py * dstStride;
            for (int px = 0; px < visibleWidth; px++) {
                int src = rgb_data[srcRow + px];
                int srcA = (src >> 24) & 0xFF;

                if (srcA == 0) continue;          /* Fully transparent, skip */

                if (srcA == 0xFF) {
                    /* Fully opaque, fast copy */
                    gfx->pixels[dstRow + px] = (uint32_t)src;
                    continue;
                }

                /* Partial alpha: blend */
                int invSrcA = 255 - srcA;
                int dst = gfx->pixels[dstRow + px];
                int dstA = (dst >> 24) & 0xFF;

                int outA = srcA + ((dstA * invSrcA) >> 8);
                if (outA == 0) continue;

                int srcR = (src >> 16) & 0xFF;
                int srcG = (src >> 8) & 0xFF;
                int srcB = src & 0xFF;
                int dstR = (dst >> 16) & 0xFF;
                int dstG = (dst >> 8) & 0xFF;
                int dstB = dst & 0xFF;

                int r = ((srcR * srcA) + ((dstR * dstA * invSrcA) >> 8)) / outA;
                int g = ((srcG * srcA) + ((dstG * dstA * invSrcA) >> 8)) / outA;
                int b = ((srcB * srcA) + ((dstB * dstA * invSrcA) >> 8)) / outA;

                /* Clamp to 255 */
                if (r > 255) r = 255;
                if (g > 255) g = 255;
                if (b > 255) b = 255;

                gfx->pixels[dstRow + px] = (uint32_t)((outA << 24) | (r << 16) | (g << 8) | b);
            }
        }
    }

    return NATIVE_RETURN_VOID();
}

void init_javax_microedition_lcdui_graphics(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"javax/microedition/lcdui/Graphics", "setColor", "(I)V", native_graphics_setColor},
        {"javax/microedition/lcdui/Graphics", "setColor", "(III)V", native_graphics_setColorRGB},
        {"javax/microedition/lcdui/Graphics", "getColor", "()I", native_graphics_getColor},
        {"javax/microedition/lcdui/Graphics", "setFont", "(Ljavax/microedition/lcdui/Font;)V", native_graphics_setFont},
        {"javax/microedition/lcdui/Graphics", "drawLine", "(IIII)V", native_graphics_drawLine},
        {"javax/microedition/lcdui/Graphics", "fillRect", "(IIII)V", native_graphics_fillRect},
        {"javax/microedition/lcdui/Graphics", "drawRect", "(IIII)V", native_graphics_drawRect},
        {"javax/microedition/lcdui/Graphics", "fillRoundRect", "(IIIIII)V", native_graphics_fillRoundRect},
        {"javax/microedition/lcdui/Graphics", "drawRoundRect", "(IIIIII)V", native_graphics_drawRoundRect},
        {"javax/microedition/lcdui/Graphics", "fillArc", "(IIIIII)V", native_graphics_fillArc},
        {"javax/microedition/lcdui/Graphics", "drawArc", "(IIIIII)V", native_graphics_drawArc},
        {"javax/microedition/lcdui/Graphics", "drawString", "(Ljava/lang/String;III)V", native_graphics_drawString},
        {"javax/microedition/lcdui/Graphics", "drawImage", "(Ljavax/microedition/lcdui/Image;III)V", native_graphics_drawImage},
        /* CRITICAL: drawRegion - used by most games for sprite rendering */
        {"javax/microedition/lcdui/Graphics", "drawRegion", "(Ljavax/microedition/lcdui/Image;IIIIIIII)V", native_graphics_drawRegion},
        /* Additional drawing methods */
        {"javax/microedition/lcdui/Graphics", "drawImage", "(Ljavax/microedition/lcdui/Image;IIII)V", native_graphics_drawImage_anchor},
        {"javax/microedition/lcdui/Graphics", "copyArea", "(IIIIIII)V", native_graphics_copyArea},
        /* Clip methods */
        {"javax/microedition/lcdui/Graphics", "setClip", "(IIII)V", native_graphics_setClip},
        {"javax/microedition/lcdui/Graphics", "clipRect", "(IIII)V", native_graphics_clipRect},
        {"javax/microedition/lcdui/Graphics", "getClipX", "()I", native_graphics_getClipX},
        {"javax/microedition/lcdui/Graphics", "getClipY", "()I", native_graphics_getClipY},
        {"javax/microedition/lcdui/Graphics", "getClipWidth", "()I", native_graphics_getClipWidth},
        {"javax/microedition/lcdui/Graphics", "getClipHeight", "()I", native_graphics_getClipHeight},
        /* Translate methods */
        {"javax/microedition/lcdui/Graphics", "translate", "(II)V", native_graphics_translate},
        {"javax/microedition/lcdui/Graphics", "getTranslateX", "()I", native_graphics_getTranslateX},
        {"javax/microedition/lcdui/Graphics", "getTranslateY", "()I", native_graphics_getTranslateY},
        /* Stroke style */
        {"javax/microedition/lcdui/Graphics", "setStrokeStyle", "(I)V", native_graphics_setStrokeStyle},
        {"javax/microedition/lcdui/Graphics", "getStrokeStyle", "()I", native_graphics_getStrokeStyle},
        /* Gray scale */
        {"javax/microedition/lcdui/Graphics", "setGrayScale", "(I)V", native_graphics_setGrayScale},
        {"javax/microedition/lcdui/Graphics", "getGrayScale", "()I", native_graphics_getGrayScale},
        /* Display color */
        {"javax/microedition/lcdui/Graphics", "getDisplayColor", "(I)I", native_graphics_getDisplayColor},
        /* drawRGB - pixel-level rendering */
        {"javax/microedition/lcdui/Graphics", "drawRGB", "([IIIIIZ)V", native_graphics_drawRGB},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

void init_javax_microedition_lcdui_font(JVM* jvm) {
    NativeMethodEntry methods[] = {
        {"javax/microedition/lcdui/Font", "getDefaultFont", "()Ljavax/microedition/lcdui/Font;", native_font_getDefaultFont},
        {"javax/microedition/lcdui/Font", "getFont", "(III)Ljavax/microedition/lcdui/Font;", native_font_getFont},
        {"javax/microedition/lcdui/Font", "stringWidth", "(Ljava/lang/String;)I", native_font_stringWidth},
        {"javax/microedition/lcdui/Font", "getHeight", "()I", native_font_height},
        /* Additional Font methods */
        {"javax/microedition/lcdui/Font", "charWidth", "(C)I", native_font_charWidth},
        {"javax/microedition/lcdui/Font", "charsWidth", "([CII)I", native_font_charsWidth},
        {"javax/microedition/lcdui/Font", "substringWidth", "(Ljava/lang/String;II)I", native_font_substringWidth},
        {"javax/microedition/lcdui/Font", "getBaselinePosition", "()I", native_font_getBaselinePosition},
        {"javax/microedition/lcdui/Font", "getFace", "()I", native_font_getFace},
        {"javax/microedition/lcdui/Font", "getStyle", "()I", native_font_getStyle},
        {"javax/microedition/lcdui/Font", "getSize", "()I", native_font_getSize},
        {"javax/microedition/lcdui/Font", "isBold", "()Z", native_font_isBold},
        {"javax/microedition/lcdui/Font", "isItalic", "()Z", native_font_isItalic},
        {"javax/microedition/lcdui/Font", "isUnderlined", "()Z", native_font_isUnderlined},
        {"javax/microedition/lcdui/Font", "isPlain", "()Z", native_font_isPlain},
    };
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
}

