// The DS address space in the DS window (include/ds_addr.h).
//
// Every region is ordinary memory. VRAM is the only one with a mapping that changes: banks A..I are mapped into the
// CPU's view (0x06000000..0x068a3fff) as the VRAMCNT registers say. The console's memory controller shows the same
// bank memory wherever it is mapped; without an MMU a bank's contents live at one place at a time: in the CPU view
// where the bank is mapped, or in the bank's own storage when it is not CPU-visible (texture slots, extended
// palettes, off). Remapping a bank moves its contents. Everything that reads banks (renderers, capture) goes through
// g_vram_bank_ptr.
#include "port.h"
#include "hostapi.h"

#include <string.h>

uint8_t *g_vram_bank_ptr[VRAM_BANK_COUNT];
const uint32_t g_vram_bank_offset[VRAM_BANK_COUNT] = {0x00000, 0x20000, 0x40000, 0x60000, 0x80000,
                                                      0x90000, 0x94000, 0x98000, 0xa0000};
const uint32_t g_vram_bank_size[VRAM_BANK_COUNT] = {0x20000, 0x20000, 0x20000, 0x20000, 0x10000,
                                                    0x4000,  0x4000,  0x8000,  0x4000};
// LCDC addresses of the banks (VRAMCNT MST 0).
static const uint32_t s_lcdc_addr[VRAM_BANK_COUNT] = {0x06800000, 0x06820000, 0x06840000, 0x06860000, 0x06880000,
                                                      0x06890000, 0x06894000, 0x06898000, 0x068a0000};
static uint8_t s_bank_store[VRAM_TOTAL_SIZE] __attribute__((aligned(64)));
static uint32_t s_bank_view[VRAM_BANK_COUNT]; // DS address of the CPU view the bank is in, 0 = in its storage

bool port_mem_init(void) {
    uint8_t *window = hapi_ds_window();
    if (!window || (uintptr_t) window != (uintptr_t) PORT_DS_WINDOW) {
        port_log("memory: the DS window is not at %08x", (unsigned) PORT_DS_WINDOW);
        return false;
    }
    memset(window, 0, DS_WINDOW_SIZE);
    memset(window + DSR_GBA_OFS, 0xff, DSR_GBA_HI - DSR_GBA_LO); // empty GBA slot (open bus)
    memset(s_bank_store, 0, sizeof(s_bank_store));
    for (int bank = 0; bank < VRAM_BANK_COUNT; bank++) {
        g_vram_bank_ptr[bank] = s_bank_store + g_vram_bank_offset[bank];
        s_bank_view[bank] = 0;
    }
    // Power-on state of the registers the game reads before writing.
    IO16(0x130) = 0x03ff; // KEYINPUT: nothing pressed
    for (int bank = 0; bank < VRAM_BANK_COUNT; bank++) {
        port_vram_remap(bank);
    }
    return true;
}

// Where the bank is visible to the CPU for a VRAMCNT value: base address, or 0 when it is not CPU-visible
// (texture image / palette slots, extended palettes, disabled).
static uint32_t cpu_address(int bank, uint8_t cnt) {
    if (!(cnt & 0x80)) {
        return 0;
    }
    uint32_t mst = cnt & 7, ofs = (cnt >> 3) & 3;
    switch (bank) {
    case VRAM_A:
    case VRAM_B:
    case VRAM_C:
    case VRAM_D:
        if (bank >= VRAM_C && mst == 4) {
            return bank == VRAM_C ? 0x06200000 : 0x06600000; // C: BG engine B, D: OBJ engine B
        }
        switch (mst & 3) { // A/B: MST is 2 bits
        case 0:
            return s_lcdc_addr[bank];
        case 1:
            return 0x06000000 + ofs * 0x20000;
        case 2:
            // A/B: OBJ engine A; C/D: ARM7 work RAM, not visible here
            return bank <= VRAM_B ? 0x06400000 + (ofs & 1) * 0x20000 : 0;
        default:
            return 0; // texture image slot
        }
    case VRAM_E:
        switch (mst) {
        case 0: return s_lcdc_addr[bank];
        case 1: return 0x06000000;
        case 2: return 0x06400000;
        default: return 0; // texture palette, BG / OBJ extended palette
        }
    case VRAM_F:
    case VRAM_G:
        switch (mst) {
        case 0: return s_lcdc_addr[bank];
        case 1: return 0x06000000 + (ofs & 1) * 0x4000 + (ofs >> 1) * 0x10000;
        case 2: return 0x06400000 + (ofs & 1) * 0x4000 + (ofs >> 1) * 0x10000;
        default: return 0;
        }
    case VRAM_H:
        switch (mst & 3) {
        case 0: return s_lcdc_addr[bank];
        case 1: return 0x06200000;
        default: return 0; // BG extended palette of engine B
        }
    case VRAM_I:
        switch (mst & 3) {
        case 0: return s_lcdc_addr[bank];
        case 1: return 0x06208000;
        case 2: return 0x06600000;
        default: return 0; // OBJ extended palette of engine B
        }
    }
    return 0;
}

// The CPU view at a DS VRAM address, or NULL when the address is outside the views of the window.
static uint8_t *view_ptr(uint32_t ds, uint32_t size) {
    uint8_t *p = (uint8_t *) DSP(ds);
    if ((uintptr_t) p == ds || DSA(ds + size - 1) != DSA(ds) + size - 1) {
        return NULL; // not in one region
    }
    return p;
}

static void move_bank(int bank, uint8_t *to) {
    uint8_t *from = g_vram_bank_ptr[bank];
    if (from != to) {
        memmove(to, from, g_vram_bank_size[bank]);
        g_vram_bank_ptr[bank] = to;
    }
}

static bool overlaps(uint32_t a, uint32_t a_size, uint32_t b, uint32_t b_size) { return a < b + b_size && b < a + a_size; }

void port_vram_remap(int bank) {
    static const uint32_t cnt_offset[VRAM_BANK_COUNT] = {0x240, 0x241, 0x242, 0x243, 0x244, 0x245, 0x246, 0x248, 0x249};
    uint32_t base = cpu_address(bank, IO8(cnt_offset[bank]));
    uint32_t size = g_vram_bank_size[bank];
    uint8_t *view = base ? view_ptr(base, size) : NULL;
    if (!view) {
        base = 0;
    }
    if (base == s_bank_view[bank]) {
        return;
    }
    // Another bank in the same place (the console would show both, ORed): it goes back to its storage.
    if (base) {
        for (int other = 0; other < VRAM_BANK_COUNT; other++) {
            if (other != bank && s_bank_view[other] &&
                overlaps(base, size, s_bank_view[other], g_vram_bank_size[other])) {
                move_bank(other, s_bank_store + g_vram_bank_offset[other]);
                s_bank_view[other] = 0;
            }
        }
    }
    move_bank(bank, base ? view : s_bank_store + g_vram_bank_offset[bank]);
    s_bank_view[bank] = base;
}

void port_vram_gather(uint8_t *dst) {
    for (int bank = 0; bank < VRAM_BANK_COUNT; bank++) {
        memcpy(dst + g_vram_bank_offset[bank], g_vram_bank_ptr[bank], g_vram_bank_size[bank]);
    }
}
