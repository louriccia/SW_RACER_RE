#pragma once

// One finished race, frozen from the telemetry snapshot the moment the classification is final.
// Neutral between consumers: the results log writes it to disk, the championship table scores it.

#define RACE_RESULT_MAX_ROWS 20

struct RaceResultRow {
    int rank;     // 1-based final classification
    int pilot_id; // 0..22, -1 unknown
    char name[48];
    bool finished;// crossed the line (or was classified by the DNF cutoff)
    bool dead;
    int lap;             // laps completed
    float total_time_s;  // finishers only, else 0
    float gap_leader_s;  // behind the winner; 0 for the winner
    float gap_leader_laps;
};

struct RaceResult {
    int race_number;// the orchestrator's race counter (0 when unknown)
    int track_index;
    char track_name[64];
    int laps;
    int n;
    char finished_at[24];// local "YYYY-MM-DD HH:MM:SS"
    float winner_time_s;
    RaceResultRow rows[RACE_RESULT_MAX_ROWS];
};

// Freeze the live telemetry snapshot. False when no race is up or the field is empty.
bool race_result_Capture(int race_number, RaceResult *out);
