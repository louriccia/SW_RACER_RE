# SW_RACER_RE — Audio Modernization Roadmap

**Status:** design (2026-07-04). Living document. Owner: lightningpirate.

Goal: give SWE1R a **modern audio backend** — HRTF binaural, environmental reverb, occlusion,
and modern-format / higher-fidelity assets — **without changing the faithful default sound.** The
game targets a dead API (Aureal A3D); on any current machine it falls back to A3D's bundled
software renderer, which degrades to plain 2D/panned stereo. Everything A3D was *good* at
(geometry-driven reflections, reverb, occlusion — "Wavetracing") is inert today. This roadmap
routes the game's already-computed listener/source geometry into a live, maintained engine
(OpenAL Soft) behind the swrSound API, and finally makes the dormant reverb/reflection/occlusion
controls mean something.

Like the graphics, modes, replay, and modding work, this is a **delta on understood behavior.** It
rides the `dinput_hook/` Microsoft Detours layer and the **reimplemented `swrSound` seam** — it
does **not** change the faithful `src/` sim. The default build stays byte-faithful A3D; the modern
backend is **opt-in behind a build flag** (`AUDIO_REPLACEMENT`, mirroring `RENDERER_REPLACEMENT`).
This protects the run-verification story (`VERIFICATION_ROADMAP.md`) — although audio does not feed
the sim, keeping a byte-identical default is the standing contract across all these roadmaps.

> **Addresses/offsets below come from `swrSound.h` + Ghidra-DB + source exploration (2026-07-04)
> and MUST be reconfirmed at implementation time** (the Steam EXE `.text` is SteamStub-encrypted on
> disk — verify via Ghidra disasm or runtime, not file bytes). Effort tags: **S** < ~half day,
> **M** ~1–2 sessions, **L** multi-session. Line numbers are as of 2026-07-04.

---

## 0. Decisions (2026-07-04)

| # | Decision | Choice | Consequence |
|---|----------|--------|-------------|
| 1 | **Where the backend swaps** | **Behind the reimplemented `swrSound` API** (native seam), not an `a3d.dll` COM shim | one abstraction consumes the geometry the game already computes; also catches the SMUSH cutscene path (§1.4), which a pure A3D shim cannot |
| 2 | **Backend library** | **OpenAL Soft** (LGPL, C API, actively maintained, HRTF + EFX/EAX-emulation, up to 7.1/B-format) | maps cleanly onto A3D's source/listener model; miniaudio considered but lacks built-in HRTF/EFX; Steam Audio deferred to Tier 3 as an advanced propagation layer *on top* of OpenAL |
| 3 | **Faithfulness** | **Byte-faithful A3D is the default build.** Modern backend gated by `AUDIO_REPLACEMENT` (build) + an in-game/ImGui toggle | vanilla is always shippable and verifiable; modern audio never a silent default |
| 4 | **Asset policy** | Modern-format loose files (Ogg/FLAC/hi-rate WAV) are **opt-in additive**, decoded to PCM at load; vanilla `data/wavs/**` untouched | lifts the 22 kHz/PCM ceiling for modders without breaking `sounds.map` or the save |
| 5 | **Volume/persistence** | Fold in the #221 fixes as the unified pipeline's foundation (single master → all paths incl. cutscenes) | already partly shipped (PR #223); this roadmap generalizes it |

**A3D shim (rejected as the primary path, noted as an alternative):** a DirectSound-DSOAL-style
`a3d.dll` that translates `IA3d4`/`IA3dSource`/`IA3dListener` → OpenAL. Pro: engine-agnostic, no
game changes. Con: no off-the-shelf A3D→OpenAL shim exists (unlike DSOAL for DirectSound), the
cutscene path (SMUSH/DirectSound) bypasses it entirely, and we already own the swrSound API from
the decomp. The open-source [Aureal A3D SDK](https://github.com/sigmaco/a3d-sdk) documents the full
COM surface if this is ever revisited.

---

## 1. Current state (what the decomp already gives us)

### 1.1 The engine is Aureal A3D 2.0/3.0 — abandoned since 2000
The game drives the `IA3d4` device, `IA3dListener`, and `IA3dSource` COM interfaces
(`src/types_a3d.h`), reimplemented in `src/Swr/swrSound.c`. A3D was Aureal's EAX competitor; its
differentiator was **Wavetracing** — real-time geometric reflection/reverb/occlusion from the
actual scene geometry. Aureal went bankrupt in 2000; **Creative bought the assets and shelved A3D**
to protect EAX. There has been no vendor or driver since.

### 1.2 How it runs today — silent degradation to 2D
`swrSound_Init` (`0x004848a0`) calls `GetHardwareCaps`; with no Aureal Vortex chip present there is
no hardware 3D, so the engine sets `Sound_enabled_3d = 0` and forces every source to native render
mode (`0x20`) — basic software 2D/pan mixing via the bundled `a3d.dll` software fallback. Doppler,
distance attenuation, and pan are computed by `swrSound_Update` (`0x00449ef0`, the per-frame
8-channel mixer) and handed to A3D, but **there is no HRTF, no environmental reverb, no occlusion**
in practice.

### 1.3 The environmental machinery is defined and DEAD
`src/types_a3d.h` fully declares the wavetracing surface: `IA3dSource::SetReflectionDelayScale` /
`SetReflectionGainScale` / `GetOcclusionFactor`, `IA3d4::SetHFAbsorbFactor` /
`SetMaxReflectionDelayTime`, and the feature flags `A3D_1ST_REFLECTIONS 0x02`, `A3D_OCCLUSIONS
0x40`, `A3D_REVERB 0x100`, `A3D_GEOMETRIC_REVERB 0x200`. **`swrSound.c` never calls any of them.**
The config even persists `REFLECTIONS`/`REVERB` toggles (`swrConfig.c` §1.5) that today control
nothing. The intent was there; the hardware to honor it is gone. **This is exactly the ground a
modern backend reclaims.**

### 1.4 There are TWO audio engines
- **Gameplay / menu / music** → A3D via `swrSound` (this roadmap's main seam).
- **FMV cutscenes** → **SMUSH** (LucasArts SAN codec) on a **separate DirectSound path**
  (`Window_PlayCinematic`, Ghidra-verified). The A3D master gain cannot attenuate it. On this
  branch the delta layer already intercepts SMUSH *video* frames (`Window_SmushPlayCallback_delta`,
  `Window_delta.c:289` → `renderer_drawSmushFrame`, `renderer_utils.cpp:258`); the *audio* side is
  the #221 work. A unified pipeline must own both engines.

### 1.5 The fidelity ceiling
- **Assets:** 8/16-bit PCM WAV at **11.025 or 22.05 kHz**, mono/stereo, under `data/wavs/11K` and
  `data/wavs/22K` (chosen by `Main_hiRes_sound`), plus `data/wavs/Music` (streamed, 22050/16/stereo,
  double-buffered) and `data/wavs/Voice`. Registry: `data/sounds.map` (`NUMSOUNDS`/`NUMVOICES`),
  resolved by name through a hashtable. 22 kHz source material is a hard ceiling.
- **Mixer:** 8 voice channels; distance + doppler + pan wired.
- **Spatialization:** A3D positional (or 2D fallback). No binaural.

### 1.6 The "finicky" volume/persistence layer (issue #221, largely fixed)
Two volume pipelines (A3D output gain vs SMUSH/DirectSound) × two persistence stores
(`audio.cfg` vs the `tgfd.dat` save image). `WriteAudioConfig` never serialized the volume bytes;
`swrSound_Startup` forces output gain to 1.0 every boot; intro cutscene volume was hardcoded to
max; config writes are CWD-relative and fail silently under Program Files. **Shipped as PR #223**
(delta-layer master + cutscene sliders). Residual: redirect writes to `%LOCALAPPDATA%`. See
[[audio-issues-221]].

### 1.7 RE / reimplementation progress — the API is essentially OWNED
`swrSound` is nearly fully mapped and mostly reimplemented across PRs #42, #56, #66, #79, #116,
#144, #207: streaming thread, RIFF/WAVE parser, 8-channel mixer, SFX throttle/cooldown, the music
controller, and the jump-table `ResolveSfxId` — all runtime-verified in-game. Remaining stubs:
`swrSound_Startup` (`0x00421d90`) and `swrSound_PlaySpatial` (`0x00426d80`). **Owning this API is
the whole reason a clean backend swap is now feasible.** See [[swrsound-subsystem]].

---

## 2. The core realization — swrSound is the seam

We do not need to reverse or intercept A3D. The game **already computes**, every frame, exactly
what any modern spatial engine needs: listener position/orientation/velocity
(`swrSound_SetTransforms` `0x00484f40`), per-source position/velocity
(`swrSound_SetPosition` `0x00484e10`, `swrSound_SetVelocityClamped` `0x00484e40`), gain, pitch,
min/max distance, and PCM buffers (`swrSound_ParseWave` `0x004851a0`,
`swrSound_CreateSourceFromFile` `0x00423050`). All of it funnels through a small, reimplemented,
**owned** API surface.

So the backend abstraction is: **intercept the ~20 leaf A3D-wrapper functions** (the `0x00484xxx`
cluster + streaming lock/unlock) in the **delta layer** (`hook_replace`, not by editing `src/`) and
dispatch them to an `IAudioBackend` with an A3D implementation (default) and an OpenAL
implementation (opt-in). Everything above the wrapper — id resolution, throttling, music
cross-fade, preload, and the entire per-frame mixer (`swrSound_Update` @ `0x00449ef0`) — is
engine-agnostic and stays put, running the game's own (or reimplemented) code unchanged.

**Why this works (audited 2026-07-04, `A3D-Update.md` decomp of `sub_449EF0`):** the mixer — the
primary audio driver, still original `.text` — reaches A3D **only** through the named leaf wrappers
(`sub_484D90` SetGain, `sub_484E10` SetPosition, `sub_484BE0` Play, `sub_484FA0` Flush, the
`sub_484F10`/`sub_484F40` listener calls, …). It holds each source as an opaque pointer
(descriptor `+0x48`), and only **stores, compares, and passes** it — it **never** dereferences an
`IA3dSource*` vtable inline. Therefore a backend can hand back an opaque non-A3D handle and the
mixer round-trips it untouched. This is what makes the leaf-wrapper seam airtight without
reimplementing the mixer first.

---

## 3. Target architecture

```
game code (swrRace, swrObj*, menus, music controller)
        │  (unchanged: computes positions/velocities/gains/ids)
        ▼
swrSound gameplay API  (ResolveSfxId, PlaySfxThrottled, PlaySpatial, UpdateMusic, Update mixer)
        │  (engine-agnostic; reimplemented, owned)
        ▼
┌─────────────────────────────────────────────────────────────┐
│  IAudioBackend  (NEW abstraction — the ~20 leaf wrappers)     │
│  NewSource / Play / SetPosition / SetVelocity / SetGain /     │
│  SetPitch / SetMinMaxDistance / SetListener / SetOutputGain / │
│  Flush / Lock+Write (streaming) / ReleaseSource               │
└───────────────┬──────────────────────────┬───────────────────┘
                │ (default)                 │ (AUDIO_REPLACEMENT + toggle)
                ▼                            ▼
        A3D backend                   OpenAL Soft backend
   (byte-faithful vanilla)      HRTF · EFX reverb · occlusion filters
                                      │
                                      ├─ Tier 4: modern-format asset decode (Ogg/FLAC/hi-rate)
                                      └─ Tier 3+: Steam Audio propagation (optional)

  cutscenes: SMUSH/DirectSound ── (unify volume) ──► same master gain / (later) same backend
```

The A3D backend is the current `swrSound.c` bodies, refactored behind the interface with **zero
behavior change** (the acceptance test). The OpenAL backend maps A3D concepts 1:1: source→`alSource`,
listener→`alListener`, distance model→`AL_INVERSE_DISTANCE_CLAMPED`, doppler→`alDopplerFactor`,
reverb/occlusion→EFX auxiliary sends + filters.

---

## 4. Phases

### Phase 0 — Backend abstraction + A3D pass-through. Effort: M. [DONE 2026-07-05, commit 6ba2783]
The thing every other phase hangs off — no OpenAL yet. Establishes the seam and proves it is
transparent. **Touches only `dinput_hook/`; `src/` stays a clean decomp.** SHIPPED as
`dinput_hook/audio_backend.h` + `game_deltas/swrSound_delta.{h,cpp}` (22 leaf deltas → `g_audio`,
A3D pass-through backend, `init_audio_hooks()`), `AUDIO_REPLACEMENT` CMake option (ON), wired in
`main.cpp:LoadIconHook`. Builds ON+OFF; **playtest-verified transparent**. NOTE vs the scope below:
files live flat in `dinput_hook/` (not an `audio/` subdir — the source GLOB does not recurse there);
device lifecycle (`Init`/`Shutdown`) is **not** hooked (A3D device stays game-managed, so the 0c
sentinel is deferred to Phase 1); 0d is **superseded by PR #223** and not re-folded here.

**0a — the interface.** A C struct of function pointers (`dinput_hook/audio/audio_backend.h`),
one entry per leaf A3D operation the wrappers actually perform (§8.1 is the exact inventory). The
source handle is opaque:

```c
typedef void* AudioSourceHandle;   // A3D backend: IA3dSource*; OpenAL backend: its own struct*

typedef struct AudioBackend {
    // engine / device
    int  (*init)(void);                        // bring up device; set the gates the game reads (see 0c)
    void (*shutdown)(void);
    void (*set_output_gain)(float gain);       // master
    void (*flush)(void);                        // commit batched state (A3D Flush)
    unsigned (*get_hardware_flags)(void);       // caps, for SetDefaultConfig / reflections gating
    // listener
    void (*set_listener_transform)(rdVector3* pos, rdVector3* fwd, rdVector3* up);
    void (*set_listener_velocity)(rdVector3* vel);
    // source lifecycle
    AudioSourceHandle (*new_source)(int stereo, int samples_per_sec, int bits, int size_bytes, int flags);
    AudioSourceHandle (*duplicate_source)(AudioSourceHandle src);
    void (*release_source)(AudioSourceHandle src);
    int  (*play)(AudioSourceHandle src, int loop);      // true on success (looping also forced by src type)
    int  (*rewind)(AudioSourceHandle src);              // = stop + rewind
    int  (*get_wave_position)(AudioSourceHandle src, unsigned* out_pos);  // -1 err / 0 stopped / 1 playing
    // per-source params
    void (*set_gain)(AudioSourceHandle src, float gain);                  // 2D-fallback gain_adjust is backend-internal
    void (*set_pitch)(AudioSourceHandle src, float pitch);
    void (*set_min_max_distance)(AudioSourceHandle src, float min, float max);
    void (*set_position)(AudioSourceHandle src, rdVector3* pos);          // A3D maps (x, z, -y)
    void (*set_velocity)(AudioSourceHandle src, rdVector3* vel);          // clamped ±340/±10/±340
    void (*set_pan)(AudioSourceHandle src, float pan);                    // -1..1; 2D/UI sounds
    void (*set_distance_model_scale)(AudioSourceHandle src, float scale);
    void (*set_render_mode)(AudioSourceHandle src, unsigned mode);        // A3D 0x20 = native/2D
    unsigned (*get_render_mode)(AudioSourceHandle src);
    // streaming (PCM fill; A3D uses Lock/Unlock, OpenAL uses buffer queueing)
    void* (*lock)(AudioSourceHandle src, int nbytes, int* first_block_len);
    int   (*unlock)(AudioSourceHandle src, void* ptr, unsigned nbytes);
} AudioBackend;

extern AudioBackend* g_audio;   // selected at init; default = &a3d_backend
```

**0b — interception.** One `swrSound_X_delta` per leaf wrapper (e.g. `swrSound_SetPosition_delta`
→ `g_audio->set_position(src, pos)`), registered with `hook_replace(swrSound_SetPosition,
swrSound_SetPosition_delta)`. The wrappers are already reverse-hooked (reimplemented in
`swrSound.c`), so they are in the `hooks` map and `hook_replace` redirects the game address to the
delta. The **A3D backend** implements each op as `hook_call_original(swrSound_X, …)` — i.e. it runs
the untouched game/reimpl code, so this backend is byte-faithful by construction. Lives in
`dinput_hook/game_deltas/swrSound_delta.cpp` + `dinput_hook/audio/a3d_backend.cpp`.

**0c — the gate globals.** Nearly every wrapper and `swrSound_Init` guard on `IA3d4_ptr != NULL` /
`Sound_enabled_3d`. When a non-A3D backend is selected, its `init()` sets `IA3d4_ptr` to a non-NULL
sentinel + `Sound_enabled_3d = 1` so the game's gates pass and the mixer runs; the leaf deltas then
intercept before any sentinel is dereferenced (safe because of the §2 no-inline-deref audit — the
one remaining thing to confirm is `swrSound_LoadSound`/`AcquireSource`, which manage the descriptor,
not the source vtable). The A3D backend uses the real pointer as today.

**0d — unified volume.** Fold in the #221 foundation: one master gain feeds `set_output_gain`
**and** the SMUSH cutscene path (`Window_PlayCinematic`), so cutscenes finally obey the master.

**Build gate:** the whole thing behind `AUDIO_REPLACEMENT` (CMake option mirroring
`RENDERER_REPLACEMENT`). Off ⇒ no hooks registered ⇒ literally today's binary.

**Acceptance:** (1) `AUDIO_REPLACEMENT=OFF` build is byte-identical to today. (2) With it on and
`g_audio = &a3d_backend`, in-game audio is unchanged (the delta indirection is transparent). Proves
the seam before any OpenAL code exists.

> **Optional de-risk (not a blocker):** closing the last three `swrSound` stubs — `Update`
> (`0x449ef0`), `PlaySpatial` (`0x426d80`), `Startup` (`0x421d90`) — as reimplementations makes
> *all* A3D access flow through owned leaf wrappers with zero original-`.text` vtable derefs on the
> audio path, retiring the 0c sentinel hack and taking `swrSound.c` to 100%. The mixer audit shows
> the current original code is already clean, so this is sequencing preference, not a dependency.

### Phase 1 — OpenAL Soft backend: parity + HRTF. Effort: L. Depends: 0.

> **Acquisition (found 2026-07-05):** the sibling `modules/OpenJKDF2` already builds OpenAL Soft
> **1.23.1** with a MinGW toolchain — `cmake_modules/build_openal.cmake` (`ExternalProject_Add`,
> `SOURCE_DIR lib/openal`, `ALSOFT_STATIC_LIBGCC`/`ALSOFT_STATIC_STDCXX`, utils/examples off). Its
> `lib/openal` is an uninitialized submodule (source not present). This is the validated
> build-from-source path to reuse; a prebuilt-32-bit-binary drop-in is the lighter alternative.
> Start Phase 1 with the OpenAL backend selectable at runtime beside A3D (the RB2 A/B toggle) and
> the 0c gate-globals sentinel (`IA3d4_ptr` non-NULL + `Sound_enabled_3d = 1`) in its `init()`.

- Implement `IAudioBackend` over OpenAL Soft: create context/device, translate source lifecycle,
  positions, velocities, gain/pitch, min/max distance, listener transform, streaming (queue
  buffers where A3D used Lock/Unlock double-buffering), output gain.
- Coordinate mapping: A3D receives `(x, z, -y)`; feed OpenAL the equivalent right-handed vectors
  (reconfirm handedness against `SetCoordinateSystem(0)` at implementation).
- **HRTF** as the first genuine upgrade: opt-in binaural for headphone users; speakers keep
  standard panning.
- **Acceptance:** all SFX/voice/music play correctly through OpenAL; positional cues match or beat
  A3D; HRTF toggle audibly widens the stage on headphones; A3D backend still selectable.

### Phase 2 — Environmental reverb (revive the dead REVERB/REFLECTIONS flags). Effort: M. Depends: 1. [ITER-1 BUILT 2026-07-05, unplaytested]
- Wire OpenAL **EFX** reverb (EAX-1..4-emulation reverb) via an auxiliary effect slot.
- Map each track/planet (or terrain behavior) to a reverb preset — canyon, hangar, tunnel, open
  desert. The `REVERB`/`REFLECTIONS` config toggles and the `A3D_REVERB`/`A3D_1ST_REFLECTIONS`
  intent (§1.3) finally do something.
- Preset selection can hook the track-load path; per-track override is a natural modding hook
  (ties to `MODDING_ARCHITECTURE.md`).
- **Acceptance:** tunnels/hangars sound enclosed, open tracks dry; reverb-off = Phase 1.

**ITER-1 (built, green, UNCOMMITTED):** `dinput_hook/openal_reverb.{h,cpp}` — one EFX aux effect
slot + one EAXReverb effect (falls back to standard Reverb), a 15-entry environment preset library
(subset of `AL/efx-presets.h`: plains/mountains/canyon/valley/forest/cave/quarry/tunnel/hangar/
space/ice/underwater/city/arena) and a 25-entry `swrRace_TRACK`→preset table. Backend wiring in
`openal_backend.cpp`: `openal_reverb_init/shutdown` in `oal_init/oal_shutdown`, `ALC_MAX_AUXILIARY_
SENDS=2` context attr, and per-source `AL_AUXILIARY_SEND_FILTER` routing — attached on `set_position`
(3D world sources), dropped on `set_pan` (2D/UI stays dry); the shadow-music source is never wired,
so music stays dry. Auto-preset in `swrSound_Flush_delta`: polls `multiplayer_track_select`
(`0x00ea02b0`, via `swrRace_GetSelectedTrack`) and applies the mapped preset on a track change.
ImGui audio panel: "Environmental reverb" toggle → "Auto (match track)" + a manual "Room" combo +
"Reverb amount" (aux-slot gain) slider. Off by default ⇒ dry ⇒ byte-identical to Phase 1.
**Pending:** playtest (is it audible / preset fit by ear?); persist toggle/preset in the ini;
optionally drive the game's own persisted REVERB config flag as the source of truth; verify OFF build.

### Phase 3 — Occlusion via the collision mesh (A3D's Wavetracing, done right). Effort: L. Depends: 1.
- We already have the collision geometry (the collision-viewer work, [[collision-viewer-feature]]).
  Raycast listener→source against it; on a hit, apply an EFX low-pass **occlusion/obstruction
  filter** to that source. This is precisely what A3D Wavetracing promised and modern engines
  (Steam Audio) do now.
- Budget the raycasts (only near/audible sources; amortize across frames).
- Optional Tier 3+: swap the hand-rolled raycast occlusion for **Steam Audio** propagation
  (physically-based occlusion + convolution reverb) layered on OpenAL — evaluate cost/benefit here.
- **Acceptance:** a wall between pod and an emitter muffles it; removing the wall restores it; no
  audible cost on open tracks.

### Phase 4 — Modern-format / higher-fidelity assets. Effort: M. Depends: 1 (and rides the asset delta).
- Extend the loose-file asset loader (already the delta layer's job, [[asset-replacement-architecture]])
  so `swrSound_CreateSourceFromFile` / the streaming loader accept **Ogg Vorbis / FLAC / hi-rate
  WAV**, decoded to PCM at load. Vanilla `data/wavs/**` is untouched; a mod folder overrides by name.
- Lifts the 22 kHz PCM ceiling and enables full-quality music/voice packs.
- **Acceptance:** a 44.1/48 kHz Ogg dropped in a mod folder plays in place of the vanilla WAV; no
  vanilla asset changes; `sounds.map` still authoritative for the registry.

### Phase 5 — DSP chain + polish. Effort: M. Stretch. Depends: 1.
- Optional master DSP: EQ, limiter/soft-clip, dynamic-range (night mode), per-category submix
  (SFX / voice / music / engine) with independent volumes — generalizing #221's master into a real
  mixer.
- Optional per-source EFX (e.g. band-pass on comm chatter).
- All opt-in, neutral by default.

### Phase 6 — Proximity "whoosh" (speed cues from nearby geometry). Effort: M. Stretch. Depends: 1, 3.
A new **enhancement** (not in vanilla): when the pod skims a wall / rock / structure at speed, you
hear the surface streak past — the near-miss wind-rush that sells velocity in racing/flight games.
This is the natural payoff of the same geometry work Phase 3 stands up, and OpenAL gives most of it
for free.

- **The elegant framing — sonify the nearest surface.** Reuse Phase 3's collision-mesh query to find
  the closest point on nearby geometry each frame, drop a virtual **whoosh emitter at that point**,
  and feed it a looping air-rush. Then the *existing* 3D pipeline does the hard part: OpenAL's
  panning places it on the correct side, and **doppler** (Phase 1) supplies the pitch streak as you
  pass. Intensity = f(closing speed, 1/distance) → gain/low-pass; below a distance/speed threshold it
  is silent, so open track stays clean.
- **Only walls, not the floor.** Exclude the surface you're riding on (filter by surface normal —
  near-vertical = wall — or by lateral offset from travel), or it drones constantly. The
  terrain/surface-tag seam (`MODES_MODIFIERS_ROADMAP.md`) can classify what counts as "wall."
- **Asset.** A wind/whoosh loop — reuse an existing engine/wind sample if one fits, else a loose-file
  asset (rides Phase 4). Could also be filtered noise if a tiny synth is ever added.
- **Cost / feel.** Shares Phase 3's raycast budget (amortize; only the nearest 1–2 surfaces).
  Smooth gain/pitch to avoid pops; tune thresholds so it triggers on genuine near-misses, not every
  gentle bank. Purely opt-in, off by default, never in the faithful path (like all of §5).
- **Acceptance:** threading a canyon or grazing a tunnel wall at speed produces a directional
  whoosh that rises on approach and falls/doppler-drops as it passes; open desert is silent; toggle
  off = Phase 1.

### Phase 7 — Geometry-driven reverb (retire the per-track presets). Depends: 2, 3.
Phase 2's presets treat each track as **one room**, but a track runs open desert → canyon → tunnel →
cavern; the acoustics should change *as you move through it*. This is A3D's Wavetracing intent for
the reverb half (Phase 3 already does it for occlusion). Two tiers:

- **Tier A — geometry-*estimated* reverb. Effort: M.** Not ray-traced reflections, but the reverb
  responds to the *actual local space*. Each tick (amortized), probe a fan of raycasts from the
  listener (up / down / lateral / fore-aft) against the collision mesh — the same `swrRace_InitUnk`
  occlusion/whoosh already use — and estimate the enclosure: escape-to-sky fraction (openness), mean
  hit distance (size), ceiling height, nearest-surface delay. Drive the EFX reverb params
  continuously from those (decay ∝ size, wet/room-gain ∝ enclosure, HF damping, reflections delay ∝
  nearest wall), smoothed to avoid zipper/pop. The Phase 2 preset table becomes the seed/fallback.
  **Acceptance:** driving a single track, the reverb opens up in the open and tightens in a
  tunnel/canyon without a track change; presets no longer needed for variety within a level.
- **Tier B — true physically-based propagation (Steam Audio). Effort: L+.** The genuine article:
  ray-traced early reflections, geometry occlusion, and convolution reverb from the actual scene mesh
  + surface materials. Integrate Valve's free SDK — export the collision geometry into its scene,
  assign materials (ties to the terrain surface-tag seam, `MODES_MODIFIERS_ROADMAP.md`), run its
  simulation, feed the result through / in place of the OpenAL spatial path. This is the roadmap's
  long-standing Tier-3 idea (also referenced from Phase 3's occlusion). Big dependency + mesh-export
  pipeline; evaluate cost/benefit against Tier A once Tier A is heard.
- **Sequencing:** do Tier A first (most of the "this level breathes" win, reuses the existing raycast
  + EFX, no heavy dependency); adopt Tier B only if Tier A's estimated reverb isn't convincing.

---

## 5. Cross-cutting concerns
- **`AUDIO_REPLACEMENT=OFF` must build and run** exactly as today (mirror the `RENDERER_REPLACEMENT`
  discipline — see [[renderer-replacement-toggle]]).
- **Faithful default.** Shipped default = A3D backend, byte-identical. Modern audio is a toggle,
  never silent. Ties to `VERIFICATION_ROADMAP.md`.
- **Two engines.** Any "master volume" / mute / focus-mute behavior must cover **both** the swrSound
  (A3D/OpenAL) path and the SMUSH cutscene path (§1.4).
- **Streaming model mismatch.** A3D uses Lock/Unlock on a circular buffer with position-notify
  events; OpenAL uses buffer queueing. The streaming backend must translate cleanly (the double-
  buffer refill math in `swrSound_UpdateStreaming` is the reference).
- **Coordinate handedness.** Reconfirm against `SetCoordinateSystem(0)` and the `(x, z, -y)`
  convention before trusting positional parity.
- **Licensing.** OpenAL Soft is LGPL — dynamic link / ship the DLL alongside `dinput.dll`
  (consistent with how the renderer ships). Steam Audio is a separate free SDK (evaluate at Tier 3).
- **ImGui home + persistence.** Toggles (backend select, HRTF, reverb, occlusion, per-category
  volumes) live in the audio Settings bucket per `DEBUG_UI_ROADMAP`; persist with the existing
  settings (`SW_RACER_RE.ini`), building on the #221 sliders.
- **Perf budget.** Audio is a background thread; keep EFX/raycast cost off the main loop. Surface a
  cost readout as with the graphics chain.

---

## 6. Anticipated roadblocks
- **RB1 — streaming translation.** Lock/Unlock ↔ buffer-queue is the highest-risk mechanical port;
  underruns/loop seams are audible. Port + runtime-verify streaming *before* building reverb on it.
- **RB2 — positional/handedness drift.** A subtle axis flip is easy to miss and hard to A/B; build a
  side-by-side A3D-vs-OpenAL toggle early for direct comparison.
- **RB3 — SMUSH is a separate stack.** Unifying master volume touches DirectSound, not just the
  backend; the cutscene path can't be forgotten (it's the loudest #221 complaint).
- **RB4 — reverb preset authoring.** Mapping tracks→presets is content work; the terrain-behavior
  seam (`MODES_MODIFIERS_ROADMAP.md` §5) may be a cleaner key than per-track hardcoding.
- **RB5 — occlusion cost.** Naive per-source raycasts every frame will spike; must be budgeted /
  amortized (RB shared with the collision-mesh consumers).
- **RB6 — the two remaining stubs.** `swrSound_Startup` and `swrSound_PlaySpatial` should get real
  bodies (or explicit delta handling) before/along with Phase 0, so the abstraction wraps a complete
  API rather than a stub. `PlaySpatial` is the positional dispatch the backend most needs.
- **RB7 — address drift.** All `_ADDR`s below must be reconfirmed against the live Ghidra DB at
  implementation time (encrypted `.text`).

---

## 7. Phasing summary

| Phase | Scope | Effort | Risk | Depends |
|-------|-------|--------|------|---------|
| **0** | `AudioBackend` interface + leaf-wrapper delta hooks + A3D pass-through backend + unified volume; `AUDIO_REPLACEMENT` flag (**[SCOPED]** §4) | M | low | — |
| **1** | OpenAL Soft backend: full parity + HRTF | L | med (RB1/RB2) | 0 |
| **2** | EFX environmental reverb; revive REVERB/REFLECTIONS; track→preset | M | med (RB4) | 1 |
| **3** | Collision-mesh occlusion (opt. Steam Audio propagation) | L | med (RB5) | 1 |
| **4** | Modern-format asset decode (Ogg/FLAC/hi-rate) via the loose-file loader | M | low | 1 |
| **5** | Master DSP chain + per-category submix | M | low | 1 |
| **6** | Proximity "whoosh" speed cue — sonify the nearest wall via the collision mesh (stretch, enhancement) | M | med (RB5/tuning) | 1, 3 |
| **7A** | Geometry-*estimated* reverb — probe-rays drive EFX reverb params continuously (retires the per-track presets; reverb changes *within* a level) | M | med (tuning) | 2, 3 |
| **7B** | True physically-based propagation — Steam Audio (ray-traced reflections + occlusion + convolution reverb from the mesh) | L+ | high (integration) | 1, 3 |

**Recommended first action:** Phase 0 — the abstraction + A3D pass-through — because it proves the
seam is transparent (byte-identical default) at zero audible risk, and everything else plugs into
it. Then Phase 1 with an A3D↔OpenAL A/B toggle wired from the start (de-risks RB2), leading with
HRTF as the first tangible "why bother" win.

---

## 8. RE-grounded reference map

### 8.1 The backend seam (leaf A3D wrappers → `IAudioBackend`)
| Symbol | Addr | Role |
|--------|------|------|
| `swrSound_Init` | `0x004848a0` | A3D COM bring-up (device/listener); backend init point |
| `swrSound_Shutdown` | `0x00484a20` | A3D teardown; backend shutdown |
| `swrSound_SetOutputGain` | `0x00484a80` | master gain (the unified-volume anchor) |
| `swrSound_NewSource` / `DuplicateSource` | `0x00484aa0` / `0x00484bb0` | source create / polyphony clone |
| `swrSound_Play` / `Rewind` / `ReleaseSource` | `0x00484be0` / `0x00485070` / `0x004850a0` | source lifecycle |
| `swrSound_SetPosition` / `SetVelocityClamped` | `0x00484e10` / `0x00484e40` | per-source 3D `(x,z,-y)` + velocity |
| `swrSound_SetGain` / `SetPitch` / `SetMinMaxDistance` | `0x00484d90` / `0x00484dd0` / `0x00484df0` | per-source params |
| `swrSound_SetTransforms` / `SetVelocity` | `0x00484f40` / `0x00484f10` | **listener** transform/velocity |
| `swrSound_SetDistanceModelScale` | `0x00484fb0` | rolloff |
| `swrSound_SetRenderMode` / `GetRenderMode` | `0x00485020` / `0x00485040` | 2D-fallback gate (`0x20`) |
| `swrSound_WriteLocked` / `UnlockSource` | `0x00485110` / `0x00485170` | streaming buffer fill (→ OpenAL queue) |
| `swrSound_ParseWave` | `0x004851a0` | RIFF/WAVE PCM parse |

### 8.2 Engine-agnostic layer (stays put, above the seam)
| Symbol | Addr | Role |
|--------|------|------|
| `swrSound_Update` | `0x00449ef0` | per-frame 8-channel mixer (position/velocity/gain/pitch/pan + listener + Flush) |
| `swrSound_PlaySpatial` | `0x00426d80` | **STUB** — positional SFX dispatch (RB6; backend needs it) |
| `swrSound_PlaySfxThrottled` / `PlayRandomSfx` | `0x00427410` / `0x00427590` | SFX gating |
| `swrSound_ResolveSfxId` | `0x00427110` | (category,id) → bank index (fully reversed) |
| `swrSound_UpdateMusic` / `SelectTrackMusic` / `SetMusicFade` | `0x00427880` / `0x00427ea0` / `0x004277f0` | streamed-music controller |
| `swrSound_Startup` | `0x00421d90` | **STUB** — high-level bring-up + `SetOutputGain(1.0)` tail (#221) |

### 8.3 Assets, config, cutscenes
| Symbol | Addr / ref | Role |
|--------|-----------|------|
| `swrSound_CreateSourceFromFile` | `0x00423050` | **the modding asset seam** (Phase 4) |
| `swrSound_LoadSoundMap` / `LoadDefaultBank` | `0x00421f30` / `0x00422060` | `data/sounds.map` registry |
| `swrConfig_WriteAudioConfig` / `ReadAudioConfig` | `swrConfig.c:363` / `:460` | `audio.cfg` (SYS/HIRES/3D/DOPPLER/REFLECTIONS/GAINMATCH/VOICE/MUSIC/REVERB) |
| `Window_SmushPlayCallback_delta` | `Window_delta.c:289` | SMUSH cutscene frame delta (video today; audio-unify seam) |
| `renderer_drawSmushFrame` | `renderer_utils.cpp:258` | SMUSH frame draw |

### 8.4 A3D interface + globals
| Symbol | Ref | Role |
|--------|-----|------|
| `IA3d4` / `IA3dSource` / `IA3dListener` vtables | `src/types_a3d.h` | the COM surface (incl. **unused** `SetReflection*`/`GetOcclusionFactor`/`SetHFAbsorbFactor`) |
| `A3D_1ST_REFLECTIONS/OCCLUSIONS/REVERB/GEOMETRIC_REVERB` | `src/types_a3d.h:10-16` | dormant feature flags (§1.3) |
| `Sound_enabled_3d` | `0x0050d550` | 3D-vs-2D-fallback gate |
| `Main_hiRes_sound` | `0x004b6d14` | 22K vs 11K asset select |
| `sound_sfx_volume` / `sound_music_volume` | `0x00e364a5` / `0x00e364a6` | per-category volume bytes (save image) |

---

## 9. Cross-references
- Memory: [[swrsound-subsystem]] (the reimplemented API), [[audio-issues-221]] (#221 volume/persistence,
  PR #223), [[todo_ghidra_restart_sound]] (remaining stubs / play-func history),
  [[asset-replacement-architecture]] (loose-file loader = Phase 4 seam),
  [[collision-viewer-feature]] (occlusion raycast source = Phase 3).
- Roadmaps: `GRAPHICS_ROADMAP.md` (same faithfulness-first, opt-in, delta-layer pattern;
  `RENDERER_REPLACEMENT` precedent), `MODDING_ARCHITECTURE.md` / `MODDING_API_ROADMAP.md` (asset +
  reverb-preset modding), `MODES_MODIFIERS_ROADMAP.md` (terrain-behavior seam as a reverb key),
  `VERIFICATION_ROADMAP.md` (byte-faithful default), `DEBUG_UI_ROADMAP.md` (audio settings bucket).
- External: [OpenAL Soft](https://openal-soft.org/) · [DSOAL (DirectSound→OpenAL precedent)](https://github.com/kcat/dsoal)
  · [Aureal A3D SDK](https://github.com/sigmaco/a3d-sdk) · [A3D 3.0 API Reference](https://www.worknd.ru/a3d30ref.pdf)
  · [Steam Audio](https://valvesoftware.github.io/steam-audio/).
