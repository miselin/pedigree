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

#ifndef KERNEL_UTILITIES_LIB_H
#define KERNEL_UTILITIES_LIB_H

// IWYU pragma: private, include "pedigree/kernel/utilities/utility.h"

#include "pedigree/kernel/compiler.h"
#include "pedigree/kernel/processor/types.h"
#include <stdarg.h>

// Condense X86-ish systems into one define for utilities.
/// \note this will break testsuite/hosted builds on non-x86 hosts.
#if X86_COMMON || HOSTED_X86_COMMON || UTILITY_LINUX
#define TARGET_IS_X86
#endif

#ifdef __cplusplus
#define NOTHROW noexcept
#else
#define NOTHROW
#endif

#ifdef __cplusplus
extern "C" {
#endif

// This is not a prototype so doesn't fall into the block below.
#define StringLength(x) \
    (IS_CONSTANT(x) ? __builtin_strlen((x)) : _StringLength(x))

// Bounded version of StringLength that avoids reading past the given
// maximum length. Useful for buffers that might have a much smaller string in
// them but doesn't overflow a buffer that doesn't have a null character at the
// end of it.
#define BoundedStringLength(x, y) \
    (IS_CONSTANT(x) ? __builtin_strlen((x)) : _BoundedStringLength(x, y))

#define ConstStringLength(x) static_assert(IS_CONSTANT((x))), __builtin_strlen((x))

// String functions.
EXPORTED_PUBLIC size_t _StringLength(const char *buf) PURE;
EXPORTED_PUBLIC size_t _BoundedStringLength(const char *buf, size_t maxlen) PURE;
EXPORTED_PUBLIC char *StringCopy(char *dest, const char *src);
EXPORTED_PUBLIC char *StringCopyN(char *dest, const char *src, size_t len);
EXPORTED_PUBLIC int StringCompare(const char *p1, const char *p2) PURE;
EXPORTED_PUBLIC int
StringCompareN(const char *p1, const char *p2, size_t n) PURE;
EXPORTED_PUBLIC int StringCompareNOffset(
    const char *p1, const char *p2, size_t n, size_t *offset) PURE;
EXPORTED_PUBLIC char *StringConcat(char *dest, const char *src);
EXPORTED_PUBLIC char *StringConcatN(char *dest, const char *src, size_t n);
EXPORTED_PUBLIC char *StringFind(const char *str, int target) PURE;
EXPORTED_PUBLIC char *StringReverseFind(const char *str, int target) PURE;
EXPORTED_PUBLIC int StringContains(const char *str, const char *search) PURE;
EXPORTED_PUBLIC int StringContainsN(
    const char *str, size_t len, const char *search, size_t slen) PURE;
EXPORTED_PUBLIC int VStringFormat(char *buf, const char *fmt, va_list arg);
EXPORTED_PUBLIC int StringFormat(char *buf, const char *fmt, ...)
    FORMAT(printf, 2, 3);
EXPORTED_PUBLIC unsigned long
StringToUnsignedLong(const char *nptr, char **endptr, int base);

// These work similarly to StringCompare, but only return 0 for match and -1
// for a failed match (rather than returning the sign of the non-matching
// comparison like StringCompare does) - use this if you only ever care that a
// match is present, and don't care about the sign of the non-matching result.
EXPORTED_PUBLIC int StringMatch(const char *p1, const char *p2) PURE;
EXPORTED_PUBLIC int
StringMatchN(const char *p1, const char *p2, size_t n) PURE;
EXPORTED_PUBLIC int StringMatchNOffset(
    const char *p1, const char *p2, size_t n, size_t *offset) PURE;

// Performs NOTICE/ERROR/etc() on the formatted output, which allows the
// Pedigree log to be used from C code.
#if !defined(__cplusplus) || defined(IMPLEMENTING_LOG_FORMAT_FUNCTIONS)
// DO NOT use in C++ code, except to implement.
EXPORTED_PUBLIC int Debugf(const char *fmt, ...) FORMAT(printf, 1, 2);
EXPORTED_PUBLIC int Noticef(const char *fmt, ...) FORMAT(printf, 1, 2);
EXPORTED_PUBLIC int Warningf(const char *fmt, ...) FORMAT(printf, 1, 2);
EXPORTED_PUBLIC int Errorf(const char *fmt, ...) FORMAT(printf, 1, 2);
EXPORTED_PUBLIC int Fatalf(const char *fmt, ...) FORMAT(printf, 1, 2) NORETURN;
#endif

// Compares the two strings with optional case-sensitivity. The offset out
// parameter holds the offset of a failed match in the case of a non-zero
// return, or the length of the string otherwise.
EXPORTED_PUBLIC int StringCompareCase(
    const char *s1, const char *s2, int sensitive, size_t length,
    size_t *offset);

// Memory functions.
EXPORTED_PUBLIC void *ByteSet(void *buf, int c, size_t len);
EXPORTED_PUBLIC void *WordSet(void *buf, int c, size_t len);
EXPORTED_PUBLIC void *DoubleWordSet(void *buf, unsigned int c, size_t len);
EXPORTED_PUBLIC void *QuadWordSet(void *buf, unsigned long long c, size_t len);
EXPORTED_PUBLIC void *ForwardMemoryCopy(void *s1, const void *s2, size_t n);
EXPORTED_PUBLIC void *MemoryCopy(void *s1, const void *s2, size_t n);
EXPORTED_PUBLIC int
MemoryCompare(const void *p1, const void *p2, size_t len) PURE;

// Misc utilities for paths etc
EXPORTED_PUBLIC const char *
SDirectoryName(const char *path, char *buf, size_t buflen) PURE;
EXPORTED_PUBLIC const char *
SBaseName(const char *path, char *buf, size_t buflen) PURE;
EXPORTED_PUBLIC const char *DirectoryName(const char *path) PURE;
EXPORTED_PUBLIC const char *BaseName(const char *path) PURE;

// Character checks.
EXPORTED_PUBLIC int isspace(int c) NOTHROW;
EXPORTED_PUBLIC int isupper(int c) NOTHROW;
EXPORTED_PUBLIC int islower(int c) NOTHROW;
EXPORTED_PUBLIC int isdigit(int c) NOTHROW;
EXPORTED_PUBLIC int isalpha(int c) NOTHROW;

// Built-in PRNG.
void random_seed(uint64_t seed);
EXPORTED_PUBLIC uint64_t random_next(void);

EXPORTED_PUBLIC char toUpper(char c) PURE;
EXPORTED_PUBLIC char toLower(char c) PURE;
EXPORTED_PUBLIC int max(size_t a, size_t b) PURE;
EXPORTED_PUBLIC int min(size_t a, size_t b) PURE;

// Memory allocation for C code
#if !(UTILITY_LINUX || HOSTED)
EXPORTED_PUBLIC void *malloc(size_t) MALLOC ALLOC_SIZE(1) C_NOTHROW MUST_USE_RESULT;
EXPORTED_PUBLIC void *calloc(size_t, size_t) MALLOC ALLOC_SIZE(1, 2) C_NOTHROW MUST_USE_RESULT;
EXPORTED_PUBLIC void *realloc(void *, size_t) ALLOC_SIZE(2) C_NOTHROW MUST_USE_RESULT;
EXPORTED_PUBLIC void free(void *) C_NOTHROW;
#endif

EXPORTED_PUBLIC size_t nextCharacter(const char *s, size_t i);
EXPORTED_PUBLIC size_t prevCharacter(const char *s, size_t i);

/// Basic 8-bit checksum check (returns 1 if checksum is correct).
uint8_t checksum(const uint8_t *pMemory, size_t sMemory);

/// Fletcher 16-bit checksum.
uint16_t checksum16(const uint16_t *pMemory, size_t sMemory);

/// Fletcher 32-bit checksum.
uint32_t checksum32(const uint32_t *pMemory, size_t sMemory);

/// Fletcher 32-bit checksum.
uint32_t checksum32_naive(const uint32_t *pMemory, size_t sMemory);

/// Checksum a page of memory.
uint32_t checksumPage(uintptr_t address);

/// ELF-style hash.
uint32_t elfHash(const char *buffer, size_t length);

/// Jenkins hash.
uint32_t jenkinsHash(const char *buffer, size_t length);

/// Spooky hash.
uint32_t spookyHash(const char *buffer, size_t length);
uint64_t spookyHash64(const char *buffer, size_t length);
void spookyHash128(const char *buffer, size_t length, uint64_t *h1, uint64_t *h2);

/// Report whether or not two pointers regions overlap.
EXPORTED_PUBLIC int overlaps(const void *s1, const void *s2, size_t n) PURE;

#ifdef __cplusplus
}

// Export C++ support library header.
#include "pedigree/kernel/utilities/cpp.h"
#endif

#endif  // KERNEL_UTILITIES_LIB_H
