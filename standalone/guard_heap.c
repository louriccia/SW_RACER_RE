// SWR_GUARD_HEAP=1: every malloc/calloc/realloc/free in this image (linked with --wrap) gets its own
// pages, the block ending right against an inaccessible page, so a write past the end faults at the
// writing instruction instead of corrupting the CRT heap. A diagnostic; memory hungry.

#include <windows.h>
#include <stdint.h>
#include <string.h>

void* __real_malloc(size_t size);
void* __real_calloc(size_t n, size_t size);
void* __real_realloc(void* p, size_t size);
void __real_free(void* p);

#define GUARD_RESERVE ((size_t) 1 << 30)// 1 GiB of address space, reserved below 2 GiB
#define GUARD_PAGE 4096

static int g_state;// 0 unknown, 1 off, 2 on
static uint8_t* g_base;
static uint8_t* g_next;
static SRWLOCK g_lock = SRWLOCK_INIT;

static int guard_on(void)
{
    if (g_state == 0) {
        g_state = 1;
        if (GetEnvironmentVariableA("SWR_GUARD_HEAP", NULL, 0)) {
            for (uintptr_t at = 0x20000000; at < 0x60000000 && !g_base; at += 0x10000000)
                g_base = (uint8_t*) VirtualAlloc((void*) at, GUARD_RESERVE, MEM_RESERVE, PAGE_NOACCESS);
            if (g_base) {
                g_next = g_base;
                g_state = 2;
            }
        }
    }
    return g_state == 2;
}

static int is_guarded(void* p)
{
    return g_base && (uint8_t*) p >= g_base && (uint8_t*) p < g_base + GUARD_RESERVE;
}

// The block's size lives in the 8 bytes before it (inside the committed span).
static void* guard_alloc(size_t size)
{
    size_t span = (size + 8 + GUARD_PAGE - 1) & ~(size_t) (GUARD_PAGE - 1);
    AcquireSRWLockExclusive(&g_lock);
    uint8_t* at = g_next;
    if (at + span + GUARD_PAGE > g_base + GUARD_RESERVE) {
        ReleaseSRWLockExclusive(&g_lock);
        return NULL;
    }
    g_next = at + span + GUARD_PAGE;
    ReleaseSRWLockExclusive(&g_lock);
    if (!VirtualAlloc(at, span, MEM_COMMIT, PAGE_READWRITE))
        return NULL;
    // read-only guard: the game over-reads tiles faithfully (harmless), only writes past the end fault
    VirtualAlloc(at + span, GUARD_PAGE, MEM_COMMIT, PAGE_READONLY);
    // 4-aligned (the game's own alignment) so even a 4-byte overrun reaches the guard page
    uint8_t* block = at + span - ((size + 3) & ~(size_t) 3);
    if (block - 8 < at)
        block = at + 8;
    ((size_t*) block)[-1] = size;
    return block;
}

void* __wrap_malloc(size_t size)
{
    if (!guard_on())
        return __real_malloc(size);
    return guard_alloc(size ? size : 1);
}

void* __wrap_calloc(size_t n, size_t size)
{
    if (!guard_on())
        return __real_calloc(n, size);
    void* p = guard_alloc(n * size ? n * size : 1);
    if (p)
        memset(p, 0, n * size);
    return p;
}

void __wrap_free(void* p)
{
    if (!p)
        return;
    if (is_guarded(p)) {
        uint8_t* page = (uint8_t*) ((uintptr_t) p & ~(uintptr_t) (GUARD_PAGE - 1));
        size_t size = ((size_t*) p)[-1];
        VirtualFree(page, ((uint8_t*) p + size) - page, MEM_DECOMMIT);// use-after-free faults too
        return;
    }
    __real_free(p);
}

void* __wrap_realloc(void* p, size_t size)
{
    if (!guard_on() && !is_guarded(p))
        return __real_realloc(p, size);
    if (!p)
        return __wrap_malloc(size);
    if (!is_guarded(p)) {
        // allocated before the guard heap came up: copy into a guarded block
        void* q = guard_alloc(size ? size : 1);
        size_t old = _msize(p);
        if (q)
            memcpy(q, p, old < size ? old : size);
        __real_free(p);
        return q;
    }
    size_t old = ((size_t*) p)[-1];
    void* q = guard_alloc(size ? size : 1);
    if (q)
        memcpy(q, p, old < size ? old : size);
    __wrap_free(p);
    return q;
}
