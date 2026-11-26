#include "texture_cache.h"

#include <Engine/rdMaterial.h>
#include <Swr/swrLoader.h>
#include <Swr/swrModel.h>
#include <Swr/swrSprite.h>
#include <engine_config.h>
#include <globals.h>
#include <macros.h>

#include <stdlib.h>
#include <string.h>

// Until a block sizes the cache, it is retail's fixed array.
static void** cache = NULL;
static uint32_t cache_count = 0;

static void** slots(uint32_t* count)
{
    if (cache != NULL) {
        *count = cache_count;
        return cache;
    }
    *count = SWR_TEXTURE_BUFFER_SIZE;
    return (void**)texture_buffer;
}

void** refined_texture_cache_resize(uint32_t count, int clear_all)
{
    void** grown = (void**)realloc(cache, (count ? count : 1) * sizeof(void*));
    if (grown == NULL)
        return cache;
    if (clear_all)
        memset(grown, 0, count * sizeof(void*));
    else if (count > cache_count)
        memset(grown + cache_count, 0, (count - cache_count) * sizeof(void*));
    cache = grown;
    cache_count = count;
    return cache;
}

// The materials a few effects patch at load time, by texture.
static void note_special_material(TEXID texture_index, swrMaterial* material)
{
    switch (texture_index) {
    case TEXID_b_alpha_waterfall_ctr_rgb:
        rdMaterial_b_alpha_waterfall_ctr_rgb = (RdMaterial*)material;
        break;
    case TEXID_b_alpha_waterfall_edge_rgb:
        rdMaterial_b_alpha_waterfall_edge_rgb = (RdMaterial*)material;
        break;
    case TEXID_a_alpha_ice03_rgb:
        rdMaterial_a_alpha_ice03_rgb = (RdMaterial*)material;
        break;
    case TEXID_mg_engturbine_rgb:
        rdMaterial_mg_engturbine_rgb = (RdMaterial*)material;
        break;
    case TEXID_clouds2_rgb:
        rdMaterial_InvertTextureColorR4G4B4A4((RdMaterial*)material);
        break;
    default:
        break;
    }
}

// 0x00447490
void refined_swrModel_LoadModelTexture(TEXID texture_index, swrMaterial* SWR_P32* material_ptr, uint8_t* SWR_P32* palette_data_ptr)
{
    uint32_t count;
    void** entries = slots(&count);
    const int index = (int)texture_index;

    if (index < 0 || index >= (int)texture_count || (uint32_t)index >= count) {
        *material_ptr = NULL;
        *palette_data_ptr = NULL;
        return;
    }
    // an entry points at the first loader's {material, palette} pair
    if (entries[index] != NULL) {
        *material_ptr = ((swrMaterial* SWR_P32*)entries[index])[0];
        *palette_data_ptr = ((uint8_t* SWR_P32*)entries[index])[1];
        return;
    }

    int offsets[3];
    swrLoader_ReadAt(swrLoader_TYPE_TEXTURE_BLOCK, index * 8 + 4, offsets, sizeof(offsets));
    for (int i = 0; i < 3; i++)
        offsets[i] = SWAP32(offsets[i]);
    swrModel_LoadTextureDataAndPalette(offsets, (uint8_t* SWR_P32*)material_ptr, palette_data_ptr);
    swrModel_DoConvertTextureDataToRdMaterial(material_ptr, palette_data_ptr);
    entries[index] = material_ptr;
    note_special_material(texture_index, *material_ptr);
}

// 0x004475d0
void refined_swrAssetBuffer_InvalidateTexturesBelow(char* ptr)
{
    uint32_t count;
    void** entries = slots(&count);
    for (uint32_t i = 0; i < count; i++) {
        if ((char*)entries[i] > ptr)
            entries[i] = NULL;
    }
}
