/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include <gtest/gtest.h>

#include "modules/subsys/posix/IntervalTimerState.h"

using PosixIntervalTimerState::Consumption;
using PosixIntervalTimerState::AbsoluteConsumption;

TEST(IntervalTimerState, LeavesDisarmedAndZeroElapsedTimersAlone)
{
    const Consumption disarmed =
        PosixIntervalTimerState::consume(10, 20, false, 50);
    EXPECT_EQ(disarmed.value, 10U);
    EXPECT_FALSE(disarmed.armed);
    EXPECT_FALSE(disarmed.expired);

    const Consumption zero =
        PosixIntervalTimerState::consume(10, 20, true, 0);
    EXPECT_EQ(zero.value, 10U);
    EXPECT_TRUE(zero.armed);
    EXPECT_FALSE(zero.expired);
}

TEST(IntervalTimerState, CountsDownWithoutExpiring)
{
    const Consumption result =
        PosixIntervalTimerState::consume(11, 7, true, 6);
    EXPECT_EQ(result.value, 5U);
    EXPECT_TRUE(result.armed);
    EXPECT_FALSE(result.expired);
}

TEST(IntervalTimerState, ExpiresOneShotOnExactBoundary)
{
    const Consumption result =
        PosixIntervalTimerState::consume(11, 0, true, 11);
    EXPECT_EQ(result.value, 0U);
    EXPECT_FALSE(result.armed);
    EXPECT_TRUE(result.expired);
}

TEST(IntervalTimerState, ReloadsPeriodicTimerOnExactBoundary)
{
    const Consumption result =
        PosixIntervalTimerState::consume(11, 7, true, 11);
    EXPECT_EQ(result.value, 7U);
    EXPECT_TRUE(result.armed);
    EXPECT_TRUE(result.expired);
}

TEST(IntervalTimerState, RetainsPhaseAcrossCoalescedPeriods)
{
    const Consumption result =
        PosixIntervalTimerState::consume(5, 7, true, 30);
    EXPECT_EQ(result.value, 3U);
    EXPECT_TRUE(result.armed);
    EXPECT_TRUE(result.expired);
}

TEST(IntervalTimerState, StaleAbsoluteSnapshotCannotDebitRearmedTimer)
{
    const AbsoluteConsumption result =
        PosixIntervalTimerState::consumeAbsolute(
            20, 0, true, 200, 150);

    EXPECT_EQ(result.timer.value, 20U);
    EXPECT_TRUE(result.timer.armed);
    EXPECT_FALSE(result.timer.expired);
    EXPECT_EQ(result.baseline, 200U);
}

TEST(IntervalTimerState, AbsoluteSnapshotRetainsPeriodicPhase)
{
    const AbsoluteConsumption result =
        PosixIntervalTimerState::consumeAbsolute(
            5, 7, true, 100, 130);

    EXPECT_EQ(result.timer.value, 3U);
    EXPECT_TRUE(result.timer.armed);
    EXPECT_TRUE(result.timer.expired);
    EXPECT_EQ(result.baseline, 130U);
}
