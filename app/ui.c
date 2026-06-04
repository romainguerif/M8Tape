// ui.c - pure SDL2/SDL_ttf rendering for M8Tape Studio (TE x Nothing).
#include "ui.h"
#include <math.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---- low-level helpers -----------------------------------------------------
static void fillc(SDL_Surface *s, int x, int y, int w, int h, SDL_Color c) {
    SDL_Rect r = {x, y, w, h};
    SDL_FillRect(s, &r, SDL_MapRGB(s->format, c.r, c.g, c.b));
}

static int draw_text(SDL_Surface *s, TTF_Font *f, const char *str, SDL_Color c, int x, int y) {
    if (!f || !str || !*str) return 0;
    SDL_Surface *t = TTF_RenderUTF8_Blended(f, str, c);
    if (!t) return 0;
    SDL_Rect r = {x, y, t->w, t->h};
    SDL_BlitSurface(t, NULL, s, &r);
    int w = t->w;
    SDL_FreeSurface(t);
    return w;
}

static void draw_text_c(SDL_Surface *s, TTF_Font *f, const char *str, SDL_Color c, int cx, int y) {
    if (!f || !str || !*str) return;
    SDL_Surface *t = TTF_RenderUTF8_Blended(f, str, c);
    if (!t) return;
    SDL_Rect r = {cx - t->w / 2, y, t->w, t->h};
    SDL_BlitSurface(t, NULL, s, &r);
    SDL_FreeSurface(t);
}

static void draw_text_r(SDL_Surface *s, TTF_Font *f, const char *str, SDL_Color c, int rx, int y) {
    if (!f || !str || !*str) return;
    SDL_Surface *t = TTF_RenderUTF8_Blended(f, str, c);
    if (!t) return;
    SDL_Rect r = {rx - t->w, y, t->w, t->h};
    SDL_BlitSurface(t, NULL, s, &r);
    SDL_FreeSurface(t);
}

// uppercase label with letter-spacing (the TE/Nothing label treatment).
static int draw_label(SDL_Surface *s, TTF_Font *f, const char *str, SDL_Color c,
                      int x, int y, int tracking) {
    if (!f || !str) return 0;
    int cx = x;
    for (const char *p = str; *p; p++) {
        char ch[2] = {(char)toupper((unsigned char)*p), 0};
        if (*p == ' ') { cx += tracking * 2; continue; }
        SDL_Surface *t = TTF_RenderUTF8_Blended(f, ch, c);
        if (t) {
            SDL_Rect r = {cx, y, t->w, t->h};
            SDL_BlitSurface(t, NULL, s, &r);
            cx += t->w + tracking;
            SDL_FreeSurface(t);
        }
    }
    return cx - x;
}

// filled disc (per-row spans, no sqrt in inner loop)
static void disc(SDL_Surface *s, int cx, int cy, int rad, SDL_Color c) {
    for (int dy = -rad; dy <= rad; dy++) {
        int dx = (int)(sqrt((double)(rad * rad - dy * dy)) + 0.5);
        fillc(s, cx - dx, cy + dy, 2 * dx + 1, 1, c);
    }
}

// annulus (ring) between r_in and r_out
static void ring(SDL_Surface *s, int cx, int cy, int r_out, int r_in, SDL_Color c) {
    for (int dy = -r_out; dy <= r_out; dy++) {
        int xo = (int)(sqrt((double)(r_out * r_out - dy * dy)) + 0.5);
        int inside = r_in * r_in - dy * dy;
        if (inside > 0) {
            int xi = (int)(sqrt((double)inside) + 0.5);
            fillc(s, cx - xo, cy + dy, xo - xi, 1, c);     // left band
            fillc(s, cx + xi, cy + dy, xo - xi, 1, c);     // right band
        } else {
            fillc(s, cx - xo, cy + dy, 2 * xo + 1, 1, c);  // full row
        }
    }
}

// thick line via square stamps
static void line_thick(SDL_Surface *s, int x0, int y0, int x1, int y1, int w, SDL_Color c) {
    int dx = abs(x1 - x0), dy = abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    int h = w / 2;
    for (;;) {
        fillc(s, x0 - h, y0 - h, w, w, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

// a tape reel: concentric rings + hub + 3 spokes (one bright → motion)
static void draw_reel(SDL_Surface *s, int cx, int cy, int R, double angle) {
    ring(s, cx, cy, R, R - 4, C_GREY);                 // outer rim
    ring(s, cx, cy, (int)(R * 0.70), (int)(R * 0.70) - 2, C_HAIR);
    ring(s, cx, cy, (int)(R * 0.44), (int)(R * 0.44) - 2, C_HAIR);
    for (int i = 0; i < 3; i++) {
        double a = angle + i * (2.0 * M_PI / 3.0);
        int x = cx + (int)((R - 8) * cos(a));
        int y = cy + (int)((R - 8) * sin(a));
        line_thick(s, cx, cy, x, y, 5, i == 0 ? C_WHITE : C_GREY);
    }
    disc(s, cx, cy, (int)(R * 0.16), C_GREY);
    disc(s, cx, cy, 5, C_WHITE);
}

// ---- fonts -----------------------------------------------------------------
static TTF_Font *openf(const char *dir, const char *file, int pt) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    return TTF_OpenFont(path, pt);
}

int ui_load_fonts(UI *ui, const char *dir) {
    memset(ui, 0, sizeof(*ui));
    // Nothing-style dot-matrix font for EVERYTHING (scaled up for the tiny,
    // high-density 3.2" 1024x768 panel).
    ui->wordmark = openf(dir, "dotmatrix.ttf", 64);
    ui->seg_big  = openf(dir, "dotmatrix.ttf", 172); // hero timecode
    ui->seg_mid  = openf(dir, "dotmatrix.ttf", 64);
    ui->hero     = openf(dir, "dotmatrix.ttf", 92);  // source name
    ui->h1       = openf(dir, "dotmatrix.ttf", 64);
    ui->label    = openf(dir, "dotmatrix.ttf", 40);
    ui->mono     = openf(dir, "dotmatrix.ttf", 44);
    ui->mono_sm  = openf(dir, "dotmatrix.ttf", 34);
    if (!ui->wordmark || !ui->seg_big || !ui->hero || !ui->h1 ||
        !ui->label || !ui->mono || !ui->mono_sm)
        return 1;
    return 0;
}

void ui_free(UI *ui) {
    TTF_Font *fs[] = {ui->wordmark, ui->seg_big, ui->seg_mid, ui->hero,
                      ui->h1, ui->label, ui->mono, ui->mono_sm};
    for (int i = 0; i < 8; i++) if (fs[i]) TTF_CloseFont(fs[i]);
    memset(ui, 0, sizeof(*ui));
}

// ---- screens ---------------------------------------------------------------
#define MARGIN 32

static void header(UI *ui, SDL_Surface *s, const char *mode, const char *right) {
    int w = draw_text(s, ui->wordmark, "M8TAPE", C_WHITE, MARGIN, 24);
    draw_label(s, ui->label, mode, C_GREY, MARGIN + w + 30, 48, 2);
    if (right) draw_text_r(s, ui->mono, right, C_GREY, s->w - MARGIN, 40);
    fillc(s, MARGIN, 110, s->w - 2 * MARGIN, 2, C_HAIR);
}

static void status_dot(UI *ui, SDL_Surface *s, int x, int y, const char *lbl, int on) {
    disc(s, x + 7, y + 14, 7, on ? C_WHITE : C_HAIR);
    draw_label(s, ui->label, lbl, on ? C_GREY : C_HAIR, x + 28, y, 2);
}

void ui_draw_home(UI *ui, SDL_Surface *s, const UIInput *in) {
    fillc(s, 0, 0, s->w, s->h, C_BG);
    header(ui, s, "STUDIO", NULL);

    int x = MARGIN;
    int y = 188;
    draw_label(s, ui->label, "INPUT SOURCE", C_GREY, x, y, 3);

    const char *name = in->present ? in->source : "NO INPUT";
    int w = draw_text(s, ui->hero, name, in->present ? C_WHITE : C_GREY, x, y + 44);
    if (in->present) disc(s, x + w + 36, y + 44 + 52, 15, C_RED);

    char spec[128];
    if (in->present) {
        snprintf(spec, sizeof(spec), "%d CH   %d HZ   %s",
                 in->channels, in->rate, in->format ? in->format : "?");
        draw_text(s, ui->mono, spec, C_GREY, x, y + 156);
    } else {
        draw_text(s, ui->mono, "CONNECT A USB AUDIO DEVICE", C_GREY, x, y + 156);
    }

    // status dots row
    int sy = y + 232;
    status_dot(ui, s, x,       sy, "USB",  in->present);
    status_dot(ui, s, x + 200, sy, "SYNC", in->present);
    status_dot(ui, s, x + 400, sy, "CLIP", 0);
    status_dot(ui, s, x + 580, sy, "SD",   1);

    // record affordance: big ring, raised so its label clears the footer
    int cx = s->w / 2, cy = s->h - 244;
    SDL_Color ring_c = in->present ? C_RED : C_HAIR;
    ring(s, cx, cy, 90, 80, ring_c);
    disc(s, cx, cy, 44, in->present ? C_RED : C_PANEL);
    draw_label(s, ui->label, "RECORD", in->present ? C_WHITE : C_GREY,
               cx - 58, cy + 104, 3);

    // footer (abbreviated to avoid overlapping the RECORD label)
    fillc(s, MARGIN, s->h - 92, s->w - 2 * MARGIN, 2, C_HAIR);
    draw_text_r(s, ui->mono_sm, in->present ? "A REC   X LIB   SEL SET   B QUIT" : "X LIB   SEL SET   B QUIT",
                C_GREY, s->w - MARGIN, s->h - 64);
}

// --- library browser / menus ------------------------------------------------
#define BR_TOP 150
#define BR_ROWH 78

int ui_browser_visible_rows(SDL_Surface *s) {
    int avail = (s->h - 100) - BR_TOP;
    int n = avail / BR_ROWH;
    return n < 1 ? 1 : n;
}

void ui_draw_browser(UI *ui, SDL_Surface *s, const char *crumb,
                     const char **names, const int *isdir, int count,
                     int sel, int scroll, int play_idx, const char *hints) {
    fillc(s, 0, 0, s->w, s->h, C_BG);
    draw_label(s, ui->label, crumb ? crumb : "LIBRARY", C_GREY, MARGIN, 44, 2);
    fillc(s, MARGIN, 112, s->w - 2 * MARGIN, 2, C_HAIR);

    int vis = ui_browser_visible_rows(s);
    if (count == 0) {
        draw_text_c(s, ui->mono, "EMPTY", C_HAIR, s->w / 2, s->h / 2 - 30);
    }
    for (int i = 0; i < vis; i++) {
        int idx = scroll + i;
        if (idx >= count) break;
        int y = BR_TOP + i * BR_ROWH;
        int seld = (idx == sel);
        if (seld) {
            fillc(s, MARGIN, y, s->w - 2 * MARGIN, BR_ROWH - 10, C_PANEL);
            fillc(s, MARGIN, y, 7, BR_ROWH - 10, C_RED);
        }
        // type marker
        const char *mark = isdir[idx] ? "[ ]" : ">";
        draw_text(s, ui->mono_sm, mark, isdir[idx] ? C_AMBER : C_GREY, MARGIN + 24, y + 16);
        draw_text(s, ui->mono, names[idx], C_WHITE, MARGIN + 110, y + 8);
        if (idx == play_idx)
            draw_text_r(s, ui->mono_sm, "PLAYING", C_RED, s->w - MARGIN - 24, y + 16);
        else if (isdir[idx])
            draw_text_r(s, ui->mono_sm, "DIR", C_HAIR, s->w - MARGIN - 24, y + 16);
    }
    fillc(s, MARGIN, s->h - 88, s->w - 2 * MARGIN, 2, C_HAIR);
    draw_text_r(s, ui->mono_sm, hints ? hints : "", C_GREY, s->w - MARGIN, s->h - 62);
}

#define MENU_TOP 180
#define MENU_ROWH 78
int ui_menu_visible_rows(SDL_Surface *s) {
    int n = ((s->h - 90) - MENU_TOP) / MENU_ROWH;
    return n < 1 ? 1 : n;
}
void ui_draw_menu(UI *ui, SDL_Surface *s, const char *title,
                  const char **opts, int count, int sel, int scroll) {
    fillc(s, 0, 0, s->w, s->h, C_BG);
    draw_label(s, ui->label, title ? title : "ACTIONS", C_GREY, MARGIN, 44, 2);
    fillc(s, MARGIN, 112, s->w - 2 * MARGIN, 2, C_HAIR);
    int vis = ui_menu_visible_rows(s);
    for (int i = 0; i < vis; i++) {
        int idx = scroll + i;
        if (idx >= count) break;
        int y = MENU_TOP + i * MENU_ROWH;
        if (idx == sel) {
            fillc(s, MARGIN, y, s->w - 2 * MARGIN, MENU_ROWH - 12, C_PANEL);
            fillc(s, MARGIN, y, 7, MENU_ROWH - 12, C_RED);
        }
        draw_text(s, ui->h1, opts[idx], idx == sel ? C_WHITE : C_GREY, MARGIN + 40, y + 6);
    }
    draw_text_r(s, ui->mono_sm, "A SELECT   B BACK", C_GREY, s->w - MARGIN, s->h - 62);
}

#define WF_TOP 168
#define WF_BOT 540
int ui_editor_cols(SDL_Surface *s) { return s->w - 2 * MARGIN; }

void ui_draw_editor(UI *ui, SDL_Surface *s, const char *name, const char *info,
                    const float *mn, const float *mx, int cols,
                    double in_frac, double out_frac, double cur_frac, int playing,
                    double view_lo, double view_hi, const char *pos) {
    fillc(s, 0, 0, s->w, s->h, C_BG);
    draw_text(s, ui->h1, name ? name : "", C_WHITE, MARGIN, 24);
    if (info) draw_text_r(s, ui->mono_sm, info, C_GREY, s->w - MARGIN, 40);
    fillc(s, MARGIN, 108, s->w - 2 * MARGIN, 2, C_HAIR);

    int x0 = MARGIN, w = cols;
    int mid = (WF_TOP + WF_BOT) / 2, half = (WF_BOT - WF_TOP) / 2 - 6;
    int in_x  = x0 + (int)(in_frac  * w);
    int out_x = x0 + (int)(out_frac * w);

    fillc(s, in_x, WF_TOP, out_x - in_x, WF_BOT - WF_TOP, C_PANEL);  // selection band
    fillc(s, x0, mid, w, 1, C_HAIR);                                  // center line
    for (int i = 0; i < w; i++) {
        int x = x0 + i;
        int top = mid - (int)(mx[i] * half);
        int bot = mid - (int)(mn[i] * half);
        if (bot < top) { int t = top; top = bot; bot = t; }
        if (bot == top) bot = top + 1;
        SDL_Color c = (x >= in_x && x <= out_x) ? C_WHITE : C_GREY;
        fillc(s, x, top, 1, bot - top, c);
    }
    fillc(s, in_x, WF_TOP, 2, WF_BOT - WF_TOP, C_AMBER);
    fillc(s, out_x, WF_TOP, 2, WF_BOT - WF_TOP, C_AMBER);
    int cur_x = x0 + (int)(cur_frac * w);
    fillc(s, cur_x, WF_TOP - 8, 3, WF_BOT - WF_TOP + 16, playing ? C_RED : C_WHITE);

    // overview bar (whole file) with the current view window highlighted
    int oy = WF_BOT + 26, oh = 12;
    fillc(s, x0, oy, w, oh, C_PANEL);
    int vlo = x0 + (int)(view_lo * w), vhi = x0 + (int)(view_hi * w);
    if (vhi - vlo < 3) vhi = vlo + 3;
    fillc(s, vlo, oy, vhi - vlo, oh, C_AMBER);
    if (pos) draw_text_c(s, ui->mono_sm, pos, C_GREY, s->w / 2, oy + 26);

    // footer
    fillc(s, MARGIN, s->h - 92, s->w - 2 * MARGIN, 2, C_HAIR);
    draw_text(s, ui->mono_sm, "<>MOVE  L2/R2 ZOOM  Y IN  X OUT", C_GREY, MARGIN, s->h - 64);
    draw_text_r(s, ui->mono_sm, "A PLAY  START EDIT  B EXIT", C_GREY, s->w - MARGIN, s->h - 64);
}

void ui_draw_confirm(UI *ui, SDL_Surface *s, const char *l1, const char *l2) {
    fillc(s, 0, 0, s->w, s->h, C_BG);
    draw_text_c(s, ui->hero, l1 ? l1 : "", C_RED, s->w / 2, s->h / 2 - 110);
    draw_text_c(s, ui->h1, l2 ? l2 : "", C_WHITE, s->w / 2, s->h / 2 - 10);
    draw_text_c(s, ui->mono, "A  YES        B  NO", C_GREY, s->w / 2, s->h / 2 + 90);
}

void ui_draw_fx(UI *ui, SDL_Surface *s, const char *algo,
                const char **labels, const char **values, int count, int sel, int playing) {
    fillc(s, 0, 0, s->w, s->h, C_BG);
    draw_label(s, ui->label, "H3000", C_GREY, MARGIN, 30, 3);
    int aw = draw_text(s, ui->h1, algo ? algo : "", C_WHITE, MARGIN + 180, 22);
    if (playing) {
        disc(s, MARGIN + 180 + aw + 28, 44, 7, C_RED);
        draw_label(s, ui->label, "PREVIEW", C_RED, MARGIN + 180 + aw + 52, 30, 2);
    }
    fillc(s, MARGIN, 96, s->w - 2 * MARGIN, 2, C_HAIR);

    int top = 130;
    int rowh = (s->h - 92 - top) / (count > 0 ? count : 1);   // fit above footer
    if (rowh > 84) rowh = 84;
    for (int i = 0; i < count; i++) {
        int y = top + i * rowh;
        if (i == sel) {
            fillc(s, MARGIN, y, s->w - 2 * MARGIN, rowh - 14, C_PANEL);
            fillc(s, MARGIN, y, 7, rowh - 14, C_RED);
        }
        draw_text(s, ui->h1, labels[i], i == sel ? C_WHITE : C_GREY, MARGIN + 40, y + 6);
        draw_text_r(s, ui->h1, values[i], i == sel ? C_AMBER : C_GREY, s->w - MARGIN - 40, y + 6);
    }
    fillc(s, MARGIN, s->h - 92, s->w - 2 * MARGIN, 2, C_HAIR);
    draw_text(s, ui->mono_sm, "<> ADJUST   A PREVIEW", C_GREY, MARGIN, s->h - 64);
    draw_text_r(s, ui->mono_sm, "START RENDER   B BACK", C_GREY, s->w - MARGIN, s->h - 64);
}

void ui_draw_settings(UI *ui, SDL_Surface *s, const char *title,
                      const char **labels, const char **values, int count, int sel) {
    fillc(s, 0, 0, s->w, s->h, C_BG);
    draw_label(s, ui->label, title ? title : "SETTINGS", C_GREY, MARGIN, 44, 2);
    fillc(s, MARGIN, 112, s->w - 2 * MARGIN, 2, C_HAIR);
    int top = 180, rowh = 96;
    for (int i = 0; i < count; i++) {
        int y = top + i * rowh;
        if (i == sel) {
            fillc(s, MARGIN, y, s->w - 2 * MARGIN, rowh - 14, C_PANEL);
            fillc(s, MARGIN, y, 7, rowh - 14, C_RED);
        }
        draw_text(s, ui->h1, labels[i], i == sel ? C_WHITE : C_GREY, MARGIN + 40, y + 8);
        draw_text_r(s, ui->h1, values[i], i == sel ? C_AMBER : C_GREY, s->w - MARGIN - 40, y + 8);
    }
    draw_text_r(s, ui->mono_sm, "<> CHANGE   A TOGGLE   B SAVE", C_GREY, s->w - MARGIN, s->h - 62);
}

// --- on-screen keyboard -----------------------------------------------------
const char *kb_rows[KB_CHAR_ROWS] = {
    "1234567890",
    "QWERTYUIOP",
    "ASDFGHJKL_",
    "ZXCVBNM-.,",
};
const char *kb_actions[KB_ACTIONS] = {"CAPS", "DEL", "OK"};

static void cell(SDL_Surface *s, TTF_Font *f, const char *txt, int cx, int cy,
                 int w, int h, int selected, int accent) {
    if (selected) {
        fillc(s, cx - w / 2, cy - h / 2, w, h, accent ? C_RED : C_WHITE);
    } else {
        // thin outline cell
        fillc(s, cx - w / 2, cy - h / 2, w, 1, C_HAIR);
        fillc(s, cx - w / 2, cy + h / 2, w, 1, C_HAIR);
        fillc(s, cx - w / 2, cy - h / 2, 1, h, C_HAIR);
        fillc(s, cx + w / 2, cy - h / 2, 1, h, C_HAIR);
    }
    SDL_Color tc = selected ? C_BG : C_WHITE;
    draw_text_c(s, f, txt, tc, cx, cy - 20);
}

void ui_draw_keyboard(UI *ui, SDL_Surface *s, const char *title,
                      const char *text, int caps, int row, int col) {
    fillc(s, 0, 0, s->w, s->h, C_BG);

    // title
    draw_label(s, ui->label, title ? title : "NAME", C_GREY, MARGIN, 40, 3);
    fillc(s, MARGIN, 92, s->w - 2 * MARGIN, 2, C_HAIR);

    // text field with blinking-less cursor block
    int tx = draw_text(s, ui->h1, (text && *text) ? text : "", C_WHITE, MARGIN, 128);
    fillc(s, MARGIN + tx + 6, 138, 22, 44, C_RED); // cursor

    // char grid
    int gridW = s->w - 2 * MARGIN;
    int cw = gridW / KB_COLS;
    int ch = 70;
    int top = 280;
    for (int r = 0; r < KB_CHAR_ROWS; r++) {
        for (int c = 0; c < KB_COLS; c++) {
            char k = kb_rows[r][c];
            char buf[2] = { caps ? k : (char)tolower((unsigned char)k), 0 };
            int cx = MARGIN + c * cw + cw / 2;
            int cy = top + r * (ch + 8) + ch / 2;
            cell(s, ui->mono, buf, cx, cy, cw - 8, ch, (row == r && col == c), 1);
        }
    }

    // action row
    int ay = top + KB_CHAR_ROWS * (ch + 8) + 20;
    int aw = gridW / KB_ACTIONS;
    for (int a = 0; a < KB_ACTIONS; a++) {
        int cx = MARGIN + a * aw + aw / 2;
        int sel = (row == KB_CHAR_ROWS && col == a);
        const char *lbl = kb_actions[a];
        char tmp[16];
        if (a == 0) { snprintf(tmp, sizeof(tmp), "CAPS %s", caps ? "ON" : "OFF"); lbl = tmp; }
        cell(s, ui->mono_sm, lbl, cx, ay + ch / 2, aw - 12, ch, sel, 0);
    }

    // hints
    draw_text_r(s, ui->mono_sm, "A TYPE   B DEL   Y _   START OK", C_GREY,
                s->w - MARGIN, s->h - 56);
}

void ui_draw_record(UI *ui, SDL_Surface *s, const UIInput *in, const UIRec *r) {
    fillc(s, 0, 0, s->w, s->h, C_BG);

    // header: blinking red dot + REC, source on the right
    if (r->blink) disc(s, MARGIN + 16, 54, 15, C_RED);
    draw_text(s, ui->h1, "REC", C_WHITE, MARGIN + 48, 24);
    char hr[96];
    snprintf(hr, sizeof(hr), "%s  %d %s", in->source ? in->source : "",
             in->rate, in->format ? in->format : "");
    draw_text_r(s, ui->mono_sm, hr, C_GREY, s->w - MARGIN, 46);
    fillc(s, MARGIN, 110, s->w - 2 * MARGIN, 2, C_HAIR);

    // hero timecode (big dot-matrix)
    long e = r->elapsed < 0 ? 0 : r->elapsed;
    char tc[16];
    snprintf(tc, sizeof(tc), "%02ld:%02ld:%02ld", e / 3600, (e % 3600) / 60, e % 60);
    draw_text_c(s, ui->seg_big, tc, C_AMBER, s->w / 2, 150);

    // tape machine
    int midY = s->h / 2 + 110;
    int R = 132;
    int lx = s->w / 2 - 230, rx = s->w / 2 + 230;
    fillc(s, lx, midY - 26, rx - lx, 52, C_PANEL);
    fillc(s, lx, midY - 26, rx - lx, 2, C_HAIR);
    fillc(s, lx, midY + 24, rx - lx, 2, C_HAIR);
    // scrolling take line (red dashes moving with angle)
    int off = (int)(r->angle / (2 * M_PI) * 48.0);
    for (int dx = -((off) % 48); dx < rx - lx; dx += 48)
        if (lx + dx >= lx && lx + dx < rx - 20)
            fillc(s, lx + dx, midY - 3, 26, 6, C_RED);
    draw_reel(s, lx, midY, R, r->angle);
    draw_reel(s, rx, midY, R, r->angle);
    // fixed center playhead
    fillc(s, s->w / 2 - 1, midY - 48, 3, 96, C_WHITE);

    // level meter (L/R) under the tape
    if (r->levelL >= 0 || r->levelR >= 0) {
        int mw = 600, mx = (s->w - mw) / 2, my = midY + R + 30;
        float lv[2] = {r->levelL, r->levelR};
        for (int k = 0; k < 2; k++) {
            int y = my + k * 24;
            float v = lv[k] < 0 ? 0 : lv[k] > 1 ? 1 : lv[k];
            int fillw = (int)(mw * v);
            int clipw = (int)(mw * 0.85f);
            fillc(s, mx, y, mw, 14, C_PANEL);
            SDL_Color c = (fillw > clipw) ? C_RED : C_WHITE;
            fillc(s, mx, y, fillw, 14, c);
            draw_text(s, ui->mono_sm, k ? "R" : "L", C_GREY, mx - 28, y - 8);
        }
    }

    // footer transport
    fillc(s, MARGIN, s->h - 92, s->w - 2 * MARGIN, 2, C_HAIR);
    if (r->take) {
        char lt[160];
        snprintf(lt, sizeof(lt), "REC  %s", r->take);
        draw_text(s, ui->mono_sm, lt, C_GREY, MARGIN, s->h - 64);
    }
    draw_text_r(s, ui->mono_sm, "A STOP", C_WHITE, s->w - MARGIN, s->h - 64);
}
