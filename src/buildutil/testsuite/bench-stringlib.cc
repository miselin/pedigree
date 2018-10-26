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

#define PEDIGREE_EXTERNAL_SOURCE 1

#include <string.h>

#include <benchmark/benchmark.h>

#include "pedigree/kernel/utilities/utility.h"

#define CONSTANT_MESSAGE "hello, world! this is a constant string."

static void BM_StringLength(benchmark::State &state)
{
    char *buf = new char[state.range(0)];
    memset(buf, 'a', state.range(0));
    buf[state.range(0) - 1] = '\0';

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(StringLength(buf));
    }

    state.SetBytesProcessed(
        int64_t(state.iterations()) * int64_t(state.range(0)));
    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetComplexityN(state.range(0));

    delete[] buf;
}

static void BM_StringLengthConstant(benchmark::State &state)
{
    while (state.KeepRunning())
    {
        // constexpr here ensures, via compiler check, that we're actually
        // properly testing the compile-time constant version of StringLength
        constexpr size_t result = StringLength(CONSTANT_MESSAGE);
        benchmark::DoNotOptimize(result);
    }

    state.SetBytesProcessed(
        int64_t(state.iterations()) * strlen(CONSTANT_MESSAGE));
    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_StringCopy(benchmark::State &state)
{
    char *buf = new char[state.range(0)];
    memset(buf, 'a', state.range(0));
    buf[state.range(0) - 1] = '\0';
    char *dest = new char[state.range(0)];

    while (state.KeepRunning())
    {
        StringCopy(dest, buf);
    }

    state.SetBytesProcessed(
        int64_t(state.iterations()) * int64_t(state.range(0)));
    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetComplexityN(state.range(0));

    delete[] dest;
    delete[] buf;
}

static void BM_StringCopyN(benchmark::State &state)
{
    char *buf = new char[state.range(0)];
    memset(buf, 'a', state.range(0));
    buf[state.range(0) - 1] = '\0';
    char *dest = new char[state.range(0)];

    while (state.KeepRunning())
    {
        StringCopyN(dest, buf, state.range(0));
    }

    state.SetBytesProcessed(
        int64_t(state.iterations()) * int64_t(state.range(0)));
    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetComplexityN(state.range(0));

    delete[] dest;
    delete[] buf;
}

static void BM_StringCompare(benchmark::State &state)
{
    char *buf1 = new char[state.range(0)];
    memset(buf1, 'a', state.range(0));
    buf1[state.range(0) - 1] = '\0';
    char *buf2 = new char[state.range(0)];
    memset(buf2, 'a', state.range(0));
    buf2[state.range(0) - 1] = '\0';

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(StringCompare(buf1, buf2));
    }

    state.SetBytesProcessed(
        int64_t(state.iterations()) * int64_t(state.range(0)));
    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetComplexityN(state.range(0));

    delete[] buf2;
    delete[] buf1;
}

static void BM_StringCompareN(benchmark::State &state)
{
    char *buf1 = new char[state.range(0)];
    memset(buf1, 'a', state.range(0));
    buf1[state.range(0) - 1] = '\0';
    char *buf2 = new char[state.range(0)];
    memset(buf2, 'a', state.range(0));
    buf2[state.range(0) - 1] = '\0';

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(StringCompareN(buf1, buf2, state.range(0)));
    }

    state.SetBytesProcessed(
        int64_t(state.iterations()) * int64_t(state.range(0)));
    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetComplexityN(state.range(0));

    delete[] buf2;
    delete[] buf1;
}

static void BM_StringMatch(benchmark::State &state)
{
    char *buf1 = new char[state.range(0)];
    memset(buf1, 'a', state.range(0));
    buf1[state.range(0) - 1] = '\0';
    char *buf2 = new char[state.range(0)];
    memset(buf2, 'a', state.range(0));
    buf2[state.range(0) - 1] = '\0';

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(StringMatch(buf1, buf2));
    }

    state.SetBytesProcessed(
        int64_t(state.iterations()) * int64_t(state.range(0)));
    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetComplexityN(state.range(0));

    delete[] buf2;
    delete[] buf1;
}

static void BM_StringMatchN(benchmark::State &state)
{
    char *buf1 = new char[state.range(0)];
    memset(buf1, 'a', state.range(0));
    buf1[state.range(0) - 1] = '\0';
    char *buf2 = new char[state.range(0)];
    memset(buf2, 'a', state.range(0));
    buf2[state.range(0) - 1] = '\0';

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(StringMatchN(buf1, buf2, state.range(0)));
    }

    state.SetBytesProcessed(
        int64_t(state.iterations()) * int64_t(state.range(0)));
    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetComplexityN(state.range(0));

    delete[] buf2;
    delete[] buf1;
}

static void BM_StringCompareCaseSensitive(benchmark::State &state)
{
    char *buf1 = new char[state.range(0)];
    memset(buf1, 'a', state.range(0));
    buf1[state.range(0) - 1] = '\0';
    char *buf2 = new char[state.range(0)];
    memset(buf2, 'a', state.range(0));
    buf2[state.range(0) - 1] = '\0';

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(
            StringCompareCase(buf1, buf2, 1, state.range(0), 0));
    }

    state.SetBytesProcessed(
        int64_t(state.iterations()) * int64_t(state.range(0)));
    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetComplexityN(state.range(0));

    delete[] buf2;
    delete[] buf1;
}

static void BM_StringCompareCaseInsensitive(benchmark::State &state)
{
    char *buf1 = new char[state.range(0)];
    memset(buf1, 'a', state.range(0));
    buf1[state.range(0) - 1] = '\0';
    char *buf2 = new char[state.range(0)];
    memset(buf2, 'a', state.range(0));
    buf2[state.range(0) - 1] = '\0';

    // aAaAaA etc... for truly testing the insensitive case
    for (size_t i = 0; i < state.range(0) - 1; ++i)
    {
        if ((i % 2) == 0)
        {
            buf2[i] = 'A';
        }
    }

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(
            StringCompareCase(buf1, buf2, 0, state.range(0), 0));
    }

    state.SetBytesProcessed(
        int64_t(state.iterations()) * int64_t(state.range(0)));
    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetComplexityN(state.range(0));

    delete[] buf2;
    delete[] buf1;
}

static void BM_StringFind(benchmark::State &state)
{
    char *buf = new char[state.range(0)];
    memset(buf, 'a', state.range(0));
    buf[state.range(0) - 1] = '\0';

    while (state.KeepRunning())
    {
        // Navigates the entire string, finds nothing.
        benchmark::DoNotOptimize(StringFind(buf, 'b'));
    }

    state.SetBytesProcessed(
        int64_t(state.iterations()) * int64_t(state.range(0)));
    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetComplexityN(state.range(0));
}

static void BM_StringReverseFind(benchmark::State &state)
{
    char *buf = new char[state.range(0)];
    memset(buf, 'a', state.range(0));
    buf[state.range(0) - 1] = '\0';

    while (state.KeepRunning())
    {
        // Navigates the entire string, finds nothing.
        benchmark::DoNotOptimize(StringReverseFind(buf, 'b'));
    }

    state.SetBytesProcessed(
        int64_t(state.iterations()) * int64_t(state.range(0)));
    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetComplexityN(state.range(0));
}

static void BM_StringConcat(benchmark::State &state)
{
    char *buf1 = new char[(state.range(0) * 2) + 1];
    char *buf2 = new char[state.range(0)];
    memset(buf1, 'a', state.range(0) * 2);
    memset(buf2, 'a', state.range(0));

    buf1[state.range(0) * 2] = 0;
    buf2[state.range(0)] = 0;

    while (state.KeepRunning())
    {
        buf1[state.range(0)] = 0;
        StringConcat(buf1, buf2);
    }

    state.SetBytesProcessed(
        int64_t(state.iterations()) * int64_t(state.range(0)));
    state.SetItemsProcessed(int64_t(state.iterations()));

    delete [] buf1;
    delete [] buf2;
}

static void BM_StringNextCharacterASCII(benchmark::State &state)
{
    const char *s = "hello";

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(nextCharacter(s, 1));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_StringNextCharacter2byte(benchmark::State &state)
{
    const char *s = "h»b";

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(nextCharacter(s, 1));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_StringNextCharacter3byte(benchmark::State &state)
{
    const char *s = "h€b";

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(nextCharacter(s, 1));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_StringNextCharacter4byte(benchmark::State &state)
{
    const char *s = "h𐍈b";

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(nextCharacter(s, 1));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

BENCHMARK(BM_StringLength)->Range(8, 8 << 16)->Complexity();
BENCHMARK(BM_StringLengthConstant);
BENCHMARK(BM_StringCopy)->Range(8, 8 << 16)->Complexity();
BENCHMARK(BM_StringCopyN)->Range(8, 8 << 16)->Complexity();
BENCHMARK(BM_StringCompare)->Range(8, 8 << 16)->Complexity();
BENCHMARK(BM_StringCompareN)->Range(8, 8 << 16)->Complexity();
BENCHMARK(BM_StringMatch)->Range(8, 8 << 16)->Complexity();
BENCHMARK(BM_StringMatchN)->Range(8, 8 << 16)->Complexity();
//BENCHMARK(BM_StringCompareCaseSensitive)->Range(8, 8 << 16)->Complexity();
//BENCHMARK(BM_StringCompareCaseInsensitive)->Range(8, 8 << 16)->Complexity();
BENCHMARK(BM_StringFind)->Range(8, 8 << 16)->Complexity();
BENCHMARK(BM_StringReverseFind)->Range(8, 8 << 16)->Complexity();
BENCHMARK(BM_StringConcat)->Range(8, 8 << 16);
BENCHMARK(BM_StringNextCharacterASCII);
BENCHMARK(BM_StringNextCharacter2byte);
BENCHMARK(BM_StringNextCharacter3byte);
BENCHMARK(BM_StringNextCharacter4byte);
