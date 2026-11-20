// A fast renderer for the DS 2D engines (the PSP's): tile by tile instead of pixel by pixel, BGR555 output (the
// PSP's 5551 texture format), the 3D layer and the master brightness left to the GPU. It covers what the game uses
// in its races and menus; for anything else (windows, mosaic, main memory display) it says no and the caller uses the
// complete renderer (gpu2d.c).
#pragma once

#include "gpu2d.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Fast2dEngineOut {
    // Engine output, 256x192, BGR555 with bit 15 set (opaque). With the 3D layer (engine A, BG0 3D): what lies
    // under the 3D layer.
    uint16_t *out;
    // Engine A with the 3D layer: what lies above it, 0xAABBGGRR (alpha: opacity over the 3D layer), 256x192.
    uint32_t *above;
    bool split;          // set when `above` was written (the 3D layer is shown between `out` and `above`)
    uint16_t brightness; // MASTER_BRIGHT, for the GPU to apply
    bool capture;        // engine A: display capture is on (DISPCAPCNT): the caller captures what it shows
    bool above_5551;     // `above` holds BGR555 | 0x8000 texels (no blending with the 3D layer), 0 = transparent
} Fast2dEngineOut;

// Renders engine A (is_a) or B. Returns false when the engine uses something this renderer does not do. Display
// capture is left to the caller (it needs the 3D layer): see Fast2dEngineOut.capture.
bool fast2d_render_engine(const FrameSnapshot *f, bool is_a, Fast2dEngineOut *out);

#ifdef __cplusplus
}
#endif
