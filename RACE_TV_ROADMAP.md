# SW_RACER_RE — Race TV + the broadcast/automation capabilities under it

**Status:** design (2026-09-06). Living document. Owner: lightningpirate.
**Trigger:** Katalysor (Discord, 2026-09-04) wants a TV running unattended AI-only pod races all
day at a Halloween murder-mystery party so guests can bet fake currency. Deadline 2026-10-31.
**Framing rule:** Race TV is a thin *assembly* of capabilities that are each useful on their own
(multiplayer streams, attract/b-roll mode, spectating, testing, modding). Nothing Race-TV-specific
may live in a shared module. Each capability below names its other consumers; if a design choice
only serves the party, it goes in `race_tv.cpp`.

Drops to Katalysor: ~10-04 (playable loop), ~10-18 (tuned), ~10-25 (final).

---

## 0. What Katalysor asked for

| Need | Their words | Reading |
| --- | --- | --- |
| Zero interaction | "automate races without someone needing to interact between races" | boot -> loop forever, self-recovering |
| Betting window | "3 presets, 2 / 5 / 15 minute pause" | fixed pause between races, NEXT roster visible, hot-switchable |
| Verify results | "see/verify the results" | results stay up through the pause |
| Field | "all 25 racers" | 23 pilots exist, **hard cap 20 per race** (§2.4). 20 + 3 rotating, or fixed 20. Ask. |
| Race length | "adjust with the number of laps" | laps per preset (1..125 already supported) |
| Camera | "birdseye, sometimes shifts to a racer" | director cam: overhead + follow segments |
| Overlays | nameplates, left leaderboard with positions + time diff | broadcast overlay + name labels |
| Upsets | "bots slightly dumber", marble-race upsets | per-race AI form variance, no processions |

Non-goals: real money, MP, touching human-race sim fidelity. (Score tallying across the day was a
non-goal until 2026-09-08, when Lou asked for a championship: it is C9 below.)

---

## 1. Capability map (the timeless part)

Each row is its own module + panel + INI section, works with a human racing, and ships as its
own upstream PR. Race TV consumes them; so do the other consumers listed.

| # | Capability | Module | Other consumers beyond Race TV | Existing roadmap slot |
| --- | --- | --- | --- | --- |
| C1 | **Race telemetry snapshot** — one read-only, per-frame normalized view of the race: racers, positions, gaps (laps + seconds), lap, finished, names (SP character / MP player), source (SP, MP, all-AI, later replay) | `dinput_hook/broadcast/race_telemetry.{cpp,h}` | every overlay; auto-director; stream/JSON export; replay HUD; run-verification logs; the AI inspector already on `feature/ai-tuning-panel` | new (data layer under CAMERA §6) |
| C2 | **Broadcast overlay** — leaderboard, nameplates (SP names), timers/banners, HUD-trim profiles, clean-feed, TV-scale theme | `dinput_hook/broadcast/overlay.{cpp,h}` + `swrPlayerHUD_delta` extension | MP streams/casting (the #143 ask, generalized), replay viewing, speedrun overlays, tournament casting | CAMERA_ROADMAP §6 (phase 6) |
| C3 | **Camera director** — focus target + lifecycle robustness, auto-director policies (leader / battle / random / overtake / finish), overhead shot | `dinput_hook/camera/director.{cpp,h}` on the freecam takeover | MP spectating (server feed, MULTIPLAYER_ROADMAP), b-roll/trailer capture, replay-follow, casting | CAMERA_ROADMAP §2 + §5 (phases 2, 5) |
| C4 | **Race orchestrator** — start a race programmatically from a *race descriptor* {track, roster, laps, AI settings, human slots incl. zero}, emit lifecycle events (loaded / green / lap / finish / results / teardown), auto-continue policy, no-human race support | `dinput_hook/orchestrator.{cpp,h}` | attract mode (C6); soak / regression runs for the reimpl verification harness (N deterministic AI races, diff vs vanilla); renderer perf benchmark scenes (fixed seed, fixed roster); "viewer-picked race" for streams; game-modes descriptor (GAME_MODES: same struct grows into the ruleset descriptor); modding API (issue #153 hook registry gets its first real event source) | new; feeds game_modes_roadmap + MODDING_API_ROADMAP |
| C5 | **AI variance ("form")** — per-race seeded per-racer multiplier, optional blunders, optional real pod stats for AI, rubberband off | extend `dinput_hook/ai_tuning.cpp` | SP "unpredictable AI" option, difficulty presets, MP-vs-AI fields, AI_ROADMAP C1-C3 | AI_ROADMAP |
| C6 | **Attract mode** — idle timeout in the front-end -> AI race with director cam + overlay, any input returns to the menu | small consumer of C3/C4 in `orchestrator.cpp` | exhibition PCs, streamer BRB screens, b-roll, "screen saver" for the repo demos | revives the retail dead demo path |
| C7 | **Results log / export** — every finished race appended to a readable table + a JSON-lines feed; later per-event stream / named pipe | `dinput_hook/broadcast/race_result.{cpp,h}` + `results_log.{cpp,h}` | OBS text sources, Discord webhook (the `BottoAPP` bot in the server could announce winners), Twitch bets, verification logs (VERIFICATION_ROADMAP) | new; trivial once C1/C4 exist |
| C9 | **Championship standings** — points for the top finishers of every race, carried across a series, summary card at the end of each race, table persisted | `dinput_hook/broadcast/standings.{cpp,h}` | tournament casting, the GAME_MODES grand-prix/circuit ruleset, community leagues | new; feeds game_modes_roadmap |
| C8 | **Kiosk robustness** — autostart from boot, unfocused run, watchdog relaunch, event log | `race_tv_watchdog.bat` + toggles in orchestrator | soak tests, future dedicated-server dream in MULTIPLAYER_ROADMAP, stream PCs | new |
| RT | **Race TV** — betting pauses, presets, bench rotation, betting board | `dinput_hook/race_tv.{cpp,h}` (thin) | none by design | this doc |

Design constraints that keep the capabilities general:
- **C1 has no notion of "who the viewer is".** Local player is just a flag on a row. Name
  resolution reuses PR #154's rule (MP player name, else character name).
- **C2 windows are independent toggles with an INI theme (scale, anchor, opacity).** No
  "Race TV layout"; Race TV just picks a preset. Clean-feed = hide everything but the world.
- **C3 policies are data, not code paths**: `{target_rule, dwell_s, shot_mix}`; Race TV, a
  caster, and attract mode pass different tables into the same engine.
- **C4 speaks in a descriptor + events.** Every entry into a race goes through the same
  `swrObjHang_LoadScreen(hang,1,0)` seam the retail demo path uses; the descriptor is the seed
  of the game-modes ruleset descriptor (same fields: track is outside it there — keep the two
  aligned when C4 lands).
- **C5 only ever post-processes the AI multiplier seam** (`swrRace_UpdateCatchup`,
  speedMultiplier not paceMultiplier), so the sim stays byte-faithful for humans.

---

## 2. RE foundation (mapped 2026-09-06, Ghidra)

SWE1R ships a hidden **attract/demo mode**. Retail reaches it only via a dev egg (profile name
"C" + held input on the main-menu accept, or clearing circuit 2), but every piece is live code:

- `swrObjHang_SelectDemoTracks_Maybe` @0x00440c10 — `swrRace_demoMode`@0x0050ca3c = 1,
  `hang->flag |= 8`, 5 random unlocked tracks into `char[5]` @0x00e29890. Callers
  `swrRace_MainMenu` @0x436f34, `swrRace_ResultsMenu` @0x43a497.
- `swrObjHang_BuildRosterSinglePlayer` @0x0045b7d0 — **`hang->demo_mode != 0` -> every slot
  `'AAII'`** (all-AI). `demo_mode == 2` also randomises the "player" vehicle. Field =
  `swrObjHang+0x64` (already in types.h). Other readers: `swrObjHang_LoadScreen` (skips the
  planet cinematic), `swrObjHang_StartRace` (`==2` branches), `swrObjHang_F4 'Abrt'` (`==2`),
  the race-settings menu row 5 in tracks_delta.c (hidden "Demo Mode" toggle).
- `swrObjHang_StartRace` @0x0045b290 — with `swrRace_demoMode`: track = demo list
  [`DAT_0050c960`], `demo_mode = 1`, num_players forced **2**, countdown payload
  (`subEvents[8]`) = **1000000** = infinite hold. `demo_mode == 2`: 3 racers, 30 s.
- `swrObjHang_F4` @0x0045a040 `'Abrt'`/`'Fini'` — with `swrRace_demoMode`: `DAT_0050c960++`,
  while `< 5` **`swrObjHang_LoadScreen(hang, 1, 0)`** directly = next race, no menu. After 5:
  clears demo state, splash. **This is the C4 entry seam.**
- `swrObjHang_AssignRacerCameras` @0x0045b210 — `'NAsn'` (payload: index+1, `score+0x7c`) to
  camera-man `id` for every `'Locl'` score **or `score->flag & 0x20`** (FavoritePilot slot).
  All-AI race => camera follows the favourite. Re-dispatching `'NAsn'` = engine-native
  retarget (**C3 v1 seam**).
- `swrObjcMan_UpdateCamera` @0x00453e00 — when `swrRace_demoMode == 0` and the followed pod is
  not local (`flags0 & 0x20 == 0`): auto-cycles `mode_type` through
  `swrObjcMan_SpectatorCamModes[7]` @0x004c3ef0 on a random timer (`_DAT_0050c8a0`, index
  `DAT_0050c8a4`; index 6 sets pod `flags0 | 0x100000`). With `swrRace_demoMode != 0` forces
  mode 4. The stock spectator cycle is what we want and only runs with the global flag **off**.
- `swrObjJdge_F4 'Begn'` @0x00463a50 — `flag |= 0x20` iff countdown payload > 0 (demo hold),
  `flag |= 0x40` iff `firstLocalPlayer == NULL`. `swrObjJdge_F0` @0x0045e200: state 1 + 0x20 =
  hold then `'Abrt'` (attract never finishes a race); states 3/4 auto-advance on
  `flag & 0x60`; state 5 (on-track results) 9.1 s. A no-human race with countdown 0 runs the full
  finish -> sweep -> results flow unattended.
- `swrObjJdge_F2` @0x0045ea30 — in state 1 sets nibble 2 each frame, knocked back to 1 while any
  *relevant* racer is unfinished; **relevant = every racer when `firstLocalPlayer == NULL`**.
  Stragglers get extrapolated times (`total / rank * num_laps`) once state sticks at 2.
- `swrObjJdge_F0` state 2 needs an accept edge (`KeyDownForPlayer1Or2(0x201)` or
  `swrControl_acceptReleasedEdge`) to `swrObjJdge_Clear(jdge,'Fini')`. **The one stall**; C4
  fires it (or sets the edge, the PR #222 trick).
- `swrRace_demoMode != 0` side effects we do NOT want: `swrObjJdge_F3` scrolls credits instead
  of the HUD, `swrRace_UpdateAutopilotControl` spams `'Snap'`, `swrRace_PoddAnimateEngines`
  reads it. **Never set `swrRace_demoMode`; borrow only `hang->demo_mode = 1`.**

### 2.4 Racer count
`swrScores` is `swrScore[20]` @0x00e29bc0; 23 pilots (`swrRacer_PodData[23]`). Past 20 =
relocate `swrScores` + audit every `base + i*0x88` site (racer_count_limit memory). Not for this
deadline. C4's descriptor carries a roster of up to 20; a "bench rotation" policy is Race TV's.

### 2.5 Seams to reuse
| Piece | Where | State |
| --- | --- | --- |
| count / laps / AI speed | `hang->num_players`, `hang->numLaps` (PR #97), `hang->AISpeed`, set before LoadScreen as tracks_delta.c:1217 does | shipped |
| roster contents | hook `BuildRosterSinglePlayer` to write chosen ids into `vehicleOpponent[]` | mapped |
| results hold | pin `jdge->raceTimer_ms` while nibble == 5 | mapped |
| racer table / AI knobs | `ai_tuning.cpp` on `feature/ai-tuning-panel` | built, unplaytested, not PR'd |
| name labels | `swrPlayerHUD_delta.cpp` (PR #154), SP path passes through | shipped |
| HUD hide groups | player-camera PR #303 | shipped |
| camera takeover | freecam PR #257 `rdCamera_Update` delta + `drive_viewport_cameras` | shipped |
| panel + INI pattern | `debug_ui` registry, per-module load/save ([camera], [ai]) | shipped |
| cutscene skips | PR #222 | shipped |
| pre-race fly-by CTD | PR #296 — a kiosk running 100+ races/day must carry it | MERGED |
| crash logger, release zip | `crash_logger.cpp`, `createReleaseArchive.py` | shipped |

---

## 3. Capability designs

### C4 Race orchestrator (new, the keystone)
```
struct RaceDescriptor { track_index; laps; racer_count; pilot_ids[20]; human_slots (0..2);
                        ai_settings (level/spread/speed/form seed); mirror; countdown_s; }
events: RACE_LOADED, GREEN, LAP(racer), FINISH(racer), ALL_FINISHED, RESULTS_SHOWN, TEARDOWN
policy: on ALL_FINISHED -> {wait_for_input | auto_fini_after_s | end_after_winner_s}
        on TEARDOWN     -> {return_to_menu | next(descriptor_provider)}
```
- Entry: fill `hang->*` from the descriptor (`demo_mode = 1` when `human_slots == 0`), hook
  the roster builder for `pilot_ids`, `swrObjHang_LoadScreen(hang,1,0)`.
- Events come from small detours already in `swrObjJdge_delta.cpp` (state nibble transitions,
  `results_P1_Lap` deltas) — no new sim code.
- `end_after_winner_s`: force state 2 early so vanilla extrapolation fills straggler times.
- Guard: never when `multiplayer_enabled`; all detours early-out when no descriptor is armed.
- Testing consumer: a `--soak N` style INI flag that runs N random all-AI races and exits,
  logging crashes — this is the missing runtime loop for the reimpl verification harness.

### C1 Race telemetry snapshot
`race_telemetry_Get()` -> `{n, rows[20]{slot, pilot_id, name, is_local, is_ai, position, lap,
lap_frac, gap_leader_laps, gap_leader_s, gap_ahead_s, finished, speed}}` built once per frame
from `swrScoresPtr` + `swrRace` (`gapToLeader` in laps -> seconds via leader speed and
`invTrackLen`). Source tag SP/MP/AI. Nothing else reads engine structs for display.

### C2 Broadcast overlay
- Leaderboard window (positions, name, gap, lap; finished ticks; winner banner).
- Nameplates: "character names in single-player" toggle on the PR #154 redirect (with no
  local player every pod is labelled; own-pod suppression is a `swrObjJdge_UpdateMinimap`
  local-player special case).
- HUD-trim profiles (player / caster / clean) via the PR #303 keep-set + hide groups.
- Timer / banner / roster-card widgets fed by C1 + C4 events (Race TV supplies the text).
- Theme: scale, anchor, opacity in `[broadcast]`; SDF text (PR #182) for 1080p+ TVs.

### C3 Camera director
- v1: retarget via `'NAsn'` on a dwell timer; rules leader / random / battle (smallest
  neighbour gap from C1) / finish (winner on last lap); stock spectator-mode cycling kept.
  Lifecycle: target finished/exploded -> fallback rule (CAMERA_ROADMAP phase 2 requirement).
- v2: overhead shot through the freecam seam — camera above the pack centroid looking
  down-track, eased cuts, `shot_mix` weights per policy. Known risks: culling/LOD/fog outside
  the track envelope (CAMERA_ROADMAP §roadblocks); audio is 2D on modern Windows so no desync.
- Attract/caster/Race TV differ only by policy table.

### C5 AI variance
- `form`: per-race seeded per-racer multiplier (e.g. 0.92-1.08) at the `UpdateCatchup` seam.
- `blunders`: rare 1 s throttle cuts.
- `real_pod_stats_for_ai` (experimental): roster hook copies
  `swrRacer_PodHandlingData[vehicle]` instead of the shared AI profile; interacts with the AI
  full-physics gate (ai_fidelity_lod) — separate toggle, heavy playtest.
- Rubberband 0 for no-human races (tethers point at nothing; the favourite still paces the
  field via `flag & 0x20`, so form is what breaks processions).

### C6 Attract mode
Idle timer in the front-end (swrObjHang F0 delta) -> C4 descriptor {random track, 12-20 AI,
1 lap, `end_after_winner`} with C3 policy "cinematic" + C2 profile "clean + leaderboard";
any input -> `'Abrt'` -> menu. Racer TV = attract mode with betting pauses.

### C7 Results log / export  (v1 SHIPPED 2026-09-08)
`dinput_hook/broadcast/race_result.{cpp,h}` freezes the classification out of the C1 snapshot the
moment it stops changing; `results_log.{cpp,h}` appends it to `race_results
aces.txt` (readable
table per race) and `race_results
aces.jsonl` (one JSON object per race). `[results_log]` in the
INI, "Race > Results log" panel. Answers Katalysor's "a race nobody watched can still be looked up".
Still to do: per-event stream (lap / overtake) and the named pipe.

### C9 Championship standings  (v1 SHIPPED 2026-09-08)
Points per finishing position (`[standings] points`, default `10,8,6,4,2` = top 5), scored from the
same `RaceResult`. Table keyed by pilot id, sorted points > wins > podiums > best finish, persisted
to `race_results\standings.csv` (survives a restart mid-party) and mirrored to `standings.txt` for
an OBS text source. The summary card comes up with the results (the orchestrator holds the window
open for `summary_s`, default 20 s) and stays through the load and the next grid until the race goes
green. The championship also feeds the roster: `swrObjHang_BuildRosterSinglePlayer` is post-hooked so
the standings' top N (`[orchestrator] seed_standings_top`, default 10) are always in the field --
the stock builder picks the AI at random bar the track's `FavoritePilot`, so a leader could otherwise
sit out several races running. The grid follows the table too: `swrScore.gridIndex` (0x14, named
from `swrObjJdge_GetSpawnTransform`'s own parameter; 0 = pole) is handed out in championship order
with a jitter over it (`[orchestrator] grid_order` / `grid_jitter`), replacing the old random
shuffle; reverse-championship and random are the other modes. `series_races > 0` closes a championship after N races: the card names the champion, then the
next race starts a fresh table. "Race > Standings" panel, with a reset button.

### C8 Kiosk
Autostart (skip startup movies via PR #222, auto-profile, arm C4 on the main menu), run
unfocused, `race_tv_watchdog.bat` relaunching `steam://rungameid/808910`, crash files kept.

### RT Race TV (thin)
Presets `{pause_s, laps, results_hold_s}` x3 (2/5/15 min), hotkeys (switch preset, next race
now, pause loop), bench rotation over 23 pilots, betting board text ("next race in mm:ss",
roster, last results). Everything else is C1-C8 configuration.

---

## 4. Sequencing (effort S < half day, M 1-2 sessions, L multi-session)

| Step | Delivers | Capabilities | Effort |
| --- | --- | --- | --- |
| 0 | **DONE 2026-09-06** (commit on `feature/race-tv`, `dinput_hook/orchestrator.{cpp,h}`): 5+ races unattended, 0 DNF, ~1 snap/race. Findings folded in: the no-local bot-model gate @0x466576; the tail of the field sat past the 20000u lodDistance clamp and ran on rails (stalls) -> clamp for all AI at the CalcTargetTurnRate seam; the judge's state-2 accept-edge stall; the vanilla HUD returns with numLocalPlayers == 0 (hence an ImGui leaderboard stand-in); AI pods have a light bank (entity id) but only local roots are tagged to use it; AI never run the fire-damage tick / Repair (added, repair off by default for drama). Lou's decisions: AI may not repair; Skip/Restart buttons. Spike also proved: results on the hangar screen work with no human and the 'Fini' handler re-entry is clean. |
| 0' | **Spike (original plan)**: hotkey -> `demo_mode=1`, 20 racers, 1 lap, `LoadScreen(hang,1,0)`; auto-`'Fini'` in state 2; on hang `'Fini'` wait 10 s and repeat. Verify all-AI roster, camera on favourite + mode cycling, HUD/results with no human, clean teardown, **10+ races with no asset-buffer growth**. Check what POST_RACE_INFO does with no local player and that `SelectDemoTracks_Maybe` never trips. | C4 proof | S/M |
| 1 | **PARTLY DONE 2026-09-06** (`feature/race-tv`, worktree SWR_wt_racetv): RaceDescriptor + Subscribe/events (RACE_STARTED / WINNER_IN / RACE_ENDED), `[orchestrator]` INI, on-track results window (no holotable: F4 delta chains LoadScreen like the demo). TODO: roster hook for chosen pilot_ids, end policies as data, soak flag. | C4 | M |
| 2 | **PARTLY DONE 2026-09-06**: `broadcast/race_telemetry` (snapshot, MP/SP names, pace-derived gaps) + `broadcast/overlay` leaderboard (Race > Broadcast panel, `[broadcast]` INI: toggle/scale/opacity/anchor; works in any race; consumers force/title/footer). TODO: SP nameplates toggle, HUD-trim profiles, pilot flag icons (needs sprite->GL texture), SDF text. | C1, C2 | M |
| 3 | Race TV scheduler, presets, betting board, bench rotation | RT | S/M |
| 4 | **DONE 2026-09-06** `camera/director.{h,cpp}`: 'NAsn' retarget of cMan 0 ({'NAsn', cman->metaCamIndex_count, pod} = full camera re-init), auto policy leader/battle/random weighted + dwell, cut-away from finished/crashed, leaderboard click-to-follow (overlay row-click handler + highlight), manual hold, Camera > Director panel, [director] INI. Only acts when telemetry source == ALL_AI. v2 drone shot DONE same day via `playercam_SetCameraOverride` seam (eased high camera above/behind the followed pod). | C3 | M |
| 5 | **DONE 2026-09-06 (v1)** `dinput_hook/ai_variance.{h,cpp}`: raw hook on swrRace_UpdateCatchup (post): replaces stock pacing for AI in all-AI races with form (per race) x OU swing x pack compression (target spread, gain, clamp) x blunders (Poisson, throttle cut); speed only; Race > AI Variance panel + live table; [ai_variance] INI. Not yet tuned by playtest. Independent of feature/ai-tuning-panel (that branch hooks the same seam -- merge them later, ours must run after). | C5 | S/M |
| 6 | Kiosk: autostart, watchdog, 8 h soak | C8 | S/M |
| 7 | Director v2 overhead shots | C3 | L |
| 8 | Attract mode + JSON export (cheap once 1-4 exist) | C6, C7 | S each |

Drop 1 (~10-04) = steps 0-4. Drop 2 (~10-18) = 5-7. Final (~10-25) = 8 + soak fixes.
Upstream PRs, in this order and each standalone: ai-tuning panel; C4 orchestrator (+ soak
flag); C1+C2 broadcast overlay; C3 director v1; C5; C6/C7. `feature/race-tv` bundles them plus
PR #296 for the party build so review timing never gates Katalysor.

---

## 5. Open questions for Katalysor
1. 20 racers per race (engine cap) with 3 rotating out, or a fixed 20?
2. Laps per preset? (3 laps ~3-6 min; Baroonda / Abyss are long)
3. All 25 tracks or a curated list?
4. Results on-track (pods parading, sweep camera) or on the hangar results screen?
5. Sound on the TV?
6. PC specs, resolution, does that machine own the game?
7. Bets close at the green light or during the grid sweep?

## 6. Risks
- No-human paths we have not run yet (state-2 stall is known; others surface in the spike).
- Slowest AI decides race length -> `end_after_winner_s`.
- Long-run stability (asset buffer, texture cache, sound voices) -> soak + watchdog.
- Director v2 is the only piece with real unknowns; v1 is enough for Drop 1.
- Generality tax: keep C1-C4 interfaces small; if a capability starts growing Race-TV
  fields, move them to `race_tv.cpp`.

Related: AI_ROADMAP.md, CAMERA_ROADMAP.md (§2/§5/§6, §16 L2), MULTIPLAYER_ROADMAP.md
(spectator feed), REPLAY_ROADMAP.md, MODDING_API_ROADMAP.md, VERIFICATION_ROADMAP.md; memories
[[race_tv_feature]], [[race_manager_subsystem]], [[ai_tuning_panel]], [[racer_count_limit]],
[[mp_player_names_above_pods]], [[camera_cman_subsystem]], [[player_camera_phase0]],
[[skip_cutscenes_feature]], [[game_modes_roadmap]], [[reimpl_verification_harness]].
