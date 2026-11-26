#include "stdPlatform.h"
#include "./Win95/std.h"
#include "General/stdMemory.h"
#include "types.h"
#include "globals.h"

#include <macros.h>
#include <engine_config.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#define CSTDLIB_HEAP_ONLY
#include <Platform/cstdlib.h> // c_malloc/c_free: the game's CRT heap, not the DLL's

// 0x00408e40
void* stdPlatform_Alloc(unsigned int size)
{
    return SWR_FN32(stdPlatform_hostServices_ptr->alloc)(size);
}

// 0x00484720
unsigned int stdPlatform_Initialize(HostServices* hs)
{
    unsigned short cw;

    stdPlatform_hostServices_ptr = hs;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    stdPlatfom_FPU1 = cw;
    *(unsigned short*)&stdPlatfom_FPU3 = (cw & 0xff) | STD_FPU_CW_ROUND_UP;
    stdPlatfom_FPU2 = (cw & 0xff) | STD_FPU_CW_ROUND_DOWN;
    stdPlatform_hostServices_initialized = 1;
    // retail leaves the control word in AX
    return ((unsigned int)hs & 0xffff0000) | cw;
}

// 0x00484760
void stdPlatform_Shutdown(void)
{
    stdPlatform_hostServices_initialized = 0;
}

// Called as a macro using errorPrint, __FILE__, __LINE__, "msg", ...
// 0x00484780
int stdPlatform_DebugLog(void* log_function, char* compile_filepath, int line_nb, char* msg, ...)
{
    int baseOffset = 0;
    int hasSeparator = 0;
    int i;
    int len;
    int remaining;
    va_list args;

    if (log_function == NULL)
        return 0;

    for (i = 0; compile_filepath[i] != '\0'; i++) {
        if (compile_filepath[i] == '\\') {
            hasSeparator = 1;
            baseOffset = i;
        }
    }
    if (hasSeparator)
        baseOffset++;

    len = _snprintf(debug_buffer, sizeof(debug_buffer), compile_filepath + baseOffset);
    remaining = sizeof(debug_buffer) - len;
    i = _snprintf(debug_buffer + len, remaining, "(%d): ", line_nb);
    remaining -= i;
    len += i;
    va_start(args, msg);
    _vsnprintf(debug_buffer + len, remaining, msg, args);
    va_end(args);
    ((int (*)(const char*, ...))log_function)(debug_buffer);
    return 0;
}

// 0x0048c570
int stdPlatform_Printf(const char* format, ...)
{
    va_list args;
    va_start(args, format);

    vsnprintf(std_output_buffer, 0x800, format, args);
    OutputDebugStringA(std_output_buffer);

    va_end(args);

    return 1;
}

// 0x0048c4a0
void stdPlatform_Assert(const char* msg, const char* filePath, int line)
{
    char buffer[512];
    int baseOffset = 0;
    int hasSeparator = 0;
    int i;

    if (stdPlatform_bAssertInProgress) {
        DebugBreak();
        exit(1);
    }
    stdPlatform_bAssertInProgress = 1;

    for (i = 0; filePath[i] != '\0'; i++) {
        if (filePath[i] == '\\') {
            hasSeparator = 1;
            baseOffset = i;
        }
    }
    if (hasSeparator)
        baseOffset++;

    _snprintf(buffer, sizeof(buffer), "%s(%d):  %s\n", filePath + baseOffset, line, msg);
    SWR_FN32(stdPlatform_hostServices_ptr->errorPrint)("ASSERT: %s", buffer);
    MessageBoxA(NULL, buffer, "Assert Handler", MB_TASKMODAL);
    DebugBreak();
    exit(1);
}

// 0x48c5a0
void* stdPlatform_AllocHandle(size_t _Size)
{
    return c_malloc(_Size);
}

// 0x0048c5b0
void stdPlatform_FreeHandle(void* _Memory)
{
    return c_free(_Memory);
}

// 0x0048c5c0
void* stdPlatform_ReallocHandle(void* _Memory, size_t _NewSize)
{
    return c_realloc(_Memory, _NewSize);
}

// 0x0048c5e0
uint32_t stdPlatform_LockHandle(uint32_t param_1)
{
    return param_1;
}

// 0x00423cb0
void stdPlatform_noop(uint32_t x)
{
    return;
}

#if defined(__x86_64__)
// A 4-byte function slot reaches only our own image; system DLLs load above 4 GB.
static uint32_t stdPlatform_TimerTick(void)
{
    return timeGetTime();
}

static int stdPlatform_Feof(FILE* f)
{
    return feof(f);
}
#endif

// 0x0048c3d0
void stdPlatform_InitServices(HostServices* handlers)
{
    handlers->some_float = 1000.0;
    SWR_FN32_SET(handlers->statusPrint, stdPlatform_Printf);
    SWR_FN32_SET(handlers->messagePrint, stdPlatform_Printf);
    SWR_FN32_SET(handlers->warningPrint, stdPlatform_Printf);
    SWR_FN32_SET(handlers->errorPrint, stdPlatform_Printf);
    SWR_FN32_SET(handlers->debugPrint, NULL);
    SWR_FN32_SET(handlers->assert, stdPlatform_Assert);
    handlers->unk_0 = 0;
    SWR_FN32_SET(handlers->alloc, daAlloc);
    SWR_FN32_SET(handlers->free, daFree);
    SWR_FN32_SET(handlers->realloc, daRealloc);
#if defined(__x86_64__)
    SWR_FN32_SET(handlers->getTimerTick, stdPlatform_TimerTick);
#else
    SWR_FN32_SET(handlers->getTimerTick, (uint32_t(*)(void))timeGetTime); // stdcall without args can be safely converted to cdecl
#endif
    SWR_FN32_SET(handlers->fileOpen, stdFileOpen);
    SWR_FN32_SET(handlers->fileClose, stdFileClose);
    SWR_FN32_SET(handlers->fileRead, stdFileRead);
    SWR_FN32_SET(handlers->fileGets, stdFileGets);
    SWR_FN32_SET(handlers->fileWrite, stdFileWrite);
#if defined(__x86_64__)
    SWR_FN32_SET(handlers->feof, stdPlatform_Feof);
#else
    SWR_FN32_SET(handlers->feof, feof);
#endif
    SWR_FN32_SET(handlers->ftell, stdFtell);
    SWR_FN32_SET(handlers->fseek, stdFseek);
    SWR_FN32_SET(handlers->fileSize, stdFileSize);
    SWR_FN32_SET(handlers->filePrintf, stdFilePrintf);
    SWR_FN32_SET(handlers->fileGetws, stdFileGetws);
    SWR_FN32_SET(handlers->allocHandle, stdPlatform_AllocHandle);
    SWR_FN32_SET(handlers->freeHandle, stdPlatform_FreeHandle);
    SWR_FN32_SET(handlers->reallocHandle, stdPlatform_ReallocHandle);
    SWR_FN32_SET(handlers->lockHandle, stdPlatform_LockHandle);
    SWR_FN32_SET(handlers->unlockHandle, stdPlatform_noop);
}
