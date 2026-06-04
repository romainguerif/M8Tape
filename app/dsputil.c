// dsputil.c - see dsputil.h.
#include "dsputil.h"
#include <stdlib.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// --- fractional delay line ---------------------------------------------------
int dl_init(DelayLine *d, int rate, float max_ms) {
    d->n = (int)((float)rate * (max_ms / 1000.0f)) + 8;
    if (d->n < 8) d->n = 8;
    d->buf = calloc(d->n, sizeof(float));
    d->wr = 0;
    return d->buf != NULL;
}
void dl_free(DelayLine *d) { if (d) { free(d->buf); d->buf = NULL; } }
void dl_clear(DelayLine *d) { if (d && d->buf) for (int i = 0; i < d->n; i++) d->buf[i] = 0.0f; }
void dl_write(DelayLine *d, float v) { d->buf[d->wr] = v; d->wr++; if (d->wr >= d->n) d->wr = 0; }
float dl_read_samp(const DelayLine *d, float samples) {
    if (samples < 0) samples = 0;
    if (samples > d->n - 2) samples = (float)(d->n - 2);
    // wr was post-incremented past the last write, so the most recent sample is
    // at wr-1: a delay of 0 must return "now", not the oldest sample in the line.
    float pos = (float)(d->wr - 1) - samples;
    while (pos < 0) pos += d->n;
    int i0 = (int)pos; float frac = pos - i0;
    int i1 = i0 + 1; if (i1 >= d->n) i1 -= d->n;
    return d->buf[i0] * (1.0f - frac) + d->buf[i1] * frac;
}
float dl_read_ms(const DelayLine *d, float ms, int rate) {
    return dl_read_samp(d, ms / 1000.0f * (float)rate);
}

// --- one-pole ----------------------------------------------------------------
void op_set_lp(OnePole *f, float hz, int rate) {
    if (hz < 1) hz = 1;
    float x = expf(-2.0f * (float)M_PI * hz / (float)rate);
    f->a = 1.0f - x;   // y += a*(in - y)
    // keep state
}
float op_lp(OnePole *f, float in) { f->z += f->a * (in - f->z); return f->z; }
float op_hp(OnePole *f, float in) { return in - op_lp(f, in); }

// --- biquad (RBJ) ------------------------------------------------------------
static void bq_norm(Biquad *f, float b0, float b1, float b2, float a0, float a1, float a2) {
    f->b0 = b0 / a0; f->b1 = b1 / a0; f->b2 = b2 / a0;
    f->a1 = a1 / a0; f->a2 = a2 / a0;
}
void bq_reset(Biquad *f) { f->x1 = f->x2 = f->y1 = f->y2 = 0.0f; }
void bq_lowpass(Biquad *f, float hz, float q, int rate) {
    if (q < 0.05f) q = 0.05f;
    float w = 2.0f * (float)M_PI * hz / (float)rate, c = cosf(w), s = sinf(w), al = s / (2.0f * q);
    bq_norm(f, (1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
}
void bq_highpass(Biquad *f, float hz, float q, int rate) {
    if (q < 0.05f) q = 0.05f;
    float w = 2.0f * (float)M_PI * hz / (float)rate, c = cosf(w), s = sinf(w), al = s / (2.0f * q);
    bq_norm(f, (1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
}
void bq_bandpass(Biquad *f, float hz, float q, int rate) {
    if (q < 0.05f) q = 0.05f;
    float w = 2.0f * (float)M_PI * hz / (float)rate, c = cosf(w), s = sinf(w), al = s / (2.0f * q);
    bq_norm(f, al, 0.0f, -al, 1 + al, -2 * c, 1 - al);   // constant 0 dB peak
}
float bq_process(Biquad *f, float in) {
    float y = f->b0 * in + f->b1 * f->x1 + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1; f->x1 = in; f->y2 = f->y1; f->y1 = y;
    return y;
}

// --- Schroeder allpass -------------------------------------------------------
int ap_init(Allpass *a, int len_samples, float g) {
    a->n = len_samples > 1 ? len_samples : 1;
    a->buf = calloc(a->n, sizeof(float));
    a->wr = 0; a->g = g;
    return a->buf != NULL;
}
void ap_free(Allpass *a) { if (a) { free(a->buf); a->buf = NULL; } }
float ap_process(Allpass *a, float in) {
    // true (lossless) Schroeder allpass:  y[n] = -g*v[n] + v[n-N],  v[n] = in + g*v[n-N]
    // H(z) = (-g + z^-N)/(1 - g*z^-N), magnitude 1 at all frequencies — safe inside
    // reverb feedback loops. (An earlier form fed `in` instead of `v` into the
    // output and was not unity-gain.)
    float d = a->buf[a->wr];          // v[n-N]
    float v = in + a->g * d;          // v[n]
    a->buf[a->wr] = v;
    a->wr++; if (a->wr >= a->n) a->wr = 0;
    return -a->g * v + d;
}

// --- deterministic LCG -------------------------------------------------------
unsigned dsp_lcg(unsigned *s) { *s = (*s) * 1103515245u + 12345u; return (*s >> 1) & 0x7fffffffu; }
float dsp_frand(unsigned *s) { return (float)dsp_lcg(s) / 2147483647.0f; }
