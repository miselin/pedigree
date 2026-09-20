/* Copyright (c) 2026, Pedigree Developers. */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/utilities/Cache.h"

#include <functional>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#if STANDALONE_CACHE && (defined(__unix__) || defined(__APPLE__))
#include <unistd.h>

#include <sys/mman.h>
#endif

namespace {
constexpr size_t Page = TargetInfo::getPageSize();
constexpr uint64_t Period = CACHE_WRITEBACK_PERIOD * 1000000ULL;

uintptr_t fill(Cache& cache, uintptr_t key, bool dirty = false) {
  uintptr_t location = cache.insert(key);
  if (location) {
    auto* bytes = reinterpret_cast<unsigned char*>(location);
    for (size_t i = 0; i < Page; ++i)
      bytes[i] = 0x57;
    cache.markNoLongerEditing(key);
    if (dirty)
      cache.markDirty(key);
  }
  return location;
}

struct Observer {
  Cache* cache = nullptr;
  std::vector<uintptr_t> writes;
  std::vector<unsigned char> writtenBytes;
  std::vector<uintptr_t> batchKeys;
  bool succeed = true;
  std::function<void(uintptr_t, uintptr_t)> onWrite;

  static bool write(CacheConstants::CallbackCause cause, uintptr_t key, uintptr_t location,
                    void* context) {
    auto& observer = *static_cast<Observer*>(context);
    if (cause != CacheConstants::WriteBack)
      return true;
    observer.writes.push_back(key);
    observer.writtenBytes.push_back(*reinterpret_cast<unsigned char*>(location));
    auto action = std::move(observer.onWrite);
    observer.onWrite = nullptr;
    if (action)
      action(key, location);
    return observer.succeed;
  }

  static bool batch(const Cache::WritebackPage* pages, size_t count, void* context) {
    auto& observer = *static_cast<Observer*>(context);
    observer.batchKeys.clear();
    for (size_t i = 0; i < count; ++i) {
      observer.batchKeys.push_back(pages[i].key);
      EXPECT_FALSE(observer.cache->evict(pages[i].key));
      write(CacheConstants::WriteBack, pages[i].key, pages[i].location, context);
    }
    return observer.succeed;
  }
};
}  // namespace

#if STANDALONE_CACHE && (defined(__unix__) || defined(__APPLE__))
TEST(CacheDirtyTracking, CleanExplicitPagesDoNotReadPayloadOnPublicationSyncOrEviction) {
  size_t writes = 0;
  Cache cache;
  cache.setDirtyTracking(Cache::DirtyTracking::Explicit);
  cache.setCallback(
      [](CacheConstants::CallbackCause cause, uintptr_t, uintptr_t, void* context) {
        if (cause == CacheConstants::WriteBack)
          ++*static_cast<size_t*>(context);
        return true;
      },
      &writes);
  const long hostPageSize = sysconf(_SC_PAGESIZE);
  ASSERT_GT(hostPageSize, 0);
  const size_t protectionSize = static_cast<size_t>(hostPageSize);
  ASSERT_EQ(protectionSize % Page, 0U);
  const size_t allocationSize = 2 * protectionSize;
  const uintptr_t location = cache.insert(0, allocationSize);
  ASSERT_NE(location, 0U);
  const uintptr_t protectedLocation =
      ((location + protectionSize - 1) / protectionSize) * protectionSize;
  const uintptr_t key = protectedLocation - location;
  ASSERT_EQ(mprotect(reinterpret_cast<void*>(protectedLocation), protectionSize, PROT_NONE), 0);
  struct RestoreProtection {
    uintptr_t location;
    size_t length;
    ~RestoreProtection() {
      EXPECT_EQ(mprotect(reinterpret_cast<void*>(location), length, PROT_READ | PROT_WRITE), 0);
    }
  } restore{protectedLocation, protectionSize};

  cache.markNoLongerEditing(0, allocationSize);
  cache.triggerChecksum(key);
  cache.timer(Period);
  EXPECT_TRUE(cache.sync(key, false));
  EXPECT_TRUE(cache.syncAll());
  EXPECT_TRUE(cache.syncBatch(
      &key, 1,
      [](const Cache::WritebackPage*, size_t, void*) {
        ADD_FAILURE() << "Clean explicit batch invoked its payload callback";
        return false;
      },
      nullptr));
  EXPECT_TRUE(cache.evict(key));
  EXPECT_EQ(writes, 0U);
}
#endif

TEST(CacheDirtyTracking, OrdinaryCompletionAndEvictionRetainMutationsDuringTheirCallbacks) {
  Observer observer;
  Cache cache;
  observer.cache = &cache;
  cache.setDirtyTracking(Cache::DirtyTracking::Explicit);
  cache.setCallback(Observer::write, &observer);
  ASSERT_NE(fill(cache, 0, true), 0U);
  auto redirty = [&](uintptr_t key, uintptr_t location) {
    *reinterpret_cast<unsigned char*>(location) = 0xA6;
    cache.markDirty(key);
    EXPECT_FALSE(cache.syncAll());
  };
  observer.onWrite = redirty;
  EXPECT_TRUE(cache.syncAll());
  ASSERT_EQ(observer.writes.size(), 1U);
  cache.timer(Period);
  EXPECT_EQ(observer.writtenBytes, (std::vector<unsigned char>{0x57, 0xA6}));
  EXPECT_TRUE(cache.syncAll());
  EXPECT_EQ(observer.writes.size(), 2U);

  cache.markDirty(0);
  observer.onWrite = redirty;
  EXPECT_FALSE(cache.evict(0));
  EXPECT_TRUE(cache.exists(0, Page));
  EXPECT_TRUE(cache.evict(0));
  EXPECT_EQ(observer.writes.size(), 4U);
}

TEST(CacheDirtyTracking, FilteredBatchRetainsFailuresAndRedirtyUntilTheirOwnCompletion) {
  Observer observer;
  Cache cache;
  observer.cache = &cache;
  cache.setDirtyTracking(Cache::DirtyTracking::Explicit);
  cache.setCallback(Observer::write, &observer);
  constexpr uintptr_t Keys[] = {0, Page, 2 * Page};
  for (auto key : Keys)
    ASSERT_NE(fill(cache, key, key != 0), 0U);
  observer.succeed = false;
  EXPECT_FALSE(cache.syncBatch(Keys, 3, Observer::batch, &observer));
  EXPECT_EQ(observer.batchKeys, (std::vector<uintptr_t>{Page, 2 * Page}));
  EXPECT_FALSE(cache.evict(Page));
  EXPECT_TRUE(cache.exists(Page, Page));

  observer.succeed = true;
  observer.onWrite = [&](uintptr_t key, uintptr_t location) {
    *reinterpret_cast<unsigned char*>(location) = 0xB7;
    cache.markDirty(key);
  };
  EXPECT_TRUE(cache.syncBatch(Keys, 3, Observer::batch, &observer));
  EXPECT_EQ(observer.batchKeys, (std::vector<uintptr_t>{Page, 2 * Page}));
  const size_t submitted = observer.writes.size();
  cache.timer(Period);
  ASSERT_EQ(observer.writes.size(), submitted + 1);
  EXPECT_EQ(observer.writes.back(), Page);
  EXPECT_EQ(observer.writtenBytes.back(), 0xB7);
  EXPECT_TRUE(cache.syncBatch(Keys, 3, Observer::batch, &observer));
  EXPECT_EQ(observer.writes.size(), submitted + 1);
  EXPECT_TRUE(cache.empty());
}

TEST(CacheDirtyTracking, ExternalMappingFallbackPreservesDirtyStateAndEndsAtEviction) {
  Observer observer;
  Cache cache;
  cache.setDirtyTracking(Cache::DirtyTracking::Explicit);
  cache.setCallback(Observer::write, &observer);
  uintptr_t location = fill(cache, 0, true);
  ASSERT_NE(location, 0U);
  ASSERT_NE(fill(cache, Page), 0U);
  cache.markExternallyWritable(0);
  cache.timer(Period);
  ASSERT_EQ(observer.writes.size(), 1U);
  *reinterpret_cast<unsigned char*>(location) = 0xA6;
  cache.markExternallyWritable(0);
  cache.timer(Period);
  EXPECT_EQ(observer.writes.size(), 1U);
  cache.timer(Period);
  EXPECT_EQ(observer.writtenBytes, (std::vector<unsigned char>{0x57, 0xA6}));
  EXPECT_TRUE(cache.evict(0));

  location = fill(cache, 0);
  ASSERT_NE(location, 0U);
  *reinterpret_cast<unsigned char*>(location) = 0xB7;
  cache.timer(Period);
  EXPECT_TRUE(cache.syncAll());
  EXPECT_TRUE(cache.evict(0));
  EXPECT_EQ(observer.writes.size(), 2U);
}

TEST(CacheDirtyTracking, StableLookupDistinguishesBusyPagesFromMissesAndPreservesLoans) {
  Observer observer;
  Cache cache;
  cache.setCallback(Observer::write, &observer);
  uintptr_t result = ~uintptr_t{0};
  EXPECT_TRUE(cache.lookupStable(0, result));
  EXPECT_EQ(result, 0U);
  const uintptr_t location = cache.insert(0);
  ASSERT_NE(location, 0U);
  EXPECT_FALSE(cache.lookupStable(0, result));
  EXPECT_EQ(result, 0U);
  cache.markNoLongerEditing(0);
  ASSERT_TRUE(cache.pin(0));
  ASSERT_TRUE(cache.lookupStable(0, result));
  EXPECT_EQ(result, location);
  cache.release(0);
  observer.onWrite = [&](uintptr_t key, uintptr_t) {
    EXPECT_FALSE(cache.lookupStable(key, result));
    EXPECT_EQ(result, 0U);
  };
  EXPECT_TRUE(cache.sync(0, false));
  EXPECT_FALSE(cache.evict(0));
  cache.release(0);
  EXPECT_TRUE(cache.evict(0));
  EXPECT_TRUE(cache.lookupStable(0, result));
  EXPECT_EQ(result, 0U);
}

TEST(CacheDirtyTracking, OverlappingMutableLoansShareTheirBaselineUntilLastReturn) {
  Observer observer;
  Cache cache;
  cache.setDirtyTracking(Cache::DirtyTracking::Explicit);
  cache.setCallback(Observer::write, &observer);
  const uintptr_t location = fill(cache, 0);
  ASSERT_NE(location, 0U);
  ASSERT_EQ(cache.lookup(0), location);
  ASSERT_TRUE(cache.beginMutableLoan(0));
  *reinterpret_cast<unsigned char*>(location) = 0xA6;
  ASSERT_EQ(cache.lookup(0), location);
  ASSERT_TRUE(cache.beginMutableLoan(0));
  cache.endMutableLoan(0);
  cache.release(0);
  EXPECT_TRUE(cache.syncAll());
  EXPECT_EQ(observer.writtenBytes, (std::vector<unsigned char>{0xA6}));

  *reinterpret_cast<unsigned char*>(location) = 0xB7;
  cache.endMutableLoan(0);
  cache.release(0);
  EXPECT_TRUE(cache.syncAll());
  EXPECT_EQ(observer.writtenBytes, (std::vector<unsigned char>{0xA6, 0xB7}));
  EXPECT_TRUE(cache.syncAll());
  EXPECT_EQ(observer.writes.size(), 2U);
}

TEST(CacheDirtyTracking, MutableReturnPreservesKnownDirtyDataAndFailedWriteback) {
  Observer observer;
  Cache cache;
  cache.setDirtyTracking(Cache::DirtyTracking::Explicit);
  cache.setCallback(Observer::write, &observer);
  const uintptr_t location = fill(cache, 0, true);
  ASSERT_NE(location, 0U);
  ASSERT_EQ(cache.lookup(0), location);
  ASSERT_TRUE(cache.beginMutableLoan(0));
  observer.succeed = false;
  EXPECT_FALSE(cache.syncAll());
  cache.endMutableLoan(0);
  cache.release(0);
  observer.succeed = true;
  EXPECT_TRUE(cache.syncAll());
  EXPECT_EQ(observer.writtenBytes, (std::vector<unsigned char>{0x57, 0x57}));
  EXPECT_TRUE(cache.syncAll());
  EXPECT_EQ(observer.writes.size(), 2U);
}

TEST(CacheDirtyTracking, LoanBeginningDuringUntrackedWritebackRetainsLaterRestoration) {
  Observer observer;
  Cache cache;
  cache.setDirtyTracking(Cache::DirtyTracking::Explicit);
  cache.setCallback(Observer::write, &observer);
  const uintptr_t location = fill(cache, 0, true);
  ASSERT_NE(location, 0U);
  observer.onWrite = [&](uintptr_t key, uintptr_t page) {
    ASSERT_EQ(cache.lookup(key), page);
    ASSERT_TRUE(cache.beginMutableLoan(key));
    *reinterpret_cast<unsigned char*>(page) = 0xA6;
    // Model the backend sampling after the loan's write, not at callback entry.
    observer.writtenBytes.back() = 0xA6;
  };
  EXPECT_TRUE(cache.syncAll());
  EXPECT_EQ(observer.writtenBytes, (std::vector<unsigned char>{0xA6}));

  *reinterpret_cast<unsigned char*>(location) = 0x57;
  cache.endMutableLoan(0);
  cache.release(0);
  EXPECT_TRUE(cache.syncAll());
  EXPECT_EQ(observer.writtenBytes, (std::vector<unsigned char>{0xA6, 0x57}));
}

TEST(CacheDirtyTracking, LastLoanReturnCannotBeSettledByAnOlderActiveWriteback) {
  Observer observer;
  Cache cache;
  cache.setDirtyTracking(Cache::DirtyTracking::Explicit);
  cache.setCallback(Observer::write, &observer);
  const uintptr_t location = fill(cache, 0, true);
  ASSERT_NE(location, 0U);
  ASSERT_EQ(cache.lookup(0), location);
  ASSERT_TRUE(cache.beginMutableLoan(0));
  observer.onWrite = [&](uintptr_t key, uintptr_t page) {
    *reinterpret_cast<unsigned char*>(page) = 0xA6;
    observer.writtenBytes.back() = 0xA6;
    *reinterpret_cast<unsigned char*>(page) = 0x57;
    cache.endMutableLoan(key);
    cache.release(key);
  };
  EXPECT_TRUE(cache.syncAll());
  EXPECT_EQ(observer.writtenBytes, (std::vector<unsigned char>{0xA6}));
  EXPECT_TRUE(cache.syncAll());
  EXPECT_EQ(observer.writtenBytes, (std::vector<unsigned char>{0xA6, 0x57}));
}

TEST(CacheDirtyTracking, PermanentWritableMappingOutlivesTemporaryMutableLoan) {
  Observer observer;
  Cache cache;
  cache.setDirtyTracking(Cache::DirtyTracking::Explicit);
  cache.setCallback(Observer::write, &observer);
  const uintptr_t location = fill(cache, 0);
  ASSERT_NE(location, 0U);
  ASSERT_EQ(cache.lookup(0), location);
  ASSERT_TRUE(cache.beginMutableLoan(0));
  *reinterpret_cast<unsigned char*>(location) = 0xA6;
  cache.markExternallyWritable(0);
  cache.endMutableLoan(0);
  cache.release(0);
  EXPECT_TRUE(cache.syncAll());
  EXPECT_EQ(observer.writtenBytes, (std::vector<unsigned char>{0xA6}));

  *reinterpret_cast<unsigned char*>(location) = 0xB7;
  EXPECT_TRUE(cache.syncAll());
  EXPECT_EQ(observer.writtenBytes, (std::vector<unsigned char>{0xA6, 0xB7}));
}

TEST(CacheDirtyTracking, BatchCompletionRetainsChangesAtLastMutableReturn) {
  Observer observer;
  Cache cache;
  observer.cache = &cache;
  cache.setDirtyTracking(Cache::DirtyTracking::Explicit);
  cache.setCallback(Observer::write, &observer);
  const uintptr_t key = 0;
  const uintptr_t location = fill(cache, key, true);
  ASSERT_NE(location, 0U);
  ASSERT_EQ(cache.lookup(key), location);
  ASSERT_TRUE(cache.beginMutableLoan(key));
  observer.onWrite = [&](uintptr_t writtenKey, uintptr_t page) {
    *reinterpret_cast<unsigned char*>(page) = 0xA6;
    cache.endMutableLoan(writtenKey);
    cache.release(writtenKey);
  };
  EXPECT_TRUE(cache.syncBatch(&key, 1, Observer::batch, &observer));
  EXPECT_EQ(observer.writtenBytes, (std::vector<unsigned char>{0x57}));
  EXPECT_TRUE(cache.syncBatch(&key, 1, Observer::batch, &observer));
  EXPECT_EQ(observer.writtenBytes, (std::vector<unsigned char>{0x57, 0xA6}));
}

#if STANDALONE_CACHE && (defined(__unix__) || defined(__APPLE__))
TEST(CacheDirtyTracking, CleanReturnedMutableLoanStopsReadingPayload) {
  size_t writes = 0;
  Cache cache;
  cache.setDirtyTracking(Cache::DirtyTracking::Explicit);
  cache.setCallback(
      [](CacheConstants::CallbackCause cause, uintptr_t, uintptr_t, void* context) {
        if (cause == CacheConstants::WriteBack)
          ++*static_cast<size_t*>(context);
        return true;
      },
      &writes);
  const long hostPageSize = sysconf(_SC_PAGESIZE);
  ASSERT_GT(hostPageSize, 0);
  const size_t protectionSize = static_cast<size_t>(hostPageSize);
  ASSERT_EQ(protectionSize % Page, 0U);
  const size_t allocationSize = 2 * protectionSize;
  const uintptr_t location = cache.insert(0, allocationSize);
  ASSERT_NE(location, 0U);
  const uintptr_t protectedLocation =
      ((location + protectionSize - 1) / protectionSize) * protectionSize;
  const uintptr_t key = protectedLocation - location;
  cache.markNoLongerEditing(0, allocationSize);
  ASSERT_EQ(cache.lookup(key), protectedLocation);
  ASSERT_TRUE(cache.beginMutableLoan(key));
  cache.endMutableLoan(key);
  cache.release(key);

  ASSERT_EQ(mprotect(reinterpret_cast<void*>(protectedLocation), protectionSize, PROT_NONE), 0);
  struct RestoreProtection {
    uintptr_t location;
    size_t length;
    ~RestoreProtection() {
      EXPECT_EQ(mprotect(reinterpret_cast<void*>(location), length, PROT_READ | PROT_WRITE), 0);
    }
  } restore{protectedLocation, protectionSize};
  cache.timer(Period);
  EXPECT_TRUE(cache.sync(key, false));
  EXPECT_TRUE(cache.syncAll());
  EXPECT_TRUE(cache.evict(key));
  EXPECT_EQ(writes, 0U);
}
#endif
