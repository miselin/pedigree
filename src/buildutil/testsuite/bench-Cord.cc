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

#include "pedigree/kernel/utilities/Cord.h"
#include "pedigree/kernel/utilities/StaticCord.h"

static void BM_CordIndexing(benchmark::State &state)
{
    Cord str;
    str.append("he", 2);
    str.append("ll", 2);
    str.append("o ", 2);
    str.append("wo", 2);
    str.append("rl", 2);
    str.append("d!", 2);

    uint64_t i = 0;
    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(str[i++ % str.length()]);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_CordIteration(benchmark::State &state)
{
    Cord str;
    str.append("he", 2);
    str.append("ll", 2);
    str.append("o ", 2);
    str.append("wo", 2);
    str.append("rl", 2);
    str.append("d!", 2);

    uint64_t i = 0;
    while (state.KeepRunning())
    {
        for (auto it : str)
        {
            benchmark::DoNotOptimize(it);
        }
    }

    state.SetItemsProcessed(int64_t(state.iterations() * str.length()));
}

static void BM_StaticCordIndexing(benchmark::State &state)
{
    StaticCord<8> str;
    str.append("he", 2);
    str.append("ll", 2);
    str.append("o ", 2);
    str.append("wo", 2);
    str.append("rl", 2);
    str.append("d!", 2);

    uint64_t i = 0;
    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(str[i++ % str.length()]);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_StaticCordIteration(benchmark::State &state)
{
    StaticCord<8> str;
    str.append("he", 2);
    str.append("ll", 2);
    str.append("o ", 2);
    str.append("wo", 2);
    str.append("rl", 2);
    str.append("d!", 2);

    uint64_t i = 0;
    while (state.KeepRunning())
    {
        for (auto it : str)
        {
            benchmark::DoNotOptimize(it);
        }
    }

    state.SetItemsProcessed(int64_t(state.iterations() * str.length()));
}

BENCHMARK(BM_CordIndexing);
BENCHMARK(BM_CordIteration);
BENCHMARK(BM_StaticCordIndexing);
BENCHMARK(BM_StaticCordIteration);
