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
#include <alsa/asoundlib.h>   // used ONLY inside the forked preview child
#include <sys/mman.h>
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
#include "h3000.h"

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
static char g_sd[512];                     // <SDCARD> mount root — browse/move/folders span the whole card (e.g. to the LGPT folder)
static char g_lgpt[1024];                  // LGPT/Piggy data dir (under the hidden .ports/) — direct-jump target, empty if not found
static char g_tmp[600];                    // raw take staged here before naming
static char g_tmpplay[640];                // selection audition temp

static int dir_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}
// Pull value="..." (or value='...') for `key` out of an XML-ish line; trims a
// trailing slash. Returns 1 on success.
static int xml_value(const char *line, const char *key, char *out, int cap) {
    const char *k = strstr(line, key); if (!k) return 0;
    const char *v = strstr(k, "value"); if (!v) return 0;
    v += 5;
    while (*v && *v != '"' && *v != '\'') v++;
    if (!*v) return 0;
    char q = *v++; int i = 0;
    while (*v && *v != q && i < cap - 1) out[i++] = *v++;
    out[i] = '\0';
    while (i > 0 && out[i - 1] == '/') out[--i] = '\0';
    return i > 0;
}
// Locate the LGPT/Piggy SAMPLE LIBRARY for the direct jump. The data is under
// PortMaster's HIDDEN .ports/ (the browser hides dotdirs, so it can't be reached
// by navigating). Best source of truth = the build's config.xml <SAMPLELIB .../>
// (the user's dev build points at the stock build's populated samplelib). Falls
// back to scanning for an existing samplelib, then any gamedir.
static void detect_lgpt(void) {
    g_lgpt[0] = '\0';
    static const char *const roots[] = {
        "Roms/Ports (PORTS)/.ports", "Roms/PORTS/.ports", "Roms/ports/.ports", "Ports/.ports", 0 };
    static const char *const games[] = { "littlegptracker-dev", "littlegptracker", 0 };
    char cfg[1000], line[1024], val[1024], p[1024];
    // 1) read SAMPLELIB straight from a build's config.xml (follows the user's config)
    for (int r = 0; roots[r] && !g_lgpt[0]; r++)
        for (int g = 0; games[g] && !g_lgpt[0]; g++) {
            snprintf(cfg, sizeof(cfg), "%s/%s/%s/config.xml", g_sd, roots[r], games[g]);
            FILE *f = fopen(cfg, "r"); if (!f) continue;
            while (fgets(line, sizeof(line), f))
                if (xml_value(line, "SAMPLELIB", val, sizeof(val)) && dir_exists(val)) {
                    snprintf(g_lgpt, sizeof(g_lgpt), "%s", val); break;
                }
            fclose(f);
        }
    if (g_lgpt[0]) return;
    // 2) fallback: first existing samplelib, then any gamedir
    static const char *const subs[] = {
        "littlegptracker/samplelib", "littlegptracker-dev/samplelib",
        "littlegptracker-dev", "littlegptracker", 0 };
    for (int r = 0; roots[r]; r++)
        for (int s = 0; subs[s]; s++) {
            snprintf(p, sizeof(p), "%s/%s/%s", g_sd, roots[r], subs[s]);
            if (dir_exists(p)) { snprintf(g_lgpt, sizeof(g_lgpt), "%s", p); return; }
        }
}

static void setup_library(void) {
    char mp[256];
    sdcard_mount(mp, sizeof(mp));
    snprintf(g_sd, sizeof(g_sd), "%s", mp);
    detect_lgpt();
    snprintf(g_lib, sizeof(g_lib), "%s/M8Tape", mp);
    mkdir(g_lib, 0777);
    char uns[600];
    snprintf(uns, sizeof(uns), "%s/Unsorted", g_lib);
    mkdir(uns, 0777);
    snprintf(g_tmp, sizeof(g_tmp), "%s/.tmprec.wav", g_lib);
    snprintf(g_tmpplay, sizeof(g_tmpplay), "%s/.tmpplay.wav", g_lib);
}

// --- settings (persisted in the pak) ----------------------------------------
struct Settings { int trim_on_rec; int trim_db; int autostop; int autostop_sec; };
static struct Settings g_set = {0, -48, 0, 3};

static void load_settings(void) {
    char p[700]; snprintf(p, sizeof(p), "%s/settings.cfg", g_out_dir);
    FILE *f = fopen(p, "r"); if (!f) return;
    char line[128]; int v;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "trim_on_rec=%d", &v) == 1) g_set.trim_on_rec = v;
        else if (sscanf(line, "trim_db=%d", &v) == 1) g_set.trim_db = v;
        else if (sscanf(line, "autostop=%d", &v) == 1) g_set.autostop = v;
        else if (sscanf(line, "autostop_sec=%d", &v) == 1) g_set.autostop_sec = v;
    }
    fclose(f);
}
static void save_settings(void) {
    char p[700]; snprintf(p, sizeof(p), "%s/settings.cfg", g_out_dir);
    FILE *f = fopen(p, "w"); if (!f) return;
    fprintf(f, "trim_on_rec=%d\ntrim_db=%d\nautostop=%d\nautostop_sec=%d\n",
            g_set.trim_on_rec, g_set.trim_db, g_set.autostop, g_set.autostop_sec);
    fclose(f);
}
static float db_to_lin(int db) { return powf(10.0f, db / 20.0f); }

// --- live input level (read the tail of the growing raw take) ---------------
static float g_levelL = -1, g_levelR = -1;

static int bps_of_fmt(const char *fmt) {
    if (!fmt) return 2;
    if (strstr(fmt, "24_3")) return 3;
    if (strstr(fmt, "S32") || strstr(fmt, "24_LE") || strstr(fmt, "32")) return 4;
    if (strstr(fmt, "16")) return 2;
    if (strstr(fmt, "U8") || strstr(fmt, "S8")) return 1;
    return 2;
}
static float decf(const unsigned char *p, int bps) {
    if (bps == 2) return (int16_t)(p[0] | p[1] << 8) / 32768.0f;
    if (bps == 3) { int v = p[0] | p[1] << 8 | p[2] << 16; if (v & 0x800000) v |= ~0xFFFFFF; return v / 8388608.0f; }
    if (bps == 4) { int32_t v = (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24); return v / 2147483648.0f; }
    return ((int)p[0] - 128) / 128.0f;
}
static void update_rec_level(const struct Input *in) {
    int bps = bps_of_fmt(in->format), ch = in->channels > 0 ? in->channels : 2;
    long frame = (long)ch * bps;
    FILE *f = fopen(g_tmp, "rb"); if (!f) return;
    fseek(f, 0, SEEK_END); long sz = ftell(f);
    long avail = sz - 44;
    if (avail < frame) { fclose(f); return; }
    long readb = 16384; if (readb > avail) readb = avail; readb -= readb % frame;
    if (readb < frame) { fclose(f); return; }
    fseek(f, sz - readb, SEEK_SET);
    unsigned char *buf = malloc(readb);
    if (!buf) { fclose(f); return; }
    size_t got = fread(buf, 1, readb, f); fclose(f);
    long nf = (long)got / frame;
    float pk0 = 0, pk1 = 0;
    for (long i = 0; i < nf; i++) {
        const unsigned char *fp = buf + i * frame;
        float a = fabsf(decf(fp, bps)); if (a > pk0) pk0 = a;
        if (ch >= 2) { float b = fabsf(decf(fp + bps, bps)); if (b > pk1) pk1 = b; }
    }
    free(buf);
    if (ch < 2) pk1 = pk0;
    g_levelL = pk0 > g_levelL ? pk0 : g_levelL * 0.80f;   // fast attack, slow decay
    g_levelR = pk1 > g_levelR ? pk1 : g_levelR * 0.80f;
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

    // optional: auto-trim leading/trailing silence on the raw take (streaming)
    if (g_set.trim_on_rec) {
        char t2[760];
        snprintf(t2, sizeof(t2), "%s.trim", g_tmp);
        if (wav_trim_silence_file(g_tmp, t2, db_to_lin(g_set.trim_db)) == 0) {
            unlink(g_tmp); rename(t2, g_tmp);
        }
    }

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
    size_t ls = strlen(g_lib), ss = strlen(g_sd);
    if (strcmp(g_cur, g_lib) == 0) snprintf(out, cap, "M8TAPE");
    else if (strncmp(g_cur, g_lib, ls) == 0 && g_cur[ls] == '/') snprintf(out, cap, "M8TAPE%s", g_cur + ls);
    else if (strcmp(g_cur, g_sd) == 0) snprintf(out, cap, "SD");
    else if (strncmp(g_cur, g_sd, ss) == 0 && g_cur[ss] == '/') snprintf(out, cap, "SD%s", g_cur + ss);
    else snprintf(out, cap, "%s", g_cur);
}

// true at the SD-card root — the top of navigation now (was the M8Tape root)
static int at_sd_root(void) { return strcmp(g_cur, g_sd) == 0; }

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
    if (strlen(g_cur) < strlen(g_sd)) snprintf(g_cur, sizeof(g_cur), "%s", g_sd);
    g_bsel = 0; g_bscroll = 0; list_dir();
}

// --- editor state -----------------------------------------------------------
static Audio g_au;
static long g_in, g_out, g_curpos;
static int g_modified;
static char g_edit_path[1300];
static float g_mn[2048], g_mx[2048];   // cached waveform peaks (of the view)
static int g_wave_cols, g_wave_dirty;
static long g_view_span;               // visible frames (zoom)
static long g_pk_vs = -1, g_pk_span = -1; // peak cache key (view start/span)

// --- real-time H3000 preview as a CHILD PROCESS -----------------------------
// CRITICAL: ALSA is only ever opened in the forked child (the main SDL process
// opening ALSA wedges the whole audio codec). Live params + mono source live in
// an mmap'd shared region so the UI can tweak parameters in real time.
typedef struct {
    volatile int stop;
    int algo;                       // index into h3k_algos
    float params[H3K_MAX_PARAMS];   // updated live by the UI
    int frames;
    int rate;
    // mono source floats follow immediately after this struct in the mapping
} PvShared;

static int   g_pv_active = 0;
static pid_t g_pv_pid = 0;
static PvShared *g_pv_shm = NULL;
static size_t g_pv_size = 0;

// runs in the child: opens ALSA, loops the source through the chosen algo, reading
// live params from shared memory. Never returns (calls _exit).
static void pv_child(PvShared *shm) {
    float *src = (float *)((char *)shm + sizeof(PvShared));
    snd_pcm_t *pcm = NULL;
    int ok = 0;
    for (int t = 0; t < 10 && !ok; t++) {
        if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) == 0 &&
            // 400 ms of buffer (was 80) so scheduling jitter / a heavy DSP block
            // (reverb, vocoder, band delay...) can't underrun the device -> crackle.
            snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                               2, shm->rate, 1, 400000) == 0) ok = 1;
        else { if (pcm) { snd_pcm_close(pcm); pcm = NULL; } usleep(50000); }
    }
    if (!ok) _exit(1);
    // Re-apply the system volume now the PCM/codec is open: opening "default" can
    // reset the mixer, leaving the preview silent until the user nudges volume +/-.
    SetVolume(GetVolume());
    H3kEngine *st = h3k_create(shm->algo, shm->rate);
    if (!st) { snd_pcm_close(pcm); _exit(1); }
    enum { BLK = 2048 };                           // bigger blocks = more headroom
    float dry[BLK], out[BLK * 2];
    short s16[BLK * 2];
    long pos = 0, xruns = 0, blocks = 0;
    // Seamless preview loop: play a loop of length loopL = frames - xf, and
    // equal-power crossfade the HEAD (fading in) with the TAIL (fading out) over
    // the first xf samples. The wrap then joins adjacent samples (loopL-1 -> loopL)
    // and the crossfade smooths head/tail, so there is no per-loop seam click.
    long xf = (long)(0.020 * (double)shm->rate);
    if (xf > shm->frames / 4) xf = shm->frames / 4;
    if (xf < 1) xf = 1;
    long loopL = shm->frames - xf; if (loopL < 1) loopL = 1;
    while (!shm->stop) {
        if (getppid() == 1) break;                 // parent gone → don't orphan
        for (int i = 0; i < BLK; i++) {
            float smp;
            if (pos < xf) {                        // crossfade head-in with tail-out
                float t = ((float)pos + 0.5f) / (float)xf;
                smp = src[pos] * sinf(t * 1.5707963f) + src[loopL + pos] * cosf(t * 1.5707963f);
            } else {
                smp = src[pos];
            }
            dry[i] = smp;
            if (++pos >= loopL) pos = 0;
        }
        float p[H3K_MAX_PARAMS];
        for (int k = 0; k < H3K_MAX_PARAMS; k++) p[k] = shm->params[k];   // live snapshot
        h3k_block(st, dry, BLK, p, out);
        for (int i = 0; i < BLK * 2; i++) {
            float v = out[i]; if (v > 1) v = 1; if (v < -1) v = -1;
            s16[i] = (short)(v * 32767);
        }
        snd_pcm_sframes_t w = snd_pcm_writei(pcm, s16, BLK);
        if (w < 0) { xruns++; snd_pcm_recover(pcm, (int)w, 1); }
        blocks++;
    }
    h3k_destroy(st);
    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    // breadcrumb: lets us tell real underruns (crackle) from clean playback.
    char lp[1024];
    snprintf(lp, sizeof(lp), "%s/preview.log", g_out_dir);
    FILE *lf = fopen(lp, "w");
    if (lf) { fprintf(lf, "algo=%d rate=%d blk=%d blocks=%ld xruns=%ld\n",
                      shm->algo, shm->rate, BLK, blocks, xruns); fclose(lf); }
    _exit(0);
}

static void preview_stop(void) {
    if (!g_pv_active) return;
    if (g_pv_shm) g_pv_shm->stop = 1;             // ask child to finish gracefully
    int st, i = 0;
    while (g_pv_pid > 0 && waitpid(g_pv_pid, &st, WNOHANG) == 0 && i < 40) { usleep(10000); i++; }
    if (i >= 40 && g_pv_pid > 0) { kill(g_pv_pid, SIGKILL); waitpid(g_pv_pid, &st, 0); }
    if (g_pv_shm) munmap(g_pv_shm, g_pv_size);
    g_pv_shm = NULL; g_pv_pid = 0; g_pv_active = 0;
}
// reap the preview child if it exited on its own (e.g. ALSA open failed) so the
// PREVIEW indicator reflects reality.
static void reap_preview(void) {
    if (g_pv_active && g_pv_pid > 0) {
        int st;
        if (waitpid(g_pv_pid, &st, WNOHANG) == g_pv_pid) {
            if (g_pv_shm) munmap(g_pv_shm, g_pv_size);
            g_pv_shm = NULL; g_pv_pid = 0; g_pv_active = 0;
        }
    }
}
static void preview_start(const Audio *a, int algo, const float *params) {
    if (g_pv_active || !a->data || a->frames < 1) return;
    stop_play();                                   // free the device for the child
    long F = a->frames;
    size_t sz = sizeof(PvShared) + (size_t)F * sizeof(float);
    PvShared *shm = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shm == MAP_FAILED) return;
    shm->stop = 0; shm->frames = (int)F;
    shm->rate = a->rate > 0 ? a->rate : 48000;
    shm->algo = algo;
    for (int k = 0; k < H3K_MAX_PARAMS; k++) shm->params[k] = params[k];
    float *src = (float *)((char *)shm + sizeof(PvShared));
    for (long i = 0; i < F; i++) {
        float s = 0;
        for (int c = 0; c < a->ch; c++) s += a->data[i * a->ch + c];
        src[i] = s / a->ch;
    }
    // (The preview child crossfades the loop seam itself — see pv_child — so the
    // source is passed through unmodified here.)
    pid_t pid = fork();
    if (pid < 0) { munmap(shm, sz); return; }
    if (pid == 0) {
        freopen("/dev/null", "w", stdout);
        pv_child(shm);   // never returns
        _exit(0);
    }
    g_pv_pid = pid; g_pv_shm = shm; g_pv_size = sz; g_pv_active = 1;
}

// --- app --------------------------------------------------------------------
enum Mode { M_HOME, M_REC, M_NAME, M_BROWSE, M_MENU, M_CONFIRM, M_MOVE, M_EDIT, M_EMENU, M_EDIT_EXIT, M_SETTINGS, M_FX_PICK, M_H3000 };
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
    PWR_disableAutosleep();   // audio app: never idle-sleep the screen mid-listen
                              // (during preview/playback there are no button presses,
                              //  and the system 'sleep off' wasn't honored in-app)
    SetVolume(GetVolume());   // push the saved system volume to the mixer on launch
                              // (we open ALSA directly, so nothing else applies it →
                              //  otherwise the NextUI volume isn't honored until the
                              //  user nudges +/- in-app)
    setup_library();
    load_settings();

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
    static const char *EMENU[] = {"H3000 FX", "NORMALIZE", "GAIN +1 DB", "GAIN -1 DB",
        "FADE IN", "FADE OUT", "LOOP XFADE", "REVERSE", "HIGH-PASS",
        "TRIM TO SELECTION", "TRIM SILENCE", "TO MONO", "16-BIT", "HALF RATE",
        "SAVE", "SAVE AS", "EXIT"};
    const int EMENU_N = 17;
    int emenu_sel = 0, emenu_scroll = 0;

    // recording level / auto-stop, settings cursor
    int lvlctr = 0, set_sel = 0;
    time_t last_sound = 0;
    int edit_accel = 0;   // editor cursor acceleration (frames held)

    // H3000 FX: current algorithm + its live params + cursors
    int   fx_algo = 0;
    float fx_params[H3K_MAX_PARAMS] = {0};
    int   h_sel = 0;                            // selected param row in FX screen
    int   fx_pick_sel = 0, fx_pick_scroll = 0;  // algorithm picker list

    while (!quitting) {
        GFX_startFrame();
        PWR_update(&redraw, NULL, NULL, NULL);
        PAD_poll();
        reap_play();
        reap_preview();

        if (mode == M_REC) {
            int stop_now = PAD_justPressed(BTN_A);
            if (++lvlctr >= 6) {           // ~100ms: refresh level meter
                lvlctr = 0;
                update_rec_level(&in);
                float lv = g_levelL > g_levelR ? g_levelL : g_levelR;
                if (lv > db_to_lin(g_set.trim_db)) last_sound = time(NULL);
            }
            if (g_set.autostop && (time(NULL) - rec.start) > 1 &&
                (time(NULL) - last_sound) >= g_set.autostop_sec) stop_now = 1;
            if (stop_now) {
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
            if (PAD_justPressed(BTN_B)) { if (at_sd_root()) { stop_play(); mode = M_HOME; redraw = 1; } else go_up(); }
            if (PAD_justPressed(BTN_SELECT) && g_lgpt[0]) { stop_play(); snprintf(g_cur, sizeof(g_cur), "%s", g_lgpt); g_bsel = 0; g_bscroll = 0; list_dir(); }
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
                        g_view_span = g_au.frames; g_pk_vs = -1;
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
                if (at_sd_root()) { snprintf(g_cur, sizeof(g_cur), "%s", move_from); g_bsel = 0; g_bscroll = 0; list_dir(); mode = M_BROWSE; }
                else go_up();
            }
            if (PAD_justPressed(BTN_SELECT) && g_lgpt[0]) { snprintf(g_cur, sizeof(g_cur), "%s", g_lgpt); g_bsel = 0; g_bscroll = 0; list_dir(); }
            if (PAD_justPressed(BTN_START) || PAD_justPressed(BTN_Y)) {
                char src[1300], dst[1400];
                snprintf(src, sizeof(src), "%s/%s", move_from, move_name);
                snprintf(dst, sizeof(dst), "%s/%s", g_cur, move_name);
                if (strcmp(src, dst) != 0) { rename(src, dst); sync(); }  // flush so LGPT sees the moved sample
                snprintf(g_cur, sizeof(g_cur), "%s", move_from);
                g_bsel = 0; g_bscroll = 0; list_dir(); mode = M_BROWSE;
            }
            redraw = 1;
        } else if (mode == M_EDIT) {
            int ecols = ui_editor_cols(screen);
            long minspan = g_au.frames < 128 ? g_au.frames : 128;
            if (g_view_span > g_au.frames) g_view_span = g_au.frames;
            if (g_view_span < minspan) g_view_span = minspan;
            long span = g_view_span;
            long step = span / ecols; if (step < 1) step = 1;   // ~1px → fine when zoomed
            long page = span / 4; if (page < 1) page = 1;
            if (PAD_justPressed(BTN_R2)) { g_view_span /= 2; if (g_view_span < minspan) g_view_span = minspan; }
            if (PAD_justPressed(BTN_L2)) { g_view_span *= 2; if (g_view_span > g_au.frames) g_view_span = g_au.frames; }
            // held-direction cursor scrub with acceleration: a tap nudges one
            // fine step; holding ramps the speed up (capped).
            int cdir = PAD_isPressed(BTN_RIGHT) ? 1 : PAD_isPressed(BTN_LEFT) ? -1 : 0;
            if (cdir) {
                edit_accel++;
                long mv = step;
                if (edit_accel > 12) mv = step + (long)(step * (edit_accel - 12) * 0.6f);
                long cap = span / 10; if (cap < 1) cap = 1;
                if (mv > cap) mv = cap;
                if (mv < 1) mv = 1;
                g_curpos += cdir * mv;
            } else edit_accel = 0;
            if (NAV(BTN_L1)) g_curpos -= page;
            if (NAV(BTN_R1)) g_curpos += page;
            if (g_curpos < 0) g_curpos = 0;
            if (g_curpos > g_au.frames) g_curpos = g_au.frames;
            // zero-snap window scaled to the zoom: a few pixels of the visible
            // span, so IN/OUT land on the cursor even when zoomed in (capped at
            // ~10 ms when zoomed out, where a small snap is imperceptible).
            long zwin = span / 256; if (zwin < 1) zwin = 1;
            long zcap = g_au.rate / 100; if (zcap < 1) zcap = 1;
            if (zwin > zcap) zwin = zcap;
            if (PAD_justPressed(BTN_Y)) { g_in = au_snap_zero(&g_au, g_curpos, zwin); if (g_in >= g_out) g_in = g_out > 0 ? g_out - 1 : 0; }
            if (PAD_justPressed(BTN_X)) { g_out = au_snap_zero(&g_au, g_curpos, zwin); if (g_out <= g_in) g_out = g_in + 1; if (g_out > g_au.frames) g_out = g_au.frames; }
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
                if (!strcmp(op, "H3000 FX")) { fx_pick_sel = 0; fx_pick_scroll = 0; stop_play(); mode = M_FX_PICK; back = -1; }
                else if (!strcmp(op, "NORMALIZE")) au_normalize(&g_au);
                else if (!strcmp(op, "GAIN +1 DB")) au_gain_db(&g_au, g_in, g_out, 1.0f);
                else if (!strcmp(op, "GAIN -1 DB")) au_gain_db(&g_au, g_in, g_out, -1.0f);
                else if (!strcmp(op, "FADE IN")) au_fade_in(&g_au, g_in, g_out);
                else if (!strcmp(op, "FADE OUT")) au_fade_out(&g_au, g_in, g_out);
                else if (!strcmp(op, "LOOP XFADE")) au_loop_xfade(&g_au, g_in, g_out);
                else if (!strcmp(op, "HIGH-PASS")) au_highpass(&g_au, 80.0f);
                else if (!strcmp(op, "REVERSE")) au_reverse(&g_au, g_in, g_out);
                else if (!strcmp(op, "TRIM TO SELECTION")) { au_crop(&g_au, g_in, g_out); g_in = 0; g_out = g_au.frames; g_curpos = 0; g_view_span = g_au.frames; }
                else if (!strcmp(op, "TRIM SILENCE")) { long s, e; au_silence_bounds(&g_au, 0.01f, &s, &e); au_crop(&g_au, s, e); g_in = 0; g_out = g_au.frames; g_curpos = 0; g_view_span = g_au.frames; }
                else if (!strcmp(op, "TO MONO")) au_to_mono(&g_au);
                else if (!strcmp(op, "16-BIT")) au_dither16(&g_au);
                else if (!strcmp(op, "HALF RATE")) { au_halve_rate(&g_au); g_in = 0; g_out = g_au.frames; g_curpos = 0; g_view_span = g_au.frames; }
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
        } else if (mode == M_FX_PICK) {
            int mvis = ui_menu_visible_rows(screen);
            if (NAV(BTN_UP))   { if (fx_pick_sel > 0) fx_pick_sel--; }
            if (NAV(BTN_DOWN)) { if (fx_pick_sel < h3k_algo_count - 1) fx_pick_sel++; }
            if (fx_pick_sel < fx_pick_scroll) fx_pick_scroll = fx_pick_sel;
            if (fx_pick_sel >= fx_pick_scroll + mvis) fx_pick_scroll = fx_pick_sel - mvis + 1;
            if (PAD_justPressed(BTN_A)) {                 // choose this algorithm
                fx_algo = fx_pick_sel;
                h3k_defaults(fx_algo, fx_params);
                h_sel = 0;
                mode = M_H3000;
            }
            if (PAD_justPressed(BTN_B)) mode = M_EDIT;
            redraw = 1;
        } else if (mode == M_H3000) {
            const H3kAlgoDef *def = h3k_algos[fx_algo];
            // L1/R1 = previous/next effect without leaving the FX screen (loads
            // that algo's default params; restarts the preview so A/B is instant).
            int algo_d = PAD_justPressed(BTN_R1) ? 1 : PAD_justPressed(BTN_L1) ? -1 : 0;
            if (algo_d) {
                fx_algo = (fx_algo + algo_d + h3k_algo_count) % h3k_algo_count;
                def = h3k_algos[fx_algo];
                h3k_defaults(fx_algo, fx_params);
                h_sel = 0;
                if (g_pv_active) { preview_stop(); preview_start(&g_au, fx_algo, fx_params); }
            }
            if (NAV(BTN_UP))   { if (h_sel > 0) h_sel--; }
            if (NAV(BTN_DOWN)) { if (h_sel < def->nparams - 1) h_sel++; }
            int dl = NAV(BTN_RIGHT) ? 1 : NAV(BTN_LEFT) ? -1 : 0;
            if (dl) fx_params[h_sel] = h3k_adjust(&def->params[h_sel], fx_params[h_sel], dl);
            if (g_pv_active && g_pv_shm)                  // live: tweak heard now
                for (int k = 0; k < H3K_MAX_PARAMS; k++) g_pv_shm->params[k] = fx_params[k];
            if (PAD_justPressed(BTN_A)) {                 // toggle real-time looped preview
                if (g_pv_active) preview_stop();
                else preview_start(&g_au, fx_algo, fx_params);
            }
            if (PAD_justPressed(BTN_START)) {             // RENDER (destructive)
                preview_stop();
                if (h3k_render(&g_au, fx_algo, fx_params) == 0) {
                    g_modified = 1; g_wave_dirty = 1;
                    g_view_span = g_au.frames;
                    if (g_out > g_au.frames) g_out = g_au.frames;
                }
                mode = M_EDIT;
            }
            if (PAD_justPressed(BTN_B)) { preview_stop(); mode = M_FX_PICK; }   // back to picker to switch
            redraw = 1;
        } else if (mode == M_SETTINGS) {
            if (NAV(BTN_UP))   { if (set_sel > 0) set_sel--; }
            if (NAV(BTN_DOWN)) { if (set_sel < 3) set_sel++; }
            int dl = NAV(BTN_RIGHT) ? 1 : NAV(BTN_LEFT) ? -1 : 0;
            int tog = PAD_justPressed(BTN_A);
            if (set_sel == 0 && (dl || tog)) g_set.trim_on_rec = !g_set.trim_on_rec;
            else if (set_sel == 1 && dl) { g_set.trim_db += dl * 6; if (g_set.trim_db < -60) g_set.trim_db = -36; if (g_set.trim_db > -36) g_set.trim_db = -60; }
            else if (set_sel == 2 && (dl || tog)) g_set.autostop = !g_set.autostop;
            else if (set_sel == 3 && dl) { int seq[4] = {2, 3, 5, 10}, idx = 0; for (int i = 0; i < 4; i++) if (seq[i] == g_set.autostop_sec) idx = i; idx = (idx + (dl > 0 ? 1 : 3)) % 4; g_set.autostop_sec = seq[idx]; }
            if (PAD_justPressed(BTN_B)) { save_settings(); mode = M_HOME; redraw = 1; }
            redraw = 1;
        } else { // M_HOME
            if (PAD_justPressed(BTN_A) && in.present) { if (start_rec(&rec, &in)) { g_levelL = 0; g_levelR = 0; lvlctr = 0; last_sound = time(NULL); mode = M_REC; } }
            if (PAD_justPressed(BTN_X)) { snprintf(g_cur, sizeof(g_cur), "%s", g_lib); g_bsel = 0; g_bscroll = 0; list_dir(); mode = M_BROWSE; redraw = 1; }
            if (PAD_justPressed(BTN_SELECT)) { set_sel = 0; mode = M_SETTINGS; redraw = 1; }
            // quit on a fresh B press (justReleased here would catch the release
            // of a B used to leave Settings/Library → app would quit by mistake)
            if (PAD_justPressed(BTN_B) || PAD_justReleased(BTN_MENU)) quitting = 1;
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
                    .blink = ((time(NULL) % 2) == 0), .levelL = g_levelL, .levelR = g_levelR };
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
                                    "START HERE  SEL LGPT  A OPEN  B BACK");
                } else {
                    ui_draw_browser(&ui, screen, crumb, names, isd, g_nent, g_bsel, g_bscroll, g_play_sel,
                                    "A OPEN  Y MENU  X FOLDER  SEL LGPT  B BACK");
                }
            } else if (mode == M_MENU) {
                ui_draw_menu(&ui, screen, "ACTIONS", menu_opts, menu_n, menu_sel, menu_scroll);
            } else if (mode == M_CONFIRM) {
                ui_draw_confirm(&ui, screen, "DELETE", g_nent ? g_ents[g_bsel].name : "");
            } else if (mode == M_EDIT) {
                int cols = ui_editor_cols(screen); if (cols > 2048) cols = 2048;
                long span = g_view_span; if (span > g_au.frames) span = g_au.frames; if (span < 1) span = 1;
                long vs = g_curpos - span / 2;             // view follows cursor
                if (vs > g_au.frames - span) vs = g_au.frames - span;
                if (vs < 0) vs = 0;
                if (g_wave_dirty || cols != g_wave_cols || vs != g_pk_vs || span != g_pk_span) {
                    au_peaks(&g_au, vs, vs + span, cols, g_mn, g_mx);
                    g_wave_cols = cols; g_pk_vs = vs; g_pk_span = span; g_wave_dirty = 0;
                }
                char info[96];
                long secs = g_au.rate ? g_au.frames / g_au.rate : 0;
                snprintf(info, sizeof(info), "%ld:%02ld  %dHZ  %dCH  %dBIT%s",
                         secs / 60, secs % 60, g_au.rate, g_au.ch, g_au.bits, g_modified ? "  *" : "");
                double inf = (double)(g_in - vs) / span; if (inf < 0) inf = 0; if (inf > 1) inf = 1;
                double outf = (double)(g_out - vs) / span; if (outf < 0) outf = 0; if (outf > 1) outf = 1;
                double curf = (double)(g_curpos - vs) / span; if (curf < 0) curf = 0; if (curf > 1) curf = 1;
                double vlo = (double)vs / g_au.frames, vhi = (double)(vs + span) / g_au.frames;
                char pos[48];
                double ct = g_au.rate ? (double)g_curpos / g_au.rate : 0;
                int cm = (int)ct / 60; double cssec = ct - cm * 60;
                snprintf(pos, sizeof(pos), "CURSOR  %d:%06.3f", cm, cssec);
                const char *nm = strrchr(g_edit_path, '/'); nm = nm ? nm + 1 : g_edit_path;
                ui_draw_editor(&ui, screen, nm, info, g_mn, g_mx, cols, inf, outf, curf,
                               g_play_pid > 0, vlo, vhi, pos);
            } else if (mode == M_EMENU) {
                ui_draw_menu(&ui, screen, "EDIT", EMENU, EMENU_N, emenu_sel, emenu_scroll);
            } else if (mode == M_EDIT_EXIT) {
                ui_draw_confirm(&ui, screen, "DISCARD?", "UNSAVED CHANGES");
            } else if (mode == M_SETTINGS) {
                char v0[8], v1[16], v2[8], v3[16];
                snprintf(v0, sizeof(v0), "%s", g_set.trim_on_rec ? "ON" : "OFF");
                snprintf(v1, sizeof(v1), "%d DB", g_set.trim_db);
                snprintf(v2, sizeof(v2), "%s", g_set.autostop ? "ON" : "OFF");
                snprintf(v3, sizeof(v3), "%d S", g_set.autostop_sec);
                const char *slabels[4] = {"TRIM SILENCE ON REC", "SILENCE THRESHOLD",
                                          "AUTO-STOP ON SILENCE", "AUTO-STOP AFTER"};
                const char *svalues[4] = {v0, v1, v2, v3};
                ui_draw_settings(&ui, screen, "SETTINGS", slabels, svalues, 4, set_sel);
            } else if (mode == M_FX_PICK) {
                const char *names[32];
                int nn = h3k_algo_count; if (nn > 32) nn = 32;
                for (int i = 0; i < nn; i++) names[i] = h3k_algos[i]->name;
                ui_draw_menu(&ui, screen, "H3000 FX", names, nn, fx_pick_sel, fx_pick_scroll);
            } else if (mode == M_H3000) {
                const H3kAlgoDef *def = h3k_algos[fx_algo];
                char vbuf[H3K_MAX_PARAMS][24];
                const char *hl[H3K_MAX_PARAMS];
                const char *hv[H3K_MAX_PARAMS];
                for (int i = 0; i < def->nparams; i++) {
                    hl[i] = def->params[i].label;
                    h3k_format(&def->params[i], fx_params[i], vbuf[i], sizeof(vbuf[i]));
                    hv[i] = vbuf[i];
                }
                ui_draw_fx(&ui, screen, def->name, hl, hv, def->nparams, h_sel, g_pv_active);
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

    preview_stop();
    stop_play();
    if (rec.active) stop_rec(&rec);
    ui_free(&ui);
    QuitSettings();
    PWR_quit();
    PAD_quit();
    GFX_quit();
    return 0;
}
