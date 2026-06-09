#ifndef SWRWIDESCREEN_DELTA_H
#define SWRWIDESCREEN_DELTA_H

#include <windows.h> // BOOL

// Widescreen UI -- phase 2 ("Mode B": 16:9 gameplay + centered 4:3 UI).
//
// Phase 1 (swrSprite_delta) un-stretched the 2D UI (uniform scale = vertical
// scale); it left the UI LEFT-ANCHORED. Phase 2 pillarboxes it: add a constant
// horizontal offset so the un-stretched 4:3 UI is centered, and apply the inverse
// transform to the cursor so the (CPU-side, design-space) hit-test still lines up.
//
//   uniformScale = screenHeight / 480              (same scale phase 1 renders with)
//   offsetX      = (screenWidth - 640*uniformScale) / 2
//   screenX      = designX*uniformScale + offsetX  (render)
//   designX      = (rawCursorX - offsetX) / uniformScale   (hit-test, inverse)
//
// All gated by imgui_state.widescreen_ui (the same toggle as phase 1) so it can
// be A/B compared and disabled.
//
// =============================== STATUS ===============================
// *** SCAFFOLD -- NOT YET VERIFIED IN-GAME. *** Centering is a visual change that
// must be checked on a real widescreen display. Open items before this is PR-ready:
//   - confirm the offset SIGN / magnitude and that text + sprites line up;
//   - the rotated/scaled sprite path rdProcEntry_Add2DQuad5 (0x44efa0, fixed-point
//     <<2 coords) and rdProcEntry_Add2DPolygon (0x4321b0, mixed short/float coords)
//     are NOT hooked here yet -- they need their coord layout confirmed live;
//   - verify no other 2D overlay (HUD gauges, lens flare, weather) uses a path
//     these hooks miss.
// ======================================================================

// Sprite/text emit funnels: add offsetX to the final screen-X.
void rdProcEntry_Add2DQuad2_delta(short x, short y, short a, short b, short c, short d, short e, short f);
void rdProcEntry_Add2DQuad3_delta(short x0, short y0, short x1, short y1, float tex_width, float tex_height, BOOL textured, BOOL add_z_offset);
void rdProcEntry_Add2DQuad_delta(short x0, short y0, short x1, short y1, float tex_u0, float tex_v0, float tex_u1, float tex_v1);

// Cursor: map raw screen pixels back to UI design space so hit-test matches the
// centered render (read by swrUI_ProcessMouse -> swrUI_HitTest).
void swrUI_UpdateMouseState_delta(void);

#endif // SWRWIDESCREEN_DELTA_H
