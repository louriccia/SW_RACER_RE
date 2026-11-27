# Track modding PRs — restack plan

**Status:** plan (2026-11-27). No code moved yet. Owner: lightningpirate.
Related: `CUSTOM_TRACKS_ROADMAP.md` (the feature design), `refined/tracks/README.md` and
`refined/core/README.md` on `refine/track_lifecycle` (the refinement target), `REFINEMENT_LAYER_ROADMAP.md`.

## 1. Where the stack stands

Five open upstream PRs, each containing the one below it. All are 94 commits behind `dev`; none has
a review (only comment: the #276 rebase note on #316).

| PR | Branch | Own commits | Own diff | What it adds |
|---|---|---|---|---|
| [#312](https://github.com/tim-tim707/SW_RACER_RE/pull/312) | `feature/track-catalog` | 2 | +764 / 8 files | catalog client + worker (`track_catalog`), browser panel, `serve_catalog.py`, registry rescan |
| [#313](https://github.com/tim-tim707/SW_RACER_RE/pull/313) | `feature/track-times` | 8 | +781 / 14 files | `track_times` sidecar (key = slug, content hash, mirror, laps, upgrades), `hash_util`, course-info records, converter content hash |
| [#314](https://github.com/tim-tim707/SW_RACER_RE/pull/314) draft | `feature/junkyard-signin` | 5 | +988 / 12 files | `http_client`, `junkyard_account` device-link sign-in, `account_panel`, record submit, worker shutdown fixes in `Window_delta` |
| [#315](https://github.com/tim-tim707/SW_RACER_RE/pull/315) draft | `feature/native-track-select` | 2 | +280 / 8 files | catalog "ghost" tracks in the native track select, download on choose; refuse to start a track whose assets did not bind |
| [#316](https://github.com/tim-tim707/SW_RACER_RE/pull/316) draft | `feature/track-env` | 13 | +1308 / 20 files | `TrackEnv` descriptor + manifest `environment`, table write/restore, AI/fog/weather/sun/holo/dust/trigger-planet overrides, custom wavs (`sound_map`) |

Why it is stuck: one 4,000-line chain mixing network services with game binding, so the bottom
cannot be reviewed without implicitly signing up for the top, and every `dev` rebase touches all five.

## 2. The split: services vs binding

The stack is two different kinds of code.

- **Services** never touch game memory: networking, the content store, records storage, account,
  panels, offline scripts. They are ordinary modding-layer features, and mostly fine as written.
- **Binding** is everything that makes the game see a custom track: slot allocation in the inflated
  track table, block views, writing per-track values into retail tables and restoring them, the
  "run the original, then overwrite its outputs" deltas, the per-frame re-apply for the hovered
  track. This is the hacky part, and exactly what `refined/tracks` replaces in `swe1r`.

Today services call binding internals directly (`track_registry_*` by table index, `TrackEnv`
fields, `tracks_delta.c` hooks). The plan puts **one interface** between them.

### 2.1 File lanes

| Lane | Files (from the stack) |
|---|---|
| Services | `track_catalog.*`, `track_browser.cpp`, `http_client.*`, `junkyard_account.*`, `account_panel.cpp`, `track_times.*` (storage + submit logic), `hash_util.*`, `scripts/serve_catalog.py`, `convert_track_pack.py`, `add_track_sound.py`, `gc_content_store.py` |
| Contract (data) | `track_manifest.*` (manifest parsing, incl. the `environment` block), `TrackEnv` / `TrackTimeKey` structs, `sound_map.*` (parses `Sounds.map`) |
| Binding (retail DLL backend) | `track_registry.*` (table slots, views, ghosts), `track_env.cpp` (`ApplyTables` / `RevertTables`, weather/sun/holo writes), `virtual_block.*` edits, the edits to `tracks_delta.c`, `swrObjJdge_delta.cpp`, `swrRace_delta.cpp`, `swrMultiplayer_delta.cpp`, `renderer_hook.cpp`, `ai_tuning.cpp` |
| Separate fix | `Window_delta.c` worker shutdown (only matters once workers exist; goes with the first PR that starts one) |

### 2.2 The interface (`dinput_hook/track_api.h`)

Services and menus code against this; nothing else from the binding lane is visible to them. It is
the same surface `refined/tracks` provides in `swe1r`, so the services port unchanged.

```c
/* identity */
typedef struct TrackRef { const char* slug; const char* content_hash; } TrackRef;
int  track_api_Count(void);
bool track_api_At(int i, TrackRef* out);              /* menu order, stock first */
int  track_api_Find(const char* slug);                 /* -1 if unknown */
bool track_api_Current(TrackRef* out);                 /* the track being raced */

/* install state (catalog ghosts) */
enum TrackInstall { TRACK_INSTALLED, TRACK_GHOST, TRACK_DOWNLOADING, TRACK_INSTALL_FAILED, TRACK_BIND_FAILED };
enum TrackInstall track_api_InstallState(const char* slug, char* text, int size, float* fraction);
void track_api_RequestInstall(const char* slug);
void track_api_Rescan(void);

/* properties services and menus need */
bool track_api_IsPointToPoint(const char* slug);
const TrackEnv* track_api_Env(const char* slug);       /* resolved descriptor; read-only */

/* lifecycle (same names as refined/track_lifecycle) */
int track_api_OnBeforeLoad(const char* owner, void (*fn)(void*), void* user);
int track_api_OnLoaded(const char* owner, void (*fn)(void*), void* user);
int track_api_OnUnload(const char* owner, void (*fn)(void*), void* user);

/* race results, for times */
int track_api_OnResults(const char* owner, void (*fn)(const struct TrackRaceResult*, void*), void* user);
```

- Slugs, not table indices, cross the interface. The table index stays a binding detail (it is
  install-order dependent, which is also the MP problem T3 in the tracks dossier).
- `TrackRaceResult` is a plain struct (key fields, times, laps, pilot, upgrades, fps evidence) filled
  by the binding from the hangar/judge; `track_times` no longer reads `swrObjHang` itself.
- Course-info record drawing stays a binding hook that asks `track_times_Get(key)`.

### 2.3 What stays hacky, and where

The retail DLL has no other way in: table write/restore, the operand-patched inflated table and the
"original then overwrite" deltas remain the retail backend. The plan only **contains** them:

- one file per mechanism (`track_bind_tables.cpp`, `track_bind_env.cpp`, ...), each with a header
  comment naming the retail reader it serves and the refined replacement (`refined/tracks` step);
- no service or menu calls them;
- the hovered-track per-frame re-apply becomes an apply-on-cursor-change (still a table write, but
  once per change, with the same restore);
- with no custom tracks installed, the backend is a no-op: stock lockstep must match retail
  exactly (oracle for every binding PR).

## 3. New PR series

Every PR is cut fresh from current `upstream/dev`, base `dev`, `/pre-pr-check` before opening.
Stacked only where a later PR needs an earlier one's interface; each states its parent.

| # | PR | Lane | Content (from) | Size | Needs |
|---|---|---|---|---|---|
| A | **Custom-track record and bind fixes** | fix | `StartRace` record read past the 50-slot save array for custom indices (tracks dossier U2; write side already guarded on dev); refuse to start a track whose assets did not bind (6d4c73e2) instead of racing the stock slot it stands in for; refuse a row with model or spline -1 instead of silently racing Mon Gazza (tracks dossier S1) | S | — |
| B | **Track interface + retail backend** | binding | `track_api.h`; existing `track_registry` behind it (slug lookup, lifecycle events from the existing InitTrack delta, `IsPointToPoint`); `TrackEnv` resolved from the table row only (no overrides yet). Behaviour unchanged | M | A |
| C | **Track catalog** | services | #312 content against B (`track_api_Rescan`, slugs); worker shutdown fix from #314 (`Window_delta`) since this PR starts the first worker | M | B |
| D | **Times by slug** | services + thin binding | #313: sidecar store, key/detail, converter content hash; results via `track_api_OnResults`; course-info drawing hook in binding | M | B |
| E | **Junkyard sign-in + record submit** (draft until backend sign-off) | services | #314 minus the shutdown fix (now in C) | M | C, D |
| F | **Catalog tracks in native track select** | binding (menus) + services | #315: ghosts as `TRACK_GHOST` install state, download on choose; `tracks_delta.c` menu edits only | S | C |
| G1 | **Track environment: descriptor + plain tables** | contract + binding | #316 step 1-2: manifest `environment` (`inherit` + fields), music/intro/ambient/cinematic via contained table write/restore | M | B |
| G2 | **Track environment: AI and fog** | binding | #316 step 3 | S | G1 |
| G3 | **Track environment: weather, sun, planet identity** | binding | #316 weather, sun, holo name/tilt/spin; hovered re-apply as apply-on-change | M | G1 |
| G4 | **Track environment: dust and trigger planet** | binding | #316 dust palette, trigger FX set (explicitly temporary: superseded by the swrObjTrig action lists) | S | G1 |
| G5 | **Custom track sounds** | contract + binding | `sound_map`, bank growth, `add_track_sound.py` | M | G1 |

Order to open: A, B, then C/D/G1 in parallel (independent once B lands), then E, F, G2–G5.
Each is reviewable alone; the biggest (B, C, D, G1, G3, G5) stay under ~600 lines.

### 3.1 What happens to #312–#316

Close the five with a short note pointing at the replacements ("split into A–G, same content behind
one interface"), and link each new PR to the old one it carries. Rebuilding on the old branches would
drag their 94-commit-old bases and the cumulative diffs along; nothing was reviewed, so no
discussion is lost. Old branches stay on origin as backups until the series merges.

## 4. Mapping to the refinement layer

| Restack piece | `swe1r` (refined) | Retail DLL |
|---|---|---|
| `track_api.h` | implemented by `refined/tracks` registry (dossier step 2) on core registries | B's wrapper over `track_registry` |
| `TrackEnv` | resolved env (tracks dossier 4.2); consumers read it directly | G1–G4 table/global writes |
| Manifest `environment` | core schema loader (core step 3), same JSON | `track_manifest.cpp` parser |
| Lifecycle events | `refined/track_lifecycle` | existing InitTrack / ResetToIndex deltas |
| Times key | same `(slug, content_hash, mirror, laps, upgrades)` | same |

So the services and the data format are written once; only the backend differs. If tim takes
`refined/` upstream (roadmap 9.1), the DLL backend shrinks consumer by consumer as the refined
versions land. If not, the interface still keeps upstream's track code reviewable.

Field-name alignment to settle before G1 (the manifest is the published contract): the tracks dossier
names (`env.music`, `env.weather.stages[]`, `env.ai.*`) and #316's JSON (`music`, `weather.stages`,
`ai.level`) mostly agree; diff them once and freeze one schema, with the slug scheme
(`vanilla:track:NN`, already in published manifests and times files) kept canonical.

## 5. Verification per PR

| PR | Check |
|---|---|
| all | full DLL link; `/pre-pr-check`; lockstep stock 25/25 with no custom tracks installed (binding must be invisible) |
| A | custom track from the record screen and course info: no over-read (ASan-style guard or log), unbound track refused with a message |
| B | stock + legacy + manifest tracks list and load as before; slug round-trip for every entry |
| C | `serve_catalog.py` local catalog: fetch, install, rescan, quit mid-download (worker shutdown) |
| D | record set on stock and custom track, mirror and lap variants; save image untouched for stock |
| G1–G5 | Mute City (inherits Boonta Training) then Boonta Training raced to prove every restore, as #316 did; audio-id and texture-hash checks where available |

## 6. Open questions

- Slug scheme: keep `vanilla:track:NN` canonical (published) and alias `core.track.*` in the
  refined registry, or the reverse? (Also open in the tracks dossier.)
- Does E wait for a backend contract with junkyard, or ship as a draft behind an INI switch?
- G4 (trigger planet) is throwaway once swrObjTrig lands: ship it, or leave cross-planet trigger
  FX to the refined path only?
- Ask tim whether he prefers the series opened all at once (as drafts) or one at a time.
