#ifndef REFINED_RACER_OPTIONS_H
#define REFINED_RACER_OPTIONS_H

// Per-race options that widen a retail rule. All off = retail.
//
// full_lod_ai: AI pods load and animate like a local player's (full pod + rider model, engine
//   and connector animation at any distance). Retail gives them the low-detail model and stops
//   animating them past a distance.
// flamejet_local / flamejet_ai: any pilot's taunt fires the flame attack. Retail allows it for
//   Sebulba (pilot 2) only; the AI keeps its own speed / lap / opponent-ahead conditions.

#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RacerOptions {
    int full_lod_ai;
    int flamejet_local;
    int flamejet_ai;
} RacerOptions;

extern RacerOptions racer_options;

void refined_swrObjJdge_SpawnRacers(swrObjJdge* judge, swrScore* scores); // 0x004663e0
void refined_swrRace_PoddAnimateVariousThings(swrRace* player);          // 0x00471760
void refined_swrRace_UpdatePlayerControl(swrRace* player);               // 0x0046bec0
void refined_swrObjTest_F0(swrRace* player);                             // 0x0046d170

#ifdef __cplusplus
}
#endif

#endif
