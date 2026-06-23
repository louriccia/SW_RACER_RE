# Custom Content / Modding Architecture

**Status:** design (2026-06-10). Living document. Owner: lightningpirate.

Goal: make it dead simple to create, share, and play modded game entities — tracks,
planets, circuits, racers/pods, and campaigns — by moving hard-coded data out of the
EXE into editable game files, and (eventually) wiring a create -> upload -> download ->
play loop through an external website/API.

This is a **delta on understood behavior**: it lives in the `dinput_hook/` Microsoft
Detours layer (the live mod layer), **not** in the `src/` faithful reimpls. `src/` stays
a clean decomp; `dinput_hook/` owns all data externalization.

---

## 1. Locked decisions (2026-06-10)

| Decision | Choice | Consequence |
|---|---|---|
| Manifest format | **JSON** | Web/API consume it directly; vendor a single-header JSON lib (e.g. nlohmann/json) into the delta layer. |
| Vanilla data handling | **Overlay / overridable** | Vanilla entities are loaded into the model as defaults; mods can ADD new entities or OVERRIDE fields of existing ones by slug. Players can retune a stock track's AI/music, not just add new tracks. |
| First vertical slice | **Phase 0 — track metadata** | Build on the ~60%-done track system; prove the model->bind pipeline cheaply. |
| Online distribution | **Manual for now** | Download page + drop the package into the assets folder + rescan. No client code yet; an in-game ImGui browser is the most likely future client. |

---

## 2. The big picture: tracks are already ~60% done, *as a pattern to generalize*

`dinput_hook/game_deltas/tracks_delta.c` (~1532 lines) + `dinput_hook/custom_tracks.cpp`
already implement custom-track geometry, spline, name, and menu enumeration. More
importantly they establish **three reusable binding techniques** that the rest of this
system is built on:

1. **Inflate-the-table** — a fixed global array is replaced by a larger heap array, and
   *every code reference to the old base address is rewritten* by scanning `.text`.
   - Example: `TrackInfo[25] @ 0x4bfee8` -> heap `g_aNewTrackInfos[98]`, via
     `patch_trackInfos_Usages` (scans `.text` 0x401000-0x4AB800, swaps the immediate).
   - Use for: any fixed global table (racer stats, planet name buffer, etc.).

2. **Hook-the-reader** — when data is baked into a *function body* (switch / fn-local
   array) there is no table to inflate, so detour the function and serve from the model.
   - Example: `swrUI_GetTrackNameFromId @ 0x440620` (id switch) is detoured;
     ids >= 25 read `g_aCustomTrackNames`.
   - Use for: spline/fog switch, AI-difficulty array, music selection, etc.

3. **Asset-id redirect** — model/spline/texture/sound payloads are loaded by numeric id
   from packed `.bin` blocks; redirect the block path pointers and remap the ids.
   - Example: `custom_tracks.cpp` swaps the hardcoded block path ptrs at
     `0x4B9598` (model) / `0x4B9590` (spline) / `0x4B9594` (texture) and remaps custom
     ids into the `>= 420` range. Loose per-asset loaders (textures, models, fonts)
     already exist in the delta layer; extend the same pattern to audio.

**The whole project is "apply these three techniques to every remaining attribute and
entity type."** That classification *is* the engineering task list (section 5).

---

## 3. Architecture spine: one canonical model, populated then bound

```
  assets/mods/<mod>/manifest.json + asset files
            |  (1) SCAN + PARSE
            v
  +---------------------------------+
  |  Content Registry (delta layer) |   owns the in-memory canonical model:
  |  - seed vanilla defaults        |   Track / Planet / Circuit / Racer / Campaign,
  |  - merge mod manifests (overlay)|   each keyed by a STABLE STRING SLUG.
  |  - allocate runtime numeric ids |   Numeric game ids are an allocation detail.
  |  - resolve cross-refs (slug->id)|
  +---------------------------------+
            |  (2) BIND  — per reader, choose one technique:
            +-- inflate-the-table   (fixed global arrays)
            +-- hook-the-reader     (switch / fn-local data)
            +-- asset-id redirect   (model/spline/texture/sound blocks)
            v
        Game reads the model transparently
            |
   Menu layer:   native track/planet/racer selection enumerates the model
   Online layer: website API <-> (future) in-game browser <-> assets/ + rescan
```

Two principles that shape everything downstream:

- **Key on stable string slugs, not numeric ids.** The `>= 420` custom ranges, planet
  index, racer index, sprite slots, sound ids are allocation details the registry hands
  out. A track is referenced everywhere (save profiles, campaign manifests, online
  catalog) as e.g. `"my-cool-track"`, which resolves to whatever numeric id is free in
  the current install. Raw numeric ids would break the moment the installed mod set
  changes; slugs do not.

- **Cross-entity references resolve at bind time.** A track manifest names its planet /
  circuit / favorite-racer / music by slug. A "planet pack" mod can ship a planet + 3
  tracks + a circuit together. The registry resolves the dependency graph and allocates
  ids in dependency order. This is the same machinery the online layer needs for
  dependency download.

---

## 4. Manifest format (JSON)

A **mod** is a folder under `assets/mods/<mod>/` containing one `manifest.json` plus
asset files. One mod may declare multiple entities. Envelope sketch:

```jsonc
{
  "schema": 1,
  "mod": {
    "slug": "lightningpirate.boonta-remix",   // globally unique, namespaced
    "name": "Boonta Remix Pack",
    "author": "lightningpirate",
    "version": "1.0.0",
    "game_compat": ">=1.0",
    "depends": []                               // slugs of required mods (e.g. a planet)
  },
  "tracks": [
    {
      "slug": "boonta-remix",
      "name": "Boonta Classic (Remix)",
      "overrides": null,                        // or a vanilla track slug to OVERLAY
      "planet": "vanilla:tatooine",             // slug ref (vanilla or custom)
      "circuit": "vanilla:amateur",             // slug ref
      "track_in_circuit_order": 0,
      "favorite_pilot": "vanilla:anakin",       // slug ref
      "model_block": "out_modelblock.bin",
      "spline_block": "out_splineblock.bin",
      "texture_block": "out_textureblock.bin",  // optional
      "preview_model": "preview.bin",           // optional; else derive
      "music": { "intro": "intro.ogg", "race": "race.ogg" },  // pending RE (sec. 9)
      "ai": { "speed": 11.2, "difficulty": 38.0 },            // hooks InitAISettingsForTrack
      "fog": { "color": "#202830", "near": 1000, "far": 8000 },
      "sfx": []                                  // ambient/positional placements (pending RE)
    }
  ]
}
```

- **Overlay semantics:** every entity has a `slug`. If `overrides` names an existing
  (vanilla or custom) slug, the registry takes that entity as a base and applies only the
  fields present in this manifest. Vanilla entities are seeded with canonical slugs
  (`vanilla:tatooine`, `vanilla:amateur`, `vanilla:anakin`, `vanilla:track:00` ...).
- Asset paths are relative to the mod folder. Block files keep the existing
  `out_*block.bin` format so the blender-swe1r export pipeline still works unchanged.

---

## 5. Binding map (the RE-grounded work list)

Per attribute: where the game reads it today, and which technique frees it.
**Addresses below are from current Ghidra-DB exploration and MUST be reconfirmed against
the live DB at implementation time** (the EXE `.text` is SteamStub-encrypted on disk —
verify via Ghidra disasm or runtime, not file bytes).

### 5.1 Tracks  (DONE = already in tracks_delta.c / custom_tracks.cpp)

| Attribute | Read site | Technique | Status |
|---|---|---|---|
| Core record (id, spline id, planet idx, track-in-planet, favorite) | `TrackInfo[25] @ 0x4bfee8` | inflate (`g_aNewTrackInfos[98]`) | DONE |
| Geometry / spline / texture blocks | block path ptrs `0x4B9598/90/94` | asset-id redirect (id `>=420`) | DONE |
| Display name | `swrUI_GetTrackNameFromId @ 0x440620` (switch) | hook (`g_aCustomTrackNames`) | DONE |
| Menu enumeration / circuit pages | `GetCircuitCount`/`GetTrackCount`, `DrawTracks_delta` | hook | DONE |
| Planet assignment | `custom_tracks.cpp` hardcodes `PlanetIdx = 1` | manifest field -> inflated record | **Phase 0** |
| Track-in-planet number | hardcoded `0` | manifest field | **Phase 0** |
| Favorite pilot | hardcoded `2` | manifest field | **Phase 0** |
| AI speed / difficulty | `InitAISettingsForTrack @ 0x4667e0` (fn-local `float[62]`, idx `(subtrack + planet*4)*2`) | hook-the-reader | **Phase 0** |
| Spline id + fog/clear color | `swrObjJdge_SetupTrackEnvironment @ 0x464b90` (nested switch) | hook-the-reader | Phase 0/1 |
| Preview model | `DrawTrackPreview @ 0x456c70`, model-node array `@ 0xe29a88` | inflate + asset | Phase 0/1 |
| In-race music (per track) | `swrSound_SelectTrackMusic` reads `short[planet*3+subtrack]` table `@0x4b8750` -> `DAT_004b8744`; played channel 7 by per-frame `swrSound_UpdateMusic`. See sec. 9 for resolved values | hook selector + register custom wav | **resolved 2026-06-10** |
| Intro music (per planet) | `short[planet]` table `@0x4b8780`, read by `swrSound_PreloadSoundSet` | hook + register | **resolved 2026-06-10** |
| Ambient SFX (per track) | `swrSfxPreloadSets @0x4b8fa8[planet*3+subtrack]` -> list of `{startProgress, endProgress, soundIdx, flags}`; played by `swrSound_UpdateEngineAudio` keyed on lap progress (NOT XYZ). See sec. 9 | hook play+preload reads + register sfx wavs | **resolved 2026-06-10** |

### 5.2 Circuits

| Attribute | Read site | Technique |
|---|---|---|
| Track count per circuit | `g_aTracksInCircuits char[4] @ 0x4bfee0` | inflate |
| Track ordering per circuit | `g_aTrackIDs int[28] @ 0x4c0018` (idx `circuit*7 + slot`) | inflate |
| Names | `"Amateur"/"Semi-Pro"/"Galactic"/"Invitational"` ptrs `@ 0x4c0eb0/0e7c/0e48/0e10` | inflate/hook |
| Colors | switch on `circuitIdx` in `DrawTracks_delta` (4 hardcoded RGB) | hook |
| Unlock / completion | `swrRace_UnlockDataBase @ 0xe35a84`, `g_aBeatTracksGlobal @ 0xe364ac` | inflate (ties to campaigns) |

### 5.3 Planets  (HARD blocker: cap of 8 everywhere)

| Attribute | Read site | Technique |
|---|---|---|
| Name | `Tatooine_textbuffer char[8][92] @ 0xe98f5c` | inflate + kill `PlanetIdx < 8` guards |
| Holo model node | array `@ 0xe299f4` (by planet idx) | inflate + asset |
| Sun/moon sprite ranges + rotation | `@ 0xe98f40` stride `0x17` (`DrawHoloPlanet @ 0x456800`) | inflate |
| Sun/moon model nodes | `@ 0xe29a18` | inflate + asset |
| Preview sprite | sprite slots `69..76` (idx `planet + 69`) | inflate sprite slots |
| Planet+subtrack -> spline/fog | switch `@ 0x464b90` | hook (shared with tracks) |

Planets are the single largest table-inflation job: ~5 parallel arrays + literal
`PlanetIdx < 8` checks (e.g. course-selection menu) all need the inflate +
`patch_*_Usages` treatment together. Tackled in Phase 2.

### 5.4 Racers / pods  (no delta exists yet; clean inflate target)

| Attribute | Read site | Technique |
|---|---|---|
| Name/lastname ptrs, pod model id(s), pilot sprite, puppet model | `swrRacerData[23] @ 0x4c2700` stride `0x34` | inflate |
| Handling stats (15 floats: maxSpeed, accel, turn, traction, boost, heat, ...) | `PodHandlingData[23] @ 0x4c2bb0` stride `0x3c` | inflate |
| AI handling tuning | `PodHandlingData @ 0x4c3114` | inflate |
| Pilot sprite load | `swrObjHang_LoadAllPilotSprites @ 0x457bd0` | follows inflated table + asset |

`TrackInfo`, `swrRacerData`, `PodHandlingData` are already typedef'd in `src/types.h` —
the field layouts are known. Phase 3.

### 5.5 Campaigns / career

Career structure + unlock requirements live in `GetRequiredPlaceToProceed @ 0x440a00`,
`isTrackUnlocked @ 0x440a20`, `isTrackPlayable @ 0x440aa0`, backed by
`swrRace_UnlockDataBase @ 0xe35a84` / `g_aBeatTracksGlobal @ 0xe364ac`. Data-driving the
career order/unlock rules is Phase 4 (depends on circuits/tracks being data first).

---

## 6. ID allocation

The registry owns a deterministic allocator per id-type, handing out values above the
vanilla range. Allocation must be **stable for a given installed mod set** so save data
and cross-references stay valid.

| Id type | Vanilla range | Custom range | Notes |
|---|---|---|---|
| MODELID (track/preview/pod/planet models) | < 420 | `>= 420` | already used by custom tracks |
| SPLINEID | < 420 | `>= 420` | already used |
| Planet index | 0..7 | `>= 8` | requires inflating all planet arrays |
| Racer index | 0..22 | `>= 23` | requires inflating racer tables |
| Sprite slots (previews/UI) | various | 256..399 custom track slots used today | extend ranges per entity |
| Sound ids | TBD | TBD | pending music/sfx RE |

Slug -> id resolution happens once at bind; the map is exposed so menu and save code can
round-trip.

---

## 7. Phasing

| Phase | Scope | Risk | Depends on |
|---|---|---|---|
| **0** | Manifest spine + full track metadata (planet/favorite/AI/circuit/music for custom tracks; overlay for vanilla) | low | existing track delta |
| 1 | Custom circuits (names/colors/membership/ordering data-driven) | low | 0 |
| 2 | Custom planets (the big multi-array inflation + `<8` removal) | **high** | 0 |
| 3 | Custom racers/pods (inflate the two stat tables + hangar UI) | med | none (parallel) |
| 4 | Campaigns / career structure data-driven | med | 1, 2 |
| 5 | Online: catalog API + (manual now) package install + dependency resolution; in-game browser later | med | 0 |

Phases 3 and 5(thin) can run in parallel with the track line.

---

## 8. Phase 0 — detailed plan (track metadata)

1. **Vendor a JSON parser** into `dinput_hook/` (single-header, e.g. nlohmann/json);
   confirm it builds under the WinLibs GCC 13.2 recipe.
2. **Content Registry skeleton** (`dinput_hook/content_registry.{h,cpp}` or similar):
   - data model structs (Track first), keyed by slug;
   - seed the 28 vanilla tracks as defaults with canonical slugs (overlay base);
   - scan `assets/mods/*/manifest.json`, parse, validate, merge/override by slug;
   - allocate ids and resolve slug refs (planet/circuit/favorite).
3. **Re-point `custom_tracks.cpp`** to take `PlanetIdx`, `planetTrackNumber`,
   `FavoritePilot` from the registry record instead of the hardcoded `1/0/2`.
4. **Hook `InitAISettingsForTrack @ 0x4667e0`** to serve `ai.speed`/`ai.difficulty`
   from the model (fall through to vanilla for un-overridden tracks).
5. **Circuit assignment per custom track** from the manifest (replace the generic
   "Custom Tracks Page N" packing where a manifest specifies a circuit).
6. **Music (per track)** — RE resolved (sec. 9): register the mod's `data/wavs/Music`
   wav -> bank index, then hook `FUN_00427ea0` to return it for the track's
   (planet, subtrack). Playback/fade path (`FUN_00427880`) needs no change.
7. **Preview model** for custom tracks (manifest `preview_model`, else a sensible
   default), via `DrawTrackPreview @ 0x456c70` + model-node array `@ 0xe29a88`.
8. Keep `assets/custom_tracks/` working as a legacy/no-manifest path (auto-synthesize a
   default manifest) so existing folders don't break.

Build note: incremental relink of `dinput.dll`; **close the game first** (it holds the
DLL). See the build recipe.

---

## 9. Music & SFX data (RESOLVED 2026-06-10)

Sound ids are 0-based **bank indices = active-entry order in `data/Sounds.map`** (skip `#`
comments / blanks / `NUM*` directives). Files in `data/wavs/Music/` are auto-flagged streamed
when > `0x81330` bytes (~516 KB) by `swrSound_RegisterSound`. Functions named in Ghidra this
session: `swrSound_SelectTrackMusic` (0x427ea0), `swrSound_UpdateMusic` (0x427880),
`swrSound_SetMusicFade` (0x4277f0), `swrSound_ResetMusic` (0x427d70). (The stream-begin logic is
the inline tail of `swrSound_LoadSound @0x422ac0`, not a separate function.) Three data tables
(now named in Ghidra; reconfirmed by reading the GOG-addressed
`.data` straight off the Steam EXE — SteamStub only encrypts `.text`):

- **In-race music** — `short[8 planets][3 subtracks]` @ **`0x4b8750`**, read by
  `swrSound_SelectTrackMusic(planet, subtrack, restore)` -> `DAT_004b8744` (queued), cross-faded
  per frame by `swrSound_UpdateMusic` (`swrMain_RunFrame`) via `playASound(idx, channel 7, ...)`.
  `subtrack==3` special-cased (planet1->146 escapeloop, planet4->150 ConflictLoop3). Drivers:
  `swrObjJdge_F3` (race), `StartPostRaceSequence` (results), `swrObjHang_UpdateSplashScreen`
  (menu). Vanilla values resolved: p0 [DroidLoop1, podloop1, none]; p1 [anakin, boss, anakin];
  p2 [Psyche x3]; p3 [DroidLoop1, boss, Destroy]; p4 [conflict, Battle1, Battle2]; p5 [escape,
  Destroy, DroidLoop2]; p6 [conflict, Conflict2, Conflict3]; p7 [sebulba x3].
- **Per-planet intro/preload theme** — `short[8]` @ **`0x4b8780`**, read by
  `swrSound_PreloadSoundSet`. Vanilla: p0 mt01desert, p1 sfx_monks_chant_loop, p2
  mb00aquilarisintro, p3 mt01desert, p4 mx091lavacaves, p5 mx091lavacaves, p6 me00spiceintro,
  p7 mx091lavacaves. So intro music is **per-planet, not per-subtrack** (corrects an earlier note).
- **Per-track ambient SFX — RESOLVED.** `swrSfxPreloadSets` pointer table @ **`0x4b8fa8`**
  `[planet*3 + subtrack]` -> a list of 12-byte entries `{ float startProgress, float endProgress,
  u16 soundIndex, u16 flags }`, terminated by `startProgress == -1.0`. **Placement is by LAP
  PROGRESS (0..1), not XYZ** — the sound plays on channel 6 when the racer's progress is within
  `[start, end]`, volume ramped across the range (`start > end` wraps across the start/finish line);
  `flags & 1` = periodic random retrigger (a one-shot at random intervals) vs. continuous loop.
  Played each frame by `swrSound_UpdateEngineAudio(planet, subtrack, progress)` (from swrObjTest
  engine-audio `0x46d7a0`); preloaded by `swrSound_PreloadSoundSet`. `subtrack==3` special-cased
  (planet1->`0x4b8928`, planet4->`0x4b8cc8`); a global always-on list of sound indices is at
  `0x4b9008`. Example (planet 0 sub 1): `0.97..0.04 crowd_big_loop, 0.20..0.22 wind, 0.36..0.385
  mott_growl, 0.76..0.80 sandcrawler`. **Manifest model:** a per-track list of
  `{ sound, start, end, mode }`. Bind by registering custom sfx wavs and hooking the `0x4b8fa8`
  read in both `swrSound_UpdateEngineAudio` (play) and `swrSound_PreloadSoundSet` (preload).

**Binding (music):** register the mod's wav (-> bank index), hook `swrSound_SelectTrackMusic`
(in-race) and the `0x4b8780` read in `swrSound_PreloadSoundSet` (intro) to return it for the
track's (planet, subtrack); the playback/fade path needs no change.

---

## 10. Online / distribution (deferred — manual for now)

Near-term (manual): a download page serves a **package** = a zip of the mod folder. The
manifest's `slug` + `version` + content hash is its identity. Players unzip into
`assets/mods/` and rescan (or restart). Hash-dedupe against vanilla already exists for
track blocks (`custom_tracks.cpp`).

Future client (most likely an **in-game ImGui mod browser**, reusing the existing
overlay): hit the catalog API (`GET /index` search; `GET /download/{slug}/{version}`),
download into `assets/mods/`, verify hash, resolve `depends`, trigger a registry rescan.
Upload/publish is a web flow with server-side format validation (reuse
`scripts/validate_raw_model.py` logic) + hash pinning. **Safety note:** custom model DL
bytes are fixed up and rendered, so malformed content can crash — validation and hash
pinning are not optional for an online catalog.

---

## 11. Tooling

- **blender-swe1r addon** (`swe1r_import_export`) already exports the RAWM model + the
  `out_*block.bin` blocks. Natural home for an "export mod" action that also writes
  `manifest.json` so authoring is one step.
- `scripts/validate_raw_model.py` / `extract_raw_model.py` already validate/extract
  block payloads; reuse for client + server validation.

---

## 12. References

- `dinput_hook/game_deltas/tracks_delta.c` / `.h` — custom-track menu + table inflation.
- `dinput_hook/custom_tracks.cpp` / `.h` — folder scan, block redirect, id remap.
- `dinput_hook/game_deltas/swrModel_delta.cpp` — texture-buffer cap lift; loose model loader.
- `dinput_hook/texture_replacement.cpp`, `model_replacement.cpp` — loose per-asset templates.
- `src/types.h` — `TrackInfo`, `swrRacerData`, `PodHandlingData` typedefs.
- Key globals (reconfirm in Ghidra): `TrackInfo[25] @0x4bfee8`, `swrRacerData[23] @0x4c2700`,
  `PodHandlingData[23] @0x4c2bb0`, `g_aTracksInCircuits @0x4bfee0`, `g_aTrackIDs @0x4c0018`,
  planet name buffer `@0xe98f5c`, holo-model nodes `@0xe299f4`, sun/moon `@0xe98f40`.
- Key readers: `swrObjJdge_SetupTrackEnvironment @0x464b90`, `InitAISettingsForTrack @0x4667e0`,
  `swrUI_GetTrackNameFromId @0x440620`, `DrawTrackPreview @0x456c70`, `DrawHoloPlanet @0x456800`.
