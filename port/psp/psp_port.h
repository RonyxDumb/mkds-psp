// The PSP side of the port: shared between the files of port/psp.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "frame.h"

// Thread priorities (lower runs first): sound must never starve, the game threads take the rest.
#define PSP_PRIO_AUDIO 0x10
#define PSP_PRIO_MAIN 0x18
#define PSP_PRIO_GAME 0x20
#define PSP_PRIO_RENDER 0x30 // below the game: drawing takes the time the game leaves

void psp_thread_start(const char *name, void (*fn)(void *), void *arg, int priority, int stack);
void psp_log_open(const char *path);
void psp_show_fatal(void); // shows the last log lines on screen (port_fatal)

// video.c
void video_init(void);
void video_present(const FrameSnapshot *f); // renders both screens and shows them
void video_message(const char *text);       // a full-screen text message (missing ROM, errors)

// Screen layouts (SELECT + L / R cycles through them).
enum {
    LAYOUT_FULL,         // the top screen on the whole PSP screen (16:9: races are projected wide)
    LAYOUT_TOP_LARGE,    // top screen as large as it fits, the touch screen small on the right
    LAYOUT_BOTTOM_LARGE, // the same with the screens swapped
    LAYOUT_TOP_ONLY,     // only the top screen, full height
    LAYOUT_SIDE_BY_SIDE, // both screens at their DS size
    LAYOUT_COUNT
};
extern int g_layout;
// Where the touch screen is on the PSP screen (for the pointer), in PSP pixels.
typedef struct ScreenRect {
    float x, y, w, h;
} ScreenRect;
bool video_touch_rect(ScreenRect *out); // false when the touch screen is not shown

// input.c
void input_init(void);
void input_poll(void);   // reads the pad, feeds the game; called once per frame
void input_draw_pointer(void); // draws the touch pointer (inside the GU frame)

// audio.c
void audio_init(void);
