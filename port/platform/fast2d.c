// Fast DS 2D engine renderer (see fast2d.h).
//
// Each line is drawn back to front, layer by layer in the hardware's priority order (for priority 3 down to 0: the
// backgrounds of that priority from BG3 to BG0, then the sprites of that priority), each layer writing only its
// opaque pixels. Text backgrounds are walked tile by tile: one map entry and one row of character data per 8 pixels,
// rows without a single opaque pixel skipped at once. Where colour effects need the layer under the top one, each
// pixel keeps its two topmost layers.
#include "port.h"
#include "fast2d.h"

#include <string.h>

#define L_OBJ 4
#define L_BD 5
#define L_3D 6 // engine A in split mode: the 3D layer (BG0)

typedef struct Sprite {
    int16_t x, y;      // top left of the bounding box
    uint8_t bw, bh;    // bounding box
    uint8_t w, h;      // sprite size
    uint8_t prio, mode, c256, affine, hflip, vflip;
    uint16_t tile, pal; // character number, palette number
    int16_t pa, pb, pc, pd;
    uint8_t alpha;      // bitmap sprites: 0..15
    uint8_t index;
} Sprite;

typedef struct F2 {
    const FrameSnapshot *f;
    const uint8_t *io;
    const uint16_t *pal; // BG palette; the OBJ palette follows (+256)
    bool is_a;
    uint32_t dispcnt;
    const uint8_t *bgp[32]; // the BG region, per 16 KiB page (NULL: unmapped)
    const uint8_t *objp[16];
    const uint16_t *bgext[4]; // BG extended palette slots (8 KiB each)
    const uint16_t *objext;
    Sprite sprites[128];
    int sprite_count;
    // The line being drawn: top layer and the one under it.
    uint16_t c0[256], c1[256];
    uint8_t l0[256], l1[256];
    bool second;    // keep the second layer (colour effects need it)
    // Sprites of the line.
    uint16_t oc[256]; // colour | 0x8000
    uint8_t op[256];  // priority, 4 = none
    uint8_t oa[256];  // 0xff normal, 0xfe semi-transparent, 0..15 bitmap alpha
    uint8_t ow[256];  // inside the OBJ window
    bool obj_window;  // some sprite draws the OBJ window
    bool line_semi;   // a semi-transparent or bitmap sprite pixel on the line
    // Windows: what each pixel of the line shows (bits 0..3 BG, 4 OBJ, 5 colour effects).
    bool masked;
    uint8_t mask[256];
} F2;

static F2 s_f2;

static inline uint16_t io16(const F2 *e, uint32_t off) { return (uint16_t) (e->io[off] | e->io[off + 1] << 8); }
static inline uint32_t io32(const F2 *e, uint32_t off) { return io16(e, off) | (uint32_t) io16(e, off + 2) << 16; }

static inline const uint8_t *bg_at(const F2 *e, uint32_t off) {
    const uint8_t *p = e->bgp[(off >> 14) & 31];
    return p ? p + (off & 0x3fff) : NULL;
}
static inline const uint8_t *obj_at(const F2 *e, uint32_t off) {
    const uint8_t *p = e->objp[(off >> 14) & 15];
    return p ? p + (off & 0x3fff) : NULL;
}
static inline uint16_t bg16(const F2 *e, uint32_t off) {
    const uint8_t *p = bg_at(e, off);
    return p ? *(const uint16_t *) p : 0;
}
static inline uint8_t bg8(const F2 *e, uint32_t off) {
    const uint8_t *p = bg_at(e, off);
    return p ? *p : 0;
}

#define VISIBLE(e, x, layer) (!(e)->masked || ((e)->mask[x] >> (layer) & 1))
#define PUT(e, x, col, layer)                                                                                          \
    do {                                                                                                               \
        if (!VISIBLE(e, x, layer)) break;                                                                              \
        if ((e)->second) {                                                                                             \
            (e)->c1[x] = (e)->c0[x];                                                                                   \
            (e)->l1[x] = (e)->l0[x];                                                                                   \
        }                                                                                                              \
        (e)->c0[x] = (uint16_t) (col);                                                                                 \
        (e)->l0[x] = (uint8_t) (layer);                                                                                \
    } while (0)

// ------------------------------------------------------------------------------------------------ backgrounds
static void bg_text(F2 *e, int bg, int y) {
    uint16_t cnt = io16(e, 0x08 + bg * 2);
    uint32_t hofs = io16(e, 0x10 + bg * 4) & 0x1ff, vofs = io16(e, 0x12 + bg * 4) & 0x1ff;
    uint32_t size = cnt >> 14;
    uint32_t w = size & 1 ? 512 : 256, h = size & 2 ? 512 : 256;
    uint32_t screen_base = ((cnt >> 8) & 31) * 0x800, char_base = ((cnt >> 2) & 15) * 0x4000;
    if (e->is_a) {
        screen_base += ((e->dispcnt >> 27) & 7) * 0x10000;
        char_base += ((e->dispcnt >> 24) & 7) * 0x10000;
    }
    bool c256 = cnt & 0x80;
    const uint16_t *ext = NULL;
    if (c256 && (e->dispcnt & 0x40000000)) {
        int slot = bg < 2 && (cnt & 0x2000) ? bg + 2 : bg;
        ext = e->bgext[slot];
        if (!ext) return;
    }
    uint32_t sy = (y + vofs) & (h - 1);
    uint32_t row = screen_base + (sy >= 256 ? (w == 512 ? 2 : 1) * 0x800 : 0) + ((sy & 255) >> 3) * 64;
    uint32_t ty = sy & 7;
    uint32_t tiles_w = w >> 3;
    int x = -(int) (hofs & 7);
    uint32_t tx = hofs >> 3;
    for (; x < 256; x += 8, tx++) {
        tx &= tiles_w - 1;
        uint16_t entry = bg16(e, row + (tx >= 32 ? 0x800 : 0) + (tx & 31) * 2);
        uint32_t r = entry & 0x800 ? 7 - ty : ty;
        bool hflip = entry & 0x400;
        uint32_t tile = entry & 0x3ff;
        if (!c256) {
            const uint8_t *p = bg_at(e, char_base + tile * 32 + r * 4);
            if (!p) continue;
            uint32_t v = *(const uint32_t *) p;
            if (!v) continue;
            const uint16_t *pal = e->pal + (entry >> 12) * 16;
            for (int j = 0; j < 8; j++) {
                int px = x + j;
                uint32_t n = (v >> ((hflip ? 7 - j : j) * 4)) & 15;
                if (n && (unsigned) px < 256) PUT(e, px, pal[n] & 0x7fff, bg);
            }
        } else {
            const uint8_t *p = bg_at(e, char_base + tile * 64 + r * 8);
            if (!p) continue;
            uint32_t v0 = ((const uint32_t *) p)[0], v1 = ((const uint32_t *) p)[1];
            if (!(v0 | v1)) continue;
            const uint16_t *pal = ext ? ext + (entry >> 12) * 256 : e->pal;
            for (int j = 0; j < 8; j++) {
                int px = x + j;
                int k = hflip ? 7 - j : j;
                uint32_t n = ((k < 4 ? v0 : v1) >> ((k & 3) * 8)) & 255;
                if (n && (unsigned) px < 256) PUT(e, px, pal[n] & 0x7fff, bg);
            }
        }
    }
}

// Affine and extended backgrounds. kind: 0 affine (8-bit map), 1 extended with 16-bit map, 2 256-colour bitmap,
// 3 direct colour bitmap.
static void bg_affine(F2 *e, int bg, int y, int kind) {
    uint16_t cnt = io16(e, 0x08 + bg * 2);
    uint32_t base = 0x20 + (bg - 2) * 0x10;
    int32_t pa = (int16_t) io16(e, base), pb = (int16_t) io16(e, base + 2);
    int32_t pc = (int16_t) io16(e, base + 4), pd = (int16_t) io16(e, base + 6);
    int32_t rx = ((int32_t) (io32(e, base + 8) << 4)) >> 4, ry = ((int32_t) (io32(e, base + 12) << 4)) >> 4;
    rx += pb * y;
    ry += pd * y;
    bool wrap = cnt & 0x2000;
    uint32_t size = cnt >> 14;
    int32_t w, h;
    if (kind <= 1) {
        w = h = 128 << size;
    } else {
        static const uint16_t bw[4] = {128, 256, 512, 512}, bh[4] = {128, 256, 256, 512};
        w = bw[size];
        h = bh[size];
    }
    uint32_t screen_base = ((cnt >> 8) & 31) * 0x800, char_base = ((cnt >> 2) & 15) * 0x4000;
    if (e->is_a && kind <= 1) {
        screen_base += ((e->dispcnt >> 27) & 7) * 0x10000;
        char_base += ((e->dispcnt >> 24) & 7) * 0x10000;
    }
    uint32_t bitmap_base = ((cnt >> 8) & 31) * 0x4000;
    const uint16_t *ext = kind == 1 && (e->dispcnt & 0x40000000) ? e->bgext[bg] : NULL;
    if (kind == 1 && (e->dispcnt & 0x40000000) && !ext) return;
    int32_t tiles_w = w >> 3;
    if (pc == 0 && kind <= 1) {
        // Not rotated (scaled or not): the same map row for the whole line; the tile changes every 8 texels.
        int32_t sy = ry >> 8;
        if (wrap) sy &= h - 1;
        else if ((uint32_t) sy >= (uint32_t) h) return;
        uint32_t map_row = screen_base + (uint32_t) ((sy >> 3) * tiles_w) * (kind == 1 ? 2 : 1);
        uint32_t ty = sy & 7;
        int32_t cur_tile = -1;
        const uint8_t *row = NULL; // the 8 texels of the current tile on this line
        const uint16_t *pal = e->pal;
        bool hflip = false;
        for (int x = 0; x < 256; x++, rx += pa) {
            int32_t sx = rx >> 8;
            if (wrap) sx &= w - 1;
            else if ((uint32_t) sx >= (uint32_t) w) continue;
            int32_t tile_x = sx >> 3;
            if (tile_x != cur_tile) {
                cur_tile = tile_x;
                if (kind == 0) {
                    uint32_t tile = bg8(e, map_row + tile_x);
                    row = bg_at(e, char_base + tile * 64 + ty * 8);
                } else {
                    uint16_t entry = bg16(e, map_row + tile_x * 2);
                    uint32_t r = entry & 0x800 ? 7 - ty : ty;
                    row = bg_at(e, char_base + (entry & 0x3ff) * 64 + r * 8);
                    hflip = entry & 0x400;
                    pal = ext ? ext + (entry >> 12) * 256 : e->pal;
                }
                if (row && !(((const uint32_t *) row)[0] | ((const uint32_t *) row)[1])) row = NULL;
            }
            if (!row) continue;
            uint32_t n = row[hflip ? 7 - (sx & 7) : sx & 7];
            if (n) PUT(e, x, pal[n] & 0x7fff, bg);
        }
        return;
    }
    if (pa == 0 && kind <= 1) {
        // Turned by a quarter (the race map): the same map column for the whole line; the tile changes every 8 texels
        // down the column.
        int32_t sx = rx >> 8;
        if (wrap) sx &= w - 1;
        else if ((uint32_t) sx >= (uint32_t) w) return;
        int32_t tile_x = sx >> 3;
        uint32_t tx = sx & 7;
        int32_t cur_row = -1;
        const uint8_t *tile_data = NULL; // the current tile (64 bytes)
        const uint16_t *pal = e->pal;
        bool vflip = false;
        uint32_t col = tx;
        for (int x = 0; x < 256; x++, ry += pc) {
            int32_t sy = ry >> 8;
            if (wrap) sy &= h - 1;
            else if ((uint32_t) sy >= (uint32_t) h) continue;
            int32_t tile_y = sy >> 3;
            if (tile_y != cur_row) {
                cur_row = tile_y;
                if (kind == 0) {
                    uint32_t tile = bg8(e, screen_base + tile_y * tiles_w + tile_x);
                    tile_data = bg_at(e, char_base + tile * 64);
                    col = tx;
                    vflip = false;
                } else {
                    uint16_t entry = bg16(e, screen_base + (tile_y * tiles_w + tile_x) * 2);
                    tile_data = bg_at(e, char_base + (entry & 0x3ff) * 64);
                    col = entry & 0x400 ? 7 - tx : tx;
                    vflip = entry & 0x800;
                    pal = ext ? ext + (entry >> 12) * 256 : e->pal;
                }
            }
            if (!tile_data) continue;
            uint32_t r = vflip ? 7 - (sy & 7) : sy & 7;
            uint32_t n = tile_data[r * 8 + col];
            if (n) PUT(e, x, pal[n] & 0x7fff, bg);
        }
        return;
    }
    for (int x = 0; x < 256; x++, rx += pa, ry += pc) {
        int32_t sx = rx >> 8, sy = ry >> 8;
        if (wrap) {
            sx &= w - 1;
            sy &= h - 1;
        } else if ((uint32_t) sx >= (uint32_t) w || (uint32_t) sy >= (uint32_t) h) {
            continue;
        }
        uint16_t color;
        if (kind == 0) {
            uint32_t tile = bg8(e, screen_base + (sy >> 3) * tiles_w + (sx >> 3));
            uint32_t n = bg8(e, char_base + tile * 64 + (sy & 7) * 8 + (sx & 7));
            if (!n) continue;
            color = e->pal[n];
        } else if (kind == 1) {
            uint16_t entry = bg16(e, screen_base + ((sy >> 3) * tiles_w + (sx >> 3)) * 2);
            uint32_t tx = sx & 7, ty = sy & 7;
            if (entry & 0x400) tx = 7 - tx;
            if (entry & 0x800) ty = 7 - ty;
            uint32_t n = bg8(e, char_base + (entry & 0x3ff) * 64 + ty * 8 + tx);
            if (!n) continue;
            color = ext ? ext[(entry >> 12) * 256 + n] : e->pal[n];
        } else if (kind == 2) {
            uint32_t n = bg8(e, bitmap_base + sy * w + sx);
            if (!n) continue;
            color = e->pal[n];
        } else {
            uint16_t c = bg16(e, bitmap_base + (sy * w + sx) * 2);
            if (!(c & 0x8000)) continue;
            color = c;
        }
        PUT(e, x, color & 0x7fff, bg);
    }
}

static void bg_line(F2 *e, int bg, int y) {
    uint32_t mode = e->dispcnt & 7;
    if (bg < 2 || (bg == 2 && (mode == 0 || mode == 1 || mode == 3)) || (bg == 3 && mode == 0)) {
        bg_text(e, bg, y);
    } else if ((bg == 2 && (mode == 2 || mode == 1)) || (bg == 3 && (mode == 1 || mode == 2))) {
        bg_affine(e, bg, y, 0);
    } else {
        uint16_t cnt = io16(e, 0x08 + bg * 2);
        bg_affine(e, bg, y, !(cnt & 0x80) ? 1 : (cnt & 4) ? 3 : 2);
    }
}

// ------------------------------------------------------------------------------------------------ sprites
static const uint8_t s_obj_w[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
static const uint8_t s_obj_h[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

// The visible sprites, from the last OAM entry to the first (so that the first ones win when drawn in order).
static void sprites_prepare(F2 *e) {
    const uint16_t *oam = (const uint16_t *) (e->f->oam + (e->is_a ? 0 : 0x400));
    e->sprite_count = 0;
    if (!(e->dispcnt & 0x1000)) return;
    for (int i = 127; i >= 0; i--) {
        const uint16_t *a = oam + i * 4;
        uint16_t a0 = a[0], a1 = a[1], a2 = a[2];
        bool affine = a0 & 0x100;
        if (!affine && (a0 & 0x200)) continue;
        uint32_t shape = (a0 >> 14) & 3, size = (a1 >> 14) & 3, mode = (a0 >> 10) & 3;
        if (shape == 3) continue; // prohibited shape
        Sprite *s = &e->sprites[e->sprite_count];
        s->w = s_obj_w[shape][size];
        s->h = s_obj_h[shape][size];
        s->bw = s->w;
        s->bh = s->h;
        if (affine && (a0 & 0x200)) {
            s->bw *= 2;
            s->bh *= 2;
        }
        int x = a1 & 0x1ff;
        if (x >= 256) x -= 512;
        if (x + s->bw <= 0) continue;
        s->x = (int16_t) x;
        s->y = (int16_t) (a0 & 0xff);
        s->prio = (uint8_t) ((a2 >> 10) & 3);
        s->mode = (uint8_t) mode;
        s->c256 = (a0 & 0x2000) != 0;
        s->affine = affine;
        s->hflip = !affine && (a1 & 0x1000);
        s->vflip = !affine && (a1 & 0x2000);
        s->tile = a2 & 0x3ff;
        s->pal = a2 >> 12;
        s->alpha = (uint8_t) (a2 >> 12);
        s->index = (uint8_t) i;
        if (affine) {
            const uint16_t *p = oam + ((a1 >> 9) & 31) * 16;
            s->pa = (int16_t) p[3];
            s->pb = (int16_t) p[7];
            s->pc = (int16_t) p[11];
            s->pd = (int16_t) p[15];
        }
        e->sprite_count++;
    }
}

// Colour of texel (tx, ty) of a sprite: 0 when transparent, else colour | 0x8000.
static inline uint16_t sprite_texel(const F2 *e, const Sprite *s, int tx, int ty, bool map1d, uint32_t bound) {
    if (s->mode == 3) {
        uint32_t addr;
        if (e->dispcnt & 0x40) {
            addr = s->tile * (128u << ((e->dispcnt >> 22) & 1)) + (ty * s->w + tx) * 2;
        } else {
            uint32_t width = e->dispcnt & 0x20 ? 256 : 128, mask = width / 8 - 1;
            addr = (((s->tile / (width / 8)) * 8 + ty) * width + (s->tile & mask) * 8 + tx) * 2;
        }
        const uint8_t *p = obj_at(e, addr);
        uint16_t c = p ? *(const uint16_t *) p : 0;
        return c & 0x8000 ? c : 0;
    }
    if (s->c256) {
        uint32_t addr = map1d ? s->tile * bound + ((ty >> 3) * (s->w >> 3) + (tx >> 3)) * 64
                              : s->tile * 32 + (ty >> 3) * 32 * 32 + (tx >> 3) * 64;
        const uint8_t *p = obj_at(e, addr + (ty & 7) * 8 + (tx & 7));
        uint32_t n = p ? *p : 0;
        if (!n) return 0;
        uint16_t c = e->objext && (e->dispcnt & 0x80000000u) ? e->objext[s->pal * 256 + n] : e->pal[256 + n];
        return c | 0x8000;
    }
    uint32_t cell = map1d ? (ty >> 3) * (s->w >> 3) + (tx >> 3) : (ty >> 3) * 32 + (tx >> 3);
    const uint8_t *p = obj_at(e, (map1d ? s->tile * bound : s->tile * 32) + cell * 32 + (ty & 7) * 4 + (tx & 7) / 2);
    uint32_t n = p ? (tx & 1 ? *p >> 4 : *p & 15) : 0;
    if (!n) return 0;
    return e->pal[256 + s->pal * 16 + n] | 0x8000;
}

// Draws the sprites of line y into oc/op/oa. Returns the set of priorities present (bit p).
static uint32_t sprites_line(F2 *e, int y) {
    uint32_t prios = 0;
    e->line_semi = false;
    if (!e->sprite_count) return 0;
    memset(e->op, 4, sizeof(e->op));
    if (e->obj_window) memset(e->ow, 0, sizeof(e->ow));
    bool map1d = e->dispcnt & 0x10;
    uint32_t bound = map1d ? 32u << ((e->dispcnt >> 20) & 3) : 32;
    for (int i = 0; i < e->sprite_count; i++) {
        const Sprite *s = &e->sprites[i];
        int dy = (y - s->y) & 0xff;
        if (dy >= s->bh) continue;
        uint8_t alpha = s->mode == 1 ? 0xfe : s->mode == 3 ? s->alpha : 0xff;
        int x0 = s->x < 0 ? -s->x : 0, x1 = s->x + s->bw > 256 ? 256 - s->x : s->bw;
        if (s->affine) {
            int cy = dy - s->bh / 2;
            for (int bx = x0; bx < x1; bx++) {
                int cx = bx - s->bw / 2;
                int tx = ((s->pa * cx + s->pb * cy) >> 8) + s->w / 2;
                int ty = ((s->pc * cx + s->pd * cy) >> 8) + s->h / 2;
                if ((unsigned) tx >= s->w || (unsigned) ty >= s->h) continue;
                uint16_t c = sprite_texel(e, s, tx, ty, map1d, bound);
                int x = s->x + bx;
                if (c && s->mode == 2) {
                    e->ow[x] = 1;
                } else if (c && s->prio <= e->op[x]) {
                    e->oc[x] = c;
                    e->op[x] = s->prio;
                    e->oa[x] = alpha;
                    prios |= 1u << s->prio;
                    e->line_semi |= alpha != 0xff;
                }
            }
        } else if (s->mode != 3) {
            // Regular sprites: one row of character data per 8 pixels.
            int ty = s->vflip ? s->h - 1 - dy : dy;
            for (int bx = x0; bx < x1;) {
                int tx = s->hflip ? s->w - 1 - bx : bx;
                int cell_x = tx >> 3;
                const uint8_t *row;
                if (s->c256) {
                    uint32_t addr = map1d ? s->tile * bound + ((ty >> 3) * (s->w >> 3) + cell_x) * 64
                                          : s->tile * 32 + (ty >> 3) * 32 * 32 + cell_x * 64;
                    row = obj_at(e, addr + (ty & 7) * 8);
                } else {
                    uint32_t cell = map1d ? (ty >> 3) * (s->w >> 3) + cell_x : (ty >> 3) * 32 + cell_x;
                    row = obj_at(e, (map1d ? s->tile * bound : s->tile * 32) + cell * 32 + (ty & 7) * 4);
                }
                // The pixels of this cell: up to the next cell boundary in texel space.
                int run = s->hflip ? (tx & 7) + 1 : 8 - (tx & 7);
                if (run > x1 - bx) run = x1 - bx;
                if (row) {
                    const uint16_t *pal = s->c256 ? (e->objext && (e->dispcnt & 0x80000000u) ? e->objext + s->pal * 256
                                                                                              : e->pal + 256)
                                                  : e->pal + 256 + s->pal * 16;
                    for (int k = 0; k < run; k++) {
                        int t = s->hflip ? tx - k : tx + k;
                        uint32_t n = s->c256 ? row[t & 7] : (row[(t & 7) >> 1] >> ((t & 1) * 4)) & 15;
                        if (!n) continue;
                        int x = s->x + bx + k;
                        if (s->mode == 2) {
                            e->ow[x] = 1;
                        } else if (s->prio <= e->op[x]) {
                            e->oc[x] = pal[n] | 0x8000;
                            e->op[x] = s->prio;
                            e->oa[x] = alpha;
                            prios |= 1u << s->prio;
                            e->line_semi |= alpha != 0xff;
                        }
                    }
                }
                bx += run;
            }
        } else {
            int ty = s->vflip ? s->h - 1 - dy : dy;
            for (int bx = x0; bx < x1; bx++) {
                int tx = s->hflip ? s->w - 1 - bx : bx;
                uint16_t c = sprite_texel(e, s, tx, ty, map1d, bound);
                int x = s->x + bx;
                if (c && s->mode == 2) {
                    e->ow[x] = 1;
                } else if (c && s->prio <= e->op[x]) {
                    e->oc[x] = c;
                    e->op[x] = s->prio;
                    e->oa[x] = alpha;
                    prios |= 1u << s->prio;
                    e->line_semi |= alpha != 0xff;
                }
            }
        }
    }
    return prios;
}

// ------------------------------------------------------------------------------------------------ windows
static void windows_line(F2 *e, int y) {
    uint32_t wins = (e->dispcnt >> 13) & 7;
    uint16_t winin = io16(e, 0x48), winout = io16(e, 0x4a);
    memset(e->mask, winout & 0x3f, 256);
    if ((wins & 4) && e->obj_window) {
        uint8_t m = (winout >> 8) & 0x3f;
        for (int x = 0; x < 256; x++) {
            if (e->ow[x]) e->mask[x] = m;
        }
    }
    for (int w = 1; w >= 0; w--) {
        if (!(wins & (1u << w))) continue;
        uint16_t hreg = io16(e, 0x40 + w * 2), vreg = io16(e, 0x44 + w * 2);
        int x1 = hreg >> 8, x2 = hreg & 0xff, y1 = vreg >> 8, y2 = vreg & 0xff;
        bool in_y = y1 <= y2 ? (y >= y1 && y < y2) : (y >= y1 || y < y2);
        if (!in_y) continue;
        uint8_t m = (winin >> (w * 8)) & 0x3f;
        for (int x = 0; x < 256; x++) {
            int xx = x > 254 ? 254 : x; // a window ending at 255 covers the last column too (as gpu2d.c)
            bool in_x = x1 <= x2 ? (xx >= x1 && xx < x2) : (xx >= x1 || xx < x2);
            if (in_x) e->mask[x] = m;
        }
    }
}

// ------------------------------------------------------------------------------------------------ effects
static inline uint16_t blend555(uint16_t a, uint16_t b, uint32_t ea, uint32_t eb) {
    uint32_t r = ((a & 31) * ea + (b & 31) * eb) >> 4;
    uint32_t g = (((a >> 5) & 31) * ea + ((b >> 5) & 31) * eb) >> 4;
    uint32_t bl = (((a >> 10) & 31) * ea + ((b >> 10) & 31) * eb) >> 4;
    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (bl > 31) bl = 31;
    return (uint16_t) (r | g << 5 | bl << 10);
}
static inline uint16_t brighten555(uint16_t c, uint32_t ey) {
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r += (31 - r) * ey >> 4;
    g += (31 - g) * ey >> 4;
    b += (31 - b) * ey >> 4;
    return (uint16_t) (r | g << 5 | b << 10);
}
static inline uint16_t darken555(uint16_t c, uint32_t ey) {
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r -= r * ey >> 4;
    g -= g * ey >> 4;
    b -= b * ey >> 4;
    return (uint16_t) (r | g << 5 | b << 10);
}
static inline uint32_t to8888(uint16_t c, uint32_t a) {
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return (r << 3 | r >> 2) | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2) << 16 | a << 24;
}

// ------------------------------------------------------------------------------------------------ engine
static void setup(F2 *e, const FrameSnapshot *f, bool is_a) {
    e->f = f;
    e->is_a = is_a;
    e->io = f->io + (is_a ? 0 : 0x1000);
    e->pal = (const uint16_t *) (f->palette + (is_a ? 0 : 0x400));
    e->dispcnt = io32(e, 0);
    static VramMap map;
    vram_map_build(&map, f->vramcnt);
    const uint8_t *base = SNAP_VRAM(f);
    int bg_region = is_a ? VR_BG_A : VR_BG_B, obj_region = is_a ? VR_OBJ_A : VR_OBJ_B;
    for (int i = 0; i < 32; i++) {
        e->bgp[i] = map.page[bg_region][i] >= 0 ? base + map.page[bg_region][i] : NULL;
    }
    for (int i = 0; i < 16; i++) {
        e->objp[i] = map.page[obj_region][i] >= 0 ? base + map.page[obj_region][i] : NULL;
    }
    // Extended palettes: engine A's BG slots are 4 x 8 KiB in 32 KiB (two pages), B's likewise.
    int bgext = is_a ? VR_BGEXT_A : VR_BGEXT_B, objext = is_a ? VR_OBJEXT_A : VR_OBJEXT_B;
    for (int s = 0; s < 4; s++) {
        int32_t pg = map.page[bgext][s / 2];
        e->bgext[s] = pg >= 0 ? (const uint16_t *) (base + pg + (s & 1) * 0x2000) : NULL;
    }
    e->objext = map.page[objext][0] >= 0 ? (const uint16_t *) (base + map.page[objext][0]) : NULL;
}

bool fast2d_render_engine(const FrameSnapshot *f, bool is_a, Fast2dEngineOut *out) {
    F2 *e = &s_f2;
    setup(e, f, is_a);
    uint32_t display = (e->dispcnt >> 16) & 3;
    if (!is_a && display > 1) display = 1;
    if (display == 3) {
        return false; // main memory display
    }
    out->capture = is_a && (f->io[0x67] & 0x80);
    for (int bg = 0; bg < 4; bg++) {
        if ((e->dispcnt & (0x100u << bg)) && (io16(e, 0x08 + bg * 2) & 0x40)) return false; // mosaic
    }
    out->brightness = io16(e, 0x6c);
    out->split = false;
    if ((e->dispcnt & 0x80) || display == 0) { // forced blank / display off: white
        for (int i = 0; i < 256 * 192; i++) out->out[i] = 0xffff;
        return true;
    }
    if (display == 2) { // VRAM display: an LCDC bank as a direct-colour picture
        static VramMap map;
        vram_map_build(&map, f->vramcnt);
        uint32_t bank = (e->dispcnt >> 18) & 3;
        for (int y = 0; y < 192; y++) {
            for (int x = 0; x < 256; x++) {
                const uint8_t *p = vram_ptr(&map, SNAP_VRAM(f), VR_LCDC, bank * 0x20000 + (y * 256 + x) * 2);
                out->out[y * 256 + x] = (p ? *(const uint16_t *) p : 0) | 0x8000;
            }
        }
        return true;
    }

    bool split = is_a && (e->dispcnt & 8) && (e->dispcnt & 0x100) && out->above;
    uint16_t bldcnt = io16(e, 0x50), bldalpha = io16(e, 0x52);
    uint32_t effect = (bldcnt >> 6) & 3;
    uint32_t eva = bldalpha & 31, evb = (bldalpha >> 8) & 31, evy = io16(e, 0x54) & 31;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;
    if ((effect == 2 || effect == 3) && evy == 0) effect = 0;
    if ((bldcnt & 0x3f) == 0 || (effect == 1 && (bldcnt & 0x3f00) == 0)) effect = 0; // no target: no effect
    uint16_t backdrop = e->pal[0] & 0x7fff;
    uint32_t prio[4];
    for (int bg = 0; bg < 4; bg++) prio[bg] = io16(e, 0x08 + bg * 2) & 3;
    bool bg0_3d = is_a && (e->dispcnt & 8);
    sprites_prepare(e);
    e->masked = (e->dispcnt & 0xe000) != 0;
    e->obj_window = false;
    for (int i = 0; i < e->sprite_count; i++) e->obj_window |= e->sprites[i].mode == 2;
    for (int y = 0; y < 192; y++) {
        uint32_t oprios = sprites_line(e, y);
        // The second layer is needed for alpha blending (effect 1) and semi-transparent or bitmap sprites.
        e->second = effect == 1 || e->line_semi;
        bool fx = effect != 0 || e->line_semi;
        for (int x = 0; x < 256; x++) e->c0[x] = backdrop;
        memset(e->l0, L_BD, 256);
        if (e->second) {
            for (int x = 0; x < 256; x++) e->c1[x] = backdrop;
            memset(e->l1, L_BD, 256);
        }
        if (e->masked) windows_line(e, y);
        uint16_t *dst = out->out + y * 256;
        bool in_above = false;
        for (int p = 3; p >= 0; p--) {
            for (int bg = 3; bg >= 0; bg--) {
                if (prio[bg] != (uint32_t) p || !(e->dispcnt & (0x100u << bg))) continue;
                if (bg == 0 && bg0_3d) {
                    if (split) {
                        // Everything so far lies under the 3D layer.
                        for (int x = 0; x < 256; x++) dst[x] = e->c0[x] | 0x8000;
                        memset(e->l0, L_3D, 256);
                        if (e->second) memset(e->l1, L_3D, 256);
                        in_above = true;
                    }
                    continue;
                }
                bg_line(e, bg, y);
            }
            if (oprios & (1u << p)) {
                for (int x = 0; x < 256; x++) {
                    if (e->op[x] == (uint32_t) p) PUT(e, x, e->oc[x] & 0x7fff, L_OBJ);
                }
            }
        }
        if (fx) {
            for (int x = 0; x < 256; x++) {
                uint32_t top = e->l0[x];
                if (top == L_3D) continue;
                uint32_t under = e->second ? e->l1[x] : L_BD;
                bool first = (bldcnt >> (top == L_BD ? 5 : top)) & 1;
                bool second = under == L_3D ? (bldcnt >> 8) & 1 : (bldcnt >> (8 + (under == L_BD ? 5 : under))) & 1;
                if (under == L_3D) continue; // blends with the 3D layer: below
                uint16_t c = e->c0[x];
                bool effects = !e->masked || (e->mask[x] & 0x20);
                if (top == L_OBJ && e->oa[x] != 0xff && second) {
                    uint32_t a = e->oa[x] == 0xfe ? eva : e->oa[x] + 1u, b = e->oa[x] == 0xfe ? evb : 16 - a;
                    c = blend555(c, e->c1[x], a, b);
                } else if (first && effects) {
                    if (effect == 1 && second) c = blend555(c, e->c1[x], eva, evb);
                    else if (effect == 2) c = brighten555(c, evy);
                    else if (effect == 3) c = darken555(c, evy);
                }
                e->c0[x] = c;
            }
        }
        if (in_above) {
            uint32_t *above = out->above + y * 256;
            for (int x = 0; x < 256; x++) {
                uint32_t top = e->l0[x];
                if (top == L_3D) {
                    above[x] = 0;
                    continue;
                }
                uint32_t a = 255;
                if (e->second && e->l1[x] == L_3D && (bldcnt & 0x100)) {
                    // Over the 3D layer with blending: the GPU mixes by alpha.
                    bool first = (bldcnt >> top) & 1;
                    if (top == L_OBJ && e->oa[x] != 0xff) {
                        a = (e->oa[x] == 0xfe ? eva : e->oa[x] + 1u) * 255 / 16;
                    } else if (first && effect == 1) {
                        a = eva * 255 / 16;
                    }
                }
                above[x] = to8888(e->c0[x], a);
            }
        } else {
            for (int x = 0; x < 256; x++) dst[x] = e->c0[x] | 0x8000;
        }
        out->split |= in_above;
    }
    return true;
}
