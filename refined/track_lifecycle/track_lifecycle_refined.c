// swrObjJdge_InitTrack is sacred (its load order feeds the grid and the RNG draws): this is the
// src/ body unchanged, plus the loaded event after the last load.
#include "track_lifecycle.h"

#include <Swr/swrAssetBuffer.h>
#include <Swr/swrModel.h>
#include <Swr/swrObj.h>
#include <Swr/swrPlayerHUD.h>
#include <Swr/swrSound.h>
#include <Swr/swrSprite.h>
#include <Swr/swrSpline.h>
#include <Swr/swrWeather.h>
#include <engine_config.h>
#include <globals.h>
#include <swr.h>

// 0x00466bd0
unsigned int refined_swrObjJdge_InitTrack(swrObjJdge* judge, swrScore* scores)
{
    swrScore* score;
    int i;

    rdroid_g_curRenderOptions2 = 0;
    for (i = 0; i < SWR_MAX_RACERS; i++)
        swrObjJdge_prevRacePositions[i] = i;
    swrAssetBuffer_ResetToIndex(1);
    swrObjJdge_raceBufferStart = swrAssetBuffer_GetBuffer();
    swrObjJdge_splineBufferBytes = 0;
    lowMemoryRacerCount = 0;
    swr_noop2();
    swrModel_ClearSceneAnimations();
    swrWeather_ResetParticles();
    swrSprite_ResetLightStreakSprites();
    swrSprite_ResetSunAndLensFlareSprites();
    swrSprite_ResetPlayerSpriteValues();

    judge->camSweepState = NULL;
    swrScoresPtr = scores;
    firstLocalPlayer = NULL;
    secondLocalPlayer = NULL;
    thirdLocalPlayer = NULL;
    fourthLocalPlayer = NULL;
    numLocalPlayers = 0;
    for (i = 0; i < judge->num_players; i++) {
        score = &scores[i];
        if (score->identifier != 'Locl')
            continue;
        numLocalPlayers++;
        if (firstLocalPlayer == NULL)
            firstLocalPlayer = score;
        else if (secondLocalPlayer == NULL)
            secondLocalPlayer = score;
        else if (thirdLocalPlayer == NULL)
            thirdLocalPlayer = score;
        else if (fourthLocalPlayer == NULL)
            fourthLocalPlayer = score;
    }

    swrModel_Node1 = NULL;
    swrModel_Node3 = NULL;
    swrModel_Node2 = NULL;
    swrModel_Node4 = NULL;
    swrObjJdge_splitscreenActive = 0;
    swrObjJdge_splitscreenWideFov = 0;
    if (numLocalPlayers > 1) {
        swrObjJdge_singleLocalViewport = 0;
        swrObjJdge_splitscreenActive = 1;
        if (judge->planetId == 6)
            swrObjJdge_splitscreenWideFov = 1;
    }
    if (judge->planetId == 3 && judge->planet_track_number == 2)
        swrObjTrig_regionGridLayout = 1;
    else if (judge->planetId == 1 && judge->planet_track_number == 0)
        swrObjTrig_regionGridLayout = 2;
    else
        swrObjTrig_regionGridLayout = 0;

    swrObjJdge_LoadTrackModels(judge);
    swrPlayerHUD_LoadRaceHUD(judge->planetId, judge->planet_track_number, (int)judge);
    swrObjJdge_LoadTrackSpline(judge);
    swrObjJdge_SetupStartingGrid(judge);
    swrObjJdge_InitPrimaryLight();
    swrObjJdge_InitAISettingsForTrack(judge);
    swrObjJdge_SpawnRacers(judge, scores);
    swrSound_ResetMusic();
    for (i = 0; i < SWR_MAX_RACERS; i++)
        swrSound_ClearSfxFlag(i, 0xff0000);

    swrObjJdge_raceBufferEnd = swrAssetBuffer_GetBuffer();
    swrObjJdge_raceBufferFree = swrAssetBuffer_RemainingSize();
    swrObjJdge_raceBufferOtherBytes = swrObjJdge_raceBufferEnd - swrObjJdge_raceBufferStart;
    swrObjJdge_raceBufferSlack = SWR_RACE_BUFFER_TALLY_SIZE - swrObjJdge_raceBufferOtherBytes - swrObjJdge_raceBufferFree;
    for (i = 0; i < SWR_MAX_RACERS; i++)
        swrObjJdge_raceBufferOtherBytes -= swrObjJdge_racerBufferBytes[i];
    swrObjJdge_raceBufferOtherBytes -= swrObjJdge_testObjBufferBytes + swrObjJdge_splineBufferBytes + swrObjJdge_trackModelBufferBytes;
    swrAssetBuffer_CheckOverflow();
    track_lifecycle_notify_loaded();
    // faithful: the original returns CheckOverflow's leftover EAX; the only caller ignores it
    return 0;
}
