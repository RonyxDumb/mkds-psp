// PSP entry point: finds the ROM next to the EBOOT, starts the game, draws each frame it shows.
#include "port.h"
#include "guest_if.h"

#include <pspkernel.h>
#include <psppower.h>
#include <pspdisplay.h>
#include <dirent.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "hostapi.h"
#include "psp_port.h"

PSP_MODULE_INFO("MarioKartDS", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_PRIORITY(PSP_PRIO_MAIN);
// The heap takes what is free except room for the threads' stacks.
PSP_HEAP_THRESHOLD_SIZE_KB(4096);


static char s_dir[256];

static int exit_callback(int arg1, int arg2, void *common) {
    (void) arg1, (void) arg2, (void) common;
    port_save_flush();
    sceKernelExitGame();
    return 0;
}

static int callback_thread(SceSize args, void *argp) {
    (void) args, (void) argp;
    int cbid = sceKernelCreateCallback("exit_callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

// The last log lines, for the fatal error screen.
void psp_show_fatal(void) {
    video_message("\nMario Kart DS has stopped. See mkds.log next to the EBOOT.\n");
}

// Drawing runs in its own thread, below the game's priority: it only gets the CPU while the game waits for its next
// VBlank, so the game always keeps its 60 frames per second and the picture takes the time that is left. At each
// VBlank the game thread only reads the controls and captures the frame's registers (frame.c).
static void *s_frame_sem;
static uint64_t s_stat_render, s_stat_drawn, s_stat_render_cpu;

// Pictures at a steady rhythm: one every s_cadence VBlanks (1: 60 per second, 2: 30, 3: 20, 4: 15), adapted to
// what the PSP manages: a picture that is not done when the next one is due counts as late; several late ones slow
// the rhythm down, a long run of pictures done with time to spare speeds it up.
static int s_cadence = 2;
static volatile bool s_render_busy;
static volatile uint64_t s_render_started_ns, s_render_took_ns;
static int s_late, s_on_time_quick, s_window;

static uint64_t thread_cpu_us(void) {
    SceKernelThreadRunStatus st;
    st.size = sizeof(st);
    if (sceKernelReferThreadRunStatus(0, &st) < 0) return 0;
    return ((uint64_t) st.runClocks.hi << 32) | st.runClocks.low;
}

static void render_thread(void *arg) {
    (void) arg;
    for (;;) {
        hapi_sem_wait(s_frame_sem);
        const FrameSnapshot *f = port_frame_acquire();
        if (!f) continue;
        s_render_busy = true;
        uint64_t t0 = hapi_now_ns(), c0 = thread_cpu_us();
        video_present(f);
        uint64_t took = hapi_now_ns() - t0;
        s_render_took_ns = took;
        s_stat_render += took;
        s_stat_render_cpu += thread_cpu_us() - c0;
        s_stat_drawn++;
        s_render_busy = false;
    }
}

static void pace(void) {
    static uint64_t s_vblank;
    s_vblank++;
    if (s_vblank % (uint64_t) s_cadence != 0) return;
    if (s_render_busy) {
        s_late++; // the last picture is not done: this one is skipped
    } else {
        if (s_render_took_ns && s_render_took_ns < (uint64_t) (s_cadence - 1) * 16683000 * 3 / 4) s_on_time_quick++;
        hapi_sem_post(s_frame_sem);
    }
    if (++s_window == 60) {
        if (s_late > 3 && s_cadence < 4) s_cadence++;
        else if (s_late == 0 && s_on_time_quick == 60 && s_cadence > 1) s_cadence--;
        s_late = s_on_time_quick = s_window = 0;
    }
}

static void on_frame(const FrameSnapshot *f) {
    static uint64_t s_stat_start, s_stat_frames;
    uint64_t t0 = hapi_now_ns();
    if (!s_stat_start) s_stat_start = t0;
    input_poll();
    s_stat_frames++;
#ifndef PSP_NODRAW // profiling: the game alone
    pace();
#endif
    if (s_stat_frames == 120) {
        uint64_t total = hapi_now_ns() - s_stat_start;
        extern uint64_t g_psp_idle_ns;
        extern uint64_t g_video_stats[4];
        extern unsigned int g_port_vblank_waits;
        static unsigned int s_last_waits;
        unsigned int game_frames = g_port_vblank_waits - s_last_waits;
        s_last_waits = g_port_vblank_waits;
        port_log("perf: %u game frames per second (60 = full speed), %u pictures (1 every %d VBlanks, %u ms CPU each); "
                 "%u VBlanks in %u ms; drawing %u ms (2D A %u B %u, 3D %u, GE wait %u), game idle %u ms, scene %d",
                 game_frames * 1000 / (unsigned) (total / 1000000 ? total / 1000000 : 1),
                 (unsigned) s_stat_drawn * 1000 / (unsigned) (total / 1000000 ? total / 1000000 : 1), s_cadence,
                 (unsigned) (s_stat_drawn ? s_stat_render_cpu / s_stat_drawn / 1000 : 0), (unsigned) s_stat_frames, (unsigned) (total / 1000000), (unsigned) (s_stat_render / 1000000),
                 (unsigned) (g_video_stats[0] / 1000), (unsigned) (g_video_stats[1] / 1000),
                 (unsigned) (g_video_stats[2] / 1000), (unsigned) (g_video_stats[3] / 1000),
                 (unsigned) (g_psp_idle_ns / 1000000), f->scene);
        memset(g_video_stats, 0, sizeof(g_video_stats));
        extern uint32_t g_ge_parts[3];
        port_log("  GE parts of one picture: 2D under %u us, 3D %u us, 2D above %u us", (unsigned) g_ge_parts[0],
                 (unsigned) g_ge_parts[1], (unsigned) g_ge_parts[2]);
        extern uint32_t g_ge_stats[8];
        port_log("  3D: %u cache flushes, %u textures decoded (%u Ktexels, %u ms), %u polygons, %u GE vertices, "
                 "%u batches (%u ms); CPU %d MHz, bus %d MHz",
                 (unsigned) g_ge_stats[0], (unsigned) g_ge_stats[1], (unsigned) g_ge_stats[2] / 1024,
                 (unsigned) g_ge_stats[3] / 1000, (unsigned) g_ge_stats[4], (unsigned) g_ge_stats[5],
                 (unsigned) g_ge_stats[6], (unsigned) g_ge_stats[7] / 1000, scePowerGetCpuClockFrequency(),
                 scePowerGetBusClockFrequency());
        memset(g_ge_stats, 0, sizeof(g_ge_stats));
        extern uint64_t g_frame_work_ns, g_frame_work_max_ns;
        extern uint32_t g_frame_work_count, g_frame_work_late;
        port_log("  game frames: %u, finished on average %u us after their VBlank (max %u us), %u late",
                 (unsigned) g_frame_work_count,
                 (unsigned) (g_frame_work_count ? g_frame_work_ns / g_frame_work_count / 1000 : 0),
                 (unsigned) (g_frame_work_max_ns / 1000), (unsigned) g_frame_work_late);
        g_frame_work_ns = g_frame_work_max_ns = 0;
        g_frame_work_count = g_frame_work_late = 0;
        extern uint32_t g_sys_stats[6];
        port_log("  kernel calls: clock %u, mutex %u, sem post %u, sem wait %u, thread id %u, sleep %u",
                 (unsigned) g_sys_stats[0], (unsigned) g_sys_stats[1], (unsigned) g_sys_stats[2],
                 (unsigned) g_sys_stats[3], (unsigned) g_sys_stats[4], (unsigned) g_sys_stats[5]);
        memset(g_sys_stats, 0, sizeof(g_sys_stats));
        static int s_mem_log;
        if (s_mem_log++ % 10 == 0) {
            struct mallinfo mi = mallinfo();
            port_log("memory: heap %u KiB, in use %u KiB, kernel free %u KiB", (unsigned) mi.arena / 1024,
                     (unsigned) mi.uordblks / 1024, (unsigned) sceKernelTotalFreeMemSize() / 1024);
        }
        s_stat_start = s_stat_render = s_stat_frames = s_stat_drawn = s_stat_render_cpu = 0;
        g_psp_idle_ns = 0;
    }
}

static bool is_rom(const char *path) {
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return false;
    char code[4] = {0};
    sceIoLseek32(fd, 0x0c, PSP_SEEK_SET);
    bool ok = sceIoRead(fd, code, 4) == 4 && memcmp(code, "AMCP", 4) == 0;
    sceIoClose(fd);
    return ok;
}

// mkds.nds next to the EBOOT, or any .nds file there that is the game.
static bool find_rom(char *out, size_t size) {
    snprintf(out, size, "%s/mkds.nds", s_dir);
    if (is_rom(out)) return true;
    DIR *d = opendir(s_dir);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while (!found && (e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n > 4 && strcasecmp(e->d_name + n - 4, ".nds") == 0) {
            snprintf(out, size, "%s/%s", s_dir, e->d_name);
            found = is_rom(out);
        }
    }
    closedir(d);
    return found;
}

int main(int argc, char **argv) {
    snprintf(s_dir, sizeof(s_dir), "%s", argc > 0 && argv[0] ? argv[0] : "ms0:/PSP/GAME/MKDS/EBOOT.PBP");
    char *slash = strrchr(s_dir, '/');
    if (slash) *slash = 0;

    SceUID cb = sceKernelCreateThread("mkds_callbacks", callback_thread, 0x11, 0x1000, PSP_THREAD_ATTR_USER, NULL);
    if (cb >= 0) sceKernelStartThread(cb, 0, NULL);

    char path[300];
    snprintf(path, sizeof(path), "%s/mkds.log", s_dir);
    psp_log_open(path);
    scePowerSetClockFrequency(333, 333, 166);
    port_log("Mario Kart DS for PSP, from %s", s_dir);
    port_log("memory: %u KiB free (largest block %u KiB)", (unsigned) sceKernelTotalFreeMemSize() / 1024,
             (unsigned) sceKernelMaxFreeMemSize() / 1024);

    video_init();
    input_init();

    char rom[300];
    if (!find_rom(rom, sizeof(rom))) {
        video_message("\n  Mario Kart DS for PSP\n\n"
                      "  No ROM found.\n\n"
                      "  Copy your dump of Mario Kart DS (Europe, AMCP) as mkds.nds\n"
                      "  into the folder of this EBOOT.PBP:\n\n  ");
        video_message(s_dir);
        video_message("\n\n  Press HOME to quit.\n");
        sceKernelSleepThread();
        return 0;
    }
    if (!port_mem_init() || !port_rom_open(rom)) {
        port_fatal("cannot start the game");
    }
    snprintf(path, sizeof(path), "%s/mkds.sav", s_dir);
    port_save_set_path(path);

    s_frame_sem = hapi_sem_new();
    psp_thread_start("mkds_render", render_thread, NULL, PSP_PRIO_RENDER, 256 * 1024);
    g_frame_hook = on_frame;
    g_gx3d_lite_enabled = true; // at most every other frame is drawn
    audio_init();
    guest_start_game();
    sceKernelSleepThread();
    return 0;
}
