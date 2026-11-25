// Standalone launcher: runs the game entirely from the src/ reimplementation, without executing
// any retail code. Retail's data sections are still taken from the player's own SWEP1RCR.EXE and
// mapped at their original addresses, so the fixed-address globals in globals.h keep working.
// Retail's code range is filled with int3, and every function we have a body for gets a jmp at its
// retail address, so code pointers stored in retail data (object callback tables and the like)
// land in our code. Anything else that reaches retail code traps and is logged.

#include <windows.h>
#define DIRECTINPUT_VERSION 0x0500
#include <dinput.h>
#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "standalone.h"

#define RETAIL_BASE 0x00400000u
#define RETAIL_TEXT_LO 0x00401000u
#define RETAIL_TEXT_HI 0x004ac000u

// SizeOfImage of SWEP1RCR.EXE, .bind included.
#define RETAIL_SPAN 0x00af3000u

FILE* standalone_log = NULL;
FILE* hook_log = NULL;
static uint32_t retail_image_size = 0;

static void die(const char* fmt, const char* arg)
{
    char msg[1024];
    snprintf(msg, sizeof(msg), fmt, arg);
    if (standalone_log) {
        fprintf(standalone_log, "FATAL: %s\n", msg);
        fflush(standalone_log);
    }
    MessageBoxA(NULL, msg, "SWE1R standalone", MB_OK | MB_ICONERROR);
    ExitProcess(1);
}

static uint8_t* read_file(const char* path, DWORD* size)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return NULL;
    *size = GetFileSize(f, NULL);
    uint8_t* buf = (uint8_t*)HeapAlloc(GetProcessHeap(), 0, *size);
    DWORD got = 0;
    ReadFile(f, buf, *size, &got, NULL);
    CloseHandle(f);
    return got == *size ? buf : NULL;
}

static LONG CALLBACK retail_trap_handler(EXCEPTION_POINTERS* ep)
{
    uint32_t pc = (uint32_t)ep->ExceptionRecord->ExceptionAddress;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_BREAKPOINT && code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    uint32_t* esp = (uint32_t*)ep->ContextRecord->Esp;
    fprintf(standalone_log, "EXCEPTION 0x%08lx at 0x%08x (esp 0x%08x, [esp] 0x%08x)", code, pc, (uint32_t)esp,
            IsBadReadPtr(esp, 4) ? 0 : esp[0]);
    if (code == EXCEPTION_ACCESS_VIOLATION)
        fprintf(standalone_log, " %s 0x%08x", ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                (uint32_t)ep->ExceptionRecord->ExceptionInformation[1]);
    if (pc >= RETAIL_TEXT_LO && pc < RETAIL_TEXT_HI)
        fprintf(standalone_log, "  <- reached RETAIL code with no reimplementation");
    HMODULE pcmod = NULL;
    char modname[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &pcmod))
        GetModuleFileNameA(pcmod, modname, sizeof(modname));
    CONTEXT* c = ep->ContextRecord;
    fprintf(standalone_log, "  in %s+0x%x\n  eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx", modname,
            pc - (uint32_t)pcmod, c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp);
    fprintf(standalone_log, "\n  stack code refs:");
    // Candidate return addresses into our image (resolve with nm -n) or into retail's code range.
    for (int i = 0, n = 0; i < 2048 && n < 24; i++) {
        if (IsBadReadPtr(esp + i, 4))
            break;
        uint32_t v = esp[i];
        if ((v >= 0x10001000u && v < 0x10200000u) || (v >= RETAIL_TEXT_LO && v < RETAIL_TEXT_HI)) {
            fprintf(standalone_log, " %08x", v);
            n++;
        }
    }
    fprintf(standalone_log, "\n");
    fflush(standalone_log);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Fill retail's import address table the way the loader would for SWEP1RCR.EXE.
static void resolve_imports(const IMAGE_NT_HEADERS32* nt)
{
    const IMAGE_DATA_DIRECTORY* dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress)
        return;
    IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(RETAIL_BASE + dir->VirtualAddress);
    for (; d->Name; d++) {
        const char* dll = (const char*)(RETAIL_BASE + d->Name);
        HMODULE mod = LoadLibraryA(dll);
        fprintf(standalone_log, "import %-14s -> %p\n", dll, (void*)mod);
        uint32_t* thunk = (uint32_t*)(RETAIL_BASE + d->FirstThunk);
        uint32_t* names = (uint32_t*)(RETAIL_BASE + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        for (; *names; names++, thunk++) {
            FARPROC fn = NULL;
            const char* what;
            char ord[16];
            if (*names & IMAGE_ORDINAL_FLAG32) {
                snprintf(ord, sizeof(ord), "#%u", (unsigned)(*names & 0xffff));
                what = ord;
                fn = mod ? GetProcAddress(mod, (LPCSTR)(uintptr_t)(*names & 0xffff)) : NULL;
            } else {
                what = (const char*)(RETAIL_BASE + *names + 2);
                fn = mod ? GetProcAddress(mod, what) : NULL;
            }
            if (!fn)
                fprintf(standalone_log, "  unresolved %s!%s\n", dll, what);
            *thunk = (uint32_t)fn;
        }
    }
}

static int commit_retail_range(void)
{
    // Reserved for us by the parent (see relaunch_with_reserved_range), or still free.
    if (VirtualAlloc((void*)RETAIL_BASE, RETAIL_SPAN, MEM_COMMIT, PAGE_EXECUTE_READWRITE) == (void*)RETAIL_BASE)
        return 1;
    return VirtualAlloc((void*)RETAIL_BASE, RETAIL_SPAN, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE) == (void*)RETAIL_BASE;
}

// Windows places the process heap at 0x400000 when no image sits there, before any of our code
// runs. So run the game in a child created suspended, whose retail range we reserve before its
// loader has initialised anything.
static void relaunch_with_reserved_range(void)
{
    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, MAX_PATH);
    SetEnvironmentVariableA("SWR_STANDALONE_CHILD", "1");
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(self, GetCommandLineA(), NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi))
        die("Cannot start %s", self);
    if (VirtualAllocEx(pi.hProcess, (void*)RETAIL_BASE, RETAIL_SPAN, MEM_RESERVE, PAGE_NOACCESS) != (void*)RETAIL_BASE) {
        standalone_log = fopen("standalone_launcher.log", "w");
        fprintf(standalone_log, "VirtualAllocEx error %lu\n", GetLastError());
        for (uint32_t a = 0x10000; a < 0x01000000;) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQueryEx(pi.hProcess, (void*)a, &mbi, sizeof(mbi)))
                break;
            if (mbi.State != MEM_FREE)
                fprintf(standalone_log, "%08x alloc %p size %08lx state %lx type %lx\n", a, mbi.AllocationBase, (unsigned long)mbi.RegionSize,
                        mbi.State, mbi.Type);
            a += mbi.RegionSize;
        }
        TerminateProcess(pi.hProcess, 1);
        die("Cannot reserve 0x400000 in %s", self);
    }
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    ExitProcess(code);
}

static void map_retail(void)
{
    char exe[MAX_PATH];
    GetCurrentDirectoryA(sizeof(exe) - 16, exe);
    strcat(exe, "\\SWEP1RCR.EXE");

    DWORD size = 0;
    uint8_t* file = read_file(exe, &size);
    if (!file)
        die("Cannot read %s. Run from the game directory.", exe);

    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)file;
    const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(file + dos->e_lfanew);
    if (nt->OptionalHeader.ImageBase != RETAIL_BASE)
        die("%s has an unexpected image base", exe);
    retail_image_size = nt->OptionalHeader.SizeOfImage;

    if (retail_image_size > RETAIL_SPAN)
        die("%s is larger than expected", exe);
    uint8_t* base = (uint8_t*)RETAIL_BASE;
    if (!commit_retail_range()) {
        MEMORY_BASIC_INFORMATION mbi;
        VirtualQuery((void*)RETAIL_BASE, &mbi, sizeof(mbi));
        fprintf(standalone_log, "0x400000 region: base %p alloc %p state 0x%lx size 0x%lx\n", mbi.BaseAddress, mbi.AllocationBase,
                mbi.State, (unsigned long)mbi.RegionSize);
        die("Address range 0x400000 is already in use (%s)", "see standalone.log");
    }

    memcpy(base, file, nt->OptionalHeader.SizeOfHeaders);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        char name[9] = { 0 };
        memcpy(name, s->Name, 8);
        uint8_t* dst = base + s->VirtualAddress;
        if (s->VirtualAddress + RETAIL_BASE == RETAIL_TEXT_LO) {
            // Encrypted by SteamStub on disk, and never executed here anyway.
            memset(dst, 0xcc, s->Misc.VirtualSize);
        } else if (!strcmp(name, ".bind")) {
            continue;
        } else {
            uint32_t n = s->SizeOfRawData < s->Misc.VirtualSize ? s->SizeOfRawData : s->Misc.VirtualSize;
            memcpy(dst, file + s->PointerToRawData, n);
        }
        fprintf(standalone_log, "section %-8s 0x%08lx +0x%06lx\n", name, (unsigned long)(RETAIL_BASE + s->VirtualAddress),
                (unsigned long)s->Misc.VirtualSize);
    }
    // dinput.lib's standard data formats were linked into retail .text (stdControl_df* in globals.h).
    memcpy((void*)0x0049e690, &c_dfDIKeyboard, sizeof(DIDATAFORMAT));
    memcpy((void*)0x0049e720, &c_dfDIMouse, sizeof(DIDATAFORMAT));
    memcpy((void*)0x0049d670, &c_dfDIJoystick, sizeof(DIDATAFORMAT));
    resolve_imports((const IMAGE_NT_HEADERS32*)(base + dos->e_lfanew));
    HeapFree(GetProcessHeap(), 0, file);
}

void standalone_write_jmp(uint32_t retail_addr, uint32_t target)
{
    uint8_t* p = (uint8_t*)retail_addr;
    p[0] = 0xe9;
    *(uint32_t*)(p + 1) = target - retail_addr - 5;
}

// Runs before the CRT calls WinMain, as early as a constructor can, so the 0x400000 range is still free.
__attribute__((constructor(101))) static void standalone_init(void)
{
    if (!GetEnvironmentVariableA("SWR_STANDALONE_CHILD", NULL, 0))
        relaunch_with_reserved_range();

    standalone_log = fopen("standalone.log", "w");
    hook_log = standalone_log;
    fprintf(standalone_log, "[SWE1R standalone]\n");
    AddVectoredExceptionHandler(1, retail_trap_handler);

    // Retail's CRT runs the x87 unit at 53-bit precision; mingw's default is 64-bit.
    _controlfp(_PC_53, _MCW_PC);

    // Search the game directory for its own DLLs (Smush, IFORCE2, a3d), as retail would, but take
    // the system DirectInput: the game directory's dinput.dll is the hook DLL.
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH - 16);
    strcat(sys, "\\dinput.dll");
    LoadLibraryA(sys);
    char cwd[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, cwd);
    SetDllDirectoryA(cwd);

    map_retail();
    standalone_install_bodies();
    standalone_install_compat();
    fflush(standalone_log);
}
