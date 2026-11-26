#ifndef SMUSH_H
#define SMUSH_H

#include <Windows.h>
#include "types.h"

// smush.dll (SMUSH video player) is imported by the EXE; there is no import library, so calls go
// through the EXE's import address table slots.
#include "swr_data_addr.h"

#define SMUSH_IAT_SET_VOLUME (0x004ac1bc)
#define SMUSH_IAT_PLAY (0x004ac1c0)
#define SMUSH_IAT_SHUTDOWN (0x004ac1c4)
#define SMUSH_IAT_STARTUP (0x004ac1c8)

typedef int (*SmushFrameCallback)(const SmushImage* image);

typedef int (*SmushStartup_t)(HWND hwnd, int unk);
typedef void (*SmushSetVolume_t)(int volume);
typedef int (*SmushPlay_t)(const char* file, int flags, int unk2, int unk3, int unk4, int width, int height, int unk7, int unk8, SmushFrameCallback callback, int unk10, int unk11, int unk12);
typedef void (*SmushShutdown_t)(void);

// Import slots are 4 bytes on every build (src/ptr32.h): call them as SmushX(...), fill them with
// SWR_FN32_SET(SmushX_SLOT, fn).
typedef SWR_FNSLOT_T(SmushStartup_t, SmushStartup_slot);
typedef SWR_FNSLOT_T(SmushSetVolume_t, SmushSetVolume_slot);
typedef SWR_FNSLOT_T(SmushPlay_t, SmushPlay_slot);
typedef SWR_FNSLOT_T(SmushShutdown_t, SmushShutdown_slot);
#define SmushStartup_SLOT (*(SmushStartup_slot*)SWR_GLOBAL_PTR(SMUSH_IAT_STARTUP))
#define SmushSetVolume_SLOT (*(SmushSetVolume_slot*)SWR_GLOBAL_PTR(SMUSH_IAT_SET_VOLUME))
#define SmushPlay_SLOT (*(SmushPlay_slot*)SWR_GLOBAL_PTR(SMUSH_IAT_PLAY))
#define SmushShutdown_SLOT (*(SmushShutdown_slot*)SWR_GLOBAL_PTR(SMUSH_IAT_SHUTDOWN))
#define SmushStartup SWR_FN32(SmushStartup_SLOT)
#define SmushSetVolume SWR_FN32(SmushSetVolume_SLOT)
#define SmushPlay SWR_FN32(SmushPlay_SLOT)
#define SmushShutdown SWR_FN32(SmushShutdown_SLOT)

#endif // SMUSH_H
