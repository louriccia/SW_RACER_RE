// hook_mode.h for the standalone build: every reimplementation is the only implementation, so
// hook_install() points each retail entry at our body and nothing ever points the other way.

#include <windows.h>

#include "hook_mode.h"
#include "standalone.h"

int (*hook_mode_delta_owned)(uint32_t reimpl_addr, uint32_t retail_addr) = NULL;
void* (*hook_mode_shadow_entry)(const char* function_name, uint32_t reimpl_addr, uint32_t retail_addr, hook_ret_kind ret) = NULL;

static int installed = 0;
static int skipped = 0;

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
    standalone_write_jmp(retail_addr, reimpl_addr);
    installed++;
}

void hook_mode_log_summary(FILE* hook_log)
{
    fprintf(hook_log, "bodies installed at their retail addresses: %d (skipped %d)\n", installed, skipped);
}

void hook_generated(FILE* hook_log);

void standalone_install_bodies(void)
{
    hook_generated(standalone_log);
}
