#include "race_timing.h"

#include <globals.h>

#define RACE_TIMING_RACERS 20 // swrScores

static float later_laps[RACE_TIMING_RACERS][RACE_TIMING_MAX_LAPS - RACE_TIMING_SCORE_SLOTS];
static float overflow_lap; // laps past RACE_TIMING_MAX_LAPS all land here
static float time_cap = 3000.0f;

static int racer_of(swrScore* score)
{
    int i = (int)(score - &swrScores[0]);
    if (i >= 0 && i < RACE_TIMING_RACERS)
        return i;
    i = (int)(score - swrScoresPtr);
    if (swrScoresPtr != NULL && i >= 0 && i < RACE_TIMING_RACERS)
        return i;
    return RACE_TIMING_RACERS - 1;
}

static int race_laps = 3;

float* race_timing_lap(swrScore* score, int lap, int num_laps)
{
    if (num_laps <= RACE_TIMING_SCORE_SLOTS || (lap >= 0 && lap < RACE_TIMING_SCORE_SLOTS))
        return &score->results_P1_Lap1 + lap;
    if (lap < 0 || lap >= RACE_TIMING_MAX_LAPS)
        return &overflow_lap;
    return &later_laps[racer_of(score)][lap - RACE_TIMING_SCORE_SLOTS];
}

void race_timing_track(swrScore* score, int num_laps)
{
    race_laps = num_laps;
    if (num_laps > RACE_TIMING_SCORE_SLOTS && score->results_P1_Lap == 0) {
        float* row = later_laps[racer_of(score)];
        for (int i = 0; i < RACE_TIMING_MAX_LAPS - RACE_TIMING_SCORE_SLOTS; i++)
            row[i] = 0.0f;
    }
}

int race_timing_laps(void)
{
    return race_laps;
}

float race_timing_time_cap(void)
{
    return time_cap;
}

void race_timing_set_time_cap(float seconds)
{
    time_cap = seconds > 0.0f ? seconds : 3000.0f;
}
