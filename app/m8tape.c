// M8Tape - sampling utility for the TrimUI Brick (NextUI / tg5040).
// Milestone 1: boots with a Nothing-OS-style screen and auto-detects the
// connected USB audio capture source (channel count / rate / format) by
// reading /proc/asound. No audio is captured yet — this proves the toolchain,
// the rendering, and the detection on real hardware.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <msettings.h>
#include <SDL2/SDL_ttf.h>

#include "defines.h"
#include "api.h"
#include "utils.h"

// --- Nothing OS palette (brand-faithful working values) ---------------------
static const SDL_Color NT_WHITE = {255, 255, 255, 255};
static const SDL_Color NT_GREY  = {130, 130, 130, 255};
static const SDL_Color NT_DIM   = {58, 58, 58, 255};
static const SDL_Color NT_RED   = {215, 25, 33, 255}; // #D71921, single accent

// --- detected input ---------------------------------------------------------
struct Input {
    int   present;        // 1 if a USB capture device was found
    char  id[64];         // ALSA card id (e.g. "M8")
    char  name[128];      // human name (first line of stream0)
    int   channels;       // max capture channels
    int   rate;           // a supported rate (Hz)
    char  format[32];     // e.g. "S24_3LE"
    int   card;           // ALSA card number
};

// read_file slurps up to cap-1 bytes of path into buf (NUL-terminated).
static int read_file(const char *path, char *buf, int cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int n = (int)fread(buf, 1, cap - 1, f);
    fclose(f);
    if (n < 0) n = 0;
    buf[n] = '\0';
    return n;
}

// after_label returns a pointer just past the first occurrence of label in s,
// or NULL. Used to read "Channels: 24", "Rates: 44100", "Format: S24_3LE".
static const char *after_label(const char *s, const char *label) {
    const char *p = strstr(s, label);
    if (!p) return NULL;
    p += strlen(label);
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

// detect_input scans /proc/asound/cardN/stream0 for a capture-capable device
// (these files are created by snd-usb-audio, so they identify USB audio cards).
// It parses the Capture section for channels/rate/format. Returns 1 on success.
static int detect_input(struct Input *in) {
    memset(in, 0, sizeof(*in));
    char path[64];
    char buf[8192];

    for (int card = 0; card < 16; card++) {
        snprintf(path, sizeof(path), "/proc/asound/card%d/stream0", card);
        if (read_file(path, buf, sizeof(buf)) <= 0) continue;

        const char *cap = strstr(buf, "Capture:");
        if (!cap) continue; // playback-only device, skip

        // name = first non-empty line of stream0
        const char *nl = strchr(buf, '\n');
        int nlen = nl ? (int)(nl - buf) : (int)strlen(buf);
        if (nlen > (int)sizeof(in->name) - 1) nlen = sizeof(in->name) - 1;
        memcpy(in->name, buf, nlen);
        in->name[nlen] = '\0';

        // id from /proc/asound/cardN/id
        char idpath[64], idbuf[64];
        snprintf(idpath, sizeof(idpath), "/proc/asound/card%d/id", card);
        if (read_file(idpath, idbuf, sizeof(idbuf)) > 0) {
            idbuf[strcspn(idbuf, "\r\n")] = '\0';
            snprintf(in->id, sizeof(in->id), "%s", idbuf);
        }

        // parse the largest Channels value within the Capture section
        const char *p = cap;
        int max_ch = 0;
        while ((p = after_label(p, "Channels:")) != NULL) {
            int c = atoi(p);
            if (c > max_ch) max_ch = c;
        }
        in->channels = max_ch;

        const char *r = after_label(cap, "Rates:");
        if (r) in->rate = atoi(r);

        const char *fmt = after_label(cap, "Format:");
        if (fmt) {
            int i = 0;
            while (fmt[i] && fmt[i] != '\r' && fmt[i] != '\n' &&
                   fmt[i] != ' ' && i < (int)sizeof(in->format) - 1) {
                in->format[i] = fmt[i];
                i++;
            }
            in->format[i] = '\0';
        }

        in->card = card;
        in->present = 1;
        return 1;
    }
    return 0;
}

// classify maps a channel count to a friendly source label.
static const char *classify(const struct Input *in) {
    if (!in->present) return "NO INPUT";
    if (in->channels >= 24) return "DIRTYWAVE M8";
    if (in->channels == 2)  return "STEREO INPUT";
    if (in->channels == 1)  return "MONO INPUT";
    return "USB AUDIO";
}

// --- drawing helpers --------------------------------------------------------
static void fill(SDL_Surface *s, int x, int y, int w, int h, SDL_Color c) {
    SDL_Rect r = {x, y, w, h};
    SDL_FillRect(s, &r, SDL_MapRGB(s->format, c.r, c.g, c.b));
}

// text draws a single line at (x,y); returns its pixel width.
static int text(SDL_Surface *s, TTF_Font *f, const char *str, SDL_Color c, int x, int y) {
    if (!str || !*str) return 0;
    SDL_Surface *t = TTF_RenderUTF8_Blended(f, str, c);
    if (!t) return 0;
    SDL_Rect r = {x, y, t->w, t->h};
    SDL_BlitSurface(t, NULL, s, &r);
    int w = t->w;
    SDL_FreeSurface(t);
    return w;
}

// text_r draws a single line right-aligned to right_x.
static void text_r(SDL_Surface *s, TTF_Font *f, const char *str, SDL_Color c, int right_x, int y) {
    if (!str || !*str) return;
    SDL_Surface *t = TTF_RenderUTF8_Blended(f, str, c);
    if (!t) return;
    SDL_Rect r = {right_x - t->w, y, t->w, t->h};
    SDL_BlitSurface(t, NULL, s, &r);
    SDL_FreeSurface(t);
}

struct AppState { int redraw; int quitting; int exit_code; };

static void handle_input(struct AppState *st) {
    PAD_poll();
    if (PAD_justReleased(BTN_A) || PAD_justReleased(BTN_B) || PAD_justReleased(BTN_MENU)) {
        st->quitting = 1;
        st->redraw = 0;
    }
}

static void draw_screen(SDL_Surface *screen, const struct Input *in) {
    const int W = screen->w, H = screen->h;
    const int M = W / 16; // margin, scales with screen

    // background: pure black
    fill(screen, 0, 0, W, H, (SDL_Color){0, 0, 0, 255});

    // header
    text(screen, font.large, "M8TAPE", NT_WHITE, M, M);
    text(screen, font.small, "SAMPLER", NT_GREY, M, M + 64);

    // divider
    fill(screen, M, M + 104, W - 2 * M, 2, NT_DIM);

    // INPUT block
    int y = H / 3;
    text(screen, font.small, "INPUT", NT_GREY, M, y);

    // source name (big) + red accent dot when a device is present
    int nx = text(screen, font.large, classify(in), NT_WHITE, M, y + 36);
    if (in->present) {
        int dot = 18;
        fill(screen, M + nx + 22, y + 36 + 20, dot, dot, NT_RED);
    }

    // technical line
    char line[160];
    if (in->present) {
        snprintf(line, sizeof(line), "%d CH   %d HZ   %s",
                 in->channels, in->rate, in->format[0] ? in->format : "?");
        text(screen, font.medium, line, NT_GREY, M, y + 116);

        snprintf(line, sizeof(line), "ALSA: %s  (hw:%d,0)",
                 in->id[0] ? in->id : "?", in->card);
        text(screen, font.small, line, NT_DIM, M, y + 160);
    } else {
        text(screen, font.medium, "CONNECT A USB AUDIO DEVICE", NT_GREY, M, y + 116);
    }

    // footer hint
    text_r(screen, font.small, "B / MENU  —  QUIT", NT_GREY, W - M, H - M);
}

int main(int argc, char *argv[]) {
    PWR_setCPUSpeed(CPU_SPEED_MENU);

    SDL_Surface *screen = GFX_init(MODE_MAIN);
    PAD_init();
    PWR_init();
    InitSettings();

    struct Input in;
    detect_input(&in);

    struct AppState st = {.redraw = 1, .quitting = 0, .exit_code = EXIT_SUCCESS};
    int frames = 0;

    while (!st.quitting) {
        GFX_startFrame();
        PWR_update(&st.redraw, NULL, NULL, NULL);
        handle_input(&st);

        // re-scan for a device about once per second so hot-plug is reflected
        if (++frames >= 60) {
            frames = 0;
            struct Input now;
            detect_input(&now);
            if (now.present != in.present || now.channels != in.channels ||
                now.card != in.card) {
                in = now;
                st.redraw = 1;
            }
        }

        if (st.redraw) {
            GFX_clear(screen);
            draw_screen(screen, &in);
            GFX_flip(screen);
            st.redraw = 0;
        } else {
            GFX_sync();
        }
    }

    QuitSettings();
    PWR_quit();
    PAD_quit();
    GFX_quit();
    return st.exit_code;
}
