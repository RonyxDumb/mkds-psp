// Video on the PSP: the two DS screens, rendered by the platform layer into textures, scaled onto the 480x272
// display by the GE.
#include "port.h"
#include "gpu2d.h"
#include "fast2d.h"
#include "ge3d.h"

#include <pspdisplay.h>
#include <pspgu.h>
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
    return f->scene == 2 ? LAYOUT_TOP_LARGE : LAYOUT_SIDE_BY_SIDE;
}

static int s_shown_layout = LAYOUT_SIDE_BY_SIDE;

// Where each DS screen goes for a layout: top, bottom (w == 0: not shown).
static void layout_rects(int layout, ScreenRect *top, ScreenRect *bottom) {
    const float big_w = 272.0f * 4 / 3; // 362.67
    switch (layout) {
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

static uint16_t *s_engine[2];  // fast2d outputs, BGR555 | 0x8000 (GU_PSM_5551)
static uint32_t *s_above;      // engine A above the 3D layer
static int s_frame_parity;

void video_present(const FrameSnapshot *f) {
    if (s_debug_screen) return;
    if (!s_engine[0]) {
        for (int i = 0; i < 2; i++) s_engine[i] = memalign(64, 256 * 192 * 2);
        s_above = memalign(64, 256 * 192 * 4);
    }
    s_frame_parity ^= 1;
    Fast2dEngineOut out[2] = {{s_engine[0], s_above, false, 0}, {s_engine[1], NULL, false, 0}};
    bool fast = fast2d_render_engine(f, true, &out[0]) && fast2d_render_engine(f, false, &out[1]);
    bool a_top = gpu2d_engine_a_on_top(f);
    bool layer3d = false;
    if (!fast) {
        // The complete renderer, 3D included.
        soft3d_render(SNAP_3D(f), f, s_layer3d);
        gpu2d_render(f, s_layer3d, s_screen_tex[0], s_screen_tex[1], 0);
        sceKernelDcacheWritebackRange(s_screen_tex[0], 256 * 192 * 4);
        sceKernelDcacheWritebackRange(s_screen_tex[1], 256 * 192 * 4);
    } else {
        sceKernelDcacheWritebackRange(s_engine[0], 256 * 192 * 2);
        sceKernelDcacheWritebackRange(s_engine[1], 256 * 192 * 2);
        if (out[0].split) {
            sceKernelDcacheWritebackRange(s_above, 256 * 192 * 4);
            layer3d = true;
        }
    }

    s_shown_layout = current_layout(f);
    ScreenRect top, bottom;
    layout_rects(s_shown_layout, &top, &bottom);

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
            ScreenRect r = (eng == 0) == a_top ? top : bottom;
            draw_texture(s_engine[eng], GU_PSM_5551, r);
            if (eng == 0 && out[0].split) {
                sceGuEnable(GU_BLEND);
                sceGuDisable(GU_BLEND);
                if (layer3d) ge3d_draw(SNAP_3D(f), f, r);
                sceGuEnable(GU_BLEND);
                draw_texture(s_above, GU_PSM_8888, r);
                sceGuDisable(GU_BLEND);
            }
            draw_brightness(out[eng].brightness, r);
        }
    }
    input_draw_pointer();
    sceGuFinish();
    sceGuSync(0, 0);
    sceGuSwapBuffers();
}

void video_message(const char *text) {
    if (!s_debug_screen) {
        sceGuDisplay(GU_FALSE);
        pspDebugScreenInit();
        s_debug_screen = true;
    }
    pspDebugScreenPrintf("%s", text);
}
