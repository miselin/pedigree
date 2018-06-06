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

#include <benchmark/benchmark.h>

#include "pedigree/kernel/utilities/LruCache.h"

static void BM_LruCacheNoChurn(benchmark::State &state)
{
    LruCache<int, int> cache;

    for (int i = 0; i < 32; ++i)
    {
        cache.store(i, i);
    }

    while (state.KeepRunning())
    {
        for (int i = 0; i < 32; ++i)
        {
            int x;
            benchmark::DoNotOptimize(cache.get(i, x));
        }
    }

    state.SetItemsProcessed(int64_t(state.iterations() * 32));
}

static void BM_LruCacheChurning(benchmark::State &state)
{
    LruCache<int, int> cache;

    for (int i = 0; i < 32; ++i)
    {
        cache.store(i, i);
    }

    while (state.KeepRunning())
    {
        for (int i = 0; i < 32; ++i)
        {
            int x;
            benchmark::DoNotOptimize(cache.get(i, x));
            cache.store(i, i);  // pushes this one to the front
        }
    }

    state.SetItemsProcessed(int64_t(state.iterations() * 32));
}

static void BM_LruCacheRotating(benchmark::State &state)
{
    LruCache<int, int> cache;

    // intentionally exceeding capacity in this test
    for (int i = 0; i < 64; ++i)
    {
        cache.store(i, i);
    }

    while (state.KeepRunning())
    {
        for (int i = 0; i < 64; ++i)
        {
            int x;
            benchmark::DoNotOptimize(cache.get(i, x));
            cache.store(i, i);  // pushes this one to the front
        }
    }

    state.SetItemsProcessed(int64_t(state.iterations() * 64));
}

static void BM_LruCacheAllMisses(benchmark::State &state)
{
    LruCache<int, int> cache;

    while (state.KeepRunning())
    {
        int x;
        benchmark::DoNotOptimize(cache.get(5, x));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

BENCHMARK(BM_LruCacheNoChurn);
BENCHMARK(BM_LruCacheChurning);
BENCHMARK(BM_LruCacheRotating);
BENCHMARK(BM_LruCacheAllMisses);
