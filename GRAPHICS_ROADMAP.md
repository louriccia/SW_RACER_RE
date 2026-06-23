# SW_RACER_RE -- Graphics / Post-Processing Roadmap

**Status:** design (2026-06-19). Living document. Owner: lightningpirate.

Goal: layer modern real-time graphics onto the OpenGL replacement renderer -- HDR +
post-processing, bloom, speed-reactive motion blur, dynamic shadows, color grading -- **without
changing the faithful N64/PC look at default settings**. Every effect rides a single
post-processing **resolve pass** that becomes the renderer's new backbone; each later effect is an
incremental pass on that chain rather than its own plumbing project.

Lives in the `dinput_hook/` Detours layer (the OpenGL-replacement renderer): `renderer_hook.cpp`,
`renderer_utils.cpp/.h`, the GLSL under `assets/shaders/`, and the ImGui controls in
`imgui_utils.cpp/.h`. It does NOT touch `src/` reimpls. The whole chain only exists in the GL
takeover build -- with `-DRENDERER_REPLACEMENT=OFF` none of this compiles in (see Cross-cutting).

> Effort tags: **S** < ~half day, **M** ~1-2 sessions, **L** multi-session. File line numbers are
> as of 2026-06-19 and must be reconfirmed at implementation time.

---

## 0. Decisions locked (2026-06-19)

- **The HDR resolve pass is the foundation (Phase 1).** Today the scene renders into an offscreen
  MSAA FBO and is blitted straight to the backbuffer -- there is no post stage at all. Phase 1
  inserts `MSAA HDR FBO -> resolve -> fullscreen post pass -> backbuffer`. Bloom, motion blur,
  grading and tonemapping are then incremental passes on that chain. Nothing else starts until
  this lands.
- **Faithfulness-first default.** At neutral settings (exposure 1.0, tonemap = passthrough, every
  effect off) the frame must be **byte-identical to today**. That identity is the Phase 1
  acceptance test and the standing contract for every later phase: new looks are opt-in toggles,
  never silent defaults. A "Classic" preset pins all of them off.
- **Color-space split is the central constraint.** The two shader paths live in different spaces:
  `n64_shader.frag` (the whole world) outputs **display-referred sRGB** (raw vertex colors /
  textures, no linearization); `pbrShader.frag` (HD pods/tracks/env) works in **linear** and
  applies gamma at the end. A single filmic tonemap over the combined frame would double-darken
  the world path. Therefore: Phase 1 ships an HDR buffer + exposure but **tonemap defaults to
  passthrough**; true filmic tonemapping requires first linearizing the world path, which is its
  own opt-in phase (Phase 7), never bundled into the scaffold.
- **Per-viewport, splitscreen-correct.** Post runs per viewport (each half can get its own
  exposure later), and the post shader samples by `texelFetch(tex, ivec2(gl_FragCoord.xy), 0)`
  -- absolute window pixels -- so the existing sub-rect layout works with no UV-remap and no bleed
  across the split.
- **Pods already have real lighting.** The IBL/PBR path means pods/HD assets are already
  dynamically lit. The user-facing "real lighting instead of baked" ask is really (a) **dynamic
  shadows** (Phase 5) and (b) **relighting the baked world path** (Phase 7) -- not a toggle, a
  content/shader project. Set expectations accordingly.
- **Controls home in the graphics panel** and follow `DEBUG_UI_ROADMAP` (player-facing Settings
  bucket); do not grow a second ad-hoc block in `opengl_render_imgui`.

---

## 1. Current state (what the GL renderer already gives us)

Confirmed by reading the pipeline 2026-06-19:

- **Offscreen MSAA FBO**, configurable sample count (`swrViewport_Render_Hook`,
  `renderer_hook.cpp:882`). Color is **`GL_RGBA8` (LDR)**, depth `DEPTH_COMPONENT32`, both
  multisample.
- **Direct resolve to backbuffer** via one `glBlitFramebuffer` (color+depth) per viewport
  (`renderer_hook.cpp:1080`). **This blit is the seam Phase 1 replaces.**
- **World path = N64 color-combiner emulation** (`assets/shaders/n64_shader.frag`): faithful RDP
  combiner, one directional light + ambient (Gouraud, only where verts carry normals,
  `type & 0x11`), linear fog. Output is display-referred. Most world geometry is **baked vertex
  color**, no normals.
- **HD path = PBR + IBL** (`assets/shaders/pbrShader.frag`) lit by a **live environment cubemap**
  (`setupIBL`, one cube face refreshed per frame), with normal/occlusion/metallic-roughness +
  lightmap support. Tonemap is bare gamma. Used for pods, HD track/env replacements.
- **Already on:** MSAA, anisotropic filtering + trilinear mipmaps (game textures
  `std3D_delta.cpp:306`, loose textures `texture_replacement.cpp:54`), skybox, mouse-picking.
- **Reusable scaffolding:** `assets/shaders/fullscreen.vert` (fullscreen triangle from
  `gl_VertexID`, empty VAO), `compileProgram` + `readFileAsString` (`shaders_utils.cpp`), and the
  `fullScreenTextureShader` / `renderer_drawSmushFrame` pattern (`renderer_utils.cpp:152`) to copy
  for new full-screen passes.

**Gaps that this roadmap closes:** LDR buffer (blocks bloom/exposure), no post chain, no bloom, no
motion blur, no dynamic shadows, no SSAO, no color grading, no render-scale control, bare-gamma
tonemap.

---

## 2. Target architecture -- the resolve-pass backbone

```
per viewport (swrViewport_Render_Hook):

  scene graph  ->  [ MSAA FBO, RGBA16F color + depth ]      (Phase 1: RGBA8 -> RGBA16F)
                          |
                          |  glBlitFramebuffer (color, sub-rect)   <- MSAA resolve
                          v
                   [ resolve FBO, single-sample RGBA16F ]
                          |
            (Phase 2 bloom reads here -> bloom mip chain -> additive)
            (Phase 5 shadow map sampled earlier, in n64/pbr shaders)
                          |
                          v
              fullscreen post pass  (assets/shaders/post.frag)
              exposure * color -> [grade] -> [motion blur] -> [tonemap] -> [vignette/CA/grain]
                          |
                          v
                   backbuffer sub-rect (glViewport)        + depth blit preserved for HUD/lensflare
```

The post pass is one program with `#define`/uniform-gated stages so effects compose without a
combinatorial shader explosion. Bloom and shadow map are the only passes that need their own FBOs;
everything else is a branch inside `post.frag`.

---

## 3. Phases

### Phase 1 -- HDR resolve pass + exposure (the backbone). Effort: M  [SCOPED]

The thing every other phase hangs off. Detailed scope agreed 2026-06-19:

- **FBO color `GL_RGBA8` -> `GL_RGBA16F`** (`renderer_hook.cpp:912`).
- **New globals** `resolve_framebuffer` + `resolve_color_tex` (single-sample RGBA16F) beside the
  existing framebuffer globals (`renderer_hook.cpp:837`); (re)created in the same size/MSAA-change
  block (`:882`).
- **Replace the end-of-hook blit** (`:1080-1088`) with: (1) resolve MSAA color -> resolve FBO
  (sub-rect); (2) blit depth MSAA -> backbuffer (preserve, later passes read it); (3)
  `renderer_runPostProcess(resolve_color_tex, exposure, tonemap_mode)` -> backbuffer sub-rect.
- **New shader `assets/shaders/post.frag`** (+ reuse `fullscreen.vert`): `texelFetch` HDR,
  `* exposure`, `tonemapMode` switch (0 passthrough / 1 Reinhard / 2 ACES), default 0. **No sRGB
  encode** (content already display-referred).
- **New helper** `renderer_runPostProcess` + `postProcessShader` struct in `renderer_utils.cpp/.h`,
  mirroring `get_or_compile_fullscreenTextureShader`.
- **ImGui:** `post_exposure` (float, 1.0) + `post_tonemap_mode` (int, 0) on `ImGuiState`, slider +
  combo in the graphics panel.
- **Leave `pbrShader.frag` as-is** (keeps its gamma/tonemap; buffer stays display-referred until
  Phase 7).

Risks: asset deploy (new shader must be hand-synced into the game `assets/shaders/` -- CMake does
not copy shaders, it will `abort()` in `readFileAsString` if missing); depth blit kept
deliberately until confirmed dead.

**Acceptance:** at `exposure=1, tonemap=passthrough` the image is identical to today; pushing
exposure to ~1.5 visibly brightens -> proves the pass is live and transparent at neutral.

### Phase 2 -- Bloom. Effort: M.  Depends: Phase 1.

The single biggest "wow per line" on a game wall-to-wall with engine glow, boost flames, Sebulba's
flamejet, lava tracks, sun glints.

- Approach: dual-filter / progressive-downsample-then-upsample bloom mip chain reading the
  resolve HDR texture; additive composite in `post.frag`.
- To get glow with **zero change to the base look**, push *emissive* sources above 1.0 rather than
  raising overall brightness: boost `EmissiveFactor` in the PBR path, and key the n64 path's known
  bright materials (engine/lava -- by primitive color / model id) to store >1.0. Bloom threshold
  at 1.0 then picks up only those; everything else is untouched.
- Splitscreen: clamp downsample taps to the viewport sub-rect to avoid bleed across the split.
- Controls: bloom on/off, threshold, intensity.

**Acceptance:** engines/lava glow; non-emissive scene unchanged vs Phase 1; no bleed across the
splitscreen divider.

### Phase 3 -- Speed-reactive motion blur. Effort: S (radial) then M (velocity).  Depends: Phase 1.

*The* genre effect for a podracer; ties directly to gameplay feel.

- **3a (S) -- radial speed blur:** blur radially from screen center, intensity scaled by pod
  speed/boost (already available via `currentPlayer_Test`). Pure `post.frag` branch, no extra
  buffers. Ship this first.
- **3b (M) -- velocity motion blur:** reproject with the previous frame's view-proj, sample along
  screen-space velocity (depth-derived). More correct, needs prev-matrix plumbing + the depth we
  already keep.
- Controls: motion blur off / radial / velocity, strength.

**Acceptance:** strong sensation of speed under boost, scales with velocity, off = identical to
Phase 1/2.

### Phase 4 -- Color grading + framing FX. Effort: S.  Depends: Phase 1.

- Per-environment grade (LUT or lift/gamma/gain) -- warm on lava, cool on ice tracks -- the
  original art reads flat; mood for almost free.
- Subtle vignette + optional chromatic aberration + film grain, used sparingly.
- All branches inside `post.frag`; per-track grade selection can hook the track-load path.
- Controls: grade preset / strength, vignette, CA, grain (all default off/neutral).

### Phase 5 -- Sun + pod shadow map. Effort: L.  Depends: Phase 1.

The closest achievable thing to the "real lighting" ask: the pod currently casts **no shadow on
the track at all**.

- Depth-only pass from `lightDirection1` (the sun dir already exists), sampled in **both**
  `n64_shader.frag` and `pbrShader.frag` as a multiplicative shadow term over the existing
  (baked + IBL) lighting -- additive on top of the art, not a replacement.
- Start with pod + key dynamic geometry casters; single map, then cascades if range demands.
- Fallback / cheap interim: blob shadow under each pod.
- Risk: shadow acne/peter-panning tuning; cost of the extra pass (see perf budget).

**Acceptance:** pods drop a believable shadow on the track that tracks the sun; baked world look
otherwise unchanged.

### Phase 6 -- Render-scale / supersampling slider. Effort: S.  Depends: Phase 1.

Scene already lives in an FBO, so rendering at >native and downsampling in the resolve is a few
lines -- the single biggest pure image-quality win where GPU headroom exists. Also enables a
downscale option for weak GPUs.

- Controls: render-scale slider (e.g. 0.5x-2.0x); resolve/post already handle arbitrary FBO size.

### Phase 7 -- Linear world path + true filmic + SSAO + heat haze. Effort: L. Stretch / opt-in.

The contentious, faithfulness-affecting work -- explicitly opt-in, behind an "Enhanced lighting"
mode, never default.

- **Linearize the n64 world path** (linearize texture + vertex color samples, output linear) so
  the whole frame is linear HDR and a single filmic tonemap (move sRGB encode into `post.frag`,
  flip `tonemapMode` default) is correct. Changes the authored look -> needs playtesting and a
  faithful-vs-enhanced toggle.
- **SSAO/GTAO:** depth-derived normals (the world path has no normal G-buffer) for contact shadows
  in crevices / under the pod. Lower fidelity than a true normal buffer but cheap.
- **Heat-haze / screen-space refraction** on engine exhaust + flamejets -- very podracer.

---

## 4. Cross-cutting concerns

- **Asset deployment.** New `.frag`/`.vert` files must be **hand-synced** into the game's
  `assets/shaders/` -- CMake does not copy shaders. A missing file `abort()`s on first frame in
  `readFileAsString`. (Same hazard noted in the blue-flash work.)
- **ImGui home.** All toggles live in the graphics Settings bucket per `DEBUG_UI_ROADMAP`; persist
  them with the other graphics settings rather than spawning a parallel block.
- **Perf budget.** Race is already CPU/draw-bound with the HD-pod glTF path ~12.5 ms/frame (see
  renderer-perf investigation). Each post pass adds GPU cost; budget per effect, keep the neutral
  path to ~one extra resolve blit + one fullscreen pass, and surface a cost readout.
- **Splitscreen correctness.** `gl_FragCoord` sampling + sub-rect viewport is the standing rule;
  any pass that samples neighbors (bloom, blur, SSAO) must clamp to the sub-rect.
- **`RENDERER_REPLACEMENT=OFF`.** This whole chain is GL-takeover only; guard so the off-build
  (native dgVoodoo, gameplay deltas only) still compiles and runs.
- **IBL cubemap untouched.** `envInfos.ibl_framebuffer` and the env-capture pass in
  `debug_render_mesh` are a separate pipeline; Phases 1-6 do not touch them.
- **Shared milestones.** Depth-of-field + photo-mode framing belong with `CAMERA_ROADMAP` /
  `REPLAY_ROADMAP` (replay + photo mode), not here -- this roadmap provides the post chain they
  plug DoF into.

---

## 5. Suggested order

1. **Phase 1** (backbone) -- nothing else starts first.
2. **Phase 3a** (radial speed blur) -- cheapest high-impact, proves the chain end-to-end.
3. **Phase 2** (bloom) -- biggest visual payoff.
4. **Phase 4** (grading/vignette) -- cheap mood, rounds out the "modern" feel.
5. **Phase 6** (render-scale) -- drop-in IQ win.
6. **Phase 5** (shadows) -- the headline "real lighting" feature.
7. **Phase 3b / Phase 7** -- velocity blur, then the opt-in enhanced-lighting work.

Each phase ships behind its own toggle, defaulting off/neutral, with the byte-identical-at-neutral
contract held throughout.
