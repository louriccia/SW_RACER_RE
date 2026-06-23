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

## 3. Screen-space effects -- explicitly OUT of scope

Weather (`swrWeather_RenderParticles` 0x42cca0) positions particles from
`swrViewport_ProjectToScreen` clipped against `screen_width/height`. Lens flare and world HUD text
work the same way. They already fill the real screen and must NOT receive any widget transform.
The 3D viewport (`swrViewport_ComputeScreenRect` 0x4830e0) likewise keeps real width.

Caveat to verify: Phase 1's uniform `GetUIScale` change shrank ALL sprite widths ~25% at 1080p
(it is a global scale). For round-ish weather/flare particles this is cosmetically negligible, but
under the target identity-scale model the effect sprites must keep sizing correctly -- confirm
when scale goes to identity.

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

### Cross-cutting -- Consumer audit. Effort: DONE (2026-06-17), see inventory doc SS7
Result: the 640/480 dependency is **highly localized**. The design reciprocals (1/640, 1/480) have
a SINGLE consumer (`swrSprite_GetUIScale`). 2D-UI consumers needing rework are a short named list:
GetUIScale + text recip (-> identity, Phase A), `swrUI_BuildPanelFrame` (drop its
`screen_width != 0x280` frame fudge), `swrUI_DrawCaret` (drop its 512 special-case),
`swrText_SetEntryClipRect` (follow the layout), `swrSprite_InitDrawing` (keep, verify). Everything
else reading screen dims is screen-space/3D (weather, lens flare, HUD, viewport) and correctly
stays. `swrSprite_AddDirtyRect` is likely dead under GL. Full classification:
`ghidra_analysis/ui_menu_layout_inventory.md` SS7. No sprawling hidden web -> model is viable.

---

## 7. Status

- **DONE / shipped:** Phase 1 stretch fix (`swrSprite_GetUIScale_delta`, commit 6615223, hooked at
  `renderer_hook.cpp:1076`, toggle `widescreen_ui`). Makes X scale uniform, kills the stretch,
  left-anchored. Text X reciprocal patched at 0x004ac628. This is superseded by the target model
  (scale -> identity) but stays as the A/B toggle until Phase A lands.
- **EXISTS:** cursor remap delta (stretched; see SS4). HD model/material texture loose-file system
  (3D path; does not cover 2D UI sprites).
- **ABANDONED:** centering/pillarbox (SS9 / SS0).
- **NEXT:** Phase A.

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

### 11e. Text clip rects follow the UI space
`swrText_SetEntryClipRect` (0x450310) and the clip clamp inside Add2DQuad2 (bounds DAT_00e99750-5c)
must track the layout, not raw 640/480. Already listed in the consumer audit (SS6 / inventory SS7).

### Net
Text adds no blocker -- it is largely handled by Phase 1's X-recip patch and rides the same
uniformScale. It (a) reinforces the uniform-logical staging (text is free there), (b) stays a
deliberate logical-space element even in the physical endgame (which is fine), and (c) gets HD via
a font-page texture swap. Functions: DrawString 0x42e150, GetStringWidth 0x42de30 / Height 0x42df70,
Add2DQuad2 0x42d990, BindFontPage 0x42ddf0, InitFonts 0x42d720; recips 0x4ac628/0x4ac630.

## Cross-references
- `ghidra_analysis/ui_system_notes.md` -- the raw trace this roadmap supersedes (page registry,
  message IDs, widget ctors still useful).
- Memory: UI widescreen root cause, swrUI mapping progress, asset replacement architecture.
