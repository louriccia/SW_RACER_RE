// hook_mode.h for the standalone build: by default every reimplementation is the only
// implementation. hook_install() records which body stands for each retail entry, and points the
// retail entry at it when the retail code range is mapped (feature build, retail bisecting).
//
// With retail's decrypted code loaded (SWR_STANDALONE_RETAIL_TEXT), the functions named in
// SWR_STANDALONE_RETAIL (a file, one name or `prefix*` per line) run retail's body instead: the
// retail entry is left alone and our entry jumps to it, so callers on either side reach retail.
// That makes the standalone a bisecting tool: retail and src bodies side by side in one process.

#include <windows.h>
#include <string.h>

#include "hook_mode.h"
#include "standalone.h"

int (*hook_mode_delta_owned)(uint32_t reimpl_addr, uint32_t retail_addr) = NULL;
void* (*hook_mode_shadow_entry)(const char* function_name, uint32_t reimpl_addr, uint32_t retail_addr, hook_ret_kind ret) = NULL;

void standalone_ftol(void);

#define MAX_BODIES 4096
static uint32_t body_addr[MAX_BODIES], body_retail[MAX_BODIES];
static int body_count = 0;

// The retail entry a src body was installed at, or 0.
uint32_t standalone_retail_of(uint32_t body)
{
    for (int i = 0; i < body_count; i++)
        if (body_addr[i] == body)
            return body_retail[i];
    return 0;
}

// Our body for a retail function entry, or 0.
uint32_t standalone_body_of(uint32_t retail)
{
    for (int i = 0; i < body_count; i++)
        if (body_retail[i] == retail)
            return body_addr[i];
    return 0;
}

static void record_body(uint32_t body, uint32_t retail)
{
    if (body_count < MAX_BODIES) {
        body_addr[body_count] = body;
        body_retail[body_count++] = retail;
    }
}

static int installed = 0;
static int skipped = 0;
static int retail_run = 0;

#define MAX_RETAIL_RULES 4096
static char* retail_rules[MAX_RETAIL_RULES];
static int retail_rule_count = 0;

// The lockstep harness hooks these bodies of ours; they must stay ours.
static const char* const always_ours[] = { "swrRace_IncrementFrameTimer", "swrObjHang_StartRace", "swrControl_ProcessInputs",
                                           "swrObjHang_F0", "stdDisplay_Update", "WinMain" };

uint8_t* hook_retail_ptr(uint32_t retail_addr)
{
    return (uint8_t*)retail_addr;
}

hook_mode hook_mode_for(const char* function_name)
{
    (void)function_name;
    return HOOK_MODE_REIMPL;
}

int hook_verify_requested(void)
{
    return 0;
}

static void load_retail_rules(void)
{
    char path[MAX_PATH];
    if (!standalone_retail_text_loaded() || !GetEnvironmentVariableA("SWR_STANDALONE_RETAIL", path, sizeof(path)))
        return;
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(standalone_log, "retail list %s not found\n", path);
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f) && retail_rule_count < MAX_RETAIL_RULES) {
        char* s = line;
        while (*s == ' ' || *s == '\t')
            s++;
        size_t n = strcspn(s, " \t\r\n#");
        if (n == 0)
            continue;
        s[n] = '\0';
        retail_rules[retail_rule_count++] = _strdup(s);
    }
    fclose(f);
    fprintf(standalone_log, "retail list %s: %d rules\n", path, retail_rule_count);
}

static int wants_retail(const char* name)
{
    for (size_t i = 0; i < sizeof(always_ours) / sizeof(always_ours[0]); i++)
        if (!strcmp(name, always_ours[i]))
            return 0;
    for (int i = 0; i < retail_rule_count; i++) {
        const char* r = retail_rules[i];
        size_t n = strlen(r);
        if (n && r[n - 1] == '*' ? !strncmp(name, r, n - 1) : !strcmp(name, r))
            return 1;
    }
    return 0;
}

void hook_install(const char* function_name, uint32_t reimpl_addr, uint32_t retail_addr, hook_install_kind kind, hook_ret_kind ret, FILE* hook_log)
{
    (void)kind;
    (void)ret;
    (void)hook_log;
    if (retail_addr < 0x00401000u || retail_addr >= 0x004ac000u) {
        fprintf(standalone_log, "skip %s: 0x%08x outside retail .text\n", function_name, retail_addr);
        skipped++;
        return;
    }
    if (wants_retail(function_name)) {
        DWORD old;
        VirtualProtect((void*)reimpl_addr, 5, PAGE_EXECUTE_READWRITE, &old);
        standalone_write_jmp(reimpl_addr, retail_addr);
        VirtualProtect((void*)reimpl_addr, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)reimpl_addr, 5);
        record_body(reimpl_addr, retail_addr);
        retail_run++;
        return;
    }
    // retail's _ftol takes its argument in ST0, unlike the C stand-in
    if (!strcmp(function_name, "c__ftol"))
        reimpl_addr = (uint32_t)standalone_ftol;
    // Without the retail code range (the default), the table alone maps retail entries to bodies.
    if (standalone_code_range_mapped())
        standalone_write_jmp(retail_addr, reimpl_addr);
    record_body(reimpl_addr, retail_addr);
    installed++;
}

void hook_mode_log_summary(FILE* hook_log)
{
    fprintf(hook_log, "bodies installed at their retail addresses: %d, running retail: %d (skipped %d)\n", installed, retail_run,
            skipped);
}

void hook_generated(FILE* hook_log);

void standalone_install_bodies(void)
{
    load_retail_rules();
    hook_generated(standalone_log);
}
