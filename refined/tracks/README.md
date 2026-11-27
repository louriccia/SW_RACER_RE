# tracks (refined) — dossier

Status: dossier only, no code (2026-11-27). Base: `refine/track_lifecycle` (proto line). Builds on
[refined/core](../core/README.md) (registries, schema loader, packs, fingerprint, events),
[refined/track_lifecycle](../track_lifecycle/README.md) (load/unload events) and
[refined/swrObjTrig](../swrObjTrig/README.md) (trigger assets and lists; not redone here).

The problem in one line: a track is not a thing in retail, it is a row index. `g_aTrackInfos[25]`
gives a model id, a spline id and the pair `(PlanetIdx, planetTrackNumber)`, and about twenty
consumers then decide fog, music, AI, weather, sun, dust, fly-by, HUD quirks and even sim rules from
`if (planetId == N && planet_track_number == M)` ladders. Menus add a second index space
(`g_aTrackIDs[circuit*7 + slot]`), planets a third capped at 8. Custom tracks get in by inflating the
row table (operand patches / global rebind), borrowing a stock `(planet, subtrack)` pair and
everything that pair means, and patching retail tables while the track is current.

Goal: a track is a registry record with a slug; a planet is a preset of environment fields a track
may inherit, never a dependency; a circuit is an ordered list of track slugs. Every consumer reads
the resolved environment of the current track instead of the two numbers. With only `core.*` data
loaded the game is lockstep-identical to retail.

Ghidra HTTP (127.0.0.1:8080) was not reachable while writing this. Table values below were read from
the Steam `SWEP1RCR.EXE` `.data` section on disk (plaintext; only `.text` is SteamStub-encrypted)
with a PE walk on 2026-11-27; code claims cite the faithful bodies in `src/`.

## 1. Boundary

All functions below have full faithful bodies in `src/` (no `HANG`).

**Identity and selection (hangar)**

| Address | Ghidra name | Role |
|---|---|---|
| 0x004360e0 | `swrObjHang_DrawTracks` | Track list per circuit (swrObj.c:64) |
| 0x004368a0 | `swrObjHang_MainMenu` | Shows the selected track's name |
| 0x0043b0b0 | `swrUI_Front_HandleCircuits` | Circuit paging (swrUI.c:4798) |
| 0x0043b240 | `swrObjHang_CourseSelectionMenu` | `track_index = g_aTrackIDs[circuit*7 + sel]` (swrRace.c:2245); planet sprite `PlanetIdx + 0x45`, planet name from `swrPlanetTable` (2293/2298) |
| 0x0043b880 | `swrObjHang_CourseInfoMenu` | Holo planet + track preview, favourite pilot, records |
| 0x00440620 | `swrUI_Front_GetTrackNameFromId` | 25-case `switch` over `SCREENTEXT_497..` (swrUI.c:5031) |
| 0x00440a00 / 0x00440a20 / 0x00440aa0 / 0x00440af0 | `swrObjHang_GetRequiredPlaceToProceed` / `IsTrackUnlocked` / `IsTrackPlayable` / `VerifySelectedTrack` | Career gating (swrObj.c:155-205) |
| 0x004584a0 | `swrObjHang_InitTrackSprites` | Track preview images (sprite slots 99+id) |
| 0x00456c70 | `swrObjHang_DrawTrackPreview` | Holotable model `swr_sceneModels[0x62 + trackId]` + per-track scale `switch` (swrObj.c:2112-2180) |
| 0x00456800 / 0x004565e0 | `swrObjHang_DrawHoloPlanet` / `OrderHoloRacerIcons` | Planet hologram, sun/moons; `planetIdx == 6` special |
| 0x0045ab50 | `swrObjHang_Init` | Fills `swrPlanetTable[8]` (names, sun/moon ranges, per-boot random spin) |
| 0x00457c20 | `swrUI_Front_LoadPlanetModels` | Hologram models into `swrPlanetHoloModelNodes[8]` |
| 0x00457410 | `swrObjHang_LoadScreen` | Pre-race cinematic by `PlanetIdx`; `planetIntrosSeen` bit (swrObj.c:3089-3093) |

**Race setup (the seam where a track becomes numbers)**

| Address | Ghidra name | Role |
|---|---|---|
| 0x0045b290 | `swrObjHang_StartRace` | Copies the row into the `'Begn'` event (swrObj.c:3821-3826); record slot `track_index*2 + mirror` (3836); silent fallback to Mon Gazza Speedway if model/spline is -1 (3845-3851); MP broadcast |
| 0x00463a50 | `swrObjJdge_F4` 'Begn' | `jdge->planetId = subEvents[3]` (swrObj.c:7098) |
| 0x00466bd0 | `swrObjJdge_InitTrack` | Region-grid layout by `(planet, sub)` (swrObj.c:8190-8195); splitscreen wide FOV on planet 6 (8187) |
| 0x00465510 | `swrObjJdge_LoadTrackModels` | Planet 4 sub 0 loads `beachanim` into `someRootNodeChildNodes[2]` (swrObj.c:7650) |
| 0x00464b90 | `swrObjJdge_SetupTrackEnvironment` | Fly-by spline `camSplines[8][4]`, then fog / draw distance / clear colour ladder (swrObj.c:7435-7603); planet 4 sub 1/2 node-flag flip |
| 0x004667e0 | `swrObjJdge_InitAISettingsForTrack` | Function-local `float aiTable[64]`, AI script and spline variant by `(planet, sub)` (swrObj.c:7805-7870) |
| 0x00463ff0 | `swrObjJdge_SetPlanetIdAndTrackNumber` | Sets `PlanetID` 0x0050ca8c / `PlanetTrackNumber` 0x0050ca90 |
| 0x00464010 / 0x00464630 | `swrPlayerHUD_SetupTrackOverlay` / `swrPlayerHUD_LoadRaceHUD` | Sun, lens flare, weather setup per planet (swrPlayerHUD.c:136-297; LoadRaceHUD's `hudType/weatherLevel` are planet/subtrack) |
| 0x00463ec0 / 0x0042c2e0 | `swrObjJdge_SetupLensFlareSprites` / `swrSprite_SetSunSprite` | Sun placement |

**Per-frame consumers of the two numbers**

| Address | Ghidra name | Keyed decision |
|---|---|---|
| 0x0045ea30 | `swrObjJdge_F2` | Ando Prime blizzard escalation on line crossing, P1's lap only (swrObj.c:6052-6085) |
| (swrObj.c:6223) | `swrObjJdge_DrawBoostDialHud` | Planet 0 sub 0 "Boost" hint; **draws `swrUtils_Rand`** (6298-6302) |
| (swrObj.c:6429) | `swrObjJdge_DrawRaceHUD` | Minimap density 3.0 on planet 1 sub 3 (6556) |
| (swrObj.c:6723) | `swrObjJdge_IsRacerRacing` | Planet 1 sub 3 "not racing" zone (`swrObjJdge_notRacingZone*` 0x004ad464..70) (6729) |
| 0x00463a50 | `swrObjJdge_F4` | `swrSound_SelectPlanetIntroMusic(planetId)`; planet 3 sub 1 light-streak param (7130) |
| 0x00427b20 | `swrSound_UpdateEngineAudio` | Ambient cue list `swrSfxPreloadSets[planet*3+sub]`; Baroonda random calls **draw `swrUtils_Rand`** (swrSound.c:1573-1586) |
| 0x00427ea0 / 0x00427d90 | `swrSound_SelectTrackMusic` / `swrSound_PreloadSoundSet` | Music table, intro table, cue preload; sub 3 special-cased (swrSound.c:2224-2290) |
| 0x0046fd60 | `swrRace_SpawnGroundDustKick` | `switch (PlanetID)` x surface bits -> dust colour (swrRace.c:8055) |
| 0x0047ddc0 | `swrObjTrig_LoadAndInitializeTriggerModels` | Planet if-chain — **owned by swrObjTrig**, referenced only |

**Owned tables** (values: EXE `.data`, 2026-11-27)

| Global | Address | Type | Retail content |
|---|---|---|---|
| `g_aTracksInCircuits` | 0x004bfee0 | `char[4]` | 7, 7, 7, 4 |
| `g_aTrackInfos` | 0x004bfee8 | `TrackInfo[25]` (stride 0xC) | e.g. #0 {115, 74, sub 0, planet 0, fav 2}, #2 {130, 11, 0, 1, 4}, #24 {315, 60, 2, 7, 2} |
| `g_aTrackIDs` | 0x004c0018 | `int[28]` | 0,16,2,6,22,19,17 / 7,3,23,9,18,12,8 / 20,24,13,4,10,14,1 / 5,11,21,15,-1,-1,-1 |
| `swrMusicTrackTable` | 0x004b8750 | `int16[8][3]` | planet 0 sub 2 = -1; no column for sub 3 |
| `swrMusicPlanetIntroTable` | 0x004b8780 | `int16[12]` | 164, 34, 162, 164, 165, 165, 163, 165, then -1 x4 |
| `swrPlanetIntroCinematics` | 0x004b7a08 | `char*[8]` | ptrs 0x4b7ac4..0x4b7a70 (`PlanetTAT.znm` ..) — named on PR #316 only |
| `swrSfxPreloadSets` | 0x004b8fa8 | `swrSfxCue*[24]` | + `swrSfxCuesAndoPrimeTrack3` / `swrSfxCuesBaroondaTrack3` |
| `swrPlanetTable` | 0x00e98f40 | `swrPlanetInfo[8]` (0x5c) | runtime-filled by `swrObjHang_Init` |
| `swrPlanetHoloModelNodes` | 0x00e299f4 | `void*[8]` | runtime |
| function-local | 0x00464b90 / 0x004667e0 / 0x00456c70 | `camSplines[8][4]`, fog ladder, `aiTable[64]`, preview scale `switch` | in code |
| unlock state | 0x00e35a84 / 0x00e35a8a / 0x00e364ac | `char[6]` / `uint16[4]` / `uint8[4]` | per-circuit bitmasks |
| `swrMultiplayer_trackSelect` | 0x00ea02b0 | `swrRace_TRACK` | sent as `payload[20]` (swrMultiplayer.c:2208/2238) |

Seams:
- In: hangar menus (selection), `StartRace` -> `'Begn'` -> judge F4 (the one place the row becomes
  `jdge->planetId` / `planet_track_number`), MP roster/track select, refined `race_timing`
  (reads `g_aTrackIDs` / `g_aTracksInCircuits` for tournament progression,
  race_timing_refined.c:557-704).
- Out: `swrLoader` block reads (model/spline/texture), `swrSound_*`, `swrWeather_*`, `swrSprite_*`,
  `rdModel_SetFogEnabled` / `swrRender_SetFogParameters`, AI globals (`swrRace_AILevel`,
  `ai_spread`, `ai_track_script`, `track_spline_variant`), `swrObjTrig_regionGridLayout`.
- Lifetime: `track.before_load` / `track.unload` / `track.loaded` (core event bus, today
  track_lifecycle's tables).

## 2. Tier table

| Function / data | Tier | Reason |
|---|---|---|
| `StartRace` row read, `'Begn'` copy, judge F4 | **sacred** | Feeds spawn, AI, MP broadcast; RNG draws (credits random track) in order |
| `InitAISettingsForTrack` | **sacred** | AI level/spread/script/spline variant drive AI steering |
| `IsRacerRacing` zone, region-grid layout in `InitTrack` | **sacred** | Racing status and trigger activation |
| `DrawBoostDialHud` boost hint, `UpdateEngineAudio` Baroonda calls | **sacred** (RNG only) | Cosmetic output, but they draw the shared `swrUtils_Rand` stream; draw count and order must stay |
| `SpawnGroundDustKick` | **sacred** until proven | Colour is cosmetic; not checked whether the spawned dust objects draw RNG or feed anything sim (uncertain = sacred) |
| `SetupTrackEnvironment` fly-by part | equivalent | Already refined in track_lifecycle; unchanged here |
| `SetupTrackEnvironment` fog / draw distance / clear colour | equivalent | `drawDistance` is only read by `swrViewport_SetCameraParameters` (swrObj.c:7284); texture-hash oracle |
| `SetupTrackOverlay`, `F2` weather, sun/flare | equivalent | Weather particles are rendered by our GL path; no RNG in these blocks (swrPlayerHUD.c:239-297, swrObj.c:6052-6085) |
| Music / intro / cue preload / cinematic | equivalent | Audio and video only (cue *playback* RNG is in the sacred row above) |
| Menus, names, previews, holo planets, `swrObjHang_Init` planet fill | equivalent | UI; but `swrObjHang_Init` draws `swrUtils_Rand` per planet for holo angles and spin (swrObj.c:4876-4886); keep count and order |
| Unlock / required place / playable | equivalent | Save-visible; save image must stay byte-identical |
| Record slots in `swrSaveData` | equivalent | Save format is sacred-as-data: stock tracks keep their 50 slots |

Sim-affecting vs cosmetic, per field (feeds the core fingerprint, section 4.5): geometry, spline,
AI fields, region grid, not-racing zone, favourite pilot (picks a tournament opponent,
swrObj.c:3968), trigger lists (swrObjTrig), and anything drawing the game RNG (ambient random calls,
boost hint, dust until proven) are `REG_SIM`. Fog, draw distance, music, cinematic, sun, weather,
names, holo, preview are cosmetic.

## 3. Flaw inventory

| Tag | Address / place | Finding | Refinement |
|---|---|---|---|
| HARDCODE (H1) | 0x00464b90 | Fog, draw distance, clear colour as a nested `if` ladder on `(planet, sub)`; plus a planet 4 sub 1/2 node-flag flip | Env fields with dumped retail values; flip becomes `env.hide_root_child0` |
| HARDCODE (H2) | 0x004667e0 | `aiTable[64]` on the stack; AI script and spline variant by `if` ladder | `env.ai.{level, spread, script, spline_variant}` |
| HARDCODE (H3) | 0x0045ea30 / 0x00464010 | Weather is control flow: setup by planet, escalation by sub and line crossing, sun alpha as a side effect | `env.weather.{color, stretch, stages[]}`; stock Ando stages dumped 1:1 |
| HARDCODE (H4) | swrObj.c:6298, 6556, 6729, 7130, 7650, 8187-8195 | Six one-off `(planet, sub)` quirks: boost hint, minimap density, not-racing zone, light-streak param, beach anim, wide FOV, region grid | One named env field each (section 4.2); no planet test left in game code |
| HARDCODE (H5) | 0x00456c70 | Preview scale `switch(trackId)` over 25 ids; preview model slot `0x62 + trackId` | `track.preview.{scale}`; later spline-generated preview (CUSTOM_TRACKS 4.4 step 5) |
| HARDCODE (H6) | 0x00440620 | Track names as a 25-case `switch` | `track.name` (text id or literal) |
| HARDCODE (H7) | 0x00440a00; tracks_delta.c:574 | Required place (top 4 / top 3) and circuit colours in code | `circuit.required_place[]`, `circuit.color` |
| LIMIT (L1) | `g_aTrackInfos[25]`, types.h:750 | Fixed table; `hang->track_index` is a signed `char` (127 tracks max); band-aid table `MAX_NB_TRACKS` 98 | Registry-sized table; index range checked against `char` until the hangar is refined |
| LIMIT (L2) | `swrPlanetTable[8]`, `SWR_PLANET_COUNT`, sprite 69..76, holo nodes [8], intro/cinematic [8] | Planet cap of 8 everywhere | Planets are presets; identity read from the current track's env (section 4.3), no slot inflation |
| LIMIT (L3) | 0x00440a20 / 0x00440aa0 | Unlock state = one byte per circuit, 2 bits per track (`uint16`): <= 8 tracks per circuit, 4 circuits | Custom circuits keep their progress in a sidecar, keyed by slug |
| LIMIT (L4) | 0x004b8750 | Music table has 3 subtrack columns; sub 3 hard-coded to 0x92 / 0x96 for planets 1 / 4, nothing else (swrSound.c:2229-2240) | `env.music` per track |
| UNSAFE (U1) | swrSound.c:1581-1586, 2277-2284 | Subtrack 3 on a planet other than 1/4: `UpdateEngineAudio` reads the bits of the float `lapProgress` as a cue-list pointer, `PreloadSoundSet` casts the planet int to a pointer. Unreachable on stock data, reachable by any custom track claiming sub 3 | Cue list from the env, never derived from the numbers |
| UNSAFE (U2) | swrObj.c:3836-3842 | `StartRace` reads `recordLapTimes[track_index*2 + mirror]` past the 50 slots for every custom track (the write side is guarded by the M0 pre-latch, swrRace_delta.cpp:335; the display guard uses 28 not 25, tracks_delta.c:1193) | Records looked up by slug; stock slugs map to their save slots |
| UNSAFE (U3) | custom_tracks.cpp:435 | `fixup_n64_display_list` calls `std::abort()` on an unknown display-list command at load | Validate at scan, reject the pack with a log line |
| SILENT (S1) | swrObj.c:3845-3851 | A row with model or spline -1 silently races Mon Gazza Speedway | Unresolvable track refused in the menu, logged |
| TANGLE (T1) | 0x00464b90 | Fog tail sits after the fly-by early return: a track with no cam spline keeps the previous track's fog and draw distance (all 25 stock pairs have one, so retail never shows it) | Fog applied independently of the fly-by |
| TANGLE (T2) | everywhere above | `(planet, sub)` is the key for ~20 unrelated decisions; a custom track borrowing a pair inherits all of them (CUSTOM_TRACKS 4.2) | One resolved `TrackEnv` per load; consumers read fields |
| TANGLE (T3) | swrMultiplayer.c:2208/2238 | MP sends the numeric track index; custom indices depend on install order | Handshake on `(slug, content_hash)`; index is per-session |
| GLOBAL (G1) | PR #316 `track_env_ApplyTables` | Band-aid writes the current track's values into retail tables and restores them on change, plus a per-frame re-apply on the select page for the hovered track | Readers take the env; tables untouched |
| DUP (D1) | custom_tracks.cpp vs track_registry.cpp | Two registration paths (legacy `>= 420` id range with block-path swap; manifest tracks reusing stock block indices with views), two name tables, two "current track" notions | One registry; legacy folders become synthesized manifests at scan |
| DUP (D2) | `DrawTrackPreview`, `SetupTrackEnvironment`, `LoadTrackModels` | `planet_track_number` picks the start node / visible nodes by a hand-written `if` per value in several places | One `track.start_node` / visibility field |
| CLEVER (C1) | tracks_delta.h:13-18 | `DEFAULT_NB_TRACKS 28` is the menu grid (4 x 7), not the track count (25); `DEFAULT_NB_CIRCUIT` = 7 is "tracks per circuit", `..._PER_TRACK` = 4 is the circuit count. Rows 25..27 of `g_aNewTrackInfos` are zero-filled holes (custom_tracks.cpp:301 copies 25) | Names that mean what they count; no holes |
| CLEVER (C2) | swrObj.c:6055 | Weather stages follow player 1's lap only | Kept (equivalent); documented as `stages.by = "p1_lap"` |
| HARDCODE (H8) | custom_tracks.cpp:121-124 | Legacy packs get `planet 1, sub 0, favourite 2` for every track | Synthesized manifest inherits a declared or default preset |

## 4. Target shape

### 4.1 Registries (core section 3.1)

| Registry | Name | Record | Scope |
|---|---|---|---|
| `track` | `core.track.<slug>` / `<pack>.<slug>` | identity, assets, env overrides, preview, rules | session (packs at boot) |
| `env_preset` | `core.planet.<name>`, `core.env.<track>` | any subset of `TrackEnv` fields | session |
| `circuit` | `core.circuit.amateur` .. | ordered track names, colour, required places, unlock rule | session |

- **Identity:** the name is the slug. Retail ids map to `core.track.*` names through a fixed table
  (`swrRace_TRACK` order). Aliases `vanilla:track:NN` and `vanilla:<planet>` (published manifests,
  times sidecar) resolve to the same records.
- **Runtime index:** the registry allocates `track_index` at boot (stock 0..24 fixed, packs after,
  stable for a given pack set, `<= 127` while `track_index` is a `char`). Nothing persists an index.
- **The row table becomes a projection.** Faithful readers still index `g_aTrackInfos` (StartRace,
  menus, refined race_timing). The registry owns a growable `TrackInfo[]` built from the records and
  points the global at it (`standalone_global_rebind`, already how custom tracks work in swe1r). For
  manifest tracks `PlanetIdx` / `planetTrackNumber` in the row are the *inherited preset's* numbers,
  kept only for faithful readers not yet moved to the env.

### 4.2 `TrackEnv`: the resolved environment

Resolved once on `track.before_load`: core defaults < planet preset < inherited track preset <
track's own fields. Consumers (refined bodies of the section-1 functions) read
`track_env_current()`; none tests a planet number. Field set = PR #316's `TrackEnv` (prior art,
unmerged on `feature/track-env`) plus the quirks it left in code:

| Field | Retail source | Sim |
|---|---|---|
| `spline`, `model`, `textures` (asset refs) | `g_aTrackInfos`, blocks | yes (geometry) |
| `start_node` (0..3) | `planet_track_number` uses in `SetupTrackEnvironment` / `LoadTrackModels` | yes (visibility of shared nodes) |
| `ai.{level, spread, script, spline_variant}` | `aiTable[64]`, swrObj.c:7829-7860 | yes |
| `region_grid_layout` (0/1/2) | swrObj.c:8190-8195 | yes (triggers) |
| `not_racing_zone` {min, max, behaviour bit 8} | swrObj.c:6729 + 0x004ad464..70 | yes |
| `favorite_pilot` | `TrackInfo.FavoritePilot` | yes (tournament roster) |
| `ambient.{cues[], random_call}` | `swrSfxPreloadSets`, Baroonda block | random_call draws game RNG |
| `boost_hint` (bool) | swrObj.c:6298 | draws game RNG |
| `dust_palette` | swrRace.c:8055 `switch` (7 surface bits, names unknown) | until proven |
| `fly_by.spline` | `camSplines[8][4]` | no (track_lifecycle) |
| `fog.{enabled, near, color}`, `draw_distance`, `hide_root_child0` | swrObj.c:7502-7603 | no |
| `music`, `intro_music`, `cinematic` | 0x004b8750, 0x004b8780, 0x004b7a08 | no |
| `weather.{color, stretch, stages[]}`, `sun`, `lens_flare` | swrPlayerHUD.c:136-297, swrObj.c:6052 | no |
| `minimap_density`, `light_streak_param`, `beach_anim`, `splitscreen_wide_fov` | swrObj.c:6556, 7130, 7650, 8187 | no |
| `planet_identity.{name, holo_model, tilt, spin, sun_moons}` | `swrPlanetTable`, holo nodes | no |
| `triggers.preload`, trigger lists | swrObjTrig (its section 4.4) | yes — swrObjTrig owns them |

Defaults are dumped from the compiled tables (core 3.2): the dumper runs the *faithful* ladders for
all 8 x 4 pairs and writes `core.planet.*` / `core.env.*`, so the data cannot drift from code.

### 4.3 Planets are presets

`core.planet.ando_prime` holds what is truly per planet (intro music, cinematic, sun, holo identity,
dust palette, trigger preload); per-track ladders (fog by sub, AI by sub) live in `core.env.<track>`.
A custom track can name a planet preset, inherit a stock track, or define everything inline. Identity
consumers (`CourseSelectionMenu` name/sprite, `DrawHoloPlanet`, `LoadScreen` cinematic) read
`env.planet_identity` of the shown track; the galaxy map keeps the 8 stock planets
(CUSTOM_TRACKS 4.5 decision). `planetIntrosSeen` is a profile bitmask keyed by planet index
(swrObj.c:3093): a custom planet keys its "seen" flag by slug in the sidecar.

### 4.4 Circuits

`circuit` records list track names in order. Stock: `core.circuit.amateur` = boonta_training,
mon_gazza_speedway, beedos_wild_ride, aquilaris_classic, malastare_100, vengeance, spice_mine_run
(`g_aTrackIDs` row 0). The menu maps (circuit, slot) -> track through the record, replacing
`circuit*7 + slot` arithmetic (tracks_delta.c:730-734) and the synthetic "Custom Tracks Page N"
circuits; the retail `g_aTrackIDs` / `g_aTracksInCircuits` stay as the projection for faithful
readers. Stock progression keeps the save bitmasks; pack circuits keep progress in a sidecar.

### 4.5 Fingerprint and times

- Fingerprint (core 3.4) covers the `REG_SIM` fields of the current track's resolved env plus the
  asset hashes (`content_hash`); cosmetic fields are excluded, so a music or fog pack keeps a stock
  track retail-comparable.
- Times key on `(slug, content_hash, mirror, laps)` (TIMES_ROADMAP / PR #313). Stock tracks still
  write their `swrSaveData` slots (save format unchanged); everything else goes to the sidecar. The
  record lookup in `StartRace` goes through the same function (fixes U2).
- MP: the lobby exchanges `(slug, content_hash)` and the fingerprint; `payload[20]` stays an index,
  valid only after both peers resolved the same slug (T3).

### 4.6 Example: stock track as core data (dumped, not hand-written)

```json
"core.track.beedos_wild_ride": {
  "retail_id": 2, "name": "/SCREENTEXT_499/~~Beedo's Wild Ride",
  "model": { "block_index": 130 }, "spline": { "block_index": 11 },
  "favorite_pilot": "core.pilot.aldar_beedo",
  "env": {
    "inherit": "core.planet.ando_prime",
    "start_node": 0,
    "ai": { "level": 8.784, "spread": 35.0, "script": 1, "spline_variant": 1 },
    "region_grid_layout": 2,
    "fly_by": { "spline": "SPLINEID_planeta_track6came" },
    "fog": { "near": 996, "color": "#d2d7e1" }, "draw_distance": 11000,
    "music": 142,
    "weather": { "color": [230, 230, 255, 200], "stretch": 1.0, "stages": [
      { "at": "setup",           "cap": 0 },
      { "at": "p1_line_cross_0", "cap": 20, "velocity": [25, 45] },
      { "at": "p1_line_cross_n", "cap": 60, "velocity": [20, 60], "sun_alpha": 128 } ] }
  },
  "preview": { "scale": 1.2 }
}
```
(`core.planet.ando_prime`: intro 34, cinematic `PlanetA.znm`, sun, trigger preload.) The weather
`setup` stage stands for the planet-1 branch of `SetupTrackOverlay`; exact field split is decided
when the dumper is written.

### 4.7 Example: custom track pack

```json
{
  "schema": 2, "slug": "lightningpirate.mute-city", "version": "1.2.0", "name": "Mute City",
  "model":  { "sha256": "ab12...", "format": "RAWM", "block_index": 115 },
  "spline": { "sha256": "cd34...", "format": "RAWS", "block_index": 74 },
  "textures": [ { "sha256": "ef56...", "format": "RAWT", "block_index": 1648 } ],
  "env": {
    "inherit": "core.track.boonta_training",
    "fog": { "near": 992, "color": "#5a2878" }, "draw_distance": 3000,
    "music": "track:mute_city_theme",
    "weather": false,
    "planet_identity": { "name": "Big Blue", "holo": { "tilt": 0.3, "spin": 4.0 } }
  },
  "circuit": { "name": "lightningpirate.fzero_cup", "slot": 0 },
  "rules": { "point_to_point": false },
  "data": [ "triggers.json" ],
  "content_hash": "9f77..."
}
```
`env.ai` absent = inherited from Boonta Training -> pack is retail-comparable on AI; a
`triggers.json` with non-core actions makes it not (swrObjTrig 5.1).

Name map (refined <- Ghidra <- address), one refined slot per row of section 1 that gets a refined
body; listed in `refined.c` as for other subsystems. Planned: `refined_swrObjJdge_SetupTrackEnvironment`
(extends track_lifecycle's), `refined_swrObjJdge_InitAISettingsForTrack` 0x004667e0,
`refined_swrSound_SelectTrackMusic` 0x00427ea0, `refined_swrSound_PreloadSoundSet` 0x00427d90,
`refined_swrSound_UpdateEngineAudio` 0x00427b20, `refined_swrPlayerHUD_SetupTrackOverlay` 0x00464010,
`refined_swrUI_Front_GetTrackNameFromId` 0x00440620, `refined_swrObjHang_DrawTrackPreview` 0x00456c70,
menus 0x0043b240 / 0x0043b880 / 0x0043b0b0 / 0x004360e0, `refined_swrObjHang_StartRace` 0x0045b290
(record lookup only; rest verbatim).

## 5. Band-aids retired (swe1r)

| Band-aid | Where | Replaced by |
|---|---|---|
| `g_aNewTrackInfos[98]` + `standalone_global_rebind` / `patch_trackInfos_Usages` (5 `.text` operand scans) | custom_tracks.cpp:251-308 | Registry-owned projection (DLL keeps the patch, as other refined subsystems do) |
| Hash-diff detection of whole-block packs, block-path swap, `>= 420` id range | custom_tracks.cpp:82-247, 342-366, 476-531; `swrModel_LoadFromId_delta`, `swrSpline_LoadSplineById_delta` | Legacy folder -> synthesized manifest at scan; all tracks bind through virtual block views on `before_load` |
| Hard-coded `planet 1 / sub 0 / pilot 2` | custom_tracks.cpp:121-124 | Inherited preset |
| `overrides_stock_slot` / `placement` | track_manifest.cpp:152-158, track_registry.cpp:48-51 | `env.inherit` (read for old manifests) |
| `g_aCustomTrackNames`, name delta | tracks_delta.c:1444 | `track.name` |
| Circuit paging, `circuit*7+slot`, colour `switch`, `IsTrackPlayable_delta` circuit >= 4 | tracks_delta.c:33-58, 406-528, 574, 730, 1512 | Circuit registry |
| Preview sprite slots 256+ | tracks_delta.c:71-93 | Allocated per track by the registry |
| Record guard pre-latch, display guard `< 28` | swrRace_delta.cpp:335-355, tracks_delta.c:1193 | Times by slug (U2) |
| `InitAISettingsForTrack` hook | ai_tuning.cpp:383 | Env fields; ai_tuning becomes a schema'd option on top |
| PR #316 table patch / restore, hovered-track re-apply, weather/sun/dust/trigger-planet deltas | `feature/track-env` (unmerged) | Consumers read the env; no retail table written |
| `fixup_n64_*` at load with `abort()` | custom_tracks.cpp:368-470 | Validation at scan (U3); fixups applied to the pack's own buffers |

## 6. Oracle plan

| Step | Check |
|---|---|
| Registry + projection (no consumer moved) | Lockstep `sa3_t0..24` 25/25 and `seqS1v3` / `seqS2v3` / `seqC1v3`, refined on vs off; projected `TrackInfo[0..24]` byte-equal to retail `.data` |
| Dumper | `load(dump(defaults)) == defaults`; dumped env for all 25 stock tracks equals a probe of the faithful ladders (fog/AI/music/weather globals logged after each load) |
| Sim consumers (AI, region grid, zone, boost hint, ambient RNG) | Same lockstep set, one consumer per commit; RNG draw count per race unchanged |
| Cosmetic consumers (fog, weather, sun, music, preview, names) | Texture-hash oracle per stock track; audio: logged music/cue ids per race identical; screenshots of course select / info for 25 tracks |
| Custom tracks | `seqC1v3` (stock -> custom -> stock) plus a pack with sub-3 / planet-5 inheritance (U1 path): no fault, env as declared |
| Times / MP | Sidecar round trip; StartRace on a custom track reads no out-of-range slot (address log); MP: two-process lobby with differing pack order picks the same track |

Gaps: (1) the RNG-drawing consumers (boost hint on Boonta Training, Baroonda calls) only fire
under conditions idle lockstep pods may not reach in 1500 frames — log their draw count per race to
prove coverage; (2) tournament progression and unlocks are not exercised by lockstep (they run in the
hangar); needs test-runner plans; (3) weather stages after lap 1 need autopilot recordings, the same
firing-oracle gap swrObjTrig has; (4) no MP oracle beyond a manual two-process run.

## 7. Step plan

1. **S — Names and holes.** `C1` renames in the hook layer; drop rows 25..27; name the two
   cutscene masks if confirmed. No behaviour change.
2. **M — Track registry + projection** on core registries (core steps 2-3). Stock records from the
   EXE tables; legacy folders and manifests both register through it. Retires D1, the operand scan
   in swe1r. Lockstep + sequences.
3. **M — Env dumper + `TrackEnv` resolve on `before_load`** (core step 5 for packs). Readers not
   moved yet; a debug probe compares env to the faithful ladders on every load.
4. **L — Move cosmetic consumers** one per commit: fog/draw distance (fixes T1), music/intro/cues
   (fixes U1, L4), cinematic, sun/flare, weather, names, preview, holo identity. Texture-hash and
   audio-id oracles. Retires PR #316's table patching.
5. **M — Move sim consumers:** AI, region grid, not-racing zone, favourite pilot, boost hint and
   ambient RNG, dust (after proving its RNG use). Lockstep per commit; fields tagged `REG_SIM` for
   the fingerprint (core step 4).
6. **M — Circuits registry** and menus; pack circuits and their progress sidecar.
7. **S — Times by slug** in `StartRace` / results (U2); MP slug handshake (T3, with the MP work).
8. **S — Retire band-aids** in swe1r (section 5); legacy packs only via synthesized manifests.

Depends on: core steps 1-5 before step 2-3 here; swrObjTrig step 3 (per-planet preload as data)
before `triggers.preload` leaves the planet chain; track_lifecycle events already in place.

## Open questions

- Slug scheme for stock content: `core.track.<name>` with `vanilla:track:NN` as alias, or keep
  `vanilla:` as the canonical namespace (it is already in published manifests and times files)?
- Galaxy map: list custom planets (needs the cap lifted in the hangar UI) or stay at 8 forever?
- `track_index` as `char`: refine the hangar struct to `int`, or cap packs at 102 tracks?
- Custom circuit progress: sidecar per profile, or extend the save image (breaks retail saves)?
- Dust: does `SpawnGroundDustKick`'s output reach the sim or draw RNG? Decides its tier.
- Location (roadmap 9.1), as for the rest of `refined/`.
