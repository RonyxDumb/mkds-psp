// The state of one displayed frame, captured at VBlank for the renderer thread.
#pragma once

#include "gx.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FrameSnapshot {
    uint8_t io[0x1070];          // 0x04000000..: both 2D engines' registers (engine B at 0x1000)
    uint8_t palette[0x800];      // 0x05000000
    uint8_t oam[0x800];          // 0x07000000
#ifndef PSP
    uint8_t vram[0xa4000];       // banks A..I (see g_vram_bank_offset)
#endif
    uint8_t vramcnt[10];         // 0x04000240..0x04000249
    uint8_t scene;               // the game's scene number (2: race)
    uint64_t time_ns;            // when it was captured (CLOCK_MONOTONIC, host time)
#ifndef PSP
    Gx3dFrame frame3d;           // displayed 3D list (only the used vertices and polygons are copied)
#endif
    uint64_t number;
} FrameSnapshot;

// The PSP draws each frame on the game thread at VBlank (psp/main.c): the renderers read the live VRAM banks (their
// VRAM maps hold addresses, the base is NULL) and the live 3D list instead of copies.
#ifdef PSP
#define SNAP_VRAM(f) ((const uint8_t *) 0)
#define SNAP_3D(f) gx3d_displayed()
#else
#define SNAP_VRAM(f) ((const uint8_t *) (f)->vram)
#define SNAP_3D(f) (&(f)->frame3d)
#endif

// Triple buffering between the game thread (writer) and the renderer thread (reader).
// Returns the newest complete snapshot not returned before, or NULL; it stays valid until the next call.
const FrameSnapshot *port_frame_acquire(void);
// Optional hook run on the game thread after each capture (tests).
extern void (*g_frame_hook)(const FrameSnapshot *snapshot);
extern void *g_lockstep_ready, *g_lockstep_done;

#ifdef __cplusplus
}
#endif
