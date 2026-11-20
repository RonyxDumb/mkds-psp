// PSP controls → DS buttons and touch screen.
//
//   D-pad / analog stick  D-pad          Cross     A        Circle    B
//   Square                Y              Triangle  X        L / R     L / R
//   START                 START          SELECT    SELECT (on release, when used alone)
//   SELECT + analog stick moves the touch pointer, SELECT + Cross touches the touch screen
//   SELECT + L / SELECT + R   previous / next screen layout
#include "port.h"

#include <pspctrl.h>
#include <pspgu.h>
#include <string.h>

#include "psp_port.h"

static float s_px = 128, s_py = 96; // touch pointer, DS touch screen coordinates
static bool s_pointer_shown;
static bool s_select_used;
static uint32_t s_prev;
static int s_select_tap; // frames left of a SELECT press sent to the game

void input_init(void) {
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
}

void input_poll(void) {
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);
    uint32_t b = pad.Buttons, pressed = b & ~s_prev;
    s_prev = b;
    int ax = (int) pad.Lx - 128, ay = (int) pad.Ly - 128;
    const int dead = 40;

    PortInput in;
    memset(&in, 0, sizeof(in));
    bool select = b & PSP_CTRL_SELECT;
    if (select) {
        if (pressed & PSP_CTRL_LTRIGGER) {
            g_layout = (g_layout < 0 ? 0 : g_layout + LAYOUT_COUNT - 1) % LAYOUT_COUNT;
            s_select_used = true;
        }
        if (pressed & PSP_CTRL_RTRIGGER) {
            g_layout = (g_layout < 0 ? 0 : g_layout + 1) % LAYOUT_COUNT;
            s_select_used = true;
        }
        // Touch pointer.
        float speed = 4.0f / 128;
        if (ax > dead || ax < -dead) s_px += (float) (ax - (ax > 0 ? dead : -dead)) * speed;
        if (ay > dead || ay < -dead) s_py += (float) (ay - (ay > 0 ? dead : -dead)) * speed;
        if (b & PSP_CTRL_LEFT) s_px -= 2;
        if (b & PSP_CTRL_RIGHT) s_px += 2;
        if (b & PSP_CTRL_UP) s_py -= 2;
        if (b & PSP_CTRL_DOWN) s_py += 2;
        if (s_px < 0) s_px = 0;
        if (s_px > 255) s_px = 255;
        if (s_py < 0) s_py = 0;
        if (s_py > 191) s_py = 191;
        if (ax > dead || ax < -dead || ay > dead || ay < -dead || (b & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT |
                                                                       PSP_CTRL_RIGHT | PSP_CTRL_CROSS))) {
            s_select_used = true;
        }
        if (b & PSP_CTRL_CROSS) {
            in.touching = true;
            in.touch_x = (uint16_t) s_px;
            in.touch_y = (uint16_t) s_py;
        }
        s_pointer_shown = true;
    } else {
        if (s_pointer_shown && !s_select_used) {
            s_select_tap = 4; // SELECT alone: a SELECT press for the game
        }
        s_pointer_shown = false;
        s_select_used = false;
        if (b & PSP_CTRL_CROSS) in.buttons |= PAD_A;
        if (b & PSP_CTRL_CIRCLE) in.buttons |= PAD_B;
        if (b & PSP_CTRL_SQUARE) in.buttons |= PAD_Y;
        if (b & PSP_CTRL_TRIANGLE) in.buttons |= PAD_X;
        if (b & PSP_CTRL_LTRIGGER) in.buttons |= PAD_L;
        if (b & PSP_CTRL_RTRIGGER) in.buttons |= PAD_R;
        if (b & PSP_CTRL_START) in.buttons |= PAD_START;
        if ((b & PSP_CTRL_UP) || ay < -dead * 2) in.buttons |= PAD_UP;
        if ((b & PSP_CTRL_DOWN) || ay > dead * 2) in.buttons |= PAD_DOWN;
        if ((b & PSP_CTRL_LEFT) || ax < -dead) in.buttons |= PAD_LEFT;
        if ((b & PSP_CTRL_RIGHT) || ax > dead) in.buttons |= PAD_RIGHT;
    }
    if (s_select_tap > 0) {
        in.buttons |= PAD_SELECT;
        s_select_tap--;
    }
    port_input_set(&in);
}

typedef struct PointerVertex {
    unsigned int color;
    float x, y, z;
} PointerVertex;

void input_draw_pointer(void) {
    ScreenRect r;
    if (!s_pointer_shown || !video_touch_rect(&r)) return;
    float x = r.x + (s_px + 0.5f) * r.w / 256, y = r.y + (s_py + 0.5f) * r.h / 192;
    PointerVertex *v = sceGuGetMemory(sizeof(PointerVertex) * 8);
    unsigned int c = (s_prev & PSP_CTRL_CROSS) ? 0xff0000ff : 0xffffffff, k = 0xff000000;
    v[0] = (PointerVertex) {k, x - 6, y - 1, 0};
    v[1] = (PointerVertex) {k, x + 7, y + 2, 0};
    v[2] = (PointerVertex) {k, x - 1, y - 6, 0};
    v[3] = (PointerVertex) {k, x + 2, y + 7, 0};
    v[4] = (PointerVertex) {c, x - 5, y, 0};
    v[5] = (PointerVertex) {c, x + 6, y + 1, 0};
    v[6] = (PointerVertex) {c, x, y - 5, 0};
    v[7] = (PointerVertex) {c, x + 1, y + 6, 0};
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 8, 0, v);
    sceGuEnable(GU_TEXTURE_2D);
}
