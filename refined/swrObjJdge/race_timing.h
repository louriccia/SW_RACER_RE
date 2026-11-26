#ifndef REFINED_RACE_TIMING_H
#define REFINED_RACE_TIMING_H

// Race timing: each racer's per-lap times and the race-time clamp.
//
// Retail keeps lap times in five floats of swrScore (results_P1_Lap1..Lap5), indexed by the lap
// number, so a race of more than five laps writes over the rest of the score. Here laps 0..4
// still live in those five slots (everything that reads them directly sees retail values) and
// later laps live in a per-racer store. For any race of five laps or fewer the refined functions
// below behave exactly like the faithful ones.

#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RACE_TIMING_SCORE_SLOTS 5 // swrScore::results_P1_Lap1..Lap5
#define RACE_TIMING_MAX_LAPS 256

// The time of lap `lap` (0-based) of `score`'s racer, in a race of `num_laps` laps. With five
// laps or fewer this is exactly retail's address, (&score->results_P1_Lap1)[lap], even past the
// five slots (after the last lap the HUD reads one past it, landing on total_time).
float* race_timing_lap(swrScore* score, int lap, int num_laps);

// Called by the judge for each racing score every tick: remembers the lap count (for callers
// without a judge at hand) and clears the racer's later laps while it is still on lap 0.
void race_timing_track(swrScore* score, int num_laps);
int race_timing_laps(void);

// Race and lap times clamp at this many seconds. Default 3000 (50:00), as retail.
float race_timing_time_cap(void);
void race_timing_set_time_cap(float seconds);

// The faithful functions that read or write per-lap times, through race_timing_lap().
void refined_swrObjJdge_F2(swrObjJdge* jdge);                                   // 0x0045ea30
void refined_swrRace_InRaceTimer(swrScore* score, swrObjJdge* jdge);            // 0x00460950
void refined_swrRace_InRaceEndStatistics(swrObjJdge* jdge, swrScore* score);    // 0x00462320
void refined_swrObjHang_ResultsMenu(swrObjHang* hang);                          // 0x00439ce0
void refined_swrMultiplayer_InRace(void);                                       // 0x0041da00

#ifdef __cplusplus
}
#endif

#endif
