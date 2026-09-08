#include "director.h"
#include "../broadcast/race_telemetry.h"
#include "../broadcast/overlay.h"
#include "../debug_ui.h"
#include "../imgui_utils.h"
#include "../config.h"
#include "../hook_helper.h"
#include "player_camera.h"

#include <imgui.h>

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

extern "C" {
#include <Swr/swrObj.h>
#include <Swr/swrRace.h>
#include <Swr/swrEvent.h>
#include <Swr/swrModel.h>// BuildLookAtTransform
#include <Swr/swrSpline.h>// trackside: walk the pod's spline cursor ahead
#include <globals.h>
}

// ---------------------------------------------------------------------------------------------
// Settings ([director])

static bool g_auto = true;
static float g_dwell_s = 12.0f;      // seconds on a target before auto considers a cut
static float g_min_shot_s = 6.0f;    // no shot is cut short of this (target cuts, occlusion), dead/finished aside
static float g_manual_hold_s = 45.0f;// a manual pick holds this long before auto resumes
static int g_w_leader = 3, g_w_battle = 3, g_w_random = 1;// target pick weights
static float g_battle_gap_s = 1.5f;                       // two racers this close are a "battle"
// Shot mix (weights) + parameters. The stock spectator-mode cycling is disabled while active.
static int g_w_chase = 1, g_w_drone = 6, g_w_orbit = 4, g_w_cockpit = 1;
static int g_w_trackside = 3;
static int g_w_face = 3, g_w_chase_far = 2, g_w_bumper = 1, g_w_fp_wide = 1;
static float g_face_dist = 30.0f;  // face cam: ahead of the pod
static float g_face_height = 7.0f; // ... and up
static float g_face_side = 4.0f;   // ... and a touch to one side (random sign)
static float g_face_max_s = 7.0f;  // face / bumper / fp-wide shots are short like cockpit
static float g_trackside_ahead = 750.0f;// plant the camera this far along the spline ahead of the pod
static float g_trackside_side = 70.0f;  // beside the spline (random side)
static float g_trackside_height = 25.0f;
static float g_trackside_past = 450.0f; // release once the pod is this far past the camera
static float g_trackside_max_s = 22.0f; // or after this long (pod stalled / went the other way)
static float g_trackside_aim_smooth = 0.15f;
static float g_trackside_zoom = 0.45f;// FOV multiplier at plant distance (1 = no zoom); eases to 1 as the pod arrives
static float g_trackside_zoom_near = 90.0f;// fully zoomed out by the time the pod is this close
static float g_drone_height = 95.0f; // world units above the pod
static const int CFG_VERSION = 8;    // bump when a default should override a stored value
static float g_drone_back = 85.0f;   // behind the pod along its horizontal heading
static float g_drone_ahead = 80.0f;  // aim point ahead of the pod
static float g_drone_smooth = 0.5f;  // position time constant (s)
static float g_drone_blend_s = 1.6f; // drone -> drone cut: fly to the new pod over this long
static float g_drone_blend_tau = 0.55f;
static float g_drone_blend_max = 2500.0f;// further than this and the drone cuts instead of flying
static float g_orbit_dist = 180.0f;  // rival within this many world units -> orbit shot possible
static float g_orbit_rate = 0.35f;   // orbit sweep rate (rad/s)
static float g_orbit_sweep = 0.6f;// sweep amplitude (rad) either side of the "away from rival" line
static float g_orbit_radius = 60.0f;// camera distance from the followed pod
static float g_orbit_height = 18.0f;
static float g_orbit_smooth = 0.4f;
static float g_cockpit_max_s = 8.0f;// cockpit shots are short
static float g_occlusion_s = 1.2f;  // free camera blocked by the track this long -> back to chase
static float g_intro_height = 380.0f;// grid intro: the drone starts this far above its normal height
static float g_intro_s = 4.5f;       // ... and descends onto the grid over this long
static float g_finish_lock_s = 10.0f;// cut to the leader this long before the win and hold through it

// ---------------------------------------------------------------------------------------------
// State

static bool g_enabled = false;
static int g_target_slot = -1;
static DWORD g_last_cut_ms = 0;
static DWORD g_manual_until_ms = 0;
static const char *g_last_rule = "";
static int g_cuts = 0;

enum Shot {
    SHOT_CHASE = 0,
    SHOT_DRONE,
    SHOT_ORBIT,
    SHOT_COCKPIT,
    SHOT_TRACKSIDE,
    SHOT_INTRO,
    SHOT_FACE,     // rigid, in front of the pod looking back at the pilot
    SHOT_CHASE_FAR,// stock views
    SHOT_BUMPER,
    SHOT_FP_WIDE,
};
static const char *SHOT_NAMES[] = {"chase", "drone",  "orbit",     "cockpit", "trackside",
                                   "intro", "face",   "chase far", "bumper",  "fp wide"};

// The game view a shot runs on (our override shots ride on chase near).
static int shot_stock_mode(Shot s) {
    switch (s) {
        case SHOT_CHASE_FAR:
            return 2;
        case SHOT_BUMPER:
            return 4;
        case SHOT_FP_WIDE:
            return 5;
        default:
            return 1;
    }
}
static Shot g_shot = SHOT_CHASE;
static DWORD g_shot_start_ms = 0;
static bool g_cam_seeded = false;
static DWORD g_dead_since_ms = 0;      // followed racer died at (0 = alive)
static const DWORD DEAD_HOLD_MS = 1200;// show the wreck for a beat, then cut
static rdVector3 g_cam_pos, g_cam_aim;
static int g_orbit_rival = -1;
static float g_orbit_ang = 0.0f;// eased, unwrapped camera angle around the pair
static bool g_orbit_ang_seeded = false;
static rdVector3 g_trackside_pos;// fixed camera position
static rdVector3 g_trackside_fwd;// spline tangent at the camera (pass-by test)
static bool g_trackside_planted = false;
static DWORD g_blend_until_ms = 0;// drone -> drone: the camera flies rather than cuts until then
static DWORD g_blocked_since_ms = 0;// free-camera line of sight to the pod lost at (0 = clear)
static DWORD g_win_seen_ms = 0;     // the winner crossed the line at (0 = not yet)
static DWORD g_intro_start_ms = 0;
static float g_face_sign = 1.0f;
static float g_fov_eased = 0.0f;// trackside zoom state (0 = unseeded)

static swrObjcMan *camera_man() {
    return (swrObjcMan *) swrEvent_FindObjectById('cMan', 0);
}

int director_FollowedSlot() {
    swrObjcMan *cman = camera_man();
    if (cman == NULL || cman->unkf4_objTest == NULL || swrScoresPtr == NULL)
        return -1;
    const RaceTelemetry *t = race_telemetry_Get();
    for (int k = 0; k < t->n; k++)
        if (swrScoresPtr[t->rows[k].slot].obj_test_ptr == cman->unkf4_objTest)
            return t->rows[k].slot;
    return -1;
}

static const RaceTelemetryRow *row_for_slot(const RaceTelemetry *t, int slot) {
    for (int k = 0; k < t->n; k++)
        if (t->rows[k].slot == slot)
            return &t->rows[k];
    return NULL;
}

static bool followable(const RaceTelemetryRow &r) {
    return !r.finished && !r.dead;
}

static float pod_distance(int a, int b) {
    const swrRace *pa = swrScoresPtr[a].obj_test_ptr;
    const swrRace *pb = swrScoresPtr[b].obj_test_ptr;
    if (pa == NULL || pb == NULL)
        return 1e9f;
    const float dx = pa->transform.vD.x - pb->transform.vD.x;
    const float dy = pa->transform.vD.y - pb->transform.vD.y;
    const float dz = pa->transform.vD.z - pb->transform.vD.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

// Line of sight from the free camera to the pod against the track collision model. Ray record as
// swrRace_RaycastGround builds it: origin, unit direction, max length; RaycastModel returns the hit
// distance or -1. The facing filter is off so back faces (tunnel roofs, cliff undersides) count.
static bool view_blocked(const swrRace *pod) {
    if (!g_cam_seeded || pod->collisionModel == NULL)
        return false;
    float ray[7] = {g_cam_pos.x, g_cam_pos.y, g_cam_pos.z, 0.0f, 0.0f, 0.0f, 0.0f};
    const float dx = pod->transform.vD.x - g_cam_pos.x, dy = pod->transform.vD.y - g_cam_pos.y,
                dz = pod->transform.vD.z + 3.0f - g_cam_pos.z;
    const float len = sqrtf(dx * dx + dy * dy + dz * dz);
    if (len < 10.0f)
        return false;
    ray[3] = dx / len;
    ray[4] = dy / len;
    ray[5] = dz / len;
    ray[6] = len - 6.0f;// stop short of the pod itself
    rdVector3 hit, normal;
    // declared in swrObj.h but not reimplemented: call the game's routine by address
    typedef void(__cdecl * SetRaycastIgnoreFacing_t)(int);
    const SetRaycastIgnoreFacing_t set_ignore_facing =
        (SetRaycastIgnoreFacing_t) swrModel_SetRaycastIgnoreFacing_ADDR;
    set_ignore_facing(1);
    const float d = swrRace_RaycastModel(pod->collisionModel, ray, &hit, &normal);
    set_ignore_facing(0);
    return d >= 0.0f;
}

// Nearest racer to `slot` in the world (ahead or behind) within `max_dist`, or -1. Progress gaps
// in seconds put pods a corner apart in "range"; the orbit needs them in the same shot.
static int nearest_rival(const RaceTelemetry *t, int slot, float max_dist) {
    if (swrScoresPtr == NULL || row_for_slot(t, slot) == NULL)
        return -1;
    int best = -1;
    float best_d = 1e9f;
    for (int k = 0; k < t->n; k++) {
        const RaceTelemetryRow &r = t->rows[k];
        if (r.slot == slot || !followable(r))
            continue;
        const float d = pod_distance(slot, r.slot);
        if (d < best_d) {
            best_d = d;
            best = r.slot;
        }
    }
    return best_d <= max_dist ? best : -1;
}

static void set_shot(Shot s) {
    if (g_shot == SHOT_COCKPIT && s != SHOT_COCKPIT)
        playercam_SetExternalCockpit(false);
    // Drone to drone (a new target while already airborne): keep the eased camera state and let
    // it fly across to the new pod instead of snapping.
    bool fly = g_shot == SHOT_DRONE && s == SHOT_DRONE && g_cam_seeded;
    if (fly && swrScoresPtr != NULL && g_target_slot >= 0) {
        const swrRace *pod = swrScoresPtr[g_target_slot].obj_test_ptr;
        const float dx = pod != NULL ? pod->transform.vD.x - g_cam_pos.x : 0.0f;
        const float dy = pod != NULL ? pod->transform.vD.y - g_cam_pos.y : 0.0f;
        const float dz = pod != NULL ? pod->transform.vD.z - g_cam_pos.z : 0.0f;
        fly = pod != NULL && sqrtf(dx * dx + dy * dy + dz * dz) <= g_drone_blend_max;
    }
    g_shot = s;
    g_shot_start_ms = GetTickCount();
    g_cam_seeded = fly;
    g_blend_until_ms = fly ? g_shot_start_ms + (DWORD) (g_drone_blend_s * 1000.0f) : 0;
    g_orbit_ang_seeded = false;
    g_trackside_planted = false;
    g_blocked_since_ms = 0;
    g_face_sign = (rand() & 1) ? 1.0f : -1.0f;
    g_fov_eased = 0.0f;
    if (s == SHOT_COCKPIT)
        playercam_SetExternalCockpit(true);
    swrObjcMan *cman = camera_man();
    if (cman != NULL && cman->mode_type != 8 && cman->mode_type != 9 &&
        cman->mode_type != shot_stock_mode(s))
        playercam_SetStockMode(cman, shot_stock_mode(s));
}

// Roll a shot for the new target; orbit only when a rival is in range.
static Shot pick_shot(const RaceTelemetry *t, int slot) {
    g_orbit_rival = nearest_rival(t, slot, g_orbit_dist);
    const int wo = g_orbit_rival >= 0 ? g_w_orbit : 0;
    const int total = g_w_chase + g_w_drone + wo + g_w_cockpit + g_w_trackside + g_w_face +
                      g_w_chase_far + g_w_bumper + g_w_fp_wide;
    if (total <= 0)
        return SHOT_CHASE;
    int roll = rand() % total;
    if (roll < g_w_chase)
        return SHOT_CHASE;
    roll -= g_w_chase;
    if (roll < g_w_drone)
        return SHOT_DRONE;
    roll -= g_w_drone;
    if (roll < wo)
        return SHOT_ORBIT;
    roll -= wo;
    if (roll < g_w_cockpit)
        return SHOT_COCKPIT;
    roll -= g_w_cockpit;
    if (roll < g_w_trackside)
        return SHOT_TRACKSIDE;
    roll -= g_w_trackside;
    if (roll < g_w_face)
        return SHOT_FACE;
    roll -= g_w_face;
    if (roll < g_w_chase_far)
        return SHOT_CHASE_FAR;
    roll -= g_w_chase_far;
    if (roll < g_w_bumper)
        return SHOT_BUMPER;
    return SHOT_FP_WIDE;
}

// A shot that ran its course (trackside pass-by, cockpit timer, rival gone, view blocked) hands
// over to a different shot on the same target rather than always falling back to the stock chase.
static void next_shot(const RaceTelemetry *t) {
    const Shot prev = g_shot;
    Shot s = pick_shot(t, g_target_slot);
    if (s == prev)
        s = prev == SHOT_DRONE ? SHOT_CHASE : SHOT_DRONE;
    set_shot(s);
}

// The cut: same payload swrObjHang_AssignRacerCameras sends at spawn ({'NAsn', camIndex, pod}).
static void cut_to(int slot, const char *rule) {
    swrObjcMan *cman = camera_man();
    if (cman == NULL || swrScoresPtr == NULL || slot < 0 || slot >= RACE_TELEMETRY_MAX_ROWS)
        return;
    swrRace *pod = swrScoresPtr[slot].obj_test_ptr;
    if (pod == NULL)
        return;
    if (pod != cman->unkf4_objTest) {
        int ev[3] = {'NAsn', cman->metaCamIndex_count, (int) pod};
        swrEvent_DispatchSubEvents(cman, ev);
    }
    g_target_slot = slot;
    g_last_cut_ms = GetTickCount();
    g_last_rule = rule;
    g_dead_since_ms = 0;
    g_cuts++;
    const RaceTelemetry *t = race_telemetry_Get();
    set_shot(strcmp(rule, "manual") == 0 || strcmp(rule, "showcase") == 0 ? SHOT_CHASE
                                                                          : pick_shot(t, slot));
    const RaceTelemetryRow *r = row_for_slot(t, slot);
    fprintf(hook_log, "[director] cut -> slot %d (%s) by %s, %s shot\n", slot, r ? r->name : "?",
            rule, SHOT_NAMES[g_shot]);
    fflush(hook_log);
}

// Auto pick among racers still racing: leader / closest battle (the chaser) / random, weighted.
static int pick_auto(const RaceTelemetry *t, const char **rule) {
    int leader = -1, battle = -1, count = 0;
    int pool[RACE_TELEMETRY_MAX_ROWS];
    float best_gap = 1e9f;
    int prev = -1;
    for (int k = 0; k < t->n; k++) {
        const RaceTelemetryRow &r = t->rows[k];
        if (!followable(r))
            continue;
        pool[count++] = r.slot;
        if (leader < 0)
            leader = r.slot;
        if (prev >= 0) {
            const RaceTelemetryRow *p = row_for_slot(t, prev);
            if (p != NULL && r.gap_leader_s >= 0.0f && p->gap_leader_s >= 0.0f) {
                const float gap = r.gap_leader_s - p->gap_leader_s;
                if (gap < best_gap && gap <= g_battle_gap_s) {
                    best_gap = gap;
                    battle = r.slot;// the chaser: the overtake attempt is in front of its camera
                }
            }
        }
        prev = r.slot;
    }
    if (count == 0)
        return -1;
    const int wl = leader >= 0 ? g_w_leader : 0;
    const int wb = battle >= 0 ? g_w_battle : 0;
    const int wr = count > 1 ? g_w_random : 0;
    const int total = wl + wb + wr;
    if (total == 0) {
        *rule = "leader";
        return leader;
    }
    int roll = rand() % total;
    if (roll < wl) {
        *rule = "leader";
        return leader;
    }
    roll -= wl;
    if (roll < wb) {
        *rule = "battle";
        return battle;
    }
    *rule = "random";
    for (int tries = 0; tries < 8; tries++) {
        const int s = pool[rand() % count];
        if (s != g_target_slot || count == 1)
            return s;
    }
    return pool[0];
}

void director_Service() {
    if (!g_enabled)
        return;
    const RaceTelemetry *t = race_telemetry_Get();
    if (!t->valid || t->n == 0 || t->source != RACE_SOURCE_ALL_AI)
        return;
    if (t->judge_state >= 3)// post-race sequence / results: the game's own cameras
        return;
    const DWORD now = GetTickCount();
    swrObjcMan *cman = camera_man();

    // Our shots replace the stock spectator cycle: swrObjcMan_UpdateCamera counts
    // swrObjcMan_spectatorCycleTimer down whenever the followed pod is not LOCAL and then picks a
    // random entry of swrObjcMan_SpectatorCamModes (chase / first-person / the static spline camera
    // that stares at wherever the pod was). Keep the countdown from ever expiring, and hold the
    // shot's own mode -- on the grid as well, where that cycle was the "orbit looking the wrong
    // way" between showcase cuts.
    swrObjcMan_spectatorCycleTimer = 1.0e9f;
    const int want_mode = shot_stock_mode(g_shot);
    if (cman != NULL && cman->mode_type != want_mode && cman->mode_type != 8 &&
        cman->mode_type != 9)
        playercam_SetStockMode(cman, want_mode);// 8/9 = death camera, left alone
    if (t->judge_state == 0)// countdown: the orchestrator's showcase owns the cuts
        return;

    const int followed = director_FollowedSlot();
    if (followed != g_target_slot && followed >= 0)
        g_target_slot = followed;// something else (respawn, spawn) moved the camera

    const RaceTelemetryRow *cur = row_for_slot(t, g_target_slot);
    const bool manual_hold = now < g_manual_until_ms;
    if (g_shot == SHOT_INTRO)
        set_shot(SHOT_DRONE);

    // The win is never off screen: once the leader is within g_finish_lock_s of the line, cut to
    // it and hold every other cut until a beat after it has crossed.
    bool finish_lock = false;
    if (!t->leader_finished) {
        g_win_seen_ms = 0;
        const RaceTelemetryRow *lead = t->n > 0 ? &t->rows[0] : NULL;
        if (lead != NULL && !lead->finished && !lead->dead && t->leader_pace_lps > 0.0f) {
            const float remaining_s = ((float) t->num_laps - lead->progress) / t->leader_pace_lps;
            if (remaining_s <= g_finish_lock_s) {
                finish_lock = true;
                if (lead->slot != g_target_slot) {
                    cut_to(lead->slot, "finish");
                    cur = row_for_slot(t, g_target_slot);
                }
            }
        }
    } else {
        if (g_win_seen_ms == 0)
            g_win_seen_ms = now;
        finish_lock = cur != NULL && cur->finished && now - g_win_seen_ms < 3000;
    }

    // A followed racer who finishes stays on screen (the finish is the shot) and is simply never
    // picked again. A dead racer is shown for a beat, then cut away from.
    if (cur != NULL && cur->dead) {
        if (g_dead_since_ms == 0)
            g_dead_since_ms = now;
    } else {
        g_dead_since_ms = 0;
    }
    const bool must_cut =
        cur == NULL || (g_dead_since_ms != 0 && now - g_dead_since_ms >= DEAD_HOLD_MS);
    const bool shot_young = now - g_shot_start_ms < (DWORD) (g_min_shot_s * 1000.0f);
    const bool dwell_over = now - g_last_cut_ms >= (DWORD) (g_dwell_s * 1000.0f) && !shot_young;

    // Shot upkeep: orbit needs its rival in range; cockpit shots are short; a free camera that
    // loses sight of the pod behind the track for a while drops back to the chase view.
    if (!must_cut) {
        // (a trackside camera planted ahead often has terrain between it and the approaching pod,
        // so the occlusion cut also waits out the minimum shot length)
        if (g_shot == SHOT_DRONE || g_shot == SHOT_ORBIT || g_shot == SHOT_TRACKSIDE) {
            const swrRace *pod = swrScoresPtr[g_target_slot].obj_test_ptr;
            if (pod != NULL && view_blocked(pod)) {
                if (g_blocked_since_ms == 0)
                    g_blocked_since_ms = now;
                else if (!shot_young &&
                         now - g_blocked_since_ms > (DWORD) (g_occlusion_s * 1000.0f)) {
                    fprintf(hook_log, "[director] %s shot blocked by the track\n",
                            SHOT_NAMES[g_shot]);
                    fflush(hook_log);
                    next_shot(t);
                }
            } else {
                g_blocked_since_ms = 0;
            }
        }
        if (g_shot == SHOT_ORBIT) {
            const RaceTelemetryRow *rv = row_for_slot(t, g_orbit_rival);
            const bool keep = rv != NULL && followable(*rv) &&
                              pod_distance(g_target_slot, g_orbit_rival) <= g_orbit_dist * 1.5f;
            if (!keep) {
                const int rival = nearest_rival(t, g_target_slot, g_orbit_dist);
                if (rival < 0)
                    next_shot(t);
                else
                    g_orbit_rival = rival;
            }
        } else if (g_shot == SHOT_COCKPIT &&
                   now - g_shot_start_ms > (DWORD) (g_cockpit_max_s * 1000.0f)) {
            next_shot(t);
        } else if ((g_shot == SHOT_FACE || g_shot == SHOT_BUMPER || g_shot == SHOT_FP_WIDE) &&
                   now - g_shot_start_ms > (DWORD) (g_face_max_s * 1000.0f)) {
            next_shot(t);
        } else if (g_shot == SHOT_TRACKSIDE && g_trackside_planted) {
            const swrRace *pod = swrScoresPtr[g_target_slot].obj_test_ptr;
            const float dx = pod != NULL ? pod->transform.vD.x - g_trackside_pos.x : 0.0f;
            const float dy = pod != NULL ? pod->transform.vD.y - g_trackside_pos.y : 0.0f;
            const float along = dx * g_trackside_fwd.x + dy * g_trackside_fwd.y;
            if (pod == NULL || along > g_trackside_past ||
                now - g_shot_start_ms > (DWORD) (g_trackside_max_s * 1000.0f))
                next_shot(t);
        }
    }

    if (!must_cut && (finish_lock || manual_hold || !g_auto || !dwell_over))
        return;
    const char *rule = "";
    const int next = pick_auto(t, &rule);
    if (next >= 0 && next != g_target_slot)
        cut_to(next, must_cut ? "cut-away" : rule);
    else if (must_cut)
        g_last_cut_ms = now;// nothing better to show; re-check after a dwell
}

void director_Showcase(int slot) {
    cut_to(slot, "showcase");
}

float director_GridIntroSeconds() {
    return g_intro_s;
}

void director_GridIntro(int slot) {
    cut_to(slot, "showcase");
    set_shot(SHOT_INTRO);
    g_intro_start_ms = GetTickCount();
}

void director_FollowSlot(int slot) {
    if (slot < 0) {
        g_manual_until_ms = 0;
        return;
    }
    cut_to(slot, "manual");
    g_manual_until_ms = GetTickCount() + (DWORD) (g_manual_hold_s * 1000.0f);
}

// ---------------------------------------------------------------------------------------------
// Camera shots (run right after swrObjcMan_UpdateCamera for the camera-man following our target)

static void ease_to(const rdVector3 &want_pos, const rdVector3 &want_aim, float tau) {
    if (!g_cam_seeded) {
        g_cam_pos = want_pos;
        g_cam_aim = want_aim;
        g_cam_seeded = true;
        return;
    }
    const float dt = (float) swrRace_deltaTimeSecs;
    const float a = tau > 0.0f ? 1.0f - expf(-dt / tau) : 1.0f;
    const float b = std::min(1.0f, a * 2.0f);
    g_cam_pos.x += (want_pos.x - g_cam_pos.x) * a;
    g_cam_pos.y += (want_pos.y - g_cam_pos.y) * a;
    g_cam_pos.z += (want_pos.z - g_cam_pos.z) * a;
    g_cam_aim.x += (want_aim.x - g_cam_aim.x) * b;
    g_cam_aim.y += (want_aim.y - g_cam_aim.y) * b;
    g_cam_aim.z += (want_aim.z - g_cam_aim.z) * b;
}

static void write_camera(swrObjcMan *cman) {
    swrTranslationRotation tr;
    rdMatrix44 out;
    rdVector3 from = g_cam_pos, to = g_cam_aim;
    BuildLookAtTransform(&from, &to, &out, &tr, 0.0f);
    cman->unk20_mat = out;
    cman->focusTransform_mat.vD.x = to.x;
    cman->focusTransform_mat.vD.y = to.y;
    cman->focusTransform_mat.vD.z = to.z;
}

static void heading_xy(const swrRace *pod, float *fx, float *fy) {
    *fx = pod->transform.vB.x;
    *fy = pod->transform.vB.y;
    const float l = sqrtf(*fx * *fx + *fy * *fy);
    if (l < 1e-3f) {
        *fx = 0.0f;
        *fy = 1.0f;
    } else {
        *fx /= l;
        *fy /= l;
    }
}

// Drone: high, behind along the heading, aimed ahead.
static void shot_drone(swrObjcMan *cman, const swrRace *pod) {
    float fx, fy;
    heading_xy(pod, &fx, &fy);
    const rdVector3 p = {pod->transform.vD.x, pod->transform.vD.y, pod->transform.vD.z};
    const rdVector3 want_pos = {p.x - fx * g_drone_back, p.y - fy * g_drone_back,
                                p.z + g_drone_height};
    const rdVector3 want_aim = {p.x + fx * g_drone_ahead, p.y + fy * g_drone_ahead, p.z};
    const bool flying = g_blend_until_ms != 0 && GetTickCount() < g_blend_until_ms;
    ease_to(want_pos, want_aim, flying ? std::max(g_drone_smooth, g_drone_blend_tau) : g_drone_smooth);
    write_camera(cman);
}

// Orbit: ride with the followed pod and watch the rival. The camera sits at a fixed radius from
// the pod on the side away from the rival (so the pod is in the foreground and the rival beyond
// it), slowly sweeping around that line, and aims between the two, weighted toward the rival.
// The position is rigid in the pod's frame every frame -- easing a world position at race speed
// left the old version trailing hundreds of units behind and staring at empty track; only the
// angle and the aim are eased.
static void shot_orbit(swrObjcMan *cman, const swrRace *pod, const swrRace *rival) {
    const rdVector3 p = {pod->transform.vD.x, pod->transform.vD.y, pod->transform.vD.z};
    const rdVector3 q = {rival->transform.vD.x, rival->transform.vD.y, rival->transform.vD.z};
    float dx = p.x - q.x, dy = p.y - q.y;
    const float sep = sqrtf(dx * dx + dy * dy);
    if (sep < 1e-3f) {
        heading_xy(pod, &dx, &dy);
        dx = -dx;
        dy = -dy;
    } else {
        dx /= sep;
        dy /= sep;
    }
    const float t = (GetTickCount() - g_shot_start_ms) / 1000.0f;
    const float phase = g_orbit_sweep * sinf(t * g_orbit_rate);
    const float want_ang = atan2f(dy, dx) + phase;

    // The pair line flips 180 degrees when the pods swap order: ease the angle itself along the
    // shortest arc so the camera swings around rather than jumping.
    const float dt = (float) swrRace_deltaTimeSecs;
    const float a = g_orbit_smooth > 0.0f ? 1.0f - expf(-dt / g_orbit_smooth) : 1.0f;
    if (!g_orbit_ang_seeded) {
        g_orbit_ang = want_ang;
        g_orbit_ang_seeded = true;
    } else {
        float d = want_ang - g_orbit_ang;
        while (d > 3.14159265f)
            d -= 6.2831853f;
        while (d < -3.14159265f)
            d += 6.2831853f;
        g_orbit_ang += d * a;
    }
    const rdVector3 pos = {p.x + cosf(g_orbit_ang) * g_orbit_radius,
                           p.y + sinf(g_orbit_ang) * g_orbit_radius, p.z + g_orbit_height};
    const rdVector3 want_aim = {p.x * 0.35f + q.x * 0.65f, p.y * 0.35f + q.y * 0.65f,
                                p.z * 0.35f + q.z * 0.65f + 2.0f};
    ease_to(pos, want_aim, 0.12f);
    g_cam_pos = pos;// rigid to the pod; only the aim eases
    write_camera(cman);
}

// Face cam: rigid in the pod's frame, just ahead and above, looking back at the cockpit so the
// pilot fills the frame. Rigid like the orbit: no positional easing at race speed.
static void shot_face(swrObjcMan *cman, const swrRace *pod) {
    const rdVector3 f = {pod->transform.vB.x, pod->transform.vB.y, pod->transform.vB.z};
    const rdVector3 u = {pod->transform.vC.x, pod->transform.vC.y, pod->transform.vC.z};
    const rdVector3 r = {pod->transform.vA.x, pod->transform.vA.y, pod->transform.vA.z};
    const float side = g_face_side * g_face_sign;
    const rdVector3 c = {pod->cockpitXf.vD.x, pod->cockpitXf.vD.y, pod->cockpitXf.vD.z};
    const rdVector3 pos = {c.x + f.x * g_face_dist + u.x * g_face_height + r.x * side,
                           c.y + f.y * g_face_dist + u.y * g_face_height + r.y * side,
                           c.z + f.z * g_face_dist + u.z * g_face_height + r.z * side};
    const rdVector3 aim = {c.x + u.x * 1.5f, c.y + u.y * 1.5f, c.z + u.z * 1.5f};
    ease_to(pos, aim, 0.0f);
    write_camera(cman);
}

// Grid intro: the first shot of a race. A drone high above the grid pans down onto the pack over
// g_intro_s, then holds the normal drone framing until the showcase cuts take over.
static void shot_intro(swrObjcMan *cman, const swrRace *pod) {
    float fx, fy;
    heading_xy(pod, &fx, &fy);
    const float t = std::min(1.0f, (GetTickCount() - g_intro_start_ms) / (g_intro_s * 1000.0f));
    const float ease = 1.0f - (1.0f - t) * (1.0f - t);// ease-out
    const float extra = g_intro_height * (1.0f - ease);
    const rdVector3 p = {pod->transform.vD.x, pod->transform.vD.y, pod->transform.vD.z};
    const rdVector3 pos = {p.x - fx * (g_drone_back + extra * 0.35f), p.y - fy * (g_drone_back + extra * 0.35f),
                           p.z + g_drone_height + extra};
    const rdVector3 aim = {p.x + fx * g_drone_ahead * 0.5f, p.y + fy * g_drone_ahead * 0.5f, p.z};
    ease_to(pos, aim, 0.0f);
    write_camera(cman);
}

// Trackside: walk a copy of the pod's spline cursor forward until it is g_trackside_ahead world
// units away, plant the camera beside that point (spline right vector, random side) and leave it
// there; only the aim follows the pod. Released by the upkeep once the pod is past the camera.
static bool plant_trackside(const swrRace *pod) {
    swrSplineCursor c = pod->splineCursor;
    if (c.spline == NULL) {
        fprintf(hook_log, "[director] trackside: pod has no spline cursor\n");
        fflush(hook_log);
        return false;
    }
    c.velocity = 0.0f;
    rdMatrix44 m;
    const rdVector3 start = {pod->transform.vD.x, pod->transform.vD.y, pod->transform.vD.z};
    for (int i = 0; i < 400; i++) {
        c.segmentT += 0.05f;
        if (c.segmentT > 1.0f) {
            c.segmentT -= 1.0f;
            c.endFlag = 0;
            swrSpline_CursorStep(c.spline, 1, &c);
        }
        swrSpline_EvaluateToMatrix(&c, &m);
        const float dx = m.vD.x - start.x, dy = m.vD.y - start.y, dz = m.vD.z - start.z;
        if (sqrtf(dx * dx + dy * dy + dz * dz) >= g_trackside_ahead)
            break;
    }
    const float side = (rand() & 1) ? g_trackside_side : -g_trackside_side;
    g_trackside_pos = {m.vD.x + m.vA.x * side + m.vC.x * g_trackside_height,
                       m.vD.y + m.vA.y * side + m.vC.y * g_trackside_height,
                       m.vD.z + m.vA.z * side + m.vC.z * g_trackside_height};
    g_trackside_fwd = {m.vB.x, m.vB.y, 0.0f};
    const float l = sqrtf(g_trackside_fwd.x * g_trackside_fwd.x + g_trackside_fwd.y * g_trackside_fwd.y);
    if (l > 1e-3f) {
        g_trackside_fwd.x /= l;
        g_trackside_fwd.y /= l;
    }
    g_trackside_planted = true;
    {
        const float dx = g_trackside_pos.x - start.x, dy = g_trackside_pos.y - start.y,
                    dz = g_trackside_pos.z - start.z;
        fprintf(hook_log, "[director] trackside planted %.0f units from the pod (side %+.0f)\n",
                sqrtf(dx * dx + dy * dy + dz * dz), side);
        fflush(hook_log);
    }
    return true;
}

static void shot_trackside(swrObjcMan *cman, const swrRace *pod) {
    if (!g_trackside_planted && !plant_trackside(pod)) {
        set_shot(SHOT_CHASE);
        return;
    }
    const rdVector3 want_aim = {pod->transform.vD.x, pod->transform.vD.y, pod->transform.vD.z};
    ease_to(g_trackside_pos, want_aim, g_trackside_aim_smooth);
    g_cam_pos = g_trackside_pos;// the position never eases: a tripod, not a drone
    write_camera(cman);
}

// Trackside zoom: a long lens while the pod is far up the road, pulling out to the normal FOV as
// it arrives, like a trackside operator following the approach.
static float fov_override(swrObjcMan *cman, float fov) {
    if (!g_enabled || g_shot != SHOT_TRACKSIDE || !g_trackside_planted || cman == NULL)
        return fov;
    const swrRace *pod = cman->unkf4_objTest;
    if (pod == NULL || g_trackside_zoom >= 1.0f)
        return fov;
    const float dx = pod->transform.vD.x - g_trackside_pos.x;
    const float dy = pod->transform.vD.y - g_trackside_pos.y;
    const float dz = pod->transform.vD.z - g_trackside_pos.z;
    const float d = sqrtf(dx * dx + dy * dy + dz * dz);
    const float span = std::max(1.0f, g_trackside_ahead - g_trackside_zoom_near);
    const float t = std::clamp((d - g_trackside_zoom_near) / span, 0.0f, 1.0f);
    const float want = fov * (1.0f - t * (1.0f - g_trackside_zoom));
    const float dt = (float) swrRace_deltaTimeSecs;
    const float a = 1.0f - expf(-dt / 0.25f);
    g_fov_eased = g_fov_eased <= 0.0f ? want : g_fov_eased + (want - g_fov_eased) * a;
    return g_fov_eased;
}

static bool camera_override(swrObjcMan *cman) {
    if (!g_enabled || cman == NULL || g_shot == SHOT_CHASE)
        return false;
    swrRace *pod = cman->unkf4_objTest;
    if (pod == NULL || swrScoresPtr == NULL || g_target_slot < 0 ||
        swrScoresPtr[g_target_slot].obj_test_ptr != pod)
        return false;
    const RaceTelemetry *t = race_telemetry_Get();
    if (!t->valid || t->source != RACE_SOURCE_ALL_AI)
        return false;
    switch (g_shot) {
        case SHOT_DRONE:
            shot_drone(cman, pod);
            return true;
        case SHOT_ORBIT: {
            swrRace *rival = g_orbit_rival >= 0 ? swrScoresPtr[g_orbit_rival].obj_test_ptr : NULL;
            if (rival == NULL)
                return false;
            shot_orbit(cman, pod, rival);
            return true;
        }
        case SHOT_COCKPIT:
            if (pod->partNodes == NULL)
                return false;
            playercam_ApplyTrueCockpit(cman, pod);
            return true;
        case SHOT_TRACKSIDE:
            shot_trackside(cman, pod);
            return true;
        case SHOT_INTRO:
            shot_intro(cman, pod);
            return true;
        case SHOT_FACE:
            if (pod->partNodes == NULL)
                return false;
            shot_face(cman, pod);
            return true;
        default:
            return false;
    }
}

void director_SetEnabled(bool on) {
    if (on == g_enabled)
        return;
    g_enabled = on;
    g_manual_until_ms = 0;
    set_shot(SHOT_CHASE);
    overlay_SuppressNameplates(false);
    overlay_SetRowClickHandler(on ? director_FollowSlot : NULL);
    playercam_SetCameraOverride(on ? camera_override : NULL);
    playercam_SetFovOverride(on ? fov_override : NULL);
    if (!on)
        overlay_SetHighlightSlot(-1);
}

bool director_IsEnabled() {
    return g_enabled;
}

// ---------------------------------------------------------------------------------------------
// Config + panel

static const char *INI_SECTION = "director";

static void load_config() {
    g_auto = config::get_int(INI_SECTION, "auto", g_auto) != 0;
    g_dwell_s = config::get_float(INI_SECTION, "dwell_s", g_dwell_s);
    g_manual_hold_s = config::get_float(INI_SECTION, "manual_hold_s", g_manual_hold_s);
    g_w_leader = config::get_int(INI_SECTION, "w_leader", g_w_leader);
    g_w_battle = config::get_int(INI_SECTION, "w_battle", g_w_battle);
    g_w_random = config::get_int(INI_SECTION, "w_random", g_w_random);
    g_battle_gap_s = config::get_float(INI_SECTION, "battle_gap_s", g_battle_gap_s);
    g_w_drone = config::get_int(INI_SECTION, "shot_drone", g_w_drone);
    g_w_cockpit = config::get_int(INI_SECTION, "shot_cockpit", g_w_cockpit);
    g_w_trackside = config::get_int(INI_SECTION, "shot_trackside", g_w_trackside);
    g_w_face = config::get_int(INI_SECTION, "shot_face", g_w_face);
    g_w_chase_far = config::get_int(INI_SECTION, "shot_chase_far", g_w_chase_far);
    g_w_bumper = config::get_int(INI_SECTION, "shot_bumper", g_w_bumper);
    g_w_fp_wide = config::get_int(INI_SECTION, "shot_fp_wide", g_w_fp_wide);
    g_face_dist = config::get_float(INI_SECTION, "face_dist", g_face_dist);
    g_face_height = config::get_float(INI_SECTION, "face_height", g_face_height);
    g_face_side = config::get_float(INI_SECTION, "face_side", g_face_side);
    g_face_max_s = config::get_float(INI_SECTION, "face_max_s", g_face_max_s);
    g_trackside_side = config::get_float(INI_SECTION, "trackside_side", g_trackside_side);
    g_trackside_zoom = config::get_float(INI_SECTION, "trackside_zoom", g_trackside_zoom);
    g_trackside_zoom_near = config::get_float(INI_SECTION, "trackside_zoom_near", g_trackside_zoom_near);
    g_trackside_height = config::get_float(INI_SECTION, "trackside_height", g_trackside_height);
    const int stored_version = config::get_int(INI_SECTION, "cfg_version", 1);
    if (stored_version >= 3) {// v3: further / longer trackside defaults
        g_trackside_ahead = config::get_float(INI_SECTION, "trackside_ahead", g_trackside_ahead);
        g_trackside_past = config::get_float(INI_SECTION, "trackside_past", g_trackside_past);
        g_trackside_max_s = config::get_float(INI_SECTION, "trackside_max_s", g_trackside_max_s);
    }
    if (stored_version >= 4) {// v4: pod-anchored orbit, narrower sweep
        g_orbit_sweep = config::get_float(INI_SECTION, "orbit_sweep", g_orbit_sweep);
        g_orbit_radius = config::get_float(INI_SECTION, "orbit_radius", g_orbit_radius);
        g_orbit_height = config::get_float(INI_SECTION, "orbit_height", g_orbit_height);
    }
    if (stored_version >= 5) {// v5: closer drone, wider orbit range, occlusion cut
        g_orbit_dist = config::get_float(INI_SECTION, "orbit_dist", g_orbit_dist);
        g_occlusion_s = config::get_float(INI_SECTION, "occlusion_s", g_occlusion_s);
    }
    g_finish_lock_s = config::get_float(INI_SECTION, "finish_lock_s", g_finish_lock_s);
    g_intro_height = config::get_float(INI_SECTION, "intro_height", g_intro_height);
    g_intro_s = config::get_float(INI_SECTION, "intro_s", g_intro_s);
    if (stored_version >= 6) {// v6: orbit weight 2 -> 4
        g_w_orbit = config::get_int(INI_SECTION, "shot_orbit", g_w_orbit);
    }
    if (stored_version >= 7) {// v7: chase weight 2 -> 1
        g_w_chase = config::get_int(INI_SECTION, "shot_chase", g_w_chase);
    }
    if (stored_version >= CFG_VERSION) {// v8: closer drone, fly-over cap halved
        g_drone_height = config::get_float(INI_SECTION, "drone_height", g_drone_height);
        g_drone_back = config::get_float(INI_SECTION, "drone_back", g_drone_back);
        g_drone_blend_max = config::get_float(INI_SECTION, "drone_blend_max", g_drone_blend_max);
    }
    g_min_shot_s = config::get_float(INI_SECTION, "min_shot_s", g_min_shot_s);
    g_drone_blend_s = config::get_float(INI_SECTION, "drone_blend_s", g_drone_blend_s);
    g_drone_blend_tau = config::get_float(INI_SECTION, "drone_blend_tau", g_drone_blend_tau);
    g_drone_ahead = config::get_float(INI_SECTION, "drone_ahead", g_drone_ahead);
    g_drone_smooth = config::get_float(INI_SECTION, "drone_smooth", g_drone_smooth);
    g_orbit_rate = config::get_float(INI_SECTION, "orbit_rate", g_orbit_rate);
    g_orbit_smooth = config::get_float(INI_SECTION, "orbit_smooth", g_orbit_smooth);
    g_cockpit_max_s = config::get_float(INI_SECTION, "cockpit_max_s", g_cockpit_max_s);
}

static void save_config() {
    config::set_int(INI_SECTION, "cfg_version", CFG_VERSION);
    config::set_int(INI_SECTION, "auto", g_auto);
    config::set_float(INI_SECTION, "dwell_s", g_dwell_s);
    config::set_float(INI_SECTION, "min_shot_s", g_min_shot_s);
    config::set_float(INI_SECTION, "manual_hold_s", g_manual_hold_s);
    config::set_int(INI_SECTION, "w_leader", g_w_leader);
    config::set_int(INI_SECTION, "w_battle", g_w_battle);
    config::set_int(INI_SECTION, "w_random", g_w_random);
    config::set_float(INI_SECTION, "battle_gap_s", g_battle_gap_s);
    config::set_int(INI_SECTION, "shot_chase", g_w_chase);
    config::set_int(INI_SECTION, "shot_drone", g_w_drone);
    config::set_int(INI_SECTION, "shot_orbit", g_w_orbit);
    config::set_int(INI_SECTION, "shot_cockpit", g_w_cockpit);
    config::set_int(INI_SECTION, "shot_trackside", g_w_trackside);
    config::set_int(INI_SECTION, "shot_face", g_w_face);
    config::set_int(INI_SECTION, "shot_chase_far", g_w_chase_far);
    config::set_int(INI_SECTION, "shot_bumper", g_w_bumper);
    config::set_int(INI_SECTION, "shot_fp_wide", g_w_fp_wide);
    config::set_float(INI_SECTION, "face_dist", g_face_dist);
    config::set_float(INI_SECTION, "face_height", g_face_height);
    config::set_float(INI_SECTION, "face_side", g_face_side);
    config::set_float(INI_SECTION, "face_max_s", g_face_max_s);
    config::set_float(INI_SECTION, "trackside_ahead", g_trackside_ahead);
    config::set_float(INI_SECTION, "trackside_side", g_trackside_side);
    config::set_float(INI_SECTION, "trackside_zoom", g_trackside_zoom);
    config::set_float(INI_SECTION, "trackside_zoom_near", g_trackside_zoom_near);
    config::set_float(INI_SECTION, "trackside_height", g_trackside_height);
    config::set_float(INI_SECTION, "trackside_past", g_trackside_past);
    config::set_float(INI_SECTION, "trackside_max_s", g_trackside_max_s);
    config::set_float(INI_SECTION, "drone_height", g_drone_height);
    config::set_float(INI_SECTION, "drone_back", g_drone_back);
    config::set_float(INI_SECTION, "drone_ahead", g_drone_ahead);
    config::set_float(INI_SECTION, "drone_smooth", g_drone_smooth);
    config::set_float(INI_SECTION, "drone_blend_s", g_drone_blend_s);
    config::set_float(INI_SECTION, "drone_blend_tau", g_drone_blend_tau);
    config::set_float(INI_SECTION, "drone_blend_max", g_drone_blend_max);
    config::set_float(INI_SECTION, "orbit_dist", g_orbit_dist);
    config::set_float(INI_SECTION, "orbit_rate", g_orbit_rate);
    config::set_float(INI_SECTION, "orbit_sweep", g_orbit_sweep);
    config::set_float(INI_SECTION, "orbit_radius", g_orbit_radius);
    config::set_float(INI_SECTION, "orbit_height", g_orbit_height);
    config::set_float(INI_SECTION, "orbit_smooth", g_orbit_smooth);
    config::set_float(INI_SECTION, "cockpit_max_s", g_cockpit_max_s);
    config::set_float(INI_SECTION, "occlusion_s", g_occlusion_s);
    config::set_float(INI_SECTION, "finish_lock_s", g_finish_lock_s);
    config::set_float(INI_SECTION, "intro_height", g_intro_height);
    config::set_float(INI_SECTION, "intro_s", g_intro_s);
    config::save();
}

static void panel_director() {
    ImGui::TextWrapped(
        "Picks which pod the camera follows in an all-AI race and which shot to use. "
        "Click a name on the leaderboard to follow that racer; auto resumes after the "
        "hold.");
    ImGui::Text("Director: %s", g_enabled ? "enabled (by the orchestrator)" : "idle");
    bool changed = false;
    changed |= ImGui::Checkbox("Auto director", &g_auto);
    changed |= ImGui::SliderFloat("Dwell (s)", &g_dwell_s, 3.0f, 60.0f, "%.0f");
    changed |= ImGui::SliderFloat("Minimum shot length (s)", &g_min_shot_s, 0.0f, 20.0f, "%.0f");
    changed |= ImGui::SliderFloat("Manual hold (s)", &g_manual_hold_s, 5.0f, 300.0f, "%.0f");
    ImGui::SeparatorText("Target pick weights");
    changed |= ImGui::SliderInt("Leader", &g_w_leader, 0, 10);
    changed |= ImGui::SliderInt("Battle", &g_w_battle, 0, 10);
    changed |= ImGui::SliderInt("Random", &g_w_random, 0, 10);
    changed |= ImGui::SliderFloat("Battle gap (s)", &g_battle_gap_s, 0.2f, 5.0f, "%.1f");
    ImGui::SeparatorText("Shot mix weights");
    changed |= ImGui::SliderInt("Chase", &g_w_chase, 0, 10);
    changed |= ImGui::SliderInt("Drone", &g_w_drone, 0, 10);
    changed |= ImGui::SliderInt("Orbit (needs a rival in range)", &g_w_orbit, 0, 10);
    changed |= ImGui::SliderInt("Cockpit", &g_w_cockpit, 0, 10);
    changed |= ImGui::SliderInt("Trackside", &g_w_trackside, 0, 10);
    changed |= ImGui::SliderInt("Face (pilot close-up)", &g_w_face, 0, 10);
    changed |= ImGui::SliderInt("Chase far (stock)", &g_w_chase_far, 0, 10);
    changed |= ImGui::SliderInt("Bumper (stock first person)", &g_w_bumper, 0, 10);
    changed |= ImGui::SliderInt("First person wide (stock)", &g_w_fp_wide, 0, 10);
    changed |= ImGui::SliderFloat("Cut when the track blocks the view for (s)", &g_occlusion_s, 0.2f, 5.0f, "%.1f");
    changed |= ImGui::SliderFloat("Cut to the leader this long before the win (s)", &g_finish_lock_s, 0.0f, 30.0f, "%.0f");
    ImGui::SeparatorText("Face cam");
    changed |= ImGui::SliderFloat("Ahead of the cockpit", &g_face_dist, 8.0f, 120.0f, "%.0f");
    changed |= ImGui::SliderFloat("Above", &g_face_height, -10.0f, 40.0f, "%.0f");
    changed |= ImGui::SliderFloat("To the side", &g_face_side, 0.0f, 40.0f, "%.0f");
    changed |= ImGui::SliderFloat("Max duration (s)##face", &g_face_max_s, 2.0f, 30.0f, "%.0f");
    ImGui::SeparatorText("Grid intro");
    changed |= ImGui::SliderFloat("Start height above the drone view", &g_intro_height, 0.0f, 1500.0f, "%.0f");
    changed |= ImGui::SliderFloat("Descent (s)", &g_intro_s, 1.0f, 15.0f, "%.1f");
    ImGui::SeparatorText("Drone");
    changed |= ImGui::SliderFloat("Height", &g_drone_height, 20.0f, 800.0f, "%.0f");
    changed |= ImGui::SliderFloat("Behind", &g_drone_back, 0.0f, 600.0f, "%.0f");
    changed |= ImGui::SliderFloat("Aim ahead", &g_drone_ahead, 0.0f, 400.0f, "%.0f");
    changed |= ImGui::SliderFloat("Smoothing (s)##drone", &g_drone_smooth, 0.0f, 2.0f, "%.2f");
    changed |= ImGui::SliderFloat("Fly-over on drone-to-drone cuts (s)", &g_drone_blend_s, 0.0f, 5.0f, "%.1f");
    changed |= ImGui::SliderFloat("Fly-over smoothing (s)", &g_drone_blend_tau, 0.1f, 2.0f, "%.2f");
    changed |= ImGui::SliderFloat("Fly-over max distance", &g_drone_blend_max, 200.0f, 20000.0f, "%.0f");
    ImGui::SeparatorText("Orbit");
    changed |=
        ImGui::SliderFloat("Rival within (world units)", &g_orbit_dist, 20.0f, 400.0f, "%.0f");
    changed |= ImGui::SliderFloat("Sweep rate (rad/s)", &g_orbit_rate, 0.0f, 1.5f, "%.2f");
    changed |= ImGui::SliderFloat("Sweep amplitude (rad)", &g_orbit_sweep, 0.0f, 2.5f, "%.2f");
    changed |= ImGui::SliderFloat("Distance from pod", &g_orbit_radius, 15.0f, 200.0f, "%.0f");
    changed |= ImGui::SliderFloat("Height##orbit", &g_orbit_height, 0.0f, 120.0f, "%.0f");
    changed |= ImGui::SliderFloat("Smoothing (s)##orbit", &g_orbit_smooth, 0.0f, 2.0f, "%.2f");
    ImGui::SeparatorText("Cockpit");
    changed |= ImGui::SliderFloat("Max duration (s)", &g_cockpit_max_s, 2.0f, 30.0f, "%.0f");
    ImGui::SeparatorText("Trackside");
    changed |= ImGui::SliderFloat("Plant ahead (world units)", &g_trackside_ahead, 50.0f, 2000.0f, "%.0f");
    changed |= ImGui::SliderFloat("Beside the track", &g_trackside_side, 0.0f, 300.0f, "%.0f");
    changed |= ImGui::SliderFloat("Zoom at plant distance (FOV x)", &g_trackside_zoom, 0.2f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Fully zoomed out within", &g_trackside_zoom_near, 20.0f, 600.0f, "%.0f");
    changed |= ImGui::SliderFloat("Height##trackside", &g_trackside_height, 0.0f, 200.0f, "%.0f");
    changed |= ImGui::SliderFloat("Release when past by", &g_trackside_past, 20.0f, 1500.0f, "%.0f");
    changed |= ImGui::SliderFloat("Max duration (s)##trackside", &g_trackside_max_s, 3.0f, 40.0f, "%.0f");
    if (changed)
        save_config();

    ImGui::Separator();
    const RaceTelemetry *t = race_telemetry_Get();
    const RaceTelemetryRow *cur = row_for_slot(t, director_FollowedSlot());
    ImGui::Text("Following: %s  |  shot: %s  |  last cut by %s, %d cuts", cur ? cur->name : "-",
                SHOT_NAMES[g_shot], g_last_rule, g_cuts);
    ImGui::BeginDisabled(!g_enabled || !t->valid);
    if (ImGui::Button("Cut now")) {
        const char *rule = "";
        const int next = pick_auto(t, &rule);
        if (next >= 0)
            cut_to(next, rule);
    }
    ImGui::SameLine();
    if (ImGui::Button("Release hold"))
        g_manual_until_ms = 0;
    ImGui::SameLine();
    if (ImGui::Button("Chase"))
        set_shot(SHOT_CHASE);
    ImGui::SameLine();
    if (ImGui::Button("Drone"))
        set_shot(SHOT_DRONE);
    ImGui::SameLine();
    if (ImGui::Button("Orbit")) {
        g_orbit_rival = nearest_rival(t, g_target_slot, g_orbit_dist);
        set_shot(g_orbit_rival >= 0 ? SHOT_ORBIT : SHOT_CHASE);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cockpit"))
        set_shot(SHOT_COCKPIT);
    ImGui::SameLine();
    if (ImGui::Button("Trackside"))
        set_shot(SHOT_TRACKSIDE);
    ImGui::SameLine();
    if (ImGui::Button("Face"))
        set_shot(SHOT_FACE);
    ImGui::SameLine();
    if (ImGui::Button("Far"))
        set_shot(SHOT_CHASE_FAR);
    ImGui::SameLine();
    if (ImGui::Button("Bumper"))
        set_shot(SHOT_BUMPER);
    ImGui::SameLine();
    if (ImGui::Button("FP wide"))
        set_shot(SHOT_FP_WIDE);
    ImGui::EndDisabled();
}

static DebugPanel g_panel = {.category = "Camera",
                             .name = "Director",
                             .draw = panel_director,
                             .dev_only = false};

void director_RegisterPanel() {
    load_config();
    save_config();// stamps cfg_version so the one-time default override does not repeat
    debug_ui_register(&g_panel);
}
