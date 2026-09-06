/* Copyright (c) 2026, Pedigree Developers. */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/process/ExecutionPersonality.h"
#include "pedigree/kernel/process/LoadAverage.h"

#include "modules/subsys/posix/system-information-abi.h"
#include <gtest/gtest.h>

TEST(SystemInformation, MemoryUnitsAreCheckedBeforeChangingTheRecord) {
  PosixSystemInformation::Record record;
  ASSERT_TRUE(PosixSystemInformation::setMemory(record, 200, 50, 80, 3, 4096));
  EXPECT_EQ(record.totalram, 819200U);
  EXPECT_EQ(record.freeram, 204800U);
  EXPECT_EQ(record.totalswap, 327680U);
  EXPECT_EQ(record.freeswap, 315392U);
  EXPECT_EQ(record.mem_unit, 1U);
  EXPECT_EQ(record.alignment | record.pad | record.tail, 0U);
  EXPECT_FALSE(PosixSystemInformation::setMemory(record, ~uint64_t(0), 0, 0, 0, 4096));
  EXPECT_FALSE(PosixSystemInformation::setMemory(record, 10, 11, 0, 0, 4096));
  EXPECT_FALSE(PosixSystemInformation::setMemory(record, 10, 1, 3, 4, 4096));
  EXPECT_EQ(record.totalram, 819200U);
}

TEST(SystemInformation, LoadAveragesUseElapsedTimeAndDistinctDecayWindows) {
  LoadAverage average;
  uint64_t loads[3];
  average.update(LoadAverage::PeriodNanoseconds - 1, 1);
  average.snapshot(loads);
  EXPECT_EQ(loads[0] | loads[1] | loads[2], 0U);
  for (uint64_t second = 5; second <= 60; second += 5)
    average.update(second * 1000000000ULL, 1);
  average.snapshot(loads);
  EXPECT_GT(loads[0], LoadAverage::Scale * 60 / 100);
  EXPECT_LT(loads[0], LoadAverage::Scale * 65 / 100);
  EXPECT_GT(loads[1], LoadAverage::Scale * 16 / 100);
  EXPECT_LT(loads[1], LoadAverage::Scale * 20 / 100);
  EXPECT_GT(loads[2], LoadAverage::Scale * 5 / 100);
  EXPECT_LT(loads[2], LoadAverage::Scale * 8 / 100);
  const uint64_t before = loads[0];
  average.update(59 * 1000000000ULL, 500);
  average.snapshot(loads);
  EXPECT_EQ(loads[0], before);
  average.update(24 * 3600 * 1000000000ULL, 0);
  average.snapshot(loads);
  EXPECT_EQ(loads[0] | loads[1] | loads[2], 0U);
}

TEST(SystemInformation, PersonalityInheritanceIsIndependentAndUnsupportedChangesAreAtomic) {
  ExecutionPersonality parent, child;
  uint32_t previous = 99;
  ASSERT_TRUE(parent.select(ExecutionPersonality::Linux32, previous));
  EXPECT_EQ(previous, ExecutionPersonality::Native);
  child.inherit(parent);
  EXPECT_EQ(child.value(), ExecutionPersonality::Linux32);
  ASSERT_TRUE(child.select(ExecutionPersonality::Native, previous));
  EXPECT_EQ(previous, ExecutionPersonality::Linux32);
  EXPECT_EQ(parent.value(), ExecutionPersonality::Linux32);
  EXPECT_FALSE(parent.select(ExecutionPersonality::Linux32 | 0x40000, previous));
  EXPECT_FALSE(parent.select(6, previous));
  EXPECT_EQ(parent.value(), ExecutionPersonality::Linux32);
  ASSERT_TRUE(parent.select(ExecutionPersonality::Query, previous));
  EXPECT_EQ(previous, ExecutionPersonality::Linux32);
}
