#include "swrSpline_delta.h"

#include <macros.h>

extern "C" {
#include <Swr/swrSpline.h>
}

#include "../hook_helper.h"
#include "../custom_tracks.h"

extern FILE *hook_log;

// No stock spline and no custom pack has more than 180 control points, so a count past this is a
// garbage read, not a real spline.
#define SPLINE_PLAUSIBLE_MAX_CONTROL_POINTS 4096

// A cursor holds a raw swrSpline* into the asset buffer, which the next track load rewinds and
// refills, so a cursor that outlives its track reads non-NULL but garbage (seen:
// num_control_points 0xffcea415). Interpolating through it faults in swrSpline_Interpolate
// (0x0044e774) and so does re-seeding it, since swrSpline_CursorSeek walks control_points too
// (0x0044ef14) -- it cannot be repaired from here, only refused.
static bool cursor_spline_is_usable(const swrSplineCursor *c) {
    const swrSpline *s = c->spline;
    return s != NULL && s->control_points != NULL && s->num_control_points > 0 &&
        s->num_control_points <= SPLINE_PLAUSIBLE_MAX_CONTROL_POINTS;
}

// Callers that gate cinematics on a cursor need the same test: a NULL check alone passes a cursor
// left pointing into a reloaded asset buffer, and the evaluation above then (correctly) refuses to
// run it, which stalls whatever was waiting for the path to end.
bool spline_cursor_has_usable_spline(const void *cursor) {
    const swrSplineCursor *c = (const swrSplineCursor *) cursor;
    return c != NULL && cursor_spline_is_usable(c);
}

// Report once per distinct caller -- these run per cursor per frame, so an unfiltered log would
// bury everything else in hook.log.
static bool report_once_per_caller(const void *caller) {
    static const void *seen[8] = {};
    static int seen_count = 0;
    for (int i = 0; i < seen_count; i++) {
        if (seen[i] == caller)
            return false;
    }
    if (seen_count < (int) (sizeof(seen) / sizeof(seen[0])))
        seen[seen_count++] = caller;
    return true;
}

// nodeLookahead indexes control_points and swrSpline_Interpolate does not range-check it, while
// swrSpline_CursorSeekToProgress leaves the window untouched when its scan matches no node -- so a
// cursor reused across tracks can hold indices from a longer spline.
static bool cursor_window_out_of_range(const swrSplineCursor *c) {
    const swrSpline *s = c->spline;
    for (int i = 0; i < 4; i++) {
        if (c->nodeLookahead[i] < 0 || c->nodeLookahead[i] >= (int) s->num_control_points)
            return true;
    }
    return false;
}

#ifdef SWR_REFINED
// swe1r: the track lifecycle (refined/track_lifecycle) clears the fly-by cursor whenever it is not
// seeded and zeroes registered cursors at every asset-buffer rewind, so no cursor outlives its
// spline. These hooks only check that invariant and log its first violation per caller. A cursor
// with no usable spline is still not evaluated (a stale transform beats a fault); an out-of-range
// node window is no longer repaired.
static void report_invariant(const char *what, const swrSplineCursor *c, const void *caller) {
    if (!report_once_per_caller(caller))
        return;
    fprintf(hook_log, "[track_lifecycle] invariant broken: %s (cursor %p, spline %p, called from %p)\n", what,
            (const void *) c, c != NULL ? (const void *) c->spline : NULL, caller);
    fflush(hook_log);
}

// 0x0047e8b0
void swrSpline_CursorSeekToProgress_delta(void *cursor, int progress) {
    hook_call_original(swrSpline_CursorSeekToProgress, cursor, progress);

    swrSplineCursor *c = (swrSplineCursor *) cursor;
    if (c != NULL && cursor_spline_is_usable(c) && cursor_window_out_of_range(c))
        report_invariant("seek left a node window outside the spline", c, __builtin_return_address(0));
}

// 0x0044ed80
void swrSpline_EvaluateToMatrix_delta(void *cursor, void *out) {
    swrSplineCursor *c = (swrSplineCursor *) cursor;
    const void *caller = __builtin_return_address(0);
    if (c == NULL || !cursor_spline_is_usable(c)) {
        report_invariant("evaluating a cursor with no usable spline", c, caller);
        return;
    }
    if (cursor_window_out_of_range(c))
        report_invariant("evaluating a node window outside the spline", c, caller);

    hook_call_original(swrSpline_EvaluateToMatrix, cursor, (rdMatrix44 *) out);
}
#else
// The spline is known good here, so swrSpline_CursorSeek can safely rebuild the whole chain from
// node 0.
static void clamp_cursor_indices(swrSplineCursor *c, const void *caller) {
    const swrSpline *s = c->spline;
    if (!cursor_window_out_of_range(c))
        return;

    if (report_once_per_caller(caller)) {
        fprintf(hook_log,
                "[spline] cursor %p nodes %d/%d/%d/%d are outside its %u control points, called "
                "from %p; re-seeding to node 0\n",
                (void *) c, c->nodeLookahead[0], c->nodeLookahead[1], c->nodeLookahead[2],
                c->nodeLookahead[3], s->num_control_points, caller);
        fflush(hook_log);
    }

    swrSpline_CursorSeek(c, 0);
    c->segmentT = 0.0f;
}

// 0x0047e8b0
void swrSpline_CursorSeekToProgress_delta(void *cursor, int progress) {
    hook_call_original(swrSpline_CursorSeekToProgress, cursor, progress);

    swrSplineCursor *c = (swrSplineCursor *) cursor;
    if (c != NULL && cursor_spline_is_usable(c))
        clamp_cursor_indices(c, __builtin_return_address(0));
}

// 0x0044ed80
void swrSpline_EvaluateToMatrix_delta(void *cursor, void *out) {
    swrSplineCursor *c = (swrSplineCursor *) cursor;
    const void *caller = __builtin_return_address(0);
    if (c == NULL || !cursor_spline_is_usable(c)) {
        if (report_once_per_caller(caller)) {
            fprintf(hook_log,
                    "[spline] refusing to evaluate a cursor with no usable spline: cursor=%p, "
                    "spline=%p, called from %p\n",
                    cursor, (void *) (c != NULL ? c->spline : NULL), caller);
            fflush(hook_log);
        }
        return;// leave `out` as it was; a stale transform beats a fault
    }

    clamp_cursor_indices(c, caller);

    hook_call_original(swrSpline_EvaluateToMatrix, cursor, (rdMatrix44 *) out);
}
#endif

// 0x004472e0
char *swrSpline_LoadSplineById_delta(char *splineBuffer) {
    const bool is_custom_track = prepare_loading_custom_track_spline((SPLINEID *) &splineBuffer);

    char *res = hook_call_original(swrSpline_LoadSplineById, splineBuffer);

    if (is_custom_track)
        finalize_loading_custom_track_spline(*(swrSpline* SWR_P32*) res);
    return res;
}
