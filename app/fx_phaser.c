// fx_phaser.c - PHASER: the classic analog-style swept-notch phaser.
//
// FIDELITY NOTES (researched against Julius O. Smith III, "Phasing with
// First-Order Allpass Filters", CCRMA/Stanford & "Physical Audio Signal
// Processing"; and the WolfSound allpass reference):
//
//   A classic analog phaser (MXR Phase 90, Small Stone, ...) is a CHAIN of
//   first-order all-pass stages whose single break frequency is swept by an
//   LFO. Summing the all-pass chain output with the dry signal produces the
//   characteristic moving NOTCHES: each pair of all-pass stages contributes
//   one notch, so an N-stage chain yields ~N/2 notches that sweep up and down
//   with the LFO. A FEEDBACK path from the last stage back into the first
//   sharpens and emphasises the notches ("resonance"/"colour" on real units).
//   Wet/dry MIX sets the depth of the notches (50% = deepest, true nulls).
//
//   FAITHFUL here:
//     * First-order all-pass stages, exact transfer function
//         H(z) = (a + z^-1) / (1 + a*z^-1),  y = a*x + x_prev - a*y_prev
//       with the bilinear break-frequency mapping
//         a = (tan(pi*fc/fs) - 1) / (tan(pi*fc/fs) + 1).
//       (CCRMA eq. for H_AP1; WolfSound eq. 3 & 9. This is the textbook
//       analog-modelled stage, not an approximation.)
//     * fc swept by a SINE LFO, log-interpolated between a min and a max
//       corner frequency (sweeping in octaves matches how the ear and the
//       analog RC sweep behave far better than linear Hz).
//     * Series chain + dry sum (notches) + feedback last->first + wet/dry mix.
//     * Stereo: the right channel's LFO runs 90 degrees ahead of the left so
//       the notches sweep out of phase across the field (classic stereo
//       phaser width). Input is mono, upmixed by the effect.
//
//   MODELED / adapted (honest):
//     * STAGES is user-selectable (2..8, even) rather than a fixed 4. Real
//       pedals have a fixed stage count; exposing it lets one box cover
//       Phase-90 (4) through thicker 6/8-stage voicings.
//     * DEPTH controls the *sweep span* (how wide the notches travel), mapped
//       onto a fixed musical centre (~500 Hz) so min/max stay in the audio
//       band at any depth. The hardware's depth is the LFO amplitude into the
//       OTA/FET sweep; this is the same idea in the frequency domain.
//     * A one-pole smoother de-zippers the swept coefficient per block edge so
//       live parameter edits in the preview never click.
//
// Engine contract: mono dry[n] in, interleaved-stereo outLR[2n] out. All state
// is allocated in create() and freed in destroy(); no global/static mutable
// state. Feedback is clamped strictly < 1, every recursive state is denormal/
// NaN guarded, and the feedback node is soft-clipped so a high-feedback,
// in-phase sweep stays bounded and click-free.
#include "h3000.h"
#include "dsputil.h"
#include <stdlib.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define PH_TWO_PI  6.28318530717958647692f

#define PH_MAX_STAGES   8          // hard ceiling for the allpass chain
#define PH_CENTRE_HZ    500.0f     // geometric centre the sweep span opens around
#define PH_SWEEP_OCT    2.6f       // +/- octaves spanned at DEPTH = 1.0

// Parameter indices (MUST match the params[] order in the def below).
enum { P_RATE, P_DEPTH, P_STAGES, P_FEEDBACK, P_MIX };

// One first-order all-pass stage: a single sample of x and y history.
typedef struct { float xprev, yprev; } APStage;

// Per-channel phaser voice: its own allpass chain + feedback memory.
typedef struct {
    APStage stage[PH_MAX_STAGES];
    float   fb_last;               // last stage output fed back into the chain
    float   a_smooth;              // de-zippered allpass coefficient
} PhaserVoice;

typedef struct {
    PhaserVoice L, R;
    double      phase;             // LFO phase accumulator, [0,1) cycles
    int         rate;
} PhState;

// Flush denormals / NaNs to zero so the feedback loop never stalls on subnormal
// floats (large CPU penalty) or latches a NaN into the recursive state.
static inline float ph_san(float x) {
    if (!(x == x)) return 0.0f;                          // NaN
    if (x > -1.0e-20f && x < 1.0e-20f) return 0.0f;      // denormal -> 0
    return x;
}

// Smooth bounded saturation for the feedback node: ~linear for |x|<1, asymptotes
// to +/-1.4. Tames a high-feedback sweep without a hard-clip click.
static inline float ph_softclip(float x) {
    const float lim = 1.4f;
    float a = x / lim;
    return lim * (a / (1.0f + 0.28f * a * a));
}

// Bilinear break-frequency -> first-order all-pass coefficient.
//   a = (tan(pi*fc/fs) - 1) / (tan(pi*fc/fs) + 1)
// Keep fc safely inside (0, Nyquist) so tan() never blows up.
static float ph_coeff(float fc, int rate) {
    float ny = 0.49f * (float)rate;
    if (fc < 20.0f) fc = 20.0f;
    if (fc > ny)    fc = ny;
    float t = tanf((float)M_PI * fc / (float)rate);
    return (t - 1.0f) / (t + 1.0f);
}

// Run one sample through a voice's allpass chain (nst stages) with feedback,
// sum with dry to form the notches, return the wet sample. `a` is the (already
// de-zippered) per-stage coefficient, shared by every stage in the chain.
static inline float ph_voice(PhaserVoice *v, float in, float a, int nst, float fb) {
    // Inject feedback from the previous block's last-stage output.
    float x = in + fb * v->fb_last;
    x = ph_san(x);
    for (int k = 0; k < nst; k++) {
        APStage *s = &v->stage[k];
        // y = a*x + x_prev - a*y_prev   (unity-gain first-order allpass)
        float y = a * x + s->xprev - a * s->yprev;
        s->xprev = x;
        s->yprev = ph_san(y);
        x = y;
    }
    v->fb_last = ph_softclip(ph_san(x));   // bound the feedback memory
    return x;                              // allpass-chain output (the "wet" path)
}

static void *ph_create(int rate) {
    PhState *s = calloc(1, sizeof(*s));    // zero-inits all stage history & phase
    if (!s) return NULL;
    s->rate = rate > 0 ? rate : 48000;
    // Seed the smoothers at the geometric centre so the very first block does
    // not lurch from a=0 to the swept value.
    float a0 = ph_coeff(PH_CENTRE_HZ, s->rate);
    s->L.a_smooth = a0;
    s->R.a_smooth = a0;
    return s;
}

static void ph_block(void *st, const float *dry, int n, const float *p, float *outLR) {
    PhState *s = (PhState *)st;
    int rate = s->rate;

    // --- read + clamp live params -------------------------------------------
    float rateHz = p[P_RATE];
    if (rateHz < 0.05f) rateHz = 0.05f;
    if (rateHz > 10.0f) rateHz = 10.0f;

    float depth = p[P_DEPTH];
    if (depth < 0.0f) depth = 0.0f;
    if (depth > 1.0f) depth = 1.0f;

    int nst = (int)(p[P_STAGES] + 0.5f);
    nst &= ~1;                               // force even (notches come in pairs)
    if (nst < 2)             nst = 2;
    if (nst > PH_MAX_STAGES) nst = PH_MAX_STAGES;

    float fb = p[P_FEEDBACK];
    if (fb < 0.0f)  fb = 0.0f;
    if (fb > 0.9f)  fb = 0.9f;               // spec max; strictly < 1 for stability

    float mix = p[P_MIX];
    if (mix < 0.0f) mix = 0.0f;
    if (mix > 1.0f) mix = 1.0f;

    // Sweep span: DEPTH scales the half-width in octaves around PH_CENTRE_HZ.
    // fc_min..fc_max are geometric (the LFO interpolates in log-frequency).
    float half_oct = depth * PH_SWEEP_OCT;
    float fc_min   = PH_CENTRE_HZ * powf(2.0f, -half_oct);
    float fc_max   = PH_CENTRE_HZ * powf(2.0f,  half_oct);
    float log_min  = logf(fc_min);
    float log_span = logf(fc_max) - log_min;

    // Per-sample LFO increment in cycles. R leads L by 90 degrees (0.25 cycle).
    double inc = (double)rateHz / (double)rate;
    // One-pole smoothing coefficient for the swept coefficient (~2 ms glide):
    // de-zippers fast LFO motion and live edits without smearing the sweep.
    float smc = 1.0f - expf(-1.0f / (0.002f * (float)rate));

    for (int i = 0; i < n; i++) {
        float in = dry[i];

        // LFO -> log-frequency -> allpass coefficient, per channel.
        // sin() in [-1,1] mapped to [0,1] then onto the log-frequency span.
        float ph_l = (float)s->phase;
        float ph_r = (float)s->phase + 0.25f;        // +90 degrees
        if (ph_r >= 1.0f) ph_r -= 1.0f;

        float sl = 0.5f + 0.5f * sinf(PH_TWO_PI * ph_l);
        float sr = 0.5f + 0.5f * sinf(PH_TWO_PI * ph_r);

        float fcl = expf(log_min + log_span * sl);
        float fcr = expf(log_min + log_span * sr);

        float a_l = ph_coeff(fcl, rate);
        float a_r = ph_coeff(fcr, rate);

        // De-zipper the coefficients (smooth, bounded changes only).
        s->L.a_smooth += smc * (a_l - s->L.a_smooth);
        s->R.a_smooth += smc * (a_r - s->R.a_smooth);

        float wetL = ph_voice(&s->L, in, s->L.a_smooth, nst, fb);
        float wetR = ph_voice(&s->R, in, s->R.a_smooth, nst, fb);

        // Sum allpass output with dry to form the notches, then wet/dry blend.
        // At mix=0.5 the wet and dry are equal -> deepest cancellation (nulls).
        float yl = (1.0f - mix) * in + mix * wetL;
        float yr = (1.0f - mix) * in + mix * wetR;

        outLR[i * 2 + 0] = ph_san(yl);
        outLR[i * 2 + 1] = ph_san(yr);

        // Advance the shared LFO phase accumulator (no Date/clock).
        s->phase += inc;
        if (s->phase >= 1.0) s->phase -= 1.0;
    }
}

static void ph_destroy(void *st) {
    free(st);
}

const H3kAlgoDef phaser_def = {
    .name = "PHASER",
    .nparams = 5,
    .params = {
        { "RATE",     0.05f, 10.0f, 0.05f, 0.5f, PK_FLOAT,   0 },  // LFO speed, Hz
        { "DEPTH",    0,     1,     0.05f, 0.7f, PK_PERCENT, 0 },  // sweep span
        { "STAGES",   2,     8,     2,     4,    PK_INT,     0 },  // allpass count (even)
        { "FEEDBACK", 0,     0.9f,  0.05f, 0.3f, PK_PERCENT, 0 },  // notch resonance
        { "MIX",      0,     1,     0.05f, 0.5f, PK_PERCENT, 0 },  // wet/dry (0.5=deep)
    },
    .create = ph_create,
    .block = ph_block,
    .destroy = ph_destroy,
};
