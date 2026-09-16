/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include "pedigree/kernel/utilities/RingBuffer.h"

#include <deque>
#include <vector>

#include <benchmark/benchmark.h>

static void BM_RingBufferWriteRead(benchmark::State& state) {
  const size_t chunkSize = static_cast<size_t>(state.range(0));
  std::vector<char> input(chunkSize, 'x');
  std::vector<char> output(chunkSize);
  RingBuffer<char, 65536> buffer(65536);

  while (state.KeepRunning()) {
    size_t written = buffer.write(input.data(), input.size());
    size_t read = buffer.read(output.data(), output.size());
    benchmark::DoNotOptimize(written);
    benchmark::DoNotOptimize(read);
  }

  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * 2);
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(chunkSize) * 2);
}

static void BM_RingBufferWriteReadStdDeque(benchmark::State& state) {
  const size_t chunkSize = static_cast<size_t>(state.range(0));
  std::vector<char> input(chunkSize, 'x');
  std::vector<char> output(chunkSize);
  std::deque<char> buffer;

  while (state.KeepRunning()) {
    buffer.insert(buffer.end(), input.begin(), input.end());
    for (size_t i = 0; i < chunkSize; ++i) {
      output[i] = buffer.front();
      buffer.pop_front();
    }
    benchmark::DoNotOptimize(output.data());
  }

  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * 2);
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(chunkSize) * 2);
}

BENCHMARK(BM_RingBufferWriteRead)->RangeMultiplier(4)->Range(1, 4096);
BENCHMARK(BM_RingBufferWriteReadStdDeque)->RangeMultiplier(4)->Range(1, 4096);
