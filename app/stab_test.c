// stab_test.c - stability probe (temporary): drive feedback/resonant effects at
// extreme settings with a tone burst then SILENCE, and confirm the tail DECAYS
// (no self-oscillation) and carries no NaN / DC. clang stab_test.c h3000.c
// pitchcore.c dsputil.c fx_*.c wav.c -lm -o /tmp/stab && /tmp/stab
#include "h3000.h"
#include "wav.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static void probe(const char *tag, int algo, const float *p) {
    int rate = 48000; long F = rate * 4;          // 2 s tone + 2 s silence
    Audio a = {0}; a.ch = 1; a.rate = rate; a.bits = 16; a.frames = F;
    a.data = malloc((size_t)F * sizeof(float));
    for (long i = 0; i < F; i++)
        a.data[i] = (i < rate * 2)
            ? 0.5f * sinf(2.0f * (float)M_PI * 220.0f * i / rate) : 0.0f;
    int rc = h3k_render(&a, algo, p);
    double peak = 0, tailpk = 0, taildc = 0; int bad = 0;
    long tail0 = (F - rate / 2) * 2;               // last 0.5 s (stereo)
    for (long i = 0; i < F * 2; i++) {
        float v = a.data[i];
        if (!isfinite(v)) bad++;
        double av = fabs(v); if (av > peak) peak = av;
        if (i >= tail0) { if (av > tailpk) tailpk = av; taildc += v; }
    }
    taildc /= (double)(F * 2 - tail0);
    printf("%-22s peak=%.3f  tailPeak=%.4f  tailDC=%+.4f  bad=%d  %s\n",
           tag, peak, tailpk, taildc, bad,
           (bad == 0 && peak < 4.0 && tailpk < 0.25) ? "DECAYS-OK" : "CHECK");
    audio_free(&a);
}

int main(void) {
    // BAND DELAY (algo 5): [TIME,FB,RES,LOW,HIGH,MIX] — the audit's worst cases
    { float p[H3K_MAX_PARAMS]={0}; h3k_defaults(5,p); p[1]=0.9f; p[2]=1.0f; p[5]=1.0f; probe("BANDDELAY FB.9 RES1", 5, p); }
    { float p[H3K_MAX_PARAMS]={0}; h3k_defaults(5,p); p[1]=0.9f; p[2]=0.6f; p[5]=1.0f; probe("BANDDELAY FB.9 RES.6", 5, p); }
    { float p[H3K_MAX_PARAMS]={0}; h3k_defaults(5,p); p[1]=0.7f; p[2]=0.9f; p[5]=1.0f; probe("BANDDELAY FB.7 RES.9", 5, p); }
    // REVERB (algo 6): [PREDELAY,DECAY,SIZE,DAMP,MIX] — DC + buildup worst case
    { float p[H3K_MAX_PARAMS]={0}; h3k_defaults(6,p); p[1]=0.95f; p[2]=0.0f; p[4]=1.0f; probe("REVERB DEC.95 SIZE0", 6, p); }
    // DUAL DELAY (algo 4) high feedback
    { float p[H3K_MAX_PARAMS]={0}; h3k_defaults(4,p); p[2]=0.9f; p[5]=1.0f; probe("DUALDELAY FB.9", 4, p); }
    // STRING (algo 10) max sustain
    { float p[H3K_MAX_PARAMS]={0}; h3k_defaults(10,p); p[1]=0.99f; p[3]=1.0f; p[5]=1.0f; probe("STRING DECAY.99", 10, p); }
    return 0;
}
