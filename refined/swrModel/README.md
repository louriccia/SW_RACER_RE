# swrModel texture cache (refined)

One slot per texture of the open texture block, holding the loader's {material, palette} pair
once the texture is converted. Retail sizes it for its own block (`texture_buffer`, 1700
entries) and reaches it from three functions.

## Boundary
| Retail | Faithful | Refined | Tier |
|---|---|---|---|
| 0x00447490 | `swrModel_LoadModelTexture` | `refined_swrModel_LoadModelTexture` | equivalent |
| 0x004475d0 | `swrAssetBuffer_InvalidateTexturesBelow` | `refined_swrAssetBuffer_InvalidateTexturesBelow` | equivalent |
| 0x00447420 | `swrModel_InitializeTextureBuffer` | (modding layer delta, sizes the cache) | — |

State: the cache (`refined_texture_cache_resize`). Until a block sizes it, the refined functions
use retail's `texture_buffer`, so they are correct with or without the custom-track delta.

## Flaws addressed
| Tag | Finding | Refinement |
|---|---|---|
| LIMIT | Fixed 1700 slots; a block with more textures indexes past the array | Cache sized by the block's texture count |
| DUP / CLEVER | Five per-texture `if`s patch special materials | One `switch` (`note_special_material`), same order of effects |

## Band-aids retired
`swrModel_InitializeTextureBuffer_delta` no longer writes five operands in retail's texture code
(0x4474B1..0x4475E7) in the standalone; it calls `refined_texture_cache_resize`.

## Oracle
- Lockstep tracks 5 / 14 and the 25-track sweep with the refined functions active.
- Texture-hash oracle (lockstep `texture_hash`), track 14: refined vs `SWR_REFINED_OFF=1`,
  2134 uploads, identical hashes in the same order.
- Custom track "bigblue" (1676 textures): the cache grows 1648 -> 1676 and the track renders
  with its own textures.
