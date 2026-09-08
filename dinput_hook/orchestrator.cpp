#include "orchestrator.h"
#include "hook_helper.h"
#include "debug_ui.h"
#include "patch.h"
#include "imgui_utils.h"
#include "config.h"
#include "game_deltas/tracks_delta.h"// swrUI_GetTrackNameFromId_delta
#include "broadcast/overlay.h"
#include "camera/director.h"
#include "broadcast/voice.h"

#include <imgui.h>

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>

extern "C" {
#include <Swr/swrObj.h>
#include <Swr/swrRace.h>
#include <Swr/swrSound.h>// swrSound_SetSfxFlag (stock announcer one-shot)
#include <swr.h>// playASound (binder ignition)
#include <Swr/swrEvent.h>
#include <Swr/swrMultiplayer.h>
#include <Swr/swrText.h>
#include <Swr/swrRender.h>
#include <globals.h>
void hook_function(const char *function_name, uint32_t original_address, uint8_t *hook_address);
}

// ---------------------------------------------------------------------------------------------
// Settings ([orchestrator] in SW_RACER_RE.ini)

static int g_laps = 1;
static int g_racers = 20;
static float g_cooldown_s =
    60.0f;// results / betting window: from the winner's finish to the next load
static float g_all_done_s = 5.0f;  // once the last racer is in, the window shrinks to this
static float g_grid_hold_s = 20.0f;// extra time on the pre-race grid view for bettors to pick
static bool g_rotate_tracks = true;
static bool g_unstick = true;
static float g_stuck_s = 8.0f;// no progress for this long -> snap the pod back onto the spline
static bool g_dnf = false;
static float g_dnf_s = 240.0f;    // after the winner, racers still out are marked finished
static bool g_full_physics = true;// keep every AI pod off the on-rails LOD path
static bool g_ai_damage = true;   // AI take fire damage and can explode like a human
static bool g_ai_repair =
    false;// let AI repair (off: fires burn until the engine blows -- more drama)
static float g_repair_start = 0.8f;     // damage on the worst segment before an AI bothers (0.8 = 20% health left)
static float g_repair_stop = 0.2f;      // repair until the worst engine is this clean
static float g_repair_turn_limit = 150.0f;// |turnRateTarget| below this counts as a straight
static bool g_ai_lighting = true;  // light AI pods from the followed pod's light bank
static int g_hero_count = 5;// grid hold: cut to this many random racers with announcer lines (0 = off)
static bool g_ignite = true;          // after the introductions: the field lights its energy binders
static float g_ignite_spread_s = 8.0f;// ... one pod after another over this long; the crowd
                                      // shot's pan runs with it and ends when the last one lights
static const int CFG_VERSION = 4;// bump when a default should override a stored value
static bool g_shuffle_grid = true; // random starting grid (stock: roster order, favourite up front)
static bool g_no_blue_flash = true;// keep the respawn light override off the shared AI light bank
static float g_snapshot_s = 20.0f; // periodic field snapshot to hook.log (0 = off)

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
static int g_menu_track =
    -1;// hangar track_index when armed; restored on disarm (menu indexes by circuit)
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
static DWORD g_grid_hold_start_ms = 0;// first frame of the pre-race orbit (state 5 before 'Go')
static bool g_fini_fired = false;
static DWORD g_last_snapshot_ms = 0;
static DWORD g_first_finish_ms = 0;
static const int MAX_RACERS = 20;
static int g_heroes_shown = 0;// grid showcase progress
static int g_countdown_beat = -1;// last 3-2-1 beat that got its own racer
static DWORD g_hero_next_ms = 0;
static DWORD g_hero_hold_until_ms = 0;// keep the grid until the last intro has finished
static bool g_hero_used[MAX_RACERS];
static DWORD g_ignite_start_ms = 0;// 0 = not yet
// The broadcast's music: the planet's arrival fanfare over the grid, the track theme from the
// green light. Both go through the streamed-music controller's queue.
enum MusicPhase { MUSIC_PHASE_NONE = 0, MUSIC_PHASE_GRID, MUSIC_PHASE_RACE };
static MusicPhase g_music_phase = MUSIC_PHASE_NONE;
static const int SHARED_AI_BANK =
    10;// the one light bank every AI pod reads (see apply_ai_lighting)
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
    g_grid_hold_start_ms = 0;
    g_heroes_shown = 0;
    g_countdown_beat = -1;
    g_hero_next_ms = 0;
    g_hero_hold_until_ms = 0;
    memset(g_hero_used, 0, sizeof(g_hero_used));
    g_ignite_start_ms = 0;
    g_music_phase = MUSIC_PHASE_NONE;
}

// Grid showcase: while the grid is held, cut to a few random racers in turn and play the
// announcer's pre-race lines for them (swrObjJdge_F0 does this once, for the local pilot only,
// 2 s into the pre-race orbit).
// Energy binders. swrRace_UpdateEnergyBinder shows a pod's binder beam once the global
// binder-ignition timer exceeds 0.1 s x the pod's entity id (a 0.1 s stagger across the field; the
// compare constant at 0x4adb48 is a double 0.01 on the squared value), and pins the timer to 1000
// once the pod is racing. The pre-race orbit advances that timer only while the camera-man follows
// a LOCAL pod (swrEvent_UpdateTimedSound_Maybe), after playing the ignition sound (sfx 0x74) --
// so an all-AI grid sat dark until the green light. Drive the timer ourselves after the
// introductions, on a wide drone, with the same sound.
static float *const g_binder_ignition_timer = (float *) 0x0050caf8;
static const int BINDER_IGNITION_SFX = 0x74;

// Music stings are not attempted here any more: the fanfares are megabyte-plus STREAMED bank
// entries, the engine has exactly one stream, and swrSound_UpdateMusic's arm step plays a freshly
// queued track at whatever gain the channel already holds before fading it down -- three rounds of
// that never produced an audible sting and silenced the race music. If revisited: stop the live
// music voice with swrSound_StopVoiceById first (parking the queue leaves the looping voice playing
// and holding the stream), or give the sting a non-streamed bank entry of its own.
//
// What IS needed is the arming the game does in swrObjJdge_F0's pre-race orbit (state 5), where it
// calls swrSound_SelectTrackMusic(planet, track, 0) to queue the track theme. Race TV holds the
// countdown and the orbit is skippable, so that select never ran; the judge's per-frame
// SetMusicFade(1) then arms an empty queue and the race runs silent. Do the select once per race.
// Queueing alone was not enough: swrSound_UpdateMusic's arm step plays the queued track at the
// gain the channel already holds (zero here, so playASoundImpl drops it) and then fades down toward
// SetMusicFade(0), which clears the queue again. So hold the gain up and re-arm every frame while
// the loop owns the screen -- the pattern swrObjJdge_F3 uses for a human race -- and kick the first
// play directly. loop = 1 throughout, so UpdateMusic's own play dedups onto that one voice (a
// loop = 0 start would not, and a second voice on a streamed entry fights the single stream).
static void sustain_track_music() {
    if (!g_armed || swrSound_queuedMusicId <= 0)
        return;
    swrSound_musicGain = 1.0f;
    swrSound_SetMusicFade(1);
}

// Free the one stream (swrSoundStream_file) before starting another streamed entry: parking the
// queue leaves the outgoing looping voice playing and holding it.
static void stop_music_voice(int sfx) {
    if (sfx > 0)
        swrSound_StopVoiceById(sfx);
}

static int g_sting_sfx = -1;// the grid fanfare's id, so its voice can be stopped at the green light

// Grid: the planet's arrival fanfare (swrMusicPlanetIntroTable, the sting the game plays when you
// touch down on a planet), ONCE. A one-shot voice with an empty controller queue: with nothing
// queued, swrSound_UpdateMusic has nothing to re-issue, so the sting is not looped or faded and the
// grid runs quiet after it finishes.
static void arm_music(const swrObjJdge *jdge, int state) {
    if (jdge == NULL)
        return;
    if (state == 0 || state == 5) {
        if (g_music_phase != MUSIC_PHASE_NONE)
            return;
        g_music_phase = MUSIC_PHASE_GRID;
        stop_music_voice(swrSound_queuedMusicId);
        swrSound_queuedMusicId = -1;
        swrSound_currentMusicId = -1;
        const int planet = std::clamp((int) jdge->planetId, 0, 7);
        g_sting_sfx = (int) swrMusicPlanetIntroTable[planet];
        if (g_sting_sfx > 0)
            playASound(g_sting_sfx, 7, 0.25f, 1.0f, 0);
        fprintf(hook_log, "[orchestrator] planet sting: music sfx 0x%x (once)\n", g_sting_sfx);
        fflush(hook_log);
        return;
    }
    if (g_music_phase == MUSIC_PHASE_RACE)
        return;
    g_music_phase = MUSIC_PHASE_RACE;
    stop_music_voice(g_sting_sfx);
    g_sting_sfx = -1;
    swrSound_SelectTrackMusic(jdge->planetId, jdge->planet_track_number, 0);
    const int sfx = swrSound_queuedMusicId;
    swrSound_musicGain = 1.0f;
    swrSound_SetMusicFade(1);
    if (sfx > 0)
        playASound(sfx, 7, 0.25f, 1.0f, 1);
    fprintf(hook_log, "[orchestrator] track theme: music sfx 0x%x\n", sfx);
    fflush(hook_log);
}

static const float BINDER_STAGGER_S = 0.1f;// per entity id

static void ignite_field(const swrObjJdge *jdge, DWORD now) {
    if (!g_ignite)
        return;
    const float total = BINDER_STAGGER_S * (float) (jdge->num_players + 1);// timer value that lights the last pod
    if (g_ignite_start_ms == 0) {
        g_ignite_start_ms = now;
        *g_binder_ignition_timer = 0.0f;
        g_hero_hold_until_ms = now + (DWORD) (g_ignite_spread_s * 1000.0f) + 1200;
        playASound(BINDER_IGNITION_SFX, 6, 0.25f, 0.5f, 0);
        int slot = -1;// trackside pan down the grid while the beams come up
        for (int i = jdge->num_players / 2; i < jdge->num_players && i < MAX_RACERS && slot < 0; i++)
            if (swrScoresPtr[i].obj_test_ptr != NULL)
                slot = i;
        if (slot >= 0 && director_IsEnabled()) {
            director_GridIgnition(slot, g_ignite_spread_s);
            overlay_SetHighlightSlot(slot);
        }
        fprintf(hook_log, "[orchestrator] race %d: binders igniting\n", g_races_started);
        fflush(hook_log);
        return;
    }
    if (*g_binder_ignition_timer < total + 1.0f) {
        const float rate = total / std::max(0.5f, g_ignite_spread_s);// timer units per second
        *g_binder_ignition_timer += (float) swrRace_deltaTimeSecs * rate;
    }
}

// 3-2-1: a different racer on each beat of the countdown.
static void countdown_cuts(const swrObjJdge *jdge) {
    if (!director_IsEnabled())
        return;
    const int beat = (int) ceilf(jdge->raceTimer_ms);
    if (beat == g_countdown_beat || beat < 1 || beat > 3)
        return;
    g_countdown_beat = beat;
    int candidates[MAX_RACERS], n = 0;
    const int current = director_FollowedSlot();
    for (int i = 0; i < jdge->num_players && i < MAX_RACERS; i++)
        if (i != current && swrScoresPtr[i].obj_test_ptr != NULL)
            candidates[n++] = i;
    if (n == 0)
        return;
    const int slot = candidates[rand() % n];
    director_Showcase(slot);
    overlay_SetHighlightSlot(slot);
}

static void showcase_heroes(const swrObjJdge *jdge, DWORD now) {
    if (g_hero_count <= 0 || g_heroes_shown >= g_hero_count || !director_IsEnabled())
        return;
    if (g_hero_next_ms == 0) {
        // Suppress the stock line for the stale local pilot id; it uses this one-shot flag.
        swrSound_SetSfxFlag(0, 0x200000);
        // Opening shot: a drone high over the grid pans down onto the pack; the introductions
        // start once it has landed.
        int slot = -1;
        for (int i = 0; i < jdge->num_players && i < MAX_RACERS && slot < 0; i++)
            if (swrScoresPtr[i].obj_test_ptr != NULL)
                slot = i;
        if (slot >= 0) {
            director_GridIntro(slot);
            overlay_SetHighlightSlot(slot);
        }
        g_hero_next_ms = now + (DWORD) (director_GridIntroSeconds() * 1000.0f) + 500;
        return;
    }
    if (now < g_hero_next_ms)
        return;
    int candidates[MAX_RACERS], n = 0;
    for (int i = 0; i < jdge->num_players && i < MAX_RACERS; i++)
        if (!g_hero_used[i] && swrScoresPtr[i].obj_test_ptr != NULL)
            candidates[n++] = i;
    if (n == 0)
        return;
    const int slot = candidates[rand() % n];
    g_hero_used[slot] = true;
    director_Showcase(slot);
    overlay_SetHighlightSlot(slot);
    const int line_ms = voice_AnnounceRacer(slot, g_heroes_shown);
    g_heroes_shown++;
    // next hero once this intro has played out (plus a beat); the grid waits for the last one
    const DWORD gap = std::max((DWORD) 3000, (DWORD) line_ms + 900);
    g_hero_next_ms = now + gap;
    g_hero_hold_until_ms = now + gap + 1500;
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
        if (!g_fini_fired) {
            // We did not end this race (pause-menu quit, or an accept press in state 2): the user is
            // leaving, so stop the loop instead of chaining straight into the next race.
            orchestrator_ToggleArmed();
            set_status("race %d ended by the user (%s); loop disarmed", g_races_started,
                       event == 'Fini' ? "Fini" : "Abrt");
            return r;
        }
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
        // swrRace_TakeDamage ADDS into engineHealth and clamps at 1.0, so despite the name the
        // field is accumulated damage: 0 pristine, 1 destroyed. `worst` is the sickest segment.
        worst = std::max(worst, pod->engineHealth[i]);
    }
    // Repair like a driver would: only once a segment is genuinely in trouble (g_repair_start, 0.8
    // damage = 20% health left), and only where the track allows a hand off the controls -- so the
    // AI waits for a straight and stops the moment it has to steer again or boosts. Once started it
    // works down to g_repair_stop.
    bool repairing = false;
    if (g_ai_repair) {
        const bool straight = fabsf(pod->turnRateTarget) < g_repair_turn_limit;
        const bool boosting = (pod->flags0 & swrObjTest_FLAG0_BOOSTING) != 0;
        repairing = (pod->flags0 & swrObjTest_FLAG0_REPAIRING) != 0;
        if (!repairing)
            repairing = straight && !boosting && worst > g_repair_start;
        else if (!straight || boosting || worst < g_repair_stop)
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

// Respawn flash. swrObjTest_F3 ends by painting the pod's own light bank blue while flags0 & 0x6000
// (respawn invincibility / spinout). Every AI borrows the followed pod's bank (apply_ai_lighting), so
// the followed pod's flash lit the whole field. Cache that bank while it is normal and put it back
// right after F3 has written the flash. The renderer reads bank + 1 (slot 0 is the default light).
static rdVector4 g_bank_color, g_bank_ambient;
static int g_bank_cached = -1;

typedef void(__cdecl *swrObjTest_F3_t)(swrRace *pod);

static void __cdecl swrObjTest_F3_delta(swrRace *pod) {
    hook_call_original((swrObjTest_F3_t) swrObjTest_F3_ADDR, pod);
    if (!g_armed || !g_no_blue_flash || pod == NULL || firstLocalPlayer != NULL)
        return;
    const int followed = director_FollowedSlot();
    if (followed < 0 || swrScoresPtr == NULL || swrScoresPtr[followed].obj_test_ptr != pod)
        return;
    const int slot = SHARED_AI_BANK + 1;// the renderer reads bank + 1
    if ((pod->flags0 & (swrObjTest_FLAG0_RESPAWN_INVINC | swrObjTest_FLAG0_DEAD)) == 0) {
        g_bank_color = lightColor1[slot];
        g_bank_ambient = lightAmbientColor[slot];
        g_bank_cached = slot;
    } else if (g_bank_cached == slot) {
        lightColor1[slot] = g_bank_color;
        lightAmbientColor[slot] = g_bank_ambient;
    }
}

static float *__cdecl SetLightColorsAndDirection2_delta(int a1, rdVector3 *ambient,
                                                        rdVector3 *color, rdVector3 *dir);

void orchestrator_RegisterHooks() {
    hook_function("swrObjJdge_SpawnRacers", (uint32_t) swrObjJdge_SpawnRacers_ADDR,
                  (uint8_t *) swrObjJdge_SpawnRacers_delta);
    hook_function("swrObjHang_F4", (uint32_t) swrObjHang_F4_ADDR, (uint8_t *) swrObjHang_F4_delta);
    hook_function("swrObjTest_F3", (uint32_t) swrObjTest_F3_ADDR, (uint8_t *) swrObjTest_F3_delta);
    hook_function("SetLightColorsAndDirection2", (uint32_t) SetLightColorsAndDirection2_ADDR,
                  (uint8_t *) SetLightColorsAndDirection2_delta);
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
// Every pod's bank index is its entity id (0..19) but the light arrays hold 13 slots; vanilla only
// ever lights local pods (ids 0/1). With the camera on any pod and AI respawning, ids >= 12 wrote past
// lightColor1 into the neighbouring arrays every frame. Route the followed pod's writes (camera-man
// terrain light, F3 respawn flash) to one fixed bank, drop everyone else's (nobody reads them), and
// bounds-guard the writer regardless.
static int followed_light_id() {
    const int followed = director_FollowedSlot();
    if (followed < 0 || swrScoresPtr == NULL || swrScoresPtr[followed].obj_test_ptr == NULL)
        return -1;
    return swrScoresPtr[followed].obj_test_ptr->current_light_index;
}

typedef float *(__cdecl *SetLightColorsAndDirection2_t)(int a1, rdVector3 *ambient,
                                                        rdVector3 *color, rdVector3 *dir);

static float *__cdecl SetLightColorsAndDirection2_delta(int a1, rdVector3 *ambient,
                                                        rdVector3 *color, rdVector3 *dir) {
    if (g_armed && firstLocalPlayer == NULL && a1 >= 0) {
        if (a1 != followed_light_id())
            return (float *) a1;// unread bank: skip (and never overflow)
        a1 = SHARED_AI_BANK;
    }
    if (a1 >= LIGHT_BANK_COUNT)
        return (float *) a1;// would write past the 13-entry light arrays
    return hook_call_original((SetLightColorsAndDirection2_t) SetLightColorsAndDirection2_ADDR, a1,
                              ambient, color, dir);
}

static void apply_ai_lighting(swrObjJdge *jdge) {
    const int bank = SHARED_AI_BANK;
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
// Sound-layer health, for the "audio dies after N races" hunt: bank bytes loaded, the 8 live
// mixer channels (id:state, -1 free, -2 pending) and the 8 request slots.
static void log_sound_health() {
    char line[512];
    int o = snprintf(line, sizeof(line), "[orchestrator] sound: loaded %d B, 3d %d, sfx vol %d, live",
                     swrSound_loadedBytes, Sound_enabled_3d, (int) sound_sfx_volume);
    for (int i = 0; i < 8 && o < (int) sizeof(line) - 24; i++)
        o += snprintf(line + o, sizeof(line) - o, " %d:%d", swrSound_voicesLive[i].id,
                      swrSound_voicesLive[i].activeSoundId);
    o += snprintf(line + o, sizeof(line) - o, " | req");
    for (int i = 0; i < 8 && o < (int) sizeof(line) - 24; i++)
        o += snprintf(line + o, sizeof(line) - o, " %d:%d", swrSound_voicesRequested[i].id,
                      swrSound_voicesRequested[i].activeSoundId);
    fprintf(hook_log, "%s\n", line);
    fflush(hook_log);
}

static void log_snapshot(swrObjJdge *jdge, DWORD now) {
    if (g_snapshot_s <= 0.0f || now - g_last_snapshot_ms < (DWORD) (g_snapshot_s * 1000.0f))
        return;
    g_last_snapshot_ms = now;
    log_sound_health();
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
    snprintf(title, sizeof(title), "RACE %d", g_races_started);
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
        arm_music(jdge, state);
        sustain_track_music();

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
        // Pre-race grid view: the judge orbits the grid in state 5 (before anyone has the racing
        // bit) for 9 s, then counts down. Hold that orbit for g_grid_hold_s more so bettors can
        // read the grid, by keeping its timer from expiring.
        bool any_racing = false;
        for (int i = 0; i < jdge->num_players && i < MAX_RACERS; i++)
            if ((swrScoresPtr[i].flag & 1) != 0)
                any_racing = true;
        // The orbit (state 5) may be skipped by the cutscene toggles, so the hold lives in the
        // countdown (state 0): its timer is pinned above the 3-2-1 light windows until the hold
        // elapses, then runs out normally.
        if ((state == 5 || state == 0) && !any_racing) {
            if (g_grid_hold_start_ms == 0)
                g_grid_hold_start_ms = now;
            const bool holding = now - g_grid_hold_start_ms < (DWORD) (g_grid_hold_s * 1000.0f) ||
                                 now < g_hero_hold_until_ms;
            if (holding && state == 0 && jdge->raceTimer_ms < 3.5f)
                jdge->raceTimer_ms = 3.5f;
            if (holding && state == 0) {
                showcase_heroes(jdge, now);
                const bool heroes_done = g_hero_count <= 0 || g_heroes_shown >= g_hero_count;
                if (heroes_done && (g_hero_next_ms == 0 || now >= g_hero_next_ms - 900))
                    ignite_field(jdge, now);
            } else if (state == 0) {
                countdown_cuts(jdge);
            }
        }
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
            // Everyone in (or state 2 = nobody relevant left): a short transition is enough.
            if (g_cooldown_active) {
                bool all_in = true;
                for (int i = 0; i < jdge->num_players && i < MAX_RACERS; i++)
                    if (racer_out_on_track(&swrScoresPtr[i]))
                        all_in = false;
                const DWORD soon = now + (DWORD) (g_all_done_s * 1000.0f);
                if ((all_in || state == 2) && g_cooldown_end_ms > soon)
                    g_cooldown_end_ms = soon;
            }
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
        swrObjHang *hang = get_hang();
        g_menu_track = hang != NULL ? hang->track_index : -1;
    } else {
        swrObjHang *hang = get_hang();
        if (hang != NULL) {
            hang->demo_mode = 0;
            // Our random picks span circuits; the hangar menu indexes the track within its current
            // circuit, so put the track it was on back before it redraws.
            if (g_menu_track >= 0)
                hang->track_index = (char) g_menu_track;
        }
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

static const char *INI_SECTION = "orchestrator";

static void load_config() {
    g_laps = std::clamp(config::get_int(INI_SECTION, "laps", g_laps), 1, 10);
    g_racers =
        std::clamp(config::get_int(INI_SECTION, "racers", g_racers), 1, 20);
    g_cooldown_s = config::get_float(INI_SECTION, "cooldown_s", g_cooldown_s);
    g_all_done_s = config::get_float(INI_SECTION, "all_done_s", g_all_done_s);
    g_grid_hold_s = config::get_float(INI_SECTION, "grid_hold_s", g_grid_hold_s);
    g_rotate_tracks =
        config::get_int(INI_SECTION, "rotate_tracks", g_rotate_tracks) != 0;
    g_unstick = config::get_int(INI_SECTION, "unstick", g_unstick) != 0;
    g_stuck_s = config::get_float(INI_SECTION, "stuck_s", g_stuck_s);
    g_dnf = config::get_int(INI_SECTION, "dnf", g_dnf) != 0;
    g_dnf_s = config::get_float(INI_SECTION, "dnf_s", g_dnf_s);
    g_full_physics = config::get_int(INI_SECTION, "full_physics", g_full_physics) != 0;
    g_ai_damage = config::get_int(INI_SECTION, "ai_damage", g_ai_damage) != 0;
    g_ai_repair = config::get_int(INI_SECTION, "ai_repair", g_ai_repair) != 0;
    g_repair_stop = config::get_float(INI_SECTION, "repair_stop", g_repair_stop);
    g_ai_lighting = config::get_int(INI_SECTION, "ai_lighting", g_ai_lighting) != 0;
    const int stored_version = config::get_int(INI_SECTION, "cfg_version", 1);
    if (stored_version >= 2)// v2: 5 heroes
        g_hero_count = config::get_int(INI_SECTION, "hero_count", g_hero_count);
    g_repair_start = config::get_float(INI_SECTION, "repair_start", g_repair_start);
    g_repair_turn_limit = config::get_float(INI_SECTION, "repair_turn_limit", g_repair_turn_limit);
    g_ignite = config::get_int(INI_SECTION, "ignite", g_ignite) != 0;
    if (stored_version >= CFG_VERSION)// v4: one duration for the beams and the crowd pan
        g_ignite_spread_s = config::get_float(INI_SECTION, "ignite_spread_s", g_ignite_spread_s);
    g_shuffle_grid = config::get_int(INI_SECTION, "shuffle_grid", g_shuffle_grid) != 0;
    g_no_blue_flash =
        config::get_int(INI_SECTION, "no_blue_flash", g_no_blue_flash) != 0;
    g_snapshot_s = config::get_float(INI_SECTION, "snapshot_s", g_snapshot_s);
}

static void save_config() {
    config::set_int(INI_SECTION, "laps", g_laps);
    config::set_int(INI_SECTION, "racers", g_racers);
    config::set_float(INI_SECTION, "cooldown_s", g_cooldown_s);
    config::set_float(INI_SECTION, "all_done_s", g_all_done_s);
    config::set_float(INI_SECTION, "grid_hold_s", g_grid_hold_s);
    config::set_int(INI_SECTION, "rotate_tracks", g_rotate_tracks);
    config::set_int(INI_SECTION, "unstick", g_unstick);
    config::set_float(INI_SECTION, "stuck_s", g_stuck_s);
    config::set_int(INI_SECTION, "dnf", g_dnf);
    config::set_float(INI_SECTION, "dnf_s", g_dnf_s);
    config::set_int(INI_SECTION, "full_physics", g_full_physics);
    config::set_int(INI_SECTION, "ai_damage", g_ai_damage);
    config::set_int(INI_SECTION, "ai_repair", g_ai_repair);
    config::set_float(INI_SECTION, "repair_start", g_repair_start);
    config::set_float(INI_SECTION, "repair_stop", g_repair_stop);
    config::set_float(INI_SECTION, "repair_turn_limit", g_repair_turn_limit);
    config::set_int(INI_SECTION, "ignite", g_ignite);
    config::set_float(INI_SECTION, "ignite_spread_s", g_ignite_spread_s);
    config::set_int(INI_SECTION, "ai_lighting", g_ai_lighting);
    config::set_int(INI_SECTION, "hero_count", g_hero_count);
    config::set_int(INI_SECTION, "cfg_version", CFG_VERSION);
    config::set_int(INI_SECTION, "shuffle_grid", g_shuffle_grid);
    config::set_int(INI_SECTION, "no_blue_flash", g_no_blue_flash);
    config::set_float(INI_SECTION, "snapshot_s", g_snapshot_s);
    config::save();
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
    changed |= ImGui::SliderFloat("Transition once everyone is in (s)", &g_all_done_s, 0.0f, 60.0f,
                                  "%.0f");
    changed |= ImGui::SliderFloat("Extra grid time before the start (s)", &g_grid_hold_s, 0.0f,
                                  120.0f, "%.0f");
    changed |= ImGui::Checkbox("Random track (no repeat in last 10)", &g_rotate_tracks);
    changed |= ImGui::Checkbox("Full physics for all AI (no on-rails LOD)", &g_full_physics);
    changed |= ImGui::Checkbox("AI engine damage: fires burn, engines explode", &g_ai_damage);
    changed |= ImGui::Checkbox("AI may repair", &g_ai_repair);
    if (g_ai_repair) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        changed |= ImGui::SliderFloat("until##rep", &g_repair_stop, 0.0f, 0.5f, "%.2f");
        changed |= ImGui::SliderFloat("Damage before repairing (0.8 = 20% health)", &g_repair_start, 0.3f, 0.98f, "%.2f");
        changed |= ImGui::SliderFloat("Straightaway (max |turn rate| to repair)", &g_repair_turn_limit, 20.0f, 400.0f, "%.0f");
    }
    changed |= ImGui::Checkbox("Light AI pods from the followed pod's light bank", &g_ai_lighting);
    changed |= ImGui::SliderInt("Grid showcase: racers introduced by the announcer", &g_hero_count, 0, 10);
    changed |= ImGui::Checkbox("Grid: the field ignites its binders after the introductions", &g_ignite);
    changed |= ImGui::SliderFloat("Ignition + pan duration (s)", &g_ignite_spread_s, 1.0f, 20.0f, "%.1f");
    if (ImGui::Button("Reset sound channels (if audio has died)")) {
        log_sound_health();
        swrSound_ResetRequestedVoices();
        swrSound_ResetChannels();
        fprintf(hook_log, "[orchestrator] sound channels reset by the user\n");
        fflush(hook_log);
    }
    changed |= ImGui::Checkbox("Random starting grid", &g_shuffle_grid);
    changed |= ImGui::Checkbox("No respawn blue flash on the shared AI lighting", &g_no_blue_flash);
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
