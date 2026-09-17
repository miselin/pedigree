#define PEDIGREE_EXTERNAL_SOURCE 1

#include "pedigree/kernel/core/SlamBitmap.h"

#include <benchmark/benchmark.h>
#include <vector>

static void BM_SlamBitmapFindFreeRun(benchmark::State& state) {
  const size_t pages = static_cast<size_t>(state.range(0));
  const size_t entriesCount = (pages + 63) / 64;
  std::vector<SlamBitmap::Entry> entries(entriesCount);
  SlamBitmap bitmap(entries.data(), entries.size(), pages);
  bitmap.reserve(0, pages / 2);

  for (auto _ : state) benchmark::DoNotOptimize(bitmap.findFreeRun(8));
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}

BENCHMARK(BM_SlamBitmapFindFreeRun)->Range(64, 1 << 20);
