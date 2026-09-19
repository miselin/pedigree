/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include "pedigree/kernel/SpinlockWord.h"

#include <atomic>
#include <thread>

#include <gtest/gtest.h>

namespace {
template <class Predicate>
bool waitUntil(Predicate ready) {
  for (size_t attempt = 0; attempt < 100000; ++attempt) {
    if (ready()) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}
}  // namespace

TEST(PedigreeSpinlockWord, InitialStateAndCheckedRelease) {
  SpinlockWord lock;
  EXPECT_FALSE(lock.acquired());
  EXPECT_FALSE(lock.releaseChecked());
  ASSERT_TRUE(lock.tryAcquire());
  EXPECT_TRUE(lock.acquired());
  EXPECT_FALSE(lock.tryAcquire());
  EXPECT_TRUE(lock.releaseChecked());
  EXPECT_FALSE(lock.acquired());
  EXPECT_FALSE(lock.releaseChecked());
  ASSERT_TRUE(lock.tryAcquire());
  lock.release();
  EXPECT_FALSE(lock.acquired());

  SpinlockWord initiallyLocked(true);
  EXPECT_TRUE(initiallyLocked.acquired());
  EXPECT_FALSE(initiallyLocked.tryAcquire());
  EXPECT_TRUE(initiallyLocked.releaseChecked());
  EXPECT_FALSE(initiallyLocked.releaseChecked());
  ASSERT_TRUE(initiallyLocked.tryAcquire());
  initiallyLocked.release();
  EXPECT_FALSE(initiallyLocked.acquired());
}

TEST(PedigreeSpinlockWord, ContendersHaveOneWinner) {
  constexpr size_t ThreadCount = 8;
  SpinlockWord lock;
  std::atomic<bool> start(false);
  std::atomic<size_t> winners(0);
  std::atomic<size_t> timedOut(0);
  std::thread workers[ThreadCount];

  for (std::thread& worker : workers) {
    worker = std::thread([&] {
      if (!waitUntil([&] { return start.load(std::memory_order_acquire); })) {
        timedOut.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      if (lock.tryAcquire()) {
        winners.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  start.store(true, std::memory_order_release);
  for (std::thread& worker : workers) {
    worker.join();
  }

  EXPECT_EQ(timedOut.load(), 0U);
  EXPECT_EQ(winners.load(), 1U);
  EXPECT_TRUE(lock.acquired());
  EXPECT_TRUE(lock.releaseChecked());
}

TEST(PedigreeSpinlockWord, ConcurrentCriticalSectionsAreExclusive) {
  constexpr size_t ThreadCount = 4;
  constexpr size_t Iterations = 1000;
  SpinlockWord lock;
  std::atomic<bool> start(false);
  std::atomic<size_t> active(0);
  std::atomic<size_t> completed(0);
  std::atomic<size_t> failures(0);
  std::thread workers[ThreadCount];

  for (std::thread& worker : workers) {
    worker = std::thread([&] {
      if (!waitUntil([&] { return start.load(std::memory_order_acquire); })) {
        failures.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      for (size_t iteration = 0; iteration < Iterations; ++iteration) {
        if (!waitUntil([&] { return lock.tryAcquire(); })) {
          failures.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        if (active.fetch_add(1, std::memory_order_relaxed) != 0) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
        std::this_thread::yield();
        if (active.fetch_sub(1, std::memory_order_relaxed) != 1) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
        completed.fetch_add(1, std::memory_order_relaxed);
        lock.release();
      }
    });
  }
  start.store(true, std::memory_order_release);
  for (std::thread& worker : workers) {
    worker.join();
  }

  EXPECT_EQ(failures.load(), 0U);
  EXPECT_EQ(active.load(), 0U);
  EXPECT_EQ(completed.load(), ThreadCount * Iterations);
  EXPECT_FALSE(lock.acquired());
}

TEST(PedigreeSpinlockWord, ReleasePublishesProtectedData) {
  constexpr size_t Iterations = 1000;
  const bool checkedReleases[] = {false, true};
  for (bool checkedRelease : checkedReleases) {
    SpinlockWord lock;
    bool ready = false;
    size_t payload[8] = {};
    std::atomic<size_t> failures(0);
    std::atomic<size_t> consumed(0);

    auto release = [&] {
      if (checkedRelease) {
        if (!lock.releaseChecked()) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
      } else {
        lock.release();
      }
    };

    // Only the lock publishes ready and payload; thread joins happen afterward.
    std::thread writer([&] {
      for (size_t sequence = 1; sequence <= Iterations; ++sequence) {
        if (!waitUntil([&] {
              if (!lock.tryAcquire()) {
                return false;
              }
              if (ready) {
                release();
                return false;
              }
              for (size_t field = 0; field < 8; ++field) {
                payload[field] = sequence * 8 + field;
              }
              ready = true;
              release();
              return true;
            })) {
          failures.fetch_add(1, std::memory_order_relaxed);
          return;
        }
      }
    });
    std::thread reader([&] {
      for (size_t sequence = 1; sequence <= Iterations; ++sequence) {
        if (!waitUntil([&] {
              if (!lock.tryAcquire()) {
                return false;
              }
              if (!ready) {
                release();
                return false;
              }
              for (size_t field = 0; field < 8; ++field) {
                if (payload[field] != sequence * 8 + field) {
                  failures.fetch_add(1, std::memory_order_relaxed);
                }
              }
              ready = false;
              consumed.fetch_add(1, std::memory_order_relaxed);
              release();
              return true;
            })) {
          failures.fetch_add(1, std::memory_order_relaxed);
          return;
        }
      }
    });
    writer.join();
    reader.join();

    EXPECT_EQ(failures.load(), 0U) << "checked release: " << checkedRelease;
    EXPECT_EQ(consumed.load(), Iterations) << "checked release: " << checkedRelease;
    EXPECT_FALSE(ready);
    EXPECT_FALSE(lock.acquired());
  }
}
