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
    ui->wordmark = openf(dir, "dotmatrix.ttf", 34);
    ui->seg_big  = openf(dir, "seg.ttf", 96);
    ui->seg_mid  = openf(dir, "seg.ttf", 34);
    ui->hero     = openf(dir, "label.ttf", 50);
    ui->h1       = openf(dir, "label.ttf", 22);
    ui->label    = openf(dir, "label.ttf", 15);
    ui->mono     = openf(dir, "mono.ttf", 16);
    ui->mono_sm  = openf(dir, "mono.ttf", 12);
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
    draw_text(s, ui->wordmark, "M8TAPE", C_WHITE, MARGIN, 22);
    draw_label(s, ui->label, mode, C_GREY, MARGIN + 250, 34, 3);
    if (right) draw_text_r(s, ui->mono, right, C_GREY, s->w - MARGIN, 32);
    fillc(s, MARGIN, 78, s->w - 2 * MARGIN, 1, C_HAIR);
}

static void status_dot(UI *ui, SDL_Surface *s, int x, int y, const char *lbl, int on) {
    disc(s, x + 5, y + 8, 5, on ? C_WHITE : C_HAIR);
    draw_label(s, ui->label, lbl, on ? C_GREY : C_HAIR, x + 18, y, 2);
}

void ui_draw_home(UI *ui, SDL_Surface *s, const UIInput *in) {
    fillc(s, 0, 0, s->w, s->h, C_BG);
    header(ui, s, "STUDIO", NULL);

    int x = MARGIN;
    int y = 150;
    draw_label(s, ui->label, "INPUT SOURCE", C_GREY, x, y, 4);

    const char *name = in->present ? in->source : "NO INPUT";
    int w = draw_text(s, ui->hero, name, in->present ? C_WHITE : C_GREY, x, y + 28);
    if (in->present) disc(s, x + w + 26, y + 28 + 34, 10, C_RED);

    char spec[128];
    if (in->present) {
        snprintf(spec, sizeof(spec), "%d CH    %d HZ    %s",
                 in->channels, in->rate, in->format ? in->format : "?");
        draw_text(s, ui->mono, spec, C_GREY, x, y + 104);
    } else {
        draw_text(s, ui->mono, "CONNECT A USB AUDIO DEVICE", C_GREY, x, y + 104);
    }

    // status dots row
    int sy = y + 150;
    status_dot(ui, s, x,       sy, "USB",  in->present);
    status_dot(ui, s, x + 130, sy, "SYNC", in->present);
    status_dot(ui, s, x + 270, sy, "CLIP", 0);
    status_dot(ui, s, x + 400, sy, "SD",   1);

    // record affordance: big ring, centered low
    int cx = s->w / 2, cy = s->h - 200;
    SDL_Color ring_c = in->present ? C_RED : C_HAIR;
    ring(s, cx, cy, 64, 58, ring_c);
    disc(s, cx, cy, 30, in->present ? C_RED : C_PANEL);
    draw_label(s, ui->label, "RECORD", in->present ? C_WHITE : C_GREY,
               cx - 28, cy + 84, 4);

    // footer
    fillc(s, MARGIN, s->h - 70, s->w - 2 * MARGIN, 1, C_HAIR);
    draw_text_r(s, ui->mono_sm, in->present ? "A  REC      B  QUIT" : "B  QUIT",
                C_GREY, s->w - MARGIN, s->h - 50);
}

void ui_draw_record(UI *ui, SDL_Surface *s, const UIInput *in, const UIRec *r) {
    fillc(s, 0, 0, s->w, s->h, C_BG);

    // header: blinking red dot + REC, source on the right
    if (r->blink) disc(s, MARGIN + 10, 36, 11, C_RED);
    draw_text(s, ui->h1, "REC", C_WHITE, MARGIN + 32, 24);
    char hr[96];
    snprintf(hr, sizeof(hr), "%s   %d/%s", in->source ? in->source : "",
             in->rate, in->format ? in->format : "");
    draw_text_r(s, ui->mono, hr, C_GREY, s->w - MARGIN, 32);
    fillc(s, MARGIN, 78, s->w - 2 * MARGIN, 1, C_HAIR);

    // hero timecode (DSEG7) with ghost segments behind
    long e = r->elapsed < 0 ? 0 : r->elapsed;
    char tc[16];
    snprintf(tc, sizeof(tc), "%02ld:%02ld:%02ld", e / 3600, (e % 3600) / 60, e % 60);
    if (ui->seg_big) {
        draw_text_c(s, ui->seg_big, "88:88:88", (SDL_Color){60, 29, 6, 255}, s->w / 2, 110);
        draw_text_c(s, ui->seg_big, tc, C_AMBER, s->w / 2, 110);
    } else {
        draw_text_c(s, ui->hero, tc, C_AMBER, s->w / 2, 110);
    }

    // tape machine
    int midY = s->h / 2 + 70;
    int R = 116;
    int lx = s->w / 2 - 200, rx = s->w / 2 + 200;
    // ribbon
    fillc(s, lx, midY - 22, rx - lx, 44, C_PANEL);
    fillc(s, lx, midY - 22, rx - lx, 1, C_HAIR);
    fillc(s, lx, midY + 22, rx - lx, 1, C_HAIR);
    // scrolling take line on the ribbon (red dashes moving with angle)
    int off = (int)(r->angle / (2 * M_PI) * 40.0);
    for (int dx = -((off) % 40); dx < rx - lx; dx += 40)
        if (lx + dx >= lx && lx + dx < rx - 16)
            fillc(s, lx + dx, midY - 2, 22, 4, C_RED);
    draw_reel(s, lx, midY, R, r->angle);
    draw_reel(s, rx, midY, R, r->angle);
    // fixed center playhead
    fillc(s, s->w / 2 - 1, midY - 40, 2, 80, C_WHITE);
    line_thick(s, s->w / 2 - 7, midY - 46, s->w / 2 + 7, midY - 46, 2, C_WHITE);

    // footer transport
    fillc(s, MARGIN, s->h - 70, s->w - 2 * MARGIN, 1, C_HAIR);
    if (r->take) {
        char lt[160];
        snprintf(lt, sizeof(lt), "REC  %s", r->take);
        draw_text(s, ui->mono_sm, lt, C_GREY, MARGIN, s->h - 50);
    }
    draw_text_r(s, ui->mono_sm, "A  STOP", C_WHITE, s->w - MARGIN, s->h - 50);
}
