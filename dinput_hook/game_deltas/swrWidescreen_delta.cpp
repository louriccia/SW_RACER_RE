#include "../imgui_utils.h"
#include "../hook_helper.h"

extern "C" {
#include "swrWidescreen_delta.h"
#include <Swr/swrSprite.h>
#include <Swr/swrUI.h>
#include <globals.h>
}

// Horizontal centering offset for the un-stretched 4:3 UI on a wide framebuffer.
// uniformScale mirrors phase 1's vertical scale exactly (screenHeight * recipH,
// recipH ~ 1/480 as a DOUBLE at 0x004acce0). 640 is the design width.
static float widescreen_ui_offset_x() {
    if (swrDisplay_screenHeight == 0)
        return 0.0f;
    const double recipH = *(double *) 0x004acce0;
    const float uniformScale = (float) ((double) swrDisplay_screenHeight * recipH);
    return ((float) swrDisplay_screenWidth - 640.0f * uniformScale) * 0.5f;
}

// 0x004325b0 rdProcEntry_Add2DQuad3 -- the clean sprite emitter (x0/x1 are plain
// pixel coords). Shift both X coords right by the centering offset.
extern "C" void rdProcEntry_Add2DQuad3_delta(short x0, short y0, short x1, short y1, float tex_width,
                                             float tex_height, BOOL textured, BOOL add_z_offset) {
    if (imgui_state.widescreen_ui) {
        const short off = (short) widescreen_ui_offset_x();
        x0 += off;
        x1 += off;
    }
    hook_call_original(rdProcEntry_Add2DQuad3, x0, y0, x1, y1, tex_width, tex_height, textured,
                       add_z_offset);
}

// 0x0042d990 rdProcEntry_Add2DQuad2 -- the text-glyph emitter (first arg is the
// pen X). Shift the glyph origin by the centering offset.
extern "C" void rdProcEntry_Add2DQuad2_delta(short x, short y, short a, short b, short c, short d,
                                             short e, short f) {
    if (imgui_state.widescreen_ui)
        x += (short) widescreen_ui_offset_x();
    hook_call_original(rdProcEntry_Add2DQuad2, x, y, a, b, c, d, e, f);
}

// 0x004329c0 rdProcEntry_Add2DQuad -- plain textured quad (x0/x1 plain pixels).
extern "C" void rdProcEntry_Add2DQuad_delta(short x0, short y0, short x1, short y1, float tex_u0,
                                            float tex_v0, float tex_u1, float tex_v1) {
    if (imgui_state.widescreen_ui) {
        const short off = (short) widescreen_ui_offset_x();
        x0 += off;
        x1 += off;
    }
    hook_call_original(rdProcEntry_Add2DQuad, x0, y0, x1, y1, tex_u0, tex_v0, tex_u1, tex_v1);
}

// 0x004083d0 swrUI_UpdateMouseState -- after the game refreshes the cursor, map it
// from raw screen pixels back into UI design space so swrUI_HitTest (which compares
// against design-coord element rects) matches the centered render. The cursor that
// swrUI_ProcessMouse reads lives at 0x00ec874c (x) / 0x00ec8754 (y).
extern "C" void swrUI_UpdateMouseState_delta() {
    hook_call_original(swrUI_UpdateMouseState);
    if (!imgui_state.widescreen_ui || swrDisplay_screenHeight == 0)
        return;
    const double recipH = *(double *) 0x004acce0;
    const float uniformScale = (float) ((double) swrDisplay_screenHeight * recipH);
    if (uniformScale == 0.0f)
        return;
    const float offsetX = ((float) swrDisplay_screenWidth - 640.0f * uniformScale) * 0.5f;
    int *cursor_x = (int *) 0x00ec874c;
    int *cursor_y = (int *) 0x00ec8754;
    *cursor_x = (int) (((float) *cursor_x - offsetX) / uniformScale);
    *cursor_y = (int) ((float) *cursor_y / uniformScale);
}
