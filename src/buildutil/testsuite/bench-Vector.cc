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

#include <vector>

#include <benchmark/benchmark.h>

#include "pedigree/kernel/utilities/Vector.h"

static void BM_VectorPushBack(benchmark::State &state)
{
    Vector<int64_t> vector;
    const int64_t value = 1;

    while (state.KeepRunning())
    {
        vector.pushBack(value);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_VectorPushBackSTL(benchmark::State &state)
{
    std::vector<int64_t> vector;
    const int64_t value = 1;

    while (state.KeepRunning())
    {
        vector.push_back(value);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_VectorPushFront(benchmark::State &state)
{
    Vector<int64_t> vector;
    const int64_t value = 1;

    while (state.KeepRunning())
    {
        vector.pushFront(value);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_VectorPushFrontSTL(benchmark::State &state)
{
    std::vector<int64_t> vector;
    const int64_t value = 1;

    while (state.KeepRunning())
    {
        vector.insert(vector.begin(), value);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_VectorReservedPushFront(benchmark::State &state)
{
    Vector<int64_t> vector;
    const int64_t value = 1;
    vector.reserve(1024, false);

    while (state.KeepRunning())
    {
        vector.pushFront(value);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_VectorReservedPushBack(benchmark::State &state)
{
    Vector<int64_t> vector;
    const int64_t value = 1;
    vector.reserve(1024, false);

    while (state.KeepRunning())
    {
        vector.pushBack(value);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_VectorPopFront(benchmark::State &state)
{
    Vector<int64_t> vector;
    const int64_t value = 1;

    while (state.KeepRunning())
    {
        state.PauseTiming();
        vector.clear();
        for (size_t i = 0; i < state.range(0); ++i)
        {
            vector.pushBack(value);
        }
        state.ResumeTiming();

        for (size_t i = 0; i < state.range(0); ++i)
        {
            vector.popFront();
        }
    }

    state.SetItemsProcessed(
        int64_t(state.iterations()) * int64_t(state.range(0)));
}

static void BM_VectorPopBack(benchmark::State &state)
{
    Vector<int64_t> vector;
    const int64_t value = 1;

    while (state.KeepRunning())
    {
        state.PauseTiming();
        vector.clear();
        for (size_t i = 0; i < state.range(0); ++i)
        {
            vector.pushBack(value);
        }
        state.ResumeTiming();

        for (size_t i = 0; i < state.range(0); ++i)
        {
            vector.popBack();
        }
    }

    state.SetItemsProcessed(
        int64_t(state.iterations()) * int64_t(state.range(0)));
}

// Vector is *not fast* to do huge amounts of insertions/deletions in, so we
// don't have quite as large a range as, say, List<T>'s tests.
BENCHMARK(BM_VectorPushFront);
BENCHMARK(BM_VectorPushFrontSTL);
BENCHMARK(BM_VectorPushBack);
BENCHMARK(BM_VectorPushBackSTL);
BENCHMARK(BM_VectorReservedPushFront);
BENCHMARK(BM_VectorReservedPushBack);
BENCHMARK(BM_VectorPopFront)->Range(8, 8 << 8);
BENCHMARK(BM_VectorPopBack)->Range(8, 8 << 8);
