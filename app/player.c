// player.c - ALSA WAV playback with a ~30 ms fade-out on SIGTERM (no stop click).
#include "player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <alsa/asoundlib.h>

static volatile sig_atomic_t g_stop = 0;
static void on_term(int sig) { (void)sig; g_stop = 1; }

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

// scale one interleaved frame (all channels) by gain g in [0,1], in place.
static void scale_frame(unsigned char *p, int ch, int bps, double g) {
    for (int c = 0; c < ch; c++) {
        unsigned char *s = p + c * bps;
        long v;
        if (bps == 2) { v = (int16_t)(s[0] | s[1] << 8); v = (long)(v * g); s[0] = v; s[1] = v >> 8; }
        else if (bps == 3) {
            v = s[0] | s[1] << 8 | s[2] << 16; if (v & 0x800000) v |= ~0xFFFFFFL;
            v = (long)(v * g); s[0] = v; s[1] = v >> 8; s[2] = v >> 16;
        } else if (bps == 4) {
            v = (int32_t)rd32(s); v = (long)(v * g); s[0] = v; s[1] = v >> 8; s[2] = v >> 16; s[3] = v >> 24;
        } else if (bps == 1) { v = (long)((s[0] - 128) * g); s[0] = (unsigned char)(v + 128); }
    }
}

int play_wav_fade(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) { fclose(f); return -1; }

    unsigned int rate = 0; int ch = 0, bits = 0; uint32_t data_size = 0; int have_fmt = 0, have_data = 0;
    for (;;) {
        unsigned char ck[8];
        if (fread(ck, 1, 8, f) != 8) break;
        uint32_t sz = rd32(ck + 4);
        if (!memcmp(ck, "fmt ", 4)) {
            unsigned char fb[40]; uint32_t n = sz > sizeof(fb) ? (uint32_t)sizeof(fb) : sz;
            if (fread(fb, 1, n, f) != n) { fclose(f); return -1; }
            ch = rd16(fb + 2); rate = rd32(fb + 4); bits = rd16(fb + 14); have_fmt = 1;
            if (sz > n) fseek(f, (long)(sz - n), SEEK_CUR);
            if (sz & 1) fseek(f, 1, SEEK_CUR);
        } else if (!memcmp(ck, "data", 4)) { data_size = sz; have_data = 1; break; }
        else fseek(f, (long)(sz + (sz & 1)), SEEK_CUR);
    }
    if (!have_fmt || !have_data || ch < 1 || bits < 8 || (bits & 7)) { fclose(f); return -1; }

    int bps = bits / 8, frame = ch * bps;
    snd_pcm_format_t fmt = bits == 16 ? SND_PCM_FORMAT_S16_LE : bits == 24 ? SND_PCM_FORMAT_S24_3LE
                          : bits == 32 ? SND_PCM_FORMAT_S32_LE : SND_PCM_FORMAT_U8;

    snd_pcm_t *pcm;
    if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) { fclose(f); return -1; }
    // 60 ms buffer: small enough that pressing stop feels immediate (little
    // full-volume audio stays queued ahead of the fade), big enough that light
    // file streaming won't underrun.
    if (snd_pcm_set_params(pcm, fmt, SND_PCM_ACCESS_RW_INTERLEAVED, ch, rate, 1, 60000) < 0) {
        snd_pcm_close(pcm); fclose(f); return -1;
    }

    struct sigaction sa = {0};
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    const int BLK = 1024;
    unsigned char *buf = malloc((size_t)frame * BLK);
    if (!buf) { snd_pcm_close(pcm); fclose(f); return -1; }

    uint64_t remaining = (data_size && data_size != 0xFFFFFFFFu) ? data_size : (uint64_t)-1;
    int fade_total = (int)(rate * 30 / 1000); if (fade_total < 1) fade_total = 1;
    int fade_left = fade_total, fading = 0, done = 0;

    while (!done) {
        size_t want = BLK;
        if (remaining != (uint64_t)-1) { uint64_t rf = remaining / frame; if (!rf) break; if (rf < want) want = (size_t)rf; }
        size_t got = fread(buf, frame, want, f);
        if (got == 0) break;

        if (g_stop && !fading) fading = 1;
        if (fading) {
            size_t i;
            for (i = 0; i < got; i++) {
                double g = (double)fade_left / fade_total;
                scale_frame(buf + i * frame, ch, bps, g);
                if (--fade_left <= 0) { i++; done = 1; break; }
            }
            got = i;
        }

        // write `got` frames (handle partial writes / recovery)
        unsigned char *p = buf; snd_pcm_uframes_t left = got;
        while (left > 0) {
            snd_pcm_sframes_t w = snd_pcm_writei(pcm, p, left);
            if (w < 0) {
                if (w == -EINTR) continue;
                if (w == -EPIPE) { snd_pcm_prepare(pcm); continue; }
                if (snd_pcm_recover(pcm, (int)w, 1) < 0) { done = 1; break; }
                continue;
            }
            p += (size_t)w * frame; left -= w;
        }
        if (remaining != (uint64_t)-1) remaining -= (uint64_t)got * frame;
    }

    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    free(buf);
    fclose(f);
    return 0;
}
