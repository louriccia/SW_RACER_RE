#ifndef REFINED_ASSET_BUFFER_H
#define REFINED_ASSET_BUFFER_H

// The level asset arena: one block every model, texture and spline of a race is bump-allocated
// from. Retail fixes it at 8 MiB; custom tracks and HD assets need more.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Arena size used by the next world initialization. Default: retail's SWR_ASSET_BUFFER_SIZE_RETAIL.
void refined_asset_buffer_set_size(size_t bytes);
size_t refined_asset_buffer_size(void);

// swrScene_InitWorld with the arena size above. 0x00449040
void refined_swrScene_InitWorld(void);

#ifdef __cplusplus
}
#endif

#endif
