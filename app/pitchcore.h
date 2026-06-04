// pitchcore.h - H3000 time-domain pitch voice (2-tap moving delay + crossfade)
// with the H949 autocorrelation de-glitch and the four splice characters.
// Shared by every pitch-family algorithm (MicroPitch, Dual/Diatonic/Stereo
// Shift, Crystal...). One voice = one mono pitch shifter.
#ifndef PITCHCORE_H
#define PITCHCORE_H

// Splice character (see h3000 module docs / ValhallaDSP / patent US6049766):
//   H910   = no de-glitch, fixed window + LC-clock drift (1975 character)
//   H949-1 = autocorrelation de-glitch (1977), classic
//   H949-2 = de-glitch, longer correlation + crossfade (smoother)
//   MODERN = finest de-glitch search + longest crossfade (cleanest)
typedef enum {
    SPLICE_H910 = 0,
    SPLICE_H949_1,
    SPLICE_H949_2,
    SPLICE_MODERN,
    SPLICE_COUNT
} SpliceMode;

// UI choice labels for a SPLICE parameter (NULL-terminated, length SPLICE_COUNT+1)
extern const char *const PITCH_SPLICE_CHOICES[];

typedef struct PitchVoice PitchVoice;

PitchVoice *pv_create(int rate);
void  pv_destroy(PitchVoice *v);
void  pv_set_cents(PitchVoice *v, float cents);   // detune in cents (sets ratio)
void  pv_set_splice(PitchVoice *v, int mode);     // SpliceMode
float pv_process(PitchVoice *v, float in);        // one mono sample in -> out

#endif
