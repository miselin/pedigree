/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include <gtest/gtest.h>

#include "system/kernel/machine/mach_pc/PcSchedulerTimerSelection.h"

TEST(PcSchedulerTimerSelection, DefaultsToPitFallback)
{
    PcSchedulerTimerSelection selection;

    EXPECT_EQ(selection.source(), PcSchedulerTimerSelection::Source::Pit);
    EXPECT_TRUE(selection.usesPit());
    EXPECT_FALSE(selection.usesLocalApic());
}

TEST(PcSchedulerTimerSelection, FailedLocalApicKeepsPitFallback)
{
    PcSchedulerTimerSelection selection;

    selection.recordLocalApicInitialisation(false);

    EXPECT_TRUE(selection.usesPit());
    EXPECT_FALSE(selection.usesLocalApic());
}

TEST(PcSchedulerTimerSelection, SuccessfulLocalApicSuppressesPit)
{
    PcSchedulerTimerSelection selection;

    selection.recordLocalApicInitialisation(true);

    EXPECT_FALSE(selection.usesPit());
    EXPECT_TRUE(selection.usesLocalApic());
}

TEST(PcSchedulerTimerSelection, LatestInitialisationOutcomeIsAuthoritative)
{
    PcSchedulerTimerSelection selection;

    selection.recordLocalApicInitialisation(true);
    selection.recordLocalApicInitialisation(false);

    EXPECT_TRUE(selection.usesPit());
    EXPECT_FALSE(selection.usesLocalApic());
}
