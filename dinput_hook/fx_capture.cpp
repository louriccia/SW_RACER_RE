#include "fx_capture.h"
#include "hook_helper.h"
#include "debug_ui.h"
#include "imgui_utils.h"
#include "config.h"

#include <imgui.h>

#include <windows.h>
#include <algorithm>
#include <vector>

extern "C" {
#include <Primitives/rdVector.h>
#include <Swr/swrEvent.h>// swrEvent_GetEventCount, swrEvent_CallAllF0
#include <Swr/swrObj.h>  // swrRace_Explode_ADDR, swrObjSmok_Spawn
#include <Swr/swrRace.h> // swrRace_SpawnFlameAttack_ADDR
#include <globals.h>     // currentPlayer_Test, fireballChildNodesPtr, fireballNodePtr, swrUtils_randState
}

namespace {

constexpr int SMOK_EVENT = 0x536d6f6b;// 'Smok', the particle entity's event tag
constexpr int SMOK_NODES_PER_OBJ = 5; // fireballChildNodesPtr holds 5 nodes per Smok slot
constexpr int TRIGGER_KEY = VK_F10;   // F9 is the free camera
constexpr int FLAME_KEY = VK_F11;
constexpr double STEP_DT = 1.0 / 60.0;// one tick's worth of time for the frame-step button

// Engine-select bits of swrRace_Explode's second argument (bit 1 = left, bit 2 = right).
enum ExplodeEngines {
    EXPLODE_LEFT = 1,
    EXPLODE_RIGHT = 2,
    EXPLODE_BOTH = 3,
};

// The four configurations swrObjSmok_Spawn actually sets up; any other type falls through its
// switch with every field left zeroed. They share the particle pool, the spin/length curves and the
// 3s recycle period -- they differ only in drift axis, end width and UV scroll rate.
struct BurstType {
    const char *name;
    int type;
};
const BurstType BURST_TYPES[] = {
    {"Explosion (death burst)", swrObjSmok_TYPE_EXPLOSION},
    {"Flamejet (Sebulba's taunt)", swrObjSmok_TYPE_FLAME_ATTACK},
    {"Engine damage smoke", swrObjSmok_TYPE_ENGINE_SMOKE},
    {"Fire (burning track prop)", swrObjSmok_TYPE_FIRE},
};
constexpr int BURST_TYPE_COUNT = (int) (sizeof(BURST_TYPES) / sizeof(BURST_TYPES[0]));

struct MatteColor {
    const char *name;
    float rgba[4];
};
// Black first: the FX are additive / alpha billboards, so a black plate is already premultiplied --
// alpha comes straight off luminance. The others are for the two-pass alpha lift, and for judging
// how much dark smoke a burst carries.
const MatteColor MATTE_COLORS[] = {
    {"Black", {0.0f, 0.0f, 0.0f, 1.0f}},
    {"White", {1.0f, 1.0f, 1.0f, 1.0f}},
    {"Green", {0.0f, 1.0f, 0.0f, 1.0f}},
    {"Magenta", {1.0f, 0.0f, 1.0f, 1.0f}},
    {"Mid grey", {0.5f, 0.5f, 0.5f, 1.0f}},
};
constexpr int MATTE_COLOR_COUNT = (int) (sizeof(MATTE_COLORS) / sizeof(MATTE_COLORS[0]));

struct Config {
    bool matte = false;
    int matte_color = 0;
    bool keep_pod = true;
    bool hide_hud = false;
    int engines = EXPLODE_BOTH;
    bool lock_seed = true;
    int seed = 0x2750250;// swrUtils_Rand's own self-seed value
    int burst_type = 0;// index into BURST_TYPES
    float burst_scale = 1.0f;
    float burst_lifetime = 1.0f;
    float burst_offset[3] = {0.0f, 0.0f, 0.0f};
    float time_scale = 1.0f;
};
Config g_cfg;

bool g_paused = false;
bool g_step_request = false;   // frame-step button: run one tick's worth of time while paused
bool g_explode_request = false;// serviced on the sim tick, not mid-overlay
bool g_burst_request = false;
bool g_flame_request = false;
bool g_trigger_was_down = false;
bool g_flame_was_down = false;

bool g_saved_fog = true;// imgui_state.enable_fog, handed back when the matte goes off
bool g_fog_saved = false;

// This frame's FX node set, sorted for binary search: the Smok particle slots plus the shared
// fireball node. Rebuilt per viewport render -- the pool pointer is only valid inside a race.
std::vector<const swrModel_Node *> g_fx_nodes;

void load_config() {
    g_cfg.matte_color = config::get_int("fx_capture", "matte_color", g_cfg.matte_color);
    g_cfg.keep_pod = config::get_int("fx_capture", "keep_pod", g_cfg.keep_pod) != 0;
    g_cfg.hide_hud = config::get_int("fx_capture", "hide_hud", g_cfg.hide_hud) != 0;
    g_cfg.engines = config::get_int("fx_capture", "engines", g_cfg.engines);
    g_cfg.lock_seed = config::get_int("fx_capture", "lock_seed", g_cfg.lock_seed) != 0;
    g_cfg.seed = config::get_int("fx_capture", "seed", g_cfg.seed);
    g_cfg.burst_type = std::clamp(config::get_int("fx_capture", "burst_type", g_cfg.burst_type),
                                  0, BURST_TYPE_COUNT - 1);
    g_cfg.burst_scale = config::get_float("fx_capture", "burst_scale", g_cfg.burst_scale);
    g_cfg.burst_lifetime = config::get_float("fx_capture", "burst_lifetime", g_cfg.burst_lifetime);
    g_cfg.time_scale = config::get_float("fx_capture", "time_scale", g_cfg.time_scale);
    g_cfg.matte_color = std::clamp(g_cfg.matte_color, 0, MATTE_COLOR_COUNT - 1);
}

void save_config() {
    config::set_int("fx_capture", "matte_color", g_cfg.matte_color);
    config::set_bool("fx_capture", "keep_pod", g_cfg.keep_pod);
    config::set_bool("fx_capture", "hide_hud", g_cfg.hide_hud);
    config::set_int("fx_capture", "engines", g_cfg.engines);
    config::set_bool("fx_capture", "lock_seed", g_cfg.lock_seed);
    config::set_int("fx_capture", "seed", g_cfg.seed);
    config::set_int("fx_capture", "burst_type", g_cfg.burst_type);
    config::set_float("fx_capture", "burst_scale", g_cfg.burst_scale);
    config::set_float("fx_capture", "burst_lifetime", g_cfg.burst_lifetime);
    config::set_float("fx_capture", "time_scale", g_cfg.time_scale);
    config::save();
}

// The matte only means anything against a live 3D scene.
bool in_race() {
    return currentPlayer_Test != nullptr;
}

// Fog tints the plume with the track's horizon colour, which is exactly what the matte is there to
// remove. Park the user's setting while the matte is up and hand it back afterwards.
void sync_fog(bool matte_on) {
    if (matte_on && !g_fog_saved) {
        g_saved_fog = imgui_state.enable_fog;
        g_fog_saved = true;
        imgui_state.enable_fog = false;
    } else if (!matte_on && g_fog_saved) {
        imgui_state.enable_fog = g_saved_fog;
        g_fog_saved = false;
    }
}

void lock_seed() {
    if (!g_cfg.lock_seed)
        return;
    swrUtils_randState = g_cfg.seed;
    swrUtils_randInitialized = 1;
}

// Fire the game's own explosion on the local pod. swrRace_Explode has no reimplemented body (it is
// a HANG stub), so go through its address like the other original-only calls in the overlay.
void fire_explosion(swrRace *pod) {
    lock_seed();
    ((void (*) (swrRace *, char) ) swrRace_Explode_ADDR)(pod, (char) g_cfg.engines);
}

// The real flame attack, as Sebulba throws it: plays the flame-burst sounds and attaches an
// owner-managed type-8 Smok to the pod, which the pod then carries. Like swrRace_Explode this has
// no reimplemented body, so call it through its address.
void fire_flamejet(swrRace *pod) {
    lock_seed();
    ((void (*) (swrRace *) ) swrRace_SpawnFlameAttack_ADDR)(pod);
}

// A bare particle burst with no pod involvement, placed relative to the pod. Isolates the
// smoke/fire billboards from the fireball debris, and lets the plume be sized freely.
void spawn_burst(swrRace *pod) {
    lock_seed();
    rdVector3 pos;
    pos.x = pod->transform.vD.x + g_cfg.burst_offset[0];
    pos.y = pod->transform.vD.y + g_cfg.burst_offset[1];
    pos.z = pod->transform.vD.z + g_cfg.burst_offset[2];
    const int type = BURST_TYPES[std::clamp(g_cfg.burst_type, 0, BURST_TYPE_COUNT - 1)].type;
    swrObjSmok_Spawn(type, 0, g_cfg.burst_lifetime, &pos, g_cfg.burst_scale);
}

void panel_fx_capture() {
    const bool racing = in_race();
    if (!racing)
        ImGui::TextDisabled("Not in a race - load one to stage the shot.");

    bool dirty = false;

    ImGui::SeparatorText("Stage");
    ImGui::Checkbox("Matte background (hide the world)", &g_cfg.matte);
    ImGui::BeginDisabled(!g_cfg.matte);
    const char *color_names[MATTE_COLOR_COUNT];
    for (int i = 0; i < MATTE_COLOR_COUNT; i++)
        color_names[i] = MATTE_COLORS[i].name;
    dirty |= ImGui::Combo("Matte colour", &g_cfg.matte_color, color_names, MATTE_COLOR_COUNT);
    dirty |= ImGui::Checkbox("Keep the pod", &g_cfg.keep_pod);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The pod's own debris and engine glow read as part of the burst.\n"
                          "Off = the plume alone.");
    ImGui::EndDisabled();
    dirty |= ImGui::Checkbox("Hide HUD", &g_cfg.hide_hud);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("In-race only - the front-end menus share the same sprite/text path.");
    ImGui::TextDisabled("Park the shot with the free camera (F9).");

    ImGui::SeparatorText("Trigger");
    dirty |= ImGui::RadioButton("Left engine", &g_cfg.engines, EXPLODE_LEFT);
    ImGui::SameLine();
    dirty |= ImGui::RadioButton("Right", &g_cfg.engines, EXPLODE_RIGHT);
    ImGui::SameLine();
    dirty |= ImGui::RadioButton("Both", &g_cfg.engines, EXPLODE_BOTH);
    ImGui::BeginDisabled(!racing);
    if (ImGui::Button("Trigger explosion"))
        g_explode_request = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("(F10)");
    ImGui::BeginDisabled(!racing);
    if (ImGui::Button("Trigger flamejet"))
        g_flame_request = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("(F11)");
    ImGui::SameLine();
    // One jet at a time: the pod holds the handle until the Smok expires, so a retrigger while it
    // is live does nothing. Show which it is rather than letting the button look dead.
    if (racing && currentPlayer_Test->flameSmokeHandle != nullptr)
        ImGui::TextDisabled("- jet live");
    else
        ImGui::TextDisabled("- idle");
    ImGui::TextWrapped("The explosion's fireball needs a free fx-animation slot, so back-to-back "
                       "triggers give smoke only - let the previous one finish. The flamejet rides "
                       "the pod and moves with it; the explosion is fixed where it fires.");

    dirty |= ImGui::Checkbox("Lock RNG seed", &g_cfg.lock_seed);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The burst's orientation and scale come from swrUtils_Rand.\n"
                          "Locking the seed replays the identical burst every trigger - which is\n"
                          "what makes a black pass and a white pass line up frame for frame.");
    ImGui::BeginDisabled(!g_cfg.lock_seed);
    dirty |= ImGui::InputInt("Seed", &g_cfg.seed);
    ImGui::EndDisabled();

    ImGui::SeparatorText("Standalone particle burst");
    const char *burst_names[BURST_TYPE_COUNT];
    for (int i = 0; i < BURST_TYPE_COUNT; i++)
        burst_names[i] = BURST_TYPES[i].name;
    dirty |= ImGui::Combo("Type", &g_cfg.burst_type, burst_names, BURST_TYPE_COUNT);
    dirty |= ImGui::SliderFloat("Scale", &g_cfg.burst_scale, 0.1f, 20.0f, "%.2f");
    dirty |= ImGui::SliderFloat("Lifetime", &g_cfg.burst_lifetime, 0.1f, 30.0f, "%.2f s");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Not a stretch of the burst: the 5 billboards recycle through the whole\n"
                          "birth-to-death curve every 3s, so lifetime buys more cycles. Past ~3s\n"
                          "any type reads as a sustained jet rather than a one-shot.");
    dirty |= ImGui::SliderFloat3("Offset from pod", g_cfg.burst_offset, -500.0f, 500.0f, "%.0f");
    ImGui::BeginDisabled(!racing);
    if (ImGui::Button("Spawn burst"))
        g_burst_request = true;
    ImGui::EndDisabled();

    ImGui::SeparatorText("Time");
    dirty |= ImGui::SliderFloat("Time scale", &g_cfg.time_scale, 0.02f, 2.0f, "%.2f x");
    if (ImGui::Button(g_paused ? "Resume" : "Pause"))
        g_paused = !g_paused;
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_paused);
    if (ImGui::Button("Step one tick"))
        g_step_request = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("1x")) {
        g_cfg.time_scale = 1.0f;
        dirty = true;
    }

    sync_fog(g_cfg.matte);

    // Persist once the user finishes editing (no widget active), not every drag frame.
    static bool pending = false;
    if (dirty)
        pending = true;
    if (pending && !ImGui::IsAnyItemActive()) {
        save_config();
        pending = false;
    }
}

DebugPanel g_panel_fx_capture = {
    .category = "Capture", .name = "FX Capture", .draw = panel_fx_capture, .dev_only = false};

bool game_has_focus() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

}// namespace

// Per-frame sim seam. swrEvent_CallAllF0 ticks every live entity, so scaling the frame delta here
// slows the pod, the particles and their lifetimes together, and everything downstream this frame
// (F1/F2/F3 and the render) sees the same scaled value. The game recomputes the delta each frame,
// so there is nothing to restore -- but guard against the value we wrote coming back unchanged, or
// the scale would compound toward zero.
extern "C" void __cdecl swrEvent_CallAllF0_delta(void) {
    static double last_written = -1.0;

    const bool stepping = g_paused && g_step_request;
    g_step_request = false;
    if (swrRace_deltaTimeSecs != last_written) {
        if (stepping)
            swrRace_deltaTimeSecs = STEP_DT;
        else if (g_paused)
            swrRace_deltaTimeSecs = 0.0;
        else if (g_cfg.time_scale != 1.0f)
            swrRace_deltaTimeSecs *= g_cfg.time_scale;
        last_written = swrRace_deltaTimeSecs;
    }

    // Hotkey edge, plus the panel's buttons: service them on the tick rather than mid-overlay.
    const bool focused = game_has_focus();
    const bool trigger_down = focused && (GetAsyncKeyState(TRIGGER_KEY) & 0x8000) != 0;
    const bool trigger_edge = trigger_down && !g_trigger_was_down;
    g_trigger_was_down = trigger_down;
    const bool flame_down = focused && (GetAsyncKeyState(FLAME_KEY) & 0x8000) != 0;
    const bool flame_edge = flame_down && !g_flame_was_down;
    g_flame_was_down = flame_down;

    swrRace *pod = currentPlayer_Test;
    if ((g_explode_request || trigger_edge) && pod != nullptr)
        fire_explosion(pod);
    if ((g_flame_request || flame_edge) && pod != nullptr)
        fire_flamejet(pod);
    if (g_burst_request && pod != nullptr)
        spawn_burst(pod);
    g_explode_request = false;
    g_flame_request = false;
    g_burst_request = false;

    hook_call_original(swrEvent_CallAllF0);
}

void fxcapture_RegisterHooks() {
    hook_replace(swrEvent_CallAllF0, swrEvent_CallAllF0_delta);
}

void fxcapture_RegisterPanel() {
    load_config();
    debug_ui_register(&g_panel_fx_capture);
}

bool fxcapture_MatteHidesWorld() {
    return g_cfg.matte && in_race();
}

void fxcapture_MatteColor(float out_rgba[4]) {
    const int i = std::clamp(g_cfg.matte_color, 0, MATTE_COLOR_COUNT - 1);
    for (int k = 0; k < 4; k++)
        out_rgba[k] = MATTE_COLORS[i].rgba[k];
}

bool fxcapture_KeepPod() {
    return g_cfg.keep_pod;
}

bool fxcapture_HudHidden() {
    // In-race only. The 2D front-end is drawn entirely through the same sprite/text path, so an
    // unconditional hide blanks the menus from boot.
    return g_cfg.hide_hud && in_race();
}

void fxcapture_BeginFrame() {
    g_fx_nodes.clear();
    if (!fxcapture_MatteHidesWorld())
        return;

    // The particle nodes are a fixed pool handed to swrObjSmok_SetFireballChildNodesPtr at track
    // load: 5 slots per Smok entity, indexed by the entity's id. Taking the whole pool covers every
    // burst alive this frame; the slots of expired ones are hidden by their own node flags anyway.
    if (fireballChildNodesPtr != nullptr) {
        const int pool = swrEvent_GetEventCount(SMOK_EVENT) * SMOK_NODES_PER_OBJ;
        for (int i = 0; i < pool; i++) {
            if (fireballChildNodesPtr[i] != nullptr)
                g_fx_nodes.push_back(fireballChildNodesPtr[i]);
        }
    }
    if (fireballNodePtr != nullptr)
        g_fx_nodes.push_back(fireballNodePtr);

    std::sort(g_fx_nodes.begin(), g_fx_nodes.end());
    g_fx_nodes.erase(std::unique(g_fx_nodes.begin(), g_fx_nodes.end()), g_fx_nodes.end());
}

bool fxcapture_IsFxNode(const swrModel_Node *node) {
    return std::binary_search(g_fx_nodes.begin(), g_fx_nodes.end(), node);
}
