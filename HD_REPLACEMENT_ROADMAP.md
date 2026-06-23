# SW_RACER_RE -- HD Pod Replacement / Player-Pod Association Roadmap

**Status:** Phases 0-2 PLAYTESTED GOOD + cleaned up (2026-06-20) -- pile fixed; temp diagnostics
removed; `try_replace_pod` reduced to hangar-only (dead in-race branch + `currentPlayer_Test` +
`0x00E28980` removed); builds clean; not committed. Remaining before PR: finish Phase 4 (name the
env/track root literals in renderer_hook.cpp + hangar `children.nodes[15]` path), then `/pre-pr-check`.
Reflection dropout is a SEPARATE deferred engine-level thread (see below). Living document. Owner: lightningpirate.

Goal: make the HD glTF pod replacer associate each rendered pod with the **racer entity that
actually owns it**, instead of stamping everything on the single global `currentPlayer_Test` and
guessing AI placement. This fixes the "pile of HD models riding the player" bug that appears when
the shipped `ai_full_lod` feature (PR #65) is on and more than one replacement pod model is present,
and it generalizes the pod path to splitscreen and to multiple racers sharing one character.

Lives in the `dinput_hook/` Detours layer (the OpenGL-replacement renderer): `replacements.cpp/.h`,
`node_utils.cpp/.h`, `renderer_hook.cpp`. It does NOT touch `src/` reimpls. Only compiles in the GL
takeover build (`-DRENDERER_REPLACEMENT=ON`).

> Effort tags: **S** < ~half day, **M** ~1-2 sessions, **L** multi-session. File line numbers are
> as of 2026-06-20 and must be reconfirmed at implementation time.

---

## 0. The bug, precisely

The renderer dispatches pod replacement by MODELID *class*: `NODE_BASIC + isPodModel()` ->
`try_replace_pod` (the PLAYER branch, draws at `currentPlayer_Test->engineXfR/L`+`cockpitXf`,
[replacements.cpp:782]); `NODE_SELECTOR + isAIPodModel()` -> `try_replace_AIPod` (a heuristic that
fakes engine placement with magic `6.0`/`2.5` offsets off the node matrix, [replacements.cpp:883]).

The `ai_full_lod` toggle NOPs `swrObjJdge_SpawnRacers` JNZ @0x46654d so **every racer loads the full
`_pod` model class** -- the same class the local player uses. Every AI pod node then matches
`isPodModel()` and takes the **player** branch, which draws the triggering character's body at the
**local player's** transforms. `replacedTries[model_id]` dedups per-MODELID, and each AI is a
different character, so each clears the gate and draws once -- all stacked on the player. With one
replacement gltf this is invisible (only `fileExist` pods draw); with a full set it's a pile.

So `ai_full_lod` + HD replacement are incompatible today, because gate-1 erases the player/AI
model-class distinction the MODELID dispatch depends on. The fix is to associate the rendered pod
with its racer entity, which the game already tracks.

## 1. Ghidra findings (all confirmed by decompile, 2026-06-20)

1. **Same-character racers get DISTINCT node trees.** `swrModel_LoadFromId` (~0x448xxx) never caches:
   it `swrAssetBuffer_GetBuffer()` -> decompress -> `swrAssetBuffer_SetBuffer()` (advance), every call.
   `swrObjJdge_SpawnRacers` (0x4663e0) calls it once per racer and the buffer advances per racer. Two
   racers on Anakin => two node trees at distinct pointer ranges => `find_model_id_for_node`'s range
   registry can tell them apart. Node->entity is injective by range. (Disproves the "shared instance"
   worry that earlier made a full roster-first rewrite look necessary.)
2. **Each entity stores its own pod node + roster back-pointer.** `swrObjJdge_SpawnRacer` (0x466xxx):
   `score->obj_test_ptr = player`, `player->score_ptr = score`, and the racer's pod scene node (a
   `0xd065` NODE_TRANSFORMED_WITH_PIVOT wrapper whose child is the pod model root) is stored at
   `player->unk1994_node` (types.h:580).
3. **Per-entity engine/cockpit transforms are authoritative -- for full pods only.** `swrObjTest_F3`
   (0x470610) calls `swrRace_PoddAnimateEngines(entity)` (0x470ae0) every frame for EVERY racer,
   writing that entity's `engineXfR/L`+`cockpitXf` from its own `transform`(0x20) + a char-indexed
   offset table (`DAT_004c7088 + score->unk18*0x6c`). It early-returns when
   `entity->unk344_nodeArray == 0`, so those transforms are live only for full-pod racers (player
   always; AI under `ai_full_lod`) and NOT for part-LOD AI (`ai_full_lod` off).

**Authoritative globals:** `swrScores[20]` @0xE29BC0 (each `swrScore.obj_test_ptr` -> `swrRace`),
`firstLocalPlayer` @0xE2899C / `secondLocalPlayer` @0xE27820, `currentPlayer_Test` @0x4D78A8
(== `firstLocalPlayer->obj_test_ptr`).

## 2. Decision

Take the **node->entity, traversal-driven** path, not a full roster-first rewrite. The same-character
concern is disproven, so node->entity is viable; it is a smaller, lower-risk diff and preserves the
per-viewport cull / mirror / LOD traversal that already works. Roster-first (a standalone per-viewport
draw loop) is deferred to Phase 5, where true splitscreen pods need it; both designs use the roster as
source of truth and converge.

---

## Phase 0 -- node->entity resolver (S) [CODE COMPLETE, BUILDS CLEAN 2026-06-20]

The enabler. No behavior change yet. Implemented in node_utils.{h,cpp} (`PodNodeOwner`,
`pod_node_owners`, `find_asset_range_for_node`, `first_node_in_asset_range`,
`rebuild_pod_node_owners`, `find_entity_for_node`) + per-frame gated call in renderer_hook.cpp before
`debug_render_node`. Incremental dinput.dll build links clean. Result is computed but not yet consumed
(Phase 1). Runtime-observe still pending (no log/ImGui readout added yet -- a `pod_node_owners.size()`
line in the replacement-tries panel would confirm population in race).

- `node_utils.{h,cpp}`: add `struct PodNodeOwner { char* begin; char* end; swrRace* entity; }` and
  `std::vector<PodNodeOwner> pod_node_owners`.
- `rebuild_pod_node_owners()`: iterate `swrScores[0..19]`; accept a slot only if it is
  self-consistent and live -- `e = swrScores[i].obj_test_ptr; e && e->score_ptr == &swrScores[i] &&
  e->unk1994_node` (the back-pointer check rejects stale/empty slots without needing a racer count).
  For each, find the first descendant of `e->unk1994_node` that resolves to an asset range, and
  record `{range.begin, range.end, e}`. Sort by `begin`.
- `find_entity_for_node(node)`: binary search over `pod_node_owners` (mirror of
  `find_model_id_for_node`, node_utils.cpp:91), returns `nullptr` if none.
- Add a non-aborting range lookup helper (current `find_model_id_for_node` calls `std::abort()` past
  the last range -- the descendant search must not).
- Call `rebuild_pod_node_owners()` once per frame at the top of the scene traversal in
  `swrViewport_Render_Hook` (renderer_hook.cpp, before `debug_render_node`). <=20 entries, trivial.

**Acceptance:** builds; resolver populated in-race (verify via a temporary log / ImGui count); no
behavior change.

## Phase 1 -- retarget the in-race pod draw to the owning entity (M) [CODE COMPLETE, BUILDS CLEAN 2026-06-20]

- `debug_render_node` ([renderer_hook.cpp]) resolves `swrRace* owner = find_entity_for_node(node)` for
  the NODE_BASIC isPodModel branch and, when non-null, calls the new `try_replace_pod_entity`; null
  (hangar / part-LOD AI) keeps the legacy `try_replace_pod`.
- `try_replace_pod_entity` (replacements.cpp) draws from `owner->engineXfR/L` + `owner->cockpitXf`,
  per-entity blue-flash tint, and the mirror reflection gated to the local player.
- Realization that simplified the design: `pod_node_owners` only contains FULL-pod racers, because
  bot/part-LOD AI have `unk1994_node == 0` in SpawnRacer and are skipped by the resolver. So the entity
  path is entered only when `engineXf/cockpitXf` are guaranteed live, and part-LOD AI (gate-1 off) stays
  on the existing `try_replace_AIPod` heuristic untouched -- the roadmap's "owner->transform fallback"
  is unnecessary.

**Acceptance: PLAYTESTED GOOD (2026-06-20).** With `ai_full_lod` on + two gltf pods, each draws on its
own pod, no pile on the player. Readout showed 12 owners on a 12-racer grid, all distinct
characters/entities, exactly one LOCAL (P1), 11 AI -- runtime-confirms the resolver + classification.

## Phase 2 -- drop `replacedTries` for pods (S) [DONE for the pod path]

`try_replace_pod_entity` intentionally has NO `replacedTries` guard -- the traversal visits each pod
node once per frame, so N distinct full-pod racers each draw once at their own position (this is what
kills the pile). The legacy `try_replace_pod` (hangar) and `try_replace_AIPod` (part-LOD AI) still use
the per-MODELID guard; that's fine/intended and not the pile path.

## Phase 3 -- generalize tint + mirror per entity (S)

- `compute_pod_flash_tint` already takes a `swrRace*` ([replacements.cpp:730]) -> pass `owner`. Bonus:
  AI pods flash blue on respawn too.
- Keep the mirror reflection (the `root_node->children.nodes[1]` subtree, [replacements.cpp:788])
  scoped to the local player (`owner == firstLocalPlayer->obj_test_ptr`) for now.

## Phase 4 -- kill hardcoded structural assumptions (M) [literals DONE; index path deferred]

- DONE: the `(uint32_t) root_node == 0x00E28980` race literal in `try_replace_pod` is gone (the whole
  in-race branch was removed when the function became hangar-only).
- DONE: the env/track dispatch literals in `debug_render_node` ([renderer_hook.cpp]) now use the named
  globals `&someRootNode` (race scene root, 0x00E28980) and `&someUnkRootNode` (hangar scene root,
  0x00E2A660) from globals.h. No raw scene-root address literals remain in the hook layer.
- TODO (deferred, separate PR): hangar inspection ([replacements.cpp] try_replace_pod) still uses the
  `children.nodes[15]->[0]->[2]` index path; replace with the hangar front-end's selected-pod id
  (`swrObjHang` state). RE-heavy; hangar verified working, so low urgency.

## Deferred -- Tatooine reflection dropout (NOT this work)

The HD/vanilla pod reflection on Tatooine shows ~5s then disappears for good. Proven (via temp counters,
HD-off comparison) to be the GAME dropping the `root_node->children.nodes[1]` reflection-subtree
visibility -- upstream of all rendering, byte-identical to pre-refactor logic, so NOT caused by this
work. Separate engine-level investigation; see memory `pod_reflection_engine_dropout`. Untested lead:
`ai_full_lod`-off (12 full pods may starve the engine's reflection maintenance).

## Phase 5 -- (later) roster-first standalone draw for true splitscreen pods (L)

With node->entity in place, promoting to "draw each viewport's pods from the roster loop" is a small
step; needed for per-viewport splitscreen pods (see LOCAL_MULTIPLAYER_ROADMAP.md). Convergence note:
both designs use the roster as source of truth; this just changes the draw driver from
trigger-at-node to a standalone loop.

---

## Risks / open items

- **Part-LOD AI fallback fidelity** (Phase 1 else-branch) -- acceptable, no worse than today.
- **`unk1994_node` non-null for all spawned racers** -- set unconditionally in `SpawnRacer` when
  `podModel != 0`; confirm for the bot-model branch.
- **Resolver staleness** -- per-frame rebuild + the `score_ptr` self-consistency check guard against
  entity-lifetime churn (respawn / DNF). Gate the rebuild on being in a race.
- **Verification asymmetry** -- the no-regression case (single gltf, player pod still correct) is
  testable by Lou; the pile fix needs a multi-character gltf set + `ai_full_lod` on + a RenderDoc
  capture (Tim's setup).

## Related

LOCAL_MULTIPLAYER_ROADMAP.md (per-viewport pods), GRAPHICS_ROADMAP.md, and memories
ai_fidelity_lod_subsystem, respawn_blue_flash_hd, asset_replacement_architecture,
pod_flightmodel_subsystem.
