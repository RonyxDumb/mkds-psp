// The host API (include/hostapi.h) on the PSP: kernel threads and semaphores, the system clock, the ROM read from the
// Memory Stick on demand (32 MiB do not fit in the PSP's memory) and the save file.
#include "port.h"
#include "hostapi.h"

#include <pspkernel.h>
#include <psprtc.h>
#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include "psp_port.h"

// ------------------------------------------------------------------------------------------------ log
static SceUID s_log_fd = -1;

void psp_log_open(const char *path) {
    s_log_fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
}

void hapi_log(const char *line) {
    // stdout reaches PSPLink and the emulators' logs; the file is for real hardware.
    printf("%s\n", line);
    if (s_log_fd >= 0) {
        sceIoWrite(s_log_fd, line, strlen(line));
        sceIoWrite(s_log_fd, "\n", 1);
    }
}

void hapi_abort(void) {
    psp_show_fatal();
    sceKernelExitGame();
    for (;;) sceKernelSleepThread();
}

// ------------------------------------------------------------------------------------------------ time
uint64_t hapi_now_ns(void) { return (uint64_t) sceKernelGetSystemTimeWide() * 1000ull; }

uint64_t g_psp_idle_ns; // time the game spent waiting (statistics)

void hapi_sleep_ns(uint64_t ns) {
    uint64_t us = ns / 1000;
    if (us) {
        uint64_t t0 = sceKernelGetSystemTimeWide();
        sceKernelDelayThread((SceUInt) (us > 0x7fffffff ? 0x7fffffff : us));
        g_psp_idle_ns += (sceKernelGetSystemTimeWide() - t0) * 1000;
    }
}

void hapi_local_time(HostTime *out) {
    ScePspDateTime t;
    memset(&t, 0, sizeof(t));
    sceRtcGetCurrentClockLocalTime(&t);
    out->year = t.year;
    out->month = t.month;
    out->day = t.day;
    out->weekday = sceRtcGetDayOfWeek(t.year, t.month, t.day);
    out->hour = t.hour;
    out->minute = t.minute;
    out->second = t.second;
}

// ------------------------------------------------------------------------------------------------ synchronisation
// Handles are kernel UIDs stored in the pointer.
void *hapi_mutex_new(void) { return (void *) (intptr_t) sceKernelCreateSema("mkds_mutex", 0, 1, 1, NULL); }
void hapi_mutex_lock(void *m) { sceKernelWaitSema((SceUID) (intptr_t) m, 1, NULL); }
void hapi_mutex_unlock(void *m) { sceKernelSignalSema((SceUID) (intptr_t) m, 1); }

void *hapi_sem_new(void) { return (void *) (intptr_t) sceKernelCreateSema("mkds_sem", 0, 0, 0x7fffffff, NULL); }
void hapi_sem_free(void *s) { sceKernelDeleteSema((SceUID) (intptr_t) s); }
void hapi_sem_post(void *s) { sceKernelSignalSema((SceUID) (intptr_t) s, 1); }
void hapi_sem_wait(void *s) { sceKernelWaitSema((SceUID) (intptr_t) s, 1, NULL); }

// ------------------------------------------------------------------------------------------------ threads
typedef struct Start {
    void (*fn)(void *);
    void *arg;
} Start;

static int thread_entry(SceSize args, void *argp) {
    (void) args;
    Start s = **(Start **) argp;
    free(*(Start **) argp);
    s.fn(s.arg);
    sceKernelExitDeleteThread(0);
    return 0;
}

// Game threads run one at a time (os.c hands the CPU over with semaphores), at one priority.
void psp_thread_start(const char *name, void (*fn)(void *), void *arg, int priority, int stack) {
    Start *s = malloc(sizeof(*s));
    s->fn = fn;
    s->arg = arg;
    SceUID id = sceKernelCreateThread(name, thread_entry, priority, stack, PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU,
                                      NULL);
    if (id < 0) {
        port_fatal("host: cannot create thread %s (%08x)", name, (unsigned) id);
    }
    sceKernelStartThread(id, sizeof(s), &s);
}

void host_start_guest_thread(void (*fn)(void *), void *arg) {
    psp_thread_start("mkds_game", fn, arg, PSP_PRIO_GAME, 512 * 1024);
}

void hapi_thread_start(void (*fn)(void *), void *arg) {
    psp_thread_start("mkds_os", fn, arg, PSP_PRIO_GAME, 256 * 1024);
}

void hapi_thread_exit(void) {
    sceKernelExitDeleteThread(0);
    for (;;) sceKernelSleepThread();
}

// One pointer per thread: a small table keyed by thread id.
#define TLS_MAX 96
static struct {
    SceUID thread;
    void *value;
} s_tls[TLS_MAX];

void *hapi_tls_get(void) {
    SceUID self = sceKernelGetThreadId();
    for (int i = 0; i < TLS_MAX; i++) {
        if (s_tls[i].thread == self) return s_tls[i].value;
    }
    return NULL;
}

void hapi_tls_set(void *value) {
    SceUID self = sceKernelGetThreadId();
    int free_slot = -1;
    for (int i = 0; i < TLS_MAX; i++) {
        if (s_tls[i].thread == self) {
            s_tls[i].value = value;
            return;
        }
        if (free_slot < 0 && s_tls[i].thread == 0) free_slot = i;
    }
    if (free_slot < 0) {
        // Threads that ended leave their entries: reuse the ones whose thread no longer exists.
        SceKernelThreadInfo info;
        for (int i = 0; i < TLS_MAX && free_slot < 0; i++) {
            info.size = sizeof(info);
            if (sceKernelReferThreadStatus(s_tls[i].thread, &info) < 0) free_slot = i;
        }
        if (free_slot < 0) port_fatal("host: too many threads");
    }
    s_tls[free_slot].thread = self;
    s_tls[free_slot].value = value;
}

// ------------------------------------------------------------------------------------------------ ROM
// Read through a small cache of 32 KiB blocks: the game reads its files in large pieces, but also headers and tables
// a few bytes at a time.
#define ROM_BLOCK 0x8000u
#define ROM_CACHE 16
static SceUID s_rom_fd = -1;
static uint32_t s_rom_size;
static void *s_rom_lock;
static struct {
    uint32_t block; // block number + 1, 0 = empty
    uint32_t used;
    uint8_t *data;
} s_rom_cache[ROM_CACHE];
static uint32_t s_rom_clock;

bool port_rom_open(const char *path) {
    s_rom_fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (s_rom_fd < 0) {
        port_log("rom: cannot open %s", path);
        return false;
    }
    s_rom_size = (uint32_t) sceIoLseek(s_rom_fd, 0, PSP_SEEK_END);
    char code[4];
    sceIoLseek(s_rom_fd, 0x0c, PSP_SEEK_SET);
    if (s_rom_size < 0x4000 || sceIoRead(s_rom_fd, code, 4) != 4 || memcmp(code, "AMCP", 4) != 0) {
        port_log("rom: %s is not Mario Kart DS (Europe, AMCP)", path);
        sceIoClose(s_rom_fd);
        s_rom_fd = -1;
        return false;
    }
    s_rom_lock = hapi_mutex_new();
    for (int i = 0; i < ROM_CACHE; i++) {
        s_rom_cache[i].data = memalign(64, ROM_BLOCK);
    }
    return true;
}

uint32_t hapi_rom_size(void) { return s_rom_size; }

static void rom_read_raw(uint32_t offset, void *dst, uint32_t size) {
    sceIoLseek32(s_rom_fd, (int) offset, PSP_SEEK_SET);
    uint8_t *p = dst;
    while (size) {
        int n = sceIoRead(s_rom_fd, p, size);
        if (n <= 0) {
            port_log("rom: read error at %08x (%08x)", (unsigned) offset, (unsigned) n);
            memset(p, 0, size);
            return;
        }
        p += n;
        size -= (uint32_t) n;
    }
}

static const uint8_t *rom_block(uint32_t block) {
    int victim = 0;
    for (int i = 0; i < ROM_CACHE; i++) {
        if (s_rom_cache[i].block == block + 1) {
            s_rom_cache[i].used = ++s_rom_clock;
            return s_rom_cache[i].data;
        }
        if (s_rom_cache[i].used < s_rom_cache[victim].used) victim = i;
    }
    uint32_t offset = block * ROM_BLOCK, size = ROM_BLOCK;
    if (offset + size > s_rom_size) size = s_rom_size - offset;
    rom_read_raw(offset, s_rom_cache[victim].data, size);
    s_rom_cache[victim].block = block + 1;
    s_rom_cache[victim].used = ++s_rom_clock;
    return s_rom_cache[victim].data;
}

void hapi_rom_read(uint32_t offset, void *dst, uint32_t size) {
    hapi_mutex_lock(s_rom_lock);
    uint8_t *out = dst;
    while (size) {
        uint32_t in_block = offset % ROM_BLOCK;
        // Whole blocks straight into the destination.
        if (in_block == 0 && size >= ROM_BLOCK) {
            uint32_t n = size - size % ROM_BLOCK;
            rom_read_raw(offset, out, n);
            offset += n, out += n, size -= n;
            continue;
        }
        uint32_t n = ROM_BLOCK - in_block;
        if (n > size) n = size;
        memcpy(out, rom_block(offset / ROM_BLOCK) + in_block, n);
        offset += n, out += n, size -= n;
    }
    hapi_mutex_unlock(s_rom_lock);
}

// ------------------------------------------------------------------------------------------------ save
static char s_save_path[256];

void port_save_set_path(const char *path) { snprintf(s_save_path, sizeof(s_save_path), "%s", path); }

uint32_t hapi_save_read(void *data, uint32_t size) {
    SceUID fd = sceIoOpen(s_save_path, PSP_O_RDONLY, 0);
    if (fd < 0) return 0;
    int n = sceIoRead(fd, data, size);
    sceIoClose(fd);
    port_log("save: loaded %d bytes from %s", n, s_save_path);
    return n > 0 ? (uint32_t) n : 0;
}

bool hapi_save_write(const void *data, uint32_t size) {
    if (!s_save_path[0]) return false;
    SceUID fd = sceIoOpen(s_save_path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        port_log("save: cannot write %s", s_save_path);
        return false;
    }
    int n = sceIoWrite(fd, data, size);
    sceIoClose(fd);
    return n == (int) size;
}

// ------------------------------------------------------------------------------------------------ network
// The DS wireless is not available on the PSP.
bool hapi_net_open(void) { return false; }
void hapi_net_close(void) {}
void hapi_net_mac(uint8_t *mac) {
    static const uint8_t m[6] = {0x00, 0x09, 0xbf, 0x12, 0x34, 0x56};
    memcpy(mac, m, 6);
}
void hapi_net_send(const HostNetAddr *to, const void *data, uint32_t size) { (void) to, (void) data, (void) size; }
int32_t hapi_net_recv(void *data, uint32_t capacity, HostNetAddr *from) {
    (void) data, (void) capacity, (void) from;
    return -1;
}
