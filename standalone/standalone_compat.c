// What retail gets from its compatibility layers (RUNASADMIN), done without elevation: the game opens
// its HKLM key with KEY_ALL_ACCESS, which a standard user is denied. Our own imports are redirected
// to wrappers that fall back to read access, so settings load; saving them to HKLM is skipped.

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "standalone.h"

typedef LSTATUS(WINAPI* RegOpenKeyExA_fn)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
typedef LSTATUS(WINAPI* RegCreateKeyExA_fn)(HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM, const LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD);
static RegOpenKeyExA_fn real_RegOpenKeyExA;
static RegCreateKeyExA_fn real_RegCreateKeyExA;

// The 32-bit installer wrote the game's keys; a 64-bit build reads the same (WOW64) view.
#if defined(_WIN64)
#define COMPAT_VIEW KEY_WOW64_32KEY
#else
#define COMPAT_VIEW 0
#endif

static LSTATUS WINAPI compat_RegOpenKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult)
{
    LSTATUS s = real_RegOpenKeyExA(hKey, lpSubKey, ulOptions, samDesired | COMPAT_VIEW, phkResult);
    if (s == ERROR_ACCESS_DENIED)
        s = real_RegOpenKeyExA(hKey, lpSubKey, ulOptions, KEY_READ | COMPAT_VIEW, phkResult);
    return s;
}

static LSTATUS WINAPI compat_RegCreateKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD Reserved, LPSTR lpClass, DWORD dwOptions, REGSAM samDesired,
                                             const LPSECURITY_ATTRIBUTES lpSecurityAttributes, PHKEY phkResult, LPDWORD lpdwDisposition)
{
    LSTATUS s = real_RegCreateKeyExA(hKey, lpSubKey, Reserved, lpClass, dwOptions, samDesired | COMPAT_VIEW, lpSecurityAttributes, phkResult, lpdwDisposition);
    if (s == ERROR_ACCESS_DENIED) {
        s = real_RegOpenKeyExA(hKey, lpSubKey, 0, KEY_READ | COMPAT_VIEW, phkResult);
        if (s == ERROR_SUCCESS && lpdwDisposition)
            *lpdwDisposition = REG_OPENED_EXISTING_KEY;
    }
    return s;
}

static int patch_own_import(const char* dll, const char* name, void* replacement)
{
    uint8_t* base = (uint8_t*)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY* dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir->VirtualAddress); d->Name; d++) {
        if (_stricmp((const char*)(base + d->Name), dll))
            continue;
        uintptr_t* names = (uintptr_t*)(base + d->OriginalFirstThunk);
        uintptr_t* iat = (uintptr_t*)(base + d->FirstThunk);
        for (; *names; names++, iat++) {
            if ((*names & IMAGE_ORDINAL_FLAG) || strcmp((const char*)(base + *names + 2), name))
                continue;
            DWORD old;
            VirtualProtect(iat, sizeof(*iat), PAGE_READWRITE, &old);
            *iat = (uintptr_t)replacement;
            VirtualProtect(iat, sizeof(*iat), old, &old);
            return 1;
        }
    }
    return 0;
}

// SWR_STANDALONE_RES=WxH overrides the "Display Width"/"Display Height" registry values.
typedef LSTATUS(WINAPI* RegQueryValueExA_fn)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
static RegQueryValueExA_fn real_RegQueryValueExA;
static int res_override_w, res_override_h;

static LSTATUS WINAPI compat_RegQueryValueExA(HKEY hKey, LPCSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD size)
{
    int value = 0;
    if (name && !_stricmp(name, "Display Width"))
        value = res_override_w;
    else if (name && !_stricmp(name, "Display Height"))
        value = res_override_h;
    if (value && data && size && *size >= 4) {
        *(DWORD*)data = (DWORD)value;
        *size = 4;
        if (type)
            *type = REG_DWORD;
        return ERROR_SUCCESS;
    }
    return real_RegQueryValueExA(hKey, name, reserved, type, data, size);
}

void standalone_install_compat(void)
{
    HMODULE advapi = GetModuleHandleA("advapi32.dll");
    real_RegOpenKeyExA = (RegOpenKeyExA_fn)GetProcAddress(advapi, "RegOpenKeyExA");
    real_RegCreateKeyExA = (RegCreateKeyExA_fn)GetProcAddress(advapi, "RegCreateKeyExA");
    real_RegQueryValueExA = (RegQueryValueExA_fn)GetProcAddress(advapi, "RegQueryValueExA");
    int n = patch_own_import("advapi32.dll", "RegOpenKeyExA", (void*)compat_RegOpenKeyExA);
    n += patch_own_import("advapi32.dll", "RegCreateKeyExA", (void*)compat_RegCreateKeyExA);
    char res[32];
    if (GetEnvironmentVariableA("SWR_STANDALONE_RES", res, sizeof(res)) && sscanf(res, "%dx%d", &res_override_w, &res_override_h) == 2)
        n += patch_own_import("advapi32.dll", "RegQueryValueExA", (void*)compat_RegQueryValueExA);
    fprintf(standalone_log, "registry compat shims: %d\n", n);
}
