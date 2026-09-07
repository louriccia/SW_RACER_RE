#include "race_telemetry.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

extern "C" {
#include <Swr/swrObj.h>
#include <Swr/swrRace.h>
#include <Swr/swrEvent.h>
#include <Swr/swrMultiplayer.h>
#include <Swr/swrText.h>
#include <globals.h>
}

static RaceTelemetry g_t;

// Leader pace: sampled once a second and smoothed, so progress deficits can be shown as seconds.
static float g_pace_prev_prog = -1.0f;
static DWORD g_pace_prev_ms = 0;
static float g_pace = 0.0f;
static const swrObjJdge *g_pace_jdge = NULL;// reset the estimator when a new race starts

typedef char *(__cdecl *swrMultiplayer_GetPlayerNameAscii_t)(int playerIndex);

static void strip_codes(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < n; p++) {
        if (*p == '~' && p[1]) {
            p++;// skip "~x" formatting codes
            continue;
        }
        out[o++] = *p;
    }
    out[o] = '\0';
}

static void resolve_name(const swrScore *score, int slot, char *out, size_t n) {
    char raw[128] = {0};
    if (multiplayer_enabled != 0) {
        // MP: swrScoresPtr index == player number; the name buffer is static, copy at once.
        const char *pn =
            ((swrMultiplayer_GetPlayerNameAscii_t) swrMultiplayer_GetPlayerNameAscii_ADDR)(slot);
        if (pn != NULL && pn[0] != '\0') {
            strip_codes(pn, out, n);
            return;
        }
    }
    if (score->pilotId != NULL && *score->pilotId >= 0 && *score->pilotId < 23)
        swrText_FormatPodName(*score->pilotId, raw, sizeof(raw));
    strip_codes(raw, out, n);
}

static float leader_progress(const swrObjJdge *jdge) {
    float best = 0.0f;
    for (int i = 0; i < jdge->num_players && i < RACE_TELEMETRY_MAX_ROWS; i++) {
        swrScore *score = &swrScoresPtr[i];
        if (score->obj_test_ptr == NULL)
            continue;
        const float p =
            (score->flag & 2) ? (float) jdge->num_laps : swrObjJdge_GetRacerProgress(score);
        best = std::max(best, p);
    }
    return best;
}

static void update_pace(const swrObjJdge *jdge, DWORD now) {
    if (jdge != g_pace_jdge || (jdge->flag & 0xf) == 0) {
        g_pace_jdge = jdge;
        g_pace_prev_prog = -1.0f;
        g_pace_prev_ms = 0;
        g_pace = 0.0f;
    }
    const float prog = leader_progress(jdge);
    if (g_pace_prev_ms == 0) {
        g_pace_prev_prog = prog;
        g_pace_prev_ms = now;
        return;
    }
    if (now - g_pace_prev_ms < 1000)
        return;
    const float rate = (prog - g_pace_prev_prog) / ((now - g_pace_prev_ms) / 1000.0f);
    if (rate > 0.0f)
        g_pace = g_pace > 0.0f ? g_pace * 0.7f + rate * 0.3f : rate;
    g_pace_prev_prog = prog;
    g_pace_prev_ms = now;
}

void race_telemetry_Update() {
    g_t.valid = false;
    g_t.n = 0;
    swrObjJdge *jdge = (swrObjJdge *) swrEvent_GetItem('Jdge', 0);
    if (jdge == NULL || swrScoresPtr == NULL)
        return;
    const bool asleep = (((const uint8_t *) &jdge->obj.flags)[1] & 0x10) != 0;
    if (asleep || swrJdge_Cleared != 0)
        return;

    const DWORD now = GetTickCount();
    update_pace(jdge, now);

    g_t.judge_state = jdge->flag & 0xf;
    g_t.num_laps = jdge->num_laps;
    g_t.race_clock_s = jdge->raceTimer_ms;
    g_t.leader_pace_lps = g_pace;
    swrObjHang *hang = (swrObjHang *) swrEvent_GetItem('Hang', 0);
    g_t.track_index = hang != NULL ? hang->track_index : -1;

    int n = 0;
    bool any_local = false;
    for (int i = 0; i < jdge->num_players && i < RACE_TELEMETRY_MAX_ROWS; i++) {
        swrScore *score = &swrScoresPtr[i];
        swrRace *pod = score->obj_test_ptr;
        if (pod == NULL)
            continue;
        RaceTelemetryRow &r = g_t.rows[n++];
        memset(&r, 0, sizeof(r));
        r.slot = i;
        r.sprite_slot = pod->obj.id;
        r.pilot_id = score->pilotId != NULL ? *score->pilotId : -1;
        resolve_name(score, i, r.name, sizeof(r.name));
        r.is_local = (pod->flags0 & swrObjTest_FLAG0_LOCAL) != 0;
        r.is_ai = (pod->flags0 & swrObjTest_FLAG0_AI) != 0;
        r.finished = (score->flag & 2) != 0;
        r.progress = r.finished ? (float) jdge->num_laps : swrObjJdge_GetRacerProgress(score);
        r.lap = std::min(jdge->num_laps, (int) r.progress + 1);
        r.total_time_s = r.finished ? score->results_P1_total_time : jdge->raceTimer_ms;
        r.speed = pod->speedValue;
        r.dead = (pod->flags0 & swrObjTest_FLAG0_DEAD) != 0;
        for (int e = 0; e < 6; e++)
            if ((pod->engineStatus[e] & 8) != 0)
                r.on_fire = true;
        any_local |= r.is_local;
    }
    g_t.n = n;
    if (n == 0)
        return;

    std::sort(g_t.rows, g_t.rows + n, [](const RaceTelemetryRow &a, const RaceTelemetryRow &b) {
        if (a.finished != b.finished)
            return a.finished;
        if (a.finished)
            return a.total_time_s < b.total_time_s;
        return a.progress > b.progress;
    });

    const float lead_prog = leader_progress(jdge);
    g_t.leader_finished = g_t.rows[0].finished;
    g_t.leader_time_s = g_t.leader_finished ? g_t.rows[0].total_time_s : jdge->raceTimer_ms;
    for (int k = 0; k < n; k++) {
        RaceTelemetryRow &r = g_t.rows[k];
        r.rank = k + 1;
        if (r.finished) {
            r.gap_leader_laps = 0.0f;
            r.gap_leader_s = r.total_time_s - g_t.leader_time_s;
        } else {
            r.gap_leader_laps = std::max(0.0f, lead_prog - r.progress);
            r.gap_leader_s = g_pace > 0.0f ? r.gap_leader_laps / g_pace : -1.0f;
        }
    }

    g_t.source = multiplayer_enabled != 0 ? RACE_SOURCE_MULTIPLAYER
                 : any_local              ? RACE_SOURCE_SINGLE_PLAYER
                                          : RACE_SOURCE_ALL_AI;
    g_t.valid = true;
}

const RaceTelemetry *race_telemetry_Get() {
    return &g_t;
}
