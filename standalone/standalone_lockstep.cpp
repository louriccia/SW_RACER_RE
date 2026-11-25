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
#include <Swr/swrEvent.h>
#include <Swr/swrObj.h>
#include <Swr/swrRace.h>
#include <Win95/Window.h>
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

// Every puppet's state and root node transform, so runs can be diffed whatever the camera sees.
static void dump_elmo(FILE *e) {
    int n = swrEvent_GetEventCount('Elmo');
    for (int i = 0; i < n; i++) {
        swrObjElmo *elmo = (swrObjElmo *) swrEvent_GetItem('Elmo', i);
        if (!elmo)
            continue;
        fprintf(e, "%d kind %d anim %d pos %.4f %.4f %.4f yaw %.4f tgt %.4f %.4f %.4f wp %d", i, (int) elmo->kind,
                elmo->animCmd, elmo->position.x, elmo->position.y, elmo->position.z, elmo->yaw, elmo->moveTarget.x,
                elmo->moveTarget.y, elmo->moveTarget.z, elmo->lastWaypoint);
        swrModel_NodeTransformed *root = elmo->unk30_nodes ? (swrModel_NodeTransformed *) elmo->unk30_nodes[0] : NULL;
        if (root) {
            const float *m = (const float *) &root->transform;
            fprintf(e, " root");
            for (int k = 0; k < 12; k++)
                fprintf(e, " %.4f", m[k]);
        }
        fprintf(e, "\n");
    }
}

// Test aid: SWR_STANDALONE_ELMO_LOG=N appends every puppet's state to elmo_log.txt each N presented
// frames (menus and cutscenes included).
static void elmo_log_service(void) {
    static int every = -2, frame = 0;
    if (every == -2) {
        const char *v = getenv("SWR_STANDALONE_ELMO_LOG");
        every = v && *v ? atoi(v) : -1;
    }
    if (every <= 0 || ++frame % every)
        return;
    FILE *e = fopen("elmo_log.txt", "a");
    if (!e)
        return;
    fprintf(e, "frame %d count %d\n", frame, swrEvent_GetEventCount('Elmo'));
    dump_elmo(e);
    fclose(e);
}

// Test aid: SWR_STANDALONE_FREEZE_FRAME=N holds the presented image of race frame N (messages keep
// pumping) and writes freeze.txt, so screenshots of two runs can be compared at the same sim frame.
static void freeze_service(void) {
    static int at = -2;
    if (at == -2) {
        const char *v = getenv("SWR_STANDALONE_FREEZE_FRAME");
        at = v && *v ? atoi(v) : -1;
    }
    if (at < 0 || lockstep_shadow_frame() < at)
        return;
    FILE *e = fopen("elmo_dump.txt", "w");
    if (e) {
        dump_elmo(e);
        fclose(e);
    }
    FILE *f = fopen("freeze.txt", "w");
    if (f) {
        fprintf(f, "%d\n", lockstep_shadow_frame());
        fclose(f);
    }
    for (;;) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        Sleep(20);
    }
}

static int __cdecl stdDisplay_Update_lockstep(void) {
    int r = hook_call_original(stdDisplay_Update);
    lockstep_frame_service();
    freeze_service();
    elmo_log_service();
    return r;
}

static int __cdecl stdDisplay_Update_elmo(void) {
    int r = hook_call_original(stdDisplay_Update);
    elmo_log_service();
    return r;
}

// Lockstep runs skip every Smush movie: they only cost wall-clock time, and whether the intro
// played (it also polls input every frame) made some replays diverge.
static int __cdecl Window_PlayCinematic_skip(char *) {
    return 1;
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
    if (!on) {
        if (getenv("SWR_STANDALONE_ELMO_LOG"))
            attach("stdDisplay_Update", (void *) stdDisplay_Update_ADDR, (void *) stdDisplay_Update, (void *) stdDisplay_Update_elmo);
        return;
    }
    int n = 0;
    n += attach("swrRace_IncrementFrameTimer", (void *) swrRace_IncrementFrameTimer_ADDR, (void *) swrRace_IncrementFrameTimer,
                (void *) swrRace_IncrementFrameTimer_lockstep);
    n += attach("swrObjHang_StartRace", (void *) swrObjHang_StartRace_ADDR, (void *) swrObjHang_StartRace,
                (void *) swrObjHang_StartRace_lockstep);
    n += attach("swrControl_ProcessInputs", (void *) swrControl_ProcessInputs_ADDR, (void *) swrControl_ProcessInputs,
                (void *) swrControl_ProcessInputs_lockstep);
    n += attach("swrObjHang_F0", (void *) swrObjHang_F0_ADDR, (void *) swrObjHang_F0, (void *) swrObjHang_F0_lockstep);
    n += attach("stdDisplay_Update", (void *) stdDisplay_Update_ADDR, (void *) stdDisplay_Update, (void *) stdDisplay_Update_lockstep);
    n += attach("Window_PlayCinematic", (void *) Window_PlayCinematic_ADDR, (void *) Window_PlayCinematic,
                (void *) Window_PlayCinematic_skip);
    fprintf(standalone_log, "lockstep: %d hooks installed, vanilla %d\n", n, (int) lockstep_vanilla());
    fflush(standalone_log);
}

// Test aid: SWR_STANDALONE_AUTOPILOT=1 lets the AI steering drive the local pod, so a whole race
// (finish line, results, back to the hangar) can run unattended.
static void __cdecl swrRace_UpdatePlayerControl_autopilot(swrRace *player) {
    swrRace_UpdateAutopilotControl(player);
}

__attribute__((constructor(111))) static void standalone_autopilot_init(void) {
    const char *v = getenv("SWR_STANDALONE_AUTOPILOT");
    if (!v || !*v || *v == '0')
        return;
    bool ok = attach("swrRace_UpdatePlayerControl", (void *) swrRace_UpdatePlayerControl_ADDR,
                     (void *) swrRace_UpdatePlayerControl, (void *) swrRace_UpdatePlayerControl_autopilot);
    fprintf(standalone_log, "autopilot: %s\n", ok ? "on" : "FAILED");
    fflush(standalone_log);
}
