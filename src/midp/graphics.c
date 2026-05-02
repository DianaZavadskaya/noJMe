/*
 * J2ME Emulator - MIDP2 Graphics API
 * Supports PNG, JPEG, BMP images via stb_image
 */

#define _USE_MATH_DEFINES
#include <math.h>
#include "debug.h"
#include "debug_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "midp.h"
#include "jvm.h"
#include "bitmap_font.h"
#include "stb_image.h"

/* Current graphics context */
static MidpGraphics* current_gfx = NULL;

/* Headless text capture - tracks drawn text for console output */
#ifdef J2ME_HEADLESS
typedef struct {
    char text[512];
    int x, y;
    uint32_t color;
} HeadlessTextEntry;

static HeadlessTextEntry g_headless_text_log[2048];
static int g_headless_text_count = 0;
static int g_headless_text_frame = 0; /* Incremented each paint cycle */
static int g_headless_last_logged_frame = -1;
static volatile int g_headless_text_activity = 0; /* Set when new text is drawn */

/* Check and reset text activity flag. Returns 1 if text was drawn since last call. */
int headless_check_text_activity(void) {
    int v = g_headless_text_activity;
    g_headless_text_activity = 0;
    return v;
}

/* Add a text entry to the headless log */
static void headless_log_text(const char* text, int x, int y, uint32_t color) {
    if (!text || text[0] == '\0') return;
    
    /* Skip very short or whitespace-only strings */
    int len = strlen(text);
    int printable = 0;
    for (int i = 0; i < len; i++) {
        if (text[i] > ' ') { printable++; }
    }
    if (printable < 2) return;
    
    /* Mark that we have new text activity (for idle detection) */
    g_headless_text_activity = 1;
    
    /* Print immediately to stdout so it's always visible */
    printf("[CANVAS TEXT] (%d,%d) color=0x%06X: %s\n", x, y, color, text);
    fflush(stdout);
    
    if (g_headless_text_count < 2048) {
        HeadlessTextEntry* entry = &g_headless_text_log[g_headless_text_count];
        strncpy(entry->text, text, sizeof(entry->text) - 1);
        entry->text[sizeof(entry->text) - 1] = '\0';
        entry->x = x;
        entry->y = y;
        entry->color = color;
        g_headless_text_count++;
    }
}

/* Print all unique texts captured during the last paint frame to stderr */
void headless_print_captured_text(void) {
    if (g_headless_text_count == 0 || g_headless_last_logged_frame == g_headless_text_frame) return;
    
    /* Deduplicate: only print texts we haven't seen before */
    static char seen_texts[2048][512];
    static int seen_count = 0;
    
    fprintf(stdout, "\n=== Screen text (frame %d) ===\n", g_headless_text_frame);
    
    for (int i = 0; i < g_headless_text_count; i++) {
        char* text = g_headless_text_log[i].text;
        int is_new = 1;
        
        /* Check if we've already logged this text */
        for (int j = 0; j < seen_count && j < 2048; j++) {
            if (strcmp(seen_texts[j], text) == 0) {
                is_new = 0;
                break;
            }
        }
        
        if (is_new && seen_count < 2048) {
            uint32_t c = g_headless_text_log[i].color;
            /* Extract RGB from packed color */
            int r = (c >> 16) & 0xFF;
            int g = (c >> 8) & 0xFF;
            int b = c & 0xFF;
            fprintf(stdout, "  [TEXT] (%d,%d) rgb(%d,%d,%d): %s\n",
                    g_headless_text_log[i].x, g_headless_text_log[i].y, r, g, b, text);
            snprintf(seen_texts[seen_count], 512, "%s", text);
            seen_count++;
        }
    }
    
    g_headless_last_logged_frame = g_headless_text_frame;
    fprintf(stdout, "=== End screen text ===\n\n");
    fflush(stdout);
}

/* Reset captured text for new paint cycle */
void headless_reset_text_capture(void) {
    g_headless_text_count = 0;
    g_headless_text_frame++;
}
#endif /* J2ME_HEADLESS */

/* Runtime screen dimensions (can be changed from libretro) */
static int g_midp_screen_width = MIDP_DEFAULT_WIDTH;
static int g_midp_screen_height = MIDP_DEFAULT_HEIGHT;

/* Set the screen dimensions (called from libretro core) */
void midp_set_screen_dimensions(int width, int height) {
    g_midp_screen_width = width;
    g_midp_screen_height = height;
    DEBUG_LOG("[MIDP] Screen dimensions set to %dx%d", width, height);
}

/* Forward declaration */
static void set_pixel(MidpGraphics* gfx, int x, int y);

/* Draw a single character using the bitmap font (legacy single-byte) */
static void midp_graphics_draw_char(MidpGraphics* gfx, char c, int x, int y) {
    const uint8_t* char_data = get_char_data(c);
    
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t row_data = char_data[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            /* MSB is leftmost pixel, so use (FONT_WIDTH - 1 - col) to get correct bit */
            if (row_data & (1 << (FONT_WIDTH - 1 - col))) {
                set_pixel(gfx, x + col, y + row);
            }
        }
    }
}

/* Draw a single character using Unicode codepoint */
static void midp_graphics_draw_char_unicode(MidpGraphics* gfx, int codepoint, int x, int y) {
    const uint8_t* char_data = get_char_data_unicode(codepoint);
    
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t row_data = char_data[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            /* MSB is leftmost pixel, so use (FONT_WIDTH - 1 - col) to get correct bit */
            if (row_data & (1 << (FONT_WIDTH - 1 - col))) {
                set_pixel(gfx, x + col, y + row);
            }
        }
    }
}

/* Initialize MIDP2 */
int midp_init(JVM* jvm) {
    /* Initialize MIDP2 native methods */
    init_javax_microedition_lcdui_graphics(jvm);
    init_javax_microedition_lcdui_display(jvm);
    init_javax_microedition_lcdui_image(jvm);
    init_javax_microedition_lcdui_font(jvm);
    init_javax_microedition_lcdui_game_gamecanvas(jvm);
    init_javax_microedition_lcdui_form(jvm);  /* Form, TextField, ChoiceGroup, etc. */
    init_javax_microedition_rms(jvm);
    
    /* Initialize Java lang classes */
    extern void init_java_lang_integer(JVM* jvm);
    extern void init_java_util_random(JVM* jvm);
    init_java_lang_integer(jvm);
    init_java_util_random(jvm);
    
    /* Seed random number generator */
    srand((unsigned int)time(NULL));
    
    return JNI_OK;
}

/*
 * Graphics operations
 */

void midp_graphics_init(MidpGraphics* gfx, uint32_t* pixels, int width, int height) {
    gfx->pixels = pixels;
    gfx->width = width;
    gfx->height = height;
    gfx->clip_x = 0;
    gfx->clip_y = 0;
    gfx->clip_width = width;
    gfx->clip_height = height;
    gfx->translate_x = 0;
    gfx->translate_y = 0;
    gfx->rgb_color = 0x000000;
    gfx->alpha = 255;
    gfx->stroke_style = 0;
    gfx->font = 0;
}

void midp_graphics_set_clip(MidpGraphics* gfx, int x, int y, int width, int height) {
    gfx->clip_x = x;
    gfx->clip_y = y;
    gfx->clip_width = width;
    gfx->clip_height = height;
}

void midp_graphics_clip_rect(MidpGraphics* gfx, int x, int y, int width, int height) {
    /* Intersect with current clip */
    int x1 = gfx->clip_x > x ? gfx->clip_x : x;
    int y1 = gfx->clip_y > y ? gfx->clip_y : y;
    int x2 = (gfx->clip_x + gfx->clip_width) < (x + width) ? 
             (gfx->clip_x + gfx->clip_width) : (x + width);
    int y2 = (gfx->clip_y + gfx->clip_height) < (y + height) ?
             (gfx->clip_y + gfx->clip_height) : (y + height);
    
    int new_w = x2 - x1;
    int new_h = y2 - y1;
    /* Clamp to 0 when there is no overlap */
    if (new_w < 0) new_w = 0;
    if (new_h < 0) new_h = 0;
    
    gfx->clip_x = x1;
    gfx->clip_y = y1;
    gfx->clip_width = new_w;
    gfx->clip_height = new_h;
}

void midp_graphics_translate(MidpGraphics* gfx, int x, int y) {
    gfx->translate_x += x;
    gfx->translate_y += y;
}

void midp_graphics_set_color(MidpGraphics* gfx, int rgb, int alpha) {
    GFX_DEBUG("setColor: RGB=0x%06X, alpha=%d", rgb & 0xFFFFFF, alpha);
    gfx->rgb_color = rgb & 0xFFFFFF;
    gfx->alpha = alpha;
}

static void set_pixel(MidpGraphics* gfx, int x, int y) {
    x += gfx->translate_x;
    y += gfx->translate_y;
    
    if (x < gfx->clip_x || x >= gfx->clip_x + gfx->clip_width ||
        y < gfx->clip_y || y >= gfx->clip_y + gfx->clip_height) {
        return;
    }
    
    if (x < 0 || x >= gfx->width || y < 0 || y >= gfx->height) {
        return;
    }
    
    uint32_t color = (gfx->alpha << 24) | gfx->rgb_color;
    gfx->pixels[y * gfx->width + x] = color;
}

void midp_graphics_draw_line(MidpGraphics* gfx, int x1, int y1, int x2, int y2) {
    GFX_DEBUG("drawLine: (%d,%d) -> (%d,%d), color=0x%06X", 
            x1, y1, x2, y2, gfx->rgb_color);
    x1 += gfx->translate_x;
    y1 += gfx->translate_y;
    x2 += gfx->translate_x;
    y2 += gfx->translate_y;
    
    /* Pre-compute clip bounds once for inline pixel writes */
    int clip_x1 = gfx->clip_x;
    int clip_y1 = gfx->clip_y;
    int clip_x2 = gfx->clip_x + gfx->clip_width;
    int clip_y2 = gfx->clip_y + gfx->clip_height;
    int scr_w = gfx->width;
    int scr_h = gfx->height;
    uint32_t color = (gfx->alpha << 24) | gfx->rgb_color;
    
    /* Bresenham's line algorithm */
    int dx = abs(x2 - x1);
    int dy = abs(y2 - y1);
    int sx = x1 < x2 ? 1 : -1;
    int sy = y1 < y2 ? 1 : -1;
    int err = dx - dy;
    
    /* MIDP spec: the line does not include the endpoint (x2, y2) */
    while (true) {
        if (x1 == x2 && y1 == y2) break;  /* Don't draw the endpoint */
        
        /* Inline clip check and direct framebuffer write */
        if (x1 >= clip_x1 && x1 < clip_x2 && y1 >= clip_y1 && y1 < clip_y2 &&
            x1 >= 0 && x1 < scr_w && y1 >= 0 && y1 < scr_h) {
            gfx->pixels[y1 * scr_w + x1] = color;
        }
        
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x1 += sx; }
        if (e2 < dx) { err += dx; y1 += sy; }
    }
}

void midp_graphics_fill_rect(MidpGraphics* gfx, int x, int y, int w, int h) {
    GFX_DEBUG("fillRect: (%d,%d) %dx%d, color=0x%06X, clip=(%d,%d %dx%d)", 
            x, y, w, h, gfx->rgb_color, gfx->clip_x, gfx->clip_y, gfx->clip_width, gfx->clip_height);
    x += gfx->translate_x;
    y += gfx->translate_y;
    
    /* Compute clipped rect once */
    int x1 = x > gfx->clip_x ? x : gfx->clip_x;
    int y1 = y > gfx->clip_y ? y : gfx->clip_y;
    int x2 = (x + w) < (gfx->clip_x + gfx->clip_width) ? (x + w) : (gfx->clip_x + gfx->clip_width);
    int y2 = (y + h) < (gfx->clip_y + gfx->clip_height) ? (y + h) : (gfx->clip_y + gfx->clip_height);
    
    /* Clamp to screen bounds once */
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > gfx->width) x2 = gfx->width;
    if (y2 > gfx->height) y2 = gfx->height;
    
    int row_width = x2 - x1;
    if (row_width <= 0) return;
    
    uint32_t color = (gfx->alpha << 24) | gfx->rgb_color;
    
    /* Fast path: opaque fill — tight loop, no per-pixel bounds checks */
    if (gfx->alpha == 255) {
        for (int py = y1; py < y2; py++) {
            uint32_t *row = gfx->pixels + py * gfx->width + x1;
            for (int i = 0; i < row_width; i++) {
                row[i] = color;
            }
        }
    } else {
        /* Alpha blend path with pre-computed values */
        uint8_t alpha = gfx->alpha;
        uint8_t inv_alpha = 255 - alpha;
        uint8_t sr = (gfx->rgb_color >> 16) & 0xFF;
        uint8_t sg = (gfx->rgb_color >> 8) & 0xFF;
        uint8_t sb = gfx->rgb_color & 0xFF;
        for (int py = y1; py < y2; py++) {
            uint32_t *row = gfx->pixels + py * gfx->width + x1;
            for (int i = 0; i < row_width; i++) {
                uint32_t dst = row[i];
                uint8_t r = (sr * alpha + ((dst >> 16) & 0xFF) * inv_alpha + 128) >> 8;
                uint8_t g = (sg * alpha + ((dst >> 8) & 0xFF) * inv_alpha + 128) >> 8;
                uint8_t b = (sb * alpha + (dst & 0xFF) * inv_alpha + 128) >> 8;
                row[i] = (alpha << 24) | (r << 16) | (g << 8) | b;
            }
        }
    }
}

void midp_graphics_draw_rect(MidpGraphics* gfx, int x, int y, int w, int h) {
    midp_graphics_draw_line(gfx, x, y, x + w - 1, y);
    midp_graphics_draw_line(gfx, x, y + h - 1, x + w - 1, y + h - 1);
    midp_graphics_draw_line(gfx, x, y, x, y + h - 1);
    midp_graphics_draw_line(gfx, x + w - 1, y, x + w - 1, y + h - 1);
}

void midp_graphics_draw_arc(MidpGraphics* gfx, int x, int y, int w, int h,
                            int start_angle, int arc_angle) {
    x += gfx->translate_x;
    y += gfx->translate_y;
    
    if (w < 0 || h < 0) return;
    
    double cx = x + w / 2.0;
    double cy = y + h / 2.0;
    double rx = w / 2.0;
    double ry = h / 2.0;
    
    /* MIDP uses clockwise angles with 0=right, 90=up, 180=left, 270=down
     * Mathematical convention: 0=right, 90=down (screen coords), 180=left, 270=up
     * Need to negate the angle to convert from MIDP to screen coordinates */
    double start_rad = -start_angle * M_PI / 180.0;
    double end_rad = -(start_angle + arc_angle) * M_PI / 180.0;
    
    int steps = abs(arc_angle) * 2;
    if (steps < 4) steps = 4;
    if (steps > 720) steps = 720;
    
    double step = (end_rad - start_rad) / steps;
    
    /* Pre-compute clip bounds for inline pixel writes */
    int clip_x1 = gfx->clip_x;
    int clip_y1 = gfx->clip_y;
    int clip_x2 = gfx->clip_x + gfx->clip_width;
    int clip_y2 = gfx->clip_y + gfx->clip_height;
    int scr_w = gfx->width;
    int scr_h = gfx->height;
    uint32_t color = (gfx->alpha << 24) | gfx->rgb_color;
    
    int prev_px = -1, prev_py = -1;
    for (int i = 0; i <= steps; i++) {
        double angle = start_rad + i * step;
        int px = (int)(cx + rx * cos(angle));
        int py = (int)(cy + ry * sin(angle));
        /* Only draw if position changed (avoid overdraw) */
        if (px != prev_px || py != prev_py) {
            /* Inline clip check and direct framebuffer write */
            if (px >= clip_x1 && px < clip_x2 && py >= clip_y1 && py < clip_y2 &&
                px >= 0 && px < scr_w && py >= 0 && py < scr_h) {
                gfx->pixels[py * scr_w + px] = color;
            }
            prev_px = px;
            prev_py = py;
        }
    }
}

void midp_graphics_fill_arc(MidpGraphics* gfx, int x, int y, int w, int h,
                            int start_angle, int arc_angle) {
    x += gfx->translate_x;
    y += gfx->translate_y;
    
    if (w <= 0 || h <= 0) return;
    
    double cx = x + w / 2.0;
    double cy = y + h / 2.0;
    double rx = w / 2.0;
    double ry = h / 2.0;
    
    double start_rad = -start_angle * M_PI / 180.0;
    double end_rad = -(start_angle + arc_angle) * M_PI / 180.0;
    
    /* Normalize angles to [0, 2*PI) */
    while (start_rad < 0) start_rad += 2 * M_PI;
    while (start_rad >= 2 * M_PI) start_rad -= 2 * M_PI;
    while (end_rad < 0) end_rad += 2 * M_PI;
    while (end_rad >= 2 * M_PI) end_rad -= 2 * M_PI;
    
    double arc_start, arc_end;
    
    if (arc_angle > 0) {
        arc_start = end_rad;
        arc_end = start_rad;
    } else {
        arc_start = start_rad;
        arc_end = end_rad;
    }
    
    bool full_circle = (abs(arc_angle) >= 360);
    
    /* Pre-compute clip bounds for inline pixel writes */
    int clip_x1 = gfx->clip_x;
    int clip_y1 = gfx->clip_y;
    int clip_x2 = gfx->clip_x + gfx->clip_width;
    int clip_y2 = gfx->clip_y + gfx->clip_height;
    int scr_w = gfx->width;
    int scr_h = gfx->height;
    uint32_t color = (gfx->alpha << 24) | gfx->rgb_color;
    
    /* Fast path for full circle: use row-based ellipse scan without angle check */
    if (full_circle) {
        int bx1 = x > clip_x1 ? x : clip_x1;
        int by1 = y > clip_y1 ? y : clip_y1;
        int bx2 = (x + w) < clip_x2 ? (x + w) : clip_x2;
        int by2 = (y + h) < clip_y2 ? (y + h) : clip_y2;
        if (bx1 < 0) bx1 = 0;
        if (by1 < 0) by1 = 0;
        if (bx2 > scr_w) bx2 = scr_w;
        if (by2 > scr_h) by2 = scr_h;
        
        for (int py = by1; py < by2; py++) {
            double ndy = (py - cy) / ry;
            double ndy2 = ndy * ndy;
            if (ndy2 > 1.0) continue;
            double ndx_max = sqrt(1.0 - ndy2);
            int px_start = (int)(cx - ndx_max * rx);
            int px_end = (int)(cx + ndx_max * rx);
            if (px_start < bx1) px_start = bx1;
            if (px_end >= bx2) px_end = bx2 - 1;
            uint32_t *row = gfx->pixels + py * scr_w;
            for (int px = px_start; px <= px_end; px++) {
                row[px] = color;
            }
        }
        return;
    }
    
    /* Pre-compute direction vectors for cross-product arc sector test.
     * This replaces per-pixel atan2() with just multiply+compare ops.
     * For a point (dx,dy), cross(dir_A, P) and cross(P, dir_B) determine
     * whether the point's angle falls within the arc sector. */
    double sin_start = sin(arc_start);
    double cos_start = cos(arc_start);
    double sin_end = sin(arc_end);
    double cos_end = cos(arc_end);
    /* cross_AB = sin(arc_end - arc_start), determines sector winding */
    double cross_AB = cos_start * sin_end - sin_start * cos_end;
    
    for (int py = y; py < y + h && py < scr_h; py++) {
        if (py < 0 || py < clip_y1 || py >= clip_y2) continue;
        
        for (int px = x; px < x + w && px < scr_w; px++) {
            if (px < 0 || px < clip_x1 || px >= clip_x2) continue;
            
            if (rx <= 0 || ry <= 0) continue;
            
            double dx = (px - cx) / rx;
            double dy = (py - cy) / ry;
            
            if (dx * dx + dy * dy > 1.0) continue;
            
            /* Cross-product sector test: much faster than atan2 on ARM */
            double cross_ap = cos_start * dy - sin_start * dx;
            double cross_pb = dx * sin_end - dy * cos_end;
            
            bool in_arc;
            if (cross_AB >= 0) {
                /* Non-wrapping sector: point must be CCW from A and B CCW from P */
                in_arc = (cross_ap >= 0) && (cross_pb >= 0);
            } else {
                /* Wrapping sector: point must be on either side */
                in_arc = (cross_ap >= 0) || (cross_pb >= 0);
            }
            
            if (in_arc) {
                gfx->pixels[py * scr_w + px] = color;
            }
        }
    }
}

void midp_graphics_draw_round_rect(MidpGraphics* gfx, int x, int y, int w, int h,
                                   int arc_w, int arc_h) {
    /* Draw lines (drawLine excludes endpoint, so pass arc boundary as endpoint
     * to draw pixels up to one pixel before the arc starts) */
    midp_graphics_draw_line(gfx, x + arc_w/2, y, x + w - arc_w/2, y);
    midp_graphics_draw_line(gfx, x + arc_w/2, y + h - 1, x + w - arc_w/2, y + h - 1);
    midp_graphics_draw_line(gfx, x, y + arc_h/2, x, y + h - arc_h/2);
    midp_graphics_draw_line(gfx, x + w - 1, y + arc_h/2, x + w - 1, y + h - arc_h/2);
    
    /* Draw arcs */
    midp_graphics_draw_arc(gfx, x, y, arc_w, arc_h, 90, 90);
    midp_graphics_draw_arc(gfx, x + w - arc_w, y, arc_w, arc_h, 0, 90);
    midp_graphics_draw_arc(gfx, x + w - arc_w, y + h - arc_h, arc_w, arc_h, 270, 90);
    midp_graphics_draw_arc(gfx, x, y + h - arc_h, arc_w, arc_h, 180, 90);
}

void midp_graphics_fill_round_rect(MidpGraphics* gfx, int x, int y, int w, int h,
                                   int arc_w, int arc_h) {
    /* Fill center */
    midp_graphics_fill_rect(gfx, x + arc_w/2, y, w - arc_w, h);
    midp_graphics_fill_rect(gfx, x, y + arc_h/2, arc_w/2, h - arc_h);
    midp_graphics_fill_rect(gfx, x + w - arc_w/2, y + arc_h/2, arc_w/2, h - arc_h);
    
    /* Fill corners */
    midp_graphics_fill_arc(gfx, x, y, arc_w, arc_h, 90, 90);
    midp_graphics_fill_arc(gfx, x + w - arc_w, y, arc_w, arc_h, 0, 90);
    midp_graphics_fill_arc(gfx, x + w - arc_w, y + h - arc_h, arc_w, arc_h, 270, 90);
    midp_graphics_fill_arc(gfx, x, y + h - arc_h, arc_w, arc_h, 180, 90);
}

void midp_graphics_draw_string(MidpGraphics* gfx, const char* str,
                               int x, int y, int anchor) {
    GFX_DEBUG("drawString: str='%s', x=%d, y=%d, anchor=%d, color=0x%06X",
            str ? str : "(null)", x, y, anchor, gfx ? gfx->rgb_color : 0);
    if (!str || !gfx) return;
    
#ifdef J2ME_HEADLESS
    /* Capture text for headless console output */
    headless_log_text(str, x, y, gfx->rgb_color);
#endif
    
    int byte_len = strlen(str);
    if (byte_len == 0) return;
    
    /* Count characters (not bytes) using UTF-8 decoding */
    int char_count = utf8_strlen(str);
    if (char_count == 0) return;
    
    /* ИСПРАВЛЕНО: Properly truncate both char_count and calculate max byte_len */
    int max_chars = 4096;
    if (char_count > max_chars) {
        char_count = max_chars;
        /* Find the byte offset for the truncated character count */
        int chars_seen = 0;
        int safe_byte_len = 0;
        while (chars_seen < max_chars && safe_byte_len < byte_len) {
            int codepoint = utf8_decode(str, &safe_byte_len, byte_len);
            if (codepoint < 0) break;
            chars_seen++;
        }
        byte_len = safe_byte_len;
    }
    
    /* Calculate text dimensions based on character count */
    int text_width = char_count * (FONT_WIDTH + 1) - 1;  /* +1 for spacing between chars */
    int text_height = FONT_HEIGHT;
    
    /* Apply translation */
    int draw_x = x + gfx->translate_x;
    int draw_y = y + gfx->translate_y;
    
    /* MIDP2 anchor constants:
     * HCENTER = 1, VCENTER = 2, LEFT = 4, RIGHT = 8, TOP = 16, BOTTOM = 32, BASELINE = 64
     */
    
    /* Apply horizontal anchor */
    if (anchor & 0x01) {        /* HCENTER = 1 */
        draw_x -= text_width / 2;
    } else if (anchor & 0x08) { /* RIGHT = 8 */
        draw_x -= text_width;
    }
    /* LEFT = 4 is default (no adjustment needed) */
    
    /* Apply vertical anchor */
    if (anchor & 0x02) {        /* VCENTER = 2 */
        draw_y -= text_height / 2;
    } else if (anchor & 0x40) { /* BASELINE = 64 */
        draw_y -= FONT_HEIGHT - 2;  /* Baseline is near bottom */
    } else if (anchor & 0x20) { /* BOTTOM = 32 */
        draw_y -= text_height;
    }
    /* TOP = 16 is default (no adjustment needed) */
    
    /* Debug output for anchor calculation */
    GFX_DEBUG("drawString: char_count=%d, text_width=%d, draw_x=%d, draw_y=%d (anchor: HCENTER=%d RIGHT=%d LEFT=%d)",
            char_count, text_width, draw_x, draw_y, 
            (anchor & 0x01) ? 1 : 0, 
            (anchor & 0x08) ? 1 : 0,
            (anchor & 0x04) ? 1 : 0);
    
    /* Draw each character, decoding UTF-8 */
    /* Use a separate counter to limit drawn characters (prevents buffer issues) */
    int cur_x = draw_x;
    int i = 0;
    int chars_drawn = 0;
    while (i < byte_len && chars_drawn < char_count) {
        int codepoint = utf8_decode(str, &i, byte_len);
        if (codepoint >= 0) {
            midp_graphics_draw_char_unicode(gfx, codepoint, cur_x, draw_y);
            cur_x += FONT_WIDTH + 1;  /* +1 for spacing between characters */
            chars_drawn++;
        }
    }
}

void midp_graphics_draw_image(MidpGraphics* gfx, MidpImage* img,
                              int x, int y, int anchor) {
    x += gfx->translate_x;
    y += gfx->translate_y;
    
    if (!img || !img->pixels) {
        return;
    }
    
    /* MIDP2 anchor constants:
     * HCENTER = 1, VCENTER = 2, LEFT = 4, RIGHT = 8, TOP = 16, BOTTOM = 32, BASELINE = 64
     */
    
    /* Apply horizontal anchor */
    if (anchor & 0x01) {        /* HCENTER */
        x -= img->width / 2;
    } else if (anchor & 0x08) { /* RIGHT */
        x -= img->width;
    }
    /* LEFT = 4 is default */
    
    /* Apply vertical anchor */
    if (anchor & 0x02) {        /* VCENTER */
        y -= img->height / 2;
    } else if (anchor & 0x20) { /* BOTTOM */
        y -= img->height;
    }
    /* TOP = 16 is default */
    
    /* Compute clipped destination region once */
    int dst_x1 = x > gfx->clip_x ? x : gfx->clip_x;
    int dst_y1 = y > gfx->clip_y ? y : gfx->clip_y;
    int dst_x2 = (x + img->width) < (gfx->clip_x + gfx->clip_width) ? 
                  (x + img->width) : (gfx->clip_x + gfx->clip_width);
    int dst_y2 = (y + img->height) < (gfx->clip_y + gfx->clip_height) ?
                 (y + img->height) : (gfx->clip_y + gfx->clip_height);
    
    /* Clamp to screen bounds */
    if (dst_x1 < 0) dst_x1 = 0;
    if (dst_y1 < 0) dst_y1 = 0;
    if (dst_x2 > gfx->width) dst_x2 = gfx->width;
    if (dst_y2 > gfx->height) dst_y2 = gfx->height;
    
    int copy_w = dst_x2 - dst_x1;
    int copy_h = dst_y2 - dst_y1;
    if (copy_w <= 0 || copy_h <= 0) return;
    
    /* Compute source region offset */
    int src_x_off = dst_x1 - x;
    int src_y_off = dst_y1 - y;
    
    /* Check if image is likely fully opaque by sampling corner pixels */
    bool likely_opaque = !img->alpha;
    if (!likely_opaque) {
        uint32_t p1 = img->pixels[src_y_off * img->width + src_x_off];
        if (((p1 >> 24) & 0xFF) == 0xFF) {
            uint32_t p2 = img->pixels[(src_y_off + copy_h - 1) * img->width + (src_x_off + copy_w - 1)];
            if (((p2 >> 24) & 0xFF) == 0xFF) {
                uint32_t p3 = img->pixels[src_y_off * img->width + (src_x_off + copy_w - 1)];
                uint32_t p4 = img->pixels[(src_y_off + copy_h - 1) * img->width + src_x_off];
                if (((p3 >> 24) & 0xFF) == 0xFF && ((p4 >> 24) & 0xFF) == 0xFF) {
                    likely_opaque = true;
                }
            }
        }
    }
    
    if (likely_opaque) {
        /* Fast path: memcpy per row for opaque images */
        for (int py = 0; py < copy_h; py++) {
            uint32_t *src_row = img->pixels + (src_y_off + py) * img->width + src_x_off;
            uint32_t *dst_row = gfx->pixels + (dst_y1 + py) * gfx->width + dst_x1;
            memcpy(dst_row, src_row, copy_w * sizeof(uint32_t));
        }
    } else {
        /* Alpha blending path with shift instead of division (faster on ARM) */
        for (int py = 0; py < copy_h; py++) {
            uint32_t *src_row = img->pixels + (src_y_off + py) * img->width + src_x_off;
            uint32_t *dst_row = gfx->pixels + (dst_y1 + py) * gfx->width + dst_x1;
            for (int px = 0; px < copy_w; px++) {
                uint32_t src_color = src_row[px];
                uint8_t alpha = (src_color >> 24) & 0xFF;
                if (alpha == 255) {
                    dst_row[px] = src_color;
                } else if (alpha > 0) {
                    uint32_t dst_color = dst_row[px];
                    uint8_t inv_alpha = 255 - alpha;
                    uint8_t r = (((src_color >> 16) & 0xFF) * alpha + 
                                 ((dst_color >> 16) & 0xFF) * inv_alpha + 128) >> 8;
                    uint8_t g = (((src_color >> 8) & 0xFF) * alpha + 
                                 ((dst_color >> 8) & 0xFF) * inv_alpha + 128) >> 8;
                    uint8_t b = ((src_color & 0xFF) * alpha + 
                                 (dst_color & 0xFF) * inv_alpha + 128) >> 8;
                    dst_row[px] = (0xFF << 24) | (r << 16) | (g << 8) | b;
                }
            }
        }
    }
}

/* Draw a region of an image with optional transform
 * MIDP2 Transform constants:
 * TRANS_NONE = 0, TRANS_MIRROR_ROT180 = 1, TRANS_MIRROR = 2, TRANS_ROT180 = 3
 * TRANS_MIRROR_ROT270 = 4, TRANS_ROT90 = 5, TRANS_ROT270 = 6, TRANS_MIRROR_ROT90 = 7
 */
void midp_graphics_draw_region(MidpGraphics* gfx, MidpImage* img,
                               int x_src, int y_src, int w, int h,
                               int transform, int x_dest, int y_dest, int anchor) {
    x_dest += gfx->translate_x;
    y_dest += gfx->translate_y;
    
    if (!img || !img->pixels) return;
    if (w <= 0 || h <= 0) return;
    if (x_src < 0 || y_src < 0 || x_src + w > img->width || y_src + h > img->height) return;
    
    /* Calculate output dimensions based on transform */
    int out_w = w;
    int out_h = h;
    
    /* Rotations 90 and 270 swap width and height */
    if (transform == 5 || transform == 6 || transform == 4 || transform == 7) {
        out_w = h;
        out_h = w;
    }
    
    /* Apply anchor */
    if (anchor & 0x01) {        /* HCENTER */
        x_dest -= out_w / 2;
    } else if (anchor & 0x08) { /* RIGHT */
        x_dest -= out_w;
    }
    
    if (anchor & 0x02) {        /* VCENTER */
        y_dest -= out_h / 2;
    } else if (anchor & 0x20) { /* BOTTOM */
        y_dest -= out_h;
    }
    
    /* Fast path for TRANS_NONE: pre-compute clipped region, use memcpy for opaque */
    if (transform == 0) {
        int dx1 = x_dest > gfx->clip_x ? x_dest : gfx->clip_x;
        int dy1 = y_dest > gfx->clip_y ? y_dest : gfx->clip_y;
        int dx2 = (x_dest + w) < (gfx->clip_x + gfx->clip_width) ?
                  (x_dest + w) : (gfx->clip_x + gfx->clip_width);
        int dy2 = (y_dest + h) < (gfx->clip_y + gfx->clip_height) ?
                  (y_dest + h) : (gfx->clip_y + gfx->clip_height);
        if (dx1 < 0) dx1 = 0;
        if (dy1 < 0) dy1 = 0;
        if (dx2 > gfx->width) dx2 = gfx->width;
        if (dy2 > gfx->height) dy2 = gfx->height;
        
        int cw = dx2 - dx1;
        int ch = dy2 - dy1;
        if (cw <= 0 || ch <= 0) return;
        
        int sx_off = dx1 - x_dest;
        int sy_off = dy1 - y_dest;
        
        /* Check opacity by sampling pixels */
        bool opaque = !img->alpha;
        if (!opaque) {
            uint32_t p = img->pixels[(y_src + sy_off) * img->width + (x_src + sx_off)];
            if (((p >> 24) & 0xFF) == 0xFF) {
                p = img->pixels[(y_src + sy_off + ch - 1) * img->width + (x_src + sx_off + cw - 1)];
                if (((p >> 24) & 0xFF) == 0xFF) opaque = true;
            }
        }
        
        if (opaque) {
            for (int py = 0; py < ch; py++) {
                memcpy(gfx->pixels + (dy1 + py) * gfx->width + dx1,
                       img->pixels + (y_src + sy_off + py) * img->width + (x_src + sx_off),
                       cw * sizeof(uint32_t));
            }
        } else {
            for (int py = 0; py < ch; py++) {
                uint32_t *src_row = img->pixels + (y_src + sy_off + py) * img->width + (x_src + sx_off);
                uint32_t *dst_row = gfx->pixels + (dy1 + py) * gfx->width + dx1;
                for (int px = 0; px < cw; px++) {
                    uint32_t sc = src_row[px];
                    uint8_t a = (sc >> 24) & 0xFF;
                    if (a == 255) {
                        dst_row[px] = sc;
                    } else if (a > 0) {
                        uint32_t dc = dst_row[px];
                        uint8_t ia = 255 - a;
                        uint8_t r = (((sc >> 16) & 0xFF) * a + ((dc >> 16) & 0xFF) * ia + 128) >> 8;
                        uint8_t g = (((sc >> 8) & 0xFF) * a + ((dc >> 8) & 0xFF) * ia + 128) >> 8;
                        uint8_t b = ((sc & 0xFF) * a + (dc & 0xFF) * ia + 128) >> 8;
                        dst_row[px] = (0xFF << 24) | (r << 16) | (g << 8) | b;
                    }
                }
            }
        }
        return;
    }
    
    /* General transform path with pre-computed clip bounds and optimized alpha blend */
    int clip_x1 = gfx->clip_x;
    int clip_y1 = gfx->clip_y;
    int clip_x2 = gfx->clip_x + gfx->clip_width;
    int clip_y2 = gfx->clip_y + gfx->clip_height;
    int scr_w = gfx->width;
    int scr_h = gfx->height;
    
    for (int py = 0; py < h; py++) {
        for (int px = 0; px < w; px++) {
            int src_x = x_src + px;
            int src_y = y_src + py;
            
            uint32_t src_color = img->pixels[src_y * img->width + src_x];
            uint8_t alpha = (src_color >> 24) & 0xFF;
            if (alpha == 0) continue;
            
            /* Calculate destination coordinates based on transform */
            int dst_px, dst_py;
            
            switch (transform) {
                /* MIDP2 Sprite Transform constants:
                 * TRANS_NONE = 0: no transform
                 * TRANS_MIRROR_ROT180 = 1: reflect horizontally (X-axis mirror), same as 180° rotation then mirror
                 * TRANS_MIRROR = 2: reflect vertically (Y-axis mirror)  
                 * TRANS_ROT180 = 3: rotate 180° clockwise
                 * TRANS_MIRROR_ROT270 = 4: reflect horizontally then rotate 90° counter-clockwise
                 * TRANS_ROT90 = 5: rotate 90° clockwise
                 * TRANS_ROT270 = 6: rotate 270° clockwise (90° counter-clockwise)
                 * TRANS_MIRROR_ROT90 = 7: reflect horizontally then rotate 90° clockwise
                 */
                case 1: /* TRANS_MIRROR_ROT180 - vertical flip (Y-axis mirror) */
                    dst_px = px; dst_py = h - 1 - py; break;
                case 2: /* TRANS_MIRROR - horizontal flip (X-axis mirror) */
                    dst_px = w - 1 - px; dst_py = py; break;
                case 3: /* TRANS_ROT180 */
                    dst_px = w - 1 - px; dst_py = h - 1 - py; break;
                case 4: /* TRANS_MIRROR_ROT270 - reflect X, then rotate 90° CCW = transpose */
                    dst_px = py; dst_py = px; break;
                case 5: /* TRANS_ROT90 - rotate 90° clockwise */
                    dst_px = h - 1 - py; dst_py = px; break;
                case 6: /* TRANS_ROT270 - rotate 270° clockwise (90° CCW) */
                    dst_px = py; dst_py = w - 1 - px; break;
                case 7: /* TRANS_MIRROR_ROT90 - reflect X, then rotate 90° CW */
                    dst_px = h - 1 - py; dst_py = w - 1 - px; break;
                default: 
                    dst_px = px; dst_py = py; break;
            }
            
            int dst_x = x_dest + dst_px;
            int dst_y = y_dest + dst_py;
            
            /* Clip check */
            if (dst_x < clip_x1 || dst_x >= clip_x2 || dst_y < clip_y1 || dst_y >= clip_y2) continue;
            if (dst_x < 0 || dst_x >= scr_w || dst_y < 0 || dst_y >= scr_h) continue;
            
            /* Alpha blending with shift instead of division */
            if (alpha == 255) {
                gfx->pixels[dst_y * scr_w + dst_x] = src_color;
            } else {
                uint32_t dst_color = gfx->pixels[dst_y * scr_w + dst_x];
                uint8_t inv_alpha = 255 - alpha;
                uint8_t r = (((src_color >> 16) & 0xFF) * alpha + 
                             ((dst_color >> 16) & 0xFF) * inv_alpha + 128) >> 8;
                uint8_t g = (((src_color >> 8) & 0xFF) * alpha +
                             ((dst_color >> 8) & 0xFF) * inv_alpha + 128) >> 8;
                uint8_t b = (((src_color & 0xFF) * alpha +
                             (dst_color & 0xFF) * inv_alpha + 128)) >> 8;
                gfx->pixels[dst_y * scr_w + dst_x] = (0xFF << 24) | (r << 16) | (g << 8) | b;
            }
        }
    }
}

/* Copy a region within the graphics context */
void midp_graphics_copy_area(MidpGraphics* gfx, int x_src, int y_src,
                             int w, int h, int x_dest, int y_dest, int anchor) {
    x_src += gfx->translate_x;
    y_src += gfx->translate_y;
    x_dest += gfx->translate_x;
    y_dest += gfx->translate_y;
    
    if (w <= 0 || h <= 0) return;
    
    /* Apply anchor for destination */
    if (anchor & 0x01) {        /* HCENTER */
        x_dest -= w / 2;
    } else if (anchor & 0x08) { /* RIGHT */
        x_dest -= w;
    }
    
    if (anchor & 0x02) {        /* VCENTER */
        y_dest -= h / 2;
    } else if (anchor & 0x20) { /* BOTTOM */
        y_dest -= h;
    }
    
    /* Compute valid source region clamped to screen bounds */
    int sx1 = x_src > 0 ? x_src : 0;
    int sy1 = y_src > 0 ? y_src : 0;
    int sx2 = (x_src + w) < gfx->width ? (x_src + w) : gfx->width;
    int sy2 = (y_src + h) < gfx->height ? (y_src + h) : gfx->height;
    int cw = sx2 - sx1;
    int ch = sy2 - sy1;
    if (cw <= 0 || ch <= 0) return;
    
    /* Corresponding destination coords */
    int dx1 = x_dest + (sx1 - x_src);
    int dy1 = y_dest + (sy1 - y_src);
    
    /* Clip destination to clip region and screen */
    int clip_x2 = gfx->clip_x + gfx->clip_width;
    int clip_y2 = gfx->clip_y + gfx->clip_height;
    int adj;
    adj = gfx->clip_x - dx1; if (adj > 0) { dx1 += adj; sx1 += adj; cw -= adj; }
    adj = gfx->clip_y - dy1; if (adj > 0) { dy1 += adj; sy1 += adj; ch -= adj; }
    if (dx1 + cw > clip_x2) cw = clip_x2 - dx1;
    if (dy1 + ch > clip_y2) ch = clip_y2 - dy1;
    if (dx1 + cw > gfx->width) cw = gfx->width - dx1;
    if (dy1 + ch > gfx->height) ch = gfx->height - dy1;
    if (cw <= 0 || ch <= 0) return;
    
    /* Use temp buffer for safe copy (handles overlapping regions), memcpy per row */
    size_t row_bytes = (size_t)cw * sizeof(uint32_t);
    uint32_t* temp = (uint32_t*)malloc(ch * row_bytes);
    if (!temp) return;
    
    for (int py = 0; py < ch; py++) {
        memcpy(temp + (size_t)py * cw,
               gfx->pixels + (sy1 + py) * gfx->width + sx1,
               row_bytes);
    }
    for (int py = 0; py < ch; py++) {
        memcpy(gfx->pixels + (dy1 + py) * gfx->width + dx1,
               temp + (size_t)py * cw,
               row_bytes);
    }
    
    free(temp);
}

void midp_graphics_get_rgb(MidpGraphics* gfx, jint* rgb_data,
                           int offset, int scanlength, int x, int y, int w, int h) {
    x += gfx->translate_x;
    y += gfx->translate_y;
    
    for (int py = 0; py < h; py++) {
        int src_y = y + py;
        if (src_y < 0 || src_y >= gfx->height) continue;
        
        for (int px = 0; px < w; px++) {
            int src_x = x + px;
            if (src_x < 0 || src_x >= gfx->width) continue;
            
            rgb_data[offset + py * scanlength + px] = 
                (jint)gfx->pixels[src_y * gfx->width + src_x];
        }
    }
}

/*
 * Image operations
 */

/* Maximum image dimension to prevent OOM from malformed data */
#define MIDP_MAX_IMAGE_DIMENSION 2048
#define MIDP_MAX_IMAGE_PIXELS (MIDP_MAX_IMAGE_DIMENSION * MIDP_MAX_IMAGE_DIMENSION)

MidpImage* midp_image_create(int width, int height, bool mutable) {
    /* Validate dimensions to prevent OOM from malformed images */
    if (width <= 0 || height <= 0) {
        GFX_DEBUG("midp_image_create: Invalid dimensions: %dx%d", width, height);
        return NULL;
    }
    
    if (width > MIDP_MAX_IMAGE_DIMENSION || height > MIDP_MAX_IMAGE_DIMENSION) {
        GFX_DEBUG("midp_image_create: REJECTED - dimensions too large: %dx%d (max %d)", 
                width, height, MIDP_MAX_IMAGE_DIMENSION);
        return NULL;
    }
    
    /* Check for integer overflow */
    size_t pixel_count = (size_t)width * (size_t)height;
    if (pixel_count > MIDP_MAX_IMAGE_PIXELS) {
        GFX_DEBUG("midp_image_create: REJECTED - pixel count overflow: %zu", pixel_count);
        return NULL;
    }
    
    MidpImage* img = (MidpImage*)malloc(sizeof(MidpImage));
    if (!img) return NULL;
    
    img->pixels = (uint32_t*)calloc(pixel_count, sizeof(uint32_t));
    if (!img->pixels) {
        free(img);
        return NULL;
    }
    
    img->width = width;
    img->height = height;
    img->mutable = mutable;
    img->alpha = true;
    
    /* MIDP spec: mutable images are initially filled with white pixels */
    if (mutable) {
        memset(img->pixels, 0xFF, pixel_count * sizeof(uint32_t));
    }
    
    return img;
}

MidpImage* midp_image_create_from_rgb(const jint* rgb, int width, int height,
                                       bool process_alpha) {
    /* createRGBImage creates immutable images per MIDP spec */
    MidpImage* img = midp_image_create(width, height, false);
    if (!img) return NULL;
    
    for (int i = 0; i < width * height; i++) {
        if (process_alpha) {
            img->pixels[i] = (uint32_t)rgb[i];
        } else {
            img->pixels[i] = 0xFF000000 | ((uint32_t)rgb[i] & 0xFFFFFF);
        }
    }
    
    img->alpha = process_alpha;
    return img;
}

void midp_image_destroy(MidpImage* img) {
    if (img) {
        free(img->pixels);
        free(img);
    }
}

void midp_image_get_rgb(MidpImage* img, jint* rgb, int offset, int scanlength,
                        int x, int y, int width, int height) {
    if (!img || !rgb) return;
    
    for (int py = 0; py < height; py++) {
        int src_y = y + py;
        if (src_y < 0 || src_y >= img->height) continue;
        
        for (int px = 0; px < width; px++) {
            int src_x = x + px;
            if (src_x < 0 || src_x >= img->width) continue;
            
            rgb[offset + py * scanlength + px] = 
                (jint)img->pixels[src_y * img->width + src_x];
        }
    }
}

MidpGraphics* midp_image_get_graphics(MidpImage* img) {
    if (!img || !img->mutable) {
        return NULL;
    }
    
    MidpGraphics* gfx = (MidpGraphics*)malloc(sizeof(MidpGraphics));
    if (!gfx) return NULL;
    
    midp_graphics_init(gfx, img->pixels, img->width, img->height);
    
    return gfx;
}

/* ============================================
 * Image decoding via stb_image
 * Supports: PNG, JPEG, BMP
 * ============================================ */

/* Check if data is a PNG */
static bool is_png(const uint8_t* data, int len) {
    static const uint8_t png_sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    return len >= 8 && memcmp(data, png_sig, 8) == 0;
}

/* Check if data is a JPEG */
static bool is_jpeg(const uint8_t* data, int len) {
    return len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
}

/* Check if data is a BMP */
static bool is_bmp(const uint8_t* data, int len) {
    return len >= 2 && data[0] == 'B' && data[1] == 'M';
}

/* Decode image data to RGBA pixels using stb_image */
MidpImage* midp_image_create_from_data(const uint8_t* data, int offset, int length) {
    if (!data || length < 8) return NULL;
    
    const uint8_t* img_data = data + offset;
    int img_len = length - offset;
    
    /* Check image format for logging */
    const char* format = "unknown";
    if (is_png(img_data, img_len)) format = "PNG";
    else if (is_jpeg(img_data, img_len)) format = "JPEG";
    else if (is_bmp(img_data, img_len)) format = "BMP";
    
    /* Decode using stb_image */
    int width, height, channels;
    uint8_t* pixels = stbi_load_from_memory(img_data, img_len, &width, &height, &channels, 4);
    
    if (!pixels) {
        GFX_DEBUG("Failed to decode %s: %s", format, stbi_failure_reason());
        return NULL;
    }
    
    GFX_DEBUG("Decoded %s: %dx%d (%d channels)", format, width, height, channels);
    
    /* Create MIDP image (resource-loaded images are immutable per MIDP spec) */
    MidpImage* img = midp_image_create(width, height, false);
    if (!img) {
        stbi_image_free(pixels);
        return NULL;
    }
    
    /* Convert from stb_image format (RGBA bytes) to our format (ARGB uint32_t)
     * Optimized: pointer arithmetic, process 4 pixels at a time */
    {
        const uint8_t *src = pixels;
        uint32_t *dst = img->pixels;
        int total = width * height;
        int i = 0;
        
        /* Process 4 pixels at a time for better instruction pipelining */
        for (; i + 3 < total; i += 4) {
            dst[i]     = ((uint32_t)src[3] << 24) | ((uint32_t)src[0] << 16) | ((uint32_t)src[1] << 8) | src[2];
            dst[i + 1] = ((uint32_t)src[7] << 24) | ((uint32_t)src[4] << 16) | ((uint32_t)src[5] << 8) | src[6];
            dst[i + 2] = ((uint32_t)src[11] << 24) | ((uint32_t)src[8] << 16) | ((uint32_t)src[9] << 8) | src[10];
            dst[i + 3] = ((uint32_t)src[15] << 24) | ((uint32_t)src[12] << 16) | ((uint32_t)src[13] << 8) | src[14];
            src += 16;
        }
        /* Handle remaining pixels */
        for (; i < total; i++) {
            dst[i] = ((uint32_t)src[3] << 24) | ((uint32_t)src[0] << 16) | ((uint32_t)src[1] << 8) | src[2];
            src += 4;
        }
    }
    
    img->alpha = (channels == 4);
    
    stbi_image_free(pixels);
    return img;
}

/*
 * Font operations
 */

MidpFont* midp_font_get_default(void) {
    static MidpFont default_font = {
        .face = FONT_FACE_SYSTEM,
        .style = FONT_STYLE_PLAIN,
        .size = FONT_SIZE_MEDIUM,
        .height = FONT_HEIGHT,
        .baseline = FONT_HEIGHT - 2,
        .native_font = NULL
    };
    return &default_font;
}

MidpFont* midp_font_get(int face, int style, int size) {
    MidpFont* font = (MidpFont*)malloc(sizeof(MidpFont));
    if (!font) return NULL;
    
    font->face = face;
    font->style = style;
    font->size = size;
    /* Use bitmap font dimensions */
    font->height = FONT_HEIGHT;
    font->baseline = FONT_HEIGHT - 2;
    font->native_font = NULL;
    
    return font;
}

int midp_font_string_width(MidpFont* font, const char* str) {
    if (!font || !str) return 0;
    /* Each character is FONT_WIDTH pixels + 1 pixel spacing */
    /* Count UTF-8 characters, not bytes */
    int char_count = utf8_strlen(str);
    /* Prevent integer overflow for very long strings */
    if (char_count > 4096) char_count = 4096;
    return char_count > 0 ? char_count * (FONT_WIDTH + 1) - 1 : 0;
}

int midp_font_char_width(MidpFont* font, jchar ch) {
    (void)ch;
    if (!font) return 0;
    return FONT_WIDTH;
}

int midp_font_height(MidpFont* font) {
    return font ? font->height : FONT_HEIGHT;
}

int midp_font_baseline_position(MidpFont* font) {
    return font ? font->baseline : FONT_HEIGHT - 2;
}

/*
 * Display operations
 */

void midp_display_get_dimensions(int* width, int* height) {
    if (width) *width = g_midp_screen_width;
    if (height) *height = g_midp_screen_height;
}

bool midp_display_is_color(void) {
    return true;
}

int midp_display_num_colors(void) {
    return 16777216;  /* 24-bit color, matches FreeJ2ME */
}

int midp_display_num_alpha_levels(void) {
    return 256;
}
