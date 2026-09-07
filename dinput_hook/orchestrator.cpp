#include "orchestrator.h"
#include "hook_helper.h"
#include "debug_ui.h"
#include "patch.h"
#include "imgui_utils.h"             // settings_ini_path
#include "game_deltas/tracks_delta.h"// swrUI_GetTrackNameFromId_delta
#include "broadcast/overlay.h"
#include "camera/director.h"

#include <imgui.h>

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>

extern "C" {
#include <Swr/swrObj.h>
#include <Swr/swrRace.h>
#include <Swr/swrEvent.h>
#include <Swr/swrMultiplayer.h>
#include <Swr/swrText.h>
#include <globals.h>
void hook_function(const char *function_name, uint32_t original_address, uint8_t *hook_address);
}

// ---------------------------------------------------------------------------------------------
// Settings ([orchestrator] in SW_RACER_RE.ini)

static int g_laps = 1;
static int g_racers = 20;
static float g_cooldown_s =
    60.0f;// results / betting window: from the winner's finish to the next load
static bool g_rotate_tracks = true;
static bool g_unstick = true;
static float g_stuck_s = 8.0f;// no progress for this long -> snap the pod back onto the spline
static bool g_dnf = false;
static float g_dnf_s = 240.0f;    // after the winner, racers still out are marked finished
static bool g_full_physics = true;// keep every AI pod off the on-rails LOD path
static bool g_ai_damage = true;   // AI take fire damage and can explode like a human
static bool g_ai_repair =
    false;// let AI repair (off: fires burn until the engine blows -- more drama)
static float g_repair_start = 0.5f;
static float g_repair_stop = 0.2f;
static bool g_ai_lighting = true; // light AI pods from the followed pod's light bank
static bool g_shuffle_grid = true;// random starting grid (stock: roster order, favourite up front)
static float g_snapshot_s = 20.0f;// periodic field snapshot to hook.log (0 = off)

// ---------------------------------------------------------------------------------------------
// Run state

static bool g_armed = false;
static int g_races_started = 0;
static int g_races_finished = 0;
static bool g_start_requested = false;  // panel "Start now" (from a menu)
static bool g_skip_requested = false;   // panel "Skip": end the current race / cooldown now
static bool g_restart_requested = false;// panel "Restart": end it and rerun the same track
static int g_force_track = -1;          // next start_race uses this track instead of picking one
static int g_next_track = -1;// pre-picked at the winner's finish so the overlay can show it
static char g_status[128] = "idle";

static RaceDescriptor g_race = {-1, 1, 20, 0};
static OrchestratorListener g_listeners[8];
static int g_listener_count = 0;

void orchestrator_Subscribe(OrchestratorListener cb) {
    if (cb != NULL && g_listener_count < 8)
        g_listeners[g_listener_count++] = cb;
}

const RaceDescriptor *orchestrator_CurrentRace() {
    return &g_race;
}

static void emit(int event) {
    for (int i = 0; i < g_listener_count; i++)
        g_listeners[i](event, &g_race);
}

// Per race
static bool g_cooldown_active = false;// winner is in; counting down to the next race
static DWORD g_cooldown_end_ms = 0;
static bool g_fini_fired = false;
static DWORD g_last_snapshot_ms = 0;
static DWORD g_first_finish_ms = 0;
static const int MAX_RACERS = 20;
static float g_last_progress[MAX_RACERS];
static DWORD g_last_progress_ms[MAX_RACERS];
static int g_snaps[MAX_RACERS];
static bool g_dnf_marked[MAX_RACERS];
static int g_snaps_total = 0;
static int g_dnf_total = 0;
static int g_ai_explosions = 0;

// Track pick: uniform over the 25 tracks, never one of the last TRACK_HISTORY played.
static const int TRACK_COUNT = 25;
static const int TRACK_HISTORY = 10;
static int g_track_history[TRACK_HISTORY];
static int g_track_history_len = 0;

static void set_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, ap);
    va_end(ap);
    fprintf(hook_log, "[orchestrator] %s\n", g_status);
    fflush(hook_log);
}

static swrObjHang *get_hang() {
    return (swrObjHang *) swrEvent_GetItem('Hang', 0);
}

static swrObjJdge *get_jdge() {
    return (swrObjJdge *) swrEvent_GetItem('Jdge', 0);
}

static bool jdge_asleep(const swrObjJdge *jdge) {
    return (((const uint8_t *) &jdge->obj.flags)[1] & 0x10) != 0;
}

static bool racer_out_on_track(const swrScore *score) {
    return (score->flag & 1) != 0 && (score->flag & 2) == 0 && score->obj_test_ptr != NULL;
}

// ---------------------------------------------------------------------------------------------
// Full model for AI when nobody local is racing.
//
// swrObjJdge_SpawnRacers picks the pod model class per racer: the ai_full_lod patch (main.cpp)
// NOPs the 'Locl' compare so every racer takes the full-pod path, but a second gate right after it
// (0x00466570: CMP ECX(numLocalPlayers),0 / JNZ / MOV EDI,3) still drops the whole grid to the
// low-detail bot model whenever there is no local player. NOP that MOV while the loop is armed.

static const uint32_t SPAWN_NOLOCAL_BOTCLASS_ADDR = 0x00466576;// MOV EDI,0x3 (bf 03 00 00 00)
static const uint8_t SPAWN_NOLOCAL_BOTCLASS_STOCK[5] = {0xbf, 0x03, 0x00, 0x00, 0x00};
static const PatchOwner PATCH_OWNER = "orchestrator";
static bool g_lod_patched = false;

static void apply_lod_patch(bool on) {
    if (on == g_lod_patched)
        return;
    if (on) {
        if (memcmp((const void *) SPAWN_NOLOCAL_BOTCLASS_ADDR, SPAWN_NOLOCAL_BOTCLASS_STOCK, 5) !=
            0) {
            set_status("LOD gate bytes differ from stock at 0x%08x, not patching",
                       SPAWN_NOLOCAL_BOTCLASS_ADDR);
            g_lod_patched = true;// do not retry every frame
            return;
        }
        static const uint8_t nops[5] = {0x90, 0x90, 0x90, 0x90, 0x90};
        g_lod_patched = WriteMemory(PATCH_OWNER, (void *) SPAWN_NOLOCAL_BOTCLASS_ADDR, nops, 5);
    } else {
        UndoOwner(PATCH_OWNER);
        g_lod_patched = false;
    }
}

// ---------------------------------------------------------------------------------------------
// Track pick + race entry

typedef void(__cdecl *swrObjHang_LoadScreen_t)(swrObjHang *hang, int a, int b);

static void remember_track(int t) {
    if (g_track_history_len < TRACK_HISTORY) {
        g_track_history[g_track_history_len++] = t;
    } else {
        memmove(g_track_history, g_track_history + 1, sizeof(int) * (TRACK_HISTORY - 1));
        g_track_history[TRACK_HISTORY - 1] = t;
    }
}

static bool track_recent(int t) {
    for (int i = 0; i < g_track_history_len; i++)
        if (g_track_history[i] == t)
            return true;
    return false;
}

static int pick_track(int current) {
    if (!g_rotate_tracks)
        return current;
    int candidates[TRACK_COUNT];
    int n = 0;
    for (int t = 0; t < TRACK_COUNT; t++)
        if (!track_recent(t))
            candidates[n++] = t;
    if (n == 0)
        return current;
    return candidates[rand() % n];
}

static const char *track_name(int track_index) {
    static char buf[64];
    const char *raw = swrUI_GetTrackNameFromId_delta(track_index);
    size_t o = 0;
    for (const char *p = raw ? raw : ""; *p && o + 1 < sizeof(buf); p++) {
        if (*p == '~' && p[1]) {
            p++;
            continue;
        }
        buf[o++] = *p;
    }
    buf[o] = '\0';
    return buf;
}

static void reset_race_watch() {
    for (int i = 0; i < MAX_RACERS; i++) {
        g_last_progress[i] = -1.0f;
        g_last_progress_ms[i] = 0;
        g_snaps[i] = 0;
        g_dnf_marked[i] = false;
    }
    g_first_finish_ms = 0;
    g_fini_fired = false;
    g_cooldown_active = false;
    g_cooldown_end_ms = 0;
}

// Configure the hangar for an all-AI race and jump straight into the loading screen, the way the
// demo 'Abrt' handler and the pause-menu 'RStr' restart do. Never call from inside the ImGui frame:
// LoadScreen renders the progress bar through stdDisplay_Update (nested frame).
static void start_race(swrObjHang *hang) {
    hang->demo_mode = 1;
    hang->num_players = (char) g_racers;
    hang->numLaps = (char) g_laps;
    hang->isTournamentMode = 0;
    hang->timeAttackMode = 0;
    if (g_force_track >= 0) {
        hang->track_index = (char) g_force_track;
    } else {
        hang->track_index =
            (char) (g_next_track >= 0 ? g_next_track : pick_track(hang->track_index));
        remember_track(hang->track_index);
    }
    g_force_track = -1;
    g_next_track = -1;

    g_races_started++;
    reset_race_watch();
    g_race.track_index = hang->track_index;
    g_race.laps = g_laps;
    g_race.racer_count = g_racers;
    g_race.human_slots = 0;
    set_status("race %d: track %d (%s), %d racers, %d lap(s)", g_races_started, hang->track_index,
               track_name(hang->track_index), g_racers, g_laps);
    ((swrObjHang_LoadScreen_t) swrObjHang_LoadScreen_ADDR)(hang, 1, 0);
    emit(ORCH_RACE_STARTED);
}

// ---------------------------------------------------------------------------------------------
// Hooks

typedef int(__cdecl *swrObjHang_F4_t)(swrObjHang *hang, int *subEvents, int *p3);

// Race-end events reach the hangar here ('Fini' after a race, 'Abrt' on a bail-out). Chain the next
// race right here, exactly where the retail demo loop calls LoadScreen, so the holotable results
// screen is never shown: the results / betting window already happened on the track.
// swrObjHang_F4 is a reverse-hooked HANG stub, so hook the raw game address (see hook_mechanism).
static int __cdecl swrObjHang_F4_delta(swrObjHang *hang, int *subEvents, int *p3) {
    const int event = *subEvents;
    const int r = hook_call_original((swrObjHang_F4_t) swrObjHang_F4_ADDR, hang, subEvents, p3);
    if (g_armed && swrMultiplayer_IsMultiplayerEnabled() == 0 &&
        (event == 'Fini' || event == 'Abrt')) {
        if (g_races_started == 0) {// the first race was started from the menu by hand
            g_races_started = 1;
            remember_track(hang->track_index);
            g_race.track_index = hang->track_index;
            g_race.laps = hang->numLaps;
            g_race.racer_count = hang->num_players;
            g_race.human_slots = 0;
        }
        g_races_finished++;
        emit(ORCH_RACE_ENDED);
        set_status("race %d ended (%s); %d snaps, %d DNF, %d explosions so far", g_races_started,
                   event == 'Fini' ? "Fini" : "Abrt", g_snaps_total, g_dnf_total, g_ai_explosions);
        start_race(hang);
    }
    return r;
}

// Full physics for every AI pod. swrObjTest_F0 stores the distance to the nearest viewport camera in
// lodDistance and every fidelity gate (ground raycast, wall contact, surface tags, move update) keys
// on (lodDistance - 400) / 600 >= 1: past ~1000 units a non-local pod runs on rails with no wall
// collision. swrRace_delta.cpp already clamps it after F0 for pods within 20000 units; with the camera
// on the favourite, the tail of a 20-pod field sits beyond that and drifts off the track (its spline
// progress then stops validating -- the "stuck" pods). CalcTargetTurnRate is F0's very next call after
// the store, so clamping here covers the autopilot and everything downstream. 90 keeps the hover-pad
// detail refresh (gate at 100) on too.
static const int FULL_PHYSICS_LOD = 90;

// Engine damage has no effect on an AI pod in vanilla: the steering pull lives in the player control
// path and the explosion check lives in swrRace_Repair, which only humans (and the post-finish
// FORCE_GROUND pod) ever run. The human path (swrRace_UpdatePlayerControl) also ticks
// swrRace_ApplyEngineDamage every frame: each engine on fire (engineStatus bit 8, lit by
// swrRace_UpdateHeat) takes (rand*0.1 + 0.1) * dt damage until repaired or destroyed. Same tick here
// (minus that routine's fire SFX / rumble, which key on the shared channel-0 sfx flag), then Repair:
// with REPAIRING clear it never selects an engine to heal but still runs the destroyed-engine
// warning + explosion path (swrRace_Explode -> death -> respawn).
static void supervise_ai_damage(swrRace *pod) {
    if ((pod->flags0 & swrObjTest_FLAG0_AI) == 0 || (pod->flags0 & swrObjTest_FLAG0_LOCAL) != 0)
        return;
    if ((pod->flags0 & swrObjTest_FLAG0_STATE_MASK) != swrObjTest_FLAG0_RACING ||
        (pod->flags0 & swrObjTest_FLAG0_DEAD) != 0 ||
        (pod->flags1 & swrObjTest_FLAG1_FINISHED) != 0 ||
        (pod->flags1 & swrObjTest_FLAG1_FORCE_GROUND) != 0)
        return;
    const float dt = (float) swrRace_deltaTimeSecs;
    bool fire = false;
    float worst = 0.0f;
    for (int i = 0; i < 6; i++) {
        if ((pod->engineStatus[i] & 8) != 0) {
            fire = true;
            const float r = (float) rand() / (float) RAND_MAX;
            swrRace_TakeDamage((int) pod, i, (r * 0.1f + 0.1f) * dt);
        }
        worst = std::max(worst, pod->engineHealth[i]);
    }
    bool repairing = false;
    if (g_ai_repair) {
        repairing = (pod->flags0 & swrObjTest_FLAG0_REPAIRING) != 0;
        if (fire || (worst > g_repair_start && (pod->flags0 & swrObjTest_FLAG0_BOOSTING) == 0))
            repairing = true;
        else if (worst < g_repair_stop || (pod->flags0 & swrObjTest_FLAG0_BOOSTING) != 0)
            repairing = false;
    }
    if (repairing)
        pod->flags0 = (swrObjTest_FLAG0) (pod->flags0 | swrObjTest_FLAG0_REPAIRING);
    else
        pod->flags0 = (swrObjTest_FLAG0) (pod->flags0 & ~swrObjTest_FLAG0_REPAIRING);
    const bool was_dead = (pod->flags0 & swrObjTest_FLAG0_DEAD) != 0;
    swrRace_Repair(pod);
    if (!was_dead && (pod->flags0 & swrObjTest_FLAG0_DEAD) != 0) {
        g_ai_explosions++;
        set_status("race %d: pod id %d exploded from engine damage (%.2f)", g_races_started,
                   pod->obj.id, worst);
    }
}

typedef void(__cdecl *swrRace_CalcTargetTurnRate_t)(swrRace *player);

static void __cdecl swrRace_CalcTargetTurnRate_delta(swrRace *player) {
    if (g_armed && g_full_physics && player != NULL &&
        (player->flags0 & swrObjTest_FLAG0_LOCAL) == 0 && player->lodDistance > FULL_PHYSICS_LOD)
        player->lodDistance = FULL_PHYSICS_LOD;
    hook_call_original((swrRace_CalcTargetTurnRate_t) swrRace_CalcTargetTurnRate_ADDR, player);
    if (g_armed && g_ai_damage && player != NULL && firstLocalPlayer == NULL)
        supervise_ai_damage(player);
}

// Starting grid. swrObjJdge_SpawnRacer places each pod at grid index score->unk14 = its roster
// slot, so the favourite (slot 1) starts on the front row every race. Shuffle the indices among the
// racers before the spawn loop when nobody local is racing.
typedef void(__cdecl *swrObjJdge_SpawnRacers_t)(swrObjJdge *judge, swrScore *scores);

static void __cdecl swrObjJdge_SpawnRacers_delta(swrObjJdge *judge, swrScore *scores) {
    if (g_armed && g_shuffle_grid && judge != NULL && scores != NULL && firstLocalPlayer == NULL) {
        const int n = std::min(judge->num_players, MAX_RACERS);
        for (int i = n - 1; i > 0; i--) {
            const int j = rand() % (i + 1);
            std::swap(scores[i].unk14, scores[j].unk14);
        }
        set_status("race %d: grid shuffled", g_races_started);
    }
    hook_call_original((swrObjJdge_SpawnRacers_t) swrObjJdge_SpawnRacers_ADDR, judge, scores);
}

void orchestrator_RegisterHooks() {
    hook_function("swrObjJdge_SpawnRacers", (uint32_t) swrObjJdge_SpawnRacers_ADDR,
                  (uint8_t *) swrObjJdge_SpawnRacers_delta);
    hook_function("swrObjHang_F4", (uint32_t) swrObjHang_F4_ADDR, (uint8_t *) swrObjHang_F4_delta);
    hook_function("swrRace_CalcTargetTurnRate", (uint32_t) swrRace_CalcTargetTurnRate_ADDR,
                  (uint8_t *) swrRace_CalcTargetTurnRate_delta);
    srand((unsigned) time(NULL));
}

// ---------------------------------------------------------------------------------------------
// In-race supervision

// Light AI pods. Every pod carries a light bank index (its entity id), but swrObjJdge_SpawnRacer tags
// only LOCAL pods' root node to use it (flags_5 |= 0xc, light_index) plus per-part light bits, and
// only a pod followed by a camera-man gets its bank refreshed from the terrain
// (swrObjcMan_UpdateLighting). In an all-AI race that is the favourite (score flag 0x20). Point every
// other full pod at that bank with the same node tags SpawnRacer applies.
static const int LIGHT_BANK_COUNT = 12;// numEnabledLights[12]; lightColor1[13] is indexed +1

static void apply_ai_lighting(swrObjJdge *jdge) {
    // The bank being refreshed is the followed pod's (swrObjcMan_UpdateLighting runs for the pod
    // the camera-man follows); fall back to the favourite before the camera is assigned.
    int bank = -1;
    const int followed = director_FollowedSlot();
    if (followed >= 0 && swrScoresPtr[followed].obj_test_ptr != NULL)
        bank = swrScoresPtr[followed].obj_test_ptr->current_light_index;
    for (int i = 0; bank < 0 && i < jdge->num_players && i < MAX_RACERS; i++) {
        const swrScore *score = &swrScoresPtr[i];
        if ((score->flag & 0x20) != 0 && score->obj_test_ptr != NULL)
            bank = score->obj_test_ptr->current_light_index;
    }
    if (bank < 0 || bank >= LIGHT_BANK_COUNT)
        return;
    static const int LIT_PARTS_0x10[] = {1, 2, 3, 4, 5, 0x47};
    static const int LIT_PARTS_0x100[] = {0x2a, 0x2b, 0x1c, 0x1d};
    for (int i = 0; i < jdge->num_players && i < MAX_RACERS; i++) {
        swrRace *pod = swrScoresPtr[i].obj_test_ptr;
        if (pod == NULL || pod->partNodes == NULL || (pod->flags0 & swrObjTest_FLAG0_LOCAL) != 0)
            continue;
        swrModel_Node *root = pod->partNodes[0];
        if (root == NULL)
            continue;
        root->flags_5 |= 0xc;
        root->light_index = (uint16_t) bank;
        for (int k: LIT_PARTS_0x10)
            if (pod->partNodes[k] != NULL)
                pod->partNodes[k]->flags_5 |= 0x10;
        for (int k: LIT_PARTS_0x100)
            if (pod->partNodes[k] != NULL)
                pod->partNodes[k]->flags_5 |= 0x100;
    }
}

// A pod whose lap progress has not moved for g_stuck_s is snapped back onto the spline with the
// game's own 'Snap' sub-event (swrObjTest_F4: arg 2 -> swrRace_ResetToSpline(pod, +0.01), the same
// call the retail demo spams). Dead pods are left to their own respawn.
static void supervise_stuck(swrObjJdge *jdge, DWORD now) {
    for (int i = 0; i < jdge->num_players && i < MAX_RACERS; i++) {
        swrScore *score = &swrScoresPtr[i];
        if (!racer_out_on_track(score))
            continue;
        swrRace *pod = score->obj_test_ptr;
        const float progress = swrObjJdge_GetRacerProgress(score);
        if (g_last_progress_ms[i] == 0 || progress > g_last_progress[i] + 0.001f) {
            g_last_progress[i] = progress;
            g_last_progress_ms[i] = now;
            continue;
        }
        if ((pod->flags0 & swrObjTest_FLAG0_DEAD) != 0 ||
            (pod->flags0 & swrObjTest_FLAG0_STATE_MASK) != swrObjTest_FLAG0_RACING)
            continue;
        if (now - g_last_progress_ms[i] < (DWORD) (g_stuck_s * 1000.0f))
            continue;
        int snap[2] = {'Snap', 2};
        swrEvent_DispatchSubEvents(pod, snap);
        g_snaps[i]++;
        g_snaps_total++;
        g_last_progress_ms[i] = now;
        set_status("race %d: slot %d stuck at %.3f laps -> Snap (#%d) speed %.1f x%.2f lod %d "
                   "flags0 %08x flags1 %08x pos %.0f %.0f %.0f",
                   g_races_started, i, progress, g_snaps[i], pod->speedValue, pod->speedMultiplier,
                   pod->lodDistance, (unsigned) pod->flags0, (unsigned) pod->flags1,
                   pod->transform.vD.x, pod->transform.vD.y, pod->transform.vD.z);
    }
}

// Compact per-slot line every g_snapshot_s: progress, speed, camera distance, state nibble.
static void log_snapshot(swrObjJdge *jdge, DWORD now) {
    if (g_snapshot_s <= 0.0f || now - g_last_snapshot_ms < (DWORD) (g_snapshot_s * 1000.0f))
        return;
    g_last_snapshot_ms = now;
    char line[1024];
    int o = snprintf(line, sizeof(line), "[orchestrator] race %d field:", g_races_started);
    for (int i = 0; i < jdge->num_players && i < MAX_RACERS && o < (int) sizeof(line) - 40; i++) {
        swrScore *score = &swrScoresPtr[i];
        const swrRace *pod = score->obj_test_ptr;
        if (pod == NULL)
            continue;
        o += snprintf(line + o, sizeof(line) - o, " %d:%.2f/%.0f/%d/%x%s", i,
                      swrObjJdge_GetRacerProgress(score), pod->speedValue, pod->lodDistance,
                      (unsigned) (pod->flags0 & 0xf), (score->flag & 2) ? "F" : "");
    }
    fprintf(hook_log, "%s\n", line);
    fflush(hook_log);
}

// Optional: after the winner, racers still out after g_dnf_s are marked finished (score flag 0x2).
static void supervise_dnf(swrObjJdge *jdge, DWORD now) {
    if (g_first_finish_ms == 0 || now - g_first_finish_ms < (DWORD) (g_dnf_s * 1000.0f))
        return;
    for (int i = 0; i < jdge->num_players && i < MAX_RACERS; i++) {
        swrScore *score = &swrScoresPtr[i];
        if (!racer_out_on_track(score) || g_dnf_marked[i])
            continue;
        score->flag |= 2;
        g_dnf_marked[i] = true;
        g_dnf_total++;
        set_status("race %d: slot %d DNF at %.3f laps", g_races_started, i,
                   swrObjJdge_GetRacerProgress(score));
    }
}

// Progress of the racer furthest along (finished racers count as num_laps).

static bool any_finished(const swrObjJdge *jdge) {
    for (int i = 0; i < jdge->num_players && i < MAX_RACERS; i++)
        if ((swrScoresPtr[i].flag & 2) != 0)
            return true;
    return false;
}

// End the race: judge teardown -> 'Fini' to the hangar -> swrObjHang_F4_delta chains the next race.
static void end_race(swrObjJdge *jdge, int event) {
    if (g_fini_fired)
        return;
    g_fini_fired = true;
    swrObjJdge_Clear(jdge, event);
}

// ---------------------------------------------------------------------------------------------
// Per-frame service (game thread, before the ImGui frame)

void orchestrator_Service() {
    apply_lod_patch(g_armed);
    if (!g_armed)
        return;
    if (swrMultiplayer_IsMultiplayerEnabled() != 0)
        return;

    swrObjHang *hang = get_hang();
    if (hang == NULL)
        return;

    // Keep the all-AI roster flag up while armed; BuildRosterSinglePlayer reads it at race start
    // (covers the first race, started from the menu by hand).
    hang->demo_mode = 1;

    overlay_ForceLeaderboard(true);
    overlay_ForceNameplates(true);
    director_SetEnabled(true);
    overlay_SetHighlightSlot(director_FollowedSlot());
    char title[32];
    snprintf(title, sizeof(title), "%s %d", g_cooldown_active ? "RESULTS" : "RACE",
             g_races_started);
    overlay_SetTitle(title);
    if (g_cooldown_active) {
        const DWORD now_ms = GetTickCount();
        const float left =
            now_ms >= g_cooldown_end_ms ? 0.0f : (g_cooldown_end_ms - now_ms) / 1000.0f;
        const int track = g_next_track >= 0 ? g_next_track : (int) hang->track_index;
        char footer[160];
        snprintf(footer, sizeof(footer), "NEXT: %s\n%d racers, %d lap%s  |  starts in %d:%02d",
                 track_name(track), g_racers, g_laps, g_laps == 1 ? "" : "s", (int) left / 60,
                 (int) left % 60);
        overlay_SetFooter(footer);
    } else {
        overlay_SetFooter("");
    }

    swrObjJdge *jdge = get_jdge();
    const bool in_race = jdge != NULL && !jdge_asleep(jdge) && swrJdge_Cleared == 0;
    if (in_race) {
        const int state = jdge->flag & 0xf;
        const DWORD now = GetTickCount();

        if (g_skip_requested || g_restart_requested) {
            if (g_restart_requested)
                g_force_track = hang->track_index;
            g_skip_requested = false;
            g_restart_requested = false;
            set_status("race %d: %s requested -> Abrt", g_races_started,
                       g_force_track >= 0 ? "restart" : "skip");
            end_race(jdge, 'Abrt');
            return;
        }
        if (firstLocalPlayer != NULL)
            return;// a human is racing: never touch that race

        if (g_ai_lighting)
            apply_ai_lighting(jdge);
        if (state == 1 || state == 2) {
            log_snapshot(jdge, now);
            if (g_unstick)
                supervise_stuck(jdge, now);

            // Winner in -> open the results / betting window and pre-pick the next track so the
            // overlay can announce it. The rest of the field keeps racing underneath.
            if (!g_cooldown_active && any_finished(jdge)) {
                g_cooldown_active = true;
                g_first_finish_ms = now;
                g_cooldown_end_ms = now + (DWORD) (g_cooldown_s * 1000.0f);
                g_next_track = pick_track(hang->track_index);
                set_status("race %d: winner in; next race (track %d, %s) in %.0fs", g_races_started,
                           g_next_track, track_name(g_next_track), g_cooldown_s);
                emit(ORCH_WINNER_IN);
            }
            if (g_cooldown_active && g_dnf)
                supervise_dnf(jdge, now);
            if (g_cooldown_active && now >= g_cooldown_end_ms)
                end_race(jdge, 'Fini');
        }
        return;
    }

    // Out of a race (menus): Start now / Skip start one, Restart reruns the last track.
    if (g_skip_requested || g_restart_requested) {
        if (g_restart_requested)
            g_force_track = hang->track_index;
        g_skip_requested = false;
        g_restart_requested = false;
        g_start_requested = true;
    }
    if (g_start_requested) {
        g_start_requested = false;
        start_race(hang);
    }
}

extern "C" void orchestrator_ToggleArmed(void) {
    g_armed = !g_armed;
    g_start_requested = false;
    g_skip_requested = false;
    g_restart_requested = false;
    g_force_track = -1;
    g_next_track = -1;
    if (g_armed) {
        reset_race_watch();
    } else {
        swrObjHang *hang = get_hang();
        if (hang != NULL)
            hang->demo_mode = 0;
        overlay_ForceLeaderboard(false);
        overlay_ForceNameplates(false);
        overlay_SetTitle("");
        overlay_SetFooter("");
        director_SetEnabled(false);
    }
    set_status(g_armed ? "armed (start a Free Play race, or press Start now)" : "disarmed");
}

// ---------------------------------------------------------------------------------------------
// Config: [orchestrator] in SW_RACER_RE.ini (loaded at panel registration, saved on every edit)

static const wchar_t *INI_SECTION = L"orchestrator";

static float ini_get_float(const wchar_t *ini, const wchar_t *key, float def) {
    wchar_t got[48], defbuf[48];
    swprintf(defbuf, 48, L"%.4f", def);
    GetPrivateProfileStringW(INI_SECTION, key, defbuf, got, 48, ini);
    return (float) wcstod(got, NULL);
}
static void ini_set_float(const wchar_t *ini, const wchar_t *key, float v) {
    wchar_t buf[48];
    swprintf(buf, 48, L"%.4f", v);
    WritePrivateProfileStringW(INI_SECTION, key, buf, ini);
}
static void ini_set_int(const wchar_t *ini, const wchar_t *key, int v) {
    wchar_t buf[16];
    swprintf(buf, 16, L"%d", v);
    WritePrivateProfileStringW(INI_SECTION, key, buf, ini);
}

static void load_config() {
    const wchar_t *ini = settings_ini_path();
    g_laps = std::clamp((int) GetPrivateProfileIntW(INI_SECTION, L"laps", g_laps, ini), 1, 10);
    g_racers =
        std::clamp((int) GetPrivateProfileIntW(INI_SECTION, L"racers", g_racers, ini), 1, 20);
    g_cooldown_s = ini_get_float(ini, L"cooldown_s", g_cooldown_s);
    g_rotate_tracks =
        GetPrivateProfileIntW(INI_SECTION, L"rotate_tracks", g_rotate_tracks, ini) != 0;
    g_unstick = GetPrivateProfileIntW(INI_SECTION, L"unstick", g_unstick, ini) != 0;
    g_stuck_s = ini_get_float(ini, L"stuck_s", g_stuck_s);
    g_dnf = GetPrivateProfileIntW(INI_SECTION, L"dnf", g_dnf, ini) != 0;
    g_dnf_s = ini_get_float(ini, L"dnf_s", g_dnf_s);
    g_full_physics = GetPrivateProfileIntW(INI_SECTION, L"full_physics", g_full_physics, ini) != 0;
    g_ai_damage = GetPrivateProfileIntW(INI_SECTION, L"ai_damage", g_ai_damage, ini) != 0;
    g_ai_repair = GetPrivateProfileIntW(INI_SECTION, L"ai_repair", g_ai_repair, ini) != 0;
    g_repair_start = ini_get_float(ini, L"repair_start", g_repair_start);
    g_repair_stop = ini_get_float(ini, L"repair_stop", g_repair_stop);
    g_ai_lighting = GetPrivateProfileIntW(INI_SECTION, L"ai_lighting", g_ai_lighting, ini) != 0;
    g_shuffle_grid = GetPrivateProfileIntW(INI_SECTION, L"shuffle_grid", g_shuffle_grid, ini) != 0;
    g_snapshot_s = ini_get_float(ini, L"snapshot_s", g_snapshot_s);
}

static void save_config() {
    const wchar_t *ini = settings_ini_path();
    ini_set_int(ini, L"laps", g_laps);
    ini_set_int(ini, L"racers", g_racers);
    ini_set_float(ini, L"cooldown_s", g_cooldown_s);
    ini_set_int(ini, L"rotate_tracks", g_rotate_tracks);
    ini_set_int(ini, L"unstick", g_unstick);
    ini_set_float(ini, L"stuck_s", g_stuck_s);
    ini_set_int(ini, L"dnf", g_dnf);
    ini_set_float(ini, L"dnf_s", g_dnf_s);
    ini_set_int(ini, L"full_physics", g_full_physics);
    ini_set_int(ini, L"ai_damage", g_ai_damage);
    ini_set_int(ini, L"ai_repair", g_ai_repair);
    ini_set_float(ini, L"repair_start", g_repair_start);
    ini_set_float(ini, L"repair_stop", g_repair_stop);
    ini_set_int(ini, L"ai_lighting", g_ai_lighting);
    ini_set_int(ini, L"shuffle_grid", g_shuffle_grid);
    ini_set_float(ini, L"snapshot_s", g_snapshot_s);
}

// ---------------------------------------------------------------------------------------------
// Panel

static void panel_orchestrator() {
    ImGui::TextWrapped(
        "Unattended AI-only races back to back. Arm it, then start a Free Play race "
        "normally (or press Start now from a menu). F8 toggles. When the winner finishes "
        "the overlay shows results + the next race; at zero the next race loads directly.");
    bool changed = false;
    bool armed = g_armed;
    if (ImGui::Checkbox("Armed", &armed))
        orchestrator_ToggleArmed();
    changed |= ImGui::SliderInt("Racers", &g_racers, 1, 20);
    changed |= ImGui::SliderInt("Laps", &g_laps, 1, 10);
    changed |= ImGui::SliderFloat("Results / betting window after the winner (s)", &g_cooldown_s,
                                  5.0f, 900.0f, "%.0f");
    changed |= ImGui::Checkbox("Random track (no repeat in last 10)", &g_rotate_tracks);
    changed |= ImGui::Checkbox("Full physics for all AI (no on-rails LOD)", &g_full_physics);
    changed |= ImGui::Checkbox("AI engine damage: fires burn, engines explode", &g_ai_damage);
    changed |= ImGui::Checkbox("AI may repair", &g_ai_repair);
    if (g_ai_repair) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        changed |= ImGui::SliderFloat("start##rep", &g_repair_start, 0.2f, 0.95f, "%.2f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        changed |= ImGui::SliderFloat("stop##rep", &g_repair_stop, 0.0f, 0.5f, "%.2f");
    }
    changed |= ImGui::Checkbox("Light AI pods from the followed pod's light bank", &g_ai_lighting);
    changed |= ImGui::Checkbox("Random starting grid", &g_shuffle_grid);
    ImGui::SetNextItemWidth(120.0f);
    changed |= ImGui::SliderFloat("Field snapshot to log (s)", &g_snapshot_s, 0.0f, 60.0f, "%.0f");
    changed |= ImGui::Checkbox("Snap stuck pods", &g_unstick);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    changed |= ImGui::SliderFloat("after (s)##stuck", &g_stuck_s, 3.0f, 30.0f, "%.0f");
    changed |= ImGui::Checkbox("DNF stragglers", &g_dnf);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    changed |= ImGui::SliderFloat("after winner (s)##dnf", &g_dnf_s, 10.0f, 600.0f, "%.0f");
    if (changed)
        save_config();

    swrObjHang *hang = get_hang();
    swrObjJdge *jdge = get_jdge();
    const bool in_race = jdge != NULL && !jdge_asleep(jdge);
    ImGui::BeginDisabled(!g_armed || hang == NULL || in_race);
    if (ImGui::Button("Start now"))
        g_start_requested = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_armed || hang == NULL);
    if (ImGui::Button("Skip"))
        g_skip_requested = true;
    ImGui::SameLine();
    if (ImGui::Button("Restart"))
        g_restart_requested = true;
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::Text("Started %d, finished %d, snaps %d, DNF %d, AI explosions %d, LOD patch %s",
                g_races_started, g_races_finished, g_snaps_total, g_dnf_total, g_ai_explosions,
                g_lod_patched ? "on" : "off");
    if (jdge != NULL)
        ImGui::Text("Judge: %s, state %d, %d racers%s", in_race ? "awake" : "asleep",
                    jdge->flag & 0xf, jdge->num_players,
                    g_cooldown_active ? ", results window open" : "");
    ImGui::TextWrapped("Status: %s", g_status);
}

static DebugPanel g_panel = {.category = "Race",
                             .name = "Orchestrator",
                             .draw = panel_orchestrator,
                             .dev_only = true};

void orchestrator_RegisterPanel() {
    load_config();
    debug_ui_register(&g_panel);
}
