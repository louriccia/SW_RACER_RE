#include "player_camera.h"
#include "../hook_helper.h"
#include "../debug_ui.h"
#include "../imgui_utils.h"

#include <imgui.h>

// Defined in hook_helper.cpp (registers a raw game-address detour); not prototyped in hook_helper.h.
extern "C" void hook_function(const char *function_name, uint32_t original_address,
                              uint8_t *hook_address);
// Defined in imgui_utils.cpp (persists imgui_state to [settings]); not prototyped in imgui_utils.h.
void save_settings_ini();

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

extern "C" {
#include <Primitives/rdMatrix.h>
#include <Primitives/rdVector.h>
#include <Swr/swrObj.h>      // swrObjcMan_* detour targets
#include <Swr/swrModel.h>    // BuildLookAtTransform, swrModel_NodeModifyFlags, sun / light-streak updaters
#include <Swr/swrPlayerHUD.h>// swrPlayerHUD_RenderWorldSprites_ADDR
#include <globals.h>         // swrRacer_PodVisualData, g_objHang2
}

namespace {

// swrObjcMan_UpdateCamera modes the player can drive in; spline / sweep / death modes are left alone.
enum CamMode {
    CAM_MODE_CHASE_NEAR = 1,
    CAM_MODE_CHASE_FAR = 2, // chase with doubled trail / height
    CAM_MODE_FIRST_PERSON = 4,
    CAM_MODE_FIRST_PERSON_WIDE = 5,// first person at the 120-degree FOV
};

// Panel choices -> forced mode (0 = the game's own).
constexpr int VIEW_MODES[] = {0, CAM_MODE_CHASE_NEAR, CAM_MODE_CHASE_FAR, CAM_MODE_FIRST_PERSON,
                              CAM_MODE_FIRST_PERSON_WIDE};
const char *const VIEW_NAMES[] = {"Game default", "Chase (near)", "Chase (far)", "First person",
                                  "First person (wide)"};

constexpr int NUM_PILOTS = 23;// swrRacer_PodVisualData[23]

struct PlayerCameraConfig {
    int view = 0;               // index into VIEW_MODES
    float trail_scale = 1.0f;   // chase-camera distance multiplier
    float height_scale = 1.0f;  // chase-camera height multiplier
    float fov_offset = 0.0f;    // degrees added to the game's camera FOV
    float dynamic_fov = 0.0f;   // extra degrees at top speed (0 = off), eased in with speed^2
    bool disable_roll = false;  // level the horizon (drop the chase cam's banking)
    float cockpit_right = 0.0f; // first-person camera offset, pod-local axes (units)
    float cockpit_forward = 0.0f;
    float cockpit_up = 0.0f;
    bool show_pod_first_person = false;// keep the pod visible in the first-person views
    bool hide_own_pod = false;
    bool hide_hud = false;
    bool hide_guide_arrow = false;
    bool hide_suns = false;        // suns + lens flares
    bool hide_light_streaks = false;
    // Perlin camera shake (port of the GLSEPRAC Cheat Engine camera mod); gains on its amplitudes.
    float shake_intensity = 0.0f;// master gain; 0 = off
    float shake_speed = 6.0f;    // noise-time units per second (the mod advanced 0.1 per tick)
    float shake_boost = 1.0f;    // gain on the steady boost rumble
    float shake_boost_burst = 1.0f;// gain on the boost-onset burst
    float shake_vibration = 1.0f;// gain on the collision / terrain vibration term
};
PlayerCameraConfig g_cfg;

// Mode the player had before the override, restored when it is switched back off.
bool g_view_forced = false;
int g_view_saved = 0;
int g_view_forced_mode = 0;

enum SpriteGroup { GROUP_NONE, GROUP_SUN, GROUP_LIGHT_STREAKS };
SpriteGroup g_sprite_group = GROUP_NONE;

bool in_race() {
    // hangar / front-end active screen <=> flag bit 0
    swrObjHang *hang = g_objHang2;
    return hang == nullptr || (hang->flag & 1) == 0;
}

bool is_player_view(int mode) {
    return mode == CAM_MODE_CHASE_NEAR || mode == CAM_MODE_CHASE_FAR ||
           mode == CAM_MODE_FIRST_PERSON || mode == CAM_MODE_FIRST_PERSON_WIDE;
}

bool is_first_person(int mode) {
    return mode == CAM_MODE_FIRST_PERSON || mode == CAM_MODE_FIRST_PERSON_WIDE;
}

// Followed racer if it is the local player's pod; spectator / demo cameras are left alone.
swrRace *local_followed_racer(swrObjcMan *cman) {
    swrRace *racer = cman->unkf4_objTest;
    if (racer == nullptr || (racer->flags0 & swrObjTest_FLAG0_LOCAL) == 0)
        return nullptr;
    return racer;
}

void apply_view_override(swrObjcMan *cman) {
    const int want = VIEW_MODES[std::clamp(g_cfg.view, 0, 4)];
    if (want != 0 && local_followed_racer(cman) != nullptr && is_player_view(cman->mode_type)) {
        if (!g_view_forced) {
            g_view_saved = cman->mode_type;
            g_view_forced = true;
        }
        cman->mode_type = want;
        g_view_forced_mode = want;
        return;
    }
    if (g_view_forced && want == 0) {
        if (cman->mode_type == g_view_forced_mode)
            cman->mode_type = g_view_saved;
        g_view_forced = false;
    }
}

// Offset camera and aim point together, in pod axes (vA right, vB forward, vC up).
void apply_cockpit_offset(swrObjcMan *cman, swrRace *racer) {
    if (g_cfg.cockpit_right == 0.0f && g_cfg.cockpit_forward == 0.0f && g_cfg.cockpit_up == 0.0f)
        return;
    const rdMatrix44 &pod = racer->transform;
    rdVector3 off;
    off.x = pod.vA.x * g_cfg.cockpit_right + pod.vB.x * g_cfg.cockpit_forward +
            pod.vC.x * g_cfg.cockpit_up;
    off.y = pod.vA.y * g_cfg.cockpit_right + pod.vB.y * g_cfg.cockpit_forward +
            pod.vC.y * g_cfg.cockpit_up;
    off.z = pod.vA.z * g_cfg.cockpit_right + pod.vB.z * g_cfg.cockpit_forward +
            pod.vC.z * g_cfg.cockpit_up;
    rdVector3 *cam = (rdVector3 *) &cman->unk20_mat.vD;
    rdVector3 *focus = (rdVector3 *) &cman->focusTransform_mat.vD;
    rdVector_Scale3Add3(cam, cam, 1.0f, &off);
    rdVector_Scale3Add3(focus, focus, 1.0f, &off);
}

// Same look-at rebuild swrObjcMan_UpdateCamera ends with, roll forced to 0.
void remove_roll(swrObjcMan *cman) {
    rdMatrix44 level;
    swrTranslationRotation tr;
    BuildLookAtTransform((rdVector3 *) &cman->unk20_mat.vD, (rdVector3 *) &cman->focusTransform_mat.vD,
                         &level, &tr, 0.0f);
    rdMatrix_Copy44(&cman->unk20_mat, &level);
}

// Ken Perlin's improved noise, same permutation table as the CE mod.
namespace perlin {
constexpr unsigned char PERMUTATION[256] = {
    151, 160, 137, 91,  90,  15,  131, 13,  201, 95,  96,  53,  194, 233, 7,   225, 140, 36,  103, 30,
    69,  142, 8,   99,  37,  240, 21,  10,  23,  190, 6,   148, 247, 120, 234, 75,  0,   26,  197, 62,
    94,  252, 219, 203, 117, 35,  11,  32,  57,  177, 33,  88,  237, 149, 56,  87,  174, 20,  125, 136,
    171, 168, 68,  175, 74,  165, 71,  134, 139, 48,  27,  166, 77,  146, 158, 231, 83,  111, 229, 122,
    60,  211, 133, 230, 220, 105, 92,  41,  55,  46,  245, 40,  244, 102, 143, 54,  65,  25,  63,  161,
    1,   216, 80,  73,  209, 76,  132, 187, 208, 89,  18,  169, 200, 196, 135, 130, 116, 188, 159, 86,
    164, 100, 109, 198, 173, 186, 3,   64,  52,  217, 226, 250, 124, 123, 5,   202, 38,  147, 118, 126,
    255, 82,  85,  212, 207, 206, 59,  227, 47,  16,  58,  17,  182, 189, 28,  42,  223, 183, 170, 213,
    119, 248, 152, 2,   44,  154, 163, 70,  221, 153, 101, 155, 167, 43,  172, 9,   129, 22,  39,  253,
    19,  98,  108, 110, 79,  113, 224, 232, 178, 185, 112, 104, 218, 246, 97,  228, 251, 34,  242, 193,
    238, 210, 144, 12,  191, 179, 162, 241, 81,  51,  145, 235, 249, 14,  239, 107, 49,  192, 214, 31,
    181, 199, 106, 157, 184, 84,  204, 176, 115, 121, 50,  45,  127, 4,   150, 254, 138, 236, 205, 93,
    222, 114, 67,  29,  24,  72,  243, 141, 128, 195, 78,  66,  215, 61,  156, 180};

inline int p(int i) {
    return PERMUTATION[i & 255];
}
inline float fade(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}
inline float lerp(float t, float a, float b) {
    return a + t * (b - a);
}
inline float grad(int hash, float x, float y, float z) {
    const int h = hash & 15;
    const float u = h < 8 ? x : y;
    const float v = h < 4 ? y : ((h == 12 || h == 14) ? x : z);
    return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
}
float noise(float x, float y, float z) {
    const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const int X = (int) fx & 255, Y = (int) fy & 255, Z = (int) fz & 255;
    x -= fx;
    y -= fy;
    z -= fz;
    const float u = fade(x), v = fade(y), w = fade(z);
    const int A = p(X) + Y, AA = p(A) + Z, AB = p(A + 1) + Z;
    const int B = p(X + 1) + Y, BA = p(B) + Z, BB = p(B + 1) + Z;
    return lerp(w,
                lerp(v, lerp(u, grad(p(AA), x, y, z), grad(p(BA), x - 1, y, z)),
                     lerp(u, grad(p(AB), x, y - 1, z), grad(p(BB), x - 1, y - 1, z))),
                lerp(v, lerp(u, grad(p(AA + 1), x, y, z - 1), grad(p(BA + 1), x - 1, y, z - 1)),
                     lerp(u, grad(p(AB + 1), x, y - 1, z - 1), grad(p(BB + 1), x - 1, y - 1, z - 1))));
}
}// namespace perlin

// The mod's amplitudes (degrees): boost rumble noise*4*0.15, onset burst / vibration noise*5.
constexpr float SHAKE_BOOST_AMPLITUDE_DEG = 4.0f * 0.15f;
constexpr float SHAKE_BURST_AMPLITUDE_DEG = 5.0f;
constexpr float SHAKE_VIBRATION_AMPLITUDE_DEG = 5.0f;
constexpr float SHAKE_BURST_PEAK = 25.0f / 50.0f;
constexpr float SHAKE_BURST_SECONDS = 25.0f / 60.0f;// 25 ticks of the mod's ~60 Hz timer

float g_shake_time = 0.0f; // noise-space time
float g_shake_burst = 0.0f;// boost-onset burst, seconds remaining
bool g_shake_was_boosting = false;

// Yaw / pitch / roll (degrees) about the camera's own up / right / forward axes.
void apply_shake(swrObjcMan *cman, swrRace *racer) {
    if (g_cfg.shake_intensity <= 0.0f)
        return;
    const float dt = swrRace_deltaTimeSecs;
    g_shake_time += dt * g_cfg.shake_speed;

    const bool boosting = (racer->flags0 & swrObjTest_FLAG0_BOOSTING) != 0;
    if (boosting && !g_shake_was_boosting)
        g_shake_burst = SHAKE_BURST_SECONDS;
    g_shake_was_boosting = boosting;
    g_shake_burst = std::max(0.0f, g_shake_burst - dt);

    const float t = g_shake_time;
    const float boost = boosting ? SHAKE_BOOST_AMPLITUDE_DEG * g_cfg.shake_boost : 0.0f;
    const float burst = SHAKE_BURST_AMPLITUDE_DEG * SHAKE_BURST_PEAK *
                        (g_shake_burst / SHAKE_BURST_SECONDS) * g_cfg.shake_boost_burst;
    const float vib = std::clamp(racer->unk2b8, 0.0f, 1.0f);
    const float vibration = SHAKE_VIBRATION_AMPLITUDE_DEG * vib * vib * g_cfg.shake_vibration;

    const float g = g_cfg.shake_intensity;
    const float yaw =
        g * (perlin::noise(t, 0, 50) * boost + perlin::noise(-t, 0, 50) * (burst + vibration));
    const float pitch =
        g * (perlin::noise(0, t, 50) * boost + perlin::noise(0, -t, 50) * (burst + vibration));
    const float roll =
        g * (perlin::noise(50, 50, t) * boost + perlin::noise(0, 0, -t) * (burst + vibration));
    if (yaw == 0.0f && pitch == 0.0f && roll == 0.0f)
        return;

    rdMatrix44 in = cman->unk20_mat;
    rdMatrix_AddRotationFromVectorAngle44Before(&cman->unk20_mat, yaw, 0.0f, 0.0f, 1.0f, &in);
    in = cman->unk20_mat;
    rdMatrix_AddRotationFromVectorAngle44Before(&cman->unk20_mat, pitch, 1.0f, 0.0f, 0.0f, &in);
    in = cman->unk20_mat;
    rdMatrix_AddRotationFromVectorAngle44Before(&cman->unk20_mat, roll, 0.0f, 1.0f, 0.0f, &in);
}

constexpr const wchar_t *INI_SECTION = L"player_camera";

float ini_get_float(const wchar_t *ini, const wchar_t *key, float def) {
    wchar_t got[48], defbuf[48];
    swprintf(defbuf, 48, L"%.4f", def);
    GetPrivateProfileStringW(INI_SECTION, key, defbuf, got, 48, ini);
    return (float) wcstod(got, nullptr);
}
void ini_set_float(const wchar_t *ini, const wchar_t *key, float v) {
    wchar_t buf[48];
    swprintf(buf, 48, L"%.4f", v);
    WritePrivateProfileStringW(INI_SECTION, key, buf, ini);
}
bool ini_get_bool(const wchar_t *ini, const wchar_t *key, bool def) {
    return GetPrivateProfileIntW(INI_SECTION, key, def, ini) != 0;
}
void ini_set_bool(const wchar_t *ini, const wchar_t *key, bool v) {
    WritePrivateProfileStringW(INI_SECTION, key, v ? L"1" : L"0", ini);
}

void load_config() {
    const wchar_t *ini = settings_ini_path();
    g_cfg.view = std::clamp((int) GetPrivateProfileIntW(INI_SECTION, L"view", g_cfg.view, ini), 0, 4);
    g_cfg.trail_scale = ini_get_float(ini, L"trail_scale", g_cfg.trail_scale);
    g_cfg.height_scale = ini_get_float(ini, L"height_scale", g_cfg.height_scale);
    g_cfg.fov_offset = ini_get_float(ini, L"fov_offset", g_cfg.fov_offset);
    g_cfg.dynamic_fov = ini_get_float(ini, L"dynamic_fov", g_cfg.dynamic_fov);
    g_cfg.disable_roll = ini_get_bool(ini, L"disable_roll", g_cfg.disable_roll);
    g_cfg.cockpit_right = ini_get_float(ini, L"cockpit_right", g_cfg.cockpit_right);
    g_cfg.cockpit_forward = ini_get_float(ini, L"cockpit_forward", g_cfg.cockpit_forward);
    g_cfg.cockpit_up = ini_get_float(ini, L"cockpit_up", g_cfg.cockpit_up);
    g_cfg.show_pod_first_person =
        ini_get_bool(ini, L"show_pod_first_person", g_cfg.show_pod_first_person);
    g_cfg.hide_own_pod = ini_get_bool(ini, L"hide_own_pod", g_cfg.hide_own_pod);
    g_cfg.hide_hud = ini_get_bool(ini, L"hide_hud", g_cfg.hide_hud);
    g_cfg.hide_guide_arrow = ini_get_bool(ini, L"hide_guide_arrow", g_cfg.hide_guide_arrow);
    g_cfg.hide_suns = ini_get_bool(ini, L"hide_suns", g_cfg.hide_suns);
    g_cfg.hide_light_streaks = ini_get_bool(ini, L"hide_light_streaks", g_cfg.hide_light_streaks);
    g_cfg.shake_intensity = ini_get_float(ini, L"shake_intensity", g_cfg.shake_intensity);
    g_cfg.shake_speed = ini_get_float(ini, L"shake_speed", g_cfg.shake_speed);
    g_cfg.shake_boost = ini_get_float(ini, L"shake_boost", g_cfg.shake_boost);
    g_cfg.shake_boost_burst = ini_get_float(ini, L"shake_boost_burst", g_cfg.shake_boost_burst);
    g_cfg.shake_vibration = ini_get_float(ini, L"shake_vibration", g_cfg.shake_vibration);
}
void save_config() {
    const wchar_t *ini = settings_ini_path();
    wchar_t buf[16];
    swprintf(buf, 16, L"%d", g_cfg.view);
    WritePrivateProfileStringW(INI_SECTION, L"view", buf, ini);
    ini_set_float(ini, L"trail_scale", g_cfg.trail_scale);
    ini_set_float(ini, L"height_scale", g_cfg.height_scale);
    ini_set_float(ini, L"fov_offset", g_cfg.fov_offset);
    ini_set_float(ini, L"dynamic_fov", g_cfg.dynamic_fov);
    ini_set_bool(ini, L"disable_roll", g_cfg.disable_roll);
    ini_set_float(ini, L"cockpit_right", g_cfg.cockpit_right);
    ini_set_float(ini, L"cockpit_forward", g_cfg.cockpit_forward);
    ini_set_float(ini, L"cockpit_up", g_cfg.cockpit_up);
    ini_set_bool(ini, L"show_pod_first_person", g_cfg.show_pod_first_person);
    ini_set_bool(ini, L"hide_own_pod", g_cfg.hide_own_pod);
    ini_set_bool(ini, L"hide_hud", g_cfg.hide_hud);
    ini_set_bool(ini, L"hide_guide_arrow", g_cfg.hide_guide_arrow);
    ini_set_bool(ini, L"hide_suns", g_cfg.hide_suns);
    ini_set_bool(ini, L"hide_light_streaks", g_cfg.hide_light_streaks);
    ini_set_float(ini, L"shake_intensity", g_cfg.shake_intensity);
    ini_set_float(ini, L"shake_speed", g_cfg.shake_speed);
    ini_set_float(ini, L"shake_boost", g_cfg.shake_boost);
    ini_set_float(ini, L"shake_boost_burst", g_cfg.shake_boost_burst);
    ini_set_float(ini, L"shake_vibration", g_cfg.shake_vibration);
}

void panel_player_camera() {
    bool dirty = false;

    ImGui::SeparatorText("View");
    dirty |= ImGui::Combo("Camera view", &g_cfg.view, VIEW_NAMES, IM_ARRAYSIZE(VIEW_NAMES));
    dirty |= ImGui::SliderFloat("Chase distance", &g_cfg.trail_scale, 0.25f, 4.0f, "x%.2f");
    dirty |= ImGui::SliderFloat("Chase height", &g_cfg.height_scale, 0.25f, 4.0f, "x%.2f");
    dirty |= ImGui::Checkbox("Level horizon (no camera roll)", &g_cfg.disable_roll);

    ImGui::SeparatorText("Field of view");
    dirty |= ImGui::SliderFloat("FOV offset", &g_cfg.fov_offset, -40.0f, 40.0f, "%+.0f deg");
    dirty |= ImGui::SliderFloat("Dynamic FOV (at top speed)", &g_cfg.dynamic_fov, 0.0f, 40.0f,
                                "+%.0f deg");

    ImGui::SeparatorText("First person / cockpit");
    dirty |= ImGui::SliderFloat("Offset right", &g_cfg.cockpit_right, -3.0f, 3.0f, "%.2f");
    dirty |= ImGui::SliderFloat("Offset forward", &g_cfg.cockpit_forward, -6.0f, 6.0f, "%.2f");
    dirty |= ImGui::SliderFloat("Offset up", &g_cfg.cockpit_up, -3.0f, 3.0f, "%.2f");
    dirty |= ImGui::Checkbox("Show my pod in first person", &g_cfg.show_pod_first_person);

    ImGui::SeparatorText("Camera shake");
    dirty |= ImGui::SliderFloat("Shake intensity", &g_cfg.shake_intensity, 0.0f, 3.0f, "x%.2f");
    dirty |= ImGui::SliderFloat("Shake speed", &g_cfg.shake_speed, 1.0f, 20.0f, "%.1f");
    dirty |= ImGui::SliderFloat("Boost rumble", &g_cfg.shake_boost, 0.0f, 3.0f, "x%.2f");
    dirty |= ImGui::SliderFloat("Boost onset burst", &g_cfg.shake_boost_burst, 0.0f, 3.0f, "x%.2f");
    dirty |= ImGui::SliderFloat("Collision / terrain vibration", &g_cfg.shake_vibration, 0.0f, 3.0f,
                                "x%.2f");

    ImGui::SeparatorText("Visibility");
    dirty |= ImGui::Checkbox("Hide HUD", &g_cfg.hide_hud);
    dirty |= ImGui::Checkbox("Hide guide arrow", &g_cfg.hide_guide_arrow);
    dirty |= ImGui::Checkbox("Hide suns / lens flares", &g_cfg.hide_suns);
    dirty |= ImGui::Checkbox("Hide light streaks", &g_cfg.hide_light_streaks);
    dirty |= ImGui::Checkbox("Hide my pod", &g_cfg.hide_own_pod);
    // shared with the general settings
    bool labels = imgui_state.show_pod_names;
    if (ImGui::Checkbox("Hide racer labels / position numbers", &labels)) {
        imgui_state.show_pod_names = !labels;
        save_settings_ini();
    }
    bool weather = imgui_state.enable_weather;
    if (ImGui::Checkbox("Hide weather", &weather)) {
        imgui_state.enable_weather = !weather;
        save_settings_ini();
    }

    ImGui::Separator();
    ImGui::TextDisabled("Applies to your own pod camera in a race. The free camera overrides it.");

    static bool pending = false;
    if (dirty)
        pending = true;
    if (pending && !ImGui::IsAnyItemActive()) {
        save_config();
        pending = false;
    }
}

DebugPanel g_panel = {
    .category = "Camera", .name = "Player Camera", .draw = panel_player_camera, .dev_only = false};

}// namespace

bool playercam_HudHidden() {
    return g_cfg.hide_hud && in_race();
}

bool playercam_SuppressSpriteVisibility() {
    return (g_sprite_group == GROUP_SUN && g_cfg.hide_suns) ||
           (g_sprite_group == GROUP_LIGHT_STREAKS && g_cfg.hide_light_streaks);
}

bool playercam_HideOwnPod() {
    return g_cfg.hide_own_pod;
}

bool playercam_ShowPodInFirstPerson() {
    return g_cfg.show_pod_first_person;
}

void playercam_RegisterPanel() {
    load_config();
    debug_ui_register(&g_panel);
}

// After the original, unk20_mat / focusTransform_mat hold this frame's camera and aim point; the
// quake pass in swrObjcMan_F3 and the viewport camera-state update consume them afterwards.
typedef void(__cdecl *swrObjcMan_UpdateCameraFn)(swrObjcMan *);
extern "C" void __cdecl swrObjcMan_UpdateCamera_delta(swrObjcMan *cman) {
    apply_view_override(cman);
    hook_call_original((swrObjcMan_UpdateCameraFn) swrObjcMan_UpdateCamera_ADDR, cman);

    swrRace *racer = local_followed_racer(cman);
    if (racer == nullptr || !is_player_view(cman->mode_type))
        return;
    if (is_first_person(cman->mode_type))
        apply_cockpit_offset(cman, racer);
    if (g_cfg.disable_roll)
        remove_roll(cman);
    apply_shake(cman, racer);
}

// Scale the followed pilot's swrRacer_PodVisualData trail / height around the call only.
typedef void(__cdecl *swrObjcMan_UpdateChaseCameraFn)(swrObjcMan *);
extern "C" void __cdecl swrObjcMan_UpdateChaseCamera_delta(swrObjcMan *cman) {
    swrRace *racer = local_followed_racer(cman);
    int pilot = -1;
    if (racer != nullptr && racer->score_ptr != nullptr && racer->score_ptr->pilotId != nullptr)
        pilot = *racer->score_ptr->pilotId;
    const bool scale = pilot >= 0 && pilot < NUM_PILOTS &&
                       (g_cfg.trail_scale != 1.0f || g_cfg.height_scale != 1.0f);
    if (!scale) {
        hook_call_original((swrObjcMan_UpdateChaseCameraFn) swrObjcMan_UpdateChaseCamera_ADDR, cman);
        return;
    }
    swrRacerVisualData &row = swrRacer_PodVisualData[pilot];
    const float trail = row.chaseCamTrail;
    const float height = row.chaseCamHeight;
    row.chaseCamTrail = trail * g_cfg.trail_scale;
    row.chaseCamHeight = height * g_cfg.height_scale;
    hook_call_original((swrObjcMan_UpdateChaseCameraFn) swrObjcMan_UpdateChaseCamera_ADDR, cman);
    row.chaseCamTrail = trail;
    row.chaseCamHeight = height;
}

// stagingTransformFocus.vD.w is the camera FOV, re-staged every frame by the mode function (100, or
// 120 for the wide first person) and pushed to the viewport here.
typedef void(__cdecl *swrObjcMan_UpdateFogAndViewportFn)(swrObjcMan *);
extern "C" void __cdecl swrObjcMan_UpdateFogAndViewport_delta(swrObjcMan *cman) {
    swrRace *racer = local_followed_racer(cman);
    if (racer != nullptr && is_player_view(cman->mode_type)) {
        float fov = cman->stagingTransformFocus.vD.w + g_cfg.fov_offset;
        if (g_cfg.dynamic_fov > 0.0f && racer->podStats.maxSpeed > 0.0f) {
            const float t = std::clamp(racer->speedValue / racer->podStats.maxSpeed, 0.0f, 1.0f);
            fov += g_cfg.dynamic_fov * t * t;
        }
        cman->stagingTransformFocus.vD.w = std::clamp(fov, 20.0f, 170.0f);
    }
    hook_call_original((swrObjcMan_UpdateFogAndViewportFn) swrObjcMan_UpdateFogAndViewport_ADDR,
                       cman);
}

// The original re-shows the arrow node every frame; hide it again with swrObjJdge_UpdatePlayerHUD's
// flag write. guideArrowNode is stale memory until FLAG0_GUIDE_ARROW is set.
typedef void(__cdecl *swrObjcMan_UpdateSplineGuideMarkerFn)(swrObjcMan *);
extern "C" void __cdecl swrObjcMan_UpdateSplineGuideMarker_delta(swrObjcMan *cman) {
    hook_call_original((swrObjcMan_UpdateSplineGuideMarkerFn) swrObjcMan_UpdateSplineGuideMarker_ADDR,
                       cman);
    if (!g_cfg.hide_guide_arrow)
        return;
    swrRace *racer = cman->unkf4_objTest;
    if (racer != nullptr && (racer->flags0 & swrObjTest_FLAG0_GUIDE_ARROW) != 0 &&
        racer->guideArrowNode != nullptr)
        swrModel_NodeModifyFlags(racer->guideArrowNode, 2, -4, 0x10, 3);
}

// Both updaters re-assert visibility every frame; the SetVisible detour (camera.cpp) forces a hidden
// group off.
typedef void(__cdecl *WorldSpriteUpdaterFn)(swrViewport *);
extern "C" void __cdecl UpdateSunAndLensFlareSprites_delta(swrViewport *vp) {
    g_sprite_group = GROUP_SUN;
    hook_call_original((WorldSpriteUpdaterFn) UpdateSunAndLensFlareSprites_ADDR, vp);
    g_sprite_group = GROUP_NONE;
}
extern "C" void __cdecl UpdateLightStreakSprites_delta(swrViewport *vp) {
    g_sprite_group = GROUP_LIGHT_STREAKS;
    hook_call_original((WorldSpriteUpdaterFn) UpdateLightStreakSprites_ADDR, vp);
    g_sprite_group = GROUP_NONE;
}
extern "C" void __cdecl swrPlayerHUD_RenderWorldSprites_delta(swrViewport *vp) {
    g_sprite_group = GROUP_LIGHT_STREAKS;
    hook_call_original((WorldSpriteUpdaterFn) swrPlayerHUD_RenderWorldSprites_ADDR, vp);
    g_sprite_group = GROUP_NONE;
}

void playercam_RegisterHooks() {
    // Raw-address detours, not hook_replace: keeps the reverse hooks intact (see camera.cpp).
    hook_function("swrObjcMan_UpdateCamera", (uint32_t) swrObjcMan_UpdateCamera_ADDR,
                  (uint8_t *) swrObjcMan_UpdateCamera_delta);
    hook_function("swrObjcMan_UpdateChaseCamera", (uint32_t) swrObjcMan_UpdateChaseCamera_ADDR,
                  (uint8_t *) swrObjcMan_UpdateChaseCamera_delta);
    hook_function("swrObjcMan_UpdateFogAndViewport", (uint32_t) swrObjcMan_UpdateFogAndViewport_ADDR,
                  (uint8_t *) swrObjcMan_UpdateFogAndViewport_delta);
    hook_function("swrObjcMan_UpdateSplineGuideMarker",
                  (uint32_t) swrObjcMan_UpdateSplineGuideMarker_ADDR,
                  (uint8_t *) swrObjcMan_UpdateSplineGuideMarker_delta);
    hook_function("UpdateSunAndLensFlareSprites", (uint32_t) UpdateSunAndLensFlareSprites_ADDR,
                  (uint8_t *) UpdateSunAndLensFlareSprites_delta);
    hook_function("UpdateLightStreakSprites", (uint32_t) UpdateLightStreakSprites_ADDR,
                  (uint8_t *) UpdateLightStreakSprites_delta);
    hook_function("swrPlayerHUD_RenderWorldSprites", (uint32_t) swrPlayerHUD_RenderWorldSprites_ADDR,
                  (uint8_t *) swrPlayerHUD_RenderWorldSprites_delta);
}
