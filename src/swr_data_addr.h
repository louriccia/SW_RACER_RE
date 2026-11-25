#ifndef SWR_DATA_ADDR_H
#define SWR_DATA_ADDR_H

// Where a retail data address lives. The hook DLL runs inside retail, so every global is at its
// retail address. The standalone (SWR_DATA_RELOC) keeps retail's .rdata/.data/.bss image in its own
// swr_data_image instead, so addresses in [SWR_DATA_LO, SWR_DATA_HI) map into that array; with
// SWR_GLOBALS_OWNED most named globals are separate variables (globals_owned.h) and only the rest
// stays in the image.
//
// SWR_IMAGE_PTR(a): the address in the image (a constant; what globals.h uses).
// SWR_GLOBAL_PTR(a): wherever retail address a lives now, owned variable or image.
#define SWR_DATA_LO 0x004ac000u
#define SWR_DATA_HI 0x00ece000u

#ifdef SWR_DATA_RELOC
#ifdef __cplusplus
extern "C" {
#endif
extern char swr_data_image[];
void* swr_data_ptr(unsigned retail);
#ifdef __cplusplus
}
#endif
#define SWR_IMAGE_PTR(a)                                                                                                \
    ((unsigned)(a) >= SWR_DATA_LO && (unsigned)(a) < SWR_DATA_HI ? (void*)(swr_data_image + ((unsigned)(a) - SWR_DATA_LO)) \
                                                                   : (void*)(a))
#ifdef SWR_GLOBALS_OWNED
#define SWR_GLOBAL_PTR(a) swr_data_ptr((unsigned)(a))
#else
#define SWR_GLOBAL_PTR(a) SWR_IMAGE_PTR(a)
#endif
#else
#define SWR_IMAGE_PTR(a) ((void*)(a))
#define SWR_GLOBAL_PTR(a) ((void*)(a))
#endif

#endif // SWR_DATA_ADDR_H
