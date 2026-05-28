import {
    array_data,
    string_utf8,
    string_chars,
    T_BOOLEAN,
    T_CHAR,
    DESC_OBJECT,
    ACC_STATIC,
    OBJ_TYPE_ARRAY,
    gc_add_root,
} from '../jvm/heap.mjs';

import {
    midp_graphics_init,
    midp_graphics_set_color,
    midp_graphics_fill_rect,
    midp_graphics_draw_rect,
    midp_graphics_draw_line,
    midp_graphics_draw_string,
    midp_graphics_draw_arc,
    midp_graphics_fill_arc,
    midp_font_get_default,
    midp_font_height,
    midp_font_string_width,
} from './graphics.mjs';

import {
    jvm_new_string,
    jvm_new_string_utf16,
    jvm_new_array,
    jvm_new_object,
    jvm_load_class,
    jvm_resolve_method,
    jvm_current_thread,
} from '../jvm/jvm.mjs';

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

export const ITEM_STRINGITEM  = 1;
export const ITEM_TEXTFIELD   = 2;
export const ITEM_IMAGEITEM   = 3;
export const ITEM_CHOICEGROUP = 4;
export const ITEM_GAUGE       = 5;
export const ITEM_SPACER      = 6;
export const ITEM_DATEFIELD   = 7;

export const CHOICE_EXCLUSIVE = 1;
export const CHOICE_MULTIPLE  = 2;
export const CHOICE_IMPLICIT  = 3;
export const CHOICE_POPUP     = 4;

export const TEXTFIELD_ANY         = 0;
export const TEXTFIELD_EMAILADDR   = 1;
export const TEXTFIELD_NUMERIC     = 2;
export const TEXTFIELD_PHONENUMBER = 3;
export const TEXTFIELD_URL         = 4;
export const TEXTFIELD_DECIMAL     = 5;
export const TEXTFIELD_PASSWORD    = 0x10000;

const VK_CHARS_PER_ROW = 10;
const VK_CHAR_COUNT    = 48;

// ---------------------------------------------------------------------------
// Injected callbacks (SDL backend + execute_method)
// ---------------------------------------------------------------------------

let _sdl_get_global_context  = () => null;
let _sdl_request_redraw      = () => {};
let _sdl_get_ticks           = (_ctx) => BigInt(Date.now());
let _execute_method          = (_jvm, _thread, _method, _args, _result) => 0;
let _call_command_action_for_list = (_jvm, _list, _idx) => false;

export function set_form_callbacks(cbs) {
    if (cbs.sdlGetGlobalContext)      _sdl_get_global_context       = cbs.sdlGetGlobalContext;
    if (cbs.sdlRequestRedraw)         _sdl_request_redraw            = cbs.sdlRequestRedraw;
    if (cbs.sdlGetTicks)              _sdl_get_ticks                 = cbs.sdlGetTicks;
    if (cbs.executeMethod)            _execute_method                = cbs.executeMethod;
    if (cbs.callCommandActionForList) _call_command_action_for_list  = cbs.callCommandActionForList;
}

// ---------------------------------------------------------------------------
// Module-level state (mirrors C file-scope statics)
// ---------------------------------------------------------------------------

let g_current_form           = null;
let g_current_displayable    = null;
let g_focused_item_index     = 0;
let g_list_selected_index    = 0;
let g_cg_focused_element     = 0;
let g_select_command         = null;
let g_item_state_listener    = null;

const g_vkb = {
    active:        false,
    selected_char: 0,
    text_buffer:   '',
    text_length:   0,
    cursor_pos:    0,
    max_length:    255,
    constraints:   0,
    target_item:   null,
    displayable:   null,
};

// Alert timeout tracking
let _alert_start_time = 0n;
let _last_alert       = null;

// ---------------------------------------------------------------------------
// Helper: OBJECT_HAS_FIELDS equivalent
// ---------------------------------------------------------------------------

function OBJECT_HAS_FIELDS(obj, count) {
    if (!obj || !obj.fields) return false;
    return obj.fields.length >= count;
}

// ---------------------------------------------------------------------------
// Helper: find_object_field_slot (walk class hierarchy)
// ---------------------------------------------------------------------------

function find_object_field_slot(obj, field_name) {
    if (!obj || !field_name) return -1;
    const clazz = obj.header && obj.header.clazz;
    if (!clazz) return -1;

    const hierarchy = [];
    let c = clazz;
    while (c && hierarchy.length < 64) {
        hierarchy.push(c);
        c = c.super_class || null;
    }

    let slot = 0;
    for (let h = hierarchy.length - 1; h >= 0; h--) {
        const current = hierarchy[h];
        if (!current.fields) continue;
        for (let i = 0; i < current.fields_count; i++) {
            const field = current.fields[i];
            if (!field) continue;
            if (field.access_flags & ACC_STATIC) continue;
            if (field.name === field_name) return slot;
            slot++;
            if (field.descriptor && (field.descriptor[0] === 'J' || field.descriptor[0] === 'D')) {
                slot++;
            }
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Helper: get_field_offset (same logic, takes class directly)
// ---------------------------------------------------------------------------

function get_field_offset(clazz, field_name) {
    if (!clazz || !field_name) return -1;
    const hierarchy = [];
    let c = clazz;
    while (c && hierarchy.length < 64) {
        hierarchy.push(c);
        c = c.super_class || null;
    }
    let slot = 0;
    for (let h = hierarchy.length - 1; h >= 0; h--) {
        const current = hierarchy[h];
        if (!current.fields) continue;
        for (let i = 0; i < current.fields_count; i++) {
            const field = current.fields[i];
            if (!field) continue;
            if (field.access_flags & ACC_STATIC) continue;
            if (field.name === field_name) return slot;
            slot++;
            if (field.descriptor && (field.descriptor[0] === 'J' || field.descriptor[0] === 'D')) {
                slot++;
            }
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Helper: get_string_from_object
// ---------------------------------------------------------------------------

function get_string_from_object(jvm, str_obj) {
    if (!str_obj) return '';
    const result = string_utf8(jvm, str_obj);
    return result || '';
}

// ---------------------------------------------------------------------------
// Helper: get_screen_graphics
// ---------------------------------------------------------------------------

function get_screen_graphics() {
    const sdl_ctx = _sdl_get_global_context();
    if (!sdl_ctx || !sdl_ctx.framebuffer) return null;
    const gfx = {};
    midp_graphics_init(gfx, sdl_ctx.framebuffer, sdl_ctx.width, sdl_ctx.height);
    return gfx;
}

// ---------------------------------------------------------------------------
// Helper: get_item_type
// ---------------------------------------------------------------------------

function get_item_type(item) {
    if (!item || !item.header || !item.header.clazz) return 0;
    const cn = item.header.clazz.class_name;
    if (!cn) return 0;
    if (cn.includes('StringItem'))  return ITEM_STRINGITEM;
    if (cn.includes('TextField'))   return ITEM_TEXTFIELD;
    if (cn.includes('ImageItem'))   return ITEM_IMAGEITEM;
    if (cn.includes('ChoiceGroup')) return ITEM_CHOICEGROUP;
    if (cn.includes('Gauge'))       return ITEM_GAUGE;
    if (cn.includes('Spacer'))      return ITEM_SPACER;
    if (cn.includes('DateField'))   return ITEM_DATEFIELD;
    return 0;
}

// ---------------------------------------------------------------------------
// Virtual keyboard
// ---------------------------------------------------------------------------

function vkb_start(item, max_size, constraints) {
    g_vkb.active        = true;
    g_vkb.selected_char = 0;
    g_vkb.text_buffer   = '';
    g_vkb.text_length   = 0;
    g_vkb.cursor_pos    = 0;
    g_vkb.max_length    = Math.min(max_size, 255);
    g_vkb.constraints   = constraints;
    g_vkb.target_item   = item;

    if (item) {
        const text_slot = find_object_field_slot(item, 'text');
        if (text_slot >= 0 && OBJECT_HAS_FIELDS(item, text_slot + 1)) {
            const text_str = item.fields[text_slot].ref;
            if (text_str && text_str.utf8) {
                g_vkb.text_buffer = text_str.utf8.slice(0, 255);
                g_vkb.text_length = g_vkb.text_buffer.length;
                g_vkb.cursor_pos  = g_vkb.text_length;
            }
        }
    }
}

function vkb_close(jvm, save) {
    if (save && g_vkb.target_item && jvm) {
        const text_str = jvm_new_string(jvm, g_vkb.text_buffer);
        if (text_str) {
            const text_slot = find_object_field_slot(g_vkb.target_item, 'text');
            if (text_slot >= 0 && OBJECT_HAS_FIELDS(g_vkb.target_item, text_slot + 1)) {
                g_vkb.target_item.fields[text_slot].ref = text_str;
                if (g_item_state_listener && g_item_state_listener.header && g_item_state_listener.header.clazz) {
                    const ism = jvm_resolve_method(jvm, g_item_state_listener.header.clazz,
                        'itemStateChanged', '(Ljavax/microedition/lcdui/Item;)V');
                    if (ism) {
                        const ism_args = [
                            { ref: g_item_state_listener },
                            { ref: g_vkb.target_item },
                        ];
                        _execute_method(jvm, jvm_current_thread(jvm), ism, ism_args, {});
                    }
                }
            }
        }
    }
    g_vkb.active      = false;
    g_vkb.target_item = null;
}

function vkb_handle_key(jvm, game_action) {
    if (!g_vkb.active) return false;

    const SYMBOLS = ' .,!?@_-:/';

    switch (game_action) {
        case 1: // UP
            g_vkb.selected_char -= VK_CHARS_PER_ROW;
            if (g_vkb.selected_char < 0) g_vkb.selected_char += VK_CHAR_COUNT;
            break;
        case 6: // DOWN
            g_vkb.selected_char += VK_CHARS_PER_ROW;
            if (g_vkb.selected_char >= VK_CHAR_COUNT) g_vkb.selected_char -= VK_CHAR_COUNT;
            break;
        case 2: // LEFT
            g_vkb.selected_char--;
            if (g_vkb.selected_char < 0) g_vkb.selected_char = VK_CHAR_COUNT - 1;
            break;
        case 5: // RIGHT
            g_vkb.selected_char++;
            if (g_vkb.selected_char >= VK_CHAR_COUNT) g_vkb.selected_char = 0;
            break;
        case 8: { // FIRE
            const idx = g_vkb.selected_char;
            if (idx >= 47) {
                vkb_close(jvm, true);
            } else if (idx >= 46) {
                if (g_vkb.text_length > 0 && g_vkb.cursor_pos > 0) {
                    g_vkb.text_buffer = g_vkb.text_buffer.slice(0, g_vkb.cursor_pos - 1) +
                                        g_vkb.text_buffer.slice(g_vkb.cursor_pos);
                    g_vkb.text_length--;
                    g_vkb.cursor_pos--;
                }
            } else if (g_vkb.text_length < g_vkb.max_length) {
                let ch;
                if (idx < 26)       ch = String.fromCharCode(65 + idx);
                else if (idx < 36)  ch = String.fromCharCode(48 + idx - 26);
                else                ch = SYMBOLS[idx - 36] || '';
                if (ch) {
                    g_vkb.text_buffer = g_vkb.text_buffer.slice(0, g_vkb.cursor_pos) + ch +
                                        g_vkb.text_buffer.slice(g_vkb.cursor_pos);
                    g_vkb.text_length++;
                    g_vkb.cursor_pos++;
                }
            }
            break;
        }
        default:
            return false;
    }
    return true;
}

function vkb_render(gfx) {
    if (!g_vkb.active || !gfx) return;
    const screen_w = gfx.width;
    const screen_h = gfx.height;
    const kb_y = screen_h - 160;
    const kb_h = 150;

    midp_graphics_set_color(gfx, 0xD0D0D0, 255);
    midp_graphics_fill_rect(gfx, 0, kb_y, screen_w, kb_h);
    midp_graphics_set_color(gfx, 0x000000, 255);
    midp_graphics_draw_rect(gfx, 0, kb_y, screen_w, kb_h);

    const text_y = kb_y + 5;
    midp_graphics_set_color(gfx, 0xFFFFFF, 255);
    midp_graphics_fill_rect(gfx, 5, text_y, screen_w - 10, 20);
    midp_graphics_set_color(gfx, 0x000000, 255);
    midp_graphics_draw_rect(gfx, 5, text_y, screen_w - 10, 20);
    midp_graphics_draw_string(gfx, g_vkb.text_buffer, 8, text_y + 3, 0);

    const cursor_x = 8 + g_vkb.cursor_pos * 6;
    midp_graphics_draw_line(gfx, cursor_x, text_y + 2, cursor_x, text_y + 18);

    const grid_y = text_y + 25;
    const cell_w = Math.floor((screen_w - 10) / VK_CHARS_PER_ROW);
    const cell_h = 16;
    const SYMBOLS = ' .,!?@_-:/';

    for (let i = 0; i < VK_CHAR_COUNT; i++) {
        const row = Math.floor(i / VK_CHARS_PER_ROW);
        const col = i % VK_CHARS_PER_ROW;
        const cell_x = 5 + col * cell_w;
        const cell_y = grid_y + row * cell_h;

        if (i === g_vkb.selected_char) {
            midp_graphics_set_color(gfx, 0x000080, 255);
            midp_graphics_fill_rect(gfx, cell_x, cell_y, cell_w - 2, cell_h - 2);
            midp_graphics_set_color(gfx, 0xFFFFFF, 255);
        } else {
            midp_graphics_set_color(gfx, 0x000000, 255);
        }

        let ch_str;
        if (i < 26)       ch_str = String.fromCharCode(65 + i);
        else if (i < 36)  ch_str = String.fromCharCode(48 + i - 26);
        else if (i < 46)  ch_str = SYMBOLS[i - 36] || ' ';
        else if (i === 46) ch_str = 'DEL';
        else               ch_str = 'OK';

        midp_graphics_draw_string(gfx, ch_str, cell_x + 2, cell_y + 1, 0);
    }

    midp_graphics_set_color(gfx, 0x404040, 255);
    midp_graphics_draw_string(gfx, 'Arrows:Move Fire:Select', 5, kb_y + kb_h - 15, 0);
}

export function midp_is_vkb_active() {
    return g_vkb.active;
}

export function midp_vkb_process_key(jvm, game_action) {
    return vkb_handle_key(jvm, game_action);
}

// ---------------------------------------------------------------------------
// Render: StringItem
// ---------------------------------------------------------------------------

function render_stringitem(jvm, gfx, item, y) {
    let label = '';
    let text  = '';
    const label_slot = find_object_field_slot(item, 'label');
    const text_slot  = find_object_field_slot(item, 'text');
    if (label_slot >= 0 && text_slot >= 0 && OBJECT_HAS_FIELDS(item, Math.max(label_slot, text_slot) + 1)) {
        const ls = item.fields[label_slot].ref;
        const ts = item.fields[text_slot].ref;
        if (ls) label = get_string_from_object(jvm, ls);
        if (ts) text  = get_string_from_object(jvm, ts);
    }
    const font = midp_font_get_default();
    const fh   = font ? midp_font_height(font) : 12;
    if (label) {
        midp_graphics_set_color(gfx, 0x0000FF, 255);
        midp_graphics_draw_string(gfx, label, 5, y, 0);
        y += fh + 2;
    }
    if (text) {
        midp_graphics_set_color(gfx, 0x000000, 255);
        midp_graphics_draw_string(gfx, text, 5, y, 0);
        y += fh + 4;
    }
    return y;
}

// ---------------------------------------------------------------------------
// Render: TextField
// ---------------------------------------------------------------------------

function render_textfield(jvm, gfx, item, y, focused) {
    let label = '';
    let text  = '';
    let constraints = 0;
    const label_slot       = find_object_field_slot(item, 'label');
    const text_slot        = find_object_field_slot(item, 'text');
    const maxsize_slot     = find_object_field_slot(item, 'maxSize');
    const constraints_slot = find_object_field_slot(item, 'constraints');
    if (label_slot >= 0 && text_slot >= 0 && maxsize_slot >= 0 && constraints_slot >= 0 &&
        OBJECT_HAS_FIELDS(item, constraints_slot + 1)) {
        const ls = item.fields[label_slot].ref;
        const ts = item.fields[text_slot].ref;
        if (ls) label = get_string_from_object(jvm, ls);
        if (ts) text  = get_string_from_object(jvm, ts);
        constraints = item.fields[constraints_slot].i;
    }
    const font = midp_font_get_default();
    const fh   = font ? midp_font_height(font) : 12;
    const box_height = fh + 8;
    if (label) {
        midp_graphics_set_color(gfx, 0x0000FF, 255);
        midp_graphics_draw_string(gfx, label, 5, y, 0);
        y += fh + 2;
    }
    const box_y = y;
    const box_w = gfx.width - 10;
    midp_graphics_set_color(gfx, 0xFFFFFF, 255);
    midp_graphics_fill_rect(gfx, 5, box_y, box_w, box_height);
    midp_graphics_set_color(gfx, focused ? 0x0000FF : 0x808080, 255);
    midp_graphics_draw_rect(gfx, 5, box_y, box_w, box_height);
    midp_graphics_set_color(gfx, 0x000000, 255);
    const display_text = (constraints & TEXTFIELD_PASSWORD)
        ? (text ? '*'.repeat(text.length) : '')
        : text;
    if (display_text) midp_graphics_draw_string(gfx, display_text, 8, box_y + 3, 0);
    if (focused) {
        const cx = 8 + (font && text ? midp_font_string_width(font, text) : 0);
        midp_graphics_draw_line(gfx, cx, box_y + 2, cx, box_y + box_height - 2);
    }
    return y + box_height + 6;
}

// ---------------------------------------------------------------------------
// Render: ChoiceGroup
// ---------------------------------------------------------------------------

function render_choicegroup(jvm, gfx, item, y, focused) {
    let label       = '';
    let choice_type = CHOICE_EXCLUSIVE;
    let strings     = null;
    let selected    = null;
    const label_slot      = find_object_field_slot(item, 'label');
    const choicetype_slot = find_object_field_slot(item, 'choiceType');
    const strings_slot    = find_object_field_slot(item, 'strings');
    const selected_slot   = find_object_field_slot(item, 'selected');
    if (label_slot >= 0 && choicetype_slot >= 0 && strings_slot >= 0 && selected_slot >= 0 &&
        OBJECT_HAS_FIELDS(item, selected_slot + 1)) {
        const ls = item.fields[label_slot].ref;
        if (ls) label = get_string_from_object(jvm, ls);
        choice_type = item.fields[choicetype_slot].i;
        strings     = item.fields[strings_slot].ref;
        selected    = item.fields[selected_slot].ref;
    }
    const font = midp_font_get_default();
    const fh   = font ? midp_font_height(font) : 12;
    if (label) {
        midp_graphics_set_color(gfx, 0x0000FF, 255);
        midp_graphics_draw_string(gfx, label, 5, y, 0);
        y += fh + 4;
    }
    if (strings && strings.element_type === DESC_OBJECT) {
        const sd    = array_data(strings);
        const sel_d = selected ? array_data(selected) : null;
        for (let i = 0; i < strings.length; i++) {
            const cs = sd ? sd[i] : null;
            const ct = cs ? get_string_from_object(jvm, cs) : '';
            const is_sel = sel_d ? (sel_d[i] !== 0) : false;
            const cx = 10;
            const cy = y + Math.floor(fh / 2);
            midp_graphics_set_color(gfx, 0x000000, 255);
            if (choice_type === CHOICE_EXCLUSIVE || choice_type === CHOICE_POPUP) {
                midp_graphics_draw_arc(gfx, cx - 5, cy - 5, 10, 10, 0, 360);
                if (is_sel) midp_graphics_fill_arc(gfx, cx - 3, cy - 3, 6, 6, 0, 360);
            } else {
                midp_graphics_draw_rect(gfx, cx - 5, cy - 5, 10, 10);
                if (is_sel) {
                    midp_graphics_draw_line(gfx, cx - 3, cy - 3, cx + 3, cy + 3);
                    midp_graphics_draw_line(gfx, cx - 3, cy + 3, cx + 3, cy - 3);
                }
            }
            midp_graphics_draw_string(gfx, ct, cx + 10, y, 0);
            y += fh + 4;
        }
    }
    return y + 4;
}

// ---------------------------------------------------------------------------
// Render: Gauge
// ---------------------------------------------------------------------------

function render_gauge(jvm, gfx, item, y, focused) {
    let label       = '';
    let max_value   = 100;
    let cur_value   = 0;
    let interactive = false;
    const label_slot       = find_object_field_slot(item, 'label');
    const interactive_slot = find_object_field_slot(item, 'interactive');
    const maxvalue_slot    = find_object_field_slot(item, 'maxValue');
    const value_slot       = find_object_field_slot(item, 'value');
    if (label_slot >= 0 && interactive_slot >= 0 && maxvalue_slot >= 0 && value_slot >= 0 &&
        OBJECT_HAS_FIELDS(item, value_slot + 1)) {
        const ls = item.fields[label_slot].ref;
        if (ls) label = get_string_from_object(jvm, ls);
        interactive = item.fields[interactive_slot].i !== 0;
        max_value   = item.fields[maxvalue_slot].i;
        cur_value   = item.fields[value_slot].i;
    }
    const font = midp_font_get_default();
    const fh   = font ? midp_font_height(font) : 12;
    if (label) {
        midp_graphics_set_color(gfx, 0x0000FF, 255);
        midp_graphics_draw_string(gfx, label, 5, y, 0);
        y += fh + 4;
    }
    const bar_y = y;
    const bar_w = gfx.width - 20;
    const bar_h = 20;
    midp_graphics_set_color(gfx, 0xC0C0C0, 255);
    midp_graphics_fill_rect(gfx, 10, bar_y, bar_w, bar_h);
    if (max_value > 0) {
        const fill_w = Math.floor((cur_value * bar_w) / max_value);
        midp_graphics_set_color(gfx, interactive ? 0x0000FF : 0x008000, 255);
        midp_graphics_fill_rect(gfx, 10, bar_y, fill_w, bar_h);
    }
    midp_graphics_set_color(gfx, focused ? 0x0000FF : 0x000000, 255);
    midp_graphics_draw_rect(gfx, 10, bar_y, bar_w, bar_h);
    midp_graphics_set_color(gfx, 0x000000, 255);
    midp_graphics_draw_string(gfx, `${cur_value}/${max_value}`, 100, bar_y + 4, 0x10);
    return y + bar_h + 8;
}

// ---------------------------------------------------------------------------
// Render: Spacer
// ---------------------------------------------------------------------------

function render_spacer(jvm, gfx, item, y) {
    let height = 0;
    const height_slot = find_object_field_slot(item, 'height');
    if (height_slot >= 0 && OBJECT_HAS_FIELDS(item, height_slot + 1)) {
        height = item.fields[height_slot].i;
    }
    if (height <= 0) height = 10;
    return y + height;
}

// ---------------------------------------------------------------------------
// Render: Alert (internal)
// ---------------------------------------------------------------------------

function render_alert_internal(jvm, gfx, alert) {
    let title = 'Alert';
    let text  = '';
    const title_slot = find_object_field_slot(alert, 'title');
    const text_slot  = find_object_field_slot(alert, 'text');
    const type_slot  = find_object_field_slot(alert, 'alertType');
    const max_slot   = Math.max(title_slot, text_slot, type_slot);
    if (title_slot >= 0 && text_slot >= 0 && type_slot >= 0 && OBJECT_HAS_FIELDS(alert, max_slot + 1)) {
        const ts = alert.fields[title_slot].ref;
        const xs = alert.fields[text_slot].ref;
        if (ts) title = get_string_from_object(jvm, ts);
        if (xs) text  = get_string_from_object(jvm, xs);
    }
    const sw = gfx.width;
    const sh = gfx.height;
    const bw = sw - 20;
    const bh = 100;
    const bx = 10;
    const by = Math.floor((sh - bh) / 2);
    midp_graphics_set_color(gfx, 0xFFFFCC, 255);
    midp_graphics_fill_rect(gfx, bx, by, bw, bh);
    midp_graphics_set_color(gfx, 0x000000, 255);
    midp_graphics_draw_rect(gfx, bx, by, bw, bh);
    midp_graphics_set_color(gfx, 0x0000FF, 255);
    midp_graphics_draw_string(gfx, title, Math.floor(sw / 2), by + 5, 0x10);
    midp_graphics_set_color(gfx, 0x000000, 255);
    midp_graphics_draw_string(gfx, text, Math.floor(sw / 2), by + 30, 0x10);
    const btn_w = 60;
    const btn_h = 25;
    const btn_x = Math.floor((sw - btn_w) / 2);
    const btn_y = by + bh - 35;
    midp_graphics_set_color(gfx, 0xC0C0C0, 255);
    midp_graphics_fill_rect(gfx, btn_x, btn_y, btn_w, btn_h);
    midp_graphics_set_color(gfx, 0x000000, 255);
    midp_graphics_draw_rect(gfx, btn_x, btn_y, btn_w, btn_h);
    midp_graphics_draw_string(gfx, 'OK', Math.floor(sw / 2), btn_y + 5, 0x10);
}

// ---------------------------------------------------------------------------
// Render: List (internal)
// ---------------------------------------------------------------------------

function render_list_internal(jvm, gfx, list, focused_index) {
    let title     = '';
    let list_type = CHOICE_IMPLICIT;
    let strings   = null;
    let selected  = null;
    const list_class   = list.header && list.header.clazz;
    const title_idx    = list_class ? get_field_offset(list_class, 'title')    : -1;
    const listtype_idx = list_class ? get_field_offset(list_class, 'listType') : -1;
    const strings_idx  = list_class ? get_field_offset(list_class, 'strings')  : -1;
    const selected_idx = list_class ? get_field_offset(list_class, 'selected') : -1;
    if (title_idx >= 0 && listtype_idx >= 0 && strings_idx >= 0 && selected_idx >= 0) {
        const ts = list.fields[title_idx].ref;
        if (ts) title = get_string_from_object(jvm, ts);
        list_type = list.fields[listtype_idx].i;
        strings   = list.fields[strings_idx].ref;
        selected  = list.fields[selected_idx].ref;
    }
    const font = midp_font_get_default();
    const fh   = font ? midp_font_height(font) : 12;
    let y = 30;
    midp_graphics_set_color(gfx, 0x0000FF, 255);
    midp_graphics_draw_string(gfx, title, Math.floor(gfx.width / 2), 5, 0x10);
    midp_graphics_set_color(gfx, 0x808080, 255);
    midp_graphics_draw_line(gfx, 0, 25, gfx.width, 25);
    if (strings && strings.element_type === DESC_OBJECT) {
        const sd    = array_data(strings);
        const sel_d = selected ? array_data(selected) : null;
        for (let i = 0; i < strings.length; i++) {
            const is_obj = sd ? sd[i] : null;
            const it = is_obj ? get_string_from_object(jvm, is_obj) : '';
            if (i === focused_index) {
                midp_graphics_set_color(gfx, 0x000080, 255);
                midp_graphics_fill_rect(gfx, 0, y - 2, gfx.width, fh + 4);
                midp_graphics_set_color(gfx, 0xFFFFFF, 255);
            } else {
                midp_graphics_set_color(gfx, 0x000000, 255);
            }
            let text_x = 5;
            if (list_type === CHOICE_EXCLUSIVE || list_type === CHOICE_MULTIPLE) {
                const is_sel = sel_d ? (sel_d[i] !== 0) : false;
                const cx = 15;
                const cy = y + Math.floor(fh / 2);
                if (list_type === CHOICE_EXCLUSIVE) {
                    midp_graphics_draw_arc(gfx, cx - 5, cy - 5, 10, 10, 0, 360);
                    if (is_sel) midp_graphics_fill_arc(gfx, cx - 3, cy - 3, 6, 6, 0, 360);
                } else {
                    midp_graphics_draw_rect(gfx, cx - 5, cy - 5, 10, 10);
                    if (is_sel) {
                        midp_graphics_draw_line(gfx, cx - 3, cy - 3, cx + 3, cy + 3);
                        midp_graphics_draw_line(gfx, cx - 3, cy + 3, cx + 3, cy - 3);
                    }
                }
                text_x = 30;
            }
            midp_graphics_draw_string(gfx, it, text_x, y, 0);
            y += fh + 6;
        }
    }
}

// ---------------------------------------------------------------------------
// Render: TextBox (internal)
// ---------------------------------------------------------------------------

function render_textbox_internal(jvm, gfx, textbox, input_text, cursor_pos) {
    let title       = '';
    let constraints = 0;
    const title_slot       = find_object_field_slot(textbox, 'title');
    const constraints_slot = find_object_field_slot(textbox, 'constraints');
    if (title_slot >= 0 && constraints_slot >= 0 && OBJECT_HAS_FIELDS(textbox, constraints_slot + 1)) {
        const ts = textbox.fields[title_slot].ref;
        if (ts) title = get_string_from_object(jvm, ts);
        constraints = textbox.fields[constraints_slot].i;
    }
    const font = midp_font_get_default();
    const fh   = font ? midp_font_height(font) : 12;
    midp_graphics_set_color(gfx, 0x000080, 255);
    midp_graphics_fill_rect(gfx, 0, 0, gfx.width, 25);
    midp_graphics_set_color(gfx, 0xFFFFFF, 255);
    midp_graphics_draw_string(gfx, title, Math.floor(gfx.width / 2), 5, 0x10);
    const text_y = 30;
    const text_h = gfx.height - 80;
    midp_graphics_set_color(gfx, 0xFFFFFF, 255);
    midp_graphics_fill_rect(gfx, 5, text_y, gfx.width - 10, text_h);
    midp_graphics_set_color(gfx, 0x000000, 255);
    midp_graphics_draw_rect(gfx, 5, text_y, gfx.width - 10, text_h);
    if (input_text) {
        const dt = (constraints & TEXTFIELD_PASSWORD) ? '*'.repeat(input_text.length) : input_text;
        midp_graphics_draw_string(gfx, dt, 10, text_y + 5, 0);
    }
    if (cursor_pos >= 0) {
        const prefix = input_text ? input_text.slice(0, cursor_pos) : '';
        const cx = 10 + (font ? midp_font_string_width(font, prefix) : 0);
        midp_graphics_draw_line(gfx, cx, text_y + 5, cx, text_y + fh + 5);
    }
    const btn_h = 25;
    midp_graphics_set_color(gfx, 0xC0C0C0, 255);
    midp_graphics_fill_rect(gfx, 0, gfx.height - btn_h, gfx.width, btn_h);
    midp_graphics_set_color(gfx, 0x000000, 255);
    midp_graphics_draw_line(gfx, 0, gfx.height - btn_h, gfx.width, gfx.height - btn_h);
    midp_graphics_draw_string(gfx, 'Cancel', 10, gfx.height - btn_h + 5, 0);
    midp_graphics_draw_string(gfx, 'OK', gfx.width - 30, gfx.height - btn_h + 5, 0);
}

// ---------------------------------------------------------------------------
// Native: Form methods
// ---------------------------------------------------------------------------

function native_form_init(jvm, thread, args, arg_count) {
    const form  = args[0].ref;
    const title = args[1].ref;
    if (!form) return { i: 0 };
    const title_slot = find_object_field_slot(form, 'title');
    const items_slot = find_object_field_slot(form, 'items');
    if (title_slot >= 0 && items_slot >= 0 && OBJECT_HAS_FIELDS(form, items_slot + 1)) {
        form.fields[title_slot].ref = title;
        form.fields[items_slot].ref = jvm_new_array(jvm, DESC_OBJECT, 0, null);
    }
    return { i: 0 };
}

function native_form_append(jvm, thread, args, arg_count) {
    const form = args[0].ref;
    const item = args[1].ref;
    if (!form || !item) return { i: -1 };
    const items_slot = find_object_field_slot(form, 'items');
    if (items_slot < 0 || !OBJECT_HAS_FIELDS(form, items_slot + 1)) return { i: -1 };
    let items = form.fields[items_slot].ref;
    const old_count = items ? items.length : 0;
    const new_items = jvm_new_array(jvm, DESC_OBJECT, old_count + 1, null);
    if (!new_items) return { i: -1 };
    const new_data = array_data(new_items);
    if (items) {
        const old_data = array_data(items);
        for (let i = 0; i < old_count; i++) new_data[i] = old_data[i];
    }
    new_data[old_count] = item;
    form.fields[items_slot].ref = new_items;
    return { i: old_count };
}

function native_form_delete(jvm, thread, args, arg_count) {
    const form  = args[0].ref;
    const index = args[1].i;
    if (!form) return { i: 0 };
    const items_slot = find_object_field_slot(form, 'items');
    if (items_slot < 0 || !OBJECT_HAS_FIELDS(form, items_slot + 1)) return { i: 0 };
    const items = form.fields[items_slot].ref;
    if (!items || items.length === 0 || index < 0 || index >= items.length) return { i: 0 };
    const new_len = items.length - 1;
    const new_items = jvm_new_array(jvm, DESC_OBJECT, new_len, null);
    if (!new_items) return { i: 0 };
    const old_data = array_data(items);
    const new_data = array_data(new_items);
    for (let i = 0; i < index; i++) new_data[i] = old_data[i];
    for (let i = index + 1; i < items.length; i++) new_data[i - 1] = old_data[i];
    form.fields[items_slot].ref = new_items;
    return { i: 0 };
}

function native_form_deleteAll(jvm, thread, args, arg_count) {
    const form = args[0].ref;
    if (form) {
        const items_slot = find_object_field_slot(form, 'items');
        if (items_slot >= 0 && OBJECT_HAS_FIELDS(form, items_slot + 1)) {
            form.fields[items_slot].ref = jvm_new_array(jvm, DESC_OBJECT, 0, null);
        }
    }
    return { i: 0 };
}

function native_form_size(jvm, thread, args, arg_count) {
    const form = args[0].ref;
    if (form) {
        const items_slot = find_object_field_slot(form, 'items');
        if (items_slot >= 0 && OBJECT_HAS_FIELDS(form, items_slot + 1)) {
            const items = form.fields[items_slot].ref;
            if (items) return { i: items.length };
        }
    }
    return { i: 0 };
}

function native_form_get(jvm, thread, args, arg_count) {
    const form  = args[0].ref;
    const index = args[1].i;
    if (!form) return { ref: null };
    const items_slot = find_object_field_slot(form, 'items');
    if (items_slot < 0 || !OBJECT_HAS_FIELDS(form, items_slot + 1)) return { ref: null };
    const items = form.fields[items_slot].ref;
    if (!items || index < 0 || index >= items.length) return { ref: null };
    const data = array_data(items);
    return { ref: data ? data[index] : null };
}

function native_form_getTitle(jvm, thread, args, arg_count) {
    const form = args[0].ref;
    if (form) {
        const slot = find_object_field_slot(form, 'title');
        if (slot >= 0 && OBJECT_HAS_FIELDS(form, slot + 1)) return { ref: form.fields[slot].ref };
    }
    return { ref: null };
}

function native_form_setTitle(jvm, thread, args, arg_count) {
    const form  = args[0].ref;
    const title = args[1].ref;
    if (form) {
        const slot = find_object_field_slot(form, 'title');
        if (slot >= 0 && OBJECT_HAS_FIELDS(form, slot + 1)) form.fields[slot].ref = title;
    }
    return { i: 0 };
}

function native_form_setItemStateListener(jvm, thread, args, arg_count) {
    g_item_state_listener = args[1].ref;
    return { i: 0 };
}

function native_form_setTicker(jvm, thread, args, arg_count) {
    const form   = args[0].ref;
    const ticker = args[1].ref;
    if (form) {
        const slot = find_object_field_slot(form, 'ticker');
        if (slot >= 0 && OBJECT_HAS_FIELDS(form, slot + 1)) form.fields[slot].ref = ticker;
    }
    return { i: 0 };
}

function native_form_getTicker(jvm, thread, args, arg_count) {
    const form = args[0].ref;
    if (form) {
        const slot = find_object_field_slot(form, 'ticker');
        if (slot >= 0 && OBJECT_HAS_FIELDS(form, slot + 1)) return { ref: form.fields[slot].ref };
    }
    return { ref: null };
}

// ---------------------------------------------------------------------------
// Native: Item methods
// ---------------------------------------------------------------------------

function native_item_setLabel(jvm, thread, args, arg_count) {
    const item  = args[0].ref;
    const label = args[1].ref;
    if (item && OBJECT_HAS_FIELDS(item, 1)) item.fields[0].ref = label;
    return { i: 0 };
}

function native_item_getLabel(jvm, thread, args, arg_count) {
    const item = args[0].ref;
    if (item && OBJECT_HAS_FIELDS(item, 1)) return { ref: item.fields[0].ref };
    return { ref: null };
}

// ---------------------------------------------------------------------------
// Native: StringItem
// ---------------------------------------------------------------------------

function native_stringitem_init(jvm, thread, args, arg_count) {
    const item  = args[0].ref;
    const label = args[1].ref;
    const text  = args[2].ref;
    if (!item) return { i: 0 };
    const ls = find_object_field_slot(item, 'label');
    const ts = find_object_field_slot(item, 'text');
    if (ls >= 0 && ts >= 0 && OBJECT_HAS_FIELDS(item, ts + 1)) {
        item.fields[ls].ref = label;
        item.fields[ts].ref = text;
    }
    return { i: 0 };
}

function native_stringitem_setText(jvm, thread, args, arg_count) {
    const item = args[0].ref;
    const text = args[1].ref;
    if (item) {
        const slot = find_object_field_slot(item, 'text');
        if (slot >= 0 && OBJECT_HAS_FIELDS(item, slot + 1)) item.fields[slot].ref = text;
    }
    return { i: 0 };
}

function native_stringitem_getText(jvm, thread, args, arg_count) {
    const item = args[0].ref;
    if (item) {
        const slot = find_object_field_slot(item, 'text');
        if (slot >= 0 && OBJECT_HAS_FIELDS(item, slot + 1)) return { ref: item.fields[slot].ref };
    }
    return { ref: null };
}

// ---------------------------------------------------------------------------
// Native: TextField
// ---------------------------------------------------------------------------

function native_textfield_init(jvm, thread, args, arg_count) {
    const item        = args[0].ref;
    const label       = args[1].ref;
    const text        = args[2].ref;
    const max_size    = args[3].i;
    const constraints = args[4].i;
    if (!item) return { i: 0 };
    const ls = find_object_field_slot(item, 'label');
    const ts = find_object_field_slot(item, 'text');
    const ms = find_object_field_slot(item, 'maxSize');
    const cs = find_object_field_slot(item, 'constraints');
    if (ls >= 0 && ts >= 0 && ms >= 0 && cs >= 0 && OBJECT_HAS_FIELDS(item, cs + 1)) {
        item.fields[ls].ref = label;
        item.fields[ts].ref = text;
        item.fields[ms].i   = max_size;
        item.fields[cs].i   = constraints;
    }
    return { i: 0 };
}

function native_textfield_setString(jvm, thread, args, arg_count) {
    const item = args[0].ref;
    const text = args[1].ref;
    if (item) {
        const slot = find_object_field_slot(item, 'text');
        if (slot >= 0 && OBJECT_HAS_FIELDS(item, slot + 1)) item.fields[slot].ref = text;
    }
    return { i: 0 };
}

function native_textfield_getString(jvm, thread, args, arg_count) {
    const item = args[0].ref;
    if (item) {
        const slot = find_object_field_slot(item, 'text');
        if (slot >= 0 && OBJECT_HAS_FIELDS(item, slot + 1)) return { ref: item.fields[slot].ref };
    }
    return { ref: null };
}

function native_textfield_setChars(jvm, thread, args, arg_count) {
    const item   = args[0].ref;
    const chars  = args[1].ref;
    const offset = args[2].i;
    const length = args[3].i;
    if (item && chars && chars.element_type === T_CHAR) {
        const chars_data = array_data(chars);
        const sub = chars_data ? chars_data.slice(offset, offset + length) : [];
        const str = jvm_new_string_utf16(jvm, sub, length);
        const slot = find_object_field_slot(item, 'text');
        if (slot >= 0 && OBJECT_HAS_FIELDS(item, slot + 1)) item.fields[slot].ref = str;
    }
    return { i: 0 };
}

function native_textfield_size(jvm, thread, args, arg_count) {
    const item = args[0].ref;
    if (item) {
        const slot = find_object_field_slot(item, 'text');
        if (slot >= 0 && OBJECT_HAS_FIELDS(item, slot + 1)) {
            const text = item.fields[slot].ref;
            if (text) return { i: text.length || 0 };
        }
    }
    return { i: 0 };
}

function native_textfield_getMaxSize(jvm, thread, args, arg_count) {
    const item = args[0].ref;
    if (item) {
        const slot = find_object_field_slot(item, 'maxSize');
        if (slot >= 0 && OBJECT_HAS_FIELDS(item, slot + 1)) return { i: item.fields[slot].i };
    }
    return { i: 32 };
}

function native_textfield_setMaxSize(jvm, thread, args, arg_count) {
    const item     = args[0].ref;
    const max_size = args[1].i;
    if (item && max_size > 0) {
        const slot = find_object_field_slot(item, 'maxSize');
        if (slot >= 0 && OBJECT_HAS_FIELDS(item, slot + 1)) item.fields[slot].i = max_size;
    }
    return { i: 0 };
}

function native_textfield_getConstraints(jvm, thread, args, arg_count) {
    const item = args[0].ref;
    if (item) {
        const slot = find_object_field_slot(item, 'constraints');
        if (slot >= 0 && OBJECT_HAS_FIELDS(item, slot + 1)) return { i: item.fields[slot].i };
    }
    return { i: 0 };
}

function native_textfield_setConstraints(jvm, thread, args, arg_count) {
    const item        = args[0].ref;
    const constraints = args[1].i;
    if (item) {
        const slot = find_object_field_slot(item, 'constraints');
        if (slot >= 0 && OBJECT_HAS_FIELDS(item, slot + 1)) item.fields[slot].i = constraints;
    }
    return { i: 0 };
}

function native_textfield_getChars(jvm, thread, args, arg_count) {
    const item = args[0].ref;
    const dest = args[1].ref;
    if (!item || !dest || dest.element_type !== T_CHAR) return { i: 0 };
    const slot = find_object_field_slot(item, 'text');
    if (slot >= 0 && OBJECT_HAS_FIELDS(item, slot + 1)) {
        const text = item.fields[slot].ref;
        if (text) {
            const src_data  = string_chars(text);
            const dest_data = array_data(dest);
            const copy_len  = Math.min(text.length || 0, dest.length);
            if (src_data && dest_data) {
                for (let i = 0; i < copy_len; i++) dest_data[i] = src_data[i];
            }
        }
    }
    return { i: 0 };
}

function native_textfield_delete(jvm, thread, args, arg_count) {
    const item   = args[0].ref;
    const offset = args[1].i;
    const length = args[2].i;
    if (!item) return { i: 0 };
    const slot = find_object_field_slot(item, 'text');
    if (slot < 0 || !OBJECT_HAS_FIELDS(item, slot + 1)) return { i: 0 };
    const text = item.fields[slot].ref;
    if (!text || offset < 0 || length <= 0 || offset >= (text.length || 0)) return { i: 0 };
    const old_data = string_chars(text);
    const old_len  = text.length || 0;
    const del_len  = Math.min(length, old_len - offset);
    const new_len  = old_len - del_len;
    const new_chars = [];
    for (let i = 0; i < offset; i++) new_chars.push(old_data ? old_data[i] : 0);
    for (let i = offset + del_len; i < old_len; i++) new_chars.push(old_data ? old_data[i] : 0);
    item.fields[slot].ref = jvm_new_string_utf16(jvm, new_chars, new_len);
    return { i: 0 };
}

function native_textfield_insert(jvm, thread, args, arg_count) {
    const item       = args[0].ref;
    const insert_str = args[1].ref;
    const position   = args[2].i;
    if (!item || !insert_str) return { i: 0 };
    const text_slot    = find_object_field_slot(item, 'text');
    const maxsize_slot = find_object_field_slot(item, 'maxSize');
    if (text_slot < 0 || !OBJECT_HAS_FIELDS(item, text_slot + 1)) return { i: 0 };
    const text     = item.fields[text_slot].ref;
    const old_len  = text ? (text.length || 0) : 0;
    const max_size = (maxsize_slot >= 0 && OBJECT_HAS_FIELDS(item, maxsize_slot + 1))
        ? item.fields[maxsize_slot].i : 32;
    if (old_len + (insert_str.length || 0) > max_size) return { i: 0 };
    let pos = Math.max(0, Math.min(position, old_len));
    const old_data    = text ? string_chars(text) : null;
    const insert_data = string_chars(insert_str);
    const ins_len     = insert_str.length || 0;
    const new_chars   = [];
    for (let i = 0; i < pos; i++) new_chars.push(old_data ? old_data[i] : 0);
    for (let i = 0; i < ins_len; i++) new_chars.push(insert_data ? insert_data[i] : 0);
    for (let i = pos; i < old_len; i++) new_chars.push(old_data ? old_data[i] : 0);
    item.fields[text_slot].ref = jvm_new_string_utf16(jvm, new_chars, new_chars.length);
    return { i: 0 };
}

function native_textfield_getCaretPosition(jvm, thread, args, arg_count) {
    const item = args[0].ref;
    if (item) {
        const slot = find_object_field_slot(item, 'text');
        if (slot >= 0 && OBJECT_HAS_FIELDS(item, slot + 1)) {
            const text = item.fields[slot].ref;
            if (text) return { i: text.length || 0 };
        }
    }
    return { i: 0 };
}

// ---------------------------------------------------------------------------
// Native: ChoiceGroup
// ---------------------------------------------------------------------------

function native_choicegroup_init(jvm, thread, args, arg_count) {
    const group       = args[0].ref;
    const label       = args[1].ref;
    const choice_type = args[2].i;
    if (!group) return { i: 0 };
    const ls  = find_object_field_slot(group, 'label');
    const cts = find_object_field_slot(group, 'choiceType');
    const ss  = find_object_field_slot(group, 'strings');
    const sel = find_object_field_slot(group, 'selected');
    if (ls >= 0 && cts >= 0 && ss >= 0 && sel >= 0 && OBJECT_HAS_FIELDS(group, sel + 1)) {
        group.fields[ls].ref  = label;
        group.fields[cts].i   = choice_type;
        group.fields[ss].ref  = jvm_new_array(jvm, DESC_OBJECT, 0, null);
        group.fields[sel].ref = jvm_new_array(jvm, T_BOOLEAN, 0, null);
    }
    return { i: 0 };
}

function native_choicegroup_init_array(jvm, thread, args, arg_count) {
    const group       = args[0].ref;
    const label       = args[1].ref;
    const choice_type = args[2].i;
    const strings     = args[3].ref;
    if (!group) return { i: 0 };
    const ls  = find_object_field_slot(group, 'label');
    const cts = find_object_field_slot(group, 'choiceType');
    const ss  = find_object_field_slot(group, 'strings');
    const sel = find_object_field_slot(group, 'selected');
    if (ls >= 0 && cts >= 0 && ss >= 0 && sel >= 0 && OBJECT_HAS_FIELDS(group, sel + 1)) {
        group.fields[ls].ref  = label;
        group.fields[cts].i   = choice_type;
        group.fields[ss].ref  = strings;
        if (strings) {
            group.fields[sel].ref = jvm_new_array(jvm, T_BOOLEAN, strings.length, null);
        }
    }
    return { i: 0 };
}

function native_choicegroup_append(jvm, thread, args, arg_count) {
    const group = args[0].ref;
    const text  = args[1].ref;
    if (!group) return { i: -1 };
    const ss  = find_object_field_slot(group, 'strings');
    const sel = find_object_field_slot(group, 'selected');
    if (ss < 0 || sel < 0 || !OBJECT_HAS_FIELDS(group, sel + 1)) return { i: -1 };
    const strings  = group.fields[ss].ref;
    const selected = group.fields[sel].ref;
    const old_len  = strings ? strings.length : 0;
    const new_len  = old_len + 1;
    const new_strings  = jvm_new_array(jvm, DESC_OBJECT, new_len, null);
    const new_selected = jvm_new_array(jvm, T_BOOLEAN, new_len, null);
    if (!new_strings || !new_selected) return { i: -1 };
    const ns = array_data(new_strings);
    const nsel = array_data(new_selected);
    if (strings) {
        const os = array_data(strings);
        for (let i = 0; i < old_len; i++) ns[i] = os[i];
    }
    if (selected) {
        const os = array_data(selected);
        for (let i = 0; i < old_len; i++) nsel[i] = os[i];
    }
    ns[old_len]   = text;
    nsel[old_len] = 0;
    group.fields[ss].ref  = new_strings;
    group.fields[sel].ref = new_selected;
    return { i: old_len };
}

function native_choicegroup_setSelectedIndex(jvm, thread, args, arg_count) {
    const group    = args[0].ref;
    const index    = args[1].i;
    const selected = args[2].i;
    if (!group) return { i: 0 };
    const cts = find_object_field_slot(group, 'choiceType');
    const sel = find_object_field_slot(group, 'selected');
    if (cts < 0 || sel < 0 || !OBJECT_HAS_FIELDS(group, sel + 1)) return { i: 0 };
    const sel_arr = group.fields[sel].ref;
    if (!sel_arr || sel_arr.element_type !== T_BOOLEAN) return { i: 0 };
    if (index < 0 || index >= sel_arr.length) return { i: 0 };
    const choice_type = group.fields[cts].i;
    const sel_data = array_data(sel_arr);
    if (choice_type === 1 && sel_data) {
        for (let i = 0; i < sel_arr.length; i++) sel_data[i] = 0;
    }
    if (sel_data) sel_data[index] = selected;
    return { i: 0 };
}

function native_choicegroup_getSelectedIndex(jvm, thread, args, arg_count) {
    const group = args[0].ref;
    if (group) {
        const sel = find_object_field_slot(group, 'selected');
        if (sel >= 0 && OBJECT_HAS_FIELDS(group, sel + 1)) {
            const sel_arr = group.fields[sel].ref;
            if (sel_arr && sel_arr.element_type === T_BOOLEAN) {
                const data = array_data(sel_arr);
                if (data) {
                    for (let i = 0; i < sel_arr.length; i++) {
                        if (data[i]) return { i };
                    }
                }
            }
        }
    }
    return { i: -1 };
}

// ---------------------------------------------------------------------------
// Native: Gauge
// ---------------------------------------------------------------------------

function native_gauge_init(jvm, thread, args, arg_count) {
    const gauge       = args[0].ref;
    const label       = args[1].ref;
    const interactive = args[2].i;
    const max_value   = args[3].i;
    const init_value  = args[4].i;
    if (!gauge) return { i: 0 };
    const ls  = find_object_field_slot(gauge, 'label');
    const is  = find_object_field_slot(gauge, 'interactive');
    const mvs = find_object_field_slot(gauge, 'maxValue');
    const vs  = find_object_field_slot(gauge, 'value');
    if (ls >= 0 && is >= 0 && mvs >= 0 && vs >= 0 && OBJECT_HAS_FIELDS(gauge, vs + 1)) {
        gauge.fields[ls].ref = label;
        gauge.fields[is].i   = interactive;
        gauge.fields[mvs].i  = max_value;
        gauge.fields[vs].i   = init_value;
    }
    return { i: 0 };
}

function native_gauge_setValue(jvm, thread, args, arg_count) {
    const gauge = args[0].ref;
    const value = args[1].i;
    if (gauge) {
        const slot = find_object_field_slot(gauge, 'value');
        if (slot >= 0 && OBJECT_HAS_FIELDS(gauge, slot + 1)) {
            const old_value = gauge.fields[slot].i;
            gauge.fields[slot].i = value;
            if (old_value !== value && g_item_state_listener && g_item_state_listener.header && g_item_state_listener.header.clazz) {
                const ism = jvm_resolve_method(jvm, g_item_state_listener.header.clazz,
                    'itemStateChanged', '(Ljavax/microedition/lcdui/Item;)V');
                if (ism) {
                    _execute_method(jvm, jvm_current_thread(jvm), ism,
                        [{ ref: g_item_state_listener }, { ref: gauge }], {});
                }
            }
        }
    }
    return { i: 0 };
}

function native_gauge_getValue(jvm, thread, args, arg_count) {
    const gauge = args[0].ref;
    if (gauge) {
        const slot = find_object_field_slot(gauge, 'value');
        if (slot >= 0 && OBJECT_HAS_FIELDS(gauge, slot + 1)) return { i: gauge.fields[slot].i };
    }
    return { i: 0 };
}

// ---------------------------------------------------------------------------
// Native: List
// ---------------------------------------------------------------------------

function native_list_init(jvm, thread, args, arg_count) {
    const list      = args[0].ref;
    const title     = args[1].ref;
    const list_type = args[2].i;
    ensure_select_command(jvm);
    if (!list) return { i: 0 };
    const list_class   = list.header && list.header.clazz;
    const title_idx    = list_class ? get_field_offset(list_class, 'title')    : -1;
    const listtype_idx = list_class ? get_field_offset(list_class, 'listType') : -1;
    const strings_idx  = list_class ? get_field_offset(list_class, 'strings')  : -1;
    const selected_idx = list_class ? get_field_offset(list_class, 'selected') : -1;
    if (title_idx >= 0 && listtype_idx >= 0 && strings_idx >= 0 && selected_idx >= 0) {
        list.fields[title_idx].ref    = title;
        list.fields[listtype_idx].i   = list_type;
        list.fields[strings_idx].ref  = jvm_new_array(jvm, DESC_OBJECT, 0, null);
        list.fields[selected_idx].ref = jvm_new_array(jvm, T_BOOLEAN, 0, null);
    }
    return { i: 0 };
}

function native_list_init_with_elements(jvm, thread, args, arg_count) {
    const list      = args[0].ref;
    const title     = args[1].ref;
    const list_type = args[2].i;
    const strings   = args[3].ref;
    ensure_select_command(jvm);
    if (!list) return { i: 0 };
    const list_class   = list.header && list.header.clazz;
    const title_idx    = list_class ? get_field_offset(list_class, 'title')    : -1;
    const listtype_idx = list_class ? get_field_offset(list_class, 'listType') : -1;
    const strings_idx  = list_class ? get_field_offset(list_class, 'strings')  : -1;
    const selected_idx = list_class ? get_field_offset(list_class, 'selected') : -1;
    if (title_idx >= 0 && listtype_idx >= 0 && strings_idx >= 0 && selected_idx >= 0) {
        list.fields[title_idx].ref   = title;
        list.fields[listtype_idx].i  = list_type;
        list.fields[strings_idx].ref = strings;
        if (strings) {
            const sel = jvm_new_array(jvm, T_BOOLEAN, strings.length, null);
            list.fields[selected_idx].ref = sel;
            if (sel && strings.length > 0 && (list_type === 1 || list_type === 3 || list_type === 4)) {
                const data = array_data(sel);
                if (data) data[0] = 1;
            }
        } else {
            list.fields[selected_idx].ref = jvm_new_array(jvm, T_BOOLEAN, 0, null);
        }
    }
    return { i: 0 };
}

function native_list_append(jvm, thread, args, arg_count) {
    const list = args[0].ref;
    const text = args[1].ref;
    if (!list) return { i: 0 };
    const list_class   = list.header && list.header.clazz;
    const strings_idx  = list_class ? get_field_offset(list_class, 'strings')  : -1;
    const selected_idx = list_class ? get_field_offset(list_class, 'selected') : -1;
    if (strings_idx >= 0 && selected_idx >= 0) {
        const strings  = list.fields[strings_idx].ref;
        const selected = list.fields[selected_idx].ref;
        const old_len  = strings ? strings.length : 0;
        const new_len  = old_len + 1;
        const ns = jvm_new_array(jvm, DESC_OBJECT, new_len, null);
        const nsel = jvm_new_array(jvm, T_BOOLEAN, new_len, null);
        if (ns) {
            const nd = array_data(ns);
            if (strings && strings.element_type === DESC_OBJECT) {
                const od = array_data(strings);
                for (let i = 0; i < old_len; i++) nd[i] = od[i];
            }
            nd[old_len] = text;
            list.fields[strings_idx].ref = ns;
        }
        if (nsel) {
            const nd = array_data(nsel);
            if (selected && selected.element_type === T_BOOLEAN) {
                const od = array_data(selected);
                for (let i = 0; i < old_len; i++) nd[i] = od[i];
            } else {
                for (let i = 0; i < old_len; i++) nd[i] = 0;
            }
            nd[old_len] = 0;
            list.fields[selected_idx].ref = nsel;
        }
    }
    return { i: 0 };
}

function native_list_getSelectedIndex(jvm, thread, args, arg_count) {
    const list = args[0].ref;
    if (list === g_current_displayable) return { i: g_list_selected_index };
    if (list) {
        const list_class   = list.header && list.header.clazz;
        const selected_idx = list_class ? get_field_offset(list_class, 'selected') : -1;
        if (selected_idx >= 0) {
            const selected = list.fields[selected_idx].ref;
            if (selected && selected.element_type === T_BOOLEAN) {
                const data = array_data(selected);
                if (data) {
                    for (let i = 0; i < selected.length; i++) {
                        if (data[i]) return { i };
                    }
                }
            }
        }
    }
    return { i: g_list_selected_index };
}

function native_list_setSelectedIndex(jvm, thread, args, arg_count) {
    return { i: 0 };
}

function native_list_delete(jvm, thread, args, arg_count) {
    const list  = args[0].ref;
    const index = args[1].i;
    if (!list || index < 0) return { i: 0 };
    const list_class   = list.header && list.header.clazz;
    const strings_idx  = list_class ? get_field_offset(list_class, 'strings')  : -1;
    const selected_idx = list_class ? get_field_offset(list_class, 'selected') : -1;
    if (strings_idx >= 0 && selected_idx >= 0) {
        const strings  = list.fields[strings_idx].ref;
        const selected = list.fields[selected_idx].ref;
        if (strings && strings.length > 0 && index < strings.length) {
            const new_len = strings.length - 1;
            const ns  = jvm_new_array(jvm, DESC_OBJECT, new_len, null);
            const nsel = jvm_new_array(jvm, T_BOOLEAN, new_len, null);
            if (ns && strings.element_type === DESC_OBJECT) {
                const od = array_data(strings);
                const nd = array_data(ns);
                for (let i = 0; i < index; i++) nd[i] = od[i];
                for (let i = index; i < new_len; i++) nd[i] = od[i + 1];
                list.fields[strings_idx].ref = ns;
            }
            if (nsel) {
                const nd = array_data(nsel);
                if (selected && selected.element_type === T_BOOLEAN) {
                    const od = array_data(selected);
                    for (let i = 0; i < index; i++) nd[i] = od[i];
                    for (let i = index; i < new_len; i++) nd[i] = od[i + 1];
                } else {
                    for (let i = 0; i < new_len; i++) nd[i] = 0;
                }
                list.fields[selected_idx].ref = nsel;
            }
        }
    }
    return { i: 0 };
}

function native_list_set(jvm, thread, args, arg_count) {
    const list  = args[0].ref;
    const index = args[1].i;
    const text  = args[2].ref;
    if (!list || index < 0) return { i: 0 };
    const list_class  = list.header && list.header.clazz;
    const strings_idx = list_class ? get_field_offset(list_class, 'strings') : -1;
    if (strings_idx >= 0) {
        const strings = list.fields[strings_idx].ref;
        if (strings && strings.element_type === DESC_OBJECT && index < strings.length) {
            const data = array_data(strings);
            if (data) data[index] = text;
        }
    }
    return { i: 0 };
}

function native_list_size(jvm, thread, args, arg_count) {
    const list = args[0].ref;
    if (list) {
        const list_class  = list.header && list.header.clazz;
        const strings_idx = list_class ? get_field_offset(list_class, 'strings') : -1;
        if (strings_idx >= 0) {
            const strings = list.fields[strings_idx].ref;
            if (strings) return { i: strings.length };
        }
    }
    return { i: 0 };
}

function native_list_getString(jvm, thread, args, arg_count) {
    const list  = args[0].ref;
    const index = args[1].i;
    if (list) {
        const list_class  = list.header && list.header.clazz;
        const strings_idx = list_class ? get_field_offset(list_class, 'strings') : -1;
        if (strings_idx >= 0) {
            const strings = list.fields[strings_idx].ref;
            if (strings && index >= 0 && index < strings.length) {
                const data = array_data(strings);
                return { ref: data ? data[index] : null };
            }
        }
    }
    return { ref: null };
}

// ---------------------------------------------------------------------------
// Native: Alert
// ---------------------------------------------------------------------------

function native_alert_init(jvm, thread, args, arg_count) {
    const alert      = args[0].ref;
    const title      = args[1].ref;
    const text       = args[2].ref;
    const image      = args[3].ref;
    const alert_type = args[4].ref;
    if (!alert) return { i: 0 };
    const ts  = find_object_field_slot(alert, 'title');
    const xs  = find_object_field_slot(alert, 'text');
    const is  = find_object_field_slot(alert, 'image');
    const at  = find_object_field_slot(alert, 'alertType');
    const tos = find_object_field_slot(alert, 'timeout');
    if (ts >= 0 && xs >= 0 && is >= 0 && at >= 0 && tos >= 0 && OBJECT_HAS_FIELDS(alert, tos + 1)) {
        alert.fields[ts].ref  = title;
        alert.fields[xs].ref  = text;
        alert.fields[is].ref  = image;
        alert.fields[at].ref  = alert_type;
        alert.fields[tos].i   = 3000;
    }
    return { i: 0 };
}

function native_alert_setString(jvm, thread, args, arg_count) {
    const alert = args[0].ref;
    const text  = args[1].ref;
    if (alert) {
        const slot = find_object_field_slot(alert, 'text');
        if (slot >= 0 && OBJECT_HAS_FIELDS(alert, slot + 1)) alert.fields[slot].ref = text;
    }
    return { i: 0 };
}

function native_alert_getString(jvm, thread, args, arg_count) {
    const alert = args[0].ref;
    if (alert) {
        const slot = find_object_field_slot(alert, 'text');
        if (slot >= 0 && OBJECT_HAS_FIELDS(alert, slot + 1)) return { ref: alert.fields[slot].ref };
    }
    return { ref: null };
}

function native_alert_setTitle(jvm, thread, args, arg_count) {
    const alert = args[0].ref;
    const title = args[1].ref;
    if (alert) {
        const slot = find_object_field_slot(alert, 'title');
        if (slot >= 0 && OBJECT_HAS_FIELDS(alert, slot + 1)) alert.fields[slot].ref = title;
    }
    return { i: 0 };
}

function native_alert_getTitle(jvm, thread, args, arg_count) {
    const alert = args[0].ref;
    if (alert) {
        const slot = find_object_field_slot(alert, 'title');
        if (slot >= 0 && OBJECT_HAS_FIELDS(alert, slot + 1)) return { ref: alert.fields[slot].ref };
    }
    return { ref: null };
}

function native_alert_setTimeout(jvm, thread, args, arg_count) {
    const alert   = args[0].ref;
    const timeout = args[1].i;
    if (alert) {
        const slot = find_object_field_slot(alert, 'timeout');
        if (slot >= 0 && OBJECT_HAS_FIELDS(alert, slot + 1)) alert.fields[slot].i = timeout;
    }
    return { i: 0 };
}

function native_alert_getTimeout(jvm, thread, args, arg_count) {
    const alert = args[0].ref;
    if (alert) {
        const slot = find_object_field_slot(alert, 'timeout');
        if (slot >= 0 && OBJECT_HAS_FIELDS(alert, slot + 1)) return { i: alert.fields[slot].i };
    }
    return { i: 0 };
}

// ---------------------------------------------------------------------------
// Native: TextBox
// ---------------------------------------------------------------------------

function native_textbox_init(jvm, thread, args, arg_count) {
    const textbox     = args[0].ref;
    const title       = args[1].ref;
    const text        = args[2].ref;
    const max_size    = args[3].i;
    const constraints = args[4].i;
    if (!textbox) return { i: 0 };
    const ts  = find_object_field_slot(textbox, 'title');
    const xs  = find_object_field_slot(textbox, 'text');
    const ms  = find_object_field_slot(textbox, 'maxSize');
    const cs  = find_object_field_slot(textbox, 'constraints');
    if (ts >= 0 && xs >= 0 && ms >= 0 && cs >= 0 && OBJECT_HAS_FIELDS(textbox, cs + 1)) {
        textbox.fields[ts].ref = title;
        textbox.fields[xs].ref = text;
        textbox.fields[ms].i   = max_size;
        textbox.fields[cs].i   = constraints;
    }
    return { i: 0 };
}

function native_textbox_setString(jvm, thread, args, arg_count) {
    const textbox = args[0].ref;
    const text    = args[1].ref;
    if (textbox) {
        const slot = find_object_field_slot(textbox, 'text');
        if (slot >= 0 && OBJECT_HAS_FIELDS(textbox, slot + 1)) textbox.fields[slot].ref = text;
    }
    return { i: 0 };
}

function native_textbox_getString(jvm, thread, args, arg_count) {
    const textbox = args[0].ref;
    if (textbox) {
        const slot = find_object_field_slot(textbox, 'text');
        if (slot >= 0 && OBJECT_HAS_FIELDS(textbox, slot + 1)) return { ref: textbox.fields[slot].ref };
    }
    return { ref: null };
}

function native_textbox_setTitle(jvm, thread, args, arg_count) {
    const textbox = args[0].ref;
    const title   = args[1].ref;
    if (textbox) {
        const slot = find_object_field_slot(textbox, 'title');
        if (slot >= 0 && OBJECT_HAS_FIELDS(textbox, slot + 1)) textbox.fields[slot].ref = title;
    }
    return { i: 0 };
}

function native_textbox_getTitle(jvm, thread, args, arg_count) {
    const textbox = args[0].ref;
    if (textbox) {
        const slot = find_object_field_slot(textbox, 'title');
        if (slot >= 0 && OBJECT_HAS_FIELDS(textbox, slot + 1)) return { ref: textbox.fields[slot].ref };
    }
    return { ref: null };
}

function native_textbox_getMaxSize(jvm, thread, args, arg_count) {
    const textbox = args[0].ref;
    if (textbox) {
        const slot = find_object_field_slot(textbox, 'maxSize');
        if (slot >= 0 && OBJECT_HAS_FIELDS(textbox, slot + 1)) return { i: textbox.fields[slot].i };
    }
    return { i: 32 };
}

function native_textbox_getConstraints(jvm, thread, args, arg_count) {
    const textbox = args[0].ref;
    if (textbox) {
        const slot = find_object_field_slot(textbox, 'constraints');
        if (slot >= 0 && OBJECT_HAS_FIELDS(textbox, slot + 1)) return { i: textbox.fields[slot].i };
    }
    return { i: 0 };
}

// ---------------------------------------------------------------------------
// Native: Command
// ---------------------------------------------------------------------------

function native_command_init(jvm, thread, args, arg_count) {
    const cmd      = args[0].ref;
    const label    = args[1].ref;
    const type     = args[2].i;
    const priority = args[3].i;
    if (cmd && OBJECT_HAS_FIELDS(cmd, 3)) {
        cmd.fields[0].ref = label;
        cmd.fields[1].i   = type;
        cmd.fields[2].i   = priority;
    }
    return { i: 0 };
}

function native_command_getCommandType(jvm, thread, args, arg_count) {
    const cmd = args[0].ref;
    if (!cmd) return { i: 0 };
    let slot = find_object_field_slot(cmd, 'type');
    if (slot < 0) slot = 1;
    if (OBJECT_HAS_FIELDS(cmd, slot + 1)) return { i: cmd.fields[slot].i };
    return { i: 0 };
}

function native_command_getLabel(jvm, thread, args, arg_count) {
    const cmd = args[0].ref;
    if (!cmd) return { ref: null };
    let slot = find_object_field_slot(cmd, 'label');
    if (slot < 0) slot = 0;
    if (OBJECT_HAS_FIELDS(cmd, slot + 1)) return { ref: cmd.fields[slot].ref };
    return { ref: null };
}

function native_command_getPriority(jvm, thread, args, arg_count) {
    const cmd = args[0].ref;
    if (!cmd) return { i: 0 };
    let slot = find_object_field_slot(cmd, 'priority');
    if (slot < 0) slot = 2;
    if (OBJECT_HAS_FIELDS(cmd, slot + 1)) return { i: cmd.fields[slot].i };
    return { i: 0 };
}

// ---------------------------------------------------------------------------
// Native: Displayable
// ---------------------------------------------------------------------------

function native_displayable_addCommand(jvm, thread, args, arg_count) {
    const displayable = args[0].ref;
    const cmd         = args[1].ref;
    if (!displayable || !cmd) return { i: 0 };
    const commands_slot = find_object_field_slot(displayable, 'commands');
    if (commands_slot < 0) return { i: 0 };
    if (!OBJECT_HAS_FIELDS(displayable, commands_slot + 1)) return { i: 0 };
    let commands = displayable.fields[commands_slot].ref;
    if (!commands) {
        commands = jvm_new_array(jvm, DESC_OBJECT, 0, null);
        displayable.fields[commands_slot].ref = commands;
    }
    const old_count = commands ? commands.length : 0;
    if (old_count > 0) {
        const old_data = array_data(commands);
        if (old_data) {
            for (let i = 0; i < old_count; i++) {
                if (old_data[i] === cmd) return { i: 0 };
            }
        }
    }
    const new_commands = jvm_new_array(jvm, DESC_OBJECT, old_count + 1, null);
    if (!new_commands) return { i: 0 };
    const nd = array_data(new_commands);
    if (old_count > 0) {
        const od = array_data(commands);
        for (let i = 0; i < old_count; i++) nd[i] = od[i];
    }
    nd[old_count] = cmd;
    displayable.fields[commands_slot].ref = new_commands;
    return { i: 0 };
}

function native_displayable_removeCommand(jvm, thread, args, arg_count) {
    const displayable = args[0].ref;
    const cmd         = args[1].ref;
    if (!displayable || !cmd) return { i: 0 };
    const commands_slot = find_object_field_slot(displayable, 'commands');
    if (commands_slot < 0) return { i: 0 };
    const commands = displayable.fields[commands_slot].ref;
    if (!commands || commands.length === 0) return { i: 0 };
    const old_data  = array_data(commands);
    const old_count = commands.length;
    let found_idx = -1;
    if (old_data) {
        for (let i = 0; i < old_count; i++) {
            if (old_data[i] === cmd) { found_idx = i; break; }
        }
    }
    if (found_idx < 0) return { i: 0 };
    const new_count    = old_count - 1;
    const new_commands = jvm_new_array(jvm, DESC_OBJECT, new_count, null);
    if (!new_commands) return { i: 0 };
    const nd = array_data(new_commands);
    for (let i = 0, j = 0; i < old_count; i++) {
        if (i !== found_idx) nd[j++] = old_data[i];
    }
    displayable.fields[commands_slot].ref = new_commands;
    return { i: 0 };
}

function native_displayable_setCommandListener(jvm, thread, args, arg_count) {
    const displayable = args[0].ref;
    const listener    = args[1].ref;
    if (!displayable) return { i: 0 };
    const slot = find_object_field_slot(displayable, 'listener');
    if (slot >= 0) displayable.fields[slot].ref = listener;
    return { i: 0 };
}

// ---------------------------------------------------------------------------
// Native: Spacer
// ---------------------------------------------------------------------------

function native_spacer_init(jvm, thread, args, arg_count) {
    const spacer  = args[0].ref;
    const width   = args[1].i;
    const height  = args[2].i;
    if (!spacer) return { i: 0 };
    const ws = find_object_field_slot(spacer, 'width');
    const hs = find_object_field_slot(spacer, 'height');
    if (ws >= 0 && hs >= 0 && OBJECT_HAS_FIELDS(spacer, hs + 1)) {
        spacer.fields[ws].i = width;
        spacer.fields[hs].i = height;
    }
    return { i: 0 };
}

function native_spacer_setMinimumSize(jvm, thread, args, arg_count) {
    return native_spacer_init(jvm, thread, args, arg_count);
}

// ---------------------------------------------------------------------------
// Native: ImageItem
// ---------------------------------------------------------------------------

function native_imageitem_init(jvm, thread, args, arg_count) {
    const item     = args[0].ref;
    const label    = args[1].ref;
    const image    = args[2].ref;
    const layout   = args[3].i;
    const alt_text = args[4].ref;
    if (!item) return { i: 0 };
    const ls  = find_object_field_slot(item, 'label');
    const is  = find_object_field_slot(item, 'image');
    const lys = find_object_field_slot(item, 'layout');
    const ats = find_object_field_slot(item, 'altText');
    if (ls >= 0 && is >= 0 && lys >= 0 && ats >= 0 && OBJECT_HAS_FIELDS(item, ats + 1)) {
        item.fields[ls].ref  = label;
        item.fields[is].ref  = image;
        item.fields[lys].i   = layout;
        item.fields[ats].ref = alt_text;
    }
    return { i: 0 };
}

function native_imageitem_setImage(jvm, thread, args, arg_count) {
    const item  = args[0].ref;
    const image = args[1].ref;
    if (item) {
        const slot = find_object_field_slot(item, 'image');
        if (slot >= 0 && OBJECT_HAS_FIELDS(item, slot + 1)) item.fields[slot].ref = image;
    }
    return { i: 0 };
}

// ---------------------------------------------------------------------------
// Native: Choice (shared by List and ChoiceGroup)
// ---------------------------------------------------------------------------

function native_choice_size(jvm, thread, args, arg_count) {
    const choice = args[0].ref;
    if (choice) {
        const slot = find_object_field_slot(choice, 'strings');
        if (slot >= 0 && OBJECT_HAS_FIELDS(choice, slot + 1)) {
            const strings = choice.fields[slot].ref;
            if (strings) return { i: strings.length };
        }
    }
    return { i: 0 };
}

function native_choice_getString(jvm, thread, args, arg_count) {
    const choice = args[0].ref;
    const index  = args[1].i;
    if (choice) {
        const slot = find_object_field_slot(choice, 'strings');
        if (slot >= 0 && OBJECT_HAS_FIELDS(choice, slot + 1)) {
            const strings = choice.fields[slot].ref;
            if (strings && index >= 0 && index < strings.length) {
                const data = array_data(strings);
                return { ref: data ? data[index] : null };
            }
        }
    }
    return { ref: null };
}

function native_choice_isSelected(jvm, thread, args, arg_count) {
    const choice = args[0].ref;
    const index  = args[1].i;
    if (choice) {
        const list_class   = choice.header && choice.header.clazz;
        const selected_idx = list_class ? get_field_offset(list_class, 'selected') : -1;
        if (selected_idx >= 0) {
            const selected = choice.fields[selected_idx].ref;
            if (selected && selected.element_type === T_BOOLEAN && index >= 0 && index < selected.length) {
                const data = array_data(selected);
                return { i: data ? (data[index] ? 1 : 0) : 0 };
            }
        }
    }
    return { i: 0 };
}

// ---------------------------------------------------------------------------
// Native: Ticker
// ---------------------------------------------------------------------------

function native_ticker_init(jvm, thread, args, arg_count) {
    const ticker = args[0].ref;
    const text   = args[1].ref;
    if (ticker && OBJECT_HAS_FIELDS(ticker, 1)) ticker.fields[0].ref = text;
    return { i: 0 };
}

function native_ticker_getString(jvm, thread, args, arg_count) {
    const ticker = args[0].ref;
    if (ticker && OBJECT_HAS_FIELDS(ticker, 1)) return { ref: ticker.fields[0].ref };
    return { ref: null };
}

function native_ticker_setString(jvm, thread, args, arg_count) {
    const ticker = args[0].ref;
    const text   = args[1].ref;
    if (ticker && OBJECT_HAS_FIELDS(ticker, 1)) ticker.fields[0].ref = text;
    return { i: 0 };
}

// ---------------------------------------------------------------------------
// Native: DateField
// ---------------------------------------------------------------------------

function native_datefield_init(jvm, thread, args, arg_count) {
    const field = args[0].ref;
    const label = args[1].ref;
    const mode  = args[2].i;
    if (field) {
        const ls = find_object_field_slot(field, 'label');
        const ms = find_object_field_slot(field, 'mode');
        const ds = find_object_field_slot(field, 'date');
        if (ls >= 0 && ms >= 0 && ds >= 0 && OBJECT_HAS_FIELDS(field, ds + 1)) {
            field.fields[ls].ref = label;
            field.fields[ms].i   = mode;
            field.fields[ds].j   = 0n;
        }
    }
    return { i: 0 };
}

// ---------------------------------------------------------------------------
// ensure_select_command
// ---------------------------------------------------------------------------

function ensure_select_command(jvm) {
    if (g_select_command) return;
    const cmd_class = jvm_load_class(jvm, 'javax/microedition/lcdui/Command');
    if (!cmd_class) return;
    const select_cmd = jvm_new_object(jvm, cmd_class);
    if (select_cmd && OBJECT_HAS_FIELDS(select_cmd, 3)) {
        const label = jvm_new_string(jvm, 'Select');
        select_cmd.fields[0] = { ref: label };
        select_cmd.fields[1] = { i: 4 };
        select_cmd.fields[2] = { i: 0 };
    }
    g_select_command = select_cmd;
    const list_class = jvm_load_class(jvm, 'javax/microedition/lcdui/List');
    if (list_class && list_class.static_fields_count > 0) {
        list_class.static_fields[0].value = { ref: select_cmd };
    }
}

// ---------------------------------------------------------------------------
// midp_set_current_displayable
// ---------------------------------------------------------------------------

export function midp_set_current_displayable(displayable) {
    g_current_displayable = displayable;
    g_focused_item_index = 0;
    g_list_selected_index = 0;
    g_vkb.active = false;
}

// ---------------------------------------------------------------------------
// midp_render_form
// ---------------------------------------------------------------------------

export function midp_render_form(jvm, form) {
    if (!form) return;
    g_current_displayable = form;
    const gfx = get_screen_graphics();
    if (!gfx) return;

    midp_graphics_set_color(gfx, 0xFFFFFF, 255);
    midp_graphics_fill_rect(gfx, 0, 0, gfx.width, gfx.height);

    let title = '';
    const title_slot = find_object_field_slot(form, 'title');
    if (title_slot >= 0 && OBJECT_HAS_FIELDS(form, title_slot + 1)) {
        const title_str = form.fields[title_slot].ref;
        if (title_str) title = get_string_from_object(jvm, title_str);
    }

    midp_graphics_set_color(gfx, 0x000080, 255);
    midp_graphics_fill_rect(gfx, 0, 0, gfx.width, 20);
    midp_graphics_set_color(gfx, 0xFFFFFF, 255);
    midp_graphics_draw_string(gfx, title, Math.floor(gfx.width / 2), 3, 0x10);

    midp_graphics_set_color(gfx, 0x808080, 255);
    midp_graphics_draw_line(gfx, 0, 20, gfx.width, 20);

    let y = 25;
    const max_y = g_vkb.active ? (gfx.height - 170) : (gfx.height - 30);

    const items_slot = find_object_field_slot(form, 'items');
    if (items_slot >= 0 && OBJECT_HAS_FIELDS(form, items_slot + 1)) {
        const items = form.fields[items_slot].ref;
        if (items && items.element_type === DESC_OBJECT) {
            const items_data = array_data(items);
            for (let i = 0; i < items.length && y < max_y; i++) {
                const item = items_data[i];
                if (!item) continue;
                const focused = (i === g_focused_item_index);
                const item_type = get_item_type(item);
                switch (item_type) {
                    case ITEM_STRINGITEM:  y = render_stringitem(jvm, gfx, item, y); break;
                    case ITEM_TEXTFIELD:   y = render_textfield(jvm, gfx, item, y, focused); break;
                    case ITEM_CHOICEGROUP: y = render_choicegroup(jvm, gfx, item, y, focused); break;
                    case ITEM_GAUGE:       y = render_gauge(jvm, gfx, item, y, focused); break;
                    case ITEM_SPACER:      y = render_spacer(jvm, gfx, item, y); break;
                    default: y += 20; break;
                }
            }
        }
    }

    if (!g_vkb.active) {
        midp_graphics_set_color(gfx, 0xC0C0C0, 255);
        midp_graphics_fill_rect(gfx, 0, gfx.height - 25, gfx.width, 25);
        midp_graphics_set_color(gfx, 0x000000, 255);
        midp_graphics_draw_line(gfx, 0, gfx.height - 25, gfx.width, gfx.height - 25);

        const commands_idx = find_object_field_slot(form, 'commands');
        if (commands_idx >= 0 && OBJECT_HAS_FIELDS(form, commands_idx + 1)) {
            const commands = form.fields[commands_idx].ref;
            if (commands && commands.length > 0) {
                const cmd_data = array_data(commands);
                const cmd_count = commands.length;
                let left_cmd_idx = 0;
                let right_cmd_idx = -1;

                for (let ci = 0; ci < cmd_count; ci++) {
                    const cmd = cmd_data[ci];
                    if (!cmd || !cmd.header || !cmd.header.clazz) continue;
                    if (OBJECT_HAS_FIELDS(cmd, 3)) {
                        const cmd_type = cmd.fields[1].i;
                        if ((cmd_type === 2 || cmd_type === 7) && right_cmd_idx < 0) {
                            right_cmd_idx = ci;
                        }
                    }
                }

                if (cmd_count === 1) {
                    left_cmd_idx = 0;
                    right_cmd_idx = -1;
                } else if (right_cmd_idx >= 0 && cmd_count >= 2) {
                    left_cmd_idx = (right_cmd_idx === 0) ? 1 : 0;
                } else {
                    left_cmd_idx = 0;
                    right_cmd_idx = (cmd_count >= 2) ? 1 : -1;
                }

                const draw_label = (cmd, x, align_right) => {
                    if (!cmd || !OBJECT_HAS_FIELDS(cmd, 3)) return;
                    const lref = cmd.fields[0].ref;
                    let text = lref ? (string_utf8(jvm, lref) || '') : '';
                    if (!text) text = align_right ? 'Back' : 'OK';
                    if (align_right) {
                        midp_graphics_draw_string(gfx, text, gfx.width - text.length * 6 - 5, gfx.height - 20, 0);
                    } else {
                        midp_graphics_draw_string(gfx, text, 5, gfx.height - 20, 0);
                    }
                };

                draw_label(cmd_data[left_cmd_idx], 5, false);
                if (right_cmd_idx >= 0) draw_label(cmd_data[right_cmd_idx], 0, true);
            }
        }
    }

    vkb_render(gfx);
    _sdl_request_redraw();
}

// ---------------------------------------------------------------------------
// midp_render_alert
// ---------------------------------------------------------------------------

export function midp_render_alert(jvm, gfx, alert) {
    if (!alert || !gfx) return;
    render_alert_internal(jvm, gfx, alert);
}

// ---------------------------------------------------------------------------
// midp_render_list
// ---------------------------------------------------------------------------

export function midp_render_list(jvm, list, focused_index) {
    if (!list) return;
    g_current_displayable = list;
    const gfx = get_screen_graphics();
    if (!gfx) return;
    midp_graphics_set_color(gfx, 0xFFFFFF, 255);
    midp_graphics_fill_rect(gfx, 0, 0, gfx.width, gfx.height);
    render_list_internal(jvm, gfx, list, focused_index);
    _sdl_request_redraw();
}

// ---------------------------------------------------------------------------
// midp_render_textbox
// ---------------------------------------------------------------------------

export function midp_render_textbox(jvm, textbox, input_text, cursor_pos) {
    if (!textbox) return;
    g_current_displayable = textbox;
    const gfx = get_screen_graphics();
    if (!gfx) return;
    midp_graphics_set_color(gfx, 0xFFFFFF, 255);
    midp_graphics_fill_rect(gfx, 0, 0, gfx.width, gfx.height);
    render_textbox_internal(jvm, gfx, textbox, input_text, cursor_pos);
    vkb_render(gfx);
    _sdl_request_redraw();
}

// ---------------------------------------------------------------------------
// midp_check_alert_timeout
// ---------------------------------------------------------------------------

export function midp_check_alert_timeout(jvm) {
    if (!g_current_displayable) return false;
    const clazz = g_current_displayable.header && g_current_displayable.header.clazz;
    if (!clazz || !clazz.class_name) return false;

    let is_alert = false;
    let check = clazz;
    while (check) {
        if (check.class_name === 'javax/microedition/lcdui/Alert') { is_alert = true; break; }
        check = check.super_class || null;
    }
    if (!is_alert) return false;

    const timeout_slot = find_object_field_slot(g_current_displayable, 'timeout');
    if (timeout_slot < 0 || !OBJECT_HAS_FIELDS(g_current_displayable, timeout_slot + 1)) return false;

    let timeout = g_current_displayable.fields[timeout_slot].i;
    if (timeout === -1) return false;
    if (timeout <= 0) timeout = 2000;

    const sdl_ctx = _sdl_get_global_context();
    const now = _sdl_get_ticks(sdl_ctx);

    if (_last_alert !== g_current_displayable) {
        _last_alert = g_current_displayable;
        _alert_start_time = now;
        return false;
    }

    if (!sdl_ctx) return false;
    const elapsed = Number(now - _alert_start_time);
    if (elapsed < timeout) return false;

    const listener_slot = find_object_field_slot(g_current_displayable, 'listener');
    if (listener_slot >= 0 && OBJECT_HAS_FIELDS(g_current_displayable, listener_slot + 1)) {
        const listener = g_current_displayable.fields[listener_slot].ref;
        if (listener && listener.header && listener.header.clazz) {
            const cmd_class = jvm_load_class(jvm, 'javax/microedition/lcdui/Command');
            if (cmd_class) {
                const dismiss_cmd = jvm_new_object(jvm, cmd_class);
                if (dismiss_cmd && OBJECT_HAS_FIELDS(dismiss_cmd, 3)) {
                    dismiss_cmd.fields[0] = { ref: jvm_new_string(jvm, 'Dismiss') };
                    dismiss_cmd.fields[1] = { i: 3 };
                    dismiss_cmd.fields[2] = { i: 0 };
                    const method = jvm_resolve_method(jvm, listener.header.clazz,
                        'commandAction',
                        '(Ljavax/microedition/lcdui/Command;Ljavax/microedition/lcdui/Displayable;)V');
                    if (method) {
                        _execute_method(jvm, jvm_current_thread(jvm), method,
                            [{ ref: listener }, { ref: dismiss_cmd }, { ref: g_current_displayable }], {});
                    }
                }
            }
        }
    }

    _last_alert = null;
    return true;
}

// ---------------------------------------------------------------------------
// midp_form_handle_key
// ---------------------------------------------------------------------------

export function midp_form_handle_key(jvm, game_action) {
    if (g_vkb.active) return vkb_handle_key(jvm, game_action);

    if (!g_current_displayable) return false;
    const clazz = g_current_displayable.header && g_current_displayable.header.clazz;
    if (!clazz || !clazz.class_name) return false;

    if (clazz.class_name.includes('Form')) {
        let item_count = 0;
        let items = null;
        const items_slot = find_object_field_slot(g_current_displayable, 'items');
        if (items_slot >= 0 && OBJECT_HAS_FIELDS(g_current_displayable, items_slot + 1)) {
            items = g_current_displayable.fields[items_slot].ref;
            if (items) item_count = items.length;
        }

        switch (game_action) {
            case 1: // UP
                if (g_focused_item_index > 0) {
                    g_focused_item_index--;
                    midp_render_form(jvm, g_current_displayable);
                    return true;
                }
                break;
            case 6: // DOWN
                if (g_focused_item_index < item_count - 1) {
                    g_focused_item_index++;
                    midp_render_form(jvm, g_current_displayable);
                    return true;
                }
                break;
            case 8: { // FIRE
                if (items && g_focused_item_index < items.length) {
                    const items_data = array_data(items);
                    const item = items_data[g_focused_item_index];
                    const item_type = get_item_type(item);

                    if (item_type === ITEM_TEXTFIELD) {
                        let max_size = 32, constraints = 0;
                        const ms = find_object_field_slot(item, 'maxSize');
                        const cs = find_object_field_slot(item, 'constraints');
                        if (ms >= 0 && cs >= 0 && OBJECT_HAS_FIELDS(item, cs + 1)) {
                            max_size = item.fields[ms].i;
                            constraints = item.fields[cs].i;
                        }
                        vkb_start(item, max_size, constraints);
                        midp_render_form(jvm, g_current_displayable);
                        return true;
                    } else if (item_type === ITEM_CHOICEGROUP) {
                        const cts = find_object_field_slot(item, 'choiceType');
                        const sels = find_object_field_slot(item, 'selected');
                        if (cts >= 0 && sels >= 0 && OBJECT_HAS_FIELDS(item, sels + 1)) {
                            const choice_type = item.fields[cts].i;
                            const selected = item.fields[sels].ref;
                            const sel_count = selected ? selected.length : 0;
                            if (selected && sel_count > 0) {
                                const sel_data = array_data(selected);
                                if (choice_type === CHOICE_EXCLUSIVE || choice_type === CHOICE_POPUP) {
                                    let cur = -1;
                                    for (let si = 0; si < sel_count; si++) {
                                        if (sel_data[si]) { cur = si; break; }
                                    }
                                    if (cur >= 0) sel_data[cur] = 0;
                                    sel_data[(cur + 1) % sel_count] = 1;
                                } else {
                                    const idx = g_cg_focused_element % sel_count;
                                    sel_data[idx] = sel_data[idx] ? 0 : 1;
                                    g_cg_focused_element = (g_cg_focused_element + 1) % sel_count;
                                }
                                _notify_item_state(jvm, item);
                                midp_render_form(jvm, g_current_displayable);
                                return true;
                            }
                        }
                    } else if (item_type === ITEM_GAUGE) {
                        const mvs = find_object_field_slot(item, 'maxValue');
                        const vs  = find_object_field_slot(item, 'value');
                        if (mvs >= 0 && vs >= 0 && OBJECT_HAS_FIELDS(item, vs + 1)) {
                            const max_val = item.fields[mvs].i;
                            let cur_val = item.fields[vs].i;
                            if (max_val > 0) {
                                cur_val = (cur_val + 1) % (max_val + 1);
                                item.fields[vs].i = cur_val;
                                _notify_item_state(jvm, item);
                                midp_render_form(jvm, g_current_displayable);
                                return true;
                            }
                        }
                    }
                }
                break;
            }
        }
    } else if (clazz.class_name.includes('List')) {
        let list_size = 0;
        const strings_slot = find_object_field_slot(g_current_displayable, 'strings');
        if (strings_slot >= 0 && OBJECT_HAS_FIELDS(g_current_displayable, strings_slot + 1)) {
            const strings = g_current_displayable.fields[strings_slot].ref;
            if (strings) list_size = strings.length;
        }

        switch (game_action) {
            case 1: // UP
                if (g_list_selected_index > 0) {
                    g_list_selected_index--;
                    midp_render_list(jvm, g_current_displayable, g_list_selected_index);
                    return true;
                }
                break;
            case 6: // DOWN
                if (g_list_selected_index < list_size - 1) {
                    g_list_selected_index++;
                    midp_render_list(jvm, g_current_displayable, g_list_selected_index);
                    return true;
                }
                break;
            case 8: { // FIRE
                let listener = null;
                let listener_idx = get_field_offset(g_current_displayable.header.clazz, 'listener');
                if (listener_idx < 0) {
                    listener_idx = get_field_offset(g_current_displayable.header.clazz, 'commandListener');
                }
                if (listener_idx >= 0 && OBJECT_HAS_FIELDS(g_current_displayable, listener_idx + 1)) {
                    listener = g_current_displayable.fields[listener_idx].ref;
                }

                if (listener) {
                    ensure_select_command(jvm);
                    if (g_select_command) {
                        const method = jvm_resolve_method(jvm, listener.header.clazz,
                            'commandAction',
                            '(Ljavax/microedition/lcdui/Command;Ljavax/microedition/lcdui/Displayable;)V');
                        if (method) {
                            _execute_method(jvm, jvm_current_thread(jvm), method,
                                [{ ref: listener }, { ref: g_select_command }, { ref: g_current_displayable }], {});
                        }
                    }
                } else {
                    _call_command_action_for_list(jvm, g_current_displayable, g_list_selected_index);
                }
                return true;
            }
        }
    } else if (clazz.class_name.includes('TextBox')) {
        if (game_action === 8) {
            let max_size = 32, constraints = 0;
            const ms = find_object_field_slot(g_current_displayable, 'maxSize');
            const cs = find_object_field_slot(g_current_displayable, 'constraints');
            if (ms >= 0 && cs >= 0 && OBJECT_HAS_FIELDS(g_current_displayable, cs + 1)) {
                max_size = g_current_displayable.fields[ms].i;
                constraints = g_current_displayable.fields[cs].i;
            }
            vkb_start(g_current_displayable, max_size, constraints);
            midp_render_textbox(jvm, g_current_displayable, g_vkb.text_buffer, g_vkb.cursor_pos);
            return true;
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// Internal: notify ItemStateListener
// ---------------------------------------------------------------------------

function _notify_item_state(jvm, item) {
    if (!g_item_state_listener || !g_item_state_listener.header || !g_item_state_listener.header.clazz) return;
    const ism = jvm_resolve_method(jvm, g_item_state_listener.header.clazz,
        'itemStateChanged', '(Ljavax/microedition/lcdui/Item;)V');
    if (ism) {
        _execute_method(jvm, jvm_current_thread(jvm), ism,
            [{ ref: g_item_state_listener }, { ref: item }], {});
    }
}

// ---------------------------------------------------------------------------
// native_register_methods (local helper)
// ---------------------------------------------------------------------------

function native_register_methods(jvm, methods) {
    if (jvm && typeof jvm.registerNativeMethods === 'function') {
        jvm.registerNativeMethods(methods);
    }
}

// ---------------------------------------------------------------------------
// init_javax_microedition_lcdui_form
// ---------------------------------------------------------------------------

export function init_javax_microedition_lcdui_form(jvm) {
    gc_add_root(jvm, () => g_current_form,           (v) => { g_current_form = v; });
    gc_add_root(jvm, () => g_current_displayable,    (v) => { g_current_displayable = v; });
    gc_add_root(jvm, () => g_select_command,         (v) => { g_select_command = v; });
    gc_add_root(jvm, () => g_item_state_listener,    (v) => { g_item_state_listener = v; });

    const methods = [
        { class_name: 'javax/microedition/lcdui/Form',        method_name: '<init>',               descriptor: '(Ljava/lang/String;)V',                                                                                            handler: native_form_init },
        { class_name: 'javax/microedition/lcdui/Form',        method_name: 'append',               descriptor: '(Ljavax/microedition/lcdui/Item;)I',                                                                               handler: native_form_append },
        { class_name: 'javax/microedition/lcdui/Form',        method_name: 'delete',               descriptor: '(I)V',                                                                                                             handler: native_form_delete },
        { class_name: 'javax/microedition/lcdui/Form',        method_name: 'deleteAll',             descriptor: '()V',                                                                                                             handler: native_form_deleteAll },
        { class_name: 'javax/microedition/lcdui/Form',        method_name: 'size',                 descriptor: '()I',                                                                                                             handler: native_form_size },
        { class_name: 'javax/microedition/lcdui/Form',        method_name: 'get',                  descriptor: '(I)Ljavax/microedition/lcdui/Item;',                                                                               handler: native_form_get },
        { class_name: 'javax/microedition/lcdui/Form',        method_name: 'getTitle',              descriptor: '()Ljava/lang/String;',                                                                                             handler: native_form_getTitle },
        { class_name: 'javax/microedition/lcdui/Form',        method_name: 'setTitle',              descriptor: '(Ljava/lang/String;)V',                                                                                            handler: native_form_setTitle },
        { class_name: 'javax/microedition/lcdui/Form',        method_name: 'setItemStateListener',  descriptor: '(Ljavax/microedition/lcdui/ItemStateListener;)V',                                                                 handler: native_form_setItemStateListener },
        { class_name: 'javax/microedition/lcdui/Form',        method_name: 'setTicker',             descriptor: '(Ljavax/microedition/lcdui/Ticker;)V',                                                                            handler: native_form_setTicker },
        { class_name: 'javax/microedition/lcdui/Form',        method_name: 'getTicker',             descriptor: '()Ljavax/microedition/lcdui/Ticker;',                                                                             handler: native_form_getTicker },
        { class_name: 'javax/microedition/lcdui/Item',        method_name: 'setLabel',             descriptor: '(Ljava/lang/String;)V',                                                                                            handler: native_item_setLabel },
        { class_name: 'javax/microedition/lcdui/Item',        method_name: 'getLabel',             descriptor: '()Ljava/lang/String;',                                                                                             handler: native_item_getLabel },
        { class_name: 'javax/microedition/lcdui/StringItem',  method_name: '<init>',               descriptor: '(Ljava/lang/String;Ljava/lang/String;)V',                                                                          handler: native_stringitem_init },
        { class_name: 'javax/microedition/lcdui/StringItem',  method_name: 'setText',              descriptor: '(Ljava/lang/String;)V',                                                                                            handler: native_stringitem_setText },
        { class_name: 'javax/microedition/lcdui/StringItem',  method_name: 'getText',              descriptor: '()Ljava/lang/String;',                                                                                             handler: native_stringitem_getText },
        { class_name: 'javax/microedition/lcdui/TextField',   method_name: '<init>',               descriptor: '(Ljava/lang/String;Ljava/lang/String;II)V',                                                                        handler: native_textfield_init },
        { class_name: 'javax/microedition/lcdui/TextField',   method_name: 'setString',            descriptor: '(Ljava/lang/String;)V',                                                                                            handler: native_textfield_setString },
        { class_name: 'javax/microedition/lcdui/TextField',   method_name: 'getString',            descriptor: '()Ljava/lang/String;',                                                                                             handler: native_textfield_getString },
        { class_name: 'javax/microedition/lcdui/TextField',   method_name: 'setChars',             descriptor: '([CII)V',                                                                                                          handler: native_textfield_setChars },
        { class_name: 'javax/microedition/lcdui/TextField',   method_name: 'size',                 descriptor: '()I',                                                                                                              handler: native_textfield_size },
        { class_name: 'javax/microedition/lcdui/TextField',   method_name: 'getMaxSize',           descriptor: '()I',                                                                                                              handler: native_textfield_getMaxSize },
        { class_name: 'javax/microedition/lcdui/TextField',   method_name: 'setMaxSize',           descriptor: '(I)I',                                                                                                             handler: native_textfield_setMaxSize },
        { class_name: 'javax/microedition/lcdui/TextField',   method_name: 'getConstraints',       descriptor: '()I',                                                                                                              handler: native_textfield_getConstraints },
        { class_name: 'javax/microedition/lcdui/TextField',   method_name: 'setConstraints',       descriptor: '(I)V',                                                                                                             handler: native_textfield_setConstraints },
        { class_name: 'javax/microedition/lcdui/TextField',   method_name: 'getChars',             descriptor: '([C)V',                                                                                                            handler: native_textfield_getChars },
        { class_name: 'javax/microedition/lcdui/TextField',   method_name: 'delete',               descriptor: '(II)V',                                                                                                            handler: native_textfield_delete },
        { class_name: 'javax/microedition/lcdui/TextField',   method_name: 'insert',               descriptor: '(Ljava/lang/String;I)V',                                                                                           handler: native_textfield_insert },
        { class_name: 'javax/microedition/lcdui/TextField',   method_name: 'getCaretPosition',     descriptor: '()I',                                                                                                              handler: native_textfield_getCaretPosition },
        { class_name: 'javax/microedition/lcdui/ChoiceGroup', method_name: '<init>',               descriptor: '(Ljava/lang/String;I)V',                                                                                           handler: native_choicegroup_init },
        { class_name: 'javax/microedition/lcdui/ChoiceGroup', method_name: '<init>',               descriptor: '(Ljava/lang/String;I[Ljava/lang/String;[Ljavax/microedition/lcdui/Image;)V',                                       handler: native_choicegroup_init_array },
        { class_name: 'javax/microedition/lcdui/ChoiceGroup', method_name: 'append',               descriptor: '(Ljava/lang/String;Ljavax/microedition/lcdui/Image;)I',                                                            handler: native_choicegroup_append },
        { class_name: 'javax/microedition/lcdui/ChoiceGroup', method_name: 'setSelectedIndex',     descriptor: '(IZ)V',                                                                                                            handler: native_choicegroup_setSelectedIndex },
        { class_name: 'javax/microedition/lcdui/ChoiceGroup', method_name: 'getSelectedIndex',     descriptor: '()I',                                                                                                              handler: native_choicegroup_getSelectedIndex },
        { class_name: 'javax/microedition/lcdui/ChoiceGroup', method_name: 'size',                 descriptor: '()I',                                                                                                              handler: native_choice_size },
        { class_name: 'javax/microedition/lcdui/ChoiceGroup', method_name: 'getString',            descriptor: '(I)Ljava/lang/String;',                                                                                            handler: native_choice_getString },
        { class_name: 'javax/microedition/lcdui/ChoiceGroup', method_name: 'isSelected',           descriptor: '(I)Z',                                                                                                             handler: native_choice_isSelected },
        { class_name: 'javax/microedition/lcdui/Gauge',       method_name: '<init>',               descriptor: '(Ljava/lang/String;ZII)V',                                                                                         handler: native_gauge_init },
        { class_name: 'javax/microedition/lcdui/Gauge',       method_name: 'setValue',             descriptor: '(I)V',                                                                                                             handler: native_gauge_setValue },
        { class_name: 'javax/microedition/lcdui/Gauge',       method_name: 'getValue',             descriptor: '()I',                                                                                                              handler: native_gauge_getValue },
        { class_name: 'javax/microedition/lcdui/List',        method_name: '<init>',               descriptor: '(Ljava/lang/String;I)V',                                                                                           handler: native_list_init },
        { class_name: 'javax/microedition/lcdui/List',        method_name: '<init>',               descriptor: '(Ljava/lang/String;I[Ljava/lang/String;[Ljavax/microedition/lcdui/Image;)V',                                       handler: native_list_init_with_elements },
        { class_name: 'javax/microedition/lcdui/List',        method_name: 'append',               descriptor: '(Ljava/lang/String;Ljavax/microedition/lcdui/Image;)I',                                                            handler: native_list_append },
        { class_name: 'javax/microedition/lcdui/List',        method_name: 'getSelectedIndex',     descriptor: '()I',                                                                                                              handler: native_list_getSelectedIndex },
        { class_name: 'javax/microedition/lcdui/List',        method_name: 'setSelectedIndex',     descriptor: '(IZ)V',                                                                                                            handler: native_list_setSelectedIndex },
        { class_name: 'javax/microedition/lcdui/List',        method_name: 'delete',               descriptor: '(I)V',                                                                                                             handler: native_list_delete },
        { class_name: 'javax/microedition/lcdui/List',        method_name: 'set',                  descriptor: '(ILjava/lang/String;Ljavax/microedition/lcdui/Image;)V',                                                           handler: native_list_set },
        { class_name: 'javax/microedition/lcdui/List',        method_name: 'size',                 descriptor: '()I',                                                                                                              handler: native_list_size },
        { class_name: 'javax/microedition/lcdui/List',        method_name: 'getString',            descriptor: '(I)Ljava/lang/String;',                                                                                            handler: native_list_getString },
        { class_name: 'javax/microedition/lcdui/List',        method_name: 'isSelected',           descriptor: '(I)Z',                                                                                                             handler: native_choice_isSelected },
        { class_name: 'javax/microedition/lcdui/Alert',       method_name: '<init>',               descriptor: '(Ljava/lang/String;Ljava/lang/String;Ljavax/microedition/lcdui/Image;Ljavax/microedition/lcdui/AlertType;)V',      handler: native_alert_init },
        { class_name: 'javax/microedition/lcdui/Alert',       method_name: 'setString',            descriptor: '(Ljava/lang/String;)V',                                                                                            handler: native_alert_setString },
        { class_name: 'javax/microedition/lcdui/Alert',       method_name: 'getString',            descriptor: '()Ljava/lang/String;',                                                                                             handler: native_alert_getString },
        { class_name: 'javax/microedition/lcdui/Alert',       method_name: 'setTitle',             descriptor: '(Ljava/lang/String;)V',                                                                                            handler: native_alert_setTitle },
        { class_name: 'javax/microedition/lcdui/Alert',       method_name: 'getTitle',             descriptor: '()Ljava/lang/String;',                                                                                             handler: native_alert_getTitle },
        { class_name: 'javax/microedition/lcdui/Alert',       method_name: 'setTimeout',           descriptor: '(I)V',                                                                                                             handler: native_alert_setTimeout },
        { class_name: 'javax/microedition/lcdui/Alert',       method_name: 'getTimeout',           descriptor: '()I',                                                                                                              handler: native_alert_getTimeout },
        { class_name: 'javax/microedition/lcdui/TextBox',     method_name: '<init>',               descriptor: '(Ljava/lang/String;Ljava/lang/String;II)V',                                                                        handler: native_textbox_init },
        { class_name: 'javax/microedition/lcdui/TextBox',     method_name: 'setString',            descriptor: '(Ljava/lang/String;)V',                                                                                            handler: native_textbox_setString },
        { class_name: 'javax/microedition/lcdui/TextBox',     method_name: 'getString',            descriptor: '()Ljava/lang/String;',                                                                                             handler: native_textbox_getString },
        { class_name: 'javax/microedition/lcdui/TextBox',     method_name: 'setTitle',             descriptor: '(Ljava/lang/String;)V',                                                                                            handler: native_textbox_setTitle },
        { class_name: 'javax/microedition/lcdui/TextBox',     method_name: 'getTitle',             descriptor: '()Ljava/lang/String;',                                                                                             handler: native_textbox_getTitle },
        { class_name: 'javax/microedition/lcdui/TextBox',     method_name: 'getMaxSize',           descriptor: '()I',                                                                                                              handler: native_textbox_getMaxSize },
        { class_name: 'javax/microedition/lcdui/TextBox',     method_name: 'getConstraints',       descriptor: '()I',                                                                                                              handler: native_textbox_getConstraints },
        { class_name: 'javax/microedition/lcdui/Spacer',      method_name: '<init>',               descriptor: '(II)V',                                                                                                            handler: native_spacer_init },
        { class_name: 'javax/microedition/lcdui/Spacer',      method_name: 'setMinimumSize',       descriptor: '(II)V',                                                                                                            handler: native_spacer_setMinimumSize },
        { class_name: 'javax/microedition/lcdui/ImageItem',   method_name: '<init>',               descriptor: '(Ljava/lang/String;Ljavax/microedition/lcdui/Image;ILjava/lang/String;)V',                                        handler: native_imageitem_init },
        { class_name: 'javax/microedition/lcdui/ImageItem',   method_name: 'setImage',             descriptor: '(Ljavax/microedition/lcdui/Image;)V',                                                                             handler: native_imageitem_setImage },
        { class_name: 'javax/microedition/lcdui/Command',     method_name: '<init>',               descriptor: '(Ljava/lang/String;II)V',                                                                                          handler: native_command_init },
        { class_name: 'javax/microedition/lcdui/Command',     method_name: 'getCommandType',       descriptor: '()I',                                                                                                              handler: native_command_getCommandType },
        { class_name: 'javax/microedition/lcdui/Command',     method_name: 'getLabel',             descriptor: '()Ljava/lang/String;',                                                                                             handler: native_command_getLabel },
        { class_name: 'javax/microedition/lcdui/Command',     method_name: 'getPriority',          descriptor: '()I',                                                                                                              handler: native_command_getPriority },
        { class_name: 'javax/microedition/lcdui/Displayable', method_name: 'addCommand',           descriptor: '(Ljavax/microedition/lcdui/Command;)V',                                                                            handler: native_displayable_addCommand },
        { class_name: 'javax/microedition/lcdui/Displayable', method_name: 'removeCommand',        descriptor: '(Ljavax/microedition/lcdui/Command;)V',                                                                            handler: native_displayable_removeCommand },
        { class_name: 'javax/microedition/lcdui/Displayable', method_name: 'setCommandListener',   descriptor: '(Ljavax/microedition/lcdui/CommandListener;)V',                                                                    handler: native_displayable_setCommandListener },
        { class_name: 'javax/microedition/lcdui/Ticker',      method_name: '<init>',               descriptor: '(Ljava/lang/String;)V',                                                                                            handler: native_ticker_init },
        { class_name: 'javax/microedition/lcdui/Ticker',      method_name: 'getString',            descriptor: '()Ljava/lang/String;',                                                                                             handler: native_ticker_getString },
        { class_name: 'javax/microedition/lcdui/Ticker',      method_name: 'setString',            descriptor: '(Ljava/lang/String;)V',                                                                                            handler: native_ticker_setString },
        { class_name: 'javax/microedition/lcdui/DateField',   method_name: '<init>',               descriptor: '(Ljava/lang/String;I)V',                                                                                           handler: native_datefield_init },
    ];

    native_register_methods(jvm, methods);

    const list_class = jvm_load_class(jvm, 'javax/microedition/lcdui/List');
    if (list_class && list_class.static_fields_count > 0) {
        for (let i = 0; i < list_class.static_fields_count; i++) {
            const sf = list_class.static_fields[i];
            if (sf && sf.name === 'SELECT_COMMAND') {
                const cmd_class = jvm_load_class(jvm, 'javax/microedition/lcdui/Command');
                if (cmd_class) {
                    const select_cmd = jvm_new_object(jvm, cmd_class);
                    if (select_cmd && OBJECT_HAS_FIELDS(select_cmd, 3)) {
                        const empty_label = jvm_new_string(jvm, '');
                        select_cmd.fields[0] = { ref: empty_label };
                        select_cmd.fields[1] = { i: 4 };
                        select_cmd.fields[2] = { i: 0 };
                        sf.value = { ref: select_cmd };
                    }
                }
                break;
            }
        }
    }
}
