#include "track_lifecycle.h"

#include <Swr/swrAssetBuffer.h>
#include <Swr/swrModel.h>
#include <engine_config.h>
#include <globals.h>
#include <swr.h>

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
static TrackListenerList unload_listeners;
static TrackListenerList loaded_listeners;

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

int track_on_unload(const char* owner, track_listener_fn fn, void* user)
{
    return listener_add(&unload_listeners, owner, fn, user);
}

int track_on_loaded(const char* owner, track_listener_fn fn, void* user)
{
    return listener_add(&loaded_listeners, owner, fn, user);
}

void track_lifecycle_notify_loaded(void)
{
    listeners_run(&loaded_listeners);
}

// 0x00445aa0
void refined_swrAssetBuffer_ResetToIndex(int index)
{
    char* checkpoint;

    listeners_run(&unload_listeners);
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
