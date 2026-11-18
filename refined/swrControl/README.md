# swrControl refinement dossier

**Status:** dossier (2026-11-18). Pilot subsystem for R1 of `REFINEMENT_LAYER_ROADMAP.md`.
Base: `upstream/dev @ ebc920bf`. Faithful source: `src/Main/swrControl.c` / `.h`.

**Gate status:** not passed. #285 (hook-mode switch + differential harness) is OPEN, and no
swrControl body has run in `reimpl` mode. The first work session verifies the faithful layer
(step 0); refined code starts after that.

**Open decisions** (roadmap §9, plus two local ones):
- Location of `refined/` (upstream vs separate tree). Not agreed with tim yet.
- C vs C++ for refined code.
- User profile format: keep `<name>_control.map` byte-compatible (recommended; players have
  these files) and use JSON only for shipped defaults and the action table?
- How far step 6 may reach into `stdControl` (the DirectInput layer). Several limits in this
  subsystem are really stdControl limits.

---

## 1. Boundary

Subsystem = input binding/mapping, per-frame input processing, menu accept/cancel polling, and
the force-feedback (IFORCE) driver. The DirectInput layer (`stdControl_*` in
`src/Platform/stdControl.h`) is a neighbour, not part of this scope.

### Functions

| Address | Ghidra name | Faithful | Notes |
|---|---|---|---|
| 0x402250 | `swrControl_MappingsMenu` | **stub** | swrUI menu callback for the controls pages |
| 0x404b10 | `swrControl_Initialize` | full | |
| 0x404da0 | `swrControl_Shutdown` | full | |
| 0x404dd0 | `swrControl_ProcessInputs` | full | per-frame core; **replaced by a delta today** |
| 0x405cf0 | `swrControl_ClearDeviceState` | full | |
| 0x405dd0 | `swrControl_ScanPressedButtons` | full | |
| 0x405ea0 | `swrControl_SetDefaultMappings` | full | |
| 0x406470 | `stdConfFile_readAndApplyConf` | full | the `.map` loader; misleading prefix |
| 0x406a50 | `swrControl_FormatBinding` | full | |
| 0x406cc0 | `swrControl_CaptureBinding` | **none** | header-only; capture loop for a rebind |
| 0x407500 | `swrControl_RemoveMapping` | full | |
| 0x407630 | `swrControl_ApplyAxisConfig` | full | |
| 0x407700 | `swrControl_FindMovedAxis` | full | |
| 0x407800 | `swrControl_ClearBindings` | full | |
| 0x4078a0 | `swrControl_ReplaceMapping` | full | |
| 0x4078e0 | `swrControl_AddMapping` | full | |
| 0x4079f0 | `swrControl_FindMapping` | full | |
| 0x407a90 | `stdConfig_getKeymap_id` | full | misleading prefix |
| 0x407cd0 | `swrControl_ParseFunctionName` | full | |
| 0x407d90 | `swrControl_FindKeyName` | full | |
| 0x407de0 | `swrControl_SelectSavedJoystick` | full | |
| 0x407ea0 | `swrControl_PollAccept` | full | |
| 0x407f80 | `swrControl_PollCancel` | full | |
| 0x408020 | `swrControl_IsKeyStringControl` | full | |
| 0x408040 | `stdControl_isAxisAboveDeadzone` | full | misleading prefix |
| 0x408120 | `swrControl_SnapshotKeyboard` | full | |
| 0x4081c0 | `swrControl_ClearPollState_Maybe` | full | clears swrUI input-change flags |
| 0x409d70 | `swrControl_LoadForceEffects` | full | |
| 0x409ee0 | `swrControl_PlayForceEffect` | full | |
| 0x40a0b0 | `swrControl_StopForceEffect` | full | |
| 0x40a120 | `swrControl_StopAllForceEffects` | full | |
| 0x40a160 | `swrControl_FindForceEffectSlot` | full | |
| 0x40a190 | `swrControl_CheckForceEffectType` | full | |
| 0x40a240 | `swrControl_PrepareForceEffect` | full | |
| 0x40a330 | `swrControl_ConfigureForceEffect` | full | |
| 0x40a500 | `swrControl_UpdateForceEffect` | full | |
| 0x40b110 | `swrControl_SetForceFeedbackPlayer` | full | |
| 0x40b150 | `swrControl_UpdateForceFeedback` | full | called from inside the physics contact update |
| 0x40b1c0 | `swrControl_UpdateTractionForceEffect` | full | |
| 0x40b3d0 | `swrControl_UpdateSpeedForceEffect` | full | |
| 0x40b5e0 | `swrControl_UpdateImpactForceEffect` | full | |

**Out of scope:** `swrControl_Startup` @ 0x423efd. It's not an input function: Ghidra shows a
fragment of `Main_StartupSoundAndControl` (the body starting at an unaligned address, entered
from 0x411366) that calls `swrControl_Initialize` and then does display registry saves,
`swrRender_InitScene`, `swrScene_Startup`, `swrUI_BuildMenuPages`, `swrRace_InitGameData` and a
`ShowWindow` through an unaffected `EBX`. It is a carve/naming problem for the Main subsystem;
fix it there (separate naming PR), not here.

### Owned globals (from the faithful bodies)

- **Binding tables:** `swrControl_bindingsJoystick` / `Mouse` / `Keyboard` (12-byte
  `swrControlBinding {flags, input, action}`, 0xff-flags terminated), `swrControl_bindingCounts[3]`,
  default tables `swrControl_defaultBindings{Joystick,Mouse,Keyboard}`.
- **Axis config:** `swrControl_joystickFlipAxis[6]`, `flip_x_axis`/`flip_y_axis`/`flip_z_axis`
  (used as an array by address), `swrControl_axisSensitivity[]`, `Deadzone`,
  `swrControl_defaultDeadzone`, `swrControl_joystickAxisScale[6]`, `swrControl_joystickAxisMask`,
  `swrControl_mouseAxisMask`, default flip tables.
- **Per-frame state:** `swrControl_joystickAxes`/`mouseAxes`/`keyboardAxes`, `swrControl_axesPrev`,
  `swrControl_mouseAxesAccum`, `JoystickAxisInput`/`MouseAxisInput`/`KeyboardAxisInput`,
  `*ButtonPressedInput` (×3), `swrControl_brakeIsAnalog`, `swrControl_buttonHeldPrev`,
  `swrControl_actionEdgeOnly`, `swrControl_viewHoldTime[]`, `swrControl_viewButton[]` (both used
  as arrays by address), `swrControl_inputRecordActions`.
- **Accept/cancel edges:** `swrControl_accept*`/`cancel*` `PressedEdge`/`ReleasedEdge`/`HeldPrev`/
  `UpPrev`, `swrControl_menuAccept*`, `swrControl_menuAcceptGuard`, `swrControl_mouseSuppressed`,
  `swrControl_uiInputActive`, `swrControl_bindingCaptureActive`.
- **Force feedback:** `swrControl_forceEffects` (cifr project + slots),
  `swrControl_forceEffectSettings[]`, `forceEffectValue[]`, `forceEffectPlaying[]`,
  `forceEffectNames`, `forceEffectSlotLastIds`, `forceEffectsDeviceType`, `forceEffectsLoaded`,
  `forceStrength`, `swrControl_ff*` (score, max speed, ratios, last coeff/period/magnitude, boost,
  tilt, traction flags), `currentPlayer_Test`.
- **Key/action tables:** `keyMapping0` (axes), `keyMapping1` (buttons), `keyMapping2` (keys),
  `keyMapping2_0` (the 22-entry bindable action table), `swrConfig_aControlKeyStrings` (reserved
  keys).

**Written outside the subsystem's own state** (seams, see below): `swrRace_ThrottleInput` and the
three neighbouring analog inputs (used as an array by address), `inRaceLocalPlayerInputRecord`,
`swrConfig_joystick*`/`mouse*` caps and `*_enabled`, `joystick_detected`, `mouse_detected`,
`enableWindowInput`, the Windows key queue (`WindowsInputStack*`, `stdControlInputItems`),
`swrUI_inputChangeFlags`.

### Seams (crossing edges; static call graph from 2026-06-21, so approximate)

**Called from outside:**
- `swrMain2_GuiAdvance`, `Window_SmushPlayCallback` → `ProcessInputs`
- `Main_Shutdown` → `Shutdown`; `Main_Startup` → `ClearPollState_Maybe`
- `Window_SmushPlayCallback`, `swrMain_UpdateNetworkTick`, `swrObjHang_LoadScreen`,
  `swrRace_HandleDeathSnap` → `ClearDeviceState`
- `swrUI_Menu_SaveLoadConfig` → `stdConfFile_readAndApplyConf`
- `swrConfig_SetMappingRowText` → `FormatBinding`; `swrConfig_WriteMappings` → `FindKeyName`
- FF, from race/hangar/pause/UI: `PlayForceEffect` (7 callers incl.
  `swrRace_UpdatePlayerControl`, `swrRace_ApplyEngineDamage`, `swrObjTest_F4`, `swrObjHang_F0`,
  `swrUI_Menu_ForceFeedback`), `StopForceEffect` (5), `StopAllForceEffects` (7 incl.
  `requestPause`, `swrRace_Explode`, `swrMain_ProcessDebugKeys`), `SetForceFeedbackPlayer`
  (`swrRace_Init`, `swrRace_HandleDeathSnap`), `UpdateForceFeedback`
  (`swrObjTest_UpdatePhysicsContact`), `LoadForceEffects` (`swrRace_MainMenu`)
- Indirect (UI callback pointers, not in the static graph): `MappingsMenu`, `CaptureBinding`
- **Data readers** (not calls): physics/race code reads `swrRace_*Input`; the MP/replay path reads
  `inRaceLocalPlayerInputRecord`; the hangar, cutscenes and UI read the accept/cancel edges.

**Calls out:**
- `stdControl_*` (ReadControls, ReadAxis, ReadKey, ReadKeyAsAxis, ReadAxisAsKeyEx, EnableAxis,
  SetAxisDeadzone, SetMouseRange, Startup/Open/Close/Shutdown)
- `cifr_*` (Immersion IFORCE SDK wrapper)
- `stdConffile_*`, `stdString_RemoveTEXTTag`, `wuRegistry_GetString`
- `swrConfig_*` (Read/WriteForceFeedbackConfig, SetDefaultForce, WriteMappings,
  ControlToString)
- `swrText_Translate`, `swrUI_GetCurrentPage`, `swrUI_GetMenuRoot_Maybe`, `playUISound`
- `swrModel_MeshGetBehavior` (FF terrain check)
- Reads `g_objHang2->menuScreen` and the race/pod structs (`swrRace`, `swrObjTest` flags,
  engine health/status, terrain model, speed, tilt)

## 2. Tier table

Rule of thumb: anything whose output feeds the simulation, the MP input record or a replay is
**sacred**. Binding-table manipulation is **equivalent** with a strict contract: the resulting
tables must be byte-identical. UI and force feedback are **equivalent** (they only reach the
screen or the device). No function in this subsystem draws from `swrUtils_Rand`.

| Function | Tier | Reason |
|---|---|---|
| `ProcessInputs` | **sacred** | Produces `swrRace_*Input` floats, `inRaceLocalPlayerInputRecord` and the action buttons the physics reads |
| `ClampUnit` (static helper) | **sacred** | x87 clamp, NaN behaviour matters (see the zero-ms NaN bug) |
| `stdControl_isAxisAboveDeadzone` | **sacred** | Axis-as-button decisions inside ProcessInputs |
| `ClearDeviceState` | **sacred** | Zeroes the input record and per-device axes mid-race (death snap, network tick) |
| `ApplyAxisConfig` | **sacred** | Sets axis scale and deadzone that ProcessInputs multiplies by |
| `PollAccept`, `PollCancel` | equivalent | Menu/cutscene edges only, but consumed in ProcessInputs; keep the call order |
| `SnapshotKeyboard` | equivalent | Text/menu key queue |
| `SetDefaultMappings`, `ClearBindings` | equivalent | Byte-identical tables. `ClearBindings` also zeroes `swrRace_*Input`: keep that |
| `stdConfFile_readAndApplyConf` | equivalent | Byte-identical tables and flags for every input file |
| `Add/Remove/ReplaceMapping`, `FindMapping`, `ParseFunctionName`, `stdConfig_getKeymap_id`, `FindKeyName`, `IsKeyStringControl` | equivalent | Table queries and edits |
| `FormatBinding` | equivalent | Display text only |
| `ScanPressedButtons`, `FindMovedAxis`, `CaptureBinding`, `MappingsMenu` | equivalent | Rebind UI |
| `SelectSavedJoystick`, `Initialize`, `Shutdown`, `ClearPollState_Maybe` | equivalent | Startup and device selection |
| All force-feedback functions | equivalent | Output to the device only; reads race state, writes none |

**Fix-tier candidates** are listed in the flaw inventory with tag `BUG` and in step 7.

## 3. Flaw inventory

Sources: the faithful bodies on dev, the 2026-07 controller-config audit, the binding-crash
investigation, the unified-controls-table work and the DualSense report.

| Tag | Address | Function | Finding | Proposed refinement |
|---|---|---|---|---|
| DECOMP | 0x404dd0 | ProcessInputs | `(&swrRace_ThrottleInput)[i]`, `(&swrControl_viewButton)[i]`, `(&swrControl_viewHoldTime)[i]`, `(&flip_x_axis)[i]`: neighbouring globals used as arrays by address | Real arrays in the refined state; mirror to retail addresses through a seam |
| DECOMP | 0x404dd0 | ProcessInputs | `*(short*)&inRaceLocalPlayerInputRecord[0]` / `[2]`: untyped record bytes | A typed `swrInputRecord {short steerX, steerY; char actions[N]}` |
| CLEVER | 0x407500, 0x4078e0, 0x4079f0 | Remove/Add/FindMapping | `cid` is either a device id (0/1/2) or a table pointer | Separate functions per form; device enum |
| CLEVER | 0x406a50 | FormatBinding | A custom table arrives through `flagsMask`, whose low byte is then also the mask | Explicit table argument |
| CLEVER | 0x404dd0 | ProcessInputs | Binding flags overload kind/source/analog/direction; POSITIVE tests `isAxisAboveDeadzone(-direction)` and NEGATIVE `(+direction)`, the origin of the axis-as-button flip bug | Named flag enum with one meaning per bit; document the sign chain once |
| UNSAFE | 0x407500, 0x4078e0 | Remove/AddMapping | With a table pointer, `swrControl_bindingCounts[(int)cid]` indexes by pointer value (audit #14) | Count lives with its table |
| UNSAFE | 0x4078e0, 0x406470 | AddMapping, loader | Appends without checking `SWR_CONTROL_BINDINGS_PER_DEVICE` | Bounded append with an error |
| UNSAFE | 0x404dd0 | ProcessInputs | Mouse analog path indexes the 3-entry mouse flip table with any axis id | Bounds check |
| UNSAFE | 0x406470 | loader | `sprintf` of `configName` into `path[256]` and `fileName[32]`, no bound | Sized path builder |
| UNSAFE | 0x406a50 | FormatBinding | A translated string is the `sprintf` format (`/MONDOTEXT_H_0213/BUTTON %s`, `..._0216/%s%s AXIS`); a `%` in a translation reads the stack (audit #1) | Fixed format, translated parts as arguments |
| UNSAFE | 0x407630 | ApplyAxisConfig | `1.0f / swrControl_axisSensitivity[MOUSE]` with sensitivity 0 when the file has no mouse SENSITIVITY line (audit #2) | Validate at load |
| UNSAFE | 0x409ee0, 0x40a240 | Play/PrepareForceEffect | Slot -1 forms a pointer before the table; an empty slot writes index -1 of two arrays | Explicit "no slot" path |
| UNSAFE | 0x40a330 | ConfigureForceEffect | `params[7] / coeff` with `coeff` possibly 0 | Guard |
| UNSAFE | 0x407de0 | SelectSavedJoystick | Narrowing loop runs until it sees `}`; `cchMax` passed as a byte count | Bounded conversion |
| UNSAFE | 0x406cc0 | CaptureBinding | `FindKeyName` returns NULL for ids outside the action table, then `stricmp(NULL)` crashes (binding-crash CTD) | Name lookup never returns NULL to a comparer |
| BUG | 0x407800 | ClearBindings | Clears a table without writing the 0xff terminator; a save then walks off the end (thousands of junk lines) | Always terminate. **Fix tier**, though it can't change race behaviour |
| BUG | 0x406470 | loader | One unparseable line clears the whole device's bindings (the "bindings keep getting wiped" report) | Skip the bad line, log it. **Fix tier** |
| BUG | 0x406cc0 | CaptureBinding | A conflict prompt opens a modal dialog inside the capture loop and hangs | Resolve the conflict outside the loop. **Fix tier** |
| BUG | 0x404dd0 | ProcessInputs | A screen load resets the down-trackers, so a held Enter/Esc fires again and skips the next screen | Edge tracking that survives screen loads. **Fix tier** |
| LIMIT | 0x405dd0, 0x407700 | ScanPressedButtons, FindMovedAxis | Hard-coded 32 joystick buttons, 4 mouse buttons, 6 joystick / 3 mouse axes; ignores the device caps (audit #5) | Capacities from device caps (needs a stdControl seam: it reads only 16 buttons and 6 axes) |
| LIMIT | 0x407cd0 | ParseFunctionName | Bindable actions are a fixed 22-entry table in `.rdata` (`keyMapping2_0`); adding an action (HUD cycle, camera views) needs an operand patch | Action table as data |
| LIMIT | 0x404dd0 | ProcessInputs | Output record has fixed action slots (`swrControl_inputRecordActions`; slots 5/6/7 unused) | Data-defined record mapping |
| HARDCODE | 0x404dd0 | ProcessInputs | Arrows and numpad always drive the keyboard axes, outside the binding tables | Default bindings in data |
| HARDCODE | 0x407ea0, 0x407f80 | PollAccept/Cancel | Accept = button 0, mouse 0, Enter/numpad Enter/Space; cancel = button 1, mouse 1, Esc; wheel throttle above a threshold accepts and brake cancels. Wrong for raw PlayStation pads | "Menu accept/cancel" as bindable actions with these as defaults |
| HARDCODE | 0x405ea0 | SetDefaultMappings | Default tables in `.rdata`, Xbox-ordered (wrong for raw DualSense) | Default binding files per device family |
| HARDCODE | 0x40b1c0–0x40b5e0 | FF drivers | Tuning constants inline: 200000/150000/20000/8000 period, 6500 g-force scale, 9500 boost, 500 floor/hysteresis, 50/10 rumble | FF tuning file |
| HARDCODE | 0x409d70 | LoadForceEffects | `.\data\bundle.fcr` / `bundle2.fcr` paths inline | Config |
| FRAMERATE | 0x404dd0 | ProcessInputs | `swrControl_viewHoldTime[i] += swrRace_deltaTimeSecs` per call; ProcessInputs runs per frame, also from the FMV callback | Decide which clock hold timers use; consistent under fixed timestep |
| GLOBAL | (all) | (all) | About 60 mutable globals across tables, per-frame state, edges and FF | `swrControlState` with one owner (§4) |
| TANGLE | 0x404dd0 | ProcessInputs | Reads `g_objHang2->menuScreen` and `swrUI_GetCurrentPage`, plays a UI sound | UI passes its mode in; sound played by UI |
| TANGLE | 0x407ea0 | PollAccept | Reads `swrUI_GetMenuRoot_Maybe` to decide whether Space accepts | Same: UI passes text-entry state in |
| TANGLE | 0x40b150–0x40b5e0 | FF drivers | Read pod internals directly (`currentPlayer_Test` flags, engine health/status, terrain behaviour, speed, tilt) from inside the physics contact update | Race fills a small `ForceFeedbackInput` struct; FF consumes only that |
| TANGLE | 0x404dd0 | ProcessInputs | Writes race globals and the MP record directly | Produce an input frame; race and MP consume it |
| TANGLE | (header) | | `stdConfFile_*`, `stdConfig_*`, `stdControl_isAxisAboveDeadzone` live in `swrControl.h` under other prefixes; `swrControl_Startup` is not an input function | Name map fixes it on the refined side; faithful renames go to tim as a naming PR |
| DUP | 0x407500/0x4078e0/0x4079f0/0x406a50/0x405ea0 | | Device → table/limits switch repeated five times | One `device_bindings(device)` accessor |
| DUP | 0x407ea0/0x407f80 | PollAccept/Cancel | Near-identical bodies | One poll over a binding set |
| DUP | 0x40a330/0x40a500 | Configure/UpdateForceEffect | Same fetch-copy-modify-upload sequence | One helper with a param mutator |
| DUP | 0x404dd0 | ProcessInputs | Three similar per-device binding walks | One walk over a device descriptor |
| DEAD | 0x404dd0 | ProcessInputs | `swr_noop2()` call | Drop |
| DEAD | 0x408120 | SnapshotKeyboard | Writes `stdControl_KeyPressed_unused[]` | Drop after confirming no reader |
| DEAD | 0x405dd0 | ScanPressedButtons | Keyboard bitmask mode always returns `SWR_CONTROL_SCAN_NONE` | Drop the mode |
| DEAD | 0x409d70… | FF | The Immersion IFORCE path does nothing on modern hardware; real rumble comes from the XInput/Sony bridges | Keep as one backend behind an FF backend interface |
| SILENT | 0x4078a0 | ReplaceMapping | Swallows `AddMapping` failure (audit #12) | Return and surface the error |
| SILENT | 0x406470 | loader | Return codes -1/0/1 overloaded (missing / bad line / ok); `Initialize` only applies defaults on -1 | Distinct results; log bad lines |

## 4. Target shape

```
refined/swrControl/
  README.md               this dossier
  input_state.h/.c        swrControlState: per-device {bindings, count, flip, sensitivity,
                          deadzone, axis scale, caps}, edges, frame outputs; retail mirror seam
  input_bindings.h/.c     table ops: add/remove/replace/find/format; action + key-name lookup
  input_profile.h/.c      .map load/save (byte-compatible), default tables, device families
  input_frame.h/.c        per-frame processing -> swrInputFrame (sacred core)
  input_menu.h/.c         accept/cancel/menu edges, rebind capture helpers
  force_feedback.h/.c     effect model + backend interface (cifr, XInput, Sony)
  data/                   actions.json, default bindings per device family, ff_tuning.json
```

**Public interface (contracts):**
- `swrInput_Init(state)` / `swrInput_Shutdown(state)`: device setup and profile load, the same
  order as `swrControl_Initialize`.
- `swrInput_ProcessFrame(state, const swrInputUiMode* ui, swrInputFrame* out)`: bit-exact
  equivalent of `ProcessInputs`. The `ui` argument replaces the hangar/UI reads; the retail seam
  copies `out` into `swrRace_*Input`, the action buttons and `inRaceLocalPlayerInputRecord`.
- `swrInput_Bindings(state, device)`: table accessor; add/remove/replace/find on it.
- `swrInput_LoadProfile(state, name, filter)` / `swrInput_SaveProfile(...)`: `.map` round-trip,
  explicit result enum.
- `swrInput_FormatBinding(...)`: display text with fixed formats.
- `swrInput_PollMenu(state, excludeDevice)`: accept/cancel/menu edges.
- `swrFF_Update(state, const swrForceFeedbackInput* in)`, `swrFF_Play/Stop/StopAll`: effect
  model; backend chosen at init.

**Data to externalize (defaults = retail values, compiled in and shipped as files; a missing or
bad file falls back with a logged warning):**
- `actions.json`: the 22 `keyMapping2_0` entries (id, name/text key, kind flags) and the input
  record slot mapping. New actions are added here.
- `default_bindings/{xbox,generic,sony,wheel,mouse,keyboard}.json`: from
  `swrControl_defaultBindings*`, the shipped `control.map` / `wheel.map`, and the fixed
  arrow/numpad axes and accept/cancel keys.
- `ff_tuning.json`: the FF driver constants and bundle file paths.
- User profiles stay `.map`, byte-compatible (open decision).

**Name map (refined ↔ Ghidra ↔ address):** filled in as each step lands. Seed:
`swrInput_ProcessFrame` ↔ `swrControl_ProcessInputs` ↔ 0x404dd0;
`swrInput_LoadProfile` ↔ `stdConfFile_readAndApplyConf` ↔ 0x406470;
`swrInput_AxisPastThreshold` ↔ `stdControl_isAxisAboveDeadzone` ↔ 0x408040;
`swrInput_PollMenu` ↔ `swrControl_PollAccept` + `swrControl_PollCancel` ↔ 0x407ea0, 0x407f80.

## 5. Band-aids retired

Once refined, each of these becomes plain code, data or a toggle instead of a hook or patch:

| Band-aid | Where | Becomes |
|---|---|---|
| `swrControl_ProcessInputs_delta` (accept/cancel debounce, advance guard, foreign-mouse mask, gamepad-nav injection) | `dinput_hook/game_deltas/swrControl_delta.cpp`, `hook_replace` in `renderer_hook.cpp` | Edge tracking in `input_menu` (Fix toggle); nav injection as an input source |
| XInput rumble bridge | `swrControl_delta.cpp` (`ENABLE_XINPUT_RUMBLE`) | An FF backend |
| Sony pad rumble | `game_deltas/sony_pad_rumble.cpp` | An FF backend |
| Sony DIJOYSTATE rewrite via the `GetDeviceState` vtable patch | `stdControl_delta.c` | A `sony` default-binding family plus a stdControl seam (partial: the layout remap stays at the stdControl level) |
| Loader reimpl that skips bad lines (Fix C) | PR #270, `stdControl_delta.c` | `input_profile` + Fix toggle |
| `ClearBindings` terminator hook (Fix D) | PR #270 | Tables always terminated |
| `FindKeyName` return-address-scoped "" (Fix A) | PR #270 / `renderer_hook.cpp` | Non-NULL name lookup for comparers |
| Conflict-dialog scoped hook (Fix E) | PR #270 / `renderer_hook.cpp` | Conflict resolved outside the capture loop |
| `capture_binding_delta` (axis-as-button, delete-to-clear, drain loop) and the 14-byte patch at 0x405bd9 | PR #270 | Native in `input_menu` / `input_frame` (flip-sign rule is a Fix toggle) |
| `gamepad_button_names` ControlToString override scoped by return address | `feature/controls-unified-table` | `FormatBinding` takes a device family and asks for friendly names |
| Unified controls table `Build*Menu` detours | PR #270 | Partial: the menus are swrConfig/swrUI, but they get a binding API with no return-address scoping |
| Planned operand patch to grow `keyMapping2_0` (rebindable HUD cycle / camera views) | not built | A row in `actions.json` |
| Zero-ms input read clamp (#358, merged) | `stdControl_delta.c` | Stays in stdControl (neighbour); NaN handling noted in `ClampUnit` |

## 6. Oracle plan

| Step | Verification | Status |
|---|---|---|
| 0. Faithful gate | #285 `both` mode with globals snapshot/restore for: `ParseFunctionName`, `stdConfig_getKeymap_id`, `FindKeyName`, `FindMapping`, `FormatBinding`, `Add/Remove/ReplaceMapping`, `SetDefaultMappings`, `ClearBindings`, `isAxisAboveDeadzone`, `IsKeyStringControl`, `FindForceEffectSlot`, `CheckForceEffectType`, `FindMovedAxis`. Loader: run retail and ours on a corpus of `.map` files (shipped default, wheel, real user profiles, the junk-file case, #334's parser fuzzers) and compare tables, flip/sensitivity/deadzone and return code byte for byte. Then live flip `swrControl_*` to `reimpl` with everything else `original` under test-runner plans (races + menu tour). | **Gap:** no swrControl cases in #285 yet; the harness covers math only |
| `ProcessInputs` (sacred) | Needs **input injection**: record raw stdControl state per frame (keys, axes, buttons, dt) and replay it into retail and ours, comparing `swrRace_*Input`, action buttons, the input record and all edges bit for bit. Shares machinery with REPLAY_ROADMAP §10 input takeover | **Gap:** injection harness not built |
| Force feedback | Mock the cifr layer (record calls and arguments) and compare call sequences for recorded race telemetry | **Gap** |
| `MappingsMenu`, `CaptureBinding` | Test-runner menu tour + a scripted rebind plan; manual playtest with a pad | **Gap:** no rebind plan; faithful bodies missing |
| Steps 1–6 (refined) | Same harness cases with `refined` vs `original`; sacred functions also under input injection | Depends on #285 `refined` mode |
| Step 7 (Fix tier) | Toggle off: steps 1–6 checks still pass. Toggle on: targeted tests (bad-line file, junk-file case, held-Enter screen skip, conflict rebind) | |

## 7. Step plan

| # | Step | Size | Notes |
|---|---|---|---|
| 0 | **Faithful gate:** faithful `CaptureBinding` (none) and `MappingsMenu` (stub), or leave both `original` and gate without them; add the §6 harness cases; build the loader corpus test; live-flip and fix faithful bugs found | M | First work session. Ends with a verified faithful subsystem, no refined code |
| 0b | Input-injection harness for `ProcessInputs` | M | Can be its own session; also unblocks replay/TAS work |
| 1 | `DECOMP`: typed state, real arrays, flag enum, device enum, typed input record | S | Sacred functions: types and names only |
| 2 | `DEAD` / `DUP`: one device accessor, one poll, one FF param helper, one binding walk | S | |
| 3 | `GLOBAL` → `swrControlState` with retail mirror seam | M | Mirror `swrRace_*Input`, input record, edges until their readers are refined |
| 4 | `HARDCODE` → data: actions, default bindings per device family, FF tuning | M | Retail values as defaults; profile format decision needed first |
| 5 | `TANGLE` → interface: UI mode in, input frame out, `ForceFeedbackInput` from race | M | |
| 6 | `UNSAFE` / `LIMIT`: bounds, sized strings, capacities from caps | M | Caps beyond 16 buttons / 6 axes need a stdControl seam decision |
| 7 | Fix tier behind vanilla toggles: loader skip, terminator, conflict modal, edge debounce, NULL name, format strings, sensitivity 0, flip-sign rule | M | Most of these already exist as deltas (§5) |
| 8 | Retire band-aids: port §5 deltas onto the interface and data; delete patches and scoped hooks | L | Coordinate with PR #270 |

**Recommended first session:** step 0 (and 0b if time allows). Done = faithful swrControl
verified in `reimpl` mode against retail, gaps recorded here, no refined code, nothing pushed.
