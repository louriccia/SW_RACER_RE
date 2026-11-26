# track_lifecycle (refined) — dossier

Status: steps 1-3 done (2026-11-26), see "Progress" at the end. Base: `proto/x64` (the line that carries
`refined/`; the 32-bit build is the oracle build, the 64-bit build must still compile and race).

The problem in one line: retail has no notion of "the track was unloaded". A track's models,
splines and textures live in the asset buffer; loading the next track rewinds the buffer and
overwrites them in place, and every pointer still aimed at the old data (in game structs or in our
own feature caches) silently turns into garbage. Retail never notices because, with stock data and
stock features, nothing reads those pointers after the switch. Custom tracks and our features do,
and the delta layer has grown one band-aid per crash. This pass gives the track a lifetime with one
owner, so stale pointers are cleared at the source and features subscribe to load/unload events
instead of wrapping `swrObjJdge_InitTrack`.

## 1. Boundary

Functions (all have full faithful bodies in `src/`; the standalone runs zero retail code).

| Address | Ghidra name | Role here |
|---|---|---|
| 0x00445aa0 | `swrAssetBuffer_ResetToIndex` | **The unload point.** Rewinds the arena; everything above the mark is dead |
| 0x0045b290 | `swrObjHang_StartRace` | Calls ResetToIndex(1) before a race |
| 0x0045ce90 | `swrObjHang_CullElmoAssets` | Calls ResetToIndex(checkpoint) (hangar asset cull) |
| 0x00466bd0 | `swrObjJdge_InitTrack` | Calls ResetToIndex(1), then loads models, HUD, spline, grid, racers |
| 0x00464b90 | `swrObjJdge_SetupTrackEnvironment` | Seeds the fly-by cursor; early-returns without clearing it |
| 0x004472e0 | `swrSpline_LoadSplineById` | Loads a spline into the arena |
| 0x0047e8b0 | `swrSpline_CursorSeekToProgress` | Leaves the node window untouched when no node matches |
| 0x0044ed80 | `swrSpline_EvaluateToMatrix` | Evaluates a cursor (faults on a stale spline) |
| 0x00448780 | `swrModel_LoadFromId` | Loads a model into the arena |
| 0x00454c60 | `swrModel_ClearLoadedModels` | Clears the loaded-model table on track change |

Owned state: the asset-buffer marks (`swrAssetBuffer_*`, already partly refined in
`refined/swrAssetBuffer`), `swrObjJdge.cam_spline` / `camSweepCursor` / `camSweepTransform`, the
viewport scene roots (`swrViewport_array[*].model_root_node` children, `someRootNodeChildNodes`).

Seams (crossing edges, debt until the neighbour is refined):
- In: hangar (`StartRace`, `CullElmoAssets`), judge (`InitTrack`), every caller of the spline
  cursor API (judge F2 fly-by, `swrRace_UpdateSurfaceTag`, swrObjTest, cMan cinematics, AI).
- Out: `swrObjJdge_LoadTrackModels`, `swrObjJdge_LoadTrackSpline`, `swrPlayerHUD_LoadRaceHUD`,
  `swrObjJdge_SpawnRacers` (stay faithful, called in retail order).

## 2. Tier table

| Function | Tier | Reason |
|---|---|---|
| `swrAssetBuffer_ResetToIndex` | equivalent | Adds an event; arena arithmetic unchanged |
| `swrObjJdge_InitTrack` | **sacred** | Load order feeds spawn and RNG draws; refined copy may only add events at the edges |
| `swrObjJdge_SetupTrackEnvironment` | equivalent | Clearing the cursor on the early-return path is unreachable with stock data (see flaw U1) |
| `swrSpline_LoadSplineById`, `swrModel_LoadFromId` | equivalent | Wrap with load events; load bytes unchanged |
| `swrSpline_CursorSeekToProgress` | **sacred** | AI and progress use it; no math change. Only an invalid-window guard on a path retail never takes |
| `swrSpline_EvaluateToMatrix` | **sacred** | Physics and camera; untouched. The guard moves out (stale cursors no longer exist) |
| `swrModel_ClearLoadedModels` | equivalent | Becomes an unload listener trigger |

## 3. Flaw inventory

| Tag | Address | Function | Finding | Refinement |
|---|---|---|---|---|
| UNSAFE (U1) | 0x00464b90 | `SetupTrackEnvironment` | Returns early when the track has no camera spline (`cam_splineId < 0`), leaving `cam_spline` and `camSweepCursor.spline` aimed at the previous track's arena data. Root cause of the 2026-09 custom-track CTDs (PR #307). Stock data never takes it: the camSplines table has an entry for every one of the 25 stock tracks (2+4+3+3+4+3+3+3); every -1 is a slot with no stock track | Clear `cam_spline`, `camSweepCursor`, `camSweepTransform` on every early return |
| GLOBAL (G1) | 0x00445aa0 | `ResetToIndex` | The unload is implicit; nobody can tell which pointers it killed | Track generation counter + ordered `on_track_unload` / `on_track_loaded` listeners |
| UNSAFE (U2) | 0x0047d310 | `swrObjTrig_AddNodeToScene` / `swrObjTrig_ModelArray1` | Traced in step 2: trigger-wrapper nodes take their child from `swrObjTrig_ModelArray1[slot]`, which is never cleared between tracks and is written only by `LoadPropModel` and planet 1's flag setup. A planet that fills a slot with `LoadFxModel` (ModelArray2 only) or not at all leaves the previous track's node there. The delta nulls the resulting invalid child after InitTrack (`fixup_invalid_node_ptrs`, HACK) | Clear `ModelArray1` at unload (done, step 2); retire the scan in step 4 |
| UNSAFE (U5) | 0x0047ddc0 | `swrObjTrig_LoadAndInitializeTriggerModels` | Same carry-over for `swrObjTrig_ModelArray2` and `swrObjTrig_AnimationArray`: trigger handlers (`Handle314Or501Trigger`, earthquake, break FX) write transforms and flags through `ModelArray2[index]` and animate `AnimationArray[index]`, so a trigger whose slot the planet does not fill writes into the previous track's (rewound) data. Not reached by the step-2 repro; clearing them turns a stale write into a NULL path the handlers only partly guard | Audit the handlers' NULL handling, then clear both at unload (later step) |
| UNSAFE (U3) | 0x0047e8b0 | `CursorSeekToProgress` | Scan falls through without writing when no node matches, keeping a node window from a longer spline. Only reachable with a cursor carried across tracks | With U1/U4 fixed, prove unreachable (log); keep a guard only behind a debug assert |
| UNSAFE (U4) | — | cursor owners | Any `swrSplineCursor` that outlives its track (fly-by, swrObjTest `splineCursor@0xac`, cMan) holds a raw `swrSpline*` into the arena | Cursors registered with the lifetime owner are reset on unload |
| TANGLE (T1) | 0x00466bd0 | `InitTrack` (delta) | One wrapper does nine unrelated things for different features: track_registry mapping, cable bends, lap tracking, scene-animation snapshot, trigger state, countdown latch, crash-log stages, node fixup | Each feature registers its own load/unload listener; the wrapper disappears |
| DUP (D1) | — | caches | Four feature caches flush themselves from four different hooks (mesh geometry via ClearLoadedModels, texture cache via InitializeTextureBuffer, trigger state and cable bends via InitTrack) | All flush from `on_track_unload` |

## 4. Target shape

```c
/* refined/track_lifecycle/track_lifecycle.h */
uint32_t track_generation(void);                 /* bumps at every unload */
void track_on_unload(const char *owner, void (*fn)(void *), void *user);
void track_on_loaded(const char *owner, void (*fn)(void *), void *user);
void track_register_cursor(swrSplineCursor *c);  /* reset to {spline=NULL} on unload */
bool track_pointer_is_live(const void *p);       /* p inside the arena below the current top */
```

- Listeners run in registration order; unload listeners run inside the refined
  `ResetToIndex`, before the rewind, so they may still read the old data.
- `on_track_loaded` fires at the end of refined `InitTrack` (after `CheckOverflow`); a pre-load
  hook (`track_registry` mapping) uses `on_track_unload` of the previous track, or a dedicated
  `track_on_before_load` if ordering requires it (decide in step 3).
- No data files: this subsystem has nothing to externalize.

Name map:

| Refined | Ghidra | Address |
|---|---|---|
| `refined_swrAssetBuffer_ResetToIndex` | `swrAssetBuffer_ResetToIndex` | 0x00445aa0 |
| `refined_swrObjJdge_InitTrack` | `swrObjJdge_InitTrack` | 0x00466bd0 |
| `refined_swrObjJdge_SetupTrackEnvironment` | `swrObjJdge_SetupTrackEnvironment` | 0x00464b90 |
| `refined_swrModel_ClearLoadedModels` | `swrModel_ClearLoadedModels` | 0x00454c60 |

## 5. Band-aids retired (swe1r)

| Band-aid | Where | Replaced by |
|---|---|---|
| `swrSpline_EvaluateToMatrix_delta` refusal + clamp | `swrSpline_delta.cpp` | U1/U4 fixed at the source; refusal kept as a debug assert only |
| `swrSpline_CursorSeekToProgress_delta` re-seed | `swrSpline_delta.cpp` | U3 unreachable |
| `spline_cursor_has_usable_spline` in the fly-by gate | `swrObjJdge_delta.cpp` | cursor is NULL after an early return, plain NULL check suffices |
| `fixup_invalid_node_ptrs` HACK | `swrObjJdge_delta.cpp` | U2 reset at unload |
| `swrObjJdge_InitTrack_delta` (9 duties) | `swrObjJdge_delta.cpp` | per-feature listeners |
| `swrModel_ClearLoadedModels_delta` mesh-cache flush | `renderer_hook.cpp` | `on_track_unload` listener |
| texture-cache clear on block change | `refined/swrModel`, `swrModel_delta.cpp` | `on_track_unload` listener |

The DLL build keeps its retail-side hooks (unchanged), as for the other refined subsystems.

## 6. Oracle plan

| Step | Check |
|---|---|
| Lifecycle events (equivalent) | Lockstep ALL MATCH on all 25 tracks, refined on vs `SWR_REFINED_OFF=1`; texture-hash oracle unchanged |
| U1 cursor clear | Same lockstep (path unreachable with stock data) + custom-track crash repro: Boonta Training (model 115 / spline 74) then Big Blue with `restore_prerace_track_sweep=1`, no fault, fly-by skipped |
| U2 node slots | Log which root slots change across stock→custom→stock; after the fix, the old scan (kept as a log-only check for one round) finds 0 invalid nodes |
| U3/U4 | Instrumented run over the same switch sequence: zero out-of-range windows, zero stale cursors |
| Listener migration | Each feature's own behaviour: fast restart, trigger FX, cable bends, lap tracking, crash-log stages, mesh cache (renderer perf stays at ~110 fps on a 16-racer grid) |

Gaps: lockstep runs one race per process, so a multi-race track-switch sequence has no machine
oracle yet; the switch repro is a scripted launch + log check. Adding a "sequence" mode to the
lockstep driver (race A, back to hangar, race B, hash both) would close it (size M).

## 7. Step plan

1. **S — Lifetime owner + events.** `refined_swrAssetBuffer_ResetToIndex` with generation +
   unload listeners; `on_track_loaded` from a refined `InitTrack` that is a verbatim copy plus one
   call at the end. Lockstep 25/25.
2. **M — Trace U2.** Find the root-child slots that carry over; record findings here.
3. **S — U1 + U4.** Refined `SetupTrackEnvironment` clears the cursor on early return; cursor
   registry resets registered cursors on unload. Switch repro clean, lockstep 25/25.
4. **M — Migrate listeners.** Move the nine `InitTrack_delta` duties, the mesh-cache flush and the
   texture-cache clear onto events, one commit per feature; delete the wrappers in swe1r.
5. **S — Retire spline guards.** Downgrade the EvaluateToMatrix / CursorSeekToProgress deltas to
   debug asserts; confirm zero hits over the switch sequence.
6. **M (optional) — Lockstep sequence mode** to give step 3-5 a machine oracle.

## Open questions

- Location (roadmap 9.1) still unagreed with tim; this lives on the proto line like the rest of
  `refined/`.
- Step 1 ordering: does `track_registry_ApplyForCurrentTrack` need a before-load event, or can it
  run as the previous track's unload listener? Depends on whether anything between the rewind and
  `LoadTrackModels` reads the mapping.

## Progress

### Step 1 (done) — lifetime owner + events

- `track_lifecycle.c`: `track_generation()`, `track_on_unload()` / `track_on_loaded()` (fixed table
  of `TRACK_LIFECYCLE_MAX_LISTENERS`, registration order, duplicate (fn, user) ignored) and
  `refined_swrAssetBuffer_ResetToIndex` (0x00445aa0, equivalent): unload listeners, generation bump,
  then the faithful rewind unchanged. Every rewind is an unload, including the hangar's
  `CullElmoAssets` and the two that precede a race (`StartRace`, then `InitTrack`).
- `track_lifecycle_refined.c`: `refined_swrObjJdge_InitTrack` (0x00466bd0, sacred) is the src/
  body byte-for-byte plus `track_lifecycle_notify_loaded()` after `swrAssetBuffer_CheckOverflow()`.
- Both in `refined_slots`. No listener is registered yet (step 4 migrates the delta duties).
- Verified: swe1r full link; lockstep 25/25 ALL MATCH refined on, 25/25 ALL MATCH with
  `SWR_REFINED_OFF=1` (sa_t0..24, 1500 frames, pod 0); x64 (llvm-mingw, Release) compiles and links.

### Step 2 (done) — U2 traced and fixed

Trace: a temporary walk of `swrViewport_array[0].model_root_node` after InitTrack (logging every
node `fixup_invalid_node_ptrs` would drop, with its path and arena offset), plus a probe of
`swrObjTrig_ModelArray1[0..5]` after each InitTrack stage. Track switches were driven by a new
lockstep key, `then <track>` (record mode: at the frame limit, select that track and restart via
the judge's `'RStr'`, the pause-menu restart; up to 8 in a row), so one process races several
tracks without menu input.

Findings (Boonta Training, hangar track 0, then Bowser's Castle 1, custom track 31):
- Only one scene slot carries over: root child 4 (the trigger root `swrObjTrig_RootNode`),
  children 3 and 4. Both are trigger wrappers from `swrObjTrig_AddNodeToScene` (static
  `swrObjTrig_NodePerTriggerArray` pivot nodes, type 0xD065) whose child list is
  `&swrObjTrig_ModelArray1[2]`.
- `ModelArray1[2]` is Boonta's `balloon01_part` (planet 0 `LoadPropModel(2, ...)`, arena+675736).
  Bowser's Castle 1 runs as planet 1, which loads slot 2 with `LoadFxModel` (writes ModelArray2
  only) but carries BalloonSpawner / FlameSpawner triggers (`AddNodeToScene(desc, flags, 2)`), so
  its wrappers point at the old balloon, overwritten by the new track's data (type reads 0).
- A fresh process racing Bowser's Castle 1 first is clean (`ModelArray1` is all NULL), which is
  why the bug needs a previous track. Big Blue (planet 1, no spawner triggers) is unaffected.
- Every other root slot is rebuilt by `LoadTrackModels` (it memsets `someRootNodeChildNodes`), and
  the dust/fireball/pod roots are re-initialized each load.

Fix: `refined_swrAssetBuffer_ResetToIndex` clears `swrObjTrig_ModelArray1` after the unload
listeners. Retail never reads a slot its planet did not fill on stock data the lockstep covers
(each lockstep race is the first of its process, where the array is all NULL anyway), so this is
equivalent tier. Gap: no oracle covers a stock-to-stock switch; whether any stock track wraps a
slot its planet does not fill is unchecked (a scan of the stock trigger descriptions per planet would
settle it). The wrappers then get a NULL child, which the renderer already handles (it is what
the HACK produced). ModelArray2 / AnimationArray have the same carry-over (U5, not fixed: see the
flaw table).

Verified: repro Boonta -> Bowser 1 -> Big Blue -> Boonta -> Bowser 1, refined on: 0 invalid nodes,
no HACK line; `SWR_REFINED_OFF=1`: the HACK fires on both Bowser loads (same arena offset).

Gates: swe1r full link; lockstep sa_t0..24 ALL MATCH refined on (25/25) and off (25/25); x64 builds.

### Step 3 (done) — U1 + U4

- `track_environment_refined.c`: `refined_swrObjJdge_SetupTrackEnvironment` (0x00464b90,
  equivalent) is the src/ body plus `flyby_clear()` on both early returns: `cam_spline` and
  `camSweepAnim` NULL, `camSweepCursor` zeroed, `camSweepTransform` identity (the values the
  judge's 'Load'/'RSet' reset uses). The fog/draw-distance tail the early returns skip stays skipped,
  as in retail.
- Cursor registry (`track_register_cursor` / `track_unregister_cursor`, `TRACK_LIFECYCLE_MAX_CURSORS`):
  registered cursors are zeroed at every unload. Only the judge's `camSweepCursor` is registered (on
  every SetupTrackEnvironment call). That is safe because the judge is allocated once at world init
  (`swrScene_LoadObjects`, below checkpoint 1) and never freed. Not registered yet: `swrObjJdge.cursor`,
  swrObjTest `splineCursor@0xac`, cMan cursors. They are re-seeded before use in retail, and some
  live in objects freed at race end, so each needs its owner's unregister point first.
- Interplay to keep in mind for step 5: the unload zeroes the fly-by cursor even if the restored
  sweep gate (`camSweepState`, set by the `prerace_sweep` delta) were still open. InitTrack closes it
  right after its rewind, and the StartRace rewind happens with the race torn down, so F2 never
  evaluates a zeroed cursor. The delta's EvaluateToMatrix refusal still backs that up.

Verified: crash repro Boonta Training (0) -> Big Blue (28) -> Bowser's Castle 1 (31), all deltas,
`restore_prerace_track_sweep=1`: refined on, both custom tracks log `cam_spline=00000000, cursor
spline=00000000` and leave the sweep dormant, no exception, and Boonta still plays its sweep;
`SWR_REFINED_OFF=1` logs Boonta's stale cursor spline on both custom tracks.
Gates: swe1r full link; lockstep sa_t0..24 ALL MATCH refined on (25/25) and off (25/25); x64 builds.

Oracle gaps (all steps): lockstep only covers the first race of a process, so the unload-time
resets (trigger slots, cursor registry) are machine-checked only for "no change on a first race".
Track switches are covered by the scripted `then` repro and log checks, not by hashes. A lockstep
sequence mode (step 6) would close that.

