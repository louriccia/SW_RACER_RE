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

// A consumer (e.g. the race orchestrator) can force the leaderboard on regardless of the user's
// toggle, retitle it, and add a footer line. Empty strings clear.
void overlay_ForceLeaderboard(bool on);
void overlay_SetTitle(const char *title);  // replaces the default "RACE" word
void overlay_SetFooter(const char *footer);// one line under the table

// Rows become clickable when a handler is set (called with the swrScoresPtr slot); the highlighted
// slot is drawn selected (e.g. the pod the camera follows). -1 / NULL clear.
void overlay_SetRowClickHandler(void (*handler)(int slot));
void overlay_SetHighlightSlot(int slot);

// Names over pods instead of the stock position numbers (single-player / all-AI; multiplayer
// already shows player names). User toggle in the Broadcast panel, or forced by a consumer.
bool overlay_NameplatesActive();
void overlay_ForceNameplates(bool on);
