// PORT: the decomp's register map, plus what the native platform layer needs on top of it.
// The I/O space (0x04000000) is ordinary memory reserved at its DS address (port/platform/memory.c). Registers whose
// reads must be computed are redirected here; writes with side effects are rewritten by tools/portify.py into
// PORT_IO_WRITE (port/platform/include/port_hw.h).
#pragma once
#include_next "registers.h"
#include "port_hw.h"

#define reg_G3_VIEWPORT (*(vu32 *) DSA(0x04000580))

// Divider and square root: computed from the parameter registers when the result is read.
#undef reg_CP_DIV_RESULT
#undef reg_CP_DIVREM_RESULT
#undef reg_CP_DIV_RESULT_LO
#undef reg_CP_DIVREM_RESULT_LO
#undef reg_CP_SQRT_RESULT
#define reg_CP_DIV_RESULT (*(vu64 *) port_cp_div())
#define reg_CP_DIVREM_RESULT (*(vu64 *) (port_cp_div() + 8))
#define reg_CP_DIV_RESULT_LO (*(vu32 *) port_cp_div())
#define reg_CP_DIVREM_RESULT_LO (*(vu32 *) (port_cp_div() + 8))
#define reg_CP_SQRT_RESULT (*(vu32 *) port_cp_sqrt())
