// dsputil.h - small shared DSP building blocks (float, no deps but math).
// Used by the time-domain H3000 algorithms: delays, reverbs, band delay, tone.
#ifndef DSPUTIL_H
#define DSPUTIL_H

// --- fractional delay line ---------------------------------------------------
typedef struct { float *buf; int n, wr; } DelayLine;
int   dl_init(DelayLine *d, int rate, float max_ms);   // 1 ok, 0 alloc fail
void  dl_free(DelayLine *d);
void  dl_clear(DelayLine *d);
void  dl_write(DelayLine *d, float v);
float dl_read_ms(const DelayLine *d, float ms, int rate);  // linear-interp tap
float dl_read_samp(const DelayLine *d, float samples);     // tap by sample delay

// --- one-pole filters --------------------------------------------------------
typedef struct { float a, z; } OnePole;     // a = smoothing coeff, z = state
void  op_set_lp(OnePole *f, float hz, int rate);
float op_lp(OnePole *f, float in);          // low-pass
float op_hp(OnePole *f, float in);          // high-pass = in - low-pass

// --- biquad (RBJ cookbook) ---------------------------------------------------
typedef struct { float b0, b1, b2, a1, a2, x1, x2, y1, y2; } Biquad;
void  bq_lowpass (Biquad *f, float hz, float q, int rate);
void  bq_highpass(Biquad *f, float hz, float q, int rate);
void  bq_bandpass(Biquad *f, float hz, float q, int rate);  // 0 dB peak gain
void  bq_reset(Biquad *f);
float bq_process(Biquad *f, float in);

// --- Schroeder allpass (reverb diffusion) ------------------------------------
typedef struct { float *buf; int n, wr; float g; } Allpass;
int   ap_init(Allpass *a, int len_samples, float g);   // 1 ok, 0 alloc fail
void  ap_free(Allpass *a);
float ap_process(Allpass *a, float in);

// --- deterministic LCG (for fixed, reproducible randomization) ---------------
unsigned dsp_lcg(unsigned *state);          // returns 0..2^31-1-ish
float    dsp_frand(unsigned *state);        // 0..1

#endif
