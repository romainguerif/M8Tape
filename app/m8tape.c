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
#include <strings.h>
#include <time.h>
#include <math.h>
#include <signal.h>
#include <dirent.h>
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
#include "player.h"

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
static char g_tmpplay[640];                // selection audition temp

static void setup_library(void) {
    char mp[256];
    sdcard_mount(mp, sizeof(mp));
    snprintf(g_lib, sizeof(g_lib), "%s/M8Tape", mp);
    mkdir(g_lib, 0777);
    char uns[600];
    snprintf(uns, sizeof(uns), "%s/Unsorted", g_lib);
    mkdir(uns, 0777);
    snprintf(g_tmp, sizeof(g_tmp), "%s/.tmprec.wav", g_lib);
    snprintf(g_tmpplay, sizeof(g_tmpplay), "%s/.tmpplay.wav", g_lib);
}

// --- recording --------------------------------------------------------------
struct Rec { int active; pid_t pid; time_t start; char path[700]; };

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

// --- library browser + playback ---------------------------------------------
struct Entry { char name[256]; int is_dir; };
static char g_cur[1024];
static struct Entry g_ents[512];
static int g_nent, g_bsel, g_bscroll;
static pid_t g_play_pid;
static int g_play_sel = -1;

static int ent_cmp(const void *a, const void *b) {
    const struct Entry *x = a, *y = b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    return strcasecmp(x->name, y->name);
}

static void list_dir(void) {
    g_nent = 0;
    DIR *d = opendir(g_cur);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && g_nent < 512) {
            if (e->d_name[0] == '.') continue;             // hide dotfiles
            char p[1300];
            snprintf(p, sizeof(p), "%s/%s", g_cur, e->d_name);
            struct stat st;
            if (stat(p, &st) != 0) continue;
            int isdir = S_ISDIR(st.st_mode);
            if (!isdir) {
                const char *dot = strrchr(e->d_name, '.');
                if (!dot || strcasecmp(dot, ".wav") != 0) continue; // wav only
            }
            snprintf(g_ents[g_nent].name, sizeof(g_ents[g_nent].name), "%s", e->d_name);
            g_ents[g_nent].is_dir = isdir;
            g_nent++;
        }
        closedir(d);
    }
    qsort(g_ents, g_nent, sizeof(struct Entry), ent_cmp);
    if (g_bsel >= g_nent) g_bsel = g_nent ? g_nent - 1 : 0;
}

static void crumb_of(char *out, int cap) {
    if (strcmp(g_cur, g_lib) == 0) snprintf(out, cap, "M8TAPE");
    else snprintf(out, cap, "M8TAPE%s", g_cur + strlen(g_lib));
}

static int at_lib_root(void) { return strcmp(g_cur, g_lib) == 0; }

static void reap_play(void) {
    if (g_play_pid > 0) {
        int st;
        if (waitpid(g_play_pid, &st, WNOHANG) == g_play_pid) { g_play_pid = 0; g_play_sel = -1; }
    }
}
static void stop_play(void) {
    if (g_play_pid > 0) { kill(g_play_pid, SIGTERM); int st; waitpid(g_play_pid, &st, 0); }
    g_play_pid = 0; g_play_sel = -1;
}
static void start_play_path(const char *path, int sel) {
    stop_play();
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        char logp[640];
        snprintf(logp, sizeof(logp), "%s/play.log", g_out_dir);
        freopen(logp, "w", stderr); freopen("/dev/null", "w", stdout);
        _exit(play_wav_fade(path) == 0 ? 0 : 1);  // our ALSA player (fade-out on stop)
    }
    g_play_pid = pid; g_play_sel = sel;
}
static void start_play(int idx) {
    char p[1300];
    snprintf(p, sizeof(p), "%s/%s", g_cur, g_ents[idx].name);
    start_play_path(p, idx);
}

static void enter_dir(const char *name) {
    stop_play();
    char nc[1024];
    snprintf(nc, sizeof(nc), "%s/%s", g_cur, name);
    snprintf(g_cur, sizeof(g_cur), "%s", nc);
    g_bsel = 0; g_bscroll = 0; list_dir();
}
static void go_up(void) {
    stop_play();
    char *slash = strrchr(g_cur, '/');
    if (slash && slash > g_cur) *slash = '\0';
    if (strlen(g_cur) < strlen(g_lib)) snprintf(g_cur, sizeof(g_cur), "%s", g_lib);
    g_bsel = 0; g_bscroll = 0; list_dir();
}

// --- editor state -----------------------------------------------------------
static Audio g_au;
static long g_in, g_out, g_curpos;
static int g_modified;
static char g_edit_path[1300];
static float g_mn[2048], g_mx[2048];   // cached waveform peaks
static int g_wave_cols, g_wave_dirty;

// --- app --------------------------------------------------------------------
enum Mode { M_HOME, M_REC, M_NAME, M_BROWSE, M_MENU, M_CONFIRM, M_MOVE, M_EDIT, M_EMENU, M_EDIT_EXIT };
enum NamePurpose { NP_REC, NP_RENAME, NP_NEWFOLDER, NP_SAVEAS };

#define NAV(b) (PAD_justPressed(b) || PAD_justRepeated(b))
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
    char name[64] = {0};
    int caps = 0, krow = 1, kcol = 0, rec_channels = 2, name_purpose = NP_REC;

    // actions menu + move state
    const char *menu_opts[6]; int menu_n = 0, menu_sel = 0, menu_scroll = 0;
    char move_from[1024] = {0}, move_name[256] = {0};

    // editor ops menu
    static const char *EMENU[] = {"NORMALIZE", "FADE IN", "FADE OUT", "REVERSE",
        "TRIM TO SELECTION", "TRIM SILENCE", "TO MONO", "16-BIT", "HALF RATE",
        "SAVE", "SAVE AS", "EXIT"};
    const int EMENU_N = 12;
    int emenu_sel = 0, emenu_scroll = 0;

    while (!quitting) {
        GFX_startFrame();
        PWR_update(&redraw, NULL, NULL, NULL);
        PAD_poll();
        reap_play();

        if (mode == M_REC) {
            if (PAD_justPressed(BTN_A)) {
                rec_channels = in.channels;
                stop_rec(&rec);
                time_t now = time(NULL); struct tm *tm = localtime(&now);
                strftime(name, sizeof(name), "rec_%H%M%S", tm);
                name_purpose = NP_REC; caps = 0; krow = 1; kcol = 0;
                mode = M_NAME;
            }
            redraw = 1;
        } else if (mode == M_NAME) {
            int n = (int)strlen(name);
            if (NAV(BTN_UP))    { if (krow > 0) krow--; }
            if (NAV(BTN_DOWN))  { if (krow < KB_CHAR_ROWS) krow++; }
            if (NAV(BTN_LEFT))  { if (kcol > 0) kcol--; }
            if (NAV(BTN_RIGHT)) { if (kcol < row_width(krow) - 1) kcol++; }
            if (kcol > row_width(krow) - 1) kcol = row_width(krow) - 1;

            int confirm = 0;
            if (PAD_justPressed(BTN_A)) {
                if (krow < KB_CHAR_ROWS) {
                    char k = kb_rows[krow][kcol];
                    if (n < (int)sizeof(name) - 1) { name[n] = caps ? k : (char)tolower((unsigned char)k); name[n + 1] = '\0'; }
                } else if (kcol == 0) caps = !caps;
                else if (kcol == 1) { if (n > 0) name[n - 1] = '\0'; }
                else confirm = 1;
            }
            if (PAD_justPressed(BTN_B)) { if (n > 0) name[n - 1] = '\0'; }
            if (PAD_justPressed(BTN_Y)) { if (n < (int)sizeof(name) - 1) { name[n] = '_'; name[n + 1] = '\0'; } }
            if (PAD_justPressed(BTN_X)) caps = !caps;
            if (PAD_justPressed(BTN_START)) confirm = 1;

            if (confirm) {
                char nm[64]; sanitize_name(name, nm, sizeof(nm));
                if (name_purpose == NP_REC) {
                    finalize_save(name, rec_channels, last_take, sizeof(last_take));
                    mode = M_HOME;
                } else if (name_purpose == NP_RENAME && g_nent > 0) {
                    char oldp[1300], newp[1400];
                    snprintf(oldp, sizeof(oldp), "%s/%s", g_cur, g_ents[g_bsel].name);
                    if (g_ents[g_bsel].is_dir) snprintf(newp, sizeof(newp), "%s/%s", g_cur, nm);
                    else snprintf(newp, sizeof(newp), "%s/%s.wav", g_cur, nm);
                    rename(oldp, newp); stop_play(); list_dir(); mode = M_BROWSE;
                } else if (name_purpose == NP_SAVEAS) {
                    char dest[1400]; snprintf(dest, sizeof(dest), "%s/%s.wav", g_cur, nm);
                    remount("async"); wav_save(dest, &g_au); remount("sync");
                    g_modified = 0; audio_free(&g_au); list_dir(); mode = M_BROWSE;
                } else {
                    char np[1300]; snprintf(np, sizeof(np), "%s/%s", g_cur, nm); mkdir(np, 0777);
                    list_dir(); mode = M_BROWSE;
                }
            }
            redraw = 1;
        } else if (mode == M_BROWSE) {
            int vis = ui_browser_visible_rows(screen);
            if (NAV(BTN_UP))   { if (g_bsel > 0) g_bsel--; }
            if (NAV(BTN_DOWN)) { if (g_bsel < g_nent - 1) g_bsel++; }
            if (g_bsel < g_bscroll) g_bscroll = g_bsel;
            if (g_bsel >= g_bscroll + vis) g_bscroll = g_bsel - vis + 1;

            if (PAD_justPressed(BTN_A) && g_nent > 0) {
                if (g_ents[g_bsel].is_dir) enter_dir(g_ents[g_bsel].name);
                else { if (g_play_sel == g_bsel) stop_play(); else start_play(g_bsel); }
            }
            if (PAD_justPressed(BTN_B)) { if (at_lib_root()) { stop_play(); mode = M_HOME; redraw = 1; } else go_up(); }
            if (PAD_justPressed(BTN_X)) { name[0] = '\0'; name_purpose = NP_NEWFOLDER; caps = 0; krow = 1; kcol = 0; mode = M_NAME; }
            if (PAD_justPressed(BTN_Y) && g_nent > 0) {
                menu_n = 0;
                if (g_ents[g_bsel].is_dir) menu_opts[menu_n++] = "OPEN";
                else { menu_opts[menu_n++] = "PLAY"; menu_opts[menu_n++] = "EDIT"; }
                menu_opts[menu_n++] = "RENAME";
                menu_opts[menu_n++] = "MOVE";
                menu_opts[menu_n++] = "DELETE";
                menu_sel = 0; menu_scroll = 0; mode = M_MENU;
            }
            redraw = 1;
        } else if (mode == M_MENU) {
            int mvis = ui_menu_visible_rows(screen);
            if (NAV(BTN_UP))   { if (menu_sel > 0) menu_sel--; }
            if (NAV(BTN_DOWN)) { if (menu_sel < menu_n - 1) menu_sel++; }
            if (menu_sel < menu_scroll) menu_scroll = menu_sel;
            if (menu_sel >= menu_scroll + mvis) menu_scroll = menu_sel - mvis + 1;
            if (PAD_justPressed(BTN_B)) mode = M_BROWSE;
            if (PAD_justPressed(BTN_A)) {
                const char *op = menu_opts[menu_sel];
                if (!strcmp(op, "OPEN")) { enter_dir(g_ents[g_bsel].name); mode = M_BROWSE; }
                else if (!strcmp(op, "PLAY")) { start_play(g_bsel); mode = M_BROWSE; }
                else if (!strcmp(op, "EDIT")) {
                    char p[1300]; snprintf(p, sizeof(p), "%s/%s", g_cur, g_ents[g_bsel].name);
                    stop_play();
                    if (wav_load(p, &g_au) == 0 && g_au.frames > 0) {
                        snprintf(g_edit_path, sizeof(g_edit_path), "%s", p);
                        g_in = 0; g_out = g_au.frames; g_curpos = 0; g_modified = 0;
                        g_wave_dirty = 1; emenu_sel = 0; emenu_scroll = 0; mode = M_EDIT;
                    } else { audio_free(&g_au); mode = M_BROWSE; }
                }
                else if (!strcmp(op, "RENAME")) {
                    snprintf(name, sizeof(name), "%s", g_ents[g_bsel].name);
                    if (!g_ents[g_bsel].is_dir) { char *d = strrchr(name, '.'); if (d && !strcasecmp(d, ".wav")) *d = '\0'; }
                    name_purpose = NP_RENAME; caps = 0; krow = 1; kcol = 0; mode = M_NAME;
                } else if (!strcmp(op, "MOVE")) {
                    snprintf(move_from, sizeof(move_from), "%s", g_cur);
                    snprintf(move_name, sizeof(move_name), "%s", g_ents[g_bsel].name);
                    stop_play();
                    snprintf(g_cur, sizeof(g_cur), "%s", g_lib);
                    g_bsel = 0; g_bscroll = 0; list_dir(); mode = M_MOVE;
                } else if (!strcmp(op, "DELETE")) mode = M_CONFIRM;
            }
            redraw = 1;
        } else if (mode == M_CONFIRM) {
            if (PAD_justPressed(BTN_B)) mode = M_BROWSE;
            if (PAD_justPressed(BTN_A)) {
                char p[1300]; snprintf(p, sizeof(p), "%s/%s", g_cur, g_ents[g_bsel].name);
                stop_play();
                if (g_ents[g_bsel].is_dir) { char c[1400]; snprintf(c, sizeof(c), "rm -rf '%s'", p); int rc = system(c); (void)rc; }
                else unlink(p);
                list_dir(); mode = M_BROWSE;
            }
            redraw = 1;
        } else if (mode == M_MOVE) {
            int vis = ui_browser_visible_rows(screen);
            if (NAV(BTN_UP))   { if (g_bsel > 0) g_bsel--; }
            if (NAV(BTN_DOWN)) { if (g_bsel < g_nent - 1) g_bsel++; }
            if (g_bsel < g_bscroll) g_bscroll = g_bsel;
            if (g_bsel >= g_bscroll + vis) g_bscroll = g_bsel - vis + 1;
            if (PAD_justPressed(BTN_A) && g_nent > 0 && g_ents[g_bsel].is_dir) enter_dir(g_ents[g_bsel].name);
            if (PAD_justPressed(BTN_B)) {
                if (at_lib_root()) { snprintf(g_cur, sizeof(g_cur), "%s", move_from); g_bsel = 0; g_bscroll = 0; list_dir(); mode = M_BROWSE; }
                else go_up();
            }
            if (PAD_justPressed(BTN_START) || PAD_justPressed(BTN_Y)) {
                char src[1300], dst[1400];
                snprintf(src, sizeof(src), "%s/%s", move_from, move_name);
                snprintf(dst, sizeof(dst), "%s/%s", g_cur, move_name);
                if (strcmp(src, dst) != 0) rename(src, dst);
                snprintf(g_cur, sizeof(g_cur), "%s", move_from);
                g_bsel = 0; g_bscroll = 0; list_dir(); mode = M_BROWSE;
            }
            redraw = 1;
        } else if (mode == M_EDIT) {
            long step = g_au.frames / 1000; if (step < 1) step = 1;
            long fast = g_au.frames / 20; if (fast < 1) fast = 1;
            if (NAV(BTN_LEFT))  g_curpos -= step;
            if (NAV(BTN_RIGHT)) g_curpos += step;
            if (NAV(BTN_L1))    g_curpos -= fast;
            if (NAV(BTN_R1))    g_curpos += fast;
            if (g_curpos < 0) g_curpos = 0;
            if (g_curpos > g_au.frames) g_curpos = g_au.frames;
            if (PAD_justPressed(BTN_Y)) { g_in = g_curpos; if (g_in >= g_out) g_in = g_out > 0 ? g_out - 1 : 0; }
            if (PAD_justPressed(BTN_X)) { g_out = g_curpos; if (g_out <= g_in) g_out = g_in + 1; if (g_out > g_au.frames) g_out = g_au.frames; }
            if (PAD_justPressed(BTN_A)) {
                if (g_play_pid > 0) stop_play();
                else if (wav_save_range(g_tmpplay, &g_au, g_in, g_out) == 0) start_play_path(g_tmpplay, -2);
            }
            if (PAD_justPressed(BTN_START)) { emenu_sel = 0; emenu_scroll = 0; mode = M_EMENU; }
            if (PAD_justPressed(BTN_B)) {
                if (g_modified) mode = M_EDIT_EXIT;
                else { stop_play(); audio_free(&g_au); mode = M_BROWSE; }
            }
            redraw = 1;
        } else if (mode == M_EDIT_EXIT) {
            if (PAD_justPressed(BTN_A)) { stop_play(); audio_free(&g_au); mode = M_BROWSE; }
            if (PAD_justPressed(BTN_B)) mode = M_EDIT;
            redraw = 1;
        } else if (mode == M_EMENU) {
            int mvis = ui_menu_visible_rows(screen);
            if (NAV(BTN_UP))   { if (emenu_sel > 0) emenu_sel--; }
            if (NAV(BTN_DOWN)) { if (emenu_sel < EMENU_N - 1) emenu_sel++; }
            if (emenu_sel < emenu_scroll) emenu_scroll = emenu_sel;
            if (emenu_sel >= emenu_scroll + mvis) emenu_scroll = emenu_sel - mvis + 1;
            if (PAD_justPressed(BTN_B)) mode = M_EDIT;
            if (PAD_justPressed(BTN_A)) {
                const char *op = EMENU[emenu_sel];
                int back = 1;
                if (!strcmp(op, "NORMALIZE")) au_normalize(&g_au);
                else if (!strcmp(op, "FADE IN")) au_fade_in(&g_au, g_in, g_out);
                else if (!strcmp(op, "FADE OUT")) au_fade_out(&g_au, g_in, g_out);
                else if (!strcmp(op, "REVERSE")) au_reverse(&g_au, g_in, g_out);
                else if (!strcmp(op, "TRIM TO SELECTION")) { au_crop(&g_au, g_in, g_out); g_in = 0; g_out = g_au.frames; g_curpos = 0; }
                else if (!strcmp(op, "TRIM SILENCE")) { long s, e; au_silence_bounds(&g_au, 0.01f, &s, &e); au_crop(&g_au, s, e); g_in = 0; g_out = g_au.frames; g_curpos = 0; }
                else if (!strcmp(op, "TO MONO")) au_to_mono(&g_au);
                else if (!strcmp(op, "16-BIT")) g_au.bits = 16;
                else if (!strcmp(op, "HALF RATE")) { au_halve_rate(&g_au); g_in = 0; g_out = g_au.frames; g_curpos = 0; }
                else if (!strcmp(op, "SAVE")) { remount("async"); wav_save(g_edit_path, &g_au); remount("sync"); g_modified = 0; list_dir(); back = 0; }
                else if (!strcmp(op, "SAVE AS")) { name[0] = '\0'; name_purpose = NP_SAVEAS; caps = 0; krow = 1; kcol = 0; mode = M_NAME; back = -1; }
                else if (!strcmp(op, "EXIT")) {
                    back = -1;
                    if (g_modified) mode = M_EDIT_EXIT;
                    else { stop_play(); audio_free(&g_au); mode = M_BROWSE; }
                }
                if (back == 1) { g_modified = 1; g_wave_dirty = 1; mode = M_EDIT; }
                else if (back == 0) { g_wave_dirty = 1; mode = M_EDIT; }
            }
            redraw = 1;
        } else { // M_HOME
            if (PAD_justPressed(BTN_A) && in.present) { if (start_rec(&rec, &in)) mode = M_REC; }
            if (PAD_justPressed(BTN_X)) { snprintf(g_cur, sizeof(g_cur), "%s", g_lib); g_bsel = 0; g_bscroll = 0; list_dir(); mode = M_BROWSE; redraw = 1; }
            if (PAD_justReleased(BTN_B) || PAD_justReleased(BTN_MENU)) quitting = 1;
            if (++frames >= 60) {
                frames = 0;
                struct Input now; detect_input(&now);
                if (now.present != in.present || now.channels != in.channels || now.card != in.card) {
                    in = now; write_detect_log(g_out_dir, &in); redraw = 1;
                }
            }
        }

        if (redraw && fonts_ok) {
            UIInput ui_in = {in.present, classify(&in), in.channels, in.rate, in.format[0] ? in.format : NULL};
            if (mode == M_REC) {
                UIRec ui_r = { .elapsed = (long)(time(NULL) - rec.start), .take = NULL,
                    .angle = (double)(SDL_GetTicks() % 1200) / 1200.0 * 2.0 * M_PI,
                    .blink = ((time(NULL) % 2) == 0), .levelL = -1, .levelR = -1 };
                ui_draw_record(&ui, screen, &ui_in, &ui_r);
            } else if (mode == M_NAME) {
                const char *title = name_purpose == NP_RENAME ? "RENAME"
                                  : name_purpose == NP_NEWFOLDER ? "NEW FOLDER" : "NAME SAMPLE";
                ui_draw_keyboard(&ui, screen, title, name, caps, krow, kcol);
            } else if (mode == M_BROWSE || mode == M_MOVE) {
                const char *names[512]; int isd[512];
                for (int i = 0; i < g_nent; i++) { names[i] = g_ents[i].name; isd[i] = g_ents[i].is_dir; }
                char crumb[700]; crumb_of(crumb, sizeof(crumb));
                if (mode == M_MOVE) {
                    char mc[770]; snprintf(mc, sizeof(mc), "MOVE TO  %s", crumb);
                    ui_draw_browser(&ui, screen, mc, names, isd, g_nent, g_bsel, g_bscroll, -1,
                                    "START HERE   A OPEN   B BACK");
                } else {
                    ui_draw_browser(&ui, screen, crumb, names, isd, g_nent, g_bsel, g_bscroll, g_play_sel,
                                    "A OPEN/PLAY  Y ACTIONS  X NEW FOLDER  B BACK");
                }
            } else if (mode == M_MENU) {
                ui_draw_menu(&ui, screen, "ACTIONS", menu_opts, menu_n, menu_sel, menu_scroll);
            } else if (mode == M_CONFIRM) {
                ui_draw_confirm(&ui, screen, "DELETE", g_nent ? g_ents[g_bsel].name : "");
            } else if (mode == M_EDIT) {
                int cols = ui_editor_cols(screen); if (cols > 2048) cols = 2048;
                if (g_wave_dirty || cols != g_wave_cols) {
                    au_peaks(&g_au, 0, g_au.frames, cols, g_mn, g_mx);
                    g_wave_cols = cols; g_wave_dirty = 0;
                }
                char info[96];
                long secs = g_au.rate ? g_au.frames / g_au.rate : 0;
                snprintf(info, sizeof(info), "%ld:%02ld  %dHZ  %dCH  %dBIT%s",
                         secs / 60, secs % 60, g_au.rate, g_au.ch, g_au.bits, g_modified ? "  *" : "");
                double inf = g_au.frames ? (double)g_in / g_au.frames : 0;
                double outf = g_au.frames ? (double)g_out / g_au.frames : 1;
                double curf = g_au.frames ? (double)g_curpos / g_au.frames : 0;
                const char *nm = strrchr(g_edit_path, '/'); nm = nm ? nm + 1 : g_edit_path;
                ui_draw_editor(&ui, screen, nm, info, g_mn, g_mx, cols, inf, outf, curf, g_play_pid > 0);
            } else if (mode == M_EMENU) {
                ui_draw_menu(&ui, screen, "EDIT", EMENU, EMENU_N, emenu_sel, emenu_scroll);
            } else if (mode == M_EDIT_EXIT) {
                ui_draw_confirm(&ui, screen, "DISCARD?", "UNSAVED CHANGES");
            } else {
                ui_draw_home(&ui, screen, &ui_in);
            }
            GFX_flip(screen);
            if (!shot_done) { char shot[640]; snprintf(shot, sizeof(shot), "%s/screen.bmp", g_out_dir); SDL_SaveBMP(screen, shot); shot_done = 1; }
            if (mode == M_HOME) redraw = 0;
        } else if (!redraw) {
            GFX_sync();
        }
    }

    stop_play();
    if (rec.active) stop_rec(&rec);
    ui_free(&ui);
    QuitSettings();
    PWR_quit();
    PAD_quit();
    GFX_quit();
    return 0;
}
