// What retail gets from its compatibility layers (RUNASADMIN), done without elevation: the game opens
// its HKLM key with KEY_ALL_ACCESS, which a standard user is denied. Our own imports are redirected
// to wrappers that fall back to read access, so settings load; saving them to HKLM is skipped.

#include <windows.h>
#include <string.h>

#include "standalone.h"

typedef LSTATUS(WINAPI* RegOpenKeyExA_fn)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
typedef LSTATUS(WINAPI* RegCreateKeyExA_fn)(HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM, const LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD);
static RegOpenKeyExA_fn real_RegOpenKeyExA;
static RegCreateKeyExA_fn real_RegCreateKeyExA;

static LSTATUS WINAPI compat_RegOpenKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult)
{
    LSTATUS s = real_RegOpenKeyExA(hKey, lpSubKey, ulOptions, samDesired, phkResult);
    if (s == ERROR_ACCESS_DENIED)
        s = real_RegOpenKeyExA(hKey, lpSubKey, ulOptions, KEY_READ, phkResult);
    return s;
}

static LSTATUS WINAPI compat_RegCreateKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD Reserved, LPSTR lpClass, DWORD dwOptions, REGSAM samDesired,
                                             const LPSECURITY_ATTRIBUTES lpSecurityAttributes, PHKEY phkResult, LPDWORD lpdwDisposition)
{
    LSTATUS s = real_RegCreateKeyExA(hKey, lpSubKey, Reserved, lpClass, dwOptions, samDesired, lpSecurityAttributes, phkResult, lpdwDisposition);
    if (s == ERROR_ACCESS_DENIED) {
        s = real_RegOpenKeyExA(hKey, lpSubKey, 0, KEY_READ, phkResult);
        if (s == ERROR_SUCCESS && lpdwDisposition)
            *lpdwDisposition = REG_OPENED_EXISTING_KEY;
    }
    return s;
}

static int patch_own_import(const char* dll, const char* name, void* replacement)
{
    uint8_t* base = (uint8_t*)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY* dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir->VirtualAddress); d->Name; d++) {
        if (_stricmp((const char*)(base + d->Name), dll))
            continue;
        uint32_t* names = (uint32_t*)(base + d->OriginalFirstThunk);
        uint32_t* iat = (uint32_t*)(base + d->FirstThunk);
        for (; *names; names++, iat++) {
            if ((*names & IMAGE_ORDINAL_FLAG32) || strcmp((const char*)(base + *names + 2), name))
                continue;
            DWORD old;
            VirtualProtect(iat, 4, PAGE_READWRITE, &old);
            *iat = (uint32_t)replacement;
            VirtualProtect(iat, 4, old, &old);
            return 1;
        }
    }
    return 0;
}

void standalone_install_compat(void)
{
    HMODULE advapi = GetModuleHandleA("advapi32.dll");
    real_RegOpenKeyExA = (RegOpenKeyExA_fn)GetProcAddress(advapi, "RegOpenKeyExA");
    real_RegCreateKeyExA = (RegCreateKeyExA_fn)GetProcAddress(advapi, "RegCreateKeyExA");
    int n = patch_own_import("advapi32.dll", "RegOpenKeyExA", (void*)compat_RegOpenKeyExA);
    n += patch_own_import("advapi32.dll", "RegCreateKeyExA", (void*)compat_RegCreateKeyExA);
    fprintf(standalone_log, "registry compat shims: %d\n", n);
}
