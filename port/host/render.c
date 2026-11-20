// Host frame output: both screens (top above bottom) as a PNG, drawn by the platform's 2D renderer with the software
// 3D rasteriser for the 3D layer.
#include "port.h"
#include "frame.h"
#include "gpu2d.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];

static uint32_t crc(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    }
    c ^= 0xffffffffu;
    while (n--) c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t) (v >> 24);
    p[1] = (uint8_t) (v >> 16);
    p[2] = (uint8_t) (v >> 8);
    p[3] = (uint8_t) v;
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t hdr[8];
    be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    fwrite(data, 1, len, f);
    uint32_t c = crc(0, (const uint8_t *) type, 4);
    c = crc(c, data, len);
    uint8_t cb[4];
    be32(cb, c);
    fwrite(cb, 1, 4, f);
}

// RGB PNG with stored (uncompressed) deflate blocks.
static void write_png(const char *path, const uint32_t *rgba, int w, int h) {
    size_t raw_len = (size_t) h * (1 + w * 3);
    uint8_t *raw = malloc(raw_len);
    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t) y * (1 + w * 3);
        row[0] = 0;
        for (int x = 0; x < w; x++) {
            uint32_t c = rgba[y * w + x];
            row[1 + x * 3] = (uint8_t) c;
            row[2 + x * 3] = (uint8_t) (c >> 8);
            row[3 + x * 3] = (uint8_t) (c >> 16);
        }
    }
    size_t blocks = (raw_len + 65534) / 65535;
    size_t z_len = 2 + raw_len + blocks * 5 + 4;
    uint8_t *z = malloc(z_len), *o = z;
    *o++ = 0x78;
    *o++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t off = 0; off < raw_len; off += 65535) {
        size_t n = raw_len - off < 65535 ? raw_len - off : 65535;
        *o++ = off + n == raw_len;
        *o++ = (uint8_t) n;
        *o++ = (uint8_t) (n >> 8);
        *o++ = (uint8_t) ~n;
        *o++ = (uint8_t) (~n >> 8);
        memcpy(o, raw + off, n);
        o += n;
    }
    be32(o, (b << 16) | a);
    o += 4;
    FILE *f = fopen(path, "wb");
    if (!f) {
        port_log("cannot write %s", path);
        free(raw);
        free(z);
        return;
    }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t) w);
    be32(ihdr + 4, (uint32_t) h);
    ihdr[8] = 8;
    ihdr[9] = 2;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t) (o - z));
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
}

extern float g_host_aspect;
#ifdef HOST_GL
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include "present.h"

// OpenGL ES output: the same presentation code as on Android, into a pbuffer, read back as PNG.
#define GL_SCALE 3
static bool gl_frame(const FrameSnapshot *f, const char *path) {
    static bool ready, failed;
    static int W, H;
    if (!W) {
        W = (int) (192 * GL_SCALE * g_host_aspect + 0.5f) & ~3;
        H = 384 * GL_SCALE;
    }
    if (failed) return false;
    if (!ready) {
        EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (!eglInitialize(dpy, NULL, NULL)) {
            failed = true;
            return false;
        }
        const EGLint cfg_attr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                   EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
        EGLConfig cfg;
        EGLint n = 0;
        eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n);
        if (n < 1) {
            port_log("host: no EGL config");
            failed = true;
            return false;
        }
        const EGLint pb_attr[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE};
        EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb_attr);
        eglBindAPI(EGL_OPENGL_ES_API);
        const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
        eglMakeCurrent(dpy, surf, surf, ctx);
        port_log("host: GL %s", (const char *) glGetString(GL_RENDERER));
        present_init();
        ready = true;
    }
    if (getenv("MKDS_DROPCAP") && f->number % 3 == 0) ((FrameSnapshot *) f)->io[0x67] &= 0x7f; // tests: as on phones
    present_update(f, GL_SCALE);
    present_set_wide(g_host_aspect);
    const int layout[8] = {0, 0, W, H / 2, 0, H / 2, W, H / 2};
    const char *interp = getenv("MKDS_INTERP"); // tests: draw the 3D between the last two frames
    present_draw(layout, W, H, interp ? (float) atof(interp) : 1.0f);
    static uint32_t pixels[768 * GL_SCALE * 384 * GL_SCALE], flipped[768 * GL_SCALE * 384 * GL_SCALE];
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    for (int y = 0; y < H; y++) memcpy(flipped + y * W, pixels + (H - 1 - y) * W, W * 4);
    write_png(path, flipped, W, H);
    return true;
}
#endif

float g_host_aspect = 4.0f / 3.0f; // --wide: the top screen's aspect ratio (OpenGL output only)

void host_write_frame(const FrameSnapshot *f, const char *path) {
#ifdef HOST_GL
    if (gl_frame(f, path)) return;
#endif
    static uint32_t screens[2][256 * 192];
    static uint32_t layer3d[256 * 192];
    soft3d_render(&f->frame3d, f, layer3d);
    gpu2d_render(f, layer3d, screens[0], screens[1], 0);
    static uint32_t out[256 * 384];
    memcpy(out, screens[0], sizeof(screens[0]));
    memcpy(out + 256 * 192, screens[1], sizeof(screens[1]));
    write_png(path, out, 256, 384);
}

// Tests: the fast 2D renderer (platform/fast2d.c) against the complete one, pixel by pixel (BGR555, no 3D layer, no
// master brightness).
#include "fast2d.h"
void fast2d_check(const FrameSnapshot *f, uint64_t n) {
    static uint32_t ref[2][256 * 192];
    static uint16_t fast[256 * 192];
    FrameSnapshot *g = (FrameSnapshot *) f;
    uint16_t mb[2] = {*(uint16_t *) (g->io + 0x6c), *(uint16_t *) (g->io + 0x106c)};
    *(uint16_t *) (g->io + 0x6c) = *(uint16_t *) (g->io + 0x106c) = 0;
    gpu2d_render(f, NULL, ref[0], ref[1], 0);
    bool a_top = gpu2d_engine_a_on_top(f);
    for (int eng = 0; eng < 2; eng++) {
        Fast2dEngineOut o = {fast, NULL, false, 0};
        if (!fast2d_render_engine(f, eng == 0, &o)) {
            port_log("f2check %llu engine %c: not supported", (unsigned long long) n, 'A' + eng);
            continue;
        }
        const uint32_t *r = ref[(eng == 0) == a_top ? 0 : 1];
        int bad = 0, first = -1;
        for (int i = 0; i < 256 * 192; i++) {
            uint32_t c = r[i];
            uint16_t c555 = (uint16_t) (((c >> 3) & 31) | ((c >> 11) & 31) << 5 | ((c >> 19) & 31) << 10);
            int d = 0; // per channel, rounding differences of blends ignored
            for (int k = 0; k < 15; k += 5) {
                int u = (fast[i] >> k) & 31, v = (c555 >> k) & 31;
                if (u - v > d) d = u - v;
                if (v - u > d) d = v - u;
            }
            if (d > 1) {
                if (first < 0) first = i;
                bad++;
            }
        }
        port_log("f2check %llu scene %d engine %c: %d pixels differ%s", (unsigned long long) n, f->scene, 'A' + eng, bad,
                 bad ? "" : " (ok)");
        if (bad) {
            port_log("  first at %d,%d: fast %04x ref %06x", first % 256, first / 256, fast[first], r[first] & 0xffffff);
            if (getenv("MKDS_F2OAM")) {
                const uint16_t *oam = (const uint16_t *) (f->oam + eng * 0x400);
                for (int i = 0; i < 128; i++) {
                    uint16_t a0 = oam[i * 4], a1 = oam[i * 4 + 1], a2 = oam[i * 4 + 2];
                    if (!(a0 & 0x100) && (a0 & 0x200)) continue;
                    int x = a1 & 0x1ff;
                    if (x >= 256) x -= 512;
                    port_log("  oam %d x %d y %d shape %d size %d aff %d flip %d%d mode %d c256 %d tile %d prio %d", i, x,
                             a0 & 0xff, a0 >> 14, a1 >> 14, (a0 >> 8) & 1, (a1 >> 12) & 1, (a1 >> 13) & 1,
                             (a0 >> 10) & 3, (a0 >> 13) & 1, a2 & 0x3ff, (a2 >> 10) & 3);
                }
            }
            if (getenv("MKDS_F2DUMP")) {
                static uint32_t img[256 * 384];
                for (int i = 0; i < 256 * 192; i++) {
                    uint16_t c = fast[i];
                    img[i] = r[i];
                    img[256 * 192 + i] = ((c & 31) << 3) | (((c >> 5) & 31) << 11) | (((c >> 10) & 31) << 19);
                }
                char path[256];
                snprintf(path, sizeof(path), "%s/f2_%05llu_%c.png", getenv("MKDS_F2DUMP"), (unsigned long long) n, 'A' + eng);
                write_png(path, img, 256, 384);
            }
        }
    }
    *(uint16_t *) (g->io + 0x6c) = mb[0];
    *(uint16_t *) (g->io + 0x106c) = mb[1];
}
