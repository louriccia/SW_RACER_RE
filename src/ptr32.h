#ifndef SWR_PTR32_H
#define SWR_PTR32_H

// Retail data, asset files and the decompiled code's raw offsets all assume 4-byte pointers. On a
// 64-bit build every pointer stored inside a game struct is 4 bytes and all game memory and code
// live below 2 GB, so layouts stay byte-identical. On 32-bit builds these are plain pointers and
// native types.
//
// Data pointers: SWR_P32 (clang __ptr32, sign-extended on load).
// Function pointers: SWR_FNSLOT / SWR_FNSLOT_T, a 4-byte integer slot that remembers the pointer
// type. Clang miscompiles calls through __ptr32 function pointers (it loads 8 bytes from the slot),
// so a slot is read with SWR_FN32(slot), written with SWR_FN32_SET(slot, fn) and compared through
// SWR_FN32_RAW(slot).

#include <stdint.h>

#if defined(__x86_64__) && defined(__clang__)
#include <_mingw.h>
#undef __ptr32// mingw defines it empty for gcc
#define SWR_P32 __ptr32 __sptr
#define SWR_WPARAM32 uint32_t
#define SWR_LPARAM32 int32_t
#define SWR_SIZE32 uint32_t
#define SWR_INTPTR32 int32_t

#define SWR_FNSLOT(ret, name, params)                                                              \
    union __attribute__((packed, aligned(4))) {                                                    \
        uint32_t raw;                                                                              \
        ret(*proto[0]) params;                                                                     \
    } name
#define SWR_FNSLOT_T(fntype, name)                                                                 \
    union __attribute__((packed, aligned(4))) {                                                    \
        uint32_t raw;                                                                              \
        fntype proto[0];                                                                           \
    } name
#define SWR_FN32(f) ((__typeof__((f).proto[0])) (uintptr_t) (f).raw)
static inline uint32_t swr_fn32_narrow(uintptr_t v)
{
    if (v >> 32)
        __builtin_trap();// a function outside our image (system DLL): needs an in-image thunk
    return (uint32_t) v;
}
#define SWR_FN32_SET(f, v) ((f).raw = swr_fn32_narrow((uintptr_t) (v)))
#define SWR_FN32_RAW(f) SWR_FN32(f)

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
#define SWR_P32
#define SWR_WPARAM32 WPARAM
#define SWR_LPARAM32 LPARAM
#define SWR_SIZE32 size_t
#define SWR_INTPTR32 intptr_t

#define SWR_FNSLOT(ret, name, params) ret (*name) params
#define SWR_FNSLOT_T(fntype, name) fntype name
#define SWR_FN32(f) (f)
#define SWR_FN32_SET(f, v) ((f) = (v))
#define SWR_FN32_RAW(f) (f)

#define SWR_OUT_NATIVE(T, slot, call)                                                              \
    ({                                                                                             \
        T *const swr_out_ = (T *) &(slot);                                                         \
        (call);                                                                                    \
    })
#endif

#endif
