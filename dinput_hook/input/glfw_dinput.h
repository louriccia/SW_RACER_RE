#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// DirectInputCreateA backed by GLFW: keyboard, mouse and joysticks reach the faithful stdControl
// through the DirectInput interface it was written against, without the system DirectInput.
HRESULT __stdcall glfw_dinput_create(HINSTANCE hinst, DWORD version, void **out, void *outer);

#ifdef __cplusplus
}
#endif
