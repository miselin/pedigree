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

#include "pedigree/kernel/utilities/IntrusiveList.h"

#include <vector>

#include <benchmark/benchmark.h>

struct IntrusiveListValue {
  int64_t value = 1;
  IntrusiveListNode<IntrusiveListValue> listNode;
};

using BenchList = IntrusiveList<IntrusiveListValue, &IntrusiveListValue::listNode>;

static void BM_IntrusiveListPushBack(benchmark::State& state) {
  std::vector<IntrusiveListValue> values(state.range(0));
  BenchList list;

  for (auto _ : state) {
    for (auto& value : values)
      list.pushBack(value);
    benchmark::DoNotOptimize(list);

    state.PauseTiming();
    list.clear();
    state.ResumeTiming();
  }

  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_IntrusiveListPushFront(benchmark::State& state) {
  std::vector<IntrusiveListValue> values(state.range(0));
  BenchList list;

  for (auto _ : state) {
    for (auto& value : values)
      list.pushFront(value);
    benchmark::DoNotOptimize(list);

    state.PauseTiming();
    list.clear();
    state.ResumeTiming();
  }

  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_IntrusiveListPushPop(benchmark::State& state) {
  IntrusiveListValue value;
  BenchList list;

  for (auto _ : state) {
    list.pushFront(value);
    benchmark::DoNotOptimize(list.popFront());
  }

  state.SetItemsProcessed(state.iterations());
}

static void BM_IntrusiveListPopFront(benchmark::State& state) {
  std::vector<IntrusiveListValue> values(state.range(0));
  BenchList list;

  for (auto _ : state) {
    state.PauseTiming();
    for (auto& value : values)
      list.pushFront(value);
    state.ResumeTiming();

    for (size_t i = 0; i < values.size(); ++i)
      benchmark::DoNotOptimize(list.popFront());
  }

  state.SetItemsProcessed(state.iterations() * state.range(0));
}

static void BM_IntrusiveListPopBack(benchmark::State& state) {
  std::vector<IntrusiveListValue> values(state.range(0));
  BenchList list;

  for (auto _ : state) {
    state.PauseTiming();
    for (auto& value : values)
      list.pushFront(value);
    state.ResumeTiming();

    for (size_t i = 0; i < values.size(); ++i)
      benchmark::DoNotOptimize(list.popBack());
  }

  state.SetItemsProcessed(state.iterations() * state.range(0));
}

BENCHMARK(BM_IntrusiveListPushFront)->Range(8, 8 << 16);
BENCHMARK(BM_IntrusiveListPushBack)->Range(8, 8 << 16);
BENCHMARK(BM_IntrusiveListPushPop);
BENCHMARK(BM_IntrusiveListPopFront)->Range(8, 8 << 16);
BENCHMARK(BM_IntrusiveListPopBack)->Range(8, 8 << 16);
