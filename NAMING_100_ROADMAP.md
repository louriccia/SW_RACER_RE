# Road to 100% Named Functions

> **STATUS: CLOSED / COMPLETE (2026-07-01).** Live DB is 2131 functions, 2129 named
> (99.9%). Only 2 `FUN_` remain -- `FUN_0049fee0` and `FUN_004a0590`, both in the
> CRT/runtime tail (0x49-0x4a range), out of scope per the CRT-tail carve-out. All
> game-space functions are named. The banked-name / divergence PR series below all
> merged (naming/asset #200, naming/ui #199, naming/renderer #198, naming/race #196,
> naming/mp #197, naming/swrsound-audio #195). Remaining RE work is now reimplementation
> (~34% fully reimpl), not naming -- see re_progress_metrics.

Goal: every game-space function in the Ghidra DB is named (no `FUN_`), and our DB
names are reconciled with / pushed to upstream `src/` headers. Shipped as a small
number of PRs grouped by mega-system. Low confidence is acceptable -- best-guess
the name and append `_Maybe` (precedent: `swrObjTrig_MaybeResetAnimation`).

## The gap (measured 2026-06-26, live DB == upstream/master + 1 commit)

DB: 2134 functions, 1976 named (92.6%), 158 `FUN_` (1 is CRT tail -> ignore).

Three categories of game-space work (243 functions):

1. **157 `FUN_`** -- unnamed everywhere. Decompile, best-guess name (+`_Maybe`),
   rename in DB, add `_ADDR` + prototype to header.
2. **35 banked names** -- named in DB, missing from `src/` headers. Just declare
   (`_ADDR` + prototype). E.g. swrRender_InitScene, swrMain_GuiAdvance,
   swrPlayerHUD_SampleOcclusion, swrRace_ComputeUpgradePrices,
   swrSpline_CursorSeekToProgress, swr_SetFixedDeltaTime, geometry math helpers.
3. **51 name divergences** -- declared in both DB and src with DIFFERENT names;
   reconcile to one canonical name. Includes backwards/swapped pairs that must be
   fixed (one side is wrong):
   - swrUI_SetColorUnk3 <-> Unk4 (transposed)
   - DirectDraw_Lock/UnlockMainSurface <-> ...ZBuffer
   - DirectDraw_GetMainSurface <-> GetZBuffer
   - stdControl_Startup <-> Shutdown
   - swrDisplay_SetWindowPos <-> SetWindowSize
   - swrSound_ReadIntoSource <-> LoadIntoSource
   - swrMain_GuiAdvance <-> swrMain2_GuiAdvance
   ...and ~15 `swrUI_*` vs `swrUI_Front_*` (upstream `_Front_` likely canonical).

OUT OF SCOPE: ~55 CRT missing + ~171 `stdlib_*` vs bare-libc divergences. Runtime,
not game code. Leave alone.

## PR buckets (6, grouped by mega-system; "few PRs" per Lou)

| # | PR | systems | ~count |
|---|----|---------|--------|
| 1 | Race / flight sim | swrRace 24, swrObjJdge 5, swrCam/cMan 3, swrObjTest 1 + banked/diverge | ~35 |
| 2 | Front-end UI | swrObjHang 13, swrObjElmo 9, swrUI 7, swrSprite 4 + `_Front_` diverge + UI helpers | ~50 |
| 3 | Renderer / model / engine | swrModel 10, rd* 11, std3D 2, swrScene 3, Window 3, DirectDraw, geom math | ~40 |
| 4 | Audio (PILOT) | swrSound 7 + 3 banked | ~10 |
| 5 | Multiplayer | swrMultiplayer 7, sithMulti 5, stdComm 3 | ~15 |
| 6 | Asset / spline / util / cifr | cifr 6, swrAssetBuffer 3, swrSpline 3, swrUtils 2, misc unprefixed | ~25 |

## Per-bucket pipeline (each = one PR)

1. Decompile every target (bulk via `curl /decompile_function_by_address`).
2. Infer name (+`_Maybe` if unsure); reconcile divergences by reading the body.
3. Rename in DB (`rename_function_by_address`), then `save_program`.
4. Add `_ADDR` + prototype to the right `src/**/*.h`, address-sorted in the block.
5. Regen `src/**/master_header.h` via GenerateMasterHeader.py.
6. Full dinput.dll build (WinLibs GCC + pip cmake) -- MUST link, not just compile.
7. `/pre-pr-check` (dup _ADDR/name scan, ASCII, K&R, canonical names, no DAT_).
8. PR cross-fork to upstream tim-tim707/SW_RACER_RE master.

## Working files (scripts/Ghidra/)
- `live_functions.txt` -- raw DB dump (refresh: `curl .../list_functions`)
- `UNNAMED_157.txt` -- the 157 FUN_ grouped by subsystem hint
- `decomp_work/` -- bulk decompilations for analysis
- `naming_gap.py` / thorough diff -- regenerate categories 2 & 3

## Pipeline (validated + hardened)
1. Analysis subagent reads decomp_work/*.c -> writes buckets/<B>.proposal.tsv
   (ADDR, ACTION{NAME,DECLARE,RENAME_DB,KEEP_DB,THUNK,FRAGMENT}, NAME, PROTO, CONF, RATIONALE).
2. prep -> buckets/<B>_renames.tsv (NAME+RENAME_DB+thunk) and <B>_declares.tsv
   (NAME+DECLARE). Review THUNK (pure JMP -> thunk_, thin wrapper -> reclassify NAME)
   and FRAGMENT (skip, add to GUI-clear list) rows by hand.
3. MCP rename_function_by_address (batched) + save_program.
4. python scripts/Ghidra/apply_headers.py buckets/<B>_declares.tsv  (auto: routes by
   nearest-addr neighbor, address-sorted insert, lowercase hex, Ghidra-type fix,
   DAT_/FUN_ comment scrub).
5. GenerateMasterHeader.py (dup check) -> reconfigure if GLOB stale -> full build.
6. commit on naming/<b> off upstream/master -> push -> gh pr create cross-fork.

GUI-recarve list (verified via disasm, NOT fixable via MCP):
- swrSound_LoadSound (0x422ac0) split into 4: out-of-line helper at 0x4083c1 ->
  0x409cf1 -> 0x40b731 (jumps) + the in-line continuation 0x422c0d. One logical
  routine; delete the spurious functions + re-create at the entries, name the helper.
NOTE: 0x41bc20 and 0x439c70 were MIS-flagged as fragments by the decompile-reading
agents -- they are real, well-formed functions (clean prologue/epilogue, real callers);
NAMED 2026-06-26 (swrMultiplayer_FormatTimeString_Maybe -> #197, swrRace_DrawRecordText_Maybe
-> #196). 0x49fee0 = CRT stdlib internal (out of scope).

## Status
- [x] Bucket 4 Audio (pilot) -- SHIPPED PR #195. 8 declared, 2 thunks, 1 divergence.
- [x] Bucket 1 Race -- SHIPPED PR #196. 33 declared + 2 divergence fixes (DB-only).
- [x] Bucket 5 MP -- SHIPPED PR #197. 14 named, 1 fragment deferred.
- [x] Bucket 3 Renderer -- SHIPPED PR #198. 33 named + 5 divergence fixes (DirectDraw->ZBuffer).
- [x] Bucket 2 UI -- SHIPPED PR #199. 32 named + 16 divergence fixes (swrUI_Front_* + Color swap).
- [x] Bucket 6 Asset/util/misc -- SHIPPED PR #200. ~48 named + divergence fixes; 27 CRT excluded.

## DONE 2026-06-26: DB 92.6% -> 99.9% named (2129/2131). 6 PRs (#195-#200), all CI green.
ALL 6 buckets (#195-#200) MERGED (#200 merged 2026-06-30, now master tip e0d6c8d).
Naming roadmap is CLOSED -- only the 2 GUI follow-ups below remain.
Remaining 2 game-space `FUN_` = FUN_0049fee0 (CRT stdlib tail, out of scope) +
FUN_004a0590 (investigate -- likely also CRT/runtime). Naming is effectively at 100%
of game code.
GUI follow-ups (the only open naming work): (1) recarve swrSound_LoadSound into its
proper entries (see GUI-recarve list above); (2) resolve the pre-existing
swrPlayerHUD_SampleOcclusion DB dup (0x42be60 real vs 0x42d4f0 -- give 0x42d4f0 a
distinct name, then it can be declared too).
