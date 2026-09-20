#define PEDIGREE_EXTERNAL_SOURCE 1

#include "pedigree/kernel/core/SlamBitmap.h"

#include <benchmark/benchmark.h>
#include <vector>

static void BM_SlamBitmapFindFreeRun(benchmark::State& state) {
  const size_t pages = static_cast<size_t>(state.range(0));
  const size_t entriesCount = (pages + 63) / 64;
  std::vector<uint64_t> entries(entriesCount * 3);
  SlamBitmap bitmap;
  bitmap.useMemory(entries.data(), entriesCount, pages);
  bitmap.reserve(0, pages / 2);

  for (auto _ : state) benchmark::DoNotOptimize(bitmap.findFreeRun(8));
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}

BENCHMARK(BM_SlamBitmapFindFreeRun)->Range(64, 1 << 20);

struct BenchmarkBitmap {
  explicit BenchmarkBitmap(size_t pages) : entries(((pages + 63) / 64) * 3) {
    bitmap.useMemory(entries.data(), (pages + 63) / 64, pages);
  }

  std::vector<uint64_t> entries;
  SlamBitmap bitmap;
};

static void report(benchmark::State& state) {
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}

static void BM_SlamBitmapFindFreeRunEmpty(benchmark::State& state) {
  BenchmarkBitmap fixture(state.range(0));
  for (auto _ : state) benchmark::DoNotOptimize(fixture.bitmap.findFreeRun(8));
  report(state);
}

static void BM_SlamBitmapFindFreeRunNoFit(benchmark::State& state) {
  BenchmarkBitmap fixture(state.range(0));
  for (size_t page = 0; page < static_cast<size_t>(state.range(0)); page += 2)
    fixture.bitmap.reserve(page, 1);
  for (auto _ : state) benchmark::DoNotOptimize(fixture.bitmap.findFreeRun(8));
  report(state);
}

static void BM_SlamBitmapFindFreeRunFitAtEnd(benchmark::State& state) {
  BenchmarkBitmap fixture(state.range(0));
  fixture.bitmap.reserve(0, state.range(0) - 8);
  for (auto _ : state) benchmark::DoNotOptimize(fixture.bitmap.findFreeRun(8));
  report(state);
}

static void BM_SlamBitmapFindFreeRunRandom(benchmark::State& state) {
  BenchmarkBitmap fixture(state.range(0));
  uint32_t random = 0x12345678;
  for (size_t page = 0; page < static_cast<size_t>(state.range(0)) - 8; ++page) {
    random = random * 1664525 + 1013904223;
    if (random & 1) fixture.bitmap.reserve(page, 1);
  }
  fixture.bitmap.release(state.range(0) - 8, 8);
  for (auto _ : state) benchmark::DoNotOptimize(fixture.bitmap.findFreeRun(8));
  report(state);
}

static void BM_SlamBitmapFindReserveRelease(benchmark::State& state) {
  BenchmarkBitmap fixture(state.range(0));
  fixture.bitmap.reserve(0, state.range(0) - 8);

  for (auto _ : state) {
    const size_t start = fixture.bitmap.findFreeRun(8);
    benchmark::DoNotOptimize(start);
    fixture.bitmap.reserve(start, 8);
    fixture.bitmap.release(start, 8);
  }
  report(state);
}

#define SLAM_BITMAP_BENCHMARK(name) \
  BENCHMARK(name)->RangeMultiplier(8)->Range(64, 1 << 20)

SLAM_BITMAP_BENCHMARK(BM_SlamBitmapFindFreeRunEmpty);
SLAM_BITMAP_BENCHMARK(BM_SlamBitmapFindFreeRunNoFit);
SLAM_BITMAP_BENCHMARK(BM_SlamBitmapFindFreeRunFitAtEnd);
SLAM_BITMAP_BENCHMARK(BM_SlamBitmapFindFreeRunRandom);
SLAM_BITMAP_BENCHMARK(BM_SlamBitmapFindReserveRelease);
