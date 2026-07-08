# SW_RACER_RE — Modes + Modifiers Roadmap

**Status:** design (2026-07-04). Living document. Owner: lightningpirate.

Goal: a **Modes + Modifiers layer** that extends replayability and gives SWE1R fresh, modern
twists — icy tracks, floor-is-lava, elimination, survival, beat-the-clock, anti-grav — that
players can **mix, record, and race online.** Nothing like this exists for SWE1R today.

Like the modding, replay, and multiplayer work, this is a **delta on understood behavior**: it
lives in the `dinput_hook/` Microsoft Detours layer, **not** in the `src/` faithful reimpls.
`src/` stays a clean decomp; `dinput_hook/` owns the ruleset descriptor, the mode/modifier
registries, the surface-override wrapper, the sidecar record store, and the MP handshake. This
keeps the reimplemented sim byte-faithful, which also protects the run-verification story
(`VERIFICATION_ROADMAP.md`) and the vanilla `tgfd.dat` save.

> **Addresses/offsets below come from Ghidra-DB + source exploration (2026-07-04) and MUST be
> reconfirmed against the live DB at implementation time** (the Steam EXE `.text` is
> SteamStub-encrypted on disk — verify via Ghidra disasm or runtime, not file bytes). The record
> field offsets (§5) and the MP lobby message layout (§6) are the least-confirmed and should be
> re-verified first. Effort: **S** < ~half day, **M** ~1–2 sessions, **L** multi-session.

---

## 1. The core realization — two orthogonal, composable axes

Game modes are not a flat list; they fall on **two independent axes that compose.** Build a
small set of toggles on each axis and let players mix them, instead of hand-authoring N modes.

| Axis | Question it answers | Where it hooks | Example toggles |
|------|---------------------|----------------|-----------------|
| **1 — Rules** | who wins / when the race ends | the race manager `swrObjJdge` | Elimination, Survival, Beat-the-clock |
| **2 — World** | how the world physically behaves | the surface-behavior lookup `swrRace_UpdateSurfaceTag` | all-ice, floor-is-lava, anti-grav, turbo, molasses |

The payoff is combinatorial: ~6 rule modes × ~8 world modifiers ⇒ a large catalog from a small
build. "Elimination on an all-ice track," "Survival + rising lava," "Time-trial in zero-g."

**Terminology (kept distinct in the data model):** a **MODE** is the single win-condition for a
race (Axis 1, exactly one per race). A **MODIFIER** is a stackable world/physics tweak (Axis 2,
zero or more). MP authority and record-metric selection **branch on the mode**; modifiers merely
**accumulate**. This split is load-bearing — see §4 and §6.

---

## 2. The Ruleset Descriptor — the spine of the whole layer

The single realization that ties records and multiplayer together: **the thing records need as
a key is the same thing MP needs as a payload** — a canonical description of *what ruleset this
race was*. Design this object + its serialization first; everything else "consumes the
descriptor."

```
RulesetDescriptor {
  format_version : u16          // gates BOTH record interop and the MP handshake (see §3)
  mode           : ModeId       // exactly one win-condition (VANILLA, ELIMINATION, SURVIVAL, TIME_TRIAL, ...)
  mode_params    : ParamMap     // e.g. TIME_TRIAL.target_ms, ELIMINATION.interval
  modifiers      : ModifierEntry[]   // stackable; sorted by id, deduped
  base           : { lapcount:u8, mirror:bool }   // vanilla-native race axes
}
ModifierEntry { id: ModifierId, params: ParamMap }   // e.g. SPEED_MULT.factor=1.25
```

- **Track is deliberately NOT in the descriptor.** Records are physically per-track and track
  already has its own selection + wire path, so the record key is `(track_id, hash(descriptor))`.
- `lapcount` / `mirror` **are** inside the descriptor because they change the challenge.

### 2.1 Canonical serialization (must be exactly right)

The same bytes are hashed for records **and** sent on the wire, so encoding must be deterministic
across machines:

- Little-endian (matches x86), fixed field order.
- Modifiers **sorted by id, deduped** (each id appears ≤ once).
- Params emitted in a fixed per-modifier schema order, and **params equal to their schema default
  are omitted.** This is the key trick: adding a new optional param later does **not** change the
  hash of any ruleset that did not set it → old records stay valid across registry growth.

### 2.2 Two independent version concepts (this is what makes §3 policies coherent)

- **`format_version`** — the *encoding structure*. Bumped only on an incompatible layout change.
  This one field gates everything: sidecar records only compare within one `format_version`, and
  the MP handshake refuses across versions.
- **Registry version** — the *set of known mode/modifier ids*. Grows freely (add modifiers)
  **without** a format bump, because both consumers absorb it: canonical hashing stays stable
  (§2.1), and MP handles unknown ids via capability bitsets (§6).

So "a format bump invalidates old records AND fails the handshake" is literally one field driving
both, while everyday "we added a modifier" costs nothing.

### 2.3 Tangible example — "Ice Elimination, 5 laps"

```
{ format_version:1, mode:ELIMINATION, mode_params:{interval:PER_LAP},
  modifiers:[{id:ALL_ICE}], base:{lapcount:5, mirror:false} }
```
→ canonical bytes → `hash` = the record bucket for this track; ranked **only if** this exact combo
is a registered preset (§5); broadcast via the new subtype (§6); any client that doesn't know
`ELIMINATION` or `ALL_ICE` is refused at join.

---

## 3. Locked decisions (2026-07-04)

| # | Decision | Choice | Consequence for the descriptor |
|---|----------|--------|-------------------------------|
| 1 | **Composition** | À-la-carte free stacking **+ named presets** on top | modifiers are a free set validated by a compat table; presets are just pre-validated descriptors (no separate type) |
| 2 | **Parameterization** | **Parameterized from v1** (not a bitmask) | each mode/modifier carries a `ParamMap`; serialization is keyed, not a flag word |
| 3 | **MP compatibility** | **Require support / refuse** mismatched clients | capability handshake at join; host refuses clients missing a required id or on a different `format_version` |
| 4 | **Record eligibility** | **Curated modes/presets only** are ranked | free-stacked custom rulesets are *sandbox* (playable, unranked); bounds the key space |

---

## 4. Axis 1 — Rules (the `swrObjJdge` seams)

The race manager `swrObjJdge` is a state machine with three clean seams a mode hooks:

| Seam | Function | Addr / ref | Role for modes |
|------|----------|-----------|----------------|
| **Config-in** | `swrObjJdge_F4` `'Begn'` case | `src/Swr/swrObj.c:1451` | consume the descriptor at race start; add a `raceMode` field to the payload |
| **Win-condition gate** | `swrObjJdge_F0` state 2 (Racing→Finish) | `src/Swr/swrObj.c:661` | branch the "race over?" check per mode |
| **Ranking** | `swrObjJdge_UpdateStandings` / `GetRacerRankValue` | `src/Swr/swrObj.c:352` / `:324` | alternate scoring/placement per mode |

**F0 state map** (`src/Swr/swrObj.c:594-791`): 0 pre-race countdown · 1 "Go!" hold → racing ·
**2 racing → finish** · 3 finish hold (3 s) · 4 post-race camera sweep · 5 results · 6 teardown.

**`'Begn'` payload** (already carries most race config — extend it with `raceMode` + a descriptor
handle): `subEvents[2]`=num_players, `[3]`=planetId, `[8]`=countdownTimer_ms, `[9]`=num_laps,
`[0xc]`=aiSpeedSetting, `[0xd]`=mirror, `[0xe]`=localRacerId. Broadcast from the `'Join'` case via
`swrEvent_Broadcast('Jdge', ev)` (`src/Swr/swrObj.c:1549`).

**Per-racer state the modes read** (`swrScore`): `flag & 0x2` = finished; `results_P1_Lap` =
current lap; `results_P1_total_time`; `results_P1_Position` = placement (assigned by
`UpdateStandings`).

**Reference modes:**
- **Elimination** — each lap rollover, `GetRacerRankValue` ordering picks last place → trigger the
  existing explode/finish path ([[death_explosion_subsystem]]). **The recommended vertical slice**
  — it exercises all three seams and reuses everything.
- **Survival / last-pod-standing** — change state-2's end check from "player finishes" to "one pod
  left"; reuses the finish flag + explosion.
- **Beat-the-clock / target-time** — reuse `countdownTimer_ms` + `results_P1_total_time`; ends on
  timer expiry, win = laps completed.

> NOTE: SWE1R has **no points-standings system** (tournament is pass/fail per track — see
> [[galaxy_map_screen_subsystem]]). A Mario-Kart-style cup is therefore genuinely new; the modding
> content layer's circuit data (`MODDING_ARCHITECTURE.md`) is where a scoring table would bolt on.

---

## 5. Axis 2 — World (the surface-behavior seam)

The whole surface system is one reversed choke point. Each frame, per pod,
`swrRace_UpdateSurfaceTag` (`src/Swr/swrRace.c:1136`, `0x00476ea0`) fetches the terrain mesh's
`swrModel_Behavior` block via `swrModel_MeshGetBehavior` (`src/Swr/swrModel.h:84`, `0x004318b0`)
and translates its `vehicle_reaction` bitfield into pod `flags0`/`flags1` + three eased traction
targets.

**`swrVehicleReaction` vocabulary** (`src/types_enums.h:26`) → forcing a bit globally = an "all-X"
mode:

| Reaction bit | Effect in `UpdateSurfaceTag` | "All-X" modifier |
|---|---|---|
| `Slip` 0x20 | skid modifier → 0.2 | **all-ice** (skiddy everywhere) |
| `Lava` 0x2000 | sets `FLAG1_ON_LAVA` (damage/death floor) | **floor-is-lava** |
| `Fall` 0x4000 | sets `FLAG1_ON_FALL` → forces `RESPAWN` | **don't-touch-ground** (air-only) |
| `Fast` 0x4 | iceTarget → 200 (speed strip) | **turbo track** |
| `Slow`/`Swst` 0x8/0x10 | traction 0.75/0.1, kills boost | **molasses** |
| `ZOn` 0x1 | sets `FLAG0_ZON` (zero-g/orbit) | **anti-grav** |
| `unk1 & 0x20` | sets `FLAG1_MAGNET` (surface-relative gravity) | **corkscrew/wall-ride** (see [[magnet_surface_gravity]], ~80%) |
| `Swmp` 0x400 | sets `FLAG1_ON_SWAMP` | **swamp world** |
| `Dust`/`Snow`/`Wet`/`Mirr` | cosmetic (particles/reflection) | visual variants |

`DrawTerrainTypeDebugText` (`src/Swr/swrObj.h:441`, `0x00454060`) already renders live reaction
bits — a **free authoring/QA overlay** for building these.

### 5.1 The clean injection point

**Wrap `swrModel_MeshGetBehavior` in the delta layer** and return a **doctored copy** of the
behavior block with the modifier set's override mask OR'd into `vehicle_reaction` / `unk1`.
Everything downstream (`UpdateSurfaceTag`, lighting, fog, triggers) reads the returned pointer,
so one wrapper reskins the entire world's physics **without touching the reimplemented sim**.

> **CAVEAT — do not mutate the original.** `swrModel_Behavior` blocks are **shared, loaded model
> data** (`src/types.h:1544`). Mutating one in place corrupts it permanently and bleeds across
> meshes. The wrapper must return a **per-call scratch copy**.

**Vertical slice for this axis:** the `MeshGetBehavior` wrapper + a single force-on toggle
(all-ice is the most visually obvious for testing). Interacts with traction easing → note
[[fixed_timestep_feature]] / [[fps_dependent_physics]].

---

## 6. Records — mode-specific, in a sidecar (decision #4)

### 6.1 Why the vanilla store can't hold this

`tgfd.dat` = 4-byte CRC32 + **0xfd0-byte payload**, magic `0x10003`; the CRC covers the whole
payload, so naively adding fields **breaks every existing save.** Records are keyed **per-track
only**, with just **two** time slots: `best_lap_time_ms` (+0x28) and `recordLap3_ms` (+0x2c) in
`swrObjJdge` (`src/Swr/swrObj.c:1464`), passed into the race via `'Begn'` and compared at finish.
**There is no 5-lap slot and no mirror slot** — even lap-count is a dimension it can't represent.
(Load/save: `swrRace_InitGameData`@`0x00421810`, `LoadGameData`@`0x00421b90`,
`SaveGameData`@`0x00421c90`; write path `swrRace_SaveCurrentProfile`@`0x0044e560`, only partially
mapped; display `swrRace_DrawRecordText_Maybe`@`0x00439c70`.)

### 6.2 Design

- **Sidecar JSON store**, separate from `tgfd.dat`, which is **never touched** (preserves CRC +
  vanilla save + verification integrity). Same slug-keying discipline as
  [[modding_content_system]] / `MODDING_ARCHITECTURE.md` and `REPLAY_ROADMAP.md` §6.
- **Key = `(track_id, hash(descriptor))`.**
- **Ranked iff the descriptor exactly matches a registered "ranked ruleset"** — a `curated` mode
  with an empty modifier-set, or a registered named preset. Any other à-la-carte stack is
  **sandbox: fully playable, just unwritten.** Bounds the key space and keeps leaderboards
  meaningful.
- **Metric is per-mode**, declared by the mode registry: `{LAP_TIME | TOTAL_TIME | PLACEMENT |
  TIME_ALIVE | LAPS_DONE}` + sort direction. Store `(metric, value, holder, ...)` — metric is
  mode-derived, not stored per-run.
- **Captured at the finish seam in the delta layer** (read `swrScore` results → compute the mode's
  metric → write keyed by `(track, hash)`), so we **never reimplement** the partially-mapped
  vanilla record-write path.
- Vanilla best-lap/3-lap stay authoritative in `tgfd.dat`; the sidecar is **purely additive** for
  the non-vanilla curated rulesets. A unified records UI reads both.

### 6.3 The descriptor as a shareable token

The canonical descriptor **is** the shareable "here's my exact challenge" token — the seed of
ghost-sharing, daily-challenge, and community-leaderboard features. It composes directly with the
`.swrr` replay header (`REPLAY_ROADMAP.md` §6): a shared ghost already needs to name its ruleset.

---

## 7. Multiplayer — host-authoritative, refuse mismatches (decision #3)

### 7.1 The existing precedent (subtler than "sync it")

The two shipped MP modifiers — **pod upgrades** and **no-collision** — are **LOCAL-ONLY, never
synced** (`dinput_hook/game_deltas/swrMultiplayer_delta.cpp:179`;
`swrRace_delta.cpp:112`). They work in MP **only because remote pods are transform-replayed**
(position/velocity over the wire), not simulated from stats. Toggles:
`imgui_state.mp_allow_upgrades` + `mp_upgrade_levels[7]`, `imgui_state.mp_disable_collision`.

That reveals **two modifier classes** — the crux of MP design:

- **Local-authoritative** (works per-client via replay): pod upgrades, no-collision, cosmetic
  skins. Technically don't *need* syncing — but create **fairness asymmetry** (one racer skids,
  another grips). Fine casual, unacceptable competitive.
- **Global / rules-authoritative** (must be identical or the race is incoherent): all-ice,
  all-lava, elimination, lap count, target time. **Must be host-broadcast and applied
  identically.**

Each modifier's registry entry carries this `class`. (Even under "require support" everything is
host-uniform for now; `class` is retained for a future "allow local asymmetry" toggle and
fairness-tagging.)

### 7.2 Wire path (RE-grounded)

Race setup flows host→client as lobby message **`0x3a`** (`swrMultiplayer_BroadcastRaceSettings`
@`0x0041c2a0` → `swrMultiplayer_ApplyLobbyState`@`0x0041c330`), then each machine builds its own
local `'Begn'` from the synced globals. Handler table: `src/Swr/swrMultiplayer.h:205` (0x02 Chat,
0x17 Event, 0x38 RaceSettings, 0x3a LobbyState). Transport is `tSithMessage` (`src/types.h:2943`,
2592-byte payload) over DirectPlay. The lobby already rebroadcasts on setting change (the lap
stepper hook, `swrMultiplayer_delta.cpp:127`). **The format is fixed-size with no version /
handshake** → extending `0x3a` breaks vanilla clients.

### 7.3 Design

1. **Broadcast the serialized descriptor as a NEW message subtype** (e.g. `0x3e`). Vanilla clients
   ignore unknown subtypes, so nothing crashes on the wire.
2. **Capability handshake at join**: each side sends `{format_version, known_mode_ids,
   known_modifier_ids}` as bitsets; the host stores every client's caps.
3. When the host sets/changes the ruleset, validate against **all** joined clients. A client
   missing any required id, or on a different `format_version`, is **refused** (blocked from
   ready-up with a clear message) rather than silently racing the wrong rules. Adding a modifier
   later doesn't break the protocol — clients that don't know that id just can't join *lobbies that
   use it*, which is exactly correct.
4. Rebroadcast reuses the existing "setting-changed → broadcast" pattern. Each client deserializes,
   re-hashes to verify, applies at its local `'Begn'`.

**MP persistent records are out of scope** — network results are session-scoped; persistent MP
leaderboards are a server/out-of-band concern, not a `tgfd.dat` one. See [[multiplayer_subsystem]]
and `MULTIPLAYER_ROADMAP.md`.

---

## 8. Delivery surface (open)

Where players select mode + modifiers. Not a descriptor fork; can start dev-facing and grow.

| Option | Pro | Con |
|--------|-----|-----|
| **ImGui debug menu** | fast, proves mechanics, no menu-reversing | dev-facing, not shippable UX |
| **Native `swrObjHang` front-end** | real player UX in the race-setup screen | swrUI menu work, partly GUI-blocked ([[hangar_frontend_subsystem]], `UI_ROADMAP.md`) |

Recommended: prototype in ImGui, port winners to the native front-end once the descriptor +
registries are stable.

---

## 9. Anticipated roadblocks

- **RB1 — behavior-block aliasing (§5.1).** Shared loaded data; the `MeshGetBehavior` wrapper must
  return a per-call scratch copy or it corrupts the track. Watch for callers that cache the pointer
  across frames.
- **RB2 — modifier ↔ modifier incompatibility.** anti-grav vs magnet, all-fall vs all-lava, etc.
  The compat table (`excludes`/`requires`) must be authored and enforced at descriptor-build time,
  not at race start.
- **RB3 — mode ↔ metric ↔ scoring coupling.** Elimination has no "time"; survival's "time" is
  time-alive, not lap-time. `UpdateStandings` assumes lap/time ranking — each non-time mode needs
  its own ranking branch, and the HUD/results screens assume a time readout.
- **RB4 — record legitimacy vs the sim delta.** A modded record must never mix with vanilla
  `tgfd.dat` records; tag sidecar entries with `format_version` + a "modded-layer" flag. Ties to
  `VERIFICATION_ROADMAP.md` (reimpls dormant ⇒ sim = original `.text`).
- **RB5 — MP silent-mismatch is the real failure.** With no existing version check, a modded host +
  vanilla client would otherwise race different rules invisibly. The §7.3 handshake is mandatory,
  not optional.
- **RB6 — >5-lap / lap-count edges.** Modes that change lap counting cross the `swrScore` 5-slot
  split array / `swrObjJdge_F2` unbounded index (see [[hundred_lap_limit]]).
- **RB7 — descriptor offset drift.** The record field offsets (§6.1) and the `0x3a` layout (§7.2)
  are the least-confirmed items — reconfirm against the live DB before building on them.

---

## 10. Phasing

| Phase | Scope | Effort | Risk | Depends on |
|-------|-------|--------|------|------------|
| **0** | Ruleset Descriptor spec: fields, canonical serialization, mode/modifier registries + compat table, versioning | S–M | low | — |
| **1** | Axis-2 slice: `MeshGetBehavior` wrapper (scratch copy) + all-ice force-on, ImGui toggle | S | low (RB1) | 0 |
| **2** | Axis-1 slice: `raceMode` in `'Begn'` + Elimination (state-2 gate + `GetRacerRankValue` + explosion), ImGui | M | med (RB3) | 0 |
| **3** | Compose: full modifier set (lava/fall/turbo/anti-grav/…) + Survival + Beat-the-clock; compat table enforced | M | med | 1, 2 |
| **4** | Records: sidecar store, curated ranked-ruleset table, per-mode metric, capture-at-finish, records UI | M | med (RB4) | 0, 3 |
| **5** | Multiplayer: capability handshake + new descriptor subtype + refuse-mismatch + lobby UX | M–L | high (RB5) | 0, 3 |
| **6** | Native front-end: mode/modifier/preset selection in the race-setup screen | M | med (GUI) | 3 |

**Recommended first action:** Phase 0 (write the descriptor + registry spec) then the Phase 1
all-ice slice — it's the cheapest end-to-end proof (single reversed choke point, one wrapper, no
menu or judge surgery) and de-risks RB1.

---

## 11. RE-grounded reference map

### 11.1 Axis 1 — rules
| Symbol | Addr / ref | Role |
|--------|-----------|------|
| `swrObjJdge_F0` | `src/Swr/swrObj.c:594` | 7-state race dispatcher (state 2 = win-condition gate) |
| `swrObjJdge_F4` `'Begn'` | `src/Swr/swrObj.c:1451` | consumes race config; descriptor entry point |
| `swrObjJdge_UpdateStandings` | `src/Swr/swrObj.c:352` | assigns placements (ranking seam) |
| `swrObjJdge_GetRacerProgress` | `src/Swr/swrObj.c:324` | lap + fractional spline progress |
| `swrEvent_Broadcast('Jdge')` | `src/Swr/swrObj.c:1549` | posts `'Begn'` |

### 11.2 Axis 2 — world / surface
| Symbol | Addr / ref | Role |
|--------|-----------|------|
| `swrRace_UpdateSurfaceTag` | `src/Swr/swrRace.c:1136` / `0x00476ea0` | reads behavior → pod flags + traction |
| `swrModel_MeshGetBehavior` | `src/Swr/swrModel.h:84` / `0x004318b0` | **the wrap point** |
| `swrModel_Behavior` | `src/types.h:1544` | per-mesh behavior block (shared data) |
| `swrVehicleReaction` | `src/types_enums.h:26` | surface-behavior vocabulary |
| `swrObjTest_FLAG1_*` | `src/types_enums.h:76` | derived terrain flags (ON_LAVA/ON_FALL/MAGNET/…) |
| `DrawTerrainTypeDebugText` | `src/Swr/swrObj.h:441` / `0x00454060` | live reaction-bit overlay (QA) |

### 11.3 Records
| Symbol | Addr / ref | Role |
|--------|-----------|------|
| `best_lap_time_ms` / `recordLap3_ms` | `src/Swr/swrObj.c:1464` (+0x28 / +0x2c) | the only two vanilla record slots |
| `swrRace_SaveCurrentProfile` | `0x0044e560` | vanilla write path (partial RE — do NOT extend) |
| `swrRace_DrawRecordText_Maybe` | `0x00439c70` | vanilla records display |
| tgfd.dat | 0xfd4 = CRC32 + 0xfd0, magic `0x10003` | vanilla save (**never touch**) |

### 11.4 Multiplayer
| Symbol | Addr / ref | Role |
|--------|-----------|------|
| `swrMultiplayer_BroadcastRaceSettings` | `0x0041c2a0` | host lobby broadcast (msg 0x3a) |
| `swrMultiplayer_ApplyLobbyState` | `0x0041c330` | client applies lobby state |
| MP handler table | `src/Swr/swrMultiplayer.h:205` | subtype dispatch (add the new descriptor subtype here) |
| `tSithMessage` | `src/types.h:2943` | 2592-byte DirectPlay payload |
| pod-upgrades / no-collision deltas | `swrMultiplayer_delta.cpp:179` / `swrRace_delta.cpp:112` | the local-only modifier precedent |
| lobby rebroadcast hook | `swrMultiplayer_delta.cpp:127` | "setting changed → broadcast" pattern to reuse |

---

## 12. Cross-references

- Memory: `game_modes_roadmap.md` (this design, condensed).
- Subsystems: [[race_manager_subsystem]] · [[pod_flightmodel_subsystem]] ·
  [[magnet_surface_gravity]] · [[death_explosion_subsystem]] · [[save_profile_subsystem]] ·
  [[multiplayer_subsystem]] · [[mp_upgrades_feature]] · [[galaxy_map_screen_subsystem]] ·
  [[hangar_frontend_subsystem]] · [[hundred_lap_limit]] · [[modding_content_system]].
- Roadmaps: `REPLAY_ROADMAP.md` (shared descriptor/slug keying, ghost = a raced ruleset) ·
  `MULTIPLAYER_ROADMAP.md` (transport, lobby, `'Locl'`/`'REMO'` path) · `MODDING_ARCHITECTURE.md`
  (slug keying, content registry) · `VERIFICATION_ROADMAP.md` (sim faithfulness, record trust) ·
  `AI_ROADMAP.md` (difficulty knob composes as a modifier) · `UI_ROADMAP.md` /
  `DEBUG_UI_ROADMAP.md` (delivery surface) · `COMMUNITY_ISSUES_ROADMAP.md` ("game too easy / add
  challenge" demand).
