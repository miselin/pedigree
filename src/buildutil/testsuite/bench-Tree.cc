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

#include <limits.h>
#include <stdlib.h>
#include <time.h>

#include <iostream>
#include <map>

#include <benchmark/benchmark.h>

#include "pedigree/kernel/utilities/BloomFilter.h"
#include "pedigree/kernel/utilities/Tree.h"

#define RANDOM_MAX 0x10000

template class Tree<int64_t, int64_t>;

static const int RandomNumber()
{
    static bool seeded = false;
    if (!seeded)
    {
        srand(time(0));
        seeded = true;
    }

    // Artificially limit the random number range so we get collisions.
    return rand() % RANDOM_MAX;
}

static void BM_TreeInsert(benchmark::State &state)
{
    const int64_t value = 1;

    while (state.KeepRunning())
    {
        Tree<int64_t, int64_t> tree;
        for (int64_t i = 0; i < state.range(0); ++i)
        {
            tree.insert(i, value);
        }
    }

    state.SetItemsProcessed(
        int64_t(state.iterations()) * int64_t(state.range(0)));
    state.SetComplexityN(state.range(0));
}

static void BM_TreeInsertReverse(benchmark::State &state)
{
    const int64_t value = 1;

    while (state.KeepRunning())
    {
        Tree<int64_t, int64_t> tree;
        for (int64_t i = state.range(0); i >= 0; --i)
        {
            tree.insert(i, value);
        }
    }

    state.SetItemsProcessed(
        int64_t(state.iterations()) * int64_t(state.range(0)));
    state.SetComplexityN(state.range(0));
}

static void BM_TreeLookupSingle(benchmark::State &state)
{
    Tree<int64_t, int64_t> tree;

    for (size_t i = 0; i < state.range(0); ++i)
    {
        tree.insert(i, RandomNumber());
    }

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(tree.lookup(0));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_TreeLookup(benchmark::State &state)
{
    Tree<int64_t, int64_t> tree;

    for (size_t i = 0; i < state.range(0); ++i)
    {
        tree.insert(RandomNumber(), RandomNumber());
    }

    int64_t a = 0;
    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(tree.lookup(a++ % state.range(0)));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetComplexityN(state.range(0));
}

static void BM_TreeLookupWithFilter(benchmark::State &state)
{
    Tree<int64_t, int64_t> tree;

    size_t n = state.range(0);
    // ln(0.001) - where p (failure rate) = 0.001
    // ln(2) ~= 0.6931
    size_t m = -((n * -6.9078) / (0.6931 * 0.6931));
    size_t k = (m / n) * 0.6931;

    BloomFilter<int64_t> filter(m, k);

    for (size_t i = 0; i < state.range(0); ++i)
    {
        int64_t key = static_cast<int64_t>(i);
        tree.insert(key, RandomNumber());
        filter.add(key);
    }

    int64_t a = 0;
    while (state.KeepRunning())
    {
        // 100% hit rate (all keys exist)
        int64_t key = a++ % state.range(0);
        if (filter.contains(key))
        {
            benchmark::DoNotOptimize(tree.lookup(key));
        }
        else
        {
            std::cerr << "Bloom filter did not contain key " << key
                      << " [range=" << state.range(0) << "]" << std::endl;
            abort();
        }
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetComplexityN(state.range(0));
}

static void BM_TreeFailedLookupWithFilter(benchmark::State &state)
{
    /// \todo rethink this
    Tree<int64_t, int64_t> tree;

    size_t n = state.range(0);
    // ln(0.001) - where p (failure rate) = 0.001
    // ln(2) ~= 0.6931
    size_t m = -((n * -6.9078) / (0.6931 * 0.6931));
    size_t k = (m / n) * 0.6931;

    BloomFilter<int64_t> filter(m, k);

    for (size_t i = 0; i < state.range(0); ++i)
    {
        tree.insert(i, RandomNumber());
        filter.add(i);
    }

    int64_t a = 0;
    while (state.KeepRunning())
    {
        // 100% hit rate (all keys exist)
        if (filter.contains(-1))
        {
            benchmark::DoNotOptimize(tree.lookup(-1));
        }
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetComplexityN(state.range(0));
}

static void BM_TreeLookupDoesNotExist(benchmark::State &state)
{
    /// \todo rethink this
    Tree<int64_t, int64_t> tree;

    for (size_t i = 0; i < state.range(0); ++i)
    {
        tree.insert(i, RandomNumber());
    }

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(tree.lookup(-1));
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

BENCHMARK(BM_TreeInsert)->Range(4, 1 << 18)->Complexity();
BENCHMARK(BM_TreeLookup)->Range(4, 1 << 18)->Complexity();
BENCHMARK(BM_TreeInsertReverse)->Range(4, 1 << 18)->Complexity();
BENCHMARK(BM_TreeLookupSingle)->Range(4, 1 << 18)->Complexity();
BENCHMARK(BM_TreeLookupDoesNotExist)->Range(4, 1 << 18);

BENCHMARK(BM_TreeLookupWithFilter)->Range(4, 1 << 18)->Complexity();
BENCHMARK(BM_TreeFailedLookupWithFilter)->Range(4, 1 << 18)->Complexity();
