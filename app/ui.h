// ui.h - pure SDL2/SDL_ttf rendering for M8Tape Studio.
// No MinUI / device dependencies, so the exact same drawing runs both on the
// Brick and in the Mac preview harness (preview.c) for fast design iteration.
#ifndef UI_H
#define UI_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

// TE x Nothing palette
#define C_BG     ((SDL_Color){10, 10, 10, 255})    // #0A0A0A matte black
#define C_PANEL  ((SDL_Color){22, 22, 22, 255})    // #161616
#define C_HAIR   ((SDL_Color){42, 42, 42, 255})    // #2A2A2A hairline
#define C_GREY   ((SDL_Color){138, 138, 138, 255}) // #8A8A8A labels
#define C_WHITE  ((SDL_Color){242, 242, 240, 255}) // #F2F2F0 aluminium white
#define C_RED    ((SDL_Color){229, 51, 42, 255})   // #E5332A record/accent
#define C_AMBER  ((SDL_Color){255, 122, 26, 255})  // #FF7A1A segmented readout

typedef struct {
    TTF_Font *wordmark;  // dot-matrix, headline/wordmark
    TTF_Font *seg_big;   // DSEG7, hero timecode
    TTF_Font *seg_mid;   // DSEG7, smaller readouts
    TTF_Font *hero;      // Archivo, big source name
    TTF_Font *h1;        // Archivo, header mode name
    TTF_Font *label;     // Archivo, small uppercase labels
    TTF_Font *mono;      // Space Mono, tabular values
    TTF_Font *mono_sm;   // Space Mono, small
    SDL_Surface *reel;   // cached reel rings (built lazily)
} UI;

typedef struct {
    int         present;
    const char *source;   // e.g. "DIRTYWAVE M8"
    int         channels;
    int         rate;
    const char *format;   // e.g. "S24_3LE"
} UIInput;

typedef struct {
    long        elapsed;  // seconds
    const char *take;     // filename
    double      angle;    // reel rotation (radians)
    int         blink;    // record dot on/off
    float       levelL, levelR; // 0..1 (optional; <0 = unknown)
} UIRec;

// load every font instance from resdir (dir containing the .ttf files).
// returns 0 on success, non-zero if a font is missing.
int  ui_load_fonts(UI *ui, const char *resdir);
void ui_free(UI *ui);

void ui_draw_home(UI *ui, SDL_Surface *s, const UIInput *in);
void ui_draw_record(UI *ui, SDL_Surface *s, const UIInput *in, const UIRec *r);

// --- on-screen keyboard -----------------------------------------------------
#define KB_COLS 10
#define KB_CHAR_ROWS 4
#define KB_ACTIONS 3
extern const char *kb_rows[KB_CHAR_ROWS];   // 10 chars each (uppercase)
extern const char *kb_actions[KB_ACTIONS];  // {"CAPS","DEL","OK"}

// draws the naming keyboard: title, the current text with a cursor, the char
// grid + action row, and the selected cell. row 0..3 = chars, row 4 = actions.
void ui_draw_keyboard(UI *ui, SDL_Surface *s, const char *title,
                      const char *text, int caps, int row, int col);

// --- library browser / menus ------------------------------------------------
// file list: names[i], isdir[i]; sel = highlighted, scroll = first visible row,
// play_idx = currently-playing file (or -1). hints drawn in the footer.
void ui_draw_browser(UI *ui, SDL_Surface *s, const char *crumb,
                     const char **names, const int *isdir, int count,
                     int sel, int scroll, int play_idx, const char *hints);
int  ui_browser_visible_rows(SDL_Surface *s); // rows that fit, for scrolling

void ui_draw_menu(UI *ui, SDL_Surface *s, const char *title,
                  const char **opts, int count, int sel, int scroll);
int  ui_menu_visible_rows(SDL_Surface *s);
void ui_draw_confirm(UI *ui, SDL_Surface *s, const char *l1, const char *l2);

// settings: label[i] + value[i] rows, sel highlighted.
void ui_draw_settings(UI *ui, SDL_Surface *s, const char *title,
                      const char **labels, const char **values, int count, int sel);

// FX screen: category + algorithm name + scrollable parameter rows + optional
// response-curve viz (vizDb[vizN], dB; NULL = none) + A/B + render.
void ui_draw_fx(UI *ui, SDL_Surface *s, const char *category, const char *algo,
                const char **labels, const char **values, int count, int sel, int scroll,
                int playing, const float *vizDb, int vizN);
int  ui_fx_visible_rows(SDL_Surface *s, int hasViz);  // param rows that fit (for scrolling)

// --- waveform editor --------------------------------------------------------
// mn[i]/mx[i] are min/max peaks of the VISIBLE view (length = cols). in/out/cur
// are fractions [0,1] within the view. view_lo/view_hi are the view window as
// fractions of the whole file (for the overview bar). pos = cursor time string.
void ui_draw_editor(UI *ui, SDL_Surface *s, const char *name, const char *info,
                    const float *mn, const float *mx, int cols,
                    double in_frac, double out_frac, double cur_frac, int playing,
                    double view_lo, double view_hi, const char *pos);
int  ui_editor_cols(SDL_Surface *s); // waveform pixel width

#endif
