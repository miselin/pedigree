#define PEDIGREE_EXTERNAL_SOURCE 1

#include <array>
#include <vector>

#include "system/kernel/machine/mach_pc/RtcCalendar.h"
#include <gtest/gtest.h>

namespace {
struct Cmos {
  std::array<uint8_t, 128> bytes{};
  std::vector<uint8_t> reads, writes;

  Cmos() {
    bytes[0x0d] = 0x80;
    bytes[0x0b] = 2;
    bytes[4] = 0x23;
    bytes[7] = 0x08;
    bytes[8] = 0x09;
    bytes[9] = 0x26;
  }
  bool read(uint8_t century, RtcTimeAccounting::CivilTime& time) {
    return RtcCalendar::read(
        [this](uint8_t index) {
          reads.push_back(index);
          return bytes[index];
        },
        century, time);
  }
  bool write(uint8_t century, const RtcTimeAccounting::CivilTime& time) {
    return RtcCalendar::write([this](uint8_t index) { return bytes[index]; },
                              [this](uint8_t index, uint8_t value) {
                                writes.push_back(index);
                                bytes[index] = value;
                              },
                              century, time);
  }
};
}  // namespace

TEST(RtcCalendar, AbsentCenturyNeverTouchesFirmwareCmos) {
  Cmos cmos;
  cmos.bytes[0x32] = 0xa5;
  RtcTimeAccounting::CivilTime time{};
  ASSERT_TRUE(cmos.read(0, time));
  EXPECT_EQ(time.year, 2026U);
  ASSERT_TRUE(cmos.write(0, time));
  EXPECT_EQ(cmos.bytes[0x32], 0xa5);
  for (auto index : cmos.reads)
    EXPECT_LE(index, 0x0d);
  for (auto index : cmos.writes)
    EXPECT_LE(index, 9);
}

TEST(RtcCalendar, UsesOnlyDeclaredCenturyAndPreservesClockFormat) {
  for (uint8_t status : {0U, 2U, 4U, 6U}) {
    for (uint8_t hour : {0U, 12U, 23U}) {
      Cmos cmos;
      cmos.bytes[0x0b] = status;
      cmos.bytes[0x32] = 0xa5;
      RtcTimeAccounting::CivilTime original{2100, 3, 1, hour, 59, 58, 0}, result{};
      ASSERT_TRUE(cmos.write(0x37, original));
      ASSERT_TRUE(cmos.read(0x37, result));
      EXPECT_EQ(result.year, original.year);
      EXPECT_EQ(result.hour, original.hour);
      EXPECT_EQ(result.minute, original.minute);
      EXPECT_EQ(result.second, original.second);
      EXPECT_EQ(cmos.bytes[0x0b], status);
      EXPECT_EQ(cmos.bytes[0x32], 0xa5);
    }
  }
}

TEST(RtcCalendar, MissingCenturyHasFixedReversibleRange) {
  Cmos cmos;
  RtcTimeAccounting::CivilTime time{};
  cmos.bytes[9] = 0x70;
  ASSERT_TRUE(cmos.read(0, time));
  EXPECT_EQ(time.year, 1970U);
  cmos.bytes[9] = 0x69;
  ASSERT_TRUE(cmos.read(0, time));
  EXPECT_EQ(time.year, 2069U);
  time.year = 2070;
  EXPECT_FALSE(cmos.write(0, time));
  EXPECT_TRUE(cmos.writes.empty());
}

TEST(RtcCalendar, InvalidStateDoesNotPublishOrWritePartialCalendar) {
  for (uint8_t century : {1U, 0x0dU, 0x80U, 0xb2U, 0xffU}) {
    Cmos cmos;
    RtcTimeAccounting::CivilTime time{2026, 1, 1, 0, 0, 0, 0};
    EXPECT_FALSE(cmos.read(century, time));
    EXPECT_FALSE(cmos.write(century, time));
    EXPECT_TRUE(cmos.reads.empty());
    EXPECT_TRUE(cmos.writes.empty());
  }
  for (auto invalid : {std::pair<uint8_t, uint8_t>{0x0d, 0}, {9, 0xfa}, {7, 0x31}, {4, 0x24}}) {
    Cmos cmos;
    cmos.bytes[invalid.first] = invalid.second;
    RtcTimeAccounting::CivilTime time{2001, 1, 1, 0, 0, 0, 0};
    EXPECT_FALSE(cmos.read(0, time));
    EXPECT_EQ(time.year, 2001U);
  }
  Cmos cmos;
  EXPECT_FALSE(cmos.write(0x32, {2100, 2, 29, 0, 0, 0, 0}));
  EXPECT_TRUE(cmos.writes.empty());
}
