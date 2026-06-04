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
#include <ctype.h>
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
#include "wav.h"

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

// --- library ----------------------------------------------------------------
static const char *g_out_dir = ".";       // pak dir (logs, screenshot)
static char g_lib[512];                    // <SDCARD>/M8Tape
static char g_tmp[600];                    // raw take staged here before naming

static void setup_library(void) {
    char mp[256];
    sdcard_mount(mp, sizeof(mp));
    snprintf(g_lib, sizeof(g_lib), "%s/M8Tape", mp);
    mkdir(g_lib, 0777);
    char uns[600];
    snprintf(uns, sizeof(uns), "%s/Unsorted", g_lib);
    mkdir(uns, 0777);
    snprintf(g_tmp, sizeof(g_tmp), "%s/.tmprec.wav", g_lib);
}

// --- recording --------------------------------------------------------------
struct Rec { int active; pid_t pid; time_t start; char path[512]; };

static int start_rec(struct Rec *r, const struct Input *in) {
    if (!in->present) return 0;
    snprintf(r->path, sizeof(r->path), "%s", g_tmp);  // stage raw take

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
        snprintf(logp, sizeof(logp), "%s/arecord.log", g_out_dir);
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
    r->active = 0; r->pid = 0;
    // card stays async until finalize_save() (so the split writes fast)
}

static const char *basename_of(const char *p) {
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}

// sanitize: keep [A-Za-z0-9._-], turn anything else into '_'; ensure non-empty.
static void sanitize_name(const char *in, char *out, int cap) {
    int j = 0;
    for (int i = 0; in[i] && j < cap - 1; i++) {
        char c = in[i];
        if (isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_') out[j++] = c;
        else out[j++] = '_';
    }
    out[j] = '\0';
    if (j == 0) snprintf(out, cap, "take");
}

// finalize_save: split (M8) or move (stereo) the staged raw take into the
// library's Unsorted folder under the given name, then restore the sync mount.
// returns the base name actually used (into outname).
static void finalize_save(const char *name, int channels, char *outname, int outcap) {
    char nm[64];
    sanitize_name(name, nm, sizeof(nm));

    char unsorted[700];
    snprintf(unsorted, sizeof(unsorted), "%s/Unsorted", g_lib);
    mkdir(g_lib, 0777);
    mkdir(unsorted, 0777);

    if (channels >= 4 && (channels % 2) == 0 && channels > 2) {
        // multichannel (M8): split into stereo pairs, name as prefix
        if (wav_split(g_tmp, unsorted, nm) > 0) unlink(g_tmp);
    } else {
        char dest[900];
        snprintf(dest, sizeof(dest), "%s/%s.wav", unsorted, nm);
        if (rename(g_tmp, dest) != 0) { /* best effort */ }
    }
    remount("sync");
    snprintf(outname, outcap, "%s", nm);
}

// --- app --------------------------------------------------------------------
enum Mode { M_HOME, M_REC, M_NAME };

static int row_width(int row) { return row < KB_CHAR_ROWS ? KB_COLS : KB_ACTIONS; }

int main(int argc, char *argv[]) {
    g_out_dir = (argc > 1) ? argv[1] : ".";

    PWR_setCPUSpeed(CPU_SPEED_MENU);
    SDL_Surface *screen = GFX_init(MODE_MAIN);
    PAD_init();
    PWR_init();
    InitSettings();
    setup_library();

    UI ui;
    char res_dir[600];
    snprintf(res_dir, sizeof(res_dir), "%s/res", g_out_dir);
    int fonts_ok = (ui_load_fonts(&ui, res_dir) == 0);

    struct Input in;
    detect_input(&in);
    write_detect_log(g_out_dir, &in);

    struct Rec rec = {0};
    enum Mode mode = M_HOME;
    int quitting = 0, redraw = 1, frames = 0, shot_done = 0;
    char last_take[64] = {0};

    // naming/keyboard state
    char name[40] = {0};
    int caps = 0, krow = 1, kcol = 0, rec_channels = 2;

    while (!quitting) {
        GFX_startFrame();
        PWR_update(&redraw, NULL, NULL, NULL);
        PAD_poll();

        if (mode == M_REC) {
            if (PAD_justReleased(BTN_A)) {
                rec_channels = in.channels;
                stop_rec(&rec);
                time_t now = time(NULL);
                struct tm *tm = localtime(&now);
                strftime(name, sizeof(name), "rec_%H%M%S", tm);
                caps = 0; krow = 1; kcol = 0;
                mode = M_NAME;
            }
            redraw = 1; // animate reels/timer
        } else if (mode == M_NAME) {
            int n = (int)strlen(name);
            if (PAD_justReleased(BTN_UP))    { if (krow > 0) krow--; }
            if (PAD_justReleased(BTN_DOWN))  { if (krow < KB_CHAR_ROWS) krow++; }
            if (PAD_justReleased(BTN_LEFT))  { if (kcol > 0) kcol--; }
            if (PAD_justReleased(BTN_RIGHT)) { if (kcol < row_width(krow) - 1) kcol++; }
            if (kcol > row_width(krow) - 1) kcol = row_width(krow) - 1;

            int confirm = 0;
            if (PAD_justReleased(BTN_A)) {
                if (krow < KB_CHAR_ROWS) {
                    char k = kb_rows[krow][kcol];
                    if (n < (int)sizeof(name) - 1) {
                        name[n] = caps ? k : (char)tolower((unsigned char)k);
                        name[n + 1] = '\0';
                    }
                } else if (kcol == 0) {
                    caps = !caps;
                } else if (kcol == 1) {
                    if (n > 0) name[n - 1] = '\0';
                } else {
                    confirm = 1;
                }
            }
            if (PAD_justReleased(BTN_B)) { if (n > 0) name[n - 1] = '\0'; }
            if (PAD_justReleased(BTN_Y)) { if (n < (int)sizeof(name) - 1) { name[n] = '_'; name[n + 1] = '\0'; } }
            if (PAD_justReleased(BTN_X)) caps = !caps;
            if (PAD_justReleased(BTN_START)) confirm = 1;

            if (confirm) {
                finalize_save(name, rec_channels, last_take, sizeof(last_take));
                mode = M_HOME;
            }
            redraw = 1;
        } else { // M_HOME
            if (PAD_justReleased(BTN_A) && in.present) { if (start_rec(&rec, &in)) mode = M_REC; }
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
            if (mode == M_REC) {
                UIRec ui_r = {
                    .elapsed = (long)(time(NULL) - rec.start), .take = NULL,
                    .angle = (double)(SDL_GetTicks() % 1200) / 1200.0 * 2.0 * M_PI,
                    .blink = ((time(NULL) % 2) == 0), .levelL = -1, .levelR = -1,
                };
                ui_draw_record(&ui, screen, &ui_in, &ui_r);
            } else if (mode == M_NAME) {
                ui_draw_keyboard(&ui, screen, "NAME SAMPLE", name, caps, krow, kcol);
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
            if (mode == M_HOME) redraw = 0;
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
