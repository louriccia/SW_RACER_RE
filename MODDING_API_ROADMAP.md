# Modding API Roadmap

Plan for issue #153 "Better modding API": reversible memory patching + hook
lifecycle + a config system, built so the public surface survives the eventual full
decompilation. Local planning doc (kept out of git via `.git/info/exclude`).

Scope note: this is the CODE/patch modding API (reversible hooks, mod toggling,
settings). It is distinct from the CONTENT modding work (externalizing track/racer/
circuit data to JSON, mostly via `tracks_delta.c`); they meet only at the settings
file. See issue: https://github.com/tim-tim707/SW_RACER_RE/issues/153
Update 2026-11-20: content packs now share this API's governance (one `mod.json` manifest, the
same dependency / conflict / sim-affecting rules); the content formats themselves stay in
MODDING_ARCHITECTURE.md. See "End-game architecture" and Phase 6 "Hosting and distribution".
Update 2026-11-26: a 64-bit standalone build exists; see "64-bit build implications" (ABI
neutrality, lobby `arch` field, byte-patch backend retail-only).

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

## Design resolution (#153 discussion, 2026-06-28)

Tim's framing split the world into two buckets, and that split IS the design spine:

- **Journal (corruption-prone, must be reversible):** replace a full function *(which can
  generate new events)*; patch global variables.
- **Mod actions (behavioral):** run before / middle / after via event dispatch with tagged
  values; update some value from the stack / heap.

These are not two kinds of mod - they are **persistent vs transient mutation**. That is the
through-line for every question below.

### Layer map (the 3-layer model, made concrete)

- **Layer 0** - original assembly (the base).
- **Layer 1 (delta)** - the journal/patch kernel + the ONE-detour-per-target multiplexer +
  the event sources. "Permanent hooks emit events" lives here. The renderer replacement and
  the always-on reimpls are Layer 1 *providers*, NOT peer mods.
- **Layer 2 (mods)** - subscribers. They get events with tagged/mutable values; for a
  PERSISTENT change they call an owner-tagged journal write.

### Direct memory: the boundary is "no UNTRACKED mutation", not "no memory access"

A hard sandbox is unachievable (C in one address space - a mod can always deref a pointer)
and unnecessary. Split three ways:

- **Reads** - unrestricted, encouraged. Observation cannot corrupt or conflict; gating it
  is pointless. Mods inspect globals/state directly; no event needed merely to observe.
- **Persistent writes** ("always full LOD") - route through owner-tagged `WriteMemory` /
  `PatchPointer`. Reversible + conflict-checked. (Tim's "patch global variables -> journal".)
- **Transient writes** ("scale this pod's speed THIS call") - mutate the event payload in
  transit. No journal: nothing in `.text`/`.data` persists, scope is the call. (Tim's
  "update some value from the stack/heap -> event dispatch".)

So the rule: change a value FOR THIS CALL -> event, mutate in transit, no journal. Change it
PERSISTENTLY -> owner-tagged journal patch. Raw direct writes are not forbidden (can't be) -
they are UNSUPPORTED; the no-corruption / clean-disable guarantee only covers journal writes.

### Event structure

Events carry the args, and the args are MUTABLE. The hook context needs four things:

```c
typedef enum { HOOK_BEFORE, HOOK_AFTER } HookPhase;

typedef struct {
    HookPhase phase;
    void* args;          // typed struct for curated events; tagged-value array for generic
    void* ret;           // valid in HOOK_AFTER; mutable
    void* state;         // per-call scratch a BEFORE hook stashes for its AFTER (Harmony __state)
    bool skip_original;  // a BEFORE hook sets this to preempt the original
} HookContext;
```

- **Typed vs tagged payload - use both, at different tiers.** Curated named events
  (`OnRaceLoad`, `OnPodStateUpdate`) get a typed struct: type-safe, self-documenting,
  decomp-survivable. The generic "hook any address before/after" escape hatch uses tagged
  values (`{type, void*}` array matching the calling convention) because the dispatcher
  can't know arbitrary signatures. Curated events ARE the generic path with a signature
  declared once. Push modders to typed events; keep tagged for power users who declare the
  signature themselves.
- **"Middle" - deferred, and never arbitrary.** A mid-function hook needs a stable
  instruction offset + live register/stack knowledge, and it is the LEAST decomp-survivable
  construct in the design (a mid-function offset is meaningless after recompile). If ever
  needed, model it as a NAMED injection point the delta layer publishes (just another
  curated event that fires mid-function), never a raw offset supplied by a mod.

### The override spectrum (how hard a mod overrides)

1. **Observe** (read args/ret) - fully composable, cannot conflict.
2. **Mutate in transit** (rewrite args/ret) - composable, order-sensitive -> needs priority.
3. **Cancel** (`skip_original`) - semi-exclusive; flag it when other observers share the target.
4. **Replace** (own the impl) - exclusive; one per target.

Maps 1:1 to Harmony (Prefix-observe / Prefix-mutate / Prefix-return-false / full replace).

### Full replacement = an event SOURCE, not a black hole

The delta layer defines an EVENT CONTRACT per hooked function ("this target fires `OnX`
before/after"). Whoever owns the body honors it:

- The **original** (reached via trampoline) fires the events - normal case.
- A **replacer** takes the single exclusive slot AND inherits the obligation to fire the
  same canonical events, so before/after observers keep composing on top of it instead of
  silently breaking. It may also emit ADDITIONAL events (Tim's "could generate new events").
- **Two replacers on one target = the one true hard conflict** -> reject loudly, naming both
  mods + the target.

The **renderer** is the archetype: model it as a Layer-1 *provider* (owns the draw path,
emits `OnFrame`/`OnPodDraw`/...), not a peer Layer-2 mod. Visual mods depend on it and sit on
its events. Same pattern as SoH / OpenRCT2 keeping the renderer core, not a plugin.

### Forks - resolved

- **A. Cancel allowed?** YES, as a flagged capability. It's the difference between "one mod
  tweaks the result" and "one mod must seize the whole exclusive slot". Harmony allows it;
  it's useful and is NOT the same as replacement.
- **B. Replacers re-emit the original's events?** YES, by contract. Otherwise installing a
  replacer silently disables every observer on that target. (Tim's own "could generate new
  events" implies it.)

### One-line principle

The delta layer owns ONE hook per target and publishes its event contract; whoever owns the
body (original-via-trampoline or an exclusive replacer) must honor it; mods
observe/mutate/cancel through it (composable, priority-ordered); persistent writes go through
the owner-tagged journal. "Clean layering" = no UNTRACKED mutation, not no memory access.

This also dissolves a concrete pain: today each feature hand-writes its own wrapper
(`swrRace_InRaceTimer_delta`, `swrPlayerHUD_delta`, ...) and they collide (the recurring
3-branch merge conflict on the shared graphics-settings block). Under this model the delta
layer owns ONE hook per such function and fires an event; splitscreen / HD-pods /
overhead-names subscribe instead of forking the wrapper. The event system is the
formalization of the wrapper pattern already written by hand.

## Prior art: annodue (everalert/annodue) - deltas + what to borrow

Annodue is a runtime extension platform OVER the stock dgVoodoo game (no decomp goal). It is
already at our Phase 6: external DLL plugins (`LoadLibraryA` + C-ABI exports resolved by name
via `GetProcAddress` per a fixed `PluginExportFn` enum). Read of `core/Hook.zig`,
`core/SharedDef.zig`, `util/memory.zig` (2026-06-28).

Every plugin callback is `fn (*GlobalState, *GlobalFunction) callconv(.C) void` - two structs
are the whole API:
- `GlobalState` - host-computed, VERSIONED (=5) read-mostly snapshot of pre-digested game
  state (dt, fps, race_state, player heat/boost/dead/upgrades). Host fills it per frame.
- `GlobalFunction` - VERSIONED (=29) vtable of host capabilities plugins may call (settings,
  input, draw, freeze, hide-UI, toast, terrain/trigger resource requests).

**Convergence (validates us):** curated named hook points with Before/After variants
(`...B`/`...A` enum); host-owned multiplexing for scarce shared slots (`RTerrainRequest` /
`RTriggerRequest` -> request a slot, get a `Handle`, release-by-handle or release-all-by-
owner) = our one-owner-per-target registry, but they only built it where collisions bite;
owner identity threaded everywhere (`OwnerId`/`working_owner`, core `0x0000` / user `0x0800`)
= our `ModId`; owner-scoped teardown (`Vacate`/`ReleaseAll` by owner) = our `UndoOwner`, but
scoped to managed resources only.

**Deltas (different goals: they extend a frozen binary for VETTED authors; we govern
arbitrary mods on a soon-recompilable tree):**

1. **Mutation: NO journal.** `util/memory.zig` is raw `VirtualProtect`+`memcpy`, no record /
   owner tag / overlap check (TODOs even want a faster `write_unsafe`). Plugins poke memory
   directly, unmanaged. Clean-disable unsolved for writes (only for managed handles).
   Corruption prevention = SHA-512 **whitelist** baked at build (`plugin_hashes`), non-listed
   DLLs rejected unless Developer build. SOCIAL safety. We chose MECHANICAL safety (journal +
   registry) because we can't assume central vetting and we have an exclusive-replacement case
   (renderer) they don't. This delta IS #153.
2. **Conflict: append-all.** `PluginFnCallback` calls every plugin's callback in load order,
   `void` return - no priority, no ordering, no cancel, no managed replace (WoW
   `hooksecurefunc` model). Cannot have a "two hooks, one slot" bug because nothing can
   preempt anything; the cost is no ordering/cancel/replace expressiveness. They get away with
   it by pushing all real override into unmanaged memory writes. Our override spectrum is
   strictly more expressive at the cost of machinery they skip.
3. **Payload: digested shared state, not args.** Events are bare notifications; data channel is
   `GlobalState` (read) + `GlobalFunction` (do); no arg payload, no `skip_original`. To change
   a call's args you write memory raw.

**Borrow (3) + we're ahead (1):**
- **BORROW: versioned digested-state read snapshot.** Add an annodue-style host-filled,
  VERSIONED `GameState` struct as the low-friction READ surface (implements our "reads are
  free/encouraged"; keeps most mods off raw addresses + per-fn signatures). Keep our
  typed/tagged arg-mutation events for the cases that must rewrite a specific call. Do BOTH.
- **BORROW: enforced ABI versioning** on the shared struct(s) + capability vtable; reject
  version mismatch at load (their `PluginCompatibilityVersion` gate). Fold into Phase 6.
- **BORROW (confirmation): request -> Handle -> release-by-owner** resource pattern; scoping it
  tightly to scarce slots (not every hook) is fine.
- **AHEAD OF THEM: journal-backed disable/hot-reload.** Annodue hot-reloads whole plugin DLLs
  (FILETIME watch + copy-to-tmp + reload) but CANNOT cleanly revert a reloaded plugin's memory
  writes (no journal). Our `UndoOwner` makes unload/hot-reload actually safe - adopt their
  hot-reload ergonomics on top of our journal at Phase 6.
- **Dep seed:** their `OnPluginInitA/LateA/DeinitA(owner)` inter-plugin notifications are the
  start of mod-deps (explicitly incomplete in their code). Prefer a declarative manifest
  `deps` list driving load order over reactive notifications. Future, not MVP.

## Migration check: existing deltas -> this API (2026-06-28)

Validation by design-by-migration: can the API express the mods we already shipped? Mapped
every `dinput_hook` delta against the four mechanisms. Result: ALL fit
{observer, replacement, journal-patch, provider} - the model is complete against our codebase
- but migration forced five refinements (below) and revealed the real cost is fusion, not fit.

Two facts about the delta layer TODAY that shape this:
- **No journal exists, and most mods don't need one.** Toggle state = one global `imgui_state`
  bool struct. Dominant pattern is "always-hooked, conditionally-active": `enable_weather`,
  `HD_replacement`, `show_pod_names`, `enable_fog`, `cache_meshes` are runtime branches inside
  permanently-attached wrappers. Disable = skip extra behavior, nothing to revert. Only
  `set_ai_full_lod` + the 100-lap/Window_Main raw patches mutate `.text`. Memory-patching is
  the EXCEPTION.
- **Two primitives already exist:** `hook_function` (wrap, calls original via
  `hook_call_original`) and `hook_replace` (replace, silently LAST-WINS in a `std::map`).
  All registered unconditionally at boot in `init_renderer_hooks`/`init_hooks`.

Taxonomy (existing mod -> mechanism -> friction):
- ai_full_lod (`set_ai_full_lod`), 100-lap de-index (`swrObjJdge_F2`), racer-count -> JOURNAL
  PATCH + ModModule. None - this is the prototype.
- overhead names, FPS overlay, pod-owners readout -> OBSERVER (after). Low; wants a read snapshot.
- MP vehicle-pin (`swrObjHang_F0`), speedometer demux, time/standings entries -> OBSERVER w/
  in-transit mutate. Needs the event to expose the right args.
- asset/model/texture load (`swrModel_*`), splines, MP transport (`stdComm_Send`/`sithMulti`),
  font SDF, weather GL -> EXCLUSIVE REPLACEMENT. Must re-emit events (fork B).
- renderer takeover (`swrViewport_Render_Hook` + `renderer_utils`/`std3D`/`rdMatrix`) ->
  LAYER-1 PROVIDER. Big - fused monolith.
- splitscreen, gamepad-nav, rumble, HD-pod association -> ModModule = N observers/replacements.
  Needs mod-instance state.
- fixed-timestep (PR #194) -> EXCLUSIVE REPLACE + re-emit ticks. before/after can't express it.
- alt-tab fix, HID-enum fix, window shutdown, crash logger -> LAYER-1 CORE (stay; not mods).

### Audit: double-`hook_replace`? CLEAN (2026-06-28)

All 15 `hook_replace` targets are DISTINCT - "reject second replacement loudly" breaks nothing
today. One same-target multi-mechanism case: `Window_Main` is Detour-hooked at its prologue AND
raw-`memcpy`-patched in its body (`0x0049cede`, `0x0049cd60` in `main.cpp`) - same mod,
intentional, but a live test case for the journal overlap checker + the stolen-prologue pitfall.
100-lap de-indexes INSIDE the `swrObjJdge_F2` replacement (not a separate patch over the bypassed
original), so no double-touch.

### Five refinements forced by migration (folded into phases below)

1. **disable != journal-revert for the common case.** Most mods are runtime-gated
   observers/replacements; disable = "stop calling this observer", `UndoOwner` is a no-op
   (no journal entries). Journal is the EXCEPTION path (memory-patch mods only). [Phase 2]
2. **Mod-instance state missing from `ModModule`.** Multi-hook mods (splitscreen) share
   per-mod state across their own hooks; that is instance state, distinct from per-call
   `HookContext.state`. Add a `void* user` threaded to all the mod's callbacks. [Phase 2]
3. **Renderer provider needs DOMAIN events, not function before/after.** Half the feature set
   lives INSIDE the replaced renderer and branches on node/material/model_id; generic "wrap
   `swrViewport_Render`" is useless to them. Phase 5 must publish a draw-event vocabulary
   (`OnNodeDraw(node, material, model_id)`, `OnFrameSetup`, `OnResolve`), not just `OnRaceLoad`.
4. **Structural / "middle" mods are real - fixed-timestep proves it.** It decomposes
   `swrMain_RunFrame` and sub-steps the world sim (a loop the original lacks). Only expressible
   via exclusive-Replace where the replacer re-implements the loop and RE-EMITS `OnPhysicsTick`
   N times. So fork B is load-bearing, and "middle" can't be permanently deferred - our own
   flagship mod needs replace-and-re-emit or a named mid-function injection point. [Phase 3/5]
5. **Mod->mod dependencies exist NOW (>=4 edges):** respawn-blue-flash -> HD-pods,
   speedometer -> splitscreen, overhead-names -> renderer(projection), all-visual ->
   renderer-provider. "This mod requires provider X" must be expressible early, not pure
   Phase 6, or half our features can't declare why they need `RENDERER_REPLACEMENT=ON`. [Phase 2/3]

### The real cost is FUSION, not fit

Cheap migrations (ai_full_lod -> proves journal; settings -> proves config; observers like
overhead-names/FPS) are nearly free and validate the phase order. Expensive part: the renderer
features are fused into a `renderer_utils.cpp`/`renderer_hook.cpp` monolith + one `imgui_state`
global (same shape as the debug-UI-monolith problem). The API doesn't fight them; extracting
provider + subscriber mods is a real refactor gated on refinement 3. Recommended migration
order = cheap journal/config/observer wins now -> defer renderer un-fusing to last (after the
provider event vocabulary exists). Matches the existing phase order - a good sign.

## End-game architecture (2026-11-20, from the PR #437 discussion)

Worked out while deciding what to do with PR #437 (SoupPotato's pod editor). Sibling docs:
REFINEMENT_LAYER_ROADMAP.md (the layer this one sits on), MODDING_ARCHITECTURE.md (content
packs), POD_MODDING_ROADMAP.md (pods).

### The stack

```
  content packs (data)    |  plugins (code: 1st-party bundled + 3rd-party opt-in)
  ------------------------+------------------------------------------------------
        mod host: deps, conflicts, sim gate, journal        (this doc, Phases 1-3)
  -------------------------------------------------------------------------------
        curated seams: events / services / providers   (versioned, stable; Phase 5)
  -------------------------------------------------------------------------------
        refinement layer   (what players run; free to refactor behind the seams;
                            fix tier toggles)
  -------------------------------------------------------------------------------
        faithful build     (oracle for equivalence + verified-run baseline; not a
                            mode players pick day to day)
```

- **Seams, not every function.** Plugins hook named events (`OnPhysicsTick`, `OnMeshDraw`, ...),
  call services (read snapshot, panels, asset keys) and swap providers (renderer, audio, input).
  Hooking every refined function would freeze the refinement layer's internals as public API,
  which defeats the point of that layer. A raw per-function before/after hook stays as an escape
  hatch, flagged unstable, no compatibility promise (the Forge-events + Harmony split).
- **Refinement defines and emits the seams; the mod host governs who gets them; mods subscribe.**
  Refinement never knows a specific mod exists (no ImGui, no mod names, no includes from
  `dinput_hook/`).
- **Data is its own axis.** Most mods will be content packs, not code. They sit beside plugins,
  share the manifest format and the same governance flags.
- **First-party features become first-party plugins** (fixed timestep, HUD clusters, HD
  renderer, ...): bundled, default-on, same format as third-party. Dogfooding the API is the test
  that it is good enough.

### Where the modding layer ends and refinement begins

One line: **refinement makes the game correct, capable and hookable; modding decides what it does
with that.** Tests, in order:

1. **Is there a right answer?** Defects (crash, leak, overflow, frame-rate-dependent physics) go in
   refinement's fix tier with a vanilla toggle. Taste (HUD layout, graphics, new modes) is a mod.
   If nobody can agree on the default, it is a mod.
2. **Capacity or content?** Making a table loadable or lifting a cap (pods, 20 racers, 8 planets)
   is refinement. Filling it is modding.
3. **Mechanism or policy?** Seams, events, loaders and provider interfaces are refinement. What
   runs at the seam is modding.
4. **Can the oracle check it?** Refinement must be bit-exact, equivalent, or "toggle off passes,
   toggle on has its own test". Anything else is not refinement.
5. **Still the shipped game?** If someone who knows SWE1R would call it the shipped game, it is
   refinement; if it is recognisably "SWE1R plus something", it is a mod.

Gray zones, decided:

| Thing | Refinement | Modding |
|---|---|---|
| Fixed timestep (#337) | Separating sim tick from render frame, emitting `OnPhysicsTick`; fixed rate as a fix-tier toggle | Render smoothing / interpolation (cosmetic) |
| UI resolution independence (#364) | The anchoring mechanism (identical at 4:3) | Cluster layouts, `ui_layout.txt` overrides, HUD sway |
| Splitscreen | P2 speedometer / P2 position bugs (fix tier) | Splitscreen HUD clusters |
| Pods (#437) | Pod tables loadable and inflatable, stable asset identity, socket contract, stock part drives as data (REFINEMENT_LAYER_ROADMAP §7.1) | Pod packages, the pod editor, new drive signals |
| Multiplayer | The `stdComm` seam, lockstep-ready sim | Replacement transport (provider), player names above pods |
| Localization | Language and asset loader | Translations (content packs) |
| Crash fixes | Fix tier, arguably default-on (no speedrun tech depends on a crash) | - |
| HD renderer | The renderer provider interface | The HD renderer itself |

Warning signs: a refinement PR that needs a settings panel, an asset folder, or a toggle with no
agreed default is a mod in disguise. A mod that has to reach past the seams into refinement
internals means a seam is missing; add it in refinement.

### Content on top of refinement

New pods, new tracks and asset replacement are content packs on top of refinement, but the
ability to load them is refinement:

| | Refinement provides | Content pack provides |
|---|---|---|
| Replacing assets | One asset resolver seam: every model/texture/sprite/sound lookup by id goes through it; stock behaviour with no packs (equivalent tier) | Overrides keyed by stable asset id |
| New pods | Inflatable `swrRacerData` / `PodHandlingData` / `swrRacer_PodVisualData`, slug-allocated ids, roster UI past 23, socket contract | Pod package: model, sockets, handling, pilot, camera, drives |
| New tracks | Track registry, spline/trigger loading, 8-planet cap lifted, circuits, times by slug | Track package: geometry, spline, triggers, metadata, music |

- Data can only use behaviours the game already has (stock trigger types, stock part behaviours,
  handling values). New behaviour is a plugin; packs may depend on plugins.
- Sacred code with modded data is fine: physics stays bit-exact, equivalence is defined over stock
  data. A pack carrying handling stats is therefore **sim-affecting**; a visual-only pack is
  cosmetic.
- Today's content band-aids (`custom_tracks.cpp`, `model_replacement.cpp`, texture-buffer `.text`
  re-pointing, roster operand patches) do not have to wait: route them through one resolver seam
  in the hook layer now, and move the seam into `refined/` when that subsystem is refined.

### Player control: contexts and resolution

The player picks mods in a mod manager (toggles, saved profiles). The mod host resolves every
change with these rules, in order:

1. **Context gate.** Each mod declares `sim_affecting`. The play context decides what is allowed:

   | Context | Sim-affecting mods | Cosmetic mods |
   |---|---|---|
   | Solo / local | Player's choice | Player's choice |
   | Online lobby | Locked to the lobby host's set (lockstep, #285) | Player's choice, never in the handshake |
   | Verified run | Certified only; the rest are disabled | Player's choice |

   A mod that would pull in a disallowed sim-affecting dependency is refused.
2. **Dependency closure.** Enabling a mod enables everything it requires; disabling a mod
   disables everything that requires it (`ModModule.depends_on`, Phase 2).
3. **Seam conflicts.** Observers and mutators compose in priority order. A replacement or provider
   holds its seam exclusively; a second claimant forces an explicit swap or cancel in the UI. The
   host never picks a winner silently (Phase 3).
4. **Clean undo.** Disabling a mod reverts its journaled writes (Phase 1) and unregisters its
   handlers, so toggling mid-session needs no restart (except "requires-reload" mods, see
   Pitfalls).

Gaps against the phases as written: Phase 2's `ModModule` needs a `sim_affecting` flag and a
`certified` notion; Phase 3 needs exclusive-seam claims declared up front (in the manifest, so the
host can check conflicts without loading code); the per-context gate is new.

### Worked example: PR #437 pod editor

| What #437 does | Mechanism it really needs | Phase |
|---|---|---|
| "Mods > Pod Editor" ImGui panel | Service `register_panel` | 5/6 (in-tree `debug_ui_register` today) |
| `pod_edits_export_mesh` called from the renderer | Domain event `OnMeshDraw` {racer, part, mesh, matrix, verts, uv, texture} | 5 (needs 3) |
| `note_model` / `drop_models_from` in the model loader | `OnModelLoaded` / `OnAssetRangeReleased` | 5 |
| `pod_edits_animations_frame(in_race)` | `OnFrame` | 5 |
| `find_asset_offset`, `engineXf`, visual-data rows | Versioned read snapshot (annodue idea) | 5 |
| `glGetTexImage` texture readback | Renderer provider service `read_texture` | 5 |
| Desktop workspace in the replacement search path | Content pack under `mods/`, host hot reload | content system |
| Keeping baked spinning parts animated | Pod part behaviour / drive rows in the pod package | refinement + content |
| Rewriting `out_modelblock.bin` | Not a plugin capability; offline packaging tool at most | - |

About half the engine-side C++ in #437 exists only because the platform is missing. Ideal end
state: a content pack (the edited pod), a ~150-line plugin (export button + mesh capture against
the C ABI), and the editor as an external tool in its own repo whose contract is the export JSON
in and the pod package out.

**Interim policy until Phases 3/5/6 land: plugin-shaped in-tree code.** Feature code sits behind
one narrow header, registers as a `ModModule`, and touches the renderer/loader only at one or two
named call sites (future event sources). Extracting it into a DLL later is a mechanical move, and
each such feature tells us which events the API needs. Large standalone tools (like the #437
editor UI) live in their own repos and are linked, not vendored.

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
- **STATUS: MERGED as PR #209 (2026-06-29, e761d55).** Tim's two review asks both addressed:
  (1) undo must clear journal entries so apply/undo is a fully inverse op -> redesigned
  `WriteMemory` into a pure SNAPSHOT-ON-WRITE STACK (each write snapshots current bytes +
  pushes; `UndoOwner` unwinds newest-first + erases). Dropped the reapply / exact-match /
  partial-overlap special-casing; cross-owner range-overlap refusal kept as the corruption
  guard. (2) corrected a misleading `owner` field comment. **Design consequence:** the journal
  models ONE-SHOT stock->modded patches only - re-writing the same range repeatedly (update
  in place) is unsupported (that WAS the removed reapply path).
- **FOLLOW-UP MERGED: raw-patch migration (PR #216, 2026-06-29).** Routed
  `swrObjJdge_PatchLapTimeOverflow` (owner `lap_time_overflow`) + `swrObjJdge_PatchRaceTimeCap`
  (owner `race_time_cap`) through `WriteMemory` (kept verify-stock-then-write; one-shot startup
  patches, no behavior change). DEFERRED (do NOT fit the snapshot stack - repeated write of a
  moving value):  `swrModel_InitializeTextureBuffer_delta` (re-points 5 `.text` immediates to a
  `realloc`'d buffer every track load) + `custom_tracks.cpp::patch_occurrence` (repeated
  whole-`.text` scan-replace). Both left raw; revisit only with a tracked-binding/update-in-place
  primitive (the path #209 review removed).

## Phase 2 - Mod module + registry (governance MVP)

- **Goal:** make a feature a unit you can enable/disable, Blender-add-on style.
- **API:**
  ```c
  typedef struct {
      const char* name;
      const char* version;
      const char* const* requires;  // provider/mod deps, e.g. {"renderer", NULL} (refinement 5)
      void* user;                   // mod-INSTANCE state, threaded to all this mod's callbacks
                                    //   (splitscreen per-player snapshots) - distinct from
                                    //   per-call HookContext.state (refinement 2)
      void (*enable)(ModId self, void* user);   // registers observers/replacements + journal writes, tagged with self
      void (*on_disable)(void* user);           // OPTIONAL: non-journal teardown only (GL/textures)
  } ModModule;
  ModId register_mod(const ModModule*);
  void  enable_mod(ModId);
  void  disable_mod(ModId);         // unregister this owner's observers/replacements, then
                                    //   UndoOwner(self) for any journal entries, then on_disable()
  ```
- **disable is mostly NOT a journal revert (refinement 1).** Migration shows most of our mods
  are runtime-gated observers/replacements - disabling them just stops the multiplexer calling
  them; `UndoOwner` is a no-op because they hold no journal entries. The journal-revert path is
  the EXCEPTION, used only by memory-patch mods (`ai_full_lod`, 100-lap). Don't over-build
  journal coverage for mods that never mutate memory.
- **Key win over annodue/Blender (for the patch mods that DO use it):** Phase 1 captured the
  originals, so `disable_mod` auto-reverts via the journal - the author writes only `enable()`,
  no hand-mirrored unregister to get wrong. `on_disable` is reserved for genuinely un-journalable
  resources (GL textures, file handles).
- **Dependencies (refinement 5):** `requires` is checked at enable time - a mod naming a missing
  or disabled provider fails to enable, loudly. This is the early, minimal dep mechanism (NOT
  deferred to Phase 6): at minimum `requires {"renderer"}` lets the visual mods declare why they
  need `RENDERER_REPLACEMENT=ON` instead of silently misbehaving when it is off.
- **Deliverable:** register the existing feature deltas as mods - `ai_full_lod`, 100-lap,
  splitscreen, HD pods, overhead racer names. `hook_generated`'s always-on hooks stay the
  "core"; only opt-in features get a `ModId` and route through the registry.
- **Lift:** medium.
- **STATUS: MERGED as PR #305 (2026-09-16), boot-verified.** `dinput_hook/mod_registry.{h,cpp}`: `ModModule {name, version, depends_on, user, enable,
  on_disable}` + `register_mod / find_mod / mod_info / mod_count / mod_owner / enable_mod /
  disable_mod / set_mod_enabled / mod_enabled`. `name` IS the journal owner (`mod_owner(self)`), so
  `disable_mod` = `UndoOwner(name)` -> `on_disable()`. `enable()` returns bool; a false return makes
  the registry `UndoOwner` whatever was journaled so far (failed enable leaves stock). Field is
  `depends_on`, not `requires` - `requires` is a C++20 keyword. Dependency check is against
  registered+enabled mods only; no separate "provider" concept yet (no in-tree consumer - the
  renderer-fused visual mods are the first candidates, left for Phase 5 un-fusing).
  Migrated: `ai_full_lod` (main.cpp, live), `hd_font` (swrModel_delta.cpp, live, enable can fail on
  missing assets), `lap_time_overflow` + `race_time_cap` (swrObjJdge_delta.cpp, startup-scoped:
  their paired F2/time-formatter replacements stay attached -> Phase 3 brings those under the
  same ModId; not exposed as runtime toggles). Skipped `asset_buffer_size` (a sized parameter,
  not a toggle). Checkboxes now mirror the registry's resulting state
  (`imgui_state.x = set_mod_enabled(...)`). Read-only dev "Mods" panel (Debug category) lists
  name/version/state + depends_on tooltip. Registration is explicit at each feature's wiring point
  (LoadIconHook / init_renderer_hooks / the existing Patch* functions), NOT static-init.
  hook.log shows `[mods] enabled '<name>' 1.0` after each mod's `[patch]` lines.
  Unplaytested: the live F5 toggle path through the registry (same journal calls as before).

## Phase 2b - Per-feature file split: `hook_<feature>()` + `panel_<feature>()`  [tim's churn ask]

- **Goal:** stop `renderer_hook.cpp` / `imgui_utils.cpp` being the merge-conflict hubs. Tim's
  words: keeping every `hook_function` in one place "is not really practical and not that
  useful"; he wants "a tree of functions that would call the hook_X related to the feature", and
  "same for ui_X for imgui menus", and notes this "would still benefit from the modding api
  checks" (= Phase 3's conflict detection).
- **Today:** `init_renderer_hooks` is ONE function with ~146 `hook_function` calls
  (renderer_hook.cpp, 2.6k lines). The debug-UI side has the MECHANISM (panel registry, PR #184)
  but not the migration: 17 of 19 panel bodies still live in imgui_utils.cpp (2.5k lines); only
  the two camera panels register from their own files. Conflict matrix (2026-07-19): these two
  files are the hubs for ~25 open PRs.
- **Shape (pure move, zero behavior change):** each feature's delta file owns
  `void hook_<feature>()` (its `hook_function`/`hook_replace` calls, in today's order) and its
  `panel_<feature>()` + `DebugPanel` + `debug_ui_register` call. `init_renderer_hooks` shrinks to
  a short tree: `hook_weather(); hook_hundred_lap(); hook_splitscreen(); ...`;
  `register_builtin_debug_panels()` likewise. Where a feature is already a `ModModule` (Phase 2),
  all three live side by side in its file: module, hooks, panel.
- **Why it is a modding-API phase, not a detour:** Phase 3 makes the host own one detour per
  target and multiplex observers/replacements - that needs each feature to register from ONE
  identifiable site, which is exactly `hook_<feature>()`. Split first as a pure move so the
  Phase 3 diff is behavior, not relocation. Eventually `hook_<feature>()` becomes the body of that
  feature's `ModModule.enable` (Phase 3 lifts the "all hooks before init_hooks()" constraint).
- **Rollout (the open PR queue is the constraint - a big-bang split conflicts with everything):**
  1. Adopt the convention NOW: every PR that rebases moves only ITS OWN hook calls + panel body
     into its feature file. Monolith shrinks per merge; no PR conflicts with the split itself.
  2. One mechanical "move the untouched remainder" PR once the queue drains (whatever no open PR
     touches). Reviewable as a pure move (`git diff --color-moved`).
  3. Ordering caveat: `init_renderer_hooks` interleaves `hook_function` with startup patches
     (`swrObjJdge_PatchLapTimeOverflow`, `swrAssetBuffer_PatchSize` "must land before
     swrScene_Startup") - keep the call order of the tree identical to today's flat order.
- **Lift:** small per PR under (1); medium for (2). No new API.
- **STATUS: not started (added 2026-09-06).** First reply to tim on #153 pending.

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
- **STATUS: parser MERGED as PR #217 (2026-09-06).** Every settings key in the delta layer now goes through it; the only remaining Win32 INI calls are in #182, for the standalone shareable font-profile files the parser does not cover. `dinput_hook/config.{h,cpp}`
  = portable round-trip INI (preserves comments/blank-lines/key-order/unknown-keys; same
  `SW_RACER_RE.ini` + `[section]`/`key=value`; `;`+`#` comments; `_wfopen` unicode paths). All
  three Win32-INI consumers migrated (`imgui_utils.cpp`, `debug_ui.cpp`, `swrMultiplayer_delta.cpp`);
  no `GetPrivateProfile*`/`WritePrivateProfile*` left in the delta layer. Auto-refresh/file-watch
  deferred (it drives `enable_mod`/`disable_mod` -> lands with Phase 2).

## Phase 5 - Named events + address abstraction (decomp-survivable public API)

- **Goal:** the Forge-bus lesson - give mods stable named hooks instead of raw addresses.
- **Approach:** define a curated event set the game fires (lifecycle candidates: `OnRaceLoad`,
  `OnRaceEnd`, `OnFrame`, `OnPodStateUpdate`, `OnMenuDraw`), implemented internally via Phase 3
  observers on the relevant functions. Mods subscribe by event name + symbol name, never `0x...`.
  This is the surface that survives the decomp; the byte-patch backend can be swapped for linked
  hooks under it without breaking mods.
- **DOMAIN events, not just function before/after (refinement 3).** Migration shows half our
  feature set lives INSIDE the replaced renderer and branches on node/material/model_id - a
  generic "wrap `swrViewport_Render` before/after" is useless to them. The renderer PROVIDER must
  publish a real draw-event vocabulary (`OnNodeDraw(node, material, model_id)`, `OnFrameSetup`,
  `OnResolve`, ...). These are domain events the provider emits, not 1:1 function wraps. This is
  the bigger, harder part of Phase 5 and the prerequisite for un-fusing the renderer monolith.
- **Borrow annodue's digested read snapshot:** alongside the events, expose a versioned,
  host-filled read-only `GameState` struct (dt/fps/race_state/player...) as the low-friction
  observe surface, so most mods never touch raw addresses or per-fn signatures. (See Prior art.)
- **Structural / mid-function points (refinement 4):** where a mod must restructure control flow
  (fixed-timestep decomposing `swrMain_RunFrame`), the path is exclusive-Replace re-emitting the
  sub-events (e.g. `OnPhysicsTick` x N). If raw before/after is ever insufficient, expose a NAMED
  mid-function injection point as just another curated event - never a raw offset from a mod.
- **Lift:** medium for lifecycle events; LARGE for the renderer domain-event vocabulary
  (incremental - add events as demand appears).

## Phase 6 - External plugins + decomp transition  [FUTURE]

- **Goal:** out-of-tree mods (today every "mod" is compiled into `dinput.dll`).
- **Approach:** annodue-style `LoadLibrary` + resolve C-ABI lifecycle exports
  (`enable`/`on_disable`/event callbacks) + a manifest (name/version/priority/deps) + a
  plugin manager. Each plugin gets a `ModId` and plays by Phases 1-5.
- **Decomp transition:** when reimpls become a recompilable source tree, retarget the
  backend (Phase 1) from byte-patches to compile-time/linked hooks while keeping the
  governance surface (Phases 2-5) intact.
- **Lift:** large; gated on real out-of-tree demand.

### Hosting and distribution (2026-11-20)

**Package = a folder with a manifest.** One format for plugins and content packs:

```
mods/
  pod-editor-export/
    mod.json
    plugin.dll          (optional: code)
    content/            (optional: data)
```

```json
{
  "slug": "pod-editor-export",
  "version": "1.2.0",
  "kind": "plugin",
  "host_api": ">=1.4 <2",
  "sim_affecting": false,
  "depends_on": { "hd-renderer": ">=2.0" },
  "observes": ["OnMeshDraw"],
  "replaces": [],
  "source": "https://github.com/SoupPotato/swr-pod-editor",
  "license": "MIT",
  "sha256": { "plugin.dll": "..." }
}
```

The manifest carries everything the resolver needs (version ranges, `sim_affecting`, seam claims),
so the host can list, conflict-check and context-gate a mod without loading its code. Content
packs use `"kind": "content"`. Install locations: `<game>/mods/`, plus a per-user
`%LOCALAPPDATA%\SW_RACER_RE\mods\` for when the game folder is not writable (the Program Files
problem #437 worked around with a Desktop folder).

**ABI.**
- C ABI only: one export `swr_plugin_init(const SwrHostApi*, ModModule*)`, a versioned host vtable
  that only ever gains fields at the end; the host refuses plugins built for a newer major
  version. Any compiler targeting 32-bit x86 works.
- Memory stays on its own side: the host never frees what a plugin allocated and vice versa;
  allocation goes through the host API, so mixed CRTs are safe.
- ImGui across the DLL boundary: plugins must use the host's `ImGuiContext` and allocators and the
  exact ImGui version, so either pin the version in the ABI or hand panels a thin drawing facade.
- Ship an SDK: headers, a template repo and a CI workflow that builds a plugin.

Sketch of the v1 surface:

```c
typedef struct SwrHostApi {
    uint32_t version;
    const SwrReadState *state;            // racers, pod frames, visual rows
    void (*register_panel)(ModModule *, const char *category, void (*draw)(void));
    void (*subscribe)(ModModule *, SwrEvent ev, SwrHandler fn);  // OnMeshDraw, OnFrame...
    bool (*asset_key)(const void *ptr, SwrAssetKey *out);        // stable mesh identity
    bool (*read_texture)(uint32_t tex, SwrImage *out);
    const char *(*mod_dir)(ModModule *);  // the mod's own writable folder
} SwrHostApi;
SWR_EXPORT bool swr_plugin_init(const SwrHostApi *api, ModModule *self);
```

**Discovery channels, phased.**

| Phase | Channel | Notes |
|---|---|---|
| A (now) | Drop a zip into `mods/` | No infrastructure; "local, unverified" warning |
| B | Git-based index repo (`swr-mod-index`) | One JSON entry per mod: source repo, tag, hashes. Added by PR, reviewed in the open, free hosting (Scoop bucket / Homebrew tap model) |
| C | In-game browser | Mod manager reads the index, downloads, checks the hash, installs; could reuse the Discord sign-in backend (#314) for author identity / ratings |

An existing hub (mod.io, Thunderstore, Nexus) can replace or mirror C; the git index stays the
source of truth for the verified tier.

**Trust: review, not a sandbox.** A native DLL has full access to the player's machine; an
in-process sandbox is not realistic.
- **First-party:** built by the main repo's CI, shipped in the release zip, same format.
- **Verified:** listed in the index, and the **index CI builds the DLL from the tagged source** and
  publishes the hash; players run binaries the project built. The index is signed (minisign or
  sigstore).
- **Unverified:** anything local; loads after a warning; never allowed in verified runs.
- Content packs move the risk into the host's parsers: model, texture and manifest loaders treat
  input as hostile (`swr_memsafety`).
- Building from source also covers the standalone / 64-bit port: prebuilt 32-bit DLLs would all
  break; source builds just get a new target. Keep the API source-compatible across
  architectures from the start, binary-compatible only within one.

**Tie-in with play contexts.**
- Profile = lockfile / modlist of `slug@version#hash`; players share a file, not instructions.
- Online lobby: the host advertises its sim-affecting set as `(slug, version, hash)`; clients may
  auto-fetch missing *verified* mods and must refuse anything else. Cosmetic mods never enter the
  handshake.
- Verified runs record the full modlist hash in the run file (run verification roadmap).
- Never silently update a sim-affecting mod; pin versions in lobbies and recorded runs. Cosmetic
  mods update freely.

**Later: a scripting tier.** Once the event API exists, WASM (or Lua) for gameplay-logic mods gives
a real sandbox and architecture-independence. Native DLLs stay for providers and hot paths. Same
manifest, `"kind": "script"`.

**Starting order.**
1. Define `mod.json` and make the mod host read it; load first-party features from it.
2. Freeze `SwrHostApi` v1 on a small surface: panels, read snapshot, two or three events.
3. Publish the SDK template; the #437 pod editor export is a good first third-party test.
4. Stand up the git index with build-from-source CI.
5. In-game browser last.

## 64-bit build implications (2026-11-26)

The standalone `swe1r` now builds and races as a 64-bit exe (branch `proto/x64`; see
REFINEMENT_LAYER_ROADMAP section 10 for how it works). Every pointer stored in game memory stays
4 bytes and all game memory sits below 2 GB; x64 physics is not bit-identical to retail.

- **The governance/backend split is now load-bearing.** The retail-hooking `dinput.dll` cannot
  follow to 64-bit (retail is a 32-bit exe; a 64-bit DLL cannot load into it). Governance (Phases
  2-5: registry, lifecycle, observer/replacement, conflict rules, events) carries over unchanged.
  The byte-patch backend (Phase 1 journal over retail `.text`) only matters on the retail-DLL
  path: in `swe1r` no feature patches retail code any more (all four byte-patch mods are
  refined), and on x64 there is no retail code to patch. Deltas still attach by symbol to our own
  bodies (`hook_function` / `hook_replace`, 250 attached on x64).
- **Plugin ABI must be architecture-neutral.** A plugin must match the exe's bitness, and raw game
  struct pointers mean different things per build (4-byte `__ptr32` slots on x64). So `SwrHostApi`
  hands plugins versioned read snapshots, handles (request -> Handle -> release-by-owner) and a
  versioned function table, never raw `swrRace*` / `swrObj*` pointers. This was the "borrow from
  annodue" idea; x64 makes it a requirement. Source-compatible across architectures,
  binary-compatible only within one (as Hosting already says).
- **Existing retail-DLL mods do not run on x64.** annodue-style plugins targeting retail addresses
  stay on the 32-bit retail path. The WASM/Lua scripting tier becomes more attractive: one binary
  for both architectures.
- **Play contexts gain an architecture field.** x86 and x64 sims can drift, so the online lobby
  handshake includes `arch` (and FP mode) alongside the sim-affecting set; mismatched clients are
  refused or put on a common build. Verified runs require the 32-bit build (the oracle build)
  unless the x64 build gets its own certification. Cosmetic mods are unaffected.
- **Manifest:** `mod.json` gets an optional `"arch": ["x86", "x64"]` for plugins that ship
  prebuilt binaries; the index CI builds each listed architecture from source.
- **Address space.** Mod content (HD textures, custom tracks) does not gain from 64-bit until the
  subsystems that hold it store native pointers (refinement work, not mod-host work).

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

1 (backend + journal) -> 2 (registry + lifecycle) -> 2b (per-feature hook_X/panel_X file split,
pure move, incremental per rebased PR) -> 3 (conflict policy) -> 4 (settings, parser can overlap
1-2) -> 5 (named events) -> 6 (external plugins, future). Phases 1 + 4 alone satisfy issue #153;
2/2b/3/5 are the "do it the way the mature scenes did" layer; 6 and the decomp retarget are the
long horizon. 2b is tim's churn ask (imgui/renderer_hook conflict hubs) and the prerequisite for
3's per-target registration sites.

## Open questions

- File-watch trigger: `ReadDirectoryChangesW` on its own thread vs an mtime poll folded
  into the existing per-frame tick (simpler, avoids a thread).
- Frame-boundary hook for applying queued toggles - reuse the same point the renderer/
  ImGui already run on?
- Do we expose `WriteMemory`/raw addresses to in-tree feature deltas during Phases 1-3
  (pragmatic) while keeping ONLY named events/symbols in the eventual external-plugin
  surface (Phase 5/6)? Recommended: yes.
- ~~Mod metadata format for Phase 6 (manifest file vs exported struct)~~ - RESOLVED 2026-11-20:
  manifest file (`mod.json`), so the host can resolve and gate without loading code. See Phase 6
  "Hosting and distribution".
- Who owns and reviews `swr-mod-index` (tim's org, a community org, or this repo)?
- Who classifies a mod as `sim_affecting`: author-declared and trusted, or checked (a sim-state
  hash with the mod on vs off over a replay corpus, once lockstep exists)?
- What does "certified" for verified runs require, and who grants it (speedrun community)?
- ImGui across the DLL boundary: pinned exact version in the ABI vs a thin panel facade.
