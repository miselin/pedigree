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

namespace {
constexpr size_t TimerCleanPages = 4;
constexpr size_t TimerPages = TimerCleanPages + 2;

struct TimerObserver {
  uintptr_t pages[TimerPages] = {};
  size_t writes[TimerPages] = {};
  unsigned char writtenBytes[TimerPages] = {};
  bool disturbPrefix = false;
  bool failRetries = false;
  size_t disturbances = 0;

  static bool callback(CacheConstants::CallbackCause cause, uintptr_t key, uintptr_t page,
                       void* context) {
    auto& observer = *static_cast<TimerObserver*>(context);
    if (cause != CacheConstants::WriteBack) {
      return true;
    }
    const size_t index = key / Page;
    EXPECT_LT(index, TimerPages);
    if (index >= TimerPages) {
      return false;
    }
    ++observer.writes[index];
    observer.writtenBytes[index] = *reinterpret_cast<unsigned char*>(page);
    if (observer.disturbPrefix && index >= TimerCleanPages) {
      // Standalone callbacks run inline between timer scan restarts. Restoring
      // the bytes after the second admission exposes any repeated checksum of
      // a clean prefix as an unnecessary write in the next epoch.
      for (size_t i = 0; i < TimerCleanPages; ++i) {
        *reinterpret_cast<unsigned char*>(observer.pages[i]) ^= 0xA5;
      }
      ++observer.disturbances;
    }
    return !(observer.failRetries && index >= TimerCleanPages);
  }
};
}  // namespace

TEST(CacheSync, TimerChecksCleanPrefixesOncePerEpochAndServicesLaterChanges) {
  TimerObserver observer;
  Cache cache;
  cache.setCallback(TimerObserver::callback, &observer);
  for (size_t i = 0; i < TimerPages; ++i) {
    observer.pages[i] = cache.insert(i * Page);
    ASSERT_NE(observer.pages[i], 0U);
    auto* bytes = reinterpret_cast<unsigned char*>(observer.pages[i]);
    for (size_t j = 0; j < Page; ++j) {
      bytes[j] = 0x57;
    }
    cache.markNoLongerEditing(i * Page);
  }
  const uint64_t period = CACHE_WRITEBACK_PERIOD * 1000000ULL;
  cache.timer(period);
  for (size_t i = 0; i < TimerPages; ++i) {
    EXPECT_EQ(observer.writes[i], 0U);
  }

  cache.markDirty(TimerCleanPages * Page);
  cache.markDirty((TimerCleanPages + 1) * Page);
  observer.failRetries = true;
  observer.disturbPrefix = true;
  cache.timer(period);
  observer.disturbPrefix = false;
  EXPECT_EQ(observer.disturbances, 2U);
  for (size_t i = 0; i < TimerCleanPages; ++i) {
    EXPECT_EQ(*reinterpret_cast<unsigned char*>(observer.pages[i]), 0x57);
    EXPECT_EQ(observer.writes[i], 0U);
  }
  EXPECT_EQ(observer.writes[TimerCleanPages], 1U);
  EXPECT_EQ(observer.writes[TimerCleanPages + 1], 1U);

  cache.timer(period);
  for (size_t i = 0; i < TimerCleanPages; ++i) {
    EXPECT_EQ(observer.writes[i], 0U);
  }
  EXPECT_EQ(observer.writes[TimerCleanPages], 2U);
  EXPECT_EQ(observer.writes[TimerCleanPages + 1], 2U);
  EXPECT_TRUE(cache.exists(0, TimerPages * Page));

  *reinterpret_cast<unsigned char*>(observer.pages[0]) = 0xB6;
  cache.timer(period);
  EXPECT_EQ(observer.writes[0], 0U);
  EXPECT_EQ(observer.writes[TimerCleanPages], 3U);
  EXPECT_EQ(observer.writes[TimerCleanPages + 1], 3U);
  cache.timer(period);
  EXPECT_EQ(observer.writes[0], 1U);
  EXPECT_EQ(observer.writtenBytes[0], 0xB6);
  EXPECT_EQ(observer.writes[TimerCleanPages], 4U);
  EXPECT_EQ(observer.writes[TimerCleanPages + 1], 4U);

  observer.failRetries = false;
  cache.timer(period);
  EXPECT_EQ(observer.writes[TimerCleanPages], 5U);
  EXPECT_EQ(observer.writes[TimerCleanPages + 1], 5U);
  const size_t completedPrefixWrites = observer.writes[0];
  cache.timer(period);
  EXPECT_EQ(observer.writes[0], completedPrefixWrites);
  for (size_t i = 1; i < TimerCleanPages; ++i) {
    EXPECT_EQ(observer.writes[i], 0U);
  }
  EXPECT_EQ(observer.writes[TimerCleanPages], 5U);
  EXPECT_EQ(observer.writes[TimerCleanPages + 1], 5U);
  EXPECT_TRUE(cache.empty());
}
