#include "ai_variance.h"
#include "broadcast/race_telemetry.h"
#include "debug_ui.h"
#include "imgui_utils.h"
#include "config.h"
#include "hook_helper.h"
#include "game_deltas/swrRace_delta.h"

#include <imgui.h>

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

extern "C" {
#include <Swr/swrObj.h>
#include <Swr/swrRace.h>
#include <Swr/swrSound.h>
#include <Swr/swrSpline.h>
#include <Swr/swrEvent.h>
#include <globals.h>
void hook_function(const char *function_name, uint32_t original_address, uint8_t *hook_address);
}

// ---------------------------------------------------------------------------------------------
// Settings ([ai_variance])

static bool g_enabled = true;
static bool g_with_humans = false;    // also apply to AI in races a human drives
static bool g_replace_stock = true;   // ignore the stock rank-offset pacing (all AI start equal)
static float g_form_amp = 0.04f;      // per-race per-racer constant factor in [1-a, 1+a]
static float g_swing_amp = 0.05f;     // slow pace swings (Ornstein-Uhlenbeck), stddev
static float g_swing_tau_s = 8.0f;    // swing time constant
static float g_pack_gain = 0.03f;     // multiplier per second of deviation from the target spacing
static float g_pack_spread_s = 6.0f;  // target first-to-last spread of the field (s)
static float g_pack_clamp = 0.15f;    // max +/- from pack compression
static float g_blunder_per_min = 4.0f;// blunders per racer per minute
static float g_blunder_min_s = 0.5f, g_blunder_max_s = 8.0f;
static float g_blunder_depth = 1.0f;             // multiplier during a blunder
static float g_clamp_lo = 0.5f, g_clamp_hi = 1.6f;// same envelope as the stock AI
// Boost. Humans charge by holding throttle + nose-down at > 75% top speed for 1 s, then fire;
// boosting drains engineTemp (100 = cool, 0 = overheated -> a random engine catches fire).
static bool g_boost = true;
static bool g_flame = true;// Sebulba's flame attack from lap 1 (stock AI waits for lap 3)
static int g_flame_min_lap = 0;
// Heat budget. engineTemp runs 100 (cool) to 0 (an engine catches fire); boosting drains it at
// podStats.heatRate, coasting refills it at coolRate, so total boost time per race is fixed and the
// only real choice is long-and-rare vs short-and-often. Greedy spends it in the longest chunks the
// gauge allows: charge to nearly full, hold the boost until it is nearly empty, go again as soon as
// it has refilled -- no dice roll, no duration timer.
static bool g_boost_greedy = true;
static float g_boost_start_per_s = 2.0f;// non-greedy only: chance per second to begin charging
static float g_boost_min_s = 8.0f, g_boost_max_s = 12.0f;// non-greedy only: boost duration
static float g_boost_release_temp = 5.0f;   // let go of the boost when engineTemp falls to this
static float g_boost_min_start_temp = 90.0f;// only start a boost with the gauge this full
// A boost lasts exactly as long as the gap between those two, so they must never meet: equal
// values mean every boost ends on the frame it starts.
static const float BOOST_GAUGE_MIN_SPAN = 10.0f;

static void enforce_gauge_span() {
    if (g_boost_min_start_temp - g_boost_release_temp < BOOST_GAUGE_MIN_SPAN)
        g_boost_release_temp = g_boost_min_start_temp - BOOST_GAUGE_MIN_SPAN;
    if (g_boost_release_temp < 0.0f)
        g_boost_release_temp = 0.0f;
}
static float g_boost_turn_limit = 400.0f;   // |turnRateTarget| above this = cornering, no boost
static float g_boost_cooldown_s = 0.5f;
static float g_boost_p_overheat = 0.2f;// chance a boost is held until the engines catch fire
static bool g_boost_sound = true;
static float g_boost_sfx_range = 450.0f;// only AI within this many units of the camera play the boost sound
static bool g_wall_damage = true;// AI take the player's wall scrape / impact path (damage, sparks)
static bool g_impact_death =
    true;// AI explode on a death-speed impact like a human (stock: quiet respawn)
static float g_impact_toughness =
    1.5f;// AI death-speed thresholds scaled by this (1 = same as the player)
// Corner anticipation. The stock autopilot runs a flat 0.98 throttle and only lifts once its
// heading error is already past 0.3 rad -- i.e. after it has missed the corner -- and it never
// touches the air brake. Scan the spline ahead, work out the speed each corner allows, and brake
// for it. Also the gate that keeps a boost from being carried into one.
static bool g_corner = false;
static float g_corner_lat = 600.0f;    // cornering grip, units/s^2: speed limit = sqrt(lat * radius)
static float g_corner_decel = 700.0f;  // assumed braking capability, units/s^2 (sets the brake point)
static float g_corner_margin = 1.0f;   // scales the speed limit (below 1 = slower through corners)
static int g_corner_samples = 10;      // spline samples ahead, each ~half an AI look-ahead (~45 u)
static float g_corner_lift = 0.45f;    // throttle multiplier while braking
static float g_corner_coast = 0.75f;   // throttle multiplier just under the limit (lift, no brake)
static bool g_corner_lean = true;      // bank into corners (visual: the pods look like they steer)
static float g_corner_lean_gain = 1.0f;// |tilt| at full steering lock
static bool g_corner_slide = true;    // release grip in corners for a drifty look -- it IS less
                                       // grip (slide2 x0.45 -> velocity lags the nose), so the pods
                                       // run wider; off by default
static float g_corner_slide_over = 2.0f;   // slide only while this far over the corner's limit
static float g_boost_corner_margin = 1.0f; // do not START a boost while the limit ahead is under
                                           // speed x this
static float g_boost_corner_cancel = 0.8f;// but only DROP a burning one once the limit ahead is
                                          // under speed x this -- a boost pushes the pod over its
                                          // own corner limit, so cancelling on the same threshold
                                          // that gates the start makes every boost self-cancelling
static float g_boost_min_hold_s = 1.5f;   // once lit, hold at least this long (corners aside)
// swrRace_TakeDamage cancels a boost on ANY hit, and in a 20-pod pack the scrapes never stop, so
// nothing survived a second. The human rule is looser -- swrRace_UpdatePlayerControl only drops a
// boost once GetDamagedEngineSides reports a hurt side -- so hold a boost through hits smaller than
// this unless one of them actually damages an engine side.
static bool g_boost_shrug_scrapes = true;
static float g_boost_scrape_tol = 0.05f;
static bool g_corner_log = false;          // one line per racer per second to hook.log, for tuning
// Rolling on the boost. Pure showmanship, and the one thing manual tilt is good for: it feeds only
// the pod's roll angle (and the lava spinout bias), never grip or turn rate.
static bool g_boost_roll = true;
static float g_boost_roll_chance = 0.5f;// share of boosts that get a roll
static float g_boost_roll_min = 1.0f, g_boost_roll_max = 1.0f;// |roll|, 1 = the full 80 deg bank
static const int CFG_VERSION = 3;            // bump when a default should override a stored value
static float g_boost_steer_scale = 0.5f;     // steering authority while boosting (risky in corners)
static const float BOOST_CHARGE_PITCH = 0.8f;// nose-down held while charging, like the player

// ---------------------------------------------------------------------------------------------
// Per-race state (indexed by swrScoresPtr slot)

static const int MAX_SLOTS = 20;
static float g_form_u[MAX_SLOTS];// per-race unit draw in [-1, 1]; form = 1 + u * amplitude (live)
static float g_form[MAX_SLOTS];
static float g_swing[MAX_SLOTS];
static DWORD g_blunder_until_ms[MAX_SLOTS];
static float g_last_mult[MAX_SLOTS];
static DWORD g_boost_until_ms[MAX_SLOTS];
static DWORD g_boost_cooldown_ms[MAX_SLOTS];
static bool g_boost_hold_to_fire[MAX_SLOTS];
static int g_boosts_total = 0;
static int g_flames_total = 0;
static int g_explosions_total = 0;
static DWORD g_dead_since_ms[MAX_SLOTS];
static int g_respawns_total = 0;
static float g_corner_v_allow[MAX_SLOTS];// speed the tightest corner in range allows
static float g_corner_k[MAX_SLOTS];      // its curvature, rad/unit
static float g_corner_dist[MAX_SLOTS];   // distance to it
static float g_tilt_target[MAX_SLOTS];
static bool g_corner_braking[MAX_SLOTS];
static DWORD g_boost_started_ms[MAX_SLOTS];
static bool g_boost_was_on[MAX_SLOTS];// to notice a boost something else put out
static int g_boost_cancels_corner = 0, g_boost_cancels_turn = 0, g_boost_cancels_temp = 0,
           g_boost_cancels_time = 0, g_boost_cancels_extern = 0;
static int g_boost_blocked_fire = 0;
// Which routine actually cleared BOOSTING, sampled by the probes below. The flag can be cleared
// anywhere in the frame, so reading BRAKING after the fact proves nothing.
static const char *g_boost_killer[MAX_SLOTS];
static int g_kills_damage = 0, g_kills_surface = 0, g_kills_speed = 0;
static int g_boost_scrapes_shrugged = 0;
static float g_boost_roll_target[MAX_SLOTS];// roll held for this boost, 0 = this one is flat
static int g_boost_rolls_total = 0;
static int g_tilt_calls = 0;    // swrRace_Tilt calls this delta took over
static float g_tilt_last_b = 0.0f;// what it last fed the pod
static int g_tilt_seen = 0;       // swrRace_Tilt calls that reached the override at all
static int g_corner_brakes_total = 0;
static DWORD g_corner_log_next_ms = 0;
static DWORD g_tilt_log_next_ms = 0;
static int g_seeded_track = -2;
static int g_seeded_state0_seen = 0;
static int g_blunders_total = 0;

static float frand() {
    return (float) rand() / (float) RAND_MAX;
}

static float gauss() {// Box-Muller
    const float u1 = std::max(1e-6f, frand()), u2 = frand();
    return sqrtf(-2.0f * logf(u1)) * cosf(6.2831853f * u2);
}

static void reseed(DWORD now) {
    (void) now;
    for (int i = 0; i < MAX_SLOTS; i++) {
        g_form_u[i] = frand() * 2.0f - 1.0f;
        g_form[i] = 1.0f + g_form_u[i] * g_form_amp;
        g_swing[i] = 0.0f;
        g_blunder_until_ms[i] = 0;
        g_last_mult[i] = 1.0f;
        g_boost_until_ms[i] = 0;
        g_boost_cooldown_ms[i] = 0;
        g_boost_hold_to_fire[i] = false;
        g_dead_since_ms[i] = 0;
        g_corner_v_allow[i] = 1e9f;
        g_corner_k[i] = 0.0f;
        g_corner_dist[i] = 0.0f;
        g_tilt_target[i] = 0.0f;
        g_corner_braking[i] = false;
        g_boost_roll_target[i] = 0.0f;
        g_boost_started_ms[i] = 0;
        g_boost_was_on[i] = false;
    }
    fprintf(hook_log, "[ai_variance] reseeded form:");
    for (int i = 0; i < MAX_SLOTS; i++)
        fprintf(hook_log, " %.3f", g_form[i]);
    fprintf(hook_log, "\n");
    fflush(hook_log);
}

// New race detection: the judge passes through state 0 (countdown) once per race.
static void maybe_reseed(const RaceTelemetry *t, DWORD now) {
    if (t->judge_state == 0) {
        if (!g_seeded_state0_seen) {
            g_seeded_state0_seen = 1;
            g_seeded_track = t->track_index;
            reseed(now);
        }
    } else {
        g_seeded_state0_seen = 0;
    }
}

static bool applies(const RaceTelemetry *t) {
    if (!g_enabled || !t->valid)
        return false;
    if (t->source == RACE_SOURCE_ALL_AI)
        return true;
    return g_with_humans && t->source == RACE_SOURCE_SINGLE_PLAYER;
}

// ---------------------------------------------------------------------------------------------
// Corner anticipation

// Tightest corner within reach of the pod's spline cursor, expressed as the speed it allows here.
// swrSpline_EvaluateAtOffset works on a copy of the cursor, so sampling ahead has no side effects;
// row vB of the matrix it fills is the unit path tangent, vD the point.
struct CornerAhead {
    float v_allow;// speed allowed at the pod's current position
    float k_max;  // curvature of the corner that set it, rad/unit
    float dist;   // distance to that corner
};

static CornerAhead scan_corner(const swrRace *pod) {
    CornerAhead out = {1e9f, 0.0f, 0.0f};
    if (pod->splineCursor.spline == NULL)
        return out;

    // The pod's own look-ahead offset is eased to about 90 units of path, so half of it is a
    // ready-made step size that adapts to how the track's control points are spaced.
    const float step = std::clamp(pod->aiLookAhead * 0.5f, 0.005f, 2.0f);
    const float lat = std::max(1.0f, g_corner_lat) * std::max(0.05f, g_corner_margin);
    const int n = std::clamp(g_corner_samples, 1, 16);

    rdMatrix44 m;
    swrSpline_EvaluateAtOffset((void *) &pod->splineCursor, &m, 0.0f);
    rdVector3 p_prev = *(rdVector3 *) &m.vD, t_prev = *(rdVector3 *) &m.vB;
    float dist = 0.0f;

    for (int i = 1; i <= n; i++) {
        swrSpline_EvaluateAtOffset((void *) &pod->splineCursor, &m, step * (float) i);
        const rdVector3 p = *(rdVector3 *) &m.vD, t = *(rdVector3 *) &m.vB;
        rdVector3 d;
        rdVector_Sub3(&d, &p, &p_prev);
        const float ds = rdVector_Len3(&d);
        if (ds < 1.0f)
            continue;// end of the path, or a duplicated control point
        const float dtheta = acosf(std::clamp(rdVector_Dot3(&t_prev, &t), -1.0f, 1.0f));
        const float k = dtheta / ds;
        if (k > 1e-5f) {
            // v_limit is what the corner itself allows (v^2 = lateral accel * radius); add back the
            // speed that can still be shed over the distance to it (v^2 = v_limit^2 + 2 a d) to get
            // the limit that applies here.
            const float v_limit = sqrtf(lat / k);
            const float v_allow =
                sqrtf(v_limit * v_limit + 2.0f * std::max(1.0f, g_corner_decel) * dist);
            if (v_allow < out.v_allow) {
                out.v_allow = v_allow;
                out.k_max = k;
                out.dist = dist;
            }
        }
        dist += ds;
        p_prev = p;
        t_prev = t;
    }
    return out;
}

// The air brake (FLAG0_BRAKING), the slide flag and the manual tilt are only ever driven by the
// human control path, so an AI pod's copies of them are ours to own: what this sets holds until it
// clears it again.
static void supervise_corner(swrRace *pod, int slot) {
    const CornerAhead c = scan_corner(pod);
    g_corner_v_allow[slot] = c.v_allow;
    g_corner_k[slot] = c.k_max;
    g_corner_dist[slot] = c.dist;

    const float v = pod->speedValue;
    // Hysteresis: hold the brake until a little under the limit so it does not stutter.
    const bool brake = g_corner_braking[slot] ? v > c.v_allow * 0.97f : v > c.v_allow;
    if (brake && !g_corner_braking[slot])
        g_corner_brakes_total++;
    g_corner_braking[slot] = brake;

    // The air brake clears BOOSTING in swrRace_UpdateBoost, so while a boost is burning only a
    // corner severe enough to be worth losing it gets the brake; anything milder is handled by
    // lifting the throttle, which the boost survives.
    const bool boosting = (pod->flags0 & swrObjTest_FLAG0_BOOSTING) != 0;
    const bool severe = v > c.v_allow / std::max(0.1f, g_boost_corner_cancel);
    if (brake && boosting && !severe) {
        pod->flags0 = (swrObjTest_FLAG0) (pod->flags0 & ~swrObjTest_FLAG0_BRAKING);
        pod->throttle *= g_corner_lift;
    } else if (brake) {
        pod->flags0 = (swrObjTest_FLAG0) (pod->flags0 | swrObjTest_FLAG0_BRAKING);
        pod->throttle *= g_corner_lift;
    } else {
        pod->flags0 = (swrObjTest_FLAG0) (pod->flags0 & ~swrObjTest_FLAG0_BRAKING);
        if (v > c.v_allow * 0.9f)
            pod->throttle *= g_corner_coast;
    }

    if (g_corner_slide && v > c.v_allow * g_corner_slide_over)
        pod->flags1 = (swrObjTest_FLAG1) (pod->flags1 | swrObjTest_FLAG1_SLIDING);
    else
        pod->flags1 = (swrObjTest_FLAG1) (pod->flags1 & ~swrObjTest_FLAG1_SLIDING);

    if (g_corner_log) {
        const DWORD now = GetTickCount();
        if (now >= g_corner_log_next_ms) {
            g_corner_log_next_ms = now + 1000;
            fprintf(hook_log,
                    "[ai_variance] corner slot %d speed %.0f limit %.0f radius %.0f at %.0f%s\n",
                    slot, v, c.v_allow, c.k_max > 1e-5f ? 1.0f / c.k_max : 0.0f, c.dist,
                    brake ? " BRAKE" : "");
            fflush(hook_log);
        }
    }
}

// What the swrRace_Tilt detour below should feed the pod this frame: the roll drawn for a boost
// while it burns, otherwise a lean scaled by how hard the pod is steering. Same sign as the pod's
// own bank-from-turn-rate (swrRace_SetAngleFromTurnRate negates turnRate / maxTurnRate), so the
// lean only deepens a roll the corner already produces.
static void supervise_tilt(const swrRace *pod, int slot) {
    float tilt = 0.0f;
    if (g_corner_lean && pod->podStats.maxTurnRate > 1.0f) {
        tilt = std::clamp(-pod->turnRateTarget / pod->podStats.maxTurnRate, -1.0f, 1.0f);
        if (fabsf(tilt) < 0.2f)
            tilt = 0.0f;
        tilt *= std::clamp(g_corner_lean_gain, 0.0f, 1.0f);
    }
    if (g_boost_roll && (pod->flags0 & swrObjTest_FLAG0_BOOSTING) != 0)
        tilt = g_boost_roll_target[slot];
    g_tilt_target[slot] = tilt;

    if (g_corner_log && tilt != 0.0f) {
        const DWORD now = GetTickCount();
        if (now >= g_tilt_log_next_ms) {
            g_tilt_log_next_ms = now + 250;
            fprintf(hook_log,
                    "[ai_variance] tilt slot %d want %+.2f mult %+.3f angle %+.1f speed %.0f "
                    "lod %d turn %+.0f flags1 %s%s\n",
                    slot, tilt, pod->tiltManualMult, pod->tiltAngle, pod->speedValue,
                    pod->lodDistance, pod->turnRateTarget,
                    (pod->flags1 & swrObjTest_FLAG1_MAGNET) ? "MAGNET " : "",
                    (pod->flags1 & swrObjTest_FLAG1_ON_SIDE) ? "ON_SIDE" : "");
            fflush(hook_log);
        }
    }
}

// A corner close enough that a boost fired now would still be burning in it.
static bool corner_blocks_boost(const swrRace *pod, int slot) {
    if (!g_corner)
        return false;
    return g_corner_braking[slot] ||
           g_corner_v_allow[slot] < pod->speedValue * g_boost_corner_margin;
}

// Severe enough to be worth throwing away a boost that is already burning.
static bool corner_drops_boost(const swrRace *pod, int slot) {
    if (!g_corner)
        return false;
    return g_corner_v_allow[slot] < pod->speedValue * g_boost_corner_cancel;
}

// The pods' manual tilt. swrRace_UpdateAutopilotControl calls swrRace_Tilt with 0 for every racing
// AI pod every frame (only a pod on its side gets 1), so replacing that argument is the whole seam.
// swrRace_delta.cpp owns that hook for the tilt-at-any-speed cheat -- hooking it here too just
// replaced its delta and never ran -- so register as its override instead.
static float ai_variance_tilt_override(swrRace *player, float b) {
    g_tilt_seen++;
    if ((g_corner_lean || g_boost_roll) && b == 0.0f &&
        (player->flags0 & swrObjTest_FLAG0_AI) != 0 &&
        (player->flags0 & swrObjTest_FLAG0_LOCAL) == 0 && player->score_ptr != NULL &&
        swrScoresPtr != NULL && applies(race_telemetry_Get())) {
        const int slot = (int) (player->score_ptr - swrScoresPtr);
        if (slot >= 0 && slot < MAX_SLOTS) {
            b = g_tilt_target[slot];
            g_tilt_calls++;
            g_tilt_last_b = b;
        }
    }
    return b;
}

// swrRace_TakeDamage, swrRace_UpdateSurfaceTag (Slow / Swst terrain) and swrRace_UpdateSpeed (via
// UpdateBoost's air-brake check) are the three routines that clear BOOSTING for a non-human pod.
// Watch the flag across each call and record which one puts a boost out.
static int slot_of(const swrRace *pod) {
    if (pod == NULL || pod->score_ptr == NULL || swrScoresPtr == NULL)
        return -1;
    const int slot = (int) (pod->score_ptr - swrScoresPtr);
    return slot >= 0 && slot < MAX_SLOTS ? slot : -1;
}

static int boost_watch_begin(const swrRace *pod) {
    if (pod == NULL || (pod->flags0 & swrObjTest_FLAG0_BOOSTING) == 0)
        return -1;
    return slot_of(pod);
}

static void boost_watch_end(const swrRace *pod, int slot, const char *who, int *counter) {
    if (slot < 0 || (pod->flags0 & swrObjTest_FLAG0_BOOSTING) != 0)
        return;
    g_boost_killer[slot] = who;
    (*counter)++;
}

typedef void(__cdecl *swrRace_TakeDamage_t)(int player, int engineIndex, float amount);

static void __cdecl swrRace_TakeDamage_delta(int player, int engineIndex, float amount) {
    swrRace *pod = (swrRace *) player;
    const int slot = boost_watch_begin(pod);
    hook_call_original((swrRace_TakeDamage_t) swrRace_TakeDamage_ADDR, player, engineIndex, amount);
    if (slot >= 0 && g_boost_shrug_scrapes && amount < g_boost_scrape_tol &&
        (pod->flags0 & swrObjTest_FLAG0_BOOSTING) == 0 && (pod->flags0 & swrObjTest_FLAG0_AI) != 0 &&
        swrRace_GetDamagedEngineSides(pod) == 0 && applies(race_telemetry_Get())) {
        // The damage itself stands; only the boost cancel is undone.
        pod->flags0 = (swrObjTest_FLAG0) (pod->flags0 | swrObjTest_FLAG0_BOOSTING);
        g_boost_scrapes_shrugged++;
        return;
    }
    boost_watch_end(pod, slot, "damage", &g_kills_damage);
}

typedef void(__cdecl *swrRace_UpdateSurfaceTag_t)(swrRace *pod);

static void __cdecl swrRace_UpdateSurfaceTag_delta(swrRace *pod) {
    const int slot = boost_watch_begin(pod);
    hook_call_original((swrRace_UpdateSurfaceTag_t) swrRace_UpdateSurfaceTag_ADDR, pod);
    boost_watch_end(pod, slot, "terrain", &g_kills_surface);
}

typedef float(__cdecl *swrRace_UpdateSpeed_t)(swrRace *pod);

static float __cdecl swrRace_UpdateSpeed_delta(swrRace *pod) {
    const int slot = boost_watch_begin(pod);
    const float r = hook_call_original((swrRace_UpdateSpeed_t) swrRace_UpdateSpeed_ADDR, pod);
    boost_watch_end(pod, slot, "air brake", &g_kills_speed);
    return r;
}

// AI boost: drive the same boostIndicatorStatus / boostChargeTimer machine the human path uses
// (0 idle -> 1 charging for 1 s -> 2 ready -> fire), gated on CAN_CHARGE_BOOST (the game sets it at
// > 75% top speed), a straight, and enough engine coolness. A boost lasts a few seconds or until
// engineTemp reaches the release point; with a small chance it is held until the engines ignite.
static const float BOOST_CHARGE_S = 1.0f;// _DAT_004ad7f4
static const int BOOST_SFX_ID = 0x72;    // swrRace_BoostCharge's fire sound

// Sebulba's flame attack. swrObjTest_F0@0x46d170 lets an AI Sebulba (score->pilotId == 2) call
// swrRace_SpawnFlameAttack only from the third lap (results_P1_Lap >= 2), above 200 speed, with a
// pod within 10000 units on its vA side. The flame itself (swrRace_UpdateEngineDamageFX) already
// lights every pod within 64 units of its tip, AI included, so the only thing stopping AI-on-AI
// fire is that late lap gate. Re-run the trigger with a configurable minimum lap;
// SpawnFlameAttack refuses while a flame is already burning.
static const float FLAME_MIN_SPEED = 200.0f;// _DAT_004ad820
static const float FLAME_SCAN_RANGE = 10000.0f;// 0x461c4000

static void supervise_flame(swrRace *pod, int slot, const char *name) {
    if (pod->score_ptr->pilotId == NULL || *pod->score_ptr->pilotId != 2)
        return;
    if (pod->score_ptr->results_P1_Lap < g_flame_min_lap || pod->speedValue <= FLAME_MIN_SPEED)
        return;
    if (pod->flameSmokeHandle != NULL)
        return;
    for (int i = 0; i < MAX_SLOTS; i++) {
        const swrRace *other = swrScoresPtr[i].obj_test_ptr;
        if (other == NULL || other == pod || (swrScoresPtr[i].flag & 1) == 0)
            continue;
        rdVector3 d;
        rdVector_Sub3(&d, (rdVector3 *) &other->transform.vD, (rdVector3 *) &pod->transform.vD);
        if (rdVector_Len3(&d) > FLAME_SCAN_RANGE)
            continue;
        if (rdVector_Dot3(&d, (rdVector3 *) &pod->transform.vA) > 0.0f) {
            swrRace_SpawnFlameAttack(pod);
            if (pod->flameSmokeHandle != NULL) {
                g_flames_total++;
                fprintf(hook_log, "[ai_variance] slot %d (%s) flame attack\n", slot, name);
                fflush(hook_log);
            }
            return;
        }
    }
}

static bool near_camera(const swrRace *pod, float range) {
    const swrObjcMan *cman = (const swrObjcMan *) swrEvent_FindObjectById('cMan', 0);
    if (cman == NULL)
        return true;
    const float dx = pod->transform.vD.x - cman->unk20_mat.vD.x;
    const float dy = pod->transform.vD.y - cman->unk20_mat.vD.y;
    const float dz = pod->transform.vD.z - cman->unk20_mat.vD.z;
    return dx * dx + dy * dy + dz * dz <= range * range;
}

// A pod with an engine alight cannot hold a boost: the burning-engine damage tick (the player's
// swrRace_ApplyEngineDamage, mirrored for AI by the orchestrator) calls swrRace_TakeDamage every
// frame, and that clears BOOSTING outright. Firing one anyway just burns the gauge for a frame or
// two, so wait until it is repaired -- which is also what the damaged human path does.
static bool engine_alight(const swrRace *pod) {
    for (int i = 0; i < 6; i++)
        if ((pod->engineStatus[i] & 8) != 0)
            return true;
    return false;
}

static void supervise_boost(swrRace *pod, int slot, float dt, DWORD now, const char *name) {
    const bool boosting = (pod->flags0 & swrObjTest_FLAG0_BOOSTING) != 0;
    const bool can_charge = (pod->flags0 & swrObjTest_FLAG0_CAN_CHARGE_BOOST) != 0;
    const bool corner = corner_blocks_boost(pod, slot);
    const bool straight = fabsf(pod->turnRateTarget) < g_boost_turn_limit && !corner;
    const bool hard_turn = fabsf(pod->turnRateTarget) > g_boost_turn_limit * 1.3f;

    // A boost something else put out (the air brake in swrRace_UpdateBoost, Slow/Swst terrain in
    // UpdateSurfaceTag, an engine igniting in UpdateHeat) still counts as one of ours ending.
    if (g_boost_was_on[slot] && !boosting && pod->boostIndicatorStatus == 0) {
        g_boost_cancels_extern++;
        if (g_corner_log) {
            fprintf(hook_log, "[ai_variance] slot %d boost put out by %s (temp %.0f, %.1fs in)\n",
                    slot, g_boost_killer[slot] != NULL ? g_boost_killer[slot] : "something unseen",
                    pod->engineTemp, (now - g_boost_started_ms[slot]) / 1000.0f);
            fflush(hook_log);
        }
    }
    g_boost_was_on[slot] = boosting;

    if (boosting) {
        // Boosting narrows what the pilot can do with the stick: scale the autopilot's steering
        // demand so a boost carried into a corner is a real gamble.
        pod->turnRateTarget *= g_boost_steer_scale;
        // Greedy holds the boost on the gauge alone; otherwise the drawn duration also ends it.
        const bool held = now - g_boost_started_ms[slot] < (DWORD) (g_boost_min_hold_s * 1000.0f);
        const bool timed_out = !g_boost_greedy && now >= g_boost_until_ms[slot];
        const bool too_hot = !g_boost_hold_to_fire[slot] && pod->engineTemp <= g_boost_release_temp;
        const bool drop_corner = corner_drops_boost(pod, slot);
        if (timed_out || too_hot || drop_corner || (hard_turn && !held)) {
            if (too_hot)
                g_boost_cancels_temp++;
            else if (timed_out)
                g_boost_cancels_time++;
            else if (drop_corner)
                g_boost_cancels_corner++;
            else
                g_boost_cancels_turn++;
            pod->flags0 = (swrObjTest_FLAG0) (pod->flags0 & ~swrObjTest_FLAG0_BOOSTING);
            g_boost_was_on[slot] = false;
            g_boost_cooldown_ms[slot] = now + (DWORD) (g_boost_cooldown_s * 1000.0f);
            if (g_corner_log) {
                fprintf(hook_log, "[ai_variance] slot %d boost released: %s (temp %.0f, %.1fs in)\n",
                        slot,
                        too_hot ? "gauge down to the release point"
                                : timed_out ? "timer"
                                            : drop_corner ? "corner ahead" : "steering hard",
                        pod->engineTemp, (now - g_boost_started_ms[slot]) / 1000.0f);
                fflush(hook_log);
            }
        }
        return;
    }
    switch (pod->boostIndicatorStatus) {
        case 0:
            if (engine_alight(pod)) {
                g_boost_blocked_fire++;
                break;
            }
            if (can_charge && straight && now >= g_boost_cooldown_ms[slot] &&
                pod->engineTemp >= g_boost_min_start_temp &&
                (g_boost_greedy || frand() < g_boost_start_per_s * dt)) {
                pod->boostIndicatorStatus = 1;
                pod->boostChargeTimer = 0.0f;
            }
            break;
        case 1:
            if (!can_charge) {
                pod->boostIndicatorStatus = 0;
                break;
            }
            pod->pitch = BOOST_CHARGE_PITCH;// hold the nose down for the charge second
            pod->boostChargeTimer += dt;
            if (pod->boostChargeTimer > BOOST_CHARGE_S)
                pod->boostIndicatorStatus = 2;
            break;
        case 2:
            if (!can_charge) {
                pod->boostIndicatorStatus = 0;
                break;
            }
            if (straight) {
                pod->boostIndicatorStatus = 0;
                pod->flags0 = (swrObjTest_FLAG0) (pod->flags0 | swrObjTest_FLAG0_BOOSTING);
                g_boost_until_ms[slot] =
                    now + (DWORD) ((g_boost_min_s +
                                    frand() * std::max(0.0f, g_boost_max_s - g_boost_min_s)) *
                                   1000.0f);
                g_boost_hold_to_fire[slot] = frand() < g_boost_p_overheat;
                g_boost_started_ms[slot] = now;
                g_boost_killer[slot] = NULL;
                // Roll for this one, or keep it flat -- decided per boost, per racer.
                g_boost_roll_target[slot] = 0.0f;
                if (g_boost_roll && frand() < g_boost_roll_chance) {
                    const float mag =
                        g_boost_roll_min +
                        frand() * std::max(0.0f, g_boost_roll_max - g_boost_roll_min);
                    g_boost_roll_target[slot] = frand() < 0.5f ? -mag : mag;
                    g_boost_rolls_total++;
                }
                g_boosts_total++;
                // A human race plays this for one pod; twenty AI boosting once a second each is a
                // few thousand positional one-shots per race, so only pods the camera can hear get one.
                if (g_boost_sound && near_camera(pod, g_boost_sfx_range))
                    swrSound_PlaySpatialRange(
                        BOOST_SFX_ID, 7,
                        // swrRace_BoostCharge@0x46bd20: rand*0.1 - (-0.18) => 0.18..0.28 (the
                        // constant at 0x4ad8c0 is negative); slightly wider than stock
                        frand() * 0.14f + 0.15f,
                        1.0f, (rdVector3 *) &pod->transform.vD, 0, 1, 10.0f, 500.0f);
                fprintf(hook_log, "[ai_variance] slot %d (%s) boost%s%s\n", slot, name,
                        g_boost_hold_to_fire[slot] ? " (holding to overheat)" : "",
                        g_boost_roll_target[slot] != 0.0f
                            ? (g_boost_roll_target[slot] < 0.0f ? " rolling left" : " rolling right")
                            : "");
                fflush(hook_log);
            }
            break;
        default:
            pod->boostIndicatorStatus = 0;
            break;
    }
}

// Respawn for pods without a camera-man. After an explosion the pod update runs the death snap for
// every pod (swrRace_HandleDeathSnap: 'Snap' back onto the spline, fires out, engines to 0.1), but
// the step that clears FLAG0_DEAD and grants respawn invincibility is swrObjcMan_UpdateDeathCamera
// stage 3 -- the death camera -- and only the followed pod has a camera-man. Everyone else stayed
// dead with the throttle cut. Emulate that stage after the death camera's ~3 s; the followed pod is
// left to its camera.
static const float DEAD_RESPAWN_S = 3.0f;

static void supervise_dead(swrRace *pod, int slot, DWORD now) {
    const bool dead = (pod->flags0 & swrObjTest_FLAG0_DEAD) != 0;
    if (!dead) {
        g_dead_since_ms[slot] = 0;
        return;
    }
    if (g_dead_since_ms[slot] == 0) {
        g_dead_since_ms[slot] = now;
        return;
    }
    if (now - g_dead_since_ms[slot] < (DWORD) (DEAD_RESPAWN_S * 1000.0f))
        return;
    swrObjcMan *cman = (swrObjcMan *) swrEvent_FindObjectById('cMan', 0);
    if (cman != NULL && cman->unkf4_objTest == pod)
        return;// the death camera handles the followed pod
    pod->flags1 = (swrObjTest_FLAG1) (pod->flags1 & ~(swrObjTest_FLAG1_EXPLODING |
                                                      swrObjTest_FLAG1_EXPLODING_LEFT |
                                                      swrObjTest_FLAG1_EXPLODING_RIGHT));
    if ((pod->flags0 & swrObjTest_FLAG0_RESET) != 0) {
        // death snap never ran (timer still counting): do its work now
        int snap[1] = {'Snap'};
        swrEvent_DispatchSubEvents(pod, snap);
        for (int e = 0; e < 6; e++) {
            pod->engineStatus[e] &= ~8u;
            if (pod->engineHealth[e] > 0.1f)
                pod->engineHealth[e] = 0.1f;
        }
        pod->flags0 = (swrObjTest_FLAG0) (pod->flags0 & ~swrObjTest_FLAG0_RESET);
    }
    pod->respawnInvincibilityTimer = 3.0f;
    pod->flags0 = (swrObjTest_FLAG0) ((pod->flags0 & ~swrObjTest_FLAG0_DEAD) |
                                      swrObjTest_FLAG0_RESPAWN_INVINC);
    g_dead_since_ms[slot] = 0;
    g_respawns_total++;
    fprintf(hook_log, "[ai_variance] slot %d respawned (no camera-man)\n", slot);
    fflush(hook_log);
}

typedef void(__cdecl *swrRace_UpdateCatchup_t)(swrRace *player);

// Wall contact. swrRace_UpdateWallContact routes only LOCAL pods through swrRace_DetectWallScrape +
// swrRace_ApplyWallCollision (scrape sparks, impact speed clamp, 'Hitt' damage events); everyone
// else gets the plain block-move collision, so an AI can rub a wall all race without a scratch. Run
// the player path for AI by lending them the LOCAL bit for the call (nothing in that path plays
// force feedback or player-only sound).
// swrObjTest_UpdatePhysicsContact_delta hides the AI bit for the whole original call, and the wall
// contact / death-speed hooks run nested inside it, so they must still recognise that pod as AI.
static swrRace *g_ai_bit_hidden = NULL;

static bool is_ai(const swrRace *player) {
    return (player->flags0 & swrObjTest_FLAG0_AI) != 0 || player == g_ai_bit_hidden;
}

typedef float(__cdecl *swrRace_UpdateWallContact_t)(swrRace *player, float *a, float *b,
                                                    rdVector3 *c);

static bool lend_local_for_scrape(swrRace *player) {
    const bool lend = g_wall_damage && player != NULL && is_ai(player) &&
                      (player->flags0 & swrObjTest_FLAG0_LOCAL) == 0 && applies(race_telemetry_Get());
    if (lend)
        player->flags0 = (swrObjTest_FLAG0) (player->flags0 | swrObjTest_FLAG0_LOCAL);
    return lend;
}

// Zero-g / orbit surfaces (FLAG0_ZON); the ordinary driving path is swrRace_UpdateGroundContact.
static float __cdecl swrRace_UpdateWallContact_delta(swrRace *player, float *a, float *b,
                                                     rdVector3 *c) {
    const bool lend = lend_local_for_scrape(player);
    const float r = hook_call_original((swrRace_UpdateWallContact_t) swrRace_UpdateWallContact_ADDR,
                                       player, a, b, c);
    if (lend)
        player->flags0 = (swrObjTest_FLAG0) (player->flags0 & ~swrObjTest_FLAG0_LOCAL);
    return r;
}

// Same LOCAL gate around DetectWallScrape + ApplyWallCollision at 0x47a108; non-local pods get the
// plain block move (and a spline z-clamp) instead.
typedef float(__cdecl *swrRace_UpdateGroundContact_t)(swrRace *player, float *velocity,
                                                      int scrapeData, rdVector3 *up,
                                                      int hoverPadState);

static float __cdecl swrRace_UpdateGroundContact_delta(swrRace *player, float *velocity,
                                                       int scrapeData, rdVector3 *up,
                                                       int hoverPadState) {
    const bool lend = lend_local_for_scrape(player);
    const float r =
        hook_call_original((swrRace_UpdateGroundContact_t) swrRace_UpdateGroundContact_ADDR, player,
                           velocity, scrapeData, up, hoverPadState);
    if (lend)
        player->flags0 = (swrObjTest_FLAG0) (player->flags0 & ~swrObjTest_FLAG0_LOCAL);
    return r;
}


static void __cdecl swrRace_UpdateCatchup_delta(swrRace *player) {
    hook_call_original((swrRace_UpdateCatchup_t) swrRace_UpdateCatchup_ADDR, player);
    if (player == NULL || (player->flags0 & swrObjTest_FLAG0_AI) == 0 ||
        (player->flags0 & swrObjTest_FLAG0_LOCAL) != 0)
        return;
    const RaceTelemetry *t = race_telemetry_Get();
    if (!applies(t))
        return;
    const DWORD now = GetTickCount();
    maybe_reseed(t, now);
    if (t->judge_state != 1 && t->judge_state != 2)
        return;
    if (player->score_ptr == NULL || swrScoresPtr == NULL)
        return;
    const int slot = (int) (player->score_ptr - swrScoresPtr);
    if (slot < 0 || slot >= MAX_SLOTS)
        return;
    supervise_dead(player, slot, now);
    if ((player->flags0 & swrObjTest_FLAG0_STATE_MASK) != swrObjTest_FLAG0_RACING ||
        (player->flags1 & swrObjTest_FLAG1_FINISHED) != 0 ||
        (player->flags0 & swrObjTest_FLAG0_DEAD) != 0)
        return;

    const float dt = (float) swrRace_deltaTimeSecs;
    g_form[slot] = 1.0f + g_form_u[slot] * g_form_amp;// amplitude changes apply at once

    // slow swing: dx = -x/tau dt + sigma*sqrt(2 dt/tau) dW
    if (g_swing_tau_s > 0.0f && dt > 0.0f) {
        const float k = dt / g_swing_tau_s;
        g_swing[slot] += -g_swing[slot] * k + g_swing_amp * sqrtf(2.0f * k) * gauss();
        g_swing[slot] = std::clamp(g_swing[slot], -3.0f * g_swing_amp, 3.0f * g_swing_amp);
    }

    // pack compression: pull each racer toward its share of the target spread
    float pack = 1.0f;
    const RaceTelemetryRow *row = NULL;
    int racing_rank = 0, racing_n = 0;
    for (int k = 0; k < t->n; k++) {
        if (t->rows[k].finished)
            continue;
        if (t->rows[k].slot == slot) {
            row = &t->rows[k];
            racing_rank = racing_n;
        }
        racing_n++;
    }
    if (row != NULL && row->gap_leader_s >= 0.0f && racing_n > 1) {
        const float target = g_pack_spread_s * (float) racing_rank / (float) (racing_n - 1);
        const float err = row->gap_leader_s - target;// positive = further back than wanted
        pack = std::clamp(1.0f + g_pack_gain * err, 1.0f - g_pack_clamp, 1.0f + g_pack_clamp);
    }

    // blunders: per-frame Bernoulli at the configured rate (so the rate applies live)
    float blunder = 1.0f;
    if (g_blunder_until_ms[slot] != 0 && now < g_blunder_until_ms[slot]) {
        blunder = g_blunder_depth;
    } else {
        g_blunder_until_ms[slot] = 0;
        const float p = g_blunder_per_min / 60.0f * dt;
        if (p > 0.0f && frand() < p) {
            const float dur =
                g_blunder_min_s + frand() * std::max(0.0f, g_blunder_max_s - g_blunder_min_s);
            g_blunder_until_ms[slot] = now + (DWORD) (dur * 1000.0f);
            g_blunders_total++;
            fprintf(hook_log, "[ai_variance] slot %d (%s) blunder %.1fs\n", slot,
                    row ? row->name : "?", dur);
            fflush(hook_log);
            blunder = g_blunder_depth;
        }
    }

    if (g_corner)
        supervise_corner(player, slot);
    if (g_boost)
        supervise_boost(player, slot, dt, now, row ? row->name : "?");
    supervise_tilt(player, slot);
    if (g_flame)
        supervise_flame(player, slot, row ? row->name : "?");

    const float ours = g_form[slot] * (1.0f + g_swing[slot]) * pack * blunder;
    const float base = g_replace_stock ? 1.0f : player->speedMultiplier;
    player->speedMultiplier = std::clamp(base * ours, g_clamp_lo, g_clamp_hi);
    // speedMultiplier scales the pod's speed outright (swrRace_UpdateSpeed), which would undo the
    // braking: while braking for a corner the pacing may only ever slow a pod down.
    if (g_corner && g_corner_braking[slot] && player->speedMultiplier > 1.0f)
        player->speedMultiplier = 1.0f;
    // The pace-setter's rubberband also scales AI steering (AutopilotSteer scales turnRateTarget
    // and its clamp by paceMultiplier), which is the favourite's remaining edge. Equalize it.
    if (g_replace_stock)
        player->paceMultiplier = 1.0f;
    g_last_mult[slot] = player->speedMultiplier;
}

// Impact deaths. swrRace_DeathSpeed applies the same DeathSpeedMin / DeathSpeedDrop thresholds to
// every pod, but only a non-AI pod explodes (swrRace_Explode); an AI is merely flagged to respawn.
// Hide the AI bit for the call so a hard enough hit blows an AI up like a human.
typedef void(__cdecl *swrRace_DeathSpeed_t)(swrRace *player, float a, float b);

static void __cdecl swrRace_DeathSpeed_delta(swrRace *player, float a, float b) {
    const bool lend =
        g_impact_death && player != NULL && is_ai(player) &&
        (player->flags0 & swrObjTest_FLAG0_LOCAL) == 0 && applies(race_telemetry_Get());
    const bool hide_bit = lend && (player->flags0 & swrObjTest_FLAG0_AI) != 0;
    const float min_saved = swrRace_DeathSpeedMin, drop_saved = swrRace_DeathSpeedDrop;
    if (lend) {
        if (hide_bit)
            player->flags0 = (swrObjTest_FLAG0) (player->flags0 & ~swrObjTest_FLAG0_AI);
        swrRace_DeathSpeedMin = min_saved * g_impact_toughness;
        swrRace_DeathSpeedDrop = drop_saved * g_impact_toughness;
    }
    hook_call_original((swrRace_DeathSpeed_t) swrRace_DeathSpeed_ADDR, player, a, b);
    if (lend) {
        if (hide_bit)
            player->flags0 = (swrObjTest_FLAG0) (player->flags0 | swrObjTest_FLAG0_AI);
        swrRace_DeathSpeedMin = min_saved;
        swrRace_DeathSpeedDrop = drop_saved;
    }
}

// The explosion itself. swrRace_Explode returns at once unless the pod is LOCAL or FORCE_GROUND
// (the post-finish player pod handed to the AI), so every death path above -- destroyed engine,
// death-speed impact -- was a no-op for AI. Lend FORCE_GROUND for the call (the game's own
// admission for a non-local pod; the LOCAL-only block inside is just a force-feedback stop).
typedef void(__cdecl *swrRace_Explode_t)(swrRace *player, int mode);

static void __cdecl swrRace_Explode_delta(swrRace *player, int mode) {
    // Not keyed on the AI bit: swrRace_DeathSpeed_delta hides it for its call, and in an all-AI
    // race every non-local pod is an AI anyway.
    const bool lend = player != NULL && (player->flags0 & swrObjTest_FLAG0_LOCAL) == 0 &&
                      (player->flags1 & swrObjTest_FLAG1_FORCE_GROUND) == 0 &&
                      applies(race_telemetry_Get());
    const bool was_exploding = player != NULL && (player->flags1 & swrObjTest_FLAG1_EXPLODING) != 0;
    if (lend)
        player->flags1 = (swrObjTest_FLAG1) (player->flags1 | swrObjTest_FLAG1_FORCE_GROUND);
    hook_call_original((swrRace_Explode_t) swrRace_Explode_ADDR, player, mode);
    if (lend) {
        player->flags1 = (swrObjTest_FLAG1) (player->flags1 & ~swrObjTest_FLAG1_FORCE_GROUND);
        if (!was_exploding && (player->flags1 & swrObjTest_FLAG1_EXPLODING) != 0) {
            g_explosions_total++;
            const int slot = player->score_ptr != NULL && swrScoresPtr != NULL
                                 ? (int) (player->score_ptr - swrScoresPtr)
                                 : -1;
            fprintf(hook_log, "[ai_variance] slot %d exploded (mode %d)\n", slot, mode);
            fflush(hook_log);
        }
    }
}

// Wall impacts never reached swrRace_DeathSpeed for AI: swrObjTest_UpdatePhysicsContact calls it
// only for non-AI (or FORCE_GROUND) pods. Hide the AI bit for that call; nothing else in the
// routine reads it.
typedef void(__cdecl *swrObjTest_UpdatePhysicsContact_t)(swrRace *player);

static void __cdecl swrObjTest_UpdatePhysicsContact_delta(swrRace *player) {
    const bool lend =
        g_impact_death && player != NULL && is_ai(player) &&
        (player->flags0 & swrObjTest_FLAG0_LOCAL) == 0 && applies(race_telemetry_Get());
    if (lend) {
        player->flags0 = (swrObjTest_FLAG0) (player->flags0 & ~swrObjTest_FLAG0_AI);
        g_ai_bit_hidden = player;
    }
    hook_call_original((swrObjTest_UpdatePhysicsContact_t) swrObjTest_UpdatePhysicsContact_ADDR,
                       player);
    if (lend) {
        g_ai_bit_hidden = NULL;
        player->flags0 = (swrObjTest_FLAG0) (player->flags0 | swrObjTest_FLAG0_AI);
    }
}

// Spinout visuals. When an engine blows, swrRace_Explode sets EXPLODING_LEFT/RIGHT and the pod
// update spins the dead engine and the cockpit (swrRace_AnimateSpinoutEngines) and shows the damaged
// engine nodes (swrRace_UpdateSpinoutNodes) -- but both early-out unless the pod is LOCAL or
// FORCE_GROUND, so an AI just coasted upright until the death snap. Lend LOCAL for the two calls.
typedef void(__cdecl *swrRace_SpinoutVisual_t)(swrRace *player);

static void call_spinout_visual(uint32_t addr, swrRace *player) {
    const bool lend = player != NULL && is_ai(player) &&
                      (player->flags0 & swrObjTest_FLAG0_LOCAL) == 0 &&
                      (player->flags1 & (swrObjTest_FLAG1_EXPLODING_LEFT | swrObjTest_FLAG1_EXPLODING_RIGHT)) != 0 &&
                      applies(race_telemetry_Get());
    if (lend)
        player->flags0 = (swrObjTest_FLAG0) (player->flags0 | swrObjTest_FLAG0_LOCAL);
    hook_call_original((swrRace_SpinoutVisual_t) addr, player);
    if (lend)
        player->flags0 = (swrObjTest_FLAG0) (player->flags0 & ~swrObjTest_FLAG0_LOCAL);
}

static void __cdecl swrRace_AnimateSpinoutEngines_delta(swrRace *player) {
    call_spinout_visual(swrRace_AnimateSpinoutEngines_ADDR, player);
}

static void __cdecl swrRace_UpdateSpinoutNodes_delta(swrRace *player) {
    call_spinout_visual(swrRace_UpdateSpinoutNodes_ADDR, player);
}

void ai_variance_RegisterHooks() {
    hook_function("swrRace_AnimateSpinoutEngines", (uint32_t) swrRace_AnimateSpinoutEngines_ADDR,
                  (uint8_t *) swrRace_AnimateSpinoutEngines_delta);
    hook_function("swrRace_UpdateSpinoutNodes", (uint32_t) swrRace_UpdateSpinoutNodes_ADDR,
                  (uint8_t *) swrRace_UpdateSpinoutNodes_delta);
    hook_function("swrRace_UpdateCatchup", (uint32_t) swrRace_UpdateCatchup_ADDR,
                  (uint8_t *) swrRace_UpdateCatchup_delta);
    swrRace_SetTiltOverride(ai_variance_tilt_override);
    hook_function("swrRace_UpdateWallContact", (uint32_t) swrRace_UpdateWallContact_ADDR,
                  (uint8_t *) swrRace_UpdateWallContact_delta);
    hook_function("swrRace_UpdateGroundContact", (uint32_t) swrRace_UpdateGroundContact_ADDR,
                  (uint8_t *) swrRace_UpdateGroundContact_delta);
    hook_function("swrRace_DeathSpeed", (uint32_t) swrRace_DeathSpeed_ADDR,
                  (uint8_t *) swrRace_DeathSpeed_delta);
    hook_function("swrRace_Explode", (uint32_t) swrRace_Explode_ADDR,
                  (uint8_t *) swrRace_Explode_delta);
    hook_function("swrRace_TakeDamage", (uint32_t) swrRace_TakeDamage_ADDR,
                  (uint8_t *) swrRace_TakeDamage_delta);
    hook_function("swrRace_UpdateSurfaceTag", (uint32_t) swrRace_UpdateSurfaceTag_ADDR,
                  (uint8_t *) swrRace_UpdateSurfaceTag_delta);
    hook_function("swrRace_UpdateSpeed", (uint32_t) swrRace_UpdateSpeed_ADDR,
                  (uint8_t *) swrRace_UpdateSpeed_delta);
    hook_function("swrObjTest_UpdatePhysicsContact",
                  (uint32_t) swrObjTest_UpdatePhysicsContact_ADDR,
                  (uint8_t *) swrObjTest_UpdatePhysicsContact_delta);
}

// ---------------------------------------------------------------------------------------------
// Config + panel

static const char *INI_SECTION = "ai_variance";

static void load_config() {
    g_enabled = config::get_int(INI_SECTION, "enabled", g_enabled) != 0;
    g_with_humans = config::get_int(INI_SECTION, "with_humans", g_with_humans) != 0;
    g_replace_stock =
        config::get_int(INI_SECTION, "replace_stock", g_replace_stock) != 0;
    g_wall_damage = config::get_int(INI_SECTION, "wall_damage", g_wall_damage) != 0;
    g_impact_death = config::get_int(INI_SECTION, "impact_death", g_impact_death) != 0;
    g_impact_toughness = config::get_float(INI_SECTION, "impact_toughness", g_impact_toughness);
    g_form_amp = config::get_float(INI_SECTION, "form_amp", g_form_amp);
    g_swing_amp = config::get_float(INI_SECTION, "swing_amp", g_swing_amp);
    g_swing_tau_s = config::get_float(INI_SECTION, "swing_tau_s", g_swing_tau_s);
    g_pack_gain = config::get_float(INI_SECTION, "pack_gain", g_pack_gain);
    g_pack_spread_s = config::get_float(INI_SECTION, "pack_spread_s", g_pack_spread_s);
    g_pack_clamp = config::get_float(INI_SECTION, "pack_clamp", g_pack_clamp);
    g_blunder_per_min = config::get_float(INI_SECTION, "blunder_per_min", g_blunder_per_min);
    g_blunder_min_s = config::get_float(INI_SECTION, "blunder_min_s", g_blunder_min_s);
    g_blunder_max_s = config::get_float(INI_SECTION, "blunder_max_s", g_blunder_max_s);
    g_blunder_depth = config::get_float(INI_SECTION, "blunder_depth", g_blunder_depth);
    g_boost = config::get_int(INI_SECTION, "boost", g_boost) != 0;
    g_flame = config::get_int(INI_SECTION, "flame", g_flame) != 0;
    g_flame_min_lap = config::get_int(INI_SECTION, "flame_min_lap", g_flame_min_lap);
    if (config::get_int(INI_SECTION, "cfg_version", 1) >= CFG_VERSION) {
        // v2: the more aggressive boost defaults replace whatever an older build stored
        g_boost_greedy = config::get_int(INI_SECTION, "boost_greedy", g_boost_greedy) != 0;
        g_boost_start_per_s = config::get_float(INI_SECTION, "boost_start_per_s", g_boost_start_per_s);
        g_boost_min_s = config::get_float(INI_SECTION, "boost_min_s", g_boost_min_s);
        g_boost_max_s = config::get_float(INI_SECTION, "boost_max_s", g_boost_max_s);
        g_boost_release_temp = config::get_float(INI_SECTION, "boost_release_temp", g_boost_release_temp);
        g_boost_min_start_temp =
            config::get_float(INI_SECTION, "boost_min_start_temp", g_boost_min_start_temp);
        enforce_gauge_span();
        g_boost_turn_limit = config::get_float(INI_SECTION, "boost_turn_limit", g_boost_turn_limit);
        g_boost_cooldown_s = config::get_float(INI_SECTION, "boost_cooldown_s", g_boost_cooldown_s);
        g_boost_p_overheat = config::get_float(INI_SECTION, "boost_p_overheat", g_boost_p_overheat);
        g_boost_sound = config::get_int(INI_SECTION, "boost_sound", g_boost_sound) != 0;
        g_boost_sfx_range = config::get_float(INI_SECTION, "boost_sfx_range", g_boost_sfx_range);
        g_boost_steer_scale = config::get_float(INI_SECTION, "boost_steer_scale", g_boost_steer_scale);
        g_corner = config::get_int(INI_SECTION, "corner", g_corner) != 0;
        g_corner_lat = config::get_float(INI_SECTION, "corner_lat", g_corner_lat);
        g_corner_decel = config::get_float(INI_SECTION, "corner_decel", g_corner_decel);
        g_corner_margin = config::get_float(INI_SECTION, "corner_margin", g_corner_margin);
        g_corner_samples = config::get_int(INI_SECTION, "corner_samples", g_corner_samples);
        g_corner_lift = config::get_float(INI_SECTION, "corner_lift", g_corner_lift);
        g_corner_coast = config::get_float(INI_SECTION, "corner_coast", g_corner_coast);
        g_corner_lean = config::get_int(INI_SECTION, "corner_lean", g_corner_lean) != 0;
        g_corner_lean_gain = config::get_float(INI_SECTION, "corner_lean_gain", g_corner_lean_gain);
        g_corner_slide = config::get_int(INI_SECTION, "corner_slide", g_corner_slide) != 0;
        g_corner_slide_over =
            config::get_float(INI_SECTION, "corner_slide_over", g_corner_slide_over);
        g_boost_corner_margin =
            config::get_float(INI_SECTION, "boost_corner_margin", g_boost_corner_margin);
        g_boost_corner_cancel =
            config::get_float(INI_SECTION, "boost_corner_cancel", g_boost_corner_cancel);
        g_boost_min_hold_s = config::get_float(INI_SECTION, "boost_min_hold_s", g_boost_min_hold_s);
        g_boost_shrug_scrapes =
            config::get_int(INI_SECTION, "boost_shrug_scrapes", g_boost_shrug_scrapes) != 0;
        g_boost_scrape_tol = config::get_float(INI_SECTION, "boost_scrape_tol", g_boost_scrape_tol);
        g_corner_log = config::get_int(INI_SECTION, "corner_log", g_corner_log) != 0;
        g_boost_roll = config::get_int(INI_SECTION, "boost_roll", g_boost_roll) != 0;
        g_boost_roll_chance = config::get_float(INI_SECTION, "boost_roll_chance", g_boost_roll_chance);
        g_boost_roll_min = config::get_float(INI_SECTION, "boost_roll_min", g_boost_roll_min);
        g_boost_roll_max = config::get_float(INI_SECTION, "boost_roll_max", g_boost_roll_max);
    }
}

static void save_config() {
    config::set_int(INI_SECTION, "cfg_version", CFG_VERSION);
    config::set_int(INI_SECTION, "enabled", g_enabled);
    config::set_int(INI_SECTION, "with_humans", g_with_humans);
    config::set_int(INI_SECTION, "replace_stock", g_replace_stock);
    config::set_int(INI_SECTION, "wall_damage", g_wall_damage);
    config::set_int(INI_SECTION, "impact_death", g_impact_death);
    config::set_float(INI_SECTION, "impact_toughness", g_impact_toughness);
    config::set_float(INI_SECTION, "form_amp", g_form_amp);
    config::set_float(INI_SECTION, "swing_amp", g_swing_amp);
    config::set_float(INI_SECTION, "swing_tau_s", g_swing_tau_s);
    config::set_float(INI_SECTION, "pack_gain", g_pack_gain);
    config::set_float(INI_SECTION, "pack_spread_s", g_pack_spread_s);
    config::set_float(INI_SECTION, "pack_clamp", g_pack_clamp);
    config::set_float(INI_SECTION, "blunder_per_min", g_blunder_per_min);
    config::set_float(INI_SECTION, "blunder_min_s", g_blunder_min_s);
    config::set_float(INI_SECTION, "blunder_max_s", g_blunder_max_s);
    config::set_float(INI_SECTION, "blunder_depth", g_blunder_depth);
    config::set_int(INI_SECTION, "boost", g_boost);
    config::set_int(INI_SECTION, "flame", g_flame);
    config::set_int(INI_SECTION, "flame_min_lap", g_flame_min_lap);
    config::set_int(INI_SECTION, "boost_greedy", g_boost_greedy);
    config::set_float(INI_SECTION, "boost_start_per_s", g_boost_start_per_s);
    config::set_float(INI_SECTION, "boost_min_s", g_boost_min_s);
    config::set_float(INI_SECTION, "boost_max_s", g_boost_max_s);
    config::set_float(INI_SECTION, "boost_release_temp", g_boost_release_temp);
    config::set_float(INI_SECTION, "boost_min_start_temp", g_boost_min_start_temp);
    config::set_float(INI_SECTION, "boost_turn_limit", g_boost_turn_limit);
    config::set_float(INI_SECTION, "boost_cooldown_s", g_boost_cooldown_s);
    config::set_float(INI_SECTION, "boost_p_overheat", g_boost_p_overheat);
    config::set_int(INI_SECTION, "boost_sound", g_boost_sound);
    config::set_float(INI_SECTION, "boost_sfx_range", g_boost_sfx_range);
    config::set_float(INI_SECTION, "boost_steer_scale", g_boost_steer_scale);
    config::set_int(INI_SECTION, "corner", g_corner);
    config::set_float(INI_SECTION, "corner_lat", g_corner_lat);
    config::set_float(INI_SECTION, "corner_decel", g_corner_decel);
    config::set_float(INI_SECTION, "corner_margin", g_corner_margin);
    config::set_int(INI_SECTION, "corner_samples", g_corner_samples);
    config::set_float(INI_SECTION, "corner_lift", g_corner_lift);
    config::set_float(INI_SECTION, "corner_coast", g_corner_coast);
    config::set_int(INI_SECTION, "corner_lean", g_corner_lean);
    config::set_float(INI_SECTION, "corner_lean_gain", g_corner_lean_gain);
    config::set_int(INI_SECTION, "corner_slide", g_corner_slide);
    config::set_float(INI_SECTION, "corner_slide_over", g_corner_slide_over);
    config::set_float(INI_SECTION, "boost_corner_margin", g_boost_corner_margin);
    config::set_float(INI_SECTION, "boost_corner_cancel", g_boost_corner_cancel);
    config::set_float(INI_SECTION, "boost_min_hold_s", g_boost_min_hold_s);
    config::set_int(INI_SECTION, "boost_shrug_scrapes", g_boost_shrug_scrapes);
    config::set_float(INI_SECTION, "boost_scrape_tol", g_boost_scrape_tol);
    config::set_int(INI_SECTION, "corner_log", g_corner_log);
    config::set_int(INI_SECTION, "boost_roll", g_boost_roll);
    config::set_float(INI_SECTION, "boost_roll_chance", g_boost_roll_chance);
    config::set_float(INI_SECTION, "boost_roll_min", g_boost_roll_min);
    config::set_float(INI_SECTION, "boost_roll_max", g_boost_roll_max);
    config::save();
}

static void panel_ai_variance() {
    ImGui::TextWrapped(
        "Closer, less predictable AI races: per-race form, slow pace swings, pack "
        "compression toward a target spread, and random blunders. Speed only; steering "
        "is untouched. Applies to all-AI races (optionally to the AI in yours).");
    bool changed = false;
    changed |= ImGui::Checkbox("Enabled", &g_enabled);
    changed |= ImGui::Checkbox("Also in races a human drives", &g_with_humans);
    changed |= ImGui::Checkbox("Replace stock pacing (equal speed + steering, no pace-setter)",
                               &g_replace_stock);
    changed |= ImGui::Checkbox("AI take wall damage (player scrape / impact path)", &g_wall_damage);
    changed |=
        ImGui::Checkbox("AI explode on death-speed impacts (like the player)", &g_impact_death);
    changed |= ImGui::SliderFloat("AI impact toughness (x death-speed thresholds)",
                                  &g_impact_toughness, 1.0f, 3.0f, "%.2f");
    ImGui::SeparatorText("Form + swings");
    changed |= ImGui::SliderFloat("Form amplitude (per race)", &g_form_amp, 0.0f, 0.15f, "%.3f");
    changed |= ImGui::SliderFloat("Swing amplitude", &g_swing_amp, 0.0f, 0.15f, "%.3f");
    changed |= ImGui::SliderFloat("Swing time constant (s)", &g_swing_tau_s, 1.0f, 30.0f, "%.0f");
    ImGui::SeparatorText("Pack compression");
    changed |= ImGui::SliderFloat("Target field spread (s)", &g_pack_spread_s, 0.0f, 30.0f, "%.1f");
    changed |= ImGui::SliderFloat("Gain (x per s of error)", &g_pack_gain, 0.0f, 0.1f, "%.3f");
    changed |= ImGui::SliderFloat("Clamp (+/-)", &g_pack_clamp, 0.0f, 0.5f, "%.2f");
    ImGui::SeparatorText("Blunders");
    changed |= ImGui::SliderFloat("Per racer per minute", &g_blunder_per_min, 0.0f, 4.0f, "%.2f");
    changed |= ImGui::SliderFloat("Min duration (s)", &g_blunder_min_s, 0.5f, 5.0f, "%.1f");
    changed |= ImGui::SliderFloat("Max duration (s)", &g_blunder_max_s, 0.5f, 8.0f, "%.1f");
    changed |= ImGui::SliderFloat("Depth (multiplier)", &g_blunder_depth, 0.2f, 1.0f, "%.2f");
    ImGui::SeparatorText("Sebulba");
    changed |= ImGui::Checkbox("Flame attack before lap 3 (stock AI waits)", &g_flame);
    changed |= ImGui::SliderInt("Flame attack from lap", &g_flame_min_lap, 0, 4);
    ImGui::SeparatorText("Boost");
    changed |= ImGui::Checkbox("AI boost", &g_boost);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("sound", &g_boost_sound);
    changed |= ImGui::Checkbox("Boost whenever the heat gauge allows (no dice, no timer)",
                               &g_boost_greedy);
    ImGui::TextWrapped("Boosting drains the engine gauge and coasting refills it, so the time "
                       "spent boosting over a race is fixed by the pod -- these two only choose "
                       "long-and-rare or short-and-often. A boost lasts as long as the gap between "
                       "them, so a wide gap is what makes boosts long.");
    changed |= ImGui::SliderFloat("Fire when the gauge climbs to", &g_boost_min_start_temp, 20.0f,
                                  100.0f, "%.0f");
    changed |= ImGui::SliderFloat("Hold until the gauge drops to", &g_boost_release_temp, 0.0f,
                                  90.0f, "%.0f");
    if (changed)
        enforce_gauge_span();
    ImGui::Text("=> each boost spends %.0f%% of the gauge",
                g_boost_min_start_temp - g_boost_release_temp);
    ImGui::BeginDisabled(g_boost_greedy);
    changed |= ImGui::SliderFloat("Start chance per s (when eligible)", &g_boost_start_per_s, 0.0f,
                                  2.0f, "%.2f");
    changed |= ImGui::SliderFloat("Min duration (s)##boost", &g_boost_min_s, 0.5f, 8.0f, "%.1f");
    changed |= ImGui::SliderFloat("Max duration (s)##boost", &g_boost_max_s, 0.5f, 12.0f, "%.1f");
    ImGui::EndDisabled();
    changed |=
        ImGui::SliderFloat("Straight: |turn| below", &g_boost_turn_limit, 5.0f, 400.0f, "%.0f");
    changed |= ImGui::SliderFloat("Cooldown (s)", &g_boost_cooldown_s, 0.0f, 20.0f, "%.1f");
    changed |= ImGui::SliderFloat("Chance to hold until overheat", &g_boost_p_overheat, 0.0f, 1.0f,
                                  "%.2f");
    changed |=
        ImGui::SliderFloat("Steering while boosting (x)", &g_boost_steer_scale, 0.1f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Do not start a boost below corner limit x",
                                  &g_boost_corner_margin, 0.5f, 2.0f, "%.2f");
    changed |= ImGui::SliderFloat("Drop a burning boost below corner limit x",
                                  &g_boost_corner_cancel, 0.3f, 1.2f, "%.2f");
    changed |= ImGui::SliderFloat("Minimum hold once lit (s)", &g_boost_min_hold_s, 0.0f, 5.0f,
                                  "%.1f");
    changed |= ImGui::Checkbox("Hold boosts through scrapes (the game cancels on any contact)",
                               &g_boost_shrug_scrapes);
    changed |= ImGui::SliderFloat("Shrug off hits below", &g_boost_scrape_tol, 0.0f, 0.3f, "%.3f");
    changed |= ImGui::Checkbox("Roll while boosting (random side, per boost)", &g_boost_roll);
    changed |= ImGui::SliderFloat("Share of boosts that roll", &g_boost_roll_chance, 0.0f, 1.0f,
                                  "%.2f");
    changed |= ImGui::SliderFloat("Roll amount: min", &g_boost_roll_min, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Roll amount: max", &g_boost_roll_max, 0.0f, 1.0f, "%.2f");
    ImGui::SeparatorText("Corners");
    ImGui::TextWrapped("The stock autopilot holds a flat throttle and never brakes -- it only "
                       "lifts once it has already missed the corner. This scans the spline ahead "
                       "and brakes for what is coming.");
    changed |= ImGui::Checkbox("Brake for corners", &g_corner);
    changed |= ImGui::SliderFloat("Cornering grip (units/s^2)", &g_corner_lat, 100.0f, 3000.0f,
                                  "%.0f");
    changed |= ImGui::SliderFloat("Safety margin (x limit)", &g_corner_margin, 0.4f, 1.5f, "%.2f");
    changed |= ImGui::SliderFloat("Assumed braking (units/s^2)", &g_corner_decel, 100.0f, 3000.0f,
                                  "%.0f");
    changed |= ImGui::SliderInt("Look-ahead samples", &g_corner_samples, 1, 16);
    changed |= ImGui::SliderFloat("Throttle while braking (x)", &g_corner_lift, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Throttle just under the limit (x)", &g_corner_coast, 0.0f, 1.0f,
                                  "%.2f");
    changed |= ImGui::Checkbox("Lean into corners", &g_corner_lean);
    changed |= ImGui::SliderFloat("Lean at full lock", &g_corner_lean_gain, 0.0f, 1.0f, "%.2f");
    changed |=
        ImGui::Checkbox("Slide in corners (drifty look, but really is less grip)", &g_corner_slide);
    changed |= ImGui::SliderFloat("Slide while over the limit by (x)", &g_corner_slide_over, 1.0f,
                                  2.0f, "%.2f");
    changed |= ImGui::Checkbox("Log corner decisions to hook.log (1/s)", &g_corner_log);
    if (changed)
        save_config();

    ImGui::Separator();
    ImGui::Text("Blunders so far: %d, boosts %d (%d rolling), flames %d, explosions %d, "
                "respawns %d, corner brakes %d",
                g_blunders_total, g_boosts_total, g_boost_rolls_total, g_flames_total,
                g_explosions_total, g_respawns_total, g_corner_brakes_total);
    ImGui::Text("Tilt calls: %d seen, %d taken over, last fed %+.2f", g_tilt_seen, g_tilt_calls,
                g_tilt_last_b);
    ImGui::Text("Boosts ended by: corner %d, turn %d, heat %d, timer %d, the game %d",
                g_boost_cancels_corner, g_boost_cancels_turn, g_boost_cancels_temp,
                g_boost_cancels_time, g_boost_cancels_extern);
    ImGui::Text("Boosts put out by: damage %d, terrain %d, air brake %d; scrapes shrugged off %d",
                g_kills_damage, g_kills_surface, g_kills_speed, g_boost_scrapes_shrugged);
    ImGui::TextWrapped(
        "A boost held to overheat leaves an engine alight, and the burning-engine damage tick "
        "cancels every later boost until the pod repairs -- so a high overheat chance is what "
        "makes boosts short, not the heat gauge. Frames spent waiting on a fire: %d.",
        g_boost_blocked_fire);
    if (ImGui::Button("Reseed form now"))
        reseed(GetTickCount());
    const RaceTelemetry *t = race_telemetry_Get();
    if (t->valid && ImGui::BeginTable("var", 8, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Racer");
        ImGui::TableSetupColumn("form");
        ImGui::TableSetupColumn("swing");
        ImGui::TableSetupColumn("mult");
        ImGui::TableSetupColumn("gap s");
        ImGui::TableSetupColumn("speed");
        ImGui::TableSetupColumn("limit");
        ImGui::TableSetupColumn("corner");
        ImGui::TableHeadersRow();
        for (int k = 0; k < t->n; k++) {
            const RaceTelemetryRow &r = t->rows[k];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.name);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", g_form[r.slot]);
            ImGui::TableNextColumn();
            ImGui::Text("%+.3f", g_swing[r.slot]);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", g_last_mult[r.slot]);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", r.gap_leader_s);
            const swrRace *pod = swrScoresPtr != NULL ? swrScoresPtr[r.slot].obj_test_ptr : NULL;
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", pod != NULL ? pod->speedValue : 0.0f);
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", g_corner_v_allow[r.slot] > 1e8f ? 0.0f : g_corner_v_allow[r.slot]);
            ImGui::TableNextColumn();
            ImGui::Text("%s r%.0f", g_corner_braking[r.slot] ? "BRAKE" : "-",
                        g_corner_k[r.slot] > 1e-5f ? 1.0f / g_corner_k[r.slot] : 0.0f);
        }
        ImGui::EndTable();
    }
}

static DebugPanel g_panel = {.category = "Race",
                             .name = "AI Variance",
                             .draw = panel_ai_variance,
                             .dev_only = false};

void ai_variance_RegisterPanel() {
    load_config();
    save_config();// stamps cfg_version so the one-time default override does not repeat
    srand((unsigned) GetTickCount());
    debug_ui_register(&g_panel);
}
