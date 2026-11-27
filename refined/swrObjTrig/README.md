# swrObjTrig (refined) — dossier

Status: dossier only, no code (2026-11-26). Base: `refine/track_lifecycle` (proto line). Requested by
the user after the track_lifecycle U5 audit: "This is what the refinement layer is for. The game's
trigger system is a hard coded rigid system and we need to make it a flexible, easy to mod system."

The problem in one line: a trigger's behaviour is chosen by its type id, but the models, animations and
FX it needs are chosen by the planet. Each planet's setup fills a few numbered slots of three global
arrays (`swrObjTrig_ModelArray1`, `swrObjTrig_ModelArray2`, `swrObjTrig_AnimationArray`), and each
handler reads a hard-coded slot. A trigger type therefore only works on the planets whose setup happens
to fill its slot; anywhere else it reads NULL (fresh process) or the previous track's freed data. Stock
data never mixes them; custom tracks do.

Goal: a data-driven trigger system. A trigger type declares and owns the models, animations and FX it
needs, so any trigger type works on any planet or track; mods and custom tracks can define types and
parameters. No planet-indexed slots, no hard-coded per-planet setup. With every setting at its default
the game behaves exactly like retail (lockstep).

## 1. Boundary

All functions have full faithful bodies in `src/` (`src/Swr/swrObj.c`, `src/Swr/swrRace.c`).

| Address | Ghidra name | Role |
|---|---|---|
| 0x0047ddc0 | `swrObjTrig_LoadAndInitializeTriggerModels` | Per-planet setup: fills the slot arrays, builds the trigger scene root, walks the track model |
| (src static) | `swrObjTrig_LoadFxModel` / `swrObjTrig_LoadPropModel` | Slot fillers (FX: ModelArray2 + AnimationArray; prop: all three) |
| 0x0047dd90 | `swrObjTrig_CreateTriggerSceneNode` | Trigger scene root (`swrObjTrig_RootNode`, children `swrObjTrig_SceneChildNodes[64]`) |
| 0x0047dc40 | `swrObjTrig_FindAndInitializeTriggersInNode` | Walks the track model; every mesh behaviour's `triggers` list is registered |
| 0x0047d380 | `swrObjTrig_AddTriggerDescriptions` | Per-type setup at load (flags, spawned nodes, one `swrUtils_Rand` draw for Earthquake) |
| 0x0047e760 | `swrObjTrig_AddTriggerDescription` | Appends to `swrObjTrig_TriggerDescriptionArray[200]` (MP index table) |
| 0x0047d310 | `swrObjTrig_AddNodeToScene` | Spawn wrapper node whose child is `&ModelArray1[slot]` |
| 0x0047c130 | `swrObjTrig_InitNodeForTrigger` | Wrapper pool `swrObjTrig_NodePerTriggerArray[50]` |
| 0x0047d890 / 0x0047da10 | `swrObjTrig_InitRegionGrid` / `ComputeNodeRegionMask` | Region grid (2 layouts, track-specific) |
| 0x00476ac0 | `swrRace_ActivateTriggersInRange` | Per tick: flag tests, `FindOrCreate`, dispatch |
| 0x0047c7d0 | `swrObjTrig_FindOrCreate` | 'Trig' event object per live trigger |
| 0x0047ce60 | `swrRace_TriggerHandler` | Type switch: the effect of firing |
| 0x0047ca90 | `swrObjTrig_HandleCrashHitTrigger` | Break-destructable effect |
| 0x0047c920 | `swrObjTrig_HandleTrigger108` | Slalom flag knock-over |
| 0x0047cd90 | `swrObjTrig_Handle314Or501Trigger` | Methane vapor / lava splash |
| 0x0047c190 | `swrObjTrig_SpawnEarthquakeShake` | Earthquake FX + camera shake |
| 0x0047c390 / 0x0047c500 / 0x0047c710 | `swrObjTrig_F0` / `F2` / `F4` | 'Trig' object update / FX follow / events |
| 0x0047bea0 / 0x0047bee0 / 0x0047bf20 | `EnableFXAnimation` / `StopFXAnimation` / `AnimationActive` | AnimationArray slot helpers |
| 0x0047bf70 / 0x0047c080 / 0x0047c330 | `MaybeResetAnimation` / `...ByTriggerType` / `MaybeResetCameraShake` | Reset paths |
| 0x0047c0f0 | `swrObjTrig_FindNode` | |
| 0x0047e790 / 0x0047e7c0 / 0x0047e7e0 / 0x0047e830 | `FindTriggerDescriptionIndex` / `GetTriggerDescription` / `CreateAndActivateTriggerFromMultiplayerEvent` / `SendMultiplayerTriggerEvent` | MP trigger sync by index |
| 0x004651f0 | `swrObjJdge_AddTriggersToScene` | Seam: the judge's load calls the setup, root goes to scene slot 4 |

Owned state: `swrObjTrig_ModelArray1/2[6]`, `swrObjTrig_AnimationArray[6]`, `swrObjTrig_RootNode`,
`swrObjTrig_SceneChildNodes[64]`, `swrObjTrig_NodePerTriggerArray[50]`,
`swrObjTrig_TriggerDescriptionArray2[50]`, `swrObjTrig_TriggerDescriptionArray[200]` +
`swrObjTrig_NumTriggerDescriptions`, `swrObjTrig_CurrentPlanetId`, `specialActiveTrigger`, the region
grid globals, `swrObjTrig_regionGridLayout` (set by InitTrack per track).

Descriptor format (track data, parsed in place from the model block):
`swrModel_TriggerDescription { rdVector3 center; rdVector3 direction; float size_xy; float size_z;
swrModel_Node* affected_node; uint16 type; uint16 flags; next* }`, linked from a mesh's
`swrModel_Behavior.triggers`. The type id is the only behaviour selector; there is no parameter block.

Seams (crossing edges; the subsystem is tightly sealed):
- In: `swrObjJdge_AddTriggersToScene` (load), `swrRace_UpdateSurfaceTag` -> `ActivateTriggersInRange`
  (per tick), `swrMultiplayer_ApplyEvent` -> `CreateAndActivateTriggerFromMultiplayerEvent`, and the
  'Trig' event object's F0/F2/F4.
- Out: `swrModel_*` node/animation calls, `swrModel_LoadFromId` / `LoadAllAnimationsOfModel`,
  `swrRace_Explode`, `swrRace_UpdateFireEffects`, cMan `'Shak'` events, `swrSound_*`,
  `swrUtils_Rand`, `swrMultiplayer_SendEvent`.
- Lifetime: track_lifecycle events (load/unload) — see section 4.

## 2. Tier table

| Function | Tier | Reason |
|---|---|---|
| `swrRace_ActivateTriggersInRange` | **sacred** | Per-tick range/flag tests decide when effects hit pods |
| `swrRace_TriggerHandler`, `HandleCrashHitTrigger`, `HandleTrigger108`, `Handle314Or501Trigger`, `SpawnEarthquakeShake` | **sacred** | Change pod state (thrust decelerator, explode, teleport, kill), schedule FX timing, draw RNG |
| `swrObjTrig_F0` / `F2` / `F4`, `FindOrCreate`, `MaybeReset*` | **sacred** | Gate effect duration on `AnimationActive`; object lifetimes feed the 'Trig' event list order |
| `AddTriggerDescriptions` | **sacred** | Sets activation flags and draws `swrUtils_Rand` (Earthquake lap skip) in load order |
| `LoadAndInitializeTriggerModels`, `LoadFxModel` / `LoadPropModel`, `CreateTriggerSceneNode`, `AddNodeToScene`, `InitNodeForTrigger` | equivalent | Model/FX plumbing; load order must keep the asset-buffer layout and texture uploads (texture-hash oracle) |
| `EnableFXAnimation` / `StopFXAnimation` / `AnimationActive` | **sacred** (signature) / equivalent (storage) | Their results gate sacred timing; where the list lives is free |
| `FindAndInitializeTriggersInNode`, region grid | **sacred** | Order of registration = order of the MP index table and of RNG draws |
| MP index functions | equivalent | Wire format between peers of the same build |

Uncertain cases default to sacred. Tier rule for the refactor: the sacred handlers keep their arithmetic
and call order; what changes is where they get a node/animation from (a per-type asset record instead of
a planet slot), which is equivalent wherever retail's slot was filled.

## 3. Flaw inventory

| Tag | Address | Function | Finding | Refinement |
|---|---|---|---|---|
| HARDCODE (H1) | 0x0047ddc0 | `LoadAndInitializeTriggerModels` | Assets chosen per planet by a hard-coded `if (planetId == N)` ladder filling numbered slots; a trigger type's assets depend on the planet it is raced on | Asset needs declared per trigger type (data), loaded for the types the track uses |
| HARDCODE (H2) | 0x0047ce60 etc. | handlers | Slot per type hard-coded in five places (F0, F2, HandleCrashHit, MaybeResetAnimation, TriggerHandler): break 100/102/201/212 + MethaneVapor -> 0; break 202 + Earthquake -> 1; SpawnFlag -> 2 (+ `ModelArray1[3]/[4]` standing/fallen flag); EarthquakeShortcut -> 3; LavaSplash -> 5. Scene wrappers: Balloon/Flame -> 2, Fish/Dozer -> 0, SpawnFlag -> 3 | One type record holds its FX/prop references; handlers take them from the trigger, not a slot |
| UNSAFE (U1, was track_lifecycle U5) | see right | handlers | Unguarded slot dereferences: `NodeSetTranslation` / `NodeSetTransform` on `ModelArray2[slot]` in HandleCrashHitTrigger (0x0047ca90), Handle314Or501Trigger (0x0047cd90), SpawnEarthquakeShake (0x0047c190), HandleTrigger108 (slot 2), F2 (0x0047c500); `EnableFXAnimation` / `StopFXAnimation` / `AnimationActive` walk `*AnimationArray[slot]` with no NULL test (F0, HandleTrigger108, Handle314Or501, SpawnEarthquakeShake, MaybeResetAnimation). Only the `NodeModifyFlags` calls and `NodeFindFirstMaterial` / `ComputeFirstMeshAABB` are guarded. A trigger type on a planet that does not fill its slot dereferences NULL (fresh process) or writes into the previous track's rewound data | Gone by construction: a type's assets are loaded with it. A type whose assets are missing (bad mod data) is rejected at load with a logged error, never at fire time |
| GLOBAL (G1) | — | slot arrays | `ModelArray1/2` and `AnimationArray` are never cleared between tracks (ModelArray1 now cleared at unload by refined track_lifecycle step 2; the other two are not) | Arrays retired; per-track trigger state owned by the refined subsystem and dropped on `track_on_unload` |
| GLOBAL (G2) | 0x0047e760 | `AddTriggerDescription` | `swrObjTrig_NumTriggerDescriptions` is never reset, in retail too: the 200-entry MP index table fills up after about 16 track loads in a session and later tracks' triggers get no index (seen in the track_lifecycle step-6 sequence probe). MP sync sends indices into it, so peers with different load histories disagree (MP trigger-index desync) | Reset per track load (as the MP band-aid does); index = registration order of the current track |
| DUP (D1) | types_enums.h | flags | Two enums for the same descriptor flag bits with contradictory names: `swrModel_TRIGGERFLAG_DISABLED` = 0x1 = `swrModel_TriggerDescription_FLAG_ENABLED`; `TRIGGERFLAG_LOCAL_ONLY` = 0x20 = `FLAG_IGNORE_AI`; `MIN_SPEED` = `NEEDS_SPEED` | One flag enum, named by what activation does with the bit |
| LIMIT (L1) | — | pools | Fixed pools: 64 scene children, 50 wrapper nodes / registered descriptions, 200 index entries, 6 slots | Sized from the track's trigger count |
| TANGLE (T1) | 0x0047d380 | `AddTriggerDescriptions` | One function mixes registration, per-type flag defaults (`NEEDS_SPEED`, `IGNORE_AI`, `SKIP_LAP*`), node spawning and an RNG draw | Per-type `on_register` hook in the type record; flag defaults are type data |
| HARDCODE (H3) | 0x0047d380 | `AddTriggerDescriptions` | `specialActiveTrigger` chosen by type and planet (`KillPlayer`, or `BreakDestructable_202` only on planet 1) | Type data |
| HARDCODE (H4) | 0x00466bd0 | `swrObjJdge_InitTrack` | Region-grid layout chosen per track by InitTrack: planet 3 track 2 -> layout 1, planet 1 track 0 -> layout 2, else none | Track data |

Stock data, probed in track_lifecycle step 6 (all 25 tracks; trigger types per planet):

| Planet | Slots filled (MA1 / MA2+Anim) | Stock trigger types |
|---|---|---|
| 0 Tatooine | prop 2 (balloon) / 0, 1, 2 | 202, 203, 208 |
| 1 Ando Prime | 3, 4 (flag) / 0, 1, 2, 3 (+ MA2[4]) | 100, 103, 105, 108, 202 |
| 2 Aquilaris | prop 0 (fish) / 0, 1 | 304, 306, 307, 308 |
| 3 Ord Ibanna | — / — | none |
| 4 Baroonda | prop 2 (flame) / 0, 1, 2, 3, 5 | 202, 211, 212, 213, 501, 503 |
| 5 Mon Gazza | props 0, 2 (dozer, flame) / 0, 2 | 211 |
| 6 Oovo IV | — / 0 | none |
| 7 Malastare | — / 0 | none |

Every stock track only uses slots its planet fills, so retail never hits U1 on stock data. The planet
names above follow the swrObjJdge planet order; confirm against `swrUI` track info when writing the data
file.

## 4. Target shape

```c
/* refined/swrObjTrig/trigger_types.h */
typedef struct TriggerAssetRef { MODELID model; int child; /* -1 = entries[0].node */ int animated; } TriggerAssetRef;

typedef struct TriggerType {
    uint16_t id;                 /* descriptor type id; mods/custom tracks may add ids */
    const char* name;
    uint16_t default_flags;      /* activation flags applied at registration (one flag enum) */
    TriggerAssetRef fx;          /* effect played when fired (was ModelArray2/AnimationArray[slot]) */
    TriggerAssetRef prop;        /* spawned scene node (was ModelArray1[slot]) */
    TriggerAssetRef alt;         /* second state, e.g. knocked-over flag (was ModelArray1[4]) */
    int handler;                 /* which sacred effect routine runs; retail ids map 1:1 */
    /* effect parameters lifted from handler constants: shake strength/duration, FX scale, ... */
} TriggerType;

const TriggerType* trigger_type_find(uint16_t id);
int trigger_type_register(const TriggerType* type);      /* mods / custom track packs */
void* trigger_type_fx_node(const swrObjTrig* trig);     /* replaces ModelArray2[slot] */
swrModel_Animation** trigger_type_fx_anims(const swrObjTrig* trig);  /* replaces AnimationArray[slot] */
```

- Type registry with retail defaults compiled in (one record per id in the type table above, assets as
  each planet loads them today) and a data file `refined/swrObjTrig/data/trigger_types.json` with the same
  defaults; a missing or malformed file falls back to the compiled defaults with a logged warning. Custom
  track packs and mods add or override types through the same records (format open, see questions).
- Per-track asset instances: the refined trigger setup (it runs inside InitTrack, from
  `swrObjJdge_AddTriggersToScene`, before racers spawn) loads the assets of the types the track actually
  uses, in retail's order for stock data (so the asset buffer and texture uploads match). Two
  types that share an asset share one instance, as two triggers of one type do today.
- Handlers get assets from the trigger's type record. Their sacred arithmetic and call order stay as
  retail; only the slot lookup becomes a record lookup.
- Lifetime: registers on track_lifecycle — `track_on_unload` drops the per-track instances, the MP index
  table and the wrapper pool (retires G1/G2); `track_on_before_load` is where a custom track pack's type
  overrides are installed for the coming load.
- MP index table reset per track load and documented as the wire contract.
- Name map:

| Refined | Ghidra | Address |
|---|---|---|
| `refined_swrObjTrig_LoadAndInitializeTriggerModels` | `swrObjTrig_LoadAndInitializeTriggerModels` | 0x0047ddc0 |
| `refined_swrObjTrig_AddTriggerDescriptions` | `swrObjTrig_AddTriggerDescriptions` | 0x0047d380 |
| `refined_swrObjTrig_AddNodeToScene` | `swrObjTrig_AddNodeToScene` | 0x0047d310 |
| `refined_swrObjTrig_HandleCrashHitTrigger` | `swrObjTrig_HandleCrashHitTrigger` | 0x0047ca90 |
| `refined_swrObjTrig_HandleTrigger108` | `swrObjTrig_HandleTrigger108` | 0x0047c920 |
| `refined_swrObjTrig_Handle314Or501Trigger` | `swrObjTrig_Handle314Or501Trigger` | 0x0047cd90 |
| `refined_swrObjTrig_SpawnEarthquakeShake` | `swrObjTrig_SpawnEarthquakeShake` | 0x0047c190 |
| `refined_swrObjTrig_F0` / `F2` | `swrObjTrig_F0` / `F2` | 0x0047c390 / 0x0047c500 |
| `refined_swrObjTrig_MaybeResetAnimation` | `swrObjTrig_MaybeResetAnimation` | 0x0047bf70 |
| `refined_swrObjTrig_EnableFXAnimation` / `StopFXAnimation` / `AnimationActive` | same | 0x0047bea0 / 0x0047bee0 / 0x0047bf20 |
| `refined_swrRace_TriggerHandler` | `swrRace_TriggerHandler` | 0x0047ce60 |
| `refined_swrObjTrig_AddTriggerDescription` | `swrObjTrig_AddTriggerDescription` | 0x0047e760 |

## 5. Band-aids retired (swe1r)

| Band-aid | Where | Replaced by |
|---|---|---|
| `swrObjTrig_LoadAndInitializeTriggerModels_delta` (MP index reset, note 5) | `swrMultiplayer_delta.cpp` | Index table owned and reset per load (G2); also the fix for the MP trigger-index desync work |
| Receiver-side "description inside the arena" check (note 5b) | `swrMultiplayer_delta.cpp` | Indices only ever refer to the current track's table |
| `swrObjTrig_EnableFXAnimation_delta` (fast restart records which FX indices played, because unused slots hold stale lists) | `swrObjJdge_delta.cpp` | Per-track FX instances; fast restart resets the current track's instances |
| `swrRace_TriggerHandler_delta` snapshot of fired triggers (fast restart) | `swrObjJdge_delta.cpp` | Trigger state owned by the subsystem with a reset API (keeps the rumble hook as a listener) |
| `ModelArray1` clear at unload | `refined/track_lifecycle` | Arrays retired |
| Trigger-type table in memory notes / annodue port tables | collision viewer, docs | The registry is the single source (collision viewer reads names from it) |

## 6. Oracle plan

| Step | Check |
|---|---|
| Registry + per-type assets (equivalent plumbing) | Lockstep single-race 25/25 and the step-6 sequences (`seqS1v3`, `seqS2v3`, stock -> stock), refined on vs off; texture-hash oracle unchanged (asset load order) |
| Handlers reading type records (sacred) | Same lockstep, plus trigger-firing coverage (below) |
| MP index reset | Two-process MP test or a recorded MP event stream; at minimum a sequence run that logs the index table per track (no saturation) |
| Custom tracks | Sequence `seqC1v3` (stock -> custom) and a new pack that carries other planets' trigger types (flag on Tatooine, lava on Ando Prime): no fault, effects visible |

Gap: trigger firing. Lockstep replays use recorded inputs, and the recorded runs are mostly idle pods
(autostart, no throttle), so few triggers ever fire: the sacred handlers are largely unverified by the
oracle. Needed before handler work: recordings that drive through triggers — an autopilot run
(`SWR_STANDALONE_AUTOPILOT=1`) recorded in retail on each planet with triggers (0, 1, 2, 4, 5), or the
recorded human/TAS routes from the replay corpus — plus a per-race count of fired triggers by type
in the lockstep log so coverage is measurable.

## 7. Step plan

1. **M — Trigger-firing oracle.** Autopilot (or replay-corpus) retail recordings that fire every stock
   trigger type; lockstep logs fired types per race. Prerequisite for any sacred change.
2. **S — DUP/naming.** One flag enum (D1); type table as named constants. No behaviour change.
3. **M — Type registry with retail defaults** (compiled in), handlers unchanged: the registry only
   describes today's planet slots. Lockstep 25/25 + sequences.
4. **L — Per-type assets.** Refined setup loads assets per used type (retail order for stock data);
   refined handlers take assets from the type record. Retires H1, H2, U1, G1. Lockstep + firing oracle
   + texture hash; custom cross-planet pack shows the effects.
5. **S — MP index table owned and reset per load** (G2); retire the MP delta (coordinate with the MP
   trigger-index desync work).
6. **M — Data file + mod/custom-track registration** (`trigger_types.json`, pack overrides on
   `track_on_before_load`), effect parameters lifted out of handler constants.
7. **S — Retire band-aids** (fast-restart FX/trigger snapshot deltas onto the subsystem's reset API).

## Open questions

- Data format for the type file (roadmap 9.4: JSON vs binary) and how a custom track pack ships its
  types (same file in the pack folder?).
- New trigger ids from mods: reserve a range (e.g. >= 1000) so a pack cannot redefine a stock id by
  accident, or allow overrides explicitly?
- Effect parameters: which handler constants become parameters (shake strength/duration, FX scale,
  speed threshold 150, flag offsets +8/-24) without touching sacred arithmetic for the defaults.
- Location (roadmap 9.1), as for the rest of `refined/`.
