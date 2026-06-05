// fx_drive.c - COLOR: a multi-mode saturation / drive / "color" processor for the
// M8Tape Studio engine. One mono program in, processed identically to both output
// channels. The character is chosen by MODE; DRIVE pushes the signal into the
// nonlinearity, TONE is a post low-pass to tame fizz, MIX blends dry/wet (parallel
// "New York" style saturation) and OUTPUT trims the result.
//
// WHY OVERSAMPLING (audio quality is the priority here):
//   A memoryless nonlinearity f(x) generates harmonics far above the input
//   bandwidth. At the working sample rate those harmonics fold back below Nyquist
//   as inharmonic ALIASING -- the harsh, dissonant "digital fizz" that makes naive
//   distortion sound bad. The fix is the textbook oversampling sandwich:
//       1. UPSAMPLE by N (zero-stuff: insert N-1 zeros between samples, multiply
//          by N to preserve level) and low-pass to remove the spectral IMAGES the
//          stuffing created -> a clean band-limited signal at the N*rate domain.
//       2. Apply the nonlinearity at N*rate. The harmonics it makes now have room
//          to live below the *oversampled* Nyquist, so far less of them fold.
//       3. ANTI-ALIAS low-pass at N*rate (cutoff ~ base Nyquist) to kill the
//          harmonics that still sit above base Nyquist, THEN decimate (keep 1 of
//          every N samples). What folds back is now tiny.
//   See e.g. "Oversampling for Nonlinear Waveshaping: Choosing the Right Filters"
//   (Kahles/Holters/Zoelzer, DAFx-19) and the standard DAFx oversampling material.
//
//   Factor: 4x for the continuous shapers (TUBE/TAPE/TRANSISTOR/DIODE/FOLD). 4x
//   gives a wide transition band so a modest Butterworth reaches deep stop-band
//   attenuation while staying cheap and unconditionally stable. The anti-image and
//   anti-alias filters are each a CASCADE OF TWO RBJ biquad low-passes whose pole
//   Qs (0.5412, 1.3066) make a 4th-order Butterworth (maximally flat passband, no
//   ripple) cut at 0.92 * base-Nyquist. Two independent cascades are kept per
//   channel-stage (up, down) because biquads carry state.
//
//   CRUSH is the deliberate exception: a bit-depth + sample-rate decimator whose
//   whole point is lo-fi aliasing, so it runs at the BASE rate (no oversampling) --
//   oversampling it would defeat the effect. It is still bounded and finite.
//
// STABILITY / SAFETY: every transfer function is bounded (the worst case, the
// wavefolder, is |y|<=1), feedback-free, and the output is scrubbed for NaN/Inf and
// flushed of denormals on the raw bits so it stays correct under -Ofast/-ffast-math
// (the aarch64 device build compiles this file with -Ofast).
//
// Only h3000.h / dsputil.h symbols are used; the waveshapers, the oversampler and
// the decimator are all local. No global mutable state; buffers are tiny and live
// on the stack per block-chunk.
#include "h3000.h"
#include "dsputil.h"
#include <stdlib.h>
#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Parameter indices -- MUST match the params[] order in drive_def below.
enum { P_MODE, P_DRIVE, P_TONE, P_MIX, P_OUTPUT };

// MODE values (index into MODE_CHOICES below).
enum { M_TUBE, M_TAPE, M_TRANSISTOR, M_DIODE, M_FOLD, M_CRUSH, M_COUNT };
static const char *const MODE_CHOICES[] = {
    "TUBE", "TAPE", "TRANSISTOR", "DIODE", "FOLD", "CRUSH", 0
};

#define OS          4          // oversampling factor for the continuous shapers
#define OS_CUT      0.92f      // anti-alias/anti-image cutoff as fraction of base Nyquist
#define DENORM      1.0e-25f   // flush threshold to kill denormals

// 4th-order Butterworth = cascade of two RBJ biquad low-passes at these pole Qs.
#define BW_Q0       0.54119610f
#define BW_Q1       1.30656296f

typedef struct {
    int   rate;        // base rate
    int   osrate;      // OS * base rate (the shaping domain)

    // Anti-image (interpolation) low-pass: smooths the zero-stuffed stream.
    Biquad up0, up1;
    // Anti-alias (decimation) low-pass: band-limits before we throw samples away.
    Biquad dn0, dn1;
    // Post-shaper TONE low-pass (runs at the base rate, on the wet signal).
    Biquad tone0, tone1;

    // CRUSH (base-rate decimator) running state.
    float  crush_hold;     // last emitted (sample-and-held) value
    float  crush_phase;    // fractional sample-rate-reduction accumulator

    // Wet-path DC blocker (one-pole high-pass ~8 Hz). Real tube/tape gear is
    // AC-coupled; asymmetric shapers (TUBE bias, signal-dependent even harmonics)
    // legitimately produce DC that we must not pass into the mix / output.
    float  dc_x1, dc_y1;   // high-pass state: y = x - x1 + R*y1
    float  dc_R;           // pole radius (set from rate in dr_create)
} DriveState;

// ------------------------------------------------------------------ shapers ---
// All take an already drive-scaled input and return a bounded output. They are
// memoryless except where noted, so they oversample cleanly.

// scrub one value: kill NaN/Inf and flush denormals (bit-level so -ffast-math
// can't optimise the comparisons away).
static inline float scrub(float v) {
    union { float f; uint32_t u; } b; b.f = v;
    uint32_t exp = b.u & 0x7f800000u;
    if (exp == 0x7f800000u) return 0.0f;          // NaN or Inf
    if (exp == 0u)          return 0.0f;          // subnormal / zero -> flush
    return v;
}

// TUBE: asymmetric soft clip. A triode-like curve -- the negative half saturates
// sooner / harder than the positive half, generating EVEN harmonics (the warm,
// "second-harmonic" tube character) on top of the odd ones. tanh gives the smooth
// soft-clip knee; the asymmetry comes from a small DC-ish bias added before the
// shaper and removed after (so steady-state output stays centred).
static inline float sh_tube(float x) {
    const float bias = 0.18f;
    float y = tanhf(x + bias) - tanhf(bias);   // shift in, recentre out
    return y;
}

// TAPE: odd-symmetric soft saturation with a gentle "more compression as it gets
// louder" knee -- the tape-like rounding of transients. Pure odd harmonics. The
// rational sigmoid x/(1+|x|)-flavoured curve here (a smoothed arctan) has a softer,
// later knee than tanh, which reads as the slow magnetic compression of tape.
static inline float sh_tape(float x) {
    // (2/pi)*atan saturates to +/-1; scaled so small signals pass near unity gain.
    return (2.0f / (float)M_PI) * atanf(x * 1.4f);
}

// TRANSISTOR: a harder, more aggressive odd-harmonic clip than tape (solid-state
// "edge"), with a touch of asymmetry for grit. tanh with a higher internal gain
// gives the firmer knee; the cubic-ish bias adds slight even content.
static inline float sh_transistor(float x) {
    // Firm solid-state knee: tanh at a higher internal gain than TAPE (clips ~sooner,
    // harder "edge"). A small input bias b adds even-harmonic asymmetry/grit; the
    // -tanh(b) recentres so the curve maps 0 -> 0 (the previous form left a constant
    // -0.03 offset at the origin -> a DC bias on the wet signal: now fixed).
    const float b = 0.10f;
    float y = tanhf(1.5f * x + b) - tanhf(b);
    if (y >  1.0f) y =  1.0f;
    if (y < -1.0f) y = -1.0f;
    return y;
}

// DIODE: a diode-clipper transfer (the heart of overdrive pedals). A pair of
// anti-parallel diodes clamps the signal with the exponential Shockley knee:
// soft as it enters conduction, then a hard near-flat top. Modelled with a
// symmetric "soft-knee then hard-clip" curve.
static inline float sh_diode(float x) {
    // exponential approach to the clamp, symmetric (anti-parallel pair).
    float a = fabsf(x);
    float k = 1.0f - expf(-a);          // 0 at 0, -> 1 as |x| grows (soft knee)
    float y = (x >= 0.0f) ? k : -k;
    return y;
}

// FOLD: a wavefolder. Instead of clipping at the rail, the signal that would
// exceed it is folded back -- sin() is a smooth, classic full-wave folder that
// generates a dense, bell-like harmonic series the harder you drive it. Bounded
// to [-1,1] by construction. (Folders alias VERY readily, which is exactly why
// this mode benefits most from the 4x oversampling.)
static inline float sh_fold(float x) {
    return sinf(x);
}

// Dispatch to the selected continuous shaper.
static inline float shape(int mode, float x) {
    switch (mode) {
        case M_TUBE:       return sh_tube(x);
        case M_TAPE:       return sh_tape(x);
        case M_TRANSISTOR: return sh_transistor(x);
        case M_DIODE:      return sh_diode(x);
        case M_FOLD:       return sh_fold(x);
        default:           return sh_tape(x);
    }
}

// ------------------------------------------------------------------ engine ----
static void *dr_create(int rate) {
    DriveState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->rate   = rate > 0 ? rate : 48000;
    s->osrate = s->rate * OS;

    // Butterworth low-passes for the up/down sampling, cut just below base Nyquist
    // but specified in the OVERSAMPLED domain (that is where they run).
    float cut = OS_CUT * 0.5f * (float)s->rate;       // Hz
    bq_lowpass(&s->up0, cut, BW_Q0, s->osrate);
    bq_lowpass(&s->up1, cut, BW_Q1, s->osrate);
    bq_lowpass(&s->dn0, cut, BW_Q0, s->osrate);
    bq_lowpass(&s->dn1, cut, BW_Q1, s->osrate);

    // DC blocker pole for ~8 Hz at the base rate: R = 1 - 2*pi*fc/fs.
    s->dc_R = 1.0f - (6.2831853f * 8.0f / (float)s->rate);
    if (s->dc_R < 0.0f)   s->dc_R = 0.0f;
    if (s->dc_R > 0.9999f) s->dc_R = 0.9999f;

    return s;
}

static void dr_block(void *st, const float *dry, int n, const float *p, float *outLR) {
    DriveState *s = (DriveState *)st;

    // --- read & clamp params --------------------------------------------------
    int mode = (int)(p[P_MODE] + 0.5f);
    if (mode < 0) mode = 0;
    if (mode >= M_COUNT) mode = M_COUNT - 1;

    float drive_db = p[P_DRIVE];                          // input gain in dB
    if (drive_db < 0.0f)  drive_db = 0.0f;
    if (drive_db > 48.0f) drive_db = 48.0f;
    float drive = powf(10.0f, drive_db / 20.0f);          // linear input gain

    float tone = p[P_TONE];                               // post low-pass cutoff
    float nyq  = 0.5f * (float)s->rate;
    if (tone < 200.0f)       tone = 200.0f;
    if (tone > nyq - 500.0f) tone = nyq - 500.0f;

    float mix = p[P_MIX];
    if (mix < 0.0f) mix = 0.0f;
    if (mix > 1.0f) mix = 1.0f;

    float out_db = p[P_OUTPUT];
    if (out_db < -24.0f) out_db = -24.0f;
    if (out_db >  24.0f) out_db =  24.0f;
    float outgain = powf(10.0f, out_db / 20.0f);

    // Post-shaper TONE low-pass (Butterworth, base rate). Recomputed per block so
    // live edits are heard; state is preserved across blocks (coeff change only).
    bq_lowpass(&s->tone0, tone, BW_Q0, s->rate);
    bq_lowpass(&s->tone1, tone, BW_Q1, s->rate);

    // CRUSH controls are derived from DRIVE so the param set stays at five: more
    // DRIVE -> fewer bits and a lower internal sample rate (heavier crushing).
    // drive_db 0..48 -> t 0..1.
    float t        = drive_db / 48.0f;
    float bits     = 16.0f - 13.0f * t;                   // 16 .. 3 bits
    if (bits < 2.0f) bits = 2.0f;
    float levels   = powf(2.0f, bits);
    float halfstep = 1.0f / levels;                       // quantiser step / 2 (range +/-1)
    // sample-rate reduction factor (>=1): hold each emitted sample this many input
    // samples. 1 .. ~24 across the DRIVE range.
    float srr      = 1.0f + 23.0f * t * t;

    for (int i = 0; i < n; i++) {
        float in = scrub(dry[i]);
        float wet;

        if (mode == M_CRUSH) {
            // --- bit + sample-rate decimator (BASE rate; aliasing is intended) --
            // Sample-rate reduction: advance a phase by 1 each input sample and
            // re-sample (sample-and-hold) the bit-quantised input when it wraps.
            s->crush_phase += 1.0f;
            if (s->crush_phase >= srr) {
                s->crush_phase -= srr;
                // input gain into the crusher, then quantise to `levels` steps.
                float xg = in * drive;
                if (xg >  4.0f) xg =  4.0f;            // bound before quantise
                if (xg < -4.0f) xg = -4.0f;
                float q = floorf(xg / halfstep + 0.5f) * halfstep;
                s->crush_hold = q;
            }
            wet = s->crush_hold;
        } else {
            // --- oversampled continuous waveshaper -----------------------------
            // Upsample by OS via zero-stuffing (x OS to preserve level), filter
            // out the images, shape, anti-alias filter, then decimate (the last
            // filtered subsample is the kept one).
            float decimated = 0.0f;
            float xin = in * drive;
            for (int k = 0; k < OS; k++) {
                // zero-stuff: energy only in the first phase of each input sample.
                float u = (k == 0) ? (xin * (float)OS) : 0.0f;
                // anti-image low-pass (4th-order Butterworth cascade).
                u = bq_process(&s->up1, bq_process(&s->up0, u));
                // memoryless nonlinearity at the oversampled rate.
                float y = shape(mode, u);
                // anti-alias low-pass (4th-order Butterworth cascade).
                y = bq_process(&s->dn1, bq_process(&s->dn0, y));
                decimated = y;     // keep the last sub-phase as the output sample
            }
            wet = decimated;
        }

        wet = scrub(wet);

        // Post TONE low-pass on the wet path only (Butterworth, base rate).
        wet = bq_process(&s->tone1, bq_process(&s->tone0, wet));

        // DC blocker on the wet path (AC-couple): remove any offset the asymmetric
        // shapers introduce before it reaches the mix.
        float hp = wet - s->dc_x1 + s->dc_R * s->dc_y1;
        s->dc_x1 = wet; s->dc_y1 = hp;
        wet = scrub(hp);

        // Dry/wet mix, then output trim.
        float o = (mix * wet + (1.0f - mix) * in) * outgain;
        o = scrub(o);
        if (o > -DENORM && o < DENORM) o = 0.0f;

        // mono program -> identical to both interleaved channels.
        outLR[i * 2 + 0] = o;
        outLR[i * 2 + 1] = o;
    }
}

static void dr_destroy(void *st) {
    free(st);
}

const H3kAlgoDef drive_def = {
    .name     = "COLOR",
    .category = "STUDIO",
    .nparams  = 5,
    .params   = {
        { "MODE",   0,    M_COUNT - 1, 1,    M_TUBE, PK_CHOICE,  MODE_CHOICES },
        { "DRIVE",  0,    48,          1,    12,     PK_DB,      0 },
        { "TONE",   200,  18000,       250,  12000,  PK_HZ,      0 },
        { "MIX",    0,    1,           0.05f, 1.0f,  PK_PERCENT, 0 },
        { "OUTPUT", -24,  24,          1,    0,      PK_DB,      0 },
    },
    .create  = dr_create,
    .block   = dr_block,
    .destroy = dr_destroy,
};
