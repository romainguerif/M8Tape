// fx_vocoder.c - VOCODER: a classic channel vocoder. The H3000's vocoder splits
// the input across a bank of band-pass filters (the "analysis bank"), follows the
// envelope of each band, and uses those envelopes to gate the SAME bands of a
// carrier (the "synthesis bank"); summed, the carrier ends up "speaking" with the
// formants of the input. (Manuel: "banc d'analyse + porteuse + banc de synthese".)
//
// Here the mono input is the MODULATOR. The carrier is synthesized locally: a
// bright band-limited sawtooth from a per-sample phase accumulator (PolyBLEP to
// keep it from aliasing into mush), rich in harmonics so every band has energy to
// shape. N log-spaced bands (~120 Hz .. 8 kHz). Each band: analysis band-pass ->
// rectify -> one-pole smoother (~20 ms, no zipper) -> multiply the carrier's
// matching synthesis band -> sum. A high-passed slice of the dry modulator is mixed
// back in for sibilance ("S"/"T") so words stay intelligible. Wet/dry MIX.
//
// Shape copied from fx_micropitch.c: state struct + static create/block/destroy +
// one `const H3kAlgoDef vocoder_def`. Params are read live from `p` in params[]
// order (see the enum). No global/static mutable state; bands are heap-allocated in
// create and freed in destroy. Output is normalized by band count and guarded
// against NaN/denormals.
#include "h3000.h"
#include "dsputil.h"
#include <stdlib.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Parameter indices (must match the `params[]` order in the def below).
enum { P_CARRIER, P_BANDS, P_SHIFT, P_BRIGHT, P_SIBIL, P_MIX };

#define VOC_MAX_BANDS 16
// Carrier pitch is referenced to this MIDI note. 36 = C2 (~65.41 Hz): a low, buzzy
// synth-bass root, the canonical vocoder/"robot voice" carrier. The CARRIER param
// shifts +/-24 semitones around it (so 0 -> C2, +12 -> C3, etc.).
#define VOC_CARRIER_BASE_NOTE 36
// Analysis/synthesis band layout: log-spaced over the speech-formant range.
#define VOC_F_LO 120.0f
#define VOC_F_HI 8000.0f

typedef struct {
    Biquad  ana;   // analysis band-pass (shapes the modulator)
    Biquad  syn;   // synthesis band-pass (same centre/Q, shapes the carrier)
    OnePole env;   // envelope follower for this band (rectified, smoothed)
} VocBand;

typedef struct {
    int      rate;
    int      nbands;       // currently configured band count (<= VOC_MAX_BANDS)
    float    centre[VOC_MAX_BANDS];  // base centre freq per band (before SHIFT)
    VocBand  band[VOC_MAX_BANDS];
    double   phase;        // carrier phase accumulator, 0..1
    double   dphase;       // carrier phase increment per sample
    Biquad   sib_hp;       // high-pass for the sibilance/unvoiced passthrough
    float    last_carrier; // -24..24 cache: rebuild dphase only when pitch changes
    float    last_bands;   // rebuild band filters only when count/shift changes
    float    last_shift;
} VocState;

// MIDI note -> Hz.
static float voc_note_hz(float note) {
    return 440.0f * powf(2.0f, (note - 69.0f) / 12.0f);
}

// PolyBLEP residual: subtracts the alias energy at a sawtooth's wrap discontinuity
// so a phase-accumulator saw stays bright without turning to fizz up top. `t` is
// the phase 0..1, `dt` the per-sample phase increment.
static float voc_polyblep(float t, float dt) {
    if (dt <= 0.0f) return 0.0f;
    if (t < dt) {                 // just after the wrap
        t /= dt;
        return t + t - t * t - 1.0f;
    } else if (t > 1.0f - dt) {   // just before the wrap
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

// Lay out `nb` log-spaced band centres and (re)tune both filter banks. Q is derived
// from the per-band frequency ratio so adjacent bands tile the spectrum (overlap
// near -3 dB) rather than leaving gaps or smearing into each other. `shift_semi`
// moves the synthesis (carrier) bank's centres up/down for a formant shift while the
// analysis centres stay put — the classic "gender/character" control.
static void voc_build_bands(VocState *s, int nb, float shift_semi) {
    if (nb < 1) nb = 1;
    if (nb > VOC_MAX_BANDS) nb = VOC_MAX_BANDS;
    s->nbands = nb;

    // log-spaced centres from VOC_F_LO to VOC_F_HI
    float ratio = (nb > 1) ? powf(VOC_F_HI / VOC_F_LO, 1.0f / (float)(nb - 1)) : 1.0f;
    // Constant-Q for log spacing: Q = fc / bandwidth, bandwidth ~ fc*(ratio-1/ratio)
    // /2 between neighbours. Clamp to a musically useful, stable range.
    float span = ratio - 1.0f / ratio;
    float q = (span > 1e-4f) ? (1.0f / span) : 6.0f;
    if (q < 2.0f) q = 2.0f;
    if (q > 8.0f) q = 8.0f;

    float shift_mul = powf(2.0f, shift_semi / 12.0f);
    float nyq = 0.45f * (float)s->rate;
    float fc = VOC_F_LO;
    for (int b = 0; b < nb; b++) {
        s->centre[b] = fc;
        float fa = fc;             // analysis centre (modulator)
        float fsyn = fc * shift_mul; // synthesis centre (carrier), formant-shifted
        if (fa   > nyq) fa   = nyq;
        if (fsyn > nyq) fsyn = nyq;
        if (fsyn < 20.0f) fsyn = 20.0f;
        bq_bandpass(&s->band[b].ana, fa,   q, s->rate);
        bq_bandpass(&s->band[b].syn, fsyn, q, s->rate);
        // envelope smoothing ~20 ms (=~8 Hz). Slow enough to kill zipper, fast
        // enough to track syllables.
        op_set_lp(&s->band[b].env, 8.0f, s->rate);
        fc *= ratio;
    }
}

static void *voc_create(int rate) {
    VocState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->rate = rate > 0 ? rate : 48000;
    s->phase = 0.0;
    s->last_carrier = 1e9f;  // force first-block (re)build
    s->last_bands = -1.0f;
    s->last_shift = 1e9f;
    voc_build_bands(s, 14, 0.0f);
    for (int b = 0; b < s->nbands; b++) {
        bq_reset(&s->band[b].ana);
        bq_reset(&s->band[b].syn);
        s->band[b].env.z = 0.0f;
    }
    // sibilance high-pass: above the formant bands so only fricative/consonant
    // hiss passes through to the wet signal for intelligibility.
    bq_highpass(&s->sib_hp, 6500.0f, 0.707f, s->rate);
    bq_reset(&s->sib_hp);
    return s;
}

static void voc_block(void *st, const float *dry, int n, const float *p, float *outLR) {
    VocState *s = (VocState *)st;

    float carrier = p[P_CARRIER];
    if (carrier < -24.0f) carrier = -24.0f; if (carrier > 24.0f) carrier = 24.0f;
    int nb = (int)(p[P_BANDS] + 0.5f);
    if (nb < 6) nb = 6; if (nb > VOC_MAX_BANDS) nb = VOC_MAX_BANDS;
    float shift = p[P_SHIFT];
    if (shift < -12.0f) shift = -12.0f; if (shift > 12.0f) shift = 12.0f;
    float bright = p[P_BRIGHT]; if (bright < 0.0f) bright = 0.0f; if (bright > 1.0f) bright = 1.0f;
    float sibil = p[P_SIBIL];   if (sibil < 0.0f) sibil = 0.0f;   if (sibil > 1.0f) sibil = 1.0f;
    float mix = p[P_MIX];       if (mix < 0.0f) mix = 0.0f;       if (mix > 1.0f) mix = 1.0f;

    // (Re)tune bands only when the count or formant shift actually changes — cheap
    // to leave them be every block, and avoids resetting filter history each call.
    if (nb != (int)(s->last_bands + 0.5f) || shift != s->last_shift) {
        voc_build_bands(s, nb, shift);
        s->last_bands = (float)nb;
        s->last_shift = shift;
    }
    // Carrier frequency from the chosen pitch.
    if (carrier != s->last_carrier) {
        float hz = voc_note_hz((float)VOC_CARRIER_BASE_NOTE + carrier);
        if (hz < 10.0f) hz = 10.0f;
        s->dphase = (double)hz / (double)s->rate;
        s->last_carrier = carrier;
    }

    // Output normalization: each band can contribute up to ~its envelope, so sum
    // grows with band count. Scale by 1/sqrt(nb) (bands are largely decorrelated)
    // and a modest makeup so a full-wet vocoder sits near unity.
    float norm = 1.6f / sqrtf((float)s->nbands);
    float dt = (float)s->dphase;

    for (int i = 0; i < n; i++) {
        float in = dry[i];
        if (!(in == in)) in = 0.0f;   // NaN guard on input

        // --- carrier: band-limited sawtooth in [-1,1] -----------------------
        float ph = (float)s->phase;
        float saw = 2.0f * ph - 1.0f;
        saw -= voc_polyblep(ph, dt);
        // `bright` blends toward a brighter pulse-ish tone by mixing in a phase-
        // shifted copy (adds odd-harmonic edge) — gives the user a timbre tilt
        // without a second oscillator's cost.
        if (bright > 0.0f) {
            float ph2 = ph + 0.5f; if (ph2 >= 1.0f) ph2 -= 1.0f;
            float saw2 = 2.0f * ph2 - 1.0f;
            saw2 -= voc_polyblep(ph2, dt);
            saw = saw * (1.0f - 0.5f * bright) - saw2 * (0.5f * bright);
        }
        s->phase += s->dphase;
        if (s->phase >= 1.0) s->phase -= 1.0;

        // --- analysis + synthesis across the bank ---------------------------
        float wetL = 0.0f, wetR = 0.0f;
        for (int b = 0; b < s->nbands; b++) {
            // analyse the modulator band and follow its (rectified) envelope
            float a = bq_process(&s->band[b].ana, in);
            float env = op_lp(&s->band[b].env, fabsf(a));
            // shape the carrier's matching band by that envelope
            float c = bq_process(&s->band[b].syn, saw);
            float v = c * env;
            // spread bands across the stereo field: low bands centre, alternating
            // bands lean L/R for width while staying mono-compatible in sum.
            float panR = (float)b / (float)(s->nbands > 1 ? s->nbands - 1 : 1); // 0..1
            wetL += v * (1.0f - 0.5f * panR);
            wetR += v * (0.5f + 0.5f * panR);
        }
        wetL *= norm;
        wetR *= norm;

        // --- sibilance / unvoiced passthrough for intelligibility -----------
        if (sibil > 0.0f) {
            float hp = bq_process(&s->sib_hp, in) * sibil;
            wetL += hp;
            wetR += hp;
        }

        // denormal/NaN guard on the wet result
        if (!(wetL == wetL)) wetL = 0.0f;
        if (!(wetR == wetR)) wetR = 0.0f;
        wetL += 1e-20f; wetL -= 1e-20f;
        wetR += 1e-20f; wetR -= 1e-20f;

        float oL = mix * wetL + (1.0f - mix) * in;
        float oR = mix * wetR + (1.0f - mix) * in;
        // final safety clamp — keep output bounded regardless of settings
        if (oL >  4.0f) oL =  4.0f; if (oL < -4.0f) oL = -4.0f;
        if (oR >  4.0f) oR =  4.0f; if (oR < -4.0f) oR = -4.0f;
        outLR[i * 2 + 0] = oL;
        outLR[i * 2 + 1] = oR;
    }
}

static void voc_destroy(void *st) {
    VocState *s = (VocState *)st;
    if (!s) return;
    free(s);   // all band state lives inline in VocState; nothing else to free
}

const H3kAlgoDef vocoder_def = {
    .name = "VOCODER",
    .nparams = 6,
    .params = {
        // CARRIER: semitone offset over base note C2 (=MIDI 36, ~65.4 Hz).
        { "CARRIER",   -24,  24,   1,    0,    PK_SEMI,    0 },
        { "BANDS",       6,  16,   1,    14,   PK_INT,     0 },
        // SHIFT: formant/band shift of the synthesis (carrier) bank only.
        { "SHIFT",     -12,  12,   1,    0,    PK_SEMI,    0 },
        { "BRIGHT",      0,   1,   0.05f, 0.35f, PK_PERCENT, 0 },
        { "SIBILANCE",   0,   1,   0.05f, 0.25f, PK_PERCENT, 0 },
        { "MIX",         0,   1,   0.05f, 1.0f,  PK_PERCENT, 0 },
    },
    .create = voc_create,
    .block = voc_block,
    .destroy = voc_destroy,
};
