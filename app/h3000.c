// h3000.c - H3000-style DSP. Time-domain pitch core (2-tap moving delay line +
// crossfade, à la Faust transpose / H910) + the MicroPitch algorithm.
// Block-based with persistent state, so it can run a real-time preview while
// parameters change live.
#include "h3000.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

// --- time-domain pitch voice -------------------------------------------------
typedef struct {
    float *buf;
    int    n;
    int    wr;
    float  phase;   // sawtooth 0..w
    float  w, x;    // window, crossfade (samples)
    float  ratio;   // 2^(cents/1200) — updated live each block
} PitchVoice;

static float wrapf(float v, float m) {
    while (v >= m) v -= m;
    while (v < 0)  v += m;
    return v;
}
static float pv_read(const PitchVoice *v, float delay) {
    float pos = (float)v->wr - delay;
    while (pos < 0) pos += v->n;
    int i0 = (int)pos;
    float frac = pos - i0;
    int i1 = i0 + 1; if (i1 >= v->n) i1 -= v->n;
    return v->buf[i0] * (1.0f - frac) + v->buf[i1] * frac;
}
static void pv_init(PitchVoice *v, float rate) {
    v->w = 0.050f * rate;
    v->x = 0.012f * rate;
    v->ratio = 1.0f;
    v->n = (int)(v->w * 3.0f) + 64;
    v->buf = calloc(v->n, sizeof(float));
    v->wr = 0; v->phase = 0.0f;
}
static void pv_free(PitchVoice *v) { free(v->buf); v->buf = NULL; }
static float pv_process(PitchVoice *v, float in) {
    v->buf[v->wr] = in;
    v->wr++; if (v->wr >= v->n) v->wr = 0;
    float d = v->phase;
    float base = v->w;
    float s1 = pv_read(v, base + d);
    float s2 = pv_read(v, base + d + v->w);
    float g = d / v->x; if (g > 1.0f) g = 1.0f;
    float out = g * s1 + (1.0f - g) * s2;
    v->phase += (1.0f - v->ratio);
    v->phase = wrapf(v->phase, v->w);
    return out;
}

// --- delay line with feedback tap -------------------------------------------
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

// --- MicroPitch streaming state ---------------------------------------------
struct MicroPitchState {
    PitchVoice va, vb;
    Delay da, db;
    float lastA, lastB;
    float rate;
};

MicroPitchState *mp_create(int rate) {
    MicroPitchState *st = calloc(1, sizeof(*st));
    if (!st) return NULL;
    st->rate = (float)(rate > 0 ? rate : 48000);
    pv_init(&st->va, st->rate);
    pv_init(&st->vb, st->rate);
    dl_init(&st->da, st->rate, 1010);   // up to ~1 s delay
    dl_init(&st->db, st->rate, 1010);
    return st;
}
void mp_destroy(MicroPitchState *st) {
    if (!st) return;
    pv_free(&st->va); pv_free(&st->vb); dl_free(&st->da); dl_free(&st->db);
    free(st);
}

// process n mono frames -> n stereo frames, reading params live each call.
void mp_block(MicroPitchState *st, const float *dry, int n,
              const MicroPitchParams *p, float *outLR) {
    st->va.ratio = powf(2.0f, p->cents_a / 1200.0f);
    st->vb.ratio = powf(2.0f, p->cents_b / 1200.0f);
    float fb = p->feedback; if (fb < 0) fb = 0; if (fb > 0.95f) fb = 0.95f;
    float mix = p->mix; if (mix < 0) mix = 0; if (mix > 1) mix = 1;
    for (int i = 0; i < n; i++) {
        float in = dry[i];
        float pa = pv_process(&st->va, in + fb * st->lastA);
        dl_write(&st->da, pa);
        float wa = dl_read(&st->da, p->delay_a_ms, st->rate);
        st->lastA = wa;
        float pb = pv_process(&st->vb, in + fb * st->lastB);
        dl_write(&st->db, pb);
        float wb = dl_read(&st->db, p->delay_b_ms, st->rate);
        st->lastB = wb;
        outLR[i * 2 + 0] = mix * wa + (1.0f - mix) * in;
        outLR[i * 2 + 1] = mix * wb + (1.0f - mix) * in;
    }
}

// --- whole-sample destructive render (uses the streaming core) ---------------
int h3000_micropitch(Audio *a, const MicroPitchParams *p) {
    if (!a->data || a->frames < 1 || a->ch < 1) return -1;
    long F = a->frames;
    float *dry = malloc((size_t)F * sizeof(float));
    float *out = malloc((size_t)F * 2 * sizeof(float));
    MicroPitchState *st = mp_create(a->rate);
    if (!dry || !out || !st) { free(dry); free(out); mp_destroy(st); return -1; }

    for (long i = 0; i < F; i++) {
        float s = 0;
        for (int c = 0; c < a->ch; c++) s += a->data[i * a->ch + c];
        dry[i] = s / a->ch;
    }
    // process in chunks to bound stack usage
    long off = 0;
    while (off < F) {
        int n = (int)(F - off < 4096 ? F - off : 4096);
        mp_block(st, dry + off, n, p, out + off * 2);
        off += n;
    }

    mp_destroy(st); free(dry);
    free(a->data);
    a->data = out;
    a->ch = 2;
    return 0;
}
