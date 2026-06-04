// preview.c - Mac harness: renders the UI screens to BMPs at 1024x768 using
// system SDL2, so the design can be iterated locally without the device.
//   clang preview.c ui.c $(pkg-config --cflags --libs sdl2 SDL2_ttf) -lm -o preview
#include "ui.h"
#include <stdio.h>

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
               .angle = 0.7, .blink = 1, .levelL = -1, .levelR = -1};
    ui_draw_record(&ui, s, &m8, &r); save(s, "out_record.bmp");

    ui_draw_keyboard(&ui, s, "NAME SAMPLE", "rain_forest", 0, 1, 3);
    save(s, "out_keyboard.bmp");

    SDL_FreeSurface(s);
    ui_free(&ui);
    TTF_Quit();
    return 0;
}
