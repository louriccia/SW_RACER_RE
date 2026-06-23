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

## Thread 1 - Hot-plug a new input (device absent at startup)  [PRIORITY]

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
- Depends on PR #152 (both touch stdControl_delta.c); land after #152 merges, then
  /pre-pr-check + PR. DESIGN: auto-enable on hot-plug = YES (user-confirmed).

## Thread 2 - Per-player input assignment (local coop)  [FLAGSHIP]

- **Symptom:** no way to bind a specific device to player 1 vs player 2.
- **Root cause:** input is read into GLOBAL state (`stdControl_aAxisPos` / `aKeyInfos` /
  `aKeyIdleTimes`); no device -> player ownership anywhere.
- **Approach:** add a per-slot input-source mapping; route each device's reads into a
  per-player input record feeding the per-player input bitsets. Pairs with reviving local
  splitscreen ([[local_multiplayer_subsystem]]: splitscreen is intact in the binary, gated
  only by the roster builder `FUN_0045b610` emitting a single 'Locl').
- **Lift:** large; depends on splitscreen plumbing.

## Thread 3 - Disconnect UX  [OPTIONAL POLISH]

- The underlying re-acquire already works, so this is feedback only: a "controller
  disconnected" prompt / auto-pause when a player's device goes lost.
- **Lift:** small.

## Sequencing

1 (hot-plug new) -> 2 (per-player + splitscreen) -> 3 (polish). The pending XInput bridge
(#114/#115) is orthogonal (menu nav + rumble); these threads target the DirectInput path.

## Open questions

- Re-enum trigger: `WM_DEVICECHANGE` via a message window vs a GLFW joystick callback vs a
  periodic poll in the read loop.
- Once #114/#115 merge, avoid double-counting a pad as both a DInput joystick and an XInput
  controller.
