// Feature build of the standalone EXE (swe1r.exe): the dinput_hook feature layer (GL renderer,
// deltas, mods, imgui) linked into the standalone instead of injected into retail.
//
// The hook DLL attaches its deltas to retail code: hook_function(name, retail_addr, delta) is a
// Detours hook on the retail entry, and hook_replace(reimpl, delta) moves a reverse-hooked entry
// to the delta. Here no retail code runs: every retail entry is a jmp to our src body. So both
// forms become one Detours hook on our body, and hook_call_original() reaches that body through
// the trampoline, whether the caller names the retail address or the src symbol.

#include "audio/openal_audio.h"
#include "movies/movie_player.h"
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

#include "hook_helper.h"
#include "patch.h"
#include "lockstep.h"
#include "crash_logger.h"
#include "memsafety.h"
#include "renderer_hook.h"
#include "custom_tracks.h"
#include "track_registry.h"
#include "mod_registry.h"

extern "C" {
#include <hook_mode.h>
#include <Win95/Window.h>
#include "standalone.h"
}

std::map<void *, DebugFunctionInfo> hooks __attribute__((init_priority(101)));
std::map<void *, void *> hook_replacements __attribute__((init_priority(101)));

static bool in_retail_text(uintptr_t a) {
    return a >= SWR_TEXT_ADDR_ && a < 0x004ac000u;
}

// The body a retail entry runs: standalone_install_bodies wrote `jmp body` there.
static void *body_of(uintptr_t retail) {
    const uint8_t *p = (const uint8_t *) retail;
    if (p[0] != 0xe9)
        return (void *) retail;
    int32_t rel;
    memcpy(&rel, p + 1, sizeof(rel));
    return (void *) (retail + 5 + rel);
}

extern "C" void hook_function(const char *function_name, uint32_t original_address, uint8_t *hook_address) {
    hooks[(void *) original_address] = DebugFunctionInfo{
        .name = function_name,
        .address = hook_address,
        .original_address = (void *) original_address,
        .hook_state = (void *) original_address,
    };
}

extern "C" void patchMemoryAccess(uint32_t address, void *newAddress) {
    PatchPointer("patchMemoryAccess", (void *) address, (uint32_t) newAddress);
}

static int delta_owned(uint32_t reimpl_addr, uint32_t retail_addr) {
    return hook_replacements.contains((void *) reimpl_addr) || hooks.contains((void *) retail_addr);
}

// lockstep.txt asks for a record or replay run.
static bool lockstep_configured() {
    FILE *f = fopen("lockstep.txt", "r");
    char line[64];
    bool on = false;
    while (f && fgets(line, sizeof(line), f))
        on |= !strncmp(line, "record", 6) || !strncmp(line, "replay", 6);
    if (f)
        fclose(f);
    return on;
}

// Lockstep runs skip every Smush movie: they only cost wall-clock time, and whether the intro played
// (it polls input every frame) made some replays diverge.
static int __cdecl Window_PlayCinematic_skip(char *) {
    return 1;
}

struct Attach {
    std::string name;
    void *delta;
    void *key;// what this delta's hook_call_original() names: the retail address or the src symbol
};

// A function can carry several deltas (e.g. freecam's forward hook plus a hook_replace). In the DLL
// the second Detours attach copies the first one's jmp into its trampoline, so they chain: the
// replace delta runs first, its original is the forward delta, whose original is the body. Same
// order here: forward hooks attach first (innermost), replacements last (outermost).
extern "C" void init_hooks() {
    fprintf(hook_log, "[init_hooks standalone]\n");
    lockstep_vanilla_filter();
    hook_mode_delta_owned = delta_owned;
    if (lockstep_configured())
        hook_replacements[(void *) Window_PlayCinematic] = (void *) Window_PlayCinematic_skip;

    std::map<void *, std::vector<Attach>> forward, replace;// keyed by body
    std::set<std::pair<void *, void *>> seen;
    auto add = [&](std::map<void *, std::vector<Attach>> &into, void *body, void *delta, void *key, const std::string &name) {
        if (delta == body || !seen.insert({body, delta}).second)
            return;
        into[body].push_back(Attach{name, delta, key});
    };
    for (auto &[key, info]: hooks) {
        const uintptr_t orig = (uintptr_t) info.original_address;
        if (in_retail_text(orig)) {
            // hook_function(name, retail, delta); a reimpl named as the delta may itself be replaced
            auto r = hook_replacements.find(info.address);
            if (body_of(orig) == info.original_address)
                fprintf(hook_log, "    WARNING %s: no src body installed at 0x%08x, src callers bypass the delta\n",
                        info.name.c_str(), (unsigned) orig);
            add(forward, body_of(orig), r != hook_replacements.end() ? r->second : info.address, info.original_address, info.name);
        } else {
            // reverse form, hook_function(name, reimpl, retail): only matters if a delta replaces it
            auto r = hook_replacements.find(info.original_address);
            if (r != hook_replacements.end())
                add(replace, info.original_address, r->second, info.original_address, info.name);
        }
    }
    for (auto &[reimpl, delta]: hook_replacements)
        add(replace, reimpl, delta, reimpl, "(replace)");

    std::map<void *, DebugFunctionInfo> installed;
    std::set<void *> bodies;
    int ok = 0, failed = 0;
    auto attach = [&](void *body, const Attach &a) {
        DebugFunctionInfo info{a.name, a.delta, body, body};
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        LONG err = DetourAttach(&info.hook_state, a.delta);
        if (err == NO_ERROR)
            err = DetourTransactionCommit();
        else
            DetourTransactionAbort();
        if (err != NO_ERROR) {
            fprintf(hook_log, "    %s: cannot hook body %p (err %ld)\n", a.name.c_str(), body, err);
            failed++;
            return;
        }
        fprintf(hook_log, "    %s: body %p -> %p\n", a.name.c_str(), body, a.delta);
        installed[a.key] = info;
        bodies.insert(body);
        ok++;
    };
    for (auto &[body, list]: forward)
        for (auto &a: list)
            attach(body, a);
    for (auto &[body, list]: replace)
        for (auto &a: list)
            attach(body, a);

    // A delta may name its function either way; fill in the key a chain does not use.
    hooks.clear();
    for (auto &[key, info]: installed)
        hooks[key] = info;
    for (void *body: bodies) {
        const uintptr_t retail = standalone_retail_of((uint32_t) body);
        if (!hooks.contains(body)) {
            // forward-only chain: the src symbol reaches the innermost original
            hooks[body] = installed.at((void *) retail);
        }
        if (retail && !hooks.contains((void *) retail))
            hooks[(void *) retail] = installed.at(body);
    }
    fprintf(hook_log, "[init_hooks standalone]: %d deltas attached, %d failed\n", ok, failed);
    fflush(hook_log);
}

// The DLL's ai_full_lod NOPs three branches in retail .text; here it needs a src-level switch.
static bool ai_full_lod_enable(ModId, void *) {
    fprintf(hook_log, "[ai_full_lod] not available in the standalone build yet\n");
    return false;
}

static const ModModule ai_full_lod_mod = {
    .name = "ai_full_lod",
    .version = "1.0",
    .depends_on = nullptr,
    .user = nullptr,
    .enable = ai_full_lod_enable,
    .on_disable = nullptr,
};
ModId mod_ai_full_lod = MOD_ID_INVALID;

// The CRT's call to WinMain lands here (-Wl,--wrap=WinMain@16) once every static initializer has
// run, which the feature layer needs; then the game starts through our WinMain, now detoured to
// WinMain_delta.
extern "C" int __stdcall __real_WinMain(HINSTANCE, HINSTANCE, PSTR, int);
extern "C" int __stdcall __wrap_WinMain(HINSTANCE hInstance, HINSTANCE hPrev, PSTR cmd, int show) {
    crash_logger_install();
    crash_logger_start();
    mod_ai_full_lod = register_mod(&ai_full_lod_mod);
    memsafety_Init();
    memsafety_RegisterHooks();
    crash_logger_stage("init: renderer hooks");
    init_renderer_hooks();
#ifdef SWR_OPENAL_AUDIO
    init_openal_audio_hooks();
#endif
#ifdef SWR_OWN_MOVIES
    init_movie_player();
#endif
    crash_logger_stage("init: game hooks");
    init_hooks();
    crash_logger_stage("init: custom tracks");
    init_customTracks();
    track_registry_Init();
    crash_logger_stage("init: complete");
    return __real_WinMain(hInstance, hPrev, cmd, show);
}
