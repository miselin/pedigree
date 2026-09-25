/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#define PEDIGREE_EXTERNAL_SOURCE 1

#include "pedigree/kernel/utilities/BloomFilter.h"
#include "pedigree/kernel/utilities/ExtensibleBitmap.h"
#include "pedigree/kernel/utilities/HashTable.h"
#include "pedigree/kernel/utilities/LruCache.h"
#include "pedigree/kernel/utilities/ObjectPool.h"
#include "pedigree/kernel/utilities/Tree.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

namespace {
constexpr size_t MaxOperations = 64;
constexpr size_t BitmapBits = 144;
constexpr size_t Keys = 16;

[[noreturn]] void fail(const char* utility, size_t step, const char* check) {
  std::fprintf(stderr, "%s mismatch at operation %zu: %s\n", utility, step, check);
  std::abort();
}

void require(bool condition, const char* utility, size_t step, const char* check) {
  if (!condition) {
    fail(utility, step, check);
  }
}

struct Input {
  const uint8_t* data;
  size_t size;
  size_t offset = 1;

  uint8_t next(uint8_t fallback) {
    return offset < size ? data[offset++] : fallback;
  }

  size_t operations() const {
    return std::min(MaxOperations, std::max<size_t>(16, (size + 3) / 4));
  }
};

using Bits = std::array<bool, BitmapBits>;

void checkBitmap(ExtensibleBitmap& bitmap, const Bits& model, size_t step) {
  size_t first = static_cast<size_t>(~0U);
  size_t last = static_cast<size_t>(~0U);
  size_t firstClear = BitmapBits;
  for (size_t bit = 0; bit < BitmapBits; ++bit) {
    require(bitmap.test(bit) == model[bit], "ExtensibleBitmap", step, "bit value");
    if (model[bit]) {
      if (first == static_cast<size_t>(~0U)) {
        first = bit;
      }
      last = bit;
    } else if (firstClear == BitmapBits) {
      firstClear = bit;
    }
  }
  require(bitmap.getFirstSet() == first, "ExtensibleBitmap", step, "first set");
  require(bitmap.getLastSet() == last, "ExtensibleBitmap", step, "last set");
  require(bitmap.getFirstClear() == firstClear, "ExtensibleBitmap", step, "first clear");
}

void fuzzBitmap(Input input) {
  ExtensibleBitmap bitmaps[2];
  Bits models[2]{};
  for (size_t step = 0; step < input.operations(); ++step) {
    const uint8_t action = input.next(step * 7 + 3);
    const size_t bit = input.next(step * 13 + 61) % BitmapBits;
    const size_t target = input.next(step / 4) & 1;
    const size_t other = target ^ 1;
    switch (action % 6) {
      case 0:
      case 1:
        bitmaps[target].set(bit);
        models[target][bit] = true;
        break;
      case 2:
      case 3:
        bitmaps[target].clear(bit);
        models[target][bit] = false;
        break;
      case 4: {
        ExtensibleBitmap copy(bitmaps[target]);
        checkBitmap(copy, models[target], step);
        bitmaps[other] = copy;
        models[other] = models[target];
        break;
      }
      case 5:
        bitmaps[target] = bitmaps[other];
        models[target] = models[other];
        break;
    }
    if (step % 4 == 0 || step + 1 == input.operations()) {
      checkBitmap(bitmaps[0], models[0], step);
      checkBitmap(bitmaps[1], models[1], step);
    }
  }
}

void fuzzBloom(Input input) {
  const size_t hashes = 1 + input.next(2) % 5;
  BloomFilter<char> filter(127, hashes);
  std::set<char> members;
  for (size_t step = 0; step < input.operations(); ++step) {
    const uint8_t action = input.next(step * 7 + 3);
    const char value = static_cast<char>(input.next(step * 13 + 61));
    if (action % 8 == 0) {
      filter.clear();
      members.clear();
      for (size_t key = 0; key < 32; ++key) {
        require(!filter.contains(static_cast<char>(key)), "BloomFilter", step, "clear");
      }
    } else if (action % 3 == 0) {
      // False positives are legal; a newly added value must never be absent.
      filter.add(value);
      members.insert(value);
      require(filter.contains(value), "BloomFilter", step, "new member");
    } else {
      filter.contains(value);
    }
    for (char member : members) {
      require(filter.contains(member), "BloomFilter", step, "stored member");
    }
  }
}

struct Key {
  int value = 0;

  size_t hash() const {
    return static_cast<size_t>(value & 3);
  }
  bool operator==(const Key& other) const {
    return value == other.value;
  }
  bool operator!=(const Key& other) const {
    return value != other.value;
  }
};

template <bool Quadratic>
using Table = HashTable<Key, int, Key, 4, Quadratic>;

template <bool Quadratic>
void checkTable(const Table<Quadratic>& table, const std::array<int, Keys>& model, size_t step) {
  size_t count = 0;
  for (size_t key = 0; key < Keys; ++key) {
    const Key actualKey{static_cast<int>(key)};
    const auto found = table.lookup(actualKey);
    const bool present = model[key] != 0;
    require(table.contains(actualKey) == present, "HashTable", step, "contains");
    require(found.hasValue() == present, "HashTable", step, "lookup presence");
    if (present) {
      require(found.value() == model[key], "HashTable", step, "lookup value");
      ++count;
    }
  }
  require(table.count() == count, "HashTable", step, "count");
  std::array<bool, Keys> seen{};
  for (size_t index = 0; index < count; ++index) {
    const auto result = table.getNth(index);
    require(result.hasValue(), "HashTable", step, "nth presence");
    const int key = result.value().first().value;
    require(key >= 0 && key < static_cast<int>(Keys), "HashTable", step, "nth key");
    require(!seen[key] && model[key] == result.value().second(), "HashTable", step, "nth value");
    seen[key] = true;
  }
  require(table.getNth(count).hasError(), "HashTable", step, "nth end");
}

template <bool Quadratic>
void fuzzTable(Input input) {
  Table<Quadratic> table;
  std::array<int, Keys> model{};
  for (size_t step = 0; step < input.operations(); ++step) {
    const uint8_t action = input.next(step * 7 + 3);
    const size_t key = input.next(step * 13 + 61) % Keys;
    const int value = 1 + static_cast<int>(key) * 256 + input.next(step * 17 + 5);
    const Key actualKey{static_cast<int>(key)};
    switch (action % 7) {
      case 0:
      case 1:
        require(table.insert(actualKey, value) == (model[key] == 0), "HashTable", step,
                "insert result");
        if (!model[key]) {
          model[key] = value;
        }
        break;
      case 2:
        require(table.update(actualKey, value) == (model[key] != 0), "HashTable", step,
                "update result");
        if (model[key]) {
          model[key] = value;
        }
        break;
      case 3:
        table.remove(actualKey);
        model[key] = 0;
        break;
      case 4:
        table.reserve(1 + (action >> 3) % 32);
        break;
      case 5: {
        Table<Quadratic> copy;
        copy.copyFrom(table);
        checkTable(copy, model, step);
        break;
      }
      case 6:
        table.clear();
        model.fill(0);
        break;
    }
    if (step % 4 == 0 || step + 1 == input.operations()) {
      checkTable(table, model, step);
    }
  }
}

void checkTree(const Tree<int, int>& tree, const std::map<int, int>& model, int query,
               size_t step) {
  require(tree.count() == model.size(), "Tree", step, "count");
  for (int key = 0; key < static_cast<int>(Keys); ++key) {
    const auto found = model.find(key);
    require(tree.contains(key) == (found != model.end()), "Tree", step, "contains");
    require(tree.lookup(key) == (found == model.end() ? 0 : found->second), "Tree", step, "lookup");
  }

  int foundKey = -1, foundValue = -1;
  auto bound = model.lower_bound(query);
  require(tree.lowerBound(query, foundKey, foundValue) == (bound != model.end()), "Tree", step,
          "lower bound presence");
  if (bound != model.end()) {
    require(foundKey == bound->first && foundValue == bound->second, "Tree", step,
            "lower bound value");
  } else {
    require(foundKey == -1 && foundValue == -1, "Tree", step, "lower bound output");
  }

  foundKey = -1;
  foundValue = -1;
  bound = model.upper_bound(query);
  const bool floorPresent = bound != model.begin();
  if (floorPresent) {
    --bound;
  }
  require(tree.floorBound(query, foundKey, foundValue) == floorPresent, "Tree", step,
          "floor bound presence");
  if (floorPresent) {
    require(foundKey == bound->first && foundValue == bound->second, "Tree", step,
            "floor bound value");
  } else {
    require(foundKey == -1 && foundValue == -1, "Tree", step, "floor bound output");
  }

  auto expected = model.begin();
  for (auto it = tree.begin(); it != tree.end(); ++it) {
    require(expected != model.end(), "Tree", step, "iteration length");
    require(it.key() == expected->first && it.value() == expected->second, "Tree", step,
            "iteration order");
    ++expected;
  }
  require(expected == model.end(), "Tree", step, "iteration completion");
}

void fuzzTree(Input input) {
  Tree<int, int> tree;
  std::map<int, int> model;
  for (size_t step = 0; step < input.operations(); ++step) {
    const uint8_t action = input.next(step * 7 + 3);
    const int key = input.next(step * 13 + 61) % Keys;
    const int value = 1 + key * 256 + input.next(step * 17 + 5);
    switch (action % 8) {
      case 0:
      case 1:
        tree.insert(key, value);
        model[key] = value;
        break;
      case 2:
        tree.remove(key);
        model.erase(key);
        break;
      case 3: {
        int taken = -1;
        const auto found = model.find(key);
        require(tree.take(key, taken) == (found != model.end()), "Tree", step, "take presence");
        require(taken == (found == model.end() ? -1 : found->second), "Tree", step, "take value");
        model.erase(key);
        break;
      }
      case 4: {
        int* found = tree.find(key);
        require((found != nullptr) == (model.find(key) != model.end()), "Tree", step,
                "mutable find");
        if (found) {
          *found = value;
          model[key] = value;
        }
        break;
      }
      case 5: {
        Tree<int, int> copy(tree);
        checkTree(copy, model, key, step);
        break;
      }
      case 6:
        tree.clear();
        model.clear();
        break;
      case 7:
        tree = static_cast<const Tree<int, int>&>(tree);
        break;
    }
    if (step % 4 == 0 || step + 1 == input.operations()) {
      checkTree(tree, model, key, step);
    }
  }
}

void fuzzLru(Input input) {
  LruCache<int, int, 4> cache;
  std::array<int, Keys> lastStored{};
  size_t hits = 0, misses = 0;
  for (size_t step = 0; step < input.operations(); ++step) {
    const uint8_t action = input.next(step * 7 + 3);
    const int key = input.next(step * 13 + 61) % Keys;
    const int value = 1 + key * 256 + input.next(step * 17 + 5);
    int found = -1;
    if (action & 1) {
      cache.store(key, value);
      lastStored[key] = value;
      require(cache.get(key, found) && found == value, "LruCache", step, "store result");
      ++hits;
    } else if (cache.get(key, found)) {
      require(lastStored[key] == found, "LruCache", step, "stale value");
      ++hits;
    } else {
      ++misses;
    }
    require(cache.hits() == hits && cache.misses() == misses, "LruCache", step, "statistics");
  }
}

struct PoolValue {
  explicit PoolValue(int initial = 0) : value(initial) {
    ++live;
  }
  ~PoolValue() {
    --live;
  }
  PoolValue(const PoolValue&) = delete;
  PoolValue& operator=(const PoolValue&) = delete;

  int value;
  static int live;
};
int PoolValue::live = 0;

void fuzzObjectPool(Input input) {
  require(PoolValue::live == 0, "ObjectPool", 0, "initial lifetime");
  {
    ObjectPool<PoolValue, 4> pool;
    std::array<PoolValue*, 12> active{};
    size_t count = 0;
    for (size_t step = 0; step < input.operations(); ++step) {
      const uint8_t action = input.next(step * 7 + 3);
      const int value = input.next(step * 13 + 61);
      if ((action & 1) && count < active.size()) {
        PoolValue* item = pool.tryAllocate(value);
        require(item != nullptr, "ObjectPool", step, "allocation");
        for (size_t i = 0; i < count; ++i) {
          require(item != active[i], "ObjectPool", step, "unique loan");
        }
        item->value = value;
        active[count++] = item;
      } else if (count) {
        const size_t index = static_cast<size_t>(value) % count;
        PoolValue* item = active[index];
        active[index] = active[--count];
        pool.deallocate(item);
      }
      require(PoolValue::live >= static_cast<int>(count) && PoolValue::live <= 16, "ObjectPool",
              step, "live objects");
    }
    while (count) {
      pool.deallocate(active[--count]);
    }
  }
  require(PoolValue::live == 0, "ObjectPool", input.operations(), "final lifetime");
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > 512) {
    return 0;
  }

  Input input{data, size};
  switch (size ? data[0] % 7 : 3) {
    case 0:
      fuzzBitmap(input);
      break;
    case 1:
      fuzzBloom(input);
      break;
    case 2:
      fuzzTable<true>(input);
      break;
    case 3:
      fuzzTree(input);
      break;
    case 4:
      fuzzLru(input);
      break;
    case 5:
      fuzzObjectPool(input);
      break;
    case 6:
      fuzzTable<false>(input);
      break;
  }
  return 0;
}
