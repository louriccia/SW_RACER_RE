#include "swrSpline_delta.h"

#include <macros.h>

extern "C" {
#include <Swr/swrSpline.h>
}

#include "../hook_helper.h"
#include "../custom_tracks.h"

extern FILE *hook_log;

// 0x0044ed80
void swrSpline_EvaluateToMatrix_delta(void *cursor, void *out) {
    swrSplineCursor *c = (swrSplineCursor *) cursor;
    if (c == NULL || c->spline == NULL) {
        // Report once per distinct caller -- this runs per pod per frame, so an unfiltered log
        // would bury everything else in hook.log.
        static const void *seen[8] = {};
        static int seen_count = 0;
        const void *caller = __builtin_return_address(0);
        bool known = false;
        for (int i = 0; i < seen_count; i++)
            known = known || seen[i] == caller;
        if (!known) {
            if (seen_count < (int) (sizeof(seen) / sizeof(seen[0])))
                seen[seen_count++] = caller;
            fprintf(hook_log,
                    "[spline] refusing to evaluate a cursor with no spline: cursor=%p, called from "
                    "%p\n",
                    cursor, caller);
            fflush(hook_log);
        }
        return;// leave `out` as it was; a stale transform beats a fault
    }

    hook_call_original(swrSpline_EvaluateToMatrix, cursor, (rdMatrix44 *) out);
}

// 0x004472e0
char *swrSpline_LoadSplineById_delta(char *splineBuffer) {
    const bool is_custom_track = prepare_loading_custom_track_spline((SPLINEID *) &splineBuffer);

    char *res = hook_call_original(swrSpline_LoadSplineById, splineBuffer);

    if (is_custom_track)
        finalize_loading_custom_track_spline(*(swrSpline **) res);
    return res;
}
