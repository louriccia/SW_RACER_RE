# refined/ — the refinement layer

The same game as `src/`, rewritten to be readable, modular and data-driven, so features stop
being byte patches against retail machine code. The design, tiers and oracle are in
REFINEMENT_LAYER_ROADMAP.md (kept outside the repo for now).

Rules:
- `src/` (the faithful, matching layer) is never edited from here. A refined function stands in
  for a faithful one; the host routes the faithful function's callers to it.
- Each refined function carries the `// 0xADDR` of the retail function it replaces, and is
  listed in `refined.c` (`refined_slots`) next to the faithful symbol.
- With every setting at its default, a refined function behaves exactly like retail (checked
  by lockstep and the texture-hash oracle). A behaviour change is a setting, never a silent edit.
- Features and mods use the refined APIs (`refined_*_set_*`), not memory patches.

Hosting today: the standalone feature build (`swe1r`, `-DSWR_STANDALONE_REFINED=ON`) installs
`refined_slots`. `SWR_REFINED_OFF=1` runs the faithful functions instead (A/B checks). The hook
DLL still uses its retail-side patches for these features.

| Subsystem | Replaces | Retired band-aid (in swe1r) |
|---|---|---|
| [swrAssetBuffer](swrAssetBuffer/README.md) | `swrScene_InitWorld` | asset buffer size immediates (`asset_buffer_size`) |
| [swrModel](swrModel/README.md) | `swrModel_LoadModelTexture`, `swrAssetBuffer_InvalidateTexturesBelow` | texture-buffer operand patches (custom tracks) |
| [swrRace](swrRace/README.md) | `swrObjJdge_SpawnRacers`, `swrRace_PoddAnimateVariousThings`, `swrRace_UpdatePlayerControl`, `swrObjTest_F0` | `ai_full_lod` NOPs, flamejet cheat JNZ patches |
| [swrObjJdge](swrObjJdge/README.md) | `swrObjJdge_F2`, `swrRace_InRaceTimer`, `swrRace_InRaceEndStatistics`, `swrObjHang_ResultsMenu`, `swrMultiplayer_InRace` | lap-slot de-indexing (`lap_time_overflow`), 3000 s constants (`race_time_cap`) |

Refined functions attach underneath every delta on the same function, so a delta's
`hook_call_original` reaches the refined version.

Seam outside this directory: `g_aTrackInfos` is an indirect global in the standalone
(`standalone/owned_indirect.txt`); custom tracks point it at their larger table with
`standalone_global_rebind` instead of patching every operand that addresses it.
