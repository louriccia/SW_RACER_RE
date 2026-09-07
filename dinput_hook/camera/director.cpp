#include "director.h"
#include "../broadcast/race_telemetry.h"
#include "../broadcast/overlay.h"
#include "../debug_ui.h"
#include "../imgui_utils.h"// settings_ini_path
#include "../hook_helper.h"

#include <imgui.h>

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

extern "C" {
#include <Swr/swrObj.h>
#include <Swr/swrRace.h>
#include <Swr/swrEvent.h>
#include <globals.h>
}

// ---------------------------------------------------------------------------------------------
// Settings ([director])

static bool g_auto = true;
static float g_dwell_s = 12.0f;      // seconds on a target before auto considers a cut
static float g_manual_hold_s = 45.0f;// a manual pick holds this long before auto resumes
static int g_w_leader = 3, g_w_battle = 3, g_w_random = 1;// pick weights
static float g_battle_gap_s = 1.5f;                       // two racers this close are a "battle"

// ---------------------------------------------------------------------------------------------
// State

static bool g_enabled = false;
static int g_target_slot = -1;
static DWORD g_last_cut_ms = 0;
static DWORD g_manual_until_ms = 0;
static const char *g_last_rule = "";
static int g_cuts = 0;

static swrObjcMan *camera_man() {
    return (swrObjcMan *) swrEvent_FindObjectById('cMan', 0);
}

int director_FollowedSlot() {
    swrObjcMan *cman = camera_man();
    if (cman == NULL || cman->unkf4_objTest == NULL || swrScoresPtr == NULL)
        return -1;
    const RaceTelemetry *t = race_telemetry_Get();
    for (int k = 0; k < t->n; k++)
        if (swrScoresPtr[t->rows[k].slot].obj_test_ptr == cman->unkf4_objTest)
            return t->rows[k].slot;
    return -1;
}

static const RaceTelemetryRow *row_for_slot(const RaceTelemetry *t, int slot) {
    for (int k = 0; k < t->n; k++)
        if (t->rows[k].slot == slot)
            return &t->rows[k];
    return NULL;
}

// The cut: same payload swrObjHang_AssignRacerCameras sends at spawn ({'NAsn', camIndex, pod}).
static void cut_to(int slot, const char *rule) {
    swrObjcMan *cman = camera_man();
    if (cman == NULL || swrScoresPtr == NULL || slot < 0 || slot >= RACE_TELEMETRY_MAX_ROWS)
        return;
    swrRace *pod = swrScoresPtr[slot].obj_test_ptr;
    if (pod == NULL || pod == cman->unkf4_objTest)
        return;
    int ev[3] = {'NAsn', cman->metaCamIndex_count, (int) pod};
    swrEvent_DispatchSubEvents(cman, ev);
    g_target_slot = slot;
    g_last_cut_ms = GetTickCount();
    g_last_rule = rule;
    g_cuts++;
    const RaceTelemetryRow *r = row_for_slot(race_telemetry_Get(), slot);
    fprintf(hook_log, "[director] cut -> slot %d (%s) by %s\n", slot, r ? r->name : "?", rule);
    fflush(hook_log);
}

static bool followable(const RaceTelemetryRow &r) {
    return !r.finished && !r.dead;
}

// Auto pick among racers still racing: leader / closest battle (the chaser) / random, weighted.
static int pick_auto(const RaceTelemetry *t, const char **rule) {
    int leader = -1, battle = -1, count = 0;
    int pool[RACE_TELEMETRY_MAX_ROWS];
    float best_gap = 1e9f;
    int prev = -1;
    for (int k = 0; k < t->n; k++) {
        const RaceTelemetryRow &r = t->rows[k];
        if (!followable(r))
            continue;
        pool[count++] = r.slot;
        if (leader < 0)
            leader = r.slot;
        if (prev >= 0) {
            const RaceTelemetryRow *p = row_for_slot(t, prev);
            if (p != NULL && r.gap_leader_s >= 0.0f && p->gap_leader_s >= 0.0f) {
                const float gap = r.gap_leader_s - p->gap_leader_s;
                if (gap < best_gap && gap <= g_battle_gap_s) {
                    best_gap = gap;
                    battle = r.slot;// the chaser: the overtake attempt is in front of its camera
                }
            }
        }
        prev = r.slot;
    }
    if (count == 0)
        return -1;
    const int wl = leader >= 0 ? g_w_leader : 0;
    const int wb = battle >= 0 ? g_w_battle : 0;
    const int wr = count > 1 ? g_w_random : 0;
    const int total = wl + wb + wr;
    if (total == 0) {
        *rule = "leader";
        return leader;
    }
    int roll = rand() % total;
    if (roll < wl) {
        *rule = "leader";
        return leader;
    }
    roll -= wl;
    if (roll < wb) {
        *rule = "battle";
        return battle;
    }
    *rule = "random";
    // random: avoid re-picking the current target when there is a choice
    for (int tries = 0; tries < 8; tries++) {
        const int s = pool[rand() % count];
        if (s != g_target_slot || count == 1)
            return s;
    }
    return pool[0];
}

void director_Service() {
    if (!g_enabled)
        return;
    const RaceTelemetry *t = race_telemetry_Get();
    if (!t->valid || t->n == 0 || t->source != RACE_SOURCE_ALL_AI)
        return;
    if (t->judge_state == 0)// countdown: leave the assigned camera alone
        return;
    const DWORD now = GetTickCount();
    const int followed = director_FollowedSlot();
    if (followed != g_target_slot && followed >= 0)
        g_target_slot = followed;// something else (respawn, spawn) moved the camera

    const RaceTelemetryRow *cur = row_for_slot(t, g_target_slot);
    const bool manual_hold = now < g_manual_until_ms;
    const bool must_cut = cur == NULL || !followable(*cur);// finished / crashed: cut away now
    const bool dwell_over = now - g_last_cut_ms >= (DWORD) (g_dwell_s * 1000.0f);
    if (!must_cut && (manual_hold || !g_auto || !dwell_over))
        return;
    const char *rule = "";
    const int next = pick_auto(t, &rule);
    if (next >= 0 && next != g_target_slot)
        cut_to(next, must_cut ? "cut-away" : rule);
    else if (must_cut)
        g_last_cut_ms = now;// nothing better to show; re-check after a dwell
}

void director_FollowSlot(int slot) {
    if (slot < 0) {
        g_manual_until_ms = 0;
        return;
    }
    cut_to(slot, "manual");
    g_manual_until_ms = GetTickCount() + (DWORD) (g_manual_hold_s * 1000.0f);
}

void director_SetEnabled(bool on) {
    if (on == g_enabled)
        return;
    g_enabled = on;
    g_manual_until_ms = 0;
    overlay_SetRowClickHandler(on ? director_FollowSlot : NULL);
    if (!on)
        overlay_SetHighlightSlot(-1);
}

bool director_IsEnabled() {
    return g_enabled;
}

// ---------------------------------------------------------------------------------------------
// Config + panel

static const wchar_t *INI_SECTION = L"director";

static float ini_get_float(const wchar_t *ini, const wchar_t *key, float def) {
    wchar_t got[48], defbuf[48];
    swprintf(defbuf, 48, L"%.4f", def);
    GetPrivateProfileStringW(INI_SECTION, key, defbuf, got, 48, ini);
    return (float) wcstod(got, NULL);
}
static void ini_set_float(const wchar_t *ini, const wchar_t *key, float v) {
    wchar_t buf[48];
    swprintf(buf, 48, L"%.4f", v);
    WritePrivateProfileStringW(INI_SECTION, key, buf, ini);
}
static void ini_set_int(const wchar_t *ini, const wchar_t *key, int v) {
    wchar_t buf[16];
    swprintf(buf, 16, L"%d", v);
    WritePrivateProfileStringW(INI_SECTION, key, buf, ini);
}

static void load_config() {
    const wchar_t *ini = settings_ini_path();
    g_auto = GetPrivateProfileIntW(INI_SECTION, L"auto", g_auto, ini) != 0;
    g_dwell_s = ini_get_float(ini, L"dwell_s", g_dwell_s);
    g_manual_hold_s = ini_get_float(ini, L"manual_hold_s", g_manual_hold_s);
    g_w_leader = GetPrivateProfileIntW(INI_SECTION, L"w_leader", g_w_leader, ini);
    g_w_battle = GetPrivateProfileIntW(INI_SECTION, L"w_battle", g_w_battle, ini);
    g_w_random = GetPrivateProfileIntW(INI_SECTION, L"w_random", g_w_random, ini);
    g_battle_gap_s = ini_get_float(ini, L"battle_gap_s", g_battle_gap_s);
}

static void save_config() {
    const wchar_t *ini = settings_ini_path();
    ini_set_int(ini, L"auto", g_auto);
    ini_set_float(ini, L"dwell_s", g_dwell_s);
    ini_set_float(ini, L"manual_hold_s", g_manual_hold_s);
    ini_set_int(ini, L"w_leader", g_w_leader);
    ini_set_int(ini, L"w_battle", g_w_battle);
    ini_set_int(ini, L"w_random", g_w_random);
    ini_set_float(ini, L"battle_gap_s", g_battle_gap_s);
}

static void panel_director() {
    ImGui::TextWrapped("Picks which pod the camera follows in an all-AI race. Click a name on the "
                       "leaderboard to follow that racer; auto resumes after the hold.");
    ImGui::Text("Director: %s", g_enabled ? "enabled (by the orchestrator)" : "idle");
    bool changed = false;
    changed |= ImGui::Checkbox("Auto director", &g_auto);
    changed |= ImGui::SliderFloat("Dwell (s)", &g_dwell_s, 3.0f, 60.0f, "%.0f");
    changed |= ImGui::SliderFloat("Manual hold (s)", &g_manual_hold_s, 5.0f, 300.0f, "%.0f");
    changed |= ImGui::SliderInt("Weight: leader", &g_w_leader, 0, 10);
    changed |= ImGui::SliderInt("Weight: battle", &g_w_battle, 0, 10);
    changed |= ImGui::SliderInt("Weight: random", &g_w_random, 0, 10);
    changed |= ImGui::SliderFloat("Battle gap (s)", &g_battle_gap_s, 0.2f, 5.0f, "%.1f");
    if (changed)
        save_config();

    ImGui::Separator();
    const RaceTelemetry *t = race_telemetry_Get();
    const RaceTelemetryRow *cur = row_for_slot(t, director_FollowedSlot());
    ImGui::Text("Following: %s  (last cut by %s, %d cuts)", cur ? cur->name : "-", g_last_rule,
                g_cuts);
    ImGui::BeginDisabled(!g_enabled || !t->valid);
    if (ImGui::Button("Cut now")) {
        const char *rule = "";
        const int next = pick_auto(t, &rule);
        if (next >= 0)
            cut_to(next, rule);
    }
    ImGui::SameLine();
    if (ImGui::Button("Release manual hold"))
        g_manual_until_ms = 0;
    ImGui::EndDisabled();
}

static DebugPanel g_panel = {.category = "Camera",
                             .name = "Director",
                             .draw = panel_director,
                             .dev_only = false};

void director_RegisterPanel() {
    load_config();
    debug_ui_register(&g_panel);
}
