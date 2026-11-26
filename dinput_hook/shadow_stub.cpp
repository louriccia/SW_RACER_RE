// Shadow mode runs retail machine code next to our functions through 32-bit register thunks; a
// 64-bit build has no retail code to compare against, so it reports nothing installed.
#include "shadow.h"

#include <cstring>

extern "C" void shadow_init(void) {}
extern "C" void shadow_resolve(void) {}
extern "C" void shadow_frame_tick(void) {}
extern "C" void shadow_race_loaded(void) {}
extern "C" void shadow_report(void) {}

void shadow_get_status(ShadowStatus *st) {
    std::memset(st, 0, sizeof(*st));
}

int shadow_function_count(void) {
    return 0;
}

bool shadow_get_function(int, ShadowFunctionStats *) {
    return false;
}

int shadow_recent_mismatches(const ShadowMismatch **out) {
    *out = nullptr;
    return 0;
}

void shadow_set_enabled(bool) {}
void shadow_set_anytime(bool) {}
void shadow_set_budget_ms(double) {}
void shadow_reset_stats(void) {}
