// fx_reverse.c - REVERSE SHIFT / CRYSTAL. The H3000 "Reverse Shift" algorithm
// (the engine behind the famous "Crystal Echoes" preset, modernized by Soundtoys
// as Crystallizer). The input is captured in SEGMENT-long slices; each slice is
// played BACKWARD, pitch-shifted, and the reversed+pitched output is fed back
// into the input so every echo gets re-reversed and re-pitched -> the spiraling
// "crystal" cascade.
//
// FAITHFUL (matches the hardware / Crystallizer manual):
//   - Slice capture + backward playback, with the natural ~one-segment onset
//     latency a reverse-delay has ("Splice 1000ms => ~1000ms before the effect",
//     Crystallizer manual p.6-7). It falls out of the capture-then-reverse scheme.
//   - Pitch applied to the reversed stream regardless of direction (manual p.9),
//     using pitchcore's old-school resample+crossfade shifter -- the same
//     "simpler resample and crossfade" / glitchy retro shifter the manual credits
//     for Crystallizer's character. So the pitch path is faithful, not modeled.
//   - Recycle/feedback: wet fed back to input -> cascading, spiraling pitch
//     (manual: "Splice 500ms+, Pitch 1200, decent recycle = classic Crystal
//     Echoes preset", p.7). The SPLICE de-glitch character is exposed.
//   - Click-free segment joins via an equal-power crossfade across the boundary,
//     i.e. the "Smoothing" crossfade-between-splices behavior (manual p.12).
//
// MODELED / SIMPLIFIED (honest):
//   - One mono reverse voice (the hardware is one-in/two-out with two independent
//     reverse voices); we emit the single wet voice to both channels (a faithful
//     mono program), MIX-blended against the centered dry. No per-side Pitch/
//     Splice/Delay offsets, Threshold/Gate/Duck, or Low/High-cut from the plugin.
//   - Crossfade length is a fixed fraction (~15%) of the segment, not a separate
//     Smoothing knob.
//
// Implementation: a single continuous capture ring written forward, plus up to
// two overlapping backward "grains" (read heads) scheduled so each new grain
// fades in (equal-power) over the same window the previous grain fades out ->
// click-free segment joins (the spec's two-read-heads-crossfaded approach). All
// of this is local; only pitchcore.h / dsputil.h / h3000.h symbols are used. No
// global/static mutable state; all buffers allocated in create, freed in destroy.
#include "h3000.h"
#include "pitchcore.h"
#include "dsputil.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

// Parameter indices -- MUST match the params[] order in reverse_def below.
enum { P_SEGMENT, P_PITCH, P_FEEDBACK, P_MIX, P_SPLICE };

// Hard ceilings so the work buffers are sized once in create() and the live
// SEGMENT param can never read past them.
#define SEG_MAX_MS   1000.0f   // matches the SEGMENT param max
#define XFADE_FRAC   0.15f     // crossfade region as a fraction of the segment
#define FB_LIMIT     4.0f      // safety clamp on the feedback sample (anti-runaway)

typedef struct {
    PitchVoice *pv;        // reversed-stream pitch shifter (faithful retro shifter)
    int   rate;

    // Reverse capture: one continuous ring. We write the (input + feedback)
    // stream forward into it; `wr` is the running absolute write count (samples
    // ever written) and wr % capLen is the live index. A reverse "grain"
    // launched at write-count W plays the slice spanning absolute times
    // [W-seg, W) BACKWARD over `seg` samples (newest sample first). Because that
    // slice was fully captured *before* the grain launches and capture only ever
    // advances forward, the read region never collides with the write region;
    // the ring just needs to hold ~2 segments of live data.
    float *cap;            // capture ring
    int    segMax;         // per-slice capacity (samples) at SEG_MAX_MS
    int    capLen;         // ring length (>= 2*segMax + guard)
    long   wr;             // running absolute write count

    // Up to two simultaneous backward grains. They overlap by the crossfade
    // length `xf`: the next grain launches `xf` samples before the current one
    // ends, so their equal-power fade-out / fade-in regions coincide and the
    // segment join is click-free. Hop between launches = seg - xf.
    long   gStart[2];      // absolute write-count at which the grain launched
    int    gSeg[2];        // grain length (samples), frozen at launch
    int    gXf[2];         // crossfade length (samples), frozen at launch
    int    gOn[2];         // grain active?
    long   nextLaunch;     // absolute write-count of the next grain launch
    int    slot;           // which grain slot to (re)use next (toggles 0/1)
    int    primed;         // capture has filled the first segment yet?

    float  fbState;        // last reversed+pitched sample, for the feedback path
} RvState;

// Scrub NaN/Inf/denormals to 0. Done on the raw IEEE-754 bits so it stays
// correct under -ffast-math/-Ofast (the device build), where the compiler may
// assume float compares like (x==x) never see a NaN and optimize them away.
static float flush_denorm(float x) {
    uint32_t u;
    memcpy(&u, &x, sizeof(u));
    uint32_t exp = (u >> 23) & 0xFF;
    if (exp == 0xFF) return 0.0f;   // NaN or Inf  -> 0
    if (exp == 0)    return 0.0f;   // zero/denormal -> 0 (kills denormal stalls)
    return x;
}

// Equal-power fade weight for phase t in [0,1] (0 = silent, 1 = full).
static float eqp_up(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return sinf(t * 1.5707963267948966f);   // sin(t*pi/2)
}

// Read one backward grain at the current write-count `now`. The grain plays the
// captured slice [gStart-seg, gStart) from newest -> oldest as its age advances
// (one sample per output sample, so sample-accurate), weighted by an equal-power
// in/out envelope so that where two grains overlap their powers sum to ~unity
// (click-free splice join).
static float read_grain(const RvState *s, int g, long now) {
    if (!s->gOn[g]) return 0.0f;
    int seg = s->gSeg[g];
    if (seg < 2) return 0.0f;
    long age = now - s->gStart[g];
    if (age < 0 || age >= seg) return 0.0f;

    // Backward read: age 0 -> newest sample (abs time gStart-1),
    //                age seg-1 -> oldest sample (abs time gStart-seg). The read
    // advances exactly one sample per output sample, so it is sample-accurate
    // (no fractional interpolation needed here; the pitch path handles resampling).
    long rabs = s->gStart[g] - 1 - age;          // absolute time being read
    long ipos = rabs % s->capLen;
    if (ipos < 0) ipos += s->capLen;
    float smp = s->cap[(int)ipos];

    // Equal-power envelope: fade in over the first xf samples, out over the last.
    int xf = s->gXf[g];
    if (xf < 1) xf = 1;
    if (xf > seg / 2) xf = seg / 2;
    float gain = 1.0f;
    if (age < xf)                  gain = eqp_up((float)age / (float)xf);
    else if (age >= seg - xf)      gain = eqp_up((float)(seg - 1 - age) / (float)xf);
    return smp * gain;
}

static void *rv_create(int rate) {
    RvState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->rate = rate > 0 ? rate : 48000;
    s->pv = pv_create(s->rate);
    if (!s->pv) { free(s); return NULL; }

    s->segMax = (int)(SEG_MAX_MS * 0.001f * (float)s->rate) + 1;
    if (s->segMax < 2) s->segMax = 2;
    // Hold ~2 segments of live data (read slice + write slice never overlap),
    // plus a one-segment guard so a live param change to a larger segment can
    // never make a grain read into the region currently being written.
    s->capLen = 3 * s->segMax + 16;
    s->cap = calloc((size_t)s->capLen, sizeof(float));
    if (!s->cap) { pv_destroy(s->pv); free(s); return NULL; }

    s->wr = 0;
    s->gOn[0] = s->gOn[1] = 0;
    s->gStart[0] = s->gStart[1] = 0;
    s->nextLaunch = 0;             // set once capture has primed one segment
    s->slot = 0;
    s->primed = 0;
    s->fbState = 0.0f;
    return s;
}

static void rv_block(void *st, const float *dry, int n, const float *p, float *outLR) {
    RvState *s = (RvState *)st;

    // --- read + clamp live params -------------------------------------------
    float segMs = p[P_SEGMENT];
    if (segMs < 10.0f) segMs = 10.0f;
    if (segMs > SEG_MAX_MS) segMs = SEG_MAX_MS;
    int seg = (int)(segMs * 0.001f * (float)s->rate);
    if (seg < 2) seg = 2;
    if (seg > s->segMax) seg = s->segMax;
    int xf = (int)(XFADE_FRAC * (float)seg);     // crossfade region (~15% of seg)
    if (xf < 1) xf = 1;
    if (xf > seg / 2) xf = seg / 2;
    int hop = seg - xf;                          // launch spacing (overlap = xf)
    if (hop < 1) hop = 1;

    float semis = p[P_PITCH];
    if (semis < -24.0f) semis = -24.0f;
    if (semis >  24.0f) semis =  24.0f;
    pv_set_cents(s->pv, semis * 100.0f);

    float fb = p[P_FEEDBACK];
    if (fb < 0.0f) fb = 0.0f;
    if (fb > 0.9f) fb = 0.9f;          // spec cap; strictly < 1.0 (no runaway)

    float mix = p[P_MIX];
    if (mix < 0.0f) mix = 0.0f;
    if (mix > 1.0f) mix = 1.0f;

    int sp = (int)(p[P_SPLICE] + 0.5f);
    if (sp < 0) sp = 0;
    if (sp >= SPLICE_COUNT) sp = SPLICE_COUNT - 1;
    pv_set_splice(s->pv, sp);

    for (int i = 0; i < n; i++) {
        float in = dry[i];

        // 1) Sum input with the recycled wet and capture it forward into the
        //    ring. fbState is clamped/flushed so the feedback path can never
        //    diverge or carry NaN/denormals into the buffer (-> crystal cascade
        //    that decays instead of blowing up).
        float fed = in + fb * s->fbState;
        fed = flush_denorm(fed);
        if (fed >  FB_LIMIT) fed =  FB_LIMIT;
        if (fed < -FB_LIMIT) fed = -FB_LIMIT;
        s->cap[(int)(s->wr % s->capLen)] = fed;
        s->wr++;                               // now wr = count of samples written

        // 2) Once the first segment is captured, schedule reverse grains. Each
        //    launches `hop` (= seg - xf) after the previous so the equal-power
        //    fade-out of the old grain overlaps the fade-in of the new one by
        //    `xf` samples -> a click-free segment join. The launch reads the
        //    just-captured slice [wr-seg, wr), played backward.
        if (!s->primed && s->wr >= seg) {
            s->primed = 1;
            s->nextLaunch = s->wr;             // first grain launches now
        }
        if (s->primed && s->wr >= s->nextLaunch) {
            int g = s->slot;
            // If the slot we want is still finishing, fall back to the other one
            // so we never truncate a grain mid-crossfade.
            if (s->gOn[g] && (s->wr - s->gStart[g]) < s->gSeg[g] - s->gXf[g])
                g ^= 1;
            s->gStart[g] = s->wr;              // reads slice [wr-seg, wr) backward
            s->gSeg[g]   = seg;
            s->gXf[g]    = xf;
            s->gOn[g]    = 1;
            s->slot      = g ^ 1;              // alternate slots
            s->nextLaunch = s->wr + hop;
        }

        // 3) Sum the (up to two overlapping) backward grains, evaluated at the
        //    current write-count.
        float rev = read_grain(s, 0, s->wr) + read_grain(s, 1, s->wr);
        rev = flush_denorm(rev);
        // Retire grains that have run their length.
        for (int g = 0; g < 2; g++)
            if (s->gOn[g] && (s->wr - s->gStart[g]) >= s->gSeg[g]) s->gOn[g] = 0;

        // 4) Pitch-shift the reversed stream (faithful old-school resample +
        //    crossfade shifter via pitchcore). Pitch is applied to the reversed
        //    signal, matching the hardware/Crystallizer behavior.
        float wet = pv_process(s->pv, rev);
        wet = flush_denorm(wet);
        s->fbState = wet;

        // 5) Mono wet to both channels, MIX-blended against the centered dry.
        float l = mix * wet + (1.0f - mix) * in;
        outLR[i * 2 + 0] = flush_denorm(l);
        outLR[i * 2 + 1] = flush_denorm(l);
    }
}

static void rv_destroy(void *st) {
    RvState *s = (RvState *)st;
    if (!s) return;
    pv_destroy(s->pv);
    free(s->cap);
    free(s);
}

const H3kAlgoDef reverse_def = {
    .name = "REVERSE",
    .nparams = 5,
    .params = {
        { "SEGMENT",  50, 1000,   10,    250,  PK_MS,      0 },
        { "PITCH",   -24,   24,    1,     12,   PK_SEMI,    0 },
        { "FEEDBACK",  0,  0.9f, 0.05f,   0.3f, PK_PERCENT, 0 },
        { "MIX",       0,    1,  0.05f,   0.5f, PK_PERCENT, 0 },
        { "SPLICE",    0,    3,    1,      0,    PK_CHOICE,  PITCH_SPLICE_CHOICES },
    },
    .create = rv_create,
    .block = rv_block,
    .destroy = rv_destroy,
};
