#pragma once

// OpenAL Soft in place of A3D (swe1r only). Every swrSound function that talks to the A3D device,
// listener or a source is replaced with one driving OpenAL instead, so a3dapi.dll is never loaded.
// OpenAL32.dll is loaded at runtime from the game directory; when it is missing nothing is
// replaced and the game keeps its A3D path.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Register the replacements. Must run before init_hooks().
void init_openal_audio_hooks(void);

// A movie soundtrack: interleaved 16-bit PCM played once. start returns 0 when OpenAL is
// unavailable; time is the playback position in seconds, -1 once it has ended.
int openal_movie_audio_start(const int16_t *pcm, int samples, int channels, int rate, float gain);
double openal_movie_audio_time(void);
void openal_movie_audio_stop(void);

#ifdef __cplusplus
}
#endif
