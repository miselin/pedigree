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

#include <compiler.h>
#include <processor/types.h>

#undef memcpy

// Condense X86-ish systems into one define for this file.
#if defined(X86_COMMON) || defined(HOSTED_X86_COMMON)
#define IS_X86
#endif

#ifdef HOSTED_X64
#define X64
#endif

void *memset(void *buf, int c, size_t n)
{
#ifdef IS_X86
    int a, b;
    asm volatile("rep stosb" : "=&D" (a), "=&c" (b) : "0" (buf), "a" (c), "1" (n) : "memory");
    return buf;
#else
    unsigned char *tmp = (unsigned char *)buf;
    for(size_t i = 0; i < n; ++i)
    {
      *tmp++ = c;
    }
    return buf;
#endif
}

void *wmemset(void *buf, int c, size_t n)
{
#ifdef IS_X86
    int a, b;
    asm volatile("rep stosw" : "=&D" (a), "=&c" (b) : "0" (buf), "a" (c), "1" (n) : "memory");
    return buf;
#else
    unsigned short *tmp = (unsigned short *)buf;
    while(n--)
    {
      *tmp++ = c;
    }
    return buf;
#endif
}

void *dmemset(void *buf, unsigned int c, size_t n)
{
#ifdef IS_X86
    int a, b;
    asm volatile("rep stosl" : "=&D" (a), "=&c" (b) : "0" (buf), "a" (c), "1" (n) : "memory");
    return buf;
#else
    unsigned int *tmp = (unsigned int *)buf;
    while(n--)
    {
      *tmp++ = c;
    }
    return buf;
#endif
}

void *qmemset(void *buf, unsigned long long c, size_t n)
{
#ifdef X64
    int a, b;
    asm volatile("rep stosq" : "=&D" (a), "=&c" (b) : "0" (buf), "a" (c), "1" (n) : "memory");
    return buf;
#else
    unsigned long long *p = (unsigned long long*) buf;
    while(n--)
      *p++ = c;
    return buf;
#endif
}

void *memcpy(void *restrict s1, const void *restrict s2, size_t n)
{
#ifdef IS_X86
    int a, b, c;
    asm volatile("rep movsb" : "=&c" (a), "=&D" (b), "=&S" (c): "1" (s1), "2" (s2), "0" (n) : "memory");
    return s1;
#else
    const unsigned char *restrict sp = (const unsigned char *restrict)s2;
    unsigned char *restrict dp = (unsigned char *restrict)s1;
    for (; n != 0; n--) *dp++ = *sp++;
    return s1;
#endif
}

static int overlaps(const void *restrict s1, const void *restrict s2, size_t n) CONST;
static int overlaps(const void *restrict s1, const void *restrict s2, size_t n)
{
  uintptr_t a = (uintptr_t) s1;
  uintptr_t a_end = (uintptr_t) s1 + n;
  uintptr_t b = (uintptr_t) s2;
  uintptr_t b_end = (uintptr_t) s2 + n;

  return (a < b_end && b < a_end) ? 1 : 0;
}

void *memmove(void *s1, const void *s2, size_t n)
{
  if (UNLIKELY(!n)) return s1;

  const size_t orig_n = n;
  if (s1 < s2)
  {
    // Writing bytes from s2 into s1 can be done forwards, use memcpy.
    memcpy(s1, s2, n);
  }
  else
  {
    // Writing bytes from s2 into s1 cannot be done forwards, use memmove.
    const unsigned char *sp = (const unsigned char *) s2 + (n - 1);
    unsigned char *dp = (unsigned char *) s1 + (n - 1);
    for (; n != 0; n--) *dp-- = *sp--;
  }

#ifdef ADDITIONAL_CHECKS
    // We can't memcmp if the regions overlap at all.
    if (LIKELY(!overlaps(s1, s2, orig_n)))
    {
      assert(!memcmp(s1, s2, orig_n));
    }
#endif

  return s1;
}

int memcmp(const void *p1, const void *p2, size_t len)
{
    const char* a = (const char*) p1;
    const char* b = (const char*) p2;
    size_t i = 0;
    int r = 0;
    for(; i < len; i++)
    {
        if ((r = a[i] - b[i]) != 0)
          break;
    }
    return r;
}
