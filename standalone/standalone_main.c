// Standalone launcher: runs the game entirely from the src/ reimplementation, without executing
// any retail code. Retail's initial data is taken from the player's own SWEP1RCR.EXE at startup.
//
// Default build (SWR_DATA_RELOC + SWR_GLOBALS_OWNED): every named global has storage of its own,
// unnamed data lives in swr_data_image, data pointers are relocated, and code pointers in data are
// pointed at our bodies. Nothing sits at a retail address, so the EXE is an ordinary relocatable one.
//
// The feature build (its deltas call game functions through *_ADDR) and retail bisecting still map
// retail's code range: int3 everywhere, a jmp to our body at each function's retail entry, and with
// SWR_STANDALONE_RETAIL_TEXT retail's own code for the functions listed in SWR_STANDALONE_RETAIL
// (that needs -DSWR_STANDALONE_DATA_RELOC=OFF, which keeps retail's data at its retail address).

#include <windows.h>
#define DIRECTINPUT_VERSION 0x0500
#include <dinput.h>
#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "standalone.h"
#include "swr_data_addr.h"

#define RETAIL_BASE 0x00400000u
#define RETAIL_TEXT_LO 0x00401000u
#define RETAIL_TEXT_HI 0x004ac000u

// SizeOfImage of SWEP1RCR.EXE, .bind included.
#define RETAIL_SPAN 0x00af3000u

#ifdef SWR_DATA_RELOC
// Retail's .rdata/.data/.bss image, at our own address. globals.h reaches it through SWR_GLOBAL_PTR.
__attribute__((aligned(4096))) char swr_data_image[SWR_DATA_HI - SWR_DATA_LO];
extern const uint32_t standalone_data_relocs[];
extern const int standalone_data_reloc_count;
#endif

// Where a retail virtual address lives in the data image (sections and imports are copied there whole).
static uint8_t* retail_va(uint32_t va)
{
    return (uint8_t*)SWR_IMAGE_PTR(va);
}

#ifdef SWR_GLOBALS_OWNED
#include "standalone_owned.h"

// Wherever retail data address a lives now: the owned variable that covers it, else the image.
void* swr_data_ptr(unsigned a)
{
    int lo = 0, hi = standalone_owned_global_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const StandaloneOwnedGlobal* g = &standalone_owned_globals[mid];
        if (a < g->retail)
            hi = mid - 1;
        else if (a >= g->retail + g->size)
            lo = mid + 1;
        else
            return (char*)g->ours + (a - g->retail);
    }
    return SWR_IMAGE_PTR(a);
}

#define OWNED_POISON 0xd5

// Guarded diagnostic build: each block ends right before a no-access page. A read or write past a
// block's footprint faults there; the handler names the block, opens the page and carries on, so
// one run lists every global whose real extent is larger than data_symbols says.
static uint8_t owned_dummy_canary[64];
static uint32_t* owned_guard_pages;
static uint8_t* owned_guard_reported;

static LONG CALLBACK owned_guard_handler(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    uint32_t addr = (uint32_t)ep->ExceptionRecord->ExceptionInformation[1];
    for (int i = 0; i < standalone_owned_global_count; i++) {
        if (addr < owned_guard_pages[i] || addr >= owned_guard_pages[i] + 0x1000)
            continue;
        const StandaloneOwnedGlobal* g = &standalone_owned_globals[i];
        uint32_t past = addr - ((uint32_t)g->ours + g->size);
        if (!owned_guard_reported[i]++)
            fprintf(standalone_log, "GUARD %s past %s (0x%08x, size 0x%x) by 0x%x, eip %08lx\n",
                    ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read", g->name, g->retail, g->size, past + 1,
                    ep->ContextRecord->Eip);
        fflush(standalone_log);
        DWORD old;
        VirtualProtect((void*)owned_guard_pages[i], 0x1000, PAGE_READWRITE, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void owned_allocate_guarded(void)
{
    owned_guard_pages = (uint32_t*)calloc(standalone_owned_global_count, sizeof(uint32_t));
    owned_guard_reported = (uint8_t*)calloc(standalone_owned_global_count, 1);
    for (int i = 0; i < standalone_owned_global_count; i++) {
        StandaloneOwnedGlobal* g = &standalone_owned_globals[i];
        uint32_t pages = (g->size + 0xfff) / 0x1000;
        uint8_t* base = (uint8_t*)VirtualAlloc(NULL, (pages + 1) * 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        DWORD old;
        VirtualProtect(base + pages * 0x1000, 0x1000, PAGE_NOACCESS, &old);
        g->ours = base + pages * 0x1000 - g->size;
        g->canary = owned_dummy_canary;
        swr_owned_ptr[i] = (unsigned char*)g->ours;
        owned_guard_pages[i] = (uint32_t)(base + pages * 0x1000);
    }
    AddVectoredExceptionHandler(1, owned_guard_handler);
    fprintf(standalone_log, "owned globals: guarded layout, one guard page after each of %d blocks\n", standalone_owned_global_count);
}

static void report_stale_at_exit(void)
{
    standalone_owned_report_stale(standalone_log);
}

void standalone_owned_report_stale(FILE* log)
{
    int stale = 0, overrun = 0;
    for (int i = 0; i < standalone_owned_global_count; i++) {
        const StandaloneOwnedGlobal* g = &standalone_owned_globals[i];
        const uint8_t* p = retail_va(g->retail);
        for (uint32_t k = 0; g->retail >= SWR_DATA_LO && k < g->size; k++) {
            if (p[k] != OWNED_POISON) {
                fprintf(log, "  stale write: %s+0x%x (0x%08x, size 0x%x)\n", g->name, k, g->retail, g->size);
                stale++;
                break;
            }
        }
        for (int k = 0; k < standalone_owned_canary_size; k++) {
            if (g->canary[k] != OWNED_POISON) {
                fprintf(log, "  overrun: past %s (0x%08x, size 0x%x) by at least 0x%x\n", g->name, g->retail, g->size, k + 1);
                overrun++;
                break;
            }
        }
    }
    fprintf(log, "  %d blocks written through their retail address, %d blocks overrun\n", stale, overrun);
    fflush(log);
}
#endif

FILE* standalone_log = NULL;

// Whether v points into this EXE's own code (where the image actually loaded).
static int in_our_code(uint32_t v)
{
    static uint32_t lo, hi;
    if (!lo) {
        const uint8_t* base = (const uint8_t*)GetModuleHandleA(NULL);
        const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
        lo = (uint32_t)base + nt->OptionalHeader.BaseOfCode;
        hi = lo + nt->OptionalHeader.SizeOfCode;
    }
    return v >= lo && v < hi;
}
FILE* hook_log = NULL;
static uint32_t retail_image_size = 0;
static int retail_text_loaded = 0;

// The plain standalone runs without retail's address range: no code there, data in its own
// storage. The feature build's deltas still call game functions through *_ADDR constants, and
// retail bisecting needs retail's code at its real address, so those map the range.
int standalone_code_range_mapped(void)
{
#if defined(SWR_STANDALONE_FEATURES) || !defined(SWR_DATA_RELOC)
    return 1; // without SWR_DATA_RELOC retail's data itself sits in the range
#else
    static int mapped = -1;
    if (mapped < 0)
        mapped = GetEnvironmentVariableA("SWR_STANDALONE_RETAIL_TEXT", NULL, 0) || GetEnvironmentVariableA("SWR_STANDALONE_CODE_RANGE", NULL, 0);
    return mapped;
#endif
}

int standalone_retail_text_loaded(void)
{
    return retail_text_loaded;
}

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

// Debugging aid: a hardware write watchpoint (DR0, 4 bytes) on the calling thread. Each write logs
// the writing instruction and the stack's code references.
static LONG CALLBACK watch_write_handler(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(ep->ContextRecord->Dr6 & 1))
        return EXCEPTION_CONTINUE_SEARCH;
    uint32_t* esp = (uint32_t*)ep->ContextRecord->Esp;
    fprintf(standalone_log, "WATCH write at eip %08lx, value now %08x, stack:", ep->ContextRecord->Eip,
            *(uint32_t*)ep->ContextRecord->Dr0);
    for (int i = 0, n = 0; i < 512 && n < 10; i++)
        if (in_our_code(esp[i])) {
            fprintf(standalone_log, " %08x", esp[i]);
            n++;
        }
    fprintf(standalone_log, "\n");
    fflush(standalone_log);
    ep->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static HANDLE watch_target;
static DWORD watch_addr;

// SetThreadContext does not apply debug registers to the calling thread, so a helper suspends it.
static DWORD WINAPI watch_arm_thread(LPVOID unused)
{
    (void)unused;
    SuspendThread(watch_target);
    CONTEXT c = { 0 };
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    c.Dr0 = watch_addr;
    c.Dr7 = 1 | (1u << 16) | (3u << 18); // L0, break on write, 4 bytes
    SetThreadContext(watch_target, &c);
    ResumeThread(watch_target);
    return 0;
}

void standalone_watch_write(void* addr)
{
    AddVectoredExceptionHandler(1, watch_write_handler);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &watch_target, 0, FALSE, DUPLICATE_SAME_ACCESS);
    watch_addr = (DWORD)addr;
    HANDLE h = CreateThread(NULL, 0, watch_arm_thread, NULL, 0, NULL);
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    fprintf(standalone_log, "WATCH armed on %p\n", addr);
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
        if (in_our_code(v) || (v >= RETAIL_TEXT_LO && v < RETAIL_TEXT_HI)) {
            fprintf(standalone_log, " %08x", v);
            n++;
        }
    }
    fprintf(standalone_log, "\n");
#ifdef SWR_GLOBALS_OWNED
    static int stale_reported = 0;
    if (!stale_reported++)
        standalone_owned_report_stale(standalone_log);
#endif
    fflush(standalone_log);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Fill retail's import address table the way the loader would for SWEP1RCR.EXE.
static void resolve_imports(const IMAGE_NT_HEADERS32* nt)
{
    const IMAGE_DATA_DIRECTORY* dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress)
        return;
    IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)retail_va(RETAIL_BASE + dir->VirtualAddress);
    for (; d->Name; d++) {
        const char* dll = (const char*)retail_va(RETAIL_BASE + d->Name);
        HMODULE mod = LoadLibraryA(dll);
        fprintf(standalone_log, "import %-14s -> %p\n", dll, (void*)mod);
        uint32_t* thunk = (uint32_t*)retail_va(RETAIL_BASE + d->FirstThunk);
        uint32_t* names = (uint32_t*)retail_va(RETAIL_BASE + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        for (; *names; names++, thunk++) {
            FARPROC fn = NULL;
            const char* what;
            char ord[16];
            if (*names & IMAGE_ORDINAL_FLAG32) {
                snprintf(ord, sizeof(ord), "#%u", (unsigned)(*names & 0xffff));
                what = ord;
                fn = mod ? GetProcAddress(mod, (LPCSTR)(uintptr_t)(*names & 0xffff)) : NULL;
            } else {
                what = (const char*)retail_va(RETAIL_BASE + *names + 2);
                fn = mod ? GetProcAddress(mod, what) : NULL;
            }
            if (!fn)
                fprintf(standalone_log, "  unresolved %s!%s\n", dll, what);
            *thunk = (uint32_t)fn;
        }
    }
}

#ifdef SWR_DATA_RELOC
// Only the header page and the code range: the data range stays reserved and inaccessible, so any
// pointer into it that missed relocation faults instead of reading stale memory.
#define RETAIL_COMMIT_SPAN (RETAIL_TEXT_HI - RETAIL_BASE)
#else
#define RETAIL_COMMIT_SPAN RETAIL_SPAN
#endif

static int commit_retail_range(void)
{
    // Reserved for us by the parent (see relaunch_with_reserved_range), or still free.
    if (VirtualAlloc((void*)RETAIL_BASE, RETAIL_COMMIT_SPAN, MEM_COMMIT, PAGE_EXECUTE_READWRITE) == (void*)RETAIL_BASE)
        return 1;
    return VirtualAlloc((void*)RETAIL_BASE, RETAIL_SPAN, MEM_RESERVE, PAGE_NOACCESS) == (void*)RETAIL_BASE &&
        VirtualAlloc((void*)RETAIL_BASE, RETAIL_COMMIT_SPAN, MEM_COMMIT, PAGE_EXECUTE_READWRITE) == (void*)RETAIL_BASE;
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
    const int code_range = standalone_code_range_mapped();
    if (code_range && !commit_retail_range()) {
        MEMORY_BASIC_INFORMATION mbi;
        VirtualQuery((void*)RETAIL_BASE, &mbi, sizeof(mbi));
        fprintf(standalone_log, "0x400000 region: base %p alloc %p state 0x%lx size 0x%lx\n", mbi.BaseAddress, mbi.AllocationBase,
                mbi.State, (unsigned long)mbi.RegionSize);
        die("Address range 0x400000 is already in use (%s)", "see standalone.log");
    }

    if (code_range)
        memcpy(base, file, nt->OptionalHeader.SizeOfHeaders);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        char name[9] = { 0 };
        memcpy(name, s->Name, 8);
        uint8_t* dst = retail_va(RETAIL_BASE + s->VirtualAddress);
#ifdef SWR_DATA_RELOC
        if (RETAIL_BASE + s->VirtualAddress >= SWR_DATA_HI) {
            fprintf(standalone_log, "section %-8s not loaded\n", name);
            continue;
        }
#endif
        if (s->VirtualAddress + RETAIL_BASE == RETAIL_TEXT_LO && !code_range) {
            fprintf(standalone_log, "section %-8s not mapped (no retail code range)\n", name);
            continue;
        }
        if (s->VirtualAddress + RETAIL_BASE == RETAIL_TEXT_LO) {
            // Encrypted by SteamStub on disk. Traps by default; SWR_STANDALONE_RETAIL_TEXT names a
            // decrypted image of the running game (base 0x400000) to take retail's code from, so the
            // functions in SWR_STANDALONE_RETAIL can run retail's bodies (see standalone_hook.c).
            memset(dst, 0xcc, s->Misc.VirtualSize);
            char imgPath[MAX_PATH];
#ifdef SWR_DATA_RELOC
            // Retail's code addresses retail's data directly, which this build keeps elsewhere.
            if (GetEnvironmentVariableA("SWR_STANDALONE_RETAIL_TEXT", NULL, 0))
                die("Running retail bodies needs a build with %s.", "-DSWR_STANDALONE_DATA_RELOC=OFF");
#endif
            if (GetEnvironmentVariableA("SWR_STANDALONE_RETAIL_TEXT", imgPath, sizeof(imgPath))) {
                DWORD imgSize = 0;
                uint8_t* img = read_file(imgPath, &imgSize);
                if (img && imgSize >= s->VirtualAddress + s->Misc.VirtualSize) {
                    memcpy(dst, img + s->VirtualAddress, s->Misc.VirtualSize);
                    retail_text_loaded = 1;
                    fprintf(standalone_log, "retail .text loaded from %s\n", imgPath);
                } else {
                    fprintf(standalone_log, "cannot use retail image %s\n", imgPath);
                }
                if (img)
                    HeapFree(GetProcessHeap(), 0, img);
            }
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
    memcpy(SWR_GLOBAL_PTR(0x0049e690), &c_dfDIKeyboard, sizeof(DIDATAFORMAT));
    memcpy(SWR_GLOBAL_PTR(0x0049e720), &c_dfDIMouse, sizeof(DIDATAFORMAT));
    memcpy(SWR_GLOBAL_PTR(0x0049d670), &c_dfDIJoystick, sizeof(DIDATAFORMAT));
    resolve_imports(nt);
    // After the imports: some named globals are import slots.
#ifdef SWR_DATA_RELOC
#ifdef SWR_GLOBALS_OWNED
    // Owned globals start from retail's initial bytes, then every data pointer, wherever its slot
    // lives, is pointed at wherever its target lives.
    if (standalone_owned_guarded)
        owned_allocate_guarded();
    for (int i = 0; i < standalone_owned_global_count; i++) {
        const StandaloneOwnedGlobal* g = &standalone_owned_globals[i];
        if (g->retail >= SWR_DATA_LO)
            memcpy(g->ours, retail_va(g->retail), g->size);
    }
    int moved = 0;
    for (int i = 0; i < standalone_data_reloc_count; i++) {
        uint32_t* slot = (uint32_t*)swr_data_ptr(standalone_data_relocs[i]);
        if (*slot >= SWR_DATA_LO && *slot < SWR_DATA_HI) {
            *slot = (uint32_t)swr_data_ptr(*slot);
            moved++;
        }
    }
    fprintf(standalone_log, "data image at %p, %d owned globals: %d of %d pointers relocated\n", (void*)swr_data_image,
            standalone_owned_global_count, moved, standalone_data_reloc_count);
    // Poison the image copies of owned globals: anything that later writes one reached the global
    // through a retail neighbour's address (see standalone_owned_report_stale).
    for (int i = 0; i < standalone_owned_global_count; i++) {
        const StandaloneOwnedGlobal* g = &standalone_owned_globals[i];
        if (g->retail >= SWR_DATA_LO)
            memset(retail_va(g->retail), OWNED_POISON, g->size);
        memset(g->canary, OWNED_POISON, standalone_owned_canary_size);
    }
    atexit(report_stale_at_exit);
#else
    // Pointers inside the image still hold retail addresses; move them with the image.
    const uint32_t delta = (uint32_t)swr_data_image - SWR_DATA_LO;
    int moved = 0;
    for (int i = 0; i < standalone_data_reloc_count; i++) {
        uint32_t* slot = (uint32_t*)retail_va(standalone_data_relocs[i]);
        if (*slot >= SWR_DATA_LO && *slot < SWR_DATA_HI) {
            *slot += delta;
            moved++;
        }
    }
    fprintf(standalone_log, "data image at %p: %d of %d pointers relocated\n", (void*)swr_data_image, moved, standalone_data_reloc_count);
#endif
#endif
    HeapFree(GetProcessHeap(), 0, file);
}

void standalone_write_jmp(uint32_t retail_addr, uint32_t target)
{
    uint8_t* p = (uint8_t*)retail_addr;
    p[0] = 0xe9;
    *(uint32_t*)(p + 1) = target - retail_addr - 5;
}

static HANDLE main_thread = NULL;
void DirectDraw_FillMainSurface(void);
void stdPlatform_noop(uint32_t x);

struct std3D_DrawProbe {
    int calls, dropped, failed, verts, maxBatch;
    HRESULT lastErr;
    float zMin, zMax, rhwMin, rhwMax, xMin, xMax, yMin, yMax;
    unsigned int alphaMin, alphaMax, rgbMax, texNull, fullQuads, fullColor;
};
extern struct std3D_DrawProbe std3D_drawProbe;

static void log_draw_probe(void)
{
    struct std3D_DrawProbe p = std3D_drawProbe;
    memset(&std3D_drawProbe, 0, sizeof(std3D_drawProbe));
    fprintf(standalone_log, "draw: maxVerts %u calls %d dropped %d failed %d (last %08lx) maxBatch %d | 3D verts %d x %.0f..%.0f y %.0f..%.0f z %.4f..%.4f rhw %.4f..%.4f a %u..%u rgbMax %u texNull %u fullQuads %u color %08x\n",
            *(unsigned int*)SWR_GLOBAL_PTR(0x0052e624), p.calls, p.dropped, p.failed, (unsigned long)p.lastErr, p.maxBatch, p.verts, p.xMin, p.xMax, p.yMin, p.yMax,
            p.zMin, p.zMax, p.rhwMin, p.rhwMax, p.alphaMin, p.alphaMax, p.rgbMax, p.texNull, p.fullQuads, p.fullColor);
}

// SWR_STANDALONE_WATCH=1: log a few globals every 2 s (debugging aid for the prototype).
static DWORD WINAPI watch_thread(LPVOID unused)
{
    (void)unused;
    for (;;) {
        Sleep(2000);
        log_draw_probe();
        fprintf(standalone_log, "sound: A3Dinitted %d IA3d4 %08x streamSource %08x mapSounds %d layeredStream %d\n", *(int*)SWR_GLOBAL_PTR(0x004c7d80),
                *(uint32_t*)SWR_GLOBAL_PTR(0x0050d548), *(uint32_t*)SWR_GLOBAL_PTR(0x004eb414), *(int*)SWR_GLOBAL_PTR(0x00e9f360), *(int*)SWR_GLOBAL_PTR(0x0050c69c));
        {
            // every 'Test' (pod) object: position, speed, flags0
            uint32_t** it = *(uint32_t***)SWR_GLOBAL_PTR(0x004bfec0); // eventManagerMain
            for (; it && *it; it++) {
                uint32_t* m = *it;
                if (m[0] == 'Jdge' && m[2] > 0) {
                    uint8_t* j = (uint8_t*)m[4];
                    fprintf(standalone_log, "jdge: flag %08x raceTimer %.3f\n", *(uint32_t*)(j + 8), (double)*(float*)(j + 0xc));
                }
                if (m[0] != 'Test')
                    continue;
                fprintf(standalone_log, "pods: count %u f0 %08x f1 %08x f2 %08x f3 %08x f4 %08x |", m[2], m[5], m[6], m[7], m[8], m[9]);
                for (uint32_t i = 0; i < m[2] && i < 20; i++) {
                    uint8_t* pod = (uint8_t*)m[4] + m[3] * i;
                    fprintf(standalone_log, " [%u] %.0f,%.0f f0 %x spd %.0f", i, *(float*)(pod + 0x50), *(float*)(pod + 0x54), *(uint32_t*)(pod + 0x60),
                            *(float*)(pod + 0x1a0));
                }
                fprintf(standalone_log, "\n");
            }
        }
        {
            uint8_t* pod = *(uint8_t**)SWR_GLOBAL_PTR(0x004d78a8); // currentPlayer_Test
            if (pod && !IsBadReadPtr(pod, 0x200))
                fprintf(standalone_log, "pod: %p pos %.2f %.2f %.2f flags0 %08x flags1 %08x throttle %.3f speed %.3f\n", pod, *(float*)(pod + 0x50),
                        *(float*)(pod + 0x54), *(float*)(pod + 0x58), *(uint32_t*)(pod + 0x60), *(uint32_t*)(pod + 0x64), *(float*)(pod + 0x18c), *(float*)(pod + 0x1a0));
        }
        {
            LPDIRECTINPUTDEVICEA kbd = *(LPDIRECTINPUTDEVICEA*)SWR_GLOBAL_PTR(0x0050d89c);
            char state[256];
            HRESULT hr = kbd ? IDirectInputDevice_GetDeviceState(kbd, sizeof(state), state) : E_POINTER;
            int down = 0;
            for (int k = 0; hr == DI_OK && k < 256; k++)
                down += state[k] != 0;
            fprintf(standalone_log, "watch: kbd GetDeviceState hr %08lx keys down %d\n", (unsigned long)hr, down);
        }
        if (main_thread) {
            CONTEXT c;
            c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT;
            SuspendThread(main_thread);
            if (GetThreadContext(main_thread, &c)) {
                fprintf(standalone_log, "fpu: main thread x87 control word %04lx (PC %s)\n", c.FloatSave.ControlWord & 0xffff, ((c.FloatSave.ControlWord >> 8) & 3) == 0 ? "24-bit" : ((c.FloatSave.ControlWord >> 8) & 3) == 2 ? "53-bit" : "64-bit");
                fprintf(standalone_log, "watch: screen %dx%d backbuf %dx%d full %d zfill %08x | ", *(int*)SWR_GLOBAL_PTR(0x00ec86c4), *(int*)SWR_GLOBAL_PTR(0x00ec85e8), *(int*)SWR_GLOBAL_PTR(0x00ec8da0 + 0xc), *(int*)SWR_GLOBAL_PTR(0x00ec8da0 + 0x10), *(int*)SWR_GLOBAL_PTR(0x0052d458), *(int*)SWR_GLOBAL_PTR(0x004c86c0));
                {
                    const float* v = (const float*)SWR_GLOBAL_PTR(0x00b6b0e8);
                    for (int k = 0; k < 3; k++, v += 8)
                        fprintf(standalone_log, "v%d (%.1f %.1f z%.4f rhw%.4f) col %08x spec %08x uv %.3f %.3f | ", k, v[0], v[1], v[2], v[3],
                                ((const uint32_t*)v)[4], ((const uint32_t*)v)[5], v[6], v[7]);
                }
                fprintf(standalone_log, "watch: rdCache drawnFaces %d frame %d procFaces %d hwVerts %d | ", *(int*)SWR_GLOBAL_PTR(0x00deb0e8), *(int*)SWR_GLOBAL_PTR(0x00deb0ec), *(int*)SWR_GLOBAL_PTR(0x006930c8), *(int*)SWR_GLOBAL_PTR(0x00a530d0));
                fprintf(standalone_log, "watch: cooldown %f loaded %d dt %f | ", (double)*(float*)SWR_GLOBAL_PTR(0x004d5588), *(int*)SWR_GLOBAL_PTR(0x004b6c90), *(double*)SWR_GLOBAL_PTR(0x00e22a40));
                fprintf(standalone_log, "watch: enterHeld %d uiActive %d readtime %d main eip %08lx stack:", ((int*)SWR_GLOBAL_PTR(0x0050e868))[0x1c], *(int*)SWR_GLOBAL_PTR(0x004b2940), *(int*)SWR_GLOBAL_PTR(0x0050fecc), c.Eip);
                uint32_t* sp = (uint32_t*)c.Esp;
                for (int i = 0, n = 0; i < 4096 && n < 16 && !IsBadReadPtr(sp + i, 4); i++)
                    if (in_our_code(sp[i])) {
                        fprintf(standalone_log, " %08x", sp[i]);
                        n++;
                    }
                fprintf(standalone_log, "\n");
            }
            ResumeThread(main_thread);
        }
        fprintf(standalone_log, "watch: startup %d open %d active %d enterN %d escN %d | ", *(int*)SWR_GLOBAL_PTR(0x0050fea8), *(int*)SWR_GLOBAL_PTR(0x0050feac), *(int*)SWR_GLOBAL_PTR(0x0050feb4), ((int*)SWR_GLOBAL_PTR(0x0050f668))[0x1c], ((int*)SWR_GLOBAL_PTR(0x0050f668))[0x01]);
        fprintf(standalone_log, "watch: kbd dev %08x nkbd %d mouse dev %08x skip %d movies %d fg %d\n", *(uint32_t*)SWR_GLOBAL_PTR(0x0050d89c),
                *(int*)SWR_GLOBAL_PTR(0x0050febc), *(uint32_t*)SWR_GLOBAL_PTR(0x00ecb484), *(int*)SWR_GLOBAL_PTR(0x0050b5b8), *(int*)SWR_GLOBAL_PTR(0x0050b5c4),
                GetForegroundWindow() != NULL && GetWindowThreadProcessId(GetForegroundWindow(), NULL) != 0
                    && ({ DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid); pid == GetCurrentProcessId(); }));
        fflush(standalone_log);
    }
    return 0;
}

// stdDisplay_CreateZBuffer only installs the per-frame z-buffer clear (DirectDraw_FillMainSurface)
// when the device reports DDCAPS2 bit 0x10000000; DDrawCompat and modern drivers don't, so the
// z-buffer is never cleared and only z = 0 geometry (the 2D UI) passes the depth test. Retail on
// a modern system has the same problem. Re-point the fill hook whenever the game sets the no-op.
// SWR_STANDALONE_NO_ZCLEAR=1 keeps retail's behaviour.
static DWORD WINAPI zclear_thread(LPVOID unused)
{
    void** fill = (void**)SWR_GLOBAL_PTR(0x004c86b8); // stdDisplay_FillMainSurface_ptr
    (void)unused;
    for (;;) {
        if (*fill == (void*)stdPlatform_noop) {
            *fill = (void*)DirectDraw_FillMainSurface;
            fprintf(standalone_log, "zclear: installed per-frame z-buffer clear\n");
            fflush(standalone_log);
        }
        Sleep(50);
    }
    return 0;
}

// Retail data that points at retail code (object callback tables and the like) is pointed at our
// bodies, so no code pointer depends on the retail code range.
extern const uint32_t standalone_code_relocs[];
extern const int standalone_code_reloc_count;

static void relocate_code_pointers(void)
{
    int moved = 0, unknown = 0;
    for (int i = 0; i < standalone_code_reloc_count; i++) {
        uint32_t* slot = (uint32_t*)SWR_GLOBAL_PTR(standalone_code_relocs[i]);
        uint32_t v = *slot;
        if (v < RETAIL_TEXT_LO || v >= RETAIL_TEXT_HI)
            continue;
        uint32_t body = standalone_body_of(v);
        void* owned = SWR_GLOBAL_PTR(v);
        if (body) {
            *slot = body;
            moved++;
        } else if ((uint32_t)owned != v) {
            *slot = (uint32_t)owned;
            moved++;
        } else if (unknown++ < 16) {
            fprintf(standalone_log, "code pointer at 0x%08x -> 0x%08x has no body\n", standalone_code_relocs[i], v);
        }
    }
    fprintf(standalone_log, "code pointers: %d moved to our bodies, %d left (no body)\n", moved, unknown);
}

// Runs before the CRT calls WinMain, as early as a constructor can, so the 0x400000 range is still free.
__attribute__((constructor(101))) static void standalone_init(void)
{
    if (standalone_code_range_mapped() && !GetEnvironmentVariableA("SWR_STANDALONE_CHILD", NULL, 0))
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
    relocate_code_pointers();
    standalone_install_compat();
    if (!GetEnvironmentVariableA("SWR_STANDALONE_NO_ZCLEAR", NULL, 0))
        CreateThread(NULL, 0, zclear_thread, NULL, 0, NULL);
    if (GetEnvironmentVariableA("SWR_STANDALONE_WATCH", NULL, 0)) {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &main_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
        CreateThread(NULL, 0, watch_thread, NULL, 0, NULL);
    }
    fflush(standalone_log);
}
