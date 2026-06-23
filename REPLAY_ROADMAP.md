# SW_RACER_RE — Replay / Ghost Roadmap

**Status:** design (2026-06-12). Living document. Owner: lightningpirate.

Goal: a well-functioning, **shareable** replay/ghost feature — record a run, share the file,
and have it play back in any other install (decomp build, and ideally annodue too). The
playerbase wants to watch, compare, and race against their own and others' runs; nothing
like this exists for SWE1R yet.

Like the modding work, this is a **delta on understood behavior**: it lives in the
`dinput_hook/` Microsoft Detours layer, **not** in the `src/` faithful reimpls. `src/` stays
a clean decomp; `dinput_hook/` owns the recorder, the playback feed, and the file format.

> **Addresses below are from current Ghidra-DB exploration and MUST be reconfirmed against
> the live DB at implementation time** (the Steam EXE `.text` is SteamStub-encrypted on disk —
> verify via Ghidra disasm or runtime, not file bytes). Effort: **S** < ~half day,
> **M** ~1–2 sessions, **L** multi-session.

---

## 1. The core realization

A replay system is **two halves: a recorder and a playback renderer.** The multiplayer
layer is a *half-built replay engine* — we mostly need to redirect it from the network
socket to a file.

| Half | What MP already does | What replay needs |
|------|----------------------|-------------------|
| **Record** | `swrMultiplayer_PublishPodState` @ `0x41d930` snapshots a pod each frame (transform + speed + a few fields) | Same read, but hooked so it runs in single-player and writes to a file |
| **Transport** | DirectPlay `Send` (per-tick, lossy, fire-and-forget) | A local file (lossless, full rate) |
| **Playback** | `'REMO'` pods render every frame from the per-player state arrays | Feed those same arrays from the file |

**Replay = swap the source of the REMO arrays from the socket to a file.** We are not
building a renderer; we are building a file reader that writes into the arrays the network
handlers already write into.

---

## 2. Prior art: annodue (the community's leading platform)

[louriccia/annodue](https://github.com/louriccia/annodue) (Zig, hooks via `dinput.dll`) has a
mature **Savestate & Rewind** feature (since v0.1.0, 2024) — but it is **in-memory only,
never written to disk, and not shareable.** No ghost or replay-file feature exists anywhere
in the community. So this is greenfield, and annodue's savestate is the reference for *what a
run's state actually is*.

annodue's savestate (`src/patch/dll_savestate.zig`) snapshots **10 regions** each frame with a
`TemporalCompressor(4,4,4)` delta scheme:

> RaceData (player) · Test entity · Hang entity · cMan (camera) · Smok particles · Toss
> particles · combined inputs · global input state · TIMING data · **4-byte RNG seed**

Two takeaways that shape this whole design:
- It captures **full state + inputs + RNG together** — it deliberately does **not** trust pure
  input re-simulation. That is the strongest possible signal that the sim is **not** cheaply
  deterministic (see Tier C).
- It stays **in-memory** precisely because the snapshot is full of live pointers (section 6).

**Interop note:** `src/types.h` already carries the comment *"Shifted by 4 bytes from
annodue?"* on `swrRace` — the two projects already cross-reference struct maps. The shared
`.swrr` format (section 7) should be **co-designed with the annodue author** so runs are
playable across both ecosystems. That is the literal meaning of "playback in other games."

---

## 3. Three fidelity tiers — the central architecture decision

| Tier | What's recorded | File size | Fidelity | Race-able? | Risk |
|------|-----------------|-----------|----------|-----------|------|
| **A — Ghost** | per-frame transform + speed + lap (the ~112-byte MP record) | <50 KB/run | position-accurate "video"; no physics interaction, camera free to orbit | **Yes** (ghost pod) | **Low** — reuses REMO machinery |
| **B — Full snapshot** | annodue's 10 regions, delta-compressed | hundreds KB – few MB | pixel-perfect: effects, camera, HUD | **No** (must freeze sim) | **Low–Med** + the pointer wall |
| **C — Deterministic input** | inputs + RNG seed + track/pod/upgrades | <5 KB/run | "true" re-sim; tiny; verifiable | n/a | **High** — needs a frame-independent physics reimpl |

**Recommended path:** ship **A** first (it is what players actually want and is mostly
wiring), keep **B** as a "perfect capture" mode for content creators, treat **C as research.**

> **Tier C is gated on determinism we don't have.** The sim advances on a wall-clock-derived
> frame timer (`swrRace_IncrementFrameTimer` @ `0x480540`, variable `dt`; `-nut` tick). Input-only
> replay diverges across framerates/hardware unless the physics is reimplemented on a fixed
> clock. The reimplemented flight model (`reimpl/pod-flightmodel`, 8 fns reverse-hooked) is the
> seed for that, but making it fixed-timestep + bit-reproducible is its own epic.
> **Update (RE 2026-06-12):** the engine **already has a fixed-timestep code path** — `swr_useFixedDeltaTime`
> (`0x50cb68`) makes `swrRace_IncrementFrameTimer` (`0x480540`) emit a fixed `swr_frameDeltaTime`
> (`0xE22A40`) instead of the wall-clock delta. That's a real starting point for Tier C (force fixed
> dt + drive the timestep, as the CE camera mod does for slow-mo), though cross-hardware
> bit-reproducibility of the float physics still needs care.
> **Update (2026-06-19):** a **fixed-timestep SPIKE now exists** on branch `proto/fixed-timestep`
> (`dinput_hook/game_deltas/swrMain_delta.cpp`) — it drives `swrMain_RunFrame`'s phase-1 (sim)
> calls on a wall-clock accumulator at a fixed dt, reusing the `swr_FastMode` @0x50cb68 path above,
> while render runs free. It was built to fix framerate-dependent traction (Oovo sliding), but it is
> the **same machinery Tier C needs**: once it sub-steps only the world sim and float
> reproducibility is settled, input-only replay/validation becomes viable. See
> COMMUNITY_ISSUES_ROADMAP.md §3 and [[fps_dependent_physics]].

---

## 4. RE-grounded address map

### 4.1 Recorder (read side)
| Symbol | Addr | Role |
|--------|------|------|
| `swrMultiplayer_PublishPodState` | `0x41d930` | snapshots a pod (transform via `rdMatrix_Copy44`, `speedValue`, ~3 ancillary). **Sole caller:** `swrObjTest_F3` @ call site `0x470654`, MP-gated. |
| `swrObjTest_F3` | `0x470610` | per-frame pod update (still a `HANG("TODO")` stub in `src/` → detour the original). The SP recorder hook point. |
| `swrEvent_CallAllF3` | `0x450a30` | per-frame dispatcher that calls every entity's F3. Alternative recorder hook (catch the local player here). |
| `swrRace_IncrementFrameTimer` | `0x480540` | the frame clock (`dt`). Governs capture cadence (R2). |

### 4.2 Playback (write side)
| Symbol / array | Addr | Role |
|----------------|------|------|
| `swrMultiplayer_ApplyPlayerStates` | `0x41d6f0` | writes a received per-pod record into the REMO arrays. **The pattern the file reader copies.** |
| `swrRace_ExtrapolateTransform` | `0x4705d0` | team comment: *"used for multiplayer/replay extrapolation"* — advances a pod forward by `speed*dt`. REMO **extrapolates**, see P1. |
| REMO transform array | `DAT_00ec76a0` | per-pod 4×4 transform (stride `0x10` ints). |
| REMO speed array | `DAT_00ec7640` | per-pod speed. |
| REMO orientation quad | `DAT_00ea05c0` | per-pod orientation/anim quad (stride 4). |
| REMO state block | `DAT_00ea0720` | per-pod state, stride `0x1f28` (= `swrRace` size). |
| REMO timestamp | `DAT_00ea0200` | `stdlib_timeGetTime()` per pod — MP's wall-clock interp basis. |

### 4.3 The per-pod wire record (from `ApplyPlayerStates`)
~**28 ints ≈ 112 bytes** per pod per tick:
- `[2..0x11]` — 16 ints: full 4×4 transform matrix
- `[0x12]` — speed
- `[0x13..0x15]` — 3 ancillary (the `unk4_0100+4/+0xc`, `unk1e70+0x78` from `PublishPodState`)
- `[0x16..0x1a]` — 5 ints: orientation quad (4) + **pod tag** (`'AAII'` = `0x41414949`)
- `[0x1b]`, `[0x1c]` — lap/progress fields

This is the Tier A frame payload, and the engine already knows how to consume it.

### 4.4 Key struct
`swrRace` @ `0x00e29c44`, **sizeof `0x1f28`** (≈8 KB). `transform` (rdMatrix44) @ `0x20`,
`speedValue` @ `0x1a0`, `lapComp*` @ `0xb0`-ish, `score_ptr` @ `0x1e70`. See `src/types.h:434`.

---

## 5. Level-2 roadblocks (anticipated)

### 5.1 Recorder
- **R1 — capture hook is MP-only as wired.** `PublishPodState` only runs inside the MP branch
  of `swrObjTest_F3` (sole call site `0x470654`). SP recording needs a **new** detour on
  `swrObjTest_F3` (`0x470610`) or `swrEvent_CallAllF3` (`0x450a30`), and must identify the
  **local** player entity (not AI, not the whole roster). Reuse the *logic*, not the call.
- **R2 — capture cadence ≠ playback cadence.** F3 runs once per render frame at variable `dt`.
  A run recorded at 144 fps and one at 30 fps differ in sample density. **Normalize to a fixed
  capture rate + per-sample timestamps**, or the same file plays back at different
  smoothness/speed on different machines (kills "playback in other games").
- **R3 — start/stop/restart boundaries.** annodue's changelog is a list of exactly these bugs
  ("loading state overriding settings on new scene", "rewind not restoring UI"). Define clean
  semantics for countdown start, mid-race Esc-restart, pause, and DNF. A >5-lap run crosses the
  `swrScore` 5-slot split array / `swrObjJdge_F2` unbounded index (see the 100-lap memo).

### 5.2 Playback (reusing REMO)
- **P1 — extrapolation, not interpolation.** REMO *predicts* pods forward
  (`swrRace_ExtrapolateTransform`) between sparse samples → overshoot + snap on the next sample.
  Fine for live MP, ugly for a replay where the entire future is already recorded. A good replay
  **interpolates** (Catmull-Rom on position, slerp on orientation). Reusing the REMO consume path
  verbatim inherits MP's snap artifacts — add a replay-specific smoothing path or dense samples.
- **P2 — the phantom-entity problem.** A ghost needs a spawned ~8 KB `swrRace` that the engine
  **renders** but **excludes from everything else**: collision (the Q3 toggle), standings/placement,
  finish (`'fini'`) events, lap triggers, minimap/HUD nametags, catch-up AI, and Sebulba flamejet.
  Inclusion is driven by the `'Locl'`/`'REMO'`/`'AAII'` tag dispatch — a ghost needs a **4th
  "render-only" class** honored at *every* racer-iteration site. Enumerating those sites is the
  bulk of Phase 2.
- **P3 — fixed-size array ceiling (the Q5 wall).** A ghost is "+1 racer." Per-player arrays are
  fixed (`0x1f28` stride; `0x14`=20-slot loops). Racing your PB ghost inside a full 8-opponent
  field may overrun a bound. Audit every per-racer array (overflow risk) or steal an AI slot.
- **P4 — camera for ghost-only playback.** "Watch my replay" with no live pod needs `cMan` to
  target the ghost entity; the camera modes assume a player pod. A non-player target path is new.

### 5.3 Tier B — the actual wall
- **B1 — POINTERS.** This is *why annodue's savestate never went to disk.* `swrRace` carries
  ≥8 pointer fields — `swrModel_Node*` at `unkec_node`, `model_unk` (0x13c), `terrainModel`,
  `unk344_nodeArray`, `unk348_node`, `unk34c_node`, `unk1994_node`; `swrScore* score_ptr` (0x1e70) —
  plus the `swrObj` base (vtable + entity links). A byte-copy to a file restores **dangling heap
  addresses** on reload; they only mean something in the writing process. annodue's in-memory
  rewind is safe because those pointers stay valid in-process. Cross-machine Tier B requires a
  **pointer census + a rebind-on-load pass** per entity type (null them, re-resolve model nodes
  from the loaded track/pod, re-link score). Multi-session effort.
- **B2 — variable-length pointer-linked pools.** "All Smok/Toss entities" is a dynamic list:
  snapshot = count + re-spawn + relink. Same pointer problem, worse.
- **B3 — struct-version skew.** Byte-snapshots break on any recompile that shifts an offset, and
  `swrRace` is **not** fully mapped (dozens of `unkNN` fields + opaque blobs `unk4d0[3584]`,
  `unk1610[900]`). Named-field serialization fixes it but needs more field mapping first.
- **B4 — Tier B can't be raced against.** Full-state playback must **freeze the sim** and blit
  recorded state each frame (like annodue rewind), or the live sim fights the restore. So
  **Tier A = interactive ghost; Tier B = non-interactive full replay.** Decide before building —
  it changes the entity model.

### 5.4 Cross-cutting
- **X1 — decouple capture from transport.** "Fatten the record → MP fidelity improves for free"
  collides with MP **Epic 0**: the host broadcast is a *synchronous per-peer `Send`*, so a bigger
  per-tick record makes lag worse. Resolution: **replay capture is local and free** (full rate to
  a file); the **wire** record is the bandwidth-bound, compat-gated thing. Share the *format*, not
  necessarily the wire payload; expand the wire only with version negotiation.
- **X2 — content dependency.** A replay references track/pod/upgrades/weather; playback needs that
  content present. Custom content ⇒ **slug-keyed resolution** (reuse the modding registry). You
  cannot watch a ghost on a track you do not own.
- **X3 — shareable ≠ verifiable.** Tier A/B are trivially forgeable (just positions/state). For
  leaderboard-grade trust you need Tier C (deterministic re-sim) or server-side replay validation.
  Set expectations: "share your runs" ≠ "verified WRs."

---

## 6. The `.swrr` file format (sketch)

```
Header:
  magic "SWRR", format_version
  game/engine build id            (for Tier B struct-skew gating)
  tier                            (A | B)
  track  (slug, not numeric id)   resolved via the modding registry
  pod / upgrades / weather / laps
  player name, total time, run checksum
  capture_rate_hz, frame_count
Frames (Tier A):  delta-compressed [transform(64B) + speed + quad + lap], timestamped
Frames (Tier B):  delta-compressed named-field snapshots of the 10 regions
```

- **Key on stable string slugs, not numeric ids** (same rule as the modding architecture) so a
  replay survives a changed installed mod set.
- Version + build id in the header so a player can be told "this replay needs build X / mod Y".
- Tier A frames are format-stable because they are essentially the MP wire record.

---

## 7. Phasing

| Phase | Scope | Effort | Risk | Depends on |
|-------|-------|--------|------|------------|
| **0** | `.swrr` format spec + annodue interop alignment (slug keying, versioning) | S | low | — |
| **1** | Tier A recorder: detour the F3 hook, capture local pod each SP frame, delta-compress, flush to file on finish | S–M | low | 0 |
| **2** | Tier A playback: spawn one phantom `swrRace`, feed REMO arrays from file via interpolation, tag out of collision/standings; seek/scrub | M | med (P2/P3) | 1 |
| **3** | UX: race-against-ghost (PB or downloaded), ghost selector, in-race delta; "watch" camera | M | med | 2 |
| **4** | Tier B full snapshot: pointer census + rebind-on-load, named-field serialization of the 10 regions | M–L | **high** (B1) | 2 |
| **5** | Distribution: file sharing now; in-game ImGui browser later (reuse modding online layer) | M | med | 0, 4 |
| **C** | (research) fixed-timestep deterministic physics reimpl → input-only replay + validation | L | high | pod-flightmodel reimpl |

---

## 8. Cheapest de-risk first — the Phase-1/2 spike

Before committing to the roadmap, run a **one-session spike** that exercises R1 + P1 + P2 + P3
at once while avoiding the pointer wall entirely:

1. Detour `swrObjTest_F3`; dump the **local** player's transform + speed to a file each SP frame.
2. On a second run, spawn **one** phantom `swrRace`, feed it the file via interpolation, and tag
   it out of collision + standings.

If the phantom renders and follows the recorded path cleanly, **Tier A is a week, not a month.**
This is the recommended next action.

---

## 9. Replay Studio — camera x replay convergence (shared milestone)

Where this roadmap meets `CAMERA_ROADMAP` (its §17). The content-creator headline: a Trackmania-style
editor = replay playback + an authored camera timeline laid over it. Gated on **Tier A** (recorded
pod motion); richer with Tier B (full-scene replay).

- **From replay:** Tier A/B playback (ghost the field), timeline scrub, slomo (`swr_FastMode` /
  `swr_frameDeltaTime`, already RE'd).
- **From camera:** keyframes of full rig state (position / aim / FOV) at replay-time T, with easing /
  spline interpolation + FOV curves, synced to the pod replay.
- **Editor:** ImGui timeline + keyframe handles; export.

Plan replay Tier A and the camera rig (`CAMERA_ROADMAP` Phases 1-2) independently; they converge here.
Distinct from the simpler **replay-follow camera** (a live camera that just tracks a ghost,
`CAMERA_ROADMAP` Phase 7) — the Studio is the authored, offline, keyframed editor (`CAMERA_ROADMAP`
Phase 8).

---

## 10. References
- `src/Swr/swrMultiplayer.h` — `PublishPodState` / `ApplyPlayerStates` / REMO state handlers.
- `src/Swr/swrObj.c:428` — `swrObjTest_F3` (stub); `src/Swr/swrRace.h:98` — `ExtrapolateTransform`
  (annotated "multiplayer/replay extrapolation"), F3 per-frame pipeline.
- `src/types.h:434` — `swrRace` (sizeof `0x1f28`, pointer-field census for Tier B).
- `MULTIPLAYER_ROADMAP.md` — Epic 0 (lag), Q3 collision toggle, Q5 array ceiling, the
  `'Locl'`/`'REMO'`/`'AAII'` publish path.
- `MODDING_ARCHITECTURE.md` — slug keying, content registry, online/distribution layer (reused
  by X2 + Phase 5).
- annodue: `src/patch/dll_savestate.zig` (`TemporalCompressor`, the 10-region capture list).
- `CAMERA_ROADMAP.md` — the cinematic / replay-follow camera (§5.2 P4), AND **§2.3: the author's
  CE camera mod contains a commented-out ghost-replay prototype** that records the pod transform
  each frame and replays it onto pod[1] (`0xE29CCC`) — a working Tier-A-via-phantom-entity proof
  that exercises §1 (record), §5.2 P2 (phantom entity), and P3 (slot reuse).
- Memory: `replay_system_investigation.md` (this investigation, condensed).
