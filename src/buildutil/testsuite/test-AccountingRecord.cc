/* Copyright (c) 2026, Pedigree Developers. */
#include <cstring>
#include <limits>

#include "modules/subsys/posix/accounting-record.h"
#include <gtest/gtest.h>

using PosixAccounting::compressed;
using PosixAccounting::floatBits;

TEST(AccountingRecord, CompressedCounterBoundariesAndSaturation) {
  EXPECT_EQ(compressed(0), 0);
  EXPECT_EQ(compressed(8191), 8191);
  EXPECT_EQ(compressed(8192), 0x2400);
  EXPECT_EQ(compressed(8196), 0x2401);
  EXPECT_EQ(compressed(65535), 0x4400);
  EXPECT_EQ(compressed(uint64_t(8191) << 21), 0xffff);
  EXPECT_EQ(compressed(std::numeric_limits<uint64_t>::max()), 0xffff);
}

TEST(AccountingRecord, ElapsedTicksHaveLinuxBinary32Representation) {
  EXPECT_EQ(floatBits(0), 0U);
  EXPECT_EQ(floatBits(1), 0x3f800000U);
  EXPECT_EQ(floatBits(100), 0x42c80000U);
  EXPECT_EQ(floatBits(1ULL << 63), 0x5f000000U);
  EXPECT_EQ(floatBits(std::numeric_limits<uint64_t>::max()), 0x5f7fffffU);
  for (uint64_t ticks = 1; ticks <= 1000000; ticks += 997) {
    const uint32_t bits = floatBits(ticks);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    EXPECT_EQ(value, static_cast<float>(ticks));
  }
}

TEST(AccountingRecord, ReservedFieldsAreZeroAndRecordIsSixtyFourBytes) {
  PosixAccounting::Record record;
  const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&record);
  ASSERT_EQ(sizeof(record), 64U);
  for (size_t i = 0; i < sizeof(record); ++i)
    EXPECT_EQ(bytes[i], i == 1 ? 3 : 0);
}
