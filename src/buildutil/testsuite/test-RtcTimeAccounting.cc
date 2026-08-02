/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include <gtest/gtest.h>

#include "system/kernel/machine/mach_pc/RtcTimeAccounting.h"

TEST(RtcTimeAccounting, ConsumesOnlyForwardElapsedTime)
{
    uint64_t cursor = 100;
    EXPECT_EQ(RtcTimeAccounting::consumeElapsed(350, cursor), 250U);
    EXPECT_EQ(cursor, 350U);
    EXPECT_EQ(RtcTimeAccounting::consumeElapsed(349, cursor), 0U);
    EXPECT_EQ(RtcTimeAccounting::consumeElapsed(350, cursor), 0U);
    EXPECT_EQ(cursor, 350U);
}

TEST(RtcTimeAccounting, CarriesFractionalSecondsAcrossLeapDay)
{
    RtcTimeAccounting::CivilTime time = {2024, 2, 28, 23, 59, 59, 900000000ULL};

    EXPECT_EQ(RtcTimeAccounting::advanceCivilTime(time, 200000000ULL), 1U);
    EXPECT_EQ(time.year, 2024U);
    EXPECT_EQ(time.month, 2U);
    EXPECT_EQ(time.day, 29U);
    EXPECT_EQ(time.hour, 0U);
    EXPECT_EQ(time.minute, 0U);
    EXPECT_EQ(time.second, 0U);
    EXPECT_EQ(time.nanosecond, 100000000U);
}

TEST(RtcTimeAccounting, CarriesAggregatedTimeAcrossYear)
{
    RtcTimeAccounting::CivilTime time = {
        2023, 12, 30, 22, 58, 59, 750000000ULL};
    constexpr uint64_t Delta = (2ULL * 24 * 60 * 60 + 61ULL * 60 + 2) *
                                   RtcTimeAccounting::NanosecondsPerSecond +
                               500000000ULL;

    EXPECT_EQ(
        RtcTimeAccounting::advanceCivilTime(time, Delta),
        2ULL * 24 * 60 * 60 + 61ULL * 60 + 3);
    EXPECT_EQ(time.year, 2024U);
    EXPECT_EQ(time.month, 1U);
    EXPECT_EQ(time.day, 2U);
    EXPECT_EQ(time.hour, 0U);
    EXPECT_EQ(time.minute, 0U);
    EXPECT_EQ(time.second, 2U);
    EXPECT_EQ(time.nanosecond, 250000000U);
}

TEST(RtcTimeAccounting, CarriesOneAggregateAcrossMultipleYears)
{
    RtcTimeAccounting::CivilTime time = {1999, 3, 1, 12, 34, 56, 123456789ULL};
    constexpr uint64_t Delta =
        731ULL * 24 * 60 * 60 * RtcTimeAccounting::NanosecondsPerSecond;

    EXPECT_EQ(
        RtcTimeAccounting::advanceCivilTime(time, Delta),
        731ULL * 24 * 60 * 60);
    EXPECT_EQ(time.year, 2001U);
    EXPECT_EQ(time.month, 3U);
    EXPECT_EQ(time.day, 1U);
    EXPECT_EQ(time.hour, 12U);
    EXPECT_EQ(time.minute, 34U);
    EXPECT_EQ(time.second, 56U);
    EXPECT_EQ(time.nanosecond, 123456789U);
}

TEST(RtcTimeAccounting, CenturyLeapRuleIsExact)
{
    RtcTimeAccounting::CivilTime nonLeap = {2100, 2, 28, 23, 59, 59, 0};
    RtcTimeAccounting::CivilTime leap = {2000, 2, 28, 23, 59, 59, 0};

    RtcTimeAccounting::advanceCivilTime(
        nonLeap, RtcTimeAccounting::NanosecondsPerSecond);
    RtcTimeAccounting::advanceCivilTime(
        leap, RtcTimeAccounting::NanosecondsPerSecond);

    EXPECT_EQ(nonLeap.month, 3U);
    EXPECT_EQ(nonLeap.day, 1U);
    EXPECT_EQ(leap.month, 2U);
    EXPECT_EQ(leap.day, 29U);
}
