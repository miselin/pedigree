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

#include "pedigree/kernel/utilities/ExtensibleBitmap.h"

#include <random>
#include <stdlib.h>
#include <time.h>

#include <benchmark/benchmark.h>

static int RandomNumber(int64_t max) {
  static bool seeded = false;
  if (!seeded) {
    srand(time(0));
    seeded = true;
  }

  // Artificially limit the random number range so we get collisions.
  return rand() % max;
}

static std::vector<size_t> precomputeRandomIndices(int64_t count) {
  std::vector<size_t> indices(count);

  for (auto& i : indices) {
    i = RandomNumber(count);
  }

  return indices;
}

static std::vector<size_t> precomputeShuffledIndices(size_t count) {
  std::vector<size_t> indices(count);
  std::iota(indices.begin(), indices.end(), 0);

  std::mt19937 rng(0x12345678);  // deterministic benchmark
  std::shuffle(indices.begin(), indices.end(), rng);
  return indices;
}

static void BM_ExtensibleBitmapSetLinear(benchmark::State& state) {
  while (state.KeepRunning()) {
    state.PauseTiming();
    // Have to start over to remove any reservations.
    ExtensibleBitmap bitmap;
    state.ResumeTiming();

    for (int i = 0; i < state.range(0); ++i) {
      bitmap.set(i);
    }
  }

  state.SetItemsProcessed(int64_t(state.iterations()) * int64_t(state.range(0)));
  state.SetComplexityN(state.range(0));
}

static void BM_ExtensibleBitmapSetRandomly(benchmark::State& state) {
  auto indices = precomputeRandomIndices(state.range(0));

  for (auto _ : state) {
    state.PauseTiming();
    // Have to start over to remove any reservations.
    ExtensibleBitmap bitmap;
    state.ResumeTiming();

    for (auto i : indices) {
      bitmap.set(i);
    }

    benchmark::ClobberMemory();
  }

  state.SetItemsProcessed(int64_t(state.iterations()) * int64_t(state.range(0)));
}

static void BM_ExtensibleBitmapTestLinear(benchmark::State& state) {
  ExtensibleBitmap bitmap;

  for (ssize_t i = 0; i < state.range(0); ++i) {
    if (i % 2) {
      bitmap.set(i);
    }
  }

  while (state.KeepRunning()) {
    for (int i = 0; i < state.range(0); ++i) {
      benchmark::DoNotOptimize(bitmap.test(i));
    }
  }

  state.SetItemsProcessed(int64_t(state.iterations()) * int64_t(state.range(0)));
  state.SetComplexityN(state.range(0));
}

static void BM_ExtensibleBitmapTestRandomly(benchmark::State& state) {
  auto indices = precomputeRandomIndices(state.range(0));

  ExtensibleBitmap bitmap;

  for (ssize_t i = 0; i < state.range(0); ++i) {
    if (i % 2) {
      bitmap.set(i);
    }
  }

  for (auto _ : state) {
    for (auto i : indices) {
      benchmark::DoNotOptimize(bitmap.test(i));
    }
  }

  state.SetItemsProcessed(int64_t(state.iterations()) * int64_t(state.range(0)));
}

// specifically tries to flush out a hot-path case where the 64-bit skip can't be used
// and we end up with a significant jump in the number of bit tests
static void BM_ExtensibleBitmapSetFirstClearDenseRun(benchmark::State& state) {
  const size_t hole = state.range(0);
  const size_t run = state.range(1);

  ExtensibleBitmap bitmap;

  // Dense prefix including [hole, hole + run).
  for (size_t i = 0; i < hole + run; ++i)
    bitmap.set(i);

  for (auto _ : state) {
    state.PauseTiming();

    // Forces m_nFirstClearBit back to `hole`.
    bitmap.clear(hole);

    state.ResumeTiming();

    // This must discover that hole+1 ... hole+run-1 are already set.
    bitmap.set(hole);

    benchmark::ClobberMemory();
  }

  state.SetItemsProcessed(int64_t(state.iterations()));
}

static void BM_ExtensibleBitmapSetShuffled(benchmark::State& state) {
  auto indices = precomputeShuffledIndices(state.range(0));

  for (auto _ : state) {
    state.PauseTiming();
    ExtensibleBitmap bitmap;
    state.ResumeTiming();

    for (auto i : indices) {
      bitmap.set(i);
    }

    benchmark::ClobberMemory();
  }

  state.SetItemsProcessed(int64_t(state.iterations()) * int64_t(state.range(0)));
}

BENCHMARK(BM_ExtensibleBitmapSetLinear)->Range(8, 8 << 16)->Complexity();
BENCHMARK(BM_ExtensibleBitmapSetRandomly)->Range(8, 8 << 16);
BENCHMARK(BM_ExtensibleBitmapSetShuffled)->Range(8, 8 << 16);
BENCHMARK(BM_ExtensibleBitmapTestLinear)->Range(8, 8 << 16)->Complexity();
BENCHMARK(BM_ExtensibleBitmapTestRandomly)->Range(8, 8 << 16);
BENCHMARK(BM_ExtensibleBitmapSetFirstClearDenseRun)
    ->Args({0, 1})
    ->Args({0, 7})
    ->Args({0, 8})
    ->Args({0, 15})
    ->Args({0, 31})
    ->Args({0, 63})
    ->Args({0, 64})
    ->Args({0, 65})
    ->Args({0, 127});
