#include "ai_variance.h"
#include "broadcast/race_telemetry.h"
#include "debug_ui.h"
#include "imgui_utils.h"// settings_ini_path
#include "hook_helper.h"

#include <imgui.h>

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

extern "C" {
#include <Swr/swrObj.h>
#include <Swr/swrRace.h>
#include <globals.h>
void hook_function(const char *function_name, uint32_t original_address, uint8_t *hook_address);
}

// ---------------------------------------------------------------------------------------------
// Settings ([ai_variance])

static bool g_enabled = true;
static bool g_with_humans = false;    // also apply to AI in races a human drives
static bool g_replace_stock = true;   // ignore the stock rank-offset pacing (all AI start equal)
static float g_form_amp = 0.04f;      // per-race per-racer constant factor in [1-a, 1+a]
static float g_swing_amp = 0.05f;     // slow pace swings (Ornstein-Uhlenbeck), stddev
static float g_swing_tau_s = 8.0f;    // swing time constant
static float g_pack_gain = 0.03f;     // multiplier per second of deviation from the target spacing
static float g_pack_spread_s = 6.0f;  // target first-to-last spread of the field (s)
static float g_pack_clamp = 0.15f;    // max +/- from pack compression
static float g_blunder_per_min = 0.6f;// blunders per racer per minute
static float g_blunder_min_s = 1.5f, g_blunder_max_s = 3.0f;
static float g_blunder_depth = 0.65f;             // multiplier during a blunder
static float g_clamp_lo = 0.5f, g_clamp_hi = 1.6f;// same envelope as the stock AI

// ---------------------------------------------------------------------------------------------
// Per-race state (indexed by swrScoresPtr slot)

static const int MAX_SLOTS = 20;
static float g_form[MAX_SLOTS];
static float g_swing[MAX_SLOTS];
static DWORD g_blunder_until_ms[MAX_SLOTS];
static DWORD g_blunder_next_ms[MAX_SLOTS];
static float g_last_mult[MAX_SLOTS];
static int g_seeded_track = -2;
static int g_seeded_state0_seen = 0;
static int g_blunders_total = 0;

static float frand() {
    return (float) rand() / (float) RAND_MAX;
}

static float gauss() {// Box-Muller
    const float u1 = std::max(1e-6f, frand()), u2 = frand();
    return sqrtf(-2.0f * logf(u1)) * cosf(6.2831853f * u2);
}

static DWORD schedule_blunder(DWORD now) {
    if (g_blunder_per_min <= 0.0f)
        return 0;
    const float mean_s = 60.0f / g_blunder_per_min;
    const float wait = -logf(std::max(1e-6f, frand())) * mean_s;// exponential inter-arrival
    return now + (DWORD) (wait * 1000.0f);
}

static void reseed(DWORD now) {
    for (int i = 0; i < MAX_SLOTS; i++) {
        g_form[i] = 1.0f + (frand() * 2.0f - 1.0f) * g_form_amp;
        g_swing[i] = 0.0f;
        g_blunder_until_ms[i] = 0;
        g_blunder_next_ms[i] = schedule_blunder(now);
        g_last_mult[i] = 1.0f;
    }
    fprintf(hook_log, "[ai_variance] reseeded form:");
    for (int i = 0; i < MAX_SLOTS; i++)
        fprintf(hook_log, " %.3f", g_form[i]);
    fprintf(hook_log, "\n");
    fflush(hook_log);
}

// New race detection: the judge passes through state 0 (countdown) once per race.
static void maybe_reseed(const RaceTelemetry *t, DWORD now) {
    if (t->judge_state == 0) {
        if (!g_seeded_state0_seen) {
            g_seeded_state0_seen = 1;
            g_seeded_track = t->track_index;
            reseed(now);
        }
    } else {
        g_seeded_state0_seen = 0;
    }
}

static bool applies(const RaceTelemetry *t) {
    if (!g_enabled || !t->valid)
        return false;
    if (t->source == RACE_SOURCE_ALL_AI)
        return true;
    return g_with_humans && t->source == RACE_SOURCE_SINGLE_PLAYER;
}

typedef void(__cdecl *swrRace_UpdateCatchup_t)(swrRace *player);

static void __cdecl swrRace_UpdateCatchup_delta(swrRace *player) {
    hook_call_original((swrRace_UpdateCatchup_t) swrRace_UpdateCatchup_ADDR, player);
    if (player == NULL || (player->flags0 & swrObjTest_FLAG0_AI) == 0 ||
        (player->flags0 & swrObjTest_FLAG0_LOCAL) != 0)
        return;
    const RaceTelemetry *t = race_telemetry_Get();
    if (!applies(t))
        return;
    const DWORD now = GetTickCount();
    maybe_reseed(t, now);
    if (t->judge_state != 1 && t->judge_state != 2)
        return;
    if ((player->flags0 & swrObjTest_FLAG0_STATE_MASK) != swrObjTest_FLAG0_RACING ||
        (player->flags1 & swrObjTest_FLAG1_FINISHED) != 0)
        return;
    if (player->score_ptr == NULL || swrScoresPtr == NULL)
        return;
    const int slot = (int) (player->score_ptr - swrScoresPtr);
    if (slot < 0 || slot >= MAX_SLOTS)
        return;

    const float dt = (float) swrRace_deltaTimeSecs;

    // slow swing: dx = -x/tau dt + sigma*sqrt(2 dt/tau) dW
    if (g_swing_tau_s > 0.0f && dt > 0.0f) {
        const float k = dt / g_swing_tau_s;
        g_swing[slot] += -g_swing[slot] * k + g_swing_amp * sqrtf(2.0f * k) * gauss();
        g_swing[slot] = std::clamp(g_swing[slot], -3.0f * g_swing_amp, 3.0f * g_swing_amp);
    }

    // pack compression: pull each racer toward its share of the target spread
    float pack = 1.0f;
    const RaceTelemetryRow *row = NULL;
    int racing_rank = 0, racing_n = 0;
    for (int k = 0; k < t->n; k++) {
        if (t->rows[k].finished)
            continue;
        if (t->rows[k].slot == slot) {
            row = &t->rows[k];
            racing_rank = racing_n;
        }
        racing_n++;
    }
    if (row != NULL && row->gap_leader_s >= 0.0f && racing_n > 1) {
        const float target = g_pack_spread_s * (float) racing_rank / (float) (racing_n - 1);
        const float err = row->gap_leader_s - target;// positive = further back than wanted
        pack = std::clamp(1.0f + g_pack_gain * err, 1.0f - g_pack_clamp, 1.0f + g_pack_clamp);
    }

    // blunders
    float blunder = 1.0f;
    if (g_blunder_until_ms[slot] != 0 && now < g_blunder_until_ms[slot]) {
        blunder = g_blunder_depth;
    } else {
        if (g_blunder_until_ms[slot] != 0) {
            g_blunder_until_ms[slot] = 0;
            g_blunder_next_ms[slot] = schedule_blunder(now);
        }
        if (g_blunder_next_ms[slot] != 0 && now >= g_blunder_next_ms[slot]) {
            const float dur =
                g_blunder_min_s + frand() * std::max(0.0f, g_blunder_max_s - g_blunder_min_s);
            g_blunder_until_ms[slot] = now + (DWORD) (dur * 1000.0f);
            g_blunders_total++;
            fprintf(hook_log, "[ai_variance] slot %d (%s) blunder %.1fs\n", slot,
                    row ? row->name : "?", dur);
            fflush(hook_log);
            blunder = g_blunder_depth;
        }
    }

    const float ours = g_form[slot] * (1.0f + g_swing[slot]) * pack * blunder;
    const float base = g_replace_stock ? 1.0f : player->speedMultiplier;
    player->speedMultiplier = std::clamp(base * ours, g_clamp_lo, g_clamp_hi);
    g_last_mult[slot] = player->speedMultiplier;
}

void ai_variance_RegisterHooks() {
    hook_function("swrRace_UpdateCatchup", (uint32_t) swrRace_UpdateCatchup_ADDR,
                  (uint8_t *) swrRace_UpdateCatchup_delta);
}

// ---------------------------------------------------------------------------------------------
// Config + panel

static const wchar_t *INI_SECTION = L"ai_variance";

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
    g_enabled = GetPrivateProfileIntW(INI_SECTION, L"enabled", g_enabled, ini) != 0;
    g_with_humans = GetPrivateProfileIntW(INI_SECTION, L"with_humans", g_with_humans, ini) != 0;
    g_replace_stock =
        GetPrivateProfileIntW(INI_SECTION, L"replace_stock", g_replace_stock, ini) != 0;
    g_form_amp = ini_get_float(ini, L"form_amp", g_form_amp);
    g_swing_amp = ini_get_float(ini, L"swing_amp", g_swing_amp);
    g_swing_tau_s = ini_get_float(ini, L"swing_tau_s", g_swing_tau_s);
    g_pack_gain = ini_get_float(ini, L"pack_gain", g_pack_gain);
    g_pack_spread_s = ini_get_float(ini, L"pack_spread_s", g_pack_spread_s);
    g_pack_clamp = ini_get_float(ini, L"pack_clamp", g_pack_clamp);
    g_blunder_per_min = ini_get_float(ini, L"blunder_per_min", g_blunder_per_min);
    g_blunder_min_s = ini_get_float(ini, L"blunder_min_s", g_blunder_min_s);
    g_blunder_max_s = ini_get_float(ini, L"blunder_max_s", g_blunder_max_s);
    g_blunder_depth = ini_get_float(ini, L"blunder_depth", g_blunder_depth);
}

static void save_config() {
    const wchar_t *ini = settings_ini_path();
    ini_set_int(ini, L"enabled", g_enabled);
    ini_set_int(ini, L"with_humans", g_with_humans);
    ini_set_int(ini, L"replace_stock", g_replace_stock);
    ini_set_float(ini, L"form_amp", g_form_amp);
    ini_set_float(ini, L"swing_amp", g_swing_amp);
    ini_set_float(ini, L"swing_tau_s", g_swing_tau_s);
    ini_set_float(ini, L"pack_gain", g_pack_gain);
    ini_set_float(ini, L"pack_spread_s", g_pack_spread_s);
    ini_set_float(ini, L"pack_clamp", g_pack_clamp);
    ini_set_float(ini, L"blunder_per_min", g_blunder_per_min);
    ini_set_float(ini, L"blunder_min_s", g_blunder_min_s);
    ini_set_float(ini, L"blunder_max_s", g_blunder_max_s);
    ini_set_float(ini, L"blunder_depth", g_blunder_depth);
}

static void panel_ai_variance() {
    ImGui::TextWrapped(
        "Closer, less predictable AI races: per-race form, slow pace swings, pack "
        "compression toward a target spread, and random blunders. Speed only; steering "
        "is untouched. Applies to all-AI races (optionally to the AI in yours).");
    bool changed = false;
    changed |= ImGui::Checkbox("Enabled", &g_enabled);
    changed |= ImGui::Checkbox("Also in races a human drives", &g_with_humans);
    changed |= ImGui::Checkbox("Replace stock pacing (all AI start equal)", &g_replace_stock);
    ImGui::SeparatorText("Form + swings");
    changed |= ImGui::SliderFloat("Form amplitude (per race)", &g_form_amp, 0.0f, 0.15f, "%.3f");
    changed |= ImGui::SliderFloat("Swing amplitude", &g_swing_amp, 0.0f, 0.15f, "%.3f");
    changed |= ImGui::SliderFloat("Swing time constant (s)", &g_swing_tau_s, 1.0f, 30.0f, "%.0f");
    ImGui::SeparatorText("Pack compression");
    changed |= ImGui::SliderFloat("Target field spread (s)", &g_pack_spread_s, 0.0f, 30.0f, "%.1f");
    changed |= ImGui::SliderFloat("Gain (x per s of error)", &g_pack_gain, 0.0f, 0.1f, "%.3f");
    changed |= ImGui::SliderFloat("Clamp (+/-)", &g_pack_clamp, 0.0f, 0.5f, "%.2f");
    ImGui::SeparatorText("Blunders");
    changed |= ImGui::SliderFloat("Per racer per minute", &g_blunder_per_min, 0.0f, 4.0f, "%.2f");
    changed |= ImGui::SliderFloat("Min duration (s)", &g_blunder_min_s, 0.5f, 5.0f, "%.1f");
    changed |= ImGui::SliderFloat("Max duration (s)", &g_blunder_max_s, 0.5f, 8.0f, "%.1f");
    changed |= ImGui::SliderFloat("Depth (multiplier)", &g_blunder_depth, 0.2f, 1.0f, "%.2f");
    if (changed)
        save_config();

    ImGui::Separator();
    ImGui::Text("Blunders so far: %d", g_blunders_total);
    if (ImGui::Button("Reseed form now"))
        reseed(GetTickCount());
    const RaceTelemetry *t = race_telemetry_Get();
    if (t->valid && ImGui::BeginTable("var", 5, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Racer");
        ImGui::TableSetupColumn("form");
        ImGui::TableSetupColumn("swing");
        ImGui::TableSetupColumn("mult");
        ImGui::TableSetupColumn("gap s");
        ImGui::TableHeadersRow();
        for (int k = 0; k < t->n; k++) {
            const RaceTelemetryRow &r = t->rows[k];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.name);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", g_form[r.slot]);
            ImGui::TableNextColumn();
            ImGui::Text("%+.3f", g_swing[r.slot]);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", g_last_mult[r.slot]);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", r.gap_leader_s);
        }
        ImGui::EndTable();
    }
}

static DebugPanel g_panel = {.category = "Race",
                             .name = "AI Variance",
                             .draw = panel_ai_variance,
                             .dev_only = false};

void ai_variance_RegisterPanel() {
    load_config();
    srand((unsigned) GetTickCount());
    debug_ui_register(&g_panel);
}
