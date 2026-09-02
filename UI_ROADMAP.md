# SW_RACER_RE -- UI System Roadmap

**Status:** design (2026-06-17). Living document. Owner: lightningpirate.

Goal: a **resolution-independent 2D UI** for SWE1R -- the menu/HUD widget layer lays out
responsively for any aspect ratio (16:9 and beyond) and is drawn from **high-resolution art**,
with **no hardcoded 640x480 assumptions left in the layout math**.

Lives in the `dinput_hook/` Detours layer (delta reimpls), **not** `src/`. The faithful
`src/Swr/swrUI` + `swrSprite` decomp stays a separate track; this roadmap reimplements the
geometry/layout path as deltas on top of it.

> Addresses below are from the Ghidra DB and MUST be reconfirmed against the live DB at
> implementation time (Steam `.text` is SteamStub-encrypted on disk; live values via Cheat
> Engine per the runtime-verify workflow). Effort tags: **S** < ~half day, **M** ~1-2 sessions,
> **L** multi-session.

---

## 0. Decisions locked (2026-06-17)

- **North star: resolution-independent UI.** Strip every hardcoded 640/480 decision out of the
  layout path. The working coordinate space becomes the **actual framebuffer**, and layout is
  expressed as **anchor + pixel-offset evaluated against the live resolution**, not absolute
  640-space constants.
- **Representation: physical-pixel, integer storage (NOT normalized float).** Keep the native
  `swrUI_unk` / `swrSprite` int+short geometry fields exactly as they are and **reinterpret them
  as physical pixels**. No struct-type surgery, no parallel float model. This achieves 100% of
  "rid of 640/480" and full responsiveness while avoiding rewriting every native consumer of the
  geometry fields. (Normalized-float was considered and rejected: same payoff, much larger
  surface and risk.)
- **Three independent axes.** Position (where, anchor-driven), footprint (how big, logical),
  texel density (how sharp, the GL texture). HD art touches ONLY texel density. Keep them
  decoupled -- the engine already does.
- **Effects are screen-space and stay full-screen.** Weather, lens flare, world-projected HUD
  text derive their positions from the live viewport projection, not the 640x480 design grid.
  They are not part of the re-anchoring work. See SS3.
- **Centering / pillarbox is ABANDONED.** It bolted an X offset onto the draw side while the
  cursor mapping stayed device-relative, guaranteeing draw/click desync. The Phase-2 spike on it
  was unproductive. Resolution-independent layout supersedes it. See SS9.
- **Migration behind a legacy shim.** Until every builder is reimplemented, un-reimplemented
  native builders still emit 0..639 coords; a fallback transform scales those up so the UI stays
  coherent screen-by-screen during the page-by-page conversion. See SS6.

---

## 1. The corrected mental model (what this session established)

The old `ghidra_analysis/ui_system_notes.md` roadmap chased two dead ends: a "draw-time
hit-test bbox writer" that does not exist, and global pillarbox centering. Both are wrong. The
real pipeline, traced end to end:

### 1a. Two coordinate domains, not one

| Domain | Examples | Position source | Action |
|--------|----------|-----------------|--------|
| **Screen-space** | 3D world, weather, lens flare, world-projected HUD text | `swrViewport_ProjectToScreen` vs live `screen_width/height` | Leave full-screen. Already correct. |
| **Design-space widgets** | menus, HUD gauges/counters, cursor | authored 640x480 constants in the builders | Re-author resolution-independently. |

The earlier "transform the whole 2D layer" framing was wrong precisely because it lumped these
together. Only the design-space widgets get touched.

### 1b. The widget geometry pipeline (fully mapped)

- `swrUI_HitTest` (0x4150e0) reads the element's **stored rect** (`x/y/width/height`, struct
  offsets 0x24-0x30) directly, clips it to `bbox` (0x4e0) via `swrSprite_BBoxFit` (0x417f00),
  and does a plain point-in-rect (`swrSprite_IsInsideBBox`, 0x4172c0) against the cursor.
  **There is no separate draw-time px-bbox.** The "unk00_7..10 writer" in the old notes was a
  phantom.
- The rect is written by `swrUI_SetPos` (0x414b60) / `swrUI_SetSize` (0x414b40) /
  `swrUI_SetUnk` (=SetBBox, 0x415810) -> `swrUI_OnSetElementPos` (0x416f50) /
  `swrUI_OnSetElementSize`. **Zero scaling anywhere** -- the public setters just forward to the
  message handlers, which store the values raw. The values are literal 640x480 constants emitted
  by the page builders (`swrUI_BuildMenuPages`, 0x411e10, + per-page sub-builders).
- The cursor enters via `swrUI_ProcessMouse` (0x415400) -> `swrUI_UpdateMouseState` (0x4083d0) ->
  `stdConsole_GetCursorPos` (0x4082e0). Vanilla returns raw OS px (only a hardcoded
  `screen_width == 0x200` branch applies a 1.25x correction for 512-wide mode).

### 1c. `screen_width` (0x00ec86c4) == `swrDisplay_screenWidth`; `screen_height` (0x00ec85e8) ==
`swrDisplay_screenHeight`. Same globals = the real framebuffer size. `GetUIScale`
(`swrSprite_GetUIScale`, 0x44f640) reads them to produce the 2D scale.

### 1d. The render decoupling (footprint vs texel density) -- already in the engine

From the sprite draw math (e.g. `dinput_hook/renderer_hook.cpp:678` and `swrSprite_Draw2`
0x428030 / `swrSprite_Draw1` 0x44f670):

```
footprint  = sprite->width(scale, default 1.0) * texture->header.width(logical texels)
uv_scale   = page.width(logical) / material->aTextures->ddsd.dwWidth(PHYSICAL GL texture)
```

`swrSprite_SetDim` (0x4286f0) sets `sprite->width/height` -- a **float scale multiplier**, not
absolute pixels. So the on-screen footprint is driven by the **logical** header dims, while UV
sampling is computed against the **physical** uploaded texture size. **Resolution is a free
variable.** The universal GL upload `std3D_AllocSystemTexture_delta`
(`dinput_hook/game_deltas/std3D_delta.cpp:274`) sets `ddsd.dwWidth` from whatever source pixels
it is handed.

---

## 2. Target architecture (physical-pixel resolution-independent UI)

Make logical space == the framebuffer (1 logical unit = 1 physical pixel). Then:

| Stage | Today (640x480) | Target (physical) |
|-------|-----------------|-------------------|
| `GetUIScale` | xscale=W/640 (stretched) or H/480 (Phase 1 uniform) | **identity (1.0)** -- positions already in screen space |
| Cursor map | window px -> 640x480 (stretched, see SS4) | **identity** -- cursor px == position px |
| Hit-test | design rect vs cursor | **unchanged** (px vs px) |
| Widget rects (int) | 640-space constants | **physical px** from `anchor + offset` vs live res |
| Footprint | header texels * scale | **unchanged** (scale set so footprint = desired px) |
| Texel density | original DDS | **HD DDS** (independent, see SS7) |

Why int storage is fine: the granularity worry (SS5) only existed because 1 logical unit = 480th
of the screen. With logical == physical, 1 unit = 1 px. No struct change, no quantization.

The ONLY math change at the engine layer is collapsing two transforms to identity; **all the real
work moves into the builders** (SS6), which now compute physical positions from anchors.

---

## 3. The coordinate-space map (AUTHORITATIVE -- verified in Ghidra 2026-06-24)

> **DOMAIN CORRECTION (2026-06-25, from live playtest of the GetUIScale+cursor wiring).** `GetUIScale`
> is NOT the universal 2D-UI scale -- it is the **`swrSprite_array` scale only**. Its sole caller
> `swrSprite_DrawSprites` is reached only from `swrPlayerHUD_RenderAllViewports`, which `swrMain_RunFrame`
> calls EVERY frame (front-end + race). So `GetUIScale` governs: the in-race HUD, the cursor sprite,
> weather, lens flare, and front-end **array** sprites (e.g. the menu background). It does NOT govern
> the **menu widgets**: those live in the `swrUI` element tree (`swrUI_unk.ui_elements[]`, populated by
> `swrUI_AddSprite` from the element Procs / `swrUI_BuildPanelFrame`) and are drawn by a SEPARATE swrUI
> render (scale source still to be located), with their TEXT going through the text recip
> (`swrUI_DrawText` -> `swrText_CreateTextEntry1` -> `Add2DQuad2`). So there are THREE 2D draw domains:
> (1) `swrSprite_array` via `GetUIScale`; (2) the `swrUI` element tree; (3) text. **Menus = (2)+(3).**
> Consequence: pairing `GetUIScale` (domain 1) with the cursor was wrong for menus -- it un-stretched
> the background (domain 1) and the cursor, but left the widgets (domain 2) + text (domain 3) stretched,
> so widget clicks desync from their visuals. The menu's coupled set is **{element-tree render scale +
> text recip + cursor}**, NOT GetUIScale. NEXT: locate the swrUI element-tree sprite render and its
> scale (the menu's true "GetUIScale"). The table/seams below remain valid for the in-race/projected
> (domain 1) layer.
>
> **RESOLVED (2026-06-25, located the render):** `swrUI_RenderTree` (0x415020) ->
> `swrUI_RenderElementSprites` (0x4151f0), run every frame, COPIES each element's `ui_elements[]`
> sprites into the global `swrSprite_array` (`swrSprite_NewSprite`/`SetPos`/`SetDim` at DESIGN coords),
> which are then drawn by `swrSprite_DrawSprites` -> `GetUIScale`. So menu FRAMES/BUTTONS DO ride
> `GetUIScale` after all (the "separate domain" claim above was wrong: there are really TWO domains for
> menus -- sprites via `GetUIScale`, and TEXT via the text recip). Consequence for the playtest bug:
> the `23ce185` pair (GetUIScale + cursor) actually made frame + hit-rect + cursor mutually consistent;
> the "clickable but wrong spot" was the TEXT staying on the stretched recip, so labels drew offset
> from their own (now-uniform) frames. The menu's coupled TRIPLE is **{GetUIScale (frames) + text recip
> (labels) + cursor (hit)}** -- GetUIScale IS in it. `GetUIScale` is still ALSO the in-race HUD scale,
> so making it uniform additionally affects the HUD + the projected seams (deferred, gated).

This SUPERSEDES the earlier "screen-space effects are out of scope" framing, which was WRONG. Every
2D system was traced first-hand; the model below is ground truth. The headline correction: the
3D-projected systems (weather, lens flare, opponent markers, world HUD text) are NOT an independent
"already-correct" screen space you can ignore -- they enter the SAME 640x480 design space through
conversion SEAMS, and are coupled to the draw scale. Only the pure-3D viewport
(`swrViewport_ComputeScreenRect` 0x4830e0) is genuinely separate and keeps real width.

There is ONE storage space (640x480 design) and effectively TWO scales (sprite + text) mapping it to
the framebuffer. The Phase-1 revert happened because the draw scale was changed without mirroring
the spaces coupled to it.

### Flow
```
World 3D --ProjectToScreen(0x42b710)--> real px --SetPosF/CreateTextEntry2 (/scale, SEAM)--+
                                                                                           v
OS cursor --GetCursorPos_delta (window px -> design)-------> [cursor in design] ...        |
                                                                                           v
                          +============== DESIGN SPACE  640x480 ==============+ <-----------+
                          |   sprite positions  /  widget hit-rects  /  text  |
                          +==================================================+
                              |                    |                   |
            Draw2 x GetUIScale|     Add2DQuad2      |    HitTest (rect vs cursor,
            (0x44f640)        |     x textScale     |    NO scale -- passive)
                  v           v                     v
            +-------------------- FRAMEBUFFER (real px) --------------------+
   (glyph UV is a fixed 64x128 page space, resolution-independent: SDF + HD fonts live there)
```

### The spaces (stored unit -> transform -> owner)
| Space | Stored as | Transform to framebuffer | Owner fn | Notes |
|-------|-----------|--------------------------|----------|-------|
| Sprite | design-px short (x/y); width/height = float SCALE | x GetUIScale (W/640 x H/480, stretched) | swrSprite_Draw2 0x428030 via swrSprite_DrawSprites 0x4283b0 (SOLE GetUIScale caller) | position AND footprint scaled |
| Text glyph-quad | design units | x textScale, **separate globals** swrText_designWidthRecip/HeightRecip 0x4ac628/0x4ac630, **clamped >= 1.0** | rdProcEntry_Add2DQuad2 0x42d990 | NOT the GetUIScale globals; cannot be identity (sets glyph size) |
| Text glyph-UV | fixed 64x128 page (1/64 @0x4ac644, 1/128 @0x4ac648) | none -- res-independent | Add2DQuad2 | SDF / HD-font lever; decoupled from layout |
| Text clip-rect | screen px (DAT_00e99750..5c) | follows layout | swrText_SetEntryClipRect 0x450310 | must track the layout |
| Cursor | OS raw px (512-wide -> x1.25) | window px -> design (stretched) | stdConsole_GetCursorPos 0x4082e0 + _delta | vanilla = raw px; mod delta remaps |
| Hitbox | design-px rect (raw) | NONE -- compared directly to cursor | swrUI_HitTest 0x4150e0 / swrUI_OnSetElementPos 0x416f50 | PASSIVE; correct only if cursor space == draw scale |

### The seams (real px -> design); each reads screen_width
- `swrSprite_SetPosF` (0x42bb00) -- weather, lens flare, light streaks. (Menus use the integer
  `swrSprite_SetPos` 0x428660, which stores raw design coords, no conversion.)
- `swrText_CreateTextEntry2` (0x42c7a0) -- opponent distance markers, world labels.

WHY projected elements look correct today while menus stretch: projected = real px -> /scale (seam)
-> x scale (draw) = CANCELS. Menus = authored design -> x scale only = STRETCHED. So flipping the
draw scale alone (Phase 1) un-stretches menus but DESYNCS the seams (their /scale no longer matches
the draw's x scale) -- weather/flares/markers shift by the same ~25% the cursor did. The cursor was
just the first symptom of ONE root cause: the draw scale changed without its coupled spaces.

### Consistency contract (the guardrail vs repeating Phase 1)
These all express the same design<->framebuffer relationship through DIFFERENT code and DIFFERENT
globals (one with a >=1 clamp), and MUST move as one atomic unit -- ideally routed through ONE shared
transform definition so half of it can never ship alone:
1. `swrSprite_GetUIScale` (sprite draw)            4. `swrSprite_SetPosF` (projected-sprite seam)
2. `Add2DQuad2` text recips (text draw)            5. `swrText_CreateTextEntry2` (projected-text seam)
3. `stdConsole_GetCursorPos` remap (cursor)        6. clip rect + `swrUI_BuildPanelFrame` !=640 + `swrUI_DrawCaret` 512
Hitbox is passive (no transform of its own) -- it self-corrects once cursor space == draw scale.

### Target (collapse everything onto the framebuffer)
Make design == framebuffer px (identity). Then sprite scale, cursor remap, and BOTH seams flip to
identity together (projection already emits framebuffer px, so the round-trip vanishes and the
projected systems become correct by construction). The ONLY space that stays scaled is text (it sets
glyph size), so the single surviving conversion is "text origin = framebuffer px / textScale" at
exactly two call sites (`CreateTextEntry2` for projected labels; the builder for menu labels). Glyph
UV stays fixed/independent. The first implementation artifact should be that shared transform module
-- before any builder touches a coordinate. See SS6 (consumer audit) and SS11 (text).

---

## 4. The cursor finding (important nuance)

This project ALREADY remaps the cursor out of raw px: `stdConsole_GetCursorPos_delta`
(`dinput_hook/game_deltas/stdConsole_delta.cpp:20`) maps the window cursor into 640x480:

```c
*out_x = x * 640 / w;   // window px -> design 640 (STRETCHED by width)
*out_y = y * 480 / h;
```

But it is a **stretched** inverse (independent X by width). It matched vanilla's stretched DRAW,
so menus were clickable. **Phase 1 changed only the draw to uniform and left the cursor stretched**
-> they no longer agree on X, so under `widescreen_ui` the menu hit-test is offset ~25% at 16:9
(worst at the right edge; likely unnoticed because the current branch drives menus by gamepad).

Under the target model this all collapses: draw scale -> identity, cursor map -> identity. The
existing delta becomes a passthrough (or is removed). Net: the cursor stops being a special case.

---

## 5. The positioning-granularity constraint (resolved by the model)

- Positions are integer (`swrUI_unk.x/y/w/h` int, `swrSprite.x/y` short). Sizes are float
  (`swrSprite.width/height`). So size is continuous; **position snaps to whole logical units.**
- In the old 640x480 model: `1 logical unit = (screenH/480) px` = 2.25px @1080p, 4.5px @4K, and
  N art-texels for Nx HD art. Too coarse to express pixel-precise gaps between separate sprites.
- **The target model dissolves this:** logical == physical, so 1 unit = 1 px. Pixel-precise
  placement is available everywhere; no struct change needed.
- Standing art rule regardless: **composite, do not scatter.** Bake hairlines/gaps/borders INTO a
  single texture (sampled at full HD density) rather than positioning many tiny abutting sprites.
  Shared-edge adjacency is already watertight (two quads at the same edge rasterize seamlessly);
  the only thing the grid ever struggled with was free sub-unit placement of independent pieces.

Orthogonal sampling note: a single sprite's edges can still land on fractional device pixels under
any non-integer effective scale; mitigate with pixel-snap at emit, a 1px transparent guard band in
the art, or clamp sampling. This is a sharpness detail, not a positioning one.

---

## 6. Work plan

### Phase A -- Collapse the transforms + legacy shim (foundation). Effort: M
Make the engine layer resolution-transparent so reimplemented pages can author in physical px while
un-converted pages still render.
- `GetUIScale` delta -> identity (positions already physical).
- Cursor delta -> identity (passthrough).
- **Legacy shim / migration staging (PROPOSED 2026-06-17, for review).** The end-state is identity
  scale + physical-px storage (fine grid). But the draw scale is GLOBAL, so during migration there
  is no clean per-element way to scale un-converted 640-coords without flagging every element. The
  shim is the crux of going fully physical. Recommended staging to avoid blocking on it:
  - **A1 (cheap win, no shim):** keep GetUIScale at the uniform value (screenH/480, already
    shipped) and fix ONLY the cursor to the matching uniform inverse. This makes existing menus
    click-correct AND coherent immediately -- no per-page conversion, no shim. (Fixes the SS4
    desync that Phase 1 introduced.)
  - **A2/B (responsive, still no shim):** convert builders to anchored layout but author in the
    UNIFORM wide-logical space (logicalW = screenW*480/screenH wide x 480 tall). Both converted and
    un-converted elements share one uniform scale, so un-converted 640-coords just sit left-anchored
    and the cursor/hit-test stay consistent for free. Delivers responsive menus at a COARSE (480)
    grid -- acceptable until HD art lands.
  - **Final (fine grid, shim required):** flip to identity scale + physical-px storage when the
    coarse grid actually bites for HD art. THIS is where the shim is needed; defer it until proven
    necessary, by which point most builders are already converted (shrinking the shim's surface).
  This sequences the expensive/invasive step last and delivers click-correct + responsive menus
  early. Net: SS0/SS2's identity-scale model is the END state; uniform-logical is the migration
  vehicle. (Open for your call -- could also commit to physical+shim from the start.)
- Default bbox in `swrUI_New` (0x416d90), currently `(0,0,639,479)`, parameterized to the live
  framebuffer.
- Verify: existing menus still render and (with identity cursor) click-correctly through the shim.

### Phase B -- Reimplement builders responsively, page by page. Effort: L
The bulk. Each page's sub-builder reimplemented to place widgets via `(anchor, pixel-offset)` vs
live resolution instead of 640-space literals.
- **Start with one menu (main menu, page 0x0b) to prove the whole chain end to end** -- including
  click-correctness, which is the gating unknown the whole effort hinges on. **The main-menu
  layout is already fully inventoried with per-widget anchor proposals** in
  `ghidra_analysis/ui_menu_layout_inventory.md` (proof page done 2026-06-17).
- Anchor vocabulary + transform (detailed in the inventory doc SS1): `H={LEFT,CENTER,RIGHT,
  STRETCH}` x `V={TOP,MIDDLE,BOTTOM,STRETCH}` + design offset. Builders compute physical px via a
  single `uniformScale = screenHeight/480` (square -> no stretch) applied to the anchored offset;
  `GetUIScale` stays identity (the scale lives in the builder, not the draw). Migration default =
  PROPORTIONAL (LEFT/TOP at original coords) reproduces today's look exactly at fine grid, then
  upgrade individual widgets to other anchors. The engine ALREADY has a primitive center anchor
  (`swrUI_NewLabel` centering hardcoded to 639/479) -- that IS a 640/480 decision to replace.
- Migrate the page registry from `swrUI_BuildMenuPages` (0x411e10): title, main menu, settings hub,
  video/audio/joystick/mouse/keyboard/FF, profile select, load/save. See the page table in
  `ghidra_analysis/ui_system_notes.md`.
- Risk: some F1 widget-class procs / sub-builders are still GUI-carve-blocked in Ghidra (see
  [[swrui_mapping_progress]]); carving them may be a prerequisite for faithful reimpl of a page.

### Phase C -- HD UI textures. Effort: M-L (can run parallel to B once the contract is set)
Swap physical sprite textures for denser ones while keeping logical header/page dims fixed.
- Interception point (pinned 2026-06-17): 2D UI sprite textures load via `swrSprite_LoadFromId` ->
  `swrSprite_LoadTexture` (0x446ca0) from the sprite block, then each tile goes through
  `swrModel_ConvertTileToRdMaterial` -> RdMaterial -> `std3D_AllocSystemTexture` GL upload (the SAME
  backend as 3D model textures -- they converge there). So two good options:
  (a) hook `swrSprite_LoadFromId` and, when a loose HD file exists for the sprite, build the
      RdMaterial from HD pixels while keeping the original logical `header`/`page` dims; or
  (b) generalize the existing model/material loose-file replacement at the shared convergence point,
      keyed to identify sprite tiles.
  Big enablers found: **UI sprites are name-keyed** -- `swrSprite_LoadAllSprites` (0x412650) loads
  ~130 named sprites via `swrSprite_LoadFromId(id, "sq_brdr_b" / "ok_button" / "sliderbar_end" ...)`,
  ideal replacement keys; and there is **already a loose-file precedent** --
  `swrSprite_GetTextureFromTGA("data/images/background.tga", 0xfa)` loads the menu background from a
  loose TGA. So HD UI art is closer to the existing asset system than first thought.
- **The one contract with Phase B:** the loader keeps logical `header`/`page` dims unchanged and
  swaps only the physical pixels (`ddsd`). Footprint and all positioning math stay untouched; the
  UV math (`page.width/ddsd.dwWidth`) adapts automatically.
- **Backgrounds are the one cross-axis case:** a full-bleed background must FILL the (now wider)
  layout, so its footprint/anchor is a layout decision (the fill/stretch anchor role) and it should
  be re-authored at a true widescreen aspect, not a stretched 4:3 image. Co-design background art
  with the Phase B background anchors.
- Fonts: separate glyph-atlas path; may already be HD per [[asset_replacement_architecture]].

### Phase C+ -- Vector/scalable source art (SVG sprites + TTF fonts), rasterized at target res. Effort: M (rides Phase C)
Make the texel-density axis truly resolution-independent by sourcing HD art from VECTOR formats and
rasterizing to the existing GL textures at the live resolution -- **NOT** by rendering vectors at
runtime. This is a source-format upgrade to Phase C, not a new render path; it keeps the "swap the
physical pixels, keep the logical dims" contract intact.
- **SVG sprites (NanoSVG).** At `swrSprite_LoadFromId` (the Phase C interception point), if a loose
  `.svg` exists for the name-keyed sprite, rasterize it (`nanosvg.h` + `nanosvgrast.h` -- single-
  header, zero-dep, vendored like stb / nv_dds / fastgltf) to RGBA at the target footprint density,
  build the RdMaterial from those pixels, and keep the original logical `header`/`page` dims.
  Re-rasterize on resolution change (a rare event) or bake at a fixed 2x-4x density and cache.
  Result: crisp at any resolution/aspect with no per-resolution DDS export. NanoSVG is a SIMPLE
  rasterizer (paths, gradients, AA -- no filters / embedded text), so keep authored SVGs to flat
  shapes / paths / gradients. Bonus: a whole panel (border + fill + seams) can bake into ONE vector
  -> ONE texture, directly serving the "composite, do not scatter" rule (SS5).
- **TTF/OTF fonts (stb_truetype / FreeType), not SVG** -- see SS11d. The scalable-font answer is
  glyph rasterization into the atlas, not SVG; stb_truetype is ALREADY in-tree (ImGui 1.91 bundles
  it).
- **Scope boundary (important):** this solves ONLY texel density (sharpness). It does NOT solve
  layout / anchoring -- footprint + position stay the Phase A/B builder work. An SVG/TTF asset is one
  element's pixels; it carries no inter-widget layout. Do NOT render whole menus as one SVG document
  -- that discards the native widget / hit-test / input system (the "full switch" trap).
- **Orthogonal to ImGui:** even ImGui rasterizes (stb_truetype atlas + quad drawlists; no runtime
  vectors). So this sharpness win lives entirely in the NATIVE pipeline and is not a reason to switch
  toolkits.
- **Pitfalls:** rasterize at load or on-resize and CACHE -- never per frame; match the RGBA /
  premultiplied format `std3D_AllocSystemTexture` expects; rasterizing many sprites at 4x costs VRAM
  (fine at UI scale); vet any SVG using features NanoSVG cannot render.

### Cross-cutting -- Consumer audit. Effort: DONE (2026-06-17), see inventory doc SS7
Result: the 640/480 dependency is **highly localized**. The design reciprocals (1/640, 1/480) have
a SINGLE consumer (`swrSprite_GetUIScale`). 2D-UI consumers needing rework are a short named list:
GetUIScale + text recip (-> identity, Phase A), `swrUI_BuildPanelFrame` (drop its
`screen_width != 0x280` frame fudge), `swrUI_DrawCaret` (drop its 512 special-case),
`swrText_SetEntryClipRect` (follow the layout), `swrSprite_InitDrawing` (keep, verify). The
projected systems reading screen dims are NOT all "out of scope" (corrected in SS3): the pure-3D
viewport (`swrViewport_ComputeScreenRect`) stays, but the projected-element SEAMS
(`swrSprite_SetPosF` 0x42bb00, `swrText_CreateTextEntry2` 0x42c7a0) read screen_width to convert
real-px->design and MUST flip to identity in lockstep with the draw scale (see SS3 consistency
contract) or weather/flares/markers desync. `swrSprite_AddDirtyRect` is likely dead under GL. Full
classification: `ghidra_analysis/ui_menu_layout_inventory.md` SS7. No sprawling hidden web -> model
is viable, but the seam set is wider than the original audit implied.

---

## 7. Status

- **REVERTED (not in tree):** Phase 1 stretch fix (`swrSprite_GetUIScale_delta`, was commit 6615223,
  PR #40). Merged at `84b0ccb`, then tim reverted the functional part on 2026-06-19 (`da33cd8`,
  "swrSprite_Delta changes only") -- removed `swrSprite_delta.cpp/.h`, the `widescreen_ui` toggle in
  `imgui_utils.cpp/.h`, and the `renderer_hook.cpp` hook. Reason: the delta made the DRAW uniform but
  left the cursor mapping stretched (see SS4), so clicks desynced from visuals -- exactly the
  half-measure failure this roadmap predicted. Only documentation survives: `swrSprite_GetUIScale`
  name/address in `swrSprite.h` + `data_symbols.syms`. **The 2D UI is back to vanilla 4:3-stretched.**
  This is empirical proof for the full resolution-independent reimpl over another draw-only patch.
- **EXISTS (survived the revert):** cursor remap delta (`stdConsole_GetCursorPos_delta`, maps window
  px -> stretched 640x480). With Phase 1 gone, draw (stretched) and cursor (stretched) AGREE again, so
  the current baseline is consistent-but-stretched, not broken. HD model/material texture loose-file
  system (3D path; does not cover 2D UI sprites).
- **ABANDONED:** centering/pillarbox (SS9 / SS0).
- **NEXT:** Phase A. Note A1 (cheap cursor-fix win) is now moot as a standalone -- with the sprite
  delta reverted there is no draw/cursor desync to fix; A1 only made sense layered on Phase 1. Phase A
  now starts from vanilla-stretched: re-introduce the uniform draw AND the matching uniform cursor
  together (don't ship one without the other -- that is what got reverted), or jump to the anchored
  builders (A2/B).

### 7b. Status as of 2026-09-02 (supersedes everything above in SS7)

_Everything in SS7 above describes the pre-#185 world and is kept only for the history of why the
draw-only patch was reverted. The lines below are the live status._

_This section was reconstructed on 2026-09-02 after the previous SS7b (written 2026-08-31) was lost:
the roadmaps are gitignored in the main worktree but tracked on `local/roadmaps-and-tooling`, whose
last backup was 2026-07-08, so checking that branch out silently overwrote the newer working copy.
The PR/status facts below were re-derived from upstream; any finer prose from the lost version is
gone. **Back roadmaps up before switching to that branch.**_

- **Phase A -- SHIPPED**, merged as PR #185 (2026-06-25, `feature/ui-resolution-independent`). The
  uniform draw and the matching uniform cursor mapping shipped together, which is what the reverted
  Phase 1 failed to do.
- **Edge-anchoring / widescreen fill -- SHIPPED**, merged as PR #241 (2026-09-02,
  `feature/ui-widescreen-hud`). Menus + in-race HUD anchor to the real screen edges instead of
  pillarboxing, via a runtime anchor-reconciliation table (`g_anchored_elements` +
  `hud_sprite_anchor` / `hud_text_anchor`) rather than by rewriting the builders. The
  resolution-independent toggle also ships **ON by default** as of that PR -- fresh installs only,
  since the ini fallback applies only when the key is absent, so existing configs keep their setting.
  The same PR re-keyed the anchor tables off the `swrUISprite` enum in `src/types_enums.h` and named
  the HUD text columns (`kHudTextX*`).
- **HD 2D sprite replacement -- SHIPPED** as PR #231 (`assets/replacement_sprites/`, keyed by
  `swrSprite_` name). The SS3 contract holds: logical `header`/`page` dims unchanged, only the
  physical pixels swap.
- **Phase B (reimplement builders responsively, page by page) -- NOT STARTED, and no longer
  blocked.** The old prerequisite (some F1 widget-class procs / sub-builders still GUI-carve-blocked
  in Ghidra) is CLEARED: the widget class -> ctor -> proc map was carved and named in PR #72, and
  PR #272 landed 24 swrUI/swrSprite setter bodies. With #241 merged the layout layer is settled too.
  Because #241 delivers the visible widescreen payoff through the anchor table, Phase B is now a
  question of whether the remaining pages are worth an L-effort rewrite, not a blocking dependency.
- **ABANDONED:** centering/pillarbox as the *end state* (SS9 / SS0) -- superseded by edge-anchoring
  in #241. The centering offset itself still ships as the res-independent baseline.
- **Open UI bug:** the game cursor is at half its correct position when the resolution-independent UI
  is turned OFF. Lower priority since #241 made the res-independent path the default.

---

## 8. Function / global reference (reconfirm at impl time)

| Addr | Name | Role |
|------|------|------|
| 0x411e10 | swrUI_BuildMenuPages | builds all front-end pages (640-space literals) |
| 0x416d90 | swrUI_New | element ctor; default bbox (0,0,639,479) |
| 0x414b60 / 0x414b40 | swrUI_SetPos / swrUI_SetSize | public setters (forward to msg 0xb/0xc) |
| 0x416f50 | swrUI_OnSetElementPos | stores rect raw, cascades child sprite offsets |
| 0x415810 | swrUI_SetUnk (SetBBox) | stores bbox raw |
| 0x4150e0 | swrUI_HitTest | rect vs cursor point-in-rect (BBoxFit + IsInsideBBox) |
| 0x417f00 / 0x4172c0 | swrSprite_BBoxFit / swrSprite_IsInsideBBox | bbox clip / point test |
| 0x415400 | swrUI_ProcessMouse | per-frame mouse dispatch; calls HitTest |
| 0x4083d0 | swrUI_UpdateMouseState | reads cursor into g_mouse_x/g_mouse_y2 |
| 0x4082e0 | stdConsole_GetCursorPos | cursor source (delta in stdConsole_delta.cpp) |
| 0x44f640 | swrSprite_GetUIScale | 2D scale source (delta shipped) |
| 0x428030 / 0x44f670 | swrSprite_Draw2 / swrSprite_Draw1 | sprite size/position scaling |
| 0x4286f0 | swrSprite_SetDim | sets sprite->width/height (float SCALE) |
| 0x446ca0 | swrSprite_LoadTexture | 2D UI sprite-bank load (HD interception point) |
| 0x48a5e0 | std3D_AllocSystemTexture | universal GL upload (delta) |
| 0x00ec86c4 / 0x00ec85e8 | screen_width / screen_height | real framebuffer dims (== swrDisplay_screen*) |

---

## 9. Why centering was abandoned (record so it is not re-attempted)

Pillarboxing the 4:3 UI required an additive X offset that has no home in the scale path; it had to
be injected at the 2D emit funnels, two of which (`rdProcEntry_Add2DPolygon`, `Add2DQuad5`) are
decompiler-garbled. More fundamentally, it offset the DRAW while the cursor stayed device-relative,
so clicks desynced from visuals. The Phase-2 spike confirmed it went nowhere. It also makes no sense
alongside full-screen effects (which must NOT be pillarboxed). Resolution-independent layout (this
roadmap) delivers a correct full-screen UI without any centering offset.

---

## 10. Runtime verification needed

- Confirm in the live HD build that, under `widescreen_ui` on, menu mouse clicks currently land
  offset to the right (validates SS4 before building on it).
- Confirm `screen_width`/`screen_height` hold the true framebuffer size at runtime (Cheat Engine;
  `.text` is encrypted on disk).
- After Phase A, confirm identity scale + identity cursor leave existing (shimmed) menus clickable.

---

## 11. Text & fonts (the fourth axis)

Text is NOT just another rendered element -- it has its own scale lever AND it feeds layout, so it
must be designed in, not bolted on. Three findings (grounded 2026-06-17):

### 11a. Text has a SEPARATE scale path (analog of GetUIScale)
Glyph design->screen scaling happens ONLY in `rdProcEntry_Add2DQuad2` (0x42d990):
```
xscale = screen_width  * swrText_designWidthRecip   (0x4ac628 = 1/640)
yscale = screen_height * swrText_designHeightRecip  (0x4ac630 = 1/480)   [clamped >= 1.0]
```
This is the text twin of `swrSprite_GetUIScale`. `swrText_DrawString` (0x42e150) advances the pen
in DESIGN units and emits each glyph quad through Add2DQuad2, which applies this scale.
**Phase 1 already de-stretches text** by patching the X recip so xscale == yscale.

### 11b. CRITICAL asymmetry vs sprites: text scale governs glyph SIZE, so it must stay a real
uniform scale -- it can NEVER go to identity (that would shrink glyphs to native texel size). So
where sprites move their uniform scale INTO the builder and leave GetUIScale at identity, **text
keeps its uniform scale in the emit (= screenH/480).** Consequence:
- **Uniform-logical staging: text is essentially free.** Text scale == the shared uniformScale, so
  glyphs, label rects, and cursor all live in one logical space, consistent. Nothing to do beyond
  the X-recip patch (already shipped). **This is a strong additional argument for staging.**
- **Physical/identity endgame: text stays a logical-space exception.** The glyph pen + origin are
  design units scaled by the emit, so to place a label at a physical widget the builder sets text
  origin = physicalPos / uniformScale. Text positioning therefore stays on the coarse uniformScale
  grid even in the physical model -- which is FINE: a string's ORIGIN being on a ~2px grid is
  invisible (intra-string spacing is glyph-advance precise). So text does not need the fine grid;
  only sprite art seams did.

### 11c. Text measurement feeds layout (the coupling)
`swrText_GetStringWidth` (0x42de30) / `GetStringHeight` (0x42df70) return font-native/design widths
(no screen dep). Builders use them to size labels (`swrUI_NewLabel`), right/center-align (`~r`/`~c`
escapes in DrawString), and drive settings-page x's. Rule: the measured width and the widget
position must share units. Uniform-logical -> automatic (all logical). Physical -> the builder
multiplies measurements by uniformScale when sizing text-driven widgets.

### 11d. HD fonts = denser font-page texture, keep glyph metrics (parallel to HD sprites)
Glyphs are textured quads from a font page bound by `swrText_BindFontPage` (0x42ddf0); glyph
table holds UV + advance metrics. HD fonts = swap the font-page TEXTURE for a denser one while
keeping the glyph METRICS (advances/sizes in design units) unchanged -> sharp text, layout
untouched. Exactly the footprint-vs-texel decoupling from SS1d/Phase C. NOTE: a memory claims
"fonts done as loose files" but no font-specific delta was found (font refs only in
swrSprite/swrModel deltas) -- VERIFY whether fonts already have an HD/loose path or load via the
sprite-texture system (`swrText_InitFonts` 0x42d720).

**Scalable source (parallel to Phase C+):** the denser font-page atlas can be GENERATED by
rasterizing a TTF/OTF at the target glyph size (stb_truetype -- already in-tree via ImGui 1.91; or
FreeType) instead of shipping a pre-baked denser bitmap -> truly resolution-independent text. Two
paths: (a) LOW-RISK -- keep the original glyph metrics (advances/widths in design units, which feed
layout per SS11c) and only re-raster the atlas denser; (b) full dynamic-TTF with NEW metrics, which
perturbs the measurement->layout coupling. Start with (a). SVG is the WRONG tool for fonts here --
the vector-font answer is glyph rasterization, not SVG-in-OpenType.

### 11e. Text clip rects follow the UI space
`swrText_SetEntryClipRect` (0x450310) and the clip clamp inside Add2DQuad2 (bounds DAT_00e99750-5c)
must track the layout, not raw 640/480. Already listed in the consumer audit (SS6 / inventory SS7).

### Net
Text adds no blocker -- it is largely handled by Phase 1's X-recip patch and rides the same
uniformScale. It (a) reinforces the uniform-logical staging (text is free there), (b) stays a
deliberate logical-space element even in the physical endgame (which is fine), and (c) gets HD via
a font-page texture swap. Functions: DrawString 0x42e150, GetStringWidth 0x42de30 / Height 0x42df70,
Add2DQuad2 0x42d990, BindFontPage 0x42ddf0, InitFonts 0x42d720; recips 0x4ac628/0x4ac630.

## 12. Forward-looking features the foundation must not preclude (2026-06-24)

Three features are planned on top of resolution-independence. They were pressure-tested against the
model so the shared transform (SS13) is built to make them ADDITIVE, not rewrites.

1. **Global UI scale slider** -- a user `userUIScale` multiplier on UI size. Must scale each element
   about ITS OWN anchor (grow in place, stay pinned to its edge), so the scale lives in the LAYOUT
   stage, not as a global draw multiplier (a global draw scale slides things toward the origin
   instead of growing in place). Just another input to the layout function:
   `screenPos = anchorPoint(screenDims) + designOffset * baseScale * userUIScale`.
2. **Repositionable UI elements** -- per-element position overrides, persisted to the profile.
   Needs (a) layout-as-DATA keyed by STABLE element IDs (so an override can replace a default;
   not possible if the builder hardcodes literals), and (b) an INVERTIBLE transform (drag yields a
   framebuffer position that must convert back to anchor+offset to store).
3. **In-race HUD wobble (POSITION ONLY, no rotation)** -- a per-frame TRANSLATION of the in-race HUD
   as a GROUP, driven by pod dynamics (bank / lateral g / impact shake from the swrRace entity).
   Because it is translation-only, no rotation is needed in the emit -- so it works for sprites AND
   text in both the vanilla and SDF text paths (rotation would have constrained text to the SDF path
   only; that constraint is now MOOT).

### Foundation requirements these impose (bake in now -- cheap now, expensive to retrofit)
| Property | Res-indep. needs it? | Unlocks |
|----------|----------------------|---------|
| Transform = composable **scale + translation + pivot** (similarity, NO rotation) | no (uniform scale suffices) | scale slider, reposition, wobble compose by one rule |
| **Invertible** (framebuffer <-> design) | yes (cursor) | drag-to-reposition |
| Layout = **re-runnable pure fn** of (table, screenDims, userScale) | yes (window resize) | scale slider, reposition |
| Layout-as-**data keyed by stable element IDs** | partly | reposition, persistence, modding |
| **Group/layer transform** (esp. for the imperative in-race HUD) | no | HUD wobble, per-HUD scale |
| Transform applied to **draw AND hit-test together** | yes (the Phase-1 fix) | interactive transformed elements |

Only TWO are "extra" vs a minimal res-indep MVP: choosing the composable scale+translation
representation, and the group/layer concept. Both are cheap up front and brutal to retrofit. The
other three are needed for resize / cursor / staging anyway.

### Caveats carried forward
- The in-race HUD is drawn IMPERATIVELY (swrPlayerHUD_* / direct swrSprite+swrText), NOT as a swrUI
  widget tree -- so wobble + reposition for HUD elements need the group transform + override table to
  reach the imperative draw path, separate from the menu builder rework.
- A scale slider BELOW 1.0 hits the text path's `>= 1.0` clamp (SS11b) -- shrinking text needs the
  reimplemented SDF text path (no clamp). Another reason that path matters for the full feature set.
- Scale slider + reposition are PERSISTENT -> they ride the save/profile subsystem (the override /
  config table). Rotation is explicitly OUT of scope (wobble is position-only); leaving a rotation
  field in the transform struct is an optional near-zero-cost hedge, not a requirement.

## 13. Shared transform module spec (the first implementation artifact)

Build this BEFORE any builder touches a coordinate. It is the single definition of
design<->framebuffer that all six coupled consumers (SS3 contract) route through, so the relationship
can never be half-changed again. Lives in the dinput_hook delta layer.

### Core type + authoring refs
```
#define UI_DESIGN_W 640.0f   /* authoring reference ONLY, defined in one place */
#define UI_DESIGN_H 480.0f

typedef struct { float scale; float tx, ty; } UiXform;   /* uniform scale + framebuffer translation */
typedef enum { UI_H_LEFT, UI_H_CENTER, UI_H_RIGHT, UI_H_STRETCH } UiAnchorH;
typedef enum { UI_V_TOP,  UI_V_MIDDLE, UI_V_BOTTOM, UI_V_STRETCH } UiAnchorV;
```
`UiXform` composes (`out.scale = a.scale*b.scale; out.t = a.t + a.scale*b.t`) and inverts
(`inv.scale = 1/scale; inv.t = -t/scale`). No rotation term (wobble is position-only).

### Core API
```
float  ui_layout_scale(void);                 /* = (screenH/UI_DESIGN_H) * userUIScale; uniform, square */
vec2   ui_anchor_point(UiAnchorH, UiAnchorV); /* the screen-pinned origin for an anchor, vs live dims */
vec2   ui_design_to_screen(anchor, vec2 designOffset);  /* anchor_point + scale*offset  */
vec2   ui_screen_to_design(anchor, vec2 screenPos);     /* INVERSE: (screenPos - anchor_point)/scale */
void   ui_layer_push(UiXform);  void ui_layer_pop(void);  UiXform ui_layer_current(void);
```
`userUIScale` defaults 1.0 (scale slider). The layer stack carries per-group transforms; the in-race
HUD pushes a translation layer each frame for wobble. The emit applies `ui_layer_current()` to final
positions, so menus (no layer pushed) are unaffected.

### The six call-site conversions (the SS3 contract, all through this module)
| Consumer | Delta routes to |
|----------|-----------------|
| `swrSprite_GetUIScale` | returns `ui_layout_scale()` on both axes (uniform; identity in the px endgame) |
| `rdProcEntry_Add2DQuad2` (text) | same `ui_layout_scale()` (keeps the `>=1` clamp; text never identity) |
| `stdConsole_GetCursorPos` (cursor) | `ui_screen_to_design(LEFT/TOP, rawPx)` -- uniform inverse, kills the stretch desync |
| `swrSprite_SetPosF` (projected-sprite seam) | `ui_screen_to_design(LEFT/TOP, realPx)` -- divide so the draw's multiply cancels |
| `swrText_CreateTextEntry2` (projected-text seam) | same inverse on the text origin |
| `BuildPanelFrame` / `DrawCaret` / clip rect | drop hardcoded `!=640`/`512`; size from `ui_layout_scale()` + texture dims |

### Layout function (Phase B builders call this; re-runnable)
```
screenPos(elem) = ui_design_to_screen(elem.anchor,
                      override(elem.id) ? override(elem.id).offset : elem.designOffset)
```
`elem` comes from a DATA table keyed by stable ID (enables reposition + modding). Re-run on resize /
scale-slider / override change -- not a one-shot startup bake.

### Staging knob (SS6) -- the module hides it from call sites
`ui_layout_scale()` returns `screenH/480 * userUIScale` during uniform-logical staging, or `1.0`
(identity) in the physical-px endgame; whether stored positions are design-units or framebuffer-px is
an internal convention. Either way the six call sites and the layout fn are unchanged -- the module is
the ONE place the staging decision lives.

### Extension points it must leave open (SS12)
- scale slider = the `userUIScale` factor already in `ui_layout_scale()` (done by construction).
- reposition = the `override(id)` hook in the layout fn + persistence; `ui_screen_to_design` gives the
  drag inverse.
- HUD wobble = a translation `UiXform` pushed via `ui_layer_push` around the in-race HUD draw block.

## Cross-references
- `ghidra_analysis/ui_system_notes.md` -- the raw trace this roadmap supersedes (page registry,
  message IDs, widget ctors still useful).
- Memory: UI widescreen root cause, swrUI mapping progress, asset replacement architecture.
