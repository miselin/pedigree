/* Copyright (c) 2026, Pedigree Developers. */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/utilities/Cache.h"

#include "modules/drivers/common/partition/Partition.h"
#include <gtest/gtest.h>

namespace {
constexpr size_t Page = TargetInfo::getPageSize();

uintptr_t publish(Cache& cache, uintptr_t key) {
  const uintptr_t page = cache.insert(key);
  if (page) {
    *reinterpret_cast<unsigned char*>(page) = 0x57;
    cache.markNoLongerEditing(key);
    cache.markDirty(key);
  }
  return page;
}

class NoStorageDisk : public Disk {
 public:
  bool pin(uint64_t) override {
    return false;
  }
  void unpin(uint64_t) override {}
};

struct Observer {
  Cache* cache = nullptr;
  Cache* lower = nullptr;
  uintptr_t failedKey = ~uintptr_t{0};
  size_t writes = 0;
  bool addPage = false;
  bool recurse = false;
  bool nestedResult = true;
  bool evictionResult = true;
  bool lowerResult = false;

  static bool callback(CacheConstants::CallbackCause cause, uintptr_t key, uintptr_t,
                       void* context) {
    auto& observer = *static_cast<Observer*>(context);
    if (cause != CacheConstants::WriteBack) {
      return true;
    }
    ++observer.writes;
    if (observer.addPage) {
      observer.addPage = false;
      EXPECT_NE(publish(*observer.cache, 2 * Page), 0U);
      observer.evictionResult = observer.cache->evict(Page);
    }
    if (observer.recurse) {
      observer.nestedResult = observer.cache->syncAll();
    }
    if (observer.lower) {
      observer.lowerResult = observer.lower->syncAll();
    }
    return key != observer.failedKey;
  }
};
}  // namespace

TEST(CacheSync, DrainsEveryPageAndRetainsFailureForRetry) {
  Observer observer;
  Cache cache;
  observer.cache = &cache;
  cache.setCallback(Observer::callback, &observer);
  ASSERT_NE(publish(cache, 0), 0U);
  ASSERT_NE(publish(cache, Page), 0U);
  ASSERT_NE(publish(cache, 2 * Page), 0U);
  observer.failedKey = Page;
  EXPECT_FALSE(cache.syncAll());
  EXPECT_EQ(observer.writes, 3U);
  EXPECT_TRUE(cache.exists(0, 3 * Page));
  EXPECT_FALSE(cache.evict(Page));
  EXPECT_TRUE(cache.exists(Page, Page));
  observer.failedKey = ~uintptr_t{0};
  EXPECT_TRUE(cache.syncAll());
  EXPECT_EQ(observer.writes, 7U);
  EXPECT_TRUE(cache.evict(Page));
}

TEST(CacheSync, PinsSnapshotAndExcludesLaterPublication) {
  Observer observer;
  Cache cache;
  observer.cache = &cache;
  observer.addPage = true;
  cache.setCallback(Observer::callback, &observer);
  ASSERT_NE(publish(cache, 0), 0U);
  ASSERT_NE(publish(cache, Page), 0U);
  EXPECT_TRUE(cache.syncAll());
  EXPECT_FALSE(observer.evictionResult);
  EXPECT_EQ(observer.writes, 2U);
  EXPECT_TRUE(cache.exists(2 * Page, Page));
  EXPECT_TRUE(cache.syncAll());
  EXPECT_EQ(observer.writes, 5U);
}

TEST(CacheSync, SkipsUnpublishedEditingDataAndRejectsSameCacheRecursion) {
  Observer observer;
  Cache cache;
  observer.cache = &cache;
  observer.recurse = true;
  cache.setCallback(Observer::callback, &observer);
  ASSERT_NE(cache.insert(0), 0U);
  EXPECT_FALSE(cache.syncAll());
  EXPECT_EQ(observer.writes, 0U);
  cache.markNoLongerEditing(0);
  EXPECT_TRUE(cache.syncAll());
  EXPECT_FALSE(observer.nestedResult);
  EXPECT_EQ(observer.writes, 1U);
}

TEST(CacheSync, CallbackCanDrainIndependentLowerCache) {
  Observer lowerObserver;
  Cache lower;
  lower.setCallback(Observer::callback, &lowerObserver);
  ASSERT_NE(publish(lower, 0), 0U);
  Observer upperObserver;
  upperObserver.lower = &lower;
  Cache upper;
  upper.setCallback(Observer::callback, &upperObserver);
  ASSERT_NE(publish(upper, 0), 0U);
  EXPECT_TRUE(upper.syncAll());
  EXPECT_TRUE(upperObserver.lowerResult);
  EXPECT_EQ(lowerObserver.writes, 1U);
}

TEST(CacheSync, EmptyCacheSucceedsButUnsupportedDiskDoesNot) {
  Cache cache;
  NoStorageDisk disk;
  EXPECT_TRUE(cache.syncAll());
  EXPECT_FALSE(disk.syncAll());
}

TEST(CacheSync, PartitionForwardsWholeDeviceResult) {
  class RecordingDisk final : public NoStorageDisk {
   public:
    bool syncAll() override {
      ++calls;
      return succeeds;
    }
    size_t calls = 0;
    bool succeeds = false;
  } disk;
  Partition partition(String("test"), Page, 4 * Page);
  EXPECT_FALSE(partition.syncAll());
  partition.setParent(&disk);
  EXPECT_FALSE(partition.syncAll());
  EXPECT_EQ(disk.calls, 1U);
  disk.succeeds = true;
  EXPECT_TRUE(partition.syncAll());
  EXPECT_EQ(disk.calls, 2U);
  partition.setParent(nullptr);
}
