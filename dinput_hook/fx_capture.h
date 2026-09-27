//
// FX Capture: stage the podracer explosion plume as a clean, repeatable asset shot.
//
// Three things a capture needs that the game does not give you: a way to fire the
// explosion on demand (instead of crashing into a wall and hoping the camera is
// pointed the right way), a background that is nothing but the FX, and slow
// motion so a 3-second burst is more than a handful of frames.
//
// Background: the matte suppresses every mesh draw outside the FX subtrees plus,
// optionally, the pod -- the plume itself is the swrObjSmok particle nodes
// (fireballChildNodesPtr pool) and the shared fireball node (fireballNodePtr), so
// "is this node FX" is an exact pointer test, not a heuristic. The scene
// framebuffer is then cleared to the matte colour, world sprites (suns / lens
// flares) are dropped with the HUD, and fog is forced off so nothing tints it.
//
// Pair it with the free camera (F9) to park the shot.
//
#pragma once

struct swrModel_Node;

// Registers the per-frame sim seam used for time scaling / freeze / frame step.
// Call from init_renderer_hooks(), before init_hooks() applies the detours.
void fxcapture_RegisterHooks();

// Registers the ImGui "FX Capture" panel and loads persisted settings. Call once
// at startup where the other debug panels are registered.
void fxcapture_RegisterPanel();

// True while the matte is dropping the world. The renderer clears to the matte
// colour and skips every mesh outside an FX (or kept-pod) subtree.
bool fxcapture_MatteHidesWorld();

// The matte clear colour, as RGBA in [0..1]. Only meaningful while the matte is on.
void fxcapture_MatteColor(float out_rgba[4]);

// True when the pod should survive the matte (its debris and engine glow read as
// part of the explosion).
bool fxcapture_KeepPod();

// True if this node is one of the explosion FX nodes -- a swrObjSmok particle
// slot or the shared fireball node. Everything below it draws through the matte.
bool fxcapture_IsFxNode(const swrModel_Node *node);

// Refreshes the FX node set for this frame. Call once per viewport render, before
// the scene-graph traversal.
void fxcapture_BeginFrame();

// True while the panel is hiding the HUD (consulted by camera.cpp's hud_hidden()).
bool fxcapture_HudHidden();
