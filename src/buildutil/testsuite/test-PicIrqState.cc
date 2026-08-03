/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include <gtest/gtest.h>

#include "system/kernel/machine/mach_pc/PicIrqState.h"

TEST(PicIrqState, SchedulerRouteExcludesGenericRegistration)
{
    PicIrqState state;
    const IrqPolicy scheduler = IrqPolicy::edgeHard();

    ASSERT_TRUE(state.canRegisterScheduler(0, scheduler));
    state.schedulerRegistered(0, scheduler);

    EXPECT_TRUE(state.schedulerRegistered(0));
    EXPECT_FALSE(state.canRegister(0, scheduler, IrqDelivery::Hard));
    EXPECT_FALSE(
        state.canRegister(0, IrqPolicy::edgeThreaded(), IrqDelivery::Threaded));
    EXPECT_FALSE(
        state.canRegister(0, IrqPolicy::levelHard(), IrqDelivery::Hard));
    EXPECT_FALSE(state.canRegister(
        0, IrqPolicy::levelThreaded(), IrqDelivery::Threaded));

    state.schedulerUnregistered(0);
    EXPECT_FALSE(state.schedulerRegistered(0));
    EXPECT_TRUE(state.canRegister(0, scheduler, IrqDelivery::Hard));
}

TEST(PicIrqState, GenericRegistrationExcludesSchedulerRoute)
{
    PicIrqState state;
    const IrqPolicy generic = IrqPolicy::edgeHard();

    EXPECT_FALSE(state.canRegisterScheduler(1, IrqPolicy::edgeHard()));
    EXPECT_FALSE(state.canRegisterScheduler(0, IrqPolicy::levelHard()));

    ASSERT_TRUE(state.canRegister(0, generic, IrqDelivery::Hard));
    state.handlerRegistered(0, generic, IrqDelivery::Hard);

    EXPECT_FALSE(state.canRegisterScheduler(0, IrqPolicy::edgeHard()));

    state.handlerUnregistered(0, IrqDelivery::Hard);
    EXPECT_TRUE(state.canRegisterScheduler(0, IrqPolicy::edgeHard()));
}

TEST(PicIrqState, SchedulerOccurrenceIsTerminallyAccounted)
{
    PicIrqState state;
    state.schedulerRegistered(0, IrqPolicy::edgeHard());

    const size_t generation = state.beginDispatch(0);
    ASSERT_NE(generation, static_cast<size_t>(0));
    EXPECT_TRUE(state.acknowledge(0));
    state.completeDispatch(0, generation, false);

    EXPECT_EQ(state.dispatchGeneration(0), generation);
    EXPECT_EQ(state.acknowledgedGeneration(0), generation);
    EXPECT_FALSE(state.acknowledgementPending(0));
    EXPECT_TRUE(state.enabled(0));

    state.schedulerUnregistered(0);
    EXPECT_EQ(state.handlerCount(0), static_cast<size_t>(0));
    EXPECT_FALSE(state.enabled(0));
}
