#ifndef SWR_DATA_ADDR_H
#define SWR_DATA_ADDR_H

// Where a retail data address lives. The hook DLL runs inside retail, so every global is at its
// retail address. The standalone (SWR_DATA_RELOC) keeps retail's .rdata/.data/.bss image in its own
// swr_data_image instead, so addresses in [SWR_DATA_LO, SWR_DATA_HI) map into that array.
#define SWR_DATA_LO 0x004ac000u
#define SWR_DATA_HI 0x00ece000u

#ifdef SWR_DATA_RELOC
#ifdef __cplusplus
extern "C" char swr_data_image[];
#else
extern char swr_data_image[];
#endif
#define SWR_GLOBAL_PTR(a)                                                                                              \
    ((unsigned)(a) >= SWR_DATA_LO && (unsigned)(a) < SWR_DATA_HI ? (void*)(swr_data_image + ((unsigned)(a) - SWR_DATA_LO)) \
                                                                   : (void*)(a))
#else
#define SWR_GLOBAL_PTR(a) ((void*)(a))
#endif

#endif // SWR_DATA_ADDR_H
