#include "track_lifecycle.h"

#include <Swr/swrAssetBuffer.h>
#include <Swr/swrModel.h>
#include <engine_config.h>
#include <globals.h>
#include <swr.h>

#include <string.h>

typedef struct TrackListener {
    const char* owner;
    track_listener_fn fn;
    void* user;
} TrackListener;

typedef struct TrackListenerList {
    TrackListener entries[TRACK_LIFECYCLE_MAX_LISTENERS];
    int count;
} TrackListenerList;

static uint32_t generation;
static TrackListenerList before_load_listeners;
static TrackListenerList unload_listeners;
static TrackListenerList loaded_listeners;
static swrSplineCursor* cursors[TRACK_LIFECYCLE_MAX_CURSORS];
static int cursor_count;

static int listener_add(TrackListenerList* list, const char* owner, track_listener_fn fn, void* user)
{
    for (int i = 0; i < list->count; i++) {
        if (list->entries[i].fn == fn && list->entries[i].user == user)
            return 1;
    }
    if (fn == NULL || list->count >= TRACK_LIFECYCLE_MAX_LISTENERS)
        return 0;
    list->entries[list->count].owner = owner;
    list->entries[list->count].fn = fn;
    list->entries[list->count].user = user;
    list->count++;
    return 1;
}

static void listeners_run(const TrackListenerList* list)
{
    for (int i = 0; i < list->count; i++)
        list->entries[i].fn(list->entries[i].user);
}

uint32_t track_generation(void)
{
    return generation;
}

int track_on_before_load(const char* owner, track_listener_fn fn, void* user)
{
    return listener_add(&before_load_listeners, owner, fn, user);
}

int track_on_unload(const char* owner, track_listener_fn fn, void* user)
{
    return listener_add(&unload_listeners, owner, fn, user);
}

int track_register_cursor(swrSplineCursor* cursor)
{
    for (int i = 0; i < cursor_count; i++) {
        if (cursors[i] == cursor)
            return 1;
    }
    if (cursor == NULL || cursor_count >= TRACK_LIFECYCLE_MAX_CURSORS)
        return 0;
    cursors[cursor_count++] = cursor;
    return 1;
}

void track_unregister_cursor(swrSplineCursor* cursor)
{
    for (int i = 0; i < cursor_count; i++) {
        if (cursors[i] == cursor) {
            cursors[i] = cursors[--cursor_count];
            return;
        }
    }
}

static void cursors_reset(void)
{
    for (int i = 0; i < cursor_count; i++)
        memset(cursors[i], 0, sizeof(*cursors[i]));
}

int track_on_loaded(const char* owner, track_listener_fn fn, void* user)
{
    return listener_add(&loaded_listeners, owner, fn, user);
}

void track_lifecycle_notify_before_load(void)
{
    listeners_run(&before_load_listeners);
}

void track_lifecycle_notify_loaded(void)
{
    listeners_run(&loaded_listeners);
}

// Trigger props/FX by slot. A planet's trigger setup fills only the slots it uses, and only
// swrObjTrig_LoadPropModel writes ModelArray1, yet the scene nodes of every balloon, flame and flag
// trigger take their child from ModelArray1[slot]. A slot the new planet does not fill keeps the
// previous track's node, which the rewind is about to overwrite (U2).
static void trigger_slots_reset(void)
{
    memset(swrObjTrig_ModelArray1, 0, sizeof(swrObjTrig_ModelArray1));
}

// 0x00445aa0
void refined_swrAssetBuffer_ResetToIndex(int index)
{
    char* checkpoint;

    listeners_run(&unload_listeners);
    trigger_slots_reset();
    cursors_reset();
    generation++;

    assetBufferIndex = index;
    checkpoint = (&assetBuffer)[index - 1];
    (&assetBuffer)[index] = checkpoint;
    swrAssetBuffer_InvalidateTexturesBelow(checkpoint);
    swr_noop2();
    for (int i = SWR_ASSET_BUFFER_MAX_INDEX; i > assetBufferIndex; i--)
        (&assetBuffer)[i] = NULL;
    assetBufferOverflow = 0;
    for (int slot = SWR_ASSET_BUFFER_MAX_INDEX; slot >= assetBufferIndex; slot--)
        swrModel_FreeSlotMaterials(slot);
}
