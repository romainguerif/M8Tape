// h3000_test.c - native sanity check for the H3000 DSP (no device needed).
//   clang h3000_test.c h3000.c wav.c -lm -o h3000_test && ./h3000_test
// Runs MicroPitch over a synthetic tone in several splice modes (incl. a
// stressed big-shift pass that hammers the H949 correlation de-glitch) and
// checks the output stays finite, stereo and sane.
#include "h3000.h"
#include "wav.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int run_case(const char *tag, float cents_a, float cents_b, int splice) {
    int rate = 48000; long F = rate;            // 1 s
    Audio a = {0};
    a.ch = 1; a.rate = rate; a.bits = 16; a.frames = F;
    a.data = malloc((size_t)F * sizeof(float));
    // a couple of partials so the correlation search has real periodicity
    for (long i = 0; i < F; i++)
        a.data[i] = 0.45f * sinf(2.0f * (float)M_PI * 220.0f * i / rate)
                  + 0.20f * sinf(2.0f * (float)M_PI * 440.0f * i / rate);

    MicroPitchParams p = {.cents_a = cents_a, .delay_a_ms = 15, .cents_b = cents_b,
                          .delay_b_ms = 25, .feedback = 0.0f, .mix = 0.5f,
                          .splice = splice};
    int rc = h3000_micropitch(&a, &p);

    double peak = 0, eL = 0, eR = 0, diff = 0; int bad = 0;
    for (long i = 0; i < a.frames; i++) {
        float L = a.data[i * 2], R = a.data[i * 2 + 1];
        if (isnan(L) || isnan(R) || isinf(L) || isinf(R)) bad++;
        if (fabsf(L) > peak) peak = fabsf(L);
        if (fabsf(R) > peak) peak = fabsf(R);
        eL += L * L; eR += R * R; diff += fabsf(L - R);
    }
    int ok = (rc == 0 && bad == 0 && peak > 0.01 && peak < 2.0 && diff > 1.0);
    printf("%-22s splice=%-7s ch=%d  peak=%.3f rmsL=%.4f rmsR=%.4f  L!=R=%.0f  bad=%d  %s\n",
           tag, h3000_splice_name(splice), a.ch, peak,
           sqrt(eL / a.frames), sqrt(eR / a.frames), diff, bad, ok ? "OK" : "CHECK");

    char fn[64]; snprintf(fn, sizeof(fn), "out_%s.wav", tag);
    wav_save(fn, &a);
    audio_free(&a);
    return ok;
}

int main(void) {
    int ok = 1;
    // iconic MicroPitch detune, each splice mode
    ok &= run_case("micro_h910",  -9, +11, SPLICE_H910);
    ok &= run_case("micro_h949a", -9, +11, SPLICE_H949_1);
    ok &= run_case("micro_h949b", -9, +11, SPLICE_H949_2);
    ok &= run_case("micro_modern",-9, +11, SPLICE_MODERN);
    // stressed: a fifth up / fourth down hammers the splice + de-glitch search
    ok &= run_case("shift_h910",  +700, -500, SPLICE_H910);
    ok &= run_case("shift_modern",+700, -500, SPLICE_MODERN);
    printf("\n%s\n", ok ? "ALL CASES OK" : "SOME CASES NEED A LOOK");
    return ok ? 0 : 1;
}
