// player.h - blocking WAV playback via ALSA with a click-free fade-out on stop.
#ifndef PLAYER_H
#define PLAYER_H

// Plays the WAV at `path` to the default ALSA device. Blocks until the file
// ends or SIGTERM is received; on SIGTERM it ramps the output to silence over
// ~30 ms (avoiding the click of a hard cut) before returning. Meant to be run
// in a forked child. Returns 0 on success, non-zero on error.
int play_wav_fade(const char *path);

#endif
