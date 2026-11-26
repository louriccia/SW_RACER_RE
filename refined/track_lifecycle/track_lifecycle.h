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

// Listeners run in registration order. Unload listeners run before the rewind, so they may
// still read the old data. Registering the same (fn, user) twice is a no-op. Returns 0 when the
// table is full. `owner` names the subscriber for diagnostics.
int track_on_unload(const char* owner, track_listener_fn fn, void* user);
int track_on_loaded(const char* owner, track_listener_fn fn, void* user);

// Fires the loaded listeners; called once by the refined InitTrack below.
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

// swrObjJdge_InitTrack verbatim, firing the loaded listeners at the end. 0x00466bd0
unsigned int refined_swrObjJdge_InitTrack(swrObjJdge* judge, swrScore* scores);

#ifdef __cplusplus
}
#endif

#endif
