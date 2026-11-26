// The faithful functions behind the full-detail AI and flamejet options. Each is its src/
// counterpart with one condition widened by a setting in racer_options (all off = retail).
#include "racer_options.h"

#include <Engine/sithRender.h>
#include <General/stdMath.h>
#include <General/utils.h>
#include <Main/swrControl.h>
#include <Main/swrMain.h>
#include <Platform/stdControl.h>
#include <Primitives/rdMatrix.h>
#include <Primitives/rdVector.h>
#include <Swr/swrAssetBuffer.h>
#include <Swr/swrCam.h>
#include <Swr/swrConfig.h>
#include <Swr/swrEvent.h>
#include <Swr/swrModel.h>
#include <Swr/swrMultiplayer.h>
#include <Swr/swrObj.h>
#include <Swr/swrPlayerHUD.h>
#include <Swr/swrRace.h>
#include <Swr/swrRender.h>
#include <Swr/swrSound.h>
#include <Swr/swrSpline.h>
#include <Swr/swrSprite.h>
#include <Swr/swrText.h>
#include <Swr/swrUI.h>
#include <Swr/swrViewport.h>
#include <Swr/swrWeather.h>
#include <Unknown/rdMatrixStack.h>
#include <Win95/Window.h>
#include <engine_config.h>
#include <globals.h>
#include <macros.h>
#include <swr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 0x004663e0
void refined_swrObjJdge_SpawnRacers(swrObjJdge* judge, swrScore* scores)
{
    int lodNode[SWR_MAX_RACERS];
    int spawnOrder[SWR_MAX_RACERS];
    int podHeader[SWR_MAX_RACERS];
    int lodNode2[SWR_MAX_RACERS];
    int riderHeader[SWR_MAX_RACERS]; // faithful: only written on the full-LOD path, garbage otherwise
    int aiSlot[SWR_MAX_RACERS];
    swrRacerData* racer;
    swrRacerData* other;
    swrModel_Header* header;
    swrModel_Header* rider;
    char* name;
    int duplicatePods;
    int lodClass;
    int i;
    int j;
    int pick;
    int slot;

    swrObjJdge_testObjBufferBytes = (int)swrAssetBuffer_GetBuffer();
    swrEvent_AllocateAndLoadObjs('Test', judge->num_players);
    swrEvent_FreeObjs('Test');
    swrObjJdge_testObjBufferBytes = (int)swrAssetBuffer_GetBuffer() - swrObjJdge_testObjBufferBytes;
    memset(swrObjJdge_racerBufferBytes, 0, sizeof(swrObjJdge_racerBufferBytes));
    for (i = 0; i < SWR_MAX_RACERS; i++)
        swrObjJdge_racerNameTags[i * 4] = 0;

    duplicatePods = 0;
    for (i = 0; i < judge->num_players; i++) {
        for (j = 0; j < judge->num_players; j++) {
            if (i != j && *scores[i].pilotId == *scores[j].pilotId)
                duplicatePods = 1;
        }
    }
    swrObjJdge_duplicatePods = duplicatePods;

    for (i = 0; i < judge->num_players; i++) {
        racer = (swrRacerData*)scores[i].pilotId;
        swrObjJdge_racerBufferBytes[i] = (int)swrAssetBuffer_GetBuffer();
        swrObjJdge_racerNameTags[i * 4] = swrText_Translate(racer->name)[2];
        swrObjJdge_racerNameTags[i * 4 + 1] = swrText_Translate(racer->name)[3];
        swrObjJdge_racerNameTags[i * 4 + 2] = swrText_Translate(racer->name)[4];
        swrObjJdge_racerNameTags[i * 4 + 3] = 0;

        // 1 = full pod + rider, 2 = full pod (split screen), 3 = low-detail pod
        lodClass = 3;
        if (scores[i].identifier == 'Locl' || racer_options.full_lod_ai)
            lodClass = (numLocalPlayers > 1) + 1;
        if ((judge->flag & swrObjJdge_FLAG_NON_PAUSABLE) != 0)
            lodClass = 1;
        else if (numLocalPlayers == 0)
            lodClass = 3;

        if (lodClass == 2 || lodClass == 1) {
            lodNode[i] = 0;
            lodNode2[i] = 0;
            header = swrModel_LoadFromId(racer->pod_modelID);
            podHeader[i] = (int)header;
            if (header == NULL) {
                judge->num_players = i;
                lowMemoryRacerCount = i;
            } else if (lodClass == 1 && (int)racer->pod_alt_modelID >= 0) {
                rider = swrModel_LoadFromId(racer->pod_alt_modelID);
                riderHeader[i] = (int)rider;
                if (rider == NULL) {
                    judge->num_players = i;
                    lowMemoryRacerCount = i;
                } else {
                    swrModel_FixupAltNodePointers((int*)header, (int*)rider);
                }
            }
        } else if (lodClass == 3) {
            podHeader[i] = 0;
            lodNode2[i] = 0;
            lodNode[i] = 0;
            header = swrModel_LoadFromId(racer->pod_lod_modelID);
            if (header != NULL) {
                lodNode[i] = header->entries[0].value;
                lodNode2[i] = header->entries[1].value;
            }
        } else if (lodClass == 4) {
            podHeader[i] = 0;
            lodNode2[i] = 0;
            lodNode[i] = 0;
            header = swrModel_LoadFromId(racer->unk10_modelID);
            if (header != NULL) {
                lodNode2[i] = 0;
                lodNode[i] = header->entries[0].value;
            }
        }
        swrObjJdge_racerBufferBytes[i] = (int)swrAssetBuffer_GetBuffer() - swrObjJdge_racerBufferBytes[i];
    }

    for (i = 0; i < judge->num_players; i++)
        spawnOrder[i] = -1;
    for (i = 0; i < judge->num_players; i++) {
        // pick the n-th still-free grid slot, n uniform in [0, remaining)
        pick = (int)((long double)swrUtils_Rand() * (1.0f / 2147483648.0f) * (judge->num_players - i));
        slot = -1;
        while (pick >= 0) {
            slot++;
            if (spawnOrder[slot] == -1)
                pick--;
        }
        spawnOrder[slot] = i;
    }

    slot = 1;
    for (i = 0; i < judge->num_players; i++) {
        if ((scores[i].flag & 0x20) == 0 && scores[i].identifier != 'Locl')
            aiSlot[i] = slot++;
        else
            aiSlot[i] = -1;
    }

    for (i = 0; i < judge->num_players; i++) {
        j = spawnOrder[i];
        swrObjJdge_SpawnRacer(judge, &scores[j], aiSlot[j], (void*)podHeader[j], riderHeader[j], lodNode[j], lodNode2[j], duplicatePods);
    }
}

// 0x00471760
void refined_swrRace_PoddAnimateVariousThings(swrRace* player)
{
    int pilot;
    int fusedEngines = 0;
    int hasRearEngines = 0;
    const swrRacerVisualData* visual;
    rdMatrix44 podXf;
    rdMatrix44 m;
    rdVector3 binderA;
    rdVector3 binderB;
    rdVector3 offset;
    rdVector3 deformA;
    rdVector3 deformB;
    swrModel_MeshMaterial* glowMatR;
    swrModel_MeshMaterial* glowMatL;
    float speed;
    float steer;
    float glowR;
    float glowL;
    float flare;
    float heatL;
    float heatR;
    float burnL;
    float burnR;
    float throttle;
    float scale;
    float depth;
    float tintR;
    float tintL;
    float amplitude;
    float turn;
    int tossFlag;
    int i;

    if (player->partNodes == NULL)
        return;

    if (player->partNodes[0x3b] != NULL)
        swrModel_NodeModifyFlags(player->partNodes[0x3b], 2, 3, 0x10, 2);
    rdMatrix_Copy44(&podXf, &player->transform);

    pilot = *player->score_ptr->pilotId;
    visual = &swrRacer_PodVisualData[pilot];
    if (pilot == SWR_PILOT_NEVA_KEE)
        fusedEngines = 1;
    if (player->partNodes[3] != NULL && player->partNodes[4] != NULL)
        hasRearEngines = 1;

    if (!fusedEngines) {
        rdVector_Copy3(&binderA, (rdVector3*)&player->engineXfR.vD);
        rdVector_Copy3(&offset, &visual->sweepEngineOffset);
        offset.x = -offset.x;
        rdMatrix_Multiply3(&offset, &offset, &player->engineXfR);
        rdVector_Add3(&binderA, &offset, &binderA);
        if (hasRearEngines) {
            rdVector_Copy3(&binderB, (rdVector3*)&player->engineXfL2.vD);
            rdVector_Copy3(&offset, &visual->sweepEngineOffset);
            rdMatrix_Multiply3(&offset, &offset, &player->engineXfL2);
            rdVector_Add3(&binderB, &offset, &binderB);
            binderA.z += player->unk11_1;
            binderB.z += player->unk11_1;
            swrRace_UpdateEnergyBinder(player, 2, &binderA, &binderB);

            rdVector_Copy3(&binderA, (rdVector3*)&player->engineXfR2.vD);
            rdVector_Copy3(&offset, &visual->sweepEngineOffset);
            offset.x = -offset.x;
            rdMatrix_Multiply3(&offset, &offset, &player->engineXfR2);
            rdVector_Add3(&binderA, &offset, &binderA);
        }
        rdVector_Copy3(&binderB, (rdVector3*)&player->engineXfL.vD);
        rdVector_Copy3(&offset, &visual->sweepEngineOffset);
        rdMatrix_Multiply3(&offset, &offset, &player->engineXfL);
        rdVector_Add3(&binderB, &offset, &binderB);
        binderA.z += player->unk11_1;
        binderB.z += player->unk11_1;
        swrRace_UpdateEnergyBinder(player, 1, &binderA, &binderB);
    }

    speed = player->speedValue * 0.001f;
    if (speed > 1.0f)
        speed = 1.0f;
    if (speed < 0.0f)
        speed = 0.0f;
    steer = player->turnRate * 0.016666668f;
    if (steer > 1.0f)
        steer = 1.0f;
    if (steer < -1.0f)
        steer = -1.0f;

    glowMatR = swrModel_NodeFindFirstMeshMaterial(player->partNodes[0x45]);
    glowMatL = swrModel_NodeFindFirstMeshMaterial(player->partNodes[0x46]);

    // exhaust plumes: engine matrix with Y/Z basis swapped, offset by the pilot's exhaust anchor
    rdMatrix_Copy44(&m, &player->engineXfR);
    rdVector_Copy3(&offset, &visual->exhaustOffset);
    rdMatrix_Multiply3(&offset, &offset, &m);
    m.vD.x = offset.x + m.vD.x;
    m.vD.y = offset.y + m.vD.y;
    m.vD.z = offset.z + m.vD.z;
    m.vB.x = player->engineXfR.vC.x;
    m.vB.y = player->engineXfR.vC.y;
    m.vB.z = player->engineXfR.vC.z;
    m.vC.x = player->engineXfR.vB.x;
    m.vC.y = player->engineXfR.vB.y;
    m.vC.z = player->engineXfR.vB.z;
    rdMatrix_Copy44(&player->engineExhaustXfR, &m);
    if (player->partNodes[0x43] != NULL)
        swrModel_NodeModifyFlags(player->partNodes[0x43], 2, 3, 0x10, 2);
    if (player->partNodes[0x43] != NULL)
        swrModel_NodeSetTransform((swrModel_NodeTransformed*)player->partNodes[0x43], &player->engineExhaustXfR);

    rdMatrix_Copy44(&m, &player->engineXfL);
    rdVector_Copy3(&offset, &visual->exhaustOffset);
    offset.x = offset.x * -1.0f;
    rdMatrix_Multiply3(&offset, &offset, &m);
    m.vD.x = offset.x + m.vD.x;
    m.vD.y = offset.y + m.vD.y;
    m.vD.z = offset.z + m.vD.z;
    m.vB.x = player->engineXfL.vC.x;
    m.vB.y = player->engineXfL.vC.y;
    m.vB.z = player->engineXfL.vC.z;
    m.vC.x = player->engineXfL.vB.x;
    m.vC.y = player->engineXfL.vB.y;
    m.vC.z = player->engineXfL.vB.z;
    rdMatrix_Copy44(&player->engineExhaustXfL, &m);
    if (player->partNodes[0x44] != NULL)
        swrModel_NodeModifyFlags(player->partNodes[0x44], 2, 3, 0x10, 2);
    if (player->partNodes[0x44] != NULL)
        swrModel_NodeSetTransform((swrModel_NodeTransformed*)player->partNodes[0x44], &player->engineExhaustXfL);

    scale = visual->exhaustGlowScale * 0.01f;
    depth = -((speed * 3.0f - -9.0f) * 0.01f);
    rdMatrix_SetDiagonal44(&player->engineGlowXfR, scale, scale, depth);
    rdMatrix_SetDiagonal44(&player->engineGlowXfL, scale, scale, depth);
    if (player->partNodes[0x45] != NULL)
        swrModel_NodeSetTransform((swrModel_NodeTransformed*)player->partNodes[0x45], &player->engineGlowXfR);
    if (player->partNodes[0x46] != NULL)
        swrModel_NodeSetTransform((swrModel_NodeTransformed*)player->partNodes[0x46], &player->engineGlowXfL);

    // glow alpha: speed-driven, dimmed on the side the pod steers toward
    flare = speed * 255.0f;
    if (flare > 50.0f)
        flare = 50.0f;
    if (flare < 1.0f)
        flare = 1.0f;
    turn = -steer;
    if (turn < 0.0)
        turn = 0.0f;
    glowR = (1.0f - turn * 0.8f) * flare;
    if (glowR < 1.0)
        glowR = 1.0f;
    turn = steer;
    if (turn < 0.0)
        turn = 0.0f;
    glowL = (1.0f - turn * 0.8f) * flare;
    if (glowL < 1.0)
        glowL = 1.0f;

    // worst engine heat per side (engineTemp, part health, burning engines)
    heatL = 1.0f - player->engineTemp * 0.01f;
    heatR = heatL;
    for (i = 0; i < 3; i++) {
        if (player->engineHealth[i] > heatL)
            heatL = player->engineHealth[i];
        if (heatR < player->engineHealth[i + 3])
            heatR = player->engineHealth[i + 3];
        if (player->engineStatus[i] & swrRace_ENGINESTATUS_ON_FIRE)
            heatL = 1.0f;
        if (player->engineStatus[i + 3] & swrRace_ENGINESTATUS_ON_FIRE)
            heatR = 1.0f;
    }
    heatL = (heatL - 0.5f) + (heatL - 0.5f);
    if (heatL < 0.0f)
        heatL = 0.0f;
    else if (heatL > 1.0f)
        heatL = 1.0f;
    heatR = (heatR - 0.5f) + (heatR - 0.5f);
    if (heatR < 0.0f)
        heatR = 0.0f;
    else if (heatR > 1.0f)
        heatR = 1.0f;

    burnR = heatR * 4.0f;
    if (burnR > 1.0f)
        burnR = 1.0f;
    burnL = heatL * 4.0f;
    if (burnL > 1.0f)
        burnL = 1.0f;
    throttle = player->speedValue * 0.02f;
    glowR = (240.0f - glowR) * heatR + glowR;
    glowL = (240.0f - glowL) * heatL + glowL;
    tintR = burnR * -255.0f - -255.0f;
    tintL = burnL * -255.0f - -255.0f;
    if (throttle < 0.0f)
        throttle = 0.0f;
    else if (throttle > 1.0f)
        throttle = 1.0f;
    glowR = glowR * throttle;
    glowL = glowL * throttle;
    if (player->flags1 & swrObjTest_FLAG1_EXPLODING) {
        tintR = 0.0f;
        tintL = 0.0f;
    }
    if (tintR < 0.0f)
        tintR = 0.0f;
    else if (tintR > 255.0f)
        tintR = 255.0f;
    if (glowR < 1.0f)
        glowR = 1.0f;
    else if (glowR > 254.0f)
        glowR = 254.0f;
    if (tintL < 0.0f)
        tintL = 0.0f;
    else if (tintL > 255.0f)
        tintL = 255.0f;
    if (glowL < 1.0f)
        glowL = 1.0f;
    else if (glowL > 254.0f)
        glowL = 254.0f;

    if (glowMatR != NULL) {
        swrModel_MeshMaterialSetColors(glowMatR, 0, 0, (int16_t)tintR, (int16_t)tintR, (int16_t)tintR, (int16_t)glowR);
        swrModel_MeshMaterialSetTextureUVOffset(glowMatR, 0.0f, (float)((speed * 3.5f - -0.5f) * swrRace_deltaTimeSecs));
    }
    if (glowMatL != NULL) {
        swrModel_MeshMaterialSetColors(glowMatL, 0, 0, (int16_t)tintL, (int16_t)tintL, (int16_t)tintL, (int16_t)glowL);
        swrModel_MeshMaterialSetTextureUVOffset(glowMatL, 0.0f, (float)((speed * 3.5f - -0.5f) * swrRace_deltaTimeSecs));
    }

    if (swrConfig_VIDEO_ENGINEEXHAUST == 0) {
        if (player->partNodes[0x43] != NULL)
            swrModel_NodeModifyFlags(player->partNodes[0x43], 2, -4, 0x10, 3);
        if (player->partNodes[0x44] != NULL)
            swrModel_NodeModifyFlags(player->partNodes[0x44], 2, -4, 0x10, 3);
    }
    if (swrConfig_VIDEO_ENGINEEXHAUST == 1) {
        tossFlag = (player->flags0 & swrObjTest_FLAG0_AI) != 0;
        swrRace_SpawnGroundDustKick(player, (float*)&player->shadowXf2, 0.1f, -0.1f, -0.1f, 10.0f, tossFlag);
        swrRace_SpawnGroundDustKick(player, (float*)&player->shadowXf1, 0.1f, -0.1f, 0.1f, 10.0f, tossFlag);
    }

    // engine -> cockpit cables (partNodes / partXf [10..13]); retail re-tests partNodes != NULL here and below, always true after the early return
    if (fusedEngines || (!racer_options.full_lod_ai && player->lodDistance > 75)) {
        if (player->partNodes[10] != NULL)
            swrModel_NodeModifyFlags(player->partNodes[10], 2, -4, 0x10, 3);
        if (player->partNodes[11] != NULL)
            swrModel_NodeModifyFlags(player->partNodes[11], 2, -4, 0x10, 3);
        return;
    }
    if (player->partNodes[10] != NULL) {
        rdMatrix44* cable = &(&player->unk350_mat)[10];
        swrRace_BuildStretchedQuad(&player->cockpitXf, &player->engineXfR, 1.0f, 1.0f, pilot, cable);
        cable->vD.z = player->unk11_1 + cable->vD.z;
        swrModel_NodeSetTransform((swrModel_NodeTransformed*)player->partNodes[10], cable);
    }
    if (player->partNodes[11] != NULL) {
        rdMatrix44* cable = &(&player->unk350_mat)[11];
        swrRace_BuildStretchedQuad(&player->cockpitXf, &player->engineXfL, -1.0f, 1.0f, pilot, cable);
        cable->vD.z = player->unk11_1 + cable->vD.z;
        swrModel_NodeSetTransform((swrModel_NodeTransformed*)player->partNodes[11], cable);
    }
    if (player->partNodes[12] != NULL) {
        rdMatrix44* cable = &(&player->unk350_mat)[12];
        swrRace_BuildStretchedQuad(&player->cockpitXf, &player->engineXfR2, 1.0f, -1.0f, pilot, cable);
        cable->vD.z = player->unk11_1 + cable->vD.z;
        swrModel_NodeSetTransform((swrModel_NodeTransformed*)player->partNodes[12], cable);
    }
    if (player->partNodes[13] != NULL) {
        rdMatrix44* cable = &(&player->unk350_mat)[13];
        swrRace_BuildStretchedQuad(&player->cockpitXf, &player->engineXfL2, -1.0f, -1.0f, pilot, cable);
        cable->vD.z = player->unk11_1 + cable->vD.z;
        swrModel_NodeSetTransform((swrModel_NodeTransformed*)player->partNodes[13], cable);
    }

    if (!racer_options.full_lod_ai && player->lodDistance > 50)
        return;
    player->flags1 |= swrObjTest_FLAG1_CABLES_DEFORMED;
    // only the front pair of cables sways
    rdVector_Set3(&deformA, 0.0f, 0.0f, 0.0f);
    rdVector_Set3(&deformB, 0.0f, 0.0f, 0.0f);
    turn = player->turnRate * -0.03f;
    amplitude = 0.3f;
    if (turn > 1.0)
        amplitude = turn * 0.3f;
    if (amplitude > 1.0)
        amplitude = 1.0f;
    swrModel_DeformPoddConnectionMesh(player->partNodes[10], &deformA, &deformB, amplitude, 1.0f, (float)player->lodDistance, 50.0f, 0);
    rdVector_Set3(&deformA, 0.0f, 0.0f, 0.0f);
    rdVector_Set3(&deformB, 0.0f, 0.0f, 0.0f);
    turn = player->turnRate * 0.03f;
    amplitude = 0.3f;
    if (turn > 1.0)
        amplitude = turn * 0.3f;
    if (amplitude > 1.0)
        amplitude = 1.0f;
    swrModel_DeformPoddConnectionMesh(player->partNodes[11], &deformA, &deformB, amplitude, 1.0f, (float)player->lodDistance, 50.0f, 0);
}

// 0x0046bec0
void refined_swrRace_UpdatePlayerControl(swrRace* player)
{
    float steerInput = 0.0f;
    float pitchForward = 0.0f;
    float throttleRaw = 0.0f;
    int thrustDigital = 0;
    int brakeDigital = 0;
    int bankLeft = 0;
    int bankRight = 0;
    int viewButtonInput = 0;
    int lookBackInput = 0;
    int flameTaunt = 0;
    int slideInput = 0;
    bool repairHold = false;

    player->throttle = 0.0f;
    player->turnRateTarget = 0.0f;

    swrScore* score = player->score_ptr;
    int playerIndex = *(int8_t*) &score->sfxChannel; // score+0x10 low byte = local-player slot
    int controlType = *((int8_t*) score->localPlayerProfile + 0x23);
    score->flag &= ~0x4;
    player->score_ptr->flag &= ~0x8;

    uint32_t canChargeBoost = player->flags0 & swrObjTest_FLAG0_CAN_CHARGE_BOOST;
    int bitset3 = inRaceLocalPlayerInputBitset3[playerIndex];
    int mirror = GameSettingFlags & 0x4000; // invert-steering option

    // ---- input-gather dispatch on the profile control-type byte ----
    // controlType 0/9 -> analog axes; 1..7 -> per-player bitset; 8/>9 -> no input (all stay zero).
    if (controlType == 0 || controlType == 9) {
        if (mirror) {
            steerInput = -swrRace_SteeringInput;
            bankRight = (int) swrControl_rollLeftButton;
            bankLeft = (int) swrControl_rollRightButton;
        } else {
            steerInput = swrRace_SteeringInput;
            bankLeft = (int) swrControl_rollLeftButton;
            bankRight = (int) swrControl_rollRightButton;
        }
        thrustDigital = (int) swrRace_ThrustInput;
        brakeDigital = (int) swrControl_brakeButton;
        viewButtonInput = (int) swrControl_viewButton;
        lookBackInput = (int) swrControl_inputStateButton;
        flameTaunt = (int) swrControl_tauntButton;
        repairHold = swrControl_repairButton != 0.0f && swrControl_repairHoldTime > (float) SWR_CTL_HALF_HOLD;
        slideInput = (int)swrControl_slideButton;
        throttleRaw = swrRace_ThrottleInput;

        if ((swrControl_brakeIsAnalog == 0 && brakeDigital != 0) || thrustDigital != 0) {
            score->flag &= ~0x8;
        } else if (swrRace_ThrottleInput != 0.0f) {
            score->flag |= 0x8;
        }
        if (thrustDigital != 0 && swrControl_brakeIsAnalog != 0 && brakeDigital != 0) {
            brakeDigital = 0;
        }

        pitchForward = swrRace_PitchInput * SWR_CTL_STEER_SCALE;
        if (SWR_CTL_STEER_SCALE < swrRace_PitchInput * SWR_CTL_STEER_SCALE) {
            pitchForward = SWR_CTL_STEER_SCALE;
        }
        // BUG (preserved from retail): the low clamp writes +0.8, not -0.8.
        if (pitchForward < -SWR_CTL_STEER_SCALE) {
            pitchForward = SWR_CTL_STEER_SCALE;
        }
    } else if (controlType >= 1 && controlType <= 7) {
        steerInput = swrControl_aPlayerAxisX[playerIndex];
        if (mirror) {
            steerInput = -steerInput;
            bankRight = bitset3 & 0x10;
            bankLeft = bitset3 & 0x20;
        } else {
            bankLeft = bitset3 & 0x10;
            bankRight = bitset3 & 0x20;
        }
        pitchForward = swrControl_aPlayerAxisY[playerIndex];
        thrustDigital = (int) swrRace_ThrustInput;
        brakeDigital = bitset3 & 0x2;
        lookBackInput = bitset3 & 0x8;
        viewButtonInput = inRaceLocalPlayerInputBitset1[playerIndex] & 0x4;
        if ((inRaceLocalPlayerInputBitset1[playerIndex] & 0x80) != 0) {
            if ((float) timetotal - player->tauntTapTime > (float) SWR_CTL_BOOST_TAP_TIMEOUT) {
                *((char*) &player->tauntTapState + 1) = 0;
            }
            char taps = *((char*) &player->tauntTapState + 1) + 1;
            player->tauntTapTime = (float) timetotal;
            *((char*) &player->tauntTapState + 1) = taps;
            if (taps > 1) {
                flameTaunt = 1;
            }
        }
        if ((*(char*) &inRaceLocalPlayerInputBitset2[playerIndex] & 0x80) != 0) {
            player->tauntTapTime = (float) timetotal;
        }
        if ((bitset3 & 0x80) != 0 && (float) timetotal - player->tauntTapTime > (float) SWR_CTL_HALF_HOLD) {
            repairHold = true;
        }
        slideInput = (bitset3 & 0x100) != 0;
        if (controlType == 1) {
            slideInput = thrustDigital == 0;
        }
    }

    int boostCharge = swrRace_BoostCharge((int) player);

    // ---- dual-control mode (N64 leftover): one pod driven by two controllers, one stick per engine.
    // Throttle = either stick forward, steer = difference between the sticks, brake = both back. ----
    if ((swrRace_DebugFlag & swrRace_DEBUGFLAG_DUAL_CONTROL) != 0) {
        int dualIndex = (char) player->score_ptr->sfxChannel != '\0';
        float steerA = swrControl_aPlayerAxisX[dualIndex];
        float pitchA = swrControl_aPlayerAxisY[dualIndex];
        float steerB = swrControl_aPlayerAxisX[2 + dualIndex];
        float pitchB = swrControl_aPlayerAxisY[2 + dualIndex];
        thrustDigital = pitchA > SWR_CTL_THROTTLE_SETTLE_MIN || pitchB > SWR_CTL_THROTTLE_SETTLE_MIN;
        if (mirror == 0) {
            steerInput = (pitchA - pitchB) * SWR_CTL_HALF;
            bankLeft = steerA < SWR_CTL_NEG_HALF && steerB < SWR_CTL_NEG_HALF;
            bankRight = steerA > SWR_CTL_HALF && steerB > SWR_CTL_HALF;
        } else {
            steerInput = (pitchA - pitchB) * SWR_CTL_NEG_HALF;
            bankRight = steerA < SWR_CTL_NEG_HALF && steerB < SWR_CTL_NEG_HALF;
            bankLeft = steerA > SWR_CTL_HALF && steerB > SWR_CTL_HALF;
        }
        pitchForward = (pitchB + pitchA) * SWR_CTL_HALF;
        brakeDigital = pitchA < SWR_CTL_DMG_STEER_PULL && pitchB < SWR_CTL_DMG_STEER_PULL;
        viewButtonInput = inRaceLocalPlayerInputBitset1[dualIndex] & 0x4;
        lookBackInput = inRaceLocalPlayerInputBitset3[dualIndex] & 0x8;
        if ((inRaceLocalPlayerInputBitset1[dualIndex] & 0x80) != 0) {
            if ((float) timetotal - player->tauntTapTime > (float) SWR_CTL_BOOST_TAP_TIMEOUT) {
                *((char*) &player->tauntTapState + 1) = 0;
            }
            char taps = *((char*) &player->tauntTapState + 1) + 1;
            player->tauntTapTime = (float) timetotal;
            *((char*) &player->tauntTapState + 1) = taps;
            if (taps > 1) {
                flameTaunt = 1;
            }
        }
        if ((*(char*) &inRaceLocalPlayerInputBitset2[dualIndex] & 0x80) != 0) {
            player->tauntTapTime = (float) timetotal;
        }
        if ((*(char*) &inRaceLocalPlayerInputBitset3[dualIndex] & 0x80) != 0 &&
            (float) timetotal - player->tauntTapTime > (float) SWR_CTL_HALF_HOLD) {
            repairHold = true;
        }
        slideInput = steerA > SWR_CTL_HALF && steerB < SWR_CTL_NEG_HALF;
        playerIndex = dualIndex;
    }

    // ---- scripted spline-fork override: force branch 1 during the script 5/6 lap windows ----
    // (retail clears the slot with a float 0.0 store and sets it with an int 1; both are the same
    // dword on this int field, so the plain integer assignments below are bit-identical.)
    player->splineCursor.branchSelector = 0;
    if (ai_track_script > 0) {
        if (ai_track_script == 5 && SWR_CTL_SCRIPT5_LAP_MIN < player->lapComp && player->lapComp < SWR_CTL_SCRIPT5_LAP_MAX) {
            player->splineCursor.branchSelector = 1;
        }
        if (ai_track_script == 6 && SWR_CTL_SCRIPT6_LAP_MIN < player->lapComp && player->lapComp < SWR_CTL_SCRIPT6_LAP_MAX) {
            player->splineCursor.branchSelector = 1;
        }
    }

    int damagedSides = swrRace_GetDamagedEngineSides(player);
    if (damagedSides != 0) {
        damagedSides = 1;
        bankLeft = 0;
        bankRight = 0;
        boostCharge = 0;
        player->flags0 &= ~swrObjTest_FLAG0_BOOSTING;
    }

    // ---- engine-damage steering pull + force feedback ----
    if ((player->flags0 & swrObjTest_FLAG0_THROTTLE_SETTLED) != 0 &&
        (player->flags0 & swrObjTest_FLAG0_BRAKING) == 0 &&
        (player->flags1 & swrObjTest_FLAG1_FINISHED) == 0) {
        float penalty = swrRace_GetEngineDamagePenalty(player);
        if ((damagedSides & 1) != 0) {
            penalty -= SWR_CTL_DMG_PEN_LEFT;
        } else if ((damagedSides & 2) != 0) {
            penalty -= SWR_CTL_DMG_PEN_RIGHT;
        }
        penalty *= SWR_CTL_DMG_PEN_GAIN;
        if (penalty == 0.0f) {
            swrControl_StopForceEffect(1);
        } else {
            int direction = penalty >= 0.0f ? 0x5a + 0xb4 : 0x5a;
            int magnitude = (int) (SWR_CTL_FF_MAG_BASE - penalty * SWR_CTL_FF_MAG_SLOPE);
            swrControl_PlayForceEffect(1, magnitude, direction, -1);
            swrControl_damageForceEffectPreempted = 1;
        }
        steerInput -= penalty * SWR_CTL_DMG_STEER_PULL;
        swrRace_engineDamageSteeringPenalty = penalty;
    }

    // ---- debug event dispatch ('Snap' teleport + reset-input) ----
    if ((swrRace_DebugFlag & 0x100) != 0 && swrRace_DebugLevel != 0) {
        if ((bitset3 & 0x800) != 0 || (bitset3 & 0x400) != 0) {
            int snap[2];
            snap[0] = 0x536e6170; // 'Snap'
            snap[1] = (bitset3 & 0x800) != 0 ? 1 : 2;
            swrEvent_DispatchSubEvents(player, snap);
        }
        if ((swrRace_DebugFlag & 0x100) != 0 && swrRace_DebugLevel != 0) {
            swrRace_CheckResetInput(player, playerIndex);
        }
    }

    // ---- flame attack / taunt ----
    if (flameTaunt != 0 || swrRace_debugTauntRequest != 0) {
        swrRace_debugTauntRequest = 0;
        if (((uint8_t) player->flags0 & 0xf) == swrObjTest_FLAG0_RACING &&
            (player->flags0 & (swrObjTest_FLAG0_RESET | swrObjTest_FLAG0_DEAD | swrObjTest_FLAG0_POD_HIDDEN)) == 0 &&
            (player->flags1 & swrObjTest_FLAG1_EXPLODING) == 0) {
            int soundSource = *score->pilotId;
            if (soundSource == 2 || racer_options.flamejet_local) {
                swrRace_SpawnFlameAttack(player);
                // retail passes only the timestamp; the reconstructed 5-arg prototype's extra params are fillers.
                swrRace_SendFlameAttackEvent((int) score->time_unk, 0.0, NULL, NULL, 0);
            }
            int rng = swrUtils_Rand();
            int sfxRet;
            int to;
            if ((float) SWR_CTL_HALF_HOLD <= (float) rng * SWR_CTL_RAND_NORM) {
                sfxRet = swrSound_PlayRandomSfx(1, soundSource, 0x15, 0x16, 0x17, 0x18, 0x19,
                                                (rdVector3*) &player->transform.vD);
                to = -1;
            } else if (soundSource == 0xe) {
                sfxRet = swrSound_PlayRandomSfx(1, 0xe, 3, 0x12, 0x12, 0x13, 0x14,
                                                (rdVector3*) &player->transform.vD);
                to = -1;
            } else {
                sfxRet = swrSound_PlayRandomSfx(1, soundSource, 3, 0x11, 0x12, 0x13, 0x14,
                                                (rdVector3*) &player->transform.vD);
                to = 1;
            }
            swrMultiplayer_SendEvent(to, 0, 'taun', 1, soundSource, sfxRet);
        }
    }

    // ---- flags0 write-back: INPUT_STATE (0x100000), REPAIRING (0x400), clear high bit ----
    player->flags0 = lookBackInput != 0 ? (player->flags0 | swrObjTest_FLAG0_LOOK_BACK)
                                        : (player->flags0 & ~swrObjTest_FLAG0_LOOK_BACK);
    player->flags0 = repairHold ? (player->flags0 | swrObjTest_FLAG0_REPAIRING)
                                : (player->flags0 & ~swrObjTest_FLAG0_REPAIRING);
    player->flags0 &= 0x7fffffff;

    // ---- camera ('cMan') event: view button held -> 'CBut', else a pending debug camera event ----
    if (((uint8_t) player->flags0 & 0xf) != 0 && (player->flags0 & swrObjTest_FLAG0_DEAD) == 0) {
        if (viewButtonInput == 0) {
            if (swrRace_pendingCameraEvent != 0) {
                int event[2];
                event[0] = swrRace_pendingCameraEvent;
                event[1] = (int) player;
                swrEvent_CallF4(0x634d616e, event); // 'cMan'
                swrRace_pendingCameraEvent = 0;
            }
        } else {
            int event[2];
            event[0] = 0x43427574; // 'CBut'
            event[1] = (int) player;
            swrEvent_CallF4(0x634d616e, event); // 'cMan'
        }
    }

    swrRace_ApplyEngineDamage(player);
    swrRace_Repair(player);

    // ---- zero-g (ZON): the pitch stick becomes a pitch-RATE demand ----
    if ((player->flags0 & swrObjTest_FLAG0_ZON) != 0) {
        if (pitchForward >= SWR_CTL_STEER_DEADZONE || -pitchForward >= SWR_CTL_STEER_DEADZONE) {
            float squared = pitchForward * SWR_CTL_STEER_SQUARE_GAIN * (pitchForward * SWR_CTL_STEER_SQUARE_GAIN);
            if (pitchForward < 0.0f) {
                squared = -squared;
            }
            player->zeroGPitchRateTarget = -(player->podStats.maxTurnRate * squared * SWR_CTL_STEER_SCALE) * SWR_CTL_HALF;
        } else {
            player->zeroGPitchRateTarget = 0.0f;
        }
        pitchForward = 0.0f;
    }

    // ---- boost-start window (flags1 0x800 -> 0x1000 -> 0x2000) ----
    if ((swrRace_ThrustInput != 0.0f && swrControl_thrustHoldTime < (float) SWR_CTL_BOOST_WINDOW &&
         (float) SWR_CTL_BOOST_WINDOW_LO < swrControl_thrustHoldTime) ||
        (swrRace_ThrottleInput > (float) SWR_CTL_BOOST_THROTTLE && swrRace_BoostInput != 0.0f &&
         swrControl_boostHoldTime < (float) SWR_CTL_BOOST_WINDOW &&
         (float) SWR_CTL_BOOST_WINDOW_LO < swrControl_boostHoldTime)) {
        if ((player->flags1 & swrObjTest_FLAG1_BOOST_START_CANCEL) == 0) {
            player->flags1 |= swrObjTest_FLAG1_BOOST_START_CANCEL;
            if ((player->flags1 & swrObjTest_FLAG1_BOOST_START_WINDOW) != 0) {
                player->flags1 |= swrObjTest_FLAG1_BOOST_START;
            }
        }
    }
    if ((player->flags1 & swrObjTest_FLAG1_BOOST_START) != 0) {
        if (swrSound_TestSfxFlag(0, 0x80000) == 0) {
            swrSound_PlayRandomSfx(1, *score->pilotId, 2, 2, 2, 2, 2, (rdVector3*) &player->transform.vD);
            swrSound_SetSfxFlag(0, 0x80000);
        }
        if ((swrRace_ThrustInput == 0.0f && swrRace_ThrottleInput <= (float) SWR_CTL_BOOST_THROTTLE) ||
            player->speedValue > SWR_CTL_BOOST_CANCEL_SPEED) {
            player->flags1 &= ~swrObjTest_FLAG1_BOOST_START;
        }
    }

    if (((uint8_t) player->flags0 & 0xf) != swrObjTest_FLAG0_RACING) {
        return;
    }
    if ((player->flags0 & swrObjTest_FLAG0_DEAD) != 0) {
        return;
    }

    // ---- BRAKING ----
    if (brakeDigital == 0) {
        player->flags0 &= ~swrObjTest_FLAG0_BRAKING;
    } else {
        player->flags0 |= swrObjTest_FLAG0_BRAKING;
        if (player->speedValue < (float) SWR_CTL_BRAKE_SETTLE_SPEED) {
            player->flags0 = (player->flags0 & ~swrObjTest_FLAG0_THROTTLE_SETTLED) | swrObjTest_FLAG0_BRAKING;
        }
    }

    // ---- throttle ----
    if ((score->flag & 0x8) != 0) {
        float mapped = (throttleRaw - SWR_CTL_NEG_ONE) * SWR_CTL_HALF * SWR_CTL_ANALOG_THROTTLE_GAIN;
        player->throttle = mapped;
        if (mapped > SWR_CTL_ONE) {
            player->throttle = 1.0f;
        }
        if (player->throttle < SWR_CTL_NEG_ONE) {
            player->throttle = -1.0f;
        }
    } else if (thrustDigital == 0) {
        if (pitchForward >= SWR_CTL_NEG_HALF || player->speedValue >= (float) SWR_CTL_IDLE_SPEED) {
            player->throttle = 0.1f;
        } else {
            player->throttle = pitchForward * SWR_CTL_PITCH_TRIM;
        }
    } else {
        player->throttle = 1.0f;
        if (damagedSides != 0) {
            player->throttle = 0.5f;
        }
    }

    if (player->wallHitCooldown <= 0.0f && player->throttle > SWR_CTL_THROTTLE_SETTLE_MIN) {
        player->flags0 |= swrObjTest_FLAG0_THROTTLE_SETTLED;
    }

    // ---- SLIDING (airborne forces it) ----
    uint32_t sliding = (player->flags1 & swrObjTest_FLAG1_AIRBORNE) != 0 ? 1u : (uint32_t) slideInput;
    player->flags1 = sliding != 0 ? (player->flags1 | swrObjTest_FLAG1_SLIDING)
                                  : (player->flags1 & ~swrObjTest_FLAG1_SLIDING);

    // ---- NOT_ACCEL ----
    player->flags1 = (thrustDigital != 0 || throttleRaw > SWR_CTL_HALF)
                         ? (player->flags1 & ~swrObjTest_FLAG1_NOT_ACCEL)
                         : (player->flags1 | swrObjTest_FLAG1_NOT_ACCEL);

    // ---- slide2 easing ----
    if (sliding == 0 || player->speedValue <= SWR_CTL_SLIDE_SPEED) {
        player->slide = (float)(player->slide - swrRace_deltaTimeSecs * SWR_CTL_HALF_HOLD);
        if (player->slide < 0.0f) {
            player->slide = 0.0f;
        }
    } else {
        player->slide = (float)(player->slide - swrRace_deltaTimeSecs * SWR_CTL_NEG_HALF);
        if (player->slide > SWR_CTL_ONE) {
            player->slide = 1.0f;
        }
    }

    // ---- boost latch ----
    if (boostCharge != 0 && canChargeBoost != 0) {
        swrUtils_Rand();
        player->flags0 |= swrObjTest_FLAG0_BOOSTING;
    }
    if ((player->flags0 & swrObjTest_FLAG0_BOOSTING) != 0 && thrustDigital == 0 && throttleRaw <= SWR_CTL_HALF) {
        player->flags0 &= ~swrObjTest_FLAG0_BOOSTING;
    }

    player->unk1f14 = (float)(player->unk1f14 - swrRace_deltaTimeSecs);

    // ---- tilt / bank ----
    if ((bankLeft != 0 || bankRight != 0) && (player->flags1 & swrObjTest_FLAG1_MAGNET) != 0) {
        swrSound_Play2D(0x4b, 7, 0.5f, 1.0f, 1);
    }
    float tilt = (bankLeft != 0 && bankRight == 0) ? -1.0f : (bankLeft == 0 && bankRight != 0) ? 1.0f : 0.0f;
    swrRace_Tilt(player, tilt);

    // ---- turnRateTarget from steer (squared) ----
    if (steerInput >= SWR_CTL_STEER_DEADZONE || -steerInput >= SWR_CTL_STEER_DEADZONE) {
        // retail squares in ST0 and applies the scale before maxTurnRate
        const long double scaled = (long double) steerInput * SWR_CTL_STEER_SQUARE_GAIN;
        long double squared = scaled * scaled;
        if (steerInput < 0.0f) {
            squared = -squared;
        }
        squared *= SWR_CTL_STEER_SCALE;
        player->turnRateTarget = (float) -(player->podStats.maxTurnRate * squared);
    } else {
        player->turnRateTarget = 0.0f;
    }

    // ---- pitch trims turn rate + throttle ----
    player->pitch = pitchForward;
    if (pitchForward > SWR_CTL_PITCH_HI) {
        player->turnRateTarget *= (SWR_CTL_ONE - pitchForward * SWR_CTL_PITCH_TRIM);
        if (player->throttle > SWR_CTL_HALF) {
            player->throttle -= pitchForward * SWR_CTL_PITCH_THROTTLE_TRIM;
        }
    }
    if (pitchForward < SWR_CTL_PITCH_LO) {
        player->turnRateTarget *= (SWR_CTL_ONE - pitchForward * SWR_CTL_PITCH_TRIM);
        if ((player->flags1 & swrObjTest_FLAG1_AIRBORNE) == 0 && player->throttle > SWR_CTL_HALF) {
            player->throttle -= pitchForward * SWR_CTL_PITCH_THROTTLE_TRIM;
        }
    }
    if ((player->flags0 & swrObjTest_FLAG0_BOOST_OVERDRIVE) != 0 && player->throttle < SWR_CTL_FLAT_THROTTLE_FLOOR) {
        player->throttle = 1.2f;
    }

    player->paceMultiplier = 1.0f;
}

// 0x0046d170
void refined_swrObjTest_F0(swrRace* player)
{
    rdMatrix44 splinePoint;
    if (player->flags0 & swrObjTest_FLAG0_AI) {
        if ((player->flags0 & swrObjTest_FLAG0_STATE_MASK) == swrObjTest_FLAG0_RACING && (player->flags0 & (swrObjTest_FLAG0_RESPAWN_INVINC | swrObjTest_FLAG0_DEAD)) == 0) {
            // an AI stuck for 5 s (2 s on the simple path) asks for a respawn
            if (player->idleTick > 5.0f || ((player->flags0 & swrObjTest_FLAG0_AI_SIMPLE) && player->idleTick > 2.0f))
                player->flags0 |= swrObjTest_FLAG0_RESPAWN;
        } else {
            player->idleTick = 0.0f;
        }
    }

    if (player->flags0 & swrObjTest_FLAG0_TP_TO_SPLINE) {
        swrRace_ResetToSpline(player, 0.5f);
        player->flags0 &= ~swrObjTest_FLAG0_TP_TO_SPLINE;
    }

    // AI Sebulba from lap 3 on: flame attack at anyone close in front
    if ((player->flags0 & swrObjTest_FLAG0_AI) && (*player->score_ptr->pilotId == 2 || racer_options.flamejet_ai) && player->speedValue > 200.0f && player->score_ptr->results_P1_Lap + 1 >= 3) {
        rdVector3 pos = *(rdVector3*)&player->transform.vD;
        float distances[8];
        rdVector3 deltas[8];
        swrRace* nearby[8];
        int count = swrEvent_FindNearestObjects('Test', &pos, 10000.0f, NULL, 2, distances, deltas, (void**)nearby);
        for (int i = 0; i < count; i++) {
            if (nearby[i] == player)
                continue;
            rdVector3 toOther;
            rdVector_Sub3(&toOther, (rdVector3*)&nearby[i]->transform.vD, (rdVector3*)&player->transform.vD);
            if (player->transform.vA.x * toOther.x + player->transform.vA.y * toOther.y + player->transform.vA.z * toOther.z > 0.0f)
                swrRace_SpawnFlameAttack(player);
        }
    }

    if (player->score_ptr->results_P1_total_time < 10.0f)
        player->unk12_2 = 5.0f;
    if (player->flameSmokeHandle != NULL) {
        if (player->unk12_2 < 5.0f)
            player->unk12_2 = 5.0f;
    } else {
        player->unk12_2 -= swrRace_deltaTimeSecs;
    }

    player->lodDistance = (int)swrViewport_DistanceToNearestCamera((rdVector3*)&player->transform.vD);
    if (((player->lodDistance - 400.0f) * 0.0016666667f < 1.0 || (player->flags0 & swrObjTest_FLAG0_LOCAL) || (player->flags1 & swrObjTest_FLAG1_FORCE_GROUND)) && (player->flags1 & swrObjTest_FLAG1_SPLINE_SNAP)) {
        player->flags1 &= ~swrObjTest_FLAG1_SPLINE_SNAP;
        swrSpline_EvaluateAtOffset(&player->splineCursor, &splinePoint, 0.0f);
        player->transform.vD.x = splinePoint.vD.x;
        player->transform.vD.y = splinePoint.vD.y;
        player->transform.vD.z = splinePoint.vD.z;
        player->flags1 |= swrObjTest_FLAG1_IMMUNITY;
    }

    player->flags0 &= ~swrObjTest_FLAG0_POD_HIDDEN;
    if ((player->flags0 & swrObjTest_FLAG0_LOCAL) == 0 && player->wallHitCooldown <= 0.0f)
        player->flags0 |= swrObjTest_FLAG0_THROTTLE_SETTLED;

    player->flags0 &= ~swrObjTest_FLAG0_CAN_CHARGE_BOOST;
    if (player->podStats.maxSpeed * 0.75 < player->speedValue && (player->flags0 & (swrObjTest_FLAG0_RESPAWN | swrObjTest_FLAG0_RESPAWN_INVINC | swrObjTest_FLAG0_DEAD)) == 0 && (player->flags1 & swrObjTest_FLAG1_EXPLODING) == 0)
        player->flags0 |= swrObjTest_FLAG0_CAN_CHARGE_BOOST;

    if (player->podStats.maxSpeed < player->speedValue && (player->flags0 & swrObjTest_FLAG0_BOOSTING))
        player->flags0 |= swrObjTest_FLAG0_WAS_BOOSTING;
    else
        player->flags0 &= ~swrObjTest_FLAG0_WAS_BOOSTING;

    swrRace_CalcTargetTurnRate(player);
    if (player->wallHitCooldown > 0.0f)
        player->wallHitCooldown -= swrRace_deltaTimeSecs;
    swr_noop2();
    if (player->flags0 & swrObjTest_FLAG0_RESET)
        swrRace_HandleDeathSnap(player);
    swrRace_HandleRespawnFlag(player);
}
