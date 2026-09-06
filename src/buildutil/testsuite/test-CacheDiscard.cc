/* Copyright (c) 2026, Pedigree Developers. */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/TargetInfo.h"
#include "pedigree/kernel/utilities/Cache.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

#include <condition_variable>
#include <gtest/gtest.h>

namespace {
using Status = Cache::DiscardStatus;
const size_t Page = TargetInfo::getPageSize();

struct Observer {
  size_t writes = 0;
  size_t evictions = 0;
  static bool callback(CacheConstants::CallbackCause cause, uintptr_t, uintptr_t, void* context) {
    auto& observer = *static_cast<Observer*>(context);
    if (cause == CacheConstants::WriteBack) {
      ++observer.writes;
      return false;
    }
    if (cause == CacheConstants::Eviction) {
      ++observer.evictions;
    }
    return true;
  }
};

uintptr_t publish(Cache& cache, uintptr_t key, unsigned char byte = 0x57) {
  const uintptr_t page = cache.insert(key);
  if (page) {
    *reinterpret_cast<unsigned char*>(page) = byte;
    cache.markNoLongerEditing(key);
  }
  return page;
}

class Gate {
 public:
  void open() {
    std::lock_guard<std::mutex> guard(m_Mutex);
    m_Open = true;
    m_Condition.notify_all();
  }
  void require() {
    std::unique_lock<std::mutex> guard(m_Mutex);
    if (!m_Condition.wait_for(guard, std::chrono::seconds(2), [this] { return m_Open; })) {
      ADD_FAILURE() << "Cache discard worker exceeded its bounded completion deadline";
      std::abort();
    }
  }

 private:
  std::mutex m_Mutex;
  std::condition_variable m_Condition;
  bool m_Open = false;
};
}  // namespace

TEST(CacheDiscard, CancelReopensSuffixAndPreservesPrefixReferences) {
  Cache cache;
  const uintptr_t prefix = publish(cache, 0, 0x12);
  const uintptr_t suffix = publish(cache, Page, 0x34);
  ASSERT_NE(prefix, 0U);
  ASSERT_NE(suffix, 0U);
  ASSERT_EQ(cache.lookup(0), prefix);
  ASSERT_EQ(cache.lookup(Page), suffix);
  ASSERT_TRUE(cache.pin(Page));
  const Cache::DiscardReference references[] = {{0, 1}, {Page, 2}};
  UniquePointer<Cache::PreparedDiscard> plan;
  ASSERT_EQ(cache.prepareDiscardFrom(Page, references, 2, plan), Status::Ready);
  EXPECT_EQ(cache.lookup(Page), 0U);
  EXPECT_FALSE(cache.pin(Page));
  EXPECT_TRUE(cache.exists(Page, Page));
  EXPECT_EQ(cache.lookup(0), prefix);
  cache.release(0);
  plan.reset();
  EXPECT_EQ(cache.lookup(Page), suffix);
  EXPECT_EQ(*reinterpret_cast<unsigned char*>(suffix), 0x34);
  cache.release(Page);
  cache.release(Page);
  cache.release(Page);
  cache.release(0);
}

TEST(CacheDiscard, CommitDiscardsDirtySuffixWithoutWriteback) {
  Observer observer;
  Cache cache;
  cache.setCallback(Observer::callback, &observer);
  const uintptr_t prefix = publish(cache, 0);
  ASSERT_NE(prefix, 0U);
  ASSERT_NE(publish(cache, Page), 0U);
  ASSERT_NE(publish(cache, 2 * Page), 0U);
  ASSERT_TRUE(cache.pin(0));
  ASSERT_TRUE(cache.pin(Page));
  ASSERT_TRUE(cache.pin(2 * Page));
  cache.markDirty(Page);
  cache.markDirty(2 * Page);
  const Cache::DiscardReference references[] = {{0, 1}, {Page, 1}, {2 * Page, 1}};
  UniquePointer<Cache::PreparedDiscard> plan;
  ASSERT_EQ(cache.prepareDiscardFrom(Page, references, 3, plan), Status::Ready);
  EXPECT_FALSE(cache.sync(Page, false));
  cache.release(Page);
  cache.release(2 * Page);
  plan.get()->commit();
  plan.reset();
  EXPECT_FALSE(cache.exists(Page, Page));
  EXPECT_FALSE(cache.exists(2 * Page, Page));
  EXPECT_EQ(observer.writes, 0U);
  EXPECT_EQ(observer.evictions, 2U);
  EXPECT_EQ(cache.lookup(0), prefix);
  cache.release(0);
  cache.release(0);
  // Remove the retained clean prefix before its deliberately failing callback
  // can turn unrelated final teardown into a writeback failure.
  ASSERT_EQ(cache.prepareDiscardFrom(0, nullptr, 0, plan), Status::Ready);
  plan.get()->commit();
}

TEST(CacheDiscard, UnexpectedLateReferenceRollsBackEarlierAdmission) {
  Cache cache;
  const uintptr_t first = publish(cache, Page);
  const uintptr_t second = publish(cache, 2 * Page);
  ASSERT_NE(first, 0U);
  ASSERT_NE(second, 0U);
  ASSERT_TRUE(cache.pin(Page));
  ASSERT_TRUE(cache.pin(2 * Page));
  ASSERT_TRUE(cache.pin(2 * Page));
  const Cache::DiscardReference references[] = {{Page, 1}, {2 * Page, 1}};
  UniquePointer<Cache::PreparedDiscard> plan;
  EXPECT_EQ(cache.prepareDiscardFrom(Page, references, 2, plan), Status::Busy);
  EXPECT_FALSE(plan);
  EXPECT_EQ(cache.lookup(Page), first);
  EXPECT_EQ(cache.lookup(2 * Page), second);
  cache.release(Page);
  cache.release(Page);
  cache.release(2 * Page);
  cache.release(2 * Page);
  cache.release(2 * Page);
}

TEST(CacheDiscard, MissingAndMalformedLoansLeaveAdmissionUnchanged) {
  Cache cache;
  const uintptr_t first = publish(cache, Page);
  ASSERT_NE(first, 0U);
  UniquePointer<Cache::PreparedDiscard> plan;
  const Cache::DiscardReference missing[] = {{2 * Page, 1}};
  EXPECT_EQ(cache.prepareDiscardFrom(Page, missing, 1, plan), Status::Invalid);
  EXPECT_EQ(cache.lookup(Page), first);
  cache.release(Page);
  const Cache::DiscardReference reversed[] = {{2 * Page, 0}, {Page, 0}};
  EXPECT_EQ(cache.prepareDiscardFrom(Page, reversed, 2, plan), Status::Invalid);
  EXPECT_EQ(cache.prepareDiscardFrom(Page + 1, nullptr, 0, plan), Status::Invalid);
  EXPECT_EQ(cache.prepareDiscardFrom(Page, nullptr, 1, plan), Status::Invalid);
  EXPECT_EQ(cache.lookup(Page), first);
  cache.release(Page);
}

TEST(CacheDiscard, EditingAndOtherPreparedPlansAreBusy) {
  Cache cache;
  ASSERT_NE(cache.insert(Page), 0U);
  UniquePointer<Cache::PreparedDiscard> first;
  UniquePointer<Cache::PreparedDiscard> second;
  EXPECT_EQ(cache.prepareDiscardFrom(Page, nullptr, 0, first), Status::Busy);
  cache.markNoLongerEditing(Page);
  ASSERT_EQ(cache.prepareDiscardFrom(Page, nullptr, 0, first), Status::Ready);
  EXPECT_EQ(cache.prepareDiscardFrom(Page, nullptr, 0, second), Status::Busy);
  first.reset();
  ASSERT_EQ(cache.prepareDiscardFrom(Page, nullptr, 0, second), Status::Ready);
  second.get()->commit();
}

TEST(CacheDiscard, EmptySuffixAndClosedCacheHaveDistinctResults) {
  Cache cache;
  ASSERT_NE(publish(cache, 0), 0U);
  UniquePointer<Cache::PreparedDiscard> plan;
  ASSERT_EQ(cache.prepareDiscardFrom(Page, nullptr, 0, plan), Status::Ready);
  plan.get()->commit();
  plan.reset();
  ASSERT_TRUE(cache.shutdown());
  EXPECT_EQ(cache.prepareDiscardFrom(0, nullptr, 0, plan), Status::Closed);
}

TEST(CacheDiscard, ActualQueuePinsAreReleasedAfterCallbackFailure) {
  struct CallbackState {
    Gate entered;
    Gate finish;
    static bool callback(CacheConstants::CallbackCause cause, uintptr_t, uintptr_t, void* context) {
      auto& state = *static_cast<CallbackState*>(context);
      if (cause == CacheConstants::WriteBack) {
        state.entered.open();
        state.finish.require();
        return false;
      }
      return true;
    }
  } state;
  Cache cache;
  cache.setCallback(CallbackState::callback, &state);
  ASSERT_NE(publish(cache, Page), 0U);
  ASSERT_TRUE(cache.pin(Page));
  Gate done;
  std::atomic<bool> syncResult{true};
  std::thread writer([&] {
    syncResult = cache.sync(Page, false);
    done.open();
  });
  state.entered.require();
  const Cache::DiscardReference reference{Page, 1};
  UniquePointer<Cache::PreparedDiscard> plan;
#if !THREADS
  // The native Cache runs queue work inline and has no kernel WaitQueue.
  EXPECT_EQ(cache.prepareDiscardFrom(Page, &reference, 1, plan), Status::Busy);
  EXPECT_TRUE(cache.pin(Page));
  cache.release(Page);
#endif
  state.finish.open();
  done.require();
  writer.join();
  EXPECT_FALSE(syncResult.load());
  ASSERT_EQ(cache.prepareDiscardFrom(Page, &reference, 1, plan), Status::Ready);
  cache.release(Page);
  plan.get()->commit();
}

TEST(CacheDiscard, LastWritebackReferenceStillSchedulesOrdinaryEviction) {
  struct CallbackState {
    Gate entered;
    Gate finish;
    std::atomic<size_t> evictions{0};
    static bool callback(CacheConstants::CallbackCause cause, uintptr_t, uintptr_t, void* context) {
      auto& state = *static_cast<CallbackState*>(context);
      if (cause == CacheConstants::WriteBack) {
        state.entered.open();
        state.finish.require();
      } else if (cause == CacheConstants::Eviction) {
        ++state.evictions;
      }
      return true;
    }
  } state;
  Cache cache;
  cache.setCallback(CallbackState::callback, &state);
  ASSERT_NE(publish(cache, Page), 0U);
  Gate done;
  std::atomic<bool> syncResult{false};
  std::thread writer([&] {
    syncResult = cache.sync(Page, false);
    done.open();
  });
  state.entered.require();
  cache.release(Page);
  state.finish.open();
  done.require();
  writer.join();
  EXPECT_TRUE(syncResult.load());
  EXPECT_FALSE(cache.exists(Page, Page));
  EXPECT_EQ(state.evictions.load(), 1U);
}
