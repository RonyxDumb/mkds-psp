#include "irq_wait_handler.h"
#include "interrupt_control.h"
#include "thread_scheduler.h"
#include "reset_interrupts_halt.h"
#include "bios_calls.h"

// The start of the data TCM up to the interrupt check word.
typedef struct IrqCheckView {
    u8 mPad[0x3ff8];
    u32 mCheckFlags;
} IrqCheckView;

// Optionally clears the bits irqFlags of the IRQ check flag, then sleeps the thread until one of them is set.
void OS_WaitIrq(u32 clear, u32 irqFlags) {
    u32 enabled;
    IrqCheckView *dtcm;
    u32 *pendingFlags;

#ifdef PLATFORM_PORT
    extern unsigned int g_port_vblank_waits; // PORT: statistics (frames of the game's main loop)
    if (irqFlags & 1) {
        g_port_vblank_waits++;
        extern void port_os_frame_done(void);
        port_os_frame_done();
    }
#endif
    enabled = OS_DisableInterrupts();
    dtcm = (IrqCheckView *) HW_DTCM;
    if (clear != 0) {
        dtcm->mCheckFlags &= ~irqFlags;
    }
    OS_RestoreInterrupts(enabled);
    dtcm = (IrqCheckView *) HW_DTCM;
    if ((irqFlags & dtcm->mCheckFlags) != 0) {
        return;
    }
    pendingFlags = &dtcm->mCheckFlags;
    do {
        OS_SleepThread(&OSi_IrqThreadQueue);
    } while ((irqFlags & *pendingFlags) == 0);
}
