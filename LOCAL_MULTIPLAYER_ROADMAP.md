# SW_RACER_RE — Local (Splitscreen) Multiplayer Roadmap

Roadmap for **restoring local splitscreen multiplayer**, which is non-functional on the
modern PC re-release (Steam appid 808910) even though almost the entire splitscreen system
is still present in the binary. This is distinct from `MULTIPLAYER_ROADMAP.md`, which covers
*network* (DirectPlay) play. All fixes would live in the `dinput_hook` Detours/delta layer
unless noted. Addresses are from the Ghidra DB. Effort: **S** < ~half day, **M** ~1–2 sessions,
**L** multi-session.

> **Headline finding.** Splitscreen is *not* missing — it is **starved of a second local
> player**. Every consumer of "more than one local player" (split viewports, dual cameras,
> per-half HUD, fog clamp, catch-up, per-pod input indexing) is intact and wired up. The only
> broken link is the roster builder, which hard-assigns exactly **one** `'Locl'` (local-human)
> racer. Feed the system two `'Locl'` racers and the rest is built to cascade.

---

## Root cause

`numLocalPlayers` and the splitscreen render flag `DAT_0050ccf0` are derived in
`swrObjJdge_InitTrack` (`0x00466c00`) purely by **counting roster entries whose
`swrScore.identifier` (+0x4) == `0x4c6f636c` (`'Locl'`)**, assigning each to the next local
slot (P1 `DAT_00e2899c` -> P2 `DAT_00e27820` -> P3 `DAT_00e2781c` -> P4 `DAT_00e27890`).
When the count is `> 1` it sets `DAT_0050ccf0 = 1` (splitscreen on) and friends.

The roster is built by `swrObjHang_BuildRosterMultiplayer` (`0x0045b610`), which assigns exactly
**one** `'Locl'`:

```c
// per grid slot iVar5:
if (iVar5 == DAT_004eb3b4)        // the single slot matching "my player index"
    identifier = 0x4c6f636c;      // 'Locl'  (local human)
else if (!IsHost)                 // network client
    identifier = 0x52454d4f;      // 'REMO'  (remote human)
else                              // host / offline
    identifier = 0x41414949;      // 'AAII'  (AI)   (+0x11040406 variant)
```

No path here ever emits a second `'Locl'`. Result: `numLocalPlayers` is always 1, so every
splitscreen branch downstream is skipped. (Tell: `param_1+0x70` feeds a now-empty countdown
loop immediately above this code — very likely the vestige of the console build's
"for each human player" loop that was reduced to a no-op.)

Pod type tags are set from the identifier in `swrRace_Init` (`0x00475ad0`):
`'Locl'` -> `flags0 |= 0x20`, `'REMO'` -> `0x40`, `'AAII'` -> `0x80`. The local-human gate
(`flags0 & 0x20`) and catch-up all key off this.

---

## Subsystem status — what's intact vs what's missing

| Subsystem | Status | Evidence / locus |
|-----------|--------|------------------|
| Local-player count | ✅ intact | `NumLocalPlayers()` `0x0045D350` counts the 4 slot globals; `numLocalPlayers` set in `InitTrack` |
| Splitscreen master flag | ✅ intact | `DAT_0050ccf0` set when `numLocalPlayers > 1` in `InitTrack` |
| Split viewports / layout | ✅ intact | `swrObjJdge_UpdateViewportLayout` (1P vs 2P), `swrPlayerHUD_RenderAllViewports` `0x00483d56`, `swrViewport_Render` `0x00483a9e` |
| Dual chase cameras | ✅ intact | `swrObjcMan_UpdateChaseCamera` (split offsets), `swrObjcMan_UpdateFogAndViewport` `0x00453d44` |
| Split divider / per-half HUD | ✅ intact | `swrObjJdge_DrawSplitDivider`, `UpdatePlayerHUD`, `InRaceTimer`, `HideEngineUI`, `UpdateMinimap` |
| Fog / draw-distance clamp | ✅ intact | `swrObjcMan_UpdateTerrainVisuals` `0x00451a80` clamps far plane when `>= 2` players |
| Catch-up (trailing local player) | ✅ intact | `swrRace_UpdateCatchup` `0x0046ce30` — see "Catch-up" below |
| Per-pod input indexing | ✅ intact | `swrRace_UpdatePlayerControl` `0x0046bec0` reads `DAT_00e98e80/90/a0/b0/c0` indexed by pod descriptor `unk1e70[+0x10]` (control index); type at `unk1e70+0xc -> +0x23` |
| **Two `'Locl'` in the roster** | ❌ **missing** | `swrObjHang_BuildRosterMultiplayer` `0x0045b610` emits exactly one (root cause) |
| In-race input *translation* (4-wide) | ✅ intact | `updateInRaceInputBitsets` `0x00440df0` loops **4 raw slots** (stride 0x18) -> `inRaceLocalPlayerInputBitset1/2/3` |
| UI/menu input state (per local player) | ✅ intact | `FUN_0045a460` `0x0045a460` indexes `swrUI_localPlayersInputDownBitset[player]` |
| **Device acquisition -> raw slot 1** | ❌ **the real chokepoint** | `swrControl_ProcessInputs` `0x004058e0` fills only raw slot **0** (one device). 2nd controller must be read into slot 1. |
| **"2 player" front-end / P2 char select** | ❌ missing | PC menu has no 2-player entry; no P2 profile/pod assignment path |

---

## Catch-up in local MP (already mapped — auto-activates)

`swrRace_UpdateCatchup` (`0x0046ce30`) gives the **trailing local human** a rubber-band boost,
gated on `NumLocalPlayers() > 1` (splitscreen only) and `flags0 & 0x20` (local human):

- multiplier = `1.0 + gap * L / 5000`, **capped at 1.25 (+25% top speed)**; leader gets exactly 1.0.
- `gap` is pod `+0x130`, written by `swrObjJdge_UpdateStandings` (`0x0045d4a0`): the trailing
  human's `+0x130 = leaderRank - trailerRank` (positive), the leader's `+0x130 = 0`.
  Rank value = laps + fractional lap progress (`GetRacerProgress` `0x0045d410`); `L` is a
  per-track spline value (`_DAT_004c7be0`, written by `swrSpline_TraceProgress`).
- The same multiplier (`multiplayerStats` @ `0x22c`, mirrored to `speedMultiplier` @ `0x1ac`)
  also feeds **grip**: `swrRace_ApplyTraction` (`0x00478a70`) cuts the velocity-direction
  retention by `(2.0 - multiplayerStats)` -> tighter cornering for the player who's behind.
- AI pods (`flags0 & 0x80`) get a separate two-sided rubber-band set in `swrRace_AI`
  (`0x0046b670`); that runs in all modes and reads the same `0x130`/`0x134` gap fields.

Implication: once two `'Locl'` racers exist, catch-up "just works" for free.

### QoL: make catch-up a toggle (+ strength)  (S–M, 2026-06-16)

Catch-up is currently **always-on** for splitscreen and hard-coded (cap 1.25x, `L`/divisor fixed).
Expose it as a settings/host toggle and an optional strength slider (scale the cap / `gap*L`
factor). Cheap because `UpdateCatchup` (`0x0046ce30`) is the single locus. Same feature applies in
three contexts (record once, reuse): **single-player** human-vs-AI (lift the `NumLocalPlayers() > 1`
gate so the lone human gets the trailing boost — addresses the AI-runaway "feel", see
`AI_ROADMAP.md` C5), **local co-op** (toggle/strength), and **online** (host-toggled — see
`MULTIPLAYER_ROADMAP.md`). The deficit field `gap = +0x130` is written by `swrObjJdge_UpdateStandings`
(`0x0045d4a0`) for every pod, so the input already exists in all modes; only the `NumLocalPlayers`
gate restricts WHO consumes it. NOTE: the multiplier also tightens cornering grip via
`swrRace_ApplyTraction` (`2.0 - multiplayerStats`), so a strength slider affects handling, not just
top speed — tune together.

---

## Restoration plan

| Phase | Item | Effort | Notes / locus |
|-------|------|--------|---------------|
| **P1** | **Force a 2nd `'Locl'` in the roster** | S–M | The one essential change. Hook `swrObjHang_BuildRosterMultiplayer` (`0x0045b610`) or post-patch `swrScore.identifier` fields before `InitTrack` counts them. Expect `numLocalPlayers`->2, `DAT_0050ccf0`->1, split render + catch-up to cascade. **Run this first as a throwaway probe** to confirm the cascade. |
| **P2** | **2nd input device -> raw slot 1** | M | The real chokepoint (see Pitfalls). Translation + UI-state layers are already 4-wide; only **device acquisition** is single. Assign P2 pod descriptor `unk1e70[+0x10] = 1`, and make `swrControl_ProcessInputs` (`0x004058e0`) read a 2nd controller into raw slot 1 (the buffer `updateInRaceInputBitsets` already loops 4-wide). See `input_subsystem` memory. |
| **P3** | **2-player selection + P2 character/pod** | M | PC menu has no 2-player entry. MVP: imgui toggle / hotkey in `dinput_hook` that enables splitscreen and auto-assigns P2 a default pod + control index 1. Proper: restore a front-end menu state. P2 upgrades can be stubbed (MP load path already forces fixed upgrades: `FUN_0045b290`). |
| **P4** | **Edge-case verification** | M | `UpdateViewportLayout` for 2 (verify 3–4 sanity — N64 was 2P only; **target 2P first**); results/standings with two locals (cf. `MULTIPLAYER_ROADMAP.md` B7); pause/menu ownership; per-player fixed-array bounds (cf. roadmap Q5). |

### Suggested sequencing / MVP
1. **P1 probe** — hardcode two `'Locl'`, hardcode P2 pod + control index 1, even sharing/splitting
   keyboard input. Goal: confirm the split renders and both pods drive.
2. If the cascade lights up (the code strongly implies it will), **P2** becomes the real
   engineering (device binding), then **P3** (minimal menu), then **P4** (polish).

Realistic estimate: **a few sessions for a playable 2-player splitscreen**, almost all of it in
input binding + a minimal menu — not in the race/render engine, which is already built for it.

---

## Pitfalls & scope

### 1. "The game only accepts one input" — true only at the device layer
Verified the full chain. The in-race input pipeline is **4 local players wide** from the raw
buffer onward: `updateInRaceInputBitsets` (`0x00440df0`) loops 4 raw slots (stride `0x18`) into
`inRaceLocalPlayerInputBitset1/2/3`; menu/UI input is per-player (`swrUI_localPlayersInputDownBitset[player]`
in `FUN_0045a460`); pod control reads its own slot via the descriptor control index
(`unk1e70[+0x10]`). The **only** single-player assumption is **device acquisition**:
`swrControl_ProcessInputs` (`0x004058e0`) fills raw slot **0** from one device. So restoring
splitscreen input is a localized change (feed a 2nd controller into raw slot 1), **not** a
pipeline rewrite. This is the #1 thing to de-risk after the P1 roster probe.
*Caveat:* haven't yet confirmed whether DirectInput device *enumeration* already discovers a 2nd
pad or whether that also needs adding — check `swrControl_ProcessInputs` / device init.

### 2. Online + splitscreen together — OUT OF SCOPE (separate, hard)
The offline-splitscreen plan does **not** enable two local humans inside a *network* race, and
that combination is a much deeper problem:
- The roster builder is `swrObjHang_BuildRosterMultiplayer`; on a network **client** it tags
  exactly one slot `'Locl'` (the slot `== DAT_004eb3b4`) and every other slot `'REMO'`.
- `DAT_004eb3b4` ("my player index") is assigned **once per machine** by
  `swrMultiplayer_CreateSession` (`0x0041c722`). The protocol model is **one local player per
  connection** — racer slots are keyed to DirectPlay players.
- Supporting netplay splitscreen would require advertising 2 local racers from a single
  connection and the host accounting for more racers than connections (roster sync, ready/pick
  flow, per-player event routing). That's a netcode feature, tracked separately in
  `MULTIPLAYER_ROADMAP.md`, **not** here.
- Conversely: forcing a 2nd `'Locl'` for **offline** splitscreen does not touch the netcode path,
  so the two efforts are independent.

### 3. "The player" singletons
Several systems treat P1 (`DAT_00e2899c`) as *the* player (camera ownership, pause/menu, save &
profile writes, results focus). With two locals these need auditing so P2 isn't ignored or P1's
profile isn't overwritten by P2's results. Folded into Open questions + P4.

---

## Open questions / to verify in-game

- Does `swrObjHang_BuildRosterMultiplayer` run for pure offline races, or only via the MP setup
  path? Its name suggests MP origin, yet it sets `score.identifier` for all slots and the
  `SetRosterEntry` calls are network-sync only — so it may be the universal roster builder, or
  offline splitscreen may need to ride a local "session" setup. **Confirm offline.**
- How many local viewports does `UpdateViewportLayout` actually support (2 vs 3–4)?
- Does `swrControl_ProcessInputs` / DirectInput device init already *enumerate* a 2nd controller
  (so we only need to route it to raw slot 1), or must enumeration be added too?
- Does anything else implicitly assume `numLocalPlayers == 1` (camera ownership, pause/menu,
  results focus, save/profile writes keyed to P1 `DAT_00e2899c`)? See Pitfall #3.

## Address quick-reference

```
swrObjHang_BuildRosterMultiplayer 0x0045b610  roster builder — single 'Locl' (ROOT CAUSE)
DAT_004eb3b4                            "my" local player index (the only slot tagged 'Locl')
swrMultiplayer_CreateSession 0x0041c5c0  assigns DAT_004eb3b4 (one local index per machine)
swrObjJdge_InitTrack       0x00466c00   counts 'Locl' -> numLocalPlayers, sets DAT_0050ccf0
NumLocalPlayers            0x0045D350   counts P1..P4 slot globals (e2899c/27820/2781c/27890)
DAT_0050ccf0                            splitscreen-render master flag
swrObjJdge_UpdateViewportLayout         1P vs 2P viewport/camera layout
swrPlayerHUD_RenderAllViewports 0x00483d56
swrViewport_Render         0x00483a9e
swrObjcMan_UpdateTerrainVisuals 0x00451a80   fog/draw-distance clamp in splitscreen
swrRace_UpdatePlayerControl 0x0046bec0  per-pod input via descriptor unk1e70[+0x10]
swrControl_ProcessInputs   0x004058e0   device read -> raw input slot 0 ONLY (input chokepoint)
updateInRaceInputBitsets   0x00440df0   translates 4 raw slots -> per-player bitsets (4-wide)
FUN_0045a460               0x0045a460   per-player menu/UI input state (swrUI_localPlayers*)
swrRace_Init               0x00475ad0   identifier->flags0: Locl=0x20 REMO=0x40 AAII=0x80
swrRace_UpdateCatchup      0x0046ce30   trailing-local catch-up (cap 1.25), splitscreen-gated
swrRace_ApplyTraction      0x00478a70   reads multiplayerStats -> grip
swrObjJdge_UpdateStandings 0x0045d4a0   writes gap@+0x130 (leader 0 / trailer positive)
FUN_0045b290                            MP load path forces fixed pod upgrades
```

---

## Spike log

### 2026-06-18 - P1 probe (force 2nd 'Locl') - CASCADE CONFIRMED; renderer-replacement gaps found

Implemented P1 as an off-by-default ImGui toggle `swrObjJdge_forceSplitscreen` that stamps
`scores[1].identifier = 'Locl'` inside the existing `swrObjJdge_InitTrack_delta`, *before*
`hook_call_original`. Hooking the counter (InitTrack) rather than the producer
(BuildRosterMultiplayer) sidesteps the open question of whether the roster builder runs offline -
the splitscreen flags key only off the `'Locl'` count. Built + playtested on the OpenGL
**renderer-replacement** build.

**Confirmed:** the P1 thesis holds. With the toggle on (freeplay + >=1 AI), the **HUD splits** -
two lap counters, times, and speedometers. So `numLocalPlayers -> 2` and `DAT_0050ccf0 -> 1`, and
the per-half HUD cascade fires exactly as predicted.

**Three failures observed - all in the OpenGL renderer replacement, NOT the roster/cascade logic:**

1. **Only one viewport renders (P1's camera, full-screen).** Root-caused: `swrPlayerHUD_RenderAllViewports`
   (`0x00483ed5`, the single caller of `swrViewport_Render` `0x00483A90`) *does* iterate the
   viewport array and call `swrViewport_Render(x)` for every active (`flag&1`) slot - so our
   `swrViewport_Render_Hook(x)` is genuinely invoked for both viewports. The bug is in the hook:
   it reads only `glGetIntegerv(GL_VIEWPORT)[2],[3]` (w,h) at `renderer_hook.cpp:844`, **discards
   the x,y offset**, and blits every viewport to the screen origin
   `glBlitFramebuffer(0,0,w,h, 0,0,w,h, ...)` at `renderer_hook.cpp:1001`. So each viewport
   overwrites the same screen region; the last one rendered wins the full screen.
2. **Camera is hardcoded to `swrViewport_array[1]`** for the PBR `cameraWorldPosition` uniform
   (`renderer_utils.cpp:438`) and the env FBO (`renderer_hook.cpp:539`) - the replacement is
   structurally single-camera.
3. **Pod models invisible** (shadows + jet burners still render) and **game stuck paused**
   (unpausing repauses). Unresolved. Leading hypothesis: the empty raw input **slot 1** (the P2
   device chokepoint - `swrControl_ProcessInputs` fills only slot 0) feeds garbage into a
   pause/menu bit. `updateInRaceInputBitsets` (`0x00440df0`) derives 4 per-player bitsets from a
   raw slot table (stride `0x18`); slot 1 is never cleared by a device read.

**Fix shape for HD splitscreen (now scoped, ~1 session):** in `swrViewport_Render_Hook(x)` capture
the full GL viewport rect (incl. x,y), set `glViewport` to the `swrViewport_array[x]` screen
rectangle, blit each viewport to its screen sub-rect (not origin), and parameterize the hardcoded
camera reads by `x`. Separately, zero/feed raw input slot 1 (overlaps P2 input work).

**Cheap disambiguator (untested):** build with `RENDERER_REPLACEMENT=OFF` (native dgVoodoo path) -
the whole native split path is intact, so it should render the split correctly, and it isolates
whether the stuck-pause is a renderer issue or a genuine roster/singleton bug (Pitfall #3).

### 2026-06-18 (cont.) - GL split renders; pause bug root-caused; padding fixed

- **GL renderer now splits.** Made `swrViewport_Render_Hook(x)` per-viewport-aware: compute each
  viewport's screen sub-rect from its native `viewport_x1..viewport_y2` corners, `glViewport` +
  project + blit into that sub-rect, restore the full viewport afterward for the sprite/HUD passes.
  Gated on `swrViewport_array[2].flag & 1` (the engine's own splitscreen tell) so single-player is
  byte-identical. PLAYTESTED: top/bottom split renders.
- **Padding fixed.** Viewports live inside a `(8,8)-(312,232)` content area of the 320x240 design
  space (outer border = HUD margin). Mapping the *content area* (not the raw 320x240 frame) to the
  full framebuffer makes the split fill the window edge-to-edge. (awaiting re-playtest)
- **Stuck-pause ROOT-CAUSED (disasm-verified) - it is a binary bug, not input garbage.**
  `KeyDownForPlayer1Or2` (`0x0045e120`, "is `mask` down for local player 0 or 1") has a fall-through
  at `0x0045e19c`: when `numLocalPlayers >= 2`, not paused, and *neither* player is pressing, it
  returns `EAX = param_1` (the nonzero mask) instead of 0. So every caller (pause via
  `swrObjJdge_CheckIfPauseRequested` 0x462d40 -> `pollPauseInput` 0x4457d0 -> `requestPause`;
  HUD-cycle `swrObjJdge_CycleHudMode`; `swrRace_UpdateInRaceMenu`) fires every frame in 2P ->
  permanent re-pause. FIX (shipped, awaiting playtest): `KeyDownForPlayer1Or2_delta` in
  swrObjJdge_delta.cpp wraps the original and forces 0 when numLocalPlayers>=2 and neither
  `inRaceLocalPlayerInputBitset1[0]` nor `[1]` has the bit. Inert in single-player. This supersedes
  the earlier "empty slot 1 garbage" hypothesis as the primary cause.
- **Padding + pause fixes PLAYTESTED GOOD:** full-bleed split renders, player 1 is driveable.
- **Remaining two issues share ONE root cause: the GL renderer replacement is architected for a
  single player.** (Both reproduce; neither is a roster/cascade problem.)
  - **Both halves show player 1's camera.** `swrViewport_Render_Hook` builds its view matrix from
    the global `rdCamera_pCurCamera->view_matrix` (never set in the hook layer -- a single camera)
    and hardcodes the camera *position* to `swrViewport_array[1]` (renderer_utils.cpp:438,
    renderer_hook.cpp:539). The per-viewport camera is available (`swrViewport_array[x].model_matrix`,
    or `unkCameraArray[vp.unkCameraIndex].transform` = the computed view matrix) but unused.
    FIX: derive the view matrix + camera position from the current viewport `x`, not the globals.
  - **Pod models invisible (both pods).** `try_replace_pod` (replacements.cpp:741) always draws
    `currentPlayer_Test` (single global player pod @0x4d78a8) and guards on `replacedTries[model_id]`
    (per-frame, reset at the swap in `stdDisplay_Update_Hook`). So the HD pod path is single-player:
    it can only ever draw player 1's pod, once per frame. (The visible cables/jets/shadows are
    separate GL-generated/sprite passes, not the glTF body draw.) FIX: draw the *current viewport's*
    player pod (firstLocalPlayer/secondLocalPlayer -> their swrRace*), and key/reset the guard
    per-viewport rather than per-frame.
- **Spike verdict:** P1 thesis fully proven and the native split cascade works end-to-end. Getting a
  *true* playable HD splitscreen (each half its own camera + pod) is the next chunk = parameterize
  the GL replacement's camera + HD-pod path by viewport/player. Not spike-sized; it's the real
  renderer-side feature work, now precisely scoped to the two loci above.

### 2026-06-18 (final) - SHIPPED: seamless split + per-viewport camera + pause fix; pod-body gap diagnosed

Resolved most of the above and **landed a playable splitscreen** (all on branch `proto/corkscrew-magnet`,
built + playtested). Net state of `swrViewport_Render_Hook` / `swrObjJdge_delta`:

- **Per-viewport camera (FIXED).** Source the view matrix from the ortho-inverse of
  `swrViewport_array[x].model_matrix` (`rdMatrix_InvertOrtho34`, the same construction
  `rdCamera_Update` uses) instead of the single global `rdCamera_pCurCamera->view_matrix`. Each half
  now tracks its own player. (Earlier `vp.unk_mat3` attempt was the wrong matrix -> "cameras all over
  the place"; `model_matrix` inverse is correct.)
- **Seamless 50/50 split (FIXED).** Map the `(8,8)-(312,232)` content area to the full framebuffer
  AND snap inner edges near the midline to exactly 0.5, removing the native HUD-margin gap (console
  has none).
- **Pause/HUD-cycle bug (FIXED).** `KeyDownForPlayer1Or2_delta` (see prior entry). Race is driveable.
- **Per-viewport HD pod target (DONE, dormant here).** In split, retarget `currentPlayer_Test` to each
  viewport's local player + reset `replacedTries` per viewport so each half would draw its own HD pod.
  Untestable this build -- **no HD `.gltf` pod models are present**, so every pod uses the vanilla mesh
  path (`fileExist=0`).
- **STILL OPEN -- vanilla pod engine/cockpit invisible in split.** Conclusively narrowed (NOT cull,
  camera, HD path, or roster): the pod (e.g. `Sebulba_pod`) IS fully traversed by `debug_render_node`
  in the split viewport (BASIC 0x5064 -> TRANSFORMED_WITH_PIVOT 0xD065 -> MESH_GROUP 0x3064),
  `find_model_id_for_node` resolves it (pointer-range registry, identical to 1P), and
  `debug_render_mesh` IS called for the engine/cockpit meshes. They are **submitted to GL but not
  visible** -- a render-state/transform nuance specific to the split pass (NOT a traversal/cull/flag
  issue; disabling the cull entirely in split changed nothing). The separately-drawn
  cables/jets/binders render fine. NEXT: capture one split frame in **RenderDoc/apitrace** to see if
  the pod draw is off-screen / zero-scale / depth-failed / overwritten -- print-debugging stalled
  here (~6 attempts). The node-flag cull masks for split viewports are `exact=0x16` (adds a `0x10`
  per-viewport tag bit over the usual `0x6`) / `any=0xFFFFE300` (segment mask), set by
  `swrObjcMan_UpdateCamera` (flag 4) + `swrViewport_SetNodeFlagsForAllViewports` (flag 6) via
  `swrViewport_SetNodeFlags` 0x00431a10 -- recorded in case the frame capture points back here.
- **NOT YET DONE -- P2 input** (the other half of "playable"): route a 2nd device into raw input slot
  1 (`swrControl_ProcessInputs` fills slot 0 only). The in-race translation + per-pod control index
  are already 4-wide; only device acquisition is single. See P2 in the Restoration plan.
