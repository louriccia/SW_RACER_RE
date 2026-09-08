#include "overlay.h"
#include "race_telemetry.h"
#include "../debug_ui.h"
#include "../imgui_utils.h"
#include "../config.h"
#include "../game_deltas/tracks_delta.h"// swrUI_GetTrackNameFromId_delta
#include "../hook_helper.h"
#include "../camera/director.h"// director_FollowedSlot (nameplate filter)
#include "../ui_transform.h"

extern "C" {
#include <Swr/swrObj.h>
#include <Swr/swrRace.h>
#include <Swr/swrSprite.h>
#include <globals.h>
}

#include <imgui.h>

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cwchar>

// ---------------------------------------------------------------------------------------------
// Settings ([broadcast])

static bool g_leaderboard =
    false;                     // user toggle; a consumer can force it on (overlay_ForceLeaderboard)
static bool g_show_tags = true;// FIN / DNF / racing column
static float g_scale = 1.6f;
static float g_opacity = 0.6f;
static int g_anchor = 0;         // 0 left, 1 right
static bool g_nameplates = false;// names over pods (SP / all-AI)
static int g_nameplate_neighbors = 2;      // besides the followed racer, name this many nearest pods
static float g_nameplate_max_dist = 400.0f;// ... within this many world units of it
static float g_nameplate_scale = 0.35f;    // glyph scale (stock "~F" half-size text = 0.5)
static int g_nameplate_offset_y = -14;     // screen px; negative = above the pod
static bool g_nameplates_forced = false;
static bool g_nameplates_suppressed = false;
static bool g_pod_status =
    false;// followed racer's ImGui speed + engine card (game gauges preferred)
static bool g_game_gauges =
    true;// game's own lap timer / speedometer / engines for the followed racer
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

bool overlay_NameplatesActive() {
    return g_nameplates || g_nameplates_forced;
}
void overlay_ForceNameplates(bool on) {
    g_nameplates_forced = on;
}
void overlay_SuppressNameplates(bool suppress) {
    g_nameplates_suppressed = suppress;
}
bool overlay_NameplatesSuppressed() {
    return g_nameplates_suppressed;
}

// With a camera target, only the followed racer and its nearest neighbours are named, so the
// labels read as "who is in this shot" instead of a wall of text. No target (a human race, or the
// director off): everyone.
void overlay_NameplateStyle(float *scale, int *offset_y) {
    *scale = std::clamp(g_nameplate_scale, 0.15f, 1.0f);
    *offset_y = g_nameplate_offset_y;
}

bool overlay_NameplateVisible(int score_slot) {
    const int followed = director_FollowedSlot();
    if (followed < 0 || swrScoresPtr == NULL || score_slot < 0 || score_slot >= RACE_TELEMETRY_MAX_ROWS)
        return true;
    if (score_slot == followed)
        return true;
    const swrRace *me = swrScoresPtr[followed].obj_test_ptr;
    const swrRace *pod = swrScoresPtr[score_slot].obj_test_ptr;
    if (me == NULL || pod == NULL || g_nameplate_neighbors <= 0)
        return false;
    auto dist = [&](const swrRace *o) {
        const float dx = o->transform.vD.x - me->transform.vD.x;
        const float dy = o->transform.vD.y - me->transform.vD.y;
        const float dz = o->transform.vD.z - me->transform.vD.z;
        return sqrtf(dx * dx + dy * dy + dz * dz);
    };
    const float mine = dist(pod);
    if (mine > g_nameplate_max_dist)
        return false;
    // rank this pod among the others by distance to the followed racer
    int closer = 0;
    const RaceTelemetry *t = race_telemetry_Get();
    for (int k = 0; k < t->n; k++) {
        const int slot = t->rows[k].slot;
        if (slot == followed || slot == score_slot)
            continue;
        const swrRace *o = swrScoresPtr[slot].obj_test_ptr;
        if (o != NULL && dist(o) < mine)
            closer++;
    }
    return closer < g_nameplate_neighbors;
}

static void (*g_row_click)(int slot) = NULL;
static int g_highlight_slot = -1;

void overlay_SetRowClickHandler(void (*handler)(int slot)) {
    g_row_click = handler;
}
void overlay_SetHighlightSlot(int slot) {
    g_highlight_slot = slot;
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
        if (t->leader_finished || t->n == 0)
            ImGui::Text("%s  |  %s", g_title[0] ? g_title : "RACE", track_name(t->track_index));
        else
            ImGui::Text("%s  |  %s  |  LAP %d/%d", g_title[0] ? g_title : "RACE",
                        track_name(t->track_index), t->rows[0].lap, t->num_laps);
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
                if (g_row_click != NULL) {
                    ImGui::PushID(r.slot);
                    if (ImGui::Selectable(r.name, r.slot == g_highlight_slot,
                                          ImGuiSelectableFlags_SpanAllColumns |
                                              ImGuiSelectableFlags_AllowOverlap))
                        g_row_click(r.slot);
                    ImGui::PopID();
                } else if (r.slot == g_highlight_slot) {
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%s", r.name);
                } else {
                    ImGui::TextUnformatted(r.name);
                }
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

// Followed racer's card: speed bar + the six engines (left column / right column), colored by
// damage, flame marker when burning. Bottom-right, same scale as the board.
static void draw_pod_status(const RaceTelemetry *t) {
    const RaceTelemetryRow *r = NULL;
    for (int k = 0; k < t->n; k++)
        if (t->rows[k].slot == g_highlight_slot)
            r = &t->rows[k];
    if (r == NULL || r->finished)
        return;
    const ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - g_margin_x, io.DisplaySize.y - g_margin_y),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(g_opacity);
    if (ImGui::Begin("##broadcast_podstatus", NULL, OVERLAY_FLAGS | ImGuiWindowFlags_NoInputs)) {
        ImGui::SetWindowFontScale(g_scale);
        ImGui::Text("%s%s", r->name, r->boosting ? "   BOOST" : "");
        const float h = ImGui::GetTextLineHeight();
        const float w = h * 9.0f;
        const float frac =
            r->max_speed > 0.0f ? std::clamp(r->speed / r->max_speed, 0.0f, 1.3f) : 0.0f;
        char label[32];
        snprintf(label, sizeof(label), "%.0f", r->speed);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, r->boosting ? ImVec4(1.0f, 0.55f, 0.1f, 1.0f)
                                                                  : ImVec4(0.3f, 0.7f, 1.0f, 1.0f));
        ImGui::ProgressBar(std::min(1.0f, frac / 1.3f), ImVec2(w, h * 0.8f), label);
        ImGui::
            PopStyleColor();// engines: two columns of three (left top/mid/bot | right top/mid/bot)
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float cell_w = w * 0.48f, cell_h = h * 0.45f, gap = h * 0.12f;
        for (int e = 0; e < 6; e++) {
            const int col = e / 3, row = e % 3;
            const ImVec2 a(p.x + col * (cell_w + w * 0.04f), p.y + row * (cell_h + gap));
            const ImVec2 b(a.x + cell_w, a.y + cell_h);
            const float d = r->engine_damage[e];
            const ImU32 fill =
                IM_COL32((int) (60 + 195 * d), (int) (200 * (1.0f - d) + 40), 40, 230);
            dl->AddRectFilled(a, b, IM_COL32(40, 40, 40, 200));
            dl->AddRectFilled(a, ImVec2(a.x + cell_w * (1.0f - d), b.y), fill);
            dl->AddRect(a, b, IM_COL32(220, 220, 220, 180));
            if (r->engine_fire[e]) {
                const float fx = b.x - cell_h * 0.6f, fy = a.y + cell_h * 0.1f, s = cell_h * 0.8f;
                dl->AddTriangleFilled(ImVec2(fx + s * 0.5f, fy), ImVec2(fx + s, fy + s),
                                      ImVec2(fx, fy + s), IM_COL32(255, 140, 20, 255));
            }
        }
        ImGui::Dummy(ImVec2(w, 3 * cell_h + 2 * gap));
    }
    ImGui::End();
}

void overlay_Service() {
    race_telemetry_Update();
}

// The vanilla per-player HUD (swrObjJdge_UpdatePlayerHUD) runs only for local players, so an
// all-AI race has no gauges. After F3 has drawn its frame, draw the same lap timer + engine UI
// (speedometer, engine health) for the highlighted racer with the game's own routines, in the
// single-screen slot. Finished racers get the vanilla hide, as the player would.
static swrScore *followed_score(const swrObjJdge *jdge) {
    if (!g_game_gauges || jdge == NULL || firstLocalPlayer != NULL || swrScoresPtr == NULL)
        return NULL;
    if (!(g_leaderboard || g_forced) || g_highlight_slot < 0 ||
        g_highlight_slot >= RACE_TELEMETRY_MAX_ROWS)
        return NULL;
    const int state = jdge->flag & 0xf;
    // 0 = grid / countdown, 3-5 = post-race: the broadcast runs clean there (no gauges, no minimap,
    // no position markers -- swrObjJdge_DrawRaceHUD only draws those for a local player).
    if (state == 0 || state == 3 || state == 4 || state == 5)
        return NULL;
    swrScore *score = &swrScoresPtr[g_highlight_slot];
    return score->obj_test_ptr != NULL ? score : NULL;
}

swrScore *overlay_HudStandInLocal(const swrObjJdge *jdge) {
    swrScore *score = followed_score(jdge);
    // finished: the victory lap shows no HUD (swrObjJdge_F3_delta hides the gauges too)
    return score != NULL && (score->flag & 2) == 0 ? score : NULL;
}

void __cdecl swrObjJdge_F3_delta(swrObjJdge *jdge) {
    hook_call_original(swrObjJdge_F3, jdge);
    if (jdge != NULL && (jdge->flag & 0xf) == 0 && g_game_gauges && firstLocalPlayer == NULL &&
        (g_leaderboard || g_forced)) {
        // Grid / countdown: clear the HUD frame art and gauges the last racing frame left behind
        // (they are only ever re-shown per frame). The countdown lights live at 0xa1+, untouched.
        for (short id = 0; id <= 0x2a; id++)
            swrSprite_SetVisible(id, 0);
        return;
    }
    swrScore *score = followed_score(jdge);
    if (score == NULL)
        return;
    if ((score->flag & 2) != 0) {
        // Vanilla hides the engine readout on finish; the speed-dial cluster and its readout frame
        // (swrObjJdge_DrawSpeedDialHud / LayoutHudFrameSprites) are only ever re-shown per frame, so
        // hide those too or they linger from the last racing frame.
        swrObjJdge_HideEngineUI(score);
        // Header frame 0x0-0xd (swrObjJdge_LayoutHudFrameSprites), dial cluster 0xe-0x12 and the
        // engine readout up to 0x2a; the racer markers start at 0x2b and the countdown lights live
        // at 0xa1+, both left alone.
        for (short id = 0; id <= 0x2a; id++)
            swrSprite_SetVisible(id, 0);
        return;
    }
    // The speed-dial fill ratio comes from swrRace_GetBoostBarColor, which reads the pod's
    // boostIndicatorStatus: 0 = speed / max speed, 1 = charge timer, 2 = full. AI pods never run the
    // boost-charge state machine and sit at 1 with a zero timer, which draws an empty dial; present
    // them as state 0 for the draw only.
    swrRace *pod = score->obj_test_ptr;
    const uint32_t boost_status = pod->boostIndicatorStatus;
    pod->boostIndicatorStatus = 0;
    // Same anchoring scope swrObjJdge_UpdatePlayerHUD_delta uses, so the resolution-independent UI
    // pins the header / speedometer / engine readout to the screen edges as in a player's race.
    ui_in_race_hud++;
    swrRace_InRaceTimer(score, jdge);
    swrRace_InRaceEngineUI(score, 0);
    ui_in_race_hud--;
    pod->boostIndicatorStatus = boost_status;
}

void overlay_RegisterHooks() {
    hook_replace(swrObjJdge_F3, swrObjJdge_F3_delta);
}

void overlay_Draw() {
    if (!g_leaderboard && !g_forced)
        return;
    const RaceTelemetry *t = race_telemetry_Get();
    if (!t->valid || t->n == 0)
        return;
    draw_leaderboard(t);
    if (g_pod_status && g_highlight_slot >= 0)
        draw_pod_status(t);
}

// ---------------------------------------------------------------------------------------------
// Config + panel

static const char *INI_SECTION = "broadcast";

static void load_config() {
    g_leaderboard = config::get_int(INI_SECTION, "leaderboard", g_leaderboard) != 0;
    g_show_tags = config::get_int(INI_SECTION, "show_tags", g_show_tags) != 0;
    g_nameplates = config::get_int(INI_SECTION, "nameplates", g_nameplates) != 0;
    g_nameplate_neighbors = config::get_int(INI_SECTION, "nameplate_neighbors", g_nameplate_neighbors);
    g_nameplate_max_dist = config::get_float(INI_SECTION, "nameplate_max_dist", g_nameplate_max_dist);
    g_nameplate_scale = config::get_float(INI_SECTION, "nameplate_scale", g_nameplate_scale);
    g_nameplate_offset_y = config::get_int(INI_SECTION, "nameplate_offset_y", g_nameplate_offset_y);
    g_pod_status = config::get_int(INI_SECTION, "pod_status", g_pod_status) != 0;
    g_game_gauges = config::get_int(INI_SECTION, "game_gauges", g_game_gauges) != 0;
    g_scale = config::get_float(INI_SECTION, "scale", g_scale);
    g_opacity = config::get_float(INI_SECTION, "opacity", g_opacity);
    g_anchor = std::clamp(config::get_int(INI_SECTION, "anchor", g_anchor), 0, 1);
    g_margin_x = config::get_float(INI_SECTION, "margin_x", g_margin_x);
    g_margin_y = config::get_float(INI_SECTION, "margin_y", g_margin_y);
}

static void save_config() {
    config::set_int(INI_SECTION, "leaderboard", g_leaderboard);
    config::set_int(INI_SECTION, "show_tags", g_show_tags);
    config::set_int(INI_SECTION, "nameplates", g_nameplates);
    config::set_int(INI_SECTION, "nameplate_neighbors", g_nameplate_neighbors);
    config::set_float(INI_SECTION, "nameplate_max_dist", g_nameplate_max_dist);
    config::set_float(INI_SECTION, "nameplate_scale", g_nameplate_scale);
    config::set_int(INI_SECTION, "nameplate_offset_y", g_nameplate_offset_y);
    config::set_int(INI_SECTION, "pod_status", g_pod_status);
    config::set_int(INI_SECTION, "game_gauges", g_game_gauges);
    config::set_float(INI_SECTION, "scale", g_scale);
    config::set_float(INI_SECTION, "opacity", g_opacity);
    config::set_int(INI_SECTION, "anchor", g_anchor);
    config::set_float(INI_SECTION, "margin_x", g_margin_x);
    config::set_float(INI_SECTION, "margin_y", g_margin_y);
    config::save();
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
    changed |= ImGui::Checkbox("Names over pods (instead of position numbers)", &g_nameplates);
    changed |= ImGui::SliderInt("Named neighbours of the followed racer", &g_nameplate_neighbors, 0, 19);
    changed |= ImGui::SliderFloat("... within (world units)", &g_nameplate_max_dist, 50.0f, 3000.0f, "%.0f");
    changed |= ImGui::SliderFloat("Nameplate size (0.5 = stock small text)", &g_nameplate_scale, 0.15f, 1.0f, "%.2f");
    changed |= ImGui::SliderInt("Nameplate height above the pod (px)", &g_nameplate_offset_y, -80, 40);
    changed |= ImGui::Checkbox("Game gauges for the followed racer (timer, speedo, engines)",
                               &g_game_gauges);
    changed |= ImGui::Checkbox("Followed racer card (ImGui speed + engines)", &g_pod_status);
    if (g_nameplates_forced) {
        ImGui::SameLine();
        ImGui::TextDisabled("(forced on)");
    }
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
