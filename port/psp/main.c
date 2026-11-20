// PSP entry point: finds the ROM next to the EBOOT, starts the game, draws each frame it shows.
#include "port.h"
#include "guest_if.h"

#include <pspkernel.h>
#include <psppower.h>
#include <pspdisplay.h>
#include <dirent.h>
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

// Each frame the game shows: input for the next one, and the picture when there is time for it. The game keeps its
// 60 frames per second; drawing takes what is left (frame skip: after a frame that took D to draw, the next
// ceil(D / 16.7 ms) - 1 frames are not drawn, at most MAX_SKIP).
#define MAX_SKIP 5
static void on_frame(const FrameSnapshot *f) {
    static int to_skip;
    static uint64_t s_stat_start, s_stat_render, s_stat_frames, s_stat_drawn;
    uint64_t t0 = hapi_now_ns();
    if (!s_stat_start) s_stat_start = t0;
    input_poll();
    s_stat_frames++;
#ifdef PSP_NODRAW // profiling: the game alone
    to_skip = 1;
#endif
    if (to_skip > 0) {
        to_skip--;
    } else {
        video_present(f);
        s_stat_drawn++;
        uint64_t d = hapi_now_ns() - t0;
        to_skip = (int) (d / 16683000);
        if (to_skip > MAX_SKIP) to_skip = MAX_SKIP;
    }
    s_stat_render += hapi_now_ns() - t0;
    if (s_stat_frames == 120) {
        uint64_t total = hapi_now_ns() - s_stat_start;
        extern uint64_t g_psp_idle_ns;
        extern uint64_t g_video_stats[4];
        port_log("perf: %u frames in %u ms (%u drawn), drawing %u ms (2D A %u B %u, 3D %u, GE wait %u), idle %u ms, "
                 "scene %d",
                 (unsigned) s_stat_frames, (unsigned) (total / 1000000), (unsigned) s_stat_drawn,
                 (unsigned) (s_stat_render / 1000000), (unsigned) (g_video_stats[0] / 1000),
                 (unsigned) (g_video_stats[1] / 1000), (unsigned) (g_video_stats[2] / 1000),
                 (unsigned) (g_video_stats[3] / 1000), (unsigned) (g_psp_idle_ns / 1000000), f->scene);
        memset(g_video_stats, 0, sizeof(g_video_stats));
        s_stat_start = s_stat_render = s_stat_frames = s_stat_drawn = 0;
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

    g_frame_hook = on_frame;
    audio_init();
    guest_start_game();
    sceKernelSleepThread();
    return 0;
}
