#pragma once

char *swrSpline_LoadSplineById_delta(char *splineBuffer);

// Choke point for every spline evaluation. swrSpline_Interpolate dereferences
// cursor->spline (offset 0) unconditionally, so a cursor whose spline went away faults at
// address 0. Skip the evaluation and name the caller instead of taking the process down.
void swrSpline_EvaluateToMatrix_delta(void *cursor, void *out);
