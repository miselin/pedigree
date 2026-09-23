/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#define PEDIGREE_EXTERNAL_SOURCE 1

#include "pedigree/kernel/process/RcuReadState.h"

#include <atomic>
#include <thread>

#include <gtest/gtest.h>

namespace {
template <class Predicate>
bool waitUntil(Predicate ready) {
  for (size_t attempt = 0; attempt < 1000000; ++attempt) {
    if (ready()) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

struct PublishedValue {
  explicit PublishedValue(size_t sequence)
      : value(sequence), inverse(~sequence), checksum(sequence * 31 + 17) {}

  bool valid() const {
    return inverse == ~value && checksum == value * 31 + 17;
  }

  const size_t value;
  const size_t inverse;
  const size_t checksum;
};
}  // namespace

TEST(PedigreeRcu, NestedReadersAndLaterGenerations) {
  RcuReadState state;
  const auto idle = state.snapshot();
  EXPECT_FALSE(state.active());

  state.enter();
  const auto outer = state.snapshot();
  EXPECT_TRUE(state.active());
  EXPECT_TRUE(state.passed(idle));
  EXPECT_FALSE(state.passed(outer));

  state.enter();
  const auto inner = state.snapshot();
  state.leave();
  EXPECT_TRUE(state.active());
  EXPECT_FALSE(state.passed(outer));
  EXPECT_FALSE(state.passed(inner));

  state.leave();
  EXPECT_FALSE(state.active());
  EXPECT_TRUE(state.passed(outer));
  EXPECT_TRUE(state.passed(inner));

  state.enter();
  EXPECT_TRUE(state.passed(outer));
  EXPECT_TRUE(state.passed(inner));
  EXPECT_FALSE(state.passed(state.snapshot()));
  state.leave();
}

TEST(PedigreeRcu, DelayedReaderProtectsRetiredPublication) {
  RcuReadState state;
  std::atomic<PublishedValue*> publication(new PublishedValue(17));
  std::atomic<bool> entered(false), release(false);
  std::atomic<size_t> failures(0);
  std::thread reader([&] {
    state.enter();
    const PublishedValue* value = publication.load(std::memory_order_seq_cst);
    entered.store(true, std::memory_order_release);
    if (!waitUntil([&] { return release.load(std::memory_order_acquire); })) {
      failures.fetch_add(1, std::memory_order_relaxed);
    }
    if (!value->valid() || value->value != 17) {
      failures.fetch_add(1, std::memory_order_relaxed);
    }
    state.leave();
  });

  const bool ready = waitUntil([&] { return entered.load(std::memory_order_acquire); });
  EXPECT_TRUE(ready);
  PublishedValue* retired = publication.exchange(new PublishedValue(29), std::memory_order_seq_cst);
  const auto before = state.snapshot();
  if (ready) {
    for (size_t check = 0; check < 1000; ++check) {
      EXPECT_FALSE(state.passed(before));
    }
  }
  release.store(true, std::memory_order_release);
  const bool passed = waitUntil([&] { return state.passed(before); });
  EXPECT_TRUE(passed);
  if (passed) {
    delete retired;
  }
  reader.join();
  if (!passed) {
    delete retired;
  }
  delete publication.load(std::memory_order_seq_cst);
  EXPECT_EQ(failures.load(), 0U);
}

TEST(PedigreeRcu, ReaderExitPublishesProtectedWrites) {
  RcuReadState state;
  size_t protectedWrite = 0;
  std::atomic<bool> entered(false), release(false);
  std::atomic<size_t> failures(0);
  std::thread reader([&] {
    state.enter();
    entered.store(true, std::memory_order_release);
    if (!waitUntil([&] { return release.load(std::memory_order_acquire); })) {
      failures.fetch_add(1, std::memory_order_relaxed);
    }
    protectedWrite = 42;
    state.leave();
  });

  const bool ready = waitUntil([&] { return entered.load(std::memory_order_acquire); });
  EXPECT_TRUE(ready);
  const auto before = state.snapshot();
  release.store(true, std::memory_order_release);
  const bool passed = waitUntil([&] { return state.passed(before); });
  EXPECT_TRUE(passed);
  if (ready && passed) {
    // The grace check, not thread join, must publish the reader's last write.
    EXPECT_EQ(protectedWrite, 42U);
  }
  reader.join();
  EXPECT_EQ(failures.load(), 0U);
}

TEST(PedigreeRcu, FourReadersPublishAndReclaim) {
  constexpr size_t ReaderCount = 4;
  constexpr size_t ReadsPerThread = 20000;
  constexpr size_t Publications = 2000;
  RcuReadState states[ReaderCount];
  std::atomic<PublishedValue*> publication(new PublishedValue(1));
  std::atomic<bool> start(false);
  std::atomic<size_t> ready(0), failures(0);
  size_t reads[ReaderCount] = {};
  std::thread readers[ReaderCount];
  for (size_t cpu = 0; cpu < ReaderCount; ++cpu) {
    readers[cpu] = std::thread([&, cpu] {
      ready.fetch_add(1, std::memory_order_release);
      if (!waitUntil([&] { return start.load(std::memory_order_acquire); })) {
        failures.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      for (size_t iteration = 0; iteration < ReadsPerThread; ++iteration) {
        states[cpu].enter();
        const PublishedValue* value = publication.load(std::memory_order_seq_cst);
        if (!(iteration % 256)) {
          std::this_thread::yield();
        }
        if (!value->valid()) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
        states[cpu].leave();
        ++reads[cpu];
      }
    });
  }

  EXPECT_TRUE(waitUntil([&] { return ready.load(std::memory_order_acquire) == ReaderCount; }));
  start.store(true, std::memory_order_release);
  PublishedValue* delayedRetirement = nullptr;
  size_t reclaimed = 0;
  for (size_t iteration = 0; iteration < Publications; ++iteration) {
    PublishedValue* retired =
        publication.exchange(new PublishedValue(iteration + 2), std::memory_order_seq_cst);
    RcuReadState::Snapshot before[ReaderCount];
    for (size_t cpu = 0; cpu < ReaderCount; ++cpu) {
      before[cpu] = states[cpu].snapshot();
    }
    if (!waitUntil([&] {
          for (size_t cpu = 0; cpu < ReaderCount; ++cpu) {
            if (!states[cpu].passed(before[cpu])) {
              return false;
            }
          }
          return true;
        })) {
      delayedRetirement = retired;
      failures.fetch_add(1, std::memory_order_relaxed);
      break;
    }
    delete retired;
    ++reclaimed;
  }

  for (auto& reader : readers) {
    reader.join();
  }
  delete delayedRetirement;
  delete publication.load(std::memory_order_seq_cst);
  EXPECT_EQ(failures.load(), 0U);
  EXPECT_EQ(reclaimed, Publications);
  for (size_t cpu = 0; cpu < ReaderCount; ++cpu) {
    EXPECT_EQ(reads[cpu], ReadsPerThread);
    EXPECT_FALSE(states[cpu].active());
  }
}
