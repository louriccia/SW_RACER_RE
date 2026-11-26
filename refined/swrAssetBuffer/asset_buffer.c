#include "asset_buffer.h"

#include <Swr/swrAssetBuffer.h>
#include <Swr/swrObj.h>
#include <engine_config.h>
#include <globals.h>

#include <stdlib.h>

static size_t arena_size = SWR_ASSET_BUFFER_SIZE_RETAIL;

void refined_asset_buffer_set_size(size_t bytes)
{
    arena_size = bytes < SWR_ASSET_BUFFER_SIZE_RETAIL ? SWR_ASSET_BUFFER_SIZE_RETAIL : bytes;
}

size_t refined_asset_buffer_size(void)
{
    return arena_size;
}

// 0x00449040
void refined_swrScene_InitWorld(void)
{
    assetBuffer = (char*)malloc(arena_size);
    assetBufferEnd = assetBuffer + arena_size;
    swrAssetBuffer_initialBase = assetBuffer;
    // the saved allocation marks (slots 1..9 after assetBuffer) start empty
    for (int slot = 1; slot <= SWR_ASSET_BUFFER_MAX_INDEX; slot++)
        (&assetBuffer)[slot] = NULL;
    swrViewport_frameParity = 0;
    swrScene_LoadObjects();
    swrScene_InitFog();
    swrAssetBuffer_CheckOverflow();
}
