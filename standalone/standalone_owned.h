#ifndef STANDALONE_OWNED_H
#define STANDALONE_OWNED_H

#include <stdint.h>
#include <stdio.h>

// One storage block for retail globals (see tools/gen_owned_globals.py): retail address, our
// storage, its footprint, the first name in it, and the canary bytes right after it.
typedef struct StandaloneOwnedGlobal {
    uint32_t retail;
    void* ours;
    uint32_t size;
    const char* name;
    unsigned char* canary;
} StandaloneOwnedGlobal;

extern StandaloneOwnedGlobal standalone_owned_globals[];
extern unsigned char* swr_owned_ptr[];
extern const int standalone_owned_guarded; // diagnostic build: each block ends at an inaccessible page
extern const int standalone_owned_global_count;
extern const int standalone_owned_canary_size;

// Points an indirect global (standalone/owned_indirect.txt) at other storage, e.g. a larger table;
// returns the previous storage, or NULL if `name` is not indirect.
void* standalone_global_rebind(const char* name, void* storage);

// Reports blocks whose canary changed (code ran past the block) and blocks whose old image copy was
// written (code reached them through a retail address).
void standalone_owned_report_stale(FILE* log);

#endif
