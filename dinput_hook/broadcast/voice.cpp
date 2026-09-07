#include "voice.h"
#include "race_telemetry.h"
#include "../camera/director.h"
#include "../debug_ui.h"
#include "../config.h"
#include "../hook_helper.h"

#include <imgui.h>

#include <windows.h>
#include <cstdio>
#include <cstdlib>

extern "C" void hook_function(const char *function_name, uint32_t original_address,
                              uint8_t *hook_address);

extern "C" {
#include <Swr/swrObj.h>
#include <Swr/swrRace.h>
#include <Swr/swrSound.h>
#include <globals.h>
}

// ---------------------------------------------------------------------------------------------
// Settings ([voice])

static bool g_enabled = true;
static bool g_taunts = true;   // collision / pod-contact taunts (swrObjTest_F4 'Hitt' / 'VhLt')
static bool g_overtakes = true;// positional taunts (swrObjJdge_UpdateOvertakeSounds)
static bool g_status = true;   // death, respawn, engine fire, engine blown, boost
static bool g_finish = true;   // win / lose line + crowd
static bool g_announcer = true;// pre-race commentary for the showcased racers

static const char *INI_SECTION = "voice";

// Pilot voice = swrSound category 1, variant = pilot id, ids 1..0x25 (data/wavs/Voice/<xx>spNNN).
// Ids below are the ones the stock code passes at each site.
static const float HIT_TAUNT_CHANCE = 0.15f;// _DAT_004adc1c in swrObjTest_F4
static const float BOOST_TAUNT_SPLIT = 0.5f;// _DAT_004ad778 in swrRace_UpdatePlayerControl
static const int PILOT_NEVA_KEE = 14;

// Announcer (Fode & Beed) tables swrObjJdge_F0 indexes by the local pilot id: the intro line
// (category 5, after the generic category-5 id 1 lead-in) and a second line (positive: category
// 5, negative: category 7 with the sign dropped; 0 = none).
static const int16_t *ANNOUNCER_INTRO = (const int16_t *) 0x004c4a68;
static const int16_t *ANNOUNCER_ALT = (const int16_t *) 0x004c4a98;
static const int PILOT_COUNT = 23;

// ---------------------------------------------------------------------------------------------
// State

static int g_slot = -1;
static bool g_dead = false, g_fire = false, g_boosting = false, g_finished = false;
static int g_blown = 0;
static int g_lines = 0;

static float frand() {
    return (float) rand() / (float) RAND_MAX;
}

// swrSound_PlaySfxThrottled (the sink for every pilot / announcer line) returns before playing
// when there are pods but NumLocalPlayers() == 0, and that counts the firstLocalPlayer /
// secondLocalPlayer / thirdLocalPlayer pointers. Point the first at a score for the duration of a
// play call; nothing else on that path reads it.
struct LocalPlayerLend {
    swrScore *saved;
    LocalPlayerLend() : saved(firstLocalPlayer) {
        if (firstLocalPlayer == NULL && swrScoresPtr != NULL)
            firstLocalPlayer = swrScoresPtr;
    }
    ~LocalPlayerLend() {
        firstLocalPlayer = saved;
    }
};

static DWORD g_last_taunt_roll_ms = 0;
static const DWORD TAUNT_ROLL_GAP_MS = 1500;// 'Hitt' arrives every frame of a scrape; roll once per gap

static bool applies(const RaceTelemetry *t) {
    return g_enabled && t->valid && t->source == RACE_SOURCE_ALL_AI && firstLocalPlayer == NULL;
}

static swrRace *followed_pod() {
    const int slot = director_FollowedSlot();
    if (slot < 0 || swrScoresPtr == NULL)
        return NULL;
    return swrScoresPtr[slot].obj_test_ptr;
}

static int pilot_of(const swrRace *pod) {
    if (pod == NULL || pod->score_ptr == NULL || pod->score_ptr->pilotId == NULL)
        return -1;
    return *pod->score_ptr->pilotId;
}

static void say(swrRace *pod, int a, int b, int c, int d, int e, const char *why) {
    const int pilot = pilot_of(pod);
    if (pilot < 0)
        return;
    {
        LocalPlayerLend lend;
        swrSound_PlayRandomSfx(1, pilot, a, b, c, d, e, (rdVector3 *) &pod->transform.vD);
    }
    g_lines++;
    fprintf(hook_log, "[voice] pilot %d: %s\n", pilot, why);
    fflush(hook_log);
}

// ---------------------------------------------------------------------------------------------
// Hooks

// Collision taunts. swrObjTest_F4's 'Hitt' (wall / object) and 'VhLt' (pod contact) handlers roll
// a 15% taunt only for LOCAL pods within camera range; the followed pod is always in range.
typedef int(__cdecl *swrObjTest_F4_t)(swrRace *player, int *subEvent, int ghost);

static int __cdecl swrObjTest_F4_delta(swrRace *player, int *subEvent, int ghost) {
    const int r = hook_call_original((swrObjTest_F4_t) swrObjTest_F4_ADDR, player, subEvent, ghost);
    if (!g_taunts || player == NULL || subEvent == NULL)
        return r;
    const int id = subEvent[0];
    if (id != 'Hitt' && id != 'VhLt')
        return r;
    if ((player->flags0 & swrObjTest_FLAG0_LOCAL) != 0 || player != followed_pod() ||
        !applies(race_telemetry_Get()))
        return r;
    const DWORD now = GetTickCount();
    if (now - g_last_taunt_roll_ms < TAUNT_ROLL_GAP_MS)
        return r;
    g_last_taunt_roll_ms = now;
    if (frand() >= HIT_TAUNT_CHANCE)
        return r;
    if (id == 'VhLt')
        say(player, 1, 6, 5, 6, 5, "pod contact taunt");
    else
        say(player, 5, 6, 5, 6, 7, "collision taunt");
    return r;
}

// Positional taunts: the routine only looks at LOCAL scores (the passed / passing rival speaks),
// and everything else it does is the per-slot position memory. Lend LOCAL to the followed pod.
typedef void(__cdecl *swrObjJdge_UpdateOvertakeSounds_t)(swrObjJdge *jdge);

static void __cdecl swrObjJdge_UpdateOvertakeSounds_delta(swrObjJdge *jdge) {
    swrRace *pod = followed_pod();
    const bool lend = g_overtakes && pod != NULL &&
                      (pod->flags0 & swrObjTest_FLAG0_LOCAL) == 0 && applies(race_telemetry_Get());
    if (lend)
        pod->flags0 = (swrObjTest_FLAG0) (pod->flags0 | swrObjTest_FLAG0_LOCAL);
    {
        LocalPlayerLend players;
        hook_call_original(
            (swrObjJdge_UpdateOvertakeSounds_t) swrObjJdge_UpdateOvertakeSounds_ADDR, jdge);
    }
    if (lend)
        pod->flags0 = (swrObjTest_FLAG0) (pod->flags0 & ~swrObjTest_FLAG0_LOCAL);
}

void voice_RegisterHooks() {
    hook_function("swrObjTest_F4", (uint32_t) swrObjTest_F4_ADDR, (uint8_t *) swrObjTest_F4_delta);
    hook_function("swrObjJdge_UpdateOvertakeSounds",
                  (uint32_t) swrObjJdge_UpdateOvertakeSounds_ADDR,
                  (uint8_t *) swrObjJdge_UpdateOvertakeSounds_delta);
}

// ---------------------------------------------------------------------------------------------
// Telemetry edges for the followed racer

static const RaceTelemetryRow *row_for_slot(const RaceTelemetry *t, int slot) {
    for (int k = 0; k < t->n; k++)
        if (t->rows[k].slot == slot)
            return &t->rows[k];
    return NULL;
}

static int blown_engines(const RaceTelemetryRow *r) {
    int n = 0;
    for (int i = 0; i < 6; i++)
        if (r->engine_damage[i] >= 0.999f)
            n++;
    return n;
}

static void snapshot(const RaceTelemetryRow *r) {
    g_dead = r->dead;
    g_fire = r->on_fire;
    g_boosting = r->boosting;
    g_finished = r->finished;
    g_blown = blown_engines(r);
}

void voice_Service() {
    const RaceTelemetry *t = race_telemetry_Get();
    if (!applies(t) || t->judge_state != 1 && t->judge_state != 2) {
        g_slot = -1;
        return;
    }
    const int slot = director_FollowedSlot();
    const RaceTelemetryRow *r = slot >= 0 ? row_for_slot(t, slot) : NULL;
    swrRace *pod = slot >= 0 && swrScoresPtr != NULL ? swrScoresPtr[slot].obj_test_ptr : NULL;
    if (r == NULL || pod == NULL) {
        g_slot = -1;
        return;
    }
    if (slot != g_slot) {// new target: no lines for states it was already in
        g_slot = slot;
        snapshot(r);
        return;
    }
    const int pilot = pilot_of(pod);
    if (g_status) {
        if (r->dead && !g_dead)
            say(pod, 0xe, 0xe, 0xe, 0xe, 0xe, "death");// swrRace_HandleDeathExplosion
        else if (!r->dead && g_dead)
            say(pod, 0xe, 0xe, 0xe, 0xe, 0xe, "respawn");// swrRace_HandleRespawnFlag
        if (r->on_fire && !g_fire)
            say(pod, 0xc, 0xc, 0xc, 0xc, 0xc, "engine fire");// swrRace_ApplyEngineDamage
        const int blown = blown_engines(r);
        if (blown > g_blown && !r->dead)
            say(pod, 0xd, 0xe, 0xd, 0xe, 0xd, "engine blown");// swrRace_Explode
        if (r->boosting && !g_boosting) {// swrRace_UpdatePlayerControl boost start
            if (frand() < BOOST_TAUNT_SPLIT)
                say(pod, 0x15, 0x16, 0x17, 0x18, 0x19, "boost");
            else if (pilot == PILOT_NEVA_KEE)
                say(pod, 3, 0x12, 0x12, 0x13, 0x14, "boost");
            else
                say(pod, 3, 0x11, 0x12, 0x13, 0x14, "boost");
        }
    }
    if (g_finish && r->finished && !g_finished && pilot >= 0) {// swrObjJdge_F2 lap-complete
        LocalPlayerLend lend;
        if (r->rank == 1)
            swrSound_PlaySfxThenDelayed(1, pilot, 0xf, 6, 0, 0x27);
        else if (r->rank < 5)
            swrSound_PlaySfxThrottled(6, 0, 0x27, NULL);
        else
            swrSound_PlaySfxThenDelayed(1, pilot, 0x10, 6, 0, 0x27);
        g_lines++;
        fprintf(hook_log, "[voice] pilot %d: finished P%d\n", pilot, r->rank);
        fflush(hook_log);
    }
    snapshot(r);
}

// ---------------------------------------------------------------------------------------------
// Announcer

void voice_AnnounceRacer(int slot, int variant) {
    if (!g_enabled || !g_announcer || swrScoresPtr == NULL || slot < 0)
        return;
    const int pilot = pilot_of(swrScoresPtr[slot].obj_test_ptr);
    if (pilot < 0 || pilot >= PILOT_COUNT)
        return;
    const int alt = ANNOUNCER_ALT[pilot];
    LocalPlayerLend lend;
    if ((variant & 1) != 0 && alt != 0) {
        if (alt > 0)
            swrSound_PlaySfxThrottled(5, 0, alt, NULL);
        else
            swrSound_PlaySfxThrottled(7, 0, -alt, NULL);
    } else if (variant == 0) {
        swrSound_PlaySfxThenDelayed(5, 0, 1, 5, 0, ANNOUNCER_INTRO[pilot]);
    } else {
        swrSound_PlaySfxThrottled(5, 0, ANNOUNCER_INTRO[pilot], NULL);
    }
    g_lines++;
    fprintf(hook_log, "[voice] announcer: pilot %d (variant %d)\n", pilot, variant);
    fflush(hook_log);
}

// ---------------------------------------------------------------------------------------------
// Config + panel

static void load_config() {
    g_enabled = config::get_int(INI_SECTION, "enabled", g_enabled) != 0;
    g_taunts = config::get_int(INI_SECTION, "taunts", g_taunts) != 0;
    g_overtakes = config::get_int(INI_SECTION, "overtakes", g_overtakes) != 0;
    g_status = config::get_int(INI_SECTION, "status", g_status) != 0;
    g_finish = config::get_int(INI_SECTION, "finish", g_finish) != 0;
    g_announcer = config::get_int(INI_SECTION, "announcer", g_announcer) != 0;
}

static void save_config() {
    config::set_bool(INI_SECTION, "enabled", g_enabled);
    config::set_bool(INI_SECTION, "taunts", g_taunts);
    config::set_bool(INI_SECTION, "overtakes", g_overtakes);
    config::set_bool(INI_SECTION, "status", g_status);
    config::set_bool(INI_SECTION, "finish", g_finish);
    config::set_bool(INI_SECTION, "announcer", g_announcer);
    config::save();
}

static void panel_voice() {
    bool changed = false;
    changed |= ImGui::Checkbox("Voice lines for the followed racer (all-AI races)", &g_enabled);
    changed |= ImGui::Checkbox("Collision / contact taunts", &g_taunts);
    changed |= ImGui::Checkbox("Overtake taunts (rival speaks)", &g_overtakes);
    changed |= ImGui::Checkbox("Death, fire, engine blown, boost", &g_status);
    changed |= ImGui::Checkbox("Finish: win / lose + crowd", &g_finish);
    changed |= ImGui::Checkbox("Pre-race announcer for showcased racers", &g_announcer);
    ImGui::Text("Lines played: %d", g_lines);
    if (changed)
        save_config();
}

static DebugPanel g_panel = {.category = "Race",
                             .name = "Voice lines",
                             .draw = panel_voice,
                             .dev_only = false};

void voice_RegisterPanel() {
    load_config();
    debug_ui_register(&g_panel);
}
