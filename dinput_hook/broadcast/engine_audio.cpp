#include "engine_audio.h"
#include "../camera/director.h"
#include "../debug_ui.h"
#include "../config.h"
#include "../hook_helper.h"

#include <imgui.h>

#include <windows.h>
#include <cstdio>
#include <cmath>

extern "C" void hook_function(const char *function_name, uint32_t original_address,
                              uint8_t *hook_address);

extern "C" {
#include <swr.h>
#include <Swr/swrEvent.h>
#include <Swr/swrRace.h>
#include <Swr/swrSound.h>
#include <globals.h>
#include <types.h>
}

// ---------------------------------------------------------------------------------------------
// swrRace_UpdateEngineSound picks one of two engine voices per pod:
//
//   local ('Locl', flags0 0x20) or FORCE_GROUND -> swrRace_PlayEngineSounds(pod, 0.7)
//   everyone else                               -> one spatial drone, id 0xa6 + (pilot & 1)
//
// The rich path is a four-band gearbox (engine loop 0xaa + band) plus a turbine layer (0xae), the
// boost whoosh (0x72), brake on/off (0x7f / 0x80) and a gear-change hit (0x7a), and every one of
// them is pitched by the pod's own mass:
//
//   pitch = (A - (swrRacer_PodHandlingData[pilotId].bumpMass - B) * C) * D - E
//
// so each character's pod already sounds different - the drone the AI gets throws all of that away.
//
// There is even a seam for it: the stock code routes an AI_SIMPLE pod to the rich path when
// NumLocalPlayers() == 0. Full-physics AI (what the broadcast forces) clears AI_SIMPLE, so nothing
// in an unattended race ever reaches it.
//
// Two things stop us just flipping the gate. swrRace_PlayEngineSounds keeps its gearbox band and
// brake timer in globals indexed by LOCAL PLAYER number, which is 0 for every pod once there are
// no local players - twenty pods would fight over one state machine. And it opens by driving the
// track's ambient cue list off the calling pod's lap progress, which only makes sense for the pod
// the listener is attached to.
//
// So: lend each pod local-player slot 0 for the duration of the call with its own saved band and
// brake timer swapped in, and let only the followed pod drive the ambience.
//
// How many pods can have it at once is not a matter of taste. swrSound_PlaySpatial ends in one
// playASoundImpl(soundId, ...) and the engine ids are a fixed set - one loop per gear band - so two
// pods in the same band land on the same voice and the last caller of the frame wins its pitch and
// pan. Past one pod the engine warbles between them instead of doubling, which is why the default
// is the followed pod alone.

typedef void(__cdecl *swrRace_UpdateEngineSound_t)(swrRace *player);
typedef void(__cdecl *swrRace_PlayEngineSounds_t)(swrRace *player, float volume);
typedef void(__cdecl *swrSound_UpdateEngineAudio_t)(int planet, int track, float *lapProgress);

// swrRace_PlayEngineSounds is not reimplemented; call it through its address.
static void play_engine_sounds(swrRace *player, float volume) {
    ((swrRace_PlayEngineSounds_t) swrRace_PlayEngineSounds_ADDR)(player, volume);
}

// Per-local-player scratch inside swrRace_PlayEngineSounds. Index 0 is the one every pod lands on
// once we lend it the local slot, so each pod's values are swapped through it.
#define swrRace_engineGearBand (*(int(*)[2]) 0x0050cae8)
#define swrRace_engineBrakeTimer (*(float(*)[2]) 0x004c7a58)
#define swrRace_engineBoostLatch (*(int *) 0x0050cb00)

static const char *INI_SECTION = "engine_audio";
static const int MAX_RACERS = 20;

static bool g_enabled = true;
static int g_voices = 1;        // pods nearest the listener that get the full engine
static float g_volume = 0.7f;   // swrRace_UpdateEngineSound passes 0.7 for a local pod
static float g_max_dist = 900.0f;

static int g_rich_now = 0;

struct PodEngineState {
    int band;
    float brake_timer;
    int boost_latch;
};

static PodEngineState g_state[MAX_RACERS];
static bool g_rich[MAX_RACERS];
static unsigned int g_ranked_frame = 0xffffffffu;
static int g_inside_slot = -1;

// swrRace_PlayEngineSounds reads NumLocalPlayers() and GetLocalPlayerNumberFromScore(), and gives
// up on a score that is not a local player. Lend it slot 0 for the duration of one pod's call.
struct LocalSlotLend {
    swrScore *first, *second, *third;
    LocalSlotLend(swrScore *score)
        : first(firstLocalPlayer), second(secondLocalPlayer), third(thirdLocalPlayer) {
        firstLocalPlayer = score;
        secondLocalPlayer = NULL;
        thirdLocalPlayer = NULL;
    }
    ~LocalSlotLend() {
        firstLocalPlayer = first;
        secondLocalPlayer = second;
        thirdLocalPlayer = third;
    }
};

// Only the first num_players score slots belong to this race. The rest keep whatever pod pointer a
// previous, larger field left behind, and those pods are freed between races - reading a transform
// through one is a use-after-free.
static int live_racers() {
    const swrObjJdge *jdge = (const swrObjJdge *) swrEvent_GetItem('Jdge', 0);
    if (jdge == NULL || (((const uint8_t *) &jdge->obj.flags)[1] & 0x10) != 0)
        return 0;
    const int n = jdge->num_players;
    return n < 0 ? 0 : (n > MAX_RACERS ? MAX_RACERS : n);
}

static bool racer_live(int i) {
    return swrScoresPtr != NULL && i >= 0 && i < MAX_RACERS && (swrScoresPtr[i].flag & 1) != 0 &&
           swrScoresPtr[i].obj_test_ptr != NULL;
}

static int slot_of(const swrRace *pod) {
    if (swrScoresPtr == NULL)
        return -1;
    const int n = live_racers();
    for (int i = 0; i < n; i++)
        if (swrScoresPtr[i].obj_test_ptr == pod)
            return i;
    return -1;
}

static float dist_to_listener(const swrRace *pod) {
    const float dx = pod->transform.vD.x - swrSound_Position.x;
    const float dy = pod->transform.vD.y - swrSound_Position.y;
    const float dz = pod->transform.vD.z - swrSound_Position.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

// Once per frame: the followed pod always qualifies (it is the one allowed to drive the track
// ambience), then the nearest pods fill the remaining voices.
static void rank_pods() {
    if (frametotal == g_ranked_frame)
        return;
    g_ranked_frame = frametotal;

    float dist[MAX_RACERS];
    const int n = swrScoresPtr != NULL ? live_racers() : 0;
    for (int i = 0; i < MAX_RACERS; i++) {
        g_rich[i] = false;
        dist[i] = i < n && racer_live(i) ? dist_to_listener(swrScoresPtr[i].obj_test_ptr) : -1.0f;
    }

    int taken = 0;
    const int followed = director_FollowedSlot();
    if (followed >= 0 && followed < n && dist[followed] >= 0.0f) {
        g_rich[followed] = true;
        taken = 1;
    }
    while (taken < g_voices) {
        int best = -1;
        for (int i = 0; i < MAX_RACERS; i++)
            if (!g_rich[i] && dist[i] >= 0.0f && dist[i] <= g_max_dist &&
                (best < 0 || dist[i] < dist[best]))
                best = i;
        if (best < 0)
            break;
        g_rich[best] = true;
        taken++;
    }
    g_rich_now = taken;
}

// Track ambience is a property of the listener's position in the lap, not of every pod that makes
// engine noise. Only the pod the camera is on gets to advance it.
static void __cdecl swrSound_UpdateEngineAudio_delta(int planet, int track, float *lapProgress) {
    if (g_inside_slot >= 0 && g_inside_slot != director_FollowedSlot())
        return;
    hook_call_original((swrSound_UpdateEngineAudio_t) swrSound_UpdateEngineAudio_ADDR, planet, track,
                       lapProgress);
}

static void __cdecl swrRace_UpdateEngineSound_delta(swrRace *player) {
    if (!g_enabled || player == NULL || swrScoresPtr == NULL || player->score_ptr == NULL) {
        hook_call_original((swrRace_UpdateEngineSound_t) swrRace_UpdateEngineSound_ADDR, player);
        return;
    }
    // Pods the stock gate already sends down the rich path keep their own local slot.
    if ((player->flags0 & swrObjTest_FLAG0_LOCAL) != 0 ||
        (player->flags1 & swrObjTest_FLAG1_FORCE_GROUND) != 0) {
        hook_call_original((swrRace_UpdateEngineSound_t) swrRace_UpdateEngineSound_ADDR, player);
        return;
    }

    rank_pods();
    const int slot = slot_of(player);
    if (slot < 0 || !g_rich[slot]) {
        hook_call_original((swrRace_UpdateEngineSound_t) swrRace_UpdateEngineSound_ADDR, player);
        return;
    }

    const int saved_band = swrRace_engineGearBand[0];
    const float saved_brake = swrRace_engineBrakeTimer[0];
    const int saved_latch = swrRace_engineBoostLatch;

    swrRace_engineGearBand[0] = g_state[slot].band;
    swrRace_engineBrakeTimer[0] = g_state[slot].brake_timer;
    swrRace_engineBoostLatch = g_state[slot].boost_latch;
    {
        LocalSlotLend lend(player->score_ptr);
        g_inside_slot = slot;
        play_engine_sounds(player, g_volume);
        g_inside_slot = -1;
    }
    g_state[slot].band = swrRace_engineGearBand[0];
    g_state[slot].brake_timer = swrRace_engineBrakeTimer[0];
    g_state[slot].boost_latch = swrRace_engineBoostLatch;

    swrRace_engineGearBand[0] = saved_band;
    swrRace_engineBrakeTimer[0] = saved_brake;
    swrRace_engineBoostLatch = saved_latch;
}

void engine_audio_RegisterHooks() {
    hook_function("swrRace_UpdateEngineSound", (uint32_t) swrRace_UpdateEngineSound_ADDR,
                  (uint8_t *) swrRace_UpdateEngineSound_delta);
    hook_function("swrSound_UpdateEngineAudio", (uint32_t) swrSound_UpdateEngineAudio_ADDR,
                  (uint8_t *) swrSound_UpdateEngineAudio_delta);
}

// ---------------------------------------------------------------------------------------------
// Config + panel

static void load_config() {
    g_enabled = config::get_int(INI_SECTION, "enabled", g_enabled) != 0;
    g_voices = config::get_int(INI_SECTION, "voices", g_voices);
    g_volume = config::get_float(INI_SECTION, "volume", g_volume);
    g_max_dist = config::get_float(INI_SECTION, "max_distance", g_max_dist);
}

static void save_config() {
    config::set_bool(INI_SECTION, "enabled", g_enabled);
    config::set_int(INI_SECTION, "voices", g_voices);
    config::set_float(INI_SECTION, "volume", g_volume);
    config::set_float(INI_SECTION, "max_distance", g_max_dist);
    config::save();
}

static void panel_engine_audio() {
    bool changed = false;
    changed |= ImGui::Checkbox("Full engine audio for AI pods", &g_enabled);
    ImGui::TextDisabled("Gearbox, turbine, brakes and boost, pitched by each pod's own mass,");
    ImGui::TextDisabled("instead of the single distant drone the AI normally gets.");
    changed |= ImGui::SliderInt("Pods with the full engine", &g_voices, 1, MAX_RACERS);
    ImGui::TextDisabled("One loop id per gear band, so pods in the same band share a voice and the");
    ImGui::TextDisabled("last one each frame wins its pitch. Above 1 the engine warbles between");
    ImGui::TextDisabled("them; the followed pod always keeps the slot.");
    changed |= ImGui::SliderFloat("Volume", &g_volume, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Beyond this distance, back to the drone", &g_max_dist, 100.0f,
                                  5000.0f, "%.0f");
    ImGui::Separator();
    ImGui::Text("Full engine this frame: %d", g_rich_now);
    for (int i = 0; i < MAX_RACERS; i++) {
        if (!g_rich[i])
            continue;
        ImGui::Text("slot %d: band %d, %.0f away", i, g_state[i].band,
                    racer_live(i) ? dist_to_listener(swrScoresPtr[i].obj_test_ptr) : 0.0f);
    }
    if (changed)
        save_config();
}

static DebugPanel g_panel = {.category = "Race",
                             .name = "AI engine audio",
                             .draw = panel_engine_audio,
                             .dev_only = false};

void engine_audio_RegisterPanel() {
    load_config();
    for (int i = 0; i < MAX_RACERS; i++) {
        g_state[i].band = 0;
        g_state[i].brake_timer = 1.0f;
        g_state[i].boost_latch = 0;
        g_rich[i] = false;
    }
    debug_ui_register(&g_panel);
}
