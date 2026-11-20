// The DS 3D layer on the PSP's GE (ge3d.c).
#pragma once

#include "frame.h"
#include "psp_port.h"

// Draws the 3D list into the screen rectangle r of the current GE frame, over what is there (the 2D layers under
// the 3D layer), with its own depth test.
void ge3d_draw(const Gx3dFrame *f, const FrameSnapshot *snap, ScreenRect r);
