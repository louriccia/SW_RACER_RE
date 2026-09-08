#include "standings.h"
#include "race_result.h"
#include "results_log.h"
#include "overlay.h"
#include "../config.h"
#include "../debug_ui.h"

#include <imgui.h>

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static const char *INI_SECTION = "standings";

// ---------------------------------------------------------------------------------------------
// Settings

static bool g_enabled = true;
static char g_points_spec[96] = "10,8,6,4,2";// points for P1, P2, ...; past the list, nothing
static bool g_score_dnf = false;             // a classified DNF can still take points
static int g_series_races = 0;               // races per championship; 0 = endless
static int g_display_rows = 10;              // rows on the summary card
static bool g_summary_card = true;
static float g_margin_x = 10.0f;
static float g_margin_y = 60.0f;
static int g_anchor = 1;// 0 left, 1 right

static int g_points[STANDINGS_MAX_ENTRIES];
static int g_points_count = 0;

static void parse_points() {
    g_points_count = 0;
    for (const char *p = g_points_spec; *p && g_points_count < STANDINGS_MAX_ENTRIES;) {
        while (*p == ' ' || *p == ',')
            p++;
        if (*p == '\0')
            break;
        char *end = NULL;
        const long v = strtol(p, &end, 10);
        if (end == p)
            break;
        g_points[g_points_count++] = (int) v;
        p = end;
    }
}

static int points_for_rank(int rank) {
    return rank >= 1 && rank <= g_points_count ? g_points[rank - 1] : 0;
}

// ---------------------------------------------------------------------------------------------
// Table

static StandingsRow g_rows[STANDINGS_MAX_ENTRIES];
static int g_count = 0;
static int g_races_counted = 0;
static int g_series = 1;
static bool g_series_complete = false;// the last recorded race closed the series
static bool g_summary_visible = false;

const StandingsRow *standings_Rows(int *count) {
    if (count != NULL)
        *count = g_count;
    return g_rows;
}
int standings_RacesCounted() {
    return g_races_counted;
}
int standings_SeriesLength() {
    return g_series_races;
}
void standings_ShowSummary(bool on) {
    g_summary_visible = on && g_enabled && g_summary_card;
}

static StandingsRow *find_or_add(int pilot_id, const char *name) {
    for (int i = 0; i < g_count; i++) {
        const bool same =
            pilot_id >= 0 ? g_rows[i].pilot_id == pilot_id : strcmp(g_rows[i].name, name) == 0;
        if (same)
            return &g_rows[i];
    }
    if (g_count >= STANDINGS_MAX_ENTRIES)
        return NULL;
    StandingsRow *row = &g_rows[g_count++];
    memset(row, 0, sizeof(*row));
    row->pilot_id = pilot_id;
    return row;
}

static void sort_rows() {
    std::stable_sort(g_rows, g_rows + g_count, [](const StandingsRow &a, const StandingsRow &b) {
        if (a.points != b.points)
            return a.points > b.points;
        if (a.wins != b.wins)
            return a.wins > b.wins;
        if (a.podiums != b.podiums)
            return a.podiums > b.podiums;
        const int ba = a.best_rank > 0 ? a.best_rank : 99;
        const int bb = b.best_rank > 0 ? b.best_rank : 99;
        return ba < bb;
    });
}

// ---------------------------------------------------------------------------------------------
// Persistence: the table survives a crash / restart mid-party.

static void standings_path(char *out, size_t n, const char *name) {
    snprintf(out, n, "%s\\%s", results_log_Directory(), name);
}

static void write_files() {
    CreateDirectoryA(results_log_Directory(), NULL);// ignore ERROR_ALREADY_EXISTS
    char path[256];

    standings_path(path, sizeof(path), "standings.csv");
    FILE *f = fopen(path, "w");
    if (f != NULL) {
        fprintf(f, "# pilot_id,points,races,wins,podiums,best_rank,name\n");
        fprintf(f, "series=%d\nraces=%d\n", g_series, g_races_counted);
        for (int i = 0; i < g_count; i++) {
            const StandingsRow &r = g_rows[i];
            fprintf(f, "%d,%d,%d,%d,%d,%d,%s\n", r.pilot_id, r.points, r.races, r.wins, r.podiums,
                    r.best_rank, r.name);
        }
        fclose(f);
    }

    // Plain text for a stream overlay, or anyone reading the folder.
    standings_path(path, sizeof(path), "standings.txt");
    f = fopen(path, "w");
    if (f != NULL) {
        if (g_series_races > 0)
            fprintf(f, "CHAMPIONSHIP %d -- after %d of %d races\n\n", g_series, g_races_counted,
                    g_series_races);
        else
            fprintf(f, "CHAMPIONSHIP STANDINGS -- after %d race%s\n\n", g_races_counted,
                    g_races_counted == 1 ? "" : "s");
        for (int i = 0; i < g_count; i++)
            fprintf(f, "%2d  %-24s %4d\n", i + 1, g_rows[i].name, g_rows[i].points);
        fclose(f);
    }
}

static void load_table() {
    char path[256];
    standings_path(path, sizeof(path), "standings.csv");
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return;
    g_count = 0;
    char line[256];
    while (fgets(line, sizeof(line), f) != NULL) {
        if (line[0] == '#')
            continue;
        if (sscanf(line, "series=%d", &g_series) == 1)
            continue;
        if (sscanf(line, "races=%d", &g_races_counted) == 1)
            continue;
        StandingsRow r;
        memset(&r, 0, sizeof(r));
        if (sscanf(line, "%d,%d,%d,%d,%d,%d,%47[^\r\n]", &r.pilot_id, &r.points, &r.races, &r.wins,
                   &r.podiums, &r.best_rank, r.name) == 7 &&
            g_count < STANDINGS_MAX_ENTRIES)
            g_rows[g_count++] = r;
    }
    fclose(f);
    sort_rows();
}

void standings_Reset() {
    g_count = 0;
    g_races_counted = 0;
    g_series_complete = false;
    g_summary_visible = false;
    memset(g_rows, 0, sizeof(g_rows));
    write_files();
}

// ---------------------------------------------------------------------------------------------

void standings_RecordRace(const RaceResult *result) {
    if (!g_enabled || result == NULL || result->n == 0)
        return;
    // The race that closed the previous championship has had its summary; start the new one clean.
    if (g_series_complete) {
        g_series++;
        g_count = 0;
        g_races_counted = 0;
        g_series_complete = false;
        memset(g_rows, 0, sizeof(g_rows));
    }

    for (int i = 0; i < g_count; i++)
        g_rows[i].last_points = 0;

    for (int i = 0; i < result->n; i++) {
        const RaceResultRow &src = result->rows[i];
        StandingsRow *row = find_or_add(src.pilot_id, src.name);
        if (row == NULL)
            continue;
        snprintf(row->name, sizeof(row->name), "%s", src.name);
        row->races++;
        if (!src.finished && !g_score_dnf)
            continue;
        row->last_points = points_for_rank(src.rank);
        row->points += row->last_points;
        if (src.rank == 1)
            row->wins++;
        if (src.rank <= 3)
            row->podiums++;
        if (row->best_rank == 0 || src.rank < row->best_rank)
            row->best_rank = src.rank;
    }
    g_races_counted++;
    if (g_series_races > 0 && g_races_counted >= g_series_races)
        g_series_complete = true;
    sort_rows();
    write_files();
}

// ---------------------------------------------------------------------------------------------
// Summary card

static const ImGuiWindowFlags CARD_FLAGS =
    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
    ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoScrollbar;

void standings_Draw() {
    if (!g_summary_visible || !g_enabled || g_count == 0)
        return;
    float scale = 1.6f, opacity = 0.6f;
    overlay_Theme(&scale, &opacity);

    const ImGuiIO &io = ImGui::GetIO();
    const ImVec2 pos = g_anchor == 0 ? ImVec2(g_margin_x, g_margin_y)
                                     : ImVec2(io.DisplaySize.x - g_margin_x, g_margin_y);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(g_anchor == 0 ? 0.0f : 1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(opacity);
    if (ImGui::Begin("##broadcast_standings", NULL, CARD_FLAGS)) {
        ImGui::SetWindowFontScale(scale);
        if (g_series_complete)
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "CHAMPION: %s", g_rows[0].name);
        else
            ImGui::TextUnformatted("STANDINGS");
        if (g_series_races > 0)
            ImGui::Text("race %d of %d", g_races_counted, g_series_races);
        else
            ImGui::Text("after %d race%s", g_races_counted, g_races_counted == 1 ? "" : "s");
        ImGui::Separator();

        const int rows = g_display_rows > 0 ? std::min(g_count, g_display_rows) : g_count;
        if (ImGui::BeginTable("standings", 4, ImGuiTableFlags_SizingFixedFit)) {
            for (int i = 0; i < rows; i++) {
                const StandingsRow &r = g_rows[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%2d", i + 1);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.name);
                ImGui::TableNextColumn();
                ImGui::Text("%4d", r.points);
                ImGui::TableNextColumn();
                if (r.last_points > 0)
                    ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "+%d", r.last_points);
                else
                    ImGui::TextUnformatted(" ");
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------------------------
// Config + panel

static void load_config() {
    g_enabled = config::get_int(INI_SECTION, "enabled", g_enabled) != 0;
    const std::string spec = config::get_string(INI_SECTION, "points", g_points_spec);
    if (!spec.empty())
        snprintf(g_points_spec, sizeof(g_points_spec), "%s", spec.c_str());
    g_score_dnf = config::get_int(INI_SECTION, "score_dnf", g_score_dnf) != 0;
    g_series_races = config::get_int(INI_SECTION, "series_races", g_series_races);
    g_display_rows = config::get_int(INI_SECTION, "display_rows", g_display_rows);
    g_summary_card = config::get_int(INI_SECTION, "summary_card", g_summary_card) != 0;
    g_anchor = std::clamp(config::get_int(INI_SECTION, "anchor", g_anchor), 0, 1);
    g_margin_x = config::get_float(INI_SECTION, "margin_x", g_margin_x);
    g_margin_y = config::get_float(INI_SECTION, "margin_y", g_margin_y);
    parse_points();
}

static void save_config() {
    config::set_int(INI_SECTION, "enabled", g_enabled);
    config::set_string(INI_SECTION, "points", g_points_spec);
    config::set_int(INI_SECTION, "score_dnf", g_score_dnf);
    config::set_int(INI_SECTION, "series_races", g_series_races);
    config::set_int(INI_SECTION, "display_rows", g_display_rows);
    config::set_int(INI_SECTION, "summary_card", g_summary_card);
    config::set_int(INI_SECTION, "anchor", g_anchor);
    config::set_float(INI_SECTION, "margin_x", g_margin_x);
    config::set_float(INI_SECTION, "margin_y", g_margin_y);
    config::save();
}

static void panel_standings() {
    ImGui::TextWrapped("Points for the top finishers of every race, carried across a series. The "
                       "summary card comes up with the results at the end of each race.");
    bool changed = false;
    changed |= ImGui::Checkbox("Keep championship standings", &g_enabled);
    changed |= ImGui::Checkbox("Show the summary card after each race", &g_summary_card);
    if (ImGui::InputText("Points per position", g_points_spec, sizeof(g_points_spec))) {
        parse_points();
        changed = true;
    }
    ImGui::TextDisabled("Comma-separated, first place first (%d scoring position%s).",
                        g_points_count, g_points_count == 1 ? "" : "s");
    changed |= ImGui::Checkbox("Classified DNFs score too", &g_score_dnf);
    changed |= ImGui::SliderInt("Races per championship (0 = endless)", &g_series_races, 0, 40);
    changed |= ImGui::SliderInt("Rows on the card", &g_display_rows, 3, STANDINGS_MAX_ENTRIES);
    changed |= ImGui::RadioButton("Left", &g_anchor, 0);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Right", &g_anchor, 1);
    changed |= ImGui::SliderFloat("Margin X", &g_margin_x, 0.0f, 1000.0f, "%.0f");
    changed |= ImGui::SliderFloat("Margin Y", &g_margin_y, 0.0f, 1000.0f, "%.0f");
    if (changed)
        save_config();

    ImGui::Separator();
    ImGui::Text("Championship %d, %d race%s scored", g_series, g_races_counted,
                g_races_counted == 1 ? "" : "s");
    if (ImGui::BeginTable("standings_panel", 5, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn("racer");
        ImGui::TableSetupColumn("pts");
        ImGui::TableSetupColumn("wins");
        ImGui::TableSetupColumn("races");
        ImGui::TableHeadersRow();
        for (int i = 0; i < g_count; i++) {
            const StandingsRow &r = g_rows[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%d", i + 1);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.name);
            ImGui::TableNextColumn();
            ImGui::Text("%d", r.points);
            ImGui::TableNextColumn();
            ImGui::Text("%d", r.wins);
            ImGui::TableNextColumn();
            ImGui::Text("%d", r.races);
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Reset standings"))
        standings_Reset();
    ImGui::SameLine();
    if (ImGui::Button(g_summary_visible ? "Hide card" : "Preview card"))
        standings_ShowSummary(!g_summary_visible);
}

static DebugPanel g_panel = {.category = "Race",
                             .name = "Standings",
                             .draw = panel_standings,
                             .dev_only = false};

void standings_RegisterPanel() {
    load_config();
    load_table();
    debug_ui_register(&g_panel);
}
