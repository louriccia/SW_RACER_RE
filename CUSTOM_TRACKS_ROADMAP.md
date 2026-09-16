# Custom Track UX Roadmap

**Status:** design (2026-09-10, rev 2 - discrete assets). Living document. Owner: lightningpirate.
**Scope:** replace the temporary custom-track support with a discover -> download -> play ->
record -> upload loop, where **a track ships as discrete assets, never as a packed block file**.
Sibling docs: `MODDING_ARCHITECTURE.md` (the general content model this specializes),
`MODDING_API_ROADMAP.md` (issue #153 mod/patch API), `REPLAY_ROADMAP.md` + run-verification
notes (times trust).

Ecosystem this lands in (three sibling repos, already live):

| Repo | Role | Relevant surface today |
|---|---|---|
| `../junkyard` | Vite site (wiki, leaderboards, tournaments) | `src/pages/tracks`, `src/authFetch.js` (Bearer JWT) |
| `../bottos-junkyard` (**botto-api**) | Express + Firestore + Discord OAuth/JWT | `api/v1/routes/{tracks,times,leaderboards}.js`, `leaderboard-api.md` |
| `../Botto` | Discord bot | calls botto-api with `x-bot-token` |

No `mods` collection, no file upload/storage, and no HTTP client in the game exist yet. Those
are the four new pieces.

---

## 1. Where the temporary solution actually stands (RE-grounded, 2026-09-10)

`dinput_hook/custom_tracks.cpp` (569 lines) + `game_deltas/tracks_delta.c` (1563 lines) already
do the hard engine work: table inflation (`g_aNewTrackInfos[98]` + `.text` operand rewrite),
block-path redirection (`0x4B9598/90/94`), id remap into the `>= 420` range, name hook
(`swrUI_GetTrackNameFromId`), and menu enumeration. That part is keepable.

What makes it "temporary" is everything around it:

| # | Gap | Evidence |
|---|---|---|
| G1 | **Identity is a folder name.** `g_aCustomTrackNames[customID] = folder.filename()`; multi-track folders get " 1"/" 2" appended. No stable id, version, or author. | `custom_tracks.cpp` `add_track` |
| G2 | **Metadata is hardcoded.** `planetTrackNumber = 0`, `PlanetIdx = 1`, `FavoritePilot = 2` for every custom track. No AI tuning, fog, music, ambient SFX, preview model. | same `add_track` |
| G3 | **A pack IS the game's packed archive.** A track ships `out_modelblock.bin` (every model in the game), `out_splineblock.bin` (all 91 splines), `out_textureblock.bin` (all textures) - megabytes of redistributed stock content to deliver one track. **This is the core thing rev 2 fixes.** | `try_load_custom_track_folder`, `custom_tracks.cpp:382-403` |
| G4 | **Asset ids are therefore *guessed*.** Because the pack is a whole block, the loader must hash every entry, diff against stock `data/lev01` hashes, infer which entry is "the custom one", then pair model to spline heuristically. Correct today, fragile forever - and pure consequence of G3. | `compute_track_model_infos`, `compute_spline_hashes` |
| G5 | **No dedup possible.** Hashing exists only to *detect* stock entries. Two packs sharing a texture set ship two full texture blocks. | same |
| G6 | **Records are broken on custom tracks - actual OOB write.** Both the store (`swrRace.c:415`, stock code still runs under `swrRace_ResultsMenu_delta`) and the read use `idx = bMirror + track_index * 2` into `float record3LapTimes[50]` / `recordLapTimes[50]` inside the fixed `swrSaveData` (`sizeof 0xfd4`, CRC32-checked). With `track_index` up to 97 that indexes up to **195**: a good time on a custom track scribbles into `record3LapNames[]` and, at the top of the range, past the end of the save image. The *display* path is guarded (`track_index < DEFAULT_NB_TRACKS` in `tracks_delta.c:1138`); the *write* path is not. | `src/types.h:813`, `src/Swr/swrRace.c:415/431`, `src/Swr/swrUI.c:1146` |
| G7 | **Distribution is fully manual.** Download elsewhere, unzip into `assets/custom_tracks/`, restart. | `MODDING_ARCHITECTURE.md` section 10 |
| G8 | Hard caps: `MAX_CUSTOM_TRACKS 70`; custom packs are packed into synthetic "Custom Tracks Page N" circuits. The texture-buffer growth path "assumes the custom `out_textureblock.bin` only appends" - a same-slot replacement is undefined. | `tracks_delta.h:18`, `swrModel_delta.cpp:271` |

Two crash classes already burned us here and constrain the design: malformed splines fault on
the first race frame (now rejected at load with a usability check) and the pre-race track sweep
plus unseeded fly-by cursor (PRs #296/#301). **Anything downloaded must be validated before it
is ever bound.**

---

## 2. Strategy: discrete assets + one stable identity

Two primitives are missing, and G3/G4/G5 are all one of them:

```
  track.json  (metadata + a list of DISCRETE assets, each by hash)
        |
        +-- slug + version ......... names the track everywhere (menus, times, catalog, API)
        +-- content_hash ........... names THIS BUILD of the track (leaderboard integrity)
        +-- one entry per asset .... a track model, a spline, N textures, music
                                     -> dedup, and "download only what I'm missing"
```

Locked decisions:

1. **A track is a package of discrete assets, not a folder of packed archives.** One track model,
   one spline, N textures, optional audio - each its own file, each content-addressed. A pack for
   a new track contains *only what is new*: typically one model chunk, one spline chunk, and the
   handful of textures the author actually authored.
2. **A track is a package, not a folder name.** `assets/tracks/<slug>/track.json` + its asset
   references. Legacy `assets/custom_tracks/<folder>/` (whole-block packs) keeps working through
   a synthesized manifest and the existing hash-diff heuristic, so nothing in the wild breaks.
3. **Slugs, never numeric ids, cross any boundary.** `>= 420` model/spline ids, track index,
   texture slots, sprite slots are allocation details the registry hands out per install. Save
   data, the catalog, and times key on `slug`.
4. **Content-addressed blob store, shared across packages.**
   `assets/content/<sha256[0:2]>/<sha256>`; `track.json` references each asset by hash + role.
   Dedup and incremental download become the same mechanism: fetch the hashes you lack. Stock
   assets are pre-indexed by hash, so a pack that reuses a vanilla texture references it and
   ships nothing. **No zip library needed** - a package is a manifest plus N blob GETs.
5. **The block seam becomes virtual (section 3.2).** The game keeps its `[count][offset table]
   [payloads]` block model; we serve those reads from a per-load *virtual block* assembled from
   discrete assets. No temp files, no whole-archive redistribution, no "assumes append only".
6. **Declared ids beat inferred ids.** With discrete assets there is nothing to infer: the
   manifest names the model and the spline directly. The hash-diff heuristic survives only as
   the legacy-pack fallback.
7. **Custom-track times live in a sidecar**, keyed `(slug, content_hash, mirror, laps, category)`.
   The vanilla 50-slot record array is never indexed past 27 again. Categories mirror
   `leaderboard-api.md` (fulltrack / flap, upgrades vs none, skips) from day one, so the same
   record submits to the junkyard leaderboard with no schema migration.
8. **Download in-game, upload on the web (MVP).** Read-only anonymous catalog + blob GETs need no
   auth in the client; publishing needs Discord OAuth, validation, and moderation - all of which
   the junkyard already has. In-game upload is a later phase, not an MVP need.
9. **Validate twice.** Server-side on publish (port `scripts/validate_raw_model.py`), client-side
   before first bind (existing spline-usability check + chunk structure). Hash-pin everything.

### Data flow

```
  junkyard (upload page)  --JWT-->  botto-api  --> Firestore trackMods + Storage blobs/<sha256>
                                       ^  |
   in-game ImGui browser  --GET index-+  +--> GET /blobs/<sha256>  (anonymous, CDN)
        |                                                |
        +-- write missing blobs into assets/content/ ----+
        +-- write assets/tracks/<slug>/track.json
        +-- registry rescan at the hangar reload boundary -> playable
        |
   after a race: sidecar times.json  --(opt-in, JWT)-->  botto-api /times  --> leaderboards
```

---

## 3. Discrete assets: the format and how they bind

### 3.1 The chunk formats (mostly already exist)

The blocks are all the same shape - `[count][offset table][payloads]`, big-endian, read only
through `swrLoader_ReadAt` - so a discrete asset is just one entry's byte range plus enough
header to describe it.

| Asset | Discrete file | Status |
|---|---|---|
| Track model | **`RAWM`**: `magic "RAWM"`, `mask_size`, `model_size`, then the verbatim big-endian mask + model payloads (the `[mask_offset, model_offset)` and `[model_offset, next)` ranges of one modelblock entry) | **Exists** on branch `loose-model-replacement` (`model_replacement.{h,cpp}`, + `scripts/extract_raw_model.py` / `validate_raw_model.py`) |
| Spline | **`RAWS`**: magic + the one entry's bytes = big-endian `swrSpline` header (0x10) + `num_control_points * swrSplineControlPoint` (0x54) | New, trivial - the size relation is already asserted in `compute_spline_hashes` |
| Texture | **`RAWT`**: magic + one textureblock entry's bytes (material header + pixel/palette payload) | New; the entry-range extraction is the same pattern as the model extractor |
| Audio | plain `.wav` (the game's own format; > ~516 KB auto-streams) | Exists via `swrSound_RegisterSound` (`MODDING_ARCHITECTURE.md` section 9) |

**DONE 2026-09-10** (`scripts/extract_raw_asset.py`, branch `feature/discrete-track-assets`): all
three carvers plus a `verify` mode that carves and rebuilds every entry of a block and compares it
byte for byte. Every entry of every stock block round-trips unchanged (323 models, 91 splines, 1672
textures) and so does a blender-authored pack (Mute City: 323 / 91 / 1665). Remaining: the
blender-swe1r exporter should emit discrete chunks directly instead of a rebuilt block (M8).

Block layouts, confirmed by reading the files (all big-endian, all read through `swrLoader_ReadAt`):

| Block | Header | Per entry | Entry ends at |
|---|---|---|---|
| model | `[count]` | 3 words: mask_off, model_off, (next entry's mask_off) | next entry's mask_off |
| spline | `[count]` | 1 word: entry_off (plus a final terminating offset) | next entry's offset |
| texture | `[count]` | 2 words: pixels_off, **palette_off (0 = no palette)** | next entry's pixels_off |

The texture block's two-word entry was the unknown: `swrModel_LoadModelTexture(TEXID, &material,
&palette_data)` returns two things because the entry holds two payloads. 138 of the 1672 stock
textures carry `palette_off == 0` (formats needing no palette), and the last entry of a block runs
to EOF -- verified exact in both blocks (128+2 bytes stock, 65536 bytes Mute City; no padding).

Size, measured: Mute City as discrete assets is **1 model + 1 spline + 17 textures = 5.4 MB in 19
files, against 28.6 MB for the three packed blocks** -- 82% smaller before any dedup. Its 17
textures are indices 1648-1664, exactly the range the model references, and against a VANILLA
texture block (1648 entries) they are **appended past the stock count**, not rewrites of stock
entries. A pack's own content is therefore "what it rewrote" plus "what it appended" -- counting
only rewrites finds none of blender-swe1r's textures.

**G4 is worse than "fragile": the hash-diff heuristic silently invents tracks.** The install's
`data/lev01` was modded earlier in the session (texture block 1672 entries and a rewritten model
130; vanilla is 1648 / stock 130) and was then restored to vanilla. Against vanilla the nine packs
in `assets/custom_tracks/` hold **11 real tracks**; against the modded baseline the game
enumerated **19**, because every pack carries vanilla's model 130 / spline 11 verbatim and those
entries "differed" from the modded install. So the menu grew a phantom second track per pack, and
some real tracks were misnamed by it (`custommalastare 1` was the phantom; the real track was
`custommalastare 2`). Detection keyed on "differs from whatever is on disk" is only as trustworthy
as the player's install -- another argument for declared ids in a manifest. Formats re-verified
against the restored vanilla blocks: 323 models / 91 splines / 1648 textures all round-trip.

### 3.1b Pack conversion (DONE 2026-09-10)

`scripts/convert_track_pack.py` turns one of today's `assets/custom_tracks/<pack>` folders into the
MVP layout: it diffs the pack's blocks against the game's entry-wise, carves the pack's own content
into `<out>/content/<hh>/<sha256>`, and writes `<out>/tracks/<slug>/track.json` naming those assets
by hash. It reads `TrackInfo[25]` (0x004bfee8) straight out of the EXE for two things the manifest
cannot otherwise know: which spline belongs to a changed track model (rather than pairing changed
entries positionally), and the planet / track-number / favourite-pilot of the slot being overridden
-- so a converted manifest inherits real placement instead of the hardcoded `1/0/2`.

Measured over four packs (6 tracks): **103 MB of packed blocks becomes a 10.7 MB store of 129
blobs, 90% smaller**, with dedup already paying: the three tracks inside `bowsers castle` share 37
blobs, and `koopa beach` reuses 11 of `lightning`'s. Multi-track packs, packs whose spline is
unchanged (the track would race the stock line -- warned), and changed non-track entries (an
`MAlt`) are all handled and reported.

This also settles the texture contract concretely (see 3.2): a converted manifest records each
texture's **block index**, because the model keeps its absolute texture words and the virtual block
is assembled as "stock entry, unless a declared index overrides it". No model rewriting, no
ordered-list translation.

### 3.2 The binding mechanism: a virtual block provider

`swrLoader` is a three-function `FILE*` seam - `swrLoader_OpenBlock @0x42d680` (fopen by type),
`swrLoader_ReadAt @0x42d640` (fseek + fread), `swrLoader_CloseBlock @0x42d6f0` - and every block
consumer goes through it (`swrModel_LoadFromId` reads `[count]` at offset 0, then the 8-byte
offset pair at `8 * id + 4`, then the payload).

So: **detour those three and serve the active track's blocks from memory.** The registry
assembles, per race load, a virtual block per type: a synthesized header plus entries that point
at content-store buffers. Reads for an unmapped index fall through to the real stock file.

This is a generalization of what the `loose-model-replacement` branch already proves works: it
assembles a *single-entry* modelblock (`[count=1][mask_off][model_off][end]` + payloads), writes
it to a temp file, swaps `*MODELBLOCK_PATH_PTR @0x4B9598`, and remaps the id to 0. Same idea,
minus the temp file and the path-pointer swap, and applied to all three block types at once.

Why this shape:

- **Uniform.** Model, spline, and texture binding become one mechanism instead of three
  hacks (path swap + id remap + "textureblock only appends").
- **No redistribution.** The pack ships its own chunks; stock entries are served from the
  player's own `data/lev01`.
- **Kills the append assumption (G8).** Texture slots are *allocated by the registry* into the
  virtual block, so a pack can reference stock textures by hash, add its own, and never depend on
  the block layout the author happened to build against.
- **Cache invalidation gets simple.** `swrModel_InitializeTextureBuffer_delta` already clears its
  cache when the block identity changes; a virtual block's identity is the track's `content_hash`.

**The one genuinely hard part: texture indices.** A model's material references a texture as
`texture_index | 0xA000000`, an absolute index into the texture block, resolved during model load
by `swrModel_LoadModelTexture(TEXID, ...) @` its stock address (a `HANG("TODO")` stub in `src/`,
i.e. not yet reimplemented - the stock code runs). A discrete model authored against the author's
block layout carries indices that mean nothing on another install. Two options:

- **(a) Bake at install.** When a package is installed, rewrite the model chunk's texture
  references to the registry's allocated slots and record the mapping. One-time cost, zero
  runtime risk, but it mutates the asset (so hash the *original* chunk for identity, not the
  baked one).
- **(b) Remap at load.** Keep the chunk pristine and translate `texture_index` through the
  package's `textures[]` order while the virtual texture block is mapped (the model's indices
  become indices *into the manifest's texture list*, which the registry maps to real slots).

**Confirmed empirically 2026-09-10.** Mute City's track model was carved out of its packed
`out_modelblock.bin` (entry 115 = `MODELID_tatooine_mini_track`, 616 KB) and loaded as a discrete
`RAWM` over the stock Boonta Training Course via the loose-model path (PR #308). Result: **geometry
correct, textures wrong** - exactly as predicted. Its 209 texture refs are indices 1648-1664, which
resolve against the player's *stock* texture block, because the pack's own block holds only 1665
entries with its custom art at those top slots. Nothing was out of range, so there was no crash -
just the wrong art, silently. That is this section's problem in one screenshot: a model's absolute
texture indices are meaningless outside the pack they were authored against, and no amount of
model-side work fixes it. The ordered `textures[]` list plus bind-time translation is the fix, and
`RAWT` is therefore not optional for a usable MVP - it is the difference between "a track loads"
and "a track looks right".

Also confirmed: the asset buffer is not the constraint (13.6 MB free with the 616 KB model loaded),
and the spline stays stock without `RAWS`, so laps/AI/collision mismatch the new geometry.

**Resolved 2026-09-10 -- simpler than either, because the block is virtual per track.** Keep the
model byte-identical (option b's goal) but skip the translation entirely: each texture in the
manifest declares the **block index the model already references**, and the virtual texture block
is assembled as *"stock entry i, unless a declared index overrides it"*. The model's absolute
indices then resolve with no rewriting, no ordered-list mapping, and no dependence on the pack's
own block layout or entry count; appended indices past the stock count simply extend the virtual
block. `scripts/convert_track_pack.py` emits exactly this.

### 3.2b M2 DONE 2026-09-10 - the seam works end to end

`dinput_hook/virtual_block.{h,cpp}` (branch `feature/discrete-track-assets`): detours
`swrLoader_OpenBlock` / `ReadAt` / `CloseBlock`, and `virtual_block_BuildView` assembles the block
the game reads -- a synthesized `[count][offset table]` plus one region per entry, where a declared
index is served from memory and every other entry resolves to a byte range of the player's own
archive. Each block's table convention is encoded explicitly (model = 3 consecutive words, texture
= 2 with `palette_off == 0` meaning none, spline = 1, each plus a terminating slot). Region lookup
is a binary search; a full texture layout is ~1700 regions. A view records the archive it was built
from and steps aside while the block path is swapped, so it coexists with today's custom tracks.

**Playtested:** Mute City raced from **19 discrete chunks and no packed archive at all** -- model
115, spline 74, textures 1648-1664 staged under `assets/replacement_blocks/<type>/<index>.bin` --
over the stock Boonta Training Course slot. Model, spline and textures all correct. The log shows
the split cleanly: `texture block closed: 17 header, 17 memory, 0 file` for the track's own entries
and `0 memory, N file` for every other load, with `0 unmapped` throughout. The texture count the
game read was **1665** = 1648 stock + 17 appended, straight from the view's synthesized header, so
appended textures need no block rewriting at all -- which is the last piece the "wrong textures"
finding in 3.2 was missing.

**Trap that cost four playtest rounds, worth knowing before touching this seam again:**
`hook_replace` on a reverse-hooked function detours the stock address but leaves the decomp reimpl
in `src/` unpatched, so delta code calling the reimpl symbol runs the dormant body instead of stock.
`src/Swr/swrLoader.c`'s `swrLoader_OpenBlock` has the block paths **hardcoded**, while stock reads
the swapped pointers -- so installing the seam silently made
`swrModel_InitializeTextureBuffer_delta` re-open the stock archive and size the texture buffer from
it, and custom tracks rendered white. Fix: patch the reimpl symbols to jump at the deltas too, so
every caller converges on one implementation. A view has to serve delta-layer reads, not just the
game's.

### 3.2c M3 manifest reader DONE + PLAYTESTED 2026-09-10

`dinput_hook/track_manifest.{h,cpp}`: `Read` parses one `track.json` (rejecting an unknown schema
rather than guessing), `ScanAll` walks `assets/tracks/*/track.json`, `ReadAsset` resolves a blob out
of `assets/content/<hh>/<sha256>` and rejects one whose length disagrees with the manifest, and
`InstallViews` turns a track into model / spline / texture views over the player's own archives.
Every view is built before any is installed, so a track missing an asset changes nothing rather
than leaving the game reading half of it.

**No JSON parser was vendored.** simdjson 3.9.4 is already in-tree as a fastgltf dependency, already
compiled for this 32-bit target, and `dinput_hook` already links fastgltf - so the reader needed
only an include path. That avoids adding ~900 KB of nlohmann header, and a second JSON parser in
the same binary.

**Playtested:** Mute City raced from `assets/tracks/lightningpirate.mutecity/track.json` + a
129-blob content store, with the loose-chunk folder deleted so only the manifest path could work.
Model, spline and textures all correct; the manifest's placement came from `TrackInfo` (planet 0,
track 0, favourite pilot 2) rather than the hardcoded `1/0/2`.

Not done here: **hash verification** (only blob length is checked). That is fine for locally
converted files and becomes mandatory with M5's downloader, where the bytes are untrusted. And
*which* track installs is still a placeholder (first manifest that reads) - the registry owns that
decision once it knows which track is loading.

### 3.3 Manifest (`track.json`) - the contract between three repos

Freeze this first; it is the interface the game, the API, and the site all compile against. It is
a track-scoped profile of `MODDING_ARCHITECTURE.md` section 4, so a future multi-entity
`manifest.json` can embed it verbatim.

```jsonc
{
  "schema": 1,
  "slug": "lightningpirate.mute-city",     // namespaced, immutable identity
  "version": "1.2.0",
  "name": "Mute City",                     // menu name (<= 31 chars after formatting)
  "author": { "name": "lightningpirate", "discord_id": "..." },
  "description": "F-Zero tribute, 3 laps.",
  "game_compat": ">=1.0",

  // ---- discrete assets. Every entry is one game asset, content-addressed. ----
  "model":   { "sha256": "ab12...", "size": 184320, "format": "RAWM", "block_id": 115 },
  "spline":  { "sha256": "cd34...", "size": 5460,   "format": "RAWS", "block_id": 74 },
  "textures": [                             // each declares the block index the model already
    { "sha256": "ef56...", "size": 4096, "format": "RAWT", "block_index": 1648 },  // references;
    { "sha256": "11aa...", "size": 8192, "format": "RAWT", "block_index": 1649 }   // an index the
  ],                                        // pack does not declare is served from stock art
  "preview_model": { "sha256": "77bb...", "format": "RAWM" },   // optional
  "audio": {
    "music_race":  { "sha256": "0a1b...", "size": 3145728, "format": "wav" },
    "music_intro": "vanilla:mt01desert",
    "ambient": [ { "sound": "vanilla:crowd_big_loop", "start": 0.97, "end": 0.04, "mode": "loop" } ]
  },

  "placement": {                            // replaces the hardcoded 1 / 0 / 2
    "planet": "vanilla:tatooine",
    "circuit": "custom:page1",
    "track_in_circuit_order": 0,
    "planet_track_number": 0,
    "favorite_pilot": "vanilla:anakin"
  },
  "environment": { "fog_color": "#202830", "fog_near": 1000, "fog_far": 8000 },
  "ai": { "speed": 11.2, "difficulty": 38.0 },
  "rules": { "laps_default": 3, "mirror_allowed": true },

  "content_hash": "9f77..."                 // sha256 over model + spline + ordered texture
}                                           // hashes (+ audio). The leaderboard-integrity key.
```

Notes:
- `content_hash` covers the *assets*, not cosmetic metadata: renaming a track or fixing a typo in
  the description must not void its records; changing geometry, spline, or textures must.
- Reusing vanilla art needs no manifest entry at all: an index a pack does not declare is served
  from the player's own `data/lev01`. Mute City declares 17 textures and references nothing else;
  the tracks in `bowsers castle` reference 10-66 stock indices each and ship none of them.
- No `out_*block.bin` appears anywhere in a schema-1 package. Whole-block legacy packs are a
  *loader* concern (`assets/custom_tracks/`), not a manifest one.
- Suns/moons and per-planet data stay out of scope - that is planets (`MODDING_ARCHITECTURE.md`
  Phase 2, the `< 8` cap job). Custom tracks borrow a vanilla planet's sky in the MVP.

---

## 4. MVP - the smallest end-to-end loop that satisfies all four needs

Success criterion: *a track authored in Blender is exported as discrete chunks, uploaded on
junkyard, found and downloaded from the in-game browser without restarting the game, raced, and
its lap time recorded under the track's own slug and offered for submission - where the download
is a few hundred KB, not a rebuilt modelblock, and a second track reusing the same textures
downloads them zero times.*

| Slice | Deliverable | Repo | Depends on |
|---|---|---|---|
| **M0** | **Record-write fix (ship first, standalone PR).** Gate the vanilla record store/read on `track_index < DEFAULT_NB_TRACKS` (kills the G6 OOB write); custom tracks show "--:--.---" until M2. | SW_RACER_RE | - |
| **M1** | **Discrete asset pipeline.** Land `loose-model-replacement`'s `RAWM` work as the base; add `RAWS` + `RAWT` formats and extend `extract_raw_model.py` to carve all three from stock blocks; validators for each. Deliverable proof: one existing whole-block pack converted into discrete chunks by script. | SW_RACER_RE + scripts | - |
| **M2** | **Virtual block provider.** Detour `swrLoader_OpenBlock`/`ReadAt`/`CloseBlock`; assemble per-load virtual model/spline/texture blocks from content-store buffers with fall-through to stock; texture-index translation per section 3.2(b). Retires the path-pointer swaps and the temp-file trick. | SW_RACER_RE | M1 |
| **M3** | **Manifest + track registry.** Vendor a JSON parser into `dinput_hook/`; `track_registry.{h,cpp}`: seed 28 vanilla tracks with canonical slugs, scan `assets/tracks/*/track.json`, validate, allocate ids + texture slots, resolve slug refs. Feed `g_aNewTrackInfos` from the record (kills G2). Legacy whole-block folder -> synthesized manifest + old heuristic path. | SW_RACER_RE | M1, M2 |
**M4 partly done 2026-09-10:** the store exists (the converter writes it, the manifest reader
consumes it) and blobs are **verified against their own sha256** on read, through Windows CNG so no
crypto is vendored; a verified hash is remembered per session since blobs are immutable. Proven by
flipping one byte of a texture blob: the read fails and no view is installed, so a track with one
bad asset changes nothing.

**Open UX issue (Lou, 2026-09-10):** a track that fails to map currently races the stock slot it
stands in for, so a corrupt download looks like "the wrong track loaded" rather than an error. It
should bounce back to track select with a message. Applies to any bind failure, not just a hash
mismatch, so it belongs with the browser work in M5.

Garbage collection DONE (`scripts/gc_content_store.py`): reads every manifest, reports what the
store holds beyond the hashes they name, and deletes only with `--delete` (a wrong answer breaks a
track silently, and only shows up the next time somebody races it). It also reports a manifest
naming a blob that is absent and, with `--verify`, a blob whose contents do not hash to its own
name. It found a real orphan immediately -- the converter stored an entry's model before deciding
the entry was a stock re-export to skip; the decision now precedes any store write. Current store:
244 blobs referenced, all verifying, none stray.

**M4 is therefore done** for the MVP (store + verification + collection); the in-game side will
want the same collection rule once the downloader can add blobs.

| **M4** | **Content store.** `assets/content/<sha256[0:2]>/<sha256>` + a hash index; index the player's stock assets by hash so dedup can resolve against them; garbage-collect blobs no package references. | SW_RACER_RE | M3 |
| **M5** | **In-game browser + downloader.** WinHTTP client on a worker thread (link `winhttp`, no new third-party dep); ImGui "Track Browser" panel (list/search/detail/Download, progress, disk usage); fetch only missing hashes; write manifest; validate; `registry_rescan()` at the hangar reload boundary. | SW_RACER_RE | M3, M4 |
| **M6** | **Custom-track times sidecar.** `custom_times.json` keyed `(slug, content_hash, mirror, laps, category)`. Store on race end, display in course-info + results via the existing draw path, generalized off the 50-slot array. Own versioned file with a checksum - never inside `tgfd.dat`. | SW_RACER_RE | M3 |
| **M7** | **Catalog + upload + times ingest.** Firestore `trackMods` (slug, versions[], author, manifest, asset hashes, downloads, moderation state) + Firebase Storage `blobs/<sha256>` (dedup for free: existence check before write). `GET /api/v1/tracks/mods`, `GET .../mods/{slug}`, `POST .../mods` (JWT, server-side chunk validation), blob GET/HEAD. junkyard: browse page + drag-and-drop upload wizard that reads `track.json` and uploads only blobs the server lacks. Extend `times` with `trackSlug` + `trackContentHash`. | botto-api + junkyard | manifest schema frozen (section 3.3) |
| **M8** | **Authoring export.** blender-swe1r addon action: write discrete chunks + `track.json` + hashes straight out of the exporter, so no author hand-writes JSON or rebuilds a block. | blender-swe1r | M1, schema |

**M7 DONE 2026-09-15** (botto-api louriccia/botto-api#34 + #35, junkyard #35; 19 tests under
`server/api/v1/customtracks/`). Shipped shape differs from the row above in naming, not substance:
- Download, anonymous, byte-compatible with `scripts/serve_catalog.py`:
  `GET /api/v1/customtracks/index.json` (entries `{slug, name, author (string), version,
  content_hash, download_bytes, manifest}`) and `GET .../blobs/{sha256}` -> **302** to Firebase
  Storage's public URL (`blobs/<sha256>`, bucket `junkyard-430a2.firebasestorage.app`;
  `storage.rules` = anonymous read, no client writes). The mod's default `tracks.catalog_url` is
  now `https://bottosjunkyard.com/api/v1/customtracks`; WinHTTP follows the redirect on its own.
- Publish, Discord JWT: `POST /publish/negotiate` (server lists the blobs it lacks) -> `PUT
  /blobs/{sha256}` per missing blob (hash + container check on upload) -> `POST /publish`, which
  runs a server-side port of `validate_raw_model.py` plus RAWS/RAWT size checks, texture-index
  bounds (declared or `< 1648` stock), `content_hash` recomputed with the converter's recipe, and
  `rules.point_to_point` checked against the spline. 422 with the full problem list; only the
  slug's author or an admin may publish; same version twice is 409.
- Times: `POST /customtracks/times` takes `custom_times.json` schema 2 **verbatim**; identity
  `(slug, content_hash, mirror, laps, upgrades, category)`, evidence stored not flattened, only
  improvements over the player's own best kept. Boards at `GET /{slug}/times` +
  `/times/coverage`, deliberately separate from the speedrun.com-proxied leaderboards.
- Firestore `customTracks/{slug}` (author snapshot, current version, `versions[]`,
  `moderation.state` published|hidden|removed - takedown unlists, blobs stay) and
  `customTrackTimes` (one doc per record; a `ghost: null` slot is reserved). Frontend at
  `/customtracks`, `/customtracks/:slug`, `/customtracks/upload` (drop a converter output folder;
  hashes locally, uploads only what is missing), `/customtracks/times`.
- Not built: in-game upload, ratings/comments, download counts, dependency resolution, ghosts.

Sequencing: M0 ships immediately, alone. **M1 -> M2 is the new critical path** (it is what makes
packages discrete at all); M3 needs both; M4/M6 follow M3; M5 needs M3+M4. **M7 has no code
dependency on the game side** once the schema is frozen and can run fully in parallel. M8 gates
adoption, not the engine.

Effort feel (relative, not calendar): M0 S, M1 M, M2 M-L, M3 M, M4 S-M, M5 L, M6 M, M7 L, M8 M.

### MVP simplifications, deliberately
- Circuits: custom tracks still land in synthetic custom pages; `placement.circuit` is recorded
  and honored where it points at an existing circuit, but authoring *new* circuits is Phase 9.
- Legacy whole-block packs stay on the old code path forever (heuristic + path swap). They are not
  migrated in place; a converter script (M1) is offered instead.
- No dependency graph, no auto-update, no in-game upload, no ratings/comments.
- Music/ambient SFX are parsed and validated but may no-op until the audio bind lands (Phase 10);
  the tables are already reversed (`MODDING_ARCHITECTURE.md` section 9), so that is wiring, not RE.
- Anonymous downloads; time submission is opt-in and unverified (trust = Phase 12).

---

### 4.1 M5 north star: browsing in the native track select, install on first play

**Decided 2026-09-10 (Lou), after the debug-panel browser worked.** The ImGui panel is scaffolding,
not the feature. The feature is:

1. **Catalog tracks are visible in the game's own course-select pages without being installed** --
   name, author, and (eventually) a preview, straight from the catalog entry. A player never opens
   a debug overlay to find tracks.
2. **Selecting one to play is what downloads it**, once, with progress; afterwards it behaves like
   any installed track.

What that needs, on top of what M5 already has:

- **Ghost entries in the track table.** The registry already appends a manifest track and names it
  (`g_aNewTrackInfos` + `g_aCustomTrackNames`, indices never moving); a catalog track would be
  appended the same way but flagged *not installed*, with no views to map. `isTrackPlayable_delta`
  decides whether a slot can be picked, so that is the gate to open for ghosts.
- **A download gate at confirm, not at load.** `swrObjJdge_InitTrack_delta` is too late: the race
  is already starting and the game thread must not block on the network. The gate belongs at the
  course-select confirm (`swrRace_CourseInfoMenu_delta` is already detoured and already calls the
  registry), which starts the install, shows progress, and only then lets the race begin.
- **Failure has to land somewhere.** This is the same hole as the earlier bind-failure note: a
  track that cannot be installed or verified must return the player to track select with a message,
  not silently race the stock slot it stands in for. One mechanism serves both.
- **Table pressure.** Ghosts occupy slots, and a public catalog will be larger than
  `MAX_CUSTOM_TRACKS` (70). Either raise the cap (the table is already heap-allocated and
  operand-patched, so this is a constant plus headroom) or page the catalog into the table, which
  reintroduces index churn and should be avoided.
- **Preview without payload.** A catalog entry carries metadata cheaply; a *picture* needs an asset.
  The manifest already has an optional `preview_model`, so a preview can be fetched on its own,
  ahead of the rest, when a ghost is highlighted.

Worth doing in this order: ghost entries + playability gate, then the confirm-time install with
progress and the failure path, then previews. Each step is independently testable, and the debug
panel stays useful as a developer view of the same state.

**DONE + PLAYTESTED 2026-09-15 (PR #314), failure path first:**
- Bind failure: course info says the files are missing or damaged and refuses to start (accept
  plays the cancel sound); the browser turns "installed" into "Download again"; a blob that fails
  its hash is deleted on detection, which is what lets a re-download fetch it at all (the downloader
  skips hashes the store holds). Also removed a per-frame re-hash of the broken blob.
- Ghosts: the catalog is fetched at boot (`[tracks] browse_catalog`), and `track_registry_Tick()`
  - called from `DrawTracks_delta` and `swrRace_CourseInfoMenu_delta` - folds it into table
  entries parsed from the catalog's manifest text (`track_manifest_Parse`), flagged not installed.
  Track select draws them muted with "Get"; course info reads "not downloaded yet", downloads on
  accept with a progress line, and starts the race by itself when the files land (the rescan flips
  the ghost, the same frame binds its views). Lou raced Mute City from a ghost with no overlay open.
- Side effect found and fixed: two workers starting at boot made quitting stall ~1 s (joinable
  std::thread at static destruction + WinHTTP's DLL-unload cost). Measured, then fixed on both quit
  paths; the X path now ends in TerminateProcess after Main_Shutdown.
- Still open here: the 70-slot cap versus a growing catalog, and previews (the spline-generated
  model, which is also what "show a track you don't have yet" needs for a picture).

---

## 4.2 What the stock slot actually carries (why `overrides_stock_slot` cannot just be deleted)

`placement.overrides_stock_slot` looks like authoring provenance -- the slot blender-swe1r rebuilt.
It is not only that. A track that claims planet P and track-number N inherits **behaviour** that
lives in the EXE keyed on exactly those two numbers, and a custom track gets that behaviour whether
or not it wants it. Dropping the slot means externalizing each of these first. Most are already
catalogued in `MODDING_ARCHITECTURE.md` section 5; this is the list as it bears on tracks.

| What | Where it is decided | Keyed on | Status |
|---|---|---|---|
| **Weather** (particle cap, velocity, stretch, sun alpha, and the per-lap escalation) | a switch inside the judge's per-frame update, `swrObjJdge_F2` (see `src/Swr/swrObj.c`, the `jdge->planetId == 1` block) | planet + track number + **lap** | not started; the sharpest case -- it is control flow in a function body, not a table |
| **Ambient SFX** | `swrSfxPreloadSets @0x4b8fa8`, `[planet*3 + subtrack]` -> `{startProgress, endProgress, soundIdx, flags}` lists | planet + subtrack | table resolved; needs hook + wav registration |
| **In-race music** | `short[8][3] @0x4b8750` read by `swrSound_SelectTrackMusic` | planet + subtrack | table resolved; needs hook |
| **Intro / preload theme** | `short[8] @0x4b8780` read by `swrSound_PreloadSoundSet` | planet | table resolved; needs hook |
| **Fog, clear colour, camera spline** | nested switch in `swrObjJdge_SetupTrackEnvironment @0x464b90` | planet + subtrack | mapped, not bound |
| **AI speed / difficulty** | function-local `float[62]` in `InitAISettingsForTrack @0x4667e0`, index `(subtrack + planet*4)*2` | planet + subtrack | mapped, not bound |
| **Planet identity** (name, holo model, sun/moon sprites + rotation, preview sprite) | `@0xe98f5c` names, `@0xe299f4` holo, `@0xe98f40` stride 0x17, `@0xe29a18`, sprite slots 69..76 | planet index, capped at 8 | the big inflation job (roadmap Phase 11) |
| **Track preview model** | `DrawTrackPreview @0x456c70` (a `HANG` stub, so stock code runs) reads node array `DAT_00e29a88[TrackID]`, **plus a per-track scale `switch(TrackID)`** in the same function | track | mapped -- but see below |
| **Cutscenes / intro movies** | `swrObjHang_LoadScreen @0x45753d`: `Window_PlayCinematic(PTR_s_PlanetTAT_znm_004b7a08[PlanetIdx])` | planet | **RESOLVED 2026-09-10** |
| **Trigger assets** (per-planet FX and props) | `swrObjTrig_LoadAndInitializeTriggerModels @0x47ddc0`: an `if (planet_id == N)` chain, one branch per planet | planet | **RESOLVED 2026-09-10 -- it is control flow, not a table** |

Two of the slot's fields are **already out**, and are worth naming so the remaining list is not
overstated: the **track name** (a switch of `g_pTxtTrackID_00..24` in `swrUI_GetTrackNameFromId`,
already hooked -- a custom track supplies its own) and the **track favourite pilot** (a field of the
inflated `TrackInfo` entry, which the manifest fills from `placement.favorite_pilot`). Those two
were the easy shape: one hook, one table field.

### What the three open RE gaps turned out to be (Ghidra, 2026-09-10)

- **Cutscenes.** A `const char *[8]` at `0x004b7a08` indexed by `PlanetIdx`, holding
  `PlanetTAT.znm`, `PlanetA.znm` .. `PlanetF.znm`, `PlanetJ.znm` in planet order, played from
  `swrObjHang_LoadScreen`. Gated by two unnamed bitmasks -- `DAT_00e364f0` (planets whose intro is
  suppressed) and `DAT_00e35a9c` (planets already played), both of which want naming. This is the
  easy shape: a table and a hook.
- **Trigger assets.** *Not* a vtable, which is what this document previously claimed. It is an
  `if (planet_id == N)` chain across all 8 planets inside one 222-line function, each branch loading
  that planet's FX and prop models into `swrObjTrig_ModelArray2` /
  `swrObjTrig_AnimationArray` and attaching them to the trigger root node -- 15 distinct models in
  all (`fx_shards`, `fx_rockbig`, `fx_lavafoof`, `fx_methanefoof`, `flag_tip`, `balloon01`,
  `dozer`, `char_big_fish_puppet` and so on). Same shape as weather: behaviour has to become data
  before anything can serve it.
- **Track preview image.** There is none. The course-info screen draws exactly two things keyed by
  the selection -- `DrawHoloPlanet(PlanetIdx)` and `DrawTrackPreview(track_index)` -- and every
  sprite on that screen is a pilot portrait. So the per-track visual is the 3D preview alone, which
  makes the idea below cover all of it.

**The preview model may not need externalizing at all.** It is the track's shape, and the spline is
the track's shape -- so rather than shipping and binding a preview asset per track, generate the
preview from the spline's control points at load. That removes a required asset from every manifest
instead of adding a binding, and it works for a track whose author never made one. Worth trying
before the `@0xe29a88` route: the spline is already parsed (`RAWS`), the control points carry
positions, and `DrawTrackPreview` is an unimplemented stub, so there is no faithful behaviour to
preserve. It would also absorb the per-track scale `switch(TrackID)` in that function -- framing a
generated preview is a calculation, not a table of magic numbers.

The pattern is one of three shapes, and the shape decides the work: a **table** keyed by
planet/subtrack (hook the reader, serve from the manifest), a **function-local array** (same, but
the data has to be lifted out of the body first), or **control flow** (weather -- the values have to
be turned into data before anything can serve them).

Two consequences worth stating plainly:

- Until this is done, a custom track's `planet` field is not cosmetic. Setting it to 1 gives the
  track Ando Prime's blizzard, its music, its ambient sounds and its sky, because those are what
  planet 1 *means* in the EXE. Authors are choosing a behaviour bundle, not a backdrop.
- Retiring the slot is therefore not one change but a campaign, and the order is roughly: the
  resolved audio tables (cheapest, already mapped), then fog/AI (mapped, function-local), then
  weather (needs data extraction), then planets (the cap-of-8 inflation), with cutscenes and
  trigger assets needing RE before they can be scheduled at all.

When all of it is data, `overrides_stock_slot` degrades to what it looked like in the first place:
provenance, plus the fallback that picks the stock spline for a track that ships none.

---

## 4.3 In-game sign-in (Lou, 2026-09-15): the game gets its own Discord identity

**Why.** The first end-to-end run (publish -> download -> race -> record) ended with a manual step:
drag `custom_times.json` onto the website, because the game cannot sign a request. The website
holds a 7-day HS256 JWT from the Discord OAuth code flow; the game holds nothing. Every "push
something from the game" feature (record submit now, publish and update-check later) needs the
same missing piece, so build it once as a device link, not as a per-feature hack.

**Constraint that shapes it.** Discord does not implement the OAuth device grant (RFC 8628), and a
1999 executable cannot host the redirect of the code flow sanely. So the *website's existing
login* is the approval surface and botto-api mints the game a token of its own:

```
game                          botto-api                         junkyard (browser)
 |-- POST /auth/game/start -->|                                  |
 |<-- {code, poll_token,      |                                  |
 |     verify_url, ttl 600s}  |                                  |
 |-- ShellExecute(verify_url?code=XXXX) ------------------------>|  user is (or logs) in
 |                            |<-- POST /auth/game/approve ------|  "Link SW_RACER_RE on
 |                            |    {code}  (web JWT)             |   this PC?"  [Approve]
 |-- POST /auth/game/poll --->|                                  |
 |   {poll_token} every 3s    |   428 pending | 410 expired/denied
 |<-- 200 {token, user} ------|                                  |
 store token; panel shows "Signed in as <username>"
```

- The **displayed code and the poll_token are different secrets**: whoever sees the code on a
  stream cannot claim the token, and whoever holds the poll_token cannot approve. Standard device
  flow safety; codes live 10 minutes in `gameLinkCodes`.
- **The game token is itself a JWT** (`source: 'game'`, `scope: ['times:submit']`, long expiry,
  a `jti`). `authMiddleware` branch 1 already verifies `Authorization: Bearer <jwt>` and sets
  `req.user`, so `/customtracks/times`, `/publish`, `/blobs` need **no changes**; the middleware
  gains one lookup for `source: 'game'` against `gameTokens/{jti}` (revoked?, lastUsed). The
  profile page lists linked games and revokes them. Publish scope is added later, deliberately.
- **Storage in the game:** `assets/junkyard_token.json` (token, username, issued). Not the ini -
  players paste inis into Discord. Never written to hook.log. Sign out deletes it and revokes
  server-side. Plaintext on disk is the same posture as every launcher; scope + revocation is the
  mitigation, not encryption.
- **Consent = signing in.** Once linked, a new record is submitted automatically; a panel toggle
  turns that off without signing out. Not linked = nothing ever leaves the machine (today's
  behaviour).

**Submit plumbing (T5 in TIMES_ROADMAP).** `track_times_Submit` improving a record marks it
`submitted: false` in the sidecar; the catalog worker POSTs `{schema: 2, records: [that one]}`
- the ingest already takes the file format verbatim - and flips the flag on 200. The sidecar IS
the outbox: offline runs upload on the next boot or the next record, nothing extra to persist. 401
clears the token and the panel says so; 422 keeps the record local and logs the problem list.

**Client work:** generalize `http_get` to a request with method/body/headers (WinHTTP, same
worker); `ShellExecuteW` for the browser hop; a `Tracks > Account` panel (status, Sign in with
Discord, Sign out, auto-submit toggle, "N pending"); the `submitted` flag in `track_times`.
**Server work:** `gameLinkCodes` + `gameTokens`, three endpoints, one middleware branch, `/link`
page, devices list on the profile. Neither side is large; the value is that Phase 11 and Phase 13
both become "add a scope" afterwards.

**DONE + PLAYTESTED 2026-09-15, both halves, same day.** Client on `feature/junkyard-signin`
(`http_client.{h,cpp}`, `junkyard_account.{h,cpp}`, `account_panel.cpp`, `submission` state per
record in `track_times`); backend live at `/api/v1/auth/game/*`. First run: code shown, browser
opened, approved, signed in, the two pending records flushed; auto-submit then fired unattended
after later races. Learned from the live ingest: unpublished slugs and stock tracks come back
`unknown_track` inside a 200 (stock records belong to the speedrun.com-proxied boards, by design);
re-POST is idempotent (`not_improved`). One backend quirk for later: `GET /{slug}/times` is empty
unless `?laps=N` is passed, though coverage shows the records - the board wants a default. Phase 11
is therefore complete except that quirk; Phase 13 is now "add a `tracks:publish` scope".

---

## 4.4 Retiring the stock slot: the track environment descriptor (Lou, 2026-09-15)

`overrides_stock_slot` is not one coupling. It is **one pair of numbers, `(planet, subtrack)`,
feeding about a dozen consumers of three shapes** (the inventory is section 4.2). Deleting the
field means putting a single indirection between the numbers and the consumers, then moving the
consumers across one at a time - and the shape of each consumer is its cost.

| Shape | Consumers | Cost |
|---|---|---|
| Table indexed by (planet, subtrack) | race music `@0x4b8750`, intro theme `@0x4b8780`, ambient SFX `@0x4b8fa8 [planet*3+subtrack]`, cutscene `const char *[8] @0x4b7a08`, preview scale switch in `DrawTrackPreview` | cheap: hook the reader |
| Function-local data | fog / clear colour / camera spline in `SetupTrackEnvironment @0x464b90` (nested switch), AI difficulty in `InitAISettingsForTrack @0x4667e0` (fn-local `float[62]`) | medium: lift the data into a table, then it is a table |
| Control flow | weather (the `jdge->planetId == 1` block in the judge's per-frame update: particle cap, velocity, stretch, sun alpha, per-lap escalation), trigger assets (`if (planet_id == N)` chain in `swrObjTrig_LoadAndInitializeTriggerModels @0x47ddc0`, 15 models) | expensive: behaviour becomes data first |

**The structural move: `TrackEnv`, resolved once per track load** (`dinput_hook/track_env.h`).
Everything the EXE keys on the two numbers lives in one struct. A stock track's is filled from
the EXE's own tables by those same numbers, so behaviour is byte-identical; a manifest track's is
filled from the manifest. Consumers read the descriptor, never the numbers.

The manifest field changes meaning accordingly:

```json
"environment": { "inherit": "vanilla:track:16", "planet_track_number": 2 }
```

`inherit` names a stock **preset** to copy (the same `vanilla:track:NN` identity the times use),
and any field may be overridden on its own. That is exactly what `overrides_stock_slot` did,
made explicit and per-field; it stops being load-bearing the moment a field is overridden. The
converter writes `inherit` from the pack's slot, so existing tracks migrate for free, and the old
`placement` block is still read (mapped onto the same descriptor) so nothing published breaks.

**Order, each step shippable alone:**

1. **Descriptor + `inherit`** - plumbing, zero behaviour change; retires the field itself. Every
   later step becomes "add a field and move one reader". *(DONE 2026-09-15, PR #316)*
2. **Tables** - music, intro, ambient, cutscene (+ name `DAT_00e364f0` / `DAT_00e35a9c`). The sound
   tables are already reversed (`MODDING_ARCHITECTURE.md` section 9): wiring, not RE. Payoff: a
   custom track has its own music without claiming anyone's slot. *(DONE + PLAYTESTED 2026-09-15,
   PR #317: the entries for the track's (planet, subtrack) are written at track change and restored
   on the next; readers untouched. `sound_map` resolves Sounds.map names. `swrPlanetIntroCinematics`
   named. Limits: sounds must be in the bank already - custom wavs need a bank-registration path,
   the next slice; subtrack 3 is special-cased in the readers. The two masks are still unnamed.)*
3. **Lift the fn-local data** - `read_memory` the literals out of `SetupTrackEnvironment` and
   `InitAISettingsForTrack`, publish them as tables, make both read the descriptor. Fog and AI
   difficulty become authorable. *(DONE + PLAYTESTED 2026-09-15, PR #318. AI: delta reimpl with
   the table + script/variant rules, env overrides `ai.{level,spread,script,spline_variant}` -
   also stops a slot-borrowing track inheriting Ando's scripted AI. Fog/draw distance: post-fix
   after the original (`draw_distance`, `fog {near,color} | false`), so the four unnamed globals in
   SetupTrackEnvironment did not need naming yet. Fly-by spline: declare a spline asset at the
   inherited slot's cam-spline index instead. Left in the original: planet 4 sub 1/2 node-flag
   flip.)*
4. **Weather and trigger assets as data** - the parameter set the Ando Prime block keys on becomes
   `env.weather` (stock Ando gets the same values; a custom track gets its own or none); the
   descriptor carries the list of FX/prop models to preload (stock tracks get their planet's list).
5. **Spline-generated preview** - removes `DrawTrackPreview`'s HANG stub and its per-track scale
   switch together, and gives a ghost (section 4.1) a picture.

After step 4 `planet` is cosmetic - name and hologram - which is also what makes Phase 15
(custom planets) an identity-only job: the cap-8 inflation then touches nothing behavioural.

---

## 4.5 Planets: a preset, never a dependency (Lou, 2026-09-15)

**Decision.** A planet is its own catalog entity - reusable, shareable - but **a standalone track
must be able to define everything a planet would give it, inline, without naming one.** The
descriptor is the source of truth; a planet is a named bundle of defaults over the same fields,
layered under the track's own values exactly as `inherit: "vanilla:track:NN"` is today. Nothing in
the game may need a planet to exist for a track to work.

**What "planet-dependent" turned out to mean, after steps 1-4.** Behaviour no longer rides on the
planet number: music, ambient, cinematic, fog, draw distance, AI, weather are descriptor fields
(steps 2-4), dust borrows a palette (`dust_planet`, interim until the 8x7 table is lifted), and
trigger assets are the last control-flow consumer. What is left keyed on `PlanetIdx` is
**identity**, in a handful of parallel 8-entry structures:

| Structure | Holds | Read by |
|---|---|---|
| `swrPlanetTable[8]` (`swrPlanetInfo`, typed) | display name, sun/moon sprite range, tilt, spin | `swrObjHang_Init` (fills), `swrRace_CourseSelectionMenu` (name), `DrawHoloPlanet`, `OrderHoloRacerIcons` |
| `swrPlanetHoloModelNodes[8]` | the hologram node | `DrawHoloPlanet`; loaded once at boot by `swrUI_Front_LoadPlanetModels` (9 `pln_*_part` + 3 moons) |
| sun / lens-flare setup | position, colour, size per planet | first half of `swrPlayerHUD_SetupTrackOverlay(planet, subtrack)` (the decompiler's `hudType`/`weatherLevel` are the two numbers) |
| `swrPlanetIntroCinematics[8]` | pre-race movie | done (step 2) |
| `DrawHoloPlanet` `planetIdx == 6` | one special case | control flow |

**Mechanism: the same as step 2.** Identity is data in tables, so a track that defines its own
gets it **written into its planet's row while it is current and restored on change** - no
inflation, no new slot, and the galaxy map (which shows the eight stock planets) is untouched. The
hologram is the one asset-shaped field: a track's `holo_model` is a RAWM blob, loaded into the
scene at apply and swapped into `swrPlanetHoloModelNodes[planet]` for the track's lifetime.

**Manifest shape.** Inline on the track:

```json
"environment": {
  "planet_name": "Hoth",
  "holo": { "model": "<sha256>", "tilt": 0.3, "spin": 0.4 },
  "sun": { ... }              // the SetupTrackOverlay parameters, once named
}
```

and as an entity, `assets/planets/<slug>/planet.json` carrying exactly those fields (plus its own
cinematic / music / weather / dust defaults), named from a track by `"planet": "community.hoth"`
and layered under the track's own `environment`. The catalog lists planets beside tracks; a track
that names a planet lists it as a dependency the downloader fetches first.

**Order:** (a) name, tilt, spin as table patches *(DONE + PLAYTESTED 2026-09-15, PR #316: names
need the stock markup prefix `~f4~c~s`, spin is deg/s on a per-boot-randomized +-5 table; the
select page now applies the HOVERED track's descriptor every frame - tables only - so the row
always belongs to the track under the cursor)*; (b) sun / lens flare as a
post-fix after `SetupTrackOverlay` (hooked by address like the dust delta) *(DONE + PLAYTESTED
2026-09-15: `sun: false | {position, scale, color}`; sprites created by us since planets 5/7 have
none)*; (c) the hologram model swap; (d) planet manifests + catalog entity + `"planet":`
layering; (e) trigger assets, the last control-flow consumer *(DONE 2026-09-15 as
`trigger_planet: n` - the ARGUMENT is substituted, which keeps swrObjTrig_CurrentPlanetId
consistent; 3 = none; per-slot lists need the AddTriggerDescriptions slot mapping first)*, then
`overrides_stock_slot` / `inherit` become pure convenience.
Inflating the eight slots (custom planets on the galaxy map) is deliberately **not** in this
plan: with identity as per-track data there is nothing a new slot would carry that a row patch
does not, except a place on a map that only knows eight.

---

## 5. Phases after the MVP

| Phase | Scope | Depends on | Value |
|---|---|---|---|
| 9 | **Custom circuits + campaign pages** - data-driven names/colors/membership/order (`g_aTracksInCircuits @0x4bfee0`, `g_aTrackIDs @0x4c0018` inflate) so packs stop living in "Custom Tracks Page N". | M3 | UX polish, track packs |
| 10 | **Full external definition** - retire `overrides_stock_slot` by putting one indirection (the track environment descriptor, section 4.4) between `(planet, subtrack)` and everything the EXE keys on them, then move the consumers across by shape: tables, fn-local data, control flow. Ends with `planet` cosmetic. | M3, 4.4 | requirement 2, completed |
| 11 | **Leaderboard integration** - boards + ingest SHIPPED with M7 (2026-09-15); what remains is the game submitting on its own = section 4.3 sign-in + the `submitted` outbox flag. Manual bridge today: drop `custom_times.json` on `/customtracks/times`. | M6, M7, 4.3 | requirement 3, community |
| 12 | **Trust layer** - tie submissions to the run-verification work (delta-doesn't-touch-sim attestation, replay capture) so custom boards can be moderated rather than taken on faith. | Phase 11, REPLAY_ROADMAP | speedrun credibility |
| 13 | **In-game publish + update** - upload from the game (adds a `tracks:publish` scope to the section 4.3 token), "update available" prompts, subscribe-to-author. | M5, M7, 4.3 | convenience |
| 14 | **Shared asset libraries** - because assets are discrete and hashed, a "texture pack" or "prop set" is just a package other manifests reference by hash. Falls out of M1-M4 nearly free. | M4 | authoring leverage |
| 15 | **Planets** - custom skies/holo/sun-moon; the `< 8` cap inflation job. | `MODDING_ARCHITECTURE.md` Phase 2 | the big visual unlock |
| 16 | **Multiplayer parity** - `content_hash` handshake in the lobby, "host is running a track you don't have -> download it", MP times. | M5, MULTIPLAYER_ROADMAP | MP with custom tracks |

---

## 6. Risks and the answers we already have

| Risk | Mitigation |
|---|---|
| **Detouring the loader seam breaks every block consumer** (models, sprites, splines, textures - the whole game loads through it) | Fall-through by default: only indices the registry mapped for the active track are served virtually, everything else hits the real file. Land M2 behind a config toggle, and keep the branch's temp-file path as the fallback until a full-game pass (boot, all 25 stock tracks, hangar, MP) is clean. |
| Texture-index translation is wrong -> white/garbage textures (the G8 class of bug, already seen) | The manifest's texture order is the contract (3.2(b)); validate at install that every `texture_index` the model references is `< textures.length`; reject the package otherwise instead of rendering garbage. |
| Downloaded content crashes the game (malformed model/spline; PRs #296/#301) | Validate server-side on publish AND client-side before bind; keep the spline-usability check; quarantine a package that faults once (mark, skip, surface in the browser). |
| Stock-texture reference hash mismatch (modded or different release) | Client indexes its own `data/lev01` by hash at M4; a missing stock hash is a clear "this pack expects unmodified game data" error, never a silent wrong texture. |
| `MAX_CUSTOM_TRACKS 70` / texture-slot / id-range exhaustion once downloads are one click | Registry owns allocation and reports headroom; the browser refuses to install past a cap with a clear message; raise caps in Phase 9 (pre-inflated headroom is the existing pattern). |
| Track edited under the same slug invalidates records | `content_hash` is part of the times key; a re-published build starts a new board and old times stay attributed to the old hash. |
| Save-file corruption (records) | Custom times never touch `swrSaveData`; sidecar file with its own version + checksum. M0 removes the existing OOB write. |
| Copyright / moderation (F-Zero and Mario Kart ports already sit in `assets/custom_tracks/`) | Discrete assets *reduce* exposure - packages stop redistributing stock game archives entirely. Plus moderation state on `trackMods`; takedown = flip a flag, blobs stay content-addressed and unreferenced. Policy call is Lou's, not the client's. |
| HTTP in a 1999 32-bit process | WinHTTP (ships with Windows, no new dependency), worker thread, hard timeouts, never block the game thread - the rule the async MP `SendEx` fix established (PR #96). |
| Schema churn across three repos | `schema` int in every manifest and payload; the game refuses an unknown major with a message rather than guessing. |

---

## 7. Immediate next steps

1. **Ship M0** as a small PR (record-write guard) - a real save-corruption bug, independent of
   everything else here.
2. **Revive `loose-model-replacement`** (rebase onto master, `/pre-pr-check`, PR it). It is the
   `RAWM` half of M1 and has been sitting unmerged; everything downstream builds on it.
3. **Freeze the `track.json` schema** (section 3.3) in a form botto-api and junkyard can code
   against - that unblocks M7 in parallel.
4. **Spike the virtual block provider (M2)** on `feature/loader-virtual-block`: detour the three
   `swrLoader` functions with pure fall-through first (prove zero behavior change across boot +
   a stock race), then map a single discrete `RAWM` + `RAWS` pair and race it.
5. **Convert one existing pack** (`assets/custom_tracks/mutecity` or similar) to discrete chunks
   with the extended extractor as the M1 acceptance test and the M3 fixture.
