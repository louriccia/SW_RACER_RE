// DirectDraw entry points resolved at runtime, after standalone_init() has put the game directory
// on the DLL search path, so a game-folder wrapper (DDrawCompat, dgVoodoo) is used like retail does.

#include <windows.h>
#include <ddraw.h>

static FARPROC ddraw_proc(const char* name)
{
    static HMODULE ddraw = NULL;
    if (!ddraw)
        ddraw = LoadLibraryA("ddraw.dll");
    return ddraw ? GetProcAddress(ddraw, name) : NULL;
}

HRESULT WINAPI DirectDrawCreate(GUID* lpGUID, LPDIRECTDRAW* lplpDD, IUnknown* pUnkOuter)
{
    typedef HRESULT(WINAPI * fn_t)(GUID*, LPDIRECTDRAW*, IUnknown*);
    fn_t fn = (fn_t)ddraw_proc("DirectDrawCreate");
    return fn ? fn(lpGUID, lplpDD, pUnkOuter) : DDERR_GENERIC;
}

HRESULT WINAPI DirectDrawEnumerateA(LPDDENUMCALLBACKA lpCallback, LPVOID lpContext)
{
    typedef HRESULT(WINAPI * fn_t)(LPDDENUMCALLBACKA, LPVOID);
    fn_t fn = (fn_t)ddraw_proc("DirectDrawEnumerateA");
    return fn ? fn(lpCallback, lpContext) : DDERR_GENERIC;
}
