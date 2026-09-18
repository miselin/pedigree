/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */
#define PEDIGREE_EXTERNAL_SOURCE 1

#include "pedigree/kernel/process/PerCpuTimeAccounting.h"

#include <atomic>
#include <thread>

#include <gtest/gtest.h>

TEST(PerCpuTimeAccounting, ZeroAndOverflowHaveNoStorage) {
  const size_t counts[] = {0, ~size_t(0), (~size_t(0) - 63) / 64 + 1};
  for (size_t count : counts) {
    PerCpuTimeAccounting accounting(count);
    EXPECT_FALSE(accounting.add(CpuTimeMode::User, 1, 0));
    EXPECT_FALSE(accounting.add(CpuTimeMode::Kernel, 1, 0));
    EXPECT_EQ(accounting.total(CpuTimeMode::User), 0U);
    EXPECT_EQ(accounting.total(CpuTimeMode::Kernel), 0U);
  }
}

TEST(PerCpuTimeAccounting, SeparatesModesAndIncludesLastSlotWithWideValues) {
  PerCpuTimeAccounting accounting(5);
  constexpr Time::Timestamp Wide = Time::Timestamp(1) << 48;
  EXPECT_EQ(accounting.total(CpuTimeMode::User), 0U);
  EXPECT_EQ(accounting.total(CpuTimeMode::Kernel), 0U);
  EXPECT_TRUE(accounting.add(CpuTimeMode::User, Wide, 0));
  EXPECT_TRUE(accounting.add(CpuTimeMode::User, Wide + 19, 4));
  EXPECT_TRUE(accounting.add(CpuTimeMode::Kernel, Wide + 7, 4));
  EXPECT_TRUE(accounting.add(CpuTimeMode::Kernel, 11, 2));
  EXPECT_FALSE(accounting.add(CpuTimeMode::User, Wide, 5));
  EXPECT_FALSE(accounting.add(CpuTimeMode::Kernel, Wide, ~size_t(0)));
  EXPECT_EQ(accounting.total(CpuTimeMode::User), 2 * Wide + 19);
  EXPECT_EQ(accounting.total(CpuTimeMode::Kernel), Wide + 18);
}

TEST(PerCpuTimeAccounting, IndependentWritersHaveMonotonicAndCompleteTotals) {
  constexpr size_t CpuCount = 4;
  constexpr size_t Iterations = 20000;
  PerCpuTimeAccounting accounting(CpuCount);
  std::atomic<bool> start(false);
  std::atomic<size_t> done(0);
  std::atomic<size_t> failures(0);
  std::thread writers[CpuCount];
  for (size_t cpu = 0; cpu < CpuCount; ++cpu) {
    writers[cpu] = std::thread([&, cpu]() {
      while (!start.load(std::memory_order_acquire)) {
      }
      for (size_t i = 0; i < Iterations; ++i) {
        if (!accounting.add(CpuTimeMode::User, cpu + 1, cpu) ||
            !accounting.add(CpuTimeMode::Kernel, 7 * (cpu + 1), cpu)) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
      }
      done.fetch_add(1, std::memory_order_release);
    });
  }
  start.store(true, std::memory_order_release);
  Time::Timestamp previousUser = 0;
  Time::Timestamp previousKernel = 0;
  bool monotonic = true;
  while (done.load(std::memory_order_acquire) != CpuCount) {
    const Time::Timestamp user = accounting.total(CpuTimeMode::User);
    const Time::Timestamp kernel = accounting.total(CpuTimeMode::Kernel);
    monotonic &= user >= previousUser && kernel >= previousKernel;
    previousUser = user;
    previousKernel = kernel;
  }
  for (auto& writer : writers) {
    writer.join();
  }
  EXPECT_TRUE(monotonic);
  EXPECT_EQ(failures.load(), 0U);
  constexpr Time::Timestamp Weight = CpuCount * (CpuCount + 1) / 2;
  EXPECT_EQ(accounting.total(CpuTimeMode::User), Weight * Iterations);
  EXPECT_EQ(accounting.total(CpuTimeMode::Kernel), 7 * Weight * Iterations);
}

TEST(PerCpuTimeAccounting, NativeFallbackAllowsConcurrentWritersToOneSlot) {
  PerCpuTimeAccounting accounting(1);
  constexpr size_t WriterCount = 4;
  constexpr size_t Iterations = 10000;
  std::atomic<bool> start(false);
  std::thread writers[WriterCount];
  for (auto& writer : writers) {
    writer = std::thread([&]() {
      while (!start.load(std::memory_order_acquire)) {
      }
      for (size_t i = 0; i < Iterations; ++i) {
        accounting.add(CpuTimeMode::User, 1, 0);
        accounting.add(CpuTimeMode::Kernel, 3, 0);
      }
    });
  }
  start.store(true, std::memory_order_release);
  for (auto& writer : writers) {
    writer.join();
  }
  EXPECT_EQ(accounting.total(CpuTimeMode::User), WriterCount * Iterations);
  EXPECT_EQ(accounting.total(CpuTimeMode::Kernel), 3 * WriterCount * Iterations);
}
