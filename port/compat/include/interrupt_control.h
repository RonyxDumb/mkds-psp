// PORT: the interrupt check word lives at a fixed address in the reserved DTCM area instead of relative to
// OSi_IrqThreadQueue (which the DS links at the start of DTCM; in the port it is an ordinary global).
#pragma once
#include_next "interrupt_control.h"
#undef IRQ_CHECK_FLAGS
#undef DTCM_THREAD_STACK_END
#define IRQ_CHECK_FLAGS (*(vu32 *) (HW_DTCM + 0x3ff8))
#define DTCM_THREAD_STACK_END ((u8 *) (HW_DTCM + 0x3f80))
