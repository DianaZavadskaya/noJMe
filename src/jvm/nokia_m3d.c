/*
 * Nokia M3D Software 3D Renderer - Native Implementation
 *
 * Implements com.nokia.mid.m3d.M3D for the nojme J2ME emulator.
 * Ported from the FreeJ2ME reference implementation.
 *
 * M3D is Nokia's proprietary immediate-mode 3D API available on
 * S40/S60 devices. It provides basic OpenGL-ES-like fixed-function
 * rendering for simple 3D graphics on mobile devices.
 *
 * Optimized for ARMv7 (no GPU): float instead of double, VFP-friendly
 * code, tight inner loops, fast float-to-int, inlined hot paths.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "native.h"
#include "jvm.h"
#include "heap.h"
#include "midp.h"

/* ========================================================================= */
/* ARM compiler hints                                                         */
/* ========================================================================= */

#if defined(__ARM_ARCH_7A__) || defined(__arm__)
#define M3D_INLINE __attribute__((always_inline)) static inline
#else
#define M3D_INLINE static inline
#endif

/** Fast float-to-int conversion: on ARMv7, avoids slow libgcc helper */
M3D_INLINE int m3d_float_to_int(float f) {
#if defined(__ARM_ARCH_7A__) || defined(__arm__)
    int result;
    __asm__ volatile("vcvt.s32.f32 %0, %1" : "=t"(result) : "t"(f));
    return result;
#else
    return (int)f;
#endif
}

/* ========================================================================= */
/* Constants                                                                  */
/* ========================================================================= */

#define MAX_M3D_INSTANCES  16
#define DEG2RAD            0.017453292519943295f  /* pi/180 */

/* Anchor constants (from javax.microedition.lcdui.Graphics) */
#define ANCHOR_TOP_LEFT    0x04  /* TOP | LEFT */

/* Maximum elements for face arrays (128 faces * 3 verts = 384) */
#define MAX_VERTS          256
#define MAX_UVS            256
#define MAX_FACES          384
#define MAX_FACE_UVS       384

/* ========================================================================= */
/* Per-instance state                                                         */
/* ========================================================================= */

typedef struct {
    /* Transformation matrices (column-major, OpenGL style) */
    float matrix[16];
    float stack[16];
    float projm[16];

    /* Vertex data */
    float verts[MAX_VERTS];
    float UVs[MAX_UVS];
    int vertCount;

    /* Projected face data for current draw call */
    float faces[MAX_FACES];
    float faceUVs[MAX_FACE_UVS];
    int faceCount;

    /* Framebuffer */
    int width, height;
    float near, far;
    float* zbuffer;
    MidpImage* framebuffer;
    MidpGraphics* gc;

    /* Drawing state */
    unsigned int color;      /* ARGB packed color for drawing */
    unsigned int clearcolor; /* ARGB packed color for clearing */
    int boundTexture;        /* whether a texture is bound */
    int active;              /* whether this slot is in use */
} M3DState;

/* Instance pool */
static M3DState   m3d_states[MAX_M3D_INSTANCES];
static JavaObject* m3d_objects[MAX_M3D_INSTANCES];

/* ========================================================================= */
/* Instance lookup / allocation                                               */
/* ========================================================================= */

static M3DState* m3d_get_state(JavaObject* obj) {
    for (int i = 0; i < MAX_M3D_INSTANCES; i++) {
        if (m3d_states[i].active && m3d_objects[i] == obj) {
            return &m3d_states[i];
        }
    }
    return NULL;
}

static M3DState* m3d_alloc_state(JavaObject* obj) {
    /* Try to find an existing slot for this object first */
    for (int i = 0; i < MAX_M3D_INSTANCES; i++) {
        if (m3d_states[i].active && m3d_objects[i] == obj) {
            return &m3d_states[i];
        }
    }
    /* Allocate a new slot */
    for (int i = 0; i < MAX_M3D_INSTANCES; i++) {
        if (!m3d_states[i].active) {
            memset(&m3d_states[i], 0, sizeof(M3DState));
            m3d_objects[i] = obj;
            m3d_states[i].active = 1;
            m3d_states[i].color = 0xFF000000;       /* opaque black */
            m3d_states[i].clearcolor = 0xFFFFFFFF;  /* opaque white */
            return &m3d_states[i];
        }
    }
    return NULL;
}

static void m3d_free_state(M3DState* state) {
    if (!state) return;
    if (state->zbuffer) {
        free(state->zbuffer);
        state->zbuffer = NULL;
    }
    /* Note: framebuffer is freed by the MIDP image subsystem */
    state->framebuffer = NULL;
    state->gc = NULL;
    state->width = 0;
    state->height = 0;
    state->active = 0;
}

/* ========================================================================= */
/* Matrix math helpers                                                        */
/* ========================================================================= */

/** Set a 4x4 matrix to identity (column-major) */
M3D_INLINE void m3d_identity(float m[16]) {
    memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/** Copy src matrix to dst */
M3D_INLINE void m3d_clone(float dst[16], const float src[16]) {
    memcpy(dst, src, sizeof(float) * 16);
}

/** Multiply a = a * b (column-major) */
M3D_INLINE void m3d_matmul(float a[16], const float b[16]) {
    float result[16];
    for (int j = 0; j < 4; j++) {
        for (int i = 0; i < 4; i++) {
            result[j * 4 + i] = a[j * 4 + 0] * b[0 * 4 + i]
                               + a[j * 4 + 1] * b[1 * 4 + i]
                               + a[j * 4 + 2] * b[2 * 4 + i]
                               + a[j * 4 + 3] * b[3 * 4 + i];
        }
    }
    memcpy(a, result, sizeof(float) * 16);
}

/** Set up perspective projection matrix */
M3D_INLINE void m3d_projection(float m[16], float w, float h, float n, float f) {
    memset(m, 0, sizeof(float) * 16);
    float fn = 2.0f * n;
    m[0]  = fn / w;
    m[5]  = fn / h;
    m[10] = -(f + n) / (f - n);
    m[11] = -1.0f;
    m[14] = -(2.0f * f * n) / (f - n);
    /* m[15] = 0 (already zeroed) */
}

/** Transform verts array in-place by the given 4x4 model matrix */
M3D_INLINE void m3d_apply_matrix(M3DState* state, const float m[16]) {
    int count = state->vertCount;
    for (int i = 0; i < count; i += 3) {
        float x = state->verts[i], y = state->verts[i + 1], z = state->verts[i + 2];
        state->verts[i]     = x * m[0] + y * m[4] + z * m[8]  + m[12];
        state->verts[i + 1] = x * m[1] + y * m[5] + z * m[9]  + m[13];
        state->verts[i + 2] = x * m[2] + y * m[6] + z * m[10] + m[14];
    }
}

/** Add 3 transformed vertices + UVs to the face list */
M3D_INLINE void m3d_add_faces(M3DState* state,
                              float x1, float y1, float z1,
                              float x2, float y2, float z2,
                              float x3, float y3, float z3,
                              float u1, float v1, float u2, float v2,
                              float u3, float v3) {
    int fc = state->faceCount;
    if (fc + 3 > MAX_FACES / 3) return; /* overflow guard */

    /* Vertex 1 */
    state->faces[fc * 3]     = x1;
    state->faces[fc * 3 + 1] = y1;
    state->faces[fc * 3 + 2] = z1;
    state->faceUVs[fc * 2]     = u1;
    state->faceUVs[fc * 2 + 1] = v1;
    fc++;

    /* Vertex 2 */
    state->faces[fc * 3]     = x2;
    state->faces[fc * 3 + 1] = y2;
    state->faces[fc * 3 + 2] = z2;
    state->faceUVs[fc * 2]     = u2;
    state->faceUVs[fc * 2 + 1] = v2;
    fc++;

    /* Vertex 3 */
    state->faces[fc * 3]     = x3;
    state->faces[fc * 3 + 1] = y3;
    state->faces[fc * 3 + 2] = z3;
    state->faceUVs[fc * 2]     = u3;
    state->faceUVs[fc * 2 + 1] = v3;
    fc++;

    state->faceCount = fc;
}

/* ========================================================================= */
/* Scanline triangle rasterizer with Z-buffer                                 */
/* ========================================================================= */

/**
 * Optimized scanline triangle fill with per-pixel Z-buffer.
 * Uses barycentric-like edge walking for each scanline.
 *
 * ARM optimizations:
 * - float instead of double (VFP: 1 cycle vs 6-8 cycles)
 * - Pre-computed row base pointer for pixel/Z-buffer access
 * - int counter in inner X loop (avoids repeated float→int)
 * - Fast ARM float-to-int via VCVT instruction
 */
M3D_INLINE void m3d_fill_triangle(M3DState* state,
                                  float x1, float y1, float z1,
                                  float x2, float y2, float z2,
                                  float x3, float y3, float z3) {
    int w = state->width;
    int h = state->height;
    if (w <= 0 || h <= 0) return;

    uint32_t* pixels = state->framebuffer->pixels;
    float* zbuffer = state->zbuffer;
    unsigned int color = state->color;

    /* Sort vertices by Y coordinate (top to bottom) */
    if (y1 > y2) { float t; t=x1;x1=x2;x2=t; t=y1;y1=y2;y2=t; t=z1;z1=z2;z2=t; }
    if (y1 > y3) { float t; t=x1;x1=x3;x3=t; t=y1;y1=y3;y3=t; t=z1;z1=z3;z3=t; }
    if (y2 > y3) { float t; t=x2;x2=x3;x3=t; t=y2;y2=y3;y3=t; t=z2;z2=z3;z3=t; }

    int iy1 = m3d_float_to_int(y1), iy2 = m3d_float_to_int(y2), iy3 = m3d_float_to_int(y3);

    /* Clip to screen bounds */
    int miny = (iy1 < 0) ? 0 : iy1;
    int maxy = (iy3 >= h) ? h - 1 : iy3;
    if (miny > maxy) return;

    for (int y = miny; y <= maxy; y++) {
        /* Interpolate X bounds on the left and right edges */
        float xl, xr, zl, zr;

        if (y <= iy2) {
            /* Top half: interpolate between vertex 1 and vertex 2/3 */
            float dy = (iy2 != iy1) ? (float)(y - iy1) / (float)(iy2 - iy1) : 0.0f;
            if (iy2 == iy1) {
                xl = x1; xr = x3; zl = z1; zr = z3;
            } else {
                xl = x1 + (x2 - x1) * dy;
                zl = z1 + (z2 - z1) * dy;
            }
            /* Right edge from vertex 1 to vertex 3 */
            {
                float dy3 = (iy3 != iy1) ? (float)(y - iy1) / (float)(iy3 - iy1) : 0.0f;
                xr = x1 + (x3 - x1) * dy3;
                zr = z1 + (z3 - z1) * dy3;
            }
            /* Ensure xl is on the left */
            if (xl > xr) {
                float t; t=xl;xl=xr;xr=t; t=zl;zl=zr;zr=t;
            }
        } else {
            /* Bottom half: interpolate between vertex 2 and vertex 3 */
            float dy23 = (iy3 != iy2) ? (float)(y - iy2) / (float)(iy3 - iy2) : 0.0f;
            xl = x2 + (x3 - x2) * dy23;
            zl = z2 + (z3 - z2) * dy23;
            /* Right edge from vertex 1 to vertex 3 */
            {
                float dy13 = (iy3 != iy1) ? (float)(y - iy1) / (float)(iy3 - iy1) : 0.0f;
                xr = x1 + (x3 - x1) * dy13;
                zr = z1 + (z3 - z1) * dy13;
            }
            /* Ensure xl is on the left */
            if (xl > xr) {
                float t; t=xl;xl=xr;xr=t; t=zl;zl=zr;zr=t;
            }
        }

        /* Clip X bounds */
        int ixl = m3d_float_to_int(xl);
        int ixr = m3d_float_to_int(xr);
        if (ixl < 0) ixl = 0;
        if (ixr >= w) ixr = w - 1;

        int span = ixr - ixl;
        if (span <= 0) continue;

        float dzdx = (zr - zl) / (float)span;

        /* Pre-compute row base pointers */
        uint32_t* row_pixels = pixels + y * w;
        float* row_zbuf = zbuffer + y * w;
        float z = zl;

        /* Inner loop: int counter, minimal overhead */
        for (int ix = ixl; ix <= ixr; ix++) {
            if (z < row_zbuf[ix]) {
                row_zbuf[ix] = z;
                row_pixels[ix] = color;
            }
            z += dzdx;
        }
    }
}

/* ========================================================================= */
/* Native method handlers                                                     */
/* ========================================================================= */

/*
 * Static method: M3D.createInstance()Lcom/nokia/mid/m3d/M3D;
 *
 * Creates a new M3D Java object and allocates a C-side state for it.
 * For static methods, arg_count == 0 (no 'this' pointer).
 */
static JavaValue native_m3d_createInstance(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;

    JavaClass* m3d_class = jvm_load_class(jvm, "com/nokia/mid/m3d/M3D");
    if (!m3d_class) {
        return NATIVE_RETURN_NULL();
    }

    JavaObject* obj = jvm_new_object(jvm, m3d_class);
    if (!obj) {
        return NATIVE_RETURN_NULL();
    }

    M3DState* state = m3d_alloc_state(obj);
    if (!state) {
        return NATIVE_RETURN_NULL();
    }

    /* Initialize with defaults */
    state->color = 0xFF000000;
    state->clearcolor = 0xFFFFFFFF;
    m3d_identity(state->matrix);
    m3d_identity(state->stack);

    return NATIVE_RETURN_OBJECT(obj);
}

/*
 * M3D.setupBuffers(III)V
 * args[0] = this, args[1] = flags, args[2] = displayWidth, args[3] = displayHeight
 */
static JavaValue native_m3d_setupBuffers(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    jint flags  = args[1].i;
    jint width  = args[2].i;
    jint height = args[3].i;
    (void)flags;

    /* Free previous buffers if any */
    if (state->zbuffer) {
        free(state->zbuffer);
        state->zbuffer = NULL;
    }
    state->framebuffer = NULL;
    state->gc = NULL;

    /* Validate dimensions */
    if (width <= 0 || height <= 0) return NATIVE_RETURN_VOID();
    if (width > 4096 || height > 4096) return NATIVE_RETURN_VOID();

    state->width  = width;
    state->height = height;

    /* Create framebuffer image */
    state->framebuffer = midp_image_create(width, height, 1);
    if (!state->framebuffer) return NATIVE_RETURN_VOID();

    /* Get graphics context for the framebuffer */
    state->gc = midp_image_get_graphics(state->framebuffer);
    if (!state->gc) {
        /* midp_image_create returns an image that's always drawable */
        state->framebuffer = NULL;
        return NATIVE_RETURN_VOID();
    }

    /* Allocate Z-buffer (float: 4 bytes/pixel, half the memory of double) */
    state->zbuffer = (float*)calloc((size_t)width * height, sizeof(float));
    if (!state->zbuffer) {
        state->framebuffer = NULL;
        state->gc = NULL;
        return NATIVE_RETURN_VOID();
    }

    /* Initialize Z-buffer to far distance (local variable helps compiler) */
    {
        int total = width * height;
        float far_val = -128.0f;
        for (int i = 0; i < total; i++) {
            state->zbuffer[i] = far_val;
        }
    }

    /* Do an initial clear */
    if (state->gc) {
        midp_graphics_set_color(state->gc, (int)state->clearcolor, 0xFF);
        midp_graphics_fill_rect(state->gc, 0, 0, width, height);
    }

    m3d_identity(state->matrix);
    m3d_identity(state->stack);
    state->boundTexture = 0;

    return NATIVE_RETURN_VOID();
}

/*
 * M3D.removeBuffers()V
 */
static JavaValue native_m3d_removeBuffers(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    m3d_free_state(state);
    return NATIVE_RETURN_VOID();
}

/*
 * M3D.clear(I)V
 * args[0] = this, args[1] = mask
 */
static JavaValue native_m3d_clear(JVM* jvm, JavaThread* thread,
                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state || !state->gc) return NATIVE_RETURN_VOID();

    /* Clear the framebuffer with clear color */
    midp_graphics_set_color(state->gc, (int)state->clearcolor, 0xFF);
    midp_graphics_fill_rect(state->gc, 0, 0, state->width, state->height);

    /* Reset drawing color */
    midp_graphics_set_color(state->gc, (int)state->color, 0xFF);

    /* Reset matrices */
    m3d_identity(state->matrix);
    m3d_identity(state->stack);

    /* Reset Z-buffer (local variable helps compiler optimize) */
    if (state->zbuffer) {
        int total = state->width * state->height;
        float far_val = -128.0f;
        for (int i = 0; i < total; i++) {
            state->zbuffer[i] = far_val;
        }
    }

    state->boundTexture = 0;

    return NATIVE_RETURN_VOID();
}

/*
 * M3D.blit(Ljavax/microedition/lcdui/Graphics;IIII)V
 * args[0] = this, args[1] = Graphics, args[2] = x, args[3] = y, args[4] = w, args[5] = h
 */
static JavaValue native_m3d_blit(JVM* jvm, JavaThread* thread,
                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state || !state->framebuffer) return NATIVE_RETURN_VOID();

    JavaObject* gfx_obj = (JavaObject*)args[1].ref;
    jint x = args[2].i;
    jint y = args[3].i;
    /* args[4] = w, args[5] = h - not used (draw full framebuffer) */

    MidpGraphics* screen_gfx = get_graphics_from_object(gfx_obj);
    if (!screen_gfx) return NATIVE_RETURN_VOID();

    midp_graphics_draw_image(screen_gfx, state->framebuffer, x, y, ANCHOR_TOP_LEFT);

    return NATIVE_RETURN_VOID();
}

/*
 * M3D.bindTexture(ILcom/nokia/mid/m3d/Texture;)V
 * args[0] = this, args[1] = unit, args[2] = Texture
 */
static JavaValue native_m3d_bindTexture(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    /* Mark texture as bound (texture support is a stub) */
    state->boundTexture = 1;

    return NATIVE_RETURN_VOID();
}

/*
 * M3D.loadIdentity()V
 */
static JavaValue native_m3d_loadIdentity(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    m3d_identity(state->matrix);

    return NATIVE_RETURN_VOID();
}

/*
 * M3D.pushMatrix()V
 */
static JavaValue native_m3d_pushMatrix(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    m3d_clone(state->stack, state->matrix);

    return NATIVE_RETURN_VOID();
}

/*
 * M3D.popMatrix()V
 */
static JavaValue native_m3d_popMatrix(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    m3d_clone(state->matrix, state->stack);

    return NATIVE_RETURN_VOID();
}

/*
 * M3D.frustumxi(IIIIII)V
 * args[0] = this, args[1..6] = left, right, top, bottom, nearclip, farclip
 * Values are fixed-point 16.16
 */
static JavaValue native_m3d_frustumxi(JVM* jvm, JavaThread* thread,
                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    float r = args[1].i / 65536.0f;
    float l = args[2].i / 65536.0f;
    float t = args[3].i / 65536.0f;
    float b = args[4].i / 65536.0f;
    float n = args[5].i / 65536.0f;
    float f = args[6].i / 65536.0f;

    state->near = -n;
    state->far  = -f;

    m3d_projection(state->projm, r - l, t - b, n, f);

    return NATIVE_RETURN_VOID();
}

/*
 * M3D.scalexi(III)V
 * args[0] = this, args[1..3] = X, Y, Z (fixed-point 16.16)
 */
static JavaValue native_m3d_scalexi(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    float sx = args[1].i / 65536.0f;
    float sy = args[2].i / 65536.0f;
    float sz = args[3].i / 65536.0f;

    float scale[16] = {
        sx, 0.0f, 0.0f, 0.0f,
        0.0f, sy,   0.0f, 0.0f,
        0.0f, 0.0f, sz,   0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };

    m3d_matmul(scale, state->matrix);
    m3d_clone(state->matrix, scale);

    return NATIVE_RETURN_VOID();
}

/*
 * M3D.translatexi(III)V
 * args[0] = this, args[1..3] = X, Y, Z (fixed-point 16.16)
 */
static JavaValue native_m3d_translatexi(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    float tx = args[1].i / 65536.0f;
    float ty = args[2].i / 65536.0f;
    float tz = args[3].i / 65536.0f;

    float trans[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        tx,   ty,   tz,   1.0f
    };

    m3d_matmul(trans, state->matrix);
    m3d_clone(state->matrix, trans);

    return NATIVE_RETURN_VOID();
}

/*
 * M3D.rotatexi(IIII)V
 * args[0] = this, args[1] = Angle, args[2] = X axis, args[3] = Y axis, args[4] = Z axis
 * Angle is fixed-point 16.16 degrees. Axis is a selector (1 = on that axis, 0 = off).
 */
static JavaValue native_m3d_rotatexi(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    float angle = (args[1].i / 65536.0f) * DEG2RAD;
    jint axisX = args[2].i;
    jint axisY = args[3].i;
    jint axisZ = args[4].i;

    float rotm[16];
    memset(rotm, 0, sizeof(rotm));

    if (axisX != 0) {
        /* Rotate around X axis */
        rotm[0]  = 1.0f;
        rotm[5]  = cosf(angle);
        rotm[6]  = sinf(angle);
        rotm[9]  = -sinf(angle);
        rotm[10] = cosf(angle);
        rotm[15] = 1.0f;
    } else if (axisY != 0) {
        /* Rotate around Y axis */
        rotm[5]  = 1.0f;
        rotm[0]  = cosf(angle);
        rotm[2]  = -sinf(angle);
        rotm[8]  = sinf(angle);
        rotm[10] = cosf(angle);
        rotm[15] = 1.0f;
    } else if (axisZ != 0) {
        /* Rotate around Z axis */
        rotm[10] = 1.0f;
        rotm[0]  = cosf(angle);
        rotm[1]  = sinf(angle);
        rotm[4]  = -sinf(angle);
        rotm[5]  = cosf(angle);
        rotm[15] = 1.0f;
    } else {
        /* No axis selected - identity */
        m3d_identity(rotm);
    }

    m3d_matmul(rotm, state->matrix);
    m3d_clone(state->matrix, rotm);

    return NATIVE_RETURN_VOID();
}

/*
 * M3D.color4ub(BBBB)V
 * args[0] = this, args[1] = r, args[2] = g, args[3] = b, args[4] = a
 * Bytes are passed as jint values (Java bytes are sign-extended).
 */
static JavaValue native_m3d_color4ub(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    unsigned int r = (unsigned int)(args[1].i & 0xFF);
    unsigned int g = (unsigned int)(args[2].i & 0xFF);
    unsigned int b = (unsigned int)(args[3].i & 0xFF);
    /* unsigned int a = (unsigned int)(args[4].i & 0xFF); */

    state->color = 0xFF000000u | (r << 16) | (g << 8) | b;

    return NATIVE_RETURN_VOID();
}

/*
 * M3D.clearColor4ub(BBBB)V
 * args[0] = this, args[1] = r, args[2] = g, args[3] = b, args[4] = a
 */
static JavaValue native_m3d_clearColor4ub(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    unsigned int r = (unsigned int)(args[1].i & 0xFF);
    unsigned int g = (unsigned int)(args[2].i & 0xFF);
    unsigned int b = (unsigned int)(args[3].i & 0xFF);
    /* unsigned int a = (unsigned int)(args[4].i & 0xFF); */

    state->clearcolor = 0xFF000000u | (r << 16) | (g << 8) | b;

    return NATIVE_RETURN_VOID();
}

/*
 * M3D.vertexPointerub(II[B)V
 * args[0] = this, args[1] = size, args[2] = stride, args[3] = byte[] vertices
 * Vertex data: triples of (x, y, z) bytes
 */
static JavaValue native_m3d_vertexPointerub(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    JavaArray* vertex_array = (JavaArray*)args[3].ref;
    if (!vertex_array) return NATIVE_RETURN_VOID();

    jbyte* vdata = (jbyte*)array_data(vertex_array);
    int len = (int)vertex_array->length;

    if (len > MAX_VERTS) len = MAX_VERTS;

    for (int i = 0; i < len; i++) {
        state->verts[i] = (float)vdata[i];
    }

    state->vertCount = len;

    return NATIVE_RETURN_VOID();
}

/*
 * M3D.texCoordPointerub(II[B)V
 * args[0] = this, args[1] = size, args[2] = stride, args[3] = byte[] uvs
 * UV data: pairs of (u, v) bytes
 */
static JavaValue native_m3d_texCoordPointerub(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state) return NATIVE_RETURN_VOID();

    JavaArray* uv_array = (JavaArray*)args[3].ref;
    if (!uv_array) return NATIVE_RETURN_VOID();

    jbyte* udata = (jbyte*)array_data(uv_array);
    int len = (int)uv_array->length;

    if (len > MAX_UVS) len = MAX_UVS;

    for (int i = 0; i < len; i++) {
        state->UVs[i] = (float)udata[i];
    }

    return NATIVE_RETURN_VOID();
}

/*
 * M3D.drawElementsub(II[B)V
 * args[0] = this, args[1] = mode, args[2] = count, args[3] = byte[] facelist
 * Draws triangles indexed by the face list.
 */
static JavaValue native_m3d_drawElementsub(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state || !state->gc) return NATIVE_RETURN_VOID();

    JavaArray* face_array = (JavaArray*)args[3].ref;
    if (!face_array) return NATIVE_RETURN_VOID();

    jbyte* fdata = (jbyte*)array_data(face_array);
    int flen = (int)face_array->length;

    /* Set draw color on the graphics context (used by drawArrays, not by our rasterizer) */
    midp_graphics_set_color(state->gc, (int)state->color, 0xFF);

    /* Transform all vertices by the model-view matrix */
    m3d_apply_matrix(state, state->matrix);

    /* Build face list from index array */
    state->faceCount = 0;
    for (int i = 0; i + 2 < flen; i += 3) {
        int i0 = fdata[i] & 0xFF;
        int i1 = fdata[i + 1] & 0xFF;
        int i2 = fdata[i + 2] & 0xFF;

        float x1 = state->verts[i0 * 3],     y1 = state->verts[i0 * 3 + 1], z1 = state->verts[i0 * 3 + 2];
        float x2 = state->verts[i1 * 3],     y2 = state->verts[i1 * 3 + 1], z2 = state->verts[i1 * 3 + 2];
        float x3 = state->verts[i2 * 3],     y3 = state->verts[i2 * 3 + 1], z3 = state->verts[i2 * 3 + 2];

        /* Near-plane culling */
        if (z1 > state->near && z2 > state->near && z3 > state->near) continue;

        float u1 = state->UVs[i0 * 2],     v1 = state->UVs[i0 * 2 + 1];
        float u2 = state->UVs[i1 * 2],     v2 = state->UVs[i1 * 2 + 1];
        float u3 = state->UVs[i2 * 2],     v3 = state->UVs[i2 * 2 + 1];

        m3d_add_faces(state, x1, y1, z1, x2, y2, z2, x3, y3, z3,
                               u1, v1, u2, v2, u3, v3);
    }

    /* Project vertices through projection matrix */
    {
        float* faces = state->faces;
        const float* pm = state->projm;
        int fc = state->faceCount;
        for (int i = 0; i < fc; i++) {
            float x = faces[i * 3], y = faces[i * 3 + 1], z = faces[i * 3 + 2];
            float px = x * pm[0]  + y * pm[4]  + z * pm[8]  + pm[12];
            float py = x * pm[1]  + y * pm[5]  + z * pm[9]  + pm[13];
            float pz = x * pm[2]  + y * pm[6]  + z * pm[10] + pm[14];
            float pw = x * pm[3]  + y * pm[7]  + z * pm[11] + pm[15];

            if (pw != 0.0f && pw != 1.0f) {
                px /= pw;
                py /= pw;
                pz /= pw;
            }

            faces[i * 3]     = px;
            faces[i * 3 + 1] = py;
            faces[i * 3 + 2] = pz;
        }
    }

    /* Rasterize triangles to framebuffer */
    float ox = state->width  * 0.5f;
    float oy = state->height * 0.5f;

    for (int i = 0; i + 2 < state->faceCount; i += 3) {
        float sx1 = (state->faces[i * 3]     * ox) + ox;
        float sy1 = (state->faces[i * 3 + 1] * oy) + oy;
        float sz1 =  state->faces[i * 3 + 2];

        float sx2 = (state->faces[(i + 1) * 3]     * ox) + ox;
        float sy2 = (state->faces[(i + 1) * 3 + 1] * oy) + oy;
        float sz2 =  state->faces[(i + 1) * 3 + 2];

        float sx3 = (state->faces[(i + 2) * 3]     * ox) + ox;
        float sy3 = (state->faces[(i + 2) * 3 + 1] * oy) + oy;
        float sz3 =  state->faces[(i + 2) * 3 + 2];

        m3d_fill_triangle(state, sx1, sy1, sz1, sx2, sy2, sz2, sx3, sy3, sz3);
    }

    return NATIVE_RETURN_VOID();
}

/*
 * M3D.drawArrays(III)V
 * args[0] = this, args[1] = mode, args[2] = first, args[3] = count
 * Used for water/ground plane effects.
 */
static JavaValue native_m3d_drawArrays(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;

    JavaObject* obj = (JavaObject*)args[0].ref;
    M3DState* state = m3d_get_state(obj);
    if (!state || !state->gc) return NATIVE_RETURN_VOID();

    midp_graphics_set_color(state->gc, (int)state->color, 0xFF);

    /* Apply model-view matrix to vertices */
    m3d_apply_matrix(state, state->matrix);

    /* Project Y coordinate for water/ground line */
    float y = state->verts[1];
    float z = state->verts[2];
    y = y * state->projm[5] / (-z);

    float oy = state->height * 0.5f;
    y = (y * oy) + oy;

    int iy = m3d_float_to_int(y);

    /* Fill from the projected Y line to the bottom of the screen */
    if (iy < state->height) {
        int fill_y = (iy < 0) ? 0 : iy;
        int fill_h = state->height - fill_y;
        if (fill_h > 0) {
            midp_graphics_fill_rect(state->gc, 0, fill_y, state->width, fill_h);
        }
    }

    return NATIVE_RETURN_VOID();
}

/* ---- No-op stubs ---- */

static JavaValue native_m3d_viewport(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_m3d_cullFace(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_m3d_matrixMode(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_m3d_enableClientState(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_m3d_disableClientState(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_m3d_enable(JVM* jvm, JavaThread* thread,
                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

static JavaValue native_m3d_disable(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    return NATIVE_RETURN_VOID();
}

/* ========================================================================= */
/* Registration                                                               */
/* ========================================================================= */

/**
 * Register all Nokia M3D native method implementations.
 *
 * IMPORTANT: This function registers the real handlers. If init_nokia_misc()
 * in native.c has already registered NULL stubs for M3D methods, this function
 * must be called BEFORE init_nokia_misc() so that native_find() discovers
 * the real handlers first (it returns the first matching entry).
 *
 * Alternatively, remove the M3D block from init_nokia_misc() and call this
 * function in its place.
 */
void init_nokia_m3d_impl(JVM* jvm) {
    static const NativeMethodEntry m3d_methods[] = {
        /* Instance management */
        {"com/nokia/mid/m3d/M3D", "createInstance",
         "()Lcom/nokia/mid/m3d/M3D;", native_m3d_createInstance},
        {"com/nokia/mid/m3d/M3D", "setupBuffers",
         "(III)V", native_m3d_setupBuffers},
        {"com/nokia/mid/m3d/M3D", "removeBuffers",
         "()V", native_m3d_removeBuffers},
        {"com/nokia/mid/m3d/M3D", "clear",
         "(I)V", native_m3d_clear},

        /* Rendering output */
        {"com/nokia/mid/m3d/M3D", "blit",
         "(Ljavax/microedition/lcdui/Graphics;IIII)V", native_m3d_blit},

        /* Texture binding (stub - texture support not yet implemented) */
        {"com/nokia/mid/m3d/M3D", "bindTexture",
         "(ILcom/nokia/mid/m3d/Texture;)V", native_m3d_bindTexture},

        /* Matrix operations */
        {"com/nokia/mid/m3d/M3D", "loadIdentity",
         "()V", native_m3d_loadIdentity},
        {"com/nokia/mid/m3d/M3D", "pushMatrix",
         "()V", native_m3d_pushMatrix},
        {"com/nokia/mid/m3d/M3D", "popMatrix",
         "()V", native_m3d_popMatrix},
        {"com/nokia/mid/m3d/M3D", "frustumxi",
         "(IIIIII)V", native_m3d_frustumxi},
        {"com/nokia/mid/m3d/M3D", "scalexi",
         "(III)V", native_m3d_scalexi},
        {"com/nokia/mid/m3d/M3D", "translatexi",
         "(III)V", native_m3d_translatexi},
        {"com/nokia/mid/m3d/M3D", "rotatexi",
         "(IIII)V", native_m3d_rotatexi},

        /* Drawing state */
        {"com/nokia/mid/m3d/M3D", "color4ub",
         "(BBBB)V", native_m3d_color4ub},
        {"com/nokia/mid/m3d/M3D", "clearColor4ub",
         "(BBBB)V", native_m3d_clearColor4ub},

        /* Vertex and index data */
        {"com/nokia/mid/m3d/M3D", "vertexPointerub",
         "(II[B)V", native_m3d_vertexPointerub},
        {"com/nokia/mid/m3d/M3D", "texCoordPointerub",
         "(II[B)V", native_m3d_texCoordPointerub},

        /* Drawing commands */
        {"com/nokia/mid/m3d/M3D", "drawElementsub",
         "(II[B)V", native_m3d_drawElementsub},
        {"com/nokia/mid/m3d/M3D", "drawArrays",
         "(III)V", native_m3d_drawArrays},

        /* GL state (no-ops) */
        {"com/nokia/mid/m3d/M3D", "viewport",
         "(IIII)V", native_m3d_viewport},
        {"com/nokia/mid/m3d/M3D", "cullFace",
         "(I)V", native_m3d_cullFace},
        {"com/nokia/mid/m3d/M3D", "matrixMode",
         "(I)V", native_m3d_matrixMode},
        {"com/nokia/mid/m3d/M3D", "enableClientState",
         "(I)V", native_m3d_enableClientState},
        {"com/nokia/mid/m3d/M3D", "disableClientState",
         "(I)V", native_m3d_disableClientState},
        {"com/nokia/mid/m3d/M3D", "enable",
         "(I)V", native_m3d_enable},
        {"com/nokia/mid/m3d/M3D", "disable",
         "(I)V", native_m3d_disable},
    };

    native_register_methods(jvm, m3d_methods,
                            sizeof(m3d_methods) / sizeof(m3d_methods[0]));
}
