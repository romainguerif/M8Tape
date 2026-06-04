// fx_reverb.c - REVERB: a faithful reproduction of the Jon Dattorro (1997)
// plate-class reverberator ("Effect Design, Part 1: Reverberator and Other
// Filters", JAES 45(9), Fig. 1 + Tables 1 & 2). This is the well-known
// figure-eight allpass-loop topology "in the style of Griesinger".
//
// Signal flow (mono in -> stereo out), exactly as in the paper:
//   in -> [bandwidth low-pass] -> [predelay] -> 4 series input-diffusion
//   allpasses -> figure-8 TANK. Each tank half is:
//       modulated decay-diffusion-1 allpass -> delay -> damping low-pass
//       -> decay-diffusion-2 allpass -> delay
//   The two halves cross-feed: each half's modulated allpass is driven by the
//   tank input PLUS the *other* half's final delay output scaled by `decay`.
//   L and R are each the canonical sum of 7 internal taps (Table 2).
//
// FAITHFUL (straight from the paper):
//   - Topology and node ordering of Fig. 1.
//   - Canonical delay-line lengths @ Fs = 29761 Hz: input diffusers 142/107/
//     379/277; tank decay-diffusion-1 672/908 (+excursion); tank delays
//     4453/3720 (L) and 4217/3163 (R); decay-diffusion-2 allpasses 1800/2656.
//   - Table 2 output tap node/offset pairs, all gained by 0.6, signs as printed.
//   - Default coefficients from Table 1: input diffusion 1 = 0.75, input
//     diffusion 2 = 0.625, decay diffusion 1 = 0.70; decay diffusion 2 follows
//     decay+0.15 clamped to [0.25, 0.50]; bandwidth/damping as one-pole LPs.
//   - Sign convention of the decay-diffusion-1 lattice (its multiplier is the
//     *negated* coefficient per Fig. 1, "note sign").
//
// MODELED / adapted (honest notes):
//   - All canonical sample lengths are scaled by (Fs_actual / 29761) so the
//     plate sounds identical at the host rate, then by a user SIZE control.
//   - The paper uses all-pass interpolation for the modulated taps (microtonal,
//     no added damping). We only have linear-interpolating fractional taps
//     (dsputil dl_read_samp), so the slow modulation adds a trace of HF damping
//     -- inaudible at EXCURSION ~16 samples / ~1 Hz, but noted for honesty.
//   - PREDELAY/DECAY/SIZE/DAMP/MIX are exposed as musical controls in place of
//     the paper's raw coefficients; DECAY maps to the tank's decay coefficient.
//
// No global/static mutable state: every buffer is allocated in create() and
// freed in destroy(). Tank is unconditionally stable (decay clamped < 1).
#include "h3000.h"
#include "dsputil.h"
#include <stdlib.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Parameter indices -- MUST match the params[] table order in the def below.
enum { P_PREDELAY, P_DECAY, P_SIZE, P_DAMP, P_MIX };

// Reference design rate at which all canonical lengths are specified (Table 1).
#define DAT_FS 29761.0f
// Canonical delay-line lengths in samples @ DAT_FS (Fig. 1).
#define L_PREDELAY_MAX 200.0f /* ms; paper's predelay is z^(0..inf) */
#define L_IDIFF1A 142
#define L_IDIFF1B 107
#define L_IDIFF2A 379
#define L_IDIFF2B 277
#define L_DDIF1_L 672  /* + excursion */
#define L_DELAY1_L 4453
#define L_DDIF2_L 1800
#define L_DELAY2_L 3720
#define L_DDIF1_R 908  /* + excursion */
#define L_DELAY1_R 4217
#define L_DDIF2_R 2656
#define L_DELAY2_R 3163
// Modulation of the two decay-diffusion-1 taps (Table 1: EXCURSION = 16 samples
// peak; footnote 14: rate on the order of 1 Hz).
#define EXCURSION_SAMP 16.0f
#define MOD_RATE_HZ 0.9f

// Default coefficients (Table 1).
#define IDIFF1_G 0.75f
#define IDIFF2_G 0.625f
#define DDIFF1_G 0.70f
#define TAP_GAIN 0.6f

// Flush denormals / NaNs to zero to keep the recirculating loop clean & cheap.
static inline float fflush_tiny(float x) {
    if (!(x == x)) return 0.0f;          // NaN
    if (x < 1e-20f && x > -1e-20f) return 0.0f;
    return x;
}

typedef struct {
    int   rate;
    float sr_scale;          // Fs_actual / DAT_FS

    // Input chain.
    OnePole   bw;            // bandwidth low-pass (input HF roll-off)
    DelayLine predelay;
    Allpass   id1a, id1b, id2a, id2b;   // 4 series input diffusers (fixed len)

    // Tank delay lines (we own them as DelayLines so we can read the canonical
    // internal tap points required by Table 2).
    DelayLine dd1_l, dd1_r;  // modulated decay-diffusion-1 allpass memory
    DelayLine del1_l, del1_r;
    DelayLine dd2_l, dd2_r;  // decay-diffusion-2 allpass memory
    DelayLine del2_l, del2_r;
    OnePole   damp_l, damp_r;

    // Tank feedback state (the other half's final delay output, fed back).
    float fb_l, fb_r;
    // LFO phase for the two modulated allpasses (quadrature, per the paper).
    float lfo_phase;
    float lfo_inc;
    // Base (already SIZE/rate-scaled) tap lengths for the modulated allpasses,
    // recomputed only when SIZE changes.
    float dd1_l_base, dd1_r_base, last_size;
} RevState;

// Schroeder/Dattorro allpass step over a DelayLine with an externally supplied
// (possibly fractional, modulated) delay length, so we can also tap inside it.
//
// TRUE lossless allpass H(z) = (-g + z^-N)/(1 - g z^-N):
//   w[n]   = in + g*delayed      (recursive part, stored in the line)
//   out    = -g*w[n] + delayed   (numerator on w)
// NOTE: we deliberately do NOT reuse dsputil's ap_process here. That routine
// stores `in + g*delayed` but outputs `-g*in + delayed`, which yields the
// transfer (-g + (1+g^2)z^-N)/(1 - g z^-N) -- magnitude ~1.3..1.8, i.e. NOT a
// true allpass. Harmless in a feed-forward chain, but in the figure-8 feedback
// loop that >1 gain compounds and the tank blows up. So all eight reverb
// allpasses use this corrected (genuinely lossless) form.
static inline float ap_step_dl(DelayLine *line, float d, float g, float in) {
    float delayed = dl_read_samp(line, d);
    float w = in + g * delayed;
    dl_write(line, fflush_tiny(w));
    return -g * w + delayed;
}

// Same corrected allpass over a fixed Allpass buffer (used for the 4 input
// diffusers). We own the buffer via ap_init() but process it ourselves so the
// transfer is a real allpass. Reads/writes the same circular slot -> delay = N.
static inline float ap_step_fixed(Allpass *a, float in) {
    float delayed = a->buf[a->wr];
    float w = in + a->g * delayed;
    a->buf[a->wr] = fflush_tiny(w);
    a->wr++; if (a->wr >= a->n) a->wr = 0;
    return -a->g * w + delayed;
}

// Read a tap inside a delay line at a canonical (rate/size-scaled) sample offset.
static inline float tap(const DelayLine *line, int canonical, float scale) {
    return dl_read_samp(line, (float)canonical * scale);
}

static void rv_recompute_size(RevState *s, float size) {
    // SIZE scales the *tank* lengths around the canonical value. 0 -> half
    // size (tighter, brighter), 1 -> 1.5x (larger, longer). 0.5 == canonical.
    float k = (0.5f + size) * s->sr_scale;     // size 0.5 -> 1.0 * sr_scale
    s->dd1_l_base = (float)L_DDIF1_L * k;
    s->dd1_r_base = (float)L_DDIF1_R * k;
    s->last_size = size;
}

static void *rv_create(int rate) {
    RevState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->rate = rate > 0 ? rate : 48000;
    s->sr_scale = (float)s->rate / DAT_FS;

    // Allpass lengths scale with rate (SIZE is applied to the tank only, by
    // reading shorter/longer; input diffusers use canonical*rate length).
    int id1a = (int)(L_IDIFF1A * s->sr_scale + 0.5f);
    int id1b = (int)(L_IDIFF1B * s->sr_scale + 0.5f);
    int id2a = (int)(L_IDIFF2A * s->sr_scale + 0.5f);
    int id2b = (int)(L_IDIFF2B * s->sr_scale + 0.5f);

    // Tank buffers: allocate at the largest length SIZE can request (1.5x
    // canonical) plus headroom for the modulation excursion and interp.
    float maxk = 1.5f * s->sr_scale;
    float exc  = EXCURSION_SAMP * s->sr_scale;
    float dd1_l_ms = ((float)L_DDIF1_L * maxk + exc + 4.0f) * 1000.0f / s->rate;
    float dd1_r_ms = ((float)L_DDIF1_R * maxk + exc + 4.0f) * 1000.0f / s->rate;
    float del1_l_ms = ((float)L_DELAY1_L * maxk + 4.0f) * 1000.0f / s->rate;
    float del2_l_ms = ((float)L_DELAY2_L * maxk + 4.0f) * 1000.0f / s->rate;
    float dd2_l_ms  = ((float)L_DDIF2_L  * maxk + 4.0f) * 1000.0f / s->rate;
    float del1_r_ms = ((float)L_DELAY1_R * maxk + 4.0f) * 1000.0f / s->rate;
    float del2_r_ms = ((float)L_DELAY2_R * maxk + 4.0f) * 1000.0f / s->rate;
    float dd2_r_ms  = ((float)L_DDIF2_R  * maxk + 4.0f) * 1000.0f / s->rate;

    int ok =
        ap_init(&s->id1a, id1a, IDIFF1_G) &&
        ap_init(&s->id1b, id1b, IDIFF1_G) &&
        ap_init(&s->id2a, id2a, IDIFF2_G) &&
        ap_init(&s->id2b, id2b, IDIFF2_G) &&
        dl_init(&s->predelay, s->rate, L_PREDELAY_MAX + 10.0f) &&
        dl_init(&s->dd1_l, s->rate, dd1_l_ms) &&
        dl_init(&s->dd1_r, s->rate, dd1_r_ms) &&
        dl_init(&s->del1_l, s->rate, del1_l_ms) &&
        dl_init(&s->del1_r, s->rate, del1_r_ms) &&
        dl_init(&s->dd2_l, s->rate, dd2_l_ms) &&
        dl_init(&s->dd2_r, s->rate, dd2_r_ms) &&
        dl_init(&s->del2_l, s->rate, del2_l_ms) &&
        dl_init(&s->del2_r, s->rate, del2_r_ms);
    if (!ok) {
        ap_free(&s->id1a); ap_free(&s->id1b); ap_free(&s->id2a); ap_free(&s->id2b);
        dl_free(&s->predelay);
        dl_free(&s->dd1_l); dl_free(&s->dd1_r);
        dl_free(&s->del1_l); dl_free(&s->del1_r);
        dl_free(&s->dd2_l); dl_free(&s->dd2_r);
        dl_free(&s->del2_l); dl_free(&s->del2_r);
        free(s);
        return NULL;
    }
    s->lfo_inc = 2.0f * (float)M_PI * MOD_RATE_HZ / (float)s->rate;
    rv_recompute_size(s, 0.5f);
    return s;
}

static void rv_block(void *st, const float *dry, int n, const float *p, float *outLR) {
    RevState *s = (RevState *)st;

    // --- read & clamp live params --------------------------------------------
    float predelay_ms = p[P_PREDELAY];
    if (predelay_ms < 0) predelay_ms = 0;
    if (predelay_ms > L_PREDELAY_MAX) predelay_ms = L_PREDELAY_MAX;

    float decay = p[P_DECAY];
    if (decay < 0) decay = 0;
    if (decay > 0.95f) decay = 0.95f;        // keep the figure-8 loop stable

    float size = p[P_SIZE];
    if (size < 0) size = 0; if (size > 1) size = 1;
    if (size != s->last_size) rv_recompute_size(s, size);

    float damp_hz = p[P_DAMP];
    if (damp_hz < 1000) damp_hz = 1000;
    if (damp_hz > 18000) damp_hz = 18000;
    float nyq = 0.49f * (float)s->rate;
    if (damp_hz > nyq) damp_hz = nyq;        // never exceed Nyquist at low rates

    float mix = p[P_MIX];
    if (mix < 0) mix = 0; if (mix > 1) mix = 1;

    // bandwidth low-pass: Table 1 default bandwidth = 0.9995 (very open). We
    // follow the paper's intent (slight input HF roll-off); fixed, musical.
    op_set_lp(&s->bw, 0.5f * (float)s->rate < 12000.0f ? nyq : 12000.0f, s->rate);
    // damping low-pass cutoff is user-controlled (DAMP), shared by both halves.
    op_set_lp(&s->damp_l, damp_hz, s->rate);
    op_set_lp(&s->damp_r, damp_hz, s->rate);

    // decay diffusion 2 = decay + 0.15, floor 0.25, ceiling 0.50 (Table 1).
    float ddiff2_g = decay + 0.15f;
    if (ddiff2_g < 0.25f) ddiff2_g = 0.25f;
    if (ddiff2_g > 0.50f) ddiff2_g = 0.50f;

    const float sc = s->sr_scale;                 // canonical -> host-rate
    const float tank_k = (0.5f + size) * sc;      // tank length scaler (size 0.5 -> sc)
    const float exc = EXCURSION_SAMP * sc;

    for (int i = 0; i < n; i++) {
        // --- input conditioning: bandwidth LP, then predelay -----------------
        float x = op_lp(&s->bw, dry[i]);
        dl_write(&s->predelay, fflush_tiny(x));
        float pd = dl_read_ms(&s->predelay, predelay_ms, s->rate);

        // --- 4 series input-diffusion allpasses ------------------------------
        float d = ap_step_fixed(&s->id1a, pd);
        d = ap_step_fixed(&s->id1b, d);
        d = ap_step_fixed(&s->id2a, d);
        d = ap_step_fixed(&s->id2b, d);   // = tank input, fed to BOTH halves

        // --- LFO (quadrature) for the two modulated allpasses ----------------
        float mod_l = exc * sinf(s->lfo_phase);
        float mod_r = exc * sinf(s->lfo_phase + 1.5707963f);
        s->lfo_phase += s->lfo_inc;
        if (s->lfo_phase > 2.0f * (float)M_PI) s->lfo_phase -= 2.0f * (float)M_PI;

        // ============================ LEFT HALF ==============================
        // decay-diffusion-1 is a MODULATED allpass; per Fig. 1 ("note sign")
        // its lattice multiplier is the negated coefficient.
        float in_l = d + s->fb_r * decay;     // cross-feed from right half
        float a_l = ap_step_dl(&s->dd1_l, s->dd1_l_base + mod_l, -DDIFF1_G, in_l);
        dl_write(&s->del1_l, fflush_tiny(a_l));
        float t_l = dl_read_samp(&s->del1_l, (float)L_DELAY1_L * tank_k);
        t_l = op_lp(&s->damp_l, t_l);         // damping low-pass (node 30->31)
        float b_l = ap_step_dl(&s->dd2_l, (float)L_DDIF2_L * tank_k, ddiff2_g, t_l);
        dl_write(&s->del2_l, fflush_tiny(b_l));
        float out_l = dl_read_samp(&s->del2_l, (float)L_DELAY2_L * tank_k);

        // ============================ RIGHT HALF =============================
        float in_r = d + s->fb_l * decay;     // cross-feed from left half
        float a_r = ap_step_dl(&s->dd1_r, s->dd1_r_base + mod_r, -DDIFF1_G, in_r);
        dl_write(&s->del1_r, fflush_tiny(a_r));
        float t_r = dl_read_samp(&s->del1_r, (float)L_DELAY1_R * tank_k);
        t_r = op_lp(&s->damp_r, t_r);         // damping low-pass (node 54->...)
        float b_r = ap_step_dl(&s->dd2_r, (float)L_DDIF2_R * tank_k, ddiff2_g, t_r);
        dl_write(&s->del2_r, fflush_tiny(b_r));
        float out_r = dl_read_samp(&s->del2_r, (float)L_DELAY2_R * tank_k);

        // close the figure-8: each half feeds the other next sample.
        s->fb_l = fflush_tiny(out_l);
        s->fb_r = fflush_tiny(out_r);

        // --- output taps (Table 2). Node->buffer map:
        //   node24_30 = del1_l (4453), node31_33 = dd2_l (1800),
        //   node33_39 = del2_l (3720), node48_54 = del1_r (4217),
        //   node55_59 = dd2_r (2656), node59_63 = del2_r (3163).
        float yl = TAP_GAIN * tap(&s->del1_r,  266, tank_k)
                 + TAP_GAIN * tap(&s->del1_r, 2974, tank_k)
                 - TAP_GAIN * tap(&s->dd2_r,  1913, tank_k)
                 + TAP_GAIN * tap(&s->del2_r, 1996, tank_k)
                 - TAP_GAIN * tap(&s->del1_l, 1990, tank_k)
                 - TAP_GAIN * tap(&s->dd2_l,   187, tank_k)
                 - TAP_GAIN * tap(&s->del2_l, 1066, tank_k);

        float yr = TAP_GAIN * tap(&s->del1_l,  353, tank_k)
                 + TAP_GAIN * tap(&s->del1_l, 3627, tank_k)
                 - TAP_GAIN * tap(&s->dd2_l,  1228, tank_k)
                 + TAP_GAIN * tap(&s->del2_l, 2673, tank_k)
                 - TAP_GAIN * tap(&s->del1_r, 2111, tank_k)
                 - TAP_GAIN * tap(&s->dd2_r,   335, tank_k)
                 - TAP_GAIN * tap(&s->del2_r,  121, tank_k);

        // wet/dry mix (mono dry into both channels).
        float in0 = dry[i];
        outLR[i * 2 + 0] = mix * fflush_tiny(yl) + (1.0f - mix) * in0;
        outLR[i * 2 + 1] = mix * fflush_tiny(yr) + (1.0f - mix) * in0;
    }
}

static void rv_destroy(void *st) {
    RevState *s = (RevState *)st;
    if (!s) return;
    ap_free(&s->id1a); ap_free(&s->id1b); ap_free(&s->id2a); ap_free(&s->id2b);
    dl_free(&s->predelay);
    dl_free(&s->dd1_l); dl_free(&s->dd1_r);
    dl_free(&s->del1_l); dl_free(&s->del1_r);
    dl_free(&s->dd2_l); dl_free(&s->dd2_r);
    dl_free(&s->del2_l); dl_free(&s->del2_r);
    free(s);
}

const H3kAlgoDef reverb_def = {
    .name = "REVERB",
    .nparams = 5,
    .params = {
        { "PREDELAY", 0,     200,   5,     20,    PK_MS,      0 },
        { "DECAY",    0,     0.95f, 0.02f, 0.7f,  PK_PERCENT, 0 },
        { "SIZE",     0,     1,     0.05f, 0.5f,  PK_PERCENT, 0 },
        { "DAMP",     1000,  18000, 500,   7000,  PK_HZ,      0 },
        { "MIX",      0,     1,     0.05f, 0.3f,  PK_PERCENT, 0 },
    },
    .create = rv_create,
    .block = rv_block,
    .destroy = rv_destroy,
};
