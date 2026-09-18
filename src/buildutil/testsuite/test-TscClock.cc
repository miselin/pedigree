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

namespace {
uint64_t referenceScale(uint64_t cycles, const PcTscClock::Calibration& calibration) {
  if (!calibration.cycles) {
    return 0;
  }
  const unsigned __int128 product =
      static_cast<unsigned __int128>(cycles) * calibration.nanoseconds;
  const unsigned __int128 quotient = product / calibration.cycles;
  return quotient > PcTscClock::MaximumTimestamp ? PcTscClock::MaximumTimestamp
                                                 : static_cast<uint64_t>(quotient);
}
}  // namespace

TEST(TscClockConversion, WideDivisionMatchesReferenceAtBoundaries) {
  constexpr uint64_t Maximum = PcTscClock::MaximumTimestamp;
  constexpr uint64_t HalfRange = uint64_t(1) << 63;
  struct Case {
    uint64_t cycles;
    uint64_t numerator;
    uint64_t denominator;
  };
  const Case cases[] = {
      {0, 0, 0},
      {Maximum, Maximum, 0},
      {0, Maximum, 1},
      {Maximum, 0, 1},
      {Maximum, 1, 1},
      {Maximum, 1, Maximum},
      {Maximum, Maximum, Maximum},
      // Adjacent products have high limbs denominator - 1 and denominator.
      {HalfRange, Maximum, HalfRange},
      {HalfRange + 1, Maximum, HalfRange},
      {3, HalfRange, 2},
      {4, HalfRange, 2},
      {239, 100, 240},
      {240, 100, 240},
      {241, 100, 240},
      {360, 100, 240},
      {Maximum - 1, Maximum, Maximum},
      {Maximum, Maximum, 1},
  };
  for (const Case& test : cases) {
    SCOPED_TRACE(test.cycles);
    SCOPED_TRACE(test.numerator);
    SCOPED_TRACE(test.denominator);
    const PcTscClock::Calibration calibration(test.denominator, test.numerator);
    EXPECT_EQ(PcTscClock::scale(test.cycles, calibration),
              referenceScale(test.cycles, calibration));
  }
}

TEST(TscClockConversion, WideDivisionMatchesDeterministicReferenceSweep) {
  uint64_t seed = 0x123456789abcdef0ULL;
  for (size_t sample = 0; sample < 256; ++sample) {
    SCOPED_TRACE(sample);
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    const uint64_t cycles = seed;
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    const uint64_t numerator = seed;
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    const PcTscClock::Calibration calibration(seed, numerator);
    EXPECT_EQ(PcTscClock::scale(cycles, calibration), referenceScale(cycles, calibration));
  }
}

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
