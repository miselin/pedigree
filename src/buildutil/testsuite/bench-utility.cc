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

#include "pedigree/kernel/utilities/smhasher/MurmurHash3.h"
#include "pedigree/kernel/utilities/utility.h"

static void BM_Utility_Checksum(benchmark::State &state)
{
    auto *buf = new uint8_t[state.range(0)];
    memset(buf, 'a', state.range(0));

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(checksum(buf, state.range(0)));
    }

    state.SetBytesProcessed(
        int64_t(state.iterations()) * int64_t(state.range(0)));

    delete[] buf;
}

static void BM_Utility_Checksum16(benchmark::State &state)
{
    auto *buf = new uint8_t[state.range(0)];
    memset(buf, 'a', state.range(0));

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(checksum16(buf, state.range(0)));
    }

    state.SetBytesProcessed(
        int64_t(state.iterations()) * int64_t(state.range(0)));

    delete[] buf;
}

static void BM_Utility_Checksum32(benchmark::State &state)
{
    auto *buf = new uint8_t[state.range(0)];
    memset(buf, 'a', state.range(0));

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(checksum32(buf, state.range(0)));
    }

    state.SetBytesProcessed(
        int64_t(state.iterations()) * int64_t(state.range(0)));

    delete[] buf;
}
static void BM_Utility_Checksum32Naive(benchmark::State &state)
{
    auto *buf = new uint8_t[state.range(0)];
    memset(buf, 'a', state.range(0));

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(checksum32_naive(buf, state.range(0)));
    }

    state.SetBytesProcessed(
        int64_t(state.iterations()) * int64_t(state.range(0)));

    delete[] buf;
}

static void BM_Utility_ChecksumPage(benchmark::State &state)
{
    auto *buf = new uint8_t[4096];
    memset(buf, 'a', 4096);

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(
            checksumPage(reinterpret_cast<uintptr_t>(buf)));
    }

    state.SetBytesProcessed(int64_t(state.iterations()) * 4096);

    delete[] buf;
}

static void BM_Utility_HashElf(benchmark::State &state)
{
    auto *buf = new char[state.range(0)];
    memset(buf, 'a', state.range(0));

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(elfHash(buf, state.range(0)));
    }

    state.SetBytesProcessed(
        int64_t(state.iterations()) * int64_t(state.range(0)));

    delete[] buf;
}

static void BM_Utility_HashJenkins(benchmark::State &state)
{
    auto *buf = new char[state.range(0)];
    memset(buf, 'a', state.range(0));

    while (state.KeepRunning())
    {
        benchmark::DoNotOptimize(jenkinsHash(buf, state.range(0)));
    }

    state.SetBytesProcessed(
        int64_t(state.iterations()) * int64_t(state.range(0)));

    delete[] buf;
}

static void BM_Utility_HashMurmur(benchmark::State &state)
{
    auto *buf = new char[state.range(0)];
    memset(buf, 'a', state.range(0));

    while (state.KeepRunning())
    {
        uint64_t result[2];
        MurmurHash3_x64_128(buf, state.range(0), 0, result);
        benchmark::DoNotOptimize(result);
    }

    state.SetBytesProcessed(
        int64_t(state.iterations()) * int64_t(state.range(0)));

    delete[] buf;
}

// Test checksum over a large range of sizes.
BENCHMARK(BM_Utility_Checksum)->Range(8, 8 << 24);
BENCHMARK(BM_Utility_Checksum16)->Range(8, 8 << 24);
BENCHMARK(BM_Utility_Checksum32)->Range(8, 8 << 24);
BENCHMARK(BM_Utility_Checksum32Naive)->Range(8, 8 << 24);

BENCHMARK(BM_Utility_ChecksumPage);

BENCHMARK(BM_Utility_HashElf)->Range(8, 8 << 24);
BENCHMARK(BM_Utility_HashJenkins)->Range(8, 8 << 24);
BENCHMARK(BM_Utility_HashMurmur)->Range(8, 8 << 24);
