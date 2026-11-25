// Runs dinput_hook/lockstep.cpp inside the standalone EXE, so a race recorded from retail (vanilla
// mode in the hook DLL) can be replayed here and compared frame by frame.
//
// In the hook DLL, lockstep's wrappers are Detours forward hooks on retail entries and
// hook_call_original() reaches retail through the trampoline. Here the same wrappers are Detours
// hooks on our own compiled src functions, and the trampoline reaches our body. Calls that go
// through a retail address hit standalone_main.c's jmp stub, which lands on the hooked entry too.
// The standalone has no deltas, so it always runs what lockstep calls "vanilla".

#include "hook_helper.h"
#include "lockstep.h"

extern "C" {
#include <Main/swrControl.h>
#include <Swr/swrObj.h>
#include <Swr/swrRace.h>
#include <Win95/stdDisplay.h>
}

#include "standalone.h"

// Constructed before standalone_lockstep_init (priority 110), which runs after standalone_init (101).
std::map<void *, void *> hook_replacements __attribute__((init_priority(101)));
std::map<void *, DebugFunctionInfo> hooks __attribute__((init_priority(101)));

// Delta-layer state lockstep.cpp reads; the standalone has no fixed-timestep or advance-edge deltas.
bool swr_fixedTimestep = false;
extern "C" int g_cutscene_skip_edge = 0;

// The shadow harness is DLL-only.
extern "C" void shadow_frame_tick(void) {}
extern "C" void shadow_race_loaded(void) {}
extern "C" void shadow_report(void) {}

extern "C" void __cdecl swrControl_ProcessInputs_lockstep(void);
extern "C" void __cdecl swrObjHang_F0_lockstep(swrObjHang *hang);

static int __cdecl stdDisplay_Update_lockstep(void) {
    int r = hook_call_original(stdDisplay_Update);
    lockstep_frame_service();
    return r;
}

static bool attach(const char *name, void *key, void *target, void *detour) {
    DebugFunctionInfo info{name, detour, target, target};
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    LONG err = DetourAttach(&info.hook_state, detour);
    if (err != NO_ERROR || DetourTransactionCommit() != NO_ERROR) {
        fprintf(standalone_log, "lockstep: cannot hook %s (err %ld)\n", name, err);
        return false;
    }
    hooks[key] = info;
    hooks[target] = info;
    return true;
}

__attribute__((constructor(110))) static void standalone_lockstep_init(void) {
    FILE *f = fopen("lockstep.txt", "r");
    char line[64] = "";
    bool on = false;
    while (f && fgets(line, sizeof(line), f))
        on |= !strncmp(line, "record", 6) || !strncmp(line, "replay", 6);
    if (f)
        fclose(f);
    if (!on)
        return;
    int n = 0;
    n += attach("swrRace_IncrementFrameTimer", (void *) swrRace_IncrementFrameTimer_ADDR, (void *) swrRace_IncrementFrameTimer,
                (void *) swrRace_IncrementFrameTimer_lockstep);
    n += attach("swrObjHang_StartRace", (void *) swrObjHang_StartRace_ADDR, (void *) swrObjHang_StartRace,
                (void *) swrObjHang_StartRace_lockstep);
    n += attach("swrControl_ProcessInputs", (void *) swrControl_ProcessInputs_ADDR, (void *) swrControl_ProcessInputs,
                (void *) swrControl_ProcessInputs_lockstep);
    n += attach("swrObjHang_F0", (void *) swrObjHang_F0_ADDR, (void *) swrObjHang_F0, (void *) swrObjHang_F0_lockstep);
    n += attach("stdDisplay_Update", (void *) stdDisplay_Update_ADDR, (void *) stdDisplay_Update, (void *) stdDisplay_Update_lockstep);
    fprintf(standalone_log, "lockstep: %d hooks installed, vanilla %d\n", n, (int) lockstep_vanilla());
    fflush(standalone_log);
}
