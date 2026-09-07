#include "overlay.h"
#include "race_telemetry.h"
#include "../debug_ui.h"
#include "../imgui_utils.h"             // settings_ini_path
#include "../game_deltas/tracks_delta.h"// swrUI_GetTrackNameFromId_delta

#include <imgui.h>

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>

// ---------------------------------------------------------------------------------------------
// Settings ([broadcast])

static bool g_leaderboard =
    false;                     // user toggle; a consumer can force it on (overlay_ForceLeaderboard)
static bool g_show_tags = true;// FIN / DNF / racing column
static float g_scale = 1.6f;
static float g_opacity = 0.6f;
static int g_anchor = 0;// 0 left, 1 right
static float g_margin_x = 10.0f;
static float g_margin_y = 60.0f;

// Consumer overrides
static bool g_forced = false;
static char g_title[32] = "";
static char g_footer[160] = "";

void overlay_ForceLeaderboard(bool on) {
    g_forced = on;
}
void overlay_SetTitle(const char *title) {
    snprintf(g_title, sizeof(g_title), "%s", title ? title : "");
}
void overlay_SetFooter(const char *footer) {
    snprintf(g_footer, sizeof(g_footer), "%s", footer ? footer : "");
}

// ---------------------------------------------------------------------------------------------

static const char *track_name(int track_index) {
    static char buf[64];
    if (track_index < 0)
        return "";
    const char *raw = swrUI_GetTrackNameFromId_delta(track_index);
    size_t o = 0;
    for (const char *p = raw ? raw : ""; *p && o + 1 < sizeof(buf); p++) {
        if (*p == '~' && p[1]) {
            p++;
            continue;
        }
        buf[o++] = *p;
    }
    buf[o] = '\0';
    return buf;
}

static void format_time(float seconds, char *out, size_t n) {
    if (seconds < 0.0f)
        seconds = 0.0f;
    const int cs = (int) (seconds * 100.0f + 0.5f);
    snprintf(out, n, "%d:%02d.%02d", cs / 6000, (cs / 100) % 60, cs % 100);
}

static void format_gap(float seconds, char *out, size_t n) {
    const int cs = (int) (seconds * 100.0f + 0.5f);
    if (cs >= 6000)
        snprintf(out, n, "+%d:%02d.%02d", cs / 6000, (cs / 100) % 60, cs % 100);
    else
        snprintf(out, n, "+%d.%02d", cs / 100, cs % 100);
}

// Status icons drawn with the draw list (the default ImGui font has no glyphs for these).
enum StatusIcon { ICON_NONE, ICON_FINISHED, ICON_CRASHED, ICON_FIRE };

static void draw_status_icon(StatusIcon icon) {
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float pad = h * 0.1f;
    const float size = h - 2.0f * pad;
    const ImVec2 o(p.x + pad, p.y + pad);
    switch (icon) {
        case ICON_FINISHED: {// checkered flag, 4 x 3 squares
            const float cw = size * 1.3f / 4.0f, ch = size / 3.0f;
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 4; c++) {
                    const bool dark = ((r + c) & 1) == 0;
                    dl->AddRectFilled(ImVec2(o.x + c * cw, o.y + r * ch),
                                      ImVec2(o.x + (c + 1) * cw, o.y + (r + 1) * ch),
                                      dark ? IM_COL32(20, 20, 20, 255)
                                           : IM_COL32(240, 240, 240, 255));
                }
            break;
        }
        case ICON_CRASHED: {// red X
            const float t = std::max(1.5f, size * 0.15f);
            dl->AddLine(o, ImVec2(o.x + size, o.y + size), IM_COL32(230, 60, 40, 255), t);
            dl->AddLine(ImVec2(o.x + size, o.y), ImVec2(o.x, o.y + size),
                        IM_COL32(230, 60, 40, 255), t);
            break;
        }
        case ICON_FIRE: {// orange flame: triangle over a red base
            dl->AddTriangleFilled(ImVec2(o.x + size * 0.5f, o.y), ImVec2(o.x + size, o.y + size),
                                  ImVec2(o.x, o.y + size), IM_COL32(255, 140, 20, 255));
            dl->AddTriangleFilled(ImVec2(o.x + size * 0.5f, o.y + size * 0.45f),
                                  ImVec2(o.x + size * 0.8f, o.y + size),
                                  ImVec2(o.x + size * 0.2f, o.y + size),
                                  IM_COL32(230, 40, 20, 255));
            break;
        }
        default:
            break;
    }
    ImGui::Dummy(ImVec2(size * 1.3f, h));
}

// No title bar, but movable: drag anywhere on the board to reposition it; the new spot is saved
// as the margins for the current anchor.
static const ImGuiWindowFlags OVERLAY_FLAGS =
    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
    ImGuiWindowFlags_NoScrollbar;
static bool g_reposition = true;// apply the configured margins on the next Begin
static bool g_dragging = false;
static void save_config();

static void draw_leaderboard(const RaceTelemetry *t) {
    char gaps[RACE_TELEMETRY_MAX_ROWS][48];
    for (int k = 0; k < t->n; k++) {
        const RaceTelemetryRow &r = t->rows[k];
        if (k == 0)
            format_time(t->leader_time_s, gaps[k], sizeof(gaps[k]));// leader: clock or final time
        else if (r.finished)
            format_gap(r.gap_leader_s, gaps[k], sizeof(gaps[k]));
        else if (r.gap_leader_laps >= 1.0f)
            snprintf(gaps[k], sizeof(gaps[k]), "+%d lap%s", (int) r.gap_leader_laps,
                     (int) r.gap_leader_laps == 1 ? "" : "s");
        else if (r.gap_leader_s >= 0.0f)
            format_gap(r.gap_leader_s, gaps[k], sizeof(gaps[k]));
        else
            snprintf(gaps[k], sizeof(gaps[k]), "--");// no leader pace sampled yet
    }

    const ImGuiIO &io = ImGui::GetIO();
    const ImVec2 pos = g_anchor == 0 ? ImVec2(g_margin_x, g_margin_y)
                                     : ImVec2(io.DisplaySize.x - g_margin_x, g_margin_y);
    if (g_reposition) {
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(g_anchor == 0 ? 0.0f : 1.0f, 0.0f));
        g_reposition = false;
    }
    ImGui::SetNextWindowBgAlpha(g_opacity);
    if (ImGui::Begin("##broadcast_leaderboard", NULL, OVERLAY_FLAGS)) {
        ImGui::SetWindowFontScale(g_scale);
        // Drag to move: while the window is being dragged, fold its position back into the
        // margins; persist once the button is released.
        const bool held = ImGui::IsWindowFocused() && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                          ImGui::IsMouseDragging(ImGuiMouseButton_Left);
        if (held) {
            const ImVec2 wp = ImGui::GetWindowPos();
            const ImVec2 ws = ImGui::GetWindowSize();
            g_margin_x = g_anchor == 0 ? wp.x : io.DisplaySize.x - (wp.x + ws.x);
            g_margin_y = wp.y;
            g_dragging = true;
        } else if (g_dragging) {
            g_dragging = false;
            save_config();
        }
        ImGui::Text("%s  |  %s", g_title[0] ? g_title : "RACE", track_name(t->track_index));
        ImGui::Separator();

        float gap_w = 0.0f;
        for (int k = 0; k < t->n; k++)
            gap_w = std::max(gap_w, ImGui::CalcTextSize(gaps[k]).x);

        if (ImGui::BeginTable("board", g_show_tags ? 4 : 3, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("pos");
            ImGui::TableSetupColumn("name");
            ImGui::TableSetupColumn("gap", ImGuiTableColumnFlags_WidthFixed, gap_w);
            if (g_show_tags)
                ImGui::TableSetupColumn("tag");
            const bool results = t->leader_finished;
            for (int k = 0; k < t->n; k++) {
                const RaceTelemetryRow &r = t->rows[k];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%2d", r.rank);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.name);
                ImGui::TableNextColumn();
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap_w -
                                     ImGui::CalcTextSize(gaps[k]).x);
                ImGui::TextUnformatted(gaps[k]);
                if (g_show_tags) {
                    ImGui::TableNextColumn();
                    StatusIcon icon = ICON_NONE;
                    if (r.finished)
                        icon = ICON_FINISHED;
                    else if (r.dead)
                        icon = ICON_CRASHED;
                    else if (r.on_fire)
                        icon = ICON_FIRE;
                    (void) results;
                    draw_status_icon(icon);
                }
            }
            ImGui::EndTable();
        }
        if (g_footer[0]) {
            ImGui::Separator();
            ImGui::TextUnformatted(g_footer);
        }
    }
    ImGui::End();
}

void overlay_Service() {
    race_telemetry_Update();
}

void overlay_Draw() {
    if (!g_leaderboard && !g_forced)
        return;
    const RaceTelemetry *t = race_telemetry_Get();
    if (!t->valid || t->n == 0)
        return;
    draw_leaderboard(t);
}

// ---------------------------------------------------------------------------------------------
// Config + panel

static const wchar_t *INI_SECTION = L"broadcast";

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
    g_leaderboard = GetPrivateProfileIntW(INI_SECTION, L"leaderboard", g_leaderboard, ini) != 0;
    g_show_tags = GetPrivateProfileIntW(INI_SECTION, L"show_tags", g_show_tags, ini) != 0;
    g_scale = ini_get_float(ini, L"scale", g_scale);
    g_opacity = ini_get_float(ini, L"opacity", g_opacity);
    g_anchor = std::clamp((int) GetPrivateProfileIntW(INI_SECTION, L"anchor", g_anchor, ini), 0, 1);
    g_margin_x = ini_get_float(ini, L"margin_x", g_margin_x);
    g_margin_y = ini_get_float(ini, L"margin_y", g_margin_y);
}

static void save_config() {
    const wchar_t *ini = settings_ini_path();
    ini_set_int(ini, L"leaderboard", g_leaderboard);
    ini_set_int(ini, L"show_tags", g_show_tags);
    ini_set_float(ini, L"scale", g_scale);
    ini_set_float(ini, L"opacity", g_opacity);
    ini_set_int(ini, L"anchor", g_anchor);
    ini_set_float(ini, L"margin_x", g_margin_x);
    ini_set_float(ini, L"margin_y", g_margin_y);
}

static void panel_broadcast() {
    ImGui::TextWrapped(
        "Race graphics drawn over the camera shot, for any race (yours, multiplayer, "
        "or the unattended AI loop, which forces the leaderboard on).");
    bool changed = false;
    changed |= ImGui::Checkbox("Leaderboard", &g_leaderboard);
    if (g_forced) {
        ImGui::SameLine();
        ImGui::TextDisabled("(forced on by the orchestrator)");
    }
    changed |= ImGui::Checkbox("Status icons (finished / crashed / on fire)", &g_show_tags);
    changed |= ImGui::SliderFloat("Scale", &g_scale, 1.0f, 3.0f, "%.1f");
    changed |= ImGui::SliderFloat("Opacity", &g_opacity, 0.0f, 1.0f, "%.2f");
    bool moved = false;
    moved |= ImGui::RadioButton("Left", &g_anchor, 0);
    ImGui::SameLine();
    moved |= ImGui::RadioButton("Right", &g_anchor, 1);
    moved |= ImGui::SliderFloat("Margin X", &g_margin_x, 0.0f, 1000.0f, "%.0f");
    moved |= ImGui::SliderFloat("Margin Y", &g_margin_y, 0.0f, 1000.0f, "%.0f");
    ImGui::TextDisabled("The board can also be dragged with the mouse.");
    if (moved)
        g_reposition = true;
    if (changed || moved)
        save_config();

    const RaceTelemetry *t = race_telemetry_Get();
    ImGui::Separator();
    if (t->valid)
        ImGui::Text("Telemetry: %d racers, source %d, judge state %d, leader pace %.4f laps/s",
                    t->n, (int) t->source, t->judge_state, t->leader_pace_lps);
    else
        ImGui::TextDisabled("Telemetry: no race");
}

static DebugPanel g_panel = {.category = "Race",
                             .name = "Broadcast",
                             .draw = panel_broadcast,
                             .dev_only = false};

void overlay_RegisterPanel() {
    load_config();
    debug_ui_register(&g_panel);
}
