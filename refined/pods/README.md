# pods (refined) — dossier

Status: dossier only, no code (2026-11-27). Base: `refine/track_lifecycle` (proto line). Folds in the
queued "Pod part drives" candidate (REFINEMENT_LAYER_ROADMAP §7.1). Builds on [core](../core/README.md)
(registries, schema loader, packs, fingerprint) and on the refined copies in [swrRace](../swrRace/README.md).

The problem in one line: retail has no "pod". It has a **pilot index 0..22**, and everything a pod is
(models, handling, part placement, chase camera, moving parts, special abilities, portrait, voice) hangs
off that index in three parallel fixed tables, a dozen `pilot == N` branches and one 112-row `switch`. A
modded pod can only *replace* a pilot, and then inherits that pilot's engine spacing, animations, camera,
flame attack or fused engines. The goal: pods and pilots as named data records on core, the stock 23 as
compiled-in `core.*` entries byte-identical to retail, a roster that grows past 23, and moving parts as
data rows. With no packs installed the game is retail (lockstep).

Ghidra was unreachable while writing this (HTTP 127.0.0.1:8080 down). Addresses come from `src/`
`// 0xADDR` slots and `data_symbols.syms`; table contents were read from the retail EXE's `.data`
section on disk (unencrypted; `.text` is not readable that way). Unverified claims are marked.

## 1. Boundary

### 1.1 Owned tables (all indexed by pilot id; `data_symbols.syms:369-386`, `src/types.h`)

| Global | Address | Type | Role |
|---|---|---|---|
| `swrRacer_PodData` | 0x004c2700 | `swrRacerData[23]`, stride 0x34 | Identity: self-index `id`, pod / alt (rider) / LOD / far model ids, name + lastname (`SWR_P32` string ptrs), `float20` (pilot scale; puppet + podium + taunt camera), `pilot_spriteId`, `hudSpriteId`, `puppet_modelId`; `unk1c` (float, 1.6 Anakin / 1.4 Sebulba) and `unk24` (int) unknown |
| `swrRacer_PodHandlingData` | 0x004c2bb0 | `PodHandlingData[23]`, stride 0x3c | 15 handling floats (`types.h:419`) |
| `swrRacer_AI_PodHandlingData` | 0x004c3114 | `PodHandlingData` | One shared AI profile: every AI racer gets it, plus only `hoverHeight`, `bumpMass`, `intersectRadius` from its pod (`swrObj.c:3991-3994`) |
| `swrRacer_PodVisualData` | 0x004c7088 | `swrRacerVisualData[23]`, stride 0x6c | Hover-pad offsets A/B, pre-race sweep keyframes, cable anchors, chase camera trail/height, exhaust anchor + glow scale, `unk64[2]` (`types.h:3341`) |
| `swrRace_SelectIndex` | 0x00e99240 | `swrRace_VehicleSelectEntry[23]` | Vehicle-select carousel |
| `upgradeInfos` | 0x004c1cb8 | `swrUpgradeInfo[42]` | Shop parts (price, model, type, level); upgrades are per profile, not per pod |
| per-entity | `swrRace` +0x344 / +0x1610 | `partNodes[75]`, `partOffset[75]` | Podd header slots (raw indices) and per-part smoothing state |

The live pod is reached through `swrScore.pilotId` (`types.h:1185`), typed `int*` but actually a
`swrRacerData*`; `*pilotId` reads the row's self-index `id`, which every per-pilot table and branch keys on.

### 1.2 Functions in scope

| Address | Ghidra name | Reads / does |
|---|---|---|
| 0x0045b5d0 / 0x0045b610 / 0x0045b7d0 | `swrObjHang_InitRosterScore` / `BuildRosterMultiplayer` / `BuildRosterSinglePlayer` | Copy handling into `swrScore.podStats` (the physics' only source, except engine sound); SP draws AI picks as `Rand * 23.0f` |
| 0x00449d00 / 0x004493f0 / 0x00449330 | `swrRace_ApplyUpgradesToStats` / `CalculateUpgradedStat` / `ComputeStatBars` | Upgrade math on `podStats`; garage bars |
| 0x0045cf60 | `swrObjHang_ComputeUpgradedStats` | Garage preview through `swrScores[0]` |
| 0x004663e0 | `swrObjJdge_SpawnRacers` | Model load per LOD class (refined copy exists) |
| 0x00475ad0 | `swrRace_Init` | `footprintX/Y` from `partNodes[0x3e]` AABB; Neva Kee override |
| 0x00476740 | `swrRace_UpdateHoverPads` | Hover pads from visual A/B; returns ground distance to `swrRace_UpdateGroundContact` 0x00479e10 |
| 0x00470ae0 | `swrRace_PoddAnimateEngines` | Engine/cockpit transforms from visual A/B + `partOffset[1,2,5]` |
| 0x00471760 | `swrRace_PoddAnimateVariousThings` | Cables, exhaust, glow (refined copy exists) |
| **0x00472a50** | **`swrRace_PoddAnimateSteeringParts`** | **Part drives: 7 signals, per-pilot `switch`** (`swrObj.c:10301`) |
| 0x00470510 | `swrRace_SmoothEngineExhaustSize` | Generic clamped spring on `partOffset[part].y` (misnamed) |
| 0x0046e2c0 / 0x0046dcd0 / 0x0046f0e0 | `AnimateEngineWobble` / `TiltEngines` / `BuildStretchedQuad` | Visual row consumers |
| 0x0046ecd0 | `swrRace_ApplyEngineProximityPush` | Uses `footprintX`; only caller found is `UpdateScrapeSparks` 0x0046ee20 |
| 0x0046d7a0 | `swrRace_PlayEngineSounds` | Pitch from **base table** `bumpMass` (`swrRace.c:9997`) |
| 0x00451ef0 / 0x00452aa0 / 0x004528b0 / 0x004543f0 | `swrObjcMan_UpdatePreRaceSweep` / `UpdateChaseCamera` / `UpdateFirstPersonCamera` / `F4` | Camera rows; Neva Kee / Dud Bolt branches |
| 0x0043da10 / 0x00435700 / 0x004191f0 | `swrRace_BuildPartMenuList` / `SelectVehicle` / `swrUI_Menu_HangarSelectVehicle` | Roster UI (loop `< SWR_PILOT_COUNT`) |
| 0x00457bd0 | `swrObjHang_LoadAllPilotSprites` | 3 sprite bands at id, id+0x17, id+0x2e |
| 0x004584a0 / 0x004586e0 / 0x004337e0 | `loadPilotPuppet` / `LoadScreenAssets` / `swrRace_AnimateDisplayPod` | Hangar models |
| 0x0043ca30 / 0x0043ceb0 / 0x0043f8e0 / 0x0043fbc0 / 0x00437f70 / 0x0043f770 / 0x00438d20 | taunt scene, planet intro, holo/inspect cameras | `float20` |
| 0x004208e0 / 0x0041ac30 / 0x00439ce0 / 0x0043b880 | `swrText_FormatPodName`, `DrawRaceResultRow`, `ResultsMenu`, `CourseInfoMenu` | Names |
| 0x0044b530 / 0x0044b5e0 | `swrRace_ReplaceMarsGuoWithJinnReeso` / `ReplaceBullseyeWithCyYunga` | Cheat swaps: overwrite rows 8 / 22 at boot (`swrMain.c:59,65`) |

Seams (crossing edges, stay unrefined): physics reads `player->podStats` (sacred, R3);
`swrSound_ResolveSfxId` 0x00427110 (voice bank by pilot, bounds `0..0x16`); `swrPlayerHUD` loads
`hudSpriteId` into slot `i+0x2b` (`swrPlayerHUD.c:411`); save image (`swrSaveProfile.pilotId` uint8 at
+0x24, `pilotsUnlocked` uint32 at +0x34, `record3LapPilots char[50]` + sibling record tables); MP
(`swrMultiplayer_aRacerIds int[20]` 0x00ea0260 sent raw, `swrMultiplayer.c:2207/2237`); HD renderer
(`replacements.cpp` `try_replace_pod`); the asset buffer (model load order, texture-hash oracle).

## 2. Tier table

| Function / data | Tier | Reason |
|---|---|---|
| Roster builders, `InitRosterScore` | **sacred** | Fill `podStats` (physics input); SP draws `swrUtils_Rand` per AI pick, retried until distinct |
| `ApplyUpgradesToStats`, `CalculateUpgradedStat` | **sacred** | x87 clamp order on `podStats` (`RETAIL_FP`) |
| `UpdateHoverPads`, `swrRace_Init` footprint/Neva Kee | **sacred** | Ground distance feeds `UpdateGroundContact`; Init state |
| `PoddAnimateEngines`, `TiltEngines`, `AnimateEngineWobble` | **sacred** (uncertain) | `engineXf*` positions death fireballs next to `swrUtils_Rand` draws (`swrObj.c:10796`), and the proximity push; not proven presentation-only |
| `PoddAnimateSteeringParts`, `SmoothEngineExhaustSize` | equivalent (confirm) | Writes node transforms and `partOffset[0x12..0x37].y`; no RNG; the only other `partOffset` readers found are `[1] [2] [5]` (PoddAnimateEngines) and the Init reset. Confirm no raw-offset reader before relying on it |
| `ComputeStatBars`, `ComputeUpgradedStats` | equivalent | Display only |
| UI / select / sprites / puppets / names / hangar cameras | equivalent | Observable output; keep hangar `swrUtils_Rand` draw count (roadmap §3) |
| `PlayEngineSounds`, camera consumers | equivalent | Audio / view; camera keeps its RNG order |
| `SpawnRacers` model loads | equivalent | Load order = asset-buffer layout (texture-hash oracle) |
| **Loading the tables** (registry -> rows) | equivalent | Same bytes into the same consumers; checked by a byte-for-byte dump compare |
| Cheat swaps as roster entries, roster > 23, custom pods in the AI pool | **fix** | New behaviour; vanilla toggle keeps the swap / 23-pilot pool |

Rule: physics consuming handling is sacred and untouched; making the handling *loadable* is equivalent
as long as `podStats` receives identical bytes. A pack that changes them is modded content, not a tier
change (MODDING_API_ROADMAP "Sacred code with modded data is fine").

## 3. Flaw inventory

| Tag | Address / file:line | Finding | Refinement |
|---|---|---|---|
| HARDCODE (H1) | tables §1.1 | Pod data compiled into three parallel 23-row tables keyed by pilot | Registry records; tables become a generated view |
| HARDCODE (H2) | 0x00472a50 | Part drives are a `switch(pilot)`: 15 pilots, 47 rotation builds, 112 `SetPartTransform` calls; pilots 9, 11, 16-21 have none | Drive rows (section 4.4) |
| TANGLE (T1) | 0x00472a50, `swrObj.c:10320` | Drives keyed by pilot id and raw Podd slot: a custom pod on pilot 3's index animates pilot 3's slots (or nothing); no way to add a moving part or a signal | Rows owned by the pod; parts by contract name later |
| HARDCODE (H3) | Neva Kee (14): `swrObj.c:448,971,974,9930`, `swrRace.c:6706,7914,10211,10432,10624`; Dud Bolt (10): `swrObj.c:978`; Sebulba (2): `swrObj.c:9842` (F0), UpdatePlayerControl, taunt scene `swrObj.c:2718-2725` (`PodData[2].float20`) | Abilities and quirks by pilot number: fused engines + 3x5 footprint, first-person camera offset, flame attack, taunting pilot | Pod flags / fields (`fused_engines`, `fp_camera_forward`, `flame_attack`, roster `taunter`) |
| LIMIT (L1) | `SWR_PILOT_COUNT` 23: `BuildPartMenuList` loop, `Rand * 23.0f` (`swrObj.c:3967`; retail shares the float const 0x004ad0ac with LoadScreenAssets, per memory, unverified), `LoadAllPilotSprites` `< 0x17`, `ResolveSfxId` `variant <= 0x16`, `SelectIndex[23]` | Roster fixed at 23 | Capacity from the roster registry; every site listed in step 6 |
| LIMIT (L2) | `swrSaveProfile.pilotsUnlocked` uint32; `BuildRosterSinglePlayer` `1u << (id & 0x1f)` | Unlocks cap at 32 pilots and silently alias above 31 (UNSAFE) | Stock bits stay in tgfd.dat; extra unlocks by slug in a sidecar |
| LIMIT (L3) | `vehiclePlayer`/`vehicleOpponent` char, `swrSaveProfile.pilotId` uint8, `record*Pilots` char | Pilot ids are bytes on disk and in the hangar | Runtime ids stay < 128; on disk custom pilots are stored by slug (sidecar), stock id as fallback |
| LIMIT (L4) | `swrScores[20]` 0x00e29bc0 | 20 racers per race | Out of scope (race manager); noted because roster growth is not grid growth |
| LIMIT (L5) | 0x004493f0 | Upgrade clamps are absolute (`maxSpeed` 450..650, `turnResponse` 50..1000, `acceleration` 0.1..5, `antiSkid` 0.01..1): any installed upgrade drags a custom pod with base stats outside them to the clamp | Document; Fix-tier toggle "clamp relative to pod" later, as a mod option |
| LIMIT (L6) | 0x00457bd0 | Portrait bands at fixed offsets 0x17 / 0x2e: pilot 23 collides with band 2's pilot 0 (finish screen showed Anakin for Jinn in PR #235) | Bands sized from the roster |
| GLOBAL (G1) | 0x0044b530 / 0x0044b5e0 | Cheat swaps mutate static tables at boot; mutually exclusive with the replaced pilot, AI included. Jinn also changes `hoverPadOffsetB.x` (4.43), a **sacred** input | Separate roster entries (Fix, toggle) |
| TANGLE (T2) | 0x0045cf60 | Garage preview borrows `swrScores[0]` (profile, pilotId, podStats) | Preview on a local stats struct (equivalent: nothing in the hangar reads slot 0 after) — confirm |
| DECOMP (D1) | `types.h:1185` | `swrScore.pilotId` typed `int*`, holds `swrRacerData*`; `*pilotId` relies on `id` at +0 | `const PodRecord*` + explicit `runtime_id` |
| DECOMP (D2) | `swrObj.c:3346` | `LoadAllPilotSprites` walks `(swrRacerData*)&PodData[0].pilot_spriteId`, so `data->id` is the sprite id | Plain field read |
| DECOMP (D3) | 0x00449330 | Stat bars typed `PodHandlingData` with handling field names over different meanings | `StatBars[7]` |
| CLEVER (C1) | `swrRacerData.id` | Self-index duplicated in the row; PR #235's floating cables came from copying a host row's `id` | Runtime id assigned by the registry, never copied |
| DUP (D4) | 0x0045b610 vs 0x0045b7d0 | MP builder skips upgrades (every MP race on stock pods); our MP delta re-applies them after the fact | One builder path: base stats + optional upgrade set |
| DESIGN (note) | `swrObj.c:3991` | AI ignores 12 of 15 handling fields (shared AI profile) | Keep (sacred); per-pod AI handling is a Fix toggle at most |
| PORT (P1) | `swrRacerData.name/lastname` `SWR_P32` | 4-byte string pointers in the row | Registry owns native strings; 4-byte rows only at the faithful seam |
| SILENT (S1) | `ResolveSfxId` | Out-of-range pilot -> no voice, silently | Pilot record names a voice bank (stock fallback), logged |
| UNVERIFIED | `swrRace_Init` | Memory note says pod-on-pod collision uses the right shadow footprint (`partNodes[0x3e]` AABB); in src the only `footprintX` reader is the proximity push for scrape sparks, `footprintY` has none. Treat model geometry as sim-relevant until resolved | Oracle step 1 |

Stock quirks to keep (drive table): pilot 5 writes `0x16` and `0x24` twice (spin, then yaw; last wins);
pilot 0 pairs `-pitchR` on `0x23` with `+pitchR` on `0x16`/`0x2f` and `-pitchL` on `0x15`/`0x2e` with
`+pitchL` on `0x24`. Correction to roadmap §7.1: the `partNodes[0x3b]` flag write precedes the LOD return,
but the six smoothing calls and the spin update run **after** it (`lodDistance > 75` skips them), for
every pilot including those without drives.

## 4. Target shape

### 4.1 Three registries on core

Retail binds pilot and pod 1:1; mods want a new pod for a stock pilot and vice versa. Split, with a roster
entry that binds them (open question 1):

| Registry | Record | Retail source |
|---|---|---|
| `pod` | models, handling, visual row, flags, drive rows | `PodData` model ids, `PodHandlingData`, `PodVisualData`, H3 branches, 0x00472a50 |
| `pilot` | name, lastname, puppet, scale (`float20`), portrait + HUD sprite, voice bank, `unk1c`/`unk24` (opaque until named) | `PodData` remainder |
| `racer` (roster entry) | slug, pilot, pod, unlock rule, AI-pool membership, roster order | index 0..22, `ELFSAVE_DEFAULT_PILOTS` 0x22e01 |

- **Identity:** `core.racer.<name>` for the 23 stock entries in retail order; packs add `<pack>.<slug>`.
  Runtime id = roster index: 0..22 fixed, then custom entries in (pack load order, slug) order, so every
  peer with the same fingerprint allocates the same ids. Persistent data never stores a custom runtime id.
- **Storage:** the refined subsystem owns `PodRecord`/`PilotRecord` with native types, and generates the
  retail-shaped rows the faithful readers use (`swrRacerData[N]`, `PodHandlingData[N]`,
  `swrRacerVisualData[N]`, `SelectIndex[N]`). In `swe1r` those globals are rebound with
  `standalone_global_rebind` (as custom tracks do for `g_aTrackInfos`; add them to
  `standalone/owned_indirect.txt`). In the retail DLL the PR #235 operand-patch set is the same seam.
- **Retail defaults:** compiled from the retail tables, dumped by core's dumper to `data/core/pods.json`,
  `pilots.json`, `racers.json`; test: generated rows == retail `.data` bytes for all 23 rows + AI row.
- **Units:** handling keeps retail units (they are the physics' units: speed in game units/s, turn in
  deg/s); visual offsets in pod-local game units; `exhaust_glow_scale` as stored (x0.01 applied by code).

### 4.2 Fields and sim tags (feeds core 3.4)

| Field | REG_SIM | Why |
|---|---|---|
| `handling.*` (15 floats) | yes | Copied to `podStats` for humans; 3 fields for AI |
| `visual.hover_pad_a`, `visual.hover_pad_b` | yes | `UpdateHoverPads` -> ground contact |
| `flags.fused_engines`, `flags.flame_attack` | yes | `swrRace_Init` footprint, F0 / UpdatePlayerControl |
| `models.*` | yes (until U-footprint resolved) | Footprint AABB, asset-buffer low-memory racer clamp |
| roster order and AI pool | yes | SP AI pick is `Rand * pool_size` |
| camera, sweep, cables, exhaust, drives, names, sprites, puppet, voice | no (`REG_LOCAL_ONLY` where synced state is not touched) | Presentation |

The AI profile (`core.ai_handling`) is a separate sim record.

### 4.3 Example: stock pod as core data (Sebulba, values read from the retail EXE)

```json
"core.pod.sebulba": {
  "models":   { "pod": 6, "alt": 5, "lod": 255, "far": 225 },
  "handling": { "anti_skid": 0.38, "turn_response": 228, "max_turn_rate": 95, "acceleration": 3.2,
                "max_speed": 600, "air_brake_inv": 38, "deceleration_interval": 50, "boost_thrust": 185,
                "heat_rate": 9, "cool_rate": 2, "hover_height": 4.99, "repair_rate": 0.19,
                "bump_mass": 80, "damage_immunity": 0.3, "intersect_radius": 7 },
  "visual":   { "hover_pad_a": [0, -7, 1.5], "hover_pad_b": [3, 5, 0], "sweep_cockpit": [0, -1, 0.5],
                "cable_cockpit": [0.7, 2.3, -0.2], "cable_engine": [0, -1.6, 1.35],
                "sweep_engine": [1.52, 1.42, 0], "sweep_end_x": 0, "exhaust": [0, -5.86, 0],
                "exhaust_glow_scale": 1.19, "unk64": [0.75, 1] },
  "camera":   { "chase_trail": 14.96, "chase_height": 3.86 },
  "flags":    { "flame_attack": true },
  "drives": [
    { "signal": "yaw_r", "axis": "yaw", "scale": -1, "parts": [32, 33] },
    { "signal": "yaw_l", "axis": "yaw", "scale": -1, "parts": [18, 19] }
  ]
},
"core.pilot.sebulba": { "name": "/SCREENTEXT_326/~~Sebulba", "lastname": "", "puppet": 91,
  "scale": 106, "sprite": 38, "hud_sprite": 67, "voice": 2, "unk1c": 1.4, "unk24": 8 },
"core.racer.sebulba": { "index": 2, "pilot": "core.pilot.sebulba", "pod": "core.pod.sebulba",
  "unlock": "default", "taunter": true }
```

Floats are dumped with enough digits to round-trip the exact `float` (`4.99` stands for the stored value).
Model fields are MODELIDs today; they become asset refs through the asset resolver seam (custom >= 420).

### 4.4 Part drives as data (roadmap 7.1)

- **Signals** stay computed exactly as today, once per frame, for every pod with `partNodes` and
  `lodDistance <= 75`: `flap_pitch` (part 0x2e state), `vane_yaw` (0x2f), `yaw_r` (0x20), `yaw_l` (0x12),
  `pitch_r` (0x23), `pitch_l` (0x15), `spin` (`partOffset[33].y`, speed x2 clamped 100..700 deg/s,
  wrapped to +-180). The `partOffset` index each smoother uses is part of the signal definition, not of
  the pod. The `partNodes[0x3b]` flag write stays before the LOD return.
- **Rows:** `{signal, axis, scale, parts[]}` in original order; one row = one `rdMatrix_SetRotation44`
  build applied to its parts in order, so call order and matrix arithmetic match. All stock scales are
  +-1, -2 or +-0.5 (powers of two, exact in any precision; the 0.5 is a double at 0x004ad9d8 in retail).
  15 pilots, 47 rows, 112 part writes, extracted 1:1 including pilot 5's duplicate writes.
- **Key** moves from pilot to pod: the loop reads `pod->drives`. Part references are raw Podd slots for
  stock data; part names come with the socket contract (POD_MODDING_ROADMAP §3.1/§3.5).
- **New signals** (boost, heat, damage) are plugins registering a named signal; pods reference names.
- Refined name: `pod_drives_update` (0x00472a50); `pod_spring_step` (0x00470510).

### 4.5 Custom pod package (global pack, core 3.3)

```
mods/mars-racer/
  pack.json                 { "slug": "mars-racer", "data": ["pods/redline.json"] }
  pods/redline.json
  models/redline_pod.bin  redline_alt.bin  redline_mid.bin  redline_far.bin  (+ optional .gltf)
```
```json
{
  "pods":    { "mars-racer.redline": { "base": "core.pod.anakin",
                 "models": { "pod": "models/redline_pod.bin", "alt": "models/redline_alt.bin",
                             "lod": "models/redline_mid.bin", "far": "models/redline_far.bin" },
                 "handling": { "max_speed": 560, "acceleration": 2.4 },
                 "visual": { "hover_pad_b": [3.6, 4.8, 0] },
                 "camera": { "chase_trail": 11.5 },
                 "drives": [ { "signal": "flap_pitch", "axis": "pitch", "scale": 1, "parts": ["fin_r", "fin_l"] } ] } },
  "racers":  { "mars-racer.redline": { "pilot": "core.pilot.anakin", "pod": "mars-racer.redline",
                 "unlock": "always", "ai_pool": false } }
}
```
`base` copies every unspecified field from a stock record (explicit, not an override of it). Mapping:
model -> asset resolver ids; sockets -> visual row; handling -> `podStats` source; pilot -> `swrRacerData`
pilot half; camera -> visual 0x4c/0x50; drives -> rows. A pack with `handling`, `hover_pad_*`, flags or
`ai_pool: true` is sim-affecting via the computed tags; one with only camera/drives/models-for-render is not
(models pending the footprint question).

### 4.6 Growing the roster past 23: what must follow

| Consumer | Change |
|---|---|
| `BuildPartMenuList`, `SelectIndex` | Loop and array sized from the roster |
| Unlocks | Stock bits unchanged in tgfd.dat; custom entries by slug in the profile sidecar (`track_times_sidecar` precedent) |
| `BuildRosterSinglePlayer` AI pick | Vanilla: pool = stock 23 (same `Rand * 23.0f`). Toggle: pool = roster entries with `ai_pool` (changes RNG mapping, sim-affecting) |
| `LoadAllPilotSprites`, results portraits | Bands sized N; audit fixed-slot consumers |
| Voice | Pilot record's `voice` bank (stock id); `ResolveSfxId` sees only 0..22 |
| Save (`pilotId` uint8, record pilots char) | Sidecar stores slug; tgfd.dat gets the pilot's stock fallback, so retail still reads it |
| MP (`aRacerIds` int) | Ids valid only under a matching fingerprint; lobby handshake carries roster slugs |
| HUD / names / camera deltas | Read the roster count instead of `23` |

## 5. Band-aids retired

| Band-aid | Where | Replaced by |
|---|---|---|
| Extensible-roster operand patches (110 reader sites, `BuildPartMenuList` reimpl, `23.0f` FMUL relocation) | PR #235 (open upstream; not in this tree) | Rebound tables in swe1r; the patch set stays only as the retail-DLL seam |
| Jinn / Cy cheat swaps | 0x0044b530 / 0x0044b5e0 via `swrMain.c:59,65` | Two `core.racer.*` entries behind a toggle (default: swap, as retail) |
| Chase-camera row poke around the call | `dinput_hook/camera/player_camera.cpp:525-541` | Camera override through the pod record (session-scoped override) |
| `NUM_PILOTS 23`, `HUD_NAME_MAX_PODS 23` | `player_camera.cpp:64`, `swrPlayerHUD_delta.cpp:58` | Roster count |
| Favorite-pilot name lookups by index | `tracks_delta.c:1196-1230` | Roster API (`racer_name(id)`) |
| MP upgrade post-pass on `podStats` | `swrMultiplayer_delta.cpp:159-210` | One builder path with an optional upgrade set (D4); the MP option itself stays a feature |
| HD pod child-index special cases (Ben's 4 engines, cockpit index 18) | `replacements.cpp:721-800` | Parts drawn per socket from the entity transforms (POD_MODDING_ROADMAP §3.4) |
| Pilot-2 flame / Neva Kee checks in refined copies | `refined/swrRace/racer_options_refined.c` | Pod flags; `flamejet_*` options become "grant flame_attack" |
| #437 pod editor keeping baked parts spinning | PR #437 | Drive rows in the edited pod package |

## 6. Oracle plan

| Step | Check |
|---|---|
| Table generation | Generated rows == retail `.data` bytes (0x004c2700, 0x004c2bb0, 0x004c3114, 0x004c7088), all 23 rows; `load(dump(defaults)) == defaults` |
| Any step | Lockstep `sa3_t0..24` + `seqS1v3` / `seqS2v3` / `seqC1v3`, refined on and off; texture-hash unchanged |
| Per-pilot coverage (new) | Lockstep only exercises the recorded player pilot's handling and the AI's hover rows. Record 23 short races (one per player pilot, one track, throttle held), plus one with a fully upgraded profile |
| Drive sweep (new) | For each pilot 0..22, a full-LOD entity (`full_lod_ai`) driven by a scripted signal sequence: `turnRateTarget` sweep, speed 0..800, boosting / braking / finished flags, `lodDistance` 0 / 75 / 76, fixed dt, ~120 frames. Diff all 75 part-node transforms and `partOffset[]` after every frame, faithful vs refined; against retail through shadow mode (x86) |
| Specials -> flags | Neva Kee, Dud Bolt, Sebulba recordings (pilot coverage above) + taunt scene screenshot |
| Roster > 23 | Test-runner plans: select, race, results, save round-trip with a custom entry; tgfd.dat byte-identical with no packs (snapshot and restore `data/player` first) |
| Fingerprint | Changes with a handling or hover-pad edit, unchanged with a camera/drive-only pack |

Gaps: MP (needs two processes or a recorded event stream); save compatibility with retail beyond the
byte-identical no-pack case; AI using custom pods (sim-affecting by design, no retail oracle: own
determinism check, same race replayed twice); upgrades only covered by the upgraded-profile recording.

## 7. Step plan

1. **S — Oracle groundwork:** retail table dump test; per-pilot + upgraded-profile recordings; resolve the
   footprint / collision question (UNVERIFIED row). No dependencies.
2. **M — Drive table** (roadmap 7.1): signals + 47 rows compiled in, `pod_drives_update` replaces
   0x00472a50 keyed by pilot first. Drive-sweep oracle. No core dependency.
3. **S — DECOMP + specials:** typed `pilotId`, sprite walk, stat bars (D1-D3); H3 branches read pod flags
   from compiled records (lockstep + per-pilot recordings). Builds on racer_options.
4. **M — Registries:** `pod` / `pilot` / `racer` on core (needs core steps 2-4), generated rows, sim tags,
   drives keyed by pod. Equivalence: table bytes + lockstep.
5. **L — Roster growth:** rebind tables (swe1r), UI loops, unlock + save sidecar, sprite bands, voice
   fallback, Jinn/Cy entries behind the toggle (section 4.6). Needs core step 5 (packs).
6. **M — Pod packages:** global packs, `base` inheritance, asset resolver ids for models; part names once the
   socket contract exists.
7. **M — MP:** roster slugs + fingerprint in the handshake; `aRacerIds` validated against the roster.
8. **S — Upgrades:** constants named (sacred formula unchanged); relative-clamp option as a Fix toggle.
9. **S — Retire band-aids** (section 5).

Steps 1-3 can start now; 4 waits for core steps 2-4.

## Open questions

1. Split pod / pilot / racer, or one racer record with pilot and pod halves? (Split assumed.)
2. Custom pods in the AI pool and per-pod AI handling: offered at all, and default off?
3. Jinn / Cy: keep the retail swap as default, or ship them as separate entries by default (Fix tier)?
4. Is pod model geometry sim-relevant (footprint / collision)? Decides whether a model-only pack is cosmetic.
5. Stock drive rows by raw slot forever, or renamed to contract part names once the contract lands?
6. Save sidecar: extend `custom_times.json`'s file or a separate profile sidecar?
