/* Copyright (c) 2026, Pedigree Developers. */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/utilities/Cache.h"

#include <cstdio>

#include <gtest/gtest.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

class CacheMemoryTestPeer {
 public:
  static size_t filterBytes(const Cache& cache) {
    return cache.m_PageFilter.m_Bitmap.m_DynamicMapSize;
  }
  static size_t allocatedFilterBytes(const Cache& cache) {
    const auto* bitmap = cache.m_PageFilter.m_Bitmap.m_pDynamicMap;
#if defined(__APPLE__)
    return bitmap ? malloc_size(bitmap) : 0;
#elif defined(__GLIBC__)
    return bitmap ? malloc_usable_size(const_cast<uint8_t*>(bitmap)) : 0;
#else
    return filterBytes(cache);
#endif
  }
  static bool filterContains(Cache& cache, uintptr_t key) {
    return cache.m_PageFilter.contains(key);
  }
};

namespace {
constexpr size_t Page = TargetInfo::getPageSize();
}

TEST(CacheMemory, FirstPageMetadataStaysSmallAcrossIndependentCaches) {
  Cache caches[16];
  size_t requested = 0;
  size_t allocated = 0;
  for (auto& cache : caches) {
    ASSERT_NE(cache.insert(0), 0U);
    cache.markNoLongerEditing(0);
    const size_t bytes = CacheMemoryTestPeer::filterBytes(cache);
    requested += bytes;
    allocated += CacheMemoryTestPeer::allocatedFilterBytes(cache);
    EXPECT_LE(bytes, 512U);
    EXPECT_LE(CacheMemoryTestPeer::allocatedFilterBytes(cache), 4096U);
  }
  std::printf("CACHE-MEMORY: first-page filter requested=%zu allocated=%zu caches=16\n", requested,
              allocated);
  EXPECT_LE(requested, 16U * 512U);
  EXPECT_LE(allocated, 16U * 4096U);
}

TEST(CacheMemory, SaturatedFilterPreservesLookupMissEvictionAndReinsertion) {
  Cache cache;
  cache.setCallback([](CacheConstants::CallbackCause, uintptr_t, uintptr_t, void*) { return true; },
                    nullptr);
  constexpr size_t Pages = 4096;
  for (size_t i = 0; i < Pages; ++i) {
    const uintptr_t address = cache.insert(i * Page);
    ASSERT_NE(address, 0U);
    *reinterpret_cast<uintptr_t*>(address) = i;
    cache.markNoLongerEditing(i * Page);
  }
  // ExtensibleBitmap doubles its backing when later hashes exceed the first allocation.
  EXPECT_LE(CacheMemoryTestPeer::filterBytes(cache), 1024U);
  for (size_t i = 0; i < Pages; ++i) {
    const uintptr_t address = cache.lookup(i * Page);
    ASSERT_NE(address, 0U);
    EXPECT_EQ(*reinterpret_cast<uintptr_t*>(address), i);
    cache.release(i * Page);
  }
  uintptr_t miss = Pages * Page;
  while (miss < 2 * Pages * Page && !CacheMemoryTestPeer::filterContains(cache, miss)) {
    miss += Page;
  }
  ASSERT_LT(miss, 2 * Pages * Page);
  EXPECT_EQ(cache.lookup(miss), 0U);
  for (size_t i = 0; i < Pages; i += 31) {
    ASSERT_TRUE(cache.evict(i * Page));
    EXPECT_EQ(cache.lookup(i * Page), 0U);
    const uintptr_t address = cache.insert(i * Page);
    ASSERT_NE(address, 0U);
    cache.markNoLongerEditing(i * Page);
    ASSERT_NE(cache.lookup(i * Page), 0U);
    cache.release(i * Page);
  }
}
