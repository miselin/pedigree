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
#include "pedigree/kernel/utilities/lib.h"
#include "pedigree/kernel/utilities/utility.h"

#ifdef TARGET_IS_X86

int StringCompare(const char *p1, const char *p2)
{
    if (p1 == p2)
        return 0;

    register uintptr_t a, b, c;
    asm volatile("repe cmpsb" : "=&D" (a), "=&S" (b), "=&c" (c) : "0" (p1), "1" (p2), "2" (~0UL) : "memory");

    c = ~0UL - c - 1;

    return p1[c] - p2[c];
}

int StringCompareN(const char *p1, const char *p2, size_t n)
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
                return c;
            }
        }

        return 0;
    }

    register uintptr_t a, b, c;
    asm volatile("repe cmpsb" : "=&D" (a), "=&S" (b), "=&c" (c) : "0" (p1), "1" (p2), "2" (n) : "memory");

    c = n - c - 1;

    return p1[c] - p2[c];
}

#endif
