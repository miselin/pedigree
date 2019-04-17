/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef KERNEL_UTILITY_H
#define KERNEL_UTILITY_H

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"

// Export memcpy et al
#include "pedigree/kernel/utilities/cpp.h"  // IWYU pragma: export
#include "pedigree/kernel/utilities/lib.h"  // IWYU pragma: export

#if HOSTED && !UTILITY_LINUX
// Override headers we are replacing.
#define _STRING_H 1
#endif

/** @addtogroup kernelutilities
 * @{ */

// Endianness shizzle.
#define BS8(x) (x)
#define BS16(x) (((x & 0xFF00) >> 8) | ((x & 0x00FF) << 8))
#define BS32(x)                                           \
    (((x & 0xFF000000) >> 24) | ((x & 0x00FF0000) >> 8) | \
     ((x & 0x0000FF00) << 8) | ((x & 0x000000FF) << 24))
#define BS64(x) (x)

#if TARGET_IS_LITTLE_ENDIAN

#define LITTLE_TO_HOST8(x) (x)
#define LITTLE_TO_HOST16(x) (x)
#define LITTLE_TO_HOST32(x) (x)
#define LITTLE_TO_HOST64(x) (x)

#define HOST_TO_LITTLE8(x) (x)
#define HOST_TO_LITTLE16(x) (x)
#define HOST_TO_LITTLE32(x) (x)
#define HOST_TO_LITTLE64(x) (x)

#define BIG_TO_HOST8(x) BS8((x))
#define BIG_TO_HOST16(x) BS16((x))
#define BIG_TO_HOST32(x) BS32((x))
#define BIG_TO_HOST64(x) BS64((x))

#define HOST_TO_BIG8(x) BS8((x))
#define HOST_TO_BIG16(x) BS16((x))
#define HOST_TO_BIG32(x) BS32((x))
#define HOST_TO_BIG64(x) BS64((x))

#else  // else Big endian

#define BIG_TO_HOST8(x) (x)
#define BIG_TO_HOST16(x) (x)
#define BIG_TO_HOST32(x) (x)
#define BIG_TO_HOST64(x) (x)

#define HOST_TO_BIG8(x) (x)
#define HOST_TO_BIG16(x) (x)
#define HOST_TO_BIG32(x) (x)
#define HOST_TO_BIG64(x) (x)

#define LITTLE_TO_HOST8(x) BS8((x))
#define LITTLE_TO_HOST16(x) BS16((x))
#define LITTLE_TO_HOST32(x) BS32((x))
#define LITTLE_TO_HOST64(x) BS64((x))

#define HOST_TO_LITTLE8(x) BS8((x))
#define HOST_TO_LITTLE16(x) BS16((x))
#define HOST_TO_LITTLE32(x) BS32((x))
#define HOST_TO_LITTLE64(x) BS64((x))

#endif

#define MAX_FUNCTION_NAME 128
#define MAX_PARAMS 32
#define MAX_PARAM_LENGTH 64

/** Page-align the given pointer. */
EXPORTED_PUBLIC void *page_align(void *p) PURE;

#ifdef __cplusplus
/** Add a offset (in bytes) to the pointer and return the result
 *\brief Adjust a pointer
 *\return new pointer pointing to 'pointer + offset' (NOT pointer arithmetic!)
 */
template <typename T>
inline T *adjust_pointer(T *pointer, ssize_t offset)
{
    return reinterpret_cast<T *>(reinterpret_cast<intptr_t>(pointer) + offset);
}

template <typename T>
inline void swap(T a, T b)
{
    T t = a;
    a = b;
    b = t;
}

/** Return b - a. */
template <typename T1, typename T2>
inline intptr_t pointer_diff(const T1 *a, const T2 *b)
{
    return reinterpret_cast<uintptr_t>(b) - reinterpret_cast<uintptr_t>(a);
}

template <typename T1, typename T2>
constexpr intptr_t pointer_diff_const(T1 *a, T2 *b)
{
    return reinterpret_cast<uintptr_t>(b) - reinterpret_cast<uintptr_t>(a);
}

/** Return the difference between a and b, without a sign. */
template <typename T1, typename T2>
inline uintptr_t abs_difference(T1 a, T2 b)
{
    intptr_t value = b - a;
    if (value < 0)
        value *= -1;
    return value;
}
#endif

/** @} */

#endif
