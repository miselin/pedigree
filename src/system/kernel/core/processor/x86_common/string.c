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

#include "pedigree/kernel/processor/types.h"
#include "pedigree/kernel/utilities/utility.h"

#ifdef TARGET_IS_X86

int StringCompare(const char *p1, const char *p2)
{
    if (p1 == p2)
        return 0;

    size_t l1 = StringLength(p1);
    size_t l2 = StringLength(p2);
    size_t l = min(l1, l2);

    register uintptr_t a, b, c;
    __asm__ __volatile__("repe cmpsb"
                         : "=&D"(a), "=&S"(b), "=&c"(c)
                         : "0"(p1), "1"(p2), "2"(l + 1)
                         : "memory");

    c = l - c;

    return p1[c] - p2[c];
}

int StringCompareN(const char *p1, const char *p2, size_t n)
{
    return StringCompareNOffset(p1, p2, n, 0);
}

int StringCompareNOffset(
    const char *p1, const char *p2, size_t n, size_t *offset)
{
    if (!n)
        return 0;
    if (p1 == p2)
        return 0;

    if (n < 64)
    {
        for (size_t i = 0; i < n; ++i)
        {
            char c = p1[i] - p2[i];
            if (c)
            {
                if (offset)
                {
                    *offset = i;
                }
                return c;
            }
        }

        return 0;
    }

    register uintptr_t a, b, c;
    __asm__ __volatile__("repe cmpsb"
                         : "=&D"(a), "=&S"(b), "=&c"(c)
                         : "0"(p1), "1"(p2), "2"(n)
                         : "memory");

    c = n - c - 1;

    char d = p1[c] - p2[c];
    if (d && offset)
    {
        *offset = c;
    }
    return d;
}

#endif
