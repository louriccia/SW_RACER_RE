#pragma once

// C7 results / event export: every finished race appended to disk so a race nobody watched can
// still be looked up afterwards, and so external tools (OBS text sources, bots, verification runs)
// have a machine-readable feed. Settings live in [results_log] in SW_RACER_RE.ini.

struct RaceResult;

// Append one race to the configured log(s). No-op when disabled.
void results_log_Write(const RaceResult *result);

// Directory the logs are written to, relative to the game directory.
const char *results_log_Directory();

// Panel registration (after register_builtin_debug_panels).
void results_log_RegisterPanel();
