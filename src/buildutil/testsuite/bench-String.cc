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

#include "pedigree/kernel/utilities/String.h"

static void BM_CxxStringCreation(benchmark::State &state)
{
    while (state.KeepRunning())
    {
        String s;
        benchmark::DoNotOptimize(s);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_CxxStringCreationConstexpr(benchmark::State &state)
{
    while (state.KeepRunning())
    {
        String s("Hello, world!");
        benchmark::DoNotOptimize(s);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_CxxStringCopyToStatic(benchmark::State &state)
{
    const char *assign = "Hello, world!";

    while (state.KeepRunning())
    {
        String s(assign);
        benchmark::DoNotOptimize(s);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetBytesProcessed(
        int64_t(state.iterations()) * int64_t(strlen(assign)));
}

static void BM_CxxStringCopyToDynamic(benchmark::State &state)
{
    char assign[128];
    memset(assign, 'a', 128);
    assign[127] = 0;

    while (state.KeepRunning())
    {
        String s(assign);
        benchmark::DoNotOptimize(s);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetBytesProcessed(int64_t(state.iterations()) * 128);
}

static void BM_CxxStringCopyLength(benchmark::State &state)
{
    char assign[128];
    memset(assign, 'a', 128);
    assign[127] = 0;

    while (state.KeepRunning())
    {
        String s(assign, 128);
        benchmark::DoNotOptimize(s);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetBytesProcessed(int64_t(state.iterations()) * 128);
}

static void BM_CxxStringFormat(benchmark::State &state)
{
    while (state.KeepRunning())
    {
        String s;
        s.Format("Hello, %s!", "world");
        benchmark::DoNotOptimize(s);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_CxxStringStartswithBestCase(benchmark::State &state)
{
    String s("hello, world!");

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(s.startswith("hello"));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetComplexityN(s.length());
}

static void BM_CxxStringStartswithWorstCase(benchmark::State &state)
{
    char buf[state.range(0)];
    memset(buf, 'a', state.range(0));

    String s(buf, state.range(0));

    while (state.KeepRunning())
    {
        // not in string
        benchmark::DoNotOptimize(s.startswith("goodbye"));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetComplexityN(s.length());
}

static void BM_CxxStringEndswith(benchmark::State &state)
{
    String tail("hello, world!");

    char buf[state.range(0)];
    memset(buf, 'a', state.range(0));
    strncpy(
        buf + (state.range(0) - tail.length()), static_cast<const char *>(tail),
        tail.length());

    String s(buf, state.range(0));

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(s.endswith("world!"));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetComplexityN(s.length());
}

static void BM_CxxStringStrip(benchmark::State &state)
{
    char buf[state.range(0)];
    memset(buf, ' ', state.range(0));
    buf[state.range(0) / 2] = 'a';
    buf[state.range(0) - 1] = 0;

    String s;
    while (state.KeepRunning())
    {
        s.assign(buf, state.range(0));
        s.strip();
        benchmark::DoNotOptimize(s);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
    // cater for the string assignment which is also O(N)
    state.SetComplexityN(state.range(0) * 2);
}

static void BM_CxxStringLStrip(benchmark::State &state)
{
    char buf[state.range(0)];
    memset(buf, ' ', state.range(0));
    buf[state.range(0) - 2] = 'a';
    buf[state.range(0) - 1] = 0;

    String s;
    while (state.KeepRunning())
    {
        s.assign(buf, state.range(0));
        s.lstrip();
        benchmark::DoNotOptimize(s);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
    // cater for the string assignment which is also O(N)
    state.SetComplexityN(state.range(0) * 2);
}

static void BM_CxxStringRStrip(benchmark::State &state)
{
    char buf[state.range(0)];
    memset(buf, ' ', state.range(0));
    buf[0] = 'a';
    buf[state.range(0) - 1] = 0;

    String s;
    while (state.KeepRunning())
    {
        s.assign(buf, state.range(0));
        s.rstrip();
        benchmark::DoNotOptimize(s);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
    // cater for the string assignment which is also O(N)
    state.SetComplexityN(state.range(0) * 2);
}

static void BM_CxxStringSplit(benchmark::State &state)
{
    char *buf = new char[state.range(0)];
    memset(buf, 'a', state.range(0));
    buf[state.range(0)] = 0;

    String s;
    while (state.KeepRunning())
    {
        s.assign(buf, state.range(0));
        benchmark::DoNotOptimize(s.split(state.range(0) / 2));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
    // cater for the string assignment which is also O(N)
    state.SetComplexityN(state.range(0) * 2);
}

static void BM_CxxStringSplitRef(benchmark::State &state)
{
    char *buf = new char[state.range(0)];
    memset(buf, 'a', state.range(0));
    buf[state.range(0)] = 0;

    String s, target;
    while (state.KeepRunning())
    {
        s.assign(buf, state.range(0));
        s.split(state.range(0) / 2, target);
        benchmark::DoNotOptimize(s);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
    // cater for the string assignment which is also O(N)
    state.SetComplexityN(state.range(0) * 2);
}

static void BM_CxxStringTokenize(benchmark::State &state)
{
    char buf[state.range(0)];
    memset(buf, 0, state.range(0));
    for (size_t i = 0; i < state.range(0) - 1; ++i)
    {
        if (i % 2)
        {
            buf[i] = ' ';
        }
        else
        {
            buf[i] = 'a';
        }
    }

    String s;
    while (state.KeepRunning())
    {
        s.assign(buf, state.range(0));
        benchmark::DoNotOptimize(s.tokenise(' '));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetComplexityN(state.range(0));
}

static void BM_CxxStringTokenizeRef(benchmark::State &state)
{
    char buf[state.range(0)];
    memset(buf, 0, state.range(0));
    for (size_t i = 0; i < state.range(0) - 1; ++i)
    {
        if (i % 2)
        {
            buf[i] = ' ';
        }
        else
        {
            buf[i] = 'a';
        }
    }

    String s(buf, state.range(0));
    while (state.KeepRunning())
    {
        Vector<String> tokens;
        s.tokenise(' ', tokens);
        benchmark::DoNotOptimize(s);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetComplexityN(state.range(0));
}

static void BM_CxxStringTokenizeViews(benchmark::State &state)
{
    char buf[state.range(0)];
    memset(buf, 0, state.range(0));
    for (size_t i = 0; i < state.range(0) - 1; ++i)
    {
        if (i % 2)
        {
            buf[i] = ' ';
        }
        else
        {
            buf[i] = 'a';
        }
    }

    String s(buf, state.range(0));
    while (state.KeepRunning())
    {
        Vector<StringView> tokens;
        s.tokenise(' ', tokens);
        benchmark::DoNotOptimize(s);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetComplexityN(state.range(0));
}

static void BM_CxxStringCompareBestCase(benchmark::State &state)
{
    char left[state.range(0)];
    char right[state.range(0)];
    memset(left, 'a', state.range(0));
    memset(right, 'a', state.range(0));
    right[0] = 'b';  // very early fail

    String l(left, state.range(0)), r(right, state.range(0));
    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(l == r);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetComplexityN(state.range(0));
}

static void BM_CxxStringCompareAverageCase(benchmark::State &state)
{
    char left[state.range(0)];
    char right[state.range(0)];
    memset(left, 'a', state.range(0));
    memset(right, 'a', state.range(0));
    right[state.range(0) / 2] = 'b';  // middle range fail

    String l(left, state.range(0)), r(right, state.range(0));
    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(l == r);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetComplexityN(state.range(0));
}

static void BM_CxxStringCompareWorstCase(benchmark::State &state)
{
    char left[state.range(0)];
    char right[state.range(0)];
    memset(left, 'a', state.range(0));
    memset(right, 'a', state.range(0));
    right[state.range(0) - 1] = 'b';  // end of range fail

    String l(left, state.range(0)), r(right, state.range(0));
    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(l == r);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetComplexityN(state.range(0));
}

static void BM_CxxStringCompareRawBestCase(benchmark::State &state)
{
    char left[state.range(0)];
    char right[state.range(0)];
    memset(left, 'a', state.range(0));
    memset(right, 'a', state.range(0));
    right[0] = 'b';  // very early fail

    String l(left, state.range(0));
    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(l == right);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetComplexityN(state.range(0));
}

static void BM_CxxStringCompareRawAverageCase(benchmark::State &state)
{
    char left[state.range(0)];
    char right[state.range(0)];
    memset(left, 'a', state.range(0));
    memset(right, 'a', state.range(0));
    right[state.range(0) / 2] = 'b';  // middle range fail

    String l(left, state.range(0));
    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(l == right);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetComplexityN(state.range(0));
}

static void BM_CxxStringCompareRawWorstCase(benchmark::State &state)
{
    char left[state.range(0)];
    char right[state.range(0)];
    memset(left, 'a', state.range(0));
    memset(right, 'a', state.range(0));
    right[state.range(0) - 1] = 'b';  // end of range fail

    String l(left, state.range(0)), r(right, state.range(0));
    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(l == right);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetComplexityN(state.range(0));
}

BENCHMARK(BM_CxxStringCreation);
BENCHMARK(BM_CxxStringCreationConstexpr);
BENCHMARK(BM_CxxStringCopyToStatic);
BENCHMARK(BM_CxxStringCopyToDynamic);
BENCHMARK(BM_CxxStringCopyLength);
BENCHMARK(BM_CxxStringFormat);
BENCHMARK(BM_CxxStringStartswithBestCase);
BENCHMARK(BM_CxxStringStartswithWorstCase)->Range(8, 4096)->Complexity();
BENCHMARK(BM_CxxStringEndswith)->Range(16, 4096)->Complexity();
BENCHMARK(BM_CxxStringStrip)->Range(8, 4096)->Complexity();
BENCHMARK(BM_CxxStringLStrip)->Range(8, 4096)->Complexity();
BENCHMARK(BM_CxxStringRStrip)->Range(8, 4096)->Complexity();
BENCHMARK(BM_CxxStringSplit)->Range(8, 4096)->Complexity();
BENCHMARK(BM_CxxStringSplitRef)->Range(8, 4096)->Complexity();
BENCHMARK(BM_CxxStringTokenize)->Range(8, 4096)->Complexity();
BENCHMARK(BM_CxxStringTokenizeRef)->Range(8, 4096)->Complexity();
BENCHMARK(BM_CxxStringTokenizeViews)->Range(8, 4096)->Complexity();
BENCHMARK(BM_CxxStringCompareBestCase)->Range(8, 4096)->Complexity();
BENCHMARK(BM_CxxStringCompareAverageCase)->Range(8, 4096)->Complexity();
BENCHMARK(BM_CxxStringCompareWorstCase)->Range(8, 4096)->Complexity();
BENCHMARK(BM_CxxStringCompareRawBestCase)->Range(8, 4096)->Complexity();
BENCHMARK(BM_CxxStringCompareRawAverageCase)->Range(8, 4096)->Complexity();
BENCHMARK(BM_CxxStringCompareRawWorstCase)->Range(8, 4096)->Complexity();
