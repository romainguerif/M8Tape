// fx_delay.c - DUAL DELAY / Long Digiplex: a stereo delay with two independent
// delay lines (L and R), feedback with a tone (low-pass) filter in each
// feedback path, optional ping-pong cross-feed, and wet/dry mix.
//
// Fidelity notes (Eventide H3000 Digiplex family):
//   - FAITHFUL: structure. The H3000 "Dual Digiplex" is two independent delay
//     lines — left output = delay 1, right output = delay 2 — and "Long
//     Digiplex" is the same engine with a longer single line (~1.4 s) and
//     feedback that can loop forever. We expose independent L/R times up to
//     1500 ms and feedback up to 0.9, matching that behavior.
//   - FAITHFUL: ping-pong. H3000 ping-pong presets are built by having the two
//     delay lines feed one another; our PING-PONG=ON cross-routes each line's
//     output into the OTHER line's feedback input, so echoes bounce L<->R.
//   - MODELED: the TONE control. The real Digiplex is famously "extremely
//     clean" (16-bit/44.1k, no tape-style darkening). A high-frequency damping
//     in the feedback path is our addition to get a tape/Digiplex-ish decay; it
//     lives ONLY in the feedback loop, so the dry signal and the first tap stay
//     clean and only successive repeats progressively darken. Set TONE high
//     (toward 18 kHz) for the clean hardware character.
//
// Contract (see h3000.h): mono dry[n] in -> interleaved stereo outLR[2n] out.
// All state is allocated in create() and freed in destroy(); no global/static
// mutable state. Feedback is hard-clamped below 1.0 and the feedback state is
// sanitized every sample to guard against NaN/Inf and denormal stalls.
#include "h3000.h"
#include "dsputil.h"
#include <stdlib.h>
#include <math.h>

// Parameter indices (must match the params[] order in the def below).
enum { P_TIME_L, P_TIME_R, P_FEEDBACK, P_TONE, P_PINGPONG, P_MIX };

#define DLY_MAX_MS    1500.0f  // matches the TIME L/R range below
#define DLY_FB_CEIL   0.95f    // absolute safety ceiling on feedback (< 1.0)
#define DLY_DENORM    1.0e-20f // flush magnitudes below this to 0

static const char *const PINGPONG_CHOICES[] = { "OFF", "ON", 0 };

typedef struct {
    DelayLine dl, dr;     // left / right delay lines
    OnePole   tl, tr;     // tone (low-pass) in each feedback path
    float     fbL, fbR;   // last filtered output of each line (feedback state)
    float     toneHz;     // cached tone cutoff; recompute coeffs only on change
    int       rate;
} DlyState;

// Flush denormals and replace any non-finite value with 0 so a runaway/NaN in
// the feedback loop can never poison the rest of the render.
static inline float dly_san(float x) {
    if (!isfinite(x)) return 0.0f;
    if (x > -DLY_DENORM && x < DLY_DENORM) return 0.0f;
    return x;
}

static void *dly_create(int rate) {
    DlyState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->rate = rate > 0 ? rate : 48000;
    if (!dl_init(&s->dl, s->rate, DLY_MAX_MS) ||
        !dl_init(&s->dr, s->rate, DLY_MAX_MS)) {
        dl_free(&s->dl); dl_free(&s->dr); free(s);
        return NULL;
    }
    s->toneHz = 6000.0f;
    op_set_lp(&s->tl, s->toneHz, s->rate);
    op_set_lp(&s->tr, s->toneHz, s->rate);
    return s;
}

static void dly_block(void *st, const float *dry, int n, const float *p, float *outLR) {
    DlyState *s = (DlyState *)st;

    // --- read + clamp live params ---
    float tL = p[P_TIME_L]; if (tL < 0) tL = 0; if (tL > DLY_MAX_MS) tL = DLY_MAX_MS;
    float tR = p[P_TIME_R]; if (tR < 0) tR = 0; if (tR > DLY_MAX_MS) tR = DLY_MAX_MS;
    float fb = p[P_FEEDBACK]; if (fb < 0) fb = 0; if (fb > DLY_FB_CEIL) fb = DLY_FB_CEIL;
    float mix = p[P_MIX]; if (mix < 0) mix = 0; if (mix > 1) mix = 1;

    // Tone: recompute one-pole coeffs only when the cutoff actually changes
    // (cheap, click-free — a one-pole's coeff swap doesn't discontinue state).
    float tone = p[P_TONE];
    if (tone < 1.0f) tone = 1.0f;
    if (tone > (float)s->rate * 0.49f) tone = (float)s->rate * 0.49f;
    if (tone != s->toneHz) {
        s->toneHz = tone;
        op_set_lp(&s->tl, tone, s->rate);
        op_set_lp(&s->tr, tone, s->rate);
    }

    int ping = (int)(p[P_PINGPONG] + 0.5f);  // 0 = OFF, 1 = ON

    for (int i = 0; i < n; i++) {
        float in = dry[i];

        // Feedback routing: straight (each line feeds itself) or ping-pong
        // (each line is fed by the OTHER line's output, so echoes bounce L<->R).
        // The dry input is injected into BOTH lines so a mono source seeds the
        // stereo field; ping-pong then carries it across on each repeat.
        float feedL = ping ? s->fbR : s->fbL;
        float feedR = ping ? s->fbL : s->fbR;

        // Straight: the dry seeds both lines. Ping-pong: the dry seeds the LEFT
        // line ONLY, so a mono source enters left then bounces L->R->L via the
        // cross-feedback (seeding both kept them symmetric -> ping-pong was a no-op).
        float inR = ping ? 0.0f : in;
        dl_write(&s->dl, in  + fb * feedL);
        dl_write(&s->dr, inR + fb * feedR);

        float wL = dl_read_ms(&s->dl, tL, s->rate);
        float wR = dl_read_ms(&s->dr, tR, s->rate);

        // Tone filter lives in the feedback path only: filter the wet tap before
        // it re-enters the loop, so repeats darken but the live tap stays clean.
        s->fbL = dly_san(op_lp(&s->tl, wL));
        s->fbR = dly_san(op_lp(&s->tr, wR));

        outLR[i * 2 + 0] = (1.0f - mix) * in + mix * wL;
        outLR[i * 2 + 1] = (1.0f - mix) * in + mix * wR;
    }
}

static void dly_destroy(void *st) {
    DlyState *s = (DlyState *)st;
    if (!s) return;
    dl_free(&s->dl);
    dl_free(&s->dr);
    free(s);
}

const H3kAlgoDef delay_def = {
    .name = "DUAL DELAY",
    .nparams = 6,
    .params = {
        { "TIME L",    1, 1500,  10,    300,   PK_MS,      0 },
        { "TIME R",    1, 1500,  10,    450,   PK_MS,      0 },
        { "FEEDBACK",  0, 0.9f,  0.05f, 0.35f, PK_PERCENT, 0 },
        { "TONE",    500, 18000, 500,   6000,  PK_HZ,      0 },
        { "PING-PONG", 0, 1,     1,     0,     PK_CHOICE,  PINGPONG_CHOICES },
        { "MIX",       0, 1,     0.05f, 0.4f,  PK_PERCENT, 0 },
    },
    .create = dly_create,
    .block = dly_block,
    .destroy = dly_destroy,
};
