# swrObjTrig (refined) — dossier

Status: dossier only, no code (2026-11-26; section 4 redesigned as action lists and section 5 added 2026-11-27). Base: `refine/track_lifecycle` (proto line). Requested by
the user after the track_lifecycle U5 audit: "This is what the refinement layer is for. The game's
trigger system is a hard coded rigid system and we need to make it a flexible, easy to mod system."

The problem in one line: a trigger's behaviour is chosen by its type id, but the models, animations and
FX it needs are chosen by the planet. Each planet's setup fills a few numbered slots of three global
arrays (`swrObjTrig_ModelArray1`, `swrObjTrig_ModelArray2`, `swrObjTrig_AnimationArray`), and each
handler reads a hard-coded slot. A trigger type therefore only works on the planets whose setup happens
to fill its slot; anywhere else it reads NULL (fresh process) or the previous track's freed data. Stock
data never mixes them; custom tracks do.

Goal: a data-driven trigger system. A trigger is a list of actions from a small vocabulary (play an
animation, play a sound, show or hide a node, spawn a model, shake the camera, affect the pod, wait, ...)
that names the effects and models it uses, so any trigger works on any planet or track. Retail's type ids
become built-in lists; mods and custom tracks compose new ones in data, and code mods add new actions.
No planet-indexed slots, no hard-coded per-planet setup. With every setting at its default
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
- Lifetime: track_lifecycle events (load/unload) — see section 4.4.

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

Uncertain cases default to sacred. Tier rule for the refactor: a built-in list that replaces a sacred
handler (section 4.3) must issue the same engine calls, constants and RNG draws in the same order; what
changes is where it gets a node/animation from (a named effect instead of a planet slot), which is
equivalent wherever retail's slot was filled.

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

## 4. Target shape: triggers as action lists

Revised 2026-11-27 (design discussion with the user). The first draft kept one sacred handler per type
id (`int handler` in a type record). That keeps the rigidity: ~30 ids, each a bespoke routine, many of
them the same few operations with different constants. The target is instead a small vocabulary of
**actions**, and a trigger is a list of them. A type id becomes the name of a built-in list.

### 4.1 Retail types decomposed

Read from `swrRace_TriggerHandler` (0x0047ce60), `HandleCrashHitTrigger`, `HandleTrigger108`,
`Handle314Or501Trigger`, `SpawnEarthquakeShake` and the F0 timer switch.

| Retail type | As actions |
|---|---|
| Door 101 / 307 | play animation on the affected node (101 reverses direction each fire); re-arm after `SWR_TRIG_REARM_SECS` |
| Tram 304 | set speed 1.0 on two animations; once |
| Tent shortcut 103 | hide node, show node; once |
| Collapsing glacier / falling pillar 105 / 312 | play node animation (speed 0.6), shake (0.05, 4.0); wait 4 s; stop shake |
| Earthquake 306 / shortcut 213 | shake (0.25, 8.0); wait 1.5 s; hide/show nodes, play FX at the affected node's AABB centre; wait 0.7 s; stop shake, stop FX |
| Break 100 / 102 / 201 / 202 / 212 | slow pod (decel 25 or 2.5), damage hit `'Hitt'` (100 / 300 / 600), hide node, play FX at trigger centre (102 + AI: scale 8), sound 0x39 at pod with random volume; FX follows pod velocity |
| Methane 314 / lava 501 | if FX not busy: play FX at affected node transform; else nothing |
| Slalom flag 108 | hide node, play FX facing pod heading at centre +8 / -24, swap to fallen-flag model |
| Tuskens 203 / asteroids 106 | 2D sound 0x30 (203 only), flame hazards; 203 re-arms |
| Kill 104 | explode pod, variant 1 or 2 by `swrUtils_Rand` |
| Teleport 900 | message (empty, 3 s), reset pod to spline |
| Balloon 208 / fish 308 / dozer 310 / flame 211 | spawn model on a wrapper node; 208 / 308 follow the affected node (F2) |

### 4.2 Structure of a trigger

```json
"core.break_rock_202": {
  "when":  { "flags": ["needs_speed"] },
  "after": "once",
  "do": [
    { "slow_pod":  { "decel": 2.5 } },
    { "damage":    { "amount": 600 } },
    { "hide_node": "affected" },
    { "play_fx":   { "fx": "rock_burst", "at": "trigger_center", "follow": "pod_velocity" } },
    { "sound":     { "id": "0x39", "at": "pod", "volume": { "rand": [0.25, 0.40] }, "range": [10, 500] } }
  ]
}
```

- **when** — conditions (activation flags today: needs-speed, ignore-AI, local-only, skip-lap, plus the
  Earthquake random lap skip). Evaluated by the sacred range test in `ActivateTriggersInRange`.
- **after** — re-arm policy: `once` (disable + free object), `rearm` (cooldown), `toggle` (door direction).
- **do** — a timeline: actions run in order; `wait` suspends the list on the trigger's 'Trig' object, which
  is what F0's per-type timers do today. F0's type switch disappears into the runner.
- **Context** each action can target: the triggering pod, the trigger centre, the affected node, a named
  node, the pod's heading.
- **Named effects** replace slots. An FX/prop is a named resource (`rock_burst`) with a busy state;
  "FX still playing -> skip" (methane/lava) and the slot sharing between break types become explicit:
  types that name the same effect share one instance, exactly as they share slot 0 today.
- **Randomness is positional.** An action with a `rand` parameter draws `swrUtils_Rand` at its place in
  the list. Order of actions = order of RNG draws = lockstep.

### 4.3 Retail equivalence

Each retail type ships as a built-in list (`core.*`) that must issue the same engine calls, in the same
order, with the same constants as today's handler (sacred tier applies to the list, not to a function).
Where a handler does not decompose cleanly (the 102-AI scale-up, the flag's `+8 / -24` placement and
heading basis, the combined node-flag writes), the built-in list calls a **named special action**
(`core.slalom_flag_place`, ...) holding the retail code verbatim. Specials can be decomposed later;
they are never bent into general actions to fit a quirk.

Old handlers stay compiled behind the vanilla toggle until every built-in list passes the firing oracle.

### 4.4 Lifetime and loading

- Per-track effect instances: the refined setup (inside InitTrack via `swrObjJdge_AddTriggersToScene`,
  before racers spawn) loads the effects/models the track's triggers name. For stock tracks it must
  reproduce the planet ladder's loads exactly — including loads no trigger uses (planets 6 and 7 fill
  MA2[0] with no triggers; Ando Prime fills MA2[4]) — so the retail default is a per-planet preload list
  in data, not something derived from used types. The asset buffer and texture-hash oracle decide.
- `track_on_unload` drops instances, the MP index table, the wrapper pool and trigger variables.
  `track_on_before_load` installs a custom track pack's definitions for the coming load.
- MP index table reset per load (G2). This differs from retail (which saturates after ~16 loads): a
  named deliberate change, behind the vanilla toggle.

## 5. Extensibility for modders

Two tiers: **data mods** compose existing actions in JSON (no code; most content), **code mods** register
new actions through the modding API (`MODDING_API_ROADMAP.md`, Phase 5 seams).

### 5.1 Mechanism

- **One registry, built-ins use it too.** Every action is a record — name, typed parameter schema, run
  function, tags. `core.*` actions register through the same call a plugin uses.

```c
trig_action_register(&(TrigAction){
    .name   = "mymod.gravity_flip",   /* namespaced: core.*, <mod>.* */
    .params = gravity_flip_params,    /* typed schema, validated at load */
    .run    = gravity_flip_run,
    .tags   = TRIG_POD_STATE | TRIG_USES_RNG,
});
```

- **Three registries:** actions (what happens), conditions (whether it fires), targets (who / where:
  triggering pod, all pods, trigger centre, named node, spline offset). New targets work with every action.
- **Shared parameter kinds:** number (constant, `{"rand": [a, b]}`, or variable ref), position, duration,
  asset ref, target. Adding a kind upgrades every action.
- **Flow control in the runner:** `wait`, `repeat`, `chance`, `pick_random`, `sequence` / `parallel`,
  `set_var` / `if_var` (per-trigger and per-race named variables). Variables give switches, keys and gates
  ("hit three switches to open the shortcut") with no new code.
- **Events out:** `trigger.fired` / `trigger.finished` on the mod event bus, so plugins react to stock
  triggers without replacing them.
- **Tags every action declares:** uses RNG (must draw through `swrUtils_Rand` in list order),
  changes pod state (a vanilla / competitive ruleset can reject it), local-only (UI, rumble, sound not
  synced in MP).
- **Validation at load:** unknown action, bad parameter or missing asset rejects that trigger with a log
  line. Nothing fails at fire time.

### 5.2 Vocabulary

**Tier A — retail already calls these** (needed for the built-ins; exposing them is a schema):
`play_fx` / `stop_fx`, `play_anim` (speed, loop, reverse), `show_node` / `hide_node`, `spawn_model`
(wrapper node, optional follow), `sound` (2D `swrSound_Play2D` or spatial `swrSound_PlaySpatialRange`),
`smoke` (`swrObjSmok_Spawn`), `shake` / `stop_shake` (cMan `'Shak'`), `message`
(`swrText_ShowTimedMessage`), `explode` (`swrRace_Explode`), `reset_to_spline`, `slow_pod`
(`stdMath_Decelerator` on thrust), `damage` (`'Hitt'` sub-event), `flame_hazards`.

**Tier B — cheap, on systems the project already has** (check each API before committing):
boost / speed multiplier, heat up / cool down, repair or damage a part, refill boost charge, teleport to a
point or spline offset, gamepad rumble; add or subtract race time (refined `swrObjJdge` timing), switch
music; fog colour / distance, weather on / off, light colour (fog, weather and light setters are
reimplemented); move or rotate a node over time (small new tween).

**Conditions:** pod speed above / below, AI / human, lap number, race position, player count, entry
direction, chance, variable test, race time, first N pods.

**Tier C — harder, design separately:** AI awareness of new hazards (AI drives into a mod's lava today;
needs avoid / slow hints), new physics forces (gravity, wind, push zones: sacred physics, own toggle), new
trigger shapes and triggers placed from data rather than baked into track models (custom-track tooling).
MP sync sends only "trigger N fired", so every synced action must be deterministic given that event.

## 6. Band-aids retired (swe1r)

| Band-aid | Where | Replaced by |
|---|---|---|
| `swrObjTrig_LoadAndInitializeTriggerModels_delta` (MP index reset, note 5) | `swrMultiplayer_delta.cpp` | Index table owned and reset per load (G2); also the fix for the MP trigger-index desync work |
| Receiver-side "description inside the arena" check (note 5b) | `swrMultiplayer_delta.cpp` | Indices only ever refer to the current track's table |
| `swrObjTrig_EnableFXAnimation_delta` (fast restart records which FX indices played, because unused slots hold stale lists) | `swrObjJdge_delta.cpp` | Per-track effect instances; fast restart resets the current track's instances |
| `swrRace_TriggerHandler_delta` snapshot of fired triggers (fast restart) | `swrObjJdge_delta.cpp` | Trigger state owned by the subsystem with a reset API (keeps the rumble hook as a listener) |
| `ModelArray1` clear at unload | `refined/track_lifecycle` | Arrays retired |
| Trigger-type table in memory notes / annodue port tables | collision viewer, docs | The `core.*` lists are the single source (collision viewer reads names from it) |

## 7. Oracle plan

| Step | Check |
|---|---|
| Runner + named effects (equivalent plumbing) | Lockstep single-race 25/25 and the step-6 sequences (`seqS1v3`, `seqS2v3`, stock -> stock), refined on vs off; texture-hash oracle unchanged (per-planet preload order) |
| Built-in lists replacing handlers (sacred) | Same lockstep, plus the trigger-firing oracle, one type at a time; old handler kept behind the vanilla toggle for A/B |
| MP index reset | Two-process MP test or a recorded MP event stream; at minimum a sequence run that logs the index table per track (no saturation) |
| Custom tracks / cross-planet | Sequence `seqC1v3` (stock -> custom) and a pack that uses other planets' effects (flag on Tatooine, lava on Ando Prime): no fault, effects visible |
| Mod actions | Determinism check: a list with `rand` / `chance` replayed twice gives the same lockstep hash; MP-synced actions rejected if tagged local-only |

Gap: trigger firing. Lockstep replays use recorded inputs, and the recorded runs are mostly idle pods
(autostart, no throttle), so few triggers ever fire: the sacred handlers are largely unverified by the
oracle. Needed before any built-in list replaces a handler: recordings that drive through triggers — an
autopilot run (`SWR_STANDALONE_AUTOPILOT=1`) recorded in retail on each planet with triggers (0, 1, 2,
4, 5), or the recorded human/TAS routes from the replay corpus — plus a per-race count of fired
triggers by type in the lockstep log so coverage is measurable.

## 8. Step plan

1. **M — Trigger-firing oracle.** Retail recordings that fire every stock trigger type; lockstep logs
   fired types per race. Prerequisite for any sacred change.
2. **S — DUP/naming.** One flag enum (D1); type table as named constants. No behaviour change.
3. **M — Runner core.** Action/condition/target registries, timeline runner on the 'Trig' object
   (`wait`, sequence), re-arm policies, named effects with busy state, per-planet preload data. Tier A
   actions registered. Not yet wired to any type. Lockstep 25/25 + sequences unchanged.
4. **L — Built-in lists, one type at a time.** Each retail type moves from its handler to a `core.*`
   list (specials where it does not decompose); verified by the firing oracle against the old handler.
   Retires H1, H2, U1, G1, and F0's type switch once all types move.
5. **S — MP index table owned and reset per load** (G2), behind the vanilla toggle; retire the MP delta
   (coordinate with the MP trigger-index desync work).
6. **M — Data file + pack registration** (`trigger_types.json`, compiled-in fallback, pack overrides on
   `track_on_before_load`); flow control (`chance`, `pick_random`, `repeat`, variables); conditions.
7. **M — Tier B actions and targets**, each checked against its subsystem's API.
8. **M — Plugin registration API** (`trig_action_register`, `trigger.fired` events), with the Phase 5
   seams of the modding roadmap.
9. **S — Retire band-aids** (fast-restart FX/trigger snapshot deltas onto the subsystem's reset API).

## Open questions

- Data format for the type file (roadmap 9.4: JSON vs binary) and how a custom track pack ships its
  definitions (same file in the pack folder?).
- Stock ids: may a pack override `core.*` lists explicitly, or only add new names? Descriptor type ids
  from track data map to list names — reserve a range for mod ids (e.g. >= 1000)?
- Timelines: strictly sequential (plus `parallel`), or branching (`if_var` jumping to labels)?
- Variable scope: per trigger and per race only, or persistent across races (tournament, profile)?
- Which ruleset decides whether pod-state mod actions are allowed (vanilla / competitive / MP host)?
- Effect parameters: which handler constants become list parameters (shake strength/duration, FX scale,
  speed threshold 150, flag offsets +8/-24) without touching sacred arithmetic for the defaults.
- Location (roadmap 9.1), as for the rest of `refined/`.
