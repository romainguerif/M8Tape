// fx_micropitch.c - MicroPitch: two finely-detuned pitch voices, each delayed
// and fed back, panned hard L/R. The original H3000 MicroPitch character.
// This is the reference module: copy its shape for new algorithms — a state
// struct, create/block/destroy, and one `const H3kAlgoDef <name>_def`.
#include "h3000.h"
#include "pitchcore.h"
#include "dsputil.h"
#include <stdlib.h>

// Parameter indices (must match the `params[]` order in the def below).
enum { P_CENTS_A, P_DELAY_A, P_CENTS_B, P_DELAY_B, P_FEEDBACK, P_MIX, P_SPLICE };

typedef struct {
    PitchVoice *va, *vb;
    DelayLine   da, db;
    float       lastA, lastB;
    int         rate;
} MpState;

static void *mp_create(int rate) {
    MpState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->rate = rate > 0 ? rate : 48000;
    s->va = pv_create(s->rate);
    s->vb = pv_create(s->rate);
    if (!s->va || !s->vb ||
        !dl_init(&s->da, s->rate, 1010.0f) || !dl_init(&s->db, s->rate, 1010.0f)) {
        pv_destroy(s->va); pv_destroy(s->vb); dl_free(&s->da); dl_free(&s->db); free(s);
        return NULL;
    }
    return s;
}

static void mp_block(void *st, const float *dry, int n, const float *p, float *outLR) {
    MpState *s = (MpState *)st;
    float fb = p[P_FEEDBACK]; if (fb < 0) fb = 0; if (fb > 0.95f) fb = 0.95f;
    float mix = p[P_MIX];     if (mix < 0) mix = 0; if (mix > 1) mix = 1;
    int sp = (int)(p[P_SPLICE] + 0.5f); if (sp < 0) sp = 0; if (sp >= SPLICE_COUNT) sp = SPLICE_COUNT - 1;
    pv_set_cents(s->va, p[P_CENTS_A]); pv_set_splice(s->va, sp);
    pv_set_cents(s->vb, p[P_CENTS_B]); pv_set_splice(s->vb, sp);
    for (int i = 0; i < n; i++) {
        float in = dry[i];
        float pa = pv_process(s->va, in + fb * s->lastA);
        dl_write(&s->da, pa);
        float wa = dl_read_ms(&s->da, p[P_DELAY_A], s->rate);
        s->lastA = wa;
        float pb = pv_process(s->vb, in + fb * s->lastB);
        dl_write(&s->db, pb);
        float wb = dl_read_ms(&s->db, p[P_DELAY_B], s->rate);
        s->lastB = wb;
        outLR[i * 2 + 0] = mix * wa + (1.0f - mix) * in;
        outLR[i * 2 + 1] = mix * wb + (1.0f - mix) * in;
    }
}

static void mp_destroy(void *st) {
    MpState *s = (MpState *)st;
    if (!s) return;
    pv_destroy(s->va); pv_destroy(s->vb);
    dl_free(&s->da); dl_free(&s->db);
    free(s);
}

const H3kAlgoDef micropitch_def = {
    .name = "MICROPITCH",
    .nparams = 7,
    .params = {
        { "PITCH A (L)", -50,   50,   1,    -9,   PK_CENTS,   0 },
        { "DELAY A",       0, 1000,   5,    15,   PK_MS,      0 },
        { "PITCH B (R)", -50,   50,   1,    11,   PK_CENTS,   0 },
        { "DELAY B",       0, 1000,   5,    25,   PK_MS,      0 },
        { "FEEDBACK",      0, 0.95f, 0.05f, 0,    PK_PERCENT, 0 },
        { "MIX",           0,    1,  0.05f, 0.5f, PK_PERCENT, 0 },
        { "SPLICE",        0,    3,   1,     0,   PK_CHOICE,  PITCH_SPLICE_CHOICES },
    },
    .create = mp_create,
    .block = mp_block,
    .destroy = mp_destroy,
};
