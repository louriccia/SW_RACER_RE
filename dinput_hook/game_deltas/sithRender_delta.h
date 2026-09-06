#pragma once

// Screenshot capture (issue #289).
//
// The original F12 path is sithRender_MakeScreenShot -> stdDisplay_SaveScreen ->
// stdBmp_VBufferToBmp(&stdDisplay_g_backBuffer). That back buffer is the DirectDraw surface, which
// the OpenGL takeover never fills, so the converter walks an unmapped row pointer and the game dies
// with an access violation inside stdColor_ColorConvertOneRow. The delta keeps the original's
// filename numbering but reads the finished frame back out of GL instead.

#ifdef __cplusplus
extern "C" {
#endif

// Replaces sithRender_MakeScreenShot. Picks the next free "<prefix>NNN.bmp" and arms the capture;
// the pixels are read at the end of the frame, where the scene is actually complete.
void sithRender_MakeScreenShot_delta(char* prefix);

// Called once per frame from stdDisplay_Update_Hook, after the scene has been resolved into the
// default framebuffer and before the debug overlay is drawn. No-op unless a capture is armed.
void sithRender_CapturePendingScreenshot(void);

#ifdef __cplusplus
}
#endif
