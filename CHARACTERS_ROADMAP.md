# SW_RACER_RE -- Extensible Roster / Custom Characters Roadmap

**Status:** design (2026-07-07). Living document. Owner: lightningpirate.

Goal: turn the game's hardwired **23-pilot roster** into an **extensible roster of arbitrary
size N**, so that characters can be *added* rather than *swapped in*. The immediate proof is
making the two secret pilots -- **Cy Yunga** and **Jinn Reeso** -- into first-class, separately
selectable racers (no cheat code, no clobbering Bullseye/Mars Guo). The longer arc is a
data-driven character registry that dovetails with the modding-content direction.

Lives in the `dinput_hook/` Detours layer (delta), **not** `src/`. The faithful `src/Swr` decomp
stays untouched; this roadmap relocates data and lifts a few bounds on top of it.

> Addresses are from the Ghidra DB and MUST be reconfirmed against the live DB at implementation
> time (Steam `.text` is SteamStub-encrypted on disk; live values via Cheat Engine per the
> runtime-verify workflow). Effort tags: **S** < ~half day, **M** ~1-2 sessions, **L**
> multi-session.

---

## 0. How it works today (RE'd 2026-07-07)

The two secret pilots are **not roster entries**. Each cheat combo overwrites an existing slot
in place:

- `swrRace_ReplaceMarsGuoWithJinnReeso` @ `0x0044B530` -> rewrites `swrRacer_PodData[8]` (Mars Guo)
- `swrRace_ReplaceBullseyeWithCyYunga` @ `0x0044B5E0` -> rewrites `swrRacer_PodData[22]` (Bullseye)

Each swap rewrites the slot's `name`/`lastname`/`pod_modelID`/`pod_alt_modelID`/`puppet_modelId`
**and** patches that character's entry in the engine/cockpit-xform table (Table 3 above: entry 8 =
`0x004c73f4`+`0x004c740c..0x004c742c`; entry 22 = `0x004c7a00..0x004c7a14`). It does **not** write
`PodHandlingData`, so the secret pilot inherits its host's handling (Jinn Reeso drives like Mars
Guo, Cy Yunga like Bullseye). Consequences of the in-place swap: the secret pilot is mutually
exclusive with its host, the change is global (an AI assigned that slot becomes the secret pilot
too), and the profile still thinks it selected the host.

### The 23-cap is spread across parallel fixed-address tables

**Three** parallel per-character tables (all 23 entries, all indexed by the character/racer id),
plus two menu-side buffers:

| Table | Addr | Layout | Notes |
|---|---|---|---|
| `swrRacer_PodData` | `0x004c2700` | `swrRacerData[23]`, stride `0x34` | id/models/names/sprite/puppet; ends `0x004c2bac` |
| `swrRacer_PodHandlingData` | `0x004c2bb0` | `PodHandlingData[23]`, stride `0x3C` | butts against the roster -- **no room to grow in place** |
| pod engine/cockpit xform (unnamed; propose `swrRacer_PodEngineXform`) | `0x004c7088` | stride `0x6c`, 23 entries -> ends ~`0x004c7a3c` | per-pod engine/cockpit transform vectors; indexed `score_ptr->unk18 * 0x6c` (P0-confirmed). The swaps patch entries 8/22. |
| `swrRace_SelectIndex` | `0x00e99240` | `int[23]` pairs (stride `8`) | select-screen menu list |
| per-player unlock mask | `0x00e35a94` | **32-bit int per player**, per-player stride `0x14` | bits 0..22 used -> **headroom to 32 characters** before the mask itself needs widening |

### The selection pipeline

`swrRace_BuildPartMenuList` @ `0x0043da10` loops `id = 0..0x16` (**the one hard 23-cap in the
menu path**), keeps ids passing `(unlockMask | 0x22e01) & (1<<id)` -- or all ids in multiplayer --
and writes survivors into `swrRace_SelectIndex`; `swrRace_MenuMaxSelection` (`0x00e295cc`) is the
resulting count (already dynamic). `swrRace_SelectVehicle` @ `0x00435700` maps the highlighted
menu slot back to a racer id via `swrRace_SelectIndex[sel*2]`, loads that pilot's pod/puppet and
stats, and on confirm writes the id into `hang->vehicleOpponent[]`, the per-player slot, and
(MP) `swrMultiplayer_RacerPick`.

### Enumerated consumers of the two per-racer tables (2026-07-07 xref pass)

**All are pure reads. Almost none carry a count constant** -- they index by a *runtime* racer id
(`vehicleOpponent[]`, `swrScores[i]` id, `FavoritePilot`, roster picks) and inherit their bound
from roster-size fields. This is the pivotal finding: to raise the ceiling we do **not** have to
rewrite these functions' logic -- only the *base address* they read from has to move.

`swrRacer_PodData` readers (~17): `swrObjHang_BuildRosterMultiplayer` (`0x45b610`),
`swrObjHang_BuildRosterSinglePlayer` (`0x45b947`), `swrObjHang_ComputeUpgradedStats` (`0x45cf85`),
`swrObjHang_LoadAllPilotSprites` (`0x457bd6`), `swrText_FormatPodName` (`0x4208ee`),
`swrUI_DrawRaceResultRow` (`0x41acf3`), `swrRace_SelectVehicle` (`0x435976`),
`swrObjHang_LoadScreenAssets` (`0x458900`), `swrObjHang_UpdateInspectCamera` (`0x439252`),
`swrRace_CourseInfoMenu` (`0x43c066`), `swrRace_ResultsMenu` (`0x43a797`),
`swrObjHang_UpdateTauntScene` (`0x43ca50`), `swrObjHang_AimHoloCamera` (`0x43f9da`),
`swrObjHang_UpdateHoloCameraTarget` (`0x43fc00`), `swrObjHang_UpdatePlanetSelectIntro`
(`0x43cef2`), `swrRace_AnimateDisplayPod` (`0x4340e4`), `swrObjHang_UpdateLookAtVehicle`
(`0x43801c`).

`swrRacer_PodHandlingData` readers (3, + 2 indirect): `BuildRosterMultiplayer`,
`BuildRosterSinglePlayer`, `ComputeUpgradedStats`; the handling entry pointer is then passed into
`swrRace_ApplyUpgradesToStats` / `swrRace_ComputeStatBars` (one entry at a time, no iteration).

**The only hardcoded `0x17` (23) loop** is `swrObjHang_LoadAllPilotSprites` @ `0x457bd6`, which
also derives `+0x17` / `+0x2e` sprite-slot offsets from the count of 23. It must be widened (or
reimplemented) for N.

### Ceilings that are NOT this problem

- `swrScores[20]` @ `0x00e29bc0` caps **simultaneous racers in a live race**, not the selectable
  pool. N pilots can exist; still <=20 race at once. Binds first only for "everyone races at once"
  scenarios, which is out of scope here. See [[racer-count-limit]].
- Content headroom is good: the game defines **pilot portraits 0..46** (`swrUISprite_pilotSprite_*`,
  index 23 pointedly skipped), i.e. far more portrait art than the 23-slot roster consumes. Cy
  Yunga / Jinn Reeso have their own pod (`MODELID_jinn_reeso_pod=299`, `MODELID_cy_yunga_pod=301`),
  alt-pod, and puppet (`304`/`305`) models, already handled by the HD-replacement layer.

---

## 1. Decisions locked (2026-07-07)

- **Foundation = operand-patch relocation, NOT reader reimplementation.** At startup, allocate the
  **three** per-character tables (`swrRacerData[N]`, `PodHandlingData[N]`, engine-xform`[N]`) on
  the heap, `memcpy` the 23 originals, append the extras, then **patch the base-address immediate
  operand** in every enumerated reader (P0 patch list: 73 PodData/handling read sites + the Table-3
  reader sites) so the *original, byte-identical game logic* transparently reads the enlarged
  tables. Because every operand is `oldBase + offset`, each table's sites shift by a single uniform
  delta (`delta_pod`, `delta_handling`, `delta_xform`). We move data and lift bounds; we do not
  rewrite behavior. Strictly more faithful than reimplementing ~20 functions, and it scales to any
  N. (Overlay-with-reimplementation was considered and rejected: same payoff, ~20 large functions
  diverging from original, larger maintenance/verification surface.)
- **Extended ids are always safe because the *data* moved, not the *dispatch*.** Since every
  reader now points at the N-entry arrays, an extended id is in-range everywhere a stock id is --
  no "must route through an accessor" invariant to babysit.
- **Seed from the swaps first, JSON later.** Phase 1 hardcodes Cy Yunga / Jinn Reeso as ids 23/24
  with data lifted verbatim from the two swap functions (roster fields + tuning-blob writes),
  proving the whole pipeline. The data-driven registry (Phase 3) generalizes this and aligns with
  [[modding-content-system]].
- **Faithful sim preserved.** Logic runs original .text. For run-verification, the only delta is
  data addresses + a handful of loop/mask immediates; behavior for the stock 23 is unchanged. See
  [[run-verification-roadmap]].

### The load-bearing risk

The relocation is only correct if the operand-patch set is **exhaustive**. A missed base-address
immediate means that one code path still reads the old 23-entry array -- fine for ids 0..22,
**out-of-bounds for an extended id**. P0 must produce the complete operand set from a Ghidra
range scan, not just the containing-function list already gathered.

---

## 2. Phases

### P0 -- Finish the RE (DONE 2026-07-07, except runtime portrait check)

Method: `/xrefs_to` HTTP endpoint on the live GhidraMCP server, scanned at every 4-byte boundary
across each table range (all operands are 4-aligned, so the scan is exhaustive). Raw dumps in the
session scratchpad (`roster_operands.tsv`, `roster_patchlist.txt`).

1. **Exhaustive operand scan -- DONE.** Ranges `[0x4c2700,0x4c2bac)` + `[0x4c2bb0,0x4c3114)`
   yielded **85 referencing instructions**; excluding the 12 cheat-swap WRITE sites, **73 read
   operand sites** to repoint: **64 in PodData** (17 functions), **9 in PodHandlingData** (4
   functions, incl. `swrRace_PlayEngineSounds` which the function-list pass had missed). Confirmed
   absolute-slot reads at slot 1 (`0x275c`, LoadAllPilotSprites) and slot 2 (`0x2788`, TauntScene)
   -- these would be invisible to a base+field-only sweep, validating the full scan. **Every
   operand is `oldBase + offset`, so the patch is uniform: add `delta_pod` to each PodData
   disp32, `delta_handling` to each PodHandlingData disp32.**
2. **"Tuning blob" -- RESOLVED: it is a third per-character table.** Base `0x4c7088`, stride
   `0x6c`, indexed by `score_ptr->unk18` (character id). Readers: `swrRace_PoddAnimateEngines`
   (`0x471173`/`0x4714a1`), `swrRace_AnimateDisplayPod` (`0x43407a`/`0x433ab2`/`0x433d86`),
   `swrRace_UpdateHoverPads` (`0x476782`/`0x4767aa`), `swrObjcMan_UpdatePreRaceSweep`
   (`0x451f6e`/`0x451f97`), + the 2 swap writes. Slots 23/24 read garbage unless relocated ->
   **joins the relocation set (third delta)**; seed entries 23/24 from the swap writes.
3. **`swrRace_SelectIndex` + unlock mask -- DONE.** SelectIndex `@0xe99240` (23 int-pairs): written
   by `BuildPartMenuList` (`0x43da4e`), read by `swrRace_SelectVehicle` (11 sites),
   `swrUI_Menu_MpSelectVehicle` (`0x4192d8`), + Navigate/Layout via `&DAT_00e99244`. Grow to N +
   repoint. Unlock mask `@0xe35a94` = **one 32-bit int per player** (stride `0x14`) -> **headroom to
   32** before the mask needs its own relocation; for N<=32 just set bits 23/24 (or add to the
   always-unlocked mask `0x22e01 | 0x1800000`).
4. **`LoadAllPilotSprites` @ `0x457bd6` -- DONE (wrinkle found).** Loops `id<0x17`, reads
   `PodData[id].pilot_spriteId` (`pSVar1 += 0xd` = one 0x34 entry), and creates **three sprite
   bands** at slots `id`, `id+0x17`, `id+0x2e`. Those band offsets (23/46) are baked into every
   fixed pilot-sprite consumer, so the 2D portrait sprites **do NOT relocate by a uniform delta**
   like the roster tables -- widening the bands would shift every downstream sprite id. Treat 2D
   portraits as a separate sub-problem (append extra-pilot portraits after the third band, or a
   parallel range; consumers for extended ids computed specially). **Not P1-blocking:** the select
   screen uses 3D pod/puppet models, and results rows read `name`/`lastname`; the swaps never set
   `pilot_spriteId` anyway. Portraits for extended ids -> P2/P3 via [[sprite-texture-replacement]].
5. **Newly surfaced readers to name** (project convention): `swrRace_PlayEngineSounds`,
   `swrRace_UpdateHoverPads`, `swrObjcMan_UpdatePreRaceSweep` already have names; verify + fold any
   `FUN_` stragglers when the P1 branch opens.

**P0 exit -- MET:** complete uniform patch list (73 read sites, 2 deltas), third table identified
& scoped, SelectIndex + unlock-mask widths known, portrait path scoped out of P1. Only open item
is a runtime check of whether base sprite art for Cy Yunga / Jinn Reeso exists (a P2 concern).

### P1 -- Proof: Cy Yunga + Jinn Reeso as real ids 23/24 (L)

- Delta module owns the three big arrays (`swrRacerData bigRoster[25]`,
  `PodHandlingData bigHandling[25]`, engine-xform `bigXform[25]`); copy 23 originals, fill 23 =
  Jinn Reeso, 24 = Cy Yunga -- PodData + Table-3 entries lifted from the swap writes, handling
  inherited from each one's host (Mars Guo / Bullseye) for now.
- Apply the P0 operand patches (three uniform deltas) so all readers point at the big arrays; grow
  `SelectIndex` + repoint its readers; patch `BuildPartMenuList`'s `0x17` bound to 25 and add ids
  23/24 to the always-available mask (`0x22e01 | 0x1800000`). Skip the `LoadAllPilotSprites`
  band-widening (portraits deferred -- see P0.4).
- Scope to **freeplay single-player** first (simplest path). Verify: both appear in select, load
  correct pod/puppet/stats, race correctly, appear correctly in results.
- Leave the two cheat swaps in place for now (harmless; retired in P2).

Exit: play Jinn Reeso and Cy Yunga in a freeplay race alongside Mars Guo/Bullseye, verified.

### P2 -- Parity: MP, save/unlock, HUD/results (M)

- Multiplayer: `RacerPick` transmits an id byte (headroom to 255); confirm the remote maps id->big
  roster (it will, once relocated). Splitscreen roster build likewise.
- Save/profile: selected-racer id + unlock bits must round-trip for ids >= 23 (mask width from P0).
- HUD standings / results rows / course-info favorite: confirmed by the operand patch, but
  runtime-verify names + portraits for the new ids.
- Retire the cheat swaps (or repoint them to "select id 23/24").

Exit: new pilots work in MP + splitscreen, persist across save/load, render correctly everywhere.

### P3 -- Data-driven registry + custom characters (L)

- Externalize `CharacterDef { swrRacerData pod; PodHandlingData handling; /* + tuning */ }` into a
  JSON game file; the delta builds `bigRoster[N]` from stock 23 + JSON extras at load. Aligns with
  [[modding-content-system]] / [[modding-api-roadmap]].
- Custom pod models via the model-loader wrap ([[asset-replacement-architecture]]); custom
  portraits via [[sprite-texture-replacement]]; custom names via the localization overlay
  ([[localization-subsystem]]).
- Unlock-progression hookup so added characters can be gated by career progress if desired.

---

## 3. Open questions

- Tuning-blob indexer (P0.2) -- decides whether it joins the relocation set.
- Does base art include Cy Yunga / Jinn Reeso portraits, or must P1 ship them (P0.4)?
- Unlock-mask width: is it a single 32-bit field (headroom to 32 pilots before its own
  relocation) or narrower? (P0.3)
- Career/tournament opponent tables that reference pilots by id -- any that assume <=22? (audit in
  P2).

Related: [[hangar-frontend-subsystem]], [[race-manager-subsystem]], [[ifly-cheat]],
[[racer-count-limit]], [[pod-handling-stats-subsystem]], [[modding-content-system]],
[[sprite-texture-replacement]], [[project-conventions]].
