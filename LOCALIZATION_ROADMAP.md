# Localization Roadmap - multi-language content + selection for SWE1R

Goal: bring full multi-language support to the project by reusing the game's OWN
built-in string-translation engine (it already exists and is already live), then adding
the one thing the PC build lacks - a language selector - plus the translated content.

Two tiers, split where the engine itself splits:
- **Tier 1 - Latin (EN / FR / DE / ES / IT):** the text pipeline renders this TODAY.
  Low effort, mostly content + selection wiring.
- **Tier 2 - CJK (JA / ZH-Hans; later KO / ZH-Hant):** content is available (Switch dump)
  but NOT renderable today. Gated on the SDF engine's UTF-8 + dynamic-atlas work in
  `FONT_SHAPING_ROADMAP.md`.

## How to resume this in a new session
- Read memories `asset-replacement-architecture` (loose-file redirect layer),
  `font-rendering-sdf` (SDF seam/slots), `swr-domain-naming`, and this doc.
- Engine seam: `swrText_Translate` @0x00421360 (reimpl `src/Swr/swrText.c:149`),
  `swrText_ParseRacerTab` @0x00421120, called by `Main_Startup` @0x00423d14 with the
  fixed path `"data\racer.tab"`.
- Rendering dependency for Tier 2: `FONT_SHAPING_ROADMAP.md` Phase 2 (dynamic glyph-ID
  atlas) + Phase 5 (UTF-8 decode).
- Source dump: the decrypted Switch (XCI) build - `romfs/` + the `main` NSO; the string
  table lives in `main` .rodata (~0xA4400-0xAE800).

## The engine ALREADY has localization (key facts)
- `swrText_Translate("/KEY/inline-english")` resolves KEY against a KEY->VALUE table and
  returns the localized VALUE, or the inline English fallback if the key is missing / the
  table is unloaded. Strings without a leading `/` pass through unchanged.
- The table is loaded by `swrText_ParseRacerTab` from `data\racer.tab`: TSV `KEY\tVALUE`
  per line; optional obfuscation (magic `RCNE` = 0x454e4352 -> body XOR 0xdd from offset 4);
  keys uppercased; sorted for `bsearch`.
- Universe = **~465 keys**: 240 `SCREENTEXT_*` (menus/UI) + 225 `MONDOTEXT_H_*`
  (Watto/story dialogue). Every key carries its English fallback inline in the exe.
- The **Steam build ships NO `racer.tab`** (verified in the install `data/`), so English
  runs entirely on the inline fallbacks (`swrText_racerTab_buffer == NULL` path).
- Our reimpls are **dormant reverse-hooks**, so the original binary's `Translate` calls are
  live everywhere the 1999 game placed them. => Dropping a `racer.tab` in place localizes
  the entire vanilla game with **zero engine code** (Latin scripts only).
- The PC exe has **no language selector**; 1999 localized retail simply shipped a different
  `gnome/data/racer.TAB` (EN/FR/DE/ES/IT).

## Source material
- **Original PC `racer.TAB` (EFIGS)** - canonical FR/DE/ES/IT with the exact keys; the
  preferred Tier-1 source. IT exists ONLY here (not in the Switch build).
- **OpenSWE1R/swe1r-tools** - `extract-racer-tab.py` (dumps the KEY->English master from
  `swep1rcr.exe`) and `parse-racer-tab.py` (validates a translation file). Reuse these
  (with attribution) instead of reinventing.
- **Community patches** - GOG French patch, a German patch, a Spanish Steam guide; useful
  cross-checks / gap-fillers.
- **Switch dump (`main` NSO .rodata, ~0xA4400-0xAE800)** - 6 language blocks stored in
  string-ID order: EN / FR / DE / ES / JA / ZH-Hans (confirmed; NO IT/KO/RU). This is the
  ONLY readily-available source for **JA + ZH-Hans**. Keys are NOT embedded (the port
  pre-resolved them) -> recover keys by aligning the English column to the exe key master.
- **Official Aspyr Switch language set** = EN, FR, DE, ES, JA, ZH-Hans (matches the dump).
  Steam adds Traditional Chinese (+ others).
- **Encoding**: `racer.tab` is a single-byte codepage (CP-1252 / Latin-1) for Latin scripts.
  CJK requires UTF-8 tabs + a UTF-8-aware pipeline (see Tier 2).

## Tier 1 - Latin (EN / FR / DE / ES / IT): low effort
The pipeline renders this now. The bitmap path already composes accented Latin-1 via
`swrText_extCharComposeIndex` / `swrText_extCharComposePairs`; the SDF path only needs its
atlas codepoint range widened to Latin-1 (bundled DejaVu already has the glyphs).

- **L0 - master `en.tab`. [DONE]** `scripts/localization/extract_racer_tab.py` scans a
  pristine `SWEP1RCR.EXE` and emits `assets/lang/en/racer.tab` (repo source; deploys to the
  game's `data/lang/en/racer.tab`). Result: **465 keys** (240 `SCREENTEXT_*` + 225
  `MONDOTEXT_H_*`), CP-1252, control bytes escaped so it round-trips through
  `swrText_UnescapeString`. This is the translation template AND the alignment key for the
  Switch extraction (Tier 2 / C0). NOTE: the generated `.tab` is the game's full English
  text corpus - kept OUT of git (`assets/lang/**/*.tab` in `.git/info/exclude`); each user
  extracts from their own exe (the swe1r-tools model). Committing translated content is a
  separate maintainer decision (see Key decisions).
- **L1 - file layout + load. [PROTOTYPE BUILT]** Loose files `data/lang/<code>/racer.tab`.
  Implemented in `dinput_hook/localization.cpp`: instead of a file-open redirect, `init_localization()`
  runs from the mod's boot hook (`LoadIconHook`, after `Main_Startup`'s own ParseRacerTab) and
  (re)loads the selected language's tab by calling the original `swrText_Shutdown` + `swrText_ParseRacerTab`
  (via address, reimpls are dormant). Simpler than hooking the fixed `.rodata` path and equivalent for
  the Steam case (which ships no `data\racer.tab`). English = no tab (inline fallbacks).
- **L2 - language setting + detect + persist. [PROTOTYPE BUILT]** `imgui_state.language` (index into
  `g_languages[]`); auto-detects once from the OS (`GetUserDefaultUILanguage` -> en/fr/de/es/it/ja);
  persisted in `SW_RACER_RE.ini` (`[settings] language`). Runtime switch = `localization_apply()`
  (Shutdown + null the racerTab globals + re-parse) - nulling ensures a missing file cleanly falls
  back to English. Dangling-pointer caveat (cached `Translate` results) not yet stress-tested; applied
  live, best changed from a menu screen. Compiles + links clean (verified). Deploy blocked only by the
  game locking dinput.dll while running.
- **L3 - content. [DE + IT done via retail tabs; FR/ES partial]** Retail `racer.tab`s drop in as-is
  (plaintext CP-1252, CRLF, 1183 keys = 523 SCREENTEXT + 304 MONDOTEXT_H + 356 CREDITS_H, **100% of
  the 465 master keys** + credits the Steam exe lacks fallbacks for):
  - **German** (found online) -> `data/lang/de/racer.tab`. 100%.
  - **Italian** from a 1999 IT retail install (`.../languages/starwarsracer/gnome/data/racer.TAB`, the
    canonical disc path) -> `data/lang/it/racer.tab`. 100%.
  - **French** from the GOG "Patch Francais (Catarax)" package (`data/racer.TAB`) -> `data/lang/fr/racer.tab`.
    100% (`La Classique de la Boonta`, `Attente des pilotes...`), replaced the partial Switch extract.
  - **Spanish** from a full Spanish PC restoration package (`racer.tab` + `anims` + `wavs`) ->
    `data/lang/es/racer.tab`. 100% (`Clasica Boonta`, `Esperando a los demas...`), replaced the partial
    Switch extract. (The earlier N64 XDELTA patch and the `.cdi` Dreamcast image were dead ends; this PC
    package is the real source.)
  So **all five EFIGS are now 100% text**: EN(native) + FR + DE + IT + ES. (JA still Switch-only, gated.)
  Validate any tab with `parse-racer-tab.py`; watch DE/IT/FR string length (overflows fixed-width menu
  slots - see layout note). NOTE: retail tabs are copyrighted content - fine locally; shipping them is
  the same maintainer decision as the extracted English corpus.
- **L4 - selection UI. [PROTOTYPE BUILT]** ImGui "Language" combo in the settings panel
  (`imgui_utils.cpp`), applies live + persists. A faithful in-game Options entry (`swrUI`) later.
- **L5 - SDF Latin-1 atlas.** Widen the SDF atlas range to Latin-1; verify accents on the
  SDF-on path (the legacy bitmap path already handles them).

Deliverable: a self-contained PR - real 5-language localization, English default +
auto-detect, SP and vanilla behavior untouched.

## Tier 1.5 - Localized voice + cutscenes (audio/video)
**[VOICE PROTOTYPE BUILT]** `dinput_hook/localization.cpp` wraps `stdPlatform_hostServices.fileOpen`
(`install_av_overlay()`, from `init_localization`): when a non-English language is selected it
redirects `...data\wavs\...` / `...data\anims\...` opens to `data\lang\<code>\...` IF the overlay file
exists (per-file, live via `imgui_state.language`), else the stock file. Voice deltas
for **all four** non-English EFIGS are deployed to `data/lang/<code>/wavs/` (`diff_voice.py`,
~30-38 MB each): FR 312, IT 312, ES 313, DE 314 files. Compiles + links + deploys clean; not yet
playtested. **Cutscenes need their own hook** (the fileOpen wrapper misses them): `Window_PlayCinematic`
@0x004252a0 builds `rootPathName` + `.\data\anims\` + name and hands it to the statically-linked SMUSH
lib (`SmushPlay`), which opens the `.znm` with its OWN file I/O, bypassing `hostServices.fileOpen`.
**[CUTSCENE HOOK BUILT]** `localization.cpp` Detour-hooks `Window_PlayCinematic` (`install_cutscene_hook`)
and, when `data/lang/<code>/anims/<name>` exists, injects a `..\lang\<code>\anims\` prefix into the name -
the game's own `.\data\anims\` + our `..` resolves to `.\data\lang\<code>\anims\<name>`, so SMUSH opens the
overlay. Cutscene `.znm` for all four langs staged under `data/lang/<code>/anims/`. Sources:
FR (GOG Catarax), IT (1999 retail install), ES (Spanish PC restoration package), DE (`SWRacer_DE.exe`,
an **InnoSetup** installer - unpacked WITHOUT running via portable `innoextract`: `innoextract -e -s -d
<out> SWRacer_DE.exe`).

### Cutscene bundling plan (Tier 1.5b) - DECIDED: bundle, distribute as release packs
Measured reality (`.znm` = the SMUSH intro/planet/textcrawl FMVs): cutscenes are **irreducibly big**.
Decompressed cross-language diff (EN/FR/IT/ES): 39 unique blobs / ~980 MB; only `planetG` + `planetJ`
are shared across all languages, `goldie` shared among FR/IT/ES, everything else (introscene, planetA-F,
planetTat, textcrawl) is **unique per language** (baked-in narration + text). The `.znm` are already
**gzip-compressed**, so there is NO dedup / delta / recompress win (unlike voice's 87%-shared delta).
=> ~200 MB compressed per language, ~800 MB for the four dubs.

Implications for "bundle it":
- **Not in git.** ~800 MB of copyrighted FMV would wreck clone size and is a licensing problem. The
  text tabs (tiny) and voice deltas (~32 MB/lang) CAN be bundled; cutscenes cannot live in the repo.
- **Ship as per-language AV release packs** (GitHub Release assets, <=2 GB each; one pack per language =
  voice delta + `anims/*.znm`, ~230 MB). Turnkey UX: when the user picks a language and opts in, the mod
  (or a small first-run downloader) fetches that pack into `data/lang/<code>/`, verifies a hash, and the
  existing file-overlay serves it; falls back to stock EN until present. This is "bundled" from the
  user's side (one confirm, no hunting) without repo bloat.
- **Licensing gate:** hosting LucasArts FMV is the maintainer's call (same as the retail tabs, larger
  stakes). If not hostable, the fallback stays turnkey-ish: a guided auto-extract from a user-owned
  installer/disc (we have the `innoextract` + ISO-carve recipes), NOT "go find loose files".
- **Future (Tier 1.5c) - the real shrink:** transcode the SMUSH `.znm` -> modern video (H.264/AV1) +
  add an FMV/video player to the renderer replacement (the Aspyr console ports already moved cutscenes
  to `.mp4`; precedent exists). That drops each language to ~10-20 MB (small enough to truly bundle in a
  release) and looks better, but it's real engine work - overlaps `GRAPHICS_ROADMAP.md`. Sequence: ship
  the download-pack model first (works today with the built overlay), transcode later.

The retail localized releases ship translated **voice** and **cutscenes**, not just text - e.g. the
GOG French package (`.../languages/Star Wars Episode 1 Racer Patch Francais GOG (Catarax)/data/`)
contains `anims/*.znm` (12 cutscenes: intro, textcrawl, planetA-J, goldie; ~213 MB) and
`wavs/{11K,22K}/Voice` + `wavs/Music` (~998 voice files, ~241 MB). The 1999 disc installs have the
same under `gnome/data/`. This is a natural extension of the SAME `g_language` selector.
- **Mechanism:** the game loads cutscenes from `data/anims/` and voice from `data/wavs/.../Voice/`
  with fixed paths; reuse the `dinput_hook` loose-file redirect (the texture/track path-swap layer,
  see [[asset-replacement-architecture]]) to point those opens at `data/lang/<code>/{anims,wavs}/`
  when `g_language != en`. No re-encode: the assets are already the game's native `.znm` / `.wav`.
- **Layout:** `data/lang/<code>/anims/*.znm`, `data/lang/<code>/wavs/.../Voice/*.wav` (+ Music if a
  release localizes it).
- **Ship only the DELTA (voice).** SWE1R voice is mostly language-neutral alien speech, so most of
  the tree is byte-identical to English. Measured (FR vs stock EN, `scripts/localization/diff_voice.py`,
  md5 by relpath): **2194 / 2506 files identical (87%)**; the real French delta is **312 files / ~32 MB**
  (vs 235 MB full) - entirely `Voice/` (~156 lines at both 11K + 22K), and it's exactly the spoken-English
  lines: Watto (`WTUI` x56), race/game announcer (`RALI`/`GALI`/`RAAC`/`GAIG`/`RAIG` x54), UI/announcer
  (`ASSP`/`ASUI` x44). So a **per-file overlay** works: put only the delta under `data/lang/<code>/wavs/`
  and have the redirect prefer the lang file when present, else the stock English file. At ~32 MB the
  voice delta is small enough to bundle or offer as a tiny download.
- **Cutscenes are the opposite** - largely language-specific (baked-in narration/text crawl) and big
  (~200 MB, 12 `.znm`): keep those user-supplied. (Cutscene diffing must case-normalize filenames -
  the FR/EN `.znm` names differ only in case, e.g. `planetA` vs `PLANETA`.)
- **Gate on presence:** fall back to the stock English AV whenever a language's file is absent.
- **Faithfulness:** default English AV untouched; only redirected when a non-English language is
  selected AND its assets are present. Verify the audio subsystem (SMUSH cutscene path + swrSound
  voice cache, see [[swrsound-subsystem]]) tolerates the swap (sample rates: 11K/22K variants exist).
- Reference test material: the French GOG package above is a complete FR text+voice+cutscene set.

## Tier 2 - CJK (JA / ZH-Hans; later KO / ZH-Hant): engine-gated
Content is ready (Switch dump); rendering is not. Depends on `FONT_SHAPING_ROADMAP.md`.

Render-side gates:
- **Dynamic glyph-ID atlas** (shaping roadmap Phase 2) - cannot pre-bake ~20k Han glyphs;
  rasterize on demand with an LRU/growing cache.
- **UTF-8 decode** in the layout loop (shaping roadmap Phase 5) - today it is byte-based.
- **A CJK font** must be bundled or user-supplied - the Switch cheats via the system shared
  font (`nn::fontll::ScalableFontEngine`) and ships NO CJK font file; PC has none. Candidates:
  Noto Sans CJK / Source Han Sans / M+ (license check; user-supplied model like Womprat/Moki,
  or bundle if the license permits).
- NOTE: basic Han/Kana layout is ~1:1 codepoint->glyph LTR advance, so Tier 2 does NOT need
  full HarfBuzz GSUB/GPOS. The real gates are the **dynamic atlas + UTF-8** - Tier 2 can land
  on a CJK-minimal subset (UTF-8 + codepoint->glyphID dynamic atlas) without waiting for the
  shaping roadmap's Phases 3-6. Mixed Latin+CJK strings still work.
- CJK renders only on the **SDF-on** path; the vanilla bitmap fonts cannot show it. Force
  SDF-on (or block/warn) when a CJK language is selected.

Phases:
- **C0 - extract from the Switch dump. [JA DONE; ZH deferred]** Done 2026-07-02 via
  `scripts/localization/extract_switch_tabs.py`. THE KEY STEP: `main` is an **LZ4-compressed
  32-bit-ARM NSO0** (flags 0x3f) - my first blind scan was reading *compressed* rodata (that's why
  it looked like an "interleaved pointer pool"; the stray bytes were LZ4 match/control tokens).
  Decompress the 3 segments (pure-Python LZ4 block, in the script) into a flat VA image and the
  strings are clean. Layout: .rodata holds per-language string blocks; `.data` (~0x11b8fc-0x11c274)
  holds a pointer table of **5-slot tuples, stride 0x14, column order [EN, FR, DE, ES, JA]**
  (VA == flat-image offset after decompress). Align: find each master English value in rodata, pick
  its EN-column slot (the one whose +0x10 sibling = JA/CJK), read the FR/DE/ES/JA siblings.
  The EN column per pointer-run is picked by the phase that maximizes exact matches to the master
  English values (self-validating). RESULT (in `assets/lang/_switch_extract/`, git-excluded):
  **JA 115 keys, FR 111, ES 108** (~120 keyed tuples), all key-aligned and spot-verified
  (SCREENTEXT_497 Boonta Training -> ブーンタ・トレーニングコース / Entrainement de la Boonta /
  Pista de entrenamiento; _540 Inferno -> インフェルノ). **DE = 0**: German is entirely
  English-fallback in this table (the DE slot reuses the EN pointer; German strings like
  Zeitrennen/Kosten sit unreferenced in rodata). Coverage ceiling ~120 keyed: the table holds
  ~230 tuples but only ~120 have English that exactly matches the 1999 PC master - the rest diverge
  in wording (Switch remaster != PC), so a fuzzy-match pass (risking mis-keys) is the only lift and
  isn't worth it for shelved content. **ZH-Hans NOT extractable statically**: real Simplified-Chinese
  (~99 simplified-only strings like 购买维修机器人 / 邦塔训练赛道 / 达格人德比) has ZERO static
  pointers. (The "25 ZH pointer refs" I first saw were JA-*kanji* in the JA column - 新記録/終了/
  購入/価格 - misclassified by a han-only test; the table is confirmed 5-wide [EN,FR,DE,ES,JA], no
  ZH column.) ZH strings are referenced by runtime-computed addresses -> requires an xref/code pass
  in `main.i64`/IDA, or semantic alignment of the readable `~~` proper nouns. NOT on the critical
  path (Tier-2 renderer unbuilt; FR/DE/ES come from the PC `racer.TAB`), so JA is banked and ZH
  waits for the IDA code pass.
- **C1 - engine (CJK-minimal or shaping P2+P5).** UTF-8 layout + codepoint->glyphID dynamic
  atlas + FreeType raster (FreeType is already required by the shaping roadmap).
- **C2 - CJK font integration.** Bundle/user-supply; add to the SDF slot config.
- **C3 - wire + verify.** `g_language` -> ja/zh-hans; force SDF-on; verify menus, dialogue,
  and overflow.
- **C4 - later.** KO, ZH-Hant (Steam set) and any remaining Steam languages.

## Coverage / gaps
- **Auto-covered:** all ~465 original keys, wherever the original binary calls `Translate`
  (menus, track names, dialogue, prompts). This is the bulk of player-facing text.
- **NOT covered:** mod-added UI (ImGui overlays, our new menu items / labels). Optional small
  separate mod string catalog; low priority; likely English-only (dev-facing).
- **Pre-rendered text in sprites/textures** (`out_spriteblock`) is not localizable via the
  tab; probably minimal now that a TTF path exists - audit if a specific screen needs it.
- **Voice + video** (`voice/` under `wavs`, `anims/` movies) are separate per-language assets
  the community swaps by hand. Out of scope for text loc, but the same `g_language`-keyed
  loose-file redirect could later select localized audio/video.

## Key decisions / open questions
- FR/DE/ES source: PC `racer.TAB` first, Switch as cross-check (RECOMMENDED) vs all-from-dump.
  IT can only come from the PC tab.
- Language-code set + OS-locale mapping; fallback chain (missing key -> inline English is
  already automatic).
- Persistence: `wuRegistry` vs mod config; and how it coexists with a faithful Options menu.
- Hot-reload safety (dangling pointers from cached `Translate` results) - verify or require
  restart on language change.
- Encoding strategy: Latin tabs are CP-1252 while CJK tabs are UTF-8. Decide unified UTF-8
  throughout (UTF-8 is an ASCII superset, but Latin-1 accents differ) vs per-file codepage
  with the pipeline branching. Unified UTF-8 is cleaner long-term but touches the Latin path.
- String-length / overflow: menus assume fixed-width English; long DE/ES strings may clip.
  Ties into `UI_ROADMAP.md` (resolution-independent UI) and the shaping width API
  (`swrUI_GetSubstringWidth` @0x418680, which sums bitmap `swrText_GetCharSize`).
- Force SDF-on for CJK vs surface a clear "requires crisp text" UX when SDF is off.

## Faithfulness + constraints
- English default + auto-detect; SP and vanilla behavior unchanged when English / no tab.
- SDF-OFF stays byte-identical vanilla (unchanged bitmap path); CJK only on the SDF-on path.
- `racer.tab` files are game DATA (loose files), not `src/` headers -> the ASCII-only header
  rule does not apply; keep each tab in its required encoding (CP-1252 Latin / UTF-8 CJK).
- Full `dinput.dll` link with the WinLibs GCC 13.2 MinGW32 recipe before any PR; Tier-2 deps
  (FreeType, etc.) vendored per the shaping roadmap.
- PRs target upstream `tim-tim707/SW_RACER_RE` master; reuse swe1r-tools with attribution.

## RE-grounded reference (for a cold start)
- `swrText_Translate` @0x00421360 (`src/Swr/swrText.c:149`): input `"/KEY/fallback"`; finds
  the 2nd `/`; uppercases the key; `bsearch` `swrText_racerTab_array`; returns VALUE when
  non-empty, else the fallback; returns fallback when the table is NULL.
- `swrText_ParseRacerTab` @0x00421120: reads the file; `RCNE` magic -> XOR 0xdd from offset 4;
  splits `\r`/`\n` lines; `swrText_UnescapeString`; cut at first `\t`; `strupr`; `qsort` by
  `swrText_CmpRacerTab`.
- `Main_Startup` @0x00423d14: `swrText_ParseRacerTab(s___data_racer_tab_004b7bb4)` where the
  string is `data\racer.tab` (the fixed path our redirect intercepts).
- `swrText_Shutdown` @0x00421330: frees the racerTab buffers (used for hot-reload).
- Accented Latin-1: `swrText_extCharComposeIndex` / `swrText_extCharComposePairs` drive the
  extended-glyph composition in the bitmap path (`swrText_GetStringWidth/Height`, `DrawString`).
- SDF seam + font-slot map: `swrText_RenderString` @0x0042ec50 ->
  `swrText_RenderString_delta` (`dinput_hook/game_deltas/swrText_delta.cpp`); see
  `FONT_SHAPING_ROADMAP.md` and memory `font-rendering-sdf`.
- Switch dump: `main` NSO .rodata string table ~0xA4400-0xAE800; 6 langs
  EN/FR/DE/ES/JA/ZH-Hans (no IT/KO/RU); CJK glyphs come from the Switch shared system font
  (`nn::fontll::ScalableFontEngine`), so no CJK font file ships in `romfs/`.

## Effort / sequencing
- Tier 1 is a near-term, self-contained PR (content + selection + redirect). The largest task
  is content QA and fixing fixed-width string overflow, not code.
- Tier 2 is gated on `FONT_SHAPING_ROADMAP.md` Phase 2 + Phase 5 (or a CJK-minimal subset);
  its content (C0) can be extracted from the Switch dump now and shelved until the engine is
  ready.
- Recommended first steps: **L0** (master `en.tab`) + an **L1/L2 prototype** (prove one screen
  switches language in-game), then fill Tier-1 content.
