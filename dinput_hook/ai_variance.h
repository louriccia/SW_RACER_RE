#pragma once

// AI variance ("form"): makes AI-only races close and unpredictable. The stock AI paces the whole
// field off the favourite at fixed rank offsets (a procession); this replaces that multiplier at
// the swrRace_UpdateCatchup seam with per-race form, slow pace swings, pack compression toward a
// target spread, and random blunders. Only touches speedMultiplier (never paceMultiplier, which
// scales AI steering), and by default only when no human is racing.

void ai_variance_RegisterHooks();// from init_renderer_hooks
void ai_variance_RegisterPanel();// after register_builtin_debug_panels
