# Input QoL Roadmap

Modernize SWE1R's input device handling. Local planning doc (kept out of git via
`.git/info/exclude`).

## Current state (RE-grounded, master @ be00d79)

- **DirectInput is the only active input path.** `stdControl` (DirectInput) drives
  keyboard/mouse/joystick, including gamepads. The GLFW input replacement is compiled out
  (`ENABLE_GLFW_INPUT_HANDLING=0`, commit `fbaa4dd` "disable glfw dinput replacement ... to
  make gamepads work correctly").
- **The XInput bridge is NOT in master.** PRs #114 (rumble) and #115 (menu nav: D-pad /
  START / BACK) are still OPEN, and they cover menu navigation + rumble output, not in-race
  pod control. So in-race control is DirectInput regardless of those PRs.
- **Device discovery is ONE-SHOT at startup.** `stdControl_Startup` @ 0x485360 creates the
  IDirectInput, enumerates devices into `DirectInput_EnumDevice_Callback` @ 0x486a10 (fills
  `DirectInputKeyboards[4]` / `DirectInputMouses[4]` / `DirectInputJoysticks[8]`), then
  `stdControl_InitKeyboard` @ 0x485f20 / `InitJoysticks` @ 0x485c40 / `InitMouse` @ 0x486010
  `CreateDevice` + `SetDataFormat` + `SetCooperativeLevel` + `Acquire` each one. Runs once.
- **The read path re-acquires every frame.** `stdControl_ReadJoysticks` @ 0x486340 (and
  `ReadKeyboard` @ 0x486170 / `ReadMouse` @ 0x486710) call `Acquire()` before each
  `GetDeviceState`. So unplug -> replug of a *startup-known* device resumes automatically
  (verified; matches observed behavior). A device NOT present at startup has no object and
  is invisible.
- **PR #152** (shipped) made the startup enumeration per device class
  (KEYBOARD/MOUSE/JOYSTICK) so a non-game HID device can't crash DirectInput startup.

## Thread 1 - Hot-plug a new input (device absent at startup)  [SHIPPED - PR #183 input-hotplug, merged]

- **Symptom:** plug in a controller/stick after launch and it is never picked up (no
  control, no UI). This is the real "doesn't handle a new input" gap.
- **Root cause:** one-shot enumeration -> no device object created for late arrivals.
- **Approach:** on device arrival, re-run the per-class enumeration and `CreateDevice` +
  init only the NEW device(s) - dedupe by `guidInstance` against existing
  `DirectInputJoysticks` entries, respect the 8-joystick array cap.
  - Trigger options: `WM_DEVICECHANGE` / `DBT_DEVICEARRIVAL` (the live window is GLFW's, see
    [[window_glfw_shutdown]], so either a GLFW joystick callback or a debounced periodic /
    lazy re-enum from `stdControl_ReadControls`).
  - Cleanest: a `stdControl` delta that, on a debounced device-change signal, re-runs the
    per-class enum + an InitJoysticks-for-new-device step.
- **Lift:** medium. Locus: `stdControl` + `swrControl` (same neighborhood as PR #152).

### Prototype findings (feature/input-hotplug, 2026-06-21)

Detection + DInput re-enumeration both WORK, proven via logging:
- A GLFW joystick callback (`glfwSetJoystickCallback`, fires during the per-frame
  `glfwPollEvents` in Window_delta.c) catches the hot-plug:
  `[hotplug] glfw joystick jid=0 event=0x40001` (GLFW_CONNECTED).
- The rescan (reset count, re-`EnumDevices(DIDEVTYPE_JOYSTICK)`, `InitJoysticks`) creates the
  device: `[hotplug] rescan: enum hr=0 joysticks before=0 after=1`.

BUT the game still produces no input from the late device. ROOT CAUSE = `swrControl`:
`swrControl_Initialize` @ 0x404b10 (which calls `stdControl_Startup`) latches the joystick
OFF when none is present at startup -- it enables the 6 joystick axes via
`stdControl_EnableAxis(i + stdControl_joystickDeviceIndex*6)`, and if `swrConfig_joystickNbAxis
== 0` sets `joystick_detected = 0` + `swrConfig_joystick_enabled = 0`. A late device never
flips those flags, and `swrControl_Initialize` can't simply be re-run (it bails early because
`stdControl_Startup` now returns "already started").

FIX (VALIDATED end-to-end 2026-06-21): after a rescan, run just the joystick detect/enable
block -- `swrControl_SelectSavedJoystick` (0x407de0) -> `stdControl_EnableAxis` (0x4855f0) x6
-> set `joystick_detected` / `swrConfig_joystick_enabled` (force-on per the hot-plug UX) /
`swrConfig_joystickNbAxis` -> `swrControl_ApplyAxisConfig` (0x407630) x2. Confirmed in-game: a
controller absent at startup, plugged in mid-session, now steers (log: nbAxis=5 enabled=1).
All via NAMED globals/_ADDRs (cdecl).

PRODUCTIONIZATION TODO before PR:
- Remove the [hotplug] diagnostic fprintf logging.
- Buttons: `swrConfig_joystickNbButtons` is left unset by the refresh -- VERIFY pad buttons
  (boost/brake) register; if not, set it from the device caps (and the per-axis mask @0xec887c,
  read by SetDefaultMappings/FormatBinding/the config menu -- name it).
- Rescan is a full rebuild (re-creates all pads, leaks the old device objects per event, brief
  input blip on the active pad). Fine for a rare hot-plug, but consider incremental (dedupe by
  guidInstance, init only the new device) for production.
- Depended on PR #152 (both touch stdControl_delta.c); #152 merged and this work SHIPPED + MERGED
  as PR #183 (input-hotplug). DESIGN: auto-enable on hot-plug = YES (user-confirmed).

## Thread 1.5 - Input diagnostics + device picker  [SHIPPED locally, this session]

- **What:** an "Input Diagnostics" panel (F5 -> Settings) that walks the input pipeline
  top to bottom (device -> raw axes -> bindings -> the values the pod receives) so a player
  can see where their controller stops being recognized (the #1 community issue). Includes a
  multi-controller picker: every connected joystick listed by product name, the active one
  marked, a dropdown to switch it, and a ">1 controller connected" warning.
- **RE substrate (built this session):**
  - Device names captured by chaining the JOYSTICK `EnumDevices` callback
    (`stdControl_EnumJoystickName_cb` in `stdControl_delta.c`): record `tszProductName` at
    the current `stdControl_numJoystickDevices` slot, then call the game's original callback.
  - Switching the active device = `stdControl_SelectJoystickByIndex(i)`: set
    `stdControl_joystickDeviceIndex`, enable that device's 6 axes (`stdControl_EnableAxis(i*6+n)`),
    `swrControl_ApplyAxisConfig(0/1)`. Faithful + safe because `ReadJoysticks` already polls
    EVERY device into its own `aAxisPos[dev*6..]` slots, and binding/deadzone resolution all
    key off `stdControl_joystickDeviceIndex` (confirmed in `ApplyAxisConfig` ->
    `FormatBinding` + `SetAxisDeadzone`). Same enable block the hot-plug refresh uses.
- **Known gap (follow-up):** the device switch is runtime-only; startup still selects the
  saved registry `JoystickGUID`. Persisting the pick = write that registry value (read by
  `swrControl_SelectSavedJoystick` on boot).
- This panel + picker is the device-identity groundwork Threads 2 and 4 build on.

## Thread 2 - Per-player input assignment (local coop)  [FLAGSHIP]

- **Symptom:** no way to bind a specific device to player 1 vs player 2.
- **Root cause:** input is read into GLOBAL state (`stdControl_aAxisPos` / `aKeyInfos` /
  `aKeyIdleTimes`); no device -> player ownership anywhere.
- **Approach:** add a per-slot input-source mapping; route each device's reads into a
  per-player input record feeding the per-player input bitsets. Pairs with reviving local
  splitscreen ([[local_multiplayer_subsystem]]: splitscreen is intact in the binary, gated
  only by the roster builder `FUN_0045b610` emitting a single 'Locl'). The
  `swrControl_ProcessInputs` (`0x004058e0`) device-acquisition step is the chokepoint -- it
  fills raw slot 0 only; the translation/bitset/per-pod-control layers are already 4-wide.
- **Lift:** large; depends on splitscreen plumbing. The Thread 1.5 device picker gives the
  device list + per-index selection this needs.

## Thread 3 - Disconnect UX  [OPTIONAL POLISH]

- The underlying re-acquire already works, so this is feedback only: a "controller
  disconnected" prompt / auto-pause when a player's device goes lost.
- **Lift:** small.

## Thread 4 - Per-player binding profiles (Smash-style name tags)  [FUTURE]

- **Idea (user request):** at splitscreen setup each player picks a saved control *profile*
  (their own bindings + sensitivity/deadzone/invert), assigned to their slot -- like Super
  Smash Bros. name tags, where the config travels with the player, not the port. Lets each
  local player use the layout they want, and makes configs shareable/moddable (drop a file in,
  pick it in the menu).
- **The game already has 90% of this.** Control configs are stored as named **config-set
  folders** under `.\data\config\<name>\` and read/written by the game's own code:
  - **Load:** `stdConfFile_readAndApplyConf(deviceFilter, configName, useDefaultDir)`
    (`0x00406470`) parses a controls file into the binding tables. `swrControl_Initialize`
    loads the active set `"current"` this way (and falls back to `swrControl_SetDefaultMappings`).
  - **Save:** `swrConfig_WriteMappings(name)` (`0x00406080`) serializes the current bindings to
    `.\data\config\<name>\`. The format has `JOYSTICK` / `MOUSE` / `KEYBOARD` sections, each with
    `MAPPINGS` (AXIS / BUTTON / KEY -> FUNCTION), `FLIP_AXIS`, `SENSITIVITY`, `DEADZONE`,
    `ENABLED`. So **one file fully describes one player's control setup = one profile.**
  - **Edit:** `swrControl_MappingsMenu` (`0x00402250`) is the existing rebinding UI; pair with
    `WriteMappings` to author/save a profile.
- **What's missing (all of it gated on Thread 2):**
  1. **Per-player binding storage.** Today `readAndApplyConf` writes the GLOBAL binding tables
     (`keyMapping0/1/2`), so loading P2's profile clobbers P1's. Profiles can only coexist once
     bindings are per-player (the Thread 2 lift) -- then load each player's chosen profile into
     their own table.
  2. **Profile-select UI.** At splitscreen setup, scan `.\data\config\` for available profile
     folders and give each player slot a dropdown (reuse the Thread 1.5 picker pattern; pairs
     device + profile per slot).
  3. (Optional) **Profile create/rename** flow wrapping `MappingsMenu` + `WriteMappings`.
- **Lift:** medium *on top of Thread 2* (serialization, folder scan, loader, and rebinding UI
  all already exist; the real cost is per-player binding tables, which Thread 2 delivers).
- **Sequencing:** Thread 2 (per-player input) -> Thread 4 (profiles per player). Thread 1.5's
  device picker is the UI substrate for both.

## Sequencing

1 (hot-plug new, SHIPPED) -> 1.5 (diagnostics + device picker, SHIPPED locally) -> 2
(per-player + splitscreen) -> 4 (per-player binding profiles) -> 3 (polish). The pending
XInput bridge (#114/#115) is orthogonal (menu nav + rumble); these threads target the
DirectInput path.

## Open questions

- Re-enum trigger: `WM_DEVICECHANGE` via a message window vs a GLFW joystick callback vs a
  periodic poll in the read loop.
- Once #114/#115 merge, avoid double-counting a pad as both a DInput joystick and an XInput
  controller.
