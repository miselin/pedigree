/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include <gtest/gtest.h>

#include "system/kernel/machine/mach_pc/LocalApicIcrTransaction.h"

TEST(LocalApicIcrTransaction, MasksTheOldOwnerPreemptionWindow)
{
    // Old design: normal code owned a shared ICR word, then a same-core hard
    // IRQ could preempt it and spin with IF clear on that owner. The new local
    // boundary makes that preemption impossible before the ICR is submitted.
    const LocalApicIcrTransaction normalSubmission(true);
    EXPECT_TRUE(normalSubmission.masksMaskableInterrupts());
    EXPECT_FALSE(normalSubmission.allowsMaskableHardIrqPreemption());
    EXPECT_TRUE(normalSubmission.restoreInterrupts());

    // A hard IRQ starts with IF already clear, but has no inherited software
    // owner to acquire and must preserve that state on return.
    const LocalApicIcrTransaction hardSubmission(false);
    EXPECT_TRUE(hardSubmission.masksMaskableInterrupts());
    EXPECT_FALSE(hardSubmission.allowsMaskableHardIrqPreemption());
    EXPECT_FALSE(hardSubmission.restoreInterrupts());
}
