#include "profiling.h"

// Single definition (one global guard) so the GPU context is created exactly once across all TUs.
// Defining this in a header would give each TU its own static flag and create the context more
// than once.
void ensureTracyGpuContext() {
#ifdef TRACY_ENABLE
    static bool inited = false;
    if (!inited) {
        TracyGpuContext;
        inited = true;
    }
#endif
}
