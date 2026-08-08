/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include <gtest/gtest.h>

#include "system/kernel/machine/mach_pc/LocalApicLint0Policy.h"

TEST(LocalApicLint0Policy, BootstrapProcessorOwnsVirtualWire)
{
    constexpr uint32_t Preserved = 0xA5000000;
    constexpr uint32_t Hostile =
        Preserved | LocalApicLint0Policy::Masked | 0x8000 | 0x2000 | 0x45;
    const uint32_t configured = LocalApicLint0Policy::configuredValue(
        Hostile, LocalApicLint0Policy::BootstrapProcessor);

    EXPECT_EQ(
        configured & LocalApicLint0Policy::ProgrammableFields,
        LocalApicLint0Policy::BootstrapValue);
    EXPECT_EQ(
        configured & ~LocalApicLint0Policy::ProgrammableFields, Preserved);
    EXPECT_TRUE(LocalApicLint0Policy::routesLegacyPic(configured));
    EXPECT_TRUE(LocalApicLint0Policy::matchesRole(
        configured, LocalApicLint0Policy::BootstrapProcessor));
}

TEST(LocalApicLint0Policy, ApplicationProcessorCannotOwnVirtualWire)
{
    constexpr uint32_t Preserved = 0x5A000000;
    const uint32_t configured = LocalApicLint0Policy::configuredValue(
        Preserved | LocalApicLint0Policy::ExtInt, 0);

    EXPECT_EQ(
        configured & LocalApicLint0Policy::ProgrammableFields,
        LocalApicLint0Policy::ApplicationProcessorValue);
    EXPECT_EQ(
        configured & ~LocalApicLint0Policy::ProgrammableFields, Preserved);
    EXPECT_FALSE(LocalApicLint0Policy::routesLegacyPic(configured));
    EXPECT_TRUE(LocalApicLint0Policy::matchesRole(configured, 0));
}

TEST(LocalApicLint0Policy, ExactlyOneProcessorRoutesLegacyPic)
{
    size_t routes = 0;
    for (size_t processor = 0; processor < 4; ++processor)
    {
        const uint64_t apicBase = processor == 0 ?
                                      LocalApicLint0Policy::BootstrapProcessor :
                                      0;
        const uint32_t configured =
            LocalApicLint0Policy::configuredValue(0, apicBase);
        routes += LocalApicLint0Policy::routesLegacyPic(configured) ? 1 : 0;
    }
    EXPECT_EQ(routes, static_cast<size_t>(1));
}

TEST(LocalApicLint0Policy, ReadbackMustMatchTheProcessorRole)
{
    EXPECT_FALSE(LocalApicLint0Policy::matchesRole(
        LocalApicLint0Policy::BootstrapValue |
            LocalApicLint0Policy::Masked,
        LocalApicLint0Policy::BootstrapProcessor));
    EXPECT_FALSE(LocalApicLint0Policy::matchesRole(
        LocalApicLint0Policy::ExtInt, 0));
}
