// click_test.c - locate clicks/discontinuities objectively (temporary tool).
//   clang click_test.c h3000.c pitchcore.c dsputil.c fx_*.c wav.c -lm -o /tmp/ct && /tmp/ct
// A smooth tone in -> any click is a discontinuity the effect introduced. We
// report the largest sample-to-sample step and how many exceed a click threshold.
#include "h3000.h"
#include "wav.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static void tone(Audio *a, int rate, long F) {
    a->ch = 1; a->rate = rate; a->bits = 16; a->frames = F;
    a->data = malloc((size_t)F * sizeof(float));
    for (long i = 0; i < F; i++)
        a->data[i] = 0.45f * sinf(2.0f * (float)M_PI * 220.0f * i / rate)
                   + 0.20f * sinf(2.0f * (float)M_PI * 440.0f * i / rate);
}
static void analyze(const char *tag, int algo, const float *p) {
    Audio a = {0}; tone(&a, 48000, 48000);
    h3k_render(&a, algo, p);
    double maxd = 0; int jumps = 0; const double thr = 0.15;
    for (long i = 1; i < a.frames; i++)
        for (int c = 0; c < 2; c++) {
            double d = fabs((double)a.data[i*2+c] - (double)a.data[(i-1)*2+c]);
            if (d > maxd) maxd = d;
            if (d > thr) jumps++;
        }
    printf("%-30s maxStep=%.3f  clicks(>%.2f)=%d %s\n", tag, maxd, thr, jumps,
           jumps == 0 ? "" : "  <-- CLICKS");
    audio_free(&a);
}
int main(void) {
    for (int i = 0; i < h3k_algo_count; i++) {
        float p[H3K_MAX_PARAMS] = {0}; h3k_defaults(i, p);
        char t[40]; snprintf(t, sizeof t, "%d:%s default", i, h3k_algos[i]->name);
        analyze(t, i, p);
    }
    float p[H3K_MAX_PARAMS] = {0}; h3k_defaults(0, p);
    p[0] = 700; p[2] = -500; p[6] = 3;
    analyze("MICROPITCH +700/-500 MODERN", 0, p);

    // Localize the REVERSE click (algo 7): params [SEGMENT,PITCH,FEEDBACK,MIX,SPLICE]
    { float q[H3K_MAX_PARAMS]={0}; h3k_defaults(7,q); q[1]=0; q[2]=0; analyze("REVERSE pitch0 fb0", 7, q); }
    { float q[H3K_MAX_PARAMS]={0}; h3k_defaults(7,q); q[2]=0;          analyze("REVERSE pitch+12 fb0", 7, q); }
    { float q[H3K_MAX_PARAMS]={0}; h3k_defaults(7,q); q[1]=0;          analyze("REVERSE pitch0 fb0.3", 7, q); }
    { float q[H3K_MAX_PARAMS]={0}; h3k_defaults(7,q); q[1]=0; q[2]=0; q[4]=0; analyze("REVERSE pitch0 fb0 H910", 7, q); }
    { float q[H3K_MAX_PARAMS]={0}; h3k_defaults(7,q); q[2]=0; q[4]=0;        analyze("REVERSE pitch+12 fb0 H910", 7, q); }
    { float q[H3K_MAX_PARAMS]={0}; h3k_defaults(7,q); q[2]=0; q[1]=7;        analyze("REVERSE pitch+7 fb0 (deflt splice)", 7, q); }
    // Is it the pitch core, or the reversed-stream content? Feed MICROPITCH octave-up on the plain tone:
    { float q[H3K_MAX_PARAMS]={0}; h3k_defaults(0,q); q[0]=1200; q[2]=1200; q[4]=0; q[5]=1; q[6]=1; analyze("MICROPITCH +1200 H949-1 (ref)", 0, q); }
    return 0;
}
