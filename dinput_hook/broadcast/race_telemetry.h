#pragma once

// Race telemetry snapshot: one read-only, per-frame normalized view of the live race, built from
// swrScoresPtr + swrRace so overlays, directors and exporters never read engine structs directly.
// Has no notion of "who the viewer is": a local human is just a flag on a row.

#define RACE_TELEMETRY_MAX_ROWS 20

enum RaceTelemetrySource {
    RACE_SOURCE_NONE = 0,
    RACE_SOURCE_SINGLE_PLAYER,// at least one local human
    RACE_SOURCE_MULTIPLAYER,
    RACE_SOURCE_ALL_AI,
};

struct RaceTelemetryRow {
    int slot;       // swrScoresPtr index
    int sprite_slot;// swrObj.id (overhead label / minimap slot)
    int pilot_id;   // 0..22, -1 unknown
    char name[48];  // MP player name, else character name (formatting codes stripped)
    bool is_local;
    bool is_ai;
    bool finished;
    int rank;             // 1-based; finished by total time, then the rest by progress
    int lap;              // 1-based current lap (num_laps once finished)
    float progress;       // laps completed + fraction (swrObjJdge_GetRacerProgress)
    float gap_leader_laps;// progress deficit to the leader (0 for the leader / finishers)
    float gap_leader_s;// seconds behind the leader: time gap for finishers, pace-derived otherwise
    float total_time_s;// finishers: final time; others: running race clock
    float speed;
    float max_speed;
    bool dead;
    bool on_fire;
    bool boosting;
    float engine_damage[6];// 0 clean .. 1 destroyed (left top/mid/bot, right top/mid/bot)
    bool engine_fire[6];
};

struct RaceTelemetry {
    bool valid;// a race is live (judge awake) and rows are populated
    RaceTelemetrySource source;
    int judge_state;// swrObjJdge_F0 nibble: 0 countdown, 1 racing, 2 all relevant finished, 3-6 post-race
    int track_index;
    int num_laps;
    int n;
    RaceTelemetryRow rows[RACE_TELEMETRY_MAX_ROWS];// sorted by rank
    float race_clock_s;                            // judge raceTimer while racing
    bool leader_finished;
    float leader_time_s;  // final time of the winner once in, else the race clock
    float leader_pace_lps;// smoothed leader progress rate, laps per second (0 until sampled)
};

// Rebuild the snapshot from the live game state. Call once per frame on the game thread.
void race_telemetry_Update();

// The most recent snapshot (valid == false outside a race).
const RaceTelemetry *race_telemetry_Get();
