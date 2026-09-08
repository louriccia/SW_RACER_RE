#pragma once

// Championship standings: points for the top finishers of every race, carried across a series and
// shown as a grand-prix summary at the end of each one. Keyed by pilot, so it works for any race
// the telemetry can classify (all-AI loop, single player, later multiplayer). Settings live in
// [standings] in SW_RACER_RE.ini; the table survives a restart in <results dir>\standings.csv.

#define STANDINGS_MAX_ENTRIES 23// one per pilot

struct RaceResult;

struct StandingsRow {
    int pilot_id;
    char name[48];
    int points;
    int last_points;// scored in the most recent race (0 = nothing)
    int races;
    int wins;
    int podiums;
    int best_rank;
};

// Score one race into the table and rewrite the standings files. No-op when disabled.
void standings_RecordRace(const RaceResult *result);

// Current table, sorted by points (then wins, then best finish). Never NULL.
const StandingsRow *standings_Rows(int *count);
int standings_RacesCounted();

// The leading `max` pilots of the championship, best placed first, written to `out` as pilot ids.
// Returns how many were written (0 when standings are off or nothing has been scored yet).
int standings_TopPilots(int max, int *out);
int standings_SeriesLength();// 0 = endless
void standings_Reset();

// Show / hide the summary card (the orchestrator opens it for the results window and closes it
// when the next race goes green).
void standings_ShowSummary(bool on);

// Inside the ImGui frame, after overlay_Draw.
void standings_Draw();

// Panel registration (after register_builtin_debug_panels).
void standings_RegisterPanel();
