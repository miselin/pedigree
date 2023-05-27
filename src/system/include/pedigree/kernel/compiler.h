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

#ifndef KERNEL_COMPILER_H
#define KERNEL_COMPILER_H

/** @addtogroup kernel
 * @{ */

// NOTE: See http://gcc.gnu.org/onlinedocs/gcc/Function-Attributes.html
//           http://gcc.gnu.org/onlinedocs/gcc/Variable-Attributes.html
//           http://gcc.gnu.org/onlinedocs/gcc/Type-Attributes.html
//           http://www.imodulo.com/gnu/gcc/Other-Builtins.html
//       for more information.

/** Deprecated function/variable/type */
#define DEPRECATED __attribute__((deprecated))
/** Function does not return */
#define NORETURN __attribute__((noreturn))
/** Function does not have a side-effect (except for the return value) */
#define PURE __attribute__((pure))
/** Function does not have a side-effect and does only depend on its arguments
 */
#define CONST __attribute__((const))
/** Functions that should always be inlined */
#define ALWAYS_INLINE __attribute__((always_inline))
/** Functions that should never be inlined */
#define NEVER_INLINE __attribute__((noinline))
/** Specific alignment for a type/variable */
#define ALIGN(x) __attribute__((aligned(x)))
/** No padding for a type/variable */
#define PACKED __attribute__((packed))
/** The type may alias */
#define MAY_ALIAS __attribute__((may_alias))
/** The expression is very likely to be true */
#define LIKELY(exp) __builtin_expect(!!(exp), 1)
/** The expression is very unlikely to be true */
#define UNLIKELY(exp) __builtin_expect(!!(exp), 0)
/** This code is not reachable. */
#define UNREACHABLE __builtin_unreachable()
/** This function handles a format string. */
#define FORMAT(type, idx, first) __attribute__((format(type, idx, first)))
/** True if the expression is constant at compile time, false otherwise. */
#define IS_CONSTANT(x) __builtin_constant_p(x)
/** This code should be placed in the given section. */
#ifdef __MACH__  // Mach targets have special section specifications.
#define SECTION(x)
#else
#define SECTION(x) __attribute__((__section__(x)))
#endif
/** This function must have its return value used somehow. */
#define MUST_USE_RESULT __attribute__((warn_unused_result))
/** This function or variable is used, even if it doesn't appear that way. */
#define USED __attribute__((used))
/** This function can be overridden by a library. */
#define WEAK __attribute__((weak))
/** A switch fallthrough is indeed intentional. */
#if defined(__GNUC__) && __GNUC__ >= 7
#define FALLSTHROUGH __attribute__((fallthrough));
#else
#define FALLSTHROUGH
#endif

/**
 * This function or variable is to be exposed to other objects.
 * This is needed for anything that the kernel will expose to kernel modules.
 */
#define EXPORTED_PUBLIC __attribute__((visibility("default")))

// Builtin checks.
#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

#ifndef __has_feature
#define __has_feature(x) 0
#endif

#if __has_builtin(__builtin_assume_aligned) || !defined(__clang__)
#define ASSUME_ALIGNMENT(b, sz) __builtin_assume_aligned((b), sz)
#else
#define ASSUME_ALIGNMENT(b, sz) \
    ((reinterpret_cast<uintptr_t>(b) % (sz)) == 0) ? (b) : (UNREACHABLE, (b))
#endif

/** Pack initialisation functions into a special section that could be freed
 * after the kernel initialisation is finished */
#define INITIALISATION_ONLY SECTION(".init.text")
#define INITIALISATION_ONLY_DATA SECTION(".init.data")

// We don't use a custom allocator if asan is enabled.
#if __has_feature(address_sanitizer) || __has_feature(memory_sanitizer)
#define HAS_ADDRESS_SANITIZER 1
#else
#define HAS_ADDRESS_SANITIZER 0
#endif

#if __has_feature(thread_sanitizer)
#define HAS_THREAD_SANITIZER 1
#else
#define HAS_THREAD_SANITIZER 0
#endif

#if HAS_ADDRESS_SANITIZER || HAS_THREAD_SANITIZER
#define HAS_SANITIZERS 1
#endif

#ifdef __cplusplus
#define NOT_COPY_CONSTRUCTIBLE(Type) Type(const Type &) = delete
#define NOT_ASSIGNABLE(Type) Type &operator=(const Type &) = delete
#define NOT_COPYABLE_OR_ASSIGNABLE(Type) \
    NOT_COPY_CONSTRUCTIBLE(Type);        \
    NOT_ASSIGNABLE(Type)

#define WITHOUT_IMPLICIT_CONSTRUCTORS(Type) \
    Type() = delete;                        \
    NOT_COPYABLE_OR_ASSIGNABLE(Type)
#endif

// BARRIER forces GCC to not reorder code across the barrier.
#define BARRIER() __asm__ __volatile__("" ::: "memory")

// FENCE performs a full load/store hardware memory fence.
#define FENCE() __sync_synchronize()

// Export thread-safety annotations if we're being compiled by clang
#include "pedigree/kernel/utilities/threadsafety.h"  // IWYU: export

/** @} */

#endif
