#include "refined.h"

#include "swrAssetBuffer/asset_buffer.h"
#include "swrModel/texture_cache.h"

#include <Swr/swrAssetBuffer.h>
#include <Swr/swrModel.h>
#include <Swr/swrObj.h>

const RefinedSlot refined_slots[] = {
    {"swrScene_InitWorld", (void*)swrScene_InitWorld, (void*)refined_swrScene_InitWorld},
    {"swrModel_LoadModelTexture", (void*)swrModel_LoadModelTexture, (void*)refined_swrModel_LoadModelTexture},
    {"swrAssetBuffer_InvalidateTexturesBelow", (void*)swrAssetBuffer_InvalidateTexturesBelow,
     (void*)refined_swrAssetBuffer_InvalidateTexturesBelow},
};

const int refined_slot_count = sizeof(refined_slots) / sizeof(refined_slots[0]);
