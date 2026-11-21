# SW_RACER_RE — Refinement Layer Roadmap

**Status:** proposal / design (2026-11-18). Living document. Owner: lightningpirate.
Companion skill: `/refine-subsystem` (`.claude/skills/refine-subsystem/SKILL.md`).

---

## 0. Why this layer exists

The project has two layers today, and a third one is missing between them:

```
 ┌──────────────────────────────────────────────┐
 │ MODDING LAYER   dinput_hook/ deltas, mods,   │  what players and modders touch
 │                 JSON content, QoL, fixes     │
 ├──────────────────────────────────────────────┤
 │ REFINEMENT LAYER   (this document)           │  the game, made nice to work with
 ├──────────────────────────────────────────────┤
 │ FAITHFUL LAYER  src/ matching reimpl         │  the game exactly as shipped
 │                 (Ghidra names, quirks kept)  │
 └──────────────────────────────────────────────┘
          retail SWEP1RCR.EXE = the oracle
```

- **The faithful layer** answers "what does the game do?" It must stay faithful: canonical Ghidra
  names, original quirks, original structure. That is its job and upstream's mission. Making it
  pretty would destroy its value as a reference.
- **The modding layer** answers "what do we want the game to do?" Today it is built on retail
  machine code: Detours hooks, `.text` operand patches, inflate-the-table rewrites, void-hook EAX
  traps. Even on top of a *complete* faithful reimpl it would still be a band-aid layer, because
  the code underneath is full of hard-coded tables, frame-rate coupling, global state and tangled
  call chains. Every mod or fix fights the same flaws again.
- **The refinement layer** is the missing middle. It is the same game, behaving the same way, with
  its code rewritten to be readable, modular, data-driven and memory-safe. Mods and fixes then
  become ordinary code against clean APIs, not patches against machine code.

The refinement layer is not a rewrite with a new design. It is the original design, cleaned up:
**every refined subsystem must be provably equivalent to retail, except where a change is
deliberate, named and behind a vanilla toggle.**

Where this layer ends and the modding layer begins (the five tests, gray-zone decisions, and the
end-game stack of seams, mod host and plugins on top): MODDING_API_ROADMAP.md, "End-game
architecture".

## 1. Goals and non-goals

**Goals**
1. **Readability:** code a newcomer can follow without Ghidra open.
2. **Modularity and moddability:** subsystems with explicit interfaces and state; mod-facing data
   lives outside the binary.
3. **Reduce, reuse, recycle:** one implementation of each idea; dead code gone; duplicated
   routines merged.
4. **Memory safety:** bounds-checked parsers and buffers, clear ownership, no unchecked reads or
   writes on untrusted data.
5. **Determinism and testability:** every subsystem can be run, replayed and diffed in isolation.
   This is what makes goals 1–4 safe to pursue.

**Non-goals**
- Performance work (unless a refactor makes it free).
- New gameplay. That belongs in the modding layer, which this layer exists to serve.
- Replacing the faithful layer. It stays; it is the readable specification.
- Moving every constant out of the binary. Externalize what modders want to change; name the rest.

## 2. The flaw catalogue

What a refinement pass looks for. Each subsystem dossier (section 6) classifies its findings with
these tags.

| Tag | Flaw | Typical refinement |
|---|---|---|
| `HARDCODE` | Magic numbers, fixed tables in code, per-track/per-pilot `if`/`switch` exceptions | Named constants (`engine_config.h`) for internals; data files for mod-facing tables; exceptions become data flags |
| `FRAMERATE` | Logic scaled by frame dt, render-rate RNG draws, per-frame decay | Move into the fixed tick; sim vs presentation split |
| `GLOBAL` | Mutable globals shared across subsystems | Subsystem state struct, passed explicitly or owned by one module |
| `TANGLE` | A subsystem reaching into another's internals; god structs (`swrRace`, `swrObj*`) touched everywhere | Interface functions; split struct by owner |
| `LIMIT` | Implicit capacity limits (20 racers, 25 tracks, fixed name buffers), platform lock-in | Named, checked capacities; growable where mods need it; platform seam |
| `UNSAFE` | Unchecked parsing, `strcpy`/`sprintf` into fixed buffers, unchecked indices, leaked resources (e.g. voice slots) | Bounds-checked readers, sized string APIs, ownership rules |
| `DEAD` | Unreachable code, N64 leftovers, unused debug paths | Delete (the faithful layer keeps it for reference) |
| `DUP` | The same routine written several times with small variations | One parameterized implementation |
| `CLEVER` | Bit tricks, overloaded fields, multi-purpose flags | Plain code; one meaning per field |
| `DECOMP` | Artifacts of decompilation, not of the original source: `CONCAT44`, `__ftol` shapes, pointer arithmetic into structs, `unkNN` fields | Real types and fields; idiomatic expressions |
| `BUG` | Genuine defects | **Tier-dependent:** see section 3 |
| `SILENT` | `HANG`, ignored return codes, silent failure | Explicit error path, logged |

Two cautions:
- **`DECOMP` ≠ `CLEVER`.** Much code that looks strange was written by the compiler or the
  decompiler, not by the original developers. It is the cheapest, safest category. Do it first.
- **Arena and pool allocators are a good pattern.** The game's per-level bump/pool allocation is
  not a "leak" problem. Keep it; add bounds checks and ownership rules.

## 3. The three tiers

Every function in a subsystem is assigned one tier **before** any refinement work starts.

| Tier | Contract | Verified by | Examples |
|---|---|---|---|
| **Sacred** | Bit-exact with retail. Same float operation order and precision, same RNG call order, same tick order. Code may be renamed, retyped and restructured only where the compiled arithmetic is unchanged. | Per-function differential harness + tick-level state hash | Pod physics, AI steering, collision response, judge/lap logic, RNG |
| **Equivalent** | Same behaviour the player can observe (output, timing, saved data). Internals may be rewritten freely. | Differential tests on outputs, test-runner plans, screenshot/audio diff where needed | UI and menus, loading, save files, audio, rendering, HUD |
| **Fix** | Deliberate behaviour change. Always has a vanilla toggle; vanilla is the default unless agreed otherwise. | The toggle-off path passes the sacred/equivalent checks; the toggle-on path has its own test | Frame-rate bugs, voice-slot leak, name overflow, crash fixes |

Rules:
- **Bugs in sacred code are features until proven otherwise.** Speedrun and TAS tech depends on
  them. A sacred bug fix is a **Fix**-tier change with a toggle, never a silent correction.
- **Floating point is the sacred tier's biggest trap.** The faithful build is x87
  (`-mfpmath=387`). Moving sacred code to SSE2 or 64-bit changes results slightly and desyncs
  vanilla replays. Decision (open, section 9): either pin sacred code to x87 permanently, or
  accept tolerance-based equivalence for a "modern" build and keep a strict build for
  verification.
- **RNG draw order is a cross-tier rule.** There is one shared LCG (`swrUtils_Rand`), and
  "cosmetic" code draws from it: camera update, engine audio, ~27 draws in the hangar (junkyard
  stock, holo-camera aim, `StartRace`). Equivalent-tier code must keep the number and order of its
  draws, or move them to a separate cosmetic RNG as a Fix-tier change (as the fixed-timestep RNG
  split did). Survey 2026-11-18.
- **A function can move tier only by an explicit, documented decision.** Never quietly.

## 4. The oracle: how "matching functionality" is checked

Without a machine check, a refinement pass is a rewrite that slowly drifts away from the real
game. Most of the machinery already exists or is planned in other roadmaps. The refinement layer
depends on it rather than inventing its own.

| Layer | What it checks | Source |
|---|---|---|
| Per-function differential harness | Retail vs ours on the same inputs, bit for bit (`original` / `reimpl` / `both` mode) | PR #285; STANDALONE_BUILD_ROADMAP §6.1 |
| Live per-subsystem flips | One subsystem flipped to ours, everything else retail, under test-runner plans | STANDALONE_BUILD_ROADMAP §6.2; test runner PR #334 |
| Tick-level lockstep | Per-tick hash of sim state against retail on the same input stream | STANDALONE_BUILD_ROADMAP §6.3; REPLAY_ROADMAP §10 |
| Replay corpus | Recorded inputs + per-tick state hashes, including speedrun/TAS routes for edge cases | REPLAY_ROADMAP |

The refinement layer adds one mode: **`refined`**. The hook-mode switch becomes
`original` / `reimpl` / `refined` / `both(a,b)`, so a refined function can be diffed against
retail directly, or against the faithful reimpl when retail is unavailable (standalone build).

**The oracle is always retail.** Not the faithful source and not anyone's memory of how the game
plays. The faithful layer is the readable explanation of the oracle; it is not the oracle itself.

## 5. Where the code lives

Proposal (to be agreed with tim before any code lands upstream):

```
src/          faithful layer — unchanged mission, upstream's canonical decomp
refined/      refinement layer — one directory per subsystem
  <subsystem>/
    README.md      the subsystem dossier (section 6)
    *.c / *.h      refined implementation + public interface
    data/          default data files for externalized tables (or under assets/)
dinput_hook/  modding layer — migrates onto refined/ APIs over time
```

- Refined code may call refined code and platform APIs. It calls faithful/retail code only
  through a named seam while its neighbours are still unrefined, and every such seam is listed in
  the dossier as debt.
- The same `// 0xADDR` slot convention ties each refined function back to the retail function(s)
  it replaces, so the hook-mode switch and the harness can find it. A refined function that
  merges several retail functions lists every address it covers.
- Refined code does not need Ghidra names. It does need a **name map** in the dossier
  (`refined name ↔ canonical Ghidra name ↔ address`) so the three layers stay cross-referenced.
- Alternative if upstream prefers it: a separate repository or fork that tracks `src/` as its
  reference. The tier rules and the oracle are the same either way.

## 6. The subsystem pass

Refinement happens one subsystem at a time. It does **not** wait for a 100% standalone build. A
subsystem is ready when it is **sealed enough**: its bodies are fully reimplemented in the
faithful layer, and the number of retail callers and callees across its boundary is small and
known.

### 6.1 Readiness signals (what `/refine-subsystem` ranks on)

| Signal | Why it matters | How to measure |
|---|---|---|
| Faithful coverage | You cannot refine what you have not read | `reimpl_plan.py`, `standalone_gap.py`: full vs stub vs header-only per header |
| Seal | Few boundary edges means few seams to maintain | Call graph (`call_edges.txt`): edges crossing the subsystem boundary |
| Oracle coverage | Refinement without a check is a rewrite | Functions in the harness; test-runner plans touching it |
| Modding pain | The point of the layer is to retire band-aids | Count of `dinput_hook/` deltas, forward `HOOK`s, `.text` patches and table inflations that target the subsystem |
| Flaw density | Payoff | Quick flaw-catalogue scan of the faithful bodies |
| Tier mix | Equivalent-tier subsystems are cheaper and safer to learn on | Rough share of sacred functions |
| Conflicts | Avoid stepping on open PRs and active work | Open upstream PRs touching its files |

### 6.2 Dossier (written before any refactor)

`refined/<subsystem>/README.md`:
1. **Boundary:** functions in scope (address, Ghidra name), globals owned, structs owned, seams to
   unrefined code.
2. **Tier table:** every function → sacred / equivalent / fix, with a one-line reason.
3. **Flaw inventory:** findings tagged with section-2 tags, each with an address.
4. **Target shape:** public interface, state struct, which data moves out of the binary and in
   what format (JSON per MODDING_ARCHITECTURE), name map.
5. **Band-aids retired:** which `dinput_hook/` deltas, patches and hooks become ordinary code or
   data once this lands.
6. **Oracle plan:** which harness cases, test-runner plans and replays prove equivalence; what is
   not covered yet.

### 6.3 Steps, cheapest and safest first

Each step is its own commit (or PR), and each is gated by the oracle before the next starts.

1. **`DECOMP` cleanup:** real types, real field names, idiomatic expressions. No behaviour
   change by construction.
2. **`DEAD` and `DUP`:** delete unreachable code; merge duplicates behind one function.
3. **`GLOBAL` → state:** pull the subsystem's mutable globals into a state struct with one owner.
   Retail-facing globals stay mirrored through a seam until their other readers are refined.
4. **`HARDCODE` → named constants and data:** internals to `engine_config.h`-style constants;
   mod-facing tables to data files with retail values as defaults.
5. **`TANGLE` → interface:** other subsystems go through the public interface only.
6. **`UNSAFE` and `LIMIT`:** checked parsing, sized buffers, explicit capacities.
7. **`FRAMERATE`, `BUG`, other Fix-tier changes:** last, each behind a vanilla toggle.
8. **Retire band-aids:** port the matching modding-layer deltas onto the new interface or data,
   and delete the patches.

## 7. Phases

| Phase | Content | Depends on |
|---|---|---|
| **R0 — Foundations** | Agree on the layer and its location with tim. Merge the differential harness (#285). Add the `refined` hook mode. Write the dossier template and the skill. | — |
| **R1 — Pilot subsystem** | One **equivalent-tier**, well-covered, high-pain subsystem through all of section 6.3. Learn the costs; revise this document. Pilot chosen 2026-11-18: **swrControl** (dossier `refined/swrControl/README.md`). | R0 + **faithful gate** (below) |
| **R2 — Data-driven content** | Subsystems that own the mod-facing tables (tracks, planets, pilots/pods, circuits, AI tuning) so MODDING_ARCHITECTURE's inflate-the-table and hook-the-reader tricks become plain data loading. | R1 |
| **R3 — Sacred core** | Physics, AI, collision, judge — only with tick-level lockstep (§4) running. `DECOMP`, `GLOBAL` and `TANGLE` steps only at first. | Lockstep, replay corpus |
| **R4 — Platform** | Platform seam (render, audio, input, network, file I/O) so 64-bit and non-Windows builds are possible. FP decision (§3) applies here. | Standalone build |

R1–R2 can run in parallel across subsystems once the pilot has proved the process, one writer
per set of files.

**Faithful gate (per subsystem).** Refinement does not wait for a full standalone build: retail is
present, and the hook-mode switch can diff refined against retail while everything else stays
retail. It does wait for the subsystem's *faithful* bodies to be verified live, because almost no
faithful reimpl has ever executed, and refining an unverified body bakes its transcription bugs
into nicer-looking code. Before any refined code for a subsystem:
1. #285 (hook-mode switch + differential harness) is merged.
2. The subsystem's faithful bodies run in `reimpl` mode, with everything else `original`, and
   pass its harness cases and test-runner plans (STANDALONE_BUILD_ROADMAP §6.2).

Dossiers can be written before the gate. So the first work session on a subsystem verifies the
faithful layer; refinement starts in the next one.

### 7.1 Queued candidates

Noted while designing, not started. Each still goes through the readiness ranking (§6.1) and gets
a dossier before work.

| Candidate | Phase | Why | Notes for the dossier |
|---|---|---|---|
| **Pod part drives:** `swrRace_PoddAnimateSteeringParts` 0x00472A50 (2026-11-20) | R2 (pods), after the R1 pilot | A per-pilot `switch` that is really a table of (part slot, signal, axis, scale) rows. Keyed by pilot and raw slot, so a custom pod inherits the replaced pilot's animations; nothing can add a moving part or a new signal. Small, deterministic, easy to oracle. | Target: keep the 7 signals (`flapPitch`, `vaneYaw`, `yawR/L`, `pitchR/L`, `spin`) computed as today, then loop over the pod's drive rows in original order; stock rows extracted 1:1. Later the key moves from pilot to pod and parts by contract name (POD_MODDING_ROADMAP §3.5). Keep: pilot 5 writes `0x16` and `0x24` twice (last wins); pilot 0's `pitchR`/`pitchL` pairing looks asymmetric but is stock; all 6 smoothing calls, `partOffset[33].y` spin state and the `partNodes[0x3b]` flag write run for every pilot before the LOD return. Tier: equivalent if nothing sacred reads the part-node transforms or `partOffset` state; confirm. Oracle: 23 pilots x steer/speed/LOD sweep over several frames, diff every part transform and smoothing state. `swrRace_SmoothEngineExhaustSize` is a generic spring, rename it. |

## 8. Risks

| Risk | Mitigation |
|---|---|
| Drift: refined code quietly stops matching | The tier contract plus a machine oracle on every step; no step lands unverified |
| Scope creep into a redesign | The non-goals; new behaviour goes in the modding layer or behind a Fix toggle |
| Two codebases diverge (faithful vs refined) | Name map and `// 0xADDR` slots in every refined file; the faithful layer is frozen per function once verified |
| Upstream mission mismatch | Agree on location and scope first (R0); fall back to a separate tree if needed |
| Sacred FP drift on new toolchains | Pin x87 for sacred code until the FP decision is made |
| Speedrun community trust | Sacred tier + vanilla toggles + run-verification roadmap |

## 9. Open questions

1. **Location:** `refined/` in upstream, or a separate repo/fork? (Ask tim.)
2. **Language:** C only, or C++ allowed in `refined/` (the modding layer already uses C++)?
3. **FP policy:** x87 forever for sacred code, or a tolerance-based modern build plus a strict
   verification build?
4. **Data format:** JSON for every externalized table (consistent with MODDING_ARCHITECTURE), or
   a binary format for large ones?
5. **Pilot subsystem:** pick with `/refine-subsystem` once R0 is done.
