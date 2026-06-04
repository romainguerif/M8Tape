// h3000.c - H3000-style DSP. Time-domain pitch core (2-tap moving delay line +
// crossfade, à la Faust transpose / H910) + the MicroPitch algorithm.
#include "h3000.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// --- time-domain pitch voice -------------------------------------------------
// ring buffer read by two taps a window apart; linear crossfade hides the
// splice when a tap wraps. ratio = 2^(cents/1200); offset accumulator grows by
// (1-ratio) per sample (Doppler: f_out = f_in*(1 - dDelay/dt)).
typedef struct {
    float *buf;
    int    n;       // buffer size (power-of-two-ish, > 3*w)
    int    wr;      // write index
    float  phase;   // sawtooth 0..w
    float  w, x;    // window, crossfade (samples)
    float  ratio;
} PitchVoice;

static float wrapf(float v, float m) {
    while (v >= m) v -= m;
    while (v < 0)  v += m;
    return v;
}

// fractional read `delay` samples behind the write pointer (linear interp).
static float pv_read(const PitchVoice *v, float delay) {
    float pos = (float)v->wr - delay;
    while (pos < 0) pos += v->n;
    int i0 = (int)pos;
    float frac = pos - i0;
    int i1 = i0 + 1; if (i1 >= v->n) i1 -= v->n;
    return v->buf[i0] * (1.0f - frac) + v->buf[i1] * frac;
}

static void pv_init(PitchVoice *v, float rate, float cents) {
    v->w = 0.050f * rate;             // 50 ms window
    v->x = 0.012f * rate;             // 12 ms crossfade
    v->ratio = powf(2.0f, cents / 1200.0f);
    v->n = (int)(v->w * 3.0f) + 64;   // taps live in [w .. 3w]
    v->buf = calloc(v->n, sizeof(float));
    v->wr = 0;
    v->phase = 0.0f;
}
static void pv_free(PitchVoice *v) { free(v->buf); v->buf = NULL; }

static float pv_process(PitchVoice *v, float in) {
    v->buf[v->wr] = in;
    v->wr++; if (v->wr >= v->n) v->wr = 0;

    float d = v->phase;                       // 0..w
    float base = v->w;                        // keep delays positive (>= w)
    float s1 = pv_read(v, base + d);          // tap 1
    float s2 = pv_read(v, base + d + v->w);   // tap 2, one window back
    float g = d / v->x; if (g > 1.0f) g = 1.0f;
    float out = g * s1 + (1.0f - g) * s2;

    v->phase += (1.0f - v->ratio);
    v->phase = wrapf(v->phase, v->w);
    return out;
}

// --- simple delay line with feedback tap ------------------------------------
typedef struct { float *buf; int n, wr; } Delay;
static void dl_init(Delay *dl, float rate, float max_ms) {
    dl->n = (int)(rate * (max_ms / 1000.0f)) + 8;
    dl->buf = calloc(dl->n, sizeof(float));
    dl->wr = 0;
}
static void dl_free(Delay *dl) { free(dl->buf); dl->buf = NULL; }
static float dl_read(const Delay *dl, float ms, float rate) {
    float delay = ms / 1000.0f * rate;
    float pos = (float)dl->wr - delay;
    while (pos < 0) pos += dl->n;
    int i0 = (int)pos; float frac = pos - i0;
    int i1 = i0 + 1; if (i1 >= dl->n) i1 -= dl->n;
    return dl->buf[i0] * (1.0f - frac) + dl->buf[i1] * frac;
}
static void dl_write(Delay *dl, float v) { dl->buf[dl->wr] = v; dl->wr++; if (dl->wr >= dl->n) dl->wr = 0; }

// --- MicroPitch --------------------------------------------------------------
int h3000_micropitch(Audio *a, const MicroPitchParams *p) {
    if (!a->data || a->frames < 1 || a->ch < 1) return -1;
    long F = a->frames;
    float rate = (float)(a->rate > 0 ? a->rate : 48000);

    // dry mono source
    float *dry = malloc((size_t)F * sizeof(float));
    if (!dry) return -1;
    for (long i = 0; i < F; i++) {
        float s = 0;
        for (int c = 0; c < a->ch; c++) s += a->data[i * a->ch + c];
        dry[i] = s / a->ch;
    }

    float *out = malloc((size_t)F * 2 * sizeof(float));   // stereo
    if (!out) { free(dry); return -1; }

    PitchVoice va, vb;
    pv_init(&va, rate, p->cents_a);
    pv_init(&vb, rate, p->cents_b);
    Delay da, db;
    float maxd = p->delay_a_ms > p->delay_b_ms ? p->delay_a_ms : p->delay_b_ms;
    if (maxd < 1) maxd = 1;
    dl_init(&da, rate, maxd + 10);
    dl_init(&db, rate, maxd + 10);

    float fb = p->feedback; if (fb < 0) fb = 0; if (fb > 0.95f) fb = 0.95f;
    float mix = p->mix; if (mix < 0) mix = 0; if (mix > 1) mix = 1;
    float lastA = 0, lastB = 0;

    for (long i = 0; i < F; i++) {
        float in = dry[i];

        float pa = pv_process(&va, in + fb * lastA);
        dl_write(&da, pa);
        float wa = dl_read(&da, p->delay_a_ms, rate);
        lastA = wa;

        float pb = pv_process(&vb, in + fb * lastB);
        dl_write(&db, pb);
        float wb = dl_read(&db, p->delay_b_ms, rate);
        lastB = wb;

        out[i * 2 + 0] = mix * wa + (1.0f - mix) * in;   // L = voice A
        out[i * 2 + 1] = mix * wb + (1.0f - mix) * in;   // R = voice B
    }

    pv_free(&va); pv_free(&vb); dl_free(&da); dl_free(&db); free(dry);

    free(a->data);
    a->data = out;
    a->ch = 2;
    // frames, rate, bits unchanged
    return 0;
}
