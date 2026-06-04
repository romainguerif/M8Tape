// h3000_test.c - native sanity check for the H3000 engine (no device needed).
//   clang h3000_test.c h3000.c pitchcore.c dsputil.c fx_*.c wav.c -lm -o h3000_test
//   ./h3000_test
// Renders every registered algorithm over a synthetic tone with its default
// params (plus a stressed MicroPitch de-glitch pass) and checks the output stays
// finite, stereo and sane. New fx_*.c modules are covered automatically.
#include "h3000.h"
#include "wav.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int check(const char *tag, Audio *a, int rc) {
    double peak = 0, eL = 0, eR = 0, diff = 0; int bad = 0;
    for (long i = 0; i < a->frames; i++) {
        float L = a->data[i * 2], R = a->data[i * 2 + 1];
        if (isnan(L) || isnan(R) || isinf(L) || isinf(R)) bad++;
        if (fabsf(L) > peak) peak = fabsf(L);
        if (fabsf(R) > peak) peak = fabsf(R);
        eL += L * L; eR += R * R; diff += fabsf(L - R);
    }
    int ok = (rc == 0 && bad == 0 && a->ch == 2 && peak < 4.0);
    printf("%-26s ch=%d peak=%.3f rmsL=%.4f rmsR=%.4f L!=R=%.0f bad=%d %s\n",
           tag, a->ch, peak, sqrt(eL / a->frames), sqrt(eR / a->frames), diff, bad,
           ok ? "OK" : "CHECK");
    return ok;
}

static void make_tone(Audio *a, int rate, long F) {
    a->ch = 1; a->rate = rate; a->bits = 16; a->frames = F;
    a->data = malloc((size_t)F * sizeof(float));
    for (long i = 0; i < F; i++)
        a->data[i] = 0.45f * sinf(2.0f * (float)M_PI * 220.0f * i / rate)
                   + 0.20f * sinf(2.0f * (float)M_PI * 440.0f * i / rate);
}

static int run(const char *tag, int algo, const float *params) {
    Audio a = {0}; make_tone(&a, 48000, 48000);
    int rc = h3k_render(&a, algo, params);
    int ok = check(tag, &a, rc);
    audio_free(&a);
    return ok;
}

int main(void) {
    int ok = 1;
    printf("registered algorithms: %d\n", h3k_algo_count);
    for (int i = 0; i < h3k_algo_count; i++) {
        float p[H3K_MAX_PARAMS] = {0};
        h3k_defaults(i, p);
        char tag[40]; snprintf(tag, sizeof(tag), "%d:%s default", i, h3k_algos[i]->name);
        ok &= run(tag, i, p);
    }
    // stressed MicroPitch (algo 0): big shift + MODERN splice hammers the de-glitch
    {
        float p[H3K_MAX_PARAMS] = {0};
        h3k_defaults(0, p);
        p[0] = +700; p[2] = -500; p[6] = 3 /*MODERN*/;
        ok &= run("0:MICROPITCH stress+deglitch", 0, p);
    }
    printf("\n%s\n", ok ? "ALL CASES OK" : "SOME CASES NEED A LOOK");
    return ok ? 0 : 1;
}
