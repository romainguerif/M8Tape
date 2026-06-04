// wav.h - WAV helpers for M8Tape Studio (split now; edit ops later).
#ifndef WAV_H
#define WAV_H

// Split an interleaved multichannel PCM WAV into stereo pairs named
// "<out_dir>/<prefix>_01.wav" .. "_NN.wav" (pair N = channels 2N-1,2N).
// Returns the number of stereo files written, or -1 on error.
int wav_split(const char *in_path, const char *out_dir, const char *prefix);

// --- in-memory audio for the editor -----------------------------------------
typedef struct {
    int   ch;
    int   rate;
    int   bits;       // source bit depth (write target unless changed)
    long  frames;
    float *data;      // interleaved, -1..1, ch*frames samples
} Audio;

int  wav_load(const char *path, Audio *a);             // 0 ok, -1 err
int  wav_save(const char *path, const Audio *a);       // writes at a->bits
void audio_free(Audio *a);

// editing ops (frame indices; [s,e) ranges). Some change frames/ch/rate.
void au_normalize(Audio *a);
void au_reverse(Audio *a, long s, long e);
void au_fade_in(Audio *a, long s, long e);
void au_fade_out(Audio *a, long s, long e);
int  au_crop(Audio *a, long s, long e);                // keep [s,e)
int  au_to_mono(Audio *a);
int  au_halve_rate(Audio *a);                          // averaging decimation
void au_silence_bounds(const Audio *a, float thresh, long *s, long *e);

// peaks for display: cols columns over [s,e), each gets min/max in [-1,1].
void au_peaks(const Audio *a, long s, long e, int cols, float *mn, float *mx);

// write [s,e) of a as a standalone WAV (for auditioning a selection).
int  wav_save_range(const char *path, const Audio *a, long s, long e);

// Streaming silence-trim file->file (any channel count / depth; no full load —
// used on the raw take, incl. M8 24ch). thresh is linear (0..1). Returns 0 ok.
int  wav_trim_silence_file(const char *in_path, const char *out_path, float thresh);

#endif
