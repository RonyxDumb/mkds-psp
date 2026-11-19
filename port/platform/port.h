// Internal interface of the platform layer (not seen by the game code).
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------- logging
void port_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void port_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

// ---------------------------------------------------------------- memory map (memory.c)
// The DS address space the game uses directly, reserved at the same addresses in this (32-bit) process.
// The DS regions live in the "DS window" (include/ds_addr.h): DSA(ds_address) is where a DS address is.
#include "ds_addr.h"
#define DS_ITCM_BASE DSA(0x01ff8000u)
#define DS_ITCM_SIZE 0x8000u
#define DS_DTCM_BASE DSA(0x027e0000u)
#define DS_IO_BASE DSA(0x04000000u)
#define DS_IO_SIZE 0x00010000u
#define DS_PLTT_BASE DSA(0x05000000u)
#define DS_OAM_BASE DSA(0x07000000u)

// Main arena handed to the game (OS_GetArenaLo/Hi(OS_ARENA_MAIN)): main RAM, as on the DS (the game's code and
// overlays are not in it here).
#define PORT_ARENA_MAIN_LO DSA(0x02004000u)
#define PORT_ARENA_MAIN_HI (DSA(0x023fffffu) + 1)

// The DS window: allocated by the host (hapi_ds_window), cleared, VRAM banks set up.
bool port_mem_init(void);

#define IO8(off) (*(volatile uint8_t *) (uintptr_t) (DS_IO_BASE + (off)))
#define IO16(off) (*(volatile uint16_t *) (uintptr_t) (DS_IO_BASE + (off)))
#define IO32(off) (*(volatile uint32_t *) (uintptr_t) (DS_IO_BASE + (off)))

// VRAM banks A..I. Without an MMU a bank cannot be seen at two addresses: its contents live where the CPU sees it
// (its view in the DS window) while it is mapped, and in its own storage otherwise; remapping moves them.
// port_vram_bank(bank) is where they are now.
enum { VRAM_A, VRAM_B, VRAM_C, VRAM_D, VRAM_E, VRAM_F, VRAM_G, VRAM_H, VRAM_I, VRAM_BANK_COUNT };
extern uint8_t *g_vram_bank_ptr[VRAM_BANK_COUNT];
#define port_vram_bank(bank) (g_vram_bank_ptr[bank])
extern const uint32_t g_vram_bank_offset[VRAM_BANK_COUNT];
extern const uint32_t g_vram_bank_size[VRAM_BANK_COUNT];
#define VRAM_TOTAL_SIZE 0xa4000u
// Re-applies the CPU mapping of bank `bank` after its VRAMCNT register changed.
void port_vram_remap(int bank);
// Copies the banks, in bank order (g_vram_bank_offset), into dst (VRAM_TOTAL_SIZE bytes).
void port_vram_gather(uint8_t *dst);

// ---------------------------------------------------------------- ROM (card.c)
bool port_rom_open(const char *path);
// Save data: loaded from / flushed to this file.
void port_save_set_path(const char *path);
void port_save_flush(void);

// ---------------------------------------------------------------- time (os.c)
uint64_t port_now_ns(void);

// ---------------------------------------------------------------- OS (os.c)
// Starts the game: main() on the launcher thread. Returns when the game terminates (or never).
void port_os_run(void (*entry)(void));
// Called by the scheduler when every game thread is waiting: wait for the next event (vblank, alarm), run it.
// Runs in "interrupt context": handlers may wake threads but never switch.
void port_os_raise_irq(uint32_t mask);
// Deferred completion callbacks (card, DMA): run at the next interrupt point, outside the caller's stack.
void port_os_defer(void (*fn)(void *), void *arg);
bool port_os_in_irq(void);
// Frame pacing: 0 = as fast as possible (tests), otherwise target VBlank rate in mHz (59826 for the DS).
extern uint32_t g_vblank_rate_mhz;
extern volatile bool g_quit;
extern volatile bool g_paused;

// ---------------------------------------------------------------- frame / vblank (frame.c)
// Called at each emulated VBlank (scanline 192), before the game's VBlank interrupt handler.
void port_frame_vblank(void);
extern uint64_t g_frame_count;

// ---------------------------------------------------------------- input (frame.c)
typedef struct PortInput {
    uint16_t buttons;  // DS KEYINPUT bits, 1 = pressed: A B Sel St R L U D R L (0..9), X (10), Y (11)
    bool touching;
    uint16_t touch_x, touch_y; // 0..255, 0..191
    bool lid_closed;
} PortInput;
void port_input_set(const PortInput *in);
void port_input_get(PortInput *out);
#define PAD_A 0x001
#define PAD_B 0x002
#define PAD_SELECT 0x004
#define PAD_START 0x008
#define PAD_RIGHT 0x010
#define PAD_LEFT 0x020
#define PAD_UP 0x040
#define PAD_DOWN 0x080
#define PAD_R 0x100
#define PAD_L 0x200
#define PAD_X 0x400
#define PAD_Y 0x800

// ---------------------------------------------------------------- 3D geometry engine (gx3d.c)
void gx3d_init(void);
void gx3d_command(uint32_t cmd, uint32_t param); // one command parameter word (cmd = command id)
void gx3d_fifo_write(uint32_t word);              // GXFIFO port: packed commands
void gx3d_port_write(uint32_t addr, uint32_t value); // 0x04000440..0x040005ff command ports
void gx3d_vblank(void);                           // apply a pending SWAP_BUFFERS

// ---------------------------------------------------------------- DMA (mi.c)
void port_dma_start(uint32_t channel, uint32_t src, uint32_t dst, uint32_t control);

#ifdef __cplusplus
}
#endif
