# core (refined) — dossier

Status: dossier only, no code (2026-11-27). Base: `refine/track_lifecycle` (proto line).

The problem in one line: every refined subsystem that becomes data-driven needs the same five pieces
(named registries, a data loader with retail defaults, pack overrides, a rule for whether the result
is still the retail game, and events), and without a shared design each one invents its own. The
swrObjTrig dossier already sketches all five for triggers; pod part drives, the track and planet
tables and AI tuning are next in line and need the same things. This dossier designs them once.

It replaces no retail function, so it has no tier table and no `// 0xADDR` slots. Everything here
is new mechanism that the refined subsystems call; with no packs installed it must be invisible
(lockstep identical with and without it).

## 1. What exists today

| Piece | Where | Shape | Limits |
|---|---|---|---|
| Lifecycle events | `refined/track_lifecycle` | Three fixed listener tables (`before_load`, `unload`, `loaded`), owner string, registration order, 32 slots | One-off; the next subsystem with events would copy it |
| Mod units | `dinput_hook/mod_registry` | `ModModule` {name, version, depends_on, enable, on_disable}; owner tag for the patch journal | Code mods only; knows nothing about data or sim impact |
| Settings | `dinput_hook/config` (PR #217) | Round-trip INI, `[section] key=value` | Flat scalars; no lists, no schema |
| Content data | `dinput_hook/track_manifest` | `track.json` read with simdjson, hand-written `get_string/get_int` helpers, content store by sha256 | Per-format ad hoc parsing; no schema reuse |
| Feature options | `racer_options`, `race_timing`, `ai_tuning` | C structs set from INI keys or the debug UI | Each feature its own keys and defaults |
| Play-context rule | MODDING_API roadmap ("Player control") | Mods *declare* `sim_affecting`; context gate per solo / lobby / verified run | Declared, not computed; a data pack cannot be classified |

## 2. Flaw inventory (design debt, not retail flaws)

| Tag | Where | Finding | Refinement |
|---|---|---|---|
| DUP (D1) | track_lifecycle, swrObjTrig plan, mod_registry | Three listener / registry mechanisms with the same shape (named owner, ordered, removable) | One event bus and one registry core |
| DUP (D2) | track_manifest, planned trigger/pod/drive files | Each data format parses JSON by hand | One schema-driven loader |
| HARDCODE (H1) | refined option structs | Defaults live in code with no data file; a modder cannot see what the retail value is | Compiled defaults dumped as the shipped default file; one source |
| GAP (G1) | play contexts | `sim_affecting` is declared by hand; a data pack that changes handling or a trigger list has no flag | Computed from what the pack touches (section 3.4) |
| GAP (G2) | lockstep / verified runs / lobby | Nothing identifies the effective sim data, so two machines with different packs look the same | Content fingerprint over everything sim-relevant |
| UNSAFE (U1) | mods + RNG | A mod effect that draws `swrUtils_Rand` shifts the game's stream silently | Two RNG streams, tagged use (section 3.5) |

## 3. Target shape

### 3.1 Registries

One generic registry of named records; every data-driven subsystem owns one or more (trigger
actions, conditions, targets, trigger lists, pod drive rows, planets, tracks, AI profiles).

```c
/* refined/core/registry.h */
typedef struct RegEntry {
    const char* name;       /* "core.break_rock_202", "<pack>.rockfall" */
    const void* record;     /* subsystem-typed, immutable after registration */
    RegOwner owner;         /* core, a pack, or a code mod (ModId) */
    RegScope scope;         /* session or track (dropped on track unload) */
    uint32_t tags;          /* REG_SIM, REG_USES_GAME_RNG, REG_LOCAL_ONLY, ... */
} RegEntry;

Registry* registry_create(const char* kind, const Schema* schema);
int registry_add(Registry*, const RegEntry*);            /* duplicate name -> rejected, logged */
int registry_override(Registry*, const RegEntry*);       /* must name an existing entry; explicit */
const RegEntry* registry_find(const Registry*, const char* name);
void registry_drop_owner(Registry*, RegOwner);           /* mod disabled / pack unloaded */
void registry_drop_scope(Registry*, RegScope);           /* track unload */
```

- **Namespaces:** `core.*` is built in; everything else is `<pack or mod slug>.*`. A pack may add
  names freely; replacing a `core.*` or another pack's entry needs `registry_override` (and an
  `"override"` key in data), so a typo never silently replaces stock behaviour.
- **Retail numeric ids** (trigger type 202, pilot 5) map to `core.*` names through a per-registry
  table; track data keeps its numeric ids, mod data uses names. A mod id range is not needed.
- **Frozen during a race.** Registration happens at boot, on pack enable, or on
  `track_on_before_load`; a change requested mid-race is queued to the next unload.
- **Provenance** (owner) on every entry is what lets `disable_mod` and pack unload remove exactly
  what they added, and what the UI shows ("rockfall, from Ando Expansion").

### 3.2 Schemas and the data loader

A record type is described once by a schema; the loader, the default-file dumper, validation and the
debug UI all read it.

```c
/* refined/core/schema.h */
typedef enum ParamKind {
    PK_INT, PK_FLOAT, PK_BOOL, PK_ENUM, PK_STRING,
    PK_NUMBER_EXPR,   /* constant | {"rand":[a,b]} | {"var":"name"} */
    PK_DURATION, PK_VEC3, PK_ASSET_REF, PK_TARGET, PK_NAME_REF, PK_LIST
} ParamKind;

typedef struct SchemaField {
    const char* key; ParamKind kind; size_t offset;
    const void* retail_default; double min, max;
    const char* const* enum_names; const struct Schema* element;
} SchemaField;
```

- **One source for defaults.** The compiled-in retail tables are the defaults; the shipped default
  data file is generated from them by the dumper (build step or `--dump-defaults`), never edited by
  hand. A test asserts `load(dump(defaults)) == defaults` byte for byte.
- **Load rules:** a missing file means compiled defaults, silently. A malformed file means compiled
  defaults plus a logged error. A bad entry (unknown key, out-of-range value, missing asset, unknown
  name reference) rejects that entry with a log line naming file, entry and key; the rest loads.
  Nothing fails at use time.
- **Parser:** simdjson behind a small C++ loader with a C interface (already vendored for the track
  manifest; builds for x86 and x64). `track_manifest` moves onto the same loader.
- **Units and coordinate spaces** are part of the field (seconds, game units, degrees), so data never
  carries retail's internal scales (ms vs s, `0.0033333334` speed factors).

### 3.3 Packs and layering

A content pack is a folder with a manifest; its data files feed registries.

| Layer | When loaded | Scope | Example |
|---|---|---|---|
| core | compiled in | session | retail trigger lists, stock drive rows |
| global packs | boot / pack enable | session | a pod pack, a trigger-action pack |
| track pack | `track_on_before_load` | track (dropped on unload) | a custom track's own trigger lists |

- Lookup order: track pack, then global packs (in load order), then core; an override must be
  declared.
- The custom-track manifest (`track.json`) gains `"data"`: a list of data files in the pack folder,
  loaded on that track's `before_load`, dropped on its unload (track_lifecycle already provides
  both events).
- Code mods register into the same registries through `mod_registry` (`RegOwner` = ModId), so
  `disable_mod` drops their entries with `registry_drop_owner`.

### 3.4 Sim impact and the content fingerprint

The play-context gate (modding roadmap) needs to know whether the effective game is still retail.
Computed, not declared:

- Each registry entry carries tags from its schema and, for actions, from the action record:
  `REG_SIM` (affects the simulation: handling, triggers that touch pods, AI), `REG_USES_GAME_RNG`,
  `REG_LOCAL_ONLY` (UI, sound, rumble; never synced).
- **Retail-comparable** = every `REG_SIM` entry that is reachable this race is a `core.*` entry with
  its retail values. A pack that only adds cosmetic entries stays comparable.
- **Content fingerprint** = hash over the effective `REG_SIM` records reachable this race (serialized
  through the schema, so it is stable across builds and architectures). It goes into:
  - the lockstep recording header (replay refuses a mismatched fingerprint instead of reporting a
    divergence),
  - the online lobby handshake (next to `arch` and FP mode),
  - verified runs (must equal the retail fingerprint, or a certified one).
- A code mod's `sim_affecting` flag stays declared (code can't be inspected), but data packs no
  longer need one.

### 3.5 Randomness

- **Game stream:** `swrUtils_Rand`. Only `core.*` entries draw from it, in retail order; that is
  what lockstep checks.
- **Mod stream:** a separate generator seeded per race from (race seed, entry name, trigger index),
  deterministic and identical on every peer that shares the fingerprint. Mod entries tagged
  `REG_USES_GAME_RNG` (explicitly asking for the game stream) make the race non-comparable.
- This keeps "a mod effect with randomness" possible without disturbing AI and physics draws.

### 3.6 Event bus

Generalizes the track_lifecycle tables; the subsystems define events, they never know who listens.

```c
/* refined/core/events.h */
typedef struct EventDef { const char* name; size_t payload_size; uint32_t tags; } EventDef;
EventId event_define(const EventDef*);                 /* at subsystem init */
int event_listen(EventId, RegOwner, event_fn fn, void* user, int priority);
void event_fire(EventId, const void* payload);        /* listeners in (priority, registration) order */
void event_drop_owner(RegOwner);
```

- `track.before_load`, `track.unload`, `track.loaded` move onto it (track_lifecycle keeps its
  current functions as thin wrappers; no caller changes). Then `trigger.fired`,
  `trigger.finished`, and later the Phase 5 curated events (`race.start`, `physics.tick`).
- **Observe only** at first. Mutating listeners (the roadmap's "mutate in transit" and "cancel")
  come with Phase 3's conflict policy; a listener that changes sim state must be registered with
  `REG_SIM`, which feeds the fingerprint.
- Payloads are plain structs of values and handles, no raw game pointers (x64 plugin ABI rule).

## 4. Consumers and what they retire

| Consumer | Uses | Retires |
|---|---|---|
| swrObjTrig (dossier) | actions / conditions / targets / trigger-list registries, schemas, track-scoped packs, mod RNG stream, `trigger.*` events | its own registry sketch (section 5.1 there) |
| track_lifecycle | event bus | its three listener tables (D1) |
| Pod part drives (roadmap 7.1) | drive-row registry keyed by pod, schema, global pod packs | per-pilot `switch` as data |
| Tracks / planets | track registry, `track.json` on the shared loader | `track_manifest`'s hand-written parsing (D2) |
| AI tuning, racer options, race timing | schemas for their option structs, defaults dumped | per-feature INI keys and hand-kept defaults (H1); stays INI-backed for player settings |
| Mod host (`mod_registry`) | `RegOwner` = ModId, drop by owner | nothing (it gains data packs) |

## 5. Oracle plan

| Step | Check |
|---|---|
| Event bus behind track_lifecycle | Lockstep 25/25 single races + `seqS1v3` / `seqS2v3` / `seqC1v3`, refined on vs off; listener order logged identical |
| Registry + schema core | Unit tests (no game): add / override / drop by owner and scope; duplicate and dangling names rejected |
| Loader + dumper | `load(dump(defaults)) == defaults` for every schema; fuzzed malformed files never crash and always fall back; lockstep identical with the default file present, absent and corrupted |
| Fingerprint | Same fingerprint on x86 and x64 and across two builds for the same data; changes when one `REG_SIM` value changes; unchanged by a cosmetic pack |
| Mod RNG stream | A mod list using `rand` replayed twice gives the same lockstep hash; the game stream's draw count per race is unchanged by it |
| track_manifest on the loader | Every manifest in the test packs parses to the same `TrackManifest` as before |

## 6. Step plan

1. **S — Event bus**, with track_lifecycle's three events moved onto it. Lockstep + sequences.
2. **M — Registry + schema core** in C, with unit tests. No consumer yet.
3. **M — Loader + default dumper** (simdjson, C interface); round-trip and fuzz tests.
4. **S — Tags, fingerprint, lockstep header field** (recording format v4).
5. **M — Pack layering:** global packs at boot, track packs on `before_load`; `track.json` `"data"`.
6. **S — Mod RNG stream.**
7. **S — First consumers:** racer options and race timing schemas (small, already refined), then
   swrObjTrig step 3 builds its registries on this.

Steps 1–4 come before swrObjTrig's runner (its step 3); 5–6 before its data-file step (step 6).

## Open questions

- C or C++ for `refined/core` (roadmap 9.2). The loader is C++ (simdjson) either way; the registry and
  bus interface stay C for the plugin ABI.
- Pack manifest: reuse `track.json`'s shape for every pack, or a general `pack.json` with `track.json`
  as one kind? (Modding roadmap's `mod.json` is the same question.)
- Hot reload of data packs between races: worth it for authors (cheap with scoped registries), or
  restart-only at first?
- Does the fingerprint cover cosmetic data too (for "same visuals" lobbies), as a second hash?
- Location (roadmap 9.1), as for the rest of `refined/`.
