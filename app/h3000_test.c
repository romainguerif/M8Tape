// h3000_test.c - native sanity check for the H3000 DSP (no device needed).
//   clang h3000_test.c h3000.c wav.c -lm -o h3000_test && ./h3000_test
#include "h3000.h"
#include "wav.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

int main(void) {
    int rate = 48000; long F = rate;            // 1 s
    Audio a = {0};
    a.ch = 1; a.rate = rate; a.bits = 16; a.frames = F;
    a.data = malloc((size_t)F * sizeof(float));
    for (long i = 0; i < F; i++) a.data[i] = 0.5f * sinf(2.0f * (float)M_PI * 220.0f * i / rate);

    MicroPitchParams p = {.cents_a = -9, .delay_a_ms = 15, .cents_b = +11,
                          .delay_b_ms = 25, .feedback = 0.0f, .mix = 0.5f};
    int rc = h3000_micropitch(&a, &p);
    printf("rc=%d  ch=%d frames=%ld\n", rc, a.ch, a.frames);

    double peak = 0, eL = 0, eR = 0, diff = 0; int nan = 0;
    for (long i = 0; i < a.frames; i++) {
        float L = a.data[i * 2], R = a.data[i * 2 + 1];
        if (isnan(L) || isnan(R) || isinf(L) || isinf(R)) nan++;
        if (fabsf(L) > peak) peak = fabsf(L);
        if (fabsf(R) > peak) peak = fabsf(R);
        eL += L * L; eR += R * R; diff += fabsf(L - R);
    }
    printf("peak=%.3f  rmsL=%.4f rmsR=%.4f  L!=R sum=%.1f  nan/inf=%d\n",
           peak, sqrt(eL / a.frames), sqrt(eR / a.frames), diff, nan);

    wav_save("out_micropitch.wav", &a);
    printf("wrote out_micropitch.wav (%s)\n",
           (rc == 0 && nan == 0 && peak > 0.01 && peak < 2.0 && diff > 1.0) ? "LOOKS OK" : "CHECK");
    audio_free(&a);
    return 0;
}
