#include "race_result.h"
#include "race_telemetry.h"
#include "../game_deltas/tracks_delta.h"// swrUI_GetTrackNameFromId_delta

#include <windows.h>
#include <cstdio>
#include <cstring>

static void copy_track_name(int track_index, char *out, size_t n) {
    const char *raw = track_index >= 0 ? swrUI_GetTrackNameFromId_delta(track_index) : NULL;
    size_t o = 0;
    for (const char *p = raw ? raw : ""; *p && o + 1 < n; p++) {
        if (*p == '~' && p[1]) {// formatting code (size / colour), not text
            p++;
            continue;
        }
        out[o++] = *p;
    }
    out[o] = '\0';
}

bool race_result_Capture(int race_number, RaceResult *out) {
    const RaceTelemetry *t = race_telemetry_Get();
    if (out == NULL || !t->valid || t->n == 0)
        return false;

    memset(out, 0, sizeof(*out));
    out->race_number = race_number;
    out->track_index = t->track_index;
    copy_track_name(t->track_index, out->track_name, sizeof(out->track_name));
    out->laps = t->num_laps;
    out->winner_time_s = t->leader_time_s;

    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(out->finished_at, sizeof(out->finished_at), "%04u-%02u-%02u %02u:%02u:%02u", st.wYear,
             st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    for (int k = 0; k < t->n && k < RACE_RESULT_MAX_ROWS; k++) {
        const RaceTelemetryRow &r = t->rows[k];
        RaceResultRow &d = out->rows[out->n++];
        d.rank = r.rank;
        d.pilot_id = r.pilot_id;
        snprintf(d.name, sizeof(d.name), "%s", r.name);
        d.finished = r.finished;
        d.dead = r.dead;
        d.lap = r.lap;
        d.total_time_s = r.finished ? r.total_time_s : 0.0f;
        d.gap_leader_s = k == 0 ? 0.0f : r.gap_leader_s;
        d.gap_leader_laps = r.gap_leader_laps;
    }
    return out->n > 0;
}
