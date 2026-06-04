// h3000.h - Eventide H3000-style DSP engine for M8Tape Studio.
// Faithful to the hardware's mono-program model: one pass = one algorithm,
// rendered destructively on the sample. First algorithm: MicroPitch.
#ifndef H3000_H
#define H3000_H

#include "wav.h"   // Audio

// --- splice character (pitch-shift de-glitch) --------------------------------
// The H3000's pitch core reads a delay line with two taps a window apart and
// crossfades at the splice. WHERE the splice lands defines the grain:
//   H910   = no de-glitch, fixed window + LC-clock drift (the 1975 character).
//   H949-1 = autocorrelation de-glitch (1977 ALG-3): the splice interval is
//            chosen by normalized cross-correlation so the incoming tap is in
//            phase with the outgoing one during the crossfade (least cancellation).
//   H949-2 = de-glitch with a longer correlation window + crossfade (smoother).
//   MODERN = finest de-glitch search + longest crossfade (cleanest).
// These mirror the four Splice Types Eventide exposes in the modern H90's Pitch
// algorithm. The exact #1/#2/Modern internals were never published — modeled
// here from function (search resolution, crossfade length, drift), not specs.
typedef enum {
    SPLICE_H910 = 0,
    SPLICE_H949_1,
    SPLICE_H949_2,
    SPLICE_MODERN,
    SPLICE_COUNT
} SpliceMode;
const char *h3000_splice_name(int mode);

// --- MicroPitch: two finely-detuned pitch voices, delayed, panned L/R --------
typedef struct {
    float cents_a, delay_a_ms;   // voice A → left
    float cents_b, delay_b_ms;   // voice B → right
    float feedback;              // 0..0.95 (per voice)
    float mix;                   // 0..1 dry/wet
    int   splice;                // SpliceMode — shared by both voices
} MicroPitchParams;

// Render MicroPitch over the whole sample, destructively. The result is stereo
// (a->ch becomes 2; mono input is upmixed by the effect). Returns 0 on success.
int h3000_micropitch(Audio *a, const MicroPitchParams *p);

// --- streaming core (for real-time preview) ---------------------------------
typedef struct MicroPitchState MicroPitchState;
MicroPitchState *mp_create(int rate);
void mp_destroy(MicroPitchState *st);
// n mono frames in `dry` -> n interleaved-stereo frames in `outLR`; params read
// live (call repeatedly with persistent state for a continuous stream).
void mp_block(MicroPitchState *st, const float *dry, int n,
              const MicroPitchParams *p, float *outLR);

#endif
