// M8Tape Studio - sampling utility for the TrimUI Brick (NextUI / tg5040).
//
// M1: boot + auto-detect USB capture source from /proc/asound.
// M2: record the detected source via arecord (USB tuning, async card remount,
//     large buffers), with a REC screen.
// UI: TE x Nothing visual system in ui.c (custom fonts, tape reels, DSEG7
//     timecode). MinUI provides the screen surface + input.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>

#include <msettings.h>

#include "defines.h"
#include "api.h"
#include "utils.h"
#include "ui.h"

// --- detected input ---------------------------------------------------------
struct Input {
    int   present;
    char  id[64];
    char  name[128];
    int   channels;
    int   rate;
    char  format[32];
    int   card;
};

static int read_file(const char *path, char *buf, int cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int n = (int)fread(buf, 1, cap - 1, f);
    fclose(f);
    if (n < 0) n = 0;
    buf[n] = '\0';
    return n;
}

static const char *after_label(const char *s, const char *label) {
    const char *p = strstr(s, label);
    if (!p) return NULL;
    p += strlen(label);
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static int detect_input(struct Input *in) {
    memset(in, 0, sizeof(*in));
    char path[64], buf[8192];
    for (int card = 0; card < 16; card++) {
        snprintf(path, sizeof(path), "/proc/asound/card%d/stream0", card);
        if (read_file(path, buf, sizeof(buf)) <= 0) continue;
        const char *cap = strstr(buf, "Capture:");
        if (!cap) continue;

        const char *nl = strchr(buf, '\n');
        int nlen = nl ? (int)(nl - buf) : (int)strlen(buf);
        if (nlen > (int)sizeof(in->name) - 1) nlen = sizeof(in->name) - 1;
        memcpy(in->name, buf, nlen);
        in->name[nlen] = '\0';

        char idpath[64], idbuf[64];
        snprintf(idpath, sizeof(idpath), "/proc/asound/card%d/id", card);
        if (read_file(idpath, idbuf, sizeof(idbuf)) > 0) {
            idbuf[strcspn(idbuf, "\r\n")] = '\0';
            snprintf(in->id, sizeof(in->id), "%s", idbuf);
        }

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
                in->format[i] = fmt[i]; i++;
            }
            in->format[i] = '\0';
        }
        in->card = card;
        in->present = 1;
        return 1;
    }
    return 0;
}

static const char *classify(const struct Input *in) {
    if (!in->present) return "NO INPUT";
    if (in->channels >= 24) return "DIRTYWAVE M8";
    if (in->channels == 2)  return "STEREO INPUT";
    if (in->channels == 1)  return "MONO INPUT";
    return "USB AUDIO";
}

static void write_detect_log(const char *dir, const struct Input *in) {
    char path[256];
    snprintf(path, sizeof(path), "%s/detect.txt", dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "present=%d\ncard=%d\nid=%s\nname=%s\nchannels=%d\nrate=%d\nformat=%s\nclass=%s\n",
            in->present, in->card, in->id, in->name,
            in->channels, in->rate, in->format, classify(in));
    fclose(f);
}

// --- audio plumbing (M8Tape's proven path) ----------------------------------
static void write_sysfs(const char *path, const char *val) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(val, f);
    fclose(f);
}

static void sdcard_mount(char *out, int cap) {
    snprintf(out, cap, "/mnt/SDCARD");
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "SDCARD")) {
            char dev[256], mp[256];
            if (sscanf(line, "%255s %255s", dev, mp) == 2) {
                snprintf(out, cap, "%s", mp);
                break;
            }
        }
    }
    fclose(f);
}

static void apply_usb_tuning(void) {
    write_sysfs("/sys/module/snd_usb_audio/parameters/nrpacks", "1");
    write_sysfs("/sys/module/usbcore/parameters/autosuspend", "-1");
}

static void remount(const char *opt) {
    char mp[256], cmd[512];
    sdcard_mount(mp, sizeof(mp));
    if (strcmp(opt, "sync") == 0)
        snprintf(cmd, sizeof(cmd), "sync; mount -o remount,rw,sync '%s' 2>/dev/null", mp);
    else
        snprintf(cmd, sizeof(cmd), "mount -o remount,rw,async '%s' 2>/dev/null", mp);
    int rc = system(cmd); (void)rc;
}

// --- recording --------------------------------------------------------------
struct Rec { int active; pid_t pid; time_t start; char path[512]; };
static const char *g_out_dir = ".";

static int start_rec(struct Rec *r, const struct Input *in) {
    if (!in->present) return 0;
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/recordings", g_out_dir);
    mkdir(dir, 0777);

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", tm);
    snprintf(r->path, sizeof(r->path), "%s/rec_%s.wav", dir, ts);

    char dev[32], cstr[8], rstr[16];
    snprintf(dev, sizeof(dev), "hw:%d,0", in->card);
    snprintf(cstr, sizeof(cstr), "%d", in->channels > 0 ? in->channels : 2);
    snprintf(rstr, sizeof(rstr), "%d", in->rate > 0 ? in->rate : 48000);
    const char *fmt = in->format[0] ? in->format : "S24_3LE";

    apply_usb_tuning();
    remount("async");

    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        setpriority(PRIO_PROCESS, 0, -19);
        char logp[600];
        snprintf(logp, sizeof(logp), "%s/recordings/arecord.log", g_out_dir);
        freopen(logp, "w", stderr);
        freopen("/dev/null", "w", stdout);
        execlp("arecord", "arecord", "-D", dev, "-c", cstr, "-f", fmt, "-r", rstr,
               "--buffer-time=2000000", "--period-time=500000",
               "-t", "wav", r->path, (char *)NULL);
        _exit(127);
    }
    r->active = 1; r->pid = pid; r->start = time(NULL);
    return 1;
}

static void stop_rec(struct Rec *r) {
    if (!r->active) return;
    if (r->pid > 0) {
        kill(r->pid, SIGTERM);
        int status, i = 0;
        while (waitpid(r->pid, &status, WNOHANG) == 0 && i < 100) { usleep(50000); i++; }
        if (i >= 100) { kill(r->pid, SIGKILL); waitpid(r->pid, &status, 0); }
    }
    remount("sync");
    r->active = 0; r->pid = 0;
}

static const char *basename_of(const char *p) {
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}

// --- app --------------------------------------------------------------------
int main(int argc, char *argv[]) {
    g_out_dir = (argc > 1) ? argv[1] : ".";

    PWR_setCPUSpeed(CPU_SPEED_MENU);
    SDL_Surface *screen = GFX_init(MODE_MAIN);
    PAD_init();
    PWR_init();
    InitSettings();

    UI ui;
    char res_dir[600];
    snprintf(res_dir, sizeof(res_dir), "%s/res", g_out_dir);
    int fonts_ok = (ui_load_fonts(&ui, res_dir) == 0);

    struct Input in;
    detect_input(&in);
    write_detect_log(g_out_dir, &in);

    struct Rec rec = {0};
    int quitting = 0, redraw = 1, frames = 0, shot_done = 0;
    char last_take[512] = {0};

    while (!quitting) {
        GFX_startFrame();
        PWR_update(&redraw, NULL, NULL, NULL);
        PAD_poll();

        if (rec.active) {
            if (PAD_justReleased(BTN_A)) {
                stop_rec(&rec);
                snprintf(last_take, sizeof(last_take), "%s", basename_of(rec.path));
            }
            redraw = 1; // animate
        } else {
            if (PAD_justReleased(BTN_A) && in.present) start_rec(&rec, &in);
            if (PAD_justReleased(BTN_B) || PAD_justReleased(BTN_MENU)) quitting = 1;
            if (++frames >= 60) {
                frames = 0;
                struct Input now;
                detect_input(&now);
                if (now.present != in.present || now.channels != in.channels ||
                    now.card != in.card) {
                    in = now; write_detect_log(g_out_dir, &in); redraw = 1;
                }
            }
        }

        if (redraw && fonts_ok) {
            UIInput ui_in = {in.present, classify(&in), in.channels, in.rate,
                             in.format[0] ? in.format : NULL};
            if (rec.active) {
                UIRec ui_r = {
                    .elapsed = (long)(time(NULL) - rec.start),
                    .take = last_take[0] ? last_take : basename_of(rec.path),
                    .angle = (double)(SDL_GetTicks() % 1200) / 1200.0 * 2.0 * M_PI,
                    .blink = ((time(NULL) % 2) == 0),
                    .levelL = -1, .levelR = -1,
                };
                ui_draw_record(&ui, screen, &ui_in, &ui_r);
            } else {
                ui_draw_home(&ui, screen, &ui_in);
            }
            GFX_flip(screen);
            if (!shot_done) {  // one-shot screenshot for remote design checks
                char shot[640];
                snprintf(shot, sizeof(shot), "%s/screen.bmp", g_out_dir);
                SDL_SaveBMP(screen, shot);
                shot_done = 1;
            }
            if (!rec.active) redraw = 0;
        } else if (!redraw) {
            GFX_sync();
        }
    }

    if (rec.active) stop_rec(&rec);
    ui_free(&ui);
    QuitSettings();
    PWR_quit();
    PAD_quit();
    GFX_quit();
    return 0;
}
