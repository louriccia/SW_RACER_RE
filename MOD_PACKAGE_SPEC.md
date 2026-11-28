# Mod Package Spec (tracks, racers, variants)

**Status:** design, agreed 2026-11-27/28. Planning only; nothing reads this yet.
**Rule:** pick the design that is simpler and still covers everything. Add a mechanism only for a
real, named gap.
**Relation to other docs:** uses the `mod.json` shape from `MODDING_API_ROADMAP.md` (Hosting and
distribution) with three new `kind`s. For content packages it replaces `depends` / `depends_on`,
library packs and cross-package references from `MODDING_ARCHITECTURE.md` §4: files shared by
hash cover all of those. Track field names are the frozen schema in `TRACK_PR_RESTACK_PLAN.md`
§4.1 (what PR #316 parses). The pod design in `POD_MODDING_ROADMAP.md` (sockets, parts, drivers)
is what goes inside a racer package.

---

## 1. The whole idea

- A package is a folder of plain files, split by scope, plus a `mod.json` that maps each file
  name to its sha256.
- Every file is stored once, by hash, in `assets/content/<first 2 hex>/<sha256>`.
- The loader resolves a name through the manifest. **A file that exists in the package folder
  wins; otherwise the manifest hash points into the store.** So people can make a package by
  hand (just put files in a folder), and an installed download needs only its `mod.json`.
- Upload: the tool hashes every file and the server stores only hashes it doesn't have yet.
  Download: the game hashes what it has and fetches only the hashes it's missing.
- Every track and racer is **race files** (hashed; they key records and multiplayer) plus **look
  files** (free to swap).
- A **variant** is a small package that targets a track or racer and replaces some of its look
  files.

The store covers shared textures and sounds, updates (a new manifest; only changed files
download), and reuse of stock assets. Variants cover weather options, HD model and texture
upgrades, pod skins and pod transmog.

## 2. Kinds and files

```
<game>/tracks/<slug>/    mod.json  track.json  collision.gltf  spline.json  look.json  model.gltf  model.bin  textures/*.png  music.ogg  sounds/*.wav  preview.png
<game>/racers/<slug>/    mod.json  racer.json  look.json  model.gltf  model.bin  [model_lod.gltf]  textures/*.png  animations.json  sounds/  portrait.png  preview.png
<game>/variants/<slug>/  mod.json  variant.json  <any look files of the target>
```

There's also a per-user fallback in `%LOCALAPPDATA%\SW_RACER_RE\` for a game folder that isn't
writable.

### Track

| File | Contains | Race |
|---|---|---|
| `track.json` | `inherit` (stock base), `planet_track_number`, AI (level, spread, script, spline variant), plus the sim fields reserved in the frozen schema | yes |
| `collision.gltf` + `.bin` | Collision meshes (`collidable`, surface flags, magnet, strict-spline), trigger nodes (id, type, settings, target node), load triggers | yes |
| `spline.json` | The racing line: control points and branch links (AI, lap progress, minimap) | yes |
| `look.json` | The frozen schema's `environment` presentation fields: planet name and holo, fog, sun, skybox, weather by lap, colours, music and ambient names, cutscene | no |
| `model.gltf` + `.bin` | Everything you see. Fog and lighting zones, materials (scroll, flip, blend, flipbook) | no |
| `textures/*.png`, `music.ogg`, `sounds/*.wav`, `preview.png` | Assets referenced from the above | no |

- A trigger that moves visible parts (doors, animations) targets a node in `model.gltf` by name.
  A replacement model must keep those names; the validator checks it.
- **Legacy layout:** a single #316 `track.json` (Mute City) stays readable. Its `environment`
  block is read as the look and its `ai` block as race data. Field names are the same in both
  layouts; they only move between files.

### Racer

| File | Contains | Race |
|---|---|---|
| `racer.json` | Handling stats (the `PodHandlingData` row), hover pads (cockpit pad, engine pad), pod-on-pod collision box | yes |
| `look.json` | Pod and pilot names, description, sockets (engines, cockpit, cable anchors, exhausts, glow scale), pre-race sweep offsets, chase camera, part behaviours (rigid, stretched, glow, trail, shadow), engine sound, voice set, portrait | no |
| `model.gltf` + `.bin` | Parts as named nodes (engines, cockpit, pilot, extras) | no |
| `model_lod.gltf` (optional) | Low-detail model for distant and AI pods; generated if missing | no |
| `animations.json` | Part drives (§5) | no |
| `textures/*.png`, `sounds/`, `portrait.png`, `preview.png` | Assets | no |

**Racer physics reads only `racer.json`.** The pod-on-pod collision box (retail: the first mesh
box under Podd slot 62) becomes a field in it, so a model never affects the race.

**Hover pads vs sockets.** In retail these are the same numbers: `swrRacer_PodVisualData`
`hoverPadOffsetA` (cockpit) and `hoverPadOffsetB` (engines, mirrored in X) feed both the physics
(`swrRace_UpdateHoverPads`) and where engines and cockpit are drawn (`swrRace_PoddAnimateEngines`,
`swrRace_AnimateDisplayPod`, the cockpit tether in `swrRace_AnimateEngineWobble`). The spec
splits them: `racer.json` keeps the pads, `look.json` keeps the sockets. The stock dump writes the
same values into both, so stock pods stay retail-equivalent. With transmog, B's engines are drawn
at B's sockets while A's pads do the physics. The rest of the visual row (cable anchors, exhaust
offset and glow, sweep offsets, chase camera) is read only by drawing and camera code.

### Variant

- `variant.json`: `target` (a track or racer slug) and a display label.
- The variant contains any subset of the target's look files. Anything it doesn't ship falls back
  to the target's file.
- **One active variant per track or racer.** The player picks it per slug and the profile stores
  the choice. Stacking (combining several) is deferred until there's a real need.
- Weather variant = a new `look.json`. HD upgrade = a new `model.gltf` plus textures. Pod skin =
  any of a racer's look files.
- **Transmog:** a pod can use another racer's look files as its variant (stats from A, look from
  B). Its variant list shows A's own variants plus every other racer. Standings can show
  "A (as B)".
- Variants never touch race files. Records ignore them. In multiplayer they're cosmetic and each
  player picks their own; a player who lacks one sees the target's default look.

**Whole-file rule:** any change to a race file starts new records; look files never do.

## 3. `mod.json`

```json
{
  "schema": 1,
  "kind": "track",
  "slug": "lightningpirate.mute-city",
  "version": "1.1",
  "name": "Mute City",
  "credits": [{"name": "lightningpirate", "role": "track"}, {"name": "someone", "role": "textures"}],
  "based_on": null,
  "sha256": {
    "track.json": "...", "collision.gltf": "...", "spline.json": "...",
    "look.json": "...", "model.gltf": "...", "textures/road.png": "...", "music.ogg": "..."
  }
}
```

- **`kind`**: `track`, `racer` or `variant`.
- **`slug`** = `<owner>.<name>`. It's a stable ID, not a credit line: the owner part doesn't change
  when ownership transfers.
- **Owner** = the account that first uploaded the package. Only the owner, and any co-maintainers
  they add on junkyard, can publish a new version. That's backend permissions, not format.
- **`credits`** = display only. Editing it doesn't change the content hash.
- **`based_on`**: optional credit link for a remix. A remix is a new slug under the remixer's
  account and stores only the files it changed.
- **Content hash** = sha256 over the sorted `name:sha256` lines of the kind's race files.
  Records, leaderboards and multiplayer matching key on `(slug, content hash)`, never on
  `version`. A variant has no content hash and never enters the multiplayer handshake.
- **Schema check:** readers accept `schema <= SUPPORTED` and ignore unknown fields. Never use
  `== 1` (the #316 trap).

## 4. glTF `extras`

Our custom data lives under one `swr` key in `extras`. The Blender add-on's "export as glTF" flow
writes it from its existing properties, splitting collision and visible geometry into the two
files. Stock Blender export drops registered props (tested), so the add-on writes them itself; a
hand-made or other-tool glTF just adds the same JSON.

| Where | `extras.swr` keys (from the add-on's property set) |
|---|---|
| collision node | `collidable`, `surface` (list of surface flag names), `magnet`, `strict_spline`, `elevation`, `load_trigger` (24 ints) |
| trigger node (collision file) | `trigger` `{id, type, settings, flags, target: "<model node name>"}` |
| visible node | `fog` `{...}`, `lighting` `{...}`, `skybox` `{...}` |
| material | `scroll` `[x, y]`, `flip`, `clip`, `blend_mode`, `flipbook` |
| racer part node | nothing needed: parts are found by node name |

The trigger `settings` fields are stored raw in the add-on today; each trigger type needs named
fields before this is final.

## 5. `animations.json` (ships first)

Animations work on stock pods with no new model, so they're the MVP.

```json
{
  "schema": 1,
  "drives": [
    {"parts": ["flap_r", "flap_l"], "signal": "steer",    "rotate": {"axis": "z", "range": [-20, 20]}},
    {"parts": ["fan_r"],            "signal": "throttle", "spin":   {"axis": "y", "rpm": [0, 900]}}
  ]
}
```

- `parts`: glTF node names. Stock pods get a slot-to-name table, so the same file can drive a
  stock model.
- `signal`: a named game value. The first set is what retail
  `swrRace_PoddAnimateSteeringParts` (0x00472a50) already uses, generalized from its per-pilot
  `switch`: 7 signals, 47 rotations. New signals (boost, heat, damage) get added by name later.
- The stock dump writes each stock pod's retail drives as an `animations.json`, so it reproduces
  retail exactly.

## 6. Stock content as packages

- The game dumps its own 25 tracks and 23 racers (plus the secret pods) as packages into the
  store, generated from the player's own install. These are the examples modders copy and what
  `inherit` and variants point at.
- Stock files hash the same on every install, so an upload that reuses a stock texture uploads
  nothing for it, and junkyard never hosts retail files.
- **Completeness test:** a stock package loaded back must race the same as retail (lockstep) and
  draw the same (texture_hash). If something can't be expressed, the format has a gap.

## 7. Hosting

- Junkyard holds the catalog (slug, versions, owner, co-maintainers, credits) and the hash store.
  Uploads go through the #314 Discord sign-in.
- A zip anyone shares installs exactly like a download: unpack, hash, move into the store, keep
  the `mod.json`.
- Moderation and takedown: backend question, outside this spec.

## 8. Where it gets built

Implement the format once in standalone `swe1r` through `refined/` (track and racer registries).
The retail DLL reads the same packages through the track interface in `TRACK_PR_RESTACK_PLAN.md`,
with its workarounds in their own files.

**Order:**
1. `animations.json` on stock pods.
2. Stock dump plus the completeness test.
3. Track packages (glTF plus extras).
4. Racer packages.
5. Variants, transmog included.
6. Junkyard upload and download.

## 9. Checked in src (2026-11-28) and still open

**Checked (standalone src; Ghidra was offline):**
1. **Fog and draw distance:** presentation. `swrObjJdge_SetupTrackEnvironment` sends fog to
   `rdModel_SetFogEnabled` and `swrRender_SetFogParameters`. `drawDistance` and
   `nearClipDistance` are read only by `swrViewport_SetCameraParameters`.
2. **Weather:** presentation, with one catch. `swrWeather_RenderParticles` spawns particles with
   `swrUtils_Rand`, the shared game RNG.
   - In vanilla variable-dt mode every render-time draw (weather, engine flames) already moves
     the race RNG frame by frame, so weather isn't a special case there.
   - With the fixed-timestep RNG split (#337, on by default), render draws use the cosmetic
     stream and weather can't touch the race.
   - **Rule:** look files may only drive code on the cosmetic RNG stream. In `refined/`,
     presentation RNG is always its own stream.
   - The SNW/NSNW region tags that switch weather on and off are collision surface tags, so they
     sit in `collision.gltf` (a race file). That's harmless; it just means moving them starts
     new records.
3. **Pod sockets:** see "Hover pads vs sockets" in §2. Engine and cockpit placement in retail
   *is* the hover-pad data, so the split relies on the stock dump writing both.

4. **Cockpit transform:** presentation. `cockpitXf` is touched only by the pod animators
   (`PoddAnimateEngines`, `AnimateEngineWobble`, `AnimateSpinoutEngines`, `ApplyPartSinkOffset`,
   `PoddAnimateVariousThings`), `swrObjTest_F3` (copies it onto part nodes) and the first-person
   camera. `engineXf` was checked on #437: outside drawing, only scrape-spark placement uses it.
   Neva Kee's "engines = cockpit" quirk in `swrObjTest_F3` becomes a look setting.

**Open:**
5. `spline.json` format: points plus links, using the swrSpline field names; not written yet.
