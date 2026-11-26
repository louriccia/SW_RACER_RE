#ifndef REFINED_TEXTURE_CACHE_H
#define REFINED_TEXTURE_CACHE_H

// The texture cache: one slot per texture of the open texture block, holding the loader's
// {material, palette} pair once the texture has been converted. Retail sizes it for its own
// block (1700 entries); here it follows the block's texture count, so custom tracks can append
// textures.

#include <stdint.h>
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

// Sizes the cache for `count` textures and returns it. clear_all drops every cached entry (the
// texture block changed); otherwise entries are kept and new ones start empty.
void** refined_texture_cache_resize(uint32_t count, int clear_all);

// swrModel_LoadModelTexture over the cache. 0x00447490
void refined_swrModel_LoadModelTexture(TEXID texture_index, swrMaterial** material_ptr, uint8_t** palette_data_ptr);

// swrAssetBuffer_InvalidateTexturesBelow over the cache: forgets entries allocated above `ptr`
// (the asset arena was rewound past them). 0x004475d0
void refined_swrAssetBuffer_InvalidateTexturesBelow(char* ptr);

#ifdef __cplusplus
}
#endif

#endif
