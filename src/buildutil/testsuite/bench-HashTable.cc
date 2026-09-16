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

#include "pedigree/kernel/utilities/HashTable.h"

#include <algorithm>
#include <random>
#include <string.h>
#include <vector>

#include <benchmark/benchmark.h>
#include <unordered_map>

class HashedInteger {
 public:
  HashedInteger() : n_(-1) {}

  HashedInteger(int n) : n_(n) {}

  uint32_t hash() const {
    return n_;
  }

  bool operator==(const HashedInteger& other) const {
    return n_ == other.n_;
  }

  bool operator!=(const HashedInteger& other) const {
    return n_ != other.n_;
  }

 private:
  int64_t n_;
};

class CollidingHashedInteger {
 public:
  CollidingHashedInteger() : n_(-1), modulus_(0xFFFFFFFFU) {}

  CollidingHashedInteger(int modulus, int n) : n_(n), modulus_(modulus) {}

  uint32_t hash() const {
    return n_ % modulus_;
  }

  bool operator==(const CollidingHashedInteger& other) const {
    return n_ == other.n_;
  }

  bool operator!=(const CollidingHashedInteger& other) const {
    return n_ != other.n_;
  }

 private:
  int64_t n_;
  int64_t modulus_;
};

static void BM_HashTableInsertPreallocate(benchmark::State& state) {
  int64_t value = 1;

  HashTable<HashedInteger, int64_t, int64_t, 32768> table;
  while (state.KeepRunning()) {
    state.PauseTiming();
    table.clear();
    state.ResumeTiming();

    for (ssize_t i = 0; i < state.range(0); ++i) {
      HashedInteger key(i);
      table.insert(key, value);
    }
  }

  state.SetItemsProcessed(int64_t(state.iterations()) * int64_t(state.range(0)));
}

static void BM_HashTableInsertNoChains(benchmark::State& state) {
  int64_t value = 1;

  HashTable<HashedInteger, int64_t> table;
  while (state.KeepRunning()) {
    state.PauseTiming();
    table.clear();
    state.ResumeTiming();

    for (ssize_t i = 0; i < state.range(0); ++i) {
      HashedInteger key(i);
      table.insert(key, value);
    }
  }

  state.SetItemsProcessed(int64_t(state.iterations()) * int64_t(state.range(0)));
}

static void BM_HashTableInsertNoChainsReserved(benchmark::State& state) {
  int64_t value = 1;

  HashTable<HashedInteger, int64_t> table;
  while (state.KeepRunning()) {
    state.PauseTiming();
    table.clear();
    table.reserve(state.range(0));
    state.ResumeTiming();

    for (ssize_t i = 0; i < state.range(0); ++i) {
      HashedInteger key(i);
      table.insert(key, value);
    }
  }

  state.SetItemsProcessed(int64_t(state.iterations()) * int64_t(state.range(0)));
}

static void BM_HashTableInsertNoChainsLinear(benchmark::State& state) {
  int64_t value = 1;

  HashTable<HashedInteger, int64_t, int64_t, 4, false> table;
  while (state.KeepRunning()) {
    state.PauseTiming();
    table.clear();
    state.ResumeTiming();

    for (ssize_t i = 0; i < state.range(0); ++i) {
      HashedInteger key(i);
      table.insert(key, value);
    }
  }

  state.SetItemsProcessed(int64_t(state.iterations()) * int64_t(state.range(0)));
}

static void BM_HashTableInsertNoChainsReservedLinear(benchmark::State& state) {
  int64_t value = 1;

  HashTable<HashedInteger, int64_t, int64_t, 4, false> table;
  while (state.KeepRunning()) {
    state.PauseTiming();
    table.clear();
    table.reserve(state.range(0));
    state.ResumeTiming();

    for (ssize_t i = 0; i < state.range(0); ++i) {
      HashedInteger key(i);
      table.insert(key, value);
    }
  }

  state.SetItemsProcessed(int64_t(state.iterations()) * int64_t(state.range(0)));
}

static void BM_HashTableLookupNoChains(benchmark::State& state) {
  HashTable<HashedInteger, int64_t> table;
  int64_t value = 1;
  for (ssize_t i = 0; i < state.range(0); ++i) {
    HashedInteger key(i);
    table.insert(key, value);
  }

  while (state.KeepRunning()) {
    for (ssize_t i = 0; i < state.range(0); ++i) {
      HashedInteger key(i);
      benchmark::DoNotOptimize(table.lookup(key));
    }
  }

  state.SetItemsProcessed(int64_t(state.iterations()) * int64_t(state.range(0)));
}

static void BM_HashTableLookupNoChainsLinear(benchmark::State& state) {
  HashTable<HashedInteger, int64_t, int64_t, 4, false> table;
  int64_t value = 1;
  for (ssize_t i = 0; i < state.range(0); ++i) {
    HashedInteger key(i);
    table.insert(key, value);
  }

  while (state.KeepRunning()) {
    for (ssize_t i = 0; i < state.range(0); ++i) {
      HashedInteger key(i);
      benchmark::DoNotOptimize(table.lookup(key));
    }
  }

  state.SetItemsProcessed(int64_t(state.iterations()) * int64_t(state.range(0)));
}

static void BM_HashTableLookupStdUnorderedMap(benchmark::State& state) {
  std::vector<int64_t> keys;
  keys.reserve(state.range(0));
  for (int64_t i = 0; i < state.range(0); ++i) {
    keys.push_back(i);
  }
  std::mt19937 generator(0x5eed);
  std::shuffle(keys.begin(), keys.end(), generator);

  std::unordered_map<int64_t, int64_t> table;
  table.reserve(state.range(0));
  for (size_t i = 0; i < keys.size(); ++i) {
    table.emplace(keys[i], static_cast<int64_t>(i * 17 + 3));
  }

  size_t index = 0;
  volatile int64_t observed = 0;
  while (state.KeepRunning()) {
    for (size_t i = 0; i < keys.size(); ++i) {
      int64_t key = keys[index++ % keys.size()];
      benchmark::DoNotOptimize(key);
      auto result = table.find(key);
      observed ^= result->second;
    }
  }
  benchmark::DoNotOptimize(observed);

  state.SetItemsProcessed(int64_t(state.iterations()) * int64_t(state.range(0)));
  state.SetComplexityN(state.range(0));
}

static void BM_HashTableInsertWithChains(benchmark::State& state) {
  int64_t value = 1;

  while (state.KeepRunning()) {
    state.PauseTiming();
    HashTable<CollidingHashedInteger, int64_t> table;
    state.ResumeTiming();

    for (ssize_t i = 0; i < state.range(0); ++i) {
      CollidingHashedInteger key(state.range(0) / 2, i);
      table.insert(key, value);
    }
  }

  state.SetItemsProcessed(int64_t(state.iterations()) * int64_t(state.range(0)));
}

static void BM_HashTableLookupWithChains(benchmark::State& state) {
  HashTable<CollidingHashedInteger, int64_t> table;
  int64_t value = 1;
  for (ssize_t i = 0; i < state.range(0); ++i) {
    CollidingHashedInteger key(state.range(0) / 2, i);
    table.insert(key, value);
  }

  while (state.KeepRunning()) {
    for (ssize_t i = 0; i < state.range(0); ++i) {
      CollidingHashedInteger key(state.range(0) / 2, i);
      benchmark::DoNotOptimize(table.lookup(key));
    }
  }

  state.SetItemsProcessed(int64_t(state.iterations()) * int64_t(state.range(0)));
}

BENCHMARK(BM_HashTableInsertPreallocate)->Range(8, 16384);
BENCHMARK(BM_HashTableInsertNoChains)->Range(8, 16384);
BENCHMARK(BM_HashTableInsertNoChainsReserved)->Range(8, 16384);
BENCHMARK(BM_HashTableLookupNoChains)->Range(8, 16384);
BENCHMARK(BM_HashTableInsertNoChainsLinear)->Range(8, 16384);
BENCHMARK(BM_HashTableInsertNoChainsReservedLinear)->Range(8, 16384);
BENCHMARK(BM_HashTableLookupNoChainsLinear)->Range(8, 16384);
BENCHMARK(BM_HashTableLookupStdUnorderedMap)->Range(8, 16384);
BENCHMARK(BM_HashTableInsertWithChains)->Range(8, 16384);
BENCHMARK(BM_HashTableLookupWithChains)->Range(8, 16384);
