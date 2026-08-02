/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include <gtest/gtest.h>

#include "modules/drivers/x86/vmware-gfx/VmwareSvgaState.h"

using VmwareSvgaState::FifoLayout;
using VmwareSvgaState::PollBudget;

TEST(VmwareSvgaState, EmptyFifoReservesOneWord)
{
    const FifoLayout layout = {16, 64, 16, 16};
    EXPECT_TRUE(VmwareSvgaState::valid(layout));
    EXPECT_EQ(VmwareSvgaState::freeBytes(layout), 44U);
    EXPECT_TRUE(VmwareSvgaState::canFit(layout, 11));
    EXPECT_FALSE(VmwareSvgaState::canFit(layout, 12));
}

TEST(VmwareSvgaState, DetectsFullFifoAtWrapBoundary)
{
    const FifoLayout layout = {16, 64, 60, 16};
    EXPECT_TRUE(VmwareSvgaState::valid(layout));
    EXPECT_EQ(VmwareSvgaState::freeBytes(layout), 0U);
    EXPECT_FALSE(VmwareSvgaState::canFit(layout, 1));
}

TEST(VmwareSvgaState, ComputesSpaceAcrossRingWrap)
{
    const FifoLayout layout = {16, 64, 56, 32};
    EXPECT_EQ(VmwareSvgaState::freeBytes(layout), 20U);
    EXPECT_TRUE(VmwareSvgaState::canFit(layout, 5));
    EXPECT_FALSE(VmwareSvgaState::canFit(layout, 6));
}

TEST(VmwareSvgaState, RejectsMalformedHardwareCursors)
{
    EXPECT_FALSE(VmwareSvgaState::valid({16, 64, 18, 16}));
    EXPECT_FALSE(VmwareSvgaState::valid({16, 64, 64, 16}));
    EXPECT_FALSE(VmwareSvgaState::valid({64, 16, 64, 64}));
    EXPECT_EQ(VmwareSvgaState::freeBytes({16, 64, 18, 16}), 0U);
    EXPECT_FALSE(VmwareSvgaState::canFit({16, 64, 16, 16}, 0));
}

TEST(VmwareSvgaState, RejectsFifoOutsideMappedAperture)
{
    EXPECT_TRUE(VmwareSvgaState::valid({64, 4096, 64, 64}, 64, 4096));
    EXPECT_FALSE(VmwareSvgaState::valid({60, 4096, 60, 60}, 64, 4096));
    EXPECT_FALSE(VmwareSvgaState::valid({64, 4100, 64, 64}, 64, 4096));
}

TEST(VmwareSvgaState, PollBudgetStopsAtDeadline)
{
    PollBudget budget(100, 50, 100);
    EXPECT_TRUE(budget.keepPolling(149));
    EXPECT_FALSE(budget.keepPolling(150));
    EXPECT_EQ(budget.polls(), 2U);
}

TEST(VmwareSvgaState, PollBudgetStopsWhenClockDoesNotAdvance)
{
    PollBudget budget(0, 1000, 3);
    EXPECT_TRUE(budget.keepPolling(0));
    EXPECT_TRUE(budget.keepPolling(0));
    EXPECT_FALSE(budget.keepPolling(0));
    EXPECT_EQ(budget.polls(), 3U);
}

TEST(VmwareSvgaState, ZeroPollLimitStopsWithoutConsumingBudget)
{
    PollBudget budget(0, 1000, 0);
    EXPECT_FALSE(budget.keepPolling(0));
    EXPECT_EQ(budget.polls(), 0U);
}

TEST(VmwareSvgaState, PollBudgetHandlesTimestampWrap)
{
    const uint64_t started = ~static_cast<uint64_t>(0) - 5;
    PollBudget budget(started, 10, 100);
    EXPECT_TRUE(budget.keepPolling(3));
    EXPECT_FALSE(budget.keepPolling(4));
}
