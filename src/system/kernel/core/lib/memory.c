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

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/assert.h"
#include "pedigree/kernel/utilities/utility.h"

#undef memcpy

#define WITH_SSE 0
#define SSE_THRESHOLD 1024
#define STOSB_THRESHOLD 64

#if HOSTED_X64
#define X64 1
#endif

#if UTILITY_LINUX_COVERAGE
#undef _STRING_H
#include <string.h>
#else

#if UTILITY_LINUX
#define EXPORT static
#else
#define EXPORT EXPORTED_PUBLIC
#endif

extern void memzero_xmm_aligned(void *, size_t);
extern void memzero_xmm(void *, size_t);

EXPORT int memcmp(const void *p1, const void *p2, size_t len) PURE;
EXPORT void *memset(void *buf, int c, size_t n);
void *WordSet(void *buf, int c, size_t n);
void *DoubleWordSet(void *buf, unsigned int c, size_t n);
void *QuadWordSet(void *buf, unsigned long long c, size_t n);

EXPORT void *memcpy(void *restrict s1, const void *restrict s2, size_t n);
EXPORT void *memmove(void *s1, const void *s2, size_t n);

// asan provides a memcpy/memset/etc that we care about more than our custom
// ones, in general.
#if !HAS_ADDRESS_SANITIZER

EXPORT int memcmp(const void *p1, const void *p2, size_t len)
{
    const char *a = (const char *) p1;
    const char *b = (const char *) p2;
    size_t i = 0;
    int r = 0;
    for (; i < len; i++)
    {
        if ((r = a[i] - b[i]) != 0)
            break;
    }
    return r;
}

EXPORT void *memset(void *buf, int c, size_t n)
{
#ifdef TARGET_IS_X86
    if (n >= STOSB_THRESHOLD)
    {
        int a, b;
        __asm__ __volatile__("rep stosb"
                             : "=&D"(a), "=&c"(b)
                             : "0"(buf), "a"(c), "1"(n)
                             : "memory");
        return buf;
    }
#endif
    unsigned char *tmp = (unsigned char *) buf;
    while (n--)
    {
        *tmp++ = c;
    }
    return buf;
}

EXPORT void *memcpy(void *restrict s1, const void *restrict s2, size_t n)
{
#ifdef TARGET_IS_X86
    if (n >= STOSB_THRESHOLD)
    {
        int a, b, c;
        __asm__ __volatile__("rep movsb"
                             : "=&c"(a), "=&D"(b), "=&S"(c)
                             : "1"(s1), "2"(s2), "0"(n)
                             : "memory");
        return s1;
    }
#endif
    const unsigned char *restrict sp = (const unsigned char *restrict) s2;
    unsigned char *restrict dp = (unsigned char *restrict) s1;
    while (n--)
        *dp++ = *sp++;
    return s1;
}

#ifdef TARGET_IS_X86
static inline void *memmove_x86(void *s1, const void *s2, size_t n)
{
    // Perform rep movsb in reverse.
    const unsigned char *sp = (const unsigned char *) s2 + (n - 1);
    unsigned char *dp = (unsigned char *) s1 + (n - 1);

    int a, b, c;
    __asm__ __volatile__("std; rep movsb; cld"
                         : "=&c"(a), "=&D"(b), "=&S"(c)
                         : "1"(dp), "2"(sp), "0"(n)
                         : "memory");
    return s1;
}
#endif

EXPORT void *memmove(void *s1, const void *s2, size_t n)
{
    if (UNLIKELY(!n))
        return s1;

    const size_t orig_n = n;
    if (LIKELY((s1 < s2) || !overlaps(s1, s2, n)))
    {
        // No overlap, or there's overlap but we can copy forwards.
        memcpy(s1, s2, n);
    }
    else
    {
#ifdef TARGET_IS_X86
        if (n >= STOSB_THRESHOLD)
        {
            memmove_x86(s1, s2, n);
        }
        else
        {
#endif
        // Writing bytes from s2 into s1 cannot be done forwards, use memmove.
        const unsigned char *sp = (const unsigned char *) s2 + (n - 1);
        unsigned char *dp = (unsigned char *) s1 + (n - 1);
        for (; n != 0; n--)
            *dp-- = *sp--;
#ifdef TARGET_IS_X86
        }
#endif
    }

#if EXCESSIVE_ADDITIONAL_CHECKS
    // We can't memcmp if the regions overlap at all.
    if (LIKELY(!overlaps(s1, s2, orig_n)))
    {
        assert(!memcmp(s1, s2, orig_n));
    }
#endif

    return s1;
}

#endif  // HAS_ADDRESS_SANITIZER

#endif  // UTILITY_LINUX_COVERAGE

int overlaps(const void *s1, const void *s2, size_t n)
{
    uintptr_t a = (uintptr_t) s1;
    uintptr_t a_end = (uintptr_t) s1 + n;
    uintptr_t b = (uintptr_t) s2;
    uintptr_t b_end = (uintptr_t) s2 + n;

    return (a <= b_end) && (b <= a_end) ? 1 : 0;
}

void *WordSet(void *buf, int c, size_t n)
{
#ifdef TARGET_IS_X86
    if (n >= STOSB_THRESHOLD)
    {
        int a, b;
        __asm__ __volatile__("rep stosw"
                             : "=&D"(a), "=&c"(b)
                             : "0"(buf), "a"(c), "1"(n)
                             : "memory");
        return buf;
    }
#endif
    unsigned short *tmp = (unsigned short *) buf;
    while (n--)
    {
        *tmp++ = c;
    }
    return buf;
}

void *DoubleWordSet(void *buf, unsigned int c, size_t n)
{
#ifdef TARGET_IS_X86
    if (n >= STOSB_THRESHOLD)
    {
        int a, b;
        __asm__ __volatile__("rep stosl"
                             : "=&D"(a), "=&c"(b)
                             : "0"(buf), "a"(c), "1"(n)
                             : "memory");
        return buf;
    }
#endif
    unsigned int *tmp = (unsigned int *) buf;
    while (n--)
    {
        *tmp++ = c;
    }
    return buf;
}

void *QuadWordSet(void *buf, unsigned long long c, size_t n)
{
#ifdef TARGET_IS_X86
    if (n >= STOSB_THRESHOLD)
    {
        int a, b;
        __asm__ __volatile__("rep stosq"
                             : "=&D"(a), "=&c"(b)
                             : "0"(buf), "a"(c), "1"(n)
                             : "memory");
        return buf;
    }
#endif
    unsigned long long *p = (unsigned long long *) buf;
    while (n--)
        *p++ = c;
    return buf;
}

// We still need memcpy etc as linked symbols for GCC optimisations, but we
// don't have to have their prototypes widely available. So, we implement our
// main functions in terms of the base calls.
void *ForwardMemoryCopy(void *a, const void *b, size_t c)
{
    return memcpy(a, b, c);
}

void *MemoryCopy(void *a, const void *b, size_t c)
{
    return memmove(a, b, c);
}

void *ByteSet(void *a, int b, size_t c)
{
    return memset(a, b, c);
}

int MemoryCompare(const void *a, const void *b, size_t c)
{
    return memcmp(a, b, c);
}
