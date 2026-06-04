// wav.h - WAV helpers for M8Tape Studio (split now; edit ops later).
#ifndef WAV_H
#define WAV_H

// Split an interleaved multichannel PCM WAV into stereo pairs named
// "<out_dir>/<prefix>_01.wav" .. "_NN.wav" (pair N = channels 2N-1,2N).
// Returns the number of stereo files written, or -1 on error.
int wav_split(const char *in_path, const char *out_dir, const char *prefix);

#endif
