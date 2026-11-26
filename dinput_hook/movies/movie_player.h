#pragma once

// Our SANM player in place of Smush.dll (swe1r only): fills the four Smush import slots the game
// calls through. SWR_MOVIES_SMUSH=1 keeps Smush.dll.

#ifdef __cplusplus
extern "C" {
#endif

void init_movie_player(void);

#ifdef __cplusplus
}
#endif
