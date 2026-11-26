#pragma once

// SANM movie decoder (the .znm cutscenes: gzip of a SMUSH "SANM" file with Blocky16 video and VIMA
// audio). The codecs are ported from ScummVM's Grim engine (engines/grim/movie/codecs), GPLv3+.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SanmMovie SanmMovie;

// Opens a .znm (gzip) or plain SANM file. NULL on failure.
SanmMovie *sanm_open(const char *path);
void sanm_close(SanmMovie *movie);

int sanm_width(const SanmMovie *movie);
int sanm_height(const SanmMovie *movie);
int sanm_frame_count(const SanmMovie *movie);
int sanm_frame_us(const SanmMovie *movie);// microseconds per frame

// The whole soundtrack as interleaved signed 16-bit PCM (decoded at open). NULL when silent.
const int16_t *sanm_audio(const SanmMovie *movie, int *samples_per_channel, int *channels, int *rate);

// Decodes the next frame. Returns 1 when the frame carried a picture (the RGB565 image changed),
// 0 when it only carried audio (the image stays), -1 at the end.
int sanm_next_frame(SanmMovie *movie);

// The current picture: width * height RGB565 pixels.
const uint16_t *sanm_image(const SanmMovie *movie);

#ifdef __cplusplus
}
#endif
