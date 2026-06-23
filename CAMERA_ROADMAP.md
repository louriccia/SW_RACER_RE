# SW_RACER_RE — Camera System Roadmap

**Status:** design (2026-06-12). Living document. Owner: lightningpirate.

Goal: a **sophisticated camera system** for SWE1R — free camera, configurable chase cameras,
focus/spectator + orbit, photo mode, dynamic (speed-reactive) camera + shake, a broadcast/
director overlay, and a camera that can follow a replay ghost — built on the already-mapped
`cMan` subsystem and a clean reimplementation of the author's existing CE camera mod.

Lives in the `dinput_hook/` Detours layer, **not** `src/`.

> **Addresses below are from the Ghidra DB, the author's CE camera mod, and annodue, and MUST
> be reconfirmed against the live DB at implementation time** (Steam `.text` is SteamStub-encrypted
> on disk). The CE addresses are validated to this build family (`swrRace` base `0xE29C44` matches
> `types.h`). Effort: **S** < ~half day, **M** ~1–2 sessions, **L** multi-session.

---

## 0. Decisions locked (2026-06-12)

- **North star: a full director / broadcast platform** — not just a player toolkit. End state makes
  Racer streamable/castable: freecam + photo + configurable chase + replay-follow + the esports
  overlay (standings, minimap, spectator cycle), the way the author's CE mod did.
- **First deliverable: the free camera (Phase 1)** — the spine every other capability reuses.
- **Architecture: a `dinput_hook/camera/` delta-layer controller** (same home as tracks/replay/asset
  mods), NOT a `src/` reimpl. It owns a render-only camera mode, drives the camera-state each frame,
  and captures input via `swrControl`; matrix/quat math (no hand-rolled Euler). The faithful
  `src/cMan` decomp stays a separate track.
- **Phase-0 RE foundation: DONE** (upstream PR #80 + Ghidra DB names): `swrViewport_CameraStates` +
  the camera-state struct (`swrCamera_unk`), the view-matrix writer (`swrViewport_UpdateCameras`),
  time-control (`swr_frameDeltaTime` / `swr_FastMode`), spectator table (`swrObjcMan_SpectatorCamModes`).
- **Camera rig model (see §14):** every mode is one rig — `attach-to` (position) x `look-at` (aim),
  each a `[free <-> locked]` toggle over a single shared focus target; presets = saved rig snapshots.
- **Controls (see §15):** unified gamepad + keyboard; free is a per-anchor toggle + a one-press `Free`
  escape; manual stick/mouse auto-breaks the lock (auto-free on override).
- **Operating layers / personas (see §16):** L1 free-fly utility (speedrunner), L2 live director
  (broadcaster), L3 cinematic studio (content creator) — same rig, three layers.
- **Replay Studio (see §17):** camera x replay converge into a keyframe-timeline studio (L3); shared
  milestone with `REPLAY_ROADMAP`.

---

## 1. The core realization

The behavior half of the camera (`swrObjcMan`) is **already mapped**, and the author has already
built a **near-complete camera suite** as a Cheat Engine mod. The decomp's job is not to invent
the feature set — it's to **reimplement that mod cleanly on the cMan structs** (and portably, in
the delta layer) instead of raw matrix pokes + opcode NOPs, then extend it. Three references:

1. **The author's CE "Director/Broadcast" camera mod** — the most complete prior art (centerpiece).
2. **annodue Cam7** — a shipping free camera (secondary cross-reference).
3. **The decomp `cMan` map** — the function-level dispatch to hook/reimplement.

---

## 2. Prior art A — the author's CE camera mod (the centerpiece)

A Cheat Engine Lua mod (separate from `CheatTable_TRAINER.CT`, which is the stats trainer). It is a
full **Director / Broadcast** camera suite. Architecture:

### 2.1 Mode state machine (`cameraMode`)
- **0 player** (normal tethered cam) · **1 freecam** (6DOF) · **2 focus** (chase/follow a chosen
  racer) · **3 attach** (orbit / hard-attach / soft-attach) · **photo** (pause + freecam).
- **Hard vs soft attach:** restore vs **NOP the engine's view-matrix opcodes** (`0x429602–0x42963D`)
  to make camera orientation rigidly follow the pod vs. be independent.
- **Orbit:** auto-follows the focused pod with a projected radius/pitch (`RadiusProj`/`PitchAngProj`),
  smoothed; numpad presets (`viewCloseFront/Back`, `viewMid*`, `viewFar*`) for broadcast angles.
- **Autozoom** (zoom by camera-to-pod distance), **autofollow**, **dynamic FOV** (widens past speed
  650 + pushes camera forward on boost), **perlin-noise camera shake** (speed/boost/vibration-scaled),
  **true cockpit cam** (per-pod `CockpitY/Z` offset tables, FOV 1.2).
- **Broadcast/Director mode:** an esports overlay — live standings, per-track minimap (scale/offset
  tables), lap times + gaps, spectator focus cycling, MP name/pod readout, results screen.

### 2.2 The camera write surface (the reusable RE)
| What | Address | Note |
|---|---|---|
| **Render camera matrix** | rot `0xDFB1DC` (rows stride 0x10), pos `0xDFB20C/210/214` | the mod writes the view matrix here directly each tick |
| **Active camera object ptr** | `0xE9ADAC` → `+0x30` pos, `+0x00/04/08/18/28` rot basis | the camera struct the renderer reads |
| **Camera focus/look ptr** | `0xE9ADB4` (saved as `pFocusOriginalPointer`, restored on exit) | what the cam follows; null-restore = clean takeover |
| **Non-player cam enable / target** | `0xE9AAC0` = 1, `0xE9AAC4` = target ptr | used by focus + cockpit cam |
| **Cam enable/type toggle** | `0xDFB1B0` (1 or 7) | |
| **cMan `mode_type`** | `[0x4BFE80]+0x7C` (1=tether, 3=untether, 7=sweep) | matches `swrObjcMan_UpdateCamera` dispatch |
| **Engine view-matrix writer (YPR opcodes)** | `0x429602`–`0x42963D` (inside `swrViewport_UpdateCameras`; array = **`swrViewport_CameraStates`** @ ptr `0x4b91c4`, stride `0x7c`, mat `+0x14`) | NOP = decouple cam orientation; restore = rigid follow. **Key find** (named + commented in DB 2026-06-12). |
| **Game timestep** | `swr_frameDeltaTime` `0xE22A40` (double) + `swr_useFixedDeltaTime` `0x50CB68` | slow-mo / fast / pause-scrub (also serves replay + photo); named in DB 2026-06-12 |
| **Camera-roll (banking) params** | mem records `PlayerCameraRollMod1` (0.00555555569) / `2` (1.625) | set 0 to disable roll |
| **Visibility** | pod `0x46D3CF`, smoke/dust `0xEC86B4`, reflections `0xEC86A0`, UI `0x4ACCA3`, arrow `0x453633`, pos-display `0x42C03F` | clean-cam toggles |
| **Mouse look** | delta `0xEC87D8/DC`, scroll `DINPUT.dll+1F428`, sens `0x50F53C/554`, axis `0xEC8790/94`, reset-code `0x405438`, enable `0x4D6B38` | |
| **Input ownership** | controller enable `0x4B2944`, kbd-game patch `0x486297`, **game-focus flag `DINPUT.dll+1F438`** | route input to cam vs pod |
| **Pause** | `0x50C5F0` (1/0) + PauseMenu records | photo mode |

FOV is baked into the matrix via a scale term (`camSize`/`Zoom`/`FOV`) — janky; a clean reimpl
should set FOV properly (cross-ref the widescreen-UI FOV/aspect work).

### 2.3 The experimental ghost replay (commented `thread2()`)
The mod contains a disabled prototype that **records the player's per-frame transform** (the 16-float
matrix at `[pod]+0x20`, plus speed) into Lua tables during lap 1, then **plays it back onto pod[1]
(`0xE29CCC`)** as a ghost while the player drives laps 2–3. This is a working **Tier-A ghost via
phantom-entity** prototype — it directly validates `REPLAY_ROADMAP.md` (§5.2 P2/P3) and is the
strongest evidence that the replay-camera and replay-ghost work belong together.

### 2.4 Init-block cross-confirmations (bonus RE)
The mod's init validates addresses from other subsystems: network-update time `SWEP1RCR.EXE+B6718`
(= `Main_nut_delay_ms`), AI/LOD NOPs `+6654D` / `+47F916` (= `ai-fidelity-lod` mods), cable/cockpit
toggle limit `+42F91B`, alt-tab fixes (`0x423B55`, `0x4804B4`, `0x50CB64`, `a3dapi.dll+23EF9`),
draw distance `[+BFE80]+2D4`. **Note a struct discrepancy to resolve:** the mod indexes a per-racer
participant table at stride **`0x88`** from `~0xE29C1C` (placement `+0x1C` region, lap-times block,
lap counter `+0x38`, and a `swrRace*` at `0xE29C44`), which does not match `types.h`'s single
`swrRace @0xE29C44 sizeof 0x1f28`. Worth reconciling (ties to the known `swrRace` sizeof bug).

---

## 3. Prior art B — annodue Cam7 (secondary)

annodue `src/patch/dll_cam7.zig`: one FreeCam mode via CAMSTATE **slot 31** (item xf `+0x14`),
`camstate_ref` `0x4539A0+0x170`; **FOV patch `0x4528EF`** (120->100); fog dist 7500/remove;
**visual-flags patch `0x453FA1`**; **wind SFX id 28** scaled to speed; orbit/planar/move-pod-to-cam/
look-at-vehicle; graceful exit on scene change. Useful contrasts: Cam7 handles the **audio wind
loop** and a clean **scene-transition restore** the CE mod is rougher on; the CE mod has **broadcast,
photo, dynamic cam, shake, and spectator** that Cam7 lacks.

---

## 4. The decomp `cMan` map (the clean substrate)

`swrObjcMan_UpdateCamera` (`0x453e00`) per-frame dispatch on `mode_type` (cMan+`0x7c`):
`1,2` chase (`0x452aa0`); `4,5` first-person (`0x4528b0`); `6` spline (`0x4533a0`); `7` pre-race
sweep (`0x451ef0`/`0x4525d0`); `8,9` death (`0x452600`). Helpers `CommitStagedCamera` (`0x451d60`),
`RestoreMode` (`0x451ec0`), `UpdateFogAndViewport` (`0x4538d0`), `swrCam_CamState_InitMainMat4`
(`0x428a60`). `swrViewport_*` render half done; cMan struct mapped in `types.h`. The decomp lets us
add a **new "free/render-only" mode** to the dispatch instead of NOPing opcodes.

---

## 5. Feature roadmap

Layer column = persona operating layer (see §16). All phases sit on the one rig (§14) + controls (§15).

| Phase | Layer | Scope | Effort | Depends |
|---|---|---|---|---|
| **0** | L1/all | **Player-camera settings panel** (FIRST ship) — roll/height/trail/dynamic-FOV/shake/cockpit + UI & visibility toggles. RE foundation DONE (PR #80); breakdown §11, impl §13. | S–M | — |
| **1** | L1 | **Free camera** — the two-anchor rig (§14) + controls (§15): 6DOF, look, speed/smoothing, planar/orbit, FOV, audio-follow (free), restore. **Pod handoff:** move-pod-to-cam, return-to-pod, noclip, practice/run gate. | M | cMan map |
| **2** | L2 | **Focus / spectator + orbit** — single focus-target cycle, orbit/tracking presets, autozoom. **Lifecycle robustness:** target finish/explode/disconnect -> auto-fallback (never a broken shot). | M | 1 |
| **3** | L1 | **Photo mode** — time-freeze (`swr_FastMode`) + freecam + FOV + hide-UI + screenshot. | S–M | 1 |
| **4** | L1 | **Dynamic cam + shake** — speed-reactive FOV + perlin shake; folds into the Phase-0 panel. | M | 1 |
| **5** | L2 | **Auto-director** — event-driven cut/track (leader / closest battle / overtakes / finish, from `swrObjJdge` signals) + **clean player-feed** (mirror a player's exact camera + HUD as their own footage). | M–L | 2 |
| **6** | L2 | **Broadcast/director overlay** — standings / minimap / gaps / spectator HUD. | L | 2,5 |
| **7** | L2/L3 | **Replay-follow camera** — live camera follows a replay ghost (focus + spline modes). | M | 1; replay Tier A |
| **8** | L3 | **Replay Studio** (§17) — camera keyframe timeline + easing/spline + FOV curves, synced to replay; slomo/scrub. The camera x replay convergence. | L | replay Tier A/B |
| **9** | all | **Modder camera API** — per-frame camera-control hook for replay/cutscene mods. | M | 1 |

---

## 6. Roadblocks (anticipated, now informed by the CE mod)

- **View-matrix ownership.** The CE mod NOPs the engine's view-matrix writer (`0x429602–0x42963D`)
  to take control. A clean decomp should instead **own a cMan mode** (add/replace a dispatch case)
  so the engine cooperates rather than being patched mid-instruction — avoids fragility across builds.
- **Audio listener coupling.** Engine/ambient SFX are pod-spatial; a free camera desyncs the 3D
  listener. Cam7 masks this with a wind loop; do it right — move the listener to the camera (find
  the A3D listener-position write; see `swrSound`).
- **Culling / LOD / fog when the camera leaves bounds** — the CE mod cranks draw distance + fog and
  the AI/LOD NOPs for exactly this reason. Free/broadcast cameras need the LOD/fog handling.
- **Input ownership.** Free/photo/broadcast cam must capture mouse+pad without driving the pod
  (the CE mod toggles `0x4B2944`/`0x486297` and gates on `DINPUT.dll+1F438`). Route through
  `swrControl` in the decomp instead of byte-patching.
- **Time-control coupling.** Scaling `0xE22A40` affects the whole sim (good for photo/slow-mo, but
  it is global) — coordinate with physics determinism concerns in `REPLAY_ROADMAP` Tier C.
- **Representation.** The CE mod builds Euler->matrix by hand each tick (gimbal-lock prone). Prefer
  matrix/quat on the cMan struct for smooth orbit + replay interpolation.
- **Struct/stride discrepancy** (`0x88` participant table vs `swrRace 0x1f28`) — resolve before
  relying on per-racer offsets in code.
- **MP / focus pointer safety** — focus/spectator writes a focus pointer (`0xE9ADB4`); must validate
  the target exists and restore on race end / scene change.

---

## 7. Interplay with the replay system

Tightly coupled (and the same author prototyped both):
- The CE mod's commented `thread2()` is a **Tier-A ghost replay** (records transform, replays onto
  pod[1]) — direct prior art for `REPLAY_ROADMAP` §5.2 P2/P3.
- Replay playback needs **Phase 5 here** (a camera that follows a non-player ghost) to be watchable.
- `0xE22A40` time control serves photo mode, replay scrubbing, and slow-mo alike.

Suggested order: Camera Phase 0–1 (config + freecam) land early and independently; Phase 5
(replay-follow) follows replay Tier A; Phase 6 (broadcast) is its own large track.

---

## 8. References
- The author's CE camera mod (Director/Broadcast Lua) — pasted into session 2026-06-12; archive it
  alongside `CheatTable_TRAINER.CT`.
- Memory: `camera_cman_subsystem.md` (cMan dispatch + handlers + `swrCam.h`).
- `src/Swr/swrObj.h` `swrObjcMan_*`; `src/Swr/swrCam.h`.
- annodue `src/patch/dll_cam7.zig` (CAMSTATE slot 31, FOV `0x4528EF`, fog, visual-flags `0x453FA1`, wind SFX 28).
- `REPLAY_ROADMAP.md` (replay-follow camera; the ghost prototype) · `MULTIPLAYER_ROADMAP.md` (B5 alt-tab cam) · widescreen-UI (FOV/aspect) · `ai-fidelity-lod` (LOD/fog when cam leaves bounds).

---

## 9. Phase 1 — free camera (detailed plan)

Home: new `dinput_hook/camera/camera.{cpp,h}`, driven from the existing per-frame render hook
(`renderer_hook.cpp`). Delta layer, gated by a toggle.

1. **Per-frame hook + toggle.** Run the controller each frame from the render hook; bind a toggle
   key. Active = take over; off = restore.
2. **Takeover + restore.** On enter: save the active camera-state index (`DAT_0050c038`), the focus
   pointer (`0xE9ADB4`), and cMan `mode_type`. Drive the active `swrViewport_CameraStates[idx]`
   transform (`+0x14`) directly as a render-only camera. On exit / scene change: restore saved state.
3. **Input ownership.** Capture mouse delta (`0xEC87D8/DC`) + keys/stick; suppress pod control while
   active via `swrControl` (clean gate, not byte-patches); gate on the game-focus flag.
4. **Camera math.** 6DOF translate + quaternion look (no Euler/gimbal), speed presets + smoothing,
   planar + orbit modes, proper FOV (not matrix-scale). Move-pod-to-cam + look-at-vehicle.
5. **Audio + visuals.** Lift draw distance + fog when the cam leaves pod bounds. **Audio-follow is
   FREE** — the A3D listener is derived from the active camera-state matrix (RE-resolved below), so it
   tracks whatever freecam writes; no listener override needed.
6. **ImGui panel.** Toggle, speed/smoothing/FOV sliders, hide-UI, move-pod-to-cam / look-at buttons
   (this is also where the Phase-0 chase-cam knobs live).

**Spike first:** steps 1–2 + basic WASD/look = a "moves and restores cleanly" proof, then layer 3–6.
Needs in-game testing (game running) — NOT worktree/headless-verifiable (delta layer pulls `modules/`
deps; see build-recipe remote limitation).

**Phase 1 RE — RESOLVED (2026-06-12, Ghidra; both prior open items closed):**
- **Takeover slot:** the active render camera-state is `unkCameraArray[DAT_0050c038]` (default index
  `0`; base ptr cached in `_DAT_0050c034`), 4x4 transform at `+0x14`. The CE mod's `0xDFB1DC` ==
  `unkCameraArray[0]+0x14`, confirming the approach. **Mechanism:** each frame, after
  `swrViewport_UpdateCameras` rebuilds the active state from the cMan, overwrite that state's `+0x14`
  with the freecam matrix (render-only; no opcode NOPs). `swrViewport_SetCameraIndex(idx, node)` is
  the clean index API; `FUN_00428830` initializes the 32-entry array (identity + zeroed sources).
- **Audio-follow is FREE:** `FUN_004292b0` derives the listener matrix by `rdMatrix_Copy44` from
  `unkCameraArray[active]+0x14`; `swrSound_Update` extracts pos `0x50c668` / orient `0x50c648`,`0x50c658`
  and pushes them via `swrSound_SetTransforms`. So the listener tracks the freecam automatically.
- **Optional naming follow-up PR:** `FUN_00428830` (camera-state array init), `FUN_004292b0`
  (listener-from-active-camera), listener globals `0x50c668/648/658` — verify canonical names first.

---

## 10. Toward the broadcast north star (sequencing)

Freecam (1) is the spine. Photo (3) lands next to it cheaply. Focus/spectator+orbit (2) is the
bridge to the director/broadcast suite (6) — so once freecam is solid, the spectator-cycle +
projection presets in (2) should be built with (6) in mind (they share the focus-pointer + the
`swrObjcMan_SpectatorCamModes` machinery). Replay-follow (5) slots in after replay Tier A and reuses
the focus camera. **Revised net path: 0 (player-cam panel — quick wins, ships first + builds the
panel/scaffold) → 1 freecam → 3 photo → 2 focus/spectator → (5 replay-follow) → 6 broadcast**, with
4 (dynamic FOV / shake) folded into the Phase-0 panel.

---

## 11. Player camera — feature breakdown (Phase 0 detail)

Overwhelmingly **quick wins**: the CE mod already found the addresses, and most are a one-byte patch,
one global write, or a named function call. This is why the player-camera settings panel is the
natural FIRST ship (faster + lower-risk than freecam; builds the `dinput_hook/camera/` scaffold +
ImGui panel that freecam plugs into). Addresses are from the CE mod — reconfirm in the live DB.

### Camera settings
| Setting | Mechanism (from the CE mod) | Effort |
|---|---|---|
| Disable camera roll | `PlayerCameraRollMod1`/`2` = 0 (defaults `0.00555556` / `1.625`) | quick |
| Camera height / trail (per-pod) | globals `0x4C7434` (trail) / `0x4C7438` (height); per-pod = config keyed by player racer index, written each frame | quick global / med per-pod |
| Dynamic FOV (speed/boost) | read speed `[pod]+0x1A0`; widen FOV past ~650 + forward push (`CForward`) | quick-med |
| FOV (base) | chase = matrix scale; first-person = `0x4528EF` (annodue 120->100); find the proper projection FOV | quick-med |
| Camera shake (collision/boost) | perlin -> yaw/pitch/roll + pos offsets `0xDFB20C/210/214`; magnitude from `[pod]+0x2B8` (rumble) / `[pod]+0x1F4` (side-hit) / boost | med |
| True Cockpit | first-person (mode 4): `0xDFB1B0=1`, non-player cam `0xE9AAC0=1` + cockpitXf node `[pod]+0x490`, per-pod `CockpitY/Z` offset tables, FOV 1.2 | med |

### UI / visibility toggles
| Toggle | Mechanism | Effort |
|---|---|---|
| Show/hide HUD | byte `0x4ACCA3` (clean: hook swrPlayerHUD render) | quick |
| Show/hide guide arrow | byte `0x453633` | quick |
| Show/hide opponent position numbers | byte `0x42C03F` (mod `displayPOS`; verify it's the opponent placement labels) | quick |
| Show/hide weather | **`swrWeather_Disable`/`Enable`** (`0x42d440`/`0x42d450`) — named fns, clean call | quick |
| Hide own pod (clean cockpit) | byte `0x46D3CF` + smoke/dust `0xEC86B4` + reflections `0xEC86A0` | quick |
| Show/hide suns + lens flare | gate `UpdateSunAndLensFlareSprites` (`0x42c1a0`) in the RenderViewport detour | quick |
| Show/hide lights (light streaks) | gate `UpdateLightStreakSprites` (`0x42c800`) + `swrPlayerHUD_RenderWorldSprites` (`0x42cb00`) | quick |
| Show/hide ALL in-race world sprites | `InRaceSpritesEnabled` master flag | quick |
| Show/hide planets (sky) | NOT a HUD sprite -> skybox/scene geometry; needs per-track scene-node ID. **Defer to v2.** | med / RE |

### Additional player-cam ideas (beyond the original list)
- **Granular HUD toggles** (lap timer / speedometer / boost+heat / minimap / position / lap counter)
  instead of all-or-nothing -- each is its own swrPlayerHUD draw.
- **Free-look / glance** while racing (right-stick look-around without leaving the pod cam).
- **Chase-cam distance + pitch/angle** tuning beyond height/trail (full chase rig).
- **Smoothing / lag / damping** (snappiness) + **steering lean** (subtle yaw into turns) as the
  inverse of disable-roll.
- **Hide opponents entirely** (clarity / ghost-race) and **hide damage red-flash / hit-shake**.
- **Per-pod reset-to-default** + quick hotkeys. Per-pod overrides key off the player racer index and
  could eventually live in the pod's mod manifest (ties `MODDING_ARCHITECTURE.md`).

### Build note
Most settings are "known address -> ImGui control." For the byte toggles (`0x4ACCA3`/`0x453633`/
`0x42C03F`/`0x46D3CF`) save the original bytes for clean restore (or find the gating flag / hook the
specific draw fn -- a refinement, not a blocker). Roll, height/trail, and weather are already the
clean path (global / named function), no patching. `swrWeather_Disable/Enable` confirmed named.

**Sky / world-sprite toggles — RE RESOLVED (2026-06-15).** `swrPlayerHUD_RenderViewport` (`0x42d490`)
is the single gate for all in-race world sprites — under `InRaceSpritesEnabled` it calls, in order:
`UpdateSunAndLensFlareSprites` (`0x42c1a0`, suns+lens flare), `UpdateLightStreakSprites` (`0x42c800`)
+ `swrPlayerHUD_RenderWorldSprites` (`0x42cb00`, light streaks), `swrWeather_RenderParticles`, then
`RenderDistanceText`. So **detour that one function and conditionally call each sub-renderer per the
user toggles** — no byte patches, no per-element RE. The sun family also has `SetSunSprite`
(`0x42c2e0`) / `SetLensFlareSprite` (`0x42c380`) if we later want to split sun-disc from flares.
Exception: **planets are skybox geometry, not sprites** — a separate per-track scene-node toggle (v2).

---

## 12. Settings surface (UX) — decided 2026-06-15

Three surfaces, **none blocked** (corrected after tracing the in-race menu — the earlier "swrUI
GUI-blocked" worry does NOT apply to the in-race menu):

- **Native in-race Camera menu (players, gamepad) — CHEAP, ~zero carving.** The in-race menu is a
  data-driven TEXT list, not swrUI widgets. `swrRace_UpdateInRaceMenu` (`0x42ae00`) loops
  `swrRace_GetInRaceMenuEntry(i, name[64], desc[256], &ival, &fval)` until it returns 0 and draws each
  via `swrText_CreateTextEntry1`/`CreateEntry2`; navigation = in-race input bitset; adjust =
  `swrRace_AdjustDebugValue`; activate = `swrRace_ActivateInRaceMenuEntry`. `GetInRaceMenuEntry`
  (`0x42ac70`) dispatches on `DebugMenuState` (0 debug-values / 1 vehicle-stats / 2 ESC pause).
  **Cost = ~3 delta detours (GetInRaceMenuEntry / AdjustDebugValue / ActivateInRaceMenuEntry) + a
  camera-entry provider + an ESC-menu "Camera Settings" item that pushes a new state.** Renderer +
  nav are free; all fns already defined (no carving). Text-list UX fits toggles + stepped values.
- **ImGui overlay (broadcast operator, dev, real-time)** — the dinput_hook overlay; richer controls
  (sliders/color), mouse. The right tool for the director/broadcast operator at a PC.
- **Native front-end options page (hangar)** — the EXPENSIVE one: uses swrUI widgets (the actually
  GUI-blocked path). Skipped.

**Decision:** player-facing persistent settings -> **native in-race Camera menu**; broadcast/dev +
fine control -> **ImGui**; instant in-race toggles (hide UI, freecam) -> **hotkeys**. All three read
one shared camera config (keyed by player racer index for per-pod overrides).

---

## 13. Phase 0 implementation spec — shared config + settings table

One config + one settings table feed all three surfaces (native menu, ImGui, hotkeys).

### Config (persisted: ini or project config)
- **Per-pod overrides** (keyed by player racer index 0..22 + customs, with a `default` fallback):
  `fov`, `height` (-> `0x4C7438`), `trail` (-> `0x4C7434`), `true_cockpit` + cockpit offset.
- **Global session prefs**: `disable_roll`, `dynamic_fov`, `shake` (+ intensity / speed), and all
  visibility toggles (`hud`, `guide_arrow`, `opponent_numbers`, `weather`, `suns_lensflare`,
  `lights`, `hide_own_pod`).

### Settings table (the shared spec)
A static array of descriptors:
`{ name, kind(TOGGLE|FLOAT), scope(PER_POD|GLOBAL), min/max/step, get(podId), set(podId,v), apply(podId) }`
- **Native menu**: `GetInRaceMenuEntry(i)` -> `table[i]` -> name + `get()`; `AdjustDebugValue(i, d)` ->
  `set(get +/- step)`; activate -> toggle. (3 delta detours, see §12.)
- **ImGui**: iterate the table -> checkbox / slider -> `set()`.
- **Hotkeys**: bound to specific entries (toggle HUD / freecam / etc.).

### Apply layer (engine writes)
- **Per-frame** (dinput_hook camera controller): read current player racer index, write that pod's
  per-pod globals (height/trail/FOV); evaluate dynamic-FOV + shake.
- **Visibility**: `swrPlayerHUD_RenderViewport` detour (suns/lens, lights, weather) + byte/flag
  toggles (HUD `0x4ACCA3`, arrow `0x453633`, opp-numbers `0x42C03F`, pod `0x46D3CF`, with
  original-byte save/restore) + roll via `PlayerCameraRollMod1/2`.

**Open:** confirm the per-pod vs global split above (proposed cut; e.g. `shake` could be per-pod).

---

## 14. Camera rig model (the two-anchor system)

Supersedes the loose "freecam + focus + orbit" framing. Every camera is a point in ONE rig:

- **Attach-to** (position anchor): `[free <-> locked]`. Locked -> bind position to the focus target,
  style **rigid offset** or **orbit** (distance/azimuth/elevation). Free -> 6DOF fly.
- **Look-at** (aim anchor): `[free <-> locked]`. Locked -> **look-at** the focus target, or **match**
  its orientation (cockpit-rigid). Free -> mouse/stick look.
- **Single focus target** shared by both anchors (cycle racers; "attach to me, look at the leader" is
  an advanced split). **Free is a per-anchor TOGGLE, not a 3rd menu entry** -> always one press, never
  the far end of a 12-racer cycle.
- **Movement frame** (free position only): view-relative vs planar (level). One toggle; the CE mod's
  per-axis RPY-influence matrix collapses to this (advanced/hidden if ever wanted).
- **FOV**, **smoothing/damping**, **offsets** round out the rig.
- **Presets = named saved rig snapshots** (Chase / Cockpit / Orbit / Free / TV / Heli / ...). "Save
  mode" captures the current rig. The classic free/focus/attach modes are just three presets.

---

## 15. Control scheme (gamepad + keyboard)

Camera mode captures input (pod parked/autopilot, mouse grabbed); the whole input space is the cam's.

| Group | Gamepad (Xbox) | Keyboard/Mouse |
|---|---|---|
| Toggle cam mode / save mode | Back / Start | `0` / `Ctrl+S` |
| Cycle preset | D-pad < > | `[` `]` |
| Cycle focus target ; radial | LB / RB ; hold LB | `,` `.` (1-0 direct) ; hold `` ` `` |
| Leader / my pod | dbl-tap RB / LB | `L` / `P` |
| Translate ; up/down | Left stick ; RT / LT | WASD ; Space / Ctrl |
| Look ; roll | Right stick ; (chord) | Mouse ; Q / E |
| Speed +/- ; zoom/FOV | D-pad ^ v ; (rig) | Shift, `-` `=` ; wheel |
| Lock both (follow) / free both | A / B | G / Tab |
| Free-fly (position) / free-look (aim) | Y / X | F / T |

Three behaviors: **auto-free on override** (push a stick/mouse -> that anchor goes free; rig
`auto-release` toggle for directors who want locks to stay put), **B / Tab = one-press escape to free
cam from any state**, **single focus target** (cycling never passes through None).

---

## 16. User personas -> operating layers

Same rig, three layers; each adds one thing on top. Surfaces: L1 -> hotkeys + native in-race menu;
L2 -> ImGui director panel + auto-toggle + hotkeys; L3 -> ImGui timeline/keyframe editor.

| Layer | Persona | Core loop | Adds on the rig |
|---|---|---|---|
| **L1 Free-fly utility** | Speedrunner | scout geometry, find lines | speed, movement-frame, **pod handoff** (move-to-cam / return-to-pod / noclip), **practice/run gate** |
| **L2 Live director** | Broadcaster | swap angles/players live, never break | orbit/tracking presets + focus cycle + **auto-director** + **clean player-feed** + **lifecycle robustness** |
| **L3 Cinematic studio** | Content creator | author shots over a recorded run | replay capture + scrub/slomo + **camera keyframe timeline + easing** (§17) |

New capabilities the personas add (mapped into §5): return-to-pod / noclip / practice-gate (L1);
auto-director + clean player-feed + lifecycle robustness (L2); the Replay Studio (L3, §17).

---

## 17. Replay Studio -- camera x replay convergence (shared milestone)

The L3 (content-creator) headline, and where this roadmap and `REPLAY_ROADMAP` meet. Cannot start
until **replay Tier A** exists (recorded pod motion to play back).

- **Replay playback + transport:** record the run (REPLAY Tier A/B), scrub the timeline, slomo via
  `swr_FastMode` / `swr_frameDeltaTime` (already RE'd), ghost the full field.
- **Camera timeline:** author **keyframes** of full rig state (position / aim / FOV) at replay-time T,
  with **easing / spline interpolation** between them and **FOV curves** -- synced to the pod replay
  ("ease A->B over 4s as the pod hits the chicane"). A second (camera) timeline laid over the (pod)
  replay.
- **Editor:** ImGui timeline + keyframe handles; export.

Build the camera rig (Phases 1-2) and replay Tier A independently; they converge here. Mirrored as the
same milestone in `REPLAY_ROADMAP`.
