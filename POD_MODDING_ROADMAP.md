# Pod Modding Roadmap

**Status:** design (2026-10-22). Living document. Owner: lightningpirate.
**Scope:** make custom pods modular and data-driven across the game hook (this repo), the Blender
add-on (`blender-swe1r` / `swe1r_import_export`) and future tools, so pods can go well beyond
"replace a stock pod's meshes" without hard-coding the pod layout in each tool.
Sibling docs: `MODDING_ARCHITECTURE.md` (content registry, slugs, the three binding techniques;
this specializes its section 5.4 / Phase 3), `HD_REPLACEMENT_ROADMAP.md` (glTF pods keyed to the
racer entity).

---

## 1. The problem: the pod layout is hard-coded in three places

| Where | What is fixed |
|---|---|
| The game | 75 Podd header slots with fixed meanings; three 23-row per-racer tables: identity `swrRacerData @ 0x4c2700`, handling `PodHandlingData @ 0x4c2bb0`, part placement `swrRacer_PodVisualData @ 0x004c7088` (stride 0x6c). A pod replacing Anakin always has Anakin's engine spacing. |
| Blender add-on | The pod template export hand-builds the node tree (`swe1r/modelblock.py`: `ENGINE_SLOTS`, the reference tree comment, inline slot numbers). Positions come from a copy of the exe table (`swe1r/pod_visual.py`). |
| HD glTF renderer | `dinput_hook/replacements.cpp` `try_replace_pod` finds parts by child index, with per-pod special cases ("Ben has 4 engines, cockpit is at index 18"). |

Each copy has to be kept in step by hand. The goal is one pod definition that all three read.

## 2. The model: sockets, parts, behaviours, drivers

- **Sockets** are points the game moves every frame: `engine_r`, `engine_l`, `cockpit`,
  `exhaust_r/l`, `binder_r/l`, `cable_cockpit_r/l`, `cable_engine_r/l`, and the ground under each
  part (shadows). These are the only things the game really needs.
- **Parts** are geometry attached to a socket, each with a **behaviour** the game already
  implements for specific slots, generalized so any part can use any of them:
  - rigid;
  - stretched between two sockets (cables, binders: `swrRace_BuildStretchedQuad 0x46f0e0`);
  - faces the camera (engine glow, node 53350);
  - stretches and scrolls with speed (jet trails: `PoddAnimateEngines 0x470ae0`);
  - flat on the ground (shadows: `swrRace_UpdateHoverPads`).
- **Drivers** (later) link game values (speed, steering, engine heat, boost, per-part damage and
  repair) to a part's transform or material: fins that turn with steering, vents that glow with
  heat, panels that fall off when damaged.

## 3. Workstreams

### 3.1 Pod contract, derived from the stock pods (add-on only, low risk) - NEXT
- A survey script reads all 25 stock pods (Podd + MAlt) and the decomp and writes
  `pod_contract.json`: slot meanings, the node chain under each part, flags, culling (slots
  2/4/14/62 reversed), scales (0.016 part space, 0.004 shadows).
- A test checks that the contract rebuilds every stock pod (same approach as the material Blend
  Mode presets survey).
- Export becomes "sockets to slots through the contract", replacing the hand-built template tree.
- Import uses the contract in reverse to put imported pods into the same part collections as
  Tools > New > Podd. That is the deferred "item 3" from blender-swe1r#59: today
  `get_imported_podd_root` tells imported from template pods by "has no part collections", so it
  has to detect imported pods from node tags instead.

### 3.2 Pod package format (start in the add-on; nothing reads it yet)
- A folder with `manifest.json` (slug, name, pilot, handling stats, socket positions, chase camera,
  part list with behaviours) plus assets. Blender is one producer of it.
- Socket positions travel with the pod, so the anchors in Blender stop being locked.
- Sits under `assets/mods/<mod>/` like other content in `MODDING_ARCHITECTURE.md`.

### 3.3 Hook: custom racers (`MODDING_ARCHITECTURE.md` Phase 3, extended)
- Inflate `swrRacerData` and `PodHandlingData` as planned, **and `swrRacer_PodVisualData`**, which
  the architecture doc's section 5.4 does not list yet. Its row also holds the chase camera
  (`chaseCamTrail` +0x4c, `chaseCamHeight` +0x50), so each pod can have its own camera.
- New pods become new racers (index >= 23) with name-based id allocation, instead of replacing a
  stock racer.
- Jinn Reeso / Cy Yunga (Podd 299/301) take Mars Guo's / Bullseye's slots
  (`swrRace_ReplaceMarsGuoWithJinnReeso 0x44b530`, `swrRace_ReplaceBullseyeWithCyYunga 0x44b5e0`,
  not yet decompiled; the Steam exe's code is encrypted, so confirm in Ghidra).

### 3.4 glTF parts per socket
- Draw a package's parts per socket from the racer entity's transforms (`engineXfR/L`,
  `cockpitXf`; `try_replace_pod_entity`), not by child index. Removes the per-pod special cases.
- PBR materials, any polygon count, no 0.016 scale or 2048-face limit.
- The `.bin` model is still exported as the fallback for players without the hook and as the
  low-detail model. AI racers draw a separate low-detail Part, so the package needs one; the
  add-on could generate it by decimation.

### 3.5 Drivers
- A small set of game signals bound to part transforms and material parameters, declared in the
  manifest. Needs the per-part damage and steering fields mapped in the decomp first.

### 3.6 Shared validator and tools
- One plain-Python schema check (no Blender) used by the Blender panel (the Pod Parts checklist
  from blender-swe1r#59 becomes one of its users), a CLI, and a future website.
- Socket-based parts make a web pod builder easy: mix one pod's engines with another's cockpit,
  recolour, export.

## 4. Order

1. 3.1 contract and survey (add-on). Removes today's hard-coding; enables import into parts.
2. 3.2 manifest written alongside the `.bin` export.
3. 3.3 hook Phase 3 with `PodVisualData`: custom spacing and new racers become real.
4. 3.4 glTF parts per socket, then 3.5 drivers.

## 5. Open questions

- **Multiplayer:** everyone in a lobby needs the same packages; hash them at lobby join.
- **Save files:** store pods by slug, not by racer index.
- **Physics:** pod-on-pod collision uses the right engine's shadow footprint (first mesh box under
  slot 62). Keep that, or give it its own manifest field?
- **Four-engine pods:** confirm how Ben Quadinaros's extra engines hang off the engine slots before
  generalizing "extra rigid parts on a socket".
