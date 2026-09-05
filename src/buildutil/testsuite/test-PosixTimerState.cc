/* Copyright (c) 2026, Pedigree Developers. */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include "modules/subsys/posix/posix-timer-state.h"
#include <gtest/gtest.h>

using namespace PosixTimerState;

TEST(PosixTimerState, CountsMissedPeriodsWithoutChangingPhase) {
  State state{100, 10, true, false};
  EXPECT_EQ(advance(state, 135), 4U);
  EXPECT_EQ(state.deadline, 140U);
  EXPECT_EQ(advance(state, 139), 0U);
  EXPECT_EQ(advance(state, 140), 1U);
  EXPECT_EQ(state.deadline, 150U);
}

TEST(PosixTimerState, OneShotExpiresOnceAtItsDeadline) {
  State state{100, 0, true, false};
  EXPECT_EQ(advance(state, 99), 0U);
  EXPECT_EQ(advance(state, 100), 1U);
  EXPECT_FALSE(state.armed);
  EXPECT_EQ(advance(state, 200), 0U);
}

TEST(PosixTimerState, BackwardClockChangeDoesNotConsumeFuturePeriods) {
  State state{100, 20, true, true};
  EXPECT_EQ(advance(state, 50), 0U);
  EXPECT_EQ(state.deadline, 100U);
  EXPECT_EQ(snapshot(state, 50).value.nanoseconds, 50);
  EXPECT_EQ(advance(state, 140), 3U);
  EXPECT_EQ(state.deadline, 160U);
}

TEST(PosixTimerState, SaturatedDeadlineCannotWrapOrFireForever) {
  State state{MaximumTime - 10, 8, true, false};
  EXPECT_EQ(advance(state, MaximumTime - 1), 2U);
  EXPECT_EQ(state.deadline, MaximumTime);
  EXPECT_EQ(advance(state, MaximumTime), 1U);
  EXPECT_FALSE(state.armed);
  EXPECT_EQ(advance(state, MaximumTime), 0U);
  EXPECT_EQ(add(MaximumTime - 3, 4), MaximumTime);
}

TEST(PosixTimerState, VeryLargeExpirationCountDoesNotOverflow) {
  State state{1, 1, true, false};
  EXPECT_EQ(advance(state, MaximumTime), MaximumTime);
  EXPECT_FALSE(state.armed);
}

TEST(PosixTimerState, RejectsInvalidTimespecAndSaturatesLongDurations) {
  Time::Timestamp value = 0;
  EXPECT_FALSE(decode({-1, 0}, value));
  EXPECT_FALSE(decode({0, -1}, value));
  EXPECT_FALSE(decode({0, 1000000000}, value));
  EXPECT_TRUE(decode({3, 17}, value));
  EXPECT_EQ(value, 3000000017U);
  EXPECT_TRUE(decode({0x7fffffffffffffffLL, 999999999}, value));
  EXPECT_EQ(value, MaximumTime);
}
