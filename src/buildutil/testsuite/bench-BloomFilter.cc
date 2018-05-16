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

#include "pedigree/kernel/utilities/BloomFilter.h"

static void BM_BloomFilterContains(benchmark::State &state)
{
    BloomFilter<char> filter(state.range(0), state.range(1));

    filter.add('t');
    filter.add("test1", 5);
    filter.add("test2", 5);
    filter.add("hello world", 11);

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(filter.contains('t'));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_BloomFilterDoesNotContain(benchmark::State &state)
{
    BloomFilter<char> filter(state.range(0), state.range(1));

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(filter.contains('t'));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

BENCHMARK(BM_BloomFilterContains)->Ranges({{1, 1024}, {1, 8}});
BENCHMARK(BM_BloomFilterDoesNotContain)->Ranges({{1, 1024}, {1, 8}});