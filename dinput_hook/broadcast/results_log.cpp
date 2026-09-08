#include "results_log.h"
#include "race_result.h"
#include "../config.h"
#include "../debug_ui.h"

#include <imgui.h>

#include <windows.h>
#include <cstdio>
#include <cstring>

static const char *INI_SECTION = "results_log";

static bool g_enabled = true;
static bool g_write_text = true;
static bool g_write_json = true;
static char g_dir[64] = "race_results";
static int g_races_written = 0;
static char g_last_error[160] = "";

const char *results_log_Directory() {
    return g_dir;
}

static FILE *open_log(const char *name, const char *mode) {
    CreateDirectoryA(g_dir, NULL);// ignore ERROR_ALREADY_EXISTS
    char path[256];
    snprintf(path, sizeof(path), "%s\\%s", g_dir, name);
    FILE *f = fopen(path, mode);
    if (f == NULL)
        snprintf(g_last_error, sizeof(g_last_error), "cannot write %s", path);
    return f;
}

static void format_time(float seconds, char *out, size_t n) {
    if (seconds < 0.0f)
        seconds = 0.0f;
    const int cs = (int) (seconds * 100.0f + 0.5f);
    snprintf(out, n, "%d:%02d.%02d", cs / 6000, (cs / 100) % 60, cs % 100);
}

static void write_text_block(const RaceResult *r) {
    FILE *f = open_log("races.txt", "a");
    if (f == NULL)
        return;
    fprintf(f, "================================================================\n");
    fprintf(f, "RACE %d  |  %s  |  %d lap%s  |  %s\n", r->race_number, r->track_name, r->laps,
            r->laps == 1 ? "" : "s", r->finished_at);
    fprintf(f, "----------------------------------------------------------------\n");
    for (int i = 0; i < r->n; i++) {
        const RaceResultRow &row = r->rows[i];
        char time[32];
        if (!row.finished)
            snprintf(time, sizeof(time), "DNF (lap %d)", row.lap);
        else if (i == 0)
            format_time(row.total_time_s, time, sizeof(time));
        else if (row.gap_leader_laps >= 1.0f)
            snprintf(time, sizeof(time), "+%d lap%s", (int) row.gap_leader_laps,
                     (int) row.gap_leader_laps == 1 ? "" : "s");
        else
            snprintf(time, sizeof(time), "+%.2f", row.gap_leader_s);
        fprintf(f, "%2d  %-24s %s\n", row.rank, row.name, time);
    }
    fprintf(f, "\n");
    fclose(f);
}

static void write_json_string(FILE *f, const char *s) {
    fputc('"', f);
    for (const char *p = s; *p; p++) {
        if (*p == '"' || *p == '\\')
            fprintf(f, "\\%c", *p);
        else if ((unsigned char) *p < 0x20)
            fprintf(f, "\\u%04x", (unsigned char) *p);
        else
            fputc(*p, f);
    }
    fputc('"', f);
}

// One JSON object per line (JSON Lines): appendable, and every line stands on its own for a
// consumer that tails the file.
static void write_json_line(const RaceResult *r) {
    FILE *f = open_log("races.jsonl", "a");
    if (f == NULL)
        return;
    fprintf(f, "{\"race\":%d,\"finished_at\":", r->race_number);
    write_json_string(f, r->finished_at);
    fprintf(f, ",\"track_index\":%d,\"track\":", r->track_index);
    write_json_string(f, r->track_name);
    fprintf(f, ",\"laps\":%d,\"winner_time_s\":%.3f,\"results\":[", r->laps, r->winner_time_s);
    for (int i = 0; i < r->n; i++) {
        const RaceResultRow &row = r->rows[i];
        fprintf(f, "%s{\"rank\":%d,\"pilot_id\":%d,\"name\":", i ? "," : "", row.rank, row.pilot_id);
        write_json_string(f, row.name);
        fprintf(f, ",\"finished\":%s,\"lap\":%d,\"time_s\":%.3f,\"gap_s\":%.3f,\"dead\":%s}",
                row.finished ? "true" : "false", row.lap, row.total_time_s, row.gap_leader_s,
                row.dead ? "true" : "false");
    }
    fprintf(f, "]}\n");
    fclose(f);
}

void results_log_Write(const RaceResult *result) {
    if (!g_enabled || result == NULL || result->n == 0)
        return;
    g_last_error[0] = '\0';
    if (g_write_text)
        write_text_block(result);
    if (g_write_json)
        write_json_line(result);
    g_races_written++;
}

// ---------------------------------------------------------------------------------------------
// Config + panel

static void load_config() {
    g_enabled = config::get_int(INI_SECTION, "enabled", g_enabled) != 0;
    g_write_text = config::get_int(INI_SECTION, "text", g_write_text) != 0;
    g_write_json = config::get_int(INI_SECTION, "json", g_write_json) != 0;
    const std::string dir = config::get_string(INI_SECTION, "directory", g_dir);
    if (!dir.empty())
        snprintf(g_dir, sizeof(g_dir), "%s", dir.c_str());
}

static void save_config() {
    config::set_int(INI_SECTION, "enabled", g_enabled);
    config::set_int(INI_SECTION, "text", g_write_text);
    config::set_int(INI_SECTION, "json", g_write_json);
    config::set_string(INI_SECTION, "directory", g_dir);
    config::save();
}

static void panel_results_log() {
    ImGui::TextWrapped("Every finished race is appended to a log next to the game executable, so a "
                       "race nobody watched can still be read back afterwards.");
    bool changed = false;
    changed |= ImGui::Checkbox("Write race results to disk", &g_enabled);
    changed |= ImGui::Checkbox("races.txt (readable table)", &g_write_text);
    changed |= ImGui::Checkbox("races.jsonl (one JSON object per race)", &g_write_json);
    changed |= ImGui::InputText("Directory", g_dir, sizeof(g_dir));
    if (changed)
        save_config();
    ImGui::Separator();
    ImGui::Text("%d race%s written this session", g_races_written, g_races_written == 1 ? "" : "s");
    if (g_last_error[0])
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s", g_last_error);
}

static DebugPanel g_panel = {.category = "Race",
                             .name = "Results log",
                             .draw = panel_results_log,
                             .dev_only = false};

void results_log_RegisterPanel() {
    load_config();
    debug_ui_register(&g_panel);
}
