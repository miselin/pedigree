/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/utilities/Cache.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr size_t Page = TargetInfo::getPageSize();
constexpr size_t Keys = 8;

[[noreturn]] void fail(size_t operation, const char* check) {
  std::fprintf(stderr, "Cache fuzz mismatch at operation %zu: %s\n", operation, check);
  std::abort();
}

bool writeback(CacheConstants::CallbackCause, uintptr_t, uintptr_t, void*) {
  return true;
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > 512) {
    return 0;
  }

  Cache cache;
  cache.setCallback(writeback, nullptr);
  cache.setDirtyTracking(Cache::DirtyTracking::Explicit);
  std::array<bool, Keys> present{};
  std::array<uint8_t, Keys> values{};
  const size_t operations = size / 3 < 64 ? size / 3 : 64;
  for (size_t operation = 0; operation < operations; ++operation) {
    const size_t offset = operation * 3;
    const size_t index = data[offset + 1] % Keys;
    const uintptr_t key = index * Page;
    switch (data[offset] % 5) {
      case 0:
      case 1: {
        bool existed = false;
        const uintptr_t location = cache.insert(key, &existed);
        if (!location || existed != present[index]) {
          fail(operation, "insert result");
        }
        if (!existed) {
          present[index] = true;
          values[index] = data[offset + 2];
          *reinterpret_cast<uint8_t*>(location) = values[index];
          cache.markNoLongerEditing(key);
          cache.markDirty(key);
        }
        break;
      }
      case 2: {
        const uintptr_t location = cache.lookup(key);
        if (bool(location) != present[index]) {
          fail(operation, "lookup result");
        }
        if (location) {
          if (*reinterpret_cast<const uint8_t*>(location) != values[index]) {
            fail(operation, "lookup value");
          }
          cache.release(key);
        }
        break;
      }
      case 3:
        if (present[index]) {
          const uintptr_t location = cache.lookup(key);
          if (!location) {
            fail(operation, "update lookup");
          }
          values[index] = data[offset + 2];
          *reinterpret_cast<uint8_t*>(location) = values[index];
          cache.markDirty(key);
          cache.release(key);
          if (!cache.sync(key, false)) {
            fail(operation, "sync");
          }
        }
        break;
      case 4: {
        const bool evicted = cache.evict(key);
        if (evicted != present[index]) {
          fail(operation, "eviction result");
        }
        present[index] = false;
        break;
      }
    }
  }

  for (size_t index = 0; index < Keys; ++index) {
    const uintptr_t key = index * Page;
    const uintptr_t location = cache.lookup(key);
    if (bool(location) != present[index]) {
      fail(operations, "final lookup result");
    }
    if (location) {
      if (*reinterpret_cast<const uint8_t*>(location) != values[index]) {
        fail(operations, "final lookup value");
      }
      cache.release(key);
      if (!cache.evict(key)) {
        fail(operations, "final eviction");
      }
    }
  }
  return 0;
}
