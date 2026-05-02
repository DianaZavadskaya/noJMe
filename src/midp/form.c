/*
 * J2ME Emulator - Form and UI Components
 * MIDP2 high-level UI implementation
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include "debug.h"
#include "debug_macros.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

#include "midp.h"
#include "jvm.h"
#include "native.h"
#include "heap.h"
#include "sdl_backend.h"
#include "threads.h"
#include "opcodes.h"  /* For T_BOOLEAN, DESC_OBJECT */

/* Forward declaration */
extern int execute_method(JVM* jvm, JavaThread* thread, JavaMethod* method, JavaValue* args, JavaValue* result);
extern MidpGraphics* get_graphics_from_object(JavaObject* obj);

/* Forward declaration: find_object_field_slot (defined below, used by render functions) */
static int find_object_field_slot(JavaObject* obj, const char* field_name);

/* Forward declaration for SELECT_COMMAND helper */
static void ensure_select_command(JVM* jvm);

/* Helper function: Get field offset by name, considering inheritance
 * Returns the absolute field index, or -1 if not found.
 * Fields are laid out: superclass fields first, then subclass fields.
 */
static int get_field_offset(JavaClass* clazz, const char* field_name) {
    if (!clazz || !field_name) return -1;
    
    /* Build class hierarchy from Object to actual class */
    JavaClass* hierarchy[64];
    int depth = 0;
    JavaClass* c = clazz;
    while (c && depth < 64) {
        hierarchy[depth++] = c;
        c = c->super_class;
    }
    
    /* Search for field from Object down to actual class */
    int slot = 0;
    for (int h = depth - 1; h >= 0; h--) {
        JavaClass* current = hierarchy[h];
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

/* Current form state */
static JavaObject* g_current_form = NULL;
static JavaObject* g_current_displayable = NULL;  /* Current List, Form, TextBox, etc. */
static int g_focused_item_index = 0;
static int g_list_selected_index = 0;  /* For List navigation */
static int g_cg_focused_element = 0;  /* Focused element index within current ChoiceGroup */
static JavaObject* g_select_command = NULL;  /* List.SELECT_COMMAND singleton */
static JavaObject* g_item_state_listener = NULL;  /* ItemStateListener for current form */

/* Item types */
#define ITEM_STRINGITEM   1
#define ITEM_TEXTFIELD    2
#define ITEM_IMAGEITEM    3
#define ITEM_CHOICEGROUP  4
#define ITEM_GAUGE        5
#define ITEM_SPACER       6
#define ITEM_DATEFIELD    7

/* Choice constants */
#define CHOICE_EXCLUSIVE  1
#define CHOICE_MULTIPLE   2
#define CHOICE_IMPLICIT   3
#define CHOICE_POPUP      4

/* TextField constraints */
#define TEXTFIELD_ANY        0
#define TEXTFIELD_EMAILADDR  1
#define TEXTFIELD_NUMERIC    2
#define TEXTFIELD_PHONENUMBER 3
#define TEXTFIELD_URL        4
#define TEXTFIELD_DECIMAL    5
#define TEXTFIELD_PASSWORD   0x10000

/* ============================================
 * VIRTUAL KEYBOARD FOR TEXT INPUT
 * Simple grid-based character selection
 * ============================================ */

/* Virtual keyboard character set - English letters, numbers, symbols */
static const char* VK_CHARACTERS = 
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"  /* 26 letters */
    "0123456789"                   /* 10 digits */
    " .,!?@_-:/"                   /* 10 symbols */
    "<DEL>"                        /* Delete - special */
    "<OK>";                        /* Confirm - special */

#define VK_CHARS_PER_ROW 10
#define VK_CHAR_COUNT    48  /* 26 + 10 + 10 + DEL + OK */

/* Virtual keyboard state */
static struct {
    bool active;              /* Is keyboard visible? */
    int selected_char;        /* Currently selected character index (0-47) */
    char text_buffer[256];    /* Current text being entered */
    int text_length;          /* Length of text */
    int cursor_pos;           /* Cursor position in text */
    int max_length;           /* Maximum allowed text length */
    int constraints;          /* TextField constraints */
    JavaObject* target_item;  /* TextField or TextBox being edited */
    JavaObject* displayable;  /* Parent form/screen */
} g_vkb;

/* Initialize virtual keyboard for a text field */
static void vkb_start(JavaObject* item, int max_size, int constraints) {
    g_vkb.active = true;
    g_vkb.selected_char = 0;
    g_vkb.text_length = 0;
    g_vkb.cursor_pos = 0;
    g_vkb.max_length = max_size > 255 ? 255 : max_size;
    g_vkb.constraints = constraints;
    g_vkb.target_item = item;
    g_vkb.text_buffer[0] = '\0';
    
    /* If item has existing text, copy it */
    if (item) {
        int text_slot = find_object_field_slot(item, "text");
        if (text_slot >= 0 && OBJECT_HAS_FIELDS(item, text_slot + 1)) {
            JavaString* text_str = (JavaString*)item->fields[text_slot].ref;
            if (text_str && text_str->utf8) {
                strncpy(g_vkb.text_buffer, text_str->utf8, 255);
                g_vkb.text_buffer[255] = '\0';
                g_vkb.text_length = strlen(g_vkb.text_buffer);
                g_vkb.cursor_pos = g_vkb.text_length;
            }
        }
    }
    
    FORM_DEBUG("[VKB] Started: max=%d, constraints=%d", max_size, constraints);
}

/* Close virtual keyboard and save text */
static void vkb_close(JVM* jvm, bool save) {
    if (save && g_vkb.target_item && jvm) {
        /* Create Java String from text buffer */
        JavaString* text_str = jvm_new_string(jvm, g_vkb.text_buffer);
        if (text_str) {
            int text_slot = find_object_field_slot(g_vkb.target_item, "text");
            if (text_slot >= 0 && OBJECT_HAS_FIELDS(g_vkb.target_item, text_slot + 1)) {
                g_vkb.target_item->fields[text_slot].ref = text_str;
                FORM_DEBUG("[VKB] Saved text: '%s'", g_vkb.text_buffer);
                
                /* Notify ItemStateListener about text change */
                if (g_item_state_listener && g_item_state_listener->header.clazz) {
                    JavaMethod* ism = jvm_resolve_method(jvm, g_item_state_listener->header.clazz,
                        "itemStateChanged", "(Ljavax/microedition/lcdui/Item;)V");
                    if (ism) {
                        JavaValue ism_args[2];
                        ism_args[0].ref = g_item_state_listener;
                        ism_args[1].ref = g_vkb.target_item;
                        JavaValue ism_result;
                        execute_method(jvm, jvm_current_thread(jvm), ism, ism_args, &ism_result);
                    }
                }
            }
        }
    }
    
    g_vkb.active = false;
    g_vkb.target_item = NULL;
    
    /* Repaint the form */
    if (g_current_displayable) {
        /* Trigger repaint */
        FORM_DEBUG("[VKB] Closed, should repaint");
    }
}

/* Handle key press in virtual keyboard mode */
static bool vkb_handle_key(JVM* jvm, int game_action) {
    if (!g_vkb.active) return false;
    
    switch (game_action) {
        case 1:  /* UP */
            g_vkb.selected_char -= VK_CHARS_PER_ROW;
            if (g_vkb.selected_char < 0) {
                g_vkb.selected_char += VK_CHAR_COUNT;
            }
            break;
            
        case 6:  /* DOWN */
            g_vkb.selected_char += VK_CHARS_PER_ROW;
            if (g_vkb.selected_char >= VK_CHAR_COUNT) {
                g_vkb.selected_char -= VK_CHAR_COUNT;
            }
            break;
            
        case 2:  /* LEFT */
            g_vkb.selected_char--;
            if (g_vkb.selected_char < 0) {
                g_vkb.selected_char = VK_CHAR_COUNT - 1;
            }
            break;
            
        case 5:  /* RIGHT */
            g_vkb.selected_char++;
            if (g_vkb.selected_char >= VK_CHAR_COUNT) {
                g_vkb.selected_char = 0;
            }
            break;
            
        case 8:  /* FIRE - select character */
            {
                int idx = g_vkb.selected_char;
                
                if (idx >= 46) {  /* <OK> - index 47 */
                    vkb_close(jvm, true);
                } else if (idx >= 45) {  /* <DEL> - index 46 */
                    if (g_vkb.text_length > 0 && g_vkb.cursor_pos > 0) {
                        /* Delete character before cursor */
                        memmove(&g_vkb.text_buffer[g_vkb.cursor_pos - 1],
                                &g_vkb.text_buffer[g_vkb.cursor_pos],
                                g_vkb.text_length - g_vkb.cursor_pos + 1);
                        g_vkb.text_length--;
                        g_vkb.cursor_pos--;
                    }
                } else if (idx < 26) {  /* Letter A-Z */
                    if (g_vkb.text_length < g_vkb.max_length) {
                        memmove(&g_vkb.text_buffer[g_vkb.cursor_pos + 1],
                                &g_vkb.text_buffer[g_vkb.cursor_pos],
                                g_vkb.text_length - g_vkb.cursor_pos + 1);
                        g_vkb.text_buffer[g_vkb.cursor_pos] = 'A' + idx;
                        g_vkb.text_length++;
                        g_vkb.cursor_pos++;
                    }
                } else if (idx < 36) {  /* Digit 0-9 */
                    if (g_vkb.text_length < g_vkb.max_length) {
                        memmove(&g_vkb.text_buffer[g_vkb.cursor_pos + 1],
                                &g_vkb.text_buffer[g_vkb.cursor_pos],
                                g_vkb.text_length - g_vkb.cursor_pos + 1);
                        g_vkb.text_buffer[g_vkb.cursor_pos] = '0' + (idx - 26);
                        g_vkb.text_length++;
                        g_vkb.cursor_pos++;
                    }
                } else {  /* Symbol (36-45) */
                    static const char symbols[] = " .,!?@_-:/";
                    int sym_idx = idx - 36;
                    if (sym_idx < 10 && g_vkb.text_length < g_vkb.max_length) {
                        memmove(&g_vkb.text_buffer[g_vkb.cursor_pos + 1],
                                &g_vkb.text_buffer[g_vkb.cursor_pos],
                                g_vkb.text_length - g_vkb.cursor_pos + 1);
                        g_vkb.text_buffer[g_vkb.cursor_pos] = symbols[sym_idx];
                        g_vkb.text_length++;
                        g_vkb.cursor_pos++;
                    }
                }
            }
            break;
            
        default:
            return false;
    }
    
    return true;  /* Key was handled */
}

/* Render virtual keyboard */
static void vkb_render(MidpGraphics* gfx) {
    if (!g_vkb.active || !gfx) return;
    
    int screen_w = gfx->width;
    int screen_h = gfx->height;
    
    /* Keyboard area - bottom half of screen */
    int kb_y = screen_h - 160;
    int kb_h = 150;
    
    /* Draw keyboard background */
    midp_graphics_set_color(gfx, 0xD0D0D0, 255);
    midp_graphics_fill_rect(gfx, 0, kb_y, screen_w, kb_h);
    
    /* Draw keyboard border */
    midp_graphics_set_color(gfx, 0x000000, 255);
    midp_graphics_draw_rect(gfx, 0, kb_y, screen_w, kb_h);
    
    /* Draw text field */
    int text_y = kb_y + 5;
    midp_graphics_set_color(gfx, 0xFFFFFF, 255);
    midp_graphics_fill_rect(gfx, 5, text_y, screen_w - 10, 20);
    midp_graphics_set_color(gfx, 0x000000, 255);
    midp_graphics_draw_rect(gfx, 5, text_y, screen_w - 10, 20);
    
    /* Draw current text */
    midp_graphics_set_color(gfx, 0x000000, 255);
    midp_graphics_draw_string(gfx, g_vkb.text_buffer, 8, text_y + 3, 0);
    
    /* Draw cursor */
    char temp[256];
    strncpy(temp, g_vkb.text_buffer, g_vkb.cursor_pos);
    temp[g_vkb.cursor_pos] = '\0';
    int cursor_x = 8 + strlen(temp) * 6;  /* Approximate char width */
    midp_graphics_draw_line(gfx, cursor_x, text_y + 2, cursor_x, text_y + 18);
    
    /* Draw character grid */
    int grid_y = text_y + 25;
    int cell_w = (screen_w - 10) / VK_CHARS_PER_ROW;
    int cell_h = 16;
    
    for (int i = 0; i < VK_CHAR_COUNT; i++) {
        int row = i / VK_CHARS_PER_ROW;
        int col = i % VK_CHARS_PER_ROW;
        int cell_x = 5 + col * cell_w;
        int cell_y = grid_y + row * cell_h;
        
        /* Draw selection highlight */
        if (i == g_vkb.selected_char) {
            midp_graphics_set_color(gfx, 0x000080, 255);
            midp_graphics_fill_rect(gfx, cell_x, cell_y, cell_w - 2, cell_h - 2);
            midp_graphics_set_color(gfx, 0xFFFFFF, 255);
        } else {
            midp_graphics_set_color(gfx, 0x000000, 255);
        }
        
        /* Draw character */
        char ch_str[8];
        if (i < 26) {
            ch_str[0] = 'A' + i;
            ch_str[1] = '\0';
        } else if (i < 36) {
            ch_str[0] = '0' + (i - 26);
            ch_str[1] = '\0';
        } else if (i < 46) {
            static const char symbols[] = " .,!?@_-:/";
            ch_str[0] = symbols[i - 36];
            ch_str[1] = '\0';
        } else if (i == 46) {
            strcpy(ch_str, "DEL");
        } else {
            strcpy(ch_str, "OK");
        }
        
        midp_graphics_draw_string(gfx, ch_str, cell_x + 2, cell_y + 1, 0);
    }
    
    /* Draw instructions */
    midp_graphics_set_color(gfx, 0x404040, 255);
    midp_graphics_draw_string(gfx, "Arrows:Move Fire:Select", 5, kb_y + kb_h - 15, 0);
}

/* Check if virtual keyboard is active */
bool midp_is_vkb_active(void) {
    return g_vkb.active;
}

/* Process key event for virtual keyboard */
bool midp_vkb_process_key(JVM* jvm, int game_action) {
    return vkb_handle_key(jvm, game_action);
}

/* ============================================ */

/* Get MidpGraphics for screen */
static MidpGraphics* get_screen_graphics(void) {
    SdlContext* sdl_ctx = sdl_get_global_context();
    if (!sdl_ctx || !sdl_ctx->framebuffer) return NULL;
    
    static MidpGraphics gfx;
    midp_graphics_init(&gfx, sdl_ctx->framebuffer, sdl_ctx->width, sdl_ctx->height);
    return &gfx;
}

/* Get item type from object */
static int get_item_type(JavaObject* item) {
    if (!item || !item->header.clazz) return 0;
    
    const char* class_name = item->header.clazz->class_name;
    if (!class_name) return 0;
    
    if (strstr(class_name, "StringItem")) return ITEM_STRINGITEM;
    if (strstr(class_name, "TextField")) return ITEM_TEXTFIELD;
    if (strstr(class_name, "ImageItem")) return ITEM_IMAGEITEM;
    if (strstr(class_name, "ChoiceGroup")) return ITEM_CHOICEGROUP;
    if (strstr(class_name, "Gauge")) return ITEM_GAUGE;
    if (strstr(class_name, "Spacer")) return ITEM_SPACER;
    if (strstr(class_name, "DateField")) return ITEM_DATEFIELD;
    
    return 0;
}

/* Get string from Java String object - uses proper UTF-8 encoding */
static const char* get_string_from_object(JVM* jvm, JavaObject* str_obj) {
    if (!str_obj) return "";

    /* Validate this is actually a String object, not an array or other type */
    if (!is_heap_ptr_check(str_obj)) {
        WARN_LOG("[get_string_from_object] object %p is not in heap!", (void*)str_obj);
        return "";
    }
    
    /* Check GC header to verify it's a STRING or OBJECT type, not ARRAY */
    GCObjectHeader* gc_header = (GCObjectHeader*)str_obj - 1;
    if (gc_header->type == OBJ_TYPE_ARRAY) {
        WARN_LOG("[get_string_from_object] object %p is an ARRAY, not a String!", (void*)str_obj);
        return "";
    }

    JavaString* str = (JavaString*)str_obj;
    /* Use string_utf8() which has proper UTF-8 encoding and caching */
    const char* result = string_utf8(jvm, str);
    return result ? result : "";
}

/* Render StringItem */
static int render_stringitem(JVM* jvm, MidpGraphics* gfx, JavaObject* item, int y) {
    /* StringItem has: label (String), text (String) — use dynamic slot lookup */
    const char* label = "";
    const char* text = "";
    
    int label_slot = find_object_field_slot(item, "label");
    int text_slot = find_object_field_slot(item, "text");
    int max_slot = (label_slot > text_slot) ? label_slot : text_slot;
    
    if (label_slot >= 0 && text_slot >= 0 && OBJECT_HAS_FIELDS(item, max_slot + 1)) {
        JavaString* label_str = (JavaString*)item->fields[label_slot].ref;
        JavaString* text_str = (JavaString*)item->fields[text_slot].ref;
        
        if (label_str) label = get_string_from_object(jvm, (JavaObject*)label_str);
        if (text_str) text = get_string_from_object(jvm, (JavaObject*)text_str);
    }
    
    int font_height = 12; /* Default font height */
    MidpFont* font = midp_font_get_default();
    if (font) font_height = midp_font_height(font);
    
    /* Draw label */
    if (label && *label) {
        midp_graphics_set_color(gfx, 0x0000FF, 255); /* Blue for labels */
        midp_graphics_draw_string(gfx, label, 5, y, 0);
        y += font_height + 2;
    }
    
    /* Draw text */
    if (text && *text) {
        midp_graphics_set_color(gfx, 0x000000, 255); /* Black for text */
        midp_graphics_draw_string(gfx, text, 5, y, 0);
        y += font_height + 4;
    }
    
    return y;
}

/* Render TextField */
static int render_textfield(JVM* jvm, MidpGraphics* gfx, JavaObject* item, int y, bool focused) {
    const char* label = "";
    const char* text = "";
    int max_size = 32;
    int constraints = 0;
    
    /* TextField fields: label, text, maxSize, constraints — use dynamic slot lookup */
    int label_slot = find_object_field_slot(item, "label");
    int text_slot = find_object_field_slot(item, "text");
    int maxsize_slot = find_object_field_slot(item, "maxSize");
    int constraints_slot = find_object_field_slot(item, "constraints");
    
    if (label_slot >= 0 && text_slot >= 0 && maxsize_slot >= 0 && constraints_slot >= 0 &&
        OBJECT_HAS_FIELDS(item, constraints_slot + 1)) {
        JavaString* label_str = (JavaString*)item->fields[label_slot].ref;
        JavaString* text_str = (JavaString*)item->fields[text_slot].ref;
        
        if (label_str) label = get_string_from_object(jvm, (JavaObject*)label_str);
        if (text_str) text = get_string_from_object(jvm, (JavaObject*)text_str);
        
        max_size = item->fields[maxsize_slot].i;
        constraints = item->fields[constraints_slot].i;
    }
    
    int font_height = 12;
    MidpFont* font = midp_font_get_default();
    if (font) font_height = midp_font_height(font);
    
    int box_height = font_height + 8;
    
    /* Draw label */
    if (label && *label) {
        midp_graphics_set_color(gfx, 0x0000FF, 255);
        midp_graphics_draw_string(gfx, label, 5, y, 0);
        y += font_height + 2;
    }
    
    /* Draw text box */
    int box_y = y;
    /* ИСПРАВЛЕНО: Вычисляем ширину относительно экрана */
    int box_width = gfx->width - 10;  /* 5 пикселей отступ с каждой стороны */
    midp_graphics_set_color(gfx, 0xFFFFFF, 255); /* White background */
    midp_graphics_fill_rect(gfx, 5, box_y, box_width, box_height);
    midp_graphics_set_color(gfx, focused ? 0x0000FF : 0x808080, 255); /* Border */
    midp_graphics_draw_rect(gfx, 5, box_y, box_width, box_height);
    
    /* Draw text content */
    midp_graphics_set_color(gfx, 0x000000, 255);
    if (constraints & TEXTFIELD_PASSWORD) {
        /* Draw asterisks for password */
        int len = text ? strlen(text) : 0;
        char* stars = (char*)malloc(len + 1);
        if (stars) {
            memset(stars, '*', len);
            stars[len] = '\0';
            midp_graphics_draw_string(gfx, stars, 8, box_y + 3, 0);
            free(stars);
        }
    } else {
        if (text && *text) {
            midp_graphics_draw_string(gfx, text, 8, box_y + 3, 0);
        }
    }
    
    /* Draw cursor if focused */
    if (focused) {
        int cursor_x = 8;
        if (text) {
            cursor_x += midp_font_string_width(font, text);
        }
        midp_graphics_set_color(gfx, 0x000000, 255);
        midp_graphics_draw_line(gfx, cursor_x, box_y + 2, cursor_x, box_y + box_height - 2);
    }
    
    return y + box_height + 6;
}

/* Render ChoiceGroup */
static int render_choicegroup(JVM* jvm, MidpGraphics* gfx, JavaObject* item, int y, bool focused) {
    const char* label = "";
    int choice_type = CHOICE_EXCLUSIVE;
    int num_choices = 0;
    JavaArray* strings = NULL;
    JavaArray* selected = NULL;
    
    /* ChoiceGroup fields: label, choiceType, strings[], selected[] — use dynamic slot lookup */
    int label_slot = find_object_field_slot(item, "label");
    int choicetype_slot = find_object_field_slot(item, "choiceType");
    int strings_slot = find_object_field_slot(item, "strings");
    int selected_slot = find_object_field_slot(item, "selected");
    
    if (label_slot >= 0 && choicetype_slot >= 0 && strings_slot >= 0 && selected_slot >= 0 &&
        OBJECT_HAS_FIELDS(item, selected_slot + 1)) {
        JavaString* label_str = (JavaString*)item->fields[label_slot].ref;
        if (label_str) label = get_string_from_object(jvm, (JavaObject*)label_str);
        
        choice_type = item->fields[choicetype_slot].i;
        strings = (JavaArray*)item->fields[strings_slot].ref;
        selected = (JavaArray*)item->fields[selected_slot].ref;
    }
    
    int font_height = 12;
    MidpFont* font = midp_font_get_default();
    if (font) font_height = midp_font_height(font);
    
    /* Draw label */
    if (label && *label) {
        midp_graphics_set_color(gfx, 0x0000FF, 255);
        midp_graphics_draw_string(gfx, label, 5, y, 0);
        y += font_height + 4;
    }
    
    /* Draw choices */
    if (strings && strings->element_type == DESC_OBJECT) {
        num_choices = strings->length;
        void** strings_data = (void**)array_data(strings);
        jboolean* selected_data = selected ? (jboolean*)array_data(selected) : NULL;
        
        for (int i = 0; i < num_choices; i++) {
            JavaString* choice_str = (JavaString*)strings_data[i];
            const char* choice_text = choice_str ? get_string_from_object(jvm, (JavaObject*)choice_str) : "";
            bool is_selected = false;
            
            if (selected && selected->element_type == T_BOOLEAN) {
                is_selected = selected_data[i] != 0;
            }
            
            int check_x = 10;
            int check_y = y + font_height / 2;
            
            /* Draw checkbox/radio */
            midp_graphics_set_color(gfx, 0x000000, 255);
            
            if (choice_type == CHOICE_EXCLUSIVE || choice_type == CHOICE_POPUP) {
                /* Radio button - circle */
                midp_graphics_draw_arc(gfx, check_x - 5, check_y - 5, 10, 10, 0, 360);
                if (is_selected) {
                    midp_graphics_set_color(gfx, 0x000000, 255);
                    midp_graphics_fill_arc(gfx, check_x - 3, check_y - 3, 6, 6, 0, 360);
                }
            } else {
                /* Checkbox - square */
                midp_graphics_draw_rect(gfx, check_x - 5, check_y - 5, 10, 10);
                if (is_selected) {
                    /* Draw X */
                    midp_graphics_draw_line(gfx, check_x - 3, check_y - 3, check_x + 3, check_y + 3);
                    midp_graphics_draw_line(gfx, check_x - 3, check_y + 3, check_x + 3, check_y - 3);
                }
            }
            
            /* Draw choice text */
            midp_graphics_set_color(gfx, 0x000000, 255);
            midp_graphics_draw_string(gfx, choice_text, check_x + 10, y, 0);
            
            y += font_height + 4;
        }
    }
    
    return y + 4;
}

/* Render Gauge */
static int render_gauge(JVM* jvm, MidpGraphics* gfx, JavaObject* item, int y, bool focused) {
    const char* label = "";
    int max_value = 100;
    int current_value = 0;
    bool interactive = false;
    
    /* Gauge fields: label, interactive, maxValue, value — use dynamic slot lookup */
    int label_slot = find_object_field_slot(item, "label");
    int interactive_slot = find_object_field_slot(item, "interactive");
    int maxvalue_slot = find_object_field_slot(item, "maxValue");
    int value_slot = find_object_field_slot(item, "value");
    
    if (label_slot >= 0 && interactive_slot >= 0 && maxvalue_slot >= 0 && value_slot >= 0 &&
        OBJECT_HAS_FIELDS(item, value_slot + 1)) {
        JavaString* label_str = (JavaString*)item->fields[label_slot].ref;
        if (label_str) label = get_string_from_object(jvm, (JavaObject*)label_str);
        
        interactive = item->fields[interactive_slot].i != 0;
        max_value = item->fields[maxvalue_slot].i;
        current_value = item->fields[value_slot].i;
    }
    
    int font_height = 12;
    MidpFont* font = midp_font_get_default();
    if (font) font_height = midp_font_height(font);
    
    /* Draw label */
    if (label && *label) {
        midp_graphics_set_color(gfx, 0x0000FF, 255);
        midp_graphics_draw_string(gfx, label, 5, y, 0);
        y += font_height + 4;
    }
    
    int bar_y = y;
    int bar_height = 20;
    /* ИСПРАВЛЕНО: Вычисляем ширину относительно экрана */
    int bar_width = gfx->width - 20;  /* 10 пикселей отступ с каждой стороны */
    
    /* Draw bar background */
    midp_graphics_set_color(gfx, 0xC0C0C0, 255);
    midp_graphics_fill_rect(gfx, 10, bar_y, bar_width, bar_height);
    
    /* Draw filled portion */
    if (max_value > 0) {
        int fill_width = (current_value * bar_width) / max_value;
        midp_graphics_set_color(gfx, interactive ? 0x0000FF : 0x008000, 255);
        midp_graphics_fill_rect(gfx, 10, bar_y, fill_width, bar_height);
    }
    
    /* Draw border */
    midp_graphics_set_color(gfx, focused ? 0x0000FF : 0x000000, 255);
    midp_graphics_draw_rect(gfx, 10, bar_y, bar_width, bar_height);
    
    /* Draw value text */
    char val_text[32];
    snprintf(val_text, sizeof(val_text), "%d/%d", current_value, max_value);
    midp_graphics_set_color(gfx, 0x000000, 255);
    midp_graphics_draw_string(gfx, val_text, 100, bar_y + 4, 0x10); /* HCENTER */
    
    return y + bar_height + 8;
}

/* Render Spacer */
static int render_spacer(JVM* jvm, MidpGraphics* gfx, JavaObject* item, int y) {
    int width = 0, height = 0;
    
    /* Spacer fields: width, height — use dynamic slot lookup */
    int width_slot = find_object_field_slot(item, "width");
    int height_slot = find_object_field_slot(item, "height");
    if (width_slot >= 0 && height_slot >= 0 &&
        OBJECT_HAS_FIELDS(item, height_slot + 1)) {
        width = item->fields[width_slot].i;
        height = item->fields[height_slot].i;
    }
    
    if (height <= 0) height = 10;
    
    return y + height;
}

/* Render Alert */
static void render_alert(JVM* jvm, MidpGraphics* gfx, JavaObject* alert) {
    const char* title = "Alert";
    const char* text = "";
    int type = 0;
    
    /* Alert fields: title, text, type — use dynamic slot lookup */
    int title_slot = find_object_field_slot(alert, "title");
    int text_slot = find_object_field_slot(alert, "text");
    int type_slot = find_object_field_slot(alert, "alertType");
    int max_slot = title_slot;
    if (text_slot > max_slot) max_slot = text_slot;
    if (type_slot > max_slot) max_slot = type_slot;
    
    if (title_slot >= 0 && text_slot >= 0 && type_slot >= 0 &&
        OBJECT_HAS_FIELDS(alert, max_slot + 1)) {
        JavaString* title_str = (JavaString*)alert->fields[title_slot].ref;
        JavaString* text_str = (JavaString*)alert->fields[text_slot].ref;
        
        if (title_str) title = get_string_from_object(jvm, (JavaObject*)title_str);
        if (text_str) text = get_string_from_object(jvm, (JavaObject*)text_str);
        
        type = alert->fields[type_slot].i;
    }
    
    int screen_width = gfx->width;
    int screen_height = gfx->height;
    
    int box_width = screen_width - 20;
    int box_height = 100;
    int box_x = 10;
    int box_y = (screen_height - box_height) / 2;
    
    /* Draw background */
    midp_graphics_set_color(gfx, 0xFFFFCC, 255); /* Light yellow */
    midp_graphics_fill_rect(gfx, box_x, box_y, box_width, box_height);
    
    /* Draw border */
    midp_graphics_set_color(gfx, 0x000000, 255);
    midp_graphics_draw_rect(gfx, box_x, box_y, box_width, box_height);
    
    /* Draw title */
    midp_graphics_set_color(gfx, 0x0000FF, 255);
    midp_graphics_draw_string(gfx, title, screen_width / 2, box_y + 5, 0x10); /* HCENTER | TOP */
    
    /* Draw text */
    midp_graphics_set_color(gfx, 0x000000, 255);
    midp_graphics_draw_string(gfx, text, screen_width / 2, box_y + 30, 0x10);
    
    /* Draw OK button */
    int btn_width = 60;
    int btn_height = 25;
    int btn_x = (screen_width - btn_width) / 2;
    int btn_y = box_y + box_height - 35;
    
    midp_graphics_set_color(gfx, 0xC0C0C0, 255);
    midp_graphics_fill_rect(gfx, btn_x, btn_y, btn_width, btn_height);
    midp_graphics_set_color(gfx, 0x000000, 255);
    midp_graphics_draw_rect(gfx, btn_x, btn_y, btn_width, btn_height);
    midp_graphics_draw_string(gfx, "OK", screen_width / 2, btn_y + 5, 0x10);
}

/* Render List */
static void render_list(JVM* jvm, MidpGraphics* gfx, JavaObject* list, int focused_index) {
    const char* title = "";
    int list_type = CHOICE_IMPLICIT;
    JavaArray* strings = NULL;
    JavaArray* selected = NULL;
    
    FORM_DEBUG("[render_list] list=%p, clazz=%s, instance_size=%zu",
            (void*)list, list && list->header.clazz ? list->header.clazz->class_name : "NULL",
            list && list->header.clazz ? list->header.clazz->instance_size : 0);
    
    /* Get field offsets considering inheritance (List extends Screen extends Displayable) */
    JavaClass* list_class = list->header.clazz;
    int title_idx = get_field_offset(list_class, "title");
    int listtype_idx = get_field_offset(list_class, "listType");
    int strings_idx = get_field_offset(list_class, "strings");
    int selected_idx = get_field_offset(list_class, "selected");
    
    FORM_DEBUG("[render_list] Field offsets: title=%d, listType=%d, strings=%d, selected=%d",
            title_idx, listtype_idx, strings_idx, selected_idx);
    
    /* List fields: title, listType, strings[], selected[] */
    if (title_idx >= 0 && listtype_idx >= 0 && strings_idx >= 0 && selected_idx >= 0) {
        JavaString* title_str = (JavaString*)list->fields[title_idx].ref;
        if (title_str) title = get_string_from_object(jvm, (JavaObject*)title_str);
        
        list_type = list->fields[listtype_idx].i;
        strings = (JavaArray*)list->fields[strings_idx].ref;
        selected = (JavaArray*)list->fields[selected_idx].ref;
        
        FORM_DEBUG("[render_list] title=%s, type=%d, strings=%p (len=%d), selected=%p",
                title, list_type, (void*)strings, strings ? strings->length : -1, (void*)selected);
    } else {
        FORM_DEBUG("[render_list] Failed to get field offsets! Using defaults.");
    }
    
    int font_height = 12;
    MidpFont* font = midp_font_get_default();
    if (font) font_height = midp_font_height(font);
    
    int y = 30;
    
    /* Draw title */
    midp_graphics_set_color(gfx, 0x0000FF, 255);
    midp_graphics_draw_string(gfx, title, gfx->width / 2, 5, 0x10);
    
    /* Draw separator */
    midp_graphics_set_color(gfx, 0x808080, 255);
    midp_graphics_draw_line(gfx, 0, 25, gfx->width, 25);
    
    /* Draw items */
    if (strings && strings->element_type == DESC_OBJECT) {
        void** strings_data = (void**)array_data(strings);
        jboolean* selected_data = selected ? (jboolean*)array_data(selected) : NULL;
        
        for (int i = 0; i < strings->length; i++) {
            JavaString* item_str = (JavaString*)strings_data[i];
            const char* item_text = item_str ? get_string_from_object(jvm, (JavaObject*)item_str) : "";
            
            /* Highlight focused item */
            if (i == focused_index) {
                midp_graphics_set_color(gfx, 0x000080, 255);
                midp_graphics_fill_rect(gfx, 0, y - 2, gfx->width, font_height + 4);
                midp_graphics_set_color(gfx, 0xFFFFFF, 255);
            } else {
                midp_graphics_set_color(gfx, 0x000000, 255);
            }
            
            /* Draw selection indicator for EXCLUSIVE/MULTIPLE */
            int text_x = 5;
            if (list_type == CHOICE_EXCLUSIVE || list_type == CHOICE_MULTIPLE) {
                bool is_selected = false;
                if (selected && selected->element_type == T_BOOLEAN) {
                    is_selected = selected_data[i] != 0;
                }
                
                int check_x = 15;
                int check_y = y + font_height / 2;
                
                if (list_type == CHOICE_EXCLUSIVE) {
                    midp_graphics_draw_arc(gfx, check_x - 5, check_y - 5, 10, 10, 0, 360);
                    if (is_selected) {
                        midp_graphics_fill_arc(gfx, check_x - 3, check_y - 3, 6, 6, 0, 360);
                    }
                } else {
                    midp_graphics_draw_rect(gfx, check_x - 5, check_y - 5, 10, 10);
                    if (is_selected) {
                        midp_graphics_draw_line(gfx, check_x - 3, check_y - 3, check_x + 3, check_y + 3);
                        midp_graphics_draw_line(gfx, check_x - 3, check_y + 3, check_x + 3, check_y - 3);
                    }
                }
                text_x = 30;
            }
            
            midp_graphics_draw_string(gfx, item_text, text_x, y, 0);
            y += font_height + 6;
        }
    }
}

/* Render TextBox */
static void render_textbox(JVM* jvm, MidpGraphics* gfx, JavaObject* textbox, const char* input_text, int cursor_pos) {
    const char* title = "";
    int max_size = 32;
    int constraints = 0;
    
    /* TextBox fields: title, text, maxSize, constraints — use dynamic slot lookup */
    int title_slot = find_object_field_slot(textbox, "title");
    int maxsize_slot = find_object_field_slot(textbox, "maxSize");
    int constraints_slot = find_object_field_slot(textbox, "constraints");
    
    if (title_slot >= 0 && maxsize_slot >= 0 && constraints_slot >= 0 &&
        OBJECT_HAS_FIELDS(textbox, constraints_slot + 1)) {
        JavaString* title_str = (JavaString*)textbox->fields[title_slot].ref;
        if (title_str) title = get_string_from_object(jvm, (JavaObject*)title_str);
        
        max_size = textbox->fields[maxsize_slot].i;
        constraints = textbox->fields[constraints_slot].i;
    }
    
    int font_height = 12;
    MidpFont* font = midp_font_get_default();
    if (font) font_height = midp_font_height(font);
    
    /* Draw title bar */
    midp_graphics_set_color(gfx, 0x000080, 255);
    midp_graphics_fill_rect(gfx, 0, 0, gfx->width, 25);
    midp_graphics_set_color(gfx, 0xFFFFFF, 255);
    midp_graphics_draw_string(gfx, title, gfx->width / 2, 5, 0x10);
    
    /* Draw text area */
    int text_y = 30;
    int text_height = gfx->height - 80;
    
    midp_graphics_set_color(gfx, 0xFFFFFF, 255);
    midp_graphics_fill_rect(gfx, 5, text_y, gfx->width - 10, text_height);
    midp_graphics_set_color(gfx, 0x000000, 255);
    midp_graphics_draw_rect(gfx, 5, text_y, gfx->width - 10, text_height);
    
    /* Draw text content */
    midp_graphics_set_color(gfx, 0x000000, 255);
    if (input_text && *input_text) {
        if (constraints & TEXTFIELD_PASSWORD) {
            int len = strlen(input_text);
            char* stars = (char*)malloc(len + 1);
            if (stars) {
                memset(stars, '*', len);
                stars[len] = '\0';
                midp_graphics_draw_string(gfx, stars, 10, text_y + 5, 0);
                free(stars);
            }
        } else {
            midp_graphics_draw_string(gfx, input_text, 10, text_y + 5, 0);
        }
    }
    
    /* Draw cursor */
    if (cursor_pos >= 0) {
        int cursor_x = 10;
        if (input_text && cursor_pos > 0) {
            char* temp = (char*)malloc(cursor_pos + 1);
            if (temp) {
                strncpy(temp, input_text, cursor_pos);
                temp[cursor_pos] = '\0';
                cursor_x += midp_font_string_width(font, temp);
                free(temp);
            }
        }
        midp_graphics_draw_line(gfx, cursor_x, text_y + 5, cursor_x, text_y + font_height + 5);
    }
    
    /* Draw soft buttons */
    int btn_height = 25;
    midp_graphics_set_color(gfx, 0xC0C0C0, 255);
    midp_graphics_fill_rect(gfx, 0, gfx->height - btn_height, gfx->width, btn_height);
    midp_graphics_set_color(gfx, 0x000000, 255);
    midp_graphics_draw_line(gfx, 0, gfx->height - btn_height, gfx->width, gfx->height - btn_height);
    
    /* Cancel and OK buttons */
    midp_graphics_draw_string(gfx, "Cancel", 10, gfx->height - btn_height + 5, 0);
    midp_graphics_draw_string(gfx, "OK", gfx->width - 30, gfx->height - btn_height + 5, 0);
}

/*
 * Native method implementations
 */

/* Form.append(Item) */
static JavaValue native_form_append(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* form = (JavaObject*)args[0].ref;
    JavaObject* item = (JavaObject*)args[1].ref;
    
    if (!form || !item) {
        FORM_DEBUG("append: NULL form or item");
        return NATIVE_RETURN_INT(-1);
    }
    
    FORM_DEBUG("append: form=%p, item=%p (type=%d)", 
            (void*)form, (void*)item, get_item_type(item));
    
    /* Form fields: title, items — use dynamic slot lookup */
    int title_slot = find_object_field_slot(form, "title");
    int items_slot = find_object_field_slot(form, "items");
    if (title_slot < 0 || items_slot < 0 ||
        !OBJECT_HAS_FIELDS(form, items_slot + 1)) {
        FORM_DEBUG("append: Form field slots not found! title=%d, items=%d", title_slot, items_slot);
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Get current items array */
    JavaArray* items = (JavaArray*)form->fields[items_slot].ref;
    int old_count = items ? items->length : 0;
    int new_count = old_count + 1;
    
    /* Create new items array with one more slot */
    JavaArray* new_items = jvm_new_array(jvm, DESC_OBJECT, new_count, NULL);
    if (!new_items) {
        FORM_DEBUG("append: Failed to create new items array");
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Copy old items */
    if (items && items->length > 0) {
        void** old_data = (void**)array_data(items);
        void** new_data = (void**)array_data(new_items);
        for (int i = 0; i < old_count; i++) {
            new_data[i] = old_data[i];
        }
    }
    
    /* Add new item */
    void** new_data = (void**)array_data(new_items);
    new_data[old_count] = item;
    
    /* Update form's items array */
    form->fields[items_slot].ref = new_items;
    
    FORM_DEBUG("append: Added item %p at index %d, new count=%d",
            (void*)item, old_count, new_count);
    
    return NATIVE_RETURN_INT(old_count);  /* Return the index of the new item */
}

/* Form.delete(int) */
static JavaValue native_form_delete(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* form = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    
    FORM_DEBUG("delete: form=%p, index=%d", (void*)form, index);
    
    if (!form) {
        return NATIVE_RETURN_VOID();
    }
    
    int items_slot = find_object_field_slot(form, "items");
    if (items_slot < 0 || !OBJECT_HAS_FIELDS(form, items_slot + 1)) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Get current items array */
    JavaArray* items = (JavaArray*)form->fields[items_slot].ref;
    if (!items || items->length == 0) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Validate index */
    if (index < 0 || index >= items->length) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Create new array with one less element */
    int new_len = items->length - 1;
    if (new_len == 0) {
        /* Remove all items - set to empty array */
        form->fields[items_slot].ref = jvm_new_array(jvm, DESC_OBJECT, 0, NULL);
        return NATIVE_RETURN_VOID();
    }
    
    JavaArray* new_items = jvm_new_array(jvm, DESC_OBJECT, new_len, NULL);
    if (!new_items) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Copy items, skipping the deleted one */
    void** old_data = (void**)array_data(items);
    void** new_data = (void**)array_data(new_items);
    for (int i = 0; i < index; i++) {
        new_data[i] = old_data[i];
    }
    for (int i = index + 1; i < items->length; i++) {
        new_data[i - 1] = old_data[i];
    }
    
    /* Update form's items array */
    form->fields[items_slot].ref = new_items;
    
    FORM_DEBUG("delete: removed item at index %d, new size=%d", index, new_len);
    
    return NATIVE_RETURN_VOID();
}

/* Form.size() */
static JavaValue native_form_size(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* form = (JavaObject*)args[0].ref;
    
    /* Count items in the form */
    if (form) {
        int items_slot = find_object_field_slot(form, "items");
        if (items_slot >= 0 && OBJECT_HAS_FIELDS(form, items_slot + 1)) {
            JavaArray* items = (JavaArray*)form->fields[items_slot].ref;
            if (items) {
                return NATIVE_RETURN_INT(items->length);
            }
        }
    }
    
    return NATIVE_RETURN_INT(0);
}

/* Form.get(int) - returns the Item at the specified index */
static JavaValue native_form_get(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* form = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    
    if (!form) {
        return NATIVE_RETURN_NULL();
    }
    
    int items_slot = find_object_field_slot(form, "items");
    if (items_slot < 0 || !OBJECT_HAS_FIELDS(form, items_slot + 1)) {
        FORM_DEBUG("get: Form doesn't have enough fields!");
        return NATIVE_RETURN_NULL();
    }
    
    JavaArray* items = (JavaArray*)form->fields[items_slot].ref;
    if (!items || index < 0 || index >= (jint)items->length) {
        FORM_DEBUG("get: Invalid index %d or null items array", index);
        return NATIVE_RETURN_NULL();
    }
    
    /* Get item from array - items array stores JavaObject* pointers */
    void** item_ptrs = (void**)array_data(items);
    JavaObject* item = (JavaObject*)item_ptrs[index];
    
    FORM_DEBUG("get: Returning item at index %d = %p", index, (void*)item);
    return NATIVE_RETURN_OBJECT(item);
}

/* Form.getTitle() */
static JavaValue native_form_getTitle(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* form = (JavaObject*)args[0].ref;
    
    if (form) {
        int title_slot = find_object_field_slot(form, "title");
        if (title_slot >= 0 && OBJECT_HAS_FIELDS(form, title_slot + 1)) {
            JavaString* title = (JavaString*)form->fields[title_slot].ref;
            return NATIVE_RETURN_OBJECT(title);
        }
    }
    
    return NATIVE_RETURN_NULL();
}

/* Form.setTitle(String) */
static JavaValue native_form_setTitle(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* form = (JavaObject*)args[0].ref;
    JavaString* title = (JavaString*)args[1].ref;
    
    if (form) {
        int title_slot = find_object_field_slot(form, "title");
        if (title_slot >= 0 && OBJECT_HAS_FIELDS(form, title_slot + 1)) {
            form->fields[title_slot].ref = title;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Form.setItemStateListener(ItemStateListener) */
static JavaValue native_form_setItemStateListener(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* form = (JavaObject*)args[0].ref;
    JavaObject* listener = (JavaObject*)args[1].ref;
    (void)form;
    
    g_item_state_listener = listener;
    FORM_DEBUG("setItemStateListener: %p", (void*)listener);
    
    return NATIVE_RETURN_VOID();
}

/* StringItem.setText(String) */
static JavaValue native_stringitem_setText(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    JavaString* text = (JavaString*)args[1].ref;
    
    if (item) {
        int text_slot = find_object_field_slot(item, "text");
        if (text_slot >= 0 && OBJECT_HAS_FIELDS(item, text_slot + 1)) {
            item->fields[text_slot].ref = text; /* text field */
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* StringItem.getText() */
static JavaValue native_stringitem_getText(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    
    if (item) {
        int text_slot = find_object_field_slot(item, "text");
        if (text_slot >= 0 && OBJECT_HAS_FIELDS(item, text_slot + 1)) {
            return NATIVE_RETURN_OBJECT(item->fields[text_slot].ref);
        }
    }
    
    return NATIVE_RETURN_NULL();
}

/* TextField.setString(String) */
static JavaValue native_textfield_setString(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    JavaString* text = (JavaString*)args[1].ref;
    
    if (item) {
        int text_slot = find_object_field_slot(item, "text");
        if (text_slot >= 0 && OBJECT_HAS_FIELDS(item, text_slot + 1)) {
            item->fields[text_slot].ref = text;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* TextField.getString() */
static JavaValue native_textfield_getString(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    
    if (item) {
        int text_slot = find_object_field_slot(item, "text");
        if (text_slot >= 0 && OBJECT_HAS_FIELDS(item, text_slot + 1)) {
            return NATIVE_RETURN_OBJECT(item->fields[text_slot].ref);
        }
    }
    
    return NATIVE_RETURN_NULL();
}

/* TextField.setChars(char[], int, int) */
static JavaValue native_textfield_setChars(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    JavaArray* chars = (JavaArray*)args[1].ref;
    jint offset = args[2].i;
    jint length = args[3].i;
    
    if (item && chars && chars->element_type == T_CHAR) {
        /* Create new String from char array */
        jchar* chars_data = (jchar*)array_data(chars);
        JavaString* str = jvm_new_string_utf16(jvm, chars_data + offset, length);
        int text_slot = find_object_field_slot(item, "text");
        if (text_slot >= 0 && OBJECT_HAS_FIELDS(item, text_slot + 1)) {
            item->fields[text_slot].ref = str;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* TextField.size() */
static JavaValue native_textfield_size(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    
    if (item) {
        int text_slot = find_object_field_slot(item, "text");
        if (text_slot >= 0 && OBJECT_HAS_FIELDS(item, text_slot + 1)) {
            JavaString* text = (JavaString*)item->fields[text_slot].ref;
            if (text) {
                return NATIVE_RETURN_INT(text->length);
            }
        }
    }
    
    return NATIVE_RETURN_INT(0);
}

/* TextField.getMaxSize() */
static JavaValue native_textfield_getMaxSize(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    
    if (item) {
        int maxsize_slot = find_object_field_slot(item, "maxSize");
        if (maxsize_slot >= 0 && OBJECT_HAS_FIELDS(item, maxsize_slot + 1)) {
            return NATIVE_RETURN_INT(item->fields[maxsize_slot].i);
        }
    }
    
    return NATIVE_RETURN_INT(32);  /* Default */
}

/* TextField.setMaxSize(int) */
static JavaValue native_textfield_setMaxSize(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    jint max_size = args[1].i;
    
    if (item && max_size > 0) {
        int maxsize_slot = find_object_field_slot(item, "maxSize");
        if (maxsize_slot >= 0 && OBJECT_HAS_FIELDS(item, maxsize_slot + 1)) {
            item->fields[maxsize_slot].i = max_size;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* TextField.getConstraints() */
static JavaValue native_textfield_getConstraints(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    
    if (item) {
        int constraints_slot = find_object_field_slot(item, "constraints");
        if (constraints_slot >= 0 && OBJECT_HAS_FIELDS(item, constraints_slot + 1)) {
            return NATIVE_RETURN_INT(item->fields[constraints_slot].i);
        }
    }
    
    return NATIVE_RETURN_INT(0);  /* ANY */
}

/* TextField.setConstraints(int) */
static JavaValue native_textfield_setConstraints(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    jint constraints = args[1].i;
    
    if (item) {
        int constraints_slot = find_object_field_slot(item, "constraints");
        if (constraints_slot >= 0 && OBJECT_HAS_FIELDS(item, constraints_slot + 1)) {
            item->fields[constraints_slot].i = constraints;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* TextField.getChars(char[]) */
static JavaValue native_textfield_getChars(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    JavaArray* dest = (JavaArray*)args[1].ref;
    
    if (!item || !dest || dest->element_type != T_CHAR) {
        return NATIVE_RETURN_VOID();
    }
    
    int text_slot = find_object_field_slot(item, "text");
    if (text_slot >= 0 && OBJECT_HAS_FIELDS(item, text_slot + 1)) {
        JavaString* text = (JavaString*)item->fields[text_slot].ref;
        if (text) {
            const jchar* src_data = string_chars(text);
            jchar* dest_data = (jchar*)array_data(dest);
            int copy_len = text->length < dest->length ? text->length : dest->length;
            for (int i = 0; i < copy_len; i++) {
                dest_data[i] = src_data[i];
            }
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* TextField.delete(int offset, int length) */
static JavaValue native_textfield_delete(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    jint offset = args[1].i;
    jint length = args[2].i;
    
    if (!item) {
        return NATIVE_RETURN_VOID();
    }
    
    int text_slot = find_object_field_slot(item, "text");
    if (text_slot < 0 || !OBJECT_HAS_FIELDS(item, text_slot + 1)) {
        return NATIVE_RETURN_VOID();
    }
    
    JavaString* text = (JavaString*)item->fields[text_slot].ref;
    if (!text || offset < 0 || length <= 0 || offset >= text->length) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Adjust length if it goes beyond string */
    if (offset + length > text->length) {
        length = text->length - offset;
    }
    
    /* Create new string without the deleted portion */
    const jchar* old_data = string_chars(text);
    int new_len = text->length - length;
    
    JavaArray* new_chars = jvm_new_array(jvm, T_CHAR, new_len, NULL);
    if (!new_chars) return NATIVE_RETURN_VOID();
    
    jchar* new_data = (jchar*)array_data(new_chars);
    
    /* Copy before offset */
    for (int i = 0; i < offset; i++) {
        new_data[i] = old_data[i];
    }
    /* Copy after deleted portion */
    for (int i = offset + length; i < text->length; i++) {
        new_data[i - length] = old_data[i];
    }
    
    /* Create new string */
    JavaString* new_str = jvm_new_string_utf16(jvm, new_data, new_len);
    item->fields[text_slot].ref = new_str;
    
    return NATIVE_RETURN_VOID();
}

/* TextField.insert(String, int) */
static JavaValue native_textfield_insert(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    JavaString* insert_str = (JavaString*)args[1].ref;
    jint position = args[2].i;
    
    if (!item || !insert_str) {
        return NATIVE_RETURN_VOID();
    }
    
    int text_slot = find_object_field_slot(item, "text");
    int maxsize_slot = find_object_field_slot(item, "maxSize");
    if (text_slot < 0 || !OBJECT_HAS_FIELDS(item, text_slot + 1)) {
        return NATIVE_RETURN_VOID();
    }
    
    JavaString* text = (JavaString*)item->fields[text_slot].ref;
    int old_len = text ? text->length : 0;
    
    /* Check max size */
    int max_size = (maxsize_slot >= 0 && OBJECT_HAS_FIELDS(item, maxsize_slot + 1)) ?
                   item->fields[maxsize_slot].i : 32;
    if (old_len + insert_str->length > max_size) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Clamp position */
    if (position < 0) position = 0;
    if (position > old_len) position = old_len;
    
    /* Create new string */
    int new_len = old_len + insert_str->length;
    JavaArray* new_chars = jvm_new_array(jvm, T_CHAR, new_len, NULL);
    if (!new_chars) return NATIVE_RETURN_VOID();
    
    jchar* new_data = (jchar*)array_data(new_chars);
    const jchar* insert_data = string_chars(insert_str);
    const jchar* old_data = text ? string_chars(text) : NULL;
    
    /* Copy before position */
    for (int i = 0; i < position; i++) {
        new_data[i] = old_data ? old_data[i] : 0;
    }
    /* Copy inserted string */
    for (int i = 0; i < insert_str->length; i++) {
        new_data[position + i] = insert_data[i];
    }
    /* Copy after position */
    for (int i = position; i < old_len; i++) {
        new_data[i + insert_str->length] = old_data ? old_data[i] : 0;
    }
    
    /* Create new string and set it */
    JavaString* new_str = jvm_new_string_utf16(jvm, new_data, new_len);
    item->fields[text_slot].ref = new_str;
    
    return NATIVE_RETURN_VOID();
}

/* TextField.getCaretPosition() - returns current cursor position */
static JavaValue native_textfield_getCaretPosition(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    
    /* Return end of text as default caret position */
    if (item) {
        int text_slot = find_object_field_slot(item, "text");
        if (text_slot >= 0 && OBJECT_HAS_FIELDS(item, text_slot + 1)) {
            JavaString* text = (JavaString*)item->fields[text_slot].ref;
            if (text) {
                return NATIVE_RETURN_INT(text->length);
            }
        }
    }
    
    return NATIVE_RETURN_INT(0);
}

/* ChoiceGroup.append(String, Image) */
static JavaValue native_choicegroup_append(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* group = (JavaObject*)args[0].ref;
    JavaString* text = (JavaString*)args[1].ref;
    JavaObject* image = (JavaObject*)args[2].ref;
    (void)image;  /* Image not used in basic implementation */
    
    FORM_DEBUG("[ChoiceGroup] append: text=%s", 
            text ? get_string_from_object(jvm, (JavaObject*)text) : "(null)");
    
    if (!group) {
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Get field slots — use dynamic slot lookup */
    int strings_slot = find_object_field_slot(group, "strings");
    int selected_slot = find_object_field_slot(group, "selected");
    if (strings_slot < 0 || selected_slot < 0 ||
        !OBJECT_HAS_FIELDS(group, selected_slot + 1)) {
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Get current strings and selected arrays */
    JavaArray* strings = (JavaArray*)group->fields[strings_slot].ref;
    JavaArray* selected = (JavaArray*)group->fields[selected_slot].ref;
    
    int old_len = strings ? strings->length : 0;
    int new_len = old_len + 1;
    
    /* Create new strings array */
    JavaArray* new_strings = jvm_new_array(jvm, DESC_OBJECT, new_len, NULL);
    if (!new_strings) {
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Create new selected array */
    JavaArray* new_selected = jvm_new_array(jvm, T_BOOLEAN, new_len, NULL);
    if (!new_selected) {
        return NATIVE_RETURN_INT(-1);
    }
    
    /* Copy old strings */
    if (strings && strings->length > 0) {
        void** old_data = (void**)array_data(strings);
        void** new_data = (void**)array_data(new_strings);
        for (int i = 0; i < old_len; i++) {
            new_data[i] = old_data[i];
        }
    }
    
    /* Copy old selected */
    if (selected && selected->length > 0) {
        jboolean* old_sel = (jboolean*)array_data(selected);
        jboolean* new_sel = (jboolean*)array_data(new_selected);
        for (int i = 0; i < old_len; i++) {
            new_sel[i] = old_sel[i];
        }
    }
    
    /* Add new string at the end */
    void** new_str_data = (void**)array_data(new_strings);
    new_str_data[old_len] = text;
    
    /* Initialize new selected as false */
    jboolean* new_sel_data = (jboolean*)array_data(new_selected);
    new_sel_data[old_len] = JNI_FALSE;
    
    /* Update group's arrays */
    group->fields[strings_slot].ref = new_strings;
    group->fields[selected_slot].ref = new_selected;
    
    FORM_DEBUG("[ChoiceGroup] append: added at index %d, new size=%d", old_len, new_len);
    
    return NATIVE_RETURN_INT(old_len);  /* Return index of new element */
}

/* ChoiceGroup.setSelectedIndex(int, boolean) */
static JavaValue native_choicegroup_setSelectedIndex(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* group = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    jboolean selected = args[2].i;
    
    FORM_DEBUG("[ChoiceGroup] setSelectedIndex: index=%d, selected=%d", index, selected);
    
    if (!group) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Get field slots — use dynamic slot lookup */
    int choicetype_slot = find_object_field_slot(group, "choiceType");
    int selected_slot = find_object_field_slot(group, "selected");
    if (choicetype_slot < 0 || selected_slot < 0 ||
        !OBJECT_HAS_FIELDS(group, selected_slot + 1)) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Get selected array */
    JavaArray* selected_arr = (JavaArray*)group->fields[selected_slot].ref;
    if (!selected_arr || selected_arr->element_type != T_BOOLEAN) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Validate index */
    if (index < 0 || index >= selected_arr->length) {
        return NATIVE_RETURN_VOID();
    }
    
    /* For EXCLUSIVE choice type, clear all other selections first */
    jint choice_type = group->fields[choicetype_slot].i;
    if (choice_type == 1) {  /* Choice.EXCLUSIVE = 1 */
        jboolean* sel_data = (jboolean*)array_data(selected_arr);
        for (int i = 0; i < selected_arr->length; i++) {
            sel_data[i] = JNI_FALSE;
        }
    }
    
    /* Set the selected index */
    jboolean* sel_data = (jboolean*)array_data(selected_arr);
    sel_data[index] = selected;
    
    FORM_DEBUG("[ChoiceGroup] setSelectedIndex: set index %d to %d", index, selected);
    
    return NATIVE_RETURN_VOID();
}

/* ChoiceGroup.getSelectedIndex() */
static JavaValue native_choicegroup_getSelectedIndex(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* group = (JavaObject*)args[0].ref;
    
    /* Return first selected index */
    if (group) {
        int selected_slot = find_object_field_slot(group, "selected");
        if (selected_slot >= 0 && OBJECT_HAS_FIELDS(group, selected_slot + 1)) {
            JavaArray* selected = (JavaArray*)group->fields[selected_slot].ref;
            if (selected && selected->element_type == T_BOOLEAN) {
                jboolean* selected_data = (jboolean*)array_data(selected);
                for (int i = 0; i < selected->length; i++) {
                    if (selected_data[i]) {
                        return NATIVE_RETURN_INT(i);
                    }
                }
            }
        }
    }
    
    return NATIVE_RETURN_INT(-1);
}

/* Gauge.setValue(int) */
static JavaValue native_gauge_setValue(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* gauge = (JavaObject*)args[0].ref;
    jint value = args[1].i;
    
    if (gauge) {
        int value_slot = find_object_field_slot(gauge, "value");
        if (value_slot >= 0 && OBJECT_HAS_FIELDS(gauge, value_slot + 1)) {
            jint old_value = gauge->fields[value_slot].i;
            gauge->fields[value_slot].i = value;
            
            /* Notify ItemStateListener if value changed */
            if (old_value != value && g_item_state_listener) {
                JavaClass* listener_class = g_item_state_listener->header.clazz;
                JavaMethod* ism = jvm_resolve_method(jvm, listener_class, "itemStateChanged", "(Ljavax/microedition/lcdui/Item;)V");
                if (ism) {
                    JavaValue listener_args[2];
                    listener_args[0].ref = g_item_state_listener;
                    listener_args[1].ref = gauge;
                    JavaValue result;
                    JavaThread* th = jvm_current_thread(jvm);
                    execute_method(jvm, th, ism, listener_args, &result);
                }
            }
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Gauge.getValue() */
static JavaValue native_gauge_getValue(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* gauge = (JavaObject*)args[0].ref;
    
    FORM_DEBUG("[Gauge] getValue: gauge=%p", (void*)gauge);
    
    if (gauge) {
        int value_slot = find_object_field_slot(gauge, "value");
        if (value_slot >= 0 && OBJECT_HAS_FIELDS(gauge, value_slot + 1)) {
            FORM_DEBUG("[Gauge] getValue: fields[%d].i = %d", value_slot, gauge->fields[value_slot].i);
            return NATIVE_RETURN_INT(gauge->fields[value_slot].i);
        }
    }
    
    FORM_DEBUG("[Gauge] getValue: returning 0 (not enough fields)");
    return NATIVE_RETURN_INT(0);
}

/* List.append(String, Image) */
static JavaValue native_list_append(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* list = (JavaObject*)args[0].ref;
    JavaString* text = (JavaString*)args[1].ref;
    JavaObject* image = (JavaObject*)args[2].ref;
    
    (void)image;  /* Image not used in basic implementation */
    
    FORM_DEBUG("append: text=%s", 
            text ? get_string_from_object(jvm, (JavaObject*)text) : "(null)");
    
    if (!list) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Get field offsets considering inheritance */
    JavaClass* list_class = list->header.clazz;
    int strings_idx = get_field_offset(list_class, "strings");
    int selected_idx = get_field_offset(list_class, "selected");
    
    /* List fields: title, listType, strings[], selected[] */
    if (strings_idx >= 0 && selected_idx >= 0) {
        JavaArray* strings = (JavaArray*)list->fields[strings_idx].ref;
        JavaArray* selected = (JavaArray*)list->fields[selected_idx].ref;
        
        int old_len = strings ? strings->length : 0;
        int new_len = old_len + 1;
        
        /* Create new String array with one more element */
        JavaArray* new_strings = jvm_new_array(jvm, DESC_OBJECT, new_len, NULL);
        if (new_strings) {
            void** dst_data = (void**)array_data(new_strings);
            
            /* Copy old elements */
            if (strings && strings->element_type == DESC_OBJECT) {
                void** src_data = (void**)array_data(strings);
                for (int i = 0; i < old_len; i++) {
                    dst_data[i] = src_data[i];
                }
            }
            
            /* Add new element */
            dst_data[old_len] = text;
            list->fields[strings_idx].ref = new_strings;
        }
        
        /* Create new selected array with one more element */
        JavaArray* new_selected = jvm_new_array(jvm, T_BOOLEAN, new_len, NULL);
        if (new_selected) {
            jboolean* dst_sel = (jboolean*)array_data(new_selected);
            
            /* Copy old selected values */
            if (selected && selected->element_type == T_BOOLEAN) {
                jboolean* src_sel = (jboolean*)array_data(selected);
                for (int i = 0; i < old_len; i++) {
                    dst_sel[i] = src_sel[i];
                }
            } else {
                /* Initialize with all false */
                for (int i = 0; i < old_len; i++) {
                    dst_sel[i] = 0;
                }
            }
            
            /* New element is not selected by default */
            dst_sel[old_len] = 0;
            list->fields[selected_idx].ref = new_selected;
        }
        
        FORM_DEBUG("append: added element at index %d, new size=%d", old_len, new_len);
    }
    
    return NATIVE_RETURN_INT(0);
}

/* List.getSelectedIndex() */
static JavaValue native_list_getSelectedIndex(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* list = (JavaObject*)args[0].ref;
    
    FORM_DEBUG("getSelectedIndex: list=%p, g_current_displayable=%p, g_list_selected_index=%d",
            (void*)list, (void*)g_current_displayable, g_list_selected_index);
    
    /* For IMPLICIT list, return the current focused/selected index from UI */
    if (list == g_current_displayable) {
        FORM_DEBUG("getSelectedIndex: returning g_list_selected_index=%d", g_list_selected_index);
        return NATIVE_RETURN_INT(g_list_selected_index);
    }
    
    /* For other cases, return from selected[] array */
    if (list) {
        JavaClass* list_class = list->header.clazz;
        int selected_idx = get_field_offset(list_class, "selected");
        
        if (selected_idx >= 0) {
            JavaArray* selected = (JavaArray*)list->fields[selected_idx].ref;
            if (selected && selected->element_type == T_BOOLEAN) {
                jboolean* selected_data = (jboolean*)array_data(selected);
                for (int i = 0; i < selected->length; i++) {
                    if (selected_data[i]) {
                        FORM_DEBUG("getSelectedIndex: found selected at index %d in selected[]", i);
                        return NATIVE_RETURN_INT(i);
                    }
                }
            }
        }
    }
    
    FORM_DEBUG("getSelectedIndex: fallback to g_list_selected_index=%d", g_list_selected_index);
    return NATIVE_RETURN_INT(g_list_selected_index);
}

/* List.setSelectedIndex(int) */
static JavaValue native_list_setSelectedIndex(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* list = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    
    FORM_DEBUG("setSelectedIndex: %d", index);
    
    return NATIVE_RETURN_VOID();
}

/* List.delete(int elementNum) - delete item at index */
static JavaValue native_list_delete(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* list = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    
    if (!list || index < 0) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Get field offsets considering inheritance */
    JavaClass* list_class = list->header.clazz;
    int strings_idx = get_field_offset(list_class, "strings");
    int selected_idx = get_field_offset(list_class, "selected");
    
    /* List fields: title, listType, strings[], selected[] */
    if (strings_idx >= 0 && selected_idx >= 0) {
        JavaArray* strings = (JavaArray*)list->fields[strings_idx].ref;
        JavaArray* selected = (JavaArray*)list->fields[selected_idx].ref;
        
        if (strings && strings->length > 0 && index < strings->length) {
            /* Create new arrays without the deleted element */
            int new_len = strings->length - 1;
            
            /* Create new String array */
            JavaArray* new_strings = jvm_new_array(jvm, DESC_OBJECT, new_len, NULL);
            if (new_strings && strings->element_type == DESC_OBJECT) {
                void** src_data = (void**)array_data(strings);
                void** dst_data = (void**)array_data(new_strings);
                
                /* Copy elements before index */
                for (int i = 0; i < index; i++) {
                    dst_data[i] = src_data[i];
                }
                /* Copy elements after index */
                for (int i = index; i < new_len; i++) {
                    dst_data[i] = src_data[i + 1];
                }
                list->fields[strings_idx].ref = new_strings;
            }
            
            /* Create new selected array */
            JavaArray* new_selected = jvm_new_array(jvm, T_BOOLEAN, new_len, NULL);
            if (new_selected && selected && selected->element_type == T_BOOLEAN) {
                jboolean* src_sel = (jboolean*)array_data(selected);
                jboolean* dst_sel = (jboolean*)array_data(new_selected);
                
                for (int i = 0; i < index; i++) {
                    dst_sel[i] = src_sel[i];
                }
                for (int i = index; i < new_len; i++) {
                    dst_sel[i] = src_sel[i + 1];
                }
            } else if (new_selected) {
                /* Initialize with all false */
                jboolean* dst_sel = (jboolean*)array_data(new_selected);
                for (int i = 0; i < new_len; i++) {
                    dst_sel[i] = 0;
                }
            }
            list->fields[selected_idx].ref = new_selected;
            
            FORM_DEBUG("delete(%d): new size=%d", index, new_len);
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* List.set(int elementNum, String stringElement, Image imageElement) - replace item at index */
static JavaValue native_list_set(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* list = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    JavaString* text = (JavaString*)args[2].ref;
    JavaObject* image = (JavaObject*)args[3].ref;
    (void)image;  /* Image not used in basic implementation */
    
    if (!list || index < 0) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Get field offset considering inheritance */
    JavaClass* list_class = list->header.clazz;
    int strings_idx = get_field_offset(list_class, "strings");
    
    /* List fields: title, listType, strings[], selected[] */
    if (strings_idx >= 0) {
        JavaArray* strings = (JavaArray*)list->fields[strings_idx].ref;
        
        if (strings && strings->element_type == DESC_OBJECT && index < strings->length) {
            void** strings_data = (void**)array_data(strings);
            strings_data[index] = text;
            
            FORM_DEBUG("[List] set(%d, \"%s\")", index, 
                    text ? get_string_from_object(jvm, (JavaObject*)text) : "null");
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Alert.setString(String) */
static JavaValue native_alert_setString(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* alert = (JavaObject*)args[0].ref;
    JavaString* text = (JavaString*)args[1].ref;
    
    if (alert) {
        int text_slot = find_object_field_slot(alert, "text");
        if (text_slot >= 0 && OBJECT_HAS_FIELDS(alert, text_slot + 1)) {
            alert->fields[text_slot].ref = text;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* TextBox.setString(String) */
static JavaValue native_textbox_setString(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* textbox = (JavaObject*)args[0].ref;
    JavaString* text = (JavaString*)args[1].ref;
    
    if (textbox) {
        int text_slot = find_object_field_slot(textbox, "text");
        if (text_slot >= 0 && OBJECT_HAS_FIELDS(textbox, text_slot + 1)) {
            textbox->fields[text_slot].ref = text;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* TextBox.getString() */
static JavaValue native_textbox_getString(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* textbox = (JavaObject*)args[0].ref;
    
    if (textbox) {
        int text_slot = find_object_field_slot(textbox, "text");
        if (text_slot >= 0 && OBJECT_HAS_FIELDS(textbox, text_slot + 1)) {
            return NATIVE_RETURN_OBJECT(textbox->fields[text_slot].ref);
        }
    }
    
    return NATIVE_RETURN_NULL();
}

/* ============================================
 * HELPER FUNCTIONS FOR FIELD ACCESS
 * ============================================ */

/* Find the slot index for a named field in an object.
 * This correctly traverses the class hierarchy from Object down to the actual class,
 * calculating the cumulative slot index for instance fields.
 * Returns -1 if field not found. */
static int find_object_field_slot(JavaObject* obj, const char* field_name) {
    if (!obj || !field_name) return -1;
    
    JavaClass* clazz = obj->header.clazz;
    if (!clazz) {
        return -1;
    }
    
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
        if (!current->fields) continue;
        
        for (int i = 0; i < current->fields_count; i++) {
            JavaField* field = &current->fields[i];
            
            /* Skip static fields */
            if (field->access_flags & ACC_STATIC) continue;
            
            if (field->name && strcmp(field->name, field_name) == 0) {
                FORM_DEBUG("[find_object_field_slot] '%s' -> slot %d in %s",
                        field_name, slot, current->class_name ? current->class_name : "?");
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
    
    FORM_DEBUG("[find_object_field_slot] '%s' NOT found in '%s'",
            field_name, clazz->class_name ? clazz->class_name : "?");
    return -1;  /* Field not found */
}

/* ============================================ */

/* Item.setLabel(String) */
static JavaValue native_item_setLabel(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    JavaString* label = (JavaString*)args[1].ref;
    
    if (item && OBJECT_HAS_FIELDS(item, 1)) {
        item->fields[0].ref = label;
    }
    
    return NATIVE_RETURN_VOID();
}

/* Item.getLabel() */
static JavaValue native_item_getLabel(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    
    if (item && OBJECT_HAS_FIELDS(item, 1)) {
        return NATIVE_RETURN_OBJECT(item->fields[0].ref);
    }
    
    return NATIVE_RETURN_NULL();
}

/* Command constructor */
static JavaValue native_command_init(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* cmd = (JavaObject*)args[0].ref;
    JavaString* label = (JavaString*)args[1].ref;
    jint type = args[2].i;
    jint priority = args[3].i;
    
    if (cmd && OBJECT_HAS_FIELDS(cmd, 3)) {
        cmd->fields[0].ref = label;     /* label */
        cmd->fields[1].i = type;        /* type */
        cmd->fields[2].i = priority;    /* priority */
    }
    
    FORM_DEBUG(" <init>: label=%s, type=%d, priority=%d\n",
            label ? get_string_from_object(jvm, (JavaObject*)label) : "(null)",
            type, priority);
    
    return NATIVE_RETURN_VOID();
}

/* Command.getCommandType() - returns the command type */
static JavaValue native_command_getCommandType(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* cmd = (JavaObject*)args[0].ref;
    
    if (!cmd) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Find the 'type' field - usually at index 1 */
    int type_slot = find_object_field_slot(cmd, "type");
    if (type_slot < 0) {
        /* Fallback: try known index */
        type_slot = 1;
    }
    
    if (OBJECT_HAS_FIELDS(cmd, type_slot + 1)) {
        jint type = cmd->fields[type_slot].i;
        FORM_DEBUG(" getCommandType: returning %d\n", type);
        return NATIVE_RETURN_INT(type);
    }
    
    return NATIVE_RETURN_INT(0);
}

/* Command.getLabel() - returns the command label string */
static JavaValue native_command_getLabel(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* cmd = (JavaObject*)args[0].ref;
    
    if (!cmd) {
        return NATIVE_RETURN_NULL();
    }
    
    /* Find the 'label' field - usually at index 0 */
    int label_slot = find_object_field_slot(cmd, "label");
    if (label_slot < 0) {
        label_slot = 0;
    }
    
    if (OBJECT_HAS_FIELDS(cmd, label_slot + 1)) {
        JavaString* label = (JavaString*)cmd->fields[label_slot].ref;
        return NATIVE_RETURN_OBJECT(label);
    }
    
    return NATIVE_RETURN_NULL();
}

/* Command.getPriority() - returns the command priority */
static JavaValue native_command_getPriority(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* cmd = (JavaObject*)args[0].ref;
    
    if (!cmd) {
        return NATIVE_RETURN_INT(0);
    }
    
    /* Find the 'priority' field - usually at index 2 */
    int priority_slot = find_object_field_slot(cmd, "priority");
    if (priority_slot < 0) {
        priority_slot = 2;
    }
    
    if (OBJECT_HAS_FIELDS(cmd, priority_slot + 1)) {
        jint priority = cmd->fields[priority_slot].i;
        return NATIVE_RETURN_INT(priority);
    }
    
    return NATIVE_RETURN_INT(0);
}

/* Displayable.addCommand(Command) */
static JavaValue native_displayable_addCommand(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* displayable = (JavaObject*)args[0].ref;
    JavaObject* cmd = (JavaObject*)args[1].ref;
    
    LOG_SAFE("[FORM] addCommand: displayable=%p, class=%s, cmd=%p\n",
            (void*)displayable,
            displayable && displayable->header.clazz && displayable->header.clazz->class_name ?
                displayable->header.clazz->class_name : "N/A",
            (void*)cmd);
    
    if (!displayable || !cmd) {
        return NATIVE_RETURN_VOID();
    }
    
    if (!displayable->header.clazz) {
        LOG_SAFE("[FORM] addCommand: displayable has no class! displayable=%p\n", (void*)displayable);
        return NATIVE_RETURN_VOID();
    }
    
    /* Find the commands field slot using the helper function */
    int commands_slot = find_object_field_slot(displayable, "commands");
    
    if (commands_slot < 0) {
        LOG_SAFE("[FORM] addCommand: 'commands' field not found in class '%s'! "
                "(instance_size=%zu)\n",
                displayable->header.clazz->class_name ? displayable->header.clazz->class_name : "?",
                displayable->header.clazz->instance_size);
        return NATIVE_RETURN_VOID();
    }
    
    FORM_DEBUG(" addCommand: commands field at slot %d\n", commands_slot);
    
    /* Safety check: verify object has enough fields */
    if (!OBJECT_HAS_FIELDS(displayable, commands_slot + 1)) {
        LOG_SAFE("[FORM] addCommand: displayable has insufficient fields! "
                "slot=%d, instance_size=%zu, needed=%zu, class=%s\n",
                commands_slot,
                displayable->header.clazz->instance_size,
                sizeof(ObjectHeader) + (commands_slot + 1) * sizeof(JavaValue),
                displayable->header.clazz->class_name ? displayable->header.clazz->class_name : "?");
        return NATIVE_RETURN_VOID();
    }
    
    /* Get current commands array */
    JavaArray* commands = (JavaArray*)displayable->fields[commands_slot].ref;
    int old_count = commands ? commands->length : 0;
    
    /* If commands is NULL (never initialized), create an empty array */
    if (!commands) {
        commands = jvm_new_array(jvm, DESC_OBJECT, 0, NULL);
        displayable->fields[commands_slot].ref = commands;
        old_count = 0;
    }
    
    FORM_DEBUG(" addCommand: commands_ptr=%p, old_count=%d", (void*)commands, old_count);
    
    /* Check if command already exists (prevent duplicates) */
    if (commands && commands->length > 0) {
        void* raw_data = array_data(commands);
        if (raw_data) {
            void** old_data = (void**)raw_data;
            for (int i = 0; i < old_count; i++) {
                if (old_data[i] == cmd) {
                    FORM_DEBUG(" addCommand: Command already exists at index %d, skipping\n", i);
                    return NATIVE_RETURN_VOID();  /* Already added */
                }
            }
        }
    }
    
    int new_count = old_count + 1;
    
    /* Create new commands array with one more slot */
    JavaArray* new_commands = jvm_new_array(jvm, DESC_OBJECT, new_count, NULL);
    if (!new_commands) {
        FORM_DEBUG(" addCommand: Failed to create new commands array\n");
        return NATIVE_RETURN_VOID();
    }
    
    LOG_SAFE("[FORM] addCommand: new_commands=%p, sizeof(JavaArray)=%zu\n",
            (void*)new_commands, sizeof(JavaArray));
    
    /* Copy old commands */
    if (old_count > 0) {
        void** old_data = (void**)array_data(commands);
        void** new_data = (void**)array_data(new_commands);
        for (int i = 0; i < old_count; i++) {
            new_data[i] = old_data[i];
        }
    }
    
    /* Add new command */
    void** new_data = (void**)array_data(new_commands);
    new_data[old_count] = cmd;
    
    /* Update displayable's commands array */
    displayable->fields[commands_slot].ref = new_commands;
    
    FORM_DEBUG(" addCommand: Added cmd %p at index %d, new count=%d\n",
            (void*)cmd, old_count, new_count);
    
    return NATIVE_RETURN_VOID();
}

/* Displayable.removeCommand(Command) */
static JavaValue native_displayable_removeCommand(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* displayable = (JavaObject*)args[0].ref;
    JavaObject* cmd = (JavaObject*)args[1].ref;
    
    FORM_DEBUG(" removeCommand: displayable=%p, cmd=%p\n",
            (void*)displayable, (void*)cmd);
    
    if (!displayable || !cmd) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Find the commands field slot */
    int commands_slot = find_object_field_slot(displayable, "commands");
    
    if (commands_slot < 0) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Get current commands array */
    JavaArray* commands = (JavaArray*)displayable->fields[commands_slot].ref;
    if (!commands || commands->length == 0) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Find and remove the command */
    void** old_data = (void**)array_data(commands);
    int old_count = commands->length;
    int found_idx = -1;
    
    for (int i = 0; i < old_count; i++) {
        if (old_data[i] == cmd) {
            found_idx = i;
            break;
        }
    }
    
    if (found_idx < 0) {
        return NATIVE_RETURN_VOID();  /* Command not found */
    }
    
    /* Create new array without the removed command */
    int new_count = old_count - 1;
    if (new_count == 0) {
        /* No commands left, set to empty array */
        JavaArray* empty = jvm_new_array(jvm, DESC_OBJECT, 0, NULL);
        displayable->fields[commands_slot].ref = empty;
        return NATIVE_RETURN_VOID();
    }
    
    JavaArray* new_commands = jvm_new_array(jvm, DESC_OBJECT, new_count, NULL);
    if (!new_commands) {
        return NATIVE_RETURN_VOID();
    }
    
    void** new_data = (void**)array_data(new_commands);
    for (int i = 0, j = 0; i < old_count; i++) {
        if (i != found_idx) {
            new_data[j++] = old_data[i];
        }
    }
    
    displayable->fields[commands_slot].ref = new_commands;
    
    FORM_DEBUG(" removeCommand: Removed cmd %p, new count=%d\n", (void*)cmd, new_count);
    
    return NATIVE_RETURN_VOID();
}

/* Displayable.setCommandListener(CommandListener) */
static JavaValue native_displayable_setCommandListener(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* displayable = (JavaObject*)args[0].ref;
    JavaObject* listener = (JavaObject*)args[1].ref;
    
    FORM_DEBUG(" setCommandListener: displayable=%p, listener=%p\n",
            (void*)displayable, (void*)listener);
    
    if (!displayable) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Find the listener field slot using the helper function */
    int listener_slot = find_object_field_slot(displayable, "listener");
    
    if (listener_slot >= 0) {
        displayable->fields[listener_slot].ref = listener;
        FORM_DEBUG(" Saved listener to field slot %d\n", listener_slot);
    } else {
        FORM_DEBUG(" setCommandListener: 'listener' field not found!\n");
    }
    
    return NATIVE_RETURN_VOID();
}

/* Spacer.setSize(int, int) */
static JavaValue native_spacer_setSize(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* spacer = (JavaObject*)args[0].ref;
    jint width = args[1].i;
    jint height = args[2].i;
    
    if (spacer) {
        int width_slot = find_object_field_slot(spacer, "width");
        int height_slot = find_object_field_slot(spacer, "height");
        if (width_slot >= 0 && height_slot >= 0 &&
            OBJECT_HAS_FIELDS(spacer, height_slot + 1)) {
            spacer->fields[width_slot].i = width;
            spacer->fields[height_slot].i = height;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Spacer.setMinimumSize(int, int) */
static JavaValue native_spacer_setMinimumSize(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    return native_spacer_setSize(jvm, thread, args, arg_count);
}

/* ImageItem.setImage(Image) */
static JavaValue native_imageitem_setImage(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    JavaObject* image = (JavaObject*)args[1].ref;
    
    if (item) {
        int image_slot = find_object_field_slot(item, "image");
        if (image_slot >= 0 && OBJECT_HAS_FIELDS(item, image_slot + 1)) {
            item->fields[image_slot].ref = image;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/*
 * Render a Form on screen
 * This is called from display.c when setCurrent() is called with a Form
 */
void midp_render_form(JVM* jvm, JavaObject* form) {
    if (!form) {
        LOG_SAFE("[FORM_RENDER] form is NULL!\n");
        return;
    }
    
    LOG_SAFE("[FORM_RENDER] Rendering form, class=%s, instance_size=%zu\n",
            form->header.clazz ? form->header.clazz->class_name : "NULL",
            form->header.clazz ? form->header.clazz->instance_size : 0);
    
    g_current_displayable = form;  /* Track current displayable */
    
    MidpGraphics* gfx = get_screen_graphics();
    if (!gfx) {
        LOG_SAFE("[FORM_RENDER] get_screen_graphics returned NULL! No framebuffer?\n");
        return;
    }
    
    LOG_SAFE("[FORM_RENDER] gfx=%p, pixels=%p, width=%d, height=%d\n",
            (void*)gfx, (void*)gfx->pixels, gfx->width, gfx->height);
    
    /* Clear screen */
    midp_graphics_set_color(gfx, 0xFFFFFF, 255);
    midp_graphics_fill_rect(gfx, 0, 0, gfx->width, gfx->height);
    
    /* Draw title bar */
    const char* title = "";
    int title_slot = find_object_field_slot(form, "title");
    if (title_slot >= 0 && OBJECT_HAS_FIELDS(form, title_slot + 1)) {
        JavaString* title_str = (JavaString*)form->fields[title_slot].ref;
        if (title_str) title = get_string_from_object(jvm, (JavaObject*)title_str);
    }
    
    /* Title background */
    midp_graphics_set_color(gfx, 0x000080, 255);
    midp_graphics_fill_rect(gfx, 0, 0, gfx->width, 20);
    midp_graphics_set_color(gfx, 0xFFFFFF, 255);
    midp_graphics_draw_string(gfx, title, gfx->width / 2, 3, 0x10); /* HCENTER */
    
    /* Draw separator */
    midp_graphics_set_color(gfx, 0x808080, 255);
    midp_graphics_draw_line(gfx, 0, 20, gfx->width, 20);
    
    /* Get items array from form */
    int y = 25;
    
    /* Limit y if keyboard is active */
    int max_y = g_vkb.active ? (gfx->height - 170) : (gfx->height - 30);
    
    int items_slot = find_object_field_slot(form, "items");
    if (items_slot >= 0 && OBJECT_HAS_FIELDS(form, items_slot + 1)) {
        JavaArray* items = (JavaArray*)form->fields[items_slot].ref;
        
        FORM_DEBUG("[FORM_RENDER] items=%p, length=%d",
                (void*)items, items ? items->length : -1);
        
        if (items && items->element_type == DESC_OBJECT) {
            void** items_data = (void**)array_data(items);
            
            for (int i = 0; i < items->length && y < max_y; i++) {
                JavaObject* item = (JavaObject*)items_data[i];
                if (!item) continue;
                
                bool focused = (i == g_focused_item_index);
                int item_type = get_item_type(item);
                
                switch (item_type) {
                    case ITEM_STRINGITEM:
                        y = render_stringitem(jvm, gfx, item, y);
                        break;
                    case ITEM_TEXTFIELD:
                        y = render_textfield(jvm, gfx, item, y, focused);
                        break;
                    case ITEM_CHOICEGROUP:
                        y = render_choicegroup(jvm, gfx, item, y, focused);
                        break;
                    case ITEM_GAUGE:
                        y = render_gauge(jvm, gfx, item, y, focused);
                        break;
                    case ITEM_SPACER:
                        y = render_spacer(jvm, gfx, item, y);
                        break;
                    default:
                        /* Unknown item type - skip */
                        y += 20;
                        break;
                }
            }
        }
    }
    
    /* Draw soft button area with actual command labels (only if keyboard not active) */
    if (!g_vkb.active) {
        midp_graphics_set_color(gfx, 0xC0C0C0, 255);
        midp_graphics_fill_rect(gfx, 0, gfx->height - 25, gfx->width, 25);
        midp_graphics_set_color(gfx, 0x000000, 255);
        midp_graphics_draw_line(gfx, 0, gfx->height - 25, gfx->width, gfx->height - 25);
        
        /* Get commands from displayable to show real labels */
        int commands_idx = find_object_field_slot(form, "commands");
        if (commands_idx >= 0) {
            JavaArray* commands = (JavaArray*)form->fields[commands_idx].ref;
            if (commands && commands->length > 0) {
                void** cmd_data = (void**)array_data(commands);
                int cmd_count = commands->length;
                
                /* Sort commands by type priority: BACK(2)/EXIT(7) -> right, OK(4) -> right or middle */
                int left_cmd_idx = 0;
                int right_cmd_idx = -1;
                
                /* Find BACK or EXIT command for right side */
                for (int ci = 0; ci < cmd_count; ci++) {
                    JavaObject* cmd = (JavaObject*)cmd_data[ci];
                    if (!cmd || !cmd->header.clazz) continue;
                    /* Command fields: label(0), type(1), priority(2) */
                    if (OBJECT_HAS_FIELDS(cmd, 3)) {
                        int cmd_type = cmd->fields[1].i;
                        if ((cmd_type == 2 || cmd_type == 7) && right_cmd_idx < 0) {
                            right_cmd_idx = ci;
                        }
                    }
                }
                
                /* If only 1 command, it goes left */
                if (cmd_count == 1) {
                    left_cmd_idx = 0;
                    right_cmd_idx = -1;
                } else if (right_cmd_idx >= 0 && cmd_count >= 2) {
                    /* Right has BACK/EXIT, left gets first non-right command */
                    left_cmd_idx = (right_cmd_idx == 0) ? 1 : 0;
                } else {
                    /* No BACK/EXIT found: first cmd left, second cmd right */
                    left_cmd_idx = 0;
                    right_cmd_idx = 1;
                }
                
                /* Left button label */
                if (cmd_data[left_cmd_idx]) {
                    JavaObject* cmd = (JavaObject*)cmd_data[left_cmd_idx];
                    if (OBJECT_HAS_FIELDS(cmd, 3)) {
                        JavaString* label = (JavaString*)cmd->fields[0].ref;
                        if (label) {
                            const char* text = string_utf8(jvm, label);
                            if (text && text[0]) {
                                midp_graphics_draw_string(gfx, text, 5, gfx->height - 20, 0);
                            } else {
                                midp_graphics_draw_string(gfx, "OK", 5, gfx->height - 20, 0);
                            }
                        } else {
                            midp_graphics_draw_string(gfx, "OK", 5, gfx->height - 20, 0);
                        }
                    }
                }
                
                /* Right button label */
                if (right_cmd_idx >= 0 && cmd_data[right_cmd_idx]) {
                    JavaObject* cmd = (JavaObject*)cmd_data[right_cmd_idx];
                    if (OBJECT_HAS_FIELDS(cmd, 3)) {
                        JavaString* label = (JavaString*)cmd->fields[0].ref;
                        if (label) {
                            const char* text = string_utf8(jvm, label);
                            if (text && text[0]) {
                                int text_len = strlen(text);
                                int text_width = text_len * 6;
                                midp_graphics_draw_string(gfx, text,
                                    gfx->width - text_width - 5, gfx->height - 20, 0);
                            } else {
                                midp_graphics_draw_string(gfx, "Back", gfx->width - 40, gfx->height - 20, 0);
                            }
                        } else {
                            midp_graphics_draw_string(gfx, "Back", gfx->width - 40, gfx->height - 20, 0);
                        }
                    }
                }
            }
        }
    }
    
    /* Render virtual keyboard if active */
    vkb_render(gfx);
    
    /* ИСПРАВЛЕНО: Запрашиваем перерисовку вместо прямого present */
    sdl_request_redraw();
}

/* Public function: Render Alert */
void midp_render_alert(JVM* jvm, MidpGraphics* gfx, JavaObject* alert) {
    if (!alert || !gfx) return;
    render_alert(jvm, gfx, alert);
}

/* Check if Alert timeout has expired and should be dismissed 
 * Returns true if Alert was dismissed, false otherwise */
bool midp_check_alert_timeout(JVM* jvm) {
    if (!g_current_displayable) return false;
    
    JavaClass* clazz = g_current_displayable->header.clazz;
    if (!clazz || !clazz->class_name) return false;
    
    /* Check if current displayable is an Alert */
    bool is_alert = false;
    JavaClass* check = clazz;
    while (check) {
        if (check->class_name && strcmp(check->class_name, "javax/microedition/lcdui/Alert") == 0) {
            is_alert = true;
            break;
        }
        check = check->super_class;
    }
    
    if (!is_alert) return false;
    
    FORM_DEBUG("[ALERT] Checking timeout for Alert");
    
    /* Get timeout field from Alert — use dynamic slot lookup */
    int timeout_slot = find_object_field_slot(g_current_displayable, "timeout");
    if (timeout_slot < 0 || !OBJECT_HAS_FIELDS(g_current_displayable, timeout_slot + 1)) return false;
    
    jint timeout = g_current_displayable->fields[timeout_slot].i;
    
    FORM_DEBUG("[ALERT] timeout value: %d ms", timeout);
    
    /* FOREVER = -1, meaning never timeout */
    if (timeout == -1) {
        FORM_DEBUG("[ALERT] timeout is FOREVER, not dismissing");
        return false;
    }
    
    /* Default timeout is usually 2000ms if not set */
    if (timeout <= 0) {
        timeout = 2000;  /* Default 2 seconds */
    }
    
    /* Get alert start time - we need to track this */
    static uint64_t alert_start_time = 0;
    static JavaObject* last_alert = NULL;
    
    /* Check if this is a new alert */
    if (last_alert != g_current_displayable) {
        last_alert = g_current_displayable;
        SdlContext* sdl_ctx = sdl_get_global_context();
        alert_start_time = sdl_ctx ? sdl_get_ticks(sdl_ctx) : 0;
        FORM_DEBUG("[ALERT] New Alert displayed, start_time=%llu", (unsigned long long)alert_start_time);
        return false;
    }
    
    /* Check if timeout has expired */
    SdlContext* sdl_ctx = sdl_get_global_context();
    if (!sdl_ctx) return false;
    
    uint64_t now = sdl_get_ticks(sdl_ctx);
    uint64_t elapsed = now - alert_start_time;
    
    FORM_DEBUG("[ALERT] elapsed=%llu ms, timeout=%d ms", (unsigned long long)elapsed, timeout);
    
    if (elapsed >= (uint64_t)timeout) {
        FORM_DEBUG("[ALERT] Timeout expired! Calling commandAction with DISMISS");
        
        /* Call commandAction with DISMISS command if listener exists */
        /* Find listener field in Alert */
        int listener_slot = find_object_field_slot(g_current_displayable, "listener");
        if (listener_slot >= 0) {
            JavaObject* listener = (JavaObject*)g_current_displayable->fields[listener_slot].ref;
            if (listener && listener->header.clazz) {
                /* Create a DISMISS command */
                JavaClass* cmd_class = jvm_load_class(jvm, "javax/microedition/lcdui/Command");
                if (cmd_class) {
                    JavaObject* dismiss_cmd = jvm_new_object(jvm, cmd_class);
                    if (dismiss_cmd && OBJECT_HAS_FIELDS(dismiss_cmd, 3)) {
                        JavaString* label = jvm_new_string(jvm, "Dismiss");
                        dismiss_cmd->fields[0].ref = label;  /* label */
                        dismiss_cmd->fields[1].i = 3;        /* type = DISMISS = 3 */
                        dismiss_cmd->fields[2].i = 0;        /* priority */
                        
                        /* Find and call commandAction(Command, Displayable) */
                        JavaClass* listener_class = listener->header.clazz;
                        JavaMethod* method = jvm_resolve_method(jvm, listener_class, "commandAction",
                            "(Ljavax/microedition/lcdui/Command;Ljavax/microedition/lcdui/Displayable;)V");
                        
                        if (method) {
                            extern int execute_method(JVM* jvm, JavaThread* thread, JavaMethod* method, 
                                                      JavaValue* args, JavaValue* result);
                            JavaValue args[3];
                            args[0].ref = listener;        /* this */
                            args[1].ref = dismiss_cmd;     /* Command */
                            args[2].ref = g_current_displayable;  /* Displayable */
                            
                            JavaValue result;
                            JavaThread* thread = jvm_current_thread(jvm);
                            execute_method(jvm, thread, method, args, &result);
                            FORM_DEBUG("[ALERT] commandAction called successfully");
                        } else {
                            WARN_LOG("[ALERT] commandAction method not found in %s",
                                    listener_class->class_name ? listener_class->class_name : "?");
                        }
                    }
                }
            } else {
                WARN_LOG("[ALERT] No listener set, cannot call commandAction");
            }
        }
        
        last_alert = NULL;
        return true;  /* Signal that alert should be dismissed */
    }
    
    return false;
}

/* Public function: Render List */
void midp_render_list(JVM* jvm, JavaObject* list, int focused_index) {
    if (!list) return;
    
    g_current_displayable = list;  /* Track current displayable */
    
    MidpGraphics* gfx = get_screen_graphics();
    if (!gfx) return;
    
    /* Clear screen */
    midp_graphics_set_color(gfx, 0xFFFFFF, 255);
    midp_graphics_fill_rect(gfx, 0, 0, gfx->width, gfx->height);
    
    render_list(jvm, gfx, list, focused_index);
    
    /* ИСПРАВЛЕНО: Запрашиваем перерисовку вместо прямого present */
    sdl_request_redraw();
}

/* Public function: Render TextBox */
void midp_render_textbox(JVM* jvm, JavaObject* textbox, const char* input_text, int cursor_pos) {
    if (!textbox) return;
    
    g_current_displayable = textbox;  /* Track current displayable */
    
    MidpGraphics* gfx = get_screen_graphics();
    if (!gfx) return;
    
    /* Clear screen */
    midp_graphics_set_color(gfx, 0xFFFFFF, 255);
    midp_graphics_fill_rect(gfx, 0, 0, gfx->width, gfx->height);
    
    render_textbox(jvm, gfx, textbox, input_text, cursor_pos);
    
    /* Render virtual keyboard if active */
    vkb_render(gfx);
    
    /* ИСПРАВЛЕНО: Запрашиваем перерисовку вместо прямого present */
    sdl_request_redraw();
}

/* ============================================
 * FORM NAVIGATION AND INPUT HANDLING
 * ============================================ */

/* Handle key press for current displayable (forms, lists, etc.) */
bool midp_form_handle_key(JVM* jvm, int game_action) {
    /* If virtual keyboard is active, send keys to it */
    if (g_vkb.active) {
        return vkb_handle_key(jvm, game_action);
    }
    
    /* Handle based on current displayable type */
    if (!g_current_displayable) {
        FORM_DEBUG("midp_form_handle_key: g_current_displayable is NULL!");
        return false;
    }
    
    JavaClass* clazz = g_current_displayable->header.clazz;
    if (!clazz || !clazz->class_name) {
        FORM_DEBUG("midp_form_handle_key: invalid class!");
        return false;
    }
    
    FORM_DEBUG("midp_form_handle_key: game_action=%d, class=%s", game_action, clazz->class_name);
    
    /* Check if it's a Form */
    if (strstr(clazz->class_name, "Form")) {
        /* Get items count — use dynamic slot lookup */
        int item_count = 0;
        JavaArray* items = NULL;
        
        int items_slot = find_object_field_slot(g_current_displayable, "items");
        if (items_slot >= 0 && OBJECT_HAS_FIELDS(g_current_displayable, items_slot + 1)) {
            items = (JavaArray*)g_current_displayable->fields[items_slot].ref;
            if (items) item_count = items->length;
        }
        
        switch (game_action) {
            case 1:  /* UP */
                if (g_focused_item_index > 0) {
                    g_focused_item_index--;
                    midp_render_form(jvm, g_current_displayable);
                    return true;
                }
                break;
                
            case 6:  /* DOWN */
                if (g_focused_item_index < item_count - 1) {
                    g_focused_item_index++;
                    midp_render_form(jvm, g_current_displayable);
                    return true;
                }
                break;
                
            case 8:  /* FIRE - activate focused item */
                if (items && g_focused_item_index < items->length) {
                    void** items_data = (void**)array_data(items);
                    JavaObject* item = (JavaObject*)items_data[g_focused_item_index];
                    int item_type = get_item_type(item);
                    
                    if (item_type == ITEM_TEXTFIELD) {
                        /* Start virtual keyboard for TextField */
                        int max_size = 32;
                        int constraints = 0;
                        
                        int maxsize_slot = find_object_field_slot(item, "maxSize");
                        int constraints_slot = find_object_field_slot(item, "constraints");
                        if (maxsize_slot >= 0 && constraints_slot >= 0 &&
                            OBJECT_HAS_FIELDS(item, constraints_slot + 1)) {
                            max_size = item->fields[maxsize_slot].i;
                            constraints = item->fields[constraints_slot].i;
                        }
                        
                        vkb_start(item, max_size, constraints);
                        midp_render_form(jvm, g_current_displayable);
                        return true;
                    }
                    else if (item_type == ITEM_CHOICEGROUP) {
                        /* Handle ChoiceGroup interaction */
                        int choicetype_slot = find_object_field_slot(item, "choiceType");
                        int selected_slot = find_object_field_slot(item, "selected");
                        if (choicetype_slot >= 0 && selected_slot >= 0 &&
                            OBJECT_HAS_FIELDS(item, selected_slot + 1)) {
                            int choice_type = item->fields[choicetype_slot].i;
                            JavaArray* selected = (JavaArray*)item->fields[selected_slot].ref;
                            int sel_count = selected ? selected->length : 0;
                            
                            if (selected && sel_count > 0) {
                                jboolean* sel_data = (jboolean*)array_data(selected);
                                
                                if (choice_type == CHOICE_EXCLUSIVE || choice_type == CHOICE_POPUP) {
                                    /* EXCLUSIVE or POPUP: cycle to next element */
                                    int cur = -1;
                                    for (int si = 0; si < sel_count; si++) {
                                        if (sel_data[si]) { cur = si; break; }
                                    }
                                    /* Deselect current, select next */
                                    if (cur >= 0) sel_data[cur] = 0;
                                    sel_data[(cur + 1) % sel_count] = 1;
                                } else {
                                    /* MULTIPLE: toggle focused element, advance */
                                    sel_data[g_cg_focused_element % sel_count] = 
                                        !sel_data[g_cg_focused_element % sel_count];
                                    g_cg_focused_element = (g_cg_focused_element + 1) % sel_count;
                                }
                                
                                /* Notify ItemStateListener */
                                if (g_item_state_listener && g_item_state_listener->header.clazz) {
                                    JavaMethod* ism = jvm_resolve_method(jvm, 
                                        g_item_state_listener->header.clazz,
                                        "itemStateChanged", "(Ljavax/microedition/lcdui/Item;)V");
                                    if (ism) {
                                        JavaValue ism_args[2];
                                        ism_args[0].ref = g_item_state_listener;
                                        ism_args[1].ref = item;
                                        JavaValue ism_result;
                                        execute_method(jvm, jvm_current_thread(jvm), ism, ism_args, &ism_result);
                                    }
                                }
                                
                                midp_render_form(jvm, g_current_displayable);
                                return true;
                            }
                        }
                    }
                    else if (item_type == ITEM_GAUGE) {
                        /* Toggle gauge value (increment) */
                        int maxvalue_slot = find_object_field_slot(item, "maxValue");
                        int value_slot = find_object_field_slot(item, "value");
                        if (maxvalue_slot >= 0 && value_slot >= 0 &&
                            OBJECT_HAS_FIELDS(item, value_slot + 1)) {
                            int max_val = item->fields[maxvalue_slot].i;
                            int cur_val = item->fields[value_slot].i;
                            if (max_val > 0) {
                                cur_val = (cur_val + 1) % (max_val + 1);
                                item->fields[value_slot].i = cur_val;
                                
                                /* Notify ItemStateListener */
                                if (g_item_state_listener && g_item_state_listener->header.clazz) {
                                    JavaMethod* ism = jvm_resolve_method(jvm, 
                                        g_item_state_listener->header.clazz,
                                        "itemStateChanged", "(Ljavax/microedition/lcdui/Item;)V");
                                    if (ism) {
                                        JavaValue ism_args[2];
                                        ism_args[0].ref = g_item_state_listener;
                                        ism_args[1].ref = item;
                                        JavaValue ism_result;
                                        execute_method(jvm, jvm_current_thread(jvm), ism, ism_args, &ism_result);
                                    }
                                }
                                
                                midp_render_form(jvm, g_current_displayable);
                                return true;
                            }
                        }
                    }
                }
                break;
        }
    }
    /* Check if it's a List */
    else if (strstr(clazz->class_name, "List")) {
        int list_size = 0;
        
        /* List fields — use dynamic slot lookup */
        int strings_slot = find_object_field_slot(g_current_displayable, "strings");
        if (strings_slot >= 0 && OBJECT_HAS_FIELDS(g_current_displayable, strings_slot + 1)) {
            JavaArray* strings = (JavaArray*)g_current_displayable->fields[strings_slot].ref;
            FORM_DEBUG("strings array: %p, element_type=%d", 
                    (void*)strings, strings ? strings->element_type : -1);
            if (strings) list_size = strings->length;
        } else {
            FORM_DEBUG("List field lookup failed for 'strings'!");
        }
        
        FORM_DEBUG("List navigation: game_action=%d, current_index=%d, list_size=%d", 
                game_action, g_list_selected_index, list_size);
        
        switch (game_action) {
            case 1:  /* UP */
                if (g_list_selected_index > 0) {
                    g_list_selected_index--;
                    FORM_DEBUG("UP: new index=%d", g_list_selected_index);
                    midp_render_list(jvm, g_current_displayable, g_list_selected_index);
                    return true;
                }
                break;
                
            case 6:  /* DOWN */
                if (g_list_selected_index < list_size - 1) {
                    g_list_selected_index++;
                    FORM_DEBUG("DOWN: new index=%d", g_list_selected_index);
                    midp_render_list(jvm, g_current_displayable, g_list_selected_index);
                    return true;
                }
                break;
                
            case 8:  /* FIRE - select item */
                {
                    FORM_DEBUG("FIRE: selecting item %d", g_list_selected_index);
                    
                    /* Get listener using dynamic field offset lookup */
                    JavaObject* listener = NULL;
                    int listener_idx = get_field_offset(g_current_displayable->header.clazz, "listener");
                    if (listener_idx < 0) {
                        listener_idx = get_field_offset(g_current_displayable->header.clazz, "commandListener");
                    }
                    
                    if (listener_idx >= 0 && OBJECT_HAS_FIELDS(g_current_displayable, listener_idx + 1)) {
                        listener = (JavaObject*)g_current_displayable->fields[listener_idx].ref;
                    }
                    
                    FORM_DEBUG("Listener: %p (field idx: %d)", (void*)listener, listener_idx);
                    
                    if (listener) {
                        /* Use the global SELECT_COMMAND singleton */
                        ensure_select_command(jvm);
                        
                        FORM_DEBUG("Using SELECT_COMMAND: %p", (void*)g_select_command);
                        
                        /* Call listener.commandAction(cmd, displayable) */
                        if (g_select_command) {
                            JavaClass* listener_class = listener->header.clazz;
                            JavaMethod* method = jvm_resolve_method(jvm, listener_class, "commandAction", 
                                "(Ljavax/microedition/lcdui/Command;Ljavax/microedition/lcdui/Displayable;)V");
                            
                            FORM_DEBUG("commandAction method: %p", (void*)method);
                            
                            if (method) {
                                JavaValue args[3];
                                args[0].ref = listener;                   /* this */
                                args[1].ref = g_select_command;           /* SELECT_COMMAND */
                                args[2].ref = g_current_displayable;      /* Displayable */
                                
                                JavaValue result;
                                JavaThread* thread = jvm_current_thread(jvm);
                                
                                FORM_DEBUG("Calling commandAction(SELECT_COMMAND, List)");
                                execute_method(jvm, thread, method, args, &result);
                            } else {
                                FORM_DEBUG("commandAction method not found!");
                            }
                        }
                    } else {
                        /* Fallback: Try to call commandAction through display.c's helper */
                        FORM_DEBUG("No listener field found, trying call_command_action");
                        extern bool call_command_action_for_list(JVM* jvm, JavaObject* list, int selected_index);
                        if (!call_command_action_for_list(jvm, g_current_displayable, g_list_selected_index)) {
                            FORM_DEBUG("call_command_action_for_list also failed!");
                        }
                    }
                }
                return true;
        }
    }
    /* Check if it's a TextBox */
    else if (strstr(clazz->class_name, "TextBox")) {
        if (game_action == 8) {  /* FIRE */
            /* Start virtual keyboard for TextBox */
            int max_size = 32;
            int constraints = 0;
            
            int maxsize_slot = find_object_field_slot(g_current_displayable, "maxSize");
            int constraints_slot = find_object_field_slot(g_current_displayable, "constraints");
            if (maxsize_slot >= 0 && constraints_slot >= 0 &&
                OBJECT_HAS_FIELDS(g_current_displayable, constraints_slot + 1)) {
                max_size = g_current_displayable->fields[maxsize_slot].i;
                constraints = g_current_displayable->fields[constraints_slot].i;
            }
            
            vkb_start(g_current_displayable, max_size, constraints);
            midp_render_textbox(jvm, g_current_displayable, g_vkb.text_buffer, g_vkb.cursor_pos);
            return true;
        }
    }
    
    return false;  /* Key not handled */
}

/* Ensure SELECT_COMMAND singleton is created */
static void ensure_select_command(JVM* jvm) {
    if (g_select_command) return;
    
    JavaClass* cmd_class = jvm_load_class(jvm, "javax/microedition/lcdui/Command");
    if (!cmd_class) return;
    
    g_select_command = jvm_new_object(jvm, cmd_class);
    if (g_select_command && OBJECT_HAS_FIELDS(g_select_command, 3)) {
        /* Command(label, type=4 SCREEN, priority=0) */
        JavaString* label = jvm_new_string(jvm, "Select");
        g_select_command->fields[0].ref = label;     /* label */
        g_select_command->fields[1].i = 4;          /* SCREEN type */
        g_select_command->fields[2].i = 0;          /* priority */
    }
    
    /* Also set the static field in List class */
    JavaClass* list_class = jvm_load_class(jvm, "javax/microedition/lcdui/List");
    if (list_class && list_class->static_fields_count > 0) {
        list_class->static_fields[0].value.ref = g_select_command;
    }
    
    FORM_DEBUG("Created SELECT_COMMAND singleton: %p", (void*)g_select_command);
}

/* Set current displayable */
void midp_set_current_displayable(JavaObject* displayable) {
    g_current_displayable = displayable;
    g_focused_item_index = 0;
    g_list_selected_index = 0;
    g_vkb.active = false;
}

/*
 * Additional native methods for better form support
 */

/* List.<init>(String, int) - constructor for List */
static JavaValue native_list_init(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* list = (JavaObject*)args[0].ref;
    JavaString* title = (JavaString*)args[1].ref;
    jint list_type = args[2].i;
    
    /* Create SELECT_COMMAND singleton on first List creation */
    ensure_select_command(jvm);
    
    if (!list) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Get field offsets considering inheritance */
    JavaClass* list_class = list->header.clazz;
    int title_idx = get_field_offset(list_class, "title");
    int listtype_idx = get_field_offset(list_class, "listType");
    int strings_idx = get_field_offset(list_class, "strings");
    int selected_idx = get_field_offset(list_class, "selected");
    
    /* Initialize List fields: title, listType, strings[], selected[] */
    if (title_idx >= 0 && listtype_idx >= 0 && strings_idx >= 0 && selected_idx >= 0) {
        list->fields[title_idx].ref = title;
        list->fields[listtype_idx].i = list_type;
        
        /* Create empty arrays for strings and selection */
        JavaArray* strings = jvm_new_array(jvm, DESC_OBJECT, 0, NULL);
        list->fields[strings_idx].ref = strings;
        
        /* Create empty boolean array for selection */
        JavaArray* selected = jvm_new_array(jvm, T_BOOLEAN, 0, NULL);
        list->fields[selected_idx].ref = selected;
    }
    
    FORM_DEBUG("<init>: title=%s, type=%d",
            title ? get_string_from_object(jvm, (JavaObject*)title) : "(null)",
            list_type);
    
    return NATIVE_RETURN_VOID();
}

/* List.<init>(String, int, String[], Image[]) - constructor with initial elements */
static JavaValue native_list_init_with_elements(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* list = (JavaObject*)args[0].ref;
    JavaString* title = (JavaString*)args[1].ref;
    jint list_type = args[2].i;
    JavaArray* strings = (JavaArray*)args[3].ref;
    JavaArray* images = (JavaArray*)args[4].ref;
    
    /* Create SELECT_COMMAND singleton on first List creation */
    ensure_select_command(jvm);
    
    if (!list) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Get field offsets considering inheritance */
    JavaClass* list_class = list->header.clazz;
    int title_idx = get_field_offset(list_class, "title");
    int listtype_idx = get_field_offset(list_class, "listType");
    int strings_idx = get_field_offset(list_class, "strings");
    int selected_idx = get_field_offset(list_class, "selected");
    
    /* Initialize List fields: title, listType, strings[], selected[] */
    if (title_idx >= 0 && listtype_idx >= 0 && strings_idx >= 0 && selected_idx >= 0) {
        list->fields[title_idx].ref = title;
        list->fields[listtype_idx].i = list_type;
        list->fields[strings_idx].ref = strings;
        
        /* Create selected[] array with same length as strings */
        if (strings) {
            JavaArray* selected = jvm_new_array(jvm, T_BOOLEAN, strings->length, NULL);
            list->fields[selected_idx].ref = selected;
            
            /* For IMPLICIT, EXCLUSIVE, and POPUP lists, select first item by default */
            if (selected && strings->length > 0 && 
                (list_type == 1 || list_type == 3 || list_type == 4)) { /* EXCLUSIVE=1, POPUP=3, IMPLICIT=4 */
                jboolean* selected_data = (jboolean*)array_data(selected);
                selected_data[0] = 1;
                FORM_DEBUG("<init>: set initial selection at index 0 for type=%d", list_type);
            }
        } else {
            JavaArray* selected = jvm_new_array(jvm, T_BOOLEAN, 0, NULL);
            list->fields[selected_idx].ref = selected;
        }
    }
    
    (void)images; /* Images array not used in basic rendering */
    
    FORM_DEBUG("<init>(with elements): title=%s, type=%d, elements=%d",
            title ? get_string_from_object(jvm, (JavaObject*)title) : "(null)",
            list_type,
            strings ? strings->length : 0);
    
    return NATIVE_RETURN_VOID();
}

/* Alert.<init>(String, String, Image, AlertType) */
static JavaValue native_alert_init(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* alert = (JavaObject*)args[0].ref;
    JavaString* title = (JavaString*)args[1].ref;
    JavaString* text = (JavaString*)args[2].ref;
    JavaObject* image = (JavaObject*)args[3].ref;
    JavaObject* alert_type = (JavaObject*)args[4].ref;
    
    if (!alert) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Initialize Alert fields — use dynamic slot lookup */
    int title_slot = find_object_field_slot(alert, "title");
    int text_slot = find_object_field_slot(alert, "text");
    int image_slot = find_object_field_slot(alert, "image");
    int type_slot = find_object_field_slot(alert, "alertType");
    int timeout_slot = find_object_field_slot(alert, "timeout");
    
    if (title_slot >= 0 && text_slot >= 0 && image_slot >= 0 &&
        type_slot >= 0 && timeout_slot >= 0 &&
        OBJECT_HAS_FIELDS(alert, timeout_slot + 1)) {
        alert->fields[title_slot].ref = title;
        alert->fields[text_slot].ref = text;
        alert->fields[image_slot].ref = image;
        alert->fields[type_slot].ref = alert_type;
        alert->fields[timeout_slot].i = 3000; /* Default timeout 3 seconds */
    }
    
    FORM_DEBUG("[Alert] <init>: title=%s, text=%s",
            title ? get_string_from_object(jvm, (JavaObject*)title) : "(null)",
            text ? get_string_from_object(jvm, (JavaObject*)text) : "(null)");
    
    return NATIVE_RETURN_VOID();
}

/* Alert.setTimeout(int) */
static JavaValue native_alert_setTimeout(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* alert = (JavaObject*)args[0].ref;
    jint timeout = args[1].i;
    
    if (alert) {
        int timeout_slot = find_object_field_slot(alert, "timeout");
        if (timeout_slot >= 0 && OBJECT_HAS_FIELDS(alert, timeout_slot + 1)) {
            alert->fields[timeout_slot].i = timeout;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Alert.getTimeout() - returns the timeout value */
static JavaValue native_alert_getTimeout(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* alert = (JavaObject*)args[0].ref;
    
    if (alert) {
        int timeout_slot = find_object_field_slot(alert, "timeout");
        if (timeout_slot >= 0 && OBJECT_HAS_FIELDS(alert, timeout_slot + 1)) {
            return NATIVE_RETURN_INT(alert->fields[timeout_slot].i);
        }
    }
    
    return NATIVE_RETURN_INT(0);  /* Default timeout */
}

/* Form.setTicker(Ticker) */
static JavaValue native_form_setTicker(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* form = (JavaObject*)args[0].ref;
    JavaObject* ticker = (JavaObject*)args[1].ref;
    
    FORM_DEBUG("form_setTicker: form=%p, ticker=%p", (void*)form, (void*)ticker);
    
    /* Form has fields: title, items[], ticker — use dynamic slot lookup */
    if (form) {
        int ticker_slot = find_object_field_slot(form, "ticker");
        if (ticker_slot >= 0 && OBJECT_HAS_FIELDS(form, ticker_slot + 1)) {
            form->fields[ticker_slot].ref = ticker;
            FORM_DEBUG("form_setTicker: set ticker successfully");
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Form.getTicker() - returns the ticker */
static JavaValue native_form_getTicker(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* form = (JavaObject*)args[0].ref;
    
    /* Form has fields: title, items[], ticker — use dynamic slot lookup */
    if (form) {
        int ticker_slot = find_object_field_slot(form, "ticker");
        if (ticker_slot >= 0 && OBJECT_HAS_FIELDS(form, ticker_slot + 1)) {
            return NATIVE_RETURN_OBJECT(form->fields[ticker_slot].ref);
        }
    }
    
    return NATIVE_RETURN_NULL();
}

/* TextBox.<init>(String, String, int, int) */
static JavaValue native_textbox_init(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* textbox = (JavaObject*)args[0].ref;
    JavaString* title = (JavaString*)args[1].ref;
    JavaString* text = (JavaString*)args[2].ref;
    jint max_size = args[3].i;
    jint constraints = args[4].i;
    
    if (!textbox) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Initialize TextBox fields — use dynamic slot lookup */
    int title_slot = find_object_field_slot(textbox, "title");
    int text_slot = find_object_field_slot(textbox, "text");
    int maxsize_slot = find_object_field_slot(textbox, "maxSize");
    int constraints_slot = find_object_field_slot(textbox, "constraints");
    
    if (title_slot >= 0 && text_slot >= 0 && maxsize_slot >= 0 && constraints_slot >= 0 &&
        OBJECT_HAS_FIELDS(textbox, constraints_slot + 1)) {
        textbox->fields[title_slot].ref = title;
        textbox->fields[text_slot].ref = text;
        textbox->fields[maxsize_slot].i = max_size;
        textbox->fields[constraints_slot].i = constraints;
    }
    
    LOG_SAFE("[TextBox] <init>: title=%s, maxSize=%d, constraints=%d\n",
            title ? get_string_from_object(jvm, (JavaObject*)title) : "(null)",
            max_size, constraints);
    
    return NATIVE_RETURN_VOID();
}

/* Form.<init>(String) */
static JavaValue native_form_init(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* form = (JavaObject*)args[0].ref;
    JavaString* title = (JavaString*)args[1].ref;
    
    if (!form) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Initialize Form fields: title, items[] — use dynamic slot lookup */
    int title_slot = find_object_field_slot(form, "title");
    int items_slot = find_object_field_slot(form, "items");
    if (title_slot >= 0 && items_slot >= 0 &&
        OBJECT_HAS_FIELDS(form, items_slot + 1)) {
        form->fields[title_slot].ref = title;
        
        /* Create empty items array */
        JavaArray* items = jvm_new_array(jvm, DESC_OBJECT, 0, NULL);
        form->fields[items_slot].ref = items;
    }
    
    FORM_DEBUG("<init>: title=%s",
            title ? get_string_from_object(jvm, (JavaObject*)title) : "(null)");
    
    return NATIVE_RETURN_VOID();
}

/* ChoiceGroup.<init>(String, int) */
static JavaValue native_choicegroup_init(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* group = (JavaObject*)args[0].ref;
    JavaString* label = (JavaString*)args[1].ref;
    jint choice_type = args[2].i;
    
    if (!group) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Initialize ChoiceGroup fields — use dynamic slot lookup */
    int label_slot = find_object_field_slot(group, "label");
    int choicetype_slot = find_object_field_slot(group, "choiceType");
    int strings_slot = find_object_field_slot(group, "strings");
    int selected_slot = find_object_field_slot(group, "selected");
    
    if (label_slot >= 0 && choicetype_slot >= 0 && strings_slot >= 0 && selected_slot >= 0 &&
        OBJECT_HAS_FIELDS(group, selected_slot + 1)) {
        group->fields[label_slot].ref = label;
        group->fields[choicetype_slot].i = choice_type;
        
        /* Create empty arrays */
        JavaArray* strings = jvm_new_array(jvm, DESC_OBJECT, 0, NULL);
        group->fields[strings_slot].ref = strings;
        
        JavaArray* selected = jvm_new_array(jvm, T_BOOLEAN, 0, NULL);
        group->fields[selected_slot].ref = selected;
    }
    
    LOG_SAFE("[ChoiceGroup] <init>: label=%s, type=%d\n",
            label ? get_string_from_object(jvm, (JavaObject*)label) : "(null)",
            choice_type);
    
    return NATIVE_RETURN_VOID();
}

/* ChoiceGroup.<init>(String, int, String[], Image[]) */
static JavaValue native_choicegroup_init_array(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* group = (JavaObject*)args[0].ref;
    JavaString* label = (JavaString*)args[1].ref;
    jint choice_type = args[2].i;
    JavaArray* strings = (JavaArray*)args[3].ref;
    JavaArray* images = (JavaArray*)args[4].ref;
    
    if (!group) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Initialize ChoiceGroup fields — use dynamic slot lookup */
    int label_slot = find_object_field_slot(group, "label");
    int choicetype_slot = find_object_field_slot(group, "choiceType");
    int strings_slot = find_object_field_slot(group, "strings");
    int selected_slot = find_object_field_slot(group, "selected");
    
    if (label_slot >= 0 && choicetype_slot >= 0 && strings_slot >= 0 && selected_slot >= 0 &&
        OBJECT_HAS_FIELDS(group, selected_slot + 1)) {
        group->fields[label_slot].ref = label;
        group->fields[choicetype_slot].i = choice_type;
        group->fields[strings_slot].ref = strings;
        
        /* Create selection array */
        if (strings) {
            JavaArray* selected = jvm_new_array(jvm, T_BOOLEAN, strings->length, NULL);
            group->fields[selected_slot].ref = selected;
        }
    }
    
    (void)images; /* Not used for now */
    
    LOG_SAFE("[ChoiceGroup] <init>: label=%s, type=%d, items=%d\n",
            label ? get_string_from_object(jvm, (JavaObject*)label) : "(null)",
            choice_type, strings ? strings->length : 0);
    
    return NATIVE_RETURN_VOID();
}

/* Gauge.<init>(String, boolean, int, int) */
static JavaValue native_gauge_init(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* gauge = (JavaObject*)args[0].ref;
    JavaString* label = (JavaString*)args[1].ref;
    jboolean interactive = args[2].i;
    jint max_value = args[3].i;
    jint initial_value = args[4].i;
    
    if (!gauge) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Initialize Gauge fields — use dynamic slot lookup */
    int label_slot = find_object_field_slot(gauge, "label");
    int interactive_slot = find_object_field_slot(gauge, "interactive");
    int maxvalue_slot = find_object_field_slot(gauge, "maxValue");
    int value_slot = find_object_field_slot(gauge, "value");
    
    if (label_slot >= 0 && interactive_slot >= 0 && maxvalue_slot >= 0 && value_slot >= 0 &&
        OBJECT_HAS_FIELDS(gauge, value_slot + 1)) {
        gauge->fields[label_slot].ref = label;
        gauge->fields[interactive_slot].i = interactive;
        gauge->fields[maxvalue_slot].i = max_value;
        gauge->fields[value_slot].i = initial_value;
    }
    
    LOG_SAFE("[Gauge] <init>: label=%s, interactive=%d, max=%d, value=%d\n",
            label ? get_string_from_object(jvm, (JavaObject*)label) : "(null)",
            interactive, max_value, initial_value);
    
    return NATIVE_RETURN_VOID();
}

/* StringItem.<init>(String, String) */
static JavaValue native_stringitem_init(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    JavaString* label = (JavaString*)args[1].ref;
    JavaString* text = (JavaString*)args[2].ref;
    
    if (!item) {
        return NATIVE_RETURN_VOID();
    }
    
    int label_slot = find_object_field_slot(item, "label");
    int text_slot = find_object_field_slot(item, "text");
    if (label_slot >= 0 && text_slot >= 0 &&
        OBJECT_HAS_FIELDS(item, text_slot + 1)) {
        item->fields[label_slot].ref = label;
        item->fields[text_slot].ref = text;
    }
    
    return NATIVE_RETURN_VOID();
}

/* TextField.<init>(String, String, int, int) */
static JavaValue native_textfield_init(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    JavaString* label = (JavaString*)args[1].ref;
    JavaString* text = (JavaString*)args[2].ref;
    jint max_size = args[3].i;
    jint constraints = args[4].i;
    
    if (!item) {
        return NATIVE_RETURN_VOID();
    }
    
    int label_slot = find_object_field_slot(item, "label");
    int text_slot = find_object_field_slot(item, "text");
    int maxsize_slot = find_object_field_slot(item, "maxSize");
    int constraints_slot = find_object_field_slot(item, "constraints");
    
    if (label_slot >= 0 && text_slot >= 0 && maxsize_slot >= 0 && constraints_slot >= 0 &&
        OBJECT_HAS_FIELDS(item, constraints_slot + 1)) {
        item->fields[label_slot].ref = label;
        item->fields[text_slot].ref = text;
        item->fields[maxsize_slot].i = max_size;
        item->fields[constraints_slot].i = constraints;
    }
    
    LOG_SAFE("[TextField] <init>: label=%s, maxSize=%d\n",
            label ? get_string_from_object(jvm, (JavaObject*)label) : "(null)",
            max_size);
    
    return NATIVE_RETURN_VOID();
}

/* Spacer.<init>(int, int) */
static JavaValue native_spacer_init(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* spacer = (JavaObject*)args[0].ref;
    jint width = args[1].i;
    jint height = args[2].i;
    
    if (!spacer) {
        return NATIVE_RETURN_VOID();
    }
    
    int width_slot = find_object_field_slot(spacer, "width");
    int height_slot = find_object_field_slot(spacer, "height");
    if (width_slot >= 0 && height_slot >= 0 &&
        OBJECT_HAS_FIELDS(spacer, height_slot + 1)) {
        spacer->fields[width_slot].i = width;
        spacer->fields[height_slot].i = height;
    }
    
    return NATIVE_RETURN_VOID();
}

/* ImageItem.<init>(String, Image, int, String) */
static JavaValue native_imageitem_init(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* item = (JavaObject*)args[0].ref;
    JavaString* label = (JavaString*)args[1].ref;
    JavaObject* image = (JavaObject*)args[2].ref;
    jint layout = args[3].i;
    JavaString* alt_text = (JavaString*)args[4].ref;
    
    if (!item) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Initialize ImageItem fields — use dynamic slot lookup */
    int label_slot = find_object_field_slot(item, "label");
    int image_slot = find_object_field_slot(item, "image");
    int layout_slot = find_object_field_slot(item, "layout");
    int alttext_slot = find_object_field_slot(item, "altText");
    
    if (label_slot >= 0 && image_slot >= 0 && layout_slot >= 0 && alttext_slot >= 0 &&
        OBJECT_HAS_FIELDS(item, alttext_slot + 1)) {
        item->fields[label_slot].ref = label;
        item->fields[image_slot].ref = image;
        item->fields[layout_slot].i = layout;
        item->fields[alttext_slot].ref = alt_text;
    }
    
    return NATIVE_RETURN_VOID();
}

/* List.size() */
static JavaValue native_list_size(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* list = (JavaObject*)args[0].ref;
    
    /* Get field offset considering inheritance */
    if (list) {
        JavaClass* list_class = list->header.clazz;
        int strings_idx = get_field_offset(list_class, "strings");
        
        if (strings_idx >= 0) {
            JavaArray* strings = (JavaArray*)list->fields[strings_idx].ref;
            if (strings) {
                return NATIVE_RETURN_INT(strings->length);
            }
        }
    }
    
    return NATIVE_RETURN_INT(0);
}

/* List.getString(int) */
static JavaValue native_list_getString(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* list = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    
    /* Get field offset considering inheritance */
    if (list) {
        JavaClass* list_class = list->header.clazz;
        int strings_idx = get_field_offset(list_class, "strings");
        
        if (strings_idx >= 0) {
            JavaArray* strings = (JavaArray*)list->fields[strings_idx].ref;
            if (strings && index >= 0 && index < strings->length) {
                void** strings_data = (void**)array_data(strings);
                return NATIVE_RETURN_OBJECT(strings_data[index]);
            }
        }
    }
    
    return NATIVE_RETURN_NULL();
}

/* Alert.getTitle() */
static JavaValue native_alert_getTitle(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* alert = (JavaObject*)args[0].ref;
    
    LOG_SAFE("[ALERT_GETTITLE] alert=%p, fields_count=%d\n", 
            (void*)alert, alert ? alert->header.clazz->fields_count : -1);
    
    if (alert) {
        int title_slot = find_object_field_slot(alert, "title");
        if (title_slot >= 0 && OBJECT_HAS_FIELDS(alert, title_slot + 1)) {
            JavaString* title = (JavaString*)alert->fields[title_slot].ref;
            LOG_SAFE("[ALERT_GETTITLE] title=%p, title_str='%s'\n", 
                    (void*)title, title && title->utf8 ? title->utf8 : "NULL");
            return NATIVE_RETURN_OBJECT(alert->fields[title_slot].ref);
        }
    }
    
    LOG_SAFE("[ALERT_GETTITLE] returning NULL\n");
    return NATIVE_RETURN_NULL();
}

/* Alert.getString() */
static JavaValue native_alert_getString(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* alert = (JavaObject*)args[0].ref;
    
    if (alert) {
        int text_slot = find_object_field_slot(alert, "text");
        if (text_slot >= 0 && OBJECT_HAS_FIELDS(alert, text_slot + 1)) {
            return NATIVE_RETURN_OBJECT(alert->fields[text_slot].ref);
        }
    }
    
    return NATIVE_RETURN_NULL();
}

/* Alert.setTitle(String) */
static JavaValue native_alert_setTitle(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* alert = (JavaObject*)args[0].ref;
    JavaString* title = (JavaString*)args[1].ref;
    
    if (alert) {
        int title_slot = find_object_field_slot(alert, "title");
        if (title_slot >= 0 && OBJECT_HAS_FIELDS(alert, title_slot + 1)) {
            alert->fields[title_slot].ref = title;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* TextBox.getTitle() */
static JavaValue native_textbox_getTitle(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* textbox = (JavaObject*)args[0].ref;
    
    if (textbox) {
        int title_slot = find_object_field_slot(textbox, "title");
        if (title_slot >= 0 && OBJECT_HAS_FIELDS(textbox, title_slot + 1)) {
            return NATIVE_RETURN_OBJECT(textbox->fields[title_slot].ref);
        }
    }
    
    return NATIVE_RETURN_NULL();
}

/* TextBox.setTitle(String) */
static JavaValue native_textbox_setTitle(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* textbox = (JavaObject*)args[0].ref;
    JavaString* title = (JavaString*)args[1].ref;
    
    if (textbox) {
        int title_slot = find_object_field_slot(textbox, "title");
        if (title_slot >= 0 && OBJECT_HAS_FIELDS(textbox, title_slot + 1)) {
            textbox->fields[title_slot].ref = title;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* TextBox.getMaxSize() */
static JavaValue native_textbox_getMaxSize(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* textbox = (JavaObject*)args[0].ref;
    
    if (textbox) {
        int maxsize_slot = find_object_field_slot(textbox, "maxSize");
        if (maxsize_slot >= 0 && OBJECT_HAS_FIELDS(textbox, maxsize_slot + 1)) {
            return NATIVE_RETURN_INT(textbox->fields[maxsize_slot].i);
        }
    }
    
    return NATIVE_RETURN_INT(32);
}

/* TextBox.getConstraints() */
static JavaValue native_textbox_getConstraints(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* textbox = (JavaObject*)args[0].ref;
    
    if (textbox) {
        int constraints_slot = find_object_field_slot(textbox, "constraints");
        if (constraints_slot >= 0 && OBJECT_HAS_FIELDS(textbox, constraints_slot + 1)) {
            return NATIVE_RETURN_INT(textbox->fields[constraints_slot].i);
        }
    }
    
    return NATIVE_RETURN_INT(0);
}

/* Choice.size() - for both List and ChoiceGroup */
static JavaValue native_choice_size(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* choice = (JavaObject*)args[0].ref;
    
    if (choice) {
        int strings_slot = find_object_field_slot(choice, "strings");
        if (strings_slot >= 0 && OBJECT_HAS_FIELDS(choice, strings_slot + 1)) {
            JavaArray* strings = (JavaArray*)choice->fields[strings_slot].ref;
            if (strings) {
                return NATIVE_RETURN_INT(strings->length);
            }
        }
    }
    
    return NATIVE_RETURN_INT(0);
}

/* Choice.getString(int) */
static JavaValue native_choice_getString(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* choice = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    
    if (choice) {
        int strings_slot = find_object_field_slot(choice, "strings");
        if (strings_slot >= 0 && OBJECT_HAS_FIELDS(choice, strings_slot + 1)) {
            JavaArray* strings = (JavaArray*)choice->fields[strings_slot].ref;
            if (strings && index >= 0 && index < strings->length) {
                void** strings_data = (void**)array_data(strings);
                return NATIVE_RETURN_OBJECT(strings_data[index]);
            }
        }
    }
    
    return NATIVE_RETURN_NULL();
}

/* Choice.isSelected(int) */
static JavaValue native_choice_isSelected(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* choice = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    
    if (choice) {
        JavaClass* choice_class = choice->header.clazz;
        int selected_idx = get_field_offset(choice_class, "selected");
        
        if (selected_idx >= 0) {
            JavaArray* selected = (JavaArray*)choice->fields[selected_idx].ref;
            if (selected && selected->element_type == T_BOOLEAN && 
                index >= 0 && index < selected->length) {
                jboolean* selected_data = (jboolean*)array_data(selected);
                return NATIVE_RETURN_INT(selected_data[index]);
            }
        }
    }
    
    return NATIVE_RETURN_INT(0);
}

/* Form.deleteAll() */
static JavaValue native_form_deleteAll(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* form = (JavaObject*)args[0].ref;
    
    if (form) {
        int items_slot = find_object_field_slot(form, "items");
        if (items_slot >= 0 && OBJECT_HAS_FIELDS(form, items_slot + 1)) {
            /* Create new empty items array */
            JavaArray* items = jvm_new_array(jvm, DESC_OBJECT, 0, NULL);
            form->fields[items_slot].ref = items;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Ticker.<init>(String) */
static JavaValue native_ticker_init(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* ticker = (JavaObject*)args[0].ref;
    JavaString* text = (JavaString*)args[1].ref;
    
    if (ticker && OBJECT_HAS_FIELDS(ticker, 1)) {
        ticker->fields[0].ref = text;
    }
    
    return NATIVE_RETURN_VOID();
}

/* Ticker.getString() */
static JavaValue native_ticker_getString(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ticker = (JavaObject*)args[0].ref;
    
    if (ticker && OBJECT_HAS_FIELDS(ticker, 1)) {
        return NATIVE_RETURN_OBJECT(ticker->fields[0].ref);
    }
    
    return NATIVE_RETURN_NULL();
}

/* Ticker.setString(String) */
static JavaValue native_ticker_setString(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ticker = (JavaObject*)args[0].ref;
    JavaString* text = (JavaString*)args[1].ref;
    
    if (ticker && OBJECT_HAS_FIELDS(ticker, 1)) {
        ticker->fields[0].ref = text;
    }
    
    return NATIVE_RETURN_VOID();
}

/* DateField.<init>(String, int) */
static JavaValue native_datefield_init(JVM* jvm, JavaThread* thread, JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* field = (JavaObject*)args[0].ref;
    JavaString* label = (JavaString*)args[1].ref;
    jint mode = args[2].i;
    
    if (field) {
        int label_slot = find_object_field_slot(field, "label");
        int mode_slot = find_object_field_slot(field, "mode");
        int date_slot = find_object_field_slot(field, "date");
        if (label_slot >= 0 && mode_slot >= 0 && date_slot >= 0 &&
            OBJECT_HAS_FIELDS(field, date_slot + 1)) {
            field->fields[label_slot].ref = label;
            field->fields[mode_slot].i = mode;
            field->fields[date_slot].j = 0; /* Date value */
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Re-register with additional methods */
void init_javax_microedition_lcdui_form(JVM* jvm) {
    /* Register global objects as GC roots */
    gc_add_root(jvm, (void**)&g_current_form);
    gc_add_root(jvm, (void**)&g_current_displayable);
    gc_add_root(jvm, (void**)&g_select_command);
    gc_add_root(jvm, (void**)&g_item_state_listener);
    
    NativeMethodEntry methods[] = {
        /* Form methods */
        {"javax/microedition/lcdui/Form", "<init>", "(Ljava/lang/String;)V", native_form_init},
        {"javax/microedition/lcdui/Form", "append", "(Ljavax/microedition/lcdui/Item;)I", native_form_append},
        {"javax/microedition/lcdui/Form", "delete", "(I)V", native_form_delete},
        {"javax/microedition/lcdui/Form", "deleteAll", "()V", native_form_deleteAll},
        {"javax/microedition/lcdui/Form", "size", "()I", native_form_size},
        {"javax/microedition/lcdui/Form", "get", "(I)Ljavax/microedition/lcdui/Item;", native_form_get},
        {"javax/microedition/lcdui/Form", "getTitle", "()Ljava/lang/String;", native_form_getTitle},
        {"javax/microedition/lcdui/Form", "setTitle", "(Ljava/lang/String;)V", native_form_setTitle},
        {"javax/microedition/lcdui/Form", "setItemStateListener", "(Ljavax/microedition/lcdui/ItemStateListener;)V", native_form_setItemStateListener},
        {"javax/microedition/lcdui/Form", "setTicker", "(Ljavax/microedition/lcdui/Ticker;)V", native_form_setTicker},
        {"javax/microedition/lcdui/Form", "getTicker", "()Ljavax/microedition/lcdui/Ticker;", native_form_getTicker},
        
        /* Item methods */
        {"javax/microedition/lcdui/Item", "setLabel", "(Ljava/lang/String;)V", native_item_setLabel},
        {"javax/microedition/lcdui/Item", "getLabel", "()Ljava/lang/String;", native_item_getLabel},
        
        /* StringItem methods */
        {"javax/microedition/lcdui/StringItem", "<init>", "(Ljava/lang/String;Ljava/lang/String;)V", native_stringitem_init},
        {"javax/microedition/lcdui/StringItem", "setText", "(Ljava/lang/String;)V", native_stringitem_setText},
        {"javax/microedition/lcdui/StringItem", "getText", "()Ljava/lang/String;", native_stringitem_getText},
        
        /* TextField methods */
        {"javax/microedition/lcdui/TextField", "<init>", "(Ljava/lang/String;Ljava/lang/String;II)V", native_textfield_init},
        {"javax/microedition/lcdui/TextField", "setString", "(Ljava/lang/String;)V", native_textfield_setString},
        {"javax/microedition/lcdui/TextField", "getString", "()Ljava/lang/String;", native_textfield_getString},
        {"javax/microedition/lcdui/TextField", "setChars", "([CII)V", native_textfield_setChars},
        {"javax/microedition/lcdui/TextField", "size", "()I", native_textfield_size},
        {"javax/microedition/lcdui/TextField", "getMaxSize", "()I", native_textfield_getMaxSize},
        {"javax/microedition/lcdui/TextField", "setMaxSize", "(I)I", native_textfield_setMaxSize},
        {"javax/microedition/lcdui/TextField", "getConstraints", "()I", native_textfield_getConstraints},
        {"javax/microedition/lcdui/TextField", "setConstraints", "(I)V", native_textfield_setConstraints},
        {"javax/microedition/lcdui/TextField", "getChars", "([C)V", native_textfield_getChars},
        {"javax/microedition/lcdui/TextField", "delete", "(II)V", native_textfield_delete},
        {"javax/microedition/lcdui/TextField", "insert", "(Ljava/lang/String;I)V", native_textfield_insert},
        {"javax/microedition/lcdui/TextField", "getCaretPosition", "()I", native_textfield_getCaretPosition},
        
        /* ChoiceGroup methods */
        {"javax/microedition/lcdui/ChoiceGroup", "<init>", "(Ljava/lang/String;I)V", native_choicegroup_init},
        {"javax/microedition/lcdui/ChoiceGroup", "<init>", "(Ljava/lang/String;I[Ljava/lang/String;[Ljavax/microedition/lcdui/Image;)V", native_choicegroup_init_array},
        {"javax/microedition/lcdui/ChoiceGroup", "append", "(Ljava/lang/String;Ljavax/microedition/lcdui/Image;)I", native_choicegroup_append},
        {"javax/microedition/lcdui/ChoiceGroup", "setSelectedIndex", "(IZ)V", native_choicegroup_setSelectedIndex},
        {"javax/microedition/lcdui/ChoiceGroup", "getSelectedIndex", "()I", native_choicegroup_getSelectedIndex},
        {"javax/microedition/lcdui/ChoiceGroup", "size", "()I", native_choice_size},
        {"javax/microedition/lcdui/ChoiceGroup", "getString", "(I)Ljava/lang/String;", native_choice_getString},
        {"javax/microedition/lcdui/ChoiceGroup", "isSelected", "(I)Z", native_choice_isSelected},
        
        /* Gauge methods */
        {"javax/microedition/lcdui/Gauge", "<init>", "(Ljava/lang/String;ZII)V", native_gauge_init},
        {"javax/microedition/lcdui/Gauge", "setValue", "(I)V", native_gauge_setValue},
        {"javax/microedition/lcdui/Gauge", "getValue", "()I", native_gauge_getValue},
        
        /* List methods */
        {"javax/microedition/lcdui/List", "<init>", "(Ljava/lang/String;I)V", native_list_init},
        {"javax/microedition/lcdui/List", "<init>", "(Ljava/lang/String;I[Ljava/lang/String;[Ljavax/microedition/lcdui/Image;)V", native_list_init_with_elements},
        {"javax/microedition/lcdui/List", "append", "(Ljava/lang/String;Ljavax/microedition/lcdui/Image;)I", native_list_append},
        {"javax/microedition/lcdui/List", "getSelectedIndex", "()I", native_list_getSelectedIndex},
        {"javax/microedition/lcdui/List", "setSelectedIndex", "(IZ)V", native_list_setSelectedIndex},
        {"javax/microedition/lcdui/List", "delete", "(I)V", native_list_delete},
        {"javax/microedition/lcdui/List", "set", "(ILjava/lang/String;Ljavax/microedition/lcdui/Image;)V", native_list_set},
        {"javax/microedition/lcdui/List", "size", "()I", native_list_size},
        {"javax/microedition/lcdui/List", "getString", "(I)Ljava/lang/String;", native_list_getString},
        {"javax/microedition/lcdui/List", "isSelected", "(I)Z", native_choice_isSelected},
        
        /* Alert methods */
        {"javax/microedition/lcdui/Alert", "<init>", "(Ljava/lang/String;Ljava/lang/String;Ljavax/microedition/lcdui/Image;Ljavax/microedition/lcdui/AlertType;)V", native_alert_init},
        {"javax/microedition/lcdui/Alert", "setString", "(Ljava/lang/String;)V", native_alert_setString},
        {"javax/microedition/lcdui/Alert", "getString", "()Ljava/lang/String;", native_alert_getString},
        {"javax/microedition/lcdui/Alert", "setTitle", "(Ljava/lang/String;)V", native_alert_setTitle},
        {"javax/microedition/lcdui/Alert", "getTitle", "()Ljava/lang/String;", native_alert_getTitle},
        {"javax/microedition/lcdui/Alert", "setTimeout", "(I)V", native_alert_setTimeout},
        {"javax/microedition/lcdui/Alert", "getTimeout", "()I", native_alert_getTimeout},
        
        /* TextBox methods */
        {"javax/microedition/lcdui/TextBox", "<init>", "(Ljava/lang/String;Ljava/lang/String;II)V", native_textbox_init},
        {"javax/microedition/lcdui/TextBox", "setString", "(Ljava/lang/String;)V", native_textbox_setString},
        {"javax/microedition/lcdui/TextBox", "getString", "()Ljava/lang/String;", native_textbox_getString},
        {"javax/microedition/lcdui/TextBox", "setTitle", "(Ljava/lang/String;)V", native_textbox_setTitle},
        {"javax/microedition/lcdui/TextBox", "getTitle", "()Ljava/lang/String;", native_textbox_getTitle},
        {"javax/microedition/lcdui/TextBox", "getMaxSize", "()I", native_textbox_getMaxSize},
        {"javax/microedition/lcdui/TextBox", "getConstraints", "()I", native_textbox_getConstraints},
        
        /* Spacer methods */
        {"javax/microedition/lcdui/Spacer", "<init>", "(II)V", native_spacer_init},
        {"javax/microedition/lcdui/Spacer", "setMinimumSize", "(II)V", native_spacer_setMinimumSize},
        
        /* ImageItem methods */
        {"javax/microedition/lcdui/ImageItem", "<init>", "(Ljava/lang/String;Ljavax/microedition/lcdui/Image;ILjava/lang/String;)V", native_imageitem_init},
        {"javax/microedition/lcdui/ImageItem", "setImage", "(Ljavax/microedition/lcdui/Image;)V", native_imageitem_setImage},
        
        /* Command methods */
        {"javax/microedition/lcdui/Command", "<init>", "(Ljava/lang/String;II)V", native_command_init},
        {"javax/microedition/lcdui/Command", "getCommandType", "()I", native_command_getCommandType},
        {"javax/microedition/lcdui/Command", "getLabel", "()Ljava/lang/String;", native_command_getLabel},
        {"javax/microedition/lcdui/Command", "getPriority", "()I", native_command_getPriority},
        
        /* Displayable methods */
        {"javax/microedition/lcdui/Displayable", "addCommand", "(Ljavax/microedition/lcdui/Command;)V", native_displayable_addCommand},
        {"javax/microedition/lcdui/Displayable", "removeCommand", "(Ljavax/microedition/lcdui/Command;)V", native_displayable_removeCommand},
        {"javax/microedition/lcdui/Displayable", "setCommandListener", "(Ljavax/microedition/lcdui/CommandListener;)V", native_displayable_setCommandListener},
        
        /* Ticker methods */
        {"javax/microedition/lcdui/Ticker", "<init>", "(Ljava/lang/String;)V", native_ticker_init},
        {"javax/microedition/lcdui/Ticker", "getString", "()Ljava/lang/String;", native_ticker_getString},
        {"javax/microedition/lcdui/Ticker", "setString", "(Ljava/lang/String;)V", native_ticker_setString},
        
        /* DateField methods */
        {"javax/microedition/lcdui/DateField", "<init>", "(Ljava/lang/String;I)V", native_datefield_init},
    };
    
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    
    FORM_DEBUG("Registered %zu UI native methods", sizeof(methods) / sizeof(methods[0]));
    
    /* Initialize List.SELECT_COMMAND static field */
    JavaClass* list_class = jvm_load_class(jvm, "javax/microedition/lcdui/List");
    if (list_class && list_class->static_fields_count > 0) {
        for (int i = 0; i < list_class->static_fields_count; i++) {
            if (list_class->static_fields[i].name && 
                strcmp(list_class->static_fields[i].name, "SELECT_COMMAND") == 0) {
                /* Create SELECT_COMMAND = new Command("", 4, 0) */
                JavaClass* cmd_class = jvm_load_class(jvm, "javax/microedition/lcdui/Command");
                if (cmd_class) {
                    JavaObject* select_cmd = jvm_new_object(jvm, cmd_class);
                    if (select_cmd && OBJECT_HAS_FIELDS(select_cmd, 3)) {
                        JavaString* empty_label = jvm_new_string(jvm, "");
                        select_cmd->fields[0].ref = empty_label;  /* label */
                        select_cmd->fields[1].i = 4;              /* SCREEN type */
                        select_cmd->fields[2].i = 0;              /* priority */
                        
                        list_class->static_fields[i].value.ref = select_cmd;
                        FORM_DEBUG("Initialized List.SELECT_COMMAND = %p", (void*)select_cmd);
                    }
                }
                break;
            }
        }
    }
}
