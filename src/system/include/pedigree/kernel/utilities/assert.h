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

#ifndef KERNEL_ASSERT_H
#define KERNEL_ASSERT_H

#if UTILITY_LINUX
// Redirect to system assert.h
#include <assert.h>
#else
#include "pedigree/kernel/compiler.h"
#include <stdint.h>

/** @addtogroup kernel
 * @{ */

/** If the passed argument resolves to a Boolean false value, execution will be
 * halted and a message displayed.
 */
#if DEBUGGER || ASSERTS
#define assert(x)                                          \
    do                                                     \
    {                                                      \
        bool __pedigree_assert_chk = (x);                  \
        if (UNLIKELY(!__pedigree_assert_chk))              \
            _assert(                                       \
                __pedigree_assert_chk, __FILE__, __LINE__, \
                __PRETTY_FUNCTION__);                      \
    } while (0)

#if !USE_DEBUG_ALLOCATOR
#define assert_heap_ptr_valid(x)                                               \
    _assert(                                                                   \
        _assert_ptr_valid(reinterpret_cast<uintptr_t>(x)), __FILE__, __LINE__, \
        __PRETTY_FUNCTION__)
#else
#define assert_heap_ptr_valid
#endif

#elif !defined(assert)
#define assert(x)
#define assert_heap_ptr_valid(x)
#endif

#ifndef __cplusplus
#define bool char
#endif

/// Internal use only, the assert() macro passes the additional arguments
/// automatically
#ifdef __cplusplus
extern "C" {
#endif

void EXPORTED_PUBLIC
_assert(bool b, const char *file, int line, const char *func);
bool _assert_ptr_valid(uintptr_t x);

#ifdef __cplusplus
}
#endif
#endif

/** @} */

#endif
