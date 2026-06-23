# SW_RACER_RE — Community / Forum Issues Triage

**Status:** triage (2026-06-19). Living document. Owner: lightningpirate.

Goal: a single index of **player-reported problems** (Steam/GOG forums, Discord) mapped to their
root cause, current status, and the roadmap/memory that owns the fix. This is the demand-side
companion to the topic roadmaps — it answers "what are players actually complaining about, and
where does each one already live?" Most fixes land in the `dinput_hook/` Detours delta layer.

> Frequency is the relative volume of forum posts (player-reported, approximate). Effort: **S**
> < ~half day, **M** ~1–2 sessions, **L** multi-session. Addresses live in the topic roadmaps /
> memories linked per row and must be reconfirmed at implementation time.

---

## 1. Triage table

| Issue (player words) | Freq | Root cause (our RE) | Status | Owner | Effort |
|---|---|---|---|---|---|
| Controller not recognized / modern pad unusable | ★★★ | game's DirectInput enum + no XInput | **partial** — nav + rumble shipped (PR #115/#114); in-race rebinding of arbitrary pads still on the DInput path | [[input_subsystem]], [[gamepad_nav_bridge]], [[rumble_xinput_bridge]] | M |
| **Alt-tab → stuck inputs** (locked reverse view, confirm stuck) | ★★★ | the GLFW window has no focus callback, so the game's `Window_SetActivated`→`stdControl_SetActivation` (which already zeroes held-key state + unacquires/reacquires DInput) never fires on alt-tab | **FIX BUILT** — `fix/alt-tab-stuck-input`, pending playtest | [[input_subsystem]], [[window_glfw_shutdown]] | S (done) |
| Can't save / change bindings, binding UX unclear | ★★ | binding persistence + menu clarity | open | [[input_subsystem]], [[save_profile_subsystem]], UI_ROADMAP.md | M |
| Audio silent / **needs admin** to work | ★★ | A3D/Aureal COM + elevation/registry | open — investigate | [[swrsound_subsystem]] | M |
| Audio in some scenes but not others | ★ | sound resource cache evict/budget | open | [[swrsound_subsystem]] | M |
| Audio way too loud initially | ★ | default volume setting | **quick win** | [[swrsound_subsystem]] | S |
| CTD on launch | ★★★ | classic SWE1R HID/DirectInput-enum crash (community-patched; our input layer sits on top) | **likely already fixed** — needs confirm | [[input_subsystem]] | S (verify) |
| Cyan sky on some levels (Steam, not GOG) | ★ | dgVoodoo back-buffer read-back path the GL replacement bypasses | **likely fixed w/ RENDERER_REPLACEMENT** — needs visual confirm | [[weather_hud_investigation]] | S (verify) |
| Resolution doesn't match on install | ★ | hardcoded 640×480 + initial display mode | roadmapped | UI_ROADMAP.md, [[ui_resolution_independent_roadmap]] | M |
| Save/profile can't delete/save/restore | ★ | elfSaveLoad/tgfd.dat (CRC32, live vs saved tables) | open — needs repro | [[save_profile_subsystem]] | M |
| **Oovo tubes / sliding at high FPS** | ★ | framerate-dependent physics (see §3) | **spike built** (`proto/fixed-timestep`) — pending playtest | [[fps_dependent_physics]] | spike done; Phase B M–L |
| Pit-droid repair / hidden stats confusing | ★ | poorly communicated mechanics (not a bug) | open — UX | [[pod_handling_stats_subsystem]], DEBUG_UI_ROADMAP.md | S–M |
| Game too easy / no way to add challenge | ★ | no exposed difficulty knob (AI stats already reimplemented) | roadmapped | AI_ROADMAP.md, [[ai_opponent_difficulty_subsystem]] | S–M |
| No more money after all races done | ★ | economy design, not a bug | won't-fix / design | — | — |
| Wants achievements | ★ | no system exists | large feature | (no roadmap yet) | L |
| No multi-language support | ★ | strings hardcoded in EXE | roadmapped (string externalization) | MODDING_ARCHITECTURE.md, [[modding_content_system]] | L |

---

## 2. Quick wins (tractable, near-term)

1. **Audio default volume** — clamp/lower the initial master volume (forum: "way too loud"). **S**.
2. **CTD-on-launch confirm** — verify the HID/DInput crash can't occur on our build; document as fixed if so. **S**.
3. **Cyan-sky confirm** — visually verify the GL renderer fixes the Steam cyan sky on a known-bad level; document. **S**.
4. **Alt-tab stuck inputs** — **DONE (built, pending playtest, branch `fix/alt-tab-stuck-input`)**: wire `glfwSetWindowFocusCallback` → `Window_SetActivated_delta` in `Window_delta.c`. The engine's own handler already clears held-key state + unacquires/reacquires DInput; it just wasn't being called on the GLFW window. Hits the #1 reported category. **S**.
5. **Difficulty knob** — expose the already-reimplemented AI stats (AILevel/spread/AISpeed) as a slider. **S–M**.

---

## 3. Framerate-dependent physics (active workstream)

Forum complaint: handling/traction changes with FPS (Oovo tubes slide). **Confirmed root cause**
(verified against the original binary): the flight model is mostly framerate-safe *by design*
(linear `+= dt*rate` integration; exponential decay via `stdMath_Decelerator` ≈ `e^(-k*dt)`), with
**one gross outlier** — `swrRace_ApplyTraction`'s velocity-direction blend (orig @0x478a70), written
`(1/dt)*(desired*dt*keep + dt*old*traction)` so the **dt cancels exactly**, leaving a fixed per-frame
lerp whose effective grip is `traction^FPS`. The long tail (Euler + Decelerator) is only first-order
fps-independent. Full detail + the Discord debate (tim/Galeforce favor fixed timestep over per-lerp
fixes) in [[fps_dependent_physics]].

**Chosen direction: fixed timestep, decouple sim from render** (not per-lerp retuning). The engine
already ships the scaffolding — `swrMain_RunFrame` is phase-split (phase 1 = sim, phase 2 = render),
`swrMain_GuiAdvance` calls the phases separately, and `swrRace_IncrementFrameTimer` already emits a
fixed dt under `swr_FastMode`.

- **Phase A — band-aid (optional):** dt-aware `ApplyTraction` (`powf(traction, dt/DT0)`) behind a
  toggle. Fastest Oovo relief; superseded by B. Matches Tim's PR #110 comment (done correctly — the
  literal "1/30" doesn't work since dt cancels). **S**.
- **Phase B — SPIKE BUILT (`proto/fixed-timestep`, commit 6738545):** accumulator over RunFrame's
  phase-1 calls at a fixed Hz, reusing the FastMode dt path; render runs free. ImGui toggle + sim-rate
  slider + readout. **Not yet built/playtested on the isolated branch** — see §5 queue. **M.**
- **Phase B+ — finish it:** sub-step only the world sim (not sound/input/camera), add render
  interpolation of the pod transform in the renderer hook (kills judder), expose "physics rate" as a
  user slider. Unlocks deterministic **replay** + **multiplayer** (see those roadmaps). **M–L.**

---

## 4. Investigation candidates (look before committing)

- **Oovo "increase-slide" path** — `ApplyTraction` math says higher FPS = *more* grip, which is
  *inverted* from the forum's guess. If players genuinely slide *more* at high FPS, the cause is
  likely in the still-original (un-reimplemented) collision/wall code: `swrRace_CollideTrack` /
  `swrRace_ApplyWallCollision` / `swrRace_DetectWallScrape` (all `HANG("TODO")`), or `velocitySlope`.
  Read these before declaring the fixed-timestep fix complete.
- **Audio admin requirement** — trace why audio needs elevation (A3D COM registration vs. registry
  vs. device init order) and whether swrSound's reimpl can sidestep it.
- **Save/profile repro** — reproduce the delete/restore failures before touching elfSaveLoad.

---

## 5. Queue — when home (remote session 2026-06-19)

Two isolated branches off HEAD (001c067), both built+linked but **not yet playtested**:

1. **`fix/alt-tab-stuck-input`** (1-file, `Window_delta.c`): build + playtest — start a race, alt-tab
   out while holding/just after a control, alt-tab back, confirm inputs are no longer stuck (no locked
   reverse view / latched confirm). Also sanity-check the display still re-inits cleanly on refocus.
   Lowest-risk, highest-frequency win — good first PR candidate.
2. **`proto/fixed-timestep` spike** (commit 6738545): `cmake -S . -B build` (GLOB) + build, then
   playtest — Oovo IV, toggle on, vary render FPS, confirm handling is now FPS-consistent; watch the
   sub-steps readout. Then decide Phase A (traction band-aid) vs straight to Phase B+. See
   [[fps_dependent_physics]].
3. Pick another quick win from §2 for a fast player-facing W.

> NOTE: the dll currently in the Steam game dir is a transient **corkscrew + alt-tab-fix** build
> (the timestep spike was backed out of the corkscrew tree). Rebuild whichever branch you want to test.

---

## Cross-references

Topic roadmaps: AI_ROADMAP.md · CAMERA_ROADMAP.md · DEBUG_UI_ROADMAP.md · LOCAL_MULTIPLAYER_ROADMAP.md ·
MULTIPLAYER_ROADMAP.md · REPLAY_ROADMAP.md · UI_ROADMAP.md · MODDING_ARCHITECTURE.md
