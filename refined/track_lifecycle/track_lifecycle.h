#ifndef REFINED_TRACK_LIFECYCLE_H
#define REFINED_TRACK_LIFECYCLE_H

// The lifetime of a track's assets. Models, splines and textures live in the asset buffer;
// swrAssetBuffer_ResetToIndex rewinds it and the next load overwrites them in place, so any
// pointer into the old data is dead from that moment. Retail has no such event. Here the rewind
// is the unload event and the end of swrObjJdge_InitTrack is the loaded event; code that keeps
// pointers into a track subscribes to them instead of guessing whether its data is still there.

#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TRACK_LIFECYCLE_MAX_LISTENERS 32
#define TRACK_LIFECYCLE_MAX_CURSORS 16

typedef void (*track_listener_fn)(void* user);

// Bumps at every unload (every asset-buffer rewind, including the two that precede a race).
uint32_t track_generation(void);

// Listeners run in registration order. Registering the same (fn, user) twice is a no-op.
// Returns 0 when the table is full. `owner` names the subscriber for diagnostics.
//
// before_load: a race track is about to load (start of swrObjJdge_InitTrack, before its rewind).
//   swrObjHang.track_index names the track; the previous track's data is still resident.
// unload: the asset buffer is about to rewind (any rewind: race load, hangar room/pod changes,
//   cutscene scenes). Runs before the rewind, so the old data can still be read.
// loaded: the race track, HUD, spline, grid and racers are loaded (end of InitTrack).
int track_on_before_load(const char* owner, track_listener_fn fn, void* user);
int track_on_unload(const char* owner, track_listener_fn fn, void* user);
int track_on_loaded(const char* owner, track_listener_fn fn, void* user);

// Fire the before_load / loaded listeners; called by the refined InitTrack below.
void track_lifecycle_notify_before_load(void);
void track_lifecycle_notify_loaded(void);

// A registered spline cursor is zeroed (spline = NULL) at every unload, so it can never walk a
// spline the rewind has overwritten. The cursor must stay valid until unregistered. Registering
// twice is a no-op; returns 0 when the table is full.
int track_register_cursor(swrSplineCursor* cursor);
void track_unregister_cursor(swrSplineCursor* cursor);

// swrAssetBuffer_ResetToIndex, firing the unload listeners before the rewind. 0x00445aa0
void refined_swrAssetBuffer_ResetToIndex(int index);

// swrObjJdge_SetupTrackEnvironment with the fly-by state cleared on its early returns; registers
// the fly-by cursor. 0x00464b90
void refined_swrObjJdge_SetupTrackEnvironment(swrObjJdge* judge, int* anims, int model);

// swrObjJdge_InitTrack verbatim, firing before_load at the start and loaded at the end. 0x00466bd0
unsigned int refined_swrObjJdge_InitTrack(swrObjJdge* judge, swrScore* scores);

#ifdef __cplusplus
}
#endif

#endif
