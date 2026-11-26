// The faithful functions that index the per-lap time slots. Each is its src/ counterpart with
// every per-lap time access going through race_timing_lap() and F2's 3000 s clamp through
// race_timing_time_cap(); F2 also reports each racer to race_timing_track(). Nothing else differs,
// including the order of swrUtils_Rand draws.
#include "race_timing.h"

#include <Dss/sithMessage.h>
#include <General/utils.h>
#include <Main/swrControl.h>
#include <Swr/swrModel.h>
#include <Swr/swrMultiplayer.h>
#include <Swr/swrObj.h>
#include <Swr/swrRace.h>
#include <Swr/swrSound.h>
#include <Swr/swrSprite.h>
#include <Swr/swrSpline.h>
#include <Swr/swrText.h>
#include <Swr/swrUI.h>
#include <Swr/swrWeather.h>
#include <engine_config.h>
#include <globals.h>
#include <macros.h>
#include <swr.h>

#include <stdio.h>
#include <string.h>

// 0x0045ea30
void refined_swrObjJdge_F2(swrObjJdge* jdge)
{
    swrScore* score;
    swrRace* pod;
    float crossTime;
    float rank;
    float frameRemainder;
    int lap;
    int crossed;
    int i;

    if ((jdge->flag & 0xf) == 4 && jdge->camSweepState != NULL)
        swrSpline_EvaluateToMatrix(&jdge->camSweepCursor, &jdge->camSweepTransform);

    if ((jdge->flag & 0xf) == 1) {
        swrSpline_EvaluateToMatrix(&jdge->cursor, &jdge->camBaseMat);
        // provisionally advance to state 2 ("all local racers finished");
        // re-armed back to state 1 below while a local player is still racing
        jdge->flag = (jdge->flag & ~0xd) | 2;
        for (i = 0; i < jdge->num_players; i++) {
            score = &swrScoresPtr[i];
            if ((score->flag & 1) == 0 || (score->flag & 2) != 0) {
                if ((score->flag & 2) != 0)
                    swrRace_UpdateRaceProgress(score->obj_test_ptr, &crossTime);
                continue;
            }
            pod = score->obj_test_ptr;
            race_timing_track(score, jdge->num_laps);
            crossed = swrRace_UpdateRaceProgress(pod, &crossTime);
            if (debug_showSplineMarkers != 0)
                swrObjJdge_UpdateSplineGuideNodes((int)jdge, score);
            if (crossed) {
                if (jdge->planetId == 1) {
                    // Ando Prime: escalate the blizzard when the first lap completes
                    if (jdge->planet_track_number == 0) {
                        if (score->results_P1_Lap == 0) {
                            swrWeather_SetVelocity(25.0f, 45.0f);
                            swrWeather_SetParticleCap(20);
                        } else {
                            swrWeather_SetParticleCap(60);
                            swrWeather_SetVelocity(20.0f, 60.0f);
                            swrSprite_SetSunSpriteAlpha(0, 0x80);
                        }
                    } else if (jdge->planet_track_number == 1) {
                        if (score->results_P1_Lap == 0) {
                            swrWeather_SetParticleCap(60);
                            swrWeather_SetVelocity(20.0f, 60.0f);
                            swrSprite_SetSunSpriteAlpha(0, 0x80);
                        } else {
                            swrWeather_SetParticleCap(80);
                            swrWeather_SetVelocity(10.0f, 90.0f);
                            swrWeather_SetStretchFactor(1.0f);
                            swrSprite_SetSunSpriteAlpha(0, 0x14);
                        }
                    } else if (jdge->planet_track_number == 2) {
                        if (score->results_P1_Lap == 0) {
                            swrWeather_SetParticleCap(80);
                            swrWeather_SetVelocity(10.0f, 60.0f);
                            swrSprite_SetSunSpriteAlpha(0, 0x40);
                        } else {
                            swrWeather_SetParticleCap(80);
                            swrWeather_SetVelocity(10.0f, 90.0f);
                            swrWeather_SetStretchFactor(1.0f);
                            swrSprite_SetSunSpriteAlpha(0, 0x14);
                        }
                    }
                }
                // close out the finished lap with the pre-crossing slice of this frame
                score->results_P1_total_time += crossTime;
                lap = score->results_P1_Lap;
                (*race_timing_lap(score, lap, jdge->num_laps)) += crossTime;
                if ((pod->flags0 & swrObjTest_FLAG0_LOCAL) != 0 && (*race_timing_lap(score, lap, jdge->num_laps)) < jdge->best_lap_time_ms)
                    jdge->best_lap_time_ms = (*race_timing_lap(score, lap, jdge->num_laps));
                lap++;
                score->results_P1_Lap = lap;
                if (lap == jdge->num_laps) {
                    if ((pod->flags0 & swrObjTest_FLAG0_LOCAL) != 0) {
                        pod->flags1 |= swrObjTest_FLAG1_FORCE_GROUND;
                        swrControl_StopAllForceEffects(0);
                        swrRace_resultsScreenActive = 0;
                        if ((*race_timing_lap(score, jdge->num_laps - 1, jdge->num_laps)) <= jdge->best_lap_time_ms) {
                            // final lap tied/beat the session best: fanfare (0x27) once
                            if (swrSound_TestSfxFlag((char)score->sfxChannel, swrSound_SFXFLAG_LAP_FANFARE) == 0) {
                                if ((short)score->results_P1_Position == 1)
                                    swrSound_PlaySfxThenDelayed(1, *score->pilotId, 0xf, 6, 0, 0x27);
                                else if ((short)score->results_P1_Position < 5)
                                    swrSound_PlaySfxThrottled(6, 0, 0x27, NULL);
                                else
                                    swrSound_PlaySfxThenDelayed(1, *score->pilotId, 0x10, 6, 0, 0x27);
                                swrSound_SetSfxFlag((char)score->sfxChannel, swrSound_SFXFLAG_LAP_FANFARE);
                            }
                        } else {
                            // announcer line by finishing position (0xf = won, 0x10 = trailing)
                            if ((short)score->results_P1_Position == 1)
                                swrSound_PlaySfxThrottled(1, *score->pilotId, 0xf, NULL);
                            else if (4 < (short)score->results_P1_Position)
                                swrSound_PlaySfxThrottled(1, *score->pilotId, 0x10, NULL);
                        }
                    }
                    score->flag |= 2;
                    swrMultiplayer_SendEvent(SITH_MESSAGE_TO_ALL, sithMessage_SEND_GUARANTEED, 'fini');
                    pod->flags0 &= ~swrObjTest_FLAG0_LOCAL;
                    pod->flags0 &= ~(swrObjTest_FLAG0_CAN_CHARGE_BOOST | swrObjTest_FLAG0_BOOSTING);
                    pod->flags0 |= swrObjTest_FLAG0_AI;
                    pod->flags1 |= swrObjTest_FLAG1_FINISHED;
                } else {
                    (*race_timing_lap(score, lap, jdge->num_laps)) = 0.0f;
                }
            } else {
                crossTime = 0.0f;
            }
            if ((score->flag & 2) == 0) {
                // the rest of the frame delta accrues to the (possibly new) current lap;
                // timing uses the raw, unclamped delta, capped at race_timing_time_cap()
                frameRemainder = (float)(swrRace_dt_raw_d - crossTime);
                score->results_P1_total_time += frameRemainder;
                if (race_timing_time_cap() < score->results_P1_total_time)
                    score->results_P1_total_time = race_timing_time_cap();
                lap = score->results_P1_Lap;
                (*race_timing_lap(score, lap, jdge->num_laps)) += frameRemainder;
                if (race_timing_time_cap() < (*race_timing_lap(score, lap, jdge->num_laps)))
                    (*race_timing_lap(score, lap, jdge->num_laps)) = race_timing_time_cap();
                if (firstLocalPlayer == NULL || score == firstLocalPlayer || score == secondLocalPlayer || score == thirdLocalPlayer || score == fourthLocalPlayer)
                    jdge->flag = (jdge->flag & ~0xe) | 1;
            }
        }
        swrObjJdge_UpdateStandings(jdge);
        swrObjJdge_PlayPassingVoiceLines(jdge);
        if ((jdge->flag & 0xf) == 2) {
            // every local racer just finished: freeze the race timer and extrapolate
            // unfinished racers' totals from their lap progress
            jdge->raceTimer_ms = 0.0f;
            for (i = 0; i < jdge->num_players; i++) {
                score = &swrScoresPtr[i];
                rank = swrObjJdge_GetRacerRankValue(score);
                if (rank < (float)jdge->num_laps) {
                    score->results_P1_total_time = (score->results_P1_total_time / rank) * (float)jdge->num_laps;
                    score->obj_test_ptr->flags0 &= ~(swrObjTest_FLAG0_CAN_CHARGE_BOOST | swrObjTest_FLAG0_BOOSTING);
                    score->obj_test_ptr->flags1 |= swrObjTest_FLAG1_FINISHED;
                }
            }
        }
    } else if ((jdge->flag & 0xf) == 2) {
        for (i = 0; i < jdge->num_players; i++)
            swrRace_UpdateRaceProgress(swrScoresPtr[i].obj_test_ptr, &crossTime);
    } else if ((jdge->flag & 0xf) != 6) {
        for (i = 0; i < jdge->num_players; i++)
            swrRace_UpdateRaceProgress(swrScoresPtr[i].obj_test_ptr, &crossTime);
    }
}

// 0x00460950
RETAIL_FP void refined_swrRace_InRaceTimer(swrScore* score, swrObjJdge* jdge)
{
    swrRace* pod;
    char* text;
    float x;
    float y;
    float speed;
    float lastLapTime;
    float fade;
    long double flicker;// truncated straight from ST in retail
    int numLocal;
    int isPlayer2;
    int dialY;
    int lap;
    int lapShown;
    int colR;
    int colG;
    int colB;
    int alpha;
    int lapTimeShown;
    int blinkLapCounter;
    char buffer[256];

    dialY = 0xa4;
    numLocal = swrObjJdge_NumLocalPlayers();
    pod = score->obj_test_ptr;
    isPlayer2 = (score == secondLocalPlayer) ? 1 : 0;
    if (swrRace_GetPauseState() == 0) {
        // retail tests the unrounded difference
        long double vib = pod->vibrationMagnitude - swrRace_deltaTimeSecs;
        pod->vibrationMagnitude = vib;
        if (vib < 0.0f)
            pod->vibrationMagnitude = 0.0f;
    }
    swrObjJdge_LayoutHudFrameSprites(jdge->hud_mode == swrObjJdge_HUDMODE_PROGRESS_RING ? 5 : (jdge->hud_mode == swrObjJdge_HUDMODE_GAP_ARROWS ? 2 : 0));

    // speed readout
    x = 254.0f;
    y = 190.0f;
    if (numLocal == 2) {
        x = 277.0f;
        y = (float)(isPlayer2 * 0x6e + 0x60);
    }
    speed = pod->speedValue;
    if (speed <= 0.0f)
        speed = 0.0f;
    text = swrText_Translate("~f2~c~s%.0f");
    sprintf(buffer, text, (double)speed);
    swrText_CreateTextEntry1((int)x, (int)y, 0, -0x3d, -2, -2, buffer);

    // lap-time popup panel position (the progress ring occupies the default spot)
    if (jdge->hud_mode == swrObjJdge_HUDMODE_PROGRESS_RING) {
        x = 240.0f;
        y = 30.0f;
    } else {
        x = 160.0f;
        y = 23.0f;
        if (numLocal == 2)
            y = (float)(isPlayer2 * 0x6e + 0x14);
    }
    lap = score->results_P1_Lap;
    lapTimeShown = 0;
    blinkLapCounter = 0;
    if (0 < lap) {
        lastLapTime = (*race_timing_lap(score, lap - 1, jdge->num_laps));
        // the popup fades out over the first 4 seconds of the new lap
        fade = 1.0f - (*race_timing_lap(score, lap, jdge->num_laps)) * 0.25f;
        if (fade <= 0.0f || 1.0f <= fade) {
            // popup window over: release the fanfare guard once the sfx cooldown lapses
            if (swrSound_TestSfxFlag((char)score->sfxChannel, swrSound_SFXFLAG_LAP_FANFARE) != 0 && swrSound_IsSfxOnCooldown(6, 0) == 0)
                swrSound_ClearSfxFlag((char)score->sfxChannel, swrSound_SFXFLAG_LAP_FANFARE);
        } else {
            flicker = 223.25f;
            if (pauseState == 0)
                flicker = (long double)swrUtils_Rand() * (1.0f / 2147483648.0f) * 127.0f + 128.0f;
            colR = (int)flicker;
            colG = (int)((float)colR * 0.5f);
            alpha = (int)(fade * 255.0f) * 8;
            if (255 < alpha)
                alpha = 255;
            text = swrText_Translate("~f3~c~s");
            swrText_CreateTimeEntry((int)x, (int)y, lastLapTime, colR, colG, 0x40, alpha, text);
            text = swrText_Translate("/SCREENTEXT_420/~c~sLAP TIME");
            swrText_CreateTextEntry1((int)x, (int)y + 17, colR, colG, 0x40, alpha, text);
            if (lastLapTime <= jdge->best_lap_time_ms && ((int)(fade * 16.0f) & 1) != 0) {
                // this lap tied/beat the session best: blinking "New Record" + one fanfare
                text = swrText_Translate("/SCREENTEXT_538/~s~cNew Record");
                swrText_CreateTextEntry1((int)x, (int)y + 25, -0x38, -1, 0, alpha, text);
                if (swrSound_TestSfxFlag((char)score->sfxChannel, swrSound_SFXFLAG_LAP_FANFARE) == 0) {
                    swrSound_PlaySfxThrottled(6, 0, 0x27, NULL);
                    swrSound_SetSfxFlag((char)score->sfxChannel, swrSound_SFXFLAG_LAP_FANFARE);
                }
            }
            lapTimeShown = 1;
            if (lap + 1 == jdge->num_laps) {
                if (numLocalPlayers < 2) {
                    // FINAL LAP banner, fading in over the second half of the popup window
                    long double bannerFade = (fade - 0.5f) * 2.0f;// unrounded in retail
                    if (0.0f < bannerFade) {
                        alpha = (int)(bannerFade * 255.0f) * 4;
                        if (255 < alpha)
                            alpha = 255;
                        colR = (int)(pauseState == 0 ? (long double)swrUtils_Rand() * (1.0f / 2147483648.0f) * 255.0f : 191.25f);
                        colG = (int)(pauseState == 0 ? (long double)swrUtils_Rand() * (1.0f / 2147483648.0f) * 255.0f : 191.25f);
                        colB = (int)(pauseState == 0 ? (long double)swrUtils_Rand() * (1.0f / 2147483648.0f) * 255.0f : 191.25f);
                        text = swrText_Translate("/SCREENTEXT_526/~f6~s~cFINAL LAP");
                        swrText_CreateTextEntry1(0xa0, 0x46, colR, colG, colB, alpha, text);
                    }
                } else {
                    // splitscreen has no room for the banner: blink the LAP counter instead
                    blinkLapCounter = ((int)(fade * 36.0f) & 1) != 0;
                }
            }
        }
    }
    if (lapTimeShown == 0 && numLocal < 2) {
        // no lap popup: show the running total race time
        swrRace_hudTimeLabelShown = 0;
        text = swrText_Translate("~f3~c~s");
        swrText_CreateTimeEntry((int)x, (int)y, score->results_P1_total_time, -1, -1, -1, -0x42, text);
        text = swrText_Translate("/SCREENTEXT_422/~c~sTIME");
        swrText_CreateTextEntry1((int)x, (int)y + 17, -1, -1, -1, -0x42, text);
    }
    if (jdge->hud_mode == swrObjJdge_HUDMODE_SPLIT_OFF || jdge->hud_mode == swrObjJdge_HUDMODE_SPLIT_COLUMN_TIME)
        swrText_CreateTimeEntry(0x121, (int)y, score->results_P1_total_time, -1, -1, -1, -0x42, "~f3~r~s");

    // LAP x/y counter
    x = 62.0f;
    if (jdge->hud_mode != swrObjJdge_HUDMODE_PROGRESS_RING)
        x = 42.0f;
    lapShown = lap + 1;
    if (jdge->num_laps < lapShown)
        lapShown = jdge->num_laps;
    text = swrText_Translate("~f3~c~s%d/%d");
    sprintf(buffer, text, lapShown, jdge->num_laps);
    if (numLocalPlayers < 2 || blinkLapCounter == 0) {
        swrText_CreateTextEntry1((int)x, (int)y, -1, -1, -1, -0x42, buffer);
        text = swrText_Translate("/SCREENTEXT_424/~c~sLAP");
        swrText_CreateTextEntry1((int)x, (int)y + 17, -1, -1, -1, -0x42, text);
    } else {
        swrText_CreateTextEntry1((int)x, (int)y, -1, 0x3f, 0x3f, -1, buffer);
        text = swrText_Translate("/SCREENTEXT_424/~c~sLAP");
        swrText_CreateTextEntry1((int)x, (int)y + 17, -1, 0x3f, 0x3f, -1, text);
    }

    // POS x/y counter (hidden in hud modes 1/6/7)
    if (jdge->hud_mode != swrObjJdge_HUDMODE_PROGRESS_RING && jdge->hud_mode != swrObjJdge_HUDMODE_SPLIT_OFF && jdge->hud_mode != swrObjJdge_HUDMODE_SPLIT_COLUMN_TIME) {
        if (0 < (short)score->results_P1_Position) {
            text = swrText_Translate("~f3~c~s%d/%d");
            sprintf(buffer, text, (int)(short)score->results_P1_Position, jdge->num_players);
            swrText_CreateTextEntry1(0x116, (int)y, -1, -1, -1, -0x42, buffer);
        }
        text = swrText_Translate("/SCREENTEXT_426/~c~sPOS");
        swrText_CreateTextEntry1(0x116, (int)y + 17, -1, -1, -1, -0x42, text);
    }

    if (1 < numLocalPlayers && isPlayer2 == 0)
        dialY = 0x36;
    swrObjJdge_DrawBoostDialHud(jdge, pod, 0xe1, (short)dialY, isPlayer2);
    if (swrRace_DebugLevel != 0 && assetBufferOverflow != 0)
        swrText_CreateTextEntry1(0xa0, 0x14, -1, 0, 0, -1, "~c~oZOT");
}

// 0x00462320
void refined_swrRace_InRaceEndStatistics(swrObjJdge* jdge, swrScore* score)
{
    char* text;
    float total;
    float lapTime;
    int numLocal;
    int x;
    int rowY;
    int placeY;
    int textY;
    int colR;
    int colG;
    int colB;
    int i;
    short pos;
    short spriteX;
    char buf[32];

    numLocal = swrObjJdge_NumLocalPlayers();
    if (numLocal == 2) {
        if (score == firstLocalPlayer) {
            rowY = 0x69;
            placeY = 0x1e;
            for (i = 0xf; i < 0x13; i++)
                swrSprite_SetVisible((short)i, 0);
        } else {
            rowY = 0xd7;
            placeY = 0x8c;
            for (i = 0x13; i < 0x17; i++)
                swrSprite_SetVisible((short)i, 0);
        }
    } else {
        for (i = 0; i < 0x13; i++)
            swrSprite_SetVisible((short)i, 0);
        rowY = 0xd7;
        placeY = 0x37;
    }
    rowY -= jdge->num_laps * 0xe;

    // time-column x scales with the total's digit count
    total = score->results_P1_total_time;
    if (total < 60.0f)
        x = 0x5b;
    else if (total < 600.0f)
        x = 0x69;
    else if (total < 6000.0f)
        x = 0x73;
    else if (total < 60000.0f)
        x = 0x7d;
    else
        x = 0x87;

    for (i = 0; i < jdge->num_laps; i++) {
        lapTime = (*race_timing_lap(score, i, jdge->num_laps));
        text = swrText_Translate("/SCREENTEXT_211/~sLap");
        sprintf(buf, "~f4%s", text);
        swrText_CreateTextEntry1(0x19, rowY + 1, -1, -1, 0, -1, buf);
        sprintf(buf, "~s%d", i + 1);
        swrText_CreateTextEntry1(0x2d, rowY, -1, -1, 0, -1, buf);
        if (jdge->best_lap_time_ms < lapTime) {
            colR = 0xff;
            colG = 0x80;
        } else {
            // this row is the session-best lap: flicker it
            if (pauseState == 0)
                colR = (int)((long double)swrUtils_Rand() * (1.0f / 2147483648.0f) * 127.0f + 128.0f);
            else
                colR = (int)223.25f;
            colG = colR - 0x14;
        }
        swrText_CreateTimeEntry(x, rowY - 3, lapTime, colR, colG, 0, -1, "~f1~r~s");
        rowY += 0xe;
    }

    if (jdge->recordLap3_ms < total) {
        colR = 0x32;
        colG = 0xff;
        colB = 5;
    } else {
        // new session-best race total: latch it and flicker the row
        jdge->recordLap3_ms = total;
        if (pauseState == 0)
            colR = (int)((long double)swrUtils_Rand() * (1.0f / 2147483648.0f) * 26.0f + 24.0f);
        else
            colR = (int)43.5f;
        if (pauseState == 0)
            colG = (int)((long double)swrUtils_Rand() * (1.0f / 2147483648.0f) * 127.0f + 128.0f);
        else
            colG = (int)223.25f;
        colB = 0;
    }
    text = swrText_Translate("/SCREENTEXT_534/~sTotal: ");
    sprintf(buf, "~f4%s", text);
    swrText_CreateTextEntry1(0x19, rowY + 1, -0x10, -1, 0, -1, buf);
    swrText_CreateTimeEntry(x, rowY - 3, total, colR, colG, colB, -1, "~f1~r~s");

    if (numLocalPlayers < 2) {
        if (jdge->num_players < 2)
            return;
        if (8.0f < jdge->raceTimer_ms) {
            swrSprite_SetVisible(0xa4, 0);
            swrSprite_SetVisible(0xa5, 0);
            swrSprite_SetVisible(0xa6, 0);
            return;
        }
        // finishing-place banner slides in over the first half second and back out after 7.5s
        x = 0xa0;
        if (jdge->raceTimer_ms < 0.5f)
            x = (int)(160.0f - (0.5f - jdge->raceTimer_ms) * 380.0f);
        if (7.5f < jdge->raceTimer_ms)
            x = (int)((float)x + (jdge->raceTimer_ms - 7.5f) * 380.0f);
        textY = placeY;
        pos = (short)score->results_P1_Position;
        if (pos < 4) {
            x -= 0x14;
            textY = placeY + 0x14;
            swrSprite_AddDirtyRect(-x + 0x12f, placeY - 0xd, -x + 0x151, placeY + 0x35);
            spriteX = (short)(0x140 - x);
            if (pos == 1) {
                swrSprite_SetVisible(0xa4, 1);
                swrSprite_SetPos(0xa4, spriteX, (short)placeY);
            }
            if (pos == 2) {
                swrSprite_SetVisible(0xa5, 1);
                swrSprite_SetPos(0xa5, spriteX, (short)placeY);
            }
            if (pos == 3) {
                swrSprite_SetVisible(0xa6, 1);
                swrSprite_SetPos(0xa6, spriteX, (short)placeY);
            }
        }
        if (pos == 1)
            text = "/SCREENTEXT_427/~sst";
        else if (pos == 2)
            text = "/SCREENTEXT_428/~snd";
        else if (pos == 3)
            text = "/SCREENTEXT_429/~srd";
        else
            text = "/SCREENTEXT_430/~sth";
        text = swrText_Translate(text);
        colR = (int)(pauseState == 0 ? (long double)swrUtils_Rand() * (1.0f / 2147483648.0f) * 127.0f + 128.0f : 223.25f);
        colG = (int)(pauseState == 0 ? (long double)swrUtils_Rand() * (1.0f / 2147483648.0f) * 127.0f + 128.0f : 223.25f);
        colB = (int)(pauseState == 0 ? (long double)swrUtils_Rand() * (1.0f / 2147483648.0f) * 127.0f + 128.0f : 223.25f);
        sprintf(buf, "~f2~r~s%d", (int)pos);
        swrText_CreateTextEntry1(x, textY, colR, colG, colB, -2, buf);
        swrText_CreateTextEntry1(x + 1, textY, colR, colG, colB, -2, text);
        return;
    }

    // splitscreen: fixed-position place text, no medal sprite
    pos = (short)score->results_P1_Position;
    if (pos == 1)
        text = "/SCREENTEXT_427/~sst";
    else if (pos == 2)
        text = "/SCREENTEXT_428/~snd";
    else if (pos == 3)
        text = "/SCREENTEXT_429/~srd";
    else
        text = "/SCREENTEXT_430/~sth";
    text = swrText_Translate(text);
    colR = (int)(pauseState == 0 ? (long double)swrUtils_Rand() * (1.0f / 2147483648.0f) * 127.0f + 128.0f : 223.25f);
    colG = (int)(pauseState == 0 ? (long double)swrUtils_Rand() * (1.0f / 2147483648.0f) * 127.0f + 128.0f : 223.25f);
    colB = (int)(pauseState == 0 ? (long double)swrUtils_Rand() * (1.0f / 2147483648.0f) * 127.0f + 128.0f : 223.25f);
    sprintf(buf, "~f2~r~s%d", (int)pos);
    swrText_CreateTextEntry1(0xa0, placeY, colR, colG, colB, -2, buf);
    swrText_CreateTextEntry1(0xa1, placeY, colR, colG, colB, -2, text);
}

// 0x00439ce0
void refined_swrObjHang_ResultsMenu(swrObjHang* hang)
{
    swrScore* scores[20]; // race order: local players first
    float bestLap[2];
    swrScore* score;
    char* text;
    float rowYf;
    float alphaF;
    float lapTime;
    int rowY;
    int textY;
    int alpha;
    int spriteId;
    int pilot;
    int recordIdx;
    int lap;
    int i;
    int k;
    char effLaps;
    char newCircuitIdx;
    char minLocalPlace;
    char required;
    uint8_t favoritePilot;
    int storedPlaceBits;
    int gain;
    swrObjHang_STATE state2;
    char buffer[256];

    newCircuitIdx = hang->circuitIdx;
    bestLap[0] = ELFSAVE_RECORD_TIME_EMPTY;
    bestLap[1] = ELFSAVE_RECORD_TIME_EMPTY;
    g_CircuitIdxMax = (int)(150.0f - (float)hang->num_players * 30.0f); // scroll floor (see DrawScrollbar)
    effLaps = hang->numLaps;
    if (hang->isTournamentMode != 0)
        effLaps = 3;
    text = swrText_Translate("/SCREENTEXT_189/~c~sResults");
    swrText_CreateColorlessEntry1(0xa0, 0x14, text);
    for (i = 0; i < 20; i++)
        scores[i] = NULL;

    if (swrObjHang_menuJustEntered != 0) {
        swrObjHang_menuJustEntered = 0;
        swrRace_resultsFanfareDelay = 2.0f;
        for (i = 0; i < g_aTracksInCircuits[(int)hang->circuitIdx]; i++) {
            if ((int)hang->track_index == g_aTrackIDs[hang->circuitIdx * 7 + i]) {
                swrObjHang_trackInCircuitIdx = (uint8_t)i;
                break;
            }
        }
        swrRace_resultsScrollY = 0.0f;
        swrRace_TournamentTrugutGain = 0;
        for (i = 0; i < 20; i++)
            swrRace_resultsSortedScores[i] = NULL;
        // collect + sanitize: out-of-range totals and lap times become the empty-record value
        for (i = 0; i < hang->num_players; i++) {
            score = &swrScores[i];
            scores[i] = score;
            if (score == NULL) {
                hang->num_players = (char)i;
                break;
            }
            if (ELFSAVE_RECORD_TIME_EMPTY < score->results_P1_total_time || score->results_P1_total_time < 0.0f)
                score->results_P1_total_time = ELFSAVE_RECORD_TIME_EMPTY;
            for (lap = 0; lap < effLaps; lap++) {
                if (ELFSAVE_RECORD_TIME_EMPTY < (*race_timing_lap(score, lap, effLaps)) || (*race_timing_lap(score, lap, effLaps)) < 0.0f)
                    (*race_timing_lap(score, lap, effLaps)) = ELFSAVE_RECORD_TIME_EMPTY;
            }
            swrRace_resultsSortedScores[i] = score;
        }
        // sort by finishing position (bubble)
        for (i = 1; i < hang->num_players; i++) {
            for (k = 0; k < i; k++) {
                if ((short)swrRace_resultsSortedScores[i]->results_P1_Position < (short)swrRace_resultsSortedScores[k]->results_P1_Position) {
                    score = swrRace_resultsSortedScores[i];
                    swrRace_resultsSortedScores[i] = swrRace_resultsSortedScores[k];
                    swrRace_resultsSortedScores[k] = score;
                }
            }
        }
        // best single lap per local player (a non-positive lap time ends the scan)
        for (k = 0; k < hang->num_local_players; k++) {
            bestLap[k] = (*race_timing_lap(scores[k], 0, effLaps));
            for (lap = 1; lap < effLaps; lap++) {
                lapTime = (*race_timing_lap(scores[k], lap, effLaps));
                if (lapTime <= 0.0f)
                    break;
                if (lapTime < bestLap[k])
                    bestLap[k] = lapTime;
            }
        }
        // a record-setting guest profile (not linked to a save slot) gets sent to name entry
        recordIdx = hang->bMirror + hang->track_index * 2;
        if ((swrRace_resultsStateFlags & swrRace_RESULTSFLAG_NAME_ENTRY_P1) == 0 && swrRace_aProfiles[0].linkedToSave == 0 && ((effLaps == 3 && scores[0]->results_P1_total_time < swrRace_saveData.record3LapTimes[recordIdx] && (hang->num_local_players == 1 || scores[0]->results_P1_total_time < scores[1]->results_P1_total_time)) || (bestLap[0] < swrRace_saveData.recordLapTimes[recordIdx] && (hang->num_local_players == 1 || bestLap[0] < bestLap[1])))) {
            swrRace_resultsStateFlags |= swrRace_RESULTSFLAG_NAME_ENTRY_P1;
            hang->current_player_for_vehicle_selection = 0;
            swrObjHang_SetMenuState(hang, swrObjHang_STATE_ENTER_NAME);
            return;
        }
        if (1 < hang->num_local_players && (swrRace_resultsStateFlags & swrRace_RESULTSFLAG_NAME_ENTRY_P2) == 0 && swrRace_aProfiles[1].linkedToSave == 0 && ((effLaps == 3 && scores[1]->results_P1_total_time < swrRace_saveData.record3LapTimes[recordIdx] && scores[1]->results_P1_total_time < scores[0]->results_P1_total_time) || (bestLap[1] < swrRace_saveData.recordLapTimes[recordIdx] && bestLap[1] < bestLap[0]))) {
            swrRace_resultsStateFlags |= swrRace_RESULTSFLAG_NAME_ENTRY_P2;
            hang->current_player_for_vehicle_selection = 1;
            swrObjHang_SetMenuState(hang, swrObjHang_STATE_ENTER_NAME);
            return;
        }
        // commit new records: time + holder name + holder pilot from the live profile
        if ((swrRace_resultsStateFlags & swrRace_RESULTSFLAG_RECORDS_COMMITTED) == 0) {
            for (k = 0; k < hang->num_local_players; k++) {
                recordIdx = hang->bMirror + hang->track_index * 2;
                if (effLaps == 3 && scores[k]->results_P1_total_time < ELFSAVE_RECORD_TIME_EMPTY && scores[k]->results_P1_total_time < swrRace_saveData.record3LapTimes[recordIdx]) {
                    swrRace_saveData.record3LapTimes[recordIdx] = scores[k]->results_P1_total_time;
                    for (i = 0; i < 32; i++)
                        swrRace_saveData.record3LapNames[recordIdx][i] = swrRace_aProfiles[k].name[i];
                    swrRace_saveData.record3LapPilots[recordIdx] = swrRace_aProfiles[k].pilotId;
                }
                if (bestLap[k] < ELFSAVE_RECORD_TIME_EMPTY && bestLap[k] < swrRace_saveData.recordLapTimes[recordIdx]) {
                    swrRace_saveData.recordLapTimes[recordIdx] = bestLap[k];
                    for (i = 0; i < 32; i++)
                        swrRace_saveData.recordLapNames[recordIdx][i] = swrRace_aProfiles[k].name[i];
                    swrRace_saveData.recordLapPilots[recordIdx] = swrRace_aProfiles[k].pilotId;
                }
            }
        }
        swrRace_resultsStateFlags |= swrRace_RESULTSFLAG_RECORDS_COMMITTED;

        swrRace_resultsPlaceP1 = *(char*)&scores[0]->results_P1_Position;
        swrRace_resultsPlaceP2 = -1;
        minLocalPlace = swrRace_resultsPlaceP1;
        if (1 < hang->num_local_players) {
            swrRace_resultsPlaceP2 = *(char*)&scores[1]->results_P1_Position;
            if (swrRace_resultsPlaceP2 < swrRace_resultsPlaceP1)
                minLocalPlace = swrRace_resultsPlaceP2;
        }
        if (3 < minLocalPlace) {
            // start the list scrolled so the local player's row is visible
            swrRace_resultsScrollY = (float)(minLocalPlace - 3) * -30.0f;
            if (swrRace_resultsScrollY < 150.0f - (float)hang->num_players * 30.0f)
                swrRace_resultsScrollY = 150.0f - (float)hang->num_players * 30.0f;
        }
        if (hang->num_local_players == 1 && hang->isTournamentMode != 0 && 3 < hang->num_players) {
            // equal totals share a finishing position
            for (i = 1; i < hang->num_players; i++) {
                if (swrRace_resultsSortedScores[i]->results_P1_total_time == swrRace_resultsSortedScores[i - 1]->results_P1_total_time)
                    *(short*)&swrRace_resultsSortedScores[i]->results_P1_Position = *(short*)&swrRace_resultsSortedScores[i - 1]->results_P1_Position;
            }
            // winning a track can unlock its favorite pilot (cutaway state)
            if (swrRace_resultsPlaceP1 == 1 && hang->isTournamentMode != 0 && (swrRace_resultsStateFlags & swrRace_RESULTSFLAG_PILOT_UNLOCK_SHOWN) == 0) {
                favoritePilot = g_aTrackInfos[(int)hang->track_index].FavoritePilot;
                if (favoritePilot == 2 && hang->track_index != 1)
                    favoritePilot = 0;
                if (0 < (char)favoritePilot && (swrRace_aProfiles[0].pilotsUnlocked & (1u << (favoritePilot & 0x1f))) == 0) {
                    swrRace_aProfiles[0].pilotsUnlocked |= 1u << (favoritePilot & 0x1f);
                    swrRace_saveData.pilotsUnlockedGlobal |= swrRace_aProfiles[0].pilotsUnlocked;
                    swrRace_SaveCurrentProfile();
                    swrRace_resultsStateFlags |= swrRace_RESULTSFLAG_PILOT_UNLOCK_SHOWN;
                    swrObjHang_SetMenuState(hang, swrObjHang_STATE_RESULTS_INTRO);
                    return;
                }
            }
            required = swrObjHang_GetRequiredPlaceToProceed(hang->circuitIdx, (char)swrObjHang_trackInCircuitIdx);
            if (swrRace_resultsPlaceP1 <= required) {
                storedPlaceBits = (swrRace_aProfiles[0].beatTrackPlace[(int)hang->circuitIdx] >> ((swrObjHang_trackInCircuitIdx & 0xf) * 2)) & 3;
                if (swrObjHang_IsTrackUnlocked(hang->circuitIdx, (char)swrObjHang_trackInCircuitIdx) != 0) {
                    // prize scales with the circuit: x1.0 / x1.5 / x2.0 / x2.5
                    gain = (int)hang->winnings.truguts[hang->WinningsID - 1][swrRace_resultsPlaceP1 - 1];
                    gain = (int)((1.0 - (double)(int)hang->circuitIdx * -0.5) * (double)gain);
                    swrRace_TournamentTrugutGain = gain;
                    swrRace_truguts += gain;
                    if (hang->circuitIdx < 3) {
                        if ((int)swrObjHang_trackInCircuitIdx == g_aTracksInCircuits[(int)hang->circuitIdx] - 1 && (swrRace_aProfiles[0].circuitsCompleted & (1 << (hang->circuitIdx & 0x1f))) == 0) {
                            // circuit finished for the first time: jump to its invitational track
                            newCircuitIdx = 3;
                            swrRace_aProfiles[0].circuitsCompleted |= 1 << (hang->circuitIdx & 0x1f);
                            hang->track_index = (char)g_aTrackIDs[hang->circuitIdx + 0x15];
                        } else if ((int)swrObjHang_trackInCircuitIdx < g_aTracksInCircuits[(int)hang->circuitIdx] - 1 && (swrRace_aProfiles[0].tracksUnlocked[(int)hang->circuitIdx] & (1 << ((swrObjHang_trackInCircuitIdx + 1) & 0x1f))) == 0) {
                            hang->track_index = (char)g_aTrackIDs[hang->circuitIdx * 7 + swrObjHang_trackInCircuitIdx + 1];
                        }
                        swrRace_aProfiles[0].tracksUnlocked[(int)hang->circuitIdx] |= 1 << ((swrObjHang_trackInCircuitIdx + 1) & 0x1f);
                    }
                    swrRace_UpdatePartsHealth();
                }
                if ((int)swrObjHang_trackInCircuitIdx == g_aTracksInCircuits[(int)hang->circuitIdx] - 1 && hang->circuitIdx == 2)
                    swrObjHang_SelectCreditsTracks(hang);
                // best finishing place per track, 2 bits each (stored as 4 - place)
                if (storedPlaceBits < 4 - swrRace_resultsPlaceP1) {
                    swrRace_aProfiles[0].beatTrackPlace[(int)hang->circuitIdx] &= (uint16_t)~(3 << (swrObjHang_trackInCircuitIdx * 2));
                    swrRace_aProfiles[0].beatTrackPlace[(int)hang->circuitIdx] |= (uint16_t)((4 - swrRace_resultsPlaceP1) << (swrObjHang_trackInCircuitIdx * 2));
                    if (swrRace_aProfiles[0].beatTrackPlace[0] == 0x3fff && swrRace_aProfiles[0].beatTrackPlace[1] == 0x3fff && swrRace_aProfiles[0].beatTrackPlace[2] == 0x3fff) {
                        if ((swrRace_aProfiles[0].circuitsCompleted & 8) == 0) {
                            swrRace_aProfiles[0].circuitsCompleted |= 8;
                            newCircuitIdx = 3;
                            hang->track_index = (char)g_aTrackIDs[0x18];
                        } else if (swrRace_aProfiles[0].beatTrackPlace[3] == 0xff && (swrRace_saveData.unlockFlags & swrSaveData_UNLOCK_BEAT_ALL_TRACKS_FIRST) == 0) {
                            swrRace_saveData.unlockFlags |= swrSaveData_UNLOCK_BEAT_ALL_TRACKS_FIRST;
                        }
                    }
                }
            }
            // fold the profile's unlock progress into the machine-global unlocks
            // (UnlockDataBase[1..4] = tracksUnlocked[0..2] + circuitsCompleted)
            for (i = 0; i < 4; i++) {
                if (g_aBeatTracksGlobal[i] < (uint8_t)swrRace_UnlockDataBase[i + 1])
                    g_aBeatTracksGlobal[i] = (uint8_t)swrRace_UnlockDataBase[i + 1];
            }
        }
        hang->circuitIdx = newCircuitIdx;
        swrRace_SaveCurrentProfile();
    }

    if (0.0f < swrRace_resultsFanfareDelay) {
        swrRace_resultsFanfareDelay -= swrRace_fdeltaTimeSecs;
        if (swrRace_resultsFanfareDelay <= 0.0f)
            swrSound_Play2D(0xb6, 7, 0.25f, 1.0f, 0);
    }

    // standings rows (30px apart, faded near the top/bottom edges)
    rowY = 0x1e;
    for (i = 0; i < hang->num_players; i++) {
        score = swrRace_resultsSortedScores[i];
        rowYf = ((float)rowY + swrRace_resultsScrollY) + 15.0f;
        alphaF = 255.0f;
        if (rowYf < 45.0f)
            alphaF = (float)(255.0 - (45.0 - (double)rowYf) * 8.0);
        if (160.0f < rowYf)
            alphaF = (float)(255.0 - ((double)rowYf - 160.0) * 8.0);
        if (alphaF < 0.0f)
            alphaF = 0.0f;
        if (255.0f < alphaF)
            alphaF = 255.0f;
        alpha = (int)alphaF;
        // duplicate pods use the mirrored sprite bank (+0x17 per prior appearance)
        pilot = *score->pilotId;
        spriteId = pilot;
        for (k = 0; k < i; k++) {
            if (pilot == *swrRace_resultsSortedScores[k]->pilotId)
                spriteId += 0x17;
        }
        swrSprite_SetVisible((short)spriteId, 1);
        swrSprite_SetPos((short)spriteId, 0x1e, (short)(int)rowYf);
        swrSprite_SetDim((short)spriteId, 0.5f, 0.5f);
        swrSprite_SetColor((short)spriteId, 0xff, 0xff, 0xff, (uint8_t)alpha);
        rowYf += 10.0f;
        textY = (int)rowYf;
        if (i == swrRace_resultsPlaceP1 - 1 || i == swrRace_resultsPlaceP2 - 1) {
            // local player's row: highlighted, with the profile name underneath
            text = swrText_Translate("~r~s%d:");
            sprintf(buffer, text, (int)(short)score->results_P1_Position);
            swrText_CreateTextEntry1(0x58, textY, -0x5d, -0x42, 0x11, alpha, buffer);
            text = swrText_Translate("~f4~s%s %s");
            sprintf(buffer, text, swrText_Translate(swrRacer_PodData[pilot].name), swrText_Translate(swrRacer_PodData[pilot].lastname));
            swrText_CreateTextEntry1(0x5c, (int)(rowYf + 1.0f), -0x5d, -0x42, 0x11, alpha, buffer);
            swrText_CreateTimeEntryFormat(0x109, textY, score->results_P1_total_time, -0x5d, -0x42, 0x11, alpha, 1);
            text = swrText_Translate("~f4~s%s");
            sprintf(buffer, text, (i == swrRace_resultsPlaceP1 - 1) ? swrRace_aProfiles[0].name : swrRace_aProfiles[1].name);
            swrText_CreateTextEntry1(100, (int)(rowYf + 9.0f), -0x5d, -0x42, 0x11, alpha, buffer);
        } else {
            text = swrText_Translate("~r~s%d:");
            sprintf(buffer, text, (int)(short)score->results_P1_Position);
            swrText_CreateTextEntry1(0x58, textY, 0x32, -1, -1, alpha, buffer);
            text = swrText_Translate("~f4~s%s %s");
            sprintf(buffer, text, swrText_Translate(swrRacer_PodData[pilot].name), swrText_Translate(swrRacer_PodData[pilot].lastname));
            swrText_CreateTextEntry1(0x5c, (int)(rowYf + 1.0f), 0x32, -1, -1, alpha, buffer);
            swrText_CreateTimeEntryFormat(0x109, textY, score->results_P1_total_time, 0x32, -1, -1, alpha, 1);
        }
        rowY += 0x1e;
    }
    if (4 < hang->num_players)
        swrRace_DrawScrollbar(0x122, 0x1e, 0x90);
    if (0 < swrRace_TournamentTrugutGain) {
        text = swrText_Translate("/SCREENTEXT_575/~c~sYou won %d Truguts!");
        sprintf(buffer, text, swrRace_TournamentTrugutGain);
        swrText_CreateColorlessEntry1(0x87, 0xcd, buffer);
        swrObjHang_PositionHoloNode(0, 7.0f, -7.0f, 1.0f);
    }

    state2 = swrObjHang_state2;
    for (k = 0; k < hang->num_local_players; k++) {
        if (swrControl_acceptPressedEdge != 0 || swrControl_cancelPressedEdge != 0) {
            swrUI_RunCallbacks2(swrUI_GetUI1(), 1);
            swrSound_PlayUISound(0x54);
            if (hang->isTournamentMode == 0) {
                // multiplayer results return to the lobby flow (SELECT_PLANET - 11)
                state2 = swrMultiplayer_IsMultiplayerEnabled() != 0 ? (swrObjHang_STATE)(swrObjHang_STATE_SELECT_PLANET - 11) : swrObjHang_STATE_SELECT_PLANET;
                swrObjHang_state2 = state2;
            } else if (swrRace_resultsPlaceP1 < 4 && swrObjHang_trackInCircuitIdx == 6) {
                // circuit final won: podium ceremony with the top three pilots
                for (i = 0; i < 3; i++)
                    hang->podiumCharacters[i] = *(char*)swrRace_resultsSortedScores[i]->pilotId;
                state2 = swrObjHang_STATE_PLANET_SELECT_INTRO;
                swrObjHang_state2 = state2;
            } else {
                state2 = swrObjHang_STATE_SELECT_PLANET;
                swrObjHang_state2 = state2;
            }
        }
        g_bCircuitIdxInRange = 0;
        swrUI_menuScrolled = 0;
        if (4 < hang->num_players) {
            // analog scroll through the standings
            if (swrControl_aPlayerAxisY[k] < -0.1f || 0.1f < swrControl_aPlayerAxisY[k]) {
                swrRace_resultsScrollY -= swrControl_aPlayerAxisY[k] * swrRace_fdeltaTimeSecs * -300.0f;
                if (0.0f < swrRace_resultsScrollY)
                    swrRace_resultsScrollY = 0.0f;
                if (swrRace_resultsScrollY < 150.0f - (float)hang->num_players * 30.0f)
                    swrRace_resultsScrollY = 150.0f - (float)hang->num_players * 30.0f;
            }
            swrUI_menuScrolled = (uint32_t)(swrRace_resultsScrollY < 0.0f);
            if ((float)g_CircuitIdxMax < swrRace_resultsScrollY)
                g_bCircuitIdxInRange = 1;
        }
    }
    if (state2 != ~swrObjHang_STATE_LEGAL) {
        // a state transition is pending: reset the one-shot latches for the next results screen
        swrRace_resultsStateFlags = 0;
        swrObjHang_fadeState = -1;
    }
}

// 0x0041da00
void refined_swrMultiplayer_InRace(void)
{
    swrScore* score;
    rdMatrix44* transform;
    int* record;
    int numRacers;
    int slot;
    int col;
    int row;
    int lap;

    numRacers = 0;
    for (slot = 0; slot < SWR_MAX_RACERS; slot++) {
        if (swrScores[slot].identifier == 'Locl' || swrScores[slot].identifier == 'AAII')
            numRacers++;
    }
    if (numRacers == 0)
        return;

    ((int*)swrMultiplayer_message.data)[0] = numRacers;
    record = (int*)swrMultiplayer_message.data + 1;
    for (slot = 0; slot < SWR_MAX_RACERS; slot++) {
        score = &swrScores[slot];
        if (score->identifier != 'Locl' && score->identifier != 'AAII')
            continue;

        transform = &swrMultiplayer_aLocalPodTransform[slot];
        record[0] = slot;
        // the packet carries the matrix column by column
        for (col = 0; col < 4; col++) {
            for (row = 0; row < 4; row++)
                ((float*)record)[1 + col * 4 + row] = (&transform->vA.x)[row * 4 + col];
        }
        ((float*)record)[17] = swrMultiplayer_aRemoteSpeed[slot];
        ((float*)record)[18] = swrMultiplayer_aRemoteLapComp[slot];
        ((float*)record)[19] = swrMultiplayer_aRemoteLapCompMax[slot];
        record[20] = swrMultiplayer_aRemoteLap[slot];
        record[21] = (short)score->results_P1_Position;
        lap = score->results_P1_Lap;
        record[22] = lap;
        ((float*)record)[23] = (*race_timing_lap(score, lap, race_timing_laps()));
        ((float*)record)[24] = score->results_P1_total_time;
        record[25] = score->identifier;
        record[26] = score->obj_test_ptr->flags0 & swrObjTest_FLAG0_REMOTE_SYNCED;
        record[27] = score->obj_test_ptr->flags1 & swrObjTest_FLAG1_REMOTE_PUBLISHED;
        if (swrMultiplayer_aPodStatus[slot][0] != SWR_MP_POD_STATUS_PARKED) {
            swrMultiplayer_aPodStatus[slot][0] = record[21];
            swrMultiplayer_aPodStatus[slot][1] = record[22];
            swrMultiplayer_aPodStatus[slot][2] = record[23];
            swrMultiplayer_aPodStatus[slot][3] = record[24];
        }
        record += SWR_MP_POD_STATE_RECORD_INTS;
    }
    swrMultiplayer_message.dataSize = (char*)record - (char*)swrMultiplayer_message.data;
    swrMultiplayer_message.length = 0;
    swrMultiplayer_message.callbackId = swrMultiplayer_MSG_PLAYER_STATES;
    sithMessage_SendMessage(&swrMultiplayer_message, (DPID)-1, sithMessage_STREAM_NET, 0);
}
