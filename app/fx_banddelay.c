// fx_banddelay.c - BAND DELAY: the Eventide H3000 "Band Delays" algorithm.
//
// FIDELITY NOTES (researched against the Eventide H3000 Band Delays User Guide,
// P/N 141254 Rev 4, signal-flow diagram + Expert page):
//   The hardware feeds a summed mono input into EIGHT parallel voices, each one
//   a chain  [output gain -> delay -> resonant band-pass filter -> pan]; the
//   eight pans are summed to L/R. Resonance/"ringing" is a property of the
//   band-pass filters themselves: the manual's Q Factor ranges 0.5..999 and
//   "the highest setting will oscillate for a long time after the audio source
//   is gone". So in the real unit the pitched ringing comes from very-high-Q
//   band-passes, not from per-band echo feedback.
//
//   FAITHFUL here: 8 parallel band-pass voices, each with its own delay tap and
//   pan; voices spread across a frequency range; resonance = filter Q; voices
//   panned across the stereo field; wet/dry mix; one global recirculating
//   feedback delay (the hardware has a single Feedback Delay block, independent
//   of the eight voice delays - manual 3.6.7).
//
//   MODELED / adapted (honest):
//     * The 8 centre frequencies are LOG-SPACED between LOW FREQ and HIGH FREQ
//       rather than 8 free per-voice knobs (the hardware exposes 8 freqs; we
//       expose the spread, which is the natural macro mapping for a small UI).
//     * Per-band FEEDBACK: in addition to the global feedback path, each band's
//       filtered output is fed back into its OWN delay tap. This is the
//       behaviour the module spec asks for ("resonant, pitched echoes") and it
//       layers extra rhythmic regeneration on top of the faithful filter
//       ringing. Combined with the global loop it is close in spirit, not a
//       sample-exact copy of the hardware's single-loop topology.
//     * Q is capped well below 999 for stability on the fixed offline render
//       (the engine asserts finite output, peak < 4.0); RESONANCE maps 0..1 to
//       a musical Q that still rings hard near the top.
//
// Engine contract: mono dry[n] in, interleaved-stereo outLR[2n] out. All state
// is allocated in create() and freed in destroy(); no global/static mutable
// state. Feedback is clamped < 1, filter states are denormal/NaN guarded, and
// the wet sum is soft-clipped so the resonant loops stay stable and click-free.
#include "h3000.h"
#include "dsputil.h"
#include <stdlib.h>
#include <math.h>

#define BD_BANDS    8
#define BD_MAX_MS   1100.0f   // headroom over the 1000 ms TIME max + stagger

// Parameter indices (MUST match the params[] order in the def below).
enum { P_TIME, P_FEEDBACK, P_RESONANCE, P_LOWFREQ, P_HIGHFREQ, P_MIX };

typedef struct {
    DelayLine line[BD_BANDS];   // one delay per voice (independent tap times)
    Biquad    bp[BD_BANDS];     // one resonant band-pass per voice
    float     fbk[BD_BANDS];    // last filtered output of each band (per-band FB)
    float     panL[BD_BANDS];   // constant-power pan gains, fixed at create()
    float     panR[BD_BANDS];
    DelayLine gfb;              // single global recirculating feedback delay
    float     gfb_last;         // last global-feedback delay output
    // cached coefficient inputs so we only rebuild biquads when knobs move
    float     c_q, c_lo, c_hi;
    int       rate;
} BdState;

// Flush denormals / NaNs to zero so the resonant feedback loops never stall on
// subnormal floats (huge CPU stalls) or poison the state with NaN.
static inline float bd_san(float x) {
    if (!(x == x)) return 0.0f;             // NaN
    if (x > -1.0e-20f && x < 1.0e-20f) return 0.0f;
    return x;
}

// Smooth bounded saturation for the wet sum: ~linear for |x|<1, asymptotes to
// +/-1.5. Tames the peaks when 8 high-Q bands ring in phase, no hard-clip click.
static inline float bd_softclip(float x) {
    const float lim = 1.5f;
    float a = x / lim;
    return lim * (a / (1.0f + 0.28f * a * a));
}

// RESONANCE 0..1 -> band-pass Q. Low end is gentle/wide, top end rings hard.
// Capped below the hardware's 999 for offline-render stability (finite, <4.0).
static inline float bd_res_to_q(float res) {
    if (res < 0.0f) res = 0.0f;
    if (res > 1.0f) res = 1.0f;
    return 0.7f + res * res * 34.0f;        // ~0.7 .. ~34.7, quadratic taper
}

// Log-spaced centre frequency for band k in [lo, hi], clamped to audio range.
static float bd_band_freq(int k, float lo, float hi, int rate) {
    if (lo < 20.0f) lo = 20.0f;
    if (hi <= lo)   hi = lo + 1.0f;
    float ny = 0.45f * (float)rate;         // stay below Nyquist
    if (hi > ny) hi = ny;
    if (lo > hi) lo = hi;
    float t = (BD_BANDS > 1) ? (float)k / (float)(BD_BANDS - 1) : 0.0f;
    return lo * powf(hi / lo, t);
}

static void bd_free_all(BdState *s) {
    if (!s) return;
    for (int k = 0; k < BD_BANDS; k++) dl_free(&s->line[k]);
    dl_free(&s->gfb);
    free(s);
}

static void *bd_create(int rate) {
    BdState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->rate = rate > 0 ? rate : 48000;
    for (int k = 0; k < BD_BANDS; k++) {
        if (!dl_init(&s->line[k], s->rate, BD_MAX_MS)) { bd_free_all(s); return NULL; }
        bq_reset(&s->bp[k]);
        s->fbk[k] = 0.0f;
        // Constant-power pan: alternate bands hard-ish L/R for stereo width.
        // Even bands lean left, odd bands lean right; magnitude grows with the
        // band index so the spread opens up across the spectrum.
        float spread = (BD_BANDS > 1) ? (float)k / (float)(BD_BANDS - 1) : 0.0f;
        float pos = (k & 1) ? (0.5f + 0.5f * spread)    // 0.5 .. 1.0 (right)
                            : (0.5f - 0.5f * spread);   // 0.5 .. 0.0 (left)
        float ang = pos * 1.57079633f;                  // 0..pi/2
        s->panL[k] = cosf(ang);
        s->panR[k] = sinf(ang);
    }
    if (!dl_init(&s->gfb, s->rate, BD_MAX_MS)) { bd_free_all(s); return NULL; }
    s->gfb_last = 0.0f;
    // Force a coefficient rebuild on the first block.
    s->c_q = s->c_lo = s->c_hi = -1.0f;
    return s;
}

static void bd_block(void *st, const float *dry, int n, const float *p, float *outLR) {
    BdState *s = (BdState *)st;
    int rate = s->rate;

    // --- read + clamp live params -------------------------------------------
    float time = p[P_TIME];
    if (time < 1.0f)    time = 1.0f;
    if (time > 1000.0f) time = 1000.0f;

    float fb = p[P_FEEDBACK];
    if (fb < 0.0f)   fb = 0.0f;
    if (fb > 0.9f)   fb = 0.9f;             // spec range; hard-guard < 1 below

    float res = p[P_RESONANCE];
    if (res < 0.0f) res = 0.0f;
    if (res > 1.0f) res = 1.0f;

    float lo = p[P_LOWFREQ];
    float hi = p[P_HIGHFREQ];

    float mix = p[P_MIX];
    if (mix < 0.0f) mix = 0.0f;
    if (mix > 1.0f) mix = 1.0f;

    // --- rebuild band-pass coeffs only when Q / freq spread changed ---------
    float q = bd_res_to_q(res);
    if (q != s->c_q || lo != s->c_lo || hi != s->c_hi) {
        for (int k = 0; k < BD_BANDS; k++) {
            float f = bd_band_freq(k, lo, hi, rate);
            bq_bandpass(&s->bp[k], f, q, rate);   // constant 0 dB peak band-pass
        }
        s->c_q = q; s->c_lo = lo; s->c_hi = hi;
    }

    // Per-band feedback sustains the resonant echo. Taper it only gently as Q
    // rises (the high-Q filter is itself near self-oscillation, so a little
    // recirculation goes a long way) and keep it strictly < 1 for stability.
    float fb_band = fb * (1.0f - 0.2f * res);
    if (fb_band > 0.9f)  fb_band = 0.9f;
    if (fb_band < 0.0f)  fb_band = 0.0f;

    // Global recirculation: kept a touch lower than the raw knob (it stacks with
    // every band's own loop) but still enough to repeat the multi-tap pattern.
    float fb_glob = fb * 0.6f;
    if (fb_glob > 0.85f) fb_glob = 0.85f;

    // RESONANCE compensation. A constant-0 dB-peak band-pass passes total energy
    // roughly proportional to its bandwidth (~1/Q), so high-Q bands would all
    // but vanish. The hardware does the opposite: higher Q rings LOUDER (manual:
    // "the higher the Q factor, the higher the gain through the filter ... can
    // easily be set to distort"). Boost each band ~sqrt(Q) (capped) to restore
    // that, then the final soft-clip keeps the in-phase sum bounded < 4.0.
    float band_gain = 0.34f * sqrtf(q);   // q in ~0.7..34.7 -> ~0.28 .. ~2.0
    if (band_gain > 2.1f) band_gain = 2.1f;

    // Per-band time stagger: band k tap = time * (1 + k*spread). Gives the
    // spectral-cascade smear the spec allows and the hardware's per-voice
    // differing delay times. Kept within the allocated BD_MAX_MS line.
    float band_ms[BD_BANDS];
    for (int k = 0; k < BD_BANDS; k++) {
        float t = time * (1.0f + 0.06f * (float)k);   // up to ~+42% on band 7
        if (t > BD_MAX_MS - 2.0f) t = BD_MAX_MS - 2.0f;
        band_ms[k] = t;
    }

    for (int i = 0; i < n; i++) {
        float in = dry[i];
        // Drive the bank with dry + the single global feedback tap.
        float drive = in + fb_glob * s->gfb_last;
        drive = bd_san(drive);

        float wetL = 0.0f, wetR = 0.0f, gfbIn = 0.0f;

        for (int k = 0; k < BD_BANDS; k++) {
            // Write (input + this band's own filtered feedback) into its delay.
            float w = drive + fb_band * s->fbk[k];
            dl_write(&s->line[k], bd_san(w));
            // Tap the delay, run it through the resonant band-pass.
            float d  = dl_read_ms(&s->line[k], band_ms[k], rate);
            float bo = bq_process(&s->bp[k], d);
            bo = bd_san(bo);
            // Soft-limit the per-band feedback memory so a single ringing band
            // can't diverge even at extreme Q (the biquad is 0 dB-peak but the
            // recirculation can still build).
            float fbv = bo;
            if (fbv >  1.5f) fbv =  1.5f;
            if (fbv < -1.5f) fbv = -1.5f;
            s->fbk[k] = fbv;

            float bg = bo * band_gain;
            wetL  += bg * s->panL[k];
            wetR  += bg * s->panR[k];
            gfbIn += bo;                // UN-boosted, for the global loop only
        }

        // Feed the global recirculation delay from the band sum NORMALIZED by the
        // band count (and un-boosted): the round-trip gain then stays ~fb_glob < 1
        // instead of ~fb_glob*8*band_gain, which self-oscillated at high FB/RES.
        dl_write(&s->gfb, bd_san(gfbIn * (1.0f / (float)BD_BANDS)));
        s->gfb_last = bd_san(dl_read_ms(&s->gfb, time, rate));

        // Soft-clip each wet channel, then wet/dry blend.
        wetL = bd_softclip(wetL);
        wetR = bd_softclip(wetR);

        outLR[i * 2 + 0] = mix * wetL + (1.0f - mix) * in;
        outLR[i * 2 + 1] = mix * wetR + (1.0f - mix) * in;
    }
}

static void bd_destroy(void *st) {
    bd_free_all((BdState *)st);
}

const H3kAlgoDef banddelay_def = {
    .name = "BAND DELAY",
    .nparams = 6,
    .params = {
        { "TIME",      1, 1000, 5,     200,   PK_MS,      0 },
        { "FEEDBACK",  0, 0.9f, 0.05f, 0.4f,  PK_PERCENT, 0 },
        { "RESONANCE", 0, 1,    0.05f, 0.6f,  PK_PERCENT, 0 },
        { "LOW FREQ",  100, 1000, 50,  200,   PK_HZ,      0 },
        { "HIGH FREQ", 2000, 12000, 250, 6000, PK_HZ,     0 },
        { "MIX",       0, 1,    0.05f, 0.5f,  PK_PERCENT, 0 },
    },
    .create = bd_create,
    .block = bd_block,
    .destroy = bd_destroy,
};
