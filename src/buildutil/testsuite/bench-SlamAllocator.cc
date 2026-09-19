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

#include <stddef.h>
#include <stdint.h>

#include <vector>

extern "C" size_t slamBenchmarkObjectMinimumSize();
extern "C" uintptr_t slamBenchmarkAllocate(size_t size);
extern "C" void slamBenchmarkFree(uintptr_t object);
extern "C" void slamBenchmarkClearAll();
extern "C" size_t slamBenchmarkRecovery(size_t maxSlabs);

static void BM_SlamAllocatorBackForthReference(benchmark::State& state) {
  while (state.KeepRunning()) {
    void* mem = malloc(slamBenchmarkObjectMinimumSize());
    benchmark::DoNotOptimize(mem);
    free(mem);
  }

  state.SetItemsProcessed(int64_t(state.iterations()));
  state.SetBytesProcessed(int64_t(state.iterations()) * slamBenchmarkObjectMinimumSize());
}

static void BM_SlamAllocatorBackForth(benchmark::State& state) {
  while (state.KeepRunning()) {
    uintptr_t mem = slamBenchmarkAllocate(slamBenchmarkObjectMinimumSize());
    benchmark::DoNotOptimize(mem);
    slamBenchmarkFree(mem);
  }

  state.SetItemsProcessed(int64_t(state.iterations()));
  state.SetBytesProcessed(int64_t(state.iterations()) * slamBenchmarkObjectMinimumSize());
}

BENCHMARK(BM_SlamAllocatorBackForthReference);
BENCHMARK(BM_SlamAllocatorBackForth);

static void BM_SlamAllocatorRecovery(benchmark::State& state) {
  std::vector<uintptr_t> objects(static_cast<size_t>(state.range(0)));

  while (state.KeepRunning()) {
    state.PauseTiming();
    slamBenchmarkClearAll();
    for (uintptr_t& object : objects)
      object = slamBenchmarkAllocate(1);
    for (uintptr_t object : objects)
      slamBenchmarkFree(object);
    state.ResumeTiming();

    benchmark::DoNotOptimize(slamBenchmarkRecovery(1));
  }

  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}

BENCHMARK(BM_SlamAllocatorRecovery)->RangeMultiplier(8)->Range(64, 1 << 14);
