#define PEDIGREE_EXTERNAL_SOURCE 1

#include <benchmark/benchmark.h>

#include <array>
#include <vector>

namespace {

constexpr size_t kCpuCount = 4;
constexpr size_t kObjectsPerSlab = 64;

class DistributedObjectLists {
 public:
  explicit DistributedObjectLists(size_t slabCount) {
    for (auto& list : m_Lists)
      list.reserve(slabCount * kObjectsPerSlab / kCpuCount);
    for (size_t slab = 0; slab < slabCount; ++slab)
      restoreSlab(slab);
  }

  void restoreSlab(size_t slab) {
    const size_t firstObject = slab * kObjectsPerSlab;
    for (size_t object = 0; object < kObjectsPerSlab; ++object)
      m_Lists[(firstObject + object) % kCpuCount].push_back(firstObject + object);
  }

  size_t reclaimSlab(size_t slab) {
    size_t removed = 0;
    const size_t firstObject = slab * kObjectsPerSlab;
    const size_t lastObject = firstObject + kObjectsPerSlab;
    for (auto& list : m_Lists) {
      size_t write = 0;
      for (size_t object : list) {
        if (object >= firstObject && object < lastObject) {
          ++removed;
        } else {
          list[write++] = object;
        }
      }
      list.resize(write);
    }
    return removed;
  }

 private:
  std::array<std::vector<size_t>, kCpuCount> m_Lists;
};

class SlabOwnedLists {
 public:
  explicit SlabOwnedLists(size_t slabCount) : m_Slabs(slabCount) {
    for (auto& slab : m_Slabs)
      slab.freeObjects.reserve(kObjectsPerSlab);
    for (size_t slab = 0; slab < slabCount; ++slab)
      restoreSlab(slab);
  }

  void restoreSlab(size_t slab) {
    auto& freeObjects = m_Slabs[slab].freeObjects;
    for (size_t object = 0; object < kObjectsPerSlab; ++object)
      freeObjects.push_back(object);
  }

  size_t reclaimSlab(size_t slab) {
    auto& freeObjects = m_Slabs[slab].freeObjects;
    if (freeObjects.size() != kObjectsPerSlab)
      return 0;
    freeObjects.clear();
    return kObjectsPerSlab;
  }

 private:
  struct Slab {
    std::vector<size_t> freeObjects;
  };

  std::vector<Slab> m_Slabs;
};

static void BM_SlamSlabRecoveryDistributedObjects(benchmark::State& state) {
  const size_t slabs = static_cast<size_t>(state.range(0));
  DistributedObjectLists model(slabs);
  for (auto _ : state) {
    if (model.reclaimSlab(slabs - 1) != kObjectsPerSlab) {
      state.SkipWithError("distributed model did not reclaim a full slab");
      break;
    }
    state.PauseTiming();
    model.restoreSlab(slabs - 1);
    state.ResumeTiming();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kObjectsPerSlab);
}

static void BM_SlamSlabRecoveryOwnedObjects(benchmark::State& state) {
  const size_t slabs = static_cast<size_t>(state.range(0));
  SlabOwnedLists model(slabs);
  for (auto _ : state) {
    if (model.reclaimSlab(slabs - 1) != kObjectsPerSlab) {
      state.SkipWithError("slab-owned model did not reclaim a full slab");
      break;
    }
    state.PauseTiming();
    model.restoreSlab(slabs - 1);
    state.ResumeTiming();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kObjectsPerSlab);
}

}  // namespace

BENCHMARK(BM_SlamSlabRecoveryDistributedObjects)
    ->RangeMultiplier(8)
    ->Range(64, 1 << 14);
BENCHMARK(BM_SlamSlabRecoveryOwnedObjects)->RangeMultiplier(8)->Range(64, 1 << 14);
