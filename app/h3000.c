// h3000.c - the generic H3000 engine: algorithm registry + streaming wrapper +
// whole-sample render + UI helpers. Algorithms themselves live in fx_*.c.
#include "h3000.h"
#include <stdlib.h>
#include <math.h>
#include <stdio.h>

// --- algorithm registry ------------------------------------------------------
// Each fx_*.c exposes one `const H3kAlgoDef <name>_def`. List them here; the
// order is the order shown in the picker. (This is the only shared edit when a
// new effect module is added.)
extern const H3kAlgoDef micropitch_def;
extern const H3kAlgoDef dualshift_def;
extern const H3kAlgoDef diatonic_def;
extern const H3kAlgoDef ultratap_def;
extern const H3kAlgoDef delay_def;
extern const H3kAlgoDef banddelay_def;
extern const H3kAlgoDef reverb_def;
extern const H3kAlgoDef reverse_def;
extern const H3kAlgoDef phaser_def;
extern const H3kAlgoDef stutter_def;
extern const H3kAlgoDef string_def;
extern const H3kAlgoDef vocoder_def;

const H3kAlgoDef *const h3k_algos[] = {
    &micropitch_def,
    &dualshift_def,
    &diatonic_def,
    &ultratap_def,
    &delay_def,
    &banddelay_def,
    &reverb_def,
    &reverse_def,
    &phaser_def,
    &stutter_def,
    &string_def,
    &vocoder_def,
};
const int h3k_algo_count = (int)(sizeof(h3k_algos) / sizeof(h3k_algos[0]));

// --- generic streaming engine ------------------------------------------------
struct H3kEngine { const H3kAlgoDef *def; void *st; };

H3kEngine *h3k_create(int algo, int rate) {
    if (algo < 0 || algo >= h3k_algo_count) algo = 0;
    H3kEngine *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->def = h3k_algos[algo];
    e->st = e->def->create(rate > 0 ? rate : 48000);
    if (!e->st) { free(e); return NULL; }
    return e;
}
void h3k_block(H3kEngine *e, const float *dry, int n, const float *params, float *outLR) {
    e->def->block(e->st, dry, n, params, outLR);
}
void h3k_destroy(H3kEngine *e) {
    if (!e) return;
    if (e->def && e->st) e->def->destroy(e->st);
    free(e);
}

// --- whole-sample destructive render (mono-summed in -> stereo out) ----------
int h3k_render(Audio *a, int algo, const float *params) {
    if (!a->data || a->frames < 1 || a->ch < 1) return -1;
    long F = a->frames;
    float *dry = malloc((size_t)F * sizeof(float));
    float *out = malloc((size_t)F * 2 * sizeof(float));
    H3kEngine *e = h3k_create(algo, a->rate);
    if (!dry || !out || !e) { free(dry); free(out); h3k_destroy(e); return -1; }

    for (long i = 0; i < F; i++) {
        float s = 0;
        for (int c = 0; c < a->ch; c++) s += a->data[i * a->ch + c];
        dry[i] = s / a->ch;
    }
    long off = 0;
    while (off < F) {
        int n = (int)(F - off < 4096 ? F - off : 4096);
        h3k_block(e, dry + off, n, params, out + off * 2);
        off += n;
    }
    h3k_destroy(e); free(dry);
    free(a->data);
    a->data = out;
    a->ch = 2;
    return 0;
}

// --- UI helpers --------------------------------------------------------------
void h3k_defaults(int algo, float *params) {
    if (algo < 0 || algo >= h3k_algo_count) return;
    const H3kAlgoDef *d = h3k_algos[algo];
    for (int i = 0; i < d->nparams; i++) params[i] = d->params[i].def;
}
static int choice_count(const ParamSpec *ps) {
    int n = 0; if (ps->choices) while (ps->choices[n]) n++; return n;
}
float h3k_adjust(const ParamSpec *ps, float v, int dir) {
    if (ps->kind == PK_CHOICE) {
        int n = choice_count(ps); if (n < 1) n = 1;
        int idx = (int)(v + 0.5f) + dir;
        idx = ((idx % n) + n) % n;     // wrap
        return (float)idx;
    }
    v += dir * ps->step;
    if (v < ps->min) v = ps->min;
    if (v > ps->max) v = ps->max;
    return v;
}
void h3k_format(const ParamSpec *ps, float v, char *out, int sz) {
    switch (ps->kind) {
        case PK_CENTS:   snprintf(out, sz, "%+d CENTS", (int)lroundf(v)); break;
        case PK_SEMI:    snprintf(out, sz, "%+d ST", (int)lroundf(v)); break;
        case PK_MS:      snprintf(out, sz, "%d MS", (int)lroundf(v)); break;
        case PK_PERCENT: snprintf(out, sz, "%d%%", (int)lroundf(v * 100.0f)); break;
        case PK_HZ:      snprintf(out, sz, "%d HZ", (int)lroundf(v)); break;
        case PK_DB:      snprintf(out, sz, "%+d DB", (int)lroundf(v)); break;
        case PK_INT:     snprintf(out, sz, "%d", (int)lroundf(v)); break;
        case PK_CHOICE: {
            int n = choice_count(ps);
            int idx = (int)(v + 0.5f); if (idx < 0) idx = 0; if (idx >= n) idx = n > 0 ? n - 1 : 0;
            snprintf(out, sz, "%s", (ps->choices && n > 0) ? ps->choices[idx] : "-");
            break;
        }
        default:         snprintf(out, sz, "%.2f", v); break;
    }
}
