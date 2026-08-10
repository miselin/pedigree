/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include <atomic>
#include <thread>

#include "system/kernel/machine/mach_pc/TscClock.h"
#include <gtest/gtest.h>

TEST(TscClockConversion, PreservesNonIntegerGigahertzCalibration) {
  const PcTscClock::Calibration twoPointFourGhz(240000000, 100000000);
  const PcTscClock::Calibration onePointNineGhz(190000000, 100000000);

  EXPECT_EQ(PcTscClock::scale(2400000000ULL, twoPointFourGhz), 1000000000U);
  EXPECT_EQ(PcTscClock::scale(1900000000ULL, onePointNineGhz), 1000000000U);
}

TEST(TscClockConversion, RetainsFractionalCalibrationPhase) {
  const PcTscClock::Calibration calibration(240, 100);

  EXPECT_EQ(PcTscClock::scale(0, calibration), 0U);
  EXPECT_EQ(PcTscClock::scale(239, calibration), 99U);
  EXPECT_EQ(PcTscClock::scale(240, calibration), 100U);
  EXPECT_EQ(PcTscClock::scale(360, calibration), 150U);
}

TEST(TscClockConversion, GuardsUnderflowAndSaturatesOverflow) {
  const PcTscClock::Calibration identity(1, 1);
  const PcTscClock::Calibration overflowing(1, PcTscClock::MaximumTimestamp);

  EXPECT_EQ(PcTscClock::fromAnchor(99, 100, 500, identity), 500U);
  EXPECT_EQ(PcTscClock::scale(2, overflowing), PcTscClock::MaximumTimestamp);
  EXPECT_EQ(PcTscClock::fromAnchor(110, 100, PcTscClock::MaximumTimestamp - 4, identity),
            PcTscClock::MaximumTimestamp);
}

TEST(TscClockConversion, PerCpuAnchorsRemoveAbsoluteTscOffsets) {
  const PcTscClock::Calibration calibration(24, 10);

  const uint64_t first = PcTscClock::fromAnchor(1000240, 1000000, 5000, calibration);
  const uint64_t second = PcTscClock::fromAnchor(1240, 1000, 5000, calibration);

  EXPECT_EQ(first, 5100U);
  EXPECT_EQ(second, first);
}

TEST(TscClockPublication, ClampsCrossCpuRegressionWithoutRetrying) {
  PcTscClock::MonotonicPublication publication;

  EXPECT_EQ(publication.publish(1000), 1000U);
  EXPECT_EQ(publication.publish(900), 1000U);
  EXPECT_EQ(publication.publish(1100), 1100U);
  EXPECT_EQ(publication.value(), 1100U);
}

TEST(TscClockPublication, FailedExchangeReturnsPublishedWinner) {
  EXPECT_EQ(PcTscClock::publicationResult(1200, 1100, false), 1100U);
  EXPECT_EQ(PcTscClock::publicationResult(1200, 1100, true), 1200U);
}

TEST(TscClockPublication, ConcurrentPublishersRemainMonotonic) {
  PcTscClock::MonotonicPublication publication;
  constexpr size_t ThreadCount = 4;
  constexpr size_t Samples = 20000;
  std::atomic<bool> start(false);
  std::atomic<size_t> failures(0);
  std::thread publishers[ThreadCount];

  for (size_t thread = 0; thread < ThreadCount; ++thread) {
    publishers[thread] = std::thread([&, thread]() {
      while (!start.load(std::memory_order_acquire)) {
      }

      uint64_t previous = 0;
      for (size_t sample = 1; sample <= Samples; ++sample) {
        const uint64_t candidate = sample * ThreadCount + thread;
        const uint64_t current = publication.publish(candidate);
        if (current < previous) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
        previous = current;
      }
    });
  }

  start.store(true, std::memory_order_release);
  for (size_t thread = 0; thread < ThreadCount; ++thread) {
    publishers[thread].join();
  }

  const uint64_t maximum = Samples * ThreadCount + (ThreadCount - 1);
  EXPECT_EQ(failures.load(std::memory_order_relaxed), 0U);
  EXPECT_LE(publication.value(), maximum);
  EXPECT_EQ(publication.publish(maximum), maximum);
}
