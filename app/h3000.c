// h3000.c - H3000-style DSP. Time-domain pitch core (2-tap moving delay line +
// crossfade, à la Faust transpose / H910) + the MicroPitch algorithm.
// Block-based with persistent state, so it can run a real-time preview while
// parameters change live.
#include "h3000.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

const char *h3000_splice_name(int mode) {
    switch (mode) {
        case SPLICE_H910:   return "H910";
        case SPLICE_H949_1: return "H949-1";
        case SPLICE_H949_2: return "H949-2";
        case SPLICE_MODERN: return "MODERN";
        default:            return "H910";
    }
}

// --- time-domain pitch voice -------------------------------------------------
// 2-tap moving delay line + crossfade (a faithful port of the Faust `transpose`:
// taps at delay base+d and base+d+P, gains min(d/x,1) and 1-min(d/x,1), with d
// ramping by (1-ratio) per sample and wrapping at the window P). H910 keeps P
// fixed; the H949/Modern modes re-pick P at every splice by normalized
// cross-correlation so the two crossfaded taps are phase-aligned (de-glitch).
typedef struct {
    float *buf;
    int    n;
    int    wr;
    float  phase;          // ramp 0..P
    float  rate;
    float  base;           // fixed read base (samples) — keeps reads off wr
    float  Wmax;           // max window / search ceiling (samples)
    float  P;              // current window = splice interval (samples), adaptive
    float  Pmin, Pmax;     // de-glitch period search bounds (samples)
    float  x;              // crossfade length (samples)
    float  ratio;          // 2^(cents/1200) — updated live each block
    int    splice;         // SpliceMode
    int    deglitch;       // derived: 0 for H910, 1 otherwise
    float  corrL;          // correlation window (samples)
    int    corrStep;       // lag step for the period search
    float  driftDepth;     // LC-clock drift depth (cents); 0 = none
    float  lfo1, lfo2, dlfo1, dlfo2;  // two slow incommensurate drift LFOs
} PitchVoice;

static float pv_read(const PitchVoice *v, float delay) {
    float pos = (float)v->wr - delay;
    while (pos < 0) pos += v->n;
    int i0 = (int)pos;
    float frac = pos - i0;
    int i1 = i0 + 1; if (i1 >= v->n) i1 -= v->n;
    return v->buf[i0] * (1.0f - frac) + v->buf[i1] * frac;
}
// integer-delay tap, for the correlation search
static float pv_at(const PitchVoice *v, int delay) {
    int pos = v->wr - delay;
    while (pos < 0)     pos += v->n;
    while (pos >= v->n) pos -= v->n;
    return v->buf[pos];
}
// configure the splice character (cheap; called per block so the UI can change
// the mode live). H910 forces the fixed window; the rest leave P adaptive.
static void pv_set_mode(PitchVoice *v, int splice) {
    v->splice = splice;
    float r = v->rate;
    switch (splice) {
        case SPLICE_H949_1:
            v->deglitch = 1; v->x = 0.012f * r; v->corrL = 0.006f * r; v->corrStep = 6; v->driftDepth = 1.0f; break;
        case SPLICE_H949_2:
            v->deglitch = 1; v->x = 0.016f * r; v->corrL = 0.009f * r; v->corrStep = 3; v->driftDepth = 0.0f; break;
        case SPLICE_MODERN:
            v->deglitch = 1; v->x = 0.020f * r; v->corrL = 0.012f * r; v->corrStep = 2; v->driftDepth = 0.0f; break;
        case SPLICE_H910:
        default:
            v->deglitch = 0; v->x = 0.012f * r; v->driftDepth = 3.0f; v->P = v->Wmax; break;
    }
}
// de-glitch: pick the splice interval P in [Pmin,Pmax] that maximizes the
// normalized cross-correlation between the buffer segment the incoming tap will
// read (around `base`) and the one the outgoing tap reads (around `base+P`).
// That lands the splice on a near-integer number of pitch periods, so the two
// crossfaded copies are nearly identical → minimal cancellation.
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
    // refine to single-sample resolution around the coarse winner
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
static void pv_init(PitchVoice *v, float rate) {
    v->rate = rate;
    v->Wmax = 0.050f * rate;
    v->base = v->Wmax;          // read well behind wr; constant buffer geometry
    v->Pmin = 0.005f * rate;    // 5..50 ms period search (per the H949 patent)
    v->Pmax = v->Wmax;
    v->P    = v->Wmax;
    v->x    = 0.012f * rate;
    v->ratio = 1.0f;
    v->corrL = 0.008f * rate; v->corrStep = 4;
    v->splice = SPLICE_H910; v->deglitch = 0; v->driftDepth = 0.0f;
    v->lfo1 = 0.0f; v->lfo2 = 0.0f;
    v->dlfo1 = 2.0f * (float)M_PI * 0.7f / rate;   // ~0.7 Hz
    v->dlfo2 = 2.0f * (float)M_PI * 3.1f / rate;   // ~3.1 Hz (incommensurate)
    // max access = base + 2*Pmax (read taps) and base + Pmax + corrL (search)
    v->n = (int)(v->base + 2.0f * v->Pmax + 0.012f * rate) + 256;
    v->buf = calloc(v->n, sizeof(float));
    v->wr = 0; v->phase = 0.0f;
}
static void pv_free(PitchVoice *v) { free(v->buf); v->buf = NULL; }
static float pv_process(PitchVoice *v, float in) {
    v->buf[v->wr] = in;
    v->wr++; if (v->wr >= v->n) v->wr = 0;

    float d = v->phase;
    float xeff = v->x;                         // crossfade must fit inside P
    if (xeff > 0.45f * v->P) xeff = 0.45f * v->P;
    if (xeff < 1.0f) xeff = 1.0f;
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
    v->phase += (1.0f - ratio);
    if (v->phase >= v->P) {                     // splice (downshift)
        v->phase -= v->P;
        if (v->deglitch) v->P = pv_best_period(v);
    } else if (v->phase < 0.0f) {               // splice (upshift)
        if (v->deglitch) v->P = pv_best_period(v);
        v->phase += v->P;
    }
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
    int sp = p->splice; if (sp < 0) sp = 0; if (sp >= SPLICE_COUNT) sp = SPLICE_COUNT - 1;
    pv_set_mode(&st->va, sp);
    pv_set_mode(&st->vb, sp);
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
