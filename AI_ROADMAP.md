# SW_RACER_RE — AI / Racer-Roster Roadmap

**Status:** design (2026-06-16). Living document. Owner: lightningpirate.

Goal: a grab-bag of **AI-racer and race-roster features** for SWE1R — higher-fidelity AI,
configurable grids (size, positions, who's in them), and player access to AI-only abilities
(Sebulba's flame). Most of this lives in the `dinput_hook/` Detours delta layer, built on the
already-mapped `swrObjJdge` (race manager) + `swrObjHang` (hangar/roster) + `swrRace`/`swrObjTest`
(pod) subsystems.

> Addresses are from the Ghidra DB and MUST be reconfirmed at implementation time (Steam `.text`
> is SteamStub-encrypted on disk; verify via Ghidra/CE, not file reads). Effort: **S** < ~half day,
> **M** ~1–2 sessions, **L** multi-session. See also memories [[ai_fidelity_lod_subsystem]],
> [[racer_count_limit]], [[pod_flightmodel_subsystem]], [[local_multiplayer_subsystem]].

---

## 0. Shipped / in-flight

- **AI full LOD (no model pop-in)** — DONE, PR #65 (`louriccia:ai-full-lod`). Toggle in dinput_hook;
  NOPs SpawnRacers gate-1 JNZ @0x46654d (+2 cable gates) so all racers render at player LOD. The
  OpenGL renderer also removed the vanilla >6-full-pod scene-flatten crash. See ai_fidelity memory.

---

## 1. Configurable grid size (racer count)

**Finding (verified).** Hard cap = **20 racers** — set by `swrScores` (`swrScore[20]` @ 0xe29bc0)
and the size-20 score-slot-indexed arrays (the `out` order array, the SpawnRacers scratch globals
`DAT_00e27840`/`DAT_00e28860`). Pod *entities* scale dynamically; the fixed score arrays are the wall.
Per-mode count set in `tracks_delta.c` ~L1190 (`hang->num_players`): freeplay=`nb_AI_racers`,
tournament=hardcoded **12**, time-attack=1, 2-player=`DAT_0050c55c`. `swrObjHang.num_players` is a
`char`.

- **(S) Allow odd counts / arbitrary 1–20.** The freeplay selector is even only because it steps by
  2 (`tracks_delta.c` ~L1257 `nb_AI_racers += 2`, clamp `< 20`). Change to `+= 1` → every value 1–20.
  Zero structural risk.
- **(L) Raise the cap above 20.** Must relocate/enlarge `swrScores` + audit every `base + i*0x88`
  accessor (many go via the pointer `swrScoresPtr` @0xe28960 = redirectable; some use the base
  0xe29bc0 directly). Also grow the SpawnRacers scratch + `vehicleOpponent[22]` if >22. Soft cap
  before any max = asset-buffer memory (`lowMemoryRacerCount`, "Low Memory! %d Racers"). Render
  crash ceiling already gone on the OpenGL build.

## 2. Custom starting-grid position (place the local player anywhere)

**Finding (verified).** `swrObjJdge_SpawnRacer` (0x465980) positions each pod via
`swrObjJdge_GetSpawnTransform(judge, &out, score->unk14)` (0x465840) — a pure `gridIndex -> transform`
that lays out a staggered **4/3/4/3** grid (index 0 = front). The index = the racer's roster slot
(`score->unk14`). Player = slot 0 = front. The random shuffle in SpawnRacers is spawn *order* only;
the `gridPos` arg only feeds `player->unk238` (lane/stagger), NOT the transform.

- **(S) Put the local player at any grid slot.** Before the spawn loop, set the player's
  `score->unk14` to the target slot (hook InitTrack/SpawnRacers), or hook SpawnRacer to swap the
  `gridIndex`. **Swap** with whoever's at the target slot to avoid a 1:1 overlap; an index
  `>= num_players` drops them into an empty back row (no collision). Isolated, low-risk.

## 3. Choose which pods are opponents

**Finding (verified).** Roster = `swrObjHang.vehiclePlayer` + `vehicleOpponent[22]` (chars = racer
IDs 0..22; **23 racers**: `swrRacer_PodData`=`swrRacerData[23]`@0x4c2700). Freeplay
`swrObjHang_BuildRosterSinglePlayer` (0x45b7d0) fills `vehicleOpponent[i]` **randomly**, re-rolled
until it passes: dedup, an allowed-racer bitmask (`uVar8` from `DAT_00e35a94`), and slot0 forced to
the track's favorite pilot. **AI all share one handling profile** (`swrRacer_AI_PodHandlingData`
@0x4c3114) — pod choice = model/appearance only for AI; the human gets per-racer
`swrRacer_PodHandlingData[vehicle]`.

- **(M) Pick the field.** Hook/replace `BuildRosterSinglePlayer`'s random-fill loop with chosen IDs
  (bypass dedup/mask/favorite for full control; can't exceed 23 distinct). To give AI distinct
  handling per pod, also change their stat copy to index `swrRacer_PodHandlingData[vehicleOpponent[i]]`.
  This is the natural "per-track custom field from JSON" seam → [[modding_content_system]].

## 4. Player Sebulba's flame should affect AI/REMO

**Finding (verified 2026-06-16; corrects an earlier wrong "activation" theory — the player CAN fire
the flame via taunt).** The flame *emitter* `swrRace_UpdateEngineDamageFX` (0x46f9a0, run per-pod from
`swrObjTest_F3`) burns whatever pod is nearest the flame tip (`FindNearestObjects('Test', tip, 64.0,
max=2)`), skipping only self + dying/finished (`flags0 & 0x7800`) + collision-disabled
(`flags1 & 0x2000000`). **NO `'Locl'`/identity filter** — the `target->engineStatus[engine] |= 8`
write lands on any pod incl. AI/REMO. So why does player-Sebulba's flame do nothing to AI? Two
independent reasons:
1. **No damage processing on AI.** The fire bit -> health drain is `swrRace_ApplyEngineDamage`
   (0x46aa30: for each `engineStatus[i] & 8`, `TakeDamage`). Its ONLY caller is
   `swrRace_UpdatePlayerControl`, which `swrRace_CalcTargetTurnRate` (0x46cf00) runs ONLY for the
   local human (`flags0 & 0x20`). AI -> `UpdateAutopilotControl`, REMO -> netcode; neither drains the
   fire bit. So the flame's `engineStatus |= 8` on an AI is a dead write.
2. **No visual on low-LOD AI** (the LOD point). Bot-model AI spawn with `unk344_nodeArray == NULL`
   (SpawnRacers passes `podModel = NULL` for class-3 bots; bot model -> `unk348_node`).
   `UpdateEngineDamageFX`'s fire-FX half is gated on `unk344_nodeArray != NULL` -> no engine nodes to
   render the flame on.

- **(M) Damage AI:** call the `engineStatus & 8` -> `ApplyEngineDamage` drain in the AI/autopilot
  branch of `CalcTargetTurnRate`, not just the player branch. Localized; no emitter/target changes.
- **(S, shipped) Visual:** the fire renders only with the full model's engine nodes = AI full LOD
  (PR #65). With `ai_full_lod` on, AI have `unk344_nodeArray` populated and the flame shows on them.
- **Caveat:** REMO (network) opponents must be damaged host-side only (authoritative) or it desyncs;
  single-player AI is clean.

### Flame variants (verified 2026-06-16)

Flame creator = `swrRace_SpawnExplosionEffect` (0x46ba30): `swrObjSmok_Spawn(8,...)` -> type-8 flame
object into `player->unk31c` (gated only on `flags0 & 0x6000` + `unk31c == 0`, NO racer check inside).
`UpdateEngineDamageFX` pins that flame to `engineXfR` each frame + runs the hit at the right-engine tip.

- **(S + S/M toggle) Everyone can flame, not just Sebulba.** The only Sebulba gate is ONE compare in
  `swrRace_UpdatePlayerControl` at the taunt trigger: `if (*(int*)score->unk18 == 2) SpawnExplosionEffect(...)`
  (racer ID 2 = Sebulba). Relax to `(*unk18 == 2 || g_flameForAll)`. Host toggle rides the existing
  race-settings sync (`swrMultiplayer_BroadcastRaceSettings` 0x3a -> `ApplyRaceSettings`) -- add a
  "flame mode" lobby field. Compose with the MP-networking item so it matters in MP.
- **(M) Double-sided flamejet.** Today: one flame object (`unk31c`) on `engineXfR`, single
  `FindNearestObjects` at the right tip. Mirror it: spawn a 2nd type-8 flame into a free smoke handle
  (e.g. `unk320`; `unk314/318` are engine smoke) on `engineXfL`, and duplicate the tip-compute +
  `FindNearestObjects` + `engineStatus |= 8` block for the left side in `UpdateEngineDamageFX`. No new
  mechanics -- "do the right-side block twice with engineXfL." `engineXfL` already exists in-struct.

## 5. Full-physics AI (stretch, from the AI-LOD work)

- **(M/L, riskier)** AI gate-2: clear `flags1 & 2` (spline-driven) on AI pods in `swrObjTest_F0` so
  distant AI keep full simulation instead of the on-rails spline "tiptoe" — gives real player↔AI
  collisions. Deferred in the AI-LOD effort as out of scope. See ai_fidelity memory gate 2.

## 6. AI behavior & difficulty (player complaints, verified 2026-06-16)

The AI brain is thin: `swrRace_UpdateAutopilotControl` (0x46bb70) does ONLY `AutopilotSteer` +
`Tilt`. `swrRace_AutopilotSteer` (0x46af20) is pure spline-following with an adaptive lookahead
(`unk104`) — no boost, no slide, no awareness of the player. AI speed = `swrRace_AILevel` (base,
int @ 0x4c707c, clamp 0.2-2.0) eased into `multiplayerStats` via `swrRace_AI` (0x46b670), shaped by
`ai_spread` (float @ 0x4c7080, clamp 0.5-200, named) + the AI's target pack-rank `unk23c` (random-
walks near its grid base) + distance terms `unk12c/unk130/unk134`.

- **C1 AI too easy / lappable (esp. upgraded player).** AI use the SHARED `swrRacer_AI_PodHandlingData`
  (0x4c3114), not per-racer stats, and get no upgrades, while a fully-upgraded player is much faster.
  **(S-M)** Lever: raise `AILevel`; point AI stats at per-racer `swrRacer_PodHandlingData[vehicle]`;
  grant AI upgrade tiers. A difficulty slider = "scale AILevel + choose stat source."
- **C2 AI kill you too easily on contact** + **C3 can't wreck AI (they stick to spline).** Two sides
  of the SAME thing: pod-pod collision `ResolvePodCollision` + `DeathSpeed` apply to both, but AI are
  spline-snapped (gate 2, `flags1 & 2` -> `swrObjTest_F0` teleports them back), so you eat the crash
  and they recover. **Fix = item 5 (clear `flags1 & 2` on AI):** collisions become mutual -> you can
  shove AI into walls (C3) AND they stop being immovable insta-kill walls (C2). Riskiest; gate behind
  a "physical AI" toggle + tuning.
- **C4 AI don't boost or slide.** Confirmed: not in the AI brain at all. **(M-L)** Net-new behavior:
  add heuristics to charge/release boost on straights (reuse `swrRace_ApplyBoost`/boost-charge) and
  airbrake-slide on tight turns, in `UpdateAutopilotControl`/`AutopilotSteer`.
- **C5 "leader speeds up the further you're behind."** FULLY TRACED 2026-06-16 -> NOT a deficit-
  triggered mechanic; it's a real STATIC asymmetry that feels like a rubber-band. Verified: (a) the
  `_DAT_0050cae0` rubber-band is DEAD CODE (BSS=0, no writers, gate `0.0 < cae0` always false);
  (b) `AutopilotSteer` has ZERO player awareness (pure spline-follow + adaptive lookahead `unk104`);
  (c) the AI target rank `unk23c` random-walks near its grid-derived base, NOT recomputed from live
  standings / not player-keyed; (d) the player gets NO catch-up assist in single-player
  (`UpdateCatchup` sets player `multiplayerStats = 1.0`; the assist path is 2-player splitscreen only,
  driven by `unk130` "distance behind"); (e) AI speed = `AILevel` x rank-spread (`ai_spread`, makes
  front-runners constitutively faster) x pace terms. NO code reads the player's deficit to speed the
  leader. THREE real contributors that combine into the rubber-band FEELING (in order):
  (1) **on-rails far-ahead AI** -- gate-2 spline-snap (`swrObjTest_F0`) teleports distance-far AI onto
  the spline, so a leader well ahead of you is NOT driving: it follows the ideal line at its speed,
  never braking for corners / risking walls / paying the navigation tax YOU pay (the dominant cause;
  ties to item 5);
  (2) rank-spread (`ai_spread`) makes front-runners constitutively faster;
  (3) the human gets NO catch-up in single-player.
  RESIDUAL CLOSED: `unk130` writer = `swrObjJdge_UpdateStandings` (0x45d4a0), `unk130 = leaderRank -
  thisPodRank` -> the LEADER's `unk130 == 0` (no gap boost); the AI rubber-band in `swrRace_AI` only
  speeds *trailing* AI toward the leader. (`cae0` still dead; `AutopilotSteer` still has zero player
  awareness.) **FIX for the feeling:** **(M, deepest)** gate-2 full-physics AI (item 5) so far AI
  actually drive -> slow for corners, make mistakes, become catchable; **(S)** lower `ai_spread`;
  **(M) catch-up assist** -- the multiplier infra already exists (`UpdateCatchup`: `1.0 + gap*L/5000`,
  cap 1.25x, `gap = unk130`), just gated to `NumLocalPlayers() > 1` (splitscreen). Extend it to the
  single-player human, and to local co-op / online as a host toggle -- see LOCAL_MULTIPLAYER_ROADMAP
  ("Catch-up") and MULTIPLAYER_ROADMAP Q-row.
