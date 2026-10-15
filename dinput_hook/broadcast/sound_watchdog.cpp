#include "sound_watchdog.h"
#include "../debug_ui.h"
#include "../config.h"

#include <imgui.h>

#include <windows.h>
#include <cstdio>

extern "C" {
#include <Swr/swrSound.h>
#include <globals.h>
#include <types.h>
}

extern "C" FILE *hook_log;

// ---------------------------------------------------------------------------------------------
// Why this exists
//
// swrSound_Update frees a live voice slot on exactly one condition:
//
//     if (swrSound_GetWavePosition(live[i].source, NULL) == 0) { ... ResetChannel(live[i]); }
//
// GetWavePosition returns -1 when the source is NULL or IA3dSource::GetStatus fails, so a voice
// whose source has gone bad is never released: the slot stays occupied for the rest of the run and
// its dedicated (duplicated) source is never released either.
//
// A source goes bad when swrSound_LoadSound runs the bank over its budget and swrSound_EvictSounds
// unloads an entry. Eviction only checks the entry's own shared source (descriptor +0x48); the
// duplicates handed out by swrSound_AcquireSource for concurrent playback of the same sound are
// invisible to it, and swrSound_UnloadSound releases the original out from under them.
// swrSound_Update does sweep stale voices, but only for the two frames after an evict and only for
// slots with dedicatedSource == 0 - precisely the set that cannot be the victim here.
//
// A single race rarely evicts. Twenty AI pods on a rotating track for ten races sit pinned at the
// 4 MiB budget and evict constantly, so the slots die off one by one until the stream is silent.
// swrSound_ResetChannels does not recover it: it clears the slots without releasing the sources,
// so the leak survives the reset and the A3D source pool stays exhausted.
//
// Two fixes here: reclaim the stuck slots (and their sources) every frame, and raise the bank
// budget so the eviction thrash that triggers it stops happening in the first place.

// swrSound_LoadSound compares swrSound_loadedBytes + entry->dataSize against this before calling
// swrSound_EvictSounds. Stock value 0x400000 (4 MiB); the whole 11K wav set is ~54 MiB.
#define swrSound_cacheBudgetBytes (*(unsigned int *) 0x004b6d10)
static const unsigned int STOCK_CACHE_BUDGET = 0x400000;

static const char *INI_SECTION = "sound_watchdog";

static bool g_enabled = true;
static bool g_release_sources = true;// release the stuck voice's source, not just the slot
static float g_stall_s = 6.0f;       // a playing voice whose wave position stops advancing
static int g_budget_mib = 32;

static int g_recovered_stale = 0;// bank entry unloaded under the voice
static int g_recovered_dead = 0; // source stopped answering
static int g_recovered_stall = 0;// wave position frozen
static int g_released = 0;

struct SlotState {
    int sound_id;
    DWORD wave_pos;
    DWORD unchanged_since_ms;
};

static SlotState g_slots[8];

static void apply_budget() {
    const unsigned int want = (unsigned int) g_budget_mib * 1024u * 1024u;
    if (swrSound_cacheBudgetBytes == want)
        return;
    swrSound_cacheBudgetBytes = want;
    fprintf(hook_log, "[sound] bank budget %u B (stock %u)\n", want, STOCK_CACHE_BUDGET);
    fflush(hook_log);
}

static void forget(int i) {
    g_slots[i].sound_id = -1;
    g_slots[i].wave_pos = 0;
    g_slots[i].unchanged_since_ms = 0;
}

// Hand a stuck slot back to the mixer. The source is only released when this voice owns it: a
// dedicated source that is neither the bank entry's shared one nor the reverb backup's copy,
// matching what swrSound_Update itself releases on the path that works.
static void recover(int i, const char *why) {
    swrSound *live = &swrSound_voicesLive[i];
    IA3dSource *source = live->source;
    const swrSoundDescriptor *entry = (const swrSoundDescriptor *) swrSound_GetEntry(live->id);
    const bool owned = g_release_sources && source != NULL && live->dedicatedSource != 0 &&
                       (entry == NULL || entry->source != source) &&
                       swrSound_voicesBackup[i].source != source;

    fprintf(hook_log, "[sound] voice %d stuck (%s): id %d, src %p, dedicated %d -> reset%s\n", i,
            why, live->id, (void *) source, live->dedicatedSource, owned ? " + release" : "");
    fflush(hook_log);

    if (swrSound_voicesRequested[i].activeSoundId == live->activeSoundId)
        swrSound_ResetChannel(&swrSound_voicesRequested[i]);
    swrSound_ResetChannel(live);
    if (owned) {
        swrSound_ReleaseSource(source);
        g_released++;
    }
    forget(i);
}

void sound_watchdog_Service() {
    if (!g_enabled || swrSound_Initted == 0 || swrSound_unk_init == 0)
        return;
    apply_budget();

    const DWORD now = GetTickCount();
    for (int i = 0; i < 8; i++) {
        swrSound *live = &swrSound_voicesLive[i];
        if (live->activeSoundId < 0) {
            forget(i);
            continue;
        }

        // A streamed entry (music) is refilled by the streaming thread; its wave position is not
        // ours to judge.
        const swrSoundDescriptor *entry = (const swrSoundDescriptor *) swrSound_GetEntry(live->id);
        if (entry != NULL && (entry->flags & swrSoundDescriptor_STREAMED) != 0) {
            forget(i);
            continue;
        }

        // The eviction race, caught without waiting on a timer: the voice is playing a bank entry
        // that has since been unloaded.
        if (entry != NULL && (entry->flags & swrSoundDescriptor_LOADED) == 0) {
            g_recovered_stale++;
            recover(i, "bank entry unloaded");
            continue;
        }

        DWORD pos = 0;
        const int status = swrSound_GetWavePosition(live->source, &pos);
        if (status < 0) {
            g_recovered_dead++;
            recover(i, "source not answering");
            continue;
        }
        if (status == 0)
            continue;// finished; swrSound_Update frees it this frame

        if (live->activeSoundId != g_slots[i].sound_id || pos != g_slots[i].wave_pos) {
            g_slots[i].sound_id = live->activeSoundId;
            g_slots[i].wave_pos = pos;
            g_slots[i].unchanged_since_ms = now;
            continue;
        }
        if (g_stall_s > 0.0f && g_slots[i].unchanged_since_ms != 0 &&
            now - g_slots[i].unchanged_since_ms >= (DWORD) (g_stall_s * 1000.0f)) {
            g_recovered_stall++;
            recover(i, "wave position frozen");
        }
    }
}

void sound_watchdog_ForceReset() {
    for (int i = 0; i < 8; i++) {
        if (swrSound_voicesLive[i].activeSoundId >= 0)
            recover(i, "manual reset");
        forget(i);
    }
    swrSound_ResetRequestedVoices();
    swrSound_ResetChannels();
}

// ---------------------------------------------------------------------------------------------
// Config + panel

static void load_config() {
    g_enabled = config::get_int(INI_SECTION, "enabled", g_enabled) != 0;
    g_release_sources = config::get_int(INI_SECTION, "release_sources", g_release_sources) != 0;
    g_stall_s = config::get_float(INI_SECTION, "stall_s", g_stall_s);
    g_budget_mib = config::get_int(INI_SECTION, "bank_budget_mib", g_budget_mib);
}

static void save_config() {
    config::set_bool(INI_SECTION, "enabled", g_enabled);
    config::set_bool(INI_SECTION, "release_sources", g_release_sources);
    config::set_float(INI_SECTION, "stall_s", g_stall_s);
    config::set_int(INI_SECTION, "bank_budget_mib", g_budget_mib);
    config::save();
}

static void panel_sound_watchdog() {
    bool changed = false;
    changed |= ImGui::Checkbox("Reclaim stuck voice channels", &g_enabled);
    changed |= ImGui::Checkbox("Release the stuck voice's source", &g_release_sources);
    changed |= ImGui::SliderFloat("Frozen wave position counts as stuck after (s)", &g_stall_s,
                                  0.0f, 30.0f, "%.0f");
    changed |= ImGui::SliderInt("Sound bank budget (MiB)", &g_budget_mib, 4, 96);
    ImGui::TextDisabled("Stock is 4 MiB; the 11K wav set is ~54 MiB. Evicting under a live voice");
    ImGui::TextDisabled("is what kills the mixer, so a budget the run never reaches prevents it.");
    ImGui::Separator();
    ImGui::Text("Loaded %.1f / %.0f MiB", swrSound_loadedBytes / 1048576.0f,
                swrSound_cacheBudgetBytes / 1048576.0f);
    ImGui::Text("Recovered: %d stale, %d dead, %d stalled | %d sources released", g_recovered_stale,
                g_recovered_dead, g_recovered_stall, g_released);
    for (int i = 0; i < 8; i++)
        ImGui::Text("voice %d: id %d, dedicated %d", i, swrSound_voicesLive[i].id,
                    swrSound_voicesLive[i].dedicatedSource);
    if (ImGui::Button("Reset sound channels"))
        sound_watchdog_ForceReset();
    if (changed)
        save_config();
}

static DebugPanel g_panel = {.category = "Race",
                             .name = "Sound watchdog",
                             .draw = panel_sound_watchdog,
                             .dev_only = false};

void sound_watchdog_RegisterPanel() {
    load_config();
    for (int i = 0; i < 8; i++)
        forget(i);
    debug_ui_register(&g_panel);
}
