/* Copyright (c) 2026, Pedigree Developers. */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/utilities/Cache.h"

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
