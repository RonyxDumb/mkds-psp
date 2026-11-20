// Sound on the PSP: the DS sound driver (platform/sound.c) mixes into the SRC output channel at 44.1 kHz.
#include "port.h"

#include <pspaudio.h>
#include <pspkernel.h>
#include <string.h>

#include "psp_port.h"

void port_snd_render(int16_t *out, int frames, int rate);

#define AUDIO_RATE 22050 // half the work of 44.1 kHz for the mixer; the DS mixes at 32.7 kHz
#define AUDIO_FRAMES 512

static int16_t __attribute__((aligned(64))) s_buffers[2][AUDIO_FRAMES * 2];

static void audio_thread(void *arg) {
    (void) arg;
    int which = 0;
    for (;;) {
        int16_t *buf = s_buffers[which];
        if (g_paused) {
            memset(buf, 0, sizeof(s_buffers[0]));
        } else {
            port_snd_render(buf, AUDIO_FRAMES, AUDIO_RATE);
        }
        sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, buf);
        which ^= 1;
    }
}

void audio_init(void) {
    int r = sceAudioSRCChReserve(AUDIO_FRAMES, AUDIO_RATE, 2);
    if (r < 0) {
        port_log("audio: no output channel (%08x)", (unsigned) r);
        return;
    }
    psp_thread_start("mkds_audio", audio_thread, NULL, PSP_PRIO_AUDIO, 64 * 1024);
}
