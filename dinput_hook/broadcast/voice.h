#pragma once

// Broadcast voice: the pilot voice lines and announcer commentary a human race gets, replayed for
// the racer the camera follows when nobody local is racing. The game gates every line on the pod
// being LOCAL (swrObjTest_F4 collision taunts, swrRace death / fire / boost / finish lines,
// swrObjJdge_UpdateOvertakeSounds); this module fires the same category-1 (pilot) lines for the
// followed pod from telemetry edges and lends LOCAL where the stock logic is self-contained.
// Settings live in [voice] in SW_RACER_RE.ini.

// Detours (from init_renderer_hooks).
void voice_RegisterHooks();

// Per-frame, on the game thread after the telemetry snapshot refresh.
void voice_Service();

// Panel registration.
void voice_RegisterPanel();

// Announcer introduction for one racer (the pre-race Fode & Beed line the game plays for the local
// pilot). `variant` is only logged. Returns the line length in ms (0 = nothing played).
int voice_AnnounceRacer(int slot, int variant);
