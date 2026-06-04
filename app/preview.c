// preview.c - Mac harness: renders the UI screens to BMPs at 1024x768 using
// system SDL2, so the design can be iterated locally without the device.
//   clang preview.c ui.c $(pkg-config --cflags --libs sdl2 SDL2_ttf) -lm -o preview
#include "ui.h"
#include <stdio.h>
#include <math.h>

static void save(SDL_Surface *s, const char *name) {
    SDL_SaveBMP(s, name);
    printf("wrote %s\n", name);
}

int main(void) {
    if (TTF_Init() != 0) { fprintf(stderr, "TTF_Init: %s\n", TTF_GetError()); return 1; }
    UI ui;
    if (ui_load_fonts(&ui, "res")) { fprintf(stderr, "font load failed (res/*.ttf)\n"); return 1; }

    SDL_Surface *s = SDL_CreateRGBSurface(0, 1024, 768, 32, 0, 0, 0, 0);

    UIInput m8 = {1, "DIRTYWAVE M8", 24, 44100, "S24_3LE"};
    UIInput ms = {1, "STEREO INPUT", 2, 48000, "S24_3LE"};
    UIInput none = {0, 0, 0, 0, 0};

    ui_draw_home(&ui, s, &m8);   save(s, "out_home_m8.bmp");
    ui_draw_home(&ui, s, &ms);   save(s, "out_home_stereo.bmp");
    ui_draw_home(&ui, s, &none); save(s, "out_home_noinput.bmp");

    UIRec r = {.elapsed = 3 * 60 + 27, .take = "rec_20260604_174142.wav",
               .angle = 0.7, .blink = 1, .levelL = 0.62f, .levelR = 0.91f};
    ui_draw_record(&ui, s, &m8, &r); save(s, "out_record.bmp");

    const char *sl[4] = {"TRIM SILENCE ON REC", "SILENCE THRESHOLD", "AUTO-STOP ON SILENCE", "AUTO-STOP AFTER"};
    const char *sv[4] = {"ON", "-48 DB", "OFF", "3 S"};
    ui_draw_settings(&ui, s, "SETTINGS", sl, sv, 4, 1);
    save(s, "out_settings.bmp");

    ui_draw_keyboard(&ui, s, "NAME SAMPLE", "rain_forest", 0, 1, 3);
    save(s, "out_keyboard.bmp");

    const char *names[] = {"Field", "Drums", "kick_01.wav", "rain_forest.wav", "snare_02.wav", "vox_take.wav"};
    int isd[] = {1, 1, 0, 0, 0, 0};
    ui_draw_browser(&ui, s, "M8TAPE / Unsorted", names, isd, 6, 3, 0, 3,
                    "A OPEN/PLAY  Y ACTIONS  X NEW FOLDER  B BACK");
    save(s, "out_browser.bmp");

    const char *opts[] = {"PLAY", "EDIT", "RENAME", "MOVE", "DELETE"};
    ui_draw_menu(&ui, s, "ACTIONS", opts, 5, 1, 0);
    save(s, "out_menu.bmp");

    int cols = ui_editor_cols(s);
    static float mn[2048], mx[2048];
    for (int i = 0; i < cols; i++) {
        double t = (double)i / cols;
        double env = 0.15 + 0.85 * fabs(sin(t * 6.2831 * 3));
        double a = env * (0.35 + 0.6 * ((i * 37 % 100) / 100.0));
        mx[i] = (float)a; mn[i] = (float)-a;
    }
    ui_draw_editor(&ui, s, "rain_forest.wav", "0:42  44100HZ  2CH  24BIT",
                   mn, mx, cols, 0.18, 0.72, 0.40, 0);
    save(s, "out_editor.bmp");

    const char *eopts[] = {"NORMALIZE", "FADE IN", "FADE OUT", "REVERSE",
        "TRIM TO SELECTION", "TRIM SILENCE", "TO MONO", "16-BIT", "HALF RATE",
        "SAVE", "SAVE AS", "EXIT"};
    ui_draw_menu(&ui, s, "EDIT", eopts, 12, 4, 0);
    save(s, "out_emenu.bmp");

    SDL_FreeSurface(s);
    ui_free(&ui);
    TTF_Quit();
    return 0;
}
