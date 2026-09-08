#pragma once

// Broadcast overlay: ImGui race graphics drawn over the camera shot, fed by the race telemetry
// snapshot. Works in any race (human, multiplayer, all-AI). Settings live in [broadcast] in
// SW_RACER_RE.ini; the Race > Broadcast panel exposes them.

// Per-frame, on the game thread before the ImGui frame: refreshes the telemetry snapshot.
void overlay_Service();

// Inside the ImGui frame (next to draw_fps_overlay).
void overlay_Draw();

// Panel registration (after register_builtin_debug_panels).
void overlay_RegisterPanel();

// Detours (from init_renderer_hooks): swrObjJdge_F3 post-hook that draws the game's own lap timer,
// speedometer and engine gauges for the highlighted (followed) racer when nobody local is racing.
void overlay_RegisterHooks();

// A consumer (e.g. the race orchestrator) can force the leaderboard on regardless of the user's
// toggle, retitle it, and add a footer line. Empty strings clear.
void overlay_ForceLeaderboard(bool on);
void overlay_SetTitle(const char *title);  // replaces the default "RACE" word
void overlay_SetFooter(const char *footer);// one line under the table

// Shared look for the other broadcast windows (scale, background opacity).
void overlay_Theme(float *scale, float *opacity);

// Rows become clickable when a handler is set (called with the swrScoresPtr slot); the highlighted
// slot is drawn selected (e.g. the pod the camera follows). -1 / NULL clear.
void overlay_SetRowClickHandler(void (*handler)(int slot));
void overlay_SetHighlightSlot(int slot);

// The followed racer's score when the broadcast HUD stands in for a local player (no human in the
// race, game gauges on, race running), else NULL. swrObjJdge_DrawRaceHUD lends it the local-player
// globals so the minimap / position markers draw.
struct swrObjJdge;
struct swrScore;
swrScore *overlay_HudStandInLocal(const swrObjJdge *jdge);

// Names over pods instead of the stock position numbers (single-player / all-AI; multiplayer
// already shows player names). User toggle in the Broadcast panel, or forced by a consumer.
bool overlay_NameplatesActive();
void overlay_ForceNameplates(bool on);
// Temporarily hide the labels (e.g. during a wide drone shot) without changing the toggles.
void overlay_SuppressNameplates(bool suppress);
bool overlay_NameplatesSuppressed();
// Per-racer filter for the overhead names: the followed racer always, plus its nearest
// neighbours within range ([broadcast] nameplate_neighbors / nameplate_max_dist).
bool overlay_NameplateVisible(int score_slot);
// Label glyph scale (0.5 = the stock "~F" half-size text) and vertical nudge in screen px.
void overlay_NameplateStyle(float *scale, int *offset_y);
