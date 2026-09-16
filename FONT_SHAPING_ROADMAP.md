# Font Shaping Roadmap - OpenType shaping for the SDF text engine

Goal: add real text shaping to the crisp-text (SDF) feature so it can drive advanced
OpenType fonts - ligatures, contextual alternates, stylistic sets, alternates, proper
kerning (GPOS), diacritic/mark positioning, and multi-script coverage. The north-star
target is **Womprat** (see below): the maximal OpenType font, whose feature set exercises
everything the current engine cannot do.

This is a large, post-#182 effort with new vendored dependencies. #182 (the SDF feature,
PR still OPEN awaiting review) stays as-is; this is a separate track.

## How to resume this in a new session
- Read the memory `font-rendering-sdf` (architecture, the seam, the shipped state, the
  RE'd font-slot map, and the two open follow-ups).
- The engine is `dinput_hook/sdf_text.cpp` (+ `sdf_text.h`); the reverse-hook seam is
  `swrText_RenderString_delta` in `dinput_hook/game_deltas/swrText_delta.cpp`; the panel is
  the "Font" section in `dinput_hook/imgui_utils.cpp`; per-slot config is `SdfFontSlot`;
  profiles live under `assets/fonts/profiles/`.
- Shipped SDF feature = PR #182 (louriccia:feature/sdf-text -> tim-tim707 master).

## The target: Womprat (womprat.xyz)
Commercial OpenType font by Louie Mantia / Ender Smith ($120). Advertises: "lots & lots of
ligatures" incl. multi-word ligatures as a stylistic set; multiple stylistic sets; abundant
alternates; Latin + Greek + Cyrillic + Katakana; 100+ languages with extended-Latin accents
and obsolete forms; 400+ icons/symbols, 5 ampersands, currency, math, Roman numerals,
fractions. Delivered as a **CFF OpenType** font. A free **Aurebesh** companion exists.

Implication: full support needs GSUB (ligatures/`calt`/`ssNN`/`aalt`), GPOS (kerning + mark
attachment), broad Unicode coverage, and robust CFF rasterization - i.e. the whole pipeline.

Licensing: Womprat is commercial -> **user-supplied** (engine supports it; font never bundled),
same model as Moki/Aurebesh today. The free Aurebesh companion could be bundled if its license
permits.

## Why the current engine can't do any of it (structural)
Current engine (`sdf_text.cpp`): a **codepoint-keyed** SDF atlas built with **stb_truetype**.
- Glyphs are stored/looked up by Unicode codepoint (`face_glyph`), atlas covers `0x20-0x7e`
  + 3 symbols. Ligatures/alternates are glyphs with **no codepoint** - unaddressable.
- **No shaper.** stb_truetype does zero GSUB/GPOS. Kerning is `stbtt_GetCodepointKernAdvance`
  = legacy `kern` table only (Womprat's kerning is GPOS -> we'd get none).
- **Byte input, ASCII only.** The layout loop reads bytes (not UTF-8); non-ASCII -> no glyph.
- **Partial CFF raster.** stb_truetype 1.26 (latest) has incomplete CFF support - the exact
  Moki/OTF bug. Womprat is CFF, so even addressable glyphs would render wrong.

## Target architecture
Data flow (SDF-on path only; OFF path stays byte-identical vanilla):
```
game string --(UTF-8 decode)--> Unicode run
   --> HarfBuzz shape (per slot's font + enabled features + script/lang)
   --> positioned glyph IDs (+ x/y advance & offset from GPOS)
   --> glyph-ID atlas: raster-on-demand via FreeType SDF, cache by (face, glyphID, size)
   --> emit quads through the existing SDF shader (renderList / sdf_text flush)
```
New vendored deps (both build under MinGW32; vendor in `dinput_hook/` like imgui/glfw/fastgltf):
- **HarfBuzz** - shaping (GSUB/GPOS). Reads the font blob directly (its own OT tables reader),
  independent of the rasterizer.
- **FreeType** - rasterizer. Robust CFF (fixes the OTF/Moki class of bug) and can emit SDF by
  glyph index (FT SDF module, 2.12+). HB+FT is the standard pairing.
  - Decision point: use FT's built-in SDF vs render a high-res bitmap and compute SDF
    ourselves (as stbtt does). Prototype both; pick on quality/perf.
- Rasterizer note: stb_truetype can raster by glyph index too, but its partial CFF rules it
  out for Womprat. FreeType is required for CFF.

## Phases (each independently testable)
- **Phase 0 - spike + decisions.** Vendor HB + FT, get them building in the MinGW32 toolchain,
  and prove FT-SDF glyph quality matches the current stbtt-SDF look through the existing shader.
  Confirm licensing/bundling plan. De-risks the two scariest unknowns (build + SDF quality)
  before any engine rework.
- **Phase 1 - FreeType raster swap (no shaping yet).** Replace stb_truetype rasterization with
  FreeType, keep the current codepoint-keyed layout. Immediate win: **fixes OTF/CFF fonts**
  (Moki renders correctly with no conversion; retire the OTF warning). No behavior change for
  the bundled TTFs (verify byte-close output).
- **Phase 2 - glyph-ID dynamic atlas.** Re-key the atlas by glyph ID; rasterize on demand with
  an LRU/growing cache (can't pre-raster 4 scripts + 400 symbols + alternates). Coordinate
  worker raster + main-thread GL upload (evolves the current async build model). Prereq for
  shaping (which outputs glyph IDs, incl. ligature glyphs).
- **Phase 3 - HarfBuzz shaping (Latin LTR first).** Shape each run with HB; consume glyph IDs +
  GPOS positions in the layout loop, replacing the per-codepoint advance/kern math. Enable
  `liga`, `calt`, `kern`. Immediate payoff on existing ASCII UI text (ligatures/alternates,
  real kerning). Keep the tabular-digit override for HUD numbers.
- **Phase 4 - OpenType feature UI.** Per-slot feature toggles in the "Font" panel + persisted in
  `SdfFontSlot`/profiles: `liga`/`dlig`/`calt`, stylistic sets `ss01..ssNN`, `aalt`/alternates,
  fractions, etc. Decide feature-set granularity (per slot vs global) and how it coexists with
  the game's `~` code system.
- **Phase 5 - UTF-8 + multi-script.** UTF-8 decode in the layout loop; HB script/lang selection
  (Latin/Greek/Cyrillic/Katakana). NOTE: rendering != content - the game feeds fixed ASCII, so
  non-Latin only appears with a localization/content layer (separate effort; call it out, don't
  silently imply multi-language "works"). That content layer is **`LOCALIZATION_ROADMAP.md`**.
  CJK localization (JA / ZH-Hans, from the Switch build) consumes **Phase 2 + Phase 5 ONLY**:
  Han/Kana layout is ~1:1 codepoint->glyph LTR advance (no GSUB/GPOS), so it needs the dynamic
  atlas + UTF-8 but NOT the HarfBuzz feature work in Phases 3/4/6 - a valid "CJK-minimal" exit.
  It also needs a broad CJK font (Noto Sans CJK / Source Han / M+), NOT Womprat's Katakana-only
  coverage - so the localization font target and the Womprat north-star diverge, sharing only
  the Phase-2 dynamic atlas.
- **Phase 6 - marks + symbols polish.** GPOS mark attachment for diacritics; fractions, currency,
  math, Roman numerals; verify outline/shadow/`~o`/center/right alignment across the set.

## Key decisions / open questions
- FreeType built-in SDF vs self-computed SDF (quality/perf) - settle in Phase 0.
- Atlas eviction policy + max size (dynamic content vs the current fixed 2048^2).
- How OT features are selected: per-slot config (matches today's `SdfFontSlot`), plus maybe
  parsing of the game's inline `~` codes. Womprat's multi-word ligatures are a stylistic set ->
  needs `ssNN` toggles.
- Game string encoding: today's strings are the game's own byte encoding + `~` codes; UTF-8 is
  only meaningful once content is localized.
- Interaction with the width/caret + UI-metrics follow-up: shaping changes advances again, so
  ANY width query (`swrUI_GetSubstringWidth` @0x418680, sums bitmap `swrText_GetCharSize`) is
  even more divergent. The shaped layout must expose a width API, and the caret/alignment fixes
  should route through it. Ties into the UI resolution-independent reimp.

## Faithfulness + constraints
- SDF-OFF stays byte-identical vanilla (unchanged bitmap path). Shaping only on the SDF-on path.
- Full `dinput.dll` link with the WinLibs GCC 13.2 MinGW32 recipe before any PR; deps vendored
  in-tree. HarfBuzz + FreeType both build under MinGW32 (verify in Phase 0 - main build risk).
- Per-slot defaults must keep today's look for the bundled TTFs (no regression when features off).

## RE-grounded reference (for a cold start)
- Seam: `swrText_RenderString` reverse-hooked @ `0x0042ec50` -> `swrText_RenderString_delta`
  (`game_deltas/swrText_delta.cpp`); SDF path returns before the vanilla bitmap draw when
  `imgui_state.sdf_text`. Frame flush at `std3D_EndScene` (`std3D_delta.cpp`).
- Font-slot map (vanilla `swrText_InitFonts` @0x42d720 + `RenderString` default
  `SetCurrentFont(0)`): `fontTable[7]`->`fonts[5]`: `~f0`->f3, `~f1`->f2, `~f2`->f1, `~f3`->f2,
  `~f4`->f4, `~f5`->f3, `~f6`->f0; DEFAULT (no `~f`) = **fonts[3]**. Roles: slot0=`~f6` FINAL
  LAP banner; slot1=`~f2` speedometer#/track tiles/results; slot2=`~f1`+`~f3` lap/time/pos +
  overhead labels; slot3 = default UI font (+`~f0` course-info); slot4=`~f4` body/menu text.
- Config/UI: `SdfFontSlot` (`sdf_text.h`) is the per-slot config the "Font" panel edits and
  profiles persist (`assets/fonts/profiles/<name>.ini`). Add feature toggles here in Phase 4.
- Current kerning: `stbtt_GetCodepointKernAdvance` (kern table only). Current atlas params:
  `EM_PX 128`, `SDF_PADDING 12`, `ATLAS 2048x2048`, faces keyed by `(path, shear)`.

## Effort / sequencing note
Biggest single addition to the feature: two heavy vendored deps + atlas rework + layout rework.
Phases 0-1 already deliver value (robust OTF/CFF, retire the Moki-conversion workaround) before
the shaping payoff in Phase 3. Recommend Phase 0 as a standalone spike PR to prove build + SDF
quality before committing to 2+.
