// fx_dualshift.c - DUAL SHIFT: two INDEPENDENT pitch shifters, voice A -> left,
// voice B -> right, each with its own delay and feedback, a shared wet/dry mix
// and a shared SPLICE character. This is the Eventide H3000 "Dual Shift"
// (algorithm #102): the original hardware runs two completely independent
// pitch shifters, the first on the left in/out, the second on the right in/out,
// each with Pitch / Delay / Feedback / Mix (Eventide H3000 Factory forum, and
// the research doc: "Dual Shift = 2 pitch shifters indpendants, recree 2x H910").
//
// FIDELITY NOTES (honest, since the real TMS32010 microcode was never published):
//  - FAITHFUL to function: the per-voice topology (independent shifter ->
//    delay -> feedback -> pan hard L/R) matches the documented Dual Shift module
//    list (Left/Right Pitch, Delay, Feedback, Mix).
//  - FAITHFUL range: PITCH spans +/-24 semitones (two octaves up/down), which is
//    the H3000's stated pitch-shift range; DELAY max 1500 ms matches the H3000
//    "Long Digiplex" 1.5 s delay; feedback loop is independent per voice.
//  - The actual pitch GRAIN (converter colour, clock drift, exact splice timing)
//    lives in pitchcore.c's H910/H949 splice modes; this module just exposes the
//    SPLICE selector and wires it to both voices, where large shifts make the
//    H949 de-glitch clearly audible.
//  - MODELED-from-function: the dry/wet equal-gain crossfade and the exact
//    feedback scaling are our own choices; the hardware's internal gain staging
//    is undocumented.
//
// This module is mono-in -> interleaved-stereo-out: the single dry source feeds
// BOTH voices (the standard way to drive Dual Shift from a mono signal), voice A
// is panned hard left, voice B hard right. No global/static mutable state.
#include "h3000.h"
#include "pitchcore.h"
#include "dsputil.h"
#include <stdlib.h>
#include <math.h>

// Parameter indices (MUST match the `params[]` order in the def below).
enum { P_PITCH_A, P_DELAY_A, P_PITCH_B, P_DELAY_B, P_FEEDBACK, P_MIX, P_SPLICE };

#define DS_MAX_DELAY_MS 1500.0f

typedef struct {
    PitchVoice *va, *vb;   // two independent time-domain pitch shifters
    DelayLine   da, db;    // per-voice delay line (post-shift)
    float       lastA;     // last delayed sample of voice A (feedback tap)
    float       lastB;     // last delayed sample of voice B (feedback tap)
    int         rate;
} DsState;

// Flush a non-finite (NaN/Inf) or sub-audible denormal value to a clean 0.
// Keeps the feedback loop from latching onto garbage and silences denormal
// CPU spikes on quiet tails.
static inline float ds_clean(float x) {
    if (!isfinite(x)) return 0.0f;
    if (x > -1e-20f && x < 1e-20f) return 0.0f;
    return x;
}

static void *ds_create(int rate) {
    DsState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->rate = rate > 0 ? rate : 48000;
    s->va = pv_create(s->rate);
    s->vb = pv_create(s->rate);
    // +10 ms headroom so a max-delay read never runs off the end of the line.
    if (!s->va || !s->vb ||
        !dl_init(&s->da, s->rate, DS_MAX_DELAY_MS + 10.0f) ||
        !dl_init(&s->db, s->rate, DS_MAX_DELAY_MS + 10.0f)) {
        pv_destroy(s->va); pv_destroy(s->vb);
        dl_free(&s->da);   dl_free(&s->db);
        free(s);
        return NULL;
    }
    return s;
}

static void ds_block(void *st, const float *dry, int n, const float *p, float *outLR) {
    DsState *s = (DsState *)st;

    // Read params live each block (so the preview hears edits immediately).
    float fb = p[P_FEEDBACK];
    if (fb < 0.0f) fb = 0.0f;
    if (fb > 0.95f) fb = 0.95f;        // clamp strictly < 1.0 -> stable loop
    float mix = p[P_MIX];
    if (mix < 0.0f) mix = 0.0f;
    if (mix > 1.0f) mix = 1.0f;

    float dlyA = p[P_DELAY_A];
    if (dlyA < 0.0f) dlyA = 0.0f;
    if (dlyA > DS_MAX_DELAY_MS) dlyA = DS_MAX_DELAY_MS;
    float dlyB = p[P_DELAY_B];
    if (dlyB < 0.0f) dlyB = 0.0f;
    if (dlyB > DS_MAX_DELAY_MS) dlyB = DS_MAX_DELAY_MS;

    int sp = (int)(p[P_SPLICE] + 0.5f);
    if (sp < 0) sp = 0;
    if (sp >= SPLICE_COUNT) sp = SPLICE_COUNT - 1;

    // PITCH params are SEMITONES; pitchcore wants CENTS -> *100.
    pv_set_cents(s->va, p[P_PITCH_A] * 100.0f); pv_set_splice(s->va, sp);
    pv_set_cents(s->vb, p[P_PITCH_B] * 100.0f); pv_set_splice(s->vb, sp);

    float dryWet = 1.0f - mix;

    for (int i = 0; i < n; i++) {
        float in = ds_clean(dry[i]);

        // --- Voice A -> left ------------------------------------------------
        float pa = pv_process(s->va, ds_clean(in + fb * s->lastA));
        dl_write(&s->da, pa);
        float wa = dl_read_ms(&s->da, dlyA, s->rate);
        s->lastA = ds_clean(wa);

        // --- Voice B -> right -----------------------------------------------
        float pb = pv_process(s->vb, ds_clean(in + fb * s->lastB));
        dl_write(&s->db, pb);
        float wb = dl_read_ms(&s->db, dlyB, s->rate);
        s->lastB = ds_clean(wb);

        outLR[i * 2 + 0] = mix * wa + dryWet * in;
        outLR[i * 2 + 1] = mix * wb + dryWet * in;
    }
}

static void ds_destroy(void *st) {
    DsState *s = (DsState *)st;
    if (!s) return;
    pv_destroy(s->va); pv_destroy(s->vb);
    dl_free(&s->da);   dl_free(&s->db);
    free(s);
}

const H3kAlgoDef dualshift_def = {
    .name = "DUAL SHIFT",
    .nparams = 7,
    .params = {
        { "PITCH A (L)", -24,  24,    1,    -12,  PK_SEMI,    0 },
        { "DELAY A",       0, 1500,  10,      0,  PK_MS,      0 },
        { "PITCH B (R)", -24,  24,    1,      7,  PK_SEMI,    0 },
        { "DELAY B",       0, 1500,  10,      0,  PK_MS,      0 },
        { "FEEDBACK",      0, 0.9f,  0.05f,   0,  PK_PERCENT, 0 },
        { "MIX",           0,    1,  0.05f, 0.5f, PK_PERCENT, 0 },
        { "SPLICE",        0,    3,   1,      1,  PK_CHOICE,  PITCH_SPLICE_CHOICES },
    },
    .create = ds_create,
    .block = ds_block,
    .destroy = ds_destroy,
};
