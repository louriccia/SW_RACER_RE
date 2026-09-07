#pragma once

// Race orchestrator (Phase 0 spike): runs unattended AI-only races back to back.
//
// Borrows the retail attract-mode roster path (swrObjHang.demo_mode != 0 -> every slot 'AAII')
// without ever setting swrRace_demoMode, so the race keeps its HUD, finish flow and spectator
// camera. Re-enters the next race through swrObjHang_LoadScreen(hang, 1, 0), the same call the
// demo 'Abrt' handler in swrObjHang_F4 uses.

// Race graphics live in broadcast/overlay.cpp (the orchestrator forces the leaderboard on while armed).
// Detours: swrObjHang_F4 (race end -> next race), swrRace_CalcTargetTurnRate (AI fidelity/damage).
// Call from init_renderer_hooks.
void orchestrator_RegisterHooks();

// ImGui "Orchestrator" panel. Call once after register_builtin_debug_panels.
void orchestrator_RegisterPanel();

// Per-frame service on the game thread (next to service_fast_restart): fires the judge's
// 'Fini' once every racer has finished and starts the next race when the pause elapses.
void orchestrator_Service();

// What the orchestrator is running / about to run. Seed of the game-modes ruleset descriptor.
struct RaceDescriptor {
    int track_index;// 0..24
    int laps;
    int racer_count;// 1..20
    int human_slots;// 0 = all-AI
};

enum OrchestratorEvent {
    ORCH_RACE_STARTED = 1,// LoadScreen issued for the descriptor
    ORCH_WINNER_IN,       // first racer finished; results window open
    ORCH_RACE_ENDED,      // judge torn down ('Fini' / 'Abrt'); the next race is being started
};

typedef void (*OrchestratorListener)(int event, const RaceDescriptor *race);
void orchestrator_Subscribe(OrchestratorListener cb);
const RaceDescriptor *orchestrator_CurrentRace();

#ifdef __cplusplus
extern "C" {
#endif
// Hotkey entry (F8): arm / disarm the loop.
void orchestrator_ToggleArmed(void);
#ifdef __cplusplus
}
#endif
