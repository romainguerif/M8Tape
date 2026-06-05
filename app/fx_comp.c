// fx_comp.c - COMPRESSOR: a feed-forward, log-domain dynamic range compressor.
// Classic studio design (Giannoulis/Massberg/Reiss tutorial topology): a peak
// detector feeds a dB-domain gain computer with a soft knee; the resulting
// gain-reduction (in dB) is smoothed by an attack/release "branched" peak
// follower, then converted back to linear, scaled by makeup, and applied to the
// dry signal. A MIX control blends compressed with dry for parallel (NY)
// compression. Mono in -> the same processed signal is copied to both outputs.
//
// Built on the generic H3000 engine — same shape as fx_micropitch.c: a state
// struct, create/block/destroy, and one `const H3kAlgoDef comp_def`.
//
// Signal flow (per sample):
//   x ---> |.| --> 20log10 --> [gain computer, soft knee] --> gr_dB (<=0)
//                                                              |
//                          [attack/release smoothing] <--------+
//                                       |
//                                  gr_smooth_dB
//                                       |
//   x --------------------------------> (*) <-- 10^((gr_smooth+makeup)/20)
//                                       |
//                                   compressed --> MIX with x --> outL = outR
#include "h3000.h"
#include "dsputil.h"   // (shared DSP header; ballistics implemented locally)
#include <stdlib.h>
#include <math.h>

// Parameter indices — must match the `params[]` order in the def below.
enum { P_THRESH, P_RATIO, P_ATTACK, P_RELEASE, P_KNEE, P_MAKEUP, P_MIX };

// Detector floor: levels below this (in linear) read as DB_FLOOR dB. -120 dB is
// well under any musical signal yet keeps log10 finite for true silence.
#define LIN_FLOOR   1.0e-6f
#define DB_FLOOR    (-120.0f)
// Never let the smoothed gain reduction exceed this magnitude (keeps the linear
// gain bounded away from 0 and the math well-conditioned).
#define GR_MAX_DB   (-80.0f)

typedef struct {
    int   rate;
    // Smoothed gain-reduction envelope, in dB (<= 0). This is the follower state.
    float grStateDb;
    // Cached ballistics: recompute exp() only when the time params actually move.
    float aAttack, aRelease;   // one-pole coeffs = exp(-1/(t*rate))
    float lastAttackMs, lastReleaseMs;
} CompState;

static float db_to_lin(float db) { return powf(10.0f, db * 0.05f); }       // 10^(db/20)
static float lin_to_db(float lin) {
    if (lin < LIN_FLOOR) return DB_FLOOR;
    return 20.0f * log10f(lin);
}

// One-pole smoothing coeff for a time constant `ms` at `rate` Hz: exp(-1/(t*fs)).
// Guards tiny/zero times (coeff -> 0 = instantaneous) so attack can be very fast
// without a divide-by-zero, and keeps the result in [0,1).
static float time_coeff(float ms, int rate) {
    if (ms <= 0.0f) return 0.0f;
    float t = ms * 0.001f;                 // ms -> seconds
    float c = expf(-1.0f / (t * (float)rate));
    if (!(c >= 0.0f)) c = 0.0f;            // NaN-safe lower clamp
    if (c > 0.99999f) c = 0.99999f;        // never a true integrator
    return c;
}

// dB-domain soft-knee gain computer. Returns the gain reduction in dB (<= 0)
// for an input level `levelDb`, given threshold/ratio/knee. `slope` = 1/ratio-1
// (<= 0). Three regions: below the knee (no reduction), inside the knee
// (quadratic interpolation), above the knee (linear with `slope`).
static float gain_computer_db(float levelDb, float threshDb, float slope,
                              float kneeDb) {
    float overshoot = levelDb - threshDb;
    float kneeHalf  = 0.5f * kneeDb;
    if (kneeDb > 0.0f) {
        if (overshoot <= -kneeHalf) return 0.0f;
        if (overshoot <   kneeHalf) {
            float t = overshoot + kneeHalf;        // 0 .. kneeDb
            return 0.5f * slope * (t * t) / kneeDb;
        }
    } else {
        // Hard knee.
        if (overshoot <= 0.0f) return 0.0f;
    }
    return slope * overshoot;
}

static void *comp_create(int rate) {
    CompState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->rate = rate > 0 ? rate : 48000;
    s->grStateDb = 0.0f;       // start at unity (no reduction)
    s->lastAttackMs = s->lastReleaseMs = -1.0f;   // force first-block compute
    return s;
}

static void comp_block(void *st, const float *dry, int n, const float *p,
                       float *outLR) {
    CompState *s = (CompState *)st;

    // --- read & clamp params (defensive: the engine clamps too) -------------
    float threshDb = p[P_THRESH];
    if (threshDb < -60.0f) threshDb = -60.0f; if (threshDb > 0.0f) threshDb = 0.0f;

    float ratio = p[P_RATIO];
    if (!(ratio >= 1.0f)) ratio = 1.0f;            // NaN-safe; ratio < 1 = bypass-ish
    if (ratio > 20.0f) ratio = 20.0f;
    // slope = 1/ratio - 1, in (-1 .. 0]. At ratio==1 slope==0 => no compression.
    float slope = 1.0f / ratio - 1.0f;

    float kneeDb = p[P_KNEE];
    if (kneeDb < 0.0f) kneeDb = 0.0f; if (kneeDb > 24.0f) kneeDb = 24.0f;

    float makeupDb = p[P_MAKEUP];
    if (makeupDb < 0.0f) makeupDb = 0.0f; if (makeupDb > 24.0f) makeupDb = 24.0f;

    float mix = p[P_MIX];
    if (!(mix >= 0.0f)) mix = 0.0f; if (mix > 1.0f) mix = 1.0f;

    // --- ballistics: recompute coeffs only when attack/release changed ------
    float attackMs = p[P_ATTACK], releaseMs = p[P_RELEASE];
    if (attackMs != s->lastAttackMs) {
        s->aAttack = time_coeff(attackMs, s->rate);
        s->lastAttackMs = attackMs;
    }
    if (releaseMs != s->lastReleaseMs) {
        s->aRelease = time_coeff(releaseMs, s->rate);
        s->lastReleaseMs = releaseMs;
    }
    float aA = s->aAttack, aR = s->aRelease;

    float makeupLin = db_to_lin(makeupDb);
    float grState = s->grStateDb;
    if (!isfinite(grState)) grState = 0.0f;

    for (int i = 0; i < n; i++) {
        float x = dry[i];
        if (!isfinite(x)) x = 0.0f;

        // Detector: instantaneous peak in dB.
        float ax = fabsf(x);
        float levelDb = lin_to_db(ax);

        // Gain computer -> target gain reduction in dB (<= 0).
        float targetDb = gain_computer_db(levelDb, threshDb, slope, kneeDb);
        if (targetDb > 0.0f) targetDb = 0.0f;          // safety
        if (targetDb < GR_MAX_DB) targetDb = GR_MAX_DB;

        // Branched peak smoothing on the reduction envelope. More-negative
        // target = more reduction = "attack"; recovering toward 0 = "release".
        // (Matches the reference detector's `if (in < state) attack else release`.)
        if (targetDb < grState)
            grState = aA * grState + (1.0f - aA) * targetDb;
        else
            grState = aR * grState + (1.0f - aR) * targetDb;
        if (!isfinite(grState)) grState = 0.0f;        // denormal/NaN flush

        // Apply: compressor gain (reduction) + makeup, in linear.
        float gain = db_to_lin(grState) * makeupLin;
        if (!isfinite(gain)) gain = makeupLin;

        float comp = x * gain;
        float out  = mix * comp + (1.0f - mix) * x;    // parallel/NY blend
        if (!isfinite(out)) out = 0.0f;

        outLR[i * 2 + 0] = out;     // mono -> both channels
        outLR[i * 2 + 1] = out;
    }

    s->grStateDb = grState;
}

static void comp_destroy(void *st) {
    free(st);
}

const H3kAlgoDef comp_def = {
    .name = "COMPRESSOR",
    .category = "STUDIO",
    .nparams = 7,
    .params = {
        { "THRESHOLD", -60.0f,  0.0f,  1.0f,  -18.0f, PK_DB,      0 },
        { "RATIO",       1.0f, 20.0f,  0.5f,    4.0f, PK_FLOAT,   0 },
        { "ATTACK",      0.1f, 200.0f, 1.0f,    10.0f, PK_MS,     0 },
        { "RELEASE",     5.0f, 1000.0f,5.0f,   120.0f, PK_MS,     0 },
        { "KNEE",        0.0f, 24.0f,  1.0f,     6.0f, PK_DB,     0 },
        { "MAKEUP",      0.0f, 24.0f,  1.0f,     0.0f, PK_DB,     0 },
        { "MIX",         0.0f,  1.0f,  0.05f,    1.0f, PK_PERCENT,0 },
    },
    .create = comp_create,
    .block = comp_block,
    .destroy = comp_destroy,
};
