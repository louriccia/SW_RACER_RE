#ifndef REFINED_H
#define REFINED_H

// The refinement layer's entry points: each refined function and the faithful function (src/) it
// stands in for. The host (standalone feature build) routes every call of the faithful function
// to the refined one; src/ itself is never edited.

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RefinedSlot {
    const char* name;
    void* faithful;
    void* refined;
} RefinedSlot;

extern const RefinedSlot refined_slots[];
extern const int refined_slot_count;

#ifdef __cplusplus
}
#endif

#endif
