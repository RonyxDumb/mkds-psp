// The DS address space, relocated.
//
// The game reads and writes main RAM, the shared area, the I/O registers, palettes, VRAM and OAM by absolute
// address. On the PSP (no MMU) those addresses cannot be reserved, so every DS region lives in one block of memory,
// the "DS window", at a fixed address chosen by the build (PORT_DS_WINDOW). DSA(ds_address) gives the address of a DS
// location in the window. It is a constant expression: tools/portify.py rewrites the DS addresses of the game's
// sources into DSA(...), and the platform layer uses it for the registers it reads and writes.
//
// Inside one region the mapping is linear, so the game's address arithmetic (base + offset) keeps working.
// Addresses outside every region are left unchanged, so that a missed access faults instead of corrupting memory.
#pragma once

#ifndef PORT_DS_WINDOW
#error "PORT_DS_WINDOW must be defined by the build"
#endif

// Region table: DS start, DS end (exclusive), offset in the window.
#define DSR_MAIN_LO 0x02000000u // main RAM (4 MiB, as on the DS)
#define DSR_MAIN_HI 0x02400000u
#define DSR_MAIN_OFS 0x000000u
#define DSR_HIGH_LO 0x027e0000u // DTCM and the system shared area at the top of the 8 MiB debug range
#define DSR_HIGH_HI 0x02800000u
#define DSR_HIGH_OFS 0x400000u
#define DSR_ITCM_LO 0x01ff8000u
#define DSR_ITCM_HI 0x02000000u
#define DSR_ITCM_OFS 0x420000u
#define DSR_IO_LO 0x04000000u
#define DSR_IO_HI 0x04010000u
#define DSR_IO_OFS 0x428000u
#define DSR_IO2_LO 0x04100000u // IPC receive FIFO, card data
#define DSR_IO2_HI 0x04104000u
#define DSR_IO2_OFS 0x438000u
#define DSR_PLTT_LO 0x05000000u
#define DSR_PLTT_HI 0x05000800u
#define DSR_PLTT_OFS 0x43c000u
#define DSR_OAM_LO 0x07000000u
#define DSR_OAM_HI 0x07000800u
#define DSR_OAM_OFS 0x43d000u
#define DSR_GBA_LO 0x08000000u // GBA slot: reads as empty
#define DSR_GBA_HI 0x08010000u
#define DSR_GBA_OFS 0x43e000u
#define DSR_BGA_LO 0x06000000u // VRAM: CPU views of the banks (see memory.c)
#define DSR_BGA_HI 0x06080000u
#define DSR_BGA_OFS 0x450000u
#define DSR_BGB_LO 0x06200000u
#define DSR_BGB_HI 0x06220000u
#define DSR_BGB_OFS 0x4d0000u
#define DSR_OBJA_LO 0x06400000u
#define DSR_OBJA_HI 0x06440000u
#define DSR_OBJA_OFS 0x4f0000u
#define DSR_OBJB_LO 0x06600000u
#define DSR_OBJB_HI 0x06620000u
#define DSR_OBJB_OFS 0x530000u
// LCDC: from 0x067e0000, because the SDK computes a clear-image address as 0x067e0000 + 0x20000.
#define DSR_LCDC_LO 0x067e0000u
#define DSR_LCDC_HI 0x068a4000u
#define DSR_LCDC_OFS 0x550000u
#define DS_WINDOW_SIZE 0x614000u

#define DSA_R_(a, R) ((unsigned) (a) - DSR_##R##_LO < DSR_##R##_HI - DSR_##R##_LO) \
    ? (unsigned) (a) - DSR_##R##_LO + DSR_##R##_OFS + (unsigned) PORT_DS_WINDOW :
#define DSA(a)                                                                                                         \
    ((unsigned) (DSA_R_(a, MAIN) DSA_R_(a, HIGH) DSA_R_(a, IO) DSA_R_(a, ITCM) DSA_R_(a, IO2) DSA_R_(a, PLTT)          \
                 DSA_R_(a, OAM) DSA_R_(a, GBA) DSA_R_(a, BGA) DSA_R_(a, BGB) DSA_R_(a, OBJA) DSA_R_(a, OBJB)          \
                 DSA_R_(a, LCDC)(unsigned) (a)))
#define DSP(a) ((void *) (unsigned long) DSA(a))
