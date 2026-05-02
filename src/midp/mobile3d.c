/*
 * J2ME Emulator - JSR 184 Mobile 3D Graphics API (Software Implementation)
 * 
 * This implements the M3G (Mobile 3D Graphics) API in pure software mode.
 * JSR 184 provides a retained-mode 3D graphics API for mobile devices.
 * 
 * Implemented features:
 * - Graphics3D rendering context
 * - Transform (4x4 matrix operations)
 * - VertexArray (vertex data storage)
 * - IndexBuffer (triangle indices)
 * - Mesh (renderable 3D objects)
 * - Camera (view/projection)
 * - Light (directional, point, spot)
 * - Material and Texture2D
 * - Background
 * - World (scene graph)
 * - Software rasterization with Z-buffer
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L  /* For strdup */
#endif

#include <stdio.h>
#include "debug.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "jvm.h"
#include "native.h"
#include "heap.h"
#include "opcodes.h"  /* For T_* array type constants */
#include "midp.h"     /* For load_jar_resource */
#include "sdl_backend.h"  /* For SdlContext, sdl_get_global_context, get_graphics_from_object */

/* ARM-specific optimization hints for hot-path functions */
#if defined(__ARM_ARCH_7A__) || defined(__arm__)
#define M3G_INLINE __attribute__((always_inline)) static inline
#define M3G_HOT __attribute__((hot))
#else
#define M3G_INLINE static inline
#define M3G_HOT
#endif

/* JSR-184 Light mode constants (as stored in M3G binary files and Java Light class) */
#define M3G_LIGHT_MODE_AMBIENT       128
#define M3G_LIGHT_MODE_DIRECTIONAL   129
#define M3G_LIGHT_MODE_OMNI          130
#define M3G_LIGHT_MODE_SPOT          131

/* Internal light type constants (used by the rasterizer) */
#define M3G_LIGHT_AMBIENT       1
#define M3G_LIGHT_DIRECTIONAL   2  
#define M3G_LIGHT_OMNI          4
#define M3G_LIGHT_SPOT          8

/* Map JSR-184 light mode (128-131) to internal light type (1,2,4,8) */
static int m3g_light_mode_to_type(int mode) {
    switch (mode) {
        case M3G_LIGHT_MODE_AMBIENT:     return M3G_LIGHT_AMBIENT;
        case M3G_LIGHT_MODE_DIRECTIONAL: return M3G_LIGHT_DIRECTIONAL;
        case M3G_LIGHT_MODE_OMNI:        return M3G_LIGHT_OMNI;
        case M3G_LIGHT_MODE_SPOT:        return M3G_LIGHT_SPOT;
        default: return M3G_LIGHT_DIRECTIONAL; /* fallback */
    }
}

/* ============================================================================
 * HELPER FUNCTIONS FOR FIELD ACCESS
 * ============================================================================ */

/* Find the slot index for a named field in an object.
 * This correctly traverses the class hierarchy from Object down to the actual class,
 * calculating the cumulative slot index for instance fields.
 * Returns -1 if field not found or slot is out of bounds. */
static int m3g_find_field_slot(JavaObject* obj, const char* field_name) {
    if (!obj || !field_name) return -1;
    
    JavaClass* clazz = obj->header.clazz;
    if (!clazz) return -1;
    
    /* Calculate maximum valid slot from instance_size.
     * Guard against corrupted instance_size that could be smaller than
     * ObjectHeader, which would cause max_slots to go negative (as int). */
    size_t header_size = sizeof(ObjectHeader);
    int max_slots;
    if (clazz->instance_size <= header_size) {
        return -1;  /* Object too small to have any fields */
    }
    max_slots = (int)((clazz->instance_size - header_size) / sizeof(JavaValue));
    if (max_slots <= 0) return -1;
    
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
                /* Validate slot is within bounds */
                if (slot >= max_slots) {
                    GFX_DEBUG("M3G: Field '%s' slot %d exceeds object capacity %d", 
                             field_name, slot, max_slots);
                    return -1;
                }
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
    
    return -1;  /* Field not found */
}

/* Rate-limited warning for missing fields - max 1 per second per field */
#define M3G_FIELD_WARN_MAX 200
static struct { const char* field; const char* cls; int count; } m3g_field_warns[M3G_FIELD_WARN_MAX];
static int m3g_field_warn_count = 0;

static void m3g_warn_missing_field(const char* field_name, const char* class_name) {
    /* Check if already warned for this class+field combo */
    for (int i = 0; i < m3g_field_warn_count && i < M3G_FIELD_WARN_MAX; i++) {
        if (m3g_field_warns[i].field == field_name && m3g_field_warns[i].cls == class_name) {
            m3g_field_warns[i].count++;
            if (m3g_field_warns[i].count <= 3) {
                fprintf(stderr, "[M3G-WARN] Field '%s' not found in %s (repeat #%d)\n",
                        field_name, class_name ? class_name : "?", m3g_field_warns[i].count);
            }
            return;
        }
    }
    /* First time warning */
    if (m3g_field_warn_count < M3G_FIELD_WARN_MAX) {
        fprintf(stderr, "[M3G-WARN] Field '%s' not found in class %s\n",
                field_name, class_name ? class_name : "?");
        m3g_field_warns[m3g_field_warn_count].field = field_name;
        m3g_field_warns[m3g_field_warn_count].cls = class_name;
        m3g_field_warns[m3g_field_warn_count].count = 1;
        m3g_field_warn_count++;
    }
}

/* Helper to get an int field value by name */
static jint m3g_get_int_field(JavaObject* obj, const char* field_name, jint default_val) {
    int slot = m3g_find_field_slot(obj, field_name);
    if (slot >= 0) {
        return obj->fields[slot].i;
    }
    m3g_warn_missing_field(field_name, obj && obj->header.clazz ? obj->header.clazz->class_name : NULL);
    return default_val;
}

/* Helper to set an int field value by name */
static void m3g_set_int_field(JavaObject* obj, const char* field_name, jint value) {
    int slot = m3g_find_field_slot(obj, field_name);
    if (slot >= 0) {
        obj->fields[slot].i = value;
        return;
    }
    m3g_warn_missing_field(field_name, obj && obj->header.clazz ? obj->header.clazz->class_name : NULL);
}

/* Helper to get a float field value by name */
static jfloat m3g_get_float_field(JavaObject* obj, const char* field_name, jfloat default_val) {
    int slot = m3g_find_field_slot(obj, field_name);
    if (slot >= 0) {
        return obj->fields[slot].f;
    }
    m3g_warn_missing_field(field_name, obj && obj->header.clazz ? obj->header.clazz->class_name : NULL);
    return default_val;
}

/* Helper to set a float field value by name */
static void m3g_set_float_field(JavaObject* obj, const char* field_name, jfloat value) {
    int slot = m3g_find_field_slot(obj, field_name);
    if (slot >= 0) {
        obj->fields[slot].f = value;
        return;
    }
    m3g_warn_missing_field(field_name, obj && obj->header.clazz ? obj->header.clazz->class_name : NULL);
}

/* Helper to get a reference field value by name */
static JavaObject* m3g_get_ref_field(JavaObject* obj, const char* field_name) {
    int slot = m3g_find_field_slot(obj, field_name);
    if (slot >= 0) {
        return (JavaObject*)obj->fields[slot].ref;
    }
    m3g_warn_missing_field(field_name, obj && obj->header.clazz ? obj->header.clazz->class_name : NULL);
    return NULL;
}

/* Helper to set a reference field value by name */
static void m3g_set_ref_field(JavaObject* obj, const char* field_name, JavaObject* value) {
    int slot = m3g_find_field_slot(obj, field_name);
    if (slot >= 0) {
        obj->fields[slot].ref = value;
        return;
    }
    m3g_warn_missing_field(field_name, obj && obj->header.clazz ? obj->header.clazz->class_name : NULL);
}

/* ============================================================================
 * M3G Constants (from JSR 184 specification)
 * ============================================================================ */


/* Material components */
#define M3G_MATERIAL_AMBIENT    1
#define M3G_MATERIAL_DIFFUSE    2
#define M3G_MATERIAL_EMISSIVE   4
#define M3G_MATERIAL_SPECULAR   8

/* Rendering hints */
#define M3G_ANTIALIAS           1
#define M3G_DITHER              2
#define M3G_TRUE_COLOR          4

/* Compositing modes */
#define M3G_ALPHA_BLEND         64
#define M3G_ALPHA_ADDITIVE      65
#define M3G_ALPHA_MULTIPLY      66
#define M3G_ALPHA_REPLACE       67

/* Polygon modes */
#define M3G_POLYGON_MODE_SOLID          64
#define M3G_POLYGON_MODE_WIREFRAME      65
#define M3G_POLYGON_MODE_POINT          66
#define M3G_POLYGON_MODE_POINT_SPRITE   67

/* Crop modes */
#define M3G_CROP_CENTER        96
#define M3G_CROP_STRETCH       97
#define M3G_CROP_FILL          98

/* Maximum dimensions for Image2D and render targets to prevent OOM.
 * J2ME M3G typically supports 256x256 or 512x512 textures.
 * Allow up to 1024x1024 for compatibility with newer devices. */
#define M3G_MAX_IMAGE_DIMENSION 1024
#define M3G_MAX_IMAGE_PIXELS (M3G_MAX_IMAGE_DIMENSION * M3G_MAX_IMAGE_DIMENSION)

/* Maximum vertex count for VertexArray to prevent OOM */
#define M3G_MAX_VERTEX_COUNT 65536
#define M3G_MAX_INDEX_COUNT  65536

/* ============================================================================
 * M3G Internal Data Structures
 * ============================================================================ */

/* 4x4 Transformation Matrix (column-major, OpenGL style) */
typedef struct {
    float m[16];  /* Column-major: m[col*4+row] */
} M3GTransform;

/* Vertex data */
typedef struct {
    float* positions;     /* XYZ positions */
    float* normals;       /* XYZ normals (optional) */
    float* texcoords;     /* UV coordinates (optional) */
    uint8_t* colors;      /* RGBA colors (optional) */
    int vertex_count;
    int vertex_stride;    /* Components per vertex */
} M3GVertexArray;

/* Triangle indices */
typedef struct {
    uint16_t* indices;
    int index_count;
    int primitive_type;   /* TRIANGLES=4, LINES=3, POINTS=2 */
} M3GIndexBuffer;

/* Material properties */
typedef struct {
    float ambient[4];     /* RGBA */
    float diffuse[4];
    float specular[4];
    float emissive[4];
    float shininess;
} M3GMaterial;

/* Texture data */
typedef struct {
    uint32_t* pixels;     /* ARGB pixels */
    int width;
    int height;
    int blend_s, blend_t; /* Wrapping mode */
    int filter_level;
} M3GTexture2D;

/* Light source */
typedef struct {
    JavaObject* obj;     /* Java Light object that owns this slot */
    int type;             /* AMBIENT, DIRECTIONAL, OMNI, SPOT */
    float color[4];       /* RGBA intensity */
    float direction[4];   /* For directional/spot */
    float position[4];    /* For omni/spot */
    float attenuation[3]; /* Constant, linear, quadratic */
    float spot_angle;     /* Spot cone angle */
    float spot_exponent;
} M3GLight;

/* Appearance state */
typedef struct {
    M3GMaterial* material;
    M3GTexture2D* texture;
    int compositing_mode;
    int polygon_mode;
    int layer;
    int winding;              /* 0 = CCW (default), 1 = CW */
    int two_sided_lighting;   /* 0 = one-sided, 1 = two-sided (no culling) */
    int cull_front;           /* 1 = CULL_FRONT (cull front faces), 0 = CULL_BACK */
    int blend_mode;           /* CompositingMode blending, -1 = use default */
    int alpha_threshold;      /* CompositingMode alphaThreshold * 255, -1 = use default */
    int texture_blend;        /* Texture2D blending mode */
} M3GAppearance;

/* Mesh object */
typedef struct {
    M3GVertexArray* vertices;
    M3GIndexBuffer* indices;
    M3GAppearance* appearance;
    int submesh_count;
} M3GMesh;

/* Camera */
typedef struct {
    float position[3];
    float look_at[3];
    float up[3];
    float fov;            /* Field of view in degrees */
    float aspect;         /* Width / Height */
    float near_plane;
    float far_plane;
    int projection_type;  /* 0=perspective, 1=parallel */
} M3GCamera;

/* Render buffer pool for object reuse - avoids per-frame allocations */
#define M3G_VERTEX_POOL_SIZE (64 * 1024)  /* 64K floats = ~21K vertices */
#define M3G_INDEX_POOL_SIZE (32 * 1024)   /* 32K indices */

typedef struct {
    float* vertex_pool;
    size_t vertex_pool_capacity;  /* in floats */
    size_t vertex_pool_used;
    
    uint16_t* index_pool;
    size_t index_pool_capacity;  /* in indices */
    size_t index_pool_used;
    
    int initialized;
} M3GRenderPool;

/* Rendering context */
typedef struct {
    /* Viewport */
    int viewport_x, viewport_y;
    int viewport_width, viewport_height;
    
    /* Target buffer */
    uint32_t* color_buffer;
    float* depth_buffer;
    int buffer_width, buffer_height;
    int buffers_allocated;  /* Track if buffers are allocated */
    
    /* Target for releaseTarget */
    JavaObject* target_image;       /* Image2D or Graphics Java object */
    MidpGraphics* target_gfx;       /* Native graphics context when target is Graphics */
    int target_is_graphics;         /* 1 if target is a Graphics object, 0 if Image2D */
    
    /* Current transformation */
    M3GTransform modelview;
    M3GTransform projection;
    M3GTransform mvp;     /* ModelView * Projection */
    
    /* Lights (max 8) */
    M3GLight lights[8];
    int light_count;
    
    /* Global ambient */
    float ambient_light[4];
    
    /* Render states */
    int depth_test_enabled;
    int depth_write_enabled;
    int blending_enabled;
    int culling_enabled;    /* Back-face culling */
    
    /* Render buffer pool */
    M3GRenderPool render_pool;
    
    /* Camera state for setCamera/render(Node,Transform) flow */
    JavaObject* camera;
    M3GTransform camera_transform;     /* Camera's composite transform (world space) */
    M3GTransform camera_inverse;       /* Inverse of camera transform (view matrix) */
    int camera_set;                    /* Flag: camera has been set via setCamera */
    
    /* Statistics */
    int triangles_rendered;
    int vertices_processed;
} M3GContext;

/* Global M3G context */
static M3GContext g_m3g;

/* Force-render tracking: some games (SU-30) do M3G scene setup
 * (setCamera, setActiveCamera, etc.) from a game thread, then call
 * repaint()/serviceRepaints() from paint(). But the actual bindTarget/render/
 * releaseTarget calls never happen - the game expects the emulator to handle
 * rendering of the scene graph.
 *
 * These flags are NOT reset per-paint. They track global M3G state.
 * When the game calls bindTarget/render, g_m3g_bindtarget_called is set,
 * which suppresses force-rendering. Otherwise, if scene setup was done but
 * no render happened, we force a render after the next paint() call.
 *
 * NOTE: volatile ensures compiler doesn't cache values in registers, but
 * does NOT provide atomicity or memory ordering. This is acceptable here
 * because: (a) the worst case is one extra/missed force-render, (b) the
 * game thread and paint thread are synchronized by the JVM's repaint queue.
 * __atomic_thread_fence provides the required memory ordering guarantee. */
static volatile bool g_m3g_scene_setup_done = false;   /* setCamera/setActiveCamera called */
static volatile bool g_m3g_bindtarget_called = false;   /* bindTarget called (suppresses force-render) */
static volatile bool g_m3g_render_done = false;       /* render() called this frame */
static JavaObject* g_m3g_last_world = NULL;             /* Last World set via setActiveCamera */

/* Pending render queue: records render(Node, Transform) calls when bindTarget
 * hasn't been called. During force-render, these are replayed with proper
 * transforms instead of scanning the registry with identity modelview. */
#define M3G_MAX_PENDING_RENDERS 64
typedef struct {
    JavaObject* node;
    JavaObject* transform;  /* Java Transform object - matrix stored as float[16] */
} M3GPendingRender;
static M3GPendingRender g_m3g_pending_renders[M3G_MAX_PENDING_RENDERS];
static volatile int g_m3g_pending_render_count = 0;

/* Memory barrier helper for cross-thread flag visibility.
 * Ensures writes to M3G state (world, camera) are visible to the paint thread. */
static inline void m3g_thread_fence(void) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/* Reset force-render tracking at start of each paint() cycle.
 * Only resets the per-frame render flags, NOT the scene setup flag. */
void m3g_reset_paint_tracking(void) {
    g_m3g_bindtarget_called = false;
    g_m3g_render_done = false;
    g_m3g_pending_render_count = 0;  /* Clear stale pending renders each frame */
    m3g_thread_fence();
}

/* Forward declarations needed by m3g_replay_pending_render (defined here,
 * but the actual functions are defined later in the file). */
static void m3g_transform_multiply(M3GTransform* result, const M3GTransform* a, const M3GTransform* b);
static void m3g_read_java_transform(JavaObject* transform, M3GTransform* out);
static void m3g_build_node_transform(JavaObject* node, M3GTransform* out);
static void m3g_render_mesh_with_mvp(JVM* jvm, JavaObject* mesh, M3GTransform* modelview);

/* Helper to replay a pending render(Node, Transform) call during force-render.
 * This renders the node tree using the recorded transform, same as native_graphics3d_render_node. */
static void m3g_replay_pending_render(JVM* jvm, JavaObject* node, JavaObject* transform) {
    if (!node) return;
    
    M3GTransform input_transform;
    m3g_read_java_transform(transform, &input_transform);
    
    M3GTransform combined;
    m3g_transform_multiply(&combined, &g_m3g.camera_inverse, &input_transform);
    
    JavaClass* clazz = node->header.clazz;
    if (!clazz || !clazz->class_name) return;
    const char* class_name = clazz->class_name;
    
    if (strstr(class_name, "Mesh") != NULL) {
        int render_enable = m3g_get_int_field(node, "renderingEnable", 1);
        if (render_enable) {
            M3GTransform node_transform;
            m3g_build_node_transform(node, &node_transform);
            M3GTransform full_modelview;
            m3g_transform_multiply(&full_modelview, &combined, &node_transform);
            m3g_render_mesh_with_mvp(jvm, node, &full_modelview);
        }
        return;
    }
    
    if (strstr(class_name, "Group") != NULL || strstr(class_name, "World") != NULL) {
        JavaArray* children = (JavaArray*)m3g_get_ref_field(node, "children");
        if (children && children->length > 0 && children->element_type == DESC_OBJECT) {
            JavaObject** child_arr = (JavaObject**)array_data(children);
            int rp_safe = m3g_get_int_field(node, "childCount", (int)children->length);
            if (rp_safe > (int)children->length) rp_safe = (int)children->length;
            for (jsize c = 0; c < rp_safe; c++) {
                JavaObject* child = child_arr[c];
                if (!child || !child->header.clazz || !child->header.clazz->class_name) continue;
                const char* child_name = child->header.clazz->class_name;
                
                if (strstr(child_name, "Mesh") != NULL) {
                    int re = m3g_get_int_field(child, "renderingEnable", 1);
                    if (re) {
                        M3GTransform child_transform;
                        m3g_build_node_transform(child, &child_transform);
                        M3GTransform child_combined;
                        m3g_transform_multiply(&child_combined, &input_transform, &child_transform);
                        M3GTransform full_modelview;
                        m3g_transform_multiply(&full_modelview, &g_m3g.camera_inverse, &child_combined);
                        m3g_render_mesh_with_mvp(jvm, child, &full_modelview);
                    }
                } else if (strstr(child_name, "Group") != NULL) {
                    /* Recurse into sub-groups */
                    M3GTransform child_transform;
                    m3g_build_node_transform(child, &child_transform);
                    M3GTransform child_combined;
                    m3g_transform_multiply(&child_combined, &input_transform, &child_transform);
                    M3GTransform branch_modelview;
                    m3g_transform_multiply(&branch_modelview, &g_m3g.camera_inverse, &child_combined);
                    
                    M3GTransform saved_cam_inv = g_m3g.camera_inverse;
                    g_m3g.camera_inverse = branch_modelview;
                    
                    JavaArray* sub_children = (JavaArray*)m3g_get_ref_field(child, "children");
                    if (sub_children && sub_children->length > 0 && sub_children->element_type == DESC_OBJECT) {
                        JavaObject** sub_arr = (JavaObject**)array_data(sub_children);
                        for (jsize sc = 0; sc < sub_children->length; sc++) {
                            JavaObject* sub_child = sub_arr[sc];
                            if (!sub_child || !sub_child->header.clazz || !sub_child->header.clazz->class_name) continue;
                            if (strstr(sub_child->header.clazz->class_name, "Mesh") != NULL) {
                                int re = m3g_get_int_field(sub_child, "renderingEnable", 1);
                                if (re) {
                                    M3GTransform sc_transform;
                                    m3g_build_node_transform(sub_child, &sc_transform);
                                    M3GTransform sc_full;
                                    m3g_transform_multiply(&sc_full, &branch_modelview, &sc_transform);
                                    m3g_render_mesh_with_mvp(jvm, sub_child, &sc_full);
                                }
                            }
                        }
                    }
                    g_m3g.camera_inverse = saved_cam_inv;
                }
            }
        }
    }
}

/* Check if M3G scene was set up but not rendered - caller should force render.
 * This works even when the scene setup happens from a game thread while
 * paint() only draws 2D overlay. */
bool m3g_needs_force_render(void) {
    fprintf(stderr, "[M3G-FORCE-DBG] scene_done=%d, bindtarget=%d, render_done=%d, world=%p\n",
            (int)g_m3g_scene_setup_done, (int)g_m3g_bindtarget_called, 
            (int)g_m3g_render_done, (void*)g_m3g_last_world);
    return g_m3g_scene_setup_done && !g_m3g_bindtarget_called && !g_m3g_render_done;
}

/* Forward declarations for force-render (defined after rasterizer) */
static void m3g_context_init(int width, int height);
static void m3g_clear(float r, float g, float b, float a, float depth);
static void m3g_render_pool_reset(void);
static void m3g_transform_identity(M3GTransform* t);
static void m3g_transform_look_at(M3GTransform* t,
                                   float eye_x, float eye_y, float eye_z,
                                   float at_x, float at_y, float at_z,
                                   float up_x, float up_y, float up_z);
static void m3g_transform_point(float* result, const M3GTransform* t, const float* v);
static void m3g_render_node_recursive(JVM* jvm, JavaObject* node, M3GTransform* parent_modelview);
static void m3g_transform_from_rowmajor(M3GTransform* out, const float* rowmajor);
static void m3g_transform_perspective(M3GTransform* t, float fov, float aspect, float near, float far);
static void m3g_render_single_mesh(JVM* jvm, JavaObject* mesh, M3GTransform* mvp);
/* m3g_transform_multiply, m3g_render_mesh_with_mvp, m3g_build_node_transform,
 * m3g_read_java_transform — forward-declared above (before m3g_replay_pending_render) */

/* Forward declaration for M3G Object Registry (defined later in file) */
typedef struct {
    JavaObject* objects[4096];
    int userIDs[4096];
    int count;
} M3GObjectRegistry;
extern M3GObjectRegistry g_m3g_registry;

/* C-side Texture2D -> Image2D mapping.
 * The Texture2D Java class from the game JAR may not have an 'image' or 'imageRef'
 * field (or the fields have obfuscated names), so Java field lookup silently fails.
 * This table stores the mapping directly from the M3G file parser, bypassing the
 * Java field mechanism entirely. */
#define M3G_MAX_TEX_IMAGE_MAPS 256
typedef struct {
    JavaObject* texture_obj;   /* Texture2D Java object */
    JavaObject* image_obj;     /* Image2D Java object (resolved during linking) */
    uint32_t image_ref;        /* M3G image reference ID (set during parsing) */
} M3GTexImageMap;
static M3GTexImageMap g_m3g_tex_image_map[M3G_MAX_TEX_IMAGE_MAPS];
static int g_m3g_tex_image_map_count = 0;

/* Texture build cache — avoid rebuilding the same texture every frame.
 * Keyed by Texture2D object pointer. Each entry holds a fully-built M3GTexture2D. */
#define M3G_MAX_TEXTURE_CACHE 64
typedef struct {
    JavaObject* texture_obj;   /* Texture2D Java object (cache key) */
    M3GTexture2D* built_tex;   /* Built native texture (NULL if build failed) */
    int build_attempted;       /* 1 if we tried building (even if failed) */
} M3GTextureCacheEntry;
static M3GTextureCacheEntry g_m3g_texture_cache[M3G_MAX_TEXTURE_CACHE];
static int g_m3g_texture_cache_count = 0;

/* Clear texture cache when a new M3G file is loaded */
static void m3g_clear_texture_cache(void) {
    for (int i = 0; i < g_m3g_texture_cache_count; i++) {
        if (g_m3g_texture_cache[i].built_tex) {
            if (g_m3g_texture_cache[i].built_tex->pixels)
                free(g_m3g_texture_cache[i].built_tex->pixels);
            free(g_m3g_texture_cache[i].built_tex);
        }
    }
    g_m3g_texture_cache_count = 0;
}

/* Look up a built texture from the cache */
static M3GTexture2D* m3g_lookup_texture_cache(JavaObject* texture_obj) {
    for (int i = 0; i < g_m3g_texture_cache_count; i++) {
        if (g_m3g_texture_cache[i].texture_obj == texture_obj) {
            return g_m3g_texture_cache[i].built_tex;
        }
    }
    return NULL;
}

/* Store a built texture in the cache */
static void m3g_store_texture_cache(JavaObject* texture_obj, M3GTexture2D* built_tex) {
    for (int i = 0; i < g_m3g_texture_cache_count; i++) {
        if (g_m3g_texture_cache[i].texture_obj == texture_obj) {
            g_m3g_texture_cache[i].built_tex = built_tex;
            g_m3g_texture_cache[i].build_attempted = 1;
            return;
        }
    }
    if (g_m3g_texture_cache_count < M3G_MAX_TEXTURE_CACHE) {
        g_m3g_texture_cache[g_m3g_texture_cache_count].texture_obj = texture_obj;
        g_m3g_texture_cache[g_m3g_texture_cache_count].built_tex = built_tex;
        g_m3g_texture_cache[g_m3g_texture_cache_count].build_attempted = 1;
        g_m3g_texture_cache_count++;
    }
}

/* Check if we already attempted building this texture (even if it failed) */
static int m3g_texture_build_attempted(JavaObject* texture_obj) {
    for (int i = 0; i < g_m3g_texture_cache_count; i++) {
        if (g_m3g_texture_cache[i].texture_obj == texture_obj) {
            return g_m3g_texture_cache[i].build_attempted;
        }
    }
    return 0;
}

/* C-side texture-image mapping functions */
static void m3g_store_tex_image_ref(JavaObject* texture_obj, uint32_t image_ref) {
    if (!texture_obj) return;
    if (g_m3g_tex_image_map_count >= M3G_MAX_TEX_IMAGE_MAPS) return;
    for (int i = 0; i < g_m3g_tex_image_map_count; i++) {
        if (g_m3g_tex_image_map[i].texture_obj == texture_obj) {
            g_m3g_tex_image_map[i].image_ref = image_ref;
            return;
        }
    }
    g_m3g_tex_image_map[g_m3g_tex_image_map_count].texture_obj = texture_obj;
    g_m3g_tex_image_map[g_m3g_tex_image_map_count].image_obj = NULL;
    g_m3g_tex_image_map[g_m3g_tex_image_map_count].image_ref = image_ref;
    g_m3g_tex_image_map_count++;
}

/* Resolve deferred image references after all M3G objects are parsed */
static void m3g_resolve_tex_image_refs(JavaObject** objects, int object_count) {
    for (int i = 0; i < g_m3g_tex_image_map_count; i++) {
        if (g_m3g_tex_image_map[i].image_obj) continue;
        uint32_t ref = g_m3g_tex_image_map[i].image_ref;
        if (ref > 0) {
            int idx = (int)ref - 2;
            if (idx >= 0 && idx < object_count && objects[idx]) {
                g_m3g_tex_image_map[i].image_obj = objects[idx];
            } else {
                for (int r = 0; r < g_m3g_registry.count; r++) {
                    if (g_m3g_registry.userIDs[r] == (int)ref) {
                        g_m3g_tex_image_map[i].image_obj = g_m3g_registry.objects[r];
                        break;
                    }
                }
            }
            if (g_m3g_tex_image_map[i].image_obj) {
                fprintf(stderr, "[M3G-TEX-RESOLVE] Texture2D %p -> Image2D %p (ref=%u)\n",
                        (void*)g_m3g_tex_image_map[i].texture_obj,
                        (void*)g_m3g_tex_image_map[i].image_obj, ref);
            }
        }
    }
}

/* Store a resolved Texture2D -> Image2D mapping */
static void m3g_store_tex_image_map(JavaObject* texture_obj, JavaObject* image_obj) {
    if (!texture_obj || !image_obj) return;
    if (g_m3g_tex_image_map_count >= M3G_MAX_TEX_IMAGE_MAPS) return;
    for (int i = 0; i < g_m3g_tex_image_map_count; i++) {
        if (g_m3g_tex_image_map[i].texture_obj == texture_obj) {
            g_m3g_tex_image_map[i].image_obj = image_obj;
            return;
        }
    }
    g_m3g_tex_image_map[g_m3g_tex_image_map_count].texture_obj = texture_obj;
    g_m3g_tex_image_map[g_m3g_tex_image_map_count].image_obj = image_obj;
    g_m3g_tex_image_map[g_m3g_tex_image_map_count].image_ref = 0;
    g_m3g_tex_image_map_count++;
}

/* Look up the Image2D for a Texture2D from the C-side mapping */
static JavaObject* m3g_lookup_tex_image(JavaObject* texture_obj) {
    if (!texture_obj) return NULL;
    for (int i = 0; i < g_m3g_tex_image_map_count; i++) {
        if (g_m3g_tex_image_map[i].texture_obj == texture_obj) {
            return g_m3g_tex_image_map[i].image_obj;
        }
    }
    return NULL;
}

/* Clear the texture-image mapping (called when loading a new M3G file) */
static void m3g_clear_tex_image_map(void) {
    g_m3g_tex_image_map_count = 0;
}

/* Force-render the last M3G world onto the screen framebuffer.
 * Called from midp_process_repaints() after paint() when the game
 * did scene setup but never called bindTarget/render/releaseTarget. */
void m3g_force_render(JVM* jvm, MidpGraphics* screen_gfx) {
    if (!g_m3g_scene_setup_done || g_m3g_bindtarget_called) return;
    if (!g_m3g_last_world || !screen_gfx || !screen_gfx->pixels) return;
    
    fprintf(stderr, "[M3G] Force-rendering World %p (game didn't call bindTarget)\n", 
            (void*)g_m3g_last_world);
    
    /* DON'T reset scene_done here — let force-render repeat every frame
     * until the game calls bindTarget (which sets bindtarget_called=true
     * and suppresses force-render via m3g_needs_force_render).
     * Previously resetting here caused force-render to run only once,
     * missing subsequent transform updates from the game. */
    /* g_m3g_scene_setup_done = false; */
    
    /* Initialize M3G context for screen rendering */
    m3g_context_init(screen_gfx->width, screen_gfx->height);
    if (!g_m3g.buffers_allocated) return;
    
    /* Reset render pool for this frame to prevent pool exhaustion across
     * multiple force-render calls (the pool is never reset otherwise,
     * causing OOM when large M3G files are force-rendered repeatedly). */
    m3g_render_pool_reset();
    
    g_m3g.target_gfx = screen_gfx;
    g_m3g.target_is_graphics = 1;
    
    /* FIX: Enable depth testing for force-render. The m3g_context_init resets
     * render states to defaults, but we need to ensure depth test and write
     * are enabled for correct 3D rendering. Previously, some code paths
     * (e.g., in Appearance processing) might disable depth testing. */
    g_m3g.depth_test_enabled = 1;
    g_m3g.depth_write_enabled = 1;
    
    /* FIX 30: Set viewport to full screen BEFORE recomputing projection */
    g_m3g.viewport_x = 0;
    g_m3g.viewport_y = 0;
    g_m3g.viewport_width = screen_gfx->width;
    g_m3g.viewport_height = screen_gfx->height;
    
    /* FIX 30: Recompute projection with CORRECT viewport aspect ratio.
     * The game called setPerspective(fov, 0, near, far) with aspect=0,
     * and setCamera used aspect=1.0 (fallback). Now that we have the real
     * viewport dimensions, recompute projection for correct FOV scaling.
     * Also prefer World's activeCamera over g_m3g.camera (which is NULL
     * when the game uses World.setActiveCamera() instead of G3D.setCamera()). */
    {
        float correct_aspect = (float)screen_gfx->width / (float)screen_gfx->height;
        /* Check if current projection has wrong aspect (identity-like or aspect=1.0) */
        float cur_m0 = g_m3g.projection.m[0];
        float cur_m5 = g_m3g.projection.m[5];
        int is_identity_like = (fabsf(cur_m0 - 1.0f) < 0.01f && fabsf(cur_m5 - 1.0f) < 0.01f);
        if (is_identity_like || cur_m0 <= 0.0f) {
            /* Projection is identity or wrong - recompute with correct aspect */
            float fov = 60.0f, near = 1.0f, far = 1000.0f;
            /* FIX 33: Try World's activeCamera first, then g_m3g.camera fallback */
            JavaObject* cam = m3g_get_ref_field(g_m3g_last_world, "activeCamera");
            if (!cam) cam = g_m3g.camera;
            if (cam) {
                fov = m3g_get_float_field(cam, "fov", 60.0f);
                near = m3g_get_float_field(cam, "near", 1.0f);
                far = m3g_get_float_field(cam, "far", 1000.0f);
            }
            m3g_transform_perspective(&g_m3g.projection, fov, correct_aspect, near, far);
            fprintf(stderr, "[M3G-FORCE] Recomputed projection: fov=%.1f aspect=%.2f near=%.1f far=%.1f\n",
                    fov, correct_aspect, near, far);
        }
    }
    
    /* Get background color from World */
    JavaObject* background = m3g_get_ref_field(g_m3g_last_world, "background");
    float bg_r = 0.0f, bg_g = 0.0f, bg_b = 0.0f, bg_a = 1.0f;
    if (background) {
        int clear_color = m3g_get_int_field(background, "clearColor", 0xFF000000);
        bg_a = ((clear_color >> 24) & 0xFF) / 255.0f;
        bg_r = ((clear_color >> 16) & 0xFF) / 255.0f;
        bg_g = ((clear_color >> 8) & 0xFF) / 255.0f;
        bg_b = (clear_color & 0xFF) / 255.0f;
    }
    m3g_clear(bg_r, bg_g, bg_b, bg_a, 1.0f);
    
    /* FIX 36: Render Background image as fullscreen quad (sky/ground).
     * M3G Background can have an image that should be displayed behind the 3D scene.
     * Previously we only used Background.clearColor for clearing, ignoring the image.
     * Many games (including SU-30) store the sky/ground texture in Background.image.
     * We blit it directly to the framebuffer as a fullscreen image. */
    if (background) {
        JavaObject* bg_image = m3g_get_ref_field(background, "image");
        if (bg_image) {
            int bg_width = m3g_get_int_field(bg_image, "width", 0);
            int bg_height = m3g_get_int_field(bg_image, "height", 0);
            JavaArray* bg_pixels = (JavaArray*)m3g_get_ref_field(bg_image, "pixels");
            
            if (bg_width > 0 && bg_height > 0 && bg_pixels &&
                bg_pixels->element_type == T_INT &&
                (int)bg_pixels->length >= bg_width * bg_height) {
                
                /* Blit background image stretched to fill the framebuffer.
                 * Use nearest-neighbor sampling for pixel-perfect scaling. */
                jint* src_px = (jint*)array_data(bg_pixels);
                int fb_w = g_m3g.buffer_width;
                int fb_h = g_m3g.buffer_height;
                uint32_t* dst = g_m3g.color_buffer;
                
                for (int py = 0; py < fb_h; py++) {
                    int src_y = (py * bg_height) / fb_h;
                    if (src_y >= bg_height) src_y = bg_height - 1;
                    int src_row_off = src_y * bg_width;
                    int dst_row_off = py * fb_w;
                    
                    for (int px = 0; px < fb_w; px++) {
                        int src_x = (px * bg_width) / fb_w;
                        if (src_x >= bg_width) src_x = bg_width - 1;
                        uint32_t pixel = (uint32_t)src_px[src_row_off + src_x];
                        /* Skip fully transparent pixels */
                        if ((pixel >> 24) != 0) {
                            dst[dst_row_off + px] = pixel;
                        }
                    }
                }
                fprintf(stderr, "[M3G-BG] Rendered background image %dx%d -> %dx%d framebuffer\n",
                        bg_width, bg_height, fb_w, fb_h);
            } else {
                fprintf(stderr, "[M3G-BG] Background image has invalid params: %dx%d pixels=%p len=%d\n",
                        bg_width, bg_height, (void*)bg_pixels,
                        bg_pixels ? (int)bg_pixels->length : -1);
            }
        }
    }
    
    /* DIAG: Log World's class hierarchy and field layout for debugging */
    {
        JavaObject* w = g_m3g_last_world;
        if (w && w->header.clazz) {
            JavaClass* wc = w->header.clazz;
            int max_slots_w = (int)((wc->instance_size - sizeof(ObjectHeader)) / sizeof(JavaValue));
            fprintf(stderr, "[M3G-FORCE-DIAG] World class=%s instance_size=%zu max_slots=%d fields_count=%d\n",
                    wc->class_name ? wc->class_name : "?", wc->instance_size, max_slots_w,
                    wc->fields_count);
            /* Dump all fields in hierarchy to find children slot */
            JavaClass* chain[64]; int depth_w = 0;
            JavaClass* cc = wc;
            while (cc && depth_w < 64) { chain[depth_w++] = cc; cc = cc->super_class; }
            int slot_w = 0;
            for (int h = depth_w - 1; h >= 0; h--) {
                JavaClass* cur = chain[h];
                if (!cur->fields) continue;
                for (int i = 0; i < cur->fields_count; i++) {
                    JavaField* f = &cur->fields[i];
                    if (f->access_flags & ACC_STATIC) continue;
                    if (f->name) {
                        fprintf(stderr, "[M3G-FORCE-DIAG]   slot %d: %s %s (ref=%p)\n",
                                slot_w, f->name, f->descriptor ? f->descriptor : "?",
                                (void*)w->fields[slot_w].ref);
                    }
                    slot_w++;
                    if (f->descriptor && (f->descriptor[0] == 'J' || f->descriptor[0] == 'D')) slot_w++;
                }
            }
        }
    }

    /* Set up camera from World */
    float cam_fov = 60.0f, cam_near_val = 1.0f, cam_far_val = 1000.0f; /* corrected values */
    JavaObject* camera = m3g_get_ref_field(g_m3g_last_world, "activeCamera");
    if (camera) {
        float fov = m3g_get_float_field(camera, "fov", 0.0f);
        float aspect = (float)screen_gfx->width / (float)screen_gfx->height;
        float near = m3g_get_float_field(camera, "near", 0.0f);
        float far = m3g_get_float_field(camera, "far", 0.0f);
        int proj_type = m3g_get_int_field(camera, "projectionType", 48);
        
        /* FIX 34: Many games set camera params lazily (after first frame).
         * When fov/near/far are all 0, the camera hasn't been configured yet.
         * Use sensible defaults instead of producing NaN projections. */
        if (fov <= 0.0f || fov > 170.0f) fov = 60.0f;  /* reject garbage fov */
        if (near <= 0.0f) near = 1.0f;
        if (far <= near) far = 1000.0f;
        cam_fov = fov; cam_near_val = near; cam_far_val = far; /* save corrected */
        
        if (proj_type == 48) { /* GENERIC */
            /* FIX: Read generic projection matrix from "genericMatrix" field,
             * NOT from "transform" field. The "transform" field stores the
             * camera's world-space position/orientation (Node transform),
             * while "genericMatrix" stores the 4x4 projection matrix.
             * Previously we read "transform" and used it as projection,
             * causing wildly wrong projection (since it's the camera's
             * world position, not a projection matrix). */
            JavaArray* gen_matrix = (JavaArray*)m3g_get_ref_field(camera, "genericMatrix");
            if (gen_matrix && gen_matrix->element_type == T_FLOAT && gen_matrix->length >= 16) {
                float* gm = (float*)array_data(gen_matrix);
                /* Generic matrix from M3G file is row-major; convert to column-major */
                m3g_transform_from_rowmajor(&g_m3g.projection, gm);
                fprintf(stderr, "[M3G-FORCE] Using generic projection matrix from camera\n");
                /* For generic projection, compute fov from the matrix for logging */
                if (fabsf(gm[0]) > 0.001f) {
                    float implied_fov = 2.0f * atanf(1.0f / gm[0]) * 57.2958f;
                    fprintf(stderr, "[M3G-FORCE] Generic matrix implies fov=%.1f\n", implied_fov);
                }
            } else {
                /* Fallback to perspective if genericMatrix not available */
                m3g_transform_perspective(&g_m3g.projection, fov, aspect, near, far);
                fprintf(stderr, "[M3G-FORCE] GENERIC camera but no genericMatrix, using perspective fov=%.1f\n", fov);
            }
        } else {
            /* PERSPECTIVE or PARALLEL */
            /* FIX: Validate fov to prevent garbage projections.
             * Some M3G files or game code may set absurd fov values
             * (e.g., 6351.8 degrees). Clamp to reasonable range. */
            float clamped_fov = fov;
            if (clamped_fov < 10.0f) clamped_fov = 10.0f;
            if (clamped_fov > 170.0f) clamped_fov = 60.0f;
            if (clamped_fov != fov) {
                fprintf(stderr, "[M3G-FORCE] Clamped fov from %.1f to %.1f\n", fov, clamped_fov);
            }
            m3g_transform_perspective(&g_m3g.projection, clamped_fov, aspect, near, far);
        }
        
        fprintf(stderr, "[M3G] Force-render camera: fov=%.1f aspect=%.2f near=%.1f far=%.1f\n",
                fov, aspect, near, far);
    } else {
        /* Default perspective projection */
        float aspect = (float)screen_gfx->width / (float)screen_gfx->height;
        m3g_transform_perspective(&g_m3g.projection, 60.0f, aspect, 1.0f, 1000.0f);
    }
    
    /* FIX 45: Force-render must ALWAYS use auto-camera, ignoring the game's
     * camera_inverse. The game's camera is positioned for its own coordinate
     * system (e.g., camera at millions of units). When force-rendering from
     * the registry, the game's camera_inverse places scene meshes at clip.w=0
     * or clip.w<0 (behind camera), causing ALL triangles to be clipped.
     * Instead, compute the scene bounding box from registry meshes and
     * place the camera to actually see the scene. */
    g_m3g.camera_set = 0;  /* Force auto-camera path */
    
    if (0) {  /* g_m3g.camera_set — disabled for force-render */
        g_m3g.modelview = g_m3g.camera_inverse;
        fprintf(stderr, "[M3G-FORCE] Using camera_inverse: %.3f,%.3f,%.3f,%.3f / %.3f,%.3f,%.3f,%.3f\n",
            g_m3g.camera_inverse.m[0], g_m3g.camera_inverse.m[4], g_m3g.camera_inverse.m[8], g_m3g.camera_inverse.m[12],
            g_m3g.camera_inverse.m[1], g_m3g.camera_inverse.m[5], g_m3g.camera_inverse.m[9], g_m3g.camera_inverse.m[13]);
    } else if (camera) {
        /* FIX 33: The game uses World.setActiveCamera() instead of calling
         * Graphics3D.setCamera(), so g_m3g.camera_set is never true.
         * Compute the camera's view matrix from its transform (position/orientation
         * set by the game or from the M3G file). Without this, the modelview is
         * identity, placing the camera at origin looking down -Z, while all scene
         * objects are at positive Z — behind the camera — causing all triangles
         * to be clipped to 0. */
        M3GTransform cam_transform;
        m3g_build_node_transform(camera, &cam_transform);
        
        /* DIAG: Log camera's raw transform for debugging */
        fprintf(stderr, "[M3G-FORCE] Camera %p transform: tx=%.1f ty=%.1f tz=%.1f ang=%.1f ax=%.1f ay=%.1f az=%.1f\n",
                (void*)camera,
                cam_transform.m[12], cam_transform.m[13], cam_transform.m[14],
                m3g_get_float_field(camera, "orientationAngle", 0.0f),
                m3g_get_float_field(camera, "orientationX", 0.0f),
                m3g_get_float_field(camera, "orientationY", 0.0f),
                m3g_get_float_field(camera, "orientationZ", 0.0f));
        
        /* Check if the camera has a non-identity transform.
         * If identity, the camera is at origin looking down -Z, and we need
         * to check if the scene objects are in front of it.
         *
         * FIX: Also check if the camera transform has GARBAGE values
         * (very large translations, typical when M3G file parsing is
         * partially wrong). If camera translation is unreasonably large
         * (>10000 units), the scene is clearly not designed for this
         * camera position — fall through to auto-camera. */
        float cam_tx = fabsf(cam_transform.m[12]);
        float cam_ty = fabsf(cam_transform.m[13]);
        float cam_tz = fabsf(cam_transform.m[14]);
        int cam_has_transform = (cam_tx > 0.001f || cam_ty > 0.001f || cam_tz > 0.001f ||
                                fabsf(cam_transform.m[0] - 1.0f) > 0.001f ||
                                fabsf(cam_transform.m[5] - 1.0f) > 0.001f ||
                                fabsf(cam_transform.m[10] - 1.0f) > 0.001f);
        
        /* Detect garbage camera transform: if translation is huge (>100K),
         * the camera is clearly positioned incorrectly for our force-render.
         * Fall through to auto-camera which computes proper position. */
        int cam_has_garbage_transform = (cam_tx > 100000.0f || cam_ty > 100000.0f || cam_tz > 100000.0f);
        if (cam_has_garbage_transform) {
            fprintf(stderr, "[M3G-FORCE] Camera has GARBAGE transform (%.0f, %.0f, %.0f), falling through to auto-camera\\n",
                    cam_transform.m[12], cam_transform.m[13], cam_transform.m[14]);
            cam_has_transform = 0;  /* Force auto-camera path */
        }
        
        if (cam_has_transform) {
            /* Compute inverse of camera's world transform for view matrix.
             * For a rigid body transform (rotation + translation only, no scale),
             * inverse = transpose rotation * negate translation. */
            M3GTransform cam_inv;
            m3g_transform_identity(&cam_inv);
            
            /* Transpose rotation part (upper-left 3x3) */
            for (int row = 0; row < 3; row++) {
                for (int col = 0; col < 3; col++) {
                    cam_inv.m[row * 4 + col] = cam_transform.m[col * 4 + row];
                }
            }
            
            /* Apply inverse translation: -R^T * T */
            float tx = -cam_transform.m[12];
            float ty = -cam_transform.m[13];
            float tz = -cam_transform.m[14];
            cam_inv.m[12] = cam_inv.m[0] * tx + cam_inv.m[4] * ty + cam_inv.m[8] * tz;
            cam_inv.m[13] = cam_inv.m[1] * tx + cam_inv.m[5] * ty + cam_inv.m[9] * tz;
            cam_inv.m[14] = cam_inv.m[2] * tx + cam_inv.m[6] * ty + cam_inv.m[10] * tz;
            
            g_m3g.camera_inverse = cam_inv;
            g_m3g.camera_transform = cam_transform;
            g_m3g.modelview = cam_inv;
            g_m3g.camera = camera;
            g_m3g.camera_set = 1;
            fprintf(stderr, "[M3G-FORCE] Computed camera view matrix from World's activeCamera:\n");
            fprintf(stderr, "[M3G-FORCE]   cam_pos=(%.1f, %.1f, %.1f)\n",
                    cam_transform.m[12], cam_transform.m[13], cam_transform.m[14]);
            fprintf(stderr, "[M3G-FORCE]   view: %.3f,%.3f,%.3f,%.3f / %.3f,%.3f,%.3f,%.3f\n",
                    cam_inv.m[0], cam_inv.m[4], cam_inv.m[8], cam_inv.m[12],
                    cam_inv.m[1], cam_inv.m[5], cam_inv.m[9], cam_inv.m[13]);
            
            /* NOTE: Camera adaptation removed. The M3G World's activeCamera already
             * has the correct position/orientation set by the game. Previously, this
             * code computed scene bounds from LOCAL-SPACE vertices (ignoring Group
             * transforms), concluded the scene was at millions of units, and repositioned
             * the camera to absurd coordinates — causing NaN UV coords and crashes.
             * The game's camera is the authoritative view; just use it as-is. */
        } else {
            /* Camera has identity transform - place it at a default position
             * that can see the scene. Scan actual mesh vertex positions from
             * the M3G registry for an accurate bounding box (top-level children
             * are usually Groups at origin, not the actual Mesh geometry). */
            fprintf(stderr, "[M3G-FORCE] Camera has identity transform, computing view from mesh vertex bounds\n");
            
            /* Compute scene bounding box from actual mesh vertex positions */
            float scene_min[3] = {1e30f, 1e30f, 1e30f};
            float scene_max[3] = {-1e30f, -1e30f, -1e30f};
            int meshes_scanned = 0;
            
            /* First: scan meshes from the M3G registry (contains all loaded objects).
             * Include ALL meshes (comp=2 and comp=3) for bounding box computation.
             * Some games (like SU-30) only have comp=2 meshes that represent the
             * main scene geometry in world-space coordinates. */
            for (int i = 0; i < g_m3g_registry.count && meshes_scanned < 30; i++) {
                JavaObject* obj = g_m3g_registry.objects[i];
                if (!obj || !obj->header.clazz || !obj->header.clazz->class_name) continue;
                const char* cn = obj->header.clazz->class_name;
                if (strstr(cn, "Mesh") == NULL) continue;
                
                JavaObject* vb = m3g_get_ref_field(obj, "vertexBuffer");
                if (!vb) continue;
                JavaObject* positions = m3g_get_ref_field(vb, "positions");
                if (!positions) continue;
                
                int vc = m3g_get_int_field(positions, "vertexCount", 0);
                int comp = m3g_get_int_field(positions, "componentCount", 3);
                if (vc <= 0 || comp < 1) continue;
                
                float scale = m3g_get_float_field(vb, "positionScale", 1.0f);
                float bx = m3g_get_float_field(vb, "biasX", 0.0f);
                float by = m3g_get_float_field(vb, "biasY", 0.0f);
                float bz = m3g_get_float_field(vb, "biasZ", 0.0f);
                
                JavaArray* pos_data = (JavaArray*)m3g_get_ref_field(positions, "data");
                if (!pos_data) continue;
                
                /* Sample up to 20 vertices per mesh for bounding box */
                int sample_count = vc < 20 ? vc : 20;
                int step = vc > 20 ? vc / 20 : 1;
                
                if (pos_data->element_type == T_SHORT || pos_data->element_type == T_BYTE) {
                    int is_short = (pos_data->element_type == T_SHORT);
                    for (int vi = 0; vi < sample_count; vi++) {
                        int idx = vi * step * comp;
                        if (idx + 2 >= (int)pos_data->length) break;
                        float px, py, pz = 0.0f;
                        if (is_short) {
                            int16_t* src = (int16_t*)array_data(pos_data);
                            px = (float)src[idx] * scale + bx;
                            py = (float)src[idx+1] * scale + by;
                            pz = (comp >= 3) ? (float)src[idx+2] * scale + bz : 0.0f;
                        } else {
                            int8_t* src = (int8_t*)array_data(pos_data);
                            px = (float)src[idx] * scale + bx;
                            py = (float)src[idx+1] * scale + by;
                            pz = (comp >= 3) ? (float)src[idx+2] * scale + bz : 0.0f;
                        }
                        /* Include all vertices for bounding box */
                        if (px < scene_min[0]) scene_min[0] = px;
                        if (py < scene_min[1]) scene_min[1] = py;
                        if (pz < scene_min[2]) scene_min[2] = pz;
                        if (px > scene_max[0]) scene_max[0] = px;
                        if (py > scene_max[1]) scene_max[1] = py;
                        if (pz > scene_max[2]) scene_max[2] = pz;
                    }
                }
                meshes_scanned++;
            }
            
            /* FIX 36: Handle empty registry (no meshes loaded yet).
             * When meshes_scanned=0, bbox was never updated and contains
             * 1e30/-1e30 defaults. Use a reasonable default scene. */
            int has_valid_bbox = (scene_min[0] < scene_max[0]);
            if (!has_valid_bbox || meshes_scanned == 0) {
                /* No meshes in registry — use a default view position.
                 * The scene might load later; this is just for initial frames. */
                float eye[3] = { 0.0f, 0.0f, 200.0f };
                float at[3]  = { 0.0f, 0.0f, 0.0f };
                float up[3]  = { 0.0f, 1.0f, 0.0f };
                
                m3g_transform_look_at(&g_m3g.modelview,
                                      eye[0], eye[1], eye[2],
                                      at[0], at[1], at[2],
                                      up[0], up[1], up[2]);
                g_m3g.camera_inverse = g_m3g.modelview;
                g_m3g.camera_set = 1;
                fprintf(stderr, "[M3G-FORCE] Auto-camera: no meshes found, using default eye=(0,0,200)\n");
            } else {
            
            fprintf(stderr, "[M3G-FORCE] Scanned %d meshes, bbox: (%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f)\n",
                    meshes_scanned, scene_min[0], scene_min[1], scene_min[2],
                    scene_max[0], scene_max[1], scene_max[2]);
            
            float center[3] = {
                (scene_min[0] + scene_max[0]) * 0.5f,
                (scene_min[1] + scene_max[1]) * 0.5f,
                (scene_min[2] + scene_max[2]) * 0.5f
            };
            float extent = 0.0f;
            for (int i = 0; i < 3; i++) {
                float d = scene_max[i] - scene_min[i];
                if (d > extent) extent = d;
            }
            if (extent < 1.0f) extent = 100.0f;
            
            /* Place camera behind the scene center, looking at it.
             * FIX 39: When scene extent is much larger than camera near/far
             * (e.g., comp=2 meshes with world-space coords in the millions),
             * adapt near/far to the actual view distance. The M3G camera's
             * near/far are tuned for the game's own camera position, not for
             * our auto-positioned camera millions of units away. */
            float view_dist = extent * 1.2f;
            float cam_near = cam_near_val;
            float cam_far = cam_far_val;
            
            /* Adapt near/far when view_dist is much larger than far plane */
            if (view_dist > cam_far * 1.5f) {
                cam_near = view_dist * 0.01f;
                cam_far = view_dist * 3.0f;
                /* Recompute perspective projection with corrected near/far */
                float correct_aspect = (float)g_m3g.buffer_width / (float)g_m3g.buffer_height;
                if (correct_aspect <= 0.001f) correct_aspect = 0.75f;
                float fov = m3g_get_float_field(camera, "fov", 45.0f);
                if (fov <= 0.0f || fov > 170.0f) fov = 45.0f;  /* reject garbage fov */
                m3g_transform_perspective(&g_m3g.projection, fov, correct_aspect, cam_near, cam_far);
                fprintf(stderr, "[M3G-FORCE] Auto-camera: adapted near/far to (%.1f, %.1f) for view_dist=%.1f\n",
                        cam_near, cam_far, view_dist);
            }
            
            /* Clamp view_dist to be within camera's frustum */
            if (view_dist < cam_near + 1.0f) view_dist = cam_near + (cam_far - cam_near) * 0.1f;
            if (view_dist > cam_far * 0.9f) view_dist = cam_far * 0.9f;
            
            float eye[3] = { center[0], center[1], center[2] + view_dist };
            float at[3]  = { center[0], center[1], center[2] };
            float up[3]  = { 0.0f, 1.0f, 0.0f };
            
            m3g_transform_look_at(&g_m3g.modelview,
                                  eye[0], eye[1], eye[2],
                                  at[0], at[1], at[2],
                                  up[0], up[1], up[2]);
            g_m3g.camera_inverse = g_m3g.modelview;
            g_m3g.camera_set = 1;
            fprintf(stderr, "[M3G-FORCE] Auto-camera: eye=(%.1f,%.1f,%.1f) at=(%.1f,%.1f,%.1f) extent=%.1f view_dist=%.1f near=%.1f far=%.1f\n",
                    eye[0], eye[1], eye[2], at[0], at[1], at[2], extent, view_dist, cam_near, cam_far);
            }
        }
    } else {
        m3g_transform_identity(&g_m3g.modelview);
        fprintf(stderr, "[M3G-FORCE] camera_set=false, no camera found, using identity modelview\n");
    }
    
    /* NOTE: FIX40 scene-bbox adaptation removed. It computed LOCAL-SPACE vertex
     * bounds (ignoring Group/Node transforms in the scene graph), incorrectly
     * concluded the scene was millions of units away, and recomputed projection
     * with absurd near/far. The game's camera projection is already correct;
     * the Group transforms place local vertices into the camera's view frustum. */
    
    /* Collect lights from World's children */
    g_m3g.light_count = 0;
    
    /* Render the world using camera modelview */
    g_m3g.triangles_rendered = 0;
    g_m3g.vertices_processed = 0;
    
    m3g_render_node_recursive(jvm, g_m3g_last_world, &g_m3g.modelview);
    
    /* Replay any pending render(Node, Transform) calls that were queued when
     * the game called render() without bindTarget. These use the game's own
     * transforms and node tree, giving much better results than the registry
     * fallback (which uses identity modelview). */
    if (g_m3g.triangles_rendered == 0 && g_m3g_pending_render_count > 0) {
        fprintf(stderr, "[M3G] Force-render: replaying %d pending render() calls\n",
                g_m3g_pending_render_count);
        for (int i = 0; i < g_m3g_pending_render_count && i < M3G_MAX_PENDING_RENDERS; i++) {
            JavaObject* node = g_m3g_pending_renders[i].node;
            JavaObject* transform = g_m3g_pending_renders[i].transform;
            if (node && transform) {
                fprintf(stderr, "[M3G] Force-render: replaying pending render[%d] node=%p class=%s\n",
                        i, (void*)node,
                        node->header.clazz ? node->header.clazz->class_name : "?");
                m3g_replay_pending_render(jvm, node, transform);
            }
        }
        fprintf(stderr, "[M3G] Force-render: pending renders done, total triangles=%d\n",
                g_m3g.triangles_rendered);
    }
    
    /* If World render AND pending renders produced 0 triangles, scan the M3G registry
     * for ALL Mesh objects and render them directly. This is the last-resort fallback
     * that uses the mesh's own transform (no game-supplied transform). */
    if (g_m3g.triangles_rendered == 0 && g_m3g_registry.count > 0) {
        fprintf(stderr, "[M3G] Force-render: World gave 0 triangles, scanning registry for Meshes (%d objects)\n",
                g_m3g_registry.count);
        
        /* FIX: Set up auto-camera from registry mesh bounding box.
         * When World is NULL, the camera setup from the World path is skipped.
         * Compute scene bbox from registry meshes and position camera. */
        if (!g_m3g.camera_set) {
            float scene_min[3] = {1e30f, 1e30f, 1e30f};
            float scene_max[3] = {-1e30f, -1e30f, -1e30f};
            int meshes_scanned = 0;
            
            for (int i = 0; i < g_m3g_registry.count && meshes_scanned < 30; i++) {
                JavaObject* obj = g_m3g_registry.objects[i];
                if (!obj || !obj->header.clazz || !obj->header.clazz->class_name) continue;
                if (strstr(obj->header.clazz->class_name, "Mesh") == NULL) continue;
                
                JavaObject* vb = m3g_get_ref_field(obj, "vertexBuffer");
                if (!vb) continue;
                JavaObject* positions = m3g_get_ref_field(vb, "positions");
                if (!positions) continue;
                
                int vc = m3g_get_int_field(positions, "vertexCount", 0);
                int comp = m3g_get_int_field(positions, "componentCount", 3);
                if (vc <= 0 || comp < 1) continue;
                
                float scale = m3g_get_float_field(vb, "positionScale", 1.0f);
                float bx = m3g_get_float_field(vb, "biasX", 0.0f);
                float by = m3g_get_float_field(vb, "biasY", 0.0f);
                float bz = m3g_get_float_field(vb, "biasZ", 0.0f);
                
                JavaArray* pos_data = (JavaArray*)m3g_get_ref_field(positions, "data");
                if (!pos_data) continue;
                
                int sample_count = vc < 20 ? vc : 20;
                int step = vc > 20 ? vc / 20 : 1;
                
                if (pos_data->element_type == T_SHORT || pos_data->element_type == T_BYTE) {
                    int is_short = (pos_data->element_type == T_SHORT);
                    for (int vi = 0; vi < sample_count; vi++) {
                        int idx = vi * step * comp;
                        if (idx + 2 >= (int)pos_data->length) break;
                        float px, py, pz = 0.0f;
                        if (is_short) {
                            int16_t* src = (int16_t*)array_data(pos_data);
                            px = (float)src[idx] * scale + bx;
                            py = (float)src[idx+1] * scale + by;
                            pz = (comp >= 3) ? (float)src[idx+2] * scale + bz : 0.0f;
                        } else {
                            int8_t* src = (int8_t*)array_data(pos_data);
                            px = (float)src[idx] * scale + bx;
                            py = (float)src[idx+1] * scale + by;
                            pz = (comp >= 3) ? (float)src[idx+2] * scale + bz : 0.0f;
                        }
                        if (px < scene_min[0]) scene_min[0] = px;
                        if (py < scene_min[1]) scene_min[1] = py;
                        if (pz < scene_min[2]) scene_min[2] = pz;
                        if (px > scene_max[0]) scene_max[0] = px;
                        if (py > scene_max[1]) scene_max[1] = py;
                        if (pz > scene_max[2]) scene_max[2] = pz;
                    }
                }
                meshes_scanned++;
            }
            
            if (scene_min[0] < scene_max[0]) {
                float center[3] = {
                    (scene_min[0] + scene_max[0]) * 0.5f,
                    (scene_min[1] + scene_max[1]) * 0.5f,
                    (scene_min[2] + scene_max[2]) * 0.5f
                };
                float extent = 0.0f;
                for (int i = 0; i < 3; i++) {
                    float d = scene_max[i] - scene_min[i];
                    if (d > extent) extent = d;
                }
                if (extent < 1.0f) extent = 100.0f;
                
                float view_dist = extent * 1.2f;
                float cam_near = view_dist * 0.01f;
                float cam_far = view_dist * 3.0f;
                
                float correct_aspect = (float)g_m3g.buffer_width / (float)g_m3g.buffer_height;
                if (correct_aspect <= 0.001f) correct_aspect = 0.75f;
                m3g_transform_perspective(&g_m3g.projection, 45.0f, correct_aspect, cam_near, cam_far);
                
                float eye[3] = { center[0], center[1], center[2] + view_dist };
                float at[3]  = { center[0], center[1], center[2] };
                float up[3]  = { 0.0f, 1.0f, 0.0f };
                
                m3g_transform_look_at(&g_m3g.modelview,
                                      eye[0], eye[1], eye[2],
                                      at[0], at[1], at[2],
                                      up[0], up[1], up[2]);
                g_m3g.camera_inverse = g_m3g.modelview;
                g_m3g.camera_set = 1;
                
                fprintf(stderr, "[M3G-FORCE] Registry auto-camera: bbox=(%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f) extent=%.0f\n",
                        scene_min[0], scene_min[1], scene_min[2],
                        scene_max[0], scene_max[1], scene_max[2], extent);
                fprintf(stderr, "[M3G-FORCE] Registry auto-camera: eye=(%.0f,%.0f,%.0f) near=%.1f far=%.1f\n",
                        eye[0], eye[1], eye[2], cam_near, cam_far);
            }
        }
        
        int mesh_rendered = 0;
        for (int i = 0; i < g_m3g_registry.count && mesh_rendered < 50; i++) {
            JavaObject* obj = g_m3g_registry.objects[i];
            if (!obj || !obj->header.clazz || !obj->header.clazz->class_name) continue;
            const char* cn = obj->header.clazz->class_name;
            /* Log ALL registry entries to understand what's there */
            fprintf(stderr, "[M3G-REG] registry[%d] %p class=%s\n", i, (void*)obj, cn);
            if (strstr(cn, "Mesh") != NULL) {
                int render_enable = m3g_get_int_field(obj, "renderingEnable", 1);
                fprintf(stderr, "[M3G-REG] Mesh[%d]: render_enable=%d\n", i, render_enable);
                if (!render_enable) continue;
                
                /* Check if this mesh has a vertexBuffer */
                JavaObject* vb = m3g_get_ref_field(obj, "vertexBuffer");
                fprintf(stderr, "[M3G-REG] Mesh[%d]: vb=%p\n", i, (void*)vb);
                if (!vb) continue;
                JavaObject* positions = m3g_get_ref_field(vb, "positions");
                fprintf(stderr, "[M3G-REG] Mesh[%d]: positions=%p\n", i, (void*)positions);
                if (!positions) continue;
                
                fprintf(stderr, "[M3G] Force-render: rendering registry Mesh[%d] %p (userID=%d)\n",
                        i, (void*)obj, g_m3g_registry.userIDs[i]);
                
                /* Log vertex format details */
                int pos_comp = m3g_get_int_field(positions, "componentCount", -1);
                float scale = m3g_get_float_field(vb, "positionScale", 1.0f);
                float bx = m3g_get_float_field(vb, "biasX", 0.0f);
                float by = m3g_get_float_field(vb, "biasY", 0.0f);
                float bz = m3g_get_float_field(vb, "biasZ", 0.0f);
                int vc = m3g_get_int_field(positions, "vertexCount", 0);
                fprintf(stderr, "[M3G]   pos: comp=%d, verts=%d, scale=%.4f, bias=(%.1f,%.1f,%.1f)\n",
                        pos_comp, vc, scale, bx, by, bz);
                
                /* Build proper MVP using the mesh's own transform instead of identity.
                 * Previously used &g_m3g.mvp (identity modelview * projection), causing
                 * all meshes to be at origin without proper transforms. */
                M3GTransform node_transform;
                m3g_build_node_transform(obj, &node_transform);
                
                /* Combine: modelview = camera_inverse * node_transform */
                M3GTransform modelview;
                if (g_m3g.camera_set) {
                    m3g_transform_multiply(&modelview, &g_m3g.camera_inverse, &node_transform);
                } else {
                    modelview = node_transform;
                }
                
                /* Build MVP = projection * modelview (OpenGL convention) */
                M3GTransform mesh_mvp;
                m3g_transform_multiply(&mesh_mvp, &g_m3g.projection, &modelview);
                
                /* DIAG: log node_transform, modelview, projection for first mesh */
                if (mesh_rendered == 0) {
                    fprintf(stderr, "[M3G-FORCE-DIAG] node_transform: %.3f,%.3f,%.3f,%.3f / %.3f,%.3f,%.3f,%.3f / %.3f,%.3f,%.3f,%.3f / %.3f,%.3f,%.3f,%.3f\n",
                        node_transform.m[0], node_transform.m[4], node_transform.m[8], node_transform.m[12],
                        node_transform.m[1], node_transform.m[5], node_transform.m[9], node_transform.m[13],
                        node_transform.m[2], node_transform.m[6], node_transform.m[10], node_transform.m[14],
                        node_transform.m[3], node_transform.m[7], node_transform.m[11], node_transform.m[15]);
                    fprintf(stderr, "[M3G-FORCE-DIAG] projection: %.3f,%.3f,%.3f,%.3f / %.3f,%.3f,%.3f,%.3f / %.3f,%.3f,%.3f,%.3f / %.3f,%.3f,%.3f,%.3f\n",
                        g_m3g.projection.m[0], g_m3g.projection.m[4], g_m3g.projection.m[8], g_m3g.projection.m[12],
                        g_m3g.projection.m[1], g_m3g.projection.m[5], g_m3g.projection.m[9], g_m3g.projection.m[13],
                        g_m3g.projection.m[2], g_m3g.projection.m[6], g_m3g.projection.m[10], g_m3g.projection.m[14],
                        g_m3g.projection.m[3], g_m3g.projection.m[7], g_m3g.projection.m[11], g_m3g.projection.m[15]);
                    fprintf(stderr, "[M3G-FORCE-DIAG] MVP: %.3f,%.3f,%.3f,%.3f / %.3f,%.3f,%.3f,%.3f / %.3f,%.3f,%.3f,%.3f / %.3f,%.3f,%.3f,%.3f\n",
                        mesh_mvp.m[0], mesh_mvp.m[4], mesh_mvp.m[8], mesh_mvp.m[12],
                        mesh_mvp.m[1], mesh_mvp.m[5], mesh_mvp.m[9], mesh_mvp.m[13],
                        mesh_mvp.m[2], mesh_mvp.m[6], mesh_mvp.m[10], mesh_mvp.m[14],
                        mesh_mvp.m[3], mesh_mvp.m[7], mesh_mvp.m[11], mesh_mvp.m[15]);
                }
                
                m3g_render_single_mesh(jvm, obj, &mesh_mvp);
                mesh_rendered++;
            }
        }
        fprintf(stderr, "[M3G] Force-render: rendered %d Meshes from registry\n", mesh_rendered);
    }
    
    /* Copy rendered M3G buffer to screen framebuffer */
    if (g_m3g.color_buffer && g_m3g.buffers_allocated) {
        int copy_w = screen_gfx->width < g_m3g.buffer_width ? screen_gfx->width : g_m3g.buffer_width;
        int copy_h = screen_gfx->height < g_m3g.buffer_height ? screen_gfx->height : g_m3g.buffer_height;
        for (int y = 0; y < copy_h; y++) {
            for (int x = 0; x < copy_w; x++) {
                uint32_t m3g_pixel = g_m3g.color_buffer[y * g_m3g.buffer_width + x];
                int sx = x;
                int sy = y;
                if (sx >= 0 && sx < screen_gfx->width && sy >= 0 && sy < screen_gfx->height) {
                    screen_gfx->pixels[sy * screen_gfx->width + sx] = m3g_pixel;
                }
            }
        }
        fprintf(stderr, "[M3G] Force-render complete: %d triangles, %d vertices\n",
                g_m3g.triangles_rendered, g_m3g.vertices_processed);
        
        /* Save frame as BMP for visual diagnosis.
         * Controlled by M3G_CAPTURE env var:
         *   M3G_CAPTURE=N     - save first N frames (default: 10)
         *   M3G_CAPTURE=0     - disable capture
         *   M3G_CAPTURE=every  - save every 100th frame */
        {
            static int force_render_count = 0;
            static int capture_limit = -1; /* -1 = not yet initialized */
            force_render_count++;

            if (capture_limit < 0) {
                const char* env_cap = getenv("M3G_CAPTURE");
                if (env_cap) {
                    if (strcmp(env_cap, "every") == 0) capture_limit = 999999;
                    else capture_limit = atoi(env_cap);
                } else {
                    capture_limit = 10;
                }
                if (capture_limit > 0) {
                    fprintf(stderr, "[M3G-CAP] Frame capture enabled: limit=%d (set M3G_CAPTURE to change)\n", capture_limit);
                }
            }

            int should_capture = 0;
            if (capture_limit == 999999) {
                should_capture = (force_render_count % 100 == 0); /* every 100th frame */
            } else if (capture_limit > 0) {
                should_capture = (force_render_count <= capture_limit);
            }

            if (should_capture && screen_gfx->pixels) {
                char fname[256];
                snprintf(fname, sizeof(fname), "/home/z/my-project/download/m3g_frame_%05d.bmp", force_render_count);
                int w = screen_gfx->width;
                int h = screen_gfx->height;
                int row_size = (w * 3 + 3) & ~3; /* BMP rows padded to 4 bytes */
                int pixel_data_size = row_size * h;
                int file_size = 54 + pixel_data_size;

                FILE* f = fopen(fname, "wb");
                if (f) {
                    /* BMP Header (14 bytes) */
                    uint8_t hdr[14] = {'B','M'};
                    hdr[2] = file_size & 0xFF; hdr[3] = (file_size>>8) & 0xFF;
                    hdr[4] = (file_size>>16) & 0xFF; hdr[5] = (file_size>>24) & 0xFF;
                    hdr[10] = 54; /* pixel data offset */
                    fwrite(hdr, 1, 14, f);

                    /* DIB Header (40 bytes) */
                    uint8_t dib[40] = {0};
                    dib[0] = 40; /* header size */
                    dib[4] = w & 0xFF; dib[5] = (w>>8) & 0xFF; dib[6] = (w>>16) & 0xFF; dib[7] = (w>>24) & 0xFF;
                    dib[8] = h & 0xFF; dib[9] = (h>>8) & 0xFF; dib[10] = (h>>16) & 0xFF; dib[11] = (h>>24) & 0xFF;
                    dib[12] = 1; /* color planes */
                    dib[14] = 24; /* bits per pixel */
                    dib[20] = pixel_data_size & 0xFF; dib[21] = (pixel_data_size>>8) & 0xFF;
                    dib[22] = (pixel_data_size>>16) & 0xFF; dib[23] = (pixel_data_size>>24) & 0xFF;
                    fwrite(dib, 1, 40, f);

                    /* Pixel data - BMP stores bottom-to-top, BGR order */
                    uint8_t* row_buf = (uint8_t*)calloc(row_size, 1);
                    if (row_buf) {
                        for (int y = h - 1; y >= 0; y--) {
                            memset(row_buf, 0, row_size);
                            for (int x = 0; x < w; x++) {
                                uint32_t p = screen_gfx->pixels[y * w + x];
                                row_buf[x * 3 + 0] = p & 0xFF;         /* B */
                                row_buf[x * 3 + 1] = (p >> 8) & 0xFF;  /* G */
                                row_buf[x * 3 + 2] = (p >> 16) & 0xFF; /* R */
                            }
                            fwrite(row_buf, 1, row_size, f);
                        }
                        free(row_buf);
                    }
                    fclose(f);
                    fprintf(stderr, "[M3G-CAP] Saved frame #%d to %s (%dx%d)\n", force_render_count, fname, w, h);
                }
            }
        }
    }
    
    g_m3g.target_gfx = NULL;
    g_m3g.target_is_graphics = 0;
    g_m3g_render_done = true;
    /* FIX 32b: Don't reset scene_done if we have a valid World.
     * The game loads M3G files once (in Loader.load) and expects rendering
     * every frame. Resetting scene_done here would prevent force-render from
     * running on subsequent frames after the first successful render. */
    if (!g_m3g_last_world) {
        g_m3g_scene_setup_done = false;
    }
}

/* Called from midp_process_repaints() after paint() to check if M3G force-render
 * produced output that needs to be blitted to the screen framebuffer.
 * Returns true if blit was performed (screen was modified). */
bool m3g_blit_to_screen_if_needed(JVM* jvm, MidpGraphics* screen_gfx) {
    (void)jvm;
    (void)screen_gfx;
    /* If force-render was performed this frame, the pixels are already
     * written into the framebuffer by m3g_rasterize_triangle() via the
     * render callback. No separate blit step is needed. */
    /* This function exists as a hook for display.c to call; the actual
     * pixel writing happens inside m3g_force_render() → m3g_render_node_recursive()
     * → m3g_rasterize_triangle() which writes directly to the screen buffer. */
    return false;
}

/* Global M3G Object Registry for find() - stores all loaded M3G objects by userID */
/* (typedef and forward decl at top of file, definition here) */
M3GObjectRegistry g_m3g_registry = {0};

/* Register an M3G object for global find() lookup */
static void m3g_registry_register(JavaObject* obj, jint userID) {
    if (!obj || userID < 0) return;
    if (g_m3g_registry.count >= 4096) return;  /* Increased from 1024 to handle large scenes */
    
    /* Check if already registered */
    for (int i = 0; i < g_m3g_registry.count; i++) {
        if (g_m3g_registry.objects[i] == obj) {
            g_m3g_registry.userIDs[i] = userID;  /* Update userID */
            return;
        }
    }
    
    /* Add new entry */
    g_m3g_registry.objects[g_m3g_registry.count] = obj;
    g_m3g_registry.userIDs[g_m3g_registry.count] = userID;
    g_m3g_registry.count++;
}

/* Find an M3G object by userID in the global registry */
static JavaObject* m3g_registry_find(jint userID) {
    for (int i = 0; i < g_m3g_registry.count; i++) {
        if (g_m3g_registry.userIDs[i] == userID) {
            return g_m3g_registry.objects[i];
        }
    }
    return NULL;
}

/* Clear the global registry (call when loading a new M3G file) */
static void m3g_registry_clear(void) {
    g_m3g_registry.count = 0;
}

/* Helper: Set userID on an object and register it for global find() */
static void m3g_set_userID(JavaObject* obj, jint user_id) {
    if (!obj) return;
    m3g_set_int_field(obj, "userID", user_id);
    if (user_id > 0) {
        m3g_registry_register(obj, user_id);
    }
}

/* GC ROOT SUPPORT: Export M3G registry for garbage collection marking.
 * The GC needs to mark these objects as roots to prevent them from being
 * collected while they're still referenced by the registry.
 * Returns: pointer to the objects array and sets *out_count to the number of objects.
 */
JavaObject** m3g_registry_get_objects(int* out_count) {
    if (out_count) {
        *out_count = g_m3g_registry.count;
    }
    return g_m3g_registry.objects;
}

/* ============================================================================
 * M3G Math Functions
 * ============================================================================ */

/* Transpose a 4x4 matrix from row-major (Java/M3G binary format) to column-major (OpenGL/C internal).
 * Java Transform stores: mat[row*4+col]. C M3GTransform stores: m[col*4+row].
 * This function converts in-place. */
static void m3g_transform_transpose_inplace(M3GTransform* t) {
    float tmp;
    /* Swap off-diagonal elements */
    tmp = t->m[1];  t->m[1]  = t->m[4];  t->m[4]  = tmp;
    tmp = t->m[2];  t->m[2]  = t->m[8];  t->m[8]  = tmp;
    tmp = t->m[3];  t->m[3]  = t->m[12]; t->m[12] = tmp;
    tmp = t->m[6];  t->m[6]  = t->m[9];  t->m[9]  = tmp;
    tmp = t->m[7];  t->m[7]  = t->m[13]; t->m[13] = tmp;
    tmp = t->m[11]; t->m[11] = t->m[14]; t->m[14] = tmp;
}

/* Convert a 16-float array from row-major to column-major, storing into M3GTransform */
static void m3g_transform_from_rowmajor(M3GTransform* out, const float* rowmajor) {
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            out->m[col * 4 + row] = rowmajor[row * 4 + col];
        }
    }
}

/* Create identity matrix */
static void m3g_transform_identity(M3GTransform* t) {
    memset(t->m, 0, sizeof(t->m));
    t->m[0] = 1.0f;   t->m[5] = 1.0f;   t->m[10] = 1.0f;  t->m[15] = 1.0f;
}

/* Multiply two matrices: result = a * b */
M3G_INLINE void m3g_transform_multiply(M3GTransform* result, 
                                    const M3GTransform* a, 
                                    const M3GTransform* b) {
    float r[16];
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += a->m[k * 4 + row] * b->m[col * 4 + k];
            }
            r[col * 4 + row] = sum;
        }
    }
    memcpy(result->m, r, sizeof(r));
}

/* Translate */
static void m3g_transform_translate(M3GTransform* t, float x, float y, float z) {
    M3GTransform trans;
    m3g_transform_identity(&trans);
    trans.m[12] = x;  trans.m[13] = y;  trans.m[14] = z;
    m3g_transform_multiply(t, t, &trans);
}

/* Scale */
static void m3g_transform_scale(M3GTransform* t, float x, float y, float z) {
    M3GTransform scale;
    m3g_transform_identity(&scale);
    scale.m[0] = x;  scale.m[5] = y;  scale.m[10] = z;
    m3g_transform_multiply(t, t, &scale);
}

/* Rotate around axis (angle in radians) */
static void m3g_transform_rotate(M3GTransform* t, float angle, 
                                  float ax, float ay, float az) {
    /* Normalize axis */
    float len = sqrtf(ax*ax + ay*ay + az*az);
    if (len < 0.0001f) return;
    ax /= len;  ay /= len;  az /= len;
    
    float c = cosf(angle);
    float s = sinf(angle);
    float omc = 1.0f - c;
    
    M3GTransform rot;
    m3g_transform_identity(&rot);
    
    rot.m[0] = ax*ax*omc + c;
    rot.m[1] = ax*ay*omc + az*s;
    rot.m[2] = ax*az*omc - ay*s;
    
    rot.m[4] = ax*ay*omc - az*s;
    rot.m[5] = ay*ay*omc + c;
    rot.m[6] = ay*az*omc + ax*s;
    
    rot.m[8] = ax*az*omc + ay*s;
    rot.m[9] = ay*az*omc - ax*s;
    rot.m[10] = az*az*omc + c;
    
    m3g_transform_multiply(t, t, &rot);
}

/* Set perspective projection */
static void m3g_transform_perspective(M3GTransform* t, float fov, 
                                       float aspect, float near, float far) {
    /* FIX 29: Guard against degenerate parameters that produce NaN/Infinity.
     * The game calls Camera.setPerspective(fov, 0, near, far) with aspect=0,
     * meaning "use viewport aspect ratio". Default to 0.75 (typical 240x320)
     * instead of 1.0 which gives wrong horizontal FOV.
     * Also guard near<=0 which would flip the depth range. */
    if (aspect <= 0.001f) aspect = 0.75f;
    if (near <= 0.001f) near = 0.001f;
    if (far <= near + 0.001f) far = near + 1.0f;
    if (fov <= 0.0f || fov >= 180.0f) fov = 60.0f;
    
    float f = 1.0f / tanf(fov * 3.14159f / 360.0f);
    float range = 1.0f / (near - far);
    
    memset(t->m, 0, sizeof(t->m));
    t->m[0] = f / aspect;
    t->m[5] = f;
    t->m[10] = (near + far) * range;
    t->m[11] = -1.0f;
    t->m[14] = 2.0f * near * far * range;
}

/* Set orthographic projection */
static void m3g_transform_ortho(M3GTransform* t, float left, float right,
                                 float bottom, float top, float near, float far) {
    memset(t->m, 0, sizeof(t->m));
    t->m[0] = 2.0f / (right - left);
    t->m[5] = 2.0f / (top - bottom);
    t->m[10] = 2.0f / (near - far);
    t->m[12] = -(right + left) / (right - left);
    t->m[13] = -(top + bottom) / (top - bottom);
    t->m[14] = (near + far) / (near - far);
    t->m[15] = 1.0f;
}

/* Set look-at matrix */
static void m3g_transform_look_at(M3GTransform* t,
                                   float eye_x, float eye_y, float eye_z,
                                   float at_x, float at_y, float at_z,
                                   float up_x, float up_y, float up_z) {
    /* Forward vector (Z) */
    float fx = at_x - eye_x;
    float fy = at_y - eye_y;
    float fz = at_z - eye_z;
    float flen = sqrtf(fx*fx + fy*fy + fz*fz);
    fx /= flen; fy /= flen; fz /= flen;
    
    /* Side vector (X) = forward x up */
    float sx = fy * up_z - fz * up_y;
    float sy = fz * up_x - fx * up_z;
    float sz = fx * up_y - fy * up_x;
    float slen = sqrtf(sx*sx + sy*sy + sz*sz);
    sx /= slen; sy /= slen; sz /= slen;
    
    /* Recalculate up (Y) = side x forward */
    float ux = sy * fz - sz * fy;
    float uy = sz * fx - sx * fz;
    float uz = sx * fy - sy * fx;
    
    /* Build rotation matrix */
    t->m[0] = sx;   t->m[4] = ux;   t->m[8]  = -fx;  t->m[12] = 0;
    t->m[1] = sy;   t->m[5] = uy;   t->m[9]  = -fy;  t->m[13] = 0;
    t->m[2] = sz;   t->m[6] = uz;   t->m[10] = -fz;  t->m[14] = 0;
    t->m[3] = 0;    t->m[7] = 0;    t->m[11] = 0;    t->m[15] = 1;
    
    /* Apply translation */
    M3GTransform trans;
    m3g_transform_identity(&trans);
    trans.m[12] = -eye_x;
    trans.m[13] = -eye_y;
    trans.m[14] = -eye_z;
    m3g_transform_multiply(t, t, &trans);
}

/* Transform a point by matrix */
static void m3g_transform_point(float* result, const M3GTransform* t, 
                                 const float* point) {
    float x = point[0], y = point[1], z = point[2], w = point[3];
    result[0] = t->m[0]*x + t->m[4]*y + t->m[8]*z + t->m[12]*w;
    result[1] = t->m[1]*x + t->m[5]*y + t->m[9]*z + t->m[13]*w;
    result[2] = t->m[2]*x + t->m[6]*y + t->m[10]*z + t->m[14]*w;
    result[3] = t->m[3]*x + t->m[7]*y + t->m[11]*z + t->m[15]*w;
}

/* Transform a vector (no translation) */
static void m3g_transform_vector(float* result, const M3GTransform* t,
                                  const float* vec) {
    float x = vec[0], y = vec[1], z = vec[2];
    result[0] = t->m[0]*x + t->m[4]*y + t->m[8]*z;
    result[1] = t->m[1]*x + t->m[5]*y + t->m[9]*z;
    result[2] = t->m[2]*x + t->m[6]*y + t->m[10]*z;
}

/* Vector operations */
static void m3g_vec3_normalize(float* v) {
    float len = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (len > 0.0001f) {
        v[0] /= len; v[1] /= len; v[2] /= len;
    }
}

static void m3g_vec3_cross(float* result, const float* a, const float* b) {
    result[0] = a[1]*b[2] - a[2]*b[1];
    result[1] = a[2]*b[0] - a[0]*b[2];
    result[2] = a[0]*b[1] - a[1]*b[0];
}

static float m3g_vec3_dot(const float* a, const float* b) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

/* ============================================================================
 * M3G Software Rasterizer
 * ============================================================================ */

/* Free existing context buffers - call before reinitializing */
static void m3g_context_free_buffers(void) {
    if (g_m3g.color_buffer) {
        free(g_m3g.color_buffer);
        g_m3g.color_buffer = NULL;
    }
    if (g_m3g.depth_buffer) {
        free(g_m3g.depth_buffer);
        g_m3g.depth_buffer = NULL;
    }
    g_m3g.buffers_allocated = 0;
}

/* Initialize render pool for object reuse */
static void m3g_render_pool_init(void) {
    if (g_m3g.render_pool.initialized) {
        return;  /* Already initialized */
    }
    
    g_m3g.render_pool.vertex_pool = (float*)malloc(M3G_VERTEX_POOL_SIZE * sizeof(float));
    g_m3g.render_pool.vertex_pool_capacity = g_m3g.render_pool.vertex_pool ? M3G_VERTEX_POOL_SIZE : 0;
    g_m3g.render_pool.vertex_pool_used = 0;
    
    g_m3g.render_pool.index_pool = (uint16_t*)malloc(M3G_INDEX_POOL_SIZE * sizeof(uint16_t));
    g_m3g.render_pool.index_pool_capacity = g_m3g.render_pool.index_pool ? M3G_INDEX_POOL_SIZE : 0;
    g_m3g.render_pool.index_pool_used = 0;
    
    g_m3g.render_pool.initialized = 1;
    GFX_DEBUG("Render pool initialized: vertices=%zu, indices=%zu", 
             g_m3g.render_pool.vertex_pool_capacity, g_m3g.render_pool.index_pool_capacity);
}

/* Reset render pool for new frame */
static void m3g_render_pool_reset(void) {
    g_m3g.render_pool.vertex_pool_used = 0;
    g_m3g.render_pool.index_pool_used = 0;
}

/* Allocate from vertex pool (returns NULL if pool exhausted) */
static float* m3g_render_pool_alloc_vertices(size_t count) {
    if (g_m3g.render_pool.vertex_pool_used + count > g_m3g.render_pool.vertex_pool_capacity) {
        GFX_DEBUG("Vertex pool exhausted: need %zu, have %zu", 
                 count, g_m3g.render_pool.vertex_pool_capacity - g_m3g.render_pool.vertex_pool_used);
        return NULL;
    }
    float* ptr = g_m3g.render_pool.vertex_pool + g_m3g.render_pool.vertex_pool_used;
    g_m3g.render_pool.vertex_pool_used += count;
    return ptr;
}

/* Allocate from index pool (returns NULL if pool exhausted) */
static uint16_t* m3g_render_pool_alloc_indices(size_t count) {
    if (g_m3g.render_pool.index_pool_used + count > g_m3g.render_pool.index_pool_capacity) {
        GFX_DEBUG("Index pool exhausted: need %zu, have %zu", 
                 count, g_m3g.render_pool.index_pool_capacity - g_m3g.render_pool.index_pool_used);
        return NULL;
    }
    uint16_t* ptr = g_m3g.render_pool.index_pool + g_m3g.render_pool.index_pool_used;
    g_m3g.render_pool.index_pool_used += count;
    return ptr;
}

/* Initialize M3G context */
static void m3g_context_init(int width, int height) {
    /* FIX: Free existing buffers before allocating new ones to prevent memory leak */
    m3g_context_free_buffers();
    
    /* Preserve render pool and camera state across reinit */
    M3GRenderPool saved_pool = g_m3g.render_pool;
    JavaObject* saved_camera = g_m3g.camera;
    M3GTransform saved_cam_transform = g_m3g.camera_transform;
    M3GTransform saved_cam_inverse = g_m3g.camera_inverse;
    int saved_camera_set = g_m3g.camera_set;
    
    /* Clear context (zeroes all fields including target_image, target_gfx, target_is_graphics) */
    memset(&g_m3g, 0, sizeof(g_m3g));
    
    /* Restore render pool — only if it was previously initialized.
     * If saved_pool.initialized == 0, all pointers are NULL and the pool
     * will be re-initialized by m3g_render_pool_init() below. */
    if (saved_pool.initialized) {
        g_m3g.render_pool = saved_pool;
    }
    
    /* Restore camera state (preserved across buffer reallocation) */
    g_m3g.camera = saved_camera;
    g_m3g.camera_transform = saved_cam_transform;
    g_m3g.camera_inverse = saved_cam_inverse;
    g_m3g.camera_set = saved_camera_set;
    
    g_m3g.buffer_width = width;
    g_m3g.buffer_height = height;
    g_m3g.viewport_width = width;
    g_m3g.viewport_height = height;
    
    g_m3g.color_buffer = (uint32_t*)calloc(width * height, sizeof(uint32_t));
    g_m3g.depth_buffer = (float*)calloc(width * height, sizeof(float));
    
    if (g_m3g.color_buffer && g_m3g.depth_buffer) {
        g_m3g.buffers_allocated = 1;
    }
    
    /* Initialize transforms */
    m3g_transform_identity(&g_m3g.modelview);
    m3g_transform_identity(&g_m3g.projection);
    m3g_transform_identity(&g_m3g.mvp);
    
    /* Default render state */
    g_m3g.depth_test_enabled = 1;
    g_m3g.depth_write_enabled = 1;
    g_m3g.culling_enabled = 1;
    
    /* Default ambient */
    g_m3g.ambient_light[0] = 0.2f;
    g_m3g.ambient_light[1] = 0.2f;
    g_m3g.ambient_light[2] = 0.2f;
    g_m3g.ambient_light[3] = 1.0f;
    
    /* Initialize render pool if not already done */
    m3g_render_pool_init();
    
    GFX_DEBUG("Context initialized: %dx%d, buffers=%d", width, height, g_m3g.buffers_allocated);
}

/* Clear buffers - optimized to avoid per-iteration multiplication */
M3G_HOT static void m3g_clear(float r, float g, float b, float a, float depth) {
    uint32_t color = ((uint32_t)(a * 255) << 24) |
                     ((uint32_t)(r * 255) << 16) |
                     ((uint32_t)(g * 255) << 8) |
                     ((uint32_t)(b * 255));
    
    int total = g_m3g.buffer_width * g_m3g.buffer_height;
    uint32_t* cb = g_m3g.color_buffer;
    float* db = g_m3g.depth_buffer;
    for (int i = 0; i < total; i++) {
        cb[i] = color;
        db[i] = depth;
    }
}

/* Convert clip coordinates to screen coordinates.
 * Returns 0 on success, -1 if vertex is behind near plane (w <= 0). */
static int m3g_clip_to_screen(float* screen, const float* clip) {
    /* Perspective divide - reject vertices behind camera */
    float w = clip[3];
    if (w < 0.001f) return -1;  /* Behind near plane */
    
    float ndc_x = clip[0] / w;
    float ndc_y = clip[1] / w;
    float ndc_z = clip[2] / w;
    
    /* NDC to screen */
    screen[0] = (ndc_x + 1.0f) * 0.5f * g_m3g.viewport_width + g_m3g.viewport_x;
    screen[1] = (1.0f - ndc_y) * 0.5f * g_m3g.viewport_height + g_m3g.viewport_y;  /* Y-flip */
    screen[2] = (ndc_z + 1.0f) * 0.5f;  /* Depth to [0,1] */
    screen[3] = 1.0f / w;  /* 1/w for perspective correction */
    return 0;
}

/* Near-plane clip threshold for Sutherland-Hodgman clipping */
#define M3G_NEAR_CLIP_W  0.001f

/* Interpolate between two clip-space vertices (linearly in clip space).
 * Used by Sutherland-Hodgman clipping. Also interpolates colors and texcoords. */
static void m3g_lerp_clip(float* out_clip, uint8_t* out_color, float* out_tex,
                           const float* in_clip0, const uint8_t* in_color0, const float* in_tex0,
                           const float* in_clip1, const uint8_t* in_color1, const float* in_tex1,
                           float t) {
    float s = 1.0f - t;
    out_clip[0] = s * in_clip0[0] + t * in_clip1[0];
    out_clip[1] = s * in_clip0[1] + t * in_clip1[1];
    out_clip[2] = s * in_clip0[2] + t * in_clip1[2];
    out_clip[3] = s * in_clip0[3] + t * in_clip1[3];
    if (out_color && in_color0 && in_color1) {
        out_color[0] = (uint8_t)(s * in_color0[0] + t * in_color1[0] + 0.5f);
        out_color[1] = (uint8_t)(s * in_color0[1] + t * in_color1[1] + 0.5f);
        out_color[2] = (uint8_t)(s * in_color0[2] + t * in_color1[2] + 0.5f);
        out_color[3] = (uint8_t)(s * in_color0[3] + t * in_color1[3] + 0.5f);
    }
    if (out_tex && in_tex0 && in_tex1) {
        out_tex[0] = s * in_tex0[0] + t * in_tex1[0];
        out_tex[1] = s * in_tex0[1] + t * in_tex1[1];
    }
}

/* Clip a triangle against the near plane (w >= M3G_NEAR_CLIP_W).
 * Uses Sutherland-Hodgman algorithm. Returns number of output triangles (0, 1, or 2).
 * cc0/cc1/cc2 are clipped colors, ct0/ct1/ct2 are clipped texcoords. */
static int m3g_clip_triangle_near(const float* in_clip[3], const uint8_t* in_color[3], const float* in_tex[3],
                                    float out_clip[6][4], uint8_t out_color[6][4], float out_tex[6][2]) {
    /* Determine which vertices are inside (w >= threshold) */
    int inside[3];
    for (int i = 0; i < 3; i++) {
        inside[i] = (in_clip[i][3] >= M3G_NEAR_CLIP_W) ? 1 : 0;
    }
    int inside_count = inside[0] + inside[1] + inside[2];
    
    /* All inside: no clipping needed */
    if (inside_count == 3) {
        memcpy(out_clip[0], in_clip[0], 4 * sizeof(float));
        memcpy(out_clip[1], in_clip[1], 4 * sizeof(float));
        memcpy(out_clip[2], in_clip[2], 4 * sizeof(float));
        if (in_color[0]) { memcpy(out_color[0], in_color[0], 4); memcpy(out_color[1], in_color[1], 4); memcpy(out_color[2], in_color[2], 4); }
        if (in_tex[0]) { memcpy(out_tex[0], in_tex[0], 2*sizeof(float)); memcpy(out_tex[1], in_tex[1], 2*sizeof(float)); memcpy(out_tex[2], in_tex[2], 2*sizeof(float)); }
        return 1;
    }
    
    /* All outside: discard */
    if (inside_count == 0) return 0;
    
    /* One vertex inside: produces 1 triangle */
    if (inside_count == 1) {
        int idx_in = -1, idx_out0 = -1, idx_out1 = -1;
        if (inside[0]) { idx_in = 0; idx_out0 = 1; idx_out1 = 2; }
        else if (inside[1]) { idx_in = 1; idx_out0 = 0; idx_out1 = 2; }
        else { idx_in = 2; idx_out0 = 0; idx_out1 = 1; }
        
        /* Intersect edges at near plane */
        float w_in = in_clip[idx_in][3];
        float t0 = (w_in - M3G_NEAR_CLIP_W) / (w_in - in_clip[idx_out0][3]);
        float t1 = (w_in - M3G_NEAR_CLIP_W) / (w_in - in_clip[idx_out1][3]);
        if (t0 < 0) t0 = 0; if (t0 > 1) t0 = 1;
        if (t1 < 0) t1 = 0; if (t1 > 1) t1 = 1;
        
        memcpy(out_clip[0], in_clip[idx_in], 4 * sizeof(float));
        m3g_lerp_clip(out_clip[1], out_color[1], out_tex[1],
                       in_clip[idx_in], in_color[idx_in], in_tex[idx_in],
                       in_clip[idx_out0], in_color[idx_out0], in_tex[idx_out0], t0);
        m3g_lerp_clip(out_clip[2], out_color[2], out_tex[2],
                       in_clip[idx_in], in_color[idx_in], in_tex[idx_in],
                       in_clip[idx_out1], in_color[idx_out1], in_tex[idx_out1], t1);
        if (in_color[0]) memcpy(out_color[0], in_color[idx_in], 4);
        if (in_tex[0]) memcpy(out_tex[0], in_tex[idx_in], 2 * sizeof(float));
        return 1;
    }
    
    /* Two vertices inside: produces 1 quad → 2 triangles */
    /* idx_out is the single outside vertex */
    int idx_out = -1;
    if (!inside[0]) idx_out = 0;
    else if (!inside[1]) idx_out = 1;
    else idx_out = 2;
    
    int idx_in0 = (idx_out + 1) % 3;
    int idx_in1 = (idx_out + 2) % 3;
    
    float w0 = in_clip[idx_in0][3];
    float w1 = in_clip[idx_in1][3];
    float t0 = (w0 - M3G_NEAR_CLIP_W) / (w0 - in_clip[idx_out][3]);
    float t1 = (w1 - M3G_NEAR_CLIP_W) / (w1 - in_clip[idx_out][3]);
    if (t0 < 0) t0 = 0; if (t0 > 1) t0 = 1;
    if (t1 < 0) t1 = 0; if (t1 > 1) t1 = 1;
    
    /* Clip point on edge (in0 → out) */
    float cp0[4]; uint8_t cc0[4]; float ct0[2];
    m3g_lerp_clip(cp0, cc0, ct0, in_clip[idx_in0], in_color[idx_in0], in_tex[idx_in0],
                   in_clip[idx_out], in_color[idx_out], in_tex[idx_out], t0);
    
    /* Clip point on edge (in1 → out) */
    float cp1[4]; uint8_t cc1[4]; float ct1[2];
    m3g_lerp_clip(cp1, cc1, ct1, in_clip[idx_in1], in_color[idx_in1], in_tex[idx_in1],
                   in_clip[idx_out], in_color[idx_out], in_tex[idx_out], t1);
    
    /* Triangle 1: in0, in1, cp0 */
    memcpy(out_clip[0], in_clip[idx_in0], 4 * sizeof(float));
    memcpy(out_clip[1], in_clip[idx_in1], 4 * sizeof(float));
    memcpy(out_clip[2], cp0, 4 * sizeof(float));
    if (in_color[0]) { memcpy(out_color[0], in_color[idx_in0], 4); memcpy(out_color[1], in_color[idx_in1], 4); memcpy(out_color[2], cc0, 4); }
    if (in_tex[0]) { memcpy(out_tex[0], in_tex[idx_in0], 2*sizeof(float)); memcpy(out_tex[1], in_tex[idx_in1], 2*sizeof(float)); memcpy(out_tex[2], ct0, 2*sizeof(float)); }
    
    /* Triangle 2: in1, cp1, cp0 */
    memcpy(out_clip[3], in_clip[idx_in1], 4 * sizeof(float));
    memcpy(out_clip[4], cp1, 4 * sizeof(float));
    memcpy(out_clip[5], cp0, 4 * sizeof(float));
    if (in_color[0]) { memcpy(out_color[3], in_color[idx_in1], 4); memcpy(out_color[4], cc1, 4); memcpy(out_color[5], cc0, 4); }
    if (in_tex[0]) { memcpy(out_tex[3], in_tex[idx_in1], 2*sizeof(float)); memcpy(out_tex[4], ct1, 2*sizeof(float)); memcpy(out_tex[5], ct0, 2*sizeof(float)); }
    
    return 2;
}

/* Diagnostic counter for debugging */
static int g_m3g_clip_triangles_in = 0;
static int g_m3g_clip_triangles_out = 0;
static int g_m3g_raster_pixels_written = 0;

/* Edge function for triangle rasterization */
static inline float m3g_edge_function(const float* a, const float* b, const float* c) {
    return (c[0] - a[0]) * (b[1] - a[1]) - (c[1] - a[1]) * (b[0] - a[0]);
}

/* Compute barycentric coordinates */
static void m3g_barycentric(float* bary, const float* v0, const float* v1, 
                            const float* v2, float x, float y) {
    float area = m3g_edge_function(v0, v1, v2);
    if (fabsf(area) < 0.0001f) {
        bary[0] = bary[1] = bary[2] = 0;
        return;
    }
    
    float p[2] = {x, y};
    bary[0] = m3g_edge_function(v1, v2, p) / area;
    bary[1] = m3g_edge_function(v2, v0, p) / area;
    bary[2] = m3g_edge_function(v0, v1, p) / area;
}

/* Simple color interpolation */
static void m3g_interpolate_color(uint8_t* result, 
                                   const uint8_t* c0, const uint8_t* c1, const uint8_t* c2,
                                   float w0, float w1, float w2) {
    for (int i = 0; i < 4; i++) {
        result[i] = (uint8_t)(c0[i] * w0 + c1[i] * w1 + c2[i] * w2);
    }
}

/* Interpolate texture coordinates */
static void m3g_interpolate_texcoord(float* result,
                                      const float* t0, const float* t1, const float* t2,
                                      float w0, float w1, float w2, float one_over_w) {
    /* Perspective-correct interpolation */
    float u = (t0[0] * w0 + t1[0] * w1 + t2[0] * w2) * one_over_w;
    float v = (t0[1] * w0 + t1[1] * w1 + t2[1] * w2) * one_over_w;
    result[0] = u;
    result[1] = v;
}

/* Sample texture (bilinear filtering) */
static uint32_t m3g_sample_texture(M3GTexture2D* tex, float u, float v) {
    if (!tex || !tex->pixels || tex->width <= 0 || tex->height <= 0) return 0xFFFFFFFF;
    
    /* Guard against NaN / Inf from extreme perspective division.
     * When w is near zero or negative, UV interpolation produces NaN,
     * and (int)NaN is undefined behavior (typically INT_MIN on x86),
     * leading to out-of-bounds access and SIGSEGV. */
    if (u != u || v != v ||           /* isnan */
        u == 1.0f/0.0f || u == -1.0f/0.0f ||  /* isinf */
        v == 1.0f/0.0f || v == -1.0f/0.0f) {
        return 0xFFFFFFFF;  /* Return white for invalid coords */
    }
    
    /* Wrap coordinates */
    u = u - floorf(u);
    v = v - floorf(v);
    if (u < 0) u += 1.0f;
    if (v < 0) v += 1.0f;
    
    /* Clamp to [0, 1) to handle floating-point edge cases */
    if (u >= 1.0f) u = 0.999999f;
    if (v >= 1.0f) v = 0.999999f;
    
    /* Nearest neighbor (simple version) */
    int x = (int)(u * tex->width);
    int y = (int)(v * tex->height);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= tex->width) x = tex->width - 1;
    if (y >= tex->height) y = tex->height - 1;
    
    return tex->pixels[y * tex->width + x];
}

/* Fast specular approximation using exp2(log2(x) * y).
 * On ARMv7 without FPU, powf() costs ~50+ cycles calling into libm.
 * exp2f(log2f(x) * y) compiles to ~6 VFP instructions.
 * Returns 0 for non-positive inputs (no negative specular highlights). */
M3G_INLINE float m3g_fast_specular(float ndh, float shininess) {
    if (ndh <= 0.0f) return 0.0f;
    if (shininess <= 1.0f) return ndh;
    /* Use exp2(log2(x) * y) — much cheaper than powf on ARM */
    return exp2f(log2f(ndh) * shininess);
}

/* Apply lighting to a vertex - optimized for ARMv7 */
M3G_HOT static void m3g_light_vertex(float* result, const float* position, 
                              const float* normal, M3GMaterial* mat) {
    /* Start with ambient */
    result[0] = g_m3g.ambient_light[0] * mat->ambient[0] + mat->emissive[0];
    result[1] = g_m3g.ambient_light[1] * mat->ambient[1] + mat->emissive[1];
    result[2] = g_m3g.ambient_light[2] * mat->ambient[2] + mat->emissive[2];
    
    float N[3] = {normal[0], normal[1], normal[2]};
    m3g_vec3_normalize(N);
    
    /* Process each light */
    for (int i = 0; i < g_m3g.light_count; i++) {
        M3GLight* light = &g_m3g.lights[i];
        
        float L[3];  /* Light direction */
        float attenuation = 1.0f;
        
        if (light->type == M3G_LIGHT_DIRECTIONAL) {
            L[0] = -light->direction[0];
            L[1] = -light->direction[1];
            L[2] = -light->direction[2];
        } else if (light->type == M3G_LIGHT_OMNI) {
            L[0] = light->position[0] - position[0];
            L[1] = light->position[1] - position[1];
            L[2] = light->position[2] - position[2];
            float dist = sqrtf(L[0]*L[0] + L[1]*L[1] + L[2]*L[2]);
            if (dist > 0.0001f) {
                L[0] /= dist; L[1] /= dist; L[2] /= dist;
            }
            attenuation = 1.0f / (light->attenuation[0] + 
                                  light->attenuation[1] * dist +
                                  light->attenuation[2] * dist * dist);
        } else {
            continue;  /* Skip unsupported light types */
        }
        
        /* Diffuse */
        float NdotL = m3g_vec3_dot(N, L);
        if (NdotL > 0) {
            result[0] += light->color[0] * mat->diffuse[0] * NdotL * attenuation;
            result[1] += light->color[1] * mat->diffuse[1] * NdotL * attenuation;
            result[2] += light->color[2] * mat->diffuse[2] * NdotL * attenuation;
        }
        
        /* Specular (simplified) - uses fast approximation instead of powf */
        float V[3] = {0, 0, 1};  /* View direction (assumed) */
        float H[3];  /* Half vector */
        H[0] = L[0] + V[0];
        H[1] = L[1] + V[1];
        H[2] = L[2] + V[2];
        m3g_vec3_normalize(H);
        
        float NdotH = m3g_vec3_dot(N, H);
        if (NdotH > 0) {
            float spec = m3g_fast_specular(NdotH, mat->shininess);
            result[0] += light->color[0] * mat->specular[0] * spec * attenuation;
            result[1] += light->color[1] * mat->specular[1] * spec * attenuation;
            result[2] += light->color[2] * mat->specular[2] * spec * attenuation;
        }
    }
    
    /* Clamp */
    result[0] = result[0] > 1.0f ? 1.0f : result[0];
    result[1] = result[1] > 1.0f ? 1.0f : result[1];
    result[2] = result[2] > 1.0f ? 1.0f : result[2];
}

/* ============================================================================
 * M3G Scene Graph Rendering Helpers
 * ============================================================================ */

/* Build an M3GTransform from a Node's component transform fields.
 * Nodes loaded from M3G files store translation/scale/orientation as individual
 * float fields (set by m3g_read_transformable_data). */
static void m3g_build_node_transform(JavaObject* node, M3GTransform* out) {
    m3g_transform_identity(out);

    if (!node) return;

    /* Check for a general 4x4 transform matrix (stored as float[] in "transform" field) */
    JavaArray* matrix_arr = (JavaArray*)m3g_get_ref_field(node, "transform");
    if (matrix_arr && matrix_arr->element_type == T_FLOAT && matrix_arr->length >= 16) {
        float* m = (float*)array_data(matrix_arr);
        /* DIAG: log for Mesh nodes */
        const char* cn = node->header.clazz && node->header.clazz->class_name ? node->header.clazz->class_name : "?";
        if (strstr(cn, "Mesh") != NULL) {
            fprintf(stderr, "[M3G-NODE-TF] Mesh transform float[16]: %.3f,%.3f,%.3f,%.3f / %.3f,%.3f,%.3f,%.3f / %.3f,%.3f,%.3f,%.3f / %.3f,%.3f,%.3f,%.3f\n",
                m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
        }
        /* Matrix from M3G file/Java is row-major; convert to column-major */
        m3g_transform_from_rowmajor(out, m);
        return;
    } else if (matrix_arr) {
        const char* cn = node->header.clazz && node->header.clazz->class_name ? node->header.clazz->class_name : "?";
        if (strstr(cn, "Mesh") != NULL) {
            fprintf(stderr, "[M3G-NODE-TF] Mesh transform field type=%d len=%d (not float[16])\n",
                    matrix_arr->element_type, matrix_arr->length);
        }
    }

    /* Build from component transform: T * R * S */
    float tx = m3g_get_float_field(node, "translationX", 0.0f);
    float ty = m3g_get_float_field(node, "translationY", 0.0f);
    float tz = m3g_get_float_field(node, "translationZ", 0.0f);
    float sx = m3g_get_float_field(node, "scaleX", 1.0f);
    float sy = m3g_get_float_field(node, "scaleY", 1.0f);
    float sz = m3g_get_float_field(node, "scaleZ", 1.0f);
    float angle = m3g_get_float_field(node, "orientationAngle", 0.0f);
    float ax = m3g_get_float_field(node, "orientationX", 0.0f);
    float ay = m3g_get_float_field(node, "orientationY", 0.0f);
    float az = m3g_get_float_field(node, "orientationZ", 1.0f);

    /* FIX: M3G files often leave scale at 0.0 when not explicitly set (Java default).
     * Treat scale(0,0,0) as identity (1,1,1) to avoid collapsing the transform.
     * A zero scale is degenerate and would make the entire mesh invisible. */
    if (sx == 0.0f && sy == 0.0f && sz == 0.0f) {
        sx = sy = sz = 1.0f;
    }

    /* Only apply transforms if they differ from defaults */
    int has_transform = (tx != 0.0f || ty != 0.0f || tz != 0.0f ||
                         sx != 1.0f || sy != 1.0f || sz != 1.0f ||
                         angle != 0.0f);

    if (!has_transform) return;

    /* Apply: Scale first, then Rotate, then Translate (post-multiply order) */
    if (sx != 1.0f || sy != 1.0f || sz != 1.0f) {
        m3g_transform_scale(out, sx, sy, sz);
    }
    if (angle != 0.0f) {
        m3g_transform_rotate(out, angle, ax, ay, az);
    }
    if (tx != 0.0f || ty != 0.0f || tz != 0.0f) {
        m3g_transform_translate(out, tx, ty, tz);
    }
    
    /* DIAG: for Mesh nodes, log the final transform */
    {
        const char* cn = node->header.clazz && node->header.clazz->class_name ? node->header.clazz->class_name : "?";
        if (strstr(cn, "Mesh") != NULL) {
            static int mesh_tf_diag = 0;
            if (mesh_tf_diag < 3) {
                mesh_tf_diag++;
                fprintf(stderr, "[M3G-NODE-TF] Mesh final transform: m[0]=%.3f m[5]=%.3f m[10]=%.3f m[15]=%.3f (tx=%.1f ty=%.1f tz=%.1f sx=%.1f sy=%.1f sz=%.1f ang=%.1f has_transform=%d)\n",
                    out->m[0], out->m[5], out->m[10], out->m[15], tx, ty, tz, sx, sy, sz, angle, has_transform);
            }
        }
    }
}

/* Build an M3GTexture2D from a Java Texture2D object for the rasterizer */
static M3GTexture2D* m3g_build_texture2d(JavaObject* texture_obj) {
    if (!texture_obj) return NULL;

    /* CACHE CHECK: Return cached texture if available */
    M3GTexture2D* cached = m3g_lookup_texture_cache(texture_obj);
    if (cached) return cached;
    if (m3g_texture_build_attempted(texture_obj)) return NULL; /* Already failed */

    const char* tex_class = texture_obj->header.clazz ? texture_obj->header.clazz->class_name : "?";
    static int tex_diag_count = 0;

    /* PRIMARY: Look up Image2D from C-side mapping table.
     * This was populated during M3G file parsing and brute-force pairing,
     * reliable regardless of the Texture2D Java class's field layout. */
    JavaObject* image = m3g_lookup_tex_image(texture_obj);
    int lookup_tier = image ? 1 : 0;

    /* SECONDARY: Try Java field lookup (works if Texture2D class has 'image' field) */
    if (!image) {
        image = m3g_get_ref_field(texture_obj, "image");
        if (image) lookup_tier = 2;
    }

    /* TERTIARY: Try 'imageRef' int field + registry lookup */
    if (!image) {
        int image_ref = m3g_get_int_field(texture_obj, "imageRef", 0);
        if (image_ref > 0) {
            for (int r = 0; r < g_m3g_registry.count; r++) {
                if (g_m3g_registry.userIDs[r] == image_ref) {
                    image = g_m3g_registry.objects[r];
                    if (image) break;
                }
            }
            if (image) {
                lookup_tier = 3;
                fprintf(stderr, "[M3G-TEX] Found Image2D via imageRef=%d -> %p (tier=%d)\n",
                        image_ref, (void*)image, lookup_tier);
            }
        }
    }

    /* QUATERNARY: Direct field scan — look for non-NULL ref fields pointing to Image2D */
    if (!image) {
        int max_slots = OBJECT_NUM_FIELDS(texture_obj);
        for (int s = 0; s < max_slots && s < 24; s++) {
            JavaObject* ref = texture_obj->fields[s].ref;
            if (ref && ref->header.clazz && ref->header.clazz->class_name &&
                strstr(ref->header.clazz->class_name, "Image2D")) {
                image = ref;
                lookup_tier = 4;
                if (tex_diag_count < 10) {
                    tex_diag_count++;
                    fprintf(stderr, "[M3G-TEX] Found Image2D %p via field scan slot %d (tier=%d)\n",
                            (void*)ref, s, lookup_tier);
                }
                break;
            }
        }
    }

    if (!image) {
        if (tex_diag_count < 5) {
            tex_diag_count++;
            fprintf(stderr, "[M3G-TEXBUILD-FAIL] Texture2D %p class=%s inst_size=%d\n",
                    (void*)texture_obj, tex_class,
                    texture_obj->header.clazz ? (int)texture_obj->header.clazz->instance_size : -1);
        }
        m3g_store_texture_cache(texture_obj, NULL); /* Cache the failure */
        return NULL;
    }

    const char* img_class = image->header.clazz ? image->header.clazz->class_name : "?";
    int width = m3g_get_int_field(image, "width", 0);
    int height = m3g_get_int_field(image, "height", 0);

    if (tex_diag_count <= 5) {
        tex_diag_count++;
        fprintf(stderr, "[M3G-TEXBUILD] Texture2D %p -> Image2D %p class=%s %dx%d (tier=%d)\n",
                (void*)texture_obj, (void*)image, img_class, width, height, lookup_tier);
    }

    /* FALLBACK: If dimensions look wrong, scan all slots for reasonable pairs */
    if (width <= 0 || height <= 0 || width > 2048 || height > 2048) {
        int img_slots = OBJECT_NUM_FIELDS(image);
        int best_w = 0, best_h = 0;
        int best_dist = 999999999;
        for (int try_h = 2; try_h < img_slots; try_h++) {
            for (int try_w = 2; try_w < try_h; try_w++) {
                int vw = image->fields[try_w].i;
                int vh = image->fields[try_h].i;
                if (vw > 0 && vw <= 2048 && vh > 0 && vh <= 2048) {
                    int dist = (vw - 64) * (vw - 64) + (vh - 64) * (vh - 64);
                    if (dist < best_dist) {
                        best_dist = dist;
                        best_w = vw;
                        best_h = vh;
                    }
                }
            }
        }
        if (best_w > 0 && best_h > 0) {
            static int fb_count = 0;
            if (fb_count < 3) {
                fb_count++;
                fprintf(stderr, "[M3G-TEXBUILD] Fallback: found plausible dims %dx%d (was %dx%d)\n",
                        best_w, best_h, width, height);
            }
            width = best_w;
            height = best_h;
        }
    }

    /* GUESS: Try to compute from pixels array if still unknown */
    if (width <= 0 || height <= 0) {
        JavaArray* pixels_arr = (JavaArray*)m3g_get_ref_field(image, "pixels");
        if (pixels_arr && pixels_arr->length > 0) {
            int total = (int)pixels_arr->length;
            int side = (int)sqrtf((float)total);
            if (side * side == total) { width = side; height = side; }
            else if (total % 256 == 0) { width = 256; height = total / 256; }
            else if (total % 128 == 0) { width = 128; height = total / 128; }
            else if (total % 64 == 0) { width = 64; height = total / 64; }
            else { width = side; height = total / width; }
            fprintf(stderr, "[M3G-TEXBUILD] Guessed dimensions: %dx%d\n", width, height);
        }
    }

    if (width <= 0 || height <= 0 || width > M3G_MAX_IMAGE_DIMENSION || height > M3G_MAX_IMAGE_DIMENSION) {
        fprintf(stderr, "[M3G-TEXBUILD-FAIL] Image2D %s: invalid dimensions %dx%d\n",
                img_class, width, height);
        m3g_store_texture_cache(texture_obj, NULL);
        return NULL;
    }

    int pixel_count = width * height;
    if (pixel_count > M3G_MAX_IMAGE_PIXELS) {
        m3g_store_texture_cache(texture_obj, NULL);
        return NULL;
    }

    M3GTexture2D* tex = (M3GTexture2D*)calloc(1, sizeof(M3GTexture2D));
    if (!tex) {
        m3g_store_texture_cache(texture_obj, NULL);
        return NULL;
    }

    tex->width = width;
    tex->height = height;
    tex->pixels = (uint32_t*)malloc(pixel_count * sizeof(uint32_t));
    if (!tex->pixels) { free(tex); m3g_store_texture_cache(texture_obj, NULL); return NULL; }

    JavaArray* pixels = (JavaArray*)m3g_get_ref_field(image, "pixels");
    if (pixels && pixels->element_type == T_INT && (int)pixels->length >= pixel_count) {
        jint* src = (jint*)array_data(pixels);
        for (int i = 0; i < pixel_count; i++) {
            tex->pixels[i] = (uint32_t)src[i];
        }
    }

    m3g_store_texture_cache(texture_obj, tex);
    return tex;
}

/* Forward declaration for recursive mesh rendering */
static void m3g_render_node_recursive(JVM* jvm, JavaObject* node, M3GTransform* parent_modelview);

/* Render a Sprite3D node as a billboard image at its projected 3D position.
 * Sprite3D always faces the camera and is drawn as a screen-aligned quad.
 * The image is scaled by the sprite's scale factors and perspective.
 */
static void m3g_render_sprite3d(JVM* jvm, JavaObject* sprite, const M3GTransform* mvp) {
    (void)jvm;
    if (!sprite || !mvp || !g_m3g.color_buffer) return;
    
    /* Check rendering enabled */
    int render_enable = m3g_get_int_field(sprite, "renderingEnable", 1);
    if (!render_enable) return;
    
    /* Get the Sprite3D's 3D position from its transform matrix.
     * The translation is in the 4th column of the transform (column-major).
     * But the sprite's position is already in world space; we need to apply MVP. */
    float pos[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    
    /* Get the translation from the node's float fields (more reliable) */
    pos[0] = m3g_get_float_field(sprite, "translationX", 0.0f);
    pos[1] = m3g_get_float_field(sprite, "translationY", 0.0f);
    pos[2] = m3g_get_float_field(sprite, "translationZ", 0.0f);
    
    /* Transform position by MVP to get clip coordinates */
    float clip[4];
    m3g_transform_point(clip, mvp, pos);
    
    /* Check if behind camera (w <= 0 means behind near plane) */
    if (clip[3] <= 0.001f) return;
    
    /* Perspective divide to NDC */
    float ndc_x = clip[0] / clip[3];
    float ndc_y = clip[1] / clip[3];
    
    /* Skip if outside NDC [-1, 1] with margin */
    float margin = 2.0f;
    if (ndc_x < -margin || ndc_x > margin || ndc_y < -margin || ndc_y > margin) return;
    
    /* Convert to screen coordinates */
    float screen_x = (ndc_x + 1.0f) * 0.5f * g_m3g.viewport_width + g_m3g.viewport_x;
    float screen_y = (1.0f - ndc_y) * 0.5f * g_m3g.viewport_height + g_m3g.viewport_y;
    
    /* Get scale factors for the sprite */
    float scale_x = m3g_get_float_field(sprite, "scaleX", 1.0f);
    float scale_y = m3g_get_float_field(sprite, "scaleY", 1.0f);
    
    /* Perspective distance factor: sprites farther away appear smaller */
    float perspective_scale = 1.0f / clip[3];
    float base_size = 50.0f;  /* Base size in world units - adjusted by perspective */
    
    /* Calculate screen-space size from perspective scale */
    float half_w = base_size * scale_x * perspective_scale * g_m3g.viewport_width * 0.5f;
    float half_h = base_size * scale_y * perspective_scale * g_m3g.viewport_height * 0.5f;
    
    /* Get the sprite's image */
    JavaObject* image = m3g_get_ref_field(sprite, "image");
    if (!image) return;
    
    int img_width = m3g_get_int_field(image, "width", 0);
    int img_height = m3g_get_int_field(image, "height", 0);
    if (img_width <= 0 || img_height <= 0) return;
    
    /* Use image dimensions for aspect ratio if available */
    if (half_w > 0 && half_h > 0) {
        float img_aspect = (float)img_width / (float)img_height;
        if (img_aspect > 1.0f) {
            half_h = half_w / img_aspect;
        } else {
            half_w = half_h * img_aspect;
        }
    }
    
    /* Clamp size to prevent huge sprites */
    float max_dim = g_m3g.viewport_width * 0.5f;
    if (half_w > max_dim) half_w = max_dim;
    if (half_h > max_dim) half_h = max_dim;
    
    /* Get pixel data from Image2D */
    JavaArray* pixels = (JavaArray*)m3g_get_ref_field(image, "pixels");
    if (!pixels || pixels->element_type != T_INT) return;
    
    jint* src_pixels = (jint*)array_data(pixels);
    int src_count = (int)pixels->length;
    if (!src_pixels || src_count < img_width * img_height) return;
    
    /* Draw the sprite image to the color buffer with alpha blending.
     * screen_x, screen_y is the CENTER of the sprite on screen. */
    int dst_left = (int)(screen_x - half_w);
    int dst_top = (int)(screen_y - half_h);
    int dst_right = (int)(screen_x + half_w);
    int dst_bottom = (int)(screen_y + half_h);
    
    /* Clip to viewport */
    if (dst_left < 0) dst_left = 0;
    if (dst_top < 0) dst_top = 0;
    if (dst_right >= g_m3g.buffer_width) dst_right = g_m3g.buffer_width - 1;
    if (dst_bottom >= g_m3g.buffer_height) dst_bottom = g_m3g.buffer_height - 1;
    
    if (dst_right <= dst_left || dst_bottom <= dst_top) return;
    
    int draw_w = dst_right - dst_left;
    int draw_h = dst_bottom - dst_top;
    
    /* Source UV coordinates */
    float u_start = (float)(dst_left - (int)(screen_x - half_w)) / (2.0f * half_w);
    float v_start = (float)(dst_top - (int)(screen_y - half_h)) / (2.0f * half_h);
    float u_end = (float)(dst_right - (int)(screen_x - half_w)) / (2.0f * half_w);
    float v_end = (float)(dst_bottom - (int)(screen_y - half_h)) / (2.0f * half_h);
    
    /* Blit with alpha blending */
    uint32_t* dst = g_m3g.color_buffer;
    for (int dy = 0; dy < draw_h; dy++) {
        float v = v_start + (v_end - v_start) * (float)dy / (float)draw_h;
        int src_y = (int)(v * img_height);
        if (src_y < 0) src_y = 0;
        if (src_y >= img_height) src_y = img_height - 1;
        
        int dst_y = dst_top + dy;
        for (int dx = 0; dx < draw_w; dx++) {
            float u = u_start + (u_end - u_start) * (float)dx / (float)draw_w;
            int src_x = (int)(u * img_width);
            if (src_x < 0) src_x = 0;
            if (src_x >= img_width) src_x = img_width - 1;
            
            int dst_x = dst_left + dx;
            uint32_t fg = (uint32_t)src_pixels[src_y * img_width + src_x];
            int fgA = (fg >> 24) & 0xFF;
            
            if (fgA == 0) continue;  /* Fully transparent */
            if (fgA == 255) {
                /* Fully opaque */
                dst[dst_y * g_m3g.buffer_width + dst_x] = fg;
            } else {
                /* Alpha blend */
                uint32_t bg = dst[dst_y * g_m3g.buffer_width + dst_x];
                int bgR = (bg >> 16) & 0xFF;
                int bgG = (bg >> 8) & 0xFF;
                int bgB = bg & 0xFF;
                int fgR = (fg >> 16) & 0xFF;
                int fgG = (fg >> 8) & 0xFF;
                int fgB = fg & 0xFF;
                int invA = 255 - fgA;
                int outR = (fgR * fgA + bgR * invA) / 255;
                int outG = (fgG * fgA + bgG * invA) / 255;
                int outB = (fgB * fgA + bgB * invA) / 255;
                int outA = fgA + ((bg >> 24) & 0xFF) * invA / 255;
                dst[dst_y * g_m3g.buffer_width + dst_x] = 
                    ((outA & 0xFF) << 24) | ((outR & 0xFF) << 16) | ((outG & 0xFF) << 8) | (outB & 0xFF);
            }
        }
    }
    
    g_m3g.triangles_rendered += 2;  /* Count as 2 triangles */
}

/* Render a single Mesh with the given model-view-projection matrix */
/* Forward declaration */
static void m3g_rasterize_triangle(const float* v0, const float* v1, const float* v2,
                                   const float* tc0, const float* tc1, const float* tc2,
                                   const uint8_t* c0, const uint8_t* c1, const uint8_t* c2,
                                   M3GAppearance* appearance);

static void m3g_render_single_mesh(JVM* jvm, JavaObject* mesh, M3GTransform* mvp_matrix) {
    (void)jvm;

    /* DIAG: log MVP matrix for first call */
    static int mvp_diag_count = 0;
    if (mvp_diag_count < 2) {
        mvp_diag_count++;
        fprintf(stderr, "[M3G-MVP] MVP matrix row0=(%.3f,%.3f,%.3f,%.3f)\n", mvp_matrix->m[0], mvp_matrix->m[4], mvp_matrix->m[8], mvp_matrix->m[12]);
        fprintf(stderr, "[M3G-MVP] MVP matrix row1=(%.3f,%.3f,%.3f,%.3f)\n", mvp_matrix->m[1], mvp_matrix->m[5], mvp_matrix->m[9], mvp_matrix->m[13]);
        fprintf(stderr, "[M3G-MVP] MVP matrix row2=(%.3f,%.3f,%.3f,%.3f)\n", mvp_matrix->m[2], mvp_matrix->m[6], mvp_matrix->m[10], mvp_matrix->m[14]);
        fprintf(stderr, "[M3G-MVP] MVP matrix row3=(%.3f,%.3f,%.3f,%.3f)\n", mvp_matrix->m[3], mvp_matrix->m[7], mvp_matrix->m[11], mvp_matrix->m[15]);
    }

    JavaObject* vertexBuffer = m3g_get_ref_field(mesh, "vertexBuffer");
    JavaObject* indexBuffer = m3g_get_ref_field(mesh, "indexBuffer");

    if (!vertexBuffer || !indexBuffer) {
        fprintf(stderr, "[M3G] renderMesh: missing vertexBuffer=%p or indexBuffer=%p\n", 
                (void*)vertexBuffer, (void*)indexBuffer);
        return;
    }

    /* Get position array from vertex buffer */
    JavaObject* positions = m3g_get_ref_field(vertexBuffer, "positions");
    if (!positions) {
        fprintf(stderr, "[M3G] renderMesh: missing positions\n");
        return;
    }

    int vertex_count = m3g_get_int_field(positions, "vertexCount", 0);
    int pos_components = m3g_get_int_field(positions, "componentCount", 3);
    int pos_size = m3g_get_int_field(positions, "componentSize", 1);
    float pos_scale = m3g_get_float_field(vertexBuffer, "positionScale", 1.0f);
    float bias_x = m3g_get_float_field(vertexBuffer, "biasX", 0.0f);
    float bias_y = m3g_get_float_field(vertexBuffer, "biasY", 0.0f);
    float bias_z = m3g_get_float_field(vertexBuffer, "biasZ", 0.0f);

    if (vertex_count == 0) return;
    if (vertex_count > M3G_MAX_VERTEX_COUNT) {
        fprintf(stderr, "[M3G] renderMesh: vertex_count=%d exceeds max %d\n", vertex_count, M3G_MAX_VERTEX_COUNT);
        return;
    }

    JavaArray* pos_data = (JavaArray*)m3g_get_ref_field(positions, "data");
    if (!pos_data) {
        fprintf(stderr, "[M3G] renderMesh: missing position data array\n");
        return;
    }

    /* Validate pos_data length against expected size */
    {
        int expected_len = vertex_count * pos_components;
        if (expected_len > (int)pos_data->length) {
            fprintf(stderr, "[M3G] renderMesh: pos_data len=%d < expected=%d (verts=%d, comp=%d)\n",
                    (int)pos_data->length, expected_len, vertex_count, pos_components);
        }
    }

    /* Get index data - use array length as fallback for indexCount */
    JavaArray* idx_data = (JavaArray*)m3g_get_ref_field(indexBuffer, "indices");
    if (!idx_data) {
        fprintf(stderr, "[M3G] renderMesh: missing indices array on indexBuffer %p\n", (void*)indexBuffer);
        return;
    }
    int index_count = m3g_get_int_field(indexBuffer, "indexCount", 0);
    /* Fallback: if indexCount field not set or 0, use the indices array length */
    if (index_count <= 0) {
        index_count = (int)idx_data->length;
    }
    if (index_count == 0) {
        fprintf(stderr, "[M3G] renderMesh: index_count=0 (indices array len=%d)\n", (int)idx_data->length);
        return;
    }
    if (index_count > M3G_MAX_INDEX_COUNT) return;

    GFX_DEBUG("m3g_render_single_mesh: %d verts, %d indices", vertex_count, index_count);
    fprintf(stderr, "[M3G] renderMesh: mesh=%p vb=%p pos=%p (class=%s), %d verts (comp=%d, size=%d, scale=%.3f, bias=%.1f,%.1f,%.1f), %d indices\n", 
            (void*)mesh, (void*)vertexBuffer, (void*)positions,
            positions && positions->header.clazz ? positions->header.clazz->class_name : "?",
            vertex_count, pos_components, pos_size, pos_scale,
            bias_x, bias_y, bias_z, index_count);
    fprintf(stderr, "[M3G-POST-LOG-TEST] mesh=%p reached line after renderMesh log\n", (void*)mesh); fflush(stderr);

    /* Convert positions to float array - use render pool to avoid per-frame malloc */
    /* ALWAYS expand to 3 components per vertex (X, Y, Z) regardless of source comp.
     * M3G position VertexArrays should have comp=3, but some files use comp=2.
     * When comp < 3, missing components default to 0 (then bias is applied). */
    float* vertices = m3g_render_pool_alloc_vertices(vertex_count * 3);
    if (!vertices) return;

    /* Zero-fill the entire vertices array so missing Z components are 0.0f */
    memset(vertices, 0, vertex_count * 3 * sizeof(float));

    /* Pre-compute loop limit and scale for position conversion.
     * Use size_t arithmetic to prevent int overflow when vertex_count * pos_components
     * exceeds INT_MAX (e.g., vertex_count=200000, pos_components=3 → 600000 fits,
     * but vertex_count=INT_MAX, pos_components=4 → overflow). */
    size_t pos_total_safe = (size_t)vertex_count * (size_t)pos_components;
    if (pos_total_safe > (size_t)pos_data->length) pos_total_safe = (size_t)pos_data->length;

    if (pos_components == 3) {
        /* Fast path: source has exactly 3 components — copy directly */
        if (pos_size == 1) {
            int8_t* src = (int8_t*)array_data(pos_data);
            float* dst = vertices;
            int count = (int)pos_total_safe;
            int i = 0;
            while (i + 3 <= count) {
                dst[0] = (float)src[0] * pos_scale + bias_x;
                dst[1] = (float)src[1] * pos_scale + bias_y;
                dst[2] = (float)src[2] * pos_scale + bias_z;
                src += 3; dst += 3; i += 3;
            }
            while (i < count) {
                *dst++ = (float)*src++ * pos_scale;
                i++;
            }
        } else {
            int16_t* src = (int16_t*)array_data(pos_data);
            float* dst = vertices;
            int count = (int)pos_total_safe;
            int i = 0;
            while (i + 3 <= count) {
                dst[0] = (float)src[0] * pos_scale + bias_x;
                dst[1] = (float)src[1] * pos_scale + bias_y;
                dst[2] = (float)src[2] * pos_scale + bias_z;
                src += 3; dst += 3; i += 3;
            }
            while (i < count) {
                *dst++ = (float)*src++ * pos_scale;
                i++;
            }
        }
    } else {
        /* Slow path: expand N-component source to 3-component output.
         * Source layout: [v0_c0, v0_c1, ..., v1_c0, v1_c1, ...]
         * Output layout: [v0_x, v0_y, v0_z, v1_x, v1_y, v1_z, ...] */
        int comp = pos_components < 1 ? 1 : (pos_components > 4 ? 4 : pos_components);
        if (pos_size == 1) {
            int8_t* src = (int8_t*)array_data(pos_data);
            for (int i = 0; i < vertex_count; i++) {
                int base = i * comp;
                if (base + comp <= (int)pos_data->length) {
                    vertices[i*3 + 0] = (float)src[base + 0] * pos_scale + bias_x;
                    if (comp >= 2) vertices[i*3 + 1] = (float)src[base + 1] * pos_scale + bias_y;
                    if (comp >= 3) vertices[i*3 + 2] = (float)src[base + 2] * pos_scale + bias_z;
                    else vertices[i*3 + 2] = 0.0f;
                }
            }
        } else {
            int16_t* src = (int16_t*)array_data(pos_data);
            for (int i = 0; i < vertex_count; i++) {
                int base = i * comp;
                if (base + comp <= (int)pos_data->length) {
                    vertices[i*3 + 0] = (float)src[base + 0] * pos_scale + bias_x;
                    if (comp >= 2) vertices[i*3 + 1] = (float)src[base + 1] * pos_scale + bias_y;
                    if (comp >= 3) vertices[i*3 + 2] = (float)src[base + 2] * pos_scale + bias_z;
                    else vertices[i*3 + 2] = 0.0f;
                }
            }
        }
    }

    /* NOTE: Bias was already applied during vertex conversion above
     * (each src[i] * scale + bias). Do NOT apply it again here.
     * Previous code had a double-bias bug that shifted all vertices. */

    /* =====================================================================
     * COMP=2 VERTEX HANDLING
     * =====================================================================
     * M3G comp=2 vertices store (X,Y) only with no Z. After scale+bias,
     * they are in object space. We expand to (X, Y, Z_eps, 1.0) where
     * Z_eps = 0.01 prevents clip.w = 0 after perspective projection
     * (which would cause divide-by-zero in perspective divide).
     *
     * IMPORTANT: comp=2 vertices are NOT in screen space. They have
     * world-space coordinates and MUST go through the normal 3D MVP
     * pipeline. The Z=0 after modelview would put them at the camera
     * plane, giving clip.w ≈ 0. Adding a small Z offset fixes this.
     *
     * Previous approaches that used orthographic screen-space MVP were
     * WRONG because they assumed comp=2 = pixel coordinates, but games
     * like SU-30 use comp=2 for 2D geometry in world space.
     * ============================================================ */
    /* Comp=2 vertices will use normal MVP but with Z expanded to 0.01 */
    
    /* Get indices */
    uint16_t* indices = NULL;
    int need_free_indices = 0;

    /* Handle different index element types */
    if (idx_data->element_type == T_INT) {
        /* int[] indices - need to downcast to uint16_t */
        jint* src = (jint*)array_data(idx_data);
        indices = (uint16_t*)malloc(index_count * sizeof(uint16_t));
        need_free_indices = 1;
        if (indices) {
            int copy_count = index_count < (int)idx_data->length ? index_count : (int)idx_data->length;
            for (int i = 0; i < copy_count; i++) {
                indices[i] = (uint16_t)(src[i] & 0xFFFF);
            }
        }
    } else if (idx_data->element_type == T_SHORT) {
        /* short[] indices - cast directly */
        indices = (uint16_t*)array_data(idx_data);
    } else if (idx_data->element_type == T_BYTE) {
        /* byte[] indices - need to expand to uint16_t */
        int8_t* src = (int8_t*)array_data(idx_data);
        indices = (uint16_t*)malloc(index_count * sizeof(uint16_t));
        need_free_indices = 1;
        if (indices) {
            int copy_count = index_count < (int)idx_data->length ? index_count : (int)idx_data->length;
            for (int i = 0; i < copy_count; i++) {
                indices[i] = (uint16_t)(src[i] & 0xFF);
            }
        }
    } else {
        /* Unknown index type - log and skip */
        fprintf(stderr, "[M3G] renderMesh: unsupported index element_type=%d (mesh=%p)\n",
                idx_data->element_type, (void*)mesh);
    }

    if (!indices) {
        /* vertices is pool-allocated, no free needed */
        return;
    }

    /* Resolve material diffuse color as default.
     * In M3G, the diffuse color is 0xAARRGGBB but many files store it as 0x00RRGGBB
     * where the alpha byte is 0 (not transparent — alpha is controlled by
     * CompositingMode, not the material color). Always set alpha=255 for visibility. */
    uint8_t default_color[4] = {200, 200, 200, 255};
    JavaObject* appearance = m3g_get_ref_field(mesh, "appearance");
    if (appearance) {
        JavaObject* material = m3g_get_ref_field(appearance, "material");
        if (material) {
            jint diffuse_argb = m3g_get_int_field(material, "diffuse", 0x00CCCCCC);
            default_color[0] = (diffuse_argb >> 16) & 0xFF;
            default_color[1] = (diffuse_argb >> 8) & 0xFF;
            default_color[2] = diffuse_argb & 0xFF;
            default_color[3] = 255;  /* Always opaque — M3G uses CompositingMode for transparency */
            GFX_DEBUG("M3G: material diffuse=0x%08X -> color=(%d,%d,%d,%d)",
                     diffuse_argb, default_color[0], default_color[1], default_color[2], default_color[3]);
        }
    }

    /* Check for per-vertex colors */
    JavaObject* color_va = m3g_get_ref_field(vertexBuffer, "colors");

    /* Check for texture coordinates and build texture */
    JavaObject* texcoord_va = m3g_get_ref_field(vertexBuffer, "texCoords");
    float tex_coord_scale = m3g_get_float_field(vertexBuffer, "texCoordScale", 1.0f);

    /* Build texture from Appearance's Texture2D */
    M3GTexture2D* texture = NULL;
    M3GAppearance m3g_appearance;
    memset(&m3g_appearance, 0, sizeof(m3g_appearance));

    /* FIX 41: Read PolygonMode and CompositingMode from Appearance.
     * Previously these were discarded during M3G parsing (references not saved).
     * Now they're properly parsed and linked. Read them per-mesh to control
     * culling, winding, depth test/write, and blending for each mesh. */
    int mesh_culling = -1;       /* -1 = use global default */
    int mesh_winding = 0;        /* 0 = CCW (default), 1 = CW */
    int mesh_depth_test = -1;    /* -1 = use global default */
    int mesh_depth_write = -1;
    int mesh_blend_mode = -1;
    int mesh_alpha_threshold = -1;
    int mesh_two_sided = 0;

    if (appearance) {
        JavaObject* texture_obj = m3g_get_ref_field(appearance, "texture");
        if (texture_obj) {
            texture = m3g_build_texture2d(texture_obj);
            m3g_appearance.texture = texture;
        }

        /* Read PolygonMode */
        JavaObject* polygon_mode = m3g_get_ref_field(appearance, "polygonMode");
        if (polygon_mode) {
            int culling_val = m3g_get_int_field(polygon_mode, "culling", -1);
            int winding_val = m3g_get_int_field(polygon_mode, "winding", 0);
            int two_sided_val = m3g_get_int_field(polygon_mode, "twoSidedLighting", 0);
            /* Map culling: accept both Java API enums (160-162) and raw M3G bytes (0-2).
             * Java API: CULL_NONE=160, CULL_BACK=161, CULL_FRONT=162
             * M3G binary: 0=NONE, 1=BACK, 2=FRONT */
            int cull_raw = culling_val;
            if (cull_raw >= 160 && cull_raw <= 162) cull_raw -= 160; /* Java enum → 0-2 */
            if (cull_raw == 0) mesh_culling = 0;      /* CULL_NONE */
            else if (cull_raw == 1) mesh_culling = 1;  /* CULL_BACK */
            else if (cull_raw == 2) mesh_culling = 2;  /* CULL_FRONT */
            else mesh_culling = (cull_raw > 0) ? 1 : 0;
            /* Map winding: accept both Java API enums (163-164) and raw bytes (0-1) */
            int wind_raw = winding_val;
            if (wind_raw >= 163 && wind_raw <= 164) wind_raw -= 163; /* Java enum → 0-1 */
            mesh_winding = (wind_raw == 1) ? 1 : 0;  /* CW if 1, else CCW */
            mesh_two_sided = (two_sided_val != 0) ? 1 : 0;
            fprintf(stderr, "[M3G-POLY] mesh=%p: culling_raw=%d->%d winding_raw=%d->%d twoSided=%d\n",
                    (void*)mesh, culling_val, mesh_culling, winding_val, mesh_winding, two_sided_val);
        }

        /* Read CompositingMode */
        JavaObject* comp_mode = m3g_get_ref_field(appearance, "compositingMode");
        if (comp_mode) {
            /* Parser now reads in correct M3G binary order:
             * DepthTest(u8), DepthWrite(u8), ColorWrite(u8),
             * Blending(u8), AlphaThreshold(f32), Dithering(u8) */
            int blending_val = m3g_get_int_field(comp_mode, "blending", 64);
            int depth_test_val = m3g_get_int_field(comp_mode, "depthTest", 1);
            int depth_write_val = m3g_get_int_field(comp_mode, "depthWrite", 1);
            float alpha_thresh = m3g_get_float_field(comp_mode, "alphaThreshold", 0.0f);
            if (alpha_thresh < 0.0f) alpha_thresh = 0.0f;
            if (alpha_thresh > 1.0f) alpha_thresh = 0.0f; /* Invalid, disable */
            
            /* Map blending: M3G binary uses 0-4, renderer uses 64-68.
             * M3G: 0=REPLACE, 1=ALPHA, 2=ALPHA_ADD, 3=MODULATE, 4=MODULATE_X2
             * Renderer: 64=ALPHA, 65=ALPHA_ADD, 67=REPLACE, 68=MODULATE */
            int blend_raw = blending_val;
            if (blend_raw >= 160) {
                /* Java API enum: REPLACE=192, ALPHA=193, etc. → internal */
                if (blend_raw == 192) blend_raw = 67;      /* REPLACE */
                else if (blend_raw == 193) blend_raw = 64;  /* ALPHA */
                else if (blend_raw == 194) blend_raw = 65;  /* ALPHA_ADD */
                else if (blend_raw == 195) blend_raw = 68;  /* MODULATE */
                else blend_raw = 64; /* fallback to ALPHA */
            } else {
                /* M3G binary: 0=REPLACE, 1=ALPHA, 2=ALPHA_ADD, 3=MODULATE */
                if (blend_raw == 0) blend_raw = 67;      /* REPLACE */
                else if (blend_raw == 1) blend_raw = 64;  /* ALPHA */
                else if (blend_raw == 2) blend_raw = 65;  /* ALPHA_ADD */
                else if (blend_raw == 3) blend_raw = 68;  /* MODULATE */
                else blend_raw = 64; /* fallback to ALPHA */
            }
            mesh_blend_mode = blend_raw;
            
            /* Use actual depth test/write values from the (now correctly parsed) CompositingMode */
            mesh_depth_test = (depth_test_val != 0) ? 1 : 0;
            mesh_depth_write = (depth_write_val != 0) ? 1 : 0;
            mesh_alpha_threshold = (int)(alpha_thresh * 255.0f);
            fprintf(stderr, "[M3G-COMP] mesh=%p: blending=%d depthTest=%d depthWrite=%d alphaThresh=%.2f\n",
                    (void*)mesh, blending_val, depth_test_val, depth_write_val, alpha_thresh);
        }

        /* Read texture blend mode from Texture2D */
        if (texture_obj) {
            /* Texture2D blending: FUNC_REPLACE=160, FUNC_MODULATE=161, FUNC_DECAL=162,
             * FUNC_ADD=163, FUNC_BLEND=164 (Java API constants) */
            int tex_blend = m3g_get_int_field(texture_obj, "blending", 161);
            m3g_appearance.texture_blend = tex_blend;
        }
    }

    /* Save and override global render state for this mesh */
    int saved_culling = g_m3g.culling_enabled;
    int saved_depth_test = g_m3g.depth_test_enabled;
    int saved_depth_write = g_m3g.depth_write_enabled;

    /* mesh_culling: 0=CULL_NONE, 1=CULL_BACK, 2=CULL_FRONT */
    if (mesh_culling >= 0) g_m3g.culling_enabled = (mesh_culling != 0) ? 1 : 0;
    if (mesh_depth_test >= 0) g_m3g.depth_test_enabled = mesh_depth_test;
    if (mesh_depth_write >= 0) g_m3g.depth_write_enabled = mesh_depth_write;

    /* Store mesh-level winding/culling in appearance struct for rasterizer */
    m3g_appearance.winding = mesh_winding;
    m3g_appearance.two_sided_lighting = mesh_two_sided;
    m3g_appearance.cull_front = (mesh_culling == 2);  /* CULL_FRONT */

    /* Read texcoord data if available */
    float* tex_coords = NULL;
    int tex_components = 2;
    if (texcoord_va && texture) {
        int tex_vert_count = m3g_get_int_field(texcoord_va, "vertexCount", 0);
        tex_components = m3g_get_int_field(texcoord_va, "componentCount", 2);
        int tex_comp_size = m3g_get_int_field(texcoord_va, "componentSize", 1);
        JavaArray* tex_data = (JavaArray*)m3g_get_ref_field(texcoord_va, "data");

        if (tex_data && tex_vert_count > 0 && tex_vert_count == vertex_count) {
            tex_coords = (float*)malloc(vertex_count * 2 * sizeof(float));
            if (tex_coords) {
                if (tex_comp_size == 1) {
                    int8_t* src = (int8_t*)array_data(tex_data);
                    for (int i = 0; i < vertex_count * tex_components && i < (int)tex_data->length; i++) {
                        tex_coords[i] = (float)src[i] * tex_coord_scale;
                    }
                } else {
                    int16_t* src = (int16_t*)array_data(tex_data);
                    for (int i = 0; i < vertex_count * tex_components && i < (int)tex_data->length; i++) {
                        tex_coords[i] = (float)src[i] * tex_coord_scale;
                    }
                }
            }
        }
    }

    /* Render triangles — vertices are now ALWAYS 3-component (X,Y,Z) */
    int mesh_triangles = 0;
    for (int t = 0; t + 2 < index_count; t += 3) {
        float v0[4], v1[4], v2[4];
        float clip0[4], clip1[4], clip2[4];

        int i0 = indices[t];
        int i1 = indices[t+1];
        int i2 = indices[t+2];

        /* Validate vertex indices */
        if (i0 >= 0 && i0 < vertex_count &&
            i1 >= 0 && i1 < vertex_count &&
            i2 >= 0 && i2 < vertex_count) {

            /* Load vertex positions (always 3-component X,Y,Z) */
            v0[0] = vertices[i0*3]; v0[1] = vertices[i0*3+1]; v0[2] = vertices[i0*3+2]; v0[3] = 1.0f;
            v1[0] = vertices[i1*3]; v1[1] = vertices[i1*3+1]; v1[2] = vertices[i1*3+2]; v1[3] = 1.0f;
            v2[0] = vertices[i2*3]; v2[1] = vertices[i2*3+1]; v2[2] = vertices[i2*3+2]; v2[3] = 1.0f;

            /* For comp=2 vertices, Z is always 0 (no Z component in data).
             * The auto-camera places the camera looking at Z=0 from a positive Z,
             * so vertices at Z=0 will be at view_z = -view_dist (in front of camera).
             * No special Z offset needed — the auto-camera handles this correctly.
             * Previously we set Z=0.01 which put vertices BEHIND the camera
             * (clip.w < 0) when the camera looked along -Z from positive Z. */
            /* (no Z hack needed for comp=2 — auto-camera handles Z=0 correctly) */

            /* Apply MVP transformation */
            m3g_transform_point(clip0, mvp_matrix, v0);
            m3g_transform_point(clip1, mvp_matrix, v1);
            m3g_transform_point(clip2, mvp_matrix, v2);

            /* DIAG: log first triangle clip coords per mesh */
            if (mesh_triangles == 0 && g_m3g_clip_triangles_in == 0) {
                fprintf(stderr, "[M3G-CLIP] comp=%d v0=(%.3f,%.3f,%.3f) v1=(%.3f,%.3f,%.3f) v2=(%.3f,%.3f,%.3f)\n",
                        pos_components, v0[0], v0[1], v0[2], v1[0], v1[1], v1[2], v2[0], v2[1], v2[2]);
                fprintf(stderr, "[M3G-CLIP] clip0=(%.3f,%.3f,%.3f,w=%.3f) clip1=(%.3f,%.3f,%.3f,w=%.3f) clip2=(%.3f,%.3f,%.3f,w=%.3f)\n",
                        clip0[0], clip0[1], clip0[2], clip0[3],
                        clip1[0], clip1[1], clip1[2], clip1[3],
                        clip2[0], clip2[1], clip2[2], clip2[3]);
                /* Compute predicted screen coords for diagnosis */
                if (clip0[3] > 0.001f) {
                    float sw = clip0[3];
                    float sx = (clip0[0]/sw + 1.0f) * 0.5f * g_m3g.viewport_width;
                    float sy = (1.0f - clip0[1]/sw) * 0.5f * g_m3g.viewport_height;
                    fprintf(stderr, "[M3G-CLIP] Predicted screen0=(%.1f,%.1f) depth=%.3f\n", sx, sy, clip0[2]/sw);
                } else {
                    fprintf(stderr, "[M3G-CLIP] clip0.w=%.3f BEHIND CAMERA (w < 0.001)\n", clip0[3]);
                }
            }

            /* Determine per-vertex colors */
            uint8_t c0[4], c1[4], c2[4];
            uint8_t* pc0 = NULL, *pc1 = NULL, *pc2 = NULL;

            if (color_va) {
                JavaArray* color_data = (JavaArray*)m3g_get_ref_field(color_va, "data");
                int color_components = m3g_get_int_field(color_va, "componentCount", 4);
                int color_size = m3g_get_int_field(color_va, "componentSize", 1);

                if (color_data) {
                    #define READ_VCOLOR(out, vert_idx) do { \
                        memset(out, 0, 4); \
                        if (color_size == 1) { \
                            uint8_t* cd = (uint8_t*)array_data(color_data); \
                            int base = (vert_idx) * color_components; \
                            if (base + color_components - 1 < (int)color_data->length) { \
                                if (color_components == 3) { \
                                    out[0] = cd[base]; out[1] = cd[base+1]; out[2] = cd[base+2]; out[3] = 255; \
                                } else { \
                                    out[0] = cd[base]; out[1] = cd[base+1]; out[2] = cd[base+2]; out[3] = cd[base+3]; \
                                } \
                            } \
                        } else { \
                            int16_t* cd = (int16_t*)array_data(color_data); \
                            int base = (vert_idx) * color_components; \
                            if (base + color_components - 1 < (int)color_data->length) { \
                                if (color_components == 3) { \
                                    out[0] = (uint8_t)cd[base]; out[1] = (uint8_t)cd[base+1]; out[2] = (uint8_t)cd[base+2]; out[3] = 255; \
                                } else { \
                                    out[0] = (uint8_t)cd[base]; out[1] = (uint8_t)cd[base+1]; out[2] = (uint8_t)cd[base+2]; out[3] = (uint8_t)cd[base+3]; \
                                } \
                            } \
                        } \
                    } while(0)

                    READ_VCOLOR(c0, indices[t]);
                    READ_VCOLOR(c1, indices[t+1]);
                    READ_VCOLOR(c2, indices[t+2]);
                    pc0 = c0; pc1 = c1; pc2 = c2;
                }
            }

            /* If no vertex colors, use material default color */
            if (!pc0) {
                memcpy(c0, default_color, 4);
                memcpy(c1, default_color, 4);
                memcpy(c2, default_color, 4);
                pc0 = c0; pc1 = c1; pc2 = c2;
            }

            /* Get texture coordinates for this triangle */
            float tc0[2] = {0, 0}, tc1[2] = {0, 0}, tc2[2] = {0, 0};
            float* ptc0 = NULL, *ptc1 = NULL, *ptc2 = NULL;

            if (tex_coords && texture) {
                int ti0 = indices[t] * tex_components;
                int ti1 = indices[t+1] * tex_components;
                int ti2 = indices[t+2] * tex_components;
                if (ti0 + 1 < vertex_count * tex_components &&
                    ti1 + 1 < vertex_count * tex_components &&
                    ti2 + 1 < vertex_count * tex_components) {
                    tc0[0] = tex_coords[ti0]; tc0[1] = tex_coords[ti0+1];
                    tc1[0] = tex_coords[ti1]; tc1[1] = tex_coords[ti1+1];
                    tc2[0] = tex_coords[ti2]; tc2[1] = tex_coords[ti2+1];
                    ptc0 = tc0; ptc1 = tc1; ptc2 = tc2;
                }
            }

            /* Near-plane clipping using Sutherland-Hodgman */
            g_m3g_clip_triangles_in++;
            {
                const float* clip_in[3] = { clip0, clip1, clip2 };
                const uint8_t* color_in[3] = { pc0, pc1, pc2 };
                const float* tex_in[3] = { ptc0, ptc1, ptc2 };
                float clip_out[6][4];
                uint8_t color_out[6][4];
                float tex_out[6][2];

                int num_tris = m3g_clip_triangle_near(clip_in, color_in, tex_in,
                                                       clip_out, color_out, tex_out);

                for (int ct = 0; ct < num_tris; ct++) {
                    int base = ct * 3;
                    m3g_rasterize_triangle(clip_out[base], clip_out[base+1], clip_out[base+2],
                                           ptc0 ? tex_out[base] : NULL,
                                           ptc0 ? tex_out[base+1] : NULL,
                                           ptc0 ? tex_out[base+2] : NULL,
                                           color_out[base], color_out[base+1], color_out[base+2],
                                           &m3g_appearance);  /* FIX 44: Always pass appearance for culling/blending */
                }
                g_m3g_clip_triangles_out += num_tris;
            }
        }
    }

    fprintf(stderr, "[M3G] renderMesh: clipped %d→%d triangles (total rendered=%d, pixels=%d)\n",
            g_m3g_clip_triangles_in, g_m3g_clip_triangles_out,
            g_m3g.triangles_rendered, g_m3g_raster_pixels_written);

    /* Restore global render state after this mesh */
    g_m3g.culling_enabled = saved_culling;
    g_m3g.depth_test_enabled = saved_depth_test;
    g_m3g.depth_write_enabled = saved_depth_write;

    if (need_free_indices) free(indices);
    /* vertices is pool-allocated, no free needed */
    if (tex_coords) free(tex_coords);
    if (texture) {
        if (texture->pixels) free(texture->pixels);
        free(texture);
    }
}

/* Recursively render a node and its children, applying transforms */
static void m3g_render_node_recursive(JVM* jvm, JavaObject* node, M3GTransform* parent_modelview) {
    if (!node) return;

    JavaClass* clazz = node->header.clazz;
    if (!clazz || !clazz->class_name) return;

    const char* class_name = clazz->class_name;

    /* Build this node's transform */
    M3GTransform node_transform;
    m3g_build_node_transform(node, &node_transform);

    /* Combine with parent: modelview = parent * node */
    M3GTransform combined_modelview;
    m3g_transform_multiply(&combined_modelview, parent_modelview, &node_transform);

    /* Build MVP = projection * modelview (OpenGL convention) */
    M3GTransform mvp;
    m3g_transform_multiply(&mvp, &g_m3g.projection, &combined_modelview);

    /* If this node is a Mesh, render it */
    if (strstr(class_name, "Mesh") != NULL) {
        /* Check rendering enable */
        int render_enable = m3g_get_int_field(node, "renderingEnable", 1);
        fprintf(stderr, "[M3G] renderNode: class=%s, enable=%d\n", class_name, render_enable);
        if (render_enable) {
            m3g_render_single_mesh(jvm, node, &mvp);
        }
        return;
    }

    /* If this node is a Sprite3D, render it as a billboard */
    if (strstr(class_name, "Sprite3D") != NULL) {
        fprintf(stderr, "[M3G] renderNode: Sprite3D at (%.1f, %.1f, %.1f)\n",
                m3g_get_float_field(node, "translationX", 0),
                m3g_get_float_field(node, "translationY", 0),
                m3g_get_float_field(node, "translationZ", 0));
        m3g_render_sprite3d(jvm, node, &mvp);
        return;
    }

    /* If this node is a Group or World, recurse into children */
    if (strstr(class_name, "Group") != NULL || strstr(class_name, "World") != NULL) {
        int children_slot = m3g_find_field_slot(node, "children");
        JavaArray* children = (JavaArray*)m3g_get_ref_field(node, "children");
        fprintf(stderr, "[M3G] renderNode: class=%s, children_slot=%d, children=%p, count=%d, elemType=%d\n", 
                class_name, children_slot,
                (void*)children, children ? (int)children->length : -1,
                children ? (int)children->element_type : -1);
        if (children && children->length > 0 && children->element_type == DESC_OBJECT) {
            JavaObject** child_arr = (JavaObject**)array_data(children);
            int non_null = 0;
            if (!child_arr) return;
            /* Use childCount field to limit iteration - children array may be oversized */
            int safe_count = m3g_get_int_field(node, "childCount", (int)children->length);
            if (safe_count > (int)children->length) safe_count = (int)children->length;
            for (jsize c = 0; c < safe_count; c++) {
                if (child_arr[c] && child_arr[c]->header.clazz) {
                    non_null++;
                    fprintf(stderr, "[M3G] renderNode: child[%d]=%p class=%s\n",
                            c, (void*)child_arr[c],
                            child_arr[c]->header.clazz ? child_arr[c]->header.clazz->class_name : "?");
                    m3g_render_node_recursive(jvm, child_arr[c], &combined_modelview);
                }
            }
            if (non_null == 0) {
                fprintf(stderr, "[M3G] renderNode: all %d children are NULL\n", (int)children->length);
            }
        } else if (children && children->element_type != DESC_OBJECT) {
            fprintf(stderr, "[M3G] renderNode: children array has wrong element_type=%d (expected DESC_OBJECT=%d)\n",
                    (int)children->element_type, (int)DESC_OBJECT);
        }
    }
}

/* Pixel blending modes matching FreeJ2ME Graphics3D.blendPixels */
static uint32_t m3g_blend_pixels(uint32_t bg, uint32_t fg, int alpha, int blend_mode) {
    int bgA = (bg >> 24) & 0xFF;
    int bgR = (bg >> 16) & 0xFF;
    int bgG = (bg >> 8) & 0xFF;
    int bgB = bg & 0xFF;

    int fgA = (fg >> 24) & 0xFF;
    int fgR = (fg >> 16) & 0xFF;
    int fgG = (fg >> 8) & 0xFF;
    int fgB = fg & 0xFF;

    int outR, outG, outB, outA;
    float alphaNorm = alpha / 255.0f;

    #define CLAMP8(v) ((v) < 0 ? 0 : ((v) > 255 ? 255 : (v)))

    switch (blend_mode) {
        /* CompositingMode.REPLACE / Texture2D.FUNC_REPLACE */
        case 67:
            outA = CLAMP8((int)(fgA + bgA * (1 - fgA / 255.0f)));
            outR = CLAMP8((int)(fgR * (fgA / 255.0f) + bgR * (1 - fgA / 255.0f)));
            outG = CLAMP8((int)(fgG * (fgA / 255.0f) + bgG * (1 - fgA / 255.0f)));
            outB = CLAMP8((int)(fgB * (fgA / 255.0f) + bgB * (1 - fgA / 255.0f)));
            return (outA << 24) | (outR << 16) | (outG << 8) | outB;

        /* CompositingMode.ALPHA_ADD */
        case 65:
            outR = (int)fminf(255.0f, fgR * alphaNorm + bgR);
            outG = (int)fminf(255.0f, fgG * alphaNorm + bgG);
            outB = (int)fminf(255.0f, fgB * alphaNorm + bgB);
            outA = (int)fminf(255.0f, bgA + (int)(alpha * (1 - bgA / 255.0f)));
            return (outA << 24) | (outR << 16) | (outG << 8) | outB;

        /* CompositingMode.ALPHA / Texture2D.FUNC_BLEND */
        case 64:
            outR = CLAMP8((int)((fgR * alphaNorm) + (bgR * (1 - alphaNorm))));
            outG = CLAMP8((int)((fgG * alphaNorm) + (bgG * (1 - alphaNorm))));
            outB = CLAMP8((int)((fgB * alphaNorm) + (bgB * (1 - alphaNorm))));
            outA = CLAMP8((int)(bgA * (1 - alphaNorm) + fgA * alphaNorm));
            return (outA << 24) | (outR << 16) | (outG << 8) | outB;

        /* CompositingMode.MODULATE / Texture2D.FUNC_MODULATE */
        case 68:
            outR = (fgR * bgR) / 255;
            outG = (fgG * bgG) / 255;
            outB = (fgB * bgB) / 255;
            outA = bgA > fgA ? bgA : fgA;
            return (CLAMP8(outA) << 24) | (CLAMP8(outR) << 16) | (CLAMP8(outG) << 8) | CLAMP8(outB);

        /* CompositingMode.MODULATE_X2 */
        case 69:
            outR = (2 * fgR * bgR) / 255;
            outG = (2 * fgG * bgG) / 255;
            outB = (2 * fgB * bgB) / 255;
            outA = bgA > fgA ? bgA : fgA;
            return (CLAMP8(outA) << 24) | (CLAMP8(outR) << 16) | (CLAMP8(outG) << 8) | CLAMP8(outB);

        /* Texture2D.FUNC_DECAL */
        case 70:
            outR = (fgR * fgA / 255) + (bgR * (255 - fgA) / 255);
            outG = (fgG * fgA / 255) + (bgG * (255 - fgA) / 255);
            outB = (fgB * fgA / 255) + (bgB * (255 - fgA) / 255);
            outA = fgA;
            return (CLAMP8(outA) << 24) | (CLAMP8(outR) << 16) | (CLAMP8(outG) << 8) | CLAMP8(outB);

        /* Texture2D.FUNC_ADD */
        case 71:
            outR = bgR + fgR < 255 ? bgR + fgR : 255;
            outG = bgG + fgG < 255 ? bgG + fgG : 255;
            outB = bgB + fgB < 255 ? bgB + fgB : 255;
            outA = bgA > fgA ? bgA : fgA;
            return (CLAMP8(outA) << 24) | (outR << 16) | (outG << 8) | outB;

        default:
            return bg;
    }
    #undef CLAMP8
}

/* Fog blending matching FreeJ2ME Graphics3D.blendFog */
static uint32_t m3g_blend_fog(uint32_t pixel_color, int fog_color, float fog_factor) {
    int r = (int)(((pixel_color >> 16) & 0xFF) * fog_factor + ((fog_color >> 16) & 0xFF) * (1 - fog_factor));
    int g = (int)(((pixel_color >> 8) & 0xFF) * fog_factor + ((fog_color >> 8) & 0xFF) * (1 - fog_factor));
    int b = (int)((pixel_color & 0xFF) * fog_factor + (fog_color & 0xFF) * (1 - fog_factor));
    /* Fog is always fully opaque */
    return (255 << 24) | (r << 16) | (g << 8) | b;
}

/* Appearance render state - holds per-draw compositing/fog params read from Java objects */
typedef struct {
    int blend_mode;         /* CompositingMode blending */
    int depth_test;         /* CompositingMode depth test enabled */
    int depth_write;        /* CompositingMode depth write enabled */
    float alpha_threshold;  /* CompositingMode alpha threshold 0.0-1.0 */
    int fog_enabled;
    int fog_mode;           /* 0=exponential, 1=linear */
    float fog_density;
    float fog_near;
    float fog_far;
    int fog_color;          /* ARGB */
    int texture_blend;      /* Texture2D blending mode */
} M3GRenderState;

/* Read render state from Appearance object (CompositingMode, Fog, etc.) */
static void m3g_read_render_state(M3GRenderState* state, JavaObject* appearance) {
    memset(state, 0, sizeof(M3GRenderState));
    state->depth_test = 1;   /* Default: depth test ON */
    state->depth_write = 1;  /* Default: depth write ON */
    state->alpha_threshold = 0.0f;
    state->blend_mode = 64;  /* Default: ALPHA */
    state->texture_blend = 70; /* Default: FUNC_DECAL */

    if (!appearance) return;

    /* Read CompositingMode */
    JavaObject* compositing = m3g_get_ref_field(appearance, "compositingMode");
    if (compositing) {
        state->blend_mode = m3g_get_int_field(compositing, "blending", 64);
        state->depth_test = m3g_get_int_field(compositing, "depthTest", 1);
        state->depth_write = m3g_get_int_field(compositing, "depthWrite", 1);
        state->alpha_threshold = m3g_get_float_field(compositing, "alphaThreshold", 0.0f);
    }

    /* Read Fog */
    JavaObject* fog = m3g_get_ref_field(appearance, "fog");
    if (fog) {
        state->fog_enabled = 1;
        state->fog_color = m3g_get_int_field(fog, "color", 0);
        state->fog_mode = m3g_get_int_field(fog, "mode", 0); /* 0=EXPONENTIAL, 1=LINEAR */
        state->fog_density = m3g_get_float_field(fog, "density", 1.0f);
        state->fog_near = m3g_get_float_field(fog, "nearDistance", 0.0f);
        state->fog_far = m3g_get_float_field(fog, "farDistance", 1.0f);
    }

    /* Read texture blend mode from Texture2D */
    JavaObject* texture = m3g_get_ref_field(appearance, "texture");
    if (texture) {
        state->texture_blend = m3g_get_int_field(texture, "blending", 70);
    }
}

/* Rasterize a single triangle - matches FreeJ2ME Graphics3D scanline rendering
 * Optimized for ARMv7: incremental edge functions, pre-computed row pointers,
 * cached inverse area, and local buffer stride to avoid repeated struct access. */
M3G_HOT static void m3g_rasterize_triangle(const float* v0, const float* v1, const float* v2,
                                    const float* t0, const float* t1, const float* t2,
                                    const uint8_t* c0, const uint8_t* c1, const uint8_t* c2,
                                    M3GAppearance* appearance) {
    /* Screen coordinates */
    float s0[4], s1[4], s2[4];
    if (m3g_clip_to_screen(s0, v0) < 0) return;
    if (m3g_clip_to_screen(s1, v1) < 0) return;
    if (m3g_clip_to_screen(s2, v2) < 0) return;
    
    /* Skip triangles outside viewport (depth-based) */
    if (s0[2] < 0 && s1[2] < 0 && s2[2] < 0) return;
    if (s0[2] > 1 && s1[2] > 1 && s2[2] > 1) return;
    
    /* Back-face culling
     * FIX 42: Respect winding order and culling mode from PolygonMode.
     * M3G default front face is CCW (winding=0). In screen space (Y-down),
     * CCW in 3D maps to positive edge function. So:
     *   - winding=0 (CCW): positive e = front face
     *   - winding=1 (CW):  negative e = front face
     * CULL_BACK (default): cull back faces (keep front)
     * CULL_FRONT: cull front faces (keep back)
     * twoSidedLighting: disables all culling */
    if (g_m3g.culling_enabled) {
        int is_two_sided = 0;
        if (appearance && appearance->two_sided_lighting) is_two_sided = 1;
        if (!is_two_sided) {
            float e = m3g_edge_function(s0, s1, s2);
            int winding = appearance ? appearance->winding : 0;
            int is_front;
            if (winding == 0) { /* CCW front face: positive e = front */
                is_front = (e >= 0);
            } else { /* CW front face: negative e = front */
                is_front = (e < 0);
            }
            int cull_front = appearance ? appearance->cull_front : 0;
            if (cull_front) {
                /* CULL_FRONT: discard front-facing triangles */
                if (is_front) return;
            } else {
                /* CULL_BACK: discard back-facing triangles */
                if (!is_front) return;
            }
        }
    }
    
    /* Bounding box */
    float fmin_x = s0[0] < s1[0] ? (s0[0] < s2[0] ? s0[0] : s2[0]) : (s1[0] < s2[0] ? s1[0] : s2[0]);
    float fmax_x = s0[0] > s1[0] ? (s0[0] > s2[0] ? s0[0] : s2[0]) : (s1[0] > s2[0] ? s1[0] : s2[0]);
    float fmin_y = s0[1] < s1[1] ? (s0[1] < s2[1] ? s0[1] : s2[1]) : (s1[1] < s2[1] ? s1[1] : s2[1]);
    float fmax_y = s0[1] > s1[1] ? (s0[1] > s2[1] ? s0[1] : s2[1]) : (s1[1] > s2[1] ? s1[1] : s2[1]);
    
    int min_x = (int)fmin_x;
    int max_x = (int)fmax_x;
    int min_y = (int)fmin_y;
    int max_y = (int)fmax_y;
    
    /* Clip to viewport - cache dimensions locally */
    int buf_w = g_m3g.buffer_width;
    int buf_h = g_m3g.buffer_height;
    
    /* Early exit for fully offscreen triangles */
    if (max_x < 0 || min_x >= buf_w || max_y < 0 || min_y >= buf_h) return;
    
    if (min_x < 0) min_x = 0;
    if (max_x >= buf_w) max_x = buf_w - 1;
    if (min_y < 0) min_y = 0;
    if (max_y >= buf_h) max_y = buf_h - 1;
    
    /* Area for barycentric - pre-compute inverse to avoid division per pixel */
    float area = m3g_edge_function(s0, s1, s2);
    if (fabsf(area) < 0.001f) return;
    float inv_area = 1.0f / area;
    
    /* Read render state from appearance for blending/depth/fog control
     * FIX 43: Use per-mesh settings from CompositingMode (read during mesh render)
     * instead of hardcoded defaults. Fall back to globals when appearance is NULL. */
    M3GRenderState rstate;
    memset(&rstate, 0, sizeof(rstate));
    rstate.depth_test = g_m3g.depth_test_enabled;
    rstate.depth_write = g_m3g.depth_write_enabled;
    rstate.alpha_threshold = 0.0f;
    rstate.blend_mode = 64;  /* ALPHA */
    rstate.texture_blend = 70; /* FUNC_DECAL */
    if (appearance) {
        if (appearance->blend_mode >= 0) rstate.blend_mode = appearance->blend_mode;
        if (appearance->alpha_threshold >= 0) rstate.alpha_threshold = (float)appearance->alpha_threshold / 255.0f;
        if (appearance->texture_blend > 0) rstate.texture_blend = appearance->texture_blend;
    }
    
    /* Cache buffer pointers locally for the inner loop - avoids repeated g_m3g dereference */
    uint32_t* color_buf = g_m3g.color_buffer;
    float* depth_buf = g_m3g.depth_buffer;
    int stride = buf_w;  /* pixels per row */
    
    /* Pre-compute edge function increments for scanline optimization.
     * The edge function E(a, b, p) = (p.x - a.x)*(b.y - a.y) - (p.y - a.y)*(b.x - a.x)
     * When moving from pixel (x,y) to (x+1,y), the increment is: (b.y - a.y)
     * When moving from row y to y+1, the decrement is: (b.x - a.x) */
    float row0_step_x = s2[1] - s1[1];  /* dw0/dx */
    float row0_step_y = -(s2[0] - s1[0]); /* dw0/dy */
    float row1_step_x = s0[1] - s2[1];  /* dw1/dx */
    float row1_step_y = -(s0[0] - s2[0]); /* dw1/dy */
    float row2_step_x = s1[1] - s0[1];  /* dw2/dx */
    float row2_step_y = -(s1[0] - s0[0]); /* dw2/dy */
    
    /* Pre-compute edge function at top-left corner (min_x + 0.5, min_y + 0.5) */
    float start_px = min_x + 0.5f;
    float start_py = min_y + 0.5f;
    float row_w0 = ((start_px - s1[0]) * (s2[1] - s1[1]) - (start_py - s1[1]) * (s2[0] - s1[0]));
    float row_w1 = ((start_px - s2[0]) * (s0[1] - s2[1]) - (start_py - s2[1]) * (s0[0] - s2[0]));
    float row_w2 = ((start_px - s0[0]) * (s1[1] - s0[1]) - (start_py - s0[1]) * (s1[0] - s0[0]));
    
    /* Has vertex colors flag - hoisted out of inner loop */
    int has_vertex_colors = (c0 && c1 && c2);
    int has_texture = (appearance && appearance->texture && t0 && t1 && t2);
    int has_fog = rstate.fog_enabled;
    int do_depth_test = rstate.depth_test;
    int do_depth_write = rstate.depth_write;
    int blend_mode = rstate.blend_mode;
    int texture_blend = rstate.texture_blend;
    float alpha_threshold = rstate.alpha_threshold;
    int fog_mode = rstate.fog_mode;
    float fog_density = rstate.fog_density;
    float fog_near = rstate.fog_near;
    float fog_far = rstate.fog_far;
    int fog_color = rstate.fog_color;
    
    /* Rasterize - incremental edge functions avoid redundant multiplications per pixel */
    int pixels_this_tri = 0;
    for (int y = min_y; y <= max_y; y++) {
        /* Reset w0/w1/w2 for this row using incremental step from previous row */
        float w0 = row_w0 * inv_area;
        float w1 = row_w1 * inv_area;
        float w2 = row_w2 * inv_area;
        
        int row_idx = y * stride;
        
        for (int x = min_x; x <= max_x; x++) {
            /* Inside triangle? (all barycentric >= 0) */
            if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
                /* Depth test */
                float z = s0[2] * w0 + s1[2] * w1 + s2[2] * w2;
                int idx = row_idx + x;
                
                if (do_depth_test && depth_buf[idx] < z) {
                    /* Existing pixel is closer, skip */
                } else {
                    /* Perspective correction */
                    float one_over_w = s0[3] * w0 + s1[3] * w1 + s2[3] * w2;
                    
                    /* Interpolate color */
                    uint8_t color[4] = {255, 255, 255, 255};
                    if (has_vertex_colors) {
                        m3g_interpolate_color(color, c0, c1, c2, w0, w1, w2);
                    }
                    
                    /* Determine the paint pixel */
                    uint32_t paint_pixel;
                    
                    /* Texture sampling */
                    if (has_texture) {
                        float uv[2];
                        m3g_interpolate_texcoord(uv, t0, t1, t2, w0, w1, w2, one_over_w);
                        uint32_t tex_color = m3g_sample_texture(appearance->texture, uv[0], uv[1]);
                        
                        paint_pixel = tex_color;
                        
                        /* Check alpha threshold */
                        int tex_alpha = (tex_color >> 24) & 0xFF;
                        if (tex_alpha < (int)(alpha_threshold * 255)) {
                            /* Skip transparent pixels below threshold */
                        } else if (has_vertex_colors) {
                            uint32_t vert_color = (color[3] << 24) | (color[0] << 16) | (color[1] << 8) | color[2];
                            paint_pixel = m3g_blend_pixels(vert_color, tex_color, tex_alpha, texture_blend);
                            
                            int paint_alpha = (paint_pixel >> 24) & 0xFF;
                            
                            if (has_fog) {
                                float fog_factor = 0.0f;
                                if (fog_mode == 1) { /* LINEAR */
                                    float range = fog_far - fog_near;
                                    if (range > 0.0001f) {
                                        fog_factor = (fog_far - z) / range * 250.0f;
                                        if (fog_factor < 0.0f) fog_factor = 0.0f;
                                        if (fog_factor > 1.0f) fog_factor = 1.0f;
                                    }
                                } else { /* EXPONENTIAL */
                                    fog_factor = fabsf(expf(-fog_density * z));
                                    if (fog_factor < 0.0f) fog_factor = 0.0f;
                                    if (fog_factor > 1.0f) fog_factor = 1.0f;
                                }
                                paint_pixel = m3g_blend_fog(paint_pixel, fog_color, fog_factor);
                            }
                            
                            uint32_t bg_pixel = color_buf[idx];
                            uint32_t final_pixel = m3g_blend_pixels(bg_pixel, paint_pixel, paint_alpha, blend_mode);
                            color_buf[idx] = final_pixel;
                            if (do_depth_write) depth_buf[idx] = z;
                            pixels_this_tri++;
                        } else {
                            int paint_alpha = (paint_pixel >> 24) & 0xFF;
                            if (has_fog) {
                                float fog_factor = 0.0f;
                                if (fog_mode == 1) {
                                    float range = fog_far - fog_near;
                                    if (range > 0.0001f) {
                                        fog_factor = (fog_far - z) / range * 250.0f;
                                        if (fog_factor < 0.0f) fog_factor = 0.0f;
                                        if (fog_factor > 1.0f) fog_factor = 1.0f;
                                    }
                                } else {
                                    fog_factor = fabsf(expf(-fog_density * z));
                                    if (fog_factor < 0.0f) fog_factor = 0.0f;
                                    if (fog_factor > 1.0f) fog_factor = 1.0f;
                                }
                                paint_pixel = m3g_blend_fog(paint_pixel, fog_color, fog_factor);
                            }
                            uint32_t bg_pixel = color_buf[idx];
                            uint32_t final_pixel = m3g_blend_pixels(bg_pixel, paint_pixel, paint_alpha, blend_mode);
                            color_buf[idx] = final_pixel;
                            if (do_depth_write) depth_buf[idx] = z;
                            pixels_this_tri++;
                        }
                    } else {
                        /* No texture - use vertex color directly */
                        paint_pixel = (color[3] << 24) | (color[0] << 16) | (color[1] << 8) | color[2];
                        int paint_alpha = (paint_pixel >> 24) & 0xFF;
                        
                        if (has_fog) {
                            float fog_factor = 0.0f;
                            if (fog_mode == 1) {
                                float range = fog_far - fog_near;
                                if (range > 0.0001f) {
                                    fog_factor = (fog_far - z) / range * 250.0f;
                                    if (fog_factor < 0.0f) fog_factor = 0.0f;
                                    if (fog_factor > 1.0f) fog_factor = 1.0f;
                                }
                            } else {
                                fog_factor = fabsf(expf(-fog_density * z));
                                if (fog_factor < 0.0f) fog_factor = 0.0f;
                                if (fog_factor > 1.0f) fog_factor = 1.0f;
                            }
                            paint_pixel = m3g_blend_fog(paint_pixel, fog_color, fog_factor);
                        }
                        
                        uint32_t bg_pixel = color_buf[idx];
                        uint32_t final_pixel = m3g_blend_pixels(bg_pixel, paint_pixel, paint_alpha, blend_mode);
                        color_buf[idx] = final_pixel;
                        if (do_depth_write) depth_buf[idx] = z;
                        pixels_this_tri++;
                    }
                }
            }
            
            /* Incremental edge function step for x+1 */
            w0 += row0_step_x * inv_area;
            w1 += row1_step_x * inv_area;
            w2 += row2_step_x * inv_area;
        }
        
        /* Incremental edge function step for next scanline (y+1) */
        row_w0 += row0_step_y;
        row_w1 += row1_step_y;
        row_w2 += row2_step_y;
    }
    
    g_m3g.triangles_rendered++;
    g_m3g_raster_pixels_written += pixels_this_tri;
    
    /* Diagnostic: log first 3 triangles with screen coords */
    static int diag_count = 0;
    if (diag_count < 3) {
        diag_count++;
        fprintf(stderr, "[M3G-RAST] tri#%d: s0=(%.1f,%.1f,z=%.3f) s1=(%.1f,%.1f,z=%.3f) s2=(%.1f,%.1f,z=%.3f) bbox=(%d,%d)-(%d,%d) c0=[%d,%d,%d,%d] pixels=%d\n",
                diag_count, s0[0], s0[1], s0[2], s1[0], s1[1], s1[2], s2[0], s2[1], s2[2],
                min_x, min_y, max_x, max_y, c0[0], c0[1], c0[2], c0[3], pixels_this_tri);
    }
}

/* ============================================================================
 * Java Object <-> Native Object Mapping
 * ============================================================================ */

/* Alternating temp buffers for get_native_transform - handles the common
 * case of needing two transforms simultaneously (e.g. postMultiply). */
static M3GTransform g_transform_temp_a, g_transform_temp_b;
static int g_transform_temp_idx = 0;

/* Copy native transform data from Java Transform object into caller-provided storage */
static void get_native_transform_into(JavaObject* obj, M3GTransform* out) {
    if (!obj || !out) return;
    
    memset(out, 0, sizeof(M3GTransform));
    
    /* Ensure object has fields */
    if (!OBJECT_HAS_FIELDS(obj, 1)) return;
    
    /* Transform stores 16 floats in a float[] field */
    JavaArray* arr = (JavaArray*)obj->fields[0].ref;
    if (!arr) return;
    if (arr->element_type != T_FLOAT || arr->length < 16) return;
    
    float* data = (float*)array_data(arr);
    memcpy(out->m, data, 16 * sizeof(float));
}

/* Get native transform from Java Transform object.
 * Returns a pointer to an internal temp buffer that alternates between two slots.
 * Only safe for up to 2 simultaneous transforms; callers needing more should
 * use get_native_transform_into() directly. */
static M3GTransform* get_native_transform(JavaObject* obj) {
    if (!obj) return NULL;
    
    M3GTransform* out = (g_transform_temp_idx++ & 1) ? &g_transform_temp_b : &g_transform_temp_a;
    g_transform_temp_idx &= 1;  /* keep it cycling 0,1,0,1,... */
    get_native_transform_into(obj, out);
    return out;
}

/* Set native transform to Java Transform object */
static void set_native_transform(JavaObject* obj, M3GTransform* t) {
    if (!obj || !t) return;
    
    /* Ensure object has fields */
    if (!OBJECT_HAS_FIELDS(obj, 1)) return;
    
    JavaArray* arr = (JavaArray*)obj->fields[0].ref;
    
    /* If no array exists, we cannot set - caller should ensure array exists */
    if (!arr) return;
    
    if (arr->element_type != T_FLOAT || arr->length < 16) return;
    
    float* data = (float*)array_data(arr);
    memcpy(data, t->m, 16 * sizeof(float));
}

/* Get VertexArray data */
static M3GVertexArray* get_vertex_array(JavaObject* obj, M3GVertexArray* temp) {
    if (!obj || !temp) return NULL;
    
    memset(temp, 0, sizeof(M3GVertexArray));
    
    /* VertexArray fields: data (byte[]/short[]), componentCount, componentSize, count */
    JavaArray* data = (JavaArray*)m3g_get_ref_field(obj, "data");
    int component_count = m3g_get_int_field(obj, "componentCount", 3);
    int component_size = m3g_get_int_field(obj, "componentSize", 1);
    int vertex_count = m3g_get_int_field(obj, "vertexCount", 0);
    
    if (!data || vertex_count <= 0) return NULL;
    
    /* Sanity checks to prevent OOM */
    if (vertex_count > M3G_MAX_VERTEX_COUNT) {
        GFX_DEBUG("get_vertex_array: vertex_count %d exceeds max %d", vertex_count, M3G_MAX_VERTEX_COUNT);
        return NULL;
    }
    if (component_count > 4 || component_count < 2) {
        GFX_DEBUG("get_vertex_array: invalid component_count %d", component_count);
        return NULL;
    }
    
    /* Convert to float positions */
    temp->positions = (float*)malloc(vertex_count * 3 * sizeof(float));
    if (!temp->positions) return NULL;
    
    temp->vertex_count = vertex_count;
    temp->vertex_stride = component_count;
    
    if (component_size == 1) {
        int8_t* src = (int8_t*)array_data(data);
        for (int i = 0; i < vertex_count * component_count; i++) {
            temp->positions[i] = (float)src[i];
        }
    } else {
        int16_t* src = (int16_t*)array_data(data);
        for (int i = 0; i < vertex_count * component_count; i++) {
            temp->positions[i] = (float)src[i];
        }
    }
    
    return temp;
}

/* ============================================================================
 * Native Method Implementations
 * ============================================================================ */

/* Graphics3D.getInstance() */
static JavaValue native_graphics3d_getInstance(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)thread; (void)args; (void)arg_count;
    
    /* Find Graphics3D class */
    JavaClass* g3d_class = jvm_load_class(jvm, "javax/microedition/m3g/Graphics3D");
    if (!g3d_class) {
        GFX_DEBUG("Graphics3D class not found");
        return NATIVE_RETURN_NULL();
    }
    
    /* Check for static instance field */
    for (int i = 0; i < g3d_class->static_fields_count; i++) {
        if (strcmp(g3d_class->static_fields[i].name, "instance") == 0) {
            if (g3d_class->static_fields[i].value.ref) {
                return NATIVE_RETURN_OBJECT(g3d_class->static_fields[i].value.ref);
            }
            break;
        }
    }
    
    /* Create new instance */
    JavaObject* instance = jvm_new_object(jvm, g3d_class);
    if (!instance) {
        GFX_DEBUG("Failed to create Graphics3D instance");
        return NATIVE_RETURN_NULL();
    }
    
    /* Store as static field */
    for (int i = 0; i < g3d_class->static_fields_count; i++) {
        if (strcmp(g3d_class->static_fields[i].name, "instance") == 0) {
            g3d_class->static_fields[i].value.ref = instance;
            break;
        }
    }
    
    fprintf(stderr, "[M3G] Graphics3D.getInstance() -> %p\n", (void*)instance);
    return NATIVE_RETURN_OBJECT(instance);
}

/* Graphics3D.bindTarget(Image2D) - 1-arg form */
/* Graphics3D.bindTarget(Object target, boolean depthBuffering, int hints) - 3-arg form used by games */
static JavaValue native_graphics3d_bindTarget_3arg(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* g3d = (JavaObject*)args[0].ref;
    JavaObject* target = (JavaObject*)args[1].ref;
    /* args[2] = depthBuffering hint (boolean), args[3] = hints */
    (void)g3d;
    
    g_m3g_bindtarget_called = true;  /* Track that bindTarget was called */
    g_m3g_pending_render_count = 0; /* Clear pending renders - game took over */
    m3g_thread_fence();
    
    g_m3g.target_image = NULL;
    g_m3g.target_gfx = NULL;
    g_m3g.target_is_graphics = 0;
    
    fprintf(stderr, "[M3G] bindTarget_3arg: target=%p (class=%s)\n", (void*)target,
            target && target->header.clazz && target->header.clazz->class_name ? target->header.clazz->class_name : "?");
    
    if (!target) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Determine if target is Image2D or Graphics by checking class name */
    JavaClass* target_class = target->header.clazz;
    const char* target_class_name = target_class ? target_class->class_name : NULL;
    int is_image2d = (target_class_name && strstr(target_class_name, "Image2D") != NULL);
    int is_graphics = (target_class_name && strstr(target_class_name, "Graphics") != NULL);
    
    int width = 0, height = 0;
    
    if (is_image2d) {
        /* Image2D target - read dimensions from its fields */
        width = m3g_get_int_field(target, "width", 0);
        height = m3g_get_int_field(target, "height", 0);
        g_m3g.target_is_graphics = 0;
        g_m3g.target_image = target;
    } else if (is_graphics) {
        /* Graphics target - get dimensions from the native MidpGraphics context */
        g_m3g.target_is_graphics = 1;
        g_m3g.target_image = target;
        
        extern MidpGraphics* get_graphics_from_object(JavaObject* obj);
        MidpGraphics* gfx = get_graphics_from_object(target);
        if (gfx && gfx->pixels) {
            g_m3g.target_gfx = gfx;
            width = gfx->width;
            height = gfx->height;
            fprintf(stderr, "[M3G] bindTarget(3arg): Graphics target, MidpGraphics=%p, %dx%d\n",
                    (void*)gfx, width, height);
        } else {
            /* Fallback: try to get from the screen context */
            SdlContext* sdl = sdl_get_global_context();
            if (sdl && sdl->width > 0 && sdl->height > 0) {
                width = sdl->width;
                height = sdl->height;
            }
            fprintf(stderr, "[M3G] bindTarget(3arg): Graphics target, no MidpGraphics, using screen %dx%d\n",
                    width, height);
        }
    } else {
        /* Unknown target type - try as Image2D */
        width = m3g_get_int_field(target, "width", 0);
        height = m3g_get_int_field(target, "height", 0);
        g_m3g.target_is_graphics = 0;
        g_m3g.target_image = target;
    }
    
    /* Fallback dimensions */
    if (width <= 0 || height <= 0) {
        width = m3g_get_int_field(target, "width", 0);
        height = m3g_get_int_field(target, "height", 0);
    }
    if (width <= 0 || height <= 0) {
        width = m3g_get_int_field(target, "viewportWidth", 240);
        height = m3g_get_int_field(target, "viewportHeight", 320);
    }
    if (width <= 0) width = 240;
    if (height <= 0) height = 320;
    if (width > M3G_MAX_IMAGE_DIMENSION) width = M3G_MAX_IMAGE_DIMENSION;
    if (height > M3G_MAX_IMAGE_DIMENSION) height = M3G_MAX_IMAGE_DIMENSION;
    
    int pixel_count = width * height;
    if (pixel_count <= 0 || pixel_count > M3G_MAX_IMAGE_PIXELS) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Save target info before context_init (which zeros g_m3g) */
    JavaObject* saved_target = g_m3g.target_image;
    MidpGraphics* saved_gfx = g_m3g.target_gfx;
    int saved_is_graphics = g_m3g.target_is_graphics;
    
    m3g_context_init(width, height);
    
    /* Restore target info after context_init */
    g_m3g.target_image = saved_target;
    g_m3g.target_gfx = saved_gfx;
    g_m3g.target_is_graphics = saved_is_graphics;
    
    fprintf(stderr, "[M3G] bindTarget(3arg): %dx%d, target=%p (%s), buffers=%d\n", 
            width, height, (void*)g_m3g.target_image,
            g_m3g.target_is_graphics ? "Graphics" : "Image2D",
            g_m3g.buffers_allocated);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_graphics3d_bindTarget(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* g3d = (JavaObject*)args[0].ref;
    JavaObject* target = (JavaObject*)args[1].ref;
    
    (void)g3d;  /* Instance not used in this simple impl */
    
    g_m3g_bindtarget_called = true;  /* Track that bindTarget was called */
    g_m3g_pending_render_count = 0; /* Clear pending renders - game took over */
    m3g_thread_fence();
    
    if (!target) {
        GFX_DEBUG("bindTarget: null target");
        g_m3g.target_image = NULL;
        return NATIVE_RETURN_VOID();
    }
    
    /* Get image dimensions from target */
    int width = m3g_get_int_field(target, "width", 240);
    int height = m3g_get_int_field(target, "height", 320);
    
    /* Sanity check: prevent OOM from unreasonable dimensions */
    if (width <= 0 || height <= 0) {
        GFX_DEBUG("bindTarget: Invalid dimensions %dx%d, using defaults", width, height);
        width = 240;
        height = 320;
    }
    if (width > M3G_MAX_IMAGE_DIMENSION || height > M3G_MAX_IMAGE_DIMENSION) {
        GFX_DEBUG("bindTarget: Dimensions too large %dx%d, clamping to %d", 
                width, height, M3G_MAX_IMAGE_DIMENSION);
        if (width > M3G_MAX_IMAGE_DIMENSION) width = M3G_MAX_IMAGE_DIMENSION;
        if (height > M3G_MAX_IMAGE_DIMENSION) height = M3G_MAX_IMAGE_DIMENSION;
    }
    
    /* Check for integer overflow */
    int pixel_count = width * height;
    if (pixel_count <= 0 || pixel_count > M3G_MAX_IMAGE_PIXELS) {
        GFX_DEBUG("bindTarget: Pixel count overflow or invalid: %d", pixel_count);
        return NATIVE_RETURN_VOID();
    }
    
    /* Initialize or reinitialize context (m3g_context_init internally calls
     * m3g_context_free_buffers to release old buffers before allocating new ones) */
    m3g_context_init(width, height);
    
    /* Save target image for releaseTarget */
    g_m3g.target_image = target;
    
    fprintf(stderr, "[M3G] bindTarget: %dx%d, image=%p, buffers=%d\n", width, height, (void*)target, g_m3g.buffers_allocated);
    return NATIVE_RETURN_VOID();
}

/* Graphics3D.releaseTarget() */
static JavaValue native_graphics3d_releaseTarget(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)args; (void)arg_count;
    
    fprintf(stderr, "[M3G] releaseTarget: triangles=%d, target=%p, is_graphics=%d\n", 
            g_m3g.triangles_rendered, (void*)g_m3g.target_image, g_m3g.target_is_graphics);
    
    if (!g_m3g.target_image || !g_m3g.color_buffer) {
        g_m3g.target_image = NULL;
        g_m3g.target_gfx = NULL;
        g_m3g.target_is_graphics = 0;
        return NATIVE_RETURN_VOID();
    }
    
    if (g_m3g.target_is_graphics && g_m3g.target_gfx) {
        /* Target is a Graphics object - blit rendered pixels to the Graphics pixel buffer.
         * This is the common case for games: g3d.bindTarget(getGraphics(), true, 0); */
        MidpGraphics* gfx = g_m3g.target_gfx;
        int copy_w = g_m3g.buffer_width < gfx->width ? g_m3g.buffer_width : gfx->width;
        int copy_h = g_m3g.buffer_height < gfx->height ? g_m3g.buffer_height : gfx->height;
        
        for (int y = 0; y < copy_h; y++) {
            for (int x = 0; x < copy_w; x++) {
                int src_idx = y * g_m3g.buffer_width + x;
                int dst_idx = y * gfx->width + x;
                gfx->pixels[dst_idx] = g_m3g.color_buffer[src_idx];
            }
        }
        fprintf(stderr, "[M3G] releaseTarget: copied %dx%d pixels to Graphics buffer\n", copy_w, copy_h);
        
        /* Frame capture from releaseTarget path (bindTarget→render→releaseTarget) */
        {
            static int rt_capture_count = 0;
            static int rt_capture_limit = -1;
            rt_capture_count++;
            
            if (rt_capture_limit < 0) {
                const char* env_cap = getenv("M3G_CAPTURE");
                if (env_cap) {
                    if (strcmp(env_cap, "every") == 0) rt_capture_limit = 999999;
                    else rt_capture_limit = atoi(env_cap);
                } else {
                    rt_capture_limit = 30;
                }
            }
            
            int should_capture = 0;
            if (rt_capture_limit == 999999) {
                should_capture = (rt_capture_count % 100 == 0);
            } else if (rt_capture_limit > 0) {
                should_capture = (rt_capture_count <= rt_capture_limit);
            }
            
            if (should_capture && gfx->pixels) {
                char fname[256];
                snprintf(fname, sizeof(fname), "/home/z/my-project/download/m3g_rt_%05d.bmp", rt_capture_count);
                int w = gfx->width;
                int h = gfx->height;
                int row_size = (w * 3 + 3) & ~3;
                int pixel_data_size = row_size * h;
                int file_size = 54 + pixel_data_size;
                
                FILE* f = fopen(fname, "wb");
                if (f) {
                    uint8_t hdr[14] = {'B','M'};
                    hdr[2] = file_size & 0xFF; hdr[3] = (file_size>>8) & 0xFF;
                    hdr[4] = (file_size>>16) & 0xFF; hdr[5] = (file_size>>24) & 0xFF;
                    hdr[10] = 54;
                    fwrite(hdr, 1, 14, f);
                    
                    uint8_t dib[40] = {0};
                    dib[0] = 40;
                    dib[4] = w & 0xFF; dib[5] = (w>>8) & 0xFF; dib[6] = (w>>16) & 0xFF; dib[7] = (w>>24) & 0xFF;
                    dib[8] = h & 0xFF; dib[9] = (h>>8) & 0xFF; dib[10] = (h>>16) & 0xFF; dib[11] = (h>>24) & 0xFF;
                    dib[12] = 1;
                    dib[14] = 24;
                    dib[20] = pixel_data_size & 0xFF; dib[21] = (pixel_data_size>>8) & 0xFF;
                    dib[22] = (pixel_data_size>>16) & 0xFF; dib[23] = (pixel_data_size>>24) & 0xFF;
                    fwrite(dib, 1, 40, f);
                    
                    uint8_t* row_buf = (uint8_t*)calloc(row_size, 1);
                    if (row_buf) {
                        for (int cy = h - 1; cy >= 0; cy--) {
                            memset(row_buf, 0, row_size);
                            for (int cx = 0; cx < w; cx++) {
                                uint32_t p = gfx->pixels[cy * w + cx];
                                row_buf[cx * 3 + 0] = p & 0xFF;
                                row_buf[cx * 3 + 1] = (p >> 8) & 0xFF;
                                row_buf[cx * 3 + 2] = (p >> 16) & 0xFF;
                            }
                            fwrite(row_buf, 1, row_size, f);
                        }
                        free(row_buf);
                    }
                    fclose(f);
                    fprintf(stderr, "[M3G-CAP] releaseTarget frame #%d saved to %s (%dx%d, tris=%d)\n", 
                            rt_capture_count, fname, w, h, g_m3g.triangles_rendered);
                }
            }
        }
    } else {
        /* Target is an Image2D object - copy pixels to its pixel array */
        JavaObject* image2d = g_m3g.target_image;
        JavaArray* pixels = (JavaArray*)m3g_get_ref_field(image2d, "pixels");
        
        if (pixels && pixels->element_type == T_INT) {
            int width = m3g_get_int_field(image2d, "width", g_m3g.buffer_width);
            int height = m3g_get_int_field(image2d, "height", g_m3g.buffer_height);
            int pixel_count = width * height;
            
            jint* dst = (jint*)array_data(pixels);
            if (dst && pixel_count > 0) {
                int copy_count = pixel_count < (g_m3g.buffer_width * g_m3g.buffer_height) 
                               ? pixel_count 
                               : g_m3g.buffer_width * g_m3g.buffer_height;
                
                for (int i = 0; i < copy_count; i++) {
                    dst[i] = (jint)g_m3g.color_buffer[i];
                }
                fprintf(stderr, "[M3G] releaseTarget: copied %d pixels to Image2D\n", copy_count);
            }
        }
    }
    
    g_m3g.target_image = NULL;
    g_m3g.target_gfx = NULL;
    g_m3g.target_is_graphics = 0;
    
    return NATIVE_RETURN_VOID();
}

/* Graphics3D.clear(Background) */
static JavaValue native_graphics3d_clear(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* background = (JavaObject*)args[1].ref;
    
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
    float depth = 1.0f;
    
    if (background) {
        /* Background has clearColor field (int ARGB) */
        int argb = m3g_get_int_field(background, "clearColor", 0);
        a = ((argb >> 24) & 0xFF) / 255.0f;
        r = ((argb >> 16) & 0xFF) / 255.0f;
        g = ((argb >> 8) & 0xFF) / 255.0f;
        b = (argb & 0xFF) / 255.0f;
    }
    
    m3g_clear(r, g, b, a, depth);
    
    return NATIVE_RETURN_VOID();
}

/* Graphics3D.setViewport(int x, int y, int width, int height) */
static JavaValue native_graphics3d_setViewport(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* g3d = (JavaObject*)args[0].ref;
    (void)g3d;
    
    g_m3g.viewport_x = args[1].i;
    g_m3g.viewport_y = args[2].i;
    g_m3g.viewport_width = args[3].i;
    g_m3g.viewport_height = args[4].i;
    
    GFX_DEBUG("setViewport: %d,%d %dx%d", 
            g_m3g.viewport_x, g_m3g.viewport_y, 
            g_m3g.viewport_width, g_m3g.viewport_height);
    
    return NATIVE_RETURN_VOID();
}

/* Transform.setIdentity() */
/* Transform.<init>() - default constructor, creates identity matrix */
static JavaValue native_transform_init(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    GFX_DEBUG("Transform.<init>() called, obj=%p", (void*)obj);
    
    if (!obj) {
        GFX_DEBUG("Transform.<init>: NULL object!");
        return NATIVE_RETURN_VOID();
    }
    
    /* Verify the object has fields */
    JavaClass* clazz = obj->header.clazz;
    if (!clazz) {
        GFX_DEBUG("Transform.<init>: NULL class!");
        return NATIVE_RETURN_VOID();
    }
    
    int max_slots = (clazz->instance_size - (int)sizeof(ObjectHeader)) / (int)sizeof(JavaValue);
    GFX_DEBUG("Transform.<init>: clazz=%s, instance_size=%d, max_slots=%d, fields_count=%d",
              clazz->class_name ? clazz->class_name : "?", 
              clazz->instance_size, max_slots, clazz->fields_count);
    
    if (max_slots < 1) {
        GFX_DEBUG("Transform.<init>: ERROR - no space for matrix field! instance_size=%d", clazz->instance_size);
        return NATIVE_RETURN_VOID();
    }
    
    /* Create float[16] array for the matrix */
    JavaArray* arr = jvm_new_array(jvm, T_FLOAT, 16, NULL);
    if (!arr) {
        GFX_DEBUG("Transform.<init>: failed to create float array");
        return NATIVE_RETURN_VOID();
    }
    
    /* Initialize to identity matrix */
    float* data = (float*)array_data(arr);
    memset(data, 0, 16 * sizeof(float));
    data[0] = 1.0f;   /* m[0][0] */
    data[5] = 1.0f;   /* m[1][1] */
    data[10] = 1.0f;  /* m[2][2] */
    data[15] = 1.0f;  /* m[3][3] */
    
    /* Store in Transform's first field */
    obj->fields[0].ref = arr;
    
    GFX_DEBUG("Transform.<init>(): created identity transform, arr=%p", (void*)arr);
    return NATIVE_RETURN_VOID();
}

/* Transform.<init>(Transform) - copy constructor */
static JavaValue native_transform_init_copy(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* other = (JavaObject*)args[1].ref;
    
    if (!obj) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Create float[16] array for the matrix */
    JavaArray* arr = jvm_new_array(jvm, T_FLOAT, 16, NULL);
    if (!arr) {
        GFX_DEBUG("Transform.<init>(Transform): failed to create float array");
        return NATIVE_RETURN_VOID();
    }
    
    float* data = (float*)array_data(arr);
    
    if (other && OBJECT_HAS_FIELDS(other, 1)) {
        /* Copy from other transform */
        JavaArray* other_arr = (JavaArray*)other->fields[0].ref;
        if (other_arr && other_arr->element_type == T_FLOAT && other_arr->length >= 16) {
            float* other_data = (float*)array_data(other_arr);
            memcpy(data, other_data, 16 * sizeof(float));
            GFX_DEBUG("Transform.<init>(Transform): copied transform");
        } else {
            /* Other transform has invalid data, use identity */
            memset(data, 0, 16 * sizeof(float));
            data[0] = 1.0f;
            data[5] = 1.0f;
            data[10] = 1.0f;
            data[15] = 1.0f;
            GFX_DEBUG("Transform.<init>(Transform): other has invalid data, using identity");
        }
    } else {
        /* Other is null, use identity */
        memset(data, 0, 16 * sizeof(float));
        data[0] = 1.0f;
        data[5] = 1.0f;
        data[10] = 1.0f;
        data[15] = 1.0f;
        GFX_DEBUG("Transform.<init>(Transform): other is null, using identity");
    }
    
    /* Store in Transform's first field */
    obj->fields[0].ref = arr;
    
    return NATIVE_RETURN_VOID();
}

static JavaValue native_transform_setIdentity(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    M3GTransform t;
    m3g_transform_identity(&t);
    set_native_transform(obj, &t);
    
    return NATIVE_RETURN_VOID();
}

/* Transform.postMultiply(Transform) */
static JavaValue native_transform_postMultiply(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* this_obj = (JavaObject*)args[0].ref;
    JavaObject* other_obj = (JavaObject*)args[1].ref;
    
    M3GTransform this_t, other_t;
    get_native_transform_into(this_obj, &this_t);
    get_native_transform_into(other_obj, &other_t);
    
    /* Check if either transform is all-zero (indicating invalid/null) */
    int this_valid = 0, other_valid = 0;
    for (int i = 0; i < 16; i++) {
        if (this_t.m[i] != 0.0f) this_valid = 1;
        if (other_t.m[i] != 0.0f) other_valid = 1;
    }
    if (!this_valid || !other_valid) {
        return NATIVE_RETURN_VOID();
    }
    
    M3GTransform result;
    m3g_transform_multiply(&result, &this_t, &other_t);
    set_native_transform(this_obj, &result);
    
    return NATIVE_RETURN_VOID();
}

/* Transform.postTranslate(float x, float y, float z) */
static JavaValue native_transform_postTranslate(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    float x = args[1].f;
    float y = args[2].f;
    float z = args[3].f;
    
    M3GTransform* t = get_native_transform(obj);
    if (t) {
        M3GTransform temp;
        memcpy(temp.m, t->m, sizeof(temp.m));
        m3g_transform_translate(&temp, x, y, z);
        set_native_transform(obj, &temp);
    }
    
    return NATIVE_RETURN_VOID();
}

/* Transform.postRotate(float angle, float ax, float ay, float az) */
static JavaValue native_transform_postRotate(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    float angle = args[1].f * 3.14159f / 180.0f;  /* Degrees to radians */
    float ax = args[2].f;
    float ay = args[3].f;
    float az = args[4].f;
    
    M3GTransform* t = get_native_transform(obj);
    if (t) {
        M3GTransform temp;
        memcpy(temp.m, t->m, sizeof(temp.m));
        m3g_transform_rotate(&temp, angle, ax, ay, az);
        set_native_transform(obj, &temp);
    }
    
    return NATIVE_RETURN_VOID();
}

/* Transform.postScale(float sx, float sy, float sz) */
static JavaValue native_transform_postScale(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    float sx = args[1].f;
    float sy = args[2].f;
    float sz = args[3].f;
    
    M3GTransform* t = get_native_transform(obj);
    if (t) {
        M3GTransform temp;
        memcpy(temp.m, t->m, sizeof(temp.m));
        m3g_transform_scale(&temp, sx, sy, sz);
        set_native_transform(obj, &temp);
    }
    
    return NATIVE_RETURN_VOID();
}

/* VertexArray.<init>(int numVertices, int numComponents, int componentSize) */
static JavaValue native_vertexarray_init(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint num_vertices = args[1].i;
    jint num_components = args[2].i;
    jint component_size = args[3].i;
    
    if (!obj || num_vertices <= 0 || num_components < 2 || num_components > 4) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Register in M3G registry */
    m3g_registry_register(obj, 0);
    
    /* Sanity check: limit vertex count to prevent OOM */
    if (num_vertices > M3G_MAX_VERTEX_COUNT) {
        GFX_DEBUG("VertexArray: num_vertices %d exceeds max %d", num_vertices, M3G_MAX_VERTEX_COUNT);
        return NATIVE_RETURN_VOID();
    }
    
    /* Check for integer overflow in data_size calculation */
    int data_size = num_vertices * num_components * component_size;
    if (data_size <= 0 || data_size > M3G_MAX_VERTEX_COUNT * 4 * 2) {
        GFX_DEBUG("VertexArray: data_size overflow or invalid: %d", data_size);
        return NATIVE_RETURN_VOID();
    }
    
    /* Create data array */
    JavaArray* data = jvm_new_array(jvm, component_size == 1 ? T_BYTE : T_SHORT, data_size, NULL);
    if (!data) {
        GFX_DEBUG("VertexArray: failed to allocate data array");
        return NATIVE_RETURN_VOID();
    }
    
    /* Store fields using helper functions */
    m3g_set_ref_field(obj, "data", (JavaObject*)data);
    m3g_set_int_field(obj, "componentCount", num_components);
    m3g_set_int_field(obj, "componentSize", component_size);
    m3g_set_int_field(obj, "vertexCount", num_vertices);
    
    GFX_DEBUG("VertexArray.init: %d vertices, %d components, %d bytes",
            num_vertices, num_components, component_size);
    
    return NATIVE_RETURN_VOID();
}

/* Camera.setPerspective(float fov, float aspectRatio, float near, float far) */
static JavaValue native_camera_setPerspective(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    float fov = args[1].f;
    float aspect = args[2].f;
    float near = args[3].f;
    float far = args[4].f;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* Store parameters on the camera object for force-render path */
    m3g_set_float_field(obj, "fov", fov);
    m3g_set_float_field(obj, "aspect", aspect);
    m3g_set_float_field(obj, "near", near);
    m3g_set_float_field(obj, "far", far);
    
    /* Update projection matrix */
    m3g_transform_perspective(&g_m3g.projection, fov, aspect, near, far);
    
    GFX_DEBUG("Camera.setPerspective: fov=%.1f, aspect=%.2f, near=%.2f, far=%.2f",
            fov, aspect, near, far);
    
    return NATIVE_RETURN_VOID();
}

/* Camera.setParallel(float height, float aspectRatio, float near, float far) */
static JavaValue native_camera_setParallel(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    float height = args[1].f;
    float aspect = args[2].f;
    float near = args[3].f;
    float far = args[4].f;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    float half_h = height / 2.0f;
    float half_w = half_h * aspect;
    
    m3g_transform_ortho(&g_m3g.projection, -half_w, half_w, -half_h, half_h, near, far);
    
    GFX_DEBUG("Camera.setParallel: height=%.1f, aspect=%.2f", height, aspect);
    
    return NATIVE_RETURN_VOID();
}

/* Find light index for a given Light Java object, or -1 if not found */
static int find_light_for_object(JavaObject* light_obj) {
    for (int i = 0; i < g_m3g.light_count; i++) {
        if (g_m3g.lights[i].obj == light_obj) return i;
    }
    return -1;
}

/* Light.setType(int type) */
static JavaValue native_light_setType(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint type = args[1].i;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* Check if this object already has a light slot */
    int light_idx = find_light_for_object(obj);
    
    if (light_idx < 0) {
        /* Add new light slot */
        if (g_m3g.light_count < 8) {
            light_idx = g_m3g.light_count++;
            memset(&g_m3g.lights[light_idx], 0, sizeof(M3GLight));
            g_m3g.lights[light_idx].obj = obj;
            g_m3g.lights[light_idx].attenuation[0] = 1.0f;  /* constant */
            g_m3g.lights[light_idx].color[3] = 1.0f;       /* default alpha */
        } else {
            GFX_DEBUG("Light.setType: max lights reached (8)");
            return NATIVE_RETURN_VOID();
        }
    }
    
    g_m3g.lights[light_idx].type = type;
    
    GFX_DEBUG("Light.setType: %d (idx=%d)", type, light_idx);
    
    return NATIVE_RETURN_VOID();
}

/* World.addCamera(Camera) */
static JavaValue native_world_addCamera(JVM* jvm, JavaThread* thread,
                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* world = (JavaObject*)args[0].ref;
    JavaObject* camera = (JavaObject*)args[1].ref;
    (void)world; (void)camera;
    
    GFX_DEBUG("World.addCamera");
    return NATIVE_RETURN_VOID();
}

/* World.setBackground(Background) */
static JavaValue native_world_setBackground(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* world = (JavaObject*)args[0].ref;
    JavaObject* background = (JavaObject*)args[1].ref;
    
    if (world && background) {
        m3g_set_ref_field(world, "background", background);
    }
    
    GFX_DEBUG("World.setBackground");
    return NATIVE_RETURN_VOID();
}

/* World.render(World) - Render a 3D scene */
static JavaValue native_graphics3d_renderWorld(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* g3d = (JavaObject*)args[0].ref;
    JavaObject* world = (JavaObject*)args[1].ref;
    (void)g3d;
    
    fprintf(stderr, "[M3G] renderWorld: world=%p\n", (void*)world);
    
    g_m3g_render_done = true;  /* Track that render was called this frame */
    m3g_thread_fence();
    
    /* NOTE: Do NOT reset triangles_rendered here - the game may have already
     * rendered triangles via render(Node,Transform) before calling render(World).
     * Resetting would lose the count and m3g_clear() would overwrite them. */
    
    if (!world) {
        return NATIVE_RETURN_VOID();
    }
    
    /* NOTE: Do NOT call m3g_clear() here!
     * The game already called clear(Background) explicitly after bindTarget.
     * Calling m3g_clear() here would overwrite any triangles already rendered
     * via render(Node,Transform) before this render(World) call.
     * The JSR-184 spec says render(World) replaces the content, but many games
     * call both render(Node,Transform) and render(World) in sequence, expecting
     * the Node renders to persist. */
    
    /* Get active camera from world */
    JavaObject* camera = (JavaObject*)m3g_get_ref_field(world, "activeCamera");
    if (camera) {
        /* Get camera projection parameters */
        float fov = m3g_get_float_field(camera, "fov", 60.0f);
        /* Don't read aspect from camera - it's not in the stub.
         * The aspect ratio is computed from viewport during render. */
        float aspect = 1.0f;
        float near = m3g_get_float_field(camera, "near", 1.0f);
        float far = m3g_get_float_field(camera, "far", 1000.0f);
        
        /* Compute aspect ratio from viewport */
        if (g_m3g.viewport_height > 0) {
            aspect = (float)g_m3g.viewport_width / (float)g_m3g.viewport_height;
        }
        
        m3g_transform_perspective(&g_m3g.projection, fov, aspect, near, far);
    } else {
        /* Default projection if no camera */
        float aspect = 1.0f;
        if (g_m3g.viewport_height > 0) {
            aspect = (float)g_m3g.viewport_width / (float)g_m3g.viewport_height;
        }
        m3g_transform_perspective(&g_m3g.projection, 60.0f, aspect, 1.0f, 1000.0f);
    }
    
    /* Build modelview from camera transform.
     * Prefer camera_inverse set by setCamera (which includes the Transform arg).
     * Fall back to building from camera node's own transform fields. */
    if (g_m3g.camera_set) {
        g_m3g.modelview = g_m3g.camera_inverse;
    } else {
        m3g_transform_identity(&g_m3g.modelview);
        if (camera) {
            M3GTransform camera_transform;
        m3g_build_node_transform(camera, &camera_transform);
        
        /* Check if camera has a non-identity transform */
        int has_cam_transform = 0;
        for (int i = 0; i < 16; i++) {
            if (camera_transform.m[i] != ((i % 5 == 0) ? 1.0f : 0.0f)) {
                has_cam_transform = 1;
                break;
            }
        }
        
            if (has_cam_transform) {
                /* Invert camera transform for view matrix */
                M3GTransform cam_inv;
                m3g_transform_identity(&cam_inv);
                
                /* Transpose rotation part (upper-left 3x3) */
                for (int row = 0; row < 3; row++) {
                    for (int col = 0; col < 3; col++) {
                        cam_inv.m[row * 4 + col] = camera_transform.m[col * 4 + row];
                    }
                }
                
                /* Apply inverse translation: -R^T * T */
                float cam_tx = -camera_transform.m[12];
                float cam_ty = -camera_transform.m[13];
                float cam_tz = -camera_transform.m[14];
                cam_inv.m[12] = cam_inv.m[0] * cam_tx + cam_inv.m[4] * cam_ty + cam_inv.m[8] * cam_tz;
                cam_inv.m[13] = cam_inv.m[1] * cam_tx + cam_inv.m[5] * cam_ty + cam_inv.m[9] * cam_tz;
                cam_inv.m[14] = cam_inv.m[2] * cam_tx + cam_inv.m[6] * cam_ty + cam_inv.m[10] * cam_tz;
                
                g_m3g.modelview = cam_inv;
                /* Also store in camera_inverse for render(Node,Transform) */
                g_m3g.camera_inverse = cam_inv;
                g_m3g.camera_set = 1;
            }
        }
    }
    
    /* Reset lights for new frame (matching reference's resetLights() in render(World)) */
    g_m3g.light_count = 0;
    
    /* Use recursive rendering to traverse the scene graph with transforms */
    fprintf(stderr, "[M3G] renderWorld: camera=%p, viewport=%dx%d\n", 
            (void*)camera, g_m3g.viewport_width, g_m3g.viewport_height);
    m3g_render_node_recursive(jvm, world, &g_m3g.modelview);
    
    fprintf(stderr, "[M3G] renderWorld: DONE, triangles=%d\n", g_m3g.triangles_rendered);
    
    return NATIVE_RETURN_VOID();
}

/* ============================================================================
 * Extended M3G Native Methods - Additional JSR 184 Support
 * ============================================================================ */

/* VertexArray.set(int firstVertex, int numVertices, byte[] src) */
static JavaValue native_vertexarray_set_byte(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint first = args[1].i;
    jint count = args[2].i;
    JavaArray* src = (JavaArray*)args[3].ref;
    
    if (!obj || !src) return NATIVE_RETURN_VOID();
    
    /* Get internal data array using helper functions */
    JavaArray* data = (JavaArray*)m3g_get_ref_field(obj, "data");
    int component_size = m3g_get_int_field(obj, "componentSize", 1);
    
    if (!data) return NATIVE_RETURN_VOID();
    
    /* Copy data */
    int8_t* dst_data = (int8_t*)array_data(data);
    int8_t* src_data = (int8_t*)array_data(src);
    
    int copy_count = count;
    if (first + copy_count > (int)data->length) {
        copy_count = (int)data->length - first;
    }
    
    if (copy_count > 0) {
        memcpy(dst_data + first, src_data, copy_count * component_size);
    }
    
    GFX_DEBUG("VertexArray.set: first=%d, count=%d", first, count);
    return NATIVE_RETURN_VOID();
}

/* VertexArray.set(int firstVertex, int numVertices, short[] src) */
static JavaValue native_vertexarray_set_short(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint first = args[1].i;
    jint count = args[2].i;
    JavaArray* src = (JavaArray*)args[3].ref;
    
    if (!obj || !src) return NATIVE_RETURN_VOID();
    
    /* Get internal data array using helper function */
    JavaArray* data = (JavaArray*)m3g_get_ref_field(obj, "data");
    
    if (!data || data->element_type != T_SHORT) return NATIVE_RETURN_VOID();
    
    /* Copy data */
    int16_t* dst_data = (int16_t*)array_data(data);
    int16_t* src_data = (int16_t*)array_data(src);
    
    int copy_count = count;
    if (first + copy_count > (int)data->length) {
        copy_count = (int)data->length - first;
    }
    
    if (copy_count > 0) {
        memcpy(dst_data + first, src_data, copy_count * sizeof(int16_t));
    }
    
    GFX_DEBUG("VertexArray.set(short[]): first=%d, count=%d", first, count);
    return NATIVE_RETURN_VOID();
}

/* ============================================================================
 * VertexBuffer Native Methods
 * ============================================================================ */

/* VertexBuffer.<init>() */
static JavaValue native_vertexbuffer_init(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* Register in M3G registry */
    m3g_registry_register(obj, 0);
    
    m3g_set_int_field(obj, "vertexCount", 0);
    m3g_set_ref_field(obj, "positions", NULL);
    m3g_set_ref_field(obj, "normals", NULL);
    m3g_set_ref_field(obj, "texCoords", NULL);
    m3g_set_ref_field(obj, "colors", NULL);
    m3g_set_float_field(obj, "positionScale", 1.0f);
    
    GFX_DEBUG("VertexBuffer.<init>");
    return NATIVE_RETURN_VOID();
}

/* VertexBuffer.setPositions(VertexArray positions, float scale, float[] bias) */
static JavaValue native_vertexbuffer_setPositions(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* positions = (JavaObject*)args[1].ref;
    float scale = args[2].f;
    JavaArray* bias = (JavaArray*)args[3].ref;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    m3g_set_ref_field(obj, "positions", positions);
    m3g_set_float_field(obj, "positionScale", scale);
    
    if (positions) {
        int count = m3g_get_int_field(positions, "vertexCount", 0);
        m3g_set_int_field(obj, "vertexCount", count);
    }
    
    if (bias && bias->element_type == T_FLOAT && bias->length >= 3) {
        float* bias_data = (float*)array_data(bias);
        m3g_set_float_field(obj, "biasX", bias_data[0]);
        m3g_set_float_field(obj, "biasY", bias_data[1]);
        m3g_set_float_field(obj, "biasZ", bias_data[2]);
    }
    
    GFX_DEBUG("VertexBuffer.setPositions: scale=%.2f", scale);
    return NATIVE_RETURN_VOID();
}

/* VertexBuffer.setNormals(VertexArray normals) */
static JavaValue native_vertexbuffer_setNormals(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* normals = (JavaObject*)args[1].ref;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    m3g_set_ref_field(obj, "normals", normals);
    
    GFX_DEBUG("VertexBuffer.setNormals");
    return NATIVE_RETURN_VOID();
}

/* VertexBuffer.setTexCoords(int index, VertexArray texCoords, float scale, float[] bias) */
static JavaValue native_vertexbuffer_setTexCoords(JVM* jvm, JavaThread* thread,
                                                   JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    JavaObject* texCoords = (JavaObject*)args[2].ref;
    float scale = args[3].f;
    JavaArray* bias = (JavaArray*)args[4].ref;
    (void)bias;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    if (index == 0) {
        m3g_set_ref_field(obj, "texCoords", texCoords);
        m3g_set_float_field(obj, "texCoordScale", scale);
    }
    
    GFX_DEBUG("VertexBuffer.setTexCoords: index=%d", index);
    return NATIVE_RETURN_VOID();
}

/* VertexBuffer.setColors(VertexArray colors) */
static JavaValue native_vertexbuffer_setColors(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* colors = (JavaObject*)args[1].ref;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    m3g_set_ref_field(obj, "colors", colors);
    
    GFX_DEBUG("VertexBuffer.setColors");
    return NATIVE_RETURN_VOID();
}

/* Transform.invert() */
static JavaValue native_transform_invert(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    M3GTransform* t = get_native_transform(obj);
    if (!t) return NATIVE_RETURN_VOID();
    
    /* 4x4 matrix inversion using Gauss-Jordan elimination */
    float m[16], inv[16];
    memcpy(m, t->m, sizeof(m));
    memset(inv, 0, sizeof(inv));
    inv[0] = inv[5] = inv[10] = inv[15] = 1.0f;
    
    for (int col = 0; col < 4; col++) {
        /* Find pivot */
        int pivot = col;
        for (int row = col + 1; row < 4; row++) {
            if (fabsf(m[row * 4 + col]) > fabsf(m[pivot * 4 + col])) {
                pivot = row;
            }
        }
        
        /* Swap rows */
        if (pivot != col) {
            for (int i = 0; i < 4; i++) {
                float tmp = m[col * 4 + i];
                m[col * 4 + i] = m[pivot * 4 + i];
                m[pivot * 4 + i] = tmp;
                
                tmp = inv[col * 4 + i];
                inv[col * 4 + i] = inv[pivot * 4 + i];
                inv[pivot * 4 + i] = tmp;
            }
        }
        
        /* Check for singular matrix */
        if (fabsf(m[col * 4 + col]) < 0.00001f) {
            GFX_DEBUG("Transform.invert: singular matrix");
            return NATIVE_RETURN_VOID();
        }
        
        /* Scale pivot row */
        float scale = m[col * 4 + col];
        for (int i = 0; i < 4; i++) {
            m[col * 4 + i] /= scale;
            inv[col * 4 + i] /= scale;
        }
        
        /* Eliminate column */
        for (int row = 0; row < 4; row++) {
            if (row != col) {
                float factor = m[row * 4 + col];
                for (int i = 0; i < 4; i++) {
                    m[row * 4 + i] -= factor * m[col * 4 + i];
                    inv[row * 4 + i] -= factor * inv[col * 4 + i];
                }
            }
        }
    }
    
    set_native_transform(obj, &(M3GTransform){.m = {inv[0], inv[1], inv[2], inv[3],
                                                    inv[4], inv[5], inv[6], inv[7],
                                                    inv[8], inv[9], inv[10], inv[11],
                                                    inv[12], inv[13], inv[14], inv[15]}});
    
    GFX_DEBUG("Transform.invert");
    return NATIVE_RETURN_VOID();
}

/* Transform.transform(float[] vectors) */
static JavaValue native_transform_transform(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaArray* vectors = (JavaArray*)args[1].ref;
    
    if (!obj || !vectors || vectors->element_type != T_FLOAT) {
        return NATIVE_RETURN_VOID();
    }
    
    M3GTransform* t = get_native_transform(obj);
    if (!t) return NATIVE_RETURN_VOID();
    
    float* data = (float*)array_data(vectors);
    int count = vectors->length / 4;
    
    for (int i = 0; i < count; i++) {
        float x = data[i * 4 + 0];
        float y = data[i * 4 + 1];
        float z = data[i * 4 + 2];
        float w = data[i * 4 + 3];
        
        data[i * 4 + 0] = t->m[0]*x + t->m[4]*y + t->m[8]*z + t->m[12]*w;
        data[i * 4 + 1] = t->m[1]*x + t->m[5]*y + t->m[9]*z + t->m[13]*w;
        data[i * 4 + 2] = t->m[2]*x + t->m[6]*y + t->m[10]*z + t->m[14]*w;
        data[i * 4 + 3] = t->m[3]*x + t->m[7]*y + t->m[11]*z + t->m[15]*w;
    }
    
    GFX_DEBUG("Transform.transform: %d vectors", count);
    return NATIVE_RETURN_VOID();
}

/* Light.setColor(int ARGB) */
static JavaValue native_light_setColor(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint argb = args[1].i;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* Find the correct light slot for this object */
    int light_idx = find_light_for_object(obj);
    if (light_idx < 0) {
        /* Auto-register if not found */
        if (g_m3g.light_count < 8) {
            light_idx = g_m3g.light_count++;
            memset(&g_m3g.lights[light_idx], 0, sizeof(M3GLight));
            g_m3g.lights[light_idx].obj = obj;
            g_m3g.lights[light_idx].color[3] = 1.0f;
        } else {
            return NATIVE_RETURN_VOID();
        }
    }
    
    M3GLight* light = &g_m3g.lights[light_idx];
    light->color[0] = ((argb >> 16) & 0xFF) / 255.0f;
    light->color[1] = ((argb >> 8) & 0xFF) / 255.0f;
    light->color[2] = (argb & 0xFF) / 255.0f;
    light->color[3] = ((argb >> 24) & 0xFF) / 255.0f;
    
    GFX_DEBUG("Light.setColor: 0x%08X", argb);
    return NATIVE_RETURN_VOID();
}

/* Light.setIntensity(float intensity) */
static JavaValue native_light_setIntensity(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    float intensity = args[1].f;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* Find the correct light slot for this object */
    int light_idx = find_light_for_object(obj);
    if (light_idx < 0) {
        /* Auto-register if not found */
        if (g_m3g.light_count < 8) {
            light_idx = g_m3g.light_count++;
            memset(&g_m3g.lights[light_idx], 0, sizeof(M3GLight));
            g_m3g.lights[light_idx].obj = obj;
        } else {
            return NATIVE_RETURN_VOID();
        }
    }
    
    M3GLight* light = &g_m3g.lights[light_idx];
    light->color[3] = intensity;
    
    GFX_DEBUG("Light.setIntensity: %.2f", intensity);
    return NATIVE_RETURN_VOID();
}

/* Light.setDirection(float x, float y, float z) */
static JavaValue native_light_setDirection(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    float x = args[1].f;
    float y = args[2].f;
    float z = args[3].f;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* Find the correct light slot for this object */
    int light_idx = find_light_for_object(obj);
    if (light_idx < 0) {
        /* Auto-register if not found */
        if (g_m3g.light_count < 8) {
            light_idx = g_m3g.light_count++;
            memset(&g_m3g.lights[light_idx], 0, sizeof(M3GLight));
            g_m3g.lights[light_idx].obj = obj;
        } else {
            return NATIVE_RETURN_VOID();
        }
    }
    
    M3GLight* light = &g_m3g.lights[light_idx];
    light->direction[0] = x;
    light->direction[1] = y;
    light->direction[2] = z;
    light->direction[3] = 0.0f;  /* Direction, not position */
    
    GFX_DEBUG("Light.setDirection: (%.2f, %.2f, %.2f)", x, y, z);
    return NATIVE_RETURN_VOID();
}

/* Material.setColor(int target, int ARGB) */
static JavaValue native_material_setColor(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint target = args[1].i;
    jint argb = args[2].i;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* Store in fields using helper functions */
    if (target & M3G_MATERIAL_AMBIENT) {
        m3g_set_int_field(obj, "ambient", argb);
    }
    if (target & M3G_MATERIAL_DIFFUSE) {
        m3g_set_int_field(obj, "diffuse", argb);
    }
    if (target & M3G_MATERIAL_SPECULAR) {
        m3g_set_int_field(obj, "specular", argb);
    }
    if (target & M3G_MATERIAL_EMISSIVE) {
        m3g_set_int_field(obj, "emissive", argb);
    }
    
    GFX_DEBUG("Material.setColor: target=%d, ARGB=0x%08X", target, argb);
    return NATIVE_RETURN_VOID();
}

/* Material.setShininess(float shininess) */
static JavaValue native_material_setShininess(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    float shininess = args[1].f;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* Store shininess using helper function */
    m3g_set_float_field(obj, "shininess", shininess);
    
    GFX_DEBUG("Material.setShininess: %.2f", shininess);
    return NATIVE_RETURN_VOID();
}

/* Appearance.setMaterial(Material) */
static JavaValue native_appearance_setMaterial(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* material = (JavaObject*)args[1].ref;
    
    if (obj) {
        m3g_set_ref_field(obj, "material", material);
    }
    
    GFX_DEBUG("Appearance.setMaterial: material=%p", (void*)material);
    return NATIVE_RETURN_VOID();
}

/* Appearance.getMaterial() */
static JavaValue native_appearance_getMaterial(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        return NATIVE_RETURN_NULL();
    }
    
    JavaObject* material = m3g_get_ref_field(obj, "material");
    GFX_DEBUG("Appearance.getMaterial: material=%p", (void*)material);
    return NATIVE_RETURN_OBJECT(material);
}

/* Appearance.setTexture(int index, Texture2D) */
static JavaValue native_appearance_setTexture(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    JavaObject* texture = (JavaObject*)args[2].ref;
    
    if (obj && index == 0) {
        m3g_set_ref_field(obj, "texture", texture);
    }
    
    GFX_DEBUG("Appearance.setTexture: index=%d, texture=%p", index, (void*)texture);
    return NATIVE_RETURN_VOID();
}

/* Texture2D.<init>(Image2D) */
static JavaValue native_texture2d_init(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* image = (JavaObject*)args[1].ref;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* Get image data using helper functions */
    if (image) {
        m3g_set_ref_field(obj, "pixels", m3g_get_ref_field(image, "pixels"));
        m3g_set_int_field(obj, "width", m3g_get_int_field(image, "width", 64));
        m3g_set_int_field(obj, "height", m3g_get_int_field(image, "height", 64));
    }
    
    GFX_DEBUG("Texture2D.<init>: image=%p", (void*)image);
    return NATIVE_RETURN_VOID();
}

/* Texture2D.setFiltering(int level) */
static JavaValue native_texture2d_setFiltering(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint level = args[1].i;
    (void)obj; (void)level;
    
    GFX_DEBUG("Texture2D.setFiltering: level=%d", level);
    return NATIVE_RETURN_VOID();
}

/* Texture2D.setWrapping(int wrapS, int wrapT) */
static JavaValue native_texture2d_setWrapping(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint wrap_s = args[1].i;
    jint wrap_t = args[2].i;
    (void)obj; (void)wrap_s; (void)wrap_t;
    
    GFX_DEBUG("Texture2D.setWrapping: s=%d, t=%d", wrap_s, wrap_t);
    return NATIVE_RETURN_VOID();
}

/* Mesh.<init>(VertexArray, IndexBuffer, Appearance) */
static JavaValue native_mesh_init(JVM* jvm, JavaThread* thread,
                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* vertices = (JavaObject*)args[1].ref;
    JavaObject* indices = (JavaObject*)args[2].ref;
    JavaObject* appearance = (JavaObject*)args[3].ref;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* Register in M3G registry for force-render fallback.
     * Games that create M3G objects via Java code (not our M3G binary parser)
     * need their objects tracked here so we can render them even if the
     * game's scene graph setup is incomplete. */
    m3g_registry_register(obj, 0);
    
    /* Store references using helper functions */
    m3g_set_ref_field(obj, "vertexBuffer", vertices);
    m3g_set_ref_field(obj, "indices", indices);
    m3g_set_ref_field(obj, "appearance", appearance);
    
    GFX_DEBUG("Mesh.<init>: vertices=%p, indices=%p, appearance=%p",
            (void*)vertices, (void*)indices, (void*)appearance);
    return NATIVE_RETURN_VOID();
}

/* TriangleStripArray.<init>(int firstIndex, int[] stripLengths) */
static JavaValue native_trianglestrip_init(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint first = args[1].i;
    JavaArray* lengths = (JavaArray*)args[2].ref;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* Calculate total indices needed */
    int total_indices = 0;
    int strip_count = lengths ? lengths->length : 0;
    jint* len_data = lengths ? (jint*)array_data(lengths) : NULL;
    
    /* Sanity check: limit strip count */
    if (strip_count > 1024) {
        GFX_DEBUG("TriangleStripArray: strip_count %d exceeds max 1024", strip_count);
        strip_count = 1024;
    }
    
    for (int i = 0; i < strip_count; i++) {
        int strip_len = len_data[i];
        /* Skip invalid strips (need at least 3 vertices for a triangle) */
        if (strip_len < 3) continue;
        /* Each strip of n vertices produces (n-2) triangles */
        int strip_indices = (strip_len - 2) * 3;
        /* Check for overflow */
        if (total_indices + strip_indices > M3G_MAX_INDEX_COUNT) {
            GFX_DEBUG("TriangleStripArray: total_indices overflow, clamping");
            break;
        }
        total_indices += strip_indices;
    }
    
    /* Validate total_indices before allocation */
    if (total_indices <= 0 || total_indices > M3G_MAX_INDEX_COUNT) {
        GFX_DEBUG("TriangleStripArray: invalid total_indices %d", total_indices);
        return NATIVE_RETURN_VOID();
    }
    
    /* Create index buffer */
    JavaArray* indices = jvm_new_array(jvm, T_SHORT, total_indices, NULL);
    if (indices) {
        int idx = 0;
        int vertex = first;
        
        for (int strip = 0; strip < strip_count && idx < total_indices; strip++) {
            int strip_len = len_data[strip];
            if (strip_len < 3) continue;
            
            /* Generate triangle strip indices */
            for (int t = 0; t < strip_len - 2 && idx + 2 < total_indices; t++) {
                int16_t* data = (int16_t*)array_data(indices);
                if (t % 2 == 0) {
                    data[idx++] = vertex + t;
                    data[idx++] = vertex + t + 1;
                    data[idx++] = vertex + t + 2;
                } else {
                    /* Swap last two for correct winding */
                    data[idx++] = vertex + t;
                    data[idx++] = vertex + t + 2;
                    data[idx++] = vertex + t + 1;
                }
            }
            vertex += strip_len;
        }
        
        /* Store index buffer using helper function */
        m3g_set_ref_field(obj, "indices", (JavaObject*)indices);
    }
    
    GFX_DEBUG("TriangleStripArray.<init>: first=%d, strips=%d, indices=%d",
            first, strip_count, total_indices);
    return NATIVE_RETURN_VOID();
}

/* Graphics3D.setCamera(Camera, Transform) */
static JavaValue native_graphics3d_setCamera(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* g3d = (JavaObject*)args[0].ref;
    JavaObject* camera = (JavaObject*)args[1].ref;
    JavaObject* transform = (JavaObject*)args[2].ref;
    (void)g3d;
    
    g_m3g_scene_setup_done = true;  /* Track that scene setup is happening */
    m3g_thread_fence();
    g_m3g.camera = camera;
    
    fprintf(stderr, "[M3G-SETCAM] camera=%p transform=%p\n", (void*)camera, (void*)transform);
    
    if (camera) {
        /* Build camera's composite transform */
        M3GTransform cam_trans;
        if (transform) {
            /* Read transform from the Java Transform object */
            JavaArray* matrix_arr = (JavaArray*)m3g_get_ref_field(transform, "matrix");
            fprintf(stderr, "[M3G-SETCAM] transform.matrix=%p type=%d len=%d\n",
                    (void*)matrix_arr, matrix_arr ? matrix_arr->element_type : -1, matrix_arr ? matrix_arr->length : -1);
            if (matrix_arr && matrix_arr->element_type == T_FLOAT && matrix_arr->length >= 16) {
                float* m = (float*)array_data(matrix_arr);
                fprintf(stderr, "[M3G-SETCAM] matrix raw: %.3f,%.3f,%.3f,%.3f / %.3f,%.3f,%.3f,%.3f / %.3f,%.3f,%.3f,%.3f / %.3f,%.3f,%.3f,%.3f\n",
                    m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
                /* Java Transform stores row-major; convert to column-major */
                m3g_transform_from_rowmajor(&cam_trans, m);
            } else {
                m3g_transform_identity(&cam_trans);
                fprintf(stderr, "[M3G-SETCAM] Using identity (matrix field missing or wrong type)\n");
            }
        } else {
            m3g_transform_identity(&cam_trans);
            fprintf(stderr, "[M3G-SETCAM] Using identity (transform arg is null)\n");
        }
        
        g_m3g.camera_transform = cam_trans;
        
        /* Compute inverse for view matrix */
        M3GTransform cam_inv;
        m3g_transform_identity(&cam_inv);
        
        /* Transpose rotation part (upper-left 3x3) */
        for (int row = 0; row < 3; row++) {
            for (int col = 0; col < 3; col++) {
                cam_inv.m[row * 4 + col] = cam_trans.m[col * 4 + row];
            }
        }
        
        /* Apply inverse translation: -R^T * T */
        float tx = -cam_trans.m[12];
        float ty = -cam_trans.m[13];
        float tz = -cam_trans.m[14];
        cam_inv.m[12] = cam_inv.m[0] * tx + cam_inv.m[4] * ty + cam_inv.m[8] * tz;
        cam_inv.m[13] = cam_inv.m[1] * tx + cam_inv.m[5] * ty + cam_inv.m[9] * tz;
        cam_inv.m[14] = cam_inv.m[2] * tx + cam_inv.m[6] * ty + cam_inv.m[10] * tz;
        
        g_m3g.camera_inverse = cam_inv;
        
        /* Build projection from camera parameters */
        float fov = m3g_get_float_field(camera, "fov", 60.0f);
        /* FIX 31: Compute aspect from viewport. If viewport not set yet (0x0),
         * use 0.75 as fallback for typical 240x320 screen. */
        float aspect = 0.75f;
        float near = m3g_get_float_field(camera, "near", 1.0f);
        float far = m3g_get_float_field(camera, "far", 1000.0f);
        
        if (g_m3g.viewport_width > 0 && g_m3g.viewport_height > 0) {
            aspect = (float)g_m3g.viewport_width / (float)g_m3g.viewport_height;
        }
        
        /* Check if camera uses generic projection */
        int projection_type = m3g_get_int_field(camera, "projectionType", 0);
        if (projection_type == 48) { /* GENERIC - use stored matrix */
            JavaArray* gen_matrix = (JavaArray*)m3g_get_ref_field(camera, "genericMatrix");
            if (gen_matrix && gen_matrix->element_type == T_FLOAT && gen_matrix->length >= 16) {
                float* m = (float*)array_data(gen_matrix);
                m3g_transform_from_rowmajor(&g_m3g.projection, m);
            } else {
                m3g_transform_identity(&g_m3g.projection);
            }
        } else if (projection_type == 49) { /* PARALLEL */
            /* From JSR-184 Camera.java: h = fovy, w = aspect * h */
            float fovy = fov;  /* stored as "fov" field */
            float h = fovy;
            float w = aspect * h;
            float d = fabsf(far - near);
            float b = near + far;
            /* Build parallel projection matrix matching Camera.computeMatrix() */
            memset(g_m3g.projection.m, 0, sizeof(g_m3g.projection.m));
            g_m3g.projection.m[0]  = 2.0f / w;           /* (0,0) */
            g_m3g.projection.m[5]  = 2.0f / h;           /* (1,1) */
            g_m3g.projection.m[10] = -2.0f / d;          /* (2,2) */
            g_m3g.projection.m[11] = -b / d;              /* (2,3) */
            g_m3g.projection.m[14] = 0.0f;                /* (3,2) = 0 for ortho */
            g_m3g.projection.m[15] = 1.0f;                /* (3,3) */
            GFX_DEBUG("setCamera: PARALLEL w=%.2f h=%.2f d=%.2f b=%.2f", w, h, d, b);
        } else {
            /* Perspective projection (type 50 or default) */
            m3g_transform_perspective(&g_m3g.projection, fov, aspect, near, far);
        }
        
        /* Set modelview = camera inverse */
        g_m3g.modelview = cam_inv;
    } else {
        m3g_transform_identity(&g_m3g.camera_transform);
        m3g_transform_identity(&g_m3g.camera_inverse);
        m3g_transform_identity(&g_m3g.modelview);
    }
    
    g_m3g.camera_set = 1;
    
    /* Reset clip diagnostic counter so next renderMesh will log with new camera */
    g_m3g_clip_triangles_in = 0;
    g_m3g_raster_pixels_written = 0;
    
    GFX_DEBUG("setCamera: camera=%p", (void*)camera);
    return NATIVE_RETURN_VOID();
}

/* Graphics3D.resetLights() */
static JavaValue native_graphics3d_resetLights(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    (void)args;
    
    g_m3g.light_count = 0;
    memset(g_m3g.lights, 0, sizeof(g_m3g.lights));
    
    return NATIVE_RETURN_VOID();
}

/* Helper: Build a Transform from a Java Transform object.
 * Java Transform stores matrix in row-major order; C M3GTransform uses column-major.
 * We must transpose when copying. */
static void m3g_read_java_transform(JavaObject* transform_obj, M3GTransform* out) {
    m3g_transform_identity(out);
    if (!transform_obj) return;
    
    JavaArray* matrix_arr = (JavaArray*)m3g_get_ref_field(transform_obj, "matrix");
    if (matrix_arr && matrix_arr->element_type == T_FLOAT && matrix_arr->length >= 16) {
        float* m = (float*)array_data(matrix_arr);
        /* Java stores row-major; convert to column-major */
        m3g_transform_from_rowmajor(out, m);
    }
}

/* Helper: Render a Mesh node with the given modelview-projection matrix */
static void m3g_render_mesh_with_mvp(JVM* jvm, JavaObject* mesh, M3GTransform* modelview) {
    if (!mesh) return;
    
    /* Build MVP = projection * modelview (OpenGL convention) */
    M3GTransform mvp;
    m3g_transform_multiply(&mvp, &g_m3g.projection, modelview);
    
    m3g_render_single_mesh(jvm, mesh, &mvp);
}

/* Graphics3D.render(Node, Transform) - Matches FreeJ2ME reference implementation */
static JavaValue native_graphics3d_render_node(JVM* jvm, JavaThread* thread,
                                                 JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* g3d = (JavaObject*)args[0].ref;
    JavaObject* node = (JavaObject*)args[1].ref;
    JavaObject* transform = (JavaObject*)args[2].ref;
    (void)g3d;
    
    if (!node) return NATIVE_RETURN_VOID();
    
    fprintf(stderr, "[M3G-RENDER-NODE] node=%p class=%s\n", (void*)node,
            node->header.clazz ? node->header.clazz->class_name : "?");
    
    /* If bindTarget hasn't been called (no framebuffer), record this render call
     * for later replay during force-render. This handles games that call render()
     * before bindTarget or never call bindTarget at all.
     *
     * IMPORTANT: Must check g_m3g_bindtarget_called (reset each paint cycle), NOT
     * buffers_allocated (persists forever after first allocation). Using buffers_allocated
     * would stop queuing after the first force-render allocates buffers. */
    if (!g_m3g_bindtarget_called && g_m3g_pending_render_count < M3G_MAX_PENDING_RENDERS) {
        int idx = g_m3g_pending_render_count;
        g_m3g_pending_renders[idx].node = node;
        g_m3g_pending_renders[idx].transform = transform;
        g_m3g_pending_render_count++;
        g_m3g_scene_setup_done = true;
        m3g_thread_fence();
        fprintf(stderr, "[M3G-RENDER-NODE] Queued pending render #%d (node=%p, total pending=%d)\n",
                idx, (void*)node, g_m3g_pending_render_count);
        return NATIVE_RETURN_VOID();
    }
    
    /* Read the incoming transform */
    M3GTransform input_transform;
    m3g_read_java_transform(transform, &input_transform);
    
    /* Build the combined modelview: input_transform * camera_inverse */
    M3GTransform combined;
    m3g_transform_multiply(&combined, &g_m3g.camera_inverse, &input_transform);
    
    JavaClass* clazz = node->header.clazz;
    if (!clazz || !clazz->class_name) return NATIVE_RETURN_VOID();
    
    const char* class_name = clazz->class_name;
    
    /* Handle Mesh nodes */
    if (strstr(class_name, "Mesh") != NULL) {
        /* Check rendering enabled */
        int render_enable = m3g_get_int_field(node, "renderingEnable", 1);
        fprintf(stderr, "[M3G-RENDER-NODE] Mesh renderingEnable=%d\n", render_enable);
        if (render_enable) {
            /* Build the node's local transform and combine */
            M3GTransform node_transform;
            m3g_build_node_transform(node, &node_transform);
            
            /* Full modelview = combined * node_transform */
            M3GTransform full_modelview;
            m3g_transform_multiply(&full_modelview, &combined, &node_transform);
            
            m3g_render_mesh_with_mvp(jvm, node, &full_modelview);
        }
        return NATIVE_RETURN_VOID();
    }
    
    /* Handle Group/World nodes - recurse into children */
    if (strstr(class_name, "Group") != NULL || strstr(class_name, "World") != NULL) {
        JavaArray* children = (JavaArray*)m3g_get_ref_field(node, "children");
        fprintf(stderr, "[M3G-RENDER-NODE] Group/World children=%p count=%d elemType=0x%x\n",
                (void*)children, children ? (int)children->length : -1,
                children ? children->element_type : 0);
        if (children && children->length > 0 && children->element_type == DESC_OBJECT) {
            JavaObject** child_arr = (JavaObject**)array_data(children);
            int safe_count = m3g_get_int_field(node, "childCount", (int)children->length);
            if (safe_count > (int)children->length) safe_count = (int)children->length;
            for (jsize c = 0; c < safe_count; c++) {
                JavaObject* child = child_arr[c];
                if (!child) continue;
                
                JavaClass* child_class = child->header.clazz;
                if (!child_class || !child_class->class_name) continue;
                
                const char* child_name = child_class->class_name;
                
                /* Only process renderable node types */
                if (strstr(child_name, "Mesh") != NULL || 
                    strstr(child_name, "Group") != NULL || 
                    strstr(child_name, "World") != NULL ||
                    strstr(child_name, "Sprite3D") != NULL) {
                    
                    /* Get child's composite transform */
                    M3GTransform child_transform;
                    m3g_build_node_transform(child, &child_transform);
                    
                    /* Pre-multiply with parent transform: child_transform = input_transform * child_transform */
                    M3GTransform child_combined;
                    m3g_transform_multiply(&child_combined, &input_transform, &child_transform);
                    
                    /* Store back the combined transform for the recursive call */
                    /* We need to create a temporary Java Transform with this matrix */
                    /* Since we can't easily create Java objects here, we handle it inline */
                    
                    if (strstr(child_name, "Mesh") != NULL) {
                        /* Direct mesh rendering */
                        int render_enable = m3g_get_int_field(child, "renderingEnable", 1);
                        fprintf(stderr, "[M3G-RENDER-NODE]   child Mesh %p enable=%d\n",
                                (void*)child, render_enable);
                        if (render_enable) {
                            M3GTransform full_modelview;
                            m3g_transform_multiply(&full_modelview, &g_m3g.camera_inverse, &child_combined);
                            m3g_render_mesh_with_mvp(jvm, child, &full_modelview);
                            fprintf(stderr, "[M3G-RENDER-NODE]   rendered Mesh %p, total triangles=%d\n",
                                    (void*)child, g_m3g.triangles_rendered);
                        }
                    } else {
                        /* Group or World - recurse with combined transform */
                        /* Build full modelview for this branch */
                        M3GTransform branch_modelview;
                        m3g_transform_multiply(&branch_modelview, &g_m3g.camera_inverse, &child_combined);
                        
                        /* Now recurse, using branch_modelview as the base */
                        /* We temporarily set g_m3g.camera_inverse to branch_modelview for the recursion,
                         * then restore it. This effectively passes the transform down. */
                        M3GTransform saved_cam_inv = g_m3g.camera_inverse;
                        g_m3g.camera_inverse = branch_modelview;
                        
                        /* Recurse into group's children with identity as the additional transform */
                        JavaArray* sub_children = (JavaArray*)m3g_get_ref_field(child, "children");
                        if (sub_children && sub_children->length > 0 && sub_children->element_type == DESC_OBJECT) {
                            JavaObject** sub_arr = (JavaObject**)array_data(sub_children);
                            int sub_safe = m3g_get_int_field(child, "childCount", (int)sub_children->length);
                            if (sub_safe > (int)sub_children->length) sub_safe = (int)sub_children->length;
                            for (jsize sc = 0; sc < sub_safe; sc++) {
                                JavaObject* sub_child = sub_arr[sc];
                                if (!sub_child) continue;
                                
                                JavaClass* sc_class = sub_child->header.clazz;
                                if (!sc_class || !sc_class->class_name) continue;
                                
                                const char* sc_name = sc_class->class_name;
                                
                                if (strstr(sc_name, "Mesh") != NULL) {
                                    int re = m3g_get_int_field(sub_child, "renderingEnable", 1);
                                    if (re) {
                                        M3GTransform sc_transform;
                                        m3g_build_node_transform(sub_child, &sc_transform);
                                        
                                        M3GTransform sc_full;
                                        m3g_transform_multiply(&sc_full, &branch_modelview, &sc_transform);
                                        
                                        M3GTransform sc_mvp;
                                        m3g_transform_multiply(&sc_mvp, &g_m3g.projection, &sc_full);
                                        m3g_render_single_mesh(jvm, sub_child, &sc_mvp);
                                    }
                                } else if (strstr(sc_name, "Group") != NULL || strstr(sc_name, "World") != NULL) {
                                    /* Further recursion - set camera_inverse and recurse */
                                    M3GTransform sc_node_transform;
                                    m3g_build_node_transform(sub_child, &sc_node_transform);
                                    
                                    M3GTransform sc_branch;
                                    m3g_transform_multiply(&sc_branch, &branch_modelview, &sc_node_transform);
                                    
                                    M3GTransform saved2 = g_m3g.camera_inverse;
                                    g_m3g.camera_inverse = sc_branch;
                                    
                                    JavaArray* sc_children = (JavaArray*)m3g_get_ref_field(sub_child, "children");
                                    if (sc_children && sc_children->length > 0 && sc_children->element_type == DESC_OBJECT) {
                                        JavaObject** sc_arr = (JavaObject**)array_data(sc_children);
                                        int sc_safe = m3g_get_int_field(sub_child, "childCount", (int)sc_children->length);
                                        if (sc_safe > (int)sc_children->length) sc_safe = (int)sc_children->length;
                                        for (jsize sci = 0; sci < sc_safe; sci++) {
                                            JavaObject* sci_child = sc_arr[sci];
                                            if (!sci_child) continue;
                                            JavaClass* sci_class = sci_child->header.clazz;
                                            if (!sci_class || !sci_class->class_name) continue;
                                            
                                            if (strstr(sci_class->class_name, "Mesh") != NULL) {
                                                int sre = m3g_get_int_field(sci_child, "renderingEnable", 1);
                                                if (sre) {
                                                    M3GTransform sci_transform;
                                                    m3g_build_node_transform(sci_child, &sci_transform);
                                                    
                                                    M3GTransform sci_full;
                                                    m3g_transform_multiply(&sci_full, &sc_branch, &sci_transform);
                                                    
                                                    M3GTransform sci_mvp;
                                                    m3g_transform_multiply(&sci_mvp, &g_m3g.projection, &sci_full);
                                                    m3g_render_single_mesh(jvm, sci_child, &sci_mvp);
                                                }
                                            }
                                        }
                                    }
                                    
                                    g_m3g.camera_inverse = saved2;
                                }
                            }
                        }
                        
                        g_m3g.camera_inverse = saved_cam_inv;
                    }
                }
            }
        }
        return NATIVE_RETURN_VOID();
    }
    
    /* Sprite3D - not yet implemented but logged */
    GFX_DEBUG("Graphics3D.render: Sprite3D not implemented");
    
    return NATIVE_RETURN_VOID();
}

/* Node.setScale(float sx, float sy, float sz) */
static JavaValue native_node_setScale(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    float sx = args[1].f;
    float sy = args[2].f;
    float sz = args[3].f;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* Store decomposed scale values */
    m3g_set_float_field(obj, "scaleX", sx);
    m3g_set_float_field(obj, "scaleY", sy);
    m3g_set_float_field(obj, "scaleZ", sz);
    
    /* Rebuild the composite transform matrix from decomposed values.
     * The M3G Transformable composes as: Translation * Rotation * Scale.
     * Build S, R, T matrices and compute T * R * S. */
    float tx = m3g_get_float_field(obj, "translationX", 0.0f);
    float ty = m3g_get_float_field(obj, "translationY", 0.0f);
    float tz = m3g_get_float_field(obj, "translationZ", 0.0f);
    float angle = m3g_get_float_field(obj, "orientationAngle", 0.0f);
    float ax = m3g_get_float_field(obj, "orientationX", 0.0f);
    float ay = m3g_get_float_field(obj, "orientationY", 0.0f);
    float az = m3g_get_float_field(obj, "orientationZ", 1.0f);
    
    /* Start with scale */
    M3GTransform result;
    m3g_transform_identity(&result);
    m3g_transform_scale(&result, sx, sy, sz);
    
    /* Apply rotation */
    float angle_rad = angle * 3.14159f / 180.0f;
    m3g_transform_rotate(&result, angle_rad, ax, ay, az);
    
    /* Apply translation */
    m3g_transform_translate(&result, tx, ty, tz);
    
    /* Store back into the node's transform matrix field */
    JavaArray* matrix = (JavaArray*)m3g_get_ref_field(obj, "transform");
    if (matrix && matrix->element_type == T_FLOAT && matrix->length >= 16) {
        float* m = (float*)array_data(matrix);
        memcpy(m, result.m, 16 * sizeof(float));
    }
    
    GFX_DEBUG("Node.setScale: (%.2f, %.2f, %.2f)", sx, sy, sz);
    return NATIVE_RETURN_VOID();
}

/* Node.setTranslation(float x, float y, float z) */
static JavaValue native_node_setTranslation(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    float x = args[1].f;
    float y = args[2].f;
    float z = args[3].f;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* Store decomposed translation values */
    m3g_set_float_field(obj, "translationX", x);
    m3g_set_float_field(obj, "translationY", y);
    m3g_set_float_field(obj, "translationZ", z);
    
    /* Rebuild the composite transform matrix from decomposed values */
    float sx = m3g_get_float_field(obj, "scaleX", 1.0f);
    float sy = m3g_get_float_field(obj, "scaleY", 1.0f);
    float sz = m3g_get_float_field(obj, "scaleZ", 1.0f);
    float angle = m3g_get_float_field(obj, "orientationAngle", 0.0f);
    float ax = m3g_get_float_field(obj, "orientationX", 0.0f);
    float ay = m3g_get_float_field(obj, "orientationY", 0.0f);
    float az = m3g_get_float_field(obj, "orientationZ", 1.0f);
    
    /* Start with scale */
    M3GTransform result;
    m3g_transform_identity(&result);
    m3g_transform_scale(&result, sx, sy, sz);
    
    /* Apply rotation */
    float angle_rad = angle * 3.14159f / 180.0f;
    m3g_transform_rotate(&result, angle_rad, ax, ay, az);
    
    /* Apply translation */
    m3g_transform_translate(&result, x, y, z);
    
    /* Store back into the node's transform matrix field */
    JavaArray* matrix = (JavaArray*)m3g_get_ref_field(obj, "transform");
    if (matrix && matrix->element_type == T_FLOAT && matrix->length >= 16) {
        float* m = (float*)array_data(matrix);
        memcpy(m, result.m, 16 * sizeof(float));
    }
    
    GFX_DEBUG("Node.setTranslation: (%.2f, %.2f, %.2f)", x, y, z);
    return NATIVE_RETURN_VOID();
}

/* Node.setRotation(float angle, float ax, float ay, float az) */
static JavaValue native_node_setRotation(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    float angle = args[1].f;
    float ax = args[2].f;
    float ay = args[3].f;
    float az = args[4].f;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* Store decomposed rotation values */
    m3g_set_float_field(obj, "orientationAngle", angle);
    m3g_set_float_field(obj, "orientationX", ax);
    m3g_set_float_field(obj, "orientationY", ay);
    m3g_set_float_field(obj, "orientationZ", az);
    
    /* Rebuild the composite transform matrix from decomposed values */
    float tx = m3g_get_float_field(obj, "translationX", 0.0f);
    float ty = m3g_get_float_field(obj, "translationY", 0.0f);
    float tz = m3g_get_float_field(obj, "translationZ", 0.0f);
    float sx = m3g_get_float_field(obj, "scaleX", 1.0f);
    float sy = m3g_get_float_field(obj, "scaleY", 1.0f);
    float sz = m3g_get_float_field(obj, "scaleZ", 1.0f);
    
    /* Start with scale */
    M3GTransform result;
    m3g_transform_identity(&result);
    m3g_transform_scale(&result, sx, sy, sz);
    
    /* Apply rotation */
    float angle_rad = angle * 3.14159f / 180.0f;
    m3g_transform_rotate(&result, angle_rad, ax, ay, az);
    
    /* Apply translation */
    m3g_transform_translate(&result, tx, ty, tz);
    
    /* Store back into the node's transform matrix field */
    JavaArray* matrix = (JavaArray*)m3g_get_ref_field(obj, "transform");
    if (matrix && matrix->element_type == T_FLOAT && matrix->length >= 16) {
        float* m = (float*)array_data(matrix);
        memcpy(m, result.m, 16 * sizeof(float));
    }
    
    GFX_DEBUG("Node.setRotation: angle=%.1f, axis=(%.2f, %.2f, %.2f)", angle, ax, ay, az);
    return NATIVE_RETURN_VOID();
}

/* Camera.setGeneric(Transform) */
static JavaValue native_camera_setGeneric(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* transform = (JavaObject*)args[1].ref;
    (void)obj; (void)transform;
    
    GFX_DEBUG("Camera.setGeneric");
    return NATIVE_RETURN_VOID();
}

/* Background.setColor(int ARGB) */
static JavaValue native_background_setColor(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint argb = args[1].i;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* Store color using helper function */
    m3g_set_int_field(obj, "clearColor", argb);
    
    GFX_DEBUG("Background.setColor: 0x%08X", argb);
    return NATIVE_RETURN_VOID();
}

/* Image2D.<init>(int format, int width, int height) */
static JavaValue native_image2d_init(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint format = args[1].i;
    jint width = args[2].i;
    jint height = args[3].i;
    
    if (!obj || width <= 0 || height <= 0) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Check for oversized images to prevent OOM errors from malformed M3G files */
    if (width > M3G_MAX_IMAGE_DIMENSION || height > M3G_MAX_IMAGE_DIMENSION) {
        GFX_DEBUG("Image2D.<init>: REJECTED - dimensions too large: %dx%d (max %d)", 
                width, height, M3G_MAX_IMAGE_DIMENSION);
        return NATIVE_RETURN_VOID();
    }
    
    /* Check for integer overflow in width * height calculation */
    jint pixel_count = width * height;
    if (pixel_count > M3G_MAX_IMAGE_PIXELS || pixel_count <= 0) {
        GFX_DEBUG("Image2D.<init>: REJECTED - pixel count overflow: %d", pixel_count);
        return NATIVE_RETURN_VOID();
    }
    
    /* Create pixel array (ARGB format) */
    JavaArray* pixels = jvm_new_array(jvm, T_INT, pixel_count, NULL);
    if (!pixels) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Store fields using helper functions */
    m3g_set_ref_field(obj, "pixels", (JavaObject*)pixels);
    m3g_set_int_field(obj, "width", width);
    m3g_set_int_field(obj, "height", height);
    m3g_set_int_field(obj, "format", format);
    
    GFX_DEBUG("Image2D.<init>: format=%d, %dx%d", format, width, height);
    return NATIVE_RETURN_VOID();
}

/* Image2D.<init>(int format, Object image) - creates Image2D from LCDUI Image */
static JavaValue native_image2d_init_from_image(JVM* jvm, JavaThread* thread,
                                                  JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint format = args[1].i;
    JavaObject* image = (JavaObject*)args[2].ref;
    
    if (!obj) {
        return NATIVE_RETURN_VOID();
    }
    
    GFX_DEBUG("Image2D.<init>(int, Object): format=%d, image=%p", format, (void*)image);
    
    int width = 0, height = 0;
    JavaArray* src_pixels = NULL;
    
    /* Try to get image data from LCDUI Image object using helper functions */
    if (image) {
        width = m3g_get_int_field(image, "width", 0);
        height = m3g_get_int_field(image, "height", 0);
        src_pixels = (JavaArray*)m3g_get_ref_field(image, "pixels");
        if (!src_pixels) src_pixels = (JavaArray*)m3g_get_ref_field(image, "data");
        if (!src_pixels) src_pixels = (JavaArray*)m3g_get_ref_field(image, "imageData");
    }
    
    /* If no valid dimensions, use defaults */
    if (width <= 0) width = 64;
    if (height <= 0) height = 64;
    
    /* Check for oversized images to prevent OOM errors */
    if (width > M3G_MAX_IMAGE_DIMENSION || height > M3G_MAX_IMAGE_DIMENSION) {
        GFX_DEBUG("Image2D.<init>(Object): REJECTED - dimensions too large: %dx%d (max %d)", 
                width, height, M3G_MAX_IMAGE_DIMENSION);
        return NATIVE_RETURN_VOID();
    }
    
    /* Check for integer overflow in width * height calculation */
    int pixel_count = width * height;
    if (pixel_count > M3G_MAX_IMAGE_PIXELS || pixel_count <= 0) {
        GFX_DEBUG("Image2D.<init>(Object): REJECTED - pixel count overflow: %d", pixel_count);
        return NATIVE_RETURN_VOID();
    }
    
    /* Create pixel array (ARGB format) */
    JavaArray* pixels = jvm_new_array(jvm, T_INT, pixel_count, NULL);
    if (!pixels) {
        return NATIVE_RETURN_VOID();
    }
    
    /* Copy source pixels if available */
    if (src_pixels && src_pixels->length > 0) {
        int copy_count = src_pixels->length < pixel_count ? src_pixels->length : pixel_count;
        jint* dst_data = (jint*)((uint8_t*)pixels + sizeof(JavaArray));
        jint* src_data = (jint*)((uint8_t*)src_pixels + sizeof(JavaArray));
        for (int i = 0; i < copy_count; i++) {
            dst_data[i] = src_data[i];
        }
    }
    
    /* Store fields using helper functions */
    m3g_set_ref_field(obj, "pixels", (JavaObject*)pixels);
    m3g_set_int_field(obj, "width", width);
    m3g_set_int_field(obj, "height", height);
    m3g_set_int_field(obj, "format", format);
    
    return NATIVE_RETURN_VOID();
}

/* Image2D.set(int x, int y, int width, int height, byte[] pixels) */
static JavaValue native_image2d_set(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint x = args[1].i;
    jint y = args[2].i;
    jint w = args[3].i;
    jint h = args[4].i;
    JavaArray* src = (JavaArray*)args[5].ref;
    
    if (!obj || !src) return NATIVE_RETURN_VOID();
    
    /* Get destination pixel array using helper functions */
    JavaArray* dst = (JavaArray*)m3g_get_ref_field(obj, "pixels");
    int img_width = m3g_get_int_field(obj, "width", 0);
    
    if (!dst) return NATIVE_RETURN_VOID();
    
    uint8_t* src_data = (uint8_t*)array_data(src);
    uint32_t* dst_data = (uint32_t*)array_data(dst);
    
    /* Copy and convert to ARGB */
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            int src_idx = (row * w + col) * 4;  /* Assume RGBA */
            int dst_idx = (y + row) * img_width + (x + col);
            
            if (dst_idx < (int)dst->length) {
                dst_data[dst_idx] = (0xFF << 24) |  /* Alpha */
                                    (src_data[src_idx] << 16) |  /* R */
                                    (src_data[src_idx + 1] << 8) |  /* G */
                                    src_data[src_idx + 2];  /* B */
            }
        }
    }
    
    GFX_DEBUG("Image2D.set: (%d,%d) %dx%d", x, y, w, h);
    return NATIVE_RETURN_VOID();
}

/* ============================================================================
 * Extended JSR 184 Implementation - Additional Features
 * ============================================================================ */

/* World.addChild(Node) */
static JavaValue native_world_addChild(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* world = (JavaObject*)args[0].ref;
    JavaObject* child = (JavaObject*)args[1].ref;
    
    if (!world || !child) return NATIVE_RETURN_VOID();
    
    /* Get children array using helper function */
    JavaArray* children = (JavaArray*)m3g_get_ref_field(world, "children");
    if (children) {
        /* Add child to array - find empty slot */
        JavaObject** arr = (JavaObject**)array_data(children);
        for (jsize j = 0; j < children->length; j++) {
            if (arr[j] == NULL) {
                arr[j] = child;
                GFX_DEBUG("World.addChild: added child %p at index %d", (void*)child, j);
                break;
            }
        }
    } else {
        /* No children array yet - create one */
        children = jvm_new_array(jvm, DESC_OBJECT, 16, NULL);
        if (children) {
            m3g_set_ref_field(world, "children", (JavaObject*)children);
            JavaObject** arr = (JavaObject**)array_data(children);
            arr[0] = child;
            GFX_DEBUG("World.addChild: created children array, added child %p", (void*)child);
        }
    }
    
    /* Mark scene as set up for force-render detection */
    g_m3g_scene_setup_done = true;
    m3g_thread_fence();
    
    return NATIVE_RETURN_VOID();
}

/* World.setActiveCamera(Camera) */
static JavaValue native_world_setActiveCamera(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* world = (JavaObject*)args[0].ref;
    JavaObject* camera = (JavaObject*)args[1].ref;
    
    if (world && camera) {
        m3g_set_ref_field(world, "activeCamera", camera);
        g_m3g_last_world = world;  /* Track last World for force-render */
        g_m3g_scene_setup_done = true;  /* Scene has been set up */
        m3g_thread_fence();
    }
    
    GFX_DEBUG("World.setActiveCamera: world=%p camera=%p", (void*)world, (void*)camera);
    return NATIVE_RETURN_VOID();
}

/* World.getActiveCamera() */
static JavaValue native_world_getActiveCamera(JVM* jvm, JavaThread* thread,
                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* world = (JavaObject*)args[0].ref;
    
    if (!world) return NATIVE_RETURN_NULL();
    
    /* Get active camera from World using helper function */
    JavaObject* camera = m3g_get_ref_field(world, "activeCamera");
    if (camera) {
        return NATIVE_RETURN_OBJECT(camera);
    }
    
    /* Try to find camera in children */
    JavaArray* children = (JavaArray*)m3g_get_ref_field(world, "children");
    if (children && children->element_type == DESC_OBJECT) {
        JavaObject** arr = (JavaObject**)array_data(children);
        for (jsize j = 0; j < children->length; j++) {
            if (arr[j]) {
                JavaClass* child_class = arr[j]->header.clazz;
                if (child_class && child_class->class_name && 
                    strstr(child_class->class_name, "Camera")) {
                    return NATIVE_RETURN_OBJECT(arr[j]);
                }
            }
        }
    }
    
    return NATIVE_RETURN_NULL();
}

/* Helper: Recursively search for object with userID in scene graph */
static JavaObject* m3g_find_recursive(JavaObject* obj, jint user_id) {
    if (!obj) return NULL;

    /* Check if this object matches */
    jint obj_user_id = m3g_get_int_field(obj, "userID", -1);
    if (obj_user_id == user_id) {
        return obj;
    }

    /* Check if this is a Group (has children) - World extends Group */
    JavaClass* clazz = obj->header.clazz;
    if (!clazz || !clazz->class_name) return NULL;

    /* Group, World, and Mesh can have children */
    if (strstr(clazz->class_name, "Group") || strstr(clazz->class_name, "World")) {
        JavaArray* children = (JavaArray*)m3g_get_ref_field(obj, "children");
        if (children && children->element_type == DESC_OBJECT) {
            JavaObject** arr = (JavaObject**)array_data(children);
            for (jsize i = 0; i < children->length; i++) {
                if (arr[i]) {
                    JavaObject* found = m3g_find_recursive(arr[i], user_id);
                    if (found) return found;
                }
            }
        }
    }

    return NULL;
}

/* Object3D.find(int userID) */
static JavaValue native_object3d_find(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint user_id = args[1].i;

    if (!obj) return NATIVE_RETURN_NULL();

    GFX_DEBUG("Object3D.find: searching for userID=%d", user_id);
    fprintf(stderr, "[M3G-FIND] searching userID=%d in obj=%p (registry has %d objects)\n",
            user_id, (void*)obj, g_m3g_registry.count);

    /* Recursively search this object and its children */
    JavaObject* found = m3g_find_recursive(obj, user_id);
    if (found) {
        fprintf(stderr, "[M3G-FIND] found userID=%d at %p (recursive)\n", user_id, (void*)found);
        return NATIVE_RETURN_OBJECT(found);
    }

    /* Fallback: search in global registry */
    found = m3g_registry_find(user_id);
    if (found) {
        fprintf(stderr, "[M3G-FIND] found userID=%d at %p (registry)\n", user_id, (void*)found);
        return NATIVE_RETURN_OBJECT(found);
    }

    fprintf(stderr, "[M3G-FIND] userID=%d NOT FOUND\n", user_id);
    return NATIVE_RETURN_NULL();
}

/* Object3D.getUserID() */
static JavaValue native_object3d_getUserID(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) return NATIVE_RETURN_INT(0);
    
    return NATIVE_RETURN_INT(m3g_get_int_field(obj, "userID", 0));
}

/* Object3D.setUserID(int id) */
static JavaValue native_object3d_setUserID(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint user_id = args[1].i;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    m3g_set_userID(obj, user_id);
    
    GFX_DEBUG("Object3D.setUserID: %d", user_id);
    return NATIVE_RETURN_VOID();
}

/* Object3D.duplicate() - creates a copy of the object */
static JavaValue native_object3d_duplicate(JVM* jvm, JavaThread* thread,
                                            JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) {
        GFX_DEBUG("Object3D.duplicate: null object");
        return NATIVE_RETURN_NULL();
    }
    
    GFX_DEBUG("Object3D.duplicate: duplicating %p", (void*)obj);
    
    /* For simplicity, return the same object (shallow copy) */
    /* A full implementation would create a deep copy */
    return NATIVE_RETURN_OBJECT(obj);
}

/* Node.getTransform(Transform) */
static JavaValue native_node_getTransform(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* node = (JavaObject*)args[0].ref;
    JavaObject* transform = (JavaObject*)args[1].ref;
    
    if (!node || !transform) return NATIVE_RETURN_VOID();
    
    /* Copy node's transform to the provided Transform object using helper functions */
    JavaArray* node_arr = (JavaArray*)m3g_get_ref_field(node, "transform");
    if (node_arr && node_arr->element_type == T_FLOAT) {
        float* node_matrix = (float*)array_data(node_arr);
        
        JavaArray* trans_arr = (JavaArray*)m3g_get_ref_field(transform, "matrix");
        if (trans_arr && trans_arr->element_type == T_FLOAT) {
            float* trans_matrix = (float*)array_data(trans_arr);
            memcpy(trans_matrix, node_matrix, 16 * sizeof(float));
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Node.setTransform(Transform) */
static JavaValue native_node_setTransform(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* node = (JavaObject*)args[0].ref;
    JavaObject* transform = (JavaObject*)args[1].ref;
    
    if (!node || !transform) return NATIVE_RETURN_VOID();
    
    /* Copy transform to node's transform field */
    M3GTransform* t = get_native_transform(transform);
    if (!t) return NATIVE_RETURN_VOID();
    
    JavaArray* node_arr = (JavaArray*)m3g_get_ref_field(node, "transform");
    if (node_arr && node_arr->element_type == T_FLOAT) {
        float* node_matrix = (float*)array_data(node_arr);
        memcpy(node_matrix, t->m, 16 * sizeof(float));
    }
    
    GFX_DEBUG("Node.setTransform");
    return NATIVE_RETURN_VOID();
}

/* Node.setAlphaFactor(float factor) */
static JavaValue native_node_setAlphaFactor(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* node = (JavaObject*)args[0].ref;
    float factor = args[1].f;
    
    if (!node) return NATIVE_RETURN_VOID();
    
    m3g_set_float_field(node, "alphaFactor", factor);
    
    GFX_DEBUG("Node.setAlphaFactor: %.2f", factor);
    return NATIVE_RETURN_VOID();
}

/* Group.addChild(Node) */
static JavaValue native_group_addChild(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* group = (JavaObject*)args[0].ref;
    JavaObject* child = (JavaObject*)args[1].ref;
    
    if (!group || !child) return NATIVE_RETURN_VOID();
    
    /* Validate that child is actually a Node subclass */
    if (!child->header.clazz || !child->header.clazz->class_name) return NATIVE_RETURN_VOID();
    
    int children_slot = m3g_find_field_slot(group, "children");
    fprintf(stderr, "[M3G-ADDCHILD] Group %p class=%s, children_slot=%d, instance_size=%zu\n",
            (void*)group, 
            group->header.clazz ? group->header.clazz->class_name : "?",
            children_slot,
            group->header.clazz ? group->header.clazz->instance_size : 0);
    JavaArray* children = (JavaArray*)m3g_get_ref_field(group, "children");
    if (children && children->element_type == DESC_OBJECT) {
        JavaObject** arr = (JavaObject**)array_data(children);
        /* Find empty slot */
        int added = 0;
        for (jsize j = 0; j < children->length; j++) {
            if (arr[j] == NULL) {
                arr[j] = child;
                added = 1;
                GFX_DEBUG("Group.addChild: added child %p at index %d (class=%s)",
                         (void*)child, j,
                         group->header.clazz ? group->header.clazz->class_name : "?");
                break;
            }
        }
        /* If no empty slot found, expand the array */
        if (!added && children->length < 256) {
            jsize new_len = children->length * 2;
            if (new_len > 256) new_len = 256;
            JavaArray* new_children = jvm_new_array(jvm, DESC_OBJECT, new_len, NULL);
            if (new_children) {
                JavaObject** new_arr = (JavaObject**)array_data(new_children);
                /* Copy existing children */
                for (jsize j = 0; j < children->length; j++) {
                    new_arr[j] = arr[j];
                }
                /* Add new child at the end */
                new_arr[children->length] = child;
                m3g_set_ref_field(group, "children", (JavaObject*)new_children);
                GFX_DEBUG("Group.addChild: expanded array %d->%d, added child %p at index %d",
                         (int)children->length, (int)new_len, (void*)child, (int)children->length);
            }
        }
    } else {
        /* No children array yet - create one */
        children = jvm_new_array(jvm, DESC_OBJECT, 16, NULL);
        if (children) {
            m3g_set_ref_field(group, "children", (JavaObject*)children);
            JavaObject** arr = (JavaObject**)array_data(children);
            arr[0] = child;
            GFX_DEBUG("Group.addChild: created children array, added child %p at index 0", (void*)child);
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Group.removeChild(Node) */
static JavaValue native_group_removeChild(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* group = (JavaObject*)args[0].ref;
    JavaObject* child = (JavaObject*)args[1].ref;
    
    if (!group || !child) return NATIVE_RETURN_VOID();
    
    /* FIX: Use m3g_get_ref_field instead of wrong clazz->fields[i] access */
    JavaArray* children = (JavaArray*)m3g_get_ref_field(group, "children");
    if (children && children->element_type == DESC_OBJECT) {
        JavaObject** arr = (JavaObject**)array_data(children);
        for (jsize j = 0; j < children->length; j++) {
            if (arr[j] == child) {
                arr[j] = NULL;
                GFX_DEBUG("Group.removeChild: removed child at index %d", j);
                break;
            }
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* Group.getChildCount() */
static JavaValue native_group_getChildCount(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* group = (JavaObject*)args[0].ref;
    
    if (!group) return NATIVE_RETURN_INT(0);
    
    JavaArray* children = (JavaArray*)m3g_get_ref_field(group, "children");
    if (children && children->element_type == DESC_OBJECT) {
        /* Count non-null children */
        JavaObject** arr = (JavaObject**)array_data(children);
        int count = 0;
        for (jsize j = 0; j < children->length; j++) {
            if (arr[j]) count++;
        }
        return NATIVE_RETURN_INT(count);
    }
    
    return NATIVE_RETURN_INT(0);
}

/* Group.getChild(int index) */
static JavaValue native_group_getChild(JVM* jvm, JavaThread* thread,
                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* group = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    
    if (!group || index < 0) return NATIVE_RETURN_NULL();
    
    /* FIX: Use m3g_get_ref_field instead of wrong clazz->fields[i] access */
    JavaArray* children = (JavaArray*)m3g_get_ref_field(group, "children");
    if (children && children->element_type == DESC_OBJECT && index < (jint)children->length) {
        JavaObject** arr = (JavaObject**)array_data(children);
        return NATIVE_RETURN_OBJECT(arr[index]);
    }
    
    return NATIVE_RETURN_NULL();
}

/* CompositingMode.setBlending(int mode) */
static JavaValue native_compositingmode_setBlending(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint mode = args[1].i;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    JavaClass* clazz = obj->header.clazz;
    for (int i = 0; i < clazz->fields_count; i++) {
        if (clazz->fields[i].name && strcmp(clazz->fields[i].name, "blending") == 0) {
            obj->fields[i].i = mode;
            break;
        }
    }
    
    GFX_DEBUG("CompositingMode.setBlending: %d", mode);
    return NATIVE_RETURN_VOID();
}

/* PolygonMode.setCulling(int mode) */
static JavaValue native_polygonmode_setCulling(JVM* jvm, JavaThread* thread,
                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint mode = args[1].i;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    g_m3g.culling_enabled = (mode != 0);
    
    GFX_DEBUG("PolygonMode.setCulling: %d", mode);
    return NATIVE_RETURN_VOID();
}

/* Mesh.setVertexBuffer(int index, VertexArray) */
static JavaValue native_mesh_setVertexBuffer(JVM* jvm, JavaThread* thread,
                                              JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* mesh = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    JavaObject* va = (JavaObject*)args[2].ref;
    
    if (!mesh) return NATIVE_RETURN_VOID();
    
    JavaClass* clazz = mesh->header.clazz;
    for (int i = 0; i < clazz->fields_count; i++) {
        if (clazz->fields[i].name && strcmp(clazz->fields[i].name, "vertexBuffers") == 0) {
            JavaArray* buffers = (JavaArray*)mesh->fields[i].ref;
            if (buffers && buffers->element_type == DESC_OBJECT && index >= 0 && index < (jint)buffers->length) {
                JavaObject** arr = (JavaObject**)array_data(buffers);
                arr[index] = va;
            }
            break;
        }
    }
    
    GFX_DEBUG("Mesh.setVertexBuffer: index=%d", index);
    return NATIVE_RETURN_VOID();
}

/* Camera.lookAt(float x, float y, float z, float atX, float atY, float atZ, float upX, float upY, float upZ) */
static JavaValue native_camera_lookAt(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    float eye_x = args[1].f;
    float eye_y = args[2].f;
    float eye_z = args[3].f;
    float at_x = args[4].f;
    float at_y = args[5].f;
    float at_z = args[6].f;
    float up_x = args[7].f;
    float up_y = args[8].f;
    float up_z = args[9].f;
    
    if (!obj) return NATIVE_RETURN_VOID();
    
    /* Update modelview matrix */
    m3g_transform_look_at(&g_m3g.modelview, eye_x, eye_y, eye_z, at_x, at_y, at_z, up_x, up_y, up_z);
    
    GFX_DEBUG("Camera.lookAt: eye=(%.2f,%.2f,%.2f), at=(%.2f,%.2f,%.2f)", 
            eye_x, eye_y, eye_z, at_x, at_y, at_z);
    return NATIVE_RETURN_VOID();
}

/* M3G file format constants */
#define M3G_MAGIC1 0xAB
#define M3G_MAGIC2 0xBB
#define M3G_VERSION_MIN 2
#define M3G_VERSION_MAX 3

/* M3G object type codes (from JSR-184 specification) */
#define M3G_OBJ_HEADER              0
#define M3G_OBJ_ANIMATIONCONTROLLER 1
#define M3G_OBJ_ANIMATIONTRACK      2
#define M3G_OBJ_APPEARANCE          3
#define M3G_OBJ_BACKGROUND          4
#define M3G_OBJ_CAMERA              5
#define M3G_OBJ_COMPOSITINGMODE     6
#define M3G_OBJ_FOG                 7
#define M3G_OBJ_POLYGONMODE         8
#define M3G_OBJ_GROUP               9
#define M3G_OBJ_IMAGE2D             10
#define M3G_OBJ_TRIANGLESTRIPARRAY  11   /* IndexBuffer */
#define M3G_OBJ_LIGHT               12
#define M3G_OBJ_MATERIAL            13
#define M3G_OBJ_MESH                14
#define M3G_OBJ_MORPHINGMESH        15
#define M3G_OBJ_SKINNEDMESH         16
#define M3G_OBJ_TEXTURE2D           17
#define M3G_OBJ_SPRITE3D            18
#define M3G_OBJ_KEYFRAMESEQUENCE    19
#define M3G_OBJ_VERTEXARRAY         20
#define M3G_OBJ_VERTEXBUFFER        21
#define M3G_OBJ_WORLD               22
#define M3G_OBJ_EXTERNALREF         255

#include "miniz.h"  /* Use miniz for zlib decompression - single file implementation */

/* Helper: Parse M3G file and extract objects */
/* M3G reference system: ref 0 = null, ref N = objects[N] (direct index)
 * Objects are stored at their file position (0=header, 1=first object, etc.)
 * This preserves reference indices for correct linking.
 */
typedef struct {
    JavaObject** objects;      /* Objects stored at file position index */
    int object_count;          /* Number of objects stored */
    int object_capacity;       /* Capacity of objects array */
    JavaObject* world;
    JavaObject* camera;
    JavaObject* background;
    JavaObject* light;
    int external_links;        /* Header flag */
    int current_section;       /* Current section number */
    int header_seen;           /* Track if header was already parsed */
    int objects_start_index;   /* Starting index for objects (after header) */
    /* Temporary storage for parsing - used during linking */
    JavaObject* mesh;
    JavaObject* vertex_buffer;
    JavaObject* index_buffer;
    JavaObject* appearance;
} M3GParseContext;

/* Decompress zlib data - FIX: Use two-pass approach to determine exact size */
static uint8_t* m3g_decompress(const uint8_t* data, size_t data_size, size_t* out_size) {
    if (!data || data_size < 2) return NULL;

    /* Check for zlib header (0x78 0x9C or similar) */
    if (data[0] != 0x78 || (data[1] != 0x9C && data[1] != 0xDA && data[1] != 0x01)) {
        /* Not compressed, return copy */
        GFX_DEBUG("M3G: Data not zlib compressed, returning copy");
        uint8_t* result = malloc(data_size);
        if (result) {
            memcpy(result, data, data_size);
            *out_size = data_size;
        }
        return result;
    }

    GFX_DEBUG("M3G: Decompressing zlib data, input size=%zu", data_size);

    z_stream strm;
    memset(&strm, 0, sizeof(strm));

    if (inflateInit(&strm) != Z_OK) {
        GFX_DEBUG("M3G: inflateInit failed");
        return NULL;
    }

    strm.next_in = (Bytef*)data;
    strm.avail_in = data_size;

    /* FIX: Two-pass decompression - first determine exact size needed */
    /* Use a small stack buffer for the first pass to estimate size */
    #define PROBE_BUF_SIZE 4096
    uint8_t probe_buf[PROBE_BUF_SIZE];
    size_t total_out_estimate = 0;
    
    /* First pass: decompress in chunks to count total output size */
    int ret = Z_OK;
    while (ret == Z_OK) {
        strm.next_out = probe_buf;
        strm.avail_out = PROBE_BUF_SIZE;
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_OK || ret == Z_STREAM_END) {
            total_out_estimate += (PROBE_BUF_SIZE - strm.avail_out);
        }
    }
    
    if (ret != Z_STREAM_END) {
        GFX_DEBUG("M3G: First pass inflate failed with code %d", ret);
        inflateEnd(&strm);
        return NULL;
    }
    
    *out_size = strm.total_out;
    inflateEnd(&strm);
    
    /* Sanity check: reject unreasonably large decompressed output.
     * M3G files for J2ME are typically < 2MB; anything > 64MB is certainly corrupt. */
    #define M3G_MAX_DECOMPRESSED_SIZE (64 * 1024 * 1024)
    if (*out_size == 0 || *out_size > M3G_MAX_DECOMPRESSED_SIZE) {
        GFX_DEBUG("M3G: Decompressed size %zu is out of range (max=%d)", *out_size, M3G_MAX_DECOMPRESSED_SIZE);
        return NULL;
    }
    
    GFX_DEBUG("M3G: First pass complete, exact output size=%zu bytes", *out_size);

    /* Second pass: allocate exact size and decompress */
    uint8_t* result = malloc(*out_size);
    if (!result) {
        GFX_DEBUG("M3G: Failed to allocate exact buffer of %zu bytes", *out_size);
        return NULL;
    }

    /* Reinitialize for second pass */
    memset(&strm, 0, sizeof(strm));
    if (inflateInit(&strm) != Z_OK) {
        GFX_DEBUG("M3G: inflateInit failed on second pass");
        free(result);
        return NULL;
    }

    strm.next_in = (Bytef*)data;
    strm.avail_in = data_size;
    strm.next_out = result;
    strm.avail_out = *out_size;

    ret = inflate(&strm, Z_FINISH);
    if (ret != Z_STREAM_END && ret != Z_OK) {
        GFX_DEBUG("M3G: Second pass inflate failed with code %d", ret);
        inflateEnd(&strm);
        free(result);
        return NULL;
    }

    inflateEnd(&strm);
    GFX_DEBUG("M3G: Successfully decompressed %zu bytes to %zu bytes", data_size, *out_size);
    
    return result;
}

/* Align position to 4-byte boundary per M3G spec (JSR-184 §6.2.1):
 * "All data fields that are 2 or more bytes in size are aligned to
 *  4-byte boundaries. If the current byte position is not a multiple
 *  of 4 at the start of reading a field of 2 or more bytes, 1-3
 *  zero padding bytes are inserted."
 * NOTE: The object type (1 byte) + length (4 bytes) at the start of
 * each M3G object are NOT aligned (packed as 5 bytes total). Alignment
 * only applies WITHIN each object's data, after those 5 header bytes.
 */
#define M3G_ALIGN4(pos) do { *(pos) = (*(pos) + 3) & ~(size_t)3; } while(0)

/* Read a 32-bit little-endian value (NO auto-align — caller must align if needed) */
static uint32_t m3g_read_u32(const uint8_t* data, size_t* pos, size_t max) {
    if (*pos + 4 > max) return 0;
    uint32_t val = data[*pos] | (data[*pos+1] << 8) | (data[*pos+2] << 16) | (data[*pos+3] << 24);
    *pos += 4;
    return val;
}

/* Read a 16-bit little-endian value (NO auto-align — caller must align if needed) */
static uint16_t m3g_read_u16(const uint8_t* data, size_t* pos, size_t max) {
    if (*pos + 2 > max) return 0;
    uint16_t val = data[*pos] | (data[*pos+1] << 8);
    *pos += 2;
    return val;
}

/* Read a byte (NO alignment — u8 fields are 1 byte) */
static uint8_t m3g_read_u8(const uint8_t* data, size_t* pos, size_t max) {
    if (*pos >= max) return 0;
    return data[(*pos)++];
}

/* Read a float (32-bit IEEE 754, NO auto-align — caller must align if needed) */
static float m3g_read_float(const uint8_t* data, size_t* pos, size_t max) {
    union { uint32_t i; float f; } u;
    u.i = m3g_read_u32(data, pos, max);
    return u.f;
}

/* ============================================================================
 * M3G Object Data Reading Helpers (per JSR-184 spec)
 * ============================================================================ */

/* Skip Object3D base data: userID already read, then animationTrackCount + refs, userObjectCount + data */
static void m3g_skip_object3d_data(const uint8_t* data, size_t data_size, size_t* pos) {
    /* Animation tracks count and references */
    uint32_t track_count = m3g_read_u32(data, pos, data_size);
    for (uint32_t i = 0; i < track_count; i++) {
        m3g_read_u32(data, pos, data_size);  /* Skip track reference */
    }

    /* User object count and data */
    uint32_t user_object_count = m3g_read_u32(data, pos, data_size);
    for (uint32_t i = 0; i < user_object_count; i++) {
        uint32_t key = m3g_read_u32(data, pos, data_size);
        (void)key;
        uint32_t value_len = m3g_read_u32(data, pos, data_size);
        *pos += value_len;  /* Skip value bytes */
    }
}

/* Skip Transformable data after Object3D data */
static void m3g_skip_transformable_data(const uint8_t* data, size_t data_size, size_t* pos) {
    /* hasComponentTransform (boolean) */
    uint8_t has_component = m3g_read_u8(data, pos, data_size);
    if (has_component) {
        M3G_ALIGN4(pos);
        /* translation: 3 floats */
        *pos += 12;
        /* scale: 3 floats */
        *pos += 12;
        /* orientation: angle + 3 axis floats */
        *pos += 16;
    }

    /* hasGeneralTransform (boolean) */
    uint8_t has_general = m3g_read_u8(data, pos, data_size);
    if (has_general) {
        /* 4x4 matrix = 16 floats = 64 bytes */
        *pos += 64;
    }
}

/* Skip Node data after Transformable data */
static void m3g_skip_node_data(const uint8_t* data, size_t data_size, size_t* pos) {
    /* renderingEnable (boolean) */
    m3g_read_u8(data, pos, data_size);
    /* pickingEnable (boolean) */
    m3g_read_u8(data, pos, data_size);
    /* alphaFactor (byte) */
    m3g_read_u8(data, pos, data_size);
    /* scope (uint32) */
    m3g_read_u32(data, pos, data_size);

    /* hasAlignment (boolean) */
    uint8_t has_alignment = m3g_read_u8(data, pos, data_size);
    if (has_alignment) {
        /* alignmentTarget for Z, Y */
        m3g_read_u8(data, pos, data_size);
        m3g_read_u8(data, pos, data_size);
        /* reference indices */
        m3g_read_u32(data, pos, data_size);
        m3g_read_u32(data, pos, data_size);
    }
}

/* Skip Group data after Node data */
static void m3g_skip_group_data(const uint8_t* data, size_t data_size, size_t* pos) {
    /* child count */
    uint32_t child_count = m3g_read_u32(data, pos, data_size);
    for (uint32_t i = 0; i < child_count; i++) {
        m3g_read_u32(data, pos, data_size);  /* Skip child reference */
    }
}

/* Read and store Object3D data, return userID */
static uint32_t m3g_read_object3d_data(JavaObject* obj, const uint8_t* data, size_t data_size, size_t* pos, M3GParseContext* ctx) {
    /* userID already read by caller */
    uint32_t user_id = 0;  /* Caller should pass this */

    /* Animation tracks count and references - store count for later */
    uint32_t track_count = m3g_read_u32(data, pos, data_size);
    fprintf(stderr, "[M3G-O3D] obj=%p pos=%zu track_count=%u\n", (void*)obj, *pos - 4, track_count);
    (void)track_count;
    for (uint32_t i = 0; i < track_count; i++) {
        m3g_read_u32(data, pos, data_size);  /* Skip track reference for now */
    }

    /* User object count and data */
    uint32_t user_object_count = m3g_read_u32(data, pos, data_size);
    fprintf(stderr, "[M3G-O3D] obj=%p pos=%zu user_obj_count=%u\n", (void*)obj, *pos - 4, user_object_count);
    for (uint32_t i = 0; i < user_object_count; i++) {
        uint32_t key = m3g_read_u32(data, pos, data_size);
        (void)key;
        uint32_t value_len = m3g_read_u32(data, pos, data_size);
        *pos += value_len;
    }

    return user_id;
}

/* Read Transformable data after Object3D data */
static void m3g_read_transformable_data(JavaObject* obj, const uint8_t* data, size_t data_size, size_t* pos, JVM* jvm) {
    /* hasComponentTransform (boolean) */
    uint8_t has_component = m3g_read_u8(data, pos, data_size);
    if (has_component) {
        /* translation: 3 floats */
        float tx = m3g_read_float(data, pos, data_size);
        float ty = m3g_read_float(data, pos, data_size);
        float tz = m3g_read_float(data, pos, data_size);
        /* scale: 3 floats */
        float sx = m3g_read_float(data, pos, data_size);
        float sy = m3g_read_float(data, pos, data_size);
        float sz = m3g_read_float(data, pos, data_size);
        /* orientation: angle + 3 axis floats */
        float angle = m3g_read_float(data, pos, data_size);
        float ax = m3g_read_float(data, pos, data_size);
        float ay = m3g_read_float(data, pos, data_size);
        float az = m3g_read_float(data, pos, data_size);

        /* Store transform data */
        m3g_set_float_field(obj, "translationX", tx);
        m3g_set_float_field(obj, "translationY", ty);
        m3g_set_float_field(obj, "translationZ", tz);
        m3g_set_float_field(obj, "scaleX", sx);
        m3g_set_float_field(obj, "scaleY", sy);
        m3g_set_float_field(obj, "scaleZ", sz);
        m3g_set_float_field(obj, "orientationAngle", angle);
        m3g_set_float_field(obj, "orientationX", ax);
        m3g_set_float_field(obj, "orientationY", ay);
        m3g_set_float_field(obj, "orientationZ", az);

        GFX_DEBUG("M3G: Transform tx=%.2f ty=%.2f tz=%.2f sx=%.2f sy=%.2f sz=%.2f",
                 tx, ty, tz, sx, sy, sz);
    }

    /* hasGeneralTransform (boolean) */
    uint8_t has_general = m3g_read_u8(data, pos, data_size);
    if (has_general) {
        /* 4x4 matrix = 16 floats = 64 bytes */
        /* Store in transform matrix field */
        JavaArray* matrix = jvm_new_array(jvm, T_FLOAT, 16, NULL);
        if (matrix) {
            float* m = (float*)array_data(matrix);
            for (int i = 0; i < 16; i++) {
                m[i] = m3g_read_float(data, pos, data_size);
            }
            m3g_set_ref_field(obj, "transform", (JavaObject*)matrix);
        } else {
            *pos += 64;  /* Skip if can't allocate */
        }
    }
}

/* Read Node data after Transformable data */
static void m3g_read_node_data(JavaObject* obj, const uint8_t* data, size_t data_size, size_t* pos) {
    /* renderingEnable (boolean) */
    uint8_t rendering_enable = m3g_read_u8(data, pos, data_size);
    m3g_set_int_field(obj, "renderingEnable", rendering_enable);

    /* pickingEnable (boolean) */
    uint8_t picking_enable = m3g_read_u8(data, pos, data_size);
    m3g_set_int_field(obj, "pickingEnable", picking_enable);

    /* alphaFactor (byte) - stored as float 0.0-1.0 */
    uint8_t alpha_factor = m3g_read_u8(data, pos, data_size);
    m3g_set_float_field(obj, "alphaFactor", alpha_factor / 255.0f);

    /* scope (uint32) */
    uint32_t scope = m3g_read_u32(data, pos, data_size);
    m3g_set_int_field(obj, "scope", scope);

    /* hasAlignment (boolean) */
    uint8_t has_alignment = m3g_read_u8(data, pos, data_size);
    if (has_alignment) {
        /* Skip alignment for now */
        m3g_read_u8(data, pos, data_size);  /* zTarget */
        m3g_read_u8(data, pos, data_size);  /* yTarget */
        m3g_read_u32(data, pos, data_size); /* zReference */
        m3g_read_u32(data, pos, data_size); /* yReference */
    }

    GFX_DEBUG("M3G: Node rendering=%d picking=%d alpha=%.2f scope=%u",
             rendering_enable, picking_enable, alpha_factor / 255.0f, scope);
}

/* Read Group data after Node data, store child refs for later linking */
static void m3g_read_group_data(JavaObject* obj, const uint8_t* data, size_t data_size, size_t* pos, JVM* jvm) {
    /* child count */
    uint32_t child_count = m3g_read_u32(data, pos, data_size);

    /* Store child count for linking */
    m3g_set_int_field(obj, "childCount", child_count);

    /* Create children array to store refs temporarily */
    if (child_count > 0) {
        JavaArray* child_refs = jvm_new_array(jvm, T_INT, child_count, NULL);
        if (child_refs) {
            jint* refs = (jint*)array_data(child_refs);
            for (uint32_t i = 0; i < child_count; i++) {
                refs[i] = (jint)m3g_read_u32(data, pos, data_size);
            }
            /* Store _childRefs — try by name first, then use direct slot */
            int slot = m3g_find_field_slot(obj, "_childRefs");
            if (slot >= 0) {
                obj->fields[slot].ref = (JavaObject*)child_refs;
                fprintf(stderr, "[M3G] Group %p: stored _childRefs[%d] at slot %d\n",
                        (void*)obj, child_count, slot);
            } else {
                /* Fallback: try slot 19 (_childRefs is 3rd Group field: children=17, childCount=18, _childRefs=19) */
                int max_slots = (obj->header.clazz->instance_size - (int)sizeof(ObjectHeader)) / (int)sizeof(JavaValue);
                fprintf(stderr, "[M3G] Group %p: _childRefs NOT FOUND by name, max_slots=%d, trying slot 19\n",
                        (void*)obj, max_slots);
                if (max_slots > 19) {
                    obj->fields[19].ref = (JavaObject*)child_refs;
                    fprintf(stderr, "[M3G] Group %p: stored _childRefs at fallback slot 19\n", (void*)obj);
                } else {
                    /* Last resort: store on children slot and use childCount to distinguish */
                    fprintf(stderr, "[M3G] Group %p: max_slots=%d too small for _childRefs!\n", (void*)obj, max_slots);
                }
            }
        }
    }

    fprintf(stderr, "[M3G] Group %p childCount=%u\n", (void*)obj, child_count);
}

/* Parse a single M3G object (obj_type already read, read data only) */
/* Returns object or NULL (for unimplemented/failed parsing) */
/* Objects are stored at their file position index; reference N = objects[N] */
static JavaObject* m3g_parse_object(JVM* jvm, M3GParseContext* ctx,
                                    const uint8_t* data, size_t data_size,
                                    size_t* pos, uint8_t obj_type, uint32_t obj_length) {
    if (*pos >= data_size) return NULL;

    (void)obj_length;  /* Used by caller to skip */
    GFX_DEBUG("M3G: Parsing object type=%d at pos=%zu", obj_type, *pos);

    /* Read userID first - ALL M3G objects start with this (4 bytes) */
    uint32_t user_id = m3g_read_u32(data, pos, data_size);
    GFX_DEBUG("M3G: Object type=%d userID=%u", obj_type, user_id);

    switch (obj_type) {
        case M3G_OBJ_HEADER: {
            /* Header object - version info (already read in section parsing) */
            /* Skip rest of header - it was already processed */
            GFX_DEBUG("M3G: Header object (internal, not stored)");
            return NULL;  /* Header is NOT stored in objects array */
        }

        case M3G_OBJ_WORLD: {
            JavaClass* world_class = jvm_load_class(jvm, "javax/microedition/m3g/World");
            if (!world_class) return NULL;

            JavaObject* world = jvm_new_object(jvm, world_class);
            if (world) {
                m3g_set_userID(world, user_id);

                /* Read full hierarchy: Object3D -> Transformable -> Node -> Group -> World */
                m3g_read_object3d_data(world, data, data_size, pos, ctx);
                m3g_read_transformable_data(world, data, data_size, pos, jvm);
                m3g_read_node_data(world, data, data_size, pos);
                m3g_read_group_data(world, data, data_size, pos, jvm);

                /* World-specific: activeCamera and background references */
                uint32_t camera_ref = m3g_read_u32(data, pos, data_size);
                uint32_t bg_ref = m3g_read_u32(data, pos, data_size);
                m3g_set_int_field(world, "activeCameraRef", camera_ref);
                m3g_set_int_field(world, "backgroundRef", bg_ref);

                ctx->world = world;
                GFX_DEBUG("M3G: Created World userID=%u cameraRef=%u bgRef=%u", user_id, camera_ref, bg_ref);
            }
            return world;
        }

        case M3G_OBJ_CAMERA: {
            JavaClass* camera_class = jvm_load_class(jvm, "javax/microedition/m3g/Camera");
            if (!camera_class) return NULL;

            JavaObject* camera = jvm_new_object(jvm, camera_class);
            if (camera) {
                m3g_set_userID(camera, user_id);

                /* Read full hierarchy: Object3D -> Transformable -> Node -> Camera */
                m3g_read_object3d_data(camera, data, data_size, pos, ctx);
                m3g_read_transformable_data(camera, data, data_size, pos, jvm);
                m3g_read_node_data(camera, data, data_size, pos);

                /* Camera-specific: projection */
                uint8_t proj_type = m3g_read_u8(data, pos, data_size);
                m3g_set_int_field(camera, "projectionType", proj_type);

                if (proj_type == 48) { /* GENERIC */
                    /* 4x4 matrix = 16 floats - stored in row-major in M3G file */
                    float rowmajor[16];
                    for (int i = 0; i < 16; i++) {
                        rowmajor[i] = m3g_read_float(data, pos, data_size);
                    }
                    /* Store as a float[] field in row-major (Java convention) */
                    JavaArray* gen_matrix = jvm_new_array(jvm, T_FLOAT, 16, NULL);
                    if (gen_matrix) {
                        float* dst = (float*)array_data(gen_matrix);
                        memcpy(dst, rowmajor, 16 * sizeof(float));
                        m3g_set_ref_field(camera, "genericMatrix", (JavaObject*)gen_matrix);
                    }
                } else if (proj_type == 50) { /* PERSPECTIVE */
                    float fov = m3g_read_float(data, pos, data_size);
                    float aspect = m3g_read_float(data, pos, data_size);
                    float near = m3g_read_float(data, pos, data_size);
                    float far = m3g_read_float(data, pos, data_size);
                    m3g_set_float_field(camera, "fov", fov);
                    m3g_set_float_field(camera, "aspect", aspect);
                    m3g_set_float_field(camera, "near", near);
                    m3g_set_float_field(camera, "far", far);
                } else if (proj_type == 49) { /* PARALLEL */
                    float fovy = m3g_read_float(data, pos, data_size);
                    float aspect = m3g_read_float(data, pos, data_size);
                    float near = m3g_read_float(data, pos, data_size);
                    float far = m3g_read_float(data, pos, data_size);
                    m3g_set_float_field(camera, "fov", fovy);
                    m3g_set_float_field(camera, "aspect", aspect);
                    m3g_set_float_field(camera, "near", near);
                    m3g_set_float_field(camera, "far", far);
                }

                ctx->camera = camera;
                GFX_DEBUG("M3G: Created Camera userID=%u proj=%d", user_id, proj_type);
            }
            return camera;
        }

        case M3G_OBJ_LIGHT: {
            JavaClass* light_class = jvm_load_class(jvm, "javax/microedition/m3g/Light");
            if (!light_class) return NULL;

            JavaObject* light = jvm_new_object(jvm, light_class);
            if (light) {
                m3g_set_userID(light, user_id);

                /* Read full hierarchy: Object3D -> Transformable -> Node -> Light */
                m3g_read_object3d_data(light, data, data_size, pos, ctx);
                m3g_read_transformable_data(light, data, data_size, pos, jvm);
                m3g_read_node_data(light, data, data_size, pos);

                /* Light-specific: attenuation, color, mode, intensity, spot */
                float att0 = m3g_read_float(data, pos, data_size);
                float att1 = m3g_read_float(data, pos, data_size);
                float att2 = m3g_read_float(data, pos, data_size);
                uint32_t color = m3g_read_u32(data, pos, data_size);
                uint8_t mode = m3g_read_u8(data, pos, data_size);
                float intensity = m3g_read_float(data, pos, data_size);
                float spot_angle = m3g_read_float(data, pos, data_size);
                float spot_exp = m3g_read_float(data, pos, data_size);

                m3g_set_int_field(light, "mode", mode);
                m3g_set_int_field(light, "color", color);
                m3g_set_float_field(light, "intensity", intensity);
                m3g_set_float_field(light, "constantAttenuation", att0);
                m3g_set_float_field(light, "linearAttenuation", att1);
                m3g_set_float_field(light, "quadraticAttenuation", att2);
                m3g_set_float_field(light, "spotAngle", spot_angle);
                m3g_set_float_field(light, "spotExponent", spot_exp);

                ctx->light = light;
                GFX_DEBUG("M3G: Created Light userID=%u mode=%d color=0x%08X", user_id, mode, color);
            }
            return light;
        }

        case M3G_OBJ_BACKGROUND: {
            JavaClass* bg_class = jvm_load_class(jvm, "javax/microedition/m3g/Background");
            if (!bg_class) return NULL;

            JavaObject* bg = jvm_new_object(jvm, bg_class);
            if (bg) {
                m3g_set_userID(bg, user_id);

                /* Object3D data */
                m3g_read_object3d_data(bg, data, data_size, pos, ctx);

                /* Background-specific */
                uint32_t bg_color = m3g_read_u32(data, pos, data_size);
                uint32_t image_ref = m3g_read_u32(data, pos, data_size);
                uint8_t mode_x = m3g_read_u8(data, pos, data_size);
                uint8_t mode_y = m3g_read_u8(data, pos, data_size);
                int32_t crop_x = m3g_read_u32(data, pos, data_size);  /* signed */
                int32_t crop_y = m3g_read_u32(data, pos, data_size);
                int32_t crop_w = m3g_read_u32(data, pos, data_size);
                int32_t crop_h = m3g_read_u32(data, pos, data_size);
                uint8_t depth_clear = m3g_read_u8(data, pos, data_size);
                uint8_t color_clear = m3g_read_u8(data, pos, data_size);

                m3g_set_int_field(bg, "clearColor", bg_color);
                m3g_set_int_field(bg, "imageRef", image_ref);
                m3g_set_int_field(bg, "depthClearEnable", depth_clear);
                m3g_set_int_field(bg, "colorClearEnable", color_clear);
                m3g_set_int_field(bg, "imageModeX", mode_x);
                m3g_set_int_field(bg, "imageModeY", mode_y);
                m3g_set_int_field(bg, "cropX", crop_x);
                m3g_set_int_field(bg, "cropY", crop_y);
                m3g_set_int_field(bg, "cropWidth", crop_w);
                m3g_set_int_field(bg, "cropHeight", crop_h);

                ctx->background = bg;
                GFX_DEBUG("M3G: Created Background userID=%u color=0x%08X", user_id, bg_color);
            }
            return bg;
        }

        case M3G_OBJ_MESH: {
            JavaClass* mesh_class = jvm_load_class(jvm, "javax/microedition/m3g/Mesh");
            if (!mesh_class) return NULL;

            JavaObject* mesh = jvm_new_object(jvm, mesh_class);
            if (mesh) {
                m3g_set_userID(mesh, user_id);

                /* Read full hierarchy: Object3D -> Transformable -> Node -> Mesh */
                m3g_read_object3d_data(mesh, data, data_size, pos, ctx);
                m3g_read_transformable_data(mesh, data, data_size, pos, jvm);
                m3g_read_node_data(mesh, data, data_size, pos);

                /* Mesh-specific: vertexBuffer ref, then submeshCount, indexBuffer/appearance refs */
                uint32_t vb_ref = m3g_read_u32(data, pos, data_size);
                uint32_t submesh_count = m3g_read_u32(data, pos, data_size);

                m3g_set_int_field(mesh, "vertexBufferRef", vb_ref);
                m3g_set_int_field(mesh, "submeshCount", submesh_count);

                /* Read index buffer and appearance refs for each submesh */
                /* Store first submesh refs for linking */
                uint32_t ib_ref = 0, app_ref = 0;
                for (uint32_t i = 0; i < submesh_count; i++) {
                    uint32_t this_ib = m3g_read_u32(data, pos, data_size);
                    uint32_t this_app = m3g_read_u32(data, pos, data_size);
                    if (i == 0) {
                        ib_ref = this_ib;
                        app_ref = this_app;
                    }
                }
                m3g_set_int_field(mesh, "indexBufferRef", ib_ref);
                m3g_set_int_field(mesh, "appearanceRef", app_ref);

                ctx->mesh = mesh;
                GFX_DEBUG("M3G: Created Mesh userID=%u vbRef=%u ibRef=%u appRef=%u submeshCount=%u",
                         user_id, vb_ref, ib_ref, app_ref, submesh_count);
            }
            return mesh;
        }

        case M3G_OBJ_VERTEXBUFFER: {
            JavaClass* vb_class = jvm_load_class(jvm, "javax/microedition/m3g/VertexBuffer");
            if (!vb_class) return NULL;

            JavaObject* vb = jvm_new_object(jvm, vb_class);
            if (vb) {
                m3g_set_userID(vb, user_id);

                /* Object3D data */
                m3g_read_object3d_data(vb, data, data_size, pos, ctx);

                /* VertexBuffer-specific */
                uint32_t default_color = m3g_read_u32(data, pos, data_size);
                m3g_set_int_field(vb, "defaultColor", default_color);

                /* Positions: ref + scale + bias */
                uint32_t pos_ref = m3g_read_u32(data, pos, data_size);
                float scale = m3g_read_float(data, pos, data_size);
                float bias_x = m3g_read_float(data, pos, data_size);
                float bias_y = m3g_read_float(data, pos, data_size);
                float bias_z = m3g_read_float(data, pos, data_size);

                m3g_set_int_field(vb, "positionsRef", pos_ref);
                m3g_set_float_field(vb, "positionScale", scale);
                m3g_set_float_field(vb, "biasX", bias_x);
                m3g_set_float_field(vb, "biasY", bias_y);
                m3g_set_float_field(vb, "biasZ", bias_z);

                /* Normals ref */
                uint32_t norm_ref = m3g_read_u32(data, pos, data_size);
                m3g_set_int_field(vb, "normalsRef", norm_ref);

                /* Colors ref */
                uint32_t color_ref = m3g_read_u32(data, pos, data_size);
                m3g_set_int_field(vb, "colorsRef", color_ref);

                /* TexCoord arrays */
                uint32_t tex_count = m3g_read_u32(data, pos, data_size);
                uint32_t tex_coord_ref = 0;
                float tex_coord_scale = 1.0f;
                for (uint32_t i = 0; i < tex_count; i++) {
                    uint32_t this_tex_ref = m3g_read_u32(data, pos, data_size);  /* texCoord ref */
                    float this_tex_scale = m3g_read_float(data, pos, data_size);  /* scale */
                    m3g_read_float(data, pos, data_size);  /* bias x */
                    m3g_read_float(data, pos, data_size);  /* bias y */
                    m3g_read_float(data, pos, data_size);  /* bias z (usually 0) */
                    if (i == 0) {
                        tex_coord_ref = this_tex_ref;
                        tex_coord_scale = this_tex_scale;
                    }
                }
                m3g_set_int_field(vb, "texCoordsRef", tex_coord_ref);
                m3g_set_float_field(vb, "texCoordScale", tex_coord_scale);

                ctx->vertex_buffer = vb;
                GFX_DEBUG("M3G: Created VertexBuffer userID=%u posRef=%u normRef=%u colorRef=%u texRef=%u",
                         user_id, pos_ref, norm_ref, color_ref, tex_coord_ref);
            }
            return vb;
        }

        case M3G_OBJ_VERTEXARRAY: {
            JavaClass* va_class = jvm_load_class(jvm, "javax/microedition/m3g/VertexArray");
            if (!va_class) return NULL;

            JavaObject* va = jvm_new_object(jvm, va_class);
            if (va) {
                m3g_set_userID(va, user_id);

                /* Object3D data */
                m3g_read_object3d_data(va, data, data_size, pos, ctx);

                /* VertexArray-specific */
                uint8_t component_count = m3g_read_u8(data, pos, data_size);
                uint8_t component_size = m3g_read_u8(data, pos, data_size);
                uint8_t encoding = m3g_read_u8(data, pos, data_size);
                uint16_t vertex_count = m3g_read_u16(data, pos, data_size);

                /* Sanity checks */
                if (component_count > 4) component_count = 4;
                if (component_size > 2) component_size = 2;

                int data_size_total = (int)vertex_count * component_count * component_size;
                if (data_size_total <= 0 || data_size_total > M3G_MAX_VERTEX_COUNT * 4 * 2) {
                    GFX_DEBUG("M3G: VertexArray data_size invalid: %d", data_size_total);
                    return va;
                }

                m3g_set_int_field(va, "componentCount", component_count);
                m3g_set_int_field(va, "componentSize", component_size);
                m3g_set_int_field(va, "vertexCount", vertex_count);

                /* Create data array */
                JavaArray* arr = jvm_new_array(jvm, component_size == 1 ? T_BYTE : T_SHORT,
                                                data_size_total, NULL);
                if (arr) {
                    m3g_set_ref_field(va, "data", (JavaObject*)arr);
                    void* dst = array_data(arr);

                    if (encoding == 0) { /* Raw */
                        memcpy(dst, &data[*pos], data_size_total);
                    } else { /* Delta encoding: each value = previous + diff */
                        if (component_size == 1) {
                            uint8_t last = 0;
                            uint8_t* d = (uint8_t*)dst;
                            for (int i = 0; i < data_size_total; i++) {
                                last += data[*pos + i];
                                d[i] = last;
                            }
                        } else {
                            int16_t last = 0;
                            int16_t* d = (int16_t*)dst;
                            for (int i = 0; i < data_size_total; i += 2) {
                                int16_t diff = (int16_t)(data[*pos + i] | (data[*pos + i + 1] << 8));
                                last += diff;
                                d[i / 2] = last;
                            }
                        }
                    }
                }
                *pos += data_size_total;

                GFX_DEBUG("M3G: Created VertexArray userID=%u %d vertices, %d components",
                         user_id, vertex_count, component_count);
            }
            return va;
        }

        case M3G_OBJ_TRIANGLESTRIPARRAY: {
            JavaClass* ib_class = jvm_load_class(jvm, "javax/microedition/m3g/TriangleStripArray");
            if (!ib_class) return NULL;

            JavaObject* ib = jvm_new_object(jvm, ib_class);
            if (ib) {
                m3g_set_userID(ib, user_id);

                /* Object3D data */
                m3g_read_object3d_data(ib, data, data_size, pos, ctx);

                /* TriangleStripArray-specific: encoding, then indices */
                uint8_t encoding = m3g_read_u8(data, pos, data_size);

                uint32_t first_index = 0;
                uint32_t strip_count = 0;
                int32_t* explicit_indices = NULL;
                uint32_t explicit_count = 0;

                switch (encoding) {
                    case 0:  /* Explicit first index */
                        first_index = m3g_read_u32(data, pos, data_size);
                        break;
                    case 1:
                        first_index = m3g_read_u8(data, pos, data_size);
                        break;
                    case 2:
                        first_index = m3g_read_u16(data, pos, data_size);
                        break;
                    case 128: /* Explicit array indices */
                    case 129:
                    case 130:
                        {
                            explicit_count = m3g_read_u32(data, pos, data_size);
                            if (explicit_count > 0 && explicit_count < M3G_MAX_INDEX_COUNT) {
                                explicit_indices = (int32_t*)malloc(explicit_count * sizeof(int32_t));
                                if (explicit_indices) {
                                    for (uint32_t i = 0; i < explicit_count; i++) {
                                        if (encoding == 128)
                                            explicit_indices[i] = (int32_t)m3g_read_u32(data, pos, data_size);
                                        else if (encoding == 129)
                                            explicit_indices[i] = (int32_t)m3g_read_u8(data, pos, data_size);
                                        else
                                            explicit_indices[i] = (int32_t)m3g_read_u16(data, pos, data_size);
                                    }
                                } else {
                                    /* Skip indices if allocation failed */
                                    for (uint32_t i = 0; i < explicit_count; i++) {
                                        if (encoding == 128) m3g_read_u32(data, pos, data_size);
                                        else if (encoding == 129) m3g_read_u8(data, pos, data_size);
                                        else m3g_read_u16(data, pos, data_size);
                                    }
                                }
                            } else {
                                /* Skip invalid index count */
                                for (uint32_t i = 0; i < explicit_count && i < M3G_MAX_INDEX_COUNT; i++) {
                                    if (encoding == 128) m3g_read_u32(data, pos, data_size);
                                    else if (encoding == 129) m3g_read_u8(data, pos, data_size);
                                    else m3g_read_u16(data, pos, data_size);
                                }
                                explicit_count = 0;
                            }
                        }
                        break;
                }

                /* Read strip lengths */
                strip_count = m3g_read_u32(data, pos, data_size);
                uint32_t* strip_lengths = NULL;
                if (strip_count > 0 && strip_count < 1024) {
                    strip_lengths = (uint32_t*)malloc(strip_count * sizeof(uint32_t));
                    for (uint32_t i = 0; i < strip_count; i++) {
                        strip_lengths[i] = m3g_read_u32(data, pos, data_size);
                    }
                } else {
                    for (uint32_t i = 0; i < strip_count && i < 1024; i++) {
                        m3g_read_u32(data, pos, data_size);
                    }
                    strip_count = 0;
                }

                /* Generate expanded triangle indices from strip data */
                int total_tri_indices = 0;
                for (uint32_t s = 0; s < strip_count; s++) {
                    int slen = (int)strip_lengths[s];
                    if (slen < 3) continue;
                    total_tri_indices += (slen - 2) * 3;
                }

                if (total_tri_indices > 0 && total_tri_indices <= M3G_MAX_INDEX_COUNT) {
                    JavaArray* indices = jvm_new_array(jvm, T_SHORT, total_tri_indices, NULL);
                    if (indices) {
                        int16_t* dst = (int16_t*)array_data(indices);
                        int idx = 0;
                        int vertex_offset = 0;

                        for (uint32_t s = 0; s < strip_count && idx + 2 < total_tri_indices; s++) {
                            int slen = (int)strip_lengths[s];
                            if (slen < 3) {
                                vertex_offset += slen;
                                continue;
                            }

                            for (int t = 0; t < slen - 2 && idx + 2 < total_tri_indices; t++) {
                                int i0, i1, i2;
                                if (explicit_indices) {
                                    /* Use explicit vertex indices from the M3G binary */
                                    i0 = explicit_indices[vertex_offset + t];
                                    i1 = explicit_indices[vertex_offset + t + 1];
                                    i2 = explicit_indices[vertex_offset + t + 2];
                                } else {
                                    /* Implicit indices: first_index + vertex_offset + t */
                                    i0 = (int)first_index + vertex_offset + t;
                                    i1 = (int)first_index + vertex_offset + t + 1;
                                    i2 = (int)first_index + vertex_offset + t + 2;
                                }

                                if (t % 2 == 0) {
                                    dst[idx++] = (int16_t)i0;
                                    dst[idx++] = (int16_t)i1;
                                    dst[idx++] = (int16_t)i2;
                                } else {
                                    /* Swap last two for correct winding order */
                                    dst[idx++] = (int16_t)i0;
                                    dst[idx++] = (int16_t)i2;
                                    dst[idx++] = (int16_t)i1;
                                }
                            }
                            vertex_offset += slen;
                        }

                        m3g_set_ref_field(ib, "indices", (JavaObject*)indices);
                        m3g_set_int_field(ib, "indexCount", idx);
                        fprintf(stderr, "[M3G] TriangleStripArray: %d strips, %d tri-indices, encoding=%d\n",
                                strip_count, idx, encoding);
                    }
                } else {
                    fprintf(stderr, "[M3G] TriangleStripArray: invalid total_tri_indices=%d (strips=%u)\n",
                            total_tri_indices, strip_count);
                }

                if (explicit_indices) free(explicit_indices);
                if (strip_lengths) free(strip_lengths);

                ctx->index_buffer = ib;
            }
            return ib;
        }

        case M3G_OBJ_APPEARANCE: {
            JavaClass* app_class = jvm_load_class(jvm, "javax/microedition/m3g/Appearance");
            if (!app_class) return NULL;

            JavaObject* app = jvm_new_object(jvm, app_class);
            if (app) {
                m3g_set_userID(app, user_id);

                /* Object3D data */
                m3g_read_object3d_data(app, data, data_size, pos, ctx);

                /* Appearance-specific */
                uint8_t layer = m3g_read_u8(data, pos, data_size);
                m3g_set_int_field(app, "layer", layer);

                /* References: compositingMode, fog, polygonMode, material */
                uint32_t comp_mode_ref = m3g_read_u32(data, pos, data_size);  /* compositingMode */
                uint32_t fog_ref = m3g_read_u32(data, pos, data_size);       /* fog */
                uint32_t poly_mode_ref = m3g_read_u32(data, pos, data_size); /* polygonMode */
                uint32_t material_ref = m3g_read_u32(data, pos, data_size);  /* material */
                m3g_set_int_field(app, "materialRef", material_ref);
                m3g_set_int_field(app, "compositingModeRef", comp_mode_ref);
                m3g_set_int_field(app, "fogRef", fog_ref);
                m3g_set_int_field(app, "polygonModeRef", poly_mode_ref);

                /* Textures */
                uint32_t tex_count = m3g_read_u32(data, pos, data_size);
                uint32_t texture_ref = 0;
                for (uint32_t i = 0; i < tex_count; i++) {
                    uint32_t this_tex_ref = m3g_read_u32(data, pos, data_size);  /* texture ref */
                    if (i == 0) texture_ref = this_tex_ref;
                }
                m3g_set_int_field(app, "textureRef", texture_ref);

                ctx->appearance = app;
                GFX_DEBUG("M3G: Created Appearance userID=%u layer=%d texCount=%u",
                         user_id, layer, tex_count);
            }
            return app;
        }

        case M3G_OBJ_MATERIAL: {
            JavaClass* mat_class = jvm_load_class(jvm, "javax/microedition/m3g/Material");
            if (!mat_class) return NULL;

            JavaObject* mat = jvm_new_object(jvm, mat_class);
            if (mat) {
                m3g_set_userID(mat, user_id);

                /* Object3D data */
                m3g_read_object3d_data(mat, data, data_size, pos, ctx);

                /* Material-specific per JSR-184 spec */
                uint32_t ambient = m3g_read_u32(data, pos, data_size);
                uint32_t diffuse = m3g_read_u32(data, pos, data_size);
                uint32_t emissive = m3g_read_u32(data, pos, data_size);
                uint32_t specular = m3g_read_u32(data, pos, data_size);
                float shininess = m3g_read_float(data, pos, data_size);
                uint8_t vertex_color_tracking = m3g_read_u8(data, pos, data_size);

                m3g_set_int_field(mat, "ambient", ambient);
                m3g_set_int_field(mat, "diffuse", diffuse);
                m3g_set_int_field(mat, "emissive", emissive);
                m3g_set_int_field(mat, "specular", specular);
                m3g_set_float_field(mat, "shininess", shininess);
                m3g_set_int_field(mat, "vertexColorTracking", vertex_color_tracking);

                GFX_DEBUG("M3G: Created Material userID=%u shin=%.1f", user_id, shininess);
            }
            return mat;
        }

        case M3G_OBJ_COMPOSITINGMODE: {
            JavaClass* cm_class = jvm_load_class(jvm, "javax/microedition/m3g/CompositingMode");
            if (!cm_class) return NULL;

            JavaObject* cm = jvm_new_object(jvm, cm_class);
            if (cm) {
                m3g_set_userID(cm, user_id);

                /* Object3D data */
                m3g_read_object3d_data(cm, data, data_size, pos, ctx);

                /* CompositingMode-specific per JSR-184 M3G binary format.
                 * The M3G spec stores properties in this order:
                 *   DepthTest(u8), DepthWrite(u8), ColorWrite(u8),
                 *   Blending(u8), AlphaThreshold(f32), Dithering(u8)
                 * Previous parser read Blending first (wrong order). */
                uint8_t depth_test = m3g_read_u8(data, pos, data_size);
                uint8_t depth_write = m3g_read_u8(data, pos, data_size);
                uint8_t color_write = m3g_read_u8(data, pos, data_size);
                uint8_t blending = m3g_read_u8(data, pos, data_size);
                float alpha_threshold = m3g_read_float(data, pos, data_size);
                uint8_t dithering = m3g_read_u8(data, pos, data_size);

                m3g_set_int_field(cm, "blending", blending);
                m3g_set_float_field(cm, "alphaThreshold", alpha_threshold);
                m3g_set_int_field(cm, "depthTest", depth_test);
                m3g_set_int_field(cm, "depthWrite", depth_write);
                m3g_set_int_field(cm, "colorWrite", color_write);
                m3g_set_int_field(cm, "dithering", dithering);

                /* DIAG: Log class info and verify */
                fprintf(stderr, "[M3G-PARSE-CM] userID=%u class=%s inst_size=%d fields_count=%d\n",
                        user_id, cm_class->class_name, cm_class->instance_size, cm_class->fields_count);
                fprintf(stderr, "[M3G-PARSE-CM] raw: blend=%u(0x%02x) alphaT=%.3f depthT=%u depthW=%u\n",
                        blending, blending, alpha_threshold, depth_test, depth_write);
                jint verify_blend = m3g_get_int_field(cm, "blending", -999);
                jfloat verify_alpha = m3g_get_float_field(cm, "alphaThreshold", -999.0f);
                fprintf(stderr, "[M3G-PARSE-CM] verify: blend=%d alpha=%.3f (expected %d, %.3f)\n",
                        verify_blend, verify_alpha, blending, alpha_threshold);

                GFX_DEBUG("M3G: Created CompositingMode userID=%u blend=%d depth=%d/%d",
                         user_id, blending, depth_test, depth_write);
            }
            return cm;
        }

        case M3G_OBJ_FOG: {
            JavaClass* fog_class = jvm_load_class(jvm, "javax/microedition/m3g/Fog");
            if (!fog_class) return NULL;

            JavaObject* fog = jvm_new_object(jvm, fog_class);
            if (fog) {
                m3g_set_userID(fog, user_id);

                /* Object3D data */
                m3g_read_object3d_data(fog, data, data_size, pos, ctx);

                /* Fog-specific per JSR-184 spec */
                uint8_t mode = m3g_read_u8(data, pos, data_size);
                /* Color: 3 floats (R, G, B) */
                float cr = m3g_read_float(data, pos, data_size);
                float cg = m3g_read_float(data, pos, data_size);
                float cb = m3g_read_float(data, pos, data_size);
                float density = m3g_read_float(data, pos, data_size);
                float near_dist = m3g_read_float(data, pos, data_size);
                float far_dist = m3g_read_float(data, pos, data_size);

                m3g_set_int_field(fog, "mode", mode);
                m3g_set_float_field(fog, "density", density);
                m3g_set_float_field(fog, "nearDistance", near_dist);
                m3g_set_float_field(fog, "farDistance", far_dist);
                /* Store color as packed ARGB int for convenience */
                uint32_t packed_color = 0xFF000000 |
                    (((uint32_t)(cr * 255.0f) & 0xFF)) |
                    (((uint32_t)(cg * 255.0f) & 0xFF) << 8) |
                    (((uint32_t)(cb * 255.0f) & 0xFF) << 16);
                m3g_set_int_field(fog, "colorInt", packed_color);

                GFX_DEBUG("M3G: Created Fog userID=%u mode=%d density=%.3f near=%.1f far=%.1f",
                         user_id, mode, density, near_dist, far_dist);
            }
            return fog;
        }

        case M3G_OBJ_POLYGONMODE: {
            JavaClass* pm_class = jvm_load_class(jvm, "javax/microedition/m3g/PolygonMode");
            if (!pm_class) return NULL;

            JavaObject* pm = jvm_new_object(jvm, pm_class);
            if (pm) {
                m3g_set_userID(pm, user_id);

                /* Object3D data */
                size_t pos_before = *pos;
                m3g_read_object3d_data(pm, data, data_size, pos, ctx);

                /* PolygonMode-specific per JSR-184 spec */
                uint8_t culling = m3g_read_u8(data, pos, data_size);
                uint8_t shading = m3g_read_u8(data, pos, data_size);
                uint8_t two_sided_lighting = m3g_read_u8(data, pos, data_size);
                uint8_t local_camera_lighting = m3g_read_u8(data, pos, data_size);
                (void)m3g_read_u8(data, pos, data_size); /* perspectiveCorrection */

                /* DIAG: Log class info and field details */
                fprintf(stderr, "[M3G-PARSE-PM] userID=%u class=%s inst_size=%zu fields_count=%d\n",
                        user_id, pm_class->class_name, pm_class->instance_size, pm_class->fields_count);
                fprintf(stderr, "[M3G-PARSE-PM] pos_before_o3d=%zu pos_after_o3d=%zu (o3d consumed %zu bytes)\n",
                        pos_before, *pos, *pos - pos_before);
                fprintf(stderr, "[M3G-PARSE-PM] raw bytes at pos=%zu: culling=%u(0x%02x) shading=%u(0x%02x) twoSided=%u(0x%02x) localCam=%u(0x%02x)\n",
                        pos_before, culling, culling, shading, shading, two_sided_lighting, two_sided_lighting, local_camera_lighting, local_camera_lighting);

                m3g_set_int_field(pm, "culling", culling);
                m3g_set_int_field(pm, "shading", shading);
                m3g_set_int_field(pm, "twoSidedLighting", two_sided_lighting);
                m3g_set_int_field(pm, "winding", 0); /* default: CCW */
                m3g_set_int_field(pm, "localCameraLighting", local_camera_lighting);

                /* DIAG: Verify fields were set correctly */
                jint verify_culling = m3g_get_int_field(pm, "culling", -999);
                jint verify_two_sided = m3g_get_int_field(pm, "twoSidedLighting", -999);
                fprintf(stderr, "[M3G-PARSE-PM] verify: culling=%d twoSided=%d (expected %d, %d)\n",
                        verify_culling, verify_two_sided, culling, two_sided_lighting);

                GFX_DEBUG("M3G: Created PolygonMode userID=%u cull=%d shade=%d twoSide=%d",
                         user_id, culling, shading, two_sided_lighting);
            }
            return pm;
        }

        case M3G_OBJ_IMAGE2D: {
            JavaClass* img_class = jvm_load_class(jvm, "javax/microedition/m3g/Image2D");
            if (!img_class) return NULL;

            JavaObject* img = jvm_new_object(jvm, img_class);
            if (img) {
                m3g_set_userID(img, user_id);

                /* Object3D data */
                m3g_read_object3d_data(img, data, data_size, pos, ctx);

                /* Image2D-specific per JSR-184 spec §5.7:
                 * format      : UnsignedByte  (1 byte)
                 * isMutable   : Boolean       (1 byte)
                 * width       : UnsignedInt   (4 bytes LE)
                 * height      : UnsignedInt   (4 bytes LE)
                 * [pixel data follows]
                 *
                 * BUG FIX: Previously read format as u32 (4 bytes), shifting
                 * width/height reads by 3 bytes → height read from pixel data → 0.
                 * Also: Nokia H3T exporter writes Java API constants (0x60-0x64)
                 * instead of M3G binary constants (0x10-0x14). */
                uint32_t format = m3g_read_u8(data, pos, data_size);  /* u8, not u32! */
                (void)m3g_read_u8(data, pos, data_size);              /* skip isMutable */
                int32_t width = (int32_t)m3g_read_u32(data, pos, data_size);
                int32_t height = (int32_t)m3g_read_u32(data, pos, data_size);

                m3g_set_int_field(img, "format", format);
                m3g_set_int_field(img, "width", width);
                m3g_set_int_field(img, "height", height);

                /* Sanity check dimensions */
                if (width > 0 && height > 0 && width <= 2048 && height <= 2048) {
                    /* Calculate pixel data size based on format */
                    int bytes_per_pixel = 0;
                    switch (format) {
                        /* M3G binary format constants */
                        case 0x10: bytes_per_pixel = 1; break;              /* ALPHA */
                        case 0x11: bytes_per_pixel = 1; break;              /* LUMINANCE */
                        case 0x12: bytes_per_pixel = 2; break;              /* LUMINANCE_ALPHA */
                        case 0x13: bytes_per_pixel = 3; break;              /* RGB */
                        case 0x14: bytes_per_pixel = 4; break;              /* RGBA */
                        /* Java API constants (Nokia H3T exporter) */
                        case 0x60: bytes_per_pixel = 1; break;              /* ALPHA */
                        case 0x61: bytes_per_pixel = 1; break;              /* LUMINANCE */
                        case 0x62: bytes_per_pixel = 2; break;              /* LUMINANCE_ALPHA */
                        case 0x63: bytes_per_pixel = 3; break;              /* RGB */
                        case 0x64: bytes_per_pixel = 4; break;              /* RGBA */
                        /* Packed formats */
                        case 0x90: case 0x91: bytes_per_pixel = 2; break;  /* RGB565, RGB555 */
                        case 0x92: bytes_per_pixel = 2; break;              /* RGBA4444 */
                        default:
                            fprintf(stderr, "[M3G-IMG] Unknown format 0x%02x, assuming 4 bpp\n", format);
                            bytes_per_pixel = 4; break;
                    }
                    int pixel_data_size = width * height * bytes_per_pixel;

                    /* Check for palette (format >= 0x80 && format <= 0x8F) */
                    if (format >= 0x80 && format <= 0x8F) {
                        /* Has palette: 3 or 4 byte entries */
                        if (*pos + 1 <= data_size) {
                            uint8_t palette_size_flag = m3g_read_u8(data, pos, data_size);
                            int palette_entry_size = (palette_size_flag & 1) ? 4 : 3;
                            int palette_entries = (palette_size_flag >> 1);
                            if (palette_entries <= 0) palette_entries = 256;
                            int palette_bytes = palette_entries * palette_entry_size;
                            pixel_data_size = palette_bytes + width * height; /* indexed: 1 byte per pixel */

                            /* Store palette data */
                            if (palette_bytes > 0 && palette_bytes < 65536 && *pos + palette_bytes <= data_size) {
                                JavaArray* palette_arr = jvm_new_array(jvm, T_BYTE, palette_bytes, NULL);
                                if (palette_arr) {
                                    void* dst = array_data(palette_arr);
                                    memcpy(dst, &data[*pos], palette_bytes);
                                    m3g_set_ref_field(img, "palette", (JavaObject*)palette_arr);
                                }
                            }
                            *pos += palette_bytes;
                        }
                    }

                    /* Read pixel data */
                    if (pixel_data_size > 0 && pixel_data_size < 16 * 1024 * 1024) {
                        size_t remaining = data_size - *pos;
                        if ((int)remaining >= pixel_data_size) {
                            /* Store as int[] for RGBA, or byte[] for other formats */
                            if (format == 0x14 || format == 0x64) { /* RGBA (M3G or Java API) */
                                JavaArray* pixels = jvm_new_array(jvm, T_INT, width * height, NULL);
                                if (pixels) {
                                    jint* dst = (jint*)array_data(pixels);
                                    uint8_t* src = (uint8_t*)&data[*pos];
                                    for (int p = 0; p < width * height; p++) {
                                        dst[p] = 0xFF000000 |
                                            (src[p * 4] << 16) |
                                            (src[p * 4 + 1] << 8) |
                                            src[p * 4 + 2];
                                        /* Use alpha from source */
                                        dst[p] = (src[p * 4 + 3] << 24) | (dst[p] & 0x00FFFFFF);
                                    }
                                    m3g_set_ref_field(img, "pixels", (JavaObject*)pixels);
                                }
                            } else if (format == 0x90) { /* RGB565 */
                                JavaArray* pixels = jvm_new_array(jvm, T_INT, width * height, NULL);
                                if (pixels) {
                                    jint* dst = (jint*)array_data(pixels);
                                    uint16_t* src = (uint16_t*)&data[*pos];
                                    for (int p = 0; p < width * height; p++) {
                                        uint16_t c = src[p];
                                        int r = ((c >> 11) & 0x1F) * 255 / 31;
                                        int g = ((c >> 5) & 0x3F) * 255 / 63;
                                        int b = (c & 0x1F) * 255 / 31;
                                        dst[p] = 0xFF000000 | (r << 16) | (g << 8) | b;
                                    }
                                    m3g_set_ref_field(img, "pixels", (JavaObject*)pixels);
                                }
                            } else if (format == 0x92) { /* RGBA4444 */
                                JavaArray* pixels = jvm_new_array(jvm, T_INT, width * height, NULL);
                                if (pixels) {
                                    jint* dst = (jint*)array_data(pixels);
                                    uint16_t* src = (uint16_t*)&data[*pos];
                                    for (int p = 0; p < width * height; p++) {
                                        uint16_t c = src[p];
                                        int a = ((c >> 12) & 0xF) * 255 / 15;
                                        int r = ((c >> 8) & 0xF) * 255 / 15;
                                        int g = ((c >> 4) & 0xF) * 255 / 15;
                                        int b = (c & 0xF) * 255 / 15;
                                        dst[p] = (a << 24) | (r << 16) | (g << 8) | b;
                                    }
                                    m3g_set_ref_field(img, "pixels", (JavaObject*)pixels);
                                }
                            } else if (format == 0x13 || format == 0x63) { /* RGB (M3G or Java API) */
                                JavaArray* pixels = jvm_new_array(jvm, T_INT, width * height, NULL);
                                if (pixels) {
                                    jint* dst = (jint*)array_data(pixels);
                                    uint8_t* src = (uint8_t*)&data[*pos];
                                    for (int p = 0; p < width * height; p++) {
                                        dst[p] = 0xFF000000 |
                                            (src[p * 3] << 16) |
                                            (src[p * 3 + 1] << 8) |
                                            src[p * 3 + 2];
                                    }
                                    m3g_set_ref_field(img, "pixels", (JavaObject*)pixels);
                                }
                            } else {
                                /* Generic: store raw bytes */
                                JavaArray* pixels = jvm_new_array(jvm, T_BYTE, pixel_data_size, NULL);
                                if (pixels) {
                                    void* dst = array_data(pixels);
                                    memcpy(dst, &data[*pos], pixel_data_size);
                                    m3g_set_ref_field(img, "pixels", (JavaObject*)pixels);
                                }
                            }
                        }
                    }
                    /* For palette-based formats, pos was already advanced above
                     * (palette_bytes + pixel data). pixel_data_size includes palette,
                     * so only advance by the non-palette part. */
                    if (format >= 0x80 && format <= 0x8F) {
                        *pos += width * height;  /* only pixel indices */
                    } else {
                        *pos += pixel_data_size;
                    }
                }

                GFX_DEBUG("M3G: Created Image2D userID=%u fmt=0x%02X %dx%d",
                         user_id, format, width, height);
            }
            return img;
        }

        case M3G_OBJ_TEXTURE2D: {
            JavaClass* tex_class = jvm_load_class(jvm, "javax/microedition/m3g/Texture2D");
            if (!tex_class) return NULL;

            JavaObject* tex = jvm_new_object(jvm, tex_class);
            if (tex) {
                m3g_set_userID(tex, user_id);

                size_t pos_before_obj3d = *pos;
                /* Object3D data */
                m3g_read_object3d_data(tex, data, data_size, pos, ctx);
                size_t pos_after_obj3d = *pos;

                /* Texture2D-specific per JSR-184 spec */
                size_t pos_before_imgref = *pos;
                uint32_t image_ref = m3g_read_u32(data, pos, data_size);
                uint8_t blend_s = m3g_read_u8(data, pos, data_size);
                uint8_t blend_t = m3g_read_u8(data, pos, data_size);
                (void)m3g_read_u8(data, pos, data_size); /* wrappingS */
                (void)m3g_read_u8(data, pos, data_size); /* wrappingT */
                uint8_t level_filter = m3g_read_u8(data, pos, data_size);
                uint8_t image_filter = m3g_read_u8(data, pos, data_size);

                /* DIAG: Log full hex dump of this Texture2D object for debugging */
                fprintf(stderr, "[M3G-TEX-PARSE] Texture2D userID=%u: obj3d_bytes=%zu, imgRef_pos=%zu\n",
                        user_id, pos_after_obj3d - pos_before_obj3d, pos_before_imgref);
                fprintf(stderr, "[M3G-TEX-PARSE]   imageRef_LE=%u (0x%08x), blend=%d/%d, filter=%d/%d\n",
                        image_ref, image_ref, blend_s, blend_t, level_filter, image_filter);
                /* Dump 32 bytes starting from pos_before_obj3d (the Object3D data start) */
                fprintf(stderr, "[M3G-TEX-PARSE]   hex dump from obj3d start (pos=%zu):\n    ", pos_before_obj3d);
                for (int rb = 0; rb < 32 && pos_before_obj3d + rb < data_size; rb++) {
                    fprintf(stderr, "%02x ", data[pos_before_obj3d + rb]);
                    if ((rb + 1) % 16 == 0) fprintf(stderr, "\n    ");
                }
                fprintf(stderr, "\n");

                /* Store image reference as int for later linking */
                m3g_set_int_field(tex, "imageRef", image_ref);
                m3g_set_int_field(tex, "blendS", blend_s);
                m3g_set_int_field(tex, "blendT", blend_t);
                m3g_set_int_field(tex, "filtering", level_filter | (image_filter << 4));
            }
            return tex;
        }

        case M3G_OBJ_SPRITE3D: {
            JavaClass* sprite_class = jvm_load_class(jvm, "javax/microedition/m3g/Sprite3D");
            if (!sprite_class) return NULL;

            JavaObject* sprite = jvm_new_object(jvm, sprite_class);
            if (sprite) {
                m3g_set_userID(sprite, user_id);

                /* Read full hierarchy: Object3D -> Transformable -> Node -> Sprite3D */
                m3g_read_object3d_data(sprite, data, data_size, pos, ctx);
                m3g_read_transformable_data(sprite, data, data_size, pos, jvm);
                m3g_read_node_data(sprite, data, data_size, pos);

                /* Sprite3D-specific per JSR-184 spec */
                uint8_t scaled = m3g_read_u8(data, pos, data_size);
                uint32_t image_ref = m3g_read_u32(data, pos, data_size);
                int32_t crop_x = (int32_t)m3g_read_u32(data, pos, data_size);
                int32_t crop_y = (int32_t)m3g_read_u32(data, pos, data_size);
                int32_t crop_w = (int32_t)m3g_read_u32(data, pos, data_size);
                int32_t crop_h = (int32_t)m3g_read_u32(data, pos, data_size);

                m3g_set_int_field(sprite, "scaled", scaled);
                m3g_set_int_field(sprite, "imageRef", image_ref);
                m3g_set_int_field(sprite, "cropX", crop_x);
                m3g_set_int_field(sprite, "cropY", crop_y);
                m3g_set_int_field(sprite, "cropWidth", crop_w);
                m3g_set_int_field(sprite, "cropHeight", crop_h);

                GFX_DEBUG("M3G: Created Sprite3D userID=%u scaled=%d crop=(%d,%d,%d,%d)",
                         user_id, scaled, crop_x, crop_y, crop_w, crop_h);
            }
            return sprite;
        }

        case M3G_OBJ_MORPHINGMESH: {
            /* MorphingMesh extends Mesh - parse like Mesh, then add morph targets */
            JavaClass* mm_class = jvm_load_class(jvm, "javax/microedition/m3g/MorphingMesh");
            if (!mm_class) {
                /* Fallback: use Mesh class */
                mm_class = jvm_load_class(jvm, "javax/microedition/m3g/Mesh");
            }
            if (!mm_class) return NULL;

            JavaObject* mm = jvm_new_object(jvm, mm_class);
            if (mm) {
                m3g_set_userID(mm, user_id);

                /* Read full hierarchy: Object3D -> Transformable -> Node -> Mesh */
                m3g_read_object3d_data(mm, data, data_size, pos, ctx);
                m3g_read_transformable_data(mm, data, data_size, pos, jvm);
                m3g_read_node_data(mm, data, data_size, pos);

                /* Mesh-specific data */
                uint32_t vb_ref = m3g_read_u32(data, pos, data_size);
                uint32_t submesh_count = m3g_read_u32(data, pos, data_size);
                uint32_t ib_ref = 0, app_ref = 0;
                for (uint32_t i = 0; i < submesh_count; i++) {
                    uint32_t this_ib = m3g_read_u32(data, pos, data_size);
                    uint32_t this_app = m3g_read_u32(data, pos, data_size);
                    if (i == 0) { ib_ref = this_ib; app_ref = this_app; }
                }
                m3g_set_int_field(mm, "vertexBufferRef", vb_ref);
                m3g_set_int_field(mm, "submeshCount", submesh_count);
                m3g_set_int_field(mm, "indexBufferRef", ib_ref);
                m3g_set_int_field(mm, "appearanceRef", app_ref);

                /* MorphingMesh-specific: morph target count and references */
                uint32_t morph_target_count = m3g_read_u32(data, pos, data_size);
                for (uint32_t i = 0; i < morph_target_count; i++) {
                    m3g_read_u32(data, pos, data_size); /* morph target VertexBuffer ref */
                }
                /* Morph weights: startWeight + endWeight per morph target */
                float start_weight = m3g_read_float(data, pos, data_size);
                float end_weight = m3g_read_float(data, pos, data_size);
                (void)start_weight; (void)end_weight;

                ctx->mesh = mm;
                GFX_DEBUG("M3G: Created MorphingMesh userID=%u submeshes=%u morphTargets=%u",
                         user_id, submesh_count, morph_target_count);
            }
            return mm;
        }

        case M3G_OBJ_SKINNEDMESH: {
            /* SkinnedMesh extends Mesh - parse like Mesh, then add skeleton/bones */
            JavaClass* sm_class = jvm_load_class(jvm, "javax/microedition/m3g/SkinnedMesh");
            if (!sm_class) {
                /* Fallback: use Mesh class */
                sm_class = jvm_load_class(jvm, "javax/microedition/m3g/Mesh");
            }
            if (!sm_class) return NULL;

            JavaObject* sm = jvm_new_object(jvm, sm_class);
            if (sm) {
                m3g_set_userID(sm, user_id);

                /* Read full hierarchy: Object3D -> Transformable -> Node -> Mesh */
                m3g_read_object3d_data(sm, data, data_size, pos, ctx);
                m3g_read_transformable_data(sm, data, data_size, pos, jvm);
                m3g_read_node_data(sm, data, data_size, pos);

                /* Mesh-specific data */
                uint32_t vb_ref = m3g_read_u32(data, pos, data_size);
                uint32_t submesh_count = m3g_read_u32(data, pos, data_size);
                uint32_t ib_ref = 0, app_ref = 0;
                for (uint32_t i = 0; i < submesh_count; i++) {
                    uint32_t this_ib = m3g_read_u32(data, pos, data_size);
                    uint32_t this_app = m3g_read_u32(data, pos, data_size);
                    if (i == 0) { ib_ref = this_ib; app_ref = this_app; }
                }
                m3g_set_int_field(sm, "vertexBufferRef", vb_ref);
                m3g_set_int_field(sm, "submeshCount", submesh_count);
                m3g_set_int_field(sm, "indexBufferRef", ib_ref);
                m3g_set_int_field(sm, "appearanceRef", app_ref);

                /* SkinnedMesh-specific: skeleton reference and bone data */
                uint32_t skeleton_ref = m3g_read_u32(data, pos, data_size);
                uint32_t bone_count = m3g_read_u32(data, pos, data_size);
                m3g_set_int_field(sm, "skeletonRef", skeleton_ref);

                /* Read transform/bone pairs */
                for (uint32_t i = 0; i < bone_count; i++) {
                    uint32_t transform_ref = m3g_read_u32(data, pos, data_size);
                    uint32_t bone_index = m3g_read_u32(data, pos, data_size);
                    (void)transform_ref; (void)bone_index;
                }

                /* SkinnedMesh also has per-submesh bone references */
                for (uint32_t s = 0; s < submesh_count && s < 256; s++) {
                    uint32_t bone_count_s = m3g_read_u32(data, pos, data_size);
                    for (uint32_t i = 0; i < bone_count_s && i < 256; i++) {
                        uint32_t bone_ref = m3g_read_u32(data, pos, data_size);
                        (void)bone_ref;
                    }
                }

                ctx->mesh = sm;
                GFX_DEBUG("M3G: Created SkinnedMesh userID=%u submeshes=%u bones=%u skelRef=%u",
                         user_id, submesh_count, bone_count, skeleton_ref);
            }
            return sm;
        }

        case M3G_OBJ_GROUP: {
            JavaClass* group_class = jvm_load_class(jvm, "javax/microedition/m3g/Group");
            if (!group_class) return NULL;

            JavaObject* group = jvm_new_object(jvm, group_class);
            if (group) {
                m3g_set_userID(group, user_id);

                /* Read full hierarchy: Object3D -> Transformable -> Node -> Group */
                m3g_read_object3d_data(group, data, data_size, pos, ctx);
                m3g_read_transformable_data(group, data, data_size, pos, jvm);
                m3g_read_node_data(group, data, data_size, pos);
                m3g_read_group_data(group, data, data_size, pos, jvm);

                GFX_DEBUG("M3G: Created Group userID=%u", user_id);
            }
            return group;
        }

        default:
            /* Skip unknown object types */
            GFX_DEBUG("M3G: Skipping unknown object type %d userID=%u", obj_type, user_id);
            return NULL;
    }
}

/* Loader.load(String name) - M3G file loading with correct section parsing */
/* FIX: Completely rewritten to match JSR-184 M3G specification */
static JavaValue native_loader_load(JVM* jvm, JavaThread* thread,
                                     JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaString* name_str = (JavaString*)args[0].ref;

    /* Resources that need cleanup */
    uint8_t* data = NULL;
    uint8_t* decompressed = NULL;
    JavaObject** objects = NULL;
    JavaArray* result = NULL;

    /* Result variables */
    JavaClass* world_class = NULL;
    JavaClass* camera_class = NULL;
    JavaClass* bg_class = NULL;
    JavaClass* light_class = NULL;

    /* Parse context - objects stored sequentially, M3G refs are: ref-2 = array index */
    M3GParseContext ctx = {0};
    ctx.object_count = 0;     /* Objects stored sequentially starting at index 0 */
    ctx.header_seen = 0;       /* Will be set to 0 when header is seen */
    ctx.objects_start_index = 0;  /* Index where actual objects start */
    ctx.object_capacity = 512;
    ctx.objects = calloc(ctx.object_capacity, sizeof(JavaObject*));
    objects = ctx.objects;  /* Track for cleanup */

    /* Helper: ensure objects array has capacity for one more entry */
    #define M3G_ENSURE_CAPACITY(ctx) do { \
        if ((ctx)->object_count >= (ctx)->object_capacity) { \
            int new_cap = (ctx)->object_capacity * 2; \
            JavaObject** new_arr = realloc((ctx)->objects, new_cap * sizeof(JavaObject*)); \
            if (new_arr) { \
                memset(new_arr + (ctx)->object_capacity, 0, (new_cap - (ctx)->object_capacity) * sizeof(JavaObject*)); \
                (ctx)->objects = new_arr; \
                (ctx)->object_capacity = new_cap; \
                objects = new_arr; \
                fprintf(stderr, "[M3G] Expanded objects array to %d\n", new_cap); \
            } \
        } \
    } while(0)

    /* NOTE: Do NOT clear the global M3G object registry here!
     * Games load multiple M3G files (su30.m3g, models.m3g, models_add.m3g)
     * and use find(userID) to locate objects across files.
     * Clearing the registry on each load causes find() to fail for objects
     * from previously loaded files. */

    if (!ctx.objects) {
        GFX_DEBUG("Loader.load: Failed to allocate objects array");
        goto cleanup_error;
    }

    if (!name_str) {
        GFX_DEBUG("Loader.load: null name string");
        goto cleanup_error;
    }

    const char* name = string_utf8(jvm, name_str);
    fprintf(stderr, "[M3G] Loader.load: '%s'\n", name ? name : "NULL");

    if (!name) {
        goto cleanup_error;
    }

    /* Skip leading slash if present */
    const char* resource_name = name;
    if (resource_name[0] == '/') {
        resource_name++;
    }

    /* Try to load from JAR */
    size_t data_size;
    data = load_jar_resource(resource_name, &data_size);

    if (!data) {
        fprintf(stderr, "[M3G] Loader.load: file '%s' not found in JAR\n", resource_name);
        /* Still create a World even if file not found - game might use default */
    } else {
        GFX_DEBUG("Loader.load: loaded %zu bytes from '%s'", data_size, resource_name);
        fprintf(stderr, "[M3G] Loaded '%s' (%zu bytes)\n", resource_name, data_size);
    }

    /* Parse M3G file if we have data */
    if (data && data_size >= 12) {
        /* Check M3G magic: 0xAB 'J' 'S' 'R' '1' '8' '4' 0xBB 0x0D 0x0A 0x1A 0x0A */
        if (data[0] == M3G_MAGIC1 && 
            strncmp((char*)&data[1], "JSR184", 6) == 0 && 
            data[7] == M3G_MAGIC2 &&
            data[8] == 0x0D && data[9] == 0x0A &&
            data[10] == 0x1A && data[11] == 0x0A) {
            
            fprintf(stderr, "[M3G] Loader.load: Valid M3G file, size=%zu\n", data_size);
            
            /* Clear texture caches before parsing new M3G file */
            m3g_clear_tex_image_map();
            m3g_clear_texture_cache();
            
            size_t file_pos = 12;  /* Start after magic identifier */
            int section_count = 0;
            int current_section = 0;
            uint32_t total_file_size = 0;
            
            /* Parse sections according to M3G spec */
            while (file_pos + 13 <= data_size && section_count < 100) {
                /* Read section header */
                uint8_t compression_scheme = data[file_pos++];
                uint32_t total_section_length = data[file_pos] | (data[file_pos+1] << 8) | 
                                                (data[file_pos+2] << 16) | (data[file_pos+3] << 24);
                file_pos += 4;
                uint32_t uncompressed_length = data[file_pos] | (data[file_pos+1] << 8) | 
                                               (data[file_pos+2] << 16) | (data[file_pos+3] << 24);
                file_pos += 4;
                
                GFX_DEBUG("M3G: Section %d: compression=%d, totalLen=%u, uncompressedLen=%u",
                         section_count, compression_scheme, total_section_length, uncompressed_length);
                
                /* Store current section in context for object parsing */
                ctx.current_section = current_section;
                
                /* Sanity check: section length must be reasonable */
                if (total_section_length < 13 || total_section_length > data_size) {
                    GFX_DEBUG("M3G: Invalid section length %u, stopping", total_section_length);
                    break;
                }
                
                /* Calculate section data boundaries */
                size_t section_data_start = file_pos;
                size_t section_data_size = total_section_length - 13;  /* Minus header and checksum */
                
                /* Get section data (decompress if needed) */
                uint8_t* section_data;
                size_t section_data_len;
                
                if (compression_scheme == 0) {
                    /* Uncompressed section */
                    section_data = &data[section_data_start];
                    section_data_len = section_data_size;
                } else if (compression_scheme == 1) {
                    /* Zlib compressed section */
                    section_data = m3g_decompress(&data[section_data_start], section_data_size, &section_data_len);
                    decompressed = section_data;  /* Track for cleanup */
                    
                    if (!section_data || section_data_len != uncompressed_length) {
                        GFX_DEBUG("M3G: Decompression failed or size mismatch (got %zu, expected %u)",
                                 section_data_len, uncompressed_length);
                        if (section_data) {
                            free(section_data);
                            decompressed = NULL;
                        }
                        goto next_section;
                    }
                } else {
                    GFX_DEBUG("M3G: Unknown compression scheme %d", compression_scheme);
                    goto next_section;
                }
                
                /* Parse objects within section */
                if (section_data && section_data_len > 0) {
                    size_t obj_pos = 0;
                    
                    while (obj_pos < section_data_len) {
                        /* Read object type and length */
                        if (obj_pos + 5 > section_data_len) break;
                        
                        uint8_t obj_type = section_data[obj_pos++];
                        uint32_t obj_length = section_data[obj_pos] | (section_data[obj_pos+1] << 8) |
                                            (section_data[obj_pos+2] << 16) | (section_data[obj_pos+3] << 24);
                        obj_pos += 4;
                        
                        GFX_DEBUG("M3G: Object type=%d length=%u at pos=%zu", 
                                 obj_type, obj_length, obj_pos - 5);
                        
                        /* Validate object length */
                        if (obj_length == 0 || obj_pos + obj_length > section_data_len) {
                            GFX_DEBUG("M3G: Invalid object length %u, skipping", obj_length);
                            break;
                        }
                        
                        /* Parse object based on type */
                        size_t obj_start = obj_pos;
                        
                        GFX_DEBUG("M3G: Object type=%d len=%u at offset %zu", 
                                 obj_type, obj_length, obj_start);
                        
                        switch (obj_type) {
                            case 0:  /* Header Object (inside first section) */
                                /* FIX: Header must only be in section 0 */
                                if (current_section != 0) {
                                    GFX_DEBUG("M3G ERROR: Header object in wrong section %d!", current_section);
                                    fprintf(stderr, "[M3G] ERROR: Header in wrong section %d\n", current_section);
                                    obj_pos += obj_length;
                                    break;
                                }
                                if (ctx.header_seen) {
                                    GFX_DEBUG("M3G ERROR: Duplicate header!");
                                    fprintf(stderr, "[M3G] ERROR: Duplicate header\n");
                                    obj_pos += obj_length;
                                    break;
                                }
                                ctx.header_seen = 1;
                                
                                if (obj_length >= 7) {
                                    uint8_t version_major = section_data[obj_pos++];
                                    uint8_t version_minor = section_data[obj_pos++];
                                    /* hasExternalReferences */ uint8_t has_ext_refs = section_data[obj_pos++];
                                    ctx.external_links = has_ext_refs;
                                    total_file_size = section_data[obj_pos] | (section_data[obj_pos+1] << 8) |
                                                     (section_data[obj_pos+2] << 16) | (section_data[obj_pos+3] << 24);
                                    obj_pos += 4;
                                    /* approximateContentSize */ obj_pos += 4;
                                    /* authoringField (string) - skip null-terminated string */
                                    while (obj_pos < section_data_len && section_data[obj_pos] != 0) {
                                        obj_pos++;
                                    }
                                    if (obj_pos < section_data_len) obj_pos++;  /* Skip null terminator */
                                    
                                    GFX_DEBUG("M3G: Header v%d.%d totalSize=%u hasExternalRefs=%d",
                                             version_major, version_minor, total_file_size, has_ext_refs);
                                    fprintf(stderr, "[M3G] Header v%d.%d totalSize=%u\n", 
                                            version_major, version_minor, total_file_size);
                                }
                                /* FIX: Do NOT store NULL for header - this breaks indexing! */
                                /* According to JSR-184: ref 0 = null, ref N = objects[N-2] */
                                /* So we don't increment object_count for header */
                                GFX_DEBUG("M3G: Header parsed, objects will start at index 0");
                                break;
                                
                            case 255:  /* External Reference */
                                /* FIX: External reference must only be in section 1 */
                                if (current_section != 1) {
                                    GFX_DEBUG("M3G ERROR: External reference in wrong section %d!", current_section);
                                    fprintf(stderr, "[M3G] ERROR: External reference in wrong section %d\n", current_section);
                                }
                                GFX_DEBUG("M3G: External reference, skipping %u bytes", obj_length);
                                obj_pos += obj_length;
                                /* External references are NOT stored in the objects array */
                                /* They don't increment object_count */
                                break;
                                
                            case M3G_OBJ_ANIMATIONCONTROLLER:
                            case M3G_OBJ_ANIMATIONTRACK:
                            case M3G_OBJ_KEYFRAMESEQUENCE:
                                /* Animation objects - parse and store for proper indexing */
                                GFX_DEBUG("M3G: Animation obj %d at index %d", obj_type, ctx.object_count);
                                obj_pos += obj_length;
                                /* Store placeholder to maintain correct indexing */
                                M3G_ENSURE_CAPACITY(&ctx);
                                ctx.objects[ctx.object_count] = NULL;  /* Placeholder */
                                ctx.object_count++;
                                break;

                            default: {
                                /* Parse object - returns object or NULL */
                                size_t saved_pos = obj_pos;
                                JavaObject* parsed = m3g_parse_object(jvm, &ctx, section_data, section_data_len, &obj_pos, obj_type, obj_length);
                                if (!parsed) {
                                    /* If parsing failed, skip the object data */
                                    obj_pos = saved_pos + obj_length;
                                }
                                /* CRITICAL: Always increment count to preserve indices */
                                /* Store object (or NULL if parsing failed) */
                                M3G_ENSURE_CAPACITY(&ctx);
                                ctx.objects[ctx.object_count] = parsed;  /* May be NULL */
                                ctx.object_count++;
                                GFX_DEBUG("M3G: Stored %s at array[%d] (M3G ref=%d) -> %p",
                                         parsed ? "object" : "NULL", ctx.object_count - 1, 
                                         ctx.object_count + 1, (void*)parsed);
                                break;
                            }
                        }
                        
                        /* Always ensure we move past this object */
                        obj_pos = obj_start + obj_length;
                        
                        /* Safety check */
                        if (obj_pos > section_data_len) {
                            GFX_DEBUG("M3G: obj_pos %zu > section_data_len %zu, breaking", 
                                     obj_pos, section_data_len);
                            break;
                        }
                    }
                }
                
                /* Free decompressed data if we allocated it */
                if (compression_scheme == 1 && section_data) {
                    free(section_data);
                    decompressed = NULL;
                }
                
            next_section:
                /* Move to next section (skip checksum at end) */
                file_pos = section_data_start + section_data_size + 4;  /* +4 for checksum */
                section_count++;
                current_section++;
            }
            
            fprintf(stderr, "[M3G] Parsed %d sections, %d objects\n", section_count, ctx.object_count);
        } else {
            fprintf(stderr, "[M3G] Loader.load: Invalid M3G magic, creating default World\n");
        }
    } else if (data) {
        fprintf(stderr, "[M3G] Data too small (%zu bytes), creating default World\n", data_size);
    } else {
        fprintf(stderr, "[M3G] No data loaded, creating default World\n");
    }

    /* Link objects by indices */
    /* FIX: M3G reference system per JSR-184 specification:
     *   ref 0 = null
     *   ref N = objects[N-2]  (NOT objects[N]!)
     * 
     * This is because Header (file position 0) is not stored,
     * and objects start at file position 2 (after header).
     * So when file says "reference 5", it means the object at file position 5,
     * which is stored at array index 5-2=3.
     */
    GFX_DEBUG("Loader.load: Linking %d objects...", ctx.object_count);
    for (int i = 0; i < ctx.object_count; i++) {
        JavaObject* obj = ctx.objects[i];
        if (!obj || !obj->header.clazz) continue;

        const char* class_name = obj->header.clazz->class_name;

        /* Link Mesh (and subclasses SkinnedMesh/MorphingMesh) to VertexBuffer, IndexBuffer, Appearance */
        if (strstr(class_name, "Mesh")) {
            int vb_ref = m3g_get_int_field(obj, "vertexBufferRef", 0);
            int ib_ref = m3g_get_int_field(obj, "indexBufferRef", 0);
            int app_ref = m3g_get_int_field(obj, "appearanceRef", 0);

            /* FIX: M3G reference: ref N = objects[N-2] */
            int vb_idx = vb_ref - 2;
            int ib_idx = ib_ref - 2;
            int app_idx = app_ref - 2;
            
            if (vb_ref > 0 && vb_idx >= 0 && vb_idx < ctx.object_count && ctx.objects[vb_idx]) {
                m3g_set_ref_field(obj, "vertexBuffer", ctx.objects[vb_idx]);
                GFX_DEBUG("M3G: Linked Mesh VB[ref=%d idx=%d] -> %p", vb_ref, vb_idx, ctx.objects[vb_idx]);
            } else if (vb_ref > 0) {
                GFX_DEBUG("M3G: WARNING: Mesh VB ref=%d idx=%d not found (count=%d)", vb_ref, vb_idx, ctx.object_count);
            }
            
            if (ib_ref > 0 && ib_idx >= 0 && ib_idx < ctx.object_count && ctx.objects[ib_idx]) {
                m3g_set_ref_field(obj, "indexBuffer", ctx.objects[ib_idx]);
                GFX_DEBUG("M3G: Linked Mesh IB[ref=%d idx=%d] -> %p", ib_ref, ib_idx, ctx.objects[ib_idx]);
            }
            
            if (app_ref > 0 && app_idx >= 0 && app_idx < ctx.object_count && ctx.objects[app_idx]) {
                m3g_set_ref_field(obj, "appearance", ctx.objects[app_idx]);
                GFX_DEBUG("M3G: Linked Mesh App[ref=%d idx=%d] -> %p", app_ref, app_idx, ctx.objects[app_idx]);
            }
        }

        /* Link VertexBuffer to VertexArrays */
        if (strstr(class_name, "VertexBuffer")) {
            int pos_ref = m3g_get_int_field(obj, "positionsRef", 0);
            int norm_ref = m3g_get_int_field(obj, "normalsRef", 0);
            int color_ref = m3g_get_int_field(obj, "colorsRef", 0);
            int tex_ref = m3g_get_int_field(obj, "texCoordsRef", 0);

            /* FIX: M3G reference: ref N = objects[N-2] */
            int pos_idx = pos_ref - 2;
            int norm_idx = norm_ref - 2;
            int color_idx = color_ref - 2;
            int tex_idx = tex_ref - 2;
            
            if (pos_ref > 0 && pos_idx >= 0 && pos_idx < ctx.object_count && ctx.objects[pos_idx]) {
                JavaObject* pos_obj = ctx.objects[pos_idx];
                int pos_comp = m3g_get_int_field(pos_obj, "componentCount", -1);
                int pos_vc = m3g_get_int_field(pos_obj, "vertexCount", -1);
                m3g_set_ref_field(obj, "positions", pos_obj);
                fprintf(stderr, "[M3G-LINK] VB %p: positions[ref=%d idx=%d] -> %p (comp=%d, verts=%d)\n",
                        (void*)obj, pos_ref, pos_idx, (void*)pos_obj, pos_comp, pos_vc);
                GFX_DEBUG("M3G: Linked VertexBuffer positions[ref=%d idx=%d]", pos_ref, pos_idx);
            }
            if (norm_ref > 0 && norm_idx >= 0 && norm_idx < ctx.object_count && ctx.objects[norm_idx]) {
                m3g_set_ref_field(obj, "normals", ctx.objects[norm_idx]);
                GFX_DEBUG("M3G: Linked VertexBuffer normals[ref=%d idx=%d]", norm_ref, norm_idx);
            }
            if (color_ref > 0 && color_idx >= 0 && color_idx < ctx.object_count && ctx.objects[color_idx]) {
                m3g_set_ref_field(obj, "colors", ctx.objects[color_idx]);
                GFX_DEBUG("M3G: Linked VertexBuffer colors[ref=%d idx=%d]", color_ref, color_idx);
            }
            if (tex_ref > 0 && tex_idx >= 0 && tex_idx < ctx.object_count && ctx.objects[tex_idx]) {
                m3g_set_ref_field(obj, "texCoords", ctx.objects[tex_idx]);
                GFX_DEBUG("M3G: Linked VertexBuffer texCoords[ref=%d idx=%d]", tex_ref, tex_idx);
            }
        }

        /* Link Appearance to Material, Texture2D, CompositingMode, Fog, PolygonMode */
        if (strstr(class_name, "Appearance")) {
            int mat_ref = m3g_get_int_field(obj, "materialRef", 0);
            int tex_ref = m3g_get_int_field(obj, "textureRef", 0);
            int comp_ref = m3g_get_int_field(obj, "compositingModeRef", 0);
            int fog_ref_val = m3g_get_int_field(obj, "fogRef", 0);
            int poly_ref = m3g_get_int_field(obj, "polygonModeRef", 0);

            int mat_idx = mat_ref - 2;
            int tex_idx = tex_ref - 2;
            int comp_idx = comp_ref - 2;
            int fog_idx = fog_ref_val - 2;
            int poly_idx = poly_ref - 2;

            if (mat_ref > 0 && mat_idx >= 0 && mat_idx < ctx.object_count && ctx.objects[mat_idx]) {
                m3g_set_ref_field(obj, "material", ctx.objects[mat_idx]);
                GFX_DEBUG("M3G: Linked Appearance material[ref=%d idx=%d] -> %p", mat_ref, mat_idx, ctx.objects[mat_idx]);
            }
            if (tex_ref > 0 && tex_idx >= 0 && tex_idx < ctx.object_count && ctx.objects[tex_idx]) {
                m3g_set_ref_field(obj, "texture", ctx.objects[tex_idx]);
                GFX_DEBUG("M3G: Linked Appearance texture[ref=%d idx=%d] -> %p", tex_ref, tex_idx, ctx.objects[tex_idx]);
            }
            if (comp_ref > 0 && comp_idx >= 0 && comp_idx < ctx.object_count && ctx.objects[comp_idx]) {
                m3g_set_ref_field(obj, "compositingMode", ctx.objects[comp_idx]);
                GFX_DEBUG("M3G: Linked Appearance compositingMode[ref=%d idx=%d] -> %p", comp_ref, comp_idx, ctx.objects[comp_idx]);
            }
            if (fog_ref_val > 0 && fog_idx >= 0 && fog_idx < ctx.object_count && ctx.objects[fog_idx]) {
                m3g_set_ref_field(obj, "fog", ctx.objects[fog_idx]);
                GFX_DEBUG("M3G: Linked Appearance fog[ref=%d idx=%d] -> %p", fog_ref_val, fog_idx, ctx.objects[fog_idx]);
            }
            if (poly_ref > 0 && poly_idx >= 0 && poly_idx < ctx.object_count && ctx.objects[poly_idx]) {
                m3g_set_ref_field(obj, "polygonMode", ctx.objects[poly_idx]);
                GFX_DEBUG("M3G: Linked Appearance polygonMode[ref=%d idx=%d] -> %p", poly_ref, poly_idx, ctx.objects[poly_idx]);
            }
        }

        /* Link Texture2D to Image2D */
        if (strstr(class_name, "Texture2D")) {
            int img_ref = m3g_get_int_field(obj, "imageRef", 0);

            /* FIX: Nokia H3T format stores ObjectIndex with byte-swapped words.
             * The value 0x00030000 should be 0x00000003 (section index 3).
             * Detect this by checking if upper 16 bits contain a valid ref
             * while lower 16 bits are zero (or both zero). */
            int img_ref_hi = (img_ref >> 16) & 0xFFFF;
            int img_ref_lo = img_ref & 0xFFFF;
            int obj_count_plus2 = ctx.object_count + 2;
            if (img_ref_hi > 0 && img_ref_hi < obj_count_plus2 && img_ref_lo == 0) {
                fprintf(stderr, "[M3G-LINK-TEX] Fixing byte-swapped imageRef: 0x%08x -> %d\n",
                        img_ref, img_ref_hi);
                img_ref = img_ref_hi;
                m3g_set_int_field(obj, "imageRef", img_ref);
            } else if (img_ref >= obj_count_plus2) {
                /* Also try: maybe the ref is stored as u16 at +2 offset */
                fprintf(stderr, "[M3G-LINK-TEX] imageRef=%d out of range (count+2=%d), trying byte extraction\n",
                        img_ref, obj_count_plus2);
            }

            int img_idx = img_ref - 2;

            fprintf(stderr, "[M3G-LINK-TEX] Texture2D %p: imageRef=%d, img_idx=%d, obj_count=%d\n",
                    (void*)obj, img_ref, img_idx, ctx.object_count);
            if (img_ref > 0 && img_idx >= 0 && img_idx < ctx.object_count) {
                if (ctx.objects[img_idx]) {
                    const char* target_cls = ctx.objects[img_idx]->header.clazz ? ctx.objects[img_idx]->header.clazz->class_name : "?";
                    fprintf(stderr, "[M3G-LINK-TEX] Linking Texture2D -> %s %p (ref=%d idx=%d)\n",
                            target_cls, (void*)ctx.objects[img_idx], img_ref, img_idx);
                    m3g_set_ref_field(obj, "image", ctx.objects[img_idx]);
                    /* Also store in C-side texture-image mapping table for
                     * reliable texture lookup even when Java field names are obfuscated */
                    m3g_store_tex_image_map(obj, ctx.objects[img_idx]);
                } else {
                    fprintf(stderr, "[M3G-LINK-TEX] WARNING: ctx.objects[%d] is NULL for Texture2D imageRef=%d\n", img_idx, img_ref);
                }
            } else if (img_ref > 0) {
                fprintf(stderr, "[M3G-LINK-TEX] WARNING: imageRef=%d out of range (idx=%d, count=%d)\n", img_ref, img_idx, ctx.object_count);
            }
        }

        /* Link Background to Image2D */
        if (strstr(class_name, "Background")) {
            int img_ref = m3g_get_int_field(obj, "imageRef", 0);
            int img_idx = img_ref - 2;

            if (img_ref > 0 && img_idx >= 0 && img_idx < ctx.object_count && ctx.objects[img_idx]) {
                m3g_set_ref_field(obj, "image", ctx.objects[img_idx]);
                GFX_DEBUG("M3G: Linked Background image[ref=%d idx=%d] -> %p", img_ref, img_idx, ctx.objects[img_idx]);
            }
        }

        /* Link Sprite3D to Image2D */
        if (strstr(class_name, "Sprite3D")) {
            int img_ref = m3g_get_int_field(obj, "imageRef", 0);
            int img_idx = img_ref - 2;

            if (img_ref > 0 && img_idx >= 0 && img_idx < ctx.object_count && ctx.objects[img_idx]) {
                m3g_set_ref_field(obj, "image", ctx.objects[img_idx]);
                GFX_DEBUG("M3G: Linked Sprite3D image[ref=%d idx=%d] -> %p", img_ref, img_idx, ctx.objects[img_idx]);
            }
        }

        /* Link SkinnedMesh to skeleton Group */
        if (strstr(class_name, "SkinnedMesh")) {
            int skel_ref = m3g_get_int_field(obj, "skeletonRef", 0);
            int skel_idx = skel_ref - 2;

            if (skel_ref > 0 && skel_idx >= 0 && skel_idx < ctx.object_count && ctx.objects[skel_idx]) {
                m3g_set_ref_field(obj, "skeleton", ctx.objects[skel_idx]);
                GFX_DEBUG("M3G: Linked SkinnedMesh skeleton[ref=%d idx=%d] -> %p", skel_ref, skel_idx, ctx.objects[skel_idx]);
            }
        }

        /* Resolve Group child references from _childRefs */
        if (strstr(class_name, "Group") || strstr(class_name, "World")) {
            /* Try to read _childRefs by name, then by direct slot 19 */
            JavaArray* child_refs = (JavaArray*)m3g_get_ref_field(obj, "_childRefs");
            fprintf(stderr, "[M3G-LINK] %s %p: _childRefs=%p (max_slots=%d, inst_size=%zu)\n",
                    class_name, (void*)obj, (void*)child_refs,
                    (int)((obj->header.clazz->instance_size - sizeof(ObjectHeader)) / sizeof(JavaValue)),
                    obj->header.clazz->instance_size);
            if (!child_refs) {
                /* Fallback: try direct slot 19 */
                int max_slots = (obj->header.clazz->instance_size - (int)sizeof(ObjectHeader)) / (int)sizeof(JavaValue);
                if (max_slots > 19) {
                    child_refs = (JavaArray*)obj->fields[19].ref;
                    if (child_refs) {
                        fprintf(stderr, "[M3G-LINK] %s: _childRefs found at fallback slot 19\n", class_name);
                    }
                }
            }
            if (child_refs && child_refs->element_type == T_INT) {
                jint* refs = (jint*)array_data(child_refs);
                int child_count = child_refs->length;

                /* Create children array if needed */
                JavaArray* children = (JavaArray*)m3g_get_ref_field(obj, "children");
                int children_slot = m3g_find_field_slot(obj, "children");
                fprintf(stderr, "[M3G-LINK] %s %p: childRefs len=%d, children=%p, children_slot=%d\n",
                        class_name, (void*)obj, child_count, (void*)children, children_slot);
                if (!children) {
                    children = jvm_new_array(jvm, DESC_OBJECT, child_count > 0 ? child_count : 16, NULL);
                    if (children) {
                        m3g_set_ref_field(obj, "children", (JavaObject*)children);
                        fprintf(stderr, "[M3G-LINK] %s %p: created children array %p (len=%d)\n",
                                class_name, (void*)obj, (void*)children, (int)children->length);
                    }
                }

                if (children && children->element_type == DESC_OBJECT) {
                    JavaObject** child_arr = (JavaObject**)array_data(children);
                    int added = 0;

                    for (int c = 0; c < child_count && added < children->length; c++) {
                        int child_ref = refs[c];
                        /* FIX: M3G reference: ref N = objects[N-2] */
                        int child_idx = child_ref - 2;
                        
                        if (child_ref > 0 && child_idx >= 0 && child_idx < ctx.object_count && ctx.objects[child_idx]) {
                            child_arr[added++] = ctx.objects[child_idx];
                            fprintf(stderr, "[M3G-LINK] %s %p: child[%d] = ref %d idx=%d -> %p\n",
                                    class_name, (void*)obj, added - 1, child_ref, child_idx, ctx.objects[child_idx]);
                        } else {
                            fprintf(stderr, "[M3G-LINK] %s %p: child ref %d idx=%d NOT RESOLVED (count=%d)\n",
                                    class_name, (void*)obj, child_ref, child_idx, ctx.object_count);
                        }
                    }
                    fprintf(stderr, "[M3G-LINK] %s %p: resolved %d/%d children\n",
                            class_name, (void*)obj, added, child_count);
                }

                /* Clear temporary storage */
                m3g_set_ref_field(obj, "_childRefs", NULL);
            } else if (child_refs) {
                fprintf(stderr, "[M3G-LINK] %s %p: _childRefs element_type=%d, expected T_INT=%d\n",
                        class_name, (void*)obj, child_refs->element_type, T_INT);
            }
        }

        /* Add Mesh to World if we have both */
        if (strstr(class_name, "Mesh") && ctx.world) {
            /* Add to World's children array */
            JavaArray* children = (JavaArray*)m3g_get_ref_field(ctx.world, "children");
            if (!children) {
                children = jvm_new_array(jvm, DESC_OBJECT, 16, NULL);
                if (children) {
                    m3g_set_ref_field(ctx.world, "children", (JavaObject*)children);
                }
            }
            if (children) {
                /* Find empty slot */
                JavaObject** arr = (JavaObject**)array_data(children);
                for (jsize j = 0; j < children->length; j++) {
                    if (!arr[j]) {
                        arr[j] = obj;
                        GFX_DEBUG("M3G: Added Mesh to World children[%d]", j);
                        break;
                    }
                }
            }
        }
    }

    /* Resolve World's activeCamera and background references */
    if (ctx.world) {
        int camera_ref = m3g_get_int_field(ctx.world, "activeCameraRef", 0);
        int bg_ref = m3g_get_int_field(ctx.world, "backgroundRef", 0);

        /* FIX: M3G reference: ref N = objects[N-2] */
        int camera_idx = camera_ref - 2;
        int bg_idx = bg_ref - 2;

        if (camera_ref > 0 && camera_idx >= 0 && camera_idx < ctx.object_count && ctx.objects[camera_idx]) {
            m3g_set_ref_field(ctx.world, "activeCamera", ctx.objects[camera_idx]);
            GFX_DEBUG("M3G: Linked World activeCamera[ref=%d idx=%d] -> %p", camera_ref, camera_idx, ctx.objects[camera_idx]);
        }
        if (bg_ref > 0 && bg_idx >= 0 && bg_idx < ctx.object_count && ctx.objects[bg_idx]) {
            m3g_set_ref_field(ctx.world, "background", ctx.objects[bg_idx]);
            GFX_DEBUG("M3G: Linked World background[ref=%d idx=%d] -> %p", bg_ref, bg_idx, ctx.objects[bg_idx]);
        }
    }

    /* Create World object if not found during parsing */
    world_class = jvm_load_class(jvm, "javax/microedition/m3g/World");
    if (!world_class) {
        GFX_DEBUG("Loader.load: World class not found");
        goto cleanup_error;
    }

    if (!ctx.world) {
        ctx.world = jvm_new_object(jvm, world_class);
        if (ctx.world) {
            m3g_set_int_field(ctx.world, "scope", -1);
        }
    }

    if (!ctx.world) {
        GFX_DEBUG("Loader.load: failed to create World");
        goto cleanup_error;
    }

    /* Create and attach Camera if not found during parsing */
    if (!ctx.camera) {
        camera_class = jvm_load_class(jvm, "javax/microedition/m3g/Camera");
        if (camera_class) {
            ctx.camera = jvm_new_object(jvm, camera_class);
            if (ctx.camera) {
                m3g_set_int_field(ctx.camera, "projectionType", 1);  /* Perspective */
                m3g_set_float_field(ctx.camera, "fov", 60.0f);
                m3g_set_float_field(ctx.camera, "near", 1.0f);
                m3g_set_float_field(ctx.camera, "far", 1000.0f);
                GFX_DEBUG("Loader.load: created default Camera");
            }
        }
    }

    /* Set active camera on world */
    if (ctx.camera) {
        m3g_set_ref_field(ctx.world, "activeCamera", ctx.camera);
    }

    /* Create Background if not found */
    if (!ctx.background) {
        bg_class = jvm_load_class(jvm, "javax/microedition/m3g/Background");
        if (bg_class) {
            ctx.background = jvm_new_object(jvm, bg_class);
            if (ctx.background) {
                m3g_set_int_field(ctx.background, "clearColor", 0xFF202020);
            }
        }
    }

    if (ctx.background) {
        m3g_set_ref_field(ctx.world, "background", ctx.background);
    }

    /* Create default Light if not found */
    if (!ctx.light) {
        light_class = jvm_load_class(jvm, "javax/microedition/m3g/Light");
        if (light_class) {
            ctx.light = jvm_new_object(jvm, light_class);
            if (ctx.light) {
                m3g_set_int_field(ctx.light, "lightType", 2);  /* Directional */
                m3g_set_int_field(ctx.light, "color", 0xFFFFFFFF);
                m3g_set_float_field(ctx.light, "intensity", 1.0f);
                GFX_DEBUG("Loader.load: created default Light");
            }
        }
    }

    /* Success: prepare result */
    /* The returned array should contain only non-null objects for game compatibility.
     * Reference resolution is done internally using ctx.objects[] with correct indices.
     * Games typically access objects[0] as World, or use World.find(userID).
     */

    /* Count non-null objects for result array */
    int valid_count = 0;
    for (int i = 0; i < ctx.object_count; i++) {
        if (ctx.objects[i]) valid_count++;
    }

    /* Ensure we include World */
    bool world_in_objects = false;
    for (int i = 0; i < ctx.object_count; i++) {
        if (ctx.objects[i] == ctx.world) {
            world_in_objects = true;
            break;
        }
    }
    if (ctx.world && !world_in_objects) valid_count++;

    /* Ensure at least 1 slot if we have a world */
    if (valid_count == 0 && ctx.world) valid_count = 1;

    GFX_DEBUG("Loader.load: %d objects stored, %d valid, world=%p",
             ctx.object_count, valid_count, (void*)ctx.world);

    result = jvm_new_array(jvm, DESC_OBJECT, valid_count, NULL);
    if (result) {
        JavaObject** arr = (JavaObject**)array_data(result);
        int idx = 0;

        /* Add non-null objects to result array */
        for (int i = 0; i < ctx.object_count && idx < valid_count; i++) {
            if (ctx.objects[i]) {
                arr[idx++] = ctx.objects[i];
                GFX_DEBUG("Loader.load: result[%d] = %p (file index %d)",
                         idx - 1, (void*)ctx.objects[i], i);
            }
        }

        /* Add world if not already included */
        if (ctx.world && !world_in_objects && idx < valid_count) {
            arr[idx++] = ctx.world;
            GFX_DEBUG("Loader.load: result[%d] = %p (created World)",
                     idx - 1, (void*)ctx.world);
        }
    }

    fprintf(stderr, "[M3G] Returning array[%d] with World=%p\n", valid_count, (void*)ctx.world);

    /* FIX 32: Track last World for force-render.
     * Many games (like SU-30) load M3G files and manipulate World objects
     * directly without ever calling Graphics3D.bindTarget/render/releaseTarget.
     * They set up the scene via World.setActiveCamera and Node transforms,
     * then expect the emulator to render it. By tracking the last loaded World,
     * we enable force-render to detect and render the scene. */
    if (ctx.world) {
        g_m3g_last_world = ctx.world;
        g_m3g_scene_setup_done = true;
        m3g_thread_fence();
        fprintf(stderr, "[M3G-FORCE] Loader.load: tracked World %p for force-render\n", (void*)ctx.world);
    }

    /* Cleanup and return success */
    if (data) free(data);
    if (objects) free(objects);
    return NATIVE_RETURN_OBJECT(result);

cleanup_error:
    /* FIX: Centralized cleanup for all error paths */
    if (data) free(data);
    if (decompressed) free(decompressed);
    if (objects) free(objects);
    return NATIVE_RETURN_NULL();
}

/* Loader.load(byte[] data, int offset) - Load from byte array */
static JavaValue native_loader_load_bytes(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaArray* data = (JavaArray*)args[0].ref;
    jint offset = args[1].i;
    
    if (!data) {
        GFX_DEBUG("Loader.load: null data");
        return NATIVE_RETURN_NULL();
    }
    
    /* Calculate effective data range */
    int effective_length = data->length - offset;
    if (effective_length <= 0 || offset < 0 || offset >= data->length) {
        GFX_DEBUG("Loader.load: invalid offset=%d, length=%d", offset, data->length);
        return NATIVE_RETURN_NULL();
    }
    
    GFX_DEBUG("Loader.load(byte[]): %d bytes at offset %d", effective_length, offset);
    fprintf(stderr, "[M3G] Loader.load(byte[]): %d bytes at offset %d\n", effective_length, offset);
    
    /* Copy byte array data directly into memory - no temp file needed.
     * The data is already in the Java byte[] array, just memcpy it. */
    uint8_t* src_data = (uint8_t*)array_data(data);
    if (!src_data) {
        GFX_DEBUG("Loader.load(byte[]): null array data");
        return NATIVE_RETURN_NULL();
    }
    
    long file_size = (long)effective_length;
    
    uint8_t* file_data = (uint8_t*)malloc(file_size);
    if (!file_data) {
        GFX_DEBUG("Loader.load(byte[]): failed to allocate %ld bytes", file_size);
        return NATIVE_RETURN_NULL();
    }
    
    memcpy(file_data, src_data + offset, file_size);
    
    /* --- Inline M3G parsing (same structure as native_loader_load) --- */
    
    /* Resources that need cleanup */
    uint8_t* decompressed = NULL;
    JavaObject** objects = NULL;
    JavaArray* result = NULL;
    
    /* Result variables */
    JavaClass* world_class = NULL;
    JavaClass* camera_class = NULL;
    JavaClass* bg_class = NULL;
    JavaClass* light_class = NULL;
    
    /* Parse context */
    M3GParseContext ctx = {0};
    ctx.object_count = 0;
    ctx.header_seen = 0;
    ctx.objects_start_index = 0;
    ctx.object_capacity = 256;
    ctx.objects = calloc(ctx.object_capacity, sizeof(JavaObject*));
    objects = ctx.objects;
    
    /* Clear the global M3G object registry */
    m3g_registry_clear();
    
    if (!ctx.objects) {
        free(file_data);
        goto load_bytes_cleanup;
    }
    
    /* Check M3G magic header */
    if (file_size >= 12 &&
        file_data[0] == 0xAB && 
        strncmp((char*)&file_data[1], "JSR184", 6) == 0 && 
        file_data[7] == 0xBB &&
        file_data[8] == 0x0D && file_data[9] == 0x0A &&
        file_data[10] == 0x1A && file_data[11] == 0x0A) {
        
        GFX_DEBUG("Loader.load(byte[]): Valid M3G file detected, size=%ld", file_size);
        fprintf(stderr, "[M3G] Valid M3G file from byte array (%ld bytes)\n", file_size);
        
        /* Clear texture caches before parsing new M3G file */
        m3g_clear_tex_image_map();
        m3g_clear_texture_cache();
        
        size_t fpos = 12;
        int section_count = 0;
        int current_section = 0;
        
        while (fpos + 13 <= (size_t)file_size && section_count < 100) {
            uint8_t compression_scheme = file_data[fpos++];
            uint32_t total_section_length = file_data[fpos] | (file_data[fpos+1] << 8) | 
                                            (file_data[fpos+2] << 16) | (file_data[fpos+3] << 24);
            fpos += 4;
            uint32_t uncompressed_length = file_data[fpos] | (file_data[fpos+1] << 8) | 
                                           (file_data[fpos+2] << 16) | (file_data[fpos+3] << 24);
            fpos += 4;
            
            if (total_section_length < 13 || total_section_length > (size_t)file_size) break;
            
            size_t section_data_start = fpos;
            size_t section_data_size = total_section_length - 13;
            
            uint8_t* section_data;
            size_t section_data_len;
            
            if (compression_scheme == 0) {
                section_data = &file_data[section_data_start];
                section_data_len = section_data_size;
            } else if (compression_scheme == 1) {
                section_data = m3g_decompress(&file_data[section_data_start], section_data_size, &section_data_len);
                decompressed = section_data;
                if (!section_data || section_data_len != uncompressed_length) {
                    if (section_data) { free(section_data); decompressed = NULL; }
                    goto load_bytes_next_section;
                }
            } else {
                goto load_bytes_next_section;
            }
            
            /* Parse objects within section */
            if (section_data && section_data_len > 0) {
                size_t obj_pos = 0;
                while (obj_pos < section_data_len) {
                    if (obj_pos + 5 > section_data_len) break;
                    
                    uint8_t obj_type = section_data[obj_pos++];
                    uint32_t obj_length = section_data[obj_pos] | (section_data[obj_pos+1] << 8) |
                                        (section_data[obj_pos+2] << 16) | (section_data[obj_pos+3] << 24);
                    obj_pos += 4;
                    
                    if (obj_length == 0 || obj_pos + obj_length > section_data_len) break;
                    
                    size_t obj_start = obj_pos;
                    
                    /* Allocate space for this object */
                    if (ctx.object_count >= ctx.object_capacity) {
                        ctx.object_capacity *= 2;
                        ctx.objects = realloc(ctx.objects, ctx.object_capacity * sizeof(JavaObject*));
                        objects = ctx.objects;
                        if (!ctx.objects) goto load_bytes_cleanup;
                    }
                    
                    /* Store NULL placeholder (header at position 0) */
                    JavaObject* parsed = m3g_parse_object(jvm, &ctx, section_data, section_data_len, &obj_pos, obj_type, obj_length);
                    
                    if (obj_type == 0) {
                        /* Header object - do NOT store in array, do NOT increment count.
                         * M3G refs: ref 0 = null, ref N = objects[N-2].
                         * Header is at file position 0/1 but not in the objects array.
                         * Storing it breaks all reference lookups by off-by-one. */
                    } else if (parsed) {
                        ctx.objects[ctx.object_count++] = parsed;
                    } else {
                        /* Unknown/unparsed object - store NULL placeholder */
                        ctx.objects[ctx.object_count++] = NULL;
                    }
                    
                    /* Ensure we advance past the object data */
                    if (obj_pos < obj_start + obj_length) {
                        obj_pos = obj_start + obj_length;
                    }
                }
            }
            
            load_bytes_next_section:
            /* Advance past this section + 4-byte checksum at end */
            fpos = section_data_start + section_data_size + 4;
            section_count++;
            current_section++;
        }
    } else {
        GFX_DEBUG("Loader.load(byte[]): Not a valid M3G file, creating default World");
        fprintf(stderr, "[M3G] Byte array is not valid M3G format, creating default World\n");
    }
    
    /* Link objects (same as in native_loader_load) */
    GFX_DEBUG("Loader.load(byte[]): Linking %d objects...", ctx.object_count);
    for (int i = 0; i < ctx.object_count; i++) {
        JavaObject* obj = ctx.objects[i];
        if (!obj || !obj->header.clazz) continue;
        const char* cn = obj->header.clazz->class_name;
        
        /* Link Mesh */
        if (strstr(cn, "Mesh")) {
            int vb_ref = m3g_get_int_field(obj, "vertexBufferRef", 0);
            int ib_ref = m3g_get_int_field(obj, "indexBufferRef", 0);
            int app_ref = m3g_get_int_field(obj, "appearanceRef", 0);
            int vb_idx = vb_ref - 2, ib_idx = ib_ref - 2, app_idx = app_ref - 2;
            if (vb_ref > 0 && vb_idx >= 0 && vb_idx < ctx.object_count && ctx.objects[vb_idx])
                m3g_set_ref_field(obj, "vertexBuffer", ctx.objects[vb_idx]);
            if (ib_ref > 0 && ib_idx >= 0 && ib_idx < ctx.object_count && ctx.objects[ib_idx])
                m3g_set_ref_field(obj, "indexBuffer", ctx.objects[ib_idx]);
            if (app_ref > 0 && app_idx >= 0 && app_idx < ctx.object_count && ctx.objects[app_idx])
                m3g_set_ref_field(obj, "appearance", ctx.objects[app_idx]);
        }
        /* Link VertexBuffer */
        if (strstr(cn, "VertexBuffer")) {
            int pos_ref = m3g_get_int_field(obj, "positionsRef", 0);
            int norm_ref = m3g_get_int_field(obj, "normalsRef", 0);
            int color_ref = m3g_get_int_field(obj, "colorsRef", 0);
            int tex_ref = m3g_get_int_field(obj, "texCoordsRef", 0);
            if (pos_ref > 0 && pos_ref-2 >= 0 && pos_ref-2 < ctx.object_count && ctx.objects[pos_ref-2]) {
                JavaObject* pobj = ctx.objects[pos_ref-2];
                int pc = m3g_get_int_field(pobj, "componentCount", -1);
                int pvc = m3g_get_int_field(pobj, "vertexCount", -1);
                m3g_set_ref_field(obj, "positions", pobj);
                fprintf(stderr, "[M3G-LINK2] VB %p: positions[ref=%d] -> %p (comp=%d, verts=%d)\n",
                        (void*)obj, pos_ref, (void*)pobj, pc, pvc);
            }
            if (norm_ref > 0 && norm_ref-2 >= 0 && norm_ref-2 < ctx.object_count && ctx.objects[norm_ref-2])
                m3g_set_ref_field(obj, "normals", ctx.objects[norm_ref-2]);
            if (color_ref > 0 && color_ref-2 >= 0 && color_ref-2 < ctx.object_count && ctx.objects[color_ref-2])
                m3g_set_ref_field(obj, "colors", ctx.objects[color_ref-2]);
            if (tex_ref > 0 && tex_ref-2 >= 0 && tex_ref-2 < ctx.object_count && ctx.objects[tex_ref-2])
                m3g_set_ref_field(obj, "texCoords", ctx.objects[tex_ref-2]);
        }
        /* Link Appearance */
        if (strstr(cn, "Appearance")) {
            int mat_ref = m3g_get_int_field(obj, "materialRef", 0);
            int tex_ref = m3g_get_int_field(obj, "textureRef", 0);
            int comp_ref = m3g_get_int_field(obj, "compositingModeRef", 0);
            int fog_ref_val = m3g_get_int_field(obj, "fogRef", 0);
            int poly_ref = m3g_get_int_field(obj, "polygonModeRef", 0);
            if (mat_ref > 0 && mat_ref-2 >= 0 && mat_ref-2 < ctx.object_count && ctx.objects[mat_ref-2])
                m3g_set_ref_field(obj, "material", ctx.objects[mat_ref-2]);
            if (tex_ref > 0 && tex_ref-2 >= 0 && tex_ref-2 < ctx.object_count && ctx.objects[tex_ref-2])
                m3g_set_ref_field(obj, "texture", ctx.objects[tex_ref-2]);
            if (comp_ref > 0 && comp_ref-2 >= 0 && comp_ref-2 < ctx.object_count && ctx.objects[comp_ref-2])
                m3g_set_ref_field(obj, "compositingMode", ctx.objects[comp_ref-2]);
            if (fog_ref_val > 0 && fog_ref_val-2 >= 0 && fog_ref_val-2 < ctx.object_count && ctx.objects[fog_ref_val-2])
                m3g_set_ref_field(obj, "fog", ctx.objects[fog_ref_val-2]);
            if (poly_ref > 0 && poly_ref-2 >= 0 && poly_ref-2 < ctx.object_count && ctx.objects[poly_ref-2])
                m3g_set_ref_field(obj, "polygonMode", ctx.objects[poly_ref-2]);
        }
        /* Link Group/World children */
        if (strstr(cn, "Group") || strstr(cn, "World")) {
            JavaArray* child_refs = (JavaArray*)m3g_get_ref_field(obj, "_childRefs");
            fprintf(stderr, "[M3G-LINK2] %s %p: _childRefs=%p\n", cn, (void*)obj, (void*)child_refs);
            if (child_refs && child_refs->element_type == T_INT) {
                jint* refs = (jint*)array_data(child_refs);
                int child_count = child_refs->length;
                JavaArray* children = (JavaArray*)m3g_get_ref_field(obj, "children");
                if (!children) {
                    children = jvm_new_array(jvm, DESC_OBJECT, child_count > 0 ? child_count : 16, NULL);
                    if (children) m3g_set_ref_field(obj, "children", (JavaObject*)children);
                }
                if (children && children->element_type == DESC_OBJECT) {
                    JavaObject** ca = (JavaObject**)array_data(children);
                    int added = 0;
                    for (int c = 0; c < child_count && added < children->length; c++) {
                        int cr = refs[c];
                        int ci = cr - 2;
                        if (cr > 0 && ci >= 0 && ci < ctx.object_count && ctx.objects[ci]) {
                            ca[added++] = ctx.objects[ci];
                            fprintf(stderr, "[M3G-LINK2] %s %p: child[%d] ref=%d -> %p\n",
                                    cn, (void*)obj, added-1, cr, (void*)ctx.objects[ci]);
                        }
                    }
                    fprintf(stderr, "[M3G-LINK2] %s %p: resolved %d/%d children\n", cn, (void*)obj, added, child_count);
                }
                m3g_set_ref_field(obj, "_childRefs", NULL);
            }
        }
        /* Add Mesh to World */
        if (strstr(cn, "Mesh") && ctx.world) {
            JavaArray* children = (JavaArray*)m3g_get_ref_field(ctx.world, "children");
            if (!children) {
                children = jvm_new_array(jvm, DESC_OBJECT, 16, NULL);
                if (children) m3g_set_ref_field(ctx.world, "children", (JavaObject*)children);
            }
            if (children) {
                JavaObject** ca = (JavaObject**)array_data(children);
                for (jsize j = 0; j < children->length; j++) {
                    if (!ca[j]) { ca[j] = obj; break; }
                }
            }
        }
    }
    
    /* Resolve World camera and background */
    if (ctx.world) {
        int camera_ref = m3g_get_int_field(ctx.world, "activeCameraRef", 0);
        int bg_ref = m3g_get_int_field(ctx.world, "backgroundRef", 0);
        if (camera_ref > 0 && camera_ref-2 >= 0 && camera_ref-2 < ctx.object_count && ctx.objects[camera_ref-2])
            m3g_set_ref_field(ctx.world, "activeCamera", ctx.objects[camera_ref-2]);
        if (bg_ref > 0 && bg_ref-2 >= 0 && bg_ref-2 < ctx.object_count && ctx.objects[bg_ref-2])
            m3g_set_ref_field(ctx.world, "background", ctx.objects[bg_ref-2]);
    }
    
    /* Create World if needed */
    world_class = jvm_load_class(jvm, "javax/microedition/m3g/World");
    if (!world_class) goto load_bytes_cleanup;
    
    if (!ctx.world) {
        ctx.world = jvm_new_object(jvm, world_class);
        if (ctx.world) m3g_set_int_field(ctx.world, "scope", -1);
    }
    if (!ctx.world) goto load_bytes_cleanup;
    
    /* Create default Camera if not found */
    if (!ctx.camera) {
        camera_class = jvm_load_class(jvm, "javax/microedition/m3g/Camera");
        if (camera_class) {
            ctx.camera = jvm_new_object(jvm, camera_class);
            if (ctx.camera) {
                m3g_set_int_field(ctx.camera, "projectionType", 1);
                m3g_set_float_field(ctx.camera, "fov", 60.0f);
                m3g_set_float_field(ctx.camera, "near", 1.0f);
                m3g_set_float_field(ctx.camera, "far", 1000.0f);
            }
        }
    }
    if (ctx.camera) m3g_set_ref_field(ctx.world, "activeCamera", ctx.camera);
    
    /* Create default Background if not found */
    if (!ctx.background) {
        bg_class = jvm_load_class(jvm, "javax/microedition/m3g/Background");
        if (bg_class) {
            ctx.background = jvm_new_object(jvm, bg_class);
            if (ctx.background) m3g_set_int_field(ctx.background, "clearColor", 0xFF202020);
        }
    }
    if (ctx.background) m3g_set_ref_field(ctx.world, "background", ctx.background);
    
    /* Create default Light if not found */
    if (!ctx.light) {
        light_class = jvm_load_class(jvm, "javax/microedition/m3g/Light");
        if (light_class) {
            ctx.light = jvm_new_object(jvm, light_class);
            if (ctx.light) {
                m3g_set_int_field(ctx.light, "lightType", 2);
                m3g_set_int_field(ctx.light, "color", 0xFFFFFFFF);
                m3g_set_float_field(ctx.light, "intensity", 1.0f);
            }
        }
    }
    
    /* Build result array */
    int valid_count = 0;
    for (int i = 0; i < ctx.object_count; i++) {
        if (ctx.objects[i]) valid_count++;
    }
    bool world_in_objects = false;
    for (int i = 0; i < ctx.object_count; i++) {
        if (ctx.objects[i] == ctx.world) { world_in_objects = true; break; }
    }
    if (ctx.world && !world_in_objects) valid_count++;
    if (valid_count == 0 && ctx.world) valid_count = 1;
    
    result = jvm_new_array(jvm, DESC_OBJECT, valid_count, NULL);
    if (result) {
        JavaObject** arr = (JavaObject**)array_data(result);
        int idx = 0;
        for (int i = 0; i < ctx.object_count && idx < valid_count; i++) {
            if (ctx.objects[i]) arr[idx++] = ctx.objects[i];
        }
        if (idx < valid_count && ctx.world) arr[idx] = ctx.world;
    }
    
    GFX_DEBUG("Loader.load(byte[]): returning array[%d]", valid_count);
    fprintf(stderr, "[M3G] Loader.load(byte[]): returning array[%d]\n", valid_count);
    
    /* Cleanup */
    free(file_data);
    if (decompressed) free(decompressed);
    if (objects) free(objects);
    return NATIVE_RETURN_OBJECT(result);

load_bytes_cleanup:
    if (file_data) free(file_data);
    if (decompressed) free(decompressed);
    if (objects) free(objects);
    return NATIVE_RETURN_NULL();
}

/* RayIntersection methods for picking */
static JavaValue native_rayintersection_getDistance(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) return NATIVE_RETURN_FLOAT(0.0f);
    
    /* Get distance from field */
    JavaClass* clazz = obj->header.clazz;
    for (int i = 0; i < clazz->fields_count; i++) {
        if (clazz->fields[i].name && strcmp(clazz->fields[i].name, "distance") == 0) {
            return NATIVE_RETURN_FLOAT(obj->fields[i].f);
        }
    }
    
    return NATIVE_RETURN_FLOAT(0.0f);
}

static JavaValue native_rayintersection_getIntersected(JVM* jvm, JavaThread* thread,
                                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    
    if (!obj) return NATIVE_RETURN_NULL();
    
    JavaClass* clazz = obj->header.clazz;
    for (int i = 0; i < clazz->fields_count; i++) {
        if (clazz->fields[i].name && strcmp(clazz->fields[i].name, "intersected") == 0) {
            return NATIVE_RETURN_OBJECT(obj->fields[i].ref);
        }
    }
    
    return NATIVE_RETURN_NULL();
}

/* Graphics3D.addLight(Light, Transform) */
static JavaValue native_graphics3d_addLight(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* g3d = (JavaObject*)args[0].ref;
    JavaObject* light = (JavaObject*)args[1].ref;
    JavaObject* transform = (JavaObject*)args[2].ref;
    (void)g3d; (void)transform;
    
    if (!light) return NATIVE_RETURN_INT(-1);
    
    /* Auto-register light slot */
    int light_idx = find_light_for_object(light);
    if (light_idx < 0) {
        if (g_m3g.light_count < 8) {
            light_idx = g_m3g.light_count++;
            memset(&g_m3g.lights[light_idx], 0, sizeof(M3GLight));
            g_m3g.lights[light_idx].obj = light;
            g_m3g.lights[light_idx].attenuation[0] = 1.0f;
            g_m3g.lights[light_idx].color[3] = 1.0f;
        } else {
            return NATIVE_RETURN_INT(-1);
        }
    }
    
    return NATIVE_RETURN_INT(light_idx);
}

/* Graphics3D.setLight(int index, Light, Transform) */
static JavaValue native_graphics3d_setLight(JVM* jvm, JavaThread* thread,
                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* g3d = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    JavaObject* light = (JavaObject*)args[2].ref;
    JavaObject* transform = (JavaObject*)args[3].ref;
    (void)g3d; (void)transform;
    
    if (index < 0 || index >= 8) return NATIVE_RETURN_VOID();
    
    if (light) {
        /* Update light at index */
        memset(&g_m3g.lights[index], 0, sizeof(M3GLight));
        g_m3g.lights[index].obj = light;
        
        /* Read light mode (field name "mode" in Java Light, JSR-184 value 128-131) */
        int light_mode = m3g_get_int_field(light, "mode", 129);
        g_m3g.lights[index].type = m3g_light_mode_to_type(light_mode);
        
        /* Read color */
        jint color_val = m3g_get_int_field(light, "color", 0x00FFFFFF);
        g_m3g.lights[index].color[0] = ((color_val >> 16) & 0xFF) / 255.0f;
        g_m3g.lights[index].color[1] = ((color_val >> 8) & 0xFF) / 255.0f;
        g_m3g.lights[index].color[2] = (color_val & 0xFF) / 255.0f;
        
        float intensity = m3g_get_float_field(light, "intensity", 1.0f);
        g_m3g.lights[index].color[3] = intensity;
        
        g_m3g.lights[index].attenuation[0] = m3g_get_float_field(light, "constantAttenuation", 1.0f);
        g_m3g.lights[index].attenuation[1] = m3g_get_float_field(light, "linearAttenuation", 0.0f);
        g_m3g.lights[index].attenuation[2] = m3g_get_float_field(light, "quadraticAttenuation", 0.0f);
        g_m3g.lights[index].spot_angle = m3g_get_float_field(light, "spotAngle", 180.0f);
        g_m3g.lights[index].spot_exponent = m3g_get_float_field(light, "spotExponent", 0.0f);
        
        GFX_DEBUG("Graphics3D.setLight: index=%d mode=%d type=%d", index, light_mode, g_m3g.lights[index].type);
    } else {
        /* Remove light */
        if (index < g_m3g.light_count) {
            g_m3g.lights[index].type = 0;
        }
    }
    
    return NATIVE_RETURN_VOID();
}

/* ============================================================================
 * Animation System (JSR-184)
 * 
 * Implements the M3G animation system:
 * - KeyframeSequence: stores keyframe times and values with interpolation
 * - AnimationController: controls playback speed, position, active interval, weight
 * - AnimationTrack: links a KeyframeSequence to a property of an Object3D
 * - Object3D.animate(worldTime): evaluates all tracks and applies values
 * ============================================================================ */

/* Animation property constants (from JSR-184 AnimationTrack) */
#define M3G_ANIM_ALPHA              256
#define M3G_ANIM_AMBIENT_COLOR     257
#define M3G_ANIM_COLOR             258
#define M3G_ANIM_CROP              259
#define M3G_ANIM_DENSITY           260
#define M3G_ANIM_DIFFUSE_COLOR     261
#define M3G_ANIM_EMISSIVE_COLOR    262
#define M3G_ANIM_FAR_DISTANCE      263
#define M3G_ANIM_FIELD_OF_VIEW     264
#define M3G_ANIM_INTENSITY         265
#define M3G_ANIM_MORPH_WEIGHTS     266
#define M3G_ANIM_NEAR_DISTANCE     267
#define M3G_ANIM_ORIENTATION       268
#define M3G_ANIM_PICKABILITY       269
#define M3G_ANIM_SCALE             270
#define M3G_ANIM_SHININESS         271
#define M3G_ANIM_SPECULAR_COLOR    272
#define M3G_ANIM_SPOT_ANGLE        273
#define M3G_ANIM_SPOT_EXPONENT     274
#define M3G_ANIM_TRANSLATION       275
#define M3G_ANIM_VISIBILITY        276

/* KeyframeSequence interpolation modes */
#define M3G_KF_LINEAR              176
#define M3G_KF_SLERP               177
#define M3G_KF_SPLINE              178
#define M3G_KF_SQUAD               179
#define M3G_KF_STEP                180

/* KeyframeSequence repeat modes */
#define M3G_REPEAT_CONSTANT        192
#define M3G_REPEAT_LOOP            193

/* ---- Internal animation helpers ---- */

/* Check if an AnimationController is active at the given world time */
static int m3g_anim_is_active(JavaObject* controller, jint world_time) {
    jint act_start = m3g_get_int_field(controller, "activationTime", 0);
    jint act_end = m3g_get_int_field(controller, "deactivationTime", 0);
    
    if (act_start == act_end) return 1;  /* Always active if interval is zero */
    return (world_time >= act_start && world_time < act_end);
}

/* Get the current sequence time from an AnimationController */
static jfloat m3g_anim_get_position(JavaObject* controller, jint world_time) {
    jfloat speed = m3g_get_float_field(controller, "speed", 1.0f);
    jint ref_world_time = m3g_get_int_field(controller, "refWorldTime", 0);
    jfloat ref_seq_time = m3g_get_float_field(controller, "refSequenceTime", 0.0f);
    
    return ref_seq_time + speed * ((jfloat)world_time - (jfloat)ref_world_time);
}

/* Get time remaining until controller deactivation */
static jint m3g_anim_time_to_deactivation(JavaObject* controller, jint world_time) {
    jint deact = m3g_get_int_field(controller, "deactivationTime", 0x7FFFFFFF);
    if (world_time < deact) return deact - world_time;
    return 0x7FFFFFFF;
}

/* Sample a KeyframeSequence at the given sequence time using LINEAR interpolation.
 * Returns 1 on success, 0 on error. */
static int m3g_anim_sample_linear(JavaObject* sequence, jint seq_time,
                                   jfloat* out_sample, int max_components) {
    jint num_kf = m3g_get_int_field(sequence, "numKeyframes", 0);
    jint num_comp = m3g_get_int_field(sequence, "numComponents", 0);
    jint duration = m3g_get_int_field(sequence, "duration", 0);
    jint repeat = m3g_get_int_field(sequence, "repeatMode", M3G_REPEAT_CONSTANT);
    jint first_valid = m3g_get_int_field(sequence, "firstValid", 0);
    jint last_valid = m3g_get_int_field(sequence, "lastValid", 0);
    jint interpolation = m3g_get_int_field(sequence, "interpolation", M3G_KF_LINEAR);
    
    if (num_kf <= 0 || num_comp <= 0 || num_comp > max_components) return 0;
    
    JavaArray* kf_times = (JavaArray*)m3g_get_ref_field(sequence, "keyframeTimes");
    JavaArray* kf_values = (JavaArray*)m3g_get_ref_field(sequence, "keyframeValues");
    if (!kf_times || !kf_values) return 0;
    
    jint* times = (jint*)array_data(kf_times);
    jfloat* values = (jfloat*)array_data(kf_values);
    if (!times || !values) return 0;
    
    int closed = (repeat == M3G_REPEAT_LOOP);
    
    /* Map time for closed (looping) sequences */
    jint time = seq_time;
    if (closed && duration > 0) {
        if (time < 0) time = (time % duration) + duration;
        else time = time % duration;
        /* Adjust if before first valid keyframe time */
        if (time < times[first_valid]) time += duration;
    } else {
        /* Open (constant) sequence: clamp to valid range */
        if (time < times[first_valid]) {
            for (int i = 0; i < num_comp; i++) out_sample[i] = values[first_valid * num_comp + i];
            return 1;
        }
        if (time >= times[last_valid]) {
            for (int i = 0; i < num_comp; i++) out_sample[i] = values[last_valid * num_comp + i];
            return 1;
        }
    }
    
    /* Find the keyframe segment */
    int start_kf = first_valid;
    /* Search forward for the segment containing 'time' */
    while (start_kf < last_valid && times[start_kf + 1] <= time) {
        start_kf++;
    }
    
    /* STEP interpolation: just use the start keyframe */
    if (interpolation == M3G_KF_STEP || time == times[start_kf]) {
        for (int i = 0; i < num_comp; i++) out_sample[i] = values[start_kf * num_comp + i];
        return 1;
    }
    
    /* Calculate interpolation factor */
    int end_kf = start_kf + 1;
    if (end_kf >= num_kf) end_kf = 0;  /* Wrap for closed sequences (shouldn't happen for open) */
    
    jint delta = times[end_kf] - times[start_kf];
    if (delta <= 0) {
        /* Coincident keyframes, just use start */
        for (int i = 0; i < num_comp; i++) out_sample[i] = values[start_kf * num_comp + i];
        return 1;
    }
    
    jfloat s = (jfloat)(time - times[start_kf]) / (jfloat)delta;
    
    /* Interpolate based on interpolation mode */
    const jfloat* v_start = &values[start_kf * num_comp];
    const jfloat* v_end = &values[end_kf * num_comp];
    
    switch (interpolation) {
    case M3G_KF_LINEAR: {
        /* Linear interpolation */
        for (int i = 0; i < num_comp; i++) {
            out_sample[i] = v_start[i] + s * (v_end[i] - v_start[i]);
        }
        break;
    }
    case M3G_KF_SLERP: {
        /* Spherical linear interpolation for quaternions (4 components) */
        if (num_comp == 4) {
            /* Compute dot product */
            jfloat dot = 0.0f;
            for (int i = 0; i < 4; i++) dot += v_start[i] * v_end[i];
            
            /* If dot is negative, negate one quaternion to take shorter path */
            jfloat sign = 1.0f;
            if (dot < 0.0f) { sign = -1.0f; dot = -dot; }
            
            /* Clamp dot to avoid numerical issues with acos */
            if (dot > 1.0f) dot = 1.0f;
            
            if (dot < 0.0001f) {
                /* Quaternions are nearly opposite; use simple lerp */
                for (int i = 0; i < 4; i++) {
                    out_sample[i] = v_start[i] + s * (sign * v_end[i] - v_start[i]);
                }
            } else {
                /* Standard SLERP */
                jfloat omega = acosf(dot);
                jfloat sin_omega = sinf(omega);
                jfloat a = sinf((1.0f - s) * omega) / sin_omega;
                jfloat b = sinf(s * omega) / sin_omega * sign;
                for (int i = 0; i < 4; i++) {
                    out_sample[i] = a * v_start[i] + b * v_end[i];
                }
            }
            /* Normalize result */
            jfloat len = sqrtf(out_sample[0]*out_sample[0] + out_sample[1]*out_sample[1] +
                              out_sample[2]*out_sample[2] + out_sample[3]*out_sample[3]);
            if (len > 0.0001f) {
                for (int i = 0; i < 4; i++) out_sample[i] /= len;
            }
        } else {
            /* Fallback to linear for non-4-component */
            for (int i = 0; i < num_comp; i++) {
                out_sample[i] = v_start[i] + s * (v_end[i] - v_start[i]);
            }
        }
        break;
    }
    default:
        /* Fallback to linear for SPLINE, SQUAD (not fully implemented) */
        for (int i = 0; i < num_comp; i++) {
            out_sample[i] = v_start[i] + s * (v_end[i] - v_start[i]);
        }
        break;
    }
    
    return 1;
}

/* Apply an animated property value to an Object3D/Node.
 * The values are already weighted and accumulated by the caller. */
static void m3g_anim_apply_property(JVM* jvm, JavaObject* obj, jint property,
                                     jint num_components, const jfloat* values) {
    if (!obj) return;
    
    const char* class_name = obj->header.clazz ? obj->header.clazz->class_name : NULL;
    if (!class_name) return;
    
    /* Check if this is a Node (or subclass) for transform properties */
    int is_node = (strstr(class_name, "m3g/Node") != NULL ||
                   strstr(class_name, "m3g/Group") != NULL ||
                   strstr(class_name, "m3g/World") != NULL ||
                   strstr(class_name, "m3g/Camera") != NULL ||
                   strstr(class_name, "m3g/Mesh") != NULL ||
                   strstr(class_name, "m3g/Sprite3D") != NULL ||
                   strstr(class_name, "m3g/Light") != NULL ||
                   strstr(class_name, "m3g/MorphingMesh") != NULL ||
                   strstr(class_name, "m3g/SkinnedMesh") != NULL);
    
    (void)jvm;
    
    switch (property) {
    case M3G_ANIM_TRANSLATION:
        if (is_node && num_components >= 3) {
            /* Store translation components */
            JavaArray* trans_arr = (JavaArray*)m3g_get_ref_field(obj, "translation");
            if (trans_arr && trans_arr->element_type == T_FLOAT && trans_arr->length >= 3) {
                jfloat* trans = (jfloat*)array_data(trans_arr);
                trans[0] = values[0];
                trans[1] = values[1];
                trans[2] = values[2];
            }
        }
        break;
        
    case M3G_ANIM_ORIENTATION:
        if (is_node && num_components >= 4) {
            /* Store orientation as quaternion (angle-axis to be computed during render) */
            JavaArray* orient_arr = (JavaArray*)m3g_get_ref_field(obj, "orientation");
            if (orient_arr && orient_arr->element_type == T_FLOAT && orient_arr->length >= 4) {
                jfloat* orient = (jfloat*)array_data(orient_arr);
                orient[0] = values[0];
                orient[1] = values[1];
                orient[2] = values[2];
                orient[3] = values[3];
            }
        }
        break;
        
    case M3G_ANIM_SCALE:
        if (is_node && num_components >= 1) {
            JavaArray* scale_arr = (JavaArray*)m3g_get_ref_field(obj, "scaling");
            if (scale_arr && scale_arr->element_type == T_FLOAT && scale_arr->length >= 3) {
                jfloat* scale = (jfloat*)array_data(scale_arr);
                if (num_components == 1) {
                    scale[0] = values[0];
                    scale[1] = values[0];
                    scale[2] = values[0];
                } else if (num_components >= 3) {
                    scale[0] = values[0];
                    scale[1] = values[1];
                    scale[2] = values[2];
                }
            }
        }
        break;
        
    case M3G_ANIM_ALPHA:
        if (is_node && num_components >= 1) {
            m3g_set_float_field(obj, "alphaFactor", values[0]);
        }
        break;
        
    case M3G_ANIM_VISIBILITY:
        /* Visibility is handled via enableBits internally; for now, set alpha */
        if (is_node && num_components >= 1) {
            /* value < 0.5 means invisible */
            /* For simplicity, we just note the intention; actual culling happens at render */
        }
        break;
        
    case M3G_ANIM_PICKABILITY:
        /* Similar to visibility */
        break;
        
    case M3G_ANIM_FIELD_OF_VIEW:
        if (strstr(class_name, "m3g/Camera") != NULL && num_components >= 1) {
            m3g_set_float_field(obj, "fov", values[0]);
        }
        break;
        
    case M3G_ANIM_INTENSITY:
        if (strstr(class_name, "m3g/Light") != NULL && num_components >= 1) {
            m3g_set_float_field(obj, "intensity", values[0]);
        }
        break;
        
    default:
        /* Other properties not yet implemented */
        break;
    }
}

/* Recursively animate an Object3D and its children.
 * Returns the minimum validity (time until animation state may change), or 0x7FFFFFFF if inactive. */
static jint m3g_animate_object(JVM* jvm, JavaObject* obj, jint world_time) {
    if (!obj) return 0x7FFFFFFF;
    
    jint min_validity = 0x7FFFFFFF;
    
    /* Get animation tracks for this object */
    JavaArray* tracks = (JavaArray*)m3g_get_ref_field(obj, "animTracks");
    if (tracks && tracks->element_type == DESC_OBJECT) {
        jsize track_count = tracks->length;
        JavaObject** track_arr = (JavaObject**)array_data(tracks);
        
        /* Process tracks grouped by property for blending */
        int ti = 0;
        while (ti < track_count) {
            JavaObject* track = track_arr[ti];
            if (!track) { ti++; continue; }
            
            JavaObject* controller = (JavaObject*)m3g_get_ref_field(track, "controller");
            jint property = m3g_get_int_field(track, "propertyId", -1);
            JavaObject* sequence = (JavaObject*)m3g_get_ref_field(track, "sequence");
            
            if (!sequence || property < 0) { ti++; continue; }
            
            jint num_comp = m3g_get_int_field(sequence, "numComponents", 0);
            if (num_comp <= 0 || num_comp > 16) { ti++; continue; }
            
            jfloat accum[16];
            memset(accum, 0, sizeof(accum));
            jfloat sum_weights = 0.0f;
            
            /* Accumulate contributions from all tracks targeting the same property */
            do {
                if (!track) { ti++; break; }
                
                controller = (JavaObject*)m3g_get_ref_field(track, "controller");
                property = m3g_get_int_field(track, "propertyId", -1);
                sequence = (JavaObject*)m3g_get_ref_field(track, "sequence");
                
                if (controller && sequence && m3g_anim_is_active(controller, world_time)) {
                    jfloat weight = m3g_get_float_field(controller, "weight", 1.0f);
                    
                    if (weight > 0.0f) {
                        jfloat sample[16];
                        jint seq_time = (jint)m3g_anim_get_position(controller, world_time);
                        
                        if (m3g_anim_sample_linear(sequence, seq_time, sample, num_comp)) {
                            for (int c = 0; c < num_comp; c++) {
                                accum[c] += sample[c] * weight;
                            }
                            sum_weights += weight;
                        }
                    }
                    
                    jint validity = m3g_anim_time_to_deactivation(controller, world_time);
                    if (validity < min_validity) min_validity = validity;
                }
                
                ti++;
                if (ti < track_count) track = track_arr[ti];
            } while (ti < track_count && track &&
                     m3g_get_int_field(track, "propertyId", -1) == property);
            
            /* Apply the accumulated value */
            if (sum_weights > 0.0f) {
                m3g_anim_apply_property(jvm, obj, property, num_comp, accum);
            }
        }
    }
    
    /* Recurse into children if this is a Group/World */
    const char* class_name = obj->header.clazz ? obj->header.clazz->class_name : "";
    int is_group = (strstr(class_name, "m3g/Group") != NULL || strstr(class_name, "m3g/World") != NULL);
    
    if (is_group) {
        JavaArray* children = (JavaArray*)m3g_get_ref_field(obj, "children");
        if (children && children->element_type == DESC_OBJECT) {
            JavaObject** child_arr = (JavaObject**)array_data(children);
            int ani_safe = m3g_get_int_field(obj, "childCount", (int)children->length);
            if (ani_safe > (int)children->length) ani_safe = (int)children->length;
            for (jsize ci = 0; ci < ani_safe; ci++) {
                JavaObject* child = child_arr[ci];
                if (child) {
                    jint child_validity = m3g_animate_object(jvm, child, world_time);
                    if (child_validity < min_validity) min_validity = child_validity;
                }
            }
        }
    }
    
    return min_validity;
}

/* ---- AnimationTrack native methods ---- */

static JavaValue native_animationtrack_setController(JVM* jvm, JavaThread* thread,
                                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* track = (JavaObject*)args[0].ref;
    JavaObject* controller = (JavaObject*)args[1].ref;
    
    if (track) {
        m3g_set_ref_field(track, "controller", controller);
        GFX_DEBUG("AnimationTrack.setController: track=%p controller=%p", (void*)track, (void*)controller);
    }
    return NATIVE_RETURN_VOID();
}

static JavaValue native_animationtrack_getTargetProperty(JVM* jvm, JavaThread* thread,
                                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* track = (JavaObject*)args[0].ref;
    
    if (!track) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(m3g_get_int_field(track, "propertyId", 0));
}

/* ---- KeyframeSequence native methods ---- */

static JavaValue native_keyframesequence_setKeyframe(JVM* jvm, JavaThread* thread,
                                                      JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* seq = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    jint time = args[2].i;
    JavaArray* value_arr = (JavaArray*)args[3].ref;
    
    if (!seq || !value_arr) return NATIVE_RETURN_VOID();
    
    jint num_kf = m3g_get_int_field(seq, "numKeyframes", 0);
    jint num_comp = m3g_get_int_field(seq, "numComponents", 0);
    
    if (index < 0 || index >= num_kf) return NATIVE_RETURN_VOID();
    
    /* Get or create the keyframe times and values arrays */
    JavaArray* kf_times = (JavaArray*)m3g_get_ref_field(seq, "keyframeTimes");
    JavaArray* kf_values = (JavaArray*)m3g_get_ref_field(seq, "keyframeValues");
    
    if (!kf_times) {
        kf_times = jvm_new_array(jvm, T_INT, num_kf, NULL);
        m3g_set_ref_field(seq, "keyframeTimes", (JavaObject*)kf_times);
    }
    if (!kf_values) {
        kf_values = jvm_new_array(jvm, T_FLOAT, num_kf * num_comp, NULL);
        m3g_set_ref_field(seq, "keyframeValues", (JavaObject*)kf_values);
    }
    
    if (kf_times && kf_values) {
        jint* times = (jint*)array_data(kf_times);
        jfloat* values = (jfloat*)array_data(kf_values);
        
        times[index] = time;
        
        jint copy_count = value_arr->length < num_comp ? value_arr->length : num_comp;
        if (value_arr->element_type == T_FLOAT) {
            jfloat* src = (jfloat*)array_data(value_arr);
            for (jint i = 0; i < copy_count; i++) {
                values[index * num_comp + i] = src[i];
            }
        }
        
        /* For SLERP/SQUAD, normalize quaternion keyframes */
        jint interpolation = m3g_get_int_field(seq, "interpolation", M3G_KF_LINEAR);
        if ((interpolation == M3G_KF_SLERP || interpolation == M3G_KF_SQUAD) && num_comp == 4) {
            jfloat* q = &values[index * 4];
            jfloat len = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
            if (len > 0.0001f) {
                q[0] /= len; q[1] /= len; q[2] /= len; q[3] /= len;
            }
        }
    }
    
    GFX_DEBUG("KeyframeSequence.setKeyframe: index=%d, time=%d", index, time);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_keyframesequence_getDuration(JVM* jvm, JavaThread* thread,
                                                     JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* seq = (JavaObject*)args[0].ref;
    if (!seq) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(m3g_get_int_field(seq, "duration", 0));
}

static JavaValue native_keyframesequence_setDuration(JVM* jvm, JavaThread* thread,
                                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* seq = (JavaObject*)args[0].ref;
    jint duration = args[1].i;
    if (!seq) return NATIVE_RETURN_VOID();
    m3g_set_int_field(seq, "duration", duration);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_keyframesequence_getRepeatMode(JVM* jvm, JavaThread* thread,
                                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* seq = (JavaObject*)args[0].ref;
    if (!seq) return NATIVE_RETURN_INT(M3G_REPEAT_CONSTANT);
    return NATIVE_RETURN_INT(m3g_get_int_field(seq, "repeatMode", M3G_REPEAT_CONSTANT));
}

static JavaValue native_keyframesequence_setRepeatMode(JVM* jvm, JavaThread* thread,
                                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* seq = (JavaObject*)args[0].ref;
    jint mode = args[1].i;
    if (!seq) return NATIVE_RETURN_VOID();
    m3g_set_int_field(seq, "repeatMode", mode);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_keyframesequence_getComponentCount(JVM* jvm, JavaThread* thread,
                                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* seq = (JavaObject*)args[0].ref;
    if (!seq) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(m3g_get_int_field(seq, "numComponents", 0));
}

static JavaValue native_keyframesequence_getKeyframeCount(JVM* jvm, JavaThread* thread,
                                                          JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* seq = (JavaObject*)args[0].ref;
    if (!seq) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(m3g_get_int_field(seq, "numKeyframes", 0));
}

static JavaValue native_keyframesequence_getInterpolationType(JVM* jvm, JavaThread* thread,
                                                               JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* seq = (JavaObject*)args[0].ref;
    if (!seq) return NATIVE_RETURN_INT(M3G_KF_LINEAR);
    return NATIVE_RETURN_INT(m3g_get_int_field(seq, "interpolation", M3G_KF_LINEAR));
}

static JavaValue native_keyframesequence_getKeyframe(JVM* jvm, JavaThread* thread,
                                                      JavaValue* args, int arg_count) {
    (void)thread;
    JavaObject* seq = (JavaObject*)args[0].ref;
    jint frame_index = args[1].i;
    JavaArray* value_arr = (JavaArray*)args[2].ref;
    
    if (!seq) return NATIVE_RETURN_INT(0);
    
    jint num_kf = m3g_get_int_field(seq, "numKeyframes", 0);
    jint num_comp = m3g_get_int_field(seq, "numComponents", 0);
    
    if (frame_index < 0 || frame_index >= num_kf) return NATIVE_RETURN_INT(0);
    
    /* Get the time */
    JavaArray* kf_times = (JavaArray*)m3g_get_ref_field(seq, "keyframeTimes");
    jint time = 0;
    if (kf_times) {
        jint* times = (jint*)array_data(kf_times);
        time = times[frame_index];
    }
    
    /* Copy values to output array */
    if (value_arr && value_arr->element_type == T_FLOAT) {
        JavaArray* kf_values = (JavaArray*)m3g_get_ref_field(seq, "keyframeValues");
        if (kf_values) {
            jfloat* values = (jfloat*)array_data(kf_values);
            jfloat* out = (jfloat*)array_data(value_arr);
            jint copy_count = value_arr->length < num_comp ? value_arr->length : num_comp;
            for (jint i = 0; i < copy_count; i++) {
                out[i] = values[frame_index * num_comp + i];
            }
        }
    }
    
    (void)jvm; (void)arg_count;
    return NATIVE_RETURN_INT(time);
}

static JavaValue native_keyframesequence_setValidRange(JVM* jvm, JavaThread* thread,
                                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* seq = (JavaObject*)args[0].ref;
    jint first = args[1].i;
    jint last = args[2].i;
    if (!seq) return NATIVE_RETURN_VOID();
    m3g_set_int_field(seq, "firstValid", first);
    m3g_set_int_field(seq, "lastValid", last);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_keyframesequence_getValidRangeFirst(JVM* jvm, JavaThread* thread,
                                                             JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* seq = (JavaObject*)args[0].ref;
    if (!seq) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(m3g_get_int_field(seq, "firstValid", 0));
}

static JavaValue native_keyframesequence_getValidRangeLast(JVM* jvm, JavaThread* thread,
                                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* seq = (JavaObject*)args[0].ref;
    if (!seq) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(m3g_get_int_field(seq, "lastValid", 0));
}

/* ---- AnimationController native methods ---- */

static JavaValue native_animationcontroller_setActiveInterval(JVM* jvm, JavaThread* thread,
                                                                JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ctrl = (JavaObject*)args[0].ref;
    jint start = args[1].i;
    jint end = args[2].i;
    if (!ctrl) return NATIVE_RETURN_VOID();
    m3g_set_int_field(ctrl, "activationTime", start);
    m3g_set_int_field(ctrl, "deactivationTime", end);
    GFX_DEBUG("AnimationController.setActiveInterval: %d - %d", start, end);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_animationcontroller_getActiveIntervalStart(JVM* jvm, JavaThread* thread,
                                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ctrl = (JavaObject*)args[0].ref;
    if (!ctrl) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(m3g_get_int_field(ctrl, "activationTime", 0));
}

static JavaValue native_animationcontroller_getActiveIntervalEnd(JVM* jvm, JavaThread* thread,
                                                                  JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ctrl = (JavaObject*)args[0].ref;
    if (!ctrl) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(m3g_get_int_field(ctrl, "deactivationTime", 0));
}

static JavaValue native_animationcontroller_setSpeed(JVM* jvm, JavaThread* thread,
                                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ctrl = (JavaObject*)args[0].ref;
    jfloat factor = args[1].f;
    jint world_time = args[2].i;
    if (!ctrl) return NATIVE_RETURN_VOID();
    
    /* Update ref time so position remains continuous */
    jfloat old_pos = m3g_anim_get_position(ctrl, world_time);
    m3g_set_int_field(ctrl, "refWorldTime", world_time);
    m3g_set_float_field(ctrl, "refSequenceTime", old_pos);
    m3g_set_float_field(ctrl, "speed", factor);
    
    GFX_DEBUG("AnimationController.setSpeed: %.2f at worldTime=%d", factor, world_time);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_animationcontroller_getSpeed(JVM* jvm, JavaThread* thread,
                                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ctrl = (JavaObject*)args[0].ref;
    if (!ctrl) return NATIVE_RETURN_FLOAT(1.0f);
    return NATIVE_RETURN_FLOAT(m3g_get_float_field(ctrl, "speed", 1.0f));
}

static JavaValue native_animationcontroller_setPosition(JVM* jvm, JavaThread* thread,
                                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ctrl = (JavaObject*)args[0].ref;
    jfloat seq_time = args[1].f;
    jint world_time = args[2].i;
    if (!ctrl) return NATIVE_RETURN_VOID();
    m3g_set_int_field(ctrl, "refWorldTime", world_time);
    m3g_set_float_field(ctrl, "refSequenceTime", seq_time);
    GFX_DEBUG("AnimationController.setPosition: seqTime=%.2f at worldTime=%d", seq_time, world_time);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_animationcontroller_getPosition(JVM* jvm, JavaThread* thread,
                                                        JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ctrl = (JavaObject*)args[0].ref;
    jint world_time = args[1].i;
    if (!ctrl) return NATIVE_RETURN_FLOAT(0.0f);
    return NATIVE_RETURN_FLOAT(m3g_anim_get_position(ctrl, world_time));
}

static JavaValue native_animationcontroller_setWeight(JVM* jvm, JavaThread* thread,
                                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ctrl = (JavaObject*)args[0].ref;
    jfloat weight = args[1].f;
    if (!ctrl) return NATIVE_RETURN_VOID();
    m3g_set_float_field(ctrl, "weight", weight);
    GFX_DEBUG("AnimationController.setWeight: %.2f", weight);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_animationcontroller_getWeight(JVM* jvm, JavaThread* thread,
                                                      JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ctrl = (JavaObject*)args[0].ref;
    if (!ctrl) return NATIVE_RETURN_FLOAT(1.0f);
    return NATIVE_RETURN_FLOAT(m3g_get_float_field(ctrl, "weight", 1.0f));
}

static JavaValue native_animationcontroller_getRefWorldTime(JVM* jvm, JavaThread* thread,
                                                            JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* ctrl = (JavaObject*)args[0].ref;
    if (!ctrl) return NATIVE_RETURN_INT(0);
    return NATIVE_RETURN_INT(m3g_get_int_field(ctrl, "refWorldTime", 0));
}

/* ---- Object3D animation native methods ---- */

static JavaValue native_object3d_addAnimationTrack(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* track = (JavaObject*)args[1].ref;
    
    if (!obj || !track) return NATIVE_RETURN_VOID();
    
    /* Get or create the animTracks array */
    JavaArray* tracks = (JavaArray*)m3g_get_ref_field(obj, "animTracks");
    if (!tracks) {
        /* Allocate initial array for up to 8 tracks */
        tracks = jvm_new_array(jvm, DESC_OBJECT, 8, NULL);
        m3g_set_ref_field(obj, "animTracks", (JavaObject*)tracks);
    }
    
    if (tracks && tracks->element_type == DESC_OBJECT) {
        /* Find empty slot or expand */
        JavaObject** arr = (JavaObject**)array_data(tracks);
        jsize len = tracks->length;
        jsize empty = -1;
        for (jsize i = 0; i < len; i++) {
            if (arr[i] == NULL) { empty = i; break; }
        }
        
        if (empty >= 0) {
            arr[empty] = track;
        } else {
            /* Expand array (simple realloc) */
            /* For now, just print warning if full */
            GFX_DEBUG("Object3D.addAnimationTrack: track array full (max %d)", len);
        }
    }
    
    GFX_DEBUG("Object3D.addAnimationTrack: obj=%p track=%p", (void*)obj, (void*)track);
    return NATIVE_RETURN_VOID();
}

static JavaValue native_object3d_removeAnimationTrack(JVM* jvm, JavaThread* thread,
                                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* track = (JavaObject*)args[1].ref;
    
    if (!obj || !track) return NATIVE_RETURN_VOID();
    
    JavaArray* tracks = (JavaArray*)m3g_get_ref_field(obj, "animTracks");
    if (tracks && tracks->element_type == DESC_OBJECT) {
        JavaObject** arr = (JavaObject**)array_data(tracks);
        for (jsize i = 0; i < tracks->length; i++) {
            if (arr[i] == track) {
                arr[i] = NULL;
                break;
            }
        }
    }
    
    GFX_DEBUG("Object3D.removeAnimationTrack");
    return NATIVE_RETURN_VOID();
}

static JavaValue native_object3d_getAnimationTrackCount(JVM* jvm, JavaThread* thread,
                                                         JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    if (!obj) return NATIVE_RETURN_INT(0);
    
    jint count = 0;
    JavaArray* tracks = (JavaArray*)m3g_get_ref_field(obj, "animTracks");
    if (tracks && tracks->element_type == DESC_OBJECT) {
        JavaObject** arr = (JavaObject**)array_data(tracks);
        for (jsize i = 0; i < tracks->length; i++) {
            if (arr[i] != NULL) count++;
        }
    }
    return NATIVE_RETURN_INT(count);
}

static JavaValue native_object3d_getAnimationTrack(JVM* jvm, JavaThread* thread,
                                                    JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint index = args[1].i;
    
    if (!obj) return NATIVE_RETURN_NULL();
    
    JavaArray* tracks = (JavaArray*)m3g_get_ref_field(obj, "animTracks");
    if (tracks && tracks->element_type == DESC_OBJECT) {
        JavaObject** arr = (JavaObject**)array_data(tracks);
        /* Count non-null entries to find by index */
        jint count = 0;
        for (jsize i = 0; i < tracks->length; i++) {
            if (arr[i] != NULL) {
                if (count == index) return NATIVE_RETURN_OBJECT(arr[i]);
                count++;
            }
        }
    }
    return NATIVE_RETURN_NULL();
}

static JavaValue native_object3d_animate(JVM* jvm, JavaThread* thread,
                                          JavaValue* args, int arg_count) {
    (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jint world_time = args[1].i;
    
    if (!obj) return NATIVE_RETURN_INT(0x7FFFFFFF);
    
    jint validity = m3g_animate_object(jvm, obj, world_time);
    
    GFX_DEBUG("Object3D.animate: worldTime=%d validity=%d", world_time, validity);
    return NATIVE_RETURN_INT(validity);
}

/* Sprite3D methods */
static JavaValue native_sprite3d_init(JVM* jvm, JavaThread* thread,
                                       JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    jboolean scaled = args[1].i;
    JavaObject* image = (JavaObject*)args[2].ref;
    JavaObject* appearance = (JavaObject*)args[3].ref;
    (void)obj; (void)scaled; (void)image; (void)appearance;
    
    GFX_DEBUG("Sprite3D.<init>");
    return NATIVE_RETURN_VOID();
}

static JavaValue native_sprite3d_setImage(JVM* jvm, JavaThread* thread,
                                           JavaValue* args, int arg_count) {
    (void)jvm; (void)thread; (void)arg_count;
    JavaObject* obj = (JavaObject*)args[0].ref;
    JavaObject* image = (JavaObject*)args[1].ref;
    (void)obj; (void)image;
    
    GFX_DEBUG("Sprite3D.setImage");
    return NATIVE_RETURN_VOID();
}

/* ============================================================================
 * Stub Class Registration for M3G Classes
 * ============================================================================ */

/* Helper: Add an instance field to a JavaClass (for M3G stub classes) */
static void m3g_add_instance_field(JavaClass* clazz, const char* name,
                                    const char* descriptor, uint16_t access_flags) {
    if (!clazz || !name || !descriptor) return;

    /* GUARD: Skip if a field with this name already exists (prevent duplicates).
     * stubs.c:init_stub_classes() used to add the same fields, causing duplicates
     * that doubled instance_size and shifted all slot indices. */
    for (int i = 0; i < clazz->fields_count; i++) {
        if (clazz->fields[i].name && strcmp(clazz->fields[i].name, name) == 0) {
            return;  /* Field already exists — skip */
        }
    }

    /* Allocate or extend fields array */
    int new_count = clazz->fields_count + 1;
    JavaField* new_fields = (JavaField*)realloc(clazz->fields, new_count * sizeof(JavaField));
    if (!new_fields) return;

    clazz->fields = new_fields;
    JavaField* field = &clazz->fields[clazz->fields_count];
    memset(field, 0, sizeof(JavaField));
    field->name = strdup(name);
    field->descriptor = strdup(descriptor);
    field->access_flags = access_flags;
    clazz->fields_count = new_count;
}

/* Recalculate instance_size for an M3G class including inherited fields.
 * Must be called after adding fields and after superclass is loaded.
 * IMPORTANT: processes classes in parent-before-child order. */
static void m3g_recalc_instance_size(JVM* jvm, JavaClass* clazz) {
    if (!clazz) return;

    /* Ensure superclass is loaded */
    if (clazz->super_class_name && !clazz->super_class) {
        clazz->super_class = jvm_load_class(jvm, clazz->super_class_name);
    }

    /* Start with superclass size */
    size_t size = sizeof(ObjectHeader);
    if (clazz->super_class) {
        m3g_recalc_instance_size(jvm, clazz->super_class);
        if (clazz->super_class->instance_size >= sizeof(ObjectHeader)) {
            size = clazz->super_class->instance_size;
        }
    }

    /* Add own instance fields */
    if (clazz->fields) {
        for (int i = 0; i < clazz->fields_count; i++) {
            JavaField* field = &clazz->fields[i];
            if (field->access_flags & ACC_STATIC) continue;
            size += sizeof(JavaValue);
            if (field->descriptor &&
                (field->descriptor[0] == 'J' || field->descriptor[0] == 'D')) {
                size += sizeof(JavaValue);
            }
        }
    }

    clazz->instance_size = size;
}

/* Register M3G stub classes WITH their instance fields.
 *
 * CRITICAL FIX: M3G stub classes created by get_or_create_stub_class() have
 * NO Java fields (fields_count=0, instance_size=sizeof(ObjectHeader)). This
 * means m3g_find_field_slot() always returns -1, and ALL m3g_get/set_field
 * calls silently do nothing. This is the root cause of:
 *   - addChild() children not persisting (m3g_set_ref_field("children") no-op)
 *   - renderNode finding children=NULL, count=-1
 *   - bindTarget/render never working properly
 *   - All M3G state being lost between calls
 *
 * The fix: explicitly add the instance fields needed by the M3G native code
 * to each stub class, then recalculate instance_size so jvm_new_object()
 * allocates enough space for the fields array.
 */
int init_m3g_stub_classes(JVM* jvm) {
    if (!jvm) return 0;

    extern JavaClass* get_or_create_stub_class(JVM* jvm, const char* class_name);

    JavaClass* stub;
    int count = 0;

    /* ========================================================================
     * CRITICAL FIX: Proper M3G class hierarchy setup
     * ========================================================================
     * get_or_create_stub_class() defaults super_class to java/lang/Object
     * for all classes not in its hardcoded switch. This causes:
     *   - m3g_find_field_slot() to compute wrong slot indices (flat hierarchy)
     *   - m3g_recalc_instance_size() to compute wrong instance_size
     *   - Objects allocated too small → heap corruption when writing fields
     *   - "Free Heap block modified after freed" crashes
     *
     * The fix: create all stubs FIRST, then set correct super_class pointers,
     * then add fields and recalc sizes. The hierarchy is:
     *   java/lang/Object
     *     → Object3D
     *       → Transformable
     *       → Node (includes Transformable fields, skips Transformable class)
     *         → Group
     *           → World
     *         → Camera
     *         → Light
     *         → Mesh
     *           → MorphingMesh
     *           → SkinnedMesh
     *         → Sprite3D
     *       → Background
     *       → Material
     *       → Texture2D
     *       → Image2D
     *     → VertexArray
     *     → VertexBuffer
     *     → IndexBuffer
     *       → TriangleStripArray
     *     → AnimationTrack
     *     → AnimationController
     *     → KeyframeSequence
     *     → CompositingMode
     *     → Fog
     *     → Appearance
     *     → Transform
     *     → RayIntersection
     *     → Graphics3D
     *     → Loader
     * ======================================================================== */

    /* === Phase 1: Create all M3G stub classes === */
    JavaClass* cls_object3d       = get_or_create_stub_class(jvm, "javax/microedition/m3g/Object3D");
    JavaClass* cls_transformable  = get_or_create_stub_class(jvm, "javax/microedition/m3g/Transformable");
    JavaClass* cls_node           = get_or_create_stub_class(jvm, "javax/microedition/m3g/Node");
    JavaClass* cls_group          = get_or_create_stub_class(jvm, "javax/microedition/m3g/Group");
    JavaClass* cls_world          = get_or_create_stub_class(jvm, "javax/microedition/m3g/World");
    JavaClass* cls_camera         = get_or_create_stub_class(jvm, "javax/microedition/m3g/Camera");
    JavaClass* cls_light          = get_or_create_stub_class(jvm, "javax/microedition/m3g/Light");
    JavaClass* cls_mesh           = get_or_create_stub_class(jvm, "javax/microedition/m3g/Mesh");
    JavaClass* cls_morphing_mesh  = get_or_create_stub_class(jvm, "javax/microedition/m3g/MorphingMesh");
    JavaClass* cls_skinned_mesh   = get_or_create_stub_class(jvm, "javax/microedition/m3g/SkinnedMesh");
    JavaClass* cls_sprite3d       = get_or_create_stub_class(jvm, "javax/microedition/m3g/Sprite3D");
    JavaClass* cls_background     = get_or_create_stub_class(jvm, "javax/microedition/m3g/Background");
    JavaClass* cls_material       = get_or_create_stub_class(jvm, "javax/microedition/m3g/Material");
    JavaClass* cls_texture2d      = get_or_create_stub_class(jvm, "javax/microedition/m3g/Texture2D");
    JavaClass* cls_image2d        = get_or_create_stub_class(jvm, "javax/microedition/m3g/Image2D");
    JavaClass* cls_vertex_array   = get_or_create_stub_class(jvm, "javax/microedition/m3g/VertexArray");
    JavaClass* cls_vertex_buffer  = get_or_create_stub_class(jvm, "javax/microedition/m3g/VertexBuffer");
    JavaClass* cls_index_buffer   = get_or_create_stub_class(jvm, "javax/microedition/m3g/IndexBuffer");
    JavaClass* cls_tri_strip_arr  = get_or_create_stub_class(jvm, "javax/microedition/m3g/TriangleStripArray");
    JavaClass* cls_anim_track     = get_or_create_stub_class(jvm, "javax/microedition/m3g/AnimationTrack");
    JavaClass* cls_anim_ctrl      = get_or_create_stub_class(jvm, "javax/microedition/m3g/AnimationController");
    JavaClass* cls_keyframe_seq   = get_or_create_stub_class(jvm, "javax/microedition/m3g/KeyframeSequence");
    JavaClass* cls_compositing    = get_or_create_stub_class(jvm, "javax/microedition/m3g/CompositingMode");
    JavaClass* cls_fog            = get_or_create_stub_class(jvm, "javax/microedition/m3g/Fog");
    JavaClass* cls_appearance     = get_or_create_stub_class(jvm, "javax/microedition/m3g/Appearance");
    JavaClass* cls_transform      = get_or_create_stub_class(jvm, "javax/microedition/m3g/Transform");
    JavaClass* cls_ray_intersect  = get_or_create_stub_class(jvm, "javax/microedition/m3g/RayIntersection");
    JavaClass* cls_graphics3d     = get_or_create_stub_class(jvm, "javax/microedition/m3g/Graphics3D");
    JavaClass* cls_loader         = get_or_create_stub_class(jvm, "javax/microedition/m3g/Loader");

    /* === Phase 2: Set up correct M3G class hierarchy === */
    /* Object3D extends java/lang/Object (keep default) */

    /* Transformable extends Object3D */
    if (cls_transformable && cls_object3d) {
        cls_transformable->super_class = cls_object3d;
    }

    /* Node extends Object3D (not Transformable — Node incorporates
     * Transformable's fields directly to match the JVM class layout) */
    if (cls_node && cls_object3d) {
        cls_node->super_class = cls_object3d;
    }

    /* Group extends Node */
    if (cls_group && cls_node) {
        cls_group->super_class = cls_node;
    }

    /* World extends Group */
    if (cls_world && cls_group) {
        cls_world->super_class = cls_group;
    }

    /* Camera extends Node */
    if (cls_camera && cls_node) {
        cls_camera->super_class = cls_node;
    }

    /* Light extends Node */
    if (cls_light && cls_node) {
        cls_light->super_class = cls_node;
    }

    /* Mesh extends Node */
    if (cls_mesh && cls_node) {
        cls_mesh->super_class = cls_node;
    }

    /* MorphingMesh extends Mesh */
    if (cls_morphing_mesh && cls_mesh) {
        cls_morphing_mesh->super_class = cls_mesh;
    }

    /* SkinnedMesh extends Mesh */
    if (cls_skinned_mesh && cls_mesh) {
        cls_skinned_mesh->super_class = cls_mesh;
    }

    /* Sprite3D extends Node */
    if (cls_sprite3d && cls_node) {
        cls_sprite3d->super_class = cls_node;
    }

    /* Background extends Object3D */
    if (cls_background && cls_object3d) {
        cls_background->super_class = cls_object3d;
    }

    /* Material extends Object3D */
    if (cls_material && cls_object3d) {
        cls_material->super_class = cls_object3d;
    }

    /* Texture2D extends Object3D */
    if (cls_texture2d && cls_object3d) {
        cls_texture2d->super_class = cls_object3d;
    }

    /* Image2D extends Object3D */
    if (cls_image2d && cls_object3d) {
        cls_image2d->super_class = cls_object3d;
    }

    /* TriangleStripArray extends IndexBuffer */
    if (cls_tri_strip_arr && cls_index_buffer) {
        cls_tri_strip_arr->super_class = cls_index_buffer;
    }

    /* Appearance extends Object3D */
    if (cls_appearance && cls_object3d) {
        cls_appearance->super_class = cls_object3d;
    }

    /* CompositingMode extends Object3D */
    if (cls_compositing && cls_object3d) {
        cls_compositing->super_class = cls_object3d;
    }

    /* Fog extends Object3D */
    if (cls_fog && cls_object3d) {
        cls_fog->super_class = cls_object3d;
    }

    /* VertexArray extends Object3D */
    if (cls_vertex_array && cls_object3d) {
        cls_vertex_array->super_class = cls_object3d;
    }

    /* VertexBuffer extends Object3D */
    if (cls_vertex_buffer && cls_object3d) {
        cls_vertex_buffer->super_class = cls_object3d;
    }

    /* IndexBuffer extends Object3D */
    if (cls_index_buffer && cls_object3d) {
        cls_index_buffer->super_class = cls_object3d;
    }

    /* AnimationTrack extends Object3D */
    if (cls_anim_track && cls_object3d) {
        cls_anim_track->super_class = cls_object3d;
    }

    /* AnimationController extends Object3D */
    if (cls_anim_ctrl && cls_object3d) {
        cls_anim_ctrl->super_class = cls_object3d;
    }

    /* KeyframeSequence extends Object3D */
    if (cls_keyframe_seq && cls_object3d) {
        cls_keyframe_seq->super_class = cls_object3d;
    }

    /* Log the hierarchy for debugging */
    fprintf(stderr, "[M3G] Class hierarchy:\n");
    if (cls_object3d) fprintf(stderr, "  Object3D → %s (instance_size=%zu, fields=%d)\n",
        cls_object3d->super_class ? cls_object3d->super_class->class_name : "none",
        cls_object3d->instance_size, cls_object3d->fields_count);
    if (cls_node) fprintf(stderr, "  Node → %s (instance_size=%zu, fields=%d)\n",
        cls_node->super_class ? cls_node->super_class->class_name : "none",
        cls_node->instance_size, cls_node->fields_count);
    if (cls_group) fprintf(stderr, "  Group → %s (instance_size=%zu, fields=%d)\n",
        cls_group->super_class ? cls_group->super_class->class_name : "none",
        cls_group->instance_size, cls_group->fields_count);
    if (cls_world) fprintf(stderr, "  World → %s (instance_size=%zu, fields=%d)\n",
        cls_world->super_class ? cls_world->super_class->class_name : "none",
        cls_world->instance_size, cls_world->fields_count);

    /* === Phase 3: Add instance fields (from root to leaves) === */

    /* === 1. Object3D (parent: java/lang/Object) === */
    stub = cls_object3d;
    if (stub) {
        m3g_add_instance_field(stub, "userID",  "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "animTracks", "[Ljavax/microedition/m3g/AnimationTrack;", ACC_PUBLIC);
        m3g_recalc_instance_size(jvm, stub);
        count++;
    }

    /* === 2. Transformable (parent: Object3D) ===
     * Note: Node in stubs extends Object3D directly (not Transformable),
     * so Transformable's fields go onto Node below. But we still create
     * the class for getReferences()/duplicate() support. */
    stub = cls_transformable;
    if (stub) {
        m3g_add_instance_field(stub, "transform",      "[F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "translationX",    "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "translationY",    "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "translationZ",    "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "scaleX",          "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "scaleY",          "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "scaleZ",          "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "orientationAngle", "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "orientationX",    "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "orientationY",    "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "orientationZ",    "F", ACC_PUBLIC);
        m3g_recalc_instance_size(jvm, stub);
        count++;
    }

    /* === 3. Node (parent: Object3D) - includes Transformable fields === */
    stub = cls_node;
    if (stub) {
        /* Transformable fields (since Node skips Transformable in stubs) */
        m3g_add_instance_field(stub, "transform",       "[F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "translationX",     "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "translationY",     "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "translationZ",     "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "scaleX",           "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "scaleY",           "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "scaleZ",           "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "orientationAngle", "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "orientationX",     "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "orientationY",     "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "orientationZ",     "F", ACC_PUBLIC);
        /* Node own fields */
        m3g_add_instance_field(stub, "renderingEnable",  "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "pickingEnable",    "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "alphaFactor",      "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "scope",            "I", ACC_PUBLIC);
        m3g_recalc_instance_size(jvm, stub);
        count++;
    }

    /* === 4. Group (parent: Node) === */
    stub = cls_group;
    if (stub) {
        m3g_add_instance_field(stub, "children",    "[Ljavax/microedition/m3g/Node;", ACC_PUBLIC);
        m3g_add_instance_field(stub, "childCount",  "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "_childRefs",  "[I", ACC_PUBLIC);  /* Temporary: int array of child refs from M3G binary */
        m3g_recalc_instance_size(jvm, stub);
        count++;
    }

    /* === 5. World (parent: Group) === */
    stub = cls_world;
    if (stub) {
        m3g_add_instance_field(stub, "activeCamera",     "Ljavax/microedition/m3g/Camera;", ACC_PUBLIC);
        m3g_add_instance_field(stub, "background",       "Ljavax/microedition/m3g/Background;", ACC_PUBLIC);
        m3g_add_instance_field(stub, "activeCameraRef",  "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "backgroundRef",    "I", ACC_PUBLIC);
        m3g_recalc_instance_size(jvm, stub);
        count++;
    }

    /* === 6. Camera (parent: Node) === */
    stub = cls_camera;
    if (stub) {
        m3g_add_instance_field(stub, "projectionType",  "I",  ACC_PUBLIC);
        m3g_add_instance_field(stub, "fov",             "F",  ACC_PUBLIC);
        m3g_add_instance_field(stub, "aspect",          "F",  ACC_PUBLIC);
        m3g_add_instance_field(stub, "near",            "F",  ACC_PUBLIC);
        m3g_add_instance_field(stub, "far",             "F",  ACC_PUBLIC);
        m3g_add_instance_field(stub, "genericMatrix",   "[F", ACC_PUBLIC);
        m3g_recalc_instance_size(jvm, stub);
        count++;
    }

    /* === 7. Mesh (parent: Node) === */
    stub = cls_mesh;
    if (stub) {
        m3g_add_instance_field(stub, "vertexBuffer",    "Ljavax/microedition/m3g/VertexBuffer;", ACC_PUBLIC);
        m3g_add_instance_field(stub, "indexBuffer",     "Ljavax/microedition/m3g/IndexBuffer;", ACC_PUBLIC);
        m3g_add_instance_field(stub, "submeshCount",    "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "appearance",      "Ljavax/microedition/m3g/Appearance;", ACC_PUBLIC);
        m3g_add_instance_field(stub, "morphTargets",    "[Ljavax/microedition/m3g/VertexBuffer;", ACC_PUBLIC);
        /* Temporary: M3G binary parser writes refs as ints, linker resolves to objects above */
        m3g_add_instance_field(stub, "vertexBufferRef",  "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "indexBufferRef",   "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "appearanceRef",    "I", ACC_PUBLIC);
        m3g_recalc_instance_size(jvm, stub);
        count++;
    }

    /* === 8. MorphingMesh (parent: Mesh) === */
    stub = cls_morphing_mesh;
    if (stub) {
        m3g_add_instance_field(stub, "morphTargets",  "[Ljavax/microedition/m3g/VertexBuffer;", ACC_PUBLIC);
        m3g_recalc_instance_size(jvm, stub);
        count++;
    }

    /* === 9. SkinnedMesh (parent: Mesh) === */
    stub = cls_skinned_mesh;
    if (stub) {
        m3g_add_instance_field(stub, "skeleton", "Ljavax/microedition/m3g/Group;", ACC_PUBLIC);
        m3g_add_instance_field(stub, "skeletonRef", "I", ACC_PUBLIC);
        m3g_recalc_instance_size(jvm, stub);
        count++;
    }

    /* === 10. Sprite3D (parent: Node) === */
    stub = cls_sprite3d;
    if (stub) {
        m3g_add_instance_field(stub, "image",      "Ljavax/microedition/m3g/Image2D;", ACC_PUBLIC);
        m3g_add_instance_field(stub, "cropX",      "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "cropY",      "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "cropWidth",  "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "cropHeight", "I", ACC_PUBLIC);
        m3g_recalc_instance_size(jvm, stub);
        count++;
    }

    /* === 11. Light (parent: Node) === */
    stub = cls_light;
    if (stub) {
        m3g_add_instance_field(stub, "mode",                "I",  ACC_PUBLIC);
        m3g_add_instance_field(stub, "lightType",           "I",  ACC_PUBLIC);  /* Alias used by M3G parser */
        m3g_add_instance_field(stub, "intensity",           "F",  ACC_PUBLIC);
        m3g_add_instance_field(stub, "color",               "[F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "spotAngle",           "F",  ACC_PUBLIC);
        m3g_add_instance_field(stub, "spotExponent",        "F",  ACC_PUBLIC);
        m3g_add_instance_field(stub, "constantAttenuation", "F",  ACC_PUBLIC);
        m3g_add_instance_field(stub, "linearAttenuation",   "F",  ACC_PUBLIC);
        m3g_add_instance_field(stub, "quadraticAttenuation","F",  ACC_PUBLIC);
        m3g_recalc_instance_size(jvm, stub);
        count++;
    }

    /* === 12. Material (parent: Object3D) === */
    stub = cls_material;
    if (stub) {
        m3g_add_instance_field(stub, "ambient",   "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "diffuse",   "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "emissive",  "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "specular",  "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "shininess", "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "vertexColorTracking", "I", ACC_PUBLIC);
        m3g_recalc_instance_size(jvm, stub);
        count++;
    }

    /* === 13. Appearance (parent: Object3D) === */
    stub = cls_appearance;
    if (stub) {
        m3g_add_instance_field(stub, "material",        "Ljavax/microedition/m3g/Material;", ACC_PUBLIC);
        m3g_add_instance_field(stub, "texture",         "Ljavax/microedition/m3g/Texture2D;", ACC_PUBLIC);
        m3g_add_instance_field(stub, "compositingMode", "Ljavax/microedition/m3g/CompositingMode;", ACC_PUBLIC);
        m3g_add_instance_field(stub, "polygonMode",     "Ljavax/microedition/m3g/PolygonMode;", ACC_PUBLIC);
        m3g_add_instance_field(stub, "fog",             "Ljavax/microedition/m3g/Fog;", ACC_PUBLIC);
        m3g_add_instance_field(stub, "layer",           "I", ACC_PUBLIC);
        /* Temporary: M3G binary parser writes refs as ints, linker resolves to objects above */
        m3g_add_instance_field(stub, "materialRef",         "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "textureRef",          "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "compositingModeRef",  "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "fogRef",              "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "polygonModeRef",      "I", ACC_PUBLIC);
        m3g_recalc_instance_size(jvm, stub);
        count++;
    }

    /* === 14. CompositingMode (parent: Object3D) === */
    stub = cls_compositing;
    if (stub) {
        m3g_add_instance_field(stub, "blending",        "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "alphaThreshold",  "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "depthTest",       "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "depthWrite",      "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "colorWrite",      "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "dithering",       "I", ACC_PUBLIC);
        m3g_recalc_instance_size(jvm, stub);
        count++;
    }

    /* === 15. PolygonMode (parent: Object3D) === */
    stub = get_or_create_stub_class(jvm, "javax/microedition/m3g/PolygonMode");  /* not pre-created */
    if (stub) {
        m3g_add_instance_field(stub, "culling",  "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "shading",  "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "winding",  "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "twoSidedLighting", "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "localCameraLighting", "I", ACC_PUBLIC);
        m3g_recalc_instance_size(jvm, stub);
        count++;
    }

    /* === 16. Fog (parent: Object3D) === */
    stub = cls_fog;
    if (stub) {
        m3g_add_instance_field(stub, "density",    "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "nearDistance","F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "farDistance", "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "mode",       "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "color",      "[F", ACC_PUBLIC);
        m3g_recalc_instance_size(jvm, stub);
        count++;
    }

    /* === 17. Texture2D (parent: Object3D) === */
    stub = cls_texture2d;
    if (stub) {
        m3g_add_instance_field(stub, "image",     "Ljavax/microedition/m3g/Image2D;", ACC_PUBLIC);
        m3g_add_instance_field(stub, "blendS",    "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "blendT",    "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "imageModeX","I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "imageModeY","I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "filtering", "I", ACC_PUBLIC);
        /* Temporary: M3G binary parser writes image ref as int, linker resolves to Image2D object */
        m3g_add_instance_field(stub, "imageRef",  "I", ACC_PUBLIC);
        m3g_recalc_instance_size(jvm, stub);
        count++;
    }

    /* === 18. Image2D (parent: Object3D) === */
    stub = cls_image2d;
    if (stub) {
        m3g_add_instance_field(stub, "width",  "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "height", "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "format", "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "pixels", "Ljava/lang/Object;", ACC_PUBLIC);
        m3g_add_instance_field(stub, "imageRef","I", ACC_PUBLIC);
        m3g_recalc_instance_size(jvm, stub);
        count++;
    }

    /* === 19. Background (parent: Object3D) === */
    stub = cls_background;
    if (stub) {
        m3g_add_instance_field(stub, "clearColor",      "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "colorClearEnable", "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "depthClearEnable", "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "image",           "Ljavax/microedition/m3g/Image2D;", ACC_PUBLIC);
        m3g_add_instance_field(stub, "imageModeX",      "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "imageModeY",      "I", ACC_PUBLIC);
        m3g_recalc_instance_size(jvm, stub);
        count++;
    }

    /* === 20. VertexArray (parent: Object3D) === */
    stub = cls_vertex_array;
    if (stub) {
        m3g_add_instance_field(stub, "data",           "[B", ACC_PUBLIC);
        m3g_add_instance_field(stub, "componentCount",  "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "componentSize",    "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "vertexCount",      "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "numComponents",    "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "encoding",         "I", ACC_PUBLIC);
        m3g_recalc_instance_size(jvm, stub);
        count++;
    }

    /* === 21. VertexBuffer (parent: Object3D) === */
    stub = cls_vertex_buffer;
    if (stub) {
        m3g_add_instance_field(stub, "positions",       "Ljavax/microedition/m3g/VertexArray;", ACC_PUBLIC);
        m3g_add_instance_field(stub, "normals",         "Ljavax/microedition/m3g/VertexArray;", ACC_PUBLIC);
        m3g_add_instance_field(stub, "texCoords",       "Ljavax/microedition/m3g/VertexArray;", ACC_PUBLIC);
        m3g_add_instance_field(stub, "colors",          "Ljavax/microedition/m3g/VertexArray;", ACC_PUBLIC);
        m3g_add_instance_field(stub, "defaultColor",    "I", ACC_PUBLIC);
        /* Permanent fields: scale and bias used by renderMesh every frame */
        m3g_add_instance_field(stub, "positionScale",   "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "biasX",           "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "biasY",           "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "biasZ",           "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "texCoordScale",   "F", ACC_PUBLIC);
        /* Temporary: M3G binary parser writes refs as ints, linker resolves to objects above */
        m3g_add_instance_field(stub, "positionsRef",    "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "normalsRef",      "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "colorsRef",       "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "texCoordsRef",    "I", ACC_PUBLIC);
        m3g_recalc_instance_size(jvm, stub);
        count++;
    }

    /* === 22. IndexBuffer (parent: Object3D) === */
    stub = cls_index_buffer;
    if (stub) {
        m3g_add_instance_field(stub, "indices", "[I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "indexCount", "I", ACC_PUBLIC);
        m3g_recalc_instance_size(jvm, stub);
        count++;
    }

    /* === 23. TriangleStripArray (parent: IndexBuffer) === */
    stub = cls_tri_strip_arr;
    if (stub) {
        m3g_add_instance_field(stub, "stripLengths", "[I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "indexCount",    "I",  ACC_PUBLIC);
        m3g_recalc_instance_size(jvm, stub);
        count++;
    }

    /* === 24. AnimationTrack (parent: Object3D) === */
    stub = cls_anim_track;
    if (stub) {
        m3g_add_instance_field(stub, "propertyId",  "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "sequence",    "Ljavax/microedition/m3g/KeyframeSequence;", ACC_PUBLIC);
        m3g_add_instance_field(stub, "controller",  "Ljavax/microedition/m3g/AnimationController;", ACC_PUBLIC);
        m3g_recalc_instance_size(jvm, stub);
        count++;
    }

    /* === 25. AnimationController (parent: Object3D) === */
    stub = cls_anim_ctrl;
    if (stub) {
        m3g_add_instance_field(stub, "activationTime", "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "deactivationTime","I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "speed",           "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "weight",          "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "refSequenceTime", "F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "refWorldTime",    "I", ACC_PUBLIC);
        m3g_recalc_instance_size(jvm, stub);
        count++;
    }

    /* === 26. KeyframeSequence (parent: Object3D) === */
    stub = cls_keyframe_seq;
    if (stub) {
        m3g_add_instance_field(stub, "componentCount",  "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "interpolation",   "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "duration",        "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "repeatMode",      "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "keyframeTimes",   "[I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "keyframeValues",  "[F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "numKeyframes",    "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "validRangeFirst", "I", ACC_PUBLIC);
        m3g_add_instance_field(stub, "validRangeLast",  "I", ACC_PUBLIC);
        m3g_recalc_instance_size(jvm, stub);
        count++;
    }

    /* === 27. Transform (parent: java/lang/Object) === */
    stub = cls_transform;
    if (stub) {
        m3g_add_instance_field(stub, "matrix", "[F", ACC_PUBLIC);
        m3g_recalc_instance_size(jvm, stub);
        count++;
    }

    /* === 28. Graphics3D (parent: java/lang/Object) === */
    stub = cls_graphics3d;
    if (stub) {
        /* No specific instance fields needed, singleton managed via native code */
        count++;
    }

    /* === 29. Loader (parent: java/lang/Object) === */
    stub = cls_loader;
    if (stub) {
        count++;
    }

    /* === 30. RayIntersection (parent: java/lang/Object) === */
    stub = cls_ray_intersect;
    if (stub) {
        m3g_add_instance_field(stub, "textureS",     "[F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "textureT",     "[F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "normal",       "[F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "submeshIndex", "I",  ACC_PUBLIC);
        m3g_add_instance_field(stub, "ray",          "[F", ACC_PUBLIC);
        m3g_add_instance_field(stub, "intersected",  "Ljavax/microedition/m3g/Node;", ACC_PUBLIC);
        m3g_recalc_instance_size(jvm, stub);
        count++;
    }

    /* Log summary with instance_size for key classes */
    fprintf(stderr, "[M3G] Initialized %d stub classes with instance fields\n", count);
    if (cls_world) fprintf(stderr, "[M3G]   World:  instance_size=%zu, fields=%d, super=%s\n",
        cls_world->instance_size, cls_world->fields_count,
        cls_world->super_class ? cls_world->super_class->class_name : "none");
    if (cls_group) fprintf(stderr, "[M3G]   Group:  instance_size=%zu, fields=%d, super=%s\n",
        cls_group->instance_size, cls_group->fields_count,
        cls_group->super_class ? cls_group->super_class->class_name : "none");
    if (cls_node) fprintf(stderr, "[M3G]   Node:   instance_size=%zu, fields=%d, super=%s\n",
        cls_node->instance_size, cls_node->fields_count,
        cls_node->super_class ? cls_node->super_class->class_name : "none");
    if (cls_object3d) fprintf(stderr, "[M3G]   Object3D: instance_size=%zu, fields=%d, super=%s\n",
        cls_object3d->instance_size, cls_object3d->fields_count,
        cls_object3d->super_class ? cls_object3d->super_class->class_name : "none");

    return count;
}

/* ============================================================================
 * Native Method Registration
 * ============================================================================ */

void init_javax_microedition_m3g(JVM* jvm) {
    NativeMethodEntry methods[] = {
        /* Graphics3D */
        {"javax/microedition/m3g/Graphics3D", "getInstance", "()Ljavax/microedition/m3g/Graphics3D;", native_graphics3d_getInstance},
        {"javax/microedition/m3g/Graphics3D", "bindTarget", "(Ljava/lang/Object;ZI)V", native_graphics3d_bindTarget_3arg},
        {"javax/microedition/m3g/Graphics3D", "bindTarget", "(Ljavax/microedition/m3g/Image2D;)V", native_graphics3d_bindTarget},
        {"javax/microedition/m3g/Graphics3D", "releaseTarget", "()V", native_graphics3d_releaseTarget},
        {"javax/microedition/m3g/Graphics3D", "clear", "(Ljavax/microedition/m3g/Background;)V", native_graphics3d_clear},
        {"javax/microedition/m3g/Graphics3D", "setViewport", "(IIII)V", native_graphics3d_setViewport},
        {"javax/microedition/m3g/Graphics3D", "render", "(Ljavax/microedition/m3g/World;)V", native_graphics3d_renderWorld},
        {"javax/microedition/m3g/Graphics3D", "render", "(Ljavax/microedition/m3g/Node;Ljavax/microedition/m3g/Transform;)V", native_graphics3d_render_node},
        {"javax/microedition/m3g/Graphics3D", "addLight", "(Ljavax/microedition/m3g/Light;Ljavax/microedition/m3g/Transform;)I", native_graphics3d_addLight},
        {"javax/microedition/m3g/Graphics3D", "setLight", "(ILjavax/microedition/m3g/Light;Ljavax/microedition/m3g/Transform;)V", native_graphics3d_setLight},
        {"javax/microedition/m3g/Graphics3D", "setCamera", "(Ljavax/microedition/m3g/Camera;Ljavax/microedition/m3g/Transform;)V", native_graphics3d_setCamera},
        {"javax/microedition/m3g/Graphics3D", "resetLights", "()V", native_graphics3d_resetLights},
        
        /* Transform */
        {"javax/microedition/m3g/Transform", "<init>", "()V", native_transform_init},
        {"javax/microedition/m3g/Transform", "<init>", "(Ljavax/microedition/m3g/Transform;)V", native_transform_init_copy},
        {"javax/microedition/m3g/Transform", "setIdentity", "()V", native_transform_setIdentity},
        {"javax/microedition/m3g/Transform", "postMultiply", "(Ljavax/microedition/m3g/Transform;)V", native_transform_postMultiply},
        {"javax/microedition/m3g/Transform", "postTranslate", "(FFF)V", native_transform_postTranslate},
        {"javax/microedition/m3g/Transform", "postRotate", "(FFFF)V", native_transform_postRotate},
        {"javax/microedition/m3g/Transform", "postScale", "(FFF)V", native_transform_postScale},
        {"javax/microedition/m3g/Transform", "invert", "()V", native_transform_invert},
        {"javax/microedition/m3g/Transform", "transform", "([F)V", native_transform_transform},
        
        /* VertexArray */
        {"javax/microedition/m3g/VertexArray", "<init>", "(III)V", native_vertexarray_init},
        {"javax/microedition/m3g/VertexArray", "set", "(II[B)V", native_vertexarray_set_byte},
        {"javax/microedition/m3g/VertexArray", "set", "(II[S)V", native_vertexarray_set_short},
        
        /* VertexBuffer */
        {"javax/microedition/m3g/VertexBuffer", "<init>", "()V", native_vertexbuffer_init},
        {"javax/microedition/m3g/VertexBuffer", "setPositions", "(Ljavax/microedition/m3g/VertexArray;F[F)V", native_vertexbuffer_setPositions},
        {"javax/microedition/m3g/VertexBuffer", "setNormals", "(Ljavax/microedition/m3g/VertexArray;)V", native_vertexbuffer_setNormals},
        {"javax/microedition/m3g/VertexBuffer", "setTexCoords", "(ILjavax/microedition/m3g/VertexArray;F[F)V", native_vertexbuffer_setTexCoords},
        {"javax/microedition/m3g/VertexBuffer", "setColors", "(Ljavax/microedition/m3g/VertexArray;)V", native_vertexbuffer_setColors},
        
        /* Camera */
        {"javax/microedition/m3g/Camera", "setPerspective", "(FFFF)V", native_camera_setPerspective},
        {"javax/microedition/m3g/Camera", "setParallel", "(FFFF)V", native_camera_setParallel},
        {"javax/microedition/m3g/Camera", "setGeneric", "(Ljavax/microedition/m3g/Transform;)V", native_camera_setGeneric},
        {"javax/microedition/m3g/Camera", "lookAt", "(FFFFFFFFF)V", native_camera_lookAt},
        
        /* Light */
        {"javax/microedition/m3g/Light", "setType", "(I)V", native_light_setType},
        {"javax/microedition/m3g/Light", "setMode", "(I)V", native_light_setType}, /* same handler */
        {"javax/microedition/m3g/Light", "setColor", "(I)V", native_light_setColor},
        {"javax/microedition/m3g/Light", "setIntensity", "(F)V", native_light_setIntensity},
        {"javax/microedition/m3g/Light", "setDirection", "(FFF)V", native_light_setDirection},
        
        /* Material */
        {"javax/microedition/m3g/Material", "setColor", "(II)V", native_material_setColor},
        {"javax/microedition/m3g/Material", "setShininess", "(F)V", native_material_setShininess},
        
        /* Appearance */
        {"javax/microedition/m3g/Appearance", "setMaterial", "(Ljavax/microedition/m3g/Material;)V", native_appearance_setMaterial},
        {"javax/microedition/m3g/Appearance", "getMaterial", "()Ljavax/microedition/m3g/Material;", native_appearance_getMaterial},
        {"javax/microedition/m3g/Appearance", "setTexture", "(ILjavax/microedition/m3g/Texture2D;)V", native_appearance_setTexture},
        
        /* Texture2D */
        {"javax/microedition/m3g/Texture2D", "<init>", "(Ljavax/microedition/m3g/Image2D;)V", native_texture2d_init},
        {"javax/microedition/m3g/Texture2D", "setFiltering", "(I)V", native_texture2d_setFiltering},
        {"javax/microedition/m3g/Texture2D", "setWrapping", "(II)V", native_texture2d_setWrapping},
        
        /* Mesh */
        {"javax/microedition/m3g/Mesh", "<init>", "(Ljavax/microedition/m3g/VertexBuffer;Ljavax/microedition/m3g/IndexBuffer;Ljavax/microedition/m3g/Appearance;)V", native_mesh_init},
        {"javax/microedition/m3g/Mesh", "setVertexBuffer", "(ILjavax/microedition/m3g/VertexArray;)V", native_mesh_setVertexBuffer},
        
        /* TriangleStripArray */
        {"javax/microedition/m3g/TriangleStripArray", "<init>", "(I[I)V", native_trianglestrip_init},
        
        /* Node */
        {"javax/microedition/m3g/Node", "setScale", "(FFF)V", native_node_setScale},
        {"javax/microedition/m3g/Node", "setTranslation", "(FFF)V", native_node_setTranslation},
        {"javax/microedition/m3g/Node", "setRotation", "(FFFF)V", native_node_setRotation},
        {"javax/microedition/m3g/Node", "getTransform", "(Ljavax/microedition/m3g/Transform;)V", native_node_getTransform},
        {"javax/microedition/m3g/Node", "setTransform", "(Ljavax/microedition/m3g/Transform;)V", native_node_setTransform},
        {"javax/microedition/m3g/Node", "setAlphaFactor", "(F)V", native_node_setAlphaFactor},
        
        /* Group */
        {"javax/microedition/m3g/Group", "addChild", "(Ljavax/microedition/m3g/Node;)V", native_group_addChild},
        {"javax/microedition/m3g/Group", "removeChild", "(Ljavax/microedition/m3g/Node;)V", native_group_removeChild},
        {"javax/microedition/m3g/Group", "getChildCount", "()I", native_group_getChildCount},
        {"javax/microedition/m3g/Group", "getChild", "(I)Ljavax/microedition/m3g/Node;", native_group_getChild},
        
        /* World */
        {"javax/microedition/m3g/World", "addCamera", "(Ljavax/microedition/m3g/Camera;)V", native_world_addCamera},
        {"javax/microedition/m3g/World", "setBackground", "(Ljavax/microedition/m3g/Background;)V", native_world_setBackground},
        {"javax/microedition/m3g/World", "addChild", "(Ljavax/microedition/m3g/Node;)V", native_world_addChild},
        {"javax/microedition/m3g/World", "setActiveCamera", "(Ljavax/microedition/m3g/Camera;)V", native_world_setActiveCamera},
        {"javax/microedition/m3g/World", "getActiveCamera", "()Ljavax/microedition/m3g/Camera;", native_world_getActiveCamera},
        
        /* Object3D */
        {"javax/microedition/m3g/Object3D", "find", "(I)Ljavax/microedition/m3g/Object3D;", native_object3d_find},
        {"javax/microedition/m3g/Object3D", "getUserID", "()I", native_object3d_getUserID},
        {"javax/microedition/m3g/Object3D", "setUserID", "(I)V", native_object3d_setUserID},
        {"javax/microedition/m3g/Object3D", "duplicate", "()Ljavax/microedition/m3g/Object3D;", native_object3d_duplicate},
        
        /* Background */
        {"javax/microedition/m3g/Background", "setColor", "(I)V", native_background_setColor},
        
        /* Image2D */
        {"javax/microedition/m3g/Image2D", "<init>", "(III)V", native_image2d_init},
        {"javax/microedition/m3g/Image2D", "<init>", "(ILjava/lang/Object;)V", native_image2d_init_from_image},
        {"javax/microedition/m3g/Image2D", "set", "(IIII[B)V", native_image2d_set},
        
        /* CompositingMode */
        {"javax/microedition/m3g/CompositingMode", "setBlending", "(I)V", native_compositingmode_setBlending},
        
        /* PolygonMode */
        {"javax/microedition/m3g/PolygonMode", "setCulling", "(I)V", native_polygonmode_setCulling},
        
        /* Loader */
        {"javax/microedition/m3g/Loader", "load", "(Ljava/lang/String;)[Ljavax/microedition/m3g/Object3D;", native_loader_load},
        {"javax/microedition/m3g/Loader", "load", "([BI)[Ljavax/microedition/m3g/Object3D;", native_loader_load_bytes},
        
        /* RayIntersection */
        {"javax/microedition/m3g/RayIntersection", "getDistance", "()F", native_rayintersection_getDistance},
        {"javax/microedition/m3g/RayIntersection", "getIntersected", "()Ljavax/microedition/m3g/Node;", native_rayintersection_getIntersected},
        
        /* AnimationTrack */
        {"javax/microedition/m3g/AnimationTrack", "setController", "(Ljavax/microedition/m3g/AnimationController;)V", native_animationtrack_setController},
        {"javax/microedition/m3g/AnimationTrack", "getTargetProperty", "()I", native_animationtrack_getTargetProperty},
        
        /* AnimationController */
        {"javax/microedition/m3g/AnimationController", "setActiveInterval", "(II)V", native_animationcontroller_setActiveInterval},
        {"javax/microedition/m3g/AnimationController", "getActiveIntervalStart", "()I", native_animationcontroller_getActiveIntervalStart},
        {"javax/microedition/m3g/AnimationController", "getActiveIntervalEnd", "()I", native_animationcontroller_getActiveIntervalEnd},
        {"javax/microedition/m3g/AnimationController", "setSpeed", "(FI)V", native_animationcontroller_setSpeed},
        {"javax/microedition/m3g/AnimationController", "getSpeed", "()F", native_animationcontroller_getSpeed},
        {"javax/microedition/m3g/AnimationController", "setPosition", "(FI)V", native_animationcontroller_setPosition},
        {"javax/microedition/m3g/AnimationController", "getPosition", "(I)F", native_animationcontroller_getPosition},
        {"javax/microedition/m3g/AnimationController", "setWeight", "(F)V", native_animationcontroller_setWeight},
        {"javax/microedition/m3g/AnimationController", "getWeight", "()F", native_animationcontroller_getWeight},
        {"javax/microedition/m3g/AnimationController", "getRefWorldTime", "()I", native_animationcontroller_getRefWorldTime},
        
        /* KeyframeSequence */
        {"javax/microedition/m3g/KeyframeSequence", "setKeyframe", "(II[F)V", native_keyframesequence_setKeyframe},
        {"javax/microedition/m3g/KeyframeSequence", "getDuration", "()I", native_keyframesequence_getDuration},
        {"javax/microedition/m3g/KeyframeSequence", "setDuration", "(I)V", native_keyframesequence_setDuration},
        {"javax/microedition/m3g/KeyframeSequence", "getRepeatMode", "()I", native_keyframesequence_getRepeatMode},
        {"javax/microedition/m3g/KeyframeSequence", "setRepeatMode", "(I)V", native_keyframesequence_setRepeatMode},
        {"javax/microedition/m3g/KeyframeSequence", "getComponentCount", "()I", native_keyframesequence_getComponentCount},
        {"javax/microedition/m3g/KeyframeSequence", "getKeyframeCount", "()I", native_keyframesequence_getKeyframeCount},
        {"javax/microedition/m3g/KeyframeSequence", "getInterpolationType", "()I", native_keyframesequence_getInterpolationType},
        {"javax/microedition/m3g/KeyframeSequence", "getKeyframe", "(I[F)I", native_keyframesequence_getKeyframe},
        {"javax/microedition/m3g/KeyframeSequence", "setValidRange", "(II)V", native_keyframesequence_setValidRange},
        {"javax/microedition/m3g/KeyframeSequence", "getValidRangeFirst", "()I", native_keyframesequence_getValidRangeFirst},
        {"javax/microedition/m3g/KeyframeSequence", "getValidRangeLast", "()I", native_keyframesequence_getValidRangeLast},
        
        /* Object3D animation */
        {"javax/microedition/m3g/Object3D", "animate", "(I)I", native_object3d_animate},
        {"javax/microedition/m3g/Object3D", "addAnimationTrack", "(Ljavax/microedition/m3g/AnimationTrack;)V", native_object3d_addAnimationTrack},
        {"javax/microedition/m3g/Object3D", "removeAnimationTrack", "(Ljavax/microedition/m3g/AnimationTrack;)V", native_object3d_removeAnimationTrack},
        {"javax/microedition/m3g/Object3D", "getAnimationTrackCount", "()I", native_object3d_getAnimationTrackCount},
        {"javax/microedition/m3g/Object3D", "getAnimationTrack", "(I)Ljavax/microedition/m3g/AnimationTrack;", native_object3d_getAnimationTrack},
        
        /* Sprite3D */
        {"javax/microedition/m3g/Sprite3D", "<init>", "(ZLjavax/microedition/m3g/Image2D;Ljavax/microedition/m3g/Appearance;)V", native_sprite3d_init},
        {"javax/microedition/m3g/Sprite3D", "setImage", "(Ljavax/microedition/m3g/Image2D;)V", native_sprite3d_setImage},
    };
    
    native_register_methods(jvm, methods, sizeof(methods) / sizeof(methods[0]));
    
    /* Register stub classes */
    init_m3g_stub_classes(jvm);
    
    GFX_DEBUG("Registered %zu native methods for JSR 184", 
            sizeof(methods) / sizeof(methods[0]));
}
