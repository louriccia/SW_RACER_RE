# Modding API Roadmap

Plan for issue #153 "Better modding API": reversible memory patching + hook
lifecycle + a config system, built so the public surface survives the eventual full
decompilation. Local planning doc (kept out of git via `.git/info/exclude`).

Scope note: this is the CODE/patch modding API (reversible hooks, mod toggling,
settings). It is distinct from the CONTENT modding work (externalizing track/racer/
circuit data to JSON, mostly via `tracks_delta.c`); they meet only at the settings
file. See issue: https://github.com/tim-tim707/SW_RACER_RE/issues/153

## Current state (RE-grounded, master @ 32427ce)

There are two independent hook layers; #153 is about the second one.

1. **`src/hook.c` - decomp reverse-hooks.** `hook_function` writes a 5-byte
   `jmp rel32` over a stock prologue to activate a C reimpl. Installed once at boot,
   no undo. This is the reimpl substitution, NOT a toggleable mod; leave it alone.
2. **`dinput_hook/hook_helper.cpp` - the runtime mod (MS Detours).**
   `hook_function`/`hook_replace` register into `std::map` `hooks` / `hook_replacements`
   keyed by address; `init_hooks` `DetourAttach`es ALL of them unconditionally at
   startup. `patchMemoryAccess` patches a single data/vtable pointer. Neither has undo.
   - Conflict handling TODAY: a second registration on the same address silently
     OVERWRITES the map entry (last-wins, no warning). So #153's "reject" is already
     stricter than the status quo.
3. **Scattered raw byte patches** via `VirtualProtect` + `memcpy`/`memset`: the
   `Window_Main` reroute in `main.cpp`, the NOP mods, the 100-lap de-index, etc. Zero
   undo. These (not the Detours hooks) are where a save-original journal matters most -
   Detours already saves bytes + builds a trampoline for the jmp-hook case.
4. **`set_ai_full_lod` (`dinput_hook/main.cpp:92`) is the prototype-in-miniature.** It
   already carries a `{addr, len, original[6]}` table and does a symmetric
   `on ? 0x90 : original[i]` rewrite. It is one mod, hand-rolled, in exactly the shape
   we want to generalize. Refactor it first.
5. **Settings = direct Win32 INI calls.** `GetModuleFileNameW` + `GetPrivateProfileIntW`
   + `WritePrivateProfileStringW` in `dinput_hook/imgui_utils.cpp:71` (read) / `:101`
   (write), plus `swrMultiplayer_delta.cpp` and `swrPlayerHUD_delta.cpp`. The game's own
   `swrConfig_Read*` / `Write*` (`src/Swr/swrConfig.h`) call the same Win32 API
   internally, so a shared parser could later subsume both.

## Design principles (why this shape)

Validated against mature modding scenes - they converged independently:

- **Compose-by-default + explicit priority; quarantine the exclusive ops.** Harmony
  (Prefix/Postfix stack, ordered by priority; Transpiler/`return false` are the conflict
  cases), Fabric Mixin (`@Inject` composes, `@Overwrite`/`@Redirect` are discouraged),
  WoW (`hooksecurefunc` is append-only post-hooks - cannot conflict, cannot even unhook).
  => observers compose, replacement is at most one per target.
- **Curated named extension points beat arbitrary patching.** Forge's event bus and
  WoW's API are pre-injected hook points; raw bytecode/memory patching is everyone's last
  resort and primary conflict source. => provide named events, not just "hook any addr".
- **Abstract the unstable address layer.** SKSE's Address Library and Openplanet's
  managed API exist solely to kill hardcoded offsets. => the public mod surface must
  reference NAMED symbols, never raw `0x...`.
- **Make conflicts visible, not magically resolved.** Bethesda load-order + xEdit. We can
  guarantee MECHANICAL composition (both run, memory intact); we cannot guarantee SEMANTIC
  compatibility (mod B reads what mod A changed). Surface a conflict report; never silent.

**Governance vs backend (the decomp-invariance argument).** Split the design in two:

- *Governance layer* = registry, observer/replacement, priority, conflict report, named
  events, mod lifecycle. This is identical whether the backend is byte-patches now or
  linked hooks after a full decomp. It is the part worth getting right; it SURVIVES.
- *Backend layer* = `WriteMemory` + undo journal + Detours. A full decomp DELETES most of
  this (no addresses, no prologue-stealing, no live-patch races - you recompile instead).

A 100% decomp removes the ACCIDENTAL complexity (offsets, byte-patching, thread-safety of
live `.text` writes) but NOT the ESSENTIAL one: two mods wanting to influence the same
behavior still collide - it just becomes a source merge conflict or the same registry
problem. Even fully-decompiled source ports (Ship of Harkinian, OpenRCT2, OpenRA) rebuild
a plugin API + conflict model anyway. So: design the governance now, keep addresses out of
the public surface, let the backend be swapped later.

## Phase 1 - Reversible patch backend + journal  [MVP, = issue Phase 1]

- **Goal:** a single primitive every memory mutation routes through, with automatic undo.
- **API (new `dinput_hook/patch.{h,cpp}`):**
  - `bool WriteMemory(ModId owner, void* addr, const void* src, size_t len)` - capture the
    TRUE original bytes once (refcount per overlapping range), `VirtualProtect` ->
    `memcpy` -> restore protection, record `{owner, addr, original}` in a journal.
  - `bool PatchPointer(ModId owner, void* addr, void* value)` - owner-tracked
    `patchMemoryAccess`.
  - `void UndoOwner(ModId owner)` - replay that owner's journal in REVERSE.
  - `unhook_function` / `unreplace_hook` - Detours `DetourDetach` of the recorded
    trampoline (issue's "second phase"); journal-revert for non-Detours patches.
- **Guards (the issue's two "check that we don't..." clauses, done right):** detect
  *byte-range* overlap (not "same function"), including Detours' stolen prologue window;
  refcount shared bytes so disabling owners out of order restores stock, not a peer's
  patch.
- **Deliverable / proof:** port `set_ai_full_lod` onto it (no behavior change), then bring
  `patchMemoryAccess` + the raw `VirtualProtect` sites under it. Pure centralization +
  the new ability to undo. No public mod API yet.
- **Lift:** medium. Locus: `dinput_hook` only.

## Phase 2 - Mod module + registry (governance MVP)

- **Goal:** make a feature a unit you can enable/disable, Blender-add-on style.
- **API:**
  ```c
  typedef struct {
      const char* name;
      const char* version;
      void (*enable)(ModId self);   // calls WriteMemory/Hook* tagged with self
      void (*on_disable)();         // OPTIONAL: non-memory teardown only (GL/textures)
  } ModModule;
  ModId register_mod(const ModModule*);
  void  enable_mod(ModId);
  void  disable_mod(ModId);         // on_disable() then UndoOwner(self)
  ```
- **Key win over annodue/Blender:** because Phase 1 captured the originals, `disable_mod`
  auto-reverts via the journal - the author writes only `enable()`, no hand-mirrored
  unregister to get wrong. `on_disable` is reserved for genuinely un-journalable resources
  (GL textures, file handles).
- **Deliverable:** register the existing feature deltas as mods - `ai_full_lod`, 100-lap,
  splitscreen, HD pods, overhead racer names. `hook_generated`'s always-on hooks stay the
  "core"; only opt-in features get a `ModId` and route through the journal.
- **Lift:** medium.

## Phase 3 - Conflict policy: observer/replacement registry

- **Goal:** let compatible mods touch the same function; resolves #153's "don't hook the
  same address twice" without forbidding legitimate composition.
- **Approach:** mods do NOT call `DetourAttach` directly. They register intent per target;
  the host owns exactly ONE real detour per target and multiplexes:
  - `AddObserver(ModId, void* target, Phase before|after, cb, int priority)` - N allowed,
    they compose, ordered by priority (Harmony/Mixin/WoW model).
  - `SetReplacement(ModId, void* target, void* fn)` - at most ONE; fails loudly if taken.
- **Why:** kills the LIFO-detach hazard of chained raw detours (disable a mid-stack mod by
  list-removal, not fragile re-stacking), and makes the only hard conflict "second
  replacement" / "overlapping byte writes".
- **Conflict report:** when an exclusive request collides, surface WHICH two mods + WHICH
  target. Never the silent map-overwrite of today.
- **Boundary to document:** this guarantees mechanical composition only. Semantic conflict
  is irreducible in every architecture - we make it visible, not impossible.
- **Lift:** medium-large.

## Phase 4 - Settings: key-value parser + file-watch  [= issue Phase 2]

- **Goal:** comment-aware config that drives mod toggling without ImGui.
- **Parser:** `key = value`, `#`/`;` comments, sections. Replace the Win32 INI calls in
  `imgui_utils.cpp` + `swrMultiplayer_delta.cpp` + `swrPlayerHUD_delta.cpp`. Drops the
  `GetModuleFileNameW`/`GetPrivateProfileIntW`/`WritePrivateProfileStringW` dependence.
- **Auto-refresh:** a file-watcher (`ReadDirectoryChangesW`, or an mtime poll on the
  existing per-frame tick) reparses on change, DIFFs against live state, QUEUES toggles,
  and applies them on a FRAME BOUNDARY (see pitfall: live `.text` patching races the game
  thread) -> calls `enable_mod`/`disable_mod`. This is how "toggle mods without imgui"
  works: the file is the source of truth, the watcher is the driver into Phase 2/3.
- **Optional follow-up:** hook the game's own `swrConfig_Read*`/`Write*` so game + mod
  share one config file (tim's "listen for changes" line).
- **Depends on:** Phase 2 (the enable/disable targets). The parser itself is independent
  and can land first.
- **Lift:** medium.

## Phase 5 - Named events + address abstraction (decomp-survivable public API)

- **Goal:** the Forge-bus lesson - give mods stable named hooks instead of raw addresses.
- **Approach:** define a small curated event set the game fires (candidates: `OnRaceLoad`,
  `OnRaceEnd`, `OnFrame`, `OnPodStateUpdate`, `OnMenuDraw`), implemented internally via
  Phase 3 observers on the relevant functions. Mods subscribe by event name + symbol name,
  never `0x...`. This is the surface that survives the decomp; the byte-patch backend can
  be swapped for linked hooks under it without breaking mods.
- **Lift:** medium, incremental (add events as demand appears).

## Phase 6 - External plugins + decomp transition  [FUTURE]

- **Goal:** out-of-tree mods (today every "mod" is compiled into `dinput.dll`).
- **Approach:** annodue-style `LoadLibrary` + resolve C-ABI lifecycle exports
  (`enable`/`on_disable`/event callbacks) + a manifest (name/version/priority/deps) + a
  plugin manager. Each plugin gets a `ModId` and plays by Phases 1-5.
- **Decomp transition:** when reimpls become a recompilable source tree, retarget the
  backend (Phase 1) from byte-patches to compile-time/linked hooks while keeping the
  governance surface (Phases 2-5) intact.
- **Lift:** large; gated on real out-of-tree demand.

## Pitfalls (carried from the design discussion)

- **Mechanical vs semantic compatibility** - framework promise is "no corruption / no
  silent clobber", not "mods don't logically stomp each other". Document the line.
- **Detours steals the prologue** - a byte-patch inside the relocated window silently
  no-ops and a naive overlap check misses it; the checker must know the stolen range.
- **Toggle != observable revert** - `set_ai_full_lod` only takes effect on next race load;
  reverting bytes does not unload already-loaded full-LOD models. Mods need a
  "requires-reload" flag.
- **Apply-time thread safety** - patching `.text` from the ImGui thread while the game
  thread executes it is a race; Detours suspends threads in a transaction, raw
  `WriteMemory` does not. Hence frame-boundary toggle application (Phase 4).
- **Cross-layer surprise** - a target may already be a `jmp` to a `src/hook.c` reimpl, so
  an observer wraps the reimpl, not the stock fn. Record "this target is reimpl-
  substituted".

## Sequencing

1 (backend + journal) -> 2 (registry + lifecycle) -> 3 (conflict policy) -> 4 (settings,
parser can overlap 1-2) -> 5 (named events) -> 6 (external plugins, future). Phases 1 + 4
alone satisfy issue #153; 2/3/5 are the "do it the way the mature scenes did" layer; 6 and
the decomp retarget are the long horizon.

## Open questions

- File-watch trigger: `ReadDirectoryChangesW` on its own thread vs an mtime poll folded
  into the existing per-frame tick (simpler, avoids a thread).
- Frame-boundary hook for applying queued toggles - reuse the same point the renderer/
  ImGui already run on?
- Do we expose `WriteMemory`/raw addresses to in-tree feature deltas during Phases 1-3
  (pragmatic) while keeping ONLY named events/symbols in the eventual external-plugin
  surface (Phase 5/6)? Recommended: yes.
- Mod metadata format for Phase 6 (manifest file vs exported struct) - defer until 6.
