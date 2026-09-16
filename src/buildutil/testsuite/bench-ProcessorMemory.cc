/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include "pedigree/kernel/utilities/RangeList.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <benchmark/benchmark.h>
#include <unordered_map>

namespace {

constexpr uintptr_t kPageSize = 4096;

static uintptr_t virtualAddress(size_t index) {
  return 0x400000 + index * kPageSize;
}

static void BM_PMM_ContiguousPageAllocation(benchmark::State& state) {
  const size_t pageCount = static_cast<size_t>(state.range(0));
  const uint64_t bytes = static_cast<uint64_t>(pageCount) * kPageSize * 2;

  while (state.KeepRunning()) {
    state.PauseTiming();
    RangeList<uint64_t> ranges;
    ranges.free(0, bytes);
    state.ResumeTiming();

    for (size_t i = 0; i < pageCount; ++i) {
      uint64_t address = 0;
      bool allocated = ranges.allocate(kPageSize, address);
      benchmark::DoNotOptimize(allocated);
      benchmark::DoNotOptimize(address);
    }
  }

  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(pageCount));
}

static void BM_PMM_FragmentedPageAllocation(benchmark::State& state) {
  const size_t rangeCount = static_cast<size_t>(state.range(0));

  while (state.KeepRunning()) {
    state.PauseTiming();
    RangeList<uint64_t> ranges;
    for (size_t i = 0; i < rangeCount; ++i) {
      ranges.free(i * kPageSize * 2, kPageSize);
    }
    state.ResumeTiming();

    for (size_t i = 0; i < rangeCount; ++i) {
      uint64_t address = 0;
      bool allocated = ranges.allocate(kPageSize, address);
      benchmark::DoNotOptimize(allocated);
      benchmark::DoNotOptimize(address);
    }
  }

  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(rangeCount));
}

class PageTableModel {
 public:
  bool map(uintptr_t address, uintptr_t physical) {
    Node* node = &m_Root;
    for (size_t level = 0; level < 3; ++level) {
      const size_t index = (address >> (39 - level * 9)) & 0x1ff;
      if (!node->children[index]) {
        node->children[index] = std::make_unique<Node>();
      }
      node = node->children[index].get();
    }

    const size_t leaf = (address >> 12) & 0x1ff;
    if (node->leaves[leaf]) {
      return false;
    }
    node->leaves[leaf] = physical | 1;
    return true;
  }

  uintptr_t lookup(uintptr_t address) const {
    const Node* node = &m_Root;
    for (size_t level = 0; level < 3; ++level) {
      const size_t index = (address >> (39 - level * 9)) & 0x1ff;
      node = node->children[index].get();
      if (!node) {
        return 0;
      }
    }
    return node->leaves[(address >> 12) & 0x1ff];
  }

  bool unmap(uintptr_t address) {
    Node* node = &m_Root;
    for (size_t level = 0; level < 3; ++level) {
      const size_t index = (address >> (39 - level * 9)) & 0x1ff;
      node = node->children[index].get();
      if (!node) {
        return false;
      }
    }

    const size_t leaf = (address >> 12) & 0x1ff;
    const bool mapped = node->leaves[leaf] != 0;
    node->leaves[leaf] = 0;
    return mapped;
  }

 private:
  struct Node {
    std::array<std::unique_ptr<Node>, 512> children{};
    std::array<uintptr_t, 512> leaves{};
  };

  Node m_Root;
};

static std::vector<uintptr_t> makeAddresses(size_t count) {
  std::vector<uintptr_t> addresses;
  addresses.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    addresses.push_back(virtualAddress(i));
  }
  return addresses;
}

static void BM_VMM_PageTableLookup(benchmark::State& state) {
  const auto addresses = makeAddresses(static_cast<size_t>(state.range(0)));
  PageTableModel pageTable;
  for (size_t i = 0; i < addresses.size(); ++i) {
    pageTable.map(addresses[i], i * kPageSize + kPageSize);
  }

  size_t index = 0;
  while (state.KeepRunning()) {
    benchmark::DoNotOptimize(pageTable.lookup(addresses[index]));
    index = (index + 1) % addresses.size();
  }

  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}

static void BM_VMM_PageTableLookupStdUnorderedMap(benchmark::State& state) {
  const auto addresses = makeAddresses(static_cast<size_t>(state.range(0)));
  std::unordered_map<uintptr_t, uintptr_t> mappings;
  mappings.reserve(addresses.size());
  for (size_t i = 0; i < addresses.size(); ++i) {
    mappings.emplace(addresses[i], (i * 17 + 3) * kPageSize + kPageSize);
  }

  size_t index = 0;
  volatile uintptr_t observed = 0;
  while (state.KeepRunning()) {
    uintptr_t key = addresses[index];
    benchmark::DoNotOptimize(key);
    auto result = mappings.find(key);
    observed ^= result->second;
    index = (index + 1) % addresses.size();
  }
  benchmark::DoNotOptimize(observed);

  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}

static void BM_VMM_PageTableMapUnmap(benchmark::State& state) {
  const auto addresses = makeAddresses(static_cast<size_t>(state.range(0)));
  PageTableModel pageTable;
  for (size_t i = 0; i < addresses.size(); ++i) {
    pageTable.map(addresses[i], i * kPageSize + kPageSize);
  }

  while (state.KeepRunning()) {
    for (size_t i = 0; i < addresses.size(); ++i) {
      benchmark::DoNotOptimize(pageTable.unmap(addresses[i]));
      benchmark::DoNotOptimize(pageTable.map(addresses[i], i * kPageSize + kPageSize));
    }
  }

  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(addresses.size()) * 2);
}

}  // namespace

BENCHMARK(BM_PMM_ContiguousPageAllocation)->RangeMultiplier(4)->Range(8, 4096);
BENCHMARK(BM_PMM_FragmentedPageAllocation)->RangeMultiplier(4)->Range(8, 1024);
BENCHMARK(BM_VMM_PageTableLookup)->RangeMultiplier(4)->Range(8, 4096);
BENCHMARK(BM_VMM_PageTableLookupStdUnorderedMap)->RangeMultiplier(4)->Range(8, 4096);
BENCHMARK(BM_VMM_PageTableMapUnmap)->RangeMultiplier(4)->Range(8, 4096);
