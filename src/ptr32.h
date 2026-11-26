#ifndef SWR_PTR32_H
#define SWR_PTR32_H

// Retail data, asset files and the decompiled code's raw offsets all assume 4-byte pointers. On a
// 64-bit build every pointer stored inside a game struct is a 32-bit pointer (clang __ptr32,
// sign-extended on load), and all game memory lives below 2 GB, so layouts stay byte-identical.
// On 32-bit builds these are plain pointers and native types.

#include <stdint.h>

#if defined(__x86_64__) && defined(__clang__)
#include <_mingw.h>
#undef __ptr32// mingw defines it empty for gcc
#define SWR_P32 __ptr32 __sptr
#define SWR_WPARAM32 uint32_t
#define SWR_LPARAM32 int32_t
#define SWR_SIZE32 uint32_t
#define SWR_INTPTR32 int32_t
// `call` writes a native pointer through `swr_out_`; the result is stored in the 4-byte `slot`.
#define SWR_OUT_NATIVE(T, slot, call)                                                              \
    ({                                                                                             \
        T swr_native_ = (T) (slot);                                                                \
        T *const swr_out_ = &swr_native_;                                                          \
        __auto_type swr_r_ = (call);                                                               \
        (slot) = swr_native_;                                                                      \
        swr_r_;                                                                                    \
    })
#else
#define SWR_OUT_NATIVE(T, slot, call)                                                              \
    ({                                                                                             \
        T *const swr_out_ = &(slot);                                                               \
        (call);                                                                                    \
    })
#define SWR_P32
#define SWR_WPARAM32 WPARAM
#define SWR_LPARAM32 LPARAM
#define SWR_SIZE32 size_t
#define SWR_INTPTR32 intptr_t
#endif

#endif
