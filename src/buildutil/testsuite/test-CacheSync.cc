/* Copyright (c) 2026, Pedigree Developers. */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/machine/Disk.h"
#include "pedigree/kernel/utilities/Cache.h"

#include <functional>
#include <memory>
#include <utility>
#include <vector>

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

#if !THREADS
namespace {
constexpr uint64_t ManagerPeriod = CACHE_WRITEBACK_PERIOD * 1000000ULL;

class ManagerTimerProbe : public Cache {
 public:
  void timer(uint64_t delta) override {
    deltas.push_back(delta);
    auto action = std::move(onTick);
    onTick = nullptr;
    if (action)
      action();
  }

  std::vector<uint64_t> deltas;
  std::function<void()> onTick;
};
}  // namespace

TEST(CacheManagerScan, CoalescesSubperiodTicksAndPreservesTheirElapsedTime) {
  CacheManager& manager = CacheManager::instance();
  manager.timer(ManagerPeriod);
  ManagerTimerProbe probe;
  for (size_t i = 0; i < 499; ++i)
    manager.timer(1000000);
  EXPECT_TRUE(probe.deltas.empty());
  manager.timer(1000000);
  ASSERT_EQ(probe.deltas, std::vector<uint64_t>{ManagerPeriod});
  manager.timer(0);
  manager.timer(ManagerPeriod - 1);
  EXPECT_EQ(probe.deltas.size(), 1U);
  manager.timer(1);
  EXPECT_EQ(probe.deltas, (std::vector<uint64_t>{ManagerPeriod, ManagerPeriod}));
}

TEST(CacheManagerScan, NewCachesReceiveOnlyTimeAfterTheirRegistration) {
  CacheManager& manager = CacheManager::instance();
  manager.timer(ManagerPeriod);
  ManagerTimerProbe older;
  manager.timer(ManagerPeriod / 2);
  ManagerTimerProbe newer;
  manager.timer(ManagerPeriod / 2);
  EXPECT_EQ(older.deltas, std::vector<uint64_t>{ManagerPeriod});
  EXPECT_EQ(newer.deltas, std::vector<uint64_t>{ManagerPeriod / 2});
  manager.timer(ManagerPeriod);
  EXPECT_EQ(newer.deltas, (std::vector<uint64_t>{ManagerPeriod / 2, ManagerPeriod}));
}

TEST(CacheManagerScan, StableIdsSkipRemovalAndDeferAdmissionUntilNextScan) {
  CacheManager& manager = CacheManager::instance();
  manager.timer(ManagerPeriod);
  ManagerTimerProbe first;
  auto removed = std::make_unique<ManagerTimerProbe>();
  ManagerTimerProbe last;
  std::unique_ptr<ManagerTimerProbe> added;
  first.onTick = [&] {
    removed.reset();
    added = std::make_unique<ManagerTimerProbe>();
  };
  manager.timer(ManagerPeriod);
  EXPECT_EQ(first.deltas.size(), 1U);
  EXPECT_EQ(last.deltas.size(), 1U);
  ASSERT_NE(added, nullptr);
  EXPECT_TRUE(added->deltas.empty());
  manager.timer(ManagerPeriod);
  EXPECT_EQ(first.deltas.size(), 2U);
  EXPECT_EQ(last.deltas.size(), 2U);
  EXPECT_EQ(added->deltas, std::vector<uint64_t>{ManagerPeriod});
}

TEST(CacheManagerScan, ClockWrapAndLongIntervalsRemainUsable) {
  CacheManager& manager = CacheManager::instance();
  manager.timer(ManagerPeriod);
  ManagerTimerProbe probe;
  manager.timer(~uint64_t{0});
  manager.timer(~uint64_t{0});
  EXPECT_EQ(probe.deltas, (std::vector<uint64_t>{~uint64_t{0}, ~uint64_t{0}}));
  manager.timer(ManagerPeriod - 1);
  EXPECT_EQ(probe.deltas.size(), 2U);
  manager.timer(1);
  EXPECT_EQ(probe.deltas.back(), ManagerPeriod);
  EXPECT_EQ(probe.deltas.size(), 3U);
}

TEST(CacheManagerScan, DelayedCacheSaturatesElapsedAndRejectsAnOlderScanStamp) {
  CacheManager& manager = CacheManager::instance();
  manager.timer(ManagerPeriod);
  ManagerTimerProbe first;
  ManagerTimerProbe delayed;
  first.onTick = [&] { manager.timer(~uint64_t{0}); };
  manager.timer(ManagerPeriod);
  EXPECT_EQ(first.deltas, (std::vector<uint64_t>{ManagerPeriod, ~uint64_t{0}}));
  EXPECT_EQ(delayed.deltas, std::vector<uint64_t>{~uint64_t{0}});
  manager.timer(ManagerPeriod);
  EXPECT_EQ(delayed.deltas, (std::vector<uint64_t>{~uint64_t{0}, ManagerPeriod}));
}

class CacheManagerShutdown : public ::testing::Test {
 protected:
  void TearDown() override {
    // Each test's retained owners are destroyed before the ordinary manager.
    CacheManager::destroyInstance();
  }
};

TEST_F(CacheManagerShutdown, FlushesDirtyPagesWithoutRevokingRetainedLoans) {
  Observer observer;
  Cache cache;
  cache.setCallback(Observer::callback, &observer);
  const uintptr_t page = publish(cache, 0);
  ASSERT_NE(page, 0U);
  ASSERT_TRUE(cache.pin(0));
  Cache ram;
  ASSERT_NE(publish(ram, 0), 0U);
  CacheManager& manager = CacheManager::instance();
  EXPECT_TRUE(manager.shutdown());
  EXPECT_EQ(observer.writes, 1U);
  EXPECT_TRUE(cache.exists(0, Page));
  EXPECT_EQ(*reinterpret_cast<unsigned char*>(page), 0x57);
  EXPECT_TRUE(ram.exists(0, Page));
  EXPECT_TRUE(manager.shutdown());
  EXPECT_EQ(observer.writes, 1U);
  EXPECT_FALSE(cache.sync(0, true));
  cache.release(0);
}

TEST_F(CacheManagerShutdown, SkipsCleanChecksumPagesAndStopsTimerAdmission) {
  Observer observer;
  observer.failedKey = 0;
  ManagerTimerProbe cache;
  cache.setCallback(Observer::callback, &observer);
  ASSERT_NE(cache.insert(0), 0U);
  cache.markNoLongerEditing(0);
  cache.triggerChecksum(0);
  CacheManager& manager = CacheManager::instance();
  EXPECT_TRUE(manager.shutdown());
  EXPECT_EQ(observer.writes, 0U);
  manager.timer(ManagerPeriod);
  EXPECT_TRUE(cache.deltas.empty());
  EXPECT_FALSE(manager.trimAll());
  observer.failedKey = ~uintptr_t{0};
}

TEST_F(CacheManagerShutdown, ReportsFailureAndStillFlushesOtherRetainedCaches) {
  Observer failed, successful;
  failed.failedKey = 0;
  Cache first, second;
  first.setCallback(Observer::callback, &failed);
  second.setCallback(Observer::callback, &successful);
  ASSERT_NE(publish(first, 0), 0U);
  ASSERT_NE(publish(second, 0), 0U);
  CacheManager& manager = CacheManager::instance();
  EXPECT_FALSE(manager.shutdown());
  EXPECT_EQ(failed.writes, 1U);
  EXPECT_EQ(successful.writes, 1U);
  EXPECT_TRUE(first.exists(0, Page));
  EXPECT_FALSE(manager.shutdown());
  EXPECT_EQ(failed.writes, 1U);
  failed.failedKey = ~uintptr_t{0};
  EXPECT_TRUE(first.syncAll());
}

TEST_F(CacheManagerShutdown, FlushesLowerCacheDirtiedByALaterOwner) {
  Observer lowerObserver;
  Cache lower, upper;
  lower.setCallback(Observer::callback, &lowerObserver);
  ASSERT_NE(lower.insert(0), 0U);
  lower.markNoLongerEditing(0);
  lower.triggerChecksum(0);
  upper.setCallback(
      [](CacheConstants::CallbackCause cause, uintptr_t, uintptr_t, void* context) {
        if (cause == CacheConstants::WriteBack)
          static_cast<Cache*>(context)->markDirty(0);
        return true;
      },
      &lower);
  ASSERT_NE(publish(upper, 0), 0U);
  EXPECT_TRUE(CacheManager::instance().shutdown());
  EXPECT_EQ(lowerObserver.writes, 1U);
}
#endif

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

TEST(CacheSync, TimerWritesEachMutationOnceAndRetainsFailedCompletionForRetry) {
  Observer observer;
  Cache cache;
  cache.setCallback(Observer::callback, &observer);
  const uintptr_t page = cache.insert(0);
  ASSERT_NE(page, 0U);
  auto* bytes = reinterpret_cast<unsigned char*>(page);
  for (size_t i = 0; i < Page; ++i) {
    bytes[i] = 0x57;
  }
  cache.markNoLongerEditing(0);
  const uint64_t period = CACHE_WRITEBACK_PERIOD * 1000000ULL;
  for (size_t i = 0; i < 5; ++i) {
    cache.timer(period);
  }
  EXPECT_EQ(observer.writes, 0U);

  bytes[0] = 0xA6;
  for (size_t i = 0; i < 5; ++i) {
    cache.timer(period);
  }
  EXPECT_EQ(observer.writes, 1U);

  bytes[Page - 1] = 0xC3;
  observer.failedKey = 0;
  cache.timer(period);
  EXPECT_EQ(observer.writes, 1U);
  cache.timer(period);
  EXPECT_EQ(observer.writes, 2U);
  cache.timer(period);
  EXPECT_EQ(observer.writes, 3U);
  observer.failedKey = ~uintptr_t{0};
  cache.timer(period);
  EXPECT_EQ(observer.writes, 4U);
  for (size_t i = 0; i < 5; ++i) {
    cache.timer(period);
  }
  EXPECT_EQ(observer.writes, 4U);
  EXPECT_TRUE(cache.empty());
  EXPECT_EQ(observer.writes, 4U);
}

namespace {
struct TimerCursorObserver {
  Cache* cache = nullptr;
  size_t writes[5] = {};
  bool mutate = true;
  bool removed = false;
  uintptr_t lowerPage = 0;
  uintptr_t higherPage = 0;

  static bool callback(CacheConstants::CallbackCause cause, uintptr_t key, uintptr_t,
                       void* context) {
    auto& observer = *static_cast<TimerCursorObserver*>(context);
    if (cause != CacheConstants::WriteBack) {
      return true;
    }
    const size_t index = key / Page;
    if (index >= 5) {
      ADD_FAILURE() << "Unexpected cache key " << key;
      return false;
    }
    ++observer.writes[index];
    if (key == 2 * Page && observer.mutate) {
      observer.mutate = false;
      observer.removed = observer.cache->evict(3 * Page);
      observer.lowerPage = publish(*observer.cache, 0);
      // Cache keys need not be page-aligned; resuming by page size skips this.
      observer.higherPage = publish(*observer.cache, 2 * Page + 1);
    }
    return true;
  }
};

struct TimerLastKeyObserver {
  size_t writes = 0;

  static bool callback(CacheConstants::CallbackCause cause, uintptr_t key, uintptr_t,
                       void* context) {
    if (cause == CacheConstants::WriteBack) {
      EXPECT_EQ(key, ~uintptr_t{0});
      ++static_cast<TimerLastKeyObserver*>(context)->writes;
    }
    return true;
  }
};
}  // namespace

TEST(CacheSync, TimerResumesAfterCallbackMutationsWithoutSkippingAdjacentKeys) {
  TimerCursorObserver observer;
  Cache cache;
  observer.cache = &cache;
  cache.setCallback(TimerCursorObserver::callback, &observer);
  for (uintptr_t key : {2 * Page, 3 * Page, 4 * Page}) {
    const uintptr_t page = cache.insert(key);
    ASSERT_NE(page, 0U);
    *reinterpret_cast<unsigned char*>(page) = 0x57;
    cache.markNoLongerEditing(key);
  }
  const uint64_t period = CACHE_WRITEBACK_PERIOD * 1000000ULL;
  cache.timer(period);
  cache.markDirty(2 * Page);
  cache.markDirty(4 * Page);

  cache.timer(period);
  EXPECT_TRUE(observer.removed);
  ASSERT_NE(observer.lowerPage, 0U);
  ASSERT_NE(observer.higherPage, 0U);
  EXPECT_EQ(observer.writes[0], 0U);
  EXPECT_EQ(observer.writes[2], 1U);
  EXPECT_EQ(observer.writes[3], 0U);
  EXPECT_EQ(observer.writes[4], 1U);

  // The newly inserted higher key was visited, while the lower key waits for
  // the next pass to leave EditTransition before its dirty data can be queued.
  cache.timer(period);
  EXPECT_EQ(observer.writes[0], 0U);
  EXPECT_EQ(observer.writes[2], 2U);
  EXPECT_EQ(observer.writes[4], 1U);
  cache.timer(period);
  EXPECT_EQ(observer.writes[0], 1U);
  EXPECT_EQ(observer.writes[2], 2U);
  EXPECT_EQ(observer.writes[3], 0U);
  EXPECT_EQ(observer.writes[4], 1U);
  EXPECT_TRUE(cache.empty());
}

TEST(CacheSync, TimerProcessesMaximumKeyWithoutWrappingItsCursor) {
  TimerLastKeyObserver observer;
  Cache cache;
  cache.setCallback(TimerLastKeyObserver::callback, &observer);
  constexpr uintptr_t LastKey = ~uintptr_t{0};
  const uintptr_t page = cache.insert(LastKey);
  ASSERT_NE(page, 0U);
  *reinterpret_cast<unsigned char*>(page) = 0x57;
  cache.markNoLongerEditing(LastKey);
  const uint64_t period = CACHE_WRITEBACK_PERIOD * 1000000ULL;
  cache.timer(period);
  EXPECT_EQ(observer.writes, 0U);
  cache.markDirty(LastKey);
  cache.timer(period);
  EXPECT_EQ(observer.writes, 1U);
  cache.timer(period);
  EXPECT_EQ(observer.writes, 1U);
  EXPECT_TRUE(cache.evict(LastKey));
  EXPECT_EQ(observer.writes, 1U);
}

namespace {
struct BatchObserver {
  Cache* cache = nullptr;
  size_t batches = 0;
  size_t ordinaryWrites = 0;
  bool succeed = true;
  bool ordinarySucceeds = false;
  bool mutate = false;
  unsigned char lastWritten = 0;

  static bool ordinary(CacheConstants::CallbackCause cause, uintptr_t, uintptr_t location,
                       void* context) {
    auto& observer = *static_cast<BatchObserver*>(context);
    if (cause == CacheConstants::WriteBack) {
      ++observer.ordinaryWrites;
      observer.lastWritten = *reinterpret_cast<unsigned char*>(location);
      return observer.ordinarySucceeds;
    }
    return true;
  }

  static bool batch(const Cache::WritebackPage* pages, size_t count, void* context) {
    auto& observer = *static_cast<BatchObserver*>(context);
    ++observer.batches;
    EXPECT_EQ(count, 2U);
    if (count != 2)
      return false;
    uintptr_t keys[2] = {pages[0].key, pages[1].key};
    for (size_t i = 0; i < count; ++i) {
      EXPECT_FALSE(observer.cache->evict(keys[i]));
      const uintptr_t pinned = observer.cache->lookup(keys[i]);
      EXPECT_EQ(pinned, pages[i].location);
      if (pinned)
        observer.cache->release(keys[i]);
    }
    EXPECT_FALSE(observer.cache->syncBatch(keys, count, batch, context));
    if (observer.mutate)
      *reinterpret_cast<unsigned char*>(pages[1].location) = 0xA6;
    return observer.succeed;
  }
};
}  // namespace

TEST(CacheSync, BatchClaimsAllPagesBeforeCallbackAndRetainsCallerPins) {
  BatchObserver observer;
  Cache cache;
  observer.cache = &cache;
  cache.setCallback(BatchObserver::ordinary, &observer);
  constexpr uintptr_t Keys[] = {0, Page};
  ASSERT_NE(publish(cache, Keys[0]), 0U);
  ASSERT_NE(publish(cache, Keys[1]), 0U);
  ASSERT_TRUE(cache.pin(Keys[0]));
  ASSERT_TRUE(cache.syncBatch(Keys, 2, BatchObserver::batch, &observer));
  EXPECT_EQ(observer.batches, 1U);
  EXPECT_EQ(observer.ordinaryWrites, 0U);
  EXPECT_FALSE(cache.evict(Keys[0]));
  EXPECT_TRUE(cache.evict(Keys[1]));
  cache.release(Keys[0]);
  EXPECT_TRUE(cache.evict(Keys[0]));
  EXPECT_EQ(observer.ordinaryWrites, 0U);
}

TEST(CacheSync, FailedBatchKeepsEveryPageRetryableUntilSharedCommitSucceeds) {
  BatchObserver observer;
  Cache cache;
  observer.cache = &cache;
  observer.succeed = false;
  cache.setCallback(BatchObserver::ordinary, &observer);
  constexpr uintptr_t Keys[] = {0, Page};
  ASSERT_NE(publish(cache, Keys[0]), 0U);
  ASSERT_NE(publish(cache, Keys[1]), 0U);
  ASSERT_FALSE(cache.syncBatch(Keys, 2, BatchObserver::batch, &observer));
  EXPECT_FALSE(cache.evict(Keys[0]));
  EXPECT_FALSE(cache.evict(Keys[1]));
  EXPECT_EQ(observer.ordinaryWrites, 2U);
  EXPECT_TRUE(cache.exists(0, 2 * Page));
  observer.succeed = true;
  ASSERT_TRUE(cache.syncBatch(Keys, 2, BatchObserver::batch, &observer));
  EXPECT_EQ(observer.batches, 2U);
  EXPECT_TRUE(cache.evict(Keys[0]));
  EXPECT_TRUE(cache.evict(Keys[1]));
  EXPECT_EQ(observer.ordinaryWrites, 2U);
}

TEST(CacheSync, BatchCompletionDoesNotHideMutationDuringCallback) {
  BatchObserver observer;
  Cache cache;
  observer.cache = &cache;
  observer.mutate = true;
  observer.ordinarySucceeds = true;
  cache.setCallback(BatchObserver::ordinary, &observer);
  constexpr uintptr_t Keys[] = {0, Page};
  ASSERT_NE(publish(cache, Keys[0]), 0U);
  ASSERT_NE(publish(cache, Keys[1]), 0U);
  ASSERT_TRUE(cache.syncBatch(Keys, 2, BatchObserver::batch, &observer));
  for (size_t i = 0; i < 4; ++i)
    cache.timer(CACHE_WRITEBACK_PERIOD * 1000000ULL);
  EXPECT_EQ(observer.ordinaryWrites, 1U);
  EXPECT_EQ(observer.lastWritten, 0xA6);
  EXPECT_TRUE(cache.empty());
  EXPECT_EQ(observer.ordinaryWrites, 1U);
}

TEST(CacheSync, BatchValidatesEveryKeyBeforeInvokingCallback) {
  BatchObserver observer;
  Cache cache;
  observer.cache = &cache;
  cache.setCallback(BatchObserver::ordinary, &observer);
  ASSERT_NE(publish(cache, 0), 0U);
  ASSERT_NE(cache.insert(Page), 0U);
  constexpr uintptr_t Duplicate[] = {0, 0};
  constexpr uintptr_t Missing[] = {0, 2 * Page};
  constexpr uintptr_t Editing[] = {0, Page};
  EXPECT_TRUE(cache.syncBatch(nullptr, 0, nullptr, nullptr));
  EXPECT_FALSE(cache.syncBatch(nullptr, 1, BatchObserver::batch, &observer));
  EXPECT_FALSE(
      cache.syncBatch(Duplicate, Cache::MaxWritebackPages + 1, BatchObserver::batch, &observer));
  EXPECT_FALSE(cache.syncBatch(Duplicate, 2, BatchObserver::batch, &observer));
  EXPECT_FALSE(cache.syncBatch(Missing, 2, BatchObserver::batch, &observer));
  EXPECT_FALSE(cache.syncBatch(Editing, 2, BatchObserver::batch, &observer));
  EXPECT_EQ(observer.batches, 0U);
  EXPECT_EQ(observer.ordinaryWrites, 0U);
  EXPECT_TRUE(cache.discardEditing(Page));
  observer.ordinarySucceeds = true;
  EXPECT_TRUE(cache.evict(0));
}

TEST(CacheSync, CompletedBatchSupersedesQueuedConditionalWritesButNotExplicitSync) {
  BatchObserver observer;
  Cache cache;
  observer.cache = &cache;
  observer.ordinarySucceeds = true;
  cache.setCallback(BatchObserver::ordinary, &observer);
  constexpr uintptr_t Keys[] = {0, Page};
  uintptr_t pages[] = {publish(cache, Keys[0]), publish(cache, Keys[1])};
  ASSERT_NE(pages[0], 0U);
  ASSERT_NE(pages[1], 0U);
  ASSERT_TRUE(cache.syncBatch(Keys, 2, BatchObserver::batch, &observer));
  EXPECT_EQ(cache.executeRequest(0, CacheConstants::WriteBack, Keys[0], pages[0], 0, 0, 1, 0), 2U);
  EXPECT_EQ(cache.executeRequest(0, CacheConstants::WriteBack, Keys[1], pages[1], 0, 0, 1, 0), 2U);
  EXPECT_EQ(observer.ordinaryWrites, 0U);

  *reinterpret_cast<unsigned char*>(pages[1]) = 0xA6;
  EXPECT_EQ(cache.executeRequest(0, CacheConstants::WriteBack, Keys[1], pages[1], 0, 0, 1, 0), 2U);
  EXPECT_EQ(observer.ordinaryWrites, 1U);
  EXPECT_EQ(observer.lastWritten, 0xA6);
  EXPECT_EQ(cache.executeRequest(0, CacheConstants::WriteBack, Keys[1], pages[1], 0, 0, 0, 0), 2U);
  EXPECT_EQ(observer.ordinaryWrites, 2U);
  EXPECT_TRUE(cache.evict(Keys[0]));
  EXPECT_TRUE(cache.evict(Keys[1]));
  EXPECT_EQ(observer.ordinaryWrites, 2U);
}

TEST(CacheSync, ConditionalWritesRetryFailedBatchesAndRejectEditingPages) {
  BatchObserver observer;
  Cache cache;
  observer.cache = &cache;
  observer.succeed = false;
  cache.setCallback(BatchObserver::ordinary, &observer);
  constexpr uintptr_t Keys[] = {0, Page};
  uintptr_t pages[] = {publish(cache, Keys[0]), publish(cache, Keys[1])};
  ASSERT_NE(pages[0], 0U);
  ASSERT_NE(pages[1], 0U);
  ASSERT_FALSE(cache.syncBatch(Keys, 2, BatchObserver::batch, &observer));
  EXPECT_EQ(cache.executeRequest(0, CacheConstants::WriteBack, Keys[0], pages[0], 0, 0, 1, 0), 0U);
  EXPECT_EQ(observer.ordinaryWrites, 1U);
  observer.ordinarySucceeds = true;
  for (size_t i = 0; i < 2; ++i)
    EXPECT_EQ(cache.executeRequest(0, CacheConstants::WriteBack, Keys[i], pages[i], 0, 0, 1, 0),
              2U);
  EXPECT_EQ(observer.ordinaryWrites, 3U);
  cache.markEditing(Keys[1]);
  *reinterpret_cast<unsigned char*>(pages[1]) = 0xB7;
  EXPECT_EQ(cache.executeRequest(0, CacheConstants::WriteBack, Keys[1], pages[1], 0, 0, 1, 0), 0U);
  EXPECT_EQ(observer.ordinaryWrites, 3U);
  EXPECT_TRUE(cache.discardEditing(Keys[1]));
  EXPECT_TRUE(cache.evict(Keys[0]));
  EXPECT_EQ(observer.ordinaryWrites, 3U);
}

namespace {
struct SyncWaveObserver {
  Cache* cache;
  size_t calls = 0, pages = 0;
  bool fail = false, redirty = false;
  static bool batch(const Cache::WritebackPage* pages, size_t count, void* context) {
    auto& self = *static_cast<SyncWaveObserver*>(context);
    ++self.calls;
    self.pages += count;
    EXPECT_LE(count, Cache::MaxWritebackPages);
    if (self.redirty && count) {
      self.redirty = false;
      *reinterpret_cast<unsigned char*>(pages[0].location) ^= 0x42;
      self.cache->markDirty(pages[0].key);
    }
    return !self.fail;
  }
};
}  // namespace

TEST(CacheSync, SnapshotBatchesSkipCleanChecksumPagesAndDetectUnmarkedAliases) {
  Observer ordinary;
  Cache cache;
  cache.setCallback(Observer::callback, &ordinary);
  SyncWaveObserver wave{&cache};
  uintptr_t first = 0;
  for (size_t i = 0; i < Cache::MaxWritebackPages + 1; ++i) {
    uintptr_t page = publish(cache, i * Page);
    ASSERT_NE(page, 0U);
    if (!i)
      first = page;
  }
  ASSERT_TRUE(cache.syncAll(SyncWaveObserver::batch, &wave));
  EXPECT_EQ(wave.calls, 2U);
  EXPECT_EQ(wave.pages, Cache::MaxWritebackPages + 1);
  ASSERT_TRUE(cache.syncAll(SyncWaveObserver::batch, &wave));
  EXPECT_EQ(wave.calls, 2U);
  *reinterpret_cast<unsigned char*>(first) ^= 0x11;
  ASSERT_TRUE(cache.syncAll(SyncWaveObserver::batch, &wave));
  EXPECT_EQ(wave.pages, Cache::MaxWritebackPages + 2);
  EXPECT_EQ(ordinary.writes, 0U);
}

TEST(CacheSync, SnapshotBatchFailureAndNewGenerationRemainRetryable) {
  Observer ordinary;
  Cache cache;
  cache.setDirtyTracking(Cache::DirtyTracking::Explicit);
  cache.setCallback(Observer::callback, &ordinary);
  SyncWaveObserver wave{&cache};
  ASSERT_NE(publish(cache, 0), 0U);
  ASSERT_NE(publish(cache, Page), 0U);
  wave.fail = true;
  ASSERT_FALSE(cache.syncAll(SyncWaveObserver::batch, &wave));
  EXPECT_EQ(wave.pages, 2U);
  wave.fail = false;
  wave.redirty = true;
  ASSERT_TRUE(cache.syncAll(SyncWaveObserver::batch, &wave));
  EXPECT_EQ(wave.pages, 4U);
  ASSERT_TRUE(cache.syncAll(SyncWaveObserver::batch, &wave));
  EXPECT_EQ(wave.pages, 5U);
  ASSERT_TRUE(cache.syncAll(SyncWaveObserver::batch, &wave));
  EXPECT_EQ(wave.pages, 5U);
}

namespace {
struct BackgroundObserver {
  Cache* cache;
  std::vector<size_t> sizes;
  bool succeed = true;
  bool mutate = false;
  size_t single = 0;
  static bool ordinary(CacheConstants::CallbackCause cause, uintptr_t, uintptr_t, void* context) {
    if (cause == CacheConstants::WriteBack)
      ++static_cast<BackgroundObserver*>(context)->single;
    return true;
  }
  static bool batch(const Cache::WritebackPage* pages, size_t count, void* context) {
    auto& self = *static_cast<BackgroundObserver*>(context);
    self.sizes.push_back(count);
    if (self.mutate) {
      self.mutate = false;
      self.cache->markDirty(pages[0].key);
    }
    return self.succeed;
  }
};
}  // namespace

#if !THREADS
TEST(CacheSync, BackgroundUsesBoundedBatchesAndSkipsSettledPages) {
  BackgroundObserver observer{nullptr, {}};
  Cache cache;
  observer.cache = &cache;
  cache.setDirtyTracking(Cache::DirtyTracking::Explicit);
  cache.setCallback(BackgroundObserver::ordinary, &observer);
  cache.setBackgroundWriteback(BackgroundObserver::batch);
  for (size_t i = 0; i < Cache::MaxWritebackPages + 1; ++i)
    ASSERT_NE(publish(cache, i * Page), 0U);
  cache.timer(CACHE_WRITEBACK_PERIOD * 1000000ULL);
  EXPECT_EQ(observer.sizes, (std::vector<size_t>{Cache::MaxWritebackPages, 1}));
  EXPECT_EQ(observer.single, 0U);
  cache.timer(CACHE_WRITEBACK_PERIOD * 1000000ULL);
  EXPECT_EQ(observer.sizes.size(), 2U);
}

TEST(CacheSync, BackgroundFailureAndConcurrentMutationRemainDirty) {
  BackgroundObserver observer{nullptr, {}};
  Cache cache;
  observer.cache = &cache;
  cache.setDirtyTracking(Cache::DirtyTracking::Explicit);
  cache.setCallback(BackgroundObserver::ordinary, &observer);
  cache.setBackgroundWriteback(BackgroundObserver::batch);
  ASSERT_NE(publish(cache, 0), 0U);
  ASSERT_NE(publish(cache, Page), 0U);
  observer.succeed = false;
  cache.timer(CACHE_WRITEBACK_PERIOD * 1000000ULL);
  observer.succeed = true;
  observer.mutate = true;
  cache.timer(CACHE_WRITEBACK_PERIOD * 1000000ULL);
  cache.timer(CACHE_WRITEBACK_PERIOD * 1000000ULL);
  EXPECT_EQ(observer.sizes, (std::vector<size_t>{2, 2, 1}));
  cache.timer(CACHE_WRITEBACK_PERIOD * 1000000ULL);
  EXPECT_EQ(observer.sizes.size(), 3U);
  EXPECT_EQ(observer.single, 0U);
}
#endif

namespace {
class ZeroFallbackDisk : public NoStorageDisk {
 public:
  unsigned char bytes[1024];
  size_t writes = 0, releases = 0;
  size_t getSize() const override {
    return sizeof(bytes);
  }
  BufferView read(uint64_t offset) override {
    if (offset % 512 || offset >= sizeof(bytes))
      return {};
    return BufferView::fromAddress(reinterpret_cast<uintptr_t>(bytes + offset), 512);
  }
  void write(uint64_t) override {
    ++writes;
  }
  void unpin(uint64_t) override {
    ++releases;
  }
};
}  // namespace
TEST(CacheSync, DiskZeroFallbackPreservesNeighboursAndBalancesReferences) {
  ZeroFallbackDisk disk;
  memset(disk.bytes, 0xa5, sizeof disk.bytes);
  ASSERT_TRUE(disk.zero(17, 700));
  for (size_t i = 0; i < sizeof disk.bytes; ++i)
    EXPECT_EQ(disk.bytes[i], i >= 17 && i < 717 ? 0 : 0xa5);
  EXPECT_EQ(disk.writes, 2U);
  EXPECT_EQ(disk.releases, 2U);
  EXPECT_FALSE(disk.zero(1000, 25));
  EXPECT_TRUE(disk.zero(1024, 0));
  EXPECT_EQ(disk.writes, 2U);
}
