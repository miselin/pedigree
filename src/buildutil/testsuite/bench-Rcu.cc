/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#define PEDIGREE_EXTERNAL_SOURCE 1

#include "pedigree/kernel/SpinlockWord.h"
#include "pedigree/kernel/process/RcuReadState.h"

#include <atomic>

#include <benchmark/benchmark.h>

namespace {
const size_t Value = 42;
std::atomic<const size_t*> g_Publication(&Value);
const size_t* g_LockedPublication = &Value;
SpinlockWord g_Lock;
}  // namespace

// Native protocol cost; IRQ masking and kernel scheduling are not included.
static void BM_RcuReadState(benchmark::State& state) {
  RcuReadState reader;
  for (auto _ : state) {
    reader.enter();
    const size_t* value = g_Publication.load(std::memory_order_seq_cst);
    benchmark::DoNotOptimize(*value);
    reader.leave();
  }
  state.SetItemsProcessed(state.iterations());
}

static void BM_RcuSpinlockWord(benchmark::State& state) {
  for (auto _ : state) {
    while (!g_Lock.tryAcquire()) {
    }
    const size_t* value = g_LockedPublication;
    benchmark::DoNotOptimize(value);
    benchmark::DoNotOptimize(*value);
    g_Lock.release();
  }
  state.SetItemsProcessed(state.iterations());
}

static void BM_RcuIdleGrace(benchmark::State& state) {
  RcuReadState readers[4];
  const size_t count = static_cast<size_t>(state.range(0));
  for (auto _ : state) {
    for (size_t cpu = 0; cpu < count; ++cpu) {
      benchmark::DoNotOptimize(readers[cpu].passed(readers[cpu].snapshot()));
    }
  }
  state.SetItemsProcessed(state.iterations());
}

static void BM_RcuCompletedGrace(benchmark::State& state) {
  RcuReadState readers[4];
  RcuReadState::Snapshot before[4];
  const size_t count = static_cast<size_t>(state.range(0));
  for (size_t cpu = 0; cpu < count; ++cpu) {
    readers[cpu].enter();
    before[cpu] = readers[cpu].snapshot();
    readers[cpu].leave();
    readers[cpu].enter();
  }
  for (auto _ : state) {
    for (size_t cpu = 0; cpu < count; ++cpu) {
      benchmark::DoNotOptimize(readers[cpu].passed(before[cpu]));
    }
  }
  for (size_t cpu = 0; cpu < count; ++cpu) {
    readers[cpu].leave();
  }
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_RcuReadState)->Threads(1)->Threads(4)->UseRealTime();
BENCHMARK(BM_RcuSpinlockWord)->Threads(1)->Threads(4)->UseRealTime();
BENCHMARK(BM_RcuIdleGrace)->Arg(1)->Arg(4);
BENCHMARK(BM_RcuCompletedGrace)->Arg(1)->Arg(4);
