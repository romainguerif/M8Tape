// fx_eq.c - 8-band parametric EQ (STUDIO). Eight RBJ biquads in series:
//   band 1      = low SHELF
//   bands 2..7  = PEAKING
//   band 8      = high SHELF
// Each band exposes FREQ / GAIN(dB) / Q -> 24 params total. Coefficients are the
// RBJ Audio EQ Cookbook forms; we write directly into the shared Biquad's
// b0..a2 fields (a0-normalized, matching dsputil's bq_norm) and run the same
// difference equation via bq_process(). Mono-in: the identical filter chain is
// applied to both output channels. response() reconstructs the combined dB
// magnitude from the live params alone so the UI can draw the curve.
#include "h3000.h"
#include "dsputil.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define EQ_BANDS 8

// Per-band parameter layout: [freq, gain, q] x 8. Index helpers below.
enum { PP_FREQ = 0, PP_GAIN = 1, PP_Q = 2, PP_STRIDE = 3 };

// Band kinds (drives which RBJ formula to use).
enum { BK_LOWSHELF, BK_PEAK, BK_HIGHSHELF };

static int band_kind(int b) {
    if (b == 0)            return BK_LOWSHELF;
    if (b == EQ_BANDS - 1) return BK_HIGHSHELF;
    return BK_PEAK;
}

typedef struct {
    Biquad bq[EQ_BANDS];
    float  pf[EQ_BANDS], pg[EQ_BANDS], pq[EQ_BANDS]; // last-applied params (cache)
    int    rate;
    int    primed;                                   // 0 until first block sets coeffs
} EqState;

// --- RBJ coefficient computation -------------------------------------------
// Fills b0..a2 (already divided by a0) for one band. `freq`,`gain`(dB),`q` are
// clamped by the caller's spec; we still guard the math here. Stable for all
// freq in (0, nyquist).
static void rbj_coeffs(int kind, float freq, float gainDb, float q, int rate,
                       float *b0, float *b1, float *b2, float *a1, float *a2) {
    if (rate <= 0) rate = 48000;
    if (q < 0.05f) q = 0.05f;
    // Keep the center strictly below Nyquist for shelf/peak stability.
    float fmax = (float)rate * 0.49f;
    if (freq < 1.0f)  freq = 1.0f;
    if (freq > fmax)  freq = fmax;

    float A  = powf(10.0f, gainDb / 40.0f);          // amplitude (sqrt of gain)
    float w0 = 2.0f * (float)M_PI * freq / (float)rate;
    float cw = cosf(w0), sw = sinf(w0);
    float alpha = sw / (2.0f * q);

    float B0, B1, B2, A0, A1, A2;
    if (kind == BK_PEAK) {
        B0 = 1.0f + alpha * A;
        B1 = -2.0f * cw;
        B2 = 1.0f - alpha * A;
        A0 = 1.0f + alpha / A;
        A1 = -2.0f * cw;
        A2 = 1.0f - alpha / A;
    } else {
        float sqA = sqrtf(A);
        float ap1 = A + 1.0f, am1 = A - 1.0f;
        float tsa = 2.0f * sqA * alpha;              // 2*sqrt(A)*alpha
        if (kind == BK_LOWSHELF) {
            B0 =        A * (ap1 - am1 * cw + tsa);
            B1 = 2.0f * A * (am1 - ap1 * cw);
            B2 =        A * (ap1 - am1 * cw - tsa);
            A0 =            (ap1 + am1 * cw + tsa);
            A1 =    -2.0f * (am1 + ap1 * cw);
            A2 =            (ap1 + am1 * cw - tsa);
        } else { // BK_HIGHSHELF
            B0 =        A * (ap1 + am1 * cw + tsa);
            B1 = -2.0f * A * (am1 + ap1 * cw);
            B2 =        A * (ap1 + am1 * cw - tsa);
            A0 =            (ap1 - am1 * cw + tsa);
            A1 =     2.0f * (am1 - ap1 * cw);
            A2 =            (ap1 - am1 * cw - tsa);
        }
    }

    if (!(A0 > 1e-20f) || !isfinite(A0)) {           // degenerate -> passthrough
        *b0 = 1.0f; *b1 = *b2 = *a1 = *a2 = 0.0f;
        return;
    }
    float inv = 1.0f / A0;
    *b0 = B0 * inv; *b1 = B1 * inv; *b2 = B2 * inv;
    *a1 = A1 * inv; *a2 = A2 * inv;
    if (!isfinite(*b0) || !isfinite(*b1) || !isfinite(*b2) ||
        !isfinite(*a1) || !isfinite(*a2)) {
        *b0 = 1.0f; *b1 = *b2 = *a1 = *a2 = 0.0f;
    }
}

// Recompute band b's biquad from p only if its params changed (click-free).
static void eq_update_band(EqState *s, const float *p, int b) {
    const float *bp = p + b * PP_STRIDE;
    float f = bp[PP_FREQ], g = bp[PP_GAIN], q = bp[PP_Q];
    if (s->primed && f == s->pf[b] && g == s->pg[b] && q == s->pq[b]) return;
    Biquad *bq = &s->bq[b];
    rbj_coeffs(band_kind(b), f, g, q, s->rate,
               &bq->b0, &bq->b1, &bq->b2, &bq->a1, &bq->a2);
    s->pf[b] = f; s->pg[b] = g; s->pq[b] = q;
}

static void *eq_create(int rate) {
    EqState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->rate = rate > 0 ? rate : 48000;
    for (int b = 0; b < EQ_BANDS; b++) {
        bq_reset(&s->bq[b]);
        s->bq[b].b0 = 1.0f; // unity until first block primes real coeffs
    }
    s->primed = 0;
    return s;
}

static void eq_block(void *st, const float *dry, int n, const float *p, float *outLR) {
    EqState *s = (EqState *)st;
    for (int b = 0; b < EQ_BANDS; b++) eq_update_band(s, p, b);
    s->primed = 1;
    for (int i = 0; i < n; i++) {
        float x = dry[i];
        if (!isfinite(x)) x = 0.0f;
        for (int b = 0; b < EQ_BANDS; b++) x = bq_process(&s->bq[b], x);
        if (!isfinite(x)) {                          // kill any blow-up / NaN
            x = 0.0f;
            for (int b = 0; b < EQ_BANDS; b++) bq_reset(&s->bq[b]);
        }
        // Flush denormals on the running output to dodge CPU stalls.
        if (x > -1e-25f && x < 1e-25f) x = 0.0f;
        outLR[i * 2 + 0] = x;
        outLR[i * 2 + 1] = x;
    }
}

static void eq_destroy(void *st) { free(st); }

// --- viz: combined magnitude response in dB --------------------------------
// Stateless; reconstructs each band's biquad from p and sums |H(e^jw)| in dB
// across n log-spaced bins from 20 Hz to min(20k, rate*0.45).
static int eq_response(const float *p, int rate, float *outDb, int n) {
    if (!outDb || n <= 0) return 0;
    if (rate <= 0) rate = 48000;
    float fhi = (float)rate * 0.45f;
    if (fhi > 20000.0f) fhi = 20000.0f;
    float flo = 20.0f;
    if (fhi <= flo) fhi = flo + 1.0f;
    float llo = logf(flo), lhi = logf(fhi);

    // Precompute all 8 band biquads once.
    float b0[EQ_BANDS], b1[EQ_BANDS], b2[EQ_BANDS], a1[EQ_BANDS], a2[EQ_BANDS];
    for (int b = 0; b < EQ_BANDS; b++) {
        const float *bp = p + b * PP_STRIDE;
        rbj_coeffs(band_kind(b), bp[PP_FREQ], bp[PP_GAIN], bp[PP_Q], rate,
                   &b0[b], &b1[b], &b2[b], &a1[b], &a2[b]);
    }
    for (int i = 0; i < n; i++) {
        float t  = (n > 1) ? (float)i / (float)(n - 1) : 0.0f;
        float f  = expf(llo + (lhi - llo) * t);
        float w  = 2.0f * (float)M_PI * f / (float)rate;
        float c1 = cosf(w),       s1 = sinf(w);
        float c2 = cosf(2.0f * w), s2 = sinf(2.0f * w);
        float sumDb = 0.0f;
        for (int b = 0; b < EQ_BANDS; b++) {
            // H(z) = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2), z=e^jw
            float nre = b0[b] + b1[b] * c1 + b2[b] * c2;
            float nim = -(b1[b] * s1 + b2[b] * s2);
            float dre = 1.0f + a1[b] * c1 + a2[b] * c2;
            float dim = -(a1[b] * s1 + a2[b] * s2);
            float num = nre * nre + nim * nim;        // |num|^2
            float den = dre * dre + dim * dim;        // |den|^2
            if (den < 1e-30f) den = 1e-30f;
            float mag2 = num / den;
            if (mag2 < 1e-30f) mag2 = 1e-30f;
            sumDb += 10.0f * log10f(mag2);            // 20*log10(sqrt) = 10*log10
        }
        if (!isfinite(sumDb)) sumDb = 0.0f;
        outDb[i] = sumDb;
    }
    return 1;
}

const H3kAlgoDef eq_def = {
    .name = "EQ",
    .category = "STUDIO",
    .nparams = EQ_BANDS * PP_STRIDE,   // 24
    .params = {
        // band 1 - LOW SHELF
        { "B1 FREQ",   20, 20000, 5, 60,    PK_HZ,    0 },
        { "B1 GAIN",  -18,    18, 1,  0,    PK_DB,    0 },
        { "B1 Q",    0.3f,     8, 0.1f, 0.7f, PK_FLOAT, 0 },
        // band 2 - PEAK
        { "B2 FREQ",   20, 20000, 5, 150,   PK_HZ,    0 },
        { "B2 GAIN",  -18,    18, 1,  0,    PK_DB,    0 },
        { "B2 Q",    0.3f,     8, 0.1f, 0.7f, PK_FLOAT, 0 },
        // band 3 - PEAK
        { "B3 FREQ",   20, 20000, 5, 400,   PK_HZ,    0 },
        { "B3 GAIN",  -18,    18, 1,  0,    PK_DB,    0 },
        { "B3 Q",    0.3f,     8, 0.1f, 0.7f, PK_FLOAT, 0 },
        // band 4 - PEAK
        { "B4 FREQ",   20, 20000, 5, 1000,  PK_HZ,    0 },
        { "B4 GAIN",  -18,    18, 1,  0,    PK_DB,    0 },
        { "B4 Q",    0.3f,     8, 0.1f, 0.7f, PK_FLOAT, 0 },
        // band 5 - PEAK
        { "B5 FREQ",   20, 20000, 5, 2500,  PK_HZ,    0 },
        { "B5 GAIN",  -18,    18, 1,  0,    PK_DB,    0 },
        { "B5 Q",    0.3f,     8, 0.1f, 0.7f, PK_FLOAT, 0 },
        // band 6 - PEAK
        { "B6 FREQ",   20, 20000, 5, 5000,  PK_HZ,    0 },
        { "B6 GAIN",  -18,    18, 1,  0,    PK_DB,    0 },
        { "B6 Q",    0.3f,     8, 0.1f, 0.7f, PK_FLOAT, 0 },
        // band 7 - PEAK
        { "B7 FREQ",   20, 20000, 5, 10000, PK_HZ,    0 },
        { "B7 GAIN",  -18,    18, 1,  0,    PK_DB,    0 },
        { "B7 Q",    0.3f,     8, 0.1f, 0.7f, PK_FLOAT, 0 },
        // band 8 - HIGH SHELF
        { "B8 FREQ",   20, 20000, 5, 15000, PK_HZ,    0 },
        { "B8 GAIN",  -18,    18, 1,  0,    PK_DB,    0 },
        { "B8 Q",    0.3f,     8, 0.1f, 0.7f, PK_FLOAT, 0 },
    },
    .create = eq_create,
    .block = eq_block,
    .destroy = eq_destroy,
    .response = eq_response,
};
