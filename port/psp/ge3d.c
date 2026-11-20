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
// Hash of (key, palette) -> index + 1 in s_tex (open addressing).
#define TEX_HASH 1024
static uint16_t s_tex_hash[TEX_HASH];

static inline uint32_t tex_hash(uint32_t key, uint32_t pltt) {
    return ((key * 0x9e3779b1u) ^ (pltt * 0x85ebca6bu)) >> 22;
}

// Statistics (psp/main.c logs them): cache flushes, textures decoded, texels decoded, decoding time (us),
// polygons drawn, GE vertices.
uint32_t g_ge_stats[8]; // + batches, us spent issuing GE commands

static void tex_flush(void) {
    g_ge_stats[0]++;
    for (int i = 0; i < s_tex_count; i++) free(s_tex[i].data);
    s_tex_count = 0;
    s_tex_bytes = 0;
    memset(s_tex_hash, 0, sizeof(s_tex_hash));
}

// ------------------------------------------------------------------------------------------------ texture decoding
// Straight into the GE's 16-bit formats: 5551 (opaque formats, colour 0 transparent), 4444 (A3I5, A5I3). Rows are
// read through the VRAM map one at a time (a row never crosses a 16 KiB page unless the texture is unaligned: then
// texel by texel).
static inline uint16_t c5551(uint16_t c, bool opaque) { return (uint16_t) ((c & 0x7fff) | (opaque ? 0x8000 : 0)); }
static inline uint16_t c4444(uint16_t c, uint32_t a5) {
    uint32_t r = (c & 31) >> 1, g = ((c >> 5) & 31) >> 1, b = ((c >> 10) & 31) >> 1, a = (a5 * 15 + 15) / 31;
    return (uint16_t) (r | g << 4 | b << 8 | a << 12);
}

static const uint8_t *tex_row(const VramMap *map, uint32_t off, uint32_t bytes, uint8_t *tmp) {
    off &= 0x7ffff;
    if ((off & 0x3fff) + bytes <= 0x4000) return vram_ptr(map, SNAP_VRAM(NULL), VR_TEX, off);
    for (uint32_t i = 0; i < bytes; i++) { // crosses a page
        const uint8_t *p = vram_ptr(map, SNAP_VRAM(NULL), VR_TEX, (off + i) & 0x7ffff);
        tmp[i] = p ? *p : 0;
    }
    return tmp;
}

static uint16_t pal_entry(const VramMap *map, uint32_t off) {
    const uint8_t *p = vram_ptr(map, SNAP_VRAM(NULL), VR_TEXPAL, off & 0x1ffff);
    return p ? (uint16_t) (p[0] | p[1] << 8) : 0;
}

static bool decode16(const VramMap *map, uint32_t teximage, uint32_t pltt, uint16_t *out, uint32_t stride) {
    uint32_t fmt = (teximage >> 26) & 7;
    uint32_t w = TEX_WIDTH(teximage), h = TEX_HEIGHT(teximage);
    uint32_t addr = (teximage & 0xffff) << 3;
    bool c0t = teximage & (1u << 29);
    uint32_t pal = fmt == 2 ? pltt << 3 : pltt << 4;
    static uint16_t lut[256];
    static uint8_t tmp[2048];
    if (fmt == 5) {
        // 4x4 compressed: blocks of 2-bit texels, a palette index word per block in slot 1.
        uint32_t slot1 = 0x20000 + ((addr & 0x1ffff) >> 1) + (addr >= 0x40000 ? 0x10000 : 0);
        for (uint32_t by = 0; by < h / 4; by++) {
            const uint8_t *bits = tex_row(map, addr + by * (w / 4) * 4, (w / 4) * 4, tmp);
            const uint8_t *infos = tex_row(map, slot1 + by * (w / 4) * 2, (w / 4) * 2, tmp + 1024);
            for (uint32_t bx = 0; bx < w / 4; bx++) {
                uint32_t b = bits ? (uint32_t) bits[bx * 4] | bits[bx * 4 + 1] << 8 | bits[bx * 4 + 2] << 16 |
                                        (uint32_t) bits[bx * 4 + 3] << 24
                                  : 0;
                uint16_t info = infos ? (uint16_t) (infos[bx * 2] | infos[bx * 2 + 1] << 8) : 0;
                uint32_t pb = pal + (info & 0x3fff) * 4, mode = info >> 14;
                uint16_t col0 = pal_entry(map, pb), col1 = pal_entry(map, pb + 2);
                uint16_t cols[4];
                cols[0] = c5551(col0, true);
                cols[1] = c5551(col1, true);
                if (mode == 0 || mode == 2) {
                    cols[2] = c5551(pal_entry(map, pb + 4), true);
                    cols[3] = mode == 2 ? c5551(pal_entry(map, pb + 6), true) : 0;
                } else {
                    uint16_t m1 = 0, m2 = 0;
                    for (int s = 0; s < 15; s += 5) {
                        uint32_t x0 = (col0 >> s) & 31, x1 = (col1 >> s) & 31;
                        if (mode == 1) {
                            m1 |= (uint16_t) ((x0 + x1) / 2 << s);
                        } else {
                            m1 |= (uint16_t) ((x0 * 5 + x1 * 3) / 8 << s);
                            m2 |= (uint16_t) ((x0 * 3 + x1 * 5) / 8 << s);
                        }
                    }
                    cols[2] = c5551(m1, true);
                    cols[3] = mode == 1 ? 0 : c5551(m2, true);
                }
                uint16_t *o = out + by * 4 * stride + bx * 4;
                for (uint32_t ty = 0; ty < 4; ty++, b >>= 8) {
                    o[ty * stride] = cols[b & 3];
                    o[ty * stride + 1] = cols[(b >> 2) & 3];
                    o[ty * stride + 2] = cols[(b >> 4) & 3];
                    o[ty * stride + 3] = cols[(b >> 6) & 3];
                }
            }
        }
        return true;
    }
    if (fmt == 7) {
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *row = tex_row(map, addr + y * w * 2, w * 2, tmp);
            for (uint32_t x = 0; x < w; x++) {
                uint16_t c = row ? (uint16_t) (row[x * 2] | row[x * 2 + 1] << 8) : 0;
                out[y * stride + x] = c;
            }
        }
        return true;
    }
    // Paletted formats: the palette converted once.
    uint32_t colors = fmt == 1 ? 32 : fmt == 2 ? 4 : fmt == 3 ? 16 : fmt == 6 ? 8 : 256;
    if (fmt == 1 || fmt == 6) {
        for (uint32_t i = 0; i < colors; i++) lut[i] = pal_entry(map, pal + i * 2);
    } else {
        for (uint32_t i = 0; i < colors; i++) lut[i] = c5551(pal_entry(map, pal + i * 2), !(i == 0 && c0t));
    }
    uint32_t bpp = fmt == 2 ? 2 : fmt == 3 ? 4 : 8;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = tex_row(map, addr + y * w * bpp / 8, w * bpp / 8, tmp);
        uint16_t *o = out + y * stride;
        if (!row) {
            memset(o, 0, w * 2);
            continue;
        }
        switch (fmt) {
        case 1: // A3I5
            for (uint32_t x = 0; x < w; x++) {
                uint32_t v = row[x], a3 = v >> 5;
                o[x] = c4444(lut[v & 31], (a3 << 2) + (a3 >> 1));
            }
            break;
        case 6: // A5I3
            for (uint32_t x = 0; x < w; x++) o[x] = c4444(lut[row[x] & 7], row[x] >> 3);
            break;
        case 2:
            for (uint32_t x = 0; x < w; x++) o[x] = lut[(row[x >> 2] >> ((x & 3) * 2)) & 3];
            break;
        case 3:
            for (uint32_t x = 0; x < w; x += 2) {
                o[x] = lut[row[x >> 1] & 15];
                o[x + 1] = lut[row[x >> 1] >> 4];
            }
            break;
        default:
            for (uint32_t x = 0; x < w; x++) o[x] = lut[row[x]];
            break;
        }
    }
    return true;
}

// The texture for TEXIMAGE_PARAM / PLTT_BASE, NULL for none.
static const TexEntry *texture(const VramMap *map, uint32_t teximage, uint32_t pltt) {
    uint32_t fmt = (teximage >> 26) & 7;
    if (fmt == 0) return NULL;
    // Address, size, format, colour 0 transparent, and the mirrored repeat (stored doubled).
    uint32_t mirror = (teximage >> 16) & (teximage >> 18) & 3;
    uint32_t key = (teximage & 0x3ff0ffff) | mirror << 16;
    uint32_t pkey = fmt == 7 ? 0 : pltt;
    uint32_t slot = tex_hash(key, pkey);
    for (;; slot = (slot + 1) & (TEX_HASH - 1)) {
        uint32_t i = s_tex_hash[slot];
        if (!i) break;
        if (s_tex[i - 1].key == key && s_tex[i - 1].pltt == pkey) return s_tex[i - 1].data ? &s_tex[i - 1] : NULL;
    }
    uint32_t w = TEX_WIDTH(teximage), h = TEX_HEIGHT(teximage);
    uint32_t sw = w << (mirror & 1), sh = h << (mirror >> 1);
    if (sw > 512 || sh > 512) return NULL;
    if (s_tex_count == TEX_CACHE || s_tex_bytes > TEX_MEMORY) {
        tex_flush();
        slot = tex_hash(key, pkey);
    }
    uint64_t t_dec = sceKernelGetSystemTimeWide();
    g_ge_stats[1]++;
    g_ge_stats[2] += w * h;
    bool translucent = fmt == 1 || fmt == 6;
    uint16_t *data = memalign(16, sw * sh * 2);
    if (!data) {
        tex_flush();
        slot = tex_hash(key, pkey);
        data = memalign(16, sw * sh * 2);
        if (!data) return NULL;
    }
    if (!decode16(map, teximage, pltt, data, sw)) {
        free(data);
        return NULL;
    }
    // Mirrored repeat: the texture doubled, mirrored.
    if (mirror & 1) {
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) data[y * sw + 2 * w - 1 - x] = data[y * sw + x];
        }
    }
    if (mirror & 2) {
        for (uint32_t y = 0; y < h; y++) memcpy(data + (2 * h - 1 - y) * sw, data + y * sw, sw * 2);
    }
    sceKernelDcacheWritebackRange(data, sw * sh * 2);
    g_ge_stats[3] += (uint32_t) (sceKernelGetSystemTimeWide() - t_dec);
    TexEntry *t = &s_tex[s_tex_count++];
    s_tex_hash[slot] = (uint16_t) s_tex_count;
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
    uint8_t shadow; // 0 no, 1 shadow volume mask (polygon ID 0), 2 shadow
    uint8_t id;     // polygon ID
    int32_t aspect; // of the projection (20.12)
} State;

// The framebuffer's alpha byte is the GE's stencil. In the 3D rectangle it holds what the DS keeps per pixel for its
// shadows: the polygon ID of the opaque pixel (bits 0-5) and the shadow mark (bit 7).
static void stencil_state(const State *s) {
    if (s->shadow == 1) {
        // Mask: marks the pixels where the volume is behind what was drawn (the depth test fails). No colour.
        sceGuPixelMask(0x7fffffff);
        sceGuStencilFunc(GU_ALWAYS, 0x80, 0xff);
        sceGuStencilOp(GU_KEEP, GU_REPLACE, GU_KEEP);
    } else if (s->shadow == 2) {
        sceGuPixelMask(0x7f000000); // colour and the mark only
        sceGuStencilFunc(GU_EQUAL, 0x80, 0x80);
        sceGuStencilOp(GU_KEEP, GU_KEEP, GU_ZERO);
    } else if (!s->translucent) {
        // Opaque pixels record their polygon ID.
        sceGuPixelMask(0);
        sceGuStencilFunc(GU_ALWAYS, s->id, 0xff);
        sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
    } else {
        sceGuPixelMask(0);
        sceGuStencilFunc(GU_ALWAYS, 0, 0);
        sceGuStencilOp(GU_KEEP, GU_KEEP, GU_KEEP);
    }
}

// Draws a batch of triangles in state s.
static void draw_batch(const State *s, GeVertex *v, int count) {
    if (count <= 0) return;
    if (s->shadow == 2) {
        // First no shadow on the pixels of its own polygon ID: their mark is cleared (nothing drawn).
        sceGuPixelMask(0x7fffffff);
        sceGuStencilFunc(GU_EQUAL, 0x80 | s->id, 0xbf);
        sceGuStencilOp(GU_KEEP, GU_KEEP, GU_ZERO);
        sceGuDrawArray(GU_TRIANGLES, GE_VERTEX_FORMAT, count, 0, v);
        stencil_state(s);
    }
    sceGuDrawArray(GU_TRIANGLES, GE_VERTEX_FORMAT, count, 0, v);
}

static void apply_state(const State *s, const State *prev, ScreenRect r, bool blending) {
    if (!prev || prev->proj != s->proj) {
        sceGuSetMatrix(GU_PROJECTION, &s_projs[s->proj].m);
    }
    if (!prev || memcmp(prev->viewport, s->viewport, 4) != 0 || prev->aspect != s->aspect) {
        float sx = r.w / 256.0f, sy = r.h / 192.0f;
        int x1 = s->viewport[0], y1 = s->viewport[1], x2 = s->viewport[2], y2 = s->viewport[3];
        float vw = (float) (x2 - x1 + 1), vh = (float) (y2 - y1 + 1);
        float cx = r.x + (x1 + vw / 2) * sx, cy = r.y + (191 - y2 + vh / 2) * sy;
        float w_px = vw * sx;
        if (x1 == 0 && x2 == 255 && s->aspect > 0) {
            // A whole-screen view keeps the shape its projection gives it: a 4:3 view on the wide screen is centred,
            // a wide view on a 4:3 screen shows its middle.
            w_px = vh * sy * (s->aspect / 4096.0f);
        }
        sceGuViewport(2048 - 240 + (int) (cx + 0.5f), 2048 - 136 + (int) (cy + 0.5f), (int) (w_px + 0.5f),
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
    if (!prev || prev->translucent != s->translucent || prev->depth_write != s->depth_write ||
        prev->shadow != s->shadow) {
        if (s->shadow) {
            sceGuEnable(GU_BLEND);
            sceGuDepthMask(GU_TRUE);
        } else if (s->translucent && blending) {
            sceGuEnable(GU_BLEND);
            sceGuDepthMask(s->depth_write ? GU_FALSE : GU_TRUE); // sceGuDepthMask(GU_TRUE) disables writes
        } else {
            sceGuDisable(GU_BLEND);
            sceGuDepthMask(GU_FALSE);
        }
    }
}

static inline uint32_t vertex_color(float r, float g, float b, uint32_t a) {
    // Signed conversions (cvt.w.s): unsigned ones go through the C library on the PSP.
    int ir = (int) (r * (255.0f / 31.0f) + 0.5f), ig = (int) (g * (255.0f / 31.0f) + 0.5f),
        ib = (int) (b * (255.0f / 31.0f) + 0.5f);
    ir = ir < 0 ? 0 : ir > 255 ? 255 : ir;
    ig = ig < 0 ? 0 : ig > 255 ? 255 : ig;
    ib = ib < 0 ? 0 : ib > 255 ? 255 : ib;
    return (uint32_t) ir | (uint32_t) ig << 8 | (uint32_t) ib << 16 | a << 24;
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
    sceGuClearStencil(0);
    sceGuClear(GU_DEPTH_BUFFER_BIT | GU_STENCIL_BUFFER_BIT);
    sceGuEnable(GU_STENCIL_TEST);
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
            uint8_t id = (uint8_t) ((p->attr >> 24) & 63);
            bool translucent = (alpha != 0 && alpha != 31) || fmt == 1 || fmt == 6 || mode == 3;
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
            st.shadow = mode == 3 ? (id == 0 ? 1 : 2) : 0;
            st.id = id;
            st.aspect = p->aspect;
            st.proj = p->proj;
            memcpy(st.viewport, p->viewport, 4);
            if (!have_prev || memcmp(&st, &prev_state, sizeof(st)) != 0) {
                uint64_t tb = sceKernelGetSystemTimeWide();
                if (have_prev && s_vert_count > batch_first) {
                    draw_batch(&prev_state, s_verts + batch_first, (int) (s_vert_count - batch_first));
                }
                apply_state(&st, have_prev ? &prev_state : NULL, r, blending);
                stencil_state(&st);
                g_ge_stats[6]++;
                g_ge_stats[7] += (uint32_t) (sceKernelGetSystemTimeWide() - tb);
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
            bool outside = false; // some vertex outside a clipping plane
            for (int k = 0; k < p->count; k++) {
                const GxVertex *v = &f->vertices[p->first + k];
                CV *c = &in[k];
                int32_t aw = v->w < 0 ? -v->w : v->w;
                outside |= v->z < -v->w || v->z > v->w || v->x > 3 * aw || v->x < -3 * aw || v->y > 3 * aw ||
                           v->y < -3 * aw;
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
            int n = p->count;
            const CV *poly = in;
            if (outside) {
                n = clip_polygon(in, p->count, clipped);
                if (n < 3) continue;
                poly = clipped;
            }
            const float *inv = s_projs[p->proj].inv;
            uint32_t a8 = alpha * 255 / 31;
            GeVertex gv[16];
            for (int k = 0; k < n; k++) {
                const CV *c = &poly[k];
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
        draw_batch(&prev_state, s_verts + batch_first, (int) (s_vert_count - batch_first));
    }
    sceKernelDcacheWritebackRange(s_verts, sizeof(GeVertex) * s_vert_count);
    g_ge_stats[4] += f->polygon_count;
    g_ge_stats[5] += s_vert_count;

    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_STENCIL_TEST);
    sceGuPixelMask(0);
    sceGuDepthMask(GU_TRUE);
    sceGuDisable(GU_CLIP_PLANES);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuScissor(0, 0, 480, 272);
    sceGuViewport(2048, 2048, 480, 272);
}
