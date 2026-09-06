#pragma once

// Race orchestrator (Phase 0 spike): runs unattended AI-only races back to back.
//
// Borrows the retail attract-mode roster path (swrObjHang.demo_mode != 0 -> every slot 'AAII')
// without ever setting swrRace_demoMode, so the race keeps its HUD, finish flow and spectator
// camera. Re-enters the next race through swrObjHang_LoadScreen(hang, 1, 0), the same call the
// demo 'Abrt' handler in swrObjHang_F4 uses.

// Detours: swrObjHang_F4 (race end -> schedule the next race). Call from init_renderer_hooks.
void orchestrator_RegisterHooks();

// ImGui "Orchestrator" panel. Call once after register_builtin_debug_panels.
void orchestrator_RegisterPanel();

// Per-frame service on the game thread (next to service_fast_restart): fires the judge's
// 'Fini' once every racer has finished and starts the next race when the pause elapses.
void orchestrator_Service();

// Live leaderboard window while armed. Call inside the ImGui frame (next to draw_fps_overlay).
void orchestrator_DrawOverlay();

#ifdef __cplusplus
extern "C" {
#endif
// Hotkey entry (F8): arm / disarm the loop.
void orchestrator_ToggleArmed(void);
#ifdef __cplusplus
}
#endif
