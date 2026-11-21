# SW_RACER_RE — Standalone Build Roadmap

**Status:** sizing / design (2026-11-17). Living document. Owner: lightningpirate.

> **2026-11-21 re-measure (`upstream/dev @ cb5ab18e`, after #421-#442):** 1895 of 1896 reimpl slots have real bodies; the only stub left is `DirectInputCreateA`. The section 2 table below is the 11-17 snapshot -- the "~680 real bodies to write" bucket is now essentially done; the remaining standalone work is wiring (sections 3-6), not writing bodies.

Goal: build a playable `SWEP1RCR` executable **entirely from `src/` + `dinput_hook/`**, with no
retail `SWEP1RCR.EXE` code executing, and prove it behaves identically to retail. A 64-bit build
is a later, separate milestone that depends on this one (section 7).

> Numbers below were measured on `upstream/dev @ e5c7e40d` (after #383) against the live Ghidra
> DB (2141 functions). Re-measure before quoting them (section 9). Effort: **S** < ~half day,
> **M** ~1–2 sessions, **L** multi-session.

---

## 1. Where we are today

The project is a **hook DLL**: `dinput.dll` loads into retail `SWEP1RCR.EXE`, and the
`dinput_hook/` Detours layer forward-hooks the functions it owns (renderer, input, UI/QoL
deltas). Everything else runs retail `.text` byte-for-byte.

The `src/` reimplementations are **reverse-hooked and dormant**: `hook_generated.c` points our
body back at retail, and `src/` has **zero** `// 0xADDR HOOK` annotations on dev. So the 1141
"fully reimplemented" bodies compile, link and have been reviewed, but **have never executed in
game.** A standalone build is the first time most of that code runs.

## 2. The gap, function by function

Every DB function, classified by what `src/` has for it:

| Status | Count |
|---|---|
| Full body (no `HANG`) | 1141 |
| Pure stub (`HANG("TODO")`) | 243 |
| Partial (`HANG` in a branch) | 8 |
| Header-only (`_ADDR` + prototype, no body) | 672 |
| No source at all | 77 |

What the ~1000 unfinished functions actually are:

| Bucket | ~Count | Standalone needs | Effort |
|---|---|---|---|
| CRT / compiler runtime / Win32 imports (`Platform/cstdlib.h` 140 + ~63 no-source `stdlib_*`, `__aulldiv`, SEH, incl. `FUN_0049fee0` / `FUN_004a0590`) | ~205 | Nothing — link a real CRT; map the `stdlib_*` names to it | S |
| `Platform/cifr.h` — Immersion IFORCE force-feedback SDK wrapper | 20 | Replace with the existing rumble path | S–M |
| Already owned by a `dinput_hook` delta (46 stubs + 48 header-only) | ~94 | The delta becomes the implementation (section 4) | M |
| Thunks / nullsubs / game-side `FUN_` | ~8 | Trivial; name the 3 new `FUN_` (0x43f770, 0x454920, 0x454990) | S |
| **Real game/engine bodies to write** | **~680** | Faithful reimpl, verified (section 6) | **L** |

Biggest header-only piles inside the ~680:

| Header | Header-only fns | Notes |
|---|---|---|
| `Swr/swrUI.h` | 117 | Menu builders (`swrUI_Menu_*`) and widget plumbing |
| `Swr/swrMultiplayer.h` | 70 | Lobby/messaging; pairs with the DirectPlay replacement (section 5) |
| `Swr/swrObj.h` | 70 | Hangar, pause, in-race input buffer |
| `Swr/swrModel.h` | 44 | Animation update, texture tiles, raycast |
| `Swr/swrRace.h` | 38 | Menus, death/respawn, engine sounds |
| `Main/swrControl.h` | 32 | Bindings, device scan, config parse |
| `Swr/swrText.h` / `swrSound.h` / `swrConfig.h` / `swrSprite.h` | 17 / 14 / 13 / 11 | |

The 243 stubs cluster in the same Swr subsystems plus Win95 (36), Engine (23), Platform (16) and
the Dss netcode.

**Caveat:** this assumes Ghidra found every function. Some DB bodies are known to be broken
(1-byte bodies that hide calls from xrefs), so expect a few more to surface once the linker
starts reporting undefined references.

## 3. Globals and static data

Today every global is a fixed-address macro, generated from `data_symbols.syms` through
`globals.h.j2`:

```c
#define diDataFormat (*(DIDATAFORMAT(*))0x0049e720)
```

Standalone needs **real definitions with initial values**:

1. **Generator flip (S).** The template already exists; emit `extern` declarations in `globals.h`
   and definitions in a generated `globals.c`, behind a build option so the hook DLL keeps the
   macros.
2. **Initial data (M).** Dump `.data` / `.rdata` initial values for every named global (via
   GhidraMCP `read_memory`) and emit them as initializers. Lookup tables, string tables, default
   configs and the like all live here.
3. **Unnamed globals (L).** ~1000 `DAT_` items still exist in `.data` / `.rdata`. Anything a body
   touches must be named and typed; the rest can be emitted as opaque blobs at first.
4. **Layout-dependence audit (M).** Retail code sometimes relies on adjacency: overflowing one
   array into the next global, or `base + 0x538`-style arithmetic across fields. Once the linker
   places globals freely those break silently. Catch them by building the standalone with
   AddressSanitizer (the clang/ASan CI from #328 already exists) and running the test runner.

## 4. The render/display stack

By the 2026-07-09 policy, the ~120–140 functions the GL takeover replaces (std3D, stdDisplay,
DirectDraw/Direct3d, rdCache submit, rd*_Draw, swrRender scene build, swrSprite/swrText draw,
Window video, swrModel texture load) are **not** reimplemented faithfully. For standalone the
`dinput_hook` delta simply **becomes** the implementation: link the delta bodies directly instead
of Detours-patching them over retail.

Work: a build mode where `hook_replace(X, X_delta)` turns into a direct definition of `X`
(or the call sites bind to `X_delta`), plus removing the Detours dependency for that build.
Effort **M**.

## 5. Third-party libraries

| Library | Used for | Replacement | Status |
|---|---|---|---|
| DirectPlay | MP transport (`sithMulti`, `swrMultiplayer`) | Transport at the stdComm seam (DPID-keyed) | Designed — see the MP transport notes |
| DirectSound / A3D | Audio | OpenAL Soft behind the swrSound seam | Roadmap exists |
| DirectInput | Input | GLFW/SDL path already in `dinput_hook` | Partially live |
| Immersion IFORCE (`cifr`) | Force feedback | Existing rumble path | All 20 wrapper fns reimpl'd (#389/#390, 2026-11-17); SDK calls still need swapping |
| Smush.dll | Cutscenes | GL renderer already handles playback | Mostly done |
| DirectDraw / Direct3D | Rendering | GL takeover | Done (section 4) |

## 6. Verification: lockstep matching

The target is **behavioural** matching, not byte matching. Byte matching (as in the Racer
Revenge decomp) would require the original MSVC toolchain and buys nothing here. What matters is
that our code computes exactly what retail computes. Bit-exact is realistic: on the x87 build
(`-mfpmath=387`), the 54 math cases in the first harness run went to **54/54 bit-exact** once
float constants were written as proper float32 literals.

Three layers, cheapest first. Each builds on the one before.

### 6.1 Per-function differential testing (exists — PR #285, OPEN)

Hook-mode switch (`original` / `reimpl` / `both`, configured per function via
`reimpl_config.txt`) plus a differential harness: in `both` mode, call retail and ours with the
same inputs and compare bit for bit. It has already found 12 real bugs (#283). Covers pure
functions as-is (136 candidates surveyed); extend to global-reading functions by
snapshot/restore of the globals they touch.

**Merging #285 is the first step of this whole roadmap** — 6.2 and 6.3 both depend on its mode
switch.

Lessons already paid for (keep them):
- Never characterise a residual from an incremental build; reconfigure into a fresh build dir.
- Refuse to compare when the retail prologue is already Detours-patched (0xE9 / 0xEB / 0xFF 0x25)
  — otherwise you are comparing against the delta, not retail.
- Read constants from `.rdata` and write the shortest float32 round-trip literal; Ghidra's
  printed decimals often don't round-trip, and a constant's type can change which branch runs.

### 6.2 Live per-subsystem flips (M)

Set one subsystem to `reimpl`, leave everything else `original`, and run test-runner plans
(#334). A divergence is attributable to that subsystem because its callees are still retail.
Walk subsystems in call-graph order (the reimpl dependency planner already produces it).

### 6.3 Tick-level lockstep (L)

Two processes on the same input stream with the same dt: retail, and the build under test.
Every simulation tick, hash the sim state and compare; on a mismatch, dump the first differing
field and tick.

State to hash: pod transforms and velocities, the `swrRace` structs, `swrUtils_randState`
(0x0050cb7c), per-animation `animation_time` across `swrScene_animations`, judge/lap state.

Determinism facts already established (from the run-verification work):
- RNG is **one** 4-byte integer LCG (`swrUtils_Rand` @ 0x4816b0) — bit-identical across CPUs.
- Wall-clock time does not feed single-player simulation.
- The build is x87, so FP is faithful by construction.

Hazards:
- **Cosmetic RNG draws happen at render rate** (weather, minimap, HUD), so the number of draws
  between gameplay draws depends on framerate. Either pin frame pacing identically on both sides
  or log gameplay-side RNG outputs and pin them.
- **Fixed-timestep (#194) changes the simulation** — it must be off for a vanilla comparison.
- Animation time carries over from the intro cutscene; snapshot it at T0 instead of assuming
  zero.

This machinery is shared with the replay/TAS and speedrun-verification work (REPLAY_ROADMAP
section 10, VERIFICATION_ROADMAP), so it pays off three times.

## 7. 64-bit (after standalone)

Possible, but harder, and it should follow a dedicated port-quality pass:

| Blocker | Why | Fix |
|---|---|---|
| In-place pointer fixups in asset loaders | Model/sprite/spline loaders patch 32-bit file offsets into pointers inside the loaded buffer; 64-bit pointers don't fit | A proper deserialization layer per format — **the biggest piece** |
| `types.h` assumes 4-byte pointers | Every pointer-holding struct changes size/offsets | Separate on-disk structs from in-memory structs |
| Pointer/int casts | `(int)&...` and hard-coded offsets | Audit + `intptr_t`, typed fields |
| x87 → SSE | Results stop being bit-exact; MP vs 32-bit peers desyncs | Keep the 32-bit standalone as the lockstep reference; explicit wire serialization for MP |

What 64-bit buys is mostly toolchain cleanliness and ports (Linux/ARM); the game doesn't need the
address space. Do it once the standalone exists and is verified.

## 8. Phases

| # | Phase | Depends on | Effort |
|---|---|---|---|
| 0 | Merge #285 (mode switch + harness) | — | S (review) |
| 1 | Name the 3 new `FUN_`; keep reimpl rounds going on the ~680 | — | L (ongoing) |
| 2 | Inverted bootstrap: an EXE built from `src/` that still loads retail for anything missing | 0 | M |
| 3 | Globals generator flip + `.data` initializers | 2 | M |
| 4 | Delta-as-implementation build mode (section 4) | 2 | M |
| 5 | Library replacements (section 5) | — | L |
| 6 | Per-subsystem live flips (6.2) | 0 | M per subsystem |
| 7 | Tick lockstep harness (6.3) | 0, replay input stream | L |
| 8 | Burn down the retail fallback until nothing loads from the EXE; ASan layout audit | 1–7 | L |
| 9 | 64-bit port-quality pass (section 7) | 8 | L |

Phase 2 is the key structural move: flip the current model inside out. Instead of a DLL patching
retail, build an EXE from `src/` that maps retail's image and calls into it only for functions we
don't have yet. Then the burndown is literally "remove fallbacks", and every step is playable.

## 9. Re-measuring

The classification script is `scripts/Ghidra/standalone_gap.py` (untracked, like
`reimpl_report.py`). Measure a clean tree, not a working branch:

```bash
git worktree add --detach <scratch>/wt_dev upstream/dev
curl -s "http://127.0.0.1:8080/list_functions?offset=0&limit=100000" -o <scratch>/live.txt
python -I scripts/Ghidra/standalone_gap.py <scratch>/wt_dev <scratch>/live.txt <scratch>/gap.json
git worktree remove --force <scratch>/wt_dev
```

Check `head -1 live.txt` is a `0x004xxxxx` SWE1R address (the GhidraMCP server is shared with the
Racer Revenge DB) and that it has >2000 lines before trusting the numbers.

## 10. Open questions

- How many functions Ghidra missed entirely (broken bodies) — only the linker will tell.
- Whether phase 2's inverted bootstrap can reuse the SteamStub-decrypted image at runtime, or
  needs a decrypted EXE on disk.
- Save-file and config compatibility guarantees for the standalone build.
- Whether `swrUI` menus are worth reimplementing faithfully or should move straight to the UI
  framework proposal.
