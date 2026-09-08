// Player camera settings: tunes the game's own pod camera in place via detours on the camera-man
// (swrObjcMan_UpdateCamera and helpers), so the viewport, sky sprites and audio listener all see the
// adjusted camera. Persisted to [player_camera] in SW_RACER_RE.ini; ImGui panel Camera > Player Camera.
#pragma once

void playercam_RegisterHooks();// from init_renderer_hooks(), before init_hooks()
void playercam_RegisterPanel();

// In a race only; OR'd into the freecam hide-HUD path, which owns the sprite / text filtering.
bool playercam_HudHidden();

// True while a swrSprite_SetVisible from a hidden world-sprite group (suns / light streaks) is
// running; the freecam's SetVisible detour forces those invisible.
bool playercam_SuppressSpriteVisibility();

// Renderer queries for the local player's pod root node.
bool playercam_HideOwnPod();
bool playercam_ShowPodInFirstPerson();// draw it even where its own camera hides it (POD_HIDDEN)

// Multiplier on the GL near plane (< 1 while the true cockpit is active, so the cockpit isn't clipped).
float playercam_NearClipScale();

// External camera driver (e.g. the broadcast director's drone shot). Called right after the game's
// swrObjcMan_UpdateCamera for every camera-man; return true after rewriting unk20_mat (camera-to-
// world) and focusTransform_mat.vD (aim point) to skip the player-camera post-processing. NULL clears.
struct swrObjcMan;
typedef bool (*PlayerCamOverrideFn)(swrObjcMan *cman);
void playercam_SetCameraOverride(PlayerCamOverrideFn fn);
// Per-frame FOV hook, applied where the camera-man pushes its staged FOV to the viewport
// (swrObjcMan_UpdateFogAndViewport): gets the staged FOV in degrees, returns the one to use.
typedef float (*PlayerCamFovFn)(swrObjcMan *cman, float fov);
void playercam_SetFovOverride(PlayerCamFovFn fn);

// True-cockpit camera for any pod (the director's cockpit shot): writes the cockpit transform + the
// per-pilot eye offset into the camera-man. While an external cockpit shot is active the near clip
// scale applies as for the player's true cockpit.
struct swrRace;
void playercam_ApplyTrueCockpit(swrObjcMan *cman, swrRace *racer);
// The same rig reversed: on the cockpit node, `forward` ahead of the pilot's eye point and
// `up` above it, looking back at the pilot (a true face cam).
void playercam_ApplyReverseCockpit(swrObjcMan *cman, swrRace *racer, float forward, float up);
// Rebuild a camera's look-at with its roll scaled (1 = unchanged, 0 = level horizon).
void playercam_ScaleRoll(swrObjcMan *cman, float influence);
// Perlin noise (the camera-shake source), for other camera modules.
float playercam_Noise(float x, float y, float z);
void playercam_SetExternalCockpit(bool active);
// One of the game's own views for a camera-man: 1 chase near, 2 chase far, 4 first person
// (bumper), 5 first person wide.
void playercam_SetStockMode(swrObjcMan *cman, int mode);
