// The DS 3D layer on the PSP's GE.
//
// The geometry engine (platform/gx3d.c) gives polygons in clip space. They are clipped against the near and far
// planes (and a guard band) here, taken back to view space with the inverse of the projection they were drawn with,
// and handed to the GE with that projection: the GE projects them again, with perspective-correct texturing and its
// depth test. Opaque polygons are drawn first, then translucent ones, in list order (the game sorts them).
// Textures are decoded from the texture VRAM into 16-bit textures, kept until a VRAMCNT register changes (texture
// memory can only be written while its bank is mapped for the CPU).
#include "port.h"
#include "gpu2d.h"
#include "tex.h"

#include <pspgu.h>
#include <pspkernel.h>
#include <malloc.h>
#include <math.h>
#include <string.h>

#include "psp_port.h"
#include "ge3d.h"

typedef struct GeVertex {
    float u, v;
    uint32_t color;
    float x, y, z;
} GeVertex;
#define GE_VERTEX_FORMAT (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D)

// ------------------------------------------------------------------------------------------------ textures
#define TEX_CACHE 512
#define TEX_MEMORY (3 * 1024 * 1024)
typedef struct TexEntry {
    uint32_t key, pltt;
    void *data;
    uint32_t bytes;
    uint16_t w, h; // as stored (mirrored textures are doubled)
    uint8_t psm;
} TexEntry;
static TexEntry s_tex[TEX_CACHE];
static int s_tex_count;
static uint32_t s_tex_bytes;
static uint32_t s_tex_generation = ~0u;
static uint32_t *s_decode;
static uint32_t s_decode_size;

static void tex_flush(void) {
    for (int i = 0; i < s_tex_count; i++) free(s_tex[i].data);
    s_tex_count = 0;
    s_tex_bytes = 0;
}

static inline uint16_t to5551(uint32_t c) {
    uint32_t r = (c >> 3) & 31, g = (c >> 11) & 31, b = (c >> 19) & 31;
    return (uint16_t) (r | g << 5 | b << 10 | ((c >> 24) ? 0x8000 : 0));
}
static inline uint16_t to4444(uint32_t c) {
    uint32_t r = (c >> 4) & 15, g = (c >> 12) & 15, b = (c >> 20) & 15, a = ((c >> 24) * 15 + 15) / 31;
    return (uint16_t) (r | g << 4 | b << 8 | a << 12);
}

// The texture for TEXIMAGE_PARAM / PLTT_BASE, NULL for none.
static const TexEntry *texture(const VramMap *map, uint32_t teximage, uint32_t pltt) {
    uint32_t fmt = (teximage >> 26) & 7;
    if (fmt == 0) return NULL;
    // Address, size, format, colour 0 transparent, and the mirrored repeat (stored doubled).
    uint32_t mirror = (teximage >> 16) & (teximage >> 18) & 3;
    uint32_t key = (teximage & 0x3ff0ffff) | mirror << 16;
    uint32_t pkey = fmt == 7 ? 0 : pltt;
    for (int i = 0; i < s_tex_count; i++) {
        if (s_tex[i].key == key && s_tex[i].pltt == pkey) return &s_tex[i];
    }
    uint32_t w = TEX_WIDTH(teximage), h = TEX_HEIGHT(teximage);
    uint32_t sw = w << (mirror & 1), sh = h << (mirror >> 1);
    if (sw > 512 || sh > 512) return NULL;
    if (s_tex_count == TEX_CACHE || s_tex_bytes > TEX_MEMORY) tex_flush();
    if (w * h > s_decode_size) {
        free(s_decode);
        s_decode_size = w * h;
        s_decode = malloc(s_decode_size * 4);
    }
    if (!tex_decode(map, SNAP_VRAM(NULL), teximage, pltt, s_decode)) return NULL;
    bool translucent = fmt == 1 || fmt == 6;
    uint16_t *data = memalign(16, sw * sh * 2);
    if (!data) {
        tex_flush();
        data = memalign(16, sw * sh * 2);
        if (!data) return NULL;
    }
    for (uint32_t y = 0; y < sh; y++) {
        uint32_t ty = y < h ? y : 2 * h - 1 - y;
        for (uint32_t x = 0; x < sw; x++) {
            uint32_t tx = x < w ? x : 2 * w - 1 - x;
            uint32_t c = s_decode[ty * w + tx];
            data[y * sw + x] = translucent ? to4444(c) : to5551(c);
        }
    }
    sceKernelDcacheWritebackRange(data, sw * sh * 2);
    TexEntry *t = &s_tex[s_tex_count++];
    t->key = key;
    t->pltt = pkey;
    t->data = data;
    t->bytes = sw * sh * 2;
    t->w = (uint16_t) sw;
    t->h = (uint16_t) sh;
    t->psm = translucent ? GU_PSM_4444 : GU_PSM_5551;
    s_tex_bytes += t->bytes;
    return t;
}

// ------------------------------------------------------------------------------------------------ projections
typedef struct Proj {
    ScePspFMatrix4 m; // the projection (row vectors, as the DS)
    float inv[16];
    bool ok;
} Proj;
static Proj s_projs[GX_MAX_PROJS];

static bool invert4(const float *m, float *out) {
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (fabsf(det) < 1e-12f) return false;
    det = 1.0f / det;
    for (int i = 0; i < 16; i++) out[i] = inv[i] * det;
    return true;
}

static void projs_prepare(const Gx3dFrame *f) {
    for (uint32_t i = 0; i < f->proj_count; i++) {
        float m[16];
        for (int k = 0; k < 16; k++) m[k] = f->projs[i][k] / 4096.0f;
        memcpy(&s_projs[i].m, m, sizeof(m));
        s_projs[i].ok = invert4(m, s_projs[i].inv);
    }
}

// ------------------------------------------------------------------------------------------------ clipping
typedef struct CV {
    float x, y, z, w, s, t, r, g, b;
} CV;
#define GUARD 3.0f

static float plane_dist(const CV *v, int plane) {
    switch (plane) {
    case 0: return v->z + v->w;         // near
    case 1: return v->w - v->z;         // far
    case 2: return v->x + GUARD * v->w; // guard band
    case 3: return GUARD * v->w - v->x;
    case 4: return v->y + GUARD * v->w;
    default: return GUARD * v->w - v->y;
    }
}

// Clips the polygon in in[0..n) against every plane it crosses; returns the new vertex count (in out).
static int clip_polygon(CV *in, int n, CV *out) {
    CV buf[2][16];
    CV *src = in;
    int count = n;
    int which = 0;
    for (int plane = 0; plane < 6; plane++) {
        bool any_out = false;
        for (int i = 0; i < count; i++) any_out |= plane_dist(&src[i], plane) < 0;
        if (!any_out) continue;
        CV *dst = buf[which];
        int m = 0;
        for (int i = 0; i < count; i++) {
            const CV *a = &src[i], *b = &src[(i + 1) % count];
            float da = plane_dist(a, plane), db = plane_dist(b, plane);
            if (da >= 0) dst[m++] = *a;
            if ((da >= 0) != (db >= 0) && m < 15) {
                float t = da / (da - db);
                CV *c = &dst[m++];
                c->x = a->x + (b->x - a->x) * t;
                c->y = a->y + (b->y - a->y) * t;
                c->z = a->z + (b->z - a->z) * t;
                c->w = a->w + (b->w - a->w) * t;
                c->s = a->s + (b->s - a->s) * t;
                c->t = a->t + (b->t - a->t) * t;
                c->r = a->r + (b->r - a->r) * t;
                c->g = a->g + (b->g - a->g) * t;
                c->b = a->b + (b->b - a->b) * t;
            }
        }
        src = dst;
        count = m;
        which ^= 1;
        if (count < 3) return 0;
    }
    memcpy(out, src, sizeof(CV) * (size_t) count);
    return count;
}

// ------------------------------------------------------------------------------------------------ drawing
#define MAX_GE_VERTICES 24576
static GeVertex *s_verts;
static uint32_t s_vert_count;

typedef struct State {
    const TexEntry *tex;
    uint32_t teximage; // wrap bits
    uint32_t mode;     // polygon mode (attr bits 4-5)
    bool translucent;
    bool depth_write;
    uint8_t proj;
    uint8_t viewport[4];
} State;

static void apply_state(const State *s, const State *prev, ScreenRect r, bool blending) {
    if (!prev || prev->proj != s->proj) {
        sceGuSetMatrix(GU_PROJECTION, &s_projs[s->proj].m);
    }
    if (!prev || memcmp(prev->viewport, s->viewport, 4) != 0) {
        float sx = r.w / 256.0f, sy = r.h / 192.0f;
        int x1 = s->viewport[0], y1 = s->viewport[1], x2 = s->viewport[2], y2 = s->viewport[3];
        float vw = (float) (x2 - x1 + 1), vh = (float) (y2 - y1 + 1);
        float cx = r.x + (x1 + vw / 2) * sx, cy = r.y + (191 - y2 + vh / 2) * sy;
        sceGuViewport(2048 - 240 + (int) (cx + 0.5f), 2048 - 136 + (int) (cy + 0.5f), (int) (vw * sx + 0.5f),
                      (int) (vh * sy + 0.5f));
    }
    if (!prev || prev->tex != s->tex || prev->teximage != s->teximage) {
        if (s->tex) {
            sceGuEnable(GU_TEXTURE_2D);
            sceGuTexMode(s->tex->psm, 0, 0, 0);
            sceGuTexImage(0, s->tex->w, s->tex->h, s->tex->w, s->tex->data);
            sceGuTexFlush();
            // Repeat (bits 16/17); mirrored repeat is in the doubled texture.
            sceGuTexWrap(s->teximage & (1u << 16) ? GU_REPEAT : GU_CLAMP,
                         s->teximage & (1u << 17) ? GU_REPEAT : GU_CLAMP);
        } else {
            sceGuDisable(GU_TEXTURE_2D);
        }
    }
    if (!prev || prev->mode != s->mode) {
        sceGuTexFunc(s->mode == 1 ? GU_TFX_DECAL : GU_TFX_MODULATE, GU_TCC_RGBA);
    }
    if (!prev || prev->translucent != s->translucent || prev->depth_write != s->depth_write) {
        if (s->translucent && blending) {
            sceGuEnable(GU_BLEND);
            sceGuDepthMask(s->depth_write ? GU_FALSE : GU_TRUE); // sceGuDepthMask(GU_TRUE) disables writes
        } else {
            sceGuDisable(GU_BLEND);
            sceGuDepthMask(GU_FALSE);
        }
    }
}

static inline uint32_t vertex_color(float r, float g, float b, uint32_t a) {
    uint32_t ir = (uint32_t) (r * (255.0f / 31.0f) + 0.5f), ig = (uint32_t) (g * (255.0f / 31.0f) + 0.5f),
             ib = (uint32_t) (b * (255.0f / 31.0f) + 0.5f);
    if (ir > 255) ir = 255;
    if (ig > 255) ig = 255;
    if (ib > 255) ib = 255;
    return ir | ig << 8 | ib << 16 | a << 24;
}

void ge3d_draw(const Gx3dFrame *f, const FrameSnapshot *snap, ScreenRect r) {
    if (!s_verts) s_verts = memalign(64, sizeof(GeVertex) * MAX_GE_VERTICES);
    if (g_vram_generation != s_tex_generation) {
        s_tex_generation = g_vram_generation;
        tex_flush();
    }
    VramMap map;
    vram_map_build(&map, snap->vramcnt);
    bool texturing = f->disp3dcnt & 1;
    bool blending = f->disp3dcnt & 8;
    projs_prepare(f);
    s_vert_count = 0;

    sceGuScissor((int) r.x, (int) r.y, (int) (r.x + r.w + 0.5f), (int) (r.y + r.h + 0.5f));
    sceGuDepthRange(65535, 0);
    sceGuDepthFunc(GU_GEQUAL);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuEnable(GU_CLIP_PLANES);
    sceGuShadeModel(GU_SMOOTH);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    uint32_t aref = (f->disp3dcnt & 4) ? f->alpha_ref * 255 / 31 : 0;
    sceGuAlphaFunc(GU_GREATER, (int) aref, 0xff);
    sceGuEnable(GU_ALPHA_TEST);
    ScePspFMatrix4 id = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    sceGuSetMatrix(GU_VIEW, &id);
    sceGuSetMatrix(GU_MODEL, &id);

    // Clear: the depth everywhere in the rectangle; the colour when the clear colour is not transparent.
    uint32_t cc = f->clear_color;
    uint32_t calpha = (cc >> 16) & 31;
    sceGuClearDepth(0);
    sceGuClear(GU_DEPTH_BUFFER_BIT);
    if (calpha) {
        GeVertex *q = sceGuGetMemory(sizeof(GeVertex) * 2);
        uint32_t col = vertex_color((float) (cc & 31), (float) ((cc >> 5) & 31), (float) ((cc >> 10) & 31),
                                    calpha * 255 / 31);
        q[0] = (GeVertex) {0, 0, col, r.x, r.y, 0};
        q[1] = (GeVertex) {0, 0, col, r.x + r.w, r.y + r.h, 0};
        sceGuDisable(GU_TEXTURE_2D);
        sceGuDisable(GU_DEPTH_TEST);
        sceGuEnable(GU_BLEND);
        sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, q);
        sceGuEnable(GU_DEPTH_TEST);
    }

    State prev_state;
    bool have_prev = false;
    uint32_t batch_first = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < f->polygon_count; i++) {
            const GxPolygon *p = &f->polygons[i];
            if (p->hidden) continue;
            uint32_t alpha = (p->attr >> 16) & 31;
            uint32_t fmt = texturing ? (p->teximage >> 26) & 7 : 0;
            uint32_t mode = (p->attr >> 4) & 3;
            if (mode == 3) continue; // shadow volumes: not drawn yet
            bool translucent = (alpha != 0 && alpha != 31) || fmt == 1 || fmt == 6;
            if (translucent != (pass == 1)) continue;
            if (alpha == 0) continue; // wireframe
            if (!s_projs[p->proj].ok) continue;
            if (s_vert_count + 3 * 14 > MAX_GE_VERTICES) break;

            State st;
            st.tex = fmt ? texture(&map, p->teximage, p->pltt) : NULL;
            st.teximage = p->teximage & 0x000f0000;
            st.mode = mode == 1 ? 1 : 0;
            st.translucent = translucent;
            st.depth_write = (p->attr >> 11) & 1;
            st.proj = p->proj;
            memcpy(st.viewport, p->viewport, 4);
            if (!have_prev || memcmp(&st, &prev_state, sizeof(st)) != 0) {
                if (have_prev && s_vert_count > batch_first) {
                    sceGuDrawArray(GU_TRIANGLES, GE_VERTEX_FORMAT, (int) (s_vert_count - batch_first), 0,
                                   s_verts + batch_first);
                }
                apply_state(&st, have_prev ? &prev_state : NULL, r, blending);
                prev_state = st;
                have_prev = true;
                batch_first = s_vert_count;
            }

            // Vertices: clip space, attributes as floats.
            CV in[4], clipped[16];
            float us = 1.0f, vs = 1.0f;
            if (st.tex) {
                us = 1.0f / (16.0f * st.tex->w);
                vs = 1.0f / (16.0f * st.tex->h);
            }
            const uint16_t *toon = f->toon_table;
            bool highlight = f->disp3dcnt & 2;
            for (int k = 0; k < p->count; k++) {
                const GxVertex *v = &f->vertices[p->first + k];
                CV *c = &in[k];
                c->x = (float) v->x;
                c->y = (float) v->y;
                c->z = (float) v->z;
                c->w = (float) v->w;
                c->s = v->s * us;
                c->t = v->t * vs;
                if (mode == 2 && !highlight) { // toon: the red level picks a colour of the toon table
                    uint16_t tc = toon[v->r & 31];
                    c->r = (float) (tc & 31);
                    c->g = (float) ((tc >> 5) & 31);
                    c->b = (float) ((tc >> 10) & 31);
                } else if (mode == 2) {
                    c->r = c->g = c->b = (float) v->r;
                } else {
                    c->r = v->r;
                    c->g = v->g;
                    c->b = v->b;
                }
            }
            int n = clip_polygon(in, p->count, clipped);
            if (n < 3) continue;
            const float *inv = s_projs[p->proj].inv;
            uint32_t a8 = alpha * 255 / 31;
            GeVertex gv[16];
            for (int k = 0; k < n; k++) {
                const CV *c = &clipped[k];
                // View space: clip * P^-1 (row vectors), in 20.12 units like the projection's input.
                float vx = c->x * inv[0] + c->y * inv[4] + c->z * inv[8] + c->w * inv[12];
                float vy = c->x * inv[1] + c->y * inv[5] + c->z * inv[9] + c->w * inv[13];
                float vz = c->x * inv[2] + c->y * inv[6] + c->z * inv[10] + c->w * inv[14];
                float vw = c->x * inv[3] + c->y * inv[7] + c->z * inv[11] + c->w * inv[15];
                float k_w = vw != 0 ? 1.0f / vw : 1.0f;
                gv[k] = (GeVertex) {c->s, c->t, vertex_color(c->r, c->g, c->b, a8), vx * k_w, vy * k_w, vz * k_w};
            }
            for (int k = 1; k + 1 < n; k++) {
                s_verts[s_vert_count++] = gv[0];
                s_verts[s_vert_count++] = gv[k];
                s_verts[s_vert_count++] = gv[k + 1];
            }
        }
    }
    if (have_prev && s_vert_count > batch_first) {
        sceGuDrawArray(GU_TRIANGLES, GE_VERTEX_FORMAT, (int) (s_vert_count - batch_first), 0, s_verts + batch_first);
    }
    sceKernelDcacheWritebackRange(s_verts, sizeof(GeVertex) * s_vert_count);

    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_BLEND);
    sceGuDepthMask(GU_TRUE);
    sceGuDisable(GU_CLIP_PLANES);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuScissor(0, 0, 480, 272);
    sceGuViewport(2048, 2048, 480, 272);
}
