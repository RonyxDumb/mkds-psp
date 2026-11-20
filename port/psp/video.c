// Video on the PSP: the two DS screens, rendered by the platform layer into textures, scaled onto the 480x272
// display by the GE.
#include "port.h"
#include "gpu2d.h"
#include "fast2d.h"
#include "ge3d.h"

#include <pspdisplay.h>
#include <pspgu.h>
#include <pspge.h>
#include <pspkernel.h>
#include <pspdebug.h>
#include <malloc.h>
#include <string.h>

#include "psp_port.h"

#define BUF_WIDTH 512
#define SCR_WIDTH 480
#define SCR_HEIGHT 272

static unsigned int __attribute__((aligned(64))) s_list[256 * 1024 / 4];
static uint32_t *s_screen_tex[2]; // top, bottom: 256x192 RGBA (0xAABBGGRR), stride 256
static uint32_t *s_layer3d;
static bool s_debug_screen;
int g_layout = -1; // -1: automatic (by scene)

typedef struct Vertex {
    float u, v;
    float x, y, z;
} Vertex;

void video_init(void) {
    for (int i = 0; i < 2; i++) {
        s_screen_tex[i] = memalign(64, 256 * 192 * 4);
        memset(s_screen_tex[i], 0, 256 * 192 * 4);
    }
    s_layer3d = memalign(64, 256 * 192 * 4);

    // Two 32-bit framebuffers and no depth buffer, in the PSP's VRAM.
    sceGuInit();
    sceGuStart(GU_DIRECT, s_list);
    sceGuDrawBuffer(GU_PSM_8888, (void *) 0, BUF_WIDTH);
    sceGuDispBuffer(SCR_WIDTH, SCR_HEIGHT, (void *) (BUF_WIDTH * SCR_HEIGHT * 4), BUF_WIDTH);
    sceGuDepthBuffer((void *) (BUF_WIDTH * SCR_HEIGHT * 4 * 2), BUF_WIDTH);
    sceGuDepthMask(GU_TRUE); // the 2D layers do not write depth
    sceGuOffset(2048 - SCR_WIDTH / 2, 2048 - SCR_HEIGHT / 2);
    sceGuViewport(2048, 2048, SCR_WIDTH, SCR_HEIGHT);
    sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

static int current_layout(const FrameSnapshot *f) {
    if (g_layout >= 0) return g_layout;
    // Races: the top screen large. Menus have their buttons on the touch screen: both screens side by side.
    return f->scene == 2 ? LAYOUT_FULL : LAYOUT_SIDE_BY_SIDE;
}

static int s_shown_layout = LAYOUT_SIDE_BY_SIDE;

// Where each DS screen goes for a layout: top, bottom (w == 0: not shown).
static void layout_rects(int layout, ScreenRect *top, ScreenRect *bottom) {
    const float big_w = 272.0f * 4 / 3; // 362.67
    switch (layout) {
    case LAYOUT_FULL:
        *top = (ScreenRect) {0, 0, 480, 272};
        *bottom = (ScreenRect) {0, 0, 0, 0};
        break;
    case LAYOUT_TOP_LARGE:
        *top = (ScreenRect) {0, 0, big_w, 272};
        *bottom = (ScreenRect) {big_w, 272 - (480 - big_w) * 0.75f, 480 - big_w, (480 - big_w) * 0.75f};
        break;
    case LAYOUT_BOTTOM_LARGE:
        *bottom = (ScreenRect) {480 - big_w, 0, big_w, 272};
        *top = (ScreenRect) {0, 0, 480 - big_w, (480 - big_w) * 0.75f};
        break;
    case LAYOUT_TOP_ONLY:
        *top = (ScreenRect) {(480 - big_w) / 2, 0, big_w, 272};
        *bottom = (ScreenRect) {0, 0, 0, 0};
        break;
    default: // side by side, each 240x180
        *top = (ScreenRect) {0, 46, 240, 180};
        *bottom = (ScreenRect) {240, 46, 240, 180};
        break;
    }
}

bool video_touch_rect(ScreenRect *out) {
    ScreenRect top, bottom;
    layout_rects(s_shown_layout, &top, &bottom);
    *out = bottom;
    return bottom.w > 0;
}

// Draws a 256x192 texture into a rectangle, in vertical strips (the GE's texture cache works best with narrow ones).
static void draw_texture(const void *tex, int psm, ScreenRect r) {
    if (r.w <= 0) return;
    sceGuTexMode(psm, 0, 0, 0);
    sceGuTexImage(0, 256, 256, 256, tex);
    const int strips = 8;
    Vertex *v = sceGuGetMemory(sizeof(Vertex) * 2 * strips);
    for (int i = 0; i < strips; i++) {
        float u0 = 256.0f * i / strips, u1 = 256.0f * (i + 1) / strips;
        v[i * 2] = (Vertex) {u0, 0, r.x + r.w * i / strips, r.y, 0};
        v[i * 2 + 1] = (Vertex) {u1, 192, r.x + r.w * (i + 1) / strips, r.y + r.h, 0};
    }
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2 * strips, 0, v);
}

// The same with the texture already bound (256x192 texels of it).
static void draw_texture_bound(ScreenRect r) {
    const int strips = 8;
    Vertex *v = sceGuGetMemory(sizeof(Vertex) * 2 * strips);
    for (int i = 0; i < strips; i++) {
        float u0 = 256.0f * i / strips, u1 = 256.0f * (i + 1) / strips;
        v[i * 2] = (Vertex) {u0, 0, r.x + r.w * i / strips, r.y, 0};
        v[i * 2 + 1] = (Vertex) {u1, 192, r.x + r.w * (i + 1) / strips, r.y + r.h, 0};
    }
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2 * strips, 0, v);
}

typedef struct ColorVertex {
    unsigned int color;
    float x, y, z;
} ColorVertex;

// MASTER_BRIGHT: brighten = blend towards white, darken = towards black, by factor / 16.
static void draw_brightness(uint16_t reg, ScreenRect r) {
    uint32_t mode = reg >> 14, factor = reg & 31;
    if (factor > 16) factor = 16;
    if (!factor || mode == 0 || mode == 3 || r.w <= 0) return;
    unsigned int a = factor * 255 / 16;
    unsigned int color = (a << 24) | (mode == 1 ? 0xffffff : 0);
    ColorVertex *v = sceGuGetMemory(sizeof(ColorVertex) * 2);
    v[0] = (ColorVertex) {color, r.x, r.y, 0};
    v[1] = (ColorVertex) {color, r.x + r.w, r.y + r.h, 0};
    sceGuDisable(GU_TEXTURE_2D);
    sceGuEnable(GU_BLEND);
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_TEXTURE_2D);
}

// The 2D engines' outputs, twice: the CPU draws the next frame's while the GE still shows from the other ones.
static uint16_t *s_engine[2][2]; // [engine][buffer], BGR555 | 0x8000 (GU_PSM_5551)
static uint32_t *s_above[2];     // engine A above the 3D layer
static int s_engine_buf[2];      // the buffer each engine was last drawn into
static Fast2dEngineOut s_engine_out[2];
static bool s_engine_valid[2];
static int s_frame_parity;
static bool s_list_pending;              // the GE is drawing a frame that has not been shown yet
static void *s_draw_buffer = (void *) 0; // the GE's current draw buffer (VRAM offset)
uint64_t g_video_stats[4]; // us: engine A, engine B, 3D lists, waiting for the GE
// Display capture: engine A is drawn at the DS size into this buffer, read back into the VRAM bank, then shown scaled.
#define CAPTURE_BUFFER ((void *) (BUF_WIDTH * SCR_HEIGHT * 4 * 2 + BUF_WIDTH * SCR_HEIGHT * 2))

extern int g_port_aspect;
#define ASPECT_4_3 0x1555
#define ASPECT_PSP 0x1c3c // 480 / 272

// The 2D output of one engine: drawn now into the buffer the GE is not reading, or the last one again.
static bool engine_output(const FrameSnapshot *f, int eng, bool redraw, Fast2dEngineOut *out) {
    if (!redraw && s_engine_valid[eng]) {
        *out = s_engine_out[eng];
        out->brightness = (uint16_t) (f->io[eng * 0x1000 + 0x6c] | f->io[eng * 0x1000 + 0x6d] << 8);
        return true;
    }
    int buf = s_engine_buf[eng] ^ 1;
    Fast2dEngineOut o = {s_engine[eng][buf], eng == 0 ? s_above[buf] : NULL, false, 0, false};
    if (!fast2d_render_engine(f, eng == 0, &o)) {
        s_engine_valid[eng] = false;
        return false;
    }
    sceKernelDcacheWritebackRange(o.out, 256 * 192 * 2);
    if (o.split) sceKernelDcacheWritebackRange(o.above, 256 * 192 * 4);
    s_engine_buf[eng] = buf;
    s_engine_out[eng] = o;
    s_engine_valid[eng] = true;
    *out = o;
    return true;
}

// Waits for the GE's last frame and shows it.
static void finish_pending(void) {
    if (!s_list_pending) return;
    uint64_t t = sceKernelGetSystemTimeWide();
    sceGuSync(0, 0);
    g_video_stats[3] += sceKernelGetSystemTimeWide() - t;
    s_draw_buffer = sceGuSwapBuffers();
    s_list_pending = false;
}

void video_present(const FrameSnapshot *f) {
    if (s_debug_screen) return;
    if (!s_engine[0][0]) {
        for (int b = 0; b < 2; b++) {
            for (int e = 0; e < 2; e++) s_engine[e][b] = memalign(64, 256 * 192 * 2);
            s_above[b] = memalign(64, 256 * 192 * 4);
        }
    }
    static unsigned s_drawn;
    s_drawn++;
    s_frame_parity = s_drawn & 1;
    s_shown_layout = current_layout(f);
    ScreenRect top, bottom;
    layout_rects(s_shown_layout, &top, &bottom);
    // The race camera's aspect ratio is set when a race starts: wide when races are shown on the whole screen.
    if (f->scene != 2) {
        int race_layout = g_layout >= 0 ? g_layout : LAYOUT_FULL;
        g_port_aspect = race_layout == LAYOUT_FULL ? ASPECT_PSP : ASPECT_4_3;
    }
    bool a_top = gpu2d_engine_a_on_top(f);
    ScreenRect rect_a = a_top ? top : bottom, rect_b = a_top ? bottom : top;

    // 2D on the CPU while the GE finishes the last frame. In races the 2D layers change little (HUD, map): the top
    // screen's are drawn every other frame, a screen shown small every third, a screen not shown not at all.
    bool race = f->scene == 2;
    bool capture = f->io[0x67] & 0x80;
    Fast2dEngineOut out[2];
    uint64_t t0 = sceKernelGetSystemTimeWide();
    bool redraw_a = !race || !s_frame_parity || capture || rect_a.w < 1;
    bool fast = engine_output(f, 0, redraw_a, &out[0]);
    uint64_t t1 = sceKernelGetSystemTimeWide();
    if (fast) {
        bool shown_b = rect_b.w > 0;
        bool redraw_b = shown_b && (rect_b.w >= rect_a.w || s_drawn % 3 == 0);
        if (!shown_b && s_engine_valid[1]) {
            out[1] = s_engine_out[1];
        } else {
            fast = engine_output(f, 1, redraw_b, &out[1]);
        }
    }
    uint64_t t2 = sceKernelGetSystemTimeWide();
    g_video_stats[0] += t1 - t0;
    g_video_stats[1] += t2 - t1;
    if (!fast) {
        // The complete renderer, 3D included.
        soft3d_render(SNAP_3D(f), f, s_layer3d);
        gpu2d_render(f, s_layer3d, s_screen_tex[0], s_screen_tex[1], 0);
        finish_pending(); // the GE may still read these buffers
        sceKernelDcacheWritebackRange(s_screen_tex[0], 256 * 192 * 4);
        sceKernelDcacheWritebackRange(s_screen_tex[1], 256 * 192 * 4);
    }

    // The last frame is done (it was drawing while the CPU worked): show it, and draw this one.
    finish_pending();
    sceGuStart(GU_DIRECT, s_list);
    sceGuClearColor(0xff000000);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    sceGuTexFlush();
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    if (!fast) {
        draw_texture(s_screen_tex[0], GU_PSM_8888, top);
        draw_texture(s_screen_tex[1], GU_PSM_8888, bottom);
    } else {
        for (int eng = 0; eng < 2; eng++) {
            ScreenRect r = eng == 0 ? rect_a : rect_b;
            if (r.w <= 0 && !(eng == 0 && out[0].capture)) continue;
            ScreenRect shown = r;
            if (eng == 0 && out[0].capture) {
                sceGuDrawBufferList(GU_PSM_8888, CAPTURE_BUFFER, BUF_WIDTH);
                r = (ScreenRect) {0, 0, 256, 192};
                sceGuScissor(0, 0, 256, 192);
            }
            draw_texture(out[eng].out, GU_PSM_5551, r);
            if (eng == 0 && out[0].split) {
                uint64_t t3 = sceKernelGetSystemTimeWide();
                ge3d_draw(SNAP_3D(f), f, r);
                g_video_stats[2] += sceKernelGetSystemTimeWide() - t3;
                sceGuEnable(GU_BLEND);
                draw_texture(out[0].above, GU_PSM_8888, r);
                sceGuDisable(GU_BLEND);
            }
            if (eng == 0 && out[0].capture) {
                // Read back what engine A shows (before its master brightness) into the VRAM bank.
                sceGuFinish();
                sceGuSync(0, 0);
                static uint32_t capture_px[256 * 192];
                const uint32_t *fb = (const uint32_t *) ((uintptr_t) sceGeEdramGetAddr() + (uintptr_t) CAPTURE_BUFFER);
                fb = (const uint32_t *) ((uintptr_t) fb | 0x40000000u); // uncached
                for (int y = 0; y < 192; y++) memcpy(capture_px + y * 256, fb + y * BUF_WIDTH, 256 * 4);
                gpu2d_capture(f, capture_px, capture_px);
                sceGuStart(GU_DIRECT, s_list);
                sceGuDrawBufferList(GU_PSM_8888, s_draw_buffer, BUF_WIDTH);
                sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
                if (shown.w <= 0) continue;
                sceGuTexMode(GU_PSM_8888, 0, 0, 0);
                sceGuTexImage(0, 512, 256, BUF_WIDTH, (void *) ((uintptr_t) sceGeEdramGetAddr() + (uintptr_t) CAPTURE_BUFFER));
                sceGuTexFlush();
                draw_texture_bound(shown);
                r = shown;
            }
            draw_brightness(out[eng].brightness, r);
        }
    }
    input_draw_pointer();
    sceGuFinish();
    s_list_pending = true; // shown at the next frame, once the GE is done
}

void video_message(const char *text) {
    if (!s_debug_screen) {
        sceGuDisplay(GU_FALSE);
        pspDebugScreenInit();
        s_debug_screen = true;
    }
    pspDebugScreenPrintf("%s", text);
}
