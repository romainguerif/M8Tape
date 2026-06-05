// pitchcore.c - see pitchcore.h. Faithful port of the Faust `transpose` (taps at
// delay base+d and base+d+P, gains min(d/x,1) / 1-min(d/x,1), d ramping by
// (1-ratio) per sample and wrapping at the window P). H910 keeps P fixed; the
// H949/Modern modes re-pick P at every splice by normalized cross-correlation so
// the two crossfaded taps are phase-aligned (de-glitch, WSOLA-style).
#include "pitchcore.h"
#include <stdlib.h>
#include <math.h>

const char *const PITCH_SPLICE_CHOICES[] = { "H910", "H949-1", "H949-2", "MODERN", 0 };

struct PitchVoice {
    float *buf;
    int    n;
    int    wr;
    float  phase;          // ramp 0..P
    float  rate;
    float  base;           // fixed read base (samples) — keeps reads off wr
    float  Wmax;           // max window / search ceiling (samples)
    float  P;              // current window = splice interval (samples), adaptive
    float  Ptarget;        // de-glitch picks this at a splice; applied only while
                           // the far tap is muted (g==1) so the seam stays continuous
    int    sinceRecomp, recompMin;  // cap the de-glitch search rate on fast wraps
    float  Pmin, Pmax;     // de-glitch period search bounds (samples)
    float  x;              // crossfade length (samples)
    float  ratio;          // 2^(cents/1200)
    int    splice;
    int    deglitch;       // 0 for H910, 1 otherwise
    float  corrL;          // correlation window (samples)
    int    corrStep;       // lag step for the period search
    float  driftDepth;     // LC-clock drift depth (cents); 0 = none
    float  lfo1, lfo2, dlfo1, dlfo2;
};

static float pv_read(const PitchVoice *v, float delay) {
    float pos = (float)v->wr - delay;
    while (pos < 0) pos += v->n;
    int i0 = (int)pos;
    float frac = pos - i0;
    int i1 = i0 + 1; if (i1 >= v->n) i1 -= v->n;
    return v->buf[i0] * (1.0f - frac) + v->buf[i1] * frac;
}
static float pv_at(const PitchVoice *v, int delay) {
    int pos = v->wr - delay;
    while (pos < 0)     pos += v->n;
    while (pos >= v->n) pos -= v->n;
    return v->buf[pos];
}
// pick the splice interval P in [Pmin,Pmax] maximizing the normalized
// cross-correlation between the buffer the incoming tap reads (around base) and
// the one the outgoing tap reads (around base+P) -> splice on ~integer periods.
static float pv_best_period(const PitchVoice *v) {
    int b = (int)v->base;
    int L = (int)v->corrL; if (L < 16) L = 16;
    int lo = (int)v->Pmin, hi = (int)v->Pmax;
    int step = v->corrStep > 0 ? v->corrStep : 1;
    int best = (int)v->P; float bestScore = -1e30f;
    for (int P = lo; P <= hi; P += step) {
        float dot = 0, e1 = 0, e2 = 0;
        for (int k = 0; k < L; k++) {
            float a = pv_at(v, b + k);
            float c = pv_at(v, b + P + k);
            dot += a * c; e1 += a * a; e2 += c * c;
        }
        float score = dot / (sqrtf(e1 * e2) + 1e-9f);
        if (score > bestScore) { bestScore = score; best = P; }
    }
    int rlo = best - step, rhi = best + step;
    if (rlo < lo) rlo = lo;
    if (rhi > hi) rhi = hi;
    for (int P = rlo; P <= rhi; P++) {
        float dot = 0, e1 = 0, e2 = 0;
        for (int k = 0; k < L; k++) {
            float a = pv_at(v, b + k);
            float c = pv_at(v, b + P + k);
            dot += a * c; e1 += a * a; e2 += c * c;
        }
        float score = dot / (sqrtf(e1 * e2) + 1e-9f);
        if (score > bestScore) { bestScore = score; best = P; }
    }
    return (float)best;
}

void pv_set_cents(PitchVoice *v, float cents) {
    v->ratio = powf(2.0f, cents / 1200.0f);
}
void pv_set_splice(PitchVoice *v, int splice) {
    v->splice = splice;
    float r = v->rate;
    switch (splice) {
        case SPLICE_H949_1:
            v->deglitch = 1; v->x = 0.012f * r; v->corrL = 0.006f * r; v->corrStep = 6; v->driftDepth = 0.0f; break;
        case SPLICE_H949_2:
            v->deglitch = 1; v->x = 0.016f * r; v->corrL = 0.009f * r; v->corrStep = 3; v->driftDepth = 0.0f; break;
        case SPLICE_MODERN:
            v->deglitch = 1; v->x = 0.020f * r; v->corrL = 0.012f * r; v->corrStep = 2; v->driftDepth = 0.0f; break;
        case SPLICE_H910:
        default:
            v->deglitch = 0; v->x = 0.012f * r; v->driftDepth = 3.0f; v->P = v->Wmax; v->Ptarget = v->Wmax; break;
    }
}

PitchVoice *pv_create(int rate) {
    PitchVoice *v = calloc(1, sizeof(*v));
    if (!v) return NULL;
    v->rate = (float)(rate > 0 ? rate : 48000);
    v->Wmax = 0.050f * v->rate;
    v->base = v->Wmax;
    v->Pmin = 0.005f * v->rate;     // 5..50 ms period search (per the H949 patent)
    v->Pmax = v->Wmax;
    v->P    = v->Wmax;
    v->Ptarget = v->Wmax;
    v->recompMin = (int)(0.010f * v->rate); if (v->recompMin < 1) v->recompMin = 1;
    v->x    = 0.012f * v->rate;
    v->ratio = 1.0f;
    v->corrL = 0.008f * v->rate; v->corrStep = 4;
    v->splice = SPLICE_H910; v->deglitch = 0; v->driftDepth = 0.0f;
    v->lfo1 = 0.0f; v->lfo2 = 0.0f;
    v->dlfo1 = 2.0f * (float)M_PI * 0.7f / v->rate;
    v->dlfo2 = 2.0f * (float)M_PI * 3.1f / v->rate;
    v->n = (int)(v->base + 2.0f * v->Pmax + 0.012f * v->rate) + 256;
    v->buf = calloc(v->n, sizeof(float));
    if (!v->buf) { free(v); return NULL; }
    v->wr = 0; v->phase = 0.0f;
    return v;
}
void pv_destroy(PitchVoice *v) { if (v) { free(v->buf); free(v); } }

float pv_process(PitchVoice *v, float in) {
    v->buf[v->wr] = in;
    v->wr++; if (v->wr >= v->n) v->wr = 0;

    float d = v->phase;
    float xeff = v->x;                         // crossfade must fit inside P
    if (xeff > 0.45f * v->P) xeff = 0.45f * v->P;
    if (xeff < 1.0f) xeff = 1.0f;
    // Apply a pending de-glitch window ONLY while the far tap is muted (g==1, i.e.
    // d >= xeff). The far tap delay then never jumps while it is audible, and P is
    // already at target by the next wrap, so the splice stays continuous (no click).
    if (v->deglitch && d >= xeff) v->P = v->Ptarget;
    float s1 = pv_read(v, v->base + d);          // incoming tap (closer)
    float s2 = pv_read(v, v->base + d + v->P);   // outgoing tap (one window back)
    float g = d / xeff; if (g > 1.0f) g = 1.0f; if (g < 0.0f) g = 0.0f;
    float out = g * s1 + (1.0f - g) * s2;

    float ratio = v->ratio;
    if (v->driftDepth > 0.0f) {                 // LC-clock drift (H910 character)
        float cents = v->driftDepth * (0.6f * sinf(v->lfo1) + 0.4f * sinf(v->lfo2));
        ratio *= powf(2.0f, cents / 1200.0f);
        v->lfo1 += v->dlfo1; if (v->lfo1 > 6.2831853f) v->lfo1 -= 6.2831853f;
        v->lfo2 += v->dlfo2; if (v->lfo2 > 6.2831853f) v->lfo2 -= 6.2831853f;
    }
    v->sinceRecomp++;
    v->phase += (1.0f - ratio);
    // Recompute the de-glitch period at most every recompMin samples: on big
    // upshifts wraps come very fast and the autocorrelation search would dominate
    // CPU (risking preview underruns). Between recomputes the window just holds.
    int recomp = v->deglitch && v->sinceRecomp >= v->recompMin;
    if (v->phase >= v->P) {                     // splice (downshift)
        v->phase -= v->P;
        if (recomp) { v->Ptarget = pv_best_period(v); v->sinceRecomp = 0; }
    } else if (v->phase < 0.0f) {               // splice (upshift)
        if (recomp) { v->Ptarget = pv_best_period(v); v->sinceRecomp = 0; }
        v->phase += v->P;
    }
    return out;
}
