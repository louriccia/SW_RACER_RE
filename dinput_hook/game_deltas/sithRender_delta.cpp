#include "sithRender_delta.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

extern "C" {
#include "globals.h"
}

extern "C" FILE *hook_log;

// Armed by the F12 handler, consumed at end of frame. Empty means nothing pending.
static std::string g_pending_screenshot;

// Time of the last accepted request, in glfwGetTime() seconds; negative means "none yet".
//
// swrMain_ProcessDebugKeys has no rising-edge check on F12, so one press can ask for a screenshot
// more than once and write several files. Two approaches do not work here. A frame count does not:
// measured on a 2560x1377 capture the repeat came two frames later, but two other presses in the
// same run never repeated at all, so how long the key happens to be held decides it. Reading the
// physical key state does not either: the handler fires a frame or more after the key-down edge, so
// by the time the request arrives the key already reads as held and every press gets swallowed.
//
// What does separate them is the spacing. Across the runs, repeats landed 47-70 ms behind their
// press, while deliberate presses were never closer than 1.8 s. A debounce anywhere in that gap
// collapses a press to one file, and a quarter second is far below what a person can intend as two
// screenshots.
static double g_last_request_time = -1.0;

// Requests closer together than this are the same press. See the note above for the measurements.
static constexpr double SCREENSHOT_DEBOUNCE_SECONDS = 0.25;

// 24-bit bottom-up BMP. glReadPixels already hands back rows bottom-up in GL_BGR order, which is
// exactly the on-disk layout, so the rows go straight out with only the 4-byte stride padding
// applied.
static bool write_bmp_24(const char *filename, int width, int height,
                         const std::vector<uint8_t> &bgr) {
    const int row_bytes = width * 3;
    const int padded_row = (row_bytes + 3) & ~3;
    const uint32_t pixel_bytes = (uint32_t) padded_row * (uint32_t) height;
    const uint32_t pixel_offset = 14 + 40;

    FILE *f = fopen(filename, "wb");
    if (!f)
        return false;

    uint8_t header[14 + 40] = {0};
    // BITMAPFILEHEADER
    header[0] = 'B';
    header[1] = 'M';
    const uint32_t file_size = pixel_offset + pixel_bytes;
    std::memcpy(&header[2], &file_size, 4);
    std::memcpy(&header[10], &pixel_offset, 4);
    // BITMAPINFOHEADER
    const uint32_t info_size = 40;
    const int32_t w32 = width;
    const int32_t h32 = height;
    const uint16_t planes = 1;
    const uint16_t bpp = 24;
    std::memcpy(&header[14], &info_size, 4);
    std::memcpy(&header[18], &w32, 4);
    std::memcpy(&header[22], &h32, 4);
    std::memcpy(&header[26], &planes, 2);
    std::memcpy(&header[28], &bpp, 2);
    std::memcpy(&header[34], &pixel_bytes, 4);

    bool ok = fwrite(header, 1, sizeof(header), f) == sizeof(header);

    static const uint8_t padding[3] = {0, 0, 0};
    const int pad = padded_row - row_bytes;
    for (int y = 0; ok && y < height; y++) {
        ok = fwrite(&bgr[(size_t) y * row_bytes], 1, row_bytes, f) == (size_t) row_bytes;
        if (ok && pad)
            ok = fwrite(padding, 1, pad, f) == (size_t) pad;
    }

    fclose(f);
    return ok;
}

void sithRender_MakeScreenShot_delta(char *prefix) {
    // Swallow the repeat before picking a name, so it does not burn a file index and leave a gap in
    // the numbering.
    const double now = glfwGetTime();
    if (g_last_request_time >= 0.0 && (now - g_last_request_time) < SCREENSHOT_DEBOUNCE_SECONDS) {
        g_last_request_time = now;
        return;
    }
    g_last_request_time = now;

    // Same numbering the original uses: walk the index until a name is free, and leave
    // stdDisplay_ScreenshotIndex past it so the next shot starts from there.
    char filename[80];
    FILE *probe = NULL;
    do {
        const int index = stdDisplay_ScreenshotIndex;
        stdDisplay_ScreenshotIndex = index + 1;
        snprintf(filename, sizeof(filename), "%s%03d.bmp", prefix ? prefix : "snap_", index);
        probe = fopen(filename, "rb");
        if (probe == NULL)
            break;
        fclose(probe);
    } while (true);

    // Arm only; the frame is not finished at input time. A second request in the same frame keeps
    // the first name rather than dropping a numbered file that never gets written.
    if (g_pending_screenshot.empty())
        g_pending_screenshot = filename;
}

void sithRender_CapturePendingScreenshot(void) {
    if (g_pending_screenshot.empty())
        return;

    const std::string filename = g_pending_screenshot;
    g_pending_screenshot.clear();

    int width = 0;
    int height = 0;
    GLFWwindow *window = glfwGetCurrentContext();
    if (window)
        glfwGetFramebufferSize(window, &width, &height);
    if (width <= 0 || height <= 0)
        return;

    std::vector<uint8_t> pixels((size_t) width * height * 3);

    // The scene has already been blitted into framebuffer 0 by swrViewport_Render_Hook. Read from
    // the front-most complete image, and force a tight row stride so the buffer matches the width.
    GLint prev_read_fb = 0;
    GLint prev_pack_align = 4;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fb);
    glGetIntegerv(GL_PACK_ALIGNMENT, &prev_pack_align);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_BGR, GL_UNSIGNED_BYTE, pixels.data());
    glPixelStorei(GL_PACK_ALIGNMENT, prev_pack_align);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read_fb);

    if (!write_bmp_24(filename.c_str(), width, height, pixels)) {
        fprintf(hook_log, "[screenshot] failed to write %s\n", filename.c_str());
        fflush(hook_log);
        return;
    }
    fprintf(hook_log, "[screenshot] wrote %s (%dx%d)\n", filename.c_str(), width, height);
    fflush(hook_log);
}
