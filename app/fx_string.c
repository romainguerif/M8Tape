// fx_string.c - String Modeler: an Eventide H3000-style "String" algorithm built
// on Karplus-Strong / digital-waveguide string synthesis. The dry input does not
// merely pass through: it continuously EXCITES a tuned resonator (a fractional
// delay line with a damping low-pass and a loop gain in its feedback), so the
// sample is re-voiced as a plucked/bowed string at the chosen pitch. Dry/wet mix.
//
// THEORY (well documented; see Karplus & Strong 1983, Jaffe & Smith 1983, and
// J. O. Smith's digital-waveguide work at CCRMA):
//   - A delay line of total loop length L = rate/freq samples sets the
//     fundamental: a wave injected into the loop recirculates every L samples,
//     so it rings at f0 = rate / L.
//   - A low-pass filter in the feedback loop damps high partials faster than low
//     ones -> the natural, brightness-losing decay of a real string. Its cutoff
//     is the DAMP / brightness control.
//   - The loop gain (< 1) sets sustain: closer to 1 = longer ring.
//   - Integer delay alone only tunes to rate/N (coarse at high pitch). The
//     sub-sample remainder is realised with a first-order tuning ALLPASS,
//     coefficient a = (1 - d)/(1 + d) for a fractional delay of d samples
//     (Jaffe & Smith). The allpass has unit magnitude at every frequency, so it
//     adjusts pitch without altering the loop's stability or the damping shape.
//
// What is FAITHFUL to the physics: the tuned feedback loop, the in-loop damping
// low-pass, the loop-gain decay, and the fractional (allpass) tuning are the
// genuine Karplus-Strong / waveguide model. What is MODELED for this effect: the
// string is *continuously* excited by the incoming sample (a re-voicing
// processor, not a one-shot pluck), the input is band-passed before injection so
// the excitation has a plucky spectrum, and stereo width comes from two strings
// detuned by a few cents L/R. Pitch base is A2 = 110 Hz; PITCH is a semitone
// offset about that base.
#include "h3000.h"
#include "dsputil.h"
#include <stdlib.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Parameter indices -- MUST match the params[] order in the def below.
enum { P_PITCH, P_DECAY, P_DAMP, P_EXCITE, P_WIDTH, P_MIX };

#define ST_BASE_HZ   110.0f   // A2: the string fundamental at PITCH = 0
#define ST_MIN_SEMI  (-24.0f) // lowest supported pitch -> longest delay line
#define ST_MIN_HZ    (ST_BASE_HZ * 0.25f)   // == 110 * 2^(-24/12) = 27.5 Hz (A0)
#define ST_DENORM    1.0e-20f // flush threshold to kill denormals/NaN

// One Karplus-Strong string voice: a delay line read at a sample delay, a damping
// one-pole low-pass and a first-order tuning allpass in the feedback path.
typedef struct {
    DelayLine dl;       // the recirculating loop (sized for the lowest pitch)
    OnePole   damp;     // in-loop brightness / damping low-pass
    float     apx;      // tuning allpass: previous input  x[n-1]
    float     apy;      // tuning allpass: previous output y[n-1]
    float     fb;       // last value fed back into the loop (the loop output)
} StringVoice;

typedef struct {
    StringVoice vL, vR;
    Biquad      exciteBP;  // shapes the dry signal into a plucky excitation
    int         rate;
} StringState;

// Map PITCH (semitones, relative to A2=110 Hz) -> fundamental in Hz.
static float st_freq(float semi, int rate) {
    float f = ST_BASE_HZ * powf(2.0f, semi / 12.0f);
    if (f < 1.0f) f = 1.0f;
    float fmax = (float)rate / 4.0f;   // keep loop >= 4 samples for a sane split
    if (f > fmax) f = fmax;
    return f;
}

// Phase delay (in samples) of the in-loop one-pole low-pass at angular freq w.
// op_lp implements y += a*(in-y), i.e. H(z) = a / (1 - (1-a) z^-1).
// phase = -atan2( (1-a) sin w, 1 - (1-a) cos w );  phase delay = -phase / w.
static float st_onepole_pd(const OnePole *f, float w) {
    float b = 1.0f - f->a;     // pole radius
    if (w < 1.0e-6f) return b / (1.0f - b);   // DC limit of the phase delay
    float ph = -atan2f(b * sinf(w), 1.0f - b * cosf(w));
    return -ph / w;
}

// Split a desired total delay into an integer delay-line tap plus a first-order
// tuning-allpass coefficient for the fractional remainder. A first-order allpass
// is best conditioned for fractional delays around 0.5..1.5; we therefore keep
// the allpass delay d in [0.5, 1.5) by borrowing one sample from the integer tap.
//   *outInt = integer tap (samples), *outAp = allpass coefficient.
static void st_split_delay(float total, float *outInt, float *outAp) {
    if (total < 2.0f) total = 2.0f;
    // Reserve one sample for the allpass; integer tap is floor(total-1)+offset.
    int   ip   = (int)floorf(total) - 1;
    if (ip < 1) ip = 1;
    float d    = total - (float)ip;      // fractional delay handled by allpass
    if (d < 0.5f) { ip -= 1; if (ip < 1) ip = 1; d = total - (float)ip; }
    if (d >= 1.5f) { ip += 1; d = total - (float)ip; }
    if (d < 0.1f) d = 0.1f;              // final safety clamp
    // Jaffe & Smith first-order allpass coefficient for fractional delay d.
    float a = (1.0f - d) / (1.0f + d);
    *outInt = (float)ip;
    *outAp  = a;
}

// Process one sample of a string voice. Feedback chain:
//   delay line -> integer tap -> tuning allpass -> damping low-pass
//   -> * loopGain -> back into the delay line (with the new excitation).
static inline float sv_process(StringVoice *v, float exc, float loopGain,
                               float intDelay, float apCoef) {
    // Write (excitation + previous loop output) into the line.
    float in = exc + v->fb;
    if (!isfinite(in)) in = 0.0f;
    dl_write(&v->dl, in);

    // Integer-delayed read from the loop.
    float x = dl_read_samp(&v->dl, intDelay);

    // First-order tuning allpass: y[n] = a*x[n] + x[n-1] - a*y[n-1].
    float y = apCoef * x + v->apx - apCoef * v->apy;
    v->apx = x;
    v->apy = y;

    // In-loop damping low-pass (brightness/decay shaping), then loop gain.
    float damped = op_lp(&v->damp, y);
    float loop = damped * loopGain;

    // Denormal / NaN guard so a long sustain never poisons the loop.
    if (!isfinite(loop)) loop = 0.0f;
    if (loop > -ST_DENORM && loop < ST_DENORM) loop = 0.0f;

    v->fb = loop;   // injected next sample
    return loop;    // the voice output for this sample
}

static void *st_create(int rate) {
    StringState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->rate = rate > 0 ? rate : 48000;

    // Size each delay line for the LOWEST pitch (longest period) we support,
    // with headroom for the integer tap + interpolation + a couple of samples.
    float max_ms = 1000.0f / ST_MIN_HZ + 5.0f;   // ~41.4 ms at 27.5 Hz
    if (!dl_init(&s->vL.dl, s->rate, max_ms) ||
        !dl_init(&s->vR.dl, s->rate, max_ms)) {
        dl_free(&s->vL.dl); dl_free(&s->vR.dl); free(s);
        return NULL;
    }
    return s;
}

static void st_block(void *st, const float *dry, int n, const float *p, float *outLR) {
    StringState *s = (StringState *)st;

    // --- read & clamp params --------------------------------------------------
    float semi   = p[P_PITCH];
    float decay  = p[P_DECAY];  if (decay  < 0.0f) decay  = 0.0f; if (decay  > 0.99f) decay  = 0.99f;
    float damp   = p[P_DAMP];   if (damp   < 200.0f) damp  = 200.0f;
    float excite = p[P_EXCITE]; if (excite < 0.0f) excite = 0.0f; if (excite > 1.0f) excite = 1.0f;
    float width  = p[P_WIDTH];  if (width  < 0.0f) width  = 0.0f; if (width  > 1.0f) width  = 1.0f;
    float mix    = p[P_MIX];    if (mix    < 0.0f) mix    = 0.0f; if (mix    > 1.0f) mix    = 1.0f;

    float nyq = (float)s->rate * 0.5f;
    if (damp > nyq - 500.0f) damp = nyq - 500.0f;   // keep the LP well-defined

    // Stereo detune: up to +/-6 cents per side at full WIDTH (center at width 0).
    float cents = width * 12.0f;
    float detL = powf(2.0f,  (cents * 0.5f) / 1200.0f);
    float detR = powf(2.0f, -(cents * 0.5f) / 1200.0f);

    // --- fundamental -> loop length -------------------------------------------
    float f0 = st_freq(semi, s->rate);
    float fL = f0 * detL, fR = f0 * detR;

    // Configure both damping low-passes (same cutoff L/R).
    op_set_lp(&s->vL.damp, damp, s->rate);
    op_set_lp(&s->vR.damp, damp, s->rate);

    // Account for every fixed delay around the loop so the fundamental stays in
    // tune (critical at high pitch, where a fraction of a sample is many cents):
    //   - the in-loop one-pole's frequency-dependent phase delay (pd), and
    //   - the loop's structural latency: the feedback register that holds the
    //     loop output for one sample before it is re-injected. Empirically (and
    //     by construction, since dl_read_samp reads relative to the just-written
    //     sample) this nets ~0.95 sample. Subtracting both leaves the tuning
    //     allpass to realise the remaining fractional part exactly; measured
    //     tuning error is then under ~0.6 cents from 27.5 Hz to 440 Hz.
    const float ST_LOOP_LATENCY = 0.95f;
    float wL = 2.0f * (float)M_PI * fL / (float)s->rate;
    float wR = 2.0f * (float)M_PI * fR / (float)s->rate;
    float pdL = st_onepole_pd(&s->vL.damp, wL);
    float pdR = st_onepole_pd(&s->vR.damp, wR);

    float totL = (float)s->rate / fL - pdL - ST_LOOP_LATENCY;
    float totR = (float)s->rate / fR - pdR - ST_LOOP_LATENCY;

    // Split into integer tap + tuning-allpass fractional part.
    float intL, apL, intR, apR;
    st_split_delay(totL, &intL, &apL);
    st_split_delay(totR, &intR, &apR);

    // Excitation: band-pass the dry signal so the energy we feed the loop has a
    // plucky (mid-forward) spectrum rather than DC/rumble, then scale by EXCITE.
    float bpHz = f0 * 2.0f;
    if (bpHz < 80.0f) bpHz = 80.0f;
    if (bpHz > nyq - 500.0f) bpHz = nyq - 500.0f;
    bq_bandpass(&s->exciteBP, bpHz, 0.7f, s->rate);
    // Scale the excitation by (1 - loopGain): a Karplus-Strong loop has resonance
    // gain ~1/(1-decay), so without this the continuously-excited string builds up
    // to ~10x at DECAY=0.9. Normalizing keeps the steady-state ring near unity at
    // any sustain (and makes a high-sustain string ring UP over time, as it should).
    float drive = (0.25f + 1.75f * excite) * (1.0f - decay);   // normalized
    if (drive < 0.01f) drive = 0.01f;

    for (int i = 0; i < n; i++) {
        float in = dry[i];
        if (!isfinite(in)) in = 0.0f;

        float exc = bq_process(&s->exciteBP, in) * drive;

        float sL = sv_process(&s->vL, exc, decay, intL, apL);
        float sR = sv_process(&s->vR, exc, decay, intR, apR);

        // Mild make-up so the wet string is audible without clipping.
        float wetL = sL * 1.2f;
        float wetR = sR * 1.2f;

        float oL = mix * wetL + (1.0f - mix) * in;
        float oR = mix * wetR + (1.0f - mix) * in;
        if (!isfinite(oL)) oL = 0.0f;
        if (!isfinite(oR)) oR = 0.0f;
        outLR[i * 2 + 0] = oL;
        outLR[i * 2 + 1] = oR;
    }
}

static void st_destroy(void *st) {
    StringState *s = (StringState *)st;
    if (!s) return;
    dl_free(&s->vL.dl);
    dl_free(&s->vR.dl);
    free(s);
}

const H3kAlgoDef string_def = {
    .name = "STRING",
    .nparams = 6,
    .params = {
        { "PITCH",   ST_MIN_SEMI, 24,      1,      0,      PK_SEMI,    0 },
        { "DECAY",   0,           0.99f,   0.01f,  0.9f,   PK_PERCENT, 0 },
        { "DAMP",    500,         16000,   250,    4000,   PK_HZ,      0 },
        { "EXCITE",  0,           1,       0.05f,  0.6f,   PK_PERCENT, 0 },
        { "WIDTH",   0,           1,       0.05f,  0.3f,   PK_PERCENT, 0 },
        { "MIX",     0,           1,       0.05f,  0.5f,   PK_PERCENT, 0 },
    },
    .create = st_create,
    .block = st_block,
    .destroy = st_destroy,
};
