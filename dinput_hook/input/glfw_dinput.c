// DirectInput 5 as stdControl uses it, served from GLFW: one system keyboard and mouse fed by the
// window's GLFW callbacks, and one joystick per GLFW joystick. The faithful stdControl, the Sony
// layout rewrite and joystick hot-plug run unchanged on top of it.
//
// What each device reports, matching what the system DirectInput reported for the same hardware:
// - keyboard: DIK scan codes, buffered press/release events stamped in timeGetTime ms.
// - mouse: relative X/Y in pixels, wheel Z in WHEEL_DELTA units, buttons 0-3.
// - XInput pad: the Xbox 360 DirectInput view (X/Y left stick, Z both triggers, Rx/Ry right
//   stick, 10 buttons, D-pad as POV 0), product id 045E:028E.
// - other pads: GLFW's DirectInput axes in order X, Y, Z, Rx, Ry, Rz, sliders; its buttons; its
//   hats as POVs; product id from GLFW's GUID. GLFW lists only the axes a device has, so a device
//   missing one of them reports the following axes one slot early.
// No device reports force feedback, so cifr leaves IFORCE2 alone.

#define DIRECTINPUT_VERSION 0x0700
#include "glfw_dinput.h"

#include <dinput.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <GLFW/glfw3.h>

extern FILE *hook_log;

#define GDI_MAX_EVENTS 256
#define GDI_MAX_JOYSTICKS (GLFW_JOYSTICK_LAST + 1)
#define GDI_AXIS_CENTER 32768
#define GDI_AXIS_MAX 65535
#define GDI_NUM_JOY_AXES 8// X, Y, Z, Rx, Ry, Rz, slider 0, slider 1
#define GDI_XINPUT_BUTTONS 10
#define GDI_XINPUT_AXES 5// X, Y, Z (triggers), Rx, Ry
#define GDI_MOUSE_BUTTONS 4
#define GDI_VID_MICROSOFT 0x045e
#define GDI_PID_XBOX360 0x028e

enum GdiKind { GDI_KEYBOARD, GDI_MOUSE, GDI_JOYSTICK };

typedef struct GdiEventQueue {
    DIDEVICEOBJECTDATA events[GDI_MAX_EVENTS];
    DWORD count;
    DWORD capacity;// DIPROP_BUFFERSIZE
    int overflowed;
} GdiEventQueue;

typedef struct GdiDevice {
    IDirectInputDevice2AVtbl *lpVtbl;
    LONG refs;
    enum GdiKind kind;
    int jid;
} GdiDevice;

typedef struct GdiRoot {
    IDirectInputAVtbl *lpVtbl;
    LONG refs;
} GdiRoot;

static GLFWwindow *g_window;
static GLFWkeyfun g_prevKey;
static GLFWmousebuttonfun g_prevButton;
static GLFWcursorposfun g_prevCursor;
static GLFWscrollfun g_prevScroll;
static DWORD g_sequence;
static int g_logKeys;// SWR_INPUT_LOG=1

static GdiEventQueue g_keyQueue;
static BYTE g_keyState[256];

static GdiEventQueue g_mouseQueue;
static BYTE g_mouseButtons[GDI_MOUSE_BUTTONS];
static double g_cursorX, g_cursorY;
static int g_cursorKnown;
static double g_mouseDX, g_mouseDY, g_mouseDZ;

// 'SWRJ' + joystick id: the instance GUID CreateDevice gets back from enumeration.
static const GUID GDI_GUID_KEYBOARD = {0x6f1d2b61, 0xd5a0, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID GDI_GUID_MOUSE = {0x6f1d2b60, 0xd5a0, 0x11cf, {0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
#define GDI_JOY_GUID_DATA1 0x4a525753u

static DWORD gdi_now(void) {
    return timeGetTime();
}

// The time the current window message was posted, in timeGetTime's clock.
static DWORD gdi_event_time(void) {
    const DWORD now = gdi_now();
    const DWORD stamp = (DWORD) GetMessageTime() + (now - GetTickCount());
    return (LONG) (stamp - now) > 0 ? now : stamp;
}

static void gdi_queue_push(GdiEventQueue *q, DWORD ofs, DWORD data) {
    const DWORD capacity = q->capacity ? q->capacity : GDI_MAX_EVENTS;
    if (q->count >= capacity || q->count >= GDI_MAX_EVENTS) {
        q->overflowed = 1;
        return;
    }
    DIDEVICEOBJECTDATA *e = &q->events[q->count++];
    e->dwOfs = ofs;
    e->dwData = data;
    e->dwTimeStamp = gdi_event_time();
    e->dwSequence = ++g_sequence;
}

static int gdi_window_focused(void) {
    return g_window && glfwGetWindowAttrib(g_window, GLFW_FOCUSED);
}

// GLFW's Win32 scancode is the set-1 make code with bit 8 for the E0 prefix; DIK puts that bit at 7.
static int gdi_dik_from_glfw(int key, int scancode) {
    if (key == GLFW_KEY_PAUSE)
        return DIK_PAUSE;
    if (key == GLFW_KEY_NUM_LOCK)
        return DIK_NUMLOCK;
    if (scancode <= 0 || scancode > 0x1ff)
        return 0;
    return (scancode & 0x7f) | ((scancode & 0x100) ? 0x80 : 0);
}

static void gdi_key_cb(GLFWwindow *w, int key, int scancode, int action, int mods) {
    if (action != GLFW_REPEAT) {
        const int dik = gdi_dik_from_glfw(key, scancode);
        const BYTE down = action == GLFW_PRESS ? 0x80 : 0;
        if (dik && g_keyState[dik] != down) {
            if (g_logKeys)
                fprintf(hook_log, "[input] key glfw %d scan 0x%x -> DIK 0x%02x %s\n", key, scancode, dik, down ? "down" : "up");
            g_keyState[dik] = down;
            gdi_queue_push(&g_keyQueue, (DWORD) dik, down);
        }
    }
    if (g_prevKey)
        g_prevKey(w, key, scancode, action, mods);
}

static void gdi_button_cb(GLFWwindow *w, int button, int action, int mods) {
    if (button >= 0 && button < GDI_MOUSE_BUTTONS) {
        const BYTE down = action == GLFW_PRESS ? 0x80 : 0;
        if (g_mouseButtons[button] != down) {
            g_mouseButtons[button] = down;
            gdi_queue_push(&g_mouseQueue, DIMOFS_BUTTON0 + (DWORD) button, down);
        }
    }
    if (g_prevButton)
        g_prevButton(w, button, action, mods);
}

static void gdi_cursor_cb(GLFWwindow *w, double x, double y) {
    if (g_cursorKnown && gdi_window_focused()) {
        g_mouseDX += x - g_cursorX;
        g_mouseDY += y - g_cursorY;
    }
    g_cursorX = x;
    g_cursorY = y;
    g_cursorKnown = 1;
    if (g_prevCursor)
        g_prevCursor(w, x, y);
}

static void gdi_scroll_cb(GLFWwindow *w, double dx, double dy) {
    if (gdi_window_focused())
        g_mouseDZ += dy * WHEEL_DELTA;
    if (g_prevScroll)
        g_prevScroll(w, dx, dy);
}

static void gdi_install_callbacks(void) {
    if (g_window)
        return;
    g_window = glfwGetCurrentContext();
    if (!g_window) {
        fprintf(hook_log, "[input] no GLFW window yet: keyboard and mouse stay silent\n");
        return;
    }
    g_prevKey = glfwSetKeyCallback(g_window, gdi_key_cb);
    g_prevButton = glfwSetMouseButtonCallback(g_window, gdi_button_cb);
    g_prevCursor = glfwSetCursorPosCallback(g_window, gdi_cursor_cb);
    g_prevScroll = glfwSetScrollCallback(g_window, gdi_scroll_cb);
}

// ---- joysticks ----

static int gdi_is_xinput(int jid) {
    const char *guid = glfwGetJoystickGUID(jid);
    return guid && strncmp(guid, "78696e707574", 12) == 0;// "xinput"
}

static int gdi_hex_byte(const char *s) {
    unsigned v = 0;
    return sscanf(s, "%2x", &v) == 1 ? (int) v : 0;
}

// guidProduct.Data1 = MAKELONG(vid, pid), as DirectInput reports it for HID devices.
static DWORD gdi_joystick_product(int jid) {
    if (gdi_is_xinput(jid))
        return MAKELONG(GDI_VID_MICROSOFT, GDI_PID_XBOX360);
    const char *guid = glfwGetJoystickGUID(jid);
    if (!guid || strlen(guid) < 20 || strncmp(guid, "03000000", 8) != 0)
        return 0;
    const int vid = gdi_hex_byte(guid + 8) | (gdi_hex_byte(guid + 10) << 8);
    const int pid = gdi_hex_byte(guid + 16) | (gdi_hex_byte(guid + 18) << 8);
    return MAKELONG(vid, pid);
}

static void gdi_joystick_counts(int jid, int *axes, int *buttons, int *povs) {
    if (gdi_is_xinput(jid)) {
        *axes = GDI_XINPUT_AXES;
        *buttons = GDI_XINPUT_BUTTONS;
        *povs = 1;
        return;
    }
    int na = 0, nb = 0, nh = 0;
    glfwGetJoystickAxes(jid, &na);
    glfwGetJoystickButtons(jid, &nb);
    glfwGetJoystickHats(jid, &nh);
    nb -= 4 * nh;// GLFW appends each hat as four buttons
    *axes = na < GDI_NUM_JOY_AXES ? na : GDI_NUM_JOY_AXES;
    *buttons = nb < 0 ? 0 : (nb > 32 ? 32 : nb);
    *povs = nh > 4 ? 4 : nh;
}

// GLFW normalises a DirectInput axis set to -32768..32767 as (v + 0.5) / 32767.5; undo that and
// shift to DirectInput's default 0..65535.
static LONG gdi_axis_value(float a) {
    const long v = lround(a * 32767.5 - 0.5) + GDI_AXIS_CENTER;
    return v < 0 ? 0 : (v > GDI_AXIS_MAX ? GDI_AXIS_MAX : v);
}

static DWORD gdi_pov_value(unsigned char hat) {
    switch (hat) {
        case GLFW_HAT_UP:
            return 0;
        case GLFW_HAT_RIGHT_UP:
            return 4500;
        case GLFW_HAT_RIGHT:
            return 9000;
        case GLFW_HAT_RIGHT_DOWN:
            return 13500;
        case GLFW_HAT_DOWN:
            return 18000;
        case GLFW_HAT_LEFT_DOWN:
            return 22500;
        case GLFW_HAT_LEFT:
            return 27000;
        case GLFW_HAT_LEFT_UP:
            return 31500;
        default:
            return 0xffffffffu;
    }
}

static int gdi_axis_index(DWORD ofs) {
    switch (ofs) {
        case DIJOFS_X:
            return 0;
        case DIJOFS_Y:
            return 1;
        case DIJOFS_Z:
            return 2;
        case DIJOFS_RX:
            return 3;
        case DIJOFS_RY:
            return 4;
        case DIJOFS_RZ:
            return 5;
        case DIJOFS_SLIDER(0):
            return 6;
        case DIJOFS_SLIDER(1):
            return 7;
        default:
            return -1;
    }
}

static HRESULT gdi_joystick_state(int jid, DIJOYSTATE *s) {
    if (!glfwJoystickPresent(jid))
        return DIERR_INPUTLOST;
    int na = 0, nb = 0, nh = 0;
    const float *axes = glfwGetJoystickAxes(jid, &na);
    const unsigned char *buttons = glfwGetJoystickButtons(jid, &nb);
    const unsigned char *hats = glfwGetJoystickHats(jid, &nh);
    if (!axes || !buttons)
        return DIERR_INPUTLOST;

    memset(s, 0, sizeof(*s));
    for (int p = 0; p < 4; p++)
        s->rgdwPOV[p] = 0xffffffffu;
    if (gdi_is_xinput(jid)) {
        // Xbox 360 over DirectInput: triggers share Z, LT above centre and RT below.
        const double lt = (axes[4] + 1.0) / 2.0;
        const double rt = (axes[5] + 1.0) / 2.0;
        const long z = lround(GDI_AXIS_CENTER + (lt - rt) * (GDI_AXIS_MAX - GDI_AXIS_CENTER));
        s->lX = gdi_axis_value(axes[0]);
        s->lY = gdi_axis_value(axes[1]);
        s->lZ = z < 0 ? 0 : (z > GDI_AXIS_MAX ? GDI_AXIS_MAX : z);
        s->lRx = gdi_axis_value(axes[2]);
        s->lRy = gdi_axis_value(axes[3]);
        for (int b = 0; b < GDI_XINPUT_BUTTONS && b < nb; b++)
            s->rgbButtons[b] = buttons[b] ? 0x80 : 0;
        if (nh > 0)
            s->rgdwPOV[0] = gdi_pov_value(hats[0]);
        return DI_OK;
    }

    LONG *slots[GDI_NUM_JOY_AXES] = {&s->lX, &s->lY, &s->lZ, &s->lRx, &s->lRy, &s->lRz, &s->rglSlider[0], &s->rglSlider[1]};
    int numAxes, numButtons, numPovs;
    gdi_joystick_counts(jid, &numAxes, &numButtons, &numPovs);
    for (int a = 0; a < numAxes; a++)
        *slots[a] = gdi_axis_value(axes[a]);
    for (int b = 0; b < numButtons; b++)
        s->rgbButtons[b] = buttons[b] ? 0x80 : 0;
    for (int p = 0; p < numPovs; p++)
        s->rgdwPOV[p] = gdi_pov_value(hats[p]);
    return DI_OK;
}

static void gdi_fill_instance(DIDEVICEINSTANCEA *inst, enum GdiKind kind, int jid) {
    memset(inst, 0, sizeof(*inst));
    inst->dwSize = sizeof(*inst);
    switch (kind) {
        case GDI_KEYBOARD:
            inst->guidInstance = GDI_GUID_KEYBOARD;
            inst->guidProduct = GDI_GUID_KEYBOARD;
            inst->dwDevType = DIDEVTYPE_KEYBOARD | (DIDEVTYPEKEYBOARD_PCENH << 8);
            snprintf(inst->tszInstanceName, sizeof(inst->tszInstanceName), "%s", "Keyboard");
            snprintf(inst->tszProductName, sizeof(inst->tszProductName), "%s", "Keyboard");
            break;
        case GDI_MOUSE:
            inst->guidInstance = GDI_GUID_MOUSE;
            inst->guidProduct = GDI_GUID_MOUSE;
            inst->dwDevType = DIDEVTYPE_MOUSE;
            snprintf(inst->tszInstanceName, sizeof(inst->tszInstanceName), "%s", "Mouse");
            snprintf(inst->tszProductName, sizeof(inst->tszProductName), "%s", "Mouse");
            break;
        case GDI_JOYSTICK: {
            const char *name = glfwGetJoystickName(jid);
            inst->guidInstance.Data1 = GDI_JOY_GUID_DATA1;
            inst->guidInstance.Data4[7] = (BYTE) jid;
            inst->guidProduct.Data1 = gdi_joystick_product(jid);
            memcpy(&inst->guidProduct.Data4[2], "PIDVID", 6);
            inst->dwDevType = DIDEVTYPE_JOYSTICK | (DIDEVTYPEJOYSTICK_GAMEPAD << 8) | DIDEVTYPE_HID;
            snprintf(inst->tszInstanceName, sizeof(inst->tszInstanceName), "%s", name ? name : "Joystick");
            snprintf(inst->tszProductName, sizeof(inst->tszProductName), "%s", name ? name : "Joystick");
            break;
        }
    }
}

// ---- IDirectInputDevice2A ----

#define GDI_DEV(This) ((GdiDevice *) (This))

static HRESULT __stdcall dev_QueryInterface(IDirectInputDevice2A *This, REFIID riid, LPVOID *out) {
    GDI_DEV(This)->refs++;
    *out = This;
    return DI_OK;
}

static ULONG __stdcall dev_AddRef(IDirectInputDevice2A *This) {
    return ++GDI_DEV(This)->refs;
}

static ULONG __stdcall dev_Release(IDirectInputDevice2A *This) {
    const LONG refs = --GDI_DEV(This)->refs;
    if (refs <= 0)
        HeapFree(GetProcessHeap(), 0, This);
    return refs < 0 ? 0 : refs;
}

static HRESULT __stdcall dev_GetCapabilities(IDirectInputDevice2A *This, LPDIDEVCAPS caps) {
    const GdiDevice *d = GDI_DEV(This);
    const DWORD size = caps->dwSize;
    memset(caps, 0, size);
    caps->dwSize = size;
    caps->dwFlags = DIDC_ATTACHED;
    switch (d->kind) {
        case GDI_KEYBOARD:
            caps->dwDevType = DIDEVTYPE_KEYBOARD | (DIDEVTYPEKEYBOARD_PCENH << 8);
            caps->dwButtons = 128;
            break;
        case GDI_MOUSE:
            caps->dwDevType = DIDEVTYPE_MOUSE;
            caps->dwAxes = 3;
            caps->dwButtons = GDI_MOUSE_BUTTONS;
            break;
        case GDI_JOYSTICK: {
            int axes, buttons, povs;
            gdi_joystick_counts(d->jid, &axes, &buttons, &povs);
            caps->dwDevType = DIDEVTYPE_JOYSTICK | (DIDEVTYPEJOYSTICK_GAMEPAD << 8) | DIDEVTYPE_HID;
            caps->dwAxes = axes;
            caps->dwButtons = buttons;
            caps->dwPOVs = povs;
            if (!glfwJoystickPresent(d->jid))
                caps->dwFlags = 0;
            break;
        }
    }
    return DI_OK;
}

static HRESULT __stdcall dev_EnumObjects(IDirectInputDevice2A *This, LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ref, DWORD flags) {
    return DI_OK;
}

static HRESULT __stdcall dev_GetProperty(IDirectInputDevice2A *This, REFGUID prop, LPDIPROPHEADER header) {
    const GdiDevice *d = GDI_DEV(This);
    if (prop == DIPROP_RANGE && d->kind == GDI_JOYSTICK) {
        int axes, buttons, povs;
        gdi_joystick_counts(d->jid, &axes, &buttons, &povs);
        const int index = header->dwHow == DIPH_BYOFFSET ? gdi_axis_index(header->dwObj) : -1;
        if (index < 0 || index >= axes)
            return DIERR_OBJECTNOTFOUND;
        ((LPDIPROPRANGE) header)->lMin = 0;
        ((LPDIPROPRANGE) header)->lMax = GDI_AXIS_MAX;
        return DI_OK;
    }
    if (prop == DIPROP_BUFFERSIZE && d->kind != GDI_JOYSTICK) {
        ((LPDIPROPDWORD) header)->dwData = (d->kind == GDI_KEYBOARD ? g_keyQueue : g_mouseQueue).capacity;
        return DI_OK;
    }
    return DIERR_UNSUPPORTED;
}

static HRESULT __stdcall dev_SetProperty(IDirectInputDevice2A *This, REFGUID prop, LPCDIPROPHEADER header) {
    const GdiDevice *d = GDI_DEV(This);
    if (prop == DIPROP_BUFFERSIZE && d->kind != GDI_JOYSTICK) {
        const DWORD size = ((LPCDIPROPDWORD) header)->dwData;
        (d->kind == GDI_KEYBOARD ? &g_keyQueue : &g_mouseQueue)->capacity = size < GDI_MAX_EVENTS ? size : GDI_MAX_EVENTS;
    }
    return DI_OK;
}

static HRESULT __stdcall dev_Acquire(IDirectInputDevice2A *This) {
    const GdiDevice *d = GDI_DEV(This);
    if (d->kind == GDI_JOYSTICK && !glfwJoystickPresent(d->jid))
        return DIERR_UNPLUGGED;
    return DI_OK;
}

static HRESULT __stdcall dev_Unacquire(IDirectInputDevice2A *This) {
    return DI_OK;
}

static HRESULT __stdcall dev_GetDeviceState(IDirectInputDevice2A *This, DWORD size, LPVOID data) {
    const GdiDevice *d = GDI_DEV(This);
    switch (d->kind) {
        case GDI_KEYBOARD:
            memset(data, 0, size);
            memcpy(data, g_keyState, size < sizeof(g_keyState) ? size : sizeof(g_keyState));
            return DI_OK;
        case GDI_MOUSE: {
            if (size < sizeof(DIMOUSESTATE))
                return DIERR_INVALIDPARAM;
            DIMOUSESTATE *m = (DIMOUSESTATE *) data;
            memset(data, 0, size);
            m->lX = (LONG) g_mouseDX;
            m->lY = (LONG) g_mouseDY;
            m->lZ = (LONG) g_mouseDZ;
            g_mouseDX -= m->lX;
            g_mouseDY -= m->lY;
            g_mouseDZ -= m->lZ;
            memcpy(m->rgbButtons, g_mouseButtons, sizeof(g_mouseButtons));
            return DI_OK;
        }
        case GDI_JOYSTICK: {
            if (size < sizeof(DIJOYSTATE))
                return DIERR_INVALIDPARAM;
            memset(data, 0, size);
            return gdi_joystick_state(d->jid, (DIJOYSTATE *) data);
        }
    }
    return DIERR_GENERIC;
}

static HRESULT __stdcall dev_GetDeviceData(IDirectInputDevice2A *This, DWORD objSize, LPDIDEVICEOBJECTDATA out, LPDWORD inOut, DWORD flags) {
    const GdiDevice *d = GDI_DEV(This);
    if (d->kind == GDI_JOYSTICK) {
        *inOut = 0;
        return DIERR_NOTBUFFERED;
    }
    GdiEventQueue *q = d->kind == GDI_KEYBOARD ? &g_keyQueue : &g_mouseQueue;
    const DWORD n = q->count < *inOut ? q->count : *inOut;
    if (out)
        for (DWORD i = 0; i < n; i++)
            memcpy((char *) out + i * objSize, &q->events[i], objSize < sizeof(DIDEVICEOBJECTDATA) ? objSize : sizeof(DIDEVICEOBJECTDATA));
    *inOut = n;
    const HRESULT hr = q->overflowed ? DI_BUFFEROVERFLOW : DI_OK;
    if (!(flags & DIGDD_PEEK)) {
        memmove(q->events, q->events + n, (q->count - n) * sizeof(DIDEVICEOBJECTDATA));
        q->count -= n;
        q->overflowed = 0;
    }
    return hr;
}

static HRESULT __stdcall dev_SetDataFormat(IDirectInputDevice2A *This, LPCDIDATAFORMAT format) {
    return DI_OK;
}

static HRESULT __stdcall dev_SetEventNotification(IDirectInputDevice2A *This, HANDLE event) {
    return DI_OK;
}

static HRESULT __stdcall dev_SetCooperativeLevel(IDirectInputDevice2A *This, HWND hwnd, DWORD flags) {
    return DI_OK;
}

static HRESULT __stdcall dev_GetObjectInfo(IDirectInputDevice2A *This, LPDIDEVICEOBJECTINSTANCEA info, DWORD obj, DWORD how) {
    return DIERR_OBJECTNOTFOUND;
}

static HRESULT __stdcall dev_GetDeviceInfo(IDirectInputDevice2A *This, LPDIDEVICEINSTANCEA info) {
    const GdiDevice *d = GDI_DEV(This);
    DIDEVICEINSTANCEA inst;
    gdi_fill_instance(&inst, d->kind, d->jid);
    memcpy(info, &inst, info->dwSize && info->dwSize < sizeof(inst) ? info->dwSize : sizeof(inst));
    return DI_OK;
}

static HRESULT __stdcall dev_RunControlPanel(IDirectInputDevice2A *This, HWND hwnd, DWORD flags) {
    return DIERR_UNSUPPORTED;
}

static HRESULT __stdcall dev_Initialize(IDirectInputDevice2A *This, HINSTANCE hinst, DWORD version, REFGUID guid) {
    return DI_OK;
}

static HRESULT __stdcall dev_CreateEffect(IDirectInputDevice2A *This, REFGUID guid, LPCDIEFFECT effect, LPDIRECTINPUTEFFECT *out, LPUNKNOWN outer) {
    if (out)
        *out = NULL;
    return DIERR_UNSUPPORTED;
}

static HRESULT __stdcall dev_EnumEffects(IDirectInputDevice2A *This, LPDIENUMEFFECTSCALLBACKA cb, LPVOID ref, DWORD type) {
    return DI_OK;
}

static HRESULT __stdcall dev_GetEffectInfo(IDirectInputDevice2A *This, LPDIEFFECTINFOA info, REFGUID guid) {
    return DIERR_UNSUPPORTED;
}

static HRESULT __stdcall dev_GetForceFeedbackState(IDirectInputDevice2A *This, LPDWORD state) {
    return DIERR_UNSUPPORTED;
}

static HRESULT __stdcall dev_SendForceFeedbackCommand(IDirectInputDevice2A *This, DWORD command) {
    return DIERR_UNSUPPORTED;
}

static HRESULT __stdcall dev_EnumCreatedEffectObjects(IDirectInputDevice2A *This, LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, LPVOID ref, DWORD flags) {
    return DI_OK;
}

static HRESULT __stdcall dev_Escape(IDirectInputDevice2A *This, LPDIEFFESCAPE escape) {
    return DIERR_UNSUPPORTED;
}

static HRESULT __stdcall dev_Poll(IDirectInputDevice2A *This) {
    const GdiDevice *d = GDI_DEV(This);
    if (d->kind == GDI_JOYSTICK && !glfwJoystickPresent(d->jid))
        return DIERR_INPUTLOST;
    return DI_NOEFFECT;
}

static HRESULT __stdcall dev_SendDeviceData(IDirectInputDevice2A *This, DWORD size, LPCDIDEVICEOBJECTDATA data, LPDWORD inOut, DWORD flags) {
    return DIERR_UNSUPPORTED;
}

// One table for every device, as the system DirectInput has; stdControl_delta patches its
// GetDeviceState slot for the Sony layout rewrite.
static IDirectInputDevice2AVtbl g_deviceVtbl = {
    dev_QueryInterface,
    dev_AddRef,
    dev_Release,
    dev_GetCapabilities,
    dev_EnumObjects,
    dev_GetProperty,
    dev_SetProperty,
    dev_Acquire,
    dev_Unacquire,
    dev_GetDeviceState,
    dev_GetDeviceData,
    dev_SetDataFormat,
    dev_SetEventNotification,
    dev_SetCooperativeLevel,
    dev_GetObjectInfo,
    dev_GetDeviceInfo,
    dev_RunControlPanel,
    dev_Initialize,
    dev_CreateEffect,
    dev_EnumEffects,
    dev_GetEffectInfo,
    dev_GetForceFeedbackState,
    dev_SendForceFeedbackCommand,
    dev_EnumCreatedEffectObjects,
    dev_Escape,
    dev_Poll,
    dev_SendDeviceData,
};

// ---- IDirectInputA ----

static HRESULT __stdcall root_QueryInterface(IDirectInputA *This, REFIID riid, LPVOID *out) {
    ((GdiRoot *) This)->refs++;
    *out = This;
    return DI_OK;
}

static ULONG __stdcall root_AddRef(IDirectInputA *This) {
    return ++((GdiRoot *) This)->refs;
}

static ULONG __stdcall root_Release(IDirectInputA *This) {
    const LONG refs = --((GdiRoot *) This)->refs;
    return refs < 0 ? 0 : refs;
}

static HRESULT __stdcall root_CreateDevice(IDirectInputA *This, REFGUID guid, LPDIRECTINPUTDEVICEA *out, LPUNKNOWN outer) {
    GdiDevice d = {&g_deviceVtbl, 1, GDI_KEYBOARD, -1};
    if (IsEqualGUID(guid, &GDI_GUID_KEYBOARD)) {
        d.kind = GDI_KEYBOARD;
    } else if (IsEqualGUID(guid, &GDI_GUID_MOUSE)) {
        d.kind = GDI_MOUSE;
    } else if (guid->Data1 == GDI_JOY_GUID_DATA1 && guid->Data4[7] < GDI_MAX_JOYSTICKS) {
        d.kind = GDI_JOYSTICK;
        d.jid = guid->Data4[7];
    } else {
        *out = NULL;
        return DIERR_DEVICENOTREG;
    }
    GdiDevice *dev = (GdiDevice *) HeapAlloc(GetProcessHeap(), 0, sizeof(GdiDevice));
    *dev = d;
    *out = (LPDIRECTINPUTDEVICEA) dev;
    return DI_OK;
}

static int gdi_enum_one(LPDIENUMDEVICESCALLBACKA cb, LPVOID ref, DWORD type, enum GdiKind kind, int jid) {
    DIDEVICEINSTANCEA inst;
    gdi_fill_instance(&inst, kind, jid);
    if (type != 0 && (BYTE) inst.dwDevType != (BYTE) type)
        return 1;
    return cb(&inst, ref) != DIENUM_STOP;
}

static HRESULT __stdcall root_EnumDevices(IDirectInputA *This, DWORD type, LPDIENUMDEVICESCALLBACKA cb, LPVOID ref, DWORD flags) {
    if (flags & DIEDFL_FORCEFEEDBACK)
        return DI_OK;
    if (!gdi_enum_one(cb, ref, type, GDI_MOUSE, -1) || !gdi_enum_one(cb, ref, type, GDI_KEYBOARD, -1))
        return DI_OK;
    for (int jid = 0; jid < GDI_MAX_JOYSTICKS; jid++)
        if (glfwJoystickPresent(jid) && !gdi_enum_one(cb, ref, type, GDI_JOYSTICK, jid))
            break;
    return DI_OK;
}

static HRESULT __stdcall root_GetDeviceStatus(IDirectInputA *This, REFGUID guid) {
    if (guid->Data1 == GDI_JOY_GUID_DATA1)
        return glfwJoystickPresent(guid->Data4[7]) ? DI_OK : DI_NOTATTACHED;
    return DI_OK;
}

static HRESULT __stdcall root_RunControlPanel(IDirectInputA *This, HWND hwnd, DWORD flags) {
    return DIERR_UNSUPPORTED;
}

static HRESULT __stdcall root_Initialize(IDirectInputA *This, HINSTANCE hinst, DWORD version) {
    return DI_OK;
}

static IDirectInputAVtbl g_rootVtbl = {
    root_QueryInterface, root_AddRef, root_Release, root_CreateDevice,
    root_EnumDevices,    root_GetDeviceStatus, root_RunControlPanel, root_Initialize,
};

static GdiRoot g_root = {&g_rootVtbl, 0};

HRESULT __stdcall glfw_dinput_create(HINSTANCE hinst, DWORD version, void **out, void *outer) {
    gdi_install_callbacks();
    g_logKeys = GetEnvironmentVariableA("SWR_INPUT_LOG", NULL, 0) != 0;
    g_root.refs++;
    *out = &g_root;
    int pads = 0;
    for (int jid = 0; jid < GDI_MAX_JOYSTICKS; jid++)
        if (glfwJoystickPresent(jid)) {
            fprintf(hook_log, "[input] joystick %d: %s (%s)%s\n", jid, glfwGetJoystickName(jid), glfwGetJoystickGUID(jid),
                    gdi_is_xinput(jid) ? " xinput" : "");
            pads++;
        }
    fprintf(hook_log, "[input] GLFW input (DirectInput %x interface): keyboard, mouse, %d joystick(s)\n", (unsigned) version, pads);
    fflush(hook_log);
    return DI_OK;
}
