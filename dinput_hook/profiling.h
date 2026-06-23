#pragma once
//
// Central include for Tracy frame-profiler instrumentation.
//
// All Tracy macros (ZoneScoped*, FrameMark, TracyGpu*) expand to no-ops unless the build was
// configured with -DENABLE_TRACY=ON (which defines TRACY_ENABLE), so the instrumentation can stay
// in the renderer source unconditionally at zero cost in normal builds. TracyOpenGL.hpp calls the
// GL timer-query entrypoints directly and does not pull in GL headers itself, so glad must be
// included first.
//
#include <glad/glad.h>
#include <tracy/Tracy.hpp>
#include <tracy/TracyOpenGL.hpp>

// Create the Tracy GPU (OpenGL timer-query) context exactly once, on first call. TracyGpuZone and
// TracyGpuCollect dereference the context pointer with no null check, and the game presents 2D /
// loading frames before the first 3D viewport renders, so the context must be created before ANY
// GPU zone or collect regardless of which hook runs first. Call this at the top of every function
// that uses a GPU zone/collect; it is a cheap no-op after the first call (and entirely when Tracy
// is disabled). Must be called with the GL context current (all render/present hooks qualify).
void ensureTracyGpuContext();
