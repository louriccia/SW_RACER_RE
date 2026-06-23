# SW_RACER_RE -- Debug / Overlay UI Roadmap

**Status:** design (2026-06-17; control inventory revised 2026-06-19). Living document. Owner: lightningpirate.

Goal: turn the single flat ImGui debug window into a **panel-based overlay** that scales to
100+ controls without bloat, splits cleanly into a **player-facing overlay** and a
**developer workbench**, and gives every in-flight feature branch a defined home so they stop
merge-conflicting on the same block. Adds two new player-facing surfaces: **in-race Settings**
(editable mid-race) and a **Quick-Select / practice menu** (Annodue-style racer/track/upgrade
swapping).

Lives in the `dinput_hook/` Detours layer (it is the OpenGL-replacement overlay), specifically
the ImGui code in `imgui_utils.cpp` / `imgui_utils.h`. It does NOT touch `src/` reimpls. The
faithful native options-menu track (`config/settings-menus`, swrUI pages) is a separate,
complementary effort -- see SS3 (Settings dual-home).

> Effort tags: **S** < ~half day, **M** ~1-2 sessions, **L** multi-session. Branch names and
> line numbers are as of 2026-06-17 and must be reconfirmed at implementation time.

---

## 0. Decisions locked (2026-06-17)

- **Two faces, one framework.** A single overlay with a clear divide: a **Player overlay**
  (clean, controller-navigable, pause-aware, ships to players) and a **Developer workbench**
  (dockable panels, mouse+keyboard, runs live, hidden behind a build flag). Both ride the same
  shell. Players never see dev clutter.
- **Panel registry, decentralized.** Each subsystem registers its own panel from its own delta
  file. The shell composes them; it never grows per-feature. This is the core fix for the
  merge-conflict problem (SS1).
- **Four-bucket taxonomy by audience+lifetime:** Settings (persisted, player), Tuning
  (live, dev), Inspectors (read-mostly, dev), Tools/Actions (dev). Cheats and Quick-Select are
  player buckets layered on top.
- **Settings dual-home contract.** The overlay Settings panel and the native swrUI options pages
  (`config/settings-menus`) read/write the SAME ini-backed state so they can never disagree. See
  SS3.
- **Pause-policy is per-panel, not global.** Each registered panel declares whether opening it
  pauses the game. Quick-Select pauses (you are rebuilding the race); Tune/Inspect run live (you
  are observing motion). A field on the panel struct, not a mode switch.
- **ImGui docking branch deferred, not rejected.** The registry is forward-compatible with
  docking + multi-viewport (drag panels onto a 2nd monitor). Adopt later by adding a dockspace to
  the shell; do not block the restructure on the vendored-dependency swap. See SS7.

---

## 1. The problem (why now)

Today the entire menu is one function, `opengl_render_imgui()`
(`dinput_hook/imgui_utils.cpp:306`), rendering ~30 controls top-to-bottom. Findings from the
2026-06-17 stock-take:

- **No window of its own.** There is no `ImGui::Begin/End` -- everything lands in ImGui's implicit
  "Debug" window. Step zero is giving panels explicit windows.
- **Merge-conflict generator.** Three in-flight branches each splice into the SAME two spots (the
  `graphics settings` tree at ~line 313 and the top-level checkbox run): `feature/debug-cheats-menu`
  (adds a "Debug / Cheats" tree, 10 controls), `feature/gamepad-rumble` (rumble toggle + intensity
  slider into graphics settings), `perf/remove-per-frame-glfinish` (window-mode combo + VSync +
  glFinish checkbox into graphics settings). They will collide pairwise on merge.
- **Mixed audiences.** Persisted player settings (MSAA, anisotropy, fog, widescreen, gamepad nav)
  sit inline next to dev-only inspectors (node-prop dumps, render modes) and ephemeral knobs
  (`some Ui x/y` sliders, "matrices pos").
- **Trajectory.** ~30 controls today -> ~45 once the three control-bearing branches merge -> ~60
  once the built-but-headless features (SS3 list) get knobs -> 100+ across the six roadmaps, with
  at least 3 places wanting tables and 2 wanting plots/overlays.

---

## 2. Target architecture

### 2a. The shell -- panel registry + menu bar. Effort: M

```cpp
// debug_ui.h
struct DebugPanel {
    const char *category;   // "Render", "Gameplay", "Netcode", "Tools" -- menu grouping
    const char *name;       // "AI Tuning"
    void (*draw)();         // panel body (the existing TreeNode contents move here)
    bool dev_only;          // gate behind the workbench build flag
    bool pause_on_open;     // per-panel pause policy (SS0)
    bool open;
};
void debug_ui_register(DebugPanel *panel);   // each subsystem calls this from its own file
void debug_ui_render();                        // menu bar + the open windows
```

```cpp
void debug_ui_render() {
    if (!show_imgui) return;
    if (ImGui::BeginMainMenuBar()) {
        for (auto &cat : categories) {
            if (ImGui::BeginMenu(cat.name)) {
                for (DebugPanel *p : cat.panels)
                    ImGui::MenuItem(p->name, nullptr, &p->open);
                ImGui::EndMenu();
            }
        }
        ImGui::EndMainMenuBar();
    }
    for (DebugPanel *p : panels) {
        if (!p->open) continue;
        if (ImGui::Begin(p->name, &p->open)) p->draw();   // own window each
        ImGui::End();
    }
}
```

- Each panel is its own `Begin/End` window; ImGui persists pos/size/collapsed/open to `imgui.ini`
  for free.
- Adding a feature = `debug_ui_register(&my_panel)` in that feature's delta file. The shell never
  grows; the two contested insertion points disappear.

### 2b. Two faces
The menu bar groups panels left (player) and right (dev) of a divider. A build flag / setting
(parallel to the existing `RENDERER_REPLACEMENT` toggle) decides whether the dev group renders at
all. Player face is fully controller-navigable (rides the shipped gamepad-nav work).

### 2c. Four buckets
Settings | Tuning | Inspectors | Tools. Cheats and Quick-Select are additional player buckets. The
bucket maps to the panel `category` (menu grouping).

### 2d. Persistence
Two ini files, kept distinct:
- `imgui.ini` -- ImGui's own window geometry / layout (automatic).
- `SW_RACER_RE.ini` -- game/player settings, via the existing `read_settings_ini` /
  `save_settings_ini` (`imgui_utils.cpp:72` / `:91`). All Settings-bucket controls persist here.

---

## 3. Full control inventory

Authoritative inventory (revised 2026-06-19). Two faces (SS0): the **Player face** groups are the
top-level Gameplay / Pod / Race Settings / Profile sections plus a Settings sub-tree; the
**Developer face** is Debug + Inspect, gated behind the workbench flag (SS2b). Each group below
maps to one registered panel (SS2a).

### Player face

**Gameplay**
- Frame Rate Cap
- Physics Scale (hz)
- Game Speed
- FPS/perf overlay
- Skip cutscenes [planet, intro, pod sweep, cantina, podium, pod unlock]
- countdown style [trimmed, 2 second, 1 second, quick 3-second, none]
- run while unfocused [ ]
- localization [language]
- aurabesh mode

**Pod**
- Racer (+secret pods)
- [ ] Skin (show different racer cosmetically but race with above racer's stats)
- Upgrades; each stock->max stepper; Max all / Stock all; health 0-255

**Race Settings**
- Track (dropdown + toggle alphabetical/planet/circuit sorting) (custom tracks?) (restart on selection?)
- Laps
- Mirror Mode [ ]
- Winnings
- [START/RESTART] [QUIT] [SAVE]

**Race Settings -> Grid (AI)**
- Number of Racers
- Difficulty
- Stats
- Collision on/off
- Spread
- Racers (favorite + random / all random / all same / custom)
- Player grid position

**Profile / Unlocks**
- select profile (new/delete)
- [ Unlock all ]
- track unlocks / win states
- pod / upgrade unlocks
- edit saved times
- pit-droid count
- Truguts

#### Settings

**Controls**
- mouse enabled [ ]
- joystick enabled [ ]
- keyboard enabled [ ]
- save/load loadout presets
- gamepad nav (live)
- rumble + intensity
- deadzone
- sensitivity
- invert axes (x, y, z)
- rebind (later)
- input visualizer

**Display**
- Resolution
- Display Mode (windowed / borderless / fullscreen)
- Refresh Rate
- V-Sync
- Frame Rate Cap
- Brightness / Gamma
- Aspect Ratio

**Detail & Quality**
- Resolution Scale / Supersampling
- Anti-Aliasing (MSAA)
- Anisotropic Filtering
- Texture Detail (native low/med/high)
- Model Detail (LOD)
- Draw Distance
- Full Track Geometry
- Pod Detail / Full Pod LOD

**Lighting & Reflections** (native)
- Dynamic Lighting
- Surface Reflections
- Z-Buffer Effects
- Lens Flare
- Image-Based Lighting / PBR

**Effects & Atmosphere**
- Fog
- Smoke & Dust
- Weather Effects
- Pod Shadow
- Heat Haze / Engine Shimmer (optional)

**Camera**
- mode picker (Player / Free) [ PHOTO MODE ]
- Player Camera: sub-mode [near, far, bumper]; chase distance/height/offset drags; shake toggle + intensity; true cockpit [ ]; camera roll slider; FOV
- Free Camera: Target; Anchor; Show anchor [ ]; Anchor rotation [ ]; [ SCREENSHOT ]

**UI**
- text replacement [ ]
- granular visibility toggles (racer numbers, guide arrow, etc.)
- widescreen UI / scale mode (live + UI roadmap)
- map mode
- show readout
- units (fps/mph/kph)
- show milliseconds on all timers
- HUD style / cycle-HUD
- font
- opacity
- scale
- cursor [game / native]

**Asset Replacement**
- HD textures
- HD models (live)

**Audio**
- master
- music
- lap 3 music only
- in-race voices
- 3D sound
- sound reflections / reverb
- SFX
- engine volume
- warning beeps
- engine sfx style [pc / console]
- doppler
- use high quality audio

**Cheats**
- damage on/off
- god mode
- fast mode
- disable out-of-bounds timer [ ]
- anti-grav/fly
- autopilot
- infinite boost / no-overheat
- Cy Yunga [ ]
- Jinn Reeso [ ]
- Dewback in cantina [ ]
- Ronto in cantina [ ]
- [Unlock all pods & tracks]
- [+1000 truguts]

### Developer face

**Debug**
- enable native debug menu (exposes DebugMenuState)
- Show spline
- Show collision
- Show surface tags
- Show xyz
- Telemetry
- AI -- per-racer table (AILevel/speed/spread/catchup/pos/LOD, sortable); global difficulty sliders; catchup-curve plot; LOD/physics gate toggles
- Flight model -- PodHandlingData live sliders, 15 fields; speed/heat/boost plots; reset-to-defaults
- network update time
- Netcode -- async-send toggle
- pump-cap slider
- Set spawn on spline (slider)
- lag-sim (latency/jitter/loss)
- send-time plot
- death speed min
- death speed drop

**Inspect**
- Scene -- root-node tree, node/material prop dumps, render modes, banned sprite flags
- Textures -- hover picker, collect-visible, replacement browser; texture-memory budget
- Race state -- standings table; pod readout (pos/vel/heat/health/boost); spline-cursor/track-progress; checkpoint/waypoint overlay
- Render -- draw meshes/RenderList/cubemaps/LUT, draw-test-scene, matrices-pos
- UI debug -- hit-test rect + anchor overlay; scale/cursor mode
- Replay (roadmap) -- record/stop/play/pause; scrub timeline (frame slider + keyframe ticks); playback speed; ghost (enable/load/compare); save/load/export; free-cam during playback
- Net / Multiplayer -- session info; peer table (ping/loss/state/extrapolation); async-send/pump-cap/lag-sim; local splitscreen player-count + per-player viewport (built, dormant); spectate
- Window / Layout (framework) -- per-panel show/hide; saved layout presets ("Player", "AI tuning", "Netcode", "Rendering"); theme; overlay opacity; font scale; reset layout
- frame advance

---

## 4. The Quick-Select menu -- the one genuinely new subsystem. Effort: L (spike first)

Everything else is reorganizing controls that exist or are branched. Quick-Select is new: it needs
**live write-access to racer / track / upgrade / field state mid-session** without a hangar round
trip. It overlaps but is not covered by modding-content (data map), racer_count_limit (field size),
or the part-shop reimpl (upgrade application).

De-risk with a spike answering: which of {swap racer, reapply upgrades, change track} can be done
on a live race object vs. require a re-init through the track-load pipeline
(swrObjJdge_InitTrack)? Racer/upgrade swaps are likely live-mutable; a track change almost
certainly needs a restart through the spinup path. Outcome decides whether Quick-Select edits in
place or stages a "restart with these settings" payload.

---

## 5. Work plan (phased)

### Phase A -- Shell. Effort: M
Stand up the panel registry, menu bar, two-face build flag, and per-panel pause field. Wrap the
current monolith in ONE registered panel first (behavior-identical, behind F5 as today) to prove
the shell with zero behavior change.

### Phase B -- Migrate the live menu into panels. Effort: M
Split `opengl_render_imgui()` into its natural panels: Graphics Settings, Render Debug, Scene
Inspector, Texture Inspector, Tools. Rehome the orphan `some Ui x/y` sliders and "matrices pos"
into Inspect. No new features -- pure decomposition. After this, the two contested insertion points
no longer exist.

### Phase C -- Land the in-flight branches as panels. Effort: S each
Rebase the three control-bearing branches onto the registry: Cheats panel
(feature/debug-cheats-menu), rumble controls into Settings/Controls (feature/gamepad-rumble),
window-mode/VSync/glFinish into Settings/Display+Performance (perf/remove-per-frame-glfinish). Each
becomes an independent panel registration -- they stop touching shared code, so they stop
conflicting.

### Phase D -- Quick-Select. Effort: L
After the SS4 spike. Build the Race panel; player-facing, pause-on-open, controller-navigable.

### Phase E -- Roadmap panels. Effort: L (parallelizable)
One panel per roadmap as each lands: Camera, AI Tune (table + plots), Netcode, Replay (timeline),
UI anchor inspector (overlay), Net/peer table, local splitscreen. Adopt the bigger ImGui widgets
here (BeginTable, PlotLines, foreground DrawList overlays).

### Cross-cutting (any phase)
Controller navigation for the player face; search/filter (ImGuiTextFilter); presets/profiles to
ini; later, docking + pop-out (SS7).

---

## 6. Status

- **DONE / shipped:** the monolith menu itself (`opengl_render_imgui`), gamepad-nav toggle, the
  graphics-settings persistence path.
- **READY to fold in:** feature/debug-cheats-menu, feature/gamepad-rumble,
  perf/remove-per-frame-glfinish (all have working ImGui controls today, just in the wrong place).
- **NEXT:** Phase A (shell), then Phase B (decompose), which together unblock the merge churn.

---

## 7. ImGui capability notes (what the panels unlock)

- **BeginTable** (sortable/resizable) -- AI per-racer, standings (swrScores[20]), multiplayer peers.
  Replaces every flat Text dump.
- **PlotLines / PlotHistogram** -- FPS history, netcode send-time, pod speed/heat over time.
- **Foreground/background DrawList** -- overlays drawn over the game: UI hit-test rects + anchors
  (UI roadmap), AI spline/waypoint paths, checkpoint markers. No window needed.
- **Combo / ListBox** -- camera mode, track/racer pickers.
- **DragFloat/DragFloat3** -- unbounded tuning (camera offsets, physics constants).
- **Child + auto-scroll** -- the log viewer (currently a single growing Text) -> scrolling region +
  copy.
- **Docking branch (deferred):** dockable/tabbed panels + multi-viewport (drag a panel onto a 2nd
  monitor -- tune on monitor 2 while the race runs fullscreen on monitor 1). Vendored-dependency
  swap from imgui-1.91.1 master to the docking branch + viewport init in the GLFW/OpenGL3 backends.
  The registry adopts it by adding `DockSpaceOverViewport()` to the shell without touching panels.

---

## 8. File / branch / function reference

| Item | Location | Role |
|------|----------|------|
| opengl_render_imgui | dinput_hook/imgui_utils.cpp:306 | the monolith to decompose |
| imgui_Update | dinput_hook/imgui_utils.cpp:269 | ImGui init + per-frame NewFrame/Render |
| ImGuiState | dinput_hook/imgui_utils.h:13 | shared menu state struct |
| read_settings_ini / save_settings_ini | dinput_hook/imgui_utils.cpp:72 / :91 | SW_RACER_RE.ini persistence |
| show_imgui (F5) | dinput_hook/imgui_utils.cpp:44 | overlay visibility toggle |
| feature/debug-cheats-menu | branch | Cheats panel (10 controls) |
| feature/gamepad-rumble | branch | rumble toggle + intensity slider |
| perf/remove-per-frame-glfinish | branch | window mode / VSync / glFinish |
| config/settings-menus | branch | native swrUI options pages (Settings dual-home) |
| ai-opponent-difficulty / ai-full-lod | branch | AI knobs/gates (no UI yet) |
| feature/100-lap-races | branch | laps cap (native menu, no overlay UI) |
| feature/modding-content | branch | track/racer/circuit data map |

---

## 9. Open questions

- **Quick-Select mutation model** -- in-place edit vs. staged "restart with payload" (resolved by
  the SS4 spike).
- **Settings ownership** -- is the overlay Settings panel a permanent parallel to the native swrUI
  pages, or a dev convenience the native menu eventually supersedes? Affects how much polish the
  overlay version gets.
- **Dev-face gating** -- reuse the `RENDERER_REPLACEMENT`-style build flag, or a runtime setting, or
  both (build flag strips it from release, runtime toggle for devs)?
- **Docking timing** -- adopt the docking branch as part of Phase E (when panel count makes window
  management painful), or stay on master indefinitely?

---

## Cross-references
- `UI_ROADMAP.md` -- the native resolution-independent UI / options-menu track (Settings dual-home).
- Memory: gamepad nav bridge, rumble XInput bridge, ifly_cheat (cheats), camera/cMan subsystem,
  replay system investigation, multiplayer + local multiplayer, ai opponent difficulty,
  pod handling stats, racer count limit, modding content system, renderer perf investigation.
