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
#include "pedigree/kernel/utilities/utility.h"
#include <stdarg.h>
#include <stddef.h>

extern void *malloc(size_t);
extern void free(void *);

EXPORTED_PUBLIC size_t strlen(const char *s);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t len);
EXPORTED_PUBLIC int strcmp(const char *p1, const char *p2);
EXPORTED_PUBLIC int strncmp(const char *p1, const char *p2, size_t n);
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t n);
char *strchr(const char *str, int target);
char *strrchr(const char *str, int target);
int vsprintf(char *buf, const char *fmt, va_list arg);
unsigned long strtoul(const char *nptr, char const **endptr, int base);

#define ULONG_MAX -1

char toUpper(char c)
{
    if (c < 'a' || c > 'z')
        return c;  // special chars
    c += ('A' - 'a');
    return c;
}

char toLower(char c)
{
    if (c < 'A' || c > 'Z')
        return c;  // special chars
    c -= ('A' - 'a');
    return c;
}

int max(size_t a, size_t b)
{
    return a > b ? a : b;
}

int min(size_t a, size_t b)
{
    return a > b ? b : a;
}

WEAK size_t _StringLength(const char *src)
{
    if (!src)
    {
        return 0;
    }

    // Unrolled loop that still avoids reading past the end of src (instead of
    // e.g. doing bitmasks with 64-bit views of src).
    const char *orig = src;
    size_t result = 0;
    while (1)
    {
#define UNROLL(n)    \
    if (!*(src + n)) \
        return (src + n) - orig;
        UNROLL(0);
        UNROLL(1);
        UNROLL(2);
        UNROLL(3);
        UNROLL(4);
        UNROLL(5);
        UNROLL(6);
        UNROLL(7);
#undef UNROLL
        src += 8;
    }
}

char *StringCopy(char *dest, const char *src)
{
    char *orig_dest = dest;
    while (*src)
    {
        *dest = *src;
        ++dest;
        ++src;
    }
    *dest = '\0';

    return orig_dest;
}

char *StringCopyN(char *dest, const char *src, size_t len)
{
    char *orig_dest = dest;
    while (len && LIKELY(*src))
    {
        *dest = *src;
        --len;
        ++dest;
        ++src;
    }

    // zero-pad if we hit the end of src but len is still non-zero
    while (len)
    {
        *dest = '\0';
        --len;
        ++dest;
    }

    return orig_dest;
}

int StringFormat(char *buf, const char *fmt, ...)
{
    va_list args;
    int i;

    va_start(args, fmt);
    i = VStringFormat(buf, fmt, args);
    va_end(args);

    return i;
}

WEAK int StringCompare(const char *p1, const char *p2)
{
    if (p1 == p2)
        return 0;

    while (*p1 && *p2)
    {
        char c = *p1 - *p2;
        if (c)
            return c;
        ++p1;
        ++p2;
    }

    return *p1 - *p2;
}

WEAK int StringCompareN(const char *p1, const char *p2, size_t n)
{
    if (!n)
        return 0;
    if (p1 == p2)
        return 0;

    while (*p1 && *p2)
    {
        char c = *p1 - *p2;
        if (c)
            return c;
        else if (!--n)
            return *p1 - *p2;

        ++p1;
        ++p2;
    }

    return *p1 - *p2;
}

WEAK int
StringCompareNOffset(const char *p1, const char *p2, size_t n, size_t *offset)
{
    if (!n)
    {
        return 0;
    }
    if (p1 == p2)
    {
        return 0;
    }

    size_t orig_n = n;

    while (*p1 && *p2)
    {
        char c = *p1 - *p2;
        if (c || !--n)
            break;

        ++p1;
        ++p2;
    }

    char c = *p1 - *p2;
    if (c && offset)
    {
        *offset = orig_n - n;
    }
    return c;
}

WEAK int StringMatch(const char *p1, const char *p2)
{
    if (p1 == p2)
        return 0;

    while (*p1 && *p2)
    {
        if (*p1 != *p2)
        {
            return 1;
        }
        ++p1;
        ++p2;
    }

    return (*p1 == *p2) ? 0 : 1;
}

WEAK int StringMatchN(const char *p1, const char *p2, size_t n)
{
    if (!n)
    {
        return 0;
    }
    else if (p1 == p2)
    {
        return 0;
    }

    size_t i;
    for (i = 0; i < n; ++i)
    {
        if (p1[i] != p2[i])
        {
            return 1;
        }
    }

    return 0;
}

WEAK int
StringMatchNOffset(const char *p1, const char *p2, size_t n, size_t *offset)
{
    if (!n)
    {
        return 0;
    }
    else if (p1 == p2)
    {
        return 0;
    }

    size_t i;
    for (i = 0; i < n; ++i)
    {
        if (p1[i] != p2[i])
        {
            *offset = i;
            return 1;
        }
    }

    return 0;
}

char *StringConcat(char *dest, const char *src)
{
    char *origDest = dest;
    while (*dest) ++dest;
    while (src && *src)
    {
        *dest++ = *src++;
    }

    *dest++ = 0;

    return origDest;
}

char *StringConcatN(char *dest, const char *src, size_t n)
{
    char *origDest = dest;
    while (*dest) ++dest;
    while (src && *src && n)
    {
        *dest++ = *src++;
        --n;
    }

    *dest++ = 0;

    return origDest;
}

int isspace(int c)
{
    return (c == ' ' || c == '\n' || c == '\r' || c == '\t');
}

int isupper(int c)
{
    return (c >= 'A' && c <= 'Z');
}

int islower(int c)
{
    return (c >= 'a' && c <= 'z');
}

int isdigit(int c)
{
    return (c >= '0' && c <= '9');
}

int isalpha(int c)
{
    return isupper(c) || islower(c) || isdigit(c);
}

unsigned long
StringToUnsignedLong(const char *nptr, char const **endptr, int base)
{
    register const char *s = nptr;
    register unsigned long acc;
    register int c;
    register unsigned long cutoff;
    register int neg = 0, any, cutlim;

    /*
     * See strtol for comments as to the logic used.
     */
    do
    {
        c = *s++;
    } while (isspace(c));
    if (c == '-')
    {
        neg = 1;
        c = *s++;
    }
    else if (c == '+')
        c = *s++;
    if ((base == 0 || base == 16) && c == '0' && (*s == 'x' || *s == 'X'))
    {
        c = s[1];
        s += 2;
        base = 16;
    }
    if (base == 0)
        base = c == '0' ? 8 : 10;
    cutoff = (unsigned long) ULONG_MAX / (unsigned long) base;
    cutlim = (unsigned long) ULONG_MAX % (unsigned long) base;
    for (acc = 0, any = 0;; c = *s++)
    {
        if (isdigit(c))
            c -= '0';
        else if (isalpha(c))
            c -= isupper(c) ? 'A' - 10 : 'a' - 10;
        else
            break;
        if (c >= base)
            break;
        if (any < 0 || acc > cutoff || (acc == cutoff && c > cutlim))
            any = -1;
        else
        {
            any = 1;
            acc *= base;
            acc += c;
        }
    }
    if (any < 0)
    {
        acc = ULONG_MAX;
    }
    else if (neg)
        acc = -acc;
    if (endptr != 0)
        *endptr = (const char *) (any ? s - 1 : nptr);

    return (acc);
}

// Intentionally casting const char * to char * in these functions, don't warn
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-qual"

char *StringFind(const char *str, int target)
{
    const char *s;
    char ch;
    while (1)
    {
#define UNROLL(n)     \
    s = str + n;      \
    ch = *s;          \
    if (!ch)          \
        return NULL;  \
    if (ch == target) \
        return (char *) s;

        UNROLL(0);
        UNROLL(1);
        UNROLL(2);
        UNROLL(3);
        UNROLL(4);
        UNROLL(5);
        UNROLL(6);
        UNROLL(7);
#undef UNROLL
        str += 8;
    }
}

char *StringReverseFind(const char *str, int target)
{
    // StringLength must traverse the entire string once to find the length,
    // so rather than finding the length and then traversing in reverse, we just
    // traverse the string once. This gives a small performance boost.
    const char *s;
    const char *result = NULL;
    char ch;
    while (1)
    {
#define UNROLL(n)      \
    s = str + n;       \
    ch = *s;           \
    if (!ch)           \
        return (char *) result; \
    if (ch == target)  \
        result = s;

        UNROLL(0);
        UNROLL(1);
        UNROLL(2);
        UNROLL(3);
        UNROLL(4);
        UNROLL(5);
        UNROLL(6);
        UNROLL(7);
#undef UNROLL
        str += 8;
    }
}

#pragma GCC diagnostic pop

int StringContains(const char *str, const char *search)
{
    size_t alen = StringLength(str);
    size_t blen = StringLength(search);
    return StringContainsN(str, alen, search, blen);
}

static int isPrefix(const char *word, size_t wordLength, size_t pos)
{
    size_t suffixLength = wordLength - pos;
    return StringCompareN(word, word + pos, suffixLength) == 0 ? 1 : 0;
}

static size_t suffixLength(const char *word, size_t wordLength, size_t pos)
{
    size_t i = 0;
    for (; (word[pos - i] == word[wordLength - 1 - i]) && (i < pos); i++)
        ;
    return i;
}

int StringContainsN(
    const char *str, size_t len, const char *search, size_t slen)
{
    // Quick exit cases (these shouldn't really be "contains" queries).
    if (len < slen)
    {
        return 0;
    }
    else if (!slen)
    {
        return 1;
    }
    else if (!len)
    {
        return 0;
    }
    else if (len == slen)
    {
        return StringCompareN(str, search, slen) == 0;
    }

    // Boyer-Moore string searching (around 2x faster than a naive search)
    size_t delta1[256];
    size_t *delta2 = (size_t *) malloc(slen * sizeof(size_t));

    for (size_t i = 0; i < 256; ++i)
    {
        delta1[i] = slen;
    }

    // Build delta1 array (deltas of rightmost unique character in pattern).
    for (size_t i = 0; i < slen; ++i)
    {
        delta1[(int) search[i]] = slen - 1 - i;
    }

    // Build delta2 array (full match alignment).
    ByteSet(delta2, 0, slen * sizeof(size_t));

    ssize_t lastPrefix = slen - 1;
    for (ssize_t i = slen - 1; i >= 0; --i)
    {
        if (isPrefix(search, slen, i + 1))
        {
            lastPrefix = i + 1;
        }
        delta2[i] = lastPrefix + (slen - 1 - i);
    }
    for (size_t i = 0; i < slen - 1; ++i)
    {
        size_t suffixLen = suffixLength(search, slen, i);
        if (search[i - suffixLen] != search[slen - 1 - suffixLen])
        {
            delta2[slen - 1 - suffixLen] = slen - 1 - i + suffixLen;
        }
    }

    for (size_t i = slen - 1; i < len;)
    {
        ssize_t j = slen - 1;
        while (j >= 0 && (str[i] == search[j]))
        {
            --i;
            --j;
        }

        if (j < 0)
        {
            free(delta2);
            return 1;
        }

        i += max(delta1[(int) str[i]], delta2[j]);
    }

    free(delta2);
    return 0;
}

int StringCompareCase(
    const char *s1, const char *s2, int sensitive, size_t length,
    size_t *offset)
{
    // Case-sensitive compare is just strncmp, basically.
    if (LIKELY(sensitive))
    {
        return StringCompareNOffset(s1, s2, length, offset);
    }

    if (!length)
    {
        return 0;
    }
    else if (s1 == s2)
    {
        if (offset)
        {
            *offset = StringLength(s1);
        }
        return 0;
    }
    else if (!s1)
    {
        return -1;
    }
    else if (!s2)
    {
        return 1;
    }

    static size_t local = 0;
    if (UNLIKELY(!offset))
    {
        offset = &local;
    }

    // Case insensitive search.
    size_t i = 0;
    while (*s1 && *s2)
    {
        char c = *s1 - *s2;
        if (c)
        {
            // Didn't match, check if that's because the case was wrong.
            c = toLower(*s1) - toLower(*s2);
            if (c)
            {
                break;
            }
        }
        else if (!--length)
        {
            break;
        }

        ++s1;
        ++s2;
        ++i;
    }

    *offset = i;
    return toLower(*s1) - toLower(*s2);
}

size_t nextCharacter(const char *s, size_t i)
{
    if (UNLIKELY(!s))
    {
        return i;
    }

    // UTF-8 version of getting the next character
    const uint8_t *u8buf = (const uint8_t *) s;
    if (LIKELY(u8buf[i] <= 0x7F))
    {
        return i + 1;
    }
    else if ((u8buf[i] & 0xC0) == 0xC0)
    {
        if ((u8buf[i] & 0xF8) == 0xF0)
        {
            return i + 4;  // 4-byte sequence
        }
        else if ((u8buf[i] & 0xF0) == 0xE0)
        {
            return i + 3;
        }
        else
        {
            return i + 2;
        }
    }
    return i + 1;
}

size_t prevCharacter(const char *s, size_t i)
{
    if (!s)
    {
        return i;
    }

    // TODO handle multibyte chars.
    return i - 1;
}

#if !UTILITY_LINUX
// Provide forwarding functions to handle GCC optimising things.
size_t strlen(const char *s)
{
    return StringLength(s);
}

char *strcpy(char *dest, const char *src)
{
    return StringCopy(dest, src);
}

char *strncpy(char *dest, const char *src, size_t len)
{
    return StringCopyN(dest, src, len);
}

int strcmp(const char *p1, const char *p2)
{
    return StringCompare(p1, p2);
}

int strncmp(const char *p1, const char *p2, size_t n)
{
    return StringCompareN(p1, p2, n);
}

char *strcat(char *dest, const char *src)
{
    return StringConcat(dest, src);
}

char *strncat(char *dest, const char *src, size_t n)
{
    return StringConcatN(dest, src, n);
}

char *strchr(const char *str, int target)
{
    return StringFind(str, target);
}

char *strrchr(const char *str, int target)
{
    return StringReverseFind(str, target);
}

int vsprintf(char *buf, const char *fmt, va_list arg)
{
    return VStringFormat(buf, fmt, arg);
}

unsigned long strtoul(const char *nptr, char const **endptr, int base)
{
    return StringToUnsignedLong(nptr, endptr, base);
}
#endif
