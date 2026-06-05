// fx_stutter.c - STUTTER. The classic Eventide H3000 glitch "Stutter" algorithm
// (alg 112; the lineage that became "Max Stutter"): rhythmic capture-and-replay
// of a buffer. The input is continuously captured into a ring; on a periodic
// grid (every INTERVAL) a SLICE-length chunk is grabbed and replayed REPEATS
// times back-to-back -- the stuttering/glitch -- with an optional level DECAY
// per repeat, after which the live input passes through again until the next
// trigger fires. Output is 1:1 (the stuttered stream replaces the dry while a
// burst is playing); between bursts the live input is heard.
//
// FAITHFUL to the classic stutter idea ("capture/replay de buffer rythme"):
//   - Continuous forward capture ring + periodic SLICE grab + REPEATS replays.
//   - Decaying repeats (level drop per repeat) -- the familiar trailing-off
//     stutter tail.
//   - Rhythmic INTERVAL grid between bursts, live signal in the gaps.
//
// MODELED / SIMPLIFIED (honest): the H3000's actual stutter was tempo/MIDI
// clockable with its own internal trigger logic; here the grid is a plain
// free-running INTERVAL in ms (the render is offline, so there is no host clock
// to lock to). One mono program emitted centered to both channels. No pitch /
// reverse / random-order per slice (those are later/other algorithms).
//
// CLICK-FREE (the #1 priority, and the lesson from this codebase's earlier
// reverse-grain effect that clicked): every audible junction is an EQUAL-POWER
// crossfade. The junctions are (a) live -> first repeat, (b) repeat -> next
// repeat (the slice loops back to its head), (c) last repeat -> live. Each
// repeat is rendered as a grain that is `slice + xf` long, spaced `slice`
// apart, so consecutive grains overlap by `xf`; their sin/cos envelopes sum to
// unit power across the overlap -> no click. The live signal participates as
// the grain before the first repeat and after the last, so burst entry/exit
// crossfade against the live input the same way. (Same proven overlap-add
// scheme as fx_reverse.c, but forward-playing repeated slices.)
//
// All buffers are allocated in create and freed in destroy. No global/static
// mutable state. Only h3000.h / dsputil.h symbols are used; everything else is
// local. NaN/Inf/denormals are scrubbed on the raw bits so it stays correct
// under -ffast-math/-Ofast (the device build).
#include "h3000.h"
#include "dsputil.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

// Parameter indices -- MUST match the params[] order in stutter_def below.
enum { P_SLICE, P_REPEATS, P_DECAY, P_INTERVAL, P_XFADE };

// Hard ceilings so the work buffers are sized once in create() and the live
// params can never read past them. The capture ring holds the longest slice we
// can grab plus headroom so the read region (the captured slice, frozen at
// burst start) never collides with the ongoing forward write.
#define SLICE_MAX_MS  500.0f   // matches the SLICE param max
#define XFADE_MAX_MS   40.0f   // matches the XFADE param max (per-join crossfade)

// Equal-power fade-in weight for phase t in [0,1] (0 = silent, 1 = full).
// Paired with eqp_dn so an overlap's two weights satisfy up^2 + dn^2 == 1.
static float eqp_up(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return sinf(t * 1.5707963267948966f);   // sin(t * pi/2)
}
static float eqp_dn(float t) {
    if (t <= 0.0f) return 1.0f;
    if (t >= 1.0f) return 0.0f;
    return cosf(t * 1.5707963267948966f);   // cos(t * pi/2)
}

// Scrub NaN/Inf/denormals to 0 on the raw IEEE-754 bits (correct even when the
// compiler assumes x==x can't be false under fast-math).
static float flush_denorm(float x) {
    uint32_t u;
    memcpy(&u, &x, sizeof(u));
    uint32_t e = (u >> 23) & 0xFFu;
    if (e == 0xFFu) return 0.0f;   // NaN / Inf -> 0
    if (e == 0u)    return 0.0f;   // zero / denormal -> 0
    return x;
}

typedef struct {
    int    rate;

    // Continuous forward capture ring. `wr` is the running absolute write count
    // (samples ever written); wr % capLen is the live index. When a burst is
    // armed we freeze `origin` = the absolute capture time of the slice's first
    // sample (= the latest fully-captured slice, i.e. wr - slice), so the read
    // region [origin, origin+slice) lies wholly in already-written history and
    // never chases the write head.
    float *cap;
    int    sliceMax;     // per-slice capacity (samples) at SLICE_MAX_MS
    int    xfMax;        // max crossfade (samples) at XFADE_MAX_MS
    int    capLen;       // ring length, sized to hold history + guard
    long   wr;           // running absolute write count

    // Free-running trigger grid: fire a burst every `intvl` samples.
    long   nextTrig;     // absolute write count at which the next burst arms

    // Active burst state (all frozen at arm time so live param edits never
    // tear an in-flight burst; the next burst picks up new values).
    int    active;       // a burst is currently playing?
    long   origin;       // abs capture time of the slice's first sample
    int    bSlice;       // slice length (samples), frozen at arm
    int    bXf;          // crossfade length (samples), frozen at arm
    int    bReps;        // repeat count, frozen at arm
    float  bDecay;       // level drop per repeat (0..0.9), frozen at arm
    long   bStart;       // abs write count at which the burst began playing
} StState;

// Read the captured slice for repeat `k` of the active burst, at burst-relative
// output sample `rel` (0-based within the whole burst timeline).
//
// Geometry (the proven click-free overlap-add scheme, same as fx_reverse.c):
// each repeat is a grain `slice` samples long that plays the captured slice
// STRAIGHT from head to tail (read offset = age, no wrap), shaped by an
// equal-power window that fades IN over its first xf and OUT over its last xf.
// Consecutive repeats are spaced hop = slice - xf apart, so grain k's fade-out
// window exactly coincides with grain k+1's fade-in window; the two sin/cos
// envelopes sum to unit power, so even though the tail of one grain and the
// head of the next are different slice content, both are enveloped to low
// amplitude at the crossover -> no click and no audible level dip. Crucially the
// read pointer NEVER jumps mid-grain (a tail->head wrap is exactly what clicks),
// so there is no discontinuity inside a grain either. Returns the windowed,
// decayed sample; 0 outside the grain's span.
static float read_repeat(const StState *s, int k, long rel) {
    int slice = s->bSlice;
    if (slice < 2) return 0.0f;
    int xf = s->bXf;
    if (xf < 1) xf = 1;
    if (xf > slice / 2) xf = slice / 2;
    int hop = slice - xf;                 // launch spacing (overlap = xf)
    if (hop < 1) hop = 1;

    long gstart = (long)k * hop;          // grain onset (burst-relative)
    long age = rel - gstart;              // position within this grain [0,slice)
    if (age < 0 || age >= slice) return 0.0f;

    // Straight read through the captured slice, one sample per output sample.
    long rabs = s->origin + age;          // absolute capture time to read
    long ipos = rabs % s->capLen;
    if (ipos < 0) ipos += s->capLen;
    float smp = s->cap[(int)ipos];

    // Equal-power window: fade in over first xf, hold, fade out over last xf.
    float gain = 1.0f;
    if (age < xf)
        gain = eqp_up((float)age / (float)xf);
    else if (age >= slice - xf)
        gain = eqp_dn((float)(age - (slice - xf)) / (float)xf);

    // Per-repeat level decay (linear drop; clamped >= 0). Repeat 0 is full level.
    float lvl = 1.0f - s->bDecay * (float)k;
    if (lvl < 0.0f) lvl = 0.0f;

    return smp * gain * lvl;
}

static void *st_create(int rate) {
    StState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->rate = rate > 0 ? rate : 48000;

    s->sliceMax = (int)(SLICE_MAX_MS * 0.001f * (float)s->rate) + 1;
    if (s->sliceMax < 2) s->sliceMax = 2;
    s->xfMax = (int)(XFADE_MAX_MS * 0.001f * (float)s->rate) + 1;
    if (s->xfMax < 1) s->xfMax = 1;

    // Ring sizing. A burst freezes the read region [origin, origin+slice) where
    // origin = wr - slice at arm time, then KEEPS that region fixed while the
    // burst plays. Meanwhile capture writes forward; the write head must not lap
    // around and overwrite that frozen region before the burst finishes reading
    // it. Worst-case burst length = (REPEATS-1)*hop + slice, maximized when the
    // crossfade is tiny (hop -> slice): up to 16*slice samples. The oldest read
    // sample sits `slice` behind arm, so when the last read happens the write
    // head is ~(16*slice) past arm, i.e. ~(16*slice + slice) past the oldest read
    // sample. Hold strictly more than that: 17 slices + crossfade + guard. At
    // 48 kHz / 500 ms this is ~1.6 MB -- fine on the 1 GB device.
    s->capLen = 17 * s->sliceMax + 2 * s->xfMax + 64;
    s->cap = calloc((size_t)s->capLen, sizeof(float));
    if (!s->cap) { free(s); return NULL; }

    s->wr = 0;
    s->nextTrig = 0;     // first burst arms once enough history exists (see block)
    s->active = 0;
    s->origin = 0;
    s->bSlice = 0; s->bXf = 0; s->bReps = 0; s->bDecay = 0.0f; s->bStart = 0;
    return s;
}

static void st_block(void *st, const float *dry, int n, const float *p, float *outLR) {
    StState *s = (StState *)st;

    // --- read + clamp live params (used to arm the NEXT burst) ---------------
    float sliceMs = p[P_SLICE];
    if (sliceMs < 20.0f)         sliceMs = 20.0f;
    if (sliceMs > SLICE_MAX_MS)  sliceMs = SLICE_MAX_MS;
    int slice = (int)(sliceMs * 0.001f * (float)s->rate);
    if (slice < 2) slice = 2;
    if (slice > s->sliceMax) slice = s->sliceMax;

    int reps = (int)(p[P_REPEATS] + 0.5f);
    if (reps < 1)  reps = 1;
    if (reps > 16) reps = 16;

    float decay = p[P_DECAY];
    if (decay < 0.0f) decay = 0.0f;
    if (decay > 0.9f) decay = 0.9f;

    float intvlMs = p[P_INTERVAL];
    if (intvlMs < 100.0f)  intvlMs = 100.0f;
    if (intvlMs > 2000.0f) intvlMs = 2000.0f;
    long intvl = (long)(intvlMs * 0.001f * (float)s->rate);
    if (intvl < 1) intvl = 1;

    float xfMs = p[P_XFADE];
    if (xfMs < 1.0f)         xfMs = 1.0f;
    if (xfMs > XFADE_MAX_MS) xfMs = XFADE_MAX_MS;
    int xf = (int)(xfMs * 0.001f * (float)s->rate);
    if (xf < 1) xf = 1;
    if (xf > s->xfMax) xf = s->xfMax;
    if (xf > slice / 2) xf = slice / 2;   // crossfade can't exceed half a slice
    if (xf < 1) xf = 1;

    for (int i = 0; i < n; i++) {
        float in = flush_denorm(dry[i]);

        // 1) Capture the live input forward into the ring (always running).
        s->cap[(int)(s->wr % s->capLen)] = in;
        s->wr++;                                   // wr = count of samples written

        // 2) Arm a burst on the grid, once enough history exists to grab a full
        //    slice. The first trigger is delayed until wr >= slice so the very
        //    first burst has a complete slice to replay.
        if (s->nextTrig < slice) s->nextTrig = slice;
        if (!s->active && s->wr >= s->nextTrig) {
            // Freeze the latest fully-captured slice [wr-slice, wr) and all the
            // burst params at this instant.
            s->active = 1;
            s->bSlice = slice;
            s->bXf    = xf;
            s->bReps  = reps;
            s->bDecay = decay;
            s->origin = s->wr - slice;             // first sample of the slice
            s->bStart = s->wr;                     // burst playback starts now
            s->nextTrig = s->wr + intvl;           // schedule the following burst
        }

        // 3) Produce this sample.
        //    Burst timeline (burst-relative `rel`, 0-based): repeats are grains
        //    `slice` long spaced hop = slice - xf apart (so they overlap by xf),
        //    each fading in/out by xf. Repeat k occupies [k*hop, k*hop+slice).
        //    The LIVE input is the complementary "grain": it fades OUT over
        //    repeat 0's fade-in window [0,xf) and fades back IN over the last
        //    repeat's fade-out window, so burst entry and exit crossfade against
        //    live audio with the same equal-power law as every repeat->repeat
        //    join -> click-free at every junction. Interior: live fully muted.
        float out;
        if (s->active) {
            long rel = s->wr - s->bStart;          // 0-based within the burst

            // Recompute the frozen geometry (matches read_repeat exactly).
            int xfc = s->bXf;
            if (xfc < 1) xfc = 1;
            if (xfc > s->bSlice / 2) xfc = s->bSlice / 2;
            int hop = s->bSlice - xfc;
            if (hop < 1) hop = 1;
            long lastOnset = (long)(s->bReps - 1) * hop;      // last grain start
            long burstLen  = lastOnset + s->bSlice;           // last grain end (exclusive)
            long exitStart = burstLen - xfc;                  // last grain's fade-out begins

            // Sum all repeat grains overlapping `rel` (at most two at any time).
            float wet = 0.0f;
            for (int k = 0; k < s->bReps; k++)
                wet += read_repeat(s, k, rel);

            // Live crossfade weight at the burst edges.
            float liveW = 0.0f;
            if (rel < xfc)
                liveW = eqp_dn((float)rel / (float)xfc);                 // fade out at entry
            else if (rel >= exitStart)
                liveW = eqp_up((float)(rel - exitStart) / (float)xfc);   // fade in at exit

            out = wet + liveW * in;

            // The burst ends once the last grain has fully played out and live is
            // restored (rel == burstLen -> liveW back to 1, wet == 0).
            if (rel >= burstLen) {
                s->active = 0;
                // Guard: never let the grid arm the next burst in the past.
                if (s->nextTrig <= s->wr) s->nextTrig = s->wr + 1;
            }
        } else {
            out = in;                              // live pass-through between bursts
        }

        out = flush_denorm(out);
        outLR[i * 2 + 0] = out;                    // centered: same to both channels
        outLR[i * 2 + 1] = out;
    }
}

static void st_destroy(void *st) {
    StState *s = (StState *)st;
    if (!s) return;
    free(s->cap);
    free(s);
}

const H3kAlgoDef stutter_def = {
    .name = "STUTTER",
    .nparams = 5,
    .params = {
        { "SLICE",    20,  500,   5,    120,  PK_MS,      0 },  // chunk length grabbed
        { "REPEATS",   1,   16,   1,      4,  PK_INT,     0 },  // times the slice replays
        { "DECAY",     0, 0.9f, 0.05f,    0,  PK_PERCENT, 0 },  // level drop per repeat
        { "INTERVAL",100, 2000,  10,    500,  PK_MS,      0 },  // time between bursts
        { "XFADE",     1,   40,   1,      8,  PK_MS,      0 },  // per-join crossfade (click-free)
    },
    .create = st_create,
    .block = st_block,
    .destroy = st_destroy,
};
