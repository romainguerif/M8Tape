// fx_ultratap.c - ULTRA-TAP: the Eventide H3000 "Ultra-Tap" multitap delay.
//
// One delay line, read by N taps spread evenly over a total span. Each tap has
// a gain set by a TAPER/SLOPE curve (taps fade DOWN across the series, or swell
// UP), and a stereo PAN so the taps fan out across the field for width. A TONE
// low-pass colours the whole wet signal, and an optional FEEDBACK loop sends the
// tail (a span-length delay) back into the machine for repeating bursts.
//
// Shape copied from fx_micropitch.c: a state struct, static create/block/destroy,
// and one `const H3kAlgoDef ultratap_def`. No global/static mutable state; params
// are read live each block so the real-time preview hears edits immediately.
//
// FIDELITY (researched against the Eventide UltraTap manual):
//   * FAITHFUL: multitap burst of up to N taps over a settable total time; a
//     TAPER curve that is LINEAR for |slope| <= 50 and EXPONENTIAL beyond it
//     (exactly the two-region behaviour Eventide documents); a TONE high-cut on
//     the taps; FEEDBACK fed around the whole multitap machine.
//   * MODELED / SIMPLIFIED: the hardware "Spread" knob morphs the tap *spacing*
//     (bunched at the start = slowing-down, bunched at the end = speeding-up).
//     This module's SPREAD is the total time SPAN with EVEN tap spacing, per the
//     module's parameter contract; the spacing-morph knob is not exposed.
//   * MODELED: per-tap stereo panning (equal-power) for width, and the exact pan
//     law — the original spreads taps across the field but the precise placement
//     is our choice.
//   * Polarity note: Eventide's Taper is negative = fade-UP, positive = fade-DOWN.
//     This module's SLOPE is INVERTED to match its spec (negative = decaying,
//     positive = swelling); the underlying curve math is identical.
#include "h3000.h"
#include "dsputil.h"
#include <stdlib.h>
#include <math.h>

// Parameter indices (must match the `params[]` order in the def below).
enum { P_TAPS, P_SPREAD, P_SLOPE, P_TONE, P_FEEDBACK, P_MIX };

#define UT_MAX_TAPS   16
#define UT_MAX_MS     2000.0f   // matches SPREAD max; sizes the delay line
// Feedback re-injects the full burst; give the line a little headroom past the
// largest tap so the feedback read never clamps against the write head.
#define UT_LINE_MS    (UT_MAX_MS + 50.0f)

typedef struct {
    DelayLine dl;       // mono burst buffer
    OnePole   toneL;    // wet high-cut, per output channel (independent state)
    OnePole   toneR;
    float     fbLast;   // last span-length tap, for the feedback loop
    int       rate;
} UtState;

static void *ut_create(int rate) {
    UtState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->rate = rate > 0 ? rate : 48000;
    if (!dl_init(&s->dl, s->rate, UT_LINE_MS)) {
        free(s);
        return NULL;
    }
    // Tone coeffs are set live in block(); seed something sane so the very first
    // sample is never garbage if block() were ever entered before a param push.
    op_set_lp(&s->toneL, 8000.0f, s->rate);
    op_set_lp(&s->toneR, 8000.0f, s->rate);
    return s;
}

// Per-tap gain from the SLOPE/TAPER curve. `t01` is the tap position 0..1
// (0 = first tap, 1 = last tap). Faithful two-region curve:
//   |slope| <= 50  -> linear ramp between the ends
//   |slope| >  50  -> exponential ramp (steeper toward the far end)
// SLOPE polarity follows this module's spec (inverse of Eventide's Taper):
//   negative -> the FIRST tap is loudest and the series DECAYS toward the end;
//   positive -> the LAST tap is loudest and the series SWELLS up to it.
static float ut_tap_gain(float slope, float t01) {
    float a = fabsf(slope);
    if (a < 1e-4f) return 1.0f;                 // flat: all taps equal
    if (a > 100.0f) a = 100.0f;
    // Quiet-end gain falls as |slope| grows: at 100 the far end vanishes, at 50
    // it sits at the linear midpoint (0.5).
    float depth = a / 100.0f;                    // 0..1
    // `x` runs 0 at the loud end to 1 at the quiet end. Negative slope = decay
    // (loud end is the first tap, t01=0); positive slope = swell (loud end is
    // the last tap, t01=1).
    float x = (slope < 0.0f) ? t01 : (1.0f - t01);    // 0 at loud end, 1 at quiet end
    float g;
    if (a <= 50.0f) {
        // Linear: gain falls from 1.0 to (1 - depth) across the series.
        g = 1.0f - depth * x;
    } else {
        // Exponential: same endpoints but a curved fall. Map the >50 region to a
        // decay rate so 51 is barely curved and 100 is steep.
        float k = (a - 50.0f) / 50.0f;          // 0..1 over the exp region
        float rate = 1.0f + 7.0f * k;           // gentle..steep exponent
        g = expf(-rate * x);
    }
    if (g < 0.0f) g = 0.0f;
    if (g > 1.0f) g = 1.0f;
    return g;
}

static void ut_block(void *st, const float *dry, int n, const float *p, float *outLR) {
    UtState *s = (UtState *)st;

    // --- read & clamp params -------------------------------------------------
    int taps = (int)(p[P_TAPS] + 0.5f);
    if (taps < 1) taps = 1;
    if (taps > UT_MAX_TAPS) taps = UT_MAX_TAPS;

    float span = p[P_SPREAD];
    if (span < 1.0f) span = 1.0f;
    if (span > UT_MAX_MS) span = UT_MAX_MS;

    float slope = p[P_SLOPE];
    if (slope < -100.0f) slope = -100.0f;
    if (slope >  100.0f) slope =  100.0f;

    float tone = p[P_TONE];
    if (tone < 20.0f) tone = 20.0f;
    if (tone > (float)s->rate * 0.49f) tone = (float)s->rate * 0.49f;
    op_set_lp(&s->toneL, tone, s->rate);
    op_set_lp(&s->toneR, tone, s->rate);

    float fb = p[P_FEEDBACK];
    if (fb < 0.0f) fb = 0.0f;
    if (fb > 0.95f) fb = 0.95f;          // hard guard: keep the loop < 1.0

    float mix = p[P_MIX];
    if (mix < 0.0f) mix = 0.0f;
    if (mix > 1.0f) mix = 1.0f;

    // --- precompute per-tap time / gain / pan --------------------------------
    // Tap i sits at time = span * i/(taps-1), evenly spaced over the span. With a
    // single tap we just place it at the full span (a plain echo).
    // Gains are normalised so the loudest tap is ~unity, then scaled by 1/sqrt(N)
    // so adding taps does not pile up into clipping.
    float tms[UT_MAX_TAPS];
    float tgainL[UT_MAX_TAPS];
    float tgainR[UT_MAX_TAPS];
    float norm = 1.0f / sqrtf((float)taps);
    for (int i = 0; i < taps; i++) {
        float t01 = (taps > 1) ? (float)i / (float)(taps - 1) : 1.0f;
        tms[i] = (taps > 1) ? span * t01 : span;
        float g = ut_tap_gain(slope, t01) * norm;
        // Equal-power pan, fanned across the field. First tap hard-ish left, last
        // hard-ish right; a lone tap sits centred. Keeps the burst wide without
        // pumping total energy.
        float pan = (taps > 1) ? t01 : 0.5f;          // 0=L .. 1=R
        float ang = pan * 1.5707963f;                 // 0..pi/2
        tgainL[i] = g * cosf(ang);
        tgainR[i] = g * sinf(ang);
    }
    // Pull the feedback tap from the end of the burst (the full span).
    float fbms = span;

    // --- per-sample loop -----------------------------------------------------
    for (int i = 0; i < n; i++) {
        float in = dry[i];

        // Feed input plus the recirculated tail into the line.
        float into = in + fb * s->fbLast;
        // Denormal / NaN guard on the recirculating state.
        if (!(into > -1e30f && into < 1e30f)) into = 0.0f;
        dl_write(&s->dl, into);

        // Sum the taps into L/R.
        float wetL = 0.0f, wetR = 0.0f;
        for (int t = 0; t < taps; t++) {
            float v = dl_read_ms(&s->dl, tms[t], s->rate);
            wetL += v * tgainL[t];
            wetR += v * tgainR[t];
        }

        // Tone: high-cut on the wet (independent state per channel).
        wetL = op_lp(&s->toneL, wetL);
        wetR = op_lp(&s->toneR, wetR);

        // The feedback tail is the full-span tap, taken pre-tone-filter so the
        // loop stays bright; this is the signal that recirculates next sample.
        float tail = dl_read_ms(&s->dl, fbms, s->rate);
        if (!(tail > -1e30f && tail < 1e30f)) tail = 0.0f;
        s->fbLast = tail;

        // Wet/dry. Dry is the mono input on both channels.
        outLR[i * 2 + 0] = mix * wetL + (1.0f - mix) * in;
        outLR[i * 2 + 1] = mix * wetR + (1.0f - mix) * in;
    }

    // Flush a tiny denormal that could otherwise idle in the feedback memory and
    // burn CPU on the ARM core between blocks of silence.
    if (s->fbLast > -1e-25f && s->fbLast < 1e-25f) s->fbLast = 0.0f;
}

static void ut_destroy(void *st) {
    UtState *s = (UtState *)st;
    if (!s) return;
    dl_free(&s->dl);
    free(s);
}

const H3kAlgoDef ultratap_def = {
    .name = "ULTRA-TAP",
    .nparams = 6,
    .params = {
        { "TAPS",     2,    16,    1,     8,      PK_INT,     0 },
        { "SPREAD",   20,   2000,  10,    400,    PK_MS,      0 },
        { "SLOPE",    -100, 100,   5,     -40,    PK_INT,     0 },
        { "TONE",     500,  18000, 500,   8000,   PK_HZ,      0 },
        { "FEEDBACK", 0,    0.9f,  0.05f, 0,      PK_PERCENT, 0 },
        { "MIX",      0,    1,     0.05f, 0.5f,   PK_PERCENT, 0 },
    },
    .create = ut_create,
    .block = ut_block,
    .destroy = ut_destroy,
};
