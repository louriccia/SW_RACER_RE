# swrAssetBuffer (refined)

The level asset arena: one bump-allocated block holding a race's models, textures and splines.

## Boundary
| Retail | Faithful | Refined | Tier |
|---|---|---|---|
| 0x00449040 | `swrScene_InitWorld` | `refined_swrScene_InitWorld` | equivalent |

Globals written: `assetBuffer`, `assetBufferEnd`, `swrAssetBuffer_initialBase`, the nine saved
marks after `assetBuffer`, `swrViewport_frameParity`. Calls `swrScene_LoadObjects`,
`swrScene_InitFog`, `swrAssetBuffer_CheckOverflow` (faithful).

## Flaws addressed
| Tag | Finding | Refinement |
|---|---|---|
| HARDCODE / LIMIT | Arena size is two 8 MiB immediates (malloc request and end pointer) | `refined_asset_buffer_set_size()`; default = retail size |

## Band-aids retired
`swrAssetBuffer_PatchSize` no longer patches the two immediates in the standalone; it sets the
size through the API (settings.ini `asset_buffer_mb`).

## Oracle
Lockstep with the refined function active and a 16 MiB arena: tracks 5 and 14 ALL MATCH, full
25-track sweep. A larger arena only moves where allocations land; nothing in the simulation
depends on it.
