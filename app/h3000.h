// h3000.h - Eventide H3000-style DSP engine for M8Tape Studio.
// Faithful to the hardware's mono-program model: one pass = one algorithm,
// rendered destructively on the sample. First algorithm: MicroPitch.
#ifndef H3000_H
#define H3000_H

#include "wav.h"   // Audio

// --- MicroPitch: two finely-detuned pitch voices, delayed, panned L/R --------
typedef struct {
    float cents_a, delay_a_ms;   // voice A → left
    float cents_b, delay_b_ms;   // voice B → right
    float feedback;              // 0..0.95 (per voice)
    float mix;                   // 0..1 dry/wet
} MicroPitchParams;

// Render MicroPitch over the whole sample, destructively. The result is stereo
// (a->ch becomes 2; mono input is upmixed by the effect). Returns 0 on success.
int h3000_micropitch(Audio *a, const MicroPitchParams *p);

#endif
