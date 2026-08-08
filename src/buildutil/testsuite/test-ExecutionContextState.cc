/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include <gtest/gtest.h>

#include "pedigree/kernel/process/ExecutionContext.h"

TEST(ExecutionContextState, RestoresNestedScopesInLifoOrder)
{
    ExecutionContextState state;
    EXPECT_EQ(ExecutionContext::WaitableThread, state.current());

    const ExecutionContext thread =
        state.enter(ExecutionContext::HostedSyntheticIrq);
    const ExecutionContext signal =
        state.enter(ExecutionContext::HardDeviceIrq);
    const ExecutionContext hard = state.enter(ExecutionContext::SchedulerIrq);

    EXPECT_EQ(ExecutionContext::SchedulerIrq, state.current());
    state.restore(hard);
    EXPECT_EQ(ExecutionContext::HardDeviceIrq, state.current());
    state.restore(signal);
    EXPECT_EQ(ExecutionContext::HostedSyntheticIrq, state.current());
    state.restore(thread);
    EXPECT_EQ(ExecutionContext::WaitableThread, state.current());
}

TEST(ExecutionContextState, ThreadHandoffCannotLeakAnIrqClassification)
{
    ExecutionContextState interrupted;
    ExecutionContextState scheduled;

    const ExecutionContext previous =
        interrupted.enter(ExecutionContext::HardDeviceIrq);

    // Models a timer callback switching away from an interrupted Thread. The
    // destination owns a different state object, so it cannot inherit the
    // source Thread's hard-IRQ classification.
    EXPECT_EQ(ExecutionContext::HardDeviceIrq, interrupted.current());
    EXPECT_EQ(ExecutionContext::WaitableThread, scheduled.current());

    interrupted.restore(previous);
    EXPECT_EQ(ExecutionContext::WaitableThread, interrupted.current());
    EXPECT_EQ(ExecutionContext::WaitableThread, scheduled.current());
}

TEST(ExecutionContextState, ResetMakesReusedStateWaitable)
{
    ExecutionContextState state;
    state.enter(ExecutionContext::DebuggerTrap);
    state.reset();
    EXPECT_EQ(ExecutionContext::WaitableThread, state.current());
}
