#include "refined.h"

#include "swrAssetBuffer/asset_buffer.h"
#include "swrModel/texture_cache.h"
#include "swrObjJdge/race_timing.h"
#include "swrRace/racer_options.h"

#include <Swr/swrAssetBuffer.h>
#include <Swr/swrModel.h>
#include <Swr/swrMultiplayer.h>
#include <Swr/swrObj.h>
#include <Swr/swrRace.h>

const RefinedSlot refined_slots[] = {
    {"swrScene_InitWorld", (void*)swrScene_InitWorld, (void*)refined_swrScene_InitWorld},
    {"swrModel_LoadModelTexture", (void*)swrModel_LoadModelTexture, (void*)refined_swrModel_LoadModelTexture},
    {"swrAssetBuffer_InvalidateTexturesBelow", (void*)swrAssetBuffer_InvalidateTexturesBelow,
     (void*)refined_swrAssetBuffer_InvalidateTexturesBelow},
    {"swrObjJdge_F2", (void*)swrObjJdge_F2, (void*)refined_swrObjJdge_F2},
    {"swrRace_InRaceTimer", (void*)swrRace_InRaceTimer, (void*)refined_swrRace_InRaceTimer},
    {"swrRace_InRaceEndStatistics", (void*)swrRace_InRaceEndStatistics, (void*)refined_swrRace_InRaceEndStatistics},
    {"swrObjHang_ResultsMenu", (void*)swrObjHang_ResultsMenu, (void*)refined_swrObjHang_ResultsMenu},
    {"swrMultiplayer_InRace", (void*)swrMultiplayer_InRace, (void*)refined_swrMultiplayer_InRace},
    {"swrObjJdge_SpawnRacers", (void*)swrObjJdge_SpawnRacers, (void*)refined_swrObjJdge_SpawnRacers},
    {"swrRace_PoddAnimateVariousThings", (void*)swrRace_PoddAnimateVariousThings,
     (void*)refined_swrRace_PoddAnimateVariousThings},
    {"swrRace_UpdatePlayerControl", (void*)swrRace_UpdatePlayerControl, (void*)refined_swrRace_UpdatePlayerControl},
    {"swrObjTest_F0", (void*)swrObjTest_F0, (void*)refined_swrObjTest_F0},
};

const int refined_slot_count = sizeof(refined_slots) / sizeof(refined_slots[0]);
