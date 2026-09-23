/* Copyright (c) 2026, Pedigree Developers. */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/utilities/Cache.h"

#include <algorithm>
#include <vector>

#include <gtest/gtest.h>

namespace {
constexpr size_t Page = TargetInfo::getPageSize();
uintptr_t publish(Cache& cache, uintptr_t key) {
  const auto page = cache.insert(key);
  if (page) {
    *reinterpret_cast<unsigned char*>(page) = 0x63;
    cache.markNoLongerEditing(key);
    cache.markDirty(key);
  }
  return page;
}
struct Writes {
  size_t calls = 0;
  size_t retries = 0;
  uintptr_t fail = ~uintptr_t(0);
  static bool write(uintptr_t key, uintptr_t page, void* opaque) {
    auto& self = *static_cast<Writes*>(opaque);
    ++self.calls;
    EXPECT_EQ(*reinterpret_cast<unsigned char*>(page), 0x63);
    return key != self.fail;
  }
  static bool cache(CacheConstants::CallbackCause cause, uintptr_t, uintptr_t, void* opaque) {
    if (cause == CacheConstants::WriteBack) {
      ++static_cast<Writes*>(opaque)->retries;
      return false;
    }
    return true;
  }
};
}  // namespace
TEST(CachePaging, ResidentReadStopsAtGapsAndUnpublishedPages) {
  Cache cache;
  cache.setDirtyTracking(Cache::DirtyTracking::Explicit);
  const uintptr_t first = cache.insert(0);
  ASSERT_NE(first, 0U);
  std::fill_n(reinterpret_cast<unsigned char*>(first), Page, 0x63);
  cache.markNoLongerEditing(0);
  const uintptr_t second = cache.insert(Page);
  ASSERT_NE(second, 0U);
  std::fill_n(reinterpret_cast<unsigned char*>(second), Page, 0x72);
  std::vector<unsigned char> bytes(2 * Page + 2, 0xA5);
  EXPECT_EQ(cache.read(7, 2 * Page, reinterpret_cast<uintptr_t>(bytes.data() + 1),
                       [](uintptr_t, size_t) { return false; }),
            0U);
  EXPECT_EQ(bytes[1], 0xA5);
  EXPECT_EQ(cache.read(7, 2 * Page, reinterpret_cast<uintptr_t>(bytes.data() + 1)), Page - 7);
  EXPECT_EQ(bytes.front(), 0xA5);
  EXPECT_EQ(bytes[Page - 7], 0x63);
  EXPECT_EQ(bytes[Page - 6], 0xA5);
  cache.markNoLongerEditing(Page);
  EXPECT_EQ(cache.read(7, 2 * Page, reinterpret_cast<uintptr_t>(bytes.data() + 1)), 2 * Page - 7);
  EXPECT_EQ(bytes[Page - 6], 0x72);
  EXPECT_EQ(bytes[2 * Page - 6], 0xA5);
  EXPECT_EQ(cache.read(2 * Page, Page, 0), 0U);
  EXPECT_EQ(cache.read(~uintptr_t(0) - 3, 8, 0), 0U);
  EXPECT_TRUE(cache.empty());  // All temporary page pins were released.
}

TEST(CachePaging, ResidentReadBoundsEachBatchAndDoesNotLeakPins) {
  Cache cache;
  cache.setDirtyTracking(Cache::DirtyTracking::Explicit);
  for (size_t i = 0; i < 34; ++i) {
    const uintptr_t page = cache.insert(i * Page);
    ASSERT_NE(page, 0U);
    std::fill_n(reinterpret_cast<unsigned char*>(page), Page, static_cast<unsigned char>(i));
    cache.markNoLongerEditing(i * Page);
  }
  std::vector<unsigned char> bytes(34 * Page, 0xA5);
  const size_t first = cache.read(3, bytes.size() - 3, reinterpret_cast<uintptr_t>(bytes.data()));
  ASSERT_EQ(first, 32 * Page - 3);
  EXPECT_EQ(bytes[first - 1], 31);
  EXPECT_EQ(bytes[first], 0xA5);
  EXPECT_EQ(cache.read(3 + first, 2 * Page, reinterpret_cast<uintptr_t>(bytes.data() + first)),
            2 * Page);
  EXPECT_EQ(bytes[34 * Page - 4], 33);
  EXPECT_EQ(bytes[34 * Page - 3], 0xA5);
  EXPECT_TRUE(cache.empty());
}

TEST(CachePaging, LoanRejectsActivationBeforeWriteback) {
  Cache cache;
  Writes writes;
  ASSERT_NE(publish(cache, 0), 0U);
  ASSERT_TRUE(cache.pin(0));
  UniquePointer<Cache::PreparedDiscard> plan;
  EXPECT_EQ(cache.prepareDiscardFrom(0, nullptr, 0, plan), Cache::DiscardStatus::Busy);
  EXPECT_EQ(writes.calls, 0U);
  cache.release(0);
  ASSERT_EQ(cache.prepareDiscardFrom(0, nullptr, 0, plan), Cache::DiscardStatus::Ready);
  EXPECT_TRUE(plan.get()->writeback(Writes::write, &writes));
  EXPECT_EQ(writes.calls, 1U);
  plan.get()->commit();
}
TEST(CachePaging, WriteOrLaterFlushFailureRetainsPagesForRetry) {
  Writes writes;
  Cache cache;
  cache.setCallback(Writes::cache, &writes);
  ASSERT_NE(publish(cache, 0), 0U);
  ASSERT_NE(publish(cache, Page), 0U);
  UniquePointer<Cache::PreparedDiscard> plan;
  ASSERT_EQ(cache.prepareDiscardFrom(0, nullptr, 0, plan), Cache::DiscardStatus::Ready);
  writes.fail = Page;
  EXPECT_FALSE(plan.get()->writeback(Writes::write, &writes));
  EXPECT_EQ(writes.calls, 2U);
  plan.reset();
  EXPECT_FALSE(cache.evict(Page));
  EXPECT_TRUE(cache.exists(Page, Page));
  writes.fail = ~uintptr_t(0);
  ASSERT_EQ(cache.prepareDiscardFrom(0, nullptr, 0, plan), Cache::DiscardStatus::Ready);
  EXPECT_TRUE(plan.get()->writeback(Writes::write, &writes));
  plan.reset();  // A final hardware-flush failure cancels this transaction.
  EXPECT_FALSE(cache.evict(0));
  EXPECT_TRUE(cache.exists(0, Page));
  ASSERT_EQ(cache.prepareDiscardFrom(0, nullptr, 0, plan), Cache::DiscardStatus::Ready);
  EXPECT_TRUE(plan.get()->writeback(Writes::write, &writes));
  plan.get()->commit();
  EXPECT_FALSE(cache.exists(0, Page));
  EXPECT_FALSE(cache.exists(Page, Page));
  EXPECT_EQ(writes.calls, 6U);
  EXPECT_EQ(writes.retries, 2U);
}
TEST(CachePaging, ExpectedExternalReferencesCannotBeWrittenExclusively) {
  Cache cache;
  Writes writes;
  ASSERT_NE(publish(cache, 0), 0U);
  ASSERT_TRUE(cache.pin(0));
  Cache::DiscardReference reference{0, 1};
  UniquePointer<Cache::PreparedDiscard> plan;
  ASSERT_EQ(cache.prepareDiscardFrom(0, &reference, 1, plan), Cache::DiscardStatus::Ready);
  EXPECT_FALSE(plan.get()->writeback(Writes::write, &writes));
  EXPECT_EQ(writes.calls, 0U);
  plan.reset();
  cache.release(0);
}
