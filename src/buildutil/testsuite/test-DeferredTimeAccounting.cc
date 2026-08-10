/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include "pedigree/kernel/process/DeferredTimeAccounting.h"

#include <atomic>
#include <thread>

#include <gtest/gtest.h>

TEST(ThreadTimeAccounting, IgnoresUninitialisedAndBackwardBaselines) {
  ThreadTimeAccounting accounting;

  EXPECT_EQ(accounting.elapsed(CpuTimeMode::User, 100), 0U);
  accounting.record(CpuTimeMode::User, 200);
  EXPECT_EQ(accounting.elapsed(CpuTimeMode::User, 199), 0U);
  EXPECT_EQ(accounting.elapsed(CpuTimeMode::User, 205), 5U);
}

TEST(ThreadTimeAccounting, StaleRecordCannotRegressBaseline) {
  ThreadTimeAccounting accounting;

  accounting.record(CpuTimeMode::Kernel, 200);
  accounting.record(CpuTimeMode::Kernel, 250);
  accounting.record(CpuTimeMode::Kernel, 225);

  EXPECT_EQ(accounting.elapsed(CpuTimeMode::Kernel, 260), 10U);
}

TEST(ThreadTimeAccounting, FreshUserspaceThreadKeepsItsFirstSlice) {
  ThreadTimeAccounting accounting;

  accounting.record(CpuTimeMode::User, 100);

  EXPECT_EQ(accounting.elapsed(CpuTimeMode::User, 145), 45U);
}

TEST(ThreadTimeAccounting, FreshKernelThreadKeepsItsFirstSlice) {
  ThreadTimeAccounting accounting;

  accounting.record(CpuTimeMode::Kernel, 100);

  EXPECT_EQ(accounting.elapsed(CpuTimeMode::Kernel, 145), 45U);
}

TEST(ThreadTimeAccounting, MigrationStartsFreshDestinationBaseline) {
  ThreadTimeAccounting accounting;

  accounting.record(CpuTimeMode::User, 500, 0);
  EXPECT_EQ(accounting.elapsed(CpuTimeMode::User, 550, 0), 50U);

  EXPECT_EQ(accounting.elapsed(CpuTimeMode::User, 100, 1), 0U);
  EXPECT_EQ(accounting.elapsed(CpuTimeMode::User, 125, 1), 25U);

  accounting.record(CpuTimeMode::User, 600, 0);
  EXPECT_EQ(accounting.elapsed(CpuTimeMode::User, 610, 0), 10U);
}

TEST(ThreadTimeAccounting, KeepsThreadsAndModesIndependent) {
  ThreadTimeAccounting first;
  ThreadTimeAccounting second;

  first.record(CpuTimeMode::User, 100);
  second.record(CpuTimeMode::User, 105);
  first.record(CpuTimeMode::Kernel, 200);
  second.record(CpuTimeMode::Kernel, 220);

  EXPECT_EQ(first.elapsed(CpuTimeMode::User, 130), 30U);
  EXPECT_EQ(second.elapsed(CpuTimeMode::User, 145), 40U);
  EXPECT_EQ(first.elapsed(CpuTimeMode::Kernel, 250), 50U);
  EXPECT_EQ(second.elapsed(CpuTimeMode::Kernel, 300), 80U);
}

TEST(ThreadTimeAccounting, UserKernelUserTransitionIsExact) {
  ThreadTimeAccounting accounting;
  const bool fromUserspace = true;
  Time::Timestamp user = 0;
  Time::Timestamp kernel = 0;

  accounting.record(CpuTimeMode::User, 100);
  user += accounting.elapsed(KernelTimeTransition::interrupted(fromUserspace), 130);
  accounting.record(KernelTimeTransition::handler(), 130);
  kernel += accounting.elapsed(KernelTimeTransition::handler(), 170);
  accounting.record(KernelTimeTransition::resumed(fromUserspace), 170);
  user += accounting.elapsed(CpuTimeMode::User, 200);

  EXPECT_EQ(user, 60U);
  EXPECT_EQ(kernel, 40U);
}

TEST(ThreadTimeAccounting, KernelInterruptRemainsKernel) {
  ThreadTimeAccounting accounting;
  const bool fromUserspace = false;
  Time::Timestamp user = 0;
  Time::Timestamp kernel = 0;

  accounting.record(CpuTimeMode::Kernel, 100);
  kernel += accounting.elapsed(KernelTimeTransition::interrupted(fromUserspace), 130);
  accounting.record(KernelTimeTransition::handler(), 130);
  kernel += accounting.elapsed(KernelTimeTransition::handler(), 170);
  accounting.record(KernelTimeTransition::resumed(fromUserspace), 170);
  kernel += accounting.elapsed(CpuTimeMode::Kernel, 200);

  EXPECT_EQ(user, 0U);
  EXPECT_EQ(kernel, 100U);
}

TEST(ThreadTimeAccounting, NestedKernelInterruptsRemainKernel) {
  ThreadTimeAccounting accounting;
  Time::Timestamp kernel = 0;

  accounting.record(CpuTimeMode::Kernel, 100);
  kernel += accounting.elapsed(KernelTimeTransition::interrupted(false), 120);
  accounting.record(KernelTimeTransition::handler(), 120);

  kernel += accounting.elapsed(KernelTimeTransition::interrupted(false), 150);
  accounting.record(KernelTimeTransition::handler(), 150);
  kernel += accounting.elapsed(KernelTimeTransition::handler(), 170);
  accounting.record(KernelTimeTransition::resumed(false), 170);

  kernel += accounting.elapsed(KernelTimeTransition::handler(), 200);
  accounting.record(KernelTimeTransition::resumed(false), 200);

  EXPECT_EQ(kernel, 100U);
}

TEST(DeferredTimeAccounting, StartsEmpty) {
  DeferredTimeAccounting accounting;

  EXPECT_FALSE(accounting.pending());
  EXPECT_FALSE(accounting.take());
}

TEST(DeferredTimeAccounting, CoalescesRepeatedPublicationsIntoOneEdge) {
  DeferredTimeAccounting accounting;

  EXPECT_TRUE(accounting.publish(13));
  EXPECT_FALSE(accounting.publish(7));
  EXPECT_FALSE(accounting.publish(5));
  EXPECT_FALSE(accounting.publish(0));

  EXPECT_TRUE(accounting.pending());
  EXPECT_TRUE(accounting.take());
  EXPECT_FALSE(accounting.pending());
}

TEST(DeferredTimeAccounting, RearmsAfterConsumerTake) {
  DeferredTimeAccounting accounting;

  ASSERT_TRUE(accounting.publish(4));
  ASSERT_TRUE(accounting.take());
  ASSERT_TRUE(accounting.publish(6));

  EXPECT_TRUE(accounting.take());
  EXPECT_FALSE(accounting.pending());
}

TEST(DeferredTimeAccounting, ConcurrentPublishersProduceOneEdge) {
  DeferredTimeAccounting accounting;
  std::atomic<size_t> edges(0);
  constexpr size_t PublisherCount = 4;
  constexpr size_t PublicationsPerThread = 10000;
  std::thread publishers[PublisherCount];

  for (size_t i = 0; i < PublisherCount; ++i) {
    publishers[i] = std::thread([&accounting, &edges, i]() {
      for (size_t n = 0; n < PublicationsPerThread; ++n) {
        if (accounting.publish(i + 1)) {
          edges.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (size_t i = 0; i < PublisherCount; ++i) {
    publishers[i].join();
  }

  EXPECT_EQ(edges.load(std::memory_order_relaxed), 1U);
  EXPECT_TRUE(accounting.take());
  EXPECT_FALSE(accounting.pending());
}

TEST(DeferredTimeAccountingWorkerState, StartsCaughtUpAndIdle) {
  DeferredTimeAccountingWorkerState state;

  EXPECT_TRUE(state.caughtUp());
  EXPECT_FALSE(state.ready());
  EXPECT_TRUE(state.ready(true));
}

TEST(DeferredTimeAccountingWorkerState, CompletesSnapshottedGeneration) {
  DeferredTimeAccountingWorkerState state;

  state.publish();
  state.publish();
  EXPECT_TRUE(state.ready());
  const size_t generation = state.beginBatch();
  EXPECT_TRUE(state.ready());
  state.finishBatch(generation);

  EXPECT_TRUE(state.caughtUp());
  EXPECT_FALSE(state.ready());
}

TEST(DeferredTimeAccountingWorkerState, RacingPublicationRemainsReady) {
  DeferredTimeAccountingWorkerState state;

  state.publish();
  const size_t firstGeneration = state.beginBatch();
  state.publish();
  state.finishBatch(firstGeneration);

  EXPECT_FALSE(state.caughtUp());
  EXPECT_TRUE(state.ready());

  const size_t secondGeneration = state.beginBatch();
  state.finishBatch(secondGeneration);
  EXPECT_TRUE(state.caughtUp());
  EXPECT_FALSE(state.ready());
}
